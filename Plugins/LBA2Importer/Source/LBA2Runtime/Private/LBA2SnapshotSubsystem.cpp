#include "LBA2SnapshotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "LBA2Runtime.h"
#include "Animation/AnimSequence.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "LBAAnimInstance.h"
#include "LBABoat.h"
#include "LBACharacter.h"
#include "LBADeathScreen.h"
#include "LBADinofly.h"
#include "LBAHitPointsComponent.h"
#include "LBAIslandMenu.h"
#include "LBAPlanetSubsystem.h"
#include "LBAPlayerController.h"
#include "LBAFlightMovementComponent.h"
#include "LBASceneActor.h"
#include "LBASceneAsset.h"
#include "LBASceneTravelSubsystem.h"
#include "LBATravelGateway.h"
#include "LBAWindSubsystem.h"
#include "LBAZoneVolume.h"
#include "Misc/App.h"
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
	else if (Spec.Equals(TEXT("camera"), ESearchCase::IgnoreCase))
	{
		bCamera = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("dinofly"), ESearchCase::IgnoreCase))
	{
		bDinofly = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("globe"), ESearchCase::IgnoreCase))
	{
		bGlobe = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("boat"), ESearchCase::IgnoreCase))
	{
		bBoat = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("belt"), ESearchCase::IgnoreCase))
	{
		bBelt = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("wind"), ESearchCase::IgnoreCase))
	{
		bWind = true;
		bPosesReady = true;
	}
	else if (Spec.Equals(TEXT("ladder"), ESearchCase::IgnoreCase))
	{
		bLadder = true;
		bPosesReady = true;
		FString Ladder;
		if (FParse::Value(FCommandLine::Get(), TEXT("LBA2Ladder="), Ladder, false))
		{
			FString Scene, Zone;
			if (Ladder.Split(TEXT(":"), &Scene, &Zone))
			{
				LadderScene = FCString::Atoi(*Scene);
				LadderZone = FCString::Atoi(*Zone);
			}
		}
	}
	else if (Spec.Equals(TEXT("door"), ESearchCase::IgnoreCase))
	{
		bDoor = true;
		bPosesReady = true;
		FString Door, Scene, Object;
		if (FParse::Value(FCommandLine::Get(), TEXT("LBA2Door="), Door, false) && Door.Split(TEXT(":"), &Scene, &Object))
		{
			DoorScene = FCString::Atoi(*Scene);
			DoorObject = FCString::Atoi(*Object);
		}
	}
	else if (Spec.Equals(TEXT("attack"), ESearchCase::IgnoreCase))
	{
		bAttack = true;
		bPosesReady = true;
		FString Target, Scene, Object;
		if (FParse::Value(FCommandLine::Get(), TEXT("LBA2Attack="), Target, false) && Target.Split(TEXT(":"), &Scene, &Object))
		{
			AttackScene = FCString::Atoi(*Scene);
			AttackObject = FCString::Atoi(*Object);
		}
	}
	else if (Spec.Equals(TEXT("menu"), ESearchCase::IgnoreCase))
	{
		bMenu = true;
		bPosesReady = true;
		FString Scenes;
		if (FParse::Value(FCommandLine::Get(), TEXT("LBA2MenuScenes="), Scenes, false))
		{
			TArray<FString> Parts;
			Scenes.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				MenuScenes.Add(FCString::Atoi(*Part));
			}
		}
	}
	else if (Spec.Equals(TEXT("scenes"), ESearchCase::IgnoreCase))
	{
		bScenes = true;
		bPosesReady = true;
		FString Doors;
		if (FParse::Value(FCommandLine::Get(), TEXT("LBA2Doors="), Doors, false))
		{
			TArray<FString> Parts;
			Doors.ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				SceneDoors.Add(FCString::Atoi(*Part));
			}
		}
		FParse::Value(FCommandLine::Get(), TEXT("LBA2SceneEnd="), SceneEnd);
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
					FRotator(FCString::Atod(*Parts[3]), FCString::Atod(*Parts[4]), 0.0), Parts.IsValidIndex(5) ? Parts[5] : FString() });
			}
		}
		bPosesReady = true;
	}
	FParse::Value(FCommandLine::Get(), TEXT("LBA2ShotSettle="), PoseSettle);

	// Faster than real time: every frame advances the game (and these steps' clock) by a fixed 1/N s, as fast as frames
	// render. The Dinofly's steps are all in game time (its flight substeps at 120 Hz), so it runs this way by default,
	// at 1/15 s: its run takes 45 s instead of 150, and lands within a few meters of a real-time run (2026-10-31). So do the
	// belt and globe flights: 34 s instead of 210, 33 s instead of 162, within about 1 m at each step's end, but their
	// landings end about 5 s sooner and later shots are framed differently (2026-11-07).
	// -LBA2ShotStep=0 plays in real time: frame rates and camera jerk mean nothing otherwise.
	int32 StepRate = bDinofly || bBelt || bGlobe ? 15 : 0;
	FParse::Value(FCommandLine::Get(), TEXT("LBA2ShotStep="), StepRate);
	if (StepRate > 0)
	{
		FApp::SetUseFixedTimeStep(true);
		FApp::SetFixedDeltaTime(1.0 / StepRate);
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots: fixed time step 1/%d s, as fast as frames render"), StepRate);
	}
	// The game has VSync on by default (DefaultGameUserSettings.ini): runs don't wait for the screen, and frame rates
	// measured in real time aren't capped at its refresh rate.
	if (IConsoleVariable* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync")))
	{
		VSync->Set(0, ECVF_SetByCode);
	}

	FString HeroSpec;
	if (FParse::Value(FCommandLine::Get(), TEXT("LBA2HeroAt="), HeroSpec, false))
	{
		TArray<FString> Parts;
		HeroSpec.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() >= 3)
		{
			HeroAt = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
			HeroFromZ = Parts.Num() >= 4 ? FCString::Atod(*Parts[3]) : HeroFromZ;
		}
	}

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &ULBA2SnapshotSubsystem::Tick));
}

void ULBA2SnapshotSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Super::Deinitialize();
}

