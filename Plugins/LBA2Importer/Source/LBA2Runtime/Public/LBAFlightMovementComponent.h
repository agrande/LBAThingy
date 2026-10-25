#pragma once

#include "GameFramework/PawnMovementComponent.h"
#include "LBAFlightMovementComponent.generated.h"

UENUM(BlueprintType)
enum class ELBAFlightMode : uint8
{
	/** Simplified helicopter: holds position and altitude hands-off, moves where asked, body kept level. */
	Hover,
	/** Winged glider: speed and altitude trade, pitch and bank control (see below). */
	Glide,
};

UENUM(BlueprintType)
enum class ELBAFlightImpact : uint8
{
	/** Walkable ground (landing). */
	Ground,
	/** Anything else: walls, trees, rocks, from any side. */
	Obstacle,
	Water,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FLBAFlightImpactSignature, float, ImpactSpeed, ELBAFlightImpact, Kind, FVector, Location);

/**
 * Winged flight (the Dinofly), in two modes.
 *
 * Hover (default): steered like a simplified helicopter, held up like a bird. Forward/backward and turn: the
 * horizontal velocity and the turn rate ease toward the requested ones with limited accelerations (inertia without
 * piloting). Vertically, it glides down slowly between beats, but a few seconds without one (and without the speed to
 * carry its weight) it stalls and falls faster and faster, up to a speed that hurts; each press of climb is a wing beat
 * (Beat): a punch that fades quickly, then a long tail, and it recovers from a stall. Pumping slowly holds the
 * altitude, faster climbs, with decreasing returns. Braking (held, and taps: BrakeTap) kills the speed, vertical and
 * horizontal, until the wings tire (BrakeEndurance). Flying forward lifts, more in the thick air near the ground; the
 * air thins out with altitude (ULBADinoflySettings has the values and the design). The body leans the way the flight's
 * forces lean it (path, accelerations, turns), following like a spring. Touching walkable ground (or the water) lands
 * at once, from any direction; at speed, turns are banked and wider, and the extra speed is lost slowly. Holding the
 * dive tucks the wings and dives, the longer the steeper (turning spirals down); releasing it pulls out to level flight
 * with the speed gained, or zooms up when climbing. Every hit (landing, bumping, the water) reports its speed into the
 * surface (OnImpact).
 *
 * On the ground (both modes) the inputs walk slowly (and paddle on water); only climbing (hover) or flapping
 * (glide) takes off, after a short crouch (TakeoffDelay) for the push-off animation.
 *
 * Glide: the state is the velocity, a bank angle and an angle of attack; the heading always follows the velocity
 * (no sideslip), and the body is drawn at "velocity + angle of attack". Lift and drag come from a small
 * aerodynamic model (lift ~ speed^2 * angle of attack, stall, induced drag), so speed and altitude trade
 * naturally: diving builds speed, climbing bleeds it, flapping adds thrust (and hover lift at low speed).
 *
 * Inputs stay simple: the pitch input commands the angle of attack around a trim that glides hands-off
 * (like a real stick commands load), the roll input commands the bank, and turning comes from the banked
 * lift. Assists: more trim when banked (level-ish turns), a load factor limit, soft stall protection, and a
 * flight path limit that prevents loops.
 *
 * Wind (ULBAWindSubsystem): flying, all of the above happens in the air, and the air carries the Dinofly along
 * (Velocity = its motion through the air + the drift the wind gives it). Hands-off it drifts with the wind; holding a
 * place means flying against it. Floating, the wind pushes it gently along the water; on the ground, not at all.
 */
UCLASS(ClassGroup = Movement, meta = (BlueprintSpawnableComponent))
class LBA2RUNTIME_API ULBAFlightMovementComponent : public UPawnMovementComponent
{
	GENERATED_BODY()

public:
	ULBAFlightMovementComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight")
	ELBAFlightMode Mode = ELBAFlightMode::Hover;

