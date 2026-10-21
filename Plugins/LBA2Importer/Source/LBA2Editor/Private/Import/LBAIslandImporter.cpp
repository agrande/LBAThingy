#include "Import/LBAIslandImporter.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Formats/LBABody.h"
#include "Formats/LBAIsland.h"
#include "Formats/LBAXpl.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBASeabed.h"
#include "Import/LBAWater.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBATypes.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialEditingLibrary.h"

#define LOCTEXT_NAMESPACE "LBAIslandImporter"

namespace
{
	constexpr int32 CellsPerCube = 64;

	/**
	 * Shade row that sunlit ground should get, from the island's baked terrain lighting: halfway between the most
	 * common row of land vertices (flat ground) and their 90th percentile (slopes facing the sun), which matched
	 * screenshots of the original best. 0 when the island has no land.
	 */
	float GetSunlitGroundRow(const FLBAIsland& Island)
	{
		int32 RowCounts[16] = {};
		for (const FLBAIslandSection& Section : Island.Sections)
		{
			for (int32 Z = 0; Z < 65; ++Z)
			{
				for (int32 X = 0; X < 65; ++X)
				{
					if (Section.GetHeight(X, Z) > 0)
					{
						++RowCounts[Section.GetShadeRow(X, Z)];
					}
				}
			}
		}
		int32 MostCommonRow = 0;
		int32 Total = 0;
		for (int32 Row = 0; Row < 16; ++Row)
		{
			MostCommonRow = RowCounts[Row] > RowCounts[MostCommonRow] ? Row : MostCommonRow;
			Total += RowCounts[Row];
		}
		int32 Row90 = 0;
		for (int32 Count = RowCounts[0]; Row90 < 15 && Count < Total * 9 / 10; Count += RowCounts[++Row90])
		{
		}
		return Total > 0 ? 0.5f * float(MostCommonRow + Row90) : 0.f;
	}

	int32 FloorDiv(int32 A, int32 B)
	{
		return A >= 0 ? A / B : -((-A + B - 1) / B);
	}

	/** Height of a ground vertex in island-global vertex coordinates (sea level outside of sections). */
	int32 GlobalHeight(const FLBAIsland& Island, int32 GX, int32 GZ)
	{
		const int32 CX = FloorDiv(GX, CellsPerCube);
		const int32 CZ = FloorDiv(GZ, CellsPerCube);
		if (const FLBAIslandSection* Section = Island.FindSection(CX, CZ))
		{
			return Section->GetHeight(GX - CX * CellsPerCube, GZ - CZ * CellsPerCube);
		}
		return 0;
	}

	/** What the ground mesh makes of a terrain triangle. */
	enum class EGroundTriangle : uint8
	{
		None,
		Land,
		/** Liquid at sea level: left to the level's water. */
		Water,
	};

	EGroundTriangle ClassifyGroundTriangle(const FLBAIslandSection& Section, int32 X, int32 Z, int32 TriIndex)
	{
		const int32 Cell = (Z * CellsPerCube + X) * 2;
		const FLBAGroundTriangle& Tri = Section.Triangles[Cell + TriIndex];
		if (!Tri.IsVisible())
		{
			return EGroundTriangle::None;
		}
		FIntPoint Corners[3];
		FLBAIsland::GetTriangleCorners(Section.Triangles[Cell].Sens(), TriIndex, Corners);
		bool bSeaLevel = true;
		for (const FIntPoint& Corner : Corners)
		{
			bSeaLevel &= Section.GetHeight(X + Corner.X, Z + Corner.Y) == 0;
		}
		return bSeaLevel && Tri.CodeJeu() != 0 ? EGroundTriangle::Water : EGroundTriangle::Land;
	}

	/** Douglas-Peucker on Points[First..Last] (indices modulo Num): marks the points to keep. */
	void SimplifyPolyline(const TArray<FVector2D>& Points, int32 First, int32 Last, double Tolerance, TArray<bool>& Keep)
	{
		const int32 Num = Points.Num();
		const FVector2D A = Points[First % Num];
		const FVector2D B = Points[Last % Num];
		const FVector2D AB = B - A;
		const double Length = AB.Size();
		double Farthest = 0.0;
		int32 FarthestIndex = INDEX_NONE;
		for (int32 Index = First + 1; Index < Last; ++Index)
		{
			const FVector2D AP = Points[Index % Num] - A;
			const double Distance = Length > 0.0 ? FMath::Abs(FVector2D::CrossProduct(AB, AP)) / Length : AP.Size();
			if (Distance > Farthest)
			{
				Farthest = Distance;
				FarthestIndex = Index;
			}
		}
		if (FarthestIndex != INDEX_NONE && Farthest > Tolerance)
		{
			Keep[FarthestIndex % Num] = true;
			SimplifyPolyline(Points, First, FarthestIndex, Tolerance, Keep);
			SimplifyPolyline(Points, FarthestIndex, Last, Tolerance, Keep);
		}
	}

	bool IsSimplePolygon(const TArray<FVector2D>& Points)
	{
		const int32 Num = Points.Num();
		for (int32 I = 0; I < Num; ++I)
		{
			const FVector2D& A = Points[I];
			const FVector2D& B = Points[(I + 1) % Num];
			// Segments sharing a vertex with this one meet it there, and only there when the polygon is simple.
			for (int32 J = I + 2; J < Num; ++J)
			{
				if (I == 0 && J == Num - 1)
				{
					continue;
				}
				FVector Intersection;
				if (FMath::SegmentIntersection2D(FVector(A, 0.0), FVector(B, 0.0), FVector(Points[J], 0.0), FVector(Points[(J + 1) % Num], 0.0), Intersection))
				{
					return false;
				}
			}
		}
		return true;
	}

