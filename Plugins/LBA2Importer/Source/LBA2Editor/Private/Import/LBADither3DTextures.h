/*
 * Copyright (c) 2025 Rune Skovbo Johansen
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Pattern data of Surface-Stable Fractal Dithering (Shaders/Private/LBADither3D.ush): a 3D texture of Bayer dot
 * patterns (one more dot per layer) and the ramp that maps a brightness to a dot threshold.
 */
struct FLBADither3DPattern
{
	/** 16 * DotsPerSide. */
	int32 Size = 0;
	/** DotsPerSide^2. */
	int32 Layers = 0;
	int32 DotsPerSide = 0;
	/** Size * Size * Layers radial gradients, X fastest. */
	TArray<uint8> Dots;
	/** Size thresholds (0-65535). */
	TArray<uint16> Ramp;

	/** Recursion: 1 = 2x2 dots, 2 = 4x4, 3 = 8x8. */
	static FLBADither3DPattern Build(int32 Recursion);
};
