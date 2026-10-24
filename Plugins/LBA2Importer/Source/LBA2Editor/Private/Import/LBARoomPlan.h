#pragma once

#include "CoreMinimal.h"

/** A box of grid cells, both corners included. */
struct FLBACellBox
{
	FIntVector Min = FIntVector::ZeroValue;
	FIntVector Max = FIntVector::ZeroValue;
};

/** What a grid column is at one level of a plan (FLBARoomPlan). */
enum class ELBAColumn : uint8
{
	/** Nothing of this level: a drop, the void, another level's floor. */
	Other,
	/** Walkable floor at the level. */
	Floor,
	/** A flight of stairs' column. */
	Stairs,
	/** Something standing on the level's floor that is not a wall: furniture, a step, a post. */
	Furniture,
	Wall,
};

/** The rule that made a column a wall (or kept it from being one). */
enum class ELBAWallRule : uint8
{
	None,
	/** Collision goes on above what is drawn, or what is drawn is topped by a cut-wall frame: the original cut the wall
	 * down for the camera. */
	Cut,
	/** Next to an exit. */
	Jamb,
	/** Nothing drawn at all: invisible collision (the plugs behind exits, hidden walls). */
	Invisible,
	/** Only while learning the wall bricks: TallWall cells high. */
	Tall,
	/** Drawn mostly with the library's wall bricks. */
	Masonry,
	/** TallWall cells high with the outside behind it. */
	Exterior,
	/** Wall-like, but alone in the room: furniture. */
	Pillar,
	/** A wall column in no straight run of MinWallRun: a post (furniture, or nothing of this level). */
	Post,
	/** Furniture standing against the wall behind it. */
	Against,
};

/** What a connector joins (FLBARoomPlan). */
enum class ELBAConnector : uint8
{
	/** Rooms of two levels: stairs, with the landings and doorways at their ends. */
	Stairs,
	/** Two rooms of one level: a door, an arch. */
	Passage,
	/** One room, holding a scene-change zone: the way out. */
	Exit,
	/** One room: a niche, part of its space. */
	Alcove,
	/** No room: out of reach. */
	Pocket,
};

/** What lies past one column of a room's side. */
enum class ELBARoomSide : uint8
{
	Wall,
	/** A room of the same level with no wall between: the same space. */
	OpenSide,
	/** A connector (FLBAPlanConnector). */
	Connector,
	/** A room or floor of another level. */
	Step,
	/** A drop, the void, the grid's edge: nothing to build. */
	Open,
};

/** A room: a rectangle of a level's floor and the furniture on it. */
struct FLBAPlanRoom
{
	/** Floor height of its level. */
	int32 Level = 0;
	/** Columns covered, corners included. */
	FIntPoint Min = FIntPoint::ZeroValue;
	FIntPoint Max = FIntPoint::ZeroValue;
	int32 Floors = 0;
	int32 Furniture = 0;
	int32 Space = INDEX_NONE;
	/** First cell above its air (its space's). */
	int32 Ceiling = 0;

	bool Contains(const FIntPoint& Column) const
	{
		return Column.X >= Min.X && Column.X <= Max.X && Column.Y >= Min.Y && Column.Y <= Max.Y;
	}
};

/** Rooms of one level joined by open sides: one space under one ceiling. */
struct FLBAPlanSpace
{
	TArray<int32> Rooms;
	/** Its lowest room's level. */
	int32 Level = 0;
	/** First cell above its air. */
	int32 Ceiling = 0;
	/** Spaces of higher levels that overlook it (double height). */
	TArray<int32> OverlookedBy;
};

/** Columns no room took, grouped: what they join, and how. */
struct FLBAPlanConnector
{
	ELBAConnector Kind = ELBAConnector::Pocket;
	/** X, Z: the column; Y: the level of the piece it belongs to, or INDEX_NONE for a flight of stairs' column. */
	TArray<FIntVector> Columns;
	TArray<int32> Rooms;
	TArray<int32> Levels;
	/** Passages: the fewest columns it shares with one of its rooms. */
	int32 Width = 0;
	/** First cell above its air: its highest room's ceiling. */
	int32 Ceiling = 0;
};

/** Floors climbing one cell at a time between two levels. */
struct FLBAPlanFlight
{
	TArray<FIntVector> Cells;
	TArray<int32> Levels;
};

/** One level of a plan: what each column is, and the wall lines. */
struct FLBAPlanLevel
{
	int32 Height = 0;
	TArray<ELBAColumn> Columns;
	TArray<ELBAWallRule> Rules;
	/** Straight runs of wall columns (MinWallRun or more). */
	TArray<TArray<FIntPoint>> Runs;
	/** Floor between two walls in line: openings in a wall's line. */
	TSet<FIntPoint> Gaps;
	/** Floor on a partition's line beside its end. */
	TSet<FIntPoint> PartitionLines;

	ELBAColumn Get(const FIntPoint& Column) const;
	ELBAWallRule GetRule(const FIntPoint& Column) const;
};

