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

TArray<int32> LBAScenes::FindDoubleDoors(const TArray<FDoorLeaf>& Leaves)
{
	// Scene positions are hand-placed: the Temple of Bu's leaves are up to 16 units more or less than a leaf's width apart.
	constexpr int32 Tolerance = 64;
	auto Nearest = [&Leaves](int32 Index)
	{
		const FDoorLeaf& Leaf = Leaves[Index];
		int32 Best = INDEX_NONE;
		int32 BestDistance = MAX_int32;
		for (int32 Other = 0; Other < Leaves.Num(); ++Other)
		{
			const FIntVector Offset = Leaves[Other].Position - Leaf.Position;
			// The other leaf lies behind this one's opening.
			const int32 Behind = -(Offset.X * Leaf.Direction.X + Offset.Z * Leaf.Direction.Z);
			const int32 Across = FMath::Abs(Offset.X * Leaf.Direction.Z) + FMath::Abs(Offset.Z * Leaf.Direction.X);
			if (Other != Index && Leaves[Other].Direction + Leaf.Direction == FIntVector::ZeroValue && Offset.Y == 0 && Across <= Tolerance
				&& Behind > 0 && Behind <= Leaf.Width + Tolerance && Behind < BestDistance)
			{
				Best = Other;
				BestDistance = Behind;
			}
		}
		return Best;
	};
	TArray<int32> Pairs;
	Pairs.Init(INDEX_NONE, Leaves.Num());
	for (int32 Index = 0; Index < Leaves.Num(); ++Index)
	{
		const int32 Other = Nearest(Index);
		if (Other != INDEX_NONE && Nearest(Other) == Index)
		{
			Pairs[Index] = Other;
		}
	}
	return Pairs;
}

bool LBAScenes::FindDoorTrack(const TArray<uint8>& Track, FDoorTrack& OutDoor)
{
	// Track macros (TM_* in COMMON.H) and the size of their operands, as SearchOffsetTrack skips them.
	enum : uint8
	{
		TM_END = 0, TM_LABEL = 9, TM_STOP = 11, TM_SAMPLE = 14, TM_OPEN_LEFT = 21, TM_OPEN_DOWN = 24, TM_CLOSE = 25,
		TM_SAMPLE_RND = 27, TM_SAMPLE_ALWAYS = 28, TM_PLAY_ACF = 30, TM_SIMPLE_SAMPLE = 32,
	};
	static const uint8 OperandSize[] =
	{
		0, 0, 1, 2, 1, 0, 4, 2, 1, 1, 2, 0, 1, 2, 2, 1, 2, 1, 5, 0, 2, 2, 2, 2, 2, 0, 0, 2, 2, 2, 0, 2, 2,
		2, 4, 0, 5, 0, 2, 5, 0, 1, 1, 1, 1, 1, 0, 0, 1, 5, 2, 2, 1,
	};
	// OPEN_LEFT, OPEN_RIGHT, OPEN_UP, OPEN_DOWN.
	static const FIntVector Directions[] = { FIntVector(-1, 0, 0), FIntVector(1, 0, 0), FIntVector(0, 0, -1), FIntVector(0, 0, 1) };

	FDoorTrack Door;
	int32 LastSample = INDEX_NONE;
	int32 At = 0;
	while (At < Track.Num())
	{
		const uint8 Macro = Track[At++];
		if (Macro == TM_END || Macro >= UE_ARRAY_COUNT(OperandSize))
		{
			break;
		}
		const int32 Operand = At + 1 < Track.Num() ? int32(int16(Track[At] | (Track[At + 1] << 8))) : 0;
		if (Macro == TM_LABEL || Macro == TM_STOP)
		{
			LastSample = INDEX_NONE;
		}
		else if (Macro == TM_SAMPLE || Macro == TM_SAMPLE_RND || Macro == TM_SAMPLE_ALWAYS || Macro == TM_SIMPLE_SAMPLE)
		{
			LastSample = uint16(Operand);
		}
		else if (Macro >= TM_OPEN_LEFT && Macro <= TM_OPEN_DOWN && Door.Width <= 0)
		{
			Door.Direction = Directions[Macro - TM_OPEN_LEFT];
			Door.Width = Operand;
			Door.OpenSample = LastSample;
		}
		else if (Macro == TM_CLOSE && Door.CloseSample == INDEX_NONE)
		{
			Door.CloseSample = LastSample;
		}

		if (Macro == TM_PLAY_ACF)
		{
			while (At < Track.Num() && Track[At++] != 0)
			{
			}
		}
		else
		{
			At += OperandSize[Macro];
		}
	}
	if (Door.Width <= 0)
	{
		return false;
	}
	OutDoor = Door;
	return true;
}
