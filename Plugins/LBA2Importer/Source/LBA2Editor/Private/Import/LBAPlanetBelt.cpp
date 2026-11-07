#include "Import/LBAPlanetBelt.h"

#include "Async/ParallelFor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Formats/LBAHqr.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"

namespace LBAPlanetBeltPrivate
{
	/** HOLOMAP.HQR entries (HOLO.H): Twinsun's altitudes (HQR_TWINSUN_HMT) and texture (HQR_TWINSUN_HMG). */
	constexpr int32 AltitudeEntry = 1;
	constexpr int32 TextureEntry = 2;
	/** Altitudes: rows from latitude -90 to 90, columns every 11.25 degrees of longitude (STEP_ANGLE). */
	constexpr int32 AltitudeRows = 17;
	constexpr int32 AltitudeColumns = 32;
	constexpr int32 TextureSize = 256;
	/** HOLOGLOB.CPP: the globe's vertices are this far out (TabRayon, Twinsun) plus twice their altitude. */
	constexpr double HolomapRadius = 1000.0;

	/**
	 * The grid the belt grows on: columns of longitude all around (1.8 m apart at the islands' shore, 2.9 m at the far
	 * one on Twinsun), rows of latitude RowSize apart from LatitudeTop (in the islands' sea) to LatitudeBottom (in the
	 * other hemisphere's). Pad columns are repeated on either side, so blurs and distances run on around the seam.
	 */
	constexpr int32 Columns = 1536;
	constexpr int32 Pad = 96;
	constexpr double RowSize = 200.0;
	constexpr double LatitudeTop = 18.0;
	constexpr double LatitudeBottom = -46.0;
	/** Land nearer the pole than this is the stub islands' (LBAPlanetStubs' survey). */
	constexpr double LandLatitude = 14.0;
	/** The painting's coasts run along its texel rows: noise moves them this much of its land share (features ~16 m). */
	constexpr float CoastWander = 0.35f;
	/** Pieces of land off the ring are kept when this big (cm2) and this near it (cm). */
	constexpr double MinPiece = 300.0 * 10000.0;
	constexpr double NearRing = 2500.0;

	// The ground's grammar (Tools/belt_grammar.py): as the stub islands' (benches, cliffs and ramps between them), but
	// taller, at the holomap's own heights. Lengths in cm.
	constexpr double Shore = 40.0;
	constexpr double FirstBench = 250.0;
	/** The wall: the bench over its riser is this high, the riser one cell (over 70 degrees: steeper than Twinsen walks). */
	constexpr double Wall = 1000.0;
	/** Benches over the wall, this far apart. */
	constexpr double StepMin = 350.0;
	constexpr double StepMax = 700.0;
	/** Wanted height = the holomap's times this, from the painting's dark blue to its white (about its mean over the belt). */
	constexpr double BrightLow = 0.35;
	constexpr double BrightHigh = 1.3;
	/** Wanted height of white painted land where the holomap's mesh is at the sea's level. */
	constexpr double OffMesh = 800.0;
	/** The wanted height eases in under the holomap's highest vertices around over this (a hard cap leaves plateaus). */
	constexpr double CapSoftness = 400.0;
	/** Every column's crest, this wide, is held over the wall's next bench: the islands' hemisphere is closed to walkers. */
	constexpr double CrestWidth = 1200.0;
	constexpr double BenchSetback = 200.0;
	constexpr double CoastSetback = 200.0;
	constexpr double MinBenchArea = 60.0 * 10000.0;
	/** A bench's edge rises over a cliff's width, or a ramp's on RampShare of them. */
	constexpr double CliffWidth = 200.0;
	constexpr double RampWidth = 1200.0;
	constexpr double RampShare = 0.25;
	/** The wanted height leaves the shore over this far inland: a flank before the wall. */
	constexpr double CoastRise = 2500.0;
	/** Bench edges are smoothed over this many cells (gaussian), so they don't step along the cells. */
	constexpr double EdgeSmoothing = 2.0;
	/** The wall and the levels over it drift up and down along the belt by this (features ~100 m): terraces at many heights. */
	constexpr double LevelDrift = 250.0;
	/** Mesh points move this share of a cell off the grid (hashed), so cliffs don't follow its columns. Under a quarter: from there a cell's triangle can fold over, a hole in a cliff from the side. */
	constexpr double Jitter = 0.2;
	/** Noise on the wanted height (features ~10 m) and rolling ground over the benches (~14 m). */
	constexpr double Detail = 250.0;
	constexpr double Roll = 60.0;
	/** The mean height per 11.25 degrees is brought to the holomap mesh's in this many passes. */
	constexpr int32 CalibrationPasses = 2;
	/** Under the sea: shallow (to ShelfDepth) where the painting speckles its teal fringe, else down to SkirtDepth. */
	constexpr double ShelfDepth = 250.0;
	constexpr double SkirtDepth = 800.0;
	/** Twinsen walks up to this slope (degrees): the build checks no one walks across the belt. */
	constexpr double WalkLimit = 55.0;

	/** The holomap's colors through the albedos calibrated for the enhanced grade (sRGB). */
	const FVector3f DarkIce(86.f, 122.f, 146.f);
	const FVector3f LightIce(170.f, 190.f, 200.f);
	const FVector3f SnowShade(196.f, 216.f, 230.f);
	const FVector3f Snow(232.f, 238.f, 246.f);
	const FVector3f Brown(131.f, 103.f, 55.f);
	const FVector3f BrownCliff(96.f, 78.f, 50.f);
	const FVector3f Green(110.f, 124.f, 84.f);
	const FVector3f Frozen(150.f, 186.f, 204.f);
	const FVector3f Shelf(176.f, 222.f, 204.f);
	const FVector3f Deep(40.f, 70.f, 90.f);

	/** Mesh: a sector per altitude column. */
	constexpr int32 Sectors = 32;

