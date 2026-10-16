#pragma once

#include "CoreMinimal.h"
#include "LBA2ImporterSettings.h"

class FLBAImportContext;
struct FLBAXpl;
class UMaterial;
class UMaterialInstanceConstant;
class UMaterialInterface;
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
 */
class FLBARenderingAssets
{
public:
	explicit FLBARenderingAssets(FLBAImportContext& InContext);

	/** Palette shading material instance of a look for an island palette (created on first use). */
	UMaterialInterface* GetPaletteShading(const FLBAXpl& Xpl, ELBALookMode Look);

private:
	FString GetFolder() const;
	UMaterial* GetMaster();
	UMaterial* CreateMaster();
	UMaterialInterface* GetLook(ELBALookMode Look);
	void CreateDitherTextures();
	void CreateHalftoneRamp();
	UTexture2D* CreateShadesTexture(const FLBAXpl& Xpl, const FString& Name);
	UVolumeTexture* CreatePaletteLut(const FLBAXpl& Xpl, const FString& Name);

	FLBAImportContext& Context;
	UMaterial* Master = nullptr;
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
