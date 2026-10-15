#pragma once

#include "CoreMinimal.h"
#include "MeshDescription.h"

class UMaterialInterface;

struct FLBAVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	FVector3f Normal = FVector3f::UpVector;
	FVector2f UV = FVector2f::ZeroVector;
	/** Linear RGB albedo, alpha = baked light (0.5 = neutral). */
	FVector4f Color = FVector4f(1, 1, 1, 0.5f);
	int32 Bone = 0;
};

/**
 * Accumulates triangles grouped by material slot and turns them into a static or skeletal
 * FMeshDescription. Triangles must be given in Unreal winding (front normal = (C - A) x (B - A)).
 */
class FLBAMeshBuilder
{
public:
	int32 GetSection(FName Slot, UMaterialInterface* Material);
	void AddTriangle(int32 Section, const FLBAVertex& A, const FLBAVertex& B, const FLBAVertex& C);

	/** Adds a triangle, swapping two corners when needed so it faces Outward. */
	void AddTriangleFacing(int32 Section, const FLBAVertex& A, const FLBAVertex& B, const FLBAVertex& C, const FVector3f& Outward);

	bool IsEmpty() const;
	int32 NumTriangles() const;

	void BuildStatic(FMeshDescription& Out, TArray<UMaterialInterface*>& OutMaterials, TArray<FName>& OutSlots) const;
	void BuildSkeletal(FMeshDescription& Out, TArray<UMaterialInterface*>& OutMaterials, TArray<FName>& OutSlots) const;

	FBox3f ComputeBounds() const;

	/** Unreal's front face normal for a triangle (matches FStaticMeshOperations::ComputeTriangleNormals). */
	static FVector3f FrontNormal(const FVector3f& A, const FVector3f& B, const FVector3f& C)
	{
		return (C - A) ^ (B - A);
	}

private:
	void Build(FMeshDescription& Out, TArray<UMaterialInterface*>& OutMaterials, TArray<FName>& OutSlots, bool bSkinned) const;

	struct FSection
	{
		FName Slot;
		UMaterialInterface* Material = nullptr;
		TArray<FLBAVertex> Vertices;
	};
	TArray<FSection> Sections;
};
