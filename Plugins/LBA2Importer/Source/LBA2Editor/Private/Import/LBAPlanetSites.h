#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class FLBAMeshBuilder;
struct FLBAVertex;
class UMaterialInterface;
class UWorld;
namespace LBA1
{
	class FData;
}
namespace LBAPlanetDistricts
{
	class FDistricts;
	struct FPlaced;
}

/**
 * Globe mode: places of LBA1's islands that are reached by road rather than built as towns (Phase 5 of
 * LBA1_ISLANDS_PLAN.md; prototype Tools/rune_sites.py), and the ferry's way into LBA1's harbours.
 *
 * Proxima's rune stones: the Marked Stone (LBA1 scene 46) on the Hamalayi side, behind a rock with a round hole at its
 * foot, reached by buggy; the Eclipse Stone (scene 45) on the desert side, out on a low spit, reached by motorbike. Each
 * starts at its stone's LBA1 holomap marker and moves to the tip of painted land nearest to it; from the tip (or from
 * where the hole comes out of the rock) a causeway runs out to a small round islet with the stone, as in LBA1. They are
 * shapes in the stub's ground (LBAPlanetStubs: its land before the coast is drawn, its heights before the coast and the
 * pads are laid), so the grammar's coasts, shelf and ground kinds treat them as any other land.
 *
 * Roads: from Proxim City's pad, by the cheapest path over the ground (length, weighted by the climb; a cliff costs
 * dearly, and the road cuts a ramp where it must cross one), graded to a gentle climb and laid in as earth with grass
 * verges and a row of round boulders along each edge, on along the causeways.
 *
 * The rock and its hole: a ridge across the arm (its ends run out into the sea), a mesh of its own in the stub's ground mesh
 * on a fine grid (20 cm: a 2 m hole in a 6.5 m face is finer than the ground's 1 m cells), rising out of the ground it
 * stands on: a wandering outline, steep faces with ledges, a grassy top, lower toward its ends. The hole is cut out of its
 * face and a bore with an uneven arched roof runs through, its ends meeting the faces where they are. Same material as the
 * ground (steep is rock, streaked from above like the cliffs). LBA2's pieces were tried first: Otringal's vault (decor 111,
 * its dark twin 143) is closed at one end, not a way through.
 *
 * Roads start where LBA1 does: where the stone's scenes lead back into the town (Proxim City's tarmac openings on its north
 * and south edges, FDistricts::FindWayTo).
 *
 * Harbours: from where LBA1's ferry docks (FPlaced::Docks), the widest way out to the open sea, preferring water; the
 * stub's own land within reach of it is cleared (never a pad's), so the ferry has room to come in.
 */
