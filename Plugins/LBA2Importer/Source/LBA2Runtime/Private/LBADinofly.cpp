#include "LBADinofly.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/CollisionProfile.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
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
#include "LBAEntityAsset.h"
#include "LBAFlightMovementComponent.h"
#include "LBATypes.h"

namespace
{
	TAutoConsoleVariable<bool> CVarInvertPitch(
		TEXT("lba.Dinofly.InvertPitch"), false,
		TEXT("Dinofly pitch controls: false = forward dives (flight stick), true = forward climbs."));

	constexpr float CollisionRadius = 70.f;

	/** Dinofly animation slots (entity 109). The flight ones were identified from their root motion. */
	namespace DinoflyAnim
	{
		constexpr int32 Idle = 0;
		constexpr int32 TurnLeft = 3;
		constexpr int32 TurnRight = 4;
		/** Wings spread, gliding down. */
		constexpr int32 Glide = 30;
		/** Flapping in place. */
		constexpr int32 Hover = 152;
		/** Flapping forward. */
		constexpr int32 Flap = 218;
	}
}

ALBADinofly::ALBADinofly()
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessAI = EAutoPossessAI::Disabled;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(CollisionRadius);
	Collision->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	RootComponent = Collision;

	// The mesh's feet rest at the bottom of the sphere; LBA meshes face +Y.
	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetRelativeLocationAndRotation(FVector(0.f, 0.f, -CollisionRadius), FRotator(0.f, -90.f, 0.f));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Mesh->SetAnimInstanceClass(ULBAAnimInstance::StaticClass());
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->SetRenderCustomDepth(true);
	Mesh->SetCustomDepthStencilValue(LBA::MovingObjectStencil);

	Flight = CreateDefaultSubobject<ULBAFlightMovementComponent>(TEXT("Flight"));
	Flight->SetUpdatedComponent(Collision);

	// The camera follows the heading, never the bank.
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(Collision);
	CameraBoom->SetUsingAbsoluteRotation(true);
	CameraBoom->TargetArmLength = 900.f;
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 120.f);
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 8.f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
	FollowCamera->SetFieldOfView(75.f);
}

UPawnMovementComponent* ALBADinofly::GetMovementComponent() const
{
	return Flight;
}

void ALBADinofly::BeginPlay()
{
	Super::BeginPlay();
	ApplyEntity();
	CameraBase = FRotator(-15.f, GetActorRotation().Yaw, 0.f);
}

void ALBADinofly::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	ApplyEntity();
}

void ALBADinofly::ApplyEntity()
{
	if (!Entity)
	{
		Entity = GetDefault<ULBA2Settings>()->Dinofly.LoadSynchronous();
		if (!Entity)
		{
			UE_LOG(LogLBA2, Warning, TEXT("%s: no Dinofly entity. Import the Characters layer (Tools > LBA2 Importer)."), *GetName());
			return;
		}
	}
	const bool bRidden = Rider.IsValid() || IsPlayerControlled();
	USkeletalMesh* BodyMesh = Entity->GetBodyMesh(bRidden ? BodyType : RiderlessBodyType);
	if (!BodyMesh)
	{
		BodyMesh = Entity->GetDefaultBodyMesh();
	}
	if (BodyMesh && Mesh->GetSkeletalMeshAsset() != BodyMesh)
	{
		Mesh->SetSkeletalMesh(BodyMesh);
	}
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		// The flight model moves the Dinofly; animations play in place.
		Anim->SetApplyRootMotion(false);
	}
}

ULBAAnimInstance* ALBADinofly::GetLBAAnimInstance() const
{
	return Cast<ULBAAnimInstance>(Mesh->GetAnimInstance());
}

// Riding ----------------------------------------------------------------------------------------------------

