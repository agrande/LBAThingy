#pragma once

#include "CoreMinimal.h"
#include "LBA2ImporterSettings.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class IDetailsView;

/** Tools > LBA2 Importer: pick the game folder, choose layers, import. */
class SLBA2ImporterPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLBA2ImporterPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnImport();
	FText GetStatusText() const;
	ECheckBoxState IsLayerChecked(ELBAImportLayer Layer) const;
	void OnLayerChanged(ECheckBoxState State, ELBAImportLayer Layer);

	TSharedPtr<IDetailsView> DetailsView;
	TSet<ELBAImportLayer> SelectedLayers;
	FText LastSummary;
};
