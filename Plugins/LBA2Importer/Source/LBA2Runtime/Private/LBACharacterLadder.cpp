// Ladders: the original's ladder zones (CheckZoneSce, case 6, in OBJECT.CPP) with its climb clips.

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "LBA2Settings.h"
#include "LBAAnimInstance.h"
#include "LBACharacter.h"
#include "LBAZoneVolume.h"

namespace
{
	/** The climb clips: up (loops), out at the top, onto the ladder from the top, down (loops). */
	constexpr int32 ClimbUpSlot = 12;
	constexpr int32 TopOutSlot = 13;
	constexpr int32 DownSlot = 30;
	constexpr int32 TopInSlot = 31;

	/** LBA units below the top of the zone where climbing out starts (DELTAY_ANIM_ECHELLE). */
	constexpr float TopOutHeight = 1178.f;

	/** LBA ladders are axis-aligned: the axis closest to a horizontal direction. */
	FVector SnapToAxis(const FVector& Direction)
	{
		return FMath::Abs(Direction.X) >= FMath::Abs(Direction.Y) ? FVector(FMath::Sign(Direction.X), 0.0, 0.0) : FVector(0.0, FMath::Sign(Direction.Y), 0.0);
	}
}

bool ALBACharacter::TryStartClimb()
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (ActionSlot != INDEX_NONE || !Movement->IsMovingOnGround())
	{
		return false;
	}
	const FVector Intent = GetMoveIntent();
	if (Intent.Size2D() < 0.5)
	{
		return false;
	}
	TArray<AActor*> Ladders;
	GetOverlappingActors(Ladders, ALBALadderVolume::StaticClass());
	const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Location = GetActorLocation();
	const double Feet = Location.Z - HalfHeight;
	const FVector Direction = SnapToAxis(Intent);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LBALadder), false, this);
	const float Scale = ULBA2Settings::GetWorldScale();

	for (AActor* Actor : Ladders)
	{
		ALBALadderVolume* Ladder = Cast<ALBALadderVolume>(Actor);
		if (!Ladder || !Ladder->bEnabled)
		{
			continue;
		}
		const double Top = Ladder->GetTopHeight();
		if (Feet < Top - 256.f * Scale)
		{
			// At the foot: walking into the wall grabs the ladder.
			FHitResult Hit;
			if (GetWorld()->LineTraceSingleByChannel(Hit, Location, Location + Direction * (Radius + 35.0), ECC_Visibility, Params)
				&& FVector::DotProduct(Hit.ImpactNormal.GetSafeNormal2D(), Direction) < -0.7)
			{
				ClimbLadder = Ladder;
				ClimbWallNormal = -Direction;
				SetClimbState(EClimbState::Up);
				return true;
			}
		}
		else if (Feet > Top - 60.0)
		{
			// On the ledge: walking off the edge over the ladder climbs down it, backwards like the original.
			const FVector Ahead = Location + Direction * (Radius + 30.0);
			FHitResult Hit;
			if (!GetWorld()->LineTraceSingleByChannel(Hit, Ahead, Ahead - FVector(0.0, 0.0, HalfHeight + 80.0), ECC_Visibility, Params))
			{
				ClimbLadder = Ladder;
				ClimbWallNormal = Direction;
				SetClimbState(EClimbState::TopIn);
				return true;
			}
		}
	}
	return false;
}

