#pragma once

#include "ChaosVehicleWheel.h"
#include "ChaosWheeledVehicleMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "LBACarSettings.h"
#include "LBAInteractable.h"
#include "LBACar.generated.h"

class ALBACharacter;
class UBoxComponent;
class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class ULBAEntityAsset;
class USkeletalMeshComponent;
class USpringArmComponent;
struct FInputActionValue;

/**
 * Where a kind of car's wheels and chassis are, from its LBA body (actor space: +X forward, +Y right, +Z up, the origin on
 * the ground under the car). One side's wheels; the other side mirrors them.
 */
struct LBA2RUNTIME_API FLBACarGeometry
{
	FVector FrontWheel = FVector::ZeroVector;
	FVector RearWheel = FVector::ZeroVector;
	float FrontRadius = 20.f;
	float RearRadius = 20.f;
	float WheelWidth = 15.f;
	/** The chassis box (the root that simulates): center and half size. */
	FVector ChassisCenter = FVector::ZeroVector;
	FVector ChassisExtent = FVector(50.f);
	bool bRearWheelDrive = true;

	float GetWheelbase() const { return float(FrontWheel.X - RearWheel.X); }
	FLBACarDrive ComputeDrive(const FLBACarTuning& Tuning) const;

	/**
	 * The buggy (BODY.HQR 219 / 14): front hubs (groups 4, 7) 50 cm ahead and 50 cm to each side, 16.25 cm wheels;
	 * rear (groups 9-12) 41.6 cm back, 22.5 cm wheels (BUGGY.CPP: x 400, z 400 / -350, wheels 260 and 360 across).
	 */
	static const FLBACarGeometry Buggy;
	/** The Rabibunny's scooter (BODY.HQR 148): no wheels showing; four small ones hidden under its skirt. */
	static const FLBACarGeometry Scooter;
};

/**
 * Chaos's wheeled vehicle on a plain primitive root (ALBACar's box chassis): Chaos builds its suspension constraints
 * only for a skeletal mesh root with a physics asset (FixupSkeletalMesh), so without them the wheels hold nothing up and
 * the chassis slides on the ground. This builds the same constraints for any root. It also opens the few settings Chaos
 * keeps protected to ALBACar.
 */
UCLASS(ClassGroup = "LBA")
class LBA2RUNTIME_API ULBACarMovementComponent : public UChaosWheeledVehicleMovementComponent
{
	GENERATED_BODY()

protected:
	virtual void FixupSkeletalMesh() override;
	virtual void OnDestroyPhysicsState() override;

private:
	void ReleaseRootSuspension();
	/** The suspension constraints built for a non-skeletal root (one per wheel). */
	TArray<FPhysicsConstraintHandle> RootSuspension;

public:
	/** Brakes applied with nothing pressed (coasting), as a share of full braking. */
	void SetIdleBrake(float Share) { IdleBrakeInput = Share; }
	/** S brakes, then backs up; the throttle brakes while backing up. */
	void SetArcadeBrakes() { bReverseAsBrake = true; bThrottleAsBrake = true; }
	/** Raw and applied inputs, target gear, sleeping (debug text). */
	FString DescribeInputs() const
	{
		return FString::Printf(TEXT("raw throttle %.2f brake %.2f steer %.2f, applied throttle %.2f brake %.2f steer %.2f handbrake %.2f, target gear %d, sleeping %d, simulating %d"),
			RawThrottleInput, RawBrakeInput, RawSteeringInput, ThrottleInput, BrakeInput, SteeringInput, HandbrakeInput, GetTargetGear(),
			VehicleState.bSleeping ? 1 : 0, UpdatedPrimitive && UpdatedPrimitive->IsSimulatingPhysics() ? 1 : 0);
	}
};

/** A car's wheel. Its geometry is the class defaults (Chaos reads them when it builds the car): one class per kind of wheel. */
UCLASS(Abstract)
class LBA2RUNTIME_API ULBACarWheel : public UChaosVehicleWheel
{
	GENERATED_BODY()

public:
	ULBACarWheel();

protected:
	void SetGeometry(float Radius, float Width, bool bFront, bool bDriven);
};

