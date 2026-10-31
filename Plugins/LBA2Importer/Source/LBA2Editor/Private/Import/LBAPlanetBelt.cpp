#include "Import/LBAPlanetBelt.h"

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
	/** Row of the equator and the one 11.25 degrees beyond it: the belt's crest is on them. */
	constexpr int32 EquatorRow = 8;
	constexpr int32 BeyondEquatorRow = 7;
	constexpr int32 TextureSize = 256;

	/**
	 * The cross-section (cm on the planet), from the shore facing the islands: a skirt under the sea, a pale shelf in
	 * shallow water before the shore (the holomap's light cyan band between the belt and the deep sea), the flank up to
	 * FlankTop (walkable, a landing place), the cliff band up to CliffTop (68 degrees: steeper than Twinsen walks), snow
	 * slopes up to the crest, and the far side down into the other hemisphere's sea. Low mountains (2026-10-27, user:
	 * 25 to 30 m, no more): the Dinofly flies over them; only walking, sailing and swimming stop there.
	 */
	constexpr double SkirtWidth = 1000.0;
	constexpr double SkirtDepth = 800.0;
	/** The shelf: this wide on average (its edge wanders), shallow most of the way out, this deep at its edge. */
	constexpr double ShelfWidth = 2400.0;
	constexpr double ShelfDepth = 250.0;
	constexpr double FlankTop = 600.0;
	constexpr double CliffTop = 1600.0;
	constexpr double CliffRun = 400.0;
	/** The crests, from the holomap's lowest to its highest altitude. */
	constexpr double MinCrest = 2000.0;
	constexpr double MaxCrest = 3000.0;
	/** The cliff's face juts out and back this much (cm), so it isn't a smooth wall. */
	constexpr double CliffRelief = 200.0;
	/**
	 * The belt is narrower than the holomap's band (user, 2026-10-27: a third as wide): its widths from the texture are
	 * scaled by this, then kept within these (cm).
	 */
	constexpr double WidthScale = 1.0 / 3.0;
	constexpr double MinFlank = 1000.0;
	constexpr double MaxFlank = 3300.0;
	constexpr double MinUpslope = 800.0;
	constexpr double MaxUpslope = 2700.0;
	constexpr double MinFarSide = 2000.0;
	constexpr double MaxFarSide = 5000.0;

	/** Mesh: columns around the pole (3.3 m apart at the shore), in sectors of 11.25 degrees. */
	constexpr int32 BeltColumns = 2048;
	constexpr int32 Sectors = 32;

	enum class EPart : uint8 { Skirt, Shelf, Flank, Cliff, Upslope, FarSide };

	struct FRow
	{
		EPart Part;
		double T;
	};

	/** Rows across the belt: each part's share, the same at every column so cliffs keep their rows. */
	TArray<FRow> MakeRows()
	{
		const TPair<EPart, int32> Parts[] = { { EPart::Skirt, 3 }, { EPart::Shelf, 6 }, { EPart::Flank, 10 }, { EPart::Cliff, 8 }, { EPart::Upslope, 14 }, { EPart::FarSide, 14 } };
		TArray<FRow> Rows;
		for (const TPair<EPart, int32>& Part : Parts)
		{
			for (int32 Index = 0; Index < Part.Value; ++Index)
			{
				Rows.Add({ Part.Key, double(Index) / Part.Value });
			}
		}
		Rows.Add({ EPart::FarSide, 1.0 });
		return Rows;
	}

	/** The belt across one longitude (cm). */
	struct FProfile
	{
		/** The shore facing the islands, from the pole. */
		double Shore = 0.0;
		/** The shelf out from the shore. */
		double Shelf = 0.0;
		/** Shore to the cliff's foot, the cliff's top to the crest, the crest to the far shore. */
		double Flank = 0.0;
		double Upslope = 0.0;
		double FarSide = 0.0;
		/** The crest's height. */
		double Crest = 0.0;
	};

	/** Latitudes (degrees) of the belt's edges at one texture column. */
	struct FTraced
	{
		double Shore = 0.0;
		double Snow = 0.0;
		double Core = 0.0;
		double Far = 0.0;
	};

	double RowLatitude(int32 Row)
	{
		return (Row + 0.5) / TextureSize * 180.0 - 90.0;
	}

	/**
	 * Each texture column, from the belt's whitest texel (latitudes -15 to 2): towards the pole, where the sea starts
	 * (the shore; the mid blue flanks around the snow are land, they slope down in the holomap's mesh) and where the
	 * snow does (the cliff); away from it, its far edge.
	 */
	TArray<FTraced> TraceTexture(const TArray<uint8>& Texture, const TArray<uint8>& Palette)
	{
		auto Color = [&](int32 Row, int32 Column)
		{
			const int32 Index = Texture[Row * TextureSize + Column] * 3;
			return FIntVector(Palette[Index], Palette[Index + 1], Palette[Index + 2]);
		};
		auto IsLand = [&](int32 Row, int32 Column)
		{
			const FIntVector C = Color(Row, Column);
			return C.X + C.Y + C.Z > 150;
		};
		auto IsSnow = [&](int32 Row, int32 Column)
		{
			const FIntVector C = Color(Row, Column);
			return C.X + C.Y + C.Z > 300 || (C.X > 70 && C.X > C.Z);
		};
		auto Walk = [](auto&& Inside, int32 Column, int32 Row, int32 Step, double Limit)
		{
			while (Row + Step >= 0 && Row + Step < TextureSize && Inside(Row + Step, Column) && RowLatitude(Row + Step) * Step < Limit * Step)
			{
				Row += Step;
			}
			return RowLatitude(Row);
		};
		TArray<FTraced> Traced;
		Traced.SetNum(TextureSize);
		for (int32 Column = 0; Column < TextureSize; ++Column)
		{
			int32 Core = INDEX_NONE;
			int32 Brightest = -1;
			for (int32 Row = 0; Row < TextureSize; ++Row)
			{
				const double Latitude = RowLatitude(Row);
				const FIntVector C = Color(Row, Column);
				if (Latitude >= -15.0 && Latitude <= 2.0 && C.X + C.Y + C.Z > Brightest)
				{
					Brightest = C.X + C.Y + C.Z;
					Core = Row;
				}
			}
			FTraced& Out = Traced[Column];
			Out.Core = RowLatitude(Core);
			Out.Shore = Walk(IsLand, Column, Core, 1, 14.0);
			Out.Snow = Walk(IsSnow, Column, Core, 1, 14.0);
			Out.Far = Walk(IsLand, Column, Core, -1, -40.0);
		}
		return Traced;
	}

	/** Gaussian smoothing all around (Sigma in samples). */
	TArray<double> SmoothAround(const TArray<double>& Values, double Sigma)
	{
		const int32 Num = Values.Num();
		const int32 Reach = FMath::CeilToInt(3.0 * Sigma);
		TArray<double> Out;
		Out.SetNumZeroed(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			double Sum = 0.0;
			double Weights = 0.0;
			for (int32 Offset = -Reach; Offset <= Reach; ++Offset)
			{
				const double Weight = FMath::Exp(-0.5 * FMath::Square(Offset / Sigma));
				Sum += Weight * Values[((Index + Offset) % Num + Num) % Num];
				Weights += Weight;
			}
			Out[Index] = Sum / Weights;
		}
		return Out;
	}

	/** Values all around, at Position (in samples, sample I at I), linearly. */
	double SampleAround(const TArray<double>& Values, double Position)
	{
		const int32 Num = Values.Num();
		const double Floor = FMath::Floor(Position);
		const int32 Index = ((int32(Floor) % Num) + Num) % Num;
		return FMath::Lerp(Values[Index], Values[(Index + 1) % Num], Position - Floor);
	}

	/** Values all around, at Position, through a Catmull-Rom spline. */
	double SplineAround(const TArray<double>& Values, double Position)
	{
		const int32 Num = Values.Num();
		const double Floor = FMath::Floor(Position);
		const double T = Position - Floor;
		auto At = [&](int32 Offset) { return Values[(((int32(Floor) + Offset) % Num) + Num) % Num]; };
		const double P0 = At(-1), P1 = At(0), P2 = At(1), P3 = At(2);
		return 0.5 * (2.0 * P1 + (P2 - P0) * T + (2.0 * P0 - 5.0 * P1 + 4.0 * P2 - P3) * T * T + (3.0 * P1 - P0 - 3.0 * P2 + P3) * T * T * T);
	}

	double Noise1D(double Value)
	{
		return FMath::PerlinNoise1D(float(Value));
	}

	double Noise2D(const FVector2D& Value)
	{
		return FMath::PerlinNoise2D(Value);
	}

	/** Sharp crests: 1 on the noise's zero lines, falling away from them; centered on 0. */
	double Ridges(const FVector2D& Point)
	{
		const double A = 1.0 - FMath::Abs(Noise2D(Point / 5000.0));
		const double B = 1.0 - FMath::Abs(Noise2D(Point / 1900.0 + FVector2D(7.3, 2.1)));
		return 0.7 * A * A + 0.3 * B * B - 0.5;
	}

	FLinearColor Srgb(uint8 R, uint8 G, uint8 B)
	{
		return FLinearColor::FromSRGBColor(FColor(R, G, B));
	}
}

