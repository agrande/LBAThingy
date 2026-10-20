#pragma once

#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "LBAEntityAsset.h"
#include "LBACharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UInputAction;
class UInputMappingContext;
class ULBAAnimInstance;
class ALBADinofly;
struct FInputActionValue;

/** Twinsen's behaviour modes (F5-F8 in the original game). */
UENUM(BlueprintType)
enum class ELBABehaviour : uint8
{
	Normal,
	Athletic,
	Aggressive,
	Discreet,
};

UENUM(BlueprintType)
enum class ELBAControlMode : uint8
{
	/** Original LBA "tank" controls: up/down walk along the facing, left/right turn; movement comes from the animations. */
	Authentic,
	/**
	 * Third-person controls like Unreal's template: camera-relative movement driven by the character movement
	 * component, free camera, Space jumps. Twinsen runs; animations play in place, kept in step with the movement.
	 */
	Modern,
};

/** Bones of one leg, for foot placement on the ground. */
USTRUCT(BlueprintType)
struct FLBALegBones
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA")
	FName Hip;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA")
	FName Knee;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA")
	FName Foot;
};

/**
 * A playable LBA character (Twinsen by default) driven by imported entity assets.
 *
 * Authentic mode reproduces the original feel: every move is an animation with root motion, turning in
 * place uses the turn animations, steering while walking rotates at a fixed rate, and Space performs the
 * behaviour's action (search, jump, attack, crouch). Behaviours swap the entity (and thus the animation
 * set), keeping the current outfit.
 */
UCLASS(Blueprintable, Config = Game)
class LBA2RUNTIME_API ALBACharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ALBACharacter();

	/** Entity used for each behaviour. Empty entries fall back to Project Settings > Plugins > LBA2 > Hero. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TMap<ELBABehaviour, TObjectPtr<ULBAEntityAsset>> Behaviours;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	ELBABehaviour Behaviour = ELBABehaviour::Normal;

	/** Outfit (body type of the entity). -1 uses the project default. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 BodyType = -1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	ELBAControlMode ControlMode = ELBAControlMode::Modern;

	/** Degrees per second when steering while walking (authentic mode). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Authentic")
	float SteerRate = 140.f;

	/** Upward speed given to jumps (authentic athletic jump). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Authentic")
	float JumpSpeed = 520.f;

	/** Upward speed of jumps in modern mode (the original jumps about 85 cm high). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float ModernJumpSpeed = 450.f;

	/** Acceleration and braking (cm/s2), turn rate (deg/s) and air control of modern mode. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float ModernAcceleration = 1400.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float ModernBraking = 1600.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float ModernTurnRate = 600.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float ModernAirControl = 0.35f;

	/** Free camera distance in modern mode. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float ModernCameraDistance = 360.f;

	/** Landing faster than this (cm/s) stumbles. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float HardLandingSpeed = 1100.f;

	/** Interpolates the original keyframes with smooth curves in modern mode. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	bool bSmoothKeyframes = true;

	/** Places the feet on the ground (two-bone IK) and lowers the body for the lower foot, in modern mode. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	bool bFootPlacement = true;

	/** Scales the lean into turns and accelerations (0 disables it). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	float LeanAmount = 1.f;

	/** Degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	float MaxLean = 12.f;

	/** How far the head turns towards where Twinsen is going (or, standing, where the camera looks); 0 disables it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	float HeadTurn = 0.7f;

	/** Body root bone: the lean pivots around it and the pelvis drop moves it. Twinsen: bone_01, at his feet. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	FName BodyRootBone = TEXT("bone_01");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	TArray<FLBALegBones> Legs;

	/** Neck and head bones, with their share of the head turn. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern|Animation")
	TMap<FName, float> HeadBones;

	/** Dinofly summoned with F / gamepad Y. Empty uses ALBADinofly. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Mounts")
	TSubclassOf<ALBADinofly> DinoflyClass;

	/** Mounts a nearby waiting Dinofly, or calls one over (prototype). */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	ALBADinofly* SummonDinofly();

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetBehaviour(ELBABehaviour NewBehaviour);

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetBodyType(int32 NewBodyType);

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetControlMode(ELBAControlMode NewMode);

	/** Plays an animation slot of the current entity as a one-shot action; locomotion resumes when it ends. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool PlayAction(int32 AnimType);

	/** Jumps in modern mode (Space): a running jump when moving. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool ModernJump();

	/**
	 * Drives the character like the player stick would (X = turn/right, Y = forward; in modern mode relative to the
	 * camera). Used by AI and scripts.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetMoveInput(FVector2D Input) { MoveInput = Input; }

	/** Performs the current behaviour's action (Space in the original game). */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void DoBehaviourAction();

	UFUNCTION(BlueprintPure, Category = "LBA")
	ULBAEntityAsset* GetEntity() const;

	UFUNCTION(BlueprintPure, Category = "LBA")
	ULBAAnimInstance* GetLBAAnimInstance() const;

	/** One line describing the modern locomotion state, for logs. */
	FString GetModernDebugString() const;

	USpringArmComponent* GetCameraBoom() const { return CameraBoom; }
	UCameraComponent* GetFollowCamera() const { return FollowCamera; }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;
	virtual void Landed(const FHitResult& Hit) override;

