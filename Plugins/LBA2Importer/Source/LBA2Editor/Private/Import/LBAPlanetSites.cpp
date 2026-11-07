#include "Import/LBAPlanetSites.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Formats/LBA1.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMeshBuilder.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"

namespace LBAPlanetSitesPrivate
{
	/** The sites, by island: the stone's LBA1 scene (its holomap marker), and whether a rock with a hole is before it. */
	struct FSiteEntry
	{
		const TCHAR* Island;
		const TCHAR* Name;
		int32 Scene;
		bool bRock;
	};
	constexpr FSiteEntry SiteTable[] =
	{
		{ TEXT("Proxima"), TEXT("Marked Stone"), 46, true },
		{ TEXT("Proxima"), TEXT("Eclipse Stone"), 45, false },
	};

	// Lengths in cm.
	/** The tip: the land this far in from the coast nearest to the marker. */
	constexpr double TipInland = 300.0;
	/** From where the causeway starts to the islet's center (LBA1: 14 m to the Eclipse islet, 17 m past the cliff). */
	constexpr double CausewayMin = 1400.0;
	constexpr double CausewayMax = 2400.0;
	/** Half a causeway's width: a 3 m road and its verges. */
	constexpr double CausewayHalf = 250.0;
	/** The islet (LBA1's: about 8 m across). */
	constexpr double IsletRadius = 450.0;
	/** Causeways and islets: low, level shore. */
	constexpr double LowGround = 60.0;
	/** The Marked Stone's rock: half its length along the way out (its face at the tip), half its width, its height over the ground (LBA1: 20 bricks, 6.4 m), the width over which its sides rise. */
	constexpr double RockLong = 800.0;
	constexpr double RockWide = 700.0;
	constexpr double RockHeight = 650.0;
	constexpr double RockEdge = 120.0;
	constexpr double RockGround = 80.0;
	/** Where the hole comes out: low ground before the causeway. */
	constexpr double Landing = 350.0;
	/** The bore: its half width, where its arch springs over the floor (a half circle on top), its rim's width round the mouth. */
	constexpr double HoleHalf = 115.0;
	constexpr double Spring = 160.0;
	constexpr double RimWidth = 35.0;
	/** The rock mesh: steps round its outline, rows up its sides (fine where the mouths are), its foot sunk this far. */
	constexpr int32 RockTurns = 288;
	constexpr double RockFineRows = 30.0;
	constexpr double RockFineTop = 330.0;
	constexpr int32 RockCoarseRows = 7;
	constexpr double RockSink = 40.0;
	/** The road: half its width, its grass verges, the ground easing into it, its steepest climb. */
	constexpr double RoadHalf = 150.0;
	constexpr double Verge = 100.0;
	constexpr double Shoulder = 250.0;
	constexpr double RoadGrade = 0.12;
	/** Ground steeper than this between two cells is a cliff to the road's search: crossed only at a price per cm climbed. */
	constexpr double CliffGrade = 0.45;
	constexpr double CliffCost = 60.0;
	/** Boulders along the roads: this far apart (and up to Wander off that), their radius. */
	constexpr double BoulderEvery = 110.0;
	constexpr double BoulderWander = 25.0;
	constexpr double BoulderMin = 30.0;
	constexpr double BoulderMax = 48.0;
	/** Harbours: the stub's land within this of the ferry's way in is cleared; land costs this much more to cross than water, narrows this much. */
	constexpr double HarbourHalf = 900.0;
	constexpr double HarbourLandCost = 30.0;
	constexpr double HarbourNarrowCost = 8.0;
	/** A dock's water is found this close to it. */
	constexpr int32 DockReach = 15;
	/** The stones: LBA1's rounded slab (about Twinsen's height there), a little taller to be seen from the city. */
	constexpr double StoneWidth = 140.0;
	constexpr double StoneThick = 50.0;
	constexpr double StoneShoulder = 190.0;
	constexpr double StoneSink = 20.0;
	/** The parked buggy: LBA2 decor (island, body). */
	const TCHAR* const BuggyIsland = TEXT("CITADEL");
	constexpr int32 BuggyBody = 85;


	/** Squared distance transform along one line (Felzenszwalb and Huttenlocher): F in, D out. */
	void SiteDistanceLine(const TArray<double>& F, TArray<double>& D)
	{
		const int32 Num = F.Num();
		TArray<int32> V;
		TArray<double> Z;
		V.SetNumZeroed(Num);
		Z.SetNumZeroed(Num + 1);
		int32 K = 0;
		Z[0] = -TNumericLimits<double>::Max();
		Z[1] = TNumericLimits<double>::Max();
		auto Meet = [&](int32 Q, int32 P) { return ((F[Q] + double(Q) * Q) - (F[P] + double(P) * P)) / (2.0 * (Q - P)); };
		for (int32 Q = 1; Q < Num; ++Q)
		{
			double S = Meet(Q, V[K]);
			while (S <= Z[K])
			{
				--K;
				S = Meet(Q, V[K]);
			}
			++K;
			V[K] = Q;
			Z[K] = S;
			Z[K + 1] = TNumericLimits<double>::Max();
		}
		K = 0;
		D.SetNumUninitialized(Num);
		for (int32 Q = 0; Q < Num; ++Q)
		{
			while (Z[K + 1] < Q)
			{
				++K;
			}
			D[Q] = double(FMath::Square(Q - V[K])) + F[V[K]];
		}
	}

