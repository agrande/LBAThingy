#include "Import/LBAPlanetSites.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Formats/LBA1.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBAPlanetDistricts.h"
#include "Import/LBARemakeAssets.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBACar.h"
#include "LBAPlanet.h"

namespace LBAPlanetSitesPrivate
{
	/**
	 * The sites, by island: the stone's LBA1 scene (its holomap marker), the scene its town leads to on the way (where the
	 * road leaves the town), and whether a rock with a hole is before it.
	 */
	struct FSiteEntry
	{
		const TCHAR* Island;
		const TCHAR* Name;
		int32 Scene;
		int32 FromScene;
		bool bRock;
	};
	constexpr FSiteEntry SiteTable[] =
	{
		{ TEXT("Proxima"), TEXT("Marked Stone"), 46, 47, true },
		{ TEXT("Proxima"), TEXT("Eclipse Stone"), 45, 45, false },
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
	/** The Marked Stone's ridge: half its thickness at its foot along the way out (its near face at the tip), its height over the ground (LBA1: 20 bricks, 6.4 m), its faces' width (steep), how far its ends reach past the arm's coasts, its half length's limits. */
	constexpr double RidgeHalf = 450.0;
	constexpr double RidgeHeight = 650.0;
	constexpr double FaceMin = 100.0;
	constexpr double FaceMax = 230.0;
	constexpr double RidgeEnds = 400.0;
	constexpr double RidgeMinHalf = 500.0;
	constexpr double RidgeMaxHalf = 1500.0;
	/** Its mesh: cells (cm), its foot sunk this far into the ground. */
	constexpr double RockCell = 15.0;
	/** The faces' width round the mouths (cm): about 65 degrees there. */
	constexpr double MouthFace = 300.0;
	constexpr double RockSink = 15.0;
	/** Where the hole comes out: low ground before the causeway. */
	constexpr double Landing = 350.0;
	/**
	 * The bore, as the Temple of Bu's halls (LBAHollow's vault: the roof rises with the distance from what holds it): its half
	 * width, its vault's spring at the walls and rise to the middle; pairs of the Temple's mosaic columns along its walls
	 * (inset from them, their radius, height, how far apart, the first from the near mouth), the roof coming down on each
	 * in a fan of this slope.
	 */
	constexpr double BoreHalf = 175.0;
	constexpr double BoreSpring = 230.0;
	constexpr double BoreRise = 100.0;
	constexpr double PillarInset = 40.0;
	constexpr double PillarRadius = 25.0;
	constexpr double PillarTop = 170.0;
	constexpr double PillarEvery = 240.0;
	constexpr double PillarFirst = 150.0;
	constexpr double FanSlope = 0.9;
	/** The remake's model of the Temple's column (library 3, layout 45), under www/models/lba2. */
	const TCHAR* const PillarFolder = TEXT("layouts");
	const TCHAR* const PillarFile = TEXT("CA_045_mosaic_column.glb");
	/** The mouths are cut this much wider than the bore; a flare of rock reaches out from the bore's end this far and back into the ridge this deep, closing the gap. */
	constexpr double MouthWiden = 15.0;
	constexpr double FlareOut = 110.0;
	constexpr double FlareBack = 120.0;
	/** The road: half its width, its grass verges, the ground easing into it, its steepest climb. */
	constexpr double RoadHalf = 150.0;
	constexpr double Verge = 100.0;
	constexpr double Shoulder = 250.0;
	constexpr double RoadGrade = 0.12;
	/** Ground steeper than this between two cells is a cliff to the road's search: crossed only at a price per cm climbed. */
	constexpr double CliffGrade = 0.45;
	constexpr double CliffCost = 60.0;
	/** Across water the road is a causeway: this many times dearer a meter than level ground, plus a meter for each meter deep. */
	constexpr double CausewayCost = 10.0;
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
	/** The stones: the plugin's model (Resources/Models/sites; carved side +Y once imported, foot at its origin), this tall (about Twinsen's height, as in LBA1), its foot this far into the ground. */
	const TCHAR* const StoneFolder = TEXT("sites");
	const TCHAR* const StoneFile = TEXT("runestone.glb");
	constexpr double StoneHeight = 190.0;
	constexpr double StoneSink = 8.0;


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

	/** Smooth noise, about -1 to 1, with features about Scale wide. */
	double SiteNoise(const FVector2D& P, double Scale, double Seed)
	{
		const FVector2D Q = P / Scale;
		return 0.5 * FMath::Sin(Q.X * 1.0 + 1.1 + Seed) * FMath::Cos(Q.Y * 1.3 + 0.3 + 2.0 * Seed)
			+ 0.3 * FMath::Sin((0.7 * Q.X - 0.6 * Q.Y) * 1.7 + 2.0 + 3.0 * Seed)
			+ 0.2 * FMath::Cos((0.4 * Q.X + 0.9 * Q.Y) * 2.9 + 0.5 + 5.0 * Seed);
	}

	/** How far inside the Marked Stone's ridge a point is (cm, negative outside): round a segment across the way out, its outline wandering. */
	double RidgeField(const FVector2D& P, const LBAPlanetSites::FSite& Site)
	{
		const FVector2D Across = Side(Site.Dir);
		const FVector2D From = Site.RidgeCenter - Across * FMath::Max(Site.RidgeLeft - RidgeHalf, 0.0);
		const FVector2D To = Site.RidgeCenter + Across * FMath::Max(Site.RidgeRight - RidgeHalf, 0.0);
		return RidgeHalf - SegmentDistance(P, From, To) + 80.0 * SiteNoise(P, 260.0, 0.0) + 25.0 * SiteNoise(P, 70.0, 1.0);
	}

	/** The ridge's height over the ground it stands on (cm): steep faces of uneven width with ledges, a rolling top, lower toward its ends. */
	double RidgeRise(const FVector2D& P, const LBAPlanetSites::FSite& Site)
	{
		const double Field = RidgeField(P, Site);
		if (Field <= 0.0)
		{
			return 0.0;
		}
		// Round the mouths the faces lean back more, so the bore cuts them cleanly (a cell of a sheer face spans meters up).
		const double FromWay = FMath::Abs(FVector2D::DotProduct(P - Site.Tip, Side(Site.Dir)));
		const double Face = FMath::Max(FMath::Lerp(FaceMin, FaceMax, 0.5 + 0.5 * SiteNoise(P, 180.0, 2.0)), MouthFace * (1.0 - FMath::SmoothStep(BoreHalf + 100.0, BoreHalf + 400.0, FromWay)));
		double S = FMath::Clamp(Field / Face, 0.0, 1.0);
		S = FMath::Clamp(S + 0.05 * FMath::Sin(2.0 * PI * 3.0 * S), 0.0, 1.0);
		const double Up = S * S * (3.0 - 2.0 * S);
		const double Across = FVector2D::DotProduct(P - Site.RidgeCenter, Side(Site.Dir));
		const double End = Across < 0.0 ? Site.RidgeLeft : Site.RidgeRight;
		const double Ends = FMath::SmoothStep(End - 700.0, End, FMath::Abs(Across));
		const double Top = RidgeHeight * (1.0 - 0.45 * Ends) + 60.0 * SiteNoise(P, 300.0, 3.0) + 20.0 * SiteNoise(P, 90.0, 4.0);
		return Up * Top;
	}

	/** The way through the ridge: the ground's height (cm) at Along (from the tip, along the way out), from its near mouth to its far one. */
	double RidgeFloor(double Along, const LBAPlanetSites::FSite& Site)
	{
		return FMath::Lerp(Site.NearGround, Site.FarGround, FMath::Clamp(Along / (2.0 * RidgeHalf), 0.0, 1.0));
	}

	/** The bore's columns: (along from the tip, across), pairs along its walls. */
	TArray<FVector2D> BorePillars()
	{
		TArray<FVector2D> Pillars;
		for (double Along = PillarFirst; Along <= 2.0 * RidgeHalf - PillarFirst + 1.0; Along += PillarEvery)
		{
			Pillars.Add(FVector2D(Along, -(BoreHalf - PillarInset)));
			Pillars.Add(FVector2D(Along, BoreHalf - PillarInset));
		}
		return Pillars;
	}

	/** The bore's roof over its floor (cm) at Along, V: its vault, coming down in a fan on each column. */
	double BoreRoof(double Along, double V, const LBAPlanetSites::FSite& Site)
	{
		const double Out = FMath::Clamp(FMath::Abs(V) / BoreHalf, 0.0, 1.0);
		double Roof = BoreSpring + BoreRise * FMath::Sqrt(1.0 - Out * Out);
		for (const FVector2D& Pillar : BorePillars())
		{
			const double Off = FMath::Max(FVector2D::Distance(FVector2D(Along, V), Pillar) - PillarRadius, 0.0);
			Roof = FMath::Min(Roof, PillarTop + FanSlope * Off);
		}
		return Roof;
	}

	/**
	 * In the bore's mouth (Along, V; H the middle of a ridge cell over the floor): what is cut out of the ridge's faces. A
	 * little wider than the bore all round, the gap closed by its flare.
	 */
	bool InMouth(double Along, double V, double H, const LBAPlanetSites::FSite& Site)
	{
		return Along > -300.0 && Along < 2.0 * RidgeHalf + 300.0 && FMath::Abs(V) < BoreHalf + MouthWiden && H < BoreRoof(Along, V, Site) + MouthWiden;
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

	/** A car's headroom in LBA1 cells (32 cm), and how many columns next to its start need no room at its sides. */
	constexpr int32 CarHeadroom = 4;
	constexpr int32 CarSqueezeSteps = 3;
	/** A road may leave a town this many columns (64 cm) further from its vehicle than the nearest way out. */
	constexpr int32 RimSlack = 40;
	/** Where a road leaves a town, the ground off the bricks is at most this far (cm) from the street, and this close (cells). */
	constexpr double RimHeight = 150.0;
	constexpr int32 RimOutside = 3;
	/**
	 * A ride's road keeps this far (cm) off a town's bricks: its width and verges, the car's, and the strip of sloped ground
	 * round a town (the road isn't graded into that). Nearer costs up to this many times level ground, so it leaves a town
	 * squarely and then stays away.
	 */
	constexpr double TownMargin = 600.0;
	constexpr double TownMarginCost = 30.0;
	/** A ride's road has no kerb this far (cm) from where it leaves a town: the way in stays open. */
	constexpr double RideKerbSkip = 500.0;
	/** A boulder's center nearer than this (cm) to a road's middle is dropped: the inside of a bend's kerb lands on the road. */
	constexpr double BoulderClear = 180.0;
	/** A parked car faces along its way this far on (cm). */
	constexpr double CarLookAhead = 500.0;

	/**
	 * The town's columns a car can reach from Map (breadth first, so each step's Steps is its distance): flat from column to
	 * column, a brick up or down only over a ramp, with headroom and a clear column on each side (the buggy is 1.7 m wide).
	 */
	bool FloodStreets(const LBAPlanetDistricts::FTown& Town, const FTransform& FrameToMap, const FVector& Map, TArray<LBAPlanetSites::FStreetStep>& Out)
	{
		const double CellCm = LBA1::CellSize / 8.0;
		const double StepCm = LBA1::CellHeight / 8.0;
		const FVector Origin = Town.GetOrigin();
		const FVector Cell = (FrameToMap.InverseTransformPositionNoScale(Map) - Origin) / FVector(CellCm, CellCm, StepCm);
		auto ShapeAt = [&Town](int32 X, int32 Y, int32 Z)
		{
			if (X < 0 || Y < 0 || Z < 0 || X >= Town.Size.X || Y >= Town.Size.Y || Z >= Town.Size.Z)
			{
				return 0;
			}
			const int32 Index = Town.Index(X, Y, Z);
			return Town.Bricks[Index] != 0 ? int32(Town.Shapes[Index]) : 0;
		};
		auto IsRamp = [](int32 Shape) { return Shape >= 2 && Shape <= 5; };
		auto Clear = [&](int32 X, int32 Floor, int32 Z)
		{
			for (int32 Y = Floor; Y < Floor + CarHeadroom; ++Y)
			{
				const int32 Shape = ShapeAt(X, Y, Z);
				if (Shape != 0 && !IsRamp(Shape))
				{
					return false;
				}
			}
			return true;
		};
		auto Stands = [&](int32 X, int32 Floor, int32 Z) { return ShapeAt(X, Floor - 1, Z) != 0 && Clear(X, Floor, Z); };
		auto InTown = [&Town](int32 X, int32 Z) { return X >= 0 && Z >= 0 && X < Town.Size.X && Z < Town.Size.Z && Town.Scenes[Z * Town.Size.X + X] != INDEX_NONE; };
		auto MapOf = [&](int32 X, int32 Floor, int32 Z)
		{
			return FrameToMap.TransformPosition(Origin + FVector((X + 0.5) * CellCm, (Z + 0.5) * CellCm, Floor * StepCm));
		};
		static const FIntPoint Sides[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

		const int32 X0 = FMath::RoundToInt(Cell.X);
		const int32 Z0 = FMath::RoundToInt(Cell.Y);
		int32 Floor0 = INDEX_NONE;
		for (const int32 Offset : { 0, 1, -1, 2, -2 })
		{
			if (Floor0 == INDEX_NONE && Stands(X0, FMath::RoundToInt(Cell.Z) + Offset, Z0))
			{
				Floor0 = FMath::RoundToInt(Cell.Z) + Offset;
			}
		}
		Out.Reset();
		if (Floor0 == INDEX_NONE)
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub: no street under (%.0f, %.0f, %.0f): cell (%.1f, %.1f, %.1f) of a town %d x %d x %d, its column's shapes around: %d %d %d"),
				Map.X, Map.Y, Map.Z, Cell.X, Cell.Y, Cell.Z, Town.Size.X, Town.Size.Y, Town.Size.Z,
				ShapeAt(X0, FMath::RoundToInt(Cell.Z) - 2, Z0), ShapeAt(X0, FMath::RoundToInt(Cell.Z) - 1, Z0), ShapeAt(X0, FMath::RoundToInt(Cell.Z), Z0));
			return false;
		}
		TArray<int32> Seen;
		Seen.Init(INDEX_NONE, Town.Bricks.Num());
		TArray<FIntVector> Cells = { FIntVector(X0, Floor0, Z0) };
		Seen[Town.Index(X0, Floor0, Z0)] = 0;
		Out.AddDefaulted_GetRef().Map = MapOf(X0, Floor0, Z0);
		for (int32 Head = 0; Head < Cells.Num(); ++Head)
		{
			const FIntVector Here = Cells[Head];
			for (const FIntPoint& Side : Sides)
			{
				const int32 X = Here.X + Side.X;
				const int32 Z = Here.Z + Side.Y;
				if (!InTown(X, Z))
				{
					Out[Head].bRim = true;
					continue;
				}
				for (const int32 Rise : { 0, 1, -1 })
				{
					const int32 Floor = Here.Y + Rise;
					if (!Stands(X, Floor, Z) || (Rise != 0 && !IsRamp(ShapeAt(X, Floor - 1, Z)) && !IsRamp(ShapeAt(Here.X, Here.Y - 1, Here.Z))))
					{
						continue;
					}
					bool bWide = true;
					for (const FIntPoint& Beside : Sides)
					{
						bWide = bWide && Clear(X + Beside.X, Floor, Z + Beside.Y);
					}
					if ((bWide || Out[Head].Steps < CarSqueezeSteps) && Seen[Town.Index(X, Floor, Z)] == INDEX_NONE)
					{
						Seen[Town.Index(X, Floor, Z)] = Cells.Num();
						Cells.Add(FIntVector(X, Floor, Z));
						LBAPlanetSites::FStreetStep& Step = Out.AddDefaulted_GetRef();
						Step.Map = MapOf(X, Floor, Z);
						Step.Parent = Head;
						Step.Steps = Out[Head].Steps + 1;
					}
					break;
				}
			}
		}
		return true;
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
		Entries.Add({ Site.Island, Site.Name, Site.Scene, Site.FromScene, Site.bRock, Place.GetMapLocation(Radius) });
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
	Channels.Reset();
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
		Site.FromScene = Entry.FromScene;
		Site.bRock = Entry.bRock;
		Site.Marker = Entry.Marker;
		Site.Tip = Tip;
		const FVector2D Out = Entry.Marker - Tip;
		Site.Dir = Out.Size() > 300.0 ? Out.GetSafeNormal() : (Entry.Marker - Centroid).GetSafeNormal();
		// The causeway starts at the tip, or past the rock where the hole comes out; the islet at least CausewayMin on.
		Site.Shore = Site.bRock ? Tip + Site.Dir * (2.0 * RidgeHalf + Landing - 100.0) : Tip;
		Site.Islet = Site.Shore + Site.Dir * FMath::Clamp(FVector2D::Distance(Entry.Marker, Site.Shore), CausewayMin, CausewayMax);
		if (Site.bRock)
		{
			// The ridge spans the arm and runs out into the sea past its coasts.
			Site.RidgeCenter = Tip + Site.Dir * RidgeHalf;
			auto Reach = [&](double Sign)
			{
				double Out = 0.0;
				for (; Out < RidgeMaxHalf; Out += 50.0)
				{
					const FVector2D P = Site.RidgeCenter + Side(Site.Dir) * Sign * Out;
					const int32 X = FMath::RoundToInt((P.X - Grid.Origin.X) / Grid.Cell);
					const int32 Y = FMath::RoundToInt((P.Y - Grid.Origin.Y) / Grid.Cell);
					if (X < 0 || Y < 0 || X >= Grid.SizeX || Y >= Grid.SizeY || !MainLand[Y * Grid.SizeX + X])
					{
						break;
					}
				}
				return FMath::Clamp(Out + RidgeEnds, RidgeMinHalf, RidgeMaxHalf);
			};
			Site.RidgeLeft = Reach(-1.0);
			Site.RidgeRight = Reach(1.0);
		}
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
				// Under the ridge (its mesh, EmitRocks) and round its foot: level across, sloping from its near mouth to its far one.
				const double Inside = RidgeField(P, Site);
				if (Inside > -150.0)
				{
					const double Floor = RidgeFloor(FVector2D::DotProduct(P - Site.Tip, Site.Dir), Site);
					const double Keep = FMath::SmoothStep(-150.0, 0.0, Inside);
					Height = float(FMath::Lerp(double(Height), Floor, Keep));
				}
			}
		}
	}
}

