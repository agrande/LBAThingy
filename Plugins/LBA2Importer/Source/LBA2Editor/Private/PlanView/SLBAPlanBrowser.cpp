#include "PlanView/SLBAPlanBrowser.h"

#include "Algo/Count.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "IStructureDetailsView.h"
#include "LBAGridPlanActor.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StructOnScope.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "LBAPlanBrowser"

/** A plan top-down at one level: X across, Z down, a square per column. */
class SLBAPlanCanvas : public SLeafWidget
{
public:
	DECLARE_DELEGATE_TwoParams(FOnSelect, int32 /*Room*/, int32 /*Connector*/);

	SLATE_BEGIN_ARGS(SLBAPlanCanvas) {}
		SLATE_EVENT(FOnSelect, OnSelect)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnSelect = InArgs._OnSelect;
		SetToolTipText(TAttribute<FText>::CreateSP(this, &SLBAPlanCanvas::GetHoverText));
	}

	const FLBAGridPlan* Plan = nullptr;
	int32 Level = INDEX_NONE;

	/** Shows a plan, framed on what it holds (all levels), with a column of margin and room for the numbers. */
	void SetPlan(const FLBAGridPlan* InPlan)
	{
		Plan = InPlan;
		FIntPoint Min(FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeXZ), Max(-1, -1);
		auto Add = [&Min, &Max](const FIntPoint& Column)
		{
			Min = Min.ComponentMin(Column);
			Max = Max.ComponentMax(Column);
		};
		if (Plan)
		{
			for (const FLBAGridPlanLevel& AtLevel : Plan->Levels)
			{
				for (int32 Index = 0; Index < AtLevel.Columns.Num(); ++Index)
				{
					if (AtLevel.Columns[Index] != ELBAColumn::Other)
					{
						Add(FIntPoint(Index % FLBAGridPlan::SizeXZ, Index / FLBAGridPlan::SizeXZ));
					}
				}
			}
			for (const FLBAGridPlanHollowColumn& Column : Plan->Hollow.Columns)
			{
				Add(Column.Column);
			}
		}
		if (Max.X < 0)
		{
			Min = FIntPoint(0, 0);
			Max = FIntPoint(FLBAGridPlan::SizeXZ - 1, FLBAGridPlan::SizeXZ - 1);
		}
		ViewMin = FVector2f(Min.X - 2.f, Min.Y - 2.f);
		ViewSize = FVector2f(Max.X - Min.X + 4.f, Max.Y - Min.Y + 4.f);
		Redraw();
	}
	int32 SelectedRoom = INDEX_NONE;
	int32 SelectedConnector = INDEX_NONE;
	bool bWalls = true;
	bool bRooms = true;
	bool bSides = true;
	bool bConnectors = true;
	bool bAir = false;

	void Redraw() { Invalidate(EInvalidateWidgetReason::Paint); }

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(512.0, 512.0); }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Culling, FSlateWindowElementList& Out, int32 Layer,
		const FWidgetStyle& Style, bool bParentEnabled) const override
	{
		const FSlateBrush* White = FAppStyle::GetBrush("WhiteBrush");
		const float Cell = GetCellSize(Geometry);
		// Columns (X, Z) to the widget's pixels.
		auto ToLocal = [this, Cell](float X, float Z) { return (FVector2f(X, Z) - ViewMin) * Cell; };
		auto Box = [&](float X, float Z, float W, float H, const FLinearColor& Color, int32 OnLayer)
		{
			FSlateDrawElement::MakeBox(Out, OnLayer, Geometry.ToPaintGeometry(FVector2f(W * Cell, H * Cell), FSlateLayoutTransform(ToLocal(X, Z))),
				White, ESlateDrawEffect::None, Color);
		};
		auto Line = [&](float X0, float Z0, float X1, float Z1, const FLinearColor& Color, float Thickness, int32 OnLayer)
		{
			TArray<FVector2f> Points = { ToLocal(X0, Z0), ToLocal(X1, Z1) };
			FSlateDrawElement::MakeLines(Out, OnLayer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true, Thickness);
		};
		auto Text = [&](float X, float Z, const FString& String, const FLinearColor& Color, int32 Size, int32 OnLayer)
		{
			FSlateDrawElement::MakeText(Out, OnLayer, Geometry.ToPaintGeometry(FVector2f(200.f, 40.f), FSlateLayoutTransform(ToLocal(X, Z))), String,
				FCoreStyle::GetDefaultFontStyle("Bold", Size), ESlateDrawEffect::None, Color);
		};
		auto Rectangle = [&](float X0, float Z0, float X1, float Z1, const FLinearColor& Color, float Thickness, int32 OnLayer)
		{
			Line(X0, Z0, X1, Z0, Color, Thickness, OnLayer);
			Line(X1, Z0, X1, Z1, Color, Thickness, OnLayer);
			Line(X1, Z1, X0, Z1, Color, Thickness, OnLayer);
			Line(X0, Z1, X0, Z0, Color, Thickness, OnLayer);
		};
		Box(0, 0, FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeXZ, FLinearColor(0.012f, 0.012f, 0.015f), Layer);
		// The grid every 4 columns, numbered along the top and the left (X across, Z down, as the text maps).
		const FVector2f ViewMax = ViewMin + ViewSize;
		const int32 NumberSize = FMath::Clamp(int32(Cell * 0.5f), 6, 9);
		for (int32 A = 0; A <= FLBAGridPlan::SizeXZ; A += 4)
		{
			const FLinearColor GridColor(1.f, 1.f, 1.f, A % 8 == 0 ? 0.1f : 0.04f);
			if (A >= ViewMin.X && A <= ViewMax.X)
			{
				Line(A, FMath::Max(ViewMin.Y + 1.f, 0.f), A, FMath::Min(ViewMax.Y, float(FLBAGridPlan::SizeXZ)), GridColor, 1.f, Layer);
				Text(A + 0.1f, ViewMin.Y + 0.1f, FString::FromInt(A), FLinearColor(1.f, 1.f, 1.f, 0.5f), NumberSize, Layer);
			}
			if (A >= ViewMin.Y && A <= ViewMax.Y)
			{
				Line(FMath::Max(ViewMin.X + 1.f, 0.f), A, FMath::Min(ViewMax.X, float(FLBAGridPlan::SizeXZ)), A, GridColor, 1.f, Layer);
				Text(ViewMin.X + 0.1f, A + 0.05f, FString::FromInt(A), FLinearColor(1.f, 1.f, 1.f, 0.5f), NumberSize, Layer);
			}
		}
		if (!Plan)
		{
			return Layer;
		}
		const float Gap = Cell >= 6.f ? 0.06f : 0.f;
		const int32 FontSize = FMath::Clamp(int32(Cell * 0.7f), 6, 12);

		if (Plan->Structure == ELBAEnclosureStructure::Hollow)
		{
			for (const FLBAGridPlanHollowColumn& Column : Plan->Hollow.Columns)
			{
				const FLinearColor Color = Column.bSupport ? FLinearColor(0.9f, 0.2f, 0.15f)
					: Column.bPillar ? FLinearColor(0.95f, 0.55f, 0.1f)
					: Column.bCanyon ? FLinearColor(0.25f, 0.08f, 0.3f)
					: Column.Floor == INDEX_NONE ? FLinearColor(0.1f, 0.15f, 0.3f)
					: FLinearColor::LerpUsingHSV(FLinearColor(0.2f, 0.5f, 0.9f), FLinearColor(0.95f, 0.85f, 0.3f), Column.Floor / float(FLBAGridPlan::SizeY));
				Box(Column.Column.X + Gap, Column.Column.Y + Gap, 1 - Gap * 2, 1 - Gap * 2, Color, Layer + 1);
				if (bAir && Cell >= 10.f)
				{
					Text(Column.Column.X + 0.2f, Column.Column.Y, FString::FromInt(FMath::FloorToInt(Column.Roof)), FLinearColor::Black, FMath::Max(FontSize - 2, 6), Layer + 4);
				}
			}
			DrawHover(Rectangle, Layer + 5);
			return Layer + 5;
		}

		const FLBAGridPlanLevel* AtLevel = Plan->FindLevel(Level);
		if (bWalls && AtLevel)
		{
			for (int32 Index = 0; Index < AtLevel->Columns.Num(); ++Index)
			{
				const int32 X = Index % FLBAGridPlan::SizeXZ, Z = Index / FLBAGridPlan::SizeXZ;
				const ELBAWallRule Rule = AtLevel->Rules.IsValidIndex(Index) ? AtLevel->Rules[Index] : ELBAWallRule::None;
				FLinearColor Color;
				switch (AtLevel->Columns[Index])
				{
				case ELBAColumn::Floor: Color = FLinearColor(0.32f, 0.32f, 0.32f); break;
				case ELBAColumn::Furniture: Color = Rule != ELBAWallRule::None ? FLinearColor(ULBAGridPlanComponent::GetRuleColor(Rule)) * 0.6f : FLinearColor(0.16f, 0.16f, 0.16f); break;
				case ELBAColumn::Stairs: Color = FLinearColor(0.45f, 0.4f, 0.1f); break;
				case ELBAColumn::Wall: Color = FLinearColor(ULBAGridPlanComponent::GetRuleColor(Rule)); break;
				default: continue;
				}
				Color.A = 1.f;
				Box(X + Gap, Z + Gap, 1 - Gap * 2, 1 - Gap * 2, Color, Layer + 1);
			}
			for (const FIntPoint& Column : AtLevel->Gaps)
			{
				Box(Column.X + 0.3f, Column.Y + 0.3f, 0.4f, 0.4f, FLinearColor::White, Layer + 3);
			}
			for (const FIntPoint& Column : AtLevel->PartitionLines)
			{
				Box(Column.X + 0.3f, Column.Y + 0.3f, 0.4f, 0.4f, FLinearColor(0.5f, 0.95f, 0.5f), Layer + 3);
			}
		}
		if (bConnectors)
		{
			for (int32 Index = 0; Index < Plan->Connectors.Num(); ++Index)
			{
				const FLBAGridPlanConnector& Connector = Plan->Connectors[Index];
				FLinearColor Color(ULBAGridPlanComponent::GetConnectorColor(Connector.Kind));
				Color.A = Index == SelectedConnector ? 0.95f : 0.6f;
				FVector2f Sum = FVector2f::ZeroVector;
				int32 Count = 0;
				for (const FIntVector& Piece : Connector.Columns)
				{
					if (Piece.Y == Level || (Piece.Y == INDEX_NONE && Connector.Levels.Contains(Level)))
					{
						Box(Piece.X + Gap, Piece.Z + Gap, 1 - Gap * 2, 1 - Gap * 2, Color, Layer + 2);
						Sum += FVector2f(Piece.X, Piece.Z);
						++Count;
					}
				}
				if (Count > 0 && Connector.Kind != ELBAConnector::Pocket && Cell >= 7.f)
				{
					Text(Sum.X / Count, Sum.Y / Count, FString::Printf(TEXT("C%d"), Index), FLinearColor::White, FMath::Max(FontSize - 2, 6), Layer + 6);
				}
			}
		}
		for (int32 Index = 0; Index < Plan->Rooms.Num(); ++Index)
		{
			const FLBAGridPlanRoom& Room = Plan->Rooms[Index];
			if (Room.Level != Level)
			{
				continue;
			}
			const bool bSelected = Index == SelectedRoom;
			const FLinearColor Color(ULBAGridPlanComponent::GetRoomColor(Index));
			const float X0 = Room.Min.X, Z0 = Room.Min.Y, X1 = Room.Max.X + 1, Z1 = Room.Max.Y + 1;
			if (bRooms)
			{
				Box(X0, Z0, X1 - X0, Z1 - Z0, Color.CopyWithNewOpacity(bSelected ? 0.6f : 0.4f), Layer + 2);
			}
			if (bSides)
			{
				// Each side just inside the room's edge, a band per run of what lies past it.
				const float Inset = 0.12f;
				for (int32 Side = 0; Side < 4; ++Side)
				{
					const bool bAlongZ = Side < 2;
					const float Edge = Side == 0 ? X0 + Inset : Side == 1 ? X1 - Inset : Side == 2 ? Z0 + Inset : Z1 - Inset;
					float From = bAlongZ ? Z0 : X0;
					for (const FLBAGridPlanSideRun& Run : Room.GetSide(Side).Runs)
					{
						const float To = From + Run.Count;
						const FLinearColor RunColor(ULBAGridPlanComponent::GetSideColor(Run.Kind, *Plan, Run.Other));
						if (bAlongZ)
						{
							Line(Edge, From + 0.1f, Edge, To - 0.1f, RunColor, 3.f, Layer + 4);
						}
						else
						{
							Line(From + 0.1f, Edge, To - 0.1f, Edge, RunColor, 3.f, Layer + 4);
						}
						From = To;
					}
				}
			}
			if (bSelected)
			{
				Rectangle(X0, Z0, X1, Z1, FLinearColor::White, 2.f, Layer + 5);
			}
			Text(X0 + 0.3f, Z0 + 0.25f, FString::Printf(TEXT("R%d  ceiling %d"), Index, Room.Ceiling), FLinearColor::White, FontSize, Layer + 6);
		}
		if (bAir && Cell >= 10.f)
		{
			for (const FLBAGridPlanAir& At : Plan->Air)
			{
				if (At.Level == Level)
				{
					Text(At.Column.X + 0.25f, At.Column.Y + 0.1f, FString::FromInt(At.Top - At.Floor - 1), FLinearColor(0.9f, 1.f, 0.9f), FMath::Max(FontSize - 3, 6), Layer + 6);
				}
			}
		}
		DrawHover(Rectangle, Layer + 7);
		return Layer + 7;
	}

	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		const FVector2f Local = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
		const float Cell = GetCellSize(Geometry);
		const FVector2f At = Local / Cell + ViewMin;
		const FIntPoint Column(FMath::FloorToInt(At.X), FMath::FloorToInt(At.Y));
		if (Column != Hovered)
		{
			Hovered = FLBAGridPlan::IsInGrid(Column) ? Column : FIntPoint(INDEX_NONE, INDEX_NONE);
			Redraw();
		}
		return FReply::Unhandled();
	}

	virtual void OnMouseLeave(const FPointerEvent& Event) override
	{
		Hovered = FIntPoint(INDEX_NONE, INDEX_NONE);
		Redraw();
	}

	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
	{
		if (Event.GetEffectingButton() != EKeys::LeftMouseButton || !Plan)
		{
			return FReply::Unhandled();
		}
		OnMouseMove(Geometry, Event);
		const int32 Room = FLBAGridPlan::IsInGrid(Hovered) ? Plan->FindRoom(Level, Hovered) : INDEX_NONE;
		const int32 Connector = FLBAGridPlan::IsInGrid(Hovered) && Room == INDEX_NONE ? FindConnector(Hovered) : INDEX_NONE;
		OnSelect.ExecuteIfBound(Room, Connector);
		return FReply::Handled();
	}

