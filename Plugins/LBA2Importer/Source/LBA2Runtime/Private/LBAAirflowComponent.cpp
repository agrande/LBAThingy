#include "LBAAirflowComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace
{
	TAutoConsoleVariable<bool> CVarAirflow(TEXT("lba.Airflow"), true,
		TEXT("Airflow strokes around flyers (the Dinofly)."));
	TAutoConsoleVariable<float> CVarAirflowOpacity(TEXT("lba.Airflow.Opacity"), 1.f,
		TEXT("Scales the opacity of the airflow strokes."));
	TAutoConsoleVariable<float> CVarAirflowDensity(TEXT("lba.Airflow.Density"), 1.f,
		TEXT("Scales how many airflow strokes appear."));
	TAutoConsoleVariable<int32> CVarAirflowDebug(TEXT("lba.Airflow.Debug"), 0,
		TEXT("1 logs the airflow (air speeds, downwash, stroke rates), 2 also draws the body parts and their air velocity."));

	constexpr double GravityAcceleration = 981.0;
	/** The level's rain system: its drops drift with the wind. */
	const TCHAR* RainSystemName = TEXT("NS_LBA_Rain");
	const TCHAR* RainVelocityName = TEXT("User.DropVelocity");

	namespace Param
	{
		const FName SpawnCount(TEXT("User.SpawnCount"));
		const FName Points(TEXT("User.Points"));
		const FName Extent(TEXT("User.Extent"));
		const FName Spread(TEXT("User.Spread"));
		const FName Perp1(TEXT("User.Perp1"));
		const FName Perp2(TEXT("User.Perp2"));
		const FName Rel(TEXT("User.Rel"));
		const FName Bend(TEXT("User.Bend"));
		const FName StrokeTime(TEXT("User.StrokeTime"));
		const FName Life(TEXT("User.Life"));
		const FName Drag(TEXT("User.Drag"));
		const FName Drift(TEXT("User.Drift"));
		const FName WakeTime(TEXT("User.WakeTime"));
		const FName Turbulence(TEXT("User.Turbulence"));
		const FName Visibility(TEXT("User.Visibility"));
		const FName Seed(TEXT("User.Seed"));
		const FName Color(TEXT("User.Color"));
		const FName Width(TEXT("User.Width"));
		const FName MinPixels(TEXT("User.MinPixels"));
		const FName PixelAngle(TEXT("User.PixelAngle"));
		const FName CameraPosition(TEXT("User.CameraPosition"));
		const FName NearFade(TEXT("User.NearFade"));
		const FName Stream(TEXT("User.Stream"));
		const FName LoopChance(TEXT("User.LoopChance"));
		const FName LoopSize(TEXT("User.LoopSize"));
		const FName FadeIn(TEXT("User.FadeIn"));
	}
}

ULBAAirflowComponent::ULBAAirflowComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After the owner moved and animated this frame (see CreateStrokes), before the strokes simulate.
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
	SourceStates.Reset();
	LiveStrokes.Reset();
	PendingStrokes = 0.f;
	bCreated = false;
	Super::EndPlay(EndPlayReason);
}

USceneComponent* ULBAAirflowComponent::GetSourceParent() const
{
	AActor* Owner = GetOwner();
	if (USkeletalMeshComponent* Mesh = Owner ? Owner->FindComponentByClass<USkeletalMeshComponent>() : nullptr)
	{
		return Mesh;
	}
	return Owner ? Owner->GetRootComponent() : nullptr;
}

