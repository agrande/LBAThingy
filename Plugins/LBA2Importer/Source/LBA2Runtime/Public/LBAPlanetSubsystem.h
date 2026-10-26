#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "LBAPlanet.h"
#include "Subsystems/WorldSubsystem.h"
#include "LBAPlanetSubsystem.generated.h"

class ALBAPlanetActor;
class APawn;
class ULevel;
class ULevelStreamingDynamic;
class USkyAtmosphereComponent;

/**
 * Globe mode (prototype): an island level opened on its planet (ULBAPlanetAsset). The world is flat and still: the
 * planet's islands are laid side by side where they are on its flat map (FLBAPlanetIsland::GetLevelToMap), the opened
 * island where its level has it and the others streamed in around it (their levels as instances, without their own
 * sky, lights, look or starts: the opened island's are the planet's). Gameplay, collision and navigation see that
 * flat world.
 *
 * Only the view is round. Past the planet's flat radius around the camera, every generated material moves its vertices
 * down onto a sphere of the planet's radius (LBAPlanet.ush, MPC_LBA_Planet), so the sea curves away to a horizon and far
 * islands sink below it, rising as they come near. The sea is a disc kept under the camera (the islands' Water plugin
 * sea and invisible sea floor are gone; sea queries answer the sea level), bent the same way; the sky atmosphere is a
 * planet of that radius centered under the camera, so its horizon follows.
 *
 *   lba.Globe 0/1             globe mode off/on for the next level opened (-1: the project setting)
 *   lba.Globe.Open ISLAND     opens an island in globe mode
 *   lba.Globe.Bend 0/1        the view bent onto the planet (0: the flat world as it is, to compare)
 *   lba.Globe.FlatRadius M    overrides the flat radius around the camera (m; negative: the planet's)
 *   lba.Globe.Radius M        overrides the planet's radius for the next level opened (m): the islands' places and the bend
 *   lba.Globe.Debug 1         prints where the camera is on screen
 */
UCLASS()
class LBA2RUNTIME_API ULBAPlanetSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Whether island levels opened in World play on their planet. */
	static bool IsGlobeRequested(const UWorld* World);

	bool IsActive() const { return Planet.IsValid(); }
	const ULBAPlanetAsset* GetPlanetAsset() const { return Asset; }
	/** The planet's radius in play (cm): the asset's, or lba.Globe.Radius's. */
	double GetRadius() const { return Radius; }

	/** World Z of the sea's surface at Location (cm): the sea level, everywhere; false without a planet. */
	bool GetSeaSurface(const FVector& Location, double& OutSurfaceZ) const;

	/** An island's level to world (the opened island's is identity). */
	FTransform GetIslandToWorld(int32 Island) const;
	/** World location of an island's center. */
	FVector GetIslandCenter(int32 Island) const;

	/** Island (index in the planet asset) whose level was opened. */
	int32 GetHome() const { return Home; }
	/** The island whose center is nearest to Location, and how far it is (cm, horizontally). */
	int32 FindNearestIsland(const FVector& Location, double& OutDistance) const;

	/**
	 * The view the bend is centered on, once per frame from the camera (ULBAPlanetCameraModifier): keeps the sea and
	 * the atmosphere under it and writes the materials' parameters.
	 */
	void UpdateView(const FVector& ViewLocation, bool bCameraCut);

	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void Activate(ULBAPlanetAsset* InAsset, int32 InHome);
	/** Makes a level's content fit the planet (see the class comment). */
	void AdaptLevel(ULevel* Level, int32 Island);
	void OnLevelAdded(ULevel* Level, UWorld* InWorld);
	/** Streams in the planet's other islands. */
	void LoadIslands();
	void DrawDebug() const;

	UPROPERTY(Transient)
	TObjectPtr<ULBAPlanetAsset> Asset;

	TWeakObjectPtr<ALBAPlanetActor> Planet;
	/** Index of the island whose level is open. */
	int32 Home = INDEX_NONE;
	double Radius = 200000.0;
	/** The other islands' streamed levels, by island. */
	TArray<TWeakObjectPtr<ULevelStreamingDynamic>> IslandStreams;
	TWeakObjectPtr<USkyAtmosphereComponent> Atmosphere;

	/** The view this frame and the last, for motion vectors of the bent world. */
	FVector View = FVector::ZeroVector;
	FVector PreviousView = FVector::ZeroVector;
	FVector PreviousSea = FVector::ZeroVector;
	bool bHasView = false;

	FDelegateHandle LevelAddedHandle;
};

/** Hands the camera's final location to the planet each frame (ULBAPlanetSubsystem::UpdateView). */
UCLASS()
class LBA2RUNTIME_API ULBAPlanetCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	ULBAPlanetCameraModifier();

	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;
};
