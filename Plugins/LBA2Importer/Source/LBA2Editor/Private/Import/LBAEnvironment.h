#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UMaterialInterface;
class UWorld;

/** Lighting and atmosphere for a generated level. Presets follow the original island moods. */
struct FLBAEnvironmentPreset
{
	/**
	 * Lights are in display units: with the manual exposure at ExposureBias 0 (exposure 1), a white surface facing a
	 * sun of pi lux shows as white, and the sky dome shows the palette colors as they are.
	 */
	FLinearColor SunColor = FLinearColor::White;
	float SunIntensity = 2.4f;
	float SkyLightIntensity = 0.35f;

	/**
	 * When set, the sun and sky light are calibrated instead: flat ground in the sun gets the light of this shade
	 * row (the row the original's baked terrain mostly uses; 12 is the unshaded color), and ground in the shade
	 * AmbientLight (in the same units, 1 = unshaded).
	 */
	float GroundRow = 0.f;
	float AmbientLight = 0.22f;

	/** Island palette (RESS.HQR entry, see FLBAXpl) used by the palette shading post-process. */
	int32 PaletteEntry = 42;
	/** Exposure compensation (EV) of the manual exposure. */
	float ExposureBias = 0.f;

	/**
	 * Linear distance fog (view depth, cm) towards the palette's fog color, applied by the palette shading after
	 * shading, like the original's fog tables. FogEnd 0 disables it.
	 */
	FLinearColor FogColor = FLinearColor(0.45f, 0.6f, 0.8f);
	float FogStart = 0.f;
	float FogEnd = 0.f;

	/**
	 * Original sky: a dome showing the island's cloud plane fading into the fog color (M_LBA_Sky instance).
	 * Without it, the level gets Unreal's sky atmosphere.
	 */
	UMaterialInterface* SkyMaterial = nullptr;
	bool bAtmosphere = true;

	/** Preset for an island file name (CITABAU, DESERT, EMERAUDE...). */
	static FLBAEnvironmentPreset ForIsland(const FString& IslandName);
	/** Neutral preset for isometric interiors. */
	static FLBAEnvironmentPreset Interior();
};

namespace LBAEnvironment
{
	/** Spawns sun, sky light, sky, fog and the post-process volume (palette shading, exposure) into a level. */
	void Spawn(FLBAImportContext& Context, UWorld* World, const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation);

	/** Sun rotation from LBA light angles (alpha = elevation, beta = heading, 1/4096 turns). */
	FRotator SunRotationFromLbaAngles(int32 Alpha, int32 Beta);
}
