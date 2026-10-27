#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameDelegates.h"
#include "IAssetViewport.h"
#include "Interfaces/IPluginManager.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAIslandMenu.h"
#include "LBAIslandTravelSubsystem.h"
#include "LBAPlanet.h"
#include "LBASceneAsset.h"
#include "LBASceneTravelSubsystem.h"
#include "LevelEditor.h"
#include "LevelSelector/LBALevelSelector.h"
#include "Modules/ModuleManager.h"
#include "PlanView/LBAPlanViews.h"
#include "SLBA2ImporterPanel.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "ToolMenus.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "LBA2Editor"

static const FName LBA2ImporterTabName(TEXT("LBA2Importer"));
static const FName LevelSelectorTabName(TEXT("LBA2LevelSelector"));

class FLBA2EditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Style = MakeShared<FSlateStyleSet>(TEXT("LBA2EditorStyle"));
		Style->SetContentRoot(IPluginManager::Get().FindPlugin(TEXT("LBA2Importer"))->GetBaseDir() / TEXT("Resources"));
		Style->Set("LBA2.LevelSelector", new FSlateImageBrush(Style->RootToContentDir(TEXT("Icons/LevelSelector"), TEXT(".png")), FVector2D(20.f, 20.f)));
		Style->Set("LBA2.Globe", new FSlateImageBrush(Style->RootToContentDir(TEXT("Icons/Globe"), TEXT(".png")), FVector2D(20.f, 20.f)));
		FSlateStyleRegistry::RegisterSlateStyle(*Style);

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

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LevelSelectorTabName, FOnSpawnTab::CreateRaw(this, &FLBA2EditorModule::SpawnLevelSelector))
			.SetDisplayName(LOCTEXT("LevelSelectorTitle", "LBA2 Levels"))
			.SetTooltipText(LOCTEXT("LevelSelectorTooltip", "The game's travel menu: opens islands, scenes, test levels and their stations in the editor"))
			.SetIcon(FSlateIcon(Style->GetStyleSetName(), "LBA2.LevelSelector"))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		// The selector's widget lives in the editor world: rebuilt for each level opened.
		FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		MapChangedHandle = LevelEditor.OnMapChanged().AddRaw(this, &FLBA2EditorModule::OnMapChanged);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLBA2EditorModule::RegisterMenus));
		LBAPlanViews::Register();

		OpenSelectorCommand = MakeUnique<FAutoConsoleCommand>(TEXT("lba.LevelSelector"), TEXT("Opens the LBA2 level selector (the toolbar's Levels button)."),
			FConsoleCommandDelegate::CreateRaw(this, &FLBA2EditorModule::OpenLevelSelector));
		SelectorGoCommand = MakeUnique<FAutoConsoleCommand>(TEXT("lba.LevelSelector.Go"),
			TEXT("lba.LevelSelector.Go <scene number | ISLAND | station>: opens a place in the editor like the level selector (stations of the feature test map)."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				if (Args.IsEmpty())
				{
					return;
				}
				const FString& Arg = Args[0];
				const bool bIsland = ULBAIslandTravelSubsystem::FindIslands().ContainsByPredicate([&Arg](const FLBAIslandInfo& Island)
				{
					return Island.Id.ToString().Equals(Arg, ESearchCase::IgnoreCase);
				});
				ULBALevelSelector::OpenInEditor(Arg.IsNumeric() ? FLBATravelDestination::ToScene(FCString::Atoi(*Arg))
					: bIsland ? FLBATravelDestination::ToIsland(*Arg.ToUpper())
					: FLBATravelDestination::ToTestLevel(GetDefault<ULBA2Settings>()->FeatureLevel, *Arg));
			}));
		PlayGlobeCommand = MakeUnique<FAutoConsoleCommand>(TEXT("lba.Globe.Play"),
			TEXT("Plays the planet's level in the level editor's viewport (the toolbar's Globe button)."),
			FConsoleCommandDelegate::CreateRaw(this, &FLBA2EditorModule::PlayGlobe));

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
				for (const FLBATestLevelInfo& Level : ULBAIslandTravelSubsystem::FindTestLevels())
				{
					PackagesToCook.AddUnique(Level.Level.ToSoftObjectPath().GetLongPackageFName());
					UE_LOG(LogLBA2, Display, TEXT("Cooking test level %s"), *Level.Level.ToSoftObjectPath().GetLongPackageName());
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
				// The planets, found through the settings, and their levels.
				for (const TSoftObjectPtr<ULBAPlanetAsset>& Soft : GetDefault<ULBA2Settings>()->Planets)
				{
					const ULBAPlanetAsset* Planet = Soft.LoadSynchronous();
					if (!Planet)
					{
						continue;
					}
					PackagesToCook.AddUnique(Soft.ToSoftObjectPath().GetLongPackageFName());
					if (!Planet->Level.IsNull())
					{
						PackagesToCook.AddUnique(Planet->Level.ToSoftObjectPath().GetLongPackageFName());
						UE_LOG(LogLBA2, Display, TEXT("Cooking planet level %s"), *Planet->Level.ToSoftObjectPath().GetLongPackageName());
					}
				}
			});
	}

	virtual void ShutdownModule() override
	{
		LBAPlanViews::Unregister();
		FGameDelegates::Get().GetModifyCookDelegate().Remove(ModifyCookHandle);
		if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
		{
			LevelEditor->OnMapChanged().Remove(MapChangedHandle);
		}
		LevelSelector.Reset();
		OpenSelectorCommand.Reset();
		SelectorGoCommand.Reset();
		PlayGlobeCommand.Reset();
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LBA2ImporterTabName);
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LevelSelectorTabName);
		}
		if (Style)
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*Style);
			Style.Reset();
		}
	}

