#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class IStructureDetailsView;
class SLBAPlanCanvas;
class ULBAGridPlanAsset;

/**
 * The LBA2 Plan Browser: every grid's plan (DA_Grid_NNN_Plan) top-down, one level at a time. Left: the grids; middle:
 * the plan (rooms by color, their sides by what lies past them, connectors, walls by rule; hover a column for what it
 * is, click a room or connector to select it); right: the selection's data. Show in Level opens the grid's level with
 * its plan overlay on, the selection framed.
 */
class SLBAPlanBrowser : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLBAPlanBrowser) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	/** Shows a grid's plan (by grid index). */
	void SelectGrid(int32 Grid);

private:
	struct FEntry
	{
		TStrongObjectPtr<ULBAGridPlanAsset> Asset;
		FString Label;
	};
	using FEntryPtr = TSharedPtr<FEntry>;

	void Refresh();
	void ApplyFilter();
	void ShowEntry(FEntryPtr Entry);
	void OnSelectionChanged(int32 Room, int32 Connector);
	void ShowInLevel() const;
	const struct FLBAGridPlan* GetPlan() const;

	TArray<FEntryPtr> Entries;
	TArray<FEntryPtr> Filtered;
	FString Filter;
	FEntryPtr Current;
	TSharedPtr<SListView<FEntryPtr>> List;
	TSharedPtr<SLBAPlanCanvas> Canvas;
	TSharedPtr<IStructureDetailsView> Details;
	TArray<TSharedPtr<int32>> LevelOptions;
	int32 Level = INDEX_NONE;
	int32 SelectedRoom = INDEX_NONE;
	int32 SelectedConnector = INDEX_NONE;
};
