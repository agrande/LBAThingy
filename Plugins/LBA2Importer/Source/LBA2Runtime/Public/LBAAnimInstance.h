#pragma once

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimMetaData.h"
#include "LBAAnimInstance.generated.h"

class UAnimSequence;

/**
 * Keyframe timing of an imported LBA animation, stored on the sequence. The sequence itself is sampled from the
 * original's linear in-betweens; with the key times the anim instance can interpolate the keys with smooth curves.
 */
UCLASS(meta = (DisplayName = "LBA Keyframes"))
class LBA2RUNTIME_API ULBAAnimKeyframes : public UAnimMetaData
{
	GENERATED_BODY()

public:
	/** Start time (seconds) of each original keyframe. */
	UPROPERTY(VisibleAnywhere, Category = "LBA")
	TArray<float> KeyTimes;

	/** Keyframe the original loops back to after the last one. */
	UPROPERTY(VisibleAnywhere, Category = "LBA")
	int32 LoopKey = 0;
};

/** Playback state of one sequence in the LBA anim instance. */
struct FLBAAnimPlayback
{
	TObjectPtr<UAnimSequence> Sequence = nullptr;
	const ULBAAnimKeyframes* Keys = nullptr;
	float Time = 0.f;
	float PlayRate = 1.f;
	bool bLoop = true;
	/** Where looping animations jump back to (LBA loop frame), in seconds. */
	float LoopStart = 0.f;
	bool bFinished = false;

	/** Optional second looping sequence blended in, kept in step with the first (e.g. walk under a run). */
	TObjectPtr<UAnimSequence> SyncSequence = nullptr;
	const ULBAAnimKeyframes* SyncKeys = nullptr;
	float SyncLoopStart = 0.f;
	float SyncWeight = 0.f;
	/** Phase (fraction of a loop) of the second sequence when the first one is at its loop start. */
	float SyncPhaseOffset = 0.f;
	float SyncTime = 0.f;
};

/** One leg for the foot placement of FLBAProceduralPose. */
struct FLBALegPlacement
{
	FName Hip;
	FName Knee;
	FName Foot;
	/** Height of the ground under the foot relative to the flat ground the animation assumes (component Z 0). */
	float GroundOffset = 0.f;
	/** Ground normal in component space. */
	FVector GroundNormal = FVector::UpVector;
	/** The foot bone never goes lower than this above the ground (its reference height). */
	float MinFootHeight = 0.f;
};

/**
 * Procedural adjustments applied over the animation, in component space: a lean of the whole body around its root
 * bone (at the feet), a pelvis drop, two-bone foot placement on the ground and a head turn. Weight 0 disables them.
 */
struct FLBAProceduralPose
{
	float Weight = 0.f;
	/** Bone the lean and the pelvis drop apply to (the body root, standing on the ground). */
	FName BodyRoot;
	FQuat Lean = FQuat::Identity;
	float PelvisOffset = 0.f;
	TArray<FLBALegPlacement> Legs;
	/** Bones turned about the vertical axis, with their share of HeadYaw. */
	TArray<TPair<FName, float>> HeadBones;
	/** Degrees, positive turns right. */
	float HeadYaw = 0.f;
	/** How much the feet follow the ground's slope (0..1). */
	float FootTilt = 0.6f;
};

/**
 * Native proxy: advances the current (and fading-out) sequence, extracts root motion with LBA loop
 * points and evaluates/blends the poses. No anim graph needed.
 */
struct LBA2RUNTIME_API FLBAAnimInstanceProxy : public FAnimInstanceProxy
{
	FLBAAnimInstanceProxy() = default;
	explicit FLBAAnimInstanceProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual void Update(float DeltaSeconds) override;
	virtual void PostUpdate(UAnimInstance* InAnimInstance) const override;
	virtual bool Evaluate(FPoseContext& Output) override;

	FLBAAnimPlayback Current;
	FLBAAnimPlayback Previous;
	float BlendDuration = 0.f;
	float BlendElapsed = 0.f;
	bool bApplyRootMotion = true;
	bool bSmoothKeyframes = false;

	/** Layer: a second sequence overriding some bone branches, faded in and out by weight. */
	FLBAAnimPlayback Layer;
	TArray<FName> LayerBranches;
	float LayerWeight = 0.f;
	float LayerTargetWeight = 0.f;
	/** Weight change per second. */
	float LayerBlendSpeed = 0.f;

	FLBAProceduralPose Procedural;

private:
	FTransform Advance(FLBAAnimPlayback& Playback, float DeltaSeconds, bool bExtractRootMotion) const;
	void SamplePlayback(const FLBAAnimPlayback& Playback, FPoseContext& Into) const;
	void SampleSequence(const UAnimSequence* Sequence, const ULBAAnimKeyframes* Keys, float Time, bool bLoop, FPoseContext& Into) const;
	void ApplyProcedural(FPoseContext& Output) const;
};

