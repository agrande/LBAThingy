#include "Import/LBARoomModel.h"

#include "Import/LBARoomAnalysis.h"

const FIntPoint FLBARoomModel::Sides[4] = { FIntPoint(-1, 0), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(0, 1) };

int32 FLBARoom::GetCeiling(const FIntPoint& Column) const
{
	const int32* Stairs = StairCeilings.Find(Column);
	return Stairs ? *Stairs : Ceiling;
}

int32 FLBARoom::GetFloor(const FIntPoint& Column) const
{
	const int32* Floor = Floors.Find(Column);
	return Floor ? *Floor : Level;
}

TSet<int32> FLBAWallBrickCounts::GetWallBricks() const
{
	TSet<int32> Result;
	for (const TPair<int32, int32>& Pair : Wall)
	{
		if (Pair.Value >= 2 && Pair.Value >= 4 * (Furniture.FindRef(Pair.Key) + Floor.FindRef(Pair.Key)))
		{
			Result.Add(Pair.Key);
		}
	}
	return Result;
}

namespace
{
	/** Floor cells joined to Seed by steps of one cell (sideways, one up or down) through Cells, where Keep allows. */
	TSet<FIntVector> FloodFloors(const FIntVector& Seed, const TSet<FIntVector>& Cells, TFunctionRef<bool(const FIntVector&)> Keep)
	{
		TSet<FIntVector> Result = { Seed };
		TArray<FIntVector> Todo = { Seed };
		while (Todo.Num() > 0)
		{
			const FIntVector Cell = Todo.Pop(EAllowShrinking::No);
			for (const FIntPoint& Side : FLBARoomModel::Sides)
			{
				for (const int32 DY : { 0, 1, -1 })
				{
					const FIntVector Next(Cell.X + Side.X, Cell.Y + DY, Cell.Z + Side.Y);
					if (Cells.Contains(Next) && !Result.Contains(Next) && Keep(Next))
					{
						Result.Add(Next);
						Todo.Add(Next);
					}
				}
			}
		}
		return Result;
	}

	TArray<FIntVector> Sorted(const TSet<FIntVector>& Cells)
	{
		TArray<FIntVector> Result = Cells.Array();
		Result.Sort([](const FIntVector& A, const FIntVector& B)
		{
			return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z;
		});
		return Result;
	}
}

