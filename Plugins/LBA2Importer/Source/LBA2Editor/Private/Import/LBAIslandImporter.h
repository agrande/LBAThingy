#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
struct FLBAIsland;
struct FLBAIslandSection;
class UStaticMesh;
class UTexture2D;
class UMaterialInterface;

/** The light of glowing decor texels (FLBAGlowTexels), in mesh space. */
struct FLBADecorGlowLight
{
	int32 Glow = INDEX_NONE;
	FVector3f Center = FVector3f::ZeroVector;
	/** Windows: the direction they face and their size (width, height). Lamps: no normal, the source radius in Size.X. */
	FVector3f Normal = FVector3f::ZeroVector;
	FVector2f Size = FVector2f::ZeroVector;
	FLinearColor Color = FLinearColor::White;
};

/** What glows on the decor meshes, by body. */
struct FLBADecorGlow
{
	/** Bounds of the glowing parts of lit decor (FLBADecorLight). */
	TMap<int32, FBox3f> LampBounds;
	/** Lights of glowing texels (FLBAGlowTexels). */
	TMap<int32, TArray<FLBADecorGlowLight>> TexelLights;
};

/**
 * Islands (*.ILE / *.OBL) -> textures, decor static meshes, one terrain static mesh per section and a
 * playable level with everything placed, a sea plane, lighting and a player start.
 *
 * Output layout under <Root>/Islands/<ISLAND>/:
 *   Textures/   T_<ISLAND>_Ground, T_<ISLAND>_Obj_<x>_<y>_<w>x<h>
 *   Materials/  material instances
 *   Decor/      SM_<ISLAND>_Decor_<NNN> (one per .OBL entry, reusable kit pieces)
 *   Ground/     SM_<ISLAND>_Ground_<X>_<Z>
 *   L_<ISLAND>  level
 */
class FLBAIslandImporter
{
public:
	explicit FLBAIslandImporter(FLBAImportContext& InContext);

	/** Lists the island names (ILE files) found in the game folder. */
	TArray<FString> FindIslands() const;

	int32 ImportAll();
	bool ImportIsland(const FString& IslandName);

private:
	UStaticMesh* BuildGroundMesh(const FLBAIsland& Island, const FLBAIslandSection& Section, const FString& Folder,
		UMaterialInterface* GroundMaterial, UMaterialInterface* ColorMaterial);
	/** Also returns what glows on them (lit decor, glowing texels). */
	TArray<UStaticMesh*> BuildDecorMeshes(const FLBAIsland& Island, const FString& IslandFolder, FLBADecorGlow& OutGlow);
	/**
	 * Decor bodies shaped like an entity's body 0: the island's own copies of it. The boat's (Project Settings > LBA2 >
	 * Mounts > Boat, the ferry) are moored boats instead; the car's (Mounts > Car, the buggy) parked cars.
	 */
	TSet<int32> FindBodiesLike(const FLBAIsland& Island, const class ULBAEntityAsset* Entity, const TCHAR* What) const;
	void BuildLevel(const FLBAIsland& Island, const FString& IslandFolder, const TArray<UStaticMesh*>& GroundMeshes,
		const TArray<UStaticMesh*>& DecorMeshes, const FLBADecorGlow& Glow, const TSet<int32>& BoatBodies, const TSet<int32>& CarBodies);

	FLBAImportContext& Context;
};
