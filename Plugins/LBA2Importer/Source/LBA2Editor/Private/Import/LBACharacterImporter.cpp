#include "Import/LBACharacterImporter.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Import/LBAEnvironment.h"
#include "AnimationBlueprintLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture2D.h"
#include "Import/LBAAudioImporter.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBAAnimInstance.h"
#include "LBAAnimNotifies.h"
#include "LBAEntityAsset.h"
#include "LBATypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "StaticToSkeletalMeshConverter.h"

#define LOCTEXT_NAMESPACE "LBACharacterImporter"

namespace
{
	constexpr int32 AnimFrameRate = 30;

	FName BoneName(int32 Index)
	{
		return Index == 0 ? FName(TEXT("root")) : FName(*FString::Printf(TEXT("bone_%02d"), Index));
	}

	FString AnimTypeName(int32 Type)
	{
		const UEnum* Enum = StaticEnum<ELBAAnimType>();
		if (Enum->IsValidEnumValue(Type))
		{
			return Enum->GetNameStringByValue(Type);
		}
		return FString::Printf(TEXT("Type%d"), Type);
	}

	/**
	 * Bodies with a glowing part that gives light (FLBADecorLight: Body, GlowColor, Light...): the spacecraft's blue
	 * roof (bodies 70-78, its states), a subtle beacon.
	 */
	const FLBADecorLight* FindGlowingBody(int32 BodyIndex)
	{
		static const FLBADecorLight SpacecraftRoof = { 0, 192, 0.15f, 1000.f, 0.f, false, 2.f };
		return BodyIndex >= 70 && BodyIndex <= 78 ? &SpacecraftRoof : nullptr;
	}

	/** LBA yaw rotation of a local vector (see RotateI in LIB386/3D). */
	FVector RotateLba(const FVector& V, double AngleUnits)
	{
		const double A = AngleUnits * 2.0 * UE_DOUBLE_PI / LBA::AngleFullTurn;
		const double C = FMath::Cos(A), S = FMath::Sin(A);
		return FVector(V.X * C + V.Z * S, V.Y, -V.X * S + V.Z * C);
	}
}

FLBACharacterImporter::FLBACharacterImporter(FLBAImportContext& InContext, FLBAAudioImporter& InAudio)
	: Context(InContext)
	, Audio(InAudio)
{
}

const FLBABody* FLBACharacterImporter::GetBody(int32 BodyIndex)
{
	if (TUniquePtr<FLBABody>* Found = Bodies.Find(BodyIndex))
	{
		return Found->Get();
	}
	TUniquePtr<FLBABody> Body = MakeUnique<FLBABody>();
	if (!BodyHqr || !Body->Parse(BodyHqr->GetEntry(BodyIndex)) || Body->Groups.Num() == 0)
	{
		Body.Reset();
	}
	return Bodies.Add(BodyIndex, MoveTemp(Body)).Get();
}

const FLBAAnim* FLBACharacterImporter::GetAnim(int32 AnimIndex)
{
	if (TUniquePtr<FLBAAnim>* Found = Anims.Find(AnimIndex))
	{
		return Found->Get();
	}
	TUniquePtr<FLBAAnim> Anim = MakeUnique<FLBAAnim>();
	if (!AnimHqr || !Anim->Parse(AnimHqr->GetEntry(AnimIndex)))
	{
		Anim.Reset();
	}
	return Anims.Add(AnimIndex, MoveTemp(Anim)).Get();
}

FString FLBACharacterImporter::GetEntityName(int32 EntityIndex) const
{
	FString Name = Context.GetCommunityName(TEXT("entities"), EntityIndex);
	Name.ReplaceInline(TEXT("_"), TEXT(" "));
	Name = FLBAImportContext::SanitizeName(Name);
	return Name.IsEmpty() ? FString::Printf(TEXT("Entity%03d"), EntityIndex) : Name;
}

UMaterialInterface* FLBACharacterImporter::GetBodyTexturedMaterial(const FLBATextureRegion& Region, bool bMasked)
{
	UTexture2D*& Texture = RegionTextures.FindOrAdd(Region.GetKey());
	if (!Texture)
	{
		const TArray<uint8>& Page = TexturePage;
		Texture = Context.CreatePaletteTexture(Context.GetContentPath(TEXT("Characters/Textures")), TEXT("T_Body_") + Region.GetKey(),
			Region.Width, Region.Height,
			[&Page, Region](int32 X, int32 Y) { return Page[((Region.Y + Y) & 255) * 256 + ((Region.X + X) & 255)]; }, true, true);
	}
	return Context.GetMaterials().GetTextured(bMasked ? ELBAMaterial::TexturedMasked : ELBAMaterial::Textured, Texture,
		Context.GetContentPath(TEXT("Characters/Materials")));
}

