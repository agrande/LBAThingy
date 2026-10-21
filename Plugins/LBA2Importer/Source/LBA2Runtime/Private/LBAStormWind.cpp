#include "LBAStormWind.h"

namespace
{
	/** One travelling wave of the gust patterns: wavelengths along X and Y (cm, negative: other way), rad/s, phase. */
	double Wave(const FVector& P, double Time, double LengthX, double LengthY, double Speed, double Phase)
	{
		return FMath::Sin(P.X / LengthX + P.Y / LengthY - Speed * Time + Phase);
	}
}

const FName FLBAStormWind::ShaderParameterNames[NumShaderParameters] = {
	TEXT("WindCenter"), TEXT("WindAxes"), TEXT("WindRadii"), TEXT("WindSpeed"), TEXT("WindShape"), TEXT("WindGusts"), TEXT("WindCeiling"), TEXT("WindBumps") };

void FLBAStormWind::GetShaderParameters(float Time, FLinearColor (&OutParameters)[NumShaderParameters]) const
{
	float Sin = 0.f;
	float Cos = 1.f;
	FMath::SinCos(&Sin, &Cos, FMath::DegreesToRadians(Yaw));
	OutParameters[0] = FLinearColor(Center.X, Center.Y, Center.Z, 1.f);
	OutParameters[1] = FLinearColor(Cos, Sin, Spin, Time);
	OutParameters[2] = FLinearColor(RadiusAhead, RadiusBehind, RadiusRight, RadiusLeft);
	OutParameters[3] = FLinearColor(CalmSpeed, WallSpeed, EyeSharpness, OuterDecay);
	OutParameters[4] = FLinearColor(HeightGain, GainHeight, FMath::DegreesToRadians(InflowAngle), WallWidth);
	OutParameters[5] = FLinearColor(EyeGustiness, OuterGustiness, FMath::DegreesToRadians(GustAngle), CeilingGustiness);
	OutParameters[6] = FLinearColor(TurbulenceStart, Ceiling, CeilingSink, AirPocketSpeed);
	OutParameters[7] = FLinearColor(BumpSpeed, 0.f, 0.f, 0.f);
}

FVector FLBAStormWind::EvaluateSwirl(const FVector& Location, float& OutEyeDistance) const
{
	// Into the eye's frame, in eye radii along each half axis.
	double Sin = 0.0;
	double Cos = 1.0;
	FMath::SinCos(&Sin, &Cos, FMath::DegreesToRadians(double(Yaw)));
	const double DX = Location.X - Center.X;
	const double DY = Location.Y - Center.Y;
	const double LX = Cos * DX + Sin * DY;
	const double LY = -Sin * DX + Cos * DY;
	const double RX = FMath::Max(double(LX >= 0.0 ? RadiusAhead : RadiusBehind), 1.0);
	const double RY = FMath::Max(double(LY >= 0.0 ? RadiusRight : RadiusLeft), 1.0);
	const double QX = LX / RX;
	const double QY = LY / RY;
	const double R = FMath::Sqrt(QX * QX + QY * QY);
	OutEyeDistance = float(R);

	// Outward across the eye's contours (the gradient of r), so the swirl follows the egg's shape.
	double GX = QX / RX;
	double GY = QY / RY;
	const double Gradient = FMath::Max(FMath::Sqrt(GX * GX + GY * GY), 1e-9);
	GX /= Gradient;
	GY /= Gradient;
	const FVector2D Out(Cos * GX - Sin * GY, Sin * GX + Cos * GY);
	const FVector2D Along = FVector2D(-Out.Y, Out.X) * Spin;

	const double Wall = 1.0 - WallWidth;
	const double Ramp = FMath::Clamp((R - Wall) / FMath::Max(double(WallWidth), 1e-3), 0.0, 1.0);
	double Speed = R <= 1.0
		? CalmSpeed + (WallSpeed - CalmSpeed) * FMath::Pow(R, double(EyeSharpness))
		: WallSpeed * FMath::Pow(R, -double(OuterDecay));
	Speed *= 1.0 + HeightGain * FMath::SmoothStep(0.0, double(FMath::Max(GainHeight, 1.f)), Location.Z - Center.Z);
	const double Inflow = FMath::DegreesToRadians(double(InflowAngle)) * Ramp;
	const FVector2D Wind = Speed * (FMath::Cos(Inflow) * Along - FMath::Sin(Inflow) * Out);
	return FVector(Wind.X, Wind.Y, 0.0);
}