int32 LBAPlanetSites::FSites::OpenHarbours(const FString& Island, const TArray<FVector2D>& Docks, TArray<bool>& Land, const TArray<bool>& Pad)
{
	using namespace LBAPlanetSitesPrivate;
	const int32 SizeX = Grid.SizeX;
	const int32 SizeY = Grid.SizeY;
	const int32 Cells = SizeX * SizeY;
	Channels.Init(false, Cells);
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
					if ((Pad.IsEmpty() || !Pad[Index]) && FMath::Square(X - PX) + FMath::Square(Y - PY) <= Reach * Reach)
					{
						Channels[Index] = true;
						Here += Land[Index] ? 1 : 0;
						Land[Index] = false;
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

void LBAPlanetSites::FSites::FindWays(const LBAPlanetDistricts::FDistricts& Districts, const TArray<LBAPlanetDistricts::FPlaced>& Placed)
{
	for (FSite& Site : Sites)
	{
		Site.bWay = false;
		for (const LBAPlanetDistricts::FPlaced& District : Placed)
		{
			if (!Site.bWay && Districts.FindWayTo(District, Site.FromScene, Site.WayPoint, Site.WayDir))
			{
				Site.bWay = true;
				UE_LOG(LogLBA2, Display, TEXT("LBA2 planet stub: the road to the %s leaves %s at (%.1f, %.1f) m, where LBA1 leads to scene %d"),
					*Site.Name, *District.Name, Site.WayPoint.X / 100.0, Site.WayPoint.Y / 100.0, Site.FromScene);
			}
		}
	}
}

void LBAPlanetSites::FSites::FindRides(const LBAPlanetDistricts::FDistricts& Districts, const TArray<LBAPlanetDistricts::FPlaced>& Placed)
{
	using namespace LBAPlanetSitesPrivate;
	Rides.Reset();
	TownCells.Reset();
	if (Placed.IsEmpty())
	{
		return;
	}
	TownCells.Init(false, Grid.SizeX * Grid.SizeY);
	TArray<LBAPlanetDistricts::FTown> Towns;
	Towns.SetNum(Placed.Num());
	const double CellCm = LBA1::CellSize / 8.0;
	for (int32 Place = 0; Place < Placed.Num(); ++Place)
	{
		LBAPlanetDistricts::FTown& Town = Towns[Place];
		if (!Districts.MakeTown(Placed[Place], Town))
		{
			continue;
		}
		const FVector Origin = Town.GetOrigin();
		for (int32 Z = 0; Z < Town.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Town.Size.X; ++X)
			{
				if (Town.Scenes[Z * Town.Size.X + X] == INDEX_NONE)
				{
					continue;
				}
				const FVector P = Placed[Place].FrameToMap.TransformPosition(Origin + FVector((X + 0.5) * CellCm, (Z + 0.5) * CellCm, 0.0));
				const int32 CX = FMath::RoundToInt((P.X - Grid.Origin.X) / Grid.Cell);
				const int32 CY = FMath::RoundToInt((P.Y - Grid.Origin.Y) / Grid.Cell);
				if (CX >= 0 && CY >= 0 && CX < Grid.SizeX && CY < Grid.SizeY)
				{
					TownCells[CY * Grid.SizeX + CX] = true;
				}
			}
		}
	}
	for (int32 Place = 0; Place < Placed.Num(); ++Place)
	{
		for (const LBAPlanetDistricts::FPlaced::FVehicle& Start : Placed[Place].Vehicles)
		{
			if (!Start.bStart)
			{
				continue;
			}
			for (int32 Other = 0; Other < Placed.Num(); ++Other)
			{
				for (const LBAPlanetDistricts::FPlaced::FVehicle& End : Placed[Other].Vehicles)
				{
					if (End.bStart || End.Ride != Start.Ride)
					{
						continue;
					}
					FRide Ride;
					Ride.Scene = Start.Scene;
					Ride.ToScene = End.Scene;
					Ride.Parked = Start.Location;
					Ride.Heading = Start.Heading;
					if (FloodStreets(Towns[Place], Placed[Place].FrameToMap, Start.Location, Ride.From) && FloodStreets(Towns[Other], Placed[Other].FrameToMap, End.Location, Ride.To))
					{
						Rides.Add(MoveTemp(Ride));
					}
					else
					{
						UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub: LBA1's vehicle in scene %d or %d doesn't stand on its streets: no ride"), Start.Scene, End.Scene);
					}
				}
			}
		}
	}
}

void LBAPlanetSites::FSites::LayRoads(TArray<float>& Heights, TArray<bool>& Land, const TArray<bool>& Pad, const TArray<bool>& Avoid, TArray<float>& RoadShare, TArray<float>& VergeShare)
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
			if (Site.bRock && RidgeField(P, Site) > -Margin)
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

	// A step of road over the ground (not whether it may go there): across water a causeway, dearer the deeper; else its
	// length, weighted by the climb, a cliff costing dearly.
	auto StepCost = [&](int32 From, int32 To, double Run)
	{
		if (!Land[To])
		{
			return Run * (CausewayCost + FMath::Max(0.0, -double(Heights[To])) / 100.0);
		}
		const double Climb = FMath::Abs(Heights[To] - Heights[From]);
		const double Grade = Climb / (Run * Grid.Cell);
		const double Kept = FMath::Min(Grade, CliffGrade);
		return Run * (1.0 + 40.0 * Kept * Kept + 4.0 * Grade) + (Grade > CliffGrade ? CliffCost * Climb / Grid.Cell : 0.0);
	};

	TArray<FVector2D> RoadSamples;
	// Along a path: causeways where it crosses water, graded heights, the road laid (Splat) and its kerbs of boulders.
	struct FLaidRoad
	{
		TArray<FVector> Road;
		double Length = 0.0;
		double Steepest = 0.0;
		int32 Built = 0;
	};
	auto LayLine = [&](const TArray<FVector2D>& Points, double KerbSkip, int32 Seed)
	{
		constexpr double Spacing = 50.0;
		const TArray<FVector2D> Line = Resample(Points, Spacing);
		TArray<double> Z;
		for (const FVector2D& P : Line)
		{
			Z.Add(Land[CellOf(P)] ? SampleHeight(Heights, P) : LowGround);
		}
		FLaidRoad Laid;
		// Where it crosses water, a causeway: land under it, at the shore's height (then graded with the rest).
		const int32 Wide = FMath::CeilToInt(CausewayHalf / Grid.Cell);
		for (const FVector2D& P : Line)
		{
			if (Land[CellOf(P)])
			{
				continue;
			}
			const FVector2D C = (P - Grid.Origin) / Grid.Cell;
			for (int32 Y = FMath::Max(FMath::RoundToInt(C.Y) - Wide, 0); Y <= FMath::Min(FMath::RoundToInt(C.Y) + Wide, SizeY - 1); ++Y)
			{
				for (int32 X = FMath::Max(FMath::RoundToInt(C.X) - Wide, 0); X <= FMath::Min(FMath::RoundToInt(C.X) + Wide, SizeX - 1); ++X)
				{
					const int32 Index = Y * SizeX + X;
					if (!Land[Index] && !IsPad(Index) && (Avoid.IsEmpty() || !Avoid[Index]) && FVector2D::Distance(CellToMap(X, Y), P) <= CausewayHalf)
					{
						Land[Index] = true;
						Heights[Index] = float(LowGround);
						++Laid.Built;
					}
				}
			}
		}
		const TArray<double> Profile = GradeProfile(Z, Spacing, RoadGrade);
		for (int32 Index = 0; Index < Line.Num(); ++Index)
		{
			Laid.Road.Add(FVector(Line[Index], Profile[Index]));
			if (Index > 0)
			{
				const double Run = FVector2D::Distance(Line[Index], Line[Index - 1]);
				Laid.Length += Run;
				Laid.Steepest = FMath::Max(Laid.Steepest, FMath::Abs(Profile[Index] - Profile[Index - 1]) / FMath::Max(Run, Spacing));
			}
			Splat(Line[Index], Profile[Index], true);
		}
		RoadSamples.Append(Line);
		Kerbs(Line, KerbSkip, Seed);
		return Laid;
	};

	int32 SiteSeed = 1;
	for (FSite& Site : Sites)
	{
		const FVector2D End = Site.bRock ? Site.Tip - Site.Dir * 250.0 : Site.Tip;
		const int32 Goal = CellOf(End);
		// Nothing past a rock's face: the road stops in front of it.
		auto Blocked = [&](int32 Index)
		{
			if ((Land[Index] && IsPad(Index) && !IsEdge[Index]) || (!Land[Index] && !Avoid.IsEmpty() && Avoid[Index]))
			{
				return true;
			}
			const FVector2D P = CellToMap(Index % SizeX, Index / SizeX);
			for (const FSite& Other : Sites)
			{
				const double Along = FVector2D::DotProduct(P - Other.Tip, Other.Dir);
				const double Across = FVector2D::DotProduct(P - Other.RidgeCenter, Side(Other.Dir));
				if (Other.bRock && Along > -100.0 && Along < 2.0 * RidgeHalf + 3000.0 && Across > -Other.RidgeLeft - 300.0 && Across < Other.RidgeRight + 300.0)
				{
					return true;
				}
			}
			return false;
		};
		// From where LBA1 leaves the town for the stone: straight out of it first, then on by the cheapest way.
		TArray<int32> Starts;
		TArray<FVector2D> Lead;
		if (Site.bWay)
		{
			FVector2D Out = Site.WayPoint + Site.WayDir * 500.0;
			for (int32 Step = 0; Step < 10 && IsPad(CellOf(Out)); ++Step)
			{
				Out += Site.WayDir * 100.0;
			}
			Lead = { Site.WayPoint, Out };
			Starts.Add(CellOf(Out));
		}
		else
		{
			Starts = Edge;
			Starts.Sort([&](int32 A, int32 B) { return FVector2D::DistSquared(CellToMap(A % SizeX, A / SizeX), End) < FVector2D::DistSquared(CellToMap(B % SizeX, B / SizeX), End); });
			Starts.SetNum(FMath::Min(Starts.Num(), 80));
		}
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
				return StepCost(From, To, Run);
			},
			[&](int32 Index) { return Index == Goal; });
		if (Cellpath.IsEmpty())
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub: no way found for the road to the %s"), *Site.Name);
			continue;
		}
		TArray<FVector2D> Points = Lead;
		for (int32 Index = Lead.IsEmpty() ? 0 : 1; Index < Cellpath.Num(); ++Index)
		{
			Points.Add(CellToMap(Cellpath[Index] % SizeX, Cellpath[Index] / SizeX));
		}
		Points = SmoothPath(Points, 4);
		if (!Site.bRock)
		{
			// On along the causeway to the islet.
			Points.Add(Site.Islet);
		}
		FLaidRoad Laid = LayLine(Points, 400.0, 100 * SiteSeed);
		const int32 Built = Laid.Built;
		Site.Road = MoveTemp(Laid.Road);
		Site.RoadLength = Laid.Length;
		Site.Steepest = Laid.Steepest;
		constexpr double Spacing = 50.0;
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
		for (const FVector& Point : Site.Road)
		{
			Lowest = FMath::Min(Lowest, Point.Z);
			Highest = FMath::Max(Highest, Point.Z);
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet stub: road to the %s, %.0f m (%d m2 of causeway), steepest %.0f %%, from %.1f to %.1f m high; tip (%.1f, %.1f) m, islet (%.1f, %.1f) m on the map, %.0f m from its marker"),
			*Site.Name, Site.RoadLength / 100.0, Built, 100.0 * Site.Steepest, Lowest / 100.0, Highest / 100.0, Site.Tip.X / 100.0, Site.Tip.Y / 100.0,
			Site.Islet.X / 100.0, Site.Islet.Y / 100.0, FVector2D::Distance(Site.Islet, Site.Marker) / 100.0);
		++SiteSeed;
	}

	// LBA1's vehicle rides: from the streets a car reaches at one end to those at the other, off the towns' bricks between.
	TArray<float> FromTown;
	if (!Rides.IsEmpty())
	{
		TArray<bool> OffTown;
		OffTown.SetNumUninitialized(Cells);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			OffTown[Index] = !TownCells[Index];
		}
		FromTown = SiteDistanceOutOf(OffTown, SizeX, SizeY);
	}
	int32 RideSeed = 50;
	for (FRide& Ride : Rides)
	{
		// Where a road can leave some streets: per stub cell, the street step nearest its vehicle.
		auto WaysOut = [&](const TArray<FStreetStep>& Streets)
		{
			auto Usable = [&](const FStreetStep& Step)
			{
				const int32 Cell = CellOf(FVector2D(Step.Map));
				if (!Step.bRim || !Land[Cell] || FMath::Abs(Heights[Cell] - Step.Map.Z) > RimHeight)
				{
					return false;
				}
				// Land off the bricks right there (not a quay over the sea).
				const int32 CX = Cell % SizeX;
				const int32 CY = Cell / SizeX;
				for (int32 Y = FMath::Max(CY - RimOutside, 0); Y <= FMath::Min(CY + RimOutside, SizeY - 1); ++Y)
				{
					for (int32 X = FMath::Max(CX - RimOutside, 0); X <= FMath::Min(CX + RimOutside, SizeX - 1); ++X)
					{
						if (Land[Y * SizeX + X] && !TownCells[Y * SizeX + X])
						{
							return true;
						}
					}
				}
				return false;
			};
			int32 Nearest = MAX_int32;
			for (const FStreetStep& Step : Streets)
			{
				Nearest = Usable(Step) ? FMath::Min(Nearest, Step.Steps) : Nearest;
			}
			TMap<int32, int32> Cells;
			for (int32 Index = 0; Index < Streets.Num(); ++Index)
			{
				if (Usable(Streets[Index]) && Streets[Index].Steps <= Nearest + RimSlack)
				{
					int32& Best = Cells.FindOrAdd(CellOf(FVector2D(Streets[Index].Map)), Index);
					Best = Streets[Index].Steps < Streets[Best].Steps ? Index : Best;
				}
			}
			return Cells;
		};
		const TMap<int32, int32> FromCells = WaysOut(Ride.From);
		const TMap<int32, int32> ToCells = WaysOut(Ride.To);
		if (FromCells.IsEmpty() || ToCells.IsEmpty())
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub: no way out of the streets for the ride from scene %d to %d"), Ride.Scene, Ride.ToScene);
			continue;
		}
		TArray<int32> Starts;
		FromCells.GetKeys(Starts);
		const TArray<int32> Cellpath = CheapestPath(SizeX, SizeY, Starts,
			[&](int32 From, int32 To, double Run)
			{
				if ((TownCells[To] && !FromCells.Contains(To) && !ToCells.Contains(To)) || (!Land[To] && !Avoid.IsEmpty() && Avoid[To]))
				{
					return -1.0;
				}
				const double Near = FMath::Max(0.0, 1.0 - FromTown[To] * Grid.Cell / TownMargin);
				return StepCost(From, To, Run) + Run * TownMarginCost * Near;
			},
			[&](int32 Index) { return ToCells.Contains(Index); });
		if (Cellpath.IsEmpty())
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 planet stub: no road found for the ride from scene %d to %d"), Ride.Scene, Ride.ToScene);
			continue;
		}
		const FStreetStep& Leaves = Ride.From[FromCells[Cellpath[0]]];
		const FStreetStep& Arrives = Ride.To[ToCells[Cellpath.Last()]];
		TArray<FVector2D> Points = { FVector2D(Leaves.Map) };
		for (int32 Index = 1; Index + 1 < Cellpath.Num(); ++Index)
		{
			Points.Add(CellToMap(Cellpath[Index] % SizeX, Cellpath[Index] / SizeX));
		}
		Points.Add(FVector2D(Arrives.Map));
		const FLaidRoad Laid = LayLine(SmoothPath(Points, 4), RideKerbSkip, 100 * RideSeed);
		++RideSeed;

		// The whole trip: through the streets to where the road leaves, along it, through the streets at the far end.
		Ride.Route.Reset();
		for (int32 Step = FromCells[Cellpath[0]]; Step != INDEX_NONE; Step = Ride.From[Step].Parent)
		{
			Ride.Route.Insert(FVector2D(Ride.From[Step].Map), 0);
		}
		for (const FVector& Point : Laid.Road)
		{
			Ride.Route.Add(FVector2D(Point));
		}
		for (int32 Step = ToCells[Cellpath.Last()]; Step != INDEX_NONE; Step = Ride.To[Step].Parent)
		{
			Ride.Route.Add(FVector2D(Ride.To[Step].Map));
		}
		// The car faces along it.
		for (const FVector2D& Point : Ride.Route)
		{
			if (FVector2D::Distance(Point, FVector2D(Ride.Parked)) >= CarLookAhead)
			{
				Ride.Heading = (Point - FVector2D(Ride.Parked)).GetSafeNormal();
				break;
			}
		}
		double Lowest = TNumericLimits<double>::Max(), Highest = -TNumericLimits<double>::Max();
		for (const FVector& Point : Laid.Road)
		{
			Lowest = FMath::Min(Lowest, Point.Z);
			Highest = FMath::Max(Highest, Point.Z);
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet stub: road for the ride from scene %d to %d, %.0f m (%d m2 of causeway), steepest %.0f %%, from %.1f to %.1f m high, leaving %d columns from the car"),
			Ride.Scene, Ride.ToScene, Laid.Length / 100.0, Laid.Built, 100.0 * Laid.Steepest, Lowest / 100.0, Highest / 100.0, Leaves.Steps);
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
		if (RoadSamples.ContainsByPredicate([&P](const FVector2D& Sample) { return FVector2D::DistSquared(P, Sample) < FMath::Square(BoulderClear); }))
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

