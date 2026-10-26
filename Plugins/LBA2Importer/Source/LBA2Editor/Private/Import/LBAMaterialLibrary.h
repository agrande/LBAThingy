#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UMaterial;
class UMaterialExpression;
class UMaterialInterface;
class UMaterialInstanceConstant;
class UMaterialParameterCollection;
class UTexture2D;

/**
 * Master materials shared by every imported asset, generated into <Root>/Materials on first use.
 *
 * Vertex color carries the LBA palette color (albedo) in RGB and the original baked light intensity in
 * alpha (0.5 = neutral). The "BakedShading" parameter blends between fully modern lighting (0) and the
 * original hand-tuned shading multiplied in (1).
 */
enum class ELBAMaterial : uint8
{
	/** Opaque, vertex colored (flat/gouraud polygons). */
	Color,
	/** Translucent vertex color (LBA "trans" polygons, glass). */
	Translucent,
	/** Opaque texture * vertex color. */
	Textured,
	/** Masked texture (palette index 0 is transparent) * vertex color. */
	TexturedMasked,
	/** Island ground: vertex color where the texture is transparent, texture elsewhere. */
	Ground,
	/** Isometric brick sprites projected on cubes. */
	Brick,
	/** Sea: the island's sea texture tiled in world space (opaque, like the original). */
	Water,
	/** Invisible helper (collision-only geometry). */
	Invisible,
	/** Translucent texture * vertex color (remake glass, water...). */
	TexturedTranslucent,
	/** Opaque vertex color that also glows (lit lamp glass): emissive = albedo * GlowStrength. */
	Glow,
	/** Opaque texture * vertex color whose texels with alpha glow (lit windows): emissive = texel * alpha * GlowStrength. */
	TexturedGlow,
	Count
};

/**
 * Generated materials carry a version stamp (a comment node) so a newer importer can rebuild their graph in place.
 * Materials stamped with the current version are left alone, which keeps user edits between imports.
 */
namespace LBAMaterials
{
	bool IsGeneratedVersion(const UMaterial* Material, int32 Version);
	/** Clears the graph and stamps the version. */
	void BeginRebuild(UMaterial* Material, int32 Version);
	/** Adds the version stamp to a graph built some other way. */
	void AddVersionStamp(UMaterial* Material, int32 Version);
	/** Sets usages, recompiles and marks the material for saving. */
	UMaterial* EndRebuild(FLBAImportContext& Context, UMaterial* Material);
	/**
	 * Bends the material's surface with globe mode's view (world position offset, LBAPlanetBendOffset in
	 * Shaders/Private/LBAPlanet.ush), last frame's camera for motion vectors. Nothing moves without a planet.
	 */
	void AddPlanetBend(UMaterial* Material, UMaterialParameterCollection* Planet);
	/** The bend's world position offset (current and last frame), for materials that add it to their own. */
	UMaterialExpression* MakePlanetBend(UMaterial* Material, UMaterialParameterCollection* Planet);
	/**
	 * A normal (world space, or tangent space where the tangent basis is the world's, like the Water plugin's meshes)
	 * output of Normal turned with the bend's tilt at the pixel (LBAPlanetBendVector).
	 */
	UMaterialExpression* MakePlanetBentNormal(UMaterial* Material, UMaterialParameterCollection* Planet, UMaterialExpression* Normal, int32 OutputIndex);
}

class FLBAMaterialLibrary
{
public:
	explicit FLBAMaterialLibrary(FLBAImportContext& InContext);

	UMaterial* GetMaster(ELBAMaterial Kind);

	/** Material instance of a textured master for a given texture (cached per texture and kind). */
	UMaterialInterface* GetTextured(ELBAMaterial Kind, UTexture2D* Texture, const FString& Folder);

	static FName GetSlotName(ELBAMaterial Kind);

private:
	UMaterial* CreateMaster(ELBAMaterial Kind);

	FLBAImportContext& Context;
	UMaterial* Masters[uint8(ELBAMaterial::Count)] = {};
	TMap<TPair<uint8, UTexture2D*>, UMaterialInterface*> Instances;
};
