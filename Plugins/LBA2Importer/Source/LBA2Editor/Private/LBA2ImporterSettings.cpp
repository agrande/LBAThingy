#include "LBA2ImporterSettings.h"

#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

FString ULBA2ImporterSettings::FindDataDirectoryIn(const FString& Directory)
{
	if (Directory.IsEmpty())
	{
		return FString();
	}
	static const TCHAR* Candidates[] = { TEXT(""), TEXT("Common"), TEXT("LBA2"), TEXT("Data") };
	for (const TCHAR* Candidate : Candidates)
	{
		const FString Folder = FPaths::Combine(Directory, Candidate);
		if (IFileManager::Get().FileExists(*FPaths::Combine(Folder, TEXT("LBA_BKG.HQR"))))
		{
			return FPaths::ConvertRelativePathToFull(Folder);
		}
	}
	return FString();
}

FString ULBA2ImporterSettings::ResolveDataDirectory() const
{
	return FindDataDirectoryIn(GameDirectory.Path);
}

void ULBA2ImporterSettings::AutoDetectGameDirectory()
{
	if (!ResolveDataDirectory().IsEmpty())
	{
		return;
	}
	static const TCHAR* KnownInstalls[] =
	{
		TEXT("C:/Program Files (x86)/Steam/steamapps/common/Little Big Adventure 2"),
		TEXT("C:/Program Files/Steam/steamapps/common/Little Big Adventure 2"),
		TEXT("D:/SteamLibrary/steamapps/common/Little Big Adventure 2"),
		TEXT("C:/GOG Games/Little Big Adventure 2"),
		TEXT("D:/GOG Games/Little Big Adventure 2"),
		TEXT("C:/Program Files (x86)/GOG Galaxy/Games/Little Big Adventure 2"),
	};
	for (const TCHAR* Install : KnownInstalls)
	{
		if (!FindDataDirectoryIn(Install).IsEmpty())
		{
			GameDirectory.Path = Install;
			return;
		}
	}
}

bool ULBA2ImporterSettings::IsRemakeCheckout(const FString& Directory)
{
	return !Directory.IsEmpty()
		&& IFileManager::Get().FileExists(*FPaths::Combine(Directory, TEXT("www/metadata/lba2/layouts.json")))
		&& IFileManager::Get().DirectoryExists(*FPaths::Combine(Directory, TEXT("www/models/lba2")));
}

void ULBA2ImporterSettings::AutoDetectRemakeDirectory()
{
	if (!RemakeDirectory.Path.IsEmpty())
	{
		return;
	}
	// Checkouts usually sit beside the project, either directly or in a shared refs folder.
	static const TCHAR* Candidates[] = { TEXT("../refs/lba2remake"), TEXT("../lba2remake"), TEXT("refs/lba2remake"), TEXT("lba2remake") };
	for (const TCHAR* Candidate : Candidates)
	{
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Candidate));
		FPaths::NormalizeDirectoryName(Directory);
		if (IsRemakeCheckout(Directory))
		{
			RemakeDirectory.Path = Directory;
			return;
		}
	}
}

