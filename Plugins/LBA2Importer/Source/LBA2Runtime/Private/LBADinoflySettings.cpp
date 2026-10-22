#include "LBADinoflySettings.h"

namespace
{
	constexpr float DerivedSimulationStep = 1.f / 60.f;
}

ULBADinoflySettings::ULBADinoflySettings()
{
	CategoryName = TEXT("Game");
	SectionName = TEXT("LBA2Dinofly");
}

float ULBADinoflySettings::GetAirDensity(float Z) const
{
	return FMath::Pow(2.f, (ReferenceAltitude - Z) / FMath::Max(AirHalvingHeight, 1.f));
}

float ULBADinoflySettings::GetLiftShare(float Speed, float Z) const
{
	const float Relative = Speed / FMath::Max(CruiseSpeed, 1.f);
	return FMath::Min(CruiseLift * GetAirDensity(Z) * Relative * Relative, MaxSpeedLift);
}

float ULBADinoflySettings::GetVerticalAcceleration(float Vz, float Z, float HorizontalSpeed, float Stall) const
{
	const float Thin = 1.f / FMath::Sqrt(FMath::Max(GetAirDensity(Z), 0.05f));
	// Gliding: settles toward its sink, lifted by speed (level at a share of 1, climbing above): slowly from above
	// (inertia), quicker from below (the spread wings catch the air).
	const float Sink = GlideSinkSpeed * Thin;
	const float Target = -Sink * (1.f - GetLiftShare(HorizontalSpeed, Z));
	const float Glide = (Target - Vz) * (Vz < Target ? CatchRate : VerticalInertia);
	if (Stall <= 0.f)
	{
		return Glide;
	}
	// Stalled: falls faster and faster, up to the terminal speed (beyond it, the air brakes it).
	const float Fall = FMath::Max(-Vz, 0.f);
	const float Stalled = -StallAcceleration * Thin * (1.f - FMath::Square(Fall / (FallTerminalSpeed * Thin)));
	return FMath::Lerp(Glide, Stalled, FMath::Clamp(Stall, 0.f, 1.f));
}

float ULBADinoflySettings::StepStall(float Stall, float TimeSinceBeat, float Z, float HorizontalSpeed, float Dt) const
{
	// Gives up a while after the last beat, unless the speed carries the weight; recovers otherwise.
	const float Share = GetLiftShare(HorizontalSpeed, Z);
	const float Rate = TimeSinceBeat > StallGraceTime && Share < 1.f ? 1.f - Share : -1.f;
	return FMath::Clamp(Stall + Rate * Dt / FMath::Max(StallTime, 0.01f), 0.f, 1.f);
}

float ULBADinoflySettings::GetBeatStrength(float Gap) const
{
	return 1.f - FMath::Exp(-FMath::Max(Gap, 0.f) / FMath::Max(BeatRecoveryTime, 0.01f));
}

float ULBADinoflySettings::GetDamage(float Speed, float PainSpeed, float DamageScale) const
{
	return FMath::Max(0.f, Speed - PainSpeed) * DamagePerSpeed * DamageScale;
}

float ULBADinoflySettings::SimulatePumping(float Interval, float Z, float Impulse, float HorizontalSpeed) const
{
	// The same integration as ULBAFlightMovementComponent::UpdateVertical, at a fixed altitude.
	const float Density = GetAirDensity(Z);
	const float Strength = Interval > 0.f ? GetBeatStrength(Interval) : 0.f;
	float Vz = 0.f, Punch = 0.f, Tail = 0.f, NextBeat = 0.f, Sum = 0.f, Stall = 0.f, SinceBeat = 0.f;
	int32 Counted = 0;
	for (float Time = 0.f; Time < 40.f; Time += DerivedSimulationStep)
	{
		if (Interval > 0.f && Time >= NextBeat)
		{
			const float Beat = Impulse * Strength * Density;
			Punch += BeatPunch * Beat / BeatPunchTime;
			Tail += (1.f - BeatPunch) * Beat / BeatTailTime;
			NextBeat += Interval;
			Stall = FMath::Max(0.f, Stall - BeatStallRecovery);
			SinceBeat = 0.f;
		}
		SinceBeat += DerivedSimulationStep;
		Stall = StepStall(Stall, SinceBeat, Z, HorizontalSpeed, DerivedSimulationStep);
		Vz += GetVerticalAcceleration(Vz, Z, HorizontalSpeed, Stall) * DerivedSimulationStep;
		const float PunchDecay = FMath::Exp(-DerivedSimulationStep / BeatPunchTime);
		const float TailDecay = FMath::Exp(-DerivedSimulationStep / BeatTailTime);
		Vz += Punch * BeatPunchTime * (1.f - PunchDecay) + Tail * BeatTailTime * (1.f - TailDecay);
		Punch *= PunchDecay;
		Tail *= TailDecay;
		if (Time >= 20.f)
		{
			Sum += Vz;
			++Counted;
		}
	}
	return Counted > 0 ? Sum / Counted : 0.f;
}

float ULBADinoflySettings::SimulateDrop(float Height, float Z) const
{
	// From hovering still, the last beat just done.
	float Vz = 0.f, Fallen = 0.f, Stall = 0.f;
	for (float Time = 0.f; Time < 60.f && Fallen < Height; Time += DerivedSimulationStep)
	{
		Stall = StepStall(Stall, Time, Z - Fallen, 0.f, DerivedSimulationStep);
		Vz += GetVerticalAcceleration(Vz, Z - Fallen, 0.f, Stall) * DerivedSimulationStep;
		Fallen -= Vz * DerivedSimulationStep;
	}
	return -Vz;
}

