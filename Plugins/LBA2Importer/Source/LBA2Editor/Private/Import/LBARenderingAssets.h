#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
struct FLBAXpl;
class UMaterial;
class UMaterialInterface;
class UTexture;
class UTexture2D;
class UVolumeTexture;

/**
 * Assets of the "original look" post-process, generated into <Root>/Rendering:
 *
 *  - M_LBA_PaletteShading: post-process material (after tonemapping) that maps Unreal's lit image back onto the
 *    palette's shade ramps with a world-space dither (Shaders/Private/LBAPaletteShading.ush).
 *  - Per island palette: T_LBA_Shades_<Palette> (shade paths), VT_LBA_PaletteLut_<Palette> (nearest palette color)
 *    and MI_LBA_PaletteShading_<Palette>, the blendable that generated levels put in their post-process volume.
 *  - VT_LBA_Dither3D / T_LBA_DitherRamp: Surface-Stable Fractal Dithering pattern.
 */
class FLBARenderingAssets
{
public:
	explicit FLBARenderingAssets(FLBAImportContext& InContext);

	/** Palette shading material instance for an island palette (created on first use). */
	UMaterialInterface* GetPaletteShading(const FLBAXpl& Xpl);

private:
	FString GetFolder() const;
	UMaterial* GetMaster();
	UMaterial* CreateMaster();
	void CreateDitherTextures();
	UTexture2D* CreateShadesTexture(const FLBAXpl& Xpl, const FString& Name);
	UVolumeTexture* CreatePaletteLut(const FLBAXpl& Xpl, const FString& Name);

	FLBAImportContext& Context;
	UMaterial* Master = nullptr;
	UVolumeTexture* DitherTexture = nullptr;
	UTexture2D* DitherRamp = nullptr;
	int32 DitherDotsPerSide = 0;
	TMap<int32, UMaterialInterface*> Instances;
};
