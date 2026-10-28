#include "Import/LBAHollow.h"

namespace
{
	const FIntPoint HollowSides[4] = { FIntPoint(-1, 0), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(0, 1) };

	/** Offsets of a disk of radius R columns (rounded generously, so a radius of 1 is the 3x3 square's cross and corners). */
	TArray<FIntPoint> HollowDisk(int32 R)
	{
		TArray<FIntPoint> Offsets;
		for (int32 DX = -R; DX <= R; ++DX)
		{
			for (int32 DZ = -R; DZ <= R; ++DZ)
			{
				if (DX * DX + DZ * DZ <= R * R + R)
				{
					Offsets.Add(FIntPoint(DX, DZ));
				}
			}
		}
		return Offsets;
	}

	/** A random value in [0, 1) for a lattice point. */
	float LatticeHash(int32 X, int32 Z, int32 Seed)
	{
		uint32 H = uint32(X) * 73856093u ^ uint32(Z) * 19349663u ^ uint32(Seed) * 83492791u;
		H ^= H >> 13;
		H *= 0x5bd1e995u;
		H ^= H >> 15;
		return float(H & 0xFFFFFF) / float(0x1000000);
	}

	/** Smooth value noise in [-1, 1], one lattice point every two columns. */
	float RockNoise(float X, float Z, int32 Seed)
	{
		const float U = X * 0.5f, V = Z * 0.5f;
		const int32 IX = FMath::FloorToInt(U), IZ = FMath::FloorToInt(V);
		const float FX = FMath::SmoothStep(0.f, 1.f, U - IX), FZ = FMath::SmoothStep(0.f, 1.f, V - IZ);
		const float A = FMath::Lerp(LatticeHash(IX, IZ, Seed), LatticeHash(IX + 1, IZ, Seed), FX);
		const float B = FMath::Lerp(LatticeHash(IX, IZ + 1, Seed), LatticeHash(IX + 1, IZ + 1, Seed), FX);
		return FMath::Lerp(A, B, FZ) * 2.f - 1.f;
	}

	/** A quarter ellipse: 0 at T = 0, rising steeply, 1 (level) at T = 1. */
	float Cove01(float T)
	{
		const float U = 1.f - FMath::Clamp(T, 0.f, 1.f);
		return FMath::Sqrt(FMath::Max(0.f, 1.f - U * U));
	}

	/** Distance from a point to a segment, and where along it the point is (0 at A, 1 at B). */
	float SegmentDistance(const FVector2f& P, const FVector2f& A, const FVector2f& B, float& OutAlong)
	{
		const FVector2f AB = B - A;
		const float Length2 = AB.SizeSquared();
		OutAlong = Length2 > 0.f ? FMath::Clamp(FVector2f::DotProduct(P - A, AB) / Length2, 0.f, 1.f) : 0.f;
		return FVector2f::Distance(P, A + AB * OutAlong);
	}

	/** Distance from a point to a box of columns [Min, Max + 1]. */
	float BoxDistance(float X, float Z, const FIntPoint& Min, const FIntPoint& Max)
	{
		const float DX = FMath::Max3(Min.X - X, 0.f, X - (Max.X + 1));
		const float DZ = FMath::Max3(Min.Y - Z, 0.f, Z - (Max.Y + 1));
		return FMath::Sqrt(DX * DX + DZ * DZ);
	}
}

FLBAVault::FLBAVault(const TBitArray<>& InFootprint, const FLBAVaultShape& InShape, int32 InSeed, int32 InMargin)
	: Footprint(InFootprint)
	, Shape(InShape)
	, Seed(InSeed)
	, Margin(InMargin)
{
}

bool FLBAVault::Contains(const FIntPoint& Column) const
{
	const int32 Size = FLBARoomPlan::SizeXZ + 2 * Margin;
	const int32 X = Column.X + Margin, Z = Column.Y + Margin;
	return X >= 0 && Z >= 0 && X < Size && Z < Size && Footprint.IsValidIndex(Z * Size + X) && Footprint[Z * Size + X];
}

float FLBAVault::GetDistance(float X, float Z) const
{
	const float Radius = Shape.Radius;
	if (Radius <= 0.f)
	{
		return 0.f;
	}
	float Best = Radius;
	const int32 X0 = FMath::FloorToInt(X - Radius) - 1, X1 = FMath::CeilToInt(X + Radius);
	const int32 Z0 = FMath::FloorToInt(Z - Radius) - 1, Z1 = FMath::CeilToInt(Z + Radius);
	for (int32 CX = X0; CX <= X1; ++CX)
	{
		for (int32 CZ = Z0; CZ <= Z1; ++CZ)
		{
			if (Contains(FIntPoint(CX, CZ)))
			{
				continue;
			}
			const float DX = FMath::Max3(CX - X, 0.f, X - (CX + 1));
			const float DZ = FMath::Max3(CZ - Z, 0.f, Z - (CZ + 1));
			Best = FMath::Min(Best, FMath::Sqrt(DX * DX + DZ * DZ));
		}
	}
	return Best;
}

