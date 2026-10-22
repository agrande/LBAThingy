#pragma once

#include "CoreMinimal.h"
#include "Formats/LBAIso.h"
#include "Import/LBARoomAnalysis.h"
#include "LBAWallStyles.h"

class FLBAImportContext;
class FLBAMeshBuilder;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;

/**
 * Isometric interiors (LBA_BKG.HQR).
 *
 * The original interiors are pre-rendered 48x38 isometric brick sprites stacked on a 64x25x64 grid.
 * There is no 3D source, so every brick becomes a cube whose visible faces (top, +X, +Z) are UV-mapped
 * straight onto the sprite (pixel exact from the original camera angle). Faces the iso camera never
 * sees reuse the opposite face's art. Collision comes from the per-brick shapes (full blocks, ramps,
 * diagonal half ramps) exactly as the original engine resolves them.
 *
 * Output layout under <Root>/Interiors/:
 *   Libraries/Lib_NN/   T_LibNN_Bricks atlas, material, SM_LibNN_Layout_MMM kit pieces, DA_LibNN_WallStyles
 *   Grids/              SM_Grid_NNN (+ _Collision), L_Grid_NNN_<SceneName> levels
 */
class FLBAIsoImporter
{
public:
	explicit FLBAIsoImporter(FLBAImportContext& InContext);
	~FLBAIsoImporter();

	int32 ImportAll();

	/** Brick atlas of one library: texture plus the atlas cell of every used brick. */
	struct FAtlas
	{
		UTexture2D* Texture = nullptr;
		UMaterialInterface* Material = nullptr;
		TMap<int32, FIntPoint> Cells;
		/** Average color of the top, +X and +Z faces of every brick (used to fill hidden faces). */
		TMap<int32, TStaticArray<FColor, 3>> FaceColors;
		/** Fraction (0..1) of opaque pixels on the top, +X and +Z faces of every brick. */
		TMap<int32, TStaticArray<float, 3>> FaceCoverage;
		int32 Width = 0;
		int32 Height = 0;
		UMaterialInterface* FillMaterial = nullptr;
	};

private:
	bool LoadHeader();
	/** Cube-change zones (doors) of the interior scenes, per grid. */
	void LoadDoorZones();
	void LogRooms(int32 GridIndex, const FLBARoomAnalysis& Rooms, const TArray<FLBADoorway>& Doorways, const TArray<FLBACellBox>& Zones);
	const FLBALibrary* GetLibrary(int32 LibraryIndex);
	const FAtlas* GetAtlas(int32 LibraryIndex);
	void BuildKitMeshes(int32 LibraryIndex);
	/** The wall styles of a library, found in all its grids (DA_LibNN_WallStyles). */
	const TArray<FLBAWallStyle>& GetWallStyles(int32 LibraryIndex);
	/** Doorways of a grid, with its library's doorway layouts and its scenes' cube-change zones as hints. */
	TArray<FLBADoorway> FindDoorways(int32 GridIndex, int32 LibraryIndex, const FLBARoomAnalysis& Rooms) const;
	/** Bricks of a library's walls, learned from the rooms of all its grids (FLBARoomModel). */
	const TSet<int32>& GetWallBricks(int32 LibraryIndex);
	bool ImportGrid(int32 GridIndex);
	FString GetGridLevelName(int32 GridIndex) const;

	FLBAImportContext& Context;
	const FLBAHqrFile* Bkg = nullptr;
	FLBABkgHeader Header;
	TArray<FLBASceneMapEntry> SceneMap;
	TMap<int32, TArray<FLBACellBox>> DoorZones;
	TMap<int32, TUniquePtr<FLBALibrary>> Libraries;
	TMap<int32, TUniquePtr<FAtlas>> Atlases;
	TMap<int32, TArray<FLBAWallStyle>> WallStyles;
	TMap<int32, TSet<int32>> WallBricks;
	TUniquePtr<class FLBARemakeAssets> Remake;
};
