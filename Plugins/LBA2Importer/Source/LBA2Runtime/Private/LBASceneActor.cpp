#include "LBASceneActor.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
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
#include "LBAHitPointsComponent.h"
#include "LBASceneAsset.h"
#include "LBATypes.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

namespace
{
	/** Longest hit reaction played before a dead enemy vanishes (s). */
	constexpr float MaxDeathAnimTime = 1.5f;
}

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
	// Animated only on screen, less often when small there.
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	GetMesh()->bEnableUpdateRateOptimizations = true;

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
	UpdateTicking();
	if (!IsHidden() && GetMesh()->GetSkeletalMeshAsset())
	{
		// Posed once: off screen it isn't animated, and would come into view in its reference pose.
		GetMesh()->TickAnimation(0.f, false);
		GetMesh()->RefreshBoneTransforms();
	}

	if (IsEnemy())
	{
		// One hit kills, whatever the original's life points.
		ULBAHitPointsComponent* HitPoints = NewObject<ULBAHitPointsComponent>(this, TEXT("HitPoints"));
		HitPoints->MaxLife = 1.f;
		HitPoints->OnDeath.AddDynamic(this, &ALBASceneActor::HandleDeath);
		HitPoints->RegisterComponent();
	}
}

void ALBASceneActor::SetActorHiddenInGame(bool bNewHidden)
{
	Super::SetActorHiddenInGame(bNewHidden);
	UpdateTicking();
}

void ALBASceneActor::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);
	UpdateTicking();
}

void ALBASceneActor::UpdateTicking()
{
	if (!HasActorBegunPlay() && !IsActorBeginningPlay())
	{
		return;
	}
	GetMesh()->SetComponentTickEnabled(!IsHidden());
	GetCharacterMovement()->SetComponentTickEnabled(!IsHidden() && GetCharacterMovement()->MovementMode != MOVE_None);
}

bool ALBASceneActor::IsGameplayStandIn() const
{
	return Entity && GetDefault<ULBA2Settings>()->HiddenSceneEntities.Contains(TSoftObjectPtr<ULBAEntityAsset>(Entity.Get()));
}

bool ALBASceneActor::IsEnemy() const
{
	if (!Scene || !Scene->Objects.IsValidIndex(ObjectIndex) || !Entity || IsGameplayStandIn())
	{
		return false;
	}
	// HitObj (OBJECT.CPP): armour 51 and above is invulnerable, objects without life points can't be hit.
	const FLBASceneObjectData& Object = Scene->Objects[ObjectIndex];
	if (Object.Armour >= 51 || Object.LifePoints <= 0 || Object.HasFlag(LBASceneFlags::NoChoc))
	{
		return false;
	}
	return !GetDefault<ULBA2Settings>()->NonEnemySceneEntities.Contains(TSoftObjectPtr<ULBAEntityAsset>(Entity.Get()));
}

void ALBASceneActor::HandleDeath()
{
	if (bDead)
	{
		return;
	}
	bDead = true;
	// Out of the way at once; it stays where it is while it reacts.
	SetActorEnableCollision(false);
	GetCharacterMovement()->SetMovementMode(MOVE_None);

	// The original's objects have no death animation: their hit reaction (CHOC or CHOC2, at random) plays, then they vanish.
	float Duration = 0.f;
	const int32 First = FMath::RandBool() ? int32(ELBAAnimType::Hit) : 22;
	for (const int32 Slot : { First, First == 22 ? int32(ELBAAnimType::Hit) : 22 })
	{
		UAnimSequence* Sequence = Entity ? Entity->GetAnimByIndex(Slot) : nullptr;
		ULBAAnimInstance* Instance = GetLBAAnimInstance();
		if (Sequence && Instance)
		{
			AnimType = Slot;
			Instance->PlayLBAAnimation(Sequence, false, 0.f, 0.05f, 1.f, true);
			Duration = FMath::Min(Sequence->GetPlayLength(), MaxDeathAnimTime);
			break;
		}
	}
	GetWorldTimerManager().SetTimer(VanishTimer, this, &ALBASceneActor::Vanish, FMath::Max(Duration, 0.2f));
}

