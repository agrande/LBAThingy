#include "LBAIslandMenu.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/Pawn.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SViewport.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LBAControls.h"
#include "LBAPlayerController.h"
#include "LBAPointsOfInterest.h"

#define LOCTEXT_NAMESPACE "LBAIslandMenu"

namespace
{
	constexpr float TabIconSize = 30.f;
	constexpr float IconSize = 22.f;
	constexpr float IndentPerLevel = 26.f;
	constexpr float MarkerWidth = 5.f;
	/** The player is at a station or a showcase spot when this close to it (cm). */
	constexpr double StationReach = 1500.0;

	UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color, ETextJustify::Type Justify = ETextJustify::Center,
		bool bBold = false)
	{
		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>();
		Block->SetText(Text);
		Block->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetJustification(Justify);
		return Block;
	}

	FSlateBrush BoxBrush(const FLinearColor& Color, float Radius, const FLinearColor& Outline = FLinearColor::Transparent, float OutlineWidth = 0.f)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings = FSlateBrushOutlineSettings(Radius, FSlateColor(Outline), OutlineWidth);
		return Brush;
	}

	/** Square-cornered bar: its background color (set every tick) is the whole look, like the original's menu entries. */
	const FButtonStyle& BarButtonStyle()
	{
		static const FButtonStyle Style = []
		{
			const FSlateBrush Brush = BoxBrush(FLinearColor::White, 2.f);
			FButtonStyle Result;
			Result.SetNormal(Brush).SetHovered(Brush).SetPressed(Brush).SetDisabled(Brush);
			Result.SetNormalPadding(FMargin(0.f)).SetPressedPadding(FMargin(0.f));
			return Result;
		}();
		return Style;
	}

	TSoftObjectPtr<UTexture2D> PluginIcon(const TCHAR* Name)
	{
		return TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(TEXT("/LBA2Importer/UI/%s.%s"), Name, Name)));
	}

	FString MenuWorldPackage(const UWorld* World)
	{
		return World ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) : FString();
	}

	bool IsListEntry(ELBAMenuAction Action)
	{
		return Action == ELBAMenuAction::Scene || Action == ELBAMenuAction::IslandStart || Action == ELBAMenuAction::Station
			|| Action == ELBAMenuAction::TestLevelStart || Action == ELBAMenuAction::Point;
	}
}

void ULBAIslandMenuButtonHandler::HandleClicked()
{
	if (Menu)
	{
		Menu->OnButtonClicked(Action, Value);
	}
}

ULBAIslandMenu::ULBAIslandMenu(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// The menu's own icons (Tools/make_menu_icons.py, import_location_icons.py).
	PlanetIcons.Add(ELBAPlanet::Twinsun, PluginIcon(TEXT("T_Menu_Twinsun")));
	PlanetIcons.Add(ELBAPlanet::EmeraldMoon, PluginIcon(TEXT("T_Menu_Moon")));
	PlanetIcons.Add(ELBAPlanet::ZeelishSurface, PluginIcon(TEXT("T_Menu_Zeelish")));
	PlanetIcons.Add(ELBAPlanet::ZeelishUndergas, PluginIcon(TEXT("T_Menu_Undergas")));
	IslandIcon = PluginIcon(TEXT("T_Menu_Island"));
	SectionIcon = PluginIcon(TEXT("T_Menu_Section"));
	BuildingIcon = PluginIcon(TEXT("T_Menu_Building"));
	TestsIcon = PluginIcon(TEXT("T_Menu_Tests"));
	PointsIcon = PluginIcon(TEXT("T_Menu_Points"));
}

TArray<FSoftObjectPath> ULBAIslandMenu::GetIconPaths() const
{
	TArray<FSoftObjectPath> Paths;
	for (const TPair<ELBAPlanet, TSoftObjectPtr<UTexture2D>>& Pair : PlanetIcons)
	{
		Paths.Add(Pair.Value.ToSoftObjectPath());
	}
	Paths.Add(IslandIcon.ToSoftObjectPath());
	Paths.Add(SectionIcon.ToSoftObjectPath());
	Paths.Add(BuildingIcon.ToSoftObjectPath());
	Paths.Add(TestsIcon.ToSoftObjectPath());
	Paths.Add(PointsIcon.ToSoftObjectPath());
	Paths.RemoveAll([](const FSoftObjectPath& Path) { return Path.IsNull(); });
	return Paths;
}

TSharedRef<SWidget> ULBAIslandMenu::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

bool ULBAIslandMenu::GetViewLocation(FVector& OutLocation) const
{
	const APawn* Pawn = GetOwningPlayerPawn();
	if (!Pawn)
	{
		return false;
	}
	OutLocation = Pawn->GetActorLocation();
	return true;
}

int32 ULBAIslandMenu::GetCurrentScene() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const ULBASceneTravelSubsystem* SceneTravel = GameInstance ? GameInstance->GetSubsystem<ULBASceneTravelSubsystem>() : nullptr;
	return SceneTravel ? SceneTravel->GetCurrentScene() : INDEX_NONE;
}