	/**
	 * Coastline of the island's largest land mass (world cm, closed loop), for Unreal's ocean, which leaves its inside
	 * dry: the outline of the ground the importer keeps, a cell inside it so the water still laps the shores. Land at
	 * sea level (dry riverbeds, flats) stays out of the sea, which would otherwise cover it. Sea-level water enclosed by
	 * land (basins, ponds) stays wet through a narrow channel to the sea; other holes in the ground count as land.
	 * Empty when the island has no land.
	 */
	TArray<FVector2D> TraceCoastline(const FLBAIsland& Island, double Spacing)
	{
		if (Island.Sections.IsEmpty())
		{
			return {};
		}
		FIntRect Cubes(MAX_int32, MAX_int32, MIN_int32, MIN_int32);
		for (const FLBAIslandSection& Section : Island.Sections)
		{
			Cubes.Include(FIntPoint(Section.CubeX, Section.CubeZ));
		}
		// Global cells, with a margin of open sea all around.
		constexpr int32 Pad = 2;
		const FIntPoint Origin = Cubes.Min * CellsPerCube - FIntPoint(Pad);
		const int32 W = (Cubes.Width() + 1) * CellsPerCube + 2 * Pad;
		const int32 H = (Cubes.Height() + 1) * CellsPerCube + 2 * Pad;
		TArray<bool> Land;
		TArray<bool> Water;
		Land.SetNumZeroed(W * H);
		Water.SetNumZeroed(W * H);
		for (const FLBAIslandSection& Section : Island.Sections)
		{
			for (int32 Z = 0; Z < CellsPerCube; ++Z)
			{
				for (int32 X = 0; X < CellsPerCube; ++X)
				{
					const int32 Index = (Section.CubeZ * CellsPerCube + Z - Origin.Y) * W + Section.CubeX * CellsPerCube + X - Origin.X;
					for (int32 TriIndex = 0; TriIndex < 2; ++TriIndex)
					{
						const EGroundTriangle Type = ClassifyGroundTriangle(Section, X, Z, TriIndex);
						Land[Index] |= Type == EGroundTriangle::Land;
						Water[Index] |= Type == EGroundTriangle::Water;
					}
				}
			}
		}

		// 4-connected flood fill from Seeds through the cells Passable accepts, marking Reached; returns the cells reached.
		const int32 Offsets[4] = { 1, -1, W, -W };
		auto Flood = [&](TArray<int32> Seeds, TFunctionRef<bool(int32)> Passable, TArray<bool>& Reached)
		{
			TArray<int32> Cells;
			for (int32 Seed : Seeds)
			{
				if (!Reached[Seed] && Passable(Seed))
				{
					Reached[Seed] = true;
					Cells.Add(Seed);
				}
			}
			for (int32 Next = 0; Next < Cells.Num(); ++Next)
			{
				const int32 Cell = Cells[Next];
				const int32 X = Cell % W;
				for (int32 Dir = 0; Dir < 4; ++Dir)
				{
					const int32 Neighbor = Cell + Offsets[Dir];
					if ((Dir == 0 && X == W - 1) || (Dir == 1 && X == 0) || Neighbor < 0 || Neighbor >= W * H)
					{
						continue;
					}
					if (!Reached[Neighbor] && Passable(Neighbor))
					{
						Reached[Neighbor] = true;
						Cells.Add(Neighbor);
					}
				}
			}
			return Cells;
		};
		auto FloodOutside = [&](const TArray<bool>& Solid)
		{
			TArray<int32> Border;
			for (int32 X = 0; X < W; ++X)
			{
				Border.Add(X);
				Border.Add((H - 1) * W + X);
			}
			for (int32 Y = 0; Y < H; ++Y)
			{
				Border.Add(Y * W);
				Border.Add(Y * W + W - 1);
			}
			TArray<bool> Outside;
			Outside.SetNumZeroed(W * H);
			Flood(Border, [&Solid](int32 Cell) { return !Solid[Cell]; }, Outside);
			return Outside;
		};

		// Holes in the land: water ones get a channel to the sea (the shortest way through land), others are filled.
		TArray<bool> Outside = FloodOutside(Land);
		for (int32 Start = 0; Start < W * H; ++Start)
		{
			if (Land[Start] || Outside[Start])
			{
				continue;
			}
			TArray<bool> InHole;
			InHole.SetNumZeroed(W * H);
			const TArray<int32> Hole = Flood({ Start }, [&Land](int32 Cell) { return !Land[Cell]; }, InHole);
			if (!Hole.ContainsByPredicate([&Water](int32 Cell) { return Water[Cell]; }))
			{
				for (int32 Cell : Hole)
				{
					Land[Cell] = true;
				}
				continue;
			}
			TArray<int32> Parent;
			Parent.Init(INDEX_NONE, W * H);
			TArray<bool> Visited = InHole;
			TArray<int32> Queue = Hole;
			int32 Exit = INDEX_NONE;
			for (int32 Next = 0; Next < Queue.Num() && Exit == INDEX_NONE; ++Next)
			{
				const int32 Cell = Queue[Next];
				const int32 X = Cell % W;
				for (int32 Dir = 0; Dir < 4 && Exit == INDEX_NONE; ++Dir)
				{
					const int32 Neighbor = Cell + Offsets[Dir];
					if ((Dir == 0 && X == W - 1) || (Dir == 1 && X == 0) || Neighbor < 0 || Neighbor >= W * H || Visited[Neighbor])
					{
						continue;
					}
					Visited[Neighbor] = true;
					Parent[Neighbor] = Cell;
					Exit = Outside[Neighbor] ? Neighbor : INDEX_NONE;
					Queue.Add(Neighbor);
				}
			}
			for (int32 Cell = Exit; Cell != INDEX_NONE; Cell = Parent[Cell])
			{
				Land[Cell] = false;
			}
			for (int32 Cell : Hole)
			{
				Outside[Cell] = true;
			}
			Outside = FloodOutside(Land);
		}

		// A cell inside the shores.
		TArray<bool> Inner;
		Inner.SetNumZeroed(W * H);
		for (int32 Y = 1; Y < H - 1; ++Y)
		{
			for (int32 X = 1; X < W - 1; ++X)
			{
				bool bInner = true;
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						bInner &= Land[(Y + DY) * W + X + DX];
					}
				}
				Inner[Y * W + X] = bInner;
			}
		}

		// The largest land mass, without holes nor cells touching only diagonally (the outline must be a simple
		// polygon): drop one cell of each diagonal pair until there are none.
		TArray<bool> Mass;
		for (bool bChanged = true; bChanged;)
		{
			TArray<bool> Labeled;
			Labeled.SetNumZeroed(W * H);
			TArray<int32> Largest;
			for (int32 Start = 0; Start < W * H; ++Start)
			{
				if (Inner[Start] && !Labeled[Start])
				{
					TArray<int32> Cells = Flood({ Start }, [&Inner](int32 Cell) { return Inner[Cell]; }, Labeled);
					if (Cells.Num() > Largest.Num())
				{
					Largest = MoveTemp(Cells);
				}
				}
			}
			if (Largest.IsEmpty())
			{
				return {};
			}
			Mass.Init(false, W * H);
			for (int32 Cell : Largest)
			{
				Mass[Cell] = true;
			}
			const TArray<bool> Around = FloodOutside(Mass);
			bChanged = false;
			for (int32 Cell = 0; Cell < W * H; ++Cell)
			{
				Mass[Cell] = !Around[Cell];
			}
			for (int32 Y = 0; Y + 1 < H; ++Y)
			{
				for (int32 X = 0; X + 1 < W; ++X)
				{
					const bool A = Mass[Y * W + X];
					const bool B = Mass[Y * W + X + 1];
					const bool C = Mass[(Y + 1) * W + X];
					const bool D = Mass[(Y + 1) * W + X + 1];
					if (A == D && B == C && A != B)
					{
						Mass[A ? Y * W + X : Y * W + X + 1] = false;
						bChanged = true;
					}
				}
			}
			Inner = Mass;
		}

		// Outline: the cell edges with the land mass on their left, chained (each corner starts exactly one).
		TMap<FIntPoint, FIntPoint> Edges;
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				if (!Mass[Y * W + X])
				{
					continue;
				}
				if (!Mass[(Y - 1) * W + X])
				{
					Edges.Add(FIntPoint(X, Y), FIntPoint(X + 1, Y));
				}
				if (!Mass[Y * W + X + 1])
				{
					Edges.Add(FIntPoint(X + 1, Y), FIntPoint(X + 1, Y + 1));
				}
				if (!Mass[(Y + 1) * W + X])
				{
					Edges.Add(FIntPoint(X + 1, Y + 1), FIntPoint(X, Y + 1));
				}
				if (!Mass[Y * W + X - 1])
				{
					Edges.Add(FIntPoint(X, Y + 1), FIntPoint(X, Y));
				}
			}
		}
		TArray<FVector2D> Outline;
		const FIntPoint First = Edges.CreateConstIterator().Key();
		FIntPoint Corner = First;
		do
		{
			const FIntPoint* Next = Edges.Find(Corner);
			if (!Next)
			{
				return {};
			}
			Outline.Add(FVector2D(Origin + Corner) * Spacing);
			Corner = *Next;
		}
		while (Corner != First && Outline.Num() <= Edges.Num());

		// Fewer points: within a third of a cell of the outline, unless that makes it cross itself.
		const int32 Num = Outline.Num();
		int32 Opposite = 0;
		for (int32 Index = 1; Index < Num; ++Index)
		{
			Opposite = FVector2D::DistSquared(Outline[Index], Outline[0]) > FVector2D::DistSquared(Outline[Opposite], Outline[0]) ? Index : Opposite;
		}
		TArray<bool> Keep;
		Keep.Init(false, Num);
		Keep[0] = Keep[Opposite] = true;
		SimplifyPolyline(Outline, 0, Opposite, Spacing / 3.0, Keep);
		SimplifyPolyline(Outline, Opposite, Num, Spacing / 3.0, Keep);
		TArray<FVector2D> Simplified;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (Keep[Index])
			{
				Simplified.Add(Outline[Index]);
			}
		}
		const bool bSimple = IsSimplePolygon(Simplified);
		UE_LOG(LogLBA2, Display, TEXT("LBA2 coastline %s: %d points (%d before simplifying%s)"), *Island.Name, bSimple ? Simplified.Num() : Num, Num,
			bSimple ? TEXT("") : TEXT(", which crossed itself"));
		return bSimple ? Simplified : Outline;
	}
}

