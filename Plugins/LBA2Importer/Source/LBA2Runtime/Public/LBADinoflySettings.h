#pragma once

#include "Engine/DeveloperSettings.h"
#include "LBADinoflySettings.generated.h"

/**
 * The values that shape how riding the Dinofly feels, in one place: Project Settings > Game > LBA2 Dinofly.
 * Every change applies at once, even while playing in the editor. The design behind them (Plugins/LBA2Importer/README.md,
 * "Dinofly flight: choices and tradeoffs"), three buttons, three rhythms:
 *  - pump A (climb) at different rates: different rates of climb. One beat every HoldBeatInterval holds the altitude,
 *    faster climbs, with quickly decreasing returns (BeatRecoveryTime).
 *  - hold X (dive) for different durations: a more or less aggressive dive (DiveRampTime).
 *  - play Morse on B (brake): taps and holds kill speed, to stick a smooth landing; the brake tires after about
 *    BrakeEndurance seconds of use and recovers slowly.
 * Nothing pressed, the Dinofly glides down slowly, but not for long: a few seconds without a beat (and without the speed
 * to carry its weight), the wings give up and it falls faster and faster, up to a speed that hurts (Stall). Flying
 * forward lifts it, much more in the thick air near the ground than up high, where the air thins out quickly
 * (AirHalvingHeight); but low and fast is where hitting things hurts. Its body leans as the flight's forces would
 * lean it (Attitude), so its pose tells what it is doing.
 *
 * Read-only "Derived" values below show what the current numbers give (simulated with the flight model itself).
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "LBA2 Dinofly"))
class LBA2RUNTIME_API ULBADinoflySettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULBADinoflySettings();

	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	static const ULBADinoflySettings& Get() { return *GetDefault<ULBADinoflySettings>(); }

	// Wing beats (A: climb) --------------------------------------------------------------------------------------

	/**
	 * THE key value of pumping: one beat every this many seconds holds the altitude at Reference Altitude, in still
	 * air and without moving forward. Each beat's strength follows from it. Intended range: one beat every 2.5 s holds,
	 * 1 s climbs, 0.5 s climbs fast (tiring, little better).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Wing Beats (A: climb)", meta = (ClampMin = "0.5", ClampMax = "10", Units = "s"))
	float HoldBeatInterval = 2.5f;

	/**
	 * Diminishing returns: the wings need time to beat again. A beat this soon after the previous one has 63% of its
	 * strength (1 - exp(-gap / this)); one twice as late 86%. Larger = faster pumping pays off less.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Wing Beats (A: climb)", meta = (ClampMin = "0.05", ClampMax = "5", Units = "s"))
	float BeatRecoveryTime = 0.7f;

	/** Share of a beat delivered as a punch; the rest is a long tail. */
	UPROPERTY(Config, EditAnywhere, Category = "Wing Beats (A: climb)", meta = (ClampMin = "0", ClampMax = "1"))
	float BeatPunch = 0.6f;

	/** How fast the punch and the tail of a beat fade (time constants). */
	UPROPERTY(Config, EditAnywhere, Category = "Wing Beats (A: climb)", meta = (ClampMin = "0.02", ClampMax = "1", Units = "s"))
	float BeatPunchTime = 0.12f;

	UPROPERTY(Config, EditAnywhere, Category = "Wing Beats (A: climb)", meta = (ClampMin = "0.05", ClampMax = "5", Units = "s"))
	float BeatTailTime = 0.9f;

	/** Holding the climb button beats by itself at this interval; 0 = off (every beat is a press). */
	UPROPERTY(Config, EditAnywhere, Category = "Wing Beats (A: climb)", meta = (ClampMin = "0", ClampMax = "10", Units = "s"))
	float HeldBeatInterval = 0.f;

	/** Wing beats per second of the beat (one full stroke per press; climbing used to beat at 1.6 per second). */
	UPROPERTY(Config, EditAnywhere, Category = "Wing Beats (A: climb)", meta = (ClampMin = "0.5", ClampMax = "8"))
	float BeatFrequency = 3.25f;

	// Glide (between beats) ---------------------------------------------------------------------------------------

	/** Wings spread between beats, the Dinofly sinks this fast at Reference Altitude (faster in thinner air). */
	UPROPERTY(Config, EditAnywhere, Category = "Glide (between beats)", meta = (ClampMin = "0", ClampMax = "1500", Units = "cm/s"))
	float GlideSinkSpeed = 220.f;

	/**
	 * Vertical inertia: how fast the vertical speed settles back to the glide (1/s) after a beat. Lower = more inertia:
	 * beats carry longer.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Glide (between beats)", meta = (ClampMin = "0.05", ClampMax = "10"))
	float VerticalInertia = 0.5f;

	/** Same, when falling faster than the glide (out of a stall, a dive...): the spread wings catch the air (1/s). */
	UPROPERTY(Config, EditAnywhere, Category = "Glide (between beats)", meta = (ClampMin = "0.05", ClampMax = "10"))
	float CatchRate = 1.5f;

	// Stall (no pumping) -----------------------------------------------------------------------------------------

	/**
	 * Seconds after the last beat (or use of the brake) before the wings give up, unless flying fast enough to carry
	 * the weight (thick air helps): from then on the Dinofly stalls, falling faster and faster. Longer than
	 * HoldBeatInterval, so pumping to hold the altitude never stalls; tapping the brake down to a landing neither.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Stall (no pumping)", meta = (ClampMin = "0", ClampMax = "20", Units = "s"))
	float StallGraceTime = 3.f;

	/** Seconds from giving up to falling like a stone. */
	UPROPERTY(Config, EditAnywhere, Category = "Stall (no pumping)", meta = (ClampMin = "0.1", ClampMax = "20", Units = "s"))
	float StallTime = 1.5f;

	/** Stalled, it falls with this acceleration (cm/s^2 at Reference Altitude, more in thinner air)... */
	UPROPERTY(Config, EditAnywhere, Category = "Stall (no pumping)", meta = (ClampMin = "0", ClampMax = "2000"))
	float StallAcceleration = 700.f;

	/** ...up to this speed (cm/s, faster in thinner air): well above the ground's pain speed. */
	UPROPERTY(Config, EditAnywhere, Category = "Stall (no pumping)", meta = (ClampMin = "100", ClampMax = "6000", Units = "cm/s"))
	float FallTerminalSpeed = 2000.f;

	/** How much of a stall one beat recovers (1 = a single beat saves it); braking recovers it as time passes. */
	UPROPERTY(Config, EditAnywhere, Category = "Stall (no pumping)", meta = (ClampMin = "0", ClampMax = "1"))
	float BeatStallRecovery = 0.5f;

	// Brake (B) --------------------------------------------------------------------------------------------------

	/**
	 * Braking kills the speed: the fall or rise (this fraction per second, as a rate: 2.5 removes 92% in a second)
	 * and the horizontal speed (BrakeDeceleration). Taps brake briefly, holds longer: "play Morse" to land softly.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Brake (B)", meta = (ClampMin = "0", ClampMax = "20"))
	float BrakeVerticalRate = 2.5f;

	UPROPERTY(Config, EditAnywhere, Category = "Brake (B)", meta = (ClampMin = "0", ClampMax = "5000"))
	float BrakeDeceleration = 900.f;

	/** A tap brakes at least this long (fading out), so short taps still count. */
	UPROPERTY(Config, EditAnywhere, Category = "Brake (B)", meta = (ClampMin = "0", ClampMax = "1", Units = "s"))
	float BrakeTapTime = 0.15f;

	/** Seconds of braking (hovering in place) before the wings are exhausted and the brake stops working. */
	UPROPERTY(Config, EditAnywhere, Category = "Brake (B)", meta = (ClampMin = "0.5", ClampMax = "60", Units = "s"))
	float BrakeEndurance = 5.f;

	/** Seconds to recover from exhausted to fresh while not braking in flight (three times faster standing). */
	UPROPERTY(Config, EditAnywhere, Category = "Brake (B)", meta = (ClampMin = "0.5", ClampMax = "120", Units = "s"))
	float BrakeRecoveryTime = 10.f;

	/** Exhausted, the brake works again once this much of the endurance came back. */
	UPROPERTY(Config, EditAnywhere, Category = "Brake (B)", meta = (ClampMin = "0", ClampMax = "1"))
	float BrakeResumeFraction = 0.3f;

	/**
	 * Height above the ground or the water (cm) that picks the wings' braking style: below X, back-flaps reaching
	 * forward with the legs down (landing); between, fast hovering beats; above Y, flared wings like a parachute.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Brake (B)")
	FVector2D BrakeStyleHeights = FVector2D(400.f, 1500.f);

	// Dive (X) ---------------------------------------------------------------------------------------------------

	/**
	 * Holding X dives, the longer the more aggressively: the dive's path steepens from DiveAngles.X to DiveAngles.Y
	 * (degrees below the horizon) over DiveRampTime seconds held, and the wings fold tighter. Releasing pulls out.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Dive (X)")
	FVector2D DiveAngles = FVector2D(20.f, 80.f);

	UPROPERTY(Config, EditAnywhere, Category = "Dive (X)", meta = (ClampMin = "0.1", ClampMax = "10", Units = "s"))
	float DiveRampTime = 1.5f;

	// Air --------------------------------------------------------------------------------------------------------

	/** Altitude (height above sea level, Z) where the air has its reference density: the values above are given there. */
	UPROPERTY(Config, EditAnywhere, Category = "Air", meta = (Units = "cm"))
	float ReferenceAltitude = 1000.f;

	/**
	 * The air is half as dense this much higher (twice as dense this much lower). Thin air: beats, speed and the brake
	 * lift less, the glide sinks and stalls fall faster (1/sqrt(density)), speeding up forward is harder (sqrt(density)).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Air", meta = (ClampMin = "500", Units = "cm"))
	float AirHalvingHeight = 3000.f;

	// Forward flight ---------------------------------------------------------------------------------------------

	/** Speed of forward flight (cm/s); backward is slower. Dives go much faster. */
	UPROPERTY(Config, EditAnywhere, Category = "Forward Flight", meta = (ClampMin = "100", ClampMax = "4000", Units = "cm/s"))
	float CruiseSpeed = 800.f;

	/**
	 * Lift of flying forward at CruiseSpeed at Reference Altitude, as a share of the weight: 1 glides level, above 1
	 * climbs by itself (speed gives altitude for free near the ground) and never stalls. Grows with speed squared and
	 * the air's density.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Forward Flight", meta = (ClampMin = "0", ClampMax = "10"))
	float CruiseLift = 1.3f;

	/** Limit of that share (fast out of a dive). */
	UPROPERTY(Config, EditAnywhere, Category = "Forward Flight", meta = (ClampMin = "0", ClampMax = "20"))
	float MaxSpeedLift = 4.f;

	// Attitude ---------------------------------------------------------------------------------------------------

	/**
	 * The body leans the way the flight's forces lean it: at speed its nose follows the path (from AlignSpeeds.X to
	 * AlignSpeeds.Y cm/s of air speed), slower it tips with the accelerations (forward, nose down; braking, nose up),
	 * turns and sideways pushes bank it (coordinated: tan(bank) = sideways acceleration / g).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Attitude")
	FVector2D AlignSpeeds = FVector2D(300.f, 1000.f);

	/** How much accelerations tip the body (1 = like a pendulum hanging from the wings). */
	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "0", ClampMax = "3"))
	float AccelerationTilt = 0.6f;

	/** Climbing, how much the nose follows the path up (falling and diving it follows fully). */
	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "0", ClampMax = "1"))
	float ClimbNoseFollow = 0.6f;

	/** Nose above the path (degrees): gliding, braking (flared), beating (the power stroke pulls it up). */
	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "-30", ClampMax = "60"))
	float GlideNoseUp = 4.f;

	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "-30", ClampMax = "80"))
	float BrakeNoseUp = 20.f;

	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "-30", ClampMax = "60"))
	float BeatNoseUp = 8.f;

	/** Limits (degrees). */
	UPROPERTY(Config, EditAnywhere, Category = "Attitude")
	FVector2D PitchLimits = FVector2D(-80.f, 50.f);

	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "0", ClampMax = "89"))
	float MaxBank = 60.f;

	/**
	 * The body follows that lean like a spring: its frequency (Hz, faster = stiffer) and damping (1 = no overshoot,
	 * lower swings past and settles: organic).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "0.1", ClampMax = "10"))
	float AttitudeFrequency = 0.9f;

	UPROPERTY(Config, EditAnywhere, Category = "Attitude", meta = (ClampMin = "0.05", ClampMax = "2"))
	float AttitudeDamping = 0.6f;

	// Pain -------------------------------------------------------------------------------------------------------

	/** Life of a healthy Dinofly. */
	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "1"))
	float MaxLife = 100.f;

	/**
	 * Speed into the surface (cm/s, the part of the velocity going into it) that starts to hurt: landing on the ground,
	 * hitting anything else (walls, trees, rocks, from any side), hitting the water.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0", Units = "cm/s"))
	float GroundPainSpeed = 650.f;

	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0", Units = "cm/s"))
	float ObstaclePainSpeed = 300.f;

	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0", Units = "cm/s"))
	float WaterPainSpeed = 900.f;

	/** Life lost per cm/s above the pain speed. */
	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0"))
	float DamagePerSpeed = 0.11f;

	/** Water hurts less than terrain and objects: damage scale. */
	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0", ClampMax = "2"))
	float WaterDamageScale = 0.7f;

	/** Life recovered per second while standing on the ground. */
	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0"))
	float LifeRegenPerSecond = 0.5f;

	/** One star flies out per this much damage (the original: 5 life points per star, up to 5 stars). */
	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0.1"))
	float DamagePerStar = 5.f;

	/** Seconds between the death and the "You died" screen. */
	UPROPERTY(Config, EditAnywhere, Category = "Pain", meta = (ClampMin = "0", Units = "s"))
	float RetryDelay = 2.5f;

	// Derived (read only) ----------------------------------------------------------------------------------------

	/** What the values above give, simulated: climb rates per pumping pace and altitude, falls, ceilings, damage. */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Derived", meta = (MultiLine = "true"))
	FString Summary;

	// The flight model (ULBAFlightMovementComponent) and the summary use these.

	/** Air density at altitude Z (cm), 1 at ReferenceAltitude. */
	float GetAirDensity(float Z) const;

	/** Share of the weight flying at horizontal air Speed carries at altitude Z (1 at cruise speed, reference altitude). */
	float GetLiftShare(float Speed, float Z) const;

	/**
	 * Vertical acceleration (cm/s^2) at vertical speed Vz, altitude Z, horizontal air speed and Stall (0..1): the glide
	 * (settling toward its sink, lifted by speed) blended into the stalled fall. Beats and the brake come on top.
	 */
	float GetVerticalAcceleration(float Vz, float Z, float HorizontalSpeed, float Stall) const;

	/** The stall after Dt more seconds, TimeSinceBeat seconds after the last beat (beats and the brake come on top). */
	float StepStall(float Stall, float TimeSinceBeat, float Z, float HorizontalSpeed, float Dt) const;

	/** Velocity (cm/s) a full-strength beat gives at the reference density (derived from HoldBeatInterval). */
	float GetBeatImpulse() const { return BeatImpulse; }

	/** Strength (0..1) of a beat Gap seconds after the previous one. */
	float GetBeatStrength(float Gap) const;

	/** Damage of hitting something at Speed into it, against PainSpeed (DamageScale for water). */
	float GetDamage(float Speed, float PainSpeed, float DamageScale = 1.f) const;

	virtual void PostInitProperties() override;
	virtual void PostReloadConfig(FProperty* PropertyThatWasLoaded) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Average vertical speed (cm/s) pumping once every Interval seconds at altitude Z with a beat of Impulse. */
	float SimulatePumping(float Interval, float Z, float Impulse, float HorizontalSpeed = 0.f) const;
	/** Hands-off drop from Height: the speed (cm/s) reaching the ground. */
	float SimulateDrop(float Height, float Z) const;
	void UpdateDerived();

	float BeatImpulse = 300.f;
};