void ULBA2SnapshotSubsystem::PlaceHero(UWorld* World, APawn* Pawn) const
{
	if (!HeroAt)
	{
		return;
	}
	FVector Location(HeroAt->X, HeroAt->Y, HeroFromZ);
	FHitResult Hit;
	FCollisionQueryParams Params(TEXT("LBA2HeroAt"), false, Pawn);
	if (World->LineTraceSingleByChannel(Hit, Location, FVector(HeroAt->X, HeroAt->Y, -100000.0), ECC_Visibility, Params))
	{
		Location.Z = Hit.Location.Z + Pawn->GetSimpleCollisionHalfHeight() + 2.0;
	}
	Pawn->TeleportTo(Location, FRotator(0.0, HeroAt->Z, 0.0), false, true);
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
			PlaceHero(World, Hero);
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
		PlaceHero(World, Hero);
		// Closer than in play, to see the legs.
		Hero->ModernCameraDistance = 260.f;
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

bool ULBA2SnapshotSubsystem::TickCamera(UWorld* World, float DeltaTime)
{
	struct FStep
	{
		const TCHAR* Name;
		/** From the pitch the game gives the camera (the first shot is what the player sees on arrival). */
		float Pitch;
		float Yaw;
		/** Runs: the stick (camera-relative, X right, Y forward) held this long (s), the camera left to follow. */
		FVector2D Input = FVector2D::ZeroVector;
		float Duration = 0.f;
	};
	static const FStep Steps[] =
	{
		{ TEXT("ahead"), 0.f, 0.f },
		{ TEXT("right"), 0.f, 90.f },
		{ TEXT("behind"), 0.f, 180.f },
		{ TEXT("left"), 0.f, 270.f },
		{ TEXT("up"), 40.f, 0.f },
		{ TEXT("down"), -35.f, 0.f },
		{ TEXT("run_right"), 20.f, 0.f, { 1, 0 }, 3.f },
		{ TEXT("run_back"), 0.f, 0.f, { 0, -1 }, 1.5f },
	};
	const int32 NumSteps = UE_ARRAY_COUNT(Steps);

	APlayerController* PC = World->GetFirstPlayerController();
	ALBACharacter* Hero = PC ? Cast<ALBACharacter>(PC->GetPawn()) : nullptr;
	if (!Hero)
	{
		if (Elapsed > 15.0)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots camera: the player pawn is not an ALBACharacter"));
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}
	if (HeroStep < 0)
	{
		Hero->DisableInput(PC);
		Hero->SetControlMode(ELBAControlMode::Modern);
		PlaceHero(World, Hero);
		ModernBaseYaw = Hero->GetActorRotation().Yaw;
		HeroStep = 0;
		HeroStepStart = Elapsed;
		PC->SetControlRotation(FRotator(PC->GetControlRotation().Pitch, ModernBaseYaw, 0.f));
		return true;
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

	// Let the camera settle, shoot, then turn on a later frame (the shot is the next rendered frame).
	const FStep& Step = Steps[HeroStep];
	if (Step.Duration > 0.f && !bCameraShotTaken)
	{
		// A run: where the camera turns to, and how close it comes to walls.
		Hero->SetMoveInput(Step.Input);
		if (Elapsed >= NextReport)
		{
			const FVector View = PC->PlayerCameraManager->GetCameraLocation();
			double Clearance = 1000.0;
			for (int32 Index = 0; Index <= 8; ++Index)
			{
				const FVector Direction = Index < 8 ? FRotator(0.f, Index * 45.f, 0.f).Vector() : FVector::UpVector;
				FHitResult Hit;
				if (World->LineTraceSingleByObjectType(Hit, View, View + Direction * 1000.0, FCollisionObjectQueryParams(ECC_WorldStatic),
					FCollisionQueryParams(SCENE_QUERY_STAT(LBA2ShotsClearance), false, Hero)))
				{
					Clearance = FMath::Min(Clearance, Hit.Distance);
				}
			}
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots camera %s t=%.2f: view yaw %.0f (heading %.0f, hero yaw %.0f) pitch %.0f arm %.0f, nearest wall %.0f cm, hero at %s"),
				Step.Name, Elapsed - HeroStepStart, FRotator::NormalizeAxis(PC->GetControlRotation().Yaw - ModernBaseYaw),
				FRotator::NormalizeAxis(Hero->GetVelocity().Rotation().Yaw - ModernBaseYaw), FRotator::NormalizeAxis(Hero->GetActorRotation().Yaw - ModernBaseYaw),
				FRotator::NormalizeAxis(PC->GetControlRotation().Pitch), Hero->GetCameraBoom()->TargetArmLength, Clearance, *Hero->GetActorLocation().ToCompactString());
			NextReport = Elapsed + 0.25;
		}
		if (Elapsed - HeroStepStart >= Step.Duration)
		{
			Hero->SetMoveInput(FVector2D::ZeroVector);
			FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("Camera_%02d_%s.png"), HeroStep, Step.Name), false, false);
			bCameraShotTaken = true;
			HeroStepStart = Elapsed - 1.5;
		}
	}
	else if (!bCameraShotTaken && Elapsed - HeroStepStart >= 1.5)
	{
		if (HeroStep == 0)
		{
			CameraBasePitch = FRotator::NormalizeAxis(PC->GetControlRotation().Pitch);
			// What is overhead: the ceiling the indoor camera stays under.
			FHitResult Hit;
			const FVector Head = Hero->GetActorLocation();
			if (World->LineTraceSingleByObjectType(Hit, Head, Head + FVector(0.0, 0.0, 2000.0), FCollisionObjectQueryParams(ECC_WorldStatic),
				FCollisionQueryParams(SCENE_QUERY_STAT(LBA2ShotsCeiling), true, Hero)))
			{
				UE_LOG(LogLBA2, Display, TEXT("LBA2Shots camera: overhead %s (%s) %.0f cm above the feet"), *GetNameSafe(Hit.GetActor()),
					*GetNameSafe(Hit.GetComponent()), Hit.ImpactPoint.Z - (Head.Z - Hero->GetSimpleCollisionHalfHeight()));
			}
		}
		const FVector View = PC->PlayerCameraManager->GetCameraLocation();
		const double Feet = Hero->GetActorLocation().Z - Hero->GetSimpleCollisionHalfHeight();
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots camera %s: indoors %d pitch %.0f fov %.0f arm %.0f camera %.0f cm above the feet, %.0f cm from the hero at %s yaw %.0f (view yaw %.0f)"), Step.Name,
			Hero->IsIndoors() ? 1 : 0, FRotator::NormalizeAxis(PC->GetControlRotation().Pitch), PC->PlayerCameraManager->GetFOVAngle(),
			Hero->GetCameraBoom()->TargetArmLength, View.Z - Feet, FVector::Dist(View, Hero->GetActorLocation()),
			*Hero->GetActorLocation().ToCompactString(), Hero->GetActorRotation().Yaw, PC->PlayerCameraManager->GetCameraRotation().Yaw);
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("Camera_%02d_%s.png"), HeroStep, Step.Name), false, false);
		bCameraShotTaken = true;
	}
	else if (bCameraShotTaken && Elapsed - HeroStepStart >= 2.0)
	{
		bCameraShotTaken = false;
		++HeroStep;
		HeroStepStart = Elapsed;
		if (HeroStep < NumSteps)
		{
			PC->SetControlRotation(FRotator(CameraBasePitch + Steps[HeroStep].Pitch, ModernBaseYaw + Steps[HeroStep].Yaw, 0.f));
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
		/** Pumps the climb button every this many seconds (0: no; negative: taps the brake instead). */
		float Pump = 0.f;
		/** Hurts the Dinofly this much when the step starts. */
		float Damage = 0.f;
	};
	static const FStep Steps[] =
	{
		{ TEXT("mount_on_ground"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("walk_forward"), 2.5f, { 0, 1 }, 0, false, false },
		{ TEXT("walk_turn_right"), 1.5f, { 1, 1 }, 0, false, false },
		{ TEXT("dive_on_ground_ignored"), 1.0f, { 0, 0 }, 0, true, false },
		{ TEXT("takeoff"), 2.5f, { 0, 0 }, 1, false, false },
		{ TEXT("pump_fast"), 5.0f, { 0, 0 }, 0, false, false, 0.5f },
		{ TEXT("pump_hold"), 6.0f, { 0, 0 }, 0, false, false, 2.5f },
		{ TEXT("pump_climb"), 5.0f, { 0, 0.3f }, 0, false, false, 1.f },
		{ TEXT("climb_high"), 6.0f, { 0, 0 }, 0, false, false, 0.4f },
		{ TEXT("fall_hands_off"), 5.5f, { 0, 0 }, 0, false, false },
		{ TEXT("brake_high"), 1.5f, { 0, 0 }, -1, false, false },
		{ TEXT("pump_again"), 4.0f, { 0, 0 }, 0, false, false, 0.5f },
		{ TEXT("forward_cruise"), 4.0f, { 0, 1 }, 0, false, false, 1.5f },
		{ TEXT("turn_right_cruise"), 2.0f, { 1, 1 }, 0, false, false, 1.5f },
		{ TEXT("dive_short"), 0.5f, { 0, 1 }, 0, true, false },
		{ TEXT("pull_out_short"), 3.0f, { 0, 1 }, 0, false, false },
		{ TEXT("climb_again"), 5.0f, { 0, 0.5f }, 0, false, false, 0.4f },
		{ TEXT("dive_long"), 2.0f, { 0, 1 }, 0, true, false },
		{ TEXT("zoom_climb"), 3.0f, { 0, 0 }, 1, false, false },
		{ TEXT("brake_kill_speed"), 2.0f, { 0, 1 }, -1, false, false },
		{ TEXT("pump_after_brake"), 3.0f, { 0, 0 }, 0, false, false, 1.f },
		{ TEXT("morse_landing"), 20.0f, { 0, 0 }, 0, false, true, -0.45f },
		{ TEXT("landed"), 1.0f, { 0, 0 }, 0, false, false },
		{ TEXT("hurt_standing"), 1.0f, { 0, 0 }, 0, false, false, 0.f, 15.f },
		{ TEXT("takeoff_again"), 2.5f, { 0, 0 }, 1, false, false },
		{ TEXT("climb_for_crash"), 6.0f, { 0, 0.2f }, 0, false, false, 0.45f },
		{ TEXT("brake_exhaust"), 7.0f, { 0, 0 }, -1, false, false, 0.6f },
		{ TEXT("exhausted_pump"), 3.0f, { 0, 0 }, -1, false, false, 0.5f },
		{ TEXT("hurt_flying"), 1.0f, { 0, 0 }, 0, false, false, 0.5f, 20.f },
		{ TEXT("fall_crash"), 20.0f, { 0, 0 }, 0, false, true },
		{ TEXT("landed_again"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("dismount"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("on_foot"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("summon_again"), 1.5f, { 0, 0 }, 0, false, false },
		{ TEXT("takeoff_to_die"), 2.5f, { 0, 0 }, 1, false, false },
		{ TEXT("climb_to_die"), 3.0f, { 0, 0.5f }, 0, false, false, 0.5f },
		{ TEXT("die"), 0.4f, { 0, 0 }, 0, false, false, 0.f, 1000.f },
		{ TEXT("dying_fall"), 3.0f, { 0, 0 }, 0, false, false },
		{ TEXT("you_died"), 1.0f, { 0, 0 }, 0, false, false },
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
	// Once per step: getting back on, hurting, and the first beat of pumping.
	static int32 StartedStep = INDEX_NONE;
	static double NextPump = 0.0;
	const bool bStepStart = StartedStep != HeroStep;
	StartedStep = HeroStep;
	if (bStepStart)
	{
		NextPump = Elapsed;
		ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
		if (Hero && FCString::Strcmp(Step.Name, TEXT("summon_again")) == 0)
		{
			Hero->SummonDinofly();
			Pawn = PC->GetPawn();
		}
	}
	ALBADinofly* Dinofly = Cast<ALBADinofly>(Pawn);
	if (Dinofly)
	{
		Dinofly->DisableInput(PC);
		if (bStepStart && Step.Damage > 0.f)
		{
			Dinofly->GetHitPoints()->TakeHit(Step.Damage, Dinofly->GetActorLocation() + FVector(0.f, 0.f, 150.f));
		}
		if (Step.Pump != 0.f && Elapsed >= NextPump)
		{
			if (Step.Pump > 0.f)
			{
				Dinofly->Flap();
			}
			else
			{
				Dinofly->BrakeTap();
			}
			NextPump += FMath::Abs(Step.Pump);
		}
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
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots dinofly %s t=%.2f loc=(%.0f %.0f %.0f) vel=(%.0f %.0f %.0f) speed2d=%.0f air=%.2f beats=%d stall=%.2f brake=%.2f/%.2f%s life=%.0f dead=%d dive=%d@%.2f height=%.0f pitch=%.1f yaw=%.0f roll=%.1f cam(pitch=%.1f fov=%.1f) landed=%d floating=%d takingoff=%d anim=%s@%.2f layer=%d@%.2f"),
				Step.Name, Elapsed - HeroStepStart, Dinofly->GetActorLocation().X, Dinofly->GetActorLocation().Y, Dinofly->GetActorLocation().Z,
				Flight->Velocity.X, Flight->Velocity.Y, Flight->Velocity.Z, Flight->Velocity.Size2D(), Flight->GetAirDensity(), Flight->GetBeatCount(),
					Flight->GetStall(), Flight->GetBrakeAmount(), Flight->GetBrakeStamina(), Flight->IsBrakeExhausted() ? TEXT("!") : TEXT(""),
					Dinofly->GetHitPoints()->GetLife(), Dinofly->IsDead() ? 1 : 0, Flight->IsDiving() ? (Flight->IsPullingOut() ? 2 : 1) : 0, Flight->GetDiveAggression(), Flight->GetHeightAboveSurface(10000.f),
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

	// -LBA2ShotBurst=S adds a shot every S seconds during the fast flying steps, to see things that move in stills.
	static float BurstInterval = -1.f;
	if (BurstInterval < 0.f)
	{
		BurstInterval = 0.f;
		FParse::Value(FCommandLine::Get(), TEXT("LBA2ShotBurst="), BurstInterval);
	}
	if (bStepStart)
	{
		NextBurst = Elapsed;
		BurstIndex = 0;
	}
	if (BurstInterval > 0.f && Elapsed >= NextBurst
		&& (FCString::Strcmp(Step.Name, TEXT("pump_climb")) == 0 || FCString::Strcmp(Step.Name, TEXT("forward_cruise")) == 0
			|| FCString::Strcmp(Step.Name, TEXT("turn_right_cruise")) == 0 || FCString::Strcmp(Step.Name, TEXT("dive_long")) == 0
			|| FCString::Strcmp(Step.Name, TEXT("fall_hands_off")) == 0))
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("Burst_%02d_%s_%03d.png"), HeroStep, Step.Name, BurstIndex++), false, false);
		NextBurst += BurstInterval;
	}

	if (Elapsed - HeroStepStart >= Step.Duration)
	{
		const FString File = ShotDirectory / FString::Printf(TEXT("Dinofly_%02d_%s.png"), HeroStep, Step.Name);
		// The death screen is UI.
		FScreenshotRequest::RequestScreenshot(File, FCString::Strcmp(Step.Name, TEXT("you_died")) == 0, false);
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

namespace
{
	/**
	 * The planet's belt shore (world, at sea level) straight out from its pole past the opened island (the way out),
	 * where its ground first rises above the sea; false while the belt isn't streamed in.
	 */
	bool FindBeltShore(UWorld* World, const ULBAPlanetSubsystem* Planets, FVector& OutShore, FVector& OutOutward)
	{
		TArray<UStaticMeshComponent*> Belt;
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(ULBAPlanetAsset::BeltTag))
			{
				Belt.Add(It->GetStaticMeshComponent());
			}
		}
		if (!Planets || !Planets->IsActive() || Belt.IsEmpty())
		{
			return false;
		}
		const FVector Pole = Planets->GetMapToWorld().GetLocation();
		const FVector Home = Planets->GetIslandCenter(Planets->GetHome());
		const FVector Outward = (Home - Pole).GetSafeNormal2D().IsNearlyZero() ? FVector::ForwardVector : (Home - Pole).GetSafeNormal2D();
		FCollisionQueryParams Params(TEXT("BeltShore"), true);
		for (double Distance = FVector::Dist2D(Home, Pole); Distance < 3.0 * Planets->GetRadius(); Distance += 100.0)
		{
			const FVector Point = Pole + Outward * Distance;
			for (UStaticMeshComponent* Component : Belt)
			{
				FHitResult Hit;
				if (Component->LineTraceComponent(Hit, FVector(Point.X, Point.Y, 50000.0), FVector(Point.X, Point.Y, -5000.0), Params) && Hit.ImpactPoint.Z > 0.0)
				{
					OutShore = FVector(Point.X, Point.Y, 0.0);
					OutOutward = Outward;
					UE_LOG(LogLBA2, Display, TEXT("LBA2Shots: the belt's shore is %.0f m out from the pole, %.0f m from the opened island"),
						Distance / 100.0, FVector::Dist2D(OutShore, Home) / 100.0);
					return true;
				}
			}
		}
		return false;
	}
}

bool ULBA2SnapshotSubsystem::TickBoat(UWorld* World, float DeltaTime)
{
	enum class EBoatStep : uint8 { Approach, Board, Leave, Drive, Stop, LeaveAtSea };
	struct FStep
	{
		const TCHAR* Name;
		float Duration;
		EBoatStep Kind;
		float Throttle;
		/** Steers away from the opened island (-1), towards the target (1), or holds Rudder (0). */
		float Steer;
		float Rudder;
		/** What it steers to: the opened island (0) or the voyage's destination (1). */
		int32 Target = 0;
		/** Ends once the world is held at the target. */
		bool bUntilHeld = false;
	};
	static const FStep HomeSteps[] =
	{
		{ TEXT("quay"), 1.5f, EBoatStep::Approach, 0.f, 0.f, 0.f },
		{ TEXT("on_board"), 1.5f, EBoatStep::Board, 0.f, 0.f, 0.f },
		{ TEXT("ashore"), 1.5f, EBoatStep::Leave, 0.f, 0.f, 0.f },
		{ TEXT("on_board_again"), 1.f, EBoatStep::Board, 0.f, 0.f, 0.f },
		{ TEXT("back_out"), 5.f, EBoatStep::Drive, -1.f, 0.f, 0.f },
		{ TEXT("head_out"), 6.f, EBoatStep::Drive, 0.5f, -1.f, 0.f },
		{ TEXT("full_speed"), 12.f, EBoatStep::Drive, 1.f, -1.f, 0.f },
		{ TEXT("turning"), 4.f, EBoatStep::Drive, 1.f, 0.f, 1.f },
		{ TEXT("heading_home"), 8.f, EBoatStep::Drive, 1.f, 1.f, 0.f },
		{ TEXT("stopped"), 6.f, EBoatStep::Stop, -1.f, 0.f, 0.f },
		{ TEXT("leave_at_sea"), 1.f, EBoatStep::LeaveAtSea, 0.f, 0.f, 0.f },
	};
	// -LBA2BoatTo=ISLAND: across the sea to that island, until the world is held there.
	static const FStep VoyageSteps[] =
	{
		{ TEXT("quay"), 1.5f, EBoatStep::Approach, 0.f, 0.f, 0.f },
		{ TEXT("on_board"), 1.5f, EBoatStep::Board, 0.f, 0.f, 0.f },
		{ TEXT("back_out"), 5.f, EBoatStep::Drive, -1.f, 0.f, 0.f },
		{ TEXT("head_out"), 6.f, EBoatStep::Drive, 0.5f, -1.f, 0.f },
		{ TEXT("leave_home"), 8.f, EBoatStep::Drive, 1.f, -1.f, 0.f },
		{ TEXT("voyage"), 90.f, EBoatStep::Drive, 1.f, 1.f, 0.f, 1, true },
		{ TEXT("arrived"), 4.f, EBoatStep::Drive, 0.6f, 1.f, 0.f, 1 },
		{ TEXT("stopped"), 6.f, EBoatStep::Stop, -1.f, 0.f, 0.f, 1 },
	};
	const bool bVoyage = BoatDestination != INDEX_NONE || bBoatToBelt;
	const FStep* Steps = bVoyage ? VoyageSteps : HomeSteps;
	const int32 NumSteps = bVoyage ? UE_ARRAY_COUNT(VoyageSteps) : UE_ARRAY_COUNT(HomeSteps);

	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	ULBAPlanetSubsystem* Planets = World->GetSubsystem<ULBAPlanetSubsystem>();
	if (BoatStep < 0)
	{
		ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
		// The boat moored nearest to Twinsen (at the opened island).
		ALBABoat* Boat = nullptr;
		for (TActorIterator<ALBABoat> It(World); It && Hero; ++It)
		{
			if (!Boat || FVector::Dist(It->GetActorLocation(), Hero->GetActorLocation()) < FVector::Dist(Boat->GetActorLocation(), Hero->GetActorLocation()))
			{
				Boat = *It;
			}
		}
		if (!Planets || !Planets->IsActive() || !Hero || !Boat)
		{
			if (Elapsed > 15.0)
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots boat: no planet (open the island with ?Globe), no ALBACharacter or no boat moored"));
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
			return true;
		}
		TestBoat = Boat;
		FString Destination;
		if (FParse::Value(FCommandLine::Get(), TEXT("LBA2BoatTo="), Destination) && Destination.Equals(TEXT("BELT"), ESearchCase::IgnoreCase))
		{
			if (!FindBeltShore(World, Planets, BeltShore, BeltOutward))
			{
				if (Elapsed > 30.0)
				{
					UE_LOG(LogLBA2, Error, TEXT("LBA2Shots boat: the planet has no belt out there"));
					FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
					return false;
				}
				return true;
			}
			bBoatToBelt = true;
		}
		else if (!Destination.IsEmpty())
		{
			BoatDestination = Planets->GetPlanetAsset()->Islands.IndexOfByPredicate([&Destination](const FLBAPlanetIsland& Island)
			{
				return Island.Island.ToString().Equals(Destination, ESearchCase::IgnoreCase);
			});
			if (BoatDestination == INDEX_NONE || BoatDestination == Planets->GetHome())
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots boat: %s isn't another island of the planet"), *Destination);
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat: %s at %s, yaw %.0f"), *Boat->GetName(), *Boat->GetActorLocation().ToCompactString(), Boat->GetActorRotation().Yaw);
		BoatStep = 0;
		BoatStepStart = Elapsed;
		BoatFrame = 0;
		BoatNextReport = Elapsed;
		return true;
	}
	ALBABoat* Boat = TestBoat.Get();
	if (BoatStep >= NumSteps || !Boat)
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat done: top speed %.1f m/s"), BoatTopSpeed / 100.f);
		FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
		return false;
	}

	const FStep& Step = Steps[BoatStep];
	const double StepTime = Elapsed - BoatStepStart;
	auto Shoot = [&](const FString& Name)
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("Boat_%02d_%s.png"), BoatStep, *Name), false, false);
	};
	auto NextStep = [&]()
	{
		++BoatStep;
		BoatStepStart = Elapsed;
		BoatFrame = 0;
	};
	ALBACharacter* Hero = Boat->GetRider() ? Boat->GetRider() : Cast<ALBACharacter>(Pawn);
	// Towards the belt: a point 50 m in past its shore, so it drives into it.
	const bool bToBelt = Step.Target == 1 && bBoatToBelt;
	const int32 Target = Step.Target == 1 && !bBoatToBelt ? BoatDestination : Planets->GetHome();
	const FLBAPlanetIsland& Island = Planets->GetPlanetAsset()->Islands[Target];
	const FVector IslandCenter = bToBelt ? BeltShore + BeltOutward * 5000.0 : Planets->GetIslandCenter(Target);
	const FString TargetName = bToBelt ? FString(TEXT("the belt")) : Island.Island.ToString();
	const FVector HomeCenter = Planets->GetIslandCenter(Planets->GetHome());
	// How far the boat is from the target's center (m).
	const double ToTarget = FVector::Dist2D(Boat->GetActorLocation(), IslandCenter) / 100.0;

	switch (Step.Kind)
	{
	case EBoatStep::Approach:
		if (BoatFrame == 0)
		{
			// On the quay (or the beach) next to the boat, facing it.
			FVector Spot;
			if (!Hero || !Boat->FindLandingSpot(Spot))
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots boat: no land next to the boat"));
				NextStep();
				return true;
			}
			const FVector ToBoat = Boat->GetActorLocation() - Spot;
			Hero->TeleportTo(Spot, FRotator(0.f, ToBoat.Rotation().Yaw, 0.f));
			if (PC)
			{
				PC->SetControlRotation(FRotator(-20.f, ToBoat.Rotation().Yaw, 0.f));
			}
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat: Twinsen at %s, %.0f cm from the boat"), *Spot.ToCompactString(), ToBoat.Size2D());
		}
		break;
	case EBoatStep::Board:
		if (BoatFrame == 0)
		{
			// Turned towards the boat (stepping ashore faces the land).
			if (Hero && !Hero->IsRiding())
			{
				Hero->SetActorRotation(FRotator(0.f, (Boat->GetActorLocation() - Hero->GetActorLocation()).Rotation().Yaw, 0.f));
			}
			const bool bUsed = Hero && Hero->TryInteract();
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat: use %s, pawn now %s"), bUsed ? TEXT("ok") : TEXT("found nothing"), PC && PC->GetPawn() ? *PC->GetPawn()->GetName() : TEXT("none"));
		}
		break;
	case EBoatStep::Leave:
	case EBoatStep::LeaveAtSea:
		if (BoatFrame == 0)
		{
			const bool bOff = Boat->Leave();
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat: leave %s (%s), pawn now %s"), bOff ? TEXT("ok") : TEXT("refused"),
				Step.Kind == EBoatStep::LeaveAtSea ? TEXT("at sea: refused expected") : TEXT("at the quay"), PC && PC->GetPawn() ? *PC->GetPawn()->GetName() : TEXT("none"));
		}
		break;
	case EBoatStep::Drive:
	case EBoatStep::Stop:
	{
		if (PC && PC->GetPawn() != Boat)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots boat: the player isn't on the boat (%s)"), PC->GetPawn() ? *PC->GetPawn()->GetName() : TEXT("none"));
			BoatStep = NumSteps;
			return true;
		}
		Boat->DisableInput(PC);
		float Rudder = Step.Rudder;
		if (Step.Steer != 0.f)
		{
			const FVector To = Step.Steer > 0.f ? IslandCenter - Boat->GetActorLocation() : Boat->GetActorLocation() - HomeCenter;
			const double Desired = FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X));
			Rudder = float(FMath::Clamp(FRotator::NormalizeAxis(Desired - Boat->GetActorRotation().Yaw) / 30.0, -1.0, 1.0));
		}
		const float Speed = Boat->GetBoatMovement()->GetForwardSpeed();
		const float Throttle = Step.Kind == EBoatStep::Stop && Speed < 50.f ? 0.f : Step.Throttle;
		Boat->SetDriveInput(FVector2D(Rudder, Throttle));
		BoatTopSpeed = FMath::Max(BoatTopSpeed, Speed);
		if (Elapsed >= BoatNextReport)
		{
			BoatNextReport = Elapsed + 1.0;
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat %s t=%.1f: %.1f m/s, turning %.1f deg/s, throttle %.1f rudder %.2f, %s, %.0f m from %s"),
				Step.Name, StepTime, Speed / 100.f, Boat->GetBoatMovement()->GetTurnRate(), Throttle, Rudder,
				Boat->GetBoatMovement()->IsAfloat() ? TEXT("afloat") : TEXT("NO WATER"), ToTarget, *TargetName);
		}
		if (Step.bUntilHeld && bToBelt && StepTime > 5.0 && Speed < 100.f)
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat: aground at the belt after %.1f s, %.1f m past its shore, hull %.1f m above the sea"),
				StepTime, FVector::DotProduct(Boat->GetActorLocation() - BeltShore, BeltOutward) / 100.0, Boat->GetActorLocation().Z / 100.0);
			Shoot(Step.Name);
			Boat->SetDriveInput(FVector2D::ZeroVector);
			NextStep();
			return true;
		}
		if (Step.bUntilHeld && !bToBelt && ToTarget * 100.0 < Island.Reach)
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots boat: within reach of %s after %.1f s"), *Island.Island.ToString(), StepTime);
			Shoot(Step.Name);
			Boat->SetDriveInput(FVector2D::ZeroVector);
			NextStep();
			return true;
		}
		if (Step.Kind == EBoatStep::Drive && BoatFrame > 0 && StepTime >= Step.Duration * 0.5 && StepTime - DeltaTime < Step.Duration * 0.5)
		{
			Shoot(FString(Step.Name) + TEXT("_mid"));
		}
		break;
	}
	}
	++BoatFrame;
	if (StepTime >= Step.Duration)
	{
		Shoot(Step.Name);
		Boat->SetDriveInput(FVector2D::ZeroVector);
		NextStep();
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickGlobe(UWorld* World, float DeltaTime)
{
	enum class EGlobeStep : uint8 { Fly, HighView };
	struct FStep
	{
		const TCHAR* Name;
		float Duration;
		EGlobeStep Kind;
		/** Steers away from the island (-1), towards it (1) or not (0); forward input. */
		float Steer;
		float Forward;
		float Vertical;
		/** Pumps the climb button every this many seconds (negative: taps the brake). */
		float Pump;
		bool bUntilLanded;
		/** High views: camera height above the Dinofly (cm). */
		float Height;
		/** What it steers to or looks at: the opened island (0) or the voyage's destination (1). */
		int32 Target = 0;
		/** Ends once this near the target (share of the voyage's distance at the start), or once within its reach. */
		float UntilWithin = 0.f;
		bool bUntilHeld = false;
	};
	static const FStep HomeSteps[] =
	{
		{ TEXT("takeoff"), 2.5f, EGlobeStep::Fly, 0.f, 0.f, 1.f, 0.f, false, 0.f },
		{ TEXT("climb"), 6.f, EGlobeStep::Fly, 0.f, 0.f, 0.f, 0.4f, false, 0.f },
		{ TEXT("fly_out"), 35.f, EGlobeStep::Fly, -1.f, 1.f, 0.f, 1.f, false, 0.f },
		{ TEXT("at_sea"), 1.f, EGlobeStep::Fly, -1.f, 1.f, 0.f, 1.f, false, 0.f },
		{ TEXT("turn_back"), 6.f, EGlobeStep::Fly, 1.f, 0.6f, 0.f, 1.f, false, 0.f },
		{ TEXT("island_on_horizon"), 3.f, EGlobeStep::Fly, 1.f, 1.f, 0.f, 1.f, false, 0.f },
		{ TEXT("land_on_sea"), 60.f, EGlobeStep::Fly, 1.f, 0.f, 0.f, -0.45f, true, 0.f },
		{ TEXT("floating"), 2.f, EGlobeStep::Fly, 0.f, 0.f, 0.f, 0.f, false, 0.f },
		{ TEXT("high_view"), 4.f, EGlobeStep::HighView, 0.f, 0.f, 0.f, 0.f, false, 50000.f },
		{ TEXT("higher_view"), 4.f, EGlobeStep::HighView, 0.f, 0.f, 0.f, 0.f, false, 150000.f },
		{ TEXT("takeoff_home"), 2.5f, EGlobeStep::Fly, 0.f, 0.f, 1.f, 0.f, false, 0.f },
		{ TEXT("climb_home"), 4.f, EGlobeStep::Fly, 1.f, 0.f, 0.f, 0.4f, false, 0.f },
		{ TEXT("fly_home"), 40.f, EGlobeStep::Fly, 1.f, 1.f, 0.f, 1.f, false, 0.f },
		{ TEXT("home"), 1.f, EGlobeStep::Fly, 1.f, 0.5f, 0.f, 1.f, false, 0.f },
	};
	// -LBA2GlobeTo=ISLAND: across the sea to another island, until within its reach.
	static const FStep VoyageSteps[] =
	{
		{ TEXT("takeoff"), 2.5f, EGlobeStep::Fly, 0.f, 0.f, 1.f, 0.f, false, 0.f },
		{ TEXT("climb"), 6.f, EGlobeStep::Fly, 0.f, 0.f, 0.f, 0.4f, false, 0.f },
		{ TEXT("leave_home"), 20.f, EGlobeStep::Fly, 1.f, 1.f, 0.f, 1.f, false, 0.f, 1 },
		{ TEXT("halfway"), 120.f, EGlobeStep::Fly, 1.f, 1.f, 0.f, 1.f, false, 0.f, 1, 0.5f },
		{ TEXT("destination_ahead"), 120.f, EGlobeStep::Fly, 1.f, 1.f, 0.f, 1.f, false, 0.f, 1, 0.3f },
		{ TEXT("arrived"), 120.f, EGlobeStep::Fly, 1.f, 1.f, 0.f, 1.f, false, 0.f, 1, 0.f, true },
		{ TEXT("over_destination"), 4.f, EGlobeStep::HighView, 0.f, 0.f, 0.f, 0.f, false, 50000.f, 1 },
		{ TEXT("look_back"), 4.f, EGlobeStep::HighView, 0.f, 0.f, 0.f, 0.f, false, 20000.f, 0 },
		{ TEXT("land"), 40.f, EGlobeStep::Fly, 1.f, 0.3f, 0.f, -0.45f, true, 0.f, 1 },
		{ TEXT("landed"), 2.f, EGlobeStep::Fly, 0.f, 0.f, 0.f, 0.f, false, 0.f, 1 },
	};
	const FStep* Steps = GlobeDestination != INDEX_NONE ? VoyageSteps : HomeSteps;
	const int32 NumSteps = GlobeDestination != INDEX_NONE ? UE_ARRAY_COUNT(VoyageSteps) : UE_ARRAY_COUNT(HomeSteps);

	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	ULBAPlanetSubsystem* Planets = World->GetSubsystem<ULBAPlanetSubsystem>();
	if (GlobeStep < 0)
	{
		ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
		if (!Planets || !Planets->IsActive() || !Hero)
		{
			if (Elapsed > 15.0)
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots globe: no planet (open the island with ?Globe) or no ALBACharacter"));
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
			return true;
		}
		if (!Hero->SummonDinofly())
		{
			return true;
		}
		FString Destination;
		if (FParse::Value(FCommandLine::Get(), TEXT("LBA2GlobeTo="), Destination))
		{
			GlobeDestination = Planets->GetPlanetAsset()->Islands.IndexOfByPredicate([&Destination](const FLBAPlanetIsland& Island)
			{
				return Island.Island.ToString().Equals(Destination, ESearchCase::IgnoreCase);
			});
			if (GlobeDestination == INDEX_NONE || GlobeDestination == Planets->GetHome())
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots globe: %s isn't another island of the planet"), *Destination);
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
			GlobeVoyageDistance = FVector::Dist2D(Hero->GetActorLocation(), Planets->GetIslandCenter(GlobeDestination)) / 100.0;
		}
		GlobeStep = 0;
		GlobeStepStart = Elapsed;
		GlobeFrame = 0;
		NextReport = Elapsed;
		return true;
	}
	if (GlobeStep >= NumSteps)
	{
		if (FApp::UseFixedTimeStep())
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots globe done: %d frames at a fixed time step (-LBA2ShotStep=0 measures frame rates)"), GlobeFrames);
		}
		else
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots globe done: %.1f fps average over %d frames, worst frame %.1f ms"),
				GlobeFrames / FMath::Max(GlobeFrameTime, 1e-3), GlobeFrames, GlobeWorstFrame * 1000.0);
		}
		FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
		return false;
	}

	const FStep& Step = Steps[GlobeStep];
	// -LBA2GlobeQuick: out to sea and back only.
	if (Step.Kind == EGlobeStep::HighView && FParse::Param(FCommandLine::Get(), TEXT("LBA2GlobeQuick")))
	{
		GlobeStep = NumSteps;
		return true;
	}
	const double StepTime = Elapsed - GlobeStepStart;
	ALBADinofly* Dinofly = Cast<ALBADinofly>(Pawn);
	const int32 Target = Step.Target == 1 ? GlobeDestination : Planets->GetHome();
	const FLBAPlanetIsland& Island = Planets->GetPlanetAsset()->Islands[Target];
	const FVector IslandCenter = Planets->GetIslandCenter(Target);
	// How far the Dinofly is from the target's center (m).
	const double ToTarget = Pawn ? FVector::Dist2D(Pawn->GetActorLocation(), IslandCenter) / 100.0 : 0.0;
	auto Shoot = [&](const FString& Name)
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("Globe_%02d_%s.png"), GlobeStep, *Name), false, false);
	};
	auto NextStep = [&]()
	{
		++GlobeStep;
		GlobeStepStart = Elapsed;
		GlobeFrame = 0;
	};
	if (Step.Kind == EGlobeStep::HighView)
	{
		// A camera high above the Dinofly, looking at the island over the curve of the sea.
		ACameraActor* View = Cast<ACameraActor>(Camera.Get());
		if (GlobeFrame == 0)
		{
			const FVector Location = Pawn->GetActorLocation() + FVector(0.0, 0.0, Step.Height);
			const FRotator Rotation = (IslandCenter - Location).Rotation();
			if (!View)
			{
				View = World->SpawnActor<ACameraActor>(Location, Rotation);
				Camera = View;
			}
			View->SetActorLocationAndRotation(Location, Rotation);
			View->GetCameraComponent()->SetFieldOfView(70.f);
			PC->SetViewTarget(View);
		}
		++GlobeFrame;
		if (StepTime >= Step.Duration)
		{
			// The view goes back to the Dinofly once this shot is taken (next step).
			Shoot(Step.Name);
			NextStep();
		}
		return true;
	}

	if (!Dinofly)
	{
		UE_LOG(LogLBA2, Error, TEXT("LBA2Shots globe: the player isn't on the Dinofly (%s)"), Pawn ? *Pawn->GetName() : TEXT("none"));
		FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
		return false;
	}
	Dinofly->DisableInput(PC);
	if (PC->GetViewTarget() != Dinofly)
	{
		PC->SetViewTarget(Dinofly);
	}

	// Steering towards or away from the island.
	float Turn = 0.f;
	if (Step.Steer != 0.f)
	{
		const FVector ToIsland = (IslandCenter - Dinofly->GetActorLocation()) * Step.Steer;
		const double Desired = FMath::RadiansToDegrees(FMath::Atan2(ToIsland.Y, ToIsland.X));
		Turn = float(FMath::Clamp(FRotator::NormalizeAxis(Desired - Dinofly->GetActorRotation().Yaw) / 30.0, -1.0, 1.0));
	}
	static double NextPump = 0.0;
	if (GlobeFrame == 0)
	{
		NextPump = Elapsed;
	}
	if (Step.Pump != 0.f && Elapsed >= NextPump)
	{
		Step.Pump > 0.f ? Dinofly->Flap() : Dinofly->BrakeTap();
		NextPump += FMath::Abs(Step.Pump);
	}
	Dinofly->SetHoverInput(FVector2D(Turn, Step.Forward), Step.Vertical);
	Dinofly->SetDiveInput(false);

	GlobeFrameTime += DeltaTime;
	GlobeWorstFrame = FMath::Max(GlobeWorstFrame, double(DeltaTime));
	if (DeltaTime > 0.05f && !FApp::UseFixedTimeStep())
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots globe slow frame (%s t=%.2f): %.1f ms"), Step.Name, StepTime, DeltaTime * 1000.f);
	}
	++GlobeFrames;
	++GlobeFrame;

	if (Elapsed >= NextReport)
	{
		double SeaZ = 0.0;
		Planets->GetSeaSurface(Dinofly->GetActorLocation(), SeaZ);
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots globe %s t=%.1f: %.0f m from %s, %.1f m above the sea, speed %.1f m/s, landed %d floating %d"),
			Step.Name, StepTime, ToTarget, *Island.Island.ToString(), (Dinofly->GetActorLocation().Z - SeaZ) / 100.0,
			Dinofly->GetFlight()->Velocity.Size() / 100.0, Dinofly->GetFlight()->IsLanded() ? 1 : 0, Dinofly->GetFlight()->IsFloating() ? 1 : 0);
		NextReport = Elapsed + 1.0;
	}

	const bool bLandedEarly = Step.bUntilLanded && Dinofly->GetFlight()->IsLanded();
	const bool bThere = (Step.UntilWithin > 0.f && ToTarget < Step.UntilWithin * GlobeVoyageDistance)
		|| (Step.bUntilHeld && ToTarget * 100.0 < Island.Reach);
	if (StepTime >= Step.Duration || bLandedEarly || bThere)
	{
		Shoot(Step.Name);
		NextStep();
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickBelt(UWorld* World, float DeltaTime)
{
	enum class EBeltStep : uint8 { Fly, HighView, Dismount, Walk };
	struct FStep
	{
		const TCHAR* Name;
		float Duration;
		EBeltStep Kind;
		float Forward;
		float Vertical;
		/** Pumps the climb button every this many seconds (negative: taps the brake). */
		float Pump;
		/** Where it steers (or a high view looks): this far past the shore (m), or at the pole (bAtPole). */
		float PastShore;
		/** Ends once this near the point it steers to (m), or once landed. */
		float UntilWithin = 0.f;
		bool bUntilLanded = false;
		/** High views: camera height above the pawn (cm). */
		float Height = 0.f;
		bool bAtPole = false;
		/** Walks: jumps every this many seconds. */
		float Jump = 0.f;
	};
	static const FStep Steps[] =
	{
		{ TEXT("takeoff"), 2.5f, EBeltStep::Fly, 0.f, 1.f, 0.f, 0.f },
		{ TEXT("climb"), 6.f, EBeltStep::Fly, 0.f, 0.f, 0.4f, 0.f },
		{ TEXT("to_the_belt"), 150.f, EBeltStep::Fly, 1.f, 0.f, 0.5f, 0.f, 30.f },
		// The belt is low: the Dinofly flies over it.
		{ TEXT("over_the_belt"), 60.f, EBeltStep::Fly, 1.f, 0.f, 0.5f, 200.f, 20.f },
		{ TEXT("belt_from_above"), 4.f, EBeltStep::HighView, 0.f, 0.f, 0.f, 0.f, 0.f, false, 30000.f },
		{ TEXT("whole_belt"), 4.f, EBeltStep::HighView, 0.f, 0.f, 0.f, 0.f, 0.f, false, 400000.f, true },
		{ TEXT("back_to_flank"), 90.f, EBeltStep::Fly, 1.f, 0.f, 0.5f, 5.f, 15.f },
		{ TEXT("land_on_flank"), 60.f, EBeltStep::Fly, 0.3f, 0.f, -0.45f, 5.f, 0.f, true },
		{ TEXT("landed"), 2.f, EBeltStep::Fly, 0.f, 0.f, 0.f, 5.f },
		{ TEXT("dismount"), 2.f, EBeltStep::Dismount, 0.f, 0.f, 0.f, 0.f },
		{ TEXT("walk_up"), 30.f, EBeltStep::Walk, 1.f, 0.f, 0.f, 0.f },
		{ TEXT("jump_up"), 12.f, EBeltStep::Walk, 1.f, 0.f, 0.f, 0.f, 0.f, false, 0.f, false, 1.2f },
	};
	const int32 NumSteps = UE_ARRAY_COUNT(Steps);

	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	ULBAPlanetSubsystem* Planets = World->GetSubsystem<ULBAPlanetSubsystem>();
	if (BeltStep < 0)
	{
		ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
		if (!Hero || !FindBeltShore(World, Planets, BeltShore, BeltOutward))
		{
			if (Elapsed > 30.0)
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots belt: no planet (open the island with ?Globe), no ALBACharacter or no belt"));
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
			return true;
		}
		if (!Hero->SummonDinofly())
		{
			return true;
		}
		BeltStep = 0;
		BeltStepStart = Elapsed;
		BeltFrame = 0;
		NextReport = Elapsed;
		return true;
	}
	if (BeltStep >= NumSteps)
	{
		// Twinsen's feet stay below the cliff band's top (16 m; the flank's top is 6 m). The Dinofly flies over.
		const bool bHeld = BeltHeroTop < 1600.0;
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots belt done: the Dinofly got %.1f m above the sea and %.0f m past the shore; Twinsen's feet %.1f m (the flank's top: 6 m, the cliff's: 16 m): %s"),
			BeltDinoflyTop / 100.0, BeltDeepest / 100.0, BeltHeroTop / 100.0, bHeld ? TEXT("the belt held him") : TEXT("TWINSEN CLIMBED THE BELT"));
		FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
		return false;
	}

	const FStep& Step = Steps[BeltStep];
	const double StepTime = Elapsed - BeltStepStart;
	const FVector Pole = Planets->GetMapToWorld().GetLocation();
	const FVector Point = Step.bAtPole ? FVector(Pole.X, Pole.Y, 0.0) : BeltShore + BeltOutward * (Step.PastShore * 100.0);
	auto Shoot = [&](const FString& Name)
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("Belt_%02d_%s.png"), BeltStep, *Name), false, false);
	};
	auto NextStep = [&]()
	{
		++BeltStep;
		BeltStepStart = Elapsed;
		BeltFrame = 0;
	};
	if (Pawn)
	{
		BeltDeepest = FMath::Max(BeltDeepest, FVector::DotProduct(Pawn->GetActorLocation() - BeltShore, BeltOutward));
	}

	switch (Step.Kind)
	{
	case EBeltStep::HighView:
	{
		ACameraActor* View = Cast<ACameraActor>(Camera.Get());
		if (BeltFrame == 0 && Pawn)
		{
			// Over the pole, the whole hemisphere is in view: the camera bends it into a ball below.
			const FVector Location = Step.bAtPole ? FVector(Pole.X, Pole.Y, Step.Height) : Pawn->GetActorLocation() + FVector(0.0, 0.0, Step.Height);
			const FRotator Rotation = Step.bAtPole ? FRotator(-89.f, BeltOutward.Rotation().Yaw, 0.f) : (Point - Location).Rotation();
			if (!View)
			{
				View = World->SpawnActor<ACameraActor>(Location, Rotation);
				Camera = View;
			}
			View->SetActorLocationAndRotation(Location, Rotation);
			View->GetCameraComponent()->SetFieldOfView(Step.bAtPole ? 35.f : 70.f);
			PC->SetViewTarget(View);
		}
		// Meanwhile the Dinofly keeps beating (hands off, it would glide down: falling that far hurts).
		static double NextBeat = 0.0;
		ALBADinofly* Dinofly = Cast<ALBADinofly>(Pawn);
		if (Dinofly && Elapsed >= NextBeat)
		{
			Dinofly->Flap();
			NextBeat = Elapsed + 1.0;
		}
		break;
	}
	case EBeltStep::Fly:
	{
		ALBADinofly* Dinofly = Cast<ALBADinofly>(Pawn);
		if (!Dinofly)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots belt: the player isn't on the Dinofly (%s)"), Pawn ? *Pawn->GetName() : TEXT("none"));
			BeltStep = NumSteps;
			return true;
		}
		Dinofly->DisableInput(PC);
		if (PC->GetViewTarget() != Dinofly)
		{
			PC->SetViewTarget(Dinofly);
		}
		float Turn = 0.f;
		if (Step.Forward != 0.f)
		{
			const FVector To = Point - Dinofly->GetActorLocation();
			const double Desired = FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X));
			Turn = float(FMath::Clamp(FRotator::NormalizeAxis(Desired - Dinofly->GetActorRotation().Yaw) / 30.0, -1.0, 1.0));
		}
		static double NextPump = 0.0;
		if (BeltFrame == 0)
		{
			NextPump = Elapsed;
		}
		if (Step.Pump != 0.f && Elapsed >= NextPump)
		{
			Step.Pump > 0.f ? Dinofly->Flap() : Dinofly->BrakeTap();
			NextPump += FMath::Abs(Step.Pump);
		}
		Dinofly->SetHoverInput(FVector2D(Turn, Step.Forward), Step.Vertical);
		Dinofly->SetDiveInput(false);
		const double Height = Dinofly->GetActorLocation().Z;
		BeltDinoflyTop = FMath::Max(BeltDinoflyTop, Height);
		const double ToPoint = FVector::Dist2D(Dinofly->GetActorLocation(), Point) / 100.0;
		if (Elapsed >= NextReport)
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots belt %s t=%.1f: %.0f m past the shore, %.1f m above the sea (highest %.1f), speed %.1f m/s, landed %d, life %.0f%s"),
				Step.Name, StepTime, FVector::DotProduct(Dinofly->GetActorLocation() - BeltShore, BeltOutward) / 100.0, Height / 100.0,
				BeltDinoflyTop / 100.0, Dinofly->GetFlight()->Velocity.Size() / 100.0, Dinofly->GetFlight()->IsLanded() ? 1 : 0,
				Dinofly->GetHitPoints()->GetLife(), Dinofly->IsDead() ? TEXT(" DEAD") : TEXT(""));
			NextReport = Elapsed + 1.0;
		}
		if ((Step.UntilWithin > 0.f && ToPoint < Step.UntilWithin) || (Step.bUntilLanded && Dinofly->GetFlight()->IsLanded()))
		{
			Shoot(Step.Name);
			NextStep();
			return true;
		}
		break;
	}
	case EBeltStep::Dismount:
		if (BeltFrame == 0)
		{
			ALBADinofly* Dinofly = Cast<ALBADinofly>(Pawn);
			const bool bOff = Dinofly && Dinofly->Dismount();
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots belt: dismount %s"), bOff ? TEXT("ok") : TEXT("refused"));
		}
		break;
	case EBeltStep::Walk:
	{
		ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
		if (!Hero)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots belt: Twinsen isn't the player (%s)"), Pawn ? *Pawn->GetName() : TEXT("none"));
			BeltStep = NumSteps;
			return true;
		}
		if (BeltFrame == 0)
		{
			Hero->DisableInput(PC);
			Hero->SetControlMode(ELBAControlMode::Modern);
			PC->SetViewTarget(Hero);
		}
		// Straight out, up the belt: the stick is camera-relative.
		PC->SetControlRotation(FRotator(-10.f, BeltOutward.Rotation().Yaw, 0.f));
		Hero->SetMoveInput(FVector2D(0.f, Step.Forward));
		static double NextJump = 0.0;
		if (BeltFrame == 0)
		{
			NextJump = Elapsed;
		}
		if (Step.Jump > 0.f && Elapsed >= NextJump)
		{
			Hero->ModernJump();
			NextJump += Step.Jump;
		}
		const double Feet = Hero->GetActorLocation().Z - Hero->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		BeltHeroTop = FMath::Max(BeltHeroTop, Feet);
		if (Elapsed >= NextReport)
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots belt %s t=%.1f: Twinsen %.0f m past the shore, feet %.1f m above the sea (highest %.1f), falling %d, life %.0f%s, %s"),
				Step.Name, StepTime, FVector::DotProduct(Hero->GetActorLocation() - BeltShore, BeltOutward) / 100.0, Feet / 100.0, BeltHeroTop / 100.0,
				Hero->GetCharacterMovement()->IsFalling() ? 1 : 0, Hero->GetHitPoints()->GetLife(), Hero->IsDead() ? TEXT(" DEAD") : TEXT(""),
				*Hero->GetModernDebugString());
			NextReport = Elapsed + 1.0;
		}
		break;
	}
	}
	++BeltFrame;
	if (StepTime >= Step.Duration)
	{
		if (ALBACharacter* Hero = Cast<ALBACharacter>(Pawn))
		{
			Hero->SetMoveInput(FVector2D::ZeroVector);
		}
		Shoot(Step.Name);
		NextStep();
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickWind(UWorld* World, float DeltaTime)
{
	struct FStep
	{
		const TCHAR* Name;
		float Duration;
		/** The Dinofly starts hovering there (zero: carries on), facing Yaw. */
		FVector Start;
		float Yaw;
		/** X turn right, Y forward. */
		FVector2D Move;
		float Vertical;
	};
	// Places of Citadel (storm): Lupin-Burg, Twinsen's house, the cliffs, the dome, the wall, far out at sea, the ceiling.
	static const FStep Steps[] =
	{
		{ TEXT("eye_lupinburg"), 5.f, { 35000, 38500, 1500 }, 0, { 0, 0 }, 0 },
		{ TEXT("house"), 5.f, { 34314, 42746, 1200 }, 90, { 0, 0 }, 0 },
		{ TEXT("cliffs"), 5.f, { 29614, 30277, 2500 }, 0, { 0, 0 }, 0 },
		{ TEXT("dome"), 5.f, { 26592, 33832, 1500 }, 0, { 0, 0 }, 0 },
		{ TEXT("wall_south"), 6.f, { 35000, 47500, 800 }, 90, { 0, 0 }, 0 },
		{ TEXT("fly_away_south"), 8.f, { 35000, 47500, 800 }, 90, { 0, 1 }, 0 },
		{ TEXT("far_sea_drift"), 6.f, { 35000, 60000, 800 }, 90, { 0, 0 }, 0 },
		{ TEXT("climb_to_ceiling"), 14.f, { 34314, 42746, 3500 }, 0, { 0, 0 }, 1 },
		{ TEXT("hold_high"), 8.f, FVector::ZeroVector, 0, { 0, 0 }, 0 },
	};
	const int32 NumSteps = UE_ARRAY_COUNT(Steps);

	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (HeroStep < 0)
	{
		ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
		if (!Hero || !Hero->SummonDinofly())
		{
			if (Elapsed > 15.0)
			{
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots wind: no Dinofly to ride"));
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
			return true;
		}
		HeroStep = 0;
		HeroStepStart = Elapsed;
		NextReport = Elapsed;
		bStepStarted = false;
		Pawn = PC->GetPawn();
	}
	if (HeroStep >= NumSteps)
	{
		FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
		return false;
	}

	const FStep& Step = Steps[HeroStep];
	ALBADinofly* Dinofly = Cast<ALBADinofly>(Pawn);
	if (!Dinofly)
	{
		return true;
	}
	ULBAFlightMovementComponent* Flight = Dinofly->GetFlight();
	Dinofly->DisableInput(PC);
	if (!bStepStarted)
	{
		bStepStarted = true;
		if (!Step.Start.IsZero())
		{
			Dinofly->SetActorLocationAndRotation(Step.Start, FRotator(0.f, Step.Yaw, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
			Flight->StartHovering();
		}
		StepStartLocation = Dinofly->GetActorLocation();
	}
	Dinofly->SetHoverInput(Step.Move, Step.Vertical);
	if (Elapsed >= NextReport)
	{
		const ULBAWindSubsystem* Wind = World->GetSubsystem<ULBAWindSubsystem>();
		const FVector Location = Dinofly->GetActorLocation();
		const FVector Air = Wind ? Wind->GetWindAt(Location) : FVector::ZeroVector;
		const FVector Drift = Flight->GetWindDrift();
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots wind %s t=%.2f loc=(%.0f %.0f %.0f) moved=%.0f vel=(%.0f %.0f %.0f) drift=(%.0f %.0f %.0f) wind=(%.0f %.0f %.0f) eye=%.2f landed=%d floating=%d"),
			Step.Name, Elapsed - HeroStepStart, Location.X, Location.Y, Location.Z, FVector::Dist2D(Location, StepStartLocation),
			Flight->Velocity.X, Flight->Velocity.Y, Flight->Velocity.Z, Drift.X, Drift.Y, Drift.Z, Air.X, Air.Y, Air.Z,
			Wind ? Wind->GetEyeDistance(Location) : -1.f, Flight->IsLanded() ? 1 : 0, Flight->IsFloating() ? 1 : 0);
		NextReport = Elapsed + 0.25;
	}
	if (Elapsed - HeroStepStart >= Step.Duration)
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("Wind_%02d_%s.png"), HeroStep, Step.Name), false, false);
		++HeroStep;
		HeroStepStart = Elapsed;
		bStepStarted = false;
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickMenu(UWorld* World, float DeltaTime)
{
	ALBAPlayerController* PC = Cast<ALBAPlayerController>(World->GetFirstPlayerController());
	if (!PC || !PC->GetPawn())
	{
		return true;
	}
	if (World != SceneWorld.Get())
	{
		SceneWorld = World;
		SceneStepStart = Elapsed;
		MenuPhase = 0;
		return true;
	}
	// One phase a second once the level has settled: shot, open, shot, next tab, shot, pick the next place.
	const double Time = Elapsed - SceneStepStart;
	if (Time < 3.0 + MenuPhase)
	{
		return true;
	}
	const FString Prefix = ShotDirectory / FString::Printf(TEXT("menu_%02d_%s"), SceneStep, *World->GetMapName());
	switch (MenuPhase++)
	{
	case 0:
		FScreenshotRequest::RequestScreenshot(Prefix + TEXT("_view.png"), true, false);
		break;
	case 1:
		PC->OpenIslandMenu();
		break;
	case 2:
		FScreenshotRequest::RequestScreenshot(Prefix + TEXT("_a.png"), true, false);
	{
		const ALBACharacter* Hero = Cast<ALBACharacter>(PC->GetPawn());
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots menu: %s, scene %d, at %s, life %.1f"), *Prefix,
			GetGameInstance()->GetSubsystem<ULBASceneTravelSubsystem>()->GetCurrentScene(), *PC->GetPawn()->GetActorLocation().ToString(),
			Hero ? Hero->GetHitPoints()->GetLife() : -1.f);
	}
		break;
	case 3:
	{
		// E, as the player would press it: the next tab.
		if (PC->GetIslandMenu())
		{
			FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::E, FModifierKeysState(), 0, false, 0, 0));
		}
		break;
	}
	case 4:
		FScreenshotRequest::RequestScreenshot(Prefix + TEXT("_b.png"), true, false);
		break;
	default:
		if (!MenuScenes.IsValidIndex(SceneStep))
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots menu: done"));
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		if (ULBAIslandMenu* Menu = PC->GetIslandMenu())
		{
			// Scene numbers; -1 = the first test level's start, -2 - N = its station N (the Tests tab, as clicked);
			// -100 - N = step onto the level's gateway N instead (menu closed). Pain tests (menu closed): -200 - H = drop
			// from H meters, -300 = Retry on the death screen, -400 = onto the first hit zone, -500 = walk into the first
			// enemy that hurts, -501 = stop walking.
			const int32 Pick = MenuScenes[SceneStep];
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots menu: picking %d"), Pick);
			ALBACharacter* Hero = Cast<ALBACharacter>(PC->GetPawn());
			if (Pick <= -200 && Pick > -300 && Hero)
			{
				PC->CloseIslandMenu();
				Hero->TeleportTo(Hero->GetActorLocation() + FVector(0.0, 0.0, (-200 - Pick) * 100.0), Hero->GetActorRotation());
			}
			else if (Pick == -300)
			{
				PC->CloseIslandMenu();
				for (TObjectIterator<ULBADeathScreen> It; It; ++It)
				{
					if (It->IsInViewport())
					{
						It->Retry();
					}
				}
			}
			else if (Pick == -400 && Hero)
			{
				PC->CloseIslandMenu();
				// The smallest one (its middle is clear), dropped into from above.
				const ALBAHitVolume* Smallest = nullptr;
				for (TActorIterator<ALBAHitVolume> It(World); It; ++It)
				{
					if (!Smallest || It->GetComponentsBoundingBox(true).GetVolume() < Smallest->GetComponentsBoundingBox(true).GetVolume())
					{
						Smallest = *It;
					}
				}
				if (Smallest)
				{
					const FBox Box = Smallest->GetComponentsBoundingBox(true);
					Hero->TeleportTo(FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Max.Z + 100.0), Hero->GetActorRotation(), false, true);
				}
			}
			else if ((Pick == -500 || Pick == -501) && Hero)
			{
				PC->CloseIslandMenu();
				for (TActorIterator<ALBASceneActor> It(World); It && Pick == -500; ++It)
				{
					if (It->TouchDamage > 0.f)
					{
						const FVector Direction = (It->GetActorLocation() - Hero->GetActorLocation()).GetSafeNormal2D();
						Hero->TeleportTo(It->GetActorLocation() - Direction * 160.0 + FVector(0.0, 0.0, 20.0), Direction.Rotation());
						PC->SetControlRotation(Direction.Rotation());
						break;
					}
				}
				Hero->SetMoveInput(Pick == -500 ? FVector2D(0.0, 1.0) : FVector2D::ZeroVector);
			}
			else if (Pick <= -100)
			{
				PC->CloseIslandMenu();
				TArray<ALBATravelGateway*> Gateways;
				for (TActorIterator<ALBATravelGateway> It(World); It; ++It)
				{
					Gateways.Add(*It);
				}
				Gateways.Sort([](const ALBATravelGateway& A, const ALBATravelGateway& B) { return A.GetName() < B.GetName(); });
				if (Gateways.IsValidIndex(-100 - Pick))
				{
					ALBATravelGateway* Gateway = Gateways[-100 - Pick];
					UE_LOG(LogLBA2, Display, TEXT("LBA2Shots menu: stepping onto %s (%s)"), *Gateway->GetName(), *Gateway->Destination.Describe(World).ToString());
					PC->GetPawn()->TeleportTo(Gateway->GetActorLocation() + FVector(0.0, 0.0, 100.0), Gateway->GetActorRotation());
				}
			}
			else if (Pick >= 0)
			{
				Menu->OnButtonClicked(ELBAMenuAction::Scene, Pick);
			}
			else
			{
				Menu->OnButtonClicked(ELBAMenuAction::TestsTab, INDEX_NONE);
				Menu->OnButtonClicked(ELBAMenuAction::TestLevel, 0);
				Menu->OnButtonClicked(Pick == -1 ? ELBAMenuAction::TestLevelStart : ELBAMenuAction::Station, Pick == -1 ? 0 : -2 - Pick);
			}
		}
		// Start over in the level it opens (or in this one, teleported).
		++SceneStep;
		SceneWorld.Reset();
		break;
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickScenes(UWorld* World, float DeltaTime)
{
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (World != SceneWorld.Get())
	{
		// A new level: let it settle, then screenshot from the player's camera.
		if (!Pawn)
		{
			return true;
		}
		SceneWorld = World;
		SceneStepStart = Elapsed;
		bSceneShotTaken = false;
		bWalkingIntoDoor = false;
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots scenes: in %s, hero at %s yaw %.0f"), *World->GetMapName(), *Pawn->GetActorLocation().ToString(), Pawn->GetActorRotation().Yaw);
		return true;
	}
	if (!Pawn || Elapsed - SceneStepStart < 3.0)
	{
		return true;
	}
	if (!bSceneShotTaken)
	{
		const FString File = ShotDirectory / FString::Printf(TEXT("scenes_%02d_%s.png"), SceneStep, *World->GetMapName());
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots scenes: %s, hero at %s"), *File, *Pawn->GetActorLocation().ToString());
		bSceneShotTaken = true;
		return true;
	}
	if (Elapsed - SceneStepStart < 4.0)
	{
		return true;
	}

	ULBASceneTravelSubsystem* Travel = GetGameInstance()->GetSubsystem<ULBASceneTravelSubsystem>();
	if (bWalkingIntoDoor)
	{
		// Waiting for the door (walked into) to change the level.
		if (Elapsed - SceneStepStart > 12.0)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots scenes: walking into the door did nothing (hero at %s)"), *Pawn->GetActorLocation().ToString());
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}
	const int32 Door = SceneStep;
	++SceneStep;
	if (SceneDoors.IsValidIndex(Door))
	{
		// Step into the door leading to the next scene (its bottom center), as if walking in.
		for (TActorIterator<ALBACubeChangeVolume> It(World); It; ++It)
		{
			if (It->TargetScene == SceneDoors[Door] && It->bEnabled)
			{
				const FBox Box = It->GetComponentsBoundingBox(true);
				const FVector Feet(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z);
				Pawn->TeleportTo(Feet + FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight() + 2.0), Pawn->GetActorRotation(), false, true);
				UE_LOG(LogLBA2, Display, TEXT("LBA2Shots scenes: door %s (zone %d of scene %d) to scene %d%s"), *It->GetName(), It->ZoneIndex,
					It->Scene ? It->Scene->SceneIndex : -1, It->TargetScene, It->bRequireWalkingIn ? TEXT(", walking into it") : TEXT(""));
				ALBACharacter* Hero = Cast<ALBACharacter>(Pawn);
				if (It->bRequireWalkingIn && Hero)
				{
					// Walk into the closest wall around the door zone, like a player would; the zone does the rest.
					FCollisionQueryParams Params(SCENE_QUERY_STAT(LBA2ShotsDoor), false, Pawn);
					const FVector Start = Pawn->GetActorLocation();
					FHitResult Best;
					Best.Distance = UE_BIG_NUMBER;
					const FVector Axes[4] = { FVector::ForwardVector, -FVector::ForwardVector, FVector::RightVector, -FVector::RightVector };
					for (const FVector& Axis : Axes)
					{
						FHitResult Hit;
						if (World->LineTraceSingleByChannel(Hit, Start, Start + Axis * 300.0, ECC_Visibility, Params) && Hit.Distance < Best.Distance)
						{
							Best = Hit;
						}
					}
					const FVector Toward = Best.bBlockingHit ? -Best.ImpactNormal.GetSafeNormal2D() : Pawn->GetActorForwardVector();
					PC->SetControlRotation(FRotator(-15.0, Toward.Rotation().Yaw, 0.0));
					Hero->SetMoveInput(FVector2D(0.0, 1.0));
					bWalkingIntoDoor = true;
					SceneStepStart = Elapsed;
					return true;
				}
				Travel->OnCubeChangeZone(*It, Pawn);
				return true;
			}
		}
		UE_LOG(LogLBA2, Error, TEXT("LBA2Shots scenes: no door to scene %d in %s"), SceneDoors[Door], *World->GetMapName());
	}
	else if (Door == SceneDoors.Num() && SceneEnd >= 0)
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots scenes: lba.Scene %d"), SceneEnd);
		if (Travel->TravelToScene(SceneEnd))
		{
			return true;
		}
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2Shots scenes: done"));
	FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
	return false;
}

