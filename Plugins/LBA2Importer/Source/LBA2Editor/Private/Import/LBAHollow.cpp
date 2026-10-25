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
}

FLBAVault::FLBAVault(const TBitArray<>& InFootprint, const FLBAVaultShape& InShape, int32 InSeed)
	: Footprint(InFootprint)
	, Shape(InShape)
	, Seed(InSeed)
{
}

bool FLBAVault::Contains(const FIntPoint& Column) const
{
	return FLBARoomPlan::IsInGrid(Column) && Footprint.IsValidIndex(FLBARoomPlan::ToColumnIndex(Column)) && Footprint[FLBARoomPlan::ToColumnIndex(Column)];
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

	// Drawn tops: the art the original shows (invisible collision is no rock).
	DrawnTops.Init(INDEX_NONE, NumColumns);
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = SizeY - 1; Y >= 0; --Y)
			{
				const int32 Brick = Cells.Brick(X, Y, Z);
				if (Brick != 0 && Cells.HasArt(Brick) && !Cells.IsFrame(X, Y, Z))
				{
					DrawnTops[ToColumnIndex(FIntPoint(X, Z))] = Y;
					break;
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

	// Spring: the highest floor within the reach, spread to the columns without floor in reach, smoothed.
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
	Springs.Init(float(Settings.Clear), NumColumns);
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
		Springs[Index] = (Count > 0 ? Sum / Count : 0.f) + Settings.Clear;
	}

	// Supports: what comes up to the roof over it holds it.
	const FLBAVault Bare(InHollow, Settings.Shape, Settings.Seed);
	Supports.Init(false, NumColumns);
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		if (!InHollow[Index] || FloorColumns[Index] || DrawnTops[Index] == INDEX_NONE)
		{
			continue;
		}
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		const float Roof = Springs[Index] + Bare.GetCove(Bare.GetDistance(Column.X + 0.5f, Column.Y + 0.5f));
		Supports[Index] = DrawnTops[Index] + 1 >= Roof - Reach;
	}
	TBitArray<> UnderRoof(false, NumColumns);
	for (int32 Index = 0; Index < NumColumns; ++Index)
	{
		UnderRoof[Index] = InHollow[Index] && !Supports[Index];
		NumUnderRoof += UnderRoof[Index];
	}
	Vault = FLBAVault(UnderRoof, Settings.Shape, Settings.Seed);
}

bool FLBAHollow::IsInHollow(const FIntPoint& Column) const
{
	return IsInGrid(Column) && InHollow[ToColumnIndex(Column)];
}

bool FLBAHollow::IsUnderRoof(const FIntPoint& Column) const
{
	return Vault.Contains(Column);
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
	// Bilinear between the columns' centers (clamped at the grid's edge).
	const float U = FMath::Clamp(X - 0.5f, 0.f, SizeXZ - 1.f), V = FMath::Clamp(Z - 0.5f, 0.f, SizeXZ - 1.f);
	const int32 X0 = FMath::Min(FMath::FloorToInt(U), SizeXZ - 2), Z0 = FMath::Min(FMath::FloorToInt(V), SizeXZ - 2);
	const float FX = U - X0, FZ = V - Z0;
	auto At = [this](int32 CX, int32 CZ) { return Springs[ToColumnIndex(FIntPoint(CX, CZ))]; };
	return FMath::Lerp(FMath::Lerp(At(X0, Z0), At(X0 + 1, Z0), FX), FMath::Lerp(At(X0, Z0 + 1), At(X0 + 1, Z0 + 1), FX), FZ);
}

float FLBAHollow::GetRoof(float X, float Z) const
{
	return GetSpring(X, Z) + Vault.GetRise(X, Z);
}

FString FLBAHollow::Describe() const
{
	int32 Hollow = 0, NumSupports = 0, NumFloors = 0;
	float SpringMin = TNumericLimits<float>::Max(), SpringMax = 0.f, RoofMax = 0.f;
	for (int32 Index = 0; Index < SizeXZ * SizeXZ; ++Index)
	{
		Hollow += InHollow[Index];
		NumSupports += Supports[Index];
		NumFloors += Floors[Index] != INDEX_NONE;
		if (Vault.Contains(FIntPoint(Index % SizeXZ, Index / SizeXZ)))
		{
			SpringMin = FMath::Min(SpringMin, Springs[Index]);
			SpringMax = FMath::Max(SpringMax, Springs[Index]);
			RoofMax = FMath::Max(RoofMax, GetRoof(Index % SizeXZ + 0.5f, Index / SizeXZ + 0.5f));
		}
	}
	return FString::Printf(TEXT("hollow %d columns (%d with floor), %d supports, %d under the roof; spring %.1f-%.1f, roof up to %.1f\n"),
		Hollow, NumFloors, NumSupports, NumUnderRoof, NumUnderRoof ? SpringMin : 0.f, SpringMax, RoofMax);
}

void FLBAHollow::ToGridPlan(FLBAGridPlanHollow& Out) const
{
	Out.Columns.Reset();
	for (int32 Index = 0; Index < SizeXZ * SizeXZ; ++Index)
	{
		if (!InHollow[Index])
		{
			continue;
		}
		const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
		FLBAGridPlanHollowColumn& Saved = Out.Columns.AddDefaulted_GetRef();
		Saved.Column = Column;
		Saved.Floor = GetFloor(Column);
		Saved.bSupport = Supports[Index];
		Saved.Spring = GetSpring(Column.X + 0.5f, Column.Y + 0.5f);
		Saved.Roof = GetRoof(Column.X + 0.5f, Column.Y + 0.5f);
	}
	Out.Summary = Describe().TrimEnd();
}
