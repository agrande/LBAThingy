#pragma once

#include "Blueprint/UserWidget.h"
#include "LBADeathScreen.generated.h"

class UButton;

/**
 * "You died!" over the scene once the player's mount (or hero) is dead, with one button: Retry, which reloads the
 * level (the island starts over). Mouse, Enter / Space or gamepad A. Built in C++, no asset needed; a Blueprint
 * subclass can restyle it through the colors below.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ULBADeathScreen : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor DimColor = FLinearColor(0.12f, 0.f, 0.f, 0.45f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor TitleColor = FLinearColor(1.f, 0.85f, 0.35f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor ButtonColor = FLinearColor(0.85f, 0.55f, 0.1f, 1.f);

	/** Shows the screen for the player, taking the input. */
	static ULBADeathScreen* Show(APlayerController* Player, TSubclassOf<ULBADeathScreen> ScreenClass = nullptr);

	/** Starts the level over (the current island, or whatever level is loaded). */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void Retry();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	void BuildTree();

	UFUNCTION()
	void OnRetryClicked() { Retry(); }

	UPROPERTY(Transient)
	TObjectPtr<UButton> RetryButton;

	bool bRetrying = false;
};
