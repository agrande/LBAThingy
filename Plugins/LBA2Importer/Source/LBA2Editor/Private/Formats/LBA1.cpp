#include "Formats/LBA1.h"

#include "Formats/LBAIso.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace LBA1Private
{
	/** Where GOG and Steam install LBA1's data (the "classic" game's Common folder). */
	const TCHAR* KnownInstalls[] =
	{
		TEXT("C:/GOG Games/tlba-classic/Common"),
		TEXT("D:/GOG Games/tlba-classic/Common"),
		TEXT("C:/Program Files (x86)/GOG Galaxy/Games/tlba-classic/Common"),
		TEXT("C:/Program Files (x86)/Steam/steamapps/common/Little Big Adventure/classic/Common"),
	};

	bool HasData(const FString& Directory)
	{
		return !Directory.IsEmpty() && IFileManager::Get().FileExists(*FPaths::Combine(Directory, TEXT("SCENE.HQR")))
			&& IFileManager::Get().FileExists(*FPaths::Combine(Directory, TEXT("LBA_GRI.HQR")));
	}
}

bool LBA1::FScene::Load(const TArray<uint8>& Data, int32 InIndex)
{
	Index = InIndex;
	Zones.Reset();
	FLBAReader R(Data);
	Bank = R.U8();
	R.Skip(1 + 4 + 4 + 28 + 1 + 6);
	for (int32 Script = 0; Script < 2; ++Script)
	{
		R.Skip(R.U16());
	}
	const int32 Actors = R.U16();
	for (int32 Actor = 1; Actor < Actors; ++Actor)
	{
		R.Skip(35);
		for (int32 Script = 0; Script < 2; ++Script)
		{
			R.Skip(R.U16());
		}
	}
	// One read per statement: a constructor's arguments are read in no set order.
	auto ReadPoint = [&R]()
	{
		FIntVector Point;
		Point.X = R.S16();
		Point.Y = R.S16();
		Point.Z = R.S16();
		return Point;
	};
	const int32 NumZones = R.U16();
	for (int32 Count = 0; Count < NumZones && R.IsOk(); ++Count)
	{
		FZone& Zone = Zones.AddDefaulted_GetRef();
		Zone.Min = ReadPoint();
		Zone.Max = ReadPoint();
		Zone.Type = R.U16();
		Zone.Number = R.S16();
		Zone.Info = ReadPoint();
		R.Skip(2);
	}
	return R.IsOk();
}

bool LBA1::FData::Open(const FString& Directory)
{
	using namespace LBA1Private;
	Folder = Directory;
	if (!HasData(Folder))
	{
		Folder.Reset();
		for (const TCHAR* Install : KnownInstalls)
		{
			if (HasData(Install))
			{
				Folder = Install;
				break;
			}
		}
	}
	if (Folder.IsEmpty() || !Scenes.Open(Folder / TEXT("SCENE.HQR")) || !Grids.Open(Folder / TEXT("LBA_GRI.HQR"))
		|| !Libraries.Open(Folder / TEXT("LBA_BLL.HQR")) || !Bricks.Open(Folder / TEXT("LBA_BRK.HQR")) || !Ress.Open(Folder / TEXT("RESS.HQR")))
	{
		return false;
	}
	const TArray<uint8> Colors = Ress.GetEntry(0);
	for (int32 Index = 0; Index + 2 < FMath::Min(Colors.Num(), 768); Index += 3)
	{
		Palette.Add(FVector3f(Colors[Index], Colors[Index + 1], Colors[Index + 2]));
	}
	return Palette.Num() == 256;
}

bool LBA1::FData::LoadScene(int32 Index, FScene& Out) const
{
	TArray<uint8> Data;
	return Scenes.GetEntry(Index, Data) && Out.Load(Data, Index);
}

