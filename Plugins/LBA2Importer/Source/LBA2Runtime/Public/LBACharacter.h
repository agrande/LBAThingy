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
class ULBAHitPointsComponent;
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

	/** How far in front of the character interactables (ILBAInteractable) are within reach (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	float InteractReach = 90.f;

	/** How far in front of the character melee blows reach (cm; the original pushes its box 25 cm forward). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Melee")
	float MeleeReach = 60.f;

	/** Damage of a blow (enemies die from any hit). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Melee")
	float MeleeDamage = 25.f;

	/** Modern mode: an attack turns to the closest enemy this close (cm) and this far off the aim (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Melee")
	float MeleeAssistRange = 220.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Melee")
	float MeleeAssistAngle = 60.f;

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

	/**
	 * Free camera distance indoors (interior scenes): close behind Twinsen, like third-person games in buildings. Indoors
	 * the room's walls and ceilings stop the camera too, although the camera channel ignores them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float IndoorCameraDistance = 190.f;

	/** Height above the capsule center the free camera looks at, outdoors and indoors (near the shoulders). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float ModernCameraHeight = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float IndoorCameraHeight = 50.f;

	/** Indoors the free camera's field of view narrows (degrees, horizontal) and it starts looking down this much. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float IndoorFieldOfView = 75.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float IndoorCameraPitch = -20.f;

	/**
	 * Mouse camera turn speed, as shares of the raw input, outdoors and indoors. The close indoor camera turns slower,
	 * the room swinging past less, like adventure games in buildings.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float MouseLookScale = 1.f / 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float IndoorMouseLookScale = 0.25f;

	/** Right stick camera turn speed at full tilt (degrees/s), the same at any frame rate, indoors and out. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Modern")
	float StickLookRate = 75.f;

	/** In an interior scene: the free camera comes in close. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	bool IsIndoors() const { return bIndoors; }

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

	/**
	 * Falls hurt from HardLandingSpeed on (a 6 m drop), by this much life per cm/s faster: about 50 (the original's full
	 * life) at 1700 cm/s, a 15 m drop.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Pain")
	float FallDamagePerSpeed = 0.082f;

	/** After a hit, nothing hurts for this long (s): touching an enemy or standing in a hit zone hurts once a while. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Pain")
	float HurtInvulnerability = 0.8f;

	/** Speed (cm/s) at which touching an enemy throws Twinsen back, and up. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Pain")
	FVector2D TouchKnockback = FVector2D(500.f, 300.f);

	/** Life regained per second once Twinsen has not been hurt for LifeRegenDelay (s). 0: none, like the original. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Pain")
	float LifeRegenPerSecond = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Pain")
	float LifeRegenDelay = 4.f;

	/** Seconds between dying and the death screen. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Pain")
	float RetryDelay = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Pain")
	TSubclassOf<class ULBADeathScreen> DeathScreenClass;

	/** Twinsen's life points (50, the original's), and the hit stars. */
	UFUNCTION(BlueprintPure, Category = "LBA|Pain")
	class ULBAHitPointsComponent* GetHitPoints() const { return HitPoints; }

	/** Hit points at zero. */
	UFUNCTION(BlueprintPure, Category = "LBA|Pain")
	bool IsDead() const { return bDead; }

	/** Hit zones (ALBAHitVolume) and other damage hurt Twinsen's life points. */
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation,
		FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

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

	/**
	 * Where the player pushes, in world space (length up to 1): camera-relative in modern mode, along the facing
	 * (forward or backward) in authentic mode. Zero without input.
	 */
	UFUNCTION(BlueprintPure, Category = "LBA")
	FVector GetMoveIntent() const;

	/** On a ladder (climbing up or down, getting on or off it). */
	UFUNCTION(BlueprintPure, Category = "LBA")
	bool IsClimbing() const { return ClimbState != EClimbState::None; }

	/** Performs the current behaviour's action (Space in the original game). */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void DoBehaviourAction();

	/**
	 * Punches or kicks (the aggressive behaviour's action) in any behaviour: gamepad B, left mouse button. Each blow hits
	 * whatever accepts hits (ULBAHitPointsComponent) in reach in front, at the animation's hit frame. Pressing again
	 * once the blow landed chains the next one.
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool Attack();

	/** Uses the closest interactable (ILBAInteractable) in reach in front of the character. False if there is none. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	bool TryInteract();

	/**
	 * Rides something that carries him in view (a boat): stands idle on Seat (his feet on it) and moves with it, with no
	 * collision or movement of his own, until StopRiding puts him down at Location (his capsule's center).
	 */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void StartRiding(USceneComponent* Seat);

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void StopRiding(const FVector& Location, const FRotator& Facing);

	UFUNCTION(BlueprintPure, Category = "LBA")
	bool IsRiding() const { return bRiding; }

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

	// Ladders (ALBALadderVolume, LBACharacterLadder.cpp): the original's climb clips with root motion, flying.
	enum class EClimbState : uint8
	{
		None,
		/** Climbing up (slot 12, loops). */
		Up,
		/** Climbing out at the top onto the ledge (slot 13). */
		TopOut,
		/** Getting onto the ladder from the ledge, backwards (slot 31). */
		TopIn,
		/** Climbing down (slot 30, loops). */
		Down,
	};
	bool TryStartClimb();
	void UpdateClimb(float DeltaSeconds);
	void SetClimbState(EClimbState NewState);
	void EndClimb();
	EClimbState ClimbState = EClimbState::None;
	TWeakObjectPtr<class ALBALadderVolume> ClimbLadder;
	/** Horizontal normal of the climbed wall (pointing away from it); Twinsen faces the other way. */
	FVector ClimbWallNormal = FVector::ZeroVector;
	bool bClimbWasOrientingToMovement = false;
	void CreateInput();
	void UpdateAuthentic(float DeltaSeconds);
	void UpdateModern(float DeltaSeconds);
	void UpdateModernLocomotion(float DeltaSeconds);
	void UpdateModernCamera(float DeltaSeconds);
	void UpdateProceduralPose(float DeltaSeconds);
	void ModernLanded();
	void PlaySlot(int32 AnimType, bool bLoop, float BlendTime = 0.15f, float PlayRate = 1.f);
	bool PlayActionSequence(UAnimSequence* Sequence, int32 AnimType);

	// Melee
	void UpdateAttack(float DeltaSeconds);
	/** Turns towards the closest enemy near the aim (modern mode), else towards where the player pushes. */
	void AimAttack();
	void DeliverMeleeHit();
	/** The actor accepts hits and is still alive (the Dinofly is not hit by its rider). */
	bool IsMeleeTarget(const AActor* Actor) const;
	/** Seconds since the running attack started, and when its blow lands (-1 once it did, or without attack). */
	float AttackTime = 0.f;
	float AttackHitTime = -1.f;
	float AttackLength = 0.f;
	bool bAttackQueued = false;
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
	void OnMouseLook(const FInputActionValue& Value);
	void OnStickLook(const FInputActionValue& Value);
	void ApplyLook(const FVector2D& Look);
	/** 0 outdoors, 1 indoors, blending with the camera. */
	float GetIndoorLookBlend() const;
	void OnAction(const FInputActionValue& Value);
	void OnUse(const FInputActionValue& Value);
	void OnAttack(const FInputActionValue& Value) { Attack(); }
	void OnBehaviour(ELBABehaviour NewBehaviour) { SetBehaviour(NewBehaviour); }
	void OnToggleControlMode(const FInputActionValue& Value);
	void OnSummonDinofly(const FInputActionValue& Value) { SummonDinofly(); }

	// Pain
	/** Hurts Twinsen (stars from his head, his hit reaction) unless he was just hurt. Returns true if it hurt. */
	bool Hurt(float Damage, int32 ReactionSlot);
	UFUNCTION()
	void HandleDeath();
	void ShowDeathScreen();
	bool bDead = false;
	bool bRiding = false;
	/** World time of the last hit that hurt (-1: none yet). */
	double LastHurtTime = -1.0;
	FTimerHandle DeathTimer;

	UPROPERTY(VisibleAnywhere, Category = "LBA|Pain")
	TObjectPtr<ULBAHitPointsComponent> HitPoints;

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
	TObjectPtr<UInputAction> StickLookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ActionAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> UseAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> AttackAction;

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

	// Indoors: looked up now and then from the scene Twinsen is in.
	bool bIndoors = false;
	int32 IndoorsScene = INDEX_NONE;
	double NextIndoorsCheck = 0.0;
	/** 0 outdoor camera, 1 indoor camera; -1 until the scene is known (no blend then). */
	float IndoorBlend = -1.f;
	float OutdoorFieldOfView = 90.f;

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

/**
 * Game mode that spawns an ALBACharacter (Twinsen) as the player. After a scene change (a door, lba.Scene) the player
 * arrives where ULBASceneTravelSubsystem says; otherwise at the hero start of the level's lowest-numbered scene
 * (PlayerStart tagged LBAScene_NNN), or its plain PlayerStart when it has no scenes.
 */
UCLASS()
class LBA2RUNTIME_API ALBAGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ALBAGameMode();

	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
};
