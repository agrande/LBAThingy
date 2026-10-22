#include "Import/LBARoomAnalysis.h"

namespace
{
	/** Horizontal neighbors (X, Z). */
	const FIntPoint Directions[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };

	bool CellLess(const FIntVector& A, const FIntVector& B)
	{
		return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z;
	}
}

bool FLBADoorway::IsInStub() const
{
	return FlankHeight < FLBARoomAnalysis::TallWall;
}

FLBARoomAnalysis::FLBARoomAnalysis(TFunctionRef<int32(int32, int32, int32)> Shape, TFunctionRef<int32(int32, int32, int32)> Brick,
	TFunctionRef<int32(int32, int32, int32)> Layout)
{
	const int32 NumCells = SizeXZ * SizeY * SizeXZ;
	Solid.Init(false, NumCells);
	Bricks.Init(false, NumCells);
	Layouts.Init(INDEX_NONE, NumCells);
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				const int32 Index = ToIndex(X, Y, Z);
				Solid[Index] = Shape(X, Y, Z) != 0;
				Bricks[Index] = Brick(X, Y, Z) != 0;
				Layouts[Index] = int16(Layout(X, Y, Z));
			}
		}
	}

	// Walkable floors: a solid cell with room for Twinsen above it.
	TSet<FIntVector> Floors;
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y + 1 < SizeY; ++Y)
			{
				if (!IsSolid(X, Y, Z))
				{
					continue;
				}
				bool bClear = true;
				for (int32 K = 1; K <= Headroom && bClear; ++K)
				{
					bClear = !IsSolid(X, Y + K, Z);
				}
				if (bClear)
				{
					Floors.Add(FIntVector(X, Y, Z));
				}
			}
		}
	}

	// Spaces: floors connected by steps of at most one cell.
	TSet<FIntVector> Seen;
	for (const FIntVector& Start : Floors)
	{
		if (Seen.Contains(Start))
		{
			continue;
		}
		TArray<FIntVector> Space;
		TArray<FIntVector> Todo = { Start };
		Seen.Add(Start);
		while (Todo.Num() > 0)
		{
			const FIntVector Cell = Todo.Pop(EAllowShrinking::No);
			Space.Add(Cell);
			for (const FIntPoint& Dir : Directions)
			{
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					const FIntVector Next(Cell.X + Dir.X, Cell.Y + DY, Cell.Z + Dir.Y);
					if (Floors.Contains(Next) && !Seen.Contains(Next))
					{
						Seen.Add(Next);
						Todo.Add(Next);
					}
				}
			}
		}
		if (Space.Num() >= MinSpaceCells)
		{
			RoomFloors.Append(Space);
			Spaces.Add(MoveTemp(Space));
		}
	}
}

bool FLBARoomAnalysis::IsSolid(int32 X, int32 Y, int32 Z) const
{
	return IsInside(X, Y, Z) && Solid[ToIndex(X, Y, Z)];
}

bool FLBARoomAnalysis::HasBrick(int32 X, int32 Y, int32 Z) const
{
	return IsInside(X, Y, Z) && Bricks[ToIndex(X, Y, Z)];
}

int32 FLBARoomAnalysis::LayoutAt(int32 X, int32 Y, int32 Z) const
{
	return IsInside(X, Y, Z) ? Layouts[ToIndex(X, Y, Z)] : INDEX_NONE;
}

int32 FLBARoomAnalysis::GetWallHeight(int32 X, int32 Y, int32 Z) const
{
	int32 Height = 0;
	while (Y + 1 + Height < SizeY && IsSolid(X, Y + 1 + Height, Z))
	{
		++Height;
	}
	return Height;
}

bool FLBARoomAnalysis::IsWall(int32 X, int32 Y, int32 Z, int32 Height) const
{
	if (X < 0 || Z < 0 || X >= SizeXZ || Z >= SizeXZ || Y + Height >= SizeY)
	{
		return false;
	}
	for (int32 K = 1; K <= Height; ++K)
	{
		if (!IsSolid(X, Y + K, Z))
		{
			return false;
		}
	}
	return true;
}

bool FLBARoomAnalysis::IsLined(int32 X, int32 Y, int32 Z, const FIntPoint& Away) const
{
	for (const FIntPoint& Dir : Directions)
	{
		if (Dir != Away && IsWall(X + Dir.X, Y, Z + Dir.Y))
		{
			return true;
		}
	}
	return X == 0 || Z == 0 || X == SizeXZ - 1 || Z == SizeXZ - 1;
}

