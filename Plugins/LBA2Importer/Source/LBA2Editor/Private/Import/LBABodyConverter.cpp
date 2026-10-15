#include "Import/LBABodyConverter.h"

#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "LBA2Runtime.h"
#include "LBATypes.h"

namespace
{
	FVector3f ToUE(const FIntVector& P, float Scale)
	{
		return FVector3f(LBA::ToUnreal(P.X, P.Y, P.Z, Scale));
	}

	FVector3f DirToUE(const FIntVector& N)
	{
		return FVector3f(LBA::DirToUnreal(N.X, N.Y, N.Z)).GetSafeNormal();
	}
}

FColor LBABodyConverter::GetAlbedo(const TArray<FColor>& Palette, uint16 Color, uint8 PolyType)
{
	const int32 Index = Color & 0xFF;
	if (PolyType == ELBAPolyType::Solid || PolyType == ELBAPolyType::Trans || PolyType == ELBAPolyType::Trame)
	{
		return Palette[Index];
	}
	const int32 Bank = Index & 0xF0;
	const int32 Base = Index & 0x0F;
	return Palette[Bank + FMath::Clamp(Base + 9, 0, 14)];
}

FVector4f LBABodyConverter::ToVertexColor(const FColor& Color, float BakedLight)
{
	const FLinearColor Linear = FLinearColor::FromSRGBColor(Color);
	return FVector4f(Linear.R, Linear.G, Linear.B, BakedLight);
}