void ULBADinoflySettings::PostInitProperties()
{
	Super::PostInitProperties();
	UpdateDerived();
}

void ULBADinoflySettings::PostReloadConfig(FProperty* PropertyThatWasLoaded)
{
	Super::PostReloadConfig(PropertyThatWasLoaded);
	UpdateDerived();
}

#if WITH_EDITOR
void ULBADinoflySettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateDerived();
}
#endif

void ULBADinoflySettings::UpdateDerived()
{
	// The beat that makes HoldBeatInterval hold the altitude at the reference altitude.
	{
		float Low = 0.f, High = 5000.f;
		for (int32 Step = 0; Step < 30; ++Step)
		{
			const float Mid = 0.5f * (Low + High);
			(SimulatePumping(HoldBeatInterval, ReferenceAltitude, Mid) > 0.f ? High : Low) = Mid;
		}
		BeatImpulse = 0.5f * (Low + High);
	}

	const float Intervals[] = { HoldBeatInterval, 2.f, 1.5f, 1.f, 0.75f, 0.5f, 0.3f };
	const float Altitudes[] = { 0.f, ReferenceAltitude, ReferenceAltitude + 1500.f, ReferenceAltitude + 3000.f };
	FString Text = FString::Printf(TEXT("Beat impulse %.0f cm/s at the reference density.\n"), BeatImpulse);
	Text += TEXT("Steady vertical speed (m/s) pumping once every ... s, hovering; cruising forward without pumping:\n  altitude      ");
	for (const float Interval : Intervals)
	{
		Text += FString::Printf(TEXT("%6.2fs"), Interval);
	}
	Text += TEXT("  cruise\n");
	for (const float Z : Altitudes)
	{
		Text += FString::Printf(TEXT("  %5.0f m (x%.2f)"), Z / 100.f, GetAirDensity(Z));
		for (const float Interval : Intervals)
		{
			Text += FString::Printf(TEXT("%+7.1f"), SimulatePumping(Interval, Z, BeatImpulse) / 100.f);
		}
		Text += FString::Printf(TEXT("  %+5.1f\n"), SimulatePumping(0.f, Z, BeatImpulse, CruiseSpeed) / 100.f);
	}

	auto Ceiling = [](TFunctionRef<float(float)> Climb)
	{
		float Low = -10000.f, High = 50000.f;
		for (int32 Step = 0; Step < 30; ++Step)
		{
			const float Mid = 0.5f * (Low + High);
			(Climb(Mid) > 0.f ? Low : High) = Mid;
		}
		return Low / 100.f;
	};
	Text += FString::Printf(TEXT("Pumping holds up to %.0f m at 1 beat/s, %.0f m at 3 beats/s; cruising forward climbs up to %.0f m.\n"),
		Ceiling([this](float Z) { return SimulatePumping(1.f, Z, BeatImpulse); }),
		Ceiling([this](float Z) { return SimulatePumping(0.333f, Z, BeatImpulse); }),
		Ceiling([this](float Z) { return SimulatePumping(0.f, Z, BeatImpulse, CruiseSpeed); }));

	// Hands-off drops onto the ground at sea level.
	const float SafeDrop = Ceiling([this](float Height) { return SimulateDrop(Height, Height) < GroundPainSpeed ? 1.f : -1.f; });
	Text += FString::Printf(TEXT("Stopping pumping while hovering, the drop (onto the ground at sea level) starts to hurt from %.1f m; "), SafeDrop);
	for (const float Height : { 1000.f, 2000.f, 4000.f })
	{
		const float Speed = SimulateDrop(Height, Height);
		Text += FString::Printf(TEXT("%.0f m: %.1f m/s, %.0f damage; "), Height / 100.f, Speed / 100.f, GetDamage(Speed, GroundPainSpeed));
	}
	Text += FString::Printf(TEXT("\nBraking takes a fall at the terminal speed below the pain speed in %.1f s; it lasts %.1f s, recovers in %.0f s.\n"),
		FMath::Loge(FMath::Max(FallTerminalSpeed / FMath::Max(GroundPainSpeed, 1.f), 1.f)) / FMath::Max(BrakeVerticalRate, 0.01f), BrakeEndurance, BrakeRecoveryTime);
	Text += FString::Printf(TEXT("Damage (life %.0f): wall at cruise %.0f, wall at 2x cruise %.0f, ground at the terminal fall %.0f, water at 15 m/s %.0f.\n"),
		MaxLife, GetDamage(CruiseSpeed, ObstaclePainSpeed), GetDamage(2.f * CruiseSpeed, ObstaclePainSpeed),
		GetDamage(FallTerminalSpeed, GroundPainSpeed), GetDamage(1500.f, WaterPainSpeed, WaterDamageScale));

	// How many of each kill a healthy Dinofly.
	auto ToDie = [this](float Damage) { return Damage > 0.f ? FString::FromInt(FMath::CeilToInt(MaxLife / Damage)) : FString(TEXT("never")); };
	Text += FString::Printf(TEXT("Deadly: hands-off drops from 20 m x%s, from 40 m x%s; walls hit at cruise x%s, at 2x cruise x%s; water at 15 m/s x%s."),
		*ToDie(GetDamage(SimulateDrop(2000.f, 2000.f), GroundPainSpeed)), *ToDie(GetDamage(SimulateDrop(4000.f, 4000.f), GroundPainSpeed)),
		*ToDie(GetDamage(CruiseSpeed, ObstaclePainSpeed)), *ToDie(GetDamage(2.f * CruiseSpeed, ObstaclePainSpeed)),
		*ToDie(GetDamage(1500.f, WaterPainSpeed, WaterDamageScale)));
	Summary = Text;
}
