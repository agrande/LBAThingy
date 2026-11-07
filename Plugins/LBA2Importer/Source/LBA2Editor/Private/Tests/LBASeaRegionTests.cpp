#include "Engine/Texture2D.h"
#include "Import/LBASeaRegions.h"
#include "LBAPlanet.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The imported globe's sea regions (LBASeaRegions): ice by the belt, turquoise by the desert, nothing in Citadel's
 * harbour. Needs Twinsun imported (-Layers=Islands -Islands=CITABAU,DESERT). Run: Automation RunTests LBA2.Globe.SeaRegions
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
	UTexture2D* Mask = Planet->SeaRegions.LoadSynchronous();
	if (!TestNotNull(TEXT("Twinsun has its sea regions"), Mask))
	{
		return false;
	}
	TestEqual(TEXT("Mask extent"), double(Planet->SeaRegionsExtent), LBASeaRegions::MaskExtent);
	TArray64<uint8> Data;
	const int32 Size = Mask->Source.GetSizeX();
	if (!TestTrue(TEXT("Mask source is BGRA8"), Mask->Source.GetFormat() == TSF_BGRA8 && Mask->Source.GetMipData(Data, 0) && Data.Num() == int64(Size) * Size * 4))
	{
		return false;
	}
	// Meters to the belt's land, the turquoise islands' and the others' at a map point (cm).
	auto At = [&](const FVector2D& Map)
	{
		const int32 X = FMath::Clamp(FMath::FloorToInt32((Map.X / (2.0 * Planet->SeaRegionsExtent) + 0.5) * Size), 0, Size - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt32((Map.Y / (2.0 * Planet->SeaRegionsExtent) + 0.5) * Size), 0, Size - 1);
		const uint8* Texel = Data.GetData() + (int64(Y) * Size + X) * 4;
		return FIntVector(Texel[2], Texel[1], Texel[0]);
	};

	const int32 Home = Planet->FindIslandIndex(TEXT("CITABAU"));
	const int32 Desert = Planet->FindIslandIndex(TEXT("DESERT"));
	if (!TestTrue(TEXT("Citadel and the desert are on Twinsun"), Home != INDEX_NONE && Desert != INDEX_NONE))
	{
		return false;
	}
	// Citadel's harbour (the reference shot's pose in its level): far from the belt and from the desert.
	const FVector2D Harbour(Planet->Islands[Home].GetLevelToMap(Planet->Radius).TransformPosition(FVector(31986.0, 38591.0, 0.0)));
	const FIntVector InHarbour = At(Harbour);
	TestTrue(*FString::Printf(TEXT("Harbour far from the belt (%d m)"), InHarbour.X), InHarbour.X > 100);
	TestTrue(*FString::Printf(TEXT("Harbour far from the desert (%d m)"), InHarbour.Y), InHarbour.Y > 70);
	TestTrue(*FString::Printf(TEXT("Harbour by Citadel (%d m)"), InHarbour.Z), InHarbour.Z < 30);

	// The desert's coast.
	const FLBAPlanetIsland& DesertIsland = Planet->Islands[Desert];
	if (TestTrue(TEXT("The desert has a coastline"), DesertIsland.Coastline.Num() > 2))
	{
		const FTransform LevelToMap = DesertIsland.GetLevelToMap(Planet->Radius);
		int32 Farthest = 0;
		for (int32 Index = 0; Index < DesertIsland.Coastline.Num(); Index += 16)
		{
			Farthest = FMath::Max(Farthest, At(FVector2D(LevelToMap.TransformPosition(FVector(DesertIsland.Coastline[Index], 0.0)))).Y);
		}
		TestTrue(*FString::Printf(TEXT("The desert's coast is turquoise (up to %d m off)"), Farthest), Farthest <= 2);
	}

	// From the pole out past the harbour, the belt's land comes; around the pole it is far.
	TestEqual(TEXT("The pole is far from the belt"), At(FVector2D::ZeroVector).X, 255);
	const FVector2D Out = Harbour.GetSafeNormal();
	int32 Nearest = 255;
	for (double Rho = 40000.0; Rho < 60000.0; Rho += 100.0)
	{
		Nearest = FMath::Min(Nearest, At(Out * Rho).X);
	}
	TestEqual(TEXT("The belt's land lies 400 to 600 m from the pole past the harbour"), Nearest, 0);

	// The island's sea texture recolored: its 4 ice steps and 4 turquoise steps.
	const UTexture2D* Atlas = LoadObject<UTexture2D>(nullptr, TEXT("/Game/LBA2/Islands/CITABAU/Textures/T_CITABAU_SeaRegions.T_CITABAU_SeaRegions"));
	if (TestNotNull(TEXT("Citadel's sea region atlas"), Atlas))
	{
		TestEqual(TEXT("Atlas height"), int32(Atlas->Source.GetSizeY()), int32(Atlas->Source.GetSizeX()) * 2 * LBASeaRegions::Steps);
	}
	return true;
}

#endif
