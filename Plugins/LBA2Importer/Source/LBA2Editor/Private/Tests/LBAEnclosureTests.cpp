#include "Import/LBAHollow.h"
#include "Import/LBARoomPlan.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The enclosure's concepts, each checked on a tiny made-up grid (not on the game's data): a room is a rectangle with four
 * walls under a ceiling at its room height; a hollow is a floor in the rock under a vault that springs from its edge and
 * rises inward, spans narrow gaps, and is held by what reaches it. Run: Automation RunTests LBA2.Enclosure
 */
namespace LBAEnclosureTests
{
	constexpr int32 Floor = 1;
	constexpr int32 Rock = 2;

	/** A grid of cells, all empty at first: collision shapes and bricks. */
	struct FTestGrid
	{
		TArray<uint8> Shapes;
		TArray<int32> Bricks;

		FTestGrid()
		{
			Shapes.SetNumZeroed(FLBARoomPlan::SizeXZ * FLBARoomPlan::SizeY * FLBARoomPlan::SizeXZ);
			Bricks.SetNumZeroed(Shapes.Num());
		}
		static int32 Index(int32 X, int32 Y, int32 Z) { return (Z * FLBARoomPlan::SizeXZ + X) * FLBARoomPlan::SizeY + Y; }
		bool Inside(int32 X, int32 Y, int32 Z) const
		{
			return X >= 0 && Z >= 0 && Y >= 0 && X < FLBARoomPlan::SizeXZ && Z < FLBARoomPlan::SizeXZ && Y < FLBARoomPlan::SizeY;
		}
		/** Fills a box of cells (corners included) with a solid brick (0: invisible collision). */
		void Fill(FIntVector Min, FIntVector Max, int32 Brick)
		{
			for (int32 X = Min.X; X <= Max.X; ++X)
			{
				for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
				{
					for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
					{
						Shapes[Index(X, Y, Z)] = 1;
						Bricks[Index(X, Y, Z)] = Brick;
					}
				}
			}
		}
		FLBAGridCells Cells() const
		{
			FLBAGridCells Cells;
			Cells.Shape = [this](int32 X, int32 Y, int32 Z) { return Inside(X, Y, Z) ? int32(Shapes[Index(X, Y, Z)]) : 0; };
			Cells.Brick = [this](int32 X, int32 Y, int32 Z) { return Inside(X, Y, Z) ? Bricks[Index(X, Y, Z)] : 0; };
			Cells.Layout = [this](int32 X, int32 Y, int32 Z) { return Inside(X, Y, Z) && Bricks[Index(X, Y, Z)] != 0 ? 0 : INDEX_NONE; };
			Cells.LayoutBase = [](int32 X, int32 Y, int32 Z) { return Y; };
			Cells.HasArt = [](int32 Brick) { return Brick != 0; };
			Cells.IsFrame = [](int32 X, int32 Y, int32 Z) { return false; };
			return Cells;
		}
		/** A floor slab at height 0 with a rock rim Rim cells high around it (corners included). */
		void Cave(FIntPoint Min, FIntPoint Max, int32 Rim)
		{
			Fill(FIntVector(Min.X, 0, Min.Y), FIntVector(Max.X, 0, Max.Y), Floor);
			Fill(FIntVector(Min.X - 1, 0, Min.Y - 1), FIntVector(Max.X + 1, Rim, Min.Y - 1), Rock);
			Fill(FIntVector(Min.X - 1, 0, Max.Y + 1), FIntVector(Max.X + 1, Rim, Max.Y + 1), Rock);
			Fill(FIntVector(Min.X - 1, 0, Min.Y), FIntVector(Min.X - 1, Rim, Max.Y), Rock);
			Fill(FIntVector(Max.X + 1, 0, Min.Y), FIntVector(Max.X + 1, Rim, Max.Y), Rock);
		}
	};

