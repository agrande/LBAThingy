#pragma once

#include "CoreMinimal.h"
#include "Formats/LBAIso.h"
#include "Formats/LBAScene.h"
#include "Formats/LBASprite.h"

class FLBAAudioImporter;
class FLBAImportContext;
class FLBARemakeAssets;
class UStaticMesh;
class ULBAEntityAsset;
class ULBASceneAsset;
class UWorld;

/**
 * Scenes (SCENE.HQR): what lives in the islands and interiors. Each scene becomes
 *   - a scene asset (ULBASceneAsset: ambience, hero start, objects with their scripts, zones, track points),
 *   - a level holding its actors: ALBASceneActor per 3D object, ALBASceneSprite per sprite object, a typed zone volume
 *     per zone, an ATargetPoint per track point and a tagged APlayerStart at the hero start,
 * and the level is streamed into its scenery level: every cube of an island into the island level (always loaded,
 * so the whole island lives at once), an interior into its grid level.
 *
 * Sprites (SPRITES.HQR, SPRIRAW.HQR, ANIM3DS.HQR) become one atlas and a static mesh per sprite, the sprite projected
 * onto the visible faces of its box like the interior bricks, or replaced by its lba2remake model when there is one
 * (model_replacements.json). Sprites whose track slides them open (OPEN_LEFT...) become doors (ALBASceneDoor).
 *
 * Output under <Root>/Scenes: DA_Scene_NNN_<Name>, L_Scene_NNN_<Name>; <Root>/Sprites: T_Sprites, SM_Sprite*_NNN.
 * Run after Islands, Interiors and Characters (it uses their levels and entities).
 */
class FLBASceneImporter
{
public:
	explicit FLBASceneImporter(FLBAImportContext& InContext);
	~FLBASceneImporter();

	int32 ImportAll();

private:
	/** A sub-level to add to a scenery level. */
	struct FStreamedScene
	{
		FString LevelPackage;
		int32 SceneIndex = INDEX_NONE;
		bool bInitiallyLoaded = true;
	};

	static int32 SpriteKey(ELBASpriteBank Bank, int32 Index) { return (int32(Bank) << 16) | Index; }
	/** Sprite keys (frames for animated sprites) of a scene object; empty for 3D objects. */
	TArray<int32> GetObjectSprites(const FLBASceneObjectData& Object) const;
	void BuildSprites(const TSet<int32>& Keys);
	UStaticMesh* BuildSpriteMesh(int32 Key, const FLBASprite& Sprite, const FLBASpriteBox& Box, FIntPoint Cell, FIntPoint AtlasSize, class UMaterialInterface* Material);

	void LoadEntities();
	/** Fills MenuPlaces: lba2remake's location tree, then doors for the scenes it misses. */
	void PlaceInMenu();
	FString GetSceneName(int32 SceneIndex) const;
	FString GetGridLevelPackage(int32 Grid) const;
	TArray<FString> GetSceneryLevels(const FLBAScene& Scene) const;
	/** A cube change to another cube of the same island: walking across does it in the island level. */
	bool IsSameIslandCubeChange(const FLBAScene& Scene, const FLBASceneZoneData& Zone) const;

	ULBASceneAsset* ImportScene(const FLBAScene& Scene, const TArray<FString>& SceneryLevels, FString& OutLevelPackage);
	void PopulateLevel(UWorld* World, const FLBAScene& Scene, ULBASceneAsset* Asset);
	void AddStreamedScenes(const FString& SceneryLevel, const TArray<FStreamedScene>& Scenes);

	FLBAImportContext& Context;
	TArray<FLBASceneMapEntry> SceneMap;
	TMap<int32, FLBAScene> Scenes;

	/** Where the travel menu lists a scene (ULBASceneAsset::MenuParent, MenuOrder, MenuName). */
	struct FMenuPlace
	{
		int32 Parent = INDEX_NONE;
		int32 Order = INDEX_NONE;
		FString Name;
	};
	TMap<int32, FMenuPlace> MenuPlaces;
	TMap<int32, ULBAEntityAsset*> Entities;
	/** ANIM3DS.HQR animations: first and last frame. */
	TArray<FIntPoint> SpriteAnims;
	TMap<int32, UStaticMesh*> SpriteMeshes;
	TUniquePtr<FLBARemakeAssets> Remake;
	TUniquePtr<FLBAAudioImporter> Audio;
	int32 NumObjects = 0;
	int32 NumReplacedSprites = 0;
	int32 NumDoors = 0;
	int32 NumMissingBodies = 0;
};
