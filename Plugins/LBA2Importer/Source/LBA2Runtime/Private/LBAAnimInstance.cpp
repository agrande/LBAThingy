#include "LBAAnimInstance.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/BoneReference.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "TwoBoneIK.h"

namespace
{
	const ULBAAnimKeyframes* FindKeyframes(const UAnimSequence* Sequence)
	{
		return Sequence ? Sequence->FindMetaDataByClass<ULBAAnimKeyframes>() : nullptr;
	}

	/** Tangent at P1 of a curve through P0, P1, P2 (times T0..T2): flat at extremes so holds and contacts stay put. */
	float CurveTangent(float P0, float P1, float P2, float T0, float T2)
	{
		if ((P1 - P0) * (P2 - P1) <= 0.f || T2 <= T0)
		{
			return 0.f;
		}
		return (P2 - P0) / (T2 - T0);
	}

	/** Monotone cubic Hermite between B (at TB) and C (at TC), with neighbours A and D. */
	float CubicKey(float A, float B, float C, float D, float TA, float TB, float TC, float TD, float S)
	{
		const float H = TC - TB;
		const float Delta = (C - B) / H;
		float MB = CurveTangent(A, B, C, TA, TC);
		float MC = CurveTangent(B, C, D, TB, TD);
		// Fritsch-Carlson: no overshoot between the two keys.
		if (Delta == 0.f)
		{
			MB = MC = 0.f;
		}
		else
		{
			const float Limit = 3.f * FMath::Abs(Delta);
			MB = MB * Delta < 0.f ? 0.f : FMath::Sign(Delta) * FMath::Min(FMath::Abs(MB), Limit);
			MC = MC * Delta < 0.f ? 0.f : FMath::Sign(Delta) * FMath::Min(FMath::Abs(MC), Limit);
		}
		const float S2 = S * S;
		const float S3 = S2 * S;
		return (2.f * S3 - 3.f * S2 + 1.f) * B + (S3 - 2.f * S2 + S) * H * MB + (-2.f * S3 + 3.f * S2) * C + (S3 - S2) * H * MC;
	}

	FCompactPoseBoneIndex FindBone(const FBoneContainer& Bones, FName Name)
	{
		FBoneReference Reference(Name);
		if (!Name.IsNone() && Reference.Initialize(Bones) && Reference.IsValidToEvaluate(Bones))
		{
			return Reference.GetCompactPoseIndex(Bones);
		}
		return FCompactPoseBoneIndex(INDEX_NONE);
	}

	/** Time of the second sequence of a synced blend, from the first one's time. */
	void UpdateSyncTime(FLBAAnimPlayback& Playback)
	{
		if (!Playback.Sequence || !Playback.SyncSequence)
		{
			return;
		}
		const float LoopLength = Playback.Sequence->GetPlayLength() - Playback.LoopStart;
		const float SyncLoopLength = Playback.SyncSequence->GetPlayLength() - Playback.SyncLoopStart;
		if (LoopLength <= 0.f || SyncLoopLength <= 0.f)
		{
			Playback.SyncTime = Playback.SyncLoopStart;
		}
		else if (Playback.Time >= Playback.LoopStart)
		{
			const float Phase = (Playback.Time - Playback.LoopStart) / LoopLength;
			Playback.SyncTime = Playback.SyncLoopStart + FMath::Frac(Phase + Playback.SyncPhaseOffset) * SyncLoopLength;
		}
		else
		{
			// Before the loops, both run through their lead-ins together.
			Playback.SyncTime = Playback.LoopStart > 0.f ? Playback.Time / Playback.LoopStart * Playback.SyncLoopStart : 0.f;
		}
	}
}

void ULBAAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	RootMotionMode = bApplyRootMotion ? ERootMotionMode::RootMotionFromEverything : ERootMotionMode::IgnoreRootMotion;
}

