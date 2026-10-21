#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FLBAImportContext;
class UStaticMesh;
struct FLBAGrid;
struct FLBALibrary;

/**
 * Bridge to the 3D replacement models authored by the lba2remake community (MIT): hand-made .glb
 * models replacing isometric layouts, extra props per grid and brick patches. Requires a local
 * checkout of https://github.com/LBALab/lba2remake (www/models and www/metadata are used).
 *
 * Replacement GLBs become regular static mesh assets under <Root>/Remake/ so they can also be used
 * as kit pieces. Placement follows the remake exactly (see metadata/replacements.ts there).
 */
class FLBARemakeAssets
{
public:
	explicit FLBARemakeAssets(FLBAImportContext& InContext);

	/** Loads the remake metadata. Returns false (and stays inert) when no checkout is configured. */
	bool Initialize();
	bool IsAvailable() const { return bAvailable; }

	/**
	 * A lamp of a model, in mesh space: its bulb (glTF node named "...bulb...", or a "keepMat_light..." material) or,
	 * without one, its lampshade ("LampTexture..." material). Both glow; the lamp's light sits at the bulb's center.
	 */
	struct FLamp
	{
		FVector Position = FVector::ZeroVector;
		FLinearColor Color = FLinearColor::White;
		float SourceRadius = 0.f;
	};

	struct FPlacement
	{
		UStaticMesh* Mesh = nullptr;
		FTransform Transform;
		FString Label;
		TArray<FLamp> Lamps;
	};

	struct FGridResult
	{
		/** Grid cells whose brick graphics are replaced (collision is kept). */
		TSet<FIntVector> HiddenCells;
		/** Cells whose content is replaced by a different layout block: (layout, block). */
		TMap<FIntVector, FIntPoint> PatchedCells;
		TArray<FPlacement> Placements;
	};

	/**
	 * Applies grids.json patches to the grid, matches layout replacements and returns what to hide and
	 * which models to place (Unreal space, same origin as the generated grid mesh).
	 */
	FGridResult ProcessGrid(FLBAGrid& Grid, const FLBALibrary& Library);

	/** Static mesh for a model of www/models/lba2/<SubFolder>/<File>, imported on first use. */
	UStaticMesh* GetModelMesh(const FString& SubFolder, const FString& File);
	/** Lamps of a model imported with GetModelMesh. */
	TArray<FLamp> GetModelLamps(const FString& SubFolder, const FString& File) const { return LampCache.FindRef(SubFolder / File); }

private:
	FLBAImportContext& Context;
	bool bAvailable = false;
	FString ModelsDirectory;
	TSharedPtr<FJsonObject> LayoutsMetadata;
	TSharedPtr<FJsonObject> GridsMetadata;
	TMap<FString, UStaticMesh*> MeshCache;
	TMap<FString, TArray<FLamp>> LampCache;
};
