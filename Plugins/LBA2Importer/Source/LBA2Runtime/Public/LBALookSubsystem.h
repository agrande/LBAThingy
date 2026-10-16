#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LBALookSubsystem.generated.h"

class UMaterialInstanceDynamic;

/**
 * Live controls for the "original look" post-process of generated levels (M_LBA_PaletteShading in their
 * OriginalLook post-process volume), through console variables:
 *
 *   lba.Look.Palette <0..1>   palette shading strength (-1: as set on the material)
 *   lba.Look.Dither <0..1>    world-space dither amount (-1: as set on the material)
 *   lba.Look.Debug <0..3>     0 off, 1 G-buffer base color, 2 matched palette color, 3 shade path position
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

private:
	void ApplyControls();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Instances;

	bool bInstancesCreated = false;
	float AppliedPalette = -1.f;
	float AppliedDither = -1.f;
	int32 AppliedDebug = 0;
};
