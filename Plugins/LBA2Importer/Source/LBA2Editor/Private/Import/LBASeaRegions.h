#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UTexture2D;

/**
 * Globe mode: the sea's colors by latitude. Toward Twinsun's Hamalayi in the north it pales to ice, toward the pole
 * under the sun in the south it takes on the teal of the desert's oasis (the river by the bell in scene 55 of the
 * original), so its color tells the player roughly where they are. Prototyped in Tools/sea_regions.py.
 *
 * The atlas, per island: its sea texture recolored (128 texels per slot, stacked down), every texel still a palette
 * color. Slots 0 to IceSteps - 1 are steps toward ice: each color walks up the sea's own bank of the palette, by its
 * brightness rank, onto the holomap's fringe along the Hamalayi (pale blues and white), so the pattern stays. The last
 * slot is turquoise: the sea's main tone turned to the oasis water's.
 *
 * The palette shading (LBACartoonWater) picks a slot per texel of the drifting sea texture by the sea's distance from
 * the pole (ULBAPlanetSubsystem: lba.Globe.SeaIce*, lba.Globe.SeaTurquoise*).
 */
namespace LBASeaRegions
{
	constexpr int32 IceSteps = 4;
	constexpr int32 Slots = IceSteps + 1;

	/** The atlas of an island's sea texture: SeaPage is the original's 256 wide sky and sea page (sea on the left). */
	UTexture2D* BuildAtlas(FLBAImportContext& Context, const FString& Folder, const FString& Island, const TArray<uint8>& SeaPage,
		const TArray<FColor>& Palette);
}
