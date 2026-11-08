#include "LBACar.h"

#include "Camera/CameraComponent.h"
#include "Chaos/PBDSuspensionConstraintData.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "PhysicsProxy/SuspensionConstraintProxy.h"
#include "ChaosWheeledVehicleMovementComponent.h"
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
	TAutoConsoleVariable<bool> CVarCarDebug(
		TEXT("lba.Car.Debug"), false,
		TEXT("Shows the cars' speed, gear, engine speed and wheel contacts as text above them (debug builds)."));

	FAutoConsoleCommandWithWorldAndArgs CarSpawnCommand(
		TEXT("lba.Car.Spawn"),
		TEXT("Puts a car (or 'lba.Car.Spawn scooter': a scooter) 4 m in front of the player."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Pawn)
			{
				return;
			}
			const bool bScooter = Args.Num() > 0 && Args[0].Equals(TEXT("scooter"), ESearchCase::IgnoreCase);
			const FVector Forward = Pawn->GetActorForwardVector().GetSafeNormal2D();
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			World->SpawnActor<ALBACar>(bScooter ? ALBAScooter::StaticClass() : ALBACar::StaticClass(), Pawn->GetActorLocation() + Forward * 400.f,
				FRotator(0.f, Forward.Rotation().Yaw + 90.f, 0.f), Params);
		}));

	/** Normals steeper than this are ground Twinsen can get out onto. */
	constexpr float GroundNormalZ = 0.7f;
	/** Placed with nothing under it: it keeps looking this long (s), then simulates anyway. */
	constexpr double MaxSettleTime = 10.0;
	/** Free look eases back behind the car this long after the last look input (s). */
	constexpr double LookReturnDelay = 1.5;
	constexpr float MetersPerMile = 1609.344f;

	/** The buggy's groups (BUGGY.CPP DoAnimBuggy): steering, rolling wheels, steering wheel, Twinsen's head. */
	namespace BuggyGroups
	{
		constexpr int32 SteerLeft = 3;
		constexpr int32 SteerRight = 6;
		constexpr int32 FrontWheels[] = { 4, 7 };
		constexpr int32 RearWheels[] = { 9, 11 };
		constexpr int32 SteeringWheel = 12;
		constexpr int32 Head = 14;
	}
}

// Geometry ------------------------------------------------------------------------------------------------------

const FLBACarGeometry FLBACarGeometry::Buggy = []
{
	FLBACarGeometry Geometry;
	Geometry.FrontWheel = FVector(50.0, 50.0, 16.25);
	Geometry.RearWheel = FVector(-41.6, 50.0, 22.5);
	Geometry.FrontRadius = 16.25f;
	Geometry.RearRadius = 22.5f;
	Geometry.WheelWidth = 16.f;
	// The body (1.66 m wide with its fenders, 1.54 m long, 0.87 m high): a box inside the wheels, from 14 to 56 cm up.
	Geometry.ChassisCenter = FVector(10.0, 0.0, 35.0);
	Geometry.ChassisExtent = FVector(72.0, 42.0, 21.0);
	Geometry.bRearWheelDrive = true;
	return Geometry;
}();

const FLBACarGeometry FLBACarGeometry::Scooter = []
{
	FLBACarGeometry Geometry;
	// 0.78 m wide, 1.46 m long; the skirt reaches the ground, so the wheels hide inside it.
	Geometry.FrontWheel = FVector(45.0, 20.0, 12.0);
	Geometry.RearWheel = FVector(-45.0, 20.0, 12.0);
	Geometry.FrontRadius = 12.f;
	Geometry.RearRadius = 12.f;
	Geometry.WheelWidth = 8.f;
	Geometry.ChassisCenter = FVector(0.0, 0.0, 40.0);
	Geometry.ChassisExtent = FVector(66.0, 30.0, 26.0);
	Geometry.bRearWheelDrive = false;
	return Geometry;
}();

FLBACarDrive FLBACarGeometry::ComputeDrive(const FLBACarTuning& Tuning) const
{
	const float DriveRadius = bRearWheelDrive ? RearRadius : 0.5f * (FrontRadius + RearRadius);
	return FLBACarDrive::Compute(Tuning, DriveRadius, 0.5f * (FrontRadius + RearRadius), GetWheelbase());
}

// Suspension on a box root ------------------------------------------------------------------------------------

void ULBACarMovementComponent::FixupSkeletalMesh()
{
	Super::FixupSkeletalMesh();
	ReleaseRootSuspension();
	// A skeletal root with a physics asset gets Chaos's own constraints.
	if (Cast<USkeletalMeshComponent>(GetMesh()) || !UpdatedPrimitive || !VehicleSimulationPT)
	{
		return;
	}
	FBodyInstance* Target = UpdatedPrimitive->GetBodyInstance();
	if (!Target)
	{
		return;
	}
	// As UChaosWheeledVehicleMovementComponent::FixupSkeletalMesh sets them up for each wheel.
	for (int32 WheelIdx = 0; WheelIdx < WheelSetups.Num() && WheelIdx < Wheels.Num(); ++WheelIdx)
	{
		const FChaosWheelSetup& WheelSetup = WheelSetups[WheelIdx];
		UChaosVehicleWheel* Wheel = Wheels[WheelIdx];
		FPhysicsCommand::ExecuteWrite(Target->GetPhysicsActor(), [&](const FPhysicsActorHandle& Chassis)
		{
			const FVector LocalWheel = GetWheelRestingPosition(WheelSetup);
			FPhysicsConstraintHandle Handle = FPhysicsInterface::CreateSuspension(Chassis, LocalWheel);
			if (!Handle.IsValid())
			{
				return;
			}
			RootSuspension.Add(Handle);
			if (Chaos::FSuspensionConstraint* Constraint = static_cast<Chaos::FSuspensionConstraint*>(Handle.Constraint))
			{
				Constraint->SetHardstopStiffness(1.0f);
				Constraint->SetSpringStiffness(Chaos::MToCm(Wheel->SpringRate) * 0.25f);
				Constraint->SetSpringPreload(Chaos::MToCm(Wheel->SpringPreload));
				Constraint->SetSpringDamping(Wheel->SuspensionDampingRatio * 5.0f);
				Constraint->SetMinLength(-Wheel->SuspensionMaxRaise);
				Constraint->SetMaxLength(Wheel->SuspensionMaxDrop);
				Constraint->SetAxis(-Wheel->SuspensionAxis);
			}
		});
	}
	VehicleSimulationPT->UpdateConstraintHandles(RootSuspension);
}

