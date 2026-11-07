#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class ULBAPlanetAsset;
class UTexture2D;

/**
 * Globe mode: the sea's colors by place. Toward Twinsun's Hamalayi it pales to ice, around the desert it turns to the
 * turquoise of its oasis (the river by the bell in scene 55 of the original). Prototyped in Tools/sea_regions.py.
 *
 *   atlas  per island, its sea texture recolored in Steps steps toward each region's colors (ice: slots 0 to 3,
 *          turquoise: slots 4 to 7, 128 texels each, stacked down), every texel still a palette color. Ice walks each
 *          color up the sea's own bank of the palette to its pale blues and white, so the pattern stays; turquoise is
 *          another hue, blended in linear light and snapped to the palette. The colors go by brightness rank onto
 *          measured ramps: the oasis water in lba2cc's shot of the Oasis, the holomap's fringe along the Hamalayi.
 *   mask   per planet, on its flat map (pole at the center, MaskExtent cm each way): distances in meters (0 to 255)
 *          from the sea to the belt's land (R), to the turquoise islands' land (G), to the other islands' land (B).
 *
 * The palette shading (LBACartoonWater) picks a step of the atlas per texel of the drifting sea texture from the
 * mask's distances (ULBAPlanetSubsystem: lba.Globe.SeaIce*, lba.Globe.SeaTurquoise*).
 */
namespace LBASeaRegions
{
	constexpr int32 Steps = 4;
	constexpr int32 MaskSize = 1024;
	/** Half the mask's side (cm): out to the belt's inner shores and some of its land, 1.25 m per texel. */
	constexpr double MaskExtent = 64000.0;

	/** The atlas of an island's sea texture: SeaPage is the original's 256 wide sky and sea page (sea on the left). */
	UTexture2D* BuildAtlas(FLBAImportContext& Context, const FString& Folder, const FString& Island, const TArray<uint8>& SeaPage,
		const TArray<FColor>& Palette);

	/**
	 * The planet's mask from its islands' coastlines and the belt's land (map points, cm). TurquoiseIslands: islands
	 * whose sea turns turquoise. Null when there is nothing to measure from.
	 */
	UTexture2D* BuildMask(FLBAImportContext& Context, const FString& Planet, const ULBAPlanetAsset& Asset, const TArray<FVector2D>& BeltLand,
		const TArray<FName>& TurquoiseIslands, const FString& Folder);
}