void ULBAAnimInstance::PlayLBAAnimation(UAnimSequence* Sequence, bool bLoop, float LoopStart, float BlendTime, float PlayRate, bool bRestart, float StartTime)
{
	const UAnimSequence* Active = bHasRequest ? Request.Sequence.Get() : CurrentSequence.Get();
	if (!bRestart && Sequence == Active && (bLoop || !bCurrentFinished))
	{
		PendingPlayRate = PlayRate;
		return;
	}
	Request = FLBAAnimPlayback();
	Request.Sequence = Sequence;
	Request.Keys = FindKeyframes(Sequence);
	Request.bLoop = bLoop;
	Request.LoopStart = LoopStart;
	Request.PlayRate = PlayRate;
	Request.Time = Sequence ? FMath::Clamp(StartTime, 0.f, Sequence->GetPlayLength()) : 0.f;
	RequestBlendTime = BlendTime;
	bHasRequest = true;
	// A new animation starts without a synced blend.
	bHasSyncRequest = false;
	CurrentSequence = Sequence;
	bCurrentFinished = false;
	CurrentTime = Request.Time;
}

void ULBAAnimInstance::SetSyncedBlend(UAnimSequence* Sequence, float LoopStart, float SyncWeight, float PhaseOffset)
{
	SyncRequest = FLBAAnimPlayback();
	if (Sequence && SyncWeight > 0.f)
	{
		SyncRequest.SyncSequence = Sequence;
		SyncRequest.SyncKeys = FindKeyframes(Sequence);
		SyncRequest.SyncLoopStart = LoopStart;
		SyncRequest.SyncWeight = FMath::Min(SyncWeight, 1.f);
		SyncRequest.SyncPhaseOffset = PhaseOffset;
	}
	bHasSyncRequest = true;
}

void ULBAAnimInstance::PlayLBALayer(UAnimSequence* Sequence, const TArray<FName>& BranchRoots, bool bLoop, float LoopStart, float BlendTime, float PlayRate, float StartTime)
{
	if (!Sequence)
	{
		StopLBALayer(BlendTime);
		return;
	}
	LayerRequest = FLBAAnimPlayback();
	LayerRequest.Sequence = Sequence;
	LayerRequest.Keys = FindKeyframes(Sequence);
	LayerRequest.bLoop = bLoop;
	LayerRequest.LoopStart = LoopStart;
	LayerRequest.PlayRate = PlayRate;
	LayerRequest.Time = FMath::Clamp(StartTime, 0.f, Sequence->GetPlayLength());
	LayerRequestBranches = BranchRoots;
	LayerRequestBlendTime = BlendTime;
	PendingLayerPlayRate.Reset();
	bHasLayerRequest = true;
	bLayerActive = true;
	LayerTime = LayerRequest.Time;
}

void ULBAAnimInstance::StopLBALayer(float BlendTime)
{
	if (!bLayerActive)
	{
		return;
	}
	LayerRequest = FLBAAnimPlayback();
	LayerRequestBlendTime = BlendTime;
	bHasLayerRequest = true;
	bLayerActive = false;
}

void ULBAAnimInstance::SetApplyRootMotion(bool bApply)
{
	bApplyRootMotion = bApply;
	RootMotionMode = bApply ? ERootMotionMode::RootMotionFromEverything : ERootMotionMode::IgnoreRootMotion;
}

void FLBAAnimInstanceProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	ULBAAnimInstance* Instance = CastChecked<ULBAAnimInstance>(InAnimInstance);
	bApplyRootMotion = Instance->bApplyRootMotion;
	bSmoothKeyframes = Instance->bSmoothKeyframes;
	if (Instance->bHasRequest)
	{
		Previous = Current;
		Current = Instance->Request;
		BlendDuration = Previous.Sequence ? Instance->RequestBlendTime : 0.f;
		BlendElapsed = 0.f;
		Instance->bHasRequest = false;
	}
	if (Instance->PendingPlayRate >= 0.f)
	{
		Current.PlayRate = Instance->PendingPlayRate;
		Instance->PendingPlayRate = -1.f;
	}
	if (Instance->bHasSyncRequest)
	{
		const FLBAAnimPlayback& Synced = Instance->SyncRequest;
		Current.SyncSequence = Synced.SyncSequence;
		Current.SyncKeys = Synced.SyncKeys;
		Current.SyncLoopStart = Synced.SyncLoopStart;
		Current.SyncWeight = Synced.SyncWeight;
		Current.SyncPhaseOffset = Synced.SyncPhaseOffset;
		UpdateSyncTime(Current);
		Instance->bHasSyncRequest = false;
	}
	if (Instance->bHasProcedural)
	{
		Procedural = Instance->PendingProcedural;
		Instance->bHasProcedural = false;
	}
	if (Instance->bHasLayerRequest)
	{
		const float BlendTime = Instance->LayerRequestBlendTime;
		if (Instance->LayerRequest.Sequence)
		{
			// A new layer fades in over the plain animation.
			Layer = Instance->LayerRequest;
			LayerBranches = Instance->LayerRequestBranches;
			LayerWeight = BlendTime > 0.f ? 0.f : 1.f;
			LayerTargetWeight = 1.f;
		}
		else
		{
			LayerTargetWeight = 0.f;
		}
		LayerBlendSpeed = BlendTime > 0.f ? 1.f / BlendTime : BIG_NUMBER;
		Instance->bHasLayerRequest = false;
	}
	if (Instance->PendingLayerPlayRate.IsSet())
	{
		Layer.PlayRate = Instance->PendingLayerPlayRate.GetValue();
		Instance->PendingLayerPlayRate.Reset();
	}
}