	/** Speed above the cruise is lost this slowly (cm/s^2) unless pulling back: drag, not braking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float CoastDeceleration = 250.f;

	/** Sideways acceleration of turns (g): at speed the turn rate drops below HoverTurnRate, turns get wider. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float MaxTurnLoad = 2.f;

	/**
	 * Horizontal acceleration when speeding up and when slowing down (cm/s^2), at the reference air density: the
	 * thicker the air, the more the wings bite (x sqrt(density), within 0.5 - 1.5).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float HoverAcceleration = 450.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float HoverDeceleration = 650.f;

	/** Dead, the body drops (cm/s, the fastest fall) limp, on its side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float DeadFallSpeed = 2500.f;

	/** Backward speed, as a fraction of the forward one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float BackwardSpeedFactor = 0.5f;

	/** Degrees per second at full turn input, and how fast the turn rate builds up (degrees/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float HoverTurnRate = 120.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Hover")
	float HoverTurnAcceleration = 360.f;

	/** Speed of a vertical dive (cm/s), where drag balances gravity: 36 m/s, about 130 km/h. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Dive")
	float DiveTerminalSpeed = 3600.f;

	/** Wing beats into the dive (cm/s^2), fading out toward DiveTerminalSpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Dive")
	float DiveFlapAcceleration = 400.f;

	/** How fast the path tips over into the dive (degrees/s), and the load (g) curving it at speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Dive")
	float DivePitchRate = 90.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Dive")
	float PullOutLoad = 3.f;

	/**
	 * Releasing the dive pulls out to level flight, keeping the speed; climbing meanwhile zooms up at this angle
	 * (degrees), trading the speed for height, until back to cruising speed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Dive")
	float ZoomClimbAngle = 35.f;

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
	 * The Dinofly never goes under water (Water plugin bodies, or a planet's sea): touching the surface slowly lands on it (it floats,
	 * its center this high above the surface, in cm), faster skims off it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float WaterlineHeight = 30.f;

	/** Degrees per second when turning on the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float GroundTurnRate = 90.f;

	/** Walking speed on the ground (cm/s; backward at half, paddling on water at 60%), and its acceleration (cm/s^2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float WalkSpeed = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float WalkAcceleration = 400.f;

	/** Seconds crouching on the ground between asking to take off and leaving it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float TakeoffDelay = 0.6f;

	/** Velocity given by a takeoff (cm/s, forward and up). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	FVector2D TakeoffVelocity = FVector2D(250.f, 450.f);

	/** How much of the wind carries the Dinofly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Wind")
	float WindExposure = 1.f;

	/**
	 * How quickly its drift follows the wind (1/s), horizontally and vertically: gusts push it over about a second,
	 * air pockets drop it at once.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Wind")
	FVector2D WindResponse = FVector2D(1.5f, 4.f);

	/** Fraction of the wind that pushes it along the water when floating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Wind")
	float FloatingWindDrift = 0.15f;

	/** Glide mode input: pitch [-1 nose down, 1 nose up], roll [-1 left, 1 right], flap and brake [0, 1]. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void SetFlightInput(float InPitch, float InRoll, float InFlap, float InBrake);

	/**
	 * Hover mode input: forward [-1 backward, 1 forward], turn [-1 left, 1 right], vertical [-1 descend, 1 climb] held
	 * (holding descend sinks faster; holding climb takes off, and beats if ULBADinoflySettings::HeldBeatInterval).
	 */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void SetHoverInput(float InForward, float InTurn, float InVertical);

	/** Hover mode: one wing beat (a press of climb). On the ground, takes off. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void Beat();

	/** Hover mode: a tap of the brake (descend): brakes at least ULBADinoflySettings::BrakeTapTime. Held: SetHoverInput. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void BrakeTap();

	/** Hover mode: how hard it brakes now (0..1), what is left of the brake's endurance (0..1), and whether it is spent. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetBrakeAmount() const { return BrakeAmount; }

	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetBrakeStamina() const { return BrakeStamina; }

	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsBrakeExhausted() const { return bBrakeExhausted; }

	/** Hover mode: how aggressive the dive is (0..1, from how long the dive is held). */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetDiveAggression() const;

