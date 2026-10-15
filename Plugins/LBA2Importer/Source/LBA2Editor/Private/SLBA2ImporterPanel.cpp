#include "SLBA2ImporterPanel.h"

#include "Framework/Notifications/NotificationManager.h"
#include "IDetailsView.h"
#include "LBA2ImportRunner.h"
#include "LBA2Settings.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SLBA2ImporterPanel"

void SLBA2ImporterPanel::Construct(const FArguments& InArgs)
{
	ULBA2ImporterSettings* Settings = GetMutableDefault<ULBA2ImporterSettings>();
	Settings->AutoDetectGameDirectory();

	// Layers that are implemented are selected by default.
	SelectedLayers = { ELBAImportLayer::Islands, ELBAImportLayer::Interiors, ELBAImportLayer::Characters };

	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs Args;
	Args.bAllowSearch = false;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsView = PropertyEditor.CreateDetailView(Args);
	const TArray<UObject*> EditedObjects = { Settings, GetMutableDefault<ULBA2Settings>() };
	DetailsView->SetObjects(EditedObjects);

	TSharedRef<SWrapBox> LayerBox = SNew(SWrapBox).UseAllottedSize(true);
	for (ELBAImportLayer Layer : FLBA2ImportRunner::AllLayers())
	{
		LayerBox->AddSlot()
		.Padding(4.f)
		[
			SNew(SCheckBox)
			.IsChecked(this, &SLBA2ImporterPanel::IsLayerChecked, Layer)
			.OnCheckStateChanged(this, &SLBA2ImporterPanel::OnLayerChanged, Layer)
			[
				SNew(STextBlock).Text(FText::FromString(FLBA2ImportRunner::LayerToString(Layer)))
			]
		];
	}

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(8.f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("Intro", "Imports Little Big Adventure 2 data from your installed copy of the game into Unreal assets. "
				"Nothing from the game is bundled with this plugin: point it at your own Steam or GOG install."))
		]
		+ SScrollBox::Slot()
		[
			DetailsView.ToSharedRef()
		]
		+ SScrollBox::Slot()
		.Padding(8.f, 8.f, 8.f, 0.f)
		[
			SNew(STextBlock).Text(LOCTEXT("Layers", "Layers to import"))
		]
		+ SScrollBox::Slot()
		.Padding(4.f)
		[
			LayerBox
		]
		+ SScrollBox::Slot()
		.Padding(8.f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.Text(LOCTEXT("Import", "Import"))
			.OnClicked(this, &SLBA2ImporterPanel::OnImport)
		]
		+ SScrollBox::Slot()
		.Padding(8.f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SLBA2ImporterPanel::GetStatusText)
		]
	];
}

ECheckBoxState SLBA2ImporterPanel::IsLayerChecked(ELBAImportLayer Layer) const
{
	return SelectedLayers.Contains(Layer) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SLBA2ImporterPanel::OnLayerChanged(ECheckBoxState State, ELBAImportLayer Layer)
{
	if (State == ECheckBoxState::Checked)
	{
		SelectedLayers.Add(Layer);
	}
	else
	{
		SelectedLayers.Remove(Layer);
	}
}

FText SLBA2ImporterPanel::GetStatusText() const
{
	const ULBA2ImporterSettings* Settings = GetDefault<ULBA2ImporterSettings>();
	const FString DataDirectory = Settings->ResolveDataDirectory();
	FText Status = DataDirectory.IsEmpty()
		? LOCTEXT("NoData", "Game data not found. Set Game Directory to your LBA2 install folder.")
		: FText::Format(LOCTEXT("Data", "Game data: {0}"), FText::FromString(DataDirectory));
	if (!LastSummary.IsEmpty())
	{
		Status = FText::Format(LOCTEXT("StatusWithSummary", "{0}\n\n{1}"), Status, LastSummary);
	}
	return Status;
}

FReply SLBA2ImporterPanel::OnImport()
{
	ULBA2ImporterSettings* Settings = GetMutableDefault<ULBA2ImporterSettings>();
	Settings->SaveConfig();
	GetMutableDefault<ULBA2Settings>()->TryUpdateDefaultConfigFile();

	TArray<ELBAImportLayer> Layers;
	for (ELBAImportLayer Layer : FLBA2ImportRunner::AllLayers())
	{
		if (SelectedLayers.Contains(Layer))
		{
			Layers.Add(Layer);
		}
	}

	FString Summary;
	const bool bOk = FLBA2ImportRunner::Run(*Settings, Layers, Summary);
	LastSummary = FText::FromString(Summary);

	FNotificationInfo Info(bOk ? LOCTEXT("ImportDone", "LBA2 import finished") : LOCTEXT("ImportFailed", "LBA2 import failed"));
	Info.SubText = LastSummary;
	Info.ExpireDuration = 8.f;
	FSlateNotificationManager::Get().AddNotification(Info);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
