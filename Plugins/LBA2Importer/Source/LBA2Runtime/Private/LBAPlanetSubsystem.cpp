#include "LBAPlanetSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
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
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBALookSubsystem.h"
#include "LBAPlanetCulling.h"
#include "LBAStorm.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "NiagaraActor.h"
#include "UObject/UObjectIterator.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterZoneActor.h"

namespace
{
	TAutoConsoleVariable<int32> CVarGlobe(TEXT("lba.Globe"), -1,
		TEXT("Globe mode for the next level opened: 0 off, 1 on (islands placed on a planet open on it), -1 the project setting (LBA2 > Globe)."));
	TAutoConsoleVariable<int32> CVarGlobeDebug(TEXT("lba.Globe.Debug"), 0,
		TEXT("1 prints where the camera is on the planet on screen."));
	TAutoConsoleVariable<int32> CVarGlobeBend(TEXT("lba.Globe.Bend"), 1,
		TEXT("0: the view isn't bent onto the planet (the flat world as it is, to compare)."));
	TAutoConsoleVariable<float> CVarGlobeRadius(TEXT("lba.Globe.Radius"), -1.f,
		TEXT("Overrides the planet's radius for the next level opened (m; negative: the planet's): the islands' places and the bend."));
	TAutoConsoleVariable<float> CVarGlobeSkyLightGain(TEXT("lba.Globe.SkyLightGain"), -1.f,
		TEXT("Overrides the opened island's sky light gain on the planet (debug, for tuning; negative: the island's, see FLBAPlanetIsland)."));
	TAutoConsoleVariable<int32> CVarGlobeIslands(TEXT("lba.Globe.Islands"), 1,
		TEXT("0: the planet's other islands aren't streamed in (debug, for the next level opened)."));
	TAutoConsoleVariable<int32> CVarGlobeSea(TEXT("lba.Globe.Sea"), 1,
		TEXT("The sea for the next level opened: 1 the opened island's sea level (Water plugin oceans), the planet's sea disc beyond; 0 the sea disc alone."));
	TAutoConsoleVariable<int32> CVarGlobeLumen(TEXT("lba.Globe.Lumen"), 0,
		TEXT("1: Lumen lights the planet like the island levels (to compare: it sees the flat world, so far surfaces, sunk by the bend, darken)."));
	// Measured on Citabau unbent (lba.Globe.Bend 0) against Lumen: open pier, meadow, town and flower beds match at
	// 0.8-0.94 (walls under eaves 0.94, plants 0.8).
	TAutoConsoleVariable<float> CVarGlobeUnoccludedSkyLight(TEXT("lba.Globe.UnoccludedSkyLight"), 0.85f,
		TEXT("Without Lumen, the sky light's intensity relative to the planet's: nothing shadows it but screen space AO, so it is turned down to give the light Lumen gave."));
	// Lower than the 0.75 that matched Lumen on the far sea: the haze makes the sky it reflects much paler near the horizon.
	TAutoConsoleVariable<float> CVarGlobeSeaSkyBelow(TEXT("lba.Globe.SeaSkyBelow"), 0.3f,
		TEXT("Without Lumen, how much of the sky the sea reflects below the flat horizon (the sky light is black there): 0 none."));
	TAutoConsoleVariable<int32> CVarGlobeShowClouds(TEXT("lba.Globe.ShowClouds"), 0,
		TEXT("1: the opened island's clouds (Unreal's cloud material only forms them kilometers up: a far wallpaper around so small a planet)."));
	// Measured against the flat Citabau level with its clouds: pier (122,129,128) against (123,129,125), meadow (61,92,60)
	// against (58,90,57).
	TAutoConsoleVariable<float> CVarGlobeClearSkyGain(TEXT("lba.Globe.ClearSkyGain"), 1.6f,
		TEXT("Without clouds, how much brighter the sky is: their share of the land's light (the sky light captures the sky) comes from it instead."));
	/** Without clouds the sky light is all sky blue: these give the land what their white gave it (colors can't exceed 1). */
	const FLinearColor ClearSkyLightColor(1.f, 0.9f, 0.7f);
	constexpr float ClearSkyLightGain = 0.84f;
	TAutoConsoleVariable<float> CVarGlobeHaze(TEXT("lba.Globe.Haze"), 30.f,
		TEXT("The haze that pales the sky towards the sea's horizon: how much it scatters at sea level (per km, the atmosphere's Mie scattering scale). 0: none."));
	TAutoConsoleVariable<float> CVarGlobeHazeHeight(TEXT("lba.Globe.HazeHeight"), 10.f,
		TEXT("How high the haze goes (m): its density falls by e over this height, so it also sets how far up the sky the pale band reaches."));
	TAutoConsoleVariable<float> CVarGlobeHazeGlow(TEXT("lba.Globe.HazeGlow"), 0.3f,
		TEXT("How much the haze gathers around the sun (the atmosphere's Mie anisotropy): 0 evenly around the horizon."));
	TAutoConsoleVariable<int32> CVarGlobeShowFog(TEXT("lba.Globe.ShowFog"), 0,
		TEXT("1: the opened island's height fog. It is made for a flat world: below the flat horizon the sky fills with it, and the bend sinks far surfaces deep into it."));
	TAutoConsoleVariable<int32> CVarGlobeSun(TEXT("lba.Globe.Sun"), 1,
		TEXT("1: the sun is where it is seen from the camera's place on the planet; 0: the opened island's fixed sun (to compare)."));
	// Over the sea: above the pole Desert's dunes go flat (84 degrees), this low they keep their relief. On the side that
	// keeps Citadel's shadows near their direction in its level. Elevation at their centers (coasts): Citadel 37 (25-50,
	// 31 in its level, its direction 2 degrees off), Desert 48 (34-65, 37 in its level).
	TAutoConsoleVariable<float> CVarGlobeSunColatitude(TEXT("lba.Globe.SunColatitude"), 45.f,
		TEXT("Where the sun stands overhead on the planet: its angle from the pole (degrees)."));
	TAutoConsoleVariable<float> CVarGlobeSunLongitude(TEXT("lba.Globe.SunLongitude"), -60.f,
		TEXT("Where the sun stands overhead on the planet: its longitude (degrees, the planet map's angle)."));
	TAutoConsoleVariable<float> CVarGlobeSunMinElevation(TEXT("lba.Globe.SunMinElevation"), 20.f,
		TEXT("The sun never gets lower than this (degrees), however far it is: the far side of the belt stays lit."));
	// The belt's nearest shore is 76 degrees from the pole. Citadel's north cliffs, at 51, get the first of it: a quarter of the
	// way, its harbour (33) and Desert (under 30) none.
	TAutoConsoleVariable<float> CVarGlobeSunsetFrom(TEXT("lba.Globe.SunsetFrom"), 40.f,
		TEXT("Going out from the pole, where the sunset towards the belt begins: the angle from the pole (degrees)."));
	TAutoConsoleVariable<float> CVarGlobeSunsetTo(TEXT("lba.Globe.SunsetTo"), 76.f,
		TEXT("Where the sunset is complete: the angle from the pole (degrees)."));
	TAutoConsoleVariable<float> CVarGlobeSunsetElevation(TEXT("lba.Globe.SunsetElevation"), 3.f,
		TEXT("How high the sun stands in the sunset, over the pole's side of the horizon (degrees)."));
	/** The sky's factor in the sunset, for its orange: the atmosphere scatters little red, much blue. */
	const FLinearColor SunsetSkyFactor(70.f, 16.f, 5.f);
	const FLinearColor DayHazeTint(1.f, 0.6f, 0.7f);
	const FLinearColor SunsetHazeTint(1.f, 0.5f, 0.1f);
	const FLinearColor SunsetSunTint(1.f, 0.55f, 0.25f);
	constexpr float SunsetHazeGlow = 0.6f;
	/**
	 * The look's LightTint: surfaces take their color from the palette and little from the light, so a warm sun on the
	 * belt's blue snow only made it gray. The sunset gives it more of the light's color. 0.35 is the enhanced look's own.
	 */
	constexpr float DayLightTint = 0.35f;
	constexpr float SunsetLightTint = 0.8f;
	TAutoConsoleVariable<float> CVarGlobeSunsetSunGain(TEXT("lba.Globe.SunsetSunGain"), 5.5f,
		TEXT("How much stronger the sun is in the sunset: so low, it lights hardly any ground, only walls facing it."));
	TAutoConsoleVariable<float> CVarGlobeSunGroundLight(TEXT("lba.Globe.SunGroundLight"), 1.f,
		TEXT("How much the sun's intensity makes up for its elevation: 1 keeps lit flat ground as bright as in the opened island's level, 0 leaves the intensity."));
	TAutoConsoleVariable<float> CVarGlobeSunSmoothing(TEXT("lba.Globe.SunSmoothing"), 0.5f,
		TEXT("How slowly the sun follows the camera (seconds)."));
	// Above the Dinofly's ceiling (60 m) and the belt's crest (62 m): in play the sunset stays.
	TAutoConsoleVariable<float> CVarGlobeSkySunsetFrom(TEXT("lba.Globe.SkySunsetFrom"), 60.f,
		TEXT("Climbing, where the camera starts leaving the sunset's air: its sky fades back to the day's (m above the sea)."));
	TAutoConsoleVariable<float> CVarGlobeSkySunsetTo(TEXT("lba.Globe.SkySunsetTo"), 150.f,
		TEXT("Where the camera has left the sunset's air: the sky is the day's (m above the sea)."));
	TAutoConsoleVariable<int32> CVarGlobeSurfaceSun(TEXT("lba.Globe.SurfaceSun"), 1,
		TEXT("1: each surface is lit by the sun of its own place on the planet (LBAPlanetSunRelight), 0: by the camera's place's, like the renderer (to compare)."));
	TAutoConsoleVariable<float> CVarGlobeSurfaceSkyLight(TEXT("lba.Globe.SurfaceSkyLight"), 0.4f,
		TEXT("For each surface's own sun: the sky light's share of lit flat ground's light in the day (the rest is the sun's)."));
	// Measured with the light the renderer gives (scene color / base color): the belt's walls up close (1.99,0.84,0.33)
	// against the model's (1.28,1.0,0.8), Citadel's harbour by day (0.33,0.80,0.98) against (0.97,1.0,1.03).
	TAutoConsoleVariable<FString> CVarGlobeSunsetLight(TEXT("lba.Globe.SunsetLight"), TEXT("4.6,1.05,0.43"),
		TEXT("For each surface's own sun: the renderer's light in the sunset relative to the day's, beyond the model's sun and sky light (R,G,B; semicolons or spaces also separate them)."));
	// The renderer's sunset sun is SunsetSunGain stronger, but so low that the belt's benches and folds shade each other:
	// no more of it reaches surfaces than by day (SunsetLight makes up the rest). SunsetSunGain burnt the belt seen from Citadel.
	TAutoConsoleVariable<float> CVarGlobeSurfaceSunsetGain(TEXT("lba.Globe.SurfaceSunsetGain"), 1.f,
		TEXT("For each surface's own sun: how much stronger the sunset sun is than the day's in the light it gives (shadows included)."));
	TAutoConsoleVariable<int32> CVarGlobeSurfaceSunDebug(TEXT("lba.Globe.SurfaceSunDebug"), 0,
		TEXT("For calibrating each surface's own sun, shown / 4: 1 the light the renderer gives (scene color / base color), 2 the model's for the surface's place, 3 what that multiplies its light by."));
	TAutoConsoleVariable<float> CVarGlobeSunsetTintHeight(TEXT("lba.Globe.SunsetTintHeight"), 60.f,
		TEXT("How high the sunset sun's orange reaches (m): above a surface, the air that reddens it falls by e over this height."));
	TAutoConsoleVariable<int32> CVarGlobeBelt(TEXT("lba.Globe.Belt"), 1,
		TEXT("0: the planet's belt (Twinsun's Hamalayi mountains) isn't streamed in (for the next level opened)."));
	TAutoConsoleVariable<int32> CVarGlobeStubs(TEXT("lba.Globe.Stubs"), 1,
		TEXT("0: the planet's stub islands (the holomap's other islands) aren't streamed in (for the next level opened)."));

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

