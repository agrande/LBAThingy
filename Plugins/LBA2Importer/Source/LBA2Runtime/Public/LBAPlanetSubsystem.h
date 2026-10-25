#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "LBAPlanet.h"
#include "Subsystems/WorldSubsystem.h"
#include "LBAPlanetSubsystem.generated.h"

class ALBAPlanetActor;
class APawn;
class UExponentialHeightFogComponent;
class UDirectionalLightComponent;
class ULevel;
class ULevelStreamingDynamic;
class USpringArmComponent;

/**
 * Globe mode (prototype): an island level opened on its planet (ULBAPlanetAsset). The island stays as imported, flat
 * and unmoved while the player is on it; around it the sea is a sphere, flat over a cap that holds the island. The
 * planet's other islands are streamed in where they are on it (their levels as instances, without their own sky,
 * lights, look or starts: the opened island's are the planet's).
 *
 * Out at sea the world turns under the player: whenever the player's up (the sea's normal under them) is more than the
 * planet's RecenterAngle from the world's +Z, everything but the player is rotated about the player so that it is +Z
 * again. Gameplay code keeps its flat-world assumptions (gravity -Z, Z = height) within that angle. The camera is
 * tilted by the remaining angle (ULBAPlanetCameraModifier), so the turns don't show. Near an island (the opened one
 * or another), the world is put where that island's level has it: it is the anchor. Invisible markers (volumes,
 * starts, target points) only matter on their island: turns leave them, and they are put back in place on their
 * islands whenever the anchor changes.
 *
 * In globe mode every component of the level is movable, the Water plugin's sea and the invisible sea floor are gone
 * (the planet's sea and sea queries replace them), the seabed is bent with the sea, the sky atmosphere is the planet's
 * and a second sun lights the other hemisphere.
 *
 *   lba.Globe 0/1             globe mode off/on for the next level opened (-1: the project setting)
 *   lba.Globe.Open ISLAND     opens an island in globe mode
 *   lba.Globe.Recenter DEG    turns the world under the player by DEG degrees (debug: the turn must not show)
 *   lba.Globe.Debug 1         prints the planet's state on screen
 */
UCLASS()
class LBA2RUNTIME_API ULBAPlanetSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnRecentered, const FTransform& /*Delta*/, const AActor* /*MovedAlong*/);
	/**
	 * After the world was moved (Delta: old world to new world). MovedAlong: the player's pawn when it was moved with
	 * the world (the world held at an island again), whose world-space state (directions, smoothed rotations, past
	 * positions) must then be moved by Delta too; null when the world turned under it.
	 */
	FOnRecentered OnRecentered;

	/** Whether island levels opened in World play on their planet. */
	static bool IsGlobeRequested(const UWorld* World);

	bool IsActive() const { return Planet.IsValid(); }
	const ULBAPlanetAsset* GetPlanetAsset() const { return Asset; }

	/** World Z of the sea's surface along the vertical through Location (cm); false without a planet. */
	bool GetSeaSurface(const FVector& Location, double& OutSurfaceZ) const;

	/** The sea's up (normal) under Location. */
	FVector GetUpAt(const FVector& Location) const;

	/** Planet space (center at the origin) to world. */
	FTransform GetPlanetToWorld() const;
	/** The opened island's level to world (identity while the world is where the level has it). */
	FTransform GetLevelToWorld() const;
	/** An island's level to world. */
	FTransform GetIslandToWorld(int32 Island) const;

	/** Island (index in the planet asset) whose level was opened. */
	int32 GetHome() const { return Home; }
	/** Island the world is held at, or was last; whether it is now (player on or near it). */
	int32 GetAnchor() const { return Anchor; }
	bool IsAnchored() const { return bAnchored; }
	/** Island of a level of the world (its streamed level or, by default, the opened island). */
	int32 GetLevelIsland(const ULevel* Level) const;

	/** Rotation from the world's +Z to the up under the view's pivot, which the camera applies about the pivot. */
	bool GetViewCorrection(FVector& OutPivot, FQuat& OutRotation) const;

	/**
	 * After a move of the world that took the player along (Delta), the turn about the vertical that keeps their view
	 * where it was on the planet, the view correction's change included: what turns world-space view directions.
	 */
	FQuat GetViewTurn(const FTransform& Delta) const;

	/** Moves everything in the world by Delta (old world to new), but the player's pawn unless bWithPawn. */
	void MoveWorld(const FTransform& Delta, bool bWithPawn);

	/** Turns the world about the player by Degrees around the axis to their right (debug). */
	void DebugTurn(float Degrees);

	int32 GetRecenterCount() const { return RecenterCount; }

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void Activate(ULBAPlanetAsset* InAsset, int32 InAnchor);
	/** Makes a level's content fit the planet (see the class comment). */
	void AdaptLevel(ULevel* Level);
	void OnLevelAdded(ULevel* Level, UWorld* InWorld);
	/** Streams in the planet's other islands. */
	void LoadIslands();
	/** Puts every level's invisible markers where its island is now. */
	void PlaceMarkers();

	APawn* GetPlayerPawn() const;
	/** Planet's transform when an island's level is where it was saved. */
	FTransform GetIslandPlanetToWorld(int32 Island) const;
	/** Sea radius (distance from the planet's center) in a planet direction. */
	double GetSeaRadius(const FVector& PlanetDirection) const;

	void UpdateAnchor(APawn* Pawn);
	void UpdateSea(const APawn* Pawn);
	void UpdateLights(const APawn* Pawn);
	void UpdateFog(const APawn* Pawn);
	void UpdateParameters();
	void DrawDebug(const APawn* Pawn) const;

	UPROPERTY(Transient)
	TObjectPtr<ULBAPlanetAsset> Asset;

	TWeakObjectPtr<ALBAPlanetActor> Planet;
	/** Index of the island whose level is open. */
	int32 Home = INDEX_NONE;
	/** Index of the island the world is held at (or last was). */
	int32 Anchor = INDEX_NONE;
	/** Whether the world is held where the anchor's level has it (player on or near the island). */
	bool bAnchored = true;

	struct FIslandLevel
	{
		int32 Island = INDEX_NONE;
		/** Where its markers are: its island's level to world, then. */
		FTransform Markers;
	};
	TMap<TWeakObjectPtr<ULevel>, FIslandLevel> IslandLevels;
	/** The other islands' streamed levels, by island. */
	TArray<TWeakObjectPtr<ULevelStreamingDynamic>> IslandStreams;

	TWeakObjectPtr<UDirectionalLightComponent> Sun;
	TWeakObjectPtr<UExponentialHeightFogComponent> Fog;
	/** Spring arms whose location lag was turned off for a frame by a move that took the player along. */
	TArray<TWeakObjectPtr<USpringArmComponent>> PausedLag;
	uint64 PausedLagFrame = 0;

	/** Last frame's planet and sea disc, for the sea's motion vectors. */
	FTransform PreviousPlanetToWorld;
	FVector PreviousSeaCenter = FVector::ZeroVector;
	FQuat PreviousSeaRotation = FQuat::Identity;
	bool bHasPreviousSea = false;

	FDelegateHandle LevelAddedHandle;
	int32 RecenterCount = 0;
	double LastMoveMilliseconds = 0.0;
	int32 LastMovedActors = 0;
};

/** Tilts the view about the player by what the world has yet to turn under them (ULBAPlanetSubsystem). */
UCLASS()
class LBA2RUNTIME_API ULBAPlanetCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;
};
