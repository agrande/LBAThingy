#pragma once

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "LBAAnimInstance.generated.h"

class UAnimSequence;

/** Playback state of one sequence in the LBA anim instance. */
struct FLBAAnimPlayback
{
	TObjectPtr<UAnimSequence> Sequence = nullptr;
	float Time = 0.f;
	float PlayRate = 1.f;
	bool bLoop = true;
	/** Where looping animations jump back to (LBA loop frame), in seconds. */
	float LoopStart = 0.f;
	bool bFinished = false;
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

	/** Layer: a second sequence overriding some bone branches, faded in and out by weight. */
	FLBAAnimPlayback Layer;
	TArray<FName> LayerBranches;
	float LayerWeight = 0.f;
	float LayerTargetWeight = 0.f;
	/** Weight change per second. */
	float LayerBlendSpeed = 0.f;

private:
	FTransform Advance(FLBAAnimPlayback& Playback, float DeltaSeconds, bool bExtractRootMotion) const;
};

/**
 * Plays LBA animations the way the original engine does: one animation at a time, a short blend into
 * the next one, loops that restart at the animation's loop frame, and movement coming from the
 * animation itself (root motion). A layer can run another animation on some bones only (e.g. a creature's legs at
 * their own pace while its wings keep beating). Used by ALBACharacter, usable by any skeletal mesh.
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
