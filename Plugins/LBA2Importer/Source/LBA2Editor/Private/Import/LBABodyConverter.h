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
	/**
	 * Texels of the texture page (256 x 256, row-major) that glow: the index of their glow, INDEX_NONE for none, and
	 * by glow the rectangle of the page it covers. Textured polygons whose texture region holds a whole glow's
	 * rectangle and that show its texels (not a sliver of a window bleeding into a neighbor texture) use
	 * GetGlowMaterial instead (color-keyed ones excepted) and are reported as glow patches. An empty rectangle turns
	 * its glow off.
	 */
	const TArray<int8>* TexelGlow = nullptr;
	TArray<FIntRect> GlowRects;
	TFunction<UMaterialInterface*(const FLBATextureRegion& Region)> GetGlowMaterial;
	/** Keep the per-vertex bone index (skeletal meshes). */
	bool bSkinned = false;
	/** Body index, only used for logs. */
	int32 DebugIndex = INDEX_NONE;
};

/** The glowing part of one textured polygon (see FLBABodyConvertParams::TexelGlow), in mesh space. */
struct FLBAGlowPatch
{
	/** The glow most of its glowing texels belong to. */
	int32 Glow = INDEX_NONE;
	/** Bounds of the glowing part, its area (cm2), and the polygon's front normal. */
	FBox3f Bounds = FBox3f(ForceInit);
	float Area = 0.f;
	FVector3f Normal = FVector3f::ZeroVector;
};

namespace LBABodyConverter
{
	/**
	 * Turns an LBA body into triangles (rest pose, Unreal space). Returns false when nothing was produced.
	 * OutGlowBounds receives the bounds of the glowing triangles (see GlowColors), OutGlowPatches the polygons that
	 * show glowing texels (see TexelGlow).
	 */
	bool Convert(const FLBABody& Body, const FLBABodyConvertParams& Params, FLBAMeshBuilder& Out, FBox3f* OutGlowBounds = nullptr,
		TArray<FLBAGlowPatch>* OutGlowPatches = nullptr);

	/**
	 * Albedo for an LBA polygon color. Solid/transparent polygons use the exact palette entry; shaded
	 * polygons pick a lit point on their 16-color ramp since the engine adds light to the base index.
	 */
	FColor GetAlbedo(const TArray<FColor>& Palette, uint16 Color, uint8 PolyType);

	FVector4f ToVertexColor(const FColor& Color, float BakedLight = 0.5f);
}
