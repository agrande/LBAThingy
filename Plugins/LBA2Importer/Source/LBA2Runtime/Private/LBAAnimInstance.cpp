#include "LBAAnimInstance.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "AnimationRuntime.h"

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
	Request.bLoop = bLoop;
	Request.LoopStart = LoopStart;
	Request.PlayRate = PlayRate;
	Request.Time = Sequence ? FMath::Clamp(StartTime, 0.f, Sequence->GetPlayLength()) : 0.f;
	RequestBlendTime = BlendTime;
	bHasRequest = true;
	CurrentSequence = Sequence;
	bCurrentFinished = false;
	CurrentTime = Request.Time;
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
}

bool FLBAAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	if (!Current.Sequence)
	{
		Output.ResetToRefPose();
		return true;
	}
	// Extracting root motion locks the root bone in the pose; the motion itself goes to the owner.
	auto Sample = [](const FLBAAnimPlayback& Playback, FPoseContext& Into)
	{
		FAnimationPoseData PoseData(Into);
		Playback.Sequence->GetAnimationPose(PoseData, FAnimExtractContext(double(Playback.Time), true));
	};

	if (!Previous.Sequence || BlendElapsed >= BlendDuration || BlendDuration <= 0.f)
	{
		Sample(Current, Output);
		return true;
	}

	const float Alpha = FMath::Clamp(BlendElapsed / BlendDuration, 0.f, 1.f);
	FPoseContext CurrentPose(Output);
	FPoseContext PreviousPose(Output);
	Sample(Current, CurrentPose);
	Sample(Previous, PreviousPose);
	const FAnimationPoseData CurrentData(CurrentPose);
	const FAnimationPoseData PreviousData(PreviousPose);
	FAnimationPoseData OutData(Output);
	FAnimationRuntime::BlendTwoPosesTogether(CurrentData, PreviousData, Alpha, OutData);
	return true;
}
