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
 * tunnel), a rectangle gets a cloister vault (creased along its diagonals), a wide floor a coved, level crown. The
 * footprint covers the grid and Margin columns round it (a roof may reach past the grid's edge).
 */
class FLBAVault
{
public:
	FLBAVault() = default;
	/** Footprint: (SizeXZ + 2 Margin)^2 columns, from (-Margin, -Margin). */
	FLBAVault(const TBitArray<>& InFootprint, const FLBAVaultShape& InShape, int32 InSeed = 0, int32 InMargin = 0);

	const FLBAVaultShape& GetShape() const { return Shape; }
	int32 GetMargin() const { return Margin; }
	bool Contains(const FIntPoint& Column) const;
	/** Distance from a point (X, Z in columns) to the nearest column outside the footprint, at most Radius. */
	float GetDistance(float X, float Z) const;
	/** Height over the spring at a point, in cells (bumps included). */
	float GetRise(float X, float Z) const;
	/** The cove alone: height over the spring at a distance from the supports, in cells. */
	float GetCove(float Distance) const;

private:
	TBitArray<> Footprint;
	FLBAVaultShape Shape;
	int32 Seed = 0;
	int32 Margin = 0;
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
 * Ledges    where the ground ends over nothing behind a railing: going out from the hollow past railings and posts on its
 *           edge (FSettings::RailingLayouts, PillarLayouts; LedgeLook columns at most), the original drew nothing, or the
 *           grid ends. The railings join the hollow; they hold nothing. So do railings beside the hollow no step out
 *           reaches (a ledge's corner post, a fence between two floors). (A rock rim is a wall the original cut down, a
 *           bare edge the end of the world: no ledge.)
 * Canyon    past the ledges the cave goes on: the empty columns within CanyonWidth of a ledge (past the grid's edge
 *           too) are under the roof, and the canyon's floor drops from the foot of the ledge (the bottom of what the
 *           original drew under its edge) CanyonDepth cells and climbs back up to the roof's spring at its far side.
 *           It falls into darkness with the depth under the ledge.
 * Pillars   a pillar layout (FSettings::PillarLayouts) standing free in the hollow, PillarMin cells over its ground at
 *           least, PillarSize columns across at most, PillarLedgeClear columns away from any ledge (there it is a post):
 *           the roof comes down onto its top in a fan.
 * Arches    two pillars at most ArcadeSpan columns apart, with no pillar nearer both of them and the hollow's air between
 *           them, carry an arch from top to top; the roof rises from it on both sides.
 * Roof      a vault (FLBAVault) over the hollow's columns but the supports, and the canyon, from the spring; under the
 *           fans and arches where they are lower.
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
		/** Layouts that are pillars. */
		TSet<int32> PillarLayouts;
		/** Layouts that are railings. */
		TSet<int32> RailingLayouts;
	};

	/** The edge of a ledge: between Column (the hollow's) and the next column on Side (the canyon's). */
	struct FLedge
	{
		FIntPoint Column;
		FIntPoint Side;
		/** Height of the ground at the edge (cells). */
		int32 Ground = 0;
		/** The bottom of what the original drew under the edge: where the canyon's near wall ends (cells). */
		int32 Foot = 0;
	};

	struct FPillar
	{
		TArray<FIntPoint> Columns;
		FIntPoint Min, Max;
		/** Its top: the roof comes down to it (cells). */
		float Top = 0.f;
		FVector2f GetCenter() const { return FVector2f(Min.X + Max.X + 1, Min.Y + Max.Y + 1) * 0.5f; }
	};

	/** An arch between two pillars (indices in GetPillars). */
	struct FArch
	{
		int32 A = 0, B = 0;
	};

	static constexpr int32 Headroom = 4;
	static constexpr int32 MinFloors = 16;
	static constexpr int32 MinInner = 4;
	static constexpr int32 Close = 3;
	static constexpr int32 Reach = 2;
	/** Columns the spring is smoothed over (a roof following the ground steps no more steeply than this). */
	static constexpr int32 SpringBlur = 4;
	static constexpr int32 LedgeLook = 3;
	static constexpr int32 CanyonWidth = 24;
	static constexpr int32 CanyonDepth = 36;
	/**
	 * Light on the canyon's rock (vertex shade, 1: as drawn) at its ledges' ground and above (no lamp reaches it: it glows a
	 * little brighter instead), and at its bottom.
	 */
	static constexpr float CanyonLit = 2.f;
	static constexpr float CanyonDark = 0.06f;
	static constexpr int32 PillarMin = Headroom + 1;
	static constexpr int32 PillarSize = 2;
	static constexpr int32 PillarLedgeClear = 2;
	static constexpr float ArcadeSpan = 12.f;
	/** How high an arch rises toward the crown over the pillars' tops (0: flat, 1: up to the crown). */
	static constexpr float ArchRise = 0.6f;
	/** Columns round the grid the hollow may reach (its canyons). */
	static constexpr int32 Margin = CanyonWidth;
	static constexpr int32 SizeXZ = FLBARoomPlan::SizeXZ;
	static constexpr int32 SizeY = FLBARoomPlan::SizeY;

	/** Exits: the scene-change zones (cells); their walkable floor is the hollow's, however small (the way out). */
	FLBAHollow(const FLBAGridCells& Cells, const FSettings& Settings, TConstArrayView<FLBACellBox> Exits = {});

	bool IsEmpty() const { return NumUnderRoof == 0; }
	/** In the hollow's footprint (supports included). */
	bool IsInHollow(const FIntPoint& Column) const;
	/** Under the roof: in the hollow and no support, or in a canyon (in the grid or round it). */
	bool IsUnderRoof(const FIntPoint& Column) const;
	/** In a canyon past a ledge (in the grid or round it). */
	bool IsInCanyon(const FIntPoint& Column) const;
	/** A column of a pillar that holds the roof. */
	bool IsPillar(const FIntPoint& Column) const;
	/** Height of the canyon's floor at a point, in cells (meaningful in and beside the canyon). */
	float GetCanyonFloor(float X, float Z) const;
	/** Light in the canyon at a point (cells): CanyonLit down to its nearest ledge's ground, fading to CanyonDark at its bottom. */
	float GetCanyonLight(float X, float Y, float Z) const;
	/** The lowest drawn cell of the column, or INDEX_NONE. */
	int32 GetDrawnBottom(const FIntPoint& Column) const;
	TConstArrayView<FLedge> GetLedges() const { return Ledges; }
	TConstArrayView<FPillar> GetPillars() const { return Pillars; }
	TConstArrayView<FArch> GetArches() const { return Arches; }
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
	/** The hollow as saved (FLBAGridPlan): its footprint's columns, their floor, spring and roof at their center. */
	void ToGridPlan(FLBAGridPlanHollow& Out) const;

	static bool IsInGrid(const FIntPoint& Column) { return FLBARoomPlan::IsInGrid(Column); }
	static int32 ToColumnIndex(const FIntPoint& Column) { return FLBARoomPlan::ToColumnIndex(Column); }
	/** The grid and Margin columns round it. */
	static bool IsInDomain(const FIntPoint& Column)
	{
		return Column.X >= -Margin && Column.Y >= -Margin && Column.X < SizeXZ + Margin && Column.Y < SizeXZ + Margin;
	}
	static int32 ToDomainIndex(const FIntPoint& Column) { return (Column.Y + Margin) * (SizeXZ + 2 * Margin) + Column.X + Margin; }

private:
	/** The nearest ledge's edge to a point, and the distance to it (columns); INDEX_NONE without ledges. */
	int32 FindNearestLedge(float X, float Z, float& OutDistance) const;
	/** The fans and arches over a point: the lowest, or a huge value away from any (cells). */
	float GetSkeletonRoof(float X, float Z) const;

	FSettings Settings;
	TArray<int32> Floors;
	TArray<int32> DrawnTops;
	TArray<int32> DrawnBottoms;
	/** Over the domain (the grid and Margin columns round it). */
	TArray<float> Springs;
	TBitArray<> InHollow;
	TBitArray<> Supports;
	/** Over the domain. */
	TBitArray<> InCanyon;
	TBitArray<> PillarColumns;
	TArray<FLedge> Ledges;
	TArray<FPillar> Pillars;
	TArray<FArch> Arches;
	FLBAVault Vault;
	int32 NumUnderRoof = 0;
};