/** Read access to a grid's cells. */
struct FLBAGridCells
{
	/** Collision shape (0: none). */
	TFunction<int32(int32 X, int32 Y, int32 Z)> Shape;
	/** Brick drawn (0: none). */
	TFunction<int32(int32 X, int32 Y, int32 Z)> Brick;
	/** Layout of the cell (INDEX_NONE: none, invisible collision). */
	TFunction<int32(int32 X, int32 Y, int32 Z)> Layout;
	/** Height of the first cell of the cell's layout instance. */
	TFunction<int32(int32 X, int32 Y, int32 Z)> LayoutBase;
	/** A brick has art (some bricks are blank: they block the camera-side walls). */
	TFunction<bool(int32 Brick)> HasArt;
	/** The cell is a cut-wall frame (a cap or curved rim the original put on the walls it cut down: CutFrames). */
	TFunction<bool(int32 X, int32 Y, int32 Z)> IsFrame;
};

/** Bricks of a library seen in its walls, furniture and floors (FLBARoomPlan::CountBricks over its grids). */
struct FLBAWallBrickCounts
{
	TMap<int32, int32> Wall;
	TMap<int32, int32> Furniture;
	TMap<int32, int32> Floor;

	/** Bricks seen in walls at least twice and four times as often as in furniture and floors. */
	TSet<int32> GetWallBricks() const;
};

/**
 * The plan of an isometric grid (64x25x64 cells): its rooms, the openings and stairs between them, its walls. The
 * enclosure is built from it. Prototyped, rule for rule, in the plugin's Tools/room_plan.py (top-down plans per grid).
 *
 * Every rule is a human architectural concept, applied in this order (axes: X, Z horizontal, Y up; the iso camera
 * looks from +X +Z):
 *
 * Levels      floor heights with a connected floor of at least MinLevelFloors cells, MinInnerFloors of them with floor
 *             on all four sides: the storeys (smaller floors are furniture tops and steps; one-cell strips are ledges
 *             and the tops of walls).
 * Stairs      R1 STAIRS: floors at no level's height that climb one cell at a time from a floor of one level to a
 *             floor of another: a flight of stairs.
 * Columns     at each level, each column is a Floor, a flight's Stairs, Furniture (standing on the level's floor: its
 *             first drawn layout starts at the floor or above, so a lower storey's wardrobe is no furniture of the
 *             storey above; and not another storey's floor: a level that holds no room is no storey, so the plan is
 *             made again without it), a Wall (see below), or Other.
 * Posts       a wall column in no straight run of MinWallRun wall columns is a post: furniture if it stands on the
 *             level's floor, else nothing of this level (the end of a lower storey's wall).
 * Wall lines  R2 GAP: the floor between two walls in line (a wall that stops and goes on) is an opening in that line:
 *             a doorway, a window bay, an arch. No room's.
 *             R3 PARTITION: a wall with at least MinPartition columns of its own that ends inside the floor goes on as
 *             a line across it to the next wall: the opening beside its end is no room's either.
 * Rooms       R4 RECTANGLE: a room is the largest rectangle of floor and furniture off the wall lines, at least
 *             MinRoomWidth columns across, holding MinFloors floor cells, with a wall along one of its sides or a step
 *             to a split level beside it (with neither it
 *             is the top of something: a roof, wall tops), not standing on walls (more than half of its floor on the
 *             walls of a lower storey: a roof, a hull's top; an upper storey stands over the rooms below), on its
 *             storey (more than half of its floor on the level's connected floor, not a patch at the same height: a
 *             ledge, a rim), enclosed
 *             (more than half of its border open, nothing walkable and no wall there, is open ground); then the
 *             largest among the columns left, and so on. A floor that is no rectangle (an L) is several rooms sharing
 *             open sides.
 * Connectors  the columns no room took, per level by contact, joined through the flights they lead onto: Stairs
 *             (rooms of two levels), Passage (two rooms of one level), Exit (one room, a scene-change zone), Alcove
 *             (one room), Pocket (none).
 * Spaces      rooms joined by open sides, or by a step to a split level (at most Headroom up or down, no wall between:
 *             a sunken lounge, a dais), under one ceiling: their highest room's level + RoomHeight cells of air.
 *             R5 DOUBLE HEIGHT: a space overlooked by a higher storey (its floor's edge, more than Headroom cells up,
 *             borders a floor column of the space open from the floor up past it: a gallery, a mezzanine) takes that
 *             storey's ceiling. Closer storeys are split levels (above).
 *             Connectors take their highest room's ceiling.
 *
 * What is a wall: a column with WallHeight solid cells over the level (through window gaps) that is cut (collision above
 * what is drawn, or topped by a cut-wall frame), invisible,
 * masonry, exterior or a jamb (ELBAWallRule), and goes on (a wall-like column next to it, or the outside). A masonry
 * or exterior column met from a floor is furniture against the wall when the wall-like columns going on away from
 * that floor end, within AgainstDepth, at a drawn, taller wall that is part of a wall across.
 *
 * Learning the wall bricks (no WallBricks): the height rules alone (cut, invisible, jamb, tall); CountBricks then
 * counts the bricks of the back-most walls, of free-standing furniture and of floors.
 */