float FLBAVault::GetCove(float Distance) const
{
	if (Shape.IsFlat())
	{
		return 0.f;
	}
	const float T = 1.f - FMath::Clamp(Distance / Shape.Radius, 0.f, 1.f);
	return Shape.Rise * FMath::Sqrt(FMath::Max(0.f, 1.f - T * T));
}

float FLBAVault::GetRise(float X, float Z) const
{
	if (Shape.IsFlat())
	{
		return 0.f;
	}
	const float Distance = GetDistance(X, Z);
	float Rise = GetCove(Distance);
	if (Shape.Noise > 0.f)
	{
		// Bumps fade out toward the supports: the roof meets them on its spring line.
		Rise += Shape.Noise * RockNoise(X, Z, Seed) * FMath::SmoothStep(0.f, 1.5f, Distance);
	}
	return Rise;
}

FLBAHollow::FLBAHollow(const FLBAGridCells& Cells, const FSettings& InSettings, TConstArrayView<FLBACellBox> Exits)
	: Settings(InSettings)
{
	const int32 NumColumns = SizeXZ * SizeXZ;
	auto Solid = [&Cells](int32 X, int32 Y, int32 Z)
	{
		return Y >= 0 && Y < SizeY && Cells.Shape(X, Y, Z) != 0;
	};

	// Floors: walkable cells in connected floors big enough to be ground (not the tops of things).
	TSet<FIntVector> Walkable;
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y + 1 < SizeY; ++Y)
			{
				bool bFree = Solid(X, Y, Z);
				for (int32 Up = 1; Up <= Headroom && bFree; ++Up)
				{
					bFree = !Solid(X, Y + Up, Z);
				}
				if (bFree)
				{
					Walkable.Add(FIntVector(X, Y, Z));
				}
			}
		}
	}
	Floors.Init(INDEX_NONE, NumColumns);
	TSet<FIntVector> Seen;
	for (const FIntVector& Start : Walkable)
	{
		if (Seen.Contains(Start))
		{
			continue;
		}
		TArray<FIntVector> Component = { Start };
		Seen.Add(Start);
		for (int32 Index = 0; Index < Component.Num(); ++Index)
		{
			const FIntVector Cell = Component[Index];
			for (const FIntPoint& Side : HollowSides)
			{
				for (const int32 DY : { 0, 1, -1 })
				{
					const FIntVector Next(Cell.X + Side.X, Cell.Y + DY, Cell.Z + Side.Y);
					if (Walkable.Contains(Next) && !Seen.Contains(Next))
					{
						Seen.Add(Next);
						Component.Add(Next);
					}
				}
			}
		}
		TSet<FIntVector> Members;
		Members.Append(Component);
		int32 Inner = 0;
		for (const FIntVector& Cell : Component)
		{
			bool bInner = true;
			for (const FIntPoint& Side : HollowSides)
			{
				bInner = bInner && (Members.Contains(FIntVector(Cell.X + Side.X, Cell.Y, Cell.Z + Side.Y))
					|| Members.Contains(FIntVector(Cell.X + Side.X, Cell.Y + 1, Cell.Z + Side.Y))
					|| Members.Contains(FIntVector(Cell.X + Side.X, Cell.Y - 1, Cell.Z + Side.Y)));
			}
			Inner += bInner;
		}
		if (Component.Num() < MinFloors || Inner < MinInner)
		{
			continue;
		}
		for (const FIntVector& Cell : Component)
		{
			int32& Floor = Floors[ToColumnIndex(FIntPoint(Cell.X, Cell.Z))];
			Floor = FMath::Max(Floor, Cell.Y);
		}
	}

	// The way out: walkable floor in or beside an exit's zone (from a little below it: its floor), whatever its size.
	for (const FLBACellBox& Box : Exits)
	{
		for (int32 X = Box.Min.X - 1; X <= Box.Max.X + 1; ++X)
		{
			for (int32 Z = Box.Min.Z - 1; Z <= Box.Max.Z + 1; ++Z)
			{
				for (int32 Y = FMath::Max(Box.Min.Y - 2, 0); Y <= FMath::Min(Box.Max.Y, SizeY - 1); ++Y)
				{
					if (IsInGrid(FIntPoint(X, Z)) && Walkable.Contains(FIntVector(X, Y, Z)))
					{
						int32& Floor = Floors[ToColumnIndex(FIntPoint(X, Z))];
						Floor = FMath::Max(Floor, Y);
					}
				}
			}
		}
	}

	// Drawn tops and bottoms: the art the original shows (invisible collision is no rock).
	DrawnTops.Init(INDEX_NONE, NumColumns);
	DrawnBottoms.Init(INDEX_NONE, NumColumns);
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				const int32 Brick = Cells.Brick(X, Y, Z);
				if (Brick != 0 && Cells.HasArt(Brick) && !Cells.IsFrame(X, Y, Z))
				{
					DrawnTops[ToColumnIndex(FIntPoint(X, Z))] = Y;
					int32& Bottom = DrawnBottoms[ToColumnIndex(FIntPoint(X, Z))];
					Bottom = Bottom == INDEX_NONE ? Y : Bottom;
				}
			}
		}
	}

	// Hollow: the floors' columns, closed over narrow gaps (dilated, then eroded, with the grid's outside as rock).
	const TArray<FIntPoint> CloseDisk = HollowDisk(Close);
	TBitArray<> FloorColumns(false, NumColumns), Dilated(false, NumColumns);
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		FloorColumns[Index] = Floors[Index] != INDEX_NONE;
	}
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		for (const FIntPoint& Offset : CloseDisk)
		{
			const FIntPoint Near = Column + Offset;
			if (IsInGrid(Near) && FloorColumns[ToColumnIndex(Near)])
			{
				Dilated[Index] = true;
				break;
			}
		}
	}
	InHollow.Init(false, NumColumns);
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		bool bKept = true;
		for (const FIntPoint& Offset : CloseDisk)
		{
			const FIntPoint Near = Column + Offset;
			bKept = bKept && IsInGrid(Near) && Dilated[ToColumnIndex(Near)];
		}
		InHollow[Index] = bKept || FloorColumns[Index];
	}

	// The ground: the highest floor within the reach, spread to the columns without floor in reach.
	TArray<float> Highest;
	Highest.Init(-1.f, NumColumns);
	const int32 SpringReach = FMath::Clamp(FMath::RoundToInt(Settings.SpringReach), 1, SizeXZ);
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		for (int32 DX = -SpringReach; DX <= SpringReach; ++DX)
		{
			for (int32 DZ = -SpringReach; DZ <= SpringReach; ++DZ)
			{
				const FIntPoint Near = Column + FIntPoint(DX, DZ);
				if (DX * DX + DZ * DZ <= SpringReach * SpringReach + SpringReach && IsInGrid(Near) && FloorColumns[ToColumnIndex(Near)])
				{
					Highest[Index] = FMath::Max(Highest[Index], float(Floors[ToColumnIndex(Near)]));
				}
			}
		}
	}
	for (bool bChanged = true; bChanged;)
	{
		bChanged = false;
		TArray<float> Next = Highest;
		for (int32 Index = 0; Index < NumColumns; ++Index)
		{
			if (Highest[Index] >= 0.f)
			{
				continue;
			}
			const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
			for (const FIntPoint& Side : HollowSides)
			{
				if (IsInGrid(Column + Side) && Highest[ToColumnIndex(Column + Side)] >= 0.f)
				{
					Next[Index] = FMath::Max(Next[Index], Highest[ToColumnIndex(Column + Side)]);
					bChanged = true;
				}
			}
		}
		Highest = MoveTemp(Next);
	}

	// Ledges: out from the hollow, past railings and posts on its edge, nothing drawn (or the grid's end). Not at an exit (its
	// passage leads on, walled dark).
	auto NearExit = [&Exits](const FIntPoint& Column)
	{
		return Exits.ContainsByPredicate([&Column](const FLBACellBox& Box)
		{
			return Column.X >= Box.Min.X - 1 && Column.X <= Box.Max.X + 1 && Column.Y >= Box.Min.Z - 1 && Column.Y <= Box.Max.Z + 1;
		});
	};
	const TBitArray<> Bounded = InHollow;
	// A railing (or a post) and nothing else over the ground: a fence on top of a wall is a wall.
	auto IsRailing = [&](const FIntPoint& Column, int32 Ground)
	{
		bool bAny = false;
		for (int32 Y = FMath::Max(Ground + 1, 0); Y <= DrawnTops[ToColumnIndex(Column)]; ++Y)
		{
			const int32 Brick = Cells.Brick(Column.X, Y, Column.Y);
			if (Brick == 0 || !Cells.HasArt(Brick) || Cells.IsFrame(Column.X, Y, Column.Y))
			{
				continue;
			}
			const int32 Layout = Cells.Layout(Column.X, Y, Column.Y);
			if (!Settings.RailingLayouts.Contains(Layout) && !Settings.PillarLayouts.Contains(Layout))
			{
				return false;
			}
			bAny = true;
		}
		return bAny;
	};
	// A pillar layout with a railing near it (or under it: a curb), rising over the railing by one post at most: a fence's post
	// (the Temple of Bu's urns on the corners of its walkways, on pedestals or not, its dark posts where a fence turns). A
	// column going on up to the roof with spikes at its foot is a pillar.
	auto IsFencePost = [&](const FIntPoint& Column)
	{
		const int32 Top = DrawnTops[ToColumnIndex(Column)];
		if (Top == INDEX_NONE || !Settings.PillarLayouts.Contains(Cells.Layout(Column.X, Top, Column.Y)))
		{
			return false;
		}
		int32 RailingTop = INDEX_NONE;
		for (int32 DX = -FencePostReach; DX <= FencePostReach; ++DX)
		{
			for (int32 DZ = -FencePostReach; DZ <= FencePostReach; ++DZ)
			{
				const FIntPoint Near = Column + FIntPoint(DX, DZ);
				for (int32 Y = DrawnBottoms[ToColumnIndex(Column)]; Y <= Top && IsInGrid(Near); ++Y)
				{
					const int32 Brick = Cells.Brick(Near.X, Y, Near.Y);
					if (Brick != 0 && Cells.HasArt(Brick) && Settings.RailingLayouts.Contains(Cells.Layout(Near.X, Y, Near.Y)))
					{
						RailingTop = FMath::Max(RailingTop, Y);
					}
				}
			}
		}
		return RailingTop != INDEX_NONE && Top <= RailingTop + FencePostRise;
	};
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		const int32 Ground = Floors[Index] != INDEX_NONE ? Floors[Index] : FMath::RoundToInt(Highest[Index]);
		if (!Bounded[Index] || Ground < 0 || NearExit(Column))
		{
			continue;
		}
		for (const FIntPoint& Side : HollowSides)
		{
			if (IsInGrid(Column + Side) && Bounded[ToColumnIndex(Column + Side)])
			{
				continue;
			}
			// A railing on the edge (in the hollow, or past it) tells a ledge from a wall cut down to a rim.
			bool bRailed = IsRailing(Column, Ground);
			for (int32 Step = 1; Step <= LedgeLook; ++Step)
			{
				const FIntPoint At = Column + Side * Step;
				if (IsInGrid(At) && (Bounded[ToColumnIndex(At)] || NearExit(At)))
				{
					break;
				}
				if (!IsInGrid(At) || DrawnTops[ToColumnIndex(At)] == INDEX_NONE)
				{
					if (!bRailed)
					{
						break;
					}
					FLedge& Ledge = Ledges.AddDefaulted_GetRef();
					Ledge.Column = At - Side;
					Ledge.Side = Side;
					Ledge.Ground = Ground;
					Ledge.Foot = Ground;
					for (int32 Back = 0; Back < Step; ++Back)
					{
						const int32 Edge = ToColumnIndex(Column + Side * Back);
						InHollow[Edge] = true;
						Ledge.Foot = DrawnBottoms[Edge] != INDEX_NONE ? FMath::Min(Ledge.Foot, DrawnBottoms[Edge]) : Ledge.Foot;
					}
					break;
				}
				if (DrawnTops[ToColumnIndex(At)] < Ground || !IsRailing(At, Ground))
				{
					break;
				}
				bRailed = true;
			}
		}
	}

	// Canyon: the empty columns within its width of a ledge and out past its edge, reached from the ledges through empty
	// columns.
	const int32 DomainSize = SizeXZ + 2 * Margin;
	const int32 NumDomain = DomainSize * DomainSize;
	auto IsEmpty = [this](const FIntPoint& Column)
	{
		return !IsInGrid(Column) || (DrawnTops[ToColumnIndex(Column)] == INDEX_NONE && !InHollow[ToColumnIndex(Column)]);
	};
	InCanyon.Init(false, NumDomain);
	TArray<FIntPoint> Open;
	for (const FLedge& Ledge : Ledges)
	{
		const FIntPoint Past = Ledge.Column + Ledge.Side;
		if (IsInDomain(Past) && !InCanyon[ToDomainIndex(Past)])
		{
			InCanyon[ToDomainIndex(Past)] = true;
			Open.Add(Past);
		}
	}
	for (int32 Index = 0; Index < Open.Num(); ++Index)
	{
		for (const FIntPoint& Side : HollowSides)
		{
			const FIntPoint Next = Open[Index] + Side;
			if (!IsInDomain(Next) || InCanyon[ToDomainIndex(Next)] || !IsEmpty(Next))
			{
				continue;
			}
			float Distance = 0.f;
			const int32 Nearest = FindNearestLedge(Next.X + 0.5f, Next.Y + 0.5f, Distance);
			const FLedge* Ledge = Nearest != INDEX_NONE ? &Ledges[Nearest] : nullptr;
			if (Ledge && Distance < CanyonWidth && (Next - Ledge->Column).X * Ledge->Side.X + (Next - Ledge->Column).Y * Ledge->Side.Y > 0)
			{
				InCanyon[ToDomainIndex(Next)] = true;
				Open.Add(Next);
			}
		}
	}

	// Spring: the ground smoothed, plus the clearance; over a canyon (and round the grid), its nearest ledge's.
	TArray<float> GridSprings;
	GridSprings.Init(float(Settings.Clear), NumColumns);
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		float Sum = 0.f;
		int32 Count = 0;
		for (int32 DX = -SpringBlur; DX <= SpringBlur; ++DX)
		{
			for (int32 DZ = -SpringBlur; DZ <= SpringBlur; ++DZ)
			{
				const FIntPoint Near = Column + FIntPoint(DX, DZ);
				if (IsInGrid(Near) && Highest[ToColumnIndex(Near)] >= 0.f)
				{
					Sum += Highest[ToColumnIndex(Near)];
					++Count;
				}
			}
		}
		GridSprings[Index] = (Count > 0 ? Sum / Count : 0.f) + Settings.Clear;
		// Railings and their posts hold nothing: the roof springs over them (and over its bumps).
		if (InHollow[Index] && DrawnTops[Index] != INDEX_NONE && (IsRailing(Column, FMath::RoundToInt(Highest[Index])) || IsFencePost(Column)))
		{
			GridSprings[Index] = FMath::Max(GridSprings[Index], DrawnTops[Index] + 1.f + Settings.Shape.Noise);
		}
	}
	Springs.Init(0.f, NumDomain);
	for (int32 Index = 0; Index < NumDomain; ++Index)
	{
		const FIntPoint Column(Index % DomainSize - Margin, Index / DomainSize - Margin);
		const FIntPoint Clamped(FMath::Clamp(Column.X, 0, SizeXZ - 1), FMath::Clamp(Column.Y, 0, SizeXZ - 1));
		float Distance = 0.f;
		const int32 Ledge = InCanyon[Index] ? FindNearestLedge(Column.X + 0.5f, Column.Y + 0.5f, Distance) : INDEX_NONE;
		Springs[Index] = GridSprings[ToColumnIndex(Ledge != INDEX_NONE ? Ledges[Ledge].Column : Clamped)];
	}

	// Pillars: pillar layouts standing free in the hollow, tall enough, away from the ledges.
	PillarColumns.Init(false, NumColumns);
	if (Settings.PillarLayouts.Num() > 0)
	{
		auto IsPillarColumn = [&](const FIntPoint& Column)
		{
			const int32 Index = ToColumnIndex(Column);
			const int32 Top = DrawnTops[Index];
			return InHollow[Index] && Floors[Index] == INDEX_NONE && Top != INDEX_NONE && Highest[Index] >= 0.f
				&& Settings.PillarLayouts.Contains(Cells.Layout(Column.X, Top, Column.Y)) && Top - Highest[Index] >= PillarMin
				&& !IsFencePost(Column);
		};
		TBitArray<> Visited(false, NumColumns);
		for (int32 Index = 0; Index < NumColumns; ++Index)
		{
			const FIntPoint Start(Index % SizeXZ, Index / SizeXZ);
			if (Visited[Index] || !IsPillarColumn(Start))
			{
				continue;
			}
			FPillar Pillar;
			Pillar.Columns.Add(Start);
			Visited[Index] = true;
			for (int32 At = 0; At < Pillar.Columns.Num(); ++At)
			{
				for (const FIntPoint& Side : HollowSides)
				{
					const FIntPoint Next = Pillar.Columns[At] + Side;
					if (IsInGrid(Next) && !Visited[ToColumnIndex(Next)] && IsPillarColumn(Next))
					{
						Visited[ToColumnIndex(Next)] = true;
						Pillar.Columns.Add(Next);
					}
				}
			}
			Pillar.Min = Pillar.Max = Start;
			for (const FIntPoint& Column : Pillar.Columns)
			{
				Pillar.Min = Pillar.Min.ComponentMin(Column);
				Pillar.Max = Pillar.Max.ComponentMax(Column);
				Pillar.Top = FMath::Max(Pillar.Top, float(DrawnTops[ToColumnIndex(Column)] + 1));
			}
			const bool bOnLedge = Ledges.ContainsByPredicate([&Pillar](const FLedge& Ledge)
			{
				return Ledge.Column.X >= Pillar.Min.X - PillarLedgeClear && Ledge.Column.X <= Pillar.Max.X + PillarLedgeClear
					&& Ledge.Column.Y >= Pillar.Min.Y - PillarLedgeClear && Ledge.Column.Y <= Pillar.Max.Y + PillarLedgeClear;
			});
			if (Pillar.Max.X - Pillar.Min.X >= PillarSize || Pillar.Max.Y - Pillar.Min.Y >= PillarSize || bOnLedge)
			{
				continue;
			}
			for (const FIntPoint& Column : Pillar.Columns)
			{
				PillarColumns[ToColumnIndex(Column)] = true;
			}
			Pillars.Add(MoveTemp(Pillar));
		}
	}

	// Supports: what comes up to the roof over it holds it (pillars hold it their own way; railings and posts hold nothing).
	TBitArray<> Spanned(false, NumDomain);
	for (int32 Index = 0; Index < NumDomain; ++Index)
	{
		const FIntPoint Column(Index % DomainSize - Margin, Index / DomainSize - Margin);
		Spanned[Index] = InCanyon[Index] || (IsInGrid(Column) && InHollow[ToColumnIndex(Column)]);
	}
	const FLBAVault Bare(Spanned, Settings.Shape, Settings.Seed, Margin);
	Supports.Init(false, NumColumns);
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		if (!InHollow[Index] || FloorColumns[Index] || DrawnTops[Index] == INDEX_NONE || PillarColumns[Index]
			|| IsRailing(Column, FMath::RoundToInt(Highest[Index])) || IsFencePost(Column))
		{
			continue;
		}
		const float Roof = Springs[ToDomainIndex(Column)] + Bare.GetCove(Bare.GetDistance(Column.X + 0.5f, Column.Y + 0.5f));
		Supports[Index] = DrawnTops[Index] + 1 >= Roof - Reach;
	}

	// Arches: between neighbouring pillars (no third one nearer both), over the hollow's air.
	for (int32 A = 0; A < Pillars.Num(); ++A)
	{
		for (int32 B = A + 1; B < Pillars.Num(); ++B)
		{
			const FVector2f CA = Pillars[A].GetCenter(), CB = Pillars[B].GetCenter();
			const float Span = FVector2f::Distance(CA, CB);
			bool bClear = Span <= ArcadeSpan;
			for (int32 C = 0; C < Pillars.Num() && bClear; ++C)
			{
				bClear = C == A || C == B || FMath::Max(FVector2f::Distance(CA, Pillars[C].GetCenter()), FVector2f::Distance(CB, Pillars[C].GetCenter())) >= Span;
			}
			for (float Along = 0.f; Along <= Span && bClear; Along += 0.25f)
			{
				const FVector2f P = CA + (CB - CA) * (Along / Span);
				const FIntPoint Column(FMath::FloorToInt(P.X), FMath::FloorToInt(P.Y));
				const int32 Index = ToColumnIndex(Column);
				auto Within = [&Column](const FPillar& Pillar)
				{
					return Column.X >= Pillar.Min.X && Column.X <= Pillar.Max.X && Column.Y >= Pillar.Min.Y && Column.Y <= Pillar.Max.Y;
				};
				const bool bOwn = Within(Pillars[A]) || Within(Pillars[B]);
				bClear = IsInGrid(Column) && InHollow[Index] && !Supports[Index] && (bOwn || !PillarColumns[Index]);
			}
			if (bClear)
			{
				Arches.Add(FArch{ A, B });
			}
		}
	}

	// The roof: over the hollow but its supports, and over the canyon.
	TBitArray<> UnderRoof(false, NumDomain);
	for (int32 Index = 0; Index < NumDomain; ++Index)
	{
		const FIntPoint Column(Index % DomainSize - Margin, Index / DomainSize - Margin);
		UnderRoof[Index] = InCanyon[Index] || (IsInGrid(Column) && InHollow[ToColumnIndex(Column)] && !Supports[ToColumnIndex(Column)]);
		NumUnderRoof += UnderRoof[Index];
	}
	Vault = FLBAVault(UnderRoof, Settings.Shape, Settings.Seed, Margin);
}