	/**
	 * Gives World's water zones the indices 0..N-1 again. The Water plugin packs its zones' GPU data one after another,
	 * but water bodies (and their materials) look their zone up by its index among all zones ever registered: once a
	 * zone before it is destroyed, a zone's water points past the data and draws nothing.
	 */
	void CompactWaterZones(UWorld* World)
	{
		TArray<AWaterZone*> Zones;
		for (TActorIterator<AWaterZone> It(World); It; ++It)
		{
			if (It->HasActorRegisteredAllComponents())
			{
				Zones.Add(*It);
			}
		}
		if (!Zones.ContainsByPredicate([&Zones](const AWaterZone* Zone) { return Zone->GetWaterZoneIndex() >= Zones.Num(); }))
		{
			return;
		}
		// Each takes the lowest free index when registered again.
		for (AWaterZone* Zone : Zones)
		{
			Zone->UnregisterAllComponents();
		}
		for (AWaterZone* Zone : Zones)
		{
			Zone->RegisterAllComponents();
		}
		for (TActorIterator<AWaterBody> It(World); It; ++It)
		{
			if (UWaterBodyComponent* Body = It->GetWaterBodyComponent())
			{
				Body->UpdateMaterialInstances();
			}
		}
		UE_LOG(LogLBA2, Display, TEXT("Globe mode: %d water zones registered again"), Zones.Num());
	}

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
	ViewExtension.Reset();
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
	const FString Map = UWorld::RemovePIEPrefix(InWorld.GetMapName());
	for (const TSoftObjectPtr<ULBAPlanetAsset>& Soft : GetDefault<ULBA2Settings>()->Planets)
	{
		ULBAPlanetAsset* Loaded = Soft.LoadSynchronous();
		const int32 HomeIndex = Loaded ? Loaded->FindIslandIndex(Loaded->HomeIsland) : INDEX_NONE;
		if (HomeIndex != INDEX_NONE && Loaded->Level.GetAssetName() == Map)
		{
			bPlanetLevel = true;
			Activate(Loaded, HomeIndex);
			return;
		}
	}
	if (!IsGlobeRequested(&InWorld))
	{
		return;
	}
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
	// The planet's level has its islands where they are at its own radius.
	const float RadiusOverride = bPlanetLevel ? 0.f : CVarGlobeRadius.GetValueOnGameThread();
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
	ViewExtension = FSceneViewExtensions::NewExtension<FLBAPlanetViewExtension>(World);
	bWaterSea = CVarGlobeSea.GetValueOnGameThread() != 0 && !Asset->Islands[Home].SeaLevel.IsNull();
	Spawned->GetSea()->SetStaticMesh(Asset->SeaMesh.LoadSynchronous());
	Spawned->GetSea()->SetMaterial(0, Asset->SeaMaterial.LoadSynchronous());
	Asset->MaterialParameters.LoadSynchronous();

