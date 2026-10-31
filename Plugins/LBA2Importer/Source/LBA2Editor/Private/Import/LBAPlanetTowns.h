#pragma once

#include "CoreMinimal.h"
#include "Import/LBAPlanetDistricts.h"

class FLBAImportContext;
class UWorld;

/**
 * Globe mode: LBA1's towns in 3D on their pads (L4 of the stub islands' plan, after LBAPlanetDistricts' L3). Their
 * bricks as they are, extruded as the interiors' are (LBABrickMesh): cubes UV-mapped onto LBA1's brick sprites (LBA1's
 * palette), collision from the bricks' shapes, in each district's frame on the map (FPlaced::FrameToMap, unscaled).
 * Later layers replace them with LBA2-like or remake models.
 *
 * Output under the planet's folder, Towns/: T_LBA1_<ISLAND>_Bricks (one atlas per island), SM_LBA1_Scene_NNN (+ _Collision),
 * one per LBA1 scene (the columns drawn from it), their actors tagged ULBAPlanetAsset::StubTag in the stubs' level.
 */
namespace LBAPlanetTowns
{
	/** Builds the towns of an island's placed districts into World; the number of scenes built. */
	int32 Build(FLBAImportContext& Context, UWorld* World, const FString& Folder, const FString& Island, const LBAPlanetDistricts::FDistricts& Districts,
		TConstArrayView<LBAPlanetDistricts::FPlaced> Placed, double Radius);
}