void ULBACarMovementComponent::OnDestroyPhysicsState()
{
	ReleaseRootSuspension();
	Super::OnDestroyPhysicsState();
}

void ULBACarMovementComponent::ReleaseRootSuspension()
{
	for (FPhysicsConstraintHandle& Handle : RootSuspension)
	{
		FPhysicsCommand::ExecuteWrite(Handle, [&](const FPhysicsConstraintHandle& Constraint)
		{
			FPhysicsInterface::ReleaseConstraint(Handle);
		});
	}
	RootSuspension.Empty();
}

// Wheels --------------------------------------------------------------------------------------------------------

ULBACarWheel::ULBACarWheel()
{
	// Sphere sweeps ride over brick edges and kerbs better than rays; the islands' ground is complex collision.
	SweepShape = ESweepShape::Spherecast;
	SweepType = ESweepType::ComplexSweep;
	// The body is drawn at its rest pose: the wheels may rise 6 cm into it but drop only 3 cm, so it never floats far
	// above the ground (stiff springs pushed the wheels to a full 8 cm drop and the chassis rode that much high).
	SuspensionMaxRaise = 6.f;
	SuspensionMaxDrop = 3.f;
	SuspensionDampingRatio = 0.6f;
	// Chaos needs the suspension constraints ULBACarMovementComponent builds for the box root: without them the chassis
	// lay on the ground and slid on it (the car stopped within a second when let go).
	SpringRate = 400.f;
	SpringPreload = 0.f;
	CorneringStiffness = 1000.f;
	FrictionForceMultiplier = 2.f;
	MaxBrakeTorque = 400.f;
	MaxHandBrakeTorque = 0.f;
	bABSEnabled = false;
	// Spinning wheels lose almost all their grip in Chaos's tire model: traction control holds the drive at the grip.
	bTractionControlEnabled = true;
}

void ULBACarWheel::SetGeometry(float Radius, float Width, bool bFront, bool bDriven)
{
	WheelRadius = Radius;
	WheelWidth = Width;
	WheelMass = 5.f;
	AxleType = bFront ? EAxleType::Front : EAxleType::Rear;
	bAffectedBySteering = bFront;
	MaxSteerAngle = bFront ? 30.f : 0.f;
	bAffectedByBrake = true;
	bAffectedByHandbrake = !bFront;
	MaxHandBrakeTorque = bFront ? 0.f : 1500.f;
	bAffectedByEngine = bDriven;
}

ULBABuggyFrontWheel::ULBABuggyFrontWheel()
{
	SetGeometry(FLBACarGeometry::Buggy.FrontRadius, FLBACarGeometry::Buggy.WheelWidth, true, !FLBACarGeometry::Buggy.bRearWheelDrive);
}

ULBABuggyRearWheel::ULBABuggyRearWheel()
{
	SetGeometry(FLBACarGeometry::Buggy.RearRadius, FLBACarGeometry::Buggy.WheelWidth, false, true);
}

ULBAScooterFrontWheel::ULBAScooterFrontWheel()
{
	SetGeometry(FLBACarGeometry::Scooter.FrontRadius, FLBACarGeometry::Scooter.WheelWidth, true, !FLBACarGeometry::Scooter.bRearWheelDrive);
	// Two thirds of the buggy's weight.
	SpringRate = 270.f;
}

ULBAScooterRearWheel::ULBAScooterRearWheel()
{
	SetGeometry(FLBACarGeometry::Scooter.RearRadius, FLBACarGeometry::Scooter.WheelWidth, false, true);
	// Two thirds of the buggy's weight.
	SpringRate = 270.f;
}

// Car -----------------------------------------------------------------------------------------------------------

