#pragma once

#include "Engine/DataAsset.h"
#include "LBASceneAsset.generated.h"

class UWorld;

/** Zone types of the original scenes (T_ZONE.Type, see GereZones in OBJECT.CPP). */
UENUM(BlueprintType)
enum class ELBAZoneType : uint8
{
	/** Changes the scene (doors, exits); Num = target scene. */
	CubeChange = 0,
	/** Camera shot of the original's isometric or exterior camera. */
	Camera = 1,
	/** Read by life scripts (Num = zone number). */
	Scenario = 2,
	/** Brick overlay switch (GRM); no runtime handler in the original. */
	Grid = 3,
	/** Gives bonuses on the action key. */
	Giver = 4,
	/** Shows a message when the hero faces one of its sides. */
	Message = 5,
	Ladder = 6,
	Escalator = 7,
	/** Hurts whatever stands in it. */
	Hit = 8,
	/** Track for wagons. */
	Rail = 9,
};

/** Object flags of the original scenes (T_OBJET.Flags, COMMON.H). */
namespace LBASceneFlags
{
	inline constexpr int32 CheckObjectCollision = 1 << 0;
	inline constexpr int32 CheckBrickCollision = 1 << 1;
	inline constexpr int32 CheckZones = 1 << 2;
	inline constexpr int32 SpriteClip = 1 << 3;
	inline constexpr int32 Pushable = 1 << 4;
	inline constexpr int32 LowCollision = 1 << 5;
	inline constexpr int32 CheckDrowning = 1 << 6;
	inline constexpr int32 CheckOnlyFloor = 1 << 7;
	inline constexpr int32 Invisible = 1 << 9;
	inline constexpr int32 Sprite = 1 << 10;
	inline constexpr int32 Fallable = 1 << 11;
	inline constexpr int32 NoShadow = 1 << 12;
	inline constexpr int32 Background = 1 << 13;
	inline constexpr int32 Carrier = 1 << 14;
	inline constexpr int32 MiniZV = 1 << 15;
	inline constexpr int32 AnimatedSprite = 1 << 18;
}

