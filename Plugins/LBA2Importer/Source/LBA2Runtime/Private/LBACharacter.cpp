#include "LBACharacter.h"

#include "Animation/AnimSequence.h"
#include "AnimationRuntime.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/OverlapResult.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAAnimInstance.h"
#include "LBAAnimNotifies.h"
#include "LBADinofly.h"
#include "LBAHitPointsComponent.h"
#include "LBAInteractable.h"
#include "LBAPlayerController.h"
#include "LBASceneTravelSubsystem.h"
#include "LBATypes.h"

namespace
{
	constexpr float AxisThreshold = 0.3f;
	constexpr float AnimSampleStep = 1.f / 30.f;

	/** Punch and kicks of the aggressive behaviour, in the order the original alternates them. */
	constexpr int32 AttackSlots[] = { int32(ELBAAnimType::Punch), 18, 19 };

	bool IsAttackSlot(int32 Slot)
	{
		for (const int32 Attack : AttackSlots)
		{
			if (Slot == Attack)
			{
				return true;
			}
		}
		return false;
	}

	TAutoConsoleVariable<bool> CVarSmoothKeys(TEXT("lba.Modern.SmoothKeys"), true,
		TEXT("Modern mode: interpolate LBA keyframes with smooth curves (0 = the original's straight in-betweens)."));
	TAutoConsoleVariable<bool> CVarProcedural(TEXT("lba.Modern.Procedural"), true,
		TEXT("Modern mode: lean, foot placement and head turn over the animations."));

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

	/** Root motion speed (cm/s) of an animation around a time. */
	float RootMotionSpeedAt(const UAnimSequence* Sequence, float Time)
	{
		const float Length = Sequence->GetPlayLength();
		const float From = FMath::Clamp(Time - AnimSampleStep, 0.f, Length);
		const float To = FMath::Clamp(Time + AnimSampleStep, 0.f, Length);
		if (To - From <= 0.f)
		{
			return 0.f;
		}
		const FTransform Motion = Sequence->ExtractRootMotionFromRange(From, To, FAnimExtractContext(double(From), true));
		return Motion.GetTranslation().Size2D() / (To - From);
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
	// Lit by the island's character key light too.
	GetMesh()->SetLightingChannels(true, true, false);

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

	// Twinsen's skeleton: legs hip -> knee -> ankle, the head on the neck with a little of the turn in the torso.
	Legs = { { TEXT("bone_08"), TEXT("bone_09"), TEXT("bone_10") }, { TEXT("bone_11"), TEXT("bone_12"), TEXT("bone_13") } };
	HeadBones.Add(TEXT("bone_03"), 0.25f);
	HeadBones.Add(TEXT("bone_14"), 0.75f);
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
	// The idle (and, in authentic mode, every move) comes from the new entity.
	if (ModernState == EModernState::Idle)
	{
		SetModernState(EModernState::None);
	}
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
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = !bAuthentic;
	CameraBoom->bUsePawnControlRotation = !bAuthentic;
	CameraBoom->bInheritYaw = true;
	CameraBoom->bInheritPitch = !bAuthentic;
	CameraBoom->bInheritRoll = !bAuthentic;
	CameraBoom->bEnableCameraRotationLag = bAuthentic;
	if (bAuthentic)
	{
		CameraBoom->TargetArmLength = 650.f;
		CameraBoom->SocketOffset = FVector(0.f, 0.f, 60.f);
		CameraBoom->TargetOffset = FVector::ZeroVector;
		CameraBoom->bEnableCameraLag = false;
		CameraBoom->SetRelativeRotation(FRotator(-28.f, 0.f, 0.f));
		Movement->RotationRate = FRotator(0.f, 540.f, 0.f);
		Movement->JumpZVelocity = 520.f;
		Movement->AirControl = 0.2f;
		Movement->MaxWalkSpeed = 300.f;
		Movement->MaxAcceleration = 2048.f;
		Movement->BrakingDecelerationWalking = 2000.f;
	}
	else
	{
		// Free camera, like Unreal's third-person template, looking at Twinsen's chest.
		CameraBoom->TargetArmLength = ModernCameraDistance;
		CameraBoom->SocketOffset = FVector::ZeroVector;
		CameraBoom->TargetOffset = FVector(0.f, 0.f, 30.f);
		CameraBoom->bEnableCameraLag = true;
		CameraBoom->CameraLagSpeed = 15.f;
		CameraBoom->SetRelativeRotation(FRotator::ZeroRotator);
		Movement->RotationRate = FRotator(0.f, ModernTurnRate, 0.f);
		Movement->JumpZVelocity = ModernJumpSpeed;
		Movement->AirControl = ModernAirControl;
		Movement->MaxAcceleration = ModernAcceleration;
		Movement->BrakingDecelerationWalking = ModernBraking;
		Movement->MinAnalogWalkSpeed = 40.f;
		if (AController* PC = GetController())
		{
			PC->SetControlRotation(FRotator(-15.f, GetActorRotation().Yaw, 0.f));
		}
	}
	ModernState = EModernState::None;
	ProceduralWeight = 0.f;
	LastYaw = GetActorRotation().Yaw;
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		Anim->SetApplyRootMotion(bAuthentic);
		Anim->SetSmoothKeyframes(!bAuthentic && bSmoothKeyframes);
		Anim->SetSyncedBlend(nullptr, 0.f, 0.f, 0.f);
		Anim->SetProceduralPose(FLBAProceduralPose());
	}
}

