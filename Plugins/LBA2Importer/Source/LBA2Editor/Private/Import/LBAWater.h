#pragma once

#include "CoreMinimal.h"

class AActor;
class FLBAImportContext;
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

namespace LBAWater
{
	/**
	 * Spawns a water zone and an ocean at sea level: Unreal's ocean material tinted towards the preset's SeaColor
	 * under the level's lights, with a calm swell or storm waves (shared, editable wave assets). Returns how bright
	 * open water renders relative to SeaColor, which the palette shading uses as the water's neutral light.
	 */
	float Spawn(FLBAImportContext& Context, UWorld* World, const FLBAWaterSettings& Settings, const FLBAEnvironmentPreset& Preset,
		const FLBAEnvironmentLights& Lights);
}
