#pragma once

#include "UObject/Interface.h"
#include "LBAInteractable.generated.h"

class APawn;

UINTERFACE(BlueprintType, Blueprintable, MinimalAPI)
class ULBAInteractable : public UInterface
{
	GENERATED_BODY()
};

/**
 * Something the player can use when standing in front of it (doors...): ALBACharacter::TryInteract picks the closest
 * one in reach when the player presses Use, or does the Normal behaviour's action.
 */
class LBA2RUNTIME_API ILBAInteractable
{
	GENERATED_BODY()

public:
	/** Whether the pawn can use it now (reach is checked by the caller). */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "LBA|Interaction")
	bool CanInteract(APawn* User) const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "LBA|Interaction")
	void Interact(APawn* User);

	virtual bool CanInteract_Implementation(APawn* User) const { return true; }
	virtual void Interact_Implementation(APawn* User) {}
};
