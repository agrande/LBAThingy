#include "LBAPlanet.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"

double FLBAPlanetCap::GetAngle(double Distance) const
{
	return Distance <= CapRadius ? FMath::Atan(Distance / Radius) : GetCapAngle() + (Distance - CapRadius) / Radius;
}

double FLBAPlanetCap::GetDistance(double Angle) const
{
	const double CapAngle = GetCapAngle();
	return Angle <= CapAngle ? Radius * FMath::Tan(Angle) : CapRadius + (Angle - CapAngle) * Radius;
}

double FLBAPlanetCap::GetRoundness(double Distance) const
{
	if (BlendRadius <= CapRadius)
	{
		return Distance > CapRadius ? 1.0 : 0.0;
	}
	return FMath::SmoothStep(CapRadius, BlendRadius, Distance);
}

double FLBAPlanetCap::GetSeaRadius(double Angle) const
{
	const double Roundness = GetRoundness(GetDistance(Angle));
	if (Roundness >= 1.0)
	{
		return Radius;
	}
	// The cap's plane is Radius / cos(Angle) from the center.
	return FMath::Lerp(Radius / FMath::Cos(Angle), Radius, Roundness);
}

FVector FLBAPlanetCap::Bend(const FVector& Point, const FVector2D& Center) const
{
	const FVector2D Offset = FVector2D(Point) - Center;
	const double Distance = Offset.Size();
	if (Distance <= CapRadius)
	{
		return Point;
	}
	const FVector2D Along = Offset / Distance;
	const double Angle = GetAngle(Distance);
	const FVector Direction(Along * FMath::Sin(Angle), FMath::Cos(Angle));
	const FVector Up = FMath::Lerp(FVector::UpVector, Direction, GetRoundness(Distance)).GetSafeNormal();
	const FVector PlanetCenter(Center, -Radius);
	return PlanetCenter + Direction * GetSeaRadius(Angle) + Up * Point.Z;
}

FVector FLBAPlanetIsland::GetUp() const
{
	const double Lat = FMath::DegreesToRadians(Latitude);
	const double Lon = FMath::DegreesToRadians(Longitude);
	return FVector(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
}

FQuat FLBAPlanetIsland::GetRotation() const
{
	const double Lon = FMath::DegreesToRadians(Longitude);
	const double Head = FMath::DegreesToRadians(Heading);
	const FVector Up = GetUp();
	const FVector East(-FMath::Sin(Lon), FMath::Cos(Lon), 0.0);
	const FVector North = FVector::CrossProduct(Up, East);
	const FVector X = East * FMath::Cos(Head) + North * FMath::Sin(Head);
	const FVector Y = FVector::CrossProduct(Up, X);
	return FQuat(FMatrix(X, Y, Up, FVector::ZeroVector)).GetNormalized();
}

FTransform FLBAPlanetIsland::GetLevelToPlanet(double Radius) const
{
	const FQuat Rotation = GetRotation();
	return FTransform(Rotation, -Rotation.RotateVector(FVector(Center, -Radius)));
}

FLBAPlanetCap FLBAPlanetIsland::GetCap(double Radius) const
{
	FLBAPlanetCap Cap;
	Cap.Radius = Radius;
	Cap.CapRadius = CapRadius;
	Cap.BlendRadius = FMath::Max(BlendRadius, CapRadius);
	return Cap;
}

const FLBAPlanetIsland* ULBAPlanetAsset::FindIsland(FName Island) const
{
	return Islands.FindByPredicate([Island](const FLBAPlanetIsland& Entry) { return Entry.Island == Island; });
}

ALBAPlanetActor::ALBAPlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Frame"));
	RootComponent->SetMobility(EComponentMobility::Movable);

	// The disc is placed every frame under the player (absolute), its material bends it onto the planet: no collision
	// (ULBAPlanetSubsystem answers sea queries), no shadows, not part of the distance fields Lumen traces.
	Sea = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sea"));
	Sea->SetupAttachment(RootComponent);
	Sea->SetMobility(EComponentMobility::Movable);
	Sea->SetUsingAbsoluteLocation(true);
	Sea->SetUsingAbsoluteRotation(true);
	Sea->SetUsingAbsoluteScale(true);
	Sea->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sea->SetCastShadow(false);
	Sea->bAffectDistanceFieldLighting = false;
	Sea->bAffectDynamicIndirectLighting = false;
	Sea->SetCanEverAffectNavigation(false);

	SecondSun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("SecondSun"));
	SecondSun->SetupAttachment(RootComponent);
	SecondSun->SetMobility(EComponentMobility::Movable);
	SecondSun->SetAtmosphereSunLight(true);
	SecondSun->SetAtmosphereSunLightIndex(1);
	SecondSun->SetVisibility(false);
}