UAnimSequence* ALBACharacter::FindAnim(int32 AnimType, float* OutLoopStart) const
{
	return FindBehaviourAnim(Behaviour, AnimType, OutLoopStart);
}

UAnimSequence* ALBACharacter::FindBehaviourAnim(ELBABehaviour InBehaviour, int32 AnimType, float* OutLoopStart) const
{
	const ULBAEntityAsset* Entity = Behaviours.FindRef(InBehaviour);
	if (!Entity)
	{
		Entity = GetEntity();
	}
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

const ALBACharacter::FClipInfo& ALBACharacter::GetClipInfo(const UAnimSequence* Sequence, float LoopStart) const
{
	if (const FClipInfo* Found = ClipInfos.Find(Sequence))
	{
		return *Found;
	}
	FClipInfo& Info = ClipInfos.Add(Sequence);
	const float Length = Sequence->GetPlayLength();
	Info.LoopStart = LoopStart < Length ? LoopStart : 0.f;
	Info.LoopLength = FMath::Max(Length - Info.LoopStart, AnimSampleStep);
	Info.Stride = Sequence->ExtractRootMotionFromRange(Info.LoopStart, Length, FAnimExtractContext(double(Info.LoopStart), true)).GetTranslation().Size2D();

	for (const FAnimNotifyEvent& Event : Sequence->Notifies)
	{
		const ULBAAnimNotify_Footstep* Step = Cast<ULBAAnimNotify_Footstep>(Event.Notify);
		if (Step && Step->Foot == ELBAFoot::Right && Event.GetTriggerTime() >= Info.LoopStart)
		{
			Info.StepPhase = (Event.GetTriggerTime() - Info.LoopStart) / Info.LoopLength;
			break;
		}
	}

	// Jumps: the root rises from the ground at takeoff and comes back down at landing.
	constexpr float GroundTolerance = 2.f;
	float PeakHeight = 0.f;
	float PeakTime = 0.f;
	TArray<float> Heights;
	for (float Time = 0.f; Time <= Length + KINDA_SMALL_NUMBER; Time += AnimSampleStep)
	{
		const float Height = Sequence->ExtractRootMotionFromRange(0.0, FMath::Min(Time, Length), FAnimExtractContext(0.0, true)).GetTranslation().Z;
		Heights.Add(Height);
		if (Height > PeakHeight)
		{
			PeakHeight = Height;
			PeakTime = Time;
		}
	}
	Info.LandTime = Length;
	if (PeakHeight > GroundTolerance)
	{
		for (int32 Index = 0; Index < Heights.Num(); ++Index)
		{
			if (Heights[Index] > GroundTolerance)
			{
				Info.TakeoffTime = FMath::Max(0, Index - 1) * AnimSampleStep;
				break;
			}
		}
		for (int32 Index = FMath::RoundToInt(PeakTime / AnimSampleStep); Index < Heights.Num(); ++Index)
		{
			if (Heights[Index] <= GroundTolerance)
			{
				Info.LandTime = FMath::Min(Index * AnimSampleStep, Length);
				break;
			}
		}
	}
	return Info;
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
	return PlayActionSequence(FindAnim(AnimType), AnimType);
}

bool ALBACharacter::PlayActionSequence(UAnimSequence* Sequence, int32 AnimType)
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	if (!Sequence || !Anim)
	{
		return false;
	}
	ActionSlot = AnimType;
	Anim->PlayLBAAnimation(Sequence, false, 0.f, 0.12f, 1.f, true);
	if (ControlMode == ELBAControlMode::Modern)
	{
		// Actions play in place; locomotion starts over afterwards.
		SetModernState(EModernState::None);
	}
	return true;
}

void ALBACharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!GetEntity())
	{
		return;
	}
	UpdateAttack(DeltaSeconds);
	if (IsClimbing() || TryStartClimb())
	{
		UpdateClimb(DeltaSeconds);
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

FVector ALBACharacter::GetMoveIntent() const
{
	if (ControlMode == ELBAControlMode::Authentic)
	{
		return FMath::Abs(MoveInput.Y) > AxisThreshold ? GetActorForwardVector() * FMath::Sign(MoveInput.Y) : FVector::ZeroVector;
	}
	if (!Controller || MoveInput.IsNearlyZero(0.01f))
	{
		return FVector::ZeroVector;
	}
	const FRotationMatrix Axes(FRotator(0.f, Controller->GetControlRotation().Yaw, 0.f));
	return (Axes.GetUnitAxis(EAxis::X) * MoveInput.Y + Axes.GetUnitAxis(EAxis::Y) * MoveInput.X).GetClampedToMaxSize(1.0);
}

// Modern mode ----------------------------------------------------------------------------------------------

void ALBACharacter::SetModernState(EModernState NewState)
{
	ModernState = NewState;
	ModernStateTime = 0.f;
}

void ALBACharacter::UpdateModern(float DeltaSeconds)
{
	ModernStateTime += DeltaSeconds;
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		if (ActionSlot != INDEX_NONE && Anim->IsCurrentAnimationFinished())
		{
			ActionSlot = INDEX_NONE;
		}
	}

	// Camera-relative movement, like the template (no moving during one-shot actions).
	if (Controller && ActionSlot == INDEX_NONE && !MoveInput.IsNearlyZero(0.01f))
	{
		const FRotator Yaw(0.f, Controller->GetControlRotation().Yaw, 0.f);
		const FRotationMatrix Axes(Yaw);
		AddMovementInput(Axes.GetUnitAxis(EAxis::X), MoveInput.Y);
		AddMovementInput(Axes.GetUnitAxis(EAxis::Y), MoveInput.X);
	}

	UpdateModernLocomotion(DeltaSeconds);
	UpdateProceduralPose(DeltaSeconds);
}

void ALBACharacter::UpdateModernLocomotion(float DeltaSeconds)
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	if (!Anim)
	{
		return;
	}
	UCharacterMovementComponent* Movement = GetCharacterMovement();

	// Twinsen always runs (athletic run); a half-tilted stick slows down into the normal walk.
	float RunLoopStart = 0.f;
	float WalkLoopStart = 0.f;
	UAnimSequence* Run = FindBehaviourAnim(ELBABehaviour::Athletic, int32(ELBAAnimType::Forward), &RunLoopStart);
	UAnimSequence* Walk = FindBehaviourAnim(ELBABehaviour::Normal, int32(ELBAAnimType::Forward), &WalkLoopStart);
	if (!Run || !Walk)
	{
		return;
	}
	const FClipInfo& RunInfo = GetClipInfo(Run, RunLoopStart);
	const FClipInfo& WalkInfo = GetClipInfo(Walk, WalkLoopStart);
	const float RunSpeed = FMath::Max(RunInfo.GetLoopSpeed(), 100.f);
	const float WalkSpeed = FMath::Clamp(WalkInfo.GetLoopSpeed(), 50.f, RunSpeed * 0.8f);
	Movement->MaxWalkSpeed = RunSpeed;

	if (ActionSlot != INDEX_NONE)
	{
		return;
	}

	const float Speed = GetVelocity().Size2D();
	const bool bHasInput = !MoveInput.IsNearlyZero(0.01f);

	if (Movement->IsFalling())
	{
		FallTime += DeltaSeconds;
		if (ModernState == EModernState::Jump)
		{
			// Past the jump's touchdown pose while still in the air: flail until landing.
			if (Anim->GetCurrentAnimationTime() >= JumpLandTime - 0.05f)
			{
				SetModernState(EModernState::Fall);
				PlaySlot(int32(ELBAAnimType::Falling), true, 0.3f);
			}
		}
		else if (ModernState != EModernState::Fall && FallTime > 0.2f)
		{
			SetModernState(EModernState::Fall);
			PlaySlot(int32(ELBAAnimType::Falling), true, 0.25f);
		}
		return;
	}
	FallTime = 0.f;

	if (ModernState == EModernState::Jump && ModernStateTime < 0.25f)
	{
		// The movement component takes off on its next update.
		return;
	}
	if (ModernState == EModernState::Jump || ModernState == EModernState::Fall)
	{
		// Back on the ground without the Landed event.
		SetModernState(EModernState::None);
	}
	if (ModernState == EModernState::Land)
	{
		// The landing recovery plays out, unless Twinsen runs on.
		if (!(bHasInput && ModernStateTime > 0.1f) && !Anim->IsCurrentAnimationFinished())
		{
			return;
		}
		bResumeCycle = bHasInput;
		SetModernState(EModernState::None);
	}

	const bool bMoving = bHasInput || Speed > 0.25f * RunSpeed;
	if (!bMoving)
	{
		if (ModernState != EModernState::Idle)
		{
			// Coming to a stop: the run fades into the idle while Twinsen brakes.
			PlaySlot(int32(ELBAAnimType::Idle), true, ModernState == EModernState::Move ? 0.35f : 0.2f);
			SetModernState(EModernState::Idle);
		}
		LocomotionLead = nullptr;
		return;
	}

	RunWeight = FMath::Clamp((Speed - WalkSpeed) / (RunSpeed - WalkSpeed), 0.f, 1.f);
	if (ModernState != EModernState::Move || !LocomotionLead)
	{
		// The cycle closest to the intended speed leads; it starts from its lead-in (the run start) from a stand.
		const float Intended = FMath::Min(MoveInput.Size(), 1.f) * RunSpeed;
		LocomotionLead = Intended > (WalkSpeed + RunSpeed) * 0.5f ? Run : Walk;
		const FClipInfo& Lead = LocomotionLead == Run ? RunInfo : WalkInfo;
		const float StartTime = bResumeCycle ? Lead.LoopStart : 0.f;
		Anim->PlayLBAAnimation(LocomotionLead, true, Lead.LoopStart, 0.2f, 1.f, true, StartTime);
		bResumeCycle = false;
		SetModernState(EModernState::Move);
	}
	const bool bRunLeads = LocomotionLead == Run;
	const FClipInfo& Lead = bRunLeads ? RunInfo : WalkInfo;
	const FClipInfo& Follow = bRunLeads ? WalkInfo : RunInfo;

	const float Time = Anim->GetCurrentAnimationTime();
	if (Time < Lead.LoopStart)
	{
		// Start: the lead-in plays at the pace of the actual acceleration (its own speed at that moment).
		LocomotionRate = FMath::Clamp(Speed / FMath::Max(RootMotionSpeedAt(LocomotionLead, Time), 30.f), 0.6f, 2.5f);
	}
	else
	{
		// Cycle: one loop per blended stride, so the feet keep up with the ground.
		const float Stride = FMath::Lerp(WalkInfo.Stride, RunInfo.Stride, RunWeight);
		LocomotionRate = FMath::Clamp(Speed / FMath::Max(Stride, 1.f) * Lead.LoopLength, 0.3f, 2.f);
	}
	Anim->PlayLBAAnimation(LocomotionLead, true, Lead.LoopStart, 0.2f, LocomotionRate);
	const float FollowWeight = bRunLeads ? 1.f - RunWeight : RunWeight;
	Anim->SetSyncedBlend(bRunLeads ? Walk : Run, Follow.LoopStart, FollowWeight, FMath::Frac(Follow.StepPhase - Lead.StepPhase + 1.f));
}

