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
	/** Camera-relative movement driven by the character movement component; animations play in place. */
	Modern,
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
	ELBAControlMode ControlMode = ELBAControlMode::Authentic;

	/** Degrees per second when steering while walking (authentic mode). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Authentic")
	float SteerRate = 140.f;

	/** Upward speed given to jumps (authentic athletic jump). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Authentic")
	float JumpSpeed = 520.f;

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

	/** Drives the character like the player stick would (X = turn/right, Y = forward). Used by AI and scripts. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetMoveInput(FVector2D Input) { MoveInput = Input; }

	/** Performs the current behaviour's action (Space in the original game). */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void DoBehaviourAction();

	UFUNCTION(BlueprintPure, Category = "LBA")
	ULBAEntityAsset* GetEntity() const;

	UFUNCTION(BlueprintPure, Category = "LBA")
	ULBAAnimInstance* GetLBAAnimInstance() const;

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
	void PlaySlot(int32 AnimType, bool bLoop, float BlendTime = 0.15f, float PlayRate = 1.f);
	UAnimSequence* FindAnim(int32 AnimType, float* OutLoopStart = nullptr) const;

	void OnMove(const FInputActionValue& Value);
	void OnMoveReleased(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnAction(const FInputActionValue& Value);
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
};

/** Game mode that spawns an ALBACharacter (Twinsen) as the player. */
UCLASS()
class LBA2RUNTIME_API ALBAGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ALBAGameMode();
};
