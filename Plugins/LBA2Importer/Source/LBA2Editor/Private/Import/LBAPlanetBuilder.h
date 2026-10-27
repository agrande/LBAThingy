#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
struct FLBAEnvironmentLights;
struct FLBAEnvironmentPreset;
struct FLBAIsland;
struct FLBAWaterSea;

/**
 * Globe mode (prototype): islands placed on a planet (ULBAPlanetAsset, see ULBAPlanetSubsystem), laid flat side by
 * side on its map; the view bends.
 *
 * Output under <Root>/Planets/:
 *   DA_Planet_<PLANET>         the planet: radius, sun, placed islands
 *   SM_LBA_PlanetSea_<PLANET>  the sea disc, MI_LBA_PlanetSea_<PLANET> its material (the home island's water): the sea
 *                              beyond the sea levels, or all of it without them
 *   L_<ISLAND>_GlobeSea        the planet's sea when the island is opened: a water zone over all the sea the bent view
 *                              shows from the islands, an ocean per island (each keeping its coastline dry), in the
 *                              island's water look (its level's ocean material and waves)
 *   L_<PLANET>_Belt            Twinsun's Hamalayi belt around the islands' hemisphere (LBAPlanetBelt)
 *   L_<PLANET>                 the planet's level: the placed islands' levels and their scenes, the belt and the home
 *                              island's sea as sub-levels, the islands where they are on the map around the home island
 *                              (the first placed), whose level keeps its place
 *
 * Where an island goes: its position on the original's holomap (HOLOMAP.HQR, ARROW.BIN), turned so that its sun (its
 * first section's light angles, as calibrated for its level) is where the planet's sun is. The planet's sun is the
 * direction that best gives every listed island its own sun's elevation (they can't all move: elevation is latitude
 * relative to the sun); each island's heading then matches its sun's azimuth exactly.
 */
namespace LBAPlanetBuilder
{
	/** Whether an island is placed on a planet. */
	bool IsPlaced(const FString& IslandName);

	/**
	 * Places an island on its planet: builds the planet's shared assets and writes the island's placement into the
	 * planet asset. The sea disc's look is the island's (Preset, Lights) when it is the planet's home island (first
	 * listed). Coastline (in its level, cm) and Sea (its level's ocean) go into the planet's sea levels, rebuilt for every
	 * placed island.
	 */
	void AddIsland(FLBAImportContext& Context, const FLBAIsland& Island, const FString& IslandFolder, const FLBAEnvironmentPreset& Preset,
		const FLBAEnvironmentLights& Lights, const TArray<FVector2D>& Coastline, const FLBAWaterSea& Sea);

	/** Rebuilds the level of the planet an island is placed on (L_<PLANET>), with the island's sub-levels as they are now. */
	void UpdatePlanetLevel(FLBAImportContext& Context, const FString& IslandName);
}
