#include "Import/LBARoomPlan.h"

const FIntPoint FLBARoomPlan::Sides[4] = { FIntPoint(-1, 0), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(0, 1) };

namespace
{
	const TCHAR* SideNames[4] = { TEXT("-X"), TEXT("+X"), TEXT("-Z"), TEXT("+Z") };

	const TCHAR* ConnectorName(ELBAConnector Kind)
	{
		static const TCHAR* Names[] = { TEXT("stairs"), TEXT("passage"), TEXT("exit"), TEXT("alcove"), TEXT("pocket") };
		return Names[int32(Kind)];
	}

	bool SortCells(const FIntVector& A, const FIntVector& B)
	{
		return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z;
	}

	bool SortColumns(const FIntPoint& A, const FIntPoint& B)
	{
		return A.X != B.X ? A.X < B.X : A.Y < B.Y;
	}

	/** Columns joined to Start through Keep, four ways. */
	TSet<FIntPoint> FloodColumns(const FIntPoint& Start, TFunctionRef<bool(const FIntPoint&)> Keep)
	{
		TSet<FIntPoint> Result = { Start };
		TArray<FIntPoint> Todo = { Start };
		while (Todo.Num() > 0)
		{
			const FIntPoint Column = Todo.Pop(EAllowShrinking::No);
			for (const FIntPoint& Side : FLBARoomPlan::Sides)
			{
				const FIntPoint Next = Column + Side;
				if (!Result.Contains(Next) && Keep(Next))
				{
					Result.Add(Next);
					Todo.Add(Next);
				}
			}
		}
		return Result;
	}

	/** Floor cells reached from a floor cell in one step: sideways, one up or one down. */
	void ForEachStep(const FIntVector& Cell, TFunctionRef<void(const FIntVector&)> Visit)
	{
		for (const FIntPoint& Side : FLBARoomPlan::Sides)
		{
			for (const int32 DY : { 0, 1, -1 })
			{
				Visit(FIntVector(Cell.X + Side.X, Cell.Y + DY, Cell.Z + Side.Y));
			}
		}
	}

	/** All maximal rectangles (min and max columns, included) of the Free columns: grown as far as they go both ways. */
	TArray<TPair<FIntPoint, FIntPoint>> MaximalRectangles(const TBitArray<>& Free)
	{
		auto IsFree = [&Free](int32 X, int32 Z) { return Free[FLBARoomPlan::ToColumnIndex(FIntPoint(X, Z))]; };
		TArray<TPair<FIntPoint, FIntPoint>> Found;
		TArray<int32> Heights;
		Heights.SetNumZeroed(FLBARoomPlan::SizeXZ);
		for (int32 Z = 0; Z < FLBARoomPlan::SizeXZ; ++Z)
		{
			for (int32 X = 0; X < FLBARoomPlan::SizeXZ; ++X)
			{
				Heights[X] = IsFree(X, Z) ? Heights[X] + 1 : 0;
			}
			// Histogram stack: (start column, height).
			TArray<FIntPoint> Stack;
			for (int32 X = 0; X <= FLBARoomPlan::SizeXZ; ++X)
			{
				const int32 Height = X < FLBARoomPlan::SizeXZ ? Heights[X] : 0;
				int32 Start = X;
				while (Stack.Num() > 0 && Stack.Last().Y >= Height)
				{
					const FIntPoint Top = Stack.Pop(EAllowShrinking::No);
					if (Top.Y > 0 && (Stack.Num() == 0 || Stack.Last().Y < Top.Y))
					{
						Found.Emplace(FIntPoint(Top.X, Z - Top.Y + 1), FIntPoint(X - 1, Z));
					}
					Start = Top.X;
				}
				if (Stack.Num() == 0 || Stack.Last().Y < Height)
				{
					Stack.Emplace(Start, Height);
				}
			}
		}
		// Only those that cannot grow down either.
		return Found.FilterByPredicate([&IsFree](const TPair<FIntPoint, FIntPoint>& Rect)
		{
			if (Rect.Value.Y == FLBARoomPlan::SizeXZ - 1)
			{
				return true;
			}
			for (int32 X = Rect.Key.X; X <= Rect.Value.X; ++X)
			{
				if (!IsFree(X, Rect.Value.Y + 1))
				{
					return true;
				}
			}
			return false;
		});
	}
}

ELBAColumn FLBAPlanLevel::Get(const FIntPoint& Column) const
{
	return FLBARoomPlan::IsInGrid(Column) ? Columns[FLBARoomPlan::ToColumnIndex(Column)] : ELBAColumn::Other;
}

