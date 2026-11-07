#include "Import/LBAPlanetDistricts.h"

#include "Algo/StableSort.h"
#include "Async/ParallelFor.h"
#include "Formats/LBA1.h"
#include "Import/LBAPlanetKit.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"
#include "LBATypes.h"

namespace LBAPlanetDistrictsPrivate
{
	/** LBA1's outdoor scenes on the stub islands (its interiors are left to L5), in the order districts are found. */
	struct FOutdoor
	{
		const TCHAR* Island;
		int32 Scene;
		const TCHAR* Name;
	};
	const FOutdoor Outdoor[] =
	{
		{ TEXT("Principal"), 11, TEXT("Principal harbour") },
		{ TEXT("Principal"), 12, TEXT("Outside the fortress") },
		{ TEXT("Principal"), 13, TEXT("Old Burg") },
		{ TEXT("Principal"), 17, TEXT("Ruins") },
		{ TEXT("Principal"), 18, TEXT("Outside the library") },
		{ TEXT("Principal"), 19, TEXT("Military camp") },
		{ TEXT("Principal"), 25, TEXT("Peg Leg Street") },
		{ TEXT("Principal"), 24, TEXT("Port Belooga") },
		{ TEXT("Principal"), 37, TEXT("Water tower") },
		{ TEXT("Proxima"), 42, TEXT("Proxim City") },
		{ TEXT("Proxima"), 44, TEXT("Near the inventor's house") },
		{ TEXT("Rebellion"), 59, TEXT("Rebellion harbour") },
	};

	/** LBA1's cells in cm (units / 8, as LBA2's). */
	constexpr double CellCm = LBA1::CellSize / 8.0;
	constexpr double StepCm = LBA1::CellHeight / 8.0;

	/** The fit: how far from its markers' mean (cm) and heading 0 (degrees) a district may go, in turns of TurnStep. */
	constexpr double Reach = 4500.0;
	constexpr double MaxTurn = 30.0;
	constexpr double TurnStep = 3.0;
	/** The preference for the markers and heading 0: gaussians this wide, weighed against the coast's agreement. */
	constexpr double PriorReach = 2500.0;
	constexpr double PriorTurn = 15.0;
	constexpr double PriorWeight = 0.1;
	/** Per cell of a district over one placed before. */
	constexpr double OverlapCost = 3.0;
	/** The ground: the top bricks' height this share of the cells around (GroundWindow LBA1 cells, 11 m) are under. */
	constexpr int32 GroundWindow = 17;
	constexpr double GroundShare = 0.2;
	/** Smaller pieces of water (cm2: fountains, gutters) or without bricks are ground, level with the streets around them. */
	constexpr double Pond = 30.0 * 10000.0;
	/** A scene with this much water has a sea level of its own (logged where it isn't the district's). */
	constexpr int32 MinSeaCells = 64;
	/** A pad lies this far (cm) under its bricks' lowest faces. */
	constexpr double Sink = 10.0;
	/** Quays (bricks by the sea) reach this many cells under the district's sea, down to the carved sea floor. */
	constexpr int32 QuayDepth = 4;
	/** A top brick this many cells over the ground stands on it (a house, a tree): its ground color is the nearest ground's. */
	constexpr int32 StandsOn = 2;
	constexpr int32 ColorReach = 40;
	/**
	 * The stub's ground eases into a pad over this (cm), wider where they differ more, to keep the slope near MaxEaseSlope
	 * (a little under where the ground turns to rock), up to MaxBlend; EaseWander of that width comes and goes along the edge.
	 */
	constexpr double Blend = 600.0;
	constexpr double MaxBlend = 2500.0;
	constexpr double MaxEaseSlope = 0.36;
	constexpr double EaseWander = 0.4;
	constexpr double EaseWanderSize = 900.0;
	/** The ground around the pad's edge comes up to this (cm) under the streets' top: flush with them. */
	constexpr double EdgeUnder = 1.0;
	/** The pad's edge is its land this many of the stub's cells from land off it (m): what triangles reach past a town's last cell. */
	constexpr int32 EdgeCells = 2;
	/** A town's grass, earth and dark ground go on this far (cm) past its edge, about as far again or none where noise says. */
	constexpr double KindsReach = 400.0;
	constexpr double KindsWanderSize = 350.0;
	/** Flat tiles: the top diamond this covered, at most this many pixels out of it. */
	constexpr float FlatCoverage = 0.9f;
	constexpr int32 FlatOutside = 60;
	/** LBA1's palette ramps of its ground tiles: grass and its shadows, earth, and their least shares in a tile. */
	constexpr int32 GrassRamp = 2;
	constexpr int32 GrassShadowRamp = 11;
	constexpr int32 EarthRamps[] = { 8, 9, 3 };
	constexpr float MinNatural = 0.9f;
	constexpr float MinGrass = 0.1f;
	/** LBA1's dark ground (streets of beaten earth). */
	constexpr int32 DarkFirst = 964;
	constexpr int32 DarkLast = 972;

	/** An exit across a scene's edge moves the hero a scene's width one way and little the other. */
	bool IsEdgeExit(const FIntVector& Move)
	{
		return FMath::Max(FMath::Abs(Move.X), FMath::Abs(Move.Z)) >= 29000 && FMath::Min(FMath::Abs(Move.X), FMath::Abs(Move.Z)) <= 8192;
	}

	/** Index I mirrored back into 0..Num-1 about the edges (d c b a | a b c d | d c b a). */
	int32 Reflect(int32 I, int32 Num)
	{
		I = I < 0 ? -I - 1 : I;
		I = I >= Num ? 2 * Num - I - 1 : I;
		return FMath::Clamp(I, 0, Num - 1);
	}

	/** The Rank-th smallest value (0-based) of each cell's Size x Size window, edges mirrored. */
	TArray<float> RankFilter(const TArray<float>& In, int32 SizeX, int32 SizeY, int32 Size, int32 Rank)
	{
		TArray<float> Out;
		Out.SetNumUninitialized(In.Num());
		const int32 Half = Size / 2;
		ParallelFor(SizeY, [&](int32 Y)
		{
			TArray<float> Window;
			Window.SetNumUninitialized(Size * Size);
			for (int32 X = 0; X < SizeX; ++X)
			{
				int32 N = 0;
				for (int32 DY = -Half; DY <= Half; ++DY)
				{
					const int32 Row = Reflect(Y + DY, SizeY) * SizeX;
					for (int32 DX = -Half; DX <= Half; ++DX)
					{
						Window[N++] = In[Row + Reflect(X + DX, SizeX)];
					}
				}
				Window.Sort();
				Out[Y * SizeX + X] = Window[Rank];
			}
		});
		return Out;
	}

