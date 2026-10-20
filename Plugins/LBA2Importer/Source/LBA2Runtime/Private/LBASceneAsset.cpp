#include "LBASceneAsset.h"

#include "LBATypes.h"

FVector ULBASceneAsset::ToWorld(const FIntVector& LbaPosition) const
{
	return Origin + LBA::ToUnreal(LbaPosition.X, LbaPosition.Y, LbaPosition.Z, WorldScale);
}

FIntVector ULBASceneAsset::ToScene(const FVector& WorldPosition) const
{
	const FVector Local = (WorldPosition - Origin) / FMath::Max(WorldScale, UE_SMALL_NUMBER);
	return FIntVector(FMath::RoundToInt(Local.X), FMath::RoundToInt(Local.Z), FMath::RoundToInt(Local.Y));
}

double ULBASceneAsset::BetaToActorYaw(int32 Beta)
{
	// LBA forward (sin b, 0, cos b) maps to Unreal (sin b, cos b, 0), i.e. yaw 90 - b.
	return FRotator::NormalizeAxis(90.0 - LBA::AngleToDegrees(Beta));
}

int32 ULBASceneAsset::ActorYawToBeta(double Yaw)
{
	return FMath::RoundToInt((90.0 - Yaw) * LBA::AngleFullTurn / 360.0) & (LBA::AngleFullTurn - 1);
}
