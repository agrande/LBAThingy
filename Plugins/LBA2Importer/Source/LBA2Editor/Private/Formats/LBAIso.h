#pragma once

#include "CoreMinimal.h"

class FLBAHqrFile;

/**
 * Isometric interiors (LBA_BKG.HQR). See SOURCES/GRILLE.CPP.
 *
 * Entry 0 is T_BKG_HEADER, which gives the index ranges of grids (GRI), grid overlays (GRM),
 * layout libraries (BLL) and brick sprites (BRK). The entry after the bricks maps scenes to grids.
 * A grid is 64x25x64 bricks of 512x256x512 units. Each brick references a block of a library layout,
 * and each layout block references a 48x38 pre-rendered isometric brick sprite.
 */
struct FLBABkgHeader
{
	int32 GridStart = 1;
	int32 GrmStart = 149;
	int32 LibraryStart = 179;
	int32 BrickStart = 197;
	int32 NumBricks = 0;
	int32 ForbiddenBrick = 0;

	bool Load(const FLBAHqrFile& Bkg);
	int32 GetSceneMapIndex() const { return BrickStart + NumBricks; }
	int32 GetNumGrids() const { return GrmStart - GridStart; }
	int32 GetNumLibraries() const { return BrickStart - LibraryStart; }
};

struct FLBALayoutBlock
{
	/** Collision shape: 0 none, 1 full, 2..5 slopes, 6..13 diagonal half slopes. */
	uint8 Shape = 0;
	/** Low nibble: footstep sound. High nibble: ground code (water, lava...). */
	uint8 Code = 0;
	/** 1-based global brick index (0 = no graphic). */
	uint16 Brick = 0;

	int32 GetSound() const { return Code & 0xF; }
	int32 GetGroundCode() const { return (Code >> 4) & 0xF; }
};

struct FLBALayout
{
	int32 Index = 0;
	int32 SizeX = 1;
	int32 SizeY = 1;
	int32 SizeZ = 1;
	/** Blocks indexed z * SizeX * SizeY + y * SizeX + x. */
	TArray<FLBALayoutBlock> Blocks;

	FIntVector GetBlockCoords(int32 BlockIndex) const
	{
		return FIntVector(BlockIndex % SizeX, (BlockIndex / SizeX) % SizeY, BlockIndex / (SizeX * SizeY));
	}
};

struct FLBALibrary
{
	int32 Index = 0;
	TArray<FLBALayout> Layouts;

	bool Load(const TArray<uint8>& Data, int32 InIndex);
};

struct FLBAGridCell
{
	/** 0-based layout index, INDEX_NONE when empty. */
	int16 Layout = INDEX_NONE;
	uint8 Block = 0;

	bool IsEmpty() const { return Layout < 0; }
};

struct FLBAGrid
{
	int32 Index = 0;
	int32 LibraryIndex = 0;
	int32 GrmIndex = 0;
	/** 64 * 25 * 64 cells, indexed (z * 64 + x) * 25 + y. */
	TArray<FLBAGridCell> Cells;

	bool Load(const TArray<uint8>& Data, int32 InIndex);

	static int32 CellIndex(int32 X, int32 Y, int32 Z) { return (Z * 64 + X) * 25 + Y; }
	const FLBAGridCell& Get(int32 X, int32 Y, int32 Z) const { return Cells[CellIndex(X, Y, Z)]; }
	bool IsInside(int32 X, int32 Y, int32 Z) const { return X >= 0 && X < 64 && Y >= 0 && Y < 25 && Z >= 0 && Z < 64; }
};

/** A decoded 48x38 brick sprite, palette indices (0 = transparent). */
struct FLBABrickSprite
{
	static constexpr int32 Width = 48;
	static constexpr int32 Height = 38;
	TArray<uint8> Pixels;

	bool Decode(const TArray<uint8>& Data);
};

/** Scene index -> scenery mapping (entry after the bricks in LBA_BKG.HQR). */
struct FLBASceneMapEntry
{
	bool bIsIsland = false;
	int32 SceneryIndex = 0;
};

namespace LBAIso
{
	TArray<FLBASceneMapEntry> LoadSceneMap(const TArray<uint8>& Data);
}
