#include "LBADinofly.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/CollisionProfile.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "DrawDebugHelpers.h"
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
#include "LBAControls.h"
#include "InputModifiers.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAAirflowComponent.h"
#include "LBAAnimInstance.h"
#include "LBACharacter.h"
#include "LBADeathScreen.h"
#include "LBADinoflySettings.h"
#include "LBAEntityAsset.h"
#include "LBAFlightMovementComponent.h"
#include "LBAHitPointsComponent.h"
#include "LBATypes.h"
#include "TimerManager.h"

namespace
{
	TAutoConsoleVariable<bool> CVarInvertPitch(
		TEXT("lba.Dinofly.InvertPitch"), false,
		TEXT("Dinofly pitch controls: false = forward dives (flight stick), true = forward climbs."));

	TAutoConsoleVariable<bool> CVarDinoflyDebug(
		TEXT("lba.Dinofly.Debug"), false,
		TEXT("Shows the Dinofly's life, air density, vertical speed and beats as text above it (debug builds)."));

	FAutoConsoleCommandWithWorldAndArgs HurtCommand(
		TEXT("lba.Dinofly.Hurt"),
		TEXT("Hurts the ridden Dinofly: lba.Dinofly.Hurt <damage> (default 20; 1000 kills it)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			if (ALBADinofly* Dinofly = PC ? Cast<ALBADinofly>(PC->GetPawn()) : nullptr)
			{
				const float Damage = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 20.f;
				Dinofly->GetHitPoints()->TakeHit(Damage, Dinofly->GetActorLocation() + FVector(0.f, 0.f, 150.f));
			}
		}));

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
		/**
		 * The flapping cycle's wings: up at 0, spread wide at GlideTime, lowest at 0.32. Gliding holds the spread pose;
		 * a beat plays one cycle from there back to it.
		 */
		constexpr float GlideTime = 0.12f;
		/** A flinch, head thrown back, standing (the original's "takes a hit"). */
		constexpr int32 Hurt = 5;
		constexpr float HurtLength = 0.6f;
		/** The flinch as a layer on the head and neck, flying. */
		constexpr int32 HurtLayer = -5;
		/** Lying on its side, the rider thrown off (337: the same, raising its head; 338: getting up). */
		constexpr int32 Dead = 335;

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
	// Lit by the island's character key light too.
	Mesh->SetLightingChannels(true, true, false);

	Flight = CreateDefaultSubobject<ULBAFlightMovementComponent>(TEXT("Flight"));
	Flight->SetUpdatedComponent(Collision);

	// Airflow strokes from the hotspots where the flow breaks away from the body. Body 166's skeleton (bone_N = vertex
	// group N): bone_02 body, bone_03-07 neck to head, bone_08-11 and bone_12-15 legs, bone_16-18 right wing and
	// bone_19-21 left wing (tips 280 out and 680 back from bone_18/21), bone_22-24 tail (tip 480 back), bone_25-26
	// Twinsen. Offsets in bone space at the 0.125 world scale (LBA x, z, y).
	Airflow = CreateDefaultSubobject<ULBAAirflowComponent>(TEXT("Airflow"));
	{
		auto Hotspot = [](const TCHAR* Bone, const FVector& Offset, float Spread, float Accent, float Wake, float Turbulence, float Follow = 0.f, float MinShedding = 0.f)
		{
			FLBAAirflowSource Source;
			Source.Bone = Bone;
			Source.Offset = Offset;
			Source.Spread = Spread;
			Source.Accent = Accent;
			Source.Wake = Wake;
			Source.Turbulence = Turbulence;
			Source.Follow = Follow;
			Source.MinShedding = MinShedding;
			return Source;
		};
		const FVector Zero = FVector::ZeroVector;
		const FVector RightTip(35.0, -85.0, 0.0);
		const FVector LeftTip(-35.0, -85.0, 0.0);
		Airflow->Sources = {
			// Wingtips, where the vortices start whichever way they swing, and the wings' middle (flapping steers their
			// strokes).
			Hotspot(TEXT("bone_18"), RightTip, 6.f, 1.3f, 0.4f, 10.f, 0.5f, 0.5f),
			Hotspot(TEXT("bone_21"), LeftTip, 6.f, 1.3f, 0.4f, 10.f, 0.5f, 0.5f),
			Hotspot(TEXT("bone_17"), Zero, 12.f, 0.6f, 0.45f, 5.f, 0.3f),
			Hotspot(TEXT("bone_20"), Zero, 12.f, 0.6f, 0.45f, 5.f, 0.3f),
			// The tail tip, the feet and the rider: blunt ends the flow tears off.
			Hotspot(TEXT("bone_24"), FVector(0.0, -60.0, -2.5), 6.f, 1.2f, 0.5f, 12.f),
			Hotspot(TEXT("bone_11"), Zero, 8.f, 0.8f, 0.5f, 8.f),
			Hotspot(TEXT("bone_15"), Zero, 8.f, 0.8f, 0.5f, 8.f),
			Hotspot(TEXT("bone_26"), Zero, 10.f, 0.8f, 0.5f, 8.f),
			// The head, for climbs, dives and flying backwards.
			Hotspot(TEXT("bone_07"), Zero, 10.f, 0.6f, 0.5f, 4.f),
		};
	}

	HitPoints = CreateDefaultSubobject<ULBAHitPointsComponent>(TEXT("HitPoints"));

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
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	HitPoints->MaxLife = Settings.MaxLife;
	HitPoints->DamagePerStar = Settings.DamagePerStar;
	Super::BeginPlay();
	Flight->OnImpact.AddDynamic(this, &ALBADinofly::HandleImpact);
	HitPoints->OnHurt.AddDynamic(this, &ALBADinofly::HandleHurt);
	HitPoints->OnDeath.AddDynamic(this, &ALBADinofly::HandleDeath);
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
	LastBeatCount = Flight->GetBeatCount();
	WingCyclesLeft = 0;
	WingPhase = 0.f;
	bGlideBase = false;
}

