#pragma once

#include "LBAIslandMenu.h"
#include "LBALevelSelector.generated.h"

/**
 * The game's travel menu (ULBAIslandMenu) in an editor tab, opened from the level editor toolbar's Levels button: the
 * same planets, islands, scenes and Tests tab, but picking a place opens its level in the editor (asking to save
 * changes first) and frames it in the viewports, selecting its start or station. Play here plays in the editor from
 * the place picked last.
 */
UCLASS()
class ULBALevelSelector : public ULBAIslandMenu
{
	GENERATED_BODY()

public:
	ULBALevelSelector(const FObjectInitializer& ObjectInitializer);

	/** Opens a destination's level in the editor and frames it. False if it does not exist. */
	static bool OpenInEditor(const FLBATravelDestination& Destination);

protected:
	virtual void GoTo(const FLBATravelDestination& Destination) override;
	virtual bool GetViewLocation(FVector& OutLocation) const override;
	virtual int32 GetCurrentScene() const override;
	virtual void Play() override;
};
