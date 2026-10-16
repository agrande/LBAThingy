#include "Formats/LBAXpl.h"

#include "Formats/LBAHqr.h"
#include "LBA2Runtime.h"

namespace
{
	// RESS_XPL0 + island number (IleLst in EXTFUNC.CPP), and RESS_XPL00 for Citadel without the storm.
	constexpr int32 FirstIslandEntry = 27;
	const TCHAR* IslandOrder[] =
	{
		TEXT("CITADEL"), TEXT("SENDELL"), TEXT("DESERT"), TEXT("EMERAUDE"), TEXT("OTRINGAL"), TEXT("CELEBRAT"),
		TEXT("PLATFORM"), TEXT("MOSQUIBE"), TEXT("KNARTAS"), TEXT("ILOTCX"), TEXT("ASCENCE"), TEXT("SOUSCELB"),
	};

	constexpr int32 XplHeaderSize = 44;
	constexpr int32 XplPaletteSize = 768;
	constexpr int32 XplShadeTableSize = 256 * 256;
}

bool FLBAXpl::Load(const FLBAHqrFile& Ress, int32 InEntry)
{
	const TArray<uint8> Data = Ress.GetEntry(InEntry);
	FLBAReader Reader(Data);
	if (Data.Num() < XplHeaderSize)
	{
		return false;
	}
	// S32 Version, OffsetPalette, OffsetShade, OffsetFog, OffsetTransp, ShadeStartPercent, ShadeNormalLevel,
	// ShadeEndPercent, FogCoul, TranspStartPercent, TranspEndPercent.
	const int32 OffsetPalette = Reader.S32At(4);
	const int32 OffsetFog = Reader.S32At(12);
	if (OffsetPalette < XplHeaderSize || OffsetPalette + XplPaletteSize > Data.Num() || OffsetFog < 0 || OffsetFog + XplShadeTableSize > Data.Num())
	{
		UE_LOG(LogLBA2, Warning, TEXT("RESS.HQR entry %d is not an island palette"), InEntry);
		return false;
	}

	Entry = InEntry;
	ShadeNormalLevel = FMath::Clamp(Reader.S32At(24), 0, NumShadeRows - 1);
	FogColor = Reader.S32At(32) & 255;
	Palette.SetNum(256);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const uint8* RGB = Data.GetData() + OffsetPalette + Index * 3;
		Palette[Index] = FColor(RGB[0], RGB[1], RGB[2], 255);
	}
	Shades = TArray<uint8>(Data.GetData() + OffsetFog, XplShadeTableSize);
	return true;
}

int32 FLBAXpl::GetEntryForIsland(const FString& IslandName)
{
	// CITABAU is Citadel after the storm, CELEBRA2 Celebration after the volcano woke up, MOON the Emerald moon.
	const FString Name = IslandName.ToUpper();
	if (Name == TEXT("CITABAU"))
	{
		return InteriorEntry;
	}
	const FString Base = Name == TEXT("CELEBRA2") ? TEXT("CELEBRAT") : Name == TEXT("MOON") ? TEXT("EMERAUDE") : Name;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(IslandOrder); ++Index)
	{
		if (Base == IslandOrder[Index])
		{
			return FirstIslandEntry + Index;
		}
	}
	return InteriorEntry;
}

int32 FLBAXpl::GetSkySeaEntry(int32 XplEntry)
{
	// RESS_SKYSEA0 + island number, RESS_SKYSEA00 for Citabau.
	constexpr int32 FirstSkySeaEntry = 11;
	constexpr int32 CitabauSkySeaEntry = 26;
	return XplEntry == InteriorEntry ? CitabauSkySeaEntry : FirstSkySeaEntry + (XplEntry - FirstIslandEntry);
}

FString FLBAXpl::GetName(int32 InEntry)
{
	if (InEntry == InteriorEntry)
	{
		return TEXT("Citabau");
	}
	const int32 Island = InEntry - FirstIslandEntry;
	if (Island >= 0 && Island < UE_ARRAY_COUNT(IslandOrder))
	{
		FString Name = FString(IslandOrder[Island]).ToLower();
		Name[0] = FChar::ToUpper(Name[0]);
		return Name;
	}
	return FString::Printf(TEXT("Xpl%d"), InEntry);
}
