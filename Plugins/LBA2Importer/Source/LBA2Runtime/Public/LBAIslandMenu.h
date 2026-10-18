#pragma once

#include "Blueprint/UserWidget.h"
#include "LBAIslandTravelSubsystem.h"
#include "LBAIslandMenu.generated.h"

class UButton;
class UTextBlock;
class ULBAIslandMenu;

/** Forwards one button's click to the menu with the button's index (UButton::OnClicked has no parameters). */
UCLASS()
class ULBAIslandMenuButtonHandler : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TObjectPtr<ULBAIslandMenu> Menu;

	/** Index into the menu's islands, or INDEX_NONE for Quit. */
	int32 Index = INDEX_NONE;

	UFUNCTION()
	void HandleClicked();
};

/**
 * Island picker shown by ALBAPlayerController (M / gamepad Start): one button per imported island,
 * playable with the mouse, the keyboard (arrows, Enter) or a gamepad (d-pad, A). Esc, B, M or Start close it.
 * The layout is built in C++, so it needs no asset; a Blueprint subclass can restyle it through the
 * colors below.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ULBAIslandMenu : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Background of a button that is neither focused nor hovered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor ButtonColor = FLinearColor(0.08f, 0.1f, 0.14f, 1.f);

	/** Background of the focused or hovered button. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor SelectedColor = FLinearColor(0.85f, 0.55f, 0.1f, 1.f);

	/** Text of the island the player is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor CurrentIslandColor = FLinearColor(1.f, 0.85f, 0.35f, 1.f);

	/** Gives keyboard/gamepad focus to the current island's button (or the first one). */
	void FocusDefaultButton();

	void OnButtonClicked(int32 Index);

	/** Gives keyboard/gamepad focus to a button (islands, then Quit). */
	void FocusButton(int32 Index);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	void BuildTree();
	UButton* MakeButton(const FText& Label, int32 Index, bool bCurrent);

	UPROPERTY(Transient)
	TArray<FLBAIslandInfo> Islands;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UButton>> Buttons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ULBAIslandMenuButtonHandler>> Handlers;

	TArray<bool> WasHovered;
	int32 CurrentIndex = INDEX_NONE;
	int32 SelectedIndex = 0;
};
