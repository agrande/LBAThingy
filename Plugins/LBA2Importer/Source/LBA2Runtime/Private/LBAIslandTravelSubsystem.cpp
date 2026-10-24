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
		ELBAPlanet Planet;
		/** IleLst index the scenes of this island carry (LBAScenes::GetIslandFiles), -1 for none. */
		int32 SceneIsland;
	};

	/**
	 * Names from lba2-classic-community/docs/GLOSSARY.md, grouped by planet and ordered like lba2remake's teleport
	 * menu (locator/locations), each variant after the island it changes.
	 */
	const TArray<FIslandName>& KnownIslands()
	{
		static const TArray<FIslandName> Islands = {
			{ TEXT("CITABAU"), LOCTEXT("CITABAU", "Citadel Island"), ELBAPlanet::Twinsun, 0 },
			{ TEXT("CITADEL"), LOCTEXT("CITADEL", "Citadel Island (storm)"), ELBAPlanet::Twinsun, 0 },
			{ TEXT("DESERT"), LOCTEXT("DESERT", "Desert Island"), ELBAPlanet::Twinsun, 2 },
			{ TEXT("EMERAUDE"), LOCTEXT("EMERAUDE", "Emerald Moon"), ELBAPlanet::EmeraldMoon, 3 },
			{ TEXT("MOON"), LOCTEXT("MOON", "Emerald Moon (second visit)"), ELBAPlanet::EmeraldMoon, INDEX_NONE },
			{ TEXT("OTRINGAL"), LOCTEXT("OTRINGAL", "Otringal"), ELBAPlanet::ZeelishSurface, 4 },
			{ TEXT("CELEBRAT"), LOCTEXT("CELEBRAT", "Celebration Island"), ELBAPlanet::ZeelishSurface, 5 },
			{ TEXT("CELEBRA2"), LOCTEXT("CELEBRA2", "Celebration Island (statue)"), ELBAPlanet::ZeelishSurface, 5 },
			{ TEXT("KNARTAS"), LOCTEXT("KNARTAS", "Francos Island"), ELBAPlanet::ZeelishSurface, 8 },
			{ TEXT("ASCENCE"), LOCTEXT("ASCENCE", "Elevator to the Undergas"), ELBAPlanet::ZeelishSurface, 10 },
			{ TEXT("ILOTCX"), LOCTEXT("ILOTCX", "Island CX"), ELBAPlanet::ZeelishSurface, 9 },
			{ TEXT("PLATFORM"), LOCTEXT("PLATFORM", "Island of the Wannies"), ELBAPlanet::ZeelishUndergas, 6 },
			{ TEXT("MOSQUIBE"), LOCTEXT("MOSQUIBE", "Island of the Mosquibees"), ELBAPlanet::ZeelishUndergas, 7 },
			{ TEXT("SOUSCELB"), LOCTEXT("SOUSCELB", "Volcano Island"), ELBAPlanet::ZeelishUndergas, 11 },
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
		Island.Planet = Known ? Known->Planet : ELBAPlanet::Other;
		Island.SceneIsland = Known ? Known->SceneIsland : INDEX_NONE;
	}

	// Known islands planet by planet, then any others alphabetically.
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

FText ULBAIslandTravelSubsystem::GetPlanetName(ELBAPlanet Planet)
{
	switch (Planet)
	{
	case ELBAPlanet::Twinsun: return LOCTEXT("Twinsun", "Twinsun");
	case ELBAPlanet::EmeraldMoon: return LOCTEXT("EmeraldMoon", "Emerald Moon");
	case ELBAPlanet::ZeelishSurface: return LOCTEXT("ZeelishSurface", "Zeelish Surface");
	case ELBAPlanet::ZeelishUndergas: return LOCTEXT("ZeelishUndergas", "Undergas");
	default: return LOCTEXT("OtherPlanet", "Elsewhere");
	}
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