	/** The highest (or lowest) value of each cell's 3 x 3 window, edges mirrored. */
	TArray<float> Extreme3(const TArray<float>& In, int32 SizeX, int32 SizeY, bool bHighest)
	{
		TArray<float> Out;
		Out.SetNumUninitialized(In.Num());
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				float Value = In[Y * SizeX + X];
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const float Other = In[Reflect(Y + DY, SizeY) * SizeX + Reflect(X + DX, SizeX)];
						Value = bHighest ? FMath::Max(Value, Other) : FMath::Min(Value, Other);
					}
				}
				Out[Y * SizeX + X] = Value;
			}
		}
		return Out;
	}

	/** Each cell's nearest cell where Source is set, within Radius cells (INDEX_NONE when none). */
	TArray<int32> Nearest(const TArray<bool>& Source, int32 SizeX, int32 SizeY, int32 Radius)
	{
		TArray<int32> Out;
		Out.Init(INDEX_NONE, Source.Num());
		ParallelFor(SizeY, [&](int32 Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				if (Source[Y * SizeX + X])
				{
					Out[Y * SizeX + X] = Y * SizeX + X;
					continue;
				}
				int32 Best = MAX_int32;
				for (int32 SY = FMath::Max(Y - Radius, 0); SY <= FMath::Min(Y + Radius, SizeY - 1); ++SY)
				{
					for (int32 SX = FMath::Max(X - Radius, 0); SX <= FMath::Min(X + Radius, SizeX - 1); ++SX)
					{
						const int32 Distance = FMath::Square(SX - X) + FMath::Square(SY - Y);
						if (Source[SY * SizeX + SX] && Distance < Best)
						{
							Best = Distance;
							Out[Y * SizeX + X] = SY * SizeX + SX;
						}
					}
				}
			}
		});
		return Out;
	}

	/** The cells of Mask's 4-connected pieces under MinCells. */
	TArray<bool> SmallPieces(const TArray<bool>& Mask, int32 SizeX, int32 SizeY, int32 MinCells)
	{
		TArray<bool> Out, Seen;
		Out.Init(false, Mask.Num());
		Seen.Init(false, Mask.Num());
		TArray<int32> Piece;
		for (int32 Start = 0; Start < Mask.Num(); ++Start)
		{
			if (!Mask[Start] || Seen[Start])
			{
				continue;
			}
			Piece.Reset();
			Piece.Add(Start);
			Seen[Start] = true;
			for (int32 Next = 0; Next < Piece.Num(); ++Next)
			{
				const int32 X = Piece[Next] % SizeX;
				const int32 Y = Piece[Next] / SizeX;
				for (const FIntPoint& Step : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
				{
					const int32 NX = X + Step.X;
					const int32 NY = Y + Step.Y;
					const int32 N = NY * SizeX + NX;
					if (NX >= 0 && NY >= 0 && NX < SizeX && NY < SizeY && Mask[N] && !Seen[N])
					{
						Seen[N] = true;
						Piece.Add(N);
					}
				}
			}
			for (const int32 Index : Piece)
			{
				Out[Index] = Piece.Num() < MinCells;
			}
		}
		return Out;
	}

	/** The most common of Values (the lowest of equals); 0 when empty. */
	int32 Mode(const TArray<int32>& Values)
	{
		TMap<int32, int32> Counts;
		for (const int32 Value : Values)
		{
			++Counts.FindOrAdd(Value);
		}
		int32 Best = 0;
		int32 BestCount = 0;
		for (const TPair<int32, int32>& Count : Counts)
		{
			if (Count.Value > BestCount || (Count.Value == BestCount && Count.Key < Best))
			{
				Best = Count.Key;
				BestCount = Count.Value;
			}
		}
		return Best;
	}

	float Median(TArray<float> Values)
	{
		if (Values.IsEmpty())
		{
			return 0.f;
		}
		Values.Sort();
		const int32 Half = Values.Num() / 2;
		return Values.Num() % 2 ? Values[Half] : 0.5f * (Values[Half - 1] + Values[Half]);
	}
}

/** LBA1's scenes of a district in one frame (its first scene's), seen from above: per cell (z * SizeX + x), 0.64 m. */
struct LBAPlanetDistricts::FDistricts::FDistrict
{
	FString Island;
	FString Name;
	TArray<int32> Scenes;
	int32 SizeX = 0;
	int32 SizeZ = 0;
	/** The frame's cell (x, z) at cell 0, 0. */
	FIntPoint Low = FIntPoint::ZeroValue;
	TArray<bool> Land;
	TArray<bool> Water;
	/** The ground over the sea (cm), under its bricks; the streets' top; and the top brick where that is the ground, else the nearest ground's. */
	TArray<float> Ground;
	TArray<float> Streets;
	TArray<int32> GroundBricks;
	/** Its scenes and where their frames are in its frame (units), and per cell the scene drawn there (INDEX_NONE: none). */
	TArray<TPair<int32, FIntVector>> Offsets;
	TArray<int32> SceneOf;
	/** Where LBA1's ferry docks (frame cm, x and z), in the ferry ports. */
	TArray<FVector2D> Docks;
	/** The scenes' sea level (cells in the frame), when it has water. */
	bool bSea = false;
	int32 SeaY = 0;
	/** Where the markers' mean goes: the scenes' centers' mean, cm from cell 0, 0. */
	FVector2D Pivot = FVector2D::ZeroVector;
	/** The markers' mean on the map (cm), its east and north there. */
	FVector2D Anchor = FVector2D::ZeroVector;
	FVector2D East = FVector2D::UnitX();
	FVector2D North = FVector2D::UnitY();
	double Area = 0.0;

	/** The district's x and z on the map, turned by Turn (degrees) from east and north. */
	FVector2D AxisX(double Turn) const
	{
		const double T = FMath::DegreesToRadians(Turn);
		return East * FMath::Cos(T) + North * FMath::Sin(T);
	}
	FVector2D AxisZ(double Turn) const
	{
		const double T = FMath::DegreesToRadians(Turn);
		return North * FMath::Cos(T) - East * FMath::Sin(T);
	}

	/** The cell under a map point (cm), the district's pivot at At turned by Turn; INDEX_NONE off it. */
	int32 CellAt(const FVector2D& Map, double Turn, const FVector2D& At) const
	{
		const FVector2D Local = Map - At;
		const int32 X = FMath::FloorToInt((FVector2D::DotProduct(Local, AxisX(Turn)) + Pivot.X) / LBAPlanetDistrictsPrivate::CellCm);
		const int32 Z = FMath::FloorToInt((FVector2D::DotProduct(Local, AxisZ(Turn)) + Pivot.Y) / LBAPlanetDistrictsPrivate::CellCm);
		return X >= 0 && Z >= 0 && X < SizeX && Z < SizeZ ? Z * SizeX + X : INDEX_NONE;
	}

	bool Build(const LBA1::FData& Data, const TArray<TPair<int32, FIntVector>>& InOffsets, const TArray<FVector2D>& Markers, double Radius);
};