ALBADinofly* ALBADinofly::Summon(ALBACharacter* Hero, TSubclassOf<ALBADinofly> DinoflyClass)
{
	if (!Hero || !Hero->GetController())
	{
		return nullptr;
	}
	UWorld* World = Hero->GetWorld();
	UClass* Class = DinoflyClass ? DinoflyClass.Get() : ALBADinofly::StaticClass();

	// Reuse a waiting Dinofly: mount it when close, call it over otherwise.
	ALBADinofly* Dinofly = nullptr;
	double BestDistance = TNumericLimits<double>::Max();
	for (TActorIterator<ALBADinofly> It(World); It; ++It)
	{
		if (!It->Rider.IsValid() && !It->IsPlayerControlled() && It->IsA(Class))
		{
			const double Distance = FVector::Dist(It->GetActorLocation(), Hero->GetActorLocation());
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Dinofly = *It;
			}
		}
	}
	if (Dinofly && BestDistance < Dinofly->MountDistance)
	{
		Dinofly->Mount(Hero);
		return Dinofly;
	}

	const ALBADinofly* Defaults = Class->GetDefaultObject<ALBADinofly>();
	const FVector Location = Hero->GetActorLocation() + FVector(0.f, 0.f, Defaults->SummonHeight);
	const FRotator Rotation(0.f, Hero->GetActorRotation().Yaw, 0.f);
	if (Dinofly)
	{
		Dinofly->TeleportTo(Location, Rotation);
	}
	else
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		Dinofly = World->SpawnActor<ALBADinofly>(Class, Location, Rotation, Params);
	}
	if (Dinofly)
	{
		Dinofly->Mount(Hero);
		Dinofly->Flight->Launch(Dinofly->Flight->GetTrimSpeed());
	}
	return Dinofly;
}

void ALBADinofly::Mount(ALBACharacter* Hero)
{
	AController* HeroController = Hero->GetController();
	Rider = Hero;
	// The ridden body includes Twinsen: park the hero.
	Hero->SetActorHiddenInGame(true);
	Hero->SetActorEnableCollision(false);
	Hero->SetActorTickEnabled(false);
	Hero->GetCharacterMovement()->DisableMovement();
	if (HeroController)
	{
		HeroController->Possess(this);
	}
	ApplyEntity();
	CameraBase = FRotator(-15.f, GetActorRotation().Yaw, 0.f);
	LookOffset = FVector2D::ZeroVector;
}

bool ALBADinofly::Dismount()
{
	// On the ground only: floating on the sea would drop the rider into deep water.
	if (!Flight->IsLanded() || Flight->IsFloating())
	{
		return false;
	}
	UWorld* World = GetWorld();
	ALBACharacter* Hero = Rider.Get();
	if (!Hero)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Hero = World->SpawnActor<ALBACharacter>(ALBACharacter::StaticClass(), GetActorLocation(), GetActorRotation(), Params);
		if (!Hero)
		{
			return false;
		}
	}

	// Step down beside the Dinofly: right side, else left, else behind.
	const float HalfHeight = Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FRotator Facing(0.f, GetActorRotation().Yaw, 0.f);
	const FVector Sides[] = { GetActorRightVector(), -GetActorRightVector(), -GetActorForwardVector() };
	bool bPlaced = false;
	for (const FVector& Side : Sides)
	{
		FVector Spot = GetActorLocation() + Side.GetSafeNormal2D() * 220.f;
		FHitResult Ground;
		if (World->LineTraceSingleByChannel(Ground, Spot + FVector(0.f, 0.f, 200.f), Spot - FVector(0.f, 0.f, 600.f), ECC_Visibility, FCollisionQueryParams(NAME_None, false, this)))
		{
			Spot.Z = Ground.ImpactPoint.Z + HalfHeight + 5.f;
		}
		Hero->SetActorEnableCollision(true);
		if (Hero->TeleportTo(Spot, Facing))
		{
			bPlaced = true;
			break;
		}
	}
	if (!bPlaced)
	{
		Hero->TeleportTo(GetActorLocation() + FVector(0.f, 0.f, HalfHeight + CollisionRadius), Facing, false, true);
	}
	Hero->SetActorHiddenInGame(false);
	Hero->SetActorEnableCollision(true);
	Hero->SetActorTickEnabled(true);
	Hero->GetCharacterMovement()->SetMovementMode(MOVE_Falling);

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->RemoveMappingContext(InputContext);
		}
		PC->Possess(Hero);
	}
	Rider = nullptr;
	SteerInput = FVector2D::ZeroVector;
	bFlapHeld = bBrakeHeld = false;
	Flight->SetFlightInput(0.f, 0.f, 0.f, 0.f);
	ApplyEntity();
	return true;
}

