#pragma once

#include "GameFramework/PainCausingVolume.h"
#include "GameFramework/Volume.h"
#include "LBASceneAsset.h"
#include "LBAZoneVolume.generated.h"

/**
 * A zone of an original scene: an axis-aligned trigger box. Each zone type has its own volume class carrying
 * the original's settings as editable properties; the importer places one per zone, sized to its box. The
 * base keeps the raw data (FLBASceneZoneData in the scene asset) for scripts.
 */
UCLASS(Abstract, NotBlueprintable)
class LBA2RUNTIME_API ALBAZoneVolume : public AVolume
{
	GENERATED_BODY()

public:
	ALBAZoneVolume(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	TObjectPtr<ULBASceneAsset> Scene;

	/** Zone number in the scene (FLBASceneZoneData). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	int32 ZoneIndex = INDEX_NONE;

	/** On (the original's ZONE_ON). Scripts can switch zones on and off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA")
	bool bEnabled = true;

	virtual ELBAZoneType GetZoneType() const PURE_VIRTUAL(ALBAZoneVolume::GetZoneType, return ELBAZoneType::Scenario;);

	/** Fills the typed properties from the original zone. */
	virtual void InitFromZone(const FLBASceneZoneData& Zone) {}

	const FLBASceneZoneData* GetZoneData() const;
};

/**
 * Door or exit to another scene (type 0). Twinsen entering it travels to the target scene, arriving at the
 * same spot relative to the zone in the target's arrival box. Zones between neighbouring cubes of the same
 * island are not placed: the whole island is one level, so walking across is enough.
 */
UCLASS()
class LBA2RUNTIME_API ALBACubeChangeVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBACubeChangeVolume(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 TargetScene = 0;

	/** Arrival point for the zone's corner, in the target scene's LBA units (Y relative to the zone's floor). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FIntVector Arrival = FIntVector::ZeroValue;

	/** Quarter turns applied to the hero (and to its offset in the zone) on arrival. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 ArrivalQuarterTurns = 0;

	/** Only fires when the hero walks into the door (the original tests a brick collision in front of him). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	bool bRequireWalkingIn = false;

	/** The original does not snap the hero onto the ground on arrival. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	bool bKeepArrivalHeight = false;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::CubeChange; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
	virtual void NotifyActorBeginOverlap(AActor* OtherActor) override;
	virtual void NotifyActorEndOverlap(AActor* OtherActor) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void Fire(APawn* Pawn);
	bool IsWalkingIntoWall(const APawn* Pawn) const;

	/** Player pawn inside, waiting to walk into the door (bRequireWalkingIn). */
	TWeakObjectPtr<APawn> WaitingPawn;
};

/** Camera shot of the original (type 1): kept as data, the modern camera ignores it. */
UCLASS()
class LBA2RUNTIME_API ALBACameraZoneVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBACameraZoneVolume(const FObjectInitializer& ObjectInitializer);

	/** Point the camera looks at (bricks for interiors). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FIntVector LookAt = FIntVector::ZeroValue;

	/** Camera angles (1/4096 of a turn) and distance (LBA units). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FIntVector Angles = FIntVector::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Distance = 0;

	/** Re-aims every frame the hero is inside instead of once. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	bool bForced = false;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Camera; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/** Zone read by life scripts (type 2). */
UCLASS()
class LBA2RUNTIME_API ALBAScenarioZoneVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBAScenarioZoneVolume(const FObjectInitializer& ObjectInitializer);

	/** Number scripts test (LF_ZONE / LF_ZONE_OBJ). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 ZoneNumber = 0;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Scenario; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/** Brick overlay zone (type 3, GRM): no runtime behaviour in the original either. */
UCLASS()
class LBA2RUNTIME_API ALBAGridZoneVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBAGridZoneVolume(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Overlay = 0;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Grid; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/** Gives bonuses when the hero searches inside it (type 4). */
UCLASS()
class LBA2RUNTIME_API ALBAGiverZoneVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBAGiverZoneVolume(const FObjectInitializer& ObjectInitializer);

	/** Bonus kinds (WhichBonus bit mask: money, life, magic, key, clover...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 BonusKinds = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Amount = 0;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Giver; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/** Shows a message when the hero faces one of its sides (type 5). */
UCLASS()
class LBA2RUNTIME_API ALBAMessageZoneVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBAMessageZoneVolume(const FObjectInitializer& ObjectInitializer);

	/** Message number in the scene's text bank. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Message = 0;

	/** Side the hero must face (1 north, 2 south, 4 east, 8 west). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 FacingSide = 0;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Message; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/**
 * Ladder (type 6). Twinsen climbs it when he walks into the wall inside the zone; the top of the box is as high
 * as the climb goes, where he climbs out onto the ledge.
 */
UCLASS()
class LBA2RUNTIME_API ALBALadderVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBALadderVolume(const FObjectInitializer& ObjectInitializer);

	/** Top of the climb (world Z). */
	UFUNCTION(BlueprintPure, Category = "LBA")
	float GetTopHeight() const;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Ladder; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/** Conveyor belt (type 7). */
UCLASS()
class LBA2RUNTIME_API ALBAEscalatorVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBAEscalatorVolume(const FObjectInitializer& ObjectInitializer);

	/** 1 north, 2 south, 4 east, 8 west. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Direction = 0;

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Escalator; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/** Wagon rail (type 9). */
UCLASS()
class LBA2RUNTIME_API ALBARailVolume : public ALBAZoneVolume
{
	GENERATED_BODY()

public:
	ALBARailVolume(const FObjectInitializer& ObjectInitializer);

	virtual ELBAZoneType GetZoneType() const override { return ELBAZoneType::Rail; }
	virtual void InitFromZone(const FLBASceneZoneData& Zone) override;
};

/**
 * Hit zone (type 8): Unreal's pain-causing volume, dealing the original's damage at its pace to whatever can
 * take damage (Twinsen has no health yet).
 */
UCLASS()
class LBA2RUNTIME_API ALBAHitVolume : public APainCausingVolume
{
	GENERATED_BODY()

public:
	ALBAHitVolume(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	TObjectPtr<ULBASceneAsset> Scene;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	int32 ZoneIndex = INDEX_NONE;

	/** Life points per hit (the original's Info1); one hit per PainInterval. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Damage = 0;

	void InitFromZone(const FLBASceneZoneData& Zone);
};