bool LBAPlanetDistricts::FDistricts::FDistrict::Build(const LBA1::FData& Data, const TArray<TPair<int32, FIntVector>>& InOffsets, const TArray<FVector2D>& Markers, double Radius)
{
	using namespace LBAPlanetDistrictsPrivate;
	constexpr int32 Empty = -999;
	Offsets = InOffsets;

	// The scenes laid in the frame: later ones draw over earlier ones only where they are higher.
	FIntPoint High(MIN_int32);
	Low = FIntPoint(MAX_int32);
	for (const TPair<int32, FIntVector>& Offset : Offsets)
	{
		const FIntPoint Cell(FMath::RoundToInt(Offset.Value.X / double(LBA1::CellSize)), FMath::RoundToInt(Offset.Value.Z / double(LBA1::CellSize)));
		Low = Low.ComponentMin(Cell);
		High = High.ComponentMax(Cell + FIntPoint(LBA1::SceneCells));
	}
	SizeX = High.X - Low.X;
	SizeZ = High.Y - Low.Y;
	const int32 Cells = SizeX * SizeZ;
	TArray<int32> Heights, Bottoms, Tops, TopHeights;
	TopHeights.Init(Empty, Cells);
	Heights.Init(Empty, Cells);
	Bottoms.Init(Empty, Cells);
	SceneOf.Init(INDEX_NONE, Cells);
	Tops.Init(0, Cells);
	Land.Init(false, Cells);
	FVector2D Centers = FVector2D::ZeroVector;
	for (const TPair<int32, FIntVector>& Offset : Offsets)
	{
		LBA1::FPlan Plan;
		FLBAGrid Grid;
		FLBALibrary Library;
		if (!Data.LoadPlan(Offset.Key, Plan) || !Data.LoadGrid(Offset.Key, Grid) || !Data.LoadLibrary(Offset.Key, Library))
		{
			return false;
		}
		const FIntPoint Cell = FIntPoint(FMath::RoundToInt(Offset.Value.X / double(LBA1::CellSize)), FMath::RoundToInt(Offset.Value.Z / double(LBA1::CellSize))) - Low;
		const int32 Up = FMath::RoundToInt(Offset.Value.Y / double(LBA1::CellHeight));
		for (int32 Z = 0; Z < LBA1::SceneCells; ++Z)
		{
			for (int32 X = 0; X < LBA1::SceneCells; ++X)
			{
				const int32 Column = Z * LBA1::SceneCells + X;
				if (Plan.Heights[Column] < 0)
				{
					continue;
				}
				const int32 Index = (Cell.Y + Z) * SizeX + Cell.X + X;
				const int32 Height = Plan.Heights[Column] + Up;
				if (Height > Heights[Index])
				{
					Heights[Index] = Height;
					// The top brick that draws anything (over a blank one, only collision, what's under shows).
					Tops[Index] = 0;
					TopHeights[Index] = Height;
					for (int32 Y = Plan.Heights[Column]; Y >= 0; --Y)
					{
						const FLBALayoutBlock* TopBlock = LBA1::GetBlock(Library, Grid.Get(X, Y, Z));
						if (TopBlock && TopBlock->Brick != 0 && !Data.IsBlankBrick(TopBlock->Brick))
						{
							Tops[Index] = TopBlock->Brick;
							TopHeights[Index] = Y + Up;
							break;
						}
					}
					Land[Index] = !Plan.Water[Column];
					SceneOf[Index] = Offset.Key;
					for (int32 Y = 0; Y <= Plan.Heights[Column]; ++Y)
					{
						const FLBALayoutBlock* Block = LBA1::GetBlock(Library, Grid.Get(X, Y, Z));
						if (Block && Block->Brick != 0)
						{
							Bottoms[Index] = Y + Up;
							break;
						}
					}
				}
			}
		}
		Centers += FVector2D(Offset.Value.X, Offset.Value.Z) + FVector2D(LBA1::SceneCells * LBA1::CellSize / 2);
	}
	Pivot = (Centers / Offsets.Num() - FVector2D(Low) * LBA1::CellSize) / 8.0;
	Water.SetNumUninitialized(Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		Water[Index] = Heights[Index] > Empty && !Land[Index];
	}
	// Water in pieces under Pond (fountains, gutters) and holes without bricks as small are ground, not sea.
	TArray<bool> Holes;
	Holes.SetNumUninitialized(Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		Holes[Index] = Heights[Index] == Empty;
	}
	const int32 PondCells = FMath::CeilToInt(Pond / (CellCm * CellCm));
	TArray<bool> Ponds = SmallPieces(Water, SizeX, SizeZ, PondCells);
	const TArray<bool> SmallHoles = SmallPieces(Holes, SizeX, SizeZ, PondCells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		Ponds[Index] = Ponds[Index] || SmallHoles[Index];
	}
	TArray<int32> AllWater;
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		Water[Index] = Water[Index] && !Ponds[Index];
		Land[Index] = Land[Index] || Ponds[Index];
		if (Water[Index])
		{
			AllWater.Add(Heights[Index]);
		}
		Area += Land[Index] ? CellCm * CellCm : 0.0;
	}

	// The ground: the streets' level among the top bricks (houses and trees stand above it).
	TArray<float> Top;
	Top.SetNumUninitialized(Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		Top[Index] = Heights[Index] > Empty ? float(Heights[Index]) : 999.f;
	}
	TArray<float> LowTops = RankFilter(Top, SizeX, SizeZ, GroundWindow, int32(GroundWindow * GroundWindow * GroundShare));
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		LowTops[Index] = FMath::Min(LowTops[Index], Top[Index]);
	}
	TArray<float> GroundY = Extreme3(Extreme3(LowTops, SizeX, SizeZ, true), SizeX, SizeZ, false);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		GroundY[Index] = FMath::Min(GroundY[Index], Top[Index]);
	}

	// The sea: the lowest of the scenes' own (each scene's most common water height), else the most common. LBA1's
	// scenes keep theirs at their own (Principal's harbour at 0, the scenes around it at 3 cells in one frame), but walk
	// into each other in this frame: the bricks keep it, nothing goes under the sea, the higher quays reach down to it.
	bSea = !AllWater.IsEmpty();
	SeaY = Mode(AllWater);
	TMap<int32, int32> SceneSeas;
	for (const TPair<int32, FIntVector>& Offset : Offsets)
	{
		TArray<int32> Own;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			if (SceneOf[Index] == Offset.Key && Water[Index])
			{
				Own.Add(Heights[Index]);
			}
		}
		if (Own.Num() > MinSeaCells)
		{
			SceneSeas.Add(Offset.Key, Mode(Own));
		}
	}
	for (const TPair<int32, int32>& Scene : SceneSeas)
	{
		SeaY = FMath::Min(SeaY, Scene.Value);
	}
	for (const TPair<int32, int32>& Scene : SceneSeas)
	{
		if (Scene.Value != SeaY)
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA1 district %s: scene %d's sea is %+d cells over the district's"), *Name, Scene.Key, Scene.Value - SeaY);
		}
	}
	// The ground under the bricks: their lowest, or the streets' where that is lower (under a roof or a bridge), the
	// lowest of the cells around (5 x 5, 1.3 m: the stub's 1 m triangles between a terrace and the street below it stay
	// under the street).
	TArray<float> Under;
	Under.SetNumUninitialized(Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		Under[Index] = Bottoms[Index] > Empty ? FMath::Min(GroundY[Index], float(Bottoms[Index])) : GroundY[Index];
	}
	Under = Extreme3(Extreme3(Under, SizeX, SizeZ, false), SizeX, SizeZ, false);
	Ground.SetNumUninitialized(Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		Ground[Index] = float((Under[Index] - SeaY) * StepCm - Sink);
	}

	// Its kinds of ground: the top brick's where that is the ground, else the nearest ground's (a pond's ground too).
	TArray<bool> OnGround;
	OnGround.SetNumUninitialized(Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		OnGround[Index] = Land[Index] && !Ponds[Index] && Heights[Index] - GroundY[Index] <= StandsOn;
	}
	const TArray<int32> NearestGround = Nearest(OnGround, SizeX, SizeZ, ColorReach);
	GroundBricks.SetNumUninitialized(Cells);
	Streets.SetNumUninitialized(Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		const int32 From = NearestGround[Index] != INDEX_NONE ? NearestGround[Index] : Index;
		GroundBricks[Index] = Tops[From];
		Ground[Index] = Ponds[Index] ? Ground[From] : Ground[Index];
		// A cell's top is where a brick's top face is: one cell over its height.
		Streets[Index] = float(((TopHeights[From] > Empty ? float(TopHeights[From]) : GroundY[Index]) + 1 - SeaY) * StepCm);
	}

	// Where: the mean of its scenes' markers (longitudes averaged round the circle); east and north there.
	double Latitude = 0.0;
	FVector2D Around = FVector2D::ZeroVector;
	for (const TPair<int32, FIntVector>& Offset : Offsets)
	{
		const FVector2D Marker = Markers.IsValidIndex(Offset.Key) ? Markers[Offset.Key] : FVector2D::ZeroVector;
		Latitude += Marker.X / Offsets.Num();
		Around += FVector2D(FMath::Cos(FMath::DegreesToRadians(Marker.Y)), FMath::Sin(FMath::DegreesToRadians(Marker.Y)));
	}
	FLBAPlanetIsland Place;
	Place.Latitude = Latitude;
	Place.Longitude = FMath::RadiansToDegrees(FMath::Atan2(Around.Y, Around.X));
	Anchor = Place.GetMapLocation(Radius);
	const double Lon = FMath::DegreesToRadians(Place.Longitude);
	East = FVector2D(-FMath::Sin(Lon), FMath::Cos(Lon));
	North = FVector2D(-FMath::Cos(Lon), -FMath::Sin(Lon));
	return true;
}

