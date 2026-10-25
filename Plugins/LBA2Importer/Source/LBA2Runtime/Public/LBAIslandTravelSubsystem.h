#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "LBAIslandTravelSubsystem.generated.h"

class UWorld;
class ALBATestStation;

/** Where an island lies, the travel menu's top level (lba2remake's planets). */
UENUM(BlueprintType)
enum class ELBAPlanet : uint8
{
	Twinsun,
	EmeraldMoon,
	ZeelishSurface,
	ZeelishUndergas,
	/** Islands the menu does not know. */
	Other,
};

/** An island level the player can travel to. */
USTRUCT(BlueprintType)
struct FLBAIslandInfo
{
	GENERATED_BODY()

	/** Island file name (CITABAU, DESERT...). */
	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	FName Id;

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	TSoftObjectPtr<UWorld> Level;

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	ELBAPlanet Planet = ELBAPlanet::Other;

	/** Island number of the original's scenes on it (ULBASceneAsset::Island), -1 when none are. */
	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	int32 SceneIsland = INDEX_NONE;
};

/** A station of a test level (ALBATestStation), as the travel menu lists it. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBATestStationInfo
{
	GENERATED_BODY()

	/** The station's PlayerStartTag: "Map#Tag" travels to it. */
	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	FName Tag;

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	FText Description;

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	int32 Order = 0;

	/** Where it is (filled for stations of a loaded level only). */
	FTransform Transform;

	static FLBATestStationInfo FromStation(const class ALBATestStation& Station);

	/** One station per line, fields separated by tabs (ALBATestStation::StationsTag). */
	static FString ToTagValue(const TArray<FLBATestStationInfo>& Stations);
	static TArray<FLBATestStationInfo> FromTagValue(const FString& Value);
};

/** A test level or sandbox (Project Settings > Plugins > LBA2 > Test Levels). */
USTRUCT(BlueprintType)
struct FLBATestLevelInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	TSoftObjectPtr<UWorld> Level;

	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	FText DisplayName;

	/** The feature test map: listed first, the main place for manual tests. */
	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	bool bFeatureLevel = false;

	/** In menu order. */
	UPROPERTY(BlueprintReadOnly, Category = "LBA")
	TArray<FLBATestStationInfo> Stations;
};

/** What a gateway or a menu entry leads to. */
UENUM(BlueprintType)
enum class ELBATravelKind : uint8
{
	/** An island level's start. */
	Island,
	/** A scene's hero start (an interior, or an area of an island). */
	Scene,
	/** A test level's start, or one of its stations. */
	TestLevel,
};

USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBATravelDestination
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA")
	ELBATravelKind Kind = ELBATravelKind::Scene;

	/** Island: the island (CITABAU, DESERT...). Scene: the island variant to arrive on (CITADEL = in the storm), None for the last one played. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA", meta = (EditCondition = "Kind != ELBATravelKind::TestLevel", EditConditionHides))
	FName Island;

	/** Scene number (43 = Citadel harbour, 0 = Twinsen's house...). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA", meta = (EditCondition = "Kind == ELBATravelKind::Scene", EditConditionHides))
	int32 Scene = INDEX_NONE;

	/** The test level; none for the level the gateway is in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA", meta = (EditCondition = "Kind == ELBATravelKind::TestLevel", EditConditionHides))
	TSoftObjectPtr<UWorld> Level;

	/** A station of that level (its PlayerStartTag), None for its start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA", meta = (EditCondition = "Kind == ELBATravelKind::TestLevel", EditConditionHides))
	FName Station;

	static FLBATravelDestination ToIsland(FName Island);
	static FLBATravelDestination ToScene(int32 Scene, FName Island = NAME_None);
	static FLBATravelDestination ToTestLevel(const TSoftObjectPtr<UWorld>& Level, FName Station = NAME_None);

	/** Name for signs: the island's, the scene's (menu name), the station's or the level's (World: the level "none" means). */
	FText Describe(const UWorld* World = nullptr) const;
};

/**
 * Lists the imported island levels and travels between them. Islands are discovered in the asset registry
 * (Project Settings > Plugins > LBA2 > Island Menu), so newly imported islands show up automatically; the
 * LBA2Editor module adds the same levels to every cook, since nothing references them.
 * It also lists the test levels and sandboxes (Project Settings > Plugins > LBA2 > Test Levels), the feature test map
 * first, and goes to their stations.
 */
UCLASS()
class LBA2RUNTIME_API ULBAIslandTravelSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Imported islands, planet by planet. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	TArray<FLBAIslandInfo> GetIslands() const { return FindIslands(); }

	UFUNCTION(BlueprintPure, Category = "LBA")
	static FText GetPlanetName(ELBAPlanet Planet);

	/** Island levels found under Project Settings > Plugins > LBA2 > Islands Path (also what the cook adds). */
	static TArray<FLBAIslandInfo> FindIslands();

	/**
	 * Test levels that exist: the feature test map, the levels under Test Levels Path, then the sandboxes. Stations come
	 * from the asset registry, or from LoadedWorld's actors when it is one of them (unsaved edits, live positions).
	 */
	static TArray<FLBATestLevelInfo> FindTestLevels(const UWorld* LoadedWorld = nullptr);

	/** "L_FeatureTest" -> "Feature Test", "Lvl_ThirdPerson" -> "Third Person". */
	static FText GetLevelDisplayName(const FString& PackageName);

	/** Id of the island level currently loaded, or None. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	FName GetCurrentIslandId() const;

	/** Opens the island's level; the player spawns at its PlayerStart. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TravelToIsland(FName IslandId);

	/**
	 * Opens a test level at a station (None: its start), or teleports there when it is the current level (unless bReload:
	 * it opens again, everything starting over). Test levels open with ALBAGameMode (Test Levels > Play Test Levels As
	 * Twinsen) so template sandboxes have Twinsen and the menu.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TravelToTestLevel(const TSoftObjectPtr<UWorld>& Level, FName Station, bool bReload = false);

	/** Goes to an island, a scene or a test level. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TravelTo(const FLBATravelDestination& Destination);

	/** True the first time it is asked in a packaged or standalone game, when the island menu should greet the player. */
	bool ConsumeStartupMenu(const UWorld* World);

private:
	bool bStartupMenuConsumed = false;
};