	FLBAHollow::FSettings CaveSettings()
	{
		FLBAHollow::FSettings Settings;
		Settings.Clear = 6;
		Settings.SpringReach = 2.f;
		Settings.Shape = FLBAVaultShape{ 6.f, 8.f, 0.f };
		return Settings;
	}
}
using namespace LBAEnclosureTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBAEnclosureRoomTest, "LBA2.Enclosure.Rooms.RectangleWithFourWalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLBAEnclosureRoomTest::RunTest(const FString& Parameters)
{
	// A 10x8 floor walled all round, 9 cells high: one room over the whole floor, its ceiling at its room height.
	FTestGrid Grid;
	Grid.Fill(FIntVector(10, 0, 10), FIntVector(19, 0, 17), Floor);
	Grid.Fill(FIntVector(9, 0, 9), FIntVector(20, 9, 9), Rock);
	Grid.Fill(FIntVector(9, 0, 18), FIntVector(20, 9, 18), Rock);
	Grid.Fill(FIntVector(9, 0, 10), FIntVector(9, 9, 17), Rock);
	Grid.Fill(FIntVector(20, 0, 10), FIntVector(20, 9, 17), Rock);
	const TSet<int32> WallBricks = { Rock };
	const FLBARoomPlan Plan(Grid.Cells(), &WallBricks, {}, 9);
	if (!TestEqual(TEXT("rooms"), Plan.GetRooms().Num(), 1))
	{
		return false;
	}
	const FLBAPlanRoom& Room = Plan.GetRooms()[0];
	TestTrue(TEXT("room min"), Room.Min == FIntPoint(10, 10));
	TestTrue(TEXT("room max"), Room.Max == FIntPoint(19, 17));
	TestEqual(TEXT("ceiling over the room height"), Room.Ceiling, Room.Level + 9 + 1);
	for (int32 Side = 0; Side < 4; ++Side)
	{
		const FIntPoint OnSide = Side == 0 ? FIntPoint(10, 13) : Side == 1 ? FIntPoint(19, 13) : Side == 2 ? FIntPoint(14, 10) : FIntPoint(14, 17);
		TestTrue(FString::Printf(TEXT("side %d is a wall"), Side), Plan.GetSide(0, OnSide + FLBARoomPlan::Sides[Side], Side) == ELBARoomSide::Wall);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBAEnclosureVaultTest, "LBA2.Enclosure.Vault.Shape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLBAEnclosureVaultTest::RunTest(const FString& Parameters)
{
	// A wide hall and a narrow corridor under one vault shape: coved and level in the hall, pointed in the corridor.
	TBitArray<> Footprint(false, FLBARoomPlan::SizeXZ * FLBARoomPlan::SizeXZ);
	for (int32 X = 0; X < 20; ++X)
	{
		for (int32 Z = 0; Z < 20; ++Z)
		{
			Footprint[FLBARoomPlan::ToColumnIndex(FIntPoint(10 + X, 10 + Z))] = true;
		}
		for (int32 Z = 0; Z < 3; ++Z)
		{
			Footprint[FLBARoomPlan::ToColumnIndex(FIntPoint(10 + X, 40 + Z))] = true;
		}
	}
	const FLBAVault Vault(Footprint, FLBAVaultShape{ 6.f, 8.f, 0.f });
	TestEqual(TEXT("springs at the wall"), Vault.GetRise(10.f, 20.f), 0.f, 0.01f);
	TestEqual(TEXT("level crown in the hall"), Vault.GetRise(20.f, 20.f), 8.f, 0.01f);
	TestTrue(TEXT("rises inward"), Vault.GetRise(11.f, 20.f) < Vault.GetRise(13.f, 20.f) && Vault.GetRise(13.f, 20.f) < Vault.GetRise(15.f, 20.f));
	const float Apex = Vault.GetRise(20.f, 41.5f);
	TestTrue(TEXT("the corridor's apex is lower than the crown"), Apex > 2.f && Apex < 8.f);
	TestEqual(TEXT("flat shape"), FLBAVault(Footprint, FLBAVaultShape()).GetRise(20.f, 20.f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBAEnclosureCaveTest, "LBA2.Enclosure.Hollow.CaveRoof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLBAEnclosureCaveTest::RunTest(const FString& Parameters)
{
	// A cave floor in a low rock rim: the floor is under the roof, the rim holds it; the roof springs Clear over the floor
	// at the rim and rises by the vault's rise in the middle.
	FTestGrid Grid;
	Grid.Cave(FIntPoint(10, 10), FIntPoint(29, 21), 3);
	const FLBAHollow Hollow(Grid.Cells(), CaveSettings());
	TestTrue(TEXT("floor under the roof"), Hollow.IsUnderRoof(FIntPoint(15, 15)));
	TestFalse(TEXT("the rim is rock"), Hollow.IsUnderRoof(FIntPoint(9, 15)));
	TestEqual(TEXT("floor"), Hollow.GetFloor(FIntPoint(15, 15)), 0);
	TestEqual(TEXT("springs at the rim"), Hollow.GetRoof(10.f, 16.f), 6.f, 0.01f);
	TestEqual(TEXT("full rise in the middle"), Hollow.GetRoof(20.f, 16.f), 14.f, 0.01f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBAEnclosureChasmTest, "LBA2.Enclosure.Hollow.ChasmSpanned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLBAEnclosureChasmTest::RunTest(const FString& Parameters)
{
	// Two floors either side of a chasm 4 columns across: one hollow, the roof over the chasm too. A gap wider than
	// twice the closing is rock between two hollows.
	FTestGrid Grid;
	Grid.Fill(FIntVector(10, 0, 10), FIntVector(19, 0, 19), Floor);
	Grid.Fill(FIntVector(24, 0, 10), FIntVector(33, 0, 19), Floor);
	Grid.Fill(FIntVector(10, 0, 40), FIntVector(19, 0, 49), Floor);
	Grid.Fill(FIntVector(30, 0, 40), FIntVector(39, 0, 49), Floor);
	const FLBAHollow Hollow(Grid.Cells(), CaveSettings());
	TestTrue(TEXT("narrow chasm under the roof"), Hollow.IsUnderRoof(FIntPoint(21, 15)));
	TestFalse(TEXT("wide gap is rock"), Hollow.IsUnderRoof(FIntPoint(25, 45)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBAEnclosureWayOutTest, "LBA2.Enclosure.Hollow.WayOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLBAEnclosureWayOutTest::RunTest(const FString& Parameters)
{
	// A raised passage of four cells, too small to be ground: in the hollow when an exit's zone is on it (never walled off).
	FTestGrid Grid;
	Grid.Cave(FIntPoint(10, 10), FIntPoint(29, 21), 3);
	Grid.Fill(FIntVector(40, 12, 15), FIntVector(43, 12, 15), Floor);
	FLBACellBox Exit;
	Exit.Min = FIntVector(42, 13, 15);
	Exit.Max = FIntVector(43, 16, 15);
	TestFalse(TEXT("no exit: not ground"), FLBAHollow(Grid.Cells(), CaveSettings()).IsUnderRoof(FIntPoint(42, 15)));
	const FLBAHollow Hollow(Grid.Cells(), CaveSettings(), { Exit });
	TestTrue(TEXT("the way out is under the roof"), Hollow.IsUnderRoof(FIntPoint(42, 15)));
	TestEqual(TEXT("its floor"), Hollow.GetFloor(FIntPoint(42, 15)), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLBAEnclosureSupportTest, "LBA2.Enclosure.Hollow.WhatHoldsTheRoof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLBAEnclosureSupportTest::RunTest(const FString& Parameters)
{
	// In a wide cave: a pillar up to the roof holds it; a lamp post and an invisible wall (no art) stand under it.
	FTestGrid Grid;
	Grid.Cave(FIntPoint(10, 10), FIntPoint(39, 39), 3);
	Grid.Fill(FIntVector(20, 1, 20), FIntVector(20, 20, 20), Rock);
	Grid.Fill(FIntVector(30, 1, 30), FIntVector(30, 3, 30), Rock);
	Grid.Fill(FIntVector(25, 1, 15), FIntVector(25, 20, 15), 0);
	const FLBAHollow Hollow(Grid.Cells(), CaveSettings());
	TestFalse(TEXT("pillar holds the roof"), Hollow.IsUnderRoof(FIntPoint(20, 20)));
	TestTrue(TEXT("the roof comes down on the pillar"), Hollow.GetRoof(21.f, 20.5f) < Hollow.GetRoof(24.f, 20.5f));
	TestTrue(TEXT("lamp post under the roof"), Hollow.IsUnderRoof(FIntPoint(30, 30)));
	TestTrue(TEXT("invisible wall under the roof"), Hollow.IsUnderRoof(FIntPoint(25, 15)));
	return true;
}

#endif
