#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "LBA2SnapshotSubsystem.generated.h"

/**
 * Development helper for automated visual checks. Inactive unless the game is started with
 *   -LBA2Shots=auto                       frame all imported meshes from a few angles
 *   -LBA2Shots="x,y,z,pitch,yaw;..."      explicit camera poses (Unreal units/degrees)
 *   -LBA2Shots=hero / dinofly             scripted Twinsen / Dinofly flight test (logs + a shot per step)
 *   -LBA2ShotDir=<folder>                 where to write PNGs (default Saved/LBA2Shots)
 *   -LBA2HeroAt=x,y,yaw                   with camera poses: stands the player pawn on the ground there first
 * It takes the screenshots once the level has settled, then quits.
 */
UCLASS()
class ULBA2SnapshotSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	bool Tick(float DeltaTime);
	bool TickHero(UWorld* World, float DeltaTime);
	bool TickDinofly(UWorld* World, float DeltaTime);
	void BuildAutoPoses(UWorld* World);

	/** -LBA2Shots=hero: drives the player's ALBACharacter through a scripted test and screenshots each step. */
	bool bHero = false;
	/** -LBA2Shots=dinofly: Twinsen summons the Dinofly, which flies a scripted sequence of inputs. */
	bool bDinofly = false;
	double NextReport = 0.0;
	double LastThrust = -10.0;
	int32 HeroStep = -1;
	double HeroStepStart = 0.0;

	struct FPose
	{
		FVector Location;
		FRotator Rotation;
	};
	TArray<FPose> Poses;
	FString ShotDirectory;
	bool bAuto = false;
	int32 NextPose = 0;
	double Elapsed = 0.0;
	double PoseStart = 0.0;
	bool bPosesReady = false;
	/** -LBA2HeroAt: where the player pawn stands during the camera poses (x, y, yaw). */
	TOptional<FVector> HeroAt;
	FTSTicker::FDelegateHandle TickHandle;
	TWeakObjectPtr<AActor> Camera;
};
