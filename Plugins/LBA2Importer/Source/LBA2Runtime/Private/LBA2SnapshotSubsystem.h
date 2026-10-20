#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "LBA2SnapshotSubsystem.generated.h"

/**
 * Development helper for automated visual checks. Inactive unless the game is started with
 *   -LBA2Shots=auto                       frame all imported meshes from a few angles
 *   -LBA2Shots="x,y,z,pitch,yaw;..."      explicit camera poses (Unreal units/degrees)
 *   -LBA2Shots=hero / modern / dinofly    scripted Twinsen (authentic / modern controls) / Dinofly flight test (logs + shots)
 *   -LBA2Shots=scenes -LBA2Doors=49,0 -LBA2SceneEnd=43
 *                                         takes the doors to those scenes in turn (then lba.Scene), a shot in each level
 *   -LBA2Shots=ladder -LBA2Ladder=44:7    climbs a ladder zone (scene:zone) up, then back down (log + a shot per second)
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
	bool TickModern(UWorld* World, float DeltaTime);
	bool TickDinofly(UWorld* World, float DeltaTime);
	bool TickScenes(UWorld* World, float DeltaTime);
	bool TickLadder(UWorld* World, float DeltaTime);
	void BuildAutoPoses(UWorld* World);

	/** -LBA2Shots=hero: drives the player's ALBACharacter through a scripted test and screenshots each step. */
	bool bHero = false;
	/** -LBA2Shots=modern: modern controls test (side views, bursts of shots during starts, stops, turns and jumps). */
	bool bModern = false;
	double NextBurst = 0.0;
	int32 BurstIndex = 0;
	float ModernBaseYaw = 0.f;
	/** -LBA2Shots=dinofly: Twinsen summons the Dinofly, which flies a scripted sequence of inputs. */
	bool bDinofly = false;
	/** -LBA2Shots=scenes: scene changes through doors (-LBA2Doors=target scenes) and lba.Scene (-LBA2SceneEnd). */
	bool bScenes = false;
	TArray<int32> SceneDoors;
	int32 SceneEnd = INDEX_NONE;
	int32 SceneStep = 0;
	double SceneStepStart = 0.0;
	bool bSceneShotTaken = false;
	bool bWalkingIntoDoor = false;
	TWeakObjectPtr<UWorld> SceneWorld;
	/** -LBA2Shots=ladder -LBA2Ladder=scene:zone: climbs that ladder up, then back down. */
	bool bLadder = false;
	int32 LadderScene = INDEX_NONE;
	int32 LadderZone = INDEX_NONE;
	int32 LadderPhase = 0;
	double LadderStart = 0.0;
	double NextReport = 0.0;
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