class FLBARoomPlan
{
public:
	static constexpr int32 SizeXZ = 64;
	static constexpr int32 SizeY = 25;
	static constexpr int32 Headroom = 4;
	static constexpr int32 MinLevelFloors = 16;
	static constexpr int32 MinInnerFloors = 4;
	static constexpr int32 MinFloors = 12;
	static constexpr int32 MinRoomWidth = 2;
	static constexpr int32 WallHeight = 3;
	static constexpr int32 TallWall = 6;
	/** Collision cells above the drawn top of a wall the original cut down for the camera. */
	static constexpr int32 CutCollision = 2;
	static constexpr int32 AgainstDepth = 3;
	/** Empty cells of a window in a wall. */
	static constexpr int32 WindowGap = 2;
	static constexpr int32 MinWallRun = 3;
	static constexpr int32 MinPartition = 4;
	/** Passages up to this wide are doors, wider ones arches. */
	static constexpr int32 DoorWidth = 2;
	/** -X, +X, -Z, +Z. */
	static const FIntPoint Sides[4];

	/** WallBricks null: learning them (CountBricks). */
	FLBARoomPlan(const FLBAGridCells& Cells, const TSet<int32>* WallBricks, TConstArrayView<FLBACellBox> ExitZones, int32 RoomHeight);

	const TArray<int32>& GetLevels() const { return LevelHeights; }
	/** The level of a floor height, or nullptr. */
	const FLBAPlanLevel* FindLevel(int32 Height) const;
	const TArray<FLBAPlanRoom>& GetRooms() const { return Rooms; }
	const TArray<FLBAPlanSpace>& GetSpaces() const { return Spaces; }
	const TArray<FLBAPlanConnector>& GetConnectors() const { return Connectors; }
	const TArray<FLBAPlanFlight>& GetFlights() const { return Flights; }
	/** The room of a level covering a column, or INDEX_NONE. */
	int32 FindRoom(int32 Level, const FIntPoint& Column) const;
	/** The connector holding a column at a level (Level INDEX_NONE: a flight's column), or INDEX_NONE. */
	int32 FindConnector(int32 Level, const FIntPoint& Column) const;
	/** Levels whose connected floor (a storey's) covers the column. */
	TConstArrayView<int32> GetFloorLevels(const FIntPoint& Column) const;
	/** What lies past a column on a room's side (Side into Sides); OutOther: the room or connector there. */
	ELBARoomSide GetSide(int32 Room, const FIntPoint& Column, int32 Side, int32* OutOther = nullptr) const;

	bool IsSolid(int32 X, int32 Y, int32 Z) const;
	int32 GetBrick(int32 X, int32 Y, int32 Z) const;
	/** Solid cells stacked right above height Y in the column, through window gaps; OutDrawnTop: up to the highest drawn. */
	int32 GetWallRun(int32 X, int32 Y, int32 Z, int32* OutDrawnTop = nullptr) const;

	/** Adds the bricks of the rooms' back-most walls, free-standing furniture and floors (to learn the wall bricks). */
	void CountBricks(FLBAWallBrickCounts& Counts) const;
	/** The plan as text: rooms with their sides, spaces, connectors (as Tools/room_plan.py prints it). */
	FString Describe() const;

	static bool IsInGrid(const FIntPoint& Column) { return Column.X >= 0 && Column.Y >= 0 && Column.X < SizeXZ && Column.Y < SizeXZ; }
	static int32 ToColumnIndex(const FIntPoint& Column) { return Column.Y * SizeXZ + Column.X; }

private:
	class FLevelRules;

	static int32 ToIndex(int32 X, int32 Y, int32 Z) { return (Z * SizeXZ + X) * SizeY + Y; }
	void BuildRooms(FLBAPlanLevel& Level, const FLevelRules& Rules);
	void BuildConnectors();
	void BuildSpaces();
	bool IsInZone(const FIntPoint& Column, int32 Level) const;

	TBitArray<> Solid;
	TArray<int32> Bricks;
	int32 RoomHeight = 0;
	TArray<FLBACellBox> Zones;
	TArray<int32> LevelHeights;
	/** Levels that hold no room: no storeys (their floors keep no furniture off the storeys below). */
	TSet<int32> Roomless;
	TArray<FLBAPlanLevel> Levels;
	TMap<FIntPoint, TArray<int32, TInlineAllocator<2>>> FloorLevels;
	TArray<FLBAPlanFlight> Flights;
	TArray<FLBAPlanRoom> Rooms;
	TArray<FLBAPlanSpace> Spaces;
	TArray<FLBAPlanConnector> Connectors;
	/** (X, level, Z) -> room. */
	TMap<FIntVector, int32> RoomAt;
	/** (X, level or INDEX_NONE, Z) -> connector. */
	TMap<FIntVector, int32> ConnectorAt;
};