ELBAWallRule FLBAPlanLevel::GetRule(const FIntPoint& Column) const
{
	return FLBARoomPlan::IsInGrid(Column) ? Rules[FLBARoomPlan::ToColumnIndex(Column)] : ELBAWallRule::None;
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

/** The column rules of one level: what is a wall, furniture, a post (see FLBARoomPlan). */
class FLBARoomPlan::FLevelRules
{
public:
	FLevelRules(const FLBARoomPlan& InPlan, const FLBAGridCells& InCells, const TSet<int32>* InWallBricks, const TSet<FIntPoint>& InExitColumns, int32 InLevel)
		: Plan(InPlan), Cells(InCells), WallBricks(InWallBricks), ExitColumns(InExitColumns), Level(InLevel)
	{
		WallLike.SetNumUninitialized(SizeXZ * SizeXZ);
		for (int32 Z = 0; Z < SizeXZ; ++Z)
		{
			for (int32 X = 0; X < SizeXZ; ++X)
			{
				WallLike[ToColumnIndex(FIntPoint(X, Z))] = FindWallRule(X, Z);
			}
		}
	}

	void Classify(FLBAPlanLevel& Out, const TSet<FIntPoint>& Floors, const TSet<FIntPoint>& Stairs) const
	{
		Out.Height = Level;
		Out.Columns.Init(ELBAColumn::Other, SizeXZ * SizeXZ);
		Out.Rules.Init(ELBAWallRule::None, SizeXZ * SizeXZ);
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Z = 0; Z < SizeXZ; ++Z)
			{
				const FIntPoint Column(X, Z);
				const int32 Index = ToColumnIndex(Column);
				if (Floors.Contains(Column))
				{
					Out.Columns[Index] = ELBAColumn::Floor;
				}
				else if (Stairs.Contains(Column))
				{
					Out.Columns[Index] = ELBAColumn::Stairs;
				}
				else if (IsWall(Column, Out.Rules[Index]))
				{
					Out.Columns[Index] = ELBAColumn::Wall;
				}
				else if (StandsOnFloor(Column) && !Plan.GetFloorLevels(Column).ContainsByPredicate([this](int32 Other) { return Other != Level; }))
				{
					Out.Columns[Index] = ELBAColumn::Furniture;
				}
			}
		}
		// Furniture against a wall: judged from the floors around, once every column is classed.
		if (WallBricks)
		{
			for (int32 Index = 0; Index < Out.Columns.Num(); ++Index)
			{
				if (Out.Columns[Index] != ELBAColumn::Wall || (Out.Rules[Index] != ELBAWallRule::Masonry && Out.Rules[Index] != ELBAWallRule::Exterior))
				{
					continue;
				}
				const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
				for (int32 Side = 0; Side < 4; ++Side)
				{
					if (Out.Get(Column - Sides[Side]) == ELBAColumn::Floor && IsAgainstWall(Column, Side))
					{
						Out.Columns[Index] = ELBAColumn::Furniture;
						Out.Rules[Index] = ELBAWallRule::Against;
						break;
					}
				}
			}
		}
		// Posts: wall columns in no straight run of MinWallRun.
		Out.Runs = FindRuns(Out);
		TSet<FIntPoint> InRun;
		for (const TArray<FIntPoint>& Run : Out.Runs)
		{
			InRun.Append(Run);
		}
		for (int32 Index = 0; Index < Out.Columns.Num(); ++Index)
		{
			const FIntPoint Column(Index % SizeXZ, Index / SizeXZ);
			if (Out.Columns[Index] == ELBAColumn::Wall && !InRun.Contains(Column))
			{
				Out.Columns[Index] = StandsOnFloor(Column) ? ELBAColumn::Furniture : ELBAColumn::Other;
				Out.Rules[Index] = ELBAWallRule::Post;
			}
		}
		Out.Runs = FindRuns(Out);
	}

	/** Solid cells stacked over the level in the column, through window gaps; OutDrawnTop: cells up to the highest drawn. */
	int32 Run(const FIntPoint& Column, int32* OutDrawnTop = nullptr) const
	{
		return Plan.GetWallRun(Column.X, Level, Column.Y, OutDrawnTop);
	}

	/** Top of the wall surface over the level: its run, continued by the band next door that tops a camera-side wall's stub. */
	int32 Reach(const FIntPoint& Column) const
	{
		const int32 Height = Run(Column);
		if (Height == 0)
		{
			return Level;
		}
		int32 Top = Level + Height;
		for (const FIntPoint& Side : Sides)
		{
			const FIntPoint Next = Column + Side;
			if (IsInGrid(Next) && Top + 1 < SizeY && Plan.IsSolid(Next.X, Top + 1, Next.Y) && !Plan.IsSolid(Next.X, Level + 1, Next.Y))
			{
				int32 Above = 0;
				while (Top + 2 + Above < SizeY && Plan.IsSolid(Next.X, Top + 2 + Above, Next.Y))
				{
					++Above;
				}
				Top = FMath::Max(Top, Top + 1 + Above);
			}
		}
		return Top;
	}

	ELBAWallRule GetWallLike(const FIntPoint& Column) const
	{
		return IsInGrid(Column) ? WallLike[ToColumnIndex(Column)] : ELBAWallRule::None;
	}

private:
	bool IsOutside(const FIntPoint& Column) const
	{
		if (!IsInGrid(Column))
		{
			return true;
		}
		for (int32 Y = Level; Y < FMath::Min(Level + 4, SizeY); ++Y)
		{
			if (Plan.IsSolid(Column.X, Y, Column.Y))
			{
				return false;
			}
		}
		return true;
	}

	/** The wall rules (FLBARoomPlan's class comment), in order. */
	ELBAWallRule FindWallRule(int32 X, int32 Z) const
	{
		const FIntPoint Column(X, Z);
		int32 DrawnTop = 0;
		const int32 Height = Run(Column, &DrawnTop);
		if (Height < WallHeight)
		{
			return ELBAWallRule::None;
		}
		if (Height - DrawnTop >= CutCollision || (DrawnTop > 0 && Cells.IsFrame && Cells.IsFrame(X, Level + DrawnTop, Z)))
		{
			return ELBAWallRule::Cut;
		}
		for (const FIntPoint& Side : Sides)
		{
			if (ExitColumns.Contains(Column + Side))
			{
				return ELBAWallRule::Jamb;
			}
		}
		int32 Drawn = 0, InWalls = 0;
		for (int32 Y = Level + 1; Y <= Level + Height; ++Y)
		{
			if (const int32 Brick = Plan.GetBrick(X, Y, Z))
			{
				++Drawn;
				InWalls += WallBricks && WallBricks->Contains(Brick);
			}
		}
		if (Drawn == 0)
		{
			return ELBAWallRule::Invisible;
		}
		const bool bTall = FMath::Max(Reach(Column) - Level, Height) >= TallWall;
		if (!WallBricks)
		{
			return bTall ? ELBAWallRule::Tall : ELBAWallRule::None;
		}
		if (2 * InWalls >= Drawn)
		{
			return ELBAWallRule::Masonry;
		}
		if (bTall)
		{
			for (const FIntPoint& Side : Sides)
			{
				if (IsOutside(Column + Side))
				{
					return ELBAWallRule::Exterior;
				}
			}
		}
		return ELBAWallRule::None;
	}

	/** Wall-like and going on: a wall-like column next to it, or the outside. OutRule: why (Pillar: alone). */
	bool IsWall(const FIntPoint& Column, ELBAWallRule& OutRule) const
	{
		const ELBAWallRule Rule = GetWallLike(Column);
		if (Rule == ELBAWallRule::None)
		{
			return false;
		}
		for (const FIntPoint& Side : Sides)
		{
			const FIntPoint Next = Column + Side;
			if (!IsInGrid(Next) || GetWallLike(Next) != ELBAWallRule::None || IsOutside(Next))
			{
				OutRule = Rule;
				return true;
			}
		}
		OutRule = ELBAWallRule::Pillar;
		return false;
	}

	/** The cell over the floor is solid, and the first drawn piece above the floor begins at the floor or above. */
	bool StandsOnFloor(const FIntPoint& Column) const
	{
		if (Level + 1 >= SizeY || !Plan.IsSolid(Column.X, Level + 1, Column.Y))
		{
			return false;
		}
		for (int32 Y = Level + 1; Y < SizeY; ++Y)
		{
			if (!Plan.IsSolid(Column.X, Y, Column.Y))
			{
				return true;
			}
			if (Cells.Layout(Column.X, Y, Column.Y) >= 0)
			{
				return Cells.LayoutBase(Column.X, Y, Column.Y) >= Level;
			}
		}
		return true;
	}

	/** Met from a floor going Sides[Side], the column stands against a wall: the wall-like columns going on end within
	 * AgainstDepth at a drawn wall, part of a wall across, taller than every column before it. */
	bool IsAgainstWall(const FIntPoint& Column, int32 Side) const
	{
		TArray<FIntPoint, TInlineAllocator<AgainstDepth + 2>> Chain = { Column };
		while (true)
		{
			const FIntPoint Next = Chain.Last() + Sides[Side];
			if (!IsInGrid(Next) || GetWallLike(Next) == ELBAWallRule::None)
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
			const int32 Brick = Plan.GetBrick(Wall.X, Y, Wall.Y);
			bArt = Brick != 0 && Cells.HasArt(Brick);
		}
		if (!bArt)
		{
			return false;
		}
		const FIntPoint Across(Sides[Side].Y, Sides[Side].X);
		if (GetWallLike(Wall + Across) == ELBAWallRule::None && GetWallLike(Wall - Across) == ELBAWallRule::None)
		{
			return false;
		}
		const int32 Top = Reach(Wall);
		for (int32 Index = 0; Index + 1 < Chain.Num(); ++Index)
		{
			if (Reach(Chain[Index]) >= Top)
			{
				return false;
			}
		}
		return true;
	}

	/** Maximal straight runs of at least MinWallRun wall columns, along X, then along Z. */
	static TArray<TArray<FIntPoint>> FindRuns(const FLBAPlanLevel& Level)
	{
		TArray<TArray<FIntPoint>> Result;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			for (int32 A = 0; A < SizeXZ; ++A)
			{
				TArray<FIntPoint> Current;
				for (int32 B = 0; B <= SizeXZ; ++B)
				{
					const FIntPoint Column = Axis == 0 ? FIntPoint(B, A) : FIntPoint(A, B);
					if (B < SizeXZ && Level.Get(Column) == ELBAColumn::Wall)
					{
						Current.Add(Column);
						continue;
					}
					if (Current.Num() >= MinWallRun)
					{
						Result.Add(Current);
					}
					Current.Reset();
				}
			}
		}
		return Result;
	}

	const FLBARoomPlan& Plan;
	const FLBAGridCells& Cells;
	const TSet<int32>* WallBricks;
	const TSet<FIntPoint>& ExitColumns;
	int32 Level;
	TArray<ELBAWallRule> WallLike;
};

