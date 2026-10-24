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
#include "Widgets/SViewport.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LBAPlayerController.h"

#define LOCTEXT_NAMESPACE "LBAIslandMenu"

namespace
{
	constexpr float PlanetIconSize = 48.f;
	constexpr float IconSize = 16.f;
	constexpr float IndentPerLevel = 20.f;

	UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color, ETextJustify::Type Justify = ETextJustify::Center)
	{
		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>();
		Block->SetText(Text);
		FSlateFontInfo Font = Block->GetFont();
		Font.Size = Size;
		Block->SetFont(Font);
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetJustification(Justify);
		return Block;
	}

	FSlateBrush RoundedBrush(const FLinearColor& Color, float Radius, const FLinearColor& Outline = FLinearColor::Transparent, float OutlineWidth = 0.f)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings = FSlateBrushOutlineSettings(Radius, FSlateColor(Outline), OutlineWidth);
		return Brush;
	}

	/** Flat button: its background color (set every tick) is the whole look. */
	const FButtonStyle& FlatButtonStyle()
	{
		static const FButtonStyle Style = []
		{
			const FSlateBrush Brush = RoundedBrush(FLinearColor::White, 4.f);
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
	// lba2remake's location icons (Tools/export_locations.py, import_location_icons.py).
	PlanetIcons.Add(ELBAPlanet::Twinsun, PluginIcon(TEXT("T_Location_Twinsun")));
	PlanetIcons.Add(ELBAPlanet::EmeraldMoon, PluginIcon(TEXT("T_Location_Moon")));
	PlanetIcons.Add(ELBAPlanet::ZeelishSurface, PluginIcon(TEXT("T_Location_Zeelish")));
	PlanetIcons.Add(ELBAPlanet::ZeelishUndergas, PluginIcon(TEXT("T_Location_Undergas")));
	IslandIcon = PluginIcon(TEXT("T_Location_Island"));
	SectionIcon = PluginIcon(TEXT("T_Location_Section"));
	BuildingIcon = PluginIcon(TEXT("T_Location_Building"));
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

void ULBAIslandMenu::BuildTree()
{
	SetIsFocusable(true);

	// Where the player is: an island level, and the scene under them (an interior's tells its island).
	UGameInstance* GameInstance = GetGameInstance();
	const ULBAIslandTravelSubsystem* IslandTravel = GameInstance ? GameInstance->GetSubsystem<ULBAIslandTravelSubsystem>() : nullptr;
	const ULBASceneTravelSubsystem* SceneTravel = GameInstance ? GameInstance->GetSubsystem<ULBASceneTravelSubsystem>() : nullptr;
	if (IslandTravel)
	{
		Islands = IslandTravel->GetIslands();
	}
	Scenes = ULBASceneTravelSubsystem::FindMenuScenes();
	const FName CurrentId = IslandTravel ? IslandTravel->GetCurrentIslandId() : NAME_None;
	CurrentIsland = Islands.IndexOfByPredicate([CurrentId](const FLBAIslandInfo& Island) { return Island.Id == CurrentId; });
	CurrentScene = SceneTravel ? SceneTravel->GetCurrentScene() : INDEX_NONE;
	HomeIsland = CurrentIsland;
	if (HomeIsland == INDEX_NONE)
	{
		const FLBASceneMenuEntry* Scene = Scenes.FindByPredicate([this](const FLBASceneMenuEntry& Entry) { return Entry.SceneIndex == CurrentScene; });
		if (Scene)
		{
			// The island variant the player came from (storm or sunny Citadel...), else the first.
			const FString LastLevel = SceneTravel->GetLastIslandLevel(Scene->Island);
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

	Planets.Reset();
	for (const FLBAIslandInfo& Island : Islands)
	{
		Planets.AddUnique(Island.Planet);
	}
	Planets.Sort();
	SelectedIsland = Islands.IsValidIndex(HomeIsland) ? HomeIsland : (Islands.IsEmpty() ? INDEX_NONE : 0);
	SelectedPlanet = Islands.IsValidIndex(SelectedIsland) ? Islands[SelectedIsland].Planet : ELBAPlanet::Twinsun;

	// Full-screen dim, with the panel over most of it.
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Dim"));
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.55f));
	Dim->SetPadding(FMargin(48.f, 32.f));
	WidgetTree->RootWidget = Dim;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Panel"));
	Panel->SetBrush(RoundedBrush(PanelColor, 12.f, BorderColor, 2.f));
	Panel->SetPadding(FMargin(2.f));
	Dim->SetContent(Panel);

	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Content"));
	Panel->SetContent(Content);

	// Planets.
	UHorizontalBox* PlanetBar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Planets"));
	UVerticalBoxSlot* PlanetBarSlot = Content->AddChildToVerticalBox(PlanetBar);
	PlanetBarSlot->SetPadding(FMargin(8.f, 8.f, 8.f, 6.f));
	PlanetButtons.Reset();
	for (const ELBAPlanet Planet : Planets)
	{
		FLBAMenuButton& Button = AddButton(PlanetButtons, ELBAMenuAction::Planet, int32(Planet), ULBAIslandTravelSubsystem::GetPlanetName(Planet),
			PlanetIcons.FindRef(Planet), true);
		Button.bCurrent = Islands.IsValidIndex(HomeIsland) && Islands[HomeIsland].Planet == Planet;
		UHorizontalBoxSlot* ButtonSlot = PlanetBar->AddChildToHorizontalBox(Button.Button);
		ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ButtonSlot->SetPadding(FMargin(4.f, 0.f));
	}

	UBorder* Line = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Line"));
	Line->SetBrushColor(BorderColor);
	Line->SetPadding(FMargin(0.f, 1.f));
	Content->AddChildToVerticalBox(Line);

	// Islands on the left, the selected island's scenes on the right.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Body"));
	Content->AddChildToVerticalBox(Body)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	USizeBox* IslandColumn = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("IslandColumn"));
	IslandColumn->SetWidthOverride(340.f);
	Body->AddChildToHorizontalBox(IslandColumn);
	UScrollBox* IslandScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("IslandScroll"));
	IslandScroll->SetScrollWhenFocusChanges(EScrollWhenFocusChanges::AnimatedScroll);
	IslandColumn->SetContent(IslandScroll);
	IslandList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Islands"));
	Cast<UScrollBoxSlot>(IslandScroll->AddChild(IslandList))->SetPadding(FMargin(8.f));

	UBorder* TreePanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("TreePanel"));
	TreePanel->SetBrushColor(TreeColor);
	TreePanel->SetPadding(FMargin(12.f, 8.f));
	Body->AddChildToHorizontalBox(TreePanel)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	SceneTree = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scenes"));
	SceneTree->SetScrollWhenFocusChanges(EScrollWhenFocusChanges::AnimatedScroll);
	TreePanel->SetContent(SceneTree);

	// Footer: controls, Quit.
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Footer"));
	Content->AddChildToVerticalBox(Footer)->SetPadding(FMargin(16.f, 6.f, 8.f, 6.f));
	UTextBlock* Hint = MakeText(WidgetTree, LOCTEXT("Hint", "M / Start: close    Q / E, LB / RB: change planet"), 12, FLinearColor(0.6f, 0.6f, 0.6f), ETextJustify::Left);
	UHorizontalBoxSlot* HintSlot = Footer->AddChildToHorizontalBox(Hint);
	HintSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	HintSlot->SetVerticalAlignment(VAlign_Center);
	QuitButtons.Reset();
	Footer->AddChildToHorizontalBox(AddButton(QuitButtons, ELBAMenuAction::Quit, INDEX_NONE, LOCTEXT("Quit", "Quit"), nullptr, false).Button);

	BuildIslandList();
	BuildSceneTree();
}