UCLASS()
class LBA2RUNTIME_API ULBABuggyFrontWheel : public ULBACarWheel
{
	GENERATED_BODY()
public:
	ULBABuggyFrontWheel();
};

UCLASS()
class LBA2RUNTIME_API ULBABuggyRearWheel : public ULBACarWheel
{
	GENERATED_BODY()
public:
	ULBABuggyRearWheel();
};

UCLASS()
class LBA2RUNTIME_API ULBAScooterFrontWheel : public ULBACarWheel
{
	GENERATED_BODY()
public:
	ULBAScooterFrontWheel();
};

UCLASS()
class LBA2RUNTIME_API ULBAScooterRearWheel : public ULBACarWheel
{
	GENERATED_BODY()
public:
	ULBAScooterRearWheel();
};

/**
 * A car Twinsen drives, with Unreal's vehicle physics (Chaos Vehicles): the buggy, LBA2's only car. Use it (E / gamepad
 * X) next to it to get in; W / right trigger accelerates, S / left trigger brakes then backs up, A/D or the left stick
 * steers, Space / gamepad A is the handbrake, mouse / right stick look around. Use again, slow, to get out.
 *
 * The root is a box chassis that simulates (no physics asset needed); four wheels without bones, placed from the body
 * (FLBACarGeometry). The LBA body rides on it without collision: the empty buggy while parked, the buggy with Twinsen
 * at the wheel while driven, its wheels, steering wheel and Twinsen's head turned as the original turns them
 * (BUGGY.CPP DoAnimBuggy). How it drives: Project Settings > Game > LBA2 Cars.
 *
 * Placed anywhere (even in the air, or before the ground under it has loaded), it waits for ground under it and sets
 * itself down there before simulating.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBACar : public APawn, public ILBAInteractable
{
	GENERATED_BODY()

public:
	ALBACar();

	/** Parked body. Empty: Project Settings > LBA2 > Mounts > Car. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Car")
	TObjectPtr<ULBAEntityAsset> Entity;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Car")
	int32 BodyType = 0;

	/** Body while Twinsen drives. Empty: Project Settings > LBA2 > Mounts > Car Driven. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Car")
	TObjectPtr<ULBAEntityAsset> DrivenEntity;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Car")
	int32 DrivenBodyType = 0;

	/** How fast the camera swings back behind the car (1/s): lower is lazier. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Camera")
	float CameraFollowSpeed = 3.f;

	/** Twinsen gets in, the player drives (Use next to it). */
	UFUNCTION(BlueprintCallable, Category = "LBA|Car")
	bool Board(ALBACharacter* Hero);

	/** Twinsen gets out next to the car, if it is slow and there is room. */
	UFUNCTION(BlueprintCallable, Category = "LBA|Car")
	bool Leave();

	/** Where Twinsen would get out (his capsule's center): the driver's side first, then the other, behind, ahead. */
	bool FindExitSpot(FVector& OutLocation) const;

	/** Drives like the player would: X steering (right), Y throttle (negative brakes, then backs up). */
	UFUNCTION(BlueprintCallable, Category = "LBA|Car")
	void SetDriveInput(FVector2D Drive) { DriveInput = Drive; }

	UFUNCTION(BlueprintCallable, Category = "LBA|Car")
	void SetHandbrake(bool bOn) { bHandbrakeInput = bOn; }

	UFUNCTION(BlueprintPure, Category = "LBA|Car")
	ALBACharacter* GetDriver() const { return Driver.Get(); }

	/** Speed along the heading (cm/s, negative backing up). */
	UFUNCTION(BlueprintPure, Category = "LBA|Car")
	float GetForwardSpeed() const;

	/** Set down on the ground and simulating. */
	bool IsSettled() const { return bSettled; }

	ULBACarMovementComponent* GetVehicleMovement() const { return VehicleMovement; }
	virtual UPawnMovementComponent* GetMovementComponent() const override;

	virtual const FLBACarGeometry& GetGeometry() const { return FLBACarGeometry::Buggy; }
	virtual const FLBACarTuning& GetTuning() const { return ULBACarSettings::Get().Buggy; }

	virtual bool CanInteract_Implementation(APawn* User) const override;
	virtual void Interact_Implementation(APawn* User) override;