void FLBACharacterImporter::AssignBoneNames(int32 BodyIndex, int32 SkeletonIndex)
{
	const FLBABody* Body = GetBody(BodyIndex);
	FSkeletonInfo& Info = Skeletons[SkeletonIndex];
	TArray<FName>& Names = BodyBoneNames.Add(BodyIndex);
	for (int32 Index = 0; Index < Body->Groups.Num(); ++Index)
	{
		const FLBABodyGroup& Group = Body->Groups[Index];
		FName Name = BoneName(Index);
		if (Index > 0)
		{
			const FName Parent = Names[FMath::Clamp(Group.Parent, 0, Index - 1)];
			// Same index under a different parent: use a name derived from the parent so it is stable
			// across import runs (a given bone name always implies the same parent).
			const FName* Existing = Info.ParentOf.Find(Name);
			if (Existing && *Existing != Parent)
			{
				const uint32 Hash = GetTypeHash(Parent.ToString().ToLower()) & 0xFFFF;
				Name = FName(*FString::Printf(TEXT("%s_%04X"), *BoneName(Index).ToString(), Hash));
			}
			if (!Info.ParentOf.Contains(Name))
			{
				Info.ParentOf.Add(Name, Parent);
				Info.Pivots.Add(Name, Body->Points.IsValidIndex(Group.PivotPoint) ? Body->Points[Group.PivotPoint].Position : FIntVector::ZeroValue);
			}
		}
		else
		{
			Info.Pivots.FindOrAdd(Name, FIntVector::ZeroValue);
		}
		Names.Add(Name);
		if (Info.NamesByIndex.Num() <= Index)
		{
			Info.NamesByIndex.SetNum(Index + 1);
		}
		Info.NamesByIndex[Index].AddUnique(Name);
	}
}

