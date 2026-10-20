#include "LBA2SnapshotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/PlatformMisc.h"
#include "LBA2Runtime.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "LBAAnimInstance.h"
#include "LBACharacter.h"
#include "LBADinofly.h"
#include "LBAFlightMovementComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

bool ULBA2SnapshotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	FString Value;
	return FParse::Value(FCommandLine::Get(), TEXT("LBA2Shots="), Value);
}

void ULBA2SnapshotSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	FString Spec;
	FParse::Value(FCommandLine::Get(), TEXT("LBA2Shots="), Spec, false);
	if (!FParse::Value(FCommandLine::Get(), TEXT("LBA2ShotDir="), ShotDirectory))
	{
		ShotDirectory = FPaths::ProjectSavedDir() / TEXT("LBA2Shots");
	}

	if (Spec.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		bAuto = true;
	}
	else if (Spec.Equals(TEXT("hero"), ESearchCase::IgnoreCase))
	{
		bHero = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("modern"), ESearchCase::IgnoreCase))
	{
		bModern = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("dinofly"), ESearchCase::IgnoreCase))
	{
		bDinofly = true;
		bPosesReady = true;
	}
	else
	{
		TArray<FString> Entries;
		Spec.ParseIntoArray(Entries, TEXT(";"));
		for (const FString& Entry : Entries)
		{
			TArray<FString> Parts;
			Entry.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() >= 5)
			{
				Poses.Add({ FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2])),
					FRotator(FCString::Atod(*Parts[3]), FCString::Atod(*Parts[4]), 0.0) });
			}
		}
		bPosesReady = true;
	}

	FString HeroSpec;
	if (FParse::Value(FCommandLine::Get(), TEXT("LBA2HeroAt="), HeroSpec, false))
	{
		TArray<FString> Parts;
		HeroSpec.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 3)
		{
			HeroAt = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
		}
	}

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &ULBA2SnapshotSubsystem::Tick));
}

void ULBA2SnapshotSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Super::Deinitialize();
}

void ULBA2SnapshotSubsystem::BuildAutoPoses(UWorld* World)
{
	FBox Bounds(ForceInit);
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		const UStaticMeshComponent* Component = It->GetStaticMeshComponent();
		const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		if (Mesh && Mesh->GetName().StartsWith(TEXT("SM_")))
		{
			Bounds += Component->Bounds.GetBox();
		}
	}
	if (!Bounds.IsValid)
	{
		Bounds = FBox(FVector(-1000), FVector(1000));
	}

	const FVector Center = Bounds.GetCenter();
	const double Radius = FMath::Max(Bounds.GetExtent().Size2D(), 500.0);
	auto LookAt = [](const FVector& From, const FVector& To) { return (To - From).Rotation(); };

	// Wide views from four diagonals, then a close view near the player start.
	for (int32 Corner = 0; Corner < 4; ++Corner)
	{
		const double Angle = FMath::DegreesToRadians(45.0 + 90.0 * Corner);
		const FVector From = Center + FVector(FMath::Cos(Angle) * Radius * 0.9, FMath::Sin(Angle) * Radius * 0.9, Radius * 0.55 + Bounds.GetExtent().Z);
		Poses.Add({ From, LookAt(From, Center) });
	}
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		const FVector Target = It->GetActorLocation();
		const FVector From = Target + FVector(-900.0, -900.0, 600.0);
		Poses.Add({ From, LookAt(From, Target) });
		break;
	}
	bPosesReady = true;
}