FVector FLBAStormWind::EvaluateMean(const FVector& Location) const
{
	float R = 0.f;
	FVector Wind = EvaluateSwirl(Location, R);
	Wind.Z = -CeilingSink * FMath::SmoothStep(double(TurbulenceStart), double(FMath::Max(Ceiling, TurbulenceStart + 1.f)), Location.Z - Center.Z);
	return Wind;
}

FVector FLBAStormWind::Evaluate(const FVector& Location, double Time) const
{
	float R = 0.f;
	const FVector Mean = EvaluateSwirl(Location, R);
	const double Height = Location.Z - Center.Z;
	const double Turbulence = FMath::SmoothStep(double(TurbulenceStart), double(FMath::Max(Ceiling, TurbulenceStart + 1.f)), Height);

	// Gusts: a few travelling waves (tens of meters, a few seconds) vary the speed and swing the direction.
	const double Gustiness = EyeGustiness + (OuterGustiness - EyeGustiness) * FMath::SmoothStep(0.7, 1.5, double(R))
		+ CeilingGustiness * Turbulence;
	const double SpeedNoise = (Wave(Location, Time, 2400.0, 3700.0, 1.1, 0.0) + Wave(Location, Time, -3100.0, 1900.0, 1.7, 1.3)
		+ Wave(Location, Time, 1500.0, -2700.0, 2.3, 4.1)) / 3.0;
	const double AngleNoise = (Wave(Location, Time, 2900.0, 2100.0, 0.9, 2.2) + Wave(Location, Time, -1700.0, -3300.0, 1.9, 5.0)) / 2.0;
	const double Swing = FMath::DegreesToRadians(double(GustAngle)) * Gustiness / FMath::Max(double(OuterGustiness), 1e-3) * AngleNoise;
	const FVector2D Gust = FVector2D(Mean.X, Mean.Y).GetRotated(FMath::RadiansToDegrees(Swing)) * (1.0 + Gustiness * SpeedNoise);

	// Near the ceiling the air sinks, drops in sudden pockets (where two waves peak together) and bumps up and down.
	const double Pocket = FMath::Pow(FMath::Clamp(Wave(Location, Time, 900.0, 1300.0, 2.9, 0.7) * Wave(Location, Time, -1100.0, 700.0, 2.1, 3.3), 0.0, 1.0), 2.0);
	const double Bump = (Wave(Location, Time, 600.0, -800.0, 4.0, 1.9) + Wave(Location, Time, -700.0, -500.0, 5.3, 0.4)) / 2.0;
	const double Vertical = Turbulence * (-CeilingSink - AirPocketSpeed * Pocket + BumpSpeed * Bump);
	return FVector(Gust.X, Gust.Y, Vertical);
}

float FLBAStormWind::GetEyeDistance(const FVector& Location) const
{
	float R = 0.f;
	EvaluateSwirl(Location, R);
	return R;
}

FVector FLBAStormWind::GetWallPoint(float Angle) const
{
	const FVector2D Direction(FMath::Cos(FMath::DegreesToRadians(Angle)), FMath::Sin(FMath::DegreesToRadians(Angle)));
	const FVector2D Scaled(Direction.X / FMath::Max(Direction.X >= 0.0 ? RadiusAhead : RadiusBehind, 1.f),
		Direction.Y / FMath::Max(Direction.Y >= 0.0 ? RadiusRight : RadiusLeft, 1.f));
	const FVector2D Local = Direction / FMath::Max(Scaled.Size(), 1e-9);
	const FVector2D World = Local.GetRotated(Yaw);
	return FVector(Center.X + World.X, Center.Y + World.Y, Center.Z);
}