void ALBASceneActor::Vanish()
{
	if (USoundBase* Sound = GetDefault<ULBA2Settings>()->EnemyDeathSound.LoadSynchronous())
	{
		UGameplayStatics::PlaySoundAtLocation(this, Sound, GetActorLocation());
	}
	GetMesh()->SetVisibility(false, true);
	// Hiding the actor would hide the hit stars too: it goes once they are gone.
	const ULBAHitPointsComponent* HitPoints = FindComponentByClass<ULBAHitPointsComponent>();
	SetLifeSpan(HitPoints ? HitPoints->StarLife + 0.1f : 0.1f);
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

void ALBASceneActor::ApplyBodyLights(const FLBAEntityBody* Body)
{
	for (UPointLightComponent* Light : BodyLights)
	{
		if (Light)
		{
			Light->DestroyComponent();
		}
	}
	BodyLights.Reset();
	if (!Body)
	{
		return;
	}
	for (const FLBABodyLight& Source : Body->Lights)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(this, NAME_None, RF_Transient);
		// Rebuilt with the actor's construction in the editor, like the body itself.
		Light->CreationMethod = EComponentCreationMethod::UserConstructionScript;
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetupAttachment(GetMesh());
		Light->SetRelativeLocation(Source.Location);
		if (Source.FalloffExponent > 0.f)
		{
			Light->SetUseInverseSquaredFalloff(false);
			Light->SetLightFalloffExponent(Source.FalloffExponent);
			Light->SetIntensityUnits(ELightUnits::Unitless);
		}
		else
		{
			Light->SetIntensityUnits(ELightUnits::Candelas);
		}
		Light->SetIntensity(Source.Intensity);
		Light->SetLightColor(Source.Color);
		Light->SetAttenuationRadius(Source.Radius);
		Light->SetSourceRadius(Source.SourceRadius);
		// The light sits inside the glowing part, which must not block it.
		Light->SetCastShadows(false);
		Light->SetVolumetricScatteringIntensity(0.f);
		Light->RegisterComponent();
		BodyLights.Add(Light);
	}
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
	ApplyBodyLights(BodyMesh ? Body : nullptr);

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

namespace
{
	/** Past the closed door's far edge, so that rounding never cuts a closed door (cm). */
	constexpr float ClipMargin = 0.5f;
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
	if (!OpenOffset.IsNearlyZero() && ClosedBounds.IsValid)
	{
		// Model doors (M_LBA_ColorClipped) stop at the closed door's far edge, as the original clips the sprite to a fixed
		// rectangle (OBJECT.CPP): the leaf is longer than its pillar is wide, and would come out on the pillar's other side.
		const FVector Direction = OpenOffset.GetSafeNormal();
		const FBox Closed = ClosedBounds.ShiftBy(ClosedLocation);
		const double FarEdge = FVector::DotProduct(Closed.GetCenter(), Direction) + FVector::DotProduct(Closed.GetExtent(), Direction.GetAbs());
		UStaticMeshComponent* Mesh = GetSpriteComponent();
		Mesh->SetCustomPrimitiveDataFloat(0, float(Direction.X));
		Mesh->SetCustomPrimitiveDataFloat(1, float(Direction.Y));
		Mesh->SetCustomPrimitiveDataFloat(2, float(Direction.Z));
		Mesh->SetCustomPrimitiveDataFloat(3, float(FarEdge) + ClipMargin);
	}
	SetActorTickEnabled(true);
}

void ALBASceneDoor::Open()
{
	SetWantsOpen(true);
}

void ALBASceneDoor::Close()
{
	SetWantsOpen(false);
}

void ALBASceneDoor::SetWantsOpen(bool bOpen)
{
	if (bWantsOpen != bOpen || (OtherLeaf && OtherLeaf->bWantsOpen != bOpen))
	{
		USoundBase* Sound = bOpen ? OpenSound : CloseSound;
		if (!Sound && OtherLeaf)
		{
			Sound = bOpen ? OtherLeaf->OpenSound : OtherLeaf->CloseSound;
		}
		if (Sound)
		{
			UGameplayStatics::PlaySoundAtLocation(this, Sound, GetActorLocation());
		}
	}
	bWantsOpen = bOpen;
	if (OtherLeaf)
	{
		OtherLeaf->bWantsOpen = bOpen;
	}
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