	if (bPlanetLevel)
	{
		// Already sub-levels of the world (renamed UEDPIE_<n>_<level> in PIE).
		const FString SeaPackage = Asset->Islands[Home].SeaLevel.ToSoftObjectPath().GetLongPackageName();
		const FString BeltPackage = Asset->BeltLevel.ToSoftObjectPath().GetLongPackageName();
		const FString StubPackage = Asset->StubLevel.ToSoftObjectPath().GetLongPackageName();
		for (ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			const FString Package = Streaming ? UWorld::RemovePIEPrefix(Streaming->GetWorldAssetPackageName()) : FString();
			if (Package == SeaPackage)
			{
				SeaStream = Streaming;
			}
			else if (Package == BeltPackage)
			{
				BeltStream = Streaming;
			}
			else if (Package == StubPackage)
			{
				StubStream = Streaming;
			}
		}
		bWaterSea = SeaStream.IsValid();
	}
	for (ULevel* Level : World->GetLevels())
	{
		if (!IsPlanetLevel(Level))
		{
			AdaptLevel(Level, FindIsland(Level));
		}
	}
	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(this, &ULBAPlanetSubsystem::OnLevelAdded);
	if (!bPlanetLevel)
	{
		LoadIslands();
	}

	const FLBAPlanetIsland& Island = Asset->Islands[Home];
	const FVector2D Map = Island.GetMapLocation(Radius);
	UE_LOG(LogLBA2, Display, TEXT("Globe mode: %s on %s (radius %.0f m), latitude %.1f, longitude %.1f, heading %.1f, at (%.0f, %.0f) m on the map"),
		*Island.Island.ToString(), *Asset->GetName(), Radius / 100.0, Island.Latitude, Island.Longitude, Island.Heading,
		Map.X / 100.0, Map.Y / 100.0);
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
				// As much air straight up as in the 20 km atmosphere the sky's colors were tuned with: its density hardly
				// changes over so low a height.
				const float AirDistribution = SkyAtmosphere->RayleighExponentialDistribution;
				const auto AirAbove = [AirDistribution](float Height) { return AirDistribution * (1.f - FMath::Exp(-Height / AirDistribution)); };
				SkyAtmosphere->SetRayleighScatteringScale(SkyAtmosphere->RayleighScatteringScale * AirAbove(20.f) / AirAbove(Asset->AtmosphereHeight));
				SkyAtmosphere->SetMieAbsorptionScale(0.f);
				SkyAtmosphere->SetWorldLocation(FVector(0.0, 0.0, SeaLevel - Radius));
				SkyAtmosphere->MarkRenderStateDirty();
				Atmosphere = SkyAtmosphere;
				LevelSkyFactor = SkyAtmosphere->SkyLuminanceFactor;
			}
			else if (UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Component))
			{
				if (Light->IsUsedAsAtmosphereSunLight())
				{
					SunLight = Light;
					LevelSunRotation = Light->GetComponentRotation();
					LevelSunIntensity = Light->Intensity;
					LevelSunColor = FLinearColor(Light->LightColor);
					LevelCloudScale = Light->CloudScatteredLuminanceScale;
				}
				else if (Light->CastShadows)
				{
					CharacterLight = Light;
					LevelCharacterIntensity = Light->Intensity;
				}
				else
				{
					CharacterFill = Light;
				}
			}
			else if (UVolumetricCloudComponent* Clouds = Cast<UVolumetricCloudComponent>(Component))
			{
				if (Asset->CloudBottom > 0.f && Asset->CloudHeight > 0.f)
				{
					Clouds->SetLayerBottomAltitude(Asset->CloudBottom);
					Clouds->SetLayerHeight(Asset->CloudHeight);
				}
				SkyClouds = Clouds;
			}
			else if (UExponentialHeightFogComponent* Fog = Cast<UExponentialHeightFogComponent>(Component))
			{
				HeightFog = Fog;
			}
			else if (USkyLightComponent* Sky = Cast<USkyLightComponent>(Component))
			{
				const float Override = CVarGlobeSkyLightGain.GetValueOnGameThread();
				Sky->SetIntensity(Sky->Intensity * (Override >= 0.f ? Override : Island.SkyLightGain));
				SkyLight = Sky;
				bSkyLightShadows = Sky->CastShadows;
				SkyLightIntensity = Sky->Intensity;
				SkyLightColor = FLinearColor(Sky->LightColor);
			}
		}
	}
	bool bZoneDestroyed = false;
	for (AActor* Actor : Replaced)
	{
		bZoneDestroyed |= Actor->IsA<AWaterZone>();
		Actor->Destroy();
	}
	if (bZoneDestroyed)
	{
		CompactWaterZones(GetWorld());
	}
}

