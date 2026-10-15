#pragma once

#include "CoreMinimal.h"

/**
 * LBA2 3D object ("body") format, used by BODY.HQR (characters), *.OBL (island decor) and OBJFIX.HQR
 * (inventory). Structures follow LIB386/H/AFF_OBJ.INC from the original engine.
 *
 * Objects are split into groups (bones). Points are stored contiguously per group, relative to the
 * group's origin. A group's origin is one of its parent's points (PivotPoint), so the rest pose has no
 * rotation and bone positions are the sum of pivots up the hierarchy.
 */
namespace ELBAPolyType
{
	enum Type : uint8
	{
		Solid = 0,
		Flat = 1,
		Trans = 2,
		Trame = 3,
		Gouraud = 4,
		Dither = 5,
		GouraudTable = 6,
		DitherTable = 7,
		Texture = 8,
		TextureFlat = 9,
		TextureGouraud = 10,
		TextureDither = 11,
		TextureIncrust = 12,
		TextureIncrustFlat = 13,
		TextureIncrustGouraud = 14,
		TextureIncrustDither = 15,
		TextureZ = 16,
		TextureZFog = 24,
	};
}

struct FLBABodyGroup
{
	int32 Parent = INDEX_NONE;
	int32 PivotPoint = 0;
	int32 NumPoints = 0;
	int32 NumNormals = 0;
	int32 FirstPoint = 0;
};

struct FLBABodyPoint
{
	FIntVector Position = FIntVector::ZeroValue;
	int32 Group = 0;
};

struct FLBABodyPoly
{
	uint8 Type = 0;
	bool bEnvMapped = false;
	int32 NumVertices = 3;
	int32 Vertices[4] = { 0, 0, 0, 0 };
	uint16 Color = 0;
	int32 TextureHandle = INDEX_NONE;
	/** Texture coordinates in texels (8.8 fixed point in the file). */
	FVector2f UV[4];

	bool IsTextured() const { return TextureHandle != INDEX_NONE; }
	bool IsTransparent() const { return Type == ELBAPolyType::Trans || Type == ELBAPolyType::Trame; }
	bool IsFlatShaded() const
	{
		return Type == ELBAPolyType::Solid || Type == ELBAPolyType::Flat || Type == ELBAPolyType::Trans
			|| Type == ELBAPolyType::Trame || Type == ELBAPolyType::TextureFlat || Type == ELBAPolyType::TextureIncrustFlat
			|| Type == ELBAPolyType::Texture || Type == ELBAPolyType::TextureIncrust;
	}
	/** Incrust textures use palette index 0 as a transparent color key. */
	bool UsesColorKey() const { return Type >= ELBAPolyType::TextureIncrust && Type <= ELBAPolyType::TextureIncrustDither; }
};

struct FLBABodyLine
{
	uint16 Type = 0;
	uint16 Color = 0;
	int32 Points[2] = { 0, 0 };
};

struct FLBABodySphere
{
	uint16 Type = 0;
	uint16 Color = 0;
	int32 Point = 0;
	int32 Radius = 0;
};

/** A texture region of the shared 256x256 texture page. Size is a power of two (mask + 1). */
struct FLBATextureRegion
{
	int32 X = 0;
	int32 Y = 0;
	int32 Width = 256;
	int32 Height = 256;

	bool IsFullPage() const { return X == 0 && Y == 0 && Width == 256 && Height == 256; }
	FString GetKey() const { return FString::Printf(TEXT("%d_%d_%dx%d"), X, Y, Width, Height); }
	bool operator==(const FLBATextureRegion& Other) const
	{
		return X == Other.X && Y == Other.Y && Width == Other.Width && Height == Other.Height;
	}
};

struct FLBABody
{
	int32 Info = 0;
	FIntVector BoundsMin = FIntVector::ZeroValue;
	FIntVector BoundsMax = FIntVector::ZeroValue;

	TArray<FLBABodyGroup> Groups;
	TArray<FLBABodyPoint> Points;
	TArray<FIntVector> Normals;
	TArray<FLBABodyPoly> Polys;
	TArray<FLBABodyLine> Lines;
	TArray<FLBABodySphere> Spheres;
	TArray<FLBATextureRegion> Textures;

	bool IsAnimated() const { return (Info & (1 << 8)) != 0; }
	bool HasTransparency() const { return (Info & (1 << 10)) != 0; }

	bool Parse(const TArray<uint8>& Data);

	/** Rest-pose position of every group origin, in LBA units (sum of pivots up the hierarchy). */
	TArray<FIntVector> ComputeGroupOrigins() const;

	/** Rest-pose (model space) position of a point, in LBA units. */
	FIntVector GetRestPosition(int32 PointIndex, const TArray<FIntVector>& GroupOrigins) const;
};
