#pragma once

#include "GameFramework/PawnMovementComponent.h"
#include "LBAFlightMovementComponent.generated.h"

/**
 * Simple but plausible winged flight (the Dinofly).
 *
 * The state is the velocity, a bank angle and an angle of attack; the heading always follows the velocity
 * (no sideslip), and the body is drawn at "velocity + angle of attack". Lift and drag come from a small
 * aerodynamic model (lift ~ speed^2 * angle of attack, stall, induced drag), so speed and altitude trade
 * naturally: diving builds speed, climbing bleeds it, flapping adds thrust (and hover lift at low speed).
 *
 * Inputs stay simple: the pitch input commands the angle of attack around a trim that glides hands-off
 * (like a real stick commands load), the roll input commands the bank, and turning comes from the banked
 * lift. Assists: more trim when banked (level-ish turns), a load factor limit, soft stall protection, and a
 * flight path limit that prevents loops.
 */
UCLASS(ClassGroup = Movement, meta = (BlueprintSpawnableComponent))
class LBA2RUNTIME_API ULBAFlightMovementComponent : public UPawnMovementComponent
{
	GENERATED_BODY()

public:
	ULBAFlightMovementComponent();

	/** Lift (and drag) per (cm/s)^2 per unit of coefficient: wing area * air density / mass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Aerodynamics")
	float LiftFactor = 0.0028f;

	/** Lift coefficient per radian of angle of attack. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Aerodynamics")
	float LiftSlope = 5.5f;

	/** Degrees. Beyond it lift falls off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Aerodynamics")
	float StallAngle = 16.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Aerodynamics")
	float ZeroLiftDrag = 0.04f;

	/** Drag coefficient per lift coefficient squared. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Aerodynamics")
	float InducedDrag = 0.09f;

	/** Hands-off angle of attack (degrees): sets the gliding speed (about 7.8 m/s with the defaults). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float TrimAngle = 6.f;

	/** Angle of attack at full pitch up / down (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float MaxAngle = 15.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float MinAngle = -6.f;

	/** How fast the angle of attack follows the input (1/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float AngleResponse = 5.f;

	/** Lift limits, in g. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float MaxLoadFactor = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float MinLoadFactor = -1.f;

	/** Degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float MaxBank = 50.f;

	/** How fast the bank follows the input (1/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float BankResponse = 3.f;

	/** Flight path limits (degrees): the pitch input fades out beyond them, so the Dinofly never loops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float MaxClimbAngle = 70.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Control")
	float MaxDiveAngle = 70.f;

	/** Forward acceleration while flapping (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Flapping")
	float FlapThrust = 420.f;

	/** Upward acceleration of flapping at zero speed (cm/s^2, above gravity to take off), fading out with speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Flapping")
	float FlapHoverLift = 1200.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Flapping")
	float HoverFadeSpeed = 450.f;

	/** Flapping loses its strength up to this height (cm), so the Dinofly stays around the islands. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Flapping")
	float MaxAltitude = 6000.f;

	/** Braking flares the wings: angle of attack (degrees) and extra drag coefficient. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Braking")
	float BrakeAngle = 13.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Braking")
	float BrakeDrag = 0.25f;

	/** Touching down on walkable ground slower than this (cm/s, vertical and total) lands instead of bouncing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float MaxLandingSinkSpeed = 700.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float MaxLandingSpeed = 1300.f;

	/**
	 * The Dinofly never goes under water (Water plugin bodies): touching the surface slowly lands on it (it floats,
	 * its center this high above the surface, in cm), faster skims off it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float WaterlineHeight = 30.f;

	/** Degrees per second when turning on the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float GroundTurnRate = 120.f;

	/** Velocity given by a takeoff (cm/s, forward and up). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	FVector2D TakeoffVelocity = FVector2D(250.f, 450.f);

	/** Pitch [-1 nose down, 1 nose up], roll [-1 left, 1 right], flap and brake [0, 1]. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void SetFlightInput(float InPitch, float InRoll, float InFlap, float InBrake);

	/** Starts flying (with an initial forward speed, cm/s). */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void Launch(float ForwardSpeed);

	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsLanded() const { return bLanded; }

	/** Landed on water rather than on the ground. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsFloating() const { return bLanded && bOnWater; }

	/** Degrees, positive to the right. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetBank() const { return FMath::RadiansToDegrees(Bank); }

	/** Degrees. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetAngleOfAttack() const { return FMath::RadiansToDegrees(Alpha); }

	/** Degrees, positive when climbing. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetFlightPathAngle() const;

	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsStalling() const { return !bLanded && Alpha > FMath::DegreesToRadians(StallAngle); }

	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetFlapInput() const { return Flap; }

	/** Speed of level flight at the trim angle (cm/s). */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetTrimSpeed() const;

	/** Horizontal direction of travel (or facing, when still). */
	FVector GetHeading() const { return Heading; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual float GetMaxSpeed() const override { return 4000.f; }
	virtual bool IsFalling() const override { return !bLanded; }
	virtual bool IsMovingOnGround() const override { return bLanded; }
	virtual bool IsFlying() const override { return !bLanded; }

protected:
	virtual void InitializeComponent() override;

private:
	void Simulate(float DeltaTime);
	float LiftCoefficient(float AngleOfAttack) const;
	FQuat ComputeBodyRotation() const;
	void TickAirborne(float DeltaTime);
	void TickLanded(float DeltaTime);
	void TouchDown(const FHitResult& Hit);
	/** Height of the water surface (with waves) at Location, if there is water there. */
	bool FindWaterSurface(const FVector& Location, float& OutSurfaceZ) const;
	/** Lands on or skims off the water when below the waterline. */
	void HandleWater();
	void TickFloating(float DeltaTime);

	float Pitch = 0.f;
	float Roll = 0.f;
	float Flap = 0.f;
	float Brake = 0.f;

	bool bLanded = false;
	bool bOnWater = false;
	/** Radians. */
	float Bank = 0.f;
	float Alpha = 0.f;
	FVector Heading = FVector::ForwardVector;
};
