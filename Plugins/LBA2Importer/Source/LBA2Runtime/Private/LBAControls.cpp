#include "LBAControls.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"

namespace
{
	int32 GControlsDevice = 0;
	FAutoConsoleVariableRef CVarControlsDevice(
		TEXT("lba.Controls.Device"),
		GControlsDevice,
		TEXT("Controls shown on signs and menus: 0 = the device in use, 1 = keyboard and mouse, 2 = gamepad, 3 = both."),
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*) { ULBAControlsSubsystem::OnLayoutChanged().Broadcast(); }));

	/** A stick past this switches to the gamepad (drifting sticks rest below it). */
	constexpr float GamepadAnalogThreshold = 0.35f;
	/** Pixels the mouse must move in one event to switch back. */
	constexpr float MouseMoveThreshold = 2.f;
}

/** Watches all of Slate's input, before anything handles it, without consuming it. */
class FLBAControlsInputProcessor : public IInputProcessor
{
public:
	explicit FLBAControlsInputProcessor(ULBAControlsSubsystem* InOwner) : Owner(InOwner) {}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		Use(InKeyEvent.GetKey().IsGamepadKey());
		return false;
	}

	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& InAnalogInputEvent) override
	{
		if (InAnalogInputEvent.GetKey().IsGamepadKey() && FMath::Abs(InAnalogInputEvent.GetAnalogValue()) > GamepadAnalogThreshold)
		{
			Use(true);
		}
		return false;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetCursorDelta().SizeSquared() > FMath::Square(MouseMoveThreshold))
		{
			Use(false);
		}
		return false;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
	{
		Use(false);
		return false;
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override
	{
		Use(false);
		return false;
	}

	virtual const TCHAR* GetDebugName() const override { return TEXT("LBAControls"); }

private:
	void Use(bool bGamepad)
	{
		if (ULBAControlsSubsystem* Subsystem = Owner.Get())
		{
			Subsystem->SetUsingGamepad(bGamepad);
		}
	}

	TWeakObjectPtr<ULBAControlsSubsystem> Owner;
};

void ULBAControlsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FLBAControlsInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	}
}

void ULBAControlsSubsystem::Deinitialize()
{
	if (InputProcessor && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	Super::Deinitialize();
}

void ULBAControlsSubsystem::SetUsingGamepad(bool bGamepad)
{
	if (bUsingGamepad != bGamepad)
	{
		bUsingGamepad = bGamepad;
		OnLayoutChanged().Broadcast();
	}
}

ELBAControlsLayout ULBAControlsSubsystem::GetLayout(const UObject* WorldContext)
{
	switch (GControlsDevice)
	{
	case 1: return ELBAControlsLayout::KeyboardMouse;
	case 2: return ELBAControlsLayout::Gamepad;
	case 3: return ELBAControlsLayout::Both;
	default: break;
	}
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!World || !World->IsGameWorld())
	{
		return ELBAControlsLayout::Both;
	}
	const UGameInstance* GameInstance = World->GetGameInstance();
	const ULBAControlsSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<ULBAControlsSubsystem>() : nullptr;
	return Subsystem && Subsystem->IsUsingGamepad() ? ELBAControlsLayout::Gamepad : ELBAControlsLayout::KeyboardMouse;
}

FSimpleMulticastDelegate& ULBAControlsSubsystem::OnLayoutChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

namespace LBAControls
{
	// Shown on signs only: the move and look mappings add modifiers per key (arrows move too).
	TArray<FKey> Move() { return { EKeys::W, EKeys::A, EKeys::S, EKeys::D, EKeys::Gamepad_Left2D }; }
	TArray<FKey> Look() { return { EKeys::Mouse2D, EKeys::Gamepad_Right2D }; }

	TArray<FKey> Jump() { return { EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom }; }
	TArray<FKey> Use() { return { EKeys::E, EKeys::Gamepad_FaceButton_Left }; }
	TArray<FKey> Attack() { return { EKeys::LeftMouseButton, EKeys::Gamepad_FaceButton_Right }; }
	TArray<FKey> ToggleControlMode() { return { EKeys::Tab }; }
	TArray<FKey> Dinofly() { return { EKeys::F, EKeys::Gamepad_FaceButton_Top }; }
	TArray<FKey> Menu() { return { EKeys::M, EKeys::Gamepad_Special_Right }; }

	TArray<FKey> DinoflyClimb() { return { EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom, EKeys::Gamepad_RightTrigger }; }
	TArray<FKey> DinoflyBrake() { return { EKeys::LeftControl, EKeys::C, EKeys::Gamepad_FaceButton_Right, EKeys::Gamepad_LeftTrigger }; }
	TArray<FKey> DinoflyDive() { return { EKeys::LeftShift, EKeys::Gamepad_FaceButton_Left, EKeys::Gamepad_RightShoulder }; }
	TArray<FKey> DinoflyMode() { return { EKeys::Tab, EKeys::Gamepad_Special_Left }; }

	TArray<FKey> ForDevice(TConstArrayView<FKey> Keys, bool bGamepad)
	{
		TArray<FKey> Result;
		for (const FKey& Key : Keys)
		{
			if (Key.IsGamepadKey() == bGamepad)
			{
				Result.Add(Key);
			}
		}
		return Result;
	}
}