void ULBAIslandMenu::BuildTree()
{
	SetIsFocusable(true);

	// Where the player is: an island level, and the scene under them (an interior's tells its island), or a test level
	// and the station they are at.
	const UWorld* World = GetWorld();
	const FString WorldPackage = MenuWorldPackage(World);
	UGameInstance* GameInstance = GetGameInstance();
	const ULBASceneTravelSubsystem* SceneTravel = GameInstance ? GameInstance->GetSubsystem<ULBASceneTravelSubsystem>() : nullptr;
	Islands = ULBAIslandTravelSubsystem::FindIslands();
	Scenes = ULBASceneTravelSubsystem::FindMenuScenes();
	TestLevels = ULBAIslandTravelSubsystem::FindTestLevels(World);
	CurrentIsland = Islands.IndexOfByPredicate([&WorldPackage](const FLBAIslandInfo& Island)
	{
		return Island.Level.ToSoftObjectPath().GetLongPackageName() == WorldPackage;
	});
	CurrentScene = GetCurrentScene();
	HomeIsland = CurrentIsland;
	if (HomeIsland == INDEX_NONE)
	{
		const FLBASceneMenuEntry* Scene = Scenes.FindByPredicate([this](const FLBASceneMenuEntry& Entry) { return Entry.SceneIndex == CurrentScene; });
		if (Scene)
		{
			// The island variant the player came from (storm or sunny Citadel...), else the first.
			const FString LastLevel = SceneTravel ? SceneTravel->GetLastIslandLevel(Scene->Island) : FString();
			for (int32 Index = 0; Index < Islands.Num(); ++Index)
			{
				if (Islands[Index].SceneIsland == Scene->Island
					&& (HomeIsland == INDEX_NONE || Islands[Index].Level.ToSoftObjectPath().GetLongPackageName() == LastLevel))
				{
					HomeIsland = Index;
				}
			}
		}
	}
	CurrentTestLevel = TestLevels.IndexOfByPredicate([&WorldPackage](const FLBATestLevelInfo& Level)
	{
		return Level.Level.ToSoftObjectPath().GetLongPackageName() == WorldPackage;
	});
	CurrentStation = NAME_None;
	FVector ViewLocation;
	const bool bViewKnown = GetViewLocation(ViewLocation);
	if (TestLevels.IsValidIndex(CurrentTestLevel) && bViewKnown)
	{
		double Closest = StationReach;
		for (const FLBATestStationInfo& Station : TestLevels[CurrentTestLevel].Stations)
		{
			const double Distance = FVector::Dist(Station.Transform.GetLocation(), ViewLocation);
			if (Distance < Closest)
			{
				Closest = Distance;
				CurrentStation = Station.Tag;
			}
		}
	}

	// Showcase places: their headings, and the one the player is at (an interior's scene, or within reach of a spot).
	const TArray<FLBAPointOfInterest>& Points = LBAPointsOfInterest::Get();
	PointGroups.Reset();
	CurrentPoint = INDEX_NONE;
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FLBAPointOfInterest& Point = Points[Index];
		if (!PointGroups.ContainsByPredicate([&Point](const FText& Group) { return Group.EqualTo(Point.Group); }))
		{
			PointGroups.Add(Point.Group);
		}
		const TOptional<FVector> Feet = ULBASceneTravelSubsystem::FindPointOfInterestFeet(World, Point);
		const bool bHere = Point.Scene != INDEX_NONE ? Point.Scene == CurrentScene && CurrentIsland == INDEX_NONE
			: Feet && bViewKnown && FVector::Dist(*Feet, ViewLocation) < StationReach;
		if (bHere && CurrentPoint == INDEX_NONE)
		{
			CurrentPoint = Index;
		}
	}
	SelectedPointGroup = Points.IsValidIndex(CurrentPoint)
		? FMath::Max(0, PointGroups.IndexOfByPredicate([&](const FText& Group) { return Group.EqualTo(Points[CurrentPoint].Group); }))
		: 0;

	Planets.Reset();
	for (const FLBAIslandInfo& Island : Islands)
	{
		Planets.AddUnique(Island.Planet);
	}
	Planets.Sort();
	SelectedIsland = Islands.IsValidIndex(HomeIsland) ? HomeIsland : (Islands.IsEmpty() ? INDEX_NONE : 0);
	SelectedPlanet = Islands.IsValidIndex(SelectedIsland) ? Islands[SelectedIsland].Planet : ELBAPlanet::Twinsun;
	SelectedTestLevel = TestLevels.IsValidIndex(CurrentTestLevel) ? CurrentTestLevel : (TestLevels.IsEmpty() ? INDEX_NONE : 0);
	// The menu always opens on the showcase places.
	bShowPoints = !Points.IsEmpty();
	bShowTests = !bShowPoints && (TestLevels.IsValidIndex(CurrentTestLevel) || (Islands.IsEmpty() && !TestLevels.IsEmpty()));

	// Full-screen dim, with the framed panel over most of it. The editor's tab is the panel.
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Dim"));
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, bEditorMode ? 0.f : 0.6f));
	Dim->SetPadding(bEditorMode ? FMargin(0.f) : FMargin(64.f, 40.f));
	WidgetTree->RootWidget = Dim;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Panel"));
	Panel->SetBrush(BoxBrush(PanelColor, bEditorMode ? 0.f : 4.f, BorderColor, bEditorMode ? 0.f : 3.f));
	Panel->SetPadding(FMargin(bEditorMode ? 2.f : 6.f));
	Dim->SetContent(Panel);

	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Content"));
	Panel->SetContent(Content);

	// Tabs: the showcase places, the planets, then Tests.
	UHorizontalBox* PlanetBar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Planets"));
	UVerticalBoxSlot* PlanetBarSlot = Content->AddChildToVerticalBox(PlanetBar);
	PlanetBarSlot->SetPadding(FMargin(6.f, 6.f, 6.f, 8.f));
	PlanetButtons.Reset();
	auto AddTab = [this, PlanetBar](FLBAMenuButton& Button)
	{
		UHorizontalBoxSlot* ButtonSlot = PlanetBar->AddChildToHorizontalBox(Button.Button);
		ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ButtonSlot->SetPadding(FMargin(3.f, 0.f));
	};
	if (!Points.IsEmpty())
	{
		FLBAMenuButton& Button = AddButton(PlanetButtons, ELBAMenuAction::PointsTab, INDEX_NONE, LOCTEXT("Points", "Points of interest"), PointsIcon, true);
		Button.bCurrent = Points.IsValidIndex(CurrentPoint);
		AddTab(Button);
	}
	for (const ELBAPlanet Planet : Planets)
	{
		FLBAMenuButton& Button = AddButton(PlanetButtons, ELBAMenuAction::Planet, int32(Planet), ULBAIslandTravelSubsystem::GetPlanetName(Planet),
			PlanetIcons.FindRef(Planet), true);
		Button.bCurrent = Islands.IsValidIndex(HomeIsland) && Islands[HomeIsland].Planet == Planet;
		AddTab(Button);
	}
	if (!TestLevels.IsEmpty())
	{
		FLBAMenuButton& Button = AddButton(PlanetButtons, ELBAMenuAction::TestsTab, INDEX_NONE, LOCTEXT("Tests", "Tests"), TestsIcon, true);
		Button.bCurrent = TestLevels.IsValidIndex(CurrentTestLevel);
		AddTab(Button);
	}

	UBorder* Line = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Line"));
	Line->SetBrushColor(BorderColor);
	Line->SetPadding(FMargin(0.f, 1.f));
	Content->AddChildToVerticalBox(Line);

	// The list (islands, test levels, headings) on the left, what the selected entry holds on the right.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Body"));
	UVerticalBoxSlot* BodySlot = Content->AddChildToVerticalBox(Body);
	BodySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	BodySlot->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));

	USizeBox* IslandColumn = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("IslandColumn"));
	IslandColumn->SetWidthOverride(bEditorMode ? 260.f : 360.f);
	Body->AddChildToHorizontalBox(IslandColumn);
	UScrollBox* IslandScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("IslandScroll"));
	IslandScroll->SetScrollWhenFocusChanges(EScrollWhenFocusChanges::AnimatedScroll);
	IslandColumn->SetContent(IslandScroll);
	IslandList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Islands"));
	Cast<UScrollBoxSlot>(IslandScroll->AddChild(IslandList))->SetPadding(FMargin(6.f, 2.f, 10.f, 6.f));

	UBorder* TreePanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("TreePanel"));
	TreePanel->SetBrush(BoxBrush(TreeColor, 3.f, BorderColor * FLinearColor(1.f, 1.f, 1.f, 0.5f), 1.f));
	TreePanel->SetPadding(FMargin(10.f, 8.f));
	Body->AddChildToHorizontalBox(TreePanel)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	SceneTree = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scenes"));
	SceneTree->SetScrollWhenFocusChanges(EScrollWhenFocusChanges::AnimatedScroll);
	TreePanel->SetContent(SceneTree);

	// Footer: controls, Quit (the game) or Play (the editor).
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Footer"));
	Content->AddChildToVerticalBox(Footer)->SetPadding(FMargin(10.f, 8.f, 6.f, 2.f));
	UTextBlock* Hint = MakeText(WidgetTree, FText::GetEmpty(), 14, IdleTextColor, ETextJustify::Left);
	Hint->SetAutoWrapText(true);
	FooterHint = Hint;
	FooterHintLayout = INDEX_NONE;
	UpdateFooterHint();
	UHorizontalBoxSlot* HintSlot = Footer->AddChildToHorizontalBox(Hint);
	HintSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	HintSlot->SetVerticalAlignment(VAlign_Center);
	FooterButtons.Reset();
	if (bEditorMode)
	{
		FLBAMenuButton& PlayButton = AddButton(FooterButtons, ELBAMenuAction::Play, INDEX_NONE, LOCTEXT("Play", "Play here"), nullptr, false);
		PlayButton.Button->SetToolTipText(LOCTEXT("PlayTip", "Plays in the editor from the place picked last (or the level's start)."));
		Footer->AddChildToHorizontalBox(PlayButton.Button);
	}
	else
	{
		Footer->AddChildToHorizontalBox(AddButton(FooterButtons, ELBAMenuAction::Quit, INDEX_NONE, LOCTEXT("Quit", "Quit"), nullptr, false).Button);
	}

	BuildIslandList();
	BuildSceneTree();
}

