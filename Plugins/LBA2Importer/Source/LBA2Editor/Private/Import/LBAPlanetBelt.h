#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UWorld;

/**
 * Globe mode: Twinsun's Hamalayi belt, the low mountains around the planet just past the equator that close the
 * islands' hemisphere, as real geometry on the planet's flat map (LBAPlanetBuilder). Nothing stops anyone at the belt
 * but its shape: boats run aground on its shore, Twinsen walks up its flank (to 6 m) to a cliff band steeper than he
 * can walk (68 degrees against 55, up to 16 m). The Dinofly flies over it (crests 20 to 30 m).
 *
 * Where it is comes from the original's holomap (HOLOMAP.HQR, HOLO.H): its texture (HQR_TWINSUN_HMG, 256 x 256,
 * longitude along U, latitude -90 to 90 along V) gives, at each longitude, the belt's shore facing the islands (where
 * the dark sea starts), the foot of its snow (the cliff band) and its far edge; its altitudes (HQR_TWINSUN_HMT, 17
 * latitudes x 32 longitudes) say where the crest is higher or lower (the latitudes 0 and -11.25, the higher), from 20 m
 * at its lowest to 30 m at its highest; a third as wide as the holomap's band. Twinsun at 320 m: the shore 424 to
 * 513 m from the pole.
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