bool ALBADinofly::Dismount()
{
	// On the ground only: floating on the sea would drop the rider into deep water.
	if (!Flight->IsLanded() || Flight->IsFloating() || bDead)
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
	bAscendHeld = bDescendHeld = bDiveHeld = false;
	Flight->SetFlightInput(0.f, 0.f, 0.f, 0.f);
	Flight->SetHoverInput(0.f, 0.f, 0.f);
	Flight->SetDiveInput(false);
	ApplyEntity();
	return true;
}

void ALBADinofly::SetDiveInput(bool bDive)
{
	bDiveHeld = bDive;
}

void ALBADinofly::SetHoverInput(FVector2D Move, float Vertical)
{
	SteerInput = Move;
	// Pressing (not holding) beats or pushes, like the buttons.
	AscendPresses += Vertical > 0.f && !bAscendHeld ? 1 : 0;
	DescendPresses += Vertical < 0.f && !bDescendHeld ? 1 : 0;
	bAscendHeld = Vertical > 0.f;
	bDescendHeld = Vertical < 0.f;
}

void ALBADinofly::Flap()
{
	++AscendPresses;
}

void ALBADinofly::BrakeTap()
{
	++DescendPresses;
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
	// Glide mode plays the clips at their own rates.
	bGlideBase = false;
	CameraBase = FRotator(View.Pitch, View.Yaw, 0.f);
	LookOffset = FVector2D::ZeroVector;
}

// Tick --------------------------------------------------------------------------------------------------------