void ULBAIslandMenu::BuildIslandList()
{
	IslandList->ClearChildren();
	IslandButtons.Reset();
	for (FLBAMenuButton& Button : PlanetButtons)
	{
		const ELBAMenuAction Action = Button.Handler->Action;
		Button.bActive = Action == ELBAMenuAction::TestsTab ? bShowTests
			: Action == ELBAMenuAction::PointsTab ? bShowPoints
			: !bShowTests && !bShowPoints && Button.Handler->Value == int32(SelectedPlanet);
	}
	if (bShowPoints)
	{
		BuildPointGroupList();
		return;
	}
	if (bShowTests)
	{
		BuildTestLevelList();
		return;
	}
	for (int32 Index = 0; Index < Islands.Num(); ++Index)
	{
		if (Islands[Index].Planet == SelectedPlanet)
		{
			FLBAMenuButton& Button = AddButton(IslandButtons, ELBAMenuAction::Island, Index, Islands[Index].DisplayName, IslandIcon, true);
			Button.bCurrent = Index == HomeIsland;
			IslandList->AddChildToVerticalBox(Button.Button)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
		}
	}
	if (Islands.IsEmpty())
	{
		UTextBlock* None = MakeText(WidgetTree, LOCTEXT("NoIslands", "No island imported yet (Tools > LBA2 Importer)."), 15, IdleTextColor, ETextJustify::Left);
		None->SetAutoWrapText(true);
		IslandList->AddChildToVerticalBox(None);
	}
}

