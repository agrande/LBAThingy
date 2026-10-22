#include "LBAFlightMovementComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Pawn.h"
#include "LBADinoflySettings.h"
#include "LBAWindSubsystem.h"
#include "WaterBodyComponent.h"
#include "WaterBodyManager.h"

namespace
{
	/** Fixed simulation step: the lift/attitude coupling is stiff at high speed. */
	constexpr float SimulationStep = 1.f / 120.f;
	constexpr float WalkableNormalZ = 0.65f;
}

ULBAFlightMovementComponent::ULBAFlightMovementComponent()
{
	bWantsInitializeComponent = true;
	bConstrainToPlane = false;
}

void ULBAFlightMovementComponent::InitializeComponent()
{
	Super::InitializeComponent();
	Alpha = FMath::DegreesToRadians(TrimAngle);
	if (const AActor* Owner = GetOwner())
	{
		Heading = Owner->GetActorForwardVector().GetSafeNormal2D();
		if (Heading.IsNearlyZero())
		{
			Heading = FVector::ForwardVector;
		}
	}
}

void ULBAFlightMovementComponent::SetFlightInput(float InPitch, float InRoll, float InFlap, float InBrake)
{
	Pitch = FMath::Clamp(InPitch, -1.f, 1.f);
	Roll = FMath::Clamp(InRoll, -1.f, 1.f);
	Flap = FMath::Clamp(InFlap, 0.f, 1.f);
	Brake = FMath::Clamp(InBrake, 0.f, 1.f);
}

void ULBAFlightMovementComponent::SetHoverInput(float InForward, float InTurn, float InVertical)
{
	ForwardInput = FMath::Clamp(InForward, -1.f, 1.f);
	TurnInput = FMath::Clamp(InTurn, -1.f, 1.f);
	Vertical = FMath::Clamp(InVertical, -1.f, 1.f);
}

void ULBAFlightMovementComponent::SetDiveInput(bool bInDive)
{
	// A press starts a dive (a flying input: ignored on the ground), holding keeps diving.
	bDivePressed |= bInDive && !bDiveInput;
	bDiveInput = bInDive;
}

void ULBAFlightMovementComponent::Beat()
{
	if (!bDead)
	{
		++PendingBeats;
	}
}

void ULBAFlightMovementComponent::BrakeTap()
{
	if (!bDead)
	{
		BrakeTapLeft = ULBADinoflySettings::Get().BrakeTapTime;
	}
}

float ULBAFlightMovementComponent::GetDiveAggression() const
{
	return FMath::Clamp(DiveHoldTime / FMath::Max(ULBADinoflySettings::Get().DiveRampTime, 0.01f), 0.f, 1.f);
}

void ULBAFlightMovementComponent::SetDead(bool bInDead)
{
	bDead = bInDead;
	PendingBeats = 0;
	PunchForce = TailForce = 0.f;
	BrakeAmount = BrakeTapLeft = 0.f;
	DivePhase = EDivePhase::None;
	TakeoffCountdown = -1.f;
	if (bDead && bLanded && !bOnWater)
	{
		Velocity = FVector::ZeroVector;
	}
}

float ULBAFlightMovementComponent::GetAirDensity() const
{
	return UpdatedComponent ? ULBADinoflySettings::Get().GetAirDensity(UpdatedComponent->GetComponentLocation().Z) : 1.f;
}

float ULBAFlightMovementComponent::GetCruiseSpeed() const
{
	return ULBADinoflySettings::Get().CruiseSpeed;
}

void ULBAFlightMovementComponent::AddVerticalImpulse(float Impulse)
{
	// Integrated over time, the punch gives BeatPunch of the impulse and the tail the rest.
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	PunchForce += Settings.BeatPunch * Impulse / Settings.BeatPunchTime;
	TailForce += (1.f - Settings.BeatPunch) * Impulse / Settings.BeatTailTime;
}

void ULBAFlightMovementComponent::UpdateVertical(float DeltaTime, float HorizontalSpeed)
{
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	const float Z = UpdatedComponent->GetComponentLocation().Z;
	const float Density = Settings.GetAirDensity(Z);

	// Beats: every press, or by themselves while climb is held (when enabled). A beat soon after another is weaker,
	// and thin air gives less; flapping also loses its strength toward MaxAltitude.
	if (Vertical > 0.5f && Settings.HeldBeatInterval > 0.f && TimeSinceBeat >= Settings.HeldBeatInterval)
	{
		++PendingBeats;
	}
	for (; PendingBeats > 0; --PendingBeats)
	{
		const float CeilingFade = FMath::Clamp((MaxAltitude - Z) / 1000.f, 0.f, 1.f);
		AddVerticalImpulse(Settings.GetBeatImpulse() * Settings.GetBeatStrength(TimeSinceBeat) * Density * CeilingFade);
		TimeSinceBeat = 0.f;
		++BeatCount;
		Stall = FMath::Max(0.f, Stall - Settings.BeatStallRecovery);
	}

	// The glide, lifted by speed, giving way to a stall a while after the last beat (ULBADinoflySettings: the same model
	// as its derived values); braking spreads the wings out of a stall.
	// Any wing work (a beat, the brake) holds the stall off.
	TimeSinceBrake = BrakeAmount > 0.f ? 0.f : TimeSinceBrake + DeltaTime;
	Stall = Settings.StepStall(Stall, FMath::Min(TimeSinceBeat, TimeSinceBrake), Z, HorizontalSpeed, DeltaTime);
	Velocity.Z += Settings.GetVerticalAcceleration(Velocity.Z, Z, HorizontalSpeed, Stall) * DeltaTime;

	// The beats' exact integral over the step, then their decay.
	const float PunchDecay = FMath::Exp(-DeltaTime / Settings.BeatPunchTime);
	const float TailDecay = FMath::Exp(-DeltaTime / Settings.BeatTailTime);
	Velocity.Z += PunchForce * Settings.BeatPunchTime * (1.f - PunchDecay) + TailForce * Settings.BeatTailTime * (1.f - TailDecay);
	PunchForce *= PunchDecay;
	TailForce *= TailDecay;

	// Braking kills the vertical speed, whichever way (less in thin air).
	if (BrakeAmount > 0.f)
	{
		const float Bite = FMath::Clamp(FMath::Sqrt(Density), 0.5f, 1.5f);
		Velocity.Z *= FMath::Exp(-Settings.BrakeVerticalRate * BrakeAmount * Bite * DeltaTime);
	}
}

