#include "LBABoat.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/CollisionProfile.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAAnimInstance.h"
#include "LBACharacter.h"
#include "LBAControls.h"
#include "LBAEntityAsset.h"
#include "LBAFlightMovementComponent.h"
#include "LBATypes.h"
#include "WaterBodyActor.h"

namespace
{
	TAutoConsoleVariable<bool> CVarBoatDebug(
		TEXT("lba.Boat.Debug"), false,
		TEXT("Shows the boats' speed, turn rate and throttle as text above them (debug builds)."));

	FAutoConsoleCommandWithWorldAndArgs BoatSpawnCommand(
		TEXT("lba.Boat.Spawn"),
		TEXT("Puts a boat on the water in front of the player (or where they look)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Pawn)
			{
				return;
			}
			const FVector Forward = Pawn->GetActorForwardVector().GetSafeNormal2D();
			FVector Location = Pawn->GetActorLocation() + Forward * 800.f;
			float SurfaceZ = 0.f;
			if (ULBAFlightMovementComponent::FindWaterSurface(World, Location, SurfaceZ))
			{
				Location.Z = SurfaceZ;
			}
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
			World->SpawnActor<ALBABoat>(ALBABoat::StaticClass(), Location, FRotator(0.f, Forward.Rotation().Yaw, 0.f), Params);
		}));

	/** Gravity when there's no water under it (cm/s^2). */
	constexpr float FallAcceleration = 980.f;
	/** Normals steeper than this are ground the hull rides on (no bounce off it). */
	constexpr float GroundNormalZ = 0.7f;
}

// Movement ------------------------------------------------------------------------------------------------------

void ULBABoatMovementComponent::SetInput(float InThrottle, float InRudder)
{
	Throttle = FMath::Clamp(InThrottle, -1.f, 1.f);
	Rudder = FMath::Clamp(InRudder, -1.f, 1.f);
}

float ULBABoatMovementComponent::GetForwardSpeed() const
{
	return UpdatedComponent ? float(FVector::DotProduct(Velocity, UpdatedComponent->GetForwardVector().GetSafeNormal2D())) : 0.f;
}

bool ULBABoatMovementComponent::IsUnderWay() const
{
	return (PawnOwner && PawnOwner->GetController()) || Velocity.SizeSquared() > 1.0 || FMath::Abs(TurnRate) > 0.01f;
}

void ULBABoatMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (ShouldSkipUpdate(DeltaTime) || !UpdatedComponent || !IsUnderWay())
	{
		return;
	}
	// Long frames in short steps (turning and the surface spring).
	float Remaining = FMath::Min(DeltaTime, 0.1f);
	while (Remaining > UE_KINDA_SMALL_NUMBER)
	{
		const float Step = FMath::Min(Remaining, 1.f / 60.f);
		Simulate(Step);
		Remaining -= Step;
	}
	UpdateComponentVelocity();
}

