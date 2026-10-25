#include "HAL/FileManager.h"
#include "Import/LBAGridPlanText.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAIsoImporter.h"
#include "LBA2ImporterSettings.h"
#include "LBAGridPlan.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Golden plans: each grid's plan, made afresh from the game's data with the current code and settings, must read exactly
 * as its file in the plugin's Tests/Plans (written by the last import of the interiors and kept in git). A difference
 * is a change to how the importer understands that grid: if it is meant, reimport the interiors (which rewrites the
 * files) and commit them; git diff shows what changed. Needs the game's data, and the lba2remake checkout when the
 * import uses its models (its grids.json fixes some grids' cells).
 * Run: Automation RunTests LBA2.Enclosure.Plans
 */
namespace LBAGridPlanTests
{
	/** One importer making plans for all the tests (it learns each library's wall bricks once). */
	struct FPlans
	{
		FLBAImportContext Context;
		FLBAIsoImporter Importer;
		FString Error;
		bool bReady = false;

		/** The importer's settings, with the game and the remake found where they usually are when none is set (as the import commandlet does). */
		static const ULBA2ImporterSettings& GetSettings()
		{
			ULBA2ImporterSettings* Settings = GetMutableDefault<ULBA2ImporterSettings>();
			Settings->AutoDetectGameDirectory();
			Settings->AutoDetectRemakeDirectory();
			return *Settings;
		}

		FPlans()
			: Context(GetSettings())
			, Importer(Context)
		{
			bReady = Context.Initialize(Error) && Importer.BeginPlans();
			if (!bReady && Error.IsEmpty())
			{
				Error = TEXT("LBA_BKG.HQR is missing or unreadable");
			}
		}

		/** Made on first use, kept until the editor quits (never destroyed: its context outlives the engine's objects). */
		static FPlans& Get()
		{
			static FPlans* Plans = new FPlans();
			return *Plans;
		}
	};

	/** Lines of a text, without the line ends (files may have CRLF ends after a checkout). */
	TArray<FString> ToLines(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		return Lines;
	}
}
using namespace LBAGridPlanTests;

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FLBAGridPlanGoldenTest, "LBA2.Enclosure.Plans",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FLBAGridPlanGoldenTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(LBAGridPlanText::GetFolder() / TEXT("grid*.txt")), true, false);
	Files.Sort();
	for (const FString& File : Files)
	{
		// "Grid 0 L_Grid_000_TwinsensHouse, ...": the test's name is the level's.
		FString Text, Name = FPaths::GetBaseFilename(File);
		const TArray<FString> Lines = FFileHelper::LoadFileToString(Text, *(LBAGridPlanText::GetFolder() / File)) ? ToLines(Text) : TArray<FString>();
		if (Lines.Num() > 0)
		{
			TArray<FString> Words;
			Lines[0].ParseIntoArrayWS(Words);
			Name = Words.Num() > 2 ? Words[2].LeftChop(Words[2].EndsWith(TEXT(",")) ? 1 : 0) : Name;
		}
		OutBeautifiedNames.Add(Name);
		OutTestCommands.Add(FPaths::GetBaseFilename(File).RightChop(4));
	}
}

bool FLBAGridPlanGoldenTest::RunTest(const FString& Parameters)
{
	const int32 Grid = FCString::Atoi(*Parameters);
	FPlans& Plans = FPlans::Get();
	if (!Plans.bReady)
	{
		AddError(FString::Printf(TEXT("No game data: %s"), *Plans.Error));
		return false;
	}
	FString Golden;
	if (!FFileHelper::LoadFileToString(Golden, *LBAGridPlanText::GetPath(Grid)))
	{
		AddError(FString::Printf(TEXT("No golden plan %s"), *LBAGridPlanText::GetPath(Grid)));
		return false;
	}
	FLBAGridPlan Plan;
	if (!TestTrue(TEXT("plan made"), Plans.Importer.MakeGridPlan(Grid, Plan)))
	{
		return false;
	}
	const FString Actual = LBAGridPlanText::Write(Plan);
	const TArray<FString> Expected = ToLines(Golden), Got = ToLines(Actual);
	if (Expected == Got)
	{
		return true;
	}
	// The fresh plan beside the golden one, to diff; the first differences here.
	const FString ActualPath = FPaths::ProjectSavedDir() / FString::Printf(TEXT("LookDev/plans_actual/grid%03d.txt"), Grid);
	FFileHelper::SaveStringToFile(Actual, *ActualPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	int32 Reported = 0;
	for (int32 Line = 0; Line < FMath::Max(Expected.Num(), Got.Num()) && Reported < 8; ++Line)
	{
		const FString* Want = Expected.IsValidIndex(Line) ? &Expected[Line] : nullptr;
		const FString* Have = Got.IsValidIndex(Line) ? &Got[Line] : nullptr;
		if (!Want || !Have || *Want != *Have)
		{
			AddError(FString::Printf(TEXT("line %d: expected '%s', got '%s'"), Line + 1, Want ? **Want : TEXT("(end)"), Have ? **Have : TEXT("(end)")));
			++Reported;
		}
	}
	AddError(FString::Printf(TEXT("Grid %d's plan changed: the fresh plan is in %s (diff it with %s; if the change is meant, reimport the interiors and commit Tests/Plans)"),
		Grid, *FPaths::ConvertRelativePathToFull(ActualPath), *LBAGridPlanText::GetPath(Grid)));
	return false;
}

#endif
