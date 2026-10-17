#include "Import/LBAEnvironment.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Formats/LBAXpl.h"
#include "Import/LBAImportContext.h"
#include "Import/LBARenderingAssets.h"
#include "LBA2Runtime.h"
#include "LBALookSubsystem.h"
#include "LBATypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"

namespace
{
	/**
	 * Average color of Unreal's default sky atmosphere 45 degrees above the horizon, with its clouds and fog, per lux
	 * of sun (measured on Citabau, sun about 30 degrees high): the sky luminance factor that tints the sky towards an
	 * island's sky divides by this.
	 */
	const FLinearColor DefaultSkyPerSunLux(0.0081f, 0.0133f, 0.0242f);

	/**
	 * Measured with lba.Look.Debug 3 on open flat ground of Citabau's harbour (two sky light balances, solved for both):
	 * the light flat ground gets per lux of sun, and per unit of sky light intensity, relative to what the light model
	 * predicts. The sun loses to the exposure's lens attenuation; the sky light to its dimmer horizon and occlusion.
	 */
	constexpr float MeasuredSunGain = 0.865f;
	constexpr float MeasuredSkyGain = 0.88f;

	/**
	 * Effective luminance of the gray cubemap of uniform sky lights, relative to MeasuredSkyGain (measured with
	 * lba.Look.Debug 3 on Citadel).
	 */
	constexpr float UniformSkyLuminance = 0.41f;

	/** Height fog: density halves every 20 m above sea level (FogHeightFalloff in 1/1000 of 1/cm), so the sky clears quickly above the horizon. */
	constexpr float FogHeightFalloff = 0.5f;
	/** Volumetric fog reach (cm): light shafts around the player. */
	constexpr float VolumetricFogDistance = 20000.f;

	bool IsZeelichUnderGas(const FString& Name)
	{
		return Name == TEXT("MOSQUIBE") || Name == TEXT("PLATFORM") || Name == TEXT("SOUSCELB");
	}
}

FLBAEnvironmentPreset FLBAEnvironmentPreset::ForIsland(const FString& IslandName)
{
	// Intensities come from the island's terrain data (GroundRow), sky and fog colors from its palette and sky texture
	// (set by the island importer); the mood (sun tint, clouds) is set per island here.
	FLBAEnvironmentPreset Preset;
	Preset.PaletteEntry = FLBAXpl::GetEntryForIsland(IslandName);
	Preset.bClouds = true;
	const FString Name = IslandName.ToUpper();
	if (Name == TEXT("CITABAU"))
	{
		// Sunnier than the original's mid tones in the enhanced look (the desert's sand is bright enough).
		Preset.EnhancedExposure = 0.3f;
	}
	if (Name == TEXT("CITADEL"))
	{
		// Twinsun hidden by the storm: the original lights it evenly, without shadows, under a black sky.
		Preset.SunColor = FLinearColor(0.7f, 0.75f, 0.9f);
		Preset.CloudCoverage = 1.f;
		Preset.bStormClouds = true;
		Preset.bOvercast = true;
		Preset.AmbientLight = 1.f;
		Preset.MaxAmbientFraction = 0.9f;
	}
	if (Name == TEXT("CITABAU") || Name == TEXT("CITADEL") || Name == TEXT("DESERT"))
	{
		// Twinsun's seas are water. The other worlds' "seas" (Zeelich's cloud sea and red gas, the moon's ground)
		// keep the original's textured plane.
		Preset.bWater = true;
		Preset.bStormSea = Preset.bStormClouds;
		// The original's storm sea is darker than its texture (measured on Citadel's harbour).
		Preset.SeaLight = Preset.bStormSea ? 0.7f : 0.8f;
	}
	if (Name == TEXT("EMERAUDE") || Name == TEXT("MOON"))
	{
		// Emerald moon: no atmosphere.
		Preset.SunColor = FLinearColor(0.9f, 0.95f, 1.f);
		Preset.bAtmosphere = false;
		Preset.bClouds = false;
	}
	else if (IsZeelichUnderGas(Name))
	{
		// Zeelich, under the gas layer.
		Preset.SunColor = FLinearColor(1.f, 0.7f, 0.6f);
		Preset.CloudCoverage = 0.6f;
	}
	return Preset;
}

FLBAEnvironmentPreset FLBAEnvironmentPreset::Interior()
{
	// Brick sprites carry their own painted shading, so floors should receive about the neutral light (measured on
	// Twinsen's house against the original).
	FLBAEnvironmentPreset Preset;
	Preset.SunIntensity = 2.4f;
	Preset.SkyLightIntensity = 2.4f;
	return Preset;
}

