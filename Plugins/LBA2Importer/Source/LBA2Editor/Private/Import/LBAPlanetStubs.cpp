#include "Import/LBAPlanetStubs.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Note.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Formats/LBAHqr.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBAPlanetDistricts.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"

namespace LBAPlanetStubsPrivate
{
	/** HOLOMAP.HQR's texture of Twinsun (HQR_TWINSUN_HMG). */
	constexpr int32 TextureEntry = 2;
	constexpr int32 TextureSize = 256;

	/** The survey of the hemisphere for its islands: cells (cm) out to this latitude (where the belt's flank starts). */
	constexpr double SurveyCell = 200.0;
	constexpr double LowestLatitude = 14.0;
	/** Smaller specks of painted land (cm2) are dither, not islands. */
	constexpr double MinArea = 500.0 * 10000.0;

	/** An island's mesh: cells (cm), sea around its painted land. */
	constexpr double Cell = 100.0;
	constexpr double Margin = 2000.0;

	// The ground's grammar, measured on LBA2's own islands (Tools/terrain_stats.py, prototyped in stub_grammar.py):
	// a low shore, a few flat benches (20-40 % of the land under 6 degrees) at levels a few meters apart, cliffs
	// between them and along most coasts, beaches in the bays. Lengths in cm.
	/** The lowest land, and the shore's ground (Citadel's shore 0.86 m, the desert's 0.21 m). */
	constexpr double ShoreMin = 15.0;
	constexpr double Shore = 40.0;
	/** The first bench over the coast (Citadel 3.1 m, Otringal 1.9 and 3.1 m, Knartas 1.9 m). */
	constexpr double FirstBench = 280.0;
	/** The wanted height leaves the shore within this, so most coasts are cliffs (Citadel 60 %, Otringal 93 %). */
	constexpr double CoastRise = 300.0;
	/** Benches above the first, this far apart. */
	constexpr double StepMin = 250.0;
	constexpr double StepMax = 600.0;
	/** The highest point grows with the land's area (Citadel 0.15 sqrt(m2), Otringal 0.28, the desert 0.06). */
	constexpr double PeakPerRoot = 0.15;
	constexpr double MinPeak = 400.0;
	constexpr double MaxPeak = 2500.0;
	/** A bench keeps this far back from the one below; the first one this far from a cliff coast, BeachSetback from a beach. */
	constexpr double BenchSetback = 150.0;
	constexpr double CoastSetback = 100.0;
	constexpr double BeachSetback = 1000.0;
	/** Smaller benches (cm2) are dropped. */
	constexpr double MinBenchArea = 40.0 * 10000.0;
	/** A bench's edge climbs over a cliff's width, or a ramp's on RampShare of them (LBA2: ~25 % at 10-30 degrees). */
	constexpr double CliffWidth = 140.0;
	constexpr double RampWidth = 900.0;
	constexpr double RampShare = 0.25;
	/** A bench's edge is smoothed over this many cells (gaussian), so it doesn't follow the cells in steps. */
	constexpr double EdgeSmoothing = 1.5;
	/** A coast is a bay when this share of the land around it (gaussian of BaySigma) is land; bays take beaches. */
	constexpr double BaySigma = 800.0;
	constexpr float BayMin = 0.47f;
	/** Rolling ground over the benches (the noise's peak; typical slopes 5-10 degrees). */
	constexpr double Roll = 200.0;
	/** Noise above this leaves bare earth among the grass (Citadel's benches: 40 % earth, 30 % grass). */
	constexpr float EarthPatches = 0.3f;
	/** Ground steeper than this (degrees) is bare rock. */
	constexpr double RockSlope = 32.0;
	/** Under the sea: the shelf falls this steeply from the coast, down to the skirt's depth. */
	constexpr double ShelfSlope = 0.6;
	constexpr double SkirtDepth = 800.0;
	/** A district's water (LBAPlanetDistricts) is carved at least this deep. */
	constexpr double PadSeaFloor = 120.0;

	/** The ground's colors: LBA2's own (sRGB, median of each kind on Citadel, Otringal and the desert). */
	const FVector3f RockColor(136.f, 126.f, 128.f);
	const FVector3f SandColor(220.f, 192.f, 120.f);
	const FVector3f GrassColor(120.f, 140.f, 96.f);
	const FVector3f EarthColor(124.f, 86.f, 58.f);
	const FVector3f PaleColor(206.f, 202.f, 192.f);
	const FVector3f ShelfColor(150.f, 190.f, 170.f);

	/** LBA1's holomap arrows (degrees on this planet's frame), for the islands' names. */
	struct FNamedPlace
	{
		const TCHAR* Name;
		double Latitude;
		double Longitude;
	};
	constexpr FNamedPlace NamedPlaces[] = { { TEXT("Principal"), 43.3, 131.0 }, { TEXT("Proxima"), 39.0, -101.6 }, { TEXT("Rebellion"), 25.1, -21.1 } };
	/** An island takes the name of the nearest arrow this close to its land's center (cm). */
	constexpr double NameReach = 12000.0;

	/** A grid of values, X along the map's X. */
	struct FGrid
	{
		int32 SizeX = 0;
		int32 SizeY = 0;
		TArray<float> Values;

