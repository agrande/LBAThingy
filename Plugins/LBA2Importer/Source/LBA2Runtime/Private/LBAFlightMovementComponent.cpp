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
		TickAirborne(DeltaTime);
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
	if (-Velocity.Z < MaxLandingSinkSpeed && Velocity.Size() < MaxLandingSpeed)
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
	if (Flap > 0.5f)
	{
		bLanded = false;
		bOnWater = false;
		Alpha = FMath::DegreesToRadians(TrimAngle);
		Velocity = Heading * TakeoffVelocity.X + FVector::UpVector * TakeoffVelocity.Y;
		return;
	}

	Heading = Heading.RotateAngleAxis(Roll * GroundTurnRate * DeltaTime, FVector::UpVector);
	Bank = FMath::FInterpTo(Bank, 0.f, DeltaTime, 6.f);
	Alpha = FMath::FInterpTo(Alpha, 0.f, DeltaTime, 6.f);

	// Drift to a stop, riding the waves.
	FVector Horizontal(Velocity.X, Velocity.Y, 0.f);
	Horizontal = Horizontal.GetSafeNormal() * FMath::Max(0.f, Horizontal.Size() - 400.f * DeltaTime);
	Velocity = Horizontal;
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
	Velocity = FVector::VectorPlaneProject(Velocity, Hit.Normal) * 0.3f;
	Velocity.Z = 0.f;
}

void ULBAFlightMovementComponent::TickLanded(float DeltaTime)
{
	if (Flap > 0.5f)
	{
		bLanded = false;
		Alpha = FMath::DegreesToRadians(TrimAngle);
		Velocity = Heading * TakeoffVelocity.X + FVector::UpVector * TakeoffVelocity.Y;
		return;
	}

	Heading = Heading.RotateAngleAxis(Roll * GroundTurnRate * DeltaTime, FVector::UpVector);
	Bank = FMath::FInterpTo(Bank, 0.f, DeltaTime, 6.f);
	Alpha = FMath::FInterpTo(Alpha, 0.f, DeltaTime, 6.f);

	// Slide to a stop, held down by gravity.
	FVector Horizontal(Velocity.X, Velocity.Y, 0.f);
	Horizontal = Horizontal.GetSafeNormal() * FMath::Max(0.f, Horizontal.Size() - 800.f * DeltaTime);
	Velocity = FVector(Horizontal.X, Horizontal.Y, Velocity.Z + GetGravityZ() * DeltaTime);

	FHitResult Hit;
	const FVector Delta = Velocity * DeltaTime + FVector(0.f, 0.f, -2.f);
	const FQuat Level = FQuat::Slerp(UpdatedComponent->GetComponentQuat(), Heading.ToOrientationQuat(), FMath::Min(1.f, 8.f * DeltaTime));
	SafeMoveUpdatedComponent(Delta, Level, true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		if (Hit.Normal.Z > WalkableNormalZ)
		{
			Velocity.Z = 0.f;
		}
		else
		{
			SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
		}
	}
	else if (Velocity.Z < -300.f)
	{
		// Slid off a ledge: flying again.
		bLanded = false;
	}
}
