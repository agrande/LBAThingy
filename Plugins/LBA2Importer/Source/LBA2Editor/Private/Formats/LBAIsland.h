#pragma once

#include "CoreMinimal.h"

class FLBAHqrFile;

/**
 * LBA2 exterior islands (*.ILE + *.OBL). See SOURCES/3DEXT/LOADISLE.CPP and LBA_EXT.H.
 *
 * An island is a 16x16 map of "cubes" (sections). Each cube is 64x64 ground cells of 512 units
 * (0x8000 units per cube), with 65x65 height and light samples, two triangles per cell and a list of
 * decor objects whose meshes live in the matching .OBL file.
 */
struct FLBAIslandDecor
{
	int32 Body = 0;
	FIntVector Position = FIntVector::ZeroValue;
	int32 CodeJeu = 0;
	int32 Beta = 0;
	/** Upper bits of Beta: optional game variable gating the object's presence. */
	int32 ConditionVar = 0;
	int32 ConditionValue = 0;
	FIntVector BoundsMin = FIntVector::ZeroValue;
	FIntVector BoundsMax = FIntVector::ZeroValue;
};

/** Decoded T_HALF_POLY bitfield (one per ground triangle). */
struct FLBAGroundTriangle
{
	uint32 Raw = 0;

	int32 Bank() const { return Raw & 0xF; }
	int32 TexFlag() const { return (Raw >> 4) & 0x3; }
	int32 PolyFlag() const { return (Raw >> 6) & 0x3; }
	int32 SampleStep() const { return (Raw >> 8) & 0xF; }
	int32 CodeJeu() const { return (Raw >> 12) & 0xF; }
	int32 Sens() const { return (Raw >> 16) & 0x1; }
	int32 Collision() const { return (Raw >> 17) & 0x1; }
	int32 TexIndex() const { return int32(Raw >> 19); }

	bool IsTextured() const { return TexFlag() != 0; }
	bool IsColored() const { return PolyFlag() != 0; }
	bool IsVisible() const { return IsTextured() || IsColored(); }
};

struct FLBAIslandSection
{
	/** Position in the 16x16 island map (X maps to LBA +X, Z to LBA +Z). */
	int32 CubeX = 0;
	int32 CubeZ = 0;
	/** 1-based cube id as stored in the map. */
	int32 Id = 0;
	/** Order in which sections appear when scanning the map (used by scene mappings). */
	int32 Order = 0;

	int32 LightAlpha = 0;
	int32 LightBeta = 0;
	int32 SkyY = 0;
	int32 FogStart = 0;
	int32 ClipFar = 0;

	TArray<FLBAIslandDecor> Decors;
	/** 64*64*2 triangles, indexed [(z * 64 + x) * 2 + n]. */
	TArray<FLBAGroundTriangle> Triangles;
	/** Texture coordinates per textured triangle (3 points, texels). */
	TArray<FVector2f> TexCoords;
	/** 65*65 heights indexed [z * 65 + x]. */
	TArray<int16> Heights;
	/** 65*65 light intensities indexed [z * 65 + x]. */
	TArray<uint8> Intensities;

	int32 GetHeight(int32 X, int32 Z) const { return Heights.IsValidIndex(Z * 65 + X) ? Heights[Z * 65 + X] : 0; }
	int32 GetIntensity(int32 X, int32 Z) const { return Intensities.IsValidIndex(Z * 65 + X) ? Intensities[Z * 65 + X] : 0; }
	FIntVector GetOrigin() const;
};

struct FLBAIsland
{
	FString Name;
	TArray<uint8> Map;
	TArray<uint8> GroundTexture;
	TArray<uint8> ObjectTexture;
	TArray<FLBAIslandSection> Sections;

	bool Load(const FLBAHqrFile& Ile);
	const FLBAIslandSection* FindSection(int32 CubeX, int32 CubeZ) const;

	/**
	 * Corner points (cell-local x/z offsets, 0 or 1) of the two triangles of a ground cell.
	 * Sens picks the diagonal: 0 = (0,0)-(1,1), 1 = (1,0)-(0,1).
	 */
	static void GetTriangleCorners(int32 Sens, int32 TriangleIndex, FIntPoint OutCorners[3]);
};
