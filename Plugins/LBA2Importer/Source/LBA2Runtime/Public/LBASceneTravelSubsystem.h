#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "LBASceneTravelSubsystem.generated.h"

class ALBACubeChangeVolume;
class APawn;
class ULBASceneAsset;
class UWorld;
struct FAssetData;

/** A scene as the travel menu lists it, read from the asset registry without loading the scene asset. */
struct FLBASceneMenuEntry
{
	int32 SceneIndex = INDEX_NONE;
	/** ULBASceneAsset::Island, MenuParent, MenuOrder, MenuName. */
	int32 Island = INDEX_NONE;
	int32 Parent = INDEX_NONE;
	int32 Order = INDEX_NONE;
	FString Name;
	bool bExterior = false;
};

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

	/** Imported story scenes the travel menu lists (MenuOrder >= 0), in menu order. */
	static TArray<FLBASceneMenuEntry> FindMenuScenes();

	/** Goes to a scene's hero start. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TravelToScene(int32 SceneIndex);

	/**
	 * Goes to a scene's hero start on one variant of its island (Citadel's storm or sunny level...): an exterior
	 * scene opens in that level, an interior returns to it through its doors.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TravelToSceneOnIsland(int32 SceneIndex, const TSoftObjectPtr<UWorld>& IslandLevel);

	/** The scene the player is in: the cube under them on an island, the scene travelled to in an interior. -1 if unknown. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	int32 GetCurrentScene() const;

	/**
	 * The scene at a location of a world (null: none known): the island cube under it among the visible scene levels, else
	 * the interior scene InteriorScene if it is visible, else the lowest-numbered interior scene. -1 if none.
	 */
	static int32 FindSceneAt(const UWorld* World, const FVector* Location, int32 InteriorScene = INDEX_NONE);

	/** Package of the island level last played for an island number (its variant), empty if none yet. */
	FString GetLastIslandLevel(int32 Island) const;

	/**
	 * Goes to a scene, the player's feet at a position of that scene (LBA units, scene-local), facing an LBA angle.
	 * bSnapToGround puts the feet on the floor below, like the original's ReajustPos. An exterior scene whose island
	 * level is laid on a planet (ULBAPlanetAsset::Level) is reached there when the player is on that planet, or with
	 * bToPlanet (building exits).
	 */
	bool TravelToSceneAt(int32 SceneIndex, const FIntVector& Feet, int32 Beta, bool bSnapToGround, const FString& PreferredLevel = FString(),
		bool bToPlanet = false);

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

	/**
	 * Picks the level to open for a scene: the preferred one if the scene is in it, else the current one if it is (on a
	 * planet's level, its island level there), else the island variant last visited.
	 */
	FString ChooseSceneryLevel(const ULBASceneAsset* Scene, const UWorld* World, const FString& PreferredLevel) const;
	static FVector SnapFeet(UWorld* World, const FVector& Feet, const AActor* Ignore);

	TOptional<FArrival> PendingArrival;
	/** Island number -> scenery level last played on it (Citadel's storm or sunny variant...). */
	TMap<int32, FString> LastIslandLevels;
	/** No cube change right after arriving (the hero may land in a zone). */
	double ArrivalTime = -1.0;
	/** Scene last travelled to, to tell apart the scenes sharing an interior level. */
	int32 LastScene = INDEX_NONE;
};