ALBACar::ALBACar()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessAI = EAutoPossessAI::Disabled;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	Chassis = CreateDefaultSubobject<UBoxComponent>(TEXT("Chassis"));
	Chassis->InitBoxExtent(FLBACarGeometry::Buggy.ChassisExtent);
	Chassis->SetCollisionProfileName(UCollisionProfile::Vehicle_ProfileName);
	Chassis->SetSimulatePhysics(true);
	Chassis->BodyInstance.bUseCCD = true;
	Chassis->SetCanEverAffectNavigation(false);
	Chassis->SetHiddenInGame(true);
	RootComponent = Chassis;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Chassis);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Mesh->SetAnimInstanceClass(ULBAAnimInstance::StaticClass());
	Mesh->SetRenderCustomDepth(true);
	Mesh->SetCustomDepthStencilValue(LBA::MovingObjectStencil);
	Mesh->SetLightingChannels(true, true, false);
	// Wheels turn even when the camera looks away for a moment (the pose is cheap: a few bones).
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPose;

	Seat = CreateDefaultSubobject<USceneComponent>(TEXT("Seat"));
	Seat->SetupAttachment(Chassis);

	Boarding = CreateDefaultSubobject<UBoxComponent>(TEXT("Boarding"));
	Boarding->SetupAttachment(Chassis);
	Boarding->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Boarding->SetCollisionObjectType(ECC_WorldDynamic);
	Boarding->SetCollisionResponseToAllChannels(ECR_Ignore);
	Boarding->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Boarding->SetGenerateOverlapEvents(false);
	Boarding->SetCanEverAffectNavigation(false);

	VehicleMovement = CreateDefaultSubobject<ULBACarMovementComponent>(TEXT("VehicleMovement"));
	VehicleMovement->SetUpdatedComponent(Chassis);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Chassis);
	CameraBoom->SetUsingAbsoluteRotation(true);
	CameraBoom->TargetArmLength = 480.f;
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 90.f);
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 10.f;
	CameraBoom->ProbeSize = 15.f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->SetFieldOfView(80.f);
}

UPawnMovementComponent* ALBACar::GetMovementComponent() const
{
	return VehicleMovement;
}

TSubclassOf<UChaosVehicleWheel> ALBACar::GetFrontWheelClass() const
{
	return ULBABuggyFrontWheel::StaticClass();
}

TSubclassOf<UChaosVehicleWheel> ALBACar::GetRearWheelClass() const
{
	return ULBABuggyRearWheel::StaticClass();
}

ULBAEntityAsset* ALBACar::GetDefaultEntity(bool bDriven) const
{
	const ULBA2Settings* Settings = GetDefault<ULBA2Settings>();
	return (bDriven ? Settings->CarDriven : Settings->Car).LoadSynchronous();
}

void ALBACar::PreRegisterAllComponents()
{
	// Before the chassis' physics (and the vehicle with it) is created.
	ApplyVehicleSetup();
	Super::PreRegisterAllComponents();
}

void ALBACar::ApplyVehicleSetup()
{
	const FLBACarGeometry& Geometry = GetGeometry();
	const FLBACarTuning& Tuning = GetTuning();
	const FLBACarDrive Drive = Geometry.ComputeDrive(Tuning);

	// The root is the chassis box; the car's ground origin is under its center.
	const FVector Ground = -Geometry.ChassisCenter;
	Chassis->SetBoxExtent(Geometry.ChassisExtent, false);
	Mesh->SetRelativeLocationAndRotation(Ground, FRotator(0.f, -90.f, 0.f));
	Seat->SetRelativeLocation(Ground + FVector(-10.0, 0.0, 25.0));
	Boarding->SetBoxExtent(Geometry.ChassisExtent + FVector(110.0, 110.0, 60.0), false);

	ULBACarMovementComponent* Vehicle = VehicleMovement;
	Vehicle->Mass = Tuning.Mass;
	Vehicle->bEnableCenterOfMassOverride = true;
	Vehicle->CenterOfMassOverride = Ground + FVector(0.5 * (Geometry.FrontWheel.X + Geometry.RearWheel.X), 0.0, Tuning.CenterOfMassHeight);
	Vehicle->ChassisWidth = float(Geometry.ChassisExtent.Y * 2.0);
	Vehicle->ChassisHeight = float(Geometry.ChassisExtent.Z * 2.0);
	Vehicle->DragCoefficient = 0.1f;
	Vehicle->DownforceCoefficient = 0.f;
	Vehicle->SetArcadeBrakes();
	// Coasting is a drag force (UpdateInputs), not Chaos's idle brake.
	Vehicle->SetIdleBrake(0.f);
	Vehicle->SetRequiresControllerForInputs(false);

	Vehicle->WheelSetups.SetNum(4);
	const FVector Mirror(1.0, -1.0, 1.0);
	const FVector Wheels[] = { Geometry.FrontWheel * Mirror, Geometry.FrontWheel, Geometry.RearWheel * Mirror, Geometry.RearWheel };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FChaosWheelSetup& Setup = Vehicle->WheelSetups[Index];
		Setup.WheelClass = Index < 2 ? GetFrontWheelClass() : GetRearWheelClass();
		// Chaos wants a name; on a box root (no skeleton) it places the wheel by AdditionalOffset alone.
		static const FName WheelNames[] = { TEXT("WheelFrontLeft"), TEXT("WheelFrontRight"), TEXT("WheelRearLeft"), TEXT("WheelRearRight") };
		Setup.BoneName = WheelNames[Index];
		Setup.AdditionalOffset = Wheels[Index] + Ground;
	}

	FVehicleEngineConfig& Engine = Vehicle->EngineSetup;
	Engine.MaxTorque = Drive.MaxTorque;
	Engine.MaxRPM = FLBACarDrive::MaxRPM;
	Engine.EngineIdleRPM = 900.f;
	// Chaos brakes the driven wheels by engine rpm x this (N m): 0.02 stopped the buggy as hard as its brakes. Coasting
	// comes from the idle brake (FLBACarTuning::Coasting) instead.
	Engine.EngineBrakeEffect = 0.002f;
	FRichCurve* Torque = Engine.TorqueCurve.GetRichCurve();
	Torque->Reset();
	Torque->AddKey(0.f, 1.f);
	Torque->AddKey(0.95f * FLBACarDrive::MaxRPM, 1.f);
	Torque->AddKey(FLBACarDrive::MaxRPM, 0.25f);

	Vehicle->DifferentialSetup.DifferentialType = Geometry.bRearWheelDrive ? EVehicleDifferential::RearWheelDrive : EVehicleDifferential::AllWheelDrive;
	Vehicle->DifferentialSetup.FrontRearSplit = 0.5f;

	FVehicleTransmissionConfig& Gearbox = Vehicle->TransmissionSetup;
	Gearbox.bUseAutomaticGears = true;
	Gearbox.bUseAutoReverse = true;
	Gearbox.FinalRatio = Drive.FinalRatio;
	Gearbox.ForwardGearRatios = { FLBACarDrive::Gears[0], FLBACarDrive::Gears[1], FLBACarDrive::Gears[2] };
	Gearbox.ReverseGearRatios = { Drive.ReverseRatio };
	Gearbox.ChangeUpRPM = 0.92f * FLBACarDrive::MaxRPM;
	Gearbox.ChangeDownRPM = 0.4f * FLBACarDrive::MaxRPM;
	Gearbox.GearChangeTime = FLBACarDrive::GearChangeTime;
	Gearbox.TransmissionEfficiency = FLBACarDrive::Efficiency;

	// Steering eases off with the speed (the curve's speeds are in mph).
	FVehicleSteeringConfig& Steering = Vehicle->SteeringSetup;
	Steering.SteeringType = ESteeringType::SingleAngle;
	const float TopMph = Tuning.TopSpeed / 100.f * 3600.f / MetersPerMile;
	FRichCurve* SteerCurve = Steering.SteeringCurve.GetRichCurve();
	SteerCurve->Reset();
	SteerCurve->AddKey(0.f, 1.f);
	SteerCurve->AddKey(0.4f * TopMph, FMath::Lerp(1.f, Tuning.SteeringAtTopSpeed, 0.6f));
	SteerCurve->AddKey(TopMph, Tuning.SteeringAtTopSpeed);
	SteerCurve->AddKey(2.f * TopMph, Tuning.SteeringAtTopSpeed);

	// Quick to steer, quick back to straight (the defaults are a heavy car's).
	Vehicle->SteeringInputRate.RiseRate = 4.f;
	Vehicle->SteeringInputRate.FallRate = 6.f;
	Vehicle->ThrottleInputRate.RiseRate = 6.f;
	Vehicle->ThrottleInputRate.FallRate = 10.f;
	Vehicle->BrakeInputRate.RiseRate = 8.f;
	Vehicle->BrakeInputRate.FallRate = 12.f;
}

