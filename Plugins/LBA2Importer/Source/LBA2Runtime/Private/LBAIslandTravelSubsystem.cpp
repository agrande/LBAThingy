#include "LBAIslandTravelSubsystem.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Runtime.h"
#include "LBAPlayerController.h"
#include "LBA2Settings.h"
#include "Misc/CommandLine.h"

#define LOCTEXT_NAMESPACE "LBAIslandTravel"

namespace
{
	struct FIslandName
	{
		const TCHAR* Id;
		FText Name;
	};

	/** Names from lba2-classic-community/docs/GLOSSARY.md, in the order the game visits them. */
	const TArray<FIslandName>& KnownIslands()
	{
		static const TArray<FIslandName> Islands = {
			{ TEXT("CITABAU"), LOCTEXT("CITABAU", "Citadel Island") },
			{ TEXT("CITADEL"), LOCTEXT("CITADEL", "Citadel Island (storm)") },
			{ TEXT("DESERT"), LOCTEXT("DESERT", "Desert Island") },
			{ TEXT("CELEBRAT"), LOCTEXT("CELEBRAT", "Celebration Island") },
			{ TEXT("CELEBRA2"), LOCTEXT("CELEBRA2", "Celebration Island (statue)") },
			{ TEXT("SOUSCELB"), LOCTEXT("SOUSCELB", "Volcano Island") },
			{ TEXT("EMERAUDE"), LOCTEXT("EMERAUDE", "Emerald Moon") },
			{ TEXT("MOON"), LOCTEXT("MOON", "Emerald Moon (second visit)") },
			{ TEXT("OTRINGAL"), LOCTEXT("OTRINGAL", "Otringal") },
			{ TEXT("PLATFORM"), LOCTEXT("PLATFORM", "Island of the Wannies") },
			{ TEXT("MOSQUIBE"), LOCTEXT("MOSQUIBE", "Island of the Mosquibees") },
			{ TEXT("KNARTAS"), LOCTEXT("KNARTAS", "Francos Island") },
			{ TEXT("ILOTCX"), LOCTEXT("ILOTCX", "Island CX") },
			{ TEXT("ASCENCE"), LOCTEXT("ASCENCE", "Elevator to the Undergas") },
		};
		return Islands;
	}

	FAutoConsoleCommandWithWorldAndArgs TravelCommand(
		TEXT("lba.Travel"),
		TEXT("lba.Travel <ISLAND>: opens an island level (CITABAU, DESERT...). Without argument, lists the islands."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			ULBAIslandTravelSubsystem* Travel = World && World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<ULBAIslandTravelSubsystem>() : nullptr;
			if (!Travel)
			{
				return;
			}
			if (Args.IsEmpty())
			{
				for (const FLBAIslandInfo& Island : Travel->GetIslands())
				{
					UE_LOG(LogLBA2, Display, TEXT("%s: %s"), *Island.Id.ToString(), *Island.DisplayName.ToString());
				}
				return;
			}
			Travel->TravelToIsland(*Args[0].ToUpper());
		}));

	FAutoConsoleCommandWithWorld MenuCommand(
		TEXT("lba.IslandMenu"),
		TEXT("Shows or hides the island menu (M / gamepad Start)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (ALBAPlayerController* PC = World ? Cast<ALBAPlayerController>(World->GetFirstPlayerController()) : nullptr)
			{
				PC->ToggleIslandMenu();
			}
		}));
}

TArray<FLBAIslandInfo> ULBAIslandTravelSubsystem::FindIslands()
{
	TArray<FLBAIslandInfo> Islands;
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return Islands;
	}

	const FString& IslandsPath = GetDefault<ULBA2Settings>()->IslandsPath;
	if (!FPlatformProperties::RequiresCookedData())
	{
		// Uncooked standalone games start before the registry has gathered anything (cooked builds load it whole).
		Registry->ScanPathsSynchronous({ IslandsPath });
	}

	FARFilter Filter;
	Filter.PackagePaths.Add(*IslandsPath);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
	TArray<FAssetData> Assets;
	Registry->GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets)
	{
		FString Id = Asset.AssetName.ToString();
		if (!Id.RemoveFromStart(TEXT("L_")))
		{
			continue;
		}
		FLBAIslandInfo& Island = Islands.AddDefaulted_GetRef();
		Island.Id = *Id;
		Island.Level = TSoftObjectPtr<UWorld>(Asset.GetSoftObjectPath());
		const FIslandName* Known = KnownIslands().FindByPredicate([&Id](const FIslandName& Name) { return Id == Name.Id; });
		Island.DisplayName = Known ? Known->Name : FText::FromString(Id.Left(1) + Id.Mid(1).ToLower());
	}

	// Known islands in story order, then any others alphabetically.
	auto Order = [](FName Id)
	{
		const int32 Index = KnownIslands().IndexOfByPredicate([Id](const FIslandName& Name) { return Id == Name.Id; });
		return Index == INDEX_NONE ? KnownIslands().Num() : Index;
	};
	Islands.Sort([&Order](const FLBAIslandInfo& A, const FLBAIslandInfo& B)
	{
		const int32 OrderA = Order(A.Id), OrderB = Order(B.Id);
		return OrderA != OrderB ? OrderA < OrderB : A.Id.LexicalLess(B.Id);
	});
	return Islands;
}

FName ULBAIslandTravelSubsystem::GetCurrentIslandId() const
{
	const UWorld* World = GetGameInstance()->GetWorld();
	if (!World)
	{
		return NAME_None;
	}
	const FString PackageName = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	for (const FLBAIslandInfo& Island : GetIslands())
	{
		if (Island.Level.ToSoftObjectPath().GetLongPackageName() == PackageName)
		{
			return Island.Id;
		}
	}
	return NAME_None;
}

bool ULBAIslandTravelSubsystem::TravelToIsland(FName IslandId)
{
	for (const FLBAIslandInfo& Island : GetIslands())
	{
		if (Island.Id == IslandId)
		{
			UE_LOG(LogLBA2, Log, TEXT("Travelling to %s (%s)"), *IslandId.ToString(), *Island.Level.ToString());
			UGameplayStatics::OpenLevelBySoftObjectPtr(this, Island.Level);
			return true;
		}
	}
	UE_LOG(LogLBA2, Warning, TEXT("TravelToIsland: no island level named L_%s under %s"), *IslandId.ToString(), *GetDefault<ULBA2Settings>()->IslandsPath);
	return false;
}

bool ULBAIslandTravelSubsystem::ConsumeStartupMenu(const UWorld* World)
{
	if (bStartupMenuConsumed)
	{
		return false;
	}
	bStartupMenuConsumed = true;
	// Snapshot runs (-LBA2Shots=...) must not be covered by the menu.
	FString Shots;
	return GetDefault<ULBA2Settings>()->bIslandMenuAtStartup && World && !World->IsPlayInEditor()
		&& !FParse::Value(FCommandLine::Get(), TEXT("LBA2Shots="), Shots);
}

#undef LOCTEXT_NAMESPACE