FTransform FLBAAnimInstanceProxy::Advance(FLBAAnimPlayback& Playback, float DeltaSeconds, bool bExtractRootMotion) const
{
	FTransform Delta = FTransform::Identity;
	if (!Playback.Sequence || Playback.bFinished)
	{
		return Delta;
	}
	if (Playback.PlayRate < 0.f)
	{
		// Played backwards (no loop, no root motion): runs down to the start.
		Playback.Time = FMath::Max(0.f, Playback.Time + DeltaSeconds * Playback.PlayRate);
		Playback.bFinished = Playback.Time <= 0.f;
		return Delta;
	}
	const float Length = Playback.Sequence->GetPlayLength();
	const float Start = Playback.Time;
	float End = Start + DeltaSeconds * Playback.PlayRate;
	const FAnimExtractContext Context(double(Start), true);

	if (End >= Length)
	{
		if (bExtractRootMotion)
		{
			Delta = Playback.Sequence->ExtractRootMotionFromRange(Start, Length, Context);
		}
		if (Playback.bLoop && Length > Playback.LoopStart)
		{
			// LBA loops restart at the loop frame, not necessarily at the beginning.
			const float LoopLength = Length - Playback.LoopStart;
			End = Playback.LoopStart + FMath::Fmod(End - Length, LoopLength);
			if (bExtractRootMotion)
			{
				Delta = Playback.Sequence->ExtractRootMotionFromRange(Playback.LoopStart, End, Context) * Delta;
			}
		}
		else
		{
			End = Length;
			Playback.bFinished = true;
		}
	}
	else if (bExtractRootMotion)
	{
		Delta = Playback.Sequence->ExtractRootMotionFromRange(Start, End, Context);
	}
	Playback.Time = End;
	UpdateSyncTime(Playback);
	return Delta;
}

void FLBAAnimInstanceProxy::Update(float DeltaSeconds)
{
	const FTransform RootMotion = Advance(Current, DeltaSeconds, bApplyRootMotion);
	if (BlendElapsed < BlendDuration)
	{
		Advance(Previous, DeltaSeconds, false);
		BlendElapsed += DeltaSeconds;
	}
	if (Layer.Sequence)
	{
		Advance(Layer, DeltaSeconds, false);
		LayerWeight = FMath::FInterpConstantTo(LayerWeight, LayerTargetWeight, DeltaSeconds, LayerBlendSpeed);
		if (LayerTargetWeight <= 0.f && LayerWeight <= 0.f)
		{
			Layer = FLBAAnimPlayback();
		}
	}
	if (bApplyRootMotion && !RootMotion.Equals(FTransform::Identity, KINDA_SMALL_NUMBER))
	{
		GetExtractedRootMotion().Accumulate(RootMotion);
	}
}

void FLBAAnimInstanceProxy::PostUpdate(UAnimInstance* InAnimInstance) const
{
	FAnimInstanceProxy::PostUpdate(InAnimInstance);
	ULBAAnimInstance* Instance = CastChecked<ULBAAnimInstance>(InAnimInstance);
	if (!Instance->bHasRequest)
	{
		Instance->CurrentTime = Current.Time;
		Instance->bCurrentFinished = Current.bFinished;
	}
	if (!Instance->bHasLayerRequest && Instance->bLayerActive)
	{
		Instance->LayerTime = Layer.Time;
	}
}

