#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UWorld;

/** Lighting and atmosphere for a generated level. Presets follow the original island moods. */
struct FLBAEnvironmentPreset
{
	FLinearColor SunColor = FLinearColor::White;
	float SunIntensity = 8.f;
	FLinearColor FogColor = FLinearColor(0.45f, 0.6f, 0.8f);
	float FogDensity = 0.01f;
	float SkyLightIntensity = 1.f;
	bool bAtmosphere = true;
	bool bClouds = true;
	FLinearColor WaterColor = FLinearColor(0.02f, 0.12f, 0.22f);

	/** Preset for an island file name (CITABAU, DESERT, EMERAUDE...). */
	static FLBAEnvironmentPreset ForIsland(const FString& IslandName);
	/** Neutral preset for isometric interiors. */
	static FLBAEnvironmentPreset Interior();
};

namespace LBAEnvironment
{
	/** Spawns sun, sky light, atmosphere, fog (and clouds) into a level. */
	void Spawn(FLBAImportContext& Context, UWorld* World, const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation);

	/** Sun rotation from LBA light angles (alpha = elevation, beta = heading, 1/4096 turns). */
	FRotator SunRotationFromLbaAngles(int32 Alpha, int32 Beta);
}