		void Init(int32 InSizeX, int32 InSizeY, float Value = 0.f)
		{
			SizeX = InSizeX;
			SizeY = InSizeY;
			Values.Init(Value, SizeX * SizeY);
		}
		float& operator()(int32 X, int32 Y) { return Values[Y * SizeX + X]; }
		float operator()(int32 X, int32 Y) const { return Values[Y * SizeX + X]; }
		float At(int32 X, int32 Y) const { return Values[FMath::Clamp(Y, 0, SizeY - 1) * SizeX + FMath::Clamp(X, 0, SizeX - 1)]; }
	};

	/** Gaussian blur (Sigma in cells), edges clamped. */
	void Blur(FGrid& Grid, double Sigma)
	{
		const int32 Reach = FMath::CeilToInt(3.0 * Sigma);
		TArray<float> Weights;
		float Total = 0.f;
		for (int32 Offset = -Reach; Offset <= Reach; ++Offset)
		{
			Weights.Add(float(FMath::Exp(-0.5 * FMath::Square(Offset / Sigma))));
			Total += Weights.Last();
		}
		for (float& Weight : Weights)
		{
			Weight /= Total;
		}
		FGrid Pass = Grid;
		for (int32 Y = 0; Y < Grid.SizeY; ++Y)
		{
			for (int32 X = 0; X < Grid.SizeX; ++X)
			{
				float Sum = 0.f;
				for (int32 Offset = -Reach; Offset <= Reach; ++Offset)
				{
					Sum += Weights[Offset + Reach] * Grid.At(X + Offset, Y);
				}
				Pass(X, Y) = Sum;
			}
		}
		for (int32 Y = 0; Y < Grid.SizeY; ++Y)
		{
			for (int32 X = 0; X < Grid.SizeX; ++X)
			{
				float Sum = 0.f;
				for (int32 Offset = -Reach; Offset <= Reach; ++Offset)
				{
					Sum += Weights[Offset + Reach] * Pass.At(X, Y + Offset);
				}
				Grid(X, Y) = Sum;
			}
		}
	}

