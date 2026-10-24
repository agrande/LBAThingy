#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "LBAIslandTravelSubsystem.generated.h"

class UWorld;

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

/**
 * Lists the imported island levels and travels between them. Islands are discovered in the asset registry
 * (Project Settings > Plugins > LBA2 > Island Menu), so newly imported islands show up automatically; the
 * LBA2Editor module adds the same levels to every cook, since nothing references them.
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

	/** Id of the island level currently loaded, or None. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	FName GetCurrentIslandId() const;

	/** Opens the island's level; the player spawns at its PlayerStart. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TravelToIsland(FName IslandId);

	/** True the first time it is asked in a packaged or standalone game, when the island menu should greet the player. */
	bool ConsumeStartupMenu(const UWorld* World);

private:
	bool bStartupMenuConsumed = false;
};
