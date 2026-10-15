#include "LBA2ImporterSettings.h"

#include "HAL/FileManager.h"
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

FString ULBA2ImporterSettings::GetContentRoot() const
{
	FString Root = ContentRoot.Path.IsEmpty() ? TEXT("/Game/LBA2") : ContentRoot.Path;
	if (!Root.StartsWith(TEXT("/")))
	{
		Root = TEXT("/Game/") + Root;
	}
	Root.RemoveFromEnd(TEXT("/"));
	return Root;
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
