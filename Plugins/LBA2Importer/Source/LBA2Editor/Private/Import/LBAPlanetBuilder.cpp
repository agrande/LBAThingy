#include "Import/LBAPlanetBuilder.h"

#include "Engine/StaticMesh.h"
#include "Formats/LBAHqr.h"
#include "Formats/LBAIsland.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBARenderingAssets.h"
#include "Import/LBAWater.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"
#include "LBATypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"

namespace
{
	struct FPlanetIslandPreset
	{
		const TCHAR* Island;
		/** Its index on the holomap (ARROW.BIN), the scene format's island number. */
		int32 HolomapIsland;
		/** On the globe; otherwise it only steers where the planet's sun is. */
		bool bPlaced;
		/**
		 * Its sky light's intensity on the planet relative to its level's (FLBAPlanetIsland::SkyLightGain): measured by
		 * opening it alone on the planet (lba.Globe.Islands 0) and trying gains (lba.Globe.SkyLightGain) until its
		 * ground is as light as in its level, shot from the same poses (Citadel: harbour paving and park grass, 0.88 and
		 * 1.02 of the globe's light; desert: sand and town, 0.93; the clouds vary from run to run).
		 */
		float SkyLightGain = 1.f;
	};

	struct FPlanetPreset
	{
		const TCHAR* Name;
		/** Sea level radius (cm). */
		double Radius;
		/** The world stays flat this far around the camera (cm), then bends (ULBAPlanetAsset::FlatRadius). */
		float FlatRadius;
		/** The first placed island is the planet's home: its sea's look is the planet's. */
		TArray<FPlanetIslandPreset> Islands;
	};

	const TArray<FPlanetPreset>& GetPresets()
	{
		// Twinsun at 750 m (2026-10-26, was 2 km): Citadel island (its sunny variant) and the desert island, 315 m apart.
		// Flat 100 m around the camera: the island under it stays flat, the desert sinks 30 m seen from Citadel.
		static const TArray<FPlanetPreset> Presets =
		{
			{ TEXT("TWINSUN"), 75000.0, 10000.f, { { TEXT("CITABAU"), 0, true, 1.55f }, { TEXT("DESERT"), 2, true, 1.35f } } },
		};
		return Presets;
	}

	const FPlanetPreset* FindPreset(const FString& Island)
	{
		for (const FPlanetPreset& Preset : GetPresets())
		{
			for (const FPlanetIslandPreset& Entry : Preset.Islands)
			{
				if (Island.Equals(Entry.Island, ESearchCase::IgnoreCase))
				{
					return &Preset;
				}
			}
		}
		return nullptr;
	}

	/** Sea around an island's land that is still its own (cm): its reach. */
	constexpr double ReachMargin = 2000.0;

	/** Sea disc rings: from the center out to this share of half the planet's circumference, each this much wider. */
	constexpr double SeaReach = 0.9;
	constexpr double SeaInnerRing = 50.0;
	constexpr double SeaRingGrowth = 1.035;
	constexpr int32 SeaSegments = 256;

	/**
	 * An island's place on the holomap: ARROW.BIN (HOLOMAP.HQR entry 12) holds 32-byte T_ARROW records (HOLO.H), the
	 * first ones per island; Alpha is its latitude, Beta its longitude (4096 per turn; HOLOGLOB.CPP places them by
	 * turning +Z by Alpha, then Beta). North is positive Alpha (assumed; the holomap's east-west sense isn't checked).
	 */
	bool ReadHolomapPosition(FLBAImportContext& Context, int32 HolomapIsland, double& OutLatitude, double& OutLongitude)
	{
		constexpr int32 ArrowEntry = 12;
		constexpr int32 ArrowSize = 32;
		const FLBAHqrFile* Holomap = Context.GetHqr(TEXT("HOLOMAP.HQR"));
		TArray<uint8> Arrows;
		if (!Holomap || !Holomap->GetEntry(ArrowEntry, Arrows) || Arrows.Num() < (HolomapIsland + 1) * ArrowSize)
		{
			return false;
		}
		const uint8* Arrow = Arrows.GetData() + HolomapIsland * ArrowSize;
		int32 Alpha = 0;
		int32 Beta = 0;
		FMemory::Memcpy(&Alpha, Arrow + 12, sizeof(int32));
		FMemory::Memcpy(&Beta, Arrow + 16, sizeof(int32));
		OutLatitude = LBA::AngleToDegrees(Alpha);
		OutLongitude = LBA::AngleToDegrees(Beta);
		return true;
	}

