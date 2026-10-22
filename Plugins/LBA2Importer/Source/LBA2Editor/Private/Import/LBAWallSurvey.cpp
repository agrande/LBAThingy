#include "Import/LBAWallSurvey.h"

#include "Formats/LBAIso.h"
#include "LBATypes.h"

TArray<FLBAWallRun> LBAWallSurvey::FindRuns(const FLBAGrid& Grid, const FLBALibrary& Library, TFunctionRef<float(int32, int32)> Coverage, float Opaque)
{
	constexpr int32 SizeXZ = LBA::GridSizeXZ, SizeY = LBA::GridSizeY;
	auto LayoutAt = [&Grid, &Library](int32 X, int32 Y, int32 Z) -> const FLBALayout*
	{
		const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
		return Library.Layouts.IsValidIndex(Cell.Layout) && Library.Layouts[Cell.Layout].Blocks.IsValidIndex(Cell.Block) ? &Library.Layouts[Cell.Layout] : nullptr;
	};
	auto IsSolid = [&Grid, &LayoutAt](int32 X, int32 Y, int32 Z)
	{
		if (!Grid.IsInside(X, Y, Z))
		{
			return false;
		}
		const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
		const FLBALayout* Layout = LayoutAt(X, Y, Z);
		// Cells without a layout can still carry an invisible collision shape.
		return Layout ? Layout->Blocks[Cell.Block].Shape != 0 : Cell.Layout < 0 && Cell.Block != 0;
	};

	TArray<FLBAWallRun> Runs;
	for (int32 Z = 0; Z < SizeXZ; ++Z)
	{
		for (int32 X = 0; X < SizeXZ; ++X)
		{
			for (const FIntPoint Side : { FIntPoint(1, 0), FIntPoint(0, 1) })
			{
				const int32 FX = X + Side.X, FZ = Z + Side.Y;
				const bool bFrontInside = FX < SizeXZ && FZ < SizeXZ;
				auto IsDrawn = [&](int32 Y)
				{
					const FLBALayout* Layout = LayoutAt(X, Y, Z);
					const int32 Brick = Layout ? int32(Layout->Blocks[Grid.Get(X, Y, Z).Block].Brick) : 0;
					return Brick != 0 && FMath::Max(Coverage(Brick, 1), Coverage(Brick, 2)) > 0.f && !IsSolid(FX, Y, FZ);
				};
				for (int32 Y = 0; Y < SizeY;)
				{
					if (!IsDrawn(Y))
					{
						++Y;
						continue;
					}
					FLBAWallRun Run;
					Run.Column = FIntPoint(X, Z);
					Run.Side = Side;
					Run.Foot = Y;
					bool bPanels = true;
					for (; Y < SizeY && IsDrawn(Y); ++Y)
					{
						const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
						const FLBALayout& Layout = Library.Layouts[Cell.Layout];
						// Wall panels are one cell deep toward the camera (furniture is not).
						bPanels &= (Side.X != 0 ? Layout.SizeX : Layout.SizeZ) == 1;
						Run.Cells.Add({ int32(Cell.Layout), Layout.GetBlockCoords(Cell.Block) });
						Run.OpaqueCells += Coverage(Layout.Blocks[Cell.Block].Brick, Side.X != 0 ? 1 : 2) >= Opaque;
					}
					if (!bPanels || Run.Cells.Num() < MinHeight)
					{
						continue;
					}
					bool bFloorInFront = false, bAnythingInFront = false;
					for (int32 Below = 0; Below < Run.Foot && bFrontInside; ++Below)
					{
						bAnythingInFront |= IsSolid(FX, Below, FZ);
					}
					bFloorInFront = bFrontInside && Run.Foot > 0 && IsSolid(FX, Run.Foot - 1, FZ);
					if (!bFloorInFront && bAnythingInFront)
					{
						// Something lower in front: a ledge, stairs, a lower floor. Neither side of a room.
						continue;
					}
					Run.Facing = bFloorInFront ? ELBAWallFacing::Indoor : ELBAWallFacing::Outdoor;
					Runs.Add(MoveTemp(Run));
				}
			}
		}
	}

	// Part of a wall line: MinLine columns in a row along the wall with a run of the same foot.
	TSet<FIntVector> Feet;
	auto FootKey = [](const FIntPoint& Column, const FIntPoint& Side, int32 Foot) { return FIntVector(Column.X * 2 + Side.Y, Foot, Column.Y); };
	for (const FLBAWallRun& Run : Runs)
	{
		Feet.Add(FootKey(Run.Column, Run.Side, Run.Foot));
	}
	return Runs.FilterByPredicate([&Feet, &FootKey](const FLBAWallRun& Run)
	{
		const FIntPoint Along(Run.Side.Y, Run.Side.X);
		int32 Line = 1;
		for (const int32 Step : { -1, 1 })
		{
			for (int32 K = 1; K < MinLine && Feet.Contains(FootKey(Run.Column + Along * (Step * K), Run.Side, Run.Foot)); ++K)
			{
				++Line;
			}
		}
		return Line >= MinLine;
	});
}

