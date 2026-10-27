#include "LBAPlanetSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAStorm.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "NiagaraActor.h"
#include "WaterBodyActor.h"
#include "WaterZoneActor.h"

namespace
{
	TAutoConsoleVariable<int32> CVarGlobe(TEXT("lba.Globe"), -1,
		TEXT("Globe mode for the next level opened: 0 off, 1 on (islands placed on a planet open on it), -1 the project setting (LBA2 > Globe)."));
	TAutoConsoleVariable<int32> CVarGlobeDebug(TEXT("lba.Globe.Debug"), 0,
		TEXT("1 prints where the camera is on the planet on screen."));
	TAutoConsoleVariable<int32> CVarGlobeBend(TEXT("lba.Globe.Bend"), 1,
		TEXT("0: the view isn't bent onto the planet (the flat world as it is, to compare)."));
	TAutoConsoleVariable<float> CVarGlobeFlatRadius(TEXT("lba.Globe.FlatRadius"), -1.f,
		TEXT("Overrides how far around the camera the world stays flat (m; negative: the planet's)."));
	TAutoConsoleVariable<float> CVarGlobeRadius(TEXT("lba.Globe.Radius"), -1.f,
		TEXT("Overrides the planet's radius for the next level opened (m; negative: the planet's): the islands' places and the bend."));
	TAutoConsoleVariable<float> CVarGlobeSkyLightGain(TEXT("lba.Globe.SkyLightGain"), -1.f,
		TEXT("Overrides the opened island's sky light gain on the planet (debug, for tuning; negative: the island's, see FLBAPlanetIsland)."));
	TAutoConsoleVariable<int32> CVarGlobeIslands(TEXT("lba.Globe.Islands"), 1,
		TEXT("0: the planet's other islands aren't streamed in (debug, for the next level opened)."));
	TAutoConsoleVariable<int32> CVarGlobeSea(TEXT("lba.Globe.Sea"), 1,
		TEXT("The sea for the next level opened: 1 the opened island's sea level (Water plugin oceans), the planet's sea disc beyond; 0 the sea disc alone."));
	TAutoConsoleVariable<int32> CVarGlobeBelt(TEXT("lba.Globe.Belt"), 1,
		TEXT("0: the planet's belt (Twinsun's Hamalayi mountains) isn't streamed in (for the next level opened)."));

	/**
	 * Under the sea level's water, the sea disc lies this much lower (cm), out of sight: far from the camera, the water
	 * mesh's large tiles slope down towards the coasts (their vertices on land are pulled down), meters below the sea.
	 */
	TAutoConsoleVariable<float> CVarGlobeSeaDiscDrop(TEXT("lba.Globe.SeaDiscDrop"), 1000.f,
		TEXT("How much lower the sea disc lies under the sea level's water (cm; debug, for tuning)."));

	/** Actors of island levels the planet replaces (tagged by the importer). */
	const FName SeaFloorTag(TEXT("LBA_SeaFloor"));

	/** The islands' sea level (world Z, cm): their levels have it at 0, and they are laid side by side. */
	constexpr double SeaLevel = 0.0;

	/** The camera jumped (a cut, a teleport): last frame's view is not where this one came from. */
	constexpr double ViewJump = 5000.0;

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