namespace
{
	/**
	 * Average color (linear) of the island's shores: the terrain triangles that come down to sea level, textured or
	 * colored. The seabed takes it, so the shallows look like the beaches and rocks around them.
	 */
	FLinearColor GetShoreColor(const FLBAIsland& Island, const TArray<FColor>& Palette)
	{
		FLinearColor Sum = FLinearColor::Black;
		int32 Count = 0;
		for (const FLBAIslandSection& Section : Island.Sections)
		{
			for (int32 Z = 0; Z < CellsPerCube; ++Z)
			{
				for (int32 X = 0; X < CellsPerCube; ++X)
				{
					const int32 Cell = (Z * CellsPerCube + X) * 2;
					for (int32 TriIndex = 0; TriIndex < 2; ++TriIndex)
					{
						const FLBAGroundTriangle& Tri = Section.Triangles[Cell + TriIndex];
						if (!Tri.IsVisible())
						{
							continue;
						}
						FIntPoint Corners[3];
						FLBAIsland::GetTriangleCorners(Section.Triangles[Cell].Sens(), TriIndex, Corners);
						bool bAtSeaLevel = false;
						bool bAboveSea = false;
						for (const FIntPoint& Corner : Corners)
						{
							const int32 Height = Section.GetHeight(X + Corner.X, Z + Corner.Y);
							bAtSeaLevel |= Height == 0;
							bAboveSea |= Height > 0;
						}
						if (!bAtSeaLevel || !bAboveSea)
						{
							continue;
						}
						const FColor Flat = Palette[Tri.Bank() * 16 + 11];
						const int32 TexBase = Tri.TexIndex() * 3;
						if (Tri.IsTextured() && Section.TexCoords.IsValidIndex(TexBase + 2))
						{
							// The texture at the corners and the centre; color-keyed texels show the triangle's color.
							FVector2f Centre = FVector2f::ZeroVector;
							for (int32 K = 0; K < 4; ++K)
							{
								FVector2f UV;
								if (K < 3)
								{
									UV = Section.TexCoords[TexBase + K];
									Centre += UV / 3.f;
								}
								else
								{
									UV = Centre;
								}
								const uint8 Index = Island.GroundTexture[(int32(UV.Y) & 255) * 256 + (int32(UV.X) & 255)];
								if (Index != 0 || Tri.IsColored())
								{
									Sum += FLinearColor::FromSRGBColor(Index != 0 ? Palette[Index] : Flat);
									++Count;
								}
							}
						}
						else if (Tri.IsColored())
						{
							Sum += FLinearColor::FromSRGBColor(Flat);
							++Count;
						}
					}
				}
			}
		}
		return Count > 0 ? Sum / float(Count) : FLinearColor(0.5f, 0.42f, 0.28f);
	}

	/**
	 * The island's rain density map (see FLBAEnvironmentPreset::RainDensity) over Bounds: full rain over the sea and
	 * high ground, LightRain over low ground, blended in between and smoothed over a few tens of meters so the rain
	 * gets lighter gradually. One texel per few terrain vertices; OutBounds gets the world XY it covers.
	 */
	UTexture2D* BuildRainDensity(FLBAImportContext& Context, const FLBAIsland& Island, const FBox& Bounds, const FString& Folder,
		FBox2D& OutBounds)
	{
		constexpr int32 TexelVertices = 4;
		constexpr float LightRain = 0.25f;
		// Heights above the sea (cm) where the rain starts getting heavier, and where it is full.
		constexpr float LowGround = 300.f;
		constexpr float HighGround = 800.f;
		constexpr float SmoothRadius = 1000.f;

		const float Scale = Context.GetScale();
		const double TexelSize = LBA::BrickSizeXZ * Scale * TexelVertices;
		const FIntPoint Min(FMath::FloorToInt32(Bounds.Min.X / TexelSize), FMath::FloorToInt32(Bounds.Min.Y / TexelSize));
		const FIntPoint Max(FMath::CeilToInt32(Bounds.Max.X / TexelSize), FMath::CeilToInt32(Bounds.Max.Y / TexelSize));
		const int32 Width = Max.X - Min.X;
		const int32 Height = Max.Y - Min.Y;
		if (Width <= 0 || Height <= 0)
		{
			return nullptr;
		}

		TArray<float> Density;
		Density.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				float Sum = 0.f;
				for (int32 VZ = 0; VZ < TexelVertices; ++VZ)
				{
					for (int32 VX = 0; VX < TexelVertices; ++VX)
					{
						const float Ground = float(GlobalHeight(Island, (Min.X + X) * TexelVertices + VX, (Min.Y + Y) * TexelVertices + VZ)) * Scale;
						Sum += Ground <= 0.f ? 1.f : FMath::Lerp(LightRain, 1.f, FMath::SmoothStep(LowGround, HighGround, Ground));
					}
				}
				Density[Y * Width + X] = Sum / float(TexelVertices * TexelVertices);
			}
		}

		// Gaussian smoothing, one axis then the other (the map's edges are open sea: clamped).
		const float Sigma = SmoothRadius / float(TexelSize);
		const int32 Radius = FMath::CeilToInt32(3.f * Sigma);
		TArray<float> Kernel;
		float KernelSum = 0.f;
		for (int32 K = -Radius; K <= Radius; ++K)
		{
			KernelSum += Kernel.Add_GetRef(FMath::Exp(-0.5f * FMath::Square(K / Sigma)));
		}
		for (float& Weight : Kernel)
		{
			Weight /= KernelSum;
		}
		TArray<float> Smoothed;
		Smoothed.SetNumUninitialized(Density.Num());
		for (const FIntPoint Step : { FIntPoint(1, 0), FIntPoint(0, 1) })
		{
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					float Sum = 0.f;
					for (int32 K = -Radius; K <= Radius; ++K)
					{
						const int32 SX = FMath::Clamp(X + K * Step.X, 0, Width - 1);
						const int32 SY = FMath::Clamp(Y + K * Step.Y, 0, Height - 1);
						Sum += Kernel[K + Radius] * Density[SY * Width + SX];
					}
					Smoothed[Y * Width + X] = Sum;
				}
			}
			Swap(Density, Smoothed);
		}

		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Density.Num());
		for (int32 Index = 0; Index < Density.Num(); ++Index)
		{
			const uint8 Value = uint8(FMath::RoundToInt32(FMath::Clamp(Density[Index], 0.f, 1.f) * 255.f));
			Pixels[Index] = FColor(Value, Value, Value, 255);
		}
		UTexture2D* Texture = Context.CreateTexture(Folder, FString::Printf(TEXT("T_%s_RainDensity"), *Island.Name), Width, Height, Pixels, false);
		if (!Texture)
		{
			return nullptr;
		}
		// Smooth, and read at mip 0 by the rain's vertex shaders: no mips, always resident.
		Texture->PreEditChange(nullptr);
		Texture->Filter = TF_Bilinear;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->NeverStream = true;
		Texture->PostEditChange();
		OutBounds = FBox2D(FVector2D(Min) * TexelSize, FVector2D(Max) * TexelSize);
		return Texture;
	}
}

