#include "Import/LBASeabed.h"

#include "Import/LBAImportContext.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBAMaterialLibrary.h"
#include "Math/UnrealMathUtility.h"
#include "StaticMeshAttributes.h"

namespace
{
	/**
	 * Depth the seabed slopes down to (cm), and the distance from the coast where it is about two thirds there: along
	 * low shores, and along steep ones. Shores are steep when the first land vertex is this high (cm).
	 */
	constexpr float MaxDepth = 1000.f;
	constexpr float ShelfLength = 1500.f;
	constexpr float DropLength = 250.f;
	constexpr float LowShore = 40.f;
	constexpr float SteepShore = 160.f;
	/** Seabed vertices every this many terrain vertices. */
	constexpr int32 Step = 2;
	/** Below the terrain's sea-level shores, so it never pokes through them. */
	constexpr float Clearance = 3.f;
	/** Cells deeper than this on all corners are not built (the water hides them anyway). */
	constexpr float Deep = MaxDepth * 0.75f;

	/**
	 * Exact squared Euclidean distance transform of one row (Felzenszwalb and Huttenlocher): Out[I] = min over J of
	 * (I - J)^2 + In[J], and the J reaching it in OutNearest. In is read every Stride values, the outputs are written
	 * contiguously.
	 */
	void DistanceTransform1D(const float* In, float* Out, int32* OutNearest, int32 Count, int32 Stride, TArray<int32>& V, TArray<float>& Z)
	{
		V.SetNumUninitialized(Count);
		Z.SetNumUninitialized(Count + 1);
		// Where the parabolas from Q and P intersect.
		auto Intersect = [In, Stride](int32 Q, int32 P)
		{
			return ((In[Q * Stride] + float(Q) * Q) - (In[P * Stride] + float(P) * P)) / (2.f * (Q - P));
		};
		int32 K = 0;
		V[0] = 0;
		Z[0] = -UE_BIG_NUMBER;
		Z[1] = UE_BIG_NUMBER;
		for (int32 Q = 1; Q < Count; ++Q)
		{
			float S = Intersect(Q, V[K]);
			while (S <= Z[K])
			{
				--K;
				S = Intersect(Q, V[K]);
			}
			++K;
			V[K] = Q;
			Z[K] = S;
			Z[K + 1] = UE_BIG_NUMBER;
		}
		K = 0;
		for (int32 Q = 0; Q < Count; ++Q)
		{
			while (Z[K + 1] < Q)
			{
				++K;
			}
			const int32 P = V[K];
			Out[Q] = float(Q - P) * (Q - P) + In[P * Stride];
			OutNearest[Q] = P;
		}
	}

	/**
	 * Depth below the sea of the seabed at Distance (cm) from the coast, whose first land vertex is ShoreHeight (cm)
	 * high: off low shores a shallow shelf, then deepening; off steep ones deep within a few meters.
	 */
	float SeabedDepth(float Distance, float ShoreHeight, float Noise)
	{
		const float Shelf = FMath::Square(Distance / ShelfLength);
		const float Low = 1.f - FMath::Exp(-Shelf);
		const float Steep = 1.f - FMath::Exp(-Distance / DropLength);
		const float Steepness = FMath::Clamp((ShoreHeight - LowShore) / (SteepShore - LowShore), 0.f, 1.f);
		return MaxDepth * FMath::Lerp(Low, Steep, Steepness) * (0.8f + 0.4f * Noise);
	}
}