bool LBA1::FData::LoadPlan(int32 Index, FPlan& Out) const
{
	TArray<uint8> Grid;
	TArray<uint8> LibraryData;
	FLBALibrary Library;
	if (!Grids.GetEntry(Index, Grid) || !Libraries.GetEntry(Index, LibraryData) || !Library.Load(LibraryData, Index) || Grid.Num() < SceneCells * SceneCells * 2)
	{
		return false;
	}
	constexpr int32 Columns = SceneCells * SceneCells;
	Out.Heights.Init(-1, Columns);
	Out.Colors.Init(FVector3f::ZeroVector, Columns);
	Out.Water.Init(false, Columns);
	FLBAReader R(Grid);
	for (int32 Column = 0; Column < Columns; ++Column)
	{
		// The column's cells bottom up: (1-based layout, block), 0 when empty.
		TArray<FIntPoint, TInlineAllocator<25>> Cells;
		int32 At = R.U16At(Column * 2);
		const int32 Runs = R.U8At(At++);
		for (int32 Run = 0; Run < Runs; ++Run)
		{
			const uint8 Flags = R.U8At(At++);
			const int32 Count = (Flags & 63) + 1;
			const int32 Type = Flags >> 6;
			for (int32 K = 0; K < Count; ++K)
			{
				FIntPoint Cell(0, 0);
				if (Type == 1)
				{
					Cell = FIntPoint(R.U8At(At), R.U8At(At + 1));
					At += 2;
				}
				else if (Type >= 2)
				{
					Cell = FIntPoint(R.U8At(At), R.U8At(At + 1));
				}
				Cells.Add(Cell);
			}
			if (Type >= 2)
			{
				At += 2;
			}
		}
		for (int32 Y = FMath::Min(Cells.Num(), 25) - 1; Y >= 0; --Y)
		{
			const int32 Layout = Cells[Y].X;
			const int32 Block = Cells[Y].Y;
			if (Layout < 1 || Layout > Library.Layouts.Num() || Block >= Library.Layouts[Layout - 1].Blocks.Num())
			{
				continue;
			}
			const FLBALayoutBlock& Brick = Library.Layouts[Layout - 1].Blocks[Block];
			if (Brick.Brick == 0)
			{
				continue;
			}
			Out.Heights[Column] = Y;
			Out.Colors[Column] = GetTopColor(Brick.Brick);
			Out.Water[Column] = Brick.Code == WaterCode;
			break;
		}
	}
	return R.IsOk();
}

FVector3f LBA1::FData::GetTopColor(int32 Brick) const
{
	if (const FVector3f* Known = TopColors.Find(Brick))
	{
		return *Known;
	}
	FLBABrickSprite Sprite;
	Sprite.Decode(Bricks.GetEntry(Brick - 1));
	FVector3f Top = FVector3f::ZeroVector, All = FVector3f::ZeroVector;
	int32 NumTop = 0, NumAll = 0;
	for (int32 Y = 0; Y < FLBABrickSprite::Height; ++Y)
	{
		for (int32 X = 0; X < FLBABrickSprite::Width; ++X)
		{
			const uint8 Color = Sprite.Pixels.IsValidIndex(Y * FLBABrickSprite::Width + X) ? Sprite.Pixels[Y * FLBABrickSprite::Width + X] : 0;
			if (Color == 0)
			{
				continue;
			}
			All += Palette[Color];
			++NumAll;
			if (FMath::Abs(X - 23.5f) / 24.f + FMath::Abs(Y - 11.5f) / 12.f <= 0.8f)
			{
				Top += Palette[Color];
				++NumTop;
			}
		}
	}
	const FVector3f Color = NumTop ? Top / float(NumTop) : NumAll ? All / float(NumAll) : FVector3f::ZeroVector;
	TopColors.Add(Brick, Color);
	return Color;
}

TArray<FVector2D> LBA1::FData::LoadMarkers() const
{
	// Each marker looks along Rx(angle x) Ry(angle y) (0, 0, 1); 270 degrees turn LBA1's longitudes onto LBA2's texture.
	TArray<FVector2D> Markers;
	const TArray<uint8> Data = Ress.GetEntry(8);
	FLBAReader R(Data);
	while (R.CanRead(8))
	{
		const double A = R.S16() / 1024.0 * UE_DOUBLE_TWO_PI;
		const double B = R.S16() / 1024.0 * UE_DOUBLE_TWO_PI;
		R.Skip(4);
		const FVector Look(FMath::Sin(B), -FMath::Sin(A) * FMath::Cos(B), FMath::Cos(A) * FMath::Cos(B));
		Markers.Add(FVector2D(FMath::RadiansToDegrees(-FMath::Asin(Look.Y)), FMath::RadiansToDegrees(FMath::Atan2(Look.X, Look.Z)) + 270.0));
	}
	return Markers;
}