FLBAIslandImporter::FLBAIslandImporter(FLBAImportContext& InContext)
	: Context(InContext)
{
}

TArray<FString> FLBAIslandImporter::FindIslands() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Context.GetDataDirectory(), TEXT("*.ILE")), true, false);
	TArray<FString> Names;
	for (const FString& File : Files)
	{
		Names.Add(FPaths::GetBaseFilename(File).ToUpper());
	}
	Names.Sort();
	return Names;
}

int32 FLBAIslandImporter::ImportAll()
{
	const TArray<FString>& Filter = Context.GetSettings().IslandFilter;
	int32 Count = 0;
	for (const FString& Name : FindIslands())
	{
		if (Filter.Num() > 0 && !Filter.ContainsByPredicate([&Name](const FString& F) { return F.Equals(Name, ESearchCase::IgnoreCase); }))
		{
			continue;
		}
		if (Context.IsCancelled())
		{
			break;
		}
		if (ImportIsland(Name))
		{
			++Count;
		}
	}
	return Count;
}

bool FLBAIslandImporter::ImportIsland(const FString& IslandName)
{
	Context.ReportProgress(FText::Format(LOCTEXT("Island", "Importing island {0}"), FText::FromString(IslandName)));

	const FLBAHqrFile* Ile = Context.GetHqr(IslandName + TEXT(".ILE"));
	if (!Ile)
	{
		Context.Warn(FString::Printf(TEXT("Island %s: missing .ILE file"), *IslandName));
		return false;
	}

	FLBAIsland Island;
	if (!Island.Load(*Ile))
	{
		Context.Warn(FString::Printf(TEXT("Island %s: could not parse"), *IslandName));
		return false;
	}

	// Colors and textures of the island use its own palette; other layers use the interior one.
	Context.UsePalette(FLBAXpl::GetEntryForIsland(Island.Name));
	ON_SCOPE_EXIT
	{
		Context.UsePalette(FLBAXpl::InteriorEntry);
	};

	const FString IslandFolder = Context.GetContentPath(TEXT("Islands") / Island.Name);
	const FString TextureFolder = IslandFolder / TEXT("Textures");
	const FString MaterialFolder = IslandFolder / TEXT("Materials");

	// Ground texture page: palette index 0 is transparent so the vertex color shows through.
	UTexture2D* GroundTexture = Context.CreatePaletteTexture(TextureFolder, FString::Printf(TEXT("T_%s_Ground"), *Island.Name), 256, 256,
		[&Island](int32 X, int32 Y) { return Island.GroundTexture[Y * 256 + X]; }, true, true);
	FLBAMaterialLibrary& Materials = Context.GetMaterials();
	UMaterialInterface* GroundMaterial = Materials.GetTextured(ELBAMaterial::Ground, GroundTexture, MaterialFolder);
	UMaterialInterface* ColorMaterial = Materials.GetMaster(ELBAMaterial::Color);

	FLBADecorGlow Glow;
	TArray<UStaticMesh*> DecorMeshes = BuildDecorMeshes(Island, IslandFolder, Glow);

	TArray<UStaticMesh*> GroundMeshes;
	for (const FLBAIslandSection& Section : Island.Sections)
	{
		GroundMeshes.Add(BuildGroundMesh(Island, Section, IslandFolder / TEXT("Ground"), GroundMaterial, ColorMaterial));
	}
	Context.Flush();

	if (Context.GetProjectSettings().bCreateIslandLevels)
	{
		BuildLevel(Island, IslandFolder, GroundMeshes, DecorMeshes, Glow);
	}
	return true;
}

namespace
{
	/** The glowing texels of an island's object texture page (FLBAGlowTexels). */
	struct FLBATexelGlowMap
	{
		/** The glow of each texel (256 x 256), INDEX_NONE where nothing glows. */
		TArray<int8> Glow;
		/** The page with the glowing texels moved to their palette ramp. */
		TArray<uint8> Recolored;
		/** By glow: the average color of its texels, brightest channel 1. */
		TArray<FLinearColor> LightColors;
	};

	FLBATexelGlowMap BuildTexelGlowMap(const TArray<uint8>& Page, const TArray<FColor>& Palette, const TArray<FLBAGlowTexels>& Glows)
	{
		FLBATexelGlowMap Map;
		Map.Glow.Init(INDEX_NONE, 256 * 256);
		Map.Recolored = Page;
		for (int32 GlowIndex = 0; GlowIndex < Glows.Num(); ++GlowIndex)
		{
			const FLBAGlowTexels& Glow = Glows[GlowIndex];
			FLinearColor Sum = FLinearColor::Black;
			int32 Count = 0;
			for (int32 Y = FMath::Max(Glow.Texels.Min.Y, 0); Y < FMath::Min(Glow.Texels.Max.Y, 256); ++Y)
			{
				for (int32 X = FMath::Max(Glow.Texels.Min.X, 0); X < FMath::Min(Glow.Texels.Max.X, 256); ++X)
				{
					const int32 Texel = Y * 256 + X;
					const uint8 Index = Page[Texel];
					// Glass is cool; window frames are wood, brass or stone. Index 0 is black (the color key).
					if (Index == 0 || (Glow.bGlassOnly && Palette[Index].B < Palette[Index].R))
					{
						continue;
					}
					Map.Glow[Texel] = int8(GlowIndex);
					if (Glow.RecolorBank != INDEX_NONE)
					{
						Map.Recolored[Texel] = uint8(Glow.RecolorBank * 16 + (Index & 15));
					}
					Sum += FLinearColor::FromSRGBColor(Palette[Map.Recolored[Texel]]);
					++Count;
				}
			}
			const FLinearColor Average = Count > 0 ? Sum / float(Count) : FLinearColor::White;
			Map.LightColors.Add(Average / FMath::Max(Average.GetMax(), 1e-4f));
		}
		return Map;
	}