void ULBABoatMovementComponent::Simulate(float DeltaTime)
{
	const FVector Location = UpdatedComponent->GetComponentLocation();
	const float Yaw = UpdatedComponent->GetComponentRotation().Yaw;
	const FVector Forward = FRotator(0.f, Yaw, 0.f).Vector();
	const FVector Planar(Velocity.X, Velocity.Y, 0.0);
	double Speed = FVector::DotProduct(Planar, Forward);
	FVector Side = Planar - Forward * Speed;

	// The propeller pushes ahead or astern; against the motion it brakes. The drag balances full thrust at MaxSpeed.
	double Push = 0.0;
	if (Throttle * Speed < 0.0 && FMath::Abs(Speed) > 30.0)
	{
		Push = Throttle * BrakeThrust;
	}
	else if (Throttle > 0.f)
	{
		Push = Throttle * Thrust;
	}
	else if (Throttle < 0.f && Speed > -ReverseSpeed)
	{
		Push = Throttle * Thrust * 0.5;
	}
	const double Quadratic = FMath::Max(Thrust - LinearDrag * MaxSpeed, 0.0) / FMath::Square(double(MaxSpeed));
	double Drag = LinearDrag * Speed + Quadratic * Speed * FMath::Abs(Speed);
	if (Throttle == 0.f)
	{
		Drag += StopDrag * FMath::Sign(Speed);
	}
	const double NewSpeed = Speed + (Push - Drag) * DeltaTime;
	// Drag alone never turns the motion around.
	Speed = Push == 0.0 && NewSpeed * Speed < 0.0 ? 0.0 : NewSpeed;
	Side *= FMath::Exp(-Grip * DeltaTime);
	if (Side.SizeSquared() < 1.0)
	{
		Side = FVector::ZeroVector;
	}

	// The rudder needs the water flowing past (the outboard pivots a little when still); at speed the hull can only
	// take so much sideways load. Backing up, the bow swings the other way, like a car's.
	const double AbsSpeed = FMath::Abs(Speed);
	double MaxRate = MaxTurnRate * FMath::Lerp(double(StillTurn), 1.0, FMath::Clamp(AbsSpeed / TurnSpeed, 0.0, 1.0));
	if (AbsSpeed > 1.0)
	{
		MaxRate = FMath::Min(MaxRate, FMath::RadiansToDegrees(MaxTurnLoad / AbsSpeed));
	}
	const float TargetRate = float(Rudder * MaxRate * (Speed < -30.0 ? -1.0 : 1.0));
	TurnRate += (TargetRate - TurnRate) * (1.f - FMath::Exp(-TurnResponse * DeltaTime));
	if (FMath::Abs(TurnRate) < 0.01f && TargetRate == 0.f)
	{
		TurnRate = 0.f;
	}

	// Floats at the surface; falls where there is none.
	float SurfaceZ = 0.f;
	bAfloat = ULBAFlightMovementComponent::FindWaterSurface(GetWorld(), Location, SurfaceZ);
	const double VerticalSpeed = bAfloat ? (SurfaceZ - Location.Z) * FloatResponse : Velocity.Z - FallAcceleration * DeltaTime;

	const FVector OldPlanar = Planar;
	Velocity = Forward * Speed + Side + FVector(0.0, 0.0, VerticalSpeed);
	const FVector Delta = Velocity * DeltaTime;
	FHitResult Hit;
	SafeMoveUpdatedComponent(Delta, FRotator(0.f, Yaw + TurnRate * DeltaTime, 0.f), true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		// The speed into it is lost (some bounces back off walls); along it, the hull slides.
		const double Into = -FVector::DotProduct(Velocity, Hit.Normal);
		if (Into > 0.0)
		{
			const double Kept = Hit.Normal.Z > GroundNormalZ ? 1.0 : 1.0 + Bounce;
			Velocity += Hit.Normal * Into * Kept;
			if (Into > 300.0)
			{
				UE_LOG(LogLBA2, Verbose, TEXT("%s hit %s at %.1f m/s"), *GetOwner()->GetName(), *GetNameSafe(Hit.GetActor()), Into / 100.0);
			}
		}
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}
	if (!bAfloat && Hit.IsValidBlockingHit() && Hit.Normal.Z > GroundNormalZ)
	{
		Velocity.Z = 0.0;
	}

	const FVector NewPlanar(Velocity.X, Velocity.Y, 0.0);
	Acceleration = FMath::Lerp(Acceleration, (NewPlanar - OldPlanar) / DeltaTime, 1.0 - FMath::Exp(-8.0 * DeltaTime));
}

// Boat ----------------------------------------------------------------------------------------------------------

