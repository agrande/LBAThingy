#include "LBAPlayerController.h"

#include "Blueprint/UserWidget.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "LBA2Runtime.h"
#include "LBAIslandMenu.h"
#include "LBAIslandTravelSubsystem.h"

void ALBAPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		// The input mode lives on the game viewport and survives travel: a menu open in the previous level left it UI only.
		SetInputMode(FInputModeGameOnly());

		ULBAIslandTravelSubsystem* Travel = GetGameInstance()->GetSubsystem<ULBAIslandTravelSubsystem>();
		if (Travel && Travel->ConsumeStartupMenu(GetWorld()))
		{
			// Pausing on the first frame would freeze the camera boom before it reaches the pawn.
			FTimerHandle Timer;
			GetWorldTimerManager().SetTimer(Timer, this, &ALBAPlayerController::OpenIslandMenu, 0.2f);
		}
	}
}

void ALBAPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	MenuContext = NewObject<UInputMappingContext>(this, TEXT("IMC_LBA_Menu"));
	ToggleMenuAction = NewObject<UInputAction>(this, TEXT("IA_LBA_IslandMenu"));
	ToggleMenuAction->ValueType = EInputActionValueType::Boolean;
	// The menu pauses the game; it must still be able to close itself.
	ToggleMenuAction->bTriggerWhenPaused = true;
	MenuContext->MapKey(ToggleMenuAction, EKeys::M);
	MenuContext->MapKey(ToggleMenuAction, EKeys::Gamepad_Special_Right);

	if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent))
	{
		Input->BindAction(ToggleMenuAction, ETriggerEvent::Started, this, &ALBAPlayerController::ToggleIslandMenu);
	}
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->AddMappingContext(MenuContext, 10);
	}
}

void ALBAPlayerController::OpenIslandMenu()
{
	if (IslandMenu)
	{
		return;
	}
	IslandMenu = CreateWidget<ULBAIslandMenu>(this, IslandMenuClass ? *IslandMenuClass : ULBAIslandMenu::StaticClass());
	if (!IslandMenu)
	{
		return;
	}
	IslandMenu->AddToViewport(100);

	// UI only: the menu closes itself on M / Start (see ULBAIslandMenu::NativeOnKeyDown).
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(IslandMenu->TakeWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	IslandMenu->FocusDefaultButton();
	SetShowMouseCursor(true);
	SetPause(true);
}

void ALBAPlayerController::CloseIslandMenu()
{
	if (!IslandMenu)
	{
		return;
	}
	IslandMenu->RemoveFromParent();
	IslandMenu = nullptr;

	SetInputMode(FInputModeGameOnly());
	SetShowMouseCursor(false);
	SetPause(false);
}

void ALBAPlayerController::ToggleIslandMenu()
{
	if (IslandMenu)
	{
		CloseIslandMenu();
	}
	else
	{
		OpenIslandMenu();
	}
}
