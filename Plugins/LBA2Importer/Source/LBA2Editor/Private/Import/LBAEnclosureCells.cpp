#include "Import/LBAEnclosureCells.h"

#include "Import/LBARoomAnalysis.h"
#include "LBAEnclosure.h"

namespace
{
	const FIntPoint Directions[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };

	/** Highest solid cell of a column below Below, or INDEX_NONE. */
	int32 TopBelow(const FLBARoomAnalysis& Rooms, const FIntPoint& Column, int32 Below)
	{
		for (int32 Y = FMath::Min(Below, FLBARoomAnalysis::SizeY) - 1; Y >= 0; --Y)
		{
			if (Rooms.IsSolid(Column.X, Y, Column.Y))
			{
				return Y;
			}
		}
		return INDEX_NONE;
	}

	/** Highest drawn cell of a column below Below, or INDEX_NONE. */
	int32 TopDrawnBelow(TFunctionRef<bool(int32, int32, int32)> IsDrawn, const FIntPoint& Column, int32 Below)
	{
		for (int32 Y = FMath::Min(Below, FLBARoomAnalysis::SizeY) - 1; Y >= 0; --Y)
		{
			if (IsDrawn(Column.X, Y, Column.Y))
			{
				return Y;
			}
		}
		return INDEX_NONE;
	}

	/** The columns one space's room covers. */
	struct FRegion
	{
		/** Floor height each column of the room is measured from (its own floor, or the nearest one). */
		TMap<FIntPoint, int32> Floor;
		/** Ceiling cell of each column (SizeY: no ceiling, the room reaches the top of the grid). */
		TMap<FIntPoint, int32> Ceiling;
		/** Columns between the room and the outside, raised to its ceiling. */
		TSet<FIntPoint> Perimeter;
	};
}

