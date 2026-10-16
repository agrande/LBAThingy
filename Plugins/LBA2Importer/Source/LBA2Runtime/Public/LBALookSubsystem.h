#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LBALookSubsystem.generated.h"

class APostProcessVolume;
class UMaterialInstanceDynamic;

/**
 * Live controls for the palette look post-process of generated levels (M_LBA_PaletteShading instances in their Look
 * post-process volume), through console variables:
 *
 *   lba.Look.Mode <0..1>      0 original look, 1 enhanced look, in between blends their parameters (-1: as imported)
 *   lba.Look.Pattern <0|1>    dither dots: 0 fractal dots, 1 halftone screen (-1: as set on the material)
 *   lba.Look.Palette <0..1>   palette shading strength (-1: as set on the material)
 *   lba.Look.Dither <0..1>    world-space dither amount (-1: as set on the material)
 *   lba.Look.Debug <0..3>     0 off, 1 G-buffer base color, 2 matched palette color, 3 shade path position
 *   lba.Look.Set <Parameter> <Value...>   overrides any scalar (one value) or vector (3-4 values) parameter of both
 *                             looks; without a value, removes the override. To try settings before putting them in
 *                             MI_LBA_Look_Enhanced.
 *
 * The volume's blendables are swapped for dynamic instances the first time a control is used.
 */
UCLASS()
class LBA2RUNTIME_API ULBALookSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	/** Overrides a look parameter (see lba.Look.Set); an empty Value removes the override. */
	void SetParameterOverride(FName Name, const TArray<float>& Value);

private:
	void ApplyControls();

	TMap<FName, TArray<float>> ParameterOverrides;

	struct FLookBlendable
	{
		TWeakObjectPtr<APostProcessVolume> Volume;
		int32 Index = 0;
		TWeakObjectPtr<UMaterialInstanceDynamic> Instance;
		float ImportedWeight = 0.f;
		bool bEnhanced = false;
	};
	TArray<FLookBlendable> Blendables;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Instances;

	bool bInstancesCreated = false;
	float AppliedMode = -1.f;
	float AppliedPattern = -1.f;
	float AppliedPalette = -1.f;
	float AppliedDither = -1.f;
	int32 AppliedDebug = 0;
};
