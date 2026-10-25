#include "LBAPlanetSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/Volume.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "WaterBodyActor.h"
#include "WaterZoneActor.h"

namespace
{
	TAutoConsoleVariable<int32> CVarGlobe(TEXT("lba.Globe"), -1,
		TEXT("Globe mode for the next level opened: 0 off, 1 on (islands placed on a planet open on it), -1 the project setting (LBA2 > Globe)."));
	TAutoConsoleVariable<int32> CVarGlobeDebug(TEXT("lba.Globe.Debug"), 0,
		TEXT("1 prints the planet's state on screen, 2 also logs every move of the world."));
	TAutoConsoleVariable<int32> CVarGlobeTurns(TEXT("lba.Globe.Turns"), 1,
		TEXT("0 stops turning the world under the player (debug: the camera keeps correcting the tilt)."));

	/** Actors of island levels the planet replaces (tagged by the importer). */
	const FName SeaFloorTag(TEXT("LBA_SeaFloor"));
	const FName SeabedTag(TEXT("LBA_Seabed"));

	/** Islands whose caps the materials know (MPC_LBA_Planet, LBAPlanet.ush). */
	constexpr int32 MaxShaderCaps = 2;

	FAutoConsoleCommandWithWorldAndArgs GlobeOpenCommand(TEXT("lba.Globe.Open"),
		TEXT("Opens an island on its planet: lba.Globe.Open CITABAU"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (!World || Args.IsEmpty())
			{
				return;
			}
			const FName Island(*Args[0].ToUpper());
			for (const TSoftObjectPtr<ULBAPlanetAsset>& Soft : GetDefault<ULBA2Settings>()->Planets)
			{
				const ULBAPlanetAsset* Planet = Soft.LoadSynchronous();
				if (const FLBAPlanetIsland* Entry = Planet ? Planet->FindIsland(Island) : nullptr)
				{
					UGameplayStatics::OpenLevelBySoftObjectPtr(World, Entry->Level, true, TEXT("Globe"));
					return;
				}
			}
			UE_LOG(LogLBA2, Warning, TEXT("lba.Globe.Open: %s is on no planet (LBA2 settings > Globe > Planets)"), *Island.ToString());
		}));

	FAutoConsoleCommandWithWorldAndArgs GlobeRecenterCommand(TEXT("lba.Globe.Recenter"),
		TEXT("Turns the world under the player by this many degrees (default 1) about the axis to their right: the turn must not show."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (ULBAPlanetSubsystem* Planets = World ? World->GetSubsystem<ULBAPlanetSubsystem>() : nullptr)
			{
				Planets->DebugTurn(Args.IsEmpty() ? 1.f : FCString::Atof(*Args[0]));
			}
		}));

#if WITH_EDITOR
	FAutoConsoleCommandWithWorldAndArgs GlobeSeaErrorsCommand(TEXT("lba.Globe.SeaErrors"),
		TEXT("Logs the compile errors of the planet's sea material (debug)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			const ULBAPlanetSubsystem* Planets = World ? World->GetSubsystem<ULBAPlanetSubsystem>() : nullptr;
			UMaterialInterface* Sea = Planets && Planets->GetPlanetAsset() ? Planets->GetPlanetAsset()->SeaMaterial.LoadSynchronous() : nullptr;
			UMaterial* Material = Sea ? Sea->GetMaterial() : nullptr;
			FMaterialResource* Resource = Material ? Material->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
			if (!Resource)
			{
				UE_LOG(LogLBA2, Warning, TEXT("lba.Globe.SeaErrors: no sea material"));
				return;
			}
			Resource->FinishCompilation();
			UE_LOG(LogLBA2, Display, TEXT("lba.Globe.SeaErrors: %s, %d errors"), *Material->GetName(), Resource->GetCompileErrors().Num());
			for (const FString& Error : Resource->GetCompileErrors())
			{
				UE_LOG(LogLBA2, Display, TEXT("lba.Globe.SeaErrors: %s"), *Error);
			}
		}));