void ULBAFlightMovementComponent::UpdateBrake(float DeltaTime)
{
	// Held, or for a moment after a tap; nothing once the wings are spent, until they recover enough.
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	BrakeTapLeft = FMath::Max(0.f, BrakeTapLeft - DeltaTime);
	const float Tap = Settings.BrakeTapTime > 0.f ? BrakeTapLeft / Settings.BrakeTapTime : 0.f;
	const bool bFlying = !bLanded && !bDead && Mode == ELBAFlightMode::Hover;
	BrakeAmount = bFlying && !bBrakeExhausted ? FMath::Max(Vertical < -0.5f ? 1.f : 0.f, Tap) : 0.f;
	if (BrakeAmount > 0.f)
	{
		BrakeStamina -= BrakeAmount * DeltaTime / Settings.BrakeEndurance;
		if (BrakeStamina <= 0.f)
		{
			BrakeStamina = 0.f;
			bBrakeExhausted = true;
		}
		return;
	}
	BrakeStamina = FMath::Min(1.f, BrakeStamina + DeltaTime / Settings.BrakeRecoveryTime * (bLanded ? 3.f : 1.f));
	if (bBrakeExhausted && BrakeStamina >= Settings.BrakeResumeFraction)
	{
		bBrakeExhausted = false;
	}
}

void ULBAFlightMovementComponent::UpdateAttitude(float DeltaTime, float NoseUp)
{
	// The lean the flight's forces give: at speed the nose follows the path (in the heading's vertical plane: flying
	// backward or sideways never points it), slower the body tips with the accelerations like a pendulum under its
	// wings, and sideways accelerations bank it (a coordinated turn).
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	const float Gravity = -GetGravityZ();
	const FVector Right = FRotationMatrix(Heading.Rotation()).GetScaledAxis(EAxis::Y);
	const float Forward = FMath::Max(Velocity | Heading, 0.f);
	const float PathSpeed = FMath::Sqrt(FMath::Square(Forward) + FMath::Square(Velocity.Z));
	const float Align = FMath::SmoothStep(float(Settings.AlignSpeeds.X), float(Settings.AlignSpeeds.Y), PathSpeed);
	const float PathAngle = FMath::RadiansToDegrees(FMath::Atan2(Velocity.Z, Forward));
	const float PathPitch = PathAngle > 0.f ? PathAngle * Settings.ClimbNoseFollow : PathAngle;
	const float Tilt = -FMath::RadiansToDegrees(FMath::Atan((SmoothedAcceleration | Heading) / Gravity)) * Settings.AccelerationTilt;
	const float TargetPitch = FMath::Clamp(FMath::Lerp(Tilt, PathPitch, Align) + NoseUp, Settings.PitchLimits.X, Settings.PitchLimits.Y);
	// Turning in place leans a little too, as the wings push it round.
	const float TurnLean = 10.f * FMath::Clamp(TurnRate / FMath::Max(HoverTurnRate, 1.f), -1.f, 1.f) * (1.f - Align);
	const float TargetBank = FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan((SmoothedAcceleration | Right) / Gravity)) + TurnLean,
		-Settings.MaxBank, Settings.MaxBank);

	// Followed like a spring: a little late, swinging slightly past, settling.
	const float Omega = 2.f * UE_PI * Settings.AttitudeFrequency;
	for (float Remaining = DeltaTime; Remaining > UE_KINDA_SMALL_NUMBER; Remaining -= SimulationStep)
	{
		const float Step = FMath::Min(SimulationStep, Remaining);
		LeanPitchRate += (Omega * Omega * (TargetPitch - LeanPitch) - 2.f * Settings.AttitudeDamping * Omega * LeanPitchRate) * Step;
		LeanBankRate += (Omega * Omega * (TargetBank - LeanBank) - 2.f * Settings.AttitudeDamping * Omega * LeanBankRate) * Step;
		LeanPitch += LeanPitchRate * Step;
		LeanBank += LeanBankRate * Step;
	}
}

void ULBAFlightMovementComponent::ReportImpact(const FHitResult& Hit, ELBAFlightImpact Kind)
{
	const float Into = -(Velocity | Hit.Normal);
	if (Into > 0.f && !bDead)
	{
		OnImpact.Broadcast(Into, Kind, FVector(Hit.ImpactPoint));
	}
}

bool ULBAFlightMovementComponent::IsBlocked(const FVector& Delta) const
{
	if (!UpdatedPrimitive)
	{
		return false;
	}
	const FVector Start = UpdatedComponent->GetComponentLocation();
	FHitResult Hit;
	return GetWorld()->SweepSingleByChannel(Hit, Start, Start + Delta, UpdatedComponent->GetComponentQuat(),
		UpdatedPrimitive->GetCollisionObjectType(), UpdatedPrimitive->GetCollisionShape(), FCollisionQueryParams(SCENE_QUERY_STAT(LBAFlightAhead), false, GetOwner()),
		FCollisionResponseParams(UpdatedPrimitive->GetCollisionResponseToChannels()));
}