	/** Distance (cells) from each cell to the nearest cell where Inside is false. */
	TArray<float> SiteDistanceOutOf(const TArray<bool>& Inside, int32 SizeX, int32 SizeY)
	{
		constexpr double Far = 1e20;
		TArray<float> Out;
		Out.SetNumUninitialized(SizeX * SizeY);
		TArray<double> F, D, Rows;
		Rows.SetNumUninitialized(SizeX * SizeY);
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			F.SetNumUninitialized(SizeX);
			for (int32 X = 0; X < SizeX; ++X)
			{
				F[X] = Inside[Y * SizeX + X] ? Far : 0.0;
			}
			SiteDistanceLine(F, D);
			for (int32 X = 0; X < SizeX; ++X)
			{
				Rows[Y * SizeX + X] = D[X];
			}
		}
		for (int32 X = 0; X < SizeX; ++X)
		{
			F.SetNumUninitialized(SizeY);
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				F[Y] = Rows[Y * SizeX + X];
			}
			SiteDistanceLine(F, D);
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				Out[Y * SizeX + X] = float(FMath::Sqrt(D[Y]));
			}
		}
		return Out;
	}

	double SegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1e-6), 0.0, 1.0);
		return FVector2D::Distance(P, A + T * AB);
	}

	FVector2D Side(const FVector2D& Dir) { return FVector2D(-Dir.Y, Dir.X); }

	/** The rock's outline wanders this much round its ellipse (Angle as atan2(v / wide, u / long)); not at its ends, where the bore is. */
	double RockWobble(double Angle)
	{
		const double Ends = FMath::Square(FMath::Sin(Angle));
		return 1.0 + Ends * (0.10 * FMath::Sin(3.0 * Angle + 1.3) + 0.06 * FMath::Sin(5.0 * Angle + 0.4) + 0.04 * FMath::Sin(8.0 * Angle + 2.1));
	}

	/** How far inside the Marked Stone's rock a point is (cm, negative outside): an ellipse from the tip outward, its outline wandering a little. */
	double RockInside(const FVector2D& P, const LBAPlanetSites::FSite& Site)
	{
		const FVector2D Center = Site.Tip + Site.Dir * RockLong;
		const double U = FVector2D::DotProduct(P - Center, Site.Dir);
		const double V = FVector2D::DotProduct(P - Center, Side(Site.Dir));
		const double Angle = FMath::Atan2(V / RockWide, U / RockLong);
		const double R = FMath::Sqrt(FMath::Square(U / RockLong) + FMath::Square(V / RockWide)) / RockWobble(Angle);
		return (1.0 - R) * FMath::Min(RockLong, RockWide);
	}

	/** Smooth noise, -1 to 1, with features a few meters wide. */
	double RockNoise(const FVector2D& P)
	{
		return 0.5 * FMath::Sin(P.X / 170.0 + 1.1) * FMath::Cos(P.Y / 230.0 + 0.3) + 0.5 * FMath::Sin((P.X + P.Y) / 310.0 + 2.0);
	}

	/** Cheapest path over the grid (8 neighbours) from any of Starts to the first cell Goal accepts; Step(From, To, Run) < 0 is blocked. */
	template <typename FStep, typename FGoal>
	TArray<int32> CheapestPath(int32 SizeX, int32 SizeY, const TArray<int32>& Starts, FStep Step, FGoal IsGoal)
	{
		TArray<float> Cost;
		Cost.Init(TNumericLimits<float>::Max(), SizeX * SizeY);
		TArray<int32> Prev;
		Prev.Init(INDEX_NONE, SizeX * SizeY);
		TArray<TPair<float, int32>> Heap;
		auto Less = [](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; };
		for (const int32 Start : Starts)
		{
			Cost[Start] = 0.f;
			Heap.HeapPush({ 0.f, Start }, Less);
		}
		static const FIntPoint Steps[8] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
		int32 Found = INDEX_NONE;
		while (!Heap.IsEmpty())
		{
			TPair<float, int32> Top;
			Heap.HeapPop(Top, Less, EAllowShrinking::No);
			if (Top.Key > Cost[Top.Value])
			{
				continue;
			}
			if (IsGoal(Top.Value))
			{
				Found = Top.Value;
				break;
			}
			const int32 X = Top.Value % SizeX;
			const int32 Y = Top.Value / SizeX;
			for (const FIntPoint& D : Steps)
			{
				const int32 NX = X + D.X;
				const int32 NY = Y + D.Y;
				if (NX < 0 || NY < 0 || NX >= SizeX || NY >= SizeY)
				{
					continue;
				}
				const int32 Next = NY * SizeX + NX;
				const double Run = (D.X != 0 && D.Y != 0) ? UE_SQRT_2 : 1.0;
				const double Added = Step(Top.Value, Next, Run);
				if (Added < 0.0)
				{
					continue;
				}
				const float NewCost = float(Top.Key + Added);
				if (NewCost < Cost[Next])
				{
					Cost[Next] = NewCost;
					Prev[Next] = Top.Value;
					Heap.HeapPush({ NewCost, Next }, Less);
				}
			}
		}
		TArray<int32> Path;
		for (int32 At = Found; At != INDEX_NONE; At = Prev[At])
		{
			Path.Insert(At, 0);
		}
		return Path;
	}

	/** Chaikin's corner cutting, its ends kept. */
	TArray<FVector2D> SmoothPath(TArray<FVector2D> Points, int32 Passes)
	{
		for (int32 Pass = 0; Pass < Passes && Points.Num() > 2; ++Pass)
		{
			TArray<FVector2D> Out = { Points[0] };
			for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
			{
				Out.Add(0.75 * Points[Index] + 0.25 * Points[Index + 1]);
				Out.Add(0.25 * Points[Index] + 0.75 * Points[Index + 1]);
			}
			Out.Add(Points.Last());
			Points = MoveTemp(Out);
		}
		return Points;
	}

	/** Points every Spacing along a polyline. */
	TArray<FVector2D> Resample(const TArray<FVector2D>& Points, double Spacing)
	{
		TArray<FVector2D> Out;
		if (Points.IsEmpty())
		{
			return Out;
		}
		Out.Add(Points[0]);
		double Carry = 0.0;
		for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
		{
			const FVector2D A = Points[Index];
			const FVector2D B = Points[Index + 1];
			const double Length = FVector2D::Distance(A, B);
			double At = Spacing - Carry;
			while (At <= Length)
			{
				Out.Add(A + (B - A) * (At / FMath::Max(Length, 1e-6)));
				At += Spacing;
			}
			Carry = Length - (At - Spacing);
		}
		if (FVector2D::Distance(Out.Last(), Points.Last()) > 1.0)
		{
			Out.Add(Points.Last());
		}
		return Out;
	}

	/**
	 * Heights along a path kept within Grade of each other: smoothed over about 3 m, then clamped from each end in turn
	 * and the two met halfway (so neither end drags the whole road), then clamped once more from the start.
	 */
	TArray<double> GradeProfile(const TArray<double>& Z, double Spacing, double Grade)
	{
		const int32 Reach = FMath::Max(1, FMath::RoundToInt(300.0 / Spacing));
		TArray<double> Smooth = Z;
		for (int32 Index = 0; Index < Z.Num(); ++Index)
		{
			double Sum = 0.0, Weight = 0.0;
			for (int32 Offset = -2 * Reach; Offset <= 2 * Reach; ++Offset)
			{
				const double W = FMath::Exp(-0.5 * FMath::Square(double(Offset) / Reach));
				Sum += W * Z[FMath::Clamp(Index + Offset, 0, Z.Num() - 1)];
				Weight += W;
			}
			Smooth[Index] = Sum / Weight;
		}
		const double Step = Grade * Spacing;
		TArray<double> Forward = Smooth, Backward = Smooth;
		for (int32 Index = 1; Index < Smooth.Num(); ++Index)
		{
			Forward[Index] = FMath::Clamp(Forward[Index], Forward[Index - 1] - Step, Forward[Index - 1] + Step);
		}
		for (int32 Index = Smooth.Num() - 2; Index >= 0; --Index)
		{
			Backward[Index] = FMath::Clamp(Backward[Index], Backward[Index + 1] - Step, Backward[Index + 1] + Step);
		}
		TArray<double> Out;
		Out.SetNumUninitialized(Smooth.Num());
		for (int32 Index = 0; Index < Smooth.Num(); ++Index)
		{
			Out[Index] = 0.5 * (Forward[Index] + Backward[Index]);
			if (Index > 0)
			{
				Out[Index] = FMath::Clamp(Out[Index], Out[Index - 1] - Step, Out[Index - 1] + Step);
			}
		}
		return Out;
	}

	/** Land cells in 4-connected pieces: per cell its piece (INDEX_NONE for sea), and each piece's size. */
	TArray<int32> LandPieces(const TArray<bool>& Land, int32 SizeX, int32 SizeY, TArray<int32>& Sizes)
	{
		TArray<int32> Piece;
		Piece.Init(INDEX_NONE, SizeX * SizeY);
		Sizes.Reset();
		for (int32 Start = 0; Start < Piece.Num(); ++Start)
		{
			if (!Land[Start] || Piece[Start] != INDEX_NONE)
			{
				continue;
			}
			const int32 Id = Sizes.Add(0);
			TArray<int32> Stack = { Start };
			Piece[Start] = Id;
			while (!Stack.IsEmpty())
			{
				const int32 At = Stack.Pop(EAllowShrinking::No);
				++Sizes[Id];
				const int32 X = At % SizeX;
				const int32 Y = At / SizeX;
				const int32 Near[4] = { X > 0 ? At - 1 : INDEX_NONE, X + 1 < SizeX ? At + 1 : INDEX_NONE, Y > 0 ? At - SizeX : INDEX_NONE, Y + 1 < SizeY ? At + SizeX : INDEX_NONE };
				for (const int32 N : Near)
				{
					if (N != INDEX_NONE && Land[N] && Piece[N] == INDEX_NONE)
					{
						Piece[N] = Id;
						Stack.Push(N);
					}
				}
			}
		}
		return Piece;
	}
}