ALBABoat::ALBABoat()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessAI = EAutoPossessAI::Disabled;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// The ferry (body 165): 7.4 m long, 5 m wide, its keel 58 cm under the water where the islands have it, its roof
	// 4.2 m above. The hull's box goes from the keel to 60 cm above the water.
	Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
	Collision->InitBoxExtent(FVector(370.f, 250.f, 60.f));
	Collision->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	Collision->SetCanEverAffectNavigation(false);
	RootComponent = Collision;

	Cabin = CreateDefaultSubobject<UBoxComponent>(TEXT("Cabin"));
	Cabin->SetupAttachment(Collision);
	Cabin->InitBoxExtent(FVector(370.f, 250.f, 240.f));
	Cabin->SetRelativeLocation(FVector(0.f, 0.f, 180.f));
	Cabin->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Cabin->SetCollisionObjectType(ECC_WorldDynamic);
	Cabin->SetCollisionResponseToAllChannels(ECR_Ignore);
	Cabin->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Cabin->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
	Cabin->SetCanEverAffectNavigation(false);

	Hull = CreateDefaultSubobject<USceneComponent>(TEXT("Hull"));
	Hull->SetupAttachment(Collision);

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Hull);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Mesh->SetAnimInstanceClass(ULBAAnimInstance::StaticClass());
	Mesh->SetRenderCustomDepth(true);
	Mesh->SetCustomDepthStencilValue(LBA::MovingObjectStencil);
	// Lit by the island's character key light too.
	Mesh->SetLightingChannels(true, true, false);

	Helm = CreateDefaultSubobject<USceneComponent>(TEXT("Helm"));
	Helm->SetupAttachment(Hull);

	// Found by Twinsen's reach for things to use (ALBACharacter::TryInteract), blocking nothing.
	Boarding = CreateDefaultSubobject<UBoxComponent>(TEXT("Boarding"));
	Boarding->SetupAttachment(Collision);
	// 2.5 m around the hull: from the quay's edge (or wading in) Twinsen reaches it.
	Boarding->InitBoxExtent(FVector(620.f, 500.f, 270.f));
	Boarding->SetRelativeLocation(FVector(0.f, 0.f, 150.f));
	Boarding->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Boarding->SetCollisionObjectType(ECC_WorldDynamic);
	Boarding->SetCollisionResponseToAllChannels(ECR_Ignore);
	Boarding->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Boarding->SetGenerateOverlapEvents(false);
	Boarding->SetCanEverAffectNavigation(false);

	Movement = CreateDefaultSubobject<ULBABoatMovementComponent>(TEXT("Movement"));
	Movement->SetUpdatedComponent(Collision);

	// The camera follows the heading, never the hull's rocking.
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Collision);
	CameraBoom->SetUsingAbsoluteRotation(true);
	CameraBoom->TargetArmLength = 1600.f;
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 400.f);
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 8.f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->SetFieldOfView(75.f);
}

UPawnMovementComponent* ALBABoat::GetMovementComponent() const
{
	return Movement;
}

void ALBABoat::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyEntity();
}

void ALBABoat::ApplyEntity()
{
	Mesh->SetRelativeTransform(MeshTransform);
	Helm->SetRelativeLocation(HelmLocation);
	const ULBAEntityAsset* Body = Entity ? Entity.Get() : GetDefault<ULBA2Settings>()->Boat.LoadSynchronous();
	if (!Body)
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: no boat entity. Import the Characters layer (Tools > LBA2 Importer)."), *GetName());
		return;
	}
	USkeletalMesh* BodyMesh = Body->GetBodyMesh(BodyType);
	BodyMesh = BodyMesh ? BodyMesh : Body->GetDefaultBodyMesh();
	if (BodyMesh && Mesh->GetSkeletalMeshAsset() != BodyMesh)
	{
		Mesh->SetSkeletalMesh(BodyMesh);
	}
	ULBAAnimInstance* Anim = Cast<ULBAAnimInstance>(Mesh->GetAnimInstance());
	const FLBAEntityAnim* Idle = Body->Anims.Find(int32(ELBAAnimType::Idle));
	if (Anim && Idle && Idle->Sequence)
	{
		// The boat's motion is its own; its animation plays in place.
		Anim->SetApplyRootMotion(false);
		Anim->PlayLBAAnimation(Idle->Sequence, true, Idle->LoopStartTime);
	}
}

void ALBABoat::BeginPlay()
{
	Super::BeginPlay();
	ApplyEntity();
	CameraBase = FRotator(-14.f, GetActorRotation().Yaw, 0.f);
	RockClock = FMath::FRandRange(0.f, 100.f);
}

void ALBABoat::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Controller)
	{
		Movement->SetInput(FMath::Clamp(DriveInput.Y + TriggerInput, -1.f, 1.f), DriveInput.X);
	}
	else
	{
		Movement->SetInput(0.f, 0.f);
	}
	UpdateHull(DeltaSeconds);
	if (IsPlayerControlled())
	{
		UpdateCamera(DeltaSeconds);
	}
#if ENABLE_DRAW_DEBUG
	if (CVarBoatDebug.GetValueOnGameThread())
	{
		DrawDebug();
	}
#endif
}

