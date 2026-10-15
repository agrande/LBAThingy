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

	struct FPlacement
	{
		UStaticMesh* Mesh = nullptr;
		FTransform Transform;
		FString Label;
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

private:
	FLBAImportContext& Context;
	bool bAvailable = false;
	FString ModelsDirectory;
	TSharedPtr<FJsonObject> LayoutsMetadata;
	TSharedPtr<FJsonObject> GridsMetadata;
	TMap<FString, UStaticMesh*> MeshCache;
};
