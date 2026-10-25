#include "PlanView/LBAPlanViews.h"

#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "LBAGridPlanActor.h"
#include "Misc/CoreDelegates.h"
#include "PlanView/LBAGridPlanVisualizer.h"
#include "PlanView/SLBAPlanBrowser.h"
#include "ToolMenus.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "LBAPlanViews"

namespace LBAPlanViewsPrivate
{
	const FName TabName(TEXT("LBA2PlanBrowser"));
	const FName MenuOwner(TEXT("LBAPlanViews"));
	TWeakPtr<SLBAPlanBrowser> Browser;
	TUniquePtr<FAutoConsoleCommand> OpenCommand;
	FDelegateHandle PostEngineInitHandle;

	void RegisterVisualizer()
	{
		if (GUnrealEd)
		{
			const TSharedPtr<FComponentVisualizer> Visualizer = MakeShared<FLBAGridPlanVisualizer>();
			GUnrealEd->RegisterComponentVisualizer(ULBAGridPlanComponent::StaticClass()->GetFName(), Visualizer);
			Visualizer->OnRegister();
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(MenuOwner);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("LBA2", LOCTEXT("LBA2Section", "Little Big Adventure 2"));
		Section.AddMenuEntry("OpenLBA2PlanBrowser", LOCTEXT("OpenPlanBrowser", "LBA2 Plan Browser"),
			LOCTEXT("OpenPlanBrowserTooltip", "What the importer understood of each interior grid: rooms, walls, doors, stairs, ceilings, top-down"),
			FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(TabName); })));
	}
}

void LBAPlanViews::Register()
{
	using namespace LBAPlanViewsPrivate;
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
	{
		TSharedRef<SLBAPlanBrowser> Widget = SNew(SLBAPlanBrowser);
		Browser = Widget;
		return SNew(SDockTab).TabRole(ETabRole::NomadTab)[Widget];
	}))
	.SetDisplayName(LOCTEXT("TabTitle", "LBA2 Plan Browser"))
	.SetTooltipText(LOCTEXT("TabTooltip", "The plans of the interior grids, top-down"))
	.SetMenuType(ETabSpawnerMenuType::Hidden);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenus));

	OpenCommand = MakeUnique<FAutoConsoleCommand>(TEXT("lba.PlanBrowser"), TEXT("lba.PlanBrowser [grid]: opens the LBA2 Plan Browser, at a grid's plan."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FGlobalTabmanager::Get()->TryInvokeTab(TabName);
			if (const TSharedPtr<SLBAPlanBrowser> Widget = Browser.Pin(); Widget && Args.Num() > 0)
			{
				Widget->SelectGrid(FCString::Atoi(*Args[0]));
			}
		}));

	if (GUnrealEd)
	{
		RegisterVisualizer();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&RegisterVisualizer);
	}
}

void LBAPlanViews::Unregister()
{
	using namespace LBAPlanViewsPrivate;
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
	if (GUnrealEd)
	{
		GUnrealEd->UnregisterComponentVisualizer(ULBAGridPlanComponent::StaticClass()->GetFName());
	}
	OpenCommand.Reset();
	if (UToolMenus::TryGet())
	{
		UToolMenus::UnregisterOwner(MenuOwner);
	}
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

#undef LOCTEXT_NAMESPACE