#endif

	bool IsWorldInfrastructure(const AActor* Actor)
	{
		return Actor->IsA<AController>() || Actor->IsA<APlayerCameraManager>() || Actor->IsA<AHUD>() || Actor->IsA<AWorldSettings>()
			|| Actor->IsA<AGameModeBase>() || Actor->IsA<AGameStateBase>() || Actor->IsA<APlayerState>();
	}

	/**
	 * Invisible markers of the island's levels (zones, starts, targets) only matter on the island: they stay where its
	 * level has them, so turns of the world skip them (half of a turn's cost on Citadel), and they are in place again
	 * whenever the island is (the player near it).
	 */
	bool StaysWithLevel(const AActor* Actor)
	{
		return Actor->IsA<AVolume>() || Actor->IsA<ATargetPoint>() || Actor->IsA<APlayerStart>();
	}

	FLinearColor ToParameter(const FVector& Vector, double W)
	{
		return FLinearColor(float(Vector.X), float(Vector.Y), float(Vector.Z), float(W));
	}
}

bool ULBAPlanetSubsystem::IsGlobeRequested(const UWorld* World)
{
	if (World && World->URL.HasOption(TEXT("NoGlobe")))
	{
		return false;
	}
	if (World && World->URL.HasOption(TEXT("Globe")))
	{
		return true;
	}
	const int32 Requested = CVarGlobe.GetValueOnGameThread();
	return Requested >= 0 ? Requested > 0 : GetDefault<ULBA2Settings>()->bGlobe;
}

void ULBAPlanetSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void ULBAPlanetSubsystem::Deinitialize()
{
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	Super::Deinitialize();
}

bool ULBAPlanetSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId ULBAPlanetSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(ULBAPlanetSubsystem, STATGROUP_Tickables);
}

void ULBAPlanetSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!IsGlobeRequested(&InWorld))
	{
		return;
	}
	const FString Map = UWorld::RemovePIEPrefix(InWorld.GetMapName());
	for (const TSoftObjectPtr<ULBAPlanetAsset>& Soft : GetDefault<ULBA2Settings>()->Planets)
	{
		ULBAPlanetAsset* Loaded = Soft.LoadSynchronous();
		for (int32 Index = 0; Loaded && Index < Loaded->Islands.Num(); ++Index)
		{
			if (Loaded->Islands[Index].Level.GetAssetName() == Map)
			{
				Activate(Loaded, Index);
				return;
			}
		}
	}
	UE_LOG(LogLBA2, Warning, TEXT("Globe mode: %s is on no planet (LBA2 settings > Globe > Planets)"), *Map);
}

void ULBAPlanetSubsystem::Activate(ULBAPlanetAsset* InAsset, int32 InAnchor)
{
	UWorld* World = GetWorld();
	Asset = InAsset;
	Anchor = InAnchor;
	bAnchored = true;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ALBAPlanetActor* Spawned = World->SpawnActor<ALBAPlanetActor>(ALBAPlanetActor::StaticClass(), GetAnchorPlanetToWorld(), Params);
	if (!Spawned)
	{
		return;
	}
	Planet = Spawned;
	Spawned->GetSea()->SetStaticMesh(Asset->SeaMesh.LoadSynchronous());
	Spawned->GetSea()->SetMaterial(0, Asset->SeaMaterial.LoadSynchronous());
	Asset->MaterialParameters.LoadSynchronous();

	for (ULevel* Level : World->GetLevels())
	{
		AdaptLevel(Level);
	}
	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(this, &ULBAPlanetSubsystem::OnLevelAdded);

	// The second sun is the first one's twin on the other side of the planet.
	if (UDirectionalLightComponent* First = Sun.Get())
	{
		UDirectionalLightComponent* Second = Spawned->GetSecondSun();
		Second->SetIntensity(First->Intensity);
		Second->SetLightColor(First->GetLightColor());
		Second->SetDynamicShadowDistanceMovableLight(First->DynamicShadowDistanceMovableLight);
		Second->SetWorldRotation((-First->GetForwardVector()).Rotation());
	}

	const FLBAPlanetIsland& Island = Asset->Islands[Anchor];
	UE_LOG(LogLBA2, Display, TEXT("Globe mode: %s on %s (radius %.0f m), latitude %.1f, longitude %.1f, heading %.1f, flat sea %.0f m, round from %.0f m"),
		*Island.Island.ToString(), *Asset->GetName(), Asset->Radius / 100.0, Island.Latitude, Island.Longitude, Island.Heading,
		Island.CapRadius / 100.0, Island.BlendRadius / 100.0);
}

