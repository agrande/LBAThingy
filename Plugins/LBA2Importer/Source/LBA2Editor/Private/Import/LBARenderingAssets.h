#pragma once

#include "CoreMinimal.h"
#include "LBA2ImporterSettings.h"

class FLBAImportContext;
struct FLBAXpl;
class UMaterial;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UMaterialParameterCollection;
class UTexture;
class UTexture2D;
class UVolumeTexture;

/**
 * Assets of the palette look post-process, generated into <Root>/Rendering:
 *
 *  - M_LBA_PaletteShading: post-process material (before TSR) that maps Unreal's lit image back onto the palette's
 *    shade ramps with a world-space dither (Shaders/Private/LBAPaletteShading.ush).
 *  - MI_LBA_Look_Original / MI_LBA_Look_Enhanced: the two looks, as parameter sets of that material. Edit these to
 *    restyle every level; they are only reset when a newer importer changes their defaults.
 *  - Per island palette: T_LBA_Shades_<Palette> (shade paths), VT_LBA_PaletteLut_<Palette> (nearest palette color),
 *    and MI_LBA_Original_<Palette> / MI_LBA_Enhanced_<Palette>, children of the looks. Per island tweaks of the
 *    enhanced look go in MI_LBA_Enhanced_<Palette>, which keeps them across imports.
 *  - VT_LBA_Dither3D / T_LBA_DitherRamp: Surface-Stable Fractal Dithering pattern. T_LBA_HalftoneRamp: halftone
 *    screen thresholds.
 *  - M_LBA_RainDrop: rain streaks on NS_LBA_Rain's sprites (plugin content), translucent after motion blur;
 *    M_LBA_Rain: rain splashes, a post-process after tonemapping (both in Shaders/Private/LBARain.ush). Levels with
 *    rain get their own instances (LBAEnvironment).
 *  - MPC_LBA_Wind: the storm wind for shaders (FLBAStormWind::ShaderParameterNames, Shaders/Private/LBAWind.ush),
 *    written each frame by ULBAWindSubsystem; all zero (no wind) without a storm.
 *  - MPC_LBA_Planet: globe mode's planet for shaders (Shaders/Private/LBAPlanet.ush), written each frame by
 *    ULBAPlanetSubsystem; off without a planet. M_LBA_PlanetSea: a planet's sea (single layer water bent onto it).
 */
class FLBARenderingAssets
{
public:
	explicit FLBARenderingAssets(FLBAImportContext& InContext);

	/** Palette shading material instance of a look for an island palette (created on first use). */
	UMaterialInterface* GetPaletteShading(const FLBAXpl& Xpl, ELBALookMode Look);

	/** Rain splashes post-process material (created on first use). */
	UMaterial* GetRainMaster();

	/** Rain drop material of NS_LBA_Rain (created on first use). */
	UMaterial* GetRainDropMaster();

	/** The storm wind's parameter collection (created on first use). */
	UMaterialParameterCollection* GetWindParameters();

	/** Globe mode's parameter collection, MPC_LBA_Planet (created on first use, see LBAPlanet.ush). */
	UMaterialParameterCollection* GetPlanetParameters();

	/** A planet's sea: single layer water on the sea disc, bent onto the planet (created on first use). */
	UMaterial* GetPlanetSeaMaster();

private:
	FString GetFolder() const;
	UMaterial* GetMaster();
	UMaterial* CreateMaster();
	UMaterial* CreateRainMaster();
	UMaterial* CreateRainDropMaster();
	UMaterialInterface* GetLook(ELBALookMode Look);
	void CreateDitherTextures();
	void CreateHalftoneRamp();
	UTexture2D* CreateShadesTexture(const FLBAXpl& Xpl, const FString& Name);
	UVolumeTexture* CreatePaletteLut(const FLBAXpl& Xpl, const FString& Name);

	FLBAImportContext& Context;
	UMaterial* Master = nullptr;
	UMaterial* RainMaster = nullptr;
	UMaterial* RainDropMaster = nullptr;
	UMaterialParameterCollection* WindParameters = nullptr;
	UMaterialParameterCollection* PlanetParameters = nullptr;
	/** The planet's collection changed this import: the materials using it are rebuilt. */
	bool bPlanetParametersChanged = false;
	UMaterial* PlanetSeaMaster = nullptr;
	UMaterialInterface* Looks[2] = {};
	UVolumeTexture* DitherTexture = nullptr;
	UTexture2D* DitherRamp = nullptr;
	UTexture2D* HalftoneRamp = nullptr;
	int32 DitherDotsPerSide = 0;
	TMap<TPair<int32, ELBALookMode>, UMaterialInterface*> Instances;
};

namespace LBAMaterials
{
	/**
	 * Prepares a generated material instance whose parameters users may edit: they are kept unless the instance
	 * was made by an older importer (PresetVersion parameter below Version), in which case they are cleared and
	 * true is returned so the caller sets its defaults again.
	 */
	bool ResetInstanceIfOutdated(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, int32 Version);
}
