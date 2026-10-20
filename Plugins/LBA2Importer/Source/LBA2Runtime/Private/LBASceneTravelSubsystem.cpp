#include "LBASceneTravelSubsystem.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBASceneAsset.h"
#include "LBATypes.h"
#include "LBAZoneVolume.h"

namespace
{
	FAutoConsoleCommandWithWorldAndArgs SceneCommand(
		TEXT("lba.Scene"),
		TEXT("lba.Scene <number>: goes to a scene's hero start (43 = Citadel harbour, 0 = Twinsen's house). Without argument, lists the imported scenes."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ULBASceneTravelSubsystem* Travel = World && World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<ULBASceneTravelSubsystem>() : nullptr;
			if (!Travel)
			{
				return;
			}
			if (Args.IsEmpty())
			{
				TArray<FAssetData> Scenes = ULBASceneTravelSubsystem::FindSceneAssets();
				Scenes.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });
				for (const FAssetData& Scene : Scenes)
				{
					UE_LOG(LogLBA2, Display, TEXT("%s"), *Scene.AssetName.ToString());
				}
				return;
			}
			Travel->TravelToScene(FCString::Atoi(*Args[0]));
		}));

	FString GetWorldPackage(const UWorld* World)
	{
		return World ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) : FString();
	}
}

TArray<FAssetData> ULBASceneTravelSubsystem::FindSceneAssets()
{
	TArray<FAssetData> Assets;
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return Assets;
	}
	const FString& ScenesPath = GetDefault<ULBA2Settings>()->ScenesPath;
	static bool bScanned = false;
	if (!bScanned && !FPlatformProperties::RequiresCookedData())
	{
		// Uncooked standalone games start before the registry has gathered anything (cooked builds load it whole).
		Registry->ScanPathsSynchronous({ ScenesPath });
		bScanned = true;
	}
	FARFilter Filter;
	Filter.PackagePaths.Add(*ScenesPath);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(ULBASceneAsset::StaticClass()->GetClassPathName());
	Registry->GetAssets(Filter, Assets);
	return Assets;
}

ULBASceneAsset* ULBASceneTravelSubsystem::FindScene(int32 SceneIndex)
{
	const FString Wanted = FString::FromInt(SceneIndex);
	for (const FAssetData& Asset : FindSceneAssets())
	{
		FString Value;
		if (Asset.GetTagValue(GET_MEMBER_NAME_CHECKED(ULBASceneAsset, SceneIndex), Value) && Value == Wanted)
		{
			return Cast<ULBASceneAsset>(Asset.GetAsset());
		}
	}
	return nullptr;
}

bool ULBASceneTravelSubsystem::TravelToScene(int32 SceneIndex)
{
	const ULBASceneAsset* Scene = FindScene(SceneIndex);
	if (!Scene || Scene->Objects.IsEmpty())
	{
		UE_LOG(LogLBA2, Warning, TEXT("No scene %d (import the Scenes layer)."), SceneIndex);
		return false;
	}
	const FLBASceneObjectData& Hero = Scene->Objects[0];
	return TravelToSceneAt(SceneIndex, Hero.Position, Hero.Beta, true);
}

FString ULBASceneTravelSubsystem::ChooseSceneryLevel(const ULBASceneAsset* Scene, const UWorld* World) const
{
	const FString Current = GetWorldPackage(World);
	for (const TSoftObjectPtr<UWorld>& Level : Scene->SceneryLevels)
	{
		if (Level.ToSoftObjectPath().GetLongPackageName() == Current)
		{
			return Current;
		}
	}
	if (const FString* Last = LastIslandLevels.Find(Scene->Island); Last && Scene->bExterior)
	{
		for (const TSoftObjectPtr<UWorld>& Level : Scene->SceneryLevels)
		{
			if (Level.ToSoftObjectPath().GetLongPackageName() == *Last)
			{
				return *Last;
			}
		}
	}
	return Scene->SceneryLevels.IsEmpty() ? FString() : Scene->SceneryLevels[0].ToSoftObjectPath().GetLongPackageName();
}