void ALBABoat::UpdateHull(float DeltaSeconds)
{
	const float Speed = Movement->GetForwardSpeed();
	const float SpeedAlpha = FMath::Clamp(FMath::Abs(Speed) / Movement->MaxSpeed, 0.f, 1.f);
	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
	const FVector Right(-Forward.Y, Forward.X, 0.0);
	const FVector Acceleration = Movement->GetAcceleration();
	const float ForwardAcceleration = float(FVector::DotProduct(Acceleration, Forward));
	const float SideAcceleration = float(FVector::DotProduct(Acceleration, Right));

	// The bow rises through the planing hump (around a third of the top speed) and stays a bit up; accelerating
	// lifts it more, braking dips it. Turns lean the hull in. It rocks on the water, slaps the waves at speed.
	RockClock += DeltaSeconds;
	const float Hump = FMath::Sin(PI * FMath::Clamp(SpeedAlpha / 0.6f, 0.f, 1.f));
	float Pitch = PlaningPitch * (0.8f * Hump + 0.5f * SpeedAlpha) + 4.f * FMath::Clamp(ForwardAcceleration / 980.f, -1.f, 1.f);
	float Roll = FMath::Clamp(0.5f * FMath::RadiansToDegrees(FMath::Atan(SideAcceleration / 980.f)), -15.f, 15.f);
	Pitch += Rocking * (0.8f * FMath::Sin(RockClock * 1.1f) + 0.5f * SpeedAlpha * FMath::Sin(RockClock * 9.f));
	Roll += Rocking * (1.f - 0.5f * SpeedAlpha) * FMath::Sin(RockClock * 0.8f + 1.3f);
	const float TargetHeave = 3.f * FMath::Sin(RockClock * 1.6f) + 6.f * SpeedAlpha;

	const float Ease = 1.f - FMath::Exp(-5.f * DeltaSeconds);
	Lean += (FVector2D(Pitch, Roll) - Lean) * Ease;
	Heave += (TargetHeave - Heave) * Ease;
	Hull->SetRelativeLocationAndRotation(FVector(0.f, 0.f, Heave), FRotator(Lean.X, 0.f, Lean.Y));
}

void ALBABoat::UpdateCamera(float DeltaSeconds)
{
	const float SpeedAlpha = FMath::Clamp(FMath::Abs(Movement->GetForwardSpeed()) / Movement->MaxSpeed, 0.f, 1.f);
	const float ForwardAcceleration = float(FVector::DotProduct(Movement->GetAcceleration(), GetActorForwardVector().GetSafeNormal2D()));

	// Behind the boat, swinging after it with some lag; the look offset stays where the player left it.
	CameraBase = FMath::RInterpTo(CameraBase, FRotator(-14.f, GetActorRotation().Yaw, 0.f), DeltaSeconds, CameraFollowSpeed);
	CameraBoom->SetWorldRotation(FRotator(FMath::Clamp(CameraBase.Pitch + LookOffset.Y, -80.f, 60.f), CameraBase.Yaw + LookOffset.X, 0.f));

	// Further back and wider with the speed and the acceleration.
	const float FieldOfView = 75.f + 15.f * SpeedAlpha + 8.f * FMath::Clamp(ForwardAcceleration / 1000.f, 0.f, 1.f);
	CameraBoom->TargetArmLength = FMath::FInterpTo(CameraBoom->TargetArmLength, 1200.f + 500.f * SpeedAlpha, DeltaSeconds, 2.f);
	FollowCamera->SetFieldOfView(FMath::FInterpTo(FollowCamera->FieldOfView, FieldOfView, DeltaSeconds, 3.f));
}

void ALBABoat::DrawDebug() const
{
	DrawDebugString(GetWorld(), FVector(0.f, 0.f, 400.f), FString::Printf(TEXT("%.1f m/s  turn %.0f deg/s  throttle %.2f  %s"),
		Movement->GetForwardSpeed() / 100.f, Movement->GetTurnRate(), FMath::Clamp(DriveInput.Y + TriggerInput, -1.f, 1.f),
		Movement->IsAfloat() ? TEXT("afloat") : TEXT("no water")), const_cast<ALBABoat*>(this), FColor::Cyan, 0.f);
}

// Riding --------------------------------------------------------------------------------------------------------

bool ALBABoat::CanInteract_Implementation(APawn* User) const
{
	return !Rider.IsValid() && !Controller && Cast<ALBACharacter>(User);
}

void ALBABoat::Interact_Implementation(APawn* User)
{
	Board(Cast<ALBACharacter>(User));
}

bool ALBABoat::Board(ALBACharacter* Hero)
{
	if (!Hero || Rider.IsValid() || Controller)
	{
		return false;
	}
	AController* HeroController = Hero->GetController();
	Rider = Hero;
	Hero->StartRiding(Helm);
	Hero->SetActorHiddenInGame(bRiderInside);
	if (HeroController)
	{
		HeroController->Possess(this);
	}
	CameraBase = FRotator(-14.f, GetActorRotation().Yaw, 0.f);
	LookOffset = FVector2D::ZeroVector;
	BoardTime = GetWorld()->GetTimeSeconds();
	return true;
}