void ULBAPlanetSubsystem::AdaptLevel(ULevel* Level)
{
	if (!Level || !Asset)
	{
		return;
	}
	const FLBAPlanetIsland& Island = Asset->Islands[Anchor];
	TArray<AActor*> Replaced;
	for (AActor* Actor : Level->Actors)
	{
		if (!Actor || Actor == Planet.Get())
		{
			continue;
		}
		if (Actor->IsA<AWaterBody>() || Actor->IsA<AWaterZone>() || Actor->ActorHasTag(SeaFloorTag))
		{
			Replaced.Add(Actor);
			continue;
		}
		AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor);
		if (MeshActor && Actor->ActorHasTag(SeabedTag))
		{
			if (UStaticMesh* Bent = Island.Seabed.LoadSynchronous())
			{
				MeshActor->GetStaticMeshComponent()->SetStaticMesh(Bent);
			}
		}

		TInlineComponentArray<USceneComponent*> Components(Actor);
		for (USceneComponent* Component : Components)
		{
			if (Component->Mobility != EComponentMobility::Movable)
			{
				Component->SetMobility(EComponentMobility::Movable);
			}
			// Nothing navigates yet, and every turn of the world would update the navigation octree for each mesh.
			if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
			{
				Primitive->SetCanEverAffectNavigation(false);
			}
			if (USkyAtmosphereComponent* Atmosphere = Cast<USkyAtmosphereComponent>(Component))
			{
				Atmosphere->TransformMode = ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;
				Atmosphere->SetBottomRadius(float(Asset->Radius / 100000.0));
				Atmosphere->SetAtmosphereHeight(Asset->AtmosphereHeight);
				Atmosphere->SetWorldLocation(GetPlanetToWorld().GetLocation());
				Atmosphere->MarkRenderStateDirty();
			}
			else if (UVolumetricCloudComponent* Clouds = Cast<UVolumetricCloudComponent>(Component))
			{
				Clouds->SetLayerBottomAltitude(Asset->CloudBottom);
				Clouds->SetLayerHeight(Asset->CloudHeight);
			}
			else if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Component))
			{
				if (!Sun.IsValid() && Light->IsUsedAsAtmosphereSunLight() && Light->GetAtmosphereSunLightIndex() == 0)
				{
					Sun = Light;
				}
			}
			else if (UExponentialHeightFogComponent* HeightFog = Cast<UExponentialHeightFogComponent>(Component))
			{
				Fog = HeightFog;
			}
		}
	}
	for (AActor* Actor : Replaced)
	{
		Actor->Destroy();
	}
}