FLBARoomModel::FLBARoomModel(TFunctionRef<int32(int32, int32, int32)> Shape, TFunctionRef<int32(int32, int32, int32)> Brick,
	TFunctionRef<bool(int32)> HasArt, TConstArrayView<FLBADoorway> Doorways, int32 InRoomHeight, const TSet<int32>& InWallBricks)
	: WallBricks(InWallBricks)
	, RoomHeight(InRoomHeight)
{
	Solid.Init(false, SizeXZ * SizeY * SizeXZ);
	Art.Init(false, SizeXZ * SizeY * SizeXZ);
	Bricks.SetNumZeroed(SizeXZ * SizeY * SizeXZ);
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				Solid[ToIndex(X, Y, Z)] = Shape(X, Y, Z) != 0;
				Bricks[ToIndex(X, Y, Z)] = Brick(X, Y, Z);
				Art[ToIndex(X, Y, Z)] = Bricks[ToIndex(X, Y, Z)] != 0 && HasArt(Bricks[ToIndex(X, Y, Z)]);
			}
		}
	}
	for (int32 Index = 0; Index < Doorways.Num(); ++Index)
	{
		for (const FIntVector& Cell : Doorways[Index].Cells)
		{
			DoorColumns.Add(FIntPoint(Cell.X, Cell.Z), TPair<int32, int32>(Index, Cell.Y));
			if (Doorways[Index].bExit)
			{
				ExitColumns.Add(FIntPoint(Cell.X, Cell.Z));
			}
		}
	}

	// Walkable floors, but the tops of walls.
	TSet<FIntVector> Floors;
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y + 1 < SizeY; ++Y)
			{
				if (!IsSolid(X, Y, Z) || (WallBricks.Num() > 0 && WallBricks.Contains(GetBrick(X, Y, Z))))
				{
					continue;
				}
				bool bClear = true;
				for (int32 K = 1; K <= Headroom && Y + K < SizeY; ++K)
				{
					bClear &= !IsSolid(X, Y + K, Z);
				}
				if (bClear)
				{
					Floors.Add(FIntVector(X, Y, Z));
				}
			}
		}
	}
	TArray<TSet<FIntVector>> Spaces;
	{
		TSet<FIntVector> Seen;
		for (const FIntVector& Start : Sorted(Floors))
		{
			if (Seen.Contains(Start))
			{
				continue;
			}
			TSet<FIntVector> Space = FloodFloors(Start, Floors, [](const FIntVector&) { return true; });
			Seen.Append(Space);
			if (Space.Num() >= MinSpaceCells)
			{
				Spaces.Add(MoveTemp(Space));
			}
		}
	}

	// Plateaus: each space's floors around its most common height, then the next one's among the rest.
	TArray<TPair<int32, TSet<FIntVector>>> Plateaus;
	for (const TSet<FIntVector>& Space : Spaces)
	{
		TSet<FIntVector> Left = Space;
		while (Left.Num() > 0)
		{
			TMap<int32, int32> Counts;
			for (const FIntVector& Cell : Left)
			{
				++Counts.FindOrAdd(Cell.Y);
			}
			int32 Level = INDEX_NONE;
			for (const TPair<int32, int32>& Pair : Counts)
			{
				if (Level == INDEX_NONE || Pair.Value > Counts[Level] || (Pair.Value == Counts[Level] && Pair.Key < Level))
				{
					Level = Pair.Key;
				}
			}
			TSet<FIntVector> Seen;
			for (const FIntVector& Seed : Sorted(Left))
			{
				if (Seed.Y != Level || Seen.Contains(Seed))
				{
					continue;
				}
				TSet<FIntVector> Plateau = FloodFloors(Seed, Left, [Level](const FIntVector& Cell) { return FMath::Abs(Cell.Y - Level) <= 1; });
				Seen.Append(Plateau);
				Plateaus.Emplace(Level, MoveTemp(Plateau));
			}
			Left = Left.Difference(Seen);
		}
	}

	// Plateaus that hold rooms: enough floor, some of it with floor all around.
	TArray<bool> bRoomPlateau;
	for (const TPair<int32, TSet<FIntVector>>& Plateau : Plateaus)
	{
		int32 Inner = 0;
		for (const FIntVector& Cell : Plateau.Value)
		{
			bool bInner = true;
			for (const FIntPoint& Side : Sides)
			{
				bInner &= Plateau.Value.Contains(FIntVector(Cell.X + Side.X, Cell.Y, Cell.Z + Side.Y));
			}
			Inner += bInner;
		}
		bRoomPlateau.Add(Plateau.Value.Num() >= MinRoomFloors && Inner >= MinInnerFloors);
		if (bRoomPlateau.Last())
		{
			for (const FIntVector& Cell : Plateau.Value)
			{
				RoomFloorLevels.FindOrAdd(FIntPoint(Cell.X, Cell.Z)).Add(Plateau.Key);
			}
		}
	}
	for (int32 Index = 0; Index < Plateaus.Num(); ++Index)
	{
		if (bRoomPlateau[Index])
		{
			BuildRooms(Plateaus[Index].Key, Plateaus[Index].Value);
		}
	}

	// A doorway inside a room (the room goes around it: a gap between furniture) is part of the room, unless it leads out.
	for (const FLBADoorway& Doorway : Doorways)
	{
		if (Doorway.bExit)
		{
			continue;
		}
		TSet<int32> Touching;
		for (const FIntVector& Cell : Doorway.Cells)
		{
			for (const FIntPoint& Side : Sides)
			{
				for (int32 Index = 0; Index < Rooms.Num(); ++Index)
				{
					if (Rooms[Index].Footprint.Contains(FIntPoint(Cell.X, Cell.Z) + Side))
					{
						Touching.Add(Index);
					}
				}
			}
		}
		if (Touching.Num() == 1)
		{
			FLBARoom& Room = Rooms[*Touching.CreateConstIterator()];
			for (const FIntVector& Cell : Doorway.Cells)
			{
				Room.Footprint.Add(FIntPoint(Cell.X, Cell.Z));
				Room.Floors.Add(FIntPoint(Cell.X, Cell.Z), Cell.Y);
				DoorColumns.Remove(FIntPoint(Cell.X, Cell.Z));
			}
		}
	}

	// A gap between rooms (a stairwell's top, a step between levels) that none of them reaches belongs to the highest of
	// the rooms around it: its ceiling closes the gap.
	{
		TMap<FIntPoint, TSet<int32>> Gaps;
		for (int32 Index = 0; Index < Rooms.Num(); ++Index)
		{
			for (const FIntPoint& Column : Rooms[Index].Footprint)
			{
				for (const FIntPoint& Side : Sides)
				{
					const FIntPoint Next = Column + Side;
					if (IsInGrid(Next) && !DoorColumns.Contains(Next) && !Rooms.ContainsByPredicate([&Next](const FLBARoom& Room) { return Room.Footprint.Contains(Next); }))
					{
						Gaps.FindOrAdd(Next).Add(Index);
					}
				}
			}
		}
		for (const TPair<FIntPoint, TSet<int32>>& Gap : Gaps)
		{
			if (Gap.Value.Num() < 2)
			{
				continue;
			}
			int32 Highest = INDEX_NONE;
			for (const int32 Index : Gap.Value)
			{
				Highest = Highest == INDEX_NONE || Rooms[Index].Level > Rooms[Highest].Level ? Index : Highest;
			}
			if (!IsWall(Gap.Key, Rooms[Highest].Level))
			{
				Rooms[Highest].Footprint.Add(Gap.Key);
			}
		}
	}

	// What lies past each room's border.
	TMap<FIntPoint, TArray<int32, TInlineAllocator<2>>> RoomsAt;
	for (int32 Index = 0; Index < Rooms.Num(); ++Index)
	{
		for (const FIntPoint& Column : Rooms[Index].Footprint)
		{
			RoomsAt.FindOrAdd(Column).Add(Index);
		}
	}
	for (int32 Index = 0; Index < Rooms.Num(); ++Index)
	{
		FLBARoom& Room = Rooms[Index];
		for (const FIntPoint& Column : Room.Footprint)
		{
			for (int32 Side = 0; Side < 4; ++Side)
			{
				const FIntPoint Next = Column + Sides[Side];
				if (Room.Footprint.Contains(Next))
				{
					continue;
				}
				FLBARoomEdge& Edge = Room.Edges.AddDefaulted_GetRef();
				Edge.Column = Column;
				Edge.Side = Side;
				if (!IsInGrid(Next))
				{
					Edge.Kind = ELBARoomEdge::Open;
				}
				else if (const TPair<int32, int32>* Door = DoorColumns.Find(Next))
				{
					Edge.Kind = ELBARoomEdge::Door;
					Edge.Other = Door->Key;
				}
				else if (IsWall(Next, Room.Level, Side))
				{
					Edge.Kind = ELBARoomEdge::Wall;
				}
				else if (const int32* Other = RoomsAt.Contains(Next) ? RoomsAt[Next].FindByPredicate([Index](int32 R) { return R != Index; }) : nullptr)
				{
					Edge.Kind = ELBARoomEdge::Room;
					Edge.Other = *Other;
				}
				else if (Walls.Contains(Next))
				{
					Edge.Kind = ELBARoomEdge::Rise;
				}
				else
				{
					Edge.Kind = ELBARoomEdge::Open;
				}
			}
		}
	}

	// Stairs: small plateaus touching room plateaus of two levels. Their steps keep the room height over them.
	TMap<FIntVector, int32> PlateauOf;
	for (int32 Index = 0; Index < Plateaus.Num(); ++Index)
	{
		for (const FIntVector& Cell : Plateaus[Index].Value)
		{
			PlateauOf.Add(Cell, Index);
		}
	}
	TMap<FIntPoint, int32> Stairs;
	for (int32 Index = 0; Index < Plateaus.Num(); ++Index)
	{
		if (bRoomPlateau[Index])
		{
			continue;
		}
		TSet<int32> Levels;
		for (const FIntVector& Cell : Plateaus[Index].Value)
		{
			for (const FIntPoint& Side : Sides)
			{
				for (const int32 DY : { -1, 0, 1 })
				{
					const int32* Other = PlateauOf.Find(FIntVector(Cell.X + Side.X, Cell.Y + DY, Cell.Z + Side.Y));
					if (Other && bRoomPlateau[*Other])
					{
						Levels.Add(Plateaus[*Other].Key);
					}
				}
			}
		}
		if (Levels.Num() >= 2)
		{
			for (const FIntVector& Cell : Plateaus[Index].Value)
			{
				int32& Step = Stairs.FindOrAdd(FIntPoint(Cell.X, Cell.Z), Cell.Y);
				Step = FMath::Max(Step, Cell.Y);
			}
		}
	}
	for (FLBARoom& Room : Rooms)
	{
		Room.Ceiling = FMath::Min(Room.Level + RoomHeight + 1, SizeY);
		for (const FIntPoint& Column : Room.Footprint)
		{
			if (const int32* Step = Stairs.Find(Column))
			{
				const int32 Ceiling = FMath::Min(*Step + RoomHeight + 1, SizeY);
				if (Ceiling > Room.Ceiling)
				{
					Room.StairCeilings.Add(Column, Ceiling);
				}
			}
		}
	}
}