ULBA2ImporterProjectSettings::ULBA2ImporterProjectSettings()
{
	// Seeded from a survey of every grid (Saved/LookDev/tools/survey_grids.py):
	//  - room height: the most common height, in cells above the floor, of the back walls drawn in full (>= 5 cells);
	//  - door height: the median clear height of the doorways that kept their lintel;
	//  - doorway layouts: layouts found next to the cube-change zones (doors) in at least two zones, with at least half
	//    of their instances next to one; jambs are at least 3 cells tall, thresholds 1 cell.
	auto Library = [this](int32 Index, const TCHAR* Theme, int32 RoomHeight, int32 DoorHeight, TArray<int32> Jambs,
		TArray<int32> Thresholds, bool bEnclose = true)
	{
		FLBALibraryEnclosure& Entry = LibraryEnclosures.AddDefaulted_GetRef();
		Entry.Library = Index;
		Entry.Theme = Theme;
		Entry.Enclosure.bEnclose = bEnclose;
		Entry.Enclosure.RoomHeight = RoomHeight;
		Entry.Enclosure.DoorHeight = DoorHeight;
		Entry.DoorwayJambs = MoveTemp(Jambs);
		Entry.DoorwayThresholds = MoveTemp(Thresholds);
	};
	Library(0, TEXT("Twinsen's house, Paul's house, museum, school"), 9, 8, { 33, 34, 35, 37, 41, 43, 126, 171 }, { 5, 45, 99 });
	Library(1, TEXT("Caves, sewers, School of Magic"), 6, 6, { 66, 67, 68, 113, 191 }, {}, false);
	Library(2, TEXT("Chez Luc, baggage claim, tickets office, pharmacy"), 9, 7, { 20 }, { 21, 23 });
	Library(3, TEXT("Temple of Bu, protection spell caves"), 6, 7, {}, { 135 });
	Library(4, TEXT("Airlocks, radar room, jail"), 9, 8, { 7, 72, 79, 80, 81 }, { 17, 36, 83 });
	Library(5, TEXT("Hacienda, Leone's and Baldino's houses, bar"), 6, 7, { 6, 7, 21, 31, 47 }, {});
	Library(6, TEXT("Spacecrafts"), 8, 7, { 4, 5 }, {});
	Library(7, TEXT("Fortress, bank, elevators"), 11, 7, { 104, 111, 112, 113, 114, 119, 144, 177, 197, 198, 199 }, {});
	Library(8, TEXT("Hotel, secret passages"), 11, 8, { 3, 15, 16, 54, 82 }, { 94 });
	Library(9, TEXT("Palace, Twinsun Cafe"), 13, 7, { 5, 70, 71, 72, 73, 74, 75 }, { 10, 13, 23, 24, 47, 64, 65 });
	Library(10, TEXT("Statue"), 7, 9, { 64 }, {});
	Library(11, TEXT("Docks, mine, caves, end sequences"), 8, 7, { 113, 115, 117, 118, 119, 184, 185, 186 }, { 116 }, false);
	Library(12, TEXT("Nest, tickets office, access to Island CX"), 8, 6, { 40, 42, 43, 44, 49, 50, 51, 52 }, { 41, 45 });
	Library(13, TEXT("Casino, bar, shop"), 6, 8, { 9, 14, 15, 22, 49, 54, 86, 91, 133, 159, 160, 196, 197 }, { 150, 198 });
	Library(14, TEXT("Refinery"), 6, 8, { 124, 167 }, {});
	Library(15, TEXT("Shops, bar, nursery, houses"), 8, 7, { 23, 24, 67 }, {});
	Library(16, TEXT("Control tower stairs"), 8, 6, { 177 }, {});
	Library(17, TEXT("Control tower"), 9, 8, { 5, 7, 61, 72, 79, 80, 118 }, { 9, 10, 17, 64 });

	auto Grid = [this](int32 Index, const TCHAR* Note, bool bEnclose, int32 RoomHeight = 0)
	{
		FLBAGridEnclosure& Entry = GridEnclosures.AddDefaulted_GetRef();
		Entry.Grid = Index;
		Entry.Note = Note;
		Entry.Overrides.bOverride_bEnclose = true;
		Entry.Overrides.bEnclose = bEnclose;
		Entry.Overrides.bOverride_RoomHeight = RoomHeight > 0;
		Entry.Overrides.RoomHeight = RoomHeight > 0 ? RoomHeight : Entry.Overrides.RoomHeight;
	};
	Grid(21, TEXT("Wizard's tent"), true, 8);
	Grid(26, TEXT("In the Dome"), true, 12);
	Grid(27, TEXT("School of Magic classroom"), true);
	Grid(28, TEXT("School of Magic"), true, 9);
	Grid(33, TEXT("School of Magic blowgun test"), true, 9);
	Grid(139, TEXT("In the Dome (demo)"), true, 12);
	Grid(52, TEXT("Outside the fortress"), false);
	Grid(55, TEXT("Outside the destroyed fortress"), false);
	Grid(57, TEXT("Shop"), true);
	Grid(69, TEXT("Church"), true);
}

void ULBA2ImporterProjectSettings::PostInitProperties()
{
	Super::PostInitProperties();

	// These options were per-user before: a value the project does not set yet comes from the user's old settings,
	// until the project settings are saved.
	if (HasAnyFlags(RF_ClassDefaultObject) && GConfig)
	{
		const FString Section = GetClass()->GetPathName();
		const FString OldSection = ULBA2ImporterSettings::StaticClass()->GetPathName();
		for (TFieldIterator<FProperty> It(GetClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FString Value;
			if (It->HasAnyPropertyFlags(CPF_Config)
				&& !GConfig->GetString(*Section, *It->GetName(), Value, GetClass()->GetConfigName())
				&& GConfig->GetString(*OldSection, *It->GetName(), Value, GEditorPerProjectIni))
			{
				It->ImportText_InContainer(*Value, this, this, PPF_None);
			}
		}
	}
}

FString ULBA2ImporterProjectSettings::GetContentRoot() const
{
	FString Root = ContentRoot.Path.IsEmpty() ? TEXT("/Game/LBA2") : ContentRoot.Path;
	if (!Root.StartsWith(TEXT("/")))
	{
		Root = TEXT("/Game/") + Root;
	}
	Root.RemoveFromEnd(TEXT("/"));
	return Root;
}

FLBAEnclosureSettings ULBA2ImporterProjectSettings::GetEnclosure(int32 Library, int32 Grid) const
{
	const FLBALibraryEnclosure* LibraryEntry = FindLibraryEnclosure(Library);
	FLBAEnclosureSettings Settings = LibraryEntry ? LibraryEntry->Enclosure : DefaultEnclosure;
	if (const FLBAGridEnclosure* GridEntry = GridEnclosures.FindByPredicate([Grid](const FLBAGridEnclosure& Entry) { return Entry.Grid == Grid; }))
	{
		GridEntry->Overrides.ApplyTo(Settings);
	}
	return Settings;
}

const FLBALibraryEnclosure* ULBA2ImporterProjectSettings::FindLibraryEnclosure(int32 Library) const
{
	return LibraryEnclosures.FindByPredicate([Library](const FLBALibraryEnclosure& Entry) { return Entry.Library == Library; });
}