void LBAPlanetSites::FSites::Load(const LBA1::FData& Data, double Radius)
{
	using namespace LBAPlanetSitesPrivate;
	Entries.Reset();
	const TArray<FVector2D> Markers = Data.LoadMarkers();
	for (const FSiteEntry& Site : SiteTable)
	{
		if (!Markers.IsValidIndex(Site.Scene))
		{
			continue;
		}
		FLBAPlanetIsland Place;
		Place.Latitude = Markers[Site.Scene].X;
		Place.Longitude = Markers[Site.Scene].Y;
		Entries.Add({ Site.Island, Site.Name, Site.Scene, Site.bRock, Place.GetMapLocation(Radius) });
	}
}

bool LBAPlanetSites::FSites::HasIsland(const FString& Island) const
{
	return Entries.ContainsByPredicate([&Island](const FEntry& Entry) { return Entry.Island == Island; });
}

void LBAPlanetSites::FSites::Find(const FString& Island, const FGridInfo& InGrid, const TArray<bool>& Land)
{
	using namespace LBAPlanetSitesPrivate;
	Grid = InGrid;
	Sites.Reset();
	Boulders.Reset();
	const int32 Cells = Grid.SizeX * Grid.SizeY;
	if (!HasIsland(Island))
	{
		return;
	}

	// Only the island's main land: the painting has crumbs off its coasts.
	TArray<int32> Sizes;
	const TArray<int32> Pieces = LandPieces(Land, Grid.SizeX, Grid.SizeY, Sizes);
	if (Sizes.IsEmpty())
	{
		return;
	}
	int32 Main = 0;
	for (int32 Id = 1; Id < Sizes.Num(); ++Id)
	{
		Main = Sizes[Id] > Sizes[Main] ? Id : Main;
	}
	TArray<bool> MainLand;
	MainLand.SetNumUninitialized(Cells);
	FVector2D Centroid = FVector2D::ZeroVector;
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		MainLand[Index] = Pieces[Index] == Main;
		Centroid += MainLand[Index] ? CellToMap(Index % Grid.SizeX, Index / Grid.SizeX) / Sizes[Main] : FVector2D::ZeroVector;
	}
	const TArray<float> Inland = SiteDistanceOutOf(MainLand, Grid.SizeX, Grid.SizeY);

	for (const FEntry& Entry : Entries)
	{
		if (Entry.Island != Island)
		{
			continue;
		}
		double Best = TNumericLimits<double>::Max();
		FVector2D Tip = FVector2D::ZeroVector;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			if (MainLand[Index] && Inland[Index] * Grid.Cell >= TipInland)
			{
				const FVector2D Map = CellToMap(Index % Grid.SizeX, Index / Grid.SizeX);
				const double Distance = FVector2D::DistSquared(Map, Entry.Marker);
				if (Distance < Best)
				{
					Best = Distance;
					Tip = Map;
				}
			}
		}
		if (Best == TNumericLimits<double>::Max())
		{
			continue;
		}
		FSite& Site = Sites.AddDefaulted_GetRef();
		Site.Name = Entry.Name;
		Site.Scene = Entry.Scene;
		Site.bRock = Entry.bRock;
		Site.Marker = Entry.Marker;
		Site.Tip = Tip;
		const FVector2D Out = Entry.Marker - Tip;
		Site.Dir = Out.Size() > 300.0 ? Out.GetSafeNormal() : (Entry.Marker - Centroid).GetSafeNormal();
		// The causeway starts at the tip, or past the rock where the hole comes out; the islet at least CausewayMin on.
		Site.Shore = Site.bRock ? Tip + Site.Dir * (2.0 * RockLong + Landing - 100.0) : Tip;
		Site.Islet = Site.Shore + Site.Dir * FMath::Clamp(FVector2D::Distance(Entry.Marker, Site.Shore), CausewayMin, CausewayMax);
	}
}

void LBAPlanetSites::FSites::AddLand(TArray<float>& LandField) const
{
	using namespace LBAPlanetSitesPrivate;
	for (int32 Y = 0; Y < Grid.SizeY; ++Y)
	{
		for (int32 X = 0; X < Grid.SizeX; ++X)
		{
			const FVector2D P = CellToMap(X, Y);
			double Inside = -TNumericLimits<double>::Max();
			for (const FSite& Site : Sites)
			{
				Inside = FMath::Max(Inside, IsletRadius - FVector2D::Distance(P, Site.Islet));
				Inside = FMath::Max(Inside, CausewayHalf - SegmentDistance(P, Site.Shore, Site.Islet));
				if (Site.bRock)
				{
					Inside = FMath::Max(Inside, RockInside(P, Site));
					Inside = FMath::Max(Inside, Landing - FVector2D::Distance(P, Site.Shore));
					Inside = FMath::Max(Inside, CausewayHalf - SegmentDistance(P, Site.Tip, Site.Shore));
				}
			}
			// A field of 0.25 a meter in: well past the coast's line (the painting's is soft * own - 0.5).
			float& Field = LandField[Y * Grid.SizeX + X];
			Field = FMath::Max(Field, float(Inside / 400.0));
		}
	}
}

void LBAPlanetSites::FSites::ShapeHeights(TArray<float>& Heights, const TArray<bool>& Land)
{
	using namespace LBAPlanetSitesPrivate;
	// A rock's mouths: the ground in front of the near one as it is, the causeway's at the far one.
	for (FSite& Site : Sites)
	{
		if (Site.bRock)
		{
			Site.NearGround = FMath::Max(double(SampleHeight(Heights, Site.Tip - Site.Dir * 300.0)), LowGround);
			Site.FarGround = LowGround;
		}
	}
	for (int32 Y = 0; Y < Grid.SizeY; ++Y)
	{
		for (int32 X = 0; X < Grid.SizeX; ++X)
		{
			const int32 Index = Y * Grid.SizeX + X;
			if (!Land[Index])
			{
				continue;
			}
			const FVector2D P = CellToMap(X, Y);
			float& Height = Heights[Index];
			for (const FSite& Site : Sites)
			{
				bool bLow = SegmentDistance(P, Site.Shore, Site.Islet) <= CausewayHalf || FVector2D::Distance(P, Site.Islet) <= IsletRadius;
				bLow |= Site.bRock && FVector2D::Distance(P, Site.Shore) <= Landing;
				if (bLow)
				{
					Height = float(FMath::Min(double(Height), LowGround));
				}
				if (!Site.bRock)
				{
					continue;
				}
				// Under the rock (its mesh, EmitRocks) and round its foot: level across, sloping from its near mouth to its far one.
				const double Inside = RockInside(P, Site);
				if (Inside > -250.0)
				{
					const double Along = FMath::Clamp(FVector2D::DotProduct(P - Site.Tip, Site.Dir) / (2.0 * RockLong), 0.0, 1.0);
					const double Floor = FMath::Lerp(Site.NearGround, Site.FarGround, Along);
					const double Keep = FMath::SmoothStep(-250.0, -50.0, Inside);
					Height = float(FMath::Lerp(double(Height), Floor, Keep));
				}
			}
		}
	}
}

