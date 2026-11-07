#pragma once

#include "CoreMinimal.h"
#include "Formats/LBAHqr.h"
#include "Formats/LBAIso.h"

/**
 * Little Big Adventure 1's data (the GOG "tlba-classic" Common folder), read for the globe's district pads (L3): its
 * outdoor scenes' exits, their grids seen from above and its holomap's markers. Formats as ScummVM's twine engine
 * (scene.cpp loadSceneLBA1, grid.cpp, holomap.cpp) and Tools/lba1.py:
 *
 *   scene n     SCENE.HQR entry n (LBA2 adds 1): U8 text bank (= island), U8 game over scene, 4 bytes, S16 light alpha,
 *               beta, 28 bytes of ambience, U8 music, S16 hero x, y, z, U16 size + track script, U16 size + life
 *               script; U16 actors (hero included), each 35 bytes then its two scripts; U16 zones, each S16 min x, y, z,
 *               max x, y, z, U16 type, S16 number, S16 info0..3
 *   its grid    LBA_GRI.HQR entry n: 64 x 64 U16 column offsets (z * 64 + x), each column U8 runs of { U8 flags (bits
 *               0-5 length - 1, 6-7 type: 0 empty, 1 a block per cell, 2/3 one block for the run), U8 layout (1-based),
 *               U8 block }; cells 512 x 256 x 512 units, 25 high
 *   its library LBA_BLL.HQR entry n, as LBA2's (FLBALibrary); bricks LBA_BRK.HQR entry brick - 1, as LBA2's sprites
 *   markers     RESS.HQR entry 8: one per scene, by scene number, { S16 angle x, angle y, size, text }, 1024 per turn
 *
 * Its grids hold the same cells as LBA2's interiors (FLBAGrid), its libraries the same layouts (FLBALibrary).
 *
 * An exit (zone type 0) into scene `number` puts the hero at its info0..2 from where he stood: across an edge, the
 * two scenes' frames are min - info apart.
 */
namespace LBA1
{
	/** Scene zone type of an exit into another scene. */
	constexpr int32 ZoneCube = 0;
	/** Block code of water bricks. */
	constexpr uint8 WaterCode = 0xF1;
	/** Scenes' cells (units) across and up; a scene is 64 cells wide. */
	constexpr int32 CellSize = 512;
	constexpr int32 CellHeight = 256;
	constexpr int32 SceneCells = 64;

	struct FZone
	{
		FIntVector Min = FIntVector::ZeroValue;
		FIntVector Max = FIntVector::ZeroValue;
		int32 Type = 0;
		int32 Number = 0;
		FIntVector Info = FIntVector::ZeroValue;
	};

	/** File3D of the ferry, docked in the ferry ports (Citadel 6, Principal 24, Proxima 42, desert 39). */
	constexpr int32 FerryFile3D = 27;

	/** An actor of a scene (the hero not counted): its 3D model (FILE3D.HQR entry, 65535 for a sprite) and where it stands (units). */
	struct FActor
	{
		int32 File3D = 0;
		FIntVector Position = FIntVector::ZeroValue;
	};

	struct FScene
	{
		int32 Index = INDEX_NONE;
		/** The text bank, which is the island: 0 Citadel, 1 Principal, 2 desert, 3 Proxima, 4 Rebellion... */
		int32 Bank = 0;
		TArray<FActor> Actors;
		TArray<FZone> Zones;

		bool Load(const TArray<uint8>& Data, int32 InIndex);
	};

	/** A scene's grid seen from above: per column (z * 64 + x), its top brick's cell height (-1 when empty), color, water. */
	struct FPlan
	{
		TArray<int32> Heights;
		TArray<FVector3f> Colors;
		TArray<bool> Water;
	};

	/** A grid cell's layout block in its library, null when empty. */
	const FLBALayoutBlock* GetBlock(const FLBALibrary& Library, const FLBAGridCell& Cell);

	/** LBA1's data folder and files. */
	class FData
	{
	public:
		/** Opens the files in Directory (or a known install when it is empty); false when they aren't there. */
		bool Open(const FString& Directory);
		const FString& GetDirectory() const { return Folder; }

		bool LoadScene(int32 Index, FScene& Out) const;
		/** Scene Index's grid (its library is the scene's: LibraryIndex = Index). */
		bool LoadGrid(int32 Index, FLBAGrid& Out) const;
		bool LoadLibrary(int32 Index, FLBALibrary& Out) const;
		/** A brick's sprite (1-based, as layouts' blocks name them). */
		bool LoadBrick(int32 Brick, FLBABrickSprite& Out) const;
		/** A brick with nothing to draw: its cell is only collision, and what's under it shows. */
		bool IsBlankBrick(int32 Brick) const;
		/** LBA1's palette (sRGB). */
		TArray<FColor> GetPalette() const;
		bool LoadPlan(int32 Index, FPlan& Out) const;
		/** The holomap's markers, by scene: (latitude, longitude) in degrees, on the frame of LBA2's holomap texture. */
		TArray<FVector2D> LoadMarkers() const;

	private:
		/** Mean color (sRGB) of a brick's top face: the upper diamond of its iso sprite. */
		FVector3f GetTopColor(int32 Brick) const;

		FString Folder;
		FLBAHqrFile Scenes;
		FLBAHqrFile Grids;
		FLBAHqrFile Libraries;
		FLBAHqrFile Bricks;
		FLBAHqrFile Ress;
		TArray<FVector3f> Palette;
		mutable TMap<int32, FVector3f> TopColors;
		mutable TMap<int32, bool> BlankBricks;
	};
}