bool ULBA2SnapshotSubsystem::TickLadder(UWorld* World, float DeltaTime)
{
	APlayerController* PC = World->GetFirstPlayerController();
	ALBACharacter* Hero = PC ? Cast<ALBACharacter>(PC->GetPawn()) : nullptr;
	if (!Hero)
	{
		return true;
	}
	auto Report = [&](const TCHAR* Label)
	{
		const UAnimSequence* Anim = Hero->GetLBAAnimInstance() ? Hero->GetLBAAnimInstance()->GetCurrentAnimation() : nullptr;
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots ladder %s t=%.2f loc=(%.0f %.0f %.0f) yaw=%.0f climbing=%d mode=%d anim=%s@%.2f"), Label, Elapsed - LadderStart,
			Hero->GetActorLocation().X, Hero->GetActorLocation().Y, Hero->GetActorLocation().Z, Hero->GetActorRotation().Yaw, Hero->IsClimbing() ? 1 : 0,
			int32(Hero->GetCharacterMovement()->MovementMode), Anim ? *Anim->GetName() : TEXT("none"),
			Hero->GetLBAAnimInstance() ? Hero->GetLBAAnimInstance()->GetCurrentAnimationTime() : 0.f);
	};

	if (LadderPhase == 0)
	{
		// Stand at the foot of the ladder, facing its wall (the closest wall around the zone's bottom center).
		const ALBALadderVolume* Ladder = nullptr;
		for (TActorIterator<ALBALadderVolume> It(World); It; ++It)
		{
			if (!Ladder || (It->Scene && It->Scene->SceneIndex == LadderScene && It->ZoneIndex == LadderZone))
			{
				Ladder = *It;
			}
		}
		if (!Ladder)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots ladder: no ladder volume in %s"), *World->GetMapName());
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		const FBox Box = Ladder->GetComponentsBoundingBox(true);
		const FVector Start(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z + 60.0);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(LBA2ShotsLadder), false, Hero);
		FHitResult Best;
		Best.Distance = UE_BIG_NUMBER;
		const FVector Axes[4] = { FVector::ForwardVector, -FVector::ForwardVector, FVector::RightVector, -FVector::RightVector };
		for (const FVector& Axis : Axes)
		{
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Start, Start + Axis * 600.0, ECC_Visibility, Params) && Hit.Distance < Best.Distance)
			{
				Best = Hit;
			}
		}
		const FVector Normal = Best.bBlockingHit ? Best.ImpactNormal.GetSafeNormal2D() : FVector::ForwardVector;
		const FVector Foot = (Best.bBlockingHit ? Best.ImpactPoint : Start) + Normal * (Hero->GetSimpleCollisionRadius() + 8.0);
		const FRotator Facing(0.0, (-Normal).Rotation().Yaw, 0.0);
		Hero->TeleportTo(FVector(Foot.X, Foot.Y, Box.Min.Z + Hero->GetSimpleCollisionHalfHeight() + 30.0), Facing, false, true);
		PC->SetControlRotation(FRotator(-15.0, Facing.Yaw + 50.0, 0.0));
		Hero->SetMoveInput(FVector2D::ZeroVector);
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots ladder: zone %d of scene %d, top %.0f, wall normal %s"), Ladder->ZoneIndex,
			Ladder->Scene ? Ladder->Scene->SceneIndex : -1, Ladder->GetTopHeight(), *Normal.ToString());
		LadderStart = Elapsed;
		LadderPhase = 1;
		return true;
	}

	const double T = Elapsed - LadderStart;
	if (Elapsed >= NextReport)
	{
		Report(LadderPhase == 1 ? TEXT("up") : TEXT("down"));
		NextReport = Elapsed + 0.25;
	}
	if (Elapsed >= NextBurst)
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("ladder_%02d.png"), BurstIndex++), false, false);
		NextBurst = Elapsed + 1.0;
	}
	if (LadderPhase == 1)
	{
		// Push towards the wall (camera-relative: the camera looks 50 degrees off the wall).
		if (T > 1.0)
		{
			Hero->SetMoveInput(FVector2D(-0.77, 0.64));
		}
		if (T > 2.0 && !Hero->IsClimbing() && Hero->GetCharacterMovement()->IsMovingOnGround())
		{
			Report(TEXT("climbed"));
			Hero->SetMoveInput(FVector2D::ZeroVector);
			LadderPhase = 2;
			LadderStart = Elapsed;
		}
	}
	else if (LadderPhase == 2)
	{
		// Back off the ledge over the ladder.
		if (T > 1.0)
		{
			Hero->SetMoveInput(FVector2D(0.77, -0.64));
		}
		if (T > 2.0 && !Hero->IsClimbing() && Hero->GetCharacterMovement()->IsMovingOnGround())
		{
			Report(TEXT("down"));
			Hero->SetMoveInput(FVector2D::ZeroVector);
			LadderPhase = 3;
			LadderStart = Elapsed;
		}
	}
	if (T > 20.0 || (LadderPhase == 3 && T > 1.5))
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots ladder: done (phase %d)"), LadderPhase);
		FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
		return false;
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickDoor(UWorld* World, float DeltaTime)
{
	APlayerController* PC = World->GetFirstPlayerController();
	ALBACharacter* Hero = PC ? Cast<ALBACharacter>(PC->GetPawn()) : nullptr;
	ALBASceneDoor* Door = nullptr;
	for (TActorIterator<ALBASceneDoor> It(World); It; ++It)
	{
		if (It->Scene && It->Scene->SceneIndex == DoorScene && It->ObjectIndex == DoorObject)
		{
			Door = *It;
		}
	}
	if (!Hero || !Door)
	{
		if (Elapsed > 20.0)
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots door: no hero or no door %d:%d in %s"), DoorScene, DoorObject, *World->GetMapName());
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		return true;
	}
	auto Report = [&](const TCHAR* Label)
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots door %s t=%.2f hero=(%.0f %.0f %.0f) door=(%.0f %.0f %.0f) open=%d"), Label, Elapsed - DoorStart,
			Hero->GetActorLocation().X, Hero->GetActorLocation().Y, Hero->GetActorLocation().Z,
			Door->GetActorLocation().X, Door->GetActorLocation().Y, Door->GetActorLocation().Z, Door->IsOpen() ? 1 : 0);
	};
	auto Shot = [&](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("door_%d_%d_%s.png"), DoorScene, DoorObject, Name), false, false);
	};

	const double T = Elapsed - DoorStart;
	if (DoorPhase == 0)
	{
		// Stand in front of the door, across its sliding direction, on a side with room for the hero.
		const FBox Box = Door->GetComponentsBoundingBox();
		const FVector Across = FVector(-Door->OpenOffset.Y, Door->OpenOffset.X, 0.0).GetSafeNormal();
		const double Gap = Hero->GetSimpleCollisionRadius() + 25.0;
		const double HalfDepth = FMath::Abs(Across.X) * Box.GetExtent().X + FMath::Abs(Across.Y) * Box.GetExtent().Y;
		FVector Spot = FVector::ZeroVector;
		for (const double Side : { 1.0, -1.0 })
		{
			const FVector Candidate = FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z + Hero->GetSimpleCollisionHalfHeight() + 5.0)
				+ Across * Side * (HalfDepth + Gap);
			FCollisionQueryParams Params(SCENE_QUERY_STAT(LBA2ShotsDoor), false, Hero);
			TArray<FOverlapResult> Overlaps;
			World->OverlapMultiByChannel(Overlaps, Candidate, FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeCapsule(Hero->GetSimpleCollisionRadius(), Hero->GetSimpleCollisionHalfHeight()), Params);
			for (const FOverlapResult& Overlap : Overlaps)
			{
				UE_LOG(LogLBA2, Display, TEXT("LBA2Shots door: side %.0f at %s overlaps %s"), Side, *Candidate.ToString(), *GetNameSafe(Overlap.GetComponent()));
			}
			if (!Overlaps.ContainsByPredicate([](const FOverlapResult& Overlap) { return Overlap.bBlockingHit; }))
			{
				Spot = Candidate;
				DoorTravel = -Across * Side;
				break;
			}
		}
		if (Spot.IsZero())
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots door: no room on either side of the door"));
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		const FRotator Facing(0.0, DoorTravel.Rotation().Yaw, 0.0);
		Hero->SetControlMode(ELBAControlMode::Authentic);
		Hero->TeleportTo(Spot, Facing, false, true);
		PC->SetControlRotation(FRotator(-20.0, Facing.Yaw + 35.0, 0.0));
		Hero->SetMoveInput(FVector2D::ZeroVector);
		DoorStart = Elapsed;
		DoorPhase = 1;
		Report(TEXT("placed"));
		return true;
	}
	if (Elapsed >= NextReport)
	{
		Report(TEXT("tick"));
		NextReport = Elapsed + 0.5;
	}
	switch (DoorPhase)
	{
	case 1: // Settle, then use the door (the Normal behaviour's action, like Space in authentic mode).
		if (T > 1.0)
		{
			Shot(TEXT("1_closed"));
			Hero->DoBehaviourAction();
			Report(TEXT("used"));
			DoorPhase = 2;
			DoorStart = Elapsed;
		}
		break;
	case 2: // Open: walk through.
		if (T > 2.0)
		{
			Shot(TEXT("2_open"));
			Hero->SetMoveInput(FVector2D(0.0, 1.0));
			DoorPhase = 3;
			DoorStart = Elapsed;
		}
		break;
	case 3: // Through and away: it closes behind.
		if (T > 4.0)
		{
			Hero->SetMoveInput(FVector2D::ZeroVector);
			Report(TEXT("away"));
			DoorPhase = 4;
			DoorStart = Elapsed;
		}
		break;
	case 4: // Look back at the door.
		if (T > 2.0)
		{
			Hero->SetActorRotation(FRotator(0.0, (-DoorTravel).Rotation().Yaw, 0.0));
			PC->SetControlRotation(FRotator(-20.0, (-DoorTravel).Rotation().Yaw + 35.0, 0.0));
			DoorPhase = 5;
			DoorStart = Elapsed;
		}
		break;
	case 5:
		if (T > 1.5)
		{
			Shot(TEXT("3_after"));
			// Leave: the door closes once the player is away.
			for (TActorIterator<APlayerStart> It(World); It; ++It)
			{
				Hero->TeleportTo(It->GetActorLocation(), Hero->GetActorRotation(), false, true);
				break;
			}
			Report(TEXT("left"));
			DoorPhase = 6;
			DoorStart = Elapsed;
		}
		break;
	case 6:
		if (T > 3.0)
		{
			Report(TEXT("done"));
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots door: done, %s at %s"), Door->IsOpen() ? TEXT("still open") : TEXT("closed"), *Door->GetActorLocation().ToString());
			DoorPhase = 7;
			DoorStart = Elapsed;
		}
		break;
	default:
		if (T > 1.0)
		{
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		break;
	}
	return true;
}

