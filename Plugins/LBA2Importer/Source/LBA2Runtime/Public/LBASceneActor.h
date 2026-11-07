#pragma once

#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "LBAInteractable.h"
#include "LBASceneActor.generated.h"

class UBoxComponent;
class UPointLightComponent;
class USoundBase;
struct FLBAEntityBody;
class UStaticMesh;
class UStaticMeshComponent;
class ULBAAnimInstance;
class ULBAEntityAsset;
class ULBASceneAsset;

/**
 * An object of a scene with a 3D body: characters, animals, vehicles, props. The importer places one per
 * scene object at its start position, with the entity, body and animation it starts with. Without scripts it
 * stands there playing that animation (in place: root motion is off until scripts drive it).
 *
 * The capsule is fitted inside the body's authored collision box, and a box with the exact authored size
 * blocks other pawns, like the original's object collisions.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBASceneActor : public ACharacter
{
	GENERATED_BODY()

public:
	ALBASceneActor();

	/** Scene and object number this actor was imported from (FLBASceneObjectData in the scene asset). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	TObjectPtr<ULBASceneAsset> Scene;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	int32 ObjectIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TObjectPtr<ULBAEntityAsset> Entity;

	/** Body type (outfit) of the entity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 BodyType = 0;

	/** Animation slot played at start (see ELBAAnimType). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 AnimType = 0;

	/** The original hides it at start (scripts show it later). The original still blocks; here it does not, for now. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	bool bStartHidden = false;

	/** Falls under gravity (OBJ_FALLABLE); other objects keep their height. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	bool bFallable = false;

	/**
	 * Life points the player loses touching it, thrown back (0: harmless, the imported scenes' default: their enemies
	 * don't fight back). The feature test map's enemies hurt.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA", meta = (ClampMin = "0"))
	float TouchDamage = 0.f;

	UFUNCTION(BlueprintCallable, Category = "LBA")
	void SetBodyType(int32 NewBodyType);

	/** Plays an animation slot of the entity. */
	UFUNCTION(BlueprintCallable, Category = "LBA")
	void PlayAnimSlot(int32 NewAnimType, bool bLoop = true);

	UFUNCTION(BlueprintPure, Category = "LBA")
	ULBAAnimInstance* GetLBAAnimInstance() const;

	/** Its entity stages gameplay handled differently here (ULBA2Settings::HiddenSceneEntities): hidden in game, without collision. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	bool IsGameplayStandIn() const;

	/**
	 * An enemy: the original lets Twinsen hurt it (armour below 51, life points, no NO_CHOC flag), and its entity is not in
	 * ULBA2Settings::NonEnemySceneEntities. Enemies get a ULBAHitPointsComponent at BeginPlay and die from one hit: they
	 * play their hit reaction, then vanish with the original's death sound. They don't fight back.
	 */
	UFUNCTION(BlueprintPure, Category = "LBA")
	bool IsEnemy() const;

	/** Hit and dying or gone. */
	UFUNCTION(BlueprintPure, Category = "LBA")
	bool IsDead() const { return bDead; }

	/** Sets the mesh of the current body and fits the capsule and collision box to it. */
	void ApplyBody();

	/** Height of the actor's origin (capsule center) above the entity's origin (its feet). */
	float GetOriginHeight() const { return OriginHeight; }

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void SetActorHiddenInGame(bool bNewHidden) override;
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode = 0) override;

protected:
	virtual void BeginPlay() override;

	/** The authored collision box (the capsule only approximates it). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	TObjectPtr<UBoxComponent> CollisionBox;

	/** Lights of the current body's glowing parts (FLBAEntityBody::Lights). */
	UPROPERTY(Transient, VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<TObjectPtr<UPointLightComponent>> BodyLights;

private:
	/** Replaces BodyLights with the lights of Body (null: none). */
	void ApplyBodyLights(const FLBAEntityBody* Body);

	void PlayStartAnimation();

	/**
	 * Hidden objects don't tick, nor does the movement of objects that don't move (MOVE_None): a planet has a few hundred
	 * scene objects, most standing still.
	 */
	void UpdateTicking();

	UFUNCTION()
	void HandleDeath();

	/** The end of the death: the body disappears (the hit stars fly on until the actor goes). */
	void Vanish();

	UPROPERTY()
	float OriginHeight = 0.f;

	bool bDead = false;
	FTimerHandle VanishTimer;
};

