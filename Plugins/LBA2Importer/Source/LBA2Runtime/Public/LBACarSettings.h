#pragma once

#include "Engine/DeveloperSettings.h"
#include "LBACarSettings.generated.h"

/**
 * How one kind of car drives, in plain numbers (m/s, seconds): ALBACar turns them into the Chaos vehicle's engine,
 * gears, brakes and tires (Derived shows what that gives).
 */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBACarTuning
{
	GENERATED_BODY()

	/** Top speed (cm/s): the engine's rev limit in the top gear. The original buggy: 475 cm/s. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "100", ClampMax = "5000", Units = "cm/s"))
	float TopSpeed = 800.f;

	/** Top speed backing up (cm/s). The original buggy: 250 cm/s. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "50", ClampMax = "2000", Units = "cm/s"))
	float ReverseSpeed = 300.f;

	/** From standing to top speed at full throttle on level ground (s), through 3 gears. The original buggy: about 1 s. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "0.5", ClampMax = "20", Units = "s"))
	float AccelerationTime = 3.f;

	/** Braking (cm/s^2), S or the left trigger while moving forward. The original buggy: 1500. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "100", ClampMax = "5000"))
	float Braking = 1000.f;

	/** Slowing down with nothing pressed (cm/s^2). The original buggy: 875 (it stops almost at once). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "0", ClampMax = "2000"))
	float Coasting = 250.f;

	/** The front wheels' angle at full lock (degrees), when slow. The original draws them at 26. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "5", ClampMax = "45", Units = "deg"))
	float SteeringAngle = 30.f;

	/** Share of the lock left at top speed (steering eases off with the speed, so fast turns stay drivable). */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "0.05", ClampMax = "1"))
	float SteeringAtTopSpeed = 0.25f;

	/**
	 * Tire grip (Chaos's friction multiplier): higher corners and pulls away harder before sliding. Much below 3, the
	 * buggy's rear wheels can't take its engine's pull (traction control holds them at the limit).
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "0.2", ClampMax = "5"))
	float Grip = 4.f;

	/** Mass (kg), with Twinsen. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "20", ClampMax = "5000", Units = "kg"))
	float Mass = 300.f;

	/** Height of the center of mass above the ground (cm): lower rolls over less. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Car", meta = (ClampMin = "0", ClampMax = "200", Units = "cm"))
	float CenterOfMassHeight = 15.f;
};

/**
 * The Chaos engine, gearbox and brakes giving an FLBACarTuning: a flat torque curve, 3 automatic gears, the top
 * speed at the rev limit in the top gear, the torque for the wanted acceleration, the brake torque for the braking.
 */
struct LBA2RUNTIME_API FLBACarDrive
{
	static constexpr float MaxRPM = 6000.f;
	static constexpr float Efficiency = 0.9f;
	static constexpr float GearChangeTime = 0.15f;
	/** Forward gears, the top one 1 (the final ratio sets the top speed). */
	static constexpr float Gears[3] = { 2.2f, 1.45f, 1.f };
	/** Share of the time to top speed spent in each gear, flat torque, shifting at 92% of the limit (see Compute). */
	static constexpr float AccelerationShare = 0.72f;
	/** Measured corrections (2026-11-08, flat ground, buggy and scooter): 1 = as computed. */
	static constexpr float AccelerationFactor = 1.f;
	/** The brakes stopped at about 0.8 of the deceleration computed from their torque. */
	static constexpr float BrakeFactor = 1.25f;
	/** The limiter left it about 5% above the top speed computed from the wheel radius (the sphere sweeps' contact). */
	static constexpr float TopSpeedFactor = 1.05f;

	float FinalRatio = 1.f;
	float ReverseRatio = 1.f;
	/** Engine torque (N m), flat up to near the limit. */
	float MaxTorque = 100.f;
	/** Per braked wheel (N m). */
	float BrakeTorque = 500.f;
	/** Share of the brakes applied with nothing pressed (coasting). */
	float IdleBrake = 0.f;
	/** Turning circle radius at full lock when slow (cm), for a wheelbase. */
	float TurnRadius = 0.f;

	/**
	 * DriveRadius: the driven wheels' radius (cm); BrakeRadius: the average of the braked wheels'; Wheelbase (cm).
	 */
	static FLBACarDrive Compute(const struct FLBACarTuning& Tuning, float DriveRadius, float BrakeRadius, float Wheelbase, int32 NumBrakedWheels = 4);
};

/**
 * Drivable cars (ALBACar): the Desert's buggy, Citadel's and Proxima's, and Citadel's Rabibunny scooters. Project
 * Settings > Game > LBA2 Cars. Changes apply to cars spawned or loaded afterwards (start the level again).
 * Plan and the original's numbers: Plugins/LBA2Importer/CARS_PLAN.md.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "LBA2 Cars"))
class LBA2RUNTIME_API ULBACarSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULBACarSettings();

	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	static const ULBACarSettings& Get() { return *GetDefault<ULBACarSettings>(); }

	/** The buggy (LBA2's only car: the Desert's, Citadel's parked one, Proxima's). */
	UPROPERTY(Config, EditAnywhere, Category = "Buggy")
	FLBACarTuning Buggy;

	/** The Rabibunny's scooter (Citadel's taxi), its driver taking Twinsen behind him. */
	UPROPERTY(Config, EditAnywhere, Category = "Scooter")
	FLBACarTuning Scooter;

	/** Twinsen gets out only this slow (cm/s). */
	UPROPERTY(Config, EditAnywhere, Category = "Riding", meta = (ClampMin = "0", Units = "cm/s"))
	float MaxLeaveSpeed = 150.f;

	/** On its side or roof and nearly still this long (s): it rights itself. */
	UPROPERTY(Config, EditAnywhere, Category = "Riding", meta = (ClampMin = "0.2", Units = "s"))
	float SelfRightTime = 1.5f;

	/** Water this deep (cm) starts to drown the engine; at Max Water Depth it no longer pulls. */
	UPROPERTY(Config, EditAnywhere, Category = "Riding", meta = (ClampMin = "0", Units = "cm"))
	float WaterDepthStall = 15.f;

	UPROPERTY(Config, EditAnywhere, Category = "Riding", meta = (ClampMin = "1", Units = "cm"))
	float MaxWaterDepth = 35.f;

	/** What the numbers give (read only): the Chaos engine, gears and brakes ALBACar sets up from them. */
	UPROPERTY(Transient, VisibleAnywhere, Category = "Derived", meta = (MultiLine = "true"))
	FString Derived;

	virtual void PostInitProperties() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void UpdateDerived();
};
