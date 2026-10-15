#include "Formats/LBAAnim.h"

#include "Formats/LBAHqr.h"

bool FLBAAnim::Parse(const TArray<uint8>& Data)
{
	FLBAReader R(Data);
	const int32 NumKeyframes = R.U16At(0);
	NumGroups = R.U16At(2);
	LoopFrame = R.U16At(4);
	if (NumKeyframes <= 0 || NumGroups <= 0 || NumGroups > 64 || 8 + NumKeyframes * (8 + NumGroups * 8) > Data.Num())
	{
		return false;
	}
	int32 At = 8;
	for (int32 K = 0; K < NumKeyframes; ++K)
	{
		FLBAAnimKeyframe& Key = Keyframes.AddDefaulted_GetRef();
		Key.DurationMs = R.U16At(At);
		Key.Step = FIntVector(R.S16At(At + 2), R.S16At(At + 4), R.S16At(At + 6));
		At += 8;
		Key.Groups.SetNum(NumGroups);
		for (int32 G = 0; G < NumGroups; ++G)
		{
			Key.Groups[G] = { R.S16At(At), R.S16At(At + 2), R.S16At(At + 4), R.S16At(At + 6) };
			At += 8;
		}
	}
	LoopFrame = FMath::Clamp(LoopFrame, 0, NumKeyframes - 1);
	return R.IsOk();
}

int32 FLBAAnim::GetTotalDurationMs() const
{
	int32 Total = 0;
	for (const FLBAAnimKeyframe& Key : Keyframes)
	{
		Total += Key.DurationMs;
	}
	return Total;
}

const TCHAR* FLBAAnimAction::GetTypeName(int32 Type)
{
	static const TCHAR* Names[] =
	{
		TEXT("Nop"), TEXT("Body"), TEXT("BodP"), TEXT("Anim"), TEXT("AniP"), TEXT("Hit"), TEXT("Sample"), TEXT("SampleRnd"),
		TEXT("Throw"), TEXT("ThrowMagic"), TEXT("SampleRepeat"), TEXT("ThrowSearch"), TEXT("ThrowAlpha"), TEXT("SampleStop"),
		TEXT("ZV"), TEXT("LeftStep"), TEXT("RightStep"), TEXT("HitHero"), TEXT("Throw3D"), TEXT("Throw3DAlpha"),
		TEXT("Throw3DSearch"), TEXT("Throw3DMagic"), TEXT("SuperHit"), TEXT("ThrowObj3D"), TEXT("Path"), TEXT("Flow"),
		TEXT("Flow3D"), TEXT("ThrowDart"), TEXT("Shield"), TEXT("SampleMagic"), TEXT("Throw3DConque"), TEXT("ZVAnimIt"),
		TEXT("Impact"), TEXT("Renvoie"), TEXT("Renvoyable"), TEXT("Transparent"), TEXT("Scale"), TEXT("LeftJump"),
		TEXT("RightJump"), TEXT("NewSample"), TEXT("Impact3D"), TEXT("ThrowMagicExtra"), TEXT("ThrowFoudre"),
	};
	return Type >= 0 && Type < int32(UE_ARRAY_COUNT(Names)) ? Names[Type] : TEXT("Unknown");
}

