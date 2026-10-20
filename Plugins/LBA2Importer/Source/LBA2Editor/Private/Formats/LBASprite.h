#pragma once

#include "CoreMinimal.h"

/** Sprite banks of scene objects (see GetPtrSprite in SOURCES/INTEXT.CPP and InitSprite in OBJECT.CPP). */
enum class ELBASpriteBank : uint8
{
	/** SPRITES.HQR, run-length encoded; scene sprite numbers >= 100. Boxes in RESS.HQR entry 5. */
	Sprites,
	/** SPRIRAW.HQR, uncompressed; scene sprite numbers < 100. Boxes in RESS.HQR entry 8. */
	Raw,
	/** ANIM3DS.HQR frames of animated sprites (ANIM_3DS objects), run-length encoded. Boxes in RESS.HQR entry 43. */
	Anim,
};

/**
 * A palette-indexed sprite. Each HQR entry is a one-sprite bank: a U32 offset table, then { U8 width, U8 height,
 * S8 hot x, S8 hot y } and the pixels (color 0 is transparent). Run-length lines start with their run count; each run
 * is a U8 (bits 0-5 = length - 1, bits 6-7 = 0 skip, 2 one color repeated, 1/3 literal colors).
 */
struct FLBASprite
{
	int32 Width = 0;
	int32 Height = 0;
	int32 HotX = 0;
	int32 HotY = 0;
	TArray<uint8> Pixels;

	bool Decode(const TArray<uint8>& Entry, bool bRunLength);
};

/**
 * Per-sprite placement (8 x S16): the screen offset of the sprite from the object's projected position, then the
 * object's box (x min, x max, y min, y max, z min, z max) in LBA units relative to the object.
 */
struct FLBASpriteBox
{
	FIntPoint ScreenOffset = FIntPoint::ZeroValue;
	FIntVector Min = FIntVector::ZeroValue;
	FIntVector Max = FIntVector::ZeroValue;

	bool IsEmpty() const { return Min.X >= Max.X && Min.Y >= Max.Y && Min.Z >= Max.Z; }
};

namespace LBASprites
{
	TArray<FLBASpriteBox> LoadBoxes(const TArray<uint8>& Data);

	/** The isometric projection of interiors: screen pixels of an LBA offset (x right/down, z left/down, y up). */
	inline FVector2D Project(const FVector& Lba)
	{
		return FVector2D((Lba.X - Lba.Z) * 24.0 / 512.0, (Lba.X + Lba.Z) * 12.0 / 512.0 - Lba.Y * 15.0 / 256.0);
	}
}