UMaterialInterface* FLBACharacterImporter::GetBodyGlowMaterial()
{
	UMaterialInterface* Master = Context.GetMaterials().GetMaster(ELBAMaterial::Glow);
	if (!BodyGlowMaterial)
	{
		// Half M_LBA_Glow's strength: a subtle glow (the spacecraft's roof showed as vivid cyan at 3).
		BodyGlowMaterial = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(Context.GetContentPath(TEXT("Characters/Materials")), TEXT("MI_Body_Glow"));
		if (!BodyGlowMaterial)
		{
			return Master;
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(BodyGlowMaterial, Master);
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(BodyGlowMaterial, TEXT("GlowStrength"), 1.5f);
		UMaterialEditingLibrary::UpdateMaterialInstance(BodyGlowMaterial);
		Context.MarkForSave(BodyGlowMaterial);
	}
	return BodyGlowMaterial;
}

USkeletalMesh* FLBACharacterImporter::ImportBody(int32 BodyIndex, int32 SkeletonIndex, const FString& Name)
{
	const FLBABody* Body = GetBody(BodyIndex);
	FSkeletonInfo& SkeletonInfo = Skeletons[SkeletonIndex];
	USkeleton* Skeleton = SkeletonInfo.Skeleton;
	if (!Body || !Skeleton)
	{
		return nullptr;
	}
	const float Scale = Context.GetScale();

	FReferenceSkeleton RefSkeleton;
	{
		const TArray<FName>& Names = BodyBoneNames.FindChecked(BodyIndex);
		FReferenceSkeletonModifier Modifier(RefSkeleton, nullptr);
		for (int32 Index = 0; Index < Body->Groups.Num(); ++Index)
		{
			const FLBABodyGroup& Group = Body->Groups[Index];
			const FName Bone = Names[Index];
			const int32 Parent = Index == 0 ? INDEX_NONE : FMath::Clamp(Group.Parent, 0, Index - 1);
			const FIntVector Pivot = Index > 0 && Body->Points.IsValidIndex(Group.PivotPoint) ? Body->Points[Group.PivotPoint].Position : FIntVector::ZeroValue;
			Modifier.Add(FMeshBoneInfo(Bone, Bone.ToString(), Parent), FTransform(LBA::ToUnreal(Pivot.X, Pivot.Y, Pivot.Z, Scale)));
		}
	}

	FLBABodyConvertParams Params;
	Params.Scale = Scale;
	Params.Palette = &Context.GetPalette();
	Params.bSkinned = true;
	Params.DebugIndex = BodyIndex;
	Params.ColorMaterial = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
	Params.TranslucentMaterial = Context.GetMaterials().GetMaster(ELBAMaterial::Translucent);
	Params.GetTexturedMaterial = [this](const FLBATextureRegion& Region, bool bMasked) { return GetBodyTexturedMaterial(Region, bMasked); };
	const FLBADecorLight* Glowing = FindGlowingBody(BodyIndex);
	if (Glowing)
	{
		Params.GlowColors.Add(Glowing->GlowColor);
		Params.GlowMaterial = GetBodyGlowMaterial();
	}
	FLBAMeshBuilder Builder;
	FBox3f Glow(ForceInit);
	if (!LBABodyConverter::Convert(*Body, Params, Builder, &Glow))
	{
		return nullptr;
	}
	BodyLights.Remove(BodyIndex);
	if (Glowing && Glow.IsValid)
	{
		// The light has the glowing part's color at full brightness, like the island lamps.
		const FLinearColor GlowColor = FLinearColor::FromSRGBColor(LBABodyConverter::GetAlbedo(Context.GetPalette(), Glowing->GlowColor, ELBAPolyType::Gouraud));
		FLBABodyLight& Light = BodyLights.Add(BodyIndex);
		Light.Location = FVector(Glow.GetCenter());
		Light.Color = GlowColor / FMath::Max(FMath::Max3(GlowColor.R, GlowColor.G, GlowColor.B), 1e-4f);
		Light.Intensity = LBAEnvironment::GetDecorLightIntensity(*Glowing);
		Light.Radius = Glowing->Radius;
		Light.SourceRadius = Glow.GetExtent().GetMin();
		Light.FalloffExponent = Glowing->FalloffExponent;
	}
	else if (Glowing)
	{
		Context.Warn(FString::Printf(TEXT("Body %d has no polygon of color %d to glow"), BodyIndex, Glowing->GlowColor));
	}
	FMeshDescription Description;
	TArray<UMaterialInterface*> Materials;
	TArray<FName> Slots;
	Builder.BuildSkeletal(Description, Materials, Slots);

	const FString Folder = Context.GetContentPath(TEXT("Characters/Meshes"));
	bool bExisted = false;
	USkeletalMesh* Mesh = Context.CreateOrReuseAsset<USkeletalMesh>(Folder, Name, &bExisted);
	if (!Mesh)
	{
		return nullptr;
	}
	if (bExisted)
	{
		// Same reset as UE::AssetUtils::CreateSkeletalMeshAsset before re-initializing an existing mesh.
		Mesh->PreEditChange(nullptr);
		FSkeletalMeshModel* Imported = Mesh->GetImportedModel();
		Imported->EmptyOriginalReductionSourceMeshData();
		Imported->LODModels.Empty();
		Imported->InlineReductionCacheDatas.Empty();
		Mesh->SetNumSourceModels(0);
		Mesh->GetMaterials().Empty();
		Mesh->GetRefSkeleton().Empty();
		Mesh->SetSkeleton(nullptr);
		Mesh->ReleaseResources();
		Mesh->InvalidateDeriveDataCacheGUID();
	}

	TArray<FSkeletalMaterial> SkeletalMaterials;
	for (int32 Index = 0; Index < Materials.Num(); ++Index)
	{
		SkeletalMaterials.Add(FSkeletalMaterial(Materials[Index], Slots[Index], Slots[Index]));
	}
	FStaticToSkeletalMeshConverter::FInitializationParams InitParams;
	InitParams.Materials = SkeletalMaterials;
	InitParams.bRecomputeNormals = false;
	InitParams.bRecomputeTangents = true;
	const FMeshDescription* Descriptions[] = { &Description };
	if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(Mesh, Descriptions, RefSkeleton, InitParams))
	{
		Context.Warn(FString::Printf(TEXT("Body %d: skeletal mesh creation failed"), BodyIndex));
		return nullptr;
	}

	Mesh->SetSkeleton(Skeleton);
	if (!Skeleton->MergeAllBonesToBoneTree(Mesh))
	{
		Context.Warn(FString::Printf(TEXT("Body %d does not fit skeleton %s"), BodyIndex, *Skeleton->GetName()));
	}
	if (!Skeleton->GetPreviewMesh())
	{
		Skeleton->SetPreviewMesh(Mesh);
	}
	// Bodies sharing a skeleton have slightly different proportions: let animations keep each mesh's own pivots.
	for (int32 Bone = 1; Bone < Skeleton->GetReferenceSkeleton().GetNum(); ++Bone)
	{
		Skeleton->SetBoneTranslationRetargetingMode(Bone, EBoneTranslationRetargetingMode::AnimationRelative);
	}
	Context.MarkForSave(Mesh);
	Context.MarkForSave(Skeleton);
	return Mesh;
}

