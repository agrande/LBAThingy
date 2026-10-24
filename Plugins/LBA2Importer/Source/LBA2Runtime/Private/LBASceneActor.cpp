#include "LBASceneActor.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "LBA2Settings.h"
#include "LBAAnimInstance.h"
#include "LBAEntityAsset.h"
#include "LBATypes.h"

ALBASceneActor::ALBASceneActor()
{
	PrimaryActorTick.bCanEverTick = false;
	AutoPossessAI = EAutoPossessAI::Disabled;

	GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	GetMesh()->SetAnimInstanceClass(ULBAAnimInstance::StaticClass());
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Smooth shading instead of world-anchored dots, and the island's character key light, like the hero.
	GetMesh()->SetRenderCustomDepth(true);
	GetMesh()->SetCustomDepthStencilValue(LBA::MovingObjectStencil);
	GetMesh()->SetLightingChannels(true, true, false);

	CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
	CollisionBox->SetupAttachment(GetCapsuleComponent());
	CollisionBox->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	CollisionBox->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	CollisionBox->SetCanEverAffectNavigation(false);

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = false;
	Movement->SetWalkableFloorAngle(55.f);
}

void ALBASceneActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyBody();
}

void ALBASceneActor::BeginPlay()
{
	Super::BeginPlay();
	ApplyBody();
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		// Scripts move objects; until they exist, animations play in place.
		Anim->SetApplyRootMotion(false);
	}
	PlayStartAnimation();
	if (bStartHidden || IsGameplayStandIn())
	{
		SetActorHiddenInGame(true);
		SetActorEnableCollision(false);
	}
	// Without collision a falling object would drop through the floor.
	const bool bSolid = GetActorEnableCollision() && GetCapsuleComponent()->IsCollisionEnabled();
	GetCharacterMovement()->SetMovementMode(bFallable && bSolid ? MOVE_Walking : MOVE_None);
}

bool ALBASceneActor::IsGameplayStandIn() const
{
	return Entity && GetDefault<ULBA2Settings>()->HiddenSceneEntities.Contains(TSoftObjectPtr<ULBAEntityAsset>(Entity.Get()));
}

ULBAAnimInstance* ALBASceneActor::GetLBAAnimInstance() const
{
	return Cast<ULBAAnimInstance>(GetMesh()->GetAnimInstance());
}

void ALBASceneActor::SetBodyType(int32 NewBodyType)
{
	BodyType = NewBodyType;
	ApplyBody();
}

void ALBASceneActor::PlayAnimSlot(int32 NewAnimType, bool bLoop)
{
	AnimType = NewAnimType;
	const FLBAEntityAnim* Anim = Entity ? Entity->Anims.Find(AnimType) : nullptr;
	ULBAAnimInstance* Instance = GetLBAAnimInstance();
	if (Anim && Anim->Sequence && Instance)
	{
		Instance->PlayLBAAnimation(Anim->Sequence, bLoop, Anim->LoopStartTime);
	}
}

void ALBASceneActor::PlayStartAnimation()
{
	PlayAnimSlot(AnimType, true);
}

void ALBASceneActor::ApplyBody()
{
	USkeletalMesh* BodyMesh = nullptr;
	const FLBAEntityBody* Body = nullptr;
	if (Entity)
	{
		Body = Entity->Bodies.Find(BodyType);
		BodyMesh = Body ? Body->Mesh.Get() : nullptr;
	}
	if (GetMesh()->GetSkeletalMeshAsset() != BodyMesh)
	{
		GetMesh()->SetSkeletalMesh(BodyMesh);
	}

	// The authored box is in mesh space (facing +Y); the mesh is turned -90 degrees in the actor.
	FBox Box = Body && Body->CollisionBox.IsValid ? Body->CollisionBox : (BodyMesh ? BodyMesh->GetBounds().GetBox() : FBox(FVector(-30.0, -30.0, 0.0), FVector(30.0, 30.0, 150.0)));
	Box = Box.TransformBy(FTransform(GetMesh()->GetRelativeRotation()));
	const FVector Size = Box.GetSize().ComponentMax(FVector(8.0));
	const FVector Center = Box.GetCenter();
	const float HalfHeight = Size.Z * 0.5f;
	const float Radius = FMath::Clamp(FMath::Min(Size.X, Size.Y) * 0.5f, 4.f, HalfHeight);
	GetCapsuleComponent()->SetCapsuleSize(Radius, HalfHeight);
	OriginHeight = Center.Z;
	GetMesh()->SetRelativeLocation(FVector(0.0, 0.0, -Center.Z));
	CollisionBox->SetRelativeLocation(FVector(Center.X, Center.Y, 0.0));
	CollisionBox->SetBoxExtent(Size * 0.5);
	// The original never collides with objects without a body (Body.Num -1, script helpers); nor do bodies we lack a mesh for.
	const ECollisionEnabled::Type Collision = BodyMesh ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision;
	GetCapsuleComponent()->SetCollisionEnabled(Collision);
	CollisionBox->SetCollisionEnabled(Collision);

#if WITH_EDITOR
	// Show the start animation in the editor too.
	if (GetWorld() && !GetWorld()->IsGameWorld() && BodyMesh)
	{
		GetMesh()->SetUpdateAnimationInEditor(true);
		PlayStartAnimation();
	}
#endif
}

