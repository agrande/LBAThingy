#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "LBAEnclosure.h"
#include "LBAGridPlan.generated.h"

class UWorld;

/** What a grid column is at one level of a plan. */
UENUM(BlueprintType)
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
UENUM(BlueprintType)
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

/** What a connector joins. */
UENUM(BlueprintType)
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
UENUM(BlueprintType)
enum class ELBARoomSide : uint8
{
	Wall,
	/** A room of the same level with no wall between: the same space. */
	OpenSide,
	/** A connector. */
	Connector,
	/** A room or floor of another level. */
	Step,
	/** A drop, the void, the grid's edge: nothing to build. */
	Open,
};

/** Columns in a row along a room's side with the same thing past them. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanSideRun
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	ELBARoomSide Kind = ELBARoomSide::Open;

	/** Walls: the rule that made them walls. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	ELBAWallRule Rule = ELBAWallRule::None;

	/** Columns in the run. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Count = 0;

	/** The room past an open side or a step, the connector past a connector, else -1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Other = INDEX_NONE;
};

/** One side of a room, from its lowest column to its highest. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanSide
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FLBAGridPlanSideRun> Runs;
};

/** A room: a rectangle of a level's floor and the furniture on it, four sides. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanRoom
{
	GENERATED_BODY()

	/** Floor height of its level (cells). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Level = 0;

	/** First column covered (X, Z). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	FIntPoint Min = FIntPoint::ZeroValue;

	/** Last column covered (X, Z). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	FIntPoint Max = FIntPoint::ZeroValue;

	/** First cell above its air: its space's ceiling. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Ceiling = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Space = INDEX_NONE;

	/** Floor columns in it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Floors = 0;

	/** Furniture columns in it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Furniture = 0;

	/** The side facing -X (the iso camera's far side along X): what lies past it, from its lowest Z to its highest. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan", meta = (DisplayName = "-X Side"))
	FLBAGridPlanSide MinusX;

	/** The side facing +X (toward the iso camera): from its lowest Z to its highest. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan", meta = (DisplayName = "+X Side"))
	FLBAGridPlanSide PlusX;

	/** The side facing -Z (the iso camera's far side along Z): from its lowest X to its highest. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan", meta = (DisplayName = "-Z Side"))
	FLBAGridPlanSide MinusZ;

	/** The side facing +Z (toward the iso camera): from its lowest X to its highest. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan", meta = (DisplayName = "+Z Side"))
	FLBAGridPlanSide PlusZ;

	/** Sides by index: -X, +X, -Z, +Z (FLBAGridPlan::Sides). */
	const FLBAGridPlanSide& GetSide(int32 Side) const { return Side == 0 ? MinusX : Side == 1 ? PlusX : Side == 2 ? MinusZ : PlusZ; }
	FLBAGridPlanSide& GetSide(int32 Side) { return Side == 0 ? MinusX : Side == 1 ? PlusX : Side == 2 ? MinusZ : PlusZ; }
	bool Contains(const FIntPoint& Column) const
	{
		return Column.X >= Min.X && Column.X <= Max.X && Column.Y >= Min.Y && Column.Y <= Max.Y;
	}
};