void ULBAAirflowComponent::CreateStrokes()
{
	bCreated = true;
	UNiagaraSystem* Asset = System.IsNull() ? GetDefault<ULBA2Settings>()->AirflowTrail.LoadSynchronous() : System.LoadSynchronous();
	USceneComponent* Parent = GetSourceParent();
	if (!Asset || !Parent)
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: no airflow strokes (missing Niagara system)."), *GetPathName());
		return;
	}
	// Read the bones once the owner has animated them.
	AddTickPrerequisiteComponent(Parent);
	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (UPawnMovementComponent* Movement = Pawn->GetMovementComponent())
		{
			AddTickPrerequisiteComponent(Movement);
		}
	}
	for (const FLBAAirflowSource& Source : Sources)
	{
		for (const FName& Bone : { Source.Bone, Source.EndBone })
		{
			if (!Bone.IsNone() && !Parent->DoesSocketExist(Bone))
			{
				UE_LOG(LogLBA2, Warning, TEXT("%s: no bone or socket %s on %s for airflow strokes."), *GetPathName(), *Bone.ToString(), *Parent->GetName());
			}
		}
		// One system instance per body part, placed at its middle every frame.
		UNiagaraComponent* Stroke = NewObject<UNiagaraComponent>(GetOwner());
		Stroke->SetAsset(Asset);
		Stroke->SetAutoDestroy(false);
		Stroke->SetupAttachment(GetOwner()->GetRootComponent());
		Stroke->bAutoActivate = false;
		Stroke->RegisterComponent();
		Stroke->SetVariableFloat(Param::Seed, FMath::FRand() * 100.f);
		Stroke->AddTickPrerequisiteComponent(this);
		Stroke->Activate(true);
		Strokes.Add(Stroke);
	}
	SourceStates.SetNum(Strokes.Num());
	LevelWind = bUseLevelWind ? FindLevelWind() : FVector::ZeroVector;
}

FVector ULBAAirflowComponent::FindLevelWind() const
{
	for (TActorIterator<ANiagaraActor> It(GetWorld()); It; ++It)
	{
		const UNiagaraComponent* Rain = It->GetNiagaraComponent();
		if (Rain && Rain->GetAsset() && Rain->GetAsset()->GetName() == RainSystemName)
		{
			const FNiagaraVariableBase Velocity(FNiagaraTypeDefinition::GetVec3Def(), RainVelocityName);
			const FVector3f Drop = Rain->GetOverrideParameters().GetParameterValueOrDefault<FVector3f>(Velocity, FVector3f::ZeroVector);
			return FVector(Drop.X, Drop.Y, 0.0);
		}
	}
	return FVector::ZeroVector;
}

bool ULBAAirflowComponent::IsOnScreen(const FLiveStroke& Live, double Now) const
{
	const float Age = static_cast<float>(Now - Live.Birth);
	if (Age >= Live.Life)
	{
		return false;
	}
	// The head runs until the tail has caught up with it, the tail Lag behind; both drift with the air and are carried
	// by the wake at first.
	const float HeadTime = FMath::Min(Age, Live.Life - Live.Lag);
	const float TailTime = FMath::Max(Age - Live.Lag, 0.f);
	const float Wake = FMath::Max(WakeTime, 0.01f);
	const FVector Base = Live.Origin + Live.Drift * Age + Live.Carry * Wake * (1.f - FMath::Exp(-Age / Wake));
	for (const float Time : { HeadTime, 0.5f * (HeadTime + TailTime), TailTime })
	{
		if (IsInView(Base + Live.HeadVelocity * Time))
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

bool ULBAAirflowComponent::GetCamera(FVector& OutLocation, float& OutPixelAngle) const
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
	const float HalfFov = FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle() * 0.5f);
	OutPixelAngle = 2.f * FMath::Tan(HalfFov) / FMath::Max(ViewportSize.X, 1.0);
	return true;
}

void ULBAAirflowComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const AActor* Owner = GetOwner();
	if (!bCreated)
	{
		// Not at BeginPlay: owners often set their skeletal mesh (hence the bones) in their own BeginPlay, after ours.
		CreateStrokes();
	}
	if (!Owner || Strokes.IsEmpty() || DeltaTime <= 0.f)
	{
		return;
	}
	const FVector AirWind = Wind + LevelWind;

	// The whole body's lift (m (a - g) across the path; all of it when hovering) gives the wake's downwash, which
	// sinks the marked air: circulation = lift / (rho U b'), the pair sinks at circulation / (2 pi b').
	const FVector Velocity = Owner->GetVelocity();
	if (bHasLastVelocity)
	{
		SmoothedAcceleration = FMath::VInterpTo(SmoothedAcceleration, (Velocity - LastVelocity) / DeltaTime, DeltaTime, 6.f);
	}
	LastVelocity = Velocity;
	bHasLastVelocity = true;
	const FVector AirVelocity = Velocity - AirWind;
	const FVector Force = Mass * (SmoothedAcceleration + FVector(0.0, 0.0, GravityAcceleration)) / 100.0;
	const double SpanM = Span / 100.0;
	const double VortexSpacing = UE_DOUBLE_PI / 4.0 * SpanM;
	const double DiskArea = UE_DOUBLE_PI * SpanM * SpanM / 4.0;
	const double SpeedM = AirVelocity.Size() / 100.0;
	const FVector PathDirection = AirVelocity.GetSafeNormal();
	const double AlongPath = FVector::DotProduct(Force, PathDirection);
	const double AcrossPath = (Force - AlongPath * PathDirection).Size();
	const double HoverSpeed = FMath::Sqrt(Force.Size() / (2.0 * AirDensity * DiskArea));
	const double HoverWeight = FMath::Square(HoverSpeed) / (FMath::Square(HoverSpeed) + FMath::Square(SpeedM));
	const double LiftN = FMath::Sqrt(FMath::Square(AcrossPath) + HoverWeight * FMath::Square(AlongPath));
	const double Induced = FMath::Sqrt(LiftN / (2.0 * AirDensity * DiskArea));
	const double Circulation = LiftN / (AirDensity * FMath::Sqrt(SpeedM * SpeedM + Induced * Induced) * VortexSpacing);
	const double Downwash = Circulation / (2.0 * UE_DOUBLE_PI * VortexSpacing) * 100.0 * DownwashScale;
	const FVector Drift = AirWind - FVector(0.0, 0.0, Downwash);

	const bool bOn = bEmitting && CVarAirflow.GetValueOnGameThread();
	const float DensityScale = FMath::Max(CVarAirflowDensity.GetValueOnGameThread(), 0.f);
	const float OpacityScale = FMath::Max(CVarAirflowOpacity.GetValueOnGameThread(), 0.f);
	const int32 Points = FMath::Clamp(PointsPerStroke, 3, 32);
	const float StreamRate = FMath::Max(Stream, 0.05f);
	FVector CameraLocation = Owner->GetActorLocation();
	float PixelAngle = 0.001f;
	GetCamera(CameraLocation, PixelAngle);
	const USceneComponent* Parent = GetSourceParent();
	const int32 Debug = CVarAirflowDebug.GetValueOnGameThread();
	const double Now = GetWorld()->GetTimeSeconds();

	// The whole body's motion through the air (the wind's, not its own downwash) sets how many strokes there are, how
	// bold, and which parts shed them. Standing still, even in the wind, leaves none. The rate grows mostly as the
	// square of the air speed (dynamic pressure, energy), with a linear part so slow flight still shows its flow.
	const float BodyAirSpeed = AirVelocity.Size();
	const bool bMoving = Velocity.Size() > StillSpeed;
	const float SpeedRatio = BodyAirSpeed / FMath::Max(RefSpeed, 1.f);
	const float SpeedFactor = (0.35f * SpeedRatio + 0.65f * FMath::Square(SpeedRatio)) * (bMoving ? DensityScale : 0.f);
	// Small, faint and loopy only when barely moving, quickly bold.
	const float Boldness = 1.f - FMath::Pow(1.f - FMath::Clamp(BodyAirSpeed / FullSpeed, 0.f, 1.f), FMath::Max(BoldnessBias, 0.1f));
	const FLBAAirflowStyle Style = {
		FMath::Lerp(Slow.Width, Fast.Width, Boldness),
		FMath::Lerp(Slow.Opacity, Fast.Opacity, Boldness),
		FMath::Lerp(Slow.Length, Fast.Length, Boldness),
		FMath::Lerp(Slow.Life, Fast.Life, Boldness),
		FMath::Lerp(Slow.LoopChance, Fast.LoopChance, Boldness),
		FMath::Lerp(Slow.LoopSize, Fast.LoopSize, Boldness) };
	const float Visibility = Style.Opacity * OpacityScale;
	const float Life = StrokeLife * Style.Life;
	// Seconds of path a stroke spans (its length over its air speed).
	auto PathTimeOf = [this, &Style](const FVector& Rel)
	{
		return Style.Length * FMath::Clamp(MinStrokeLength / FMath::Max(Rel.Size(), 1.0), StrokeTime, 0.3f);
	};
	const FVector BodyCenter = Owner->GetActorLocation();
	// The air around the wings moves down through them (the induced flow that holds the flyer up): when hovering, air
	// rushes down past the body; in flight the flow tilts down a little.
	const FVector LocalAir = AirWind - FVector(0.0, 0.0, Downwash);

	struct FPart
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		FVector Center = FVector::ZeroVector;
		FVector Rel = FVector::ZeroVector;
		FVector PartAcceleration = FVector::ZeroVector;
		/** Velocity the wake gives its strokes at first. */
		FVector Carry = FVector::ZeroVector;
		float Trailing = 0.f;
		/** How much it sheds, whatever the speed. */
		float Weight = 0.f;
		float Rate = 0.f;
		int32 NumNew = 0;
		/** Life of the strokes it starts this frame. */
		float Life = 0.f;
	};
	TArray<FPart, TInlineAllocator<16>> Parts;
	Parts.SetNum(Strokes.Num());
	float TotalRate = 0.f;
	for (int32 Index = 0; Index < Strokes.Num(); ++Index)
	{
		if (!Strokes[Index] || !Sources.IsValidIndex(Index))
		{
			continue;
		}
		const FLBAAirflowSource& Source = Sources[Index];
		FPart& Part = Parts[Index];
		auto PointOf = [Parent, Owner](FName Bone, const FVector& Offset)
		{
			const FTransform Frame = !Parent ? Owner->GetActorTransform()
				: Bone.IsNone() ? Parent->GetComponentTransform() : Parent->GetSocketTransform(Bone);
			return Frame.TransformPosition(Offset);
		};
		Part.Start = PointOf(Source.Bone, Source.Offset);
		Part.End = Source.EndBone.IsNone() && Source.EndOffset.IsZero() ? Part.Start : PointOf(Source.EndBone.IsNone() ? Source.Bone : Source.EndBone, Source.EndOffset);
		Part.Center = (Part.Start + Part.End) * 0.5;

		// This part's own motion, flapping included: its velocity through the air and its acceleration (lightly
		// smoothed, the animation moves in keyframed steps).
		FSourceState& State = SourceStates[Index];
		if (State.bValid)
		{
			const FVector NewVelocity = FMath::VInterpTo(State.Velocity, (Part.Center - State.LastCenter) / DeltaTime, DeltaTime, 20.f);
			const FVector Acceleration = (NewVelocity - State.Velocity) / DeltaTime;
			State.Acceleration = FMath::VInterpTo(State.Acceleration, Acceleration.GetClampedToMaxSize(6000.0), DeltaTime, 8.f);
			State.Velocity = NewVelocity;
		}
		else
		{
			State.Velocity = Velocity;
		}
		State.LastCenter = Part.Center;
		State.bValid = true;
		// Strokes follow the whole body's flow, steered only partly by the part's own (flapping) motion.
		const float Follow = FMath::Clamp(Source.Follow, 0.f, 1.f);
		Part.Rel = Velocity - LocalAir + Follow * (State.Velocity - Velocity);
		Part.PartAcceleration = SmoothedAcceleration + Follow * (State.Acceleration - SmoothedAcceleration);
		// The body drags the air near it along: its strokes keep up with it a while, then are left behind.
		Part.Carry = AirVelocity * FMath::Clamp(Source.Wake, 0.f, 0.95f);

		// The flow breaks away from the body on its sides and at its back for the way it goes through the air, never
		// on the parts it pushes ahead (their strokes would start ahead of the body).
		Part.Trailing = FMath::Max(FMath::Clamp(0.5f - static_cast<float>(FVector::DotProduct((Part.Center - BodyCenter).GetSafeNormal(), PathDirection)), 0.f, 1.f), Source.MinShedding);
		Part.Weight = Source.Accent * Part.Trailing;
		Part.Rate = Density * Part.Weight * SpeedFactor;
		TotalRate += Part.Rate;
	}

	// The strokes due, shared out among the parts by their rates, never more than MaxStrokes on screen at once (the
	// ones the camera left behind no longer count) and no backlog while at the cap. While the body moves at all, at
	// least MinStrokes are on screen: the missing ones start at once from hotspots in view, different ones if possible.
	LiveStrokes.RemoveAll([this, Now](const FLiveStroke& Live) { return !IsOnScreen(Live, Now); });
	auto Pick = [&Parts](TFunctionRef<float(const FPart&)> Chance)
	{
		float Total = 0.f;
		for (const FPart& Part : Parts)
		{
			Total += Chance(Part);
		}
		float Left = FMath::FRand() * Total;
		int32 Chosen = INDEX_NONE;
		for (int32 Index = 0; Index < Parts.Num() && (Left > 0.f || Chosen == INDEX_NONE); ++Index)
		{
			const float PartChance = Chance(Parts[Index]);
			if (PartChance > 0.f)
			{
				Chosen = Index;
				Left -= PartChance;
			}
		}
		return Chosen;
	};
	auto StartStroke = [&](FPart& Part)
	{
		if (Part.NumNew++ == 0)
		{
			Part.Life = Life * FMath::FRandRange(0.7f, 1.3f);
		}
		FLiveStroke& Live = LiveStrokes.AddDefaulted_GetRef();
		Live.Origin = Part.Center;
		Live.HeadVelocity = -StreamRate * Part.Rel;
		Live.Drift = Drift;
		Live.Carry = Part.Carry;
		Live.Birth = Now;
		Live.Life = Part.Life;
		Live.Lag = FMath::Min(PathTimeOf(Part.Rel) / StreamRate, 0.5f * Part.Life);
	};
	if (bOn && bMoving)
	{
		const int32 Fewest = FMath::Min(MinStrokes, MaxStrokes);
		while (LiveStrokes.Num() < Fewest)
		{
			int32 Chosen = Pick([this](const FPart& Part) { return Part.NumNew == 0 && IsInView(Part.Center) ? Part.Weight : 0.f; });
			if (Chosen == INDEX_NONE)
			{
				Chosen = Pick([this](const FPart& Part) { return IsInView(Part.Center) ? Part.Weight : 0.f; });
			}
			if (Chosen == INDEX_NONE)
			{
				break;
			}
			StartStroke(Parts[Chosen]);
		}
		PendingStrokes += TotalRate * DeltaTime;
		while (PendingStrokes >= 1.f && LiveStrokes.Num() < MaxStrokes)
		{
			const int32 Chosen = Pick([](const FPart& Part) { return Part.Rate; });
			if (Chosen == INDEX_NONE)
			{
				break;
			}
			StartStroke(Parts[Chosen]);
			PendingStrokes -= 1.f;
		}
		PendingStrokes = FMath::Min(PendingStrokes, 1.f);
	}
	else
	{
		PendingStrokes = 0.f;
	}

	FString PartRates;
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		UNiagaraComponent* Stroke = Strokes[Index];
		if (!Stroke || !Sources.IsValidIndex(Index))
		{
			continue;
		}
		const FLBAAirflowSource& Source = Sources[Index];
		const FPart& Part = Parts[Index];
		const FVector& Rel = Part.Rel;
		const float PathTime = PathTimeOf(Rel);
		// The path bends with the acceleration, at most MaxCurve over the whole run of a stroke's head: the path turns
		// by about |bend| s / |Rel| at s seconds back, and the head runs back Stream x Life seconds of it.
		const float RunTime = FMath::Max(StreamRate * Life, PathTime);
		const float MaxBend = FMath::DegreesToRadians(MaxCurve) * Rel.Size() / RunTime;
		const FVector PathBend = (Curvature * Part.PartAcceleration).GetClampedToMaxSize(MaxBend);

		// The frame across the flow, for where strokes start around the part and which way they ripple.
		const FVector Direction = Rel.Size() > 1.0 ? Rel.GetSafeNormal() : Owner->GetActorForwardVector();
		FVector Perp1 = FVector::CrossProduct(Direction, FVector::UpVector).GetSafeNormal();
		if (Perp1.IsNearlyZero())
		{
			Perp1 = Owner->GetActorRightVector();
		}
		const FVector Perp2 = FVector::CrossProduct(Perp1, Direction).GetSafeNormal();

		Stroke->SetWorldLocation(Part.Center);
		Stroke->SetVariableInt(Param::SpawnCount, Part.NumNew * Points);
		Stroke->SetVariableInt(Param::Points, Points);
		Stroke->SetVariableVec3(Param::Extent, (Part.End - Part.Start) * 0.5);
		Stroke->SetVariableFloat(Param::Spread, Source.Spread);
		Stroke->SetVariableVec3(Param::Perp1, Perp1);
		Stroke->SetVariableVec3(Param::Perp2, Perp2);
		Stroke->SetVariableVec3(Param::Rel, Rel);
		Stroke->SetVariableVec3(Param::Bend, PathBend);
		Stroke->SetVariableFloat(Param::StrokeTime, PathTime);
		Stroke->SetVariableFloat(Param::Life, Part.NumNew > 0 ? Part.Life : Life);
		Stroke->SetVariableVec3(Param::Drag, Part.Carry);
		Stroke->SetVariableVec3(Param::Drift, Drift);
		Stroke->SetVariableFloat(Param::WakeTime, WakeTime);
		Stroke->SetVariableFloat(Param::Turbulence, Source.Turbulence * Ripple);
		Stroke->SetVariableFloat(Param::Visibility, Visibility);
		Stroke->SetVariableLinearColor(Param::Color, Color);
		Stroke->SetVariableFloat(Param::Width, Style.Width);
		Stroke->SetVariableFloat(Param::MinPixels, MinPixels);
		Stroke->SetVariableFloat(Param::PixelAngle, PixelAngle);
		Stroke->SetVariablePosition(Param::CameraPosition, CameraLocation);
		Stroke->SetVariableFloat(Param::NearFade, NearFade);
		Stroke->SetVariableFloat(Param::Stream, StreamRate);
		Stroke->SetVariableFloat(Param::LoopChance, FMath::Clamp(Style.LoopChance * Source.Accent, 0.f, 1.f));
		Stroke->SetVariableFloat(Param::LoopSize, Style.LoopSize);
		Stroke->SetVariableFloat(Param::FadeIn, FMath::Clamp(FadeInDistance / FMath::Max((AirVelocity - Part.Carry).Size(), 1.0), 0.03f, 0.25f));

