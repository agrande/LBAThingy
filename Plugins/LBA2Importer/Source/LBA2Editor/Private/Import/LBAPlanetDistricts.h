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
 * Its pad: its land under LBA1's bricks (their lowest, or the streets' level among the top bricks where that is lower: under
 * a roof), its water carved into the sea. Along the pad's edge the ground comes up to the streets' top, and the stub's ground
 * eases into that gently and unevenly: a town has no rectangular outline. Its kinds of ground are the top bricks'.
 * Land the stub's painting lacks under a pad is grown, as LBA2 grew the desert around the Temple of Bu.
 *
 * Its town (L4): the bricks of its scenes in its frame, as LBA1 joins them (its scenes' sea levels differ: Principal's
 * are 0 and 3 cells in one frame, so a quay may stand 1 m over the sea), each column from the scene drawn over it.
 */
namespace LBA1
{
	class FData;
}

namespace LBAPlanetDistricts
{
	/** How far (cm) a stub's grid reaches past its painted land when districts are laid on it: room for land they grow. */
	constexpr double Margin = 6000.0;

	/** How much of each kind of ground (M_LBA_StubGround's) a flat tile of LBA1's is, 0 to 1. */
	struct FGroundShares
	{
		float Grass = 0.f;
		float Earth = 0.f;
		float Dark = 0.f;
	};

	/** A stub's ground on its grid (cm, X and Y along the map's), as LBAPlanetStubs grows it. */
	struct FGround
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double Cell = 100.0;
		int32 SizeX = 0;
		int32 SizeY = 0;
		TArray<float> Heights;
		TArray<bool> Land;

		/** Set by Lay: the pads' land and carved water. Heights and Land are changed. */
		TArray<bool> Pad;
		TArray<bool> PadWater;
		/** Set by Lay: where a pad decides the kinds of ground (itself and the land just off its natural ground), and those. */
		TArray<bool> Kinded;
		TArray<FGroundShares> Kinds;
	};

	/** A district as laid on its stub. */
	struct FPlaced
	{
		/** Which of the districts (FDistricts::MakeTown). */
		int32 District = INDEX_NONE;
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

	/** A district's bricks in its frame: cells (x, y up, z) from Low (frame cells: 64 cm across, 32 cm up). */
	struct FTown
	{
		FIntVector Low = FIntVector::ZeroValue;
		FIntVector Size = FIntVector::ZeroValue;
		/** Per cell (Index): LBA1's brick (1-based, 0 = none) and its collision shape. */
		TArray<int32> Bricks;
		TArray<uint8> Shapes;
		/** Per column (z * Size.X + x): the scene drawn there (INDEX_NONE: none). */
		TArray<int32> Scenes;

		int32 Index(int32 X, int32 Y, int32 Z) const { return (Z * Size.X + X) * Size.Y + Y; }
		/** Where cell 0, 0, 0 is in the frame (cm, as FPlaced::FrameToMap takes it). */
		FVector GetOrigin() const;
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

		/** A placed district's bricks: its land's, not its sea's (the planet's sea is there); quays reach under the sea. */
		bool MakeTown(const FPlaced& Placed, FTown& Out) const;
		/** LBA1's data, for the towns' bricks (after Load). */
		const LBA1::FData& GetData() const { return *Data; }

		/**
		 * A brick that is ground: a flat tile of grass, earth or a mix of both, or dark ground, drawn as terrain instead of as a
		 * brick (LBAPlanetTowns). Null for anything else, paving included: its courses follow the street, and the bricks draw them.
		 */
		const FGroundShares* GetGroundShares(int32 Brick) const;

	private:
		struct FDistrict;
		TUniquePtr<LBA1::FData> Data;
		FString Directory;
		mutable TMap<int32, TOptional<FGroundShares>> GroundBricks;
		TArray<TUniquePtr<FDistrict>> Districts;
	};
}