	/** Squared distance transform along one line (Felzenszwalb and Huttenlocher): F in, D out. */
	void DistanceLine(const TArray<double>& F, TArray<double>& D)
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
	FGrid DistanceOutOf(const TArray<bool>& Inside, int32 SizeX, int32 SizeY)
	{
		constexpr double Far = 1e20;
		FGrid Out;
		Out.Init(SizeX, SizeY);
		TArray<double> F, D;
		TArray<double> Rows;
		Rows.SetNumUninitialized(SizeX * SizeY);
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			F.SetNumUninitialized(SizeX);
			for (int32 X = 0; X < SizeX; ++X)
			{
				F[X] = Inside[Y * SizeX + X] ? Far : 0.0;
			}
			DistanceLine(F, D);
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
			DistanceLine(F, D);
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				Out(X, Y) = float(FMath::Sqrt(D[Y]));
			}
		}
		return Out;
	}

	/** The holomap's texture as colors (sRGB, 0 to 255) and painted land (0 or 1). */
	struct FPainting
	{
		TArray<FVector3f> Colors;
		TArray<float> Land;
		double Radius = 0.0;

		/** The texture at a point of the map (cm), bilinearly; texels along U wrap around. */
		template <typename T>
		T Sample(const TArray<T>& Texels, const FVector2D& Map) const
		{
			const double Latitude = 90.0 - FMath::RadiansToDegrees(Map.Size() / Radius);
			const double Longitude = FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(Map.Y, Map.X)) + 360.0, 360.0);
			const double V = (Latitude + 90.0) / 180.0 * TextureSize - 0.5;
			const double U = Longitude / 360.0 * TextureSize - 0.5;
			const int32 V0 = FMath::FloorToInt(V);
			const int32 U0 = FMath::FloorToInt(U);
			const float FV = float(V - V0);
			const float FU = float(U - U0);
			auto At = [&](int32 Row, int32 Column) { return Texels[FMath::Clamp(Row, 0, TextureSize - 1) * TextureSize + ((Column % TextureSize) + TextureSize) % TextureSize]; };
			return (At(V0, U0) * (1.f - FU) + At(V0, U0 + 1) * FU) * (1.f - FV) + (At(V0 + 1, U0) * (1.f - FU) + At(V0 + 1, U0 + 1) * FU) * FV;
		}
	};

	/** Painted land: anything warmer or greener than the sea's dark blues, or bright. */
	bool IsLand(const FVector3f& C)
	{
		return C.X + C.Y > 1.25f * C.Z + 30.f || C.X + C.Y + C.Z > 330.f;
	}

	/** What a painted color says about the ground: its kinds (shares summing to 1 with the rest) and how high it is. */
	struct FPaintedKinds
	{
		float Sand = 0.f;
		float Green = 0.f;
		float Brown = 0.f;
		float Pale = 0.f;
		/** 0 to 1: sand low, green lowland, brown ridges, pale stone highest. */
		float Height = 0.f;
	};

	/**
	 * The painting's kinds of ground, read from the hue: the holomap paints its globe dark (its own shading), so a color
	 * is judged against its own brightness. Olive green = lowland, brown = ridges, bright and warm = sand, bright and
	 * grey = bare stone up high.
	 */
	FPaintedKinds ReadPainting(const FVector3f& Color)
	{
		const float Lum = (Color.X + Color.Y + Color.Z) / 3.f;
		const FVector3f N = Color / FMath::Max(Color.GetMax(), 1.f);
		const float Bright = FMath::Clamp((Lum - 110.f) / 40.f, 0.f, 1.f);
		const float Saturation = 1.f - N.GetMin();
		FPaintedKinds Kinds;
		Kinds.Pale = Bright * FMath::Clamp((0.3f - Saturation) / 0.15f, 0.f, 1.f);
		Kinds.Sand = Bright * FMath::Clamp((N.X - N.Z - 0.2f) / 0.15f, 0.f, 1.f) * (1.f - Kinds.Pale);
		Kinds.Green = FMath::Clamp((N.Y - N.X + 0.05f) / 0.1f, 0.f, 1.f) * (1.f - Bright);
		Kinds.Brown = FMath::Clamp((N.X - N.Y) / 0.12f, 0.f, 1.f) * (1.f - Bright);
		const float Rest = FMath::Clamp(1.f - Kinds.Sand - Kinds.Green - Kinds.Brown - Kinds.Pale, 0.f, 1.f);
		const float Total = FMath::Max(Kinds.Sand + Kinds.Green + Kinds.Brown + Kinds.Pale + Rest, 1e-3f);
		Kinds.Sand /= Total;
		Kinds.Green /= Total;
		Kinds.Brown /= Total;
		Kinds.Pale /= Total;
		Kinds.Height = 0.1f * Kinds.Sand + 0.35f * Kinds.Green + 0.8f * Kinds.Brown + Kinds.Pale + 0.5f * Rest / Total;
		return Kinds;
	}

	/** Smooth noise with features about Scale cells wide: blurred white noise, a third of it beyond +-1/3, clamped to +-1. */
	FGrid MakeNoise(int32 SizeX, int32 SizeY, double Scale, int32 Seed)
	{
		FRandomStream Random(Seed);
		FGrid Noise;
		Noise.Init(SizeX, SizeY);
		for (float& Value : Noise.Values)
		{
			Value = Random.FRandRange(-1.f, 1.f);
		}
		Blur(Noise, Scale / 2.5);
		double Squares = 0.0;
		for (const float Value : Noise.Values)
		{
			Squares += double(Value) * Value;
		}
		const float Spread = float(FMath::Max(3.0 * FMath::Sqrt(Squares / Noise.Values.Num()), 1e-6));
		for (float& Value : Noise.Values)
		{
			Value = FMath::Clamp(Value / Spread, -1.f, 1.f);
		}
		return Noise;
	}

	/** The value below which Share (0 to 1) of the grid's cells lie, among those where Mask is set (all when empty). */
	float Quantile(const FGrid& Grid, const TArray<bool>& Mask, double Share)
	{
		TArray<float> Values;
		for (int32 Index = 0; Index < Grid.Values.Num(); ++Index)
		{
			if (Mask.IsEmpty() || Mask[Index])
			{
				Values.Add(Grid.Values[Index]);
			}
		}
		if (Values.IsEmpty())
		{
			return 0.f;
		}
		Values.Sort();
		return Values[FMath::Clamp(FMath::RoundToInt(Share * (Values.Num() - 1)), 0, Values.Num() - 1)];
	}

	/** Erodes (or dilates) a mask by one cell, 4-connected; cells off the grid count as outside. */
	TArray<bool> Grow(const TArray<bool>& Mask, int32 SizeX, int32 SizeY, bool bDilate)
	{
		TArray<bool> Out = Mask;
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				for (const FIntPoint& Step : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
				{
					const int32 NX = X + Step.X;
					const int32 NY = Y + Step.Y;
					const bool bNeighbour = NX >= 0 && NY >= 0 && NX < SizeX && NY < SizeY && Mask[NY * SizeX + NX];
					if (bDilate ? bNeighbour : !bNeighbour)
					{
						Out[Y * SizeX + X] = bDilate;
						break;
					}
				}
			}
		}
		return Out;
	}

	/** Drops the mask's 4-connected pieces under MinCells. */
	void DropSmallPieces(TArray<bool>& Mask, int32 SizeX, int32 SizeY, int32 MinCells)
	{
		TArray<bool> Seen;
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
			if (Piece.Num() < MinCells)
			{
				for (const int32 Index : Piece)
				{
					Mask[Index] = false;
				}
			}
		}
	}

	/** The kinds of ground the grammar lays. */
	enum class EGround : uint8
	{
		Sand,
		Grass,
		Rock,
		Earth,
		Pale,
	};

	FVector3f GetGroundColor(EGround Ground)
	{
		switch (Ground)
		{
		case EGround::Sand: return SandColor;
		case EGround::Grass: return GrassColor;
		case EGround::Rock: return RockColor;
		case EGround::Pale: return PaleColor;
		default: return EarthColor;
		}
	}

	/** A painted island found by the survey: its cells' label, count and box. */
	struct FFound
	{
		int32 Label = 0;
		int32 Cells = 0;
		FIntPoint Min = FIntPoint(MAX_int32);
		FIntPoint Max = FIntPoint(MIN_int32);
		FVector2D Sum = FVector2D::ZeroVector;
		bool bPlaced = false;
	};
}

