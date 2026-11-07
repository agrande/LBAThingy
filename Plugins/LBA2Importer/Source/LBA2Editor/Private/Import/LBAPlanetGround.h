#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UMaterialInterface;

namespace LBA1
{
	class FData;
}

/**
 * Globe mode: the stub islands' ground textures, one tiling texture per kind of ground, all in the home island's
 * palette (Citabau's) so the look shader treats them like LBA2's own art.
 *
 * Rock, grass and pale stone are Citabau's own tiles. Earth is LBA1's, its ground bricks drawn as the game draws them and
 * unprojected from iso to a square. Sand is the desert's. Tiles from elsewhere keep their pattern (brightness) but take
 * their colors from the palette ramp of their kind's color, LBA2's own for that ground: natural ground looks the same all
 * over the planet.
 *
 * Output into Folder: T_LBA_StubGround_<Kind> and MI_LBA_StubGround of M_LBA_StubGround (Shaders/Private/LBAStubGround.ush).
 */
namespace LBAPlanetGround
{
	/** The kinds' colors: LBA2's own (sRGB, median of each kind on Citadel, Otringal and the desert). */
	inline const FVector3f EarthColor(124.f, 86.f, 58.f);
	inline const FVector3f SandColor(220.f, 192.f, 120.f);

	/** The material, null when the game's island files can't be read. LBA1Data null: Citabau's bare ground instead of LBA1's earth. */
	UMaterialInterface* Build(FLBAImportContext& Context, const LBA1::FData* LBA1Data, const FString& Folder);
}
