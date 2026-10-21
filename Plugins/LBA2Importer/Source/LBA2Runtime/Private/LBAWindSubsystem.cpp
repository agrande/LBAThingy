#include "LBAWindSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "LBA2Runtime.h"
#include "LBAStorm.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace
{
	/** The level's rain system (LBAEnvironment) and its drops' velocity. */
	const TCHAR* RainSystemName = TEXT("NS_LBA_Rain");
	const FName RainVelocityName(TEXT("User.DropVelocity"));

	TAutoConsoleVariable<int32> CVarWindDebug(TEXT("lba.Wind.Debug"), 0,
		TEXT("1 draws the wind around the player and the storm's eye wall at its height, 2 also logs the wind at the player."));

	/** Debug colors by wind speed (cm/s). */
	FColor GetSpeedColor(double Speed)
	{
		struct FStop { double Speed; FLinearColor Color; };
		static const FStop Stops[] = {
			{ 0.0, FLinearColor::White }, { 400.0, FLinearColor::Yellow }, { 800.0, FLinearColor::Red }, { 1200.0, FLinearColor(0.6f, 0.f, 0.8f) } };
		for (int32 Index = 1; Index < UE_ARRAY_COUNT(Stops); ++Index)
		{
			if (Speed < Stops[Index].Speed)
			{
				const double Alpha = (Speed - Stops[Index - 1].Speed) / (Stops[Index].Speed - Stops[Index - 1].Speed);
				return FMath::Lerp(Stops[Index - 1].Color, Stops[Index].Color, float(Alpha)).ToFColor(true);
			}
		}
		return Stops[UE_ARRAY_COUNT(Stops) - 1].Color.ToFColor(true);
	}
}

FVector ULBAWindSubsystem::GetWindAt(const FVector& Location) const
{
	const double Time = GetWorld()->GetTimeSeconds();
	FVector Wind = FVector::ZeroVector;
	for (const TWeakObjectPtr<ALBAStorm>& Storm : Storms)
	{
		if (const ALBAStorm* Valid = Storm.Get())
		{
			Wind += Valid->GetWind().Evaluate(Location, Time);
		}
	}
	return Wind;
}

float ULBAWindSubsystem::GetEyeDistance(const FVector& Location) const
{
	float Nearest = UE_BIG_NUMBER;
	for (const TWeakObjectPtr<ALBAStorm>& Storm : Storms)
	{
		if (const ALBAStorm* Valid = Storm.Get())
		{
			Nearest = FMath::Min(Nearest, Valid->GetWind().GetEyeDistance(Location));
		}
	}
	return Nearest;
}

void ULBAWindSubsystem::AddStorm(ALBAStorm* Storm)
{
	Storms.AddUnique(Storm);
}

void ULBAWindSubsystem::RemoveStorm(ALBAStorm* Storm)
{
	Storms.Remove(Storm);
}

bool ULBAWindSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId ULBAWindSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(ULBAWindSubsystem, STATGROUP_Tickables);
}

void ULBAWindSubsystem::Tick(float DeltaTime)
{
	Storms.RemoveAll([](const TWeakObjectPtr<ALBAStorm>& Storm) { return !Storm.IsValid(); });
	UpdateShaderParameters();
	UpdateRain();
#if ENABLE_DRAW_DEBUG
	if (const int32 Debug = CVarWindDebug.GetValueOnGameThread())
	{
		DrawDebug(Debug);
	}
#endif
}

void ULBAWindSubsystem::UpdateShaderParameters() const
{
	// Shaders get one storm (the first with a parameter collection).
	for (const TWeakObjectPtr<ALBAStorm>& Storm : Storms)
	{
		const ALBAStorm* Valid = Storm.Get();
		if (!Valid || !Valid->MaterialParameters)
		{
			continue;
		}
		if (UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(Valid->MaterialParameters))
		{
			FLinearColor Parameters[FLBAStormWind::NumShaderParameters];
			Valid->GetWind().GetShaderParameters(float(GetWorld()->GetTimeSeconds()), Parameters);
			for (int32 Index = 0; Index < FLBAStormWind::NumShaderParameters; ++Index)
			{
				Instance->SetVectorParameterValue(FLBAStormWind::ShaderParameterNames[Index], Parameters[Index]);
			}
		}
		return;
	}
}

void ULBAWindSubsystem::UpdateRain()
{
	if (!bRainSearched)
	{
		bRainSearched = true;
		for (TActorIterator<ANiagaraActor> It(GetWorld()); It; ++It)
		{
			UNiagaraComponent* Component = It->GetNiagaraComponent();
			if (Component && Component->GetAsset() && Component->GetAsset()->GetName() == RainSystemName)
			{
				Rain = Component;
				const FNiagaraVariableBase Velocity(FNiagaraTypeDefinition::GetVec3Def(), RainVelocityName);
				RainFall = Component->GetOverrideParameters().GetParameterValueOrDefault<FVector3f>(Velocity, FVector3f::ZeroVector).Z;
				break;
			}
		}
	}
	UNiagaraComponent* RainComponent = Rain.Get();
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!RainComponent || !HasWind() || !Controller || !Controller->PlayerCameraManager)
	{
		return;
	}
	// The drops around the camera move with the wind there (their streaks follow their paths).
	const FVector Wind = GetWindAt(Controller->PlayerCameraManager->GetCameraLocation());
	RainComponent->SetVariableVec3(RainVelocityName, FVector(Wind.X, Wind.Y, RainFall));
}

void ULBAWindSubsystem::DrawDebug(int32 Level) const
{
#if ENABLE_DRAW_DEBUG
	const UWorld* World = GetWorld();
	const APlayerController* Controller = World->GetFirstPlayerController();
	const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	const FVector Around = Pawn->GetActorLocation();

	// A grid of arrows as long as the air moves in half a second.
	constexpr int32 Half = 5;
	constexpr double Spacing = 600.0;
	for (int32 Y = -Half; Y <= Half; ++Y)
	{
		for (int32 X = -Half; X <= Half; ++X)
		{
			const FVector At(FMath::GridSnap(Around.X, Spacing) + X * Spacing, FMath::GridSnap(Around.Y, Spacing) + Y * Spacing, Around.Z);
			const FVector Wind = GetWindAt(At);
			DrawDebugDirectionalArrow(World, At, At + Wind * 0.5, 60.f, GetSpeedColor(Wind.Size()), false, -1.f, 0, 4.f);
		}
	}

	for (const TWeakObjectPtr<ALBAStorm>& Storm : Storms)
	{
		if (const ALBAStorm* Valid = Storm.Get())
		{
			FLBAStormWind Wind = Valid->GetWind();
			Wind.Center.Z = Around.Z;
			constexpr int32 Segments = 96;
			for (int32 Index = 0; Index < Segments; ++Index)
			{
				DrawDebugLine(World, Wind.GetWallPoint(360.f * Index / Segments), Wind.GetWallPoint(360.f * (Index + 1) / Segments),
					FColor::Cyan, false, -1.f, 0, 8.f);
			}
		}
	}

	if (Level >= 2 && GFrameCounter % 15 == 0)
	{
		const FVector Wind = GetWindAt(Around);
		UE_LOG(LogLBA2, Display, TEXT("Wind at %s: %.1f m/s horizontal, %.1f m/s vertical, eye distance %.2f"), *Around.ToCompactString(),
			FVector2D(Wind).Size() / 100.0, Wind.Z / 100.0, GetEyeDistance(Around));
	}
#endif
}
