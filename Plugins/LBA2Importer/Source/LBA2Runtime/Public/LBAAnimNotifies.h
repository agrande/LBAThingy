#pragma once

#include "Animation/AnimNotifies/AnimNotify.h"
#include "LBAAnimNotifies.generated.h"

UENUM(BlueprintType)
enum class ELBAFoot : uint8
{
	Left,
	Right,
};

/** Footstep from the original animation (LEFT_STEP / RIGHT_STEP actions). Hook sounds/VFX in Blueprint. */
UCLASS(meta = (DisplayName = "LBA Footstep"))
class LBA2RUNTIME_API ULBAAnimNotify_Footstep : public UAnimNotify
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	ELBAFoot Foot = ELBAFoot::Left;

	virtual FString GetNotifyName_Implementation() const override
	{
		return Foot == ELBAFoot::Left ? TEXT("LBA Step L") : TEXT("LBA Step R");
	}
};

/** Damage frame from the original animation (HIT / SUPER_HIT actions). */
UCLASS(meta = (DisplayName = "LBA Hit"))
class LBA2RUNTIME_API ULBAAnimNotify_Hit : public UAnimNotify
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Strength = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	bool bSuperHit = false;

	virtual FString GetNotifyName_Implementation() const override { return FString::Printf(TEXT("LBA Hit %d"), Strength); }
};

/**
 * Any other gameplay action of the original animation (projectile throws, magic ball, shields...).
 * The raw parameters are kept so gameplay code can interpret them.
 */
UCLASS(meta = (DisplayName = "LBA Anim Action"))
class LBA2RUNTIME_API ULBAAnimNotify_Action : public UAnimNotify
{
	GENERATED_BODY()

public:
	/** Original action type (THROW = 8, THROW_MAGIC = 9, THROW_3D = 18...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 ActionType = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	FString ActionName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<uint8> Parameters;

	virtual FString GetNotifyName_Implementation() const override { return TEXT("LBA ") + ActionName; }
};
