#pragma once

#include "CoreMinimal.h"
#include "Import/LBARoomPlan.h"

/** How a vault rises over its spring (FLBAVault). */
struct FLBAVaultShape
{
	/** How far from its supports it curves up, in columns (0: flat). */
	float Radius = 0.f;
	/** How high it rises over its spring, in cells. */
	float Rise = 0.f;
	/** Rough rock: bumps this high, in cells (0: dressed stone). */
	float Noise = 0.f;

	bool IsFlat() const { return Radius <= 0.f || Rise <= 0.f; }
};

/**
 * A vault: a roof over a footprint of columns that springs from the footprint's edge and rises with the distance to the
 * nearest column outside it (what holds it: a wall, a pillar, the rock). The rise is a cove, a quarter ellipse Radius
 * columns across and Rise cells high, then level: spans narrower than twice the radius meet in a point (a pointed arch, a
 * tunnel), a rectangle gets a cloister vault (creased along its diagonals), a wide floor a coved, level crown.
 */
class FLBAVault
{
public:
	FLBAVault() = default;
	FLBAVault(const TBitArray<>& InFootprint, const FLBAVaultShape& InShape, int32 InSeed = 0);

	const FLBAVaultShape& GetShape() const { return Shape; }
	bool Contains(const FIntPoint& Column) const;
	/** Distance from a point (X, Z in columns) to the nearest column outside the footprint (the grid's outside included), at most Radius. */
	float GetDistance(float X, float Z) const;
	/** Height over the spring at a point, in cells (bumps included). */
	float GetRise(float X, float Z) const;
	/** The cove alone: height over the spring at a distance from the supports, in cells. */
	float GetCove(float Distance) const;

private:
	TBitArray<> Footprint;
	FLBAVaultShape Shape;
	int32 Seed = 0;
};

/**
 * The hollow of a grid: the air of a cave (or of a temple carved in a mountain, of a cistern) and its roof. A cave has no
 * walls in the builder's sense: the walkable floor is a hollow in the rock, and the rock rises from the hollow's edge and
 * closes over it as one vault. Each concept is found by one rule, in this order (axes: X, Z horizontal, Y up):
 *
 * Floors    walkable floors (Headroom empty cells over them) in connected floors (stepping one cell up or down at most)
 *           of at least MinFloors cells, MinInner of them with floor all around: the ground of the hollow (smaller ones
 *           are the tops of things; one-cell strips are rims), and any walkable floor in or beside an exit's zone
 *           (the way out belongs to the hollow, however narrow its passage).
 * Hollow    the columns of those floors, closed over gaps up to 2 Close columns across (a chasm a bridge crosses, a pit, a
 *           ridge between two galleries): the hollow's footprint. Everything else is the rock (or masonry) around it.
 * Spring    where the roof starts: Clear cells over the highest floor within SpringReach columns, smoothed over
 *           SpringBlur columns. A short
 *           reach lets the roof follow the ground (a cave); a long one keeps one level over the highest floor (a hall).
 * Supports  a column of the hollow without floor whose drawn top (invisible collision aside) comes within Reach cells of
 *           the roof holds it: the roof comes down onto it (a pillar, a rock wall between galleries). Lower things (rims,
 *           railings, parapets, furniture) stand under the roof.
 * Roof      a vault (FLBAVault) over the hollow's columns but the supports, from the spring.
 */
class FLBAHollow
{
public:
	struct FSettings
	{
		int32 Clear = 6;
		float SpringReach = 2.f;
		FLBAVaultShape Shape;
		int32 Seed = 0;
	};

	static constexpr int32 Headroom = 4;
	static constexpr int32 MinFloors = 16;
	static constexpr int32 MinInner = 4;
	static constexpr int32 Close = 3;
	static constexpr int32 Reach = 2;
	/** Columns the spring is smoothed over (a roof following the ground steps no more steeply than this). */
	static constexpr int32 SpringBlur = 4;
	static constexpr int32 SizeXZ = FLBARoomPlan::SizeXZ;
	static constexpr int32 SizeY = FLBARoomPlan::SizeY;

	/** Exits: the scene-change zones (cells); their walkable floor is the hollow's, however small (the way out). */
	FLBAHollow(const FLBAGridCells& Cells, const FSettings& Settings, TConstArrayView<FLBACellBox> Exits = {});

	bool IsEmpty() const { return NumUnderRoof == 0; }
	/** In the hollow's footprint (supports included). */
	bool IsInHollow(const FIntPoint& Column) const;
	/** Under the roof: in the hollow, and no support. */
	bool IsUnderRoof(const FIntPoint& Column) const;
	/** The hollow's highest floor in the column, or INDEX_NONE. */
	int32 GetFloor(const FIntPoint& Column) const;
	/** The highest drawn cell of the column, or INDEX_NONE. */
	int32 GetDrawnTop(const FIntPoint& Column) const;
	/** Height of the roof's spring at a point (X, Z in columns), in cells. */
	float GetSpring(float X, float Z) const;
	/** Height of the roof at a point, in cells. */
	float GetRoof(float X, float Z) const;
	const FLBAVault& GetVault() const { return Vault; }
	/** The hollow as text: its size, supports, spring and roof ranges. */
	FString Describe() const;

	static bool IsInGrid(const FIntPoint& Column) { return FLBARoomPlan::IsInGrid(Column); }
	static int32 ToColumnIndex(const FIntPoint& Column) { return FLBARoomPlan::ToColumnIndex(Column); }

private:
	FSettings Settings;
	TArray<int32> Floors;
	TArray<int32> DrawnTops;
	TArray<float> Springs;
	TBitArray<> InHollow;
	TBitArray<> Supports;
	FLBAVault Vault;
	int32 NumUnderRoof = 0;
};