FLBARoomPlan::FLBARoomPlan(const FLBAGridCells& Cells, const TSet<int32>* WallBricks, TConstArrayView<FLBACellBox> ExitZones, int32 InRoomHeight)
	: RoomHeight(InRoomHeight)
	, Zones(ExitZones)
{
	Solid.Init(false, SizeXZ * SizeY * SizeXZ);
	Bricks.SetNumZeroed(SizeXZ * SizeY * SizeXZ);
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				Solid[ToIndex(X, Y, Z)] = Cells.Shape(X, Y, Z) != 0;
				Bricks[ToIndex(X, Y, Z)] = Cells.Brick(X, Y, Z);
			}
		}
	}

	// Walkable floors: solid cells with Headroom free cells over them.
	TSet<FIntVector> Floors;
	TMap<int32, TSet<FIntPoint>> ByHeight;
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Y = 0; Y + 1 < SizeY; ++Y)
			{
				bool bClear = IsSolid(X, Y, Z);
				for (int32 K = 1; K <= Headroom && bClear; ++K)
				{
					bClear = !IsSolid(X, Y + K, Z);
				}
				if (bClear)
				{
					Floors.Add(FIntVector(X, Y, Z));
					ByHeight.FindOrAdd(Y).Add(FIntPoint(X, Z));
				}
			}
		}
	}

	// Levels.
	ByHeight.KeySort(TLess<int32>());
	for (const TPair<int32, TSet<FIntPoint>>& Height : ByHeight)
	{
		TArray<FIntPoint> Starts = Height.Value.Array();
		Starts.Sort(SortColumns);
		TSet<FIntPoint> Seen;
		for (const FIntPoint& Start : Starts)
		{
			if (Seen.Contains(Start))
			{
				continue;
			}
			const TSet<FIntPoint> Part = FloodColumns(Start, [&Height](const FIntPoint& Column) { return Height.Value.Contains(Column); });
			Seen.Append(Part);
			int32 Inner = 0;
			for (const FIntPoint& Column : Part)
			{
				bool bInner = true;
				for (const FIntPoint& Side : Sides)
				{
					bInner &= Part.Contains(Column + Side);
				}
				Inner += bInner;
			}
			if (Part.Num() >= MinLevelFloors && Inner >= MinInnerFloors)
			{
				for (const FIntPoint& Column : Part)
				{
					FloorLevels.FindOrAdd(Column).Add(Height.Key);
				}
				LevelHeights.AddUnique(Height.Key);
			}
		}
	}
	TSet<FIntVector> LevelFloors;
	for (const TPair<FIntPoint, TArray<int32, TInlineAllocator<2>>>& Pair : FloorLevels)
	{
		for (const int32 Y : Pair.Value)
		{
			LevelFloors.Add(FIntVector(Pair.Key.X, Y, Pair.Key.Y));
		}
	}

	// R1 STAIRS.
	{
		TArray<FIntVector> OffLevel = Floors.Array().FilterByPredicate([this](const FIntVector& Cell) { return !LevelHeights.Contains(Cell.Y); });
		OffLevel.Sort(SortCells);
		const TSet<FIntVector> OffLevelSet(OffLevel);
		TSet<FIntVector> Seen;
		for (const FIntVector& Start : OffLevel)
		{
			if (Seen.Contains(Start))
			{
				continue;
			}
			TSet<FIntVector> Part = { Start };
			TArray<FIntVector> Todo = { Start };
			while (Todo.Num() > 0)
			{
				ForEachStep(Todo.Pop(EAllowShrinking::No), [&](const FIntVector& Next)
				{
					if (OffLevelSet.Contains(Next) && !Part.Contains(Next))
					{
						Part.Add(Next);
						Todo.Add(Next);
					}
				});
			}
			Seen.Append(Part);
			TSet<int32> Touched;
			for (const FIntVector& Cell : Part)
			{
				ForEachStep(Cell, [&](const FIntVector& Next)
				{
					if (LevelFloors.Contains(Next))
					{
						Touched.Add(Next.Y);
					}
				});
			}
			if (Touched.Num() >= 2)
			{
				FLBAPlanFlight& Flight = Flights.AddDefaulted_GetRef();
				Flight.Cells = Part.Array();
				Flight.Cells.Sort(SortCells);
				Flight.Levels = Touched.Array();
				Flight.Levels.Sort();
			}
		}
	}
	TSet<FIntPoint> StairColumns;
	for (const FLBAPlanFlight& Flight : Flights)
	{
		for (const FIntVector& Cell : Flight.Cells)
		{
			StairColumns.Add(FIntPoint(Cell.X, Cell.Z));
		}
	}

	TSet<FIntPoint> ExitColumns;
	for (const FLBACellBox& Zone : Zones)
	{
		for (int32 X = Zone.Min.X; X <= Zone.Max.X; ++X)
		{
			for (int32 Z = Zone.Min.Z; Z <= Zone.Max.Z; ++Z)
			{
				if (IsInGrid(FIntPoint(X, Z)))
				{
					ExitColumns.Add(FIntPoint(X, Z));
				}
			}
		}
	}

	// Columns, then rooms, level by level.
	for (const int32 Height : LevelHeights)
	{
		const FLevelRules Rules(*this, Cells, WallBricks, ExitColumns, Height);
		FLBAPlanLevel& Level = Levels.AddDefaulted_GetRef();
		Rules.Classify(Level, ByHeight[Height], StairColumns);
		BuildRooms(Level, Rules);
	}
	for (int32 Index = 0; Index < Rooms.Num(); ++Index)
	{
		FLBAPlanRoom& Room = Rooms[Index];
		Room.Ceiling = FMath::Min(Room.Level + RoomHeight + 1, SizeY);
		for (int32 X = Room.Min.X; X <= Room.Max.X; ++X)
		{
			for (int32 Z = Room.Min.Y; Z <= Room.Max.Y; ++Z)
			{
				RoomAt.Add(FIntVector(X, Room.Level, Z), Index);
			}
		}
	}
	BuildConnectors();
	BuildSpaces();
}