	/** A grid of values, X along the columns. */
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

	/** Gaussian weights out to 3 sigmas. */
	TArray<float> Gaussian(double Sigma, int32& OutReach)
	{
		OutReach = FMath::Max(FMath::CeilToInt(3.0 * Sigma), 0);
		TArray<float> Weights;
		float Total = 0.f;
		for (int32 Offset = -OutReach; Offset <= OutReach; ++Offset)
		{
			Weights.Add(Sigma > 0.0 ? float(FMath::Exp(-0.5 * FMath::Square(Offset / Sigma))) : 1.f);
			Total += Weights.Last();
		}
		for (float& Weight : Weights)
		{
			Weight /= Total;
		}
		return Weights;
	}

	/** Gaussian blur, SigmaX along the columns and SigmaY along the rows (cells), edges clamped. */
	void Blur(FGrid& Grid, double SigmaX, double SigmaY)
	{
		int32 ReachX = 0;
		int32 ReachY = 0;
		const TArray<float> WeightsX = Gaussian(SigmaX, ReachX);
		const TArray<float> WeightsY = Gaussian(SigmaY, ReachY);
		FGrid Pass = Grid;
		ParallelFor(Grid.SizeY, [&](int32 Y)
		{
			for (int32 X = 0; X < Grid.SizeX; ++X)
			{
				float Sum = 0.f;
				for (int32 Offset = -ReachX; Offset <= ReachX; ++Offset)
				{
					Sum += WeightsX[Offset + ReachX] * Grid.At(X + Offset, Y);
				}
				Pass(X, Y) = Sum;
			}
		});
		ParallelFor(Grid.SizeY, [&](int32 Y)
		{
			for (int32 X = 0; X < Grid.SizeX; ++X)
			{
				float Sum = 0.f;
				for (int32 Offset = -ReachY; Offset <= ReachY; ++Offset)
				{
					Sum += WeightsY[Offset + ReachY] * Pass.At(X, Y + Offset);
				}
				Grid(X, Y) = Sum;
			}
		});
	}