void FLBAAnimInstanceProxy::SampleSequence(const UAnimSequence* Sequence, const ULBAAnimKeyframes* Keys, float Time, bool bLoop, FPoseContext& Into) const
{
	// Extracting root motion locks the root bone in the pose; the motion itself goes to the owner.
	auto Sample = [Sequence](float At, FPoseContext& Pose)
	{
		FAnimationPoseData PoseData(Pose);
		Sequence->GetAnimationPose(PoseData, FAnimExtractContext(double(At), true));
	};
	Sample(Time, Into);
	if (!bSmoothKeyframes || !Keys || Keys->KeyTimes.Num() < 2)
	{
		return;
	}

	// Keyframes are nodes 0..N-1, node N is the end of the sequence (the pose of the loop key).
	const TArray<float>& KeyTimes = Keys->KeyTimes;
	const int32 N = KeyTimes.Num();
	const float Length = Sequence->GetPlayLength();
	const int32 LoopKey = FMath::Clamp(Keys->LoopKey, 0, N - 1);
	auto NodeTime = [&](int32 Node) { return Node < N ? FMath::Min(KeyTimes[Node], Length) : Length; };
	int32 Segment = N - 1;
	while (Segment > 0 && NodeTime(Segment) > Time)
	{
		--Segment;
	}
	if (NodeTime(Segment + 1) - NodeTime(Segment) < 1e-3f)
	{
		return;
	}

	// Neighbours: (time to sample, unwrapped time). Loops continue through the end into the loop key.
	struct FNode
	{
		float SampleTime;
		float At;
	};
	auto Prev = [&](int32 Node) -> FNode
	{
		if (bLoop && Node == LoopKey)
		{
			return { NodeTime(N - 1), NodeTime(Node) - (Length - NodeTime(N - 1)) };
		}
		const int32 P = FMath::Max(Node - 1, 0);
		return { NodeTime(P), NodeTime(P) };
	};
	auto Next = [&](int32 Node) -> FNode
	{
		if (Node < N)
		{
			return { NodeTime(Node + 1), NodeTime(Node + 1) };
		}
		if (bLoop && LoopKey + 1 <= N)
		{
			return { NodeTime(LoopKey + 1), Length + NodeTime(LoopKey + 1) - NodeTime(LoopKey) };
		}
		return { Length, Length };
	};
	const FNode A = Prev(Segment);
	const FNode B = { NodeTime(Segment), NodeTime(Segment) };
	const FNode C = { NodeTime(Segment + 1), NodeTime(Segment + 1) };
	const FNode D = Next(Segment + 1);
	const float S = FMath::Clamp((Time - B.At) / (C.At - B.At), 0.f, 1.f);

	FPoseContext PoseA(Into);
	FPoseContext PoseB(Into);
	FPoseContext PoseC(Into);
	FPoseContext PoseD(Into);
	Sample(A.SampleTime, PoseA);
	Sample(B.SampleTime, PoseB);
	Sample(C.SampleTime, PoseC);
	Sample(D.SampleTime, PoseD);

	for (const FCompactPoseBoneIndex Index : Into.Pose.ForEachBoneIndex())
	{
		const FTransform& TA = PoseA.Pose[Index];
		const FTransform& TB = PoseB.Pose[Index];
		const FTransform& TC = PoseC.Pose[Index];
		const FTransform& TD = PoseD.Pose[Index];
		auto Cubic = [&](float VA, float VB, float VC, float VD) { return CubicKey(VA, VB, VC, VD, A.At, B.At, C.At, D.At, S); };

		const FVector PA = TA.GetTranslation(), PB = TB.GetTranslation(), PC = TC.GetTranslation(), PD = TD.GetTranslation();
		const FVector Position(Cubic(PA.X, PB.X, PC.X, PD.X), Cubic(PA.Y, PB.Y, PC.Y, PD.Y), Cubic(PA.Z, PB.Z, PC.Z, PD.Z));

		// Quaternion components on one hemisphere, then renormalized.
		const FQuat QB = TB.GetRotation();
		FQuat QA = TA.GetRotation();
		FQuat QC = TC.GetRotation();
		FQuat QD = TD.GetRotation();
		QA = (QA | QB) < 0.f ? -QA : QA;
		QC = (QC | QB) < 0.f ? -QC : QC;
		QD = (QD | QC) < 0.f ? -QD : QD;
		FQuat Rotation(Cubic(QA.X, QB.X, QC.X, QD.X), Cubic(QA.Y, QB.Y, QC.Y, QD.Y), Cubic(QA.Z, QB.Z, QC.Z, QD.Z), Cubic(QA.W, QB.W, QC.W, QD.W));
		Rotation.Normalize();

		Into.Pose[Index] = FTransform(Rotation, Position, FMath::Lerp(TB.GetScale3D(), TC.GetScale3D(), S));
	}
}

