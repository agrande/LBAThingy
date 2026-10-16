#include "LBALookSubsystem.h"

#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	TAutoConsoleVariable<float> CVarLookPalette(TEXT("lba.Look.Palette"), -1.f,
		TEXT("Palette shading strength of the LBA original look (0 = Unreal's image, 1 = palette shading, -1 = as set on the material)."));
	TAutoConsoleVariable<float> CVarLookDither(TEXT("lba.Look.Dither"), -1.f,
		TEXT("World-space dither amount of the LBA original look (0 = smooth shading, 1 = dithered, -1 = as set on the material)."));
	TAutoConsoleVariable<int32> CVarLookDebug(TEXT("lba.Look.Debug"), 0,
		TEXT("LBA original look debug view: 0 off, 1 G-buffer base color, 2 matched palette color, 3 shade path position."));

	const TCHAR* PaletteShadingMaster = TEXT("M_LBA_PaletteShading");
}

bool ULBALookSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId ULBALookSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(ULBALookSubsystem, STATGROUP_Tickables);
}

void ULBALookSubsystem::Tick(float DeltaTime)
{
	const float Palette = CVarLookPalette.GetValueOnGameThread();
	const float Dither = CVarLookDither.GetValueOnGameThread();
	const int32 Debug = CVarLookDebug.GetValueOnGameThread();
	if (Palette != AppliedPalette || Dither != AppliedDither || Debug != AppliedDebug)
	{
		AppliedPalette = Palette;
		AppliedDither = Dither;
		AppliedDebug = Debug;
		ApplyControls();
	}
}

void ULBALookSubsystem::ApplyControls()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (!bInstancesCreated)
	{
		bInstancesCreated = true;
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
		{
			for (FWeightedBlendable& Blendable : It->Settings.WeightedBlendables.Array)
			{
				UMaterialInterface* Material = Cast<UMaterialInterface>(Blendable.Object);
				const UMaterial* Base = Material ? Material->GetBaseMaterial() : nullptr;
				if (Base && Base->GetName() == PaletteShadingMaster)
				{
					UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, this);
					Blendable.Object = Instance;
					Instances.Add(Instance);
				}
			}
		}
	}

	for (UMaterialInstanceDynamic* Instance : Instances)
	{
		const UMaterialInterface* Parent = Instance->Parent;
		auto Apply = [&](const TCHAR* Name, float Override)
		{
			float Value = Override;
			if (Override < 0.f && Parent)
			{
				Parent->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value);
			}
			Instance->SetScalarParameterValue(Name, Value);
		};
		Apply(TEXT("Strength"), AppliedPalette);
		Apply(TEXT("DitherAmount"), AppliedDither);
		Instance->SetScalarParameterValue(TEXT("DebugView"), float(AppliedDebug));
	}
}
