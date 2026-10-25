#pragma once

#include "ComponentVisualizer.h"
#include "CoreMinimal.h"

class FColoredMaterialRenderProxy;

/**
 * Picking in a grid plan's overlay (ULBAGridPlanComponent): with its actor selected, a click on a room's floor or a
 * connector's column selects it, highlighted, its data in the Details panel.
 */
class FLBAGridPlanVisualizer : public FComponentVisualizer
{
public:
	FLBAGridPlanVisualizer();
	virtual ~FLBAGridPlanVisualizer() override;

	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual bool VisProxyHandleClick(FEditorViewportClient* InViewportClient, HComponentVisProxy* VisProxy, const FViewportClick& Click) override;

private:
	/** Nearly clear: the pick areas are drawn for their hit proxies, the overlay shows the colors. */
	TUniquePtr<FColoredMaterialRenderProxy> PickMaterial;
};
