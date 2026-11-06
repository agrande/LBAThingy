#include "LBAAirflowComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAFlightMovementComponent.h"
#include "LBAWindSubsystem.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace
{
	TAutoConsoleVariable<bool> CVarAirflow(TEXT("lba.Airflow"), true,
		TEXT("Wind lines around flyers (the Dinofly)."));
	TAutoConsoleVariable<float> CVarAirflowOpacity(TEXT("lba.Airflow.Opacity"), 1.f,
		TEXT("Scales the opacity of the wind lines."));
	TAutoConsoleVariable<float> CVarAirflowDensity(TEXT("lba.Airflow.Density"), 1.f,
		TEXT("Scales how many wind lines appear."));
	TAutoConsoleVariable<int32> CVarAirflowDebug(TEXT("lba.Airflow.Debug"), 0,
		TEXT("1 logs the airflow (air speed, wind, line rate), 2 also draws the lines' paths."));

	/** Tries per line to place it where it shows; a line that finds no place starts on a later frame. */
	constexpr int32 PlacementTries = 8;
	/** How much tighter a curl gets by its end. */
	constexpr float HookTightening = 0.45f;

	namespace Param
	{
		const FName SpawnCount(TEXT("User.SpawnCount"));
		const FName Points(TEXT("User.Points"));
		const FName Base(TEXT("User.Base"));
		const FName HeadVelocity(TEXT("User.HeadVelocity"));
		const FName Bend(TEXT("User.Bend"));
		const FName Drift(TEXT("User.Drift"));
		const FName Across(TEXT("User.Across"));
		const FName Life(TEXT("User.Life"));
		const FName Lag(TEXT("User.Lag"));
		const FName HookRadius(TEXT("User.HookRadius"));
		const FName HookTime(TEXT("User.HookTime"));
		const FName HookTurn(TEXT("User.HookTurn"));
		const FName Visibility(TEXT("User.Visibility"));
		const FName Width(TEXT("User.Width"));
		const FName FadeIn(TEXT("User.FadeIn"));
		const FName Color(TEXT("User.Color"));
		const FName TailOpacity(TEXT("User.TailOpacity"));
		const FName MinPixels(TEXT("User.MinPixels"));
		const FName PixelAngle(TEXT("User.PixelAngle"));
		const FName MaxAngle(TEXT("User.MaxAngle"));
		const FName CameraPosition(TEXT("User.CameraPosition"));
		const FName NearFade(TEXT("User.NearFade"));
	}
}

ULBAAirflowComponent::ULBAAirflowComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After the owner moved this frame (see CreateStrokes), before the lines simulate.
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void ULBAAirflowComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (UNiagaraComponent* Stroke : Strokes)
	{
		if (Stroke)
		{
			Stroke->DestroyComponent();
		}
	}
	Strokes.Reset();
	LiveLines.Reset();
	ExtremityLastLines.Reset();
	PendingStrokes = 0.f;
	bCreated = false;
	Super::EndPlay(EndPlayReason);
}