void FLBAAnimInstanceProxy::SamplePlayback(const FLBAAnimPlayback& Playback, FPoseContext& Into) const
{
	if (!Playback.SyncSequence || Playback.SyncWeight <= 0.f)
	{
		SampleSequence(Playback.Sequence, Playback.Keys, Playback.Time, Playback.bLoop, Into);
		return;
	}
	FPoseContext Primary(Into);
	FPoseContext Synced(Into);
	SampleSequence(Playback.Sequence, Playback.Keys, Playback.Time, Playback.bLoop, Primary);
	SampleSequence(Playback.SyncSequence, Playback.SyncKeys, Playback.SyncTime, true, Synced);
	const FAnimationPoseData PrimaryData(Primary);
	const FAnimationPoseData SyncedData(Synced);
	FAnimationPoseData OutData(Into);
	FAnimationRuntime::BlendTwoPosesTogether(PrimaryData, SyncedData, 1.f - Playback.SyncWeight, OutData);
}

void FLBAAnimInstanceProxy::ApplyProcedural(FPoseContext& Output) const
{
	const float Weight = FMath::Clamp(Procedural.Weight, 0.f, 1.f);
	if (Weight <= 0.f)
	{
		return;
	}
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	FCSPose<FCompactPose> Pose;
	Pose.InitPose(Output.Pose);

	// Lean the whole body around its root (between the feet) and lower it for the feet on lower ground.
	const float PelvisOffset = Procedural.PelvisOffset * Weight;
	const FCompactPoseBoneIndex BodyRoot = FindBone(Bones, Procedural.BodyRoot);
	if (BodyRoot.IsValid())
	{
		FTransform Body = Pose.GetComponentSpaceTransform(BodyRoot);
		Body.SetRotation(FQuat::Slerp(FQuat::Identity, Procedural.Lean, Weight) * Body.GetRotation());
		Body.AddToTranslation(FVector(0.f, 0.f, PelvisOffset));
		const FBoneTransform Transforms[] = { FBoneTransform(BodyRoot, Body) };
		Pose.SafeSetCSBoneTransforms(Transforms);
	}

	// Feet on the ground: the animated height above the flat ground, over the ground under each foot.
	for (const FLBALegPlacement& Leg : Procedural.Legs)
	{
		const FCompactPoseBoneIndex Hip = FindBone(Bones, Leg.Hip);
		const FCompactPoseBoneIndex Knee = FindBone(Bones, Leg.Knee);
		const FCompactPoseBoneIndex Foot = FindBone(Bones, Leg.Foot);
		if (!Hip.IsValid() || !Knee.IsValid() || !Foot.IsValid())
		{
			continue;
		}
		FTransform HipTransform = Pose.GetComponentSpaceTransform(Hip);
		FTransform KneeTransform = Pose.GetComponentSpaceTransform(Knee);
		FTransform FootTransform = Pose.GetComponentSpaceTransform(Foot);
		const FVector FootPosition = FootTransform.GetLocation();
		const float AnimHeight = FootPosition.Z - PelvisOffset;
		const float TargetZ = FMath::Max(AnimHeight, Leg.MinFootHeight) + Leg.GroundOffset;
		const FVector Effector(FootPosition.X, FootPosition.Y, FMath::Lerp(FootPosition.Z, TargetZ, Weight));

		// Keep the knee bending the way the animation bends it (forward, +Y, when the leg is straight).
		const FVector KneePosition = KneeTransform.GetLocation();
		const FVector Bend = KneePosition - (HipTransform.GetLocation() + FootPosition) * 0.5;
		const FVector JointTarget = KneePosition + Bend.GetSafeNormal(1.f, FVector(0.f, 1.f, 0.f)) * 30.0;
		AnimationCore::SolveTwoBoneIK(HipTransform, KneeTransform, FootTransform, JointTarget, Effector, false, 1.0, 1.0);

		// Planted feet follow the slope.
		const float Contact = 1.f - FMath::Clamp((AnimHeight - Leg.MinFootHeight) / 15.f, 0.f, 1.f);
		const FQuat Tilt = FQuat::FindBetweenNormals(FVector::UpVector, Leg.GroundNormal.GetSafeNormal(1e-4, FVector::UpVector));
		FootTransform.SetRotation(FQuat::Slerp(FQuat::Identity, Tilt, Procedural.FootTilt * Contact * Weight) * FootTransform.GetRotation());

		TArray<FBoneTransform, TInlineAllocator<3>> Transforms = { FBoneTransform(Hip, HipTransform), FBoneTransform(Knee, KneeTransform), FBoneTransform(Foot, FootTransform) };
		Transforms.Sort(FCompareBoneTransformIndex());
		Pose.SafeSetCSBoneTransforms(Transforms);
	}

	// Head turn, shared between the neck and head bones.
	for (const TPair<FName, float>& HeadBone : Procedural.HeadBones)
	{
		const FCompactPoseBoneIndex Index = FindBone(Bones, HeadBone.Key);
		if (Index.IsValid() && Procedural.HeadYaw != 0.f)
		{
			FTransform Transform = Pose.GetComponentSpaceTransform(Index);
			const float Angle = FMath::DegreesToRadians(Procedural.HeadYaw * HeadBone.Value * Weight);
			Transform.SetRotation(FQuat(FVector::UpVector, Angle) * Transform.GetRotation());
			const FBoneTransform Transforms[] = { FBoneTransform(Index, Transform) };
			Pose.SafeSetCSBoneTransforms(Transforms);
		}
	}

	// Free rotations, each about its bone's joint: the children follow.
	for (const TPair<FName, FQuat>& BoneRotation : Procedural.BoneRotations)
	{
		const FCompactPoseBoneIndex Index = FindBone(Bones, BoneRotation.Key);
		if (Index.IsValid())
		{
			FTransform Transform = Pose.GetComponentSpaceTransform(Index);
			Transform.SetRotation(FQuat::Slerp(FQuat::Identity, BoneRotation.Value, Weight) * Transform.GetRotation());
			const FBoneTransform Transforms[] = { FBoneTransform(Index, Transform) };
			Pose.SafeSetCSBoneTransforms(Transforms);
		}
	}
	FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(MoveTemp(Pose), Output.Pose);
}