bool ALBACharacter::ModernJump()
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	if (!Anim || IsClimbing() || ActionSlot != INDEX_NONE || Movement->IsFalling() || ModernState == EModernState::Land)
	{
		return false;
	}
	float LoopStart = 0.f;
	const bool bRunning = GetVelocity().Size2D() > 150.f;
	JumpAnim = FindBehaviourAnim(ELBABehaviour::Athletic, int32(bRunning ? ELBAAnimType::RunningJump : ELBAAnimType::Jump), &LoopStart);
	if (!JumpAnim)
	{
		JumpAnim = FindBehaviourAnim(ELBABehaviour::Athletic, int32(ELBAAnimType::Jump), &LoopStart);
	}
	Jump();
	if (JumpAnim)
	{
		// Straight into the air (no wind-up), the airborne part stretched over the actual flight time.
		const FClipInfo& Info = GetClipInfo(JumpAnim, 0.f);
		const float FlightTime = 2.f * Movement->JumpZVelocity / FMath::Max(-Movement->GetGravityZ(), 1.f);
		const float Rate = FMath::Clamp((Info.LandTime - Info.TakeoffTime) / FlightTime, 0.5f, 2.f);
		JumpLandTime = Info.LandTime;
		Anim->PlayLBAAnimation(JumpAnim, false, 0.f, 0.1f, Rate, true, Info.TakeoffTime);
	}
	SetModernState(EModernState::Jump);
	return true;
}

void ALBACharacter::ModernLanded()
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	const float LandingSpeed = -GetVelocity().Z;
	if (LandingSpeed > HardLandingSpeed)
	{
		PlayAction(int32(ELBAAnimType::FallLandingStumble));
		return;
	}
	if (!Anim || (ModernState != EModernState::Jump && ModernState != EModernState::Fall))
	{
		// Short drops (down a step) don't interrupt the run.
		return;
	}
	// The touchdown and recovery of the jump animation (the athletic jump's for falls).
	UAnimSequence* Landing = JumpAnim;
	if (ModernState != EModernState::Jump || !Landing)
	{
		Landing = FindBehaviourAnim(ELBABehaviour::Athletic, int32(ELBAAnimType::Jump));
	}
	if (Landing)
	{
		const FClipInfo& Info = GetClipInfo(Landing, 0.f);
		const float From = ModernState == EModernState::Jump ? FMath::Max(Anim->GetCurrentAnimationTime(), Info.LandTime - 0.1f) : Info.LandTime - 0.1f;
		Anim->PlayLBAAnimation(Landing, false, 0.f, 0.1f, 1.f, true, From);
		SetModernState(EModernState::Land);
	}
	else
	{
		SetModernState(EModernState::None);
	}
}

