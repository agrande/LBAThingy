#pragma once

#include "GameFramework/Pawn.h"
#include "LBADinofly.generated.h"

class ALBACharacter;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class ULBAAnimInstance;
class ULBAEntityAsset;
class ULBAFlightMovementComponent;
class USkeletalMeshComponent;
class USphereComponent;
class USpringArmComponent;
struct FInputActionValue;

/**
 * Rideable Dinofly (prototype): Twinsen summons it, rides it with simple flight controls, and gets off
 * once landed. The default body already has Twinsen on its back, so the hero is hidden while flying.
 *
 * Controls: W/S or stick = pitch (W dives, like a flight stick; lba.Dinofly.InvertPitch 1 flips it),
 * A/D = bank and turn, Space / A / right trigger = flap (climb, speed up, take off), Shift / left trigger
 * = brake (flare to slow down and land), mouse / right stick = look, F / Y = get off (when landed).
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBADinofly : public APawn
{
	GENERATED_BODY()

public:
	ALBADinofly();

	/** Entity providing the body and the animations. Empty uses Project Settings > Plugins > LBA2 > Dinofly. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TObjectPtr<ULBAEntityAsset> Entity;

	/** Body type of the entity: 0 = Dinofly with Twinsen, 37 = Dinofly alone, 116 = with Twinsen in the wizard tunic. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 BodyType = 0;

	/** Body shown while the Dinofly waits without its rider. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 RiderlessBodyType = 37;

	/** Height above the hero where a summoned Dinofly appears, already flying. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	float SummonHeight = 1000.f;

	/** A waiting Dinofly closer than this (cm) is mounted directly instead of flying in. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	float MountDistance = 500.f;

	/**
	 * Mounts a waiting Dinofly next to the hero, or brings one (spawning it if needed) flying above the hero.
	 * The hero is hidden (the ridden body includes Twinsen) and the Dinofly takes the hero's controller.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA", meta = (DeterminesOutputType = "DinoflyClass"))
	static ALBADinofly* Summon(ALBACharacter* Hero, TSubclassOf<ALBADinofly> DinoflyClass);

	/** Puts the rider back on the ground next to the Dinofly, which stays there waiting. Only when landed on the ground. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool Dismount();

	/** Drives the Dinofly like the player would (pitch, roll in [-1, 1], flap and brake in [0, 1]). */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetFlightInput(float Pitch, float Roll, bool bFlap, bool bBrake);

	ULBAFlightMovementComponent* GetFlight() const { return Flight; }
	virtual UPawnMovementComponent* GetMovementComponent() const override;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;
	virtual void PossessedBy(AController* NewController) override;

private:
	void Mount(ALBACharacter* Hero);
	void ApplyEntity();
	void CreateInput();
	void UpdateAnimation();
	void UpdateCamera(float DeltaSeconds);
	void PlaySlot(int32 AnimType, float PlayRate = 1.f, float BlendTime = 0.25f);
	ULBAAnimInstance* GetLBAAnimInstance() const;

	void OnSteer(const FInputActionValue& Value);
	void OnSteerReleased(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnFlap(const FInputActionValue& Value) { bFlapHeld = true; }
	void OnFlapReleased(const FInputActionValue& Value) { bFlapHeld = false; }
	void OnBrake(const FInputActionValue& Value) { bBrakeHeld = true; }
	void OnBrakeReleased(const FInputActionValue& Value) { bBrakeHeld = false; }
	void OnDismount(const FInputActionValue& Value) { Dismount(); }

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<ULBAFlightMovementComponent> Flight;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InputContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SteerAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> FlapAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> BrakeAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> DismountAction;

	/** The hero hidden while riding. */
	UPROPERTY(Transient)
	TWeakObjectPtr<ALBACharacter> Rider;

	FVector2D SteerInput = FVector2D::ZeroVector;
	bool bFlapHeld = false;
	bool bBrakeHeld = false;
	/** Mouse / right stick orbit (yaw, pitch), eased back behind the Dinofly after a while. */
	FVector2D LookOffset = FVector2D::ZeroVector;
	float LastLookTime = -100.f;
	FRotator CameraBase = FRotator::ZeroRotator;
};
