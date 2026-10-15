#pragma once

#include "CoreMinimal.h"

/**
 * Reader for Adeline's HQR resource containers (.HQR, .ILE, .OBL, .VOX).
 *
 * Layout: a table of little-endian U32 offsets (the first offset is also the table size), followed by
 * entries. Each entry starts with { U32 Size; U32 CompressedSize; U16 Method } where Method is
 * 0 = stored, 1 = LZSS (min match 2), 2 = LZMIT (min match 3). Zero offsets are empty entries.
 */
class FLBAHqrFile
{
public:
	bool Open(const FString& InPath);
	bool IsOpen() const { return Data.Num() > 0; }

	int32 Num() const { return Offsets.Num(); }
	bool HasEntry(int32 Index) const;

	/** Returns the decompressed entry, or an empty array when missing. */
	TArray<uint8> GetEntry(int32 Index) const;
	bool GetEntry(int32 Index, TArray<uint8>& Out) const;

	const FString& GetPath() const { return Path; }

	static void ExpandLZ(const uint8* Src, int32 SrcSize, uint8* Dst, int32 DstSize, int32 MinMatch);

private:
	FString Path;
	TArray<uint8> Data;
	TArray<uint32> Offsets;
};

/** Little helper to read little-endian values out of a byte buffer with bounds checks. */
class FLBAReader
{
public:
	FLBAReader(const TArray<uint8>& InData, int32 InOffset = 0)
		: Data(InData.GetData()), Size(InData.Num()), Offset(InOffset)
	{
	}

	FLBAReader(const uint8* InData, int32 InSize, int32 InOffset = 0)
		: Data(InData), Size(InSize), Offset(InOffset)
	{
	}

	bool CanRead(int32 Bytes) const { return Offset >= 0 && Offset + Bytes <= Size; }
	int32 Tell() const { return Offset; }
	void Seek(int32 InOffset) { Offset = InOffset; }
	void Skip(int32 Bytes) { Offset += Bytes; }
	int32 GetSize() const { return Size; }
	const uint8* GetData() const { return Data; }
	bool IsOk() const { return !bOverflow; }

	uint8 U8() { return Read<uint8>(); }
	int8 S8() { return Read<int8>(); }
	uint16 U16() { return Read<uint16>(); }
	int16 S16() { return Read<int16>(); }
	uint32 U32() { return Read<uint32>(); }
	int32 S32() { return Read<int32>(); }

	uint8 U8At(int32 At) const { return ReadAt<uint8>(At); }
	uint16 U16At(int32 At) const { return ReadAt<uint16>(At); }
	int16 S16At(int32 At) const { return ReadAt<int16>(At); }
	uint32 U32At(int32 At) const { return ReadAt<uint32>(At); }
	int32 S32At(int32 At) const { return ReadAt<int32>(At); }

private:
	template <typename T>
	T Read()
	{
		const T Value = ReadAt<T>(Offset);
		Offset += sizeof(T);
		return Value;
	}

	template <typename T>
	T ReadAt(int32 At) const
	{
		if (At < 0 || At + int32(sizeof(T)) > Size)
		{
			const_cast<FLBAReader*>(this)->bOverflow = true;
			return T(0);
		}
		T Value;
		FMemory::Memcpy(&Value, Data + At, sizeof(T));
		return Value;
	}

	const uint8* Data = nullptr;
	int32 Size = 0;
	int32 Offset = 0;
	bool bOverflow = false;
};
