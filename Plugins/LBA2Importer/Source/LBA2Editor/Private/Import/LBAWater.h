#pragma once

#include "CoreMinimal.h"

class AActor;
class FLBAImportContext;
class UMaterialInterface;
class UWaterWavesAsset;
class UWorld;
struct FLBAEnvironmentLights;
struct FLBAEnvironmentPreset;

/** Where a level's sea goes. */
struct FLBAWaterSettings
{
	/** Area the water zone covers (cm). The ocean goes on beyond it to the horizon (the zone's far mesh). */
	FBox Bounds = FBox(ForceInit);
	/**
	 * Coastline (world cm, closed loop): Unreal's ocean leaves the inside of its spline dry. Without one, a small loop
	 * around LandPoint, so the ocean covers everything the terrain doesn't hide.
	 */
	TArray<FVector2D> Coastline;
	FVector2D LandPoint = FVector2D::ZeroVector;
	/** Folder and name prefix of the level's water material instances. */
	FString MaterialFolder;
	FString Name;
	/** Actors whose primitives are the ground the water lies on (shore depth, foam, waves dying out). */
	TArray<AActor*> TerrainActors;
};

/** How a level's sea renders as single layer water. */
struct FLBAWaterLook
{
	/** Single scattering albedo, and the extinction split into scattering and absorption (1/cm). */
	FLinearColor Albedo = FLinearColor::Black;
	FLinearColor Scattering = FLinearColor::Black;
	FLinearColor Absorption = FLinearColor::Black;
	/** How bright open water renders relative to the preset's SeaColor (the palette shading's WaterNeutralLight). */
	float NeutralLight = 1.f;
};

/** A level's sea as spawned. */
struct FLBAWaterSea
{
	/** How bright open water renders relative to the preset's SeaColor (the palette shading's WaterNeutralLight). */
	float NeutralLight = 1.f;
	/** The ocean's material and waves. */
	UMaterialInterface* Material = nullptr;
	UWaterWavesAsset* Waves = nullptr;
};

/** Globe mode's sea (LBAPlanetBuilder): one water zone over all the sea the bent view shows, an ocean per island. */
struct FLBAGlobeSeaSettings
{
	struct FIsland
	{
		FString Name;
		/** Its coastline (world cm, closed loop), left dry; fewer than 3 points: none. */
		TArray<FVector2D> Coastline;
		/** The part of the zone its ocean covers (world cm): the islands' cells tile the zone. */
		FBox2D Cell = FBox2D(ForceInit);
	};

	/** Area the water zone covers (world cm). */
	FBox2D Bounds = FBox2D(ForceInit);
	TArray<FIsland> Islands;
	/** The opened island's ocean material and waves: its sea is the planet's. */
	UMaterialInterface* Material = nullptr;
	UWaterWavesAsset* Waves = nullptr;
};

namespace LBAWater
{
	/** The sea's look under the level's lights: tinted towards the preset's SeaColor. */
	FLBAWaterLook ComputeLook(const FLBAEnvironmentPreset& Preset, const FLBAEnvironmentLights& Lights);

	/**
	 * Spawns a water zone and an ocean at sea level: Unreal's ocean material tinted towards the preset's SeaColor
	 * under the level's lights, with a calm swell or storm waves (shared, editable wave assets).
	 */
	FLBAWaterSea Spawn(FLBAImportContext& Context, UWorld* World, const FLBAWaterSettings& Settings, const FLBAEnvironmentPreset& Preset,
		const FLBAEnvironmentLights& Lights);

	/**
	 * Spawns globe mode's sea: a water zone without far mesh (its few large flat tiles can't bend), and an ocean per
	 * island filling the island's cell. The islands' ground brings its own water terrain (their levels' sea floors).
	 */
	void SpawnGlobeSea(FLBAImportContext& Context, UWorld* World, const FLBAGlobeSeaSettings& Settings);
}