	/** Squared distance transform along one line of samples Spacing apart (Felzenszwalb and Huttenlocher): F in, D out. */
	void DistanceLine(const TArray<double>& F, double Spacing, TArray<double>& D)
	{
		const int32 Num = F.Num();
		const double S2 = Spacing * Spacing;
		TArray<int32> V;
		TArray<double> Z;
		V.SetNumZeroed(Num);
		Z.SetNumZeroed(Num + 1);
		int32 K = 0;
		Z[0] = -TNumericLimits<double>::Max();
		Z[1] = TNumericLimits<double>::Max();
		auto Meet = [&](int32 Q, int32 P) { return ((F[Q] + S2 * Q * Q) - (F[P] + S2 * P * P)) / (2.0 * S2 * (Q - P)); };
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
			D[Q] = S2 * FMath::Square(double(Q - V[K])) + F[V[K]];
		}
	}

	/** Distance (cm) from each cell to the nearest cell where Inside is false; cells DX by DY cm. */
	FGrid DistanceOutOf(const TArray<bool>& Inside, int32 SizeX, int32 SizeY, double DX, double DY)
	{
		constexpr double Far = 1e30;
		FGrid Out;
		Out.Init(SizeX, SizeY);
		TArray<double> Rows;
		Rows.SetNumUninitialized(SizeX * SizeY);
		ParallelFor(SizeY, [&](int32 Y)
		{
			TArray<double> F, D;
			F.SetNumUninitialized(SizeX);
			for (int32 X = 0; X < SizeX; ++X)
			{
				F[X] = Inside[Y * SizeX + X] ? Far : 0.0;
			}
			DistanceLine(F, DX, D);
			for (int32 X = 0; X < SizeX; ++X)
			{
				Rows[Y * SizeX + X] = D[X];
			}
		});
		ParallelFor(SizeX, [&](int32 X)
		{
			TArray<double> F, D;
			F.SetNumUninitialized(SizeY);
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				F[Y] = Rows[Y * SizeX + X];
			}
			DistanceLine(F, DY, D);
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				Out(X, Y) = float(FMath::Sqrt(FMath::Min(D[Y], 1e12)));
			}
		});
		return Out;
	}

	TArray<bool> Not(const TArray<bool>& Mask)
	{
		TArray<bool> Out;
		Out.SetNumUninitialized(Mask.Num());
		for (int32 Index = 0; Index < Mask.Num(); ++Index)
		{
			Out[Index] = !Mask[Index];
		}
		return Out;
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

	TArray<bool> Open(const TArray<bool>& Mask, int32 SizeX, int32 SizeY)
	{
		return Grow(Grow(Mask, SizeX, SizeY, false), SizeX, SizeY, true);
	}

	/** 4-connected pieces of a mask: each cell's label (0 outside, 1..N), and each label's cell count (index label - 1). */
	TArray<int32> Label(const TArray<bool>& Mask, int32 SizeX, int32 SizeY, TArray<int32>& OutCounts)
	{
		TArray<int32> Labels;
		Labels.Init(0, Mask.Num());
		OutCounts.Reset();
		TArray<int32> Piece;
		for (int32 Start = 0; Start < Mask.Num(); ++Start)
		{
			if (!Mask[Start] || Labels[Start])
			{
				continue;
			}
			const int32 Id = OutCounts.Num() + 1;
			Piece.Reset();
			Piece.Add(Start);
			Labels[Start] = Id;
			for (int32 Next = 0; Next < Piece.Num(); ++Next)
			{
				const int32 X = Piece[Next] % SizeX;
				const int32 Y = Piece[Next] / SizeX;
				for (const FIntPoint& Step : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
				{
					const int32 NX = X + Step.X;
					const int32 NY = Y + Step.Y;
					const int32 N = NY * SizeX + NX;
					if (NX >= 0 && NY >= 0 && NX < SizeX && NY < SizeY && Mask[N] && !Labels[N])
					{
						Labels[N] = Id;
						Piece.Add(N);
					}
				}
			}
			OutCounts.Add(Piece.Num());
		}
		return Labels;
	}

	/**
	 * Smooth noise with features about Scale cm wide: blurred white noise, a third of it beyond +-1/3, clamped to +-1.
	 * The white noise repeats every Columns columns (the padding copies them), so it runs on around the seam.
	 */
	FGrid MakeNoise(int32 SizeX, int32 SizeY, double Scale, int32 Seed, double DX, double DY)
	{
		FRandomStream Random(Seed);
		FGrid Noise;
		Noise.Init(SizeX, SizeY);
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 Column = 0; Column < Columns; ++Column)
			{
				Noise(Pad + Column, Y) = Random.FRandRange(-1.f, 1.f);
			}
			for (int32 X = 0; X < Pad; ++X)
			{
				Noise(X, Y) = Noise(X + Columns, Y);
				Noise(SizeX - 1 - X, Y) = Noise(SizeX - 1 - X - Columns, Y);
			}
		}
		Blur(Noise, Scale / DX / 2.5, Scale / DY / 2.5);
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

	/** A texture-sized layer (latitude rows -90 to 90, longitude columns), blurred in texels (wrapping in longitude). */
	struct FTexels
	{
		TArray<float> Values;

		void Smooth(double Sigma)
		{
			int32 Reach = 0;
			const TArray<float> Weights = Gaussian(Sigma, Reach);
			TArray<float> Pass = Values;
			for (int32 Row = 0; Row < TextureSize; ++Row)
			{
				for (int32 Column = 0; Column < TextureSize; ++Column)
				{
					float Sum = 0.f;
					for (int32 Offset = -Reach; Offset <= Reach; ++Offset)
					{
						Sum += Weights[Offset + Reach] * Values[Row * TextureSize + ((Column + Offset) % TextureSize + TextureSize) % TextureSize];
					}
					Pass[Row * TextureSize + Column] = Sum;
				}
			}
			for (int32 Row = 0; Row < TextureSize; ++Row)
			{
				for (int32 Column = 0; Column < TextureSize; ++Column)
				{
					float Sum = 0.f;
					for (int32 Offset = -Reach; Offset <= Reach; ++Offset)
					{
						Sum += Weights[Offset + Reach] * Pass[FMath::Clamp(Row + Offset, 0, TextureSize - 1) * TextureSize + Column];
					}
					Values[Row * TextureSize + Column] = Sum;
				}
			}
		}

		/** Bilinearly at a latitude and longitude (degrees), as LBAPlanetStubs' FPainting::Sample. */
		float Sample(double Latitude, double Longitude) const
		{
			const double V = (Latitude + 90.0) / 180.0 * TextureSize - 0.5;
			const double U = Longitude / 360.0 * TextureSize - 0.5;
			const int32 V0 = FMath::FloorToInt(V);
			const int32 U0 = FMath::FloorToInt(U);
			const float FV = float(V - V0);
			const float FU = float(U - U0);
			auto At = [&](int32 Row, int32 Column) { return Values[FMath::Clamp(Row, 0, TextureSize - 1) * TextureSize + ((Column % TextureSize) + TextureSize) % TextureSize]; };
			return (At(V0, U0) * (1.f - FU) + At(V0, U0 + 1) * FU) * (1.f - FV) + (At(V0 + 1, U0) * (1.f - FU) + At(V0 + 1, U0 + 1) * FU) * FV;
		}
	};

	/** The painting read for the belt, per texel. */
	struct FPainted
	{
		/** Its blues, whites, browns and greens; not the dark sea nor the teal fringe (shallow water). */
		FTexels Land;
		/** 0 for its darkest blue, 1 for its white. */
		FTexels Bright;
		FTexels BrownShare;
		FTexels GreenShare;
		FTexels Teal;
	};

	FPainted ReadPainting(const TArray<uint8>& Texture, const TArray<uint8>& Palette)
	{
		FPainted Out;
		for (FTexels* Layer : { &Out.Land, &Out.Bright, &Out.BrownShare, &Out.GreenShare, &Out.Teal })
		{
			Layer->Values.SetNumZeroed(TextureSize * TextureSize);
		}
		for (int32 Index = 0; Index < TextureSize * TextureSize; ++Index)
		{
			const int32 R = Palette[Texture[Index] * 3];
			const int32 G = Palette[Texture[Index] * 3 + 1];
			const int32 B = Palette[Texture[Index] * 3 + 2];
			const int32 Sum = R + G + B;
			const bool bTeal = R < 80 && G > 0.85 * B && Sum > 150;
			const bool bWater = Sum < 150 || bTeal;
			const bool bBrown = R > B + 10 && !bWater;
			const bool bGreen = G > R && G > B && !bWater && !bBrown;
			Out.Land.Values[Index] = bWater ? 0.f : 1.f;
			Out.Bright.Values[Index] = bWater ? 0.f : FMath::Clamp((Sum - 237.f) / (725.f - 237.f), 0.f, 1.f);
			Out.BrownShare.Values[Index] = bBrown ? 1.f : 0.f;
			Out.GreenShare.Values[Index] = bGreen ? 1.f : 0.f;
			Out.Teal.Values[Index] = bTeal ? 1.f : 0.f;
		}
		return Out;
	}

	FVector3f Lerp(const FVector3f& A, const FVector3f& B, float T)
	{
		return A + (B - A) * T;
	}

	/** The ice's color for the painting's brightness: dark ice, light ice, shaded snow, snow. */
	FVector3f IceColor(float Bright)
	{
		if (Bright < 0.45f)
		{
			return Lerp(DarkIce, LightIce, Bright / 0.45f);
		}
		if (Bright < 0.75f)
		{
			return Lerp(LightIce, SnowShade, (Bright - 0.45f) / 0.3f);
		}
		return Lerp(SnowShade, Snow, FMath::Min((Bright - 0.75f) / 0.25f, 1.f));
	}

	/** -1 to 1, fixed for a cell. */
	double Hash(int32 A, int32 B, int32 Seed)
	{
		uint32 H = uint32(A) * 73856093u ^ uint32(B) * 19349663u ^ uint32(Seed) * 83492791u;
		H ^= H >> 13;
		H *= 0x5bd1e995u;
		H ^= H >> 15;
		return (H & 0xffffff) / double(0x7fffff) - 1.0;
	}

	/** Log-sum-exp form of a smooth minimum: Value eased under Cap over Softness. */
	double SoftMin(double Value, double Cap, double Softness)
	{
		const double X = (Value - Cap) / Softness;
		return Value - Softness * (X > 30.0 ? X : FMath::Loge(1.0 + FMath::Exp(X)));
	}
}

