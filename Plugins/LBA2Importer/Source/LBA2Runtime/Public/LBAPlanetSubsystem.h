#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "LBAPlanet.h"
#include "Subsystems/WorldSubsystem.h"
#include "LBAPlanetSubsystem.generated.h"

class ALBAPlanetActor;
class FLBAPlanetViewExtension;
class APawn;
class ULevel;
class ULevelStreaming;
class ULevelStreamingDynamic;
class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UMaterialParameterCollectionInstance;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UVolumetricCloudComponent;

/**
 * Globe mode (prototype): an island level opened on its planet (ULBAPlanetAsset). The world is flat and still: the
 * planet's islands are laid side by side where they are on its flat map (FLBAPlanetIsland::GetLevelToMap), the opened
 * island where its level has it and the others streamed in around it (their levels as instances, without their own
 * sky, lights, look or starts: the opened island's are the planet's). Gameplay, collision and navigation see that
 * flat world.
 *
 * Only the view is round. Every generated material moves its vertices down onto a sphere of the planet's radius with
 * the camera on top (LBAPlanet.ush, MPC_LBA_Planet), so the sea curves away from under it to a horizon and far islands
 * sink below it, rising as they come near. The sea is the opened island's sea level (FLBAPlanetIsland::SeaLevel:
 * one Water plugin zone over all the sea in view, an ocean per island keeping its coastline dry, its water bent by its
 * material), over a disc kept under the camera for the sea beyond it (or alone, without a sea level). The islands' own
 * water and invisible sea floors are gone; sea queries answer the sea level. The sky atmosphere is a planet of that
 * radius centered under the camera, so its horizon follows. The planet's belt (ULBAPlanetAsset::BeltLevel: Twinsun's
 * Hamalayi mountains) is streamed in on the map around them, closing the islands' hemisphere by its shape alone, and
 * with it the stub islands (ULBAPlanetAsset::StubLevel: the holomap's other islands, copied from its painting).
 *
 * The planet's own level (ULBAPlanetAsset::Level) is that world built ahead: its sub-levels are the islands' levels and
 * their scenes, the belt and the home island's sea, where they are around the home island. It always plays in globe
 * mode, with the home island opened; an island level opened alone plays on its planet when globe mode is requested.
 *
 *   lba.Globe 0/1             globe mode off/on for the next island level opened (-1: the project setting)
 *   lba.Globe.Open ISLAND     opens an island in globe mode
 *   lba.Globe.Bend 0/1        the view bent onto the planet (0: the flat world as it is, to compare)
 *   lba.Globe.Radius M        overrides the planet's radius for the next island level opened (m): the islands' places and the bend
 *   lba.Globe.Debug 1         prints where the camera is on screen
 *   lba.Globe.Sea 0           the sea disc alone for the next level opened (to compare)
 *   lba.Globe.Belt 0          no belt for the next level opened
 *   lba.Globe.Stubs 0         no stub islands for the next level opened
 *   lba.Globe.BentCulling 0   the renderer's own culling, by the flat world (LBAPlanetCulling.h)
 *   lba.Globe.Sun 0           the opened island's fixed sun (to compare)
 *   lba.Globe.ShowClouds 1    the opened island's clouds (to compare)
 *   lba.Globe.ShowFog 1       the opened island's height fog (to compare)
 *   lba.Globe.Haze 0          no haze paling the sky towards the sea's horizon
 *   lba.Globe.SurfaceSun 0    every surface lit by the sun of the camera's place (to compare)
 *   lba.Globe.SeaIce 0        no ice on the sea toward the belt; lba.Globe.SeaTurquoise 0: no turquoise sea
 *
 * The bent view is a true sphere with the camera on top, so the sun (a directional light, far away) is where it is seen
 * from the camera's place on the planet: the opened island's sun and character lights turn as the camera travels, lit
 * flat ground staying as bright as in its level (UpdateSun; lba.Globe.Sun*). Towards the belt it goes down, and the
 * sky turns orange: it is always sunset on the Hamalayi (lba.Globe.Sunset*). High up the camera leaves the air that
 * makes the sky orange: above the Dinofly's ceiling it fades back to the day's (lba.Globe.SkySunset*). The
 * one sun lights the whole view as if everything stood at the camera's place, so the palette shading gives each
 * surface the sun of its own place instead (LBAPlanetSunRelight, lba.Globe.SurfaceSun), its orange paler on high ground.
 * The sea is tinted by its own place's sunset (LBAPlanetSeaSunset, lba.Globe.SeaSunsetTint).
 *
 * The sea pales to ice toward the belt and turns turquoise around the desert, like its oasis: the palette shading reads
 * how far each bit of sea is from the belt's land and the islands' coasts (ULBAPlanetAsset::SeaRegions) and shows
 * steps of the island's sea texture recolored toward those regions (LBASeaRegions in the importer; lba.Globe.SeaIce*,
 * lba.Globe.SeaTurquoise*).
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
	/** The planet's flat map (cm, its pole at the origin) to world. */
	FTransform GetMapToWorld() const;
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
	/** The planet's sea or belt level, which stay as they are. */
	bool IsPlanetLevel(const ULevel* Level) const;
	/** The island (index in the planet asset) a level of the world belongs to: its own level or one of its scenes. */
	int32 FindIsland(const ULevel* Level) const;
	/** Streams in the planet's other islands. */
	void LoadIslands();
	/** Turns the sun and the character lights to where the sun is seen from the view's place on the planet. */
	void UpdateSun(bool bCameraCut);
	/** Writes what each surface needs to be lit by the sun of its own place (LBAPlanetSunRelight in LBAPlanet.ush). */
	void UpdateSurfaceSun(UMaterialParameterCollectionInstance& Instance) const;
	/** Writes where the sea pales to ice and turns turquoise (ULBAPlanetAsset::SeaRegions), and the sea's own sunset. */
	void UpdateSeaRegions(UMaterialParameterCollectionInstance& Instance) const;
	/** The opened island has clouds, and they are off (lba.Globe.ShowClouds): the sky has to make up for them. */
	bool IsSkyClear() const;
	void DrawDebug() const;

	UPROPERTY(Transient)
	TObjectPtr<ULBAPlanetAsset> Asset;

	TWeakObjectPtr<ALBAPlanetActor> Planet;
	/** Index of the island whose level is open. */
	int32 Home = INDEX_NONE;
	/** The world is the planet's level (ULBAPlanetAsset::Level): its sub-levels are the islands, the sea and the belt. */
	bool bPlanetLevel = false;
	double Radius = 200000.0;
	/** The other islands' streamed levels, by island. */
	TArray<TWeakObjectPtr<ULevelStreamingDynamic>> IslandStreams;
	TWeakObjectPtr<USkyAtmosphereComponent> Atmosphere;
	/** The opened island's clouds and height fog, which don't suit a small round world (lba.Globe.ShowClouds, ShowFog). */
	TWeakObjectPtr<UVolumetricCloudComponent> SkyClouds;
	TWeakObjectPtr<UExponentialHeightFogComponent> HeightFog;
	/**
	 * The opened island's sky light, and whether it casts shadows in its level: without Lumen (lba.Globe.Lumen) they
	 * would be distance field AO, which like Lumen sees the flat world and darkens the bent one.
	 */
	TWeakObjectPtr<USkyLightComponent> SkyLight;
	bool bSkyLightShadows = true;
	/** Its intensity on the planet (its level's times FLBAPlanetIsland::SkyLightGain), as with Lumen. */
	float SkyLightIntensity = 1.f;
	FLinearColor SkyLightColor = FLinearColor::White;
	/** The opened island's sun and character lights, and what its level gives them. */
	TWeakObjectPtr<UDirectionalLightComponent> SunLight;
	TWeakObjectPtr<UDirectionalLightComponent> CharacterLight;
	TWeakObjectPtr<UDirectionalLightComponent> CharacterFill;
	FRotator LevelSunRotation = FRotator::ZeroRotator;
	float LevelSunIntensity = 0.f;
	FLinearColor LevelSunColor = FLinearColor::White;
	float LevelCharacterIntensity = 0.f;
	FLinearColor LevelCloudScale = FLinearColor::White;
	FLinearColor LevelSkyFactor = FLinearColor::White;
	/** The sky's factor in use: it (and the clouds' scale) makes up for the sun's intensity, so the sky stays as it is. */
	FLinearColor SkyFactor = FLinearColor::Black;
	/** The sunset the look's light tint was last set for. */
	float LookSunset = 0.f;
	/** The place the sun is seen from: the view, smoothed so that the camera swinging around Twinsen doesn't move it. */
	FVector2D SunView = FVector2D::ZeroVector;
	bool bHasSunView = false;
	/** The sun follows the camera (lba.Globe.Sun); then, as UpdateSun last found them, for each surface's own sun: */
	bool bSunFollows = false;
	/** The pole and where the sun stands overhead (flat world), the furthest it goes from overhead (radians), the sine of the level's sun elevation. */
	FVector2D SunPole = FVector2D::ZeroVector;
	FVector2D SunOverhead = FVector2D::ZeroVector;
	float SunMaxAngle = 0.f;
	float SunLevelSine = 1.f;
	/** Towards the renderer's sun, and the sunset its light and the sky are in. */
	FVector CameraToSun = FVector::UpVector;
	float CameraSunset = 0.f;
	/** The day's sky the palette shading shows high up: at the top (alpha: how much of it) and at the horizon (alpha: how far down it is, radians). */
	FLinearColor SkyDayZenith = FLinearColor::Transparent;
	FLinearColor SkyDayHorizon = FLinearColor::Transparent;
	/** The opened island's sea level (Water plugin oceans) is the sea (lba.Globe.Sea); the sea disc lies under it. */
	bool bWaterSea = false;
	/** The planet's sea regions are in use: its mask is the looks' (only at the planet's own radius). */
	bool bSeaRegions = false;
	TWeakObjectPtr<ULevelStreaming> SeaStream;
	/** The planet's belt (ULBAPlanetAsset::BeltLevel), on its map. */
	TWeakObjectPtr<ULevelStreaming> BeltStream;
	/** The planet's stub islands (ULBAPlanetAsset::StubLevel), on its map. */
	TWeakObjectPtr<ULevelStreaming> StubStream;

	/** Culls by what the bent view shows (LBAPlanetCulling.h). */
	TSharedPtr<FLBAPlanetViewExtension, ESPMode::ThreadSafe> ViewExtension;

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
