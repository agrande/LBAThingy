#include "LBAIslandTravelSubsystem.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Runtime.h"
#include "LBACharacter.h"
#include "LBAPlayerController.h"
#include "LBA2Settings.h"
#include "LBAPointsOfInterest.h"
#include "LBASceneTravelSubsystem.h"
#include "LBATestStation.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"

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

FLBATestStationInfo FLBATestStationInfo::FromStation(const ALBATestStation& Station)
{
	FLBATestStationInfo Info;
	Info.Tag = Station.PlayerStartTag;
	Info.DisplayName = Station.DisplayName.IsEmpty() ? FText::FromName(Station.PlayerStartTag) : Station.DisplayName;
	Info.Description = Station.Description;
	Info.Order = Station.Order;
	Info.Transform = Station.GetActorTransform();
	return Info;
}

FString FLBATestStationInfo::ToTagValue(const TArray<FLBATestStationInfo>& Stations)
{
	auto Field = [](const FString& Text)
	{
		return Text.Replace(TEXT("\t"), TEXT(" ")).Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" "));
	};
	TArray<FString> Lines;
	for (const FLBATestStationInfo& Station : Stations)
	{
		Lines.Add(FString::Printf(TEXT("%s\t%d\t%s\t%s"), *Station.Tag.ToString(), Station.Order,
			*Field(Station.DisplayName.ToString()), *Field(Station.Description.ToString())));
	}
	return FString::Join(Lines, TEXT("\n"));
}

TArray<FLBATestStationInfo> FLBATestStationInfo::FromTagValue(const FString& Value)
{
	TArray<FLBATestStationInfo> Stations;
	TArray<FString> Lines;
	Value.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		TArray<FString> Fields;
		Line.ParseIntoArray(Fields, TEXT("\t"), false);
		if (Fields.Num() >= 4)
		{
			FLBATestStationInfo& Station = Stations.AddDefaulted_GetRef();
			Station.Tag = *Fields[0];
			Station.Order = FCString::Atoi(*Fields[1]);
			Station.DisplayName = FText::FromString(Fields[2]);
			Station.Description = FText::FromString(Fields[3]);
		}
	}
	return Stations;
}

FLBATravelDestination FLBATravelDestination::ToIsland(FName Island)
{
	FLBATravelDestination Destination;
	Destination.Kind = ELBATravelKind::Island;
	Destination.Island = Island;
	return Destination;
}

FLBATravelDestination FLBATravelDestination::ToScene(int32 Scene, FName Island)
{
	FLBATravelDestination Destination;
	Destination.Kind = ELBATravelKind::Scene;
	Destination.Scene = Scene;
	Destination.Island = Island;
	return Destination;
}

FLBATravelDestination FLBATravelDestination::ToTestLevel(const TSoftObjectPtr<UWorld>& Level, FName Station)
{
	FLBATravelDestination Destination;
	Destination.Kind = ELBATravelKind::TestLevel;
	Destination.Level = Level;
	Destination.Station = Station;
	return Destination;
}

FLBATravelDestination FLBATravelDestination::ToPointOfInterest(FName Id)
{
	FLBATravelDestination Destination;
	Destination.Kind = ELBATravelKind::PointOfInterest;
	Destination.PointOfInterest = Id;
	return Destination;
}

FText FLBATravelDestination::Describe(const UWorld* World) const
{
	switch (Kind)
	{
	case ELBATravelKind::PointOfInterest:
	{
		const FLBAPointOfInterest* Point = LBAPointsOfInterest::Find(PointOfInterest);
		return Point ? Point->Name : FText::FromName(PointOfInterest);
	}

	case ELBATravelKind::Island:
		for (const FLBAIslandInfo& Info : ULBAIslandTravelSubsystem::FindIslands())
		{
			if (Info.Id == Island)
			{
				return Info.DisplayName;
			}
		}
		return FText::FromName(Island);

	case ELBATravelKind::Scene:
		for (const FLBASceneMenuEntry& Entry : ULBASceneTravelSubsystem::FindMenuScenes())
		{
			if (Entry.SceneIndex == Scene)
			{
				return FText::FromString(Entry.Name);
			}
		}
		return FText::Format(LOCTEXT("SceneName", "Scene {0}"), FText::AsNumber(Scene));

	case ELBATravelKind::TestLevel:
	{
		const FString Package = Level.IsNull() ? (World ? UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()) : FString())
			: Level.ToSoftObjectPath().GetLongPackageName();
		for (const FLBATestLevelInfo& Info : ULBAIslandTravelSubsystem::FindTestLevels(World))
		{
			if (Info.Level.ToSoftObjectPath().GetLongPackageName() != Package)
			{
				continue;
			}
			const FLBATestStationInfo* Found = Info.Stations.FindByPredicate([this](const FLBATestStationInfo& Entry) { return Entry.Tag == Station; });
			return Found ? Found->DisplayName : Info.DisplayName;
		}
		return Station.IsNone() ? ULBAIslandTravelSubsystem::GetLevelDisplayName(Package) : FText::FromName(Station);
	}
	}
	return FText::GetEmpty();
}