void FLBARoomPlan::BuildRooms(FLBAPlanLevel& Level, const FLevelRules& Rules)
{
	TBitArray<> Free(false, SizeXZ * SizeXZ);
	TBitArray<> Floor(false, SizeXZ * SizeXZ);
	for (int32 Index = 0; Index < Level.Columns.Num(); ++Index)
	{
		Free[Index] = Level.Columns[Index] == ELBAColumn::Floor || Level.Columns[Index] == ELBAColumn::Furniture;
		Floor[Index] = Level.Columns[Index] == ELBAColumn::Floor;
	}
	auto IsFree = [&Free](const FIntPoint& Column) { return IsInGrid(Column) && Free[ToColumnIndex(Column)]; };

	// Each wall end that meets no other wall: the free columns in its line, and the column that stops them.
	TMap<FIntPoint, int32> InRuns;
	for (const TArray<FIntPoint>& Run : Level.Runs)
	{
		for (const FIntPoint& Column : Run)
		{
			++InRuns.FindOrAdd(Column);
		}
	}
	struct FWallEnd
	{
		int32 Run = 0;
		TArray<FIntPoint> Line;
		FIntPoint Stop;
	};
	TArray<FWallEnd> Ends;
	for (int32 RunIndex = 0; RunIndex < Level.Runs.Num(); ++RunIndex)
	{
		const TArray<FIntPoint>& Run = Level.Runs[RunIndex];
		const FIntPoint Along = Run[0].X == Run.Last().X ? FIntPoint(0, 1) : FIntPoint(1, 0);
		const TPair<FIntPoint, FIntPoint> RunEnds[2] = { TPair<FIntPoint, FIntPoint>(Run[0], FIntPoint(0, 0) - Along), TPair<FIntPoint, FIntPoint>(Run.Last(), Along) };
		for (const TPair<FIntPoint, FIntPoint>& End : RunEnds)
		{
			if (InRuns[End.Key] > 1)
			{
				continue;
			}
			FWallEnd WallEnd;
			WallEnd.Run = RunIndex;
			FIntPoint Column = End.Key + End.Value;
			while (IsFree(Column))
			{
				WallEnd.Line.Add(Column);
				Column += End.Value;
			}
			WallEnd.Stop = Column;
			if (WallEnd.Line.Num() > 0)
			{
				Ends.Add(MoveTemp(WallEnd));
			}
		}
	}
	auto IsAlongZ = [](const TArray<FIntPoint>& Run) { return Run[0].X == Run.Last().X; };
	// R2 GAP: the line reaches a wall in a run along the same axis.
	for (const FWallEnd& End : Ends)
	{
		const TArray<FIntPoint>& Run = Level.Runs[End.Run];
		if (IsInGrid(End.Stop) && Level.Runs.ContainsByPredicate([&](const TArray<FIntPoint>& Other) { return IsAlongZ(Other) == IsAlongZ(Run) && Other.Contains(End.Stop); }))
		{
			Level.Gaps.Append(End.Line);
		}
	}
	// R3 PARTITION: a partition's line, up to a gap in another wall's line.
	for (const FWallEnd& End : Ends)
	{
		const TArray<FIntPoint>& Run = Level.Runs[End.Run];
		int32 Own = 0;
		for (const FIntPoint& Column : Run)
		{
			Own += InRuns[Column] == 1;
		}
		if (Level.Gaps.Contains(End.Line[0]) || Own < MinPartition)
		{
			continue;
		}
		for (const FIntPoint& Column : End.Line)
		{
			if (Level.Gaps.Contains(Column))
			{
				break;
			}
			Level.PartitionLines.Add(Column);
		}
	}
	for (const FIntPoint& Column : Level.Gaps)
	{
		Free[ToColumnIndex(Column)] = false;
	}
	for (const FIntPoint& Column : Level.PartitionLines)
	{
		Free[ToColumnIndex(Column)] = false;
	}

	// R4 RECTANGLE.
	auto IsWallAt = [&Level](int32 X, int32 Z) { return Level.Get(FIntPoint(X, Z)) == ELBAColumn::Wall; };
	auto IsWalled = [&IsWallAt](const FIntPoint& Min, const FIntPoint& Max)
	{
		for (int32 X = Min.X - 1; X <= Max.X + 1; ++X)
		{
			if (IsWallAt(X, Min.Y - 1) || IsWallAt(X, Max.Y + 1))
			{
				return true;
			}
		}
		for (int32 Z = Min.Y - 1; Z <= Max.Y + 1; ++Z)
		{
			if (IsWallAt(Min.X - 1, Z) || IsWallAt(Max.X + 1, Z))
			{
				return true;
			}
		}
		return false;
	};
	auto CountIn = [](const TBitArray<>& Bits, const FIntPoint& Min, const FIntPoint& Max)
	{
		int32 Count = 0;
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Z = Min.Y; Z <= Max.Y; ++Z)
			{
				Count += Bits[ToColumnIndex(FIntPoint(X, Z))];
			}
		}
		return Count;
	};
	while (true)
	{
		bool bFound = false;
		FIntVector4 BestKey;
		TPair<FIntPoint, FIntPoint> Best;
		for (const TPair<FIntPoint, FIntPoint>& Rect : MaximalRectangles(Free))
		{
			const FIntPoint& Min = Rect.Key;
			const FIntPoint& Max = Rect.Value;
			const int32 Floors = CountIn(Floor, Min, Max);
			if (Floors < MinFloors || FMath::Min(Max.X - Min.X, Max.Y - Min.Y) + 1 < MinRoomWidth || !IsWalled(Min, Max))
			{
				continue;
			}
			// The largest; then the one with the most floor; then the first from the grid's origin.
			const FIntVector4 Key((Max.X - Min.X + 1) * (Max.Y - Min.Y + 1), Floors, -Min.X, -Min.Y);
			auto Greater = [](const FIntVector4& A, const FIntVector4& B)
			{
				return A.X != B.X ? A.X > B.X : A.Y != B.Y ? A.Y > B.Y : A.Z != B.Z ? A.Z > B.Z : A.W > B.W;
			};
			if (!bFound || Greater(Key, BestKey))
			{
				bFound = true;
				BestKey = Key;
				Best = Rect;
			}
		}
		if (!bFound)
		{
			break;
		}
		FLBAPlanRoom& Room = Rooms.AddDefaulted_GetRef();
		Room.Level = Level.Height;
		Room.Min = Best.Key;
		Room.Max = Best.Value;
		Room.Floors = CountIn(Floor, Best.Key, Best.Value);
		Room.Furniture = CountIn(Free, Best.Key, Best.Value) - Room.Floors;
		for (int32 X = Best.Key.X; X <= Best.Value.X; ++X)
		{
			for (int32 Z = Best.Key.Y; Z <= Best.Value.Y; ++Z)
			{
				Free[ToColumnIndex(FIntPoint(X, Z))] = false;
			}
		}
	}
}