int32 FLBAHollow::FindNearestLedge(float X, float Z, float& OutDistance) const
{
	int32 Best = INDEX_NONE;
	OutDistance = TNumericLimits<float>::Max();
	const FVector2f P(X, Z);
	for (int32 Index = 0; Index < Ledges.Num(); ++Index)
	{
		// The edge between the ledge's column and the next one on its side.
		const FLedge& Ledge = Ledges[Index];
		const float EX = Ledge.Column.X + (Ledge.Side.X > 0 ? 1.f : 0.f), EZ = Ledge.Column.Y + (Ledge.Side.Y > 0 ? 1.f : 0.f);
		const FVector2f A(EX, EZ), B = Ledge.Side.X != 0 ? FVector2f(EX, EZ + 1.f) : FVector2f(EX + 1.f, EZ);
		float Along = 0.f;
		const float Distance = SegmentDistance(P, A, B, Along);
		if (Distance < OutDistance)
		{
			OutDistance = Distance;
			Best = Index;
		}
	}
	return Best;
}

float FLBAHollow::GetSkeletonRoof(float X, float Z) const
{
	if (Pillars.IsEmpty())
	{
		return TNumericLimits<float>::Max();
	}
	// From each pillar's top the roof fans out up to the crown; from each arch it rises on both sides up to the crown.
	const float Spring = GetSpring(X, Z);
	const float Crown = Spring + Settings.Shape.Rise;
	const float Radius = FMath::Max(Settings.Shape.Radius, 1.f);
	float Roof = TNumericLimits<float>::Max();
	for (const FPillar& Pillar : Pillars)
	{
		const float Distance = BoxDistance(X, Z, Pillar.Min, Pillar.Max);
		if (Distance < Radius)
		{
			Roof = FMath::Min(Roof, Pillar.Top + FMath::Max(0.f, Crown - Pillar.Top) * Cove01(Distance / Radius));
		}
	}
	for (const FArch& Arch : Arches)
	{
		const FPillar& A = Pillars[Arch.A];
		const FPillar& B = Pillars[Arch.B];
		float Along = 0.f;
		const float Distance = FMath::Max(0.f, SegmentDistance(FVector2f(X, Z), A.GetCenter(), B.GetCenter(), Along) - 0.5f);
		if (Distance < Radius)
		{
			// A semi-ellipse from top to top, rising ArchRise of the way to the crown.
			const float Base = FMath::Lerp(A.Top, B.Top, Along);
			const float Height = Base + ArchRise * FMath::Max(0.f, Crown - Base) * Cove01(1.f - FMath::Abs(1.f - 2.f * Along));
			Roof = FMath::Min(Roof, Height + FMath::Max(0.f, Crown - Height) * Cove01(Distance / Radius));
		}
	}
	// Never down into the headroom over the ground.
	return FMath::Max(Roof, Spring - Settings.Clear + Headroom + 1);
}