LBAPlanetDistricts::FDistricts::FDistricts() = default;
LBAPlanetDistricts::FDistricts::~FDistricts() = default;

bool LBAPlanetDistricts::FDistricts::Load(const FString& InDirectory, double Radius)
{
	using namespace LBAPlanetDistrictsPrivate;
	Districts.Reset();
	Data = MakeUnique<LBA1::FData>();
	if (!Data->Open(InDirectory))
	{
		return false;
	}
	Directory = Data->GetDirectory();
	const TArray<FVector2D> Markers = Data->LoadMarkers();

	// Per island, its scenes grouped by edge exits, each group in its first scene's frame.
	TMap<int32, LBA1::FScene> Scenes;
	for (const FOutdoor& Entry : Outdoor)
	{
		LBA1::FScene& Scene = Scenes.Add(Entry.Scene);
		if (!Data->LoadScene(Entry.Scene, Scene))
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA1 scene %d (%s) can't be read from %s"), Entry.Scene, Entry.Name, *Directory);
			return false;
		}
	}
	TArray<int32> Left;
	for (const FOutdoor& Entry : Outdoor)
	{
		Left.Add(Entry.Scene);
	}
	auto IslandOf = [](int32 Scene) { for (const FOutdoor& Entry : Outdoor) { if (Entry.Scene == Scene) { return FString(Entry.Island); } } return FString(); };
	auto NameOf = [](int32 Scene) { for (const FOutdoor& Entry : Outdoor) { if (Entry.Scene == Scene) { return FString(Entry.Name); } } return FString(); };
	while (!Left.IsEmpty())
	{
		const int32 First = Left[0];
		Left.RemoveAt(0);
		TArray<TPair<int32, FIntVector>> Offsets = { { First, FIntVector::ZeroValue } };
		for (int32 Next = 0; Next < Offsets.Num(); ++Next)
		{
			const TPair<int32, FIntVector> From = Offsets[Next];
			for (const LBA1::FZone& Zone : Scenes[From.Key].Zones)
			{
				const FIntVector Move = Zone.Min - Zone.Info;
				if (Zone.Type == LBA1::ZoneCube && Left.Contains(Zone.Number) && IslandOf(Zone.Number) == IslandOf(First) && IsEdgeExit(Move))
				{
					Offsets.Add({ Zone.Number, From.Value + Move });
					Left.Remove(Zone.Number);
				}
			}
		}
		TUniquePtr<FDistrict> District = MakeUnique<FDistrict>();
		District->Island = IslandOf(First);
		District->Name = NameOf(First);
		for (const TPair<int32, FIntVector>& Offset : Offsets)
		{
			District->Scenes.Add(Offset.Key);
			for (const LBA1::FActor& Actor : Scenes[Offset.Key].Actors)
			{
				if (Actor.File3D == LBA1::FerryFile3D)
				{
					District->Docks.Add(FVector2D(Offset.Value.X + Actor.Position.X, Offset.Value.Z + Actor.Position.Z) / 8.0);
				}
			}
		}
		if (!District->Build(*Data, Offsets, Markers, Radius))
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA1 district %s: its scenes' grids can't be read from %s"), *District->Name, *Directory);
			return false;
		}
		Districts.Add(MoveTemp(District));
	}
	return true;
}