void FLBARoomPlan::BuildConnectors()
{
	// Pieces: the columns of each level no room took, by contact; then each flight's.
	struct FPiece
	{
		int32 Level = INDEX_NONE;
		TArray<FIntPoint> Columns;
	};
	TArray<FPiece> Pieces;
	for (const FLBAPlanLevel& Level : Levels)
	{
		auto IsLeft = [this, &Level](const FIntPoint& Column)
		{
			const ELBAColumn Kind = Level.Get(Column);
			return IsInGrid(Column) && (Kind == ELBAColumn::Floor || Kind == ELBAColumn::Furniture) && !RoomAt.Contains(FIntVector(Column.X, Level.Height, Column.Y));
		};
		TSet<FIntPoint> Seen;
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (int32 Z = 0; Z < SizeXZ; ++Z)
			{
				const FIntPoint Start(X, Z);
				if (Seen.Contains(Start) || !IsLeft(Start))
				{
					continue;
				}
				const TSet<FIntPoint> Part = FloodColumns(Start, IsLeft);
				Seen.Append(Part);
				FPiece& Piece = Pieces.AddDefaulted_GetRef();
				Piece.Level = Level.Height;
				Piece.Columns = Part.Array();
				Piece.Columns.Sort(SortColumns);
			}
		}
	}
	const int32 FirstFlight = Pieces.Num();
	TMap<FIntPoint, TArray<int32, TInlineAllocator<2>>> FlightHeights;
	for (const FLBAPlanFlight& Flight : Flights)
	{
		FPiece& Piece = Pieces.AddDefaulted_GetRef();
		for (const FIntVector& Cell : Flight.Cells)
		{
			Piece.Columns.AddUnique(FIntPoint(Cell.X, Cell.Z));
			FlightHeights.FindOrAdd(FIntPoint(Cell.X, Cell.Z)).AddUnique(Cell.Y);
		}
	}

	// A level's piece leads onto a flight where one of its floors is a step away from the flight's cells.
	TArray<int32> Parent;
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		Parent.Add(Index);
	}
	auto Find = [&Parent](int32 Index)
	{
		while (Parent[Index] != Index)
		{
			Parent[Index] = Parent[Parent[Index]];
			Index = Parent[Index];
		}
		return Index;
	};
	for (int32 FlightIndex = 0; FlightIndex < Flights.Num(); ++FlightIndex)
	{
		TSet<FIntVector> Near;
		for (const FIntVector& Cell : Flights[FlightIndex].Cells)
		{
			ForEachStep(Cell, [&Near](const FIntVector& Next) { Near.Add(Next); });
		}
		for (int32 PieceIndex = 0; PieceIndex < FirstFlight; ++PieceIndex)
		{
			const FPiece& Piece = Pieces[PieceIndex];
			const FLBAPlanLevel* Level = FindLevel(Piece.Level);
			if (Piece.Columns.ContainsByPredicate([&](const FIntPoint& Column)
				{
					return Near.Contains(FIntVector(Column.X, Piece.Level, Column.Y)) && Level->Get(Column) == ELBAColumn::Floor;
				}))
			{
				Parent[Find(PieceIndex)] = Find(FirstFlight + FlightIndex);
			}
		}
	}
	TArray<int32> Roots;
	TMap<int32, TArray<int32>> Groups;
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const int32 Root = Find(Index);
		if (!Groups.Contains(Root))
		{
			Roots.Add(Root);
		}
		Groups.FindOrAdd(Root).Add(Index);
	}

	for (const int32 Root : Roots)
	{
		TMap<int32, int32> Touching;
		bool bExit = false;
		for (const int32 PieceIndex : Groups[Root])
		{
			const FPiece& Piece = Pieces[PieceIndex];
			for (const FIntPoint& Column : Piece.Columns)
			{
				bExit |= Piece.Level != INDEX_NONE && IsInZone(Column, Piece.Level);
				for (const FIntPoint& Side : Sides)
				{
					const FIntPoint Next = Column + Side;
					if (Piece.Level != INDEX_NONE)
					{
						if (const int32* Room = RoomAt.Find(FIntVector(Next.X, Piece.Level, Next.Y)))
						{
							++Touching.FindOrAdd(*Room);
						}
						continue;
					}
					for (const int32 Y : FlightHeights[Column])
					{
						for (const int32 LY : { Y - 1, Y + 1 })
						{
							const int32* Room = RoomAt.Find(FIntVector(Next.X, LY, Next.Y));
							if (Room && FindLevel(LY)->Get(Next) == ELBAColumn::Floor)
							{
								++Touching.FindOrAdd(*Room);
							}
						}
					}
				}
			}
		}
		const int32 Index = Connectors.Num();
		FLBAPlanConnector& Connector = Connectors.AddDefaulted_GetRef();
		Touching.GetKeys(Connector.Rooms);
		Connector.Rooms.Sort();
		for (const int32 Room : Connector.Rooms)
		{
			Connector.Levels.AddUnique(Rooms[Room].Level);
		}
		Connector.Levels.Sort();
		Connector.Kind = Connector.Levels.Num() >= 2 ? ELBAConnector::Stairs
			: Connector.Rooms.Num() >= 2 ? ELBAConnector::Passage
			: Connector.Rooms.Num() == 1 && bExit ? ELBAConnector::Exit
			: Connector.Rooms.Num() == 1 ? ELBAConnector::Alcove
			: ELBAConnector::Pocket;
		if (Connector.Kind == ELBAConnector::Passage)
		{
			Connector.Width = TNumericLimits<int32>::Max();
			for (const TPair<int32, int32>& Pair : Touching)
			{
				Connector.Width = FMath::Min(Connector.Width, Pair.Value);
			}
		}
		for (const int32 PieceIndex : Groups[Root])
		{
			for (const FIntPoint& Column : Pieces[PieceIndex].Columns)
			{
				Connector.Columns.Emplace(Column.X, Pieces[PieceIndex].Level, Column.Y);
				ConnectorAt.Add(FIntVector(Column.X, Pieces[PieceIndex].Level, Column.Y), Index);
			}
		}
	}
}

