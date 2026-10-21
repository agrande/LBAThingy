#pragma once

#include "Engine/DeveloperSettings.h"
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
 * Per-user importer options. Edited from the LBA2 Importer panel (Tools > LBA2 Importer) or
 * Editor Preferences > Plugins > LBA2 Importer.
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

	/** Islands to import (file names such as CITABAU). Empty imports all of them. */
	UPROPERTY(Config, EditAnywhere, Category = "Islands")
	TArray<FString> IslandFilter;

	/** Also create a playable level per island. */
	UPROPERTY(Config, EditAnywhere, Category = "Islands")
	bool bCreateIslandLevels = true;

	/** Grid indices to import. Empty imports all of them. */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	TArray<int32> GridFilter;

	/** Also create a playable level per interior grid. */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	bool bCreateInteriorLevels = true;

	/** How to dress brick faces the original isometric camera never showed. */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	ELBABackFaceMode BackFaces = ELBABackFaceMode::Smart;

	/** Create a reusable static mesh for every library layout (kit pieces for building new interiors). */
	UPROPERTY(Config, EditAnywhere, Category = "Interiors")
	bool bCreateLayoutKitMeshes = true;

	/** Entity indices (RESS.HQR table) to import. Empty imports all characters. 0-4 are Twinsen's behaviours. */
	UPROPERTY(Config, EditAnywhere, Category = "Characters")
	TArray<int32> EntityFilter;

	/** Scene numbers (SCENE.HQR) to import. Empty imports all of them (demo copies only with Import Demo Scenes). */
	UPROPERTY(Config, EditAnywhere, Category = "Scenes")
	TArray<int32> SceneFilter;

	/** Also import the demo (attract mode) copies of scenes, 193-221. They are streamed unloaded. */
	UPROPERTY(Config, EditAnywhere, Category = "Scenes")
	bool bImportDemoScenes = false;

	/**
	 * Optional local checkout of https://github.com/LBALab/lba2remake (MIT). When set, its hand-made 3D
	 * models replace isometric layouts in interiors and are imported as reusable static meshes.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Community 3D Models")
	FDirectoryPath RemakeDirectory;

	UPROPERTY(Config, EditAnywhere, Category = "Community 3D Models")
	bool bUseRemakeModels = true;

	/** Text language (EN, FR, DE, SP, IT). */
	UPROPERTY(Config, EditAnywhere, Category = "Text and Audio")
	FString Language = TEXT("EN");

	/** Returns the folder holding the HQR files, or empty when nothing was found. */
	FString ResolveDataDirectory() const;

	/** Returns the destination content path, e.g. /Game/LBA2. */
	FString GetContentRoot() const;

	/** Fills GameDirectory with a known Steam/GOG install when it is empty. */
	void AutoDetectGameDirectory();

	/** Fills RemakeDirectory with an lba2remake checkout next to the project when it is empty. */
	void AutoDetectRemakeDirectory();

	/** True when Directory is an lba2remake checkout with the models and metadata the importer uses. */
	static bool IsRemakeCheckout(const FString& Directory);

	static FString FindDataDirectoryIn(const FString& Directory);
};
