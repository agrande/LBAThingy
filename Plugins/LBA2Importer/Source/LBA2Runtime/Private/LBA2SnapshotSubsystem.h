#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "LBA2SnapshotSubsystem.generated.h"

class ALBASceneActor;

/**
 * Development helper for automated visual checks. Inactive unless the game is started with
 *   -LBA2Shots=auto                       frame all imported meshes from a few angles
 *   -LBA2Shots="x,y,z,pitch,yaw;..."      explicit camera poses (Unreal units/degrees)
 *   -LBA2Shots=hero / modern / dinofly    scripted Twinsen (authentic / modern controls) / Dinofly flight test (logs + shots)
 *   -LBA2Shots=scenes -LBA2Doors=49,0 -LBA2SceneEnd=43
 *                                         takes the doors to those scenes in turn (then lba.Scene), a shot in each level
 *   -LBA2Shots=ladder -LBA2Ladder=44:7    climbs a ladder zone (scene:zone) up, then back down (log + a shot per second)
 *   -LBA2Shots=door -LBA2Door=15:3        opens a door (scene:object) by interacting, walks through, checks it closes behind
 *   -LBA2Shots=attack -LBA2Attack=15:14   punches an enemy (scene:object) twice, checks it dies and vanishes
 *   -LBA2Shots=boat [-LBA2BoatTo=DESERT]  globe mode (open the island with ?Globe): gets on the moored boat, ashore and back,
 *                                         backs out, drives at full throttle, turns, (crosses to that island,) stops;
 *                                         -LBA2BoatTo=BELT drives straight at the planet's belt until it runs aground
 *   -LBA2Shots=belt                       globe mode: the Dinofly and Twinsen against the planet's belt (how high they get)
 *   -LBA2ShotDir=<folder>                 where to write PNGs (default Saved/LBA2Shots)
 *   -LBA2HeroAt=x,y,yaw[,z]               with camera poses, hero or modern: stands the player pawn on the ground there first
 *                                         (the first surface below z, default: the highest; indoors, below the ceiling)
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
	bool TickWind(UWorld* World, float DeltaTime);
	bool TickScenes(UWorld* World, float DeltaTime);
	bool TickLadder(UWorld* World, float DeltaTime);
	bool TickDoor(UWorld* World, float DeltaTime);
	bool TickAttack(UWorld* World, float DeltaTime);
	bool TickMenu(UWorld* World, float DeltaTime);
	bool TickGlobe(UWorld* World, float DeltaTime);
	bool TickBoat(UWorld* World, float DeltaTime);
	bool TickBelt(UWorld* World, float DeltaTime);
	void BuildAutoPoses(UWorld* World);
	/** -LBA2HeroAt: stands the pawn on the first surface below the point, facing its yaw. */
	void PlaceHero(UWorld* World, APawn* Pawn) const;

	/** -LBA2Shots=hero: drives the player's ALBACharacter through a scripted test and screenshots each step. */
	bool bHero = false;
	/** -LBA2Shots=modern: modern controls test (side views, bursts of shots during starts, stops, turns and jumps). */
	bool bModern = false;
	double NextBurst = 0.0;
	int32 BurstIndex = 0;
	float ModernBaseYaw = 0.f;
	/** -LBA2Shots=dinofly: Twinsen summons the Dinofly, which flies a scripted sequence of inputs. */
	bool bDinofly = false;
	/**
	 * -LBA2Shots=globe (open the island with ?Globe): the Dinofly flies out to sea and back, the island sinking below
	 * the bent horizon and rising again, and the planet is shot from high above. With -LBA2GlobeTo=ISLAND, it flies to
	 * that island instead (until within its reach), looks at both islands from high above and lands.
	 */
	bool bGlobe = false;
	/** -LBA2GlobeTo: the island (index in the planet asset) the voyage goes to, if any, and how far it was (m). */
	int32 GlobeDestination = INDEX_NONE;
	double GlobeVoyageDistance = 0.0;
	int32 GlobeStep = -1;
	double GlobeStepStart = 0.0;
	int32 GlobeFrame = 0;
	double GlobeFrameTime = 0.0;
	int32 GlobeFrames = 0;
	double GlobeWorstFrame = 0.0;
	/** -LBA2Shots=boat: Twinsen takes the boat moored at the opened island out to sea (and to -LBA2BoatTo's island). */
	bool bBoat = false;
	int32 BoatDestination = INDEX_NONE;
	/** -LBA2BoatTo=BELT: straight out at the planet's belt, until it runs aground. */
	bool bBoatToBelt = false;
	int32 BoatStep = -1;
	double BoatStepStart = 0.0;
	int32 BoatFrame = 0;
	double BoatNextReport = 0.0;
	float BoatTopSpeed = 0.f;
	TWeakObjectPtr<class ALBABoat> TestBoat;
	/**
	 * -LBA2Shots=belt (globe mode): the Dinofly flies out to the planet's belt and over it, looks at it from above, comes
	 * back and lands on its flank; Twinsen gets off and walks, then jumps, up it. Logs how high and how far past the
	 * shore each got.
	 */
	bool bBelt = false;
	int32 BeltStep = -1;
	double BeltStepStart = 0.0;
	int32 BeltFrame = 0;
	/** The belt's shore (world, sea level) straight out from the pole past the opened island, and that way out. */
	FVector BeltShore = FVector::ZeroVector;
	FVector BeltOutward = FVector::ForwardVector;
	/** The highest the Dinofly got above the sea, and Twinsen's feet; the farthest either got past the shore (cm). */
	double BeltDinoflyTop = 0.0;
	double BeltHeroTop = 0.0;
	double BeltDeepest = 0.0;
	/** -LBA2Shots=wind: the Dinofly hovers hands-off at places of Citadel's storm, then tries to fly away and climb. */
	bool bWind = false;
	bool bStepStarted = false;
	FVector StepStartLocation = FVector::ZeroVector;
	/** -LBA2Shots=scenes: scene changes through doors (-LBA2Doors=target scenes) and lba.Scene (-LBA2SceneEnd). */
	bool bScenes = false;
	TArray<int32> SceneDoors;
	int32 SceneEnd = INDEX_NONE;
	int32 SceneStep = 0;
	double SceneStepStart = 0.0;
	bool bSceneShotTaken = false;
	bool bWalkingIntoDoor = false;
	TWeakObjectPtr<UWorld> SceneWorld;
	/**
	 * -LBA2Shots=menu -LBA2MenuScenes=0,138: in each level, opens the travel menu, screenshots it, shows the next
	 * planet, screenshots again, then picks the next scene in the menu.
	 */
	bool bMenu = false;
	TArray<int32> MenuScenes;
	int32 MenuPhase = 0;
	/** -LBA2Shots=ladder -LBA2Ladder=scene:zone: climbs that ladder up, then back down. */
	bool bLadder = false;
	int32 LadderScene = INDEX_NONE;
	int32 LadderZone = INDEX_NONE;
	int32 LadderPhase = 0;

	/** -LBA2Shots=door -LBA2Door=scene:object: opens that door, walks through it and away (it closes). */
	bool bDoor = false;
	int32 DoorScene = INDEX_NONE;
	int32 DoorObject = INDEX_NONE;
	int32 DoorPhase = 0;
	double DoorStart = 0.0;
	/** Through the door, from the side the hero starts on. */
	FVector DoorTravel = FVector::ForwardVector;

	/** -LBA2Shots=attack -LBA2Attack=scene:object: stands next to that enemy, punches it twice (chained), checks it dies and vanishes. */
	bool bAttack = false;
	int32 AttackScene = INDEX_NONE;
	int32 AttackObject = INDEX_NONE;
	int32 AttackPhase = 0;
	double AttackStart = 0.0;
	TWeakObjectPtr<ALBASceneActor> AttackTarget;
	double LadderStart = 0.0;
	double NextReport = 0.0;
	int32 HeroStep = -1;
	double HeroStepStart = 0.0;

	struct FPose
	{
		FVector Location;
		FRotator Rotation;
		/** Input sent through Slate when the pose starts: a key name, Key=value for an axis, or MouseMove. */
		FString Press;
	};
	void SendPress(const FString& Press);
	TArray<FPose> Poses;
	FString ShotDirectory;
	bool bAuto = false;
	int32 NextPose = 0;
	double Elapsed = 0.0;
	double PoseStart = 0.0;
	bool bPosesReady = false;
	/** -LBA2HeroAt: where the player pawn stands during the camera poses (x, y, yaw), and the height it is looked for below. */
	TOptional<FVector> HeroAt;
	double HeroFromZ = 100000.0;
	FTSTicker::FDelegateHandle TickHandle;
	TWeakObjectPtr<AActor> Camera;
};