bool ALBABoat::Leave()
{
	ALBACharacter* Hero = Rider.Get();
	if (!Hero || FMath::Abs(Movement->GetForwardSpeed()) > MaxLeaveSpeed || GetWorld()->GetTimeSeconds() - BoardTime < 0.5)
	{
		return false;
	}
	FVector Spot;
	if (!FindLandingSpot(Spot))
	{
		UE_LOG(LogLBA2, Display, TEXT("%s: no land within reach to step onto"), *GetName());
		return false;
	}
	Hero->StopRiding(Spot, (Spot - GetActorLocation()).Rotation());
	Hero->SetActorHiddenInGame(false);
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->RemoveMappingContext(InputContext);
		}
		PC->Possess(Hero);
	}
	Rider = nullptr;
	DriveInput = FVector2D::ZeroVector;
	TriggerInput = 0.f;
	Movement->SetInput(0.f, 0.f);
	return true;
}

bool ALBABoat::FindLandingSpot(FVector& OutLocation) const
{
	UWorld* World = GetWorld();
	const FVector Origin = GetActorLocation();
	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
	const FVector Right(-Forward.Y, Forward.X, 0.0);
	const FVector Extent = Collision->GetScaledBoxExtent();
	const ALBACharacter* Hero = Rider.Get();
	const float Radius = Hero ? Hero->GetCapsuleComponent()->GetScaledCapsuleRadius() : 35.f;
	const float HalfHeight = Hero ? Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 80.f;
	float SeaZ = float(Origin.Z);
	ULBAFlightMovementComponent::FindWaterSurface(World, Origin, SeaZ);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBABoatLanding), false, this);
	Params.AddIgnoredActor(Hero);
	// Sides first (abeam, then towards the bow and the stern), then over the bow and the stern; nearest first.
	struct FSide { FVector Direction; double Edge; double Along; };
	const FSide Sides[] =
	{
		{ Right, Extent.Y, 0.0 }, { -Right, Extent.Y, 0.0 },
		{ Right, Extent.Y, 0.6 * Extent.X }, { -Right, Extent.Y, 0.6 * Extent.X },
		{ Right, Extent.Y, -0.6 * Extent.X }, { -Right, Extent.Y, -0.6 * Extent.X },
		{ Forward, Extent.X, 0.0 }, { -Forward, Extent.X, 0.0 },
	};
	const FCollisionShape Capsule = FCollisionShape::MakeCapsule(Radius, HalfHeight);
	for (double Gap = 30.0; Gap <= LeaveReach; Gap += 70.0)
	{
		for (const FSide& Side : Sides)
		{
			const FVector Along = (Side.Direction == Forward || Side.Direction == -Forward ? Right : Forward) * Side.Along;
			const FVector Point = Origin + Side.Direction * (Side.Edge + Radius + Gap) + Along;
			// The first surface below, but water (Water bodies may block traces).
			FHitResult Ground;
			FCollisionQueryParams GroundParams = Params;
			bool bGround = false;
			for (int32 Attempt = 0; Attempt < 3; ++Attempt)
			{
				bGround = World->LineTraceSingleByChannel(Ground, Point + FVector(0.0, 0.0, 250.0), Point - FVector(0.0, 0.0, 300.0 + MaxWadeDepth), ECC_Visibility, GroundParams);
				if (!bGround || !Cast<AWaterBody>(Ground.GetActor()))
				{
					break;
				}
				GroundParams.AddIgnoredActor(Ground.GetActor());
				bGround = false;
			}
			if (!bGround || Ground.ImpactNormal.Z < GroundNormalZ || Ground.ImpactPoint.Z < SeaZ - MaxWadeDepth || Ground.ImpactPoint.Z > Origin.Z + 250.0)
			{
				continue;
			}
			const FVector Spot = Ground.ImpactPoint + FVector(0.0, 0.0, HalfHeight + 5.0);
			// Room for Twinsen there, and nothing in the way from the deck.
			FHitResult Blocked;
			if (World->OverlapBlockingTestByChannel(Spot, FQuat::Identity, ECC_Pawn, Capsule, Params)
				|| World->LineTraceSingleByChannel(Blocked, Origin + FVector(0.0, 0.0, 2.0 * HalfHeight), Spot, ECC_Visibility, Params))
			{
				continue;
			}
			OutLocation = Spot;
			return true;
		}
	}
	return false;
}