void ALBADinofly::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// The landing (or a flinch) absorbs the touchdown: no input meanwhile; none at all once dead.
	const bool bLocked = ActionSlot == DinoflyAnim::Land || ActionSlot == DinoflyAnim::Hurt || bDead;
	const FVector2D Steer = bLocked ? FVector2D::ZeroVector : SteerInput;
	const bool bAscend = bAscendHeld && !bLocked;
	const bool bDescend = bDescendHeld && !bLocked;
	if (IsHovering() || Flight->IsLanded())
	{
		// Hover flight, and walking on the ground in both modes.
		Flight->SetHoverInput(Steer.Y, Steer.X, (bAscend ? 1.f : 0.f) - (bDescend ? 1.f : 0.f));
	}
	Flight->SetDiveInput(bDiveHeld && !bLocked && IsHovering());
	if (!bLocked && IsHovering())
	{
		for (; AscendPresses > 0; --AscendPresses)
		{
			Flight->Beat();
		}
		for (; DescendPresses > 0; --DescendPresses)
		{
			Flight->BrakeTap();
		}
	}
	AscendPresses = DescendPresses = 0;
	if (Flight->IsLanded() && !Flight->IsFloating())
	{
		HitPoints->Heal(ULBADinoflySettings::Get().LifeRegenPerSecond * DeltaSeconds);
	}
	if (!IsHovering())
	{
		const float PitchInput = CVarInvertPitch.GetValueOnGameThread() ? Steer.Y : -Steer.Y;
		Flight->SetFlightInput(PitchInput, Steer.X, bAscend ? 1.f : 0.f, bDescend ? 1.f : 0.f);
	}
	bProceduralWings = false;
	UpdateAnimation(DeltaSeconds);
	UpdateProceduralWings(DeltaSeconds);
	UpdateCamera(DeltaSeconds);
	// No wake on the ground or the water.
	Airflow->SetEmitting(!Flight->IsLanded());
	DrawDebug();
}

// Pain -----------------------------------------------------------------------------------------------------------

void ALBADinofly::HandleImpact(float ImpactSpeed, ELBAFlightImpact Kind, FVector Location)
{
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	const float Damage = Kind == ELBAFlightImpact::Ground ? Settings.GetDamage(ImpactSpeed, Settings.GroundPainSpeed)
		: Kind == ELBAFlightImpact::Water ? Settings.GetDamage(ImpactSpeed, Settings.WaterPainSpeed, Settings.WaterDamageScale)
		: Settings.GetDamage(ImpactSpeed, Settings.ObstaclePainSpeed);
	if (Damage > 0.f)
	{
		UE_LOG(LogLBA2, Log, TEXT("%s: hit %s at %.0f cm/s, %.1f damage"), *GetName(),
			*StaticEnum<ELBAFlightImpact>()->GetNameStringByValue(int64(Kind)), ImpactSpeed, Damage);
		// The stars fly out of the Dinofly's back, like the original's from the top of the hurt object.
		bHurtOnGround = Kind == ELBAFlightImpact::Ground;
		HitPoints->TakeHit(Damage, GetActorLocation() + FVector(0.f, 0.f, 150.f));
	}
}

void ALBADinofly::HandleHurt(float Damage, FVector Location)
{
	bHurtPending = !HitPoints->IsDead();
}

void ALBADinofly::HandleDeath()
{
	if (bDead)
	{
		return;
	}
	bDead = true;
	bHurtPending = false;
	Flight->SetDead(true);
	if (ULBAAnimInstance* Instance = GetLBAAnimInstance())
	{
		Instance->StopLBALayer(0.2f);
	}
	ActionSlot = INDEX_NONE;
	// Lying on its side, the rider thrown off: limp in the air already, it falls that way.
	PlaySlot(DinoflyAnim::Dead, 1.f, 0.35f);
	UE_LOG(LogLBA2, Display, TEXT("%s died."), *GetName());
	if (IsPlayerControlled())
	{
		GetWorldTimerManager().SetTimer(DeathTimer, this, &ALBADinofly::ShowDeathScreen, FMath::Max(ULBADinoflySettings::Get().RetryDelay, 0.01f));
	}
}

void ALBADinofly::ShowDeathScreen()
{
	ULBADeathScreen::Show(Cast<APlayerController>(GetController()), DeathScreenClass);
}

