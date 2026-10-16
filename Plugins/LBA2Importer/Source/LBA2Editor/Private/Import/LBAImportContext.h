#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Formats/LBAHqr.h"
#include "MeshDescription.h"

class ULBA2ImporterSettings;
class UMaterialInterface;
class UStaticMesh;
class UTexture2D;
class UWorld;
class AActor;
class FLBAMaterialLibrary;
class FLBARenderingAssets;
struct FLBAXpl;
struct FScopedSlowTask;

/** Collision options for imported static meshes. */
struct FLBAStaticMeshOptions
{
	/** Use the render triangles as collision (terrain, interiors). */
	bool bComplexAsSimple = false;
	/** Use this mesh (instead of the render mesh) as complex collision. */
	UStaticMesh* ComplexCollisionMesh = nullptr;
	/** Add an axis-aligned simple box collision covering the mesh. */
	bool bSimpleBox = false;
	/** No collision at all (collision-only helpers, decals...). */
	bool bNoCollision = false;
	/** Defer the build so many meshes can be built in parallel with FinishPendingBuilds(). */
	bool bDeferBuild = true;
	/** Fills simple collision shapes (boxes, convex hulls) in Unreal units. */
	TFunction<void(struct FKAggregateGeom&)> SimpleCollision;
};

/**
 * Shared state for one import run: settings, opened HQR files, palette, materials and helpers to
 * create/re-use assets and levels. Re-importing re-uses existing assets in place so references stay
 * valid, and only replaces actors that the importer itself created inside levels.
 */
class FLBAImportContext
{
public:
	static const FName GeneratedActorTag;

	explicit FLBAImportContext(const ULBA2ImporterSettings& InSettings);
	~FLBAImportContext();

	bool Initialize(FString& OutError);

	const ULBA2ImporterSettings& GetSettings() const { return Settings; }
	const FString& GetDataDirectory() const { return DataDirectory; }
	const FString& GetContentRoot() const { return ContentRoot; }
	FString GetContentPath(const FString& SubPath) const { return ContentRoot / SubPath; }
	float GetScale() const { return Scale; }

	/** Opens (and caches) a game file such as BODY.HQR. Returns null when missing. */
	const FLBAHqrFile* GetHqr(const FString& FileName);
	FString GetGameFilePath(const FString& FileName) const;

	/**
	 * Active palette for colors and textures. Each island has its own palette (FLBAXpl); interiors and characters
	 * use the Citadel (Citabau) one, like the original.
	 */
	const TArray<FColor>& GetPalette() const { return Palette; }
	FColor GetPaletteColor(int32 Index) const { return Palette.IsValidIndex(Index) ? Palette[Index] : FColor::Magenta; }

	/** Switches the active palette to an island palette (RESS.HQR entry, see FLBAXpl). Returns it, or null. */
	const FLBAXpl* UsePalette(int32 XplEntry);
	const FLBAXpl* GetXpl(int32 XplEntry);
	const FLBAXpl* GetActiveXpl() const { return ActiveXpl; }

	FLBAMaterialLibrary& GetMaterials();
	FLBARenderingAssets& GetRendering();

	/** Optional community-authored names (MIT, from the lba2remake project). May be null. */
	TSharedPtr<FJsonObject> GetCommunityNames() const { return CommunityNames; }
	FString GetCommunityName(const FString& Table, int32 Index) const;

	// Assets ------------------------------------------------------------------------------------------

	/** Creates an asset, or returns the existing one with the same path so it can be updated in place. */
	UObject* CreateOrReuseAsset(UClass* Class, const FString& Folder, const FString& Name, bool* bOutExisted = nullptr);

	template <typename T>
	T* CreateOrReuseAsset(const FString& Folder, const FString& Name, bool* bOutExisted = nullptr)
	{
		return Cast<T>(CreateOrReuseAsset(T::StaticClass(), Folder, Name, bOutExisted));
	}

	/** Loads an existing asset created by an earlier step or import run. */
	UObject* FindAsset(UClass* Class, const FString& Folder, const FString& Name) const;

	template <typename T>
	T* FindAsset(const FString& Folder, const FString& Name) const
	{
		return Cast<T>(FindAsset(T::StaticClass(), Folder, Name));
	}