void ALBACar::ApplyWheelTuning()
{
	const FLBACarTuning& Tuning = GetTuning();
	const FLBACarDrive Drive = GetGeometry().ComputeDrive(Tuning);
	for (int32 Index = 0; Index < VehicleMovement->Wheels.Num(); ++Index)
	{
		const bool bFront = Index < 2;
		VehicleMovement->SetWheelMaxBrakeTorque(Index, Drive.BrakeTorque);
		VehicleMovement->SetWheelHandbrakeTorque(Index, bFront ? 0.f : 3.f * Drive.BrakeTorque);
		VehicleMovement->SetWheelFrictionMultiplier(Index, Tuning.Grip);
		VehicleMovement->SetWheelMaxSteerAngle(Index, bFront ? Tuning.SteeringAngle : 0.f);
	}
}

void ALBACar::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyBody(bDrivenBody);
}

void ALBACar::ApplyBody(bool bDriven)
{
	bDrivenBody = bDriven;
	ULBAEntityAsset* Body = bDriven ? (DrivenEntity ? DrivenEntity.Get() : GetDefaultEntity(true)) : (Entity ? Entity.Get() : GetDefaultEntity(false));
	if (!Body)
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: no car entity. Import the Characters layer (Tools > LBA2 Importer)."), *GetName());
		return;
	}
	USkeletalMesh* BodyMesh = Body->GetBodyMesh(bDriven ? DrivenBodyType : BodyType);
	BodyMesh = BodyMesh ? BodyMesh : Body->GetDefaultBodyMesh();
	if (BodyMesh && Mesh->GetSkeletalMeshAsset() != BodyMesh)
	{
		Mesh->SetSkeletalMesh(BodyMesh);
	}
	ULBAAnimInstance* Anim = Cast<ULBAAnimInstance>(Mesh->GetAnimInstance());
	const FLBAEntityAnim* Idle = Body->Anims.Find(int32(ELBAAnimType::Idle));
	if (Anim && Idle && Idle->Sequence)
	{
		Anim->SetApplyRootMotion(false);
		Anim->PlayLBAAnimation(Idle->Sequence, true, Idle->LoopStartTime);
	}
}

FName ALBACar::GetGroupBone(int32 Group) const
{
	const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	return Asset && Asset->GetRefSkeleton().IsValidIndex(Group) ? Asset->GetRefSkeleton().GetBoneName(Group) : NAME_None;
}

void ALBACar::BeginPlay()
{
	Super::BeginPlay();
	ApplyBody(false);
	ApplyWheelTuning();
	CameraBase = FRotator(-12.f, GetActorRotation().Yaw, 0.f);
	// Held where it is (no gravity) until it stands on the ground (TrySettle). Its simulation stays on: the vehicle is
	// built for a simulating body, and turning it off and on again left Chaos's physics side idle.
	Chassis->SetEnableGravity(false);
	bSettled = false;
	SettleStart = GetWorld()->GetTimeSeconds();
	TrySettle();
}

