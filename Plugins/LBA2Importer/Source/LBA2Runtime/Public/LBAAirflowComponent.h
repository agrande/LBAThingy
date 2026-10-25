#pragma once

#include "Components/ActorComponent.h"
#include "LBAAirflowComponent.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;
class USceneComponent;

/**
 * A hotspot of the flyer's body where the airflow breaks away (a wingtip, the tail, a foot, the rider...): a point of
 * its skeleton (or a segment between two), where flow strokes are born. It sheds them only when it is on the body's
 * side or back for the way the body goes through the air: a foot when climbing or flying on, the rider's head when
 * diving, never a part the body pushes ahead.
 */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAAirflowSource
{
	GENERATED_BODY()

	/** Start of the segment: a bone or socket of the owner's skeletal mesh (none = the mesh's origin) and an offset in its space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FName Bone;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FVector Offset = FVector::ZeroVector;

	/** End of the segment (none = a single point). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FName EndBone;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FVector EndOffset = FVector::ZeroVector;

	/** Radius (cm) around the point (or segment) where strokes start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	float Spread = 10.f;

	/** How hard the flow breaks on this part (stroke rate factor): tips and blunt edges more, sheltered parts less. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	float Accent = 1.f;

	/** Part of its rate it keeps whichever way the body goes (wingtips shed their vortex however they swing). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinShedding = 0.f;

	/** Fraction of the body's motion through the air its strokes are carried along with at first (its wake). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	float Wake = 0.2f;

	/**
	 * How much its own motion (flapping) steers its strokes, against the whole body's flow: 0 = strokes follow the
	 * body's path through the air, 1 = this part's own path (a flapping wing's arcs).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	float Follow = 0.f;

	/** Ripple amplitude (cm) of its strokes: separated, turbulent flow behind blunt parts ripples more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	float Turbulence = 6.f;
};

/** How strokes look at one end of the speed range: the component blends its Slow and Fast styles by speed. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAAirflowStyle
{
	GENERATED_BODY()

	/** Stroke width (cm) at its thickest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.1"))
	float Width = 6.f;

	/** Stroke opacity (times the color's alpha). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Opacity = 1.f;

	/** Scales the stroke's length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.05"))
	float Length = 1.f;

	/** Scales the stroke's life (StrokeLife): lingering strokes have time to loop. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.1"))
	float Life = 1.f;

	/** Fraction of strokes that do a little loop on their way (times the hotspot's Accent). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LoopChance = 0.25f;

	/** Loop radius, as a fraction of the stroke's length (each loop varies around it). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0"))
	float LoopSize = 0.1f;
};

/**
 * Airflow strokes around a flying actor: a few short comic speed lines that come off where the flow breaks away from its
 * body (hotspots on its sides and back for the way it goes) and fade as quickly, drawn by a Niagara ribbon system
 * (NS_LBA_Airflow), one instance per hotspot (source).
 *
 * A stroke is led by its head, which runs back along the path its part of the body just took through the air (its
 * velocity relative to the air, bent by its acceleration), the tail following the same path: strokes show which way
 * the flyer goes (up, down, forward, turning) and stream past like the air. Their path follows the air: held back near
 * the body by the air it drags along, sinking with the downwash, blown by the wind, rippling, and now and then doing a
 * little loop. A stroke fades in as it leaves the body, grows, runs, then shrinks into its head. As long as the body
 * moves, at least MinStrokes are on screen; the faster it goes through the air, the more (as the square of its air
 * speed, like the energy it carries, up to MaxStrokes at once) and the bolder: small, faint and loopy when it barely
 * moves, quickly fat, opaque and straight (the Slow and Fast styles). None when it stands still. Flying into the wind
 * gives more and longer strokes blown back, flying with it fewer.
 *
 * The look (color, width, density...) is set here; the drawing is the Niagara system's.
 */
UCLASS(ClassGroup = LBA, meta = (BlueprintSpawnableComponent))
class LBA2RUNTIME_API ULBAAirflowComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULBAAirflowComponent();

	/** Hotspots the strokes come from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	TArray<FLBAAirflowSource> Sources;

	/** Stroke system. Empty uses Project Settings > Plugins > LBA2 > Airflow Trail. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Airflow")
	TSoftObjectPtr<UNiagaraSystem> System;

	/** Stroke color; alpha scales the styles' opacity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look")
	FLinearColor Color = FLinearColor::White;

	/** Strokes when the body barely moves: small, faint, loopy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look")
	FLBAAirflowStyle Slow = { 2.5f, 0.35f, 0.6f, 1.6f, 0.9f, 0.15f };

	/** Strokes from FullSpeed on: fat, opaque, straight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look")
	FLBAAirflowStyle Fast = { 22.f, 1.f, 1.f, 1.f, 0.15f, 0.1f };

	/** Air speed (cm/s) from which strokes have the Fast style. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "1.0"))
	float FullSpeed = 1800.f;

	/**
	 * How quickly strokes leave the Slow style for the Fast one: 1 = in step with the speed, more = bold early (at 3,
	 * a sixth of FullSpeed is already 40% of the way). The Slow style is for barely moving.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.1"))
	float BoldnessBias = 3.f;

	/** Strokes are kept at least this many pixels wide; thinner ones fade instead of flickering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.5"))
	float MinPixels = 2.5f;

	/** Seconds a stroke lives, from fading in to shrinking into its head (times the style's Life; each one varies by 30%). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.05"))
	float StrokeLife = 0.45f;

	/** Seconds of a body part's path a stroke spans: its length is that part's air speed times this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.01"))
	float StrokeTime = 0.09f;

	/** Shortest stroke (cm): slow flight still draws readable strokes, a bit longer than its path would. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0"))
	float MinStrokeLength = 70.f;

	/** Strokes per second from a hotspot at RefSpeed (Accent 1, right at the body's back). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0"))
	float Density = 2.f;

	/** Fewest strokes on screen while the body moves at all (started from the hotspots in view). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0"))
	int32 MinStrokes = 2;

	/** Most strokes on screen at once, all hotspots together. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0"))
	int32 MaxStrokes = 6;

	/** Strokes are invisible at birth and fade in while the body leaves them this far behind (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0"))
	float FadeInDistance = 80.f;

	/** Ground speed (cm/s) at or below which the body counts as standing still: no strokes, even in the wind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0"))
	float StillSpeed = 5.f;

	/** Air speed (cm/s) Density is given at; the rate grows mostly as the square of the air speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look")
	float RefSpeed = 1500.f;

	/** How much strokes bend with the path the body takes (turns, pull-outs, flapping): 0 = straight, 1 = the real curve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0"))
	float Curvature = 1.f;

	/** Most a stroke's path turns over its whole run (degrees), however hard the body turns or flaps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float MaxCurve = 25.f;

	/** Scales the body parts' Turbulence: how much strokes ripple. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0"))
	float Ripple = 0.5f;

	/**
	 * How fast a stroke's head runs back along the flow, as a fraction of the air speed, on top of the air streaming
	 * past (strokes are led by their head, their tail following the same path).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.05"))
	float Stream = 0.35f;

	/** Ribbon points per stroke (smoothness of its curve, loops and taper). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "3", ClampMax = "32"))
	int32 PointsPerStroke = 20;

	/** Strokes fade out closer than this to the camera (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Look", meta = (ClampMin = "0.0"))
	float NearFade = 300.f;

	/** Mass carried by the wings (kg): with gravity and the flyer's acceleration it gives the lift, hence the downwash. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics", meta = (ClampMin = "0.1"))
	float Mass = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics", meta = (ClampMin = "0.01"))
	float AirDensity = 1.225f;

	/** Wingspan (cm) for the downwash. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics", meta = (ClampMin = "10.0"))
	float Span = 500.f;

	/** Scales how fast marked air sinks under the lift's downwash. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics", meta = (ClampMin = "0.0"))
	float DownwashScale = 1.f;

	/** Seconds over which air dragged along by the body slows back down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics", meta = (ClampMin = "0.01"))
	float WakeTime = 0.6f;

	/** Wind (cm/s), added to the level's (ULBAWindSubsystem). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics")
	FVector Wind = FVector::ZeroVector;

	/** The air moves with the level's wind (Citadel's storm, see ALBAStorm) where the body is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics")
	bool bUseLevelWind = true;

	/** Starts or stops new strokes (the ones drawn finish). The owner turns it off when landed. */
	UFUNCTION(BlueprintCallable, Category = "Airflow")
	void SetEmitting(bool bEmit) { bEmitting = bEmit; }

	UFUNCTION(BlueprintPure, Category = "Airflow")
	bool IsEmitting() const { return bEmitting; }

	/** The wind the strokes drift with (cm/s). */
	UFUNCTION(BlueprintPure, Category = "Airflow")
	FVector GetWind() const { return Wind + LevelWind; }

	/**
	 * The world was moved with the owner (globe mode, ULBAPlanetSubsystem): its motion is moved along, the strokes in
	 * the air (simulated in the world) are dropped rather than left behind.
	 */
	void OnWorldMoved(const FTransform& Delta);

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void CreateStrokes();
	USceneComponent* GetSourceParent() const;
	/** Camera location and the size of a pixel 1 cm in front of it (radians), for the strokes' minimum width. */
	bool GetCamera(FVector& OutLocation, float& OutPixelAngle) const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> Strokes;

	struct FSourceState
	{
		FVector LastCenter = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FVector Acceleration = FVector::ZeroVector;
		bool bValid = false;
	};
	TArray<FSourceState> SourceStates;

	/** Strokes owed (fractional, carried over frames). */
	float PendingStrokes = 0.f;

	/** A stroke drawn, roughly: where it starts and how its head and tail move, to tell whether it is on screen. */
	struct FLiveStroke
	{
		FVector Origin = FVector::ZeroVector;
		/** Head velocity along the path, drift of the whole stroke and the wake carrying it at first (cm/s). */
		FVector HeadVelocity = FVector::ZeroVector;
		FVector Drift = FVector::ZeroVector;
		FVector Carry = FVector::ZeroVector;
		double Birth = 0.0;
		float Life = 0.f;
		/** Seconds the tail lags behind the head. */
		float Lag = 0.f;
	};
	TArray<FLiveStroke> LiveStrokes;
	/** Whether any of the stroke (head, middle or tail) is in the player's view. */
	bool IsOnScreen(const FLiveStroke& Live, double Now) const;
	/** Whether a point is in the player's view. */
	bool IsInView(const FVector& Location) const;

	bool bCreated = false;
	bool bEmitting = true;
	FVector LevelWind = FVector::ZeroVector;
	FVector LastVelocity = FVector::ZeroVector;
	FVector SmoothedAcceleration = FVector::ZeroVector;
	bool bHasLastVelocity = false;
};
