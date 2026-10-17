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

	/**
	 * Dinofly animation slots (entity 109), identified from their root motion, wing and leg bone motion. The flapping
	 * cycle (218) is the one shared by every flying animation (the takeoffs 339/340 loop on it); 152 and 278 are
	 * walks (wings folded), not flight.
	 */
	namespace DinoflyAnim
	{
		constexpr int32 Idle = 0;
		constexpr int32 Walk = 152;
		/** Flying pose to standing: the legs swing down at touchdown, a crouch absorbs it (slot 7, "falling"). */
		constexpr int32 Land = 343;
		constexpr float LandEnd = 0.74f;
		/**
		 * Vertical takeoff: crouch (deepest at 0.3 s), push off at 1.6 s, legs folded into the flying pose at 3.6 s.
		 * Played from the crouch at TakeoffRate, the push-off meets the flight component's TakeoffDelay (0.6 s).
		 * Its own wing beats (from 0.6 s, already twice as fast as the flapping cycle) would be frantic at that rate:
		 * from TakeoffWingsStart the wings play the flapping cycle instead, at about ClimbFlapRate.
		 */
		constexpr int32 Takeoff = 340;
		constexpr float TakeoffStart = 0.3f;
		constexpr float TakeoffWingsStart = 0.6f;
		constexpr float TakeoffLiftoff = 1.6f;
		constexpr float TakeoffEnd = 3.6f;
		constexpr float TakeoffRate = 2.2f;
		/** The takeoff's leg folding played backwards brings the legs down (legs only) while descending to land. */
		constexpr int32 LegsDown = -340;
		/** Height above the ground (cm, from the center) where the legs start coming down. */
		constexpr float LegsDownHeight = 320.f;
		/** Wings held spread, tail steering. */
		constexpr int32 TurnLeft = 3;
		constexpr int32 TurnRight = 4;
		/** Flapping while coming down. */
		constexpr int32 Descend = 30;
		/** The flapping cycle, played in place for hovering too. */
		constexpr int32 Flap = 218;
		constexpr float ClimbFlapRate = 1.3f;

		/** Bone branches for layered animations: the wings (bone_16, bone_19 chains) and the legs (bone_08, bone_12). */
		TArray<FName> WingBones() { return {TEXT("bone_16"), TEXT("bone_19")}; }
		TArray<FName> LegBones() { return {TEXT("bone_08"), TEXT("bone_12")}; }
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

	// Where Twinsen stands: his feet are the ground. Out of the way first, so the Dinofly can take his spot.
	const float HeroHalfHeight = Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Location = Hero->GetActorLocation() + FVector(0.f, 0.f, CollisionRadius + 2.f - HeroHalfHeight);
	const FRotator Rotation(0.f, Hero->GetActorRotation().Yaw, 0.f);
	Hero->SetActorEnableCollision(false);
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
		Dinofly->Flight->StartLanded();
		Dinofly->Mount(Hero);
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
	// Already standing: no landing to play.
	bWasLanded = Flight->IsLanded();
	ActionSlot = INDEX_NONE;
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		Anim->StopLBALayer(0.f);
	}
	LastYaw = GetActorRotation().Yaw;
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
	bAscendHeld = bDescendHeld = false;
	Flight->SetFlightInput(0.f, 0.f, 0.f, 0.f);
	Flight->SetHoverInput(0.f, 0.f, 0.f);
	ApplyEntity();
	return true;
}

void ALBADinofly::Thrust()
{
	Flight->Thrust();
}

void ALBADinofly::SetHoverInput(FVector2D Move, float Vertical)
{
	SteerInput = Move;
	bAscendHeld = Vertical > 0.f;
	bDescendHeld = Vertical < 0.f;
}

void ALBADinofly::SetFlightInput(float Pitch, float Roll, bool bFlap, bool bBrake)
{
	// Same convention as the stick: Y forward, so forward pitch input is "nose down" unless inverted.
	SteerInput = FVector2D(Roll, CVarInvertPitch.GetValueOnGameThread() ? Pitch : -Pitch);
	bAscendHeld = bFlap;
	bDescendHeld = bBrake;
}