/** One of the four ambient sample slots of a scene. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBASceneAmbientSample
{
	GENERATED_BODY()

	/** SAMPLES.HQR index, -1 when unused. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Sample = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Repeat = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Random = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Frequency = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Volume = 0;
};

USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBASceneAmbience
{
	GENERATED_BODY()

	/** Light angles of the scene (1/4096 of a turn). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 LightAlpha = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 LightBeta = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<FLBASceneAmbientSample> Samples;

	/** Seconds between two ambient samples: at least MinDelay, plus up to RandomDelay. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 MinDelay = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 RandomDelay = 0;

	/** Music played when entering the scene, -1 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Jingle = INDEX_NONE;
};

/**
 * One object of a scene as the original stores it (T_OBJET init values). Object 0 is the hero: only its
 * position and scripts are meaningful. Positions are in LBA units, local to the scene (see ULBASceneAsset).
 */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBASceneObjectData
{
	GENERATED_BODY()

	/** Community name, when known. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FString Name;

	/** LBASceneFlags bits. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Flags = 0;

	/** Entity (RESS.HQR entity table, see ULBAEntityAsset), -1 for sprites. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Entity = INDEX_NONE;

	/** Body type and animation slot of the entity at start. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Body = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Anim = INDEX_NONE;

	/** Sprite number: >= 100 SPRITES.HQR, below SPRIRAW.HQR, or the first frame in ANIM3DS.HQR for animated sprites. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Sprite = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FIntVector Position = FIntVector::ZeroValue;

	/** Yaw, 1/4096 of a turn, 0 facing +Z. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Beta = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 HitForce = 0;

	/** Bonus options (what it gives when hit or searched). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 OptionFlags = 0;

	/** Rotation speed as stored (the original converts it to a rotation time). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 RotationSpeed = 0;

	/** Move mode: 0 none, 1 manual, 2 follow, 3 track, ... (MOVE_* in COMMON.H). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 MoveMode = 0;

	/** Info..Info3 (meaning depends on the move mode: followed object, clip rectangle of doors...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<int32> Info;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 BonusCount = 0;

	/** Palette ramp of its dialog text. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 TextColor = 0;

	/** Animated sprites: animation (ANIM3DS.HQR info table) and frames per second. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 SpriteAnim = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 SpriteAnimFps = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Armour = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 LifePoints = 0;

	/** Original script bytecode, kept for a script interpreter. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<uint8> TrackScript;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<uint8> LifeScript;

	bool HasFlag(int32 Flag) const { return (Flags & Flag) != 0; }
};

/** A zone (trigger box) as the original stores it. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBASceneZoneData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	ELBAZoneType Type = ELBAZoneType::Scenario;

	/** Target scene, zone number, camera... (T_ZONE.Num). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Num = 0;

	/** Box corners in LBA units, local to the scene. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FIntVector Min = FIntVector::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FIntVector Max = FIntVector::ZeroValue;

	/** Info0..Info7; Info7 holds the zone flags (1 = on at start). See docs/ZONES.md of lba2-classic-community. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<int32> Info;

	int32 GetInfo(int32 Slot) const { return Info.IsValidIndex(Slot) ? Info[Slot] : 0; }
};

/**
 * Everything a scene (a "cube" of the original game) holds: ambience, hero start, objects with their scripts,
 * zones and track points. The importer builds a level from it (L_Scene_NNN_<Name>, streamed into its scenery
 * level), and gameplay code reads it to resolve scene changes.
 *
 * Positions are LBA units local to the scene: an exterior scene covers one 32768-unit square ("cube") of its
 * island, an interior scene its whole grid. ToWorld() converts them to the scenery level's space.
 */
UCLASS(BlueprintType)
class LBA2RUNTIME_API ULBASceneAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Scene number in SCENE.HQR (the original's cube number). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA")
	int32 SceneIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FString DisplayName;

	/** Exterior scenes are a cube of an island; interior ones an isometric grid. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA")
	bool bExterior = false;

	/** Island number of the original (0 Citadel, 2 desert...); interiors carry the island they belong to. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA")
	int32 Island = 0;

	/** Exterior: cube of the island's 16x16 map. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	FIntPoint Cube = FIntPoint::ZeroValue;

	/** Interior: grid of LBA_BKG.HQR. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Grid = INDEX_NONE;

	/** Demo (attract mode) copy of a game scene. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA")
	bool bDemo = false;

	/**
	 * Where the travel menu lists the scene (ULBAIslandMenu): under this scene (an interior under the area it opens
	 * on), or directly under its island when -1. From lba2remake's location tree, else the first scene whose door
	 * leads here.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA|Menu")
	int32 MenuParent = INDEX_NONE;

	/** Rank among its island's scenes in the travel menu; -1 keeps the scene out of it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA|Menu")
	int32 MenuOrder = INDEX_NONE;

	/** Name in the travel menu, short since it shows under its parent ("Cellar" under "Twinsen's House"). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA|Menu")
	FString MenuName;

	/** The scene's own level (objects, zones, points, hero start). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "LBA")
	TSoftObjectPtr<UWorld> Level;

	/**
	 * Levels the scene is streamed into: its island or grid level. Citadel's scenes are in both the storm and the
	 * sunny island; the first one is where travelling to the scene goes, unless another one is already open.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<TSoftObjectPtr<UWorld>> SceneryLevels;

	/** Where the scene's LBA origin lies in the scenery level (Unreal units). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FVector Origin = FVector::ZeroVector;

	/** Centimeters per LBA unit used when the scene was imported. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	float WorldScale = 0.125f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FLBASceneAmbience Ambience;

	/** Objects; [0] is the hero (start position and scripts). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<FLBASceneObjectData> Objects;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<FLBASceneZoneData> Zones;

	/** Track points (LBA units, local to the scene) used by move scripts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<FIntVector> Points;

	/** Converts a scene position (LBA units) to the scenery level's space. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	FVector ToWorld(const FIntVector& LbaPosition) const;

	/** Converts a position of the scenery level to the scene's LBA units. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	FIntVector ToScene(const FVector& WorldPosition) const;

	/** Unreal yaw (degrees) of an ACharacter facing an LBA angle (meshes face +Y, rotated -90). */
	static double BetaToActorYaw(int32 Beta);
	static int32 ActorYawToBeta(double Yaw);

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("LBAScene"), GetFName());
	}
};
