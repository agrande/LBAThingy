#pragma once

#include "CoreMinimal.h"
#include "Formats/LBABody.h"

class FLBAMeshBuilder;
class UMaterialInterface;

struct FLBABodyConvertParams
{
	float Scale = 0.125f;
	const TArray<FColor>* Palette = nullptr;
	UMaterialInterface* ColorMaterial = nullptr;
	UMaterialInterface* TranslucentMaterial = nullptr;
	/** Untextured polygons and spheres of these palette colors glow (lamp glass), with GlowMaterial. */
	TArray<int32> GlowColors;
	UMaterialInterface* GlowMaterial = nullptr;
	/** Returns the material for one texture region of the object's texture page. */
	TFunction<UMaterialInterface*(const FLBATextureRegion& Region, bool bMasked)> GetTexturedMaterial;
	/** Keep the per-vertex bone index (skeletal meshes). */
	bool bSkinned = false;
	/** Body index, only used for logs. */
	int32 DebugIndex = INDEX_NONE;
};

namespace LBABodyConverter
{
	/**
	 * Turns an LBA body into triangles (rest pose, Unreal space). Returns false when nothing was produced.
	 * OutGlowBounds receives the bounds of the glowing triangles (see GlowColors).
	 */
	bool Convert(const FLBABody& Body, const FLBABodyConvertParams& Params, FLBAMeshBuilder& Out, FBox3f* OutGlowBounds = nullptr);

	/**
	 * Albedo for an LBA polygon color. Solid/transparent polygons use the exact palette entry; shaded
	 * polygons pick a lit point on their 16-color ramp since the engine adds light to the base index.
	 */
	FColor GetAlbedo(const TArray<FColor>& Palette, uint16 Color, uint8 PolyType);

	FVector4f ToVertexColor(const FColor& Color, float BakedLight = 0.5f);
}