const LBAPlanetDistricts::FGroundShares* LBAPlanetDistricts::FDistricts::GetGroundShares(int32 Brick) const
{
	using namespace LBAPlanetDistrictsPrivate;
	if (Brick == 0)
	{
		return nullptr;
	}
	if (const TOptional<FGroundShares>* Known = GroundBricks.Find(Brick))
	{
		return Known->GetPtrOrNull();
	}
	TOptional<FGroundShares>& Result = GroundBricks.Add(Brick);
	FLBABrickSprite Sprite;
	if (!Data->LoadBrick(Brick, Sprite))
	{
		return nullptr;
	}
	int32 InDiamond = 0, Covered = 0, Outside = 0, Solid = 0;
	int32 Ramps[16] = {};
	for (int32 Y = 0; Y < FLBABrickSprite::Height; ++Y)
	{
		for (int32 X = 0; X < FLBABrickSprite::Width; ++X)
		{
			const uint8 Pixel = Sprite.Pixels[Y * FLBABrickSprite::Width + X];
			const bool bDiamond = FMath::Abs(X + 0.5f - 24.f) / 24.f + FMath::Abs(Y + 0.5f - 12.f) / 12.f <= 1.f;
			InDiamond += bDiamond ? 1 : 0;
			Covered += bDiamond && Pixel != 0 ? 1 : 0;
			Outside += !bDiamond && Pixel != 0 ? 1 : 0;
			Solid += Pixel != 0 ? 1 : 0;
			Ramps[Pixel / 16] += Pixel != 0 ? 1 : 0;
		}
	}
	if (float(Covered) < FlatCoverage * InDiamond || Outside > FlatOutside)
	{
		return nullptr;
	}
	FGroundShares Shares;
	if (Brick >= DarkFirst && Brick <= DarkLast)
	{
		Shares.Dark = 1.f;
		Result = Shares;
		return Result.GetPtrOrNull();
	}
	int32 Earth = 0;
	for (const int32 Ramp : EarthRamps)
	{
		Earth += Ramps[Ramp];
	}
	const float Total = float(Solid);
	const int32 Grass = Ramps[GrassRamp] + Ramps[GrassShadowRamp];
	const bool bAllEarth = Earth >= MinNatural * Total;
	const bool bMix = Earth + Grass >= MinNatural * Total && Ramps[GrassRamp] >= MinGrass * Total;
	if (!bAllEarth && !bMix)
	{
		return nullptr;
	}
	Shares.Grass = bAllEarth ? 0.f : Grass / float(Grass + Earth);
	Shares.Earth = 1.f - Shares.Grass;
	Result = Shares;
	return Result.GetPtrOrNull();
}

bool LBAPlanetDistricts::FDistricts::IsWallBrick(int32 Brick) const
{
	if (Brick == 0)
	{
		return false;
	}
	if (const bool* Known = WallBricks.Find(Brick))
	{
		return *Known;
	}
	bool& Result = WallBricks.Add(Brick, false);
	FLBABrickSprite Sprite;
	if (!Data->LoadBrick(Brick, Sprite))
	{
		return false;
	}
	// The sprite's faces: the top diamond, and under its lower edges the +Z (left) and +X (right) sides, 15 pixels high.
	int32 Pixels[3] = {}, Opaque[3] = {};
	for (int32 Y = 0; Y < FLBABrickSprite::Height; ++Y)
	{
		for (int32 X = 0; X < FLBABrickSprite::Width; ++X)
		{
			const float Edge = 24.f - FMath::Abs(X + 0.5f - 24.f) * 0.5f;
			const int32 Face = Y + 0.5f <= Edge ? 0 : Y + 0.5f < Edge + 15.f ? (X < 24 ? 1 : 2) : INDEX_NONE;
			if (Face != INDEX_NONE)
			{
				++Pixels[Face];
				Opaque[Face] += Sprite.Pixels[Y * FLBABrickSprite::Width + X] != 0 ? 1 : 0;
			}
		}
	}
	constexpr float Full = 0.9f, Empty = 0.1f;
	Result = Opaque[1] >= Full * Pixels[1] && Opaque[2] >= Full * Pixels[2] && Opaque[0] <= Empty * Pixels[0];
	return Result;
}

bool LBAPlanetDistricts::FDistricts::HasIsland(const FString& Island) const
{
	return Districts.ContainsByPredicate([&Island](const TUniquePtr<FDistrict>& District) { return District->Island == Island; });
}