bool FLBAHollow::IsInHollow(const FIntPoint& Column) const
{
	return IsInGrid(Column) && InHollow[ToColumnIndex(Column)];
}

bool FLBAHollow::IsUnderRoof(const FIntPoint& Column) const
{
	return Vault.Contains(Column);
}

bool FLBAHollow::IsInCanyon(const FIntPoint& Column) const
{
	return IsInDomain(Column) && InCanyon[ToDomainIndex(Column)];
}

bool FLBAHollow::IsPillar(const FIntPoint& Column) const
{
	return IsInGrid(Column) && PillarColumns[ToColumnIndex(Column)];
}

int32 FLBAHollow::GetDrawnBottom(const FIntPoint& Column) const
{
	return IsInGrid(Column) ? DrawnBottoms[ToColumnIndex(Column)] : INDEX_NONE;
}

float FLBAHollow::GetCanyonLight(float X, float Y, float Z) const
{
	float Distance = 0.f;
	const int32 Nearest = FindNearestLedge(X, Z, Distance);
	if (Nearest == INDEX_NONE)
	{
		return 1.f;
	}
	const float Depth = Ledges[Nearest].Ground - Y;
	return FMath::Lerp(CanyonLit, CanyonDark, FMath::SmoothStep(0.f, CanyonDepth * 0.6f, Depth));
}