float ULBAFlightMovementComponent::GetHeightAboveSurface(float MaxDistance) const
{
	if (!UpdatedComponent)
	{
		return 0.f;
	}
	const FVector Start = UpdatedComponent->GetComponentLocation();
	float SurfaceZ = Start.Z - MaxDistance;
	FHitResult Hit;
	// Whatever the Dinofly itself would land on: sweep its own shape down, the surface is under its bottom.
	if (UpdatedPrimitive)
	{
		const FCollisionShape Shape = UpdatedPrimitive->GetCollisionShape();
		const float Bottom = Shape.GetExtent().Z;
		if (GetWorld()->SweepSingleByChannel(Hit, Start, Start - FVector(0.f, 0.f, MaxDistance), UpdatedComponent->GetComponentQuat(),
			UpdatedPrimitive->GetCollisionObjectType(), Shape, FCollisionQueryParams(SCENE_QUERY_STAT(LBAFlightHeight), false, GetOwner()),
			FCollisionResponseParams(UpdatedPrimitive->GetCollisionResponseToChannels())))
		{
			SurfaceZ = Hit.Location.Z - Bottom;
		}
	}
	float WaterZ = 0.f;
	if (FindWaterSurface(Start, WaterZ) && WaterZ > SurfaceZ && WaterZ < Start.Z)
	{
		SurfaceZ = WaterZ;
	}
	return FMath::Clamp(Start.Z - SurfaceZ, 0.f, MaxDistance);
}

void ULBAFlightMovementComponent::SetMode(ELBAFlightMode NewMode)
{
	if (NewMode == Mode)
	{
		return;
	}
	Mode = NewMode;
	DivePhase = EDivePhase::None;
	if (Mode == ELBAFlightMode::Glide)
	{
		// Enter the glide with at least gliding speed, along the heading.
		Alpha = FMath::DegreesToRadians(TrimAngle);
		Bank = 0.f;
		if (!bLanded)
		{
			const FVector Horizontal(Velocity.X, Velocity.Y, 0.f);
			Velocity = Heading * FMath::Max(Horizontal.Size(), GetTrimSpeed()) + FVector(0.f, 0.f, Velocity.Z);
		}
	}
	else
	{
		const FRotator Current = UpdatedComponent ? UpdatedComponent->GetComponentRotation() : FRotator::ZeroRotator;
		LeanPitch = FRotator::NormalizeAxis(Current.Pitch);
		LeanBank = FRotator::NormalizeAxis(Current.Roll);
		LeanPitchRate = LeanBankRate = 0.f;
	}
}

void ULBAFlightMovementComponent::StartHovering()
{
	if (const AActor* Owner = GetOwner())
	{
		const FVector Forward = Owner->GetActorForwardVector().GetSafeNormal2D();
		Heading = Forward.IsNearlyZero() ? Heading : Forward;
	}
	Mode = ELBAFlightMode::Hover;
	bLanded = false;
	bOnWater = false;
	Velocity = FVector::ZeroVector;
	WindDrift = FVector::ZeroVector;
	LeanPitch = LeanBank = 0.f;
	TurnRate = 0.f;
	PunchForce = TailForce = 0.f;
	DivePhase = EDivePhase::None;
	UpdateComponentVelocity();
}

void ULBAFlightMovementComponent::StartLanded()
{
	if (const AActor* Owner = GetOwner())
	{
		const FVector Forward = Owner->GetActorForwardVector().GetSafeNormal2D();
		Heading = Forward.IsNearlyZero() ? Heading : Forward;
	}
	bLanded = true;
	bOnWater = false;
	Velocity = FVector::ZeroVector;
	WindDrift = FVector::ZeroVector;
	Bank = LeanPitch = LeanBank = TurnRate = 0.f;
	PunchForce = TailForce = 0.f;
	DivePhase = EDivePhase::None;
	TakeoffCountdown = -1.f;
	UpdateComponentVelocity();
}

bool ULBAFlightMovementComponent::WantsTakeoff() const
{
	// Walking inputs never take off: only climbing (a beat, or held, in hover) or flapping (glide) does.
	return Mode == ELBAFlightMode::Hover ? Vertical > 0.5f || PendingBeats > 0 : Flap > 0.5f;
}

bool ULBAFlightMovementComponent::TickTakeoff(float DeltaTime)
{
	if (TakeoffCountdown < 0.f)
	{
		if (WantsTakeoff())
		{
			TakeoffCountdown = TakeoffDelay;
		}
		return false;
	}
	TakeoffCountdown -= DeltaTime;
	if (TakeoffCountdown > 0.f)
	{
		return false;
	}
	TakeoffCountdown = -1.f;
	TakeOff();
	return true;
}

void ULBAFlightMovementComponent::UpdateWalk(float DeltaTime, float SpeedScale)
{
	// Crouching to take off: stand still.
	const bool bCrouching = TakeoffCountdown >= 0.f;
	const float Forward = bCrouching ? 0.f : ForwardInput;
	const float TurnAmount = bCrouching ? 0.f : (Mode == ELBAFlightMode::Hover ? TurnInput : Roll);
	Heading = Heading.RotateAngleAxis(TurnAmount * GroundTurnRate * DeltaTime, FVector::UpVector);
	Bank = FMath::FInterpTo(Bank, 0.f, DeltaTime, 6.f);
	Alpha = FMath::FInterpTo(Alpha, 0.f, DeltaTime, 6.f);

	const FVector2D Target = FVector2D(Heading) * Forward * WalkSpeed * SpeedScale * (Forward < 0.f ? 0.5f : 1.f);
	FVector2D Horizontal(Velocity.X, Velocity.Y);
	Horizontal += (Target - Horizontal).GetClampedToMaxSize(WalkAcceleration * DeltaTime);
	Velocity.X = Horizontal.X;
	Velocity.Y = Horizontal.Y;
}

