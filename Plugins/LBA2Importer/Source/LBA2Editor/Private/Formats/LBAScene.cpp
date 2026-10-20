#include "Formats/LBAScene.h"

#include "Formats/LBAHqr.h"

namespace
{
	TArray<uint8> ReadBlock(FLBAReader& R)
	{
		const int32 Size = R.S16();
		TArray<uint8> Block;
		if (Size > 0 && R.CanRead(Size))
		{
			Block.Append(R.GetData() + R.Tell(), Size);
		}
		R.Skip(FMath::Max(Size, 0));
		return Block;
	}
}

bool FLBAScene::Load(const TArray<uint8>& Data, int32 InIndex)
{
	Index = InIndex;
	FLBAReader R(Data);

	Island = R.S8();
	CubeX = R.S8();
	CubeY = R.S8();
	ShadowLevel = R.S8();
	Labyrinth = R.S8();
	CubeMode = R.S8();
	R.S8();

	Ambience.LightAlpha = R.S16();
	Ambience.LightBeta = R.S16();
	Ambience.Samples.SetNum(4);
	for (FLBASceneAmbientSample& Sample : Ambience.Samples)
	{
		Sample.Sample = R.S16();
		Sample.Repeat = R.S16();
		Sample.Random = R.S16();
		Sample.Frequency = R.S16();
		Sample.Volume = R.S16();
	}
	Ambience.MinDelay = R.S16();
	Ambience.RandomDelay = R.S16();
	Ambience.Jingle = R.S8();

	FLBASceneObjectData& Hero = Objects.AddDefaulted_GetRef();
	Hero.Name = TEXT("Twinsen");
	Hero.Entity = 0;
	Hero.Position.X = R.S16();
	Hero.Position.Y = R.S16();
	Hero.Position.Z = R.S16();
	Hero.TrackScript = ReadBlock(R);
	Hero.LifeScript = ReadBlock(R);

	const int32 NumObjects = R.S16();
	for (int32 ObjectIndex = 1; ObjectIndex < NumObjects && R.IsOk(); ++ObjectIndex)
	{
		FLBASceneObjectData& Object = Objects.AddDefaulted_GetRef();
		Object.Flags = int32(R.U32());
		Object.Entity = R.S16();
		// U8 body type of the entity (they go above 127); 255 = no body.
		Object.Body = R.U8();
		Object.Body = Object.Body == 255 ? INDEX_NONE : Object.Body;
		Object.Anim = R.S16();
		Object.Sprite = R.S16();
		Object.Position.X = R.S16();
		Object.Position.Y = R.S16();
		Object.Position.Z = R.S16();
		Object.HitForce = R.S8();
		Object.OptionFlags = R.S16();
		Object.Beta = R.S16();
		Object.RotationSpeed = R.S16();
		Object.MoveMode = R.S8();
		Object.Info.SetNum(4);
		for (int32& Info : Object.Info)
		{
			Info = R.S16();
		}
		Object.BonusCount = R.S16();
		Object.TextColor = R.S8();
		if (Object.HasFlag(LBASceneFlags::AnimatedSprite))
		{
			Object.SpriteAnim = int32(R.U32());
			Object.SpriteAnimFps = R.S16();
		}
		Object.Armour = R.S8();
		Object.LifePoints = R.S8();
		Object.TrackScript = ReadBlock(R);
		Object.LifeScript = ReadBlock(R);
	}

	R.U32();

	const int32 NumZones = R.S16();
	for (int32 ZoneIndex = 0; ZoneIndex < NumZones && R.IsOk(); ++ZoneIndex)
	{
		FLBASceneZoneData& Zone = Zones.AddDefaulted_GetRef();
		Zone.Min.X = R.S32();
		Zone.Min.Y = R.S32();
		Zone.Min.Z = R.S32();
		Zone.Max.X = R.S32();
		Zone.Max.Y = R.S32();
		Zone.Max.Z = R.S32();
		Zone.Info.SetNum(8);
		for (int32& Info : Zone.Info)
		{
			Info = R.S32();
		}
		Zone.Type = ELBAZoneType(FMath::Clamp<int32>(R.S16(), 0, int32(ELBAZoneType::Rail)));
		Zone.Num = R.S16();
	}

	const int32 NumPoints = R.S16();
	for (int32 PointIndex = 0; PointIndex < NumPoints && R.IsOk(); ++PointIndex)
	{
		FIntVector& Point = Points.AddDefaulted_GetRef();
		Point.X = R.S32();
		Point.Y = R.S32();
		Point.Z = R.S32();
	}
	return R.IsOk();
}

TArray<FString> LBAScenes::GetIslandFiles(int32 Island)
{
	static const TCHAR* IleLst[] =
	{
		TEXT("CITADEL"), TEXT("SENDELL"), TEXT("DESERT"), TEXT("EMERAUDE"), TEXT("OTRINGAL"), TEXT("CELEBRAT"),
		TEXT("PLATFORM"), TEXT("MOSQUIBE"), TEXT("KNARTAS"), TEXT("ILOTCX"), TEXT("ASCENCE"), TEXT("SOUSCELB"),
	};
	switch (Island)
	{
	case 0: return { TEXT("CITADEL"), TEXT("CITABAU") };
	case 5: return { TEXT("CELEBRAT"), TEXT("CELEBRA2") };
	default: return Island >= 0 && Island < UE_ARRAY_COUNT(IleLst) ? TArray<FString>{ IleLst[Island] } : TArray<FString>();
	}
}
