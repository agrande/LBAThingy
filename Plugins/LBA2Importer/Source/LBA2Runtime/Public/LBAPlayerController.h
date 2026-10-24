#pragma once

#include "GameFramework/PlayerController.h"
#include "LBAPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class ULBAIslandMenu;

/**
 * Player controller of ALBAGameMode. Owns the input that works whatever the pawn (Twinsen, Dinofly):
 * M / gamepad Start toggle the island menu, which pauses the game while it is open.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBAPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	/** Menu widget class. Empty uses ULBAIslandMenu. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TSubclassOf<ULBAIslandMenu> IslandMenuClass;

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void OpenIslandMenu();

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void CloseIslandMenu();

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void ToggleIslandMenu();

	UFUNCTION(BlueprintPure, Category = "LBA")
	bool IsIslandMenuOpen() const { return IslandMenu != nullptr; }

	/** The menu when it is open. */
	ULBAIslandMenu* GetIslandMenu() const { return IslandMenu; }

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<ULBAIslandMenu> IslandMenu;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MenuContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ToggleMenuAction;
};