bool ALBACar::TrySettle()
{
	UWorld* World = GetWorld();
	const FLBACarGeometry& Geometry = GetGeometry();
	// The ground origin under the chassis, as placed.
	const FVector Origin = GetActorLocation() - GetActorQuat().RotateVector(Geometry.ChassisCenter);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBACarSettle), true, this);
	FHitResult Hit;
	bool bGround = false;
	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		// What blocks static traces (not trigger volumes such as camera zones).
		bGround = World->LineTraceSingleByChannel(Hit, Origin + FVector(0.0, 0.0, 250.0), Origin - FVector(0.0, 0.0, 6000.0), ECC_WorldStatic, Params);
		// Not on water (Water bodies may answer), another car or a character.
		if (!bGround || !(Cast<AWaterBody>(Hit.GetActor()) || Cast<APawn>(Hit.GetActor())))
		{
			break;
		}
		Params.AddIgnoredActor(Hit.GetActor());
		bGround = false;
	}
	if (!bGround && World->GetTimeSeconds() - SettleStart < MaxSettleTime)
	{
		Chassis->SetPhysicsLinearVelocity(FVector::ZeroVector);
		Chassis->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
		return false;
	}
	if (bGround)
	{
		const FRotator Facing(0.f, GetActorRotation().Yaw, 0.f);
		const FVector Location = Hit.ImpactPoint + Facing.RotateVector(Geometry.ChassisCenter) + FVector(0.0, 0.0, 3.0);
		if (FVector::Dist(Location, GetActorLocation()) > 50.0)
		{
			UE_LOG(LogLBA2, Display, TEXT("%s: set down on %s, %.1f m %s where it was placed"), *GetName(), *GetNameSafe(Hit.GetActor()),
				FMath::Abs(Location.Z - GetActorLocation().Z) / 100.0, Location.Z < GetActorLocation().Z ? TEXT("below") : TEXT("above"));
		}
		SetActorLocationAndRotation(Location, Facing, false, nullptr, ETeleportType::TeleportPhysics);
	}
	else
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: no ground under it after %.0f s, simulating where it is"), *GetName(), MaxSettleTime);
	}
	Chassis->SetEnableGravity(true);
	Chassis->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Chassis->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
	bSettled = true;
	return true;
}

float ALBACar::GetForwardSpeed() const
{
	return VehicleMovement ? VehicleMovement->GetForwardSpeed() : 0.f;
}

void ALBACar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bSettled && !TrySettle())
	{
		return;
	}
	UpdateInputs(DeltaSeconds);
	UpdateSelfRighting(DeltaSeconds);
	UpdateBodyHeight(DeltaSeconds);
	UpdateBodyPose(DeltaSeconds);
	if (IsPlayerControlled())
	{
		UpdateCamera(DeltaSeconds);
	}
#if ENABLE_DRAW_DEBUG
	if (CVarCarDebug.GetValueOnGameThread())
	{
		DrawDebug();
	}
#endif
}

void ALBACar::UpdateInputs(float DeltaSeconds)
{
	if (!Driver.IsValid())
	{
		// Parked: handbrake on.
		VehicleMovement->SetThrottleInput(0.f);
		VehicleMovement->SetBrakeInput(0.f);
		VehicleMovement->SetSteeringInput(0.f);
		VehicleMovement->SetHandbrakeInput(true);
		return;
	}
	float Throttle = FMath::Clamp(DriveInput.Y + TriggerInput, -1.f, 1.f);

	// Water drowns the engine: it pulls less and less from WaterDepthStall to MaxWaterDepth.
	const ULBACarSettings& Settings = ULBACarSettings::Get();
	const FVector Ground = GetActorLocation() - GetActorQuat().RotateVector(GetGeometry().ChassisCenter);
	float SurfaceZ = 0.f;
	if (ULBAFlightMovementComponent::FindWaterSurface(GetWorld(), Ground, SurfaceZ) && SurfaceZ > Ground.Z + Settings.WaterDepthStall)
	{
		const float Depth = float(SurfaceZ - Ground.Z);
		const float Pull = 1.f - FMath::Clamp((Depth - Settings.WaterDepthStall) / FMath::Max(Settings.MaxWaterDepth - Settings.WaterDepthStall, 1.f), 0.f, 1.f);
		// Backing out of it still works a little.
		Throttle *= Throttle > 0.f ? Pull : FMath::Max(Pull, 0.4f);
	}

	// Chaos wakes only skeletal mesh roots on input: the box chassis is kept awake while driven (the physics side skips
	// sleeping bodies).
	Chassis->WakeRigidBody();
	VehicleMovement->SetThrottleInput(FMath::Max(Throttle, 0.f));
	VehicleMovement->SetBrakeInput(FMath::Max(-Throttle, 0.f));
	VehicleMovement->SetSteeringInput(FMath::Clamp(DriveInput.X, -1.f, 1.f));
	VehicleMovement->SetHandbrakeInput(bHandbrakeInput);

	// Coasting: a drag on the chassis, not Chaos's idle brake (any brake locks these small light wheels, which then slide
	// at full grip: the car stopped within a second).
	const float Speed = GetForwardSpeed();
	if (FMath::Abs(Throttle) < 0.05f && !bHandbrakeInput && FMath::Abs(Speed) > 20.f && VehicleMovement->GetWheelState(0).bInContact)
	{
		const float Coasting = FMath::Min(GetTuning().Coasting, FMath::Abs(Speed) / FMath::Max(DeltaSeconds, 1e-3f));
		Chassis->AddForce(-GetActorForwardVector() * FMath::Sign(Speed) * Coasting * Chassis->GetMass());
	}
}

