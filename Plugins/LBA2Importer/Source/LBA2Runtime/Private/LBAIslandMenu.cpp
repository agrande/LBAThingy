#include "LBAIslandMenu.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateUser.h"
#include "Widgets/SViewport.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LBAPlayerController.h"

#define LOCTEXT_NAMESPACE "LBAIslandMenu"

namespace
{
	constexpr int32 Columns = 2;

	UTextBlock* MakeText(UWidgetTree* Tree, const FText& Text, int32 Size, const FLinearColor& Color)
	{
		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>();
		Block->SetText(Text);
		FSlateFontInfo Font = Block->GetFont();
		Font.Size = Size;
		Block->SetFont(Font);
		Block->SetColorAndOpacity(FSlateColor(Color));
		Block->SetJustification(ETextJustify::Center);
		return Block;
	}
}

void ULBAIslandMenuButtonHandler::HandleClicked()
{
	if (Menu)
	{
		Menu->OnButtonClicked(Index);
	}
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

	const ULBAIslandTravelSubsystem* Travel = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULBAIslandTravelSubsystem>() : nullptr;
	if (Travel)
	{
		Islands = Travel->GetIslands();
	}
	const FName CurrentId = Travel ? Travel->GetCurrentIslandId() : NAME_None;

	// Full-screen dim, with the panel centered on it.
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Dim"));
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.55f));
	Dim->SetHorizontalAlignment(HAlign_Center);
	Dim->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Dim;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Panel"));
	Panel->SetBrushColor(FLinearColor(0.01f, 0.015f, 0.03f, 0.9f));
	Panel->SetPadding(FMargin(32.f, 24.f));
	Dim->SetContent(Panel);

	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Content"));
	Panel->SetContent(Box);

	UVerticalBoxSlot* TitleSlot = Box->AddChildToVerticalBox(MakeText(WidgetTree, LOCTEXT("Title", "Where to?"), 28, FLinearColor::White));
	TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
	TitleSlot->SetHorizontalAlignment(HAlign_Center);

	UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(), TEXT("Islands"));
	Grid->SetSlotPadding(FMargin(6.f));
	Grid->SetMinDesiredSlotWidth(300.f);
	Box->AddChildToVerticalBox(Grid);

	Buttons.Reset();
	Handlers.Reset();
	CurrentIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Islands.Num(); ++Index)
	{
		const bool bCurrent = Islands[Index].Id == CurrentId;
		if (bCurrent)
		{
			CurrentIndex = Index;
		}
		UUniformGridSlot* GridSlot = Grid->AddChildToUniformGrid(MakeButton(Islands[Index].DisplayName, Index, bCurrent), Index / Columns, Index % Columns);
		GridSlot->SetHorizontalAlignment(HAlign_Fill);
	}
	if (Islands.IsEmpty())
	{
		Box->AddChildToVerticalBox(MakeText(WidgetTree, LOCTEXT("NoIslands", "No island imported yet (Tools > LBA2 Importer)."), 16, FLinearColor::Gray));
	}

	UVerticalBoxSlot* QuitSlot = Box->AddChildToVerticalBox(MakeButton(LOCTEXT("Quit", "Quit"), INDEX_NONE, false));
	QuitSlot->SetPadding(FMargin(6.f, 18.f, 6.f, 6.f));
	QuitSlot->SetHorizontalAlignment(HAlign_Center);

	UVerticalBoxSlot* HintSlot = Box->AddChildToVerticalBox(MakeText(WidgetTree, LOCTEXT("Hint", "M / Start: show or hide this menu"), 12, FLinearColor(0.6f, 0.6f, 0.6f)));
	HintSlot->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));

	WasHovered.Init(false, Buttons.Num());
}

UButton* ULBAIslandMenu::MakeButton(const FText& Label, int32 Index, bool bCurrent)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	Button->SetBackgroundColor(ButtonColor);
	UTextBlock* Text = MakeText(WidgetTree, Label, 18, bCurrent ? CurrentIslandColor : FLinearColor::White);
	Button->SetContent(Text);
	if (UBorderSlot* ContentSlot = Cast<UBorderSlot>(Text->Slot))
	{
		ContentSlot->SetPadding(FMargin(24.f, 8.f));
	}

	ULBAIslandMenuButtonHandler* Handler = NewObject<ULBAIslandMenuButtonHandler>(this);
	Handler->Menu = this;
	Handler->Index = Index;
	Button->OnClicked.AddDynamic(Handler, &ULBAIslandMenuButtonHandler::HandleClicked);
	Handlers.Add(Handler);
	Buttons.Add(Button);
	return Button;
}

void ULBAIslandMenu::FocusDefaultButton()
{
	FocusButton(Buttons.IsValidIndex(CurrentIndex) ? CurrentIndex : 0);
}

void ULBAIslandMenu::FocusButton(int32 Index)
{
	if (Buttons.IsValidIndex(Index))
	{
		Buttons[Index]->SetUserFocus(GetOwningPlayer());
		SelectedIndex = Index;
	}
}

void ULBAIslandMenu::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// One selection: pointing at a button focuses it, so mouse and keyboard/gamepad never highlight two buttons.
	APlayerController* PC = GetOwningPlayer();
	bool bAnyFocused = false;
	for (int32 Index = 0; Index < Buttons.Num(); ++Index)
	{
		UButton* Button = Buttons[Index];
		const bool bHovered = Button->IsHovered();
		if (bHovered && !WasHovered[Index])
		{
			FocusButton(Index);
		}
		WasHovered[Index] = bHovered;
		const bool bFocused = Button->HasUserFocus(PC);
		if (bFocused)
		{
			SelectedIndex = Index;
			bAnyFocused = true;
		}
		Button->SetBackgroundColor(bFocused ? SelectedColor : ButtonColor);
	}
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
			FocusButton(SelectedIndex);
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
		if (ALBAPlayerController* PC = GetOwningPlayer<ALBAPlayerController>())
		{
			PC->CloseIslandMenu();
		}
		else
		{
			RemoveFromParent();
		}
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void ULBAIslandMenu::OnButtonClicked(int32 Index)
{
	if (Index == INDEX_NONE)
	{
		UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
		return;
	}
	if (!Islands.IsValidIndex(Index))
	{
		return;
	}
	ULBAIslandTravelSubsystem* Travel = GetGameInstance()->GetSubsystem<ULBAIslandTravelSubsystem>();
	if (Index == CurrentIndex)
	{
		// Already there: just resume.
		if (ALBAPlayerController* PC = GetOwningPlayer<ALBAPlayerController>())
		{
			PC->CloseIslandMenu();
		}
		return;
	}
	// Give the game its input back before the level changes (the menu goes away with the world).
	const FName IslandId = Islands[Index].Id;
	if (ALBAPlayerController* PC = GetOwningPlayer<ALBAPlayerController>())
	{
		PC->CloseIslandMenu();
	}
	Travel->TravelToIsland(IslandId);
}

#undef LOCTEXT_NAMESPACE