void ULBAFlightMovementComponent::TakeOff()
{
	bLanded = false;
	bOnWater = false;
	Alpha = FMath::DegreesToRadians(TrimAngle);
	if (Mode == ELBAFlightMode::Hover)
	{
		// The push-off, and the first wing beat.
		Velocity = FVector(0.f, 0.f, 0.5f * TakeoffVelocity.Y);
		PunchForce = TailForce = 0.f;
		PendingBeats = 1;
		TimeSinceBeat = 100.f;
		LeanPitchRate = LeanBankRate = 0.f;
	}
	else
	{
		Velocity = Heading * TakeoffVelocity.X + FVector::UpVector * TakeoffVelocity.Y;
	}
}

float ULBAFlightMovementComponent::UpdateTurn(float DeltaTime, float HorizontalSpeed)
{
	// The rate builds up and dies down (a little inertia); at speed the sideways load limits it.
	const float MaxRate = FMath::Min(HoverTurnRate, FMath::RadiansToDegrees(MaxTurnLoad * -GetGravityZ() / FMath::Max(HorizontalSpeed, 1.f)));
	TurnRate += FMath::Clamp(TurnInput * MaxRate - TurnRate, -HoverTurnAcceleration * DeltaTime, HoverTurnAcceleration * DeltaTime);
	const float Turned = TurnRate * DeltaTime;
	Heading = FRotator(0.f, Heading.Rotation().Yaw + Turned, 0.f).Vector();
	return Turned;
}

void ULBAFlightMovementComponent::TickHover(float DeltaTime)
{
	const float Height = GetHeightAboveSurface(5000.f);
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	if (BrakeAmount > 0.f && DivePhase != EDivePhase::None)
	{
		// Braking spreads the wings: out of the dive, the brake kills the speed.
		DivePhase = EDivePhase::None;
	}
	if (bDivePressed && DivePhase == EDivePhase::None && BrakeAmount <= 0.f)
	{
		// Into the dive from the current flight path.
		DivePhase = EDivePhase::Diving;
		DiveSpeed = Velocity.Size();
		DivePath = DiveSpeed > 100.f ? FMath::Asin(FMath::Clamp(-Velocity.Z / DiveSpeed, -1.f, 1.f)) : 0.f;
		PunchForce = TailForce = 0.f;
	}
	if (DivePhase != EDivePhase::None)
	{
		// Wings tucked: no beats meanwhile, nor a stall (the dive is flown).
		PendingBeats = 0;
		Stall = 0.f;
		TickDive(DeltaTime, Height);
		return;
	}

	// Turn; at speed the velocity turns with the heading (banked turns) instead of sliding.
	FVector2D Horizontal(Velocity.X, Velocity.Y);
	const float Speed = Horizontal.Size();
	const float Turned = UpdateTurn(DeltaTime, Speed);
	const float Banked = FMath::Clamp((Speed - GetCruiseSpeed()) / GetCruiseSpeed(), 0.f, 1.f);
	if (Banked > 0.f && FVector2D::DotProduct(Horizontal, FVector2D(Heading)) > 0.f)
	{
		Horizontal = Horizontal.GetRotated(Turned * Banked);
	}

	// Horizontal velocity eases toward the requested one: inertia without piloting; the wings bite more in thick air.
	// Braking kills it instead.
	const float Cruise = GetCruiseSpeed() * (ForwardInput < 0.f ? BackwardSpeedFactor : 1.f);
	const FVector2D TargetHorizontal = FVector2D(Heading) * ForwardInput * Cruise * (1.f - BrakeAmount);
	const float Bite = FMath::Clamp(FMath::Sqrt(GetAirDensity()), 0.5f, 1.5f);

	const bool bSpeedingUp = TargetHorizontal.SizeSquared() > Horizontal.SizeSquared();
	// Fast (after a dive): the extra speed bleeds off slowly, unless pulling back or braking.
	const bool bCoasting = !bSpeedingUp && ForwardInput >= 0.f && Speed > GetCruiseSpeed() && BrakeAmount <= 0.f;
	const float Deceleration = FMath::Lerp(bCoasting ? CoastDeceleration : HoverDeceleration, Settings.BrakeDeceleration * Bite, BrakeAmount);
	const FVector2D Change = TargetHorizontal - Horizontal;
	Horizontal += Change.GetClampedToMaxSize((bSpeedingUp ? HoverAcceleration * Bite : Deceleration) * DeltaTime);
	Velocity.X = Horizontal.X;
	Velocity.Y = Horizontal.Y;
	UpdateVertical(DeltaTime, Horizontal.Size());

	// The body leans with the flight; the nose rises when the wings flare (braking) and on the beats' power strokes.
	const float BeatPull = FMath::Clamp((PunchForce * Settings.BeatPunchTime + TailForce * Settings.BeatTailTime) / FMath::Max(Settings.GetBeatImpulse(), 1.f), 0.f, 1.f);
	UpdateAttitude(DeltaTime, Settings.GlideNoseUp + Settings.BrakeNoseUp * BrakeAmount + Settings.BeatNoseUp * BeatPull);

	const FVector Delta = Velocity * DeltaTime;
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, FRotator(LeanPitch, Heading.Rotation().Yaw, LeanBank).Quaternion(), true, Hit);
	if (!Hit.IsValidBlockingHit())
	{
		return;
	}
	if (Hit.Normal.Z > WalkableNormalZ)
	{
		// Ground is sticky: touching it from anywhere lands.
		ReportImpact(Hit, ELBAFlightImpact::Ground);
		TouchDown(Hit);
		return;
	}
	// Bump: lose the speed going into the obstacle and slide along it.
	ReportImpact(Hit, ELBAFlightImpact::Obstacle);
	const float Into = Velocity | Hit.Normal;
	if (Into < 0.f)
	{
		Velocity -= Into * Hit.Normal;
	}
	SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
}

