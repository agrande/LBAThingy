#pragma once

#include "Components/ActorComponent.h"
#include "LBAAirflowComponent.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;

/** How lines look at one end of the speed range: the component blends its Slow and Fast styles by speed. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAAirflowStyle
{
	GENERATED_BODY()

	/** Line width (cm) at its thickest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.1"))
	float Width = 6.f;

	/** Line opacity (times the color's alpha). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Opacity = 1.f;

	/** Line length (cm), each one varying by 30%. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "1.0"))
	float Length = 500.f;

	/** Fraction of lines ending in a curl. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HookChance = 0.15f;

	/** Curl radius, as a fraction of the line's length (each curl varies around it). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0"))
	float HookSize = 0.1f;
};

/** How wind lines look and how many there are. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAAirflowLook
{
	GENERATED_BODY()

	/** Line color; alpha scales the styles' opacity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FLinearColor Color = FLinearColor::White;

	/** Lines when the body barely moves: short and very faint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FLBAAirflowStyle Slow = { 1.5f, 0.15f, 150.f, 0.f, 0.12f };

	/** Lines from FullSpeed on: long and a little clearer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FLBAAirflowStyle Fast = { 4.f, 0.45f, 500.f, 0.f, 0.1f };

	/** Air speed from which lines have the Fast style. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "1.0", Units = "cm/s"))
	float FullSpeed = 1800.f;

	/**
	 * How quickly lines leave the Slow style for the Fast one: 1 = in step with the speed, more = bold early (at 1.5,
	 * half of FullSpeed is 65% of the way).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.1"))
	float BoldnessBias = 1.5f;

	/** Lines per second at RefSpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0"))
	float Density = 5.f;

	/** Air speed Density is given at; the rate grows mostly as the square of the air speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (Units = "cm/s"))
	float RefSpeed = 1500.f;

	/** Fewest lines on screen while the body moves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0"))
	int32 MinStrokes = 1;

	/** Most lines on screen at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0"))
	int32 MaxStrokes = 4;

	/** Horizontal ground speed at or below which the body counts as standing still: no lines, even in the wind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "cm/s"))
	float StillSpeed = 30.f;

	/**
	 * Vertical speed at or below which the body counts as standing still too. High enough that the bobbing left by wing
	 * beats while hovering (see VerticalSmoothing) makes no lines.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "cm/s"))
	float StillVerticalSpeed = 150.f;

	/**
	 * Seconds over which the body's vertical speed is averaged: a wing beat jolts the body up and down, and lines should
	 * not swing or bend with each beat. More = steadier lines, slower to follow a real climb or dive.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "s"))
	float VerticalSmoothing = 0.5f;

	/** How fast a line's head runs back along the flow, as a fraction of the air speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.05"))
	float Stream = 1.f;

	/** Fewest seconds a line takes to grow to its full length, at high speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.01", Units = "s"))
	float MinGrowTime = 0.2f;

	/** Most seconds a line takes to grow to its full length, at low speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.01", Units = "s"))
	float MaxGrowTime = 0.6f;

	/** Time a line slides at its full length, as a fraction of its grow time (it then shrinks as long as it grew). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0"))
	float Slide = 0.5f;

	/** Seconds over which a line fades in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.001", Units = "s"))
	float FadeIn = 0.08f;

	/** Opacity of a line's tail, against its head's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TailOpacity = 0.5f;

	/** Lines are kept at least this many pixels wide; thinner ones fade instead of flickering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.5"))
	float MinPixels = 1.f;

	/** Widest a line gets on screen, as a fraction of the viewport's width, however close it passes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0001"))
	float MaxScreenWidth = 0.003f;

	/** How much lines bend with the path the body takes (turns, pull-outs): 0 = not at all, 1 = the real curve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0"))
	float Curvature = 1.f;

	/**
	 * How much a line bows even in straight flight, towards the body's way: degrees its path turns over its run (each one
	 * varies by 50%). More makes the lines stray from the direction of the air.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "deg"))
	float BowAngle = 5.f;

	/** Most a line's path turns over its run, bow included, however hard the body turns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "180.0", Units = "deg"))
	float MaxCurve = 30.f;

	/** How far a curl turns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "720.0", Units = "deg"))
	float HookTurn = 240.f;

	/** Ribbon points per line (smoothness of its curve, curl and taper). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "3", ClampMax = "64"))
	int32 PointsPerStroke = 32;

	/** Lines fade out closer than this to the camera. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "cm"))
	float NearFade = 300.f;
};

/** Where wind lines start: on the flyer's extremities, or in the air around it. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAAirflowPlacement
{
	GENERATED_BODY()

	/** Share of lines starting on the body's extremities (the snout, the wings...); the rest start in the air around it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ExtremityShare = 0.8f;

	/**
	 * How much of the body's motion a line keeps: 1 = it grows out of the extremity and stays with it until it lets go,
	 * 0 = it stays in the air, soon left behind by the body. Whatever the value, a line slides along itself.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Carry = 0.8f;

	/** Fewest seconds between two lines from the same extremity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "s"))
	float ExtremityInterval = 0.35f;

	/**
	 * Lines from an extremity: only if it is this far sideways on screen from the body's way. Closer, the line would run
	 * straight down the screen along the body, over it, like something falling off it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "cm"))
	float MinSideOffset = 60.f;

	/** Lines in the air: closest they start to the body's way through the air. The Dinofly's wingtips are about 250 cm out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "cm"))
	float InnerRadius = 200.f;

	/** Lines in the air: farthest they start from the body's way: more fills more of the screen, farther from the body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "cm"))
	float OuterRadius = 450.f;

	/** Lines in the air: where the head starts along the body's way, ahead of the body (negative = behind it). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (Units = "cm"))
	float StartAheadMin = -200.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (Units = "cm"))
	float StartAheadMax = 150.f;

	/**
	 * Lines in the air: none within this angle, seen along the body's way, of the plane through the camera and that way: on
	 * screen they would run straight along the body's own line, down from under the tail (like something falling off it).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", ClampMax = "89.0", Units = "deg"))
	float WedgeAngle = 20.f;

	/** Shortest a line may look at its full length, as a fraction of the viewport's width: lines seen end-on are dots. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0"))
	float MinScreenLength = 0.05f;

	/** Closest a line's middle may pass to the camera. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0", Units = "cm"))
	float CameraClearance = 400.f;
};

/** An extremity of the flyer's body that the air hits: lines start on it. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAAirflowExtremity
{
	GENERATED_BODY()

	/** Bone or socket of the owner's skeletal mesh. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FName Bone;

	/** Offset in the bone's space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FVector Offset = FVector::ZeroVector;

	/** How often lines start here against the other extremities (it also takes how much the air hits it this moment). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow", meta = (ClampMin = "0.0"))
	float Accent = 1.f;
};

/**
 * Wind lines around a flying actor: a few long, thin, faint comic lines showing how fast it goes through the air and which
 * way, drawn by a Niagara ribbon system (NS_LBA_Airflow), one instance per line slot.
 *
 * Every line lies along the flyer's motion through the air (its velocity minus the wind) and slides along itself, so that
 * seen from a camera following it, all the lines fan out from the point it flies towards: above the body in a climb, below
 * it in a dive, away from its heading in a crosswind. That pattern is what they are for, so all that takes a line off that
 * direction is the bend of the body's own path in a turn, and a slight bow towards the body's way, the lines closing in
 * behind the body like its wake. A random curl (HookChance) is possible but off by default.
 *
 * Wing beats make no lines: the body's vertical speed is averaged over them (VerticalSmoothing), and the wings' own motion
 * plays no part.
 *
 * A line is led by its head, which runs back along the path: it grows from nothing, slides and shrinks into its head, with
 * sharp ends.
 *
 * Most lines start on the Extremities the air hits the most this moment, as long as they are beside the body on screen
 * (MinSideOffset; going forward from behind that leaves the wings): a line going straight back from the tail or the feet
 * runs straight down the screen, like something falling off the body. The other lines start in the air close around the
 * flyer's way (a ring from InnerRadius to OuterRadius), never in the plane through the camera and that way (WedgeAngle), for
 * the same reason.
 *
 * As long as the body moves, at least MinStrokes are on screen; the faster it goes through the air, the more (as the square
 * of its air speed, like the energy it carries, up to MaxStrokes at once) and the clearer: short and very faint when it
 * barely moves, long and a little clearer at speed (the Slow and Fast styles). None when it stands still.
 *
 * Look and Placement are set by the owner (the Dinofly's come from Project Settings > Game > LBA2 Dinofly); the drawing is
 * the Niagara system's.
 */