void FLBAWallStyleDetector::AddRuns(int32 GridIndex, TConstArrayView<FLBAWallRun> Runs)
{
	for (const FLBAWallRun& Run : Runs)
	{
		if (!Run.IsSolid())
		{
			continue;
		}
		TArray<FRow> Rows;
		for (const FLBAWallCell& Cell : Run.Cells)
		{
			Rows.Emplace(Cell.Layout, Cell.Block.Y);
		}
		// Body: the shortest block of rows stacked at least twice, with all its repeats.
		int32 BodyStart = INDEX_NONE, BodyLength = 0, BodyEnd = 0;
		for (int32 Length = 1; Length <= 4 && BodyStart == INDEX_NONE; ++Length)
		{
			for (int32 Start = 0; Start + Length * 2 <= Rows.Num() && BodyStart == INDEX_NONE; ++Start)
			{
				auto Same = [&Rows, Length](int32 A, int32 B)
				{
					for (int32 K = 0; K < Length; ++K)
					{
						if (Rows[A + K] != Rows[B + K])
						{
							return false;
						}
					}
					return true;
				};
				if (Same(Start, Start + Length))
				{
					BodyStart = Start;
					BodyLength = Length;
					BodyEnd = Start + Length * 2;
					while (BodyEnd + Length <= Rows.Num() && Same(Start, BodyEnd))
					{
						BodyEnd += Length;
					}
				}
			}
		}
		if (BodyStart == INDEX_NONE)
		{
			// Else the rows of the layout with the most of them (the first such).
			for (int32 Start = 0; Start < Rows.Num();)
			{
				int32 End = Start + 1;
				while (End < Rows.Num() && Rows[End].Key == Rows[Start].Key && Rows[End].Value == Rows[End - 1].Value + 1)
				{
					++End;
				}
				if (End - Start > BodyLength)
				{
					BodyStart = Start;
					BodyLength = End - Start;
					BodyEnd = End;
				}
				Start = End;
			}
		}
		const TArray<FRow> Foot(Rows.GetData(), BodyStart);
		const TArray<FRow> Body(Rows.GetData() + BodyStart, BodyLength);
		const TArray<FRow> Top(Rows.GetData() + BodyEnd, Rows.Num() - BodyEnd);

		FString Key = Run.Facing == ELBAWallFacing::Indoor ? TEXT("I") : TEXT("O");
		for (const TArray<FRow>* Part : { &Foot, &Body, &Top })
		{
			Key += TEXT(" |");
			for (const FRow& Row : *Part)
			{
				Key += FString::Printf(TEXT(" %d.%d"), Row.Key, Row.Value);
			}
		}
		FGroup& Group = Groups.FindOrAdd(Key);
		Group.Facing = Run.Facing;
		Group.Foot = Foot;
		Group.Body = Body;
		Group.Top = Top;
		++Group.Uses;
		Group.Grids.Add(GridIndex);
	}
}

