#include "LBACharacter.h"

#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAAnimInstance.h"
#include "LBADinofly.h"
#include "LBATypes.h"

namespace
{
	constexpr float AxisThreshold = 0.3f;

	/** Natural speed (cm/s) of an animation's root motion. */
	float RootMotionSpeed(const UAnimSequence* Sequence)
	{
		if (!Sequence || Sequence->GetPlayLength() <= 0.f)
		{
			return 0.f;
		}
		const FTransform Motion = Sequence->ExtractRootMotionFromRange(0.0, Sequence->GetPlayLength(), FAnimExtractContext(0.0, true));
		return Motion.GetTranslation().Size2D() / Sequence->GetPlayLength();
	}
}

ALBACharacter::ALBACharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	// Twinsen is about 155 cm tall at the default world scale.
	GetCapsuleComponent()->InitCapsuleSize(30.f, 78.f);

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = false;
	Movement->RotationRate = FRotator(0.f, 540.f, 0.f);
	Movement->JumpZVelocity = 520.f;
	Movement->AirControl = 0.2f;
	Movement->MaxWalkSpeed = 300.f;
	Movement->BrakingDecelerationWalking = 2000.f;
	Movement->MaxStepHeight = 33.f;
	// LBA terrain is steeper than Unreal's default walkable slope.
	Movement->SetWalkableFloorAngle(55.f);
	Movement->bAllowPhysicsRotationDuringAnimRootMotion = true;

	// LBA meshes face +Y, like Unreal mannequins.
	GetMesh()->SetRelativeLocationAndRotation(FVector(0.f, 0.f, -78.f), FRotator(0.f, -90.f, 0.f));
	GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	GetMesh()->SetAnimInstanceClass(ULBAAnimInstance::StaticClass());
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	// Moving: the palette shading post-process shades it smoothly rather than with world-anchored dither dots.
	GetMesh()->SetRenderCustomDepth(true);
	GetMesh()->SetCustomDepthStencilValue(LBA::MovingObjectStencil);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 650.f;
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 60.f);
	CameraBoom->SetRelativeRotation(FRotator(-28.f, 0.f, 0.f));
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritPitch = false;
	CameraBoom->bInheritRoll = false;
	CameraBoom->bEnableCameraRotationLag = true;
	CameraBoom->CameraRotationLagSpeed = 3.f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;
}

void ALBACharacter::BeginPlay()
{
	Super::BeginPlay();
	LoadDefaultBehaviours();
	if (BodyType < 0)
	{
		BodyType = GetDefault<ULBA2Settings>()->HeroBodyType;
	}
	ApplyEntity();
	SetControlMode(ControlMode);
}

void ALBACharacter::LoadDefaultBehaviours()
{
	const ULBA2Settings* Settings = GetDefault<ULBA2Settings>();
	const TPair<ELBABehaviour, const TSoftObjectPtr<ULBAEntityAsset>*> Defaults[] =
	{
		{ ELBABehaviour::Normal, &Settings->HeroNormal },
		{ ELBABehaviour::Athletic, &Settings->HeroAthletic },
		{ ELBABehaviour::Aggressive, &Settings->HeroAggressive },
		{ ELBABehaviour::Discreet, &Settings->HeroDiscreet },
	};
	for (const auto& Default : Defaults)
	{
		if (!Behaviours.FindRef(Default.Key))
		{
			if (ULBAEntityAsset* Entity = Default.Value->LoadSynchronous())
			{
				Behaviours.Add(Default.Key, Entity);
			}
		}
	}
	if (Behaviours.Num() == 0)
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: no LBA entity found. Import the Characters layer (Tools > LBA2 Importer)."), *GetName());
	}
}

ULBAEntityAsset* ALBACharacter::GetEntity() const
{
	if (ULBAEntityAsset* Entity = Behaviours.FindRef(Behaviour))
	{
		return Entity;
	}
	return Behaviours.FindRef(ELBABehaviour::Normal);
}

ULBAAnimInstance* ALBACharacter::GetLBAAnimInstance() const
{
	return Cast<ULBAAnimInstance>(GetMesh()->GetAnimInstance());
}

void ALBACharacter::ApplyEntity()
{
	ULBAEntityAsset* Entity = GetEntity();
	if (!Entity)
	{
		return;
	}
	USkeletalMesh* BodyMesh = Entity->GetBodyMesh(BodyType);
	if (!BodyMesh)
	{
		BodyMesh = Entity->GetDefaultBodyMesh();
	}
	if (BodyMesh && GetMesh()->GetSkeletalMeshAsset() != BodyMesh)
	{
		GetMesh()->SetSkeletalMesh(BodyMesh);
	}

	// Size the capsule from the authored collision box when there is one.
	if (const FLBAEntityBody* Body = Entity->Bodies.Find(BodyType))
	{
		if (Body->CollisionBox.IsValid)
		{
			const FVector Size = Body->CollisionBox.GetSize();
			const float HalfHeight = FMath::Max(Size.Z * 0.5f, 40.f);
			const float Radius = FMath::Clamp(FMath::Min(Size.X, Size.Y) * 0.5f, 15.f, HalfHeight);
			GetCapsuleComponent()->SetCapsuleSize(Radius, HalfHeight);
			GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -HalfHeight));
		}
	}
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		Anim->SetApplyRootMotion(ControlMode == ELBAControlMode::Authentic);
	}
}

