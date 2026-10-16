/*
 * Copyright (c) 2025 Rune Skovbo Johansen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// Port of Dither3DTextureMaker.cs (https://github.com/runevision/Dither3D), by way of the Zeboulon IllustrativeStyle
// plugin. Changes from the original: the data is returned for the importer to store in texture assets, the ramp is
// stored as 16-bit, and the inverse ramp interpolates between the two buckets around the desired brightness (the
// original kept the lower bound at 0).

#include "Import/LBADither3DTextures.h"

FLBADither3DPattern FLBADither3DPattern::Build(int32 Recursion)
{
	Recursion = FMath::Clamp(Recursion, 1, 3);

	// Bayer points: each recursion repeats the existing points at the 3 other offsets of a 2x2 grid, scaled down.
	TArray<FVector2f> BayerPoints = { FVector2f(0.0f, 0.0f), FVector2f(0.5f, 0.5f), FVector2f(0.5f, 0.0f), FVector2f(0.0f, 0.5f) };
	for (int32 R = 0; R < Recursion - 1; ++R)
	{
		const int32 Count = BayerPoints.Num();
		const float Offset = FMath::Pow(0.5f, (float)(R + 1));
		for (int32 I = 1; I < 4; ++I)
		{
			for (int32 J = 0; J < Count; ++J)
			{
				BayerPoints.Add(BayerPoints[J] + BayerPoints[I] * Offset);
			}
		}
	}

	// The shader infers the layer count and the ramp width from DotsPerSide; keep these relationships in sync.
	FLBADither3DPattern Data;
	Data.DotsPerSide = 1 << Recursion;
	Data.Layers = Data.DotsPerSide * Data.DotsPerSide;
	Data.Size = 16 * Data.DotsPerSide;
	Data.Dots.SetNumUninitialized(Data.Size * Data.Size * Data.Layers);

	// How many texels are above given brightness levels, to build the brightness lookup curve.
	constexpr int32 BucketCount = 256;
	TArray<int32> BrightnessBuckets;
	BrightnessBuckets.SetNumZeroed(BucketCount);

	const float InvRes = 1.0f / Data.Size;
	for (int32 Z = 0; Z < Data.Layers; ++Z)
	{
		const int32 DotCount = Z + 1;
		const float DotArea = 0.5f / DotCount;
		const float DotRadius = FMath::Sqrt(DotArea / UE_PI);

		for (int32 Y = 0; Y < Data.Size; ++Y)
		{
			for (int32 X = 0; X < Data.Size; ++X)
			{
				const FVector2f Point((X + 0.5f) * InvRes, (Y + 0.5f) * InvRes);
				float Dist = UE_BIG_NUMBER;
				for (int32 I = 0; I < DotCount; ++I)
				{
					FVector2f Vec = Point - BayerPoints[I];
					Vec.X = FMath::Wrap(Vec.X + 0.5f, 0.0f, 1.0f) - 0.5f;
					Vec.Y = FMath::Wrap(Vec.Y + 0.5f, 0.0f, 1.0f) - 0.5f;
					Dist = FMath::Min(Dist, Vec.Size());
				}
				// Radial gradient around each dot.
				const float Value = FMath::Clamp(1.0f - Dist / (DotRadius * 2.4f), 0.0f, 1.0f);

				Data.Dots[X + Y * Data.Size + Z * Data.Size * Data.Size] = (uint8)FMath::RoundToInt(Value * 255.0f);
				BrightnessBuckets[FMath::Clamp((int32)(Value * BucketCount), 0, BucketCount - 1)]++;
			}
		}
	}

	// Brightness ramp: the fraction of texels that pass each threshold, from the highest threshold down.
	TArray<float> BrightnessRamp;
	BrightnessRamp.SetNumZeroed(BucketCount + 1);
	const float PixelCount = (float)Data.Dots.Num();
	int32 Sum = 0;
	for (int32 I = 0; I < BucketCount; ++I)
	{
		Sum += BrightnessBuckets[BucketCount - 1 - I];
		BrightnessRamp[I + 1] = Sum / PixelCount;
	}

	// Inverse ramp: the threshold that gives a desired brightness.
	Data.Ramp.SetNumUninitialized(Data.Size);
	int32 HigherIndex = 1;
	for (int32 I = 0; I < Data.Size; ++I)
	{
		const float DesiredBrightness = I / (float)(Data.Size - 1);
		while (HigherIndex < BucketCount && BrightnessRamp[HigherIndex] < DesiredBrightness)
		{
			++HigherIndex;
		}
		const float Lower = BrightnessRamp[HigherIndex - 1];
		const float Higher = BrightnessRamp[HigherIndex];
		const float L = Higher > Lower ? FMath::Clamp((DesiredBrightness - Lower) / (Higher - Lower), 0.0f, 1.0f) : 1.0f;
		const float Threshold = (HigherIndex - 1 + L) / (float)BucketCount;
		Data.Ramp[I] = (uint16)FMath::RoundToInt(FMath::Clamp(Threshold, 0.0f, 1.0f) * 65535.0f);
	}

	return Data;
}