void LBAPlanetSites::FSites::EmitRocks(const TArray<float>& Heights, const FVector& Pivot, const FLBAVertex& Rock, const FLBAVertex& Grass, const FLBAVertex& Dark,
	FLBAMeshBuilder& Builder, int32 Section, UMaterialInterface* WallMaterial, UMaterialInterface* RoofMaterial) const
{
	using namespace LBAPlanetSitesPrivate;
	// The Temple of Bu's finishes (its enclosure's tiles, laid by position as its own walls and roof are), else the ground's.
	const int32 WallSection = WallMaterial ? Builder.GetSection(TEXT("TempleWalls"), WallMaterial) : Section;
	const int32 RoofSection = RoofMaterial ? Builder.GetSection(TEXT("TempleRoof"), RoofMaterial) : Section;
	FLBAVertex Temple;
	Temple.Color = FVector4f(1.f, 1.f, 1.f, 0.42f);
	auto Emit = [&](int32 Into, const FLBAVertex& Kind, const FVector& A, const FVector& B, const FVector& C, const FVector2f UVs[3], const FVector& Outward)
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
			if (UVs)
			{
				V[Corner].UV = UVs[Corner];
			}
		}
		Builder.AddTriangleFacing(Into, V[0], V[1], V[2], FVector3f(Normal));
	};
	// A, B, C, D round the quad; UVs optional (four).
	auto Quad = [&](int32 Into, const FLBAVertex& Kind, const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Outward, const FVector2f* UVs = nullptr)
	{
		const FVector2f First[3] = { UVs ? UVs[0] : FVector2f::ZeroVector, UVs ? UVs[1] : FVector2f::ZeroVector, UVs ? UVs[2] : FVector2f::ZeroVector };
		const FVector2f Second[3] = { UVs ? UVs[0] : FVector2f::ZeroVector, UVs ? UVs[2] : FVector2f::ZeroVector, UVs ? UVs[3] : FVector2f::ZeroVector };
		Emit(Into, Kind, A, B, C, UVs ? First : nullptr, Outward);
		Emit(Into, Kind, A, C, D, UVs ? Second : nullptr, Outward);
	};
	// The Temple's finish tile by position (LBAIsoImporter's FinishUV): 16 bricks a tile, a brick 64 cm across and 32 cm up;
	// from below, two bricks per 64 cm.
	auto WallUV = [](double Along, double Z) { return FVector2f(float(Along / 64.0 / 16.0), float(-Z / 32.0 / 16.0)); };
	auto RoofUV = [](double Across, double Along) { return FVector2f(float(Across / 64.0 / 16.0), float(Along * 2.0 / 64.0 / 16.0)); };

	for (const FSite& Site : Sites)
	{
		if (!Site.bRock)
		{
			continue;
		}
		const FVector2D Across = Side(Site.Dir);
		// Ridge coordinates: Along from the tip on the way out, V across from the way's line.
		auto MapAt = [&](double Along, double V) { return Site.Tip + Site.Dir * Along + Across * V; };
		auto RockZ = [&](const FVector2D& P) { return double(SampleHeight(Heights, P)) + RidgeRise(P, Site) - RockSink; };

		// The ridge: a fine height field over its footprint, rising out of the ground; the hole cut out of its faces.
		const double AlongMin = -RidgeHalf - 300.0;
		const double AlongMax = 3.0 * RidgeHalf + 300.0;
		const double VMin = -Site.RidgeLeft - 200.0;
		const double VMax = Site.RidgeRight + 200.0;
		const int32 NA = FMath::CeilToInt((AlongMax - AlongMin) / RockCell);
		const int32 NV = FMath::CeilToInt((VMax - VMin) / RockCell);
		TArray<FVector> Points;
		TArray<double> Rises;
		Points.SetNumUninitialized((NA + 1) * (NV + 1));
		Rises.SetNumUninitialized((NA + 1) * (NV + 1));
		for (int32 I = 0; I <= NA; ++I)
		{
			for (int32 J = 0; J <= NV; ++J)
			{
				const FVector2D P = MapAt(AlongMin + I * RockCell, VMin + J * RockCell);
				const double Up = RidgeRise(P, Site);
				Rises[I * (NV + 1) + J] = Up;
				Points[I * (NV + 1) + J] = FVector(P, SampleHeight(Heights, P) + Up - RockSink);
			}
		}
		auto PointAt = [&](int32 I, int32 J) { return Points[I * (NV + 1) + J]; };
		auto NormalAt = [&](int32 I, int32 J)
		{
			const FVector DA = PointAt(FMath::Min(I + 1, NA), J) - PointAt(FMath::Max(I - 1, 0), J);
			const FVector DV = PointAt(I, FMath::Min(J + 1, NV)) - PointAt(I, FMath::Max(J - 1, 0));
			FVector Normal = FVector::CrossProduct(DA, DV).GetSafeNormal();
			return Normal.Z < 0.0 ? -Normal : Normal;
		};
		for (int32 I = 0; I < NA; ++I)
		{
			for (int32 J = 0; J < NV; ++J)
			{
				const double Highest = FMath::Max(FMath::Max(Rises[I * (NV + 1) + J], Rises[I * (NV + 1) + J + 1]), FMath::Max(Rises[(I + 1) * (NV + 1) + J], Rises[(I + 1) * (NV + 1) + J + 1]));
				if (Highest < RockSink + 3.0)
				{
					continue;
				}
				const double Along = AlongMin + (I + 0.5) * RockCell;
				const double V = VMin + (J + 0.5) * RockCell;
				const FVector Mid = (PointAt(I, J) + PointAt(I + 1, J) + PointAt(I, J + 1) + PointAt(I + 1, J + 1)) / 4.0;
				if (InMouth(Along, V, Mid.Z - RidgeFloor(Along, Site), Site))
				{
					continue;
				}
				const FVector2D P(Mid);
				const bool bGrassy = Highest > 0.6 * RidgeHeight && SiteNoise(P, 150.0, 6.0) > -0.25;
				const int32 Corners[4][2] = { { I, J }, { I + 1, J }, { I + 1, J + 1 }, { I, J + 1 } };
				FLBAVertex V4[4];
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					V4[Corner] = bGrassy ? Grass : Rock;
					V4[Corner].Position = FVector3f(PointAt(Corners[Corner][0], Corners[Corner][1]) - Pivot);
					V4[Corner].Normal = FVector3f(NormalAt(Corners[Corner][0], Corners[Corner][1]));
				}
				Builder.AddTriangleFacing(Section, V4[0], V4[1], V4[2], FVector3f::UpVector);
				Builder.AddTriangleFacing(Section, V4[0], V4[2], V4[3], FVector3f::UpVector);
			}
		}

		// The bore: where it meets the faces, per line across it (the first point out from its middle where the rock is no
		// higher than its roof).
		const double Middle = RidgeHalf;
		auto Ends = [&](double V, double Lift, double& OutNear, double& OutFar)
		{
			auto Covered = [&](double Along) { return RockZ(MapAt(Along, V)) - RidgeFloor(Along, Site) > BoreRoof(Along, V, Site) + Lift; };
			OutNear = Middle;
			while (OutNear > AlongMin && Covered(OutNear - 5.0))
			{
				OutNear -= 5.0;
			}
			OutFar = Middle;
			while (OutFar < AlongMax && Covered(OutFar + 5.0))
			{
				OutFar += 5.0;
			}
		};
		constexpr int32 Lines = 36;
		constexpr int32 Steps = 64;
		TArray<double> Nears, Fars;
		for (int32 Line = 0; Line <= Lines; ++Line)
		{
			double Near = 0.0, Far = 0.0;
			Ends(FMath::Lerp(-BoreHalf, BoreHalf, double(Line) / Lines), 0.0, Near, Far);
			Nears.Add(Near);
			Fars.Add(Far);
		}
		// Its roof, seen from below: the Temple's vault coming down in fans on its columns.
		auto RoofPoint = [&](int32 Line, int32 Step)
		{
			const double V = FMath::Lerp(-BoreHalf, BoreHalf, double(Line) / Lines);
			const double Along = FMath::Lerp(Nears[Line], Fars[Line], double(Step) / Steps);
			return FVector(MapAt(Along, V), RidgeFloor(Along, Site) + BoreRoof(Along, V, Site));
		};
		auto RoofUVAt = [&](int32 Line, int32 Step)
		{
			const double V = FMath::Lerp(-BoreHalf, BoreHalf, double(Line) / Lines);
			return RoofUV(V, FMath::Lerp(Nears[Line], Fars[Line], double(Step) / Steps));
		};
		for (int32 Line = 0; Line < Lines; ++Line)
		{
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				const FVector2f UVs[4] = { RoofUVAt(Line, Step), RoofUVAt(Line + 1, Step), RoofUVAt(Line + 1, Step + 1), RoofUVAt(Line, Step + 1) };
				Quad(RoofSection, Temple, RoofPoint(Line, Step), RoofPoint(Line + 1, Step), RoofPoint(Line + 1, Step + 1), RoofPoint(Line, Step + 1), -FVector::UpVector, UVs);
			}
		}
		// Its walls, from under the floor up to the roof's edge.
		for (const int32 Line : { 0, Lines })
		{
			const double V = Line == 0 ? -BoreHalf : BoreHalf;
			const FVector Inward = FVector(Across * (Line == 0 ? 1.0 : -1.0), 0.0);
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				const double A0 = FMath::Lerp(Nears[Line], Fars[Line], double(Step) / Steps);
				const double A1 = FMath::Lerp(Nears[Line], Fars[Line], double(Step + 1) / Steps);
				const FVector Low0(MapAt(A0, V), RidgeFloor(A0, Site) - 20.0), Low1(MapAt(A1, V), RidgeFloor(A1, Site) - 20.0);
				const FVector High0 = RoofPoint(Line, Step), High1 = RoofPoint(Line, Step + 1);
				const FVector2f UVs[4] = { WallUV(A0, Low0.Z), WallUV(A1, Low1.Z), WallUV(A1, High1.Z), WallUV(A0, High0.Z) };
				Quad(WallSection, Temple, Low0, Low1, High1, High0, Inward, UVs);
			}
		}
		// A flare of rock round each mouth: from the bore's end out and back into the ridge, behind its face, so it shows
		// only where the face is cut: a mouth funnelling into the bore.
		for (const bool bFar : { false, true })
		{
			const double Sign = bFar ? 1.0 : -1.0;
			auto EndAt = [&](int32 Line) { return bFar ? Fars[Line] : Nears[Line]; };
			TArray<FVector> Ring;
			TArray<FVector> Outs;
			// Up the left wall, across the roof, down the right wall.
			for (int32 Row = 0; Row <= 4; ++Row)
			{
				const double Along = EndAt(0);
				Ring.Add(FVector(MapAt(Along, -BoreHalf), RidgeFloor(Along, Site) - 20.0 + (BoreRoof(Along, -BoreHalf, Site) + 20.0) * Row / 4.0));
				Outs.Add(Row == 4 ? FVector(-Across, 1.0).GetSafeNormal() : FVector(-Across, 0.0));
			}
			for (int32 Line = 1; Line < Lines; ++Line)
			{
				Ring.Add(RoofPoint(Line, bFar ? Steps : 0));
				Outs.Add(FVector::UpVector);
			}
			for (int32 Row = 4; Row >= 0; --Row)
			{
				const double Along = EndAt(Lines);
				Ring.Add(FVector(MapAt(Along, BoreHalf), RidgeFloor(Along, Site) - 20.0 + (BoreRoof(Along, BoreHalf, Site) + 20.0) * Row / 4.0));
				Outs.Add(Row == 4 ? FVector(Across, 1.0).GetSafeNormal() : FVector(Across, 0.0));
			}
			const FVector Back = FVector(Site.Dir * -Sign * FlareBack, 0.0);
			const FVector Facing = FVector(Site.Dir * Sign, 0.0);
			for (int32 Index = 0; Index + 1 < Ring.Num(); ++Index)
			{
				const FVector A = Ring[Index], B = Ring[Index + 1];
				const FVector OA = A + Outs[Index] * FlareOut + Back, OB = B + Outs[Index + 1] * FlareOut + Back;
				Quad(Section, Rock, A, B, OB, OA, (Facing + 0.5 * (Outs[Index] + Outs[Index + 1])).GetSafeNormal());
			}
		}
	}
}

