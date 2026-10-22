#pragma once

#include "CoreMinimal.h"

/** A box of grid cells, both corners included. */
struct FLBACellBox
{
	FIntVector Min = FIntVector::ZeroValue;
	FIntVector Max = FIntVector::ZeroValue;
};

/** What lies at the end of an opening through a wall, in increasing order of "outside". */
enum class ELBAOpeningEnd : uint8
{
	/** A walkable floor of one of the grid's rooms. */
	Room,
	/** Invisible collision: how the original closes an exit just past its cube-change zone. */
	Plug,
	/** The grid's edge. */
	Edge,
	/** Nothing, not even a floor below. */
	Void,
	/** A lower floor (balcony edge, steps down). */
	Drop,
	/** A visible wall: not an opening. */
	Blocked,
};

/** Why an opening counts as a doorway. */
enum class ELBADoorwaySignal : uint8
{
	None = 0,
	/** Solid cells above the opening. */
	Lintel = 1 << 0,
	/** A doorway jamb layout at its side (FLBADoorwayHints::Jambs). */
	Jamb = 1 << 1,
	/** A threshold layout on its floor (FLBADoorwayHints::Thresholds). */
	Threshold = 1 << 2,
	/** A cube-change zone at it. */
	Zone = 1 << 3,
};
ENUM_CLASS_FLAGS(ELBADoorwaySignal)

/** A doorway: an opening through a wall line, between two rooms or out of the grid. */
struct FLBADoorway
{
	/** Floor cells of the opening (the passage is the space above them). */
	TArray<FIntVector> Cells;
	/** Columns (X, Z) of the walls on its sides. */
	TArray<FIntPoint> Flanks;
	/** Grid axis the opening is crossed along: 0 = X, 2 = Z. */
	int32 Axis = 0;
	/** Leads out of the grid's rooms (an invisible plug, the grid's edge, a void or a lower floor) rather than to another room. */
	bool bExit = false;
	/** The most "outside" end of its cells. */
	ELBAOpeningEnd End = ELBAOpeningEnd::Room;
	/** Width in cells, between the flanking walls. */
	int32 Width = 0;
	/** Height of the lower flanking wall, in cells above the floor: below TallWall, a wall cut down for the camera. */
	int32 FlankHeight = 0;
	/** Empty cells between the floor and the lintel; INDEX_NONE without a lintel. */
	int32 ClearHeight = INDEX_NONE;
	ELBADoorwaySignal Signals = ELBADoorwaySignal::None;

	bool IsInStub() const;
};

/** What tells a doorway from any gap in a wall, beyond its lintel. */
struct FLBADoorwayHints
{
	/** Layouts that frame doorways (jambs, arches, posts). */
	TSet<int32> Jambs;
	/** Flat layouts lying in doorways (doormats). */
	TSet<int32> Thresholds;
	/** Cube-change zones of the scenes in the grid, in cells. */
	TArray<FLBACellBox> Zones;
};

/**
 * Rooms and doorways of an isometric grid, read from its cells (collision shape, art and layout of each of its
 * 64x25x64 cells). Prototyped and checked against the scenes' doors in Saved/LookDev/tools/survey_grids.py.
 *
 * Room floors: solid cells with Headroom empty cells above them, connected (steps of one cell up or down) into
 * spaces of at least MinSpaceCells; smaller pockets behind walls are decor the camera never reaches.
 *
 * Doorways: a room floor cell with wall columns (>= WallHeight solid cells above its floor) on both sides along one
 * axis, at most MaxWidth apart, each part of a wall line (another wall column next to it, not a lone pillar). Along
 * the other axis the opening goes through the wall to a room on one side, and to a room (at most MaxDepth deep) or
 * an invisible plug, a void or the grid's edge (an exit, at most MaxExitDepth deep) on the other. Neighboring cells
 * crossed along the same axis to the same kind of place make one doorway, kept when it has a lintel, a jamb, a
 * threshold or a cube-change zone, does not only lead down to a lower floor and has at most MaxCells cells.
 */
class FLBARoomAnalysis
{
public:
	static constexpr int32 SizeXZ = 64;
	static constexpr int32 SizeY = 25;
	static constexpr int32 Headroom = 4;
	static constexpr int32 MinSpaceCells = 16;
	static constexpr int32 WallHeight = 3;
	/** Flanking walls lower than this are the walls facing the camera, cut down by the original. */
	static constexpr int32 TallWall = 5;
	static constexpr int32 MaxWidth = 5;
	static constexpr int32 MaxDepth = 3;
	static constexpr int32 MaxExitDepth = 8;
	static constexpr int32 MaxCells = 15;

	/** Shape (collision, 0 = none), Brick (art, 0 = none) and Layout (INDEX_NONE = none) of each cell. */
	FLBARoomAnalysis(TFunctionRef<int32(int32, int32, int32)> Shape, TFunctionRef<int32(int32, int32, int32)> Brick,
		TFunctionRef<int32(int32, int32, int32)> Layout);

	/** Walkable spaces: connected room floors. */
	const TArray<TArray<FIntVector>>& GetSpaces() const { return Spaces; }
	bool IsRoomFloor(const FIntVector& Cell) const { return RoomFloors.Contains(Cell); }

	bool IsSolid(int32 X, int32 Y, int32 Z) const;
	/** The cell has art (solid cells without are invisible collision). */
	bool HasBrick(int32 X, int32 Y, int32 Z) const;
	/** Solid cells stacked right above the floor cell (X, Y, Z). */
	int32 GetWallHeight(int32 X, int32 Y, int32 Z) const;

	TArray<FLBADoorway> FindDoorways(const FLBADoorwayHints& Hints) const;

	/** True when one of Cells is at Box: next to it horizontally, from two cells below it to its top. */
	static bool IsNear(TConstArrayView<FIntVector> Cells, const FLBACellBox& Box);

private:
	struct FOpening
	{
		int32 Axis = 0;
		int32 Width = 0;
		int32 FlankHeight = 0;
		int32 ClearHeight = INDEX_NONE;
		ELBAOpeningEnd Other = ELBAOpeningEnd::Room;
		TSet<int32> FlankLayouts;
		int32 FloorLayout = INDEX_NONE;
		FIntPoint Flanks[2];
	};

	static bool IsInside(int32 X, int32 Y, int32 Z) { return X >= 0 && Y >= 0 && Z >= 0 && X < SizeXZ && Y < SizeY && Z < SizeXZ; }
	static int32 ToIndex(int32 X, int32 Y, int32 Z) { return (Z * SizeXZ + X) * SizeY + Y; }
	int32 LayoutAt(int32 X, int32 Y, int32 Z) const;
	bool IsWall(int32 X, int32 Y, int32 Z, int32 Height = WallHeight) const;
	bool IsLined(int32 X, int32 Y, int32 Z, const FIntPoint& Away) const;
	ELBAOpeningEnd GetEnd(int32 X, int32 Y, int32 Z) const;
	TMap<FIntVector, FOpening> FindOpenings() const;

	TBitArray<> Solid;
	TBitArray<> Bricks;
	TArray<int16> Layouts;
	TSet<FIntVector> RoomFloors;
	TArray<TArray<FIntVector>> Spaces;
};
