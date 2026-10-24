#pragma once

#include "CoreMinimal.h"
#include "LBASceneAsset.h"

/**
 * A scene of SCENE.HQR (entry Index + 1; entry 0 holds the size of the biggest scene). See LoadScene in
 * SOURCES/DISKFUNC.CPP:
 *
 *   header      S8 island, S8 cube x, S8 cube y, S8 shadow level, S8 labyrinth mode, S8 cube mode (1 exterior), S8 unused
 *   ambience    S16 light alpha, beta; 4 x { S16 sample, repeat, random, frequency, volume }; S16 min delay, random delay; S8 jingle
 *   hero        S16 x, y, z; S16 track size + track script; S16 life size + life script
 *   objects     S16 count (hero included), then per object: U32 flags, S16 entity, U8 body, S16 anim, S16 sprite,
 *               S16 x, y, z, S8 hit force, S16 option flags, S16 beta, S16 rotation speed, S8 move, S16 info0..3,
 *               S16 bonus count, S8 text color, [U32 anim 3DS, S16 fps when ANIM_3DS], S8 armour, S8 life,
 *               S16 track size + script, S16 life size + script
 *   U32 checksum
 *   zones       S16 count, then per zone: S32 x0, y0, z0, x1, y1, z1, S32 info0..7, S16 type, S16 num
 *   points      S16 count, then S32 x, y, z each
 *   patches     U32 count, then { S16 size, S16 offset }
 */
struct FLBAScene
{
	int32 Index = INDEX_NONE;
	int32 Island = 0;
	int32 CubeX = 0;
	int32 CubeY = 0;
	int32 ShadowLevel = 0;
	int32 Labyrinth = 0;
	int32 CubeMode = 0;

	FLBASceneAmbience Ambience;
	/** [0] is the hero. */
	TArray<FLBASceneObjectData> Objects;
	TArray<FLBASceneZoneData> Zones;
	TArray<FIntVector> Points;

	bool IsExterior() const { return CubeMode == 1; }

	bool Load(const TArray<uint8>& Data, int32 InIndex);
};

namespace LBAScenes
{
	/** First demo (attract mode) scene; 193-221 copy game scenes. */
	inline constexpr int32 FirstDemoScene = 193;

	/**
	 * Island file names (ILE) an island number shows, as InitGrilleExt picks them: Citadel is the storm island until
	 * the storm ends (CITADEL, then CITABAU), Celebration gets its statue later (CELEBRAT, then CELEBRA2).
	 */
	TArray<FString> GetIslandFiles(int32 Island);

	/**
	 * A sliding door's track (SPRITE_CLIP sprites): OPEN_LEFT/RIGHT/UP/DOWN <width> slides it along -x/+x/-z/+z by the width
	 * at 1000 units/s, CLOSE slides it back (GERETRAK.CPP, OBJECT.CPP AUTO_STOP_DOOR).
	 */
	struct FDoorTrack
	{
		/** LBA direction of the opening (unit, on x or z). */
		FIntVector Direction = FIntVector::ZeroValue;
		int32 Width = 0;
		/** Samples the track plays before opening and closing (SAMPLE...), INDEX_NONE without. */
		int32 OpenSample = INDEX_NONE;
		int32 CloseSample = INDEX_NONE;
	};

	/** Door sliding speed (units/s, SRot = 1000 in GERETRAK.CPP). */
	inline constexpr int32 DoorSpeed = 1000;
	/** The scripts close a door once Twinsen is this far (the museum's: DISTANCE > 2500). */
	inline constexpr int32 DoorAutoCloseDistance = 2500;

	/** Finds the first OPEN_* of a track script. */
	bool FindDoorTrack(const TArray<uint8>& Track, FDoorTrack& OutDoor);
}
