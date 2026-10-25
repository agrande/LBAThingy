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
	// Seeded from a survey of every grid (the plugin's Tools/survey_grids.py):
	//  - room height: the most common height, in cells above the floor, of the back walls drawn in full (>= 5 cells);
	//  - door height: the median clear height of the doorways that kept their lintel;
	//  - cut frames (the plugin's Tools/survey_frames.py): layouts at the top of walls in most of their instances,
	//    kept when they look like caps or frames (not lamps, candles, windows or railings).
	auto Library = [this](int32 Index, const TCHAR* Theme, int32 RoomHeight, int32 DoorHeight, TArray<int32> Frames, bool bEnclose = true)
	{
		FLBALibraryEnclosure& Entry = LibraryEnclosures.AddDefaulted_GetRef();
		Entry.Library = Index;
		Entry.Theme = Theme;
		Entry.Enclosure.bEnclose = bEnclose;
		Entry.Enclosure.RoomHeight = RoomHeight;
		Entry.Enclosure.DoorHeight = DoorHeight;
		Entry.CutFrames = MoveTemp(Frames);
	};
	Library(0, TEXT("Twinsen's house, Paul's house, museum, school"), 9, 8, { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20, 21, 24, 25, 26, 27, 36, 38, 39, 40, 158 });
	Library(1, TEXT("Caves, sewers, School of Magic"), 6, 6, {}, false);
	Library(2, TEXT("Chez Luc, baggage claim, tickets office, pharmacy"), 9, 7, { 3, 4, 6, 7, 8, 17, 18, 35, 39, 40, 41, 42 });
	// Caverns: platforms, bridges and rock (one roofed building, grid 32).
	Library(3, TEXT("Temple of Bu, protection spell caves"), 6, 7, {}, false);
	Library(4, TEXT("Airlocks, radar room, jail"), 9, 8, { 14, 68, 71 });
	Library(5, TEXT("Hacienda, Leone's and Baldino's houses, bar"), 6, 7, {});
	Library(6, TEXT("Spacecrafts"), 8, 7, { 33, 35, 36, 37, 39, 40, 41 });
	Library(7, TEXT("Fortress, bank, elevators"), 11, 7, { 25, 26, 27, 29, 108, 175 });
	Library(8, TEXT("Hotel, secret passages"), 11, 8, { 46, 47 });
	Library(9, TEXT("Palace, Twinsun Cafe"), 13, 7, { 36, 37, 38, 39, 41, 42, 43 });
	// The underworld: platforms on stilts over the dark, lava, caves.
	Library(10, TEXT("Statue"), 7, 9, {}, false);
	Library(11, TEXT("Docks, mine, caves, end sequences"), 8, 7, {}, false);
	// Wooden walkways over the void.
	Library(12, TEXT("Nest, tickets office, access to Island CX"), 8, 6, {}, false);
	Library(13, TEXT("Casino, bar, shop"), 6, 8, { 12, 13, 18, 46, 47, 50, 145, 146 });
	Library(14, TEXT("Refinery"), 6, 8, {});
	// The Francos' rock caves: flat walls and ceilings don't fit.
	Library(15, TEXT("Shops, bar, nursery, houses (rock caves)"), 8, 7, {}, false);
	Library(16, TEXT("Control tower stairs"), 8, 6, {});
	Library(17, TEXT("Control tower"), 9, 8, { 68, 85, 86, 88 });

	// Ceilings: the color of the top of each library's most used indoor wall style (DA_LibNN_WallStyles), 55% of the
	// way to a warm light gray, so they read lighter than the walls and in their hue.
	const TPair<int32, FColor> Ceilings[] = { {0, FColor(136, 138, 128)}, {1, FColor(158, 152, 138)}, {2, FColor(171, 164, 150)},
		{3, FColor(136, 134, 126)}, {4, FColor(125, 129, 117)}, {5, FColor(156, 146, 129)}, {6, FColor(148, 143, 128)},
		{7, FColor(173, 165, 151)}, {8, FColor(157, 142, 124)}, {9, FColor(178, 170, 136)}, {10, FColor(138, 133, 122)},
		{11, FColor(134, 126, 113)}, {12, FColor(134, 126, 113)}, {13, FColor(152, 146, 132)}, {14, FColor(146, 140, 126)},
		{15, FColor(145, 135, 120)}, {16, FColor(172, 168, 155)}, {17, FColor(154, 146, 138)} };
	for (const TPair<int32, FColor>& Ceiling : Ceilings)
	{
		if (FLBALibraryEnclosure* Entry = LibraryEnclosures.FindByPredicate([&Ceiling](const FLBALibraryEnclosure& E) { return E.Library == Ceiling.Key; }))
		{
			Entry->CeilingColor = Ceiling.Value;
		}
	}

	auto Grid = [this](int32 Index, const TCHAR* Note, bool bEnclose, int32 RoomHeight = 0, bool bCeiling = true)
	{
		FLBAGridEnclosure& Entry = GridEnclosures.AddDefaulted_GetRef();
		Entry.Grid = Index;
		Entry.Note = Note;
		Entry.Overrides.bOverride_bEnclose = true;
		Entry.Overrides.bEnclose = bEnclose;
		Entry.Overrides.bOverride_RoomHeight = RoomHeight > 0;
		Entry.Overrides.RoomHeight = RoomHeight > 0 ? RoomHeight : Entry.Overrides.RoomHeight;
		Entry.Overrides.bOverride_bCeiling = !bCeiling;
		Entry.Overrides.bCeiling = bCeiling;
	};
	Grid(21, TEXT("Wizard's tent"), true, 8);
	Grid(32, TEXT("A roofed building in the caves"), true);
	Grid(27, TEXT("School of Magic classroom"), true);
	Grid(28, TEXT("School of Magic"), true, 9);
	Grid(57, TEXT("Shop"), true);
	Grid(69, TEXT("Church"), true);
	// Open air or open space in the original (reviewed against captures of every grid): no enclosure.
	Grid(16, TEXT("Walkways on the roofs"), false);
	Grid(26, TEXT("In the Dome: a platform in a starfield, columns, no walls"), false);
	Grid(33, TEXT("School of Magic blowgun test: an open platform with columns"), false);
	Grid(51, TEXT("Top of a fortress tower, open air"), false);
	Grid(52, TEXT("Outside the fortress"), false);
	Grid(55, TEXT("Outside the destroyed fortress"), false);
	Grid(124, TEXT("Control tower grounds, open air"), false);
	Grid(125, TEXT("Control tower yard, open air"), false);
	Grid(139, TEXT("In the Dome (demo): a platform in a starfield"), false);
	// Open to the sky in the original (palms, a fountain): walls, no ceiling.
	Grid(29, TEXT("Hacienda central area (a courtyard)"), true, 0, false);

	// What holds the roof. Caves and the Temple of Bu are hollows in the rock: one rough vault over the floors, springing
	// RoomHeight cells over them and rising with the distance to the rock (libraries off: reviewed grid by grid below).
	for (const int32 Index : { 1, 3 })
	{
		if (FLBALibraryEnclosure* Entry = LibraryEnclosures.FindByPredicate([Index](const FLBALibraryEnclosure& E) { return E.Library == Index; }))
		{
			Entry->Enclosure.Structure = ELBAEnclosureStructure::Hollow;
			Entry->Enclosure.VaultRadius = 6.f;
			Entry->Enclosure.VaultRise = 8.f;
			Entry->Enclosure.VaultNoise = 1.5f;
			Entry->Enclosure.SpringReach = 2.f;
		}
	}
	auto Structure = [this, &Grid](int32 Index, const TCHAR* Note, ELBAEnclosureStructure Kind, float Radius, float Rise, float Noise, float Reach = 2.f)
	{
		FLBAGridEnclosure* Entry = GridEnclosures.FindByPredicate([Index](const FLBAGridEnclosure& E) { return E.Grid == Index; });
		if (!Entry)
		{
			Grid(Index, Note, true);
			Entry = &GridEnclosures.Last();
		}
		FLBAEnclosureOverrides& Overrides = Entry->Overrides;
		Overrides.bOverride_Structure = Overrides.bOverride_VaultRadius = Overrides.bOverride_VaultRise = Overrides.bOverride_VaultNoise = Overrides.bOverride_SpringReach = true;
		Overrides.Structure = Kind;
		Overrides.VaultRadius = Radius;
		Overrides.VaultRise = Rise;
		Overrides.VaultNoise = Noise;
		Overrides.SpringReach = Reach;
	};
	constexpr ELBAEnclosureStructure Hollow = ELBAEnclosureStructure::Hollow, Rooms = ELBAEnclosureStructure::Rooms;
	// Buildings of those libraries keep their rooms and flat ceilings.
	Structure(21, TEXT("Wizard's tent"), Rooms, 0.f, 0.f, 0.f);
	Structure(32, TEXT("A roofed building in the caves"), Rooms, 0.f, 0.f, 0.f);
	Structure(2, TEXT("Tralu's cave"), Hollow, 6.f, 8.f, 1.5f);
	Structure(19, TEXT("Tralu's cave, small Tralus"), Hollow, 6.f, 8.f, 1.5f);
	Structure(20, TEXT("Tralu's cave, Joe the elf"), Hollow, 6.f, 8.f, 1.5f);
	Structure(10, TEXT("Temple of Bu: a temple carved in the rock"), Hollow, 6.f, 8.f, 1.5f);
	Structure(11, TEXT("Temple of Bu, downstairs"), Hollow, 6.f, 8.f, 1.5f);
	Structure(12, TEXT("Temple of Bu, spacecraft area"), Hollow, 6.f, 8.f, 1.5f);
	Structure(75, TEXT("Temple of Bu, the well"), Hollow, 6.f, 8.f, 1.5f);
	// A sewer cistern: masonry all round, one level vault over its highest floor.
	Structure(17, TEXT("Sewers: a cistern"), Hollow, 8.f, 8.f, 0.f, 64.f);
	// Stone halls: built rooms under vaults (pointed where narrow, coved where wide).
	Structure(18, TEXT("Sewers, treasure room"), Rooms, 6.f, 6.f, 0.f);
	Structure(34, TEXT("Sewers, Sendell's ball room"), Rooms, 8.f, 8.f, 0.f);
	Structure(27, TEXT("School of Magic classroom: a church's nave"), Rooms, 8.f, 12.f, 0.f);
	Structure(28, TEXT("School of Magic"), Rooms, 8.f, 10.f, 0.f);
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
