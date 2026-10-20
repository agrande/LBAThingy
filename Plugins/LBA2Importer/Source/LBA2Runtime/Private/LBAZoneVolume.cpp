#include "LBAZoneVolume.h"

#include "Components/BrushComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "LBACharacter.h"
#include "LBASceneTravelSubsystem.h"

namespace
{
	const FName TriggerProfile(TEXT("Trigger"));

	void InitZoneBrush(AVolume* Volume, FColor Color)
	{
		Volume->GetBrushComponent()->SetCollisionProfileName(TriggerProfile);
		Volume->GetBrushComponent()->SetCanEverAffectNavigation(false);
		Volume->bColored = true;
		Volume->BrushColor = Color;
	}
}

ALBAZoneVolume::ALBAZoneVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InitZoneBrush(this, FColor(200, 200, 200));
}

const FLBASceneZoneData* ALBAZoneVolume::GetZoneData() const
{
	return Scene && Scene->Zones.IsValidIndex(ZoneIndex) ? &Scene->Zones[ZoneIndex] : nullptr;
}

// Cube change ------------------------------------------------------------------------------------------

ALBACubeChangeVolume::ALBACubeChangeVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	BrushColor = FColor(255, 160, 40);
}

void ALBACubeChangeVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	TargetScene = Zone.Num;
	Arrival = FIntVector(Zone.GetInfo(0), Zone.GetInfo(1), Zone.GetInfo(2));
	ArrivalQuarterTurns = Zone.GetInfo(3);
	bRequireWalkingIn = (Zone.GetInfo(5) & 1) != 0;
	bKeepArrivalHeight = (Zone.GetInfo(6) & 1) != 0;
	bEnabled = (Zone.GetInfo(7) & 1) != 0;
}

void ALBACubeChangeVolume::NotifyActorBeginOverlap(AActor* OtherActor)
{
	Super::NotifyActorBeginOverlap(OtherActor);
	APawn* Pawn = Cast<APawn>(OtherActor);
	if (!bEnabled || !Pawn || !Pawn->IsPlayerControlled())
	{
		return;
	}
	if (bRequireWalkingIn)
	{
		WaitingPawn = Pawn;
		SetActorTickEnabled(true);
	}
	else
	{
		Fire(Pawn);
	}
}

void ALBACubeChangeVolume::NotifyActorEndOverlap(AActor* OtherActor)
{
	Super::NotifyActorEndOverlap(OtherActor);
	if (OtherActor == WaitingPawn.Get())
	{
		WaitingPawn.Reset();
		SetActorTickEnabled(false);
	}
}

void ALBACubeChangeVolume::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APawn* Pawn = WaitingPawn.Get();
	if (!Pawn || !bEnabled)
	{
		WaitingPawn.Reset();
		SetActorTickEnabled(false);
		return;
	}
	if (IsWalkingIntoWall(Pawn))
	{
		Fire(Pawn);
	}
}

void ALBACubeChangeVolume::Fire(APawn* Pawn)
{
	if (ULBASceneTravelSubsystem* Travel = GetGameInstance() ? GetGameInstance()->GetSubsystem<ULBASceneTravelSubsystem>() : nullptr)
	{
		Travel->OnCubeChangeZone(this, Pawn);
	}
}

bool ALBACubeChangeVolume::IsWalkingIntoWall(const APawn* Pawn) const
{
	// Doors in walls: the original waits for the hero to bump into the door, not just to stand in front of it.
	FVector Direction = FVector::ZeroVector;
	if (const ALBACharacter* Hero = Cast<ALBACharacter>(Pawn))
	{
		Direction = Hero->GetMoveIntent();
	}
	else
	{
		Direction = Pawn->GetLastMovementInputVector();
	}
	Direction.Z = 0.0;
	const UCapsuleComponent* Capsule = Pawn->FindComponentByClass<UCapsuleComponent>();
	if (!Capsule || Direction.SizeSquared() < 0.04)
	{
		return false;
	}
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight() * 0.8f);
	const FCollisionQueryParams Params(SCENE_QUERY_STAT(LBADoor), false, Pawn);
	const FVector Start = Pawn->GetActorLocation() + FVector(0.0, 0.0, 6.0);
	return GetWorld()->SweepTestByChannel(Start, Start + Direction.GetSafeNormal() * 20.0, FQuat::Identity, ECC_Pawn, Shape, Params);
}

// Camera -----------------------------------------------------------------------------------------------

ALBACameraZoneVolume::ALBACameraZoneVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(80, 160, 255);
}

void ALBACameraZoneVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	LookAt = FIntVector(Zone.GetInfo(0), Zone.GetInfo(1), Zone.GetInfo(2));
	Angles = FIntVector(Zone.GetInfo(3), Zone.GetInfo(4), Zone.GetInfo(5));
	Distance = Zone.GetInfo(6);
	bEnabled = (Zone.GetInfo(7) & 1) != 0;
	bForced = (Zone.GetInfo(7) & 8) != 0;
}

// Scenario, grid, giver, message ----------------------------------------------------------------------

ALBAScenarioZoneVolume::ALBAScenarioZoneVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(200, 100, 255);
}

void ALBAScenarioZoneVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	ZoneNumber = Zone.Num;
}

ALBAGridZoneVolume::ALBAGridZoneVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(120, 120, 120);
}

void ALBAGridZoneVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	Overlay = Zone.Num;
}

ALBAGiverZoneVolume::ALBAGiverZoneVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(255, 230, 60);
}

void ALBAGiverZoneVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	BonusKinds = Zone.GetInfo(0);
	Amount = Zone.GetInfo(1);
}

ALBAMessageZoneVolume::ALBAMessageZoneVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(255, 255, 255);
}

void ALBAMessageZoneVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	Message = Zone.Num;
	FacingSide = Zone.GetInfo(2);
}

// Ladder, escalator, rail -------------------------------------------------------------------------------

ALBALadderVolume::ALBALadderVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(60, 255, 120);
}

void ALBALadderVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	// LoadScene copies Info0 into Info1, the "enabled" switch scripts toggle.
	bEnabled = Zone.GetInfo(0) != 0;
}

float ALBALadderVolume::GetTopHeight() const
{
	return GetComponentsBoundingBox(true).Max.Z;
}

ALBAEscalatorVolume::ALBAEscalatorVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(0, 220, 220);
}

void ALBAEscalatorVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	bEnabled = Zone.GetInfo(1) != 0;
	Direction = Zone.GetInfo(2);
}

ALBARailVolume::ALBARailVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	BrushColor = FColor(160, 110, 60);
}

void ALBARailVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	bEnabled = Zone.GetInfo(0) != 0;
}

// Hit ------------------------------------------------------------------------------------------------------

ALBAHitVolume::ALBAHitVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InitZoneBrush(this, FColor(255, 40, 40));
}

void ALBAHitVolume::InitFromZone(const FLBASceneZoneData& Zone)
{
	Damage = Zone.GetInfo(1);
	// Info2 is the delay between hits in fifths of a second.
	PainInterval = FMath::Max(Zone.GetInfo(2) * 0.2f, 0.2f);
	DamagePerSec = Damage / PainInterval;
	bPainCausing = Damage > 0;
}