FRotator LBAEnvironment::SunRotationFromLbaAngles(int32 Alpha, int32 Beta)
{
	// SetLightVector (LIGHT.CPP): the direction towards the light is the inverse rotation of +Z by (alpha, beta), so
	// (-cos a sin b, sin a, cos a cos b) in LBA space. Alpha is the light's elevation; the light travels along
	// (cos a sin b, -cos a cos b) horizontally, which is Unreal yaw beta - 90 degrees.
	const double Elevation = FMath::Clamp(LBA::AngleToDegrees(Alpha & 0xFFF), 15.0, 75.0);
	return FRotator(-Elevation, LBA::AngleToDegrees(Beta & 0xFFF) - 90.0, 0.0);
}

namespace
{
	/**
	 * Without atmosphere (space) or under an overcast sky, the sky light is a uniform gray sky: even light, like the
	 * original's, and independent of what the sky looks like.
	 */
	bool UsesUniformSkyLight(const FLBAEnvironmentPreset& Preset)
	{
		return !Preset.bAtmosphere || Preset.bOvercast;
	}

	FLinearColor GetSkyLightColor(const FLBAEnvironmentPreset& Preset)
	{
		return !Preset.bAtmosphere ? Preset.SunColor : Preset.SkyLightColor;
	}

	bool HasTintedSky(const FLBAEnvironmentPreset& Preset)
	{
		return Preset.bAtmosphere && Preset.SkyColor.GetLuminance() > 0.f;
	}
}

FLBAEnvironmentLights LBAEnvironment::ComputeLights(const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation, int32 NeutralRow)
{
	// With the manual exposure at 1 and a neutral tonemapper, a Lambert surface under a sun of E lux receives a
	// light of E * sin(elevation) / pi (1 = shows its base color), and under a uniform sky of luminance L, a light of
	// L * sky light intensity. The palette shading turns that light into a shade row.
	const float SinElevation = FMath::Max(FMath::Sin(FMath::DegreesToRadians(-SunRotation.Pitch)), 0.2f);
	FLBAEnvironmentLights Lights;
	Lights.SunIntensity = Preset.SunIntensity;
	Lights.SkyLightIntensity = Preset.SkyLightIntensity;
	if (Preset.GroundRow <= 0.f)
	{
		// Uncalibrated: only the sun's direct light is known.
		Lights.GroundLight = Lights.SunIntensity * SinElevation / UE_PI;
		return Lights;
	}
	const float SkyLuminance = UsesUniformSkyLight(Preset) ? UniformSkyLuminance * GetSkyLightColor(Preset).GetLuminance()
		: HasTintedSky(Preset) ? Preset.SkyColor.GetLuminance() : Preset.FogColor.GetLuminance();
	Lights.GroundLight = Preset.GroundRow / float(FMath::Max(NeutralRow, 1));
	Lights.AmbientLight = FMath::Min(Preset.AmbientLight, Lights.GroundLight * Preset.MaxAmbientFraction);
	Lights.SunIntensity = (Lights.GroundLight - Lights.AmbientLight) * UE_PI / (SinElevation * MeasuredSunGain);
	Lights.SkyLightIntensity = Lights.AmbientLight / (MeasuredSkyGain * FMath::Max(SkyLuminance, 0.05f));
	// A surface facing the sun gets SunIntensity * MeasuredSunGain / pi from it; the key light adds the rest.
	const float SunLightPerNdotL = Lights.SunIntensity * MeasuredSunGain / UE_PI;
	Lights.CharacterLightIntensity = FMath::Max(Preset.CharacterLight - SunLightPerNdotL, 0.f) * UE_PI / MeasuredSunGain;
	Lights.CharacterFillIntensity = FMath::Max(Preset.CharacterFill, 0.f) * UE_PI / MeasuredSunGain;
	return Lights;
}

