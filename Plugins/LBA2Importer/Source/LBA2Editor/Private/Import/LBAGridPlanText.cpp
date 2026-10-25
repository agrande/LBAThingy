#include "Import/LBAGridPlanText.h"

#include "Algo/Count.h"
#include "Interfaces/IPluginManager.h"
#include "LBAGridPlan.h"

namespace LBAGridPlanTextPrivate
{
	const TCHAR* SideNames[4] = { TEXT("-X"), TEXT("+X"), TEXT("-Z"), TEXT("+Z") };

	const TCHAR* GetConnectorName(ELBAConnector Kind)
	{
		static const TCHAR* Names[] = { TEXT("stairs"), TEXT("passage"), TEXT("exit"), TEXT("alcove"), TEXT("pocket") };
		return Names[int32(Kind)];
	}

	TCHAR GetConnectorSymbol(ELBAConnector Kind)
	{
		static const TCHAR Symbols[] = { TEXT('/'), TEXT('='), TEXT('>'), TEXT('~'), TEXT(',') };
		return Symbols[int32(Kind)];
	}

	TCHAR GetRoomLetter(int32 Room)
	{
		return Room < 26 ? TCHAR(TEXT('A') + Room) : Room < 52 ? TCHAR(TEXT('a') + Room - 26) : TEXT('*');
	}

	/** A small count as one character: 0-9, a-z, then +. */
	TCHAR GetDigit(int32 Value)
	{
		return Value < 0 ? TEXT('-') : Value < 10 ? TCHAR(TEXT('0') + Value) : Value < 36 ? TCHAR(TEXT('a') + Value - 10) : TEXT('+');
	}

	TCHAR GetRuleLetter(ELBAWallRule Rule)
	{
		switch (Rule)
		{
		case ELBAWallRule::Cut: return TEXT('c');
		case ELBAWallRule::Jamb: return TEXT('j');
		case ELBAWallRule::Invisible: return TEXT('i');
		case ELBAWallRule::Tall: return TEXT('t');
		case ELBAWallRule::Masonry: return TEXT('m');
		case ELBAWallRule::Exterior: return TEXT('e');
		case ELBAWallRule::Pillar: return TEXT('p');
		case ELBAWallRule::Post: return TEXT('o');
		case ELBAWallRule::Against: return TEXT('a');
		default: return TEXT('#');
		}
	}

	FString Range(const TCHAR* Axis, int32 Min, int32 Max)
	{
		return FString::Printf(TEXT("%s%d-%d"), Axis, Min, Max);
	}

