#pragma once

#include "Engine/DeveloperSettings.h"
#include "LBA2Settings.generated.h"

/**
 * Project-wide LBA2 settings shared by the importer and the gameplay classes.
 * Lives in Project Settings > Plugins > LBA2.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "LBA2"))
class LBA2RUNTIME_API ULBA2Settings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULBA2Settings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/**
	 * Centimeters per LBA world unit. A brick is 512x256x512 units and Twinsen is about 1240 units tall.
	 * The default (0.125) makes a brick 64x32x64 cm, which snaps nicely to power-of-two editor grids.
	 * Changing this only affects assets imported afterwards.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "World", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float WorldScale = 0.125f;

	static float GetWorldScale() { return GetDefault<ULBA2Settings>()->WorldScale; }
};
