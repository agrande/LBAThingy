#pragma once

#include "CoreMinimal.h"

/**
 * LBA2 animation (ANIM.HQR). See LIB386/ANIM/ANIM.ASM.
 * Header: { U16 NumKeyframes; U16 NumGroups; U16 LoopFrame; U16 Pad }.
 * Keyframe: { U16 DurationMs; S16 StepX, StepY, StepZ } then NumGroups x { S16 Type, Alpha, Beta, Gamma }.
 * Group 0 is not applied to the skeleton: its angles are the actor's rotation step (root motion).
 * Type 0 groups rotate (Rx(alpha) * Rz(gamma) * Ry(beta)), type 1 groups translate by (alpha, beta, gamma).
 */
struct FLBAAnimGroupFrame
{
	int16 Type = 0;
	int16 Alpha = 0;
	int16 Beta = 0;
	int16 Gamma = 0;
};

struct FLBAAnimKeyframe
{
	int32 DurationMs = 0;
	FIntVector Step = FIntVector::ZeroValue;
	TArray<FLBAAnimGroupFrame> Groups;
};

struct FLBAAnim
{
	int32 NumGroups = 0;
	int32 LoopFrame = 0;
	TArray<FLBAAnimKeyframe> Keyframes;

	bool Parse(const TArray<uint8>& Data);
	int32 GetTotalDurationMs() const;
};

/** Gameplay actions attached to an entity animation (sounds, footsteps, hits, projectiles...). */
struct FLBAAnimAction
{
	int32 Type = 0;
	int32 Frame = 0;
	TArray<uint8> Params;

	static const TCHAR* GetTypeName(int32 Type);
	int32 GetU16(int32 Offset) const { return Params.IsValidIndex(Offset + 1) ? int32(Params[Offset] | (Params[Offset + 1] << 8)) : 0; }
};

struct FLBAEntityBodyEntry
{
	int32 Type = 0;
	int32 BodyIndex = INDEX_NONE;
	bool bHasBox = false;
	FIntVector BoxMin = FIntVector::ZeroValue;
	FIntVector BoxMax = FIntVector::ZeroValue;
};

struct FLBAEntityAnimEntry
{
	int32 Type = 0;
	int32 AnimIndex = INDEX_NONE;
	TArray<FLBAAnimAction> Actions;
};

/** One entry of RESS.HQR entry 44 (the "F3D" entity file). */
struct FLBAEntity
{
	int32 Index = 0;
	TArray<FLBAEntityBodyEntry> Bodies;
	TArray<FLBAEntityAnimEntry> Anims;

	static TArray<FLBAEntity> ParseAll(const TArray<uint8>& Data);
};

namespace ELBAAnimActionType
{
	enum Type : int32
	{
		Hit = 5,
		Sample = 6,
		SampleRnd = 7,
		Throw = 8,
		ThrowMagic = 9,
		SampleRepeat = 10,
		ThrowSearch = 11,
		ThrowAlpha = 12,
		SampleStop = 13,
		ZV = 14,
		LeftStep = 15,
		RightStep = 16,
		HitHero = 17,
		SuperHit = 22,
		NewSample = 39,
	};
}