	/** A top-down map of the box of columns (X across, Z down), one character per column, with X numbered on top. */
	FString DrawMap(const FString& Title, const FIntPoint& Min, const FIntPoint& Max, TFunctionRef<TCHAR(const FIntPoint&)> At)
	{
		FString Result = Title + TEXT("\n");
		FString Tens = TEXT("    "), Units = TEXT("    ");
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			Tens.AppendChar(X % 10 == 0 || X == Min.X ? TCHAR(TEXT('0') + X / 10) : TEXT(' '));
			Units.AppendChar(TCHAR(TEXT('0') + X % 10));
		}
		Result += Tens.TrimEnd() + TEXT("\n") + Units + TEXT("\n");
		for (int32 Z = Min.Y; Z <= Max.Y; ++Z)
		{
			FString Row = FString::Printf(TEXT("z%02d "), Z);
			for (int32 X = Min.X; X <= Max.X; ++X)
			{
				Row.AppendChar(At(FIntPoint(X, Z)));
			}
			Result += Row.TrimEnd() + TEXT("\n");
		}
		return Result;
	}

	void WriteRooms(const FLBAGridPlan& Plan, FString& Result)
	{
		for (int32 RoomIndex = 0; RoomIndex < Plan.Rooms.Num(); ++RoomIndex)
		{
			const FLBAGridPlanRoom& Room = Plan.Rooms[RoomIndex];
			Result += FString::Printf(TEXT("R%d %c space S%d level %d ceiling %d x%d-%d z%d-%d (%dx%d) floors %d furniture %d\n"), RoomIndex,
				GetRoomLetter(RoomIndex), Room.Space, Room.Level, Room.Ceiling, Room.Min.X, Room.Max.X, Room.Min.Y, Room.Max.Y,
				Room.Max.X - Room.Min.X + 1, Room.Max.Y - Room.Min.Y + 1, Room.Floors, Room.Furniture);
			for (int32 Side = 0; Side < 4; ++Side)
			{
				TArray<FString> Runs;
				for (const FLBAGridPlanSideRun& Run : Room.GetSide(Side).Runs)
				{
					FString Label;
					switch (Run.Kind)
					{
					case ELBARoomSide::Wall: Label = TEXT("wall"); break;
					case ELBARoomSide::OpenSide: Label = FString::Printf(TEXT("open_side>R%d"), Run.Other); break;
					case ELBARoomSide::Connector:
						Label = FString::Printf(TEXT("%s>C%d"), Plan.Connectors.IsValidIndex(Run.Other) ? GetConnectorName(Plan.Connectors[Run.Other].Kind) : TEXT("?"), Run.Other);
						break;
					case ELBARoomSide::Step: Label = Run.Other != INDEX_NONE ? FString::Printf(TEXT("step>R%d"), Run.Other) : FString(TEXT("step")); break;
					default: Label = TEXT("open"); break;
					}
					Runs.Add(FString::Printf(TEXT("%sx%d"), *Label, Run.Count));
				}
				Result += FString::Printf(TEXT("   %s: %s\n"), SideNames[Side], *FString::Join(Runs, TEXT("  ")));
			}
		}
		for (int32 SpaceIndex = 0; SpaceIndex < Plan.Spaces.Num(); ++SpaceIndex)
		{
			const FLBAGridPlanSpace& Space = Plan.Spaces[SpaceIndex];
			TArray<FString> Rooms, Over;
			for (const int32 Room : Space.Rooms)
			{
				Rooms.Add(FString::Printf(TEXT("R%d"), Room));
			}
			for (const int32 Other : Space.OverlookedBy)
			{
				Over.Add(FString::Printf(TEXT("S%d"), Other));
			}
			Result += FString::Printf(TEXT("S%d level %d ceiling %d rooms %s%s\n"), SpaceIndex, Space.Level, Space.Ceiling, *FString::Join(Rooms, TEXT(",")),
				Over.Num() > 0 ? *FString::Printf(TEXT(" overlooked by %s (double height)"), *FString::Join(Over, TEXT(","))) : TEXT(""));
		}
		for (int32 Index = 0; Index < Plan.Connectors.Num(); ++Index)
		{
			const FLBAGridPlanConnector& Connector = Plan.Connectors[Index];
			if (Connector.Kind == ELBAConnector::Pocket)
			{
				continue;
			}
			FIntPoint Min(FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeXZ), Max(-1, -1);
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
			TArray<FString> Rooms;
			for (const int32 Room : Connector.Rooms)
			{
				Rooms.Add(FString::Printf(TEXT("R%d"), Room));
			}
			const FString Width = Connector.Kind == ELBAConnector::Passage
				? FString::Printf(TEXT(" width %d (%s)"), Connector.Width, Connector.Width <= 2 ? TEXT("door") : TEXT("arch")) : FString();
			Result += FString::Printf(TEXT("C%d %s %s x%d-%d z%d-%d cols %d rooms %s ceiling %d%s\n"), Index, GetConnectorName(Connector.Kind),
				*FString::Join(At, TEXT("+")), Min.X, Max.X, Min.Y, Max.Y, Connector.Columns.Num(), *FString::Join(Rooms, TEXT(",")), Connector.Ceiling, *Width);
		}
		const int32 Pockets = Algo::CountIf(Plan.Connectors, [](const FLBAGridPlanConnector& Connector) { return Connector.Kind == ELBAConnector::Pocket; });
		if (Pockets > 0)
		{
			Result += FString::Printf(TEXT("%d pockets (columns out of reach)\n"), Pockets);
		}
		for (int32 Index = 0; Index < Plan.Flights.Num(); ++Index)
		{
			const FLBAGridPlanFlight& Flight = Plan.Flights[Index];
			FIntVector Min(TNumericLimits<int32>::Max()), Max(TNumericLimits<int32>::Lowest());
			for (const FIntVector& Cell : Flight.Cells)
			{
				Min = Min.ComponentMin(Cell);
				Max = Max.ComponentMax(Cell);
			}
			TArray<FString> Levels;
			for (const int32 Level : Flight.Levels)
			{
				Levels.Add(FString::FromInt(Level));
			}
			Result += FString::Printf(TEXT("F%d flight levels %s %s %s steps %d-%d cells %d\n"), Index, *FString::Join(Levels, TEXT(",")),
				*Range(TEXT("x"), Min.X, Max.X), *Range(TEXT("z"), Min.Z, Max.Z), Min.Y, Max.Y, Flight.Cells.Num());
		}
	}

	void WriteLevelMaps(const FLBAGridPlan& Plan, FString& Result)
	{
		// Who holds each column at each level: (X, level, Z) -> connector; flights' columns (level -1).
		TMap<FIntVector, int32> ConnectorAt;
		for (int32 Index = 0; Index < Plan.Connectors.Num(); ++Index)
		{
			for (const FIntVector& Column : Plan.Connectors[Index].Columns)
			{
				ConnectorAt.Add(Column, Index);
			}
		}
		TSet<FIntPoint> BeyondExits;
		BeyondExits.Append(Plan.BeyondExits);
		for (const FLBAGridPlanLevel& Level : Plan.Levels)
		{
			TMap<FIntPoint, const FLBAGridPlanAir*> AirAt;
			for (const FLBAGridPlanAir& At : Plan.Air)
			{
				if (At.Level == Level.Height)
				{
					AirAt.Add(At.Column, &At);
				}
			}
			auto RoomAt = [&Plan, &Level](const FIntPoint& Column) { return Plan.FindRoom(Level.Height, Column); };
			auto ConnectorOf = [&ConnectorAt, &Plan, &Level](const FIntPoint& Column)
			{
				if (const int32* Found = ConnectorAt.Find(FIntVector(Column.X, Level.Height, Column.Y)))
				{
					return *Found;
				}
				const int32* Flight = ConnectorAt.Find(FIntVector(Column.X, INDEX_NONE, Column.Y));
				return Flight && Plan.Connectors[*Flight].Levels.Contains(Level.Height) ? *Flight : INDEX_NONE;
			};

			// The box of what this level holds.
			FIntPoint Min(FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeXZ), Max(-1, -1);
			for (int32 Z = 0; Z < FLBAGridPlan::SizeXZ; ++Z)
			{
				for (int32 X = 0; X < FLBAGridPlan::SizeXZ; ++X)
				{
					const FIntPoint Column(X, Z);
					if (Level.GetColumn(Column) != ELBAColumn::Other || AirAt.Contains(Column) || RoomAt(Column) != INDEX_NONE || ConnectorOf(Column) != INDEX_NONE)
					{
						Min = Min.ComponentMin(Column);
						Max = Max.ComponentMax(Column);
					}
				}
			}
			if (Max.X < 0)
			{
				continue;
			}
			TSet<FIntPoint> Gaps, Lines;
			Gaps.Append(Level.Gaps);
			Lines.Append(Level.PartitionLines);
			Result += TEXT("\n");
			Result += DrawMap(FString::Printf(TEXT("Level %d rooms"), Level.Height), Min, Max, [&](const FIntPoint& Column)
			{
				const int32 Room = RoomAt(Column);
				if (Room != INDEX_NONE)
				{
					return GetRoomLetter(Room);
				}
				const int32 Connector = ConnectorOf(Column);
				if (Connector != INDEX_NONE)
				{
					return GetConnectorSymbol(Plan.Connectors[Connector].Kind);
				}
				return Level.GetColumn(Column) == ELBAColumn::Wall ? TEXT('#') : TEXT(' ');
			});
			Result += DrawMap(FString::Printf(TEXT("Level %d columns"), Level.Height), Min, Max, [&](const FIntPoint& Column)
			{
				const ELBAWallRule Rule = Level.GetRule(Column);
				switch (Level.GetColumn(Column))
				{
				case ELBAColumn::Floor: return Gaps.Contains(Column) ? TEXT('g') : Lines.Contains(Column) ? TEXT('+') : TEXT('.');
				case ELBAColumn::Stairs: return TEXT('/');
				case ELBAColumn::Furniture: return Rule == ELBAWallRule::None ? TEXT('f') : GetRuleLetter(Rule);
				case ELBAColumn::Wall: return Rule == ELBAWallRule::None ? TEXT('#') : TCHAR(FChar::ToUpper(GetRuleLetter(Rule)));
				default: return TEXT(' ');
				}
			});
			Result += DrawMap(FString::Printf(TEXT("Level %d air"), Level.Height), Min, Max, [&](const FIntPoint& Column)
			{
				if (const FLBAGridPlanAir* const* At = AirAt.Find(Column))
				{
					return GetDigit((*At)->Top - (*At)->Floor - 1);
				}
				const int32 Connector = ConnectorOf(Column);
				return BeyondExits.Contains(Column) && Connector != INDEX_NONE && Plan.Connectors[Connector].Kind == ELBAConnector::Exit ? TEXT('x') : TEXT(' ');
			});
		}
	}

	void WriteHollowMaps(const FLBAGridPlan& Plan, FString& Result)
	{
		TMap<FIntPoint, const FLBAGridPlanHollowColumn*> At;
		FIntPoint Min(FLBAGridPlan::SizeXZ, FLBAGridPlan::SizeXZ), Max(-1, -1);
		for (const FLBAGridPlanHollowColumn& Column : Plan.Hollow.Columns)
		{
			At.Add(Column.Column, &Column);
			Min = Min.ComponentMin(Column.Column);
			Max = Max.ComponentMax(Column.Column);
		}
		if (Max.X < 0)
		{
			return;
		}
		Result += TEXT("\n");
		Result += DrawMap(TEXT("Hollow floor"), Min, Max, [&At](const FIntPoint& Column)
		{
			const FLBAGridPlanHollowColumn* const* Found = At.Find(Column);
			return !Found ? TEXT(' ') : (*Found)->bSupport ? TEXT('S') : (*Found)->Floor == INDEX_NONE ? TEXT('_') : GetDigit((*Found)->Floor);
		});
		Result += DrawMap(TEXT("Hollow roof"), Min, Max, [&At](const FIntPoint& Column)
		{
			const FLBAGridPlanHollowColumn* const* Found = At.Find(Column);
			return !Found ? TEXT(' ') : (*Found)->bSupport ? TEXT('S') : GetDigit(FMath::FloorToInt((*Found)->Roof));
		});
	}
}