UCLASS(ClassGroup = LBA, meta = (BlueprintSpawnableComponent))
class LBA2RUNTIME_API ULBAAirflowComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULBAAirflowComponent();

	/** Line system. Empty uses Project Settings > Plugins > LBA2 > Airflow Trail. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Airflow")
	TSoftObjectPtr<UNiagaraSystem> System;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FLBAAirflowLook Look;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	FLBAAirflowPlacement Placement;

	/** Where lines start on the body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow")
	TArray<FLBAAirflowExtremity> Extremities;

	/** Wind (cm/s), added to the level's (ULBAWindSubsystem). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics")
	FVector Wind = FVector::ZeroVector;

	/** The air moves with the level's wind (Citadel's storm, see ALBAStorm) where the body is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Airflow|Physics")
	bool bUseLevelWind = true;

	/** Starts or stops new lines (the ones drawn finish). The owner turns it off when landed. */
	UFUNCTION(BlueprintCallable, Category = "Airflow")
	void SetEmitting(bool bEmit) { bEmitting = bEmit; }

	UFUNCTION(BlueprintPure, Category = "Airflow")
	bool IsEmitting() const { return bEmitting; }

	/** The wind the lines drift with (cm/s). */
	UFUNCTION(BlueprintPure, Category = "Airflow")
	FVector GetWind() const { return Wind + LevelWind; }

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** One line: everything the Niagara system needs to draw it, set on a slot the frame it starts. */
	struct FLine
	{
		FVector Base = FVector::ZeroVector;
		/** Head velocity along the path, and how that path bends (cm/s, cm/s2). */
		FVector HeadVelocity = FVector::ZeroVector;
		FVector Bend = FVector::ZeroVector;
		/** Velocity of the air carrying the whole line (cm/s). */
		FVector Drift = FVector::ZeroVector;
		/** Across the line, facing the camera: which way it bows and curls. */
		FVector Across = FVector::ZeroVector;
		double Birth = 0.0;
		float Life = 0.f;
		/** Seconds the tail lags behind the head, which is also the time the line takes to grow. */
		float Lag = 0.f;
		float HookRadius = 0.f;
		float HookTime = 0.f;
		float Width = 0.f;
	};

	void CreateStrokes();
	/** Camera location, the size of a pixel 1 cm in front of it (radians) and the viewport's width (pixels). */
	bool GetCamera(FVector& OutLocation, float& OutPixelAngle, float& OutViewportWidth) const;
	/** Whether any of the line (head, middle or tail) is in the player's view. */
	bool IsOnScreen(const FLine& Line, double Now) const;
	/** Whether a point is in the player's view. */
	bool IsInView(const FVector& Location) const;

	/** One Niagara system instance per line slot. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> Strokes;

	/** Lines drawn. */
	TArray<FLine> LiveLines;
	/** When each extremity last started a line. */
	TArray<double> ExtremityLastLines;
	/** The next slot to draw a line with. */
	int32 NextSlot = 0;
	/** Lines owed (fractional, carried over frames). */
	float PendingStrokes = 0.f;

	bool bCreated = false;
	bool bEmitting = true;
	FVector LevelWind = FVector::ZeroVector;
	/** Last frame's velocity, without the wing beats (see VerticalSmoothing). */
	FVector LastVelocity = FVector::ZeroVector;
	FVector SmoothedAcceleration = FVector::ZeroVector;
	bool bHasLastVelocity = false;
};