TArray<FLBAWallStyle> FLBAWallStyleDetector::Finish(const TSet<int32>& Ignored) const
{
	// Rows in a row of one layout make a segment.
	auto Segments = [this](const TArray<FRow>& Rows)
	{
		TArray<FLBAWallSegment> Out;
		for (const FRow& Row : Rows)
		{
			if (Out.Num() > 0 && Out.Last().Layout == Row.Key && Out.Last().From + Out.Last().Height == Row.Value)
			{
				++Out.Last().Height;
				continue;
			}
			const FLBALayout& Layout = Library.Layouts[Row.Key];
			FLBAWallSegment& Segment = Out.AddDefaulted_GetRef();
			Segment.Layout = Row.Key;
			Segment.From = Row.Value;
			Segment.Height = 1;
			Segment.Width = FMath::Max(Layout.SizeX, Layout.SizeZ);
		}
		return Out;
	};
	// A style that is a crop of a taller one (rows of it in a row: a wall cut down, or started lower) counts as that
	// one.
	TArray<FGroup> Kept;
	for (const TPair<FString, FGroup>& Pair : Groups)
	{
		const FGroup& Group = Pair.Value;
		auto IsIgnored = [&Ignored](const FRow& Row) { return Ignored.Contains(Row.Key); };
		if (!Group.Foot.ContainsByPredicate(IsIgnored) && !Group.Body.ContainsByPredicate(IsIgnored) && !Group.Top.ContainsByPredicate(IsIgnored))
		{
			Kept.Add(Group);
		}
	}
	auto Flat = [](const FGroup& Group)
	{
		TArray<FRow> Rows = Group.Foot;
		Rows.Append(Group.Body);
		Rows.Append(Group.Top);
		return Rows;
	};
	Kept.Sort([&Flat](const FGroup& A, const FGroup& B) { return Flat(A).Num() > Flat(B).Num(); });
	for (int32 Index = Kept.Num() - 1; Index >= 0; --Index)
	{
		const TArray<FRow> Rows = Flat(Kept[Index]);
		for (int32 Other = 0; Other < Index; ++Other)
		{
			const TArray<FRow> Taller = Flat(Kept[Other]);
			if (Kept[Other].Facing != Kept[Index].Facing || Taller.Num() <= Rows.Num())
			{
				continue;
			}
			bool bCrop = false;
			for (int32 Start = 0; Start + Rows.Num() <= Taller.Num() && !bCrop; ++Start)
			{
				bCrop = FMemory::Memcmp(Taller.GetData() + Start, Rows.GetData(), Rows.Num() * sizeof(FRow)) == 0;
			}
			if (bCrop)
			{
				Kept[Other].Uses += Kept[Index].Uses;
				Kept[Other].Grids.Append(Kept[Index].Grids);
				Kept.RemoveAt(Index);
				break;
			}
		}
	}

	TArray<FLBAWallStyle> Styles;
	for (const FGroup& Group : Kept)
	{
		if (Group.Uses < MinUses)
		{
			continue;
		}
		FLBAWallStyle& Style = Styles.AddDefaulted_GetRef();
		Style.Facing = Group.Facing;
		Style.Uses = Group.Uses;
		Style.Grids = Group.Grids.Array();
		Style.Grids.Sort();
		Style.Foot = Segments(Group.Foot);
		Style.Body = Segments(Group.Body);
		Style.Top = Segments(Group.Top);
	}
	Styles.Sort([](const FLBAWallStyle& A, const FLBAWallStyle& B)
	{
		return A.Facing != B.Facing ? A.Facing < B.Facing : A.Uses > B.Uses;
	});
	return Styles;
}