void FLBARoomModel::BuildRooms(int32 Level, const TSet<FIntVector>& Plateau)
{
	TMap<FIntPoint, int32> FloorY;
	for (const FIntVector& Cell : Plateau)
	{
		const FIntPoint Column(Cell.X, Cell.Z);
		if (!DoorColumns.Contains(Column))
		{
			int32& Y = FloorY.FindOrAdd(Column, Cell.Y);
			Y = FMath::Max(Y, Cell.Y);
		}
	}
	// Floors, and what stands on the floor that is not a wall: furniture, steps.
	auto IsPassable = [&](const FIntPoint& Column, int32 Side)
	{
		if (!IsInGrid(Column) || DoorColumns.Contains(Column))
		{
			return false;
		}
		if (FloorY.Contains(Column))
		{
			return true;
		}
		// A pocket of floor at the room's level (behind furniture, too small to be a space of its own): drawn as a floor
		// (art, not a wall brick: not the top of a wall), not another room's floor. Rules beyond the height rules, which
		// learn the wall bricks, only with them.
		const int32 Top = GetBrick(Column.X, Level, Column.Y);
		if (WallBricks.Num() > 0 && !RoomFloorLevels.Contains(Column) && IsSolid(Column.X, Level, Column.Y) && Art[ToIndex(Column.X, Level, Column.Y)]
			&& !WallBricks.Contains(Top))
		{
			bool bClear = true;
			for (int32 K = 1; K <= Headroom; ++K)
			{
				bClear &= !IsSolid(Column.X, Level + K, Column.Y);
			}
			if (bClear)
			{
				return true;
			}
		}
		if (const TArray<int32, TInlineAllocator<2>>* Levels = RoomFloorLevels.Find(Column))
		{
			if (Levels->ContainsByPredicate([Level](int32 Other) { return Other != Level; }))
			{
				return false;
			}
		}
		if (IsWall(Column, Level, Side))
		{
			Walls.FindOrAdd(Column, Level);
			return false;
		}
		return Level + 1 < SizeY && IsSolid(Column.X, Level + 1, Column.Y);
	};

	TArray<FIntPoint> Starts;
	FloorY.GetKeys(Starts);
	Starts.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
	TSet<FIntPoint> Seen;
	for (const FIntPoint& Start : Starts)
	{
		if (Seen.Contains(Start))
		{
			continue;
		}
		FLBARoom Room;
		Room.Level = Level;
		Room.Footprint.Add(Start);
		TArray<FIntPoint> Todo = { Start };
		while (Todo.Num() > 0)
		{
			const FIntPoint Column = Todo.Pop(EAllowShrinking::No);
			for (int32 Side = 0; Side < 4; ++Side)
			{
				const FIntPoint Next = Column + Sides[Side];
				if (!Room.Footprint.Contains(Next) && IsPassable(Next, Side))
				{
					Room.Footprint.Add(Next);
					Todo.Add(Next);
				}
			}
		}
		Seen.Append(Room.Footprint);
		// Met as a wall from one side, furniture against a wall from another.
		for (const FIntPoint& Column : WallBricks.Num() > 0 ? Room.Footprint : TSet<FIntPoint>())
		{
			Walls.Remove(Column);
		}
		for (const FIntPoint& Column : Room.Footprint)
		{
			if (const int32* Y = FloorY.Find(Column))
			{
				Room.Floors.Add(Column, *Y);
			}
		}
		if (Room.Floors.Num() >= MinRoomColumns)
		{
			Rooms.Add(MoveTemp(Room));
		}
	}
}