	/** Marks an asset for saving at the next Flush(). */
	void MarkForSave(UObject* Asset);

	/** Finishes pending mesh/texture builds and saves every asset marked so far. */
	void Flush();

	/** Builds a texture from palette indices. Index 0 becomes transparent when bColorKey is set. */
	UTexture2D* CreatePaletteTexture(const FString& Folder, const FString& Name, int32 Width, int32 Height,
		TFunctionRef<uint8(int32 X, int32 Y)> GetIndex, bool bColorKey, bool bWrap);

	/** Builds a texture from true-color pixels (BGRA, row-major). */
	UTexture2D* CreateTexture(const FString& Folder, const FString& Name, int32 Width, int32 Height,
		const TArray<FColor>& Pixels, bool bWrap, bool bSRGB = true);

	UStaticMesh* CreateStaticMesh(const FString& Folder, const FString& Name, FMeshDescription&& MeshDescription,
		const TArray<UMaterialInterface*>& Materials, const TArray<FName>& SlotNames, const FLBAStaticMeshOptions& Options);

	/** Builds every mesh created with bDeferBuild in one parallel batch. */
	void FinishPendingBuilds();

	// Levels ------------------------------------------------------------------------------------------

	/**
	 * Opens an existing level (removing the actors a previous import generated) or creates a new one.
	 * User-placed actors are preserved.
	 */
	UWorld* BeginLevel(const FString& Folder, const FString& Name);

	/** Saves the level and releases it if this context created/loaded it. */
	void EndLevel(UWorld* World);

	/** Spawns an actor tagged as generated, with an editor label and outliner folder. */
	AActor* SpawnGeneratedActor(UWorld* World, UClass* Class, const FTransform& Transform, const FString& Label, const FName& Folder);

	template <typename T>
	T* SpawnGeneratedActor(UWorld* World, const FTransform& Transform, const FString& Label, const FName& Folder)
	{
		return Cast<T>(SpawnGeneratedActor(World, T::StaticClass(), Transform, Label, Folder));
	}

	/**
	 * Same, through the editor's actor factory for the class (falls back to a plain spawn), so the actor gets the
	 * defaults it would get when dragged into a level (water bodies: materials, waves, ocean filling its zone).
	 */
	AActor* SpawnGeneratedActorWithFactory(UWorld* World, UClass* Class, const FTransform& Transform, const FString& Label, const FName& Folder);

	template <typename T>
	T* SpawnGeneratedActorWithFactory(UWorld* World, const FTransform& Transform, const FString& Label, const FName& Folder)
	{
		return Cast<T>(SpawnGeneratedActorWithFactory(World, T::StaticClass(), Transform, Label, Folder));
	}

	// Progress / reporting ---------------------------------------------------------------------------

	void SetSlowTask(FScopedSlowTask* InTask) { SlowTask = InTask; }
	void ReportProgress(const FText& Message, float Work = 0.f);
	bool IsCancelled() const;

	void Warn(const FString& Message);
	int32 GetNumWarnings() const { return NumWarnings; }

	static FString SanitizeName(const FString& In);

private:
	bool LoadPalette(FString& OutError);
	void LoadCommunityNames();
	bool SavePackageFor(UObject* Asset);
	void TagGeneratedActor(AActor* Actor, const FString& Label, const FName& Folder);

	const ULBA2ImporterSettings& Settings;
	FString DataDirectory;
	FString ContentRoot;
	float Scale = 0.125f;

	TMap<FString, TUniquePtr<FLBAHqrFile>> HqrCache;
	TArray<FColor> Palette;
	TMap<int32, TUniquePtr<FLBAXpl>> XplCache;
	const FLBAXpl* ActiveXpl = nullptr;
	TUniquePtr<FLBAMaterialLibrary> Materials;
	TUniquePtr<FLBARenderingAssets> Rendering;
	TSharedPtr<FJsonObject> CommunityNames;

	TArray<TWeakObjectPtr<UObject>> PendingSaves;
	TArray<UStaticMesh*> PendingMeshBuilds;
	TSet<UWorld*> OwnedWorlds;

	FScopedSlowTask* SlowTask = nullptr;
	int32 NumWarnings = 0;
};