	/**
	 * The lights of a decor's glow patches: one per window (touching patches of one glow in one plane) or, for lamps,
	 * one per glow.
	 */
	TArray<FLBADecorGlowLight> GroupGlowPatches(const TArray<FLBAGlowPatch>& Patches, const TArray<FLBAGlowTexels>& Glows,
		const TArray<FLinearColor>& LightColors)
	{
		constexpr float MinPlaneCos = 0.95f;
		constexpr float PlaneTolerance = 10.f;
		constexpr float Gap = 30.f;
		TArray<int32> Parent;
		for (int32 Index = 0; Index < Patches.Num(); ++Index)
		{
			Parent.Add(Index);
		}
		auto Root = [&Parent](int32 Index)
		{
			while (Parent[Index] != Index)
			{
				Index = Parent[Index] = Parent[Parent[Index]];
			}
			return Index;
		};
		for (int32 A = 0; A < Patches.Num(); ++A)
		{
			for (int32 B = A + 1; B < Patches.Num(); ++B)
			{
				const FLBAGlowPatch& PA = Patches[A];
				const FLBAGlowPatch& PB = Patches[B];
				if (PA.Glow != PB.Glow)
				{
					continue;
				}
				const bool bSameWindow = (PA.Normal | PB.Normal) > MinPlaneCos
					&& FMath::Abs((PA.Bounds.GetCenter() - PB.Bounds.GetCenter()) | PA.Normal) < PlaneTolerance
					&& PA.Bounds.ExpandBy(Gap).Intersect(PB.Bounds);
				if (!Glows[PA.Glow].bWindow || bSameWindow)
				{
					Parent[Root(A)] = Root(B);
				}
			}
		}

		TArray<FLBADecorGlowLight> Lights;
		TArray<FBox3f> Bounds;
		TMap<int32, int32> LightByRoot;
		for (int32 Index = 0; Index < Patches.Num(); ++Index)
		{
			const FLBAGlowPatch& Patch = Patches[Index];
			const int32* Found = LightByRoot.Find(Root(Index));
			const int32 LightIndex = Found ? *Found : LightByRoot.Add(Root(Index), Lights.Num());
			if (!Found)
			{
				FLBADecorGlowLight& Light = Lights.AddDefaulted_GetRef();
				Light.Glow = Patch.Glow;
				Light.Color = LightColors[Patch.Glow];
				Bounds.Add(FBox3f(ForceInit));
			}
			Bounds[LightIndex] += Patch.Bounds;
			Lights[LightIndex].Normal += Patch.Normal * Patch.Area;
		}
		for (int32 Index = 0; Index < Lights.Num(); ++Index)
		{
			FLBADecorGlowLight& Light = Lights[Index];
			Light.Center = Bounds[Index].GetCenter();
			if (!Glows[Light.Glow].bWindow)
			{
				Light.Normal = FVector3f::ZeroVector;
				Light.Size = FVector2f(Bounds[Index].GetExtent().GetMin(), 0.f);
				continue;
			}
			// The window's width along the horizontal of its plane, its height across.
			Light.Normal = Light.Normal.GetSafeNormal();
			const FVector3f Across = (FMath::Abs(Light.Normal.Z) < 0.9f ? FVector3f::UpVector ^ Light.Normal : FVector3f::ForwardVector ^ Light.Normal).GetSafeNormal();
			const FVector3f Up = Light.Normal ^ Across;
			FVector3f Corners[8];
			Bounds[Index].GetVertices(Corners);
			FFloatInterval Width, Height;
			for (const FVector3f& Corner : Corners)
			{
				Width.Include(Corner | Across);
				Height.Include(Corner | Up);
			}
			Light.Size = FVector2f(Width.Size(), Height.Size());
		}
		// Glass seen from both sides faces nowhere.
		Lights.RemoveAll([&Glows](const FLBADecorGlowLight& Light) { return Glows[Light.Glow].bWindow && Light.Normal.IsZero(); });
		return Lights;
	}
}

TArray<UStaticMesh*> FLBAIslandImporter::BuildDecorMeshes(const FLBAIsland& Island, const FString& IslandFolder, FLBADecorGlow& OutGlow)
{
	TArray<UStaticMesh*> Meshes;
	const FLBAHqrFile* Obl = Context.GetHqr(Island.Name + TEXT(".OBL"));
	if (!Obl)
	{
		Context.Warn(FString::Printf(TEXT("Island %s: missing .OBL file, no decor"), *Island.Name));
		return Meshes;
	}

	const FString TextureFolder = IslandFolder / TEXT("Textures");
	const FString MaterialFolder = IslandFolder / TEXT("Materials");
	TMap<FString, UTexture2D*> RegionTextures;
	FLBAMaterialLibrary& Materials = Context.GetMaterials();

	FLBABodyConvertParams Params;
	Params.Scale = Context.GetScale();
	Params.Palette = &Context.GetPalette();
	Params.ColorMaterial = Materials.GetMaster(ELBAMaterial::Color);
	Params.TranslucentMaterial = Materials.GetMaster(ELBAMaterial::Translucent);
	Params.GetTexturedMaterial = [&](const FLBATextureRegion& Region, bool bMasked) -> UMaterialInterface*
	{
		UTexture2D*& Texture = RegionTextures.FindOrAdd(Region.GetKey());
		if (!Texture)
		{
			Texture = Context.CreatePaletteTexture(TextureFolder, FString::Printf(TEXT("T_%s_Obj_%s"), *Island.Name, *Region.GetKey()),
				Region.Width, Region.Height,
				[&Island, Region](int32 X, int32 Y) { return Island.ObjectTexture[((Region.Y + Y) & 255) * 256 + ((Region.X + X) & 255)]; },
				true, true);
		}
		return Materials.GetTextured(bMasked ? ELBAMaterial::TexturedMasked : ELBAMaterial::Textured, Texture, MaterialFolder);
	};

	const FLBAEnvironmentPreset Preset = FLBAEnvironmentPreset::ForIsland(Island.Name);
	const FLBATexelGlowMap TexelGlow = BuildTexelGlowMap(Island.ObjectTexture, Context.GetPalette(), Preset.GlowTexels);
	TMap<FString, UTexture2D*> GlowTextures;
	if (!Preset.GlowTexels.IsEmpty())
	{
		Params.TexelGlow = &TexelGlow.Glow;
		Params.GlowRects.SetNum(Preset.GlowTexels.Num());
		Params.GetGlowMaterial = [&](const FLBATextureRegion& Region) -> UMaterialInterface*
		{
			UTexture2D*& Texture = GlowTextures.FindOrAdd(Region.GetKey());
			if (!Texture)
			{
				// The region with its glowing texels recolored and marked in alpha.
				const TArray<FColor>& Palette = Context.GetPalette();
				TArray<FColor> Pixels;
				Pixels.SetNumUninitialized(Region.Width * Region.Height);
				for (int32 Y = 0; Y < Region.Height; ++Y)
				{
					for (int32 X = 0; X < Region.Width; ++X)
					{
						const int32 Texel = ((Region.Y + Y) & 255) * 256 + ((Region.X + X) & 255);
						FColor Color = Palette[TexelGlow.Recolored[Texel]];
						Color.A = TexelGlow.Glow[Texel] != INDEX_NONE ? 255 : 0;
						Pixels[Y * Region.Width + X] = Color;
					}
				}
				Texture = Context.CreateTexture(TextureFolder, FString::Printf(TEXT("T_%s_Obj_%s_Glow"), *Island.Name, *Region.GetKey()),
					Region.Width, Region.Height, Pixels, true);
			}
			return Materials.GetTextured(ELBAMaterial::TexturedGlow, Texture, MaterialFolder);
		};
	}

	for (int32 Index = 0; Index < Obl->Num(); ++Index)
	{
		FLBABody Body;
		FLBAMeshBuilder Builder;
		Params.DebugIndex = Index;
		const FLBADecorLight* DecorLight = Preset.FindDecorLight(Index);
		Params.GlowColors.Reset();
		Params.GlowMaterial = DecorLight ? Materials.GetMaster(ELBAMaterial::Glow) : nullptr;
		if (DecorLight)
		{
			Params.GlowColors.Add(DecorLight->GlowColor);
		}
		for (int32 GlowIndex = 0; GlowIndex < Params.GlowRects.Num(); ++GlowIndex)
		{
			const FLBAGlowTexels& Texels = Preset.GlowTexels[GlowIndex];
			Params.GlowRects[GlowIndex] = Texels.ExcludedBodies.Contains(Index) ? FIntRect() : Texels.Texels;
		}
		FBox3f Glow(ForceInit);
		TArray<FLBAGlowPatch> GlowPatches;
		if (!Body.Parse(Obl->GetEntry(Index)) || !LBABodyConverter::Convert(Body, Params, Builder, &Glow, &GlowPatches))
		{
			Meshes.Add(nullptr);
			continue;
		}
		if (DecorLight && !Glow.IsValid)
		{
			Context.Warn(FString::Printf(TEXT("Island %s: lit decor %d has no polygon of color %d"), *Island.Name, Index, DecorLight->GlowColor));
		}
		else if (Glow.IsValid)
		{
			OutGlow.LampBounds.Add(Index, Glow);
		}
		if (!GlowPatches.IsEmpty())
		{
			OutGlow.TexelLights.Add(Index, GroupGlowPatches(GlowPatches, Preset.GlowTexels, TexelGlow.LightColors));
		}

		FMeshDescription Description;
		TArray<UMaterialInterface*> MeshMaterials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, MeshMaterials, Slots);

		FLBAStaticMeshOptions Options;
		Options.bComplexAsSimple = true;
		// The light sits inside the glowing part.
		Options.NoShadowSlots.Add(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Glow));
		Meshes.Add(Context.CreateStaticMesh(IslandFolder / TEXT("Decor"), FString::Printf(TEXT("SM_%s_Decor_%03d"), *Island.Name, Index),
			MoveTemp(Description), MeshMaterials, Slots, Options));
	}
	return Meshes;
}

