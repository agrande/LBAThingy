#include "Formats/LBAIsland.h"

#include "Formats/LBAHqr.h"
#include "LBA2Runtime.h"
#include "LBATypes.h"

namespace
{
	// LOADISLE.CPP
	constexpr int32 HqrMap = 0;
	constexpr int32 HqrGroundTexture = 1;
	constexpr int32 HqrObjectTexture = 2;
	constexpr int32 HqrStartCube = 3;
	constexpr int32 HqrStepCube = 6;
	constexpr int32 CubeInfo = 0;
	constexpr int32 CubeDecors = 1;
	constexpr int32 CubeGround = 2;
	constexpr int32 CubeTexDef = 3;
	constexpr int32 CubeHeights = 4;
	constexpr int32 CubeLight = 5;
}

FIntVector FLBAIslandSection::GetOrigin() const
{
	return FIntVector(CubeX * LBA::IslandCubeSize, 0, CubeZ * LBA::IslandCubeSize);
}

bool FLBAIsland::Load(const FLBAHqrFile& Ile)
{
	Name = FPaths::GetBaseFilename(Ile.GetPath()).ToUpper();
	Map = Ile.GetEntry(HqrMap);
	GroundTexture = Ile.GetEntry(HqrGroundTexture);
	ObjectTexture = Ile.GetEntry(HqrObjectTexture);
	if (Map.Num() < 256 || GroundTexture.Num() < 65536 || ObjectTexture.Num() < 65536)
	{
		UE_LOG(LogLBA2, Warning, TEXT("%s: missing island map or textures"), *Ile.GetPath());
		return false;
	}

	int32 Order = 0;
	for (int32 MapIndex = 0; MapIndex < 256; ++MapIndex)
	{
		const int32 Id = Map[MapIndex] & 127;
		if (Id == 0)
		{
			continue;
		}

		const int32 Base = HqrStartCube + HqrStepCube * (Id - 1);
		FLBAIslandSection& Section = Sections.AddDefaulted_GetRef();
		Section.Id = Id;
		Section.CubeX = MapIndex % 16;
		Section.CubeZ = MapIndex / 16;
		Section.Order = Order++;

		const TArray<uint8> Info = Ile.GetEntry(Base + CubeInfo);
		FLBAReader InfoReader(Info);
		Section.LightAlpha = InfoReader.S32At(0) & 0xFFFF;
		Section.LightBeta = InfoReader.S32At(4);
		const int32 NumDecors = InfoReader.S32At(8);
		Section.SkyY = InfoReader.S32At(12);
		Section.FogStart = InfoReader.S32At(16);
		Section.ClipFar = InfoReader.S32At(20);

		if (NumDecors > 0)
		{
			const TArray<uint8> Decors = Ile.GetEntry(Base + CubeDecors);
			FLBAReader R(Decors);
			for (int32 DecorIndex = 0; DecorIndex < NumDecors && R.CanRead(48); ++DecorIndex)
			{
				FLBAIslandDecor& Decor = Section.Decors.AddDefaulted_GetRef();
				Decor.Body = R.S32();
				Decor.Position.X = R.S32();
				Decor.Position.Y = R.S32();
				Decor.Position.Z = R.S32();
				Decor.CodeJeu = R.S32();
				const int32 BetaWord = R.S32();
				Decor.Beta = BetaWord & 0xFFF;
				Decor.ConditionVar = (BetaWord >> 16) & 0xFF;
				Decor.ConditionValue = (BetaWord >> 24) & 0xFF;
				Decor.BoundsMin.X = R.S32();
				Decor.BoundsMin.Y = R.S32();
				Decor.BoundsMin.Z = R.S32();
				Decor.BoundsMax.X = R.S32();
				Decor.BoundsMax.Y = R.S32();
				Decor.BoundsMax.Z = R.S32();
			}
		}

		const TArray<uint8> Ground = Ile.GetEntry(Base + CubeGround);
		{
			FLBAReader R(Ground);
			Section.Triangles.SetNum(64 * 64 * 2);
			for (int32 Index = 0; Index < Section.Triangles.Num(); ++Index)
			{
				Section.Triangles[Index].Raw = R.U32At(Index * 4);
			}
		}

		const TArray<uint8> TexDef = Ile.GetEntry(Base + CubeTexDef);
		{
			// T_HALF_TEX: 3 x (U16 Tx, U16 Ty), 8.8 fixed point texels.
			FLBAReader R(TexDef);
			const int32 NumTex = TexDef.Num() / 12;
			Section.TexCoords.SetNum(NumTex * 3);
			for (int32 Index = 0; Index < NumTex * 3; ++Index)
			{
				Section.TexCoords[Index] = FVector2f(R.U16At(Index * 4) / 256.f, R.U16At(Index * 4 + 2) / 256.f);
			}
		}

		const TArray<uint8> Heights = Ile.GetEntry(Base + CubeHeights);
		{
			FLBAReader R(Heights);
			Section.Heights.SetNum(65 * 65);
			for (int32 Index = 0; Index < 65 * 65; ++Index)
			{
				Section.Heights[Index] = R.S16At(Index * 2);
			}
		}

		Section.Intensities = Ile.GetEntry(Base + CubeLight);
		Section.Intensities.SetNumZeroed(65 * 65);
	}

	return Sections.Num() > 0;
}

const FLBAIslandSection* FLBAIsland::FindSection(int32 CubeX, int32 CubeZ) const
{
	return Sections.FindByPredicate([CubeX, CubeZ](const FLBAIslandSection& S) { return S.CubeX == CubeX && S.CubeZ == CubeZ; });
}

void FLBAIsland::GetTriangleCorners(int32 Sens, int32 TriangleIndex, FIntPoint OutCorners[3])
{
	// Derived from the remake's TRIANGLE_POINTS table, expressed as (x, z) cell offsets.
	static const FIntPoint Table[2][2][3] =
	{
		{ { {0, 0}, {0, 1}, {1, 1} }, { {1, 1}, {1, 0}, {0, 0} } },
		{ { {1, 0}, {0, 0}, {0, 1} }, { {0, 1}, {1, 1}, {1, 0} } },
	};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		OutCorners[Index] = Table[Sens & 1][TriangleIndex & 1][Index];
	}
}