void ALBACharacter::SetBehaviour(ELBABehaviour NewBehaviour)
{
	if (NewBehaviour == Behaviour || !Behaviours.FindRef(NewBehaviour))
	{
		return;
	}
	Behaviour = NewBehaviour;
	ActionSlot = INDEX_NONE;
	ApplyEntity();
}

void ALBACharacter::SetBodyType(int32 NewBodyType)
{
	BodyType = NewBodyType;
	ApplyEntity();
}

void ALBACharacter::SetControlMode(ELBAControlMode NewMode)
{
	ControlMode = NewMode;
	const bool bAuthentic = ControlMode == ELBAControlMode::Authentic;
	GetCharacterMovement()->bOrientRotationToMovement = !bAuthentic;
	CameraBoom->bUsePawnControlRotation = !bAuthentic;
	CameraBoom->bInheritYaw = true;
	if (!bAuthentic)
	{
		if (AController* PC = GetController())
		{
			PC->SetControlRotation(FRotator(-20.f, GetActorRotation().Yaw, 0.f));
		}
	}
	else
	{
		CameraBoom->SetRelativeRotation(FRotator(-28.f, 0.f, 0.f));
	}
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		Anim->SetApplyRootMotion(bAuthentic);
	}
}

UAnimSequence* ALBACharacter::FindAnim(int32 AnimType, float* OutLoopStart) const
{
	const ULBAEntityAsset* Entity = GetEntity();
	const FLBAEntityAnim* Anim = Entity ? Entity->Anims.Find(AnimType) : nullptr;
	if (!Anim || !Anim->Sequence)
	{
		// Fall back to the normal behaviour's version of the slot.
		const ULBAEntityAsset* Normal = Behaviours.FindRef(ELBABehaviour::Normal);
		Anim = Normal ? Normal->Anims.Find(AnimType) : nullptr;
	}
	if (OutLoopStart)
	{
		*OutLoopStart = Anim ? Anim->LoopStartTime : 0.f;
	}
	return Anim ? Anim->Sequence.Get() : nullptr;
}

void ALBACharacter::PlaySlot(int32 AnimType, bool bLoop, float BlendTime, float PlayRate)
{
	float LoopStart = 0.f;
	if (UAnimSequence* Sequence = FindAnim(AnimType, &LoopStart))
	{
		if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
		{
			Anim->PlayLBAAnimation(Sequence, bLoop, LoopStart, BlendTime, PlayRate);
		}
	}
}

bool ALBACharacter::PlayAction(int32 AnimType)
{
	UAnimSequence* Sequence = FindAnim(AnimType);
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	if (!Sequence || !Anim)
	{
		return false;
	}
	ActionSlot = AnimType;
	Anim->PlayLBAAnimation(Sequence, false, 0.f, 0.12f, 1.f, true);
	return true;
}

void ALBACharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!GetEntity())
	{
		return;
	}
	if (ControlMode == ELBAControlMode::Authentic)
	{
		UpdateAuthentic(DeltaSeconds);
	}
	else
	{
		UpdateModern(DeltaSeconds);
	}
}

void ALBACharacter::UpdateAuthentic(float DeltaSeconds)
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	if (!Anim)
	{
		return;
	}

	// Mouse look orbits the camera a little; walking recenters it behind Twinsen like the original.
	if (FMath::Abs(MoveInput.Y) > AxisThreshold)
	{
		CameraYawOffset = FMath::FInterpTo(CameraYawOffset, 0.f, DeltaSeconds, 1.5f);
	}
	CameraBoom->SetRelativeRotation(FRotator(-28.f, CameraYawOffset, 0.f));

	if (ActionSlot != INDEX_NONE)
	{
		if (!Anim->IsCurrentAnimationFinished())
		{
			return;
		}
		ActionSlot = INDEX_NONE;
	}

	const bool bFalling = GetCharacterMovement()->IsFalling();
	if (bFalling)
	{
		FallTime += DeltaSeconds;
		if (FallTime > 0.35f)
		{
			PlaySlot(int32(ELBAAnimType::Falling), true);
		}
		return;
	}
	FallTime = 0.f;

	const float Forward = MoveInput.Y;
	const float Turn = MoveInput.X;
	if (Forward > AxisThreshold || Forward < -AxisThreshold)
	{
		PlaySlot(int32(Forward > 0.f ? ELBAAnimType::Forward : ELBAAnimType::Backward), true);
		if (FMath::Abs(Turn) > AxisThreshold)
		{
			AddActorWorldRotation(FRotator(0.f, FMath::Sign(Turn) * SteerRate * DeltaSeconds, 0.f));
		}
	}
	else if (Turn > AxisThreshold)
	{
		PlaySlot(int32(ELBAAnimType::TurnRight), true);
	}
	else if (Turn < -AxisThreshold)
	{
		PlaySlot(int32(ELBAAnimType::TurnLeft), true);
	}
	else
	{
		PlaySlot(int32(ELBAAnimType::Idle), true, 0.2f);
	}
}