void ULBAPlanetSubsystem::OnLevelAdded(ULevel* Level, UWorld* InWorld)
{
	if (InWorld != GetWorld() || !IsActive() || !Level)
	{
		return;
	}
	if (IsPlanetLevel(Level))
	{
		if (BeltStream.IsValid() && BeltStream->GetLoadedLevel() == Level)
		{
			UE_LOG(LogLBA2, Display, TEXT("Globe mode: the belt streamed in, %d actors"), Level->Actors.Num());
		}
		else if (StubStream.IsValid() && StubStream->GetLoadedLevel() == Level)
		{
			UE_LOG(LogLBA2, Display, TEXT("Globe mode: the stub islands streamed in, %d actors"), Level->Actors.Num());
		}
		return;
	}
	const int32 IslandIndex = FindIsland(Level);
	AdaptLevel(Level, IslandIndex);
	if (IslandIndex != Home)
	{
		UE_LOG(LogLBA2, Display, TEXT("Globe mode: %s streamed in, %d actors"), *Asset->Islands[IslandIndex].Island.ToString(), Level->Actors.Num());
	}
}

bool ULBAPlanetSubsystem::IsPlanetLevel(const ULevel* Level) const
{
	auto IsStreamed = [Level](const TWeakObjectPtr<ULevelStreaming>& Stream) { return Stream.IsValid() && Stream->GetLoadedLevel() == Level; };
	return Level && (IsStreamed(SeaStream) || IsStreamed(BeltStream) || IsStreamed(StubStream));
}