bool FLBARoomModel::IsSolid(int32 X, int32 Y, int32 Z) const
{
	return X >= 0 && Y >= 0 && Z >= 0 && X < SizeXZ && Y < SizeY && Z < SizeXZ && Solid[ToIndex(X, Y, Z)];
}

int32 FLBARoomModel::GetBrick(int32 X, int32 Y, int32 Z) const
{
	return X >= 0 && Y >= 0 && Z >= 0 && X < SizeXZ && Y < SizeY && Z < SizeXZ ? Bricks[ToIndex(X, Y, Z)] : 0;
}

int32 FLBARoomModel::GetRun(int32 X, int32 Y, int32 Z, int32* OutDrawnTop) const
{
	int32 Run = 0;
	int32 DrawnTop = 0;
	while (Y + 1 + Run < SizeY && IsSolid(X, Y + 1 + Run, Z))
	{
		if (GetBrick(X, Y + 1 + Run, Z) != 0)
		{
			DrawnTop = Run + 1;
		}
		++Run;
	}
	if (OutDrawnTop)
	{
		*OutDrawnTop = DrawnTop;
	}
	return Run;
}

int32 FLBARoomModel::GetReach(const FIntPoint& Column, int32 Y) const
{
	const int32 Run = GetRun(Column.X, Y, Column.Y);
	if (Run == 0)
	{
		return Y;
	}
	int32 Top = Y + Run;
	for (const FIntPoint& Side : Sides)
	{
		const FIntPoint Next = Column + Side;
		if (IsInGrid(Next) && Top + 1 < SizeY && IsSolid(Next.X, Top + 1, Next.Y) && !IsSolid(Next.X, Y + 1, Next.Y))
		{
			Top = FMath::Max(Top, Top + 1 + GetRun(Next.X, Top + 1, Next.Y));
		}
	}
	return Top;
}

