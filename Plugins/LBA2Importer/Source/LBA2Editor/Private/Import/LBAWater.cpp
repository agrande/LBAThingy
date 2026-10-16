#include "Import/LBAWater.h"

#include "CoreGlobals.h"
#include "GerstnerWaterWaves.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "LBA2Runtime.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyOceanComponent.h"
#include "WaterSplineComponent.h"
#include "WaterTerrainComponent.h"
#include "WaterWaves.h"
#include "WaterZoneActor.h"

namespace
{
	/** Bump when the wave defaults below change: the wave assets are then reset, user edits are otherwise kept. */
	constexpr int32 WavesVersion = 1;
	const TCHAR* VersionKey = TEXT("LBA2PresetVersion");

	/**
	 * Scattering coefficient of the water (1/cm). The absorption is set per island for the water's color; together
	 * they set how deep you see (clear water: about 2 m in red, 15 m in blue).
	 */
	constexpr float WaterScattering = 0.0005f;
	/** Brightest single-scattering albedo given to a channel: deep water can't scatter back all of its light. */
	constexpr float MaxWaterAlbedo = 0.9f;

	/**
	 * Light that deep water scatters back to the camera, per unit of albedo: the sun's (Schlick phase of the ocean
	 * material, anisotropy 0.1, light scattered back up towards a camera looking down) and the ambient light's
	 * (isotropic, AmbientLight being the irradiance over pi). Fresnel takes a few percent off.
	 */
	constexpr float SunBackscatterPhase = 0.06f;
	constexpr float AmbientScatterPhase = 0.25f;
	constexpr float Transmission = 0.97f;
	/**
	 * Open water's light relative to that model, measured against the original on Citabau's harbour (the model is
	 * exact for deep water; waves and reflections of the sky add about a fifth).
	 */
	constexpr float MeasuredWaterGain = 1.2f;

	/** Gerstner wave generator settings of a sea mood (cm, degrees). */
	struct FWaves
	{
		const TCHAR* Name;
		float MinWavelength;
		float MaxWavelength;
		float MinAmplitude;
		float MaxAmplitude;
		float WindAngle;
		float Spread;
		float SmallWaveSteepness;
		float LargeWaveSteepness;
	};

	// At LBA scale (a terrain cell is 64 cm, piers stand about a meter above the sea): a calm swell of a few
	// centimeters that keeps beaches and piers readable, and choppier, higher storm waves.
	constexpr FWaves CalmWaves = { TEXT("Waves_LBA_Calm"), 150.f, 1600.f, 0.8f, 9.f, -30.f, 100.f, 0.35f, 0.15f };
	constexpr FWaves StormWaves = { TEXT("Waves_LBA_Storm"), 250.f, 3600.f, 2.f, 30.f, -60.f, 150.f, 0.6f, 0.3f };

	UWaterWavesAsset* GetWavesAsset(FLBAImportContext& Context, const FWaves& Settings)
	{
		bool bExisted = false;
		UWaterWavesAsset* Asset = Context.CreateOrReuseAsset<UWaterWavesAsset>(Context.GetContentPath(TEXT("Rendering/Water")), Settings.Name, &bExisted);
		if (!Asset)
		{
			return nullptr;
		}
		FMetaData& MetaData = Asset->GetPackage()->GetMetaData();
		const FString* Stamp = MetaData.FindValue(Asset, VersionKey);
		if (bExisted && Asset->GetWaterWaves() && Stamp && FCString::Atoi(**Stamp) >= WavesVersion)
		{
			// Keep user edits.
			return Asset;
		}

		UGerstnerWaterWaves* Waves = NewObject<UGerstnerWaterWaves>(Asset, MakeUniqueObjectName(Asset, UGerstnerWaterWaves::StaticClass(), TEXT("GerstnerWaves")));
		UGerstnerWaterWaveGeneratorSimple* Generator = NewObject<UGerstnerWaterWaveGeneratorSimple>(Waves, TEXT("Generator"));
		Generator->NumWaves = 16;
		Generator->MinWavelength = Settings.MinWavelength;
		Generator->MaxWavelength = Settings.MaxWavelength;
		Generator->MinAmplitude = Settings.MinAmplitude;
		Generator->MaxAmplitude = Settings.MaxAmplitude;
		Generator->WindAngleDeg = Settings.WindAngle;
		Generator->DirectionAngularSpreadDeg = Settings.Spread;
		Generator->SmallWaveSteepness = Settings.SmallWaveSteepness;
		Generator->LargeWaveSteepness = Settings.LargeWaveSteepness;
		Waves->GerstnerWaveGenerator = Generator;
		Waves->RecomputeWaves(false);
		Asset->SetWaterWaves(Waves);
		MetaData.SetValue(Asset, VersionKey, *LexToString(WavesVersion));
		Context.MarkForSave(Asset);
		return Asset;
	}

