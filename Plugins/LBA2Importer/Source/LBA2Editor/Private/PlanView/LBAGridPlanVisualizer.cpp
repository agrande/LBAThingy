#include "PlanView/LBAGridPlanVisualizer.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "LBAGridPlanActor.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveDrawingUtils.h"
#include "RenderingThread.h"

namespace
{
	/** A room or a connector of a plan, in the viewport. */
	struct HLBAGridPlanProxy : public HComponentVisProxy
	{
		DECLARE_HIT_PROXY();

		HLBAGridPlanProxy(const UActorComponent* InComponent, int32 InRoom, int32 InConnector)
			: HComponentVisProxy(InComponent, HPP_Wireframe)
			, Room(InRoom)
			, Connector(InConnector)
		{
		}

		int32 Room;
		int32 Connector;
	};
	IMPLEMENT_HIT_PROXY(HLBAGridPlanProxy, HComponentVisProxy);

	/** A box between two cell corners (X, Z horizontal, Y up), as DrawBox takes it. */
	void DrawCellBox(FPrimitiveDrawInterface* PDI, const FVector& Low, const FVector& High, const FMaterialRenderProxy* Material)
	{
		const FVector A = FLBAGridPlan::CellToWorld(Low.X, Low.Y, Low.Z), B = FLBAGridPlan::CellToWorld(High.X, High.Y, High.Z);
		const FBox Box(A.ComponentMin(B), A.ComponentMax(B));
		DrawBox(PDI, FTranslationMatrix(Box.GetCenter()), Box.GetExtent(), Material, SDPG_World);
	}
}

FLBAGridPlanVisualizer::FLBAGridPlanVisualizer()
{
	if (GEngine && GEngine->DebugMeshMaterial)
	{
		PickMaterial = MakeUnique<FColoredMaterialRenderProxy>(GEngine->DebugMeshMaterial->GetRenderProxy(), FLinearColor(1.f, 1.f, 1.f, 0.02f));
	}
}

FLBAGridPlanVisualizer::~FLBAGridPlanVisualizer()
{
	if (PickMaterial)
	{
		FlushRenderingCommands();
	}
}

void FLBAGridPlanVisualizer::DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	const ULBAGridPlanComponent* PlanView = Cast<ULBAGridPlanComponent>(Component);
	if (!PlanView || !PlanView->Plan || !PickMaterial)
	{
		return;
	}
	const FLBAGridPlan& Plan = PlanView->Plan->Plan;
	auto IsShownLevel = [PlanView](int32 Level) { return PlanView->OnlyLevel == INDEX_NONE || PlanView->OnlyLevel == Level; };
	for (int32 Index = 0; Index < Plan.Rooms.Num(); ++Index)
	{
		const FLBAGridPlanRoom& Room = Plan.Rooms[Index];
		if (IsShownLevel(Room.Level))
		{
			PDI->SetHitProxy(new HLBAGridPlanProxy(Component, Index, INDEX_NONE));
			DrawCellBox(PDI, FVector(Room.Min.X, Room.Level + 1, Room.Min.Y), FVector(Room.Max.X + 1, Room.Level + 1.2, Room.Max.Y + 1), PickMaterial.Get());
			PDI->SetHitProxy(nullptr);
		}
	}
	for (int32 Index = 0; Index < Plan.Connectors.Num(); ++Index)
	{
		const FLBAGridPlanConnector& Connector = Plan.Connectors[Index];
		PDI->SetHitProxy(new HLBAGridPlanProxy(Component, INDEX_NONE, Index));
		for (const FIntVector& Piece : Connector.Columns)
		{
			// A flight's column: from the lower of its levels, as high as its upper one.
			const bool bFlight = Piece.Y == INDEX_NONE;
			const int32 Low = bFlight && Connector.Levels.Num() > 0 ? FMath::Min(Connector.Levels) : Piece.Y;
			const int32 High = bFlight && Connector.Levels.Num() > 0 ? FMath::Max(Connector.Levels) : Piece.Y;
			if (bFlight ? Connector.Levels.ContainsByPredicate(IsShownLevel) : IsShownLevel(Piece.Y))
			{
				DrawCellBox(PDI, FVector(Piece.X + 0.1, Low + 1, Piece.Z + 0.1), FVector(Piece.X + 0.9, High + 1.3, Piece.Z + 0.9), PickMaterial.Get());
			}
		}
		PDI->SetHitProxy(nullptr);
	}
}

bool FLBAGridPlanVisualizer::VisProxyHandleClick(FEditorViewportClient* InViewportClient, HComponentVisProxy* VisProxy, const FViewportClick& Click)
{
	const HLBAGridPlanProxy* Proxy = HitProxyCast<HLBAGridPlanProxy>(VisProxy);
	ULBAGridPlanComponent* PlanView = Proxy ? const_cast<ULBAGridPlanComponent*>(Cast<ULBAGridPlanComponent>(Proxy->Component.Get())) : nullptr;
	if (!PlanView)
	{
		return false;
	}
	PlanView->Select(Proxy->Room, Proxy->Connector);
	// The Details panel shows the new selection's data.
	if (GEditor)
	{
		GEditor->NoteSelectionChange();
	}
	return true;
}
