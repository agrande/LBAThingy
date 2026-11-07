#include "Import/LBASeaRegions.h"

#include "Algo/BinarySearch.h"
#include "Engine/Texture2D.h"
#include "Import/LBAImportContext.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"

namespace
{
	constexpr int32 SeaSize = 128;
	constexpr int32 PageWidth = 256;

	struct FRampColor
	{
		FColor Color;
		float Weight;
	};

	// The oasis river's palette colors, measured in lba2cc's shot of the Oasis (refs/lba2cc-bin/shots/classic_55.png,
	// Desert island's scene 55), weighted by how much of it each covers (4420, 2723, 2092, 1740, 1636, 1010), its greens
	// down to 40 %: shallows brighten the sea's shade rows, and the greens' bright rows are neon (88,224,184). Its
	// highlight (96,200,200) is left to the shallows too: from it they clipped to (145,255,255) on screen.
	const FRampColor OasisRamp[] =
	{
		{ FColor(36, 116, 100), 404.f }, { FColor(52, 132, 112), 654.f }, { FColor(56, 144, 120), 1089.f }, { FColor(44, 144, 152), 4420.f },
		{ FColor(64, 164, 136), 696.f }, { FColor(68, 172, 176), 2092.f },
	};
	// The palette's own sea bank paling to white, as the holomap's fringe along the Hamalayi: light blue (159, 199, 227),
	// then white (187, 223, 227). The palette's lilac whites (its shade rows go lilac) are left out.
	const FRampColor IceRamp[] =
	{
		{ FColor(128, 180, 212), 20.f }, { FColor(156, 196, 224), 35.f }, { FColor(188, 216, 236), 35.f }, { FColor(252, 252, 252), 10.f },
	};

	FVector3f ToLinear(const FColor& Color)
	{
		return FVector3f(FMath::Pow(Color.R / 255.f, 2.2f), FMath::Pow(Color.G / 255.f, 2.2f), FMath::Pow(Color.B / 255.f, 2.2f));
	}

	float Luma(const FColor& Color)
	{
		const FVector3f Linear = ToLinear(Color);
		return 0.2126f * Linear.X + 0.7152f * Linear.Y + 0.0722f * Linear.Z;
	}

