#pragma once

#include "CoreMinimal.h"

class AActor;
class FLBAImportContext;
class UTexture2D;
class UWorld;

/**
 * Island decor that gives light (lampposts), by island object (.OBL entry): its polygons of palette color GlowColor
 * glow, and a point light at their center gives surfaces facing it the light Light from 3 m away (1 = shows their
 * base color), fading out at Radius (cm). Without bShadows the light casts no shadows: a soft pool of light around the
 * decor. Halo is how much it lights the fog around it (volumetric scattering; 0: none). FalloffExponent 0 is the
 * physical inverse-square falloff; above 0 the light fades as (1 - (d / Radius)^2)^FalloffExponent instead (still
 * Light at 3 m): an even flood, without the hot spot on whatever stands right next to the light.
 */
struct FLBADecorLight
{
	int32 Body = 0;
	int32 GlowColor = 0;
	float Light = 1.f;
	float Radius = 1500.f;
	float Halo = 0.15f;
	bool bShadows = false;
	float FalloffExponent = 0.f;
};

/**
 * Texels of an island's object texture page that glow wherever decor uses them (lit windows, the lighthouse's lamp
 * room): Texels is a rectangle of the 256x256 page; with bGlassOnly only its cool texels (the glass, not the wooden or
 * brass frames) glow. RecolorBank moves the glowing texels to that palette ramp, keeping their shade (lamplight
 * behind the windows: the palette shading takes a pixel's hue from its base color, the glow only brightens it);
 * INDEX_NONE keeps their colors. ExcludedBodies are objects (.OBL entries) that use the texels for something else
 * and stay unlit. Each window (glowing polygons of a decor in one plane) gets a rect light in front of
 * it, facing out; with bWindow false the decor's glowing parts get one point light at their center instead (a
 * lamp). Light, Radius and Halo as in FLBADecorLight.
 */
struct FLBAGlowTexels
{
	FIntRect Texels;
	bool bGlassOnly = false;
	int32 RecolorBank = INDEX_NONE;
	bool bWindow = true;
	TArray<int32> ExcludedBodies;
	float Light = 1.f;
	float Radius = 1500.f;
	float Halo = 0.15f;
};

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
	float AmbientLight = 0.6f;
	float MaxAmbientFraction = 0.85f;
	/** Overcast: the sky light is a uniform gray sky tinted SkyLightColor instead of a capture of the sky. */
	bool bOvercast = false;
	FLinearColor SkyLightColor = FLinearColor::White;

	/**
	 * Characters (lighting channel 1) also get a key light along the sun, so that together with the sun they receive
	 * this light per unit of N.L (1 = shows the base color). The original lights objects with 15 shades of N.L only,
	 * their base color being shade 9: 15/9; a little less, since characters also get the sky light. 0: no key light.
	 * Calibrated presets only.
	 */
	float CharacterLight = 1.f;
	/**
	 * Fill light of the characters, from the side opposite the sun (same elevation, no shadows): light added per unit
	 * of N.L on the faces the sun misses, on top of the sky light. 0: no fill light.
	 */
	float CharacterFill = 0.2f;

	/** Island palette (RESS.HQR entry, see FLBAXpl) used by the palette shading post-process. */
	int32 PaletteEntry = 42;
	/** Exposure compensation (EV) of the manual exposure. */
	float ExposureBias = 0.f;
	/** Exposure (EV) of the enhanced look's grade on this level (0 keeps the shared enhanced look's). */
	float EnhancedExposure = 0.f;

	/**
	 * The palette's fog color: color of the height fog, and of the original look's linear distance fog (view depth,
	 * cm), applied by the palette shading after shading like the original's fog tables. FogEnd 0 disables it.
	 */
	FLinearColor FogColor = FLinearColor(0.45f, 0.6f, 0.8f);
	float FogStart = 0.f;
	float FogEnd = 0.f;

	/**
	 * Height fog, with volumetric fog: it starts at HeightFogStart (cm from the camera; the original drew everything
	 * up to its fog distance crisp) and hides 95% of a surface at sea level FogVisibility further. 0: no height fog;
	 * islands set both from the original's fog distance unless their preset sets FogVisibility. Its density halves
	 * every 1 / (FogHeightFalloff / 1000) cm above sea level (0.5: every 20 m). HeightFogColor replaces the palette's
	 * fog color (the original look keeps the palette's).
	 */
	float HeightFogStart = 0.f;
	float FogVisibility = 0.f;
	float FogHeightFalloff = 0.5f;
	TOptional<FLinearColor> HeightFogColor;
	/** Height (cm) of the height fog's "sea level": FogVisibility holds there, the fog thickens below it. */
	float HeightFogZ = 0.f;
	/**
	 * Under a roof (interiors): the sun only stands for the light from overhead, through the roofs. The height fog gets
	 * no glow towards it, and no volumetric fog (the sun would light it through the roofs).
	 */
	bool bUnderRoof = false;

	/**
	 * Rain (both looks), 0 for none: NS_LBA_Rain keeps Rain * 18000 drops in view, with the original's wind slant (70% of
	 * its speed) and palette colors, and M_LBA_Rain splashes them on the ground. RainDensity (islands generate it from
	 * their terrain) thins out drops and splashes: the fraction of them falling at each place of RainDensityBounds
	 * (world XY), none for full rain everywhere.
	 */
	float Rain = 0.f;
	UTexture2D* RainDensity = nullptr;
	FBox2D RainDensityBounds = FBox2D(ForceInit);

	/**
	 * Storm wind (an ALBAStorm, see FLBAStormWind), none without bStorm: a small hurricane whose calm eye is centered on
	 * StormEye (island cubes: cube (x, y) spans x to x + 1), its wall StormRadii cubes away (ahead +X, behind -X, right
	 * +Y, left -Y). Its other settings are FLBAStormWind's.
	 */
	bool bStorm = false;
	FVector2D StormEye = FVector2D::ZeroVector;
	FVector4f StormRadii = FVector4f(2.f, 2.f, 2.f, 2.f);

	/** Unreal's sky atmosphere; without it the sky is black, like space. */
	bool bAtmosphere = true;
	/**
	 * Average color of the island's original sky (its cloud texture), linear. The sky atmosphere is tinted to match
	 * it; black keeps Unreal's sky.
	 */
	FLinearColor SkyColor = FLinearColor::Black;
	/**
	 * Clear sky: replaces the original's pale sky color (SkyColor) with this deep blue, with white clouds lit as
	 * brightly as the sky. Unset, clouds have the original sky's color and only the sun lights them.
	 */
	TOptional<FLinearColor> ClearSkyColor;

	/** Unreal's volumetric clouds: coverage from -1 (clear) to 1 (overcast), albedo, storm clouds. */
	bool bClouds = false;
	float CloudCoverage = -0.2f;
	FLinearColor CloudColor = FLinearColor(0.98f, 0.98f, 0.98f);
	bool bStormClouds = false;

	/**
	 * The sea is Unreal water (Water plugin ocean) rather than the original's textured plane, with storm waves or a
	 * calm swell. SeaColor is the original sea's main color (linear, a palette color): the palette shading shades
	 * water along its shade path, open water getting the light SeaLight (1 = the sea color, as the original's
	 * unshaded sea texture; less for a darker sea). WaterNeutralLight is the light that shows the sea color (see
	 * LBAWater).
	 */
	bool bWater = false;
	bool bStormSea = false;
	FLinearColor SeaColor = FLinearColor::Black;
	float SeaLight = 1.f;
	/** The original sea texture and its tile size (cm), for cartoon water. */
	UTexture2D* SeaTexture = nullptr;
	/** The sea texture's steps toward ice and turquoise, for the globe's sea by place (LBASeaRegions). */
	UTexture2D* SeaRegionTexture = nullptr;
	float SeaTileSize = 1024.f;
	float WaterNeutralLight = 1.f;

	/** Lit decor (none: lamps off). */
	TArray<FLBADecorLight> DecorLights;
	const FLBADecorLight* FindDecorLight(int32 Body) const { return DecorLights.FindByPredicate([Body](const FLBADecorLight& Light) { return Light.Body == Body; }); }
	/** Glowing texels of the object texture page (none: windows dark). */
	TArray<FLBAGlowTexels> GlowTexels;
	/**
	 * Light of the lamps of lba2remake models (their bulbs and shades glow, see FLBARemakeAssets::FLamp): Light, Radius,
	 * Halo and bShadows as in FLBADecorLight. Light 0: lamps off.
	 */
	FLBADecorLight RemakeLamps = { 0, 0, 0.f };
	/** Light of the glowing pools, beacons and globes of lba2remake models (FLBARemakeAssets::FLamp::EKind), as RemakeLamps. */
	FLBADecorLight RemakePools = { 0, 0, 0.f };
	FLBADecorLight RemakeBeacons = { 0, 0, 0.f };
	FLBADecorLight RemakeGlobes = { 0, 0, 0.f };

	/** Preset for an island file name (CITABAU, DESERT, EMERAUDE...). */
	static FLBAEnvironmentPreset ForIsland(const FString& IslandName);
	/** Neutral preset for isometric interiors. */
	static FLBAEnvironmentPreset Interior();
};