void ULBAIslandMenu::BuildPointGroupList()
{
	const TArray<FLBAPointOfInterest>& Points = LBAPointsOfInterest::Get();
	for (int32 Index = 0; Index < PointGroups.Num(); ++Index)
	{
		const int32 Count = Points.FilterByPredicate([&](const FLBAPointOfInterest& Point) { return Point.Group.EqualTo(PointGroups[Index]); }).Num();
		FLBAMenuButton& Button = AddButton(IslandButtons, ELBAMenuAction::PointGroup, Index, PointGroups[Index], PointsIcon, true, 0.f,
			FText::Format(LOCTEXT("PlaceCount", "{0} {0}|plural(one=place,other=places)"), FText::AsNumber(Count)));
		Button.bCurrent = Points.IsValidIndex(CurrentPoint) && Points[CurrentPoint].Group.EqualTo(PointGroups[Index]);
		IslandList->AddChildToVerticalBox(Button.Button)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}
}

void ULBAIslandMenu::BuildTestLevelList()
{
	// The feature map first, apart, then the other test levels and sandboxes.
	bool bCaption = false;
	for (int32 Index = 0; Index < TestLevels.Num(); ++Index)
	{
		const FLBATestLevelInfo& Level = TestLevels[Index];
		if (!Level.bFeatureLevel && !bCaption && Index > 0)
		{
			UTextBlock* Caption = MakeText(WidgetTree, LOCTEXT("Sandboxes", "TEST LEVELS AND SANDBOXES"), 12, CaptionColor, ETextJustify::Left, true);
			IslandList->AddChildToVerticalBox(Caption)->SetPadding(FMargin(8.f, 10.f, 0.f, 6.f));
			bCaption = true;
		}
		const FText Subtitle = Level.bFeatureLevel
			? FText::Format(LOCTEXT("FeatureSubtitle", "Every feature, {0} stations, gateways to the game"), FText::AsNumber(Level.Stations.Num()))
			: FText::GetEmpty();
		FLBAMenuButton& Button = AddButton(IslandButtons, ELBAMenuAction::TestLevel, Index, Level.DisplayName,
			Level.bFeatureLevel ? TestsIcon : SectionIcon, true, 0.f, Subtitle);
		Button.bCurrent = Index == CurrentTestLevel;
		Button.bFeature = Level.bFeatureLevel;
		Button.Button->SetToolTipText(FText::FromString(Level.Level.ToSoftObjectPath().GetLongPackageName()));
		IslandList->AddChildToVerticalBox(Button.Button)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}
	if (TestLevels.IsEmpty())
	{
		UTextBlock* None = MakeText(WidgetTree, LOCTEXT("NoTests", "No test level yet (import the Tests layer)."), 15, IdleTextColor, ETextJustify::Left);
		None->SetAutoWrapText(true);
		IslandList->AddChildToVerticalBox(None);
	}
}

void ULBAIslandMenu::BuildSceneTree()
{
	SceneTree->ClearChildren();
	SceneButtons.Reset();
	if (bShowPoints)
	{
		BuildPointList();
	}
	else if (bShowTests)
	{
		BuildStationList();
	}
	else
	{
		for (FLBAMenuButton& Button : IslandButtons)
		{
			Button.bActive = Button.Handler->Value == SelectedIsland;
		}
		if (Islands.IsValidIndex(SelectedIsland))
		{
			const FLBAIslandInfo& Island = Islands[SelectedIsland];

			// The island level's own start first, then its areas with their interiors (lba2remake's tree, see
			// ULBASceneAsset::MenuParent). Scenes whose parent is not listed here hang from the island.
			TMap<int32, TArray<const FLBASceneMenuEntry*>> Children;
			if (Island.SceneIsland != INDEX_NONE)
			{
				TSet<int32> Listed;
				for (const FLBASceneMenuEntry& Scene : Scenes)
				{
					if (Scene.Island == Island.SceneIsland)
					{
						Listed.Add(Scene.SceneIndex);
					}
				}
				for (const FLBASceneMenuEntry& Scene : Scenes)
				{
					if (Scene.Island == Island.SceneIsland)
					{
						Children.FindOrAdd(Listed.Contains(Scene.Parent) ? Scene.Parent : INDEX_NONE).Add(&Scene);
					}
				}
			}
			const bool bCurrentSceneListed = Scenes.ContainsByPredicate([this, &Island](const FLBASceneMenuEntry& Scene)
			{
				return Scene.SceneIndex == CurrentScene && Scene.Island == Island.SceneIsland;
			});
			FLBAMenuButton& Start = AddButton(SceneButtons, ELBAMenuAction::IslandStart, SelectedIsland, Island.DisplayName, IslandIcon, false);
			Start.bCurrent = SelectedIsland == CurrentIsland && !bCurrentSceneListed;
			Cast<UScrollBoxSlot>(SceneTree->AddChild(Start.Button))->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
			AddScenes(Children, INDEX_NONE, 0);
		}
	}
	UpdateNavigation();
}