void ULBAPlanetSubsystem::OnLevelAdded(ULevel* Level, UWorld* InWorld)
{
	if (InWorld != GetWorld() || !IsActive() || !Level)
	{
		return;
	}
	// Levels come in where the island has them: put them where the island is now.
	const FTransform LevelToWorld = GetLevelToWorld();
	if (!LevelToWorld.Equals(FTransform::Identity, 1e-6))
	{
		for (AActor* Actor : Level->Actors)
		{
			USceneComponent* Root = Actor ? Actor->GetRootComponent() : nullptr;
			if (Root && !Root->GetAttachParent() && !IsWorldInfrastructure(Actor) && !StaysWithLevel(Actor))
			{
				Root->SetMobility(EComponentMobility::Movable);
				Actor->SetActorTransform(Actor->GetActorTransform() * LevelToWorld, false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
	}
	AdaptLevel(Level);
}

APawn* ULBAPlanetSubsystem::GetPlayerPawn() const
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	return Controller ? Controller->GetPawn() : nullptr;
}

FTransform ULBAPlanetSubsystem::GetPlanetToWorld() const
{
	return Planet.IsValid() ? Planet->GetActorTransform() : FTransform::Identity;
}

FTransform ULBAPlanetSubsystem::GetAnchorPlanetToWorld() const
{
	return Asset->Islands[Anchor].GetLevelToPlanet(Asset->Radius).Inverse();
}

FTransform ULBAPlanetSubsystem::GetLevelToWorld() const
{
	return IsActive() ? Asset->Islands[Anchor].GetLevelToPlanet(Asset->Radius) * GetPlanetToWorld() : FTransform::Identity;
}

double ULBAPlanetSubsystem::GetSeaRadius(const FVector& PlanetDirection) const
{
	double SeaRadius = Asset->Radius;
	for (const FLBAPlanetIsland& Island : Asset->Islands)
	{
		const FLBAPlanetCap Cap = Island.GetCap(Asset->Radius);
		const double Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(PlanetDirection, Island.GetUp()), -1.0, 1.0));
		if (Angle < Cap.GetAngle(Cap.BlendRadius))
		{
			SeaRadius = FMath::Max(SeaRadius, Cap.GetSeaRadius(Angle));
		}
	}
	return SeaRadius;
}

bool ULBAPlanetSubsystem::GetSeaSurface(const FVector& Location, double& OutSurfaceZ) const
{
	if (!IsActive())
	{
		return false;
	}
	const FVector Local = GetPlanetToWorld().InverseTransformPosition(Location);
	const double Distance = Local.Size();
	if (Distance < 1.0)
	{
		return false;
	}
	// The world's vertical is the planet's up near the player (within the turn angle).
	OutSurfaceZ = Location.Z - (Distance - GetSeaRadius(Local / Distance));
	return true;
}

FVector ULBAPlanetSubsystem::GetUpAt(const FVector& Location) const
{
	if (!IsActive())
	{
		return FVector::UpVector;
	}
	const FTransform PlanetToWorld = GetPlanetToWorld();
	const FVector Direction = PlanetToWorld.InverseTransformPosition(Location).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return FVector::UpVector;
	}
	// Flat over an island's cap, bending to the sphere's normal across its ring.
	FVector Up = Direction;
	double Nearest = -1.0;
	for (const FLBAPlanetIsland& Island : Asset->Islands)
	{
		const double Dot = FVector::DotProduct(Direction, Island.GetUp());
		if (Dot > Nearest)
		{
			Nearest = Dot;
			const FLBAPlanetCap Cap = Island.GetCap(Asset->Radius);
			const double Roundness = Cap.GetRoundness(Cap.GetDistance(FMath::Acos(FMath::Clamp(Dot, -1.0, 1.0))));
			Up = FMath::Lerp(Island.GetUp(), Direction, Roundness).GetSafeNormal();
		}
	}
	return PlanetToWorld.TransformVectorNoScale(Up);
}

bool ULBAPlanetSubsystem::GetViewCorrection(FVector& OutPivot, FQuat& OutRotation) const
{
	const APawn* Pawn = IsActive() ? GetPlayerPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	OutPivot = Pawn->GetActorLocation();
	OutRotation = FQuat::FindBetweenNormals(FVector::UpVector, GetUpAt(OutPivot));
	return true;
}

