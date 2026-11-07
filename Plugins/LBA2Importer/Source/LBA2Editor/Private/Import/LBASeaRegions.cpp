#include "Import/LBASeaRegions.h"

#include "Algo/BinarySearch.h"
#include "Engine/Texture2D.h"
#include "Import/LBAImportContext.h"
#include "LBA2Runtime.h"

namespace
{
	constexpr int32 SeaSize = 128;
	constexpr int32 PageWidth = 256;

	struct FRampColor
	{
		FColor Color;
		float Weight;
	};

	// The oasis river's main color, measured in lba2cc's shot of the Oasis (refs/lba2cc-bin/shots/classic_55.png, Desert
	// island's scene 55): a third of its water (4420 pixels), the rest its darker and lighter teals and greens.
	const FColor OasisWater(44, 144, 152);
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

	auto Identity = []()
	{
		TArray<uint8> Remap;
		Remap.SetNumUninitialized(256);
		for (int32 Index = 0; Index < 256; ++Index)
		{
			Remap[Index] = uint8(Index);
		}
		return Remap;
	};

	// Remaps[Slot][palette index]: the ice steps, then turquoise.
	TArray<TArray<uint8>> Remaps;
	FString Summary = TEXT(" ice:");
	{
		// Each color by its brightness rank onto the ice ramp.
		TArray<FRampColor> Ramp(IceRamp, UE_ARRAY_COUNT(IceRamp));
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
		// Up a chain of the sea's main bank of 16 colors (others, like a teal, would tint the steps) and the targets.
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
		for (int32 StepIndex = 1; StepIndex <= IceSteps; ++StepIndex)
		{
			const float Share = float(StepIndex) / IceSteps;
			TArray<uint8>& Remap = Remaps.Add_GetRef(Identity());
			for (int32 Index : Used)
			{
				const int32 InChain = Chain.Find(Index);
				const int32 From = InChain != INDEX_NONE ? InChain : FMath::Min(Algo::LowerBound(ChainLuma, Luma(Palette[Index])), Chain.Num() - 1);
				const int32 To = Chain.Find(Target[Index]);
				Remap[Index] = uint8(Chain[FMath::RoundToInt32(FMath::Lerp(float(From), float(To), Share))]);
			}
		}
		for (int32 Index : Used)
		{
			Summary += FString::Printf(TEXT(" %d->%d"), Index, Remaps.Last()[Index]);
		}
	}
	// Turquoise: the sea's main tone (about 40 % of its texels) turns to the oasis water's, its darker and lighter tones
	// stay blue. The pattern stays and the water goes about a third of the way to the oasis's color. All its colors to
	// the oasis's teals went too strong, and a blend toward the oasis's colors snapped the darker blues to its greens.
	const int32 Oasis = Nearest(Palette, FVector3f(OasisWater.R, OasisWater.G, OasisWater.B));
	Remaps.Add_GetRef(Identity())[MostCommon] = uint8(Oasis);
	Summary += FString::Printf(TEXT("; turquoise: %d->%d (%.0f %% of the texels)"), MostCommon, Oasis, 100.f * Counts[MostCommon] / float(SeaSize * SeaSize));

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
