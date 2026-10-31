#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UWorld;

/**
 * Globe mode: Twinsun's Hamalayi belt, the icy mountains around the planet past the equator that close the islands'
 * hemisphere, as real geometry on the planet's flat map (LBAPlanetBuilder), as the original's holomap has it
 * (prototyped in Tools/belt_grammar.py):
 *
 *   footprint  the holomap's painting (HOLOMAP.HQR HQR_TWINSUN_HMG, 256 x 256, longitude along U, latitude -90 to 90
 *              along V, RESS palette 0) at its full width: land = its blues, whites, browns and greens; the teal
 *              fringe is shallow water (as shallow as its speckles are dense); basins the open sea doesn't reach are
 *              frozen low ground; only the ring of land around the planet (and pieces near it). Its coasts wander a
 *              few meters off the texels.
 *   heights    the holomap's own (HOLOGLOB.CPP: radius 1000 plus twice the HQR_TWINSUN_HMT altitude, 17 latitudes x
 *              32 longitudes, 0 to 65 m on a 320 m Twinsun), times the painting's brightness (dark blue low, white
 *              high), eased under the highest vertices around, scaled per 11.25 degrees until its mean is the
 *              holomap mesh's; benches as the stub islands' grammar (LBAPlanetStubs) but taller: a flank over the
 *              shore, a wall (its riser one cell high and every column's crest above it, so no one walks across),
 *              levels 3.5 to 7 m apart up to ~60 m drifting with the place, cliffs or ramps between them.
 *   colors     the painting's through the albedos calibrated for the enhanced grade: dark ice, light ice, snow,
 *              brown rock on the far side, cliff ice where steep, pale shelf under the shallows.
 *
 * Nothing but its shape stops anyone: boats run aground on the shelf, Twinsen stops at the wall, the Dinofly flies
 * over its lower parts (it can't climb over the highest). The build checks that the islands' sea and the other
 * hemisphere's aren't joined by walkable ground (its log says "closed to walkers").
 *
 * Output under the planet's folder: SM_LBA_Belt_<PLANET>_NN (a sector per 11.25 degrees of longitude, vertex colored
 * with M_LBA_Color, collision from its triangles) and L_<PLANET>_Belt, their actors laid on the map (pole at the
 * origin), tagged ULBAPlanetAsset::BeltTag; the planet subsystem streams it in around the opened island.
 */
namespace LBAPlanetBelt
{
	/** Builds the belt for a planet of Radius (cm) into Folder; a null level when the holomap can't be read. */
	TSoftObjectPtr<UWorld> Build(FLBAImportContext& Context, const FString& Planet, double Radius, const FString& Folder);
}