void ULBAPlanetSubsystem::MoveWorld(const FTransform& Delta, bool bWithPawn)
{
	const double Start = FPlatformTime::Seconds();
	APawn* Pawn = GetPlayerPawn();
	int32 Moved = 0;
	// lba.Globe.Debug 3: where the time goes, by class.
	const bool bProfile = CVarGlobeDebug.GetValueOnGameThread() >= 3;
	TMap<const UClass*, TPair<int32, double>> Profile;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		USceneComponent* Root = Actor->GetRootComponent();
		if (!Root || Root->GetAttachParent() || IsWorldInfrastructure(Actor) || StaysWithLevel(Actor) || (Actor == Pawn && !bWithPawn))
		{
			continue;
		}
		const double ActorStart = bProfile ? FPlatformTime::Seconds() : 0.0;
		if (Root->Mobility != EComponentMobility::Movable)
		{
			Root->SetMobility(EComponentMobility::Movable);
		}
		Actor->SetActorTransform(Actor->GetActorTransform() * Delta, false, nullptr, ETeleportType::TeleportPhysics);
		++Moved;
		if (bProfile)
		{
			TPair<int32, double>& Entry = Profile.FindOrAdd(Actor->GetClass());
			++Entry.Key;
			Entry.Value += FPlatformTime::Seconds() - ActorStart;
		}
	}
	if (bProfile)
	{
		Profile.ValueSort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value > B.Value; });
		for (const TPair<const UClass*, TPair<int32, double>>& Entry : Profile)
		{
			UE_LOG(LogLBA2, Display, TEXT("Globe move profile: %s x%d %.2f ms"), *Entry.Key->GetName(), Entry.Value.Key, Entry.Value.Value * 1000.0);
		}
	}
	if (bWithPawn && Pawn)
	{
		if (UMovementComponent* Movement = Pawn->FindComponentByClass<UMovementComponent>())
		{
			Movement->Velocity = Delta.TransformVectorNoScale(Movement->Velocity);
		}
		if (AController* Controller = Pawn->GetController())
		{
			Controller->SetControlRotation((Delta.GetRotation() * Controller->GetControlRotation().Quaternion()).Rotator());
		}
		// Lagging cameras would fly across the jump: no lag for this frame.
		TInlineComponentArray<USpringArmComponent*> Arms(Pawn);
		for (USpringArmComponent* Arm : Arms)
		{
			if (Arm->bEnableCameraLag)
			{
				Arm->bEnableCameraLag = false;
				PausedLag.Add(Arm);
			}
		}
		PausedLagFrame = GFrameCounter;
	}

	++RecenterCount;
	LastMovedActors = Moved;
	LastMoveMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
	if (CVarGlobeDebug.GetValueOnGameThread() >= 2)
	{
		UE_LOG(LogLBA2, Display, TEXT("Globe: world moved (%s), %.2f deg, %.0f cm, %d actors in %.2f ms"), bWithPawn ? TEXT("with the player") : TEXT("about the player"),
			FMath::RadiansToDegrees(Delta.GetRotation().GetAngle()), Delta.GetTranslation().Size(), Moved, LastMoveMilliseconds);
	}
	OnRecentered.Broadcast(Delta);
}

void ULBAPlanetSubsystem::DebugTurn(float Degrees)
{
	const APawn* Pawn = IsActive() ? GetPlayerPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	const FVector Pivot = Pawn->GetActorLocation();
	const FVector Axis = FVector(Pawn->GetActorRightVector().X, Pawn->GetActorRightVector().Y, 0.0).GetSafeNormal();
	const FQuat Turn(Axis.IsNearlyZero() ? FVector::RightVector : Axis, FMath::DegreesToRadians(Degrees));
	bAnchored = false;
	MoveWorld(FTransform(-Pivot) * FTransform(Turn) * FTransform(Pivot), false);
}

