#include "Framework/Docking/TabManager.h"
#include "GameDelegates.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAIslandMenu.h"
#include "LBAIslandTravelSubsystem.h"
#include "LBASceneAsset.h"
#include "LBASceneTravelSubsystem.h"
#include "Modules/ModuleManager.h"
#include "SLBA2ImporterPanel.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "LBA2Editor"

static const FName LBA2ImporterTabName(TEXT("LBA2Importer"));

class FLBA2EditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LBA2ImporterTabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SLBA2ImporterPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "LBA2 Importer"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Import Little Big Adventure 2 game data"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLBA2EditorModule::RegisterMenus));

		// The island menu and scene changes open levels nothing references: cook them all (and the scene assets doors
		// look up by number), or packaged builds only have the startup map.
		ModifyCookHandle = FGameDelegates::Get().GetModifyCookDelegate().AddLambda(
			[](TConstArrayView<const ITargetPlatform*> TargetPlatforms, TArray<FName>& PackagesToCook, TArray<FName>& PackagesToNeverCook)
			{
				for (const FLBAIslandInfo& Island : ULBAIslandTravelSubsystem::FindIslands())
				{
					PackagesToCook.AddUnique(Island.Level.ToSoftObjectPath().GetLongPackageFName());
					UE_LOG(LogLBA2, Display, TEXT("Cooking island level %s"), *Island.Level.ToSoftObjectPath().GetLongPackageName());
				}
				int32 NumScenes = 0;
				for (const FAssetData& Scene : ULBASceneTravelSubsystem::FindSceneAssets())
				{
					PackagesToCook.AddUnique(Scene.PackageName);
					if (const ULBASceneAsset* Asset = Cast<ULBASceneAsset>(Scene.GetAsset()))
					{
						for (const TSoftObjectPtr<UWorld>& Level : Asset->SceneryLevels)
						{
							PackagesToCook.AddUnique(Level.ToSoftObjectPath().GetLongPackageFName());
						}
						if (!Asset->Level.IsNull())
						{
							PackagesToCook.AddUnique(Asset->Level.ToSoftObjectPath().GetLongPackageFName());
						}
					}
					++NumScenes;
				}
				UE_LOG(LogLBA2, Display, TEXT("Cooking %d scenes and their levels"), NumScenes);
				// The travel menu's icons, loaded by path.
				for (const FSoftObjectPath& Icon : GetDefault<ULBAIslandMenu>()->GetIconPaths())
				{
					PackagesToCook.AddUnique(Icon.GetLongPackageFName());
				}
				// Effects spawned by code (the flyers' airflow trails).
				const FSoftObjectPath Airflow = GetDefault<ULBA2Settings>()->AirflowTrail.ToSoftObjectPath();
				if (Airflow.IsValid())
				{
					PackagesToCook.AddUnique(Airflow.GetLongPackageFName());
				}
			});
	}

	virtual void ShutdownModule() override
	{
		FGameDelegates::Get().GetModifyCookDelegate().Remove(ModifyCookHandle);
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LBA2ImporterTabName);
		}
	}

private:
	FDelegateHandle ModifyCookHandle;

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("LBA2", LOCTEXT("LBA2Section", "Little Big Adventure 2"));
		Section.AddMenuEntry(
			"OpenLBA2Importer",
			LOCTEXT("OpenImporter", "LBA2 Importer"),
			LOCTEXT("OpenImporterTooltip", "Import islands, interiors, characters and more from an installed copy of LBA2"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(LBA2ImporterTabName);
			})));
	}
};

IMPLEMENT_MODULE(FLBA2EditorModule, LBA2Editor)

#undef LOCTEXT_NAMESPACE