void ALBACar::UpdateSelfRighting(float DeltaSeconds)
{
	// On its side or roof and nearly still: back on its wheels.
	const float UpZ = float(GetActorUpVector().Z);
	const bool bStuck = UpZ < 0.3f && Chassis->GetPhysicsLinearVelocity().Size() < 100.0;
	UpsideDownTime = bStuck ? UpsideDownTime + DeltaSeconds : 0.f;
	if (UpsideDownTime < ULBACarSettings::Get().SelfRightTime)
	{
		return;
	}
	UpsideDownTime = 0.f;
	const FRotator Upright(0.f, GetActorRotation().Yaw, 0.f);
	SetActorLocationAndRotation(GetActorLocation() + FVector(0.0, 0.0, 60.0), Upright, false, nullptr, ETeleportType::TeleportPhysics);
	Chassis->SetPhysicsLinearVelocity(FVector::ZeroVector);
	Chassis->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
	UE_LOG(LogLBA2, Display, TEXT("%s: back on its wheels"), *GetName());
}

void ALBACar::UpdateBodyHeight(float DeltaSeconds)
{
	// The wheels hang a little below their rest place when they carry less (the springs push the chassis up): the body,
	// drawn with its wheels at rest, follows them down so they stay on the ground.
	float Offset = 0.f;
	int32 OnGround = 0;
	for (int32 Index = 0; Index < VehicleMovement->Wheels.Num(); ++Index)
	{
		if (VehicleMovement->GetWheelState(Index).bInContact)
		{
			Offset += VehicleMovement->GetSuspensionOffset(Index);
			++OnGround;
		}
	}
	const float Target = OnGround > 0 ? FMath::Min(Offset / OnGround, 0.f) : 0.f;
	BodyDrop = FMath::FInterpTo(BodyDrop, Target, DeltaSeconds, 15.f);
	Mesh->SetRelativeLocation(FVector(-GetGeometry().ChassisCenter.X, -GetGeometry().ChassisCenter.Y, BodyDrop - GetGeometry().ChassisCenter.Z));
}

void ALBACar::UpdateBodyPose(float DeltaSeconds)
{
	ULBAAnimInstance* Anim = Cast<ULBAAnimInstance>(Mesh->GetAnimInstance());
	if (!Anim || !bDrivenBody)
	{
		return;
	}
	const FLBACarGeometry& Geometry = GetGeometry();
	const float Speed = GetForwardSpeed();
	// Rolled distance over the wheel's circumference; the steering as the front wheels show it.
	FrontWheelAngle = FMath::Fmod(FrontWheelAngle + FMath::RadiansToDegrees(Speed * DeltaSeconds / Geometry.FrontRadius), 360.f);
	RearWheelAngle = FMath::Fmod(RearWheelAngle + FMath::RadiansToDegrees(Speed * DeltaSeconds / Geometry.RearRadius), 360.f);
	const float SteerShare = VehicleMovement->Wheels.Num() > 0 ? VehicleMovement->Wheels[0]->GetSteerAngle() : 0.f;
	ShownSteer = FMath::FInterpTo(ShownSteer, SteerShare, DeltaSeconds, 12.f);

	// Component space of the body: +Y forward, +X to the car's left, +Z up. Rolling forward turns a wheel's top
	// towards +Y: about +X, negative. Steering right turns the wheels' front towards -X: about +Z, positive.
	FLBAProceduralPose Pose;
	Pose.Weight = 1.f;
	const FQuat Steer(FVector::UpVector, FMath::DegreesToRadians(ShownSteer));
	const FVector FrontAxle = Steer.RotateVector(FVector(1.f, 0.f, 0.f));
	Pose.BoneRotations.Emplace(GetGroupBone(BuggyGroups::SteerLeft), Steer);
	Pose.BoneRotations.Emplace(GetGroupBone(BuggyGroups::SteerRight), Steer);
	for (int32 Group : BuggyGroups::FrontWheels)
	{
		Pose.BoneRotations.Emplace(GetGroupBone(Group), FQuat(FrontAxle, -FMath::DegreesToRadians(FrontWheelAngle)));
	}
	for (int32 Group : BuggyGroups::RearWheels)
	{
		Pose.BoneRotations.Emplace(GetGroupBone(Group), FQuat(FVector(1.f, 0.f, 0.f), -FMath::DegreesToRadians(RearWheelAngle)));
	}
	// The steering wheel turns with the wheels (about the column, roughly the car's long axis), the head looks into
	// the bend (BUGGY.CPP: both by the front wheels' angle).
	Pose.BoneRotations.Emplace(GetGroupBone(BuggyGroups::SteeringWheel), FQuat(FVector(0.f, 1.f, 0.f), -FMath::DegreesToRadians(2.f * ShownSteer)));
	Pose.BoneRotations.Emplace(GetGroupBone(BuggyGroups::Head), FQuat(FVector::UpVector, FMath::DegreesToRadians(ShownSteer)));
	Pose.BoneRotations.RemoveAll([](const TPair<FName, FQuat>& Rotation) { return Rotation.Key.IsNone(); });
	Anim->SetProceduralPose(Pose);
}

