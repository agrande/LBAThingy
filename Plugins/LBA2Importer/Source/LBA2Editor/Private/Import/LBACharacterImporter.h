#pragma once

#include "CoreMinimal.h"
#include "Formats/LBAAnim.h"
#include "Formats/LBABody.h"
#include "LBAEntityAsset.h"

class FLBAImportContext;
class FLBAAudioImporter;
class FLBAHqrFile;
class UAnimSequence;
class USkeletalMesh;
class USkeleton;
class UTexture2D;
class UMaterialInterface;

/**
 * Characters: RESS.HQR entity table + BODY.HQR + ANIM.HQR ->
 *   Skeletons    one per bone hierarchy (bodies whose hierarchy extends another share its skeleton)
 *   Meshes       one skeletal mesh per body (rigid skinning, one bone per vertex as in LBA)
 *   Anims        one sequence per (animation, skeleton), root motion baked on the root bone,
 *                gameplay actions as notifies (footsteps, hits, sounds, throws)
 *   Entities     ULBAEntityAsset per entity mapping body/anim slots to the assets above
 * Output under <Root>/Characters/.
 */
class FLBACharacterImporter
{
public:
	FLBACharacterImporter(FLBAImportContext& InContext, FLBAAudioImporter& InAudio);

	int32 ImportAll();

private:
	/**
	 * One skeleton per character family (bodies used together by some entity). Bodies differ in their
	 * extra bones (weapons...), so a bone that hangs from a different parent gets a suffixed name and the
	 * skeleton is the union of every body's hierarchy.
	 */
	struct FSkeletonInfo
	{
		FString Name;
		TMap<FName, FName> ParentOf;
		/** Local translation (LBA units) of every bone, from the first body that declared it. */
		TMap<FName, FIntVector> Pivots;
		/** All bone names used for an LBA group index (animations address bones by index). */
		TArray<TArray<FName>> NamesByIndex;
		USkeleton* Skeleton = nullptr;
	};

	const FLBABody* GetBody(int32 BodyIndex);
	const FLBAAnim* GetAnim(int32 AnimIndex);
	void AssignBoneNames(int32 BodyIndex, int32 SkeletonIndex);
	USkeletalMesh* ImportBody(int32 BodyIndex, int32 SkeletonIndex, const FString& Name);
	UAnimSequence* ImportAnim(int32 AnimIndex, int32 SkeletonIndex, const FString& Name, const TArray<FLBAAnimAction>& Actions);
	UMaterialInterface* GetBodyTexturedMaterial(const FLBATextureRegion& Region, bool bMasked);
	/** Material of glowing body parts (see FindGlowingBody). */
	UMaterialInterface* GetBodyGlowMaterial();
	FString GetEntityName(int32 EntityIndex) const;
	void BuildGalleryLevel(const TArray<class ULBAEntityAsset*>& Entities);

	FLBAImportContext& Context;
	FLBAAudioImporter& Audio;
	const FLBAHqrFile* BodyHqr = nullptr;
	const FLBAHqrFile* AnimHqr = nullptr;
	TArray<uint8> TexturePage;
	TMap<FString, UTexture2D*> RegionTextures;

	TMap<int32, TUniquePtr<FLBABody>> Bodies;
	TMap<int32, TUniquePtr<FLBAAnim>> Anims;
	TArray<FSkeletonInfo> Skeletons;
	TMap<int32, int32> BodySkeleton;
	TMap<int32, TArray<FName>> BodyBoneNames;
	TMap<int32, USkeletalMesh*> BodyMeshes;
	/** Light of each body's glowing part (mesh space). */
	TMap<int32, FLBABodyLight> BodyLights;
	class UMaterialInstanceConstant* BodyGlowMaterial = nullptr;
	TMap<TPair<int32, int32>, UAnimSequence*> AnimAssets;
};
