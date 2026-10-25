#pragma once

#include "InputCoreTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "LBAControls.generated.h"

class IInputProcessor;

/** Which controls signs and hints show: the device in use, or both side by side (gamepad left, keyboard right). */
UENUM(BlueprintType)
enum class ELBAControlsLayout : uint8
{
	KeyboardMouse,
	Gamepad,
	Both,
};

/**
 * Follows the device the player last used, keyboard and mouse or gamepad, so the controls shown on signs and menus
 * match it (the glyphs then tell viewers of a recording which one is in use). A stick must leave its dead zone and the
 * mouse must really move to switch. Editor worlds show both. lba.Controls.Device forces a layout.
 */
UCLASS()
class LBA2RUNTIME_API ULBAControlsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The layout to show in this world. */
	static ELBAControlsLayout GetLayout(const UObject* WorldContext);

	/** Broadcast when the device in use or the forced layout changes (any world). */
	static FSimpleMulticastDelegate& OnLayoutChanged();

	bool IsUsingGamepad() const { return bUsingGamepad; }

private:
	friend class FLBAControlsInputProcessor;
	void SetUsingGamepad(bool bGamepad);

	TSharedPtr<IInputProcessor> InputProcessor;
	bool bUsingGamepad = false;
};

/**
 * The keys of the game's controls, the actions' mappings and the signs explaining them read the same lists.
 * Keyboard and mouse keys first, then the gamepad's; the first of each device is the main one.
 */
namespace LBAControls
{
	LBA2RUNTIME_API TArray<FKey> Move();
	LBA2RUNTIME_API TArray<FKey> Look();
	LBA2RUNTIME_API TArray<FKey> Jump();
	LBA2RUNTIME_API TArray<FKey> Use();
	LBA2RUNTIME_API TArray<FKey> Attack();
	LBA2RUNTIME_API TArray<FKey> ToggleControlMode();
	LBA2RUNTIME_API TArray<FKey> Dinofly();
	LBA2RUNTIME_API TArray<FKey> Menu();

	LBA2RUNTIME_API TArray<FKey> DinoflyClimb();
	LBA2RUNTIME_API TArray<FKey> DinoflyBrake();
	LBA2RUNTIME_API TArray<FKey> DinoflyDive();
	LBA2RUNTIME_API TArray<FKey> DinoflyMode();

	/** The keys of a list that a layout shows (both: all of them). */
	LBA2RUNTIME_API TArray<FKey> ForDevice(TConstArrayView<FKey> Keys, bool bGamepad);
}
