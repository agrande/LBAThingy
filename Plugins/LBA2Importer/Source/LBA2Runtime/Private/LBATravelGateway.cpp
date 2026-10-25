#include "LBATravelGateway.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "LBA2Runtime.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ALBATravelGateway::ALBATravelGateway()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// A flat disc on the floor, walked over (no collision).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Pad = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Pad"));
	Pad->SetupAttachment(RootComponent);
	Pad->SetStaticMesh(Cylinder.Object);
	Pad->SetRelativeLocation(FVector(0.0, 0.0, 2.0));
	Pad->SetRelativeScale3D(FVector(1.6, 1.6, 0.04));
	Pad->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Pad->SetCastShadow(false);

	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
	Trigger->SetupAttachment(RootComponent);
	Trigger->SetBoxExtent(FVector(60.0, 60.0, 100.0));
	Trigger->SetRelativeLocation(FVector(0.0, 0.0, 100.0));
	Trigger->SetCollisionProfileName(UCollisionProfile::DefaultProjectile_ProfileName);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Trigger->SetCollisionObjectType(ECC_WorldDynamic);
	Trigger->SetGenerateOverlapEvents(true);

	// Readable from +X, the side the gateway faces.
	Sign = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Sign"));
	Sign->SetupAttachment(RootComponent);
	Sign->SetRelativeLocation(FVector(0.0, 0.0, 240.0));
	Sign->SetHorizontalAlignment(EHTA_Center);
	Sign->SetVerticalAlignment(EVRTA_TextCenter);
	Sign->SetWorldSize(26.f);
	Sign->SetTextRenderColor(FColor(97, 206, 206));

	Glow = CreateDefaultSubobject<UPointLightComponent>(TEXT("Glow"));
	Glow->SetupAttachment(RootComponent);
	Glow->SetRelativeLocation(FVector(0.0, 0.0, 60.0));
	Glow->SetIntensity(800.f);
	Glow->SetAttenuationRadius(250.f);
	Glow->SetLightColor(FLinearColor(FColor(97, 206, 206)));
	Glow->SetCastShadows(false);
}

void ALBATravelGateway::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Sign->SetText(Label.IsEmpty() ? Destination.Describe(GetWorld()) : Label);
}

void ALBATravelGateway::BeginPlay()
{
	Super::BeginPlay();
	// Arm once nobody stands on it: not for a player who starts or arrives on the pad.
	FTimerHandle Timer;
	GetWorldTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		TArray<AActor*> Overlapping;
		GetOverlappingActors(Overlapping, APawn::StaticClass());
		bArmed = !Overlapping.ContainsByPredicate(&ALBATravelGateway::IsPlayerPawn);
	}), 0.5f, false);
}

bool ALBATravelGateway::IsPlayerPawn(const AActor* Actor)
{
	const APawn* Pawn = Cast<APawn>(Actor);
	return Pawn && Pawn->IsPlayerControlled();
}

void ALBATravelGateway::NotifyActorBeginOverlap(AActor* OtherActor)
{
	Super::NotifyActorBeginOverlap(OtherActor);
	if (bArmed && IsPlayerPawn(OtherActor))
	{
		bArmed = false;
		Travel();
	}
}

void ALBATravelGateway::NotifyActorEndOverlap(AActor* OtherActor)
{
	Super::NotifyActorEndOverlap(OtherActor);
	if (IsPlayerPawn(OtherActor))
	{
		bArmed = true;
	}
}

bool ALBATravelGateway::Travel()
{
	UGameInstance* GameInstance = GetGameInstance();
	ULBAIslandTravelSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<ULBAIslandTravelSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return false;
	}
	UE_LOG(LogLBA2, Log, TEXT("Gateway %s -> %s"), *GetName(), *Destination.Describe(GetWorld()).ToString());
	return Subsystem->TravelTo(Destination);
}