void ALBACharacter::UpdateModern(float DeltaSeconds)
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	if (!Anim)
	{
		return;
	}
	const UAnimSequence* ForwardAnim = FindAnim(int32(ELBAAnimType::Forward));
	const float NaturalSpeed = FMath::Max(RootMotionSpeed(ForwardAnim), 50.f);
	GetCharacterMovement()->MaxWalkSpeed = NaturalSpeed;

	if (ActionSlot != INDEX_NONE)
	{
		if (!Anim->IsCurrentAnimationFinished())
		{
			return;
		}
		ActionSlot = INDEX_NONE;
	}
	if (GetCharacterMovement()->IsFalling())
	{
		FallTime += DeltaSeconds;
		if (FallTime > 0.35f)
		{
			PlaySlot(int32(ELBAAnimType::Falling), true);
		}
		return;
	}
	FallTime = 0.f;

	const float Speed = GetVelocity().Size2D();
	if (Speed > 20.f)
	{
		PlaySlot(int32(ELBAAnimType::Forward), true, 0.2f, Speed / NaturalSpeed);
	}
	else
	{
		PlaySlot(int32(ELBAAnimType::Idle), true, 0.25f);
	}
}

void ALBACharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	const bool bEndsJump = ActionSlot == int32(ELBAAnimType::Jump) || ActionSlot == int32(ELBAAnimType::RunningJump);
	if (bEndsJump)
	{
		ActionSlot = INDEX_NONE;
	}
	else if (FallTime > 0.8f)
	{
		PlayAction(int32(ELBAAnimType::FallLandingStumble));
	}
	FallTime = 0.f;
}

// Input ---------------------------------------------------------------------------------------------------

void ALBACharacter::CreateInput()
{
	InputContext = NewObject<UInputMappingContext>(this, TEXT("IMC_LBA"));

	MoveAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Move"));
	MoveAction->ValueType = EInputActionValueType::Axis2D;
	auto MapMove = [this](const FKey& Key, bool bSwizzle, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(MoveAction, Key);
		if (bSwizzle)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(InputContext));
		}
		if (bNegate)
		{
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(InputContext));
		}
	};
	MapMove(EKeys::Up, true, false);
	MapMove(EKeys::W, true, false);
	MapMove(EKeys::Down, true, true);
	MapMove(EKeys::S, true, true);
	MapMove(EKeys::Right, false, false);
	MapMove(EKeys::D, false, false);
	MapMove(EKeys::Left, false, true);
	MapMove(EKeys::A, false, true);
	auto MapStick = [this](UInputAction* Action, const FKey& Key)
	{
		// Dead zone: resting sticks (or flight sticks/throttles) must not move Twinsen.
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(Action, Key);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->LowerThreshold = 0.25f;
		Mapping.Modifiers.Add(DeadZone);
	};
	MapStick(MoveAction, EKeys::Gamepad_Left2D);

	LookAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	InputContext->MapKey(LookAction, EKeys::Mouse2D);
	MapStick(LookAction, EKeys::Gamepad_Right2D);

	ActionAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Action"));
	ActionAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(ActionAction, EKeys::SpaceBar);
	InputContext->MapKey(ActionAction, EKeys::Gamepad_FaceButton_Bottom);

	ToggleModeAction = NewObject<UInputAction>(this, TEXT("IA_LBA_ToggleMode"));
	ToggleModeAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(ToggleModeAction, EKeys::Tab);

	SummonDinoflyAction = NewObject<UInputAction>(this, TEXT("IA_LBA_SummonDinofly"));
	SummonDinoflyAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(SummonDinoflyAction, EKeys::F);
	InputContext->MapKey(SummonDinoflyAction, EKeys::Gamepad_FaceButton_Top);

	// F5-F8 like the original game, 1-4 as an alternative.
	const FKey BehaviourKeys[4][2] = { { EKeys::F5, EKeys::One }, { EKeys::F6, EKeys::Two }, { EKeys::F7, EKeys::Three }, { EKeys::F8, EKeys::Four } };
	for (int32 Index = 0; Index < 4; ++Index)
	{
		UInputAction* Action = NewObject<UInputAction>(this, *FString::Printf(TEXT("IA_LBA_Behaviour%d"), Index));
		Action->ValueType = EInputActionValueType::Boolean;
		InputContext->MapKey(Action, BehaviourKeys[Index][0]);
		InputContext->MapKey(Action, BehaviourKeys[Index][1]);
		BehaviourActions.Add(Action);
	}
}

void ALBACharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	if (!InputContext)
	{
		CreateInput();
	}
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogLBA2, Warning, TEXT("ALBACharacter needs Enhanced Input (Project Settings > Input > Default Input Component Class)."));
		return;
	}
	Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ALBACharacter::OnMove);
	Input->BindAction(MoveAction, ETriggerEvent::Completed, this, &ALBACharacter::OnMoveReleased);
	Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ALBACharacter::OnLook);
	Input->BindAction(ActionAction, ETriggerEvent::Started, this, &ALBACharacter::OnAction);
	Input->BindAction(ToggleModeAction, ETriggerEvent::Started, this, &ALBACharacter::OnToggleControlMode);
	Input->BindAction(SummonDinoflyAction, ETriggerEvent::Started, this, &ALBACharacter::OnSummonDinofly);
	for (int32 Index = 0; Index < BehaviourActions.Num(); ++Index)
	{
		Input->BindAction(BehaviourActions[Index], ETriggerEvent::Started, this, &ALBACharacter::OnBehaviour, ELBABehaviour(Index));
	}
}

void ALBACharacter::PawnClientRestart()
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
			Subsystem->AddMappingContext(InputContext, 1);
		}
	}
}

void ALBACharacter::OnMove(const FInputActionValue& Value)
{
	MoveInput = Value.Get<FVector2D>();
	if (ControlMode == ELBAControlMode::Modern && Controller)
	{
		const FRotator Yaw(0.f, Controller->GetControlRotation().Yaw, 0.f);
		AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::X), MoveInput.Y);
		AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), MoveInput.X);
	}
}

void ALBACharacter::OnMoveReleased(const FInputActionValue& Value)
{
	MoveInput = FVector2D::ZeroVector;
}

void ALBACharacter::OnLook(const FInputActionValue& Value)
{
	const FVector2D Look = Value.Get<FVector2D>();
	if (ControlMode == ELBAControlMode::Modern)
	{
		AddControllerYawInput(Look.X);
		AddControllerPitchInput(-Look.Y);
	}
	else
	{
		CameraYawOffset = FMath::Clamp(CameraYawOffset + Look.X, -170.f, 170.f);
	}
}

void ALBACharacter::OnAction(const FInputActionValue& Value)
{
	DoBehaviourAction();
}

void ALBACharacter::DoBehaviourAction()
{
	if (ActionSlot != INDEX_NONE || GetCharacterMovement()->IsFalling())
	{
		return;
	}
	switch (Behaviour)
	{
	case ELBABehaviour::Normal:
		PlayAction(int32(ELBAAnimType::Action));
		break;
	case ELBABehaviour::Athletic:
	{
		const bool bRunning = MoveInput.Y > AxisThreshold || GetVelocity().Size2D() > 150.f;
		if (PlayAction(int32(bRunning ? ELBAAnimType::RunningJump : ELBAAnimType::Jump)) || PlayAction(int32(ELBAAnimType::Jump)))
		{
			LaunchCharacter(FVector(0.f, 0.f, JumpSpeed), false, true);
		}
		break;
	}
	case ELBABehaviour::Aggressive:
	{
		// The original alternates punches and kicks.
		static const int32 Attacks[] = { 17, 18, 19 };
		for (int32 Try = 0; Try < 3; ++Try)
		{
			const int32 Slot = Attacks[(AttackCycle + Try) % 3];
			if (PlayAction(Slot))
			{
				AttackCycle = (AttackCycle + Try + 1) % 3;
				break;
			}
		}
		break;
	}
	case ELBABehaviour::Discreet:
		PlayAction(int32(ELBAAnimType::Crouch));
		break;
	}
}

void ALBACharacter::OnToggleControlMode(const FInputActionValue& Value)
{
	SetControlMode(ControlMode == ELBAControlMode::Authentic ? ELBAControlMode::Modern : ELBAControlMode::Authentic);
}

ALBADinofly* ALBACharacter::SummonDinofly()
{
	if (ActionSlot != INDEX_NONE || GetCharacterMovement()->IsFalling())
	{
		return nullptr;
	}
	MoveInput = FVector2D::ZeroVector;
	return ALBADinofly::Summon(this, DinoflyClass);
}

ALBAGameMode::ALBAGameMode()
{
	DefaultPawnClass = ALBACharacter::StaticClass();
}
