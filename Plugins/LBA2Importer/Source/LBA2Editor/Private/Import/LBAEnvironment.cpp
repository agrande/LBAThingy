#include "Import/LBAEnvironment.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Formats/LBAXpl.h"
#include "Import/LBAImportContext.h"
#include "Import/LBARenderingAssets.h"
#include "LBA2Runtime.h"
#include "LBATypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"

namespace
{
	/** Far enough to enclose any island, close enough to stay well inside the far clip. */
	constexpr double SkyDomeRadius = 500000.0;
}

FLBAEnvironmentPreset FLBAEnvironmentPreset::ForIsland(const FString& IslandName)
{
	// Intensities come from the island's terrain data (GroundRow, set by the island importer); only the sun's tint
	// is set per island.
	FLBAEnvironmentPreset Preset;
	Preset.PaletteEntry = FLBAXpl::GetEntryForIsland(IslandName);
	const FString Name = IslandName.ToUpper();
	if (Name == TEXT("CITADEL"))
	{
		// Twinsun hidden by the storm.
		Preset.SunColor = FLinearColor(0.7f, 0.75f, 0.9f);
	}
	else if (Name == TEXT("EMERAUDE") || Name == TEXT("MOON"))
	{
		// Emerald moon: no atmosphere.
		Preset.SunColor = FLinearColor(0.9f, 0.95f, 1.f);
		Preset.bAtmosphere = false;
	}
	else if (Name == TEXT("MOSQUIBE") || Name == TEXT("PLATFORM") || Name == TEXT("SOUSCELB"))
	{
		// Zeelich, under the gas layer.
		Preset.SunColor = FLinearColor(1.f, 0.7f, 0.6f);
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
	// Alpha tilts the light down from the horizon, beta turns it around the vertical axis.
	const double Elevation = FMath::Clamp(LBA::AngleToDegrees(Alpha & 0xFFF), 15.0, 75.0);
	return FRotator(-Elevation, -LBA::AngleToDegrees(Beta & 0xFFF), 0.0);
}

void LBAEnvironment::Spawn(FLBAImportContext& Context, UWorld* World, const FLBAEnvironmentPreset& Preset, const FRotator& SunRotation)
{
	const FName Folder(TEXT("Environment"));
	const FLBAXpl* Xpl = Context.GetXpl(Preset.PaletteEntry);

	// With the manual exposure at 1 and a neutral tonemapper, a Lambert surface under a sun of E lux receives a
	// light of E * sin(elevation) / pi (1 = shows its base color), and under a uniform sky dome of luminance L, a
	// light of L * sky light intensity. The palette shading turns that light into a shade row.
	float SunIntensity = Preset.SunIntensity;
	float SkyLightIntensity = Preset.SkyLightIntensity;
	if (Preset.GroundRow > 0.f)
	{
		const float NeutralRow = Xpl ? float(Xpl->ShadeNormalLevel) : 12.f;
		const float GroundLight = Preset.GroundRow / NeutralRow;
		const float Ambient = FMath::Min(Preset.AmbientLight, GroundLight * 0.6f);
		const float SinElevation = FMath::Max(FMath::Sin(FMath::DegreesToRadians(-SunRotation.Pitch)), 0.2f);
		// Measured on Citabau and the desert (lba.Look.Debug 3): ground ends up about 1.5 times brighter than this
		// model predicts, from Lumen's bounce light, the brighter clouds and slopes facing the sun.
		constexpr float MeasuredLightGain = 1.5f;
		SunIntensity = (GroundLight - Ambient) * UE_PI / (SinElevation * MeasuredLightGain);
		SkyLightIntensity = Ambient / (MeasuredLightGain * FMath::Max(Preset.FogColor.GetLuminance(), 0.05f));
		UE_LOG(LogLBA2, Display, TEXT("LBA2 environment: ground row %.1f, sun pitch %.1f -> sun %.2f lux, sky light %.2f"), Preset.GroundRow, SunRotation.Pitch, SunIntensity, SkyLightIntensity);
	}

	ADirectionalLight* Sun = Context.SpawnGeneratedActor<ADirectionalLight>(World, FTransform(SunRotation, FVector(0, 0, 2000)), TEXT("Sun"), Folder);
	if (Sun)
	{
		UDirectionalLightComponent* Light = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensity(SunIntensity);
		Light->SetLightColor(Preset.SunColor);
		Light->SetAtmosphereSunLight(Preset.bAtmosphere && !Preset.SkyMaterial);
		Light->SetDynamicShadowDistanceMovableLight(20000.f);
	}

	// The sky light captures the sky in real time: the original-style dome (an "Is Sky" material) or the atmosphere.
	if (ASkyLight* Sky = Context.SpawnGeneratedActor<ASkyLight>(World, FTransform(FVector(0, 0, 2200)), TEXT("SkyLight"), Folder))
	{
		USkyLightComponent* Light = Sky->GetLightComponent();
		Light->SetMobility(EComponentMobility::Movable);
		Light->bRealTimeCapture = true;
		Light->SetIntensity(SkyLightIntensity);
		if (!Preset.bAtmosphere && !Preset.SkyMaterial)
		{
			Light->SourceType = SLS_SpecifiedCubemap;
			Light->SetLowerHemisphereColor(FLinearColor::Black);
		}
	}

	if (Preset.SkyMaterial)
	{
		UStaticMesh* Dome = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineSky/SM_SkySphere.SM_SkySphere"));
		const double MeshRadius = Dome ? FMath::Max(Dome->GetBounds().SphereRadius, 1.0) : 1.0;
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World,
			FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(SkyDomeRadius / MeshRadius)), TEXT("SkyDome"), Folder))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetMobility(EComponentMobility::Static);
			Component->SetStaticMesh(Dome);
			Component->SetMaterial(0, Preset.SkyMaterial);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCastShadow(false);
			Component->bAffectDistanceFieldLighting = false;
			Component->bAffectDynamicIndirectLighting = false;
			Component->bVisibleInRayTracing = false;
		}
	}
	else if (Preset.bAtmosphere)
	{
		Context.SpawnGeneratedActor<ASkyAtmosphere>(World, FTransform::Identity, TEXT("SkyAtmosphere"), Folder);
	}

	// Palette look: palette shading with world-space dither, manual exposure and a neutral tonemapper so palette
	// colors come out as they are (the enhanced look grades them itself). Both looks are blendables of the volume,
	// the inactive one at weight 0 (lba.Look.Mode switches live). Turn the volume off to compare with Unreal's plain
	// rendering. The palette shading also applies the original's linear distance fog, after shading like its fog
	// tables: fog added before it would read as extra light. The level gets its own instances for its fog distances,
	// and for the light of its sunlit ground, around which the enhanced look's contrast curve turns.
	const FString LevelPath = World->GetPackage()->GetName();
	const float ContrastPivot = Preset.GroundRow > 0.f && Xpl ? Preset.GroundRow / float(Xpl->ShadeNormalLevel) : 1.f;
	auto GetLevelLook = [&](ELBALookMode Look) -> UMaterialInterface*
	{
		UMaterialInterface* PaletteShading = Xpl ? Context.GetRendering().GetPaletteShading(*Xpl, Look) : nullptr;
		if (!PaletteShading)
		{
			return nullptr;
		}
		UMaterialInstanceConstant* LevelLook = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(FPackageName::GetLongPackagePath(LevelPath),
			FString::Printf(TEXT("MI_%s_%sLook"), *FPackageName::GetShortName(LevelPath), Look == ELBALookMode::Enhanced ? TEXT("Enhanced") : TEXT("Original")));
		if (!LevelLook)
		{
			return PaletteShading;
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(LevelLook, PaletteShading);
		UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(LevelLook);
		if (Preset.FogEnd > Preset.FogStart)
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(LevelLook, TEXT("FogColor"), Preset.FogColor);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("FogStart"), Preset.FogStart);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("FogEnd"), Preset.FogEnd);
		}
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(LevelLook, TEXT("ContrastPivot"), ContrastPivot);
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
