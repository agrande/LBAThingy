#pragma once

#include "CoreMinimal.h"

class FLBAHqrFile;

/**
 * Island palettes (XPL resources in RESS.HQR, see XPL_HEADER in SOURCES/COMMON.H and ChoicePalette in AMBIANCE.CPP).
 *
 * Each exterior island has its own 256-color palette with a shade table: 16 fog levels of 16 shade rows, each row
 * mapping every palette index to the index it becomes at that shade (rows mostly move inside a 16-color ramp of the
 * palette). Row ShadeNormalLevel of fog level 0 is the unshaded color. Interiors use the Citadel (Citabau) palette.
 */
struct FLBAXpl
{
	static constexpr int32 NumShadeRows = 16;
	static constexpr int32 NumFogLevels = 16;

	/** RESS.HQR entry it was loaded from. */
	int32 Entry = INDEX_NONE;
	TArray<FColor> Palette;
	/** 256 rows (fog level * 16 + shade row) of 256 palette indices. */
	TArray<uint8> Shades;
	int32 ShadeNormalLevel = 12;
	int32 FogColor = 0;

	bool Load(const FLBAHqrFile& Ress, int32 InEntry);

	uint8 GetShade(int32 Index, int32 Row, int32 FogLevel = 0) const
	{
		return Shades[((FogLevel * NumShadeRows + Row) & 255) * 256 + (Index & 255)];
	}

	/** RESS.HQR entry of the palette the original uses for an island file (CITADEL, DESERT...). */
	static int32 GetEntryForIsland(const FString& IslandName);
	/** Palette of interiors (and of Citadel once the storm is over). */
	static constexpr int32 InteriorEntry = 42;
	/**
	 * RESS.HQR entry of the island's sky/sea texture page (256x256 palette indices: sea in the top-left 128x128,
	 * clouds in the top-right 128x128, animated wave strips below).
	 */
	static int32 GetSkySeaEntry(int32 XplEntry);
	/** Short name for generated assets (Citadel, Desert, Interior...). */
	static FString GetName(int32 InEntry);
};