void ALBADinofly::SetFlightInput(float Pitch, float Roll, bool bFlap, bool bBrake)
{
	// Same convention as the stick: Y forward, so forward pitch input is "nose down" unless inverted.
	SteerInput = FVector2D(Roll, CVarInvertPitch.GetValueOnGameThread() ? Pitch : -Pitch);
	bFlapHeld = bFlap;
	bBrakeHeld = bBrake;
}

// Tick --------------------------------------------------------------------------------------------------------

void ALBADinofly::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float PitchInput = CVarInvertPitch.GetValueOnGameThread() ? SteerInput.Y : -SteerInput.Y;
	Flight->SetFlightInput(PitchInput, SteerInput.X, bFlapHeld ? 1.f : 0.f, bBrakeHeld ? 1.f : 0.f);
	UpdateAnimation();
	UpdateCamera(DeltaSeconds);
}

void ALBADinofly::PlaySlot(int32 AnimType, float PlayRate, float BlendTime)
{
	const FLBAEntityAnim* Anim = Entity ? Entity->Anims.Find(AnimType) : nullptr;
	ULBAAnimInstance* Instance = GetLBAAnimInstance();
	if (Anim && Anim->Sequence && Instance)
	{
		Instance->PlayLBAAnimation(Anim->Sequence, true, Anim->LoopStartTime, BlendTime, PlayRate);
	}
}

void ALBADinofly::UpdateAnimation()
{
	if (Flight->IsLanded())
	{
		PlaySlot(DinoflyAnim::Idle, 1.f, 0.3f);
		return;
	}
	const float Speed = Flight->Velocity.Size();
	const bool bFlapping = Flight->GetFlapInput() > 0.f;
	const float Bank = Flight->GetBank();
	if (Speed < 350.f || (bFlapping && Speed < 500.f))
	{
		PlaySlot(DinoflyAnim::Hover, bFlapping ? 1.2f : 0.9f);
	}
	else if (bFlapping)
	{
		PlaySlot(DinoflyAnim::Flap, 1.3f);
	}
	else if (FMath::Abs(Bank) > 25.f)
	{
		PlaySlot(Bank > 0.f ? DinoflyAnim::TurnRight : DinoflyAnim::TurnLeft);
	}
	else if (Speed < Flight->GetTrimSpeed() * 0.75f)
	{
		// Too slow to glide: the Dinofly flaps on its own.
		PlaySlot(DinoflyAnim::Flap, 0.9f);
	}
	else
	{
		PlaySlot(DinoflyAnim::Glide);
	}
}

void ALBADinofly::UpdateCamera(float DeltaSeconds)
{
	const float Speed = Flight->Velocity.Size();
	const float Yaw = Flight->IsLanded() ? GetActorRotation().Yaw : Flight->GetHeading().Rotation().Yaw;
	const float PathPitch = Flight->IsLanded() ? 0.f : Flight->GetFlightPathAngle();
	const FRotator Target(FMath::Clamp(-15.f + 0.4f * PathPitch, -50.f, 20.f), Yaw, 0.f);
	CameraBase = FMath::RInterpTo(CameraBase, Target, DeltaSeconds, 2.5f);

	// The look offset eases back behind the Dinofly when the player stops looking around.
	if (GetWorld()->GetTimeSeconds() - LastLookTime > 1.5f)
	{
		LookOffset = FMath::Vector2DInterpTo(LookOffset, FVector2D::ZeroVector, DeltaSeconds, 1.5f);
	}
	CameraBoom->SetWorldRotation(FRotator(FMath::Clamp(CameraBase.Pitch + LookOffset.Y, -80.f, 60.f), CameraBase.Yaw + LookOffset.X, 0.f));

	// Speed feel: pull back and widen with speed.
	const float SpeedAlpha = FMath::Clamp((Speed - Flight->GetTrimSpeed()) / 1500.f, 0.f, 1.f);
	CameraBoom->TargetArmLength = FMath::FInterpTo(CameraBoom->TargetArmLength, 850.f + 350.f * SpeedAlpha, DeltaSeconds, 2.f);
	FollowCamera->SetFieldOfView(FMath::FInterpTo(FollowCamera->FieldOfView, 75.f + 20.f * SpeedAlpha, DeltaSeconds, 2.f));
}