namespace
{
	/** Total byte size of an action record (type and frame included), from the lba2remake parser. */
	int32 ActionSize(int32 Type)
	{
		switch (Type)
		{
		case 5: return 3;   // HIT
		case 6: return 4;   // SAMPLE
		case 7: return 6;   // SAMPLE_RND
		case 8: return 13;  // THROW
		case 9: return 9;   // THROW_MAGIC
		case 10: return 11; // SAMPLE_REPEAT
		case 11: return 9;  // THROW_SEARCH
		case 12: return 13; // THROW_ALPHA
		case 13: return 4;  // SAMPLE_STOP
		case 14: return 14; // ZV
		case 15: case 16: case 17: return 2; // LEFT_STEP, RIGHT_STEP, HIT_HERO
		case 18: case 19: return 17; // THROW_3D, THROW_3D_ALPHA
		case 20: return 13; // THROW_3D_SEARCH
		case 21: return 13; // THROW_3D_MAGIC
		case 22: return 11; // SUPER_HIT
		case 23: return 18; // THROW_OBJ_3D
		case 26: return 9;  // FLOW_3D
		case 27: return 10; // THROW_DART
		case 28: return 3;  // SHIELD
		case 29: return 2;  // SAMPLE_MAGIC
		case 30: return 9;  // THROW_3D_CONQUE
		case 31: return 2;  // ZV_ANIMIT
		case 32: return 3;  // IMPACT
		case 33: case 34: return 2; // RENVOIE, RENVOYABLE
		case 35: return 1;  // TRANSPARENT
		case 36: return 5;  // SCALE
		case 37: case 38: return 2; // LEFT_JUMP, RIGHT_JUMP
		case 39: return 9;  // NEW_SAMPLE
		case 40: return 10; // IMPACT_3D
		case 41: return 10; // THROW_MAGIC_EXTRA
		case 42: return 2;  // THROW_FOUDRE
		default: return 0;
		}
	}
}

TArray<FLBAEntity> FLBAEntity::ParseAll(const TArray<uint8>& Data)
{
	TArray<FLBAEntity> Entities;
	FLBAReader R(Data);
	const int32 NumEntities = int32(R.U32At(0) / 4) - 1;
	for (int32 Index = 0; Index < NumEntities; ++Index)
	{
		FLBAEntity& Entity = Entities.AddDefaulted_GetRef();
		Entity.Index = Index;
		int32 At = int32(R.U32At(Index * 4));
		for (int32 Guard = 0; Guard < 512 && At < Data.Num(); ++Guard)
		{
			const uint8 Opcode = R.U8At(At++);
			if (Opcode == 0xFF)
			{
				break;
			}
			if (Opcode == 1)
			{
				// Body: { U8 Type; U8 Size; S16 BodyIndex; U8 HasBox; [U8 ZV; S16 x6] }
				FLBAEntityBodyEntry& Body = Entity.Bodies.AddDefaulted_GetRef();
				Body.Type = R.U8At(At);
				const int32 Size = R.U8At(At + 1);
				Body.BodyIndex = R.S16At(At + 2);
				Body.bHasBox = R.U8At(At + 4) == 1 && R.U8At(At + 5) == ELBAAnimActionType::ZV;
				if (Body.bHasBox)
				{
					Body.BoxMin = FIntVector(R.S16At(At + 6), R.S16At(At + 8), R.S16At(At + 10));
					Body.BoxMax = FIntVector(R.S16At(At + 12), R.S16At(At + 14), R.S16At(At + 16));
				}
				At += Size + 1;
			}
			else if (Opcode == 3)
			{
				// Anim: { U16 Type; U8 Size; S16 AnimIndex; U8 NumActions; actions... }
				FLBAEntityAnimEntry& Anim = Entity.Anims.AddDefaulted_GetRef();
				Anim.Type = R.U16At(At);
				const int32 Size = R.U8At(At + 2);
				Anim.AnimIndex = R.S16At(At + 3);
				const int32 RecordEnd = At + 2 + Size; // Size counts itself, the anim index and the actions
				if (Size > 2)
				{
					const int32 NumActions = R.U8At(At + 5);
					int32 ActionAt = At + 6;
					for (int32 ActionIndex = 0; ActionIndex < NumActions; ++ActionIndex)
					{
						const int32 Type = R.U8At(ActionAt);
						const int32 SizeOfAction = ActionSize(Type);
						if (SizeOfAction == 0 || ActionAt + SizeOfAction > RecordEnd)
						{
							break;
						}
						FLBAAnimAction& Action = Anim.Actions.AddDefaulted_GetRef();
						Action.Type = Type;
						Action.Frame = SizeOfAction > 1 ? R.U8At(ActionAt + 1) : 0;
						for (int32 B = 2; B < SizeOfAction; ++B)
						{
							Action.Params.Add(R.U8At(ActionAt + B));
						}
						ActionAt += SizeOfAction;
					}
				}
				At += Size ? Size + 2 : 5;
			}
			else
			{
				break;
			}
		}
	}
	return Entities;
}