private:
	FOnSelect OnSelect;
	FIntPoint Hovered = FIntPoint(INDEX_NONE, INDEX_NONE);
	/** The columns shown: from ViewMin, ViewSize across (SetPlan). */
	FVector2f ViewMin = FVector2f::ZeroVector;
	FVector2f ViewSize = FVector2f(FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeXZ);

	float GetCellSize(const FGeometry& Geometry) const
	{
		const FVector2f Size = Geometry.GetLocalSize();
		return FMath::Max(FMath::Min(Size.X / ViewSize.X, Size.Y / ViewSize.Y), 1.f);
	}

	int32 FindConnector(const FIntPoint& Column) const
	{
		const int32 Found = Plan->FindConnector(Level, Column);
		if (Found != INDEX_NONE)
		{
			return Found;
		}
		const int32 Flight = Plan->FindConnector(INDEX_NONE, Column);
		return Flight != INDEX_NONE && Plan->Connectors[Flight].Levels.Contains(Level) ? Flight : INDEX_NONE;
	}

	template <typename FRectangle>
	void DrawHover(const FRectangle& Rectangle, int32 OnLayer) const
	{
		if (FLBAGridPlan::IsInGrid(Hovered))
		{
			Rectangle(Hovered.X, Hovered.Y, Hovered.X + 1, Hovered.Y + 1, FLinearColor(1.f, 1.f, 1.f, 0.8f), 1.f, OnLayer);
		}
	}

	/** What the hovered column is, at every level. */
	FText GetHoverText() const
	{
		if (!Plan || !FLBAGridPlan::IsInGrid(Hovered))
		{
			return FText::GetEmpty();
		}
		TArray<FString> Lines = { FString::Printf(TEXT("Column x %d, z %d"), Hovered.X, Hovered.Y) };
		if (Plan->Structure == ELBAEnclosureStructure::Hollow)
		{
			const FLBAGridPlanHollowColumn* Column = Plan->Hollow.Columns.FindByPredicate([this](const FLBAGridPlanHollowColumn& At) { return At.Column == Hovered; });
			Lines.Add(!Column ? TEXT("rock") : Column->bSupport ? FString::Printf(TEXT("support (the roof comes down on it), roof %.1f"), Column->Roof)
				: Column->bPillar ? FString::Printf(TEXT("pillar (the roof comes down onto its top and arches to the next), roof %.1f"), Column->Roof)
				: Column->bCanyon ? FString::Printf(TEXT("canyon past a ledge, roof %.1f"), Column->Roof)
				: FString::Printf(TEXT("floor %d, roof springs at %.1f, roof %.1f"), Column->Floor, Column->Spring, Column->Roof));
			return FText::FromString(FString::Join(Lines, TEXT("\n")));
		}
		for (const FLBAGridPlanLevel& AtLevel : Plan->Levels)
		{
			const ELBAColumn Kind = AtLevel.GetColumn(Hovered);
			const ELBAWallRule Rule = AtLevel.GetRule(Hovered);
			FString Line = FString::Printf(TEXT("%slevel %d: %s"), AtLevel.Height == Level ? TEXT("> ") : TEXT("   "), AtLevel.Height,
				*StaticEnum<ELBAColumn>()->GetDisplayNameTextByValue(int64(Kind)).ToString());
			if (Rule != ELBAWallRule::None)
			{
				Line += FString::Printf(TEXT(" (%s)"), *StaticEnum<ELBAWallRule>()->GetDisplayNameTextByValue(int64(Rule)).ToString());
			}
			if (AtLevel.Gaps.Contains(Hovered))
			{
				Line += TEXT(", gap in a wall line");
			}
			if (AtLevel.PartitionLines.Contains(Hovered))
			{
				Line += TEXT(", on a partition's line");
			}
			const int32 Room = Plan->FindRoom(AtLevel.Height, Hovered);
			if (Room != INDEX_NONE)
			{
				Line += FString::Printf(TEXT(", room R%d (space S%d)"), Room, Plan->Rooms[Room].Space);
			}
			const int32 Connector = Plan->FindConnector(AtLevel.Height, Hovered);
			if (Connector != INDEX_NONE)
			{
				Line += FString::Printf(TEXT(", connector C%d (%s)"), Connector,
					*StaticEnum<ELBAConnector>()->GetDisplayNameTextByValue(int64(Plan->Connectors[Connector].Kind)).ToString());
			}
			for (const FLBAGridPlanAir& At : Plan->Air)
			{
				if (At.Level == AtLevel.Height && At.Column == Hovered)
				{
					Line += FString::Printf(TEXT(", air %d-%d (%d cells)"), At.Floor + 1, At.Top - 1, At.Top - At.Floor - 1);
				}
			}
			if (Kind != ELBAColumn::Other || Room != INDEX_NONE || Connector != INDEX_NONE)
			{
				Lines.Add(Line);
			}
		}
		const int32 Flight = Plan->FindConnector(INDEX_NONE, Hovered);
		if (Flight != INDEX_NONE)
		{
			Lines.Add(FString::Printf(TEXT("   flight of stairs, connector C%d"), Flight));
		}
		if (Plan->BeyondExits.Contains(Hovered))
		{
			Lines.Add(TEXT("   past an exit's doorway: left open"));
		}
		return FText::FromString(FString::Join(Lines, TEXT("\n")));
	}
};

