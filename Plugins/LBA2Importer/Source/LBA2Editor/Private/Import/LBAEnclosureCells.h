#pragma once

#include "CoreMinimal.h"

class FLBARoomAnalysis;
struct FLBADoorway;
struct FLBAEnclosureSettings;

/** What an enclosure cell makes. */
enum class ELBAEnclosurePart : uint8
{
	/** A perimeter wall raised or closed. */
	Wall,
	/** A visible perimeter wall cell drawn again behind its faces. */
	Backing,
	Ceiling,
	/** Where the ceiling steps up to a neighbor's. */
	Riser,
	/** The wall above a doorway. */
	Doorway,
	/** A wall inside the room raised to the ceiling: a partition the original cut down, or a camera-side wall's stub. */
	Partition,
};

/**
 * The cells an interior's enclosure adds to its grid, and the room air they close: the enclosure is drawn as the
 * faces of the filled cells that touch the air, so every face looks into a room and a camera outside sees through.
 *
 * Per space (FLBARoomAnalysis), the room covers the footprint the outside does not reach when it floods in from beyond
 * the grid through columns with nothing at or above the room's lowest floor (voids, lower ground, the grid's edge):
 * its floors, and the furniture, partitions and walls among them. The perimeter is the footprint next to the outside
 * but its floors (cut-down walls, their invisible collision, the trims the camera-side walls end in), closed up to the
 * ceiling, with a backing behind their visible cells (the grid only has the faces the iso camera saw). Walls inside the
 * footprint rise to the ceiling from the top of what is drawn of them when the original cut them down (their
 * collision goes on above it, invisible or as bricks without art; railings and furniture end where they are drawn),
 * and so do the stubs of the camera-side walls, whose perimeter one column further out draws nothing: the raised wall
 * is flush with them. Cells not drawn (empty, invisible collision, bricks without art, the frames the original put on
 * its cut walls) are filled; the frames inside the enclosure come off. Thin spaces (wall tops joined by steps) are
 * not rooms.
 *
 * Each room (the footprint's columns joined around walls and doorways) has one ceiling, RoomHeight cells above its
 * highest floor; walls and doorways take the highest of the rooms next to them, with risers where it steps. Above doorways whose side walls reach the ceiling, the wall is closed from the
 * top of the doorway (its lintel, or DoorHeight above the floor where the original cut the lintel away with the walls
 * facing the camera). Exits stay open below that top through the perimeter in line with them (the invisible plug the
 * original puts past them). Open edges (balconies, floors without a wall) stay open.
 */
struct FLBAEnclosureCells
{
	/** Thickest wall an exit's passage goes through (it stays open up to the top of the doorway). */
	static constexpr int32 MaxWallThickness = 3;
	/** Floor cells with floor all around a space needs to be a room (not a strip of wall tops). */
	static constexpr int32 MinInnerFloors = 4;
	/** Collision cells above what is drawn of a wall that tell a wall the original cut down (not a railing). */
	static constexpr int32 MinCutCollision = 2;

	/** Cells the enclosure draws: empty cells made solid, and invisible collision walling a room in. */
	TSet<FIntVector> Fill;
	/** Cells inside the rooms without art (empty, or invisible collision), with the index of their space. */
	TMap<FIntVector, int32> Air;
	/** Visible cells of perimeter walls: drawn again just behind their faces that look into a room, where the grid may have no back face. */
	TSet<FIntVector> Backing;
	/** What each Fill and Backing cell makes. */
	TMap<FIntVector, ELBAEnclosurePart> Parts;

	bool IsEmpty() const { return Fill.Num() == 0 && Backing.Num() == 0; }

	/** IsDrawn: the cell shows art in the grid (not empty, invisible collision, a brick without art or a frame to remove). */
	static FLBAEnclosureCells Build(const FLBARoomAnalysis& Rooms, TConstArrayView<FLBADoorway> Doorways, const FLBAEnclosureSettings& Settings,
		TFunctionRef<bool(int32, int32, int32)> IsDrawn);
};
