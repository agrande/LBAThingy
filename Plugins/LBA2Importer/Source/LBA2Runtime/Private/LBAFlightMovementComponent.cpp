#include "LBAFlightMovementComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Pawn.h"
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

void ULBAFlightMovementComponent::Thrust()
{
	// A flying input: on the ground, only climbing takes off.
	if (Mode != ELBAFlightMode::Hover || bLanded)
	{
		return;
	}
	ThrustKick = 1.f;
	const float Added = FMath::Min(ThrustImpulse, MaxThrustSpeed - ThrustSpeed);
	if (Added > 0.f)
	{
		ThrustSpeed += Added;
		Velocity += Heading * Added * 0.5f;
	}
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
	LeanPitch = LeanBank = 0.f;
	TurnRate = 0.f;
	ThrustSpeed = ThrustKick = 0.f;
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
	Bank = LeanPitch = LeanBank = TurnRate = 0.f;
	ThrustSpeed = ThrustKick = 0.f;
	TakeoffCountdown = -1.f;
	UpdateComponentVelocity();
}

bool ULBAFlightMovementComponent::WantsTakeoff() const
{
	// Walking inputs never take off: only climbing (hover) or flapping (glide) does.
	return Mode == ELBAFlightMode::Hover ? Vertical > 0.5f : Flap > 0.5f;
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
	Velocity = Mode == ELBAFlightMode::Hover
		? FVector(0.f, 0.f, ClimbSpeed)
		: Heading * TakeoffVelocity.X + FVector::UpVector * TakeoffVelocity.Y;
}

void ULBAFlightMovementComponent::TickHover(float DeltaTime)
{
	// Turn: the rate builds up and dies down, a little inertia.
	const float OldYaw = Heading.Rotation().Yaw;
	TurnRate += FMath::Clamp(TurnInput * HoverTurnRate - TurnRate, -HoverTurnAcceleration * DeltaTime, HoverTurnAcceleration * DeltaTime);
	const float NewYaw = OldYaw + TurnRate * DeltaTime;
	Heading = FRotator(0.f, NewYaw, 0.f).Vector();

	// Thrust fades (faster when pulling back).
	ThrustSpeed = FMath::Max(0.f, ThrustSpeed - ThrustDecay * DeltaTime * (ForwardInput < 0.f ? 3.f : 1.f));
	ThrustKick = FMath::Max(0.f, ThrustKick - 2.f * DeltaTime);

	// Velocity eases toward the requested one: inertia without piloting.
	const FVector Location = UpdatedComponent->GetComponentLocation();
	const float Cruise = HoverSpeed * (ForwardInput < 0.f ? BackwardSpeedFactor : 1.f);
	const FVector2D TargetHorizontal = FVector2D(Heading) * (ForwardInput * Cruise + ThrustSpeed);
	const float CeilingFade = FMath::Clamp((MaxAltitude - Location.Z) / 1000.f, 0.f, 1.f);
	const float TargetVertical = Vertical > 0.f ? Vertical * ClimbSpeed * CeilingFade : Vertical * DescendSpeed;

	FVector2D Horizontal(Velocity.X, Velocity.Y);
	const bool bSpeedingUp = TargetHorizontal.SizeSquared() > Horizontal.SizeSquared();
	const FVector2D Change = TargetHorizontal - Horizontal;
	Horizontal += Change.GetClampedToMaxSize((bSpeedingUp ? HoverAcceleration : HoverDeceleration) * DeltaTime);
	const float VerticalSpeed = Velocity.Z + FMath::Clamp(TargetVertical - Velocity.Z, -VerticalAcceleration * DeltaTime, VerticalAcceleration * DeltaTime);
	Velocity = FVector(Horizontal.X, Horizontal.Y, VerticalSpeed);

	// Cosmetic lean: nose down with forward speed, bank with the turn.
	const float ForwardSpeed = FVector2D::DotProduct(Horizontal, FVector2D(Heading));
	const float TargetPitch = -MaxLeanPitch * FMath::Clamp(ForwardSpeed / GetMaxHoverSpeed(), -1.f, 1.f);
	const float TargetBank = MaxLeanBank * FMath::Clamp(TurnRate / HoverTurnRate, -1.f, 1.f) * FMath::Clamp(Horizontal.Size() / HoverSpeed, 0.f, 1.f);
	LeanPitch = FMath::FInterpTo(LeanPitch, TargetPitch, DeltaTime, 4.f);
	LeanBank = FMath::FInterpTo(LeanBank, TargetBank, DeltaTime, 4.f);

	const FVector Delta = Velocity * DeltaTime;
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, FRotator(LeanPitch, NewYaw, LeanBank).Quaternion(), true, Hit);
	if (!Hit.IsValidBlockingHit())
	{
		return;
	}
	if (Hit.Normal.Z > WalkableNormalZ)
	{
		// Ground is sticky: touching it from anywhere lands.
		TouchDown(Hit);
		return;
	}
	// Bump: lose the speed going into the obstacle and slide along it.
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
	const float Stall = FMath::DegreesToRadians(StallAngle);
	const float Angle = FMath::Abs(AngleOfAttack);
	const float Coefficient = Angle < Stall
		? LiftSlope * Angle
		: LiftSlope * Stall * FMath::Max(0.3f, 1.f - (Angle - Stall) / Stall * 0.7f);
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
	if (bLanded && bOnWater)
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
	UpdateComponentVelocity();
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

void ULBAFlightMovementComponent::TouchDown(const FHitResult& Hit)
{
	bLanded = true;
	bOnWater = false;
	Bank = 0.f;
	TurnRate = 0.f;
	ThrustSpeed = 0.f;
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