UAnimSequence* FLBACharacterImporter::ImportAnim(int32 AnimIndex, int32 SkeletonIndex, const FString& Name, const TArray<FLBAAnimAction>& Actions)
{
	const TPair<int32, int32> Key(AnimIndex, SkeletonIndex);
	if (UAnimSequence** Found = AnimAssets.Find(Key))
	{
		return *Found;
	}
	UAnimSequence*& Result = AnimAssets.Add(Key, nullptr);

	const FLBAAnim* Anim = GetAnim(AnimIndex);
	const FSkeletonInfo& SkeletonInfo = Skeletons[SkeletonIndex];
	if (!Anim || !SkeletonInfo.Skeleton || SkeletonInfo.Skeleton->GetReferenceSkeleton().GetNum() == 0)
	{
		return nullptr;
	}
	const float Scale = Context.GetScale();
	const int32 NumKeys = Anim->Keyframes.Num();
	const int32 TotalMs = FMath::Max(Anim->GetTotalDurationMs(), 1);
	const int32 NumFrames = FMath::Max(1, FMath::RoundToInt(TotalMs * AnimFrameRate / 1000.0));
	const int32 NumBones = SkeletonInfo.NamesByIndex.Num();

	TArray<int32> KeyStartMs;
	{
		int32 Accumulated = 0;
		for (const FLBAAnimKeyframe& Keyframe : Anim->Keyframes)
		{
			KeyStartMs.Add(Accumulated);
			Accumulated += Keyframe.DurationMs;
		}
	}
	auto Locate = [&](double TimeMs, int32& OutKey, int32& OutNext, double& OutAlpha)
	{
		OutKey = NumKeys - 1;
		for (int32 K = 0; K < NumKeys; ++K)
		{
			if (TimeMs < KeyStartMs[K] + Anim->Keyframes[K].DurationMs)
			{
				OutKey = K;
				break;
			}
		}
		OutNext = OutKey + 1 < NumKeys ? OutKey + 1 : Anim->LoopFrame;
		const int32 Duration = Anim->Keyframes[OutKey].DurationMs;
		OutAlpha = Duration > 0 ? FMath::Clamp((TimeMs - KeyStartMs[OutKey]) / Duration, 0.0, 1.0) : 0.0;
	};

	// Root motion: steps and group-0 beta are velocities interpolated across each keyframe (in the actor's frame).
	TArray<FVector> RootPositions;
	TArray<double> RootYaws;
	{
		FVector Position = FVector::ZeroVector;
		double Yaw = 0.0;
		int32 NextSample = 0;
		const double SampleStepMs = 1000.0 / AnimFrameRate;
		for (int32 Ms = 0; Ms <= TotalMs && NextSample <= NumFrames; ++Ms)
		{
			while (NextSample <= NumFrames && NextSample * SampleStepMs <= Ms)
			{
				RootPositions.Add(Position);
				RootYaws.Add(Yaw);
				++NextSample;
			}
			int32 K, Next;
			double Alpha;
			Locate(Ms + 0.5, K, Next, Alpha);
			const FLBAAnimKeyframe& A = Anim->Keyframes[K];
			const FLBAAnimKeyframe& B = Anim->Keyframes[Next];
			const double Duration = FMath::Max(A.DurationMs, 1);
			const FVector Step = FMath::Lerp(FVector(A.Step), FVector(B.Step), Alpha) / Duration;
			const double YawStep = FMath::Lerp(double(A.Groups[0].Beta), double(B.Groups[0].Beta), Alpha) / Duration;
			Position += RotateLba(Step, Yaw);
			Yaw += YawStep;
		}
		while (RootPositions.Num() <= NumFrames)
		{
			RootPositions.Add(Position);
			RootYaws.Add(Yaw);
		}
	}

	const FString Folder = Context.GetContentPath(TEXT("Characters/Anims") / SkeletonInfo.Name);
	UAnimSequence* Sequence = Context.CreateOrReuseAsset<UAnimSequence>(Folder, Name);
	if (!Sequence)
	{
		return nullptr;
	}
	Sequence->SetSkeleton(SkeletonInfo.Skeleton);
	Sequence->Notifies.Reset();
	Sequence->AnimNotifyTracks.Reset();

	IAnimationDataController& Controller = Sequence->GetController();
	Controller.OpenBracket(LOCTEXT("ImportAnim", "Import LBA animation"), false);
	Controller.InitializeModel();
	Controller.RemoveAllBoneTracks(false);
	Controller.SetFrameRate(FFrameRate(AnimFrameRate, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(NumFrames), false);

	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	for (const FName& Bone_ : SkeletonInfo.NamesByIndex[Bone])
	{
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;
		const FIntVector& Pivot = SkeletonInfo.Pivots.FindChecked(Bone_);
		const FVector PivotUE = LBA::ToUnreal(Pivot.X, Pivot.Y, Pivot.Z, Scale);
		for (int32 Frame = 0; Frame <= NumFrames; ++Frame)
		{
			if (Bone == 0)
			{
				Positions.Add(LBA::ToUnreal(RootPositions[Frame], Scale));
				Rotations.Add(FQuat(FVector::UpVector, -RootYaws[Frame] * 2.0 * UE_DOUBLE_PI / LBA::AngleFullTurn));
				Scales.Add(FVector::OneVector);
				continue;
			}
			int32 K, Next;
			double Alpha;
			Locate(Frame * 1000.0 / AnimFrameRate, K, Next, Alpha);
			FVector Position = PivotUE;
			FQuat Rotation = FQuat::Identity;
			if (Bone < Anim->NumGroups)
			{
				const FLBAAnimGroupFrame& A = Anim->Keyframes[K].Groups[Bone];
				const FLBAAnimGroupFrame& B = Anim->Keyframes[Next].Groups[Bone];
				if (A.Type == 0)
				{
					Rotation = FQuat::Slerp(LBA::BoneAnglesToUnreal(A.Alpha, A.Beta, A.Gamma), LBA::BoneAnglesToUnreal(B.Alpha, B.Beta, B.Gamma), Alpha).GetNormalized();
				}
				else
				{
					const FVector Offset = FMath::Lerp(FVector(A.Alpha, A.Beta, A.Gamma), FVector(B.Alpha, B.Beta, B.Gamma), Alpha);
					Position += LBA::ToUnreal(Offset, Scale);
				}
			}
			Positions.Add(Position);
			Rotations.Add(Rotation);
			Scales.Add(FVector::OneVector);
		}
		Controller.AddBoneCurve(Bone_, false);
		Controller.SetBoneTrackKeys(Bone_, Positions, Rotations, Scales, false);
	}
	Controller.NotifyPopulated();
	Controller.CloseBracket(false);

	Sequence->bEnableRootMotion = true;
	Sequence->RootMotionRootLock = ERootMotionRootLock::RefPose;

	// Key times, for smooth (cubic) interpolation of the keys at runtime (ULBAAnimInstance::SetSmoothKeyframes).
	ULBAAnimKeyframes* Keyframes = Sequence->FindMetaDataByClass<ULBAAnimKeyframes>();
	if (!Keyframes)
	{
		Keyframes = NewObject<ULBAAnimKeyframes>(Sequence, NAME_None, RF_Transactional);
		Sequence->AddMetaData(Keyframes);
	}
	Keyframes->KeyTimes.Reset();
	for (const int32 StartMs : KeyStartMs)
	{
		Keyframes->KeyTimes.Add(StartMs / 1000.f);
	}
	Keyframes->LoopKey = Anim->LoopFrame;

	// Gameplay actions become notifies on an "LBA" track.
	if (Actions.Num() > 0)
	{
		const FName Track(TEXT("LBA"));
		UAnimationBlueprintLibrary::AddAnimationNotifyTrack(Sequence, Track, FLinearColor(0.9f, 0.6f, 0.1f));
		for (const FLBAAnimAction& Action : Actions)
		{
			const float Time = KeyStartMs.IsValidIndex(Action.Frame) ? KeyStartMs[Action.Frame] / 1000.f : 0.f;
			switch (Action.Type)
			{
			case ELBAAnimActionType::LeftStep:
			case ELBAAnimActionType::RightStep:
				if (ULBAAnimNotify_Footstep* Notify = Cast<ULBAAnimNotify_Footstep>(UAnimationBlueprintLibrary::AddAnimationNotifyEvent(Sequence, Track, Time, ULBAAnimNotify_Footstep::StaticClass())))
				{
					Notify->Foot = Action.Type == ELBAAnimActionType::LeftStep ? ELBAFoot::Left : ELBAFoot::Right;
				}
				break;
			case ELBAAnimActionType::Hit:
			case ELBAAnimActionType::SuperHit:
			case ELBAAnimActionType::HitHero:
				if (ULBAAnimNotify_Hit* Notify = Cast<ULBAAnimNotify_Hit>(UAnimationBlueprintLibrary::AddAnimationNotifyEvent(Sequence, Track, Time, ULBAAnimNotify_Hit::StaticClass())))
				{
					Notify->Strength = Action.Params.Num() > 0 ? Action.Params[0] : 0;
					Notify->bSuperHit = Action.Type == ELBAAnimActionType::SuperHit;
				}
				break;
			case ELBAAnimActionType::Sample:
			case ELBAAnimActionType::SampleRnd:
			case ELBAAnimActionType::SampleRepeat:
			case ELBAAnimActionType::NewSample:
				if (USoundWave* Sound = Audio.GetSample(Action.GetU16(0)))
				{
					if (UAnimNotify_PlaySound* Notify = Cast<UAnimNotify_PlaySound>(UAnimationBlueprintLibrary::AddAnimationNotifyEvent(Sequence, Track, Time, UAnimNotify_PlaySound::StaticClass())))
					{
						Notify->Sound = Sound;
						Notify->bFollow = true;
					}
				}
				break;
			case ELBAAnimActionType::ZV:
			case ELBAAnimActionType::SampleStop:
				break;
			default:
				if (ULBAAnimNotify_Action* Notify = Cast<ULBAAnimNotify_Action>(UAnimationBlueprintLibrary::AddAnimationNotifyEvent(Sequence, Track, Time, ULBAAnimNotify_Action::StaticClass())))
				{
					Notify->ActionType = Action.Type;
					Notify->ActionName = FLBAAnimAction::GetTypeName(Action.Type);
					Notify->Parameters = Action.Params;
				}
				break;
			}
		}
	}

	Sequence->PostEditChange();
	Context.MarkForSave(Sequence);
	Result = Sequence;
	return Sequence;
}

int32 FLBACharacterImporter::ImportAll()
{
	BodyHqr = Context.GetHqr(TEXT("BODY.HQR"));
	AnimHqr = Context.GetHqr(TEXT("ANIM.HQR"));
	const FLBAHqrFile* Ress = Context.GetHqr(TEXT("RESS.HQR"));
	if (!BodyHqr || !AnimHqr || !Ress)
	{
		Context.Warn(TEXT("BODY.HQR, ANIM.HQR or RESS.HQR is missing"));
		return 0;
	}
	TexturePage = Ress->GetEntry(6);
	TexturePage.SetNumZeroed(256 * 256);
	const TArray<FLBAEntity> Entities = FLBAEntity::ParseAll(Ress->GetEntry(44));
	const TArray<int32>& Filter = Context.GetSettings().EntityFilter;

	// Bodies, largest hierarchies first so they define the shared skeletons.
	TArray<int32> BodyOrder;
	TMap<int32, FString> BodyOwner;
	for (const FLBAEntity& Entity : Entities)
	{
		if (Filter.Num() > 0 && !Filter.Contains(Entity.Index))
		{
			continue;
		}
		for (const FLBAEntityBodyEntry& Body : Entity.Bodies)
		{
			if (Body.BodyIndex >= 0 && !BodyOwner.Contains(Body.BodyIndex) && GetBody(Body.BodyIndex))
			{
				BodyOwner.Add(Body.BodyIndex, GetEntityName(Entity.Index));
				BodyOrder.Add(Body.BodyIndex);
			}
		}
	}
	// Lowest body index first: it defines the base bone names of its family, independently of filters.
	BodyOrder.Sort();

	// Families: bodies used by a same entity share a skeleton (union-find over entities).
	TMap<int32, int32> FamilyOf;
	TFunction<int32(int32)> Find = [&](int32 Body) -> int32
	{
		int32& Parent = FamilyOf.FindOrAdd(Body, Body);
		if (Parent != Body)
		{
			Parent = Find(Parent);
		}
		return Parent;
	};
	for (const FLBAEntity& Entity : Entities)
	{
		int32 First = INDEX_NONE;
		for (const FLBAEntityBodyEntry& Body : Entity.Bodies)
		{
			if (!BodyOwner.Contains(Body.BodyIndex))
			{
				continue;
			}
			if (First == INDEX_NONE)
			{
				First = Body.BodyIndex;
				Find(First);
			}
			else
			{
				FamilyOf[Find(Body.BodyIndex)] = Find(First);
			}
		}
	}

	Context.ReportProgress(FText::Format(LOCTEXT("Bodies", "Importing {0} character bodies"), BodyOrder.Num()));
	TMap<int32, int32> SkeletonOfFamily;
	for (int32 BodyIndex : BodyOrder)
	{
		const int32 Family = Find(BodyIndex);
		int32 SkeletonIndex;
		if (const int32* Existing = SkeletonOfFamily.Find(Family))
		{
			SkeletonIndex = *Existing;
		}
		else
		{
			FSkeletonInfo& Info = Skeletons.AddDefaulted_GetRef();
			Info.Name = BodyOwner[BodyIndex];
			Info.Skeleton = Context.CreateOrReuseAsset<USkeleton>(Context.GetContentPath(TEXT("Characters/Skeletons")), TEXT("SKEL_") + Info.Name);
			SkeletonIndex = Skeletons.Num() - 1;
			SkeletonOfFamily.Add(Family, SkeletonIndex);
		}
		AssignBoneNames(BodyIndex, SkeletonIndex);
		BodySkeleton.Add(BodyIndex, SkeletonIndex);
		const FString Name = FString::Printf(TEXT("SK_%s_Body%03d"), *BodyOwner[BodyIndex], BodyIndex);
		if (USkeletalMesh* Mesh = ImportBody(BodyIndex, SkeletonIndex, Name))
		{
			BodyMeshes.Add(BodyIndex, Mesh);
		}
	}
	Context.Flush();

	int32 Count = 0;
	const float Scale = Context.GetScale();
	TArray<ULBAEntityAsset*> ImportedEntities;
	for (const FLBAEntity& Entity : Entities)
	{
		if ((Filter.Num() > 0 && !Filter.Contains(Entity.Index)) || Context.IsCancelled())
		{
			continue;
		}
		const FString EntityName = GetEntityName(Entity.Index);
		Context.ReportProgress(FText::Format(LOCTEXT("Entity", "Importing entity {0}"), FText::FromString(EntityName)));

		int32 PrimarySkeleton = INDEX_NONE;
		TArray<int32> EntitySkeletons;
		for (const FLBAEntityBodyEntry& Body : Entity.Bodies)
		{
			if (const int32* Skeleton = BodySkeleton.Find(Body.BodyIndex))
			{
				EntitySkeletons.AddUnique(*Skeleton);
				if (PrimarySkeleton == INDEX_NONE)
				{
					PrimarySkeleton = *Skeleton;
				}
			}
		}
		if (PrimarySkeleton == INDEX_NONE)
		{
			continue;
		}

		ULBAEntityAsset* Asset = Context.CreateOrReuseAsset<ULBAEntityAsset>(Context.GetContentPath(TEXT("Characters/Entities")), TEXT("DA_") + EntityName);
		if (!Asset)
		{
			continue;
		}
		Asset->DisplayName = Context.GetCommunityName(TEXT("entities"), Entity.Index);
		Asset->EntityIndex = Entity.Index;
		Asset->Bodies.Reset();
		Asset->Anims.Reset();

		for (const FLBAEntityBodyEntry& Body : Entity.Bodies)
		{
			FLBAEntityBody& Out = Asset->Bodies.FindOrAdd(Body.Type);
			Out.Mesh = BodyMeshes.FindRef(Body.BodyIndex);
			Out.SourceBody = Body.BodyIndex;
			Out.Lights.Reset();
			if (const FLBABodyLight* Light = BodyLights.Find(Body.BodyIndex))
			{
				Out.Lights.Add(*Light);
			}
			if (Body.bHasBox)
			{
				const FVector A = LBA::ToUnreal(Body.BoxMin.X, Body.BoxMin.Y, Body.BoxMin.Z, Scale);
				const FVector B = LBA::ToUnreal(Body.BoxMax.X, Body.BoxMax.Y, Body.BoxMax.Z, Scale);
				Out.CollisionBox = FBox(A.ComponentMin(B), A.ComponentMax(B));
			}
		}

		for (const FLBAEntityAnimEntry& AnimEntry : Entity.Anims)
		{
			if (AnimEntry.AnimIndex < 0)
			{
				continue;
			}
			const FString AnimName = FString::Printf(TEXT("A_%04d_%s"), AnimEntry.AnimIndex, *AnimTypeName(AnimEntry.Type));
			UAnimSequence* Primary = nullptr;
			for (int32 Skeleton : EntitySkeletons)
			{
				UAnimSequence* Sequence = ImportAnim(AnimEntry.AnimIndex, Skeleton, AnimName, AnimEntry.Actions);
				if (Skeleton == PrimarySkeleton)
				{
					Primary = Sequence;
				}
			}
			FLBAEntityAnim& Out = Asset->Anims.FindOrAdd(AnimEntry.Type);
			Out.Sequence = Primary;
			Out.SourceAnim = AnimEntry.AnimIndex;
			if (const FLBAAnim* Anim = GetAnim(AnimEntry.AnimIndex))
			{
				int32 LoopMs = 0;
				for (int32 K = 0; K < Anim->LoopFrame; ++K)
				{
					LoopMs += Anim->Keyframes[K].DurationMs;
				}
				Out.LoopStartTime = LoopMs / 1000.f;
			}
		}
		Context.MarkForSave(Asset);
		ImportedEntities.Add(Asset);
		++Count;
		if (Count % 20 == 0)
		{
			Context.Flush();
		}
	}
	Context.Flush();
	BuildGalleryLevel(ImportedEntities);
	return Count;
}

void FLBACharacterImporter::BuildGalleryLevel(const TArray<ULBAEntityAsset*>& Entities)
{
	UWorld* World = Context.BeginLevel(Context.GetContentPath(TEXT("Characters")), TEXT("L_CharacterGallery"));
	if (!World)
	{
		return;
	}
	auto SpawnCharacter = [&](USkeletalMesh* Mesh, UAnimSequence* Anim, const FVector& Location, const FString& Label, const FName& Folder)
	{
		if (!Mesh)
		{
			return;
		}
		// Meshes face +Y (LBA forward); turn them to face the -X viewer side.
		ASkeletalMeshActor* Actor = Context.SpawnGeneratedActor<ASkeletalMeshActor>(World, FTransform(FRotator(0, 90, 0), Location), Label, Folder);
		if (!Actor)
		{
			return;
		}
		USkeletalMeshComponent* Component = Actor->GetSkeletalMeshComponent();
		Component->SetSkeletalMesh(Mesh);
		if (Anim)
		{
			Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			Component->AnimationData.AnimToPlay = Anim;
			Component->AnimationData.bSavedLooping = true;
			Component->AnimationData.bSavedPlaying = true;
		}
		if (ATextRenderActor* Text = Context.SpawnGeneratedActor<ATextRenderActor>(World, FTransform(FRotator(0, 180, 0), Location + FVector(-40, 0, 230)), Label + TEXT("_Label"), Folder))
		{
			Text->GetTextRender()->SetText(FText::FromString(Label));
			Text->GetTextRender()->SetHorizontalAlignment(EHTA_Center);
			Text->GetTextRender()->SetWorldSize(18.f);
			Text->GetTextRender()->SetTextRenderColor(FColor::White);
		}
	};

	constexpr double Spacing = 250.0;
	const int32 PerRow = 10;
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const ULBAEntityAsset* Entity = Entities[Index];
		const FVector Location(Spacing * (Index / PerRow), Spacing * (Index % PerRow), 0.0);
		SpawnCharacter(Entity->GetDefaultBodyMesh(), Entity->GetAnim(ELBAAnimType::Idle), Location, Entity->GetName().RightChop(3), TEXT("Entities"));
	}

	// Twinsen showcase: one actor per well-known animation slot of the first entity.
	if (Entities.Num() > 0)
	{
		const ULBAEntityAsset* Hero = Entities[0];
		int32 Column = 0;
		for (const TPair<int32, FLBAEntityAnim>& Pair : Hero->Anims)
		{
			if (!Pair.Value.Sequence || !StaticEnum<ELBAAnimType>()->IsValidEnumValue(Pair.Key))
			{
				continue;
			}
			const FVector Location(-Spacing * (1 + Column / PerRow), Spacing * (Column % PerRow), 0.0);
			SpawnCharacter(Hero->GetDefaultBodyMesh(), Pair.Value.Sequence, Location, AnimTypeName(Pair.Key), TEXT("Animations"));
			++Column;
		}
	}

	if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
	{
		if (AStaticMeshActor* Floor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(FRotator::ZeroRotator, FVector(0, 1200, 0), FVector(60, 40, 1)), TEXT("Floor"), TEXT("Environment")))
		{
			Floor->GetStaticMeshComponent()->SetStaticMesh(Plane);
		}
	}
	// Citabau's light angles (alpha 356, beta 3411).
	LBAEnvironment::Spawn(Context, World, FLBAEnvironmentPreset::ForIsland(TEXT("CITABAU")), LBAEnvironment::SunRotationFromLbaAngles(356, 3411));
	Context.SpawnGeneratedActor<APlayerStart>(World, FTransform(FRotator(0, 0, 0), FVector(-2500, 1200, 120)), TEXT("PlayerStart"), NAME_None);
	Context.EndLevel(World);
}

#undef LOCTEXT_NAMESPACE
