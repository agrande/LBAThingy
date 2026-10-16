#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UWorld;

/** Lighting and atmosphere for a generated level. Presets follow the original island moods. */
struct FLBAEnvironmentPreset
{
	/**
	 * Lights are in display units: with the manual exposure at ExposureBias 0 (exposure 1), a white surface facing a
	 * sun of pi lux shows as white.
	 */
	FLinearColor SunColor = FLinearColor::White;
	float SunIntensity = 2.4f;
	float SkyLightIntensity = 0.35f;

	/**
	 * When set, the sun and sky light are calibrated instead: flat ground in the sun gets the light of this shade
	 * row (the row the original's baked terrain mostly uses; 12 is the unshaded color), and ground in the shade
	 * AmbientLight (in the same units, 1 = unshaded), at most MaxAmbientFraction of the sunlit ground. The original
	 * has no cast shadows, only its baked shading, so shade stays fairly bright.
	 */
	float GroundRow = 0.f;
	float AmbientLight = 0.45f;
	float MaxAmbientFraction = 0.7f;
	/** Overcast: the sky light is a uniform gray sky tinted SkyLightColor instead of a capture of the sky. */
	bool bOvercast = false;
	FLinearColor SkyLightColor = FLinearColor::White;

	/** Island palette (RESS.HQR entry, see FLBAXpl) used by the palette shading post-process. */
	int32 PaletteEntry = 42;
	/** Exposure compensation (EV) of the manual exposure. */
	float ExposureBias = 0.f;

	/**
	 * The palette's fog color: color of the height fog, and of the original look's linear distance fog (view depth,
	 * cm), applied by the palette shading after shading like the original's fog tables. FogEnd 0 disables it.
	 */
	FLinearColor FogColor = FLinearColor(0.45f, 0.6f, 0.8f);
	float FogStart = 0.f;
	float FogEnd = 0.f;

	/**
	 * Height fog, with volumetric fog: it starts at HeightFogStart (cm from the camera; the original drew everything
	 * up to its fog distance crisp) and hides 95% of a surface at sea level FogVisibility further. 0: no height fog.
	 */
	float HeightFogStart = 0.f;
	float FogVisibility = 0.f;

	/** Unreal's sky atmosphere; without it the sky is black, like space. */
	bool bAtmosphere = true;
	/**
	 * Average color of the island's original sky (its cloud texture), linear. The sky atmosphere is tinted to match
	 * it; black keeps Unreal's sky.
	 */
	FLinearColor SkyColor = FLinearColor::Black;

	/** Unreal's volumetric clouds: coverage from -1 (clear) to 1 (overcast), albedo, storm clouds. */
	bool bClouds = false;
	float CloudCoverage = -0.2f;
	FLinearColor CloudColor = FLinearColor(0.98f, 0.98f, 0.98f);
	bool bStormClouds = false;

	/** Preset for an island file name (CITABAU, DESERT, EMERAUDE...). */
	static FLBAEnvironmentPreset ForIsland(const FString& IslandName);
	/** Neutral preset for isometric interiors. */
	static FLBAEnvironmentPreset Interior();
};

namespace LBAEnvironment
{
	/**
	 * Spawns sun, sky light, sky atmosphere, clouds, fog and the post-process volume (palette looks, exposure) into
	 * a level.
	 */
	void Spawn(FLBAImportContext& Context, UWorld* World, const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation);

	/** Sun rotation from LBA light angles (alpha = elevation, beta = heading, 1/4096 turns). */
	FRotator SunRotationFromLbaAngles(int32 Alpha, int32 Beta);
}