TSoftObjectPtr<UWorld> LBAPlanetBelt::Build(FLBAImportContext& Context, const FString& Planet, double Radius, const FString& Folder)
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

	// The texture's edges, smoothed over a few texels (its colors are dithered), as distances on the planet.
	const TArray<FTraced> Traced = TraceTexture(Texture, Palette);
	TArray<double> Shores, Flanks, Upslopes, FarSides;
	auto Arc = [Radius](double Degrees) { return Radius * FMath::DegreesToRadians(Degrees); };
	for (const FTraced& Column : Traced)
	{
		Shores.Add(Arc(90.0 - Column.Shore));
		Flanks.Add(FMath::Clamp(WidthScale * Arc(Column.Shore - Column.Snow), MinFlank, MaxFlank));
		Upslopes.Add(FMath::Clamp(WidthScale * Arc(Column.Snow - Column.Core), MinUpslope, MaxUpslope));
		FarSides.Add(FMath::Clamp(WidthScale * Arc(Column.Core - Column.Far), MinFarSide, MaxFarSide));
	}
	constexpr double Sigma = 2.0;
	Shores = SmoothAround(Shores, Sigma);
	Flanks = SmoothAround(Flanks, Sigma);
	Upslopes = SmoothAround(Upslopes, Sigma);
	FarSides = SmoothAround(FarSides, Sigma);
	// The crests keep the holomap's highs and lows, between MinCrest and MaxCrest.
	TArray<double> Crests;
	for (int32 Column = 0; Column < AltitudeColumns; ++Column)
	{
		Crests.Add(FMath::Max(Altitudes[EquatorRow * AltitudeColumns + Column], Altitudes[BeyondEquatorRow * AltitudeColumns + Column]));
	}
	const double Lowest = FMath::Min(Crests);
	const double Highest = FMath::Max(FMath::Max(Crests), Lowest + 1.0);
	for (double& Crest : Crests)
	{
		Crest = FMath::Lerp(MinCrest, MaxCrest, (Crest - Lowest) / (Highest - Lowest));
	}

	// Each mesh column's profile, with some detail the holomap is too coarse for: the shore wanders, peaks vary.
	TArray<FProfile> Profiles;
	Profiles.SetNum(BeltColumns);
	for (int32 Column = 0; Column < BeltColumns; ++Column)
	{
		const double Longitude = 360.0 * Column / BeltColumns;
		const double TexturePosition = Longitude / 360.0 * TextureSize - 0.5;
		FProfile& Profile = Profiles[Column];
		Profile.Shore = SampleAround(Shores, TexturePosition);
		const double Along = Profile.Shore * FMath::DegreesToRadians(Longitude);
		Profile.Shore += 600.0 * Noise1D(Along / 4000.0) + 250.0 * Noise1D(Along / 1300.0 + 17.3);
		Profile.Shelf = ShelfWidth * (1.0 + 0.45 * Noise1D(Along / 3000.0 + 41.7) + 0.2 * Noise1D(Along / 700.0 + 3.9));
		Profile.Flank = SampleAround(Flanks, TexturePosition);
		Profile.Upslope = SampleAround(Upslopes, TexturePosition);
		Profile.FarSide = SampleAround(FarSides, TexturePosition);
		const double Crest = SplineAround(Crests, Longitude / (360.0 / AltitudeColumns)) * (1.0 + 0.15 * Noise1D(Along / 12000.0 + 5.1));
		Profile.Crest = FMath::Clamp(Crest, MinCrest, MaxCrest);
	}

	// The surface: positions on the map (pole at the origin), then normals across columns and rows.
	const TArray<FRow> Rows = MakeRows();
	const int32 NumRows = Rows.Num();
	TArray<FVector> Points;
	Points.SetNum(BeltColumns * NumRows);
	auto PointIndex = [NumRows](int32 Column, int32 Row) { return ((Column % BeltColumns + BeltColumns) % BeltColumns) * NumRows + Row; };
	for (int32 Column = 0; Column < BeltColumns; ++Column)
	{
		const FProfile& Profile = Profiles[Column];
		const double Angle = 2.0 * UE_DOUBLE_PI * Column / BeltColumns;
		const FVector2D Outward(FMath::Cos(Angle), FMath::Sin(Angle));
		const double CliffFoot = Profile.Flank;
		const double Crest = CliffFoot + CliffRun + Profile.Upslope;
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			const double T = Rows[Row].T;
			double S = 0.0;
			switch (Rows[Row].Part)
			{
			case EPart::Skirt: S = -Profile.Shelf - SkirtWidth * (1.0 - T); break;
			case EPart::Shelf: S = -Profile.Shelf * (1.0 - T); break;
			case EPart::Flank: S = CliffFoot * T; break;
			case EPart::Cliff: S = CliffFoot + CliffRun * T; break;
			case EPart::Upslope: S = CliffFoot + CliffRun + Profile.Upslope * T; break;
			case EPart::FarSide: S = Crest + Profile.FarSide * T; break;
			}
			FVector2D Map = Outward * (Profile.Shore + S);
			// Buttresses and gullies: the whole face juts out or back (as steep everywhere), easing off up the snow.
			const double Relief = Rows[Row].Part == EPart::Cliff ? 1.0 : Rows[Row].Part == EPart::Upslope ? FMath::Max(1.0 - 3.0 * T, 0.0) : 0.0;
			if (Relief > 0.0)
			{
				const FVector2D Foot = Outward * (Profile.Shore + CliffFoot);
				Map += Outward * (Relief * CliffRelief * (0.7 * Noise2D(Foot / 900.0) + 0.3 * Noise2D(Foot / 300.0 + FVector2D(4.2, 8.8))));
			}
			double Height = 0.0;
			switch (Rows[Row].Part)
			{
			case EPart::Skirt:
				Height = -FMath::Lerp(ShelfDepth, SkirtDepth, 1.0 - T);
				break;
			case EPart::Shelf:
				Height = -30.0 - (ShelfDepth - 30.0) * FMath::Square(1.0 - T) + 20.0 * Noise2D(Map / 900.0 + FVector2D(1.3, 6.6)) * FMath::Sin(T * UE_DOUBLE_PI);
				break;
			case EPart::Flank:
				Height = FlankTop * FMath::Pow(T, 1.6) + 60.0 * Noise2D(Map / 2500.0) * FMath::Sin(T * UE_DOUBLE_PI);
				break;
			case EPart::Cliff:
				Height = FMath::Lerp(FlankTop, CliffTop, T);
				break;
			case EPart::Upslope:
				Height = CliffTop + (Profile.Crest - CliffTop) * FMath::Sin(T * UE_DOUBLE_PI / 2.0)
					+ 0.35 * (Profile.Crest - CliffTop) * Ridges(Map) * FMath::Sin(T * UE_DOUBLE_PI);
				break;
			case EPart::FarSide:
				Height = (Profile.Crest + SkirtDepth) * FMath::Pow(FMath::Cos(T * UE_DOUBLE_PI / 2.0), 0.7) - SkirtDepth
					+ 0.15 * Profile.Crest * Ridges(Map) * FMath::Sin(T * UE_DOUBLE_PI);
				break;
			}
			Points[PointIndex(Column, Row)] = FVector(Map, FMath::Min(Height, MaxCrest));
		}
	}
	TArray<FVector> Normals;
	Normals.SetNum(Points.Num());
	for (int32 Column = 0; Column < BeltColumns; ++Column)
	{
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			const FVector Across = Points[PointIndex(Column, FMath::Min(Row + 1, NumRows - 1))] - Points[PointIndex(Column, FMath::Max(Row - 1, 0))];
			const FVector Along = Points[PointIndex(Column + 1, Row)] - Points[PointIndex(Column - 1, Row)];
			FVector Normal = (Across ^ Along).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			Normals[PointIndex(Column, Row)] = Normal.Z < 0.0 ? -Normal : Normal;
		}
	}

	// Colors, the holomap's (HOLOMAP.HQR texture in RESS palette 0): the side facing the islands is all ice, blue-white,
	// no bare rock; pale ice under the shallow water, light blue ice on the flank, deeper blue ice on the cliffs and
	// anything steep, white and blue-white snow where it is high and not too steep; brown rock on the far side.
	const FLinearColor ShelfColor = Srgb(176, 222, 204);
	const FLinearColor FlankColor = Srgb(170, 190, 200);
	const FLinearColor RockColor = Srgb(86, 122, 146);
	const FLinearColor FarRockColor = Srgb(131, 103, 55);
	const FLinearColor SnowColor = Srgb(232, 238, 246);
	const FLinearColor SnowShadeColor = Srgb(196, 216, 230);
	auto VertexAt = [&](const FVector& Pivot, int32 Column, int32 Row)
	{
		const FVector& Point = Points[PointIndex(Column, Row)];
		const FVector& Normal = Normals[PointIndex(Column, Row)];
		const EPart Part = Rows[Row].Part;
		const double Slope = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0, 1.0)));
		const double Steep = FMath::SmoothStep(38.0, 55.0, Slope);
		const double Snow = FMath::SmoothStep(CliffTop - 800.0, CliffTop + 400.0, Point.Z) * (1.0 - Steep);
		const bool bLow = Part == EPart::Skirt || Part == EPart::Shelf || Part == EPart::Flank;
		FLinearColor Color = bLow ? FMath::Lerp(FlankColor, RockColor, float(Steep)) : Part == EPart::FarSide ? FarRockColor : RockColor;
		Color = FMath::Lerp(ShelfColor, Color, float(FMath::Clamp(Point.Z / 100.0 + 1.0, 0.0, 1.0)));
		const FLinearColor Snowfield = FMath::Lerp(SnowShadeColor, SnowColor, float(FMath::SmoothStep(-0.2, 0.3, Noise2D(FVector2D(Point) / 2600.0 + FVector2D(8.2, 1.4)))));
		Color = FMath::Lerp(Color, Snowfield, float(Snow));
		Color *= float(0.92 + 0.08 * Noise2D(FVector2D(Point) / 1800.0 + FVector2D(3.7, 9.1)));
		FLBAVertex Vertex;
		Vertex.Position = FVector3f(Point - Pivot);
		Vertex.Normal = FVector3f(Normal);
		Vertex.Color = FVector4f(Color.R, Color.G, Color.B, 0.5f);
		return Vertex;
	};

	// A sector per altitude column: its own mesh and actor, around its shore's middle.
	UMaterialInterface* Material = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
	const FName Slot = FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color);
	const FString LevelName = FString::Printf(TEXT("L_%s_Belt"), *Planet);
	UWorld* World = Context.BeginLevel(Folder, LevelName);
	if (!World)
	{
		return nullptr;
	}
	constexpr int32 SectorColumns = BeltColumns / Sectors;
	int32 Triangles = 0;
	for (int32 Sector = 0; Sector < Sectors; ++Sector)
	{
		const int32 First = Sector * SectorColumns;
		const int32 Middle = First + SectorColumns / 2;
		const double Angle = 2.0 * UE_DOUBLE_PI * Middle / BeltColumns;
		const FVector Pivot(FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Profiles[Middle].Shore, 0.0);

		FLBAMeshBuilder Builder;
		const int32 Section = Builder.GetSection(Slot, Material);
		for (int32 Column = First; Column < First + SectorColumns; ++Column)
		{
			for (int32 Row = 0; Row + 1 < NumRows; ++Row)
			{
				const FLBAVertex A = VertexAt(Pivot, Column, Row);
				const FLBAVertex B = VertexAt(Pivot, Column + 1, Row);
				const FLBAVertex C = VertexAt(Pivot, Column, Row + 1);
				const FLBAVertex D = VertexAt(Pivot, Column + 1, Row + 1);
				Builder.AddTriangleFacing(Section, A, B, D, FVector3f::UpVector);
				Builder.AddTriangleFacing(Section, A, D, C, FVector3f::UpVector);
			}
		}
		Triangles += Builder.NumTriangles();

		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, Materials, Slots);
		FLBAStaticMeshOptions Options;
		Options.bComplexAsSimple = true;
		Options.bDeferBuild = false;
		UStaticMesh* Mesh = Context.CreateStaticMesh(Folder, FString::Printf(TEXT("SM_LBA_Belt_%s_%02d"), *Planet, Sector), MoveTemp(Description), Materials, Slots, Options);
		if (!Mesh)
		{
			continue;
		}
		// Bent onto the planet, it goes down around it: culled by where it is seen, not by where it is.
		Mesh->SetNegativeBoundsExtension(FVector(0.0, 0.0, 2.0 * Radius));
		Mesh->CalculateExtendedBounds();
		Context.MarkForSave(Mesh);
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Pivot), FString::Printf(TEXT("Belt_%02d"), Sector), TEXT("Belt")))
		{
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->Tags.Add(ULBAPlanetAsset::BeltTag);
		}
	}
	Context.EndLevel(World);

	double NearestShore = TNumericLimits<double>::Max();
	double FarthestShore = 0.0;
	double LowestCrest = TNumericLimits<double>::Max();
	double HighestCrest = 0.0;
	for (const FProfile& Profile : Profiles)
	{
		NearestShore = FMath::Min(NearestShore, Profile.Shore);
		FarthestShore = FMath::Max(FarthestShore, Profile.Shore);
		LowestCrest = FMath::Min(LowestCrest, Profile.Crest);
		HighestCrest = FMath::Max(HighestCrest, Profile.Crest);
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s belt: shore %.0f to %.0f m from the pole, crests %.0f to %.0f m, cliff band %.0f to %.0f m, %d sectors, %d triangles"),
		*Planet, NearestShore / 100.0, FarthestShore / 100.0, LowestCrest / 100.0, HighestCrest / 100.0, FlankTop / 100.0, CliffTop / 100.0, Sectors, Triangles);
	return TSoftObjectPtr<UWorld>(FSoftObjectPath(Folder / LevelName + TEXT(".") + LevelName));
}
