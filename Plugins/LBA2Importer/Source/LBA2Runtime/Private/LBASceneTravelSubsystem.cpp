#include "LBASceneTravelSubsystem.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAIslandTravelSubsystem.h"
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

	bool GetIntTag(const FAssetData& Asset, FName Tag, int32& OutValue)
	{
		FString Value;
		if (!Asset.GetTagValue(Tag, Value) || Value.IsEmpty())
		{
			return false;
		}
		OutValue = FCString::Atoi(*Value);
		return true;
	}

	bool GetBoolTag(const FAssetData& Asset, FName Tag)
	{
		FString Value;
		return Asset.GetTagValue(Tag, Value) && Value.ToBool();
	}

	/** Island number of the scenes an island level holds (Citadel's storm and sunny levels: 0), -1 for other levels. */
	int32 GetLevelSceneIsland(const FString& LevelPackage)
	{
		for (const FLBAIslandInfo& Island : ULBAIslandTravelSubsystem::FindIslands())
		{
			if (Island.Level.ToSoftObjectPath().GetLongPackageName() == LevelPackage)
			{
				return Island.SceneIsland;
			}
		}
		return INDEX_NONE;
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

TArray<FLBASceneMenuEntry> ULBASceneTravelSubsystem::FindMenuScenes()
{
	TArray<FLBASceneMenuEntry> Entries;
	for (const FAssetData& Asset : FindSceneAssets())
	{
		FLBASceneMenuEntry Entry;
		if (!GetIntTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, SceneIndex), Entry.SceneIndex)
			|| !GetIntTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, MenuOrder), Entry.Order) || Entry.Order < 0
			|| GetBoolTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, bDemo)))
		{
			continue;
		}
		GetIntTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, Island), Entry.Island);
		GetIntTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, MenuParent), Entry.Parent);
		Asset.GetTagValue(GET_MEMBER_NAME_CHECKED(ULBASceneAsset, MenuName), Entry.Name);
		if (Entry.Name.IsEmpty())
		{
			Entry.Name = FString::Printf(TEXT("Scene %d"), Entry.SceneIndex);
		}
		Entry.bExterior = GetBoolTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, bExterior));
		Entries.Add(MoveTemp(Entry));
	}
	Entries.Sort([](const FLBASceneMenuEntry& A, const FLBASceneMenuEntry& B)
	{
		return A.Order != B.Order ? A.Order < B.Order : A.SceneIndex < B.SceneIndex;
	});
	return Entries;
}

int32 ULBASceneTravelSubsystem::GetCurrentScene() const
{
	const UWorld* World = GetGameInstance()->GetWorld();
	if (!World)
	{
		return INDEX_NONE;
	}
	TSet<FString> VisibleLevels;
	for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (Streaming && Streaming->IsLevelVisible())
		{
			VisibleLevels.Add(UWorld::RemovePIEPrefix(Streaming->GetWorldAssetPackageName()));
		}
	}
	const APlayerController* Player = GetGameInstance()->GetFirstLocalPlayerController(World);
	const APawn* Pawn = Player ? Player->GetPawn() : nullptr;

	// An island streams all its cubes: the one under the player. An interior level may hold several scenes (one
	// grid, several moments of the story): the one travelled to, else the first.
	int32 Interior = INDEX_NONE;
	for (const FAssetData& Asset : FindSceneAssets())
	{
		FString Level;
		int32 SceneIndex = INDEX_NONE;
		if (!Asset.GetTagValue(GET_MEMBER_NAME_CHECKED(ULBASceneAsset, Level), Level)
			|| !VisibleLevels.Contains(FSoftObjectPath(Level).GetLongPackageName())
			|| !GetIntTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, SceneIndex), SceneIndex))
		{
			continue;
		}
		if (GetBoolTag(Asset, GET_MEMBER_NAME_CHECKED(ULBASceneAsset, bExterior)))
		{
			const ULBASceneAsset* Scene = Pawn ? Cast<ULBASceneAsset>(Asset.GetAsset()) : nullptr;
			const FIntVector Local = Scene ? Scene->ToScene(Pawn->GetActorLocation()) : FIntVector(-1);
			if (Local.X >= 0 && Local.X < LBA::IslandCubeSize && Local.Z >= 0 && Local.Z < LBA::IslandCubeSize)
			{
				return SceneIndex;
			}
		}
		else if (SceneIndex == LastScene)
		{
			return SceneIndex;
		}
		else if (Interior == INDEX_NONE || SceneIndex < Interior)
		{
			Interior = SceneIndex;
		}
	}
	return Interior;
}

FString ULBASceneTravelSubsystem::GetLastIslandLevel(int32 Island) const
{
	const FString* Level = LastIslandLevels.Find(Island);
	return Level ? *Level : FString();
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

bool ULBASceneTravelSubsystem::TravelToSceneOnIsland(int32 SceneIndex, const TSoftObjectPtr<UWorld>& IslandLevel)
{
	const ULBASceneAsset* Scene = FindScene(SceneIndex);
	if (!Scene || Scene->Objects.IsEmpty())
	{
		UE_LOG(LogLBA2, Warning, TEXT("No scene %d (import the Scenes layer)."), SceneIndex);
		return false;
	}
	const FLBASceneObjectData& Hero = Scene->Objects[0];
	return TravelToSceneAt(SceneIndex, Hero.Position, Hero.Beta, true, IslandLevel.ToSoftObjectPath().GetLongPackageName());
}

FString ULBASceneTravelSubsystem::ChooseSceneryLevel(const ULBASceneAsset* Scene, const UWorld* World, const FString& PreferredLevel) const
{
	if (!PreferredLevel.IsEmpty())
	{
		for (const TSoftObjectPtr<UWorld>& Level : Scene->SceneryLevels)
		{
			if (Level.ToSoftObjectPath().GetLongPackageName() == PreferredLevel)
			{
				return PreferredLevel;
			}
		}
	}
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

bool ULBASceneTravelSubsystem::TravelToSceneAt(int32 SceneIndex, const FIntVector& Feet, int32 Beta, bool bSnapToGround, const FString& PreferredLevel)
{
	const ULBASceneAsset* Scene = FindScene(SceneIndex);
	UWorld* World = GetGameInstance()->GetWorld();
	if (!Scene || !World)
	{
		UE_LOG(LogLBA2, Warning, TEXT("No scene %d (import the Scenes layer)."), SceneIndex);
		return false;
	}
	// The island variant being left, then the one asked for, are remembered like a door taken from them: leaving an
	// interior comes back to the same variant.
	const FString CurrentLevel = GetWorldPackage(World);
	const int32 CurrentIsland = GetLevelSceneIsland(CurrentLevel);
	if (CurrentIsland != INDEX_NONE)
	{
		LastIslandLevels.Add(CurrentIsland, CurrentLevel);
	}
	if (!PreferredLevel.IsEmpty() && GetLevelSceneIsland(PreferredLevel) == Scene->Island)
	{
		LastIslandLevels.Add(Scene->Island, PreferredLevel);
	}
	const FString Level = ChooseSceneryLevel(Scene, World, PreferredLevel);
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
		LastScene = SceneIndex;
		UE_LOG(LogLBA2, Log, TEXT("Scene %d: teleported to %s"), SceneIndex, *FeetAt.ToString());
		return true;
	}

	UE_LOG(LogLBA2, Log, TEXT("Scene %d: opening %s"), SceneIndex, *Level);
	PendingArrival = Arrival;
	LastScene = SceneIndex;
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