/** Sun and sky light of a level, in display units (see FLBAEnvironmentPreset). */
struct FLBAEnvironmentLights
{
	/** Sun illuminance (lux) and sky light intensity. */
	float SunIntensity = 0.f;
	float SkyLightIntensity = 0.f;
	/** Illuminance (lux) of the characters' key and fill lights. */
	float CharacterLightIntensity = 0.f;
	float CharacterFillIntensity = 0.f;
	/** Light flat ground receives in the shade, and in the sun (1 = shows its base color). */
	float AmbientLight = 0.f;
	float GroundLight = 0.f;
};

namespace LBAEnvironment
{
	/** Sun and sky light intensities for a preset: calibrated from its GroundRow, or the preset's own values. */
	FLBAEnvironmentLights ComputeLights(const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation, int32 NeutralRow);

	/**
	 * Spawns sun, sky light, sky atmosphere, clouds, fog and the post-process volume (palette looks, exposure) into
	 * a level.
	 */
	void Spawn(FLBAImportContext& Context, UWorld* World, const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation);

	/**
	 * Spawns the point light of lit decor, attached to the decor actor, at LocalPosition (mesh space) with the light's
	 * color and source radius (cm).
	 */
	void SpawnDecorLight(FLBAImportContext& Context, AActor* Decor, const FLBADecorLight& DecorLight, const FVector& LocalPosition,
		const FLinearColor& Color, float SourceRadius);

	/** Intensity of a decor light's point light: candelas, or unitless with a FalloffExponent (see FLBADecorLight). */
	float GetDecorLightIntensity(const FLBADecorLight& DecorLight);

	/**
	 * Spawns the light of a glowing window, attached to the decor actor: a rect light of the window's size (cm) at
	 * LocalPosition (mesh space), facing LocalNormal.
	 */
	void SpawnWindowLight(FLBAImportContext& Context, AActor* Decor, const FLBAGlowTexels& Glow, const FVector& LocalPosition,
		const FVector& LocalNormal, const FVector2D& Size, const FLinearColor& Color);

	/** Sun rotation from LBA light angles (alpha = elevation, beta = heading, 1/4096 turns). */
	FRotator SunRotationFromLbaAngles(int32 Alpha, int32 Beta);
}
