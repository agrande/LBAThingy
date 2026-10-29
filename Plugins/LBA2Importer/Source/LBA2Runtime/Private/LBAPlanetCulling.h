#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

/**
 * Globe mode's culling (ULBAPlanetSubsystem). The renderer culls by bounds in the flat world, but the bend draws far
 * things lower and nearer than they are there (LBAPlanet.ush): seen from high up (the Dinofly), a far island lies above
 * the top of the screen in the flat world while the bend shows it in the middle, so it was culled. Each frame, the
 * view's frustum is widened to every flat place the bend can bring into it, and occlusion queries (flat boxes against
 * the bent depth) are off while the view is bent.
 *
 *   lba.Globe.BentCulling 0     the renderer's own culling (to compare)
 *   lba.Globe.CullBelow M       how deep below the sea things are still seen (m)
 *   lba.Globe.CullAbove M       how high above the sea things are (m)
 */
class FLBAPlanetViewExtension : public FWorldSceneViewExtension
{
public:
	FLBAPlanetViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	/** The bend this frame, as MPC_LBA_Planet has it (cm): the camera, radius, flat radius, sea level, whether on. */
	void SetBend(const FVector& InView, double InRadius, double InFlatRadius, double InSeaLevel, bool bInBent);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;

private:
	bool IsCulling() const;
	/**
	 * Calls Visit with the flat places (from the camera) of what the bend shows along a view ray (Direction, unit) that
	 * are between CullBelow and CullAbove: the ray itself near the camera, then its bent part unbent, sampled.
	 */
	template <typename FVisit>
	void ForEachFlatPlace(const FVector& Direction, FVisit&& Visit) const;

	FVector View = FVector::ZeroVector;
	double Radius = 0.0;
	double FlatRadius = 0.0;
	double SeaLevel = 0.0;
	bool bBent = false;
};
