#include "Import/LBAEnvironment.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Import/LBAImportContext.h"
#include "LBATypes.h"

FLBAEnvironmentPreset FLBAEnvironmentPreset::ForIsland(const FString& IslandName)
{
	FLBAEnvironmentPreset Preset;
	const FString Name = IslandName.ToUpper();
	if (Name == TEXT("CITADEL"))
	{
		// Twinsun, storm.
		Preset.SunColor = FLinearColor(0.55f, 0.6f, 0.75f);
		Preset.SunIntensity = 2.f;
		Preset.FogColor = FLinearColor(0.08f, 0.09f, 0.1f);
		Preset.FogDensity = 0.05f;
		Preset.SkyLightIntensity = 0.6f;
		Preset.WaterColor = FLinearColor(0.02f, 0.05f, 0.07f);
	}
	else if (Name == TEXT("EMERAUDE") || Name == TEXT("MOON"))
	{
		// Emerald moon: no atmosphere, stars.
		Preset.SunColor = FLinearColor(0.9f, 0.95f, 1.f);
		Preset.SunIntensity = 6.f;
		Preset.FogColor = FLinearColor(0.f, 0.f, 0.f);
		Preset.FogDensity = 0.f;
		Preset.bAtmosphere = false;
		Preset.bClouds = false;
		Preset.SkyLightIntensity = 0.4f;
	}
	else if (Name == TEXT("MOSQUIBE") || Name == TEXT("PLATFORM") || Name == TEXT("SOUSCELB"))
	{
		// Zeelich, under the gas layer: red sky.
		Preset.SunColor = FLinearColor(1.f, 0.55f, 0.4f);
		Preset.SunIntensity = 4.f;
		Preset.FogColor = FLinearColor(0.44f, 0.05f, 0.03f);
		Preset.FogDensity = 0.045f;
		Preset.WaterColor = FLinearColor(0.12f, 0.03f, 0.02f);
	}
	else if (Name == TEXT("OTRINGAL") || Name == TEXT("KNARTAS") || Name == TEXT("ILOTCX") || Name == TEXT("CELEBRAT")
		|| Name == TEXT("CELEBRA2") || Name == TEXT("ASCENCE"))
	{
		// Zeelich surface: purple-grey haze.
		Preset.SunColor = FLinearColor(0.95f, 0.85f, 0.95f);
		Preset.SunIntensity = 5.f;
		Preset.FogColor = FLinearColor(0.45f, 0.41f, 0.48f);
		Preset.FogDensity = 0.035f;
		Preset.WaterColor = FLinearColor(0.08f, 0.07f, 0.12f);
	}
	return Preset;
}

FLBAEnvironmentPreset FLBAEnvironmentPreset::Interior()
{
	FLBAEnvironmentPreset Preset;
	Preset.SunIntensity = 3.f;
	Preset.FogDensity = 0.f;
	Preset.bClouds = false;
	Preset.SkyLightIntensity = 1.5f;
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

	ADirectionalLight* Sun = Context.SpawnGeneratedActor<ADirectionalLight>(World, FTransform(SunRotation, FVector(0, 0, 2000)), TEXT("Sun"), Folder);
	if (Sun)
	{
		UDirectionalLightComponent* Light = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensity(Preset.SunIntensity);
		Light->SetLightColor(Preset.SunColor);
		Light->SetAtmosphereSunLight(true);
		Light->SetDynamicShadowDistanceMovableLight(20000.f);
	}

	if (ASkyLight* Sky = Context.SpawnGeneratedActor<ASkyLight>(World, FTransform(FVector(0, 0, 2200)), TEXT("SkyLight"), Folder))
	{
		USkyLightComponent* Light = Sky->GetLightComponent();
		Light->SetMobility(EComponentMobility::Movable);
		Light->bRealTimeCapture = true;
		Light->SetIntensity(Preset.SkyLightIntensity);
		if (!Preset.bAtmosphere)
		{
			Light->SourceType = SLS_SpecifiedCubemap;
			Light->SetLowerHemisphereColor(FLinearColor::Black);
		}
	}

	if (Preset.bAtmosphere)
	{
		Context.SpawnGeneratedActor<ASkyAtmosphere>(World, FTransform::Identity, TEXT("SkyAtmosphere"), Folder);
	}

	if (Preset.FogDensity > 0.f)
	{
		if (AExponentialHeightFog* Fog = Context.SpawnGeneratedActor<AExponentialHeightFog>(World, FTransform(FVector(0, 0, 0)), TEXT("HeightFog"), Folder))
		{
			UExponentialHeightFogComponent* Component = Fog->GetComponent();
			Component->SetFogDensity(Preset.FogDensity);
			Component->SetFogHeightFalloff(0.05f);
			Component->SetFogInscatteringColor(Preset.FogColor);
		}
	}

	if (Preset.bClouds && Preset.bAtmosphere)
	{
		Context.SpawnGeneratedActor<AVolumetricCloud>(World, FTransform::Identity, TEXT("Clouds"), Folder);
	}
}
