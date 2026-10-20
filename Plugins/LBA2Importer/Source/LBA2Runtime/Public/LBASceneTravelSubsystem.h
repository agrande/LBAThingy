#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "LBASceneTravelSubsystem.generated.h"

class ALBACubeChangeVolume;
class APawn;
class ULBASceneAsset;
class UWorld;
struct FAssetData;

/**
 * Moves the player between scenes: doors and exits (ALBACubeChangeVolume), and lba.Scene. Scenes are found in the
 * asset registry by number (ULBASceneAsset::SceneIndex). A scene already loaded in the current level (another cube
 * of the same island) is reached by teleporting; otherwise its scenery level opens and ALBAGameMode places the
 * player at the arrival point.
 */
UCLASS()
class LBA2RUNTIME_API ULBASceneTravelSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Scene assets known to the asset registry. */
	static TArray<FAssetData> FindSceneAssets();

	/** Loads the scene asset of a scene number, or null when it was not imported. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	static ULBASceneAsset* FindScene(int32 SceneIndex);

	/** Goes to a scene's hero start. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TravelToScene(int32 SceneIndex);

	/**
	 * Goes to a scene, the player's feet at a position of that scene (LBA units, scene-local), facing an LBA angle.
	 * bSnapToGround puts the feet on the floor below, like the original's ReajustPos.
	 */
	bool TravelToSceneAt(int32 SceneIndex, const FIntVector& Feet, int32 Beta, bool bSnapToGround);

	/** A player pawn entered a cube change zone. */
	void OnCubeChangeZone(ALBACubeChangeVolume* Zone, APawn* Pawn);

	/**
	 * Called by ALBAGameMode when the player spawns: the transform (capsule center) waiting for this level, if any.
	 * PawnHalfHeight is the capsule half height of the pawn about to spawn.
	 */
	bool ConsumeArrival(UWorld* World, float PawnHalfHeight, FTransform& OutTransform);

private:
	struct FArrival
	{
		FString LevelPackage;
		FVector Feet = FVector::ZeroVector;
		double Yaw = 0.0;
		bool bSnapToGround = true;
	};

	/** Picks the level to open for a scene: the current one if the scene is in it, else the island variant last visited. */
	FString ChooseSceneryLevel(const ULBASceneAsset* Scene, const UWorld* World) const;
	static FVector SnapFeet(UWorld* World, const FVector& Feet, const AActor* Ignore);

	TOptional<FArrival> PendingArrival;
	/** Island number -> scenery level last played on it (Citadel's storm or sunny variant...). */
	TMap<int32, FString> LastIslandLevels;
	/** No cube change right after arriving (the hero may land in a zone). */
	double ArrivalTime = -1.0;
};
