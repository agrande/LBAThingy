#pragma once

#include "CoreMinimal.h"

struct FLBADoorway;

/** What lies past the side of a room's footprint column. */
enum class ELBARoomEdge : uint8
{
	/** A wall column (FLBARoomModel::IsWall). */
	Wall,
	/** A doorway's column. */
	Door,
	/** Another room's footprint (a step up or down to it, stairs). */
	Room,
	/** Another room's wall that is no wall from this room's floor (the side of a step up to a raised room). */
	Rise,
	/** Nothing to close: a drop, a void, the grid's edge, a railing. */
	Open,
};

/** One side of a footprint column on the room's border. */
struct FLBARoomEdge
{
	FIntPoint Column = FIntPoint::ZeroValue;
	/** Index into FLBARoomModel::Sides: the neighbor is Column + Sides[Side]. */
	int32 Side = 0;
	ELBARoomEdge Kind = ELBARoomEdge::Open;
	/** The other room (Room), the doorway (Door). */
	int32 Other = INDEX_NONE;
};

/** A room: a floor region around one level, the furniture on it, and one ceiling. */
struct FLBARoom
{
	/** Floor height (cell) the room is measured from. */
	int32 Level = 0;
	/** First cell above the room's air: RoomHeight cells over the level. */
	int32 Ceiling = 0;
	/** Walkable floor cells of the room per column (their height, Level +- 1). */
	TMap<FIntPoint, int32> Floors;
	/** Its floors and the furniture among them. */
	TSet<FIntPoint> Footprint;
	/** Columns of stairs in the footprint, whose ceiling follows their steps (higher than the room's). */
	TMap<FIntPoint, int32> StairCeilings;
	TArray<FLBARoomEdge> Edges;

	int32 GetCeiling(const FIntPoint& Column) const;
	/** Floor height of a footprint column: its own floor, else the room's level. */
	int32 GetFloor(const FIntPoint& Column) const;
};

/** Bricks of a library seen in its walls, furniture and floors (FLBARoomModel::CountBricks over its grids). */
struct FLBAWallBrickCounts
{
	TMap<int32, int32> Wall;
	TMap<int32, int32> Furniture;
	TMap<int32, int32> Floor;

	/** Bricks seen in walls at least twice and four times as often as in furniture and floors. */
	TSet<int32> GetWallBricks() const;
};

/**
 * Rooms of an isometric grid, read from its cells (collision shape and art of each of its 64x25x64 cells) and its
 * doorways (FLBARoomAnalysis::FindDoorways). Prototyped in the plugin's Tools/room_model.py (plans per grid).
 *
 * - Floors: solid cells with Headroom empty cells above, not drawn with a wall brick (the tops of walls), joined by
 *   steps of one cell into spaces of at least MinSpaceCells.
 * - Plateaus: the floors of a space around one level (within one cell of it, its most common height first). Plateaus
 *   with MinRoomFloors floors (MinInnerFloors of them with floor all around) hold rooms; smaller ones are stairs and
 *   furniture tops. Stairs: small plateaus touching room plateaus of two levels.
 * - Rooms: from a plateau's floors, the columns with something standing on its floor (furniture), not walls, not
 *   doorways, not the floors of other room plateaus; each connected part is a room.
 * - Walls: columns with WallHeight solid cells over the level, either cut down (CutCollision collision cells above
 *   what is drawn), or the jamb of an exit, or drawn mostly with wall bricks (or not drawn at all, invisible
 *   collision), or reaching TallWall (in themselves or through the band one column out that tops a camera-side wall's
 *   stub) with the outside behind them (without wall bricks: reaching TallWall). A wall goes on:
 *   another wall column next to it, or the outside behind it; a lone column inside a room is a pillar or furniture.
 *   Met from a room, a column standing against the wall behind it (IsAgainstWall: lower than that wall) is furniture:
 *   a bookcase, a lamp, curtains, shelves; the room's wall is the one behind. Runs go through windows (WindowGap).
 * - Pockets of floor at the room's level behind furniture (too small to be spaces) are part of the room.
 * The rules beyond the height rules apply with learned wall bricks only (the height rules learn them).
 * - Doorways: the room goes around those that lead nowhere else (a gap between furniture): they are part of it.
 * - Gaps between rooms that none reaches (a stairwell's top) belong to the highest room around them.
 * - Ceilings: RoomHeight cells over a room's level; over stairs, RoomHeight over each step.
 */
