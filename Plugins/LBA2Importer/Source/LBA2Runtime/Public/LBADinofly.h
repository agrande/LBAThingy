#pragma once

#include "GameFramework/Pawn.h"
#include "LBADinofly.generated.h"

class ALBACharacter;
class ULBAAirflowComponent;
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
 * Rideable Dinofly (prototype): Twinsen gets on it (on the ground), rides it with simple flight controls, and
 * gets off once landed. The default body already has Twinsen on its back, so the hero is hidden while riding.
 *
 * On the ground (or the water) W/S and A/D walk slowly; climbing takes off after a crouch. Touching the ground
 * lands at once: the legs come down while descending, the landing absorbs the touchdown.
 *
 * Hover mode (default): W/S or left stick = forward / backward, A/D = turn, Space / A / right trigger = climb,
 * Ctrl / C / B / left trigger = descend (into the ground or the sea to land). The higher, the faster it cruises.
 * Shift / X / right bumper held = dive (forward steeper, backward shallower, turning spirals down); releasing pulls
 * out with the speed gained (climbing meanwhile zooms up). It pulls out by itself before hitting anything.
 * Glide mode (Tab / View): W/S pitch (W dives unless lba.Dinofly.InvertPitch 1), A/D bank, climb = flap,
 * descend = brake. The camera follows behind, rising and looking down when descending or high above the
 * ground; its field of view widens with speed, accelerations and altitude changes. Mouse / right stick look
 * all around; the view stays where it was left. F / Y = get off (when landed on the ground).
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

	/** How fast the camera swings back behind the Dinofly when it turns (1/s): lower is lazier. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Camera")
	float CameraFollowSpeed = 4.f;

	/** Height above the ground (cm) where the camera is fully raised, looking down by up to MaxCameraDrop degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Camera")
	float HighAltitude = 3000.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Camera")
	float MaxCameraDrop = 25.f;

	/** A waiting Dinofly closer than this (cm) is mounted directly instead of flying in. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	float MountDistance = 500.f;

	/**
	 * Mounts a waiting Dinofly next to the hero where it stands, or brings one (spawning it if needed) to the hero's
	 * spot on the ground. The hero is hidden (the ridden body includes Twinsen) and the Dinofly takes the controller.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA", meta = (DeterminesOutputType = "DinoflyClass"))
	static ALBADinofly* Summon(ALBACharacter* Hero, TSubclassOf<ALBADinofly> DinoflyClass);

	/** Puts the rider back on the ground next to the Dinofly, which stays there waiting. Only when landed on the ground. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool Dismount();

	/** Drives the Dinofly like the player would: move (X turn right, Y forward), vertical [-1, 1]. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetHoverInput(FVector2D Move, float Vertical);

	/** Dive while held (hover mode, in flight). */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetDiveInput(bool bDive);

	/** Glide mode equivalent: pitch (positive nose up), roll in [-1, 1], flap and brake. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetFlightInput(float Pitch, float Roll, bool bFlap, bool bBrake);

	/** Switches between hover (default) and glide. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void ToggleFlightMode();

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
	void UpdateAnimation(float DeltaSeconds);
	/** Plays a one-shot animation slot from StartTime until EndTime (backwards when PlayRate < 0). */
	void PlayAction(int32 AnimType, float StartTime, float EndTime, float PlayRate);
	/** Plays an animation slot on some bone branches only, over the current animation. */
	void PlayLayer(int32 AnimType, const TArray<FName>& Bones, float StartTime, float PlayRate, float BlendTime);
	void UpdateCamera(float DeltaSeconds);
	void PlaySlot(int32 AnimType, float PlayRate = 1.f, float BlendTime = 0.25f);
	ULBAAnimInstance* GetLBAAnimInstance() const;

	void OnSteer(const FInputActionValue& Value);
	void OnSteerReleased(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnAscend(const FInputActionValue& Value) { bAscendHeld = true; }
	void OnAscendReleased(const FInputActionValue& Value) { bAscendHeld = false; }
	void OnDescend(const FInputActionValue& Value) { bDescendHeld = true; }
	void OnDescendReleased(const FInputActionValue& Value) { bDescendHeld = false; }
	void OnDive(const FInputActionValue& Value) { bDiveHeld = true; }
	void OnDiveReleased(const FInputActionValue& Value) { bDiveHeld = false; }
	void OnToggleMode(const FInputActionValue& Value) { ToggleFlightMode(); }
	void OnDismount(const FInputActionValue& Value) { Dismount(); }
	bool IsHovering() const;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<ULBAFlightMovementComponent> Flight;

	/** Airflow strokes around the body (hotspots, look and physics in its details). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<ULBAAirflowComponent> Airflow;

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

	/** Climb in hover mode, flap in glide mode. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> AscendAction;

	/** Descend in hover mode, brake in glide mode. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> DescendAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> DiveAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ModeAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> DismountAction;

	/** The hero hidden while riding. */
	UPROPERTY(Transient)
	TWeakObjectPtr<ALBACharacter> Rider;

	FVector2D SteerInput = FVector2D::ZeroVector;
	bool bAscendHeld = false;
	bool bDescendHeld = false;
	bool bDiveHeld = false;
	/** The wings are folded (a layer) for the dive. */
	bool bWingsTucked = false;
	/** Camera state: how far it is raised and tilted down (0-1), and the smoothed forward acceleration (cm/s^2). */
	float CameraDrop = 0.f;
	/** One-shot animation (landing, takeoff, legs coming down as a layer) running, or INDEX_NONE. */
	int32 ActionSlot = INDEX_NONE;
	float ActionEndTime = 0.f;
	float ActionRate = 1.f;
	bool bWasLanded = false;
	float LastYaw = 0.f;
	float SmoothedAcceleration = 0.f;
	float LastHorizontalSpeed = 0.f;
	/** Orbit (yaw, pitch) around the camera following the heading; kept until the player looks elsewhere. */
	FVector2D LookOffset = FVector2D::ZeroVector;
	FRotator CameraBase = FRotator::ZeroRotator;
};