/**
 * A sprite object of an interior scene (doors, signs, pickups...). The original draws them as flat images
 * at the object's position; the importer projects each one onto the visible faces of its box (top and the two
 * sides the isometric camera sees, mirrored on the others), so it is exact from the original angle and solid
 * from any other. The mesh's simple collision is the sprite's box.
 *
 * Animated sprites (ANIM3DS) cycle through one mesh per frame.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBASceneSprite : public AActor
{
	GENERATED_BODY()

public:
	ALBASceneSprite();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	TObjectPtr<ULBASceneAsset> Scene;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Scene")
	int32 ObjectIndex = INDEX_NONE;

	/** Sprite number (see FLBASceneObjectData::Sprite). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	int32 Sprite = INDEX_NONE;

	/** Frames of an animated sprite; empty for a still one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	TArray<TObjectPtr<UStaticMesh>> Frames;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA", meta = (ClampMin = "0"))
	float FramesPerSecond = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA")
	bool bStartHidden = false;

	UStaticMeshComponent* GetSpriteComponent() const { return SpriteComponent; }

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA")
	TObjectPtr<UStaticMeshComponent> SpriteComponent;

private:
	float FrameTime = 0.f;
	int32 CurrentFrame = INDEX_NONE;
};

/**
 * A sliding door: a sprite whose track opens it (OPEN_LEFT/RIGHT/UP/DOWN, SPRITE_CLIP sprites). The original's scripts
 * open most of them when Twinsen comes near or meets a condition; here the player opens and closes them by interacting.
 * It slides by its width at the original speed and closes by itself once the player is away, unless someone stands in
 * its way.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBASceneDoor : public ALBASceneSprite, public ILBAInteractable
{
	GENERATED_BODY()

public:
	ALBASceneDoor();

	/** Where the door slides to when open, from its closed position. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Door")
	FVector OpenOffset = FVector::ZeroVector;

	/** Sliding speed (cm/s); the original's 1000 units/s. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Door", meta = (ClampMin = "1"))
	float Speed = 125.f;

	/** The door closes once the player is farther than this from it (cm); 0 keeps it open. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Door", meta = (ClampMin = "0"))
	float AutoCloseDistance = 500.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Door")
	TObjectPtr<USoundBase> OpenSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Door")
	TObjectPtr<USoundBase> CloseSound;

	/** The other leaf of a double door: they open and close together. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Door")
	TObjectPtr<ALBASceneDoor> OtherLeaf;

	UFUNCTION(BlueprintCallable, Category = "LBA|Door")
	void Open();

	UFUNCTION(BlueprintCallable, Category = "LBA|Door")
	void Close();

	/** Opening or open. */
	UFUNCTION(BlueprintPure, Category = "LBA|Door")
	bool IsOpen() const { return bWantsOpen; }

	virtual bool CanInteract_Implementation(APawn* User) const override;
	virtual void Interact_Implementation(APawn* User) override;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	/** A pawn stands where the closed door goes. */
	bool IsDoorwayTaken() const;
	/** Sets this leaf and the other one going, with one sound (often only one of their tracks has any). */
	void SetWantsOpen(bool bOpen);

	FVector ClosedLocation = FVector::ZeroVector;
	/** The closed door's bounds, relative to ClosedLocation. */
	FBox ClosedBounds = FBox(ForceInit);
	/** 0 closed, 1 open. */
	float Opening = 0.f;
	bool bWantsOpen = false;
};