void FLBARoomPlan::BuildSpaces()
{
	for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
	{
		if (Rooms[RoomIndex].Space != INDEX_NONE)
		{
			continue;
		}
		const int32 SpaceIndex = Spaces.Num();
		FLBAPlanSpace& Space = Spaces.AddDefaulted_GetRef();
		Space.Level = Rooms[RoomIndex].Level;
		Space.Ceiling = FMath::Min(Space.Level + RoomHeight + 1, SizeY);
		TArray<int32> Todo = { RoomIndex };
		Rooms[RoomIndex].Space = SpaceIndex;
		while (Todo.Num() > 0)
		{
			const int32 Current = Todo.Pop(EAllowShrinking::No);
			Space.Rooms.Add(Current);
			const FLBAPlanRoom& Room = Rooms[Current];
			for (int32 Side = 0; Side < 4; ++Side)
			{
				const bool bX = Sides[Side].X != 0;
				const int32 From = bX ? Room.Min.Y : Room.Min.X, To = bX ? Room.Max.Y : Room.Max.X;
				for (int32 A = From; A <= To; ++A)
				{
					const FIntPoint Column = bX ? FIntPoint(Side == 0 ? Room.Min.X - 1 : Room.Max.X + 1, A) : FIntPoint(A, Side == 2 ? Room.Min.Y - 1 : Room.Max.Y + 1);
					int32 Other = INDEX_NONE;
					if (GetSide(Current, Column, Side, &Other) == ELBARoomSide::OpenSide && Rooms[Other].Space == INDEX_NONE)
					{
						Rooms[Other].Space = SpaceIndex;
						Todo.Add(Other);
					}
				}
			}
		}
		Space.Rooms.Sort();
	}

	// R5 DOUBLE HEIGHT. The floor and furniture of each level's rooms and one-level connectors, and their space.
	TMap<FIntPoint, TArray<TPair<int32, int32>>> Upper;
	for (const FLBAPlanRoom& Room : Rooms)
	{
		const FLBAPlanLevel* Level = FindLevel(Room.Level);
		for (int32 X = Room.Min.X; X <= Room.Max.X; ++X)
		{
			for (int32 Z = Room.Min.Y; Z <= Room.Max.Y; ++Z)
			{
				const ELBAColumn Kind = Level->Get(FIntPoint(X, Z));
				if (Kind == ELBAColumn::Floor || Kind == ELBAColumn::Furniture)
				{
					Upper.FindOrAdd(FIntPoint(X, Z)).Emplace(Room.Level, Room.Space);
				}
			}
		}
	}
	for (const FLBAPlanConnector& Connector : Connectors)
	{
		if (Connector.Levels.Num() != 1)
		{
			continue;
		}
		for (const FIntVector& Column : Connector.Columns)
		{
			if (Column.Y != INDEX_NONE)
			{
				Upper.FindOrAdd(FIntPoint(Column.X, Column.Z)).Emplace(Column.Y, Rooms[Connector.Rooms[0]].Space);
			}
		}
	}
	for (FLBAPlanSpace& Space : Spaces)
	{
		const FLBAPlanLevel* Level = FindLevel(Space.Level);
		for (const int32 RoomIndex : Space.Rooms)
		{
			const FLBAPlanRoom& Room = Rooms[RoomIndex];
			for (int32 X = Room.Min.X; X <= Room.Max.X; ++X)
			{
				for (int32 Z = Room.Min.Y; Z <= Room.Max.Y; ++Z)
				{
					if (Level->Get(FIntPoint(X, Z)) != ELBAColumn::Floor)
					{
						continue;
					}
					for (const FIntPoint& Side : Sides)
					{
						for (const TPair<int32, int32>& Above : Upper.FindRef(FIntPoint(X, Z) + Side))
						{
							if (Above.Key <= Space.Level + Headroom || Space.OverlookedBy.Contains(Above.Value))
							{
								continue;
							}
							bool bOpen = true;
							for (int32 Y = Space.Level + 1; Y <= Above.Key + 1 && Y < SizeY && bOpen; ++Y)
							{
								bOpen = !IsSolid(X, Y, Z);
							}
							if (bOpen)
							{
								Space.OverlookedBy.Add(Above.Value);
							}
						}
					}
				}
			}
		}
	}
	for (FLBAPlanSpace& Space : Spaces)
	{
		for (const int32 Other : Space.OverlookedBy)
		{
			Space.Ceiling = FMath::Max(Space.Ceiling, FMath::Min(Spaces[Other].Level + RoomHeight + 1, SizeY));
		}
	}
	for (FLBAPlanRoom& Room : Rooms)
	{
		Room.Ceiling = Spaces[Room.Space].Ceiling;
	}
	for (FLBAPlanConnector& Connector : Connectors)
	{
		for (const int32 Room : Connector.Rooms)
		{
			Connector.Ceiling = FMath::Max(Connector.Ceiling, Rooms[Room].Ceiling);
		}
	}
}