int32 FLBARoomModel::GetWallRun(int32 X, int32 Y, int32 Z, int32* OutDrawnTop) const
{
	int32 End = Y;
	for (int32 K = Y + 1; K < SizeY; ++K)
	{
		if (IsSolid(X, K, Z))
		{
			End = K;
			continue;
		}
		bool bGoesOn = false;
		for (int32 J = 1; J <= WindowGap && K > Y + 1; ++J)
		{
			bGoesOn |= IsSolid(X, K + J, Z);
		}
		if (!bGoesOn)
		{
			break;
		}
	}
	if (OutDrawnTop)
	{
		*OutDrawnTop = 0;
		for (int32 K = End; K > Y; --K)
		{
			if (GetBrick(X, K, Z) != 0)
			{
				*OutDrawnTop = K - Y;
				break;
			}
		}
	}
	return End - Y;
}

bool FLBARoomModel::IsWallLike(const FIntPoint& Column, int32 Level) const
{
	int32 DrawnTop = 0;
	// Through windows, with the learned wall bricks (the height rules that learn them do without).
	const int32 Run = WallBricks.Num() > 0 ? GetWallRun(Column.X, Level, Column.Y, &DrawnTop) : GetRun(Column.X, Level, Column.Y, &DrawnTop);
	if (Run < WallHeight)
	{
		return false;
	}
	if (Run - DrawnTop >= CutCollision)
	{
		return true;
	}
	// The jamb of an exit (rules beyond the height rules, which learn the wall bricks, only with them).
	for (const FIntPoint& Side : Sides)
	{
		if (WallBricks.Num() > 0 && ExitColumns.Contains(Column + Side))
		{
			return true;
		}
	}
	if (WallBricks.Num() > 0)
	{
		int32 Drawn = 0, InWalls = 0;
		for (int32 Y = Level + 1; Y <= Level + Run; ++Y)
		{
			if (const int32 Brick = GetBrick(Column.X, Y, Column.Y))
			{
				++Drawn;
				InWalls += WallBricks.Contains(Brick);
			}
		}
		if (Drawn == 0 || 2 * InWalls >= Drawn)
		{
			return true;
		}
	}
	// A tall column with the outside behind it is a wall, whatever stands at its foot (a bathtub, curtains).
	if (FMath::Max(GetReach(Column, Level) - Level, Run) < TallWall)
	{
		return false;
	}
	if (WallBricks.Num() == 0)
	{
		return true;
	}
	for (const FIntPoint& Side : Sides)
	{
		if (IsOutside(Column + Side, Level))
		{
			return true;
		}
	}
	return false;
}

