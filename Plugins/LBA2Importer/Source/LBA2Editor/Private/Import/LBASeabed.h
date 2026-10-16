#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class UMaterialInterface;
class UStaticMesh;

/** What a seabed is generated from. */
struct FLBASeabedSettings
{
	/** Island-global terrain vertices it covers (Min inclusive, Max exclusive). */
	FIntRect Vertices;
	/** Height of a terrain vertex above the sea (cm); land is above 0. */
	TFunction<float(int32 GX, int32 GZ)> Height;
	/** Distance between terrain vertices (cm). */
	float VertexSpacing = 64.f;
	/** Seabed color (linear albedo), varied a little with noise. */
	FLinearColor Color = FLinearColor(0.5f, 0.45f, 0.3f);
	UMaterialInterface* Material = nullptr;
};

namespace LBASeabed
{
	/**
	 * The island has no ground below the sea: this builds a seabed sloping down from the coast, so the water gets
	 * shallows along the shore (lighter water, shore depth for the water's foam and waves) and deepens away from it.
	 * Low shores (beaches) get a shallow shelf; steep ones (quays, cliffs) drop into deep water right away.
	 * Only the coastal band is built: open sea further out stays bottomless. Returns null when there is no coast.
	 */
	UStaticMesh* Build(FLBAImportContext& Context, const FLBASeabedSettings& Settings, const FString& Folder, const FString& Name);
}