bool ALBADinofly::IsHovering() const
{
	return Flight->Mode == ELBAFlightMode::Hover;
}

void ALBADinofly::ToggleFlightMode()
{
	// Keep the view where it is across the switch.
	const FRotator View = CameraBoom->GetComponentRotation();
	Flight->SetMode(IsHovering() ? ELBAFlightMode::Glide : ELBAFlightMode::Hover);
	CameraBase = FRotator(View.Pitch, View.Yaw, 0.f);
	LookOffset = FVector2D::ZeroVector;
}

// Tick --------------------------------------------------------------------------------------------------------

void ALBADinofly::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// The landing absorbs the touchdown: no input meanwhile.
	const bool bLocked = ActionSlot == DinoflyAnim::Land;
	const FVector2D Steer = bLocked ? FVector2D::ZeroVector : SteerInput;
	const bool bAscend = bAscendHeld && !bLocked;
	const bool bDescend = bDescendHeld && !bLocked;
	if (IsHovering() || Flight->IsLanded())
	{
		// Hover flight, and walking on the ground in both modes.
		Flight->SetHoverInput(Steer.Y, Steer.X, (bAscend ? 1.f : 0.f) - (bDescend ? 1.f : 0.f));
	}
	if (!IsHovering())
	{
		const float PitchInput = CVarInvertPitch.GetValueOnGameThread() ? Steer.Y : -Steer.Y;
		Flight->SetFlightInput(PitchInput, Steer.X, bAscend ? 1.f : 0.f, bDescend ? 1.f : 0.f);
	}
	UpdateAnimation(DeltaSeconds);
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

void ALBADinofly::PlayAction(int32 AnimType, float StartTime, float EndTime, float PlayRate)
{
	const int32 Slot = FMath::Abs(AnimType);
	const FLBAEntityAnim* Anim = Entity ? Entity->Anims.Find(Slot) : nullptr;
	ULBAAnimInstance* Instance = GetLBAAnimInstance();
	if (Anim && Anim->Sequence && Instance)
	{
		Instance->PlayLBAAnimation(Anim->Sequence, false, 0.f, 0.12f, PlayRate, true, StartTime);
		ActionSlot = AnimType;
		ActionEndTime = EndTime;
		ActionRate = PlayRate;
	}
}

void ALBADinofly::PlayLayer(int32 AnimType, const TArray<FName>& Bones, float StartTime, float PlayRate, float BlendTime)
{
	const FLBAEntityAnim* Anim = Entity ? Entity->Anims.Find(AnimType) : nullptr;
	ULBAAnimInstance* Instance = GetLBAAnimInstance();
	if (Anim && Anim->Sequence && Instance)
	{
		const bool bLoop = PlayRate >= 0.f;
		Instance->PlayLBALayer(Anim->Sequence, Bones, bLoop, Anim->LoopStartTime, BlendTime, PlayRate, StartTime);
	}
}

