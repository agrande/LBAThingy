#include "Formats/LBABody.h"

#include "Formats/LBAHqr.h"
#include "LBA2Runtime.h"

namespace
{
	// T_BODY_HEADER field offsets (LIB386/H/OBJECT/AFF_OBJ.H)
	constexpr int32 OffInfo = 0x00;
	constexpr int32 OffBounds = 0x08;
	constexpr int32 OffGroups = 0x20;
	constexpr int32 OffPoints = 0x28;
	constexpr int32 OffNormals = 0x30;
	constexpr int32 OffPolys = 0x40;
	constexpr int32 OffLines = 0x48;
	constexpr int32 OffSpheres = 0x50;
	constexpr int32 OffTextures = 0x58;
}

bool FLBABody::Parse(const TArray<uint8>& Data)
{
	FLBAReader R(Data);
	if (Data.Num() < 0x60)
	{
		return false;
	}

	Info = R.S32At(OffInfo);
	BoundsMin = FIntVector(R.S32At(OffBounds + 0), R.S32At(OffBounds + 8), R.S32At(OffBounds + 16));
	BoundsMax = FIntVector(R.S32At(OffBounds + 4), R.S32At(OffBounds + 12), R.S32At(OffBounds + 20));

	auto Section = [&R](int32 HeaderOffset, int32& OutCount, int32& OutOffset)
	{
		OutCount = R.S32At(HeaderOffset);
		OutOffset = R.S32At(HeaderOffset + 4);
	};

	int32 NumGroups, GroupsOffset, NumPoints, PointsOffset, NumNormals, NormalsOffset;
	int32 NumPolys, PolysOffset, NumLines, LinesOffset, NumSpheres, SpheresOffset, NumTextures, TexturesOffset;
	Section(OffGroups, NumGroups, GroupsOffset);
	Section(OffPoints, NumPoints, PointsOffset);
	Section(OffNormals, NumNormals, NormalsOffset);
	Section(OffPolys, NumPolys, PolysOffset);
	Section(OffLines, NumLines, LinesOffset);
	Section(OffSpheres, NumSpheres, SpheresOffset);
	Section(OffTextures, NumTextures, TexturesOffset);

	if (NumGroups < 0 || NumGroups > 256 || NumPoints < 0 || NumPoints > 65535)
	{
		return false;
	}

	int32 FirstPoint = 0;
	for (int32 Index = 0; Index < NumGroups; ++Index)
	{
		const int32 At = GroupsOffset + Index * 8;
		FLBABodyGroup& Group = Groups.AddDefaulted_GetRef();
		const uint16 Parent = R.U16At(At);
		Group.Parent = Parent == 0xFFFF ? INDEX_NONE : int32(Parent);
		Group.PivotPoint = R.U16At(At + 2);
		Group.NumPoints = R.U16At(At + 4);
		Group.NumNormals = R.U16At(At + 6);
		Group.FirstPoint = FirstPoint;
		FirstPoint += Group.NumPoints;
	}

	for (int32 Index = 0; Index < NumPoints; ++Index)
	{
		const int32 At = PointsOffset + Index * 8;
		FLBABodyPoint& Point = Points.AddDefaulted_GetRef();
		Point.Position = FIntVector(R.S16At(At), R.S16At(At + 2), R.S16At(At + 4));
		Point.Group = R.U16At(At + 6);
		if (!Groups.IsValidIndex(Point.Group))
		{
			Point.Group = 0;
		}
	}

	for (int32 Index = 0; Index < NumNormals; ++Index)
	{
		const int32 At = NormalsOffset + Index * 8;
		Normals.Add(FIntVector(R.S16At(At), R.S16At(At + 2), R.S16At(At + 4)));
	}

	// Polygons are stored in sections of the same type: { U16 Type; U16 Count; U32 OffsetToNextSection }.
	// The high bits of Type flag quads (0x8000) and environment-mapped polys (0x4000).
	int32 At = PolysOffset;
	const int32 PolysEnd = LinesOffset > PolysOffset ? LinesOffset : Data.Num();
	while (At + 8 <= PolysEnd)
	{
		const uint16 TypeWord = R.U16At(At);
		const int32 Count = R.U16At(At + 2);
		const int32 SectionSize = int32(R.U32At(At + 4));
		if (SectionSize <= 8 || Count == 0)
		{
			break;
		}
		const int32 BlockSize = (SectionSize - 8) / Count;
		const bool bQuad = (TypeWord & 0x8000) != 0;
		const bool bEnv = (TypeWord & 0x4000) != 0;
		const uint8 Type = uint8(TypeWord & 0xFF);
		const bool bTextured = Type >= ELBAPolyType::Texture && BlockSize >= 24;

		for (int32 PolyIndex = 0; PolyIndex < Count; ++PolyIndex)
		{
			const int32 P = At + 8 + PolyIndex * BlockSize;
			FLBABodyPoly& Poly = Polys.AddDefaulted_GetRef();
			Poly.Type = Type;
			Poly.bEnvMapped = bEnv;
			Poly.NumVertices = bQuad ? 4 : 3;
			for (int32 V = 0; V < Poly.NumVertices; ++V)
			{
				Poly.Vertices[V] = R.U16At(P + V * 2);
			}
			Poly.Color = R.U16At(P + 8);

			if (bTextured)
			{
				Poly.TextureHandle = bQuad ? R.U16At(P + 28) : R.U16At(P + 6);
				for (int32 V = 0; V < Poly.NumVertices; ++V)
				{
					// 8.8 fixed point texel coordinates.
					Poly.UV[V] = FVector2f(R.U16At(P + 12 + V * 4) / 256.f, R.U16At(P + 14 + V * 4) / 256.f);
				}
			}
		}
		At += SectionSize;
	}

	for (int32 Index = 0; Index < NumLines; ++Index)
	{
		const int32 L = LinesOffset + Index * 8;
		FLBABodyLine& Line = Lines.AddDefaulted_GetRef();
		Line.Type = R.U16At(L);
		Line.Color = R.U16At(L + 2);
		Line.Points[0] = R.U16At(L + 4);
		Line.Points[1] = R.U16At(L + 6);
	}

	for (int32 Index = 0; Index < NumSpheres; ++Index)
	{
		const int32 S = SpheresOffset + Index * 8;
		FLBABodySphere& Sphere = Spheres.AddDefaulted_GetRef();
		Sphere.Type = R.U16At(S);
		Sphere.Color = R.U16At(S + 2);
		Sphere.Point = R.U16At(S + 4);
		Sphere.Radius = R.U16At(S + 6);
	}

	for (int32 Index = 0; Index < NumTextures; ++Index)
	{
		// STRUC_OBJ_TEXTURE: { U16 OrgMap (x | y << 8); U16 RepMask (wmask | hmask << 8) }
		const int32 T = TexturesOffset + Index * 4;
		FLBATextureRegion& Region = Textures.AddDefaulted_GetRef();
		Region.X = R.U8At(T);
		Region.Y = R.U8At(T + 1);
		Region.Width = int32(R.U8At(T + 2)) + 1;
		Region.Height = int32(R.U8At(T + 3)) + 1;
	}

	// Drop polygons that reference missing data instead of failing the whole body.
	Polys.RemoveAll([this](const FLBABodyPoly& Poly)
	{
		for (int32 V = 0; V < Poly.NumVertices; ++V)
		{
			if (!Points.IsValidIndex(Poly.Vertices[V]))
			{
				return true;
			}
		}
		return false;
	});
	for (FLBABodyPoly& Poly : Polys)
	{
		if (Poly.TextureHandle != INDEX_NONE && !Textures.IsValidIndex(Poly.TextureHandle))
		{
			Poly.TextureHandle = Textures.Num() > 0 ? 0 : INDEX_NONE;
		}
	}

	return R.IsOk() && Points.Num() > 0;
}

TArray<FIntVector> FLBABody::ComputeGroupOrigins() const
{
	TArray<FIntVector> Origins;
	Origins.SetNumZeroed(Groups.Num());
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		const FLBABodyGroup& Group = Groups[Index];
		// Parents always come before children in LBA bodies.
		if (Group.Parent != INDEX_NONE && Group.Parent < Index && Points.IsValidIndex(Group.PivotPoint))
		{
			Origins[Index] = Origins[Group.Parent] + Points[Group.PivotPoint].Position;
		}
	}
	return Origins;
}

FIntVector FLBABody::GetRestPosition(int32 PointIndex, const TArray<FIntVector>& GroupOrigins) const
{
	const FLBABodyPoint& Point = Points[PointIndex];
	return GroupOrigins.IsValidIndex(Point.Group) ? GroupOrigins[Point.Group] + Point.Position : Point.Position;
}