bool LBABodyConverter::Convert(const FLBABody& Body, const FLBABodyConvertParams& Params, FLBAMeshBuilder& Out)
{
	check(Params.Palette);
	const TArray<FColor>& Palette = *Params.Palette;
	const TArray<FIntVector> Origins = Body.ComputeGroupOrigins();
	const bool bHasPointNormals = Body.Normals.Num() == Body.Points.Num();

	TArray<FVector3f> Positions;
	Positions.SetNumUninitialized(Body.Points.Num());
	for (int32 Index = 0; Index < Body.Points.Num(); ++Index)
	{
		Positions[Index] = ToUE(Body.GetRestPosition(Index, Origins), Params.Scale);
	}
	auto PointNormal = [&](int32 Index) -> FVector3f
	{
		return bHasPointNormals ? DirToUE(Body.Normals[Index]) : FVector3f::ZeroVector;
	};
	auto Bone = [&](int32 Index) { return Params.bSkinned ? Body.Points[Index].Group : 0; };

	// LBA polygon order is already front-facing once mapped to Unreal space (checked against the authored
	// vertex normals of every island object: the few disagreements are flat parts with unreliable normals).
	constexpr bool bFlip = false;

	const int32 ColorSection = Out.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color), Params.ColorMaterial);
	int32 TranslucentSection = INDEX_NONE;

	for (const FLBABodyPoly& Poly : Body.Polys)
	{
		int32 Section = ColorSection;
		FColor Albedo = GetAlbedo(Palette, Poly.Color, Poly.Type);
		FLBATextureRegion Region;
		const bool bTextured = Poly.IsTextured() && Params.GetTexturedMaterial && Body.Textures.IsValidIndex(Poly.TextureHandle);

		if (bTextured)
		{
			Region = Body.Textures[Poly.TextureHandle];
			const bool bMasked = Poly.UsesColorKey();
			UMaterialInterface* Material = Params.GetTexturedMaterial(Region, bMasked);
			const FName Slot(*FString::Printf(TEXT("Tex_%s%s"), *Region.GetKey(), bMasked ? TEXT("_Masked") : TEXT("")));
			Section = Out.GetSection(Slot, Material);
			Albedo = FColor::White;
		}
		else if (Poly.IsTransparent())
		{
			if (TranslucentSection == INDEX_NONE)
			{
				TranslucentSection = Out.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Translucent), Params.TranslucentMaterial);
			}
			Section = TranslucentSection;
		}

		const FVector4f VertexColor = ToVertexColor(Albedo);
		const bool bFlat = Poly.IsFlatShaded() || !bHasPointNormals;

		// Fan triangulation: (0,1,2) and (0,2,3) for quads.
		for (int32 Tri = 0; Tri + 2 < Poly.NumVertices; ++Tri)
		{
			int32 Corners[3] = { 0, Tri + 1, Tri + 2 };
			if (bFlip)
			{
				Swap(Corners[1], Corners[2]);
			}
			FLBAVertex V[3];
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 Point = Poly.Vertices[Corners[K]];
				V[K].Position = Positions[Point];
				V[K].Normal = PointNormal(Point);
				V[K].Color = VertexColor;
				V[K].Bone = Bone(Point);
				if (bTextured)
				{
					// Texture regions repeat (power-of-two masks), so the UV is texels / region size.
					const FVector2f Texel = Poly.UV[Corners[K]];
					V[K].UV = FVector2f(Texel.X / Region.Width, Texel.Y / Region.Height);
				}
			}
			if (bFlat)
			{
				const FVector3f Face = FLBAMeshBuilder::FrontNormal(V[0].Position, V[1].Position, V[2].Position).GetSafeNormal();
				V[0].Normal = V[1].Normal = V[2].Normal = Face;
			}
			Out.AddTriangle(Section, V[0], V[1], V[2]);
		}
	}

	// Spheres (eyes, heads, balls...) become low-poly UV spheres.
	for (const FLBABodySphere& Sphere : Body.Spheres)
	{
		if (!Body.Points.IsValidIndex(Sphere.Point) || Sphere.Radius <= 0)
		{
			continue;
		}
		const FVector3f Center = Positions[Sphere.Point];
		const float Radius = Sphere.Radius * Params.Scale;
		const FVector4f VertexColor = ToVertexColor(GetAlbedo(Palette, Sphere.Color, ELBAPolyType::Gouraud));
		const int32 SphereBone = Bone(Sphere.Point);
		constexpr int32 Segments = 10;
		constexpr int32 Rings = 6;
		auto MakeVertex = [&](int32 Ring, int32 Segment)
		{
			const float Theta = PI * Ring / Rings;
			const float Phi = 2.f * PI * Segment / Segments;
			const FVector3f Dir(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
			FLBAVertex V;
			V.Position = Center + Dir * Radius;
			V.Normal = Dir;
			V.Color = VertexColor;
			V.Bone = SphereBone;
			return V;
		};
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				const FLBAVertex A = MakeVertex(Ring, Segment);
				const FLBAVertex B = MakeVertex(Ring + 1, Segment);
				const FLBAVertex C = MakeVertex(Ring + 1, Segment + 1);
				const FLBAVertex D = MakeVertex(Ring, Segment + 1);
				const FVector3f Outward = ((A.Position + B.Position + C.Position + D.Position) * 0.25f) - Center;
				if (Ring > 0)
				{
					Out.AddTriangleFacing(ColorSection, A, B, D, Outward);
				}
				if (Ring < Rings - 1)
				{
					Out.AddTriangleFacing(ColorSection, B, C, D, Outward);
				}
			}
		}
	}

	// Lines (antennas, whiskers, strings) become thin square tubes.
	for (const FLBABodyLine& Line : Body.Lines)
	{
		if (!Body.Points.IsValidIndex(Line.Points[0]) || !Body.Points.IsValidIndex(Line.Points[1]))
		{
			continue;
		}
		const FVector3f P0 = Positions[Line.Points[0]];
		const FVector3f P1 = Positions[Line.Points[1]];
		const FVector3f Axis = (P1 - P0).GetSafeNormal();
		if (Axis.IsNearlyZero())
		{
			continue;
		}
		const FVector3f Side = (FMath::Abs(Axis.Z) < 0.9f ? (Axis ^ FVector3f::UpVector) : (Axis ^ FVector3f::ForwardVector)).GetSafeNormal();
		const FVector3f Up = (Side ^ Axis).GetSafeNormal();
		const float HalfWidth = FMath::Max(6.f * Params.Scale, 0.4f);
		const FVector4f VertexColor = ToVertexColor(GetAlbedo(Palette, Line.Color, ELBAPolyType::Solid));
		const FVector3f Offsets[4] = { Side * HalfWidth, Up * HalfWidth, -Side * HalfWidth, -Up * HalfWidth };
		for (int32 Face = 0; Face < 4; ++Face)
		{
			const FVector3f O0 = Offsets[Face];
			const FVector3f O1 = Offsets[(Face + 1) % 4];
			FLBAVertex A, B, C, D;
			A.Position = P0 + O0; A.Bone = Bone(Line.Points[0]);
			B.Position = P1 + O0; B.Bone = Bone(Line.Points[1]);
			C.Position = P1 + O1; C.Bone = Bone(Line.Points[1]);
			D.Position = P0 + O1; D.Bone = Bone(Line.Points[0]);
			const FVector3f Outward = (O0 + O1).GetSafeNormal();
			for (FLBAVertex* V : { &A, &B, &C, &D })
			{
				V->Normal = Outward;
				V->Color = VertexColor;
			}
			Out.AddTriangleFacing(ColorSection, A, B, C, Outward);
			Out.AddTriangleFacing(ColorSection, A, C, D, Outward);
		}
	}

	return !Out.IsEmpty();
}
