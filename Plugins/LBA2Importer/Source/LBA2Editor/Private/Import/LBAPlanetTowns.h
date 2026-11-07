#pragma once

#include "CoreMinimal.h"
#include "Import/LBAPlanetDistricts.h"

class FLBAImportContext;
class FLBAMeshBuilder;
class UWorld;

/**
 * Globe mode: LBA1's towns in 3D on their pads (L4 of the stub islands' plan, after LBAPlanetDistricts' L3). Their
 * bricks, extruded as the interiors' are (LBABrickMesh): cubes UV-mapped onto LBA1's brick sprites, their colors moved to the
 * home island's palette (LBAPlanetGround::MakeTownPalette), collision from the bricks' shapes, in each district's frame on the
 * map (FPlaced::FrameToMap, unscaled). Bricks that are flat ground aren't drawn: they are terrain (EmitGround). Later layers
 * replace the rest with LBA2-like or remake models.
 *
 * Output under the planet's folder, Towns/: T_LBA1_<ISLAND>_Bricks (one atlas per island), SM_LBA1_Scene_NNN (+ _Collision),
 * one per LBA1 scene (the columns drawn from it), their actors tagged ULBAPlanetAsset::StubTag in the stubs' level.
 */
namespace LBAPlanetTowns
{
	/** Builds the towns of an island's placed districts into World; the number of scenes built. */
	int32 Build(FLBAImportContext& Context, UWorld* World, const FString& Folder, const FString& Island, const LBAPlanetDistricts::FDistricts& Districts,
		TConstArrayView<LBAPlanetDistricts::FPlaced> Placed, double Radius);

	/**
	 * The towns' flat ground as terrain, into Section of the stub's mesh (M_LBA_StubGround, its vertices relative to Pivot): a
	 * quad on each such brick's top, its corners holding the kinds of the bricks around them.
	 */
	void EmitGround(const LBAPlanetDistricts::FDistricts& Districts, TConstArrayView<LBAPlanetDistricts::FPlaced> Placed, const FVector& Pivot, FLBAMeshBuilder& Builder, int32 Section);
}
