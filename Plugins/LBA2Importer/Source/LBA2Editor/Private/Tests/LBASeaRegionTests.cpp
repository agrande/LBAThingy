#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "Import/LBASeaRegions.h"
#include "LBAPlanet.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The globe's sea by latitude (LBASeaRegions): the islands lie where the default latitudes expect them (the desert's
 * sea turquoise, Citadel's blue between turquoise and ice), and the atlas's turquoise is the oasis's teal, not its
 * greens. Needs Twinsun imported (-Layers=Islands -Islands=CITABAU,DESERT). Run: Automation RunTests LBA2.Globe.SeaRegions
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBASeaRegionsTest, "LBA2.Globe.SeaRegions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLBASeaRegionsTest::RunTest(const FString& Parameters)
{
	const ULBAPlanetAsset* Planet = LoadObject<ULBAPlanetAsset>(nullptr, TEXT("/Game/LBA2/Planets/DA_Planet_TWINSUN.DA_Planet_TWINSUN"));
	if (!TestNotNull(TEXT("Twinsun is imported"), Planet))
	{
		return false;
	}
	auto Degrees = [](const TCHAR* Name)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable ? double(Variable->GetFloat()) : 0.0;
	};
	// An island's coast's latitudes (degrees from the equator), southmost and northmost.
	auto Latitudes = [Planet](const TCHAR* Name, double& South, double& North)
	{
		const int32 Index = Planet->FindIslandIndex(Name);
		South = 0.0;
		North = 90.0;
		if (Index == INDEX_NONE || Planet->Islands[Index].Coastline.Num() < 3)
		{
			return false;
		}
		const FTransform LevelToMap = Planet->Islands[Index].GetLevelToMap(Planet->Radius);
		for (const FVector2D& Point : Planet->Islands[Index].Coastline)
		{
			const double Latitude = 90.0 - FMath::RadiansToDegrees(FVector2D(LevelToMap.TransformPosition(FVector(Point, 0.0))).Size() / Planet->Radius);
			South = FMath::Max(South, Latitude);
			North = FMath::Min(North, Latitude);
		}
		return true;
	};
	// The two islands lie side by side, so Citadel's south shares the desert's latitudes: the desert's sea is turquoise
	// all around, Citadel's harbour mostly blue, no ice on Citadel's coasts.
	const double Turquoise = Degrees(TEXT("lba.Globe.SeaTurquoiseFrom"));
	const double HalfTurquoise = 0.5 * (Turquoise + Degrees(TEXT("lba.Globe.SeaTurquoiseTo")));
	double South, North;
	if (TestTrue(TEXT("The desert has a coastline"), Latitudes(TEXT("DESERT"), South, North)))
	{
		TestTrue(*FString::Printf(TEXT("The desert's sea is turquoise (its coast %.1f to %.1f degrees)"), South, North),
			North > Turquoise && South > HalfTurquoise);
	}
	if (TestTrue(TEXT("Citadel has a coastline"), Latitudes(TEXT("CITABAU"), South, North)))
	{
		TestTrue(*FString::Printf(TEXT("No ice on Citadel's coasts (%.1f to %.1f degrees)"), South, North), North > Degrees(TEXT("lba.Globe.SeaIceFrom")));
		// Its harbour (the reference shot's pose in its level).
		const int32 Home = Planet->FindIslandIndex(TEXT("CITABAU"));
		const FVector2D Harbour(Planet->Islands[Home].GetLevelToMap(Planet->Radius).TransformPosition(FVector(31986.0, 38591.0, 0.0)));
		const double HarbourLatitude = 90.0 - FMath::RadiansToDegrees(Harbour.Size() / Planet->Radius);
		TestTrue(*FString::Printf(TEXT("Citadel's harbour is mostly blue (%.1f degrees)"), HarbourLatitude), HarbourLatitude < HalfTurquoise);
	}

	// The island's sea texture recolored: its ice steps and turquoise.
	UTexture2D* Atlas = LoadObject<UTexture2D>(nullptr, TEXT("/Game/LBA2/Islands/CITABAU/Textures/T_CITABAU_SeaRegions.T_CITABAU_SeaRegions"));
	if (!TestNotNull(TEXT("Citadel's sea region atlas"), Atlas))
	{
		return false;
	}
	const int32 Size = Atlas->Source.GetSizeX();
	TestEqual(TEXT("Atlas height"), int32(Atlas->Source.GetSizeY()), Size * LBASeaRegions::Slots);
	TArray64<uint8> Data;
	if (TestTrue(TEXT("Atlas source is BGRA8"), Atlas->Source.GetFormat() == TSF_BGRA8 && Atlas->Source.GetMipData(Data, 0)
		&& Data.Num() == int64(Size) * Size * LBASeaRegions::Slots * 4))
	{
		// Teal and blue, never the oasis's greens (more green than blue): from those the shallows went neon.
		int32 Green = 0;
		TSet<uint32> Colors;
		for (int64 Texel = int64(Size) * Size * LBASeaRegions::IceSteps; Texel < int64(Size) * Size * LBASeaRegions::Slots; ++Texel)
		{
			const uint8* BGRA = Data.GetData() + Texel * 4;
			Green += BGRA[1] > BGRA[0] ? 1 : 0;
			Colors.Add(BGRA[0] | (BGRA[1] << 8) | (BGRA[2] << 16));
		}
		TestEqual(TEXT("Green texels in the turquoise"), Green, 0);
		TestTrue(*FString::Printf(TEXT("The turquoise keeps the sea's pattern (%d colors)"), Colors.Num()), Colors.Num() >= 5);
	}
	return true;
}

#endif
