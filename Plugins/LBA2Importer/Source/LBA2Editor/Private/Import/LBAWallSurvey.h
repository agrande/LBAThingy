#pragma once

#include "CoreMinimal.h"
#include "LBAWallStyles.h"

struct FLBAGrid;
struct FLBALibrary;

/** A layout cell of a wall: which layout and which of its blocks. */
struct FLBAWallCell
{
	int32 Layout = INDEX_NONE;
	FIntVector Block = FIntVector::ZeroValue;
};

/**
 * A vertical run of drawn bricks in one column of a grid whose camera-facing side (+X or +Z: the side the iso camera
 * saw, the one with art) is open, built of wall panels (layouts one cell deep toward the camera) and part of a wall
 * line (at least MinLine columns in a row along the wall have a run with the same foot: not a ladder or a post).
 * Indoor runs have a floor in front of their foot, outdoor runs nothing (lower ground, the grid's edge).
 */
struct FLBAWallRun
{
	FIntPoint Column = FIntPoint::ZeroValue;
	/** Direction of the open side: (1, 0) for +X, (0, 1) for +Z. */
	FIntPoint Side = FIntPoint(1, 0);
	int32 Foot = 0;
	ELBAWallFacing Facing = ELBAWallFacing::Indoor;
	/** From the foot up. */
	TArray<FLBAWallCell> Cells;
	/** Cells whose art covers their open side (not a fence, a net, a railing). */
	int32 OpaqueCells = 0;

	/** Mostly opaque: a wall that can close a room. */
	bool IsSolid() const { return OpaqueCells * 4 >= Cells.Num() * 3; }
};

namespace LBAWallSurvey
{
	/** Shortest run that counts as a wall. */
	constexpr int32 MinHeight = 3;
	/** Fewest columns in a row a wall line has. */
	constexpr int32 MinLine = 3;

	/**
	 * Coverage: fraction of opaque art of a brick's side (1: +X, 2: +Z); 0 for no brick, the forbidden brick or a
	 * blank one. Opaque: the coverage of an opaque side.
	 */
	TArray<FLBAWallRun> FindRuns(const FLBAGrid& Grid, const FLBALibrary& Library, TFunctionRef<float(int32, int32)> Coverage, float Opaque);
}

/**
 * Finds the wall styles of a library in its grids, from the layout rows of their wall runs. The rows a run repeats
 * are the body (the original stretches walls so: lib 0 L59 foot, L32 rows 0-1 stacked 4 times, L3 cap; lib 6 L52
 * rows 0-3, row 4 twice, row 5); else the tallest layout's rows are (lib 0: L1 8-row wallpaper panel, L4 cap). Runs
 * with the same foot, body and top make one style, whatever the number of repeats.
 */
class FLBAWallStyleDetector
{
public:
	explicit FLBAWallStyleDetector(const FLBALibrary& InLibrary) : Library(InLibrary) {}

	void AddRuns(int32 GridIndex, TConstArrayView<FLBAWallRun> Runs);
	/**
	 * Styles used in at least MinUses columns, most used first; Ignored layouts never make a style, nor see-through
	 * runs. RowDistance: how different two rows of a layout look (0: alike), to pick the row a panel body stretches by
	 * (the one most like the row under it, the highest of them: a plain filler under the panel's top band).
	 */
	TArray<FLBAWallStyle> Finish(const TSet<int32>& Ignored, TFunctionRef<float(int32, int32, int32)> RowDistance) const;

	static constexpr int32 MinUses = 3;

private:
	/** A layout row. */
	using FRow = TPair<int32, int32>;

	struct FGroup
	{
		ELBAWallFacing Facing = ELBAWallFacing::Indoor;
		TArray<FRow> Foot, Body, Top;
		/** The body is rows the original stacks again (else a panel). */
		bool bRepeated = false;
		int32 Uses = 0;
		TSet<int32> Grids;
	};

	const FLBALibrary& Library;
	TMap<FString, FGroup> Groups;
};
