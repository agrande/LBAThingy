#include "LBAGridPlanActor.h"

#include "Engine/Engine.h"
#include "SceneView.h"
#include "ShowFlags.h"

#define LOCTEXT_NAMESPACE "LBAGridPlan"

namespace
{
	TCustomShowFlag<> ShowGridPlan(TEXT("LBAGridPlan"), false, SFG_Developer, LOCTEXT("ShowGridPlan", "LBA2 Grid Plan"));

	/** Wall height drawn for wall columns, cells. */
	constexpr double WallMark = 1.0;
	/** Thickness of slabs and tiles, cells. */
	constexpr double Slab = 0.08;

	FBox CellBox(double X0, double Y0, double Z0, double X1, double Y1, double Z1)
	{
		return FBox(FLBAGridPlan::CellToWorld(X0, Y0, Z0), FLBAGridPlan::CellToWorld(X1, Y1, Z1));
	}

	class FGridPlanSceneProxy final : public FDebugRenderSceneProxy
	{
	public:
		FGridPlanSceneProxy(const ULBAGridPlanComponent* Component)
			: FDebugRenderSceneProxy(Component)
		{
			ViewFlagName = TEXT("LBAGridPlan");
			ViewFlagIndex = uint32(FEngineShowFlags::FindIndexByName(*ViewFlagName));
			DrawType = SolidAndWireMeshes;
			// Solid boxes take their colors' alpha as is.
			DrawAlpha = 1;
			TextWithoutShadowDistance = -1.f;
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			const bool bVisible = IsShown(View) && View->Family->EngineShowFlags.GetSingleFlag(ViewFlagIndex);
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = bVisible;
			Result.bDynamicRelevance = true;
			Result.bSeparateTranslucency = Result.bNormalTranslucency = bVisible;
			return Result;
		}
	};
}

ULBAGridPlanComponent::ULBAGridPlanComponent()
{
	SetCastShadow(false);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	bIsEditorOnly = true;
	bHiddenInGame = true;
	bSelectable = true;
}

FColor ULBAGridPlanComponent::GetRoomColor(int32 Room)
{
	// Golden-angle hues: neighbors by index look apart.
	return FLinearColor::MakeFromHSV8(uint8((Room * 97 + 20) % 256), 170, 235).ToFColor(true);
}

FColor ULBAGridPlanComponent::GetConnectorColor(ELBAConnector Kind)
{
	switch (Kind)
	{
	case ELBAConnector::Stairs: return FColor(240, 220, 60);
	case ELBAConnector::Passage: return FColor(60, 210, 235);
	case ELBAConnector::Exit: return FColor(235, 60, 210);
	case ELBAConnector::Alcove: return FColor(100, 120, 245);
	default: return FColor(110, 110, 110);
	}
}

FColor ULBAGridPlanComponent::GetSideColor(ELBARoomSide Kind, const FLBAGridPlan& Plan, int32 Other)
{
	switch (Kind)
	{
	case ELBARoomSide::Wall: return FColor(230, 50, 40);
	case ELBARoomSide::OpenSide: return FColor(60, 210, 90);
	case ELBARoomSide::Connector: return Plan.Connectors.IsValidIndex(Other) ? GetConnectorColor(Plan.Connectors[Other].Kind) : FColor::White;
	case ELBARoomSide::Step: return FColor(245, 150, 30);
	default: return FColor(150, 150, 150);
	}
}

FColor ULBAGridPlanComponent::GetRuleColor(ELBAWallRule Rule)
{
	switch (Rule)
	{
	case ELBAWallRule::Cut: return FColor(240, 130, 40);
	case ELBAWallRule::Jamb: return FColor(240, 220, 60);
	case ELBAWallRule::Invisible: return FColor(170, 210, 250);
	case ELBAWallRule::Tall: return FColor(170, 90, 220);
	case ELBAWallRule::Masonry: return FColor(150, 100, 60);
	case ELBAWallRule::Exterior: return FColor(120, 120, 130);
	case ELBAWallRule::Pillar:
	case ELBAWallRule::Post:
	case ELBAWallRule::Against: return FColor(190, 90, 230);
	default: return FColor(200, 200, 200);
	}
}

void ULBAGridPlanComponent::Select(int32 InRoom, int32 InConnector)
{
	SelectedRoom = InRoom;
	SelectedConnector = InConnector;
	RefreshSelection();
	MarkRenderStateDirty();
}

void ULBAGridPlanComponent::RefreshSelection()
{
	const FLBAGridPlan* Data = Plan ? &Plan->Plan : nullptr;
	Room = Data && Data->Rooms.IsValidIndex(SelectedRoom) ? Data->Rooms[SelectedRoom] : FLBAGridPlanRoom();
	Connector = Data && Data->Connectors.IsValidIndex(SelectedConnector) ? Data->Connectors[SelectedConnector] : FLBAGridPlanConnector();
}

#if WITH_EDITOR
void ULBAGridPlanComponent::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	RefreshSelection();
	MarkRenderStateDirty();
}
#endif