int32 LBAPlanetSites::FSites::OpenHarbours(const FString& Island, const TArray<FVector2D>& Docks, TArray<bool>& Land, const TArray<bool>& Pad) const
{
	using namespace LBAPlanetSitesPrivate;
	const int32 SizeX = Grid.SizeX;
	const int32 SizeY = Grid.SizeY;
	const int32 Cells = SizeX * SizeY;
	int32 Cleared = 0;
	for (const FVector2D& Dock : Docks)
	{
		TArray<bool> Sea;
		Sea.SetNumUninitialized(Cells);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Sea[Index] = !Land[Index];
		}
		const TArray<float> Clearance = SiteDistanceOutOf(Sea, SizeX, SizeY);
		// The dock's water: the sea cell nearest to it.
		const FIntPoint At = FIntPoint(FMath::RoundToInt((Dock.X - Grid.Origin.X) / Grid.Cell), FMath::RoundToInt((Dock.Y - Grid.Origin.Y) / Grid.Cell));
		int32 Start = INDEX_NONE;
		int32 Nearest = MAX_int32;
		for (int32 Y = At.Y - DockReach; Y <= At.Y + DockReach; ++Y)
		{
			for (int32 X = At.X - DockReach; X <= At.X + DockReach; ++X)
			{
				const int32 Distance = FMath::Square(X - At.X) + FMath::Square(Y - At.Y);
				if (X >= 0 && Y >= 0 && X < SizeX && Y < SizeY && Sea[Y * SizeX + X] && Distance < Nearest)
				{
					Nearest = Distance;
					Start = Y * SizeX + X;
				}
			}
		}
		if (Start == INDEX_NONE)
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub %s: no water by the ferry's dock at (%.0f, %.0f) m"), *Island, Dock.X / 100.0, Dock.Y / 100.0);
			continue;
		}
		// Out to the open sea (the grid's rim): water is cheap, the narrower the dearer; the stub's land dear, a pad's land closed.
		const TArray<int32> Path = CheapestPath(SizeX, SizeY, { Start },
			[&](int32 From, int32 To, double Run)
			{
				if (Land[To] && !Pad.IsEmpty() && Pad[To])
				{
					return -1.0;
				}
				const double Narrow = FMath::Max(0.0, 1.0 - Clearance[To] * Grid.Cell / HarbourHalf);
				return Run * (1.0 + (Land[To] ? HarbourLandCost : 0.0) + HarbourNarrowCost * Narrow * Narrow);
			},
			[&](int32 Index)
			{
				const int32 X = Index % SizeX;
				const int32 Y = Index / SizeX;
				return !Land[Index] && (X == 0 || Y == 0 || X == SizeX - 1 || Y == SizeY - 1);
			});
		TArray<int32> SizesBefore;
		LandPieces(Land, SizeX, SizeY, SizesBefore);
		const int32 Reach = FMath::CeilToInt(HarbourHalf / Grid.Cell);
		int32 Here = 0;
		for (const int32 Step : Path)
		{
			const int32 PX = Step % SizeX;
			const int32 PY = Step / SizeX;
			for (int32 Y = FMath::Max(PY - Reach, 0); Y <= FMath::Min(PY + Reach, SizeY - 1); ++Y)
			{
				for (int32 X = FMath::Max(PX - Reach, 0); X <= FMath::Min(PX + Reach, SizeX - 1); ++X)
				{
					const int32 Index = Y * SizeX + X;
					if (Land[Index] && (Pad.IsEmpty() || !Pad[Index]) && FMath::Square(X - PX) + FMath::Square(Y - PY) <= Reach * Reach)
					{
						Land[Index] = false;
						++Here;
					}
				}
			}
		}
		TArray<int32> SizesAfter;
		LandPieces(Land, SizeX, SizeY, SizesAfter);
		auto Big = [](const TArray<int32>& Sizes) { int32 Count = 0; for (const int32 Size : Sizes) { Count += Size >= 50 ? 1 : 0; } return Count; };
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet stub %s: the ferry's way in from (%.0f, %.0f) m, %d m long, %d m2 of land cleared%s"),
			*Island, Dock.X / 100.0, Dock.Y / 100.0, Path.Num(), Here, Big(SizesAfter) > Big(SizesBefore) ? TEXT(", which split the land") : TEXT(""));
		Cleared += Here;
	}
	return Cleared;
}

float LBAPlanetSites::FSites::SampleHeight(const TArray<float>& Heights, const FVector2D& Map) const
{
	const FVector2D C = (Map - Grid.Origin) / Grid.Cell;
	const int32 X0 = FMath::Clamp(FMath::FloorToInt(C.X), 0, Grid.SizeX - 2);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt(C.Y), 0, Grid.SizeY - 2);
	const float FX = float(FMath::Clamp(C.X - X0, 0.0, 1.0));
	const float FY = float(FMath::Clamp(C.Y - Y0, 0.0, 1.0));
	auto H = [&](int32 X, int32 Y) { return Heights[Y * Grid.SizeX + X]; };
	return FMath::Lerp(FMath::Lerp(H(X0, Y0), H(X0 + 1, Y0), FX), FMath::Lerp(H(X0, Y0 + 1), H(X0 + 1, Y0 + 1), FX), FY);
}

