#include "LBACarSettings.h"

#include "LBACar.h"

FLBACarDrive FLBACarDrive::Compute(const FLBACarTuning& Tuning, float DriveRadius, float BrakeRadius, float Wheelbase, int32 NumBrakedWheels)
{
	FLBACarDrive Drive;
	const float EngineSpeed = MaxRPM * 2.f * UE_PI / 60.f;
	const float TopWheelSpeed = Tuning.TopSpeed / FMath::Max(DriveRadius, 1.f);
	// The top speed at the limiter, where the torque curve falls off (TopSpeedFactor: measured).
	Drive.FinalRatio = TopSpeedFactor * EngineSpeed / TopWheelSpeed;
	Drive.ReverseRatio = Tuning.TopSpeed / FMath::Max(Tuning.ReverseSpeed, 1.f);

	// With a flat torque and shifts near the limit (gear ratios 2.2, 1.45, 1), the run to top speed spends about
	// AccelerationShare of (top speed / top gear's acceleration) accelerating, plus the two shifts.
	const float TopSpeedMeters = Tuning.TopSpeed / 100.f;
	const float Pulling = FMath::Max(Tuning.AccelerationTime - 2.f * GearChangeTime, 0.25f);
	const float TopGearAcceleration = AccelerationShare * TopSpeedMeters / Pulling;
	Drive.MaxTorque = AccelerationFactor * TopGearAcceleration * Tuning.Mass * (DriveRadius / 100.f) / (Gears[2] * Drive.FinalRatio * Efficiency);

	Drive.BrakeTorque = BrakeFactor * (Tuning.Braking / 100.f) * Tuning.Mass * (BrakeRadius / 100.f) / FMath::Max(NumBrakedWheels, 1);
	Drive.IdleBrake = FMath::Clamp(Tuning.Coasting / FMath::Max(Tuning.Braking, 1.f), 0.f, 1.f);
	Drive.TurnRadius = Wheelbase / FMath::Max(FMath::Tan(FMath::DegreesToRadians(Tuning.SteeringAngle)), 0.01f);
	return Drive;
}

ULBACarSettings::ULBACarSettings()
{
	Scooter.TopSpeed = 600.f;
	Scooter.ReverseSpeed = 200.f;
	Scooter.AccelerationTime = 2.5f;
	Scooter.Braking = 800.f;
	Scooter.Coasting = 200.f;
	Scooter.SteeringAngle = 35.f;
	Scooter.SteeringAtTopSpeed = 0.3f;
	Scooter.Grip = 4.f;
	Scooter.Mass = 200.f;
	Scooter.CenterOfMassHeight = 10.f;
}

void ULBACarSettings::PostInitProperties()
{
	Super::PostInitProperties();
	UpdateDerived();
}

#if WITH_EDITOR
void ULBACarSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateDerived();
}
#endif

void ULBACarSettings::UpdateDerived()
{
	auto Describe = [](const TCHAR* Name, const FLBACarTuning& Tuning, const FLBACarGeometry& Car)
	{
		const FLBACarDrive Drive = Car.ComputeDrive(Tuning);
		const float Stopping = FMath::Square(Tuning.TopSpeed) / (2.f * Tuning.Braking) / 100.f;
		const float Coasting = Tuning.Coasting > 0.f ? FMath::Square(Tuning.TopSpeed) / (2.f * Tuning.Coasting) / 100.f : 0.f;
		const float TopTurn = Car.GetWheelbase() / FMath::Max(FMath::Tan(FMath::DegreesToRadians(Tuning.SteeringAngle * Tuning.SteeringAtTopSpeed)), 0.01f) / 100.f;
		return FString::Printf(TEXT("%s: %.1f m/s (%.0f km/h), reverse %.1f m/s; engine %.0f N m to %.0f rpm, final ratio %.1f, ")
			TEXT("reverse ratio %.1f; brakes %.0f N m a wheel, stop from top speed in %.1f m (%.1f m coasting); ")
			TEXT("turning radius %.1f m slow, %.1f m at top speed (%.1f g)"),
			Name, Tuning.TopSpeed / 100.f, Tuning.TopSpeed * 0.036f, Tuning.ReverseSpeed / 100.f, Drive.MaxTorque, FLBACarDrive::MaxRPM,
			Drive.FinalRatio, Drive.ReverseRatio, Drive.BrakeTorque, Stopping, Coasting, Drive.TurnRadius / 100.f, TopTurn,
			FMath::Square(Tuning.TopSpeed / 100.f) / FMath::Max(TopTurn, 0.01f) / 9.81f);
	};
	Derived = Describe(TEXT("Buggy"), Buggy, FLBACarGeometry::Buggy) + TEXT("\n") + Describe(TEXT("Scooter"), Scooter, FLBACarGeometry::Scooter);
}