	/** Ocean material instance with the island's water coefficients (Absorption: 1/coefficient in cm, Scattering: coefficient in 1/m). */
	UMaterialInterface* MakeWaterMaterial(FLBAImportContext& Context, const FString& Folder, const FString& Name, const TCHAR* ParentPath,
		const FLinearColor& Absorption, const FLinearColor& Scattering)
	{
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, ParentPath);
		UMaterialInstanceConstant* Instance = Parent ? Context.CreateOrReuseAsset<UMaterialInstanceConstant>(Folder, Name) : nullptr;
		if (!Instance)
		{
			return Parent;
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
		UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(Instance);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("Absorption"), Absorption);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("Scattering"), Scattering);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Context.MarkForSave(Instance);
		return Instance;
	}
}

float LBAWater::Spawn(FLBAImportContext& Context, UWorld* World, const FLBAWaterSettings& Settings, const FLBAEnvironmentPreset& Preset,
	const FLBAEnvironmentLights& Lights)
{
	const FName Folder(TEXT("Environment"));

	// Water color: deep water scatters back Albedo * Light, Albedo = scattering / (scattering + absorption) per
	// channel. The albedo follows the sea color's hue, as bright as the light allows: the original's sea is a bright
	// painted texture, real water is darker, so the palette shading gets the remaining factor as the water's neutral
	// light.
	const FLinearColor Light = Preset.SunColor * (Lights.SunIntensity * SunBackscatterPhase) + FLinearColor::White * (Lights.AmbientLight * AmbientScatterPhase);
	FLinearColor Albedo(Preset.SeaColor.R / FMath::Max(Light.R, 1e-4f), Preset.SeaColor.G / FMath::Max(Light.G, 1e-4f), Preset.SeaColor.B / FMath::Max(Light.B, 1e-4f));
	const float AlbedoScale = FMath::Min(1.f, MaxWaterAlbedo / FMath::Max(FMath::Max3(Albedo.R, Albedo.G, Albedo.B), 1e-4f));
	Albedo *= AlbedoScale;
	Albedo.A = 1.f;
	auto InverseAbsorption = [](float ChannelAlbedo)
	{
		const float Absorption = WaterScattering * (1.f / FMath::Clamp(ChannelAlbedo, 1e-3f, 1.f) - 1.f);
		return 1.f / FMath::Max(Absorption, 1e-6f);
	};
	const FLinearColor Absorption(InverseAbsorption(Albedo.R), InverseAbsorption(Albedo.G), InverseAbsorption(Albedo.B), 1.f);
	const FLinearColor Scattering(1.f, 1.f, 1.f, WaterScattering * 1000.f);
	const float NeutralLight = AlbedoScale * Transmission * MeasuredWaterGain / FMath::Max(Preset.SeaLight, 0.05f);
	UE_LOG(LogLBA2, Display, TEXT("LBA2 water %s: sea %s, albedo %s, absorption distance %s cm, neutral light %.2f"), *Settings.Name,
		*Preset.SeaColor.ToString(), *Albedo.ToString(), *Absorption.ToString(), NeutralLight);

	UMaterialInterface* WaterMaterial = MakeWaterMaterial(Context, Settings.MaterialFolder, FString::Printf(TEXT("MI_%s_Ocean"), *Settings.Name),
		TEXT("/Water/Materials/WaterSurface/Water_Material_Ocean.Water_Material_Ocean"), Absorption, Scattering);
	UMaterialInterface* FarMaterial = MakeWaterMaterial(Context, Settings.MaterialFolder, FString::Printf(TEXT("MI_%s_OceanFar"), *Settings.Name),
		TEXT("/Water/Materials/WaterSurface/Water_FarMesh.Water_FarMesh"), Absorption, Scattering);

	// The zone covers the island and the sea around it; its water info texture (ground depth, for the shore) gets
	// about half a meter per texel.
	const FVector Center = Settings.Bounds.GetCenter();
	const FVector2D Extent(Settings.Bounds.GetSize());
	AWaterZone* Zone = Context.SpawnGeneratedActorWithFactory<AWaterZone>(World, FTransform(FVector(Center.X, Center.Y, 0.0)), TEXT("WaterZone"), Folder);
	if (!Zone)
	{
		return NeutralLight;
	}
	Zone->SetZoneExtent(Extent);
	const int32 Resolution = FMath::Clamp(int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(Extent.X, Extent.Y) / 50.0))), 256, 2048);
	Zone->SetRenderTargetResolution(FIntPoint(Resolution, Resolution));
	Zone->SetFarMeshMaterial(FarMaterial);

	for (AActor* Terrain : Settings.TerrainActors)
	{
		if (Terrain)
		{
			UWaterTerrainComponent* Component = NewObject<UWaterTerrainComponent>(Terrain, TEXT("WaterTerrain"));
			Terrain->AddInstanceComponent(Component);
			Component->RegisterComponent();
		}
	}

	// Water bodies build their render data (the meshes the water zone draws and renders into its water info) only
	// when the application can render, and save it with the level. Import commandlets can't, so they would save an
	// invisible ocean: let the water plugin build it anyway.
	TGuardValue<bool> AllowRenderData(PRIVATE_GAllowCommandletRendering, true);

	AWaterBodyOcean* Ocean = Context.SpawnGeneratedActorWithFactory<AWaterBodyOcean>(World,
		FTransform(FVector(Settings.LandPoint.X, Settings.LandPoint.Y, 0.0)), TEXT("Ocean"), Folder);
	UWaterBodyOceanComponent* Body = Ocean ? Cast<UWaterBodyOceanComponent>(Ocean->GetWaterBodyComponent()) : nullptr;
	if (!Body)
	{
		return NeutralLight;
	}
	// Coastline: a 2 m loop on land, hidden by the terrain, so the ocean covers everything else; the terrain is the
	// real coast.
	constexpr double Loop = 100.0;
	Body->GetWaterSpline()->ResetSpline({ FVector(Loop, -Loop, 0.0), FVector(Loop, Loop, 0.0), FVector(-Loop, Loop, 0.0), FVector(-Loop, -Loop, 0.0) });
	Body->SetWaterMaterial(WaterMaterial);
	if (UWaterWavesAsset* WavesAsset = GetWavesAsset(Context, Preset.bStormSea ? StormWaves : CalmWaves))
	{
		UWaterWavesAssetReference* Waves = NewObject<UWaterWavesAssetReference>(Ocean, TEXT("OceanWaterWaves"));
		Waves->SetWaterWavesAsset(WavesAsset);
		Ocean->SetWaterWaves(Waves);
	}
	FOnWaterBodyChangedParams Changed;
	Changed.bShapeOrPositionChanged = true;
	Body->UpdateAll(Changed);
	Body->UpdateWaterZones();
	Body->SetCollisionExtents(FVector(Extent / 2.0, Body->GetCollisionExtents().Z));
	Body->FillWaterZoneWithOcean();
	Body->UpdateWaterBodyRenderData();
	Zone->MarkForRebuild(EWaterZoneRebuildFlags::All);
	return NeutralLight;
}
