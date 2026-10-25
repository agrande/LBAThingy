#pragma once

#include "CoreMinimal.h"

/**
 * The editor's views of the grid plans (FLBAGridPlan): the LBA2 Plan Browser tab (Tools > LBA2 Plan Browser, console
 * lba.PlanBrowser [grid]) and picking in the plan overlay of the grid levels (FLBAGridPlanVisualizer).
 */
namespace LBAPlanViews
{
	void Register();
	void Unregister();
}