void ULBAFlightMovementComponent::TickDive(float DeltaTime, float Height)
{
	const float Gravity = -GetGravityZ();
	if (DivePhase == EDivePhase::Diving && !bDiveInput)
	{
		DivePhase = EDivePhase::PullingOut;
	}
	// Curving the path: quick at low speed, limited by the load at speed.
	const float Radius = FMath::Square(DiveSpeed) / (PullOutLoad * Gravity);
	if (DivePhase == EDivePhase::Diving)
	{
		// Pulls out by itself while it still can: the ground or the water below, anything just ahead.
		const float PullOutDrop = Radius * (1.f - FMath::Cos(FMath::Max(DivePath, 0.f)));
		if (Height < PullOutDrop + DiveSpeed * FMath::Sin(DivePath) * 0.3f + 250.f || IsBlocked(Velocity * 0.6f))
		{
			DivePhase = EDivePhase::PullingOut;
		}
	}

	const bool bDiving = DivePhase == EDivePhase::Diving;
	float TargetPath = 0.f;
	if (bDiving)
	{
		// The longer the dive is held, the steeper.
		const FVector2D& Angles = ULBADinoflySettings::Get().DiveAngles;
		TargetPath = FMath::DegreesToRadians(FMath::Lerp(Angles.X, Angles.Y, GetDiveAggression()));
	}
	else if (Vertical > 0.f && UpdatedComponent->GetComponentLocation().Z < MaxAltitude - 1000.f)
	{
		// Zoom climb: the speed carries the Dinofly up.
		TargetPath = -FMath::DegreesToRadians(ZoomClimbAngle);
	}
	const float MaxPathRate = FMath::Min(FMath::DegreesToRadians(DivePitchRate), PullOutLoad * Gravity / FMath::Max(DiveSpeed, 1.f));
	DivePath += FMath::Clamp(TargetPath - DivePath, -MaxPathRate * DeltaTime, MaxPathRate * DeltaTime);

	// Speed: gravity along the path against the drag, and a few wing beats into the dive.
	const float Drag = Gravity / FMath::Square(DiveTerminalSpeed);
	float Acceleration = Gravity * FMath::Sin(DivePath) - Drag * FMath::Square(DiveSpeed);
	if (bDiving)
	{
		Acceleration += DiveFlapAcceleration * FMath::Clamp(1.f - DiveSpeed / DiveTerminalSpeed, 0.f, 1.f);
	}
	DiveSpeed = FMath::Max(0.f, DiveSpeed + Acceleration * DeltaTime);

	// Turning spirals down: the velocity follows the heading.
	const float HorizontalSpeed = DiveSpeed * FMath::Cos(DivePath);
	UpdateTurn(DeltaTime, HorizontalSpeed);
	Velocity = Heading * HorizontalSpeed - FVector::UpVector * DiveSpeed * FMath::Sin(DivePath);

	// The body along the path, banked into the turn: wings tucked the nose points the way; pulling out they flare a bit.
	UpdateAttitude(DeltaTime, bDiving ? 0.f : ULBADinoflySettings::Get().GlideNoseUp);

	// Level again, or slowed down to cruising speed in the zoom climb: flying on normally with the speed.
	if (!bDiving && (TargetPath < 0.f ? DiveSpeed <= GetCruiseSpeed() : FMath::Abs(DivePath) < 0.01f))
	{
		DivePhase = EDivePhase::None;
	}

	const FVector Delta = Velocity * DeltaTime;
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, FRotator(LeanPitch, Heading.Rotation().Yaw, LeanBank).Quaternion(), true, Hit);
	if (!Hit.IsValidBlockingHit())
	{
		return;
	}
	DivePhase = EDivePhase::None;
	if (Hit.Normal.Z > WalkableNormalZ)
	{
		ReportImpact(Hit, ELBAFlightImpact::Ground);
		TouchDown(Hit);
		return;
	}
	ReportImpact(Hit, ELBAFlightImpact::Obstacle);
	const float Into = Velocity | Hit.Normal;
	if (Into < 0.f)
	{
		Velocity -= Into * Hit.Normal;
	}
	SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
}

void ULBAFlightMovementComponent::Launch(float ForwardSpeed)
{
	if (const AActor* Owner = GetOwner())
	{
		const FVector Forward = Owner->GetActorForwardVector().GetSafeNormal2D();
		Heading = Forward.IsNearlyZero() ? Heading : Forward;
	}
	bLanded = false;
	Bank = 0.f;
	Alpha = FMath::DegreesToRadians(TrimAngle);
	Velocity = Heading * ForwardSpeed;
	WindDrift = FVector::ZeroVector;
	UpdateComponentVelocity();
}

float ULBAFlightMovementComponent::GetFlightPathAngle() const
{
	const float Speed = Velocity.Size();
	return Speed > 1.f ? FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Velocity.Z / Speed, -1.f, 1.f))) : 0.f;
}

float ULBAFlightMovementComponent::GetTrimSpeed() const
{
	const float Lift = LiftFactor * LiftCoefficient(FMath::DegreesToRadians(TrimAngle));
	return Lift > 0.f ? FMath::Sqrt(-GetGravityZ() / Lift) : 0.f;
}