	/** The palette color nearest to an sRGB color (weighted, as the eye: green most), never the color key 0. */
	int32 Nearest(const TArray<FColor>& Palette, const FVector3f& Srgb)
	{
		int32 Best = 1;
		float BestDistance = TNumericLimits<float>::Max();
		for (int32 Index = 1; Index < Palette.Num(); ++Index)
		{
			const FColor& C = Palette[Index];
			const float Distance = 2.f * FMath::Square(C.R - Srgb.X) + 4.f * FMath::Square(C.G - Srgb.Y) + 3.f * FMath::Square(C.B - Srgb.Z);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Index;
			}
		}
		return Best;
	}

	/**
	 * Squared distances along a line (Felzenszwalb and Huttenlocher), in cells: F in (0 on what is measured from, huge
	 * elsewhere), D out.
	 */
	void DistanceLine(const TArray<double>& F, TArray<double>& D, TArray<int32>& V, TArray<double>& Z)
	{
		const int32 N = F.Num();
		D.SetNumUninitialized(N);
		V.SetNumUninitialized(N);
		Z.SetNumUninitialized(N + 1);
		int32 K = 0;
		V[0] = 0;
		Z[0] = -TNumericLimits<double>::Max();
		Z[1] = TNumericLimits<double>::Max();
		for (int32 Q = 1; Q < N; ++Q)
		{
			double S = ((F[Q] + double(Q) * Q) - (F[V[K]] + double(V[K]) * V[K])) / (2.0 * Q - 2.0 * V[K]);
			while (S <= Z[K])
			{
				--K;
				S = ((F[Q] + double(Q) * Q) - (F[V[K]] + double(V[K]) * V[K])) / (2.0 * Q - 2.0 * V[K]);
			}
			++K;
			V[K] = Q;
			Z[K] = S;
			Z[K + 1] = TNumericLimits<double>::Max();
		}
		K = 0;
		for (int32 Q = 0; Q < N; ++Q)
		{
			while (Z[K + 1] < Q)
			{
				++K;
			}
			D[Q] = FMath::Square(double(Q - V[K])) + F[V[K]];
		}
	}

	/** Distance (cells) of every cell of a Size x Size grid to the nearest set cell; huge without any. */
	TArray<double> DistanceTo(const TArray<bool>& Set, int32 Size)
	{
		constexpr double Far = 1e12;
		TArray<double> Grid;
		Grid.SetNumUninitialized(Size * Size);
		for (int32 Index = 0; Index < Grid.Num(); ++Index)
		{
			Grid[Index] = Set[Index] ? 0.0 : Far;
		}
		TArray<double> F, D, Z;
		TArray<int32> V;
		F.SetNumUninitialized(Size);
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			for (int32 Line = 0; Line < Size; ++Line)
			{
				auto At = [&](int32 I) -> double& { return Pass == 0 ? Grid[Line * Size + I] : Grid[I * Size + Line]; };
				for (int32 I = 0; I < Size; ++I)
				{
					F[I] = At(I);
				}
				DistanceLine(F, D, V, Z);
				for (int32 I = 0; I < Size; ++I)
				{
					At(I) = D[I];
				}
			}
		}
		for (double& Value : Grid)
		{
			Value = FMath::Sqrt(Value);
		}
		return Grid;
	}

	/** Fills the cells whose centers are inside a closed polygon (map cm). */
	void FillPolygon(const TArray<FVector2D>& Polygon, int32 Size, double Extent, TArray<bool>& Out)
	{
		const double Cell = 2.0 * Extent / Size;
		TArray<double> Crossings;
		for (int32 Y = 0; Y < Size; ++Y)
		{
			const double CY = -Extent + (Y + 0.5) * Cell;
			Crossings.Reset();
			for (int32 Index = 0; Index < Polygon.Num(); ++Index)
			{
				const FVector2D& A = Polygon[Index];
				const FVector2D& B = Polygon[(Index + 1) % Polygon.Num()];
				if ((A.Y <= CY) != (B.Y <= CY))
				{
					Crossings.Add(A.X + (CY - A.Y) * (B.X - A.X) / (B.Y - A.Y));
				}
			}
			Crossings.Sort();
			for (int32 Pair = 0; Pair + 1 < Crossings.Num(); Pair += 2)
			{
				const int32 First = FMath::Max(FMath::CeilToInt32((Crossings[Pair] + Extent) / Cell - 0.5), 0);
				const int32 Last = FMath::Min(FMath::FloorToInt32((Crossings[Pair + 1] + Extent) / Cell - 0.5), Size - 1);
				for (int32 X = First; X <= Last; ++X)
				{
					Out[Y * Size + X] = true;
				}
			}
		}
	}
}