// Input ---------------------------------------------------------------------------------------------------------

void ALBABoat::CreateInput()
{
	InputContext = NewObject<UInputMappingContext>(this, TEXT("IMC_LBA_Boat"));

	DriveAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Boat_Drive"));
	DriveAction->ValueType = EInputActionValueType::Axis2D;
	auto MapDrive = [this](const FKey& Key, bool bSwizzle, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(DriveAction, Key);
		if (bSwizzle)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(InputContext));
		}
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
		}
	};
	MapDrive(EKeys::Up, true, false);
	MapDrive(EKeys::W, true, false);
	MapDrive(EKeys::Down, true, true);
	MapDrive(EKeys::S, true, true);
	MapDrive(EKeys::Right, false, false);
	MapDrive(EKeys::D, false, false);
	MapDrive(EKeys::Left, false, true);
	MapDrive(EKeys::A, false, true);
	auto MapStick = [this](UInputAction* Action, const FKey& Key)
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(Action, Key);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->LowerThreshold = 0.2f;
		Mapping.Modifiers.Add(DeadZone);
	};
	MapStick(DriveAction, EKeys::Gamepad_Left2D);

	TriggerAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Boat_Triggers"));
	TriggerAction->ValueType = EInputActionValueType::Axis1D;
	InputContext->MapKey(TriggerAction, EKeys::Gamepad_RightTriggerAxis);
	InputContext->MapKey(TriggerAction, EKeys::Gamepad_LeftTriggerAxis).Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));

	LookAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Boat_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	InputContext->MapKey(LookAction, EKeys::Mouse2D);
	{
		// The stick turns the camera at a rate (degrees/s), the mouse by its movement.
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(LookAction, EKeys::Gamepad_Right2D);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->LowerThreshold = 0.2f;
		UInputModifierScalar* Rate = NewObject<UInputModifierScalar>(InputContext);
		// Vertical inverted on the stick (pushing up looks down), not on the mouse.
		Rate->Scalar = FVector(180.0, -120.0, 1.0);
		Mapping.Modifiers.Add(DeadZone);
		Mapping.Modifiers.Add(NewObject<UInputModifierScaleByDeltaTime>(InputContext));
		Mapping.Modifiers.Add(Rate);
	}

	// The key that got on.
	LeaveAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Boat_Leave"));
	LeaveAction->ValueType = EInputActionValueType::Boolean;
	for (const FKey& Key : LBAControls::Use())
	{
		InputContext->MapKey(LeaveAction, Key);
	}
}

void ALBABoat::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!InputContext)
	{
		CreateInput();
	}
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogLBA2, Warning, TEXT("ALBABoat needs Enhanced Input (Project Settings > Input > Default Input Component Class)."));
		return;
	}
	Input->BindAction(DriveAction, ETriggerEvent::Triggered, this, &ALBABoat::OnDrive);
	Input->BindAction(DriveAction, ETriggerEvent::Completed, this, &ALBABoat::OnDriveReleased);
	Input->BindAction(TriggerAction, ETriggerEvent::Triggered, this, &ALBABoat::OnTrigger);
	Input->BindAction(TriggerAction, ETriggerEvent::Completed, this, &ALBABoat::OnTriggerReleased);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ALBABoat::OnLook);
	Input->BindAction(LeaveAction, ETriggerEvent::Started, this, &ALBABoat::OnLeave);
}

void ALBABoat::PawnClientRestart()
{
	Super::PawnClientRestart();
	if (!InputContext)
	{
		CreateInput();
	}
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			// Above the hero's context, whose keys overlap.
			Subsystem->AddMappingContext(InputContext, 2);
		}
	}
}

void ALBABoat::OnDrive(const FInputActionValue& Value)
{
	DriveInput = Value.Get<FVector2D>();
}

void ALBABoat::OnTrigger(const FInputActionValue& Value)
{
	TriggerInput = Value.Get<float>();
}

void ALBABoat::OnLook(const FInputActionValue& Value)
{
	const FVector2D Look = Value.Get<FVector2D>();
	// All the way around, as many times as wanted.
	LookOffset.X = FRotator::NormalizeAxis(LookOffset.X + Look.X);
	LookOffset.Y = FMath::Clamp(LookOffset.Y + Look.Y, -60.f, 60.f);
}