TSoftObjectPtr<UWorld> LBAPlanetStubs::Build(FLBAImportContext& Context, const FString& Planet, const ULBAPlanetAsset* Asset, double Radius, const FString& Folder)
{
	using namespace LBAPlanetStubsPrivate;

	const FLBAHqrFile* Holomap = Context.GetHqr(TEXT("HOLOMAP.HQR"));
	const FLBAHqrFile* Ress = Context.GetHqr(TEXT("RESS.HQR"));
	TArray<uint8> Texture;
	TArray<uint8> Palette;
	if (!Holomap || !Ress || !Holomap->GetEntry(TextureEntry, Texture) || !Ress->GetEntry(0, Palette) || Texture.Num() < TextureSize * TextureSize || Palette.Num() < 768)
	{
		Context.Warn(FString::Printf(TEXT("Planet %s: no stub islands, the holomap's texture can't be read"), *Planet));
		return nullptr;
	}
	FPainting Painting;
	Painting.Radius = Radius;
	for (int32 Texel = 0; Texel < TextureSize * TextureSize; ++Texel)
	{
		const int32 Index = Texture[Texel] * 3;
		Painting.Colors.Add(FVector3f(Palette[Index], Palette[Index + 1], Palette[Index + 2]));
		Painting.Land.Add(IsLand(Painting.Colors.Last()) ? 1.f : 0.f);
	}

	// The survey: painted land over the hemisphere (its dithering smoothed away), split into islands.
	const double Extent = Radius * FMath::DegreesToRadians(90.0 - LowestLatitude);
	const int32 SurveySize = FMath::CeilToInt(2.0 * Extent / SurveyCell);
	auto SurveyToMap = [&](int32 X, int32 Y) { return FVector2D(-Extent + (X + 0.5) * SurveyCell, -Extent + (Y + 0.5) * SurveyCell); };
	FGrid Survey;
	Survey.Init(SurveySize, SurveySize);
	for (int32 Y = 0; Y < SurveySize; ++Y)
	{
		for (int32 X = 0; X < SurveySize; ++X)
		{
			Survey(X, Y) = Painting.Sample(Painting.Land, SurveyToMap(X, Y));
		}
	}
	Blur(Survey, 1.5);
	TArray<int32> Labels;
	Labels.Init(0, SurveySize * SurveySize);
	TArray<FFound> Found;
	for (int32 Start = 0; Start < Labels.Num(); ++Start)
	{
		const int32 StartX = Start % SurveySize;
		const int32 StartY = Start / SurveySize;
		auto IsLandCell = [&](int32 X, int32 Y) { return Survey(X, Y) > 0.5f && SurveyToMap(X, Y).Size() < Extent; };
		if (Labels[Start] != 0 || !IsLandCell(StartX, StartY))
		{
			continue;
		}
		FFound& Island = Found.AddDefaulted_GetRef();
		Island.Label = Found.Num();
		TArray<FIntPoint> Queue = { FIntPoint(StartX, StartY) };
		Labels[Start] = Island.Label;
		while (!Queue.IsEmpty())
		{
			const FIntPoint P = Queue.Pop(EAllowShrinking::No);
			++Island.Cells;
			Island.Min = Island.Min.ComponentMin(P);
			Island.Max = Island.Max.ComponentMax(P);
			Island.Sum += SurveyToMap(P.X, P.Y);
			for (const FIntPoint& Step : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
			{
				const FIntPoint N = P + Step;
				if (N.X >= 0 && N.Y >= 0 && N.X < SurveySize && N.Y < SurveySize && Labels[N.Y * SurveySize + N.X] == 0 && IsLandCell(N.X, N.Y))
				{
					Labels[N.Y * SurveySize + N.X] = Island.Label;
					Queue.Push(N);
				}
			}
		}
	}
	// The placed islands' paintings are theirs: an island painted around where one is placed isn't a stub.
	for (const FLBAPlanetIsland& Placed : Asset->Islands)
	{
		const FVector2D Map = Placed.GetMapLocation(Radius);
		const int32 CX = FMath::FloorToInt((Map.X + Extent) / SurveyCell);
		const int32 CY = FMath::FloorToInt((Map.Y + Extent) / SurveyCell);
		for (int32 Y = CY - 2; Y <= CY + 2; ++Y)
		{
			for (int32 X = CX - 2; X <= CX + 2; ++X)
			{
				if (X >= 0 && Y >= 0 && X < SurveySize && Y < SurveySize && Labels[Y * SurveySize + X] != 0)
				{
					Found[Labels[Y * SurveySize + X] - 1].bPlaced = true;
				}
			}
		}
	}

	UMaterialInterface* Material = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
	const FName Slot = FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color);
	const FString LevelName = FString::Printf(TEXT("L_%s_Stubs"), *Planet);
	UWorld* World = Context.BeginLevel(Folder, LevelName);
	if (!World)
	{
		return nullptr;
	}
	// LBA1's towns, laid as pads on the islands they were on.
	LBAPlanetDistricts::FDistricts Districts;
	const bool bDistricts = Districts.Load(GetDefault<ULBA2ImporterSettings>()->LBA1Directory.Path, Radius);
	if (!bDistricts)
	{
		Context.Warn(FString::Printf(TEXT("Planet %s: no LBA1 towns on the stub islands, LBA1's data isn't found (the importer's LBA1 Directory)"), *Planet));
	}
	const int32 Home = FMath::Max(Asset->FindIslandIndex(Asset->HomeIsland), 0);
	const FTransform MapToHome = Asset->Islands.IsValidIndex(Home) ? Asset->Islands[Home].GetLevelToMap(Radius).Inverse() : FTransform::Identity;
	int32 Islets = 0;
	for (const FFound& Island : Found)
	{
		if (Island.bPlaced || Island.Cells * SurveyCell * SurveyCell < MinArea)
		{
			continue;
		}
		const FVector2D Center = Island.Sum / Island.Cells;
		FString Name;
		double Nearest = NameReach;
		for (const FNamedPlace& Place : NamedPlaces)
		{
			FLBAPlanetIsland Arrow;
			Arrow.Latitude = Place.Latitude;
			Arrow.Longitude = Place.Longitude;
			const double Distance = FVector2D::Distance(Arrow.GetMapLocation(Radius), Center);
			if (Distance < Nearest)
			{
				Nearest = Distance;
				Name = Place.Name;
			}
		}
		if (Name.IsEmpty())
		{
			Name = FString::Printf(TEXT("Islet_%d"), ++Islets);
		}

		// The island's grid: its painting, its own land (not a neighbour's), the coast's distance, colors and heights;
		// wider where LBA1's districts may grow land.
		const bool bPads = bDistricts && Districts.HasIsland(Name);
		const double Around = Margin + (bPads ? LBAPlanetDistricts::Margin : 0.0);
		const FVector2D Origin = SurveyToMap(Island.Min.X, Island.Min.Y) - FVector2D(SurveyCell / 2.0 + Around);
		const FVector2D Size = FVector2D(Island.Max - Island.Min + FIntPoint(1)) * SurveyCell + FVector2D(2.0 * Around);
		const int32 SizeX = FMath::CeilToInt(Size.X / Cell) + 1;
		const int32 SizeY = FMath::CeilToInt(Size.Y / Cell) + 1;
		auto CellToMap = [&](int32 X, int32 Y) { return Origin + FVector2D(X, Y) * Cell; };
		FGrid Soft, Own, Red, Green, Blue;
		Soft.Init(SizeX, SizeY);
		Own.Init(SizeX, SizeY);
		Red.Init(SizeX, SizeY);
		Green.Init(SizeX, SizeY);
		Blue.Init(SizeX, SizeY);
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				const FVector2D Map = CellToMap(X, Y);
				Soft(X, Y) = Painting.Sample(Painting.Land, Map);
				const int32 SX = FMath::FloorToInt((Map.X + Extent) / SurveyCell);
				const int32 SY = FMath::FloorToInt((Map.Y + Extent) / SurveyCell);
				Own(X, Y) = SX >= 0 && SY >= 0 && SX < SurveySize && SY < SurveySize && Labels[SY * SurveySize + SX] == Island.Label ? 1.f : 0.f;
				const FVector3f Color = Painting.Sample(Painting.Colors, Map);
				Red(X, Y) = Color.X;
				Green(X, Y) = Color.Y;
				Blue(X, Y) = Color.Z;
			}
		}
		Blur(Soft, 3.0);
		Blur(Own, 2.0);
		Blur(Red, 3.5);
		Blur(Green, 3.5);
		Blur(Blue, 3.5);
		const int32 Cells = SizeX * SizeY;
		TArray<bool> Land, Sea;
		Land.SetNumUninitialized(Cells);
		Sea.SetNumUninitialized(Cells);
		FGrid Hint, Sand, Greens, Browns, Pale;
		for (FGrid* Grid : { &Hint, &Sand, &Greens, &Browns, &Pale })
		{
			Grid->Init(SizeX, SizeY);
		}
		int32 LandCells = 0;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Land[Index] = Soft.Values[Index] * Own.Values[Index] > 0.5f;
			Sea[Index] = !Land[Index];
			LandCells += Land[Index] ? 1 : 0;
			const FPaintedKinds Kinds = ReadPainting(FVector3f(Red.Values[Index], Green.Values[Index], Blue.Values[Index]));
			Hint.Values[Index] = Kinds.Height;
			Sand.Values[Index] = Kinds.Sand;
			Greens.Values[Index] = Kinds.Green;
			Browns.Values[Index] = Kinds.Brown;
			Pale.Values[Index] = Kinds.Pale;
		}
		for (FGrid* Grid : { &Hint, &Sand, &Greens, &Browns, &Pale })
		{
			Blur(*Grid, 4.0);
		}
		// Distances in cm from here on.
		FGrid Inland = DistanceOutOf(Land, SizeX, SizeY);
		FGrid Offshore = DistanceOutOf(Sea, SizeX, SizeY);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Inland.Values[Index] *= float(Cell);
			Offshore.Values[Index] *= float(Cell);
		}
		const double Area = LandCells * Cell * Cell;
		const int32 Seed = int32(FCrc::StrCrc32(*Name));

		// The wanted height: off the shore within CoastRise, then up with the painting (judged against the island's own
		// colors: its lowest low, its ridges high) and broad noise for hill masses, reaching full height half its reach in.
		const double Reach = FMath::Max(double(Quantile(Inland, Land, 0.95)), 500.0);
		const double Peak = FMath::Clamp(PeakPerRoot * FMath::Sqrt(Area / 10000.0) * 100.0, MinPeak, MaxPeak);
		const float LowHint = Quantile(Hint, Land, 0.1);
		const float HighHint = Quantile(Hint, Land, 0.9);
		const FGrid Mass = MakeNoise(SizeX, SizeY, FMath::Max(0.8 * Reach, 1000.0) / Cell, Seed);
		const FGrid Detail = MakeNoise(SizeX, SizeY, 1200.0 / Cell, Seed + 1);
		FGrid Wanted;
		Wanted.Init(SizeX, SizeY);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			const double Painted = FMath::Clamp((Hint.Values[Index] - LowHint) / (HighHint - LowHint + 0.1), 0.0, 1.0);
			const double Up = FMath::Pow(FMath::Clamp(0.55 * Painted + 0.45 * (0.5 + 0.5 * Mass.Values[Index]), 0.0, 1.0), 1.3)
				* FMath::Sqrt(FMath::Clamp(Inland.Values[Index] / (0.5 * Reach), 0.0, 1.0));
			const double First = FirstBench * (1.0 - Sand.Values[Index]);
			const double Coast = FMath::Clamp(Inland.Values[Index] / CoastRise, 0.0, 1.0);
			Wanted.Values[Index] = float(Coast * (First + (Peak - First) * Up) + 100.0 * Detail.Values[Index]);
		}

		// The levels: the shore, the first bench, then StepMin to StepMax apart up to the wanted top.
		FRandomStream Steps(Seed + 2);
		TArray<double> Levels = { Shore, FirstBench };
		const double WantedTop = Quantile(Wanted, Land, 0.99);
		while (Levels.Last() + StepMin < WantedTop)
		{
			Levels.Add(Levels.Last() + Steps.FRandRange(StepMin, StepMax));
		}
		Levels.Last() = FMath::Min(Levels.Last(), WantedTop + 50.0);

		// The coast: bays (more land than sea around) take beaches, headlands cliffs; judged on the coast, then carried
		// inland (the nearby coast's average) so a beach runs from the water up to its bench. Painted sand is beach too.
		FGrid Bay, CoastLine, BayCoast;
		Bay.Init(SizeX, SizeY);
		CoastLine.Init(SizeX, SizeY);
		BayCoast.Init(SizeX, SizeY);
		const FGrid BayNoise = MakeNoise(SizeX, SizeY, 3000.0 / Cell, Seed + 3);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Bay.Values[Index] = Land[Index] ? 1.f : 0.f;
		}
		Blur(Bay, BaySigma / Cell);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			const bool bCoast = Land[Index] && Inland.Values[Index] <= 1.5 * Cell;
			const float IsBay = FMath::Max(FMath::Clamp((Bay.Values[Index] - BayMin) / 0.1f, 0.f, 1.f) * (0.6f + 0.4f * BayNoise.Values[Index]),
				Sand.Values[Index] > 0.5f ? 1.f : 0.f);
			CoastLine.Values[Index] = bCoast ? 1.f : 0.f;
			BayCoast.Values[Index] = bCoast ? IsBay : 0.f;
		}
		Blur(CoastLine, 6.0);
		Blur(BayCoast, 6.0);
		TArray<bool> Beach;
		Beach.SetNumUninitialized(Cells);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Beach[Index] = BayCoast.Values[Index] / FMath::Max(CoastLine.Values[Index], 1e-3f) > 0.5f;
		}

		// The benches: each where the wanted height passes halfway to its level, inside the one below and kept back from
		// its edge (the first from the coast), opened (no slivers) and without small pieces.
		struct FBench
		{
			double Rise = 0.0;
			TArray<bool> Region;
		};
		TArray<FBench> Benches;
		TArray<bool> Below = Land;
		for (int32 Level = 1; Level < Levels.Num(); ++Level)
		{
			const FGrid FromEdge = DistanceOutOf(Below, SizeX, SizeY);
			const double Halfway = 0.5 * (Levels[Level - 1] + Levels[Level]);
			TArray<bool> Region;
			Region.SetNumUninitialized(Cells);
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				const double Setback = Level > 1 ? BenchSetback : Beach[Index] ? BeachSetback : CoastSetback;
				Region[Index] = Below[Index] && Wanted.Values[Index] > Halfway && FromEdge.Values[Index] * Cell > Setback;
			}
			Region = Grow(Grow(Grow(Grow(Region, SizeX, SizeY, false), SizeX, SizeY, false), SizeX, SizeY, true), SizeX, SizeY, true);
			DropSmallPieces(Region, SizeX, SizeY, FMath::CeilToInt(MinBenchArea / (Cell * Cell)));
			if (!Region.Contains(true))
			{
				break;
			}
			Benches.Add({ Levels[Level] - Levels[Level - 1], Region });
			Below = MoveTemp(Region);
		}

		// The heights: the shore (a beach rises from just above the sea), each bench's edge climbing over a cliff's or a
		// ramp's width, rolling ground over it all; the shelf under the sea.
		const FGrid RampNoise = MakeNoise(SizeX, SizeY, 2500.0 / Cell, Seed + 4);
		const float RampFrom = Quantile(RampNoise, TArray<bool>(), 1.0 - RampShare);
		const FGrid RollNoise = MakeNoise(SizeX, SizeY, 1400.0 / Cell, Seed + 6);
		FGrid Heights;
		Heights.Init(SizeX, SizeY);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			const double Up = FMath::Clamp(Inland.Values[Index] / 300.0, 0.0, 1.0);
			Heights.Values[Index] = float(Shore + (Beach[Index] ? 25.0 * Up - 10.0 * (1.0 - Up) : 0.0));
		}
		for (const FBench& Bench : Benches)
		{
			TArray<bool> Outside;
			Outside.SetNumUninitialized(Cells);
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				Outside[Index] = !Bench.Region[Index];
			}
			// The signed distance into the bench, smoothed so its edge runs between the cells instead of along them.
			const FGrid IntoBench = DistanceOutOf(Bench.Region, SizeX, SizeY);
			const FGrid OffBench = DistanceOutOf(Outside, SizeX, SizeY);
			FGrid Edge;
			Edge.Init(SizeX, SizeY);
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				Edge.Values[Index] = float((IntoBench.Values[Index] - OffBench.Values[Index]) * Cell);
			}
			Blur(Edge, EdgeSmoothing);
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				const double Ramp = FMath::SmoothStep(RampFrom - 0.08, RampFrom + 0.08, double(RampNoise.Values[Index]));
				const double Width = FMath::Lerp(CliffWidth, RampWidth, Ramp);
				Heights.Values[Index] += float(Bench.Rise * FMath::Clamp(Edge.Values[Index] / Width, 0.0, 1.0));
			}
		}
		double Highest = 0.0;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			if (Land[Index])
			{
				const double Rolled = Heights.Values[Index] + Roll * RollNoise.Values[Index] * FMath::Clamp(Inland.Values[Index] / 400.0, 0.0, 1.0);
				Heights.Values[Index] = float(FMath::Max(Rolled, ShoreMin));
				Highest = FMath::Max(Highest, Rolled);
			}
			else
			{
				Heights.Values[Index] = float(-FMath::Min(SkirtDepth, 30.0 + ShelfSlope * Offshore.Values[Index]));
			}
		}

		// LBA1's districts: their pads in the ground, the coast and the shelf around them again.
		TArray<bool> Pad;
		TArray<FVector3f> PadColors;
		TArray<LBAPlanetDistricts::FPlaced> Placed;
		if (bPads)
		{
			LBAPlanetDistricts::FGround Ground;
			Ground.Origin = Origin;
			Ground.Cell = Cell;
			Ground.SizeX = SizeX;
			Ground.SizeY = SizeY;
			Ground.Heights = Heights.Values;
			Ground.Land = Land;
			Placed = Districts.Lay(Name, Ground);
			Heights.Values = MoveTemp(Ground.Heights);
			Land = MoveTemp(Ground.Land);
			Pad = MoveTemp(Ground.Pad);
			PadColors = MoveTemp(Ground.PadColors);
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				Sea[Index] = !Land[Index];
			}
			Inland = DistanceOutOf(Land, SizeX, SizeY);
			Offshore = DistanceOutOf(Sea, SizeX, SizeY);
			Highest = 0.0;
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				Inland.Values[Index] *= float(Cell);
				Offshore.Values[Index] *= float(Cell);
				if (Land[Index])
				{
					Heights.Values[Index] = float(FMath::Max(double(Heights.Values[Index]), ShoreMin));
					Highest = FMath::Max(Highest, double(Heights.Values[Index]));
				}
				else
				{
					const double Shelf = -FMath::Min(SkirtDepth, 30.0 + ShelfSlope * Offshore.Values[Index]);
					Heights.Values[Index] = float(Ground.PadWater[Index] ? FMath::Min(Shelf, -PadSeaFloor) : Shelf);
				}
			}
		}
		auto IsPad = [&](int32 Index) { return !Pad.IsEmpty() && Pad[Index]; };

		// The ground: grass or earth on the benches by the painting (patches of earth among the grass), painted sand,
		// pale stone on the top bench where painted pale, sand on beaches, rock along cliff coasts and wherever it is steep.
		const FGrid Patches = MakeNoise(SizeX, SizeY, 1200.0 / Cell, Seed + 5);
		const TArray<bool>* TopBench = Benches.IsEmpty() ? nullptr : &Benches.Last().Region;
		auto Slope = [&](int32 X, int32 Y)
		{
			const double DX = (Heights.At(X + 1, Y) - Heights.At(X - 1, Y)) / (2.0 * Cell);
			const double DY = (Heights.At(X, Y + 1) - Heights.At(X, Y - 1)) / (2.0 * Cell);
			return FMath::RadiansToDegrees(FMath::Atan(FMath::Sqrt(DX * DX + DY * DY)));
		};
		auto GroundAt = [&](int32 X, int32 Y)
		{
			const int32 Index = Y * SizeX + X;
			EGround Ground = Greens.Values[Index] > Browns.Values[Index] && Patches.Values[Index] < EarthPatches ? EGround::Grass : EGround::Earth;
			if (Sand.Values[Index] > 0.5f)
			{
				Ground = EGround::Sand;
			}
			if (TopBench && Pale.Values[Index] > 0.4f && (*TopBench)[Index])
			{
				Ground = EGround::Pale;
			}
			if (Beach[Index] && Inland.Values[Index] < BeachSetback)
			{
				Ground = EGround::Sand;
			}
			if (!Beach[Index] && Inland.Values[Index] < CoastSetback + Cell)
			{
				Ground = EGround::Rock;
			}
			if (Slope(X, Y) > RockSlope)
			{
				Ground = EGround::Rock;
			}
			return Ground;
		};
		int32 FlatCells = 0;
		int32 BeachCoast = 0;
		int32 CoastCells = 0;
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				const int32 Index = Y * SizeX + X;
				FlatCells += Land[Index] && Slope(X, Y) < 6.0 ? 1 : 0;
				const bool bCoast = Land[Index] && Inland.Values[Index] <= Cell;
				CoastCells += bCoast ? 1 : 0;
				BeachCoast += bCoast && Beach[Index] ? 1 : 0;
			}
		}

		// Colors: the ground's, a little lighter or darker by the patches' noise; under the sea sand fading to the shelf's
		// pale shallows (like the holomap's lighter rim).
		const FVector Pivot(Center, 0.0);
		auto VertexAt = [&](int32 X, int32 Y)
		{
			const int32 Index = Y * SizeX + X;
			const FVector2D Map = CellToMap(X, Y);
			const FVector Across(2.0 * Cell, 0.0, Heights.At(X + 1, Y) - Heights.At(X - 1, Y));
			const FVector Along(0.0, 2.0 * Cell, Heights.At(X, Y + 1) - Heights.At(X, Y - 1));
			const FVector Normal = (Across ^ Along).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			const FVector3f Color = IsPad(Index) ? PadColors[Index] : Land[Index] ? GetGroundColor(GroundAt(X, Y)) * (1.f + 0.06f * Patches.Values[Index])
				: FMath::Lerp(SandColor, ShelfColor, float(FMath::Clamp(Offshore.Values[Index] / 800.0, 0.0, 1.0)));
			const FLinearColor Linear = FLinearColor::FromSRGBColor(FColor(uint8(FMath::Clamp(Color.X, 0.f, 255.f)), uint8(FMath::Clamp(Color.Y, 0.f, 255.f)), uint8(FMath::Clamp(Color.Z, 0.f, 255.f))));
			FLBAVertex Vertex;
			Vertex.Position = FVector3f(FVector(Map, Heights(X, Y)) - Pivot);
			Vertex.Normal = FVector3f(Normal);
			Vertex.Color = FVector4f(Linear.R, Linear.G, Linear.B, 0.5f);
			return Vertex;
		};

		// The mesh: every cell that rises off the skirt's floor.
		FLBAMeshBuilder Builder;
		const int32 Section = Builder.GetSection(Slot, Material);
		for (int32 Y = 0; Y + 1 < SizeY; ++Y)
		{
			for (int32 X = 0; X + 1 < SizeX; ++X)
			{
				const float Top = FMath::Max(FMath::Max(Heights(X, Y), Heights(X + 1, Y)), FMath::Max(Heights(X, Y + 1), Heights(X + 1, Y + 1)));
				if (Top <= -SkirtDepth + 10.0)
				{
					continue;
				}
				const FLBAVertex A = VertexAt(X, Y);
				const FLBAVertex B = VertexAt(X + 1, Y);
				const FLBAVertex C = VertexAt(X, Y + 1);
				const FLBAVertex D = VertexAt(X + 1, Y + 1);
				Builder.AddTriangleFacing(Section, A, B, D, FVector3f::UpVector);
				Builder.AddTriangleFacing(Section, A, D, C, FVector3f::UpVector);
			}
		}
		const int32 Triangles = Builder.NumTriangles();
		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, Materials, Slots);
		FLBAStaticMeshOptions Options;
		Options.bComplexAsSimple = true;
		Options.bDeferBuild = false;
		UStaticMesh* Mesh = Context.CreateStaticMesh(Folder, FString::Printf(TEXT("SM_LBA_Stub_%s_%s"), *Planet, *Name), MoveTemp(Description), Materials, Slots, Options);
		if (!Mesh)
		{
			continue;
		}
		// Bent onto the planet, it goes down around it: culled by where it is seen, not by where it is.
		Mesh->SetNegativeBoundsExtension(FVector(0.0, 0.0, 2.0 * Radius));
		Mesh->CalculateExtendedBounds();
		Context.MarkForSave(Mesh);
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Pivot), FString::Printf(TEXT("Stub_%s"), *Name), TEXT("Stubs")))
		{
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->Tags.Add(ULBAPlanetAsset::StubTag);
		}
		const FVector InHome = MapToHome.TransformPosition(Pivot);
		FString LevelText;
		for (int32 Level = 0; Level <= Benches.Num(); ++Level)
		{
			LevelText += FString::Printf(TEXT(" %.1f"), Levels[Level] / 100.0);
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s stub %s: %.0f m2 of land, %.1f m high, levels%s m, %.0f%% flat, %.0f%% of the coast beach, map (%.0f, %.0f) m, (%.0f, %.0f) in the home island's level, %d triangles"),
			*Planet, *Name, Area / 10000.0, Highest / 100.0, *LevelText, 100.0 * FlatCells / FMath::Max(LandCells, 1), 100.0 * BeachCoast / FMath::Max(CoastCells, 1),
			Pivot.X / 100.0, Pivot.Y / 100.0, InHome.X, InHome.Y, Triangles);
		for (const LBAPlanetDistricts::FPlaced& District : Placed)
		{
			FString Scenes;
			for (const int32 Scene : District.Scenes)
			{
				Scenes += FString::Printf(TEXT("%s%d"), Scenes.IsEmpty() ? TEXT("") : TEXT(", "), Scene);
			}
			const FString Text = FString::Printf(TEXT("LBA1's %s (scenes %s) on %s: %.0f m2 of land, turned %+.0f degrees, %.1f m from its holomap markers; its land %.0f%% on the painted land, its water %.0f%% on the sea. LBA1 frame (cm) to the map: %s"),
				*District.Name, *Scenes, *Name, District.LandArea / 10000.0, District.Turn, District.FromMarkers / 100.0, 100.0 * District.LandOnLand, 100.0 * District.WaterOnSea,
				*District.FrameToMap.ToHumanReadableString());
			UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s district: %s"), *Planet, *Text);
			const FVector2D AtCell = (District.Map - Origin) / Cell;
			const double Ground = Heights.At(FMath::RoundToInt(AtCell.X), FMath::RoundToInt(AtCell.Y));
			if (ANote* Note = Context.SpawnGeneratedActor<ANote>(World, FTransform(FVector(District.Map, Ground + 300.0)), FString::Printf(TEXT("District_%s_%s"), *Name, *FLBAImportContext::SanitizeName(District.Name)), TEXT("Stubs")))
			{
#if WITH_EDITORONLY_DATA
				Note->Text = Text;
#endif
			}
		}
	}
	Context.EndLevel(World);
	return TSoftObjectPtr<UWorld>(FSoftObjectPath(Folder / LevelName + TEXT(".") + LevelName));
}