private:
	void LoadDefaultBehaviours();
	void ApplyEntity();
	void CreateInput();
	void UpdateAuthentic(float DeltaSeconds);
	void UpdateModern(float DeltaSeconds);
	void UpdateModernLocomotion(float DeltaSeconds);
	void UpdateProceduralPose(float DeltaSeconds);
	void ModernLanded();
	void PlaySlot(int32 AnimType, bool bLoop, float BlendTime = 0.15f, float PlayRate = 1.f);
	UAnimSequence* FindAnim(int32 AnimType, float* OutLoopStart = nullptr) const;
	/** The slot of a given behaviour's entity (falls back to the current one). */
	UAnimSequence* FindBehaviourAnim(ELBABehaviour InBehaviour, int32 AnimType, float* OutLoopStart = nullptr) const;

	/** Measurements of an animation used to keep it in step with the movement. */
	struct FClipInfo
	{
		float LoopStart = 0.f;
		float LoopLength = 1.f;
		/** Distance covered by one loop (cm). */
		float Stride = 0.f;
		/** Phase of the right foot's step in the loop (0..1). */
		float StepPhase = 0.f;
		/** Jumps: when the root leaves and gets back to the ground. */
		float TakeoffTime = 0.f;
		float LandTime = 0.f;
		float GetLoopSpeed() const { return LoopLength > 0.f ? Stride / LoopLength : 0.f; }
	};
	const FClipInfo& GetClipInfo(const UAnimSequence* Sequence, float LoopStart) const;
	mutable TMap<const UAnimSequence*, FClipInfo> ClipInfos;

	void OnMove(const FInputActionValue& Value);
	void OnMoveReleased(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnAction(const FInputActionValue& Value);
	void OnUse(const FInputActionValue& Value);
	void OnBehaviour(ELBABehaviour NewBehaviour) { SetBehaviour(NewBehaviour); }
	void OnToggleControlMode(const FInputActionValue& Value);
	void OnSummonDinofly(const FInputActionValue& Value) { SummonDinofly(); }

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InputContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ActionAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> UseAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ToggleModeAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SummonDinoflyAction;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> BehaviourActions;

	FVector2D MoveInput = FVector2D::ZeroVector;
	/** Anim slot of the running one-shot action, or INDEX_NONE. */
	int32 ActionSlot = INDEX_NONE;
	int32 AttackCycle = 0;
	float FallTime = 0.f;
	float CameraYawOffset = 0.f;

	// Modern locomotion
	enum class EModernState : uint8
	{
		None,
		Idle,
		Move,
		Jump,
		Fall,
		Land,
	};
	void SetModernState(EModernState NewState);
	EModernState ModernState = EModernState::None;
	float ModernStateTime = 0.f;
	/** Leading cycle of the walk/run blend (the other one follows it). */
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> LocomotionLead;
	float LocomotionRate = 1.f;
	float RunWeight = 0.f;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> JumpAnim;
	float JumpLandTime = 0.f;
	/** Next locomotion start skips the run start (after a landing). */
	bool bResumeCycle = false;

	// Procedural pose
	float ProceduralWeight = 0.f;
	float LeanBank = 0.f;
	float LeanPitch = 0.f;
	float LastYaw = 0.f;
	float LastSpeed = 0.f;
	float HeadYaw = 0.f;
	float PelvisOffset = 0.f;
	float FootOffsets[2] = { 0.f, 0.f };
	FVector FootNormals[2] = { FVector::UpVector, FVector::UpVector };
	/** Reference heights of the foot bones above the ground, for FootHeightsMesh. */
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMesh> FootHeightsMesh;
	TArray<float> FootHeights;
};

/** Game mode that spawns an ALBACharacter (Twinsen) as the player. */
UCLASS()
class LBA2RUNTIME_API ALBAGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ALBAGameMode();
};