	/** Direction towards an island's sun in its level: its first section's light angles, like its level's Sun. */
	bool ReadSunDirection(FLBAImportContext& Context, const FString& Island, FVector& OutDirection)
	{
		const FLBAHqrFile* Ile = Context.GetHqr(Island + TEXT(".ILE"));
		FLBAIsland Data;
		if (!Ile || !Data.Load(*Ile) || Data.Sections.IsEmpty())
		{
			return false;
		}
		OutDirection = -LBAEnvironment::SunRotationFromLbaAngles(Data.Sections[0].LightAlpha, Data.Sections[0].LightBeta).Vector();
		return true;
	}

	FVector PlanetDirection(double Latitude, double Longitude)
	{
		FLBAPlanetIsland Probe;
		Probe.Latitude = Latitude;
		Probe.Longitude = Longitude;
		return Probe.GetUp();
	}

	/**
	 * The sun direction that best gives each island (planet up, sine of its sun's elevation) its elevation, searched
	 * over the sphere every quarter degree; among equally good ones, the closest to the north pole.
	 */
	FVector SolveSunDirection(const TArray<TPair<FVector, double>>& Islands)
	{
		constexpr int32 Steps = 4;
		FVector Best = FVector::UpVector;
		double BestScore = TNumericLimits<double>::Max();
		for (int32 LatStep = 0; LatStep <= 180 * Steps; ++LatStep)
		{
			const double Lat = FMath::DegreesToRadians(-90.0 + double(LatStep) / Steps);
			for (int32 LonStep = 0; LonStep < 360 * Steps; ++LonStep)
			{
				const double Lon = FMath::DegreesToRadians(double(LonStep) / Steps);
				const FVector Sun(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
				double Score = 1e-6 * (1.0 - Sun.Z);
				for (const TPair<FVector, double>& Island : Islands)
				{
					Score += FMath::Square(FVector::DotProduct(Sun, Island.Key) - Island.Value);
				}
				if (Score < BestScore)
				{
					BestScore = Score;
					Best = Sun;
				}
			}
		}
		return Best;
	}

	/** Heading (degrees) that turns the island's horizontal sun direction to the planet's sun as seen from there. */
	double SolveHeading(const FLBAPlanetIsland& Island, const FVector& LevelSun, const FVector& PlanetSun)
	{
		const FVector Up = Island.GetUp();
		const double Lon = FMath::DegreesToRadians(Island.Longitude);
		const FVector East(-FMath::Sin(Lon), FMath::Cos(Lon), 0.0);
		const FVector North = FVector::CrossProduct(Up, East);
		const FVector Horizontal = PlanetSun - Up * FVector::DotProduct(PlanetSun, Up);
		const double PlanetAzimuth = FMath::Atan2(FVector::DotProduct(Horizontal, North), FVector::DotProduct(Horizontal, East));
		const double LevelAzimuth = FMath::Atan2(LevelSun.Y, LevelSun.X);
		return FRotator::NormalizeAxis(FMath::RadiansToDegrees(PlanetAzimuth - LevelAzimuth));
	}

	/** The sea disc: flat rings around its center, which the sea material bends onto the planet. */
	UStaticMesh* BuildSeaDisc(FLBAImportContext& Context, const FString& Folder, const FString& Name, double Radius, UMaterialInterface* Material)
	{
		const double Reach = SeaReach * UE_DOUBLE_PI * Radius;
		TArray<double> Rings = { 0.0 };
		for (double Ring = SeaInnerRing; Ring < Reach; Ring *= SeaRingGrowth)
		{
			Rings.Add(Ring);
		}
		Rings.Add(Reach);

		FLBAMeshBuilder Builder;
		const int32 Section = Builder.GetSection(TEXT("Sea"), Material);
		auto Vertex = [&](int32 Ring, int32 Segment)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * double(Segment) / SeaSegments;
			FLBAVertex V;
			V.Position = FVector3f(float(FMath::Cos(Angle) * Rings[Ring]), float(FMath::Sin(Angle) * Rings[Ring]), 0.f);
			return V;
		};
		for (int32 Ring = 0; Ring + 1 < Rings.Num(); ++Ring)
		{
			for (int32 Segment = 0; Segment < SeaSegments; ++Segment)
			{
				const FLBAVertex A = Vertex(Ring, Segment);
				const FLBAVertex B = Vertex(Ring, Segment + 1);
				const FLBAVertex C = Vertex(Ring + 1, Segment);
				const FLBAVertex D = Vertex(Ring + 1, Segment + 1);
				if (Ring > 0)
				{
					Builder.AddTriangleFacing(Section, A, B, D, FVector3f::UpVector);
				}
				Builder.AddTriangleFacing(Section, A, D, C, FVector3f::UpVector);
			}
		}

		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, Materials, Slots);
		FLBAStaticMeshOptions Options;
		Options.bNoCollision = true;
		Options.bDeferBuild = false;
		Options.bNoDistanceField = true;
		UStaticMesh* Mesh = Context.CreateStaticMesh(Folder, Name, MoveTemp(Description), Materials, Slots, Options);
		if (Mesh)
		{
			// Bent onto the planet, the disc wraps down around it.
			Mesh->SetNegativeBoundsExtension(FVector(0.0, 0.0, 2.0 * Radius));
			Mesh->SetPositiveBoundsExtension(FVector::ZeroVector);
			Mesh->CalculateExtendedBounds();
			Context.MarkForSave(Mesh);
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet sea disc %s: %d rings out to %.0f m, %d triangles"), *Name, Rings.Num(), Reach / 100.0, Builder.NumTriangles());
		return Mesh;
	}
}