TArray<LBAPlanetDistricts::FPlaced> LBAPlanetDistricts::FDistricts::Lay(const FString& Island, FGround& Ground) const
{
	using namespace LBAPlanetDistrictsPrivate;
	const int32 Cells = Ground.SizeX * Ground.SizeY;
	auto CellToMap = [&](int32 X, int32 Y) { return Ground.Origin + FVector2D(X, Y) * Ground.Cell; };

	// Largest first.
	TArray<const FDistrict*> Here;
	for (const TUniquePtr<FDistrict>& District : Districts)
	{
		if (District->Island == Island)
		{
			Here.Add(District.Get());
		}
	}
	Algo::StableSort(Here, [](const FDistrict* A, const FDistrict* B) { return A->Area > B->Area; });

	// The fit: per turn, the district on the island's cells about its pivot (land +1, water -1), slid over the stub
	// (land +1, sea -1) and the districts placed before (each cell under them costs), near its markers and heading 0.
	TArray<float> Taken;
	Taken.Init(0.f, Cells);
	TArray<FPlaced> Placed;
	TArray<TPair<double, FVector2D>> Places;
	for (const FDistrict* District : Here)
	{
		const int32 Radius = FMath::CeilToInt(FMath::Sqrt(double(FMath::Square(District->SizeX) + FMath::Square(District->SizeZ))) * CellCm / Ground.Cell / 2.0
			+ District->Pivot.Size() / Ground.Cell) + 2;
		const FVector2D AnchorCell = (District->Anchor - Ground.Origin) / Ground.Cell;
		const int32 ReachCells = FMath::CeilToInt(Reach / Ground.Cell);
		const int32 NumTurns = FMath::RoundToInt(2.0 * MaxTurn / TurnStep) + 1;
		struct FBest
		{
			double Score = -UE_BIG_NUMBER;
			double Agreement = 0.0;
			FIntPoint Cell = FIntPoint::ZeroValue;
		};
		TArray<FBest> Bests;
		Bests.SetNum(NumTurns);
		ParallelFor(NumTurns, [&](int32 TurnIndex)
		{
			const double Turn = -MaxTurn + TurnIndex * TurnStep;
			struct FWeight
			{
				int32 DX;
				int32 DY;
				float Weight;
			};
			TArray<FWeight> Kernel;
			double Total = 0.0;
			for (int32 DY = -Radius; DY <= Radius; ++DY)
			{
				for (int32 DX = -Radius; DX <= Radius; ++DX)
				{
					const int32 Cell = District->CellAt(District->Anchor + FVector2D(DX, DY) * Ground.Cell, Turn, District->Anchor);
					const float Weight = Cell == INDEX_NONE ? 0.f : District->Land[Cell] ? 1.f : District->Water[Cell] ? -1.f : 0.f;
					if (Weight != 0.f)
					{
						Kernel.Add({ DX, DY, Weight });
						Total += 1.0;
					}
				}
			}
			FBest& Best = Bests[TurnIndex];
			for (int32 Y = FMath::Max(0, FMath::FloorToInt(AnchorCell.Y) - ReachCells); Y <= FMath::Min(Ground.SizeY - 1, FMath::CeilToInt(AnchorCell.Y) + ReachCells); ++Y)
			{
				for (int32 X = FMath::Max(0, FMath::FloorToInt(AnchorCell.X) - ReachCells); X <= FMath::Min(Ground.SizeX - 1, FMath::CeilToInt(AnchorCell.X) + ReachCells); ++X)
				{
					const double Far = FVector2D::Distance(FVector2D(X, Y), AnchorCell) * Ground.Cell;
					if (Far > Reach)
					{
						continue;
					}
					double Sum = 0.0;
					for (const FWeight& Weight : Kernel)
					{
						const int32 GX = X + Weight.DX;
						const int32 GY = Y + Weight.DY;
						if (GX >= 0 && GY >= 0 && GX < Ground.SizeX && GY < Ground.SizeY)
						{
							const int32 Index = GY * Ground.SizeX + GX;
							Sum += Weight.Weight * (Ground.Land[Index] ? 1.0 : -1.0) - OverlapCost * Taken[Index];
						}
					}
					const double Agreement = Sum / FMath::Max(Total, 1.0);
					const double Score = Agreement - PriorWeight * 0.5 * (FMath::Square(Far / PriorReach) + FMath::Square(Turn / PriorTurn));
					if (Score > Best.Score)
					{
						Best = { Score, Agreement, FIntPoint(X, Y) };
					}
				}
			}
		});
		int32 BestTurn = 0;
		for (int32 TurnIndex = 1; TurnIndex < NumTurns; ++TurnIndex)
		{
			BestTurn = Bests[TurnIndex].Score > Bests[BestTurn].Score ? TurnIndex : BestTurn;
		}
		const double Turn = -MaxTurn + BestTurn * TurnStep;
		const FVector2D At = CellToMap(Bests[BestTurn].Cell.X, Bests[BestTurn].Cell.Y);

		FPlaced& Place = Placed.AddDefaulted_GetRef();
		Place.District = Districts.IndexOfByPredicate([District](const TUniquePtr<FDistrict>& Each) { return Each.Get() == District; });
		Place.Name = District->Name;
		Place.Scenes = District->Scenes;
		Place.Map = At;
		Place.Turn = Turn;
		Place.FromMarkers = FVector2D::Distance(At, District->Anchor);
		Place.Agreement = Bests[BestTurn].Agreement;
		Place.LandArea = District->Area;
		const FVector2D AlongX = District->AxisX(Turn);
		const FQuat Rotation(FVector::UpVector, FMath::Atan2(AlongX.Y, AlongX.X));
		const FVector Pivot(FVector2D(District->Low) * CellCm + District->Pivot, District->bSea ? District->SeaY * StepCm : 0.0);
		Place.FrameToMap = FTransform(Rotation, FVector(At, 0.0) - Rotation.RotateVector(Pivot));
		for (const FVector2D& Dock : District->Docks)
		{
			Place.Docks.Add(FVector2D(Place.FrameToMap.TransformPosition(FVector(Dock, 0.0))));
		}
		Places.Add({ Turn, At });
		int32 OnLand = 0, On = 0, WetOnSea = 0, Wet = 0;
		for (int32 Y = 0; Y < Ground.SizeY; ++Y)
		{
			for (int32 X = 0; X < Ground.SizeX; ++X)
			{
				const int32 Cell = District->CellAt(CellToMap(X, Y), Turn, At);
				const int32 Index = Y * Ground.SizeX + X;
				if (Cell != INDEX_NONE && (District->Land[Cell] || District->Water[Cell]))
				{
					Taken[Index] += 1.f;
					On += District->Land[Cell] ? 1 : 0;
					OnLand += District->Land[Cell] && Ground.Land[Index] ? 1 : 0;
					Wet += District->Water[Cell] ? 1 : 0;
					WetOnSea += District->Water[Cell] && !Ground.Land[Index] ? 1 : 0;
				}
			}
		}
		Place.LandOnLand = double(OnLand) / FMath::Max(On, 1);
		Place.WaterOnSea = double(WetOnSea) / FMath::Max(Wet, 1);
	}

	// The pads: each district's land at its ground (a district without water: its median ground where the stub's is),
	// its water carved; the first laid keeps a cell.
	const TArray<float> Stub = Ground.Heights;
	TArray<float> PadHeights, PadStreets;
	PadHeights.Init(0.f, Cells);
	PadStreets.Init(0.f, Cells);
	Ground.Pad.Init(false, Cells);
	Ground.PadWater.Init(false, Cells);
	Ground.Kinded.Init(false, Cells);
	Ground.Kinds.Init(FGroundShares(), Cells);
	// Whether a pad's cell has natural ground to carry on past its edge (paving and walls don't).
	TArray<bool> Natural;
	Natural.Init(false, Cells);
	for (int32 Which = 0; Which < Here.Num(); ++Which)
	{
		const FDistrict* District = Here[Which];
		const double Turn = Places[Which].Key;
		const FVector2D At = Places[Which].Value;
		TArray<int32> Under;
		Under.Init(INDEX_NONE, Cells);
		TArray<float> Ours, Stubs;
		for (int32 Y = 0; Y < Ground.SizeY; ++Y)
		{
			for (int32 X = 0; X < Ground.SizeX; ++X)
			{
				const int32 Index = Y * Ground.SizeX + X;
				const int32 Cell = District->CellAt(CellToMap(X, Y), Turn, At);
				if (Cell != INDEX_NONE && !Ground.Pad[Index] && District->Land[Cell])
				{
					Under[Index] = Cell;
					Ours.Add(District->Ground[Cell]);
					Stubs.Add(Stub[Index]);
				}
				else if (Cell != INDEX_NONE && !Ground.Pad[Index] && District->Water[Cell])
				{
					Ground.PadWater[Index] = true;
				}
			}
		}
		const float Shift = District->bSea ? 0.f : Median(Stubs) - Median(Ours);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			if (Under[Index] != INDEX_NONE)
			{
				Ground.Pad[Index] = true;
				PadHeights[Index] = District->Ground[Under[Index]] + Shift;
				PadStreets[Index] = District->Streets[Under[Index]] + Shift;
				// Under paving and walls, only seen through their gaps: dark ground.
				const FGroundShares* Shares = GetGroundShares(District->GroundBricks[Under[Index]]);
				Ground.Kinded[Index] = true;
				Ground.Kinds[Index] = Shares ? *Shares : FGroundShares{ 0.f, 0.f, 1.f };
				Natural[Index] = Shares != nullptr;
			}
		}
		Placed[Which].FrameToMap.AddToTranslation(FVector(0.0, 0.0, Shift));
	}

	// The pad's edge: its land next to land off it comes up to the streets' top (the lowest around: its triangles stay under
	// the streets), so a town's ground meets the stub's with no step.
	auto AtEdge = [&](int32 X, int32 Y)
	{
		for (int32 DY = -EdgeCells; DY <= EdgeCells; ++DY)
		{
			for (int32 DX = -EdgeCells; DX <= EdgeCells; ++DX)
			{
				const int32 NX = X + DX;
				const int32 NY = Y + DY;
				if (NX >= 0 && NY >= 0 && NX < Ground.SizeX && NY < Ground.SizeY && !Ground.Pad[NY * Ground.SizeX + NX] && !Ground.PadWater[NY * Ground.SizeX + NX] && Ground.Land[NY * Ground.SizeX + NX])
				{
					return true;
				}
			}
		}
		return false;
	};
	TArray<float> EdgeHeights = PadStreets;
	for (int32 Y = 0; Y < Ground.SizeY; ++Y)
	{
		for (int32 X = 0; X < Ground.SizeX; ++X)
		{
			const int32 Index = Y * Ground.SizeX + X;
			if (!Ground.Pad[Index])
			{
				continue;
			}
			float Lowest = PadStreets[Index];
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DX = -1; DX <= 1; ++DX)
				{
					const int32 NX = X + DX;
					const int32 NY = Y + DY;
					if (NX >= 0 && NY >= 0 && NX < Ground.SizeX && NY < Ground.SizeY && Ground.Pad[NY * Ground.SizeX + NX])
					{
						Lowest = FMath::Min(Lowest, PadStreets[NY * Ground.SizeX + NX]);
					}
				}
			}
			EdgeHeights[Index] = Lowest - float(EdgeUnder);
			if (AtEdge(X, Y))
			{
				PadHeights[Index] = FMath::Max(PadHeights[Index], EdgeHeights[Index]);
			}
		}
	}

	// The stub's ground off a pad eases from the pad's edge to its own, gently, and more so where they differ more.
	const TArray<int32> NearestPad = Nearest(Ground.Pad, Ground.SizeX, Ground.SizeY, FMath::CeilToInt(MaxBlend * (1.0 + EaseWander) / Ground.Cell) + 1);
	TArray<bool> Eased;
	Eased.Init(false, Cells);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		if (Ground.Pad[Index])
		{
			Ground.Land[Index] = true;
			Ground.Heights[Index] = PadHeights[Index];
			continue;
		}
		Ground.Land[Index] = Ground.Land[Index] && !Ground.PadWater[Index];
		const int32 Source = NearestPad[Index];
		if (Ground.Land[Index] && Source != INDEX_NONE)
		{
			const FVector2D At(Index % Ground.SizeX, Index / Ground.SizeX);
			const FVector2D Map = At * Ground.Cell;
			const double Off = FVector2D::Distance(At, FVector2D(Source % Ground.SizeX, Source / Ground.SizeX)) * Ground.Cell;
			const double From = EdgeHeights[Source];
			const double Differ = FMath::Abs(double(Stub[Index]) - From);
			const double Wander = 1.0 + EaseWander * FMath::Clamp(2.0 * double(FMath::PerlinNoise2D(FVector2D(Map / EaseWanderSize))), -1.0, 1.0);
			const double Width = FMath::Clamp(Differ / MaxEaseSlope, Blend, MaxBlend) * Wander;
			if (Off < Width)
			{
				Ground.Heights[Index] = float(FMath::Lerp(From, double(Stub[Index]), FMath::SmoothStep(0.0, 1.0, Off / Width)));
				Eased[Index] = true;
			}
			// A town's natural ground carries on past its edge, unevenly.
			const double KindsOff = Off / (KindsReach * (1.0 + 1.5 * double(FMath::PerlinNoise2D(FVector2D(Map / KindsWanderSize + FVector2D(37.0))))));
			if (Natural[Source] && KindsOff >= 0.0 && KindsOff < 1.0)
			{
				Ground.Kinded[Index] = true;
				Ground.Kinds[Index] = Ground.Kinds[Source];
			}
		}
	}
	// Nearest pads change from cell to cell: two passes of a small blur take the creases out of the eased ground, its first
	// cell off the pad staying where it meets the pad.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const TArray<float> Before = Ground.Heights;
		ParallelFor(Ground.SizeY, [&](int32 Y)
		{
			for (int32 X = 0; X < Ground.SizeX; ++X)
			{
				const int32 Index = Y * Ground.SizeX + X;
				if (!Eased[Index])
				{
					continue;
				}
				float Sum = 0.f;
				float Weights = 0.f;
				bool bByPad = false;
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const int32 NX = X + DX;
						const int32 NY = Y + DY;
						if (NX >= 0 && NY >= 0 && NX < Ground.SizeX && NY < Ground.SizeY && Ground.Land[NY * Ground.SizeX + NX])
						{
							bByPad = bByPad || Ground.Pad[NY * Ground.SizeX + NX];
							Sum += Before[NY * Ground.SizeX + NX];
							Weights += 1.f;
						}
					}
				}
				Ground.Heights[Index] = bByPad || Weights == 0.f ? Before[Index] : Sum / Weights;
			}
		});
	}
	return Placed;
}

