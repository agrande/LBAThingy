#include "Import/LBAPlanetStubs.h"

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

namespace
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
	/** The beach: this high at the coast, rising to BeachTop this far inland, sand colored this far in. */
	constexpr double CoastHeight = 40.0;
	constexpr double BeachTop = 120.0;
	constexpr double BeachRun = 500.0;
	constexpr double SandRun = 400.0;
	/** The hills reach their full height this far inland; the highest point's height grows with the land's area. */
	constexpr double HillRun = 2500.0;
	constexpr double MinPeak = 600.0;
	constexpr double MaxPeak = 2000.0;
	constexpr double MaxHeight = 2400.0;
	/** Under the sea: the shelf falls this steeply from the coast, down to the skirt's depth. */
	constexpr double ShelfSlope = 0.6;
	constexpr double SkirtDepth = 800.0;
	/** The painting's colors lightened by this gamma: the holomap darkens its globe less than the game's light does. */
	constexpr float PaintGamma = 0.75f;

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

	/** How high a painted color is meant to be (0 to 1): sand low, green higher, brown higher still, rock and snow highest. */
	float HeightHint(const FVector3f& Color)
	{
		const FVector3f C = Color / 255.f;
		const float Max = C.GetMax();
		const float Min = C.GetMin();
		const float Lum = (C.X + C.Y + C.Z) / 3.f;
		const float Saturation = (Max - Min) / FMath::Max(Max, 1e-3f);
		const float Sand = FMath::Clamp((C.X - C.Z) * 3.f, 0.f, 1.f) * FMath::Clamp((Lum - 0.45f) * 5.f, 0.f, 1.f);
		const float Green = FMath::Clamp((C.Y - C.X) * 6.f, 0.f, 1.f) * FMath::Clamp((C.Y - C.Z) * 4.f, 0.f, 1.f);
		const float Pale = FMath::Clamp((Lum - 0.45f) * 4.f, 0.f, 1.f) * FMath::Clamp((0.35f - Saturation) * 4.f, 0.f, 1.f)
			+ FMath::Clamp((C.Z - C.X) * 4.f, 0.f, 1.f) * FMath::Clamp((Lum - 0.35f) * 4.f, 0.f, 1.f);
		const float Brown = FMath::Clamp(1.f - Sand - Green - Pale, 0.f, 1.f);
		return (0.15f * Sand + 0.35f * Green + 0.75f * Brown + Pale) / FMath::Max(Sand + Green + Brown + Pale, 1e-3f);
	}

	double Noise2D(const FVector2D& Value)
	{
		return FMath::PerlinNoise2D(Value);
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

		// The island's grid: its painting, its own land (not a neighbour's), the coast's distance, colors and heights.
		const FVector2D Origin = SurveyToMap(Island.Min.X, Island.Min.Y) - FVector2D(SurveyCell / 2.0 + Margin);
		const FVector2D Size = FVector2D(Island.Max - Island.Min + FIntPoint(1)) * SurveyCell + FVector2D(2.0 * Margin);
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
		TArray<bool> Land, Sea;
		Land.SetNumUninitialized(SizeX * SizeY);
		Sea.SetNumUninitialized(SizeX * SizeY);
		FGrid Hint;
		Hint.Init(SizeX, SizeY);
		int32 LandCells = 0;
		for (int32 Index = 0; Index < SizeX * SizeY; ++Index)
		{
			Land[Index] = Soft.Values[Index] * Own.Values[Index] > 0.5f;
			Sea[Index] = !Land[Index];
			LandCells += Land[Index] ? 1 : 0;
			Hint.Values[Index] = HeightHint(FVector3f(Red.Values[Index], Green.Values[Index], Blue.Values[Index]));
		}
		Blur(Hint, 4.0);
		const FGrid Inland = DistanceOutOf(Land, SizeX, SizeY);
		const FGrid Offshore = DistanceOutOf(Sea, SizeX, SizeY);
		const double Area = LandCells * Cell * Cell;
		const double Peak = FMath::Clamp(0.11 * FMath::Sqrt(Area / 10000.0) * 100.0, MinPeak, MaxPeak);

		FGrid Heights;
		Heights.Init(SizeX, SizeY);
		double Highest = 0.0;
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				const int32 Index = Y * SizeX + X;
				const FVector2D Map = CellToMap(X, Y);
				double Height;
				if (Land[Index])
				{
					const double Coast = Inland.Values[Index] * Cell;
					const double Rise = FMath::Pow(FMath::Clamp(Coast / HillRun, 0.0, 1.0), 1.2);
					const double Noise = 0.6 * Noise2D(Map / 1500.0) + 0.3 * Noise2D(Map / 600.0 + FVector2D(3.1, 7.7)) + 0.1 * Noise2D(Map / 250.0 + FVector2D(9.4, 1.6));
					Height = CoastHeight + (BeachTop - CoastHeight) * FMath::Clamp(Coast / BeachRun, 0.0, 1.0)
						+ Rise * Peak * (0.35 + 0.65 * Hint.Values[Index]) * (1.0 + 0.4 * Noise);
					Height = FMath::Min(Height, MaxHeight);
				}
				else
				{
					Height = -FMath::Min(SkirtDepth, 30.0 + ShelfSlope * Offshore.Values[Index] * Cell);
				}
				Heights(X, Y) = float(Height);
				Highest = FMath::Max(Highest, Height);
			}
		}

		// Colors: the painting's (lightened: painted dark for the holomap's own shading), sand along the coast and on the
		// shelf under the sea (pale shallows, like the holomap's lighter rim), bare earth where it is steep.
		const FVector Pivot(Center, 0.0);
		const FVector3f SandColor(214.f, 196.f, 140.f);
		const FVector3f ShelfColor(150.f, 190.f, 170.f);
		const FVector3f EarthColor(118.f, 96.f, 64.f);
		auto VertexAt = [&](int32 X, int32 Y)
		{
			const int32 Index = Y * SizeX + X;
			const FVector2D Map = CellToMap(X, Y);
			const FVector Across(2.0 * Cell, 0.0, Heights.At(X + 1, Y) - Heights.At(X - 1, Y));
			const FVector Along(0.0, 2.0 * Cell, Heights.At(X, Y + 1) - Heights.At(X, Y - 1));
			const FVector Normal = (Across ^ Along).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			const FVector3f Painted(Red.Values[Index], Green.Values[Index], Blue.Values[Index]);
			FVector3f Color(255.f * FMath::Pow(Painted.X / 255.f, PaintGamma), 255.f * FMath::Pow(Painted.Y / 255.f, PaintGamma), 255.f * FMath::Pow(Painted.Z / 255.f, PaintGamma));
			if (Land[Index])
			{
				const float Steep = float(FMath::SmoothStep(35.0, 55.0, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0, 1.0)))));
				Color = FMath::Lerp(Color, EarthColor, Steep);
				Color = FMath::Lerp(SandColor, Color, float(FMath::Clamp(Inland.Values[Index] * Cell / SandRun, 0.0, 1.0)));
			}
			else
			{
				Color = FMath::Lerp(SandColor, ShelfColor, float(FMath::Clamp(Offshore.Values[Index] * Cell / 800.0, 0.0, 1.0)));
			}
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
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s stub %s: %.0f m2 of land, %.0f m high, map (%.0f, %.0f) m, (%.0f, %.0f) in the home island's level, %d triangles"),
			*Planet, *Name, Area / 10000.0, Highest / 100.0, Pivot.X / 100.0, Pivot.Y / 100.0, InHome.X, InHome.Y, Triangles);
	}
	Context.EndLevel(World);
	return TSoftObjectPtr<UWorld>(FSoftObjectPath(Folder / LevelName + TEXT(".") + LevelName));
}
