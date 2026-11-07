#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UMaterialInterface;
struct FLBABrickSprite;

namespace LBA1
{
	class FData;
}

/**
 * Globe mode: the stub islands' ground textures, one tiling texture per kind of ground, all in the home island's
 * palette (Citabau's) so the look shader treats them like LBA2's own art.
 *
 * Rock, grass and pale stone are Citabau's own tiles. Earth and dark ground are LBA1's, its ground bricks drawn as the game
 * draws them and unprojected from iso to a square. Sand is the desert's. Tiles from elsewhere keep their pattern (brightness)
 * but take their colors from the palette ramp of their kind's color, LBA2's own for that ground: natural ground looks the
 * same all over the planet.
 *
 * Output into Folder: T_LBA_StubGround_<Kind> and MI_LBA_StubGround of M_LBA_StubGround (Shaders/Private/LBAStubGround.ush).
 */
namespace LBAPlanetGround
{
	/** The kinds' colors: LBA2's own (sRGB, median of each kind on Citadel, Otringal and the desert). */
	inline const FVector3f EarthColor(124.f, 86.f, 58.f);
	inline const FVector3f SandColor(220.f, 192.f, 120.f);
	/** LBA1's dark ground, brightness and all (sRGB): LBA2 has no such ground of its own. */
	inline const FVector3f DarkColor(84.f, 84.f, 78.f);

	/** The material, null when the game's island files can't be read. LBA1Data null: Citabau's bare ground instead of LBA1's earth. */
	UMaterialInterface* Build(FLBAImportContext& Context, const LBA1::FData* LBA1Data, const FString& Folder);

	/**
	 * LBA1's palette with its colors moved to the home palette's, ramp for ramp, each color to the step as bright: natural
	 * colors onto the ramps of LBA2's ground (grass on its olive, a little flattened, and earth on its brown), the rest onto
	 * the ramp of LBA1's own hue. Exact palette colors, so the look shader treats them as LBA2's own art.
	 */
	TArray<FColor> MakeTownPalette(const TArray<FColor>& LBA1Palette, const TArray<FColor>& Home);

	/**
	 * LBA1's plants and grass have their shadows on the ramp its stone walls are lit on: those pixels next to a plant's move
	 * to the plants' ramp, or a plant's shadow would stay blue-grey among the olive.
	 */
	void MovePlantShadows(FLBABrickSprite& Sprite);
}