void ULBAPlanetSubsystem::Tick(float DeltaTime)
{
	if (!IsActive())
	{
		return;
	}
	if (!PausedLag.IsEmpty() && PausedLagFrame != GFrameCounter)
	{
		for (const TWeakObjectPtr<USpringArmComponent>& Arm : PausedLag)
		{
			if (Arm.IsValid())
			{
				Arm->bEnableCameraLag = true;
			}
		}
		PausedLag.Reset();
	}

	APawn* Pawn = GetPlayerPawn();
	if (Pawn)
	{
		APlayerController* Controller = Cast<APlayerController>(Pawn->GetController());
		APlayerCameraManager* Camera = Controller ? Controller->PlayerCameraManager.Get() : nullptr;
		if (Camera && !Camera->FindCameraModifierByClass(ULBAPlanetCameraModifier::StaticClass()))
		{
			Camera->AddNewCameraModifier(ULBAPlanetCameraModifier::StaticClass());
		}
		if (CVarGlobeTurns.GetValueOnGameThread() != 0)
		{
			UpdateAnchor(Pawn);
		}
		UpdateSea(Pawn);
		UpdateLights(Pawn);
		UpdateFog(Pawn);
	}
	UpdateParameters();
#if ENABLE_DRAW_DEBUG
	if (CVarGlobeDebug.GetValueOnGameThread() > 0)
	{
		DrawDebug(Pawn);
	}
#endif
}

void ULBAPlanetSubsystem::UpdateAnchor(APawn* Pawn)
{
	const FLBAPlanetIsland& Island = Asset->Islands[Anchor];
	const FLBAPlanetCap Cap = Island.GetCap(Asset->Radius);
	const FTransform PlanetToWorld = GetPlanetToWorld();
	const FVector Pivot = Pawn->GetActorLocation();
	const FVector Direction = PlanetToWorld.InverseTransformPosition(Pivot).GetSafeNormal();
	const double Distance = Cap.GetDistance(FMath::Acos(FMath::Clamp(FVector::DotProduct(Direction, Island.GetUp()), -1.0, 1.0)));

	const double Enter = Cap.CapRadius + Asset->AnchorMargin;
	if (bAnchored && Distance > Enter + Asset->AnchorHysteresis)
	{
		bAnchored = false;
	}
	else if (!bAnchored && Distance < Enter)
	{
		bAnchored = true;
	}

	if (bAnchored)
	{
		// Back where the island's level has it, taking the player along.
		const FTransform Target = GetAnchorPlanetToWorld();
		const bool bInPlace = FVector::Dist(Target.GetLocation(), PlanetToWorld.GetLocation()) < 0.5
			&& Target.GetRotation().AngularDistance(PlanetToWorld.GetRotation()) < 1e-6;
		if (!bInPlace)
		{
			MoveWorld(PlanetToWorld.Inverse() * Target, true);
		}
		return;
	}

	const FVector Up = GetUpAt(Pivot);
	if (FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Up.Z, -1.0, 1.0))) > Asset->RecenterAngle)
	{
		const FQuat Turn = FQuat::FindBetweenNormals(Up, FVector::UpVector);
		MoveWorld(FTransform(-Pivot) * FTransform(Turn) * FTransform(Pivot), false);
	}
}

void ULBAPlanetSubsystem::UpdateSea(const APawn* Pawn)
{
	const FVector Center = GetPlanetToWorld().GetLocation();
	FVector Direction = (Pawn->GetActorLocation() - Center).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::UpVector;
	}
	Planet->GetSea()->SetWorldLocationAndRotation(Center + Direction * Asset->Radius, FRotationMatrix::MakeFromZ(Direction).ToQuat());
}

void ULBAPlanetSubsystem::UpdateLights(const APawn* Pawn)
{
	UDirectionalLightComponent* First = Sun.Get();
	if (!First)
	{
		return;
	}
	// A sun below the horizon only costs shadows (the atmosphere already darkens it): off until it rises.
	const FVector Up = GetUpAt(Pawn->GetActorLocation());
	const double FirstHeight = FVector::DotProduct(Up, -First->GetForwardVector());
	constexpr double BelowHorizon = -0.1;
	if (First->IsVisible() != (FirstHeight > BelowHorizon))
	{
		First->SetVisibility(FirstHeight > BelowHorizon);
	}
	UDirectionalLightComponent* Second = Planet->GetSecondSun();
	const bool bSecondUp = Asset->bSecondSun && -FirstHeight > BelowHorizon;
	if (Second->IsVisible() != bSecondUp)
	{
		Second->SetVisibility(bSecondUp);
	}
}

