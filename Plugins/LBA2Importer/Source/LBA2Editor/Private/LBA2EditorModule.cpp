#include "Framework/Docking/TabManager.h"
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
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LBA2ImporterTabName);
		}
	}

private:
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