float ULBAFlightMovementComponent::LiftCoefficient(float AngleOfAttack) const
{
	const float StallRadians = FMath::DegreesToRadians(StallAngle);
	const float Angle = FMath::Abs(AngleOfAttack);
	const float Coefficient = Angle < StallRadians
		? LiftSlope * Angle
		: LiftSlope * StallRadians * FMath::Max(0.3f, 1.f - (Angle - StallRadians) / StallRadians * 0.7f);
	return FMath::Sign(AngleOfAttack) * Coefficient;
}

void ULBAFlightMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (ShouldSkipUpdate(DeltaTime) || !UpdatedComponent || !PawnOwner)
	{
		return;
	}
	// Hitches must not explode the integration.
	DeltaTime = FMath::Min(DeltaTime, 0.1f);
	TimeSinceBeat += DeltaTime;
	DiveHoldTime = DivePhase == EDivePhase::Diving && bDiveInput ? DiveHoldTime + DeltaTime : 0.f;
	UpdateBrake(DeltaTime);
	// The flight model works in the air; the wind's drift is added back once it moved.
	Velocity -= WindDrift;
	if (bDead && !(bLanded && bOnWater))
	{
		TickDead(DeltaTime);
	}
	else if (bLanded && bOnWater)
	{
		TickFloating(DeltaTime);
	}
	else if (bLanded)
	{
		TickLanded(DeltaTime);
	}
	else
	{
		if (Mode == ELBAFlightMode::Hover)
		{
			TickHover(DeltaTime);
		}
		else
		{
			TickAirborne(DeltaTime);
		}
		HandleWater();
	}
	ApplyWind(DeltaTime);
	Velocity += WindDrift;
	bDivePressed = false;
	// What the body feels (smoothed; bumps and landings clipped): leans it the next tick.
	if (bLanded || DeltaTime <= 0.f)
	{
		SmoothedAcceleration = FVector::ZeroVector;
	}
	else
	{
		const FVector Raw = ((Velocity - LastVelocity) / DeltaTime).GetClampedToMaxSize(-3.f * GetGravityZ());
		SmoothedAcceleration = FMath::VInterpTo(SmoothedAcceleration, Raw, DeltaTime, 8.f);
	}
	LastVelocity = Velocity;
	UpdateComponentVelocity();
}

void ULBAFlightMovementComponent::ApplyWind(float DeltaTime)
{
	if (bLanded && !bOnWater)
	{
		WindDrift = FVector::ZeroVector;
		return;
	}
	FVector Target = FVector::ZeroVector;
	const ULBAWindSubsystem* Wind = GetWorld()->GetSubsystem<ULBAWindSubsystem>();
	if (Wind && Wind->HasWind())
	{
		Target = Wind->GetWindAt(UpdatedComponent->GetComponentLocation()) * WindExposure;
		if (bOnWater)
		{
			Target = FVector(Target.X, Target.Y, 0.0) * FloatingWindDrift;
		}
	}
	const float FollowHorizontal = 1.f - FMath::Exp(-WindResponse.X * DeltaTime);
	const float FollowVertical = 1.f - FMath::Exp(-WindResponse.Y * DeltaTime);
	WindDrift += (Target - WindDrift) * FVector(FollowHorizontal, FollowHorizontal, FollowVertical);

	const FVector Delta = WindDrift * DeltaTime;
	if (Delta.IsNearlyZero())
	{
		return;
	}
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		// Pushed against something: no drift into it, the rest slides along.
		const float Into = WindDrift | Hit.Normal;
		if (Into < 0.f)
		{
			WindDrift -= Into * Hit.Normal;
		}
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}
}

void ULBAFlightMovementComponent::Simulate(float DeltaTime)
{
	const float Gravity = -GetGravityZ();
	const float Speed = Velocity.Size();
	const FVector Horizontal(Velocity.X, Velocity.Y, 0.f);
	if (Horizontal.SizeSquared() > FMath::Square(50.f))
	{
		Heading = Horizontal.GetSafeNormal();
	}

	// Wind frame: along the velocity, "up" in the vertical plane of the heading, then banked.
	const FVector Dir = Speed > 1.f ? Velocity / Speed : Heading;
	const FVector Side = FVector::CrossProduct(Heading, FVector::UpVector);
	const FVector Up0 = FVector::CrossProduct(Side, Dir).GetSafeNormal();
	const FVector Right0 = FVector::CrossProduct(Up0, Dir);
	Bank += BankResponse * (Roll * FMath::DegreesToRadians(MaxBank) - Bank) * DeltaTime;
	const FVector Up = Up0 * FMath::Cos(Bank) + Right0 * FMath::Sin(Bank);

	// Angle of attack: trim (more when banked, so turns stay level-ish; flared when braking) plus input.
	const float Trim0 = FMath::DegreesToRadians(TrimAngle);
	float Trim = Trim0 / FMath::Max(0.45f, FMath::Cos(Bank));
	Trim += Brake * (FMath::DegreesToRadians(BrakeAngle) - Trim);
	float Target = Pitch >= 0.f
		? Trim + Pitch * (FMath::DegreesToRadians(MaxAngle) - Trim)
		: Trim + Pitch * (Trim - FMath::DegreesToRadians(MinAngle));

	const float Dynamic = LiftFactor * Speed * Speed * LiftSlope;
	const float PathAngle = FMath::Asin(FMath::Clamp(Dir.Z, -1.f, 1.f));
	constexpr float LimitBlend = UE_PI / 9.f;
	if (Dynamic > 1.f)
	{
		// Flight path limits: near vertical, stop curving further (climb) or pull out (dive).
		const float ClimbWeight = FMath::Clamp((PathAngle - (FMath::DegreesToRadians(MaxClimbAngle) - LimitBlend)) / LimitBlend, 0.f, 1.f);
		const float DiveWeight = FMath::Clamp((-PathAngle - (FMath::DegreesToRadians(MaxDiveAngle) - LimitBlend)) / LimitBlend, 0.f, 1.f);
		const float Neutral = Gravity * FMath::Cos(PathAngle) / Dynamic;
		Target = FMath::Min(Target, FMath::Lerp(Target, Neutral, ClimbWeight));
		Target = FMath::Max(Target, FMath::Lerp(Target, Trim0, DiveWeight));
		Target = FMath::Clamp(Target, MinLoadFactor * Gravity / Dynamic, MaxLoadFactor * Gravity / Dynamic);
	}
	Alpha += AngleResponse * (Target - Alpha) * DeltaTime;

	const float Lift = LiftCoefficient(Alpha);
	const float Pressure = LiftFactor * Speed * Speed;
	const float Drag = Pressure * (ZeroLiftDrag + InducedDrag * Lift * Lift + Brake * BrakeDrag);
	FVector Accel = Up * (Pressure * Lift) - Dir * Drag + FVector(0.f, 0.f, -Gravity);
	if (Flap > 0.f)
	{
		const float Altitude = UpdatedComponent->GetComponentLocation().Z;
		const float Strength = Flap * FMath::Clamp((MaxAltitude - Altitude) / 1000.f, 0.f, 1.f);
		const FVector Forward = Dir * FMath::Cos(Alpha) + Up * FMath::Sin(Alpha);
		Accel += Strength * (Forward * FlapThrust + FVector::UpVector * FlapHoverLift * FMath::Max(0.f, 1.f - Speed / HoverFadeSpeed));
	}
	Velocity += Accel * DeltaTime;
}

