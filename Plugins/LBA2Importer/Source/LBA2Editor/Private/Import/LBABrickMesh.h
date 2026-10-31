#pragma once

#include "CoreMinimal.h"
#include "Import/LBAIsoImporter.h"
#include "LBA2ImporterSettings.h"

class FLBAMeshBuilder;
struct FLBABrickSprite;

/**
 * The isometric interiors' bricks as cubes (FLBAIsoImporter), for other grids of bricks: LBA1's towns on the globe's stub
 * islands (LBAPlanetTowns). Every brick is a cube whose top, +X and +Z faces are UV-mapped onto its sprite, the faces the
 * iso camera never saw mirroring the opposite ones'; collision follows the bricks' shapes (full, ramps, half ramps).
 */
namespace LBABrickMesh
{
	/** Read access to a block of bricks: each cell's brick (0 = none) and collision shape. */
	struct FCells
	{
		FIntVector Size;
		TFunction<int32(int32 X, int32 Y, int32 Z)> Brick;
		TFunction<int32(int32 X, int32 Y, int32 Z)> Shape;

		bool Inside(int32 X, int32 Y, int32 Z) const
		{
			return X >= 0 && Y >= 0 && Z >= 0 && X < Size.X && Y < Size.Y && Z < Size.Z;
		}
		int32 BrickAt(int32 X, int32 Y, int32 Z) const { return Inside(X, Y, Z) ? Brick(X, Y, Z) : 0; }
		int32 ShapeAt(int32 X, int32 Y, int32 Z) const { return Inside(X, Y, Z) ? Shape(X, Y, Z) : 0; }
	};

	/**
	 * An atlas of bricks (1-based, sorted): their sprites' faces apart in cells, their faces' mean colors and coverage.
	 * Decode fills a brick's sprite (false: it keeps its cell, empty). Texture and materials are left to the caller.
	 */
	void MakeAtlas(TConstArrayView<int32> Bricks, TFunctionRef<bool(int32 Brick, FLBABrickSprite& Sprite)> Decode, const TArray<FColor>& Palette,
		FLBAIsoImporter::FAtlas& Atlas, TArray<FColor>& Pixels);

	/** The cubes' visible faces in Section (flat filled backs in a "BrickFill" section); KeepFace picks faces (cell, face 0..5). */
	void EmitVisual(const FCells& Cells, const FLBAIsoImporter::FAtlas& Atlas, FLBAMeshBuilder& Builder, int32 Section, float Scale,
		ELBABackFaceMode BackFaces, TFunctionRef<bool(const FIntVector&, int32)> KeepFace);

	/** The cells' collision shapes as triangles (complex collision). */
	void EmitCollision(const FCells& Cells, FLBAMeshBuilder& Builder, int32 Section, float Scale, TFunctionRef<bool(const FIntVector&)> KeepCell);
}