/** Rooms of one level joined by open sides (or a split-level step): one space under one ceiling. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanSpace
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<int32> Rooms;

	/** Its lowest room's level. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Level = 0;

	/** First cell above its air. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Ceiling = 0;

	/** Spaces of higher levels that overlook it (double height: a gallery, a mezzanine). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<int32> OverlookedBy;
};

/** Columns no room took, grouped: what they join, and how. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanConnector
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	ELBAConnector Kind = ELBAConnector::Pocket;

	/** X, Z: the column; Y: the level of the piece it belongs to, or -1 for a flight of stairs' column. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FIntVector> Columns;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<int32> Rooms;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<int32> Levels;

	/** Passages: the fewest columns it shares with one of its rooms (up to 2: a door, wider: an arch). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Width = 0;

	/** First cell above its air: its highest room's ceiling. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Ceiling = 0;
};

/** Floors climbing one cell at a time between two levels. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanFlight
{
	GENERATED_BODY()

	/** The steps' floor cells (X, height, Z). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FIntVector> Cells;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<int32> Levels;
};

/** A straight run of wall columns. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanWallRun
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FIntPoint> Columns;
};

/** One level of a plan (a storey): what each column is there, and the wall lines. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanLevel
{
	GENERATED_BODY()

	/** Floor height (cells). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Height = 0;

	/** What each column is at this level, 64 x 64 (index Z * 64 + X). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<ELBAColumn> Columns;

	/** The rule that made each column a wall, or kept it from being one (index Z * 64 + X). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<ELBAWallRule> Rules;

	/** Straight runs of wall columns. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FLBAGridPlanWallRun> WallRuns;

	/** Floor between two walls in line: openings in a wall's line (doorways, window bays, arches). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FIntPoint> Gaps;

	/** Floor on a partition's line beside its end. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FIntPoint> PartitionLines;

	ELBAColumn GetColumn(const FIntPoint& Column) const;
	ELBAWallRule GetRule(const FIntPoint& Column) const;
};

/** A column of air the enclosure closes: from the cell over its floor (Floor + 1) to the first cell above its air (Top). */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanAir
{
	GENERATED_BODY()

	/** X, Z. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	FIntPoint Column = FIntPoint::ZeroValue;

	/** The level its column is judged at (for a flight's column, the highest level under its step). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Level = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Floor = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Top = 0;

	/** Whose wallpaper its walls take. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Space = INDEX_NONE;

	/** A room's column: the room, else -1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Room = INDEX_NONE;

	/** A connector's or a flight's column: the connector, else -1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Connector = INDEX_NONE;
};

/** A column of a hollow (a cave's roof over its floors). */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanHollowColumn
{
	GENERATED_BODY()

	/** X, Z. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	FIntPoint Column = FIntPoint::ZeroValue;

	/** Its highest floor of the hollow, or -1 (a chasm, a pit the roof spans). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	int32 Floor = INDEX_NONE;

	/** It holds the roof (a pillar, a rock wall between galleries): the roof comes down onto it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	bool bSupport = false;

	/** A pillar standing free under the roof: the roof comes down onto its top and arches to the next pillars. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	bool bPillar = false;

	/** Past a ledge: the canyon under the roof, its floor deep below the ledge (Floor -1, Spring and Roof as over the hollow). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	bool bCanyon = false;

	/** Height of the roof's spring over its center (cells). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	float Spring = 0.f;

	/** Height of the roof over its center (cells). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	float Roof = 0.f;
};

/** The hollow of a grid built as a cave (ELBAEnclosureStructure::Hollow). */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlanHollow
{
	GENERATED_BODY()

	/** Its footprint, supports included, and its canyons' columns in the grid. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TArray<FLBAGridPlanHollowColumn> Columns;

	/** Its size, supports, spring and roof ranges, as text. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	FString Summary;
};

/**
 * The plan of an isometric grid (64 x 25 x 64 cells): what the importer understood of it, and what the enclosure is
 * built from. Rooms (rectangles of a level's floor, four sides each), the spaces they make under one ceiling, the
 * connectors between them (stairs, doors, exits), what each column is at each level, and the air the enclosure closes;
 * or, for caves, the hollow and its roof. Axes: X, Z horizontal, Y up (cells; the iso camera looks from +X +Z).
 *
 * Saved per grid as DA_Grid_NNN_Plan, drawn in its level by an ALBAGridPlanActor and in the editor's LBA2 Plan Browser,
 * and written as text for the plugin's golden tests (Tests/Plans).
 */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAGridPlan
{
	GENERATED_BODY()

	static constexpr int32 SizeXZ = 64;
	static constexpr int32 SizeY = 25;
	/** -X, +X, -Z, +Z. */
	static const FIntPoint Sides[4];

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 Grid = INDEX_NONE;

	/** The grid's level name (L_Grid_NNN_<Name>). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	FString Name;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 Library = INDEX_NONE;

	/** The enclosure closes it (the Enhanced look); open-air grids get a plan too, but no enclosure. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	bool bEnclosed = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	ELBAEnclosureStructure Structure = ELBAEnclosureStructure::Rooms;

	/** Cells of air over a room's floor. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 RoomHeight = 0;

	/** Cells of air in a doorway without a lintel. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Grid")
	int32 DoorHeight = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rooms")
	TArray<FLBAGridPlanLevel> Levels;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rooms")
	TArray<FLBAGridPlanRoom> Rooms;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rooms")
	TArray<FLBAGridPlanSpace> Spaces;

	/** Pockets included (columns out of reach). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rooms")
	TArray<FLBAGridPlanConnector> Connectors;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rooms")
	TArray<FLBAGridPlanFlight> Flights;

	/** The air the enclosure closes, column by column. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rooms")
	TArray<FLBAGridPlanAir> Air;

	/** An exit's columns past its doorway (the passage out): left open. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rooms")
	TArray<FIntPoint> BeyondExits;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Hollow")
	FLBAGridPlanHollow Hollow;

	const FLBAGridPlanLevel* FindLevel(int32 Height) const;
	/** The room of a level covering a column, or -1. */
	int32 FindRoom(int32 Level, const FIntPoint& Column) const;
	/** The connector holding a column at a level (-1: a flight's column), or -1. */
	int32 FindConnector(int32 Level, const FIntPoint& Column) const;

	static int32 ToColumnIndex(const FIntPoint& Column) { return Column.Y * SizeXZ + Column.X; }
	static bool IsInGrid(const FIntPoint& Column) { return Column.X >= 0 && Column.Y >= 0 && Column.X < SizeXZ && Column.Y < SizeXZ; }
	/** World position of a cell corner (X, Z horizontal, Y up, in cells), as the importer places the grid's meshes. */
	static FVector CellToWorld(double X, double Y, double Z);
	/** The cell under a world position (X, Z horizontal, Y up). */
	static FVector WorldToCell(const FVector& World);
};

/** The plan of an isometric grid, saved by the importer (DA_Grid_NNN_Plan). */
UCLASS(BlueprintType)
class LBA2RUNTIME_API ULBAGridPlanAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan", meta = (ShowOnlyInnerProperties))
	FLBAGridPlan Plan;

	/** The grid's level (L_Grid_NNN_<Name>). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan")
	TSoftObjectPtr<UWorld> Level;
};