void ALBADinofly::UpdateAnimation(float DeltaSeconds)
{
	const bool bLanded = Flight->IsLanded();
	const FVector Velocity = Flight->Velocity;
	const float Yaw = GetActorRotation().Yaw;
	const float YawRate = DeltaSeconds > 0.f ? FRotator::NormalizeAxis(Yaw - LastYaw) / DeltaSeconds : 0.f;
	LastYaw = Yaw;

	ULBAAnimInstance* Instance = GetLBAAnimInstance();
	if (!Instance)
	{
		return;
	}

	// One-shot actions, started by the flight state.
	if (bLanded && !bWasLanded)
	{
		// The legs are already down after a descent: start after the swing.
		PlayAction(DinoflyAnim::Land, ActionSlot == DinoflyAnim::LegsDown ? 0.1f : 0.f, DinoflyAnim::LandEnd, 1.f);
		Instance->StopLBALayer(0.12f);
	}
	else if (Flight->IsTakingOff() && ActionSlot != DinoflyAnim::Takeoff)
	{
		PlayAction(DinoflyAnim::Takeoff, DinoflyAnim::TakeoffStart, DinoflyAnim::TakeoffEnd, DinoflyAnim::TakeoffRate);
		Instance->StopLBALayer(0.12f);
	}
	else if (!bLanded && IsHovering() && ActionSlot == INDEX_NONE && Velocity.Z < -150.f
		&& Flight->GetHeightAboveSurface(DinoflyAnim::LegsDownHeight + 200.f) < DinoflyAnim::LegsDownHeight)
	{
		// Coming down near the ground: unfold the legs over the time left before touchdown, the wings keep flapping.
		const float Height = FMath::Max(Flight->GetHeightAboveSurface(DinoflyAnim::LegsDownHeight + 200.f) - CollisionRadius, 1.f);
		const float TimeLeft = FMath::Max(Height / -Velocity.Z, 0.15f);
		const float Span = DinoflyAnim::TakeoffEnd - DinoflyAnim::TakeoffLiftoff;
		PlayLayer(DinoflyAnim::Takeoff, DinoflyAnim::LegBones(), DinoflyAnim::TakeoffEnd, -Span / TimeLeft, 0.1f);
		ActionSlot = DinoflyAnim::LegsDown;
		ActionEndTime = DinoflyAnim::TakeoffLiftoff;
		ActionRate = -Span / TimeLeft;
	}
	bWasLanded = bLanded;

	if (ActionSlot == DinoflyAnim::LegsDown)
	{
		// Legs down (a layer over the flight animations): held until the touchdown; climbing again folds them back.
		if (Velocity.Z > 0.f)
		{
			Instance->StopLBALayer(0.3f);
			ActionSlot = INDEX_NONE;
		}
		else if (Instance->GetLayerTime() <= ActionEndTime)
		{
			Instance->SetLayerPlayRate(0.f);
		}
	}
	else if (ActionSlot != INDEX_NONE)
	{
		const float Time = Instance->GetCurrentAnimationTime();
		const bool bRunning = !Instance->IsCurrentAnimationFinished()
			&& (ActionRate >= 0.f ? Time < ActionEndTime : Time > ActionEndTime);
		if (ActionSlot == DinoflyAnim::Takeoff)
		{
			const FLBAEntityAnim* Flap = Entity ? Entity->Anims.Find(DinoflyAnim::Flap) : nullptr;
			if (bRunning && Time >= DinoflyAnim::TakeoffWingsStart && !Instance->IsLayerActive() && Flap && Flap->Sequence)
			{
				// Wings beating at a natural pace, a whole number of beats so they meet the flying pose at the end.
				const float TimeLeft = (DinoflyAnim::TakeoffEnd - Time) / DinoflyAnim::TakeoffRate;
				const float Cycle = Flap->Sequence->GetPlayLength() - Flap->LoopStartTime;
				const float Beats = FMath::Max(1.f, FMath::RoundToFloat(TimeLeft * DinoflyAnim::ClimbFlapRate / Cycle));
				PlayLayer(DinoflyAnim::Flap, DinoflyAnim::WingBones(), Flap->LoopStartTime, Beats * Cycle / TimeLeft, 0.1f);
			}
			else if (!bRunning && Instance->IsLayerActive() && Flap && Flap->Sequence)
			{
				// Flying: the flapping cycle carries on from the wings' beat.
				Instance->PlayLBAAnimation(Flap->Sequence, true, Flap->LoopStartTime, 0.12f, DinoflyAnim::ClimbFlapRate, false, Instance->GetLayerTime());
				Instance->StopLBALayer(0.12f);
			}
		}
		if (bRunning)
		{
			return;
		}
		ActionSlot = INDEX_NONE;
	}

	if (bLanded)
	{
		// Walking slowly, or stepping around while turning on the spot.
		const float Speed = Velocity.Size2D();
		if (Speed > 15.f)
		{
			PlaySlot(DinoflyAnim::Walk, FMath::Clamp(Speed / Flight->WalkSpeed, 0.4f, 1.2f), 0.2f);
		}
		else if (FMath::Abs(YawRate) > 20.f)
		{
			PlaySlot(DinoflyAnim::Walk, 0.6f, 0.2f);
		}
		else
		{
			PlaySlot(DinoflyAnim::Idle, 1.f, 0.3f);
		}
		return;
	}
	if (IsHovering())
	{
		const float Horizontal = Flight->Velocity.Size2D();
		if (Horizontal < 250.f)
		{
			// Hovering in place: the flapping cycle, beating faster when climbing.
			const float Vertical = Flight->Velocity.Z;
			if (Vertical < -250.f)
			{
				PlaySlot(DinoflyAnim::Descend);
			}
			else
			{
				PlaySlot(DinoflyAnim::Flap, Vertical > 100.f ? DinoflyAnim::ClimbFlapRate : 1.f);
			}
		}
		else if (FMath::Abs(Flight->GetTurnRate()) > 90.f)
		{
			PlaySlot(Flight->GetTurnRate() > 0.f ? DinoflyAnim::TurnRight : DinoflyAnim::TurnLeft);
		}
		else
		{
			// Faster beats with speed, and a burst on each thrust.
			const float Rate = FMath::GetMappedRangeValueClamped(FVector2D(250.f, Flight->GetMaxHoverSpeed()), FVector2D(0.8f, 1.5f), Horizontal);
			PlaySlot(DinoflyAnim::Flap, Rate + 0.8f * Flight->GetThrustKick());
		}
		return;
	}
	const float Speed = Flight->Velocity.Size();
	const bool bFlapping = Flight->GetFlapInput() > 0.f;
	const float Bank = Flight->GetBank();
	if (Speed < 350.f || (bFlapping && Speed < 500.f))
	{
		PlaySlot(DinoflyAnim::Flap, bFlapping ? 1.2f : 0.9f);
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
		PlaySlot(DinoflyAnim::Descend);
	}
}

