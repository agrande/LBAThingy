#pragma once

#include "Engine/DeveloperSettings.h"
#include "Materials/MaterialInterface.h"
#include "LBAEnclosure.h"
#include "LBA2ImporterSettings.generated.h"

/** How faces that the isometric camera never showed (back and bottom of bricks) are dressed. */
UENUM()
enum class ELBABackFaceMode : uint8
{
	/** Leave them open (original look from the iso angle only). */
	None,
	/** Reuse the opposite face of the same brick. */
	SameBrick,
	/**
	 * Walls get the art of the opposite side of the wall (found by walking through its thickness);
	 * other bricks get a flat fill of their average color.
	 */
	Smart,
};

/** Look of generated levels. Both are in every level's post-process volume; lba.Look.Mode switches live. */
UENUM()
enum class ELBALookMode : uint8
{
	/** Close to the original's pixels: palette shade rows picked like its Gouraud shading, fine dither. */
	Original,
	/** Graded: deeper shadows, bolder dots, split toning and more color (MI_LBA_Look_Enhanced). */
	Enhanced,
};

UENUM()
enum class ELBAImportLayer : uint8
{
	Islands,
	Interiors,
	Characters,
	Objects,
	Scenes,
	Text,
	Audio,
};

/**
 * Per-user importer options: where the game data is on this machine and which subset to import while iterating.
 * Edited from the LBA2 Importer panel (Tools > LBA2 Importer) or Editor Preferences > Plugins > LBA2 Importer.
 * How things are imported is shared by the project: see ULBA2ImporterProjectSettings.
 */
UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "LBA2 Importer"))
class ULBA2ImporterSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/**
	 * Folder of an installed copy of Little Big Adventure 2 (Steam, GOG or CD). The folder that contains
	 * LBA_BKG.HQR is found automatically, e.g. the Steam "Common" subfolder.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Source")
	FDirectoryPath GameDirectory;

	/**
	 * Optional local checkout of https://github.com/LBALab/lba2remake (MIT). When set, its hand-made 3D
	 * models replace isometric layouts in interiors and are imported as reusable static meshes.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Source")
	FDirectoryPath RemakeDirectory;

	/** Islands to import (file names such as CITABAU). Empty imports all of them. */
	UPROPERTY(Config, EditAnywhere, Category = "Filters")
	TArray<FString> IslandFilter;

	/** Grid indices to import. Empty imports all of them. */
	UPROPERTY(Config, EditAnywhere, Category = "Filters")
	TArray<int32> GridFilter;

	/** Entity indices (RESS.HQR table) to import. Empty imports all characters. 0-4 are Twinsen's behaviours. */
	UPROPERTY(Config, EditAnywhere, Category = "Filters")
	TArray<int32> EntityFilter;

	/** Scene numbers (SCENE.HQR) to import. Empty imports all of them (demo copies only with Import Demo Scenes). */
	UPROPERTY(Config, EditAnywhere, Category = "Filters")
	TArray<int32> SceneFilter;

	/** Returns the folder holding the HQR files, or empty when nothing was found. */
	FString ResolveDataDirectory() const;

	/** Fills GameDirectory with a known Steam/GOG install when it is empty. */
	void AutoDetectGameDirectory();

	/** Fills RemakeDirectory with an lba2remake checkout next to the project when it is empty. */
	void AutoDetectRemakeDirectory();

	/** True when Directory is an lba2remake checkout with the models and metadata the importer uses. */
	static bool IsRemakeCheckout(const FString& Directory);

	static FString FindDataDirectoryIn(const FString& Directory);
};

/** Enclosure of the interiors built from one layout library (each grid uses one library). */
USTRUCT()
struct FLBALibraryEnclosure
{
	GENERATED_BODY()

	/** Layout library index in LBA_BKG.HQR. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0"))
	int32 Library = 0;

	/** What the library builds, to find your way in this list. */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	FString Theme;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ShowOnlyInnerProperties))
	FLBAEnclosureSettings Enclosure;

	/**
	 * Layouts that frame doorways (jambs, arches, posts), at least 3 cells tall. An opening in a wall with one of
	 * them at its side is a doorway even without a lintel.
	 */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	TArray<int32> DoorwayJambs;

	/** Flat layouts lying in doorways (doormats, sills). An opening whose floor is one of them is a doorway. */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	TArray<int32> DoorwayThresholds;

	/**
	 * Layouts that frame the walls the original cut down for the camera (caps along their top, curves down to the
	 * doorways). Where the enclosure raises those walls, they come off and the wall goes on.
	 */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	TArray<int32> CutFrames;

	/** Color of the ceilings (and of the risers where they step). */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	FColor CeilingColor = FColor(150, 140, 125);

	/** Material of the ceilings instead of CeilingColor (UVs tile once per cell). */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	TSoftObjectPtr<UMaterialInterface> CeilingMaterial;

	/**
	 * Wall styles (DA_LibNN_WallStyles) are found in the library's grids: layouts listed here never make one (windows,
	 * posters, a wall used only once in a special place).
	 */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	TArray<int32> IgnoredWallLayouts;

	/**
	 * Layout of the wall style that dresses the inner sides of the walls facing the camera (which the original never
	 * drew). -1: the most used style of each room's back walls.
	 */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "-1"))
	int32 IndoorWallLayout = INDEX_NONE;

	/** Raised walls end in their style's top (caps, cornices) under the ceiling. */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	bool bWallTops = true;
};