void ALBADinofly::DrawDebug() const
{
#if ENABLE_DRAW_DEBUG
	if (!CVarDinoflyDebug.GetValueOnGameThread())
	{
		return;
	}
	const FVector Air = Flight->GetAirVelocity();
	const FString Text = FString::Printf(TEXT("life %.0f/%.0f\nalt %.1f m  air x%.2f\nvz %+.1f m/s  speed %.1f m/s\nbeats %d, last %.1f s ago\nstall %.0f%%  brake %.0f%%, endurance %.0f%%%s\ndive %.0f%%"),
		HitPoints->GetLife(), HitPoints->MaxLife, GetActorLocation().Z / 100.f, Flight->GetAirDensity(),
		Air.Z / 100.f, Air.Size2D() / 100.f, Flight->GetBeatCount(), Flight->GetTimeSinceBeat(),
		100.f * Flight->GetStall(), 100.f * Flight->GetBrakeAmount(), 100.f * Flight->GetBrakeStamina(), Flight->IsBrakeExhausted() ? TEXT(" (spent)") : TEXT(""),
		100.f * Flight->GetDiveAggression());
	DrawDebugString(GetWorld(), GetActorLocation() + FVector(0.f, 0.f, 320.f), Text, nullptr, HitPoints->IsDead() ? FColor::Red : FColor::White, 0.f, true, 1.1f);
#endif
}

