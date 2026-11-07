#include "LevelSelector/LBALevelSelector.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/LevelStreaming.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "LBA2Runtime.h"
#include "LBAPointsOfInterest.h"
#include "LBASceneAsset.h"
#include "LBATestStation.h"
#include "LevelEditorViewport.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"

namespace
{
	/** Where the selector last framed, for Play here. */
	struct FLastPick
	{
		FString Package;
		FTransform Start;
		int32 Scene = INDEX_NONE;
	};
	FLastPick LastPick;

	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FString GetEditorPackage()
	{
		const UWorld* World = GetEditorWorld();
		return World ? World->GetOutermost()->GetName() : FString();
	}

	/** Where the game starts a level (ALBAGameMode): the lowest-numbered scene's hero start, else a plain player start. */
	APlayerStart* FindLevelStart(UWorld* World)
	{
		APlayerStart* SceneStart = nullptr;
		APlayerStart* LevelStart = nullptr;
		int32 SceneStartIndex = TNumericLimits<int32>::Max();
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			FString Tag = It->PlayerStartTag.ToString();
			if (Tag.RemoveFromStart(TEXT("LBAScene_")))
			{
				if (FCString::Atoi(*Tag) < SceneStartIndex)
				{
					SceneStart = *It;
					SceneStartIndex = FCString::Atoi(*Tag);
				}
			}
			else if (!LevelStart && !It->IsA<ALBATestStation>())
			{
				LevelStart = *It;
			}
		}
		return SceneStart ? SceneStart : LevelStart;
	}

	APlayerStart* FindTaggedStart(UWorld* World, FName Tag)
	{
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			if (It->PlayerStartTag == Tag)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** Behind and above the place, looking where the player would. */
	void FrameViewports(const FTransform& Start, AActor* Select)
	{
		const FRotator Facing(0.0, Start.Rotator().Yaw, 0.0);
		const FVector Location = Start.GetLocation() - Facing.Vector() * 600.0 + FVector(0.0, 0.0, 300.0);
		for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
		{
			if (Client && Client->IsPerspective())
			{
				Client->SetViewLocation(Location);
				Client->SetViewRotation(FRotator(-22.0, Facing.Yaw, 0.0));
				Client->Invalidate();
			}
		}
		if (Select)
		{
			GEditor->SelectNone(false, true);
			GEditor->SelectActor(Select, true, true);
		}
	}

	/** Opens a level in the editor unless it is open; asks to save changes first. False if cancelled or missing. */
	bool OpenEditorLevel(const FString& Package)
	{
		if (Package == GetEditorPackage())
		{
			return true;
		}
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Package, Filename, FPackageName::GetMapPackageExtension())
			|| !FPackageName::DoesPackageExist(Package))
		{
			UE_LOG(LogLBA2, Warning, TEXT("Level selector: no level %s"), *Package);
			return false;
		}
		if (!FEditorFileUtils::SaveDirtyPackages(true, true, true))
		{
			return false;
		}
		return FEditorFileUtils::LoadMap(Filename, false, true) && Package == GetEditorPackage();
	}
}

ULBALevelSelector::ULBALevelSelector(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bEditorMode = true;
}

void ULBALevelSelector::GoTo(const FLBATravelDestination& Destination)
{
	// Opening a level rebuilds this widget (the tab follows the editor world): not from inside its own click.
	GEditor->GetTimerManager()->SetTimerForNextTick([Destination]()
	{
		OpenInEditor(Destination);
	});
}

