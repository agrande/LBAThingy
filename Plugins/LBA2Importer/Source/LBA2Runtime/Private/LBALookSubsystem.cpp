#include "LBALookSubsystem.h"

#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	TAutoConsoleVariable<float> CVarLookMode(TEXT("lba.Look.Mode"), -1.f,
		TEXT("LBA look of generated levels: 0 original, 1 enhanced, in between blends them (-1 = as imported)."));
	TAutoConsoleVariable<float> CVarLookPattern(TEXT("lba.Look.Pattern"), -1.f,
		TEXT("LBA look dither dots: 0 fractal dots, 1 halftone screen (-1 = as set on the material)."));
	TAutoConsoleVariable<float> CVarLookPalette(TEXT("lba.Look.Palette"), -1.f,
		TEXT("Palette shading strength of the LBA look (0 = Unreal's image, 1 = palette shading, -1 = as set on the material)."));
	TAutoConsoleVariable<float> CVarLookDither(TEXT("lba.Look.Dither"), -1.f,
		TEXT("World-space dither amount of the LBA look (0 = smooth shading, 1 = dithered, -1 = as set on the material)."));
	TAutoConsoleVariable<int32> CVarLookDebug(TEXT("lba.Look.Debug"), 0,
		TEXT("LBA look debug view: 0 off, 1 G-buffer base color, 2 matched palette color, 3 shade path position."));

	FAutoConsoleCommandWithWorldAndArgs CmdLookSet(TEXT("lba.Look.Set"),
		TEXT("lba.Look.Set <Parameter> [Value | R G B [A]]: overrides a parameter of the LBA looks (no value: remove the override)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ULBALookSubsystem* Subsystem = World && Args.Num() > 0 ? World->GetSubsystem<ULBALookSubsystem>() : nullptr;
			if (Subsystem)
			{
				TArray<float> Value;
				for (int32 Index = 1; Index < Args.Num(); ++Index)
				{
					Value.Add(FCString::Atof(*Args[Index]));
				}
				Subsystem->SetParameterOverride(FName(*Args[0]), Value);
			}
		}));

	const TCHAR* PaletteShadingMaster = TEXT("M_LBA_PaletteShading");
	const TCHAR* EnhancedLook = TEXT("MI_LBA_Look_Enhanced");

	bool IsEnhancedLook(const UMaterialInterface* Material)
	{
		for (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material); Instance; Instance = Cast<UMaterialInstance>(Instance->Parent))
		{
			if (Instance->GetName() == EnhancedLook)
			{
				return true;
			}
		}
		return false;
	}
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
	const float Mode = CVarLookMode.GetValueOnGameThread();
	const float Pattern = CVarLookPattern.GetValueOnGameThread();
	const float Palette = CVarLookPalette.GetValueOnGameThread();
	const float Dither = CVarLookDither.GetValueOnGameThread();
	const int32 Debug = CVarLookDebug.GetValueOnGameThread();
	if (Mode != AppliedMode || Pattern != AppliedPattern || Palette != AppliedPalette || Dither != AppliedDither || Debug != AppliedDebug)
	{
		AppliedMode = Mode;
		AppliedPattern = Pattern;
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
			TArray<FWeightedBlendable>& Array = It->Settings.WeightedBlendables.Array;
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				UMaterialInterface* Material = Cast<UMaterialInterface>(Array[Index].Object);
				const UMaterial* Base = Material ? Material->GetBaseMaterial() : nullptr;
				if (Base && Base->GetName() == PaletteShadingMaster)
				{
					UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, this);
					Array[Index].Object = Instance;
					Instances.Add(Instance);
					Blendables.Add({ *It, Index, Instance, Array[Index].Weight, IsEnhancedLook(Material) });
				}
			}
		}
	}

	for (const FLookBlendable& Look : Blendables)
	{
		APostProcessVolume* Volume = Look.Volume.Get();
		UMaterialInstanceDynamic* Instance = Look.Instance.Get();
		if (!Volume || !Instance || !Volume->Settings.WeightedBlendables.Array.IsValidIndex(Look.Index))
		{
			continue;
		}

		// Blendables of the same material blend their parameters by weight, so a mode between 0 and 1 mixes the looks.
		const float Mode = FMath::Clamp(AppliedMode, 0.f, 1.f);
		Volume->Settings.WeightedBlendables.Array[Look.Index].Weight = AppliedMode < 0.f ? Look.ImportedWeight : (Look.bEnhanced ? Mode : 1.f - Mode);

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
		Apply(TEXT("DotPattern"), AppliedPattern);
		Apply(TEXT("Strength"), AppliedPalette);
		Apply(TEXT("DitherAmount"), AppliedDither);
		Instance->SetScalarParameterValue(TEXT("DebugView"), float(AppliedDebug));
		for (const TPair<FName, TArray<float>>& Override : ParameterOverrides)
		{
			const TArray<float>& V = Override.Value;
			if (V.Num() >= 3)
			{
				Instance->SetVectorParameterValue(Override.Key, FLinearColor(V[0], V[1], V[2], V.Num() > 3 ? V[3] : 1.f));
			}
			else
			{
				Instance->SetScalarParameterValue(Override.Key, V[0]);
			}
		}
	}
}

void ULBALookSubsystem::SetParameterOverride(FName Name, const TArray<float>& Value)
{
	if (Value.IsEmpty())
	{
		ParameterOverrides.Remove(Name);
		// Back to the imported value.
		for (const FLookBlendable& Look : Blendables)
		{
			if (UMaterialInstanceDynamic* Instance = Look.Instance.Get())
			{
				Instance->ClearParameterValues();
			}
		}
	}
	else
	{
		ParameterOverrides.Add(Name, Value);
	}
	ApplyControls();
}