FString LBAGridPlanText::Write(const FLBAGridPlan& Plan)
{
	using namespace LBAGridPlanTextPrivate;
	const bool bHollow = Plan.Structure == ELBAEnclosureStructure::Hollow;
	FString Result = FString::Printf(TEXT("Grid %d %s, library %d: %s, %s, room height %d, door height %d\n"), Plan.Grid, *Plan.Name, Plan.Library,
		bHollow ? TEXT("hollow") : TEXT("rooms"), Plan.bEnclosed ? TEXT("enclosed") : TEXT("not enclosed"), Plan.RoomHeight, Plan.DoorHeight);
	if (bHollow)
	{
		Result += Plan.Hollow.Summary + TEXT("\n");
		Result += TEXT("Maps (X across, Z down): floor = its height (0-9 a-o), _ none (spanned), S support; roof = its height, rounded down\n");
		WriteHollowMaps(Plan, Result);
		return Result;
	}
	TArray<FString> Heights;
	for (const FLBAGridPlanLevel& Level : Plan.Levels)
	{
		Heights.Add(FString::FromInt(Level.Height));
	}
	Result += FString::Printf(TEXT("Levels %s\n"), *FString::Join(Heights, TEXT(" ")));
	Result += TEXT("Maps per level (X across, Z down):\n");
	Result += TEXT("  rooms: A-Z a-z = R0-R51, / stairs, = passage, > exit, ~ alcove, , pocket, # wall\n");
	Result += TEXT("  columns: . floor, g gap in a wall line, + partition line, / stairs, f furniture; walls by rule: C cut, J jamb, I invisible,\n");
	Result += TEXT("    T tall, M masonry, E exterior, # other; furniture kept from walls: p pillar, o post, a against\n");
	Result += TEXT("  air: cells of air over the floor (0-9 a-z), x past an exit\n\n");
	WriteRooms(Plan, Result);
	WriteLevelMaps(Plan, Result);
	return Result;
}

FString LBAGridPlanText::GetFolder()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LBA2Importer"));
	return Plugin ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Tests/Plans")) : FString();
}

FString LBAGridPlanText::GetPath(int32 Grid)
{
	return GetFolder() / FString::Printf(TEXT("grid%03d.txt"), Grid);
}