private:
	FDelegateHandle ModifyCookHandle;
	FDelegateHandle MapChangedHandle;
	TUniquePtr<FAutoConsoleCommand> OpenSelectorCommand;
	TUniquePtr<FAutoConsoleCommand> SelectorGoCommand;
	TUniquePtr<FAutoConsoleCommand> PlayGlobeCommand;
	TSharedPtr<FSlateStyleSet> Style;
	TWeakPtr<SDockTab> LevelSelectorTab;
	TStrongObjectPtr<ULBALevelSelector> LevelSelector;

	TSharedRef<SDockTab> SpawnLevelSelector(const FSpawnTabArgs&)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
		LevelSelectorTab = Tab;
		RefreshLevelSelector();
		return Tab;
	}

	/** A new selector for the editor world, showing where its level is and what exists now. */
	void RefreshLevelSelector()
	{
		const TSharedPtr<SDockTab> Tab = LevelSelectorTab.Pin();
		if (!Tab)
		{
			return;
		}
		LevelSelector.Reset();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		ULBALevelSelector* Widget = World ? CreateWidget<ULBALevelSelector>(World) : nullptr;
		if (!Widget)
		{
			Tab->SetContent(SNullWidget::NullWidget);
			return;
		}
		LevelSelector.Reset(Widget);
		Tab->SetContent(Widget->TakeWidget());
		Widget->ScrollToCurrent();
	}

	void OnMapChanged(UWorld* World, EMapChangeType ChangeType)
	{
		if (ChangeType == EMapChangeType::TearDownWorld)
		{
			// The widget goes with the world it was made in.
			LevelSelector.Reset();
			if (const TSharedPtr<SDockTab> Tab = LevelSelectorTab.Pin())
			{
				Tab->SetContent(SNullWidget::NullWidget);
			}
		}
		else if (ChangeType == EMapChangeType::LoadMap || ChangeType == EMapChangeType::NewMap)
		{
			RefreshLevelSelector();
		}
	}

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

		// One big button on the main toolbar, by Play: the travel menu, in the editor.
		const FUIAction OpenSelector(FExecuteAction::CreateRaw(this, &FLBA2EditorModule::OpenLevelSelector));
		Section.AddMenuEntry("OpenLBA2Levels", LOCTEXT("OpenLevels", "LBA2 Levels"), LOCTEXT("OpenLevelsTooltip", "Islands, scenes, test levels and their stations"),
			FSlateIcon(Style->GetStyleSetName(), "LBA2.LevelSelector"), OpenSelector);
		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		FToolMenuSection& ToolbarSection = Toolbar->FindOrAddSection("LBA2");
		FToolMenuEntry Button = FToolMenuEntry::InitToolBarButton("LBA2Levels", OpenSelector, LOCTEXT("LevelsButton", "Levels"),
			LOCTEXT("LevelsButtonTooltip", "LBA2 level selector: the game's travel menu, with the test levels. Picking a place opens its level here."),
			FSlateIcon(Style->GetStyleSetName(), "LBA2.LevelSelector"));
		// Icon and label, like the toolbar's Platforms button.
		Button.StyleNameOverride = "CalloutToolbar";
		ToolbarSection.AddEntry(Button);

		// Next to it: play the planet.
		const FUIAction PlayOnGlobe(FExecuteAction::CreateRaw(this, &FLBA2EditorModule::PlayGlobe),
			FCanExecuteAction::CreateLambda([]() { return GEditor && !GEditor->IsPlaySessionInProgress(); }));
		const FText GlobeTooltip = LOCTEXT("GlobeButtonTooltip",
			"Plays the planet's level in the viewport: Citadel (sunny) in globe mode, the islands of Twinsun laid out around it, "
			"the sea and far islands bending away to a horizon. Plays without opening the level here (console: lba.Globe.Play).");
		Section.AddMenuEntry("PlayLBA2Globe", LOCTEXT("PlayGlobe", "Play on the Globe"), GlobeTooltip, FSlateIcon(Style->GetStyleSetName(), "LBA2.Globe"), PlayOnGlobe);
		FToolMenuEntry GlobeButton = FToolMenuEntry::InitToolBarButton("LBA2Globe", PlayOnGlobe, LOCTEXT("GlobeButton", "Globe"), GlobeTooltip,
			FSlateIcon(Style->GetStyleSetName(), "LBA2.Globe"));
		GlobeButton.StyleNameOverride = "CalloutToolbar";
		ToolbarSection.AddEntry(GlobeButton);
	}

	/** The first planet's level (ULBAPlanetAsset::Level), built by the importer with its islands; null if none yet. */
	static FSoftObjectPath FindPlanetLevel()
	{
		for (const TSoftObjectPtr<ULBAPlanetAsset>& Soft : GetDefault<ULBA2Settings>()->Planets)
		{
			const ULBAPlanetAsset* Planet = Soft.LoadSynchronous();
			if (Planet && !Planet->Level.IsNull())
			{
				return Planet->Level.ToSoftObjectPath();
			}
		}
		return FSoftObjectPath();
	}

	static void Notify(const FText& Text)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 6.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	/**
	 * Plays the planet's level (globe mode by itself, ULBAPlanetSubsystem) in the level editor's viewport, like its Play
	 * button, whatever level is open here.
	 */
	void PlayGlobe()
	{
		if (!GEditor || GEditor->IsPlaySessionInProgress())
		{
			return;
		}
		const FSoftObjectPath Level = FindPlanetLevel();
		if (Level.IsNull())
		{
			UE_LOG(LogLBA2, Warning, TEXT("Globe: no planet level yet (LBA2 settings > Globe > Planets; import an island placed on it)"));
			Notify(LOCTEXT("NoPlanetLevel", "No planet level yet: import an island placed on a planet (Islands layer)."));
			return;
		}
		FRequestPlaySessionParams Params;
		Params.GlobalMapOverride = Level.ToString();
		FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		if (const TSharedPtr<IAssetViewport> Viewport = LevelEditor.GetFirstActiveViewport())
		{
			Params.DestinationSlateViewport = Viewport;
		}
		GEditor->RequestPlaySession(Params);
		UE_LOG(LogLBA2, Display, TEXT("Globe: playing %s"), *Level.ToString());
	}

	void OpenLevelSelector()
	{
		const bool bOpen = LevelSelectorTab.IsValid();
		FGlobalTabmanager::Get()->TryInvokeTab(LevelSelectorTabName);
		// Already open: refresh what it lists (levels imported since...).
		if (bOpen)
		{
			RefreshLevelSelector();
		}
	}
};

IMPLEMENT_MODULE(FLBA2EditorModule, LBA2Editor)

#undef LOCTEXT_NAMESPACE