FLBAEnclosureCells FLBAEnclosureCells::Build(const FLBARoomAnalysis& Rooms, TConstArrayView<FLBADoorway> Doorways, const FLBAEnclosureSettings& Settings,
	TFunctionRef<bool(int32, int32, int32)> IsDrawn)
{
	constexpr int32 SizeXZ = FLBARoomAnalysis::SizeXZ;
	constexpr int32 SizeY = FLBARoomAnalysis::SizeY;
	FLBAEnclosureCells Result;
	if (!Settings.bEnclose)
	{
		return Result;
	}

	const TArray<TArray<FIntVector>>& Spaces = Rooms.GetSpaces();

	// Exits stay open through the walls in line with them, up to the next floor (the invisible plug past the
	// doorway, a thick wall): those walls only rise from the top of the doorway (its lintel, or DoorHeight above it).
	TSet<FIntPoint> AllFloorColumns;
	for (const TArray<FIntVector>& Space : Spaces)
	{
		for (const FIntVector& Cell : Space)
		{
			AllFloorColumns.Add(FIntPoint(Cell.X, Cell.Z));
		}
	}
	TMap<FIntPoint, int32> PassageTops;
	for (const FLBADoorway& Doorway : Doorways)
	{
		if (!Doorway.bExit)
		{
			continue;
		}
		const FIntPoint Along = Doorway.Axis == 0 ? FIntPoint(1, 0) : FIntPoint(0, 1);
		for (const FIntVector& Cell : Doorway.Cells)
		{
			const int32 Top = Cell.Y + (Doorway.ClearHeight != INDEX_NONE ? Doorway.ClearHeight : Settings.DoorHeight) + 1;
			for (const int32 Sign : { -1, 1 })
			{
				for (int32 Step = 1; Step <= MaxWallThickness + 1; ++Step)
				{
					const FIntPoint Column = FIntPoint(Cell.X, Cell.Z) + Along * (Step * Sign);
					if (AllFloorColumns.Contains(Column))
					{
						break;
					}
					int32& PassageTop = PassageTops.FindOrAdd(Column, 0);
					PassageTop = FMath::Max(PassageTop, Top);
				}
			}
		}
	}

	TArray<FRegion> Regions;
	Regions.SetNum(Spaces.Num());
	TMap<FIntVector, int32> FloorSpaces;
	TMap<FIntVector, int32> Air;
	// Filled cells, the space that asked for them and what they make.
	TMap<FIntVector, TPair<int32, ELBAEnclosurePart>> Fill;
	// Cells that are not drawn: empty, invisible collision (it walls the room in, but the original never drew it),
	// bricks without art, and the frames the original put on the walls it cut down.
	auto AddFill = [&IsDrawn, &Fill](int32 X, int32 Y, int32 Z, int32 Space, ELBAEnclosurePart Part)
	{
		if (Y >= 0 && Y < SizeY && !IsDrawn(X, Y, Z))
		{
			// A partition belongs to the rooms on both sides: it keeps the first room's part.
			Fill.FindOrAdd(FIntVector(X, Y, Z), TPair<int32, ELBAEnclosurePart>(Space, Part));
		}
	};
	// Columns raised to the ceiling: perimeters, cut-down partitions, and the walls the camera-side ones stand for.
	TSet<FIntPoint> Raised;

	for (int32 SpaceIndex = 0; SpaceIndex < Spaces.Num(); ++SpaceIndex)
	{
		FRegion& Region = Regions[SpaceIndex];
		TMap<FIntPoint, int32> FloorColumns;
		for (const FIntVector& Cell : Spaces[SpaceIndex])
		{
			FloorSpaces.Add(Cell, SpaceIndex);
			int32& Y = FloorColumns.FindOrAdd(FIntPoint(Cell.X, Cell.Z), Cell.Y);
			Y = FMath::Max(Y, Cell.Y);
		}

		// Wall tops joined by one-cell steps make thin "spaces" nobody walks: a room has floor all around some cells.
		int32 Inner = 0;
		for (const FIntVector& Cell : Spaces[SpaceIndex])
		{
			bool bInner = true;
			for (const FIntPoint& Dir : Directions)
			{
				bInner &= FloorSpaces.FindRef(FIntVector(Cell.X + Dir.X, Cell.Y, Cell.Z + Dir.Y), INDEX_NONE) == SpaceIndex;
			}
			Inner += bInner;
		}
		if (Inner < MinInnerFloors)
		{
			continue;
		}

		// The room's footprint: the outside floods in from beyond the grid through columns with nothing at or above
		// the room's lowest floor (voids, lower ground, the grid's edge); what it does not reach is the room's:
		// its floors, the furniture, partitions and walls between them, all under its ceiling.
		int32 Lowest = SizeY;
		for (const TPair<FIntPoint, int32>& Pair : FloorColumns)
		{
			Lowest = FMath::Min(Lowest, Pair.Value);
		}
		auto IsInGrid = [](const FIntPoint& Column)
		{
			return Column.X >= 0 && Column.Y >= 0 && Column.X < SizeXZ && Column.Y < SizeXZ;
		};
		auto IsOpen = [&Rooms, &IsInGrid, Lowest](const FIntPoint& Column)
		{
			if (!IsInGrid(Column))
			{
				return true;
			}
			for (int32 Y = Lowest; Y < SizeY; ++Y)
			{
				if (Rooms.IsSolid(Column.X, Y, Column.Y))
				{
					return false;
				}
			}
			return true;
		};
		TSet<FIntPoint> Outside;
		{
			TArray<FIntPoint> Stack;
			for (int32 I = -1; I <= SizeXZ; ++I)
			{
				Stack.Append({ FIntPoint(I, -1), FIntPoint(I, SizeXZ), FIntPoint(-1, I), FIntPoint(SizeXZ, I) });
			}
			while (Stack.Num() > 0)
			{
				const FIntPoint Column = Stack.Pop(EAllowShrinking::No);
				if (Column.X < -1 || Column.Y < -1 || Column.X > SizeXZ || Column.Y > SizeXZ || Outside.Contains(Column)
					|| FloorColumns.Contains(Column) || !IsOpen(Column))
				{
					continue;
				}
				Outside.Add(Column);
				for (const FIntPoint& Dir : Directions)
				{
					Stack.Add(Column + Dir);
				}
			}
		}

		// Each column of the footprint is measured from its nearest floor.
		TArray<FIntPoint> Todo;
		for (const TPair<FIntPoint, int32>& Pair : FloorColumns)
		{
			Region.Floor.Add(Pair.Key, Pair.Value);
			Todo.Add(Pair.Key);
		}
		for (int32 Head = 0; Head < Todo.Num(); ++Head)
		{
			const FIntPoint Column = Todo[Head];
			for (const FIntPoint& Dir : Directions)
			{
				const FIntPoint Next = Column + Dir;
				if (IsInGrid(Next) && !Outside.Contains(Next) && !Region.Floor.Contains(Next))
				{
					Region.Floor.Add(Next, Region.Floor[Column]);
					Todo.Add(Next);
				}
			}
		}

		// The perimeter: the footprint next to the outside, but its floors (open edges stay open). Its columns are
		// raised to the ceiling: cut-down walls and their invisible collision, the trims the camera-side walls end in.
		for (const TPair<FIntPoint, int32>& Pair : Region.Floor)
		{
			if (FloorColumns.Contains(Pair.Key))
			{
				continue;
			}
			for (const FIntPoint& Dir : Directions)
			{
				if (Outside.Contains(Pair.Key + Dir))
				{
					Region.Perimeter.Add(Pair.Key);
					break;
				}
			}
		}
		for (const FIntPoint& Wall : Region.Perimeter)
		{
			Region.Floor.Remove(Wall);
		}

		// A column's ceiling is the highest one any floor of the room nearby needs: a lower part open to a higher
		// floor (split levels, balconies over a hall) shares its ceiling instead of showing the back of its own.
		for (const TPair<FIntPoint, int32>& Pair : Region.Floor)
		{
			int32 Floor = Pair.Value;
			for (int32 DX = -CeilingReach; DX <= CeilingReach; ++DX)
			{
				for (int32 DZ = -CeilingReach; DZ <= CeilingReach; ++DZ)
				{
					if (const int32* Near = Region.Floor.Find(Pair.Key + FIntPoint(DX, DZ)))
					{
						Floor = FMath::Max(Floor, *Near);
					}
				}
			}
			const int32 Ceiling = FMath::Min(Floor + Settings.RoomHeight + 1, SizeY);
			Region.Ceiling.Add(Pair.Key, Ceiling);
			for (int32 Y = Pair.Value + 1; Y < Ceiling; ++Y)
			{
				// Invisible collision is air too: the room is seen through it (the original keeps it on top of cut-down walls).
				if (!IsDrawn(Pair.Key.X, Y, Pair.Key.Y))
				{
					Air.FindOrAdd(FIntVector(Pair.Key.X, Y, Pair.Key.Y), SpaceIndex);
				}
			}
		}

		// Perimeter walls rise to the ceiling of the room next to them and close their gaps (the cut-down walls
		// facing the camera often end in an overhanging trim); their invisible parts get drawn, and their visible
		// cells get a backing where the grid has no back face for them.
		for (const FIntPoint& Wall : Region.Perimeter)
		{
			int32 Ceiling = 0;
			int32 Floor = SizeY;
			for (const FIntPoint& Dir : Directions)
			{
				if (const int32* Next = Region.Ceiling.Find(Wall + Dir))
				{
					Ceiling = FMath::Max(Ceiling, *Next);
					Floor = FMath::Min(Floor, Region.Floor[Wall + Dir]);
				}
			}
			const int32 PassageTop = PassageTops.FindRef(Wall);
			Raised.Add(Wall);
			for (int32 Y = Floor + 1; Y < Ceiling; ++Y)
			{
				if (IsDrawn(Wall.X, Y, Wall.Y))
				{
					Result.Backing.Add(FIntVector(Wall.X, Y, Wall.Y));
					Result.Parts.Add(FIntVector(Wall.X, Y, Wall.Y), ELBAEnclosurePart::Backing);
				}
				else if (Y >= PassageTop)
				{
					AddFill(Wall.X, Y, Wall.Y, SpaceIndex, ELBAEnclosurePart::Wall);
				}
			}
		}

		// Walls inside the footprint rise to the ceiling from the top of what is drawn of them when the original cut
		// them down: their collision goes on above it (invisible, or bricks without art), unlike railings and furniture.
		// So do the walls standing in front of a perimeter that draws nothing there (the camera-side walls: a stub
		// with a frame on top, and an undrawn band one column further out), so the raised wall is flush with them.
		auto RaiseFromTop = [&](const FIntPoint& Column, int32 Floor, ELBAEnclosurePart Part)
		{
			const int32* Ceiling = Region.Ceiling.Find(Column);
			if (!Ceiling)
			{
				return;
			}
			const int32 Top = TopDrawnBelow(IsDrawn, Column, *Ceiling);
			Raised.Add(Column);
			for (int32 Y = FMath::Max(FMath::Max(Top, Floor) + 1, PassageTops.FindRef(Column)); Y < *Ceiling; ++Y)
			{
				AddFill(Column.X, Y, Column.Y, SpaceIndex, Part);
			}
		};
		for (const TPair<FIntPoint, int32>& Pair : Region.Floor)
		{
			const FIntPoint Column = Pair.Key;
			if (FloorColumns.Contains(Column) || Rooms.GetWallHeight(Column.X, Pair.Value, Column.Y) < FLBARoomAnalysis::WallHeight)
			{
				continue;
			}
			const int32 Top = TopDrawnBelow(IsDrawn, Column, SizeY);
			const int32 SolidTop = TopBelow(Rooms, Column, SizeY);
			bool bRaise = SolidTop - Top >= MinCutCollision;
			for (int32 D = 0; D < 4 && !bRaise; ++D)
			{
				const FIntPoint Outer = Column + Directions[D];
				if (Region.Perimeter.Contains(Outer))
				{
					const int32* Ceiling = Region.Ceiling.Find(Column);
					bRaise = Ceiling && TopDrawnBelow(IsDrawn, Outer, *Ceiling) <= Pair.Value;
				}
			}
			if (bRaise)
			{
				RaiseFromTop(Column, Pair.Value, ELBAEnclosurePart::Partition);
			}
		}

		// The ceiling, with risers where it steps up to a neighbor's (stairs between floors).
		if (Settings.bCeiling)
		{
			for (const TPair<FIntPoint, int32>& Pair : Region.Ceiling)
			{
				int32 Top = Pair.Value + 1;
				for (const FIntPoint& Dir : Directions)
				{
					if (const int32* Next = Region.Ceiling.Find(Pair.Key + Dir))
					{
						Top = FMath::Max(Top, *Next);
					}
				}
				for (int32 Y = Pair.Value; Y < Top; ++Y)
				{
					AddFill(Pair.Key.X, Y, Pair.Key.Y, SpaceIndex, Y == Pair.Value ? ELBAEnclosurePart::Ceiling : ELBAEnclosurePart::Riser);
				}
			}
		}
	}

	// Above doorways whose sides reach the ceiling (raised walls, or walls drawn that high), the wall closes from
	// the doorway's top: its lintel, or DoorHeight above the floor where the original cut the lintel away with the
	// walls facing the camera (or left a frame there).
	for (const FLBADoorway& Doorway : Doorways)
	{
		for (const FIntVector& Cell : Doorway.Cells)
		{
			const int32* Space = FloorSpaces.Find(Cell);
			const FIntPoint Column(Cell.X, Cell.Z);
			const int32* Ceiling = Space ? Regions[*Space].Ceiling.Find(Column) : nullptr;
			if (!Ceiling)
			{
				continue;
			}
			bool bClosed = true;
			for (const FIntPoint& Flank : Doorway.Flanks)
			{
				bClosed &= Raised.Contains(Flank) || TopDrawnBelow(IsDrawn, Flank, SizeY) >= *Ceiling - 1;
			}
			if (!bClosed)
			{
				continue;
			}
			const int32 Top = TopDrawnBelow(IsDrawn, Column, *Ceiling);
			const int32 From = Top > Cell.Y ? Top + 1 : Cell.Y + Settings.DoorHeight + 1;
			for (int32 Y = From; Y < *Ceiling; ++Y)
			{
				AddFill(Cell.X, Y, Cell.Z, *Space, ELBAEnclosurePart::Doorway);
			}
		}
	}

	// A room never fills another one's air, but partitions are both rooms' walls.
	for (const TPair<FIntVector, TPair<int32, ELBAEnclosurePart>>& Pair : Fill)
	{
		const int32* AirSpace = Air.Find(Pair.Key);
		if (!AirSpace || *AirSpace == Pair.Value.Key || Pair.Value.Value == ELBAEnclosurePart::Partition)
		{
			Result.Fill.Add(Pair.Key);
			Result.Parts.Add(Pair.Key, Pair.Value.Value);
		}
	}
	for (const TPair<FIntVector, int32>& Pair : Air)
	{
		if (!Result.Fill.Contains(Pair.Key))
		{
			Result.Air.Add(Pair.Key, Pair.Value);
		}
	}
	return Result;
}
