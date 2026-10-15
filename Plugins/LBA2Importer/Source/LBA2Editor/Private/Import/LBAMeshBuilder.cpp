#include "Import/LBAMeshBuilder.h"

#include "BoneWeights.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshAttributes.h"

int32 FLBAMeshBuilder::GetSection(FName Slot, UMaterialInterface* Material)
{
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		if (Sections[Index].Slot == Slot)
		{
			return Index;
		}
	}
	FSection& Section = Sections.AddDefaulted_GetRef();
	Section.Slot = Slot;
	Section.Material = Material;
	return Sections.Num() - 1;
}

void FLBAMeshBuilder::AddTriangle(int32 Section, const FLBAVertex& A, const FLBAVertex& B, const FLBAVertex& C)
{
	TArray<FLBAVertex>& Vertices = Sections[Section].Vertices;
	Vertices.Add(A);
	Vertices.Add(B);
	Vertices.Add(C);
}

void FLBAMeshBuilder::AddTriangleFacing(int32 Section, const FLBAVertex& A, const FLBAVertex& B, const FLBAVertex& C, const FVector3f& Outward)
{
	if ((FrontNormal(A.Position, B.Position, C.Position) | Outward) < 0.f)
	{
		AddTriangle(Section, A, C, B);
	}
	else
	{
		AddTriangle(Section, A, B, C);
	}
}

bool FLBAMeshBuilder::IsEmpty() const
{
	return NumTriangles() == 0;
}

int32 FLBAMeshBuilder::NumTriangles() const
{
	int32 Count = 0;
	for (const FSection& Section : Sections)
	{
		Count += Section.Vertices.Num() / 3;
	}
	return Count;
}

FBox3f FLBAMeshBuilder::ComputeBounds() const
{
	FBox3f Box(ForceInit);
	for (const FSection& Section : Sections)
	{
		for (const FLBAVertex& Vertex : Section.Vertices)
		{
			Box += Vertex.Position;
		}
	}
	return Box;
}

void FLBAMeshBuilder::BuildStatic(FMeshDescription& Out, TArray<UMaterialInterface*>& OutMaterials, TArray<FName>& OutSlots) const
{
	FStaticMeshAttributes Attributes(Out);
	Attributes.Register();
	Build(Out, OutMaterials, OutSlots, false);
}

void FLBAMeshBuilder::BuildSkeletal(FMeshDescription& Out, TArray<UMaterialInterface*>& OutMaterials, TArray<FName>& OutSlots) const
{
	FSkeletalMeshAttributes Attributes(Out);
	Attributes.Register();
	Build(Out, OutMaterials, OutSlots, true);
}

void FLBAMeshBuilder::Build(FMeshDescription& Out, TArray<UMaterialInterface*>& OutMaterials, TArray<FName>& OutSlots, bool bSkinned) const
{
	FStaticMeshAttributes Attributes(Out);
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(1);

	TArray<TPair<FVertexID, int32>> VertexBones;

	int32 TotalVertices = 0;
	for (const FSection& Section : Sections)
	{
		TotalVertices += Section.Vertices.Num();
	}
	Out.ReserveNewVertexInstances(TotalVertices);
	Out.ReserveNewTriangles(TotalVertices / 3);

	// Weld corners that share a position (and bone) so smooth normals and collision stay connected.
	TMap<TPair<FVector3f, int32>, FVertexID> Welded;

	for (const FSection& Section : Sections)
	{
		if (Section.Vertices.Num() < 3)
		{
			continue;
		}
		const FPolygonGroupID Group = Out.CreatePolygonGroup();
		SlotNames[Group] = Section.Slot;
		OutMaterials.Add(Section.Material);
		OutSlots.Add(Section.Slot);

		for (int32 Corner = 0; Corner + 2 < Section.Vertices.Num(); Corner += 3)
		{
			FVertexInstanceID Instances[3];
			for (int32 K = 0; K < 3; ++K)
			{
				const FLBAVertex& V = Section.Vertices[Corner + K];
				const TPair<FVector3f, int32> Key(V.Position, bSkinned ? V.Bone : 0);
				FVertexID VertexID;
				if (const FVertexID* Found = Welded.Find(Key))
				{
					VertexID = *Found;
				}
				else
				{
					VertexID = Out.CreateVertex();
					Positions[VertexID] = V.Position;
					VertexBones.Emplace(VertexID, V.Bone);
					Welded.Add(Key, VertexID);
				}
				const FVertexInstanceID Instance = Out.CreateVertexInstance(VertexID);
				Normals[Instance] = V.Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
				UVs.Set(Instance, 0, V.UV);
				Colors[Instance] = V.Color;
				Instances[K] = Instance;
			}
			Out.CreateTriangle(Group, MakeArrayView(Instances, 3));
		}
	}

	if (bSkinned)
	{
		// LBA skinning is rigid: every vertex follows exactly one bone.
		FSkeletalMeshAttributes SkeletalAttributes(Out);
		FSkinWeightsVertexAttributesRef SkinWeights = SkeletalAttributes.GetVertexSkinWeights();
		for (const TPair<FVertexID, int32>& Pair : VertexBones)
		{
			const UE::AnimationCore::FBoneWeight Weight(FBoneIndexType(Pair.Value), 1.0f);
			SkinWeights.Set(Pair.Key, MakeArrayView(&Weight, 1));
		}
	}
}