void LBAPlanetSites::FSites::LayRoads(TArray<float>& Heights, const TArray<bool>& Land, const TArray<bool>& Pad, TArray<float>& RoadShare, TArray<float>& VergeShare)
{
	using namespace LBAPlanetSitesPrivate;
	const int32 SizeX = Grid.SizeX;
	const int32 SizeY = Grid.SizeY;
	const int32 Cells = SizeX * SizeY;
	RoadShare.Init(0.f, Cells);
	VergeShare.Init(0.f, Cells);
	auto IsPad = [&](int32 Index) { return !Pad.IsEmpty() && Pad[Index]; };
	auto CellOf = [&](const FVector2D& Map)
	{
		const int32 X = FMath::Clamp(FMath::RoundToInt((Map.X - Grid.Origin.X) / Grid.Cell), 0, SizeX - 1);
		const int32 Y = FMath::Clamp(FMath::RoundToInt((Map.Y - Grid.Origin.Y) / Grid.Cell), 0, SizeY - 1);
		return Y * SizeX + X;
	};
	auto InAnyRock = [&](const FVector2D& P, double Margin)
	{
		for (const FSite& Site : Sites)
		{
			if (Site.bRock && RockInside(P, Site) > -Margin)
			{
				return true;
			}
		}
		return false;
	};

	// Where roads leave a town: its pad's edge.
	TArray<int32> Edge;
	TArray<bool> IsEdge;
	IsEdge.Init(false, Cells);
	for (int32 Y = 1; Y + 1 < SizeY; ++Y)
	{
		for (int32 X = 1; X + 1 < SizeX; ++X)
		{
			const int32 Index = Y * SizeX + X;
			if (IsPad(Index) && Land[Index] && (!IsPad(Index - 1) || !IsPad(Index + 1) || !IsPad(Index - SizeX) || !IsPad(Index + SizeX)))
			{
				Edge.Add(Index);
				IsEdge[Index] = true;
			}
		}
	}

	// Per cell, the nearest road sample within reach: its distance and height.
	const double Reach = RoadHalf + Verge + Shoulder;
	TArray<float> Distance;
	Distance.Init(TNumericLimits<float>::Max(), Cells);
	TArray<float> RoadZ;
	RoadZ.Init(0.f, Cells);
	TArray<bool> Stamp;
	Stamp.Init(false, Cells);
	auto Splat = [&](const FVector2D& P, double Z, bool bStamp)
	{
		const FVector2D C = (P - Grid.Origin) / Grid.Cell;
		const int32 R = FMath::CeilToInt(Reach / Grid.Cell);
		for (int32 Y = FMath::Max(FMath::FloorToInt(C.Y) - R, 0); Y <= FMath::Min(FMath::CeilToInt(C.Y) + R, SizeY - 1); ++Y)
		{
			for (int32 X = FMath::Max(FMath::FloorToInt(C.X) - R, 0); X <= FMath::Min(FMath::CeilToInt(C.X) + R, SizeX - 1); ++X)
			{
				const int32 Index = Y * SizeX + X;
				const float D = float(FVector2D::Distance(CellToMap(X, Y), P));
				if (D < Distance[Index])
				{
					Distance[Index] = D;
					RoadZ[Index] = float(Z);
					Stamp[Index] = bStamp;
				}
			}
		}
	};
	TArray<FVector> BoulderSpots;
	auto Kerbs = [&](const TArray<FVector2D>& Line, double SkipStart, int32 Seed)
	{
		FRandomStream Random(Seed);
		double Along = 0.0;
		double Next = SkipStart;
		for (int32 Index = 1; Index < Line.Num(); ++Index)
		{
			const FVector2D A = Line[Index - 1];
			const FVector2D B = Line[Index];
			const double Length = FVector2D::Distance(A, B);
			const FVector2D Across = Side((B - A).GetSafeNormal());
			while (Next <= Along + Length)
			{
				const FVector2D At = A + (B - A) * ((Next - Along) / FMath::Max(Length, 1e-6));
				for (const double Sign : { -1.0, 1.0 })
				{
					const double Offset = RoadHalf + 0.55 * Verge + Random.FRandRange(-10.0, 10.0);
					BoulderSpots.Add(FVector(At + Across * Sign * Offset + (B - A).GetSafeNormal() * Random.FRandRange(-BoulderWander, BoulderWander), Random.FRandRange(BoulderMin, BoulderMax)));
				}
				Next += BoulderEvery;
			}
			Along += Length;
		}
	};

	int32 SiteSeed = 1;
	for (FSite& Site : Sites)
	{
		const FVector2D End = Site.bRock ? Site.Tip - Site.Dir * 200.0 : Site.Tip;
		const int32 Goal = CellOf(End);
		// Nothing past a rock's face: the road stops in front of it.
		auto Blocked = [&](int32 Index)
		{
			if (!Land[Index] || (IsPad(Index) && !IsEdge[Index]))
			{
				return true;
			}
			const FVector2D P = CellToMap(Index % SizeX, Index / SizeX);
			for (const FSite& Other : Sites)
			{
				if (Other.bRock && FVector2D::DotProduct(P - Other.Tip, Other.Dir) > -100.0 && FMath::Abs(FVector2D::DotProduct(P - Other.Tip, Side(Other.Dir))) < RockWide + 300.0
					&& FVector2D::Distance(P, Other.Tip) < 2.0 * RockLong + 2500.0)
				{
					return true;
				}
			}
			return false;
		};
		TArray<int32> Starts = Edge;
		Starts.Sort([&](int32 A, int32 B) { return FVector2D::DistSquared(CellToMap(A % SizeX, A / SizeX), End) < FVector2D::DistSquared(CellToMap(B % SizeX, B / SizeX), End); });
		Starts.SetNum(FMath::Min(Starts.Num(), 80));
		if (Starts.IsEmpty())
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub: no town for the road to the %s"), *Site.Name);
			continue;
		}
		const TArray<int32> Cellpath = CheapestPath(SizeX, SizeY, Starts,
			[&](int32 From, int32 To, double Run)
			{
				if (Blocked(To) && To != Goal)
				{
					return -1.0;
				}
				const double Climb = FMath::Abs(Heights[To] - Heights[From]);
				const double Grade = Climb / (Run * Grid.Cell);
				const double Kept = FMath::Min(Grade, CliffGrade);
				return Run * (1.0 + 40.0 * Kept * Kept + 4.0 * Grade) + (Grade > CliffGrade ? CliffCost * Climb / Grid.Cell : 0.0);
			},
			[&](int32 Index) { return Index == Goal; });
		if (Cellpath.IsEmpty())
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub: no way found for the road to the %s"), *Site.Name);
			continue;
		}
		TArray<FVector2D> Points;
		for (const int32 Index : Cellpath)
		{
			Points.Add(CellToMap(Index % SizeX, Index / SizeX));
		}
		Points = SmoothPath(Points, 4);
		if (!Site.bRock)
		{
			// On along the causeway to the islet.
			Points.Add(Site.Islet);
		}
		constexpr double Spacing = 50.0;
		const TArray<FVector2D> Line = Resample(Points, Spacing);
		TArray<double> Z;
		for (const FVector2D& P : Line)
		{
			Z.Add(SampleHeight(Heights, P));
		}
		const TArray<double> Profile = GradeProfile(Z, Spacing, RoadGrade);
		Site.Road.Reset();
		Site.RoadLength = 0.0;
		Site.Steepest = 0.0;
		for (int32 Index = 0; Index < Line.Num(); ++Index)
		{
			Site.Road.Add(FVector(Line[Index], Profile[Index]));
			if (Index > 0)
			{
				const double Run = FVector2D::Distance(Line[Index], Line[Index - 1]);
				Site.RoadLength += Run;
				Site.Steepest = FMath::Max(Site.Steepest, FMath::Abs(Profile[Index] - Profile[Index - 1]) / FMath::Max(Run, Spacing));
			}
			Splat(Line[Index], Profile[Index], true);
		}
		Kerbs(Line, 400.0, 100 * SiteSeed);
		// Past the rock: the causeway's path, level.
		if (Site.bRock)
		{
			const TArray<FVector2D> Walk = Resample({ Site.Tip - Site.Dir * 100.0, Site.Shore, Site.Islet }, Spacing);
			for (const FVector2D& P : Walk)
			{
				Splat(P, LowGround, false);
			}
			Kerbs(Resample({ Site.Shore, Site.Islet }, Spacing), 150.0, 100 * SiteSeed + 1);
		}
		// Round the islet, open toward the causeway.
		FRandomStream Random(100 * SiteSeed + 2);
		const double Ring = IsletRadius - 60.0;
		const int32 Count = FMath::FloorToInt(2.0 * PI * Ring / BoulderEvery);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Angle = 2.0 * PI * Index / Count;
			const FVector2D Out(FMath::Cos(Angle), FMath::Sin(Angle));
			if (FVector2D::DotProduct(Out, -Site.Dir) > FMath::Cos(FMath::DegreesToRadians(35.0)))
			{
				continue;
			}
			BoulderSpots.Add(FVector(Site.Islet + Out * (Ring + Random.FRandRange(-10.0, 10.0)), Random.FRandRange(BoulderMin, BoulderMax)));
		}
		double Lowest = TNumericLimits<double>::Max(), Highest = -TNumericLimits<double>::Max();
		for (const double Height : Profile)
		{
			Lowest = FMath::Min(Lowest, Height);
			Highest = FMath::Max(Highest, Height);
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet stub: road to the %s, %.0f m, steepest %.0f %%, from %.1f to %.1f m high; tip (%.1f, %.1f) m, islet (%.1f, %.1f) m on the map, %.0f m from its marker"),
			*Site.Name, Site.RoadLength / 100.0, 100.0 * Site.Steepest, Lowest / 100.0, Highest / 100.0, Site.Tip.X / 100.0, Site.Tip.Y / 100.0,
			Site.Islet.X / 100.0, Site.Islet.Y / 100.0, FVector2D::Distance(Site.Islet, Site.Marker) / 100.0);
		++SiteSeed;
	}

	// Laid in: level across, eased into the ground; earth on the road, grass on its verges and round the islets.
	for (int32 Y = 0; Y < SizeY; ++Y)
	{
		for (int32 X = 0; X < SizeX; ++X)
		{
			const int32 Index = Y * SizeX + X;
			const FVector2D P = CellToMap(X, Y);
			if (!Land[Index] || IsPad(Index))
			{
				continue;
			}
			for (const FSite& Site : Sites)
			{
				const double FromIslet = FVector2D::Distance(P, Site.Islet);
				if (FromIslet <= IsletRadius)
				{
					(FromIslet < IsletRadius - 120.0 ? RoadShare : VergeShare)[Index] = 1.f;
				}
			}
			const float D = Distance[Index];
			if (D > Reach)
			{
				continue;
			}
			if (Stamp[Index] && !InAnyRock(P, 50.0))
			{
				double W = FMath::Clamp(1.0 - (D - RoadHalf - Verge) / Shoulder, 0.0, 1.0);
				W = W * W * (3.0 - 2.0 * W);
				Heights[Index] = float(FMath::Lerp(double(Heights[Index]), double(RoadZ[Index]), W));
			}
			if (D <= RoadHalf)
			{
				RoadShare[Index] = 1.f;
				VergeShare[Index] = 0.f;
			}
			else if (D <= RoadHalf + Verge && RoadShare[Index] == 0.f)
			{
				VergeShare[Index] = 1.f;
			}
		}
	}

	// The boulders, on the ground as laid, half sunk; none in the sea, on a pad or in a rock.
	FRandomStream Random(7);
	for (const FVector& Spot : BoulderSpots)
	{
		const FVector2D P(Spot.X, Spot.Y);
		const int32 Index = CellOf(P);
		if (!Land[Index] || IsPad(Index) || InAnyRock(P, 30.0))
		{
			continue;
		}
		FBoulder& Boulder = Boulders.AddDefaulted_GetRef();
		Boulder.Radius = Spot.Z;
		Boulder.Center = FVector(P, SampleHeight(Heights, P) - 0.3 * Spot.Z);
		Boulder.Seed = Random.RandHelper(1 << 20);
	}
	float Top = -TNumericLimits<float>::Max(), Bottom = TNumericLimits<float>::Max();
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		if (Land[Index])
		{
			Top = FMath::Max(Top, Heights[Index]);
			Bottom = FMath::Min(Bottom, Heights[Index]);
		}
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2 planet stub: %d boulders along the roads; the land from %.1f to %.1f m high"), Boulders.Num(), Bottom / 100.0, Top / 100.0);
}