bool ULBASceneTravelSubsystem::TravelToSceneAt(int32 SceneIndex, const FIntVector& Feet, int32 Beta, bool bSnapToGround)
{
	const ULBASceneAsset* Scene = FindScene(SceneIndex);
	UWorld* World = GetGameInstance()->GetWorld();
	if (!Scene || !World)
	{
		UE_LOG(LogLBA2, Warning, TEXT("No scene %d (import the Scenes layer)."), SceneIndex);
		return false;
	}
	const FString Level = ChooseSceneryLevel(Scene, World);
	if (Level.IsEmpty())
	{
		UE_LOG(LogLBA2, Warning, TEXT("Scene %d has no scenery level."), SceneIndex);
		return false;
	}

	FArrival Arrival;
	Arrival.LevelPackage = Level;
	Arrival.Feet = Scene->ToWorld(Feet);
	Arrival.Yaw = ULBASceneAsset::BetaToActorYaw(Beta);
	Arrival.bSnapToGround = bSnapToGround;

	APlayerController* Player = GetGameInstance()->GetFirstLocalPlayerController(World);
	APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	if (Level == GetWorldPackage(World) && Pawn)
	{
		// Same level (another cube of this island): teleport.
		const UCapsuleComponent* Capsule = Pawn->FindComponentByClass<UCapsuleComponent>();
		const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 90.f;
		const FVector FeetAt = bSnapToGround ? SnapFeet(World, Arrival.Feet, Pawn) : Arrival.Feet;
		const FRotator Rotation(0.0, Arrival.Yaw, 0.0);
		Pawn->TeleportTo(FeetAt + FVector(0.0, 0.0, HalfHeight + 2.0), Rotation);
		Player->SetControlRotation(Rotation);
		ArrivalTime = World->GetTimeSeconds();
		UE_LOG(LogLBA2, Log, TEXT("Scene %d: teleported to %s"), SceneIndex, *FeetAt.ToString());
		return true;
	}

	UE_LOG(LogLBA2, Log, TEXT("Scene %d: opening %s"), SceneIndex, *Level);
	PendingArrival = Arrival;
	UGameplayStatics::OpenLevel(World, FName(*Level));
	return true;
}

void ULBASceneTravelSubsystem::OnCubeChangeZone(ALBACubeChangeVolume* Zone, APawn* Pawn)
{
	UWorld* World = Zone ? Zone->GetWorld() : nullptr;
	const ULBASceneAsset* Source = Zone ? Zone->Scene.Get() : nullptr;
	const FLBASceneZoneData* Data = Zone ? Zone->GetZoneData() : nullptr;
	// Not on the overlaps the player spawns into (a level's start, an arrival): only zones walked into afterwards.
	if (!World || !Source || !Data || PendingArrival.IsSet() || World->GetTimeSeconds() < 1.0
		|| (ArrivalTime >= 0.0 && World->GetTimeSeconds() - ArrivalTime < 1.0))
	{
		return;
	}
	if (Source->bExterior)
	{
		LastIslandLevels.Add(Source->Island, GetWorldPackage(World));
	}

	// GereZoneChangeCube: the hero keeps his offset from the zone's corner, turned by the zone's quarter turns,
	// added to the arrival point (its height relative to the zone's floor).
	const UCapsuleComponent* Capsule = Pawn->FindComponentByClass<UCapsuleComponent>();
	const FVector FeetWorld = Pawn->GetActorLocation() - FVector(0.0, 0.0, Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.0);
	const FIntVector Hero = Source->ToScene(FeetWorld);
	const int32 Turn = (-Zone->ArrivalQuarterTurns * 1024) & (LBA::AngleFullTurn - 1);
	const double Radians = FMath::DegreesToRadians(LBA::AngleToDegrees(Turn));
	const double DX = Hero.X - Data->Min.X;
	const double DZ = Hero.Z - Data->Min.Z;
	const FIntVector Arrival(
		Zone->Arrival.X + FMath::RoundToInt(DX * FMath::Cos(Radians) + DZ * FMath::Sin(Radians)),
		Hero.Y - Data->Min.Y + Zone->Arrival.Y,
		Zone->Arrival.Z + FMath::RoundToInt(DZ * FMath::Cos(Radians) - DX * FMath::Sin(Radians)));
	const int32 Beta = ULBASceneAsset::ActorYawToBeta(Pawn->GetActorRotation().Yaw) + Turn;
	UE_LOG(LogLBA2, Log, TEXT("Cube change zone %d of scene %d -> scene %d at %s"), Zone->ZoneIndex, Source->SceneIndex, Zone->TargetScene, *Arrival.ToString());
	TravelToSceneAt(Zone->TargetScene, Arrival, Beta & (LBA::AngleFullTurn - 1), !Zone->bKeepArrivalHeight);
}

bool ULBASceneTravelSubsystem::ConsumeArrival(UWorld* World, float PawnHalfHeight, FTransform& OutTransform)
{
	if (!PendingArrival.IsSet() || PendingArrival->LevelPackage != GetWorldPackage(World))
	{
		PendingArrival.Reset();
		return false;
	}
	const FArrival Arrival = PendingArrival.GetValue();
	PendingArrival.Reset();
	ArrivalTime = World->GetTimeSeconds();
	const FVector Feet = Arrival.bSnapToGround ? SnapFeet(World, Arrival.Feet, nullptr) : Arrival.Feet;
	OutTransform = FTransform(FRotator(0.0, Arrival.Yaw, 0.0), Feet + FVector(0.0, 0.0, PawnHalfHeight + 2.0));
	return true;
}

FVector ULBASceneTravelSubsystem::SnapFeet(UWorld* World, const FVector& Feet, const AActor* Ignore)
{
	// From a little above the feet (a step), so arriving under a lintel or a roof does not land on top of it.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBAArrival), false, Ignore);
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Feet + FVector(0.0, 0.0, 40.0), Feet - FVector(0.0, 0.0, 400.0), ECC_Pawn, Params))
	{
		return Hit.ImpactPoint;
	}
	return Feet;
}