void ULBAIslandMenu::BuildIslandList()
{
	IslandList->ClearChildren();
	IslandButtons.Reset();
	for (FLBAMenuButton& Button : PlanetButtons)
	{
		Button.bActive = Button.Handler->Value == int32(SelectedPlanet);
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
		IslandList->AddChildToVerticalBox(MakeText(WidgetTree, LOCTEXT("NoIslands", "No island imported yet (Tools > LBA2 Importer)."), 14, FLinearColor::Gray, ETextJustify::Left));
	}
}

void ULBAIslandMenu::BuildSceneTree()
{
	SceneTree->ClearChildren();
	SceneButtons.Reset();
	for (FLBAMenuButton& Button : IslandButtons)
	{
		Button.bActive = Button.Handler->Value == SelectedIsland;
	}
	if (!Islands.IsValidIndex(SelectedIsland))
	{
		return;
	}
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
	Cast<UScrollBoxSlot>(SceneTree->AddChild(Start.Button))->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
	AddScenes(Children, INDEX_NONE, 0);
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
		Cast<UScrollBoxSlot>(SceneTree->AddChild(Button.Button))->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
		AddScenes(Children, Scene->SceneIndex, Depth + 1);
	}
}

FLBAMenuButton& ULBAIslandMenu::AddButton(TArray<FLBAMenuButton>& List, ELBAMenuAction Action, int32 Value, const FText& Label,
	const TSoftObjectPtr<UTexture2D>& Icon, bool bTab, float Indent)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	Button->SetStyle(FlatButtonStyle());
	Button->SetBackgroundColor(FLinearColor::Transparent);

	const bool bPlanet = Action == ELBAMenuAction::Planet;
	UTextBlock* Text = MakeText(WidgetTree, Label, bPlanet || bTab ? 16 : 14, FLinearColor::White, bPlanet ? ETextJustify::Center : ETextJustify::Left);
	UTexture2D* Texture = Icon.LoadSynchronous();
	UImage* Image = nullptr;
	if (Texture)
	{
		Image = WidgetTree->ConstructWidget<UImage>();
		Image->SetBrushFromTexture(Texture);
		Image->SetDesiredSizeOverride(FVector2D(bPlanet ? PlanetIconSize : IconSize));
	}

	// Planets: icon over the name, like the remake's tabs. Others: icon, then the name.
	UWidget* Content = Text;
	if (bPlanet)
	{
		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>();
		if (Image)
		{
			Box->AddChildToVerticalBox(Image)->SetHorizontalAlignment(HAlign_Center);
		}
		UVerticalBoxSlot* TextSlot = Box->AddChildToVerticalBox(Text);
		TextSlot->SetHorizontalAlignment(HAlign_Center);
		TextSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
		Content = Box;
	}
	else if (Image)
	{
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>();
		UHorizontalBoxSlot* ImageSlot = Box->AddChildToHorizontalBox(Image);
		ImageSlot->SetVerticalAlignment(VAlign_Center);
		ImageSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
		Box->AddChildToHorizontalBox(Text)->SetVerticalAlignment(VAlign_Center);
		Content = Box;
	}
	Button->SetContent(Content);
	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Content->Slot))
	{
		ContentSlot->SetPadding(bPlanet ? FMargin(8.f, 8.f) : FMargin(12.f + Indent, bTab ? 10.f : 5.f, 12.f, bTab ? 10.f : 5.f));
		ContentSlot->SetHorizontalAlignment(bPlanet ? HAlign_Center : HAlign_Left);
	}

	ULBAIslandMenuButtonHandler* Handler = NewObject<ULBAIslandMenuButtonHandler>(this);
	Handler->Menu = this;
	Handler->Action = Action;
	Handler->Value = Value;
	Button->OnClicked.AddDynamic(Handler, &ULBAIslandMenuButtonHandler::HandleClicked);

	FLBAMenuButton& Entry = List.AddDefaulted_GetRef();
	Entry.Button = Button;
	Entry.Label = Text;
	Entry.Handler = Handler;
	Entry.bTab = bTab;
	return Entry;
}

