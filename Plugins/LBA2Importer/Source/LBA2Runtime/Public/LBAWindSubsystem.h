#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LBAWindSubsystem.generated.h"

class ALBAStorm;
class UNiagaraComponent;

/**
 * The wind of the level: the sum of its storms (ALBAStorm), none without. Flyers drift with it, effects show it: the
 * first storm's wind goes to its material parameter collection each frame (for shaders, LBAWind.ush), and the level's
 * rain (NS_LBA_Rain) falls with the wind at the camera.
 *
 *   lba.Wind.Debug 1   draws the wind around the player (arrows colored by speed: white calm, yellow 4 m/s, red 8, purple
 *                      12 and more) and the eye wall at the player's height; 2 also logs the wind at the player.
 */
UCLASS()
class LBA2RUNTIME_API ULBAWindSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Wind velocity (cm/s) at Location, now. */
	UFUNCTION(BlueprintPure, Category = "LBA|Wind")
	FVector GetWindAt(const FVector& Location) const;

	/** Distance from the nearest storm's center in eye radii (1 on its wall); large without a storm. */
	UFUNCTION(BlueprintPure, Category = "LBA|Wind")
	float GetEyeDistance(const FVector& Location) const;

	bool HasWind() const { return !Storms.IsEmpty(); }

	void AddStorm(ALBAStorm* Storm);
	void RemoveStorm(ALBAStorm* Storm);

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void DrawDebug(int32 Level) const;
	void UpdateShaderParameters() const;
	void UpdateRain();

	TArray<TWeakObjectPtr<ALBAStorm>> Storms;

	/** The level's rain and the speed its drops fall at (cm/s, negative), found once. */
	TWeakObjectPtr<UNiagaraComponent> Rain;
	float RainFall = 0.f;
	bool bRainSearched = false;
};