protected:
	virtual void PreRegisterAllComponents() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;

	/** The wheel classes (front, rear): Chaos reads their defaults. */
	virtual TSubclassOf<UChaosVehicleWheel> GetFrontWheelClass() const;
	virtual TSubclassOf<UChaosVehicleWheel> GetRearWheelClass() const;
	/** The default entities (parked, driven) when none is set. */
	virtual ULBAEntityAsset* GetDefaultEntity(bool bDriven) const;
	/** Moves the body's wheels, steering wheel and the driver's head (the buggy's groups). */
	virtual void UpdateBodyPose(float DeltaSeconds);

	/** The body shown (driven or parked), with its idle animation. */
	void ApplyBody(bool bDriven);
	/** The bone of a group of the body shown (each body mesh's bones are its groups, in order). */
	FName GetGroupBone(int32 Group) const;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBoxComponent> Chassis;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	/** Where Twinsen sits (hidden: the driven body has him in it). */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Seat;

	/** Around the car: standing in it, Twinsen can use it. */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBoxComponent> Boarding;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<ULBACarMovementComponent> VehicleMovement;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	/** Rolled angle of the wheels (degrees), front and rear; steering shown (degrees, positive right). */
	float FrontWheelAngle = 0.f;
	float RearWheelAngle = 0.f;
	float ShownSteer = 0.f;
	/** How far the body is lowered (cm) to follow the wheels' suspension (it is drawn at its rest pose). */
	float BodyDrop = 0.f;

private:
	void ApplyVehicleSetup();
	void ApplyWheelTuning();
	bool TrySettle();
	void UpdateInputs(float DeltaSeconds);
	void UpdateBodyHeight(float DeltaSeconds);
	void UpdateSelfRighting(float DeltaSeconds);
	void UpdateCamera(float DeltaSeconds);
	void DrawDebug() const;
	void CreateInput();

	void OnDrive(const FInputActionValue& Value);
	void OnDriveReleased(const FInputActionValue& Value) { DriveInput = FVector2D::ZeroVector; }
	void OnTrigger(const FInputActionValue& Value);
	void OnTriggerReleased(const FInputActionValue& Value) { TriggerInput = 0.f; }
	void OnHandbrake(const FInputActionValue& Value) { bHandbrakeInput = true; }
	void OnHandbrakeReleased(const FInputActionValue& Value) { bHandbrakeInput = false; }
	void OnLook(const FInputActionValue& Value);
	void OnLeave(const FInputActionValue& Value) { Leave(); }

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InputContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> DriveAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> TriggerAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> HandbrakeAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LeaveAction;

	UPROPERTY(Transient)
	TWeakObjectPtr<ALBACharacter> Driver;

	FVector2D DriveInput = FVector2D::ZeroVector;
	float TriggerInput = 0.f;
	bool bHandbrakeInput = false;
	bool bSettled = false;
	bool bDrivenBody = false;
	double BoardTime = 0.0;
	double SettleStart = 0.0;
	float UpsideDownTime = 0.f;
	/** Camera following the heading, and the look offset (yaw, pitch) the player left around it. */
	FRotator CameraBase = FRotator::ZeroRotator;
	FVector2D LookOffset = FVector2D::ZeroVector;
	double LastLookTime = -100.0;
};

/**
 * The Rabibunny's scooter, Citadel's taxi: its driver stays on and Twinsen rides behind him, steering. The original
 * flies it between its stands on scripted paths; here it drives on the ground, on four wheels hidden under its skirt.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBAScooter : public ALBACar
{
	GENERATED_BODY()

public:
	ALBAScooter();

	virtual const FLBACarGeometry& GetGeometry() const override { return FLBACarGeometry::Scooter; }
	virtual const FLBACarTuning& GetTuning() const override { return ULBACarSettings::Get().Scooter; }

protected:
	virtual TSubclassOf<UChaosVehicleWheel> GetFrontWheelClass() const override;
	virtual TSubclassOf<UChaosVehicleWheel> GetRearWheelClass() const override;
	virtual ULBAEntityAsset* GetDefaultEntity(bool bDriven) const override;
	virtual void UpdateBodyPose(float DeltaSeconds) override;
};