FVector LBAPlanetDistricts::FTown::GetOrigin() const
{
	using namespace LBAPlanetDistrictsPrivate;
	return FVector(Low.X * CellCm, Low.Z * CellCm, Low.Y * StepCm);
}

bool LBAPlanetDistricts::FDistricts::MakeTown(const FPlaced& Placed, FTown& Out, const LBAPlanetKit::FTable* Kit) const
{
	using namespace LBAPlanetDistrictsPrivate;
	if (!Districts.IsValidIndex(Placed.District))
	{
		return false;
	}
	const FDistrict& District = *Districts[Placed.District];
	int32 Lowest = District.bSea ? District.SeaY - QuayDepth : MAX_int32;
	int32 Highest = MIN_int32;
	for (const TPair<int32, FIntVector>& Offset : District.Offsets)
	{
		const int32 Up = FMath::RoundToInt(Offset.Value.Y / double(LBA1::CellHeight));
		Lowest = FMath::Min(Lowest, Up);
		Highest = FMath::Max(Highest, Up + LBA::GridSizeY);
	}
	Out.Low = FIntVector(District.Low.X, Lowest, District.Low.Y);
	Out.Size = FIntVector(District.SizeX, Highest - Lowest, District.SizeZ);
	Out.Bricks.Init(0, Out.Size.X * Out.Size.Y * Out.Size.Z);
	Out.Shapes.Init(0, Out.Bricks.Num());
	Out.Scenes.Init(INDEX_NONE, Out.Size.X * Out.Size.Z);
	Out.Pieces.Reset();
	Out.Replaced.Init(false, Out.Bricks.Num());
	Out.WallStyles.Reset();
	Out.WallStyle.Init(INDEX_NONE, Out.Bricks.Num());
	Out.WallRow.Init(0, Out.Bricks.Num());
	// A one-column layout's style: its bricks from the bottom up.
	auto StyleOf = [&Out](const FLBALayout& Layout)
	{
		TArray<int32> Bricks;
		for (const FLBALayoutBlock& Block : Layout.Blocks)
		{
			Bricks.Add(Block.Brick);
		}
		const int32 Found = Out.WallStyles.IndexOfByKey(Bricks);
		return Found != INDEX_NONE ? Found : Out.WallStyles.Add(MoveTemp(Bricks));
	};

	// Each column from the scene drawn over it, but the sea's (its water and the sea floor under it).
	auto IsSea = [&District](int32 X, int32 Z)
	{
		return X >= 0 && Z >= 0 && X < District.SizeX && Z < District.SizeZ && District.Water[Z * District.SizeX + X];
	};
	for (const TPair<int32, FIntVector>& Offset : District.Offsets)
	{
		FLBAGrid Grid;
		FLBALibrary Library;
		if (!Data->LoadGrid(Offset.Key, Grid) || !Data->LoadLibrary(Offset.Key, Library))
		{
			return false;
		}
		const FIntPoint Cell = FIntPoint(FMath::RoundToInt(Offset.Value.X / double(LBA1::CellSize)), FMath::RoundToInt(Offset.Value.Z / double(LBA1::CellSize))) - District.Low;
		const int32 Up = FMath::RoundToInt(Offset.Value.Y / double(LBA1::CellHeight)) - Lowest;
		for (int32 Z = 0; Z < LBA1::SceneCells; ++Z)
		{
			for (int32 X = 0; X < LBA1::SceneCells; ++X)
			{
				const int32 TX = Cell.X + X;
				const int32 TZ = Cell.Y + Z;
				const int32 Column = TZ * Out.Size.X + TX;
				if (District.SceneOf[Column] != Offset.Key || IsSea(TX, TZ))
				{
					continue;
				}
				Out.Scenes[Column] = Offset.Key;
				for (int32 Y = 0; Y < LBA::GridSizeY; ++Y)
				{
					const FLBAGridCell& GridCell = Grid.Get(X, Y, Z);
					if (const FLBALayoutBlock* Block = LBA1::GetBlock(Library, GridCell))
					{
						const int32 Index = Out.Index(TX, Y + Up, TZ);
						Out.Bricks[Index] = Block->Brick;
						Out.Shapes[Index] = Block->Shape;
						const FLBALayout* Layout = Library.Layouts.IsValidIndex(GridCell.Layout) ? &Library.Layouts[GridCell.Layout] : nullptr;
						if (Layout && Layout->SizeX == 1 && Layout->SizeZ == 1 && Layout->SizeY <= MAX_uint8 && IsWallBrick(Block->Brick))
						{
							Out.WallStyle[Index] = StyleOf(*Layout);
							Out.WallRow[Index] = uint8(GridCell.Block);
						}
					}
				}
			}
		}

		// Its kit pieces: whole layouts (block 0 is the low corner) the kit has a model for, all drawn from this scene.
		for (int32 Z = 0; Kit && Z < LBA1::SceneCells; ++Z)
		{
			for (int32 X = 0; X < LBA1::SceneCells; ++X)
			{
				for (int32 Y = 0; Y < LBA::GridSizeY; ++Y)
				{
					const FLBAGridCell& Anchor = Grid.Get(X, Y, Z);
					if (Anchor.Block != 0 || !Library.Layouts.IsValidIndex(Anchor.Layout))
					{
						continue;
					}
					const FLBALayout& Layout = Library.Layouts[Anchor.Layout];
					const int32 Model = Kit->Find(Layout);
					if (Model == INDEX_NONE)
					{
						continue;
					}
					bool bWhole = X + Layout.SizeX <= LBA1::SceneCells && Z + Layout.SizeZ <= LBA1::SceneCells && Y + Layout.SizeY <= LBA::GridSizeY;
					for (int32 Index = 0; bWhole && Index < Layout.Blocks.Num(); ++Index)
					{
						const FIntVector At = FIntVector(X, Y, Z) + Layout.GetBlockCoords(Index);
						const FLBAGridCell& Here = Grid.Get(At.X, At.Y, At.Z);
						const int32 Column = (Cell.Y + At.Z) * Out.Size.X + Cell.X + At.X;
						bWhole = (Layout.Blocks[Index].Brick == 0 || (Here.Layout == Anchor.Layout && Here.Block == Index))
							&& District.SceneOf[Column] == Offset.Key && !IsSea(Cell.X + At.X, Cell.Y + At.Z);
					}
					if (!bWhole)
					{
						continue;
					}
					const FIntVector Low(Cell.X + X, Y + Up, Cell.Y + Z);
					Out.Pieces.Add({ Model, Offset.Key, Low, FIntVector(Layout.SizeX, Layout.SizeY, Layout.SizeZ) });
					for (int32 Index = 0; Index < Layout.Blocks.Num(); ++Index)
					{
						if (Layout.Blocks[Index].Brick != 0)
						{
							const FIntVector At = Low + Layout.GetBlockCoords(Index);
							Out.Replaced[Out.Index(At.X, At.Y, At.Z)] = true;
						}
					}
				}
			}
		}
	}

	// Quays: a column by the sea goes down under it, its lowest brick repeated.
	if (District.bSea)
	{
		const int32 Floor = District.SeaY - QuayDepth - Lowest;
		for (int32 Z = 0; Z < Out.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Out.Size.X; ++X)
			{
				if (Out.Scenes[Z * Out.Size.X + X] == INDEX_NONE || !(IsSea(X + 1, Z) || IsSea(X - 1, Z) || IsSea(X, Z + 1) || IsSea(X, Z - 1)))
				{
					continue;
				}
				int32 Bottom = 0;
				while (Bottom < Out.Size.Y && Out.Bricks[Out.Index(X, Bottom, Z)] == 0)
				{
					++Bottom;
				}
				for (int32 Y = Floor; Y < Bottom && Bottom < Out.Size.Y; ++Y)
				{
					const int32 Index = Out.Index(X, Y, Z);
					const int32 BottomIndex = Out.Index(X, Bottom, Z);
					Out.Bricks[Index] = Out.Bricks[BottomIndex];
					Out.Shapes[Index] = 1;
					// A wall goes on down as its style repeats.
					const int32 Style = Out.WallStyle[BottomIndex];
					if (Style != INDEX_NONE)
					{
						const TArray<int32>& Bricks = Out.WallStyles[Style];
						const int32 Row = ((int32(Out.WallRow[BottomIndex]) - (Bottom - Y)) % Bricks.Num() + Bricks.Num()) % Bricks.Num();
						if (IsWallBrick(Bricks[Row]))
						{
							Out.Bricks[Index] = Bricks[Row];
							Out.WallStyle[Index] = Style;
							Out.WallRow[Index] = uint8(Row);
						}
					}
				}
			}
		}
	}
	return true;
}