void ALBACar::UpdateCamera(float DeltaSeconds)
{
	const float SpeedAlpha = FMath::Clamp(FMath::Abs(GetForwardSpeed()) / FMath::Max(GetTuning().TopSpeed, 1.f), 0.f, 1.f);
	// Behind the car, swinging after it with some lag (backing up too: the camera keeps looking the way it faces).
	CameraBase = FMath::RInterpTo(CameraBase, FRotator(-12.f, GetActorRotation().Yaw, 0.f), DeltaSeconds, CameraFollowSpeed);
	// Free look eases back behind after a moment without looking.
	if (GetWorld()->GetTimeSeconds() - LastLookTime > LookReturnDelay)
	{
		LookOffset = FMath::Vector2DInterpTo(LookOffset, FVector2D::ZeroVector, DeltaSeconds, 2.f);
	}
	CameraBoom->SetWorldRotation(FRotator(FMath::Clamp(CameraBase.Pitch + LookOffset.Y, -80.f, 50.f), CameraBase.Yaw + LookOffset.X, 0.f));
	CameraBoom->TargetArmLength = FMath::FInterpTo(CameraBoom->TargetArmLength, 450.f + 200.f * SpeedAlpha, DeltaSeconds, 2.f);
	FollowCamera->SetFieldOfView(FMath::FInterpTo(FollowCamera->FieldOfView, 80.f + 15.f * SpeedAlpha, DeltaSeconds, 3.f));
}

void ALBACar::DrawDebug() const
{
	int32 OnGround = 0;
	for (int32 Index = 0; Index < VehicleMovement->Wheels.Num(); ++Index)
	{
		OnGround += VehicleMovement->GetWheelState(Index).bInContact ? 1 : 0;
	}
	DrawDebugString(GetWorld(), FVector(0.f, 0.f, 150.f), FString::Printf(TEXT("%.1f m/s  gear %d  %.0f rpm  wheels %d/4  %s"),
		GetForwardSpeed() / 100.f, VehicleMovement->GetCurrentGear(), VehicleMovement->GetEngineRotationSpeed(), OnGround,
		bSettled ? TEXT("") : TEXT("settling")), const_cast<ALBACar*>(this), FColor::Cyan, 0.f);
}

// Riding --------------------------------------------------------------------------------------------------------

bool ALBACar::CanInteract_Implementation(APawn* User) const
{
	return bSettled && !Driver.IsValid() && !Controller && Cast<ALBACharacter>(User) && GetActorUpVector().Z > 0.5;
}

void ALBACar::Interact_Implementation(APawn* User)
{
	Board(Cast<ALBACharacter>(User));
}

bool ALBACar::Board(ALBACharacter* Hero)
{
	if (!Hero || Driver.IsValid() || Controller)
	{
		return false;
	}
	AController* HeroController = Hero->GetController();
	Driver = Hero;
	Hero->StartRiding(Seat);
	// The driven body has Twinsen in it.
	Hero->SetActorHiddenInGame(true);
	ApplyBody(true);
	if (HeroController)
	{
		HeroController->Possess(this);
	}
	CameraBase = FRotator(-12.f, GetActorRotation().Yaw, 0.f);
	LookOffset = FVector2D::ZeroVector;
	BoardTime = GetWorld()->GetTimeSeconds();
	bHandbrakeInput = false;
	return true;
}

bool ALBACar::Leave()
{
	ALBACharacter* Hero = Driver.Get();
	if (!Hero || FMath::Abs(GetForwardSpeed()) > ULBACarSettings::Get().MaxLeaveSpeed || GetWorld()->GetTimeSeconds() - BoardTime < 0.5)
	{
		return false;
	}
	FVector Spot;
	if (!FindExitSpot(Spot))
	{
		UE_LOG(LogLBA2, Display, TEXT("%s: no room to get out"), *GetName());
		return false;
	}
	Hero->StopRiding(Spot, GetActorRotation());
	Hero->SetActorHiddenInGame(false);
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->RemoveMappingContext(InputContext);
		}
		PC->Possess(Hero);
		PC->SetControlRotation(FRotator(-15.f, GetActorRotation().Yaw, 0.f));
	}
	Driver = nullptr;
	DriveInput = FVector2D::ZeroVector;
	TriggerInput = 0.f;
	bHandbrakeInput = true;
	ApplyBody(false);
	return true;
}

