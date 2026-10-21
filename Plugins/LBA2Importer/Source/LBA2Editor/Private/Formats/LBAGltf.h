#pragma once

#include "CoreMinimal.h"

/**
 * Minimal binary glTF (.glb) reader for the lba2remake replacement models: static triangle meshes,
 * node transforms baked into scene space, base color factors/textures and alpha modes. Animations,
 * morph targets and skins are ignored (rest pose).
 */
struct FLBAGltfImage
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<FColor> Pixels;
	/** Hash of the encoded image, used to share textures between models. */
	FString Hash;
};

struct FLBAGltfMaterial
{
	enum class EAlpha : uint8 { Opaque, Mask, Blend };

	FString Name;
	FLinearColor BaseColor = FLinearColor::White;
	FLinearColor Emissive = FLinearColor::Black;
	int32 Image = INDEX_NONE;
	EAlpha AlphaMode = EAlpha::Opaque;
	bool bUnlit = false;
};

struct FLBAGltfPrimitive
{
	/** Name of the node holding the mesh. */
	FString Node;
	int32 Material = INDEX_NONE;
	/** Scene-space data (glTF axes: right-handed, Y up). */
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UVs;
	TArray<FVector4f> Colors;
	TArray<uint32> Indices;
};

struct FLBAGltfModel
{
	TArray<FLBAGltfPrimitive> Primitives;
	TArray<FLBAGltfMaterial> Materials;
	TArray<FLBAGltfImage> Images;

	bool Load(const FString& Path, FString& OutError);
};
