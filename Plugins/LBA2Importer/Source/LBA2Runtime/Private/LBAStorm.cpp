#include "LBAStorm.h"

#include "Components/BillboardComponent.h"
#include "Components/SceneComponent.h"
#include "LBAWindSubsystem.h"

ALBAStorm::ALBAStorm()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Eye"));
#if WITH_EDITORONLY_DATA
	if (UBillboardComponent* Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite")))
	{
		Sprite->SetupAttachment(RootComponent);
		Sprite->bIsScreenSizeScaled = true;
	}
#endif
}

FLBAStormWind ALBAStorm::GetWind() const
{
	FLBAStormWind Placed = Wind;
	Placed.Center = GetActorLocation();
	Placed.Yaw = GetActorRotation().Yaw;
	return Placed;
}

void ALBAStorm::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Wind.Center = Transform.GetLocation();
	Wind.Yaw = Transform.Rotator().Yaw;
}

void ALBAStorm::BeginPlay()
{
	Super::BeginPlay();
	if (ULBAWindSubsystem* Subsystem = GetWorld()->GetSubsystem<ULBAWindSubsystem>())
	{
		Subsystem->AddStorm(this);
	}
}

void ALBAStorm::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ULBAWindSubsystem* Subsystem = GetWorld()->GetSubsystem<ULBAWindSubsystem>())
	{
		Subsystem->RemoveStorm(this);
	}
	Super::EndPlay(EndPlayReason);
}