void LBAPlanetSites::FSites::EmitBoulder(const FBoulder& Boulder, const FVector& Pivot, const FLBAVertex& Vertex, FLBAMeshBuilder& Builder, int32 Section)
{
	// An icosahedron, squashed and dented: LBA1's round boulders.
	const double T = (1.0 + FMath::Sqrt(5.0)) / 2.0;
	FVector Points[12] = { { -1, T, 0 }, { 1, T, 0 }, { -1, -T, 0 }, { 1, -T, 0 }, { 0, -1, T }, { 0, 1, T }, { 0, -1, -T }, { 0, 1, -T }, { T, 0, -1 }, { T, 0, 1 }, { -T, 0, -1 }, { -T, 0, 1 } };
	static const int32 Faces[20][3] = { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
		{ 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
	FRandomStream Random(Boulder.Seed);
	const double Turn = Random.FRandRange(0.0, 2.0 * PI);
	for (FVector& Point : Points)
	{
		Point = Point.GetSafeNormal() * Boulder.Radius * Random.FRandRange(0.85, 1.1);
		// Points are (x, z up, y) above: swap so the icosahedron's axis isn't straight up, squash, turn.
		Point = FVector(Point.X, Point.Z, Point.Y * 0.8);
		Point = FVector(Point.X * FMath::Cos(Turn) - Point.Y * FMath::Sin(Turn), Point.X * FMath::Sin(Turn) + Point.Y * FMath::Cos(Turn), Point.Z);
	}
	const FVector Center = Boulder.Center - Pivot;
	for (const auto& Face : Faces)
	{
		FLBAVertex V[3] = { Vertex, Vertex, Vertex };
		const FVector Normal = ((Points[Face[0]] + Points[Face[1]] + Points[Face[2]]) / 3.0).GetSafeNormal();
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			V[Corner].Position = FVector3f(Center + Points[Face[Corner]]);
			V[Corner].Normal = FVector3f(Normal);
		}
		Builder.AddTriangleFacing(Section, V[0], V[1], V[2], FVector3f(Normal));
	}
}

void LBAPlanetSites::FSites::EmitRocks(const FVector& Pivot, const FLBAVertex& Rock, const FLBAVertex& Grass, const FLBAVertex& Dark, FLBAMeshBuilder& Builder, int32 Section) const
{
	using namespace LBAPlanetSitesPrivate;
	// Flat shaded: each triangle its own normal, facing Outward.
	auto Triangle = [&](const FLBAVertex& Kind, const FVector& A, const FVector& B, const FVector& C, const FVector& Outward)
	{
		FVector Normal = FVector::CrossProduct(C - A, B - A).GetSafeNormal();
		if (FVector::DotProduct(Normal, Outward) < 0.0)
		{
			Normal = -Normal;
		}
		FLBAVertex V[3] = { Kind, Kind, Kind };
		const FVector Corners[3] = { A, B, C };
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			V[Corner].Position = FVector3f(Corners[Corner] - Pivot);
			V[Corner].Normal = FVector3f(Normal);
		}
		Builder.AddTriangleFacing(Section, V[0], V[1], V[2], FVector3f(Normal));
	};
	// A, B, C, D round the quad.
	auto Quad = [&](const FLBAVertex& Kind, const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Outward)
	{
		Triangle(Kind, A, B, C, Outward);
		Triangle(Kind, A, C, D, Outward);
	};
	for (const FSite& Site : Sites)
	{
		if (!Site.bRock)
		{
			continue;
		}
		const FVector2D Across = Side(Site.Dir);
		const FVector2D Center = Site.Tip + Site.Dir * RockLong;
		// The bore's floor (the ground) at U along the way out, from the rock's middle.
		auto FloorAt = [&](double U)
		{
			return FMath::Lerp(Site.NearGround, Site.FarGround, FMath::Clamp((U + RockLong) / (2.0 * RockLong), 0.0, 1.0));
		};
		auto ToMap = [&](double U, double V, double Z) { return FVector(Center + Site.Dir * U + Across * V, Z); };
		// Its outline at turn T (0 at the far mouth, PI at the near one), drawn in by Lean, with vertical flutes off the mouths.
		auto Outline = [&](double T, double Lean)
		{
			const double Flutes = 1.0 + FMath::Square(FMath::Sin(T)) * (0.05 * FMath::Sin(19.0 * T) + 0.03 * FMath::Sin(33.0 * T + 1.0) + 0.015 * FMath::Sin(71.0 * T + 2.0));
			const double Scale = RockWobble(T) * (1.0 - Lean) * Flutes;
			return FVector2D(RockLong * FMath::Cos(T), RockWide * FMath::Sin(T)) * Scale;
		};
		auto TopAt = [&](double T)
		{
			return FloorAt(RockLong * FMath::Cos(T)) + RockHeight + 70.0 * FMath::Sin(2.0 * T + 0.7) + 40.0 * FMath::Sin(5.0 * T + 2.0) + 20.0 * FMath::Sin(11.0 * T);
		};
		// The rows up its sides: fine through the mouths' height, coarse above.
		const int32 FineRows = FMath::CeilToInt((RockFineTop + RockSink) / RockFineRows);
		const int32 Rows = FineRows + RockCoarseRows;
		auto RowHeight = [&](double T, int32 Row)
		{
			const double Base = FloorAt(RockLong * FMath::Cos(T)) - RockSink;
			if (Row <= FineRows)
			{
				return Base + Row * RockFineRows;
			}
			const double From = Base + FineRows * RockFineRows;
			return From + (TopAt(T) - From) * (Row - FineRows) / double(RockCoarseRows);
		};
		// In the bore's mouth (V across, H over the floor): within its half width, under its arch.
		auto InMouth = [&](double V, double H)
		{
			return FMath::Abs(V) < HoleHalf && H < Spring + FMath::Sqrt(FMath::Max(0.0, HoleHalf * HoleHalf - V * V));
		};
		auto SidePoint = [&](int32 Turn, int32 Row)
		{
			const double T = 2.0 * PI * Turn / RockTurns;
			const double Z = RowHeight(T, Row);
			// Drawn in toward the top, spread at the foot (its scree), off the mouths.
			const double Up = FMath::Clamp((Z - FloorAt(RockLong * FMath::Cos(T))) / RockHeight, 0.0, 1.0);
			const double Lean = 0.09 * Up - FMath::Square(FMath::Sin(T)) * 0.10 * FMath::Pow(1.0 - Up, 4.0);
			const FVector2D Local = Outline(T, Lean);
			return ToMap(Local.X, Local.Y, Z);
		};
		// The sides, the mouths left open.
		for (int32 Turn = 0; Turn < RockTurns; ++Turn)
		{
			for (int32 Row = 0; Row < Rows; ++Row)
			{
				const FVector A = SidePoint(Turn, Row), B = SidePoint(Turn + 1, Row), C = SidePoint(Turn + 1, Row + 1), D = SidePoint(Turn, Row + 1);
				const FVector Mid = (A + B + C + D) / 4.0;
				const double U = FVector2D::DotProduct(FVector2D(Mid) - Center, Site.Dir);
				const double V = FVector2D::DotProduct(FVector2D(Mid) - Center, Across);
				if (InMouth(V, Mid.Z - FloorAt(U)))
				{
					continue;
				}
				Quad(Rock, A, B, C, D, FVector(FVector2D(Mid) - Center, 0.0));
			}
		}
		// The top: rings in from its edge, rounded over it, grassy inside.
		const double Rings[] = { 1.0, 0.93, 0.7, 0.45, 0.2 };
		const double Lifts[] = { 0.0, 18.0, 30.0, 34.0, 36.0 };
		constexpr int32 NumRings = UE_ARRAY_COUNT(Rings);
		auto TopPoint = [&](int32 Turn, int32 Ring)
		{
			const double T = 2.0 * PI * Turn / RockTurns;
			const FVector2D Local = Outline(T, 0.09) * Rings[Ring];
			const double Roll = Ring > 0 ? 20.0 * FMath::Sin(Local.X / 230.0 + 1.0) * FMath::Cos(Local.Y / 170.0) : 0.0;
			return ToMap(Local.X, Local.Y, TopAt(T) + Lifts[Ring] + Roll);
		};
		const FVector Middle = ToMap(0.0, 0.0, FloorAt(0.0) + RockHeight + Lifts[NumRings - 1]);
		for (int32 Turn = 0; Turn < RockTurns; ++Turn)
		{
			for (int32 Ring = 0; Ring + 1 < NumRings; ++Ring)
			{
				Quad(Ring >= 2 ? Grass : Rock, TopPoint(Turn, Ring), TopPoint(Turn + 1, Ring), TopPoint(Turn + 1, Ring + 1), TopPoint(Turn, Ring + 1), FVector::UpVector);
			}
			Triangle(Grass, TopPoint(Turn, NumRings - 1), TopPoint(Turn + 1, NumRings - 1), Middle, FVector::UpVector);
		}
		// The bore: walls and an arched roof, facing in, dark; its floor is the ground. It reaches the rims' faces.
		TArray<FVector2D> Profile;
		Profile.Add(FVector2D(-HoleHalf, -20.0));
		Profile.Add(FVector2D(-HoleHalf, Spring));
		constexpr int32 ArchSteps = 14;
		for (int32 Step = 1; Step < ArchSteps; ++Step)
		{
			const double Angle = PI * (1.0 - double(Step) / ArchSteps);
			Profile.Add(FVector2D(HoleHalf * FMath::Cos(Angle), Spring + HoleHalf * FMath::Sin(Angle)));
		}
		Profile.Add(FVector2D(HoleHalf, Spring));
		Profile.Add(FVector2D(HoleHalf, -20.0));
		constexpr double RimProud = 12.0;
		constexpr double RimDeep = 40.0;
		const double Near = -RockLong - RimProud;
		const double Far = RockLong + RimProud;
		constexpr int32 Lengths = 8;
		for (int32 Piece = 0; Piece < Lengths; ++Piece)
		{
			const double U0 = FMath::Lerp(Near, Far, double(Piece) / Lengths);
			const double U1 = FMath::Lerp(Near, Far, double(Piece + 1) / Lengths);
			for (int32 Index = 0; Index + 1 < Profile.Num(); ++Index)
			{
				const FVector2D P0 = Profile[Index], P1 = Profile[Index + 1];
				const FVector2D Mid = 0.5 * (P0 + P1);
				const FVector Inward = FVector(-Across * Mid.X, Spring - Mid.Y).GetSafeNormal();
				Quad(Dark, ToMap(U0, P0.X, FloorAt(U0) + P0.Y), ToMap(U1, P0.X, FloorAt(U1) + P0.Y), ToMap(U1, P1.X, FloorAt(U1) + P1.Y), ToMap(U0, P1.X, FloorAt(U0) + P1.Y), Inward);
			}
		}
		// A rim round each mouth: its face proud of the rock, its edge reaching back into it.
		auto Grown = [](const FVector2D& P)
		{
			// Out from the bore's middle; the floor's corners straight out sideways.
			const FVector2D From(0.0, FMath::Min(P.Y, Spring));
			return P + (P - From).GetSafeNormal() * RimWidth;
		};
		for (const double End : { -RockLong, RockLong })
		{
			const double Out = End > 0.0 ? 1.0 : -1.0;
			const double Front = End + Out * RimProud;
			const double Back = End - Out * RimDeep;
			const double Ground = FloorAt(End);
			const FVector Facing(Site.Dir * Out, 0.0);
			for (int32 Index = 0; Index + 1 < Profile.Num(); ++Index)
			{
				const FVector2D P0 = Profile[Index], P1 = Profile[Index + 1];
				const FVector2D G0 = Grown(P0), G1 = Grown(P1);
				Quad(Dark, ToMap(Front, P0.X, Ground + P0.Y), ToMap(Front, P1.X, Ground + P1.Y), ToMap(Front, G1.X, Ground + G1.Y), ToMap(Front, G0.X, Ground + G0.Y), Facing);
				const FVector2D Mid = 0.5 * (G0 + G1);
				const FVector Outward = FVector(Across * Mid.X, Mid.Y - Spring).GetSafeNormal();
				Quad(Dark, ToMap(Front, G0.X, Ground + G0.Y), ToMap(Front, G1.X, Ground + G1.Y), ToMap(Back, G1.X, Ground + G1.Y), ToMap(Back, G0.X, Ground + G0.Y), Outward);
			}
		}
	}
}