void ALBACharacter::UpdateProceduralPose(float DeltaSeconds)
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	USkeletalMeshComponent* MeshComponent = GetMesh();
	if (!Anim || !MeshComponent->GetSkeletalMeshAsset() || DeltaSeconds <= 0.f)
	{
		return;
	}
	Anim->SetSmoothKeyframes(bSmoothKeyframes && CVarSmoothKeys.GetValueOnGameThread());
	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bGrounded = Movement->IsMovingOnGround();
	ProceduralWeight = FMath::FInterpConstantTo(ProceduralWeight, bGrounded && CVarProcedural.GetValueOnGameThread() ? 1.f : 0.f, DeltaSeconds, 6.f);

	// Lean into turns (sideways acceleration v * yaw rate) and accelerations, as if balancing on the feet.
	const float Yaw = GetActorRotation().Yaw;
	const float YawRate = FMath::DegreesToRadians(FMath::FindDeltaAngleDegrees(LastYaw, Yaw)) / DeltaSeconds;
	const float Speed = GetVelocity().Size2D();
	const float ForwardAcceleration = (Speed - LastSpeed) / DeltaSeconds;
	LastYaw = Yaw;
	LastSpeed = Speed;
	const float Gravity = FMath::Max(-Movement->GetGravityZ(), 1.f);
	const float TargetBank = bGrounded ? FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan(Speed * YawRate / Gravity)) * LeanAmount, -MaxLean, MaxLean) : 0.f;
	const float TargetPitch = bGrounded ? FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan(ForwardAcceleration / Gravity)) * 0.5f * LeanAmount, -MaxLean * 0.5f, MaxLean * 0.5f) : 0.f;
	LeanBank = FMath::FInterpTo(LeanBank, TargetBank, DeltaSeconds, 6.f);
	LeanPitch = FMath::FInterpTo(LeanPitch, TargetPitch, DeltaSeconds, 4.f);
	const FVector LeanUp = (FVector::UpVector + FVector::ForwardVector * FMath::Tan(FMath::DegreesToRadians(LeanPitch)) + FVector::RightVector * FMath::Tan(FMath::DegreesToRadians(LeanBank))).GetSafeNormal();
	// Actor space to the mesh's component space.
	const FQuat MeshRotation = MeshComponent->GetRelativeRotation().Quaternion();
	const FQuat Lean = MeshRotation.Inverse() * FQuat::FindBetweenNormals(FVector::UpVector, LeanUp) * MeshRotation;

	// Head: towards where Twinsen is going, or standing, where the camera looks.
	float TargetHead = 0.f;
	if (Controller)
	{
		const float ControlYaw = Controller->GetControlRotation().Yaw;
		if (!MoveInput.IsNearlyZero(0.01f))
		{
			const float InputYaw = ControlYaw + FMath::RadiansToDegrees(FMath::Atan2(MoveInput.X, MoveInput.Y));
			TargetHead = FMath::Clamp(FMath::FindDeltaAngleDegrees(Yaw, InputYaw), -60.f, 60.f);
		}
		else if (ModernState == EModernState::Idle)
		{
			const float Delta = FMath::FindDeltaAngleDegrees(Yaw, ControlYaw);
			TargetHead = FMath::Abs(Delta) < 110.f ? FMath::Clamp(Delta, -60.f, 60.f) : 0.f;
		}
	}
	HeadYaw = FMath::FInterpTo(HeadYaw, TargetHead * HeadTurn, DeltaSeconds, 5.f);

	FLBAProceduralPose Pose;
	Pose.Weight = ProceduralWeight;
	Pose.BodyRoot = BodyRootBone;
	Pose.Lean = Lean;
	Pose.HeadYaw = HeadYaw;
	for (const TPair<FName, float>& Bone : HeadBones)
	{
		Pose.HeadBones.Add(Bone);
	}

	if (bFootPlacement)
	{
		// Reference heights of the feet above the ground (the mesh origin is at the capsule's bottom).
		USkeletalMesh* SkeletalMesh = MeshComponent->GetSkeletalMeshAsset();
		if (FootHeightsMesh != SkeletalMesh)
		{
			FootHeightsMesh = SkeletalMesh;
			FootHeights.Reset();
			const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
			for (const FLBALegBones& Leg : Legs)
			{
				const int32 Index = RefSkeleton.FindBoneIndex(Leg.Foot);
				FootHeights.Add(Index != INDEX_NONE ? FAnimationRuntime::GetComponentSpaceTransformRefPose(RefSkeleton, Index).GetLocation().Z : 0.f);
			}
		}

		// Ground under each foot, relative to the bottom of the capsule (the animation's flat ground).
		const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float Bottom = GetActorLocation().Z - HalfHeight;
		const FQuat ComponentRotation = MeshComponent->GetComponentQuat();
		FCollisionQueryParams Params(TEXT("LBAFootPlacement"), false, this);
		float Lowest = 0.f;
		for (int32 Index = 0; Index < Legs.Num() && Index < 2; ++Index)
		{
			float TargetOffset = 0.f;
			FVector TargetNormal = FVector::UpVector;
			if (bGrounded)
			{
				const FVector Foot = MeshComponent->GetBoneLocation(Legs[Index].Foot);
				FHitResult Hit;
				const FVector From(Foot.X, Foot.Y, Bottom + Movement->MaxStepHeight + 10.f);
				const FVector To(Foot.X, Foot.Y, Bottom - 60.f);
				if (GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params) && Hit.ImpactNormal.Z > 0.5f)
				{
					TargetOffset = FMath::Clamp(Hit.ImpactPoint.Z - Bottom, -45.f, Movement->MaxStepHeight);
					TargetNormal = Hit.ImpactNormal;
				}
			}
			FootOffsets[Index] = FMath::FInterpTo(FootOffsets[Index], TargetOffset, DeltaSeconds, 15.f);
			FootNormals[Index] = FMath::VInterpTo(FootNormals[Index], TargetNormal, DeltaSeconds, 10.f).GetSafeNormal();
			Lowest = FMath::Min(Lowest, FootOffsets[Index]);

			FLBALegPlacement& Placement = Pose.Legs.AddDefaulted_GetRef();
			Placement.Hip = Legs[Index].Hip;
			Placement.Knee = Legs[Index].Knee;
			Placement.Foot = Legs[Index].Foot;
			Placement.GroundOffset = FootOffsets[Index];
			Placement.GroundNormal = ComponentRotation.Inverse().RotateVector(FootNormals[Index]);
			Placement.MinFootHeight = FootHeights.IsValidIndex(Index) ? FootHeights[Index] : 0.f;
		}
		// The body drops for the lower foot, the higher one bends its knee.
		PelvisOffset = FMath::FInterpTo(PelvisOffset, FMath::Max(Lowest, -40.f), DeltaSeconds, 10.f);
		Pose.PelvisOffset = PelvisOffset;
	}
	Anim->SetProceduralPose(Pose);
}

