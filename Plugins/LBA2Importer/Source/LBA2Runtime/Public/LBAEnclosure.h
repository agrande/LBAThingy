#pragma once

#include "CoreMinimal.h"
#include "LBAEnclosure.generated.h"

/**
 * How an isometric interior is closed into rooms for the Enhanced look: walls raised to a room height and a ceiling
 * on top. Heights are in grid cells (256 units, 32 cm at the default world scale). The Original look keeps the
 * open rooms of the original.
 */
USTRUCT()
struct LBA2RUNTIME_API FLBAEnclosureSettings
{
	GENERATED_BODY()

	/** Raise walls and close rooms. Off for caves and outdoor grids. */
	UPROPERTY(EditAnywhere, Category = "Enclosure")
	bool bEnclose = true;

	/** Height of the rooms, in cells: their ceiling is this far over their floor (over each step of stairs). */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "3", ClampMax = "24", EditCondition = "bEnclose"))
	int32 RoomHeight = 8;

	/** Close rooms with a ceiling. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (EditCondition = "bEnclose"))
	bool bCeiling = true;

	/**
	 * Clear height of doorways, in cells above the floor: raised walls close above the doorways the original cut
	 * down with the walls facing the camera (those lost their lintel).
	 */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "4", ClampMax = "16", EditCondition = "bEnclose"))
	int32 DoorHeight = 7;
};

/** Changes to FLBAEnclosureSettings for one grid (or, later, one area of a level); unticked values are inherited. */
USTRUCT()
struct LBA2RUNTIME_API FLBAEnclosureOverrides
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_bEnclose = false;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_RoomHeight = false;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_bCeiling = false;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_DoorHeight = false;

	/** Raise walls and close rooms. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (EditCondition = "bOverride_bEnclose"))
	bool bEnclose = true;

	/** Height of the rooms, in cells over their floor. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "3", ClampMax = "24", EditCondition = "bOverride_RoomHeight"))
	int32 RoomHeight = 8;

	/** Close rooms with a ceiling. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (EditCondition = "bOverride_bCeiling"))
	bool bCeiling = true;

	/** Clear height of doorways, in cells above the floor. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "4", ClampMax = "16", EditCondition = "bOverride_DoorHeight"))
	int32 DoorHeight = 7;

	void ApplyTo(FLBAEnclosureSettings& Settings) const
	{
		if (bOverride_bEnclose)
		{
			Settings.bEnclose = bEnclose;
		}
		if (bOverride_RoomHeight)
		{
			Settings.RoomHeight = RoomHeight;
		}
		if (bOverride_bCeiling)
		{
			Settings.bCeiling = bCeiling;
		}
		if (bOverride_DoorHeight)
		{
			Settings.DoorHeight = DoorHeight;
		}
	}
};
