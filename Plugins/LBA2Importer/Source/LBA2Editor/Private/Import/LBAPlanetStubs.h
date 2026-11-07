#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class ULBAPlanetAsset;
class UWorld;

/**
 * Globe mode: the islands the original's holomap paints but the game never lets anyone visit, as stand-ins (stubs) on
 * the planet's flat map, copied from the holomap's painting: Twinsun's LBA1 islands that LBA2 kept on its globe
 * (Principal, Proxima, Rebellion) and two islets only LBA2 paints (east of the desert, south of Citadel). A first
 * prototype for the feel of the islands' hemisphere: the painting's coast and colors, a relief made up from them.
 *
 * The holomap's texture (HOLOMAP.HQR HQR_TWINSUN_HMG, 256 x 256, longitude along U, latitude along V, RESS palette 0)
 * is LBA1's (RESS.HQR entry 7) repainted: the same islands in the same places. Its land, laid on the map out to the
 * belt's flank (latitude 14), splits into islands; those under a placed island (the planet asset's) are left out.
 * Each kept one: its coast where the smoothed painting turns from sea to land, then ground grown by a grammar measured
 * on LBA2's own islands (Tools/terrain_stats.py, prototyped in Tools/stub_grammar.py): a low shore, a few flat benches
 * at levels meters apart, each where a wanted height (the painting read by hue: olive lowland, brown ridges, pale
 * stone highest; broad noise for hill masses) passes halfway to it, cliffs between benches and along headlands, ramps
 * on some edges, beaches in bays, rolling ground; a shelf under the sea around it. Its vertices carry how much of each
 * kind of ground is around them (grass, earth, sand, pale stone, the rest rock) for M_LBA_StubGround's textures
 * (LBAPlanetGround); the shelf and LBA1's pads are plain colors (M_LBA_Color). Collision from its triangles: boats run
 * aground, Twinsen walks on it.
 *
 * Names: LBA1's holomap arrows (tlba-classic RESS.HQR entry 8, text 201, 203, 204) over the painting; land no arrow
 * names is left out (the holomap's copies of the placed islands' own islets).
 *
 * Output under the planet's folder: SM_LBA_Stub_<PLANET>_<NAME> and L_<PLANET>_Stubs, their actors laid on the map
 * (pole at the origin), tagged ULBAPlanetAsset::StubTag; the planet subsystem streams it in like the belt.
 */
namespace LBAPlanetStubs
{
	/** Builds the stubs for Asset's islands at Radius (cm) into Folder; a null level when the holomap can't be read. */
	TSoftObjectPtr<UWorld> Build(FLBAImportContext& Context, const FString& Planet, const ULBAPlanetAsset* Asset, double Radius, const FString& Folder);
}
