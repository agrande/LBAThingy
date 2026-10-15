#include "Formats/LBAHqr.h"

#include "LBA2Runtime.h"
#include "Misc/FileHelper.h"

bool FLBAHqrFile::Open(const FString& InPath)
{
	Path = InPath;
	Data.Reset();
	Offsets.Reset();

	if (!FFileHelper::LoadFileToArray(Data, *InPath))
	{
		UE_LOG(LogLBA2, Warning, TEXT("Could not read %s"), *InPath);
		return false;
	}

	FLBAReader Reader(Data);
	const uint32 TableSize = Reader.U32At(0);
	if (TableSize < 4 || TableSize > uint32(Data.Num()) || (TableSize % 4) != 0)
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s does not look like an HQR file"), *InPath);
		Data.Reset();
		return false;
	}

	// The last offset of the table is an end marker (the original HQF_NbRes returns count - 1).
	const int32 NumOffsets = int32(TableSize / 4);
	for (int32 Index = 0; Index < NumOffsets - 1; ++Index)
	{
		Offsets.Add(Reader.U32At(Index * 4));
	}
	return true;
}

bool FLBAHqrFile::HasEntry(int32 Index) const
{
	if (!Offsets.IsValidIndex(Index))
	{
		return false;
	}
	const uint32 Offset = Offsets[Index];
	return Offset != 0 && Offset + 10 <= uint32(Data.Num());
}

TArray<uint8> FLBAHqrFile::GetEntry(int32 Index) const
{
	TArray<uint8> Out;
	GetEntry(Index, Out);
	return Out;
}

bool FLBAHqrFile::GetEntry(int32 Index, TArray<uint8>& Out) const
{
	Out.Reset();
	if (!HasEntry(Index))
	{
		return false;
	}

	FLBAReader Reader(Data, Offsets[Index]);
	const uint32 Size = Reader.U32();
	const uint32 CompressedSize = Reader.U32();
	const uint16 Method = Reader.U16();
	const int32 Start = Reader.Tell();

	if (uint64(Start) + CompressedSize > uint64(Data.Num()) || Size > 64u * 1024u * 1024u)
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: entry %d is truncated"), *Path, Index);
		return false;
	}

	Out.SetNumZeroed(Size);
	if (Method == 0)
	{
		FMemory::Memcpy(Out.GetData(), Data.GetData() + Start, FMath::Min(Size, CompressedSize));
	}
	else if (Method <= 2)
	{
		ExpandLZ(Data.GetData() + Start, CompressedSize, Out.GetData(), Size, Method + 1);
	}
	else
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: entry %d uses unknown compression %d"), *Path, Index, Method);
		Out.Reset();
		return false;
	}
	return true;
}

void FLBAHqrFile::ExpandLZ(const uint8* Src, int32 SrcSize, uint8* Dst, int32 DstSize, int32 MinMatch)
{
	// Port of ExpandLZ (LIB386/SYSTEM/LZ.ASM). One flag byte describes the next 8 tokens; a set bit is a
	// literal byte, a clear bit is a 16-bit back reference: 12-bit (distance - 1) and 4-bit (length - MinMatch).
	int32 In = 0;
	int32 OutPos = 0;
	while (OutPos < DstSize && In < SrcSize)
	{
		const uint8 Flags = Src[In++];
		for (int32 Bit = 0; Bit < 8 && OutPos < DstSize; ++Bit)
		{
			if (Flags & (1 << Bit))
			{
				if (In >= SrcSize)
				{
					return;
				}
				Dst[OutPos++] = Src[In++];
			}
			else
			{
				if (In + 1 >= SrcSize)
				{
					return;
				}
				const uint16 Word = uint16(Src[In]) | (uint16(Src[In + 1]) << 8);
				In += 2;
				const int32 Length = (Word & 0x0F) + MinMatch;
				const int32 Distance = (Word >> 4) + 1;
				int32 From = OutPos - Distance;
				for (int32 Count = 0; Count < Length && OutPos < DstSize; ++Count)
				{
					// Overlapping copies are intentional (run-length style), so copy byte by byte.
					Dst[OutPos++] = From >= 0 ? Dst[From] : 0;
					++From;
				}
			}
		}
	}
}