void SLBAPlanBrowser::Construct(const FArguments& InArgs)
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = true;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Details = PropertyEditor.CreateStructureDetailView(DetailsArgs, FStructureDetailsViewArgs(), nullptr, LOCTEXT("Selection", "Selection"));
	// Read only: the plan is what the importer understood of the grid (edits will come as overrides).
	Details->GetDetailsView()->SetIsPropertyEditingEnabledDelegate(FIsPropertyEditingEnabled::CreateLambda([]() { return false; }));

	auto Toggle = [this](const FText& Label, bool SLBAPlanCanvas::* Flag, const FText& Tip)
	{
		return SNew(SCheckBox)
			.ToolTipText(Tip)
			.IsChecked_Lambda([this, Flag]() { return Canvas.IsValid() && (*Canvas).*Flag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Flag](ECheckBoxState State)
			{
				(*Canvas).*Flag = State == ECheckBoxState::Checked;
				Canvas->Redraw();
			})
			[
				SNew(STextBlock).Text(Label)
			];
	};

	ChildSlot
	[
		SNew(SSplitter)
		+ SSplitter::Slot().Value(0.2f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("Search", "Grid number or name"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					Filter = Text.ToString();
					ApplyFilter();
				})
			]
			+ SVerticalBox::Slot().FillHeight(1.f)
			[
				SAssignNew(List, SListView<FEntryPtr>)
				.ListItemsSource(&Filtered)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow_Lambda([](FEntryPtr Entry, const TSharedRef<STableViewBase>& Owner)
				{
					return SNew(STableRow<FEntryPtr>, Owner)[SNew(STextBlock).Text(FText::FromString(Entry->Label))];
				})
				.OnSelectionChanged_Lambda([this](FEntryPtr Entry, ESelectInfo::Type)
				{
					if (Entry)
					{
						ShowEntry(Entry);
					}
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.ToolTipText(LOCTEXT("RefreshTip", "Finds the plans again (after an import)"))
				.OnClicked_Lambda([this]() { Refresh(); return FReply::Handled(); })
			]
		]
		+ SSplitter::Slot().Value(0.52f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(4.f, 2.f))
				+ SWrapBox::Slot().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("Level", "Level"))
				]
				+ SWrapBox::Slot()
				[
					SNew(SComboBox<TSharedPtr<int32>>)
					.OptionsSource(&LevelOptions)
					.OnGenerateWidget_Lambda([](TSharedPtr<int32> Option)
					{
						return SNew(STextBlock).Text(FText::AsNumber(*Option));
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<int32> Option, ESelectInfo::Type)
					{
						if (Option)
						{
							Level = *Option;
							Canvas->Level = Level;
							Canvas->Redraw();
						}
					})
					[
						SNew(STextBlock).Text_Lambda([this]()
						{
							const FLBAGridPlan* Plan = GetPlan();
							return !Plan ? FText::GetEmpty() : Plan->Structure == ELBAEnclosureStructure::Hollow ? LOCTEXT("Hollow", "hollow") : FText::AsNumber(Level);
						})
					]
				]
				+ SWrapBox::Slot().VAlign(VAlign_Center)[Toggle(LOCTEXT("Walls", "Columns"), &SLBAPlanCanvas::bWalls,
					LOCTEXT("WallsTip", "What each column is at the level: floor light gray, furniture dark gray, stairs olive, walls by their rule (cut orange, jamb yellow, invisible pale blue, masonry brown, exterior gray); gaps in wall lines white, partition lines green"))]
				+ SWrapBox::Slot().VAlign(VAlign_Center)[Toggle(LOCTEXT("Rooms", "Rooms"), &SLBAPlanCanvas::bRooms, LOCTEXT("RoomsTip", "The rooms' rectangles"))]
				+ SWrapBox::Slot().VAlign(VAlign_Center)[Toggle(LOCTEXT("Sides", "Sides"), &SLBAPlanCanvas::bSides,
					LOCTEXT("SidesTip", "What lies past each room side: wall red, open side green, connector its kind's color, step orange, open gray"))]
				+ SWrapBox::Slot().VAlign(VAlign_Center)[Toggle(LOCTEXT("Connectors", "Connectors"), &SLBAPlanCanvas::bConnectors,
					LOCTEXT("ConnectorsTip", "Stairs yellow, passage cyan, exit magenta, alcove blue, pocket gray"))]
				+ SWrapBox::Slot().VAlign(VAlign_Center)[Toggle(LOCTEXT("Air", "Air"), &SLBAPlanCanvas::bAir,
					LOCTEXT("AirTip", "Cells of air over each column (zoom in): ceilings, lintels, gallery undersides"))]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("ShowInLevel", "Show in Level"))
					.ToolTipText(LOCTEXT("ShowInLevelTip", "Opens the grid's level, turns its plan overlay on (Show > Developer > LBA2 Grid Plan) at this level and frames the selection"))
					.IsEnabled_Lambda([this]() { return Current.IsValid(); })
					.OnClicked_Lambda([this]() { ShowInLevel(); return FReply::Handled(); })
				]
				+ SWrapBox::Slot()
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenAsset", "Open Asset"))
					.IsEnabled_Lambda([this]() { return Current.IsValid(); })
					.OnClicked_Lambda([this]()
					{
						GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Current->Asset.Get());
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.f).Padding(2.f)
			[
				SAssignNew(Canvas, SLBAPlanCanvas)
				.OnSelect_Lambda([this](int32 Room, int32 Connector) { OnSelectionChanged(Room, Connector); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(4.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]()
				{
					const FLBAGridPlan* Plan = GetPlan();
					if (!Plan)
					{
						return LOCTEXT("NoPlan", "Pick a grid. No grid listed: import the interiors (plans are saved as DA_Grid_NNN_Plan).");
					}
					return FText::FromString(FString::Printf(TEXT("Grid %d %s, library %d: %s%s, room height %d. %d rooms, %d spaces, %d connectors. X across, Z down; the iso camera looks from +X +Z (bottom right). Hover a column for what it is at every level."),
						Plan->Grid, *Plan->Name, Plan->Library, Plan->Structure == ELBAEnclosureStructure::Hollow ? TEXT("hollow") : TEXT("rooms"),
						Plan->bEnclosed ? TEXT(", enclosed") : TEXT(", not enclosed"), Plan->RoomHeight, Plan->Rooms.Num(), Plan->Spaces.Num(), Plan->Connectors.Num()));
				})
			]
		]
		+ SSplitter::Slot().Value(0.28f)
		[
			Details->GetWidget().ToSharedRef()
		]
	];
	Refresh();
}