void ULBAPlanetSubsystem::UpdateFog(const APawn* Pawn)
{
	// The height fog is flat: it sits at the sea under the player.
	UExponentialHeightFogComponent* HeightFog = Fog.Get();
	double SeaZ = 0.0;
	const FVector Location = Pawn->GetActorLocation();
	if (HeightFog && GetSeaSurface(Location, SeaZ) && FMath::Abs(HeightFog->GetComponentLocation().Z - SeaZ) > 1.0)
	{
		HeightFog->SetWorldLocation(FVector(Location.X, Location.Y, SeaZ));
	}
}

void ULBAPlanetSubsystem::UpdateParameters()
{
	UMaterialParameterCollection* Collection = Asset->MaterialParameters.Get();
	UMaterialParameterCollectionInstance* Instance = Collection ? GetWorld()->GetParameterCollectionInstance(Collection) : nullptr;
	if (!Instance)
	{
		return;
	}
	const FTransform PlanetToWorld = GetPlanetToWorld();
	const UStaticMeshComponent* Sea = Planet->GetSea();
	const FVector SeaCenter = Sea->GetComponentLocation();
	const FQuat SeaRotation = Sea->GetComponentQuat();

	// The sea disc's frame, and the planet's center relative to it (precise: the shader works around the disc).
	Instance->SetVectorParameterValue(TEXT("PlanetSea"), ToParameter(SeaCenter, Asset->Radius));
	Instance->SetVectorParameterValue(TEXT("PlanetSeaX"), ToParameter(SeaRotation.GetAxisX(), 0.0));
	Instance->SetVectorParameterValue(TEXT("PlanetSeaY"), ToParameter(SeaRotation.GetAxisY(), 0.0));
	Instance->SetVectorParameterValue(TEXT("PlanetSeaZ"), ToParameter(SeaRotation.GetAxisZ(), 0.0));
	Instance->SetVectorParameterValue(TEXT("PlanetCenter"), ToParameter(PlanetToWorld.GetLocation() - SeaCenter, 1.0));

	// Last frame, for the sea's motion vectors (LBAPlanetSeaPreviousOffset): the disc's axes then, and the world's move
	// since, from now to then, relative to the discs' centers.
	if (!bHasPreviousSea)
	{
		PreviousPlanetToWorld = PlanetToWorld;
		PreviousSeaCenter = SeaCenter;
		PreviousSeaRotation = SeaRotation;
		bHasPreviousSea = true;
	}
	const FTransform Move = PlanetToWorld.Inverse() * PreviousPlanetToWorld;
	const FMatrix MoveMatrix = Move.ToMatrixNoScale();
	const FVector MoveOffset = Move.TransformPosition(SeaCenter) - PreviousSeaCenter;
	Instance->SetVectorParameterValue(TEXT("PlanetSeaXPrev"), ToParameter(PreviousSeaRotation.GetAxisX(), 0.0));
	Instance->SetVectorParameterValue(TEXT("PlanetSeaYPrev"), ToParameter(PreviousSeaRotation.GetAxisY(), 0.0));
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Instance->SetVectorParameterValue(*FString::Printf(TEXT("PlanetPrevious%c"), TEXT('X') + Axis),
			FLinearColor(float(MoveMatrix.M[0][Axis]), float(MoveMatrix.M[1][Axis]), float(MoveMatrix.M[2][Axis]), float(MoveOffset[Axis])));
	}
	PreviousPlanetToWorld = PlanetToWorld;
	PreviousSeaCenter = SeaCenter;
	PreviousSeaRotation = SeaRotation;

	for (int32 Index = 0; Index < MaxShaderCaps; ++Index)
	{
		const FLBAPlanetIsland* Island = Asset->Islands.IsValidIndex(Index) ? &Asset->Islands[Index] : nullptr;
		Instance->SetVectorParameterValue(*FString::Printf(TEXT("PlanetCap%d"), Index),
			Island ? ToParameter(PlanetToWorld.TransformVectorNoScale(Island->GetUp()), Island->CapRadius) : FLinearColor(0.f, 0.f, 1.f, 0.f));
		Instance->SetVectorParameterValue(*FString::Printf(TEXT("PlanetCapEdge%d"), Index),
			Island ? FLinearColor(FMath::Max(Island->BlendRadius, Island->CapRadius), Island ? 1.f : 0.f, 0.f, 0.f) : FLinearColor::Transparent);
	}

	// World to the anchor island's level, where surface patterns (dither dots, cartoon sea) are laid out, so they
	// stay put when the world turns. Identity (and off) while the world is where the level has it.
	const FMatrix WorldToLevel = GetLevelToWorld().Inverse().ToMatrixNoScale();
	const bool bPattern = !WorldToLevel.Equals(FMatrix::Identity, 1e-6);
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Instance->SetVectorParameterValue(*FString::Printf(TEXT("PlanetPattern%c"), TEXT('X') + Axis),
			FLinearColor(float(WorldToLevel.M[0][Axis]), float(WorldToLevel.M[1][Axis]), float(WorldToLevel.M[2][Axis]), float(WorldToLevel.M[3][Axis])));
	}
	const FLBAPlanetIsland& Island = Asset->Islands[Anchor];
	Instance->SetVectorParameterValue(TEXT("PlanetPattern"), FLinearColor(float(Island.Center.X), float(Island.Center.Y), float(Asset->Radius), bPattern ? 1.f : 0.f));
	Instance->SetVectorParameterValue(TEXT("PlanetPatternCap"), FLinearColor(Island.CapRadius, FMath::Max(Island.BlendRadius, Island.CapRadius), 1.f, 0.f));
}

