#include "LBAHitPointsComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/LineBatchComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"

namespace
{
	/**
	 * The original's hit star (RESS.HQR entry 46, POF 0): points in screen units (y down), drawn as lines in palette
	 * color 63 (white), facing the camera (POF.CPP PofDisplay3DExt; OBJECT.CPP InitHitExtraPof).
	 */
	const FVector2f StarPoints[] = {
		{ 174.f, -57.f }, { 120.f, 141.f }, { -2.f, -185.f }, { -114.f, 139.f }, { -167.f, -57.f },
		{ -69.f, 10.f }, { 3.f, 59.f }, { -46.f, -57.f }, { 45.f, -57.f }, { 71.f, 13.f },
	};
	const uint8 StarLines[][2] = {
		{ 7, 2 }, { 1, 6 }, { 5, 4 }, { 7, 4 }, { 0, 8 }, { 3, 5 }, { 3, 6 }, { 9, 0 }, { 2, 8 }, { 9, 1 },
	};
	constexpr float StarPointsRadius = 185.f;
	/** The last part of a star's life blinks before it disappears. */
	constexpr float BlinkTime = 0.4f;
}

ULBAHitPointsComponent::ULBAHitPointsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void ULBAHitPointsComponent::BeginPlay()
{
	Super::BeginPlay();
	Life = MaxLife;
	if (AActor* Owner = GetOwner())
	{
		Lines = NewObject<ULineBatchComponent>(Owner, TEXT("HitStars"), RF_Transient);
		Lines->bCalculateAccurateBounds = true;
		// Drawn every frame here: nothing to age.
		Lines->SetComponentTickEnabled(false);
		Lines->RegisterComponent();
	}
}

void ULBAHitPointsComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Lines)
	{
		Lines->DestroyComponent();
		Lines = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

bool ULBAHitPointsComponent::TakeHit(float Damage, FVector Location)
{
	if (Damage <= 0.f || IsDead())
	{
		return false;
	}
	Life = FMath::Max(0.f, Life - Damage);
	SpawnStars(Location, FMath::Clamp(FMath::CeilToInt(Damage / DamagePerStar), 1, MaxStars));
	OnHurt.Broadcast(Damage, Location);
	if (IsDead())
	{
		OnDeath.Broadcast();
	}
	return true;
}

void ULBAHitPointsComponent::Heal(float Amount)
{
	if (!IsDead())
	{
		Life = FMath::Min(MaxLife, Life + Amount);
	}
}

void ULBAHitPointsComponent::Revive()
{
	Life = MaxLife;
}

void ULBAHitPointsComponent::SpawnStars(FVector Location, int32 Count)
{
	const FVector Inherited = GetOwner() ? GetOwner()->GetVelocity() * StarInheritVelocity : FVector::ZeroVector;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		// Like the original's InitFly: thrown up and out in a random direction.
		const float Azimuth = FMath::FRandRange(0.f, 2.f * UE_PI);
		const float Elevation = FMath::DegreesToRadians(FMath::FRandRange(40.f, 80.f));
		const float Speed = StarSpeed * FMath::FRandRange(0.8f, 1.2f);
		FStar& Star = Stars.AddDefaulted_GetRef();
		Star.Location = Location;
		Star.Velocity = Inherited + Speed * FVector(FMath::Cos(Elevation) * FMath::Cos(Azimuth), FMath::Cos(Elevation) * FMath::Sin(Azimuth), FMath::Sin(Elevation));
		Star.Angle = FMath::FRandRange(0.f, 2.f * UE_PI);
		Star.Spin = FMath::DegreesToRadians(StarSpin) * (FMath::RandBool() ? 1.f : -1.f) * FMath::FRandRange(0.7f, 1.3f);
		Star.Age = 0.f;
	}
}

void ULBAHitPointsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	for (int32 Index = Stars.Num() - 1; Index >= 0; --Index)
	{
		FStar& Star = Stars[Index];
		Star.Age += DeltaTime;
		if (Star.Age >= StarLife)
		{
			Stars.RemoveAtSwap(Index);
			continue;
		}
		Star.Velocity.Z -= StarGravity * DeltaTime;
		Star.Location += Star.Velocity * DeltaTime;
		Star.Angle += Star.Spin * DeltaTime;
	}
	DrawStars();
}

void ULBAHitPointsComponent::DrawStars()
{
	if (!Lines)
	{
		return;
	}
	Lines->Flush();
	if (Stars.IsEmpty())
	{
		return;
	}
	// Facing the camera, like the original's screen-space drawing.
	FVector Right = FVector::RightVector, Up = FVector::UpVector;
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC && PC->PlayerCameraManager)
	{
		const FRotationMatrix View(PC->PlayerCameraManager->GetCameraRotation());
		Right = View.GetScaledAxis(EAxis::Y);
		Up = View.GetScaledAxis(EAxis::Z);
	}
	const float Scale = StarSize / StarPointsRadius;
	TArray<FBatchedLine> Batch;
	for (const FStar& Star : Stars)
	{
		const float Left = StarLife - Star.Age;
		if (Left < BlinkTime && FMath::FloorToInt(Left / 0.07f) % 2 == 1)
		{
			continue;
		}
		const float Cos = FMath::Cos(Star.Angle), Sin = FMath::Sin(Star.Angle);
		FVector Points[UE_ARRAY_COUNT(StarPoints)];
		for (int32 Point = 0; Point < UE_ARRAY_COUNT(StarPoints); ++Point)
		{
			const FVector2f& P = StarPoints[Point];
			const float X = (P.X * Cos - P.Y * Sin) * Scale;
			const float Y = (P.X * Sin + P.Y * Cos) * Scale;
			Points[Point] = Star.Location + Right * X - Up * Y;
		}
		for (const uint8* Line : StarLines)
		{
			Batch.Emplace(Points[Line[0]], Points[Line[1]], StarColor, 0.f, StarLineWidth, SDPG_World);
		}
	}
	Lines->DrawLines(Batch);
}