void ULBAIslandMenu::BuildPointList()
{
	for (FLBAMenuButton& Button : IslandButtons)
	{
		Button.bActive = Button.Handler->Value == SelectedPointGroup;
	}
	if (!PointGroups.IsValidIndex(SelectedPointGroup))
	{
		return;
	}
	const TArray<FLBAPointOfInterest>& Points = LBAPointsOfInterest::Get();
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FLBAPointOfInterest& Point = Points[Index];
		if (!Point.Group.EqualTo(PointGroups[SelectedPointGroup]))
		{
			continue;
		}
		FLBAMenuButton& Button = AddButton(SceneButtons, ELBAMenuAction::Point, Index, Point.Name, Point.Scene != INDEX_NONE ? BuildingIcon : SectionIcon,
			false, 0.f, Point.Description);
		Button.bCurrent = Index == CurrentPoint;
		Button.Button->SetToolTipText(FText::Format(LOCTEXT("PointTip", "lba.POI {0}"), FText::FromName(Point.Id)));
		Cast<UScrollBoxSlot>(SceneTree->AddChild(Button.Button))->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}
}

void ULBAIslandMenu::BuildStationList()
{
	for (FLBAMenuButton& Button : IslandButtons)
	{
		Button.bActive = Button.Handler->Value == SelectedTestLevel;
	}
	if (!TestLevels.IsValidIndex(SelectedTestLevel))
	{
		return;
	}
	// The level's start, then its stations.
	const FLBATestLevelInfo& Level = TestLevels[SelectedTestLevel];
	const bool bHere = SelectedTestLevel == CurrentTestLevel;
	FLBAMenuButton& Start = AddButton(SceneButtons, ELBAMenuAction::TestLevelStart, SelectedTestLevel, Level.DisplayName,
		Level.bFeatureLevel ? TestsIcon : SectionIcon, false);
	Start.bCurrent = bHere && CurrentStation.IsNone();
	Cast<UScrollBoxSlot>(SceneTree->AddChild(Start.Button))->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
	for (int32 Index = 0; Index < Level.Stations.Num(); ++Index)
	{
		const FLBATestStationInfo& Station = Level.Stations[Index];
		FLBAMenuButton& Button = AddButton(SceneButtons, ELBAMenuAction::Station, Index, Station.DisplayName, SectionIcon, false, IndentPerLevel, Station.Description);
		Button.bCurrent = bHere && Station.Tag == CurrentStation;
		Cast<UScrollBoxSlot>(SceneTree->AddChild(Button.Button))->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
	}
	if (Level.Stations.IsEmpty())
	{
		UTextBlock* None = MakeText(WidgetTree, LOCTEXT("NoStations", "No station: place LBA Test Station actors to list places here."), 15, IdleTextColor, ETextJustify::Left);
		Cast<UScrollBoxSlot>(SceneTree->AddChild(None))->SetPadding(FMargin(12.f, 8.f));
	}
}

void ULBAIslandMenu::AddScenes(const TMap<int32, TArray<const FLBASceneMenuEntry*>>& Children, int32 Parent, int32 Depth)
{
	const TArray<const FLBASceneMenuEntry*>* Entries = Children.Find(Parent);
	if (!Entries)
	{
		return;
	}
	for (const FLBASceneMenuEntry* Scene : *Entries)
	{
		FLBAMenuButton& Button = AddButton(SceneButtons, ELBAMenuAction::Scene, Scene->SceneIndex, FText::FromString(Scene->Name),
			Scene->bExterior ? SectionIcon : BuildingIcon, false, Depth * IndentPerLevel);
		Button.bCurrent = Scene->SceneIndex == CurrentScene;
		Button.Button->SetToolTipText(FText::Format(LOCTEXT("SceneTip", "Scene {0}"), FText::AsNumber(Scene->SceneIndex)));
		Cast<UScrollBoxSlot>(SceneTree->AddChild(Button.Button))->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
		AddScenes(Children, Scene->SceneIndex, Depth + 1);
	}
}

void ULBAIslandMenu::UpdateNavigation()
{
	UButton* Left = nullptr;
	for (const FLBAMenuButton& Button : IslandButtons)
	{
		if (!Left && Button.bActive)
		{
			Left = Button.Button;
		}
	}
	if (!Left && !IslandButtons.IsEmpty())
	{
		Left = IslandButtons[0].Button;
	}
	UButton* Right = nullptr;
	for (const FLBAMenuButton& Button : SceneButtons)
	{
		if (!Right && Button.bCurrent)
		{
			Right = Button.Button;
		}
	}
	if (!Right && !SceneButtons.IsEmpty())
	{
		Right = SceneButtons[0].Button;
	}
	for (const FLBAMenuButton& Button : SceneButtons)
	{
		if (Left)
		{
			Button.Button->SetNavigationRuleExplicit(EUINavigation::Left, Left);
		}
	}
	for (const FLBAMenuButton& Button : IslandButtons)
	{
		if (Right)
		{
			Button.Button->SetNavigationRuleExplicit(EUINavigation::Right, Right);
		}
	}
}