	/** Hover mode: how far the wings gave up (0 gliding, 1 falling like a stone). */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetStall() const { return Stall; }

	/** Smoothed acceleration (cm/s^2) of the flight, as the body feels it. */
	FVector GetFlightAcceleration() const { return SmoothedAcceleration; }

	/** Dead: no more control, the body drops and lies where it falls. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void SetDead(bool bInDead);

	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsDead() const { return bDead; }

	/** Wing beats so far (changes on every beat), and seconds since the last one. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	int32 GetBeatCount() const { return BeatCount; }

	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetTimeSinceBeat() const { return TimeSinceBeat; }

	/** Air density (1 at the reference altitude) where the Dinofly is. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetAirDensity() const;

	/** Height above the sea level (cm): world Z, or above a planet's sea in globe mode. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetAltitude() const;

	/** Every hit: speed into the surface (cm/s), what was hit, where. */
	UPROPERTY(BlueprintAssignable, Category = "Flight")
	FLBAFlightImpactSignature OnImpact;

	/** Hover mode: dive while held (pressing starts a dive, in flight only); releasing pulls out. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void SetDiveInput(bool bInDive);

	UFUNCTION(BlueprintCallable, Category = "Flight")
	void SetMode(ELBAFlightMode NewMode);

	/** Starts hovering in place. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void StartHovering();

	/** Starts standing on the ground, facing the owner's forward. */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void StartLanded();

	/** Starts flying (with an initial forward speed, cm/s). */
	UFUNCTION(BlueprintCallable, Category = "Flight")
	void Launch(float ForwardSpeed);

	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsLanded() const { return bLanded; }

	/** Landed on water rather than on the ground. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsFloating() const { return bLanded && bOnWater; }

	/** Crouching to take off (still on the ground). */
	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsTakingOff() const { return bLanded && TakeoffCountdown >= 0.f; }

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

	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetVerticalInput() const { return Vertical; }

	/** Hover mode: speed of forward flight (cm/s), ULBADinoflySettings::CruiseSpeed. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetCruiseSpeed() const;

	/** Hover mode: diving or pulling out of a dive. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsDiving() const { return DivePhase != EDivePhase::None; }

	UFUNCTION(BlueprintPure, Category = "Flight")
	bool IsPullingOut() const { return DivePhase == EDivePhase::PullingOut; }

	/** Height above the ground or the water below (cm), up to MaxDistance. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetHeightAboveSurface(float MaxDistance) const;

	/** Hover mode: degrees per second the heading is turning (positive to the right). */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetTurnRate() const { return TurnRate; }

	/** Speed of level flight at the trim angle (cm/s). */
	UFUNCTION(BlueprintPure, Category = "Flight")
	float GetTrimSpeed() const;

	/** Velocity the wind gives the Dinofly (cm/s), part of Velocity. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	FVector GetWindDrift() const { return WindDrift; }

	/** Motion through the air (cm/s): Velocity without the wind's drift. */
	UFUNCTION(BlueprintPure, Category = "Flight")
	FVector GetAirVelocity() const { return Velocity - WindDrift; }

	/** Horizontal direction of travel (or facing, when still). */
	FVector GetHeading() const { return Heading; }