void ULBAIslandMenu::ForEachButton(TFunctionRef<void(FLBAMenuButton&)> Visit)
{
	for (TArray<FLBAMenuButton>* List : { &PlanetButtons, &IslandButtons, &SceneButtons, &QuitButtons })
	{
		for (FLBAMenuButton& Button : *List)
		{
			Visit(Button);
		}
	}
}

void ULBAIslandMenu::SelectPlanet(ELBAPlanet Planet)
{
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

void ULBAIslandMenu::FocusDefaultButton()
{
	UButton* Target = nullptr;
	ForEachButton([&Target](FLBAMenuButton& Button)
	{
		// The scene the player is in (or the island start), else their island.
		if (!Target && Button.bCurrent && (Button.Handler->Action == ELBAMenuAction::Scene || Button.Handler->Action == ELBAMenuAction::IslandStart))
		{
			Target = Button.Button;
		}
	});
	for (const FLBAMenuButton& Button : IslandButtons)
	{
		if (!Target && Button.bActive)
		{
			Target = Button.Button;
		}
	}
	if (!Target)
	{
		Target = !IslandButtons.IsEmpty() ? IslandButtons[0].Button.Get() : (!QuitButtons.IsEmpty() ? QuitButtons[0].Button.Get() : nullptr);
	}
	FocusButton(Target);
	if (Target && Target->GetParent() == SceneTree.Get())
	{
		SceneTree->ScrollWidgetIntoView(Target, false, EDescendantScrollDestination::Center);
	}
}

void ULBAIslandMenu::FocusButton(UButton* Button)
{
	if (Button)
	{
		Button->SetUserFocus(GetOwningPlayer());
		LastFocused = Button;
	}
}

void ULBAIslandMenu::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// One selection: pointing at a button focuses it, so mouse and keyboard/gamepad never highlight two buttons.
	// Only when the mouse moves: a cursor resting where the menu opens (or where a list is rebuilt) takes nothing.
	const FVector2D CursorAt = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetCursorPos() : FVector2D::ZeroVector;
	const bool bMouseMoved = LastCursor.IsSet() && !LastCursor->Equals(CursorAt, 0.5);
	LastCursor = CursorAt;
	APlayerController* PC = GetOwningPlayer();
	bool bAnyFocused = false;
	ForEachButton([this, PC, bMouseMoved, &bAnyFocused](FLBAMenuButton& Entry)
	{
		UButton* Button = Entry.Button;
		if (bMouseMoved && Button->IsHovered() && !Button->HasUserFocus(PC))
		{
			FocusButton(Button);
		}
		const bool bFocused = Button->HasUserFocus(PC);
		if (bFocused)
		{
			LastFocused = Button;
			bAnyFocused = true;
		}
		Button->SetBackgroundColor(bFocused ? FocusColor : (Entry.bActive ? ActiveColor : FLinearColor::Transparent));
		const bool bBright = !Entry.bTab || bFocused || Entry.bActive;
		Entry.Label->SetColorAndOpacity(FSlateColor(Entry.bCurrent ? CurrentColor : (bBright ? FLinearColor::White : IdleTextColor)));
	});
	// Activating the window or clicking the backdrop hands focus to the game viewport, which would leave the
	// keyboard and gamepad with nothing to navigate: take it back.
	if (!bAnyFocused)
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
	if (Key == EKeys::Escape || Key == EKeys::M || Key == EKeys::Gamepad_Special_Right || Key == EKeys::Gamepad_FaceButton_Right)
	{
		Close();
		return FReply::Handled();
	}
	const int32 Step = Key == EKeys::Q || Key == EKeys::Gamepad_LeftShoulder ? -1 : (Key == EKeys::E || Key == EKeys::Gamepad_RightShoulder ? 1 : 0);
	if (Step != 0 && !Planets.IsEmpty())
	{
		// Next planet, focus on its island shown.
		const int32 Index = (Planets.IndexOfByKey(SelectedPlanet) + Step + Planets.Num()) % Planets.Num();
		SelectPlanet(Planets[Index]);
		FocusDefaultButton();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void ULBAIslandMenu::Close()
{
	if (ALBAPlayerController* PC = GetOwningPlayer<ALBAPlayerController>())
	{
		PC->CloseIslandMenu();
	}
	else
	{
		RemoveFromParent();
	}
}

void ULBAIslandMenu::OnButtonClicked(ELBAMenuAction Action, int32 Value)
{
	UGameInstance* GameInstance = GetGameInstance();
	switch (Action)
	{
	case ELBAMenuAction::Quit:
		UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
		return;

	case ELBAMenuAction::Planet:
		SelectPlanet(ELBAPlanet(Value));
		return;

	case ELBAMenuAction::Island:
		SelectIsland(Value);
		return;

	case ELBAMenuAction::IslandStart:
		if (!Islands.IsValidIndex(Value))
		{
			return;
		}
		// Give the game its input back before the level changes (the menu goes away with the world). Already
		// there: just resume.
		Close();
		if (Value != CurrentIsland)
		{
			GameInstance->GetSubsystem<ULBAIslandTravelSubsystem>()->TravelToIsland(Islands[Value].Id);
		}
		return;

	case ELBAMenuAction::Scene:
	{
		// On the island variant shown (storm or sunny Citadel...), also for an interior's way back out.
		const TSoftObjectPtr<UWorld> IslandLevel = Islands.IsValidIndex(SelectedIsland) ? Islands[SelectedIsland].Level : TSoftObjectPtr<UWorld>();
		Close();
		GameInstance->GetSubsystem<ULBASceneTravelSubsystem>()->TravelToSceneOnIsland(Value, IslandLevel);
		return;
	}
	}
}

#undef LOCTEXT_NAMESPACE