UStaticMesh* FLBAIslandImporter::BuildGroundMesh(const FLBAIsland& Island, const FLBAIslandSection& Section, const FString& Folder,
	UMaterialInterface* GroundMaterial, UMaterialInterface* ColorMaterial)
{
	const float Scale = Context.GetScale();
	const TArray<FColor>& Palette = Context.GetPalette();
	const int32 BaseX = Section.CubeX * CellsPerCube;
	const int32 BaseZ = Section.CubeZ * CellsPerCube;

	auto VertexNormal = [&](int32 X, int32 Z)
	{
		const int32 GX = BaseX + X;
		const int32 GZ = BaseZ + Z;
		const double DhDx = double(GlobalHeight(Island, GX + 1, GZ) - GlobalHeight(Island, GX - 1, GZ)) / (2.0 * LBA::BrickSizeXZ);
		const double DhDz = double(GlobalHeight(Island, GX, GZ + 1) - GlobalHeight(Island, GX, GZ - 1)) / (2.0 * LBA::BrickSizeXZ);
		return FVector3f(LBA::DirToUnreal(-DhDx, 1.0, -DhDz).GetSafeNormal());
	};

	FLBAMeshBuilder Builder;
	const int32 TexturedSection = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Ground), GroundMaterial);
	const int32 ColoredSection = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color), ColorMaterial);

	for (int32 Z = 0; Z < CellsPerCube; ++Z)
	{
		for (int32 X = 0; X < CellsPerCube; ++X)
		{
			const int32 Cell = (Z * CellsPerCube + X) * 2;
			const int32 Sens = Section.Triangles[Cell].Sens();
			for (int32 TriIndex = 0; TriIndex < 2; ++TriIndex)
			{
				// Liquid at sea level is replaced by the level's water.
				if (ClassifyGroundTriangle(Section, X, Z, TriIndex) != EGroundTriangle::Land)
				{
					continue;
				}
				const FLBAGroundTriangle& Tri = Section.Triangles[Cell + TriIndex];

				FIntPoint Corners[3];
				FLBAIsland::GetTriangleCorners(Sens, TriIndex, Corners);

				FLBAVertex V[3];
				for (int32 K = 0; K < 3; ++K)
				{
					const int32 VX = X + Corners[K].X;
					const int32 VZ = Z + Corners[K].Y;
					const int32 Height = Section.GetHeight(VX, VZ);
					V[K].Position = FVector3f(LBA::ToUnreal(VX * LBA::BrickSizeXZ, Height, VZ * LBA::BrickSizeXZ, Scale));
					V[K].Normal = VertexNormal(VX, VZ);
					// Vertex alpha: baked shade row / 24, so the unshaded row 12 is 0.5.
					const float Light = Section.GetShadeRow(VX, VZ) / 24.f;
					const FColor Albedo = Tri.IsColored() ? Palette[Tri.Bank() * 16 + 11] : FColor::Black;
					V[K].Color = LBABodyConverter::ToVertexColor(Albedo, Light);
				}

				int32 Target = ColoredSection;
				if (Tri.IsTextured())
				{
					const int32 TexBase = Tri.TexIndex() * 3;
					if (Section.TexCoords.IsValidIndex(TexBase + 2))
					{
						for (int32 K = 0; K < 3; ++K)
						{
							V[K].UV = Section.TexCoords[TexBase + K] / 256.f;
						}
						Target = TexturedSection;
					}
				}
				Builder.AddTriangleFacing(Target, V[0], V[1], V[2], FVector3f::UpVector);
			}
		}
	}

	if (Builder.IsEmpty())
	{
		return nullptr;
	}

	FMeshDescription Description;
	TArray<UMaterialInterface*> MeshMaterials;
	TArray<FName> Slots;
	Builder.BuildStatic(Description, MeshMaterials, Slots);

	FLBAStaticMeshOptions Options;
	Options.bComplexAsSimple = true;
	return Context.CreateStaticMesh(Folder, FString::Printf(TEXT("SM_%s_Ground_%02d_%02d"), *Island.Name, Section.CubeX, Section.CubeZ),
		MoveTemp(Description), MeshMaterials, Slots, Options);
}