UStaticMesh* LBASeabed::Build(FLBAImportContext& Context, const FLBASeabedSettings& Settings, const FString& Folder, const FString& Name)
{
	const FIntRect& R = Settings.Vertices;
	const int32 W = R.Width();
	const int32 H = R.Height();
	if (W < 2 * Step || H < 2 * Step || !Settings.Height)
	{
		return nullptr;
	}

	// Squared distance (in terrain vertices) from every vertex to the nearest land vertex, and which one it is.
	constexpr float Far = 1e12f;
	TArray<float> Heights;
	TArray<float> Distance;
	Heights.SetNumUninitialized(W * H);
	Distance.SetNumUninitialized(W * H);
	bool bAnyLand = false;
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			const float Height = Settings.Height(R.Min.X + X, R.Min.Y + Y);
			Heights[Y * W + X] = Height;
			Distance[Y * W + X] = Height > 0.f ? 0.f : Far;
			bAnyLand |= Height > 0.f;
		}
	}
	if (!bAnyLand)
	{
		return nullptr;
	}
	// Separable: columns (nearest row per column), then rows (nearest column, whose nearest row is already known).
	TArray<int32> NearestRow;
	TArray<int32> Nearest;
	NearestRow.SetNumUninitialized(W * H);
	Nearest.SetNumUninitialized(W * H);
	{
		TArray<float> Temp;
		TArray<int32> TempNearest;
		Temp.SetNumUninitialized(FMath::Max(W, H));
		TempNearest.SetNumUninitialized(FMath::Max(W, H));
		TArray<int32> V;
		TArray<float> Z;
		for (int32 X = 0; X < W; ++X)
		{
			DistanceTransform1D(&Distance[X], Temp.GetData(), TempNearest.GetData(), H, W, V, Z);
			for (int32 Y = 0; Y < H; ++Y)
			{
				Distance[Y * W + X] = Temp[Y];
				NearestRow[Y * W + X] = TempNearest[Y];
			}
		}
		for (int32 Y = 0; Y < H; ++Y)
		{
			DistanceTransform1D(&Distance[Y * W], Temp.GetData(), TempNearest.GetData(), W, 1, V, Z);
			FMemory::Memcpy(&Distance[Y * W], Temp.GetData(), W * sizeof(float));
			for (int32 X = 0; X < W; ++X)
			{
				const int32 Column = TempNearest[X];
				Nearest[Y * W + X] = NearestRow[Y * W + Column] * W + Column;
			}
		}
	}

	// Seabed grid, every Step terrain vertices.
	const int32 GW = (W - 1) / Step + 1;
	const int32 GH = (H - 1) / Step + 1;
	const float Spacing = Settings.VertexSpacing;
	TArray<FVector3f> Positions;
	TArray<float> Depths;
	TArray<float> Shades;
	Positions.SetNumUninitialized(GW * GH);
	Depths.SetNumUninitialized(GW * GH);
	Shades.SetNumUninitialized(GW * GH);
	for (int32 J = 0; J < GH; ++J)
	{
		for (int32 I = 0; I < GW; ++I)
		{
			const int32 X = I * Step;
			const int32 Y = J * Step;
			const FVector2f P(float(R.Min.X + X) * Spacing, float(R.Min.Y + Y) * Spacing);
			const float Noise = 0.5f + 0.5f * FMath::PerlinNoise2D(FVector2D(P) / 900.0);
			const float Depth = SeabedDepth(FMath::Sqrt(Distance[Y * W + X]) * Spacing, Heights[Nearest[Y * W + X]], Noise);
			Depths[J * GW + I] = Depth;
			Positions[J * GW + I] = FVector3f(P.X, P.Y, -Clearance - Depth);
			Shades[J * GW + I] = 0.85f + 0.3f * (0.5f + 0.5f * FMath::PerlinNoise2D(FVector2D(P) / 350.0 + FVector2D(31.7, 11.3)));
		}
	}

	FLBAMeshBuilder Builder;
	const int32 Section = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color), Settings.Material);
	auto Vertex = [&](int32 I, int32 J)
	{
		const int32 Index = J * GW + I;
		const float Left = Depths[J * GW + FMath::Max(I - 1, 0)];
		const float Right = Depths[J * GW + FMath::Min(I + 1, GW - 1)];
		const float Down = Depths[FMath::Max(J - 1, 0) * GW + I];
		const float Up = Depths[FMath::Min(J + 1, GH - 1) * GW + I];
		FLBAVertex V;
		V.Position = Positions[Index];
		V.Normal = FVector3f(Right - Left, Up - Down, 2.f * Step * Spacing).GetSafeNormal();
		const FLinearColor C = Settings.Color * Shades[Index];
		V.Color = FVector4f(C.R, C.G, C.B, 0.5f);
		return V;
	};
	for (int32 J = 0; J + 1 < GH; ++J)
	{
		for (int32 I = 0; I + 1 < GW; ++I)
		{
			const float D[4] = { Depths[J * GW + I], Depths[J * GW + I + 1], Depths[(J + 1) * GW + I], Depths[(J + 1) * GW + I + 1] };
			// Under land (hidden by the terrain) or deep (the water hides it anyway): no seabed.
			const bool bUnderLand = D[0] <= 0.f && D[1] <= 0.f && D[2] <= 0.f && D[3] <= 0.f;
			const bool bDeep = D[0] >= Deep && D[1] >= Deep && D[2] >= Deep && D[3] >= Deep;
			if (bUnderLand || bDeep)
			{
				continue;
			}
			const FLBAVertex A = Vertex(I, J);
			const FLBAVertex B = Vertex(I + 1, J);
			const FLBAVertex C = Vertex(I, J + 1);
			const FLBAVertex E = Vertex(I + 1, J + 1);
			Builder.AddTriangleFacing(Section, A, B, E, FVector3f::UpVector);
			Builder.AddTriangleFacing(Section, A, E, C, FVector3f::UpVector);
		}
	}
	if (Builder.IsEmpty())
	{
		return nullptr;
	}

	FMeshDescription Description;
	TArray<UMaterialInterface*> Materials;
	TArray<FName> Slots;
	Builder.BuildStatic(Description, Materials, Slots);
	FLBAStaticMeshOptions Options;
	Options.bComplexAsSimple = true;
	Options.bDeferBuild = false;
	return Context.CreateStaticMesh(Folder, Name, MoveTemp(Description), Materials, Slots, Options);
}

float LBASeabed::GetCutoffDepth()
{
	return Clearance + Deep;
}