FText ULBAIslandTravelSubsystem::GetLevelDisplayName(const FString& PackageName)
{
	FString Name = FPackageName::GetShortName(PackageName);
	if (!Name.RemoveFromStart(TEXT("L_")))
	{
		Name.RemoveFromStart(TEXT("Lvl_"));
	}
	// Words of a CamelCase name.
	FString Spaced;
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR Char = Name[Index];
		if (Char == TEXT('_'))
		{
			Spaced.AppendChar(TEXT(' '));
			continue;
		}
		if (Index > 0 && FChar::IsUpper(Char) && FChar::IsLower(Name[Index - 1]))
		{
			Spaced.AppendChar(TEXT(' '));
		}
		Spaced.AppendChar(Char);
	}
	return FText::FromString(Spaced);
}

TArray<FLBATestLevelInfo> ULBAIslandTravelSubsystem::FindTestLevels(const UWorld* LoadedWorld)
{
	TArray<FLBATestLevelInfo> Levels;
	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return Levels;
	}
	const ULBA2Settings* Settings = GetDefault<ULBA2Settings>();
	if (!FPlatformProperties::RequiresCookedData())
	{
		// Uncooked standalone games start before the registry has gathered anything (cooked builds load it whole).
		Registry->ScanPathsSynchronous({ Settings->TestLevelsPath });
	}

	// The feature map, the test levels folder, then the sandboxes; each once, and only those that exist.
	TArray<FAssetData> Assets;
	if (!Settings->FeatureLevel.IsNull())
	{
		Registry->GetAssetsByPackageName(Settings->FeatureLevel.ToSoftObjectPath().GetLongPackageFName(), Assets);
	}
	FARFilter Filter;
	Filter.PackagePaths.Add(*Settings->TestLevelsPath);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
	TArray<FAssetData> Folder;
	Registry->GetAssets(Filter, Folder);
	Folder.Sort([](const FAssetData& A, const FAssetData& B) { return A.AssetName.LexicalLess(B.AssetName); });
	Assets.Append(Folder);
	for (const TSoftObjectPtr<UWorld>& Sandbox : Settings->SandboxLevels)
	{
		if (!Sandbox.IsNull())
		{
			Registry->GetAssetsByPackageName(Sandbox.ToSoftObjectPath().GetLongPackageFName(), Assets);
		}
	}

	const FString LoadedPackage = LoadedWorld ? UWorld::RemovePIEPrefix(LoadedWorld->GetOutermost()->GetName()) : FString();
	TSet<FName> Seen;
	for (const FAssetData& Asset : Assets)
	{
		if (!Asset.IsInstanceOf(UWorld::StaticClass()) || Seen.Contains(Asset.PackageName))
		{
			continue;
		}
		Seen.Add(Asset.PackageName);
		FLBATestLevelInfo& Level = Levels.AddDefaulted_GetRef();
		Level.Level = TSoftObjectPtr<UWorld>(Asset.GetSoftObjectPath());
		Level.bFeatureLevel = Asset.PackageName == Settings->FeatureLevel.ToSoftObjectPath().GetLongPackageFName();
		Level.DisplayName = GetLevelDisplayName(Asset.PackageName.ToString());
		if (LoadedWorld && Asset.PackageName.ToString() == LoadedPackage)
		{
			for (TActorIterator<ALBATestStation> It(const_cast<UWorld*>(LoadedWorld)); It; ++It)
			{
				Level.Stations.Add(FLBATestStationInfo::FromStation(**It));
			}
		}
		else
		{
			FString Value;
			if (Asset.GetTagValue(ALBATestStation::StationsTag, Value))
			{
				Level.Stations = FLBATestStationInfo::FromTagValue(Value);
			}
		}
		Level.Stations.StableSort([](const FLBATestStationInfo& A, const FLBATestStationInfo& B) { return A.Order < B.Order; });
	}
	return Levels;
}

