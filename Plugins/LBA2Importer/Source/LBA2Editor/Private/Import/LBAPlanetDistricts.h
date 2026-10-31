#pragma once

#include "CoreMinimal.h"

/**
 * Globe mode: LBA1's towns on the stub islands (LBAPlanetStubs), as pads of ground reserved for them (L3 of the stub
 * islands' plan; Tools/district_pads.py is the prototype). LBA1's outdoor scenes of Principal, Proxima and Rebellion
 * (Formats/LBA1.h) joined by edge exits make districts, laid in one frame. Each district is placed on its stub:
 *
 *   where    the mean of its scenes' holomap markers (one per scene; on Citadel each is 12-40 m off but their mean 13 m),
 *   which way LBA1's x east, z north (heading 0, as LBA2's islands; Citadel's LBA1 frame lies at -4 degrees), unscaled
 *            (Citadel: one LBA1 scene per LBA2 cube, districts moved rather than scaled),
 *   fitted   to the stub's coast: the offset (45 m around) and turn (30 degrees around) where its land lies on the
 *            stub's land and its water on its sea, near the markers and heading 0; largest first, off the ones before.
 *
 * Its pad: its land at LBA1's ground (the streets' level among the top bricks, over each scene's own sea level), its
 * water carved into the sea, the stub's ground eased into it over 6 m, its colors the ground's top bricks'. Land the
 * stub's painting lacks under a pad is grown, as LBA2 grew the desert around the Temple of Bu.
 */
namespace LBAPlanetDistricts
{
	/** How far (cm) a stub's grid reaches past its painted land when districts are laid on it: room for land they grow. */
	constexpr double Margin = 6000.0;

	/** A stub's ground on its grid (cm, X and Y along the map's), as LBAPlanetStubs grows it. */
	struct FGround
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double Cell = 100.0;
		int32 SizeX = 0;
		int32 SizeY = 0;
		TArray<float> Heights;
		TArray<bool> Land;

		/** Set by Lay: the pads' land and carved water, and their ground colors (sRGB). Heights and Land are changed. */
		TArray<bool> Pad;
		TArray<bool> PadWater;
		TArray<FVector3f> PadColors;
	};

	/** A district as laid on its stub. */
	struct FPlaced
	{
		FString Name;
		TArray<int32> Scenes;
		/** Where its pivot (its scenes' centers' mean) is on the map (cm). */
		FVector2D Map = FVector2D::ZeroVector;
		/** LBA1's frame of the district (its first scene's, cm: x along X, z along Y, height along Z over the sea) to the map. */
		FTransform FrameToMap;
		/** Its turn from heading 0 (degrees) and how far it moved from its markers' mean (cm). */
		double Turn = 0.0;
		double FromMarkers = 0.0;
		/** The fit: -1 to 1, its land over the stub's land and water over its sea; and those shares. */
		double Agreement = 0.0;
		double LandOnLand = 0.0;
		double WaterOnSea = 0.0;
		double LandArea = 0.0;
	};

	class FDistricts
	{
	public:
		FDistricts();
		~FDistricts();

		/** Reads LBA1's outdoor scenes from Directory (a known install when empty) for a planet of Radius (cm). */
		bool Load(const FString& Directory, double Radius);
		const FString& GetDirectory() const { return Directory; }
		bool HasIsland(const FString& Island) const;

		/** Places the island's districts on its ground and lays their pads. */
		TArray<FPlaced> Lay(const FString& Island, FGround& Ground) const;

	private:
		struct FDistrict;
		FString Directory;
		TArray<TUniquePtr<FDistrict>> Districts;
	};
}