FLBAMenuButton& ULBAIslandMenu::AddButton(TArray<FLBAMenuButton>& List, ELBAMenuAction Action, int32 Value, const FText& Label,
	const TSoftObjectPtr<UTexture2D>& Icon, bool bTab, float Indent, const FText& Subtitle)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	Button->SetStyle(BarButtonStyle());
	Button->SetBackgroundColor(ButtonColor);

	const bool bTopTab = Action == ELBAMenuAction::Planet || Action == ELBAMenuAction::TestsTab || Action == ELBAMenuAction::PointsTab;
	const bool bFooter = Action == ELBAMenuAction::Quit || Action == ELBAMenuAction::Play;
	UTextBlock* Text = MakeText(WidgetTree, Label, bTopTab ? 17 : (bTab ? 18 : (bFooter ? 15 : 17)), FLinearColor::White, ETextJustify::Left, bTopTab || bTab);
	UTexture2D* Texture = Icon.LoadSynchronous();
	UImage* Image = nullptr;
	if (Texture)
	{
		Image = WidgetTree->ConstructWidget<UImage>();
		Image->SetBrushFromTexture(Texture);
		Image->SetDesiredSizeOverride(FVector2D(bTopTab ? TabIconSize : IconSize));
	}

	// The label, with its subtitle under it.
	UWidget* Name = Text;
	UTextBlock* Under = nullptr;
	if (!Subtitle.IsEmpty())
	{
		UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>();
		Lines->AddChildToVerticalBox(Text);
		Under = MakeText(WidgetTree, Subtitle, 14, CaptionColor, ETextJustify::Left);
		Under->SetAutoWrapText(true);
		Lines->AddChildToVerticalBox(Under)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
		Name = Lines;
	}

	// Marker bar (where the player is), icon, then the name, on one line.
	UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>();
	UBorder* Marker = WidgetTree->ConstructWidget<UBorder>();
	Marker->SetBrushColor(FLinearColor::Transparent);
	USizeBox* MarkerBox = WidgetTree->ConstructWidget<USizeBox>();
	MarkerBox->SetWidthOverride(MarkerWidth);
	MarkerBox->SetContent(Marker);
	UHorizontalBoxSlot* MarkerSlot = Box->AddChildToHorizontalBox(MarkerBox);
	MarkerSlot->SetVerticalAlignment(VAlign_Fill);
	MarkerSlot->SetPadding(FMargin(Indent, 0.f, 8.f, 0.f));
	if (Image)
	{
		UHorizontalBoxSlot* ImageSlot = Box->AddChildToHorizontalBox(Image);
		ImageSlot->SetVerticalAlignment(Subtitle.IsEmpty() ? VAlign_Center : VAlign_Top);
		ImageSlot->SetPadding(FMargin(0.f, Subtitle.IsEmpty() ? 0.f : 1.f, 10.f, 0.f));
	}
	UHorizontalBoxSlot* NameSlot = Box->AddChildToHorizontalBox(Name);
	NameSlot->SetVerticalAlignment(VAlign_Center);
	NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Button->SetContent(Box);
	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Box->Slot))
	{
		const float Vertical = bTopTab ? 8.f : (bTab ? 9.f : 6.f);
		ContentSlot->SetPadding(FMargin(0.f, Vertical, 12.f, Vertical));
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Fill);
	}

	ULBAIslandMenuButtonHandler* Handler = NewObject<ULBAIslandMenuButtonHandler>(this);
	Handler->Menu = this;
	Handler->Action = Action;
	Handler->Value = Value;
	Button->OnClicked.AddDynamic(Handler, &ULBAIslandMenuButtonHandler::HandleClicked);

	FLBAMenuButton& Entry = List.AddDefaulted_GetRef();
	Entry.Button = Button;
	Entry.Label = Text;
	Entry.Subtitle = Under;
	Entry.Marker = Marker;
	Entry.Handler = Handler;
	Entry.bTab = bTab;
	return Entry;
}

void ULBAIslandMenu::ForEachButton(TFunctionRef<void(FLBAMenuButton&)> Visit)
{
	for (TArray<FLBAMenuButton>* List : { &PlanetButtons, &IslandButtons, &SceneButtons, &FooterButtons })
	{
		for (FLBAMenuButton& Button : *List)
		{
			Visit(Button);
		}
	}
}

void ULBAIslandMenu::SelectPlanet(ELBAPlanet Planet)
{
	bShowTests = false;
	bShowPoints = false;
	SelectedPlanet = Planet;
	// The player's island when it is on this planet, else the planet's first.
	SelectedIsland = Islands.IsValidIndex(HomeIsland) && Islands[HomeIsland].Planet == Planet ? HomeIsland
		: Islands.IndexOfByPredicate([Planet](const FLBAIslandInfo& Island) { return Island.Planet == Planet; });
	BuildIslandList();
	BuildSceneTree();
}

void ULBAIslandMenu::SelectIsland(int32 Island)
{
	SelectedIsland = Island;
	BuildSceneTree();
}

void ULBAIslandMenu::ShowTests()
{
	bShowTests = true;
	bShowPoints = false;
	if (!TestLevels.IsValidIndex(SelectedTestLevel))
	{
		SelectedTestLevel = TestLevels.IsEmpty() ? INDEX_NONE : 0;
	}
	BuildIslandList();
	BuildSceneTree();
}

void ULBAIslandMenu::SelectTestLevel(int32 Level)
{
	SelectedTestLevel = Level;
	BuildSceneTree();
}

void ULBAIslandMenu::ShowPoints()
{
	bShowPoints = true;
	bShowTests = false;
	BuildIslandList();
	BuildSceneTree();
}

void ULBAIslandMenu::SelectPointGroup(int32 Group)
{
	SelectedPointGroup = Group;
	BuildSceneTree();
}