const FLBAGridPlan* SLBAPlanBrowser::GetPlan() const
{
	return Current && Current->Asset ? &Current->Asset->Plan : nullptr;
}

void SLBAPlanBrowser::Refresh()
{
	const int32 Grid = GetPlan() ? GetPlan()->Grid : INDEX_NONE;
	Entries.Reset();
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByClass(ULBAGridPlanAsset::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Data : Assets)
	{
		ULBAGridPlanAsset* Asset = Cast<ULBAGridPlanAsset>(Data.GetAsset());
		if (!Asset)
		{
			continue;
		}
		const FLBAGridPlan& Plan = Asset->Plan;
		FString Name = Plan.Name;
		Name.RemoveFromStart(FString::Printf(TEXT("L_Grid_%03d_"), Plan.Grid));
		FEntryPtr Entry = MakeShared<FEntry>();
		Entry->Asset.Reset(Asset);
		Entry->Label = FString::Printf(TEXT("%03d  %s  (%s)"), Plan.Grid, *Name, Plan.Structure == ELBAEnclosureStructure::Hollow ? TEXT("hollow")
			: *FString::Printf(TEXT("%d rooms%s"), Plan.Rooms.Num(), Plan.bEnclosed ? TEXT("") : TEXT(", open")));
		Entries.Add(Entry);
	}
	Entries.Sort([](const FEntryPtr& A, const FEntryPtr& B) { return A->Asset->Plan.Grid < B->Asset->Plan.Grid; });
	ApplyFilter();
	SelectGrid(Grid);
}