void LBAPlanetSites::FSites::EmitStones(const TArray<float>& Heights, const FVector& Pivot, const FLBAVertex& Stone, FLBAMeshBuilder& Builder, int32 Section) const
{
	using namespace LBAPlanetSitesPrivate;
	// Its outline seen from the front (across, up): straight sides, a half round top.
	TArray<FVector2D> Outline;
	const double Half = 0.5 * StoneWidth;
	Outline.Add(FVector2D(Half, -StoneSink));
	constexpr int32 Steps = 12;
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const double Angle = PI * double(Step) / Steps;
		Outline.Add(FVector2D(Half * FMath::Cos(Angle), StoneShoulder + Half * FMath::Sin(Angle)));
	}
	Outline.Add(FVector2D(-Half, -StoneSink));
	for (const FSite& Site : Sites)
	{
		const FVector2D Face = -Site.Dir;
		const FVector2D Across = Side(Face);
		const FVector Base(Site.Islet, SampleHeight(Heights, Site.Islet));
		// The back a little thinner toward the top.
		auto At = [&](const FVector2D& P, double Out)
		{
			const double Thick = 0.5 * StoneThick * (1.0 - 0.25 * FMath::Clamp(P.Y / (StoneShoulder + Half), 0.0, 1.0));
			return Base + FVector(Across * P.X + Face * Thick * Out, P.Y) - Pivot;
		};
		auto Add = [&](const FVector& A, const FVector& B, const FVector& C, const FVector& Outward)
		{
			FLBAVertex V[3] = { Stone, Stone, Stone };
			const FVector Normal = FVector::CrossProduct(C - A, B - A).GetSafeNormal() * (FVector::DotProduct(FVector::CrossProduct(C - A, B - A), Outward) < 0.0 ? -1.0 : 1.0);
			const FVector Corners[3] = { A, B, C };
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				V[Corner].Position = FVector3f(Corners[Corner]);
				V[Corner].Normal = FVector3f(Normal);
			}
			Builder.AddTriangleFacing(Section, V[0], V[1], V[2], FVector3f(Normal));
		};
		const FVector2D Middle(0.0, 0.5 * StoneShoulder);
		for (int32 Index = 0; Index < Outline.Num(); ++Index)
		{
			const FVector2D P0 = Outline[Index], P1 = Outline[(Index + 1) % Outline.Num()];
			// Front and back: fans from the middle.
			for (const double Out : { 1.0, -1.0 })
			{
				Add(At(Middle, Out), At(P0, Out), At(P1, Out), FVector(Face * Out, 0.0));
			}
			// Its edge, all round.
			const FVector2D Mid = 0.5 * (P0 + P1);
			const FVector Edge = FVector(Across * Mid.X, Mid.Y - StoneShoulder * 0.5).GetSafeNormal();
			Add(At(P0, 1.0), At(P1, 1.0), At(P1, -1.0), Edge);
			Add(At(P0, 1.0), At(P1, -1.0), At(P0, -1.0), Edge);
		}
	}
}