void LBAEnvironment::Spawn(FLBAImportContext& Context, UWorld* World, const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation)
{
	const FName Folder(TEXT("Environment"));
	const FLBAXpl* Xpl = Context.GetXpl(Preset.PaletteEntry);
	const FString LevelPath = World->GetPackage()->GetName();
	const FString LevelFolder = FPackageName::GetLongPackagePath(LevelPath);
	const FString LevelName = FPackageName::GetShortName(LevelPath);
	const bool bTintedSky = HasTintedSky(Preset);
	const bool bUniformSkyLight = UsesUniformSkyLight(Preset);
	const FLinearColor SkyLightColor = GetSkyLightColor(Preset);

	const FLBAEnvironmentLights Lights = ComputeLights(Preset, SunRotation, Xpl ? Xpl->ShadeNormalLevel : 12);
	const float SunIntensity = Lights.SunIntensity;
	const float SkyLightIntensity = Lights.SkyLightIntensity;
	if (Preset.GroundRow > 0.f)
	{
		UE_LOG(LogLBA2, Display, TEXT("LBA2 environment: ground row %.1f, sun pitch %.1f -> sun %.2f lux, sky light %.2f, character key %.2f fill %.2f lux"),
			Preset.GroundRow, SunRotation.Pitch, SunIntensity, SkyLightIntensity, Lights.CharacterLightIntensity, Lights.CharacterFillIntensity);
	}

	UDirectionalLightComponent* SunLight = nullptr;
	if (ADirectionalLight* Sun = Context.SpawnGeneratedActor<ADirectionalLight>(World, FTransform(FVector(0, 0, 2000)), TEXT("Sun"), Folder))
	{
		// Set afterwards: a spawn transform would be combined with the light's default 46 degree tilt.
		Sun->SetActorRotation(SunRotation);
		SunLight = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
		SunLight->SetMobility(EComponentMobility::Movable);
		SunLight->SetIntensity(SunIntensity);
		SunLight->SetLightColor(Preset.SunColor);
		SunLight->SetAtmosphereSunLight(Preset.bAtmosphere);
		// The sun, not the brighter character key light, lights translucency, water and volumetric fog.
		SunLight->SetForwardShadingPriority(1);
		SunLight->SetDynamicShadowDistanceMovableLight(20000.f);
	}

	// Lights of the characters only (lighting channel 1), direct light only: no bounce, no fog, no atmosphere. The key
	// light makes their lit side as bright as the original's, which lights objects with N.L alone; the fill light,
	// opposite the sun and tinted like the sky, sets how dark their other side gets.
	auto SpawnCharacterLight = [&](const TCHAR* Label, const FRotator& Rotation, float Intensity, const FLinearColor& Color, bool bShadows)
	{
		if (Preset.GroundRow <= 0.f || Intensity <= 0.f)
		{
			return;
		}
		if (ADirectionalLight* Actor = Context.SpawnGeneratedActor<ADirectionalLight>(World, FTransform(FVector(0, 0, 2000)), Label, Folder))
		{
			Actor->SetActorRotation(Rotation);
			UDirectionalLightComponent* Light = CastChecked<UDirectionalLightComponent>(Actor->GetLightComponent());
			Light->SetMobility(EComponentMobility::Movable);
			Light->SetIntensity(Intensity);
			Light->SetLightColor(Color);
			Light->SetAtmosphereSunLight(false);
			Light->SetLightingChannels(false, true, false);
			Light->SetIndirectLightingIntensity(0.f);
			Light->SetVolumetricScatteringIntensity(0.f);
			Light->SetCastShadows(bShadows);
			Light->SetDynamicShadowDistanceMovableLight(5000.f);
		}
	};
	const FLinearColor FillColor = bTintedSky ? Preset.SkyColor / FMath::Max(Preset.SkyColor.GetLuminance(), 1e-4f) : SkyLightColor;
	SpawnCharacterLight(TEXT("CharacterLight"), SunRotation, Lights.CharacterLightIntensity, Preset.SunColor, true);
	SpawnCharacterLight(TEXT("CharacterFill"), FRotator(SunRotation.Pitch, SunRotation.Yaw + 180.0, 0.0), Lights.CharacterFillIntensity,
		FillColor, false);

	// The sky light captures the sky (atmosphere and clouds) in real time, or is a uniform gray sky.
	if (ASkyLight* Sky = Context.SpawnGeneratedActor<ASkyLight>(World, FTransform(FVector(0, 0, 2200)), TEXT("SkyLight"), Folder))
	{
		USkyLightComponent* Light = Sky->GetLightComponent();
		Light->SetMobility(EComponentMobility::Movable);
		Light->bRealTimeCapture = !bUniformSkyLight;
		Light->SetIntensity(SkyLightIntensity);
		if (bUniformSkyLight)
		{
			Light->SourceType = SLS_SpecifiedCubemap;
			Light->SetCubemap(LoadObject<UTextureCube>(nullptr, TEXT("/Engine/EngineResources/GrayTextureCube.GrayTextureCube")));
			Light->SetLightColor(SkyLightColor);
		}
	}

	// Unreal's sky, tinted towards the island's original sky color. It neither lights the height fog (the fog has
	// the palette's fog color) nor hazes nearby surfaces, which the palette shading would read as light.
	if (Preset.bAtmosphere)
	{
		if (ASkyAtmosphere* Atmosphere = Context.SpawnGeneratedActor<ASkyAtmosphere>(World, FTransform::Identity, TEXT("SkyAtmosphere"), Folder))
		{
			USkyAtmosphereComponent* Component = Atmosphere->GetComponent();
			Component->SetHeightFogContribution(0.f);
			Component->SetAerialPerspectiveStartDepth(2.f);
			if (bTintedSky)
			{
				const FLinearColor DefaultSky = DefaultSkyPerSunLux * FMath::Max(SunIntensity, 0.01f);
				const FLinearColor Factor(Preset.SkyColor.R / DefaultSky.R, Preset.SkyColor.G / DefaultSky.G, Preset.SkyColor.B / DefaultSky.B);
				Component->SetSkyLuminanceFactor(Factor);
			}
		}
	}

	// Unreal's volumetric clouds (default cloud material), with the island's coverage and cloud color.
	if (Preset.bClouds)
	{
		UMaterialInterface* CloudBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"));
		UMaterialInstanceConstant* Clouds = CloudBase ? Context.CreateOrReuseAsset<UMaterialInstanceConstant>(LevelFolder, FString::Printf(TEXT("MI_%s_Clouds"), *LevelName)) : nullptr;
		if (Clouds)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(Clouds, CloudBase);
			UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(Clouds);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Clouds, TEXT("Cloud_GlobalCoverage"), Preset.CloudCoverage);
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Clouds, TEXT("Cloud_AlbedoColor"), Preset.CloudColor);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Clouds, TEXT("StormClouds"), Preset.bStormClouds ? 1.f : 0.f);
			if (Preset.bStormClouds)
			{
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Clouds, TEXT("Storm_AlbedoColor"), Preset.CloudColor * 0.35f);
			}
			UMaterialEditingLibrary::UpdateMaterialInstance(Clouds);
			Context.MarkForSave(Clouds);
		}
		if (AVolumetricCloud* Cloud = Context.SpawnGeneratedActor<AVolumetricCloud>(World, FTransform::Identity, TEXT("VolumetricCloud"), Folder))
		{
			if (UVolumetricCloudComponent* Component = Cloud->FindComponentByClass<UVolumetricCloudComponent>())
			{
				Component->SetMaterial(Clouds ? static_cast<UMaterialInterface*>(Clouds) : CloudBase);
			}
		}
	}

	// Height fog in the palette's fog color, with volumetric fog near the camera lit by the same colors, so the
	// palette shading can take it off surfaces exactly; only the light shafts differ.
	UExponentialHeightFogComponent* FogComponent = nullptr;
	if (Preset.FogVisibility > 0.f)
	{
		if (AExponentialHeightFog* Fog = Context.SpawnGeneratedActor<AExponentialHeightFog>(World, FTransform::Identity, TEXT("HeightFog"), Folder))
		{
			FogComponent = Fog->GetComponent();
			// Transmittance at sea level is exp2(-Density * ln 2 * Distance), Density being FogDensity / 1000 per cm.
			FogComponent->SetFogDensity(1000.f * FMath::Log2(20.f) / (UE_LN2 * Preset.FogVisibility));
			FogComponent->SetFogHeightFalloff(FogHeightFalloff);
			FogComponent->SetStartDistance(Preset.HeightFogStart);
			FogComponent->SetFogInscatteringColor(Preset.FogColor);
			FogComponent->SkyAtmosphereAmbientContributionColorScale = FLinearColor::Black;
			FogComponent->SetDirectionalInscatteringColor(Preset.SunColor);
			FogComponent->SetVolumetricFog(true);
			FogComponent->SetVolumetricFogStartDistance(Preset.HeightFogStart);
			FogComponent->SetVolumetricFogDistance(Preset.HeightFogStart + VolumetricFogDistance);
			FogComponent->SetVolumetricFogScatteringDistribution(0.6f);
			FogComponent->bOverrideLightColorsWithFogInscatteringColors = true;
			FogComponent->MarkRenderStateDirty();
		}
	}
	const FLBAHeightFogParameters HeightFog = FLBAHeightFogParameters::Make(FogComponent, SunLight);

	// Palette looks: palette shading with world-space dither, manual exposure and a neutral tonemapper so palette
	// colors come out as they are (the enhanced look grades them itself). Both looks are blendables of the volume,
	// the inactive one at weight 0 (lba.Look.Mode switches live). Turn the volume off to compare with Unreal's plain
	// rendering. The level gets its own instances: a copy of its height fog, the original look's linear distance fog
	// (after shading, like the original's fog tables, instead of Unreal's), and the light of its sunlit ground,
	// around which the enhanced look's contrast curve turns.
	const float ContrastPivot = Preset.GroundRow > 0.f && Xpl ? Preset.GroundRow / float(Xpl->ShadeNormalLevel) : 1.f;
	auto GetLevelLook = [&](ELBALookMode Look) -> UMaterialInterface*
	{
		UMaterialInterface* PaletteShading = Xpl ? Context.GetRendering().GetPaletteShading(*Xpl, Look) : nullptr;
		if (!PaletteShading)
		{
			return nullptr;
		}
		UMaterialInstanceConstant* LevelLook = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(LevelFolder,
			FString::Printf(TEXT("MI_%s_%sLook"), *LevelName, Look == ELBALookMode::Enhanced ? TEXT("Enhanced") : TEXT("Original")));
		if (!LevelLook)
		{
			return PaletteShading;
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(LevelLook, PaletteShading);
		UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(LevelLook);
		if (Look == ELBALookMode::Original && Preset.FogEnd > Preset.FogStart)
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(LevelLook, TEXT("FogColor"), Preset.FogColor);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("FogStart"), Preset.FogStart);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("FogEnd"), Preset.FogEnd);
		}
		for (const TPair<FName, float>& Scalar : HeightFog.Scalars)
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, Scalar.Key, Scalar.Value);
		}
		for (const TPair<FName, FLinearColor>& Vector : HeightFog.Vectors)
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(LevelLook, Vector.Key, Vector.Value);
		}
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("ContrastPivot"), ContrastPivot);
		if (Look == ELBALookMode::Enhanced && Preset.EnhancedExposure != 0.f)
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("Exposure"), Preset.EnhancedExposure);
		}
		if (Preset.bWater)
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(LevelLook, TEXT("WaterColor"), Preset.SeaColor);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("WaterNeutralLight"), Preset.WaterNeutralLight);
			// Cartoon foam is as much darker than white as the sea is darker than its color, twice over (grey in the storm).
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("FoamBrightness"), FMath::Square(FMath::Min(Preset.SeaLight, 1.f)));
			if (Preset.SeaTexture)
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(LevelLook, TEXT("SeaTexture"), Preset.SeaTexture);
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("SeaTileSize"), Preset.SeaTileSize);
			}
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(LevelLook);
		Context.MarkForSave(LevelLook);
		return LevelLook;
	};
	const ELBALookMode ActiveLook = Context.GetSettings().LookMode;
	if (APostProcessVolume* Volume = Context.SpawnGeneratedActor<APostProcessVolume>(World, FTransform::Identity, TEXT("Look"), Folder))
	{
		Volume->bUnbound = true;
		FPostProcessSettings& Settings = Volume->Settings;
		Settings.bOverride_AutoExposureMethod = true;
		Settings.AutoExposureMethod = AEM_Manual;
		Settings.bOverride_AutoExposureBias = true;
		Settings.AutoExposureBias = Preset.ExposureBias;
		Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		Settings.AutoExposureApplyPhysicalCameraExposure = false;
		Settings.bOverride_ToneCurveAmount = true;
		Settings.ToneCurveAmount = 0.f;
		Settings.bOverride_ExpandGamut = true;
		Settings.ExpandGamut = 0.f;
		Settings.bOverride_BlueCorrection = true;
		Settings.BlueCorrection = 0.f;
		Settings.bOverride_LocalExposureHighlightContrastScale = true;
		Settings.LocalExposureHighlightContrastScale = 1.f;
		Settings.bOverride_LocalExposureShadowContrastScale = true;
		Settings.LocalExposureShadowContrastScale = 1.f;
		Settings.bOverride_BloomIntensity = true;
		Settings.BloomIntensity = 0.15f;
		Settings.bOverride_VignetteIntensity = true;
		Settings.VignetteIntensity = 0.f;

		for (const ELBALookMode Look : { ELBALookMode::Original, ELBALookMode::Enhanced })
		{
			if (UMaterialInterface* LevelLook = GetLevelLook(Look))
			{
				Settings.WeightedBlendables.Array.Add(FWeightedBlendable(Look == ActiveLook ? 1.f : 0.f, LevelLook));
			}
		}
	}
}