FString ALBACharacter::GetModernDebugString() const
{
	static const TCHAR* StateNames[] = { TEXT("none"), TEXT("idle"), TEXT("move"), TEXT("jump"), TEXT("fall"), TEXT("land") };
	const ULBAAnimInstance* Anim = GetLBAAnimInstance();
	const UAnimSequence* Current = Anim ? Anim->GetCurrentAnimation() : nullptr;
	return FString::Printf(TEXT("state=%s(%.2f) anim=%s@%.2f rate=%.2f run=%.2f speed=%.0f vz=%.0f weight=%.2f lean(bank=%.1f pitch=%.1f) head=%.1f pelvis=%.1f feet=(%.1f %.1f) action=%d"),
		StateNames[uint8(ModernState)], ModernStateTime, Current ? *Current->GetName() : TEXT("none"), Anim ? Anim->GetCurrentAnimationTime() : 0.f,
		LocomotionRate, RunWeight, GetVelocity().Size2D(), GetVelocity().Z, ProceduralWeight, LeanBank, LeanPitch, HeadYaw, PelvisOffset,
		FootOffsets[0], FootOffsets[1], ActionSlot);
}

void ALBACharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	if (ControlMode == ELBAControlMode::Modern)
	{
		if (ActionSlot == INDEX_NONE)
		{
			ModernLanded();
		}
		FallTime = 0.f;
		return;
	}
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
	auto MapStick = [this](UInputAction* Action, const FKey& Key) -> FEnhancedActionKeyMapping&
	{
		// Dead zone: resting sticks (or flight sticks/throttles) must not move Twinsen.
		FEnhancedActionKeyMapping& Mapping = InputContext->MapKey(Action, Key);
		UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(InputContext);
		DeadZone->LowerThreshold = 0.25f;
		Mapping.Modifiers.Add(DeadZone);
		return Mapping;
	};
	MapStick(MoveAction, EKeys::Gamepad_Left2D);

	LookAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	InputContext->MapKey(LookAction, EKeys::Mouse2D);
	{
		// Vertical inverted on the stick (pushing up looks down), not on the mouse, like the Dinofly.
		UInputModifierNegate* InvertY = NewObject<UInputModifierNegate>(InputContext);
		InvertY->bX = false;
		InvertY->bZ = false;
		MapStick(LookAction, EKeys::Gamepad_Right2D).Modifiers.Add(InvertY);
	}

	// Space: the behaviour action in authentic mode, jump in modern mode.
	ActionAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Action"));
	ActionAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(ActionAction, EKeys::SpaceBar);
	InputContext->MapKey(ActionAction, EKeys::Gamepad_FaceButton_Bottom);

	// E: the behaviour action (search, punch/kick, crouch).
	UseAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Use"));
	UseAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(UseAction, EKeys::E);
	InputContext->MapKey(UseAction, EKeys::Gamepad_FaceButton_Left);

	// Gamepad B, left mouse button: punch/kick in any behaviour.
	AttackAction = NewObject<UInputAction>(this, TEXT("IA_LBA_Attack"));
	AttackAction->ValueType = EInputActionValueType::Boolean;
	InputContext->MapKey(AttackAction, EKeys::LeftMouseButton);
	InputContext->MapKey(AttackAction, EKeys::Gamepad_FaceButton_Right);

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
	Input->BindAction(UseAction, ETriggerEvent::Started, this, &ALBACharacter::OnUse);
	Input->BindAction(AttackAction, ETriggerEvent::Started, this, &ALBACharacter::OnAttack);
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
	if (ControlMode == ELBAControlMode::Modern)
	{
		ModernJump();
	}
	else
	{
		DoBehaviourAction();
	}
}

