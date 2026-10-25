#pragma once

#include "GameFramework/Actor.h"
#include "LBAIslandTravelSubsystem.h"
#include "LBATravelGateway.generated.h"

class UBoxComponent;
class UPointLightComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * A pad that sends the player somewhere when they step on it: an island, a scene of the game, a test level or one of
 * its stations (FLBATravelDestination). Its sign names the destination. Test levels use them as shortcuts to the places
 * where a feature is best tried (the storm over Citadel for the Dinofly...).
 *
 * It does not fire for a player who arrives standing on it: they must step off and back on.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBATravelGateway : public AActor
{
	GENERATED_BODY()

public:
	ALBATravelGateway();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA|Travel")
	FLBATravelDestination Destination;

	/** Sign text; empty names the destination. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA|Travel")
	FText Label;

	/** Sends the player there now. */
	UFUNCTION(BlueprintCallable, Category = "LBA|Travel")
	bool Travel();

	UTextRenderComponent* GetSign() const { return Sign; }
	UStaticMeshComponent* GetPad() const { return Pad; }

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void NotifyActorBeginOverlap(AActor* OtherActor) override;
	virtual void NotifyActorEndOverlap(AActor* OtherActor) override;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Travel")
	TObjectPtr<UBoxComponent> Trigger;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Travel")
	TObjectPtr<UStaticMeshComponent> Pad;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Travel")
	TObjectPtr<UTextRenderComponent> Sign;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Travel")
	TObjectPtr<UPointLightComponent> Glow;

private:
	static bool IsPlayerPawn(const AActor* Actor);

	/** A player stood on it when it started or when they arrived: they must step off first. */
	bool bArmed = false;
};
