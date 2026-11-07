#include "Formats/LBAScene.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Run: Automation RunTests LBA2.Scenes.DoubleDoors */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBADoubleDoorsTest, "LBA2.Scenes.DoubleDoors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLBADoubleDoorsTest::RunTest(const FString& Parameters)
{
	using LBAScenes::FDoorLeaf;
	const FIntVector Left(-1, 0, 0), Right(1, 0, 0), Up(0, 0, -1), Down(0, 0, 1);
	const TArray<FDoorLeaf> Leaves =
	{
		// Scene 10, objects 7 and 8: exactly a leaf apart.
		{ FIntVector(10240, 2560, 15104), Left, 768 }, { FIntVector(11008, 2560, 15104), Right, 768 },
		// Scene 10, objects 23 and 24: a unit less, along z.
		{ FIntVector(4640, 3584, 20720), Up, 768 }, { FIntVector(4640, 3584, 21487), Down, 768 },
		// Scene 11, objects 22 and 21: 16 units more.
		{ FIntVector(5744, 2304, 13664), Left, 768 }, { FIntVector(6528, 2304, 13664), Right, 768 },
		// Sliding towards each other: not a pair.
		{ FIntVector(20000, 0, 0), Right, 768 }, { FIntVector(20768, 0, 0), Left, 768 },
		// A floor apart.
		{ FIntVector(30000, 0, 0), Left, 768 }, { FIntVector(30768, 256, 0), Right, 768 },
		// Too far apart.
		{ FIntVector(40000, 0, 0), Left, 768 }, { FIntVector(42000, 0, 0), Right, 768 },
		// On parallel lines.
		{ FIntVector(50000, 0, 0), Left, 768 }, { FIntVector(50768, 0, 512), Right, 768 },
	};
	const TArray<int32> Expected = { 1, 0, 3, 2, 5, 4, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE };
	const TArray<int32> Pairs = LBAScenes::FindDoubleDoors(Leaves);
	TestEqual(TEXT("One result per leaf"), Pairs.Num(), Expected.Num());
	for (int32 Index = 0; Index < FMath::Min(Pairs.Num(), Expected.Num()); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("Leaf %d"), Index), Pairs[Index], Expected[Index]);
	}
	return true;
}

#endif
