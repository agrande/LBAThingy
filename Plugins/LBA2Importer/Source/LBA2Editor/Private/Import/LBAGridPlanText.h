#pragma once

#include "CoreMinimal.h"

struct FLBAGridPlan;

/**
 * A grid's plan (FLBAGridPlan) as text, kept in git under the plugin's Tests/Plans (one file per grid, rewritten by
 * each import of the interiors): the rooms with their sides, the spaces, connectors and flights, then top-down maps
 * of each level (what each column is, which room or connector holds it, the air over it). A diff of these files shows
 * what a change did to the plans; the LBA2.Enclosure.Plans tests compare fresh plans with them.
 */
namespace LBAGridPlanText
{
	FString Write(const FLBAGridPlan& Plan);

	/** The plugin's Tests/Plans folder. */
	FString GetFolder();
	/** A grid's file in it (gridNNN.txt). */
	FString GetPath(int32 Grid);
}