bool LBAPlanetBuilder::IsPlaced(const FString& IslandName)
{
	const FPlanetPreset* Preset = FindPreset(IslandName);
	return Preset && Preset->Islands.ContainsByPredicate([&IslandName](const FPlanetIslandPreset& Entry)
	{
		return Entry.bPlaced && IslandName.Equals(Entry.Island, ESearchCase::IgnoreCase);
	});
}

void LBAPlanetBuilder::AddIsland(FLBAImportContext& Context, const FLBAIsland& Island, const FString& IslandFolder, const FLBAEnvironmentPreset& Preset,
	const FLBAEnvironmentLights& Lights)
{
	const FPlanetPreset* Planet = FindPreset(Island.Name);
	if (!Planet || !IsPlaced(Island.Name))
	{
		return;
	}
	const double Radius = Planet->Radius;
	const float Scale = Context.GetScale();

	// Where every listed island is, and its sun: the planet's sun follows from them.
	FVector LevelSun = FVector::UpVector;
	FLBAPlanetIsland Entry;
	TArray<TPair<FVector, double>> Elevations;
	for (const FPlanetIslandPreset& Listed : Planet->Islands)
	{
		double Latitude = 0.0;
		double Longitude = 0.0;
		FVector Sun;
		if (!ReadHolomapPosition(Context, Listed.HolomapIsland, Latitude, Longitude) || !ReadSunDirection(Context, Listed.Island, Sun))
		{
			Context.Warn(FString::Printf(TEXT("Planet %s: no holomap position or sun for %s"), Planet->Name, Listed.Island));
			continue;
		}
		Elevations.Add({ PlanetDirection(Latitude, Longitude), Sun.Z });
		if (Island.Name.Equals(Listed.Island, ESearchCase::IgnoreCase))
		{
			Entry.SkyLightGain = Listed.SkyLightGain;
			Entry.Latitude = Latitude;
			Entry.Longitude = Longitude;
			LevelSun = Sun;
		}
	}
	const FVector PlanetSun = SolveSunDirection(Elevations);
	Entry.Heading = SolveHeading(Entry, LevelSun, PlanetSun);
	Entry.Island = FName(*Island.Name);
	const FString LevelName = TEXT("L_") + Island.Name;
	Entry.Level = TSoftObjectPtr<UWorld>(FSoftObjectPath(IslandFolder / LevelName + TEXT(".") + LevelName));

	// The island's land and a margin around it.
	FBox2D Land(ForceInit);
	TArray<FVector2D> LandPoints;
	for (const FLBAIslandSection& Section : Island.Sections)
	{
		const FIntVector Origin = Section.GetOrigin();
		for (int32 Z = 0; Z < 65; ++Z)
		{
			for (int32 X = 0; X < 65; ++X)
			{
				if (Section.GetHeight(X, Z) > 0)
				{
					const FVector2D Point(LBA::ToUnreal(Origin.X + X * LBA::BrickSizeXZ, 0, Origin.Z + Z * LBA::BrickSizeXZ, Scale));
					Land += Point;
					LandPoints.Add(Point);
				}
			}
		}
	}
	Entry.Center = Land.bIsValid ? Land.GetCenter() : FVector2D::ZeroVector;
	double LandRadius = 0.0;
	for (const FVector2D& Point : LandPoints)
	{
		LandRadius = FMath::Max(LandRadius, FVector2D::Distance(Point, Entry.Center));
	}
	Entry.Reach = float(LandRadius + ReachMargin);
	const FString PlanetFolder = Context.GetContentPath(TEXT("Planets"));

	// The planet's shared assets; the home island gives the sea its look.
	FLBARenderingAssets& Rendering = Context.GetRendering();
	UMaterialInstanceConstant* SeaMaterial = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(PlanetFolder,
		FString::Printf(TEXT("MI_LBA_PlanetSea_%s"), Planet->Name));
	const bool bHome = Planet->Islands.FindByPredicate([](const FPlanetIslandPreset& Listed) { return Listed.bPlaced; })->Island == Island.Name;
	if (SeaMaterial && bHome)
	{
		const FLBAWaterLook Look = LBAWater::ComputeLook(Preset, Lights);
		UMaterialEditingLibrary::SetMaterialInstanceParent(SeaMaterial, Rendering.GetPlanetSeaMaster());
		UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(SeaMaterial);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(SeaMaterial, TEXT("Scattering"), Look.Scattering);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(SeaMaterial, TEXT("Absorption"), Look.Absorption);
		UMaterialEditingLibrary::UpdateMaterialInstance(SeaMaterial);
		Context.MarkForSave(SeaMaterial);
	}
	UStaticMesh* SeaMesh = BuildSeaDisc(Context, PlanetFolder, FString::Printf(TEXT("SM_LBA_PlanetSea_%s"), Planet->Name), Radius, SeaMaterial);

	ULBAPlanetAsset* Asset = Context.CreateOrReuseAsset<ULBAPlanetAsset>(PlanetFolder, FString::Printf(TEXT("DA_Planet_%s"), Planet->Name));
	if (!Asset)
	{
		return;
	}
	Asset->Modify();
	Asset->Radius = Radius;
	Asset->FlatRadius = Planet->FlatRadius;
	Asset->SunDirection = PlanetSun;
	Asset->SeaMesh = SeaMesh;
	Asset->SeaMaterial = SeaMaterial;
	Asset->MaterialParameters = Rendering.GetPlanetParameters();
	// Islands of other planets or no longer placed go; this one is replaced.
	Asset->Islands.RemoveAll([](const FLBAPlanetIsland& Placed) { return !IsPlaced(Placed.Island.ToString()); });
	if (FLBAPlanetIsland* Existing = Asset->Islands.FindByPredicate([&Entry](const FLBAPlanetIsland& Placed) { return Placed.Island == Entry.Island; }))
	{
		*Existing = Entry;
	}
	else
	{
		Asset->Islands.Add(Entry);
	}
	// Laid flat side by side, islands can't overlap; on the map, distances stretch away from the pole.
	for (const FLBAPlanetIsland& Other : Asset->Islands)
	{
		const double Apart = Radius * FMath::Acos(FMath::Clamp(FVector::DotProduct(Other.GetUp(), Entry.GetUp()), -1.0, 1.0));
		const double OnMap = FVector2D::Distance(Other.GetMapLocation(Radius), Entry.GetMapLocation(Radius));
		if (Other.Island == Entry.Island)
		{
			continue;
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s: %s to %s, %.0f m on the planet, %.0f m on the map"), Planet->Name, *Island.Name, *Other.Island.ToString(),
			Apart / 100.0, OnMap / 100.0);
		if (OnMap < Other.Reach + Entry.Reach)
		{
			Context.Warn(FString::Printf(TEXT("Planet %s: %s and %s are %.0f m apart on the map, they overlap (%.0f and %.0f m)"),
				Planet->Name, *Island.Name, *Other.Island.ToString(), OnMap / 100.0, Entry.Reach / 100.0, Other.Reach / 100.0));
		}
	}
	Context.MarkForSave(Asset);

	const double Elevation = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FVector::DotProduct(PlanetSun, Entry.GetUp()), -1.0, 1.0)));
	const FVector2D Map = Entry.GetMapLocation(Radius);
	UE_LOG(LogLBA2, Display, TEXT("LBA2 planet %s: %s at latitude %.1f, longitude %.1f, heading %.1f (map (%.0f, %.0f) m, yaw %.1f); sun elevation %.1f on the planet vs %.1f in the level; land %.0f m around (%.0f, %.0f); sun towards %s"),
		Planet->Name, *Island.Name, Entry.Latitude, Entry.Longitude, Entry.Heading, Map.X / 100.0, Map.Y / 100.0, Entry.GetMapYaw(), Elevation,
		FMath::RadiansToDegrees(FMath::Asin(LevelSun.Z)), LandRadius / 100.0, Entry.Center.X, Entry.Center.Y, *PlanetSun.ToString());
}
