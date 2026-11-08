#include "LBAPointsOfInterest.h"

#define LOCTEXT_NAMESPACE "LBAPointsOfInterest"

namespace
{
	const TCHAR* TwinsunLevel = TEXT("/Game/LBA2/Planets/L_TWINSUN");
	/** Citadel is the planet's home island, laid at identity. */
	const TCHAR* CitadelFrame = TEXT("/Game/LBA2/Islands/CITABAU/L_CITABAU");
	/** The stub islands (and their LBA1 towns) and the belt are laid in map coordinates around the pole. */
	const TCHAR* StubsFrame = TEXT("/Game/LBA2/Planets/L_TWINSUN_Stubs");
	const TCHAR* BeltFrame = TEXT("/Game/LBA2/Planets/L_TWINSUN_Belt");

	FLBAPointOfInterest Spot(const TCHAR* Id, const FText& Name, const FText& Description, const TCHAR* Frame, const FVector& Feet, double Yaw)
	{
		FLBAPointOfInterest Point;
		Point.Id = Id;
		Point.Name = Name;
		Point.Description = Description;
		Point.Level = TwinsunLevel;
		Point.Frame = Frame;
		Point.Feet = Feet;
		Point.Yaw = Yaw;
		return Point;
	}

	FLBAPointOfInterest Interior(const TCHAR* Id, const FText& Name, const FText& Description, int32 Scene)
	{
		FLBAPointOfInterest Point;
		Point.Id = Id;
		Point.Name = Name;
		Point.Description = Description;
		Point.Scene = Scene;
		return Point;
	}
}

const TArray<FLBAPointOfInterest>& LBAPointsOfInterest::Get()
{
	// Showcase places, picked by hand and checked in game shots (2026-11-07). Spots on the globe are in their sub-level's
	// coordinates: Citadel's from its scenes (cube * 4096 + LBA position / 8: the harbour's hero start, the hollow where
	// scene 49's Dinofly lies), the LBA1 towns' from LBA1 scene positions through the town frames (Town_<Island>_Scene_NNN
	// actors), Hamalayi's from the belt mesh (the flat bench farthest from any edge). Re-check them when the planet or the
	// LBA1 districts are rebuilt with another layout.
	static const TArray<FLBAPointOfInterest> Points = []
	{
		return TArray<FLBAPointOfInterest>{
			Spot(TEXT("CitadelBoat"), LOCTEXT("CitadelBoat", "Citadel harbour, by the ferry"),
				LOCTEXT("CitadelBoatTip", "Board the ferry (Use) and sail around the planet."), CitadelFrame, FVector(31123.0, 38591.0, 100.0), 138.0),
			Spot(TEXT("DinoflySpot"), LOCTEXT("DinoflySpot", "Where the Dinofly waits"),
				LOCTEXT("DinoflySpotTip", "Near Twinsen's house, where LBA2's Dinofly lies. Summon yours with F / Y."), CitadelFrame,
				FVector(33950.0, 42790.0, 250.0), 0.0),
			Interior(TEXT("Start"), LOCTEXT("Start", "The start of LBA2"),
				LOCTEXT("StartTip", "Twinsen's house, where the game begins."), 0),
			Interior(TEXT("TempleOfBu"), LOCTEXT("TempleOfBu", "Temple of Bu"),
				LOCTEXT("TempleOfBuTip", "Desert Island's temple: vaults, pillars and sliding doors."), 10),
			Interior(TEXT("TraluCave"), LOCTEXT("TraluCave", "Tralu's cave"),
				LOCTEXT("TraluCaveTip", "Citadel's cave behind the wizard's tent: a hollow with a rising roof."), 2),
			Spot(TEXT("ProximCity"), LOCTEXT("ProximCity", "Proxim City"),
				LOCTEXT("ProximCityTip", "Proxima Island, from near the inventor's house."), StubsFrame, FVector(-6742.0, -27875.0, 100.0), -7.7),
			Spot(TEXT("PrincipalTown"), LOCTEXT("PrincipalTown", "Principal Island town"),
				LOCTEXT("PrincipalTownTip", "LBA1's harbour town rebuilt in 3D: below the fortress, looking into Old Burg."), StubsFrame,
				FVector(-18751.0, 19234.0, 100.0), 51.8),
			Spot(TEXT("RebellionHarbour"), LOCTEXT("RebellionHarbour", "Rebellion Island harbour"),
				LOCTEXT("RebellionHarbourTip", "Where the rebels first fight, facing the Hamalayi mountains."), StubsFrame,
				FVector(28353.0, -12246.0, 100.0), -23.4),
			Spot(TEXT("HamalayiTop"), LOCTEXT("HamalayiTop", "Hamalayi plateau"),
				LOCTEXT("HamalayiTopTip", "The belt's widest flat ground, 50 m up, looking back at the islands."), BeltFrame,
				FVector(-50445.0, 1342.0, 5170.0), -1.5),
		};
	}();
	return Points;
}

const FLBAPointOfInterest* LBAPointsOfInterest::Find(FName Id)
{
	return Get().FindByPredicate([Id](const FLBAPointOfInterest& Point) { return Point.Id == Id; });
}

#undef LOCTEXT_NAMESPACE