	/** The world was moved with the Dinofly (globe mode, ULBAPlanetSubsystem): turns its directions along. */
	void OnWorldMoved(const FTransform& Delta);

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
	void TickHover(float DeltaTime);
	void TickDive(float DeltaTime, float Height);
	/** Hover mode: turns the heading from the turn input (limited by MaxTurnLoad at that speed); degrees turned. */
	float UpdateTurn(float DeltaTime, float HorizontalSpeed);
	/** Something in the way of the Dinofly's shape moving by Delta. */
	bool IsBlocked(const FVector& Delta) const;
	/** Ground and water: counts the takeoff crouch down; true once it took off (the caller stops). */
	bool TickTakeoff(float DeltaTime);
	/** Ground and water: walking velocity (horizontal) and turning from the inputs. */
	void UpdateWalk(float DeltaTime, float SpeedScale);
	/** Takeoff requested by the inputs of the current mode. */
	bool WantsTakeoff() const;
	void TakeOff();
	/** The wind carries the Dinofly: updates its drift and moves it by it (after the flight's own move). */
	void ApplyWind(float DeltaTime);
	/** Hover mode: the vertical speed from the glide, the beats and pushes, forward speed's lift (see the class comment). */
	void UpdateVertical(float DeltaTime, float HorizontalSpeed);
	/** Adds a beat (velocity, cm/s) to the vertical forces: a punch and a tail. */
	void AddVerticalImpulse(float Impulse);
	/** Brake intensity from the held input and taps, and its endurance. */
	void UpdateBrake(float DeltaTime);
	/** The body's pitch and bank from the flight's forces, through a spring (LeanPitch, LeanBank). */
	void UpdateAttitude(float DeltaTime, float NoseUp);
	/** Reports a blocking hit of the move with Velocity (before the hit changes it). */
	void ReportImpact(const FHitResult& Hit, ELBAFlightImpact Kind);
	void TickDead(float DeltaTime);

	float Pitch = 0.f;
	float Roll = 0.f;
	float Flap = 0.f;
	float Brake = 0.f;
	float ForwardInput = 0.f;
	float TurnInput = 0.f;
	float Vertical = 0.f;
	enum class EDivePhase : uint8 { None, Diving, PullingOut };
	EDivePhase DivePhase = EDivePhase::None;
	bool bDiveInput = false;
	bool bDivePressed = false;
	/** Dive: speed along the path (cm/s), and the path angle (radians below the horizon, negative climbing). */
	float DiveSpeed = 0.f;
	float DivePath = 0.f;
	/** Beats asked for since the last tick. */
	int32 PendingBeats = 0;
	/** Vertical forces of the beats (cm/s^2): the punch and the tail, each fading on its own. */
	float PunchForce = 0.f;
	float TailForce = 0.f;
	int32 BeatCount = 0;
	float TimeSinceBeat = 100.f;
	bool bDead = false;
	/** 0 gliding .. 1 stalled; seconds since the brake was last used. */
	float Stall = 0.f;
	float TimeSinceBrake = 100.f;
	/** Brake: seconds of a tap left, intensity (0..1), endurance left (0..1). */
	float BrakeTapLeft = 0.f;
	float BrakeAmount = 0.f;
	float BrakeStamina = 1.f;
	bool bBrakeExhausted = false;
	/** Seconds the dive has been held. */
	float DiveHoldTime = 0.f;
	/** Attitude spring: angular speeds (degrees/s); acceleration estimate. */
	float LeanPitchRate = 0.f;
	float LeanBankRate = 0.f;
	FVector SmoothedAcceleration = FVector::ZeroVector;
	FVector LastVelocity = FVector::ZeroVector;
	/** Seconds left crouching before taking off, or negative. */
	float TakeoffCountdown = -1.f;
	/** Hover mode: degrees/s of heading change, and the cosmetic lean (degrees). */
	float TurnRate = 0.f;
	float LeanPitch = 0.f;
	float LeanBank = 0.f;

	bool bLanded = false;
	bool bOnWater = false;
	/** Radians. */
	float Bank = 0.f;
	float Alpha = 0.f;
	FVector Heading = FVector::ForwardVector;
	/** Drift the wind gives (cm/s), following it with some lag. */
	FVector WindDrift = FVector::ZeroVector;
};
