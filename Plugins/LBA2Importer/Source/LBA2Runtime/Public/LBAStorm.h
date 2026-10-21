#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LBAStormWind.h"
#include "LBAStorm.generated.h"

class UMaterialParameterCollection;

/**
 * A storm's wind around an island (see FLBAStormWind): the actor is the eye's center, at sea level, and its yaw turns
 * the eye's axes. ULBAWindSubsystem gathers the level's storms; anything can ask it for the wind anywhere, and
 * materials read it from MaterialParameters.
 */
UCLASS(hidecategories = (Rendering, Physics, Collision, Input, HLOD, Cooking))
class LBA2RUNTIME_API ALBAStorm : public AActor
{
	GENERATED_BODY()

public:
	ALBAStorm();

	/** The wind's parameters; its Center and Yaw follow the actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm")
	FLBAStormWind Wind;

	/**
	 * Where materials read the wind (MPC_LBA_Wind: FLBAStormWind::ShaderParameterNames, see LBAWind.ush), written each
	 * frame while playing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm")
	TObjectPtr<UMaterialParameterCollection> MaterialParameters;

	/** The wind's parameters as placed (Center and Yaw from the actor). */
	UFUNCTION(BlueprintPure, Category = "Storm")
	FLBAStormWind GetWind() const;

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
