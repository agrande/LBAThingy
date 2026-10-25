#pragma once

#include "GameFramework/PlayerStart.h"
#include "LBATestStation.generated.h"

class UTextRenderComponent;
class FAssetRegistryTagsContext;

/**
 * A spot of a test level where one feature is tried (melee, ladders, the Dinofly...). The travel menu's Tests tab lists
 * a level's stations under it and goes straight to them, in the game and in the editor's level selector; gateways can
 * lead to them too.
 *
 * It is a player start whose PlayerStartTag names the station, so travelling to it is Unreal's own "Map#Station" URL
 * portal. ALBAGameMode never starts at a station otherwise. Levels list their stations in the asset registry
 * (LBATestStations tag), so the menu shows them without loading the level.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBATestStation : public APlayerStart
{
	GENERATED_BODY()

public:
	ALBATestStation(const FObjectInitializer& ObjectInitializer);

	/** Name in the menu and on the sign above the station. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Test")
	FText DisplayName;

	/** What to try here (menu tooltip, sign under the name). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Test", meta = (MultiLine = true))
	FText Description;

	/** Menu order among the level's stations, lowest first. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Test")
	int32 Order = 0;

	/** Shows the name and description above the station in game. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Test")
	bool bShowSign = true;

	virtual void OnConstruction(const FTransform& Transform) override;

	/** Asset registry tag listing a level's stations (see FLBATestStationInfo). */
	static const FName StationsTag;

#if WITH_EDITOR
	/** Adds StationsTag to levels that have stations (bound to UObject's extra asset registry tags by the module). */
	static void AddLevelStationsTag(FAssetRegistryTagsContext Context);
#endif

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Test")
	TObjectPtr<UTextRenderComponent> NameSign;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Test")
	TObjectPtr<UTextRenderComponent> DescriptionSign;
};
