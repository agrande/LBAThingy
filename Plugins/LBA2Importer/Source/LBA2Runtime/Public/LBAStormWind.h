#pragma once

#include "CoreMinimal.h"
#include "LBAStormWind.generated.h"

/**
 * A small hurricane around an island, as one closed-form function of position and time (Evaluate): plain math on a
 * few parameters, no textures or state, so the same wind can be computed anywhere (gameplay, effects, and shaders:
 * Shaders/Private/LBAWind.ush is the same function, fed by GetShaderParameters).
 *
 * The island sits in the eye. Around it the wind swirls (Spin) and spirals in (InflowAngle), strongest on the eye
 * wall and dying off slowly beyond it:
 *
 *   r      distance from Center in eye radii: 0 at the center, 1 on the wall. The eye is an egg: each half axis has its
 *          own radius (actor space: ahead +X, behind -X, right +Y, left -Y), so the wall can hug one coast and leave
 *          another far away.
 *   speed  CalmSpeed + (WallSpeed - CalmSpeed) * r^EyeSharpness inside the wall (still at the center, rising
 *          steadily to the wall), WallSpeed * r^-OuterDecay outside; up to (1 + HeightGain) times that aloft.
 *   wind   speed * (cos(b) * along the wall + sin(b) * inward), b = InflowAngle over the outer WallWidth of the eye and
 *          beyond: the wall pushes back towards the island, hovering out there drifts.
 *
 * Gusts (travelling waves, stronger away from the eye and near the ceiling) vary its speed and direction. From
 * TurbulenceStart up to the Ceiling the air sinks, with sudden air pockets and bumps: flying higher gets hard, never
 * dangerous. Heights are above Center.Z, the sea level.
 */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAStormWind
{
	GENERATED_BODY()

	/** The eye's center (Z: the sea level) and the yaw of its axes (degrees). ALBAStorm sets them from its transform. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Eye")
	FVector Center = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Eye")
	float Yaw = 0.f;

	/** Distances from the center to the eye wall (cm) along each half axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Eye")
	float RadiusAhead = 8000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Eye")
	float RadiusBehind = 8000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Eye")
	float RadiusRight = 8000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Eye")
	float RadiusLeft = 8000.f;

	/** Wind at the center of the eye, and on the wall (cm/s, at sea level). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speed")
	float CalmSpeed = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speed")
	float WallSpeed = 800.f;

	/** How the wind rises toward the wall inside the eye (power of r): 1 steadily, higher a calmer and wider eye. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speed")
	float EyeSharpness = 1.5f;

	/** How slowly the wind dies off beyond the wall (power of 1 / r). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speed")
	float OuterDecay = 0.5f;

	/** Wind aloft: (1 + HeightGain) times stronger from GainHeight (cm above the sea) up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speed")
	float HeightGain = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speed")
	float GainHeight = 4000.f;

	/** 1 turns from +X toward +Y (clockwise seen from above), -1 the other way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Direction")
	float Spin = -1.f;

	/** How much the wind spirals in (degrees), from the inner edge of the wall (WallWidth, in eye radii) outward. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Direction")
	float InflowAngle = 25.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Direction")
	float WallWidth = 0.45f;

	/** Gusts: the speed varies by this fraction in the eye and far out, and the direction by up to GustAngle degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gusts")
	float EyeGustiness = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gusts")
	float OuterGustiness = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gusts")
	float GustAngle = 20.f;

	/** Turbulence from TurbulenceStart to the Ceiling (cm above the sea): extra gustiness, sinking air, air pockets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ceiling")
	float TurbulenceStart = 3500.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ceiling")
	float Ceiling = 6000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ceiling")
	float CeilingGustiness = 0.6f;

	/** Downward speeds at the ceiling (cm/s): steady sinking, sudden air pockets, and up and down bumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ceiling")
	float CeilingSink = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ceiling")
	float AirPocketSpeed = 1400.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ceiling")
	float BumpSpeed = 400.f;

	/** Wind velocity (cm/s) at Location at Time (seconds). */
	FVector Evaluate(const FVector& Location, double Time) const;

	/** Wind velocity without gusts or turbulence. */
	FVector EvaluateMean(const FVector& Location) const;

	/** Distance from the center in eye radii (1 on the wall). */
	float GetEyeDistance(const FVector& Location) const;

	/** Where the wall crosses the direction Angle (degrees from the eye's +X): for drawing it. */
	FVector GetWallPoint(float Angle) const;

	/** The wind packed for shaders (FLBAStormWindParameters in LBAWind.ush) at Time, named ShaderParameterNames. */
	static constexpr int32 NumShaderParameters = 8;
	static const FName ShaderParameterNames[NumShaderParameters];
	void GetShaderParameters(float Time, FLinearColor (&OutParameters)[NumShaderParameters]) const;

private:
	/** r, the outward direction across the eye's contours and the swirl, without gusts. */
	FVector EvaluateSwirl(const FVector& Location, float& OutEyeDistance) const;
};