bool ULBALevelSelector::OpenInEditor(const FLBATravelDestination& Destination)
{
	FString Package;
	switch (Destination.Kind)
	{
	case ELBATravelKind::Island:
		for (const FLBAIslandInfo& Island : ULBAIslandTravelSubsystem::FindIslands())
		{
			if (Island.Id == Destination.Island)
			{
				Package = Island.Level.ToSoftObjectPath().GetLongPackageName();
			}
		}
		if (Package.IsEmpty() || !OpenEditorLevel(Package))
		{
			return false;
		}
		if (APlayerStart* Start = FindLevelStart(GetEditorWorld()))
		{
			FrameViewports(Start->GetActorTransform(), Start);
			LastPick = { Package, Start->GetActorTransform(), INDEX_NONE };
		}
		return true;

	case ELBATravelKind::Scene:
	{
		const ULBASceneAsset* Scene = ULBASceneTravelSubsystem::FindScene(Destination.Scene);
		if (!Scene || Scene->SceneryLevels.IsEmpty() || Scene->Objects.IsEmpty())
		{
			UE_LOG(LogLBA2, Warning, TEXT("Level selector: no scene %d (import the Scenes layer)."), Destination.Scene);
			return false;
		}
		// The island variant asked for, else the level open if the scene is in it, else its first level.
		FString Variant;
		for (const FLBAIslandInfo& Island : ULBAIslandTravelSubsystem::FindIslands())
		{
			if (Island.Id == Destination.Island)
			{
				Variant = Island.Level.ToSoftObjectPath().GetLongPackageName();
			}
		}
		for (const FString& Wanted : { Variant, GetEditorPackage() })
		{
			for (const TSoftObjectPtr<UWorld>& Level : Scene->SceneryLevels)
			{
				if (Package.IsEmpty() && Level.ToSoftObjectPath().GetLongPackageName() == Wanted)
				{
					Package = Wanted;
				}
			}
		}
		if (Package.IsEmpty())
		{
			Package = Scene->SceneryLevels[0].ToSoftObjectPath().GetLongPackageName();
		}
		if (!OpenEditorLevel(Package))
		{
			return false;
		}
		// Its hero start, or where the scene puts the hero.
		APlayerStart* Start = FindTaggedStart(GetEditorWorld(), *FString::Printf(TEXT("LBAScene_%03d"), Destination.Scene));
		const FLBASceneObjectData& Hero = Scene->Objects[0];
		const FTransform Transform = Start ? Start->GetActorTransform()
			: FTransform(FRotator(0.0, ULBASceneAsset::BetaToActorYaw(Hero.Beta), 0.0), Scene->ToWorld(Hero.Position) + FVector(0.0, 0.0, 100.0));
		FrameViewports(Transform, Start);
		LastPick = { Package, Transform, Destination.Scene };
		return true;
	}

	case ELBATravelKind::TestLevel:
	{
		Package = Destination.Level.IsNull() ? GetEditorPackage() : Destination.Level.ToSoftObjectPath().GetLongPackageName();
		if (!OpenEditorLevel(Package))
		{
			return false;
		}
		UWorld* World = GetEditorWorld();
		APlayerStart* Start = Destination.Station.IsNone() ? FindLevelStart(World) : FindTaggedStart(World, Destination.Station);
		if (Start)
		{
			FrameViewports(Start->GetActorTransform(), Start);
			LastPick = { Package, Start->GetActorTransform(), INDEX_NONE };
		}
		return true;
	}

	case ELBATravelKind::PointOfInterest:
	{
		const FLBAPointOfInterest* Point = LBAPointsOfInterest::Find(Destination.PointOfInterest);
		if (!Point)
		{
			return false;
		}
		if (Point->Scene != INDEX_NONE)
		{
			return OpenInEditor(FLBATravelDestination::ToScene(Point->Scene));
		}
		if (!OpenEditorLevel(Point->Level))
		{
			return false;
		}
		// The spot is in one of the level's sub-levels' coordinates.
		FTransform Transform(FRotator(0.0, Point->Yaw, 0.0), Point->Feet + FVector(0.0, 0.0, 100.0));
		for (const ULevelStreaming* Streaming : GetEditorWorld()->GetStreamingLevels())
		{
			if (Streaming && Streaming->GetWorldAssetPackageName() == Point->Frame)
			{
				Transform = Transform * Streaming->LevelTransform;
			}
		}
		FrameViewports(Transform, nullptr);
		LastPick = { Point->Level, Transform, INDEX_NONE };
		return true;
	}
	}
	return false;
}

bool ULBALevelSelector::GetViewLocation(FVector& OutLocation) const
{
	for (const FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
	{
		if (Client && Client->IsPerspective())
		{
			OutLocation = Client->GetViewLocation();
			return true;
		}
	}
	return false;
}

int32 ULBALevelSelector::GetCurrentScene() const
{
	FVector Location;
	const bool bKnown = GetViewLocation(Location);
	const int32 Picked = LastPick.Package == GetEditorPackage() ? LastPick.Scene : INDEX_NONE;
	return ULBASceneTravelSubsystem::FindSceneAt(GetWorld(), bKnown ? &Location : nullptr, Picked);
}

void ULBALevelSelector::Play()
{
	// From the place picked in this level (Play from Here: ALBAGameMode starts there), else as the Play button does.
	FRequestPlaySessionParams Params;
	if (LastPick.Package == GetEditorPackage())
	{
		Params.StartLocation = LastPick.Start.GetLocation();
		Params.StartRotation = FRotator(0.0, LastPick.Start.Rotator().Yaw, 0.0);
	}
	GEditor->RequestPlaySession(Params);
}
