#pragma once

#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "LBAInteractable.h"
#include "LBABoat.generated.h"

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
 * A small motorboat's motion on the water. The throttle pushes it along its heading against the water's drag, which
 * grows with the speed until they balance at MaxSpeed; throttle against the motion brakes, then backs up slowly. The
 * rudder barely turns it when still and up to MaxTurnRate under way, as tight as MaxTurnLoad allows at speed; the keel
 * kills most of the sideways slide (less Grip drifts more in turns). It floats with its origin at the surface (the
 * planet's sea, or Water bodies) and runs aground where its hull meets the seabed. Kinematic: it sweeps its updated
 * component and slides along what it hits, losing the speed it had into it.
 */
UCLASS(ClassGroup = "LBA", meta = (BlueprintSpawnableComponent))
class LBA2RUNTIME_API ULBABoatMovementComponent : public UPawnMovementComponent
{
	GENERATED_BODY()

public:
	/** Top speed (cm/s), at full throttle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "100"))
	float MaxSpeed = 3000.f;

	/** Top speed backing up (cm/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0"))
	float ReverseSpeed = 500.f;

	/** Full throttle's push (cm/s^2): the drag takes it over towards MaxSpeed (0 to 25 m/s in about 4 s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0"))
	float Thrust = 1100.f;

	/** Throttle against the motion brakes this hard (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0"))
	float BrakeThrust = 800.f;

	/** Part of the drag proportional to the speed (1/s); the rest of it at MaxSpeed grows with its square. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0"))
	float LinearDrag = 0.15f;

	/** Drag bringing it to a stop once slow (cm/s^2), throttle off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0"))
	float StopDrag = 80.f;

	/** Turn rate with the rudder hard over (degrees/s), under way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0"))
	float MaxTurnRate = 75.f;

	/** Share of MaxTurnRate when still (the outboard pivots), full from TurnSpeed (cm/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0", ClampMax = "1"))
	float StillTurn = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "1"))
	float TurnSpeed = 600.f;

	/** Sideways acceleration (cm/s^2) it turns with at most: at speed v the turn rate is at most MaxTurnLoad / v. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "1"))
	float MaxTurnLoad = 1100.f;

	/** How fast the turn rate follows the rudder (1/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0.1"))
	float TurnResponse = 4.f;

	/** How fast the keel kills the sideways slide (1/s): lower drifts more in turns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0"))
	float Grip = 2.5f;

	/** How fast it settles at the surface (1/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0.1"))
	float FloatResponse = 8.f;

	/** Share of the speed into what it hits that it bounces back with. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boat", meta = (ClampMin = "0", ClampMax = "1"))
	float Bounce = 0.25f;

	/** Throttle and rudder in [-1, 1] (rudder positive turns right, the way a car steers when backing up too). */
	void SetInput(float InThrottle, float InRudder);

	/** Speed along the heading (cm/s, negative backing up). */
	UFUNCTION(BlueprintPure, Category = "Boat")
	float GetForwardSpeed() const;

	/** Turning (degrees/s, positive to the right). */
	UFUNCTION(BlueprintPure, Category = "Boat")
	float GetTurnRate() const { return TurnRate; }

	/** Smoothed acceleration (cm/s^2, world, horizontal). */
	FVector GetAcceleration() const { return Acceleration; }

	/** On water (found under it at the last update). */
	UFUNCTION(BlueprintPure, Category = "Boat")
	bool IsAfloat() const { return bAfloat; }

	/** Moving, or steered by someone: still boats don't update. */
	bool IsUnderWay() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual float GetMaxSpeed() const override { return MaxSpeed; }

private:
	void Simulate(float DeltaTime);

	float Throttle = 0.f;
	float Rudder = 0.f;
	float TurnRate = 0.f;
	FVector Acceleration = FVector::ZeroVector;
	bool bAfloat = false;
};

/**
 * A boat Twinsen steers across the sea: the ferry, moored where the islands had theirs (the island layer replaces the
 * islands' copies of its body with boats). Use it (E / gamepad X) standing next to it to get on: Twinsen goes into the
 * cabin and the player drives. W/S or the left stick's vertical, or the triggers = throttle (backing up after a stop),
 * A/D or the stick's horizontal = rudder. Mouse / right stick look around; the camera swings back behind the boat with
 * its turns and draws back with the speed. Use again, slow and next to land, to step ashore. The hull leans back with
 * the speed and the acceleration, into turns, and rocks.
 *
 * Its origin is at the waterline; the hull's box reaches below it to the keel, so the boat runs aground in the
 * shallows. The body is the entity of Project Settings > LBA2 > Mounts > Boat (the ferry) unless Entity is set.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBABoat : public APawn, public ILBAInteractable
{
	GENERATED_BODY()

public:
	ALBABoat();

	/** Entity providing the body and its idle animation. Empty uses Project Settings > LBA2 > Mounts > Boat. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat")
	TObjectPtr<ULBAEntityAsset> Entity;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat")
	int32 BodyType = 0;

	/** Where the body sits on the boat (origin at the waterline, bow along +X; LBA bodies face +Y). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat")
	FTransform MeshTransform = FTransform(FRotator(0.f, -90.f, 0.f));

	/** Where Twinsen stands at the helm (his feet; boat space, bow along +X). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat")
	FVector HelmLocation = FVector(0.f, 0.f, 60.f);

	/** Twinsen is inside (the ferry's cabin): hidden while on board. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat")
	bool bRiderInside = true;

	/** Faster than this (cm/s), Twinsen stays on board. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat", meta = (ClampMin = "0"))
	float MaxLeaveSpeed = 300.f;

	/** Twinsen steps ashore onto ground at most this far from the hull (cm) and this deep under the water (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat", meta = (ClampMin = "0"))
	float LeaveReach = 400.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat", meta = (ClampMin = "0"))
	float MaxWadeDepth = 60.f;

	/** Rocking on the water at rest (degrees), and its slap at full speed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat", meta = (ClampMin = "0"))
	float Rocking = 1.f;

	/** Bow up when planing (degrees) and at full speed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Boat", meta = (ClampMin = "0"))
	float PlaningPitch = 3.f;

	/** How fast the camera swings back behind the boat when it turns (1/s): lower is lazier. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Camera")
	float CameraFollowSpeed = 3.f;

	/** Twinsen gets on, the player steers (Use next to it). */
	UFUNCTION(BlueprintCallable, Category = "LBA|Boat")
	bool Board(ALBACharacter* Hero);

	/** Twinsen steps ashore next to the boat, if it is slow and land is near enough. */
	UFUNCTION(BlueprintCallable, Category = "LBA|Boat")
	bool Leave();

	/** Where Twinsen would step ashore (his capsule's center), if there is ground near enough. */
	bool FindLandingSpot(FVector& OutLocation) const;

	/** Steers like the player would: X rudder (right), Y throttle. */
	UFUNCTION(BlueprintCallable, Category = "LBA|Boat")
	void SetDriveInput(FVector2D Drive) { DriveInput = Drive; }

	UFUNCTION(BlueprintPure, Category = "LBA|Boat")
	ALBACharacter* GetRider() const { return Rider.Get(); }

	ULBABoatMovementComponent* GetBoatMovement() const { return Movement; }
	virtual UPawnMovementComponent* GetMovementComponent() const override;

	virtual bool CanInteract_Implementation(APawn* User) const override;
	virtual void Interact_Implementation(APawn* User) override;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;

private:
	void ApplyEntity();
	void CreateInput();
	void UpdateHull(float DeltaSeconds);
	void UpdateCamera(float DeltaSeconds);
	void DrawDebug() const;

	void OnDrive(const FInputActionValue& Value);
	void OnDriveReleased(const FInputActionValue& Value) { DriveInput = FVector2D::ZeroVector; }
	void OnTrigger(const FInputActionValue& Value);
	void OnTriggerReleased(const FInputActionValue& Value) { TriggerInput = 0.f; }
	void OnLook(const FInputActionValue& Value);
	void OnLeave(const FInputActionValue& Value) { Leave(); }

	/** The hull, from the keel to the deck (the root: it floats and collides). */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBoxComponent> Collision;

	/** Up to the roof: stops walkers and cameras (the hull's box alone sweeps). */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBoxComponent> Cabin;

	/** Leans and rocks with the boat's motion, carrying the body and Twinsen. */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Hull;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> Helm;

	/** Around the hull: standing in it in front of the boat, Twinsen can use it. */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UBoxComponent> Boarding;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<ULBABoatMovementComponent> Movement;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InputContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> DriveAction;

	/** Gamepad triggers: right = throttle, left = back. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> TriggerAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LeaveAction;

	UPROPERTY(Transient)
	TWeakObjectPtr<ALBACharacter> Rider;

	FVector2D DriveInput = FVector2D::ZeroVector;
	float TriggerInput = 0.f;
	double BoardTime = 0.0;
	/** The hull's lean (pitch, roll; degrees) and heave (cm), eased; a clock for the rocking. */
	FVector2D Lean = FVector2D::ZeroVector;
	float Heave = 0.f;
	float RockClock = 0.f;
	/** Camera following the heading, and the look offset (yaw, pitch) the player left around it. */
	FRotator CameraBase = FRotator::ZeroRotator;
	FVector2D LookOffset = FVector2D::ZeroVector;
};