FQuat ULBAFlightMovementComponent::ComputeBodyRotation() const
{
	const float Speed = Velocity.Size();
	const FVector Dir = Speed > 1.f ? Velocity / Speed : Heading;
	// Hovering or falling slowly: keep the body near level instead of pointing it along the velocity.
	const FVector VisualDir = FMath::Lerp(Heading, Dir, FMath::Clamp(Speed / 400.f, 0.f, 1.f)).GetSafeNormal();
	const FVector Side = FVector::CrossProduct(Heading, FVector::UpVector);
	const FVector Up0 = FVector::CrossProduct(Side, VisualDir).GetSafeNormal();
	const FVector Right0 = FVector::CrossProduct(Up0, VisualDir);
	const FVector Up = Up0 * FMath::Cos(Bank) + Right0 * FMath::Sin(Bank);
	const FVector Forward = VisualDir * FMath::Cos(Alpha) + Up * FMath::Sin(Alpha);
	const FVector BodyUp = Up * FMath::Cos(Alpha) - VisualDir * FMath::Sin(Alpha);
	return FRotationMatrix::MakeFromXZ(Forward, BodyUp).ToQuat();
}

void ULBAFlightMovementComponent::TickAirborne(float DeltaTime)
{
	FVector Delta = FVector::ZeroVector;
	for (float Remaining = DeltaTime; Remaining > UE_KINDA_SMALL_NUMBER; Remaining -= SimulationStep)
	{
		const float Step = FMath::Min(SimulationStep, Remaining);
		Simulate(Step);
		Delta += Velocity * Step;
	}

	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, ComputeBodyRotation(), true, Hit);
	if (!Hit.IsValidBlockingHit())
	{
		return;
	}
	ReportImpact(Hit, Hit.Normal.Z > WalkableNormalZ ? ELBAFlightImpact::Ground : ELBAFlightImpact::Obstacle);
	const float Into = Velocity | Hit.Normal;
	if (Hit.Normal.Z > WalkableNormalZ && -Into < MaxLandingSinkSpeed && Velocity.Size() < MaxLandingSpeed)
	{
		TouchDown(Hit);
		return;
	}
	// Too fast or too steep: bounce off and lose speed.
	if (Into < 0.f)
	{
		Velocity -= 1.3f * Into * Hit.Normal;
	}
	Velocity *= 0.7f;
	SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
}

bool ULBAFlightMovementComponent::FindWaterSurface(const FVector& Location, float& OutSurfaceZ) const
{
	bool bFound = false;
	FWaterBodyManager::ForEachWaterBodyComponent(GetWorld(), [&](UWaterBodyComponent* Body)
	{
		FVector Surface, Normal, WaterVelocity;
		float Depth = 0.f;
		if (Body && Body->GetWaterSurfaceInfoAtLocation(Location, Surface, Normal, WaterVelocity, Depth))
		{
			OutSurfaceZ = bFound ? FMath::Max(OutSurfaceZ, float(Surface.Z)) : float(Surface.Z);
			bFound = true;
		}
		return true;
	});
	return bFound;
}

void ULBAFlightMovementComponent::HandleWater()
{
	const FVector Location = UpdatedComponent->GetComponentLocation();
	float SurfaceZ = 0.f;
	if (bLanded || !FindWaterSurface(Location, SurfaceZ) || Location.Z >= SurfaceZ + WaterlineHeight)
	{
		return;
	}
	FHitResult Hit;
	MoveUpdatedComponent(FVector(0.f, 0.f, SurfaceZ + WaterlineHeight - Location.Z), UpdatedComponent->GetComponentQuat(), true, &Hit);
	if (-Velocity.Z > 0.f && !bDead)
	{
		OnImpact.Broadcast(-Velocity.Z, ELBAFlightImpact::Water, FVector(Location.X, Location.Y, SurfaceZ));
	}
	if (bDead)
	{
		// Floats where it fell.
		bLanded = true;
		bOnWater = true;
		Velocity = FVector(Velocity.X, Velocity.Y, 0.f) * 0.3f;
		return;
	}
	if (Mode == ELBAFlightMode::Hover || (-Velocity.Z < MaxLandingSinkSpeed && Velocity.Size() < MaxLandingSpeed))
	{
		bLanded = true;
		bOnWater = true;
		Bank = 0.f;
		Velocity = FVector(Velocity.X, Velocity.Y, 0.f) * 0.3f;
		return;
	}
	// Too fast: skim off the surface, losing speed.
	Velocity.Z = FMath::Abs(Velocity.Z) * 0.3f;
	Velocity *= 0.7f;
}