bool ULBA2SnapshotSubsystem::TickAttack(UWorld* World, float DeltaTime)
{
	APlayerController* PC = World->GetFirstPlayerController();
	ALBACharacter* Hero = PC ? Cast<ALBACharacter>(PC->GetPawn()) : nullptr;
	auto Shot = [&](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(ShotDirectory / FString::Printf(TEXT("attack_%d_%d_%s.png"), AttackScene, AttackObject, Name), false, false);
	};
	const double T = Elapsed - AttackStart;
	if (AttackPhase == 0)
	{
		int32 Enemies = 0, Actors = 0;
		for (TActorIterator<ALBASceneActor> It(World); It; ++It)
		{
			++Actors;
			Enemies += It->IsEnemy() ? 1 : 0;
			if (It->Scene && It->Scene->SceneIndex == AttackScene && It->ObjectIndex == AttackObject)
			{
				AttackTarget = *It;
			}
		}
		ALBASceneActor* Target = AttackTarget.Get();
		if (!Hero || !Target)
		{
			if (Elapsed > 20.0)
			{
				for (TActorIterator<ALBASceneActor> It(World); It; ++It)
				{
					UE_LOG(LogLBA2, Display, TEXT("LBA2Shots attack: %s scene %d object %d enemy=%d hidden=%d"), *It->GetName(), It->Scene ? It->Scene->SceneIndex : -1,
						It->ObjectIndex, It->IsEnemy() ? 1 : 0, It->IsHidden() ? 1 : 0);
				}
				UE_LOG(LogLBA2, Error, TEXT("LBA2Shots attack: no hero (%s) or no object %d:%d in %s"), *GetNameSafe(PC ? PC->GetPawn() : nullptr), AttackScene, AttackObject, *World->GetMapName());
				FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
				return false;
			}
			return true;
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots attack: %d enemies of %d scene actors; target %s (%s) enemy=%d"), Enemies, Actors, *Target->GetName(),
			Target->Entity ? *Target->Entity->GetName() : TEXT("none"), Target->IsEnemy() ? 1 : 0);

		// Stand next to it on a free side, turned 40 degrees away (the attack turns to it).
		const double Distance = Target->GetSimpleCollisionRadius() + Hero->GetSimpleCollisionRadius() + 30.0;
		const FVector Base = Target->GetActorLocation() - FVector(0.0, 0.0, Target->GetSimpleCollisionHalfHeight() - Hero->GetSimpleCollisionHalfHeight() - 5.0);
		FVector Spot = FVector::ZeroVector;
		for (int32 Side = 0; Side < 8 && Spot.IsZero(); ++Side)
		{
			const FVector Candidate = Base + FRotator(0.0, Side * 45.0, 0.0).Vector() * Distance;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(LBA2ShotsAttack), false, Hero);
			if (!World->OverlapBlockingTestByChannel(Candidate, FQuat::Identity, ECC_Pawn,
				FCollisionShape::MakeCapsule(Hero->GetSimpleCollisionRadius(), Hero->GetSimpleCollisionHalfHeight()), Params))
			{
				Spot = Candidate;
			}
		}
		if (Spot.IsZero())
		{
			UE_LOG(LogLBA2, Error, TEXT("LBA2Shots attack: no room around the target"));
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		const double ToTarget = (Target->GetActorLocation() - Spot).Rotation().Yaw;
		Hero->SetControlMode(ELBAControlMode::Modern);
		Hero->TeleportTo(Spot, FRotator(0.0, ToTarget + 40.0, 0.0), false, true);
		PC->SetControlRotation(FRotator(-20.0, ToTarget + 70.0, 0.0));
		Hero->SetMoveInput(FVector2D::ZeroVector);
		AttackStart = Elapsed;
		AttackPhase = 1;
		return true;
	}
	if (!Hero)
	{
		return true;
	}
	ALBASceneActor* Target = AttackTarget.Get();
	if (Elapsed >= NextReport)
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots attack t=%.2f phase=%d hero yaw=%.0f %s | target %s dead=%d visible=%d collision=%d"), T, AttackPhase,
			Hero->GetActorRotation().Yaw, *Hero->GetModernDebugString(), Target ? TEXT("alive") : TEXT("destroyed"), Target && Target->IsDead() ? 1 : 0,
			Target && Target->GetMesh()->IsVisible() ? 1 : 0, Target && Target->GetActorEnableCollision() ? 1 : 0);
		NextReport = Elapsed + 0.1;
	}
	switch (AttackPhase)
	{
	case 1: // Settle, then attack and press again once the first blow landed (it chains).
		if (T > 1.5)
		{
			Shot(TEXT("1_before"));
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots attack: attack %d"), Hero->Attack() ? 1 : 0);
			AttackPhase = 2;
			AttackStart = Elapsed;
		}
		break;
	case 2:
		if (T > 0.25)
		{
			Shot(TEXT("2_hit"));
			Hero->Attack();
			AttackPhase = 3;
			AttackStart = Elapsed;
		}
		break;
	case 3:
		if (T > 0.6)
		{
			Shot(TEXT("3_dying"));
			AttackPhase = 4;
			AttackStart = Elapsed;
		}
		break;
	case 4:
		if (T > 3.0)
		{
			Shot(TEXT("4_gone"));
			UE_LOG(LogLBA2, Display, TEXT("LBA2Shots attack: done, target %s"), Target ? TEXT("still there") : TEXT("gone"));
			AttackPhase = 5;
			AttackStart = Elapsed;
		}
		break;
	default:
		if (T > 1.0)
		{
			FPlatformMisc::RequestExit(false, TEXT("LBA2Shots"));
			return false;
		}
		break;
	}
	return true;
}

