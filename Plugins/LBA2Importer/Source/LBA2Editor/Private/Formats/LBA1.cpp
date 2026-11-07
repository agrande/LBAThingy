#include "Formats/LBA1.h"

#include "Formats/LBAIso.h"
#include "HAL/FileManager.h"
#include "LBATypes.h"
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

bool LBA1::FData::LoadGrid(int32 Index, FLBAGrid& Out) const
{
	TArray<uint8> Data;
	if (!Grids.GetEntry(Index, Data) || Data.Num() < SceneCells * SceneCells * 2)
	{
		return false;
	}
	Out.Index = Index;
	Out.LibraryIndex = Index;
	Out.GrmIndex = 0;
	Out.Cells.Reset();
	Out.Cells.SetNum(SceneCells * LBA::GridSizeY * SceneCells);
	FLBAReader R(Data);
	for (int32 Column = 0; Column < SceneCells * SceneCells; ++Column)
	{
		// The column's cells bottom up: (1-based layout, block), layout 0 when empty.
		const int32 X = Column % SceneCells;
		const int32 Z = Column / SceneCells;
		int32 At = R.U16At(Column * 2);
		const int32 Runs = R.U8At(At++);
		int32 Y = 0;
		for (int32 Run = 0; Run < Runs; ++Run)
		{
			const uint8 Flags = R.U8At(At++);
			const int32 Count = (Flags & 63) + 1;
			const int32 Type = Flags >> 6;
			for (int32 K = 0; K < Count; ++K, ++Y)
			{
				FLBAGridCell Cell;
				if (Type >= 1)
				{
					Cell.Layout = int16(R.U8At(At)) - 1;
					Cell.Block = R.U8At(At + 1);
					At += Type == 1 ? 2 : 0;
				}
				if (Y < LBA::GridSizeY)
				{
					Out.Cells[FLBAGrid::CellIndex(X, Y, Z)] = Cell;
				}
			}
			At += Type >= 2 ? 2 : 0;
		}
	}
	return R.IsOk();
}

bool LBA1::FData::LoadLibrary(int32 Index, FLBALibrary& Out) const
{
	TArray<uint8> Data;
	return Libraries.GetEntry(Index, Data) && Out.Load(Data, Index);
}

bool LBA1::FData::LoadBrick(int32 Brick, FLBABrickSprite& Out) const
{
	TArray<uint8> Data;
	return Brick > 0 && Bricks.GetEntry(Brick - 1, Data) && Out.Decode(Data);
}

TArray<FColor> LBA1::FData::GetPalette() const
{
	TArray<FColor> Colors;
	for (const FVector3f& Color : Palette)
	{
		Colors.Add(FColor(uint8(Color.X), uint8(Color.Y), uint8(Color.Z)));
	}
	return Colors;
}

bool LBA1::FData::LoadPlan(int32 Index, FPlan& Out) const
{
	FLBAGrid Grid;
	FLBALibrary Library;
	if (!LoadGrid(Index, Grid) || !LoadLibrary(Index, Library))
	{
		return false;
	}
	constexpr int32 Columns = SceneCells * SceneCells;
	Out.Heights.Init(-1, Columns);
	Out.Colors.Init(FVector3f::ZeroVector, Columns);
	Out.Water.Init(false, Columns);
	for (int32 Column = 0; Column < Columns; ++Column)
	{
		for (int32 Y = LBA::GridSizeY - 1; Y >= 0; --Y)
		{
			const FLBALayoutBlock* Brick = GetBlock(Library, Grid.Get(Column % SceneCells, Y, Column / SceneCells));
			if (!Brick || Brick->Brick == 0)
			{
				continue;
			}
			Out.Heights[Column] = Y;
			Out.Colors[Column] = GetTopColor(Brick->Brick);
			Out.Water[Column] = Brick->Code == WaterCode;
			break;
		}
	}
	return true;
}

const FLBALayoutBlock* LBA1::GetBlock(const FLBALibrary& Library, const FLBAGridCell& Cell)
{
	if (!Library.Layouts.IsValidIndex(Cell.Layout))
	{
		return nullptr;
	}
	const FLBALayout& Layout = Library.Layouts[Cell.Layout];
	return Layout.Blocks.IsValidIndex(Cell.Block) ? &Layout.Blocks[Cell.Block] : nullptr;
}

bool LBA1::FData::IsBlankBrick(int32 Brick) const
{
	if (const bool* Known = BlankBricks.Find(Brick))
	{
		return *Known;
	}
	FLBABrickSprite Sprite;
	bool bBlank = LoadBrick(Brick, Sprite);
	for (const uint8 Pixel : Sprite.Pixels)
	{
		bBlank = bBlank && Pixel == 0;
	}
	BlankBricks.Add(Brick, bBlank);
	return bBlank;
}

FVector3f LBA1::FData::GetTopColor(int32 Brick) const
{
	if (const FVector3f* Known = TopColors.Find(Brick))
	{
		return *Known;
	}
	FLBABrickSprite Sprite;
	LoadBrick(Brick, Sprite);
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