UTexture2D* LBASeaRegions::BuildAtlas(FLBAImportContext& Context, const FString& Folder, const FString& Island, const TArray<uint8>& SeaPage,
	const TArray<FColor>& Palette)
{
	if (SeaPage.Num() < PageWidth * SeaSize || Palette.Num() < 256)
	{
		return nullptr;
	}
	// The sea's colors, darkest first, and where each stands among its texels (the middle of its share).
	int32 Counts[256] = {};
	for (int32 Y = 0; Y < SeaSize; ++Y)
	{
		for (int32 X = 0; X < SeaSize; ++X)
		{
			++Counts[SeaPage[Y * PageWidth + X]];
		}
	}
	TArray<int32> Used;
	int32 MostCommon = 0;
	for (int32 Index = 0; Index < 256; ++Index)
	{
		if (Counts[Index] > 0)
		{
			Used.Add(Index);
		}
		MostCommon = Counts[Index] > Counts[MostCommon] ? Index : MostCommon;
	}
	Used.Sort([&Palette](int32 A, int32 B) { return Luma(Palette[A]) < Luma(Palette[B]); });
	TMap<int32, float> Rank;
	float Below = 0.f;
	for (int32 Index : Used)
	{
		Rank.Add(Index, (Below + 0.5f * Counts[Index]) / float(SeaSize * SeaSize));
		Below += Counts[Index];
	}

	// Remaps[Slot][palette index]: ice steps, then turquoise steps.
	TArray<TArray<uint8>> Remaps;
	FString Summary;
	for (int32 Region = 0; Region < 2; ++Region)
	{
		const bool bIce = Region == 0;
		TArray<FRampColor> Ramp = bIce ? TArray<FRampColor>(IceRamp, UE_ARRAY_COUNT(IceRamp)) : TArray<FRampColor>(OasisRamp, UE_ARRAY_COUNT(OasisRamp));
		Ramp.Sort([](const FRampColor& A, const FRampColor& B) { return Luma(A.Color) < Luma(B.Color); });
		TArray<float> Cumulative;
		float Total = 0.f;
		for (const FRampColor& Entry : Ramp)
		{
			Total += Entry.Weight;
			Cumulative.Add(Total);
		}
		TMap<int32, int32> Target;
		for (int32 Index : Used)
		{
			const int32 Entry = FMath::Min(Algo::LowerBound(Cumulative, Rank[Index] * Total), Ramp.Num() - 1);
			const FColor& C = Ramp[Entry].Color;
			Target.Add(Index, Nearest(Palette, FVector3f(C.R, C.G, C.B)));
		}
		// Ice: up a chain of the sea's main bank of 16 colors (others, like a teal, would tint the steps) and the targets.
		TArray<int32> Chain;
		for (int32 Index : Used)
		{
			if (Index / 16 == MostCommon / 16)
			{
				Chain.AddUnique(Index);
			}
		}
		for (const TPair<int32, int32>& Pair : Target)
		{
			Chain.AddUnique(Pair.Value);
		}
		Chain.Sort([&Palette](int32 A, int32 B) { return Luma(Palette[A]) < Luma(Palette[B]); });
		TArray<float> ChainLuma;
		for (int32 Index : Chain)
		{
			ChainLuma.Add(Luma(Palette[Index]));
		}

		for (int32 Step = 1; Step <= Steps; ++Step)
		{
			const float Share = float(Step) / Steps;
			TArray<uint8>& Remap = Remaps.AddDefaulted_GetRef();
			Remap.SetNumUninitialized(256);
			for (int32 Index = 0; Index < 256; ++Index)
			{
				Remap[Index] = uint8(Index);
			}
			for (int32 Index : Used)
			{
				if (bIce)
				{
					const int32 InChain = Chain.Find(Index);
					const int32 From = InChain != INDEX_NONE ? InChain : FMath::Min(Algo::LowerBound(ChainLuma, Luma(Palette[Index])), Chain.Num() - 1);
					const int32 To = Chain.Find(Target[Index]);
					Remap[Index] = uint8(Chain[FMath::RoundToInt32(FMath::Lerp(float(From), float(To), Share))]);
				}
				else
				{
					// In linear light, then back to sRGB.
					const FVector3f Blend = FMath::Lerp(ToLinear(Palette[Index]), ToLinear(Palette[Target[Index]]), Share);
					const FVector3f Srgb(255.f * FMath::Pow(Blend.X, 1.f / 2.2f), 255.f * FMath::Pow(Blend.Y, 1.f / 2.2f), 255.f * FMath::Pow(Blend.Z, 1.f / 2.2f));
					Remap[Index] = uint8(Nearest(Palette, Srgb));
				}
			}
		}
		Summary += bIce ? TEXT(" ice:") : TEXT("; turquoise:");
		for (int32 Index : Used)
		{
			Summary += FString::Printf(TEXT(" %d->%d"), Index, Remaps.Last()[Index]);
		}
	}

	UTexture2D* Texture = Context.CreatePaletteTexture(Folder, FString::Printf(TEXT("T_%s_SeaRegions"), *Island), SeaSize, SeaSize * Remaps.Num(),
		[&](int32 X, int32 Y) { return Remaps[Y / SeaSize][SeaPage[(Y % SeaSize) * PageWidth + X]]; }, false, true);
	if (Texture)
	{
		// Read texel by texel like the sea texture, every mip: kept resident, each mip's slots its own (2 x 2 averages).
		Texture->PreEditChange(nullptr);
		Texture->NeverStream = true;
		Texture->MipGenSettings = TMGS_SimpleAverage;
		Texture->PostEditChange();
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2 sea regions %s: %d sea colors,%s"), *Island, Used.Num(), *Summary);
	return Texture;
}

UTexture2D* LBASeaRegions::BuildMask(FLBAImportContext& Context, const FString& Planet, const ULBAPlanetAsset& Asset, const TArray<FVector2D>& BeltLand,
	const TArray<FName>& TurquoiseIslands, const FString& Folder)
{
	const int32 Size = MaskSize;
	const double Extent = MaskExtent;
	const double Cell = 2.0 * Extent / Size;
	TArray<bool> Belt, Turquoise, Others;
	Belt.Init(false, Size * Size);
	Turquoise.Init(false, Size * Size);
	Others.Init(false, Size * Size);

	// The belt's land cells (a couple of meters apart), each spread over the texels around it so no gaps remain.
	for (const FVector2D& Point : BeltLand)
	{
		const int32 X = FMath::FloorToInt32((Point.X + Extent) / Cell);
		const int32 Y = FMath::FloorToInt32((Point.Y + Extent) / Cell);
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				if (X + DX >= 0 && X + DX < Size && Y + DY >= 0 && Y + DY < Size)
				{
					Belt[(Y + DY) * Size + X + DX] = true;
				}
			}
		}
	}
	int32 NumTurquoise = 0;
	for (const FLBAPlanetIsland& Island : Asset.Islands)
	{
		if (Island.Coastline.Num() < 3)
		{
			Context.Warn(FString::Printf(TEXT("Planet %s sea regions: %s has no coastline yet (import it again)"), *Planet, *Island.Island.ToString()));
			continue;
		}
		const FTransform LevelToMap = Island.GetLevelToMap(Asset.Radius);
		TArray<FVector2D> Polygon;
		for (const FVector2D& Point : Island.Coastline)
		{
			Polygon.Add(FVector2D(LevelToMap.TransformPosition(FVector(Point, 0.0))));
		}
		const bool bTurquoise = TurquoiseIslands.Contains(Island.Island);
		NumTurquoise += bTurquoise ? 1 : 0;
		FillPolygon(Polygon, Size, Extent, bTurquoise ? Turquoise : Others);
	}
	if (BeltLand.IsEmpty() && NumTurquoise == 0)
	{
		return nullptr;
	}

	const TArray<double> FromBelt = DistanceTo(Belt, Size);
	const TArray<double> FromTurquoise = DistanceTo(Turquoise, Size);
	const TArray<double> FromOthers = DistanceTo(Others, Size);
	auto Meters = [Cell](double Cells) { return uint8(FMath::Clamp(FMath::RoundToInt32(Cells * Cell / 100.0), 0, 255)); };
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(Size * Size);
	double Strait = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		Pixels[Index] = FColor(Meters(FromBelt[Index]), Meters(FromTurquoise[Index]), Meters(FromOthers[Index]), 255);
		if (!Turquoise[Index] && !Others[Index] && FMath::Abs(FromTurquoise[Index] - FromOthers[Index]) <= 1.0)
		{
			// Where the turquoise islands' sea meets the others': the narrowest such sea.
			Strait = FMath::Min(Strait, FromTurquoise[Index] + FromOthers[Index]);
		}
	}
	UTexture2D* Texture = Context.CreateTexture(Folder, FString::Printf(TEXT("T_LBA_SeaRegions_%s"), *Planet), Size, Size, Pixels, false, false);
	if (Texture)
	{
		// Filtered, its mips for far sea (distances average well), always resident.
		Texture->PreEditChange(nullptr);
		Texture->MipGenSettings = TMGS_SimpleAverage;
		Texture->Filter = TF_Bilinear;
		Texture->NeverStream = true;
		Texture->PostEditChange();
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s sea regions: %d x %d texels of %.2f m over %.0f m around the pole, %d belt land points, %d turquoise islands, %.0f m of sea between them and the others"),
		*Planet, Size, Size, Cell / 100.0, 2.0 * Extent / 100.0, BeltLand.Num(), NumTurquoise, Strait < 1e9 ? Strait * Cell / 100.0 : -1.0);
	return Texture;
}
