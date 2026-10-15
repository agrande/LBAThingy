#include "Formats/LBAIso.h"

#include "Formats/LBAHqr.h"
#include "LBA2Runtime.h"

bool FLBABkgHeader::Load(const FLBAHqrFile& Bkg)
{
	const TArray<uint8> Data = Bkg.GetEntry(0);
	if (Data.Num() < 12)
	{
		return false;
	}
	FLBAReader R(Data);
	GridStart = R.U16();
	GrmStart = R.U16();
	LibraryStart = R.U16();
	BrickStart = R.U16();
	NumBricks = R.U16();
	ForbiddenBrick = R.U16();
	return true;
}

bool FLBALibrary::Load(const TArray<uint8>& Data, int32 InIndex)
{
	Index = InIndex;
	Layouts.Reset();
	FLBAReader R(Data);
	const int32 NumLayouts = int32(R.U32At(0) / 4);
	if (NumLayouts <= 0 || NumLayouts > 4096)
	{
		return false;
	}
	for (int32 LayoutIndex = 0; LayoutIndex < NumLayouts; ++LayoutIndex)
	{
		const int32 At = int32(R.U32At(LayoutIndex * 4));
		FLBALayout& Layout = Layouts.AddDefaulted_GetRef();
		Layout.Index = LayoutIndex;
		Layout.SizeX = FMath::Max<int32>(1, R.U8At(At));
		Layout.SizeY = FMath::Max<int32>(1, R.U8At(At + 1));
		Layout.SizeZ = FMath::Max<int32>(1, R.U8At(At + 2));
		const int32 NumBlocks = Layout.SizeX * Layout.SizeY * Layout.SizeZ;
		Layout.Blocks.SetNum(NumBlocks);
		for (int32 Block = 0; Block < NumBlocks; ++Block)
		{
			const int32 B = At + 3 + Block * 4;
			Layout.Blocks[Block].Shape = R.U8At(B);
			Layout.Blocks[Block].Code = R.U8At(B + 1);
			Layout.Blocks[Block].Brick = R.U16At(B + 2);
		}
	}
	return R.IsOk();
}

bool FLBAGrid::Load(const TArray<uint8>& Data, int32 InIndex)
{
	Index = InIndex;
	Cells.Reset();
	Cells.SetNum(64 * 25 * 64);
	if (Data.Num() < 34 + 4096 * 2)
	{
		return false;
	}

	// T_GRI_HEADER: { U8 Library; U8 Grm; U8 UsedBlocks[32] } followed by 64*64 U16 column offsets.
	FLBAReader R(Data);
	LibraryIndex = R.U8At(0);
	GrmIndex = R.U8At(1);

	for (int32 Column = 0; Column < 64 * 64; ++Column)
	{
		const int32 X = Column % 64;
		const int32 Z = Column / 64;
		int32 At = int32(R.U16At(34 + Column * 2)) + 34;
		const int32 NumRuns = R.U8At(At++);
		int32 Y = 0;
		for (int32 Run = 0; Run < NumRuns && Y < 25; ++Run)
		{
			const uint8 Flags = R.U8At(At++);
			const int32 Type = Flags >> 6;
			const int32 Height = (Flags & 31) + 1;
			if (Type == 2)
			{
				// Same block repeated for the whole run.
				const int16 Layout = int16(R.U8At(At)) - 1;
				const uint8 Block = R.U8At(At + 1);
				At += 2;
				for (int32 K = 0; K < Height && Y + K < 25; ++K)
				{
					Cells[CellIndex(X, Y + K, Z)] = { Layout, Block };
				}
			}
			else if (Type == 1)
			{
				for (int32 K = 0; K < Height; ++K)
				{
					const int16 Layout = int16(R.U8At(At)) - 1;
					const uint8 Block = R.U8At(At + 1);
					At += 2;
					if (Y + K < 25)
					{
						Cells[CellIndex(X, Y + K, Z)] = { Layout, Block };
					}
				}
			}
			Y += Height;
		}
	}
	return R.IsOk();
}

bool FLBABrickSprite::Decode(const TArray<uint8>& Data)
{
	// Header: { U8 Width; U8 Height; U8 OffsetX; U8 OffsetY } then per line: U8 runs, each run is
	// { U8 spec (bits 0-5 length-1, bits 6-7 type) [+ data] }: type 0 skip, 1/3 raw pixels, 2 fill.
	Pixels.Reset();
	Pixels.SetNumZeroed(Width * Height);
	if (Data.Num() < 4)
	{
		return false;
	}
	FLBAReader R(Data);
	const int32 LineCount = R.U8At(1);
	const int32 OffsetX = R.U8At(2);
	const int32 OffsetY = R.U8At(3);
	int32 At = 4;
	for (int32 Line = 0; Line < LineCount; ++Line)
	{
		const int32 NumRuns = R.U8At(At++);
		int32 X = 0;
		const int32 Y = Line + OffsetY;
		for (int32 Run = 0; Run < NumRuns; ++Run)
		{
			const uint8 Spec = R.U8At(At++);
			const int32 Length = (Spec & 63) + 1;
			const int32 Type = Spec >> 6;
			for (int32 K = 0; K < Length; ++K)
			{
				uint8 Color = 0;
				if (Type == 2)
				{
					Color = R.U8At(At);
				}
				else if (Type == 1 || Type == 3)
				{
					Color = R.U8At(At + K);
				}
				const int32 Px = X + OffsetX;
				if (Type != 0 && Px >= 0 && Px < Width && Y >= 0 && Y < Height)
				{
					Pixels[Y * Width + Px] = Color;
				}
				++X;
			}
			if (Type == 2)
			{
				At += 1;
			}
			else if (Type == 1 || Type == 3)
			{
				At += Length;
			}
		}
	}
	return R.IsOk();
}

TArray<FLBASceneMapEntry> LBAIso::LoadSceneMap(const TArray<uint8>& Data)
{
	TArray<FLBASceneMapEntry> Map;
	for (int32 At = 0; At + 1 < Data.Num(); At += 2)
	{
		const uint8 Opcode = Data[At];
		if (Opcode == 0)
		{
			break;
		}
		Map.Add({ Opcode == 2, int32(Data[At + 1]) });
	}
	return Map;
}
