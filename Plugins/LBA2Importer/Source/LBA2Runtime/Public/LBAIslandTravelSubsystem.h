#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "LBAIslandTravelSubsystem.generated.h"

class UWorld;

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
	/** Imported islands, in the original game's order. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	TArray<FLBAIslandInfo> GetIslands() const { return FindIslands(); }

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