void ULBAGridPlanComponent::OnRegister()
{
	Super::OnRegister();
	RefreshSelection();
}

FBoxSphereBounds ULBAGridPlanComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return FBoxSphereBounds(CellBox(0, 0, 0, FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeY, FLBAGridPlan::SizeXZ));
}

FDebugRenderSceneProxy* ULBAGridPlanComponent::CreateDebugSceneProxy()
{
	FGridPlanSceneProxy* Proxy = new FGridPlanSceneProxy(this);
	if (!Plan)
	{
		return Proxy;
	}
	const FLBAGridPlan& Data = Plan->Plan;
	auto IsShownLevel = [this](int32 Level) { return OnlyLevel == INDEX_NONE || OnlyLevel == Level; };
	auto Solid = [Proxy](const FBox& Box, FColor Color, uint8 Alpha)
	{
		Proxy->Boxes.Emplace(Box, Color.WithAlpha(Alpha), FDebugRenderSceneProxy::SolidMesh);
	};
	auto Wire = [Proxy](const FBox& Box, FColor Color, float Thickness)
	{
		Proxy->Boxes.Emplace(Box, Color, FDebugRenderSceneProxy::WireMesh, Thickness);
	};

	if (Data.Structure == ELBAEnclosureStructure::Hollow)
	{
		// A cave: its floors under the roof, the roof as tiles, the supports as pillars.
		for (const FLBAGridPlanHollowColumn& Column : Data.Hollow.Columns)
		{
			const double X = Column.Column.X, Z = Column.Column.Y;
			if (Column.bSupport)
			{
				Solid(CellBox(X + 0.3, Column.Spring, Z + 0.3, X + 0.7, Column.Roof, Z + 0.7), FColor(230, 50, 40), 160);
				continue;
			}
			if (bShowRooms && Column.Floor != INDEX_NONE)
			{
				Solid(CellBox(X, Column.Floor + 1, Z, X + 1, Column.Floor + 1 + Slab, Z + 1), GetRoomColor(Column.Floor), 110);
			}
			if (bShowAir)
			{
				Solid(CellBox(X, Column.Roof - Slab, Z, X + 1, Column.Roof, Z + 1), FColor(60, 200, 80), 70);
			}
		}
		if (bShowLabels)
		{
			Proxy->Texts.Emplace(FString::Printf(TEXT("Grid %d %s: hollow"), Data.Grid, *Data.Name), FLBAGridPlan::CellToWorld(32, 20, 32), FLinearColor::White);
		}
		return Proxy;
	}

	for (int32 RoomIndex = 0; RoomIndex < Data.Rooms.Num(); ++RoomIndex)
	{
		const FLBAGridPlanRoom& R = Data.Rooms[RoomIndex];
		if (!IsShownLevel(R.Level))
		{
			continue;
		}
		const bool bSelected = RoomIndex == SelectedRoom;
		const FColor Color = GetRoomColor(RoomIndex);
		const double Floor = R.Level + 1;
		if (bShowRooms)
		{
			Solid(CellBox(R.Min.X, Floor, R.Min.Y, R.Max.X + 1, Floor + Slab, R.Max.Y + 1), Color, bSelected ? 200 : 110);
			Wire(CellBox(R.Min.X, Floor, R.Min.Y, R.Max.X + 1, R.Ceiling, R.Max.Y + 1), bSelected ? FColor::White : Color, bSelected ? 3.f : 1.f);
		}
		if (bShowSides)
		{
			// Along each side, at its outer edge, a band per run of what lies past it.
			for (int32 Side = 0; Side < 4; ++Side)
			{
				const bool bAlongZ = Side < 2;
				const double Edge = Side == 0 ? R.Min.X : Side == 1 ? R.Max.X + 1 : Side == 2 ? R.Min.Y : R.Max.Y + 1;
				double From = bAlongZ ? R.Min.Y : R.Min.X;
				for (const FLBAGridPlanSideRun& Run : R.GetSide(Side).Runs)
				{
					const double To = From + Run.Count;
					const FVector A = bAlongZ ? FLBAGridPlan::CellToWorld(Edge, Floor + 0.25, From + 0.1) : FLBAGridPlan::CellToWorld(From + 0.1, Floor + 0.25, Edge);
					const FVector B = bAlongZ ? FLBAGridPlan::CellToWorld(Edge, Floor + 0.25, To - 0.1) : FLBAGridPlan::CellToWorld(To - 0.1, Floor + 0.25, Edge);
					Proxy->Lines.Emplace(A, B, GetSideColor(Run.Kind, Data, Run.Other), bSelected ? 5.f : 3.f);
					From = To;
				}
			}
		}
		if (bShowLabels)
		{
			const FIntPoint Size = R.Max - R.Min + FIntPoint(1, 1);
			Proxy->Texts.Emplace(FString::Printf(TEXT("R%d (S%d) level %d, ceiling %d, %dx%d"), RoomIndex, R.Space, R.Level, R.Ceiling, Size.X, Size.Y),
				FLBAGridPlan::CellToWorld((R.Min.X + R.Max.X + 1) * 0.5, Floor + 1.0, (R.Min.Y + R.Max.Y + 1) * 0.5), bSelected ? FLinearColor::White : FLinearColor(Color));
		}
	}

	if (bShowConnectors)
	{
		for (int32 Index = 0; Index < Data.Connectors.Num(); ++Index)
		{
			const FLBAGridPlanConnector& C = Data.Connectors[Index];
			const bool bSelected = Index == SelectedConnector;
			const FColor Color = GetConnectorColor(C.Kind);
			FVector Sum = FVector::ZeroVector;
			int32 Shown = 0;
			for (const FIntVector& Piece : C.Columns)
			{
				// A flight's column: on its highest step.
				int32 Floor = Piece.Y;
				for (const FLBAGridPlanFlight& Flight : Data.Flights)
				{
					for (const FIntVector& Step : Flight.Cells)
					{
						Floor = Piece.Y == INDEX_NONE && Step.X == Piece.X && Step.Z == Piece.Z ? FMath::Max(Floor, Step.Y) : Floor;
					}
				}
				if (Piece.Y != INDEX_NONE ? !IsShownLevel(Piece.Y) : !C.Levels.ContainsByPredicate(IsShownLevel))
				{
					continue;
				}
				Solid(CellBox(Piece.X + 0.1, Floor + 1, Piece.Z + 0.1, Piece.X + 0.9, Floor + 1 + Slab * 2, Piece.Z + 0.9), Color, bSelected ? 230 : 150);
				Sum += FLBAGridPlan::CellToWorld(Piece.X + 0.5, Floor + 1.5, Piece.Z + 0.5);
				++Shown;
			}
			if (bShowLabels && Shown > 0 && C.Kind != ELBAConnector::Pocket)
			{
				Proxy->Texts.Emplace(FString::Printf(TEXT("C%d %s"), Index, *StaticEnum<ELBAConnector>()->GetDisplayNameTextByValue(int64(C.Kind)).ToString()),
					Sum / Shown, bSelected ? FLinearColor::White : FLinearColor(Color));
			}
		}
	}

	if (bShowWalls)
	{
		for (const FLBAGridPlanLevel& Level : Data.Levels)
		{
			if (!IsShownLevel(Level.Height))
			{
				continue;
			}
			const double Floor = Level.Height + 1;
			for (int32 Index = 0; Index < Level.Columns.Num(); ++Index)
			{
				const double X = Index % FLBAGridPlan::SizeXZ, Z = Index / FLBAGridPlan::SizeXZ;
				const ELBAWallRule Rule = Level.Rules.IsValidIndex(Index) ? Level.Rules[Index] : ELBAWallRule::None;
				if (Level.Columns[Index] == ELBAColumn::Wall)
				{
					Solid(CellBox(X + 0.15, Floor, Z + 0.15, X + 0.85, Floor + WallMark, Z + 0.85), GetRuleColor(Rule), 150);
				}
				else if (Level.Columns[Index] == ELBAColumn::Furniture && Rule != ELBAWallRule::None)
				{
					Solid(CellBox(X + 0.3, Floor, Z + 0.3, X + 0.7, Floor + WallMark * 0.5, Z + 0.7), GetRuleColor(Rule), 150);
				}
			}
			for (const FIntPoint& Gap : Level.Gaps)
			{
				Solid(CellBox(Gap.X + 0.2, Floor + Slab, Gap.Y + 0.2, Gap.X + 0.8, Floor + Slab * 2, Gap.Y + 0.8), FColor::White, 200);
			}
			for (const FIntPoint& Line : Level.PartitionLines)
			{
				Solid(CellBox(Line.X + 0.2, Floor + Slab, Line.Y + 0.2, Line.X + 0.8, Floor + Slab * 2, Line.Y + 0.8), FColor(170, 240, 170), 200);
			}
		}
	}

	if (bShowAir)
	{
		for (const FLBAGridPlanAir& At : Data.Air)
		{
			if (IsShownLevel(At.Level))
			{
				const FColor Color = At.Room != INDEX_NONE ? GetRoomColor(At.Room) : Data.Connectors.IsValidIndex(At.Connector) ? GetConnectorColor(Data.Connectors[At.Connector].Kind) : FColor::White;
				Solid(CellBox(At.Column.X, At.Top - Slab, At.Column.Y, At.Column.X + 1, At.Top, At.Column.Y + 1), Color, 60);
			}
		}
	}
	return Proxy;
}

ALBAGridPlanActor::ALBAGridPlanActor()
{
	PlanView = CreateDefaultSubobject<ULBAGridPlanComponent>(TEXT("PlanView"));
	RootComponent = PlanView;
	bIsEditorOnlyActor = true;
	SetCanBeDamaged(false);
}

#undef LOCTEXT_NAMESPACE