int32 ULBAPlanetSubsystem::FindIsland(const ULevel* Level) const
{
	// The other islands' levels come in where they are on the map; the opened island's sub-levels are its own.
	for (int32 Index = 0; Index < IslandStreams.Num(); ++Index)
	{
		if (IslandStreams[Index].IsValid() && IslandStreams[Index]->GetLoadedLevel() == Level)
		{
			return Index;
		}
	}
	// In the planet's level, an island's level and its scenes are its sub-levels where the island is.
	const UWorld* World = GetWorld();
	if (!bPlanetLevel || !World)
	{
		return Home;
	}
	for (const ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		if (!Streaming || Streaming->GetLoadedLevel() != Level)
		{
			continue;
		}
		for (int32 Index = 0; Index < Asset->Islands.Num(); ++Index)
		{
			if (Streaming->LevelTransform.GetTranslation().Equals(GetIslandToWorld(Index).GetTranslation(), 1.0))
			{
				return Index;
			}
		}
	}
	return Home;
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
	// Laid on the map for the asset's radius, like the belt.
	if (!Asset->StubLevel.IsNull() && CVarGlobeStubs.GetValueOnGameThread() != 0 && Radius == Asset->Radius)
	{
		bool bLoaded = false;
		StubStream = ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(GetWorld(), Asset->StubLevel, GetMapToWorld(), bLoaded,
			Asset->StubLevel.GetAssetName() + TEXT("_Instance"));
		if (!bLoaded)
		{
			UE_LOG(LogLBA2, Warning, TEXT("Globe mode: couldn't stream in the stub islands (%s)"), *Asset->StubLevel.ToString());
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

	const bool bLumen = CVarGlobeLumen.GetValueOnGameThread() != 0;
	Planet->GetLighting()->bEnabled = !bLumen;
	USkyLightComponent* Sky = SkyLight.Get();
	if (Sky && Sky->CastShadows != (bLumen && bSkyLightShadows))
	{
		Sky->SetCastShadows(bLumen && bSkyLightShadows);
	}
	const bool bClearSky = IsSkyClear();
	const float SkyIntensity = SkyLightIntensity * (bLumen ? 1.f : CVarGlobeUnoccludedSkyLight.GetValueOnGameThread()) * (bClearSky ? ClearSkyLightGain : 1.f);
	if (Sky && Sky->Intensity != SkyIntensity)
	{
		Sky->SetIntensity(SkyIntensity);
	}
	if (UVolumetricCloudComponent* Clouds = SkyClouds.Get())
	{
		Clouds->SetVisibility(!bClearSky);
		if (Sky)
		{
			Sky->SetLightColor(bClearSky ? ClearSkyLightColor * SkyLightColor : SkyLightColor);
		}
	}
	if (UExponentialHeightFogComponent* Fog = HeightFog.Get())
	{
		// The palette shading follows (ULBALookSubsystem::SyncHeightFog).
		Fog->SetVisibility(CVarGlobeShowFog.GetValueOnGameThread() != 0);
	}

	// The sea disc and the atmosphere's planet under the camera.
	const FVector Sea(View.X, View.Y, SeaLevel);
	Planet->GetSea()->SetWorldLocation(Sea - FVector(0.0, 0.0, bWaterSea ? CVarGlobeSeaDiscDrop.GetValueOnGameThread() : 0.0));
	if (USkyAtmosphereComponent* SkyAtmosphere = Atmosphere.Get())
	{
		SkyAtmosphere->SetWorldLocation(FVector(View.X, View.Y, SeaLevel - Radius));
		if (Sky)
		{
			// It captures the sky from where it stands: on the atmosphere's planet, near the sea whatever the camera's height,
			// so that the land's light doesn't change in flight. Far from the camera it would be outside the low atmosphere.
			Sky->SetWorldLocation(FVector(View.X, View.Y, SeaLevel + 500.0));
		}
	}
	UpdateSun(bCameraCut);

	UMaterialParameterCollection* Collection = Asset->MaterialParameters.Get();
	UMaterialParameterCollectionInstance* Instance = Collection ? GetWorld()->GetParameterCollectionInstance(Collection) : nullptr;
	const bool bBend = CVarGlobeBend.GetValueOnGameThread() != 0;
	if (ViewExtension)
	{
		ViewExtension->SetBend(View, Radius, SeaLevel, bBend && Instance);
	}
	if (Instance)
	{
		Instance->SetVectorParameterValue(TEXT("PlanetBend"), FLinearColor(float(Radius), 0.f, float(SeaLevel), bBend ? 1.f : 0.f));
		Instance->SetVectorParameterValue(TEXT("PlanetView"), ToParameter(View, 0.0));
		Instance->SetVectorParameterValue(TEXT("PlanetViewPrevious"), ToParameter(PreviousView, 0.0));
		// The disc's move since last frame: its vertices aren't the same points of the sea from frame to frame.
		Instance->SetVectorParameterValue(TEXT("PlanetSeaMove"), ToParameter(Sea - PreviousSea, 0.0));
		// Lumen's reflections see the sky (or the world) below the flat horizon by themselves.
		const float SkyBelow = bLumen ? 0.f : CVarGlobeSeaSkyBelow.GetValueOnGameThread();
		Instance->SetVectorParameterValue(TEXT("PlanetSkyBelow"), FLinearColor(SkyBelow, SkyBelow, SkyBelow, 0.f));
		UpdateSurfaceSun(*Instance);
	}
	PreviousSea = Sea;
}

bool ULBAPlanetSubsystem::IsSkyClear() const
{
	return SkyClouds.IsValid() && CVarGlobeShowClouds.GetValueOnGameThread() == 0;
}

void ULBAPlanetSubsystem::UpdateSun(bool bCameraCut)
{
	UDirectionalLightComponent* Sun = SunLight.Get();
	if (!Sun)
	{
		return;
	}
	FRotator Rotation = LevelSunRotation;
	float SunScale = 1.f;
	float Sunset = 0.f;
	// High up, the camera leaves the air that turns the sky orange: it fades back to the day's.
	float SkyDay = 0.f;
	bSunFollows = CVarGlobeSun.GetValueOnGameThread() != 0;
	if (bSunFollows)
	{
		const FVector2D Viewed(View.X, View.Y);
		const float Smoothing = CVarGlobeSunSmoothing.GetValueOnGameThread();
		const bool bSnap = !bHasSunView || bCameraCut || FVector2D::Distance(Viewed, SunView) > ViewJump || Smoothing <= 0.f;
		SunView = bSnap ? Viewed : FMath::Lerp(SunView, Viewed, 1.0 - FMath::Exp(-GetWorld()->GetDeltaSeconds() / Smoothing));
		bHasSunView = true;

		// Where it stands overhead, on the flat map: from there the sun sinks with the angle around the planet.
		const double FromPole = Radius * FMath::DegreesToRadians(CVarGlobeSunColatitude.GetValueOnGameThread());
		const double Lon = FMath::DegreesToRadians(CVarGlobeSunLongitude.GetValueOnGameThread());
		const FVector Overhead = GetMapToWorld().TransformPosition(FVector(FMath::Cos(Lon) * FromPole, FMath::Sin(Lon) * FromPole, 0.0));
		const FVector2D ToOverhead = FVector2D(Overhead.X, Overhead.Y) - SunView;
		const double MaxAngle = FMath::DegreesToRadians(90.0 - FMath::Clamp(CVarGlobeSunMinElevation.GetValueOnGameThread(), 1.f, 89.f));
		const double Angle = FMath::Min(ToOverhead.Size() / Radius, MaxAngle);
		const FVector2D Along = ToOverhead.GetSafeNormal();
		const FVector DayToSun(Along.X * FMath::Sin(Angle), Along.Y * FMath::Sin(Angle), FMath::Cos(Angle));

		// Towards the belt the sun goes down, over the pole's side of the horizon, whichever side of the belt the camera is on.
		const FVector Pole = GetMapToWorld().GetLocation();
		const FVector2D ToPole = FVector2D(Pole.X, Pole.Y) - SunView;
		Sunset = FMath::SmoothStep(CVarGlobeSunsetFrom.GetValueOnGameThread(), CVarGlobeSunsetTo.GetValueOnGameThread(),
			float(FMath::RadiansToDegrees(ToPole.Size() / Radius)));
		const double SunsetElevation = FMath::DegreesToRadians(CVarGlobeSunsetElevation.GetValueOnGameThread());
		const FVector2D ToPoleAlong = ToPole.GetSafeNormal();
		const FVector SunsetToSun(ToPoleAlong.X * FMath::Cos(SunsetElevation), ToPoleAlong.Y * FMath::Cos(SunsetElevation), FMath::Sin(SunsetElevation));
		const FVector ToSun = FMath::Lerp(DayToSun, SunsetToSun, double(Sunset)).GetSafeNormal();
		Rotation = (-ToSun).Rotation();
		SkyDay = FMath::SmoothStep(CVarGlobeSkySunsetFrom.GetValueOnGameThread(), CVarGlobeSkySunsetTo.GetValueOnGameThread(), float(View.Z - SeaLevel) / 100.f);

		const double LevelSine = -LevelSunRotation.Vector().Z;
		// Only ever down: a sun lower than the level's, turned up, burns the walls facing it (the belt's went white).
		SunScale = float(FMath::Min(FMath::Pow(FMath::Max(LevelSine, 0.01) / ToSun.Z, double(CVarGlobeSunGroundLight.GetValueOnGameThread())), 1.0));
		SunScale *= FMath::Lerp(1.f, CVarGlobeSunsetSunGain.GetValueOnGameThread(), Sunset);

		// For each surface's own sun (LBAPlanetSunRelight): the same, from its own place.
		SunPole = FVector2D(Pole.X, Pole.Y);
		SunOverhead = FVector2D(Overhead.X, Overhead.Y);
		SunMaxAngle = float(MaxAngle);
		SunLevelSine = float(LevelSine);
		CameraToSun = ToSun;
	}
	else
	{
		bHasSunView = false;
	}
	CameraSunset = Sunset;
	// High up, the palette shading shows the day's sky instead of the sunset's (LBAPaletteShading): the clear sky measured
	// from Citadel's coast at 5 degrees pitch, from top to horizon (sRGB), the horizon as far down as the bend puts it.
	const double Height = FMath::Max(View.Z - SeaLevel, 0.0);
	SkyDayZenith = FLinearColor::FromSRGBColor(FColor(20, 120, 170));
	SkyDayZenith.A = Sunset > 0.f ? SkyDay : 0.f;
	SkyDayHorizon = FLinearColor::FromSRGBColor(FColor(112, 189, 220));
	SkyDayHorizon.A = float(FMath::Acos(Radius / (Radius + Height)));
	Sun->SetWorldRotation(Rotation);
	Sun->SetIntensity(LevelSunIntensity * SunScale);
	const FLinearColor SunColor = LevelSunColor * FMath::Lerp(FLinearColor::White, SunsetSunTint, Sunset);
	Sun->SetLightColor(SunColor);
	if (UDirectionalLightComponent* Key = CharacterLight.Get())
	{
		// The key light is what the sun lacks to light characters as in the original: together they stay the same.
		Key->SetWorldRotation(Rotation);
		Key->SetLightColor(SunColor);
		Key->SetIntensity(FMath::Max(LevelCharacterIntensity + LevelSunIntensity * (1.f - SunScale), 0.f));
	}
	if (UDirectionalLightComponent* Fill = CharacterFill.Get())
	{
		Fill->SetWorldRotation(FRotator(Rotation.Pitch, Rotation.Yaw + 180.0, 0.0));
	}
	if (FMath::Abs(Sunset - LookSunset) > 0.01f || (Sunset <= 0.f && LookSunset > 0.f))
	{
		LookSunset = Sunset;
		if (ULBALookSubsystem* Look = GetWorld()->GetSubsystem<ULBALookSubsystem>())
		{
			TArray<float> LightTint;
			if (Sunset > 0.f)
			{
				LightTint.Add(FMath::Lerp(DayLightTint, SunsetLightTint, Sunset));
			}
			Look->SetParameterOverride(TEXT("LightTint"), LightTint);
		}
	}
	if (USkyAtmosphereComponent* SkyAtmosphere = Atmosphere.Get())
	{
		// Rebuilds the atmosphere's and the sun's render state, so not for changes too small to see.
		const float ClearSkyGain = IsSkyClear() ? CVarGlobeClearSkyGain.GetValueOnGameThread() : 1.f;
		const FLinearColor DayFactor = LevelSkyFactor * ClearSkyGain;
		// Red comes first and blue goes last, or the sky turns green half way: blue, then lavender, rose and orange.
		const auto Blend = [](float Day, float Dusk, float Amount) { return FMath::Pow(Day, 1.f - Amount) * FMath::Pow(Dusk, Amount); };
		const FLinearColor Factor = FLinearColor(Blend(DayFactor.R, SunsetSkyFactor.R, FMath::Pow(Sunset, 0.7f)), Blend(DayFactor.G, SunsetSkyFactor.G, Sunset),
			Blend(DayFactor.B, SunsetSkyFactor.B, Sunset * Sunset)) / FMath::Max(SunScale, 0.01f);
		const auto Changed = [](float New, float Old) { return FMath::Abs(New - Old) > 0.01f * New; };
		if (Changed(Factor.R, SkyFactor.R) || Changed(Factor.G, SkyFactor.G) || Changed(Factor.B, SkyFactor.B))
		{
			SkyFactor = Factor;
			SkyAtmosphere->SetSkyLuminanceFactor(Factor);
			// The haze gets the sky's factor undone, so that its color is its own: pale, however tinted the sky is.
			const FLinearColor HazeTint = FMath::Lerp(DayHazeTint, SunsetHazeTint, Sunset) * FMath::Max(DayFactor.GetMin(), 1e-3f) / FMath::Max(SunScale, 0.01f);
			SkyAtmosphere->SetMieScattering(FLinearColor(HazeTint.R / FMath::Max(Factor.R, 1e-3f), HazeTint.G / FMath::Max(Factor.G, 1e-3f), HazeTint.B / FMath::Max(Factor.B, 1e-3f)));
			Sun->CloudScatteredLuminanceScale = LevelCloudScale / FMath::Max(SunScale, 0.01f);
			Sun->MarkRenderStateDirty();
		}
		const float Haze = CVarGlobeHaze.GetValueOnGameThread();
		const float HazeHeight = FMath::Max(CVarGlobeHazeHeight.GetValueOnGameThread(), 1.f) / 1000.f;
		const float HazeGlow = FMath::Lerp(CVarGlobeHazeGlow.GetValueOnGameThread(), SunsetHazeGlow, Sunset);
		if (SkyAtmosphere->MieScatteringScale != Haze || SkyAtmosphere->MieExponentialDistribution != HazeHeight || SkyAtmosphere->MieAnisotropy != HazeGlow)
		{
			SkyAtmosphere->SetMieScatteringScale(Haze);
			SkyAtmosphere->SetMieExponentialDistribution(HazeHeight);
			SkyAtmosphere->SetMieAnisotropy(HazeGlow);
		}
	}
}

void ULBAPlanetSubsystem::UpdateSurfaceSun(UMaterialParameterCollectionInstance& Instance) const
{
	Instance.SetVectorParameterValue(TEXT("PlanetSkyDayZenith"), SkyDayZenith);
	Instance.SetVectorParameterValue(TEXT("PlanetSkyDayHorizon"), SkyDayHorizon);
	// Off: PlanetSunTint's alpha 0.
	if (!bSunFollows || CVarGlobeSurfaceSun.GetValueOnGameThread() == 0)
	{
		Instance.SetVectorParameterValue(TEXT("PlanetSunTint"), FLinearColor::Transparent);
		return;
	}
	FLinearColor SunsetLight(4.6f, 1.05f, 0.43f);
	TArray<FString> Channels;
	// Any of , ; or spaces between the channels (-ExecCmds splits on commas).
	const TCHAR* Separators[] = { TEXT(","), TEXT(";"), TEXT(" ") };
	if (CVarGlobeSunsetLight.GetValueOnGameThread().ParseIntoArray(Channels, Separators, UE_ARRAY_COUNT(Separators)) == 3)
	{
		SunsetLight = FLinearColor(FCString::Atof(*Channels[0]), FCString::Atof(*Channels[1]), FCString::Atof(*Channels[2]));
	}
	const float SkyLightShare = FMath::Clamp(CVarGlobeSurfaceSkyLight.GetValueOnGameThread(), 0.01f, 1.f);
	Instance.SetVectorParameterValue(TEXT("PlanetSunPlaces"), FLinearColor(float(SunPole.X), float(SunPole.Y), float(SunOverhead.X), float(SunOverhead.Y)));
	Instance.SetVectorParameterValue(TEXT("PlanetSunAngles"), FLinearColor(FMath::DegreesToRadians(CVarGlobeSunsetFrom.GetValueOnGameThread()),
		FMath::DegreesToRadians(CVarGlobeSunsetTo.GetValueOnGameThread()), FMath::DegreesToRadians(CVarGlobeSunsetElevation.GetValueOnGameThread()), SunMaxAngle));
	Instance.SetVectorParameterValue(TEXT("PlanetSunLight"), FLinearColor(SunLevelSine, CVarGlobeSunGroundLight.GetValueOnGameThread(),
		CVarGlobeSurfaceSunsetGain.GetValueOnGameThread(), CVarGlobeSunsetTintHeight.GetValueOnGameThread() * 100.f));
	Instance.SetVectorParameterValue(TEXT("PlanetSunTint"), FLinearColor(SunsetSunTint.R, SunsetSunTint.G, SunsetSunTint.B, SkyLightShare));
	const int32 Debug = CVarGlobeSurfaceSunDebug.GetValueOnGameThread();
	Instance.SetVectorParameterValue(TEXT("PlanetSunAmbient"), FLinearColor(SunsetLight.R, SunsetLight.G, SunsetLight.B, Debug > 0 ? -float(Debug) : SunsetLightTint - DayLightTint));
	Instance.SetVectorParameterValue(TEXT("PlanetSunCamera"), FLinearColor(float(CameraToSun.X), float(CameraToSun.Y), float(CameraToSun.Z), CameraSunset));
}

void ULBAPlanetSubsystem::DrawDebug() const
{
	if (!GEngine || !bHasView)
	{
		return;
	}
	double Distance = 0.0;
	const int32 Nearest = FindNearestIsland(View, Distance);
	// How far the nearest island's center sinks (cm), and the sea horizon's distance at the camera's height (where the
	// view grazes the sea, for small angles).
	const double Drop = Radius * (1.0 - FMath::Cos(FMath::Min(Distance / Radius, UE_DOUBLE_PI)));
	const double Height = FMath::Max(View.Z - SeaLevel, 0.0);
	const double Horizon = FMath::Sqrt(2.0 * Radius * Height);
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