bool FLBARoomPlan::IsInZone(const FIntPoint& Column, int32 Level) const
{
	return Zones.ContainsByPredicate([&](const FLBACellBox& Zone)
	{
		return Column.X >= Zone.Min.X && Column.X <= Zone.Max.X && Column.Y >= Zone.Min.Z && Column.Y <= Zone.Max.Z
			&& Level >= Zone.Min.Y - 1 && Level <= Zone.Max.Y + 1;
	});
}

const FLBAPlanLevel* FLBARoomPlan::FindLevel(int32 Height) const
{
	const int32 Index = LevelHeights.IndexOfByKey(Height);
	return Index != INDEX_NONE ? &Levels[Index] : nullptr;
}

int32 FLBARoomPlan::FindRoom(int32 Level, const FIntPoint& Column) const
{
	const int32* Room = RoomAt.Find(FIntVector(Column.X, Level, Column.Y));
	return Room ? *Room : INDEX_NONE;
}

int32 FLBARoomPlan::FindConnector(int32 Level, const FIntPoint& Column) const
{
	const int32* Connector = ConnectorAt.Find(FIntVector(Column.X, Level, Column.Y));
	return Connector ? *Connector : INDEX_NONE;
}

TConstArrayView<int32> FLBARoomPlan::GetFloorLevels(const FIntPoint& Column) const
{
	const TArray<int32, TInlineAllocator<2>>* Found = FloorLevels.Find(Column);
	return Found ? TConstArrayView<int32>(*Found) : TConstArrayView<int32>();
}

ELBARoomSide FLBARoomPlan::GetSide(int32 RoomIndex, const FIntPoint& Column, int32 Side, int32* OutOther) const
{
	const FLBAPlanRoom& Room = Rooms[RoomIndex];
	int32 Other = INDEX_NONE;
	ELBARoomSide Kind = ELBARoomSide::Open;
	if (!IsInGrid(Column))
	{
		Kind = ELBARoomSide::Open;
	}
	else if ((Other = FindRoom(Room.Level, Column)) != INDEX_NONE)
	{
		Kind = ELBARoomSide::OpenSide;
	}
	else if ((Other = FindConnector(Room.Level, Column)) != INDEX_NONE || (Other = FindConnector(INDEX_NONE, Column)) != INDEX_NONE)
	{
		Kind = ELBARoomSide::Connector;
	}
	else if (FindLevel(Room.Level)->Get(Column) == ELBAColumn::Wall)
	{
		Kind = ELBARoomSide::Wall;
	}
	else if (GetFloorLevels(Column).Num() > 0)
	{
		Kind = ELBARoomSide::Step;
		for (const int32 Level : GetFloorLevels(Column))
		{
			if ((Other = FindRoom(Level, Column)) != INDEX_NONE)
			{
				break;
			}
		}
	}
	if (OutOther)
	{
		*OutOther = Other;
	}
	return Kind;
}

bool FLBARoomPlan::IsSolid(int32 X, int32 Y, int32 Z) const
{
	return X >= 0 && Y >= 0 && Z >= 0 && X < SizeXZ && Y < SizeY && Z < SizeXZ && Solid[ToIndex(X, Y, Z)];
}

int32 FLBARoomPlan::GetBrick(int32 X, int32 Y, int32 Z) const
{
	return X >= 0 && Y >= 0 && Z >= 0 && X < SizeXZ && Y < SizeY && Z < SizeXZ ? Bricks[ToIndex(X, Y, Z)] : 0;
}

int32 FLBARoomPlan::GetWallRun(int32 X, int32 Y, int32 Z, int32* OutDrawnTop) const
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

void FLBARoomPlan::CountBricks(FLBAWallBrickCounts& Counts) const
{
	for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
	{
		const FLBAPlanRoom& Room = Rooms[RoomIndex];
		const FLBAPlanLevel* Level = FindLevel(Room.Level);
		for (int32 X = Room.Min.X; X <= Room.Max.X; ++X)
		{
			for (int32 Z = Room.Min.Y; Z <= Room.Max.Y; ++Z)
			{
				const FIntPoint Column(X, Z);
				const ELBAColumn Kind = Level->Get(Column);
				if (Kind == ELBAColumn::Floor)
				{
					if (const int32 Brick = GetBrick(X, Room.Level, Z))
					{
						++Counts.Floor.FindOrAdd(Brick);
					}
					continue;
				}
				// Free-standing furniture: floor all around.
				bool bFree = Kind == ELBAColumn::Furniture;
				for (const FIntPoint& Side : Sides)
				{
					bFree &= Level->Get(Column + Side) == ELBAColumn::Floor && IsInGrid(Column + Side);
				}
				for (int32 Y = Room.Level + 1; bFree && Y < FMath::Min(SizeY, Room.Level + 6); ++Y)
				{
					if (const int32 Brick = GetBrick(X, Y, Z))
					{
						++Counts.Furniture.FindOrAdd(Brick);
					}
				}
			}
		}
		// Back-most walls: nothing stands on the floor behind them.
		for (int32 Side = 0; Side < 4; ++Side)
		{
			const bool bX = Sides[Side].X != 0;
			const int32 From = bX ? Room.Min.Y : Room.Min.X, To = bX ? Room.Max.Y : Room.Max.X;
			for (int32 A = From; A <= To; ++A)
			{
				const FIntPoint Wall = bX ? FIntPoint(Side == 0 ? Room.Min.X - 1 : Room.Max.X + 1, A) : FIntPoint(A, Side == 2 ? Room.Min.Y - 1 : Room.Max.Y + 1);
				if (GetSide(RoomIndex, Wall, Side) != ELBARoomSide::Wall)
				{
					continue;
				}
				const FIntPoint Back = Wall + Sides[Side];
				bool bBacked = false;
				for (int32 Y = Room.Level + 1; Y <= Room.Level + 3 && IsInGrid(Back); ++Y)
				{
					bBacked |= IsSolid(Back.X, Y, Back.Y);
				}
				int32 DrawnTop = 0;
				const int32 Run = GetWallRun(Wall.X, Room.Level, Wall.Y, &DrawnTop);
				if (bBacked || DrawnTop == 0)
				{
					continue;
				}
				for (int32 Y = Room.Level + 1; Y <= Room.Level + Run; ++Y)
				{
					if (const int32 Brick = GetBrick(Wall.X, Y, Wall.Y))
					{
						++Counts.Wall.FindOrAdd(Brick);
					}
				}
			}
		}
	}
}