void ALBACharacter::OnUse(const FInputActionValue& Value)
{
	// The Normal behaviour's action interacts too (with Twinsen's gesture); the others act only with nothing to use.
	if (Behaviour == ELBABehaviour::Normal || !TryInteract())
	{
		DoBehaviourAction();
	}
}

bool ALBACharacter::TryInteract()
{
	if (IsClimbing() || GetCharacterMovement()->IsFalling())
	{
		return false;
	}
	const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector Center = GetActorLocation() + GetActorForwardVector() * (Radius + InteractReach * 0.5f);
	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBAInteract), false, this);
	GetWorld()->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllObjects),
		FCollisionShape::MakeSphere(InteractReach * 0.5f + Radius), Params);

	AActor* Best = nullptr;
	double BestDistance = TNumericLimits<double>::Max();
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (!Actor || !Actor->Implements<ULBAInteractable>() || !ILBAInteractable::Execute_CanInteract(Actor, this))
		{
			continue;
		}
		const double Distance = Actor->GetComponentsBoundingBox().ComputeSquaredDistanceToPoint(Center);
		if (Distance < BestDistance)
		{
			Best = Actor;
			BestDistance = Distance;
		}
	}
	if (!Best)
	{
		return false;
	}
	ILBAInteractable::Execute_Interact(Best, this);
	return true;
}

void ALBACharacter::DoBehaviourAction()
{
	if (IsClimbing() || ActionSlot != INDEX_NONE || GetCharacterMovement()->IsFalling())
	{
		return;
	}
	switch (Behaviour)
	{
	case ELBABehaviour::Normal:
		PlayAction(int32(ELBAAnimType::Action));
		TryInteract();
		break;
	case ELBABehaviour::Athletic:
	{
		if (ControlMode == ELBAControlMode::Modern)
		{
			ModernJump();
			break;
		}
		const bool bRunning = MoveInput.Y > AxisThreshold || GetVelocity().Size2D() > 150.f;
		if (PlayAction(int32(bRunning ? ELBAAnimType::RunningJump : ELBAAnimType::Jump)) || PlayAction(int32(ELBAAnimType::Jump)))
		{
			LaunchCharacter(FVector(0.f, 0.f, JumpSpeed), false, true);
		}
		break;
	}
	case ELBABehaviour::Aggressive:
		Attack();
		break;
	case ELBABehaviour::Discreet:
		PlayAction(int32(ELBAAnimType::Crouch));
		break;
	}
}

// Melee ---------------------------------------------------------------------------------------------------

bool ALBACharacter::Attack()
{
	if (IsClimbing() || GetCharacterMovement()->IsFalling())
	{
		return false;
	}
	if (ActionSlot != INDEX_NONE)
	{
		// Pressed again once the blow landed: the next one follows.
		if (IsAttackSlot(ActionSlot) && AttackHitTime < 0.f)
		{
			bAttackQueued = true;
		}
		return false;
	}
	// The aggressive behaviour's blows, whatever the current behaviour; the original alternates punches and kicks.
	for (int32 Try = 0; Try < UE_ARRAY_COUNT(AttackSlots); ++Try)
	{
		const int32 Index = (AttackCycle + Try) % UE_ARRAY_COUNT(AttackSlots);
		UAnimSequence* Sequence = FindBehaviourAnim(ELBABehaviour::Aggressive, AttackSlots[Index]);
		if (!Sequence)
		{
			continue;
		}
		AimAttack();
		if (!PlayActionSequence(Sequence, AttackSlots[Index]))
		{
			return false;
		}
		AttackCycle = (Index + 1) % UE_ARRAY_COUNT(AttackSlots);
		AttackTime = 0.f;
		AttackLength = Sequence->GetPlayLength();
		// The blow lands at the original's hit frame (HIT_TWINSEN).
		AttackHitTime = AttackLength * 0.4f;
		for (const FAnimNotifyEvent& Event : Sequence->Notifies)
		{
			if (Cast<ULBAAnimNotify_Hit>(Event.Notify))
			{
				AttackHitTime = Event.GetTriggerTime();
				break;
			}
		}
		bAttackQueued = false;
		return true;
	}
	return false;
}

void ALBACharacter::UpdateAttack(float DeltaSeconds)
{
	if (!IsAttackSlot(ActionSlot))
	{
		AttackHitTime = -1.f;
		bAttackQueued = false;
		return;
	}
	// Timed here: ULBAAnimInstance doesn't dispatch notifies.
	AttackTime += DeltaSeconds;
	if (AttackHitTime >= 0.f && AttackTime >= AttackHitTime)
	{
		AttackHitTime = -1.f;
		DeliverMeleeHit();
	}
	if (bAttackQueued && AttackTime >= AttackLength - 0.1f)
	{
		ActionSlot = INDEX_NONE;
		Attack();
	}
}

