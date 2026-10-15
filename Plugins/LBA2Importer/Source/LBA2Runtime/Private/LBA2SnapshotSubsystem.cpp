#include "LBA2SnapshotSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/PlatformMisc.h"
#include "LBA2Runtime.h"
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