void ULBAIslandMenu::CycleTab(int32 Step)
{
	// Tabs in order: the showcase places, the planets, Tests.
	const bool bHasPoints = !LBAPointsOfInterest::Get().IsEmpty();
	const int32 First = bHasPoints ? 1 : 0;
	const int32 NumTabs = First + Planets.Num() + (TestLevels.IsEmpty() ? 0 : 1);
	if (NumTabs == 0)
	{
		return;
	}
	const int32 Current = bShowPoints ? 0 : (bShowTests ? First + Planets.Num() : First + Planets.IndexOfByKey(SelectedPlanet));
	const int32 Next = (Current + Step + NumTabs) % NumTabs;
	if (bHasPoints && Next == 0)
	{
		ShowPoints();
	}
	else if (Next == First + Planets.Num())
	{
		ShowTests();
	}
	else
	{
		SelectPlanet(Planets[Next - First]);
	}
}

void ULBAIslandMenu::FocusDefaultButton()
{
	UButton* Target = FindDefaultButton();
	FocusButton(Target);
	ScrollTo(Target);
}

void ULBAIslandMenu::ScrollToCurrent()
{
	ScrollTo(FindDefaultButton());
}

void ULBAIslandMenu::ScrollTo(UButton* Target)
{
	if (Target && Target->GetParent() == SceneTree.Get())
	{
		SceneTree->ScrollWidgetIntoView(Target, false, EDescendantScrollDestination::Center);
	}
}

UButton* ULBAIslandMenu::FindDefaultButton()
{
	UButton* Target = nullptr;
	ForEachButton([&Target](FLBAMenuButton& Button)
	{
		// The scene, station or place the player is at (or the level start), else their island or test level.
		if (!Target && Button.bCurrent && IsListEntry(Button.Handler->Action))
		{
			Target = Button.Button;
		}
	});
	// The showcase places open on their first place.
	if (!Target && bShowPoints && !SceneButtons.IsEmpty())
	{
		Target = SceneButtons[0].Button;
	}
	for (const FLBAMenuButton& Button : IslandButtons)
	{
		if (!Target && Button.bActive)
		{
			Target = Button.Button;
		}
	}
	if (!Target)
	{
		Target = !IslandButtons.IsEmpty() ? IslandButtons[0].Button.Get() : (!FooterButtons.IsEmpty() ? FooterButtons[0].Button.Get() : nullptr);
	}
	return Target;
}

void ULBAIslandMenu::FocusButton(UButton* Button)
{
	if (Button)
	{
		if (bEditorMode)
		{
			Button->SetKeyboardFocus();
		}
		else
		{
			Button->SetUserFocus(GetOwningPlayer());
		}
		LastFocused = Button;
	}
}

bool ULBAIslandMenu::HasMenuFocus(const UButton* Button) const
{
	return bEditorMode ? Button->HasKeyboardFocus() : Button->HasUserFocus(GetOwningPlayer());
}

void ULBAIslandMenu::UpdateFooterHint()
{
	const ELBAControlsLayout Layout = ULBAControlsSubsystem::GetLayout(this);
	if (!FooterHint || FooterHintLayout == int32(Layout))
	{
		return;
	}
	FooterHintLayout = int32(Layout);
	FText HintText;
	if (bEditorMode)
	{
		HintText = LOCTEXT("EditorHint", "Opens the level in the editor and frames the place.    Q / E: change tab");
	}
	else if (Layout == ELBAControlsLayout::KeyboardMouse)
	{
		HintText = LOCTEXT("KeyboardHint", "Enter: go    M / Esc: close    Q / E: change tab");
	}
	else if (Layout == ELBAControlsLayout::Gamepad)
	{
		HintText = LOCTEXT("GamepadHint", "A: go    Menu / B: close    LB / RB: change tab");
	}
	else
	{
		HintText = LOCTEXT("Hint", "Enter / A: go    M / Start: close    Q / E, LB / RB: change tab");
	}
	FooterHint->SetText(HintText);
}

void ULBAIslandMenu::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	UpdateFooterHint();

	// One selection: pointing at a button focuses it, so mouse and keyboard/gamepad never highlight two buttons.
	// Only when the mouse moves: a cursor resting where the menu opens (or where a list is rebuilt) takes nothing.
	// The editor's selector only takes focus once the user works in it: elsewhere hovering just highlights.
	const FVector2D CursorAt = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetCursorPos() : FVector2D::ZeroVector;
	const bool bMouseMoved = LastCursor.IsSet() && !LastCursor->Equals(CursorAt, 0.5);
	LastCursor = CursorAt;
	const bool bMayTakeFocus = !bEditorMode || HasFocusedDescendants();
	bool bAnyFocused = false;
	ForEachButton([this, bMouseMoved, bMayTakeFocus, &bAnyFocused](FLBAMenuButton& Entry)
	{
		UButton* Button = Entry.Button;
		if (bMouseMoved && bMayTakeFocus && Button->IsHovered() && !HasMenuFocus(Button))
		{
			FocusButton(Button);
		}
		const bool bFocused = HasMenuFocus(Button);
		if (bFocused)
		{
			LastFocused = Button;
			bAnyFocused = true;
		}
		// The choice is lit in amber with dark text, like the original's menus; the entry shown is a lighter bar; where the
		// player is gets a gold marker bar (and gold text when not lit).
		const bool bHighlighted = bFocused || (!bMayTakeFocus && Button->IsHovered());
		Button->SetBackgroundColor(bHighlighted ? FocusColor : (Entry.bActive ? ActiveColor : ButtonColor));
		const bool bBright = !Entry.bTab || Entry.bActive;
		const FLinearColor TextColor = bHighlighted ? FocusTextColor
			: (Entry.bCurrent ? CurrentColor : (Entry.bFeature ? FeatureColor : (bBright ? FLinearColor::White : IdleTextColor)));
		Entry.Label->SetColorAndOpacity(FSlateColor(TextColor));
		if (Entry.Subtitle)
		{
			Entry.Subtitle->SetColorAndOpacity(FSlateColor(bHighlighted ? FocusTextColor : CaptionColor));
		}
		if (Entry.Marker)
		{
			Entry.Marker->SetBrushColor(Entry.bCurrent ? (bHighlighted ? FocusTextColor : CurrentColor) : FLinearColor::Transparent);
		}
	});
	// Activating the window or clicking the backdrop hands focus to the game viewport, which would leave the
	// keyboard and gamepad with nothing to navigate: take it back.
	if (!bAnyFocused && !bEditorMode)
	{
		const ULocalPlayer* Player = GetOwningLocalPlayer();
		const TSharedPtr<const FSlateUser> SlateUser = Player ? Player->GetSlateUser() : nullptr;
		const TSharedPtr<SWidget> Focused = SlateUser ? SlateUser->GetFocusedWidget() : nullptr;
		const UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
		// Anything else (the console...) keeps its focus.
		if (!Focused || (Viewport && Focused.Get() == static_cast<SWidget*>(Viewport->GetGameViewportWidget().Get())))
		{
			if (LastFocused.IsValid() && LastFocused->GetParent())
			{
				FocusButton(LastFocused.Get());
			}
			else
			{
				FocusDefaultButton();
			}
		}
	}
}