bool FLBAAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	if (!Current.Sequence)
	{
		Output.ResetToRefPose();
		return true;
	}

	if (!Previous.Sequence || BlendElapsed >= BlendDuration || BlendDuration <= 0.f)
	{
		SamplePlayback(Current, Output);
	}
	else
	{
		const float Alpha = FMath::Clamp(BlendElapsed / BlendDuration, 0.f, 1.f);
		FPoseContext CurrentPose(Output);
		FPoseContext PreviousPose(Output);
		SamplePlayback(Current, CurrentPose);
		SamplePlayback(Previous, PreviousPose);
		const FAnimationPoseData CurrentData(CurrentPose);
		const FAnimationPoseData PreviousData(PreviousPose);
		FAnimationPoseData OutData(Output);
		FAnimationRuntime::BlendTwoPosesTogether(CurrentData, PreviousData, Alpha, OutData);
	}

	if (Layer.Sequence && LayerWeight > 0.f)
	{
		// The layer replaces the local transforms of its branches (bones come after their parents).
		FPoseContext LayerPose(Output);
		SampleSequence(Layer.Sequence, Layer.Keys, Layer.Time, Layer.bLoop, LayerPose);
		const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
		TArray<bool, TInlineAllocator<64>> InLayer;
		InLayer.Init(false, Output.Pose.GetNumBones());
		for (const FName& Root : LayerBranches)
		{
			const FCompactPoseBoneIndex Index = FindBone(Bones, Root);
			if (Index.IsValid())
			{
				InLayer[Index.GetInt()] = true;
			}
		}
		for (const FCompactPoseBoneIndex Index : Output.Pose.ForEachBoneIndex())
		{
			const FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(Index);
			bool& bInLayer = InLayer[Index.GetInt()];
			bInLayer = bInLayer || (Parent.IsValid() && InLayer[Parent.GetInt()]);
			if (bInLayer)
			{
				Output.Pose[Index].BlendWith(LayerPose.Pose[Index], LayerWeight);
			}
		}
	}

	ApplyProcedural(Output);
	return true;
}