bool FLBARoomModel::IsOutside(const FIntPoint& Column, int32 Level) const
{
	if (!IsInGrid(Column))
	{
		return true;
	}
	for (int32 Y = Level; Y < FMath::Min(Level + 4, SizeY); ++Y)
	{
		if (IsSolid(Column.X, Y, Column.Y))
		{
			return false;
		}
	}
	return true;
}

bool FLBARoomModel::IsAgainstWall(const FIntPoint& Column, int32 Level, int32 Side) const
{
	if (WallBricks.Num() == 0)
	{
		return false;
	}
	// Columns from this one on, going Side while wall-like: the last is the wall.
	TArray<FIntPoint, TInlineAllocator<AgainstDepth + 2>> Chain = { Column };
	while (true)
	{
		const FIntPoint Next = Chain.Last() + Sides[Side];
		if (!IsInGrid(Next) || !IsWallLike(Next, Level))
		{
			break;
		}
		Chain.Add(Next);
		if (Chain.Num() > AgainstDepth + 1)
		{
			return false;
		}
	}
	if (Chain.Num() < 2)
	{
		return false;
	}
	const FIntPoint Wall = Chain.Last();
	bool bArt = false;
	for (int32 Y = Level + 1; Y < SizeY && !bArt; ++Y)
	{
		bArt = Art[ToIndex(Wall.X, Y, Wall.Y)];
	}
	// Part of a wall across Side.
	const FIntPoint Across(Sides[Side].Y, Sides[Side].X);
	bool bAcross = false;
	for (const FIntPoint& Next : { Wall + Across, Wall - Across })
	{
		bAcross |= IsInGrid(Next) && IsWallLike(Next, Level);
	}
	if (!bArt || !bAcross)
	{
		return false;
	}
	const int32 Top = GetReach(Wall, Level);
	for (int32 Index = 0; Index + 1 < Chain.Num(); ++Index)
	{
		if (GetReach(Chain[Index], Level) >= Top)
		{
			return false;
		}
	}
	return true;
}