// Input -------------------------------------------------------------------------------------------------------

void ALBADinofly::CreateInput()
{
	InputContext = NewObject<UInputMappingContext>(this, TEXT("IMC_LBA_Dinofly"));

	SteerAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Dinofly_Steer"));
	SteerAction->ValueType = EInputActionValueType::Axis2D;
	auto MapSteer = [this](const FKey& Key, bool bSwizzle, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(SteerAction, Key);
		if (bSwizzle)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(InputContext));
		}
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
		}
	};
	MapSteer(EKeys::Up, true, false);
	MapSteer(EKeys::W, true, false);
	MapSteer(EKeys::Down, true, true);
	MapSteer(EKeys::S, true, true);
	MapSteer(EKeys::Right, false, false);
	MapSteer(EKeys::D, false, false);
	MapSteer(EKeys::Left, false, true);
	MapSteer(EKeys::A, false, true);
	auto MapStick = [this](UInputAction* Action, const FKey& Key)
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(Action, Key);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->LowerThreshold = 0.2f;
		Mapping.Modifiers.Add(DeadZone);
	};
	MapStick(SteerAction, EKeys::Gamepad_Left2D);

	LookAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Dinofly_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	InputContext->MapKey(LookAction, EKeys::Mouse2D);
	MapStick(LookAction, EKeys::Gamepad_Right2D);

	FlapAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Dinofly_Flap"));
	FlapAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(FlapAction, EKeys::SpaceBar);
	InputContext->MapKey(FlapAction, EKeys::Gamepad_FaceButton_Bottom);
	InputContext->MapKey(FlapAction, EKeys::Gamepad_RightTrigger);

	BrakeAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Dinofly_Brake"));
	BrakeAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(BrakeAction, EKeys::LeftShift);
	InputContext->MapKey(BrakeAction, EKeys::Gamepad_LeftTrigger);

	DismountAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Dinofly_Dismount"));
	DismountAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(DismountAction, EKeys::F);
	InputContext->MapKey(DismountAction, EKeys::Gamepad_FaceButton_Top);
}

void ALBADinofly::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!InputContext)
	{
		CreateInput();
	}
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogLBA2, Warning, TEXT("ALBADinofly needs Enhanced Input (Project Settings > Input > Default Input Component Class)."));
		return;
	}
	Input->BindAction(SteerAction, ETriggerEvent::Triggered, this, &ALBADinofly::OnSteer);
	Input->BindAction(SteerAction, ETriggerEvent::Completed, this, &ALBADinofly::OnSteerReleased);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ALBADinofly::OnLook);
	Input->BindAction(FlapAction, ETriggerEvent::Started, this, &ALBADinofly::OnFlap);
	Input->BindAction(FlapAction, ETriggerEvent::Completed, this, &ALBADinofly::OnFlapReleased);
	Input->BindAction(BrakeAction, ETriggerEvent::Started, this, &ALBADinofly::OnBrake);
	Input->BindAction(BrakeAction, ETriggerEvent::Completed, this, &ALBADinofly::OnBrakeReleased);
	Input->BindAction(DismountAction, ETriggerEvent::Started, this, &ALBADinofly::OnDismount);
}

void ALBADinofly::PawnClientRestart()
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

void ALBADinofly::OnSteer(const FInputActionValue& Value)
{
	SteerInput = Value.Get<FVector2D>();
}

void ALBADinofly::OnSteerReleased(const FInputActionValue& Value)
{
	SteerInput = FVector2D::ZeroVector;
}

void ALBADinofly::OnLook(const FInputActionValue& Value)
{
	const FVector2D Look = Value.Get<FVector2D>();
	LookOffset.X = FMath::Clamp(LookOffset.X + Look.X, -170.f, 170.f);
	LookOffset.Y = FMath::Clamp(LookOffset.Y + Look.Y, -60.f, 60.f);
	LastLookTime = GetWorld()->GetTimeSeconds();
}
