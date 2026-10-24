#pragma once

#include "Engine/DataAsset.h"
#include "LBAEntityAsset.generated.h"

class USkeletalMesh;
class UAnimSequence;

/** Well-known LBA2 animation slots (an entity maps each slot to an animation). */
UENUM(BlueprintType)
enum class ELBAAnimType : uint8
{
	Idle = 0,
	Forward = 1,
	Backward = 2,
	TurnLeft = 3,
	TurnRight = 4,
	Hit = 6,
	Falling = 7,
	FallLandingStumble = 8,
	FallLandingHeadHit = 9,
	Action = 11,
	ClimbUp = 12,
	LadderTopOut = 13,
	Jump = 14,
	Throw = 15,
	Crouch = 16,
	Punch = 17,
	FoundObject = 20,
	Drowning = 21,
	SwordAttack = 23,
	SwordDraw = 24,
	RunningJump = 25,
	Talk = 28,
	ThrowDart = 29,
	SlideForward = 33,
	SlideBackward = 34,
	BlowgunShoot = 35,
	WannieGloveSwing = 36,
	LaserPistolShoot = 38,
	DodgeRight = 40,
	DodgeLeft = 41,
	DodgeForward = 42,
	DodgeBackward = 43,
	DrowningLava = 44,
	DrowningStars = 47,
};

/**
 * A light given by a glowing part of a body (the spacecraft's roof): an unshadowed point light at Location (mesh space).
 * FalloffExponent 0: inverse-square falloff, Intensity in candelas; above 0: (1 - (d / Radius)^2)^FalloffExponent,
 * Intensity unitless (lux before the falloff).
 */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBABodyLight
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA", meta = (ClampMin = "0"))
	float Intensity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA", meta = (ClampMin = "0"))
	float Radius = 1000.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA", meta = (ClampMin = "0"))
	float SourceRadius = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA", meta = (ClampMin = "0"))
	float FalloffExponent = 0.f;
};

USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAEntityBody
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TObjectPtr<USkeletalMesh> Mesh;

	/** Index in BODY.HQR. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 SourceBody = INDEX_NONE;

	/** Collision box authored for this body (Unreal space, relative to the actor). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FBox CollisionBox = FBox(ForceInit);

	/** Lights of its glowing parts. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<FLBABodyLight> Lights;
};

USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAEntityAnim
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TObjectPtr<UAnimSequence> Sequence;

	/** Index in ANIM.HQR. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 SourceAnim = INDEX_NONE;

	/** Time (seconds) the original engine loops back to; 0 for anims that loop from the start. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	float LoopStartTime = 0.f;
};

/**
 * One LBA2 "entity" (RESS.HQR entry 44): the set of bodies (outfits, held weapons...) and the
 * animation for every animation slot of a character or object. Twinsen has one entity per behaviour
 * (normal, athletic, aggressive, discreet, protopack...) sharing the same bodies.
 */
UCLASS(BlueprintType)
class LBA2RUNTIME_API ULBAEntityAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FString DisplayName;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 EntityIndex = INDEX_NONE;

	/** Body type -> skeletal mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TMap<int32, FLBAEntityBody> Bodies;

	/** Animation type (see ELBAAnimType) -> animation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TMap<int32, FLBAEntityAnim> Anims;

	UFUNCTION(BlueprintPure, Category = "LBA")
	UAnimSequence* GetAnim(ELBAAnimType Type) const { return GetAnimByIndex(int32(Type)); }

	UFUNCTION(BlueprintPure, Category = "LBA")
	UAnimSequence* GetAnimByIndex(int32 Type) const
	{
		const FLBAEntityAnim* Anim = Anims.Find(Type);
		return Anim ? Anim->Sequence.Get() : nullptr;
	}

	UFUNCTION(BlueprintPure, Category = "LBA")
	USkeletalMesh* GetBodyMesh(int32 BodyType) const
	{
		const FLBAEntityBody* Body = Bodies.Find(BodyType);
		return Body ? Body->Mesh.Get() : nullptr;
	}

	/** First body (lowest body type), useful as a default. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	USkeletalMesh* GetDefaultBodyMesh() const;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("LBAEntity"), GetFName());
	}
};