FString FLBARoomPlan::Describe() const
{
	static const TCHAR* RuleNames[] = { TEXT("none"), TEXT("cut"), TEXT("jamb"), TEXT("invisible"), TEXT("tall"), TEXT("masonry"),
		TEXT("exterior"), TEXT("pillar"), TEXT("post"), TEXT("against") };
	FString Result;
	for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
	{
		const FLBAPlanRoom& Room = Rooms[RoomIndex];
		Result += FString::Printf(TEXT("R%d space S%d level %d ceiling %d x%d-%d z%d-%d (%dx%d) floors %d furniture %d\n"), RoomIndex, Room.Space,
			Room.Level, Room.Ceiling, Room.Min.X, Room.Max.X, Room.Min.Y, Room.Max.Y, Room.Max.X - Room.Min.X + 1, Room.Max.Y - Room.Min.Y + 1,
			Room.Floors, Room.Furniture);
		for (int32 Side = 0; Side < 4; ++Side)
		{
			const bool bX = Sides[Side].X != 0;
			const int32 From = bX ? Room.Min.Y : Room.Min.X, To = bX ? Room.Max.Y : Room.Max.X;
			TArray<FString> Segments;
			FString Last;
			int32 Count = 0;
			for (int32 A = From; A <= To + 1; ++A)
			{
				FString Label;
				if (A <= To)
				{
					const FIntPoint Column = bX ? FIntPoint(Side == 0 ? Room.Min.X - 1 : Room.Max.X + 1, A) : FIntPoint(A, Side == 2 ? Room.Min.Y - 1 : Room.Max.Y + 1);
					int32 Other = INDEX_NONE;
					switch (GetSide(RoomIndex, Column, Side, &Other))
					{
					case ELBARoomSide::Wall: Label = FString::Printf(TEXT("wall/%s"), RuleNames[int32(FindLevel(Room.Level)->GetRule(Column))]); break;
					case ELBARoomSide::OpenSide: Label = FString::Printf(TEXT("open_side>R%d"), Other); break;
					case ELBARoomSide::Connector: Label = FString::Printf(TEXT("%s>C%d"), ConnectorName(Connectors[Other].Kind), Other); break;
					case ELBARoomSide::Step: Label = Other != INDEX_NONE ? FString::Printf(TEXT("step>R%d"), Other) : FString(TEXT("step")); break;
					default: Label = TEXT("open"); break;
					}
				}
				if (Count > 0 && Label != Last)
				{
					// Walls print without their rule, as the prototype groups them by it.
					Segments.Add(FString::Printf(TEXT("%sx%d"), Last.StartsWith(TEXT("wall/")) ? TEXT("wall") : *Last, Count));
					Count = 0;
				}
				Last = Label;
				++Count;
			}
			Result += FString::Printf(TEXT("   %s: %s\n"), SideNames[Side], *FString::Join(Segments, TEXT("  ")));
		}
	}
	for (int32 SpaceIndex = 0; SpaceIndex < Spaces.Num(); ++SpaceIndex)
	{
		const FLBAPlanSpace& Space = Spaces[SpaceIndex];
		TArray<FString> RoomNames, Over;
		for (const int32 Room : Space.Rooms)
		{
			RoomNames.Add(FString::Printf(TEXT("R%d"), Room));
		}
		for (const int32 Other : Space.OverlookedBy)
		{
			Over.Add(FString::Printf(TEXT("S%d"), Other));
		}
		Result += FString::Printf(TEXT("S%d level %d ceiling %d rooms %s%s\n"), SpaceIndex, Space.Level, Space.Ceiling, *FString::Join(RoomNames, TEXT(",")),
			Over.Num() > 0 ? *FString::Printf(TEXT(" overlooked by %s (double height)"), *FString::Join(Over, TEXT(","))) : TEXT(""));
	}
	for (int32 Index = 0; Index < Connectors.Num(); ++Index)
	{
		const FLBAPlanConnector& Connector = Connectors[Index];
		if (Connector.Kind == ELBAConnector::Pocket)
		{
			continue;
		}
		FIntPoint Min(SizeXZ, SizeXZ), Max(-1, -1);
		TArray<FString> At;
		for (const FIntVector& Column : Connector.Columns)
		{
			Min = Min.ComponentMin(FIntPoint(Column.X, Column.Z));
			Max = Max.ComponentMax(FIntPoint(Column.X, Column.Z));
			const FString Piece = Column.Y != INDEX_NONE ? FString::Printf(TEXT("L%d"), Column.Y) : FString(TEXT("flight"));
			if (At.Num() == 0 || At.Last() != Piece)
			{
				At.Add(Piece);
			}
		}
		TArray<FString> RoomNames;
		for (const int32 Room : Connector.Rooms)
		{
			RoomNames.Add(FString::Printf(TEXT("R%d"), Room));
		}
		const FString Width = Connector.Kind == ELBAConnector::Passage
			? FString::Printf(TEXT(" width %d (%s)"), Connector.Width, Connector.Width <= DoorWidth ? TEXT("door") : TEXT("arch")) : FString();
		Result += FString::Printf(TEXT("C%d %s %s x%d-%d z%d-%d cols %d rooms %s ceiling %d%s\n"), Index, ConnectorName(Connector.Kind),
			*FString::Join(At, TEXT("+")), Min.X, Max.X, Min.Y, Max.Y, Connector.Columns.Num(), *FString::Join(RoomNames, TEXT(",")), Connector.Ceiling, *Width);
	}
	return Result;
}