bool ULBAIslandTravelSubsystem::TravelToTestLevel(const TSoftObjectPtr<UWorld>& Level, FName Station, bool bReload)
{
	UWorld* World = GetGameInstance()->GetWorld();
	if (!World)
	{
		return false;
	}
	const FString Current = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	const FString Package = Level.IsNull() ? Current : Level.ToSoftObjectPath().GetLongPackageName();
	APlayerController* Player = GetGameInstance()->GetFirstLocalPlayerController(World);
	APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	if (Package == Current && Pawn && !bReload)
	{
		// Here already: teleport to the station, or to where the level starts.
		AActor* Target = nullptr;
		for (TActorIterator<APlayerStart> It(World); It && !Target; ++It)
		{
			const bool bStation = It->IsA<ALBATestStation>();
			if (Station.IsNone() ? !bStation : (bStation && It->PlayerStartTag == Station))
			{
				Target = *It;
			}
		}
		if (Target)
		{
			UE_LOG(LogLBA2, Log, TEXT("Teleporting to %s"), Station.IsNone() ? TEXT("the level start") : *Station.ToString());
			Pawn->TeleportTo(Target->GetActorLocation(), Target->GetActorRotation());
			Player->SetControlRotation(Target->GetActorRotation());
			return true;
		}
		if (!Station.IsNone())
		{
			UE_LOG(LogLBA2, Warning, TEXT("TravelToTestLevel: no station %s in %s"), *Station.ToString(), *Package);
			return false;
		}
	}
	if (!FPackageName::DoesPackageExist(Package))
	{
		UE_LOG(LogLBA2, Warning, TEXT("TravelToTestLevel: no level %s"), *Package);
		return false;
	}
	// The URL portal (Map#Tag) is the player start the game mode spawns at.
	const FString Url = Station.IsNone() ? Package : Package + TEXT("#") + Station.ToString();
	const FString Options = GetDefault<ULBA2Settings>()->bPlayTestLevelsAsTwinsen ? TEXT("game=") + ALBAGameMode::StaticClass()->GetPathName() : FString();
	UE_LOG(LogLBA2, Log, TEXT("Travelling to %s"), *Url);
	UGameplayStatics::OpenLevel(World, FName(*Url), true, Options);
	return true;
}

bool ULBAIslandTravelSubsystem::TravelTo(const FLBATravelDestination& Destination)
{
	switch (Destination.Kind)
	{
	case ELBATravelKind::Island:
		return TravelToIsland(Destination.Island);

	case ELBATravelKind::Scene:
	{
		ULBASceneTravelSubsystem* SceneTravel = GetGameInstance()->GetSubsystem<ULBASceneTravelSubsystem>();
		const FLBAIslandInfo* Island = nullptr;
		const TArray<FLBAIslandInfo> Islands = FindIslands();
		if (!Destination.Island.IsNone())
		{
			Island = Islands.FindByPredicate([&Destination](const FLBAIslandInfo& Info) { return Info.Id == Destination.Island; });
		}
		return Island ? SceneTravel->TravelToSceneOnIsland(Destination.Scene, Island->Level) : SceneTravel->TravelToScene(Destination.Scene);
	}

	case ELBATravelKind::TestLevel:
		return TravelToTestLevel(Destination.Level, Destination.Station);

	case ELBATravelKind::PointOfInterest:
	{
		const FLBAPointOfInterest* Point = LBAPointsOfInterest::Find(Destination.PointOfInterest);
		if (!Point)
		{
			UE_LOG(LogLBA2, Warning, TEXT("No point of interest %s (lba.POI lists them)."), *Destination.PointOfInterest.ToString());
			return false;
		}
		return GetGameInstance()->GetSubsystem<ULBASceneTravelSubsystem>()->TravelToPointOfInterest(*Point);
	}
	}
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