void LBAPlanetSites::FSites::SpawnProps(FLBAImportContext& Context, UWorld* World, const TArray<float>& Heights) const
{
	using namespace LBAPlanetSitesPrivate;

	// The bores' columns: the Temple of Bu's own, from the remake, each its pillar's height, its foot on the floor.
	FLBARemakeAssets Remake(Context);
	UStaticMesh* Column = nullptr;
	if (Remake.Initialize())
	{
		Column = Remake.GetModelMesh(PillarFolder, PillarFile);
	}
	if (!Column && Sites.ContainsByPredicate([](const FSite& Site) { return Site.bRock; }))
	{
		Context.Warn(FString::Printf(TEXT("LBA1 sites: no %s/%s from the remake (remake models off, or no checkout): the bore's fans stand on nothing"), PillarFolder, PillarFile));
	}
	for (const FSite& Site : Sites)
	{
		if (!Site.bRock || !Column)
		{
			continue;
		}
		const FBox Box = Column->GetBoundingBox();
		const double Scale = (PillarTop + 15.0) / FMath::Max(Box.GetSize().Z, 1.0);
		// One mesh per column, not instances: the GPU's second view test on instances knows nothing of the bend (see LBAPlanetTowns).
		const FQuat Turn(FVector::UpVector, FMath::Atan2(Site.Dir.Y, Site.Dir.X));
		int32 ColumnIndex = 0;
		for (const FVector2D& Pillar : BorePillars())
		{
			const FVector2D At = Site.Tip + Site.Dir * Pillar.X + Side(Site.Dir) * Pillar.Y;
			const FVector Foot(At, RidgeFloor(Pillar.X, Site) - 5.0);
			const FVector Center = Box.GetCenter();
			const FTransform Transform(Turn, Foot - Turn.RotateVector(FVector(Center.X, Center.Y, Box.Min.Z) * Scale), FVector(Scale));
			if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, Transform, FString::Printf(TEXT("Site_%s_Column_%02d"), *FLBAImportContext::SanitizeName(Site.Name), ColumnIndex++), TEXT("Stubs/Sites")))
			{
				Actor->GetStaticMeshComponent()->SetStaticMesh(Column);
				Actor->Tags.Add(ULBAPlanetAsset::StubTag);
			}
		}
	}
	// The stones, on their islets, the carved side toward whoever comes down the road.
	if (!Sites.IsEmpty())
	{
		UStaticMesh* Stone = Remake.GetModelMesh(StoneFolder, StoneFile);
		if (!Stone)
		{
			Context.Warn(FString::Printf(TEXT("LBA1 sites: no model %s/%s in the plugin's Resources/Models: the sites have no stones"), StoneFolder, StoneFile));
		}
		else
		{
			const FBox Box = Stone->GetBoundingBox();
			const double Scale = StoneHeight / FMath::Max(Box.GetSize().Z, 1.0);
			for (const FSite& Site : Sites)
			{
				const FVector2D Face = -Site.Dir;
				const FQuat Turn(FVector::UpVector, FMath::Atan2(Face.Y, Face.X) - UE_HALF_PI);
				const FVector Foot(Site.Islet, SampleHeight(Heights, Site.Islet) - StoneSink - Box.Min.Z * Scale);
				if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Turn, Foot, FVector(Scale)), FString::Printf(TEXT("Site_%s_Stone"), *FLBAImportContext::SanitizeName(Site.Name)), TEXT("Stubs/Sites")))
				{
					Actor->GetStaticMeshComponent()->SetStaticMesh(Stone);
					Actor->Tags.Add(ULBAPlanetAsset::StubTag);
				}
			}
		}
	}
	// The rides' cars, where LBA1's vehicles set off (they set themselves down on the bricks when the level starts).
	for (const FRide& Ride : Rides)
	{
		const FTransform Parked(FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(Ride.Heading.Y, Ride.Heading.X)), 0.0), Ride.Parked + FVector(0.0, 0.0, 60.0));
		if (ALBACar* Car = Context.SpawnGeneratedActor<ALBACar>(World, Parked, FString::Printf(TEXT("Ride_Scene%d_Car"), Ride.Scene), TEXT("Stubs/Sites")))
		{
			Car->Tags.Add(ULBAPlanetAsset::StubTag);
			FString Route;
			FVector2D Last(TNumericLimits<double>::Max());
			for (const FVector2D& Point : Ride.Route)
			{
				if (FVector2D::Distance(Point, Last) >= 400.0 || &Point == &Ride.Route.Last())
				{
					Route += FString::Printf(TEXT("%s%.0f,%.0f"), Route.IsEmpty() ? TEXT("") : TEXT(";"), Point.X, Point.Y);
					Last = Point;
				}
			}
			UE_LOG(LogLBA2, Display, TEXT("LBA2 planet stub: a car for the ride from scene %d to %d at %s, facing %.0f; route on the map: %s"), Ride.Scene, Ride.ToScene, *Parked.GetLocation().ToCompactString(), Parked.Rotator().Yaw, *Route);
		}
	}
	for (const FSite& Site : Sites)
	{
		if (Site.bRock && Site.Road.Num() > 8)
		{
			// The buggy, parked along the road a little before it ends at the rock, the hole left in view: a car Twinsen can
			// drive (it sets itself down on the ground when the level starts).
			const FVector2D A(Site.Road[Site.Road.Num() - 8]);
			const FVector2D B(Site.Road.Last());
			const FVector2D Along = (B - A).GetSafeNormal();
			const FVector2D Map = B - Along * 500.0 + Side(Along) * 70.0;
			const FTransform Parked(FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(Along.Y, Along.X)), 0.0), FVector(Map, SampleHeight(Heights, Map) + 60.0));
			if (ALBACar* Car = Context.SpawnGeneratedActor<ALBACar>(World, Parked, FString::Printf(TEXT("Site_%s_Buggy"), *FLBAImportContext::SanitizeName(Site.Name)), TEXT("Stubs/Sites")))
			{
				Car->Tags.Add(ULBAPlanetAsset::StubTag);
			}
		}
	}
}