void SLBAPlanBrowser::ApplyFilter()
{
	Filtered = Entries.FilterByPredicate([this](const FEntryPtr& Entry) { return Filter.IsEmpty() || Entry->Label.Contains(Filter); });
	List->RequestListRefresh();
}

void SLBAPlanBrowser::SelectGrid(int32 Grid)
{
	for (const FEntryPtr& Entry : Filtered)
	{
		if (Entry->Asset->Plan.Grid == Grid)
		{
			List->SetSelection(Entry);
			List->RequestScrollIntoView(Entry);
			return;
		}
	}
}

void SLBAPlanBrowser::ShowEntry(FEntryPtr Entry)
{
	Current = Entry;
	const FLBAGridPlan& Plan = Entry->Asset->Plan;
	LevelOptions.Reset();
	// The level with the most rooms first shown.
	int32 Best = INDEX_NONE, BestRooms = -1;
	for (const FLBAGridPlanLevel& AtLevel : Plan.Levels)
	{
		LevelOptions.Add(MakeShared<int32>(AtLevel.Height));
		const int32 Rooms = Algo::CountIf(Plan.Rooms, [&AtLevel](const FLBAGridPlanRoom& Room) { return Room.Level == AtLevel.Height; });
		if (Rooms > BestRooms)
		{
			Best = AtLevel.Height;
			BestRooms = Rooms;
		}
	}
	Level = Best;
	Canvas->Level = Level;
	Canvas->SetPlan(&Plan);
	OnSelectionChanged(INDEX_NONE, INDEX_NONE);
}