bool FLBARoomModel::IsWall(const FIntPoint& Column, int32 Level, int32 Side) const
{
	if (!IsWallLike(Column, Level) || (Side != INDEX_NONE && IsAgainstWall(Column, Level, Side)))
	{
		return false;
	}
	for (const FIntPoint& Offset : Sides)
	{
		if (IsOutside(Column + Offset, Level) || IsWallLike(Column + Offset, Level))
		{
			return true;
		}
	}
	return false;
}

void FLBARoomModel::CountBricks(FLBAWallBrickCounts& Counts) const
{
	for (const FLBARoom& Room : Rooms)
	{
		const int32 Level = Room.Level;
		for (const TPair<FIntPoint, int32>& Floor : Room.Floors)
		{
			if (const int32 Brick = GetBrick(Floor.Key.X, Floor.Value, Floor.Key.Y))
			{
				++Counts.Floor.FindOrAdd(Brick);
			}
		}
		// Back-most walls: drawn, with nothing standing on the floor behind them.
		for (const FLBARoomEdge& Edge : Room.Edges)
		{
			if (Edge.Kind != ELBARoomEdge::Wall)
			{
				continue;
			}
			const FIntPoint Wall = Edge.Column + Sides[Edge.Side];
			int32 DrawnTop = 0;
			const int32 Run = GetRun(Wall.X, Level, Wall.Y, &DrawnTop);
			const FIntPoint Back = Wall + Sides[Edge.Side];
			bool bBacked = false;
			for (int32 Y = Level + 1; Y <= Level + 3 && IsInGrid(Back); ++Y)
			{
				bBacked |= IsSolid(Back.X, Y, Back.Y);
			}
			if (DrawnTop == 0 || bBacked)
			{
				continue;
			}
			for (int32 Y = Level + 1; Y <= Level + Run; ++Y)
			{
				if (const int32 Brick = GetBrick(Wall.X, Y, Wall.Y))
				{
					++Counts.Wall.FindOrAdd(Brick);
				}
			}
		}
		// Free-standing furniture: off the floor, with the room all around.
		for (const FIntPoint& Column : Room.Footprint)
		{
			if (Room.Floors.Contains(Column))
			{
				continue;
			}
			bool bFree = true;
			for (const FIntPoint& Side : Sides)
			{
				bFree &= Room.Footprint.Contains(Column + Side) && !Walls.Contains(Column + Side);
			}
			for (int32 Y = Level + 1; bFree && Y < FMath::Min(SizeY, Level + 6); ++Y)
			{
				if (const int32 Brick = GetBrick(Column.X, Y, Column.Y))
				{
					++Counts.Furniture.FindOrAdd(Brick);
				}
			}
		}
	}
}

FString FLBARoomModel::Describe() const
{
	FString Result;
	for (int32 Index = 0; Index < Rooms.Num(); ++Index)
	{
		const FLBARoom& Room = Rooms[Index];
		FIntPoint Min(SizeXZ, SizeXZ), Max(-1, -1);
		for (const FIntPoint& Column : Room.Footprint)
		{
			Min = Min.ComponentMin(Column);
			Max = Max.ComponentMax(Column);
		}
		int32 Kinds[5] = {};
		for (const FLBARoomEdge& Edge : Room.Edges)
		{
			++Kinds[int32(Edge.Kind)];
		}
		Result += FString::Printf(TEXT("R%d level %d ceiling %d floors %d footprint %d box x%d-%d z%d-%d stairs %d edges wall %d door %d room %d rise %d open %d\n"),
			Index, Room.Level, Room.Ceiling, Room.Floors.Num(), Room.Footprint.Num(), Min.X, Max.X, Min.Y, Max.Y, Room.StairCeilings.Num(),
			Kinds[0], Kinds[1], Kinds[2], Kinds[3], Kinds[4]);
	}
	return Result;
}