void ALBACharacter::SetClimbState(EClimbState NewState)
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bWasClimbing = ClimbState != EClimbState::None;
	if (!bWasClimbing)
	{
		// Root motion drives the climb, vertical part included (flying keeps it), in both control modes.
		bClimbWasOrientingToMovement = Movement->bOrientRotationToMovement;
		Movement->bOrientRotationToMovement = false;
		Movement->SetMovementMode(MOVE_Flying);
		Movement->StopMovementImmediately();
		if (Anim)
		{
			Anim->SetApplyRootMotion(true);
		}
		SetActorRotation(FRotator(0.0, (-ClimbWallNormal).Rotation().Yaw, 0.0));
	}
	ClimbState = NewState;

	float LoopStart = 0.f;
	switch (NewState)
	{
	case EClimbState::Up:
		if (UAnimSequence* Sequence = FindAnim(ClimbUpSlot, &LoopStart); Sequence && Anim)
		{
			// From the ground the grab plays first; switching back from climbing down resumes the loop.
			Anim->PlayLBAAnimation(Sequence, true, LoopStart, 0.15f, 1.f, true, bWasClimbing ? LoopStart : 0.f);
		}
		break;
	case EClimbState::TopOut:
		PlaySlot(TopOutSlot, false, 0.1f);
		break;
	case EClimbState::TopIn:
		PlaySlot(TopInSlot, false, 0.15f);
		break;
	case EClimbState::Down:
		PlaySlot(DownSlot, true, 0.15f);
		break;
	default:
		break;
	}
}

void ALBACharacter::EndClimb()
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	ClimbState = EClimbState::None;
	ClimbLadder.Reset();
	Movement->bOrientRotationToMovement = bClimbWasOrientingToMovement;
	Movement->StopMovementImmediately();
	Movement->SetMovementMode(MOVE_Walking);
	if (ULBAAnimInstance* Anim = GetLBAAnimInstance())
	{
		Anim->SetApplyRootMotion(ControlMode == ELBAControlMode::Authentic);
	}
	// Locomotion picks up from a stand.
	SetModernState(EModernState::None);
}

void ALBACharacter::UpdateClimb(float DeltaSeconds)
{
	ULBAAnimInstance* Anim = GetLBAAnimInstance();
	const ALBALadderVolume* Ladder = ClimbLadder.Get();
	if (!Anim || !Ladder)
	{
		EndClimb();
		return;
	}
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const double Top = Ladder->GetTopHeight();
	const double Feet = GetActorLocation().Z - HalfHeight;
	const float Scale = ULBA2Settings::GetWorldScale();
	SetActorRotation(FRotator(0.0, (-ClimbWallNormal).Rotation().Yaw, 0.0));

	// Up or down the ladder with the stick (towards the wall climbs), otherwise keep going like the original.
	const double Push = FVector::DotProduct(GetMoveIntent(), -ClimbWallNormal);
	switch (ClimbState)
	{
	case EClimbState::Up:
		if (Feet >= Top - TopOutHeight * Scale)
		{
			SetClimbState(EClimbState::TopOut);
		}
		else if (Push < -0.5)
		{
			SetClimbState(EClimbState::Down);
		}
		break;

	case EClimbState::TopOut:
		if (Feet > Top)
		{
			// The original never lets the climb pass the top of the zone (Info3).
			SetActorLocation(FVector(GetActorLocation().X, GetActorLocation().Y, Top + HalfHeight), false);
		}
		if (Anim->IsCurrentAnimationFinished())
		{
			// Step onto the ledge if the clip left Twinsen at its edge.
			const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
			SetActorLocation(GetActorLocation() + FVector(0.0, 0.0, 2.0) - ClimbWallNormal * Radius, true);
			EndClimb();
		}
		break;

	case EClimbState::TopIn:
		if (Anim->IsCurrentAnimationFinished())
		{
			SetClimbState(EClimbState::Down);
		}
		break;

	case EClimbState::Down:
	{
		if (Push > 0.5 && Feet < Top - TopOutHeight * Scale)
		{
			SetClimbState(EClimbState::Up);
			break;
		}
		// Down to the floor: back to walking.
		FHitResult Hit;
		const FCollisionQueryParams Params(SCENE_QUERY_STAT(LBALadder), false, this);
		const FVector Location = GetActorLocation();
		const bool bFloor = GetWorld()->SweepSingleByChannel(Hit, Location, Location - FVector(0.0, 0.0, 6.0), FQuat::Identity, ECC_Pawn,
			GetCapsuleComponent()->GetCollisionShape(-1.f), Params) && GetCharacterMovement()->IsWalkable(Hit);
		if (bFloor || Feet < Ladder->GetComponentsBoundingBox(true).Min.Z - 20.0)
		{
			EndClimb();
		}
		break;
	}

	default:
		EndClimb();
		break;
	}
}