void ULBA2SnapshotSubsystem::SendPress(const FString& Press)
{
	if (Press.IsEmpty() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	FString KeyName, Value;
	if (Press.Equals(TEXT("MouseMove"), ESearchCase::IgnoreCase))
	{
		const FVector2D At = Slate.GetCursorPos();
		Slate.ProcessMouseMoveEvent(FPointerEvent(Slate.CursorPointerIndex, At, At, FVector2D(20.0, 0.0), TSet<FKey>(), FModifierKeysState()));
	}
	else if (Press.Split(TEXT("="), &KeyName, &Value))
	{
		Slate.ProcessAnalogInputEvent(FAnalogInputEvent(FKey(*KeyName), FModifierKeysState(), 0, false, 0, 0, FCString::Atof(*Value)));
	}
	else
	{
		const FKeyEvent Event(FKey(*Press), FModifierKeysState(), 0, false, 0, 0);
		Slate.ProcessKeyDownEvent(Event);
		Slate.ProcessKeyUpEvent(Event);
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2Shots: pressed %s"), *Press);
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
	if (bCamera)
	{
		return TickCamera(World, DeltaTime);
	}
	if (bDinofly)
	{
		return TickDinofly(World, DeltaTime);
	}
	if (bWind)
	{
		return TickWind(World, DeltaTime);
	}
	if (bGlobe)
	{
		return TickGlobe(World, DeltaTime);
	}
	if (bBoat)
	{
		return TickBoat(World, DeltaTime);
	}
	if (bBelt)
	{
		return TickBelt(World, DeltaTime);
	}
	if (bScenes)
	{
		return TickScenes(World, DeltaTime);
	}
	if (bDoor)
	{
		return TickDoor(World, DeltaTime);
	}
	if (bLadder)
	{
		return TickLadder(World, DeltaTime);
	}
	if (bAttack)
	{
		return TickAttack(World, DeltaTime);
	}
	if (bMenu)
	{
		return TickMenu(World, DeltaTime);
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
			Pawn->DisableInput(PC);
			PlaceHero(World, Pawn);
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
		SendPress(Poses[NextPose].Press);
	}
	else if (Elapsed - PoseStart > (NextPose == 0 ? 2.5 : PoseSettle))
	{
		const FString File = ShotDirectory / FString::Printf(TEXT("%s_%02d.png"), *World->GetMapName(), NextPose);
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogLBA2, Display, TEXT("LBA2Shots: %s at %s %s"), *File, *Poses[NextPose].Location.ToString(), *Poses[NextPose].Rotation.ToString());
		++NextPose;
		PoseStart = NextPose < Poses.Num() ? -1.0 : Elapsed;
	}
	return true;
}