class FLBARoomModel
{
public:
	static constexpr int32 SizeXZ = 64;
	static constexpr int32 SizeY = 25;
	static constexpr int32 Headroom = 4;
	static constexpr int32 MinSpaceCells = 16;
	static constexpr int32 MinRoomFloors = 16;
	static constexpr int32 MinInnerFloors = 4;
	/** Floor columns a room needs (smaller parts are corners cut off by furniture). */
	static constexpr int32 MinRoomColumns = 4;
	static constexpr int32 WallHeight = 3;
	static constexpr int32 TallWall = 6;
	/** Collision cells above the drawn top of a wall the original cut down for the camera. */
	static constexpr int32 CutCollision = 2;
	/** Furniture columns in front of a wall (IsAgainstWall). */
	static constexpr int32 AgainstDepth = 3;
	/** Empty cells of a window in a wall (GetWallRun). */
	static constexpr int32 WindowGap = 2;
	/** -X, +X, -Z, +Z. */
	static const FIntPoint Sides[4];

	/** Shape (collision, 0 = none) and Brick (0 = none) of each cell, HasArt of a brick (blank bricks have none). Empty
	 * WallBricks: the height rules only. */
	FLBARoomModel(TFunctionRef<int32(int32, int32, int32)> Shape, TFunctionRef<int32(int32, int32, int32)> Brick,
		TFunctionRef<bool(int32)> HasArt, TConstArrayView<FLBADoorway> Doorways, int32 RoomHeight, const TSet<int32>& WallBricks);

	const TArray<FLBARoom>& GetRooms() const { return Rooms; }
	/** Wall columns met around the rooms, with the level they were judged from. */
	const TMap<FIntPoint, int32>& GetWalls() const { return Walls; }
	/** The doorway at a column and its floor, if any. */
	const TPair<int32, int32>* FindDoorColumn(const FIntPoint& Column) const { return DoorColumns.Find(Column); }

	bool IsSolid(int32 X, int32 Y, int32 Z) const;
	int32 GetBrick(int32 X, int32 Y, int32 Z) const;
	/** Solid cells stacked right above height Y in column (X, Z); OutDrawnTop: cells up to the highest drawn one of them. */
	int32 GetRun(int32 X, int32 Y, int32 Z, int32* OutDrawnTop = nullptr) const;
	/** GetRun through windows: gaps of at most WindowGap cells with the column solid again above them. */
	int32 GetWallRun(int32 X, int32 Y, int32 Z, int32* OutDrawnTop = nullptr) const;
	/** A wall rises from floor level Level in the column, and goes on (see the class comment); met from a room going
	 * Sides[Side], not standing against the wall behind it. */
	bool IsWall(const FIntPoint& Column, int32 Level, int32 Side = INDEX_NONE) const;

	/** Adds the bricks of this grid's back walls, free-standing furniture and floors (to learn a library's wall bricks). */
	void CountBricks(FLBAWallBrickCounts& Counts) const;

	/** One line per room, for logs. */
	FString Describe() const;

private:
	static bool IsInGrid(const FIntPoint& Column) { return Column.X >= 0 && Column.Y >= 0 && Column.X < SizeXZ && Column.Y < SizeXZ; }
	static int32 ToIndex(int32 X, int32 Y, int32 Z) { return (Z * SizeXZ + X) * SizeY + Y; }
	bool IsWallLike(const FIntPoint& Column, int32 Level) const;
	/** The column, met from a room going Sides[Side], stands against a wall (furniture: a bookcase, a lamp, a bed's
	 * post): going on in Side through at most AgainstDepth wall-like columns, the last one is the wall: it has art (not
	 * only a camera-side wall's invisible plug or blank band), it is part of a wall across Side, and it reaches higher
	 * than the columns in front of it (a wall seen end-on is as tall). Only with learned wall bricks. */
	bool IsAgainstWall(const FIntPoint& Column, int32 Level, int32 Side) const;
	/** Nothing stands in the column from floor level Level up (the outside, a drop), or it is beyond the grid. */
	bool IsOutside(const FIntPoint& Column, int32 Level) const;
	/** Top of the wall surface rising above height Y: the column's run, continued by a band over it next door. */
	int32 GetReach(const FIntPoint& Column, int32 Y) const;
	void BuildRooms(int32 Level, const TSet<FIntVector>& Plateau);

	TBitArray<> Solid;
	TArray<int32> Bricks;
	/** Cells with a brick that has art. */
	TBitArray<> Art;
	TSet<int32> WallBricks;
	int32 RoomHeight = 0;
	/** Doorway index and floor height of each doorway column. */
	TMap<FIntPoint, TPair<int32, int32>> DoorColumns;
	/** Columns of the doorways that lead out (FLBADoorway::bExit). */
	TSet<FIntPoint> ExitColumns;
	/** Levels of the room plateaus with a floor in each column. */
	TMap<FIntPoint, TArray<int32, TInlineAllocator<2>>> RoomFloorLevels;
	TMap<FIntPoint, int32> Walls;
	TArray<FLBARoom> Rooms;
};