void ULBAPlanetSubsystem::DrawDebug(const APawn* Pawn) const
{
	if (!GEngine || !Pawn)
	{
		return;
	}
	const FLBAPlanetIsland& Island = Asset->Islands[Anchor];
	const FTransform PlanetToWorld = GetPlanetToWorld();
	const FVector Location = Pawn->GetActorLocation();
	const FVector Local = PlanetToWorld.InverseTransformPosition(Location);
	const double Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(Local.GetSafeNormal(), Island.GetUp()), -1.0, 1.0));
	double SeaZ = 0.0;
	GetSeaSurface(Location, SeaZ);
	const FVector Up = GetUpAt(Location);
	GEngine->AddOnScreenDebugMessage(uint64(0x6C6261676C6F6265), 0.f, FColor::Cyan, FString::Printf(
		TEXT("Globe %s: %s, %.0f m from the island (%.2f deg), %.1f m above the sea, tilt %.2f deg, turns %d (last %d actors, %.2f ms)"),
		*Island.Island.ToString(), bAnchored ? TEXT("anchored") : TEXT("at sea"), Island.GetCap(Asset->Radius).GetDistance(Angle) / 100.0,
		FMath::RadiansToDegrees(Angle), (Location.Z - SeaZ) / 100.0, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Up.Z, -1.0, 1.0))),
		RecenterCount, LastMovedActors, LastMoveMilliseconds));
}

bool ULBAPlanetCameraModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	const UWorld* World = GetWorld();
	const ULBAPlanetSubsystem* Planets = World ? World->GetSubsystem<ULBAPlanetSubsystem>() : nullptr;
	FVector Pivot;
	FQuat Correction;
	if (Planets && Planets->GetViewCorrection(Pivot, Correction))
	{
		InOutPOV.Location = Pivot + Correction.RotateVector(InOutPOV.Location - Pivot);
		InOutPOV.Rotation = (Correction * InOutPOV.Rotation.Quaternion()).Rotator();
	}
	return false;
}