void ALBACharacter::AimAttack()
{
	if (ControlMode != ELBAControlMode::Modern)
	{
		return;
	}
	const FVector Intent = GetMoveIntent();
	const FVector Aim = Intent.IsNearlyZero() ? GetActorForwardVector().GetSafeNormal2D() : Intent.GetSafeNormal2D();
	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBAMeleeAim), false, this);
	GetWorld()->OverlapMultiByObjectType(Overlaps, GetActorLocation(), FQuat::Identity, FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllObjects),
		FCollisionShape::MakeSphere(MeleeAssistRange), Params);

	FVector Facing = Aim;
	double BestDistance = TNumericLimits<double>::Max();
	const double MinCos = FMath::Cos(FMath::DegreesToRadians(MeleeAssistAngle));
	for (const FOverlapResult& Overlap : Overlaps)
	{
		const AActor* Actor = Overlap.GetActor();
		if (!IsMeleeTarget(Actor))
		{
			continue;
		}
		const FVector To = (Actor->GetActorLocation() - GetActorLocation()) * FVector(1.0, 1.0, 0.0);
		const double Distance = To.Size();
		if (Distance > 1.0 && Distance < BestDistance && FVector::DotProduct(To / Distance, Aim) >= MinCos)
		{
			Facing = To / Distance;
			BestDistance = Distance;
		}
	}
	SetActorRotation(FRotator(0.f, Facing.Rotation().Yaw, 0.f));
	// A turn, not a spin to lean into.
	LastYaw = GetActorRotation().Yaw;
}

void ALBACharacter::DeliverMeleeHit()
{
	// Like the original, the character's box pushed forward.
	const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Center = GetActorLocation() + GetActorForwardVector() * (Radius + MeleeReach * 0.5f);
	const FCollisionShape Box = FCollisionShape::MakeBox(FVector(Radius + MeleeReach * 0.5f, Radius + 20.f, HalfHeight));
	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBAMeleeHit), false, this);
	GetWorld()->OverlapMultiByObjectType(Overlaps, Center, GetActorQuat(), FCollisionObjectQueryParams(FCollisionObjectQueryParams::AllObjects), Box, Params);

	TSet<AActor*> Hit;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (!IsMeleeTarget(Actor) || Hit.Contains(Actor))
		{
			continue;
		}
		Hit.Add(Actor);
		// The stars fly out of the top of the target, like the original's.
		FVector Origin, Extent;
		Actor->GetActorBounds(true, Origin, Extent);
		Actor->FindComponentByClass<ULBAHitPointsComponent>()->TakeHit(MeleeDamage, Origin + FVector(0.0, 0.0, Extent.Z));
		UE_LOG(LogLBA2, Log, TEXT("%s hits %s"), *GetName(), *Actor->GetName());
	}
}

bool ALBACharacter::IsMeleeTarget(const AActor* Actor) const
{
	if (!Actor || Actor == this || Actor->IsA<ALBADinofly>())
	{
		return false;
	}
	const ULBAHitPointsComponent* HitPoints = Actor->FindComponentByClass<ULBAHitPointsComponent>();
	return HitPoints && !HitPoints->IsDead();
}

void ALBACharacter::OnToggleControlMode(const FInputActionValue& Value)
{
	SetControlMode(ControlMode == ELBAControlMode::Authentic ? ELBAControlMode::Modern : ELBAControlMode::Authentic);
}

ALBADinofly* ALBACharacter::SummonDinofly()
{
	if (IsClimbing() || ActionSlot != INDEX_NONE || GetCharacterMovement()->IsFalling())
	{
		return nullptr;
	}
	MoveInput = FVector2D::ZeroVector;
	return ALBADinofly::Summon(this, DinoflyClass);
}

ALBAGameMode::ALBAGameMode()
{
	DefaultPawnClass = ALBACharacter::StaticClass();
	PlayerControllerClass = ALBAPlayerController::StaticClass();
}

void ALBAGameMode::RestartPlayer(AController* NewPlayer)
{
	ULBASceneTravelSubsystem* Travel = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULBASceneTravelSubsystem>() : nullptr;
	if (NewPlayer && Travel && !NewPlayer->GetPawn())
	{
		const UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
		const ACharacter* Default = PawnClass ? Cast<ACharacter>(PawnClass->GetDefaultObject()) : nullptr;
		const float HalfHeight = Default ? Default->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 90.f;
		FTransform Arrival;
		if (Travel->ConsumeArrival(GetWorld(), HalfHeight, Arrival))
		{
			RestartPlayerAtTransform(NewPlayer, Arrival);
			return;
		}
	}
	Super::RestartPlayer(NewPlayer);
}

AActor* ALBAGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// Scene hero starts are tagged LBAScene_NNN: start at the first scene's (an interior has one; an island starts
	// where its lowest-numbered scene does), else at the level's own PlayerStart.
	APlayerStart* SceneStart = nullptr;
	int32 SceneStartIndex = TNumericLimits<int32>::Max();
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		FString Tag = It->PlayerStartTag.ToString();
		if (Tag.RemoveFromStart(TEXT("LBAScene_")) && FCString::Atoi(*Tag) < SceneStartIndex)
		{
			SceneStart = *It;
			SceneStartIndex = FCString::Atoi(*Tag);
		}
	}
	return SceneStart ? SceneStart : Super::ChoosePlayerStart_Implementation(Player);
}