/**
 * Plays LBA animations the way the original engine does: one animation at a time, a short blend into
 * the next one, loops that restart at the animation's loop frame, and movement coming from the
 * animation itself (root motion). A layer can run another animation on some bones only (e.g. a creature's legs at
 * their own pace while its wings keep beating). Used by ALBACharacter, usable by any skeletal mesh.
 *
 * For modern, physics-driven movement it can also smooth the original keyframes with cubic curves, keep two
 * locomotion cycles in step while blending them, and adjust the pose procedurally (lean, foot placement, head).
 */
UCLASS(Transient, NotBlueprintable)
class LBA2RUNTIME_API ULBAAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/**
	 * Starts an animation (at StartTime seconds), blending from the current pose. Restarting the same looping
	 * animation is ignored unless bRestart. A negative PlayRate plays it backwards down to its start (no loop).
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void PlayLBAAnimation(UAnimSequence* Sequence, bool bLoop = true, float LoopStart = 0.f, float BlendTime = 0.15f, float PlayRate = 1.f, bool bRestart = false, float StartTime = 0.f);

	UFUNCTION(BlueprintPure, Category = "LBA|Animation")
	UAnimSequence* GetCurrentAnimation() const { return CurrentSequence; }

	/** True once a non-looping animation reached its end. */
	UFUNCTION(BlueprintPure, Category = "LBA|Animation")
	bool IsCurrentAnimationFinished() const { return bCurrentFinished; }

	UFUNCTION(BlueprintPure, Category = "LBA|Animation")
	float GetCurrentAnimationTime() const { return CurrentTime; }

	/** Root motion drives the owner (authentic LBA movement) or is ignored (in-place animation). */
	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void SetApplyRootMotion(bool bApply);

	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void SetPlayRate(float PlayRate) { PendingPlayRate = PlayRate; }

	/**
	 * Blends a second looping sequence into the current (looping) one with SyncWeight, keeping it in step: its loop
	 * advances at the same pace, PhaseOffset (fraction of a loop) ahead. Weight 0 or no sequence removes it.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void SetSyncedBlend(UAnimSequence* Sequence, float LoopStart, float SyncWeight, float PhaseOffset);

	/** Interpolates the original keyframes with smooth curves instead of the original's straight in-betweens. */
	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void SetSmoothKeyframes(bool bSmooth) { bSmoothKeyframes = bSmooth; }

	/** Procedural pose adjustments for the next frames (see FLBAProceduralPose). */
	void SetProceduralPose(const FLBAProceduralPose& Pose) { PendingProcedural = Pose; bHasProcedural = true; }

	/**
	 * Plays a sequence over the current animation on some bone branches only (each bone of BranchRoots and
	 * everything below it), fading in over BlendTime. One layer at a time; a negative PlayRate plays it backwards.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void PlayLBALayer(UAnimSequence* Sequence, const TArray<FName>& BranchRoots, bool bLoop = true, float LoopStart = 0.f, float BlendTime = 0.15f, float PlayRate = 1.f, float StartTime = 0.f);

	/** Fades the layer out over BlendTime, back to the current animation. */
	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void StopLBALayer(float BlendTime = 0.15f);

	UFUNCTION(BlueprintCallable, Category = "LBA|Animation")
	void SetLayerPlayRate(float PlayRate) { PendingLayerPlayRate = PlayRate; }

	/** True from PlayLBALayer until StopLBALayer. */
	UFUNCTION(BlueprintPure, Category = "LBA|Animation")
	bool IsLayerActive() const { return bLayerActive; }

	UFUNCTION(BlueprintPure, Category = "LBA|Animation")
	float GetLayerTime() const { return LayerTime; }

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override { return new FLBAAnimInstanceProxy(this); }
	virtual void NativeInitializeAnimation() override;

private:
	friend struct FLBAAnimInstanceProxy;

	// Requests (game thread -> proxy)
	bool bHasRequest = false;
	FLBAAnimPlayback Request;
	float RequestBlendTime = 0.f;
	float PendingPlayRate = -1.f;
	bool bApplyRootMotion = true;
	bool bSmoothKeyframes = false;
	bool bHasSyncRequest = false;
	FLBAAnimPlayback SyncRequest;
	bool bHasProcedural = false;
	FLBAProceduralPose PendingProcedural;
	bool bHasLayerRequest = false;
	FLBAAnimPlayback LayerRequest;
	TArray<FName> LayerRequestBranches;
	/** Fade time of the pending layer start or stop (stop when LayerRequest has no sequence). */
	float LayerRequestBlendTime = 0.f;
	TOptional<float> PendingLayerPlayRate;

	// Mirrors (proxy -> game thread)
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> CurrentSequence;
	float CurrentTime = 0.f;
	bool bCurrentFinished = false;
	bool bLayerActive = false;
	float LayerTime = 0.f;
};
