#pragma once

#include "CoreMinimal.h"

/**
 * A showcase place the travel menu lists first (its Points of interest tab): either an interior scene's hero start, or
 * a spot on a level, given in the coordinates of one of its sub-levels so it follows that sub-level when the planet is
 * rebuilt with another layout (the stub islands and the belt are laid around the pole).
 */
struct LBA2RUNTIME_API FLBAPointOfInterest
{
	/** For lba.POI and travel gateways. */
	FName Id;
	FText Name;
	FText Description;

	/** An interior: goes to this scene's hero start (Level is then unused). -1 for a spot on a level. */
	int32 Scene = INDEX_NONE;

	/** Package of the level opened (the planet). */
	FString Level;
	/** Package of the sub-level whose coordinates Feet and Yaw are in, empty for Level's own. */
	FString Frame;
	/** Twinsen's feet (cm), snapped to the ground below a little above it. */
	FVector Feet = FVector::ZeroVector;
	/** Where Twinsen and the camera look (degrees). */
	double Yaw = 0.0;
};

namespace LBAPointsOfInterest
{
	/** The showcase places, in menu order (hardcoded in LBAPointsOfInterest.cpp). */
	LBA2RUNTIME_API const TArray<FLBAPointOfInterest>& Get();

	LBA2RUNTIME_API const FLBAPointOfInterest* Find(FName Id);
}