bool ULBA2SnapshotSubsystem::TickHero(UWorld* World, float DeltaTime)
{
	struct FStep
	{
		const TCHAR* Name;
		float Duration;
		FVector2D Input;
		int32 Behaviour;
		bool bAction;
	};
	static const FStep Steps[] =
	{
		{ TEXT("idle"), 1.0f, { 0, 0 }, 0, false },
		{ TEXT("walk"), 2.0f, { 0, 1 }, -1, false },
		{ TEXT("turn_right_in_place"), 1.5f, { 1, 0 }, -1, false },
		{ TEXT("steer_left_walking"), 2.0f, { -1, 1 }, -1, false },
		{ TEXT("walk_back"), 1.5f, { 0, -1 }, -1, false },
		{ TEXT("athletic_run"), 2.0f, { 0, 1 }, 1, false },
		{ TEXT("athletic_jump"), 0.5f, { 0, 1 }, -1, true },
		{ TEXT("aggressive_attack"), 0.6f, { 0, 0 }, 2, true },
		{ TEXT("discreet_walk"), 2.0f, { 0, 1 }, 3, false },
	};

	APlayerController* PC = World->GetFirstPlayerController();
	ALBACharacter* Hero = PC ? Cast<ALBACharacter>(PC->GetPawn()) : nullptr;
	if (!Hero)
	{
		if (Elapsed > 15.0)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots hero: the player pawn is not an ALBACharacter (use ?game=/Script/LBA2Runtime.LBAGameMode)"));
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}

	auto Report = [&](const TCHAR* Label)
	{
		const UAnimSequence* Anim = Hero->GetLBAAnimInstance() ? Hero->GetLBAAnimInstance()->GetCurrentAnimation() : nullptr;
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots hero %s: location %s yaw %.1f speed %.1f anim %s falling %d"), Label,
			*Hero->GetActorLocation().ToString(), Hero->GetActorRotation().Yaw, Hero->GetVelocity().Size2D(),
			Anim ? *Anim->GetName() : TEXT("none"), Hero->GetCharacterMovement()->IsFalling() ? 1 : 0);
	};

	const int32 NumSteps = UE_ARRAY_COUNT(Steps);
	if (HeroStep >= NumSteps)
	{
		// Give the last screenshot time to be written.
		if (Elapsed - HeroStepStart > 1.0)
		{
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}

	const bool bStepDone = HeroStep >= 0 && Elapsed - HeroStepStart >= Steps[HeroStep].Duration;
	if (HeroStep < 0 || bStepDone)
	{
		if (HeroStep >= 0)
		{
			Report(Steps[HeroStep].Name);
			const FString File = ShotDirectory / FString::Printf(TEXT("Hero_%02d_%s.png"), HeroStep, Steps[HeroStep].Name);
			FScreenshotRequest::RequestScreenshot(File, false, false);
		}
		else
		{
			// Only the script drives the hero during the test.
			Hero->DisableInput(PC);
			Hero->SetControlMode(ELBAControlMode::Authentic);
			Report(TEXT("start"));
		}
		++HeroStep;
		HeroStepStart = Elapsed;
		if (HeroStep >= NumSteps)
		{
			Hero->SetMoveInput(FVector2D::ZeroVector);
			return true;
		}
		const FStep& Step = Steps[HeroStep];
		if (Step.Behaviour >= 0)
		{
			Hero->SetBehaviour(ELBABehaviour(Step.Behaviour));
		}
		Hero->SetMoveInput(Step.Input);
		if (Step.bAction)
		{
			Hero->DoBehaviourAction();
		}
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickModern(UWorld* World, float DeltaTime)
{
	struct FStep
	{
		const TCHAR* Name;
		float Duration;
		/** Stick, relative to Twinsen's starting heading (X right, Y forward). */
		FVector2D Input;
		/** Camera yaw relative to the starting heading (90 = side view of a forward run). */
		float CameraYaw;
		bool bJump;
		/** Seconds between shots during the step (0: one shot at its end). */
		float Burst;
	};
	static const FStep Steps[] =
	{
		{ TEXT("idle"), 1.5f, { 0, 0 }, 90, false, 0 },
		{ TEXT("run_start"), 0.6f, { 0, 1 }, 90, false, 0.05f },
		{ TEXT("run"), 1.2f, { 0, 1 }, 90, false, 0.1f },
		{ TEXT("stop"), 0.8f, { 0, 0 }, 90, false, 0.1f },
		{ TEXT("walk_half_tilt"), 2.0f, { 0, -0.45f }, 90, false, 0.2f },
		{ TEXT("speed_up"), 1.0f, { 0, -1 }, 90, false, 0.1f },
		{ TEXT("turn_right"), 1.0f, { -1, 0 }, 180, false, 0.1f },
		{ TEXT("reverse"), 1.2f, { 1, 0 }, 180, false, 0.1f },
		{ TEXT("stop_again"), 1.0f, { 0, 0 }, 90, false, 0 },
		{ TEXT("standing_jump"), 1.4f, { 0, 0 }, 90, true, 0.1f },
		{ TEXT("run_up"), 1.0f, { 0, 1 }, 90, false, 0 },
		{ TEXT("running_jump"), 1.4f, { 0, 1 }, 90, true, 0.1f },
		{ TEXT("run_on"), 0.6f, { 0, 1 }, 90, false, 0 },
		{ TEXT("stop_end"), 1.0f, { 0, 0 }, 90, false, 0 },
		{ TEXT("look_around"), 1.5f, { 0, 0 }, 140, false, 0 },
	};
	const int32 NumSteps = UE_ARRAY_COUNT(Steps);

	APlayerController* PC = World->GetFirstPlayerController();
	ALBACharacter* Hero = PC ? Cast<ALBACharacter>(PC->GetPawn()) : nullptr;
	if (!Hero)
	{
		if (Elapsed > 15.0)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots modern: the player pawn is not an ALBACharacter"));
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}
	if (HeroStep < 0)
	{
		Hero->DisableInput(PC);
		Hero->SetControlMode(ELBAControlMode::Modern);
		if (HeroAt)
		{
			FVector Location(HeroAt->X, HeroAt->Y, 100000.0);
			FHitResult Hit;
			FCollisionQueryParams Params(TEXT("LBA2HeroAt"), false, Hero);
			if (World->LineTraceSingleByChannel(Hit, Location, FVector(HeroAt->X, HeroAt->Y, -100000.0), ECC_Visibility, Params))
			{
				Location.Z = Hit.Location.Z + Hero->GetSimpleCollisionHalfHeight() + 2.0;
			}
			Hero->TeleportTo(Location, FRotator(0.0, HeroAt->Z, 0.0), false, true);
		}
		// Closer than in play, to see the legs.
		Hero->GetCameraBoom()->TargetArmLength = 260.f;
		Hero->GetCameraBoom()->bEnableCameraLag = false;
		ModernBaseYaw = Hero->GetActorRotation().Yaw;
		HeroStep = 0;
		HeroStepStart = Elapsed;
		NextReport = Elapsed;
		NextBurst = Elapsed;
		BurstIndex = 0;
		if (Steps[0].bJump)
		{
			Hero->ModernJump();
		}
	}
	if (HeroStep >= NumSteps)
	{
		if (Elapsed - HeroStepStart > 1.0)
		{
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}

	const FStep& Step = Steps[HeroStep];
	// The stick is camera-relative: turn the heading-relative input into the camera's frame.
	const float CameraYaw = ModernBaseYaw + Step.CameraYaw;
	PC->SetControlRotation(FRotator(-10.f, CameraYaw, 0.f));
	const float Angle = FMath::Atan2(Step.Input.X, Step.Input.Y) - FMath::DegreesToRadians(Step.CameraYaw);
	const float Magnitude = Step.Input.Size();
	Hero->SetMoveInput(FVector2D(FMath::Sin(Angle), FMath::Cos(Angle)) * Magnitude);

	if (Elapsed >= NextReport)
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots modern %s t=%.2f loc=(%.0f %.0f %.0f) yaw=%.0f %s"), Step.Name, Elapsed - HeroStepStart,
			Hero->GetActorLocation().X, Hero->GetActorLocation().Y, Hero->GetActorLocation().Z, Hero->GetActorRotation().Yaw, *Hero->GetModernDebugString());
		NextReport = Elapsed + 0.05;
	}
	if (Step.Burst > 0.f && Elapsed >= NextBurst)
	{
		const FString File = ShotDirectory / FString::Printf(TEXT("Modern_%02d_%s_%02d.png"), HeroStep, Step.Name, BurstIndex++);
		FScreenshotRequest::RequestScreenshot(File, false, false);
		NextBurst = Elapsed + Step.Burst;
	}

	if (Elapsed - HeroStepStart >= Step.Duration)
	{
		if (Step.Burst <= 0.f)
		{
			const FString File = ShotDirectory / FString::Printf(TEXT("Modern_%02d_%s.png"), HeroStep, Step.Name);
			FScreenshotRequest::RequestScreenshot(File, false, false);
		}
		++HeroStep;
		HeroStepStart = Elapsed;
		NextBurst = Elapsed;
		BurstIndex = 0;
		if (HeroStep < NumSteps && Steps[HeroStep].bJump)
		{
			Hero->ModernJump();
		}
		if (HeroStep >= NumSteps)
		{
			Hero->SetMoveInput(FVector2D::ZeroVector);
		}
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickDinofly(UWorld* World, float DeltaTime)
{
	struct FStep
	{
		const TCHAR* Name;
		float Duration;
		/** X turn right, Y forward. */
		FVector2D Move;
		float Vertical;
		/** Holds the dive. */
		bool bDive;
		/** Ends early once landed. */
		bool bUntilLanded;
	};
	static const FStep Steps[] =
	{
		{ TEXT("mount_on_ground"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("walk_forward"), 2.5f, { 0, 1 }, 0, false, false },
		{ TEXT("walk_turn_right"), 1.5f, { 1, 1 }, 0, false, false },
		{ TEXT("dive_on_ground_ignored"), 1.0f, { 0, 0 }, 0, true, false },
		{ TEXT("takeoff"), 2.5f, { 0, 0 }, 1, false, false },
		{ TEXT("forward"), 2.0f, { 0, 1 }, 0, false, false },
		{ TEXT("climb_high"), 8.0f, { 0, 0.3f }, 1, false, false },
		{ TEXT("fast_high"), 5.0f, { 0, 1 }, 0, false, false },
		{ TEXT("dive_spiral"), 1.8f, { 1, 0 }, 0, true, false },
		{ TEXT("pull_out"), 2.5f, { 0, 1 }, 0, false, false },
		{ TEXT("climb_again"), 5.0f, { 0, 0.5f }, 1, false, false },
		{ TEXT("dive_steep"), 1.6f, { 0, 1 }, 0, true, false },
		{ TEXT("zoom_climb"), 4.0f, { 0, 0 }, 1, false, false },
		{ TEXT("coast"), 3.0f, { 0, 0 }, 0, false, false },
		{ TEXT("stop"), 2.0f, { 0, 0 }, 0, false, false },
		{ TEXT("descend_land"), 15.0f, { 0, 0 }, -1, false, true },
		{ TEXT("landed"), 1.0f, { 0, 0 }, 0, false, false },
		{ TEXT("walk_after_landing"), 2.0f, { 0, 1 }, 0, false, false },
		{ TEXT("takeoff_again"), 2.5f, { 0, 0 }, 1, false, false },
		{ TEXT("turn_around"), 1.75f, { 1, 0 }, 0, false, false },
		{ TEXT("return_over_land"), 3.0f, { 0, 1 }, 0, false, false },
		{ TEXT("turn_left"), 1.0f, { -1, 0.3f }, 0, false, false },
		{ TEXT("forward_over_land"), 2.0f, { 0, 1 }, 0, false, false },
		{ TEXT("stop_over_land"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("land_on_ground"), 15.0f, { 0, 0 }, -1, false, true },
		{ TEXT("landed_again"), 1.0f, { 0, 0 }, 0, false, false },
		{ TEXT("dismount"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("on_foot"), 1.5f, { 0, 0 }, 0, false, false },
	};
	const int32 NumSteps = UE_ARRAY_COUNT(Steps);

	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (HeroStep < 0)
	{
		ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
		if (!Hero)
		{
			if (Elapsed > 15.0)
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots dinofly: the player pawn is not an ALBACharacter"));
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
			return true;
		}
		if (!Hero->SummonDinofly())
		{
			return true;
		}
		HeroStep = 0;
		HeroStepStart = Elapsed;
		NextReport = Elapsed;
		Pawn = PC->GetPawn();
	}
	if (HeroStep >= NumSteps)
	{
		if (Elapsed - HeroStepStart > 1.0)
		{
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}

	const FStep& Step = Steps[HeroStep];
	ALBADinofly* Dinofly = Cast<ALBADinofly>(Pawn);
	if (Dinofly)
	{
		Dinofly->DisableInput(PC);
		const bool bWaitingToLand = Step.bUntilLanded && !Dinofly->GetFlight()->IsLanded();
		Dinofly->SetHoverInput(Step.Move, Step.Vertical);
		Dinofly->SetDiveInput(Step.bDive);
		if (Elapsed >= NextReport)
		{
			const ULBAFlightMovementComponent* Flight = Dinofly->GetFlight();
			const UAnimInstance* Anim = Dinofly->FindComponentByClass<USkeletalMeshComponent>()->GetAnimInstance();
			const ULBAAnimInstance* LBAAnim = Cast<ULBAAnimInstance>(Anim);
			const FRotator Rotation = Dinofly->GetActorRotation();
			const UCameraComponent* ViewCamera = Dinofly->FindComponentByClass<UCameraComponent>();
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots dinofly %s t=%.2f loc=(%.0f %.0f %.0f) vel=(%.0f %.0f %.0f) speed2d=%.0f cruise=%.0f dive=%d height=%.0f pitch=%.1f yaw=%.0f roll=%.1f cam(pitch=%.1f fov=%.1f) landed=%d floating=%d takingoff=%d anim=%s@%.2f layer=%d@%.2f"),
				Step.Name, Elapsed - HeroStepStart, Dinofly->GetActorLocation().X, Dinofly->GetActorLocation().Y, Dinofly->GetActorLocation().Z,
				Flight->Velocity.X, Flight->Velocity.Y, Flight->Velocity.Z, Flight->Velocity.Size2D(), Flight->GetCruiseSpeed(), Flight->IsDiving() ? (Flight->IsPullingOut() ? 2 : 1) : 0, Flight->GetHeightAboveSurface(10000.f),
				Rotation.Pitch, Rotation.Yaw, Rotation.Roll,
				ViewCamera ? ViewCamera->GetComponentRotation().Pitch : 0.f, ViewCamera ? ViewCamera->FieldOfView : 0.f,
				Flight->IsLanded() ? 1 : 0, Flight->IsFloating() ? 1 : 0, Flight->IsTakingOff() ? 1 : 0, LBAAnim && LBAAnim->GetCurrentAnimation() ? *LBAAnim->GetCurrentAnimation()->GetName() : TEXT("none"), LBAAnim ? LBAAnim->GetCurrentAnimationTime() : 0.f,
				LBAAnim && LBAAnim->IsLayerActive() ? 1 : 0, LBAAnim ? LBAAnim->GetLayerTime() : 0.f);
			NextReport = Elapsed + 0.25;
		}
		// Landing steps end once landed (or time out).
		if (Step.bUntilLanded && !bWaitingToLand && Elapsed - HeroStepStart < Step.Duration)
		{
			HeroStepStart = Elapsed - Step.Duration;
		}
	}

	if (Elapsed - HeroStepStart >= Step.Duration)
	{
		const FString File = ShotDirectory / FString::Printf(TEXT("Dinofly_%02d_%s.png"), HeroStep, Step.Name);
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots dinofly step %s done, pawn %s"), Step.Name, Pawn ? *Pawn->GetName() : TEXT("none"));
		if (Dinofly && FCString::Strcmp(Step.Name, TEXT("dismount")) == 0)
		{
			const bool bOff = Dinofly->Dismount();
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots dinofly dismount %s, pawn now %s"), bOff ? TEXT("ok") : TEXT("refused"), PC->GetPawn() ? *PC->GetPawn()->GetName() : TEXT("none"));
		}
		++HeroStep;
		HeroStepStart = Elapsed;
	}
	return true;
}

bool ULBA2SnapshotSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || !World->HasBegunPlay())
	{
		return true;
	}
	Elapsed += DeltaTime;
	if (Elapsed < 4.0)
	{
		return true;
	}
	if (bHero)
	{
		return TickHero(World, DeltaTime);
	}
	if (bModern)
	{
		return TickModern(World, DeltaTime);
	}
	if (bDinofly)
	{
		return TickDinofly(World, DeltaTime);
	}
	if (!bPosesReady)
	{
		BuildAutoPoses(World);
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return true;
	}

	if (!Camera.IsValid())
	{
		ACameraActor* NewCamera = World->SpawnActor<ACameraActor>();
		NewCamera->GetCameraComponent()->SetFieldOfView(70.f);
		NewCamera->GetCameraComponent()->bConstrainAspectRatio = false;
		Camera = NewCamera;
		PoseStart = -1.0;

		APawn* Pawn = PC->GetPawn();
		if (HeroAt && Pawn)
		{
			// Stand the pawn on the highest surface below the point.
			FVector Location(HeroAt->X, HeroAt->Y, 100000.0);
			FHitResult Hit;
			FCollisionQueryParams Params(TEXT("LBA2HeroAt"), false, Pawn);
			if (World->LineTraceSingleByChannel(Hit, Location, FVector(HeroAt->X, HeroAt->Y, -100000.0), ECC_Visibility, Params))
			{
				Location.Z = Hit.Location.Z + Pawn->GetSimpleCollisionHalfHeight() + 2.0;
			}
			Pawn->DisableInput(PC);
			Pawn->TeleportTo(Location, FRotator(0.0, HeroAt->Z, 0.0), false, true);
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots: hero at %s"), *Pawn->GetActorLocation().ToString());
		}
	}

	if (NextPose >= Poses.Num())
	{
		if (Elapsed - PoseStart > 1.5)
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots: done"));
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}

	if (PoseStart < 0.0)
	{
		Camera->SetActorLocationAndRotation(Poses[NextPose].Location, Poses[NextPose].Rotation);
		PC->SetViewTarget(Camera.Get());
		PoseStart = Elapsed;
	}
	else if (Elapsed - PoseStart > 2.5)
	{
		const FString File = ShotDirectory / FString::Printf(TEXT("%s_%02d.png"), *World->GetMapName(), NextPose);
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots: %s at %s %s"), *File, *Poses[NextPose].Location.ToString(), *Poses[NextPose].Rotation.ToString());
		++NextPose;
		PoseStart = NextPose < Poses.Num() ? -1.0 : Elapsed;
	}
	return true;
}