void ALBADinofly::PlaySlot(int32 AnimType, float PlayRate, float BlendTime)
{
	const FLBAEntityAnim* Anim = Entity ? Entity->Anims.Find(AnimType) : nullptr;
	ULBAAnimInstance* Instance = GetLBAAnimInstance();
	if (Anim && Anim->Sequence && Instance)
	{
		Instance->PlayLBAAnimation(Anim->Sequence, true, Anim->LoopStartTime, BlendTime, PlayRate);
		bGlideBase = false;
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
		bGlideBase = false;
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
	if (bDead)
	{
		// Lying on its side (set when it died), wherever it falls.
		bWasLanded = bLanded;
		return;
	}

	// One-shot actions, started by the flight state.
	if (bLanded && !bWasLanded)
	{
		if (bHurtPending && bHurtOnGround)
		{
			// A landing that hurt: a flinch instead of the landing.
			PlayAction(DinoflyAnim::Hurt, 0.f, 10.f, 1.f);
			bHurtPending = false;
		}
		else
		{
			// The legs are already down after a descent: start after the swing.
			PlayAction(DinoflyAnim::Land, ActionSlot == DinoflyAnim::LegsDown ? 0.1f : 0.f, DinoflyAnim::LandEnd, 1.f);
		}
		Instance->StopLBALayer(0.12f);
	}
	else if (bHurtPending && bLanded && ActionSlot != DinoflyAnim::Takeoff)
	{
		// Hurt standing or floating: flinch.
		PlayAction(DinoflyAnim::Hurt, 0.f, 10.f, 1.f);
		Instance->StopLBALayer(0.12f);
		bHurtPending = false;
	}
	else if (bHurtPending && !bLanded && !Instance->IsLayerActive())
	{
		// Hurt flying: the head and neck flinch, the wings carry on.
		PlayLayer(DinoflyAnim::Hurt, { TEXT("bone_03") }, 0.f, 1.f, 0.08f);
		ActionSlot = DinoflyAnim::HurtLayer;
		ActionEndTime = DinoflyAnim::HurtLength;
		bHurtPending = false;
	}
	else if (bHurtPending && !bLanded)
	{
		// Legs down or wings tucked already (one layer at a time): the stars say it.
		bHurtPending = false;
	}
	else if (Flight->IsTakingOff() && ActionSlot != DinoflyAnim::Takeoff)
	{
		PlayAction(DinoflyAnim::Takeoff, DinoflyAnim::TakeoffStart, DinoflyAnim::TakeoffEnd, DinoflyAnim::TakeoffRate);
		Instance->StopLBALayer(0.12f);
	}
	else if (!bLanded && IsHovering() && !Flight->IsDiving() && ActionSlot == INDEX_NONE && (Velocity.Z < -150.f || IsBrakingLow())
		&& Flight->GetHeightAboveSurface(DinoflyAnim::LegsDownHeight + 200.f) < DinoflyAnim::LegsDownHeight)
	{
		// Coming down near the ground, or braking to land: unfold the legs over the time left before touchdown (at most
		// a moment), the wings keep flapping.
		const float Height = FMath::Max(Flight->GetHeightAboveSurface(DinoflyAnim::LegsDownHeight + 200.f) - CollisionRadius, 1.f);
		const float TimeLeft = FMath::Clamp(Height / FMath::Max(-Velocity.Z, 1.f), 0.15f, 0.8f);
		const float Span = DinoflyAnim::TakeoffEnd - DinoflyAnim::TakeoffLiftoff;
		PlayLayer(DinoflyAnim::Takeoff, DinoflyAnim::LegBones(), DinoflyAnim::TakeoffEnd, -Span / TimeLeft, 0.1f);
		ActionSlot = DinoflyAnim::LegsDown;
		ActionEndTime = DinoflyAnim::TakeoffLiftoff;
		ActionRate = -Span / TimeLeft;
	}
	bWasLanded = bLanded;

	// Diving: no takeoff or legs down any more (the wings fold procedurally).
	const bool bDiving = !bLanded && Flight->IsDiving();
	if (bDiving && ActionSlot != INDEX_NONE)
	{
		Instance->StopLBALayer(0.1f);
		ActionSlot = INDEX_NONE;
	}

	if (ActionSlot == DinoflyAnim::HurtLayer)
	{
		// Seconds left in ActionEndTime.
		ActionEndTime -= DeltaSeconds;
		if (ActionEndTime <= 0.f || bLanded)
		{
			Instance->StopLBALayer(0.15f);
			ActionSlot = INDEX_NONE;
		}
	}
	else if (ActionSlot == DinoflyAnim::LegsDown)
	{
		// Legs down (a layer over the flight animations): held until the touchdown; climbing again folds them back.
		if (Velocity.Z > 50.f && !IsBrakingLow())
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
			else if (!bRunning && Instance->IsLayerActive())
			{
				// Flying: the procedural wings take over from the takeoff's beats.
				Instance->StopLBALayer(0.2f);
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
		// Flying (diving too): the spread-wings pose of the flapping cycle, held, and the wings and tail moved
		// procedurally over it (UpdateProceduralWings), so every change of state is a smooth one.
		const FLBAEntityAnim* FlapAnim = Entity ? Entity->Anims.Find(DinoflyAnim::Flap) : nullptr;
		if (FlapAnim && FlapAnim->Sequence && (Instance->GetCurrentAnimation() != FlapAnim->Sequence || !bGlideBase))
		{
			Instance->PlayLBAAnimation(FlapAnim->Sequence, true, FlapAnim->LoopStartTime, 0.3f, 0.f, true, DinoflyAnim::GlideTime);
			bGlideBase = true;
		}
		bProceduralWings = true;
		return;
	}
	const float Speed = Flight->GetAirVelocity().Size();
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

bool ALBADinofly::IsBrakingLow() const
{
	return Flight->GetBrakeAmount() > 0.f
		&& Flight->GetHeightAboveSurface(ULBADinoflySettings::Get().BrakeStyleHeights.X + 100.f) < ULBADinoflySettings::Get().BrakeStyleHeights.X;
}

namespace
{
	// Braking styles by height above the ground: reaching forward to land, hovering, a flared parachute.
	const FLBAWingShape BrakeLow = { 3.f, 55.f, 15.f, -25.f, 35.f, 0.f };
	const FLBAWingShape BrakeMid = { 4.2f, 38.f, 8.f, -8.f, 18.f, 0.f };
	const FLBAWingShape BrakeHigh = { 1.4f, 22.f, 14.f, 0.f, 32.f, 2.f };
	/** Spent: weak flaps, the wings drooping. */
	const FLBAWingShape Exhausted = { 1.2f, 12.f, -14.f, 5.f, 0.f, 2.f };
	constexpr float BeatAmplitude = 45.f;
}

void ALBADinofly::UpdateProceduralWings(float DeltaSeconds)
{
	ULBAAnimInstance* Instance = GetLBAAnimInstance();
	if (!Instance)
	{
		return;
	}
	const ULBADinoflySettings& Settings = ULBADinoflySettings::Get();
	WingClock += DeltaSeconds;

	// What the flight asks of the wings.
	FLBAWingShape Target;
	bool bContinuous = false;
	const FVector Air = Flight->GetAirVelocity();
	if (Flight->IsDiving())
	{
		// Folded back, the harder the dive the tighter; the air buffets them. Pulling out spreads them, flared.
		const float Aggression = Flight->IsPullingOut() ? 0.f : Flight->GetDiveAggression();
		Target = Flight->IsPullingOut() ? FLBAWingShape{ 0.f, 0.f, 5.f, 15.f, 15.f, 3.f }
			: FLBAWingShape{ 0.f, 0.f, FMath::Lerp(-4.f, -14.f, Aggression), FMath::Lerp(35.f, 75.f, Aggression), 0.f, FMath::Lerp(2.f, 6.f, Aggression) };
	}
	else
	{
		// Gliding, breathing a little; stalling (a fast fall) pushes the wings up and makes them flutter: danger.
		const float Fall = FMath::Max(-Air.Z, 0.f);
		const float Danger = FMath::Max(Flight->GetStall(), FMath::Clamp((Fall - 300.f) / 1000.f, 0.f, 1.f));
		Target = { 0.f, 0.f, 4.f + 28.f * Danger, 8.f * Danger, 0.f, 6.f * Danger };
		if (Flight->IsBrakeExhausted())
		{
			Target = Exhausted;
			bContinuous = true;
		}
		else if (Flight->GetBrakeAmount() > 0.f)
		{
			const float Height = Flight->GetHeightAboveSurface(Settings.BrakeStyleHeights.Y * 1.3f);
			const FVector2D& Heights = Settings.BrakeStyleHeights;
			const float Low = 1.f - FMath::SmoothStep(float(Heights.X) * 0.75f, float(Heights.X) * 1.25f, Height);
			const float High = FMath::SmoothStep(float(Heights.Y) * 0.8f, float(Heights.Y) * 1.2f, Height);
			const FLBAWingShape Style = BrakeMid.Lerp(BrakeLow, Low).Lerp(BrakeHigh, High);
			Target = Target.Lerp(Style, Flight->GetBrakeAmount());
			bContinuous = true;
		}
	}

	// Eased toward it: every change is smooth.
	const float Ease = 1.f - FMath::Exp(-5.f * DeltaSeconds);
	Wings = Wings.Lerp(Target, Ease);
	WingWeight = FMath::FInterpTo(WingWeight, bProceduralWings ? 1.f : 0.f, DeltaSeconds, 8.f);
	TailYaw = FMath::FInterpTo(TailYaw, -25.f * FMath::Clamp(Flight->GetTurnRate() / FMath::Max(Flight->HoverTurnRate, 1.f), -1.f, 1.f), DeltaSeconds, 4.f);

	// Beats: one full stroke per beat (down first, the power stroke), queued; continuous flapping while braking.
	if (Flight->GetBeatCount() != LastBeatCount)
	{
		LastBeatCount = Flight->GetBeatCount();
		WingCyclesLeft = FMath::Min(WingCyclesLeft + 1, 2);
		BeatKick = 1.f;
	}
	BeatKick = FMath::FInterpTo(BeatKick, 0.f, DeltaSeconds, 3.f);
	const bool bBeating = WingCyclesLeft > 0;
	if (bContinuous || bBeating || WingPhase > 0.f)
	{
		const float Frequency = bBeating ? FMath::Max(Settings.BeatFrequency, Wings.Frequency) : Wings.Frequency;
		WingPhase += Frequency * DeltaSeconds;
		if (WingPhase >= 1.f)
		{
			WingPhase -= 1.f;
			WingCyclesLeft = FMath::Max(WingCyclesLeft - 1, 0);
			if (!bContinuous && WingCyclesLeft == 0)
			{
				// Back at rest, spread.
				WingPhase = 0.f;
			}
		}
	}
	const float Amplitude = FMath::Max(Wings.Amplitude + 15.f * BeatKick * (bContinuous ? 1.f : 0.f), bBeating ? BeatAmplitude : 0.f);
	const float Stroke = -Amplitude * FMath::Sin(2.f * UE_PI * WingPhase);
	const float Sway = 2.f * FMath::Sin(2.f * UE_PI * 0.35f * WingClock);
	const float Flutter = Wings.Flutter * FMath::Sin(2.f * UE_PI * 9.f * WingClock);

	// Bone rotations in the mesh's component space (+Y forward, +Z up; the right wing, bone_16-18, toward +X): about
	// each joint, raising (about the body's long axis), sweeping back (about the vertical) and flaring (about the
	// lateral axis, leading edge up). The elbow and the tip follow the shoulder's stroke, smaller.
	FLBAProceduralPose Pose;
	Pose.Weight = WingWeight;
	auto Wing = [&](const TCHAR* Shoulder, const TCHAR* Elbow, const TCHAR* Tip, float Side, float Phase)
	{
		auto Raise = [Side](float Degrees) { return FQuat(FVector(0.f, -Side, 0.f), FMath::DegreesToRadians(Degrees)); };
		const FQuat Back(FVector(0.f, 0.f, -Side), FMath::DegreesToRadians(Wings.Sweep));
		const FQuat Flare(FVector(1.f, 0.f, 0.f), FMath::DegreesToRadians(Wings.Flare));
		Pose.BoneRotations.Emplace(Shoulder, Back * Raise(Wings.Dihedral + Stroke + Sway * Phase) * Flare);
		Pose.BoneRotations.Emplace(Elbow, FQuat(FVector(0.f, 0.f, -Side), FMath::DegreesToRadians(0.6f * Wings.Sweep)) * Raise(0.45f * Stroke + Flutter));
		Pose.BoneRotations.Emplace(Tip, Raise(0.3f * Stroke + 1.5f * Flutter));
	};
	Wing(TEXT("bone_16"), TEXT("bone_17"), TEXT("bone_18"), 1.f, 1.f);
	Wing(TEXT("bone_19"), TEXT("bone_20"), TEXT("bone_21"), -1.f, -1.f);
	// The tail steers the turns.
	Pose.BoneRotations.Emplace(TEXT("bone_22"), FQuat(FVector::UpVector, FMath::DegreesToRadians(TailYaw)));
	Instance->SetProceduralPose(Pose);
}

void ALBADinofly::UpdateCamera(float DeltaSeconds)
{
	// Raised and looking down toward the Dinofly and the ground when descending or high above the ground.
	const bool bHovering = IsHovering();
	const FVector Velocity = Flight->Velocity;
	const float HeightAlpha = Flight->IsLanded() ? 0.f
		: FMath::Clamp((Flight->GetHeightAboveSurface(HighAltitude + 500.f) - 400.f) / (HighAltitude - 400.f), 0.f, 1.f);
	const float DescendSpeed = ULBADinoflySettings::Get().GroundPainSpeed;
	const float DescendAlpha = bHovering ? FMath::Clamp(-Velocity.Z / DescendSpeed, 0.f, 1.f) : 0.f;
	CameraDrop = FMath::FInterpTo(CameraDrop, FMath::Clamp(0.65f * HeightAlpha + 0.65f * DescendAlpha, 0.f, 1.f), DeltaSeconds, 1.5f);
	// Tilting the boom down raises the camera along its arc, looking down at the Dinofly; bring the Dinofly a bit
	// toward the center of the view meanwhile, so it stays framed with the ground around it.
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 120.f - 60.f * CameraDrop);

	// Behind the Dinofly, swinging after it with some lag; never banked. Gliding also follows the climb or dive.
	const float Yaw = !bHovering && Flight->IsLanded() ? GetActorRotation().Yaw : Flight->GetHeading().Rotation().Yaw;
	const float PathPitch = (bHovering && !Flight->IsDiving()) || Flight->IsLanded() ? 0.f : Flight->GetFlightPathAngle();
	const FRotator Target(FMath::Clamp(-15.f - MaxCameraDrop * CameraDrop + 0.4f * PathPitch, -60.f, 20.f), Yaw, 0.f);
	CameraBase = FMath::RInterpTo(CameraBase, Target, DeltaSeconds, bHovering ? CameraFollowSpeed : 2.5f);

	// The look offset stays where the player left it, turning with the Dinofly.
	CameraBoom->SetWorldRotation(FRotator(FMath::Clamp(CameraBase.Pitch + LookOffset.Y, -80.f, 60.f), CameraBase.Yaw + LookOffset.X, 0.f));

	// Field of view: wider with speed, accelerations, climbing or descending, and height.
	const float HorizontalSpeed = Velocity.Size2D();
	if (DeltaSeconds > 0.f)
	{
		SmoothedAcceleration = FMath::FInterpTo(SmoothedAcceleration, (HorizontalSpeed - LastHorizontalSpeed) / DeltaSeconds, DeltaSeconds, 6.f);
	}
	LastHorizontalSpeed = HorizontalSpeed;
	const float SpeedAlpha = bHovering
		? FMath::Clamp((Velocity.Size() - Flight->GetCruiseSpeed()) / (0.8f * Flight->DiveTerminalSpeed - Flight->GetCruiseSpeed()), 0.f, 1.f)
		: FMath::Clamp((Velocity.Size() - Flight->GetTrimSpeed()) / 1500.f, 0.f, 1.f);
	const float AccelerationAlpha = FMath::Clamp(SmoothedAcceleration / 1500.f, 0.f, 1.f);
	const float VerticalAlpha = bHovering ? FMath::Clamp(FMath::Abs(Velocity.Z) / DescendSpeed, 0.f, 1.f) : 0.f;
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

	auto MakeButton = [this](const TCHAR* Name, const TArray<FKey>& Keys)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = EInputActionValueType::Boolean;
		for (const FKey& Key : Keys)
		{
			InputContext->MapKey(Action, Key);
		}
		return Action;
	};
	AscendAction = MakeButton(TEXT("IA_LBA_Dinofly_Ascend"), LBAControls::DinoflyClimb());
	DescendAction = MakeButton(TEXT("IA_LBA_Dinofly_Descend"), LBAControls::DinoflyBrake());
	DiveAction = MakeButton(TEXT("IA_LBA_Dinofly_Dive"), LBAControls::DinoflyDive());
	ModeAction = MakeButton(TEXT("IA_LBA_Dinofly_Mode"), LBAControls::DinoflyMode());
	// The key that summoned it.
	DismountAction = MakeButton(TEXT("IA_LBA_Dinofly_Dismount"), LBAControls::Dinofly());
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
	Input->BindAction(DiveAction, ETriggerEvent::Started, this, &ALBADinofly::OnDive);
	Input->BindAction(DiveAction, ETriggerEvent::Completed, this, &ALBADinofly::OnDiveReleased);
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
	// All the way around, as many times as wanted.
	LookOffset.X = FRotator::NormalizeAxis(LookOffset.X + Look.X);
	LookOffset.Y = FMath::Clamp(LookOffset.Y + Look.Y, -60.f, 60.f);
}
