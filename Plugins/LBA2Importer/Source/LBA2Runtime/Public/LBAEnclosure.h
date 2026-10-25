#pragma once

#include "CoreMinimal.h"
#include "LBAEnclosure.generated.h"

/** What holds an interior's roof, which decides how it is closed (FLBAEnclosureSettings::Structure). */
UENUM()
enum class ELBAEnclosureStructure : uint8
{
	/** Built rooms: rectangles of floor, four walls each, a ceiling over each space (houses, offices, halls). */
	Rooms,
	/**
	 * A hollow in the rock (or in masonry): the walkable floors and what stands on them, under one roof springing from
	 * whatever bounds them (caves, a temple carved in a mountain, a cistern).
	 */
	Hollow,
};

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

	/** What holds the roof: built rooms, or a hollow in the rock (for a hollow, Room Height is its roof's spring over the floor). */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (EditCondition = "bEnclose"))
	ELBAEnclosureStructure Structure = ELBAEnclosureStructure::Rooms;

	/**
	 * Vaulted roofs: how far from its supports (walls, the rock) a roof curves up, in columns (0: flat ceilings). Spans
	 * narrower than twice this meet in a point (pointed arches, tunnels), wider ones get a coved, level crown.
	 */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "bEnclose"))
	float VaultRadius = 0.f;

	/** How high a vaulted roof rises over its spring, in cells. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0", ClampMax = "24", EditCondition = "bEnclose"))
	float VaultRise = 0.f;

	/** Rough rock: bumps of a vaulted roof, in cells (0: dressed stone). */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0", ClampMax = "4", EditCondition = "bEnclose"))
	float VaultNoise = 0.f;

	/**
	 * Hollows: how far around, in columns, the floor sets the roof's spring. Short: the roof follows the ground (a cave);
	 * long: one level over the highest floor (a cistern, a hall).
	 */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "1", ClampMax = "64", EditCondition = "bEnclose"))
	float SpringReach = 2.f;
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

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_Structure = false;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_VaultRadius = false;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_VaultRise = false;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_VaultNoise = false;

	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (InlineEditConditionToggle))
	bool bOverride_SpringReach = false;

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

	/** What holds the roof: built rooms, or a hollow in the rock. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (EditCondition = "bOverride_Structure"))
	ELBAEnclosureStructure Structure = ELBAEnclosureStructure::Rooms;

	/** How far from its supports a vaulted roof curves up, in columns (0: flat). */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "bOverride_VaultRadius"))
	float VaultRadius = 0.f;

	/** How high a vaulted roof rises over its spring, in cells. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0", ClampMax = "24", EditCondition = "bOverride_VaultRise"))
	float VaultRise = 0.f;

	/** Bumps of a vaulted roof, in cells (0: dressed stone). */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "0", ClampMax = "4", EditCondition = "bOverride_VaultNoise"))
	float VaultNoise = 0.f;

	/** Hollows: how far around, in columns, the floor sets the roof's spring. */
	UPROPERTY(EditAnywhere, Category = "Enclosure", meta = (ClampMin = "1", ClampMax = "64", EditCondition = "bOverride_SpringReach"))
	float SpringReach = 2.f;

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
		if (bOverride_Structure)
		{
			Settings.Structure = Structure;
		}
		if (bOverride_VaultRadius)
		{
			Settings.VaultRadius = VaultRadius;
		}
		if (bOverride_VaultRise)
		{
			Settings.VaultRise = VaultRise;
		}
		if (bOverride_VaultNoise)
		{
			Settings.VaultNoise = VaultNoise;
		}
		if (bOverride_SpringReach)
		{
			Settings.SpringReach = SpringReach;
		}
	}
};