FReply ULBAIslandMenu::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// Clicks on the backdrop stay in the menu instead of reaching the game viewport.
	return FReply::Handled();
}

FReply ULBAIslandMenu::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (!bEditorMode && (Key == EKeys::Escape || Key == EKeys::M || Key == EKeys::Gamepad_Special_Right || Key == EKeys::Gamepad_FaceButton_Right))
	{
		Close();
		return FReply::Handled();
	}
	const int32 Step = Key == EKeys::Q || Key == EKeys::Gamepad_LeftShoulder ? -1 : (Key == EKeys::E || Key == EKeys::Gamepad_RightShoulder ? 1 : 0);
	if (Step != 0)
	{
		// Next tab, focus on its island or level shown.
		CycleTab(Step);
		FocusDefaultButton();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void ULBAIslandMenu::Close()
{
	if (bEditorMode)
	{
		return;
	}
	if (ALBAPlayerController* PC = GetOwningPlayer<ALBAPlayerController>())
	{
		PC->CloseIslandMenu();
	}
	else
	{
		RemoveFromParent();
	}
}

void ULBAIslandMenu::GoTo(const FLBATravelDestination& Destination)
{
	// Give the game its input back before the level changes (the menu goes away with the world). Already on that
	// island: just resume.
	Close();
	if (Destination.Kind == ELBATravelKind::Island && Islands.IsValidIndex(CurrentIsland) && Islands[CurrentIsland].Id == Destination.Island)
	{
		return;
	}
	if (ULBAIslandTravelSubsystem* Travel = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULBAIslandTravelSubsystem>() : nullptr)
	{
		Travel->TravelTo(Destination);
	}
}

void ULBAIslandMenu::OnButtonClicked(ELBAMenuAction Action, int32 Value)
{
	switch (Action)
	{
	case ELBAMenuAction::Quit:
		UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
		return;

	case ELBAMenuAction::Play:
		Play();
		return;

	case ELBAMenuAction::PointsTab:
		ShowPoints();
		return;

	case ELBAMenuAction::PointGroup:
		SelectPointGroup(Value);
		return;

	case ELBAMenuAction::Point:
		if (LBAPointsOfInterest::Get().IsValidIndex(Value))
		{
			GoTo(FLBATravelDestination::ToPointOfInterest(LBAPointsOfInterest::Get()[Value].Id));
		}
		return;

	case ELBAMenuAction::Planet:
		SelectPlanet(ELBAPlanet(Value));
		return;

	case ELBAMenuAction::Island:
		SelectIsland(Value);
		return;

	case ELBAMenuAction::TestsTab:
		ShowTests();
		return;

	case ELBAMenuAction::TestLevel:
		SelectTestLevel(Value);
		return;

	case ELBAMenuAction::IslandStart:
		if (Islands.IsValidIndex(Value))
		{
			GoTo(FLBATravelDestination::ToIsland(Islands[Value].Id));
		}
		return;

	case ELBAMenuAction::Scene:
		// On the island variant shown (storm or sunny Citadel...), also for an interior's way back out.
		GoTo(FLBATravelDestination::ToScene(Value, Islands.IsValidIndex(SelectedIsland) ? Islands[SelectedIsland].Id : NAME_None));
		return;

	case ELBAMenuAction::TestLevelStart:
		if (TestLevels.IsValidIndex(Value))
		{
			GoTo(FLBATravelDestination::ToTestLevel(TestLevels[Value].Level));
		}
		return;

	case ELBAMenuAction::Station:
		if (TestLevels.IsValidIndex(SelectedTestLevel) && TestLevels[SelectedTestLevel].Stations.IsValidIndex(Value))
		{
			GoTo(FLBATravelDestination::ToTestLevel(TestLevels[SelectedTestLevel].Level, TestLevels[SelectedTestLevel].Stations[Value].Tag));
		}
		return;
	}
}

#undef LOCTEXT_NAMESPACE