	FAutoConsoleCommandWithWorldAndArgs GlobeCloudsCommand(TEXT("lba.Globe.Clouds"),
		TEXT("Sets the cloud layer: lba.Globe.Clouds BOTTOM HEIGHT (km above the sea; debug, for tuning)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			for (TObjectIterator<UVolumetricCloudComponent> It; It && World; ++It)
			{
				if (It->GetWorld() != World)
				{
					continue;
				}
				if (Args.Num() >= 2)
				{
					It->SetLayerBottomAltitude(FCString::Atof(*Args[0]));
					It->SetLayerHeight(FCString::Atof(*Args[1]));
				}
				UE_LOG(LogLBA2, Display, TEXT("lba.Globe.Clouds: %s, layer from %.2f km, %.2f km high, tracing start max %.1f km, max %.1f km"),
					*It->GetOwner()->GetName(), It->LayerBottomAltitude, It->LayerHeight, It->TracingStartMaxDistance, It->TracingMaxDistance);
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

	/**
	 * What an island level has for itself alone (its sky, lights, look, weather, starts): the opened island's are the
	 * planet's, so the other islands' go. Rain is the only effect island levels have.
	 */
	bool IsIslandEnvironment(const AActor* Actor)
	{
		return Actor->IsA<APostProcessVolume>() || Actor->IsA<APlayerStart>() || Actor->IsA<ALBAStorm>() || Actor->IsA<ANiagaraActor>()
			|| Actor->FindComponentByClass<UDirectionalLightComponent>() || Actor->FindComponentByClass<USkyLightComponent>()
			|| Actor->FindComponentByClass<USkyAtmosphereComponent>() || Actor->FindComponentByClass<UVolumetricCloudComponent>()
			|| Actor->FindComponentByClass<UExponentialHeightFogComponent>();
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

void ULBAPlanetSubsystem::Activate(ULBAPlanetAsset* InAsset, int32 InHome)
{
	UWorld* World = GetWorld();
	Asset = InAsset;
	Home = InHome;
	const float RadiusOverride = CVarGlobeRadius.GetValueOnGameThread();
	Radius = RadiusOverride > 0.f ? RadiusOverride * 100.0 : Asset->Radius;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ALBAPlanetActor* Spawned = World->SpawnActor<ALBAPlanetActor>(ALBAPlanetActor::StaticClass(), FTransform::Identity, Params);
	if (!Spawned)
	{
		return;
	}
	Planet = Spawned;
	bWaterSea = CVarGlobeSea.GetValueOnGameThread() != 0 && !Asset->Islands[Home].SeaLevel.IsNull();
	Spawned->GetSea()->SetStaticMesh(Asset->SeaMesh.LoadSynchronous());
	Spawned->GetSea()->SetMaterial(0, Asset->SeaMaterial.LoadSynchronous());
	Asset->MaterialParameters.LoadSynchronous();

	for (ULevel* Level : World->GetLevels())
	{
		AdaptLevel(Level, Home);
	}
	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(this, &ULBAPlanetSubsystem::OnLevelAdded);
	LoadIslands();

	const FLBAPlanetIsland& Island = Asset->Islands[Home];
	const FVector2D Map = Island.GetMapLocation(Radius);
	UE_LOG(LogLBA2, Display, TEXT("Globe mode: %s on %s (radius %.0f m), latitude %.1f, longitude %.1f, heading %.1f, at (%.0f, %.0f) m on the map, flat %.0f m around the camera"),
		*Island.Island.ToString(), *Asset->GetName(), Radius / 100.0, Island.Latitude, Island.Longitude, Island.Heading,
		Map.X / 100.0, Map.Y / 100.0, Asset->FlatRadius / 100.0);
	if (Radius != Asset->Radius)
	{
		UE_LOG(LogLBA2, Display, TEXT("Globe mode: radius %.0f m instead of %.0f m (lba.Globe.Radius)"), Radius / 100.0, Asset->Radius / 100.0);
	}
}

void ULBAPlanetSubsystem::AdaptLevel(ULevel* Level, int32 IslandIndex)
{
	if (!Level || !Asset)
	{
		return;
	}
	const FLBAPlanetIsland& Island = Asset->Islands[IslandIndex];
	TArray<AActor*> Replaced;
	for (AActor* Actor : Level->Actors)
	{
		if (!Actor || Actor == Planet.Get())
		{
			continue;
		}
		if (Actor->IsA<AWaterBody>() || Actor->IsA<AWaterZone>() || Actor->ActorHasTag(SeaFloorTag)
			|| (IslandIndex != Home && IsIslandEnvironment(Actor)))
		{
			Replaced.Add(Actor);
			continue;
		}
		TInlineComponentArray<USceneComponent*> Components(Actor);
		for (USceneComponent* Component : Components)
		{
			if (USkyAtmosphereComponent* SkyAtmosphere = Cast<USkyAtmosphereComponent>(Component))
			{
				// Kept under the camera (UpdateView).
				SkyAtmosphere->SetMobility(EComponentMobility::Movable);
				SkyAtmosphere->TransformMode = ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;
				SkyAtmosphere->SetBottomRadius(float(Radius / 100000.0));
				SkyAtmosphere->SetAtmosphereHeight(Asset->AtmosphereHeight);
				SkyAtmosphere->SetWorldLocation(FVector(0.0, 0.0, SeaLevel - Radius));
				SkyAtmosphere->MarkRenderStateDirty();
				Atmosphere = SkyAtmosphere;
			}
			else if (UVolumetricCloudComponent* Clouds = Cast<UVolumetricCloudComponent>(Component))
			{
				if (Asset->CloudBottom > 0.f && Asset->CloudHeight > 0.f)
				{
					Clouds->SetLayerBottomAltitude(Asset->CloudBottom);
					Clouds->SetLayerHeight(Asset->CloudHeight);
				}
			}
			else if (USkyLightComponent* SkyLight = Cast<USkyLightComponent>(Component))
			{
				const float Override = CVarGlobeSkyLightGain.GetValueOnGameThread();
				SkyLight->SetIntensity(SkyLight->Intensity * (Override >= 0.f ? Override : Island.SkyLightGain));
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
	if (InWorld != GetWorld() || !IsActive() || !Level || (SeaStream.IsValid() && SeaStream->GetLoadedLevel() == Level))
	{
		return;
	}
	if (BeltStream.IsValid() && BeltStream->GetLoadedLevel() == Level)
	{
		UE_LOG(LogLBA2, Display, TEXT("Globe mode: the belt streamed in, %d actors"), Level->Actors.Num());
		return;
	}
	// The other islands' levels come in where they are on the map; the opened island's sub-levels are its own.
	int32 IslandIndex = Home;
	for (int32 Index = 0; Index < IslandStreams.Num(); ++Index)
	{
		if (IslandStreams[Index].IsValid() && IslandStreams[Index]->GetLoadedLevel() == Level)
		{
			IslandIndex = Index;
		}
	}
	AdaptLevel(Level, IslandIndex);
	if (IslandIndex != Home)
	{
		UE_LOG(LogLBA2, Display, TEXT("Globe mode: %s streamed in, %d actors"), *Asset->Islands[IslandIndex].Island.ToString(), Level->Actors.Num());
	}
}

void ULBAPlanetSubsystem::LoadIslands()
{
	if (bWaterSea)
	{
		bool bLoaded = false;
		const TSoftObjectPtr<UWorld>& Sea = Asset->Islands[Home].SeaLevel;
		SeaStream = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(GetWorld(), Sea, FTransform::Identity, bLoaded,
			Sea.GetAssetName() + TEXT("_Instance"));
		if (!bLoaded)
		{
			UE_LOG(LogLBA2, Warning, TEXT("Globe mode: couldn't stream in the sea (%s)"), *Sea.ToString());
			bWaterSea = false;
		}
	}
	if (!Asset->BeltLevel.IsNull() && CVarGlobeBelt.GetValueOnGameThread() != 0)
	{
		if (Radius != Asset->Radius)
		{
			// Its shape is laid on the map for the asset's radius.
			UE_LOG(LogLBA2, Display, TEXT("Globe mode: no belt at a radius of %.0f m (it is built for %.0f m)"), Radius / 100.0, Asset->Radius / 100.0);
		}
		else
		{
			bool bLoaded = false;
			BeltStream = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(GetWorld(), Asset->BeltLevel, GetMapToWorld(), bLoaded,
				Asset->BeltLevel.GetAssetName() + TEXT("_Instance"));
			if (!bLoaded)
			{
				UE_LOG(LogLBA2, Warning, TEXT("Globe mode: couldn't stream in the belt (%s)"), *Asset->BeltLevel.ToString());
			}
		}
	}
	IslandStreams.SetNum(Asset->Islands.Num());
	for (int32 Index = 0; Index < Asset->Islands.Num(); ++Index)
	{
		const FLBAPlanetIsland& Island = Asset->Islands[Index];
		if (Index == Home || Island.Level.IsNull() || CVarGlobeIslands.GetValueOnGameThread() == 0)
		{
			continue;
		}
		bool bLoaded = false;
		IslandStreams[Index] = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(GetWorld(), Island.Level, GetIslandToWorld(Index), bLoaded,
			Island.Level.GetAssetName() + TEXT("_Globe"));
		if (!bLoaded)
		{
			UE_LOG(LogLBA2, Warning, TEXT("Globe mode: couldn't stream in %s (%s)"), *Island.Island.ToString(), *Island.Level.ToString());
		}
	}
}

FTransform ULBAPlanetSubsystem::GetIslandToWorld(int32 Island) const
{
	if (!IsActive() || !Asset->Islands.IsValidIndex(Island))
	{
		return FTransform::Identity;
	}
	return Asset->Islands[Island].GetLevelToMap(Radius) * GetMapToWorld();
}

FTransform ULBAPlanetSubsystem::GetMapToWorld() const
{
	return IsActive() ? Asset->Islands[Home].GetLevelToMap(Radius).Inverse() : FTransform::Identity;
}

FVector ULBAPlanetSubsystem::GetIslandCenter(int32 Island) const
{
	return IsActive() && Asset->Islands.IsValidIndex(Island)
		? GetIslandToWorld(Island).TransformPosition(FVector(Asset->Islands[Island].Center, SeaLevel)) : FVector::ZeroVector;
}

int32 ULBAPlanetSubsystem::FindNearestIsland(const FVector& Location, double& OutDistance) const
{
	int32 Nearest = INDEX_NONE;
	OutDistance = TNumericLimits<double>::Max();
	for (int32 Index = 0; IsActive() && Index < Asset->Islands.Num(); ++Index)
	{
		const double Distance = FVector::Dist2D(Location, GetIslandCenter(Index));
		if (Distance < OutDistance)
		{
			OutDistance = Distance;
			Nearest = Index;
		}
	}
	return Nearest;
}

bool ULBAPlanetSubsystem::GetSeaSurface(const FVector& Location, double& OutSurfaceZ) const
{
	if (!IsActive())
	{
		return false;
	}
	OutSurfaceZ = SeaLevel;
	return true;
}

void ULBAPlanetSubsystem::Tick(float DeltaTime)
{
	if (!IsActive())
	{
		return;
	}
	// The bend follows the camera's final view, so it is updated by a camera modifier, after everything moved.
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	APlayerCameraManager* Camera = Controller ? Controller->PlayerCameraManager.Get() : nullptr;
	if (Camera && !Camera->FindCameraModifierByClass(ULBAPlanetCameraModifier::StaticClass()))
	{
		Camera->AddNewCameraModifier(ULBAPlanetCameraModifier::StaticClass());
	}
#if ENABLE_DRAW_DEBUG
	if (CVarGlobeDebug.GetValueOnGameThread() > 0)
	{
		DrawDebug();
	}
#endif
}

void ULBAPlanetSubsystem::UpdateView(const FVector& ViewLocation, bool bCameraCut)
{
	if (!IsActive())
	{
		return;
	}
	if (!bHasView || bCameraCut || FVector::Dist(ViewLocation, View) > ViewJump)
	{
		View = ViewLocation;
		PreviousSea = FVector(ViewLocation.X, ViewLocation.Y, SeaLevel);
	}
	PreviousView = View;
	View = ViewLocation;
	bHasView = true;

	// The sea disc and the atmosphere's planet under the camera.
	const FVector Sea(View.X, View.Y, SeaLevel);
	Planet->GetSea()->SetWorldLocation(Sea - FVector(0.0, 0.0, bWaterSea ? CVarGlobeSeaDiscDrop.GetValueOnGameThread() : 0.0));
	if (USkyAtmosphereComponent* SkyAtmosphere = Atmosphere.Get())
	{
		SkyAtmosphere->SetWorldLocation(FVector(View.X, View.Y, SeaLevel - Radius));
	}

	UMaterialParameterCollection* Collection = Asset->MaterialParameters.Get();
	UMaterialParameterCollectionInstance* Instance = Collection ? GetWorld()->GetParameterCollectionInstance(Collection) : nullptr;
	if (Instance)
	{
		const float FlatRadius = CVarGlobeFlatRadius.GetValueOnGameThread();
		const bool bBend = CVarGlobeBend.GetValueOnGameThread() != 0;
		Instance->SetVectorParameterValue(TEXT("PlanetBend"), FLinearColor(float(Radius),
			FlatRadius >= 0.f ? FlatRadius * 100.f : Asset->FlatRadius, float(SeaLevel), bBend ? 1.f : 0.f));
		Instance->SetVectorParameterValue(TEXT("PlanetView"), ToParameter(View, 0.0));
		Instance->SetVectorParameterValue(TEXT("PlanetViewPrevious"), ToParameter(PreviousView, 0.0));
		// The disc's move since last frame: its vertices aren't the same points of the sea from frame to frame.
		Instance->SetVectorParameterValue(TEXT("PlanetSeaMove"), ToParameter(Sea - PreviousSea, 0.0));
	}
	PreviousSea = Sea;
}

void ULBAPlanetSubsystem::DrawDebug() const
{
	if (!GEngine || !bHasView)
	{
		return;
	}
	double Distance = 0.0;
	const int32 Nearest = FindNearestIsland(View, Distance);
	const double FlatRadius = CVarGlobeFlatRadius.GetValueOnGameThread() >= 0.f ? CVarGlobeFlatRadius.GetValueOnGameThread() * 100.0 : Asset->FlatRadius;
	const double Beyond = FMath::Max(Distance - FlatRadius, 0.0);
	// How far the nearest island's center sinks (cm), and the sea horizon's distance at the camera's height (where the
	// view grazes the sea: flat, then a parabola for small angles).
	const double Drop = Radius * (1.0 - FMath::Cos(FMath::Min(Beyond / Radius, UE_DOUBLE_PI)));
	const double Height = FMath::Max(View.Z - SeaLevel, 0.0);
	const double Horizon = FMath::Sqrt(FMath::Square(FlatRadius) + 2.0 * Radius * Height);
	GEngine->AddOnScreenDebugMessage(uint64(0x6C6261676C6F6265), 0.f, FColor::Cyan, FString::Printf(
		TEXT("Globe: camera %.0f m above the sea, %.0f m from %s (sunk %.1f m), sea horizon %.0f m away"),
		Height / 100.0, Distance / 100.0, Nearest != INDEX_NONE ? *Asset->Islands[Nearest].Island.ToString() : TEXT("-"), Drop / 100.0, Horizon / 100.0));
}

ULBAPlanetCameraModifier::ULBAPlanetCameraModifier()
{
	// Last: the bend follows the view the camera ends up with.
	Priority = 255;
}

bool ULBAPlanetCameraModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	const UWorld* World = GetWorld();
	if (ULBAPlanetSubsystem* Planets = World ? World->GetSubsystem<ULBAPlanetSubsystem>() : nullptr)
	{
		Planets->UpdateView(InOutPOV.Location, CameraOwner && CameraOwner->bGameCameraCutThisFrame);
	}
	return false;
}