void SLBAPlanBrowser::OnSelectionChanged(int32 Room, int32 Connector)
{
	SelectedRoom = Room;
	SelectedConnector = Connector;
	Canvas->SelectedRoom = Room;
	Canvas->SelectedConnector = Connector;
	Canvas->Redraw();
	// The selection's data (the asset's own: read only), else the whole plan.
	ULBAGridPlanAsset* Asset = Current ? Current->Asset.Get() : nullptr;
	if (!Asset)
	{
		Details->SetStructureData(nullptr);
	}
	else if (Asset->Plan.Rooms.IsValidIndex(Room))
	{
		Details->SetStructureData(MakeShared<FStructOnScope>(FLBAGridPlanRoom::StaticStruct(), reinterpret_cast<uint8*>(&Asset->Plan.Rooms[Room])));
	}
	else if (Asset->Plan.Connectors.IsValidIndex(Connector))
	{
		Details->SetStructureData(MakeShared<FStructOnScope>(FLBAGridPlanConnector::StaticStruct(), reinterpret_cast<uint8*>(&Asset->Plan.Connectors[Connector])));
	}
	else
	{
		Details->SetStructureData(MakeShared<FStructOnScope>(FLBAGridPlan::StaticStruct(), reinterpret_cast<uint8*>(&Asset->Plan)));
	}
}