float FLBAHollow::GetCanyonFloor(float X, float Z) const
{
	float Distance = 0.f;
	const int32 Nearest = FindNearestLedge(X, Z, Distance);
	if (Nearest == INDEX_NONE)
	{
		return 0.f;
	}
	// Down from the ledge's foot to the bottom halfway across, then up to the spring at the far side (both quarter ellipses,
	// steep at the walls).
	const float Foot = Ledges[Nearest].Foot;
	const float Bottom = Foot - CanyonDepth;
	const float T = FMath::Clamp(Distance / CanyonWidth, 0.f, 1.f);
	if (T < 0.5f)
	{
		return Foot - (Foot - Bottom) * Cove01(T * 2.f);
	}
	const float Spring = GetSpring(X, Z);
	return Spring - (Spring - Bottom) * Cove01(2.f - T * 2.f);
}

int32 FLBAHollow::GetFloor(const FIntPoint& Column) const
{
	return IsInGrid(Column) ? Floors[ToColumnIndex(Column)] : INDEX_NONE;
}

int32 FLBAHollow::GetDrawnTop(const FIntPoint& Column) const
{
	return IsInGrid(Column) ? DrawnTops[ToColumnIndex(Column)] : INDEX_NONE;
}

float FLBAHollow::GetSpring(float X, float Z) const
{
	// Bilinear between the columns' centers (clamped at the domain's edge).
	const int32 Size = SizeXZ + 2 * Margin;
	const float U = FMath::Clamp(X + Margin - 0.5f, 0.f, Size - 1.f), V = FMath::Clamp(Z + Margin - 0.5f, 0.f, Size - 1.f);
	const int32 X0 = FMath::Min(FMath::FloorToInt(U), Size - 2), Z0 = FMath::Min(FMath::FloorToInt(V), Size - 2);
	const float FX = U - X0, FZ = V - Z0;
	auto At = [this, Size](int32 CX, int32 CZ) { return Springs[CZ * Size + CX]; };
	return FMath::Lerp(FMath::Lerp(At(X0, Z0), At(X0 + 1, Z0), FX), FMath::Lerp(At(X0, Z0 + 1), At(X0 + 1, Z0 + 1), FX), FZ);
}