ELBAOpeningEnd FLBARoomAnalysis::GetEnd(int32 X, int32 Y, int32 Z) const
{
	if (X < 0 || Z < 0 || X >= SizeXZ || Z >= SizeXZ)
	{
		return ELBAOpeningEnd::Edge;
	}
	for (int32 DY = -1; DY <= 1; ++DY)
	{
		if (RoomFloors.Contains(FIntVector(X, Y + DY, Z)))
		{
			return ELBAOpeningEnd::Room;
		}
	}
	bool bSolid = false;
	bool bVisible = false;
	for (int32 K = Y + 1; K < FMath::Min(SizeY, Y + 1 + WallHeight); ++K)
	{
		bSolid |= IsSolid(X, K, Z);
		bVisible |= HasBrick(X, K, Z);
	}
	if (bSolid)
	{
		return bVisible ? ELBAOpeningEnd::Blocked : ELBAOpeningEnd::Plug;
	}
	for (int32 K = 0; K <= Y; ++K)
	{
		if (IsSolid(X, K, Z))
		{
			return ELBAOpeningEnd::Drop;
		}
	}
	return ELBAOpeningEnd::Void;
}

TMap<FIntVector, FLBARoomAnalysis::FOpening> FLBARoomAnalysis::FindOpenings() const
{
	TMap<FIntVector, FOpening> Openings;
	for (const FIntVector& Cell : RoomFloors)
	{
		const int32 X = Cell.X, Y = Cell.Y, Z = Cell.Z;
		if (IsWall(X, Y, Z))
		{
			continue;
		}
		// Crossed along X (walls on the Z sides), then along Z.
		for (int32 AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
		{
			const FIntPoint Ax = AxisIndex == 0 ? FIntPoint(1, 0) : FIntPoint(0, 1);
			const FIntPoint Sd = AxisIndex == 0 ? FIntPoint(0, 1) : FIntPoint(1, 0);
			auto FirstWall = [&](int32 Sign)
			{
				for (int32 K = 1; K <= MaxWidth; ++K)
				{
					if (IsWall(X + Sign * K * Sd.X, Y, Z + Sign * K * Sd.Y))
					{
						return K;
					}
				}
				return 0;
			};
			const int32 Left = FirstWall(-1);
			const int32 Right = FirstWall(1);
			if (Left == 0 || Right == 0 || Left + Right - 1 > MaxWidth)
			{
				continue;
			}
			const FIntPoint L(X - Left * Sd.X, Z - Left * Sd.Y);
			const FIntPoint R(X + Right * Sd.X, Z + Right * Sd.Y);
			if (!IsLined(L.X, Y, L.Y, Sd) || !IsLined(R.X, Y, R.Y, FIntPoint(-Sd.X, -Sd.Y)))
			{
				continue;
			}

			// Depth through the wall: how far along the passage both flanks continue.
			auto Flanked = [&](int32 K)
			{
				return IsWall(L.X + K * Ax.X, Y, L.Y + K * Ax.Y) && IsWall(R.X + K * Ax.X, Y, R.Y + K * Ax.Y);
			};
			auto Extent = [&](int32 Sign)
			{
				for (int32 K = 1; K <= MaxExitDepth + 1; ++K)
				{
					if (!Flanked(Sign * K))
					{
						return K - 1;
					}
				}
				return MaxExitDepth;
			};
			const int32 Neg = Extent(-1);
			const int32 Pos = Extent(1);
			const int32 Depth = Neg + Pos + 1;
			if (Depth > MaxExitDepth)
			{
				continue;
			}
			const ELBAOpeningEnd EndA = GetEnd(X - (Neg + 1) * Ax.X, Y, Z - (Neg + 1) * Ax.Y);
			const ELBAOpeningEnd EndB = GetEnd(X + (Pos + 1) * Ax.X, Y, Z + (Pos + 1) * Ax.Y);
			if ((EndA != ELBAOpeningEnd::Room && EndB != ELBAOpeningEnd::Room) || EndA == ELBAOpeningEnd::Blocked || EndB == ELBAOpeningEnd::Blocked)
			{
				continue;
			}
			const ELBAOpeningEnd Other = EndA == ELBAOpeningEnd::Room ? EndB : EndA;
			if (Other == ELBAOpeningEnd::Room && Depth > MaxDepth)
			{
				continue;
			}

			FOpening Opening;
			Opening.Axis = AxisIndex == 0 ? 0 : 2;
			Opening.Width = Left + Right - 1;
			Opening.FlankHeight = FMath::Min(GetWallHeight(L.X, Y, L.Y), GetWallHeight(R.X, Y, R.Y));
			for (int32 K = 0; Y + 1 + K < SizeY; ++K)
			{
				if (IsSolid(X, Y + 1 + K, Z))
				{
					Opening.ClearHeight = K;
					break;
				}
			}
			Opening.Other = Other;
			for (const FIntPoint& Flank : { L, R })
			{
				for (int32 FY = Y + 1; FY < FMath::Min(SizeY, Y + 1 + TallWall); ++FY)
				{
					const int32 FlankLayout = LayoutAt(Flank.X, FY, Flank.Y);
					if (FlankLayout >= 0)
					{
						Opening.FlankLayouts.Add(FlankLayout);
					}
				}
			}
			Opening.FloorLayout = LayoutAt(X, Y, Z);
			Opening.Flanks[0] = L;
			Opening.Flanks[1] = R;
			// A cell open along both axes keeps the second.
			Openings.Add(Cell, MoveTemp(Opening));
		}
	}
	return Openings;
}

TArray<FLBADoorway> FLBARoomAnalysis::FindDoorways(const FLBADoorwayHints& Hints) const
{
	const TMap<FIntVector, FOpening> Openings = FindOpenings();
	TArray<FIntVector> Seeds;
	Openings.GetKeys(Seeds);
	Seeds.Sort(CellLess);

	TArray<FLBADoorway> Doorways;
	TSet<FIntVector> Seen;
	for (const FIntVector& Seed : Seeds)
	{
		if (Seen.Contains(Seed))
		{
			continue;
		}
		// One doorway = neighboring cells crossed the same way (axis) to the same kind of place (room or outside).
		const FOpening& First = Openings[Seed];
		const bool bExit = First.Other != ELBAOpeningEnd::Room;
		TArray<FIntVector> Cells;
		TArray<FIntVector> Todo = { Seed };
		Seen.Add(Seed);
		while (Todo.Num() > 0)
		{
			const FIntVector Cell = Todo.Pop(EAllowShrinking::No);
			Cells.Add(Cell);
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				for (int32 DZ = -1; DZ <= 1; ++DZ)
				{
					for (int32 DY = -1; DY <= 1; ++DY)
					{
						const FIntVector Next(Cell.X + DX, Cell.Y + DY, Cell.Z + DZ);
						const FOpening* Opening = Openings.Find(Next);
						if (Opening && !Seen.Contains(Next) && Opening->Axis == First.Axis && (Opening->Other != ELBAOpeningEnd::Room) == bExit)
						{
							Seen.Add(Next);
							Todo.Add(Next);
						}
					}
				}
			}
		}
		if (Cells.Num() > MaxCells)
		{
			continue;
		}
		Cells.Sort(CellLess);

		FLBADoorway Doorway;
		Doorway.Axis = First.Axis;
		Doorway.bExit = bExit;
		Doorway.FlankHeight = TNumericLimits<int32>::Max();
		bool bOnlyDrops = true;
		for (const FIntVector& Cell : Cells)
		{
			const FOpening& Opening = Openings[Cell];
			Doorway.End = FMath::Max(Doorway.End, Opening.Other);
			Doorway.Width = FMath::Max(Doorway.Width, Opening.Width);
			Doorway.FlankHeight = FMath::Min(Doorway.FlankHeight, Opening.FlankHeight);
			if (Opening.ClearHeight != INDEX_NONE)
			{
				Doorway.ClearHeight = Doorway.ClearHeight == INDEX_NONE ? Opening.ClearHeight : FMath::Min(Doorway.ClearHeight, Opening.ClearHeight);
				Doorway.Signals |= ELBADoorwaySignal::Lintel;
			}
			for (int32 FlankLayout : Opening.FlankLayouts)
			{
				if (Hints.Jambs.Contains(FlankLayout))
				{
					Doorway.Signals |= ELBADoorwaySignal::Jamb;
				}
			}
			if (Hints.Thresholds.Contains(Opening.FloorLayout))
			{
				Doorway.Signals |= ELBADoorwaySignal::Threshold;
			}
			bOnlyDrops &= Opening.Other == ELBAOpeningEnd::Drop;
			Doorway.Flanks.AddUnique(Opening.Flanks[0]);
			Doorway.Flanks.AddUnique(Opening.Flanks[1]);
		}
		for (const FLBACellBox& Zone : Hints.Zones)
		{
			if (IsNear(Cells, Zone))
			{
				Doorway.Signals |= ELBADoorwaySignal::Zone;
				break;
			}
		}
		if (Doorway.Signals == ELBADoorwaySignal::None || bOnlyDrops)
		{
			continue;
		}
		Doorway.Cells = MoveTemp(Cells);
		Doorways.Add(MoveTemp(Doorway));
	}
	return Doorways;
}

bool FLBARoomAnalysis::IsNear(TConstArrayView<FIntVector> Cells, const FLBACellBox& Box)
{
	for (const FIntVector& Cell : Cells)
	{
		if (Cell.X >= Box.Min.X - 1 && Cell.X <= Box.Max.X + 1 && Cell.Z >= Box.Min.Z - 1 && Cell.Z <= Box.Max.Z + 1
			&& Cell.Y >= Box.Min.Y - 2 && Cell.Y <= Box.Max.Y)
		{
			return true;
		}
	}
	return false;
}
