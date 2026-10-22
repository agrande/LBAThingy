#pragma once

#include "Components/ActorComponent.h"
#include "LBAHitPointsComponent.generated.h"

class ULineBatchComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FLBAHurtSignature, float, Damage, FVector, Location);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FLBADeathSignature);

/**
 * Makes an actor accept hits, the original's way: it has life points, and every hit that hurts sends stars flying out
 * of it (RESS.HQR POF 0, a white line-drawn star; one per DamagePerStar of damage, up to MaxStars), drawn in the world.
 * There is no life display: the stars (and the owner's own hit reaction) are the only feedback. Actors without this
 * component do not accept hits.
 */
UCLASS(ClassGroup = LBA, meta = (BlueprintSpawnableComponent))
class LBA2RUNTIME_API ULBAHitPointsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULBAHitPointsComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points", meta = (ClampMin = "1"))
	float MaxLife = 100.f;

	/** One star per this much damage (at least one per hit that hurts), up to MaxStars. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars", meta = (ClampMin = "0.1"))
	float DamagePerStar = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	int32 MaxStars = 5;

	/** Radius of a star (cm), and the width of its lines (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	float StarSize = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	float StarLineWidth = 3.f;

	/** Launch speed (cm/s, upward and outward), gravity (cm/s^2), spin (degrees/s) and life (s) of the stars. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	float StarSpeed = 400.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	float StarGravity = 700.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	float StarSpin = 220.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	float StarLife = 1.6f;

	/** Share of the owner's velocity the stars keep, so they don't vanish behind a fast flyer at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	float StarInheritVelocity = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit Points|Stars")
	FLinearColor StarColor = FLinearColor::White;

	/** A hit that hurts (Damage > 0, life left before it). */
	UPROPERTY(BlueprintAssignable, Category = "Hit Points")
	FLBAHurtSignature OnHurt;

	/** Life reached zero. */
	UPROPERTY(BlueprintAssignable, Category = "Hit Points")
	FLBADeathSignature OnDeath;

	/** Hurts the owner by Damage, stars flying out from Location. Ignored once dead. Returns true if it hurt. */
	UFUNCTION(BlueprintCallable, Category = "Hit Points")
	bool TakeHit(float Damage, FVector Location);

	UFUNCTION(BlueprintCallable, Category = "Hit Points")
	void Heal(float Amount);

	/** Back to full life (and alive). */
	UFUNCTION(BlueprintCallable, Category = "Hit Points")
	void Revive();

	/** Sends Count stars flying out from Location, without hurting. */
	UFUNCTION(BlueprintCallable, Category = "Hit Points")
	void SpawnStars(FVector Location, int32 Count);

	UFUNCTION(BlueprintPure, Category = "Hit Points")
	float GetLife() const { return Life; }

	UFUNCTION(BlueprintPure, Category = "Hit Points")
	bool IsDead() const { return Life <= 0.f; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	struct FStar
	{
		FVector Location;
		FVector Velocity;
		float Angle;
		float Spin;
		float Age;
	};
	void DrawStars();

	float Life = 100.f;
	TArray<FStar> Stars;

	/** World-space lines of the stars, redrawn every frame. */
	UPROPERTY(Transient)
	TObjectPtr<ULineBatchComponent> Lines;
};