bool ALBACar::FindExitSpot(FVector& OutLocation) const
{
	UWorld* World = GetWorld();
	const FLBACarGeometry& Geometry = GetGeometry();
	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
	const FVector Right(-Forward.Y, Forward.X, 0.0);
	const FVector Center = GetActorLocation();
	const ALBACharacter* Hero = Driver.Get();
	const float Radius = Hero ? Hero->GetCapsuleComponent()->GetScaledCapsuleRadius() : 35.f;
	const float HalfHeight = Hero ? Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 80.f;
	const double HalfWidth = FMath::Max(Geometry.FrontWheel.Y + Geometry.WheelWidth, Geometry.ChassisExtent.Y);
	const double HalfLength = FMath::Max(Geometry.ChassisExtent.X + FMath::Abs(Geometry.ChassisCenter.X), Geometry.FrontWheel.X + Geometry.FrontRadius);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBACarExit), false, this);
	Params.AddIgnoredActor(Hero);
	struct FSide { FVector Direction; double Edge; };
	// The driver's side (left) first, then the right, behind, ahead.
	const FSide Sides[] = { { -Right, HalfWidth }, { Right, HalfWidth }, { -Forward, HalfLength }, { Forward, HalfLength } };
	const FCollisionShape Capsule = FCollisionShape::MakeCapsule(Radius, HalfHeight);
	for (double Gap = 15.0; Gap <= 200.0; Gap += 40.0)
	{
		for (const FSide& Side : Sides)
		{
			const FVector Point = Center + Side.Direction * (Side.Edge + Radius + Gap);
			FHitResult Ground;
			if (!World->LineTraceSingleByChannel(Ground, Point + FVector(0.0, 0.0, 150.0), Point - FVector(0.0, 0.0, 250.0), ECC_Visibility, Params)
				|| Ground.ImpactNormal.Z < GroundNormalZ || Cast<AWaterBody>(Ground.GetActor()))
			{
				continue;
			}
			const FVector Spot = Ground.ImpactPoint + FVector(0.0, 0.0, HalfHeight + 5.0);
			FHitResult Blocked;
			if (World->OverlapBlockingTestByChannel(Spot, FQuat::Identity, ECC_Pawn, Capsule, Params)
				|| World->LineTraceSingleByChannel(Blocked, Center + FVector(0.0, 0.0, 60.0), Spot, ECC_Visibility, Params))
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

void ALBACar::CreateInput()
{
	InputContext = NewObject<UInputMappingContext>(this, TEXT("IMC_LBA_Car"));

	DriveAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Car_Drive"));
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
	{
		// The stick steers only: the triggers drive (its vertical would fight them).
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(DriveAction, EKeys::Gamepad_LeftX);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->LowerThreshold = 0.15f;
		Mapping.Modifiers.Add(DeadZone);
	}

	TriggerAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Car_Triggers"));
	TriggerAction->ValueType = EInputActionValueType::Axis1D;
	InputContext->MapKey(TriggerAction, EKeys::Gamepad_RightTriggerAxis);
	InputContext->MapKey(TriggerAction, EKeys::Gamepad_LeftTriggerAxis).Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));

	HandbrakeAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Car_Handbrake"));
	HandbrakeAction->ValueType = EInputActionValueType::Boolean;
	for (const FKey& Key : LBAControls::Jump())
	{
		InputContext->MapKey(HandbrakeAction, Key);
	}

	LookAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Car_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(LookAction, EKeys::Mouse2D);
		UInputModifierScalar* Scale = NewObject<UInputModifierScalar>(InputContext);
		Scale->Scalar = FVector(0.5, 0.5, 1.0);
		Mapping.Modifiers.Add(Scale);
	}
	{
		// The stick turns the camera at a rate (degrees/s), the mouse by its movement; vertical inverted on the stick.
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(LookAction, EKeys::Gamepad_Right2D);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->LowerThreshold = 0.2f;
		UInputModifierScalar* Rate = NewObject<UInputModifierScalar>(InputContext);
		Rate->Scalar = FVector(150.0, -75.0, 1.0);
		Mapping.Modifiers.Add(DeadZone);
		Mapping.Modifiers.Add(NewObject<UInputModifierScaleByDeltaTime>(InputContext));
		Mapping.Modifiers.Add(Rate);
	}

	LeaveAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Car_Leave"));
	LeaveAction->ValueType = EInputActionValueType::Boolean;
	for (const FKey& Key : LBAControls::Use())
	{
		InputContext->MapKey(LeaveAction, Key);
	}
}

void ALBACar::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!InputContext)
	{
		CreateInput();
	}
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogLBA2, Warning, TEXT("ALBACar needs Enhanced Input (Project Settings > Input > Default Input Component Class)."));
		return;
	}
	Input->BindAction(DriveAction, ETriggerEvent::Triggered, this, &ALBACar::OnDrive);
	Input->BindAction(DriveAction, ETriggerEvent::Completed, this, &ALBACar::OnDriveReleased);
	Input->BindAction(TriggerAction, ETriggerEvent::Triggered, this, &ALBACar::OnTrigger);
	Input->BindAction(TriggerAction, ETriggerEvent::Completed, this, &ALBACar::OnTriggerReleased);
	Input->BindAction(HandbrakeAction, ETriggerEvent::Started, this, &ALBACar::OnHandbrake);
	Input->BindAction(HandbrakeAction, ETriggerEvent::Completed, this, &ALBACar::OnHandbrakeReleased);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ALBACar::OnLook);
	Input->BindAction(LeaveAction, ETriggerEvent::Started, this, &ALBACar::OnLeave);
}

void ALBACar::PawnClientRestart()
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

void ALBACar::OnDrive(const FInputActionValue& Value)
{
	DriveInput = Value.Get<FVector2D>();
}

void ALBACar::OnTrigger(const FInputActionValue& Value)
{
	TriggerInput = Value.Get<float>();
}

void ALBACar::OnLook(const FInputActionValue& Value)
{
	const FVector2D Look = Value.Get<FVector2D>();
	LookOffset.X = FRotator::NormalizeAxis(LookOffset.X + Look.X);
	LookOffset.Y = FMath::Clamp(LookOffset.Y + Look.Y, -60.f, 50.f);
	LastLookTime = GetWorld()->GetTimeSeconds();
}

// Scooter -------------------------------------------------------------------------------------------------------

ALBAScooter::ALBAScooter()
{
	// With Twinsen behind the Rabibunny (entity 100's body 106; 107 is the wizard's tunic).
	DrivenBodyType = 106;
	Chassis->InitBoxExtent(FLBACarGeometry::Scooter.ChassisExtent);
}

TSubclassOf<UChaosVehicleWheel> ALBAScooter::GetFrontWheelClass() const
{
	return ULBAScooterFrontWheel::StaticClass();
}

TSubclassOf<UChaosVehicleWheel> ALBAScooter::GetRearWheelClass() const
{
	return ULBAScooterRearWheel::StaticClass();
}

ULBAEntityAsset* ALBAScooter::GetDefaultEntity(bool bDriven) const
{
	return GetDefault<ULBA2Settings>()->Scooter.LoadSynchronous();
}

void ALBAScooter::UpdateBodyPose(float DeltaSeconds)
{
	// No wheels to show: its idle animation plays on.
}