/** Enclosure changes for one grid, over its library's. */
USTRUCT()
struct FLBAGridEnclosure
{
	GENERATED_BODY()

	/** Grid index in LBA_BKG.HQR (the NNN of L_Grid_NNN). */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0"))
	int32 Grid = 0;

	/** Why this grid differs, to find your way in this list. */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	FString Note;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ShowOnlyInnerProperties))
	FLBAEnclosureOverrides Overrides;
};

/**
 * How the project imports LBA2, shared by everyone working on it and kept across imports: Config/DefaultLBA2Importer.ini,
 * edited in Project Settings > Plugins > LBA2 Importer or the LBA2 Importer panel.
 */
UCLASS(Config = LBA2Importer, DefaultConfig, meta = (DisplayName = "LBA2 Importer"))
class ULBA2ImporterProjectSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULBA2ImporterProjectSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual void PostInitProperties() override;

	/** Content folder that receives every imported asset. */
	UPROPERTY(Config, EditAnywhere, Category = "Destination", meta = (ContentDir))
	FDirectoryPath ContentRoot;

	/** Generated levels override their game mode with LBAGameMode, so Play spawns Twinsen. */
	UPROPERTY(Config, EditAnywhere, Category = "Destination")
	bool bPlayAsTwinsen = true;

	/** Point sampling keeps the crisp look of the original 256x256 textures. */
	UPROPERTY(Config, EditAnywhere, Category = "Look")
	bool bPixelArtFiltering = true;

	/** Look generated levels start with. */
	UPROPERTY(Config, EditAnywhere, Category = "Look")
	ELBALookMode LookMode = ELBALookMode::Enhanced;

	/**
	 * Island fog distances relative to the original's (which stops drawing a few dozen meters away). 1 is the
	 * original's view distance; larger values let you see further while keeping its fog color.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Look", meta = (ClampMin = "0.25", UIMax = "8"))
	float FogDistanceScale = 1.5f;

	/** Also create a playable level per island. */
	UPROPERTY(Config, EditAnywhere, Category = "Islands")
	bool bCreateIslandLevels = true;

	/** Also create a playable level per interior grid. */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	bool bCreateInteriorLevels = true;

	/** How to dress brick faces the original isometric camera never showed. */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	ELBABackFaceMode BackFaces = ELBABackFaceMode::Smart;

	/** Create a reusable static mesh for every library layout (kit pieces for building new interiors). */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	bool bCreateLayoutKitMeshes = true;

	/** Use the lba2remake models (Remake Directory in the per-user settings) in interiors. */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	bool bUseRemakeModels = true;

	/** Enclosure of libraries that are not listed in Library Enclosures. */
	UPROPERTY(Config, EditAnywhere, Category = "Enclosure")
	FLBAEnclosureSettings DefaultEnclosure;

	/**
	 * Per layout library. Room heights default to the most common height of the back walls the original drew in
	 * full (the walls facing the camera are cut down to let it see in).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Enclosure", meta = (TitleProperty = "{Library}: {Theme}"))
	TArray<FLBALibraryEnclosure> LibraryEnclosures;

	/** Per grid, over its library's settings. */
	UPROPERTY(Config, EditAnywhere, Category = "Enclosure", meta = (TitleProperty = "{Grid}: {Note}"))
	TArray<FLBAGridEnclosure> GridEnclosures;

	/** Also import the demo (attract mode) copies of scenes, 193-221. They are streamed unloaded. */
	UPROPERTY(Config, EditAnywhere, Category = "Scenes")
	bool bImportDemoScenes = false;

	/** Text language (EN, FR, DE, SP, IT). */
	UPROPERTY(Config, EditAnywhere, Category = "Text and Audio")
	FString Language = TEXT("EN");

	/** Returns the destination content path, e.g. /Game/LBA2. */
	FString GetContentRoot() const;

	/** Enclosure of a grid: its library's settings (or the default), then the grid's overrides. */
	FLBAEnclosureSettings GetEnclosure(int32 Library, int32 Grid) const;

	/** The library's entry in Library Enclosures, or null. */
	const FLBALibraryEnclosure* FindLibraryEnclosure(int32 Library) const;
};
