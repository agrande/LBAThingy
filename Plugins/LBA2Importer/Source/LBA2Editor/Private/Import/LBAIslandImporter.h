#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
struct FLBAIsland;
struct FLBAIslandSection;
class UStaticMesh;
class UTexture2D;
class UMaterialInterface;

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
	/** Also returns the bounds of the glowing parts of lit decor (FLBADecorLight), by body, in mesh space. */
	TArray<UStaticMesh*> BuildDecorMeshes(const FLBAIsland& Island, const FString& IslandFolder, TMap<int32, FBox3f>& OutGlowBounds);
	void BuildLevel(const FLBAIsland& Island, const FString& IslandFolder, const TArray<UStaticMesh*>& GroundMeshes,
		const TArray<UStaticMesh*>& DecorMeshes, const TMap<int32, FBox3f>& GlowBounds);

	FLBAImportContext& Context;
};