void ULBAFlightMovementComponent::TickFloating(float DeltaTime)
{
	if (TickTakeoff(DeltaTime))
	{
		return;
	}

	// Paddle slowly, riding the waves.
	UpdateWalk(DeltaTime, 0.6f);
	Velocity.Z = 0.f;
	const FVector Horizontal = Velocity;
	const FVector Location = UpdatedComponent->GetComponentLocation();
	float SurfaceZ = Location.Z - WaterlineHeight;
	if (!FindWaterSurface(Location, SurfaceZ))
	{
		bOnWater = false;
		bLanded = false;
		return;
	}
	const FVector Delta = Horizontal * DeltaTime + FVector(0.f, 0.f, SurfaceZ + WaterlineHeight - Location.Z);
	const FQuat Level = FQuat::Slerp(UpdatedComponent->GetComponentQuat(), Heading.ToOrientationQuat(), FMath::Min(1.f, 8.f * DeltaTime));
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, Level, true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		if (Hit.Normal.Z > WalkableNormalZ)
		{
			// Drifted onto the shore.
			bOnWater = false;
		}
		else
		{
			SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
		}
	}
}

void ULBAFlightMovementComponent::TickDead(float DeltaTime)
{
	if (bLanded)
	{
		// Lying where it fell.
		Velocity = FVector::ZeroVector;
		return;
	}
	// Limp: falls, the nose dipping, keeping some of its momentum.
	const float Damping = FMath::Exp(-0.6f * DeltaTime);
	Velocity.X *= Damping;
	Velocity.Y *= Damping;
	Velocity.Z = FMath::Max(Velocity.Z + GetGravityZ() * DeltaTime, -DeadFallSpeed);
	LeanPitch = FMath::FInterpTo(LeanPitch, -20.f, DeltaTime, 2.f);
	LeanBank = FMath::FInterpTo(LeanBank, 0.f, DeltaTime, 2.f);

	const FVector Delta = Velocity * DeltaTime;
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, FRotator(LeanPitch, Heading.Rotation().Yaw, LeanBank).Quaternion(), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		if (Hit.Normal.Z > WalkableNormalZ)
		{
			bLanded = true;
			bOnWater = false;
			Velocity = FVector::ZeroVector;
			// On the ground, level: the lying pose is in the animation.
			SafeMoveUpdatedComponent(FVector::ZeroVector, FRotator(0.f, Heading.Rotation().Yaw, 0.f).Quaternion(), true, Hit);
			return;
		}
		const float Into = Velocity | Hit.Normal;
		if (Into < 0.f)
		{
			Velocity -= Into * Hit.Normal;
		}
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}
	HandleWater();
}

void ULBAFlightMovementComponent::TouchDown(const FHitResult& Hit)
{
	bLanded = true;
	bOnWater = false;
	Bank = 0.f;
	TurnRate = 0.f;
	DivePhase = EDivePhase::None;
	TakeoffCountdown = -1.f;
	// The legs absorb the touchdown.
	Velocity = FVector::ZeroVector;
}

void ULBAFlightMovementComponent::TickLanded(float DeltaTime)
{
	if (TickTakeoff(DeltaTime))
	{
		return;
	}

	// Walk slowly: along the ground (up slopes, along walls)...
	UpdateWalk(DeltaTime, 1.f);
	const FQuat Level = FQuat::Slerp(UpdatedComponent->GetComponentQuat(), Heading.ToOrientationQuat(), FMath::Min(1.f, 8.f * DeltaTime));
	const FVector Walk(Velocity.X * DeltaTime, Velocity.Y * DeltaTime, 0.f);
	FHitResult Hit;
	SafeMoveUpdatedComponent(Walk, Level, true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		if (Hit.Normal.Z <= WalkableNormalZ)
		{
			// Walls: lose the speed going into them.
			const float Into = FVector(Velocity.X, Velocity.Y, 0.f) | Hit.Normal;
			if (Into < 0.f)
			{
				Velocity.X -= Into * Hit.Normal.X;
				Velocity.Y -= Into * Hit.Normal.Y;
			}
		}
		SlideAlongSurface(Walk, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}

	// ...then down onto the floor (a few cm, enough to follow slopes going down), never sliding while standing.
	constexpr float FloorSnap = 10.f;
	Velocity.Z += GetGravityZ() * DeltaTime;
	const FVector Down(0.f, 0.f, FMath::Min(Velocity.Z * DeltaTime, 0.f) - FloorSnap);
	SafeMoveUpdatedComponent(Down, UpdatedComponent->GetComponentQuat(), true, Hit);
	if (Hit.IsValidBlockingHit() && Hit.Normal.Z > WalkableNormalZ)
	{
		Velocity.Z = 0.f;
	}
	else if (Hit.IsValidBlockingHit())
	{
		// Too steep to stand on: slide down it.
		SlideAlongSurface(Down, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}
	else if (Velocity.Z < -300.f)
	{
		// Walked off a ledge: flying again.
		bLanded = false;
	}
}