#if ENABLE_DRAW_DEBUG
		if (Debug >= 1)
		{
			const FVector Local = Owner->GetActorTransform().InverseTransformPosition(Part.Center);
			PartRates += FString::Printf(TEXT(" %s(%.0f %.0f %.0f)x%.2f=%.1f"), *Source.Bone.ToString(), Local.X, Local.Y, Local.Z, Part.Trailing, Part.Rate);
		}
		if (Debug >= 2)
		{
			DrawDebugLine(GetWorld(), Part.Start, Part.End, FColor::Yellow);
			DrawDebugLine(GetWorld(), Part.Center, Part.Center + Rel * 0.05f, Part.Rate > 0.f ? FColor::Cyan : FColor::Red);
		}
#endif
	}

#if ENABLE_DRAW_DEBUG
	if (Debug >= 1 && GFrameCounter % 15 == 0)
	{
		UE_LOG(LogLBA2, Display, TEXT("Airflow %s: air speed %.1f m/s, wind %.1f m/s, lift %.0f N (%.2f g), downwash %.2f m/s, %.1f strokes/s, %d on screen, boldness %.2f, parts (actor space, trailing x rate):%s"),
			*Owner->GetName(), SpeedM, AirWind.Size() / 100.0, LiftN, LiftN / (Mass * GravityAcceleration / 100.0), Downwash / 100.0, TotalRate, LiveStrokes.Num(), Boldness, *PartRates);
	}
#endif
}