void LBAPlanetSites::FSites::SpawnProps(FLBAImportContext& Context, UWorld* World, const TArray<float>& Heights) const
{
	using namespace LBAPlanetSitesPrivate;
	auto Find = [&](const TCHAR* Island, int32 Body)
	{
		UStaticMesh* Mesh = Context.FindAsset<UStaticMesh>(Context.GetContentPath(FString(TEXT("Islands")) / Island / TEXT("Decor")), FString::Printf(TEXT("SM_%s_Decor_%03d"), Island, Body));
		if (!Mesh)
		{
			Context.Warn(FString::Printf(TEXT("LBA1 sites: no %s decor %d (import the island first)"), Island, Body));
		}
		return Mesh;
	};
	// Stands a mesh on the ground at Map, its footprint centered there, Facing (a unit vector on the map) along its thin side, or its long one when bAlong.
	auto Place = [&](UStaticMesh* Mesh, const FVector2D& Map, const FVector2D& Facing, bool bAlong, double Scale, const FString& Label)
	{
		if (!Mesh)
		{
			return;
		}
		const FBox Box = Mesh->GetBoundingBox();
		const FVector Size = Box.GetSize();
		const bool bThinX = Size.X < Size.Y;
		// The mesh's axis to turn onto Facing.
		const double Axis = (bThinX != bAlong) ? 0.0 : 90.0;
		const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) - Axis;
		const FQuat Rotation(FVector::UpVector, FMath::DegreesToRadians(Yaw));
		const FVector Center = Box.GetCenter();
		const FVector Location = FVector(Map, SampleHeight(Heights, Map)) - Rotation.RotateVector(FVector(Center.X, Center.Y, Box.Min.Z) * Scale);
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Rotation, Location, FVector(Scale)), Label, TEXT("Stubs/Sites")))
		{
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->Tags.Add(ULBAPlanetAsset::StubTag);
		}
	};
	UStaticMesh* Buggy = nullptr;
	for (const FSite& Site : Sites)
	{
		if (Site.bRock && Site.Road.Num() > 8)
		{
			// The buggy, parked along the road a little before it ends at the rock, the hole left in view.
			Buggy = Buggy ? Buggy : Find(BuggyIsland, BuggyBody);
			const FVector2D A(Site.Road[Site.Road.Num() - 8]);
			const FVector2D B(Site.Road.Last());
			const FVector2D Along = (B - A).GetSafeNormal();
			Place(Buggy, B - Along * 500.0 + Side(Along) * 70.0, Along, true, 1.0, FString::Printf(TEXT("Site_%s_Buggy"), *FLBAImportContext::SanitizeName(Site.Name)));
		}
	}
}