namespace LBAPlanetSites
{
	/** An island's grid (as LBAPlanetStubs grows it): cm, X and Y along the map's. */
	struct FGridInfo
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double Cell = 100.0;
		int32 SizeX = 0;
		int32 SizeY = 0;
	};

	struct FSite
	{
		FString Name;
		int32 Scene = INDEX_NONE;
		/** Behind a rock with a hole (the Marked Stone). */
		bool bRock = false;
		/** On the map (cm): the marker, the land's tip, the way out (unit), where the causeway starts, the islet. */
		FVector2D Marker = FVector2D::ZeroVector;
		FVector2D Tip = FVector2D::ZeroVector;
		FVector2D Dir = FVector2D::UnitX();
		FVector2D Shore = FVector2D::ZeroVector;
		FVector2D Islet = FVector2D::ZeroVector;
		/** A rock's ground at its near and far mouths (cm): its bore's floor slopes between them. */
		double NearGround = 0.0;
		double FarGround = 0.0;
		/** The ridge: its middle (its near face at the tip) and how far it reaches either way across (cm, -Side and +Side). */
		FVector2D RidgeCenter = FVector2D::ZeroVector;
		double RidgeLeft = 0.0;
		double RidgeRight = 0.0;
		/** The LBA1 scene the town leads to on the way (FDistricts::FindWayTo), and where the road leaves the town for it. */
		int32 FromScene = INDEX_NONE;
		bool bWay = false;
		FVector2D WayPoint = FVector2D::ZeroVector;
		FVector2D WayDir = FVector2D::UnitX();
		/** The road's graded centerline (map cm, Z its height), from the town to its end. */
		TArray<FVector> Road;
		double RoadLength = 0.0;
		double Steepest = 0.0;
	};

	/** A round boulder: center on the map (cm, Z the ground) and radius. */
	struct FBoulder
	{
		FVector Center = FVector::ZeroVector;
		double Radius = 40.0;
		int32 Seed = 0;
	};

	class FSites
	{
	public:
		/** The sites' markers (LBA1's holomap) on a planet of Radius (cm). */
		void Load(const LBA1::FData& Data, double Radius);
		bool HasIsland(const FString& Island) const;

		/** Takes the island's grid (call it for every island) and finds its sites, if any, on its painted land (per cell, before the coast is drawn). */
		void Find(const FString& Island, const FGridInfo& Grid, const TArray<bool>& Land);
		/** Grows LandField (land where > 0) with the islets, causeways and rocks. */
		void AddLand(TArray<float>& LandField) const;
		/** After the benches: causeways and islets low and level, the ground under the rocks flattened (their bores' floor). */
		void ShapeHeights(TArray<float>& Heights, const TArray<bool>& Land);
		/**
		 * After the pads are laid: clears the stub's land along the widest way from each dock out to the open sea (never a
		 * pad's land). Returns the cells cleared.
		 */
		int32 OpenHarbours(const FString& Island, const TArray<FVector2D>& Docks, TArray<bool>& Land, const TArray<bool>& Pad);
		/** The ferry's ways in (per cell, after OpenHarbours): kept clear of causeways. */
		const TArray<bool>& GetChannels() const { return Channels; }
		/** After the pads are laid: where each site's road leaves its town (LBA1's way back into it). */
		void FindWays(const LBAPlanetDistricts::FDistricts& Districts, const TArray<LBAPlanetDistricts::FPlaced>& Placed);
		/**
		 * After the coast: the roads from the pads (Pad, may be empty) to the sites, graded into Heights. Where a road must cross
		 * water it is a causeway (Land grows under it, at the shore's height), as LBA1's are; never across Avoid (a harbour's
		 * water, the ferry's ways in). RoadShare and VergeShare (per cell, 0 or 1) are where the ground is the road's earth and
		 * its verges' grass.
		 */
		void LayRoads(TArray<float>& Heights, TArray<bool>& Land, const TArray<bool>& Pad, const TArray<bool>& Avoid, TArray<float>& RoadShare, TArray<float>& VergeShare);

		/**
		 * The ridges with their bores, less Pivot, into the stub's ground mesh: Rock, Grass and Dark are its vertex kinds
		 * (M_LBA_StubGround's: all rock, grass, LBA1's dark ground). The bores are lined as the Temple of Bu's halls are, with
		 * its enclosure's finishes (WallMaterial, RoofMaterial: MI_Grid_010_Finish and _Roof; the ground's when null).
		 */
		void EmitRocks(const TArray<float>& Heights, const FVector& Pivot, const FLBAVertex& Rock, const FLBAVertex& Grass, const FLBAVertex& Dark,
			FLBAMeshBuilder& Builder, int32 Section, UMaterialInterface* WallMaterial, UMaterialInterface* RoofMaterial) const;

		/** The boulders along the roads and causeways and around the islets. */
		const TArray<FBoulder>& GetBoulders() const { return Boulders; }
		/** A boulder's faceted mesh (flat shaded, Vertex's color and UV on every vertex), less Pivot. */
		static void EmitBoulder(const FBoulder& Boulder, const FVector& Pivot, const FLBAVertex& Vertex, FLBAMeshBuilder& Builder, int32 Section);

		/** The stones on their islets, less Pivot, into the stub's ground mesh: rounded slabs of Stone (pale stone). */
		void EmitStones(const TArray<float>& Heights, const FVector& Pivot, const FLBAVertex& Stone, FLBAMeshBuilder& Builder, int32 Section) const;

		/**
		 * The bores' columns (the remake's model of the Temple of Bu's mosaic column, when remake models are on) and the parked
		 * buggy (LBA2 island decor: import the islands first), in World's folder Stubs/Sites.
		 */
		void SpawnProps(FLBAImportContext& Context, UWorld* World, const TArray<float>& Heights) const;

		const TArray<FSite>& GetSites() const { return Sites; }

	private:
		float SampleHeight(const TArray<float>& Heights, const FVector2D& Map) const;
		FVector2D CellToMap(int32 X, int32 Y) const { return Grid.Origin + FVector2D(X, Y) * Grid.Cell; }

		struct FEntry
		{
			FString Island;
			FString Name;
			int32 Scene = INDEX_NONE;
			int32 FromScene = INDEX_NONE;
			bool bRock = false;
			FVector2D Marker = FVector2D::ZeroVector;
		};
		TArray<FEntry> Entries;
		FGridInfo Grid;
		TArray<FSite> Sites;
		TArray<FBoulder> Boulders;
		TArray<bool> Channels;
	};
}
