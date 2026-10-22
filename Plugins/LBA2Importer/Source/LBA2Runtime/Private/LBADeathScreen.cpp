#include "LBADeathScreen.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "LBAIslandTravelSubsystem.h"

#define LOCTEXT_NAMESPACE "LBADeathScreen"

namespace
{
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

ULBADeathScreen* ULBADeathScreen::Show(APlayerController* Player, TSubclassOf<ULBADeathScreen> ScreenClass)
{
	if (!Player)
	{
		return nullptr;
	}
	ULBADeathScreen* Screen = CreateWidget<ULBADeathScreen>(Player, ScreenClass ? *ScreenClass : ULBADeathScreen::StaticClass());
	if (!Screen)
	{
		return nullptr;
	}
	Screen->AddToViewport(90);
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(Screen->TakeWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Player->SetInputMode(InputMode);
	Player->SetShowMouseCursor(true);
	if (Screen->RetryButton)
	{
		Screen->RetryButton->SetUserFocus(Player);
	}
	return Screen;
}

TSharedRef<SWidget> ULBADeathScreen::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

void ULBADeathScreen::BuildTree()
{
	SetIsFocusable(true);

	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Dim"));
	Dim->SetBrushColor(DimColor);
	Dim->SetHorizontalAlignment(HAlign_Center);
	Dim->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Dim;

	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Content"));
	Dim->SetContent(Box);

	UVerticalBoxSlot* TitleSlot = Box->AddChildToVerticalBox(MakeText(WidgetTree, LOCTEXT("Title", "You died!"), 56, TitleColor));
	TitleSlot->SetHorizontalAlignment(HAlign_Center);
	TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 28.f));

	RetryButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Retry"));
	RetryButton->SetBackgroundColor(ButtonColor);
	UTextBlock* Label = MakeText(WidgetTree, LOCTEXT("Retry", "Retry"), 32, FLinearColor::White);
	RetryButton->SetContent(Label);
	if (UBorderSlot* LabelSlot = Cast<UBorderSlot>(Label->Slot))
	{
		LabelSlot->SetPadding(FMargin(56.f, 14.f));
	}
	RetryButton->OnClicked.AddDynamic(this, &ULBADeathScreen::OnRetryClicked);
	UVerticalBoxSlot* ButtonSlot = Box->AddChildToVerticalBox(RetryButton);
	ButtonSlot->SetHorizontalAlignment(HAlign_Center);
}

FReply ULBADeathScreen::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Enter || Key == EKeys::SpaceBar || Key == EKeys::Gamepad_FaceButton_Bottom || Key == EKeys::Gamepad_Special_Right)
	{
		Retry();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply ULBADeathScreen::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// Clicks on the backdrop stay here instead of reaching the game viewport.
	return FReply::Handled();
}

void ULBADeathScreen::Retry()
{
	if (bRetrying)
	{
		return;
	}
	bRetrying = true;
	if (APlayerController* PC = GetOwningPlayer())
	{
		PC->SetInputMode(FInputModeGameOnly());
		PC->SetShowMouseCursor(false);
	}
	ULBAIslandTravelSubsystem* Travel = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULBAIslandTravelSubsystem>() : nullptr;
	const FName IslandId = Travel ? Travel->GetCurrentIslandId() : NAME_None;
	if (IslandId.IsNone() || !Travel->TravelToIsland(IslandId))
	{
		UGameplayStatics::OpenLevel(this, FName(UGameplayStatics::GetCurrentLevelName(this, true)));
	}
}

#undef LOCTEXT_NAMESPACE