void ALBADinofly::UpdateCamera(float DeltaSeconds)
{
	// Raised and looking down toward the Dinofly and the ground when descending or high above the ground.
	const bool bHovering = IsHovering();
	const FVector Velocity = Flight->Velocity;
	const float HeightAlpha = Flight->IsLanded() ? 0.f
		: FMath::Clamp((Flight->GetHeightAboveSurface(HighAltitude + 500.f) - 400.f) / (HighAltitude - 400.f), 0.f, 1.f);
	const float DescendAlpha = bHovering ? FMath::Clamp(-Velocity.Z / Flight->DescendSpeed, 0.f, 1.f) : 0.f;
	CameraDrop = FMath::FInterpTo(CameraDrop, FMath::Clamp(0.65f * HeightAlpha + 0.65f * DescendAlpha, 0.f, 1.f), DeltaSeconds, 1.5f);
	// Tilting the boom down raises the camera along its arc, looking down at the Dinofly; bring the Dinofly a bit
	// toward the center of the view meanwhile, so it stays framed with the ground around it.
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 120.f - 60.f * CameraDrop);

	// Behind the Dinofly, swinging after it with some lag; never banked. Gliding also follows the climb or dive.
	const float Yaw = !bHovering && Flight->IsLanded() ? GetActorRotation().Yaw : Flight->GetHeading().Rotation().Yaw;
	const float PathPitch = bHovering || Flight->IsLanded() ? 0.f : Flight->GetFlightPathAngle();
	const FRotator Target(FMath::Clamp(-15.f - MaxCameraDrop * CameraDrop + 0.4f * PathPitch, -60.f, 20.f), Yaw, 0.f);
	CameraBase = FMath::RInterpTo(CameraBase, Target, DeltaSeconds, bHovering ? CameraFollowSpeed : 2.5f);

	// The look offset eases back behind the Dinofly when the player stops looking around.
	if (GetWorld()->GetTimeSeconds() - LastLookTime > 1.5f)
	{
		LookOffset = FMath::Vector2DInterpTo(LookOffset, FVector2D::ZeroVector, DeltaSeconds, 1.5f);
	}
	CameraBoom->SetWorldRotation(FRotator(FMath::Clamp(CameraBase.Pitch + LookOffset.Y, -80.f, 60.f), CameraBase.Yaw + LookOffset.X, 0.f));

	// Field of view: wider with speed, accelerations (each thrust punches it), climbing or descending, and height.
	const float HorizontalSpeed = Velocity.Size2D();
	if (DeltaSeconds > 0.f)
	{
		SmoothedAcceleration = FMath::FInterpTo(SmoothedAcceleration, (HorizontalSpeed - LastHorizontalSpeed) / DeltaSeconds, DeltaSeconds, 6.f);
	}
	LastHorizontalSpeed = HorizontalSpeed;
	const float SpeedAlpha = bHovering
		? FMath::Clamp((HorizontalSpeed - Flight->HoverSpeed) / Flight->MaxThrustSpeed, 0.f, 1.f)
		: FMath::Clamp((Velocity.Size() - Flight->GetTrimSpeed()) / 1500.f, 0.f, 1.f);
	const float AccelerationAlpha = FMath::Clamp(SmoothedAcceleration / 1500.f, 0.f, 1.f);
	const float VerticalAlpha = bHovering ? FMath::Clamp(FMath::Abs(Velocity.Z) / Flight->DescendSpeed, 0.f, 1.f) : 0.f;
	const float FieldOfView = FMath::Min(75.f + 14.f * SpeedAlpha + 10.f * AccelerationAlpha + 6.f * VerticalAlpha + 8.f * HeightAlpha, 105.f);
	CameraBoom->TargetArmLength = FMath::FInterpTo(CameraBoom->TargetArmLength, 850.f + 300.f * SpeedAlpha + 200.f * HeightAlpha, DeltaSeconds, 2.f);
	FollowCamera->SetFieldOfView(FMath::FInterpTo(FollowCamera->FieldOfView, FieldOfView, DeltaSeconds, 3.f));
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

	auto MakeButton = [this](const TCHAR* Name, std::initializer_list<FKey> Keys)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = EInputActionValueType::Boolean;
		for (const FKey& Key : Keys)
		{
			InputContext->MapKey(Action, Key);
		}
		return Action;
	};
	AscendAction = MakeButton(TEXT("IA_LBA_Dinofly_Ascend"), { EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom, EKeys::Gamepad_RightTrigger });
	DescendAction = MakeButton(TEXT("IA_LBA_Dinofly_Descend"), { EKeys::LeftControl, EKeys::C, EKeys::Gamepad_FaceButton_Right, EKeys::Gamepad_LeftTrigger });
	ThrustAction = MakeButton(TEXT("IA_LBA_Dinofly_Thrust"), { EKeys::LeftShift, EKeys::Gamepad_FaceButton_Left, EKeys::Gamepad_RightShoulder });
	ModeAction = MakeButton(TEXT("IA_LBA_Dinofly_Mode"), { EKeys::Tab, EKeys::Gamepad_Special_Left });

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
	Input->BindAction(AscendAction, ETriggerEvent::Started, this, &ALBADinofly::OnAscend);
	Input->BindAction(AscendAction, ETriggerEvent::Completed, this, &ALBADinofly::OnAscendReleased);
	Input->BindAction(DescendAction, ETriggerEvent::Started, this, &ALBADinofly::OnDescend);
	Input->BindAction(DescendAction, ETriggerEvent::Completed, this, &ALBADinofly::OnDescendReleased);
	Input->BindAction(ThrustAction, ETriggerEvent::Started, this, &ALBADinofly::OnThrust);
	Input->BindAction(ModeAction, ETriggerEvent::Started, this, &ALBADinofly::OnToggleMode);
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
