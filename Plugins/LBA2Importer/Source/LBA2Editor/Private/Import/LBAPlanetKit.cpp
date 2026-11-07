#include "Import/LBAPlanetKit.h"

#include "Formats/LBA1.h"
#include "Formats/LBAIso.h"
#include "Interfaces/IPluginManager.h"
#include "LBA2Runtime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace LBAPlanetKitPrivate
{
	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			FJsonSerializer::Deserialize(Reader, Object);
		}
		return Object;
	}

	/** The remake's layout orientations (0..3) as turns, as its interiors are placed (FLBARemakeAssets::ProcessGrid). */
	constexpr double RemakeYaw[4] = { -90.0, -180.0, 90.0, 0.0 };
}

FString LBAPlanetKit::FTable::KeyOf(const FLBALayout& Layout)
{
	FString Key = FString::Printf(TEXT("%dx%dx%d:"), Layout.SizeX, Layout.SizeY, Layout.SizeZ);
	for (int32 Index = 0; Index < Layout.Blocks.Num(); ++Index)
	{
		if (Index)
		{
			Key += TEXT(",");
		}
		Key.AppendInt(Layout.Blocks[Index].Brick);
	}
	return Key;
}

int32 LBAPlanetKit::FTable::Find(const FLBALayout& Layout) const
{
	if (ByKey.IsEmpty())
	{
		return INDEX_NONE;
	}
	const int32* Found = ByKey.Find(KeyOf(Layout));
	return Found ? *Found : INDEX_NONE;
}

void LBAPlanetKit::FTable::Load(const LBA1::FData& Data, const FString& RemakeRoot)
{
	using namespace LBAPlanetKitPrivate;
	Models.Reset();
	ByKey.Reset();
	TMap<int32, FLBALibrary> Libraries;
	auto LayoutOf = [&](int32 Scene, int32 Layout) -> const FLBALayout*
	{
		FLBALibrary* Library = Libraries.Find(Scene);
		if (!Library)
		{
			Library = &Libraries.Add(Scene);
			if (!Data.LoadLibrary(Scene, *Library))
			{
				Library->Layouts.Reset();
			}
		}
		return Library->Layouts.IsValidIndex(Layout) ? &Library->Layouts[Layout] : nullptr;
	};
	auto Add = [&](const FLBALayout& Layout, FModel&& Model, bool bOverride)
	{
		const FString Key = KeyOf(Layout);
		if (ByKey.Contains(Key) && !bOverride)
		{
			return;
		}
		ByKey.Add(Key, Models.Add(MoveTemp(Model)));
	};

	// The remake's LBA1 replacements first: the table overrides them.
	const TSharedPtr<FJsonObject> Remake = RemakeRoot.IsEmpty() ? nullptr : LoadJson(FPaths::Combine(RemakeRoot, TEXT("www/metadata/lba1/layouts.json")));
	if (Remake.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Scene : Remake->Values)
		{
			const TSharedPtr<FJsonObject>* Layouts = nullptr;
			if (!Scene.Value->TryGetObject(Layouts))
			{
				continue;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Layouts)->Values)
			{
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				bool bReplace = false;
				FString File;
				double Orientation = 0.0;
				// "layout:size:blocks" variants aren't used by the remake's LBA1 scenes.
				if (Entry.Key.Contains(TEXT(":")) || !Entry.Value->TryGetObject(Fields) || !(*Fields)->TryGetBoolField(TEXT("replace"), bReplace) || !bReplace
					|| !(*Fields)->TryGetStringField(TEXT("file"), File))
				{
					continue;
				}
				(*Fields)->TryGetNumberField(TEXT("orientation"), Orientation);
				if (const FLBALayout* Layout = LayoutOf(FCString::Atoi(*Scene.Key), FCString::Atoi(*Entry.Key)))
				{
					FModel Model;
					Model.Name = FPaths::GetBaseFilename(File);
					Model.RemakeFile = TEXT("lba1/layouts") / File;
					Model.Yaw = RemakeYaw[int32(Orientation) & 3];
					Add(*Layout, MoveTemp(Model), false);
				}
			}
		}
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LBA2Importer"));
	const FString Path = Plugin ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Models/lba1_kit.json")) : FString();
	const TSharedPtr<FJsonObject> Table = LoadJson(Path);
	const TArray<TSharedPtr<FJsonValue>>* Pieces = nullptr;
	if (!Table.IsValid() || !Table->TryGetArrayField(TEXT("pieces"), Pieces))
	{
		UE_LOG(LogLBA2, Warning, TEXT("LBA1 kit: %s can't be read, LBA1's towns stay bricks"), *Path);
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Pieces)
	{
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		FString Example, Lba2, Fit, Turn;
		if (!Value->TryGetObject(Fields) || !(*Fields)->TryGetStringField(TEXT("example"), Example))
		{
			continue;
		}
		FModel Model;
		(*Fields)->TryGetStringField(TEXT("name"), Model.Name);
		FString Scene, Layout, Body;
		if ((*Fields)->TryGetStringField(TEXT("lba2"), Lba2) && Lba2.Split(TEXT(":"), &Model.Island, &Body))
		{
			Model.Body = FCString::Atoi(*Body);
		}
		(*Fields)->TryGetStringField(TEXT("remake"), Model.RemakeFile);
		(*Fields)->TryGetNumberField(TEXT("yaw"), Model.Yaw);
		(*Fields)->TryGetNumberField(TEXT("scale"), Model.Scale);
		(*Fields)->TryGetNumberField(TEXT("every"), Model.Every);
		Model.bFitHeight = (*Fields)->TryGetStringField(TEXT("fit"), Fit) && Fit == TEXT("height");
		Model.bRandomTurn = (*Fields)->TryGetStringField(TEXT("turn"), Turn) && Turn == TEXT("random");
		const FLBALayout* Found = Example.Split(TEXT(":"), &Scene, &Layout) ? LayoutOf(FCString::Atoi(*Scene), FCString::Atoi(*Layout)) : nullptr;
		if (!Found || (Model.Body == INDEX_NONE && Model.RemakeFile.IsEmpty()))
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA1 kit: %s (example %s) has no piece or no model"), *Model.Name, *Example);
			continue;
		}
		Add(*Found, MoveTemp(Model), true);
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA1 kit: %d pieces drawn as models"), ByKey.Num());
}
