#include "Formats/LBASprite.h"

#include "Formats/LBAHqr.h"

bool FLBASprite::Decode(const TArray<uint8>& Entry, bool bRunLength)
{
	FLBAReader R(Entry);
	R.Seek(int32(R.U32At(0)));
	Width = R.U8();
	Height = R.U8();
	HotX = R.S8();
	HotY = R.S8();
	if (!R.IsOk() || Width <= 0 || Height <= 0)
	{
		return false;
	}
	Pixels.SetNumZeroed(Width * Height);
	for (int32 Y = 0; Y < Height && R.IsOk(); ++Y)
	{
		if (!bRunLength)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Pixels[Y * Width + X] = R.U8();
			}
			continue;
		}
		const int32 Runs = R.U8();
		int32 X = 0;
		for (int32 Run = 0; Run < Runs && R.IsOk(); ++Run)
		{
			const uint8 Spec = R.U8();
			const int32 Length = (Spec & 63) + 1;
			const int32 Type = Spec >> 6;
			if (Type == 2)
			{
				const uint8 Color = R.U8();
				for (int32 I = 0; I < Length; ++I, ++X)
				{
					if (X < Width)
					{
						Pixels[Y * Width + X] = Color;
					}
				}
			}
			else if (Type == 1 || Type == 3)
			{
				for (int32 I = 0; I < Length; ++I, ++X)
				{
					const uint8 Color = R.U8();
					if (X < Width)
					{
						Pixels[Y * Width + X] = Color;
					}
				}
			}
			else
			{
				X += Length;
			}
		}
	}
	return R.IsOk();
}

TArray<FLBASpriteBox> LBASprites::LoadBoxes(const TArray<uint8>& Data)
{
	TArray<FLBASpriteBox> Boxes;
	FLBAReader R(Data);
	const int32 Count = Data.Num() / 16;
	Boxes.SetNum(Count);
	for (FLBASpriteBox& Box : Boxes)
	{
		Box.ScreenOffset.X = R.S16();
		Box.ScreenOffset.Y = R.S16();
		Box.Min.X = R.S16();
		Box.Max.X = R.S16();
		Box.Min.Y = R.S16();
		Box.Max.Y = R.S16();
		Box.Min.Z = R.S16();
		Box.Max.Z = R.S16();
	}
	return Boxes;
}