float FLBAHollow::GetRoof(float X, float Z) const
{
	return FMath::Min(GetSpring(X, Z) + Vault.GetRise(X, Z), GetSkeletonRoof(X, Z));
}

FString FLBAHollow::Describe() const
{
	int32 Hollow = 0, NumSupports = 0, NumFloors = 0, NumCanyon = 0;
	float SpringMin = TNumericLimits<float>::Max(), SpringMax = 0.f, RoofMax = 0.f;
	for (int32 Index = 0; Index < InCanyon.Num(); ++Index)
	{
		NumCanyon += InCanyon[Index];
	}
	for (int32 Index = 0; Index < SizeXZ * SizeXZ; ++Index)
	{
		Hollow += InHollow[Index];
		NumSupports += Supports[Index];
		NumFloors += Floors[Index] != INDEX_NONE;
		if (Vault.Contains(FIntPoint(Index % SizeXZ, Index / SizeXZ)) && InHollow[Index])
		{
			SpringMin = FMath::Min(SpringMin, Springs[ToDomainIndex(FIntPoint(Index % SizeXZ, Index / SizeXZ))]);
			SpringMax = FMath::Max(SpringMax, Springs[ToDomainIndex(FIntPoint(Index % SizeXZ, Index / SizeXZ))]);
			RoofMax = FMath::Max(RoofMax, GetRoof(Index % SizeXZ + 0.5f, Index / SizeXZ + 0.5f));
		}
	}
	return FString::Printf(TEXT("hollow %d columns (%d with floor), %d supports, %d under the roof; spring %.1f-%.1f, roof up to %.1f\n")
		TEXT("%d ledge edges, canyon %d columns; %d pillars, %d arches\n"),
		Hollow, NumFloors, NumSupports, NumUnderRoof, NumUnderRoof ? SpringMin : 0.f, SpringMax, RoofMax, Ledges.Num(), NumCanyon, Pillars.Num(), Arches.Num());
}

void FLBAHollow::ToGridPlan(FLBAGridPlanHollow& Out) const
{
	Out.Columns.Reset();
	for (int32 Index = 0; Index < SizeXZ * SizeXZ; ++Index)
	{
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		if (!InHollow[Index] && !IsInCanyon(Column))
		{
			continue;
		}
		FLBAGridPlanHollowColumn& Saved = Out.Columns.AddDefaulted_GetRef();
		Saved.Column = Column;
		Saved.Floor = GetFloor(Column);
		Saved.bSupport = Supports[Index];
		Saved.bPillar = PillarColumns[Index];
		Saved.bCanyon = IsInCanyon(Column);
		Saved.Spring = GetSpring(Column.X + 0.5f, Column.Y + 0.5f);
		Saved.Roof = GetRoof(Column.X + 0.5f, Column.Y + 0.5f);
	}
	Out.Summary = Describe().TrimEnd();
}