void ULBAAirflowComponent::CreateStrokes()
{
	bCreated = true;
	UNiagaraSystem* Asset = System.IsNull() ? GetDefault<ULBA2Settings>()->AirflowTrail.LoadSynchronous() : System.LoadSynchronous();
	if (!Asset || !GetOwner()->GetRootComponent())
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: no wind lines (missing Niagara system)."), *GetPathName());
		return;
	}
	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (UPawnMovementComponent* Movement = Pawn->GetMovementComponent())
		{
			AddTickPrerequisiteComponent(Movement);
		}
	}
	// Read the extremities once the owner has animated them.
	const USkeletalMeshComponent* Mesh = GetOwner()->FindComponentByClass<USkeletalMeshComponent>();
	if (Mesh)
	{
		AddTickPrerequisiteComponent(const_cast<USkeletalMeshComponent*>(Mesh));
	}
	for (const FLBAAirflowExtremity& Extremity : Extremities)
	{
		if (!Mesh || !Mesh->DoesSocketExist(Extremity.Bone))
		{
			UE_LOG(LogLBA2, Warning, TEXT("%s: no bone or socket %s for wind lines."), *GetPathName(), *Extremity.Bone.ToString());
		}
	}
	ExtremityLastLines.Init(-100.0, Extremities.Num());
	// A slot per line that can be on screen; lines keep what they were given when they started, so a slot can start another
	// one while its last one finishes.
	for (int32 Slot = 0; Slot < FMath::Max(Look.MaxStrokes, 1); ++Slot)
	{
		UNiagaraComponent* Stroke = NewObject<UNiagaraComponent>(GetOwner());
		Stroke->SetAsset(Asset);
		Stroke->SetAutoDestroy(false);
		Stroke->SetupAttachment(GetOwner()->GetRootComponent());
		Stroke->bAutoActivate = false;
		Stroke->RegisterComponent();
		Stroke->AddTickPrerequisiteComponent(this);
		Stroke->Activate(true);
		Strokes.Add(Stroke);
	}
}

bool ULBAAirflowComponent::IsOnScreen(const FLine& Line, double Now) const
{
	const float Age = static_cast<float>(Now - Line.Birth);
	if (Age >= Line.Life)
	{
		return false;
	}
	// The head runs until the tail has caught up with it, the tail Lag behind; both drift with the air.
	const float HeadTime = FMath::Min(Age, Line.Life - Line.Lag);
	const float TailTime = FMath::Max(Age - Line.Lag, 0.f);
	const FVector Base = Line.Base + Line.Drift * Age;
	for (const float Time : { HeadTime, 0.5f * (HeadTime + TailTime), TailTime })
	{
		if (IsInView(Base + Line.HeadVelocity * Time))
		{
			return true;
		}
	}
	return false;
}

bool ULBAAirflowComponent::IsInView(const FVector& Location) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC)
	{
		return true;
	}
	int32 SizeX = 0;
	int32 SizeY = 0;
	PC->GetViewportSize(SizeX, SizeY);
	FVector2D Screen;
	return PC->ProjectWorldLocationToScreen(Location, Screen)
		&& Screen.X >= 0.0 && Screen.Y >= 0.0 && Screen.X <= SizeX && Screen.Y <= SizeY;
}

bool ULBAAirflowComponent::GetCamera(FVector& OutLocation, float& OutPixelAngle, float& OutViewportWidth) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return false;
	}
	OutLocation = PC->PlayerCameraManager->GetCameraLocation();
	FVector2D ViewportSize(1920.0, 1080.0);
	if (UGameViewportClient* Viewport = GetWorld()->GetGameViewport())
	{
		Viewport->GetViewportSize(ViewportSize);
	}
	OutViewportWidth = FMath::Max(ViewportSize.X, 1.0);
	const float HalfFov = FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle() * 0.5f);
	OutPixelAngle = 2.f * FMath::Tan(HalfFov) / OutViewportWidth;
	return true;
}

void ULBAAirflowComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const AActor* Owner = GetOwner();
	if (!bCreated)
	{
		CreateStrokes();
	}
	if (!Owner || Strokes.IsEmpty() || DeltaTime <= 0.f)
	{
		return;
	}
	// The air around the body moves with the level's wind there (gusts and air pockets included).
	const ULBAWindSubsystem* WindSubsystem = GetWorld()->GetSubsystem<ULBAWindSubsystem>();
	LevelWind = bUseLevelWind && WindSubsystem ? WindSubsystem->GetWindAt(Owner->GetActorLocation()) : FVector::ZeroVector;
	const FVector AirWind = Wind + LevelWind;

	// The body's motion without the jolts of its wing beats, which only move it vertically: the lines' direction and
	// bend should not change with each beat.
	FVector Velocity = Owner->GetVelocity();
	if (bHasLastVelocity)
	{
		Velocity.Z = FMath::Lerp(Velocity.Z, LastVelocity.Z, FMath::Exp(-DeltaTime / FMath::Max(Look.VerticalSmoothing, 0.001f)));
		SmoothedAcceleration = FMath::VInterpTo(SmoothedAcceleration, (Velocity - LastVelocity) / DeltaTime, DeltaTime, 6.f);
	}
	LastVelocity = Velocity;
	bHasLastVelocity = true;
	const FVector AirVelocity = Velocity - AirWind;
	const float AirSpeed = FMath::Max(AirVelocity.Size(), 1.0);
	const FVector Flow = AirVelocity.Size() > 1.0 ? AirVelocity.GetSafeNormal() : Owner->GetActorForwardVector();

	const bool bOn = bEmitting && CVarAirflow.GetValueOnGameThread();
	const float DensityScale = FMath::Max(CVarAirflowDensity.GetValueOnGameThread(), 0.f);
	const float OpacityScale = FMath::Max(CVarAirflowOpacity.GetValueOnGameThread(), 0.f);
	FVector CameraLocation = Owner->GetActorLocation();
	float PixelAngle = 0.001f;
	float ViewportWidth = 1920.f;
	GetCamera(CameraLocation, PixelAngle, ViewportWidth);
	const int32 Debug = CVarAirflowDebug.GetValueOnGameThread();
	const double Now = GetWorld()->GetTimeSeconds();

	// The body's motion through the air (the wind's included) sets how many lines there are and how bold. Standing
	// still, even in the wind, leaves none. The rate grows mostly as the square of the air speed (dynamic pressure,
	// energy), with a linear part so slow flight still shows its flow.
	const bool bMoving = Velocity.Size2D() > Look.StillSpeed || FMath::Abs(Velocity.Z) > Look.StillVerticalSpeed;
	const float SpeedRatio = AirSpeed / FMath::Max(Look.RefSpeed, 1.f);
	const float LineRate = Look.Density * (0.35f * SpeedRatio + 0.65f * FMath::Square(SpeedRatio)) * (bMoving ? DensityScale : 0.f);
	// Very faint when barely moving, clearer with speed.
	const float Boldness = 1.f - FMath::Pow(1.f - FMath::Clamp(AirSpeed / Look.FullSpeed, 0.f, 1.f), FMath::Max(Look.BoldnessBias, 0.1f));
	const FLBAAirflowStyle Style = {
		FMath::Lerp(Look.Slow.Width, Look.Fast.Width, Boldness),
		FMath::Lerp(Look.Slow.Opacity, Look.Fast.Opacity, Boldness),
		FMath::Lerp(Look.Slow.Length, Look.Fast.Length, Boldness),
		FMath::Lerp(Look.Slow.HookChance, Look.Fast.HookChance, Boldness),
		FMath::Lerp(Look.Slow.HookSize, Look.Fast.HookSize, Boldness) };

	// The frame around the body's way through the air, and the side of that way the camera is on.
	const FVector BodyCenter = Owner->GetActorLocation();
	FVector Perp1 = FVector::CrossProduct(Flow, FVector::UpVector).GetSafeNormal();
	if (Perp1.IsNearlyZero())
	{
		Perp1 = Owner->GetActorRightVector();
	}
	const FVector Perp2 = FVector::CrossProduct(Perp1, Flow).GetSafeNormal();
	const FVector ToCamera = CameraLocation - BodyCenter;
	const FVector CameraSide = (ToCamera - FVector::DotProduct(ToCamera, Flow) * Flow).GetSafeNormal();
	const double CosWedge = FMath::Cos(FMath::DegreesToRadians(Placement.WedgeAngle));
	const FVector ScreenSideways = FVector::CrossProduct(CameraSide, Flow).GetSafeNormal();

	// Where the extremities are, and how much the air hits them: most ahead of the body, half on its sides, none behind it.
	// None close to the plane through the camera and the body's way: their lines would run down the screen along the body.
	const USkeletalMeshComponent* Mesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	TArray<FVector, TInlineAllocator<16>> ExtremityLocations;
	TArray<float, TInlineAllocator<16>> ExtremityWeights;
	ExtremityLocations.SetNum(Extremities.Num());
	ExtremityWeights.SetNum(Extremities.Num());
	for (int32 Index = 0; Index < Extremities.Num(); ++Index)
	{
		const FLBAAirflowExtremity& Extremity = Extremities[Index];
		ExtremityLocations[Index] = Mesh ? Mesh->GetSocketTransform(Extremity.Bone).TransformPosition(Extremity.Offset) : BodyCenter;
		const FVector FromBody = ExtremityLocations[Index] - BodyCenter;
		const float Exposure = FMath::Clamp(0.5f + float(FVector::DotProduct(FromBody.GetSafeNormal(), Flow)), 0.f, 1.f);
		const bool bBeside = ScreenSideways.IsNearlyZero() || FMath::Abs(FVector::DotProduct(FromBody, ScreenSideways)) >= Placement.MinSideOffset;
		ExtremityWeights[Index] = Mesh && bBeside ? Extremity.Accent * Exposure : 0.f;
	}
	// An extremity for a new line (none just used it, it shows), by weight.
	auto PickExtremity = [&]()
	{
		float Total = 0.f;
		auto Chance = [&](int32 Index)
		{
			return ExtremityLastLines.IsValidIndex(Index) && Now - ExtremityLastLines[Index] >= Placement.ExtremityInterval && IsInView(ExtremityLocations[Index]) ? ExtremityWeights[Index] : 0.f;
		};
		for (int32 Index = 0; Index < Extremities.Num(); ++Index)
		{
			Total += Chance(Index);
		}
		float Left = FMath::FRand() * Total;
		for (int32 Index = 0; Index < Extremities.Num() && Total > 0.f; ++Index)
		{
			Left -= Chance(Index);
			if (Left <= 0.f && Chance(Index) > 0.f)
			{
				return Index;
			}
		}
		return int32(INDEX_NONE);
	};

	// A place for a new line: seen at its full length, from where the camera (following the body) will be by then, it
	// must show, look long enough and not be hidden by the ground or the sea. All lines lie along the air's motion past the
	// body and drift along it too, so that they all slide along themselves.
	auto PlaceLine = [&](FLine& Line)
	{
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (!PC)
		{
			return false;
		}
		const float Length = Style.Length * FMath::FRandRange(0.7f, 1.3f);
		const float Lag = FMath::Clamp(Length / (FMath::Max(Look.Stream, 0.05f) * AirSpeed), Look.MinGrowTime, FMath::Max(Look.MaxGrowTime, Look.MinGrowTime));
		const float HeadSpeed = Length / Lag;
		const FVector CameraTravel = Velocity * Lag;
		const FVector Way = -Flow;
		const FVector Drift = AirWind + AirVelocity * Placement.Carry;
		for (int32 Try = 0; Try < PlacementTries; ++Try)
		{
			FVector Base = FVector::ZeroVector;
			const int32 Picked = FMath::FRand() < Placement.ExtremityShare ? PickExtremity() : int32(INDEX_NONE);
			if (Picked != INDEX_NONE)
			{
				Base = ExtremityLocations[Picked];
			}
			else
			{
				const float Angle = FMath::FRandRange(0.f, UE_TWO_PI);
				const FVector Offset = FMath::Cos(Angle) * Perp1 + FMath::Sin(Angle) * Perp2;
				if (!CameraSide.IsNearlyZero() && FMath::Abs(FVector::DotProduct(Offset, CameraSide)) > CosWedge)
				{
					continue;
				}
				Base = BodyCenter + Offset * FMath::FRandRange(Placement.InnerRadius, FMath::Max(Placement.OuterRadius, Placement.InnerRadius)) + Flow * FMath::FRandRange(Placement.StartAheadMin, Placement.StartAheadMax);
			}
			const FVector HeadVelocity = Way * HeadSpeed;
			const FVector Tail = Base + Drift * Lag;
			const FVector Head = Tail + HeadVelocity * Lag;
			const FVector Middle = 0.5 * (Head + Tail);
			if (FVector::Dist(Middle, CameraLocation + CameraTravel) < Placement.CameraClearance)
			{
				continue;
			}
			FVector2D HeadScreen, TailScreen;
			if (!IsInView(Middle - CameraTravel)
				|| !PC->ProjectWorldLocationToScreen(Head - CameraTravel, HeadScreen)
				|| !PC->ProjectWorldLocationToScreen(Tail - CameraTravel, TailScreen)
				|| FVector2D::Distance(HeadScreen, TailScreen) < Placement.MinScreenLength * ViewportWidth)
			{
				continue;
			}
			float SeaZ = 0.f;
			if (ULBAFlightMovementComponent::FindWaterSurface(GetWorld(), Middle, SeaZ) && Middle.Z < SeaZ)
			{
				continue;
			}
			FCollisionQueryParams Query(SCENE_QUERY_STAT(LBAAirflowLine), false, Owner);
			if (GetWorld()->LineTraceTestByObjectType(CameraLocation + CameraTravel, Middle, FCollisionObjectQueryParams(ECC_WorldStatic), Query))
			{
				continue;
			}

			Line.Base = Base;
			Line.HeadVelocity = HeadVelocity;
			Line.Drift = Drift;
			Line.Birth = Now;
			if (Picked != INDEX_NONE)
			{
				ExtremityLastLines[Picked] = Now;
			}
			Line.Lag = Lag;
			Line.Life = (2.f + Look.Slide) * Lag;
			Line.Width = Style.Width * FMath::FRandRange(0.7f, 1.3f);
			// Across the line and the view, so that bow and curl show, towards the body's way: all the lines close in behind
			// the body, like its wake.
			const FVector Cross = FVector::CrossProduct(Way, (CameraLocation + CameraTravel - Middle).GetSafeNormal());
			Line.Across = Cross.IsNearlyZero() ? Perp1 : Cross.GetSafeNormal();
			if (FVector::DotProduct(Line.Across, Base - BodyCenter) > 0.0)
			{
				Line.Across = -Line.Across;
			}
			// The head's run bends like the body's path (the head covers that path's time HeadSpeed / AirSpeed times
			// as fast) and bows a little, at most MaxCurve over the run.
			const float RunTime = (1.f + Look.Slide) * Lag;
			const FVector PathAcceleration = SmoothedAcceleration - FVector::DotProduct(SmoothedAcceleration, Flow) * Flow;
			const FVector PathBend = Look.Curvature * PathAcceleration * FMath::Square(HeadSpeed / AirSpeed);
			const FVector Bow = Line.Across * FMath::DegreesToRadians(Look.BowAngle * FMath::FRandRange(0.5f, 1.5f)) * HeadSpeed / RunTime;
			Line.Bend = (PathBend + Bow).GetClampedToMaxSize(FMath::DegreesToRadians(Look.MaxCurve) * HeadSpeed / RunTime);
			// No curl seen edge on, where the line runs towards or away from the camera: it would look like a kink.
			if (Cross.Size() > 0.35 && FMath::FRand() < Style.HookChance)
			{
				Line.HookRadius = Style.HookSize * Length * FMath::FRandRange(0.7f, 1.3f);
				const float HookLength = FMath::DegreesToRadians(Look.HookTurn) * Line.HookRadius * (1.f - 0.5f * HookTightening);
				Line.HookTime = FMath::Min(HookLength / HeadSpeed, 0.6f * RunTime);
			}
			return true;
		}
		return false;
	};

	// The lines due, never more than MaxStrokes on screen at once (the ones the camera left behind no longer count) and
	// no backlog while at the cap. While the body moves at all, at least MinStrokes are on screen.
	LiveLines.RemoveAll([this, Now](const FLine& Line) { return !IsOnScreen(Line, Now); });
	TArray<int32, TInlineAllocator<16>> SpawnCounts;
	SpawnCounts.SetNumZeroed(Strokes.Num());
	const int32 Points = FMath::Clamp(Look.PointsPerStroke, 3, 64);
	auto StartLine = [&]()
	{
		FLine Line;
		const int32 Slot = NextSlot % Strokes.Num();
		if (SpawnCounts[Slot] > 0 || !PlaceLine(Line))
		{
			return false;
		}
		NextSlot = (Slot + 1) % Strokes.Num();
		SpawnCounts[Slot] = Points;
		UNiagaraComponent* Stroke = Strokes[Slot];
		Stroke->SetVariablePosition(Param::Base, Line.Base);
		Stroke->SetVariableVec3(Param::HeadVelocity, Line.HeadVelocity);
		Stroke->SetVariableVec3(Param::Bend, Line.Bend);
		Stroke->SetVariableVec3(Param::Drift, Line.Drift);
		Stroke->SetVariableVec3(Param::Across, Line.Across);
		Stroke->SetVariableFloat(Param::Life, Line.Life);
		Stroke->SetVariableFloat(Param::Lag, Line.Lag);
		Stroke->SetVariableFloat(Param::HookRadius, Line.HookRadius);
		Stroke->SetVariableFloat(Param::HookTime, Line.HookTime);
		Stroke->SetVariableFloat(Param::Width, Line.Width);
		Stroke->SetVariableFloat(Param::Visibility, Style.Opacity * OpacityScale);
		Stroke->SetVariableFloat(Param::FadeIn, Look.FadeIn);
#if ENABLE_DRAW_DEBUG
		if (Debug >= 2)
		{
			DrawDebugLine(GetWorld(), Line.Base, Line.Base + Line.HeadVelocity * Line.Lag, FColor::Cyan, false, Line.Life);
		}
#endif
		LiveLines.Add(Line);
		return true;
	};
	if (bOn && bMoving)
	{
		const int32 Fewest = FMath::Min(Look.MinStrokes, Look.MaxStrokes);
		while (LiveLines.Num() < Fewest && StartLine())
		{
		}
		PendingStrokes += LineRate * DeltaTime;
		while (PendingStrokes >= 1.f && LiveLines.Num() < Look.MaxStrokes && StartLine())
		{
			PendingStrokes -= 1.f;
		}
		PendingStrokes = FMath::Min(PendingStrokes, 1.f);
	}
	else
	{
		PendingStrokes = 0.f;
	}

	for (int32 Slot = 0; Slot < Strokes.Num(); ++Slot)
	{
		UNiagaraComponent* Stroke = Strokes[Slot];
		if (!Stroke)
		{
			continue;
		}
		Stroke->SetVariableInt(Param::SpawnCount, SpawnCounts[Slot]);
		Stroke->SetVariableInt(Param::Points, Points);
		Stroke->SetVariableFloat(Param::HookTurn, FMath::DegreesToRadians(Look.HookTurn));
		Stroke->SetVariableLinearColor(Param::Color, Look.Color);
		Stroke->SetVariableFloat(Param::TailOpacity, Look.TailOpacity);
		Stroke->SetVariableFloat(Param::MinPixels, Look.MinPixels);
		Stroke->SetVariableFloat(Param::PixelAngle, PixelAngle);
		Stroke->SetVariableFloat(Param::MaxAngle, Look.MaxScreenWidth * PixelAngle * ViewportWidth);
		Stroke->SetVariablePosition(Param::CameraPosition, CameraLocation);
		Stroke->SetVariableFloat(Param::NearFade, Look.NearFade);
	}

#if ENABLE_DRAW_DEBUG
	if (Debug >= 1 && GFrameCounter % 15 == 0)
	{
		UE_LOG(LogLBA2, Display, TEXT("Airflow %s: air speed %.1f m/s, wind %.1f m/s, %.1f lines/s, %d on screen, boldness %.2f"),
			*Owner->GetName(), AirSpeed / 100.f, AirWind.Size() / 100.0, LineRate, LiveLines.Num(), Boldness);
	}
#endif
}
