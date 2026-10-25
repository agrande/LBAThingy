#pragma once

#include "CoreMinimal.h"
#include "Debug/DebugDrawComponent.h"
#include "GameFramework/Actor.h"
#include "LBAGridPlan.h"
#include "LBAGridPlanActor.generated.h"

/**
 * Draws a grid's plan (ULBAGridPlanAsset) over its level: room floors and their air up to the ceiling, each side colored
 * by what lies past it, connectors, wall columns by the rule that made them walls, labels. Show flag: Developer > LBA2
 * Grid Plan (off by default; the LBA2 Plan Browser turns it on). Select the actor and click a room or connector in the viewport to see its data in the Details panel.
 */
UCLASS(ClassGroup = "LBA2", meta = (BlueprintSpawnableComponent), HideCategories = (Physics, Collision, Lighting, Rendering, Navigation, Mobile, HLOD, Cooking))
class LBA2RUNTIME_API ULBAGridPlanComponent : public UDebugDrawComponent
{
	GENERATED_BODY()

public:
	ULBAGridPlanComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View")
	TObjectPtr<ULBAGridPlanAsset> Plan;

	/** Only the level with this floor height (cells), -1: all levels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View", meta = (ClampMin = "-1", ClampMax = "24"))
	int32 OnlyLevel = INDEX_NONE;

	/** Room floors (colored by room) and the outline of their air up to the ceiling. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View")
	bool bShowRooms = true;

	/** Each room side, colored by what lies past it: wall red, open side green, connector its kind's color, step orange, open gray. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View")
	bool bShowSides = true;

	/** Connectors' columns: stairs yellow, passage cyan, exit magenta, alcove blue, pocket gray. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View")
	bool bShowConnectors = true;

	/**
	 * The plan's walls, a cell high over the level: colored by their rule (cut orange, jamb yellow, invisible pale blue,
	 * masonry brown, exterior gray); gaps in wall lines white, partition lines pale green, furniture kept from being a
	 * wall violet.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View")
	bool bShowWalls = true;

	/** The top of the air in every column (ceilings, lintels, gallery undersides): a tile under it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View")
	bool bShowAir = false;

	/** Names and numbers of rooms and connectors. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Plan View")
	bool bShowLabels = true;

	/** The room clicked in the viewport (or picked here), drawn highlighted; -1: none. */
	UPROPERTY(EditAnywhere, Transient, BlueprintReadOnly, Category = "Selection", meta = (ClampMin = "-1"))
	int32 SelectedRoom = INDEX_NONE;

	/** The connector clicked in the viewport (or picked here), drawn highlighted; -1: none. */
	UPROPERTY(EditAnywhere, Transient, BlueprintReadOnly, Category = "Selection", meta = (ClampMin = "-1"))
	int32 SelectedConnector = INDEX_NONE;

	/** The selected room's data. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Selection")
	FLBAGridPlanRoom Room;

	/** The selected connector's data. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Selection")
	FLBAGridPlanConnector Connector;

	/** Selects a room or a connector (-1: none) and refreshes the selection's data. */
	void Select(int32 InRoom, int32 InConnector);

	/** Colors the views use for things of a plan. */
	static FColor GetRoomColor(int32 Room);
	static FColor GetSideColor(ELBARoomSide Kind, const FLBAGridPlan& Plan, int32 Other);
	static FColor GetConnectorColor(ELBAConnector Kind);
	/** A wall rule's color (None: a wall no rule names). */
	static FColor GetRuleColor(ELBAWallRule Rule);

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
	virtual void OnRegister() override;

protected:
	virtual FDebugRenderSceneProxy* CreateDebugSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	void RefreshSelection();
};

/** A grid's plan in its level (editor only; spawned by the importer, tagged LBA2Imported). */
UCLASS(ClassGroup = "LBA2", HideCategories = (Physics, Collision, Lighting, Rendering, Navigation, Input, Actor, LOD, Cooking, Replication, HLOD, WorldPartition, DataLayers))
class LBA2RUNTIME_API ALBAGridPlanActor : public AActor
{
	GENERATED_BODY()

public:
	ALBAGridPlanActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Plan View")
	TObjectPtr<ULBAGridPlanComponent> PlanView;
};