TSoftObjectPtr<UWorld> LBAPlanetBelt::Build(FLBAImportContext& Context, const FString& Planet, double Radius, const FString& Folder, TArray<FVector2D>* OutLand)
{
	using namespace LBAPlanetBeltPrivate;

	const FLBAHqrFile* Holomap = Context.GetHqr(TEXT("HOLOMAP.HQR"));
	const FLBAHqrFile* Ress = Context.GetHqr(TEXT("RESS.HQR"));
	TArray<uint8> Altitudes;
	TArray<uint8> Texture;
	TArray<uint8> Palette;
	if (!Holomap || !Ress || !Holomap->GetEntry(AltitudeEntry, Altitudes) || !Holomap->GetEntry(TextureEntry, Texture) || !Ress->GetEntry(0, Palette)
		|| Altitudes.Num() < AltitudeRows * AltitudeColumns || Texture.Num() < TextureSize * TextureSize || Palette.Num() < 768)
	{
		Context.Warn(FString::Printf(TEXT("Planet %s: no belt, the holomap's altitudes or texture can't be read"), *Planet));
		return nullptr;
	}

	// The grid: columns (padded) by rows, each cell's latitude and longitude, its size in cm.
	const double RowDegrees = FMath::RadiansToDegrees(RowSize / Radius);
	const int32 Rows = FMath::CeilToInt((LatitudeTop - LatitudeBottom) / RowDegrees);
	const int32 SizeX = Columns + 2 * Pad;
	const int32 SizeY = Rows;
	const int32 Cells = SizeX * SizeY;
	auto RowLatitude = [&](int32 Row) { return LatitudeTop - (Row + 0.5) * RowDegrees; };
	auto ColumnLongitude = [&](int32 X) { return (((X - Pad) % Columns + Columns) % Columns + 0.5) * 360.0 / Columns; };
	const double DY = RowSize;
	const double DX = Radius * FMath::DegreesToRadians(90.0 - RowLatitude(Rows / 2)) * 2.0 * UE_DOUBLE_PI / Columns;
	const double CellArea = DX * DY;

	// 1. Footprint: the painting's land, its coasts wandering off the texels, frozen basins, the ring.
	FPainted Painted = ReadPainting(Texture, Palette);
	FTexels SoftLand = Painted.Land;
	SoftLand.Smooth(0.7);
	FTexels Speckle;
	Speckle.Values.SetNumUninitialized(TextureSize * TextureSize);
	for (int32 Index = 0; Index < Speckle.Values.Num(); ++Index)
	{
		Speckle.Values[Index] = Painted.Teal.Values[Index] + Painted.Land.Values[Index];
		Painted.Bright.Values[Index] *= Painted.Land.Values[Index];
	}
	Speckle.Smooth(1.5);
	FTexels LandShare = Painted.Land;
	for (FTexels* Layer : { &LandShare, &Painted.Bright, &Painted.BrownShare, &Painted.GreenShare })
	{
		Layer->Smooth(0.8);
	}

	const FGrid Wander = MakeNoise(SizeX, SizeY, 1600.0, 11, DX, DY);
	TArray<bool> Land;
	Land.SetNumUninitialized(Cells);
	FGrid Bright, BrownShare, GreenShare, Shallow, Mesh, Peak;
	for (FGrid* Grid : { &Bright, &BrownShare, &GreenShare, &Shallow, &Mesh, &Peak })
	{
		Grid->Init(SizeX, SizeY);
	}
	auto Vertex = [&](int32 Row, int32 Column) { return 2.0 * Altitudes[FMath::Clamp(Row, 0, AltitudeRows - 1) * AltitudeColumns + (Column % AltitudeColumns + AltitudeColumns) % AltitudeColumns] / HolomapRadius * Radius; };
	for (int32 Y = 0; Y < SizeY; ++Y)
	{
		const double Latitude = RowLatitude(Y);
		for (int32 X = 0; X < SizeX; ++X)
		{
			const double Longitude = ColumnLongitude(X);
			const int32 Index = Y * SizeX + X;
			Land[Index] = SoftLand.Sample(Latitude, Longitude) + CoastWander * Wander(X, Y) > 0.5f && Latitude <= LandLatitude;
			Bright(X, Y) = Painted.Bright.Sample(Latitude, Longitude) / FMath::Max(LandShare.Sample(Latitude, Longitude), 0.05f);
			BrownShare(X, Y) = Painted.BrownShare.Sample(Latitude, Longitude);
			GreenShare(X, Y) = Painted.GreenShare.Sample(Latitude, Longitude);
			Shallow(X, Y) = FMath::Clamp(Speckle.Sample(Latitude, Longitude) / 0.35f, 0.f, 1.f);
			// The holomap's mesh, bilinear between its vertices, and the highest of the four around.
			const double V = (Latitude + 90.0) / 11.25;
			const double U = Longitude / 11.25;
			const int32 V0 = FMath::Clamp(FMath::FloorToInt(V), 0, AltitudeRows - 2);
			const int32 U0 = FMath::FloorToInt(U);
			const double FV = V - V0;
			const double FU = U - U0;
			Mesh(X, Y) = float((Vertex(V0, U0) * (1.0 - FU) + Vertex(V0, U0 + 1) * FU) * (1.0 - FV) + (Vertex(V0 + 1, U0) * (1.0 - FU) + Vertex(V0 + 1, U0 + 1) * FU) * FV);
			Peak(X, Y) = float(FMath::Max(FMath::Max(Vertex(V0, U0), Vertex(V0, U0 + 1)), FMath::Max(Vertex(V0 + 1, U0), Vertex(V0 + 1, U0 + 1))));
		}
	}
	for (float& Value : Peak.Values)
	{
		Value = FMath::Max(Value, float(OffMesh));
	}
	Blur(Peak, 40.0, 8.0);

	// Water the open sea doesn't reach (the painting's dark basins inside the belt) is frozen: low land.
	TArray<bool> FrozenMask;
	FrozenMask.Init(false, Cells);
	{
		TArray<int32> Counts;
		const TArray<int32> Water = Label(Not(Land), SizeX, SizeY, Counts);
		TSet<int32> OpenSea;
		for (int32 X = 0; X < SizeX; ++X)
		{
			OpenSea.Add(Water[X]);
			OpenSea.Add(Water[(SizeY - 1) * SizeX + X]);
		}
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			if (Water[Index] && !OpenSea.Contains(Water[Index]))
			{
				FrozenMask[Index] = true;
				Land[Index] = true;
			}
		}
	}
	// Only the ring of land around the planet (the largest piece, through the padding) and pieces near it.
	{
		TArray<int32> Counts;
		const TArray<int32> Pieces = Label(Land, SizeX, SizeY, Counts);
		if (Counts.IsEmpty())
		{
			Context.Warn(FString::Printf(TEXT("Planet %s: no belt, the holomap's painting has no land around the equator"), *Planet));
			return nullptr;
		}
		int32 Ring = 1;
		for (int32 Id = 1; Id <= Counts.Num(); ++Id)
		{
			Ring = Counts[Id - 1] > Counts[Ring - 1] ? Id : Ring;
		}
		TArray<bool> NotRing;
		NotRing.SetNumUninitialized(Cells);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			NotRing[Index] = Pieces[Index] != Ring;
		}
		const FGrid FromRing = DistanceOutOf(NotRing, SizeX, SizeY, DX, DY);
		TSet<int32> Keep = { Ring };
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			const int32 Id = Pieces[Index];
			if (Id && Id != Ring && FromRing.Values[Index] < NearRing && Counts[Id - 1] * CellArea >= MinPiece)
			{
				Keep.Add(Id);
			}
		}
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Land[Index] = Keep.Contains(Pieces[Index]);
			FrozenMask[Index] &= Land[Index];
		}
		Land = Open(Land, SizeX, SizeY);
	}
	const FGrid Inland = DistanceOutOf(Land, SizeX, SizeY, DX, DY);
	const FGrid Offshore = DistanceOutOf(Not(Land), SizeX, SizeY, DX, DY);

	// 2. Wanted height: the holomap mesh's, by the painting's brightness; low painted ground off its mesh; noise.
	const FGrid DetailNoise = MakeNoise(SizeX, SizeY, 1000.0, 7, DX, DY);
	FGrid Wanted0;
	Wanted0.Init(SizeX, SizeY);
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		const float B = FMath::Clamp(Bright.Values[Index], 0.f, 1.f);
		const float Br = FMath::Clamp(BrownShare.Values[Index], 0.f, 1.f);
		const double Shade = (BrightLow + (BrightHigh - BrightLow) * FMath::Pow(B, 1.2f)) * (1.0 - 0.6 * Br);
		double Wanted = FMath::Max(Mesh.Values[Index] * Shade, OffMesh * B * (1.0 - Br) + 300.0 * Br);
		Wanted += Detail * DetailNoise.Values[Index] * FMath::Clamp(Wanted / 1000.0, 0.0, 1.0);
		Wanted *= FMath::Clamp(Inland.Values[Index] / CoastRise, 0.0, 1.0);
		Wanted0.Values[Index] = float(Wanted);
	}
	// The crest: a line all around along the highest ground (smoothed so it runs unbroken).
	TArray<bool> Crest;
	Crest.Init(false, Cells);
	{
		FGrid Broad = Wanted0;
		Blur(Broad, 25.0, 4.0);
		TArray<double> CrestRow;
		CrestRow.SetNumZeroed(Columns);
		for (int32 Column = 0; Column < Columns; ++Column)
		{
			const int32 X = Column + Pad;
			float Best = -1.f;
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				if (Land[Y * SizeX + X] && Broad(X, Y) > Best)
				{
					Best = Broad(X, Y);
					CrestRow[Column] = Y;
				}
			}
		}
		int32 Reach = 0;
		const TArray<float> Weights = Gaussian(12.0, Reach);
		for (int32 X = 0; X < SizeX; ++X)
		{
			double Row = 0.0;
			for (int32 Offset = -Reach; Offset <= Reach; ++Offset)
			{
				Row += Weights[Offset + Reach] * CrestRow[((X - Pad + Offset) % Columns + Columns) % Columns];
			}
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				Crest[Y * SizeX + X] = FMath::Abs(Y - Row) * DY <= CrestWidth / 2.0 && Land[Y * SizeX + X];
			}
		}
	}

	// 3. Benches: levels, each bench nested in the one below; risers cliffs or ramps; the wall's always a cliff.
	const FGrid RampNoise = MakeNoise(SizeX, SizeY, 4000.0, 4, DX, DY);
	const float RampFrom = [&]
	{
		TArray<float> Sorted = RampNoise.Values;
		Sorted.Sort();
		return Sorted[FMath::Clamp(FMath::RoundToInt((1.0 - RampShare) * (Sorted.Num() - 1)), 0, Sorted.Num() - 1)];
	}();
	const FGrid RollNoise = MakeNoise(SizeX, SizeY, 1400.0, 6, DX, DY);
	const FGrid Drift = MakeNoise(SizeX, SizeY, 10000.0, 8, DX, DY);
	TArray<double> Levels;
	int32 NumBenches = 0;
	auto BuildGround = [&](const FGrid& Wanted)
	{
		FRandomStream Random(3);
		Levels = { Shore, FirstBench, Wall };
		TArray<float> LandWanted;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			if (Land[Index])
			{
				LandWanted.Add(Wanted.Values[Index]);
			}
		}
		LandWanted.Sort();
		const double Top = LandWanted.IsEmpty() ? 0.0 : LandWanted[FMath::RoundToInt(0.995 * (LandWanted.Num() - 1))];
		while (Levels.Last() + StepMin < Top)
		{
			Levels.Add(Levels.Last() + Random.FRandRange(StepMin, StepMax));
		}
		FGrid Height;
		Height.Init(SizeX, SizeY);
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Height.Values[Index] = Land[Index] ? float(Shore) : 0.f;
		}
		TArray<bool> Below = Land;
		NumBenches = 0;
		for (int32 Level = 1; Level < Levels.Num(); ++Level)
		{
			const double Low = Levels[Level - 1];
			const double High = Levels[Level];
			const FGrid FromBelow = DistanceOutOf(Below, SizeX, SizeY, DX, DY);
			TArray<bool> Region;
			Region.SetNumUninitialized(Cells);
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				// Levels over the wall drift with the place, so the terraces aren't at one height all around.
				const double Shift = High >= Wall ? LevelDrift * Drift.Values[Index] : 0.0;
				Region[Index] = Below[Index] && Wanted.Values[Index] > (Low + High) / 2.0 + Shift && FromBelow.Values[Index] > (Level == 1 ? CoastSetback : BenchSetback);
			}
			Region = Open(Region, SizeX, SizeY);
			TArray<int32> Counts;
			const TArray<int32> Pieces = Label(Region, SizeX, SizeY, Counts);
			bool bAny = false;
			for (int32 Index = 0; Index < Cells; ++Index)
			{
				Region[Index] = Pieces[Index] && Counts[Pieces[Index] - 1] * CellArea >= MinBenchArea;
				bAny |= Region[Index];
			}
			if (!bAny)
			{
				break;
			}
			++NumBenches;
			if (High == Wall)
			{
				// The wall rises within one cell: nothing smooths it into a walkable slope.
				for (int32 Index = 0; Index < Cells; ++Index)
				{
					Height.Values[Index] += Region[Index] ? float(High - Low + LevelDrift * Drift.Values[Index]) : 0.f;
				}
			}
			else
			{
				const FGrid Inside = DistanceOutOf(Region, SizeX, SizeY, DX, DY);
				const FGrid Outside = DistanceOutOf(Not(Region), SizeX, SizeY, DX, DY);
				FGrid Edge;
				Edge.Init(SizeX, SizeY);
				for (int32 Index = 0; Index < Cells; ++Index)
				{
					Edge.Values[Index] = Inside.Values[Index] - Outside.Values[Index];
				}
				Blur(Edge, EdgeSmoothing, EdgeSmoothing);
				for (int32 Index = 0; Index < Cells; ++Index)
				{
					const double T = FMath::Clamp((RampNoise.Values[Index] - (RampFrom - 0.08)) / 0.16, 0.0, 1.0);
					const double Width = CliffWidth + (RampWidth - CliffWidth) * T * T * (3.0 - 2.0 * T);
					Height.Values[Index] += Land[Index] ? float((High - Low) * FMath::Clamp(Edge.Values[Index] / Width, 0.0, 1.0)) : 0.f;
				}
			}
			Below = Region;
		}
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			Height.Values[Index] = Land[Index] ? FMath::Max(Height.Values[Index] + float(Roll * RollNoise.Values[Index] * FMath::Clamp(Inland.Values[Index] / 400.0, 0.0, 1.0)), 15.f) : 0.f;
		}
		return Height;
	};
	auto Sector = [&](int32 X) { return ((X - Pad) % Columns + Columns) % Columns * Sectors / Columns; };
	auto SectorMeans = [&](const FGrid& Grid)
	{
		TArray<double> Sums, Counts;
		Sums.SetNumZeroed(Sectors);
		Counts.SetNumZeroed(Sectors);
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = Pad; X < Pad + Columns; ++X)
			{
				if (Land[Y * SizeX + X])
				{
					Sums[Sector(X)] += Grid(X, Y);
					Counts[Sector(X)] += 1.0;
				}
			}
		}
		for (int32 Index = 0; Index < Sectors; ++Index)
		{
			Sums[Index] /= FMath::Max(Counts[Index], 1.0);
		}
		return Sums;
	};
	const TArray<double> Target = SectorMeans(Mesh);
	TArray<double> Scale;
	Scale.Init(1.0, Sectors);
	FGrid Height;
	FGrid Wanted;
	for (int32 Pass = 0; Pass <= CalibrationPasses; ++Pass)
	{
		Wanted = Wanted0;
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				// Each sector's scale at its middle, linearly between.
				const double Position = (((X - Pad) % Columns + Columns) % Columns + 0.5) * Sectors / Columns - 0.5;
				const int32 S0 = FMath::FloorToInt(Position);
				const double T = Position - S0;
				const double SectorScale = FMath::Lerp(Scale[(S0 + Sectors) % Sectors], Scale[(S0 + 1) % Sectors], T);
				const int32 Index = Y * SizeX + X;
				double Value = SoftMin(Wanted0.Values[Index] * SectorScale, Peak.Values[Index], CapSoftness);
				if (Crest[Index])
				{
					Value = FMath::Max(Value, Wall + StepMax);
				}
				Wanted.Values[Index] = float(Value);
			}
		}
		Height = BuildGround(Wanted);
		const TArray<double> Built = SectorMeans(Height);
		for (int32 Index = 0; Index < Sectors; ++Index)
		{
			Scale[Index] = FMath::Clamp(Scale[Index] * Target[Index] / FMath::Max(Built[Index], 50.0), 0.5, 4.0);
		}
	}
	// Under the sea: shallow where the painting speckles its teal fringe, deep elsewhere.
	for (int32 Index = 0; Index < Cells; ++Index)
	{
		if (!Land[Index])
		{
			const double S = Shallow.Values[Index];
			Height.Values[Index] = -float(FMath::Min(30.0 + Offshore.Values[Index] * (0.05 + 0.6 * (1.0 - S)), ShelfDepth + (SkirtDepth - ShelfDepth) * (1.0 - S)));
		}
	}

	// No one walks from the islands' sea to the other hemisphere's: steps between cells under WalkLimit join them.
	bool bWalkable = false;
	{
		const double Limit = FMath::Tan(FMath::DegreesToRadians(WalkLimit));
		TArray<int32> Counts;
		const TArray<int32> Water = Label(Not(Land), SizeX, SizeY, Counts);
		TSet<int32> Near, Far;
		for (int32 X = 0; X < SizeX; ++X)
		{
			Near.Add(Water[X]);
			Far.Add(Water[(SizeY - 1) * SizeX + X]);
		}
		Near.Remove(0);
		Far.Remove(0);
		TArray<bool> Reached;
		Reached.Init(false, Cells);
		TArray<int32> Queue;
		for (int32 Index = 0; Index < Cells; ++Index)
		{
			if (Near.Contains(Water[Index]))
			{
				Reached[Index] = true;
				Queue.Add(Index);
			}
		}
		for (int32 Next = 0; Next < Queue.Num() && !bWalkable; ++Next)
		{
			const int32 X = Queue[Next] % SizeX;
			const int32 Y = Queue[Next] / SizeX;
			for (const FIntPoint& Step : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
			{
				// Around the seam: the padding repeats columns, so wrap within the real ones.
				const int32 NX = Pad + ((X + Step.X - Pad) % Columns + Columns) % Columns;
				const int32 NY = Y + Step.Y;
				const int32 N = NY * SizeX + NX;
				if (NY < 0 || NY >= SizeY || Reached[N] || FMath::Abs(Height(NX, NY) - Height(X, Y)) > Limit * (Step.X ? DX : DY))
				{
					continue;
				}
				Reached[N] = true;
				Queue.Add(N);
				bWalkable |= Far.Contains(Water[N]);
			}
		}
	}
	if (bWalkable)
	{
		Context.Warn(FString::Printf(TEXT("Planet %s: the belt can be walked across from the islands' sea to the other hemisphere's"), *Planet));
	}

	// 4. Points on the map (pole at the origin), normals, colors.
	auto PointAt = [&](int32 Column, int32 Row)
	{
		const int32 X = Pad + (Column % Columns + Columns) % Columns;
		const double Longitude = FMath::DegreesToRadians(ColumnLongitude(X) + Jitter * Hash(X - Pad, Row, 1) * 360.0 / Columns);
		const double Rho = Radius * FMath::DegreesToRadians(90.0 - RowLatitude(Row) + Jitter * Hash(X - Pad, Row, 2) * RowDegrees);
		return FVector(FMath::Cos(Longitude) * Rho, FMath::Sin(Longitude) * Rho, Height(X, Row));
	};
	auto NormalAt = [&](int32 Column, int32 Row)
	{
		const FVector Across = PointAt(Column, FMath::Min(Row + 1, SizeY - 1)) - PointAt(Column, FMath::Max(Row - 1, 0));
		const FVector Along = PointAt(Column + 1, Row) - PointAt(Column - 1, Row);
		const FVector Normal = (Across ^ Along).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		return Normal.Z < 0.0 ? -Normal : Normal;
	};
	auto ColorAt = [&](int32 Column, int32 Row, const FVector& Normal)
	{
		const int32 X = Pad + (Column % Columns + Columns) % Columns;
		const int32 Index = Row * SizeX + X;
		const float H = Height.Values[Index];
		if (!Land[Index])
		{
			return Lerp(Shelf, Deep, FMath::Clamp(-H / float(ShelfDepth), 0.f, 1.f));
		}
		const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0, 1.0)));
		const float Steep = FMath::Clamp(float(Slope - 38.0) / 17.f, 0.f, 1.f);
		if (FrozenMask[Index] && Slope < 20.0)
		{
			return Frozen;
		}
		const float Br = FMath::Clamp(BrownShare.Values[Index], 0.f, 1.f);
		const float Gr = FMath::Clamp(GreenShare.Values[Index], 0.f, 1.f) * (1.f - Br);
		const FVector3f Ground = IceColor(FMath::Clamp(Bright.Values[Index], 0.f, 1.f)) * (1.f - Br - Gr) + Brown * Br + Green * Gr;
		const FVector3f Cliff = DarkIce * (1.f - Br) + BrownCliff * Br;
		return Lerp(Ground, Cliff, Steep);
	};

	// A sector per altitude column: its own mesh and actor, around its land's middle; cells all under deep water left out.
	UMaterialInterface* Material = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
	const FName Slot = FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color);
	const FString LevelName = FString::Printf(TEXT("L_%s_Belt"), *Planet);
	UWorld* World = Context.BeginLevel(Folder, LevelName);
	if (!World)
	{
		return nullptr;
	}
	constexpr int32 SectorColumns = Columns / Sectors;
	int32 Triangles = 0;
	for (int32 SectorIndex = 0; SectorIndex < Sectors; ++SectorIndex)
	{
		const int32 First = SectorIndex * SectorColumns;
		const double Angle = 2.0 * UE_DOUBLE_PI * (First + SectorColumns / 2) / Columns;
		const FVector Pivot(FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius * FMath::DegreesToRadians(90.0), 0.0);

		TMap<FIntPoint, FLBAVertex> Vertices;
		auto VertexAt = [&](int32 Column, int32 Row) -> const FLBAVertex&
		{
			if (const FLBAVertex* Found = Vertices.Find(FIntPoint(Column, Row)))
			{
				return *Found;
			}
			const FVector Point = PointAt(Column, Row);
			const FVector Normal = NormalAt(Column, Row);
			const FVector3f Srgb = ColorAt(Column, Row, Normal);
			const FLinearColor Color = FLinearColor::FromSRGBColor(FColor(uint8(Srgb.X + 0.5f), uint8(Srgb.Y + 0.5f), uint8(Srgb.Z + 0.5f)));
			FLBAVertex Vertex;
			Vertex.Position = FVector3f(Point - Pivot);
			Vertex.Normal = FVector3f(Normal);
			Vertex.Color = FVector4f(Color.R, Color.G, Color.B, 0.5f);
			return Vertices.Add(FIntPoint(Column, Row), Vertex);
		};
		FLBAMeshBuilder Builder;
		const int32 Section = Builder.GetSection(Slot, Material);
		for (int32 Column = First; Column < First + SectorColumns; ++Column)
		{
			for (int32 Row = 0; Row + 1 < SizeY; ++Row)
			{
				const float Highest = FMath::Max(FMath::Max(PointAt(Column, Row).Z, PointAt(Column + 1, Row).Z), FMath::Max(PointAt(Column, Row + 1).Z, PointAt(Column + 1, Row + 1).Z));
				if (Highest <= -SkirtDepth + 1.0)
				{
					continue;
				}
				const FLBAVertex A = VertexAt(Column, Row);
				const FLBAVertex B = VertexAt(Column + 1, Row);
				const FLBAVertex C = VertexAt(Column, Row + 1);
				const FLBAVertex D = VertexAt(Column + 1, Row + 1);
				Builder.AddTriangleFacing(Section, A, B, D, FVector3f::UpVector);
				Builder.AddTriangleFacing(Section, A, D, C, FVector3f::UpVector);
			}
		}
		if (Builder.NumTriangles() == 0)
		{
			continue;
		}
		Triangles += Builder.NumTriangles();

		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, Materials, Slots);
		FLBAStaticMeshOptions Options;
		Options.bComplexAsSimple = true;
		Options.bDeferBuild = false;
		UStaticMesh* StaticMesh = Context.CreateStaticMesh(Folder, FString::Printf(TEXT("SM_LBA_Belt_%s_%02d"), *Planet, SectorIndex), MoveTemp(Description), Materials, Slots, Options);
		if (!StaticMesh)
		{
			continue;
		}
		// Bent onto the planet, it goes down around it: culled by where it is seen, not by where it is.
		StaticMesh->SetNegativeBoundsExtension(FVector(0.0, 0.0, 2.0 * Radius));
		StaticMesh->CalculateExtendedBounds();
		Context.MarkForSave(StaticMesh);
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Pivot), FString::Printf(TEXT("Belt_%02d"), SectorIndex), TEXT("Belt")))
		{
			Actor->GetStaticMeshComponent()->SetStaticMesh(StaticMesh);
			Actor->Tags.Add(ULBAPlanetAsset::BeltTag);
		}
	}
	Context.EndLevel(World);

	double LandCells = 0.0, Highest = 0.0, NearestShore = TNumericLimits<double>::Max();
	for (int32 Y = 0; Y < SizeY; ++Y)
	{
		for (int32 X = Pad; X < Pad + Columns; ++X)
		{
			if (Land[Y * SizeX + X])
			{
				LandCells += 1.0;
				Highest = FMath::Max(Highest, double(Height(X, Y)));
				NearestShore = FMath::Min(NearestShore, Radius * FMath::DegreesToRadians(90.0 - RowLatitude(Y)));
				if (OutLand)
				{
					const double Longitude = FMath::DegreesToRadians(ColumnLongitude(X));
					const double Rho = Radius * FMath::DegreesToRadians(90.0 - RowLatitude(Y));
					OutLand->Add(FVector2D(FMath::Cos(Longitude) * Rho, FMath::Sin(Longitude) * Rho));
				}
			}
		}
	}
	const TArray<double> Built = SectorMeans(Height);
	double BuiltMean = 0.0, TargetMean = 0.0;
	for (int32 Index = 0; Index < Sectors; ++Index)
	{
		BuiltMean += Built[Index] / Sectors;
		TargetMean += Target[Index] / Sectors;
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s belt: %.0f m2 of land from %.0f m from the pole, %d benches up to %.0f m (highest %.0f m), mean %.1f m against the holomap's %.1f m, %s, %d triangles"),
		*Planet, LandCells * CellArea / 10000.0, NearestShore / 100.0, NumBenches, Levels.IsEmpty() ? 0.0 : Levels[FMath::Min(NumBenches, Levels.Num() - 1)] / 100.0, Highest / 100.0,
		BuiltMean / 100.0, TargetMean / 100.0, bWalkable ? TEXT("WALKABLE ACROSS") : TEXT("closed to walkers"), Triangles);
	return TSoftObjectPtr<UWorld>(FSoftObjectPath(Folder / LevelName + TEXT(".") + LevelName));
}
