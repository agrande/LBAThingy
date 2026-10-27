#include "LBAPlanet.h"

#include "Components/StaticMeshComponent.h"

FVector FLBAPlanetIsland::GetUp() const
{
	const double Lat = FMath::DegreesToRadians(Latitude);
	const double Lon = FMath::DegreesToRadians(Longitude);
	return FVector(FMath::Cos(Lat) * FMath::Cos(Lon), FMath::Cos(Lat) * FMath::Sin(Lon), FMath::Sin(Lat));
}

FVector2D FLBAPlanetIsland::GetMapLocation(double Radius) const
{
	const double FromPole = Radius * FMath::DegreesToRadians(90.0 - Latitude);
	const double Lon = FMath::DegreesToRadians(Longitude);
	return FVector2D(FMath::Cos(Lon), FMath::Sin(Lon)) * FromPole;
}

double FLBAPlanetIsland::GetMapYaw() const
{
	// On the map, east turns about the pole (along +longitude) and north points at the pole.
	const double Lon = FMath::DegreesToRadians(Longitude);
	const double Head = FMath::DegreesToRadians(Heading);
	const FVector2D East(-FMath::Sin(Lon), FMath::Cos(Lon));
	const FVector2D North(-FMath::Cos(Lon), -FMath::Sin(Lon));
	const FVector2D X = East * FMath::Cos(Head) + North * FMath::Sin(Head);
	return FMath::RadiansToDegrees(FMath::Atan2(X.Y, X.X));
}

FTransform FLBAPlanetIsland::GetLevelToMap(double Radius) const
{
	const FQuat Rotation(FVector::UpVector, FMath::DegreesToRadians(GetMapYaw()));
	const FVector Location(GetMapLocation(Radius), 0.0);
	return FTransform(Rotation, Location - Rotation.RotateVector(FVector(Center, 0.0)));
}

const FName ULBAPlanetAsset::BeltTag(TEXT("LBA_Belt"));

const FLBAPlanetIsland* ULBAPlanetAsset::FindIsland(FName Island) const
{
	return Islands.FindByPredicate([Island](const FLBAPlanetIsland& Entry) { return Entry.Island == Island; });
}

int32 ULBAPlanetAsset::FindIslandIndex(FName Island) const
{
	return Islands.IndexOfByPredicate([Island](const FLBAPlanetIsland& Entry) { return Entry.Island == Island; });
}

FTransform ULBAPlanetAsset::GetIslandToHome(int32 Island, double InRadius) const
{
	const int32 Home = FindIslandIndex(HomeIsland);
	if (!Islands.IsValidIndex(Island) || !Islands.IsValidIndex(Home))
	{
		return FTransform::Identity;
	}
	return Islands[Island].GetLevelToMap(InRadius) * Islands[Home].GetLevelToMap(InRadius).Inverse();
}

ALBAPlanetActor::ALBAPlanetActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Frame"));
	RootComponent->SetMobility(EComponentMobility::Movable);

	// The disc is placed every frame under the camera, its material bends it onto the planet: no collision
	// (ULBAPlanetSubsystem answers sea queries), no shadows, not part of the distance fields Lumen traces.
	Sea = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sea"));
	Sea->SetupAttachment(RootComponent);
	Sea->SetMobility(EComponentMobility::Movable);
	Sea->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sea->SetCastShadow(false);
	Sea->bAffectDistanceFieldLighting = false;
	Sea->bAffectDynamicIndirectLighting = false;
	Sea->SetCanEverAffectNavigation(false);
}