void SLBAPlanBrowser::ShowInLevel() const
{
	const ULBAGridPlanAsset* Asset = Current ? Current->Asset.Get() : nullptr;
	if (!Asset || !GEditor)
	{
		return;
	}
	const FString Package = Asset->Level.ToSoftObjectPath().GetLongPackageName();
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World || World->GetPackage()->GetName() != Package)
	{
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Package, Filename, FPackageName::GetMapPackageExtension())
			|| !FEditorFileUtils::SaveDirtyPackages(true, true, false))
		{
			return;
		}
		World = FEditorFileUtils::LoadMap(Filename) ? GEditor->GetEditorWorldContext().World() : nullptr;
	}
	if (!World)
	{
		return;
	}
	const FLBAGridPlan& Plan = Asset->Plan;
	const bool bHollow = Plan.Structure == ELBAEnclosureStructure::Hollow;
	for (TActorIterator<ALBAGridPlanActor> It(World); It; ++It)
	{
		ULBAGridPlanComponent* PlanView = It->PlanView;
		PlanView->OnlyLevel = bHollow ? INDEX_NONE : Level;
		PlanView->Select(SelectedRoom, SelectedConnector);
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(*It, true, true);
	}
	// The overlay on in the level viewports.
	const int32 Flag = FEngineShowFlags::FindIndexByName(TEXT("LBAGridPlan"));
	for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Flag != INDEX_NONE)
		{
			Client->EngineShowFlags.SetSingleFlag(Flag, true);
		}
		Client->Invalidate();
	}
	// Frame the selection, else the level's rooms, else the whole grid.
	FBox Box(ForceInit);
	auto AddRoom = [&Box](const FLBAGridPlanRoom& Room)
	{
		Box += FLBAGridPlan::CellToWorld(Room.Min.X, Room.Level + 1, Room.Min.Y);
		Box += FLBAGridPlan::CellToWorld(Room.Max.X + 1, Room.Ceiling, Room.Max.Y + 1);
	};
	if (Plan.Rooms.IsValidIndex(SelectedRoom))
	{
		AddRoom(Plan.Rooms[SelectedRoom]);
	}
	else if (Plan.Connectors.IsValidIndex(SelectedConnector))
	{
		for (const FIntVector& Piece : Plan.Connectors[SelectedConnector].Columns)
		{
			const int32 Floor = Piece.Y != INDEX_NONE ? Piece.Y : Level;
			Box += FLBAGridPlan::CellToWorld(Piece.X, Floor + 1, Piece.Z);
			Box += FLBAGridPlan::CellToWorld(Piece.X + 1, Floor + 4, Piece.Z + 1);
		}
	}
	else
	{
		for (const FLBAGridPlanRoom& Room : Plan.Rooms)
		{
			if (Room.Level == Level)
			{
				AddRoom(Room);
			}
		}
	}
	if (!Box.IsValid)
	{
		Box = FBox(FLBAGridPlan::CellToWorld(0, 0, 0), FLBAGridPlan::CellToWorld(FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeY, FLBAGridPlan::SizeXZ));
	}
	GEditor->MoveViewportCamerasToBox(Box, false);
}

#undef LOCTEXT_NAMESPACE