void FLBAIslandImporter::BuildLevel(const FLBAIsland& Island, const FString& IslandFolder, const TArray<UStaticMesh*>& GroundMeshes,
	const TArray<UStaticMesh*>& DecorMeshes, const FLBADecorGlow& Glow)
{
	UWorld* World = Context.BeginLevel(IslandFolder, TEXT("L_") + Island.Name);
	if (!World)
	{
		return;
	}
	const float Scale = Context.GetScale();
	FLBAEnvironmentPreset Preset = FLBAEnvironmentPreset::ForIsland(Island.Name);

	FBox IslandBounds(ForceInit);
	TArray<AActor*> GroundActors;
	FVector HighestGround(0.0, 0.0, -UE_BIG_NUMBER);
	for (int32 Index = 0; Index < Island.Sections.Num(); ++Index)
	{
		const FLBAIslandSection& Section = Island.Sections[Index];
		const FIntVector Origin = Section.GetOrigin();
		const FVector Location = LBA::ToUnreal(Origin.X, Origin.Y, Origin.Z, Scale);
		IslandBounds += Location;
		IslandBounds += LBA::ToUnreal(Origin.X + LBA::IslandCubeSize, 0, Origin.Z + LBA::IslandCubeSize, Scale);
		for (int32 Z = 0; Z < 65; ++Z)
		{
			for (int32 X = 0; X < 65; ++X)
			{
				const FVector Vertex = LBA::ToUnreal(Origin.X + X * LBA::BrickSizeXZ, Section.GetHeight(X, Z), Origin.Z + Z * LBA::BrickSizeXZ, Scale);
				HighestGround = Vertex.Z > HighestGround.Z ? Vertex : HighestGround;
			}
		}

		if (UStaticMesh* Mesh = GroundMeshes.IsValidIndex(Index) ? GroundMeshes[Index] : nullptr)
		{
			if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Location),
				FString::Printf(TEXT("Ground_%02d_%02d"), Section.CubeX, Section.CubeZ), TEXT("Ground")))
			{
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				GroundActors.Add(Actor);
			}
		}

		const FName SectionFolder(*FString::Printf(TEXT("Decor/Section_%02d_%02d"), Section.CubeX, Section.CubeZ));
		const FName ConditionalFolder(*FString::Printf(TEXT("Decor/Section_%02d_%02d/Conditional"), Section.CubeX, Section.CubeZ));
		for (int32 DecorIndex = 0; DecorIndex < Section.Decors.Num(); ++DecorIndex)
		{
			const FLBAIslandDecor& Decor = Section.Decors[DecorIndex];
			UStaticMesh* Mesh = DecorMeshes.IsValidIndex(Decor.Body) ? DecorMeshes[Decor.Body] : nullptr;
			if (!Mesh)
			{
				continue;
			}
			const FIntVector P = Origin + Decor.Position;
			const FTransform Transform(LBA::BetaToRotator(Decor.Beta), LBA::ToUnreal(P.X, P.Y, P.Z, Scale));
			const bool bConditional = Decor.ConditionVar != 0;
			AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, Transform,
				FString::Printf(TEXT("Decor_%03d"), Decor.Body), bConditional ? ConditionalFolder : SectionFolder);
			if (Actor)
			{
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				if (bConditional)
				{
					Actor->Tags.Add(*FString::Printf(TEXT("LBA_Var_%d=%d"), Decor.ConditionVar, Decor.ConditionValue));
				}
				const FLBADecorLight* DecorLight = Preset.FindDecorLight(Decor.Body);
				const FBox3f* LampBounds = Glow.LampBounds.Find(Decor.Body);
				if (DecorLight && LampBounds)
				{
					// The light has the glass's color at full brightness.
					const FLinearColor GlassColor = FLinearColor::FromSRGBColor(LBABodyConverter::GetAlbedo(Context.GetPalette(), DecorLight->GlowColor, ELBAPolyType::Gouraud));
					const FLinearColor LightColor = GlassColor / FMath::Max(FMath::Max3(GlassColor.R, GlassColor.G, GlassColor.B), 1e-4f);
					LBAEnvironment::SpawnDecorLight(Context, Actor, *DecorLight, FVector(LampBounds->GetCenter()), LightColor, LampBounds->GetExtent().GetMin());
				}
				for (const FLBADecorGlowLight& Light : Glow.TexelLights.FindRef(Decor.Body))
				{
					const FLBAGlowTexels& Texels = Preset.GlowTexels[Light.Glow];
					if (Texels.bWindow)
					{
						LBAEnvironment::SpawnWindowLight(Context, Actor, Texels, FVector(Light.Center), FVector(Light.Normal), FVector2D(Light.Size), Light.Color);
					}
					else
					{
						FLBADecorLight Lamp;
						Lamp.Light = Texels.Light;
						Lamp.Radius = Texels.Radius;
						Lamp.Halo = Texels.Halo;
						LBAEnvironment::SpawnDecorLight(Context, Actor, Lamp, FVector(Light.Center), Light.Color, Light.Size.X);
					}
				}
			}
		}
	}

	// Sky and sea textures, fog: from the island's palette, sky/sea texture page and first section (the original
	// sets them per section, but they hardly vary within an island).
	const FLBAIslandSection& FirstSection = Island.Sections[0];
	const FLBAXpl* Xpl = Context.GetActiveXpl();
	if (Xpl)
	{
		Preset.PaletteEntry = Xpl->Entry;
		Preset.FogColor = FLinearColor::FromSRGBColor(Xpl->Palette[Xpl->FogColor]);
	}
	Preset.GroundRow = GetSunlitGroundRow(Island);

	const float FogDistanceScale = Context.GetProjectSettings().FogDistanceScale;
	Preset.FogStart = FirstSection.FogStart * Scale * FogDistanceScale;
	Preset.FogEnd = FirstSection.ClipFar * Scale * FogDistanceScale;

	const FString TextureFolder = IslandFolder / TEXT("Textures");
	const FString MaterialFolder = IslandFolder / TEXT("Materials");
	const FLBAHqrFile* Ress = Context.GetHqr(TEXT("RESS.HQR"));
	const TArray<uint8> SkySea = Xpl && Ress ? Ress->GetEntry(FLBAXpl::GetSkySeaEntry(Xpl->Entry)) : TArray<uint8>();
	UTexture2D* SeaTexture = nullptr;
	if (SkySea.Num() >= 256 * 128)
	{
		SeaTexture = Context.CreatePaletteTexture(TextureFolder, FString::Printf(TEXT("T_%s_Sea"), *Island.Name), 128, 128,
			[&SkySea](int32 X, int32 Y) { return SkySea[Y * 256 + X]; }, false, true);
		if (SeaTexture)
		{
			// The cartoon water reads its mips directly: keep them all resident.
			SeaTexture->PreEditChange(nullptr);
			SeaTexture->NeverStream = true;
			SeaTexture->PostEditChange();
		}

		// The original's sky is the cloud half of this page tiled over the fog color: its average color tints
		// Unreal's sky, and its brightest texels give the clouds' color.
		TArray<FLinearColor> Sky;
		Sky.Reserve(128 * 128);
		FLinearColor Sum = FLinearColor::Black;
		for (int32 Y = 0; Y < 128; ++Y)
		{
			for (int32 X = 0; X < 128; ++X)
			{
				Sky.Add(FLinearColor::FromSRGBColor(Xpl->Palette[SkySea[Y * 256 + 128 + X]]));
				Sum += Sky.Last();
			}
		}
		Preset.SkyColor = Sum / float(Sky.Num());
		Sky.Sort([](const FLinearColor& A, const FLinearColor& B) { return A.GetLuminance() > B.GetLuminance(); });
		FLinearColor Bright = FLinearColor::Black;
		const int32 NumBright = Sky.Num() / 5;
		for (int32 Index = 0; Index < NumBright; ++Index)
		{
			Bright += Sky[Index];
		}
		const float BrightMax = FMath::Max3(Bright.R, Bright.G, Bright.B);
		Preset.CloudColor = FLinearColor(Bright.R, Bright.G, Bright.B) * (0.98f / FMath::Max(BrightMax, 1e-4f));

		// The sea's main color: its most common palette color.
		int32 Counts[256] = {};
		for (int32 Y = 0; Y < 128; ++Y)
		{
			for (int32 X = 0; X < 128; ++X)
			{
				++Counts[SkySea[Y * 256 + X]];
			}
		}
		int32 SeaIndex = 0;
		for (int32 Index = 1; Index < 256; ++Index)
		{
			SeaIndex = Counts[Index] > Counts[SeaIndex] ? Index : SeaIndex;
		}
		Preset.SeaColor = FLinearColor::FromSRGBColor(Xpl->Palette[SeaIndex]);
		Preset.SeaTexture = SeaTexture;
		Preset.SeaTileSize = 8192.f * Scale;
	}
	Preset.bWater &= Preset.SeaColor.GetLuminance() > 0.f;

	// Modern fog: the original drew everything crisp up to its fog distance and nothing beyond; the height fog keeps
	// that range clear and lets you see the rest of the island fading into the palette's fog color, unless the
	// island's preset has its own fog. Overcast skies light the scene in the clouds' color.
	if (Preset.bAtmosphere && Preset.FogVisibility <= 0.f)
	{
		Preset.HeightFogStart = Preset.FogEnd;
		Preset.FogVisibility = 8.f * FirstSection.ClipFar * Scale;
	}
	if (Preset.bOvercast)
	{
		Preset.SkyLightColor = Preset.CloudColor;
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2 environment %s: fog %.0f-%.0f cm, visibility %.0f cm, sky %s, clouds %s"), *Island.Name,
		Preset.FogStart, Preset.FogEnd, Preset.FogVisibility, *Preset.SkyColor.ToString(), *Preset.CloudColor.ToString());

	// Sea: Unreal water on Twinsun, else a large opaque plane at LBA height 0 with the island's sea texture, like the
	// original's; and an invisible floor: where the seabed stops on Twinsun, else just below so the player can wade.
	const FRotator SunRotation = LBAEnvironment::SunRotationFromLbaAngles(FirstSection.LightAlpha, FirstSection.LightBeta);
	const FBox SeaBounds = IslandBounds.ExpandBy(FVector(LBA::IslandCubeSize * Scale * 6.0, LBA::IslandCubeSize * Scale * 6.0, 0.0));
	if (Preset.bWater)
	{
		// The terrain stops at sea level: a seabed slopes down from the shores, under the water.
		const double Spacing = LBA::BrickSizeXZ * Scale;
		FLBASeabedSettings Seabed;
		Seabed.Vertices = FIntRect(FMath::FloorToInt32(SeaBounds.Min.X / Spacing), FMath::FloorToInt32(SeaBounds.Min.Y / Spacing),
			FMath::CeilToInt32(SeaBounds.Max.X / Spacing) + 1, FMath::CeilToInt32(SeaBounds.Max.Y / Spacing) + 1);
		Seabed.Height = [&Island, Scale](int32 GX, int32 GZ) { return float(GlobalHeight(Island, GX, GZ)) * Scale; };
		Seabed.VertexSpacing = float(Spacing);
		Seabed.Color = GetShoreColor(Island, Context.GetPalette());
		Seabed.Material = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
		if (UStaticMesh* SeabedMesh = LBASeabed::Build(Context, Seabed, IslandFolder / TEXT("Ground"), FString::Printf(TEXT("SM_%s_Seabed"), *Island.Name)))
		{
			if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform::Identity, TEXT("Seabed"), TEXT("Ground")))
			{
				UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
				Component->SetStaticMesh(SeabedMesh);
				Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
				Component->SetCastShadow(false);
				GroundActors.Add(Actor);
			}
		}

		FLBAWaterSettings Water;
		Water.Bounds = SeaBounds;
		Water.Coastline = TraceCoastline(Island, Spacing);
		Water.LandPoint = FVector2D(HighestGround);
		Water.MaterialFolder = MaterialFolder;
		Water.Name = Island.Name;
		Water.TerrainActors = GroundActors;
		const FLBAEnvironmentLights Lights = LBAEnvironment::ComputeLights(Preset, SunRotation, Xpl ? Xpl->ShadeNormalLevel : 12);
		Preset.WaterNeutralLight = LBAWater::Spawn(Context, World, Water, Preset, Lights);
	}
	if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
	{
		const FVector Center = SeaBounds.GetCenter();
		const FVector Size = SeaBounds.GetSize();
		const FVector PlaneScale(Size.X / 100.0, Size.Y / 100.0, 1.0);

		UMaterialInstanceConstant* WaterMaterial = Preset.bWater ? nullptr : Context.CreateOrReuseAsset<UMaterialInstanceConstant>(MaterialFolder,
			FString::Printf(TEXT("MI_%s_Water"), *Island.Name));
		if (WaterMaterial)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(WaterMaterial, Context.GetMaterials().GetMaster(ELBAMaterial::Water));
			UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(WaterMaterial);
			if (SeaTexture)
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(WaterMaterial, TEXT("Texture"), SeaTexture);
			}
			else
			{
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(WaterMaterial, TEXT("WaterColor"), Preset.FogColor);
			}
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(WaterMaterial, TEXT("TileSize"), 8192.f * Scale);
			UMaterialEditingLibrary::UpdateMaterialInstance(WaterMaterial);
			Context.MarkForSave(WaterMaterial);
		}

		AStaticMeshActor* Sea = WaterMaterial ? Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(FRotator::ZeroRotator, FVector(Center.X, Center.Y, 0.0), PlaneScale),
			TEXT("Sea"), TEXT("Environment")) : nullptr;
		if (Sea)
		{
			UStaticMeshComponent* Component = Sea->GetStaticMeshComponent();
			Component->SetStaticMesh(Plane);
			Component->SetMaterial(0, WaterMaterial);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCastShadow(false);
			// Unshaded, so it would light the island with its color through Lumen.
			Component->bAffectDynamicIndirectLighting = false;
		}
		const double FloorZ = Preset.bWater ? -LBASeabed::GetCutoffDepth() : -60.0;
		if (AStaticMeshActor* Floor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(FRotator::ZeroRotator, FVector(Center.X, Center.Y, FloorZ), PlaneScale),
			TEXT("SeaFloor"), TEXT("Environment")))
		{
			UStaticMeshComponent* Component = Floor->GetStaticMeshComponent();
			Component->SetStaticMesh(Plane);
			Component->SetVisibility(false);
			Component->SetHiddenInGame(true);
			Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		}
	}

	if (Preset.Rain > 0.f)
	{
		Preset.RainDensity = BuildRainDensity(Context, Island, SeaBounds, TextureFolder, Preset.RainDensityBounds);
	}
	LBAEnvironment::Spawn(Context, World, Preset, SunRotation);

	// Player start: highest ground point of the section closest to the island center.
	{
		const FVector Center = IslandBounds.GetCenter();
		const FLBAIslandSection* Best = nullptr;
		double BestDistance = TNumericLimits<double>::Max();
		for (const FLBAIslandSection& Section : Island.Sections)
		{
			const FIntVector O = Section.GetOrigin();
			const FVector SectionCenter = LBA::ToUnreal(O.X + LBA::IslandCubeSize / 2, 0, O.Z + LBA::IslandCubeSize / 2, Scale);
			const double Distance = FVector::DistSquared2D(SectionCenter, Center);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = &Section;
			}
		}
		if (Best)
		{
			int32 BestX = 32, BestZ = 32, BestHeight = Best->GetHeight(32, 32);
			for (int32 Z = 16; Z <= 48; ++Z)
			{
				for (int32 X = 16; X <= 48; ++X)
				{
					if (Best->GetHeight(X, Z) > BestHeight)
					{
						BestHeight = Best->GetHeight(X, Z);
						BestX = X;
						BestZ = Z;
					}
				}
			}
			const FIntVector O = Best->GetOrigin();
			const FVector Location = LBA::ToUnreal(O.X + BestX * LBA::BrickSizeXZ, BestHeight, O.Z + BestZ * LBA::BrickSizeXZ, Scale) + FVector(0, 0, 120);
			Context.SpawnGeneratedActor<APlayerStart>(World, FTransform(Location), TEXT("PlayerStart"), NAME_None);
		}
	}

	Context.EndLevel(World);
}

#undef LOCTEXT_NAMESPACE