ALBASceneSprite::ALBASceneSprite()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	SpriteComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sprite"));
	SpriteComponent->SetMobility(EComponentMobility::Movable);
	SpriteComponent->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	SpriteComponent->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	RootComponent = SpriteComponent;
}

void ALBASceneSprite::BeginPlay()
{
	Super::BeginPlay();
	if (bStartHidden)
	{
		SetActorHiddenInGame(true);
		SetActorEnableCollision(false);
	}
	SetActorTickEnabled(Frames.Num() > 1 && FramesPerSecond > 0.f);
}

void ALBASceneSprite::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Frames.Num() < 2 || FramesPerSecond <= 0.f)
	{
		return;
	}
	FrameTime += DeltaSeconds;
	const int32 Frame = int32(FrameTime * FramesPerSecond) % Frames.Num();
	if (Frame != CurrentFrame && Frames[Frame])
	{
		CurrentFrame = Frame;
		SpriteComponent->SetStaticMesh(Frames[Frame]);
	}
}

ALBASceneDoor::ALBASceneDoor()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ALBASceneDoor::BeginPlay()
{
	Super::BeginPlay();
	ClosedLocation = GetActorLocation();
	ClosedBounds = GetSpriteComponent()->Bounds.GetBox().ShiftBy(-ClosedLocation);
	SetActorTickEnabled(true);
}

void ALBASceneDoor::Open()
{
	if (!bWantsOpen && OpenSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, OpenSound, GetActorLocation());
	}
	bWantsOpen = true;
}

void ALBASceneDoor::Close()
{
	if (bWantsOpen && CloseSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, CloseSound, GetActorLocation());
	}
	bWantsOpen = false;
}

bool ALBASceneDoor::CanInteract_Implementation(APawn* User) const
{
	return !IsHidden() && GetActorEnableCollision() && !OpenOffset.IsNearlyZero();
}

void ALBASceneDoor::Interact_Implementation(APawn* User)
{
	if (bWantsOpen)
	{
		Close();
	}
	else
	{
		Open();
	}
}

bool ALBASceneDoor::IsDoorwayTaken() const
{
	// Slightly smaller than the door, so a pawn merely touching its sides does not hold it open.
	const FBox Doorway = ClosedBounds.ShiftBy(ClosedLocation).ExpandBy(-2.0);
	return Doorway.IsValid && GetWorld()->OverlapAnyTestByObjectType(Doorway.GetCenter(), FQuat::Identity,
		FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeBox(Doorway.GetExtent().ComponentMax(FVector(1.0))));
}

void ALBASceneDoor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bWantsOpen && AutoCloseDistance > 0.f && Opening >= 1.f)
	{
		const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
		if (!Player || ClosedBounds.ShiftBy(ClosedLocation).ComputeSquaredDistanceToPoint(Player->GetActorLocation()) > FMath::Square(AutoCloseDistance))
		{
			Close();
		}
	}

	const float Target = bWantsOpen ? 1.f : 0.f;
	if (Opening == Target || OpenOffset.IsNearlyZero())
	{
		return;
	}
	if (!bWantsOpen && IsDoorwayTaken())
	{
		return;
	}
	Opening = FMath::FInterpConstantTo(Opening, Target, DeltaSeconds, Speed / OpenOffset.Size());
	SetActorLocation(ClosedLocation + OpenOffset * Opening);
}
