#include "Import/LBATestLevelImporter.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Builders/CubeBuilder.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Editor/UnrealEdTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBAControls.h"
#include "LBAControlsSign.h"
#include "LBAEntityAsset.h"
#include "LBASceneActor.h"
#include "LBASceneAsset.h"
#include "LBASceneTravelSubsystem.h"
#include "LBATestStation.h"
#include "LBATravelGateway.h"
#include "LBAZoneVolume.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "LBATestLevelImporter"

namespace
{
	/** A zone's frame: X ahead of the arriving player, Y to their right, Z up, from their feet. */
	struct FPlace
	{
		FVector Origin;
		double Yaw = 0.0;

		FVector operator()(double X, double Y, double Z) const
		{
			return Origin + FRotator(0.0, Yaw, 0.0).RotateVector(FVector(X, Y, Z));
		}

		FRotator Rotation(double Pitch = 0.0, double LocalYaw = 0.0) const
		{
			return FRotator(Pitch, Yaw + LocalYaw, 0.0);
		}
	};

	/** Places of the game the start leads to (lba2remake's names; scene numbers of SCENE.HQR). */
	struct FPlaceGateway
	{
		int32 Scene;
		const TCHAR* Island;
	};
	const FPlaceGateway StartPlaces[] = {
		{ 0, TEXT("CITABAU") },   // Twinsen's house: interior lights, walk-in doors
		{ 43, TEXT("CITABAU") },  // Citadel harbour: the sea
		{ 15, TEXT("CITABAU") },  // Museum: doors, enemies
		{ 17, TEXT("CITABAU") },  // Sewers: vaults
		{ 44, TEXT("CITABAU") },  // The Dome: a long ladder
		{ 10, TEXT("DESERT") },   // Temple of Bu: vaults, lights
		{ 28, TEXT("DESERT") },   // School of Magic: hollows
		{ 75, TEXT("EMERAUDE") }, // Moon landing zone
	};

	/** The museum: guards and enemies stand in it. */
	constexpr int32 MeleeScene = 15;
	constexpr double StationHalfHeight = 92.0;
	/** Life lost touching one of the arena's enemies (Twinsen has 50). */
	constexpr float EnemyTouchDamage = 10.f;
	/** Life lost every half second on the burning floor. */
	constexpr float HazardDamage = 5.f;

	/** Sign text drawn this much brighter than its vertex color, to read like paint in sunlight. */
	constexpr float SignBrightness = 1.6f;

	const FLinearColor SignTitleWhite(1.f, 1.f, 1.f);
}

FLBATestLevelImporter::FLBATestLevelImporter(FLBAImportContext& InContext)
	: Context(InContext)
{
}

int32 FLBATestLevelImporter::ImportAll()
{
	Context.ReportProgress(LOCTEXT("FeatureLevel", "Building the feature test map"));
	BuildFeatureLevel();
	UE_LOG(LogLBA2, Display, TEXT("Tests: feature test map with %d stations and %d gateways"), NumStations, NumGateways);
	return NumStations > 0 ? 1 : 0;
}

void FLBATestLevelImporter::CreateMaterials()
{
	const FString Folder = Context.GetContentPath(TEXT("Tests/Materials"));

	// Sign text: the engine's text material, unlit and drawn after motion blur, so neither the sun nor the palette
	// shading (which would read it as light on the board behind) changes it.
	if (UMaterial* EngineText = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultTextMaterialTranslucent.DefaultTextMaterialTranslucent")))
	{
		if (UMaterial* Material = Context.DuplicateAsset(EngineText, Folder, TEXT("M_LBA_SignText")))
		{
			UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
			UMaterialExpressionMultiply* Brighter = Cast<UMaterialExpressionMultiply>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -250, 0));
			static_cast<FExpressionInput&>(Brighter->A) = static_cast<const FExpressionInput&>(Data->BaseColor);
			Brighter->ConstB = SignBrightness;
			Data->EmissiveColor.Expression = Brighter;
			Data->EmissiveColor.OutputIndex = 0;
			Data->BaseColor.Expression = nullptr;
			Material->SetShadingModel(MSM_Unlit);
			Material->TranslucencyPass = MTP_AfterMotionBlur;
			Material->bUseTranslucencyVertexFog = false;
			UMaterialEditingLibrary::RecompileMaterial(Material);
			Context.MarkForSave(Material);
			SignText = Material;
		}
	}
	// Controls signs' panels: the engine's widget material, one-sided translucent and drawn after motion blur like the
	// sign text.
	if (UMaterial* EngineWidget = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough")))
	{
		if (UMaterial* Material = Context.DuplicateAsset(EngineWidget, Folder, TEXT("M_LBA_SignPanel")))
		{
			Material->BlendMode = BLEND_Translucent;
			Material->TwoSided = false;
			Material->TranslucencyPass = MTP_AfterMotionBlur;
			Material->bUseTranslucencyVertexFog = false;
			UMaterialEditingLibrary::RecompileMaterial(Material);
			Context.MarkForSave(Material);
			SignPanel = Material;
		}
	}

	// Surfaces: the template's prototype grid in the zone's color, its lines a darker shade.
	UMaterialInterface* GridMaster = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/LevelPrototyping/Materials/M_PrototypeGrid.M_PrototypeGrid"));
	auto Grid = [&](const TCHAR* Name, const FLinearColor& Color, const FLinearColor& Lines, float GridSize) -> UMaterialInterface*
	{
		UMaterialInstanceConstant* Instance = GridMaster ? Context.CreateOrReuseAsset<UMaterialInstanceConstant>(Folder, Name) : nullptr;
		if (!Instance)
		{
			return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, GridMaster);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("SurfaceColor"), Color);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("GridColor"), Lines);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("SubGridColor"), FMath::Lerp(Color, Lines, 0.5f));
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Grid Size"), GridSize);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Context.MarkForSave(Instance);
		return Instance;
	};
	SignBoard = Grid(TEXT("MI_Test_SignBoard"), FLinearColor(0.012f, 0.014f, 0.02f), FLinearColor(0.02f, 0.025f, 0.035f), 400.f);
	Ground = Grid(TEXT("MI_Test_Ground"), FLinearColor(0.30f, 0.30f, 0.28f), FLinearColor(0.16f, 0.16f, 0.15f), 100.f);
	Hub = Grid(TEXT("MI_Test_Hub"), FLinearColor(0.62f, 0.62f, 0.60f), FLinearColor(0.35f, 0.35f, 0.34f), 100.f);
	// Danger: red with wide yellow lines, a stripe every 50 cm.
	Danger = Grid(TEXT("MI_Test_Danger"), FLinearColor(0.55f, 0.03f, 0.01f), FLinearColor(0.85f, 0.6f, 0.02f), 50.f);
	Crate = Grid(TEXT("MI_Test_Crate"), FLinearColor(0.38f, 0.22f, 0.10f), FLinearColor(0.18f, 0.10f, 0.04f), 50.f);
	GatewayPad = Grid(TEXT("MI_Test_Gateway"), FLinearColor(0.10f, 0.55f, 0.60f), FLinearColor(0.75f, 0.95f, 1.f), 50.f);

	auto MakeZone = [&](const TCHAR* Name, const FLinearColor& Color)
	{
		FZone Zone;
		Zone.Color = Color;
		Zone.Material = Grid(*FString::Printf(TEXT("MI_Test_%s"), Name), Color, Color * 0.45f, 100.f);
		Zone.Floor = Grid(*FString::Printf(TEXT("MI_Test_%sFloor"), Name), FMath::Lerp(FLinearColor(0.30f, 0.30f, 0.28f), Color, 0.45f), Color * 0.4f, 100.f);
		return Zone;
	};
	Jumps = MakeZone(TEXT("Jumps"), FLinearColor(0.75f, 0.30f, 0.04f));
	Tower = MakeZone(TEXT("Tower"), FLinearColor(0.12f, 0.55f, 0.12f));
	Arena = MakeZone(TEXT("Arena"), FLinearColor(0.08f, 0.22f, 0.70f));
	Slopes = MakeZone(TEXT("Slopes"), FLinearColor(0.45f, 0.12f, 0.55f));
	Dinofly = MakeZone(TEXT("Dinofly"), FLinearColor(0.80f, 0.65f, 0.05f));
}

void FLBATestLevelImporter::BuildFeatureLevel()
{
	Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	CreateMaterials();
	World = Context.BeginLevel(Context.GetContentPath(TEXT("Tests")), TEXT("L_FeatureTest"));
	if (!World || !Cube)
	{
		return;
	}

	// Citadel's palette and sky, the sun high behind the start: the faces turned to the player are lit.
	AddBlock(FVector(0.0, 0.0, -50.0), FVector(30000.0, 30000.0, 100.0), FRotator::ZeroRotator, Ground, TEXT("Floor"), TEXT("Environment"));
	LBAEnvironment::Spawn(Context, World, FLBAEnvironmentPreset::ForIsland(TEXT("CITABAU")), FRotator(-50.0, 25.0, 0.0));

	// Everything within 50 m of the start, in sight from it: jumps ahead left, the tower ahead right, the arena
	// ahead, the Dinofly's tower behind it on the left, slopes on the right, gateways behind.
	BuildStart();
	BuildJumps(FVector(700.0, -1000.0, 0.0), 0.0);
	BuildTower(FVector(1100.0, 700.0, 0.0), 0.0);
	BuildArena(FVector(2400.0, 0.0, 0.0), 0.0);
	BuildSlopes(FVector(300.0, 1600.0, 0.0), 0.0);
	BuildDinofly(FVector(2000.0, -2700.0, 0.0), 0.0);

	// The editor opens the map looking at the course from behind the start.
	World->EditorViews.SetNum(FMath::Max(World->EditorViews.Num(), int32(LVT_MAX)));
	World->EditorViews[LVT_Perspective] = FLevelViewportInfo(FVector(-900.0, 0.0, 650.0), FRotator(-18.0, 0.0, 0.0), 0.f);

	Context.EndLevel(World);
	World = nullptr;
}

void FLBATestLevelImporter::BuildStart()
{
	const FName Folder(TEXT("Start"));
	Context.SpawnGeneratedActor<APlayerStart>(World, FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, StationHalfHeight)), TEXT("PlayerStart"), Folder);
	AddBlock(FVector(0.0, 0.0, 1.0), FVector(500.0, 500.0, 2.0), FRotator::ZeroRotator, Hub, TEXT("StartPad"), Folder);
	// To the left, out of the view of the course.
	AddControlsSign(FVector(150.0, -650.0, 0.0), FacingStart(FVector(150.0, -650.0, 0.0)), 120.0, LOCTEXT("Title", "LBA2 FEATURE TEST"),
		{
			{ LOCTEXT("StartMove", "Run (tilt the stick a little to walk)"), LBAControls::Move() },
			{ LOCTEXT("StartLook", "Look around"), LBAControls::Look() },
			{ LOCTEXT("StartMenu", "Travel menu: Tests > go to a zone"), LBAControls::Menu() },
		},
		LOCTEXT("TitleBody", "Every feature in sight. Gateways to the game behind you.\nRed floors hurt."),
		SignTitleWhite, Folder, 50.f, 22.f);

	// Gateways behind the start: islands, places of the game, sandboxes.
	TArray<FLBATravelDestination> Islands;
	for (const FLBAIslandInfo& Island : ULBAIslandTravelSubsystem::FindIslands())
	{
		Islands.Add(FLBATravelDestination::ToIsland(Island.Id));
	}
	AddGatewayRows(Islands, FVector(-500.0, -1050.0, 0.0), 180.0, 5, LOCTEXT("Islands", "ISLANDS"), TEXT("Gateways/Islands"));

	TArray<FLBATravelDestination> Places;
	for (const FPlaceGateway& Place : StartPlaces)
	{
		if (ULBASceneTravelSubsystem::FindScene(Place.Scene))
		{
			Places.Add(FLBATravelDestination::ToScene(Place.Scene, Place.Island));
		}
	}
	AddGatewayRows(Places, FVector(-500.0, 300.0, 0.0), 180.0, 4, LOCTEXT("Places", "PLACES"), TEXT("Gateways/Places"));

	TArray<FLBATravelDestination> Sandboxes;
	for (const FLBATestLevelInfo& Level : ULBAIslandTravelSubsystem::FindTestLevels())
	{
		if (!Level.bFeatureLevel)
		{
			Sandboxes.Add(FLBATravelDestination::ToTestLevel(Level.Level));
		}
	}
	AddGatewayRows(Sandboxes, FVector(-500.0, 1600.0, 0.0), 180.0, 5, LOCTEXT("SandboxesHeading", "SANDBOXES"), TEXT("Gateways/Sandboxes"));
	AddStation(TEXT("Gateways"), 60, LOCTEXT("GatewaysName", "Gateways"), LOCTEXT("GatewaysDesc", "Step on a pad: islands, places of the game, sandboxes."),
		FVector(-150.0, 0.0, 0.0), 180.0);
}

void FLBATestLevelImporter::BuildJumps(const FVector& Origin, double Yaw)
{
	const FPlace At{ Origin, Yaw };
	const FName Folder(TEXT("Jumps"));
	AddStation(TEXT("Jumps"), 10, LOCTEXT("JumpsName", "Jumps"),
		LOCTEXT("JumpsDesc", "Gaps of 1, 2 and 3 m over a burning floor, ledges of 50 to 125 cm. Space / A: jump, running jump when moving."),
		Origin, Yaw);
	AddBlock(At(1000.0, 150.0, 1.0), FVector(2400.0, 1100.0, 2.0), At.Rotation(), Jumps.Floor, TEXT("ZoneFloor"), Folder);
	AddControlsSign(At(150.0, -330.0, 0.0), FacingStart(At(150.0, -330.0, 0.0)), 100.0, LOCTEXT("JumpsTitle", "JUMPS"),
		{ { LOCTEXT("JumpsJump", "Jump (running jump when moving)"), LBAControls::Jump() } },
		LOCTEXT("JumpsBody", "The red floor burns"), Jumps.Color, Folder);

	// Steps up to 1 m, platforms over a burning pit, then down.
	for (int32 StepIndex = 1; StepIndex <= 4; ++StepIndex)
	{
		AddBlock(At(250.0 + StepIndex * 100.0, 0.0, StepIndex * 12.5), FVector(100.0, 300.0, StepIndex * 25.0), At.Rotation(), Jumps.Material, FString::Printf(TEXT("Step%d"), StepIndex), Folder);
	}
	struct FSpan { double From, To; };
	const FSpan Platforms[] = { { 700.0, 900.0 }, { 1000.0, 1200.0 }, { 1400.0, 1600.0 }, { 1900.0, 2100.0 } };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Platforms); ++Index)
	{
		const FSpan& Span = Platforms[Index];
		AddBlock(At((Span.From + Span.To) / 2.0, 0.0, 50.0), FVector(Span.To - Span.From, 300.0, 100.0), At.Rotation(), Jumps.Material,
			FString::Printf(TEXT("Platform%d"), Index), Folder);
		if (Index > 0)
		{
			// Only the gaps burn: the platforms stay safe to land on.
			const double Gap = Span.From - Platforms[Index - 1].To;
			AddHazard(At(Span.From - Gap / 2.0, 0.0, 0.0), FVector2D(Gap, 300.0), Yaw, FString::Printf(TEXT("BurningGap%d"), Index), Folder);
			AddText(At(Span.From - Gap / 2.0, 170.0, 140.0), Yaw + 90.0, FText::Format(LOCTEXT("Meters", "{0} m"), FText::AsNumber(FMath::RoundToInt(Gap / 100.0))),
				50.f, SignTitleWhite, Folder);
		}
	}
	AddBlock(At(2200.0, 0.0, 25.0), FVector(200.0, 300.0, 50.0), At.Rotation(), Jumps.Material, TEXT("StepDown"), Folder);

	// Ledges, to find how high a jump reaches.
	const int32 Ledges[] = { 50, 75, 100, 125 };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Ledges); ++Index)
	{
		const double X = 500.0 + Index * 300.0;
		AddBlock(At(X, 500.0, Ledges[Index] / 2.0), FVector(200.0, 200.0, Ledges[Index]), At.Rotation(), Jumps.Material, FString::Printf(TEXT("Ledge_%dcm"), Ledges[Index]), Folder);
		AddText(At(X - 101.0, 500.0, Ledges[Index] / 2.0), Yaw + 180.0, FText::Format(LOCTEXT("Cm", "{0} cm"), FText::AsNumber(Ledges[Index])), 28.f, SignTitleWhite, Folder);
	}
	AddCrates(At(1300.0, -350.0, 0.0), 300.0, 5, 11, Crate, Folder);
}

void FLBATestLevelImporter::BuildTower(const FVector& Origin, double Yaw)
{
	const FPlace At{ Origin, Yaw };
	const FName Folder(TEXT("Tower"));
	AddStation(TEXT("Tower"), 20, LOCTEXT("TowerName", "Tower"),
		LOCTEXT("TowerDesc", "Walk into the ladder to climb 10 m. Jump off: falls hurt from 6 m, kill from 15 m. The balcony is halfway."),
		Origin, Yaw);
	AddBlock(At(500.0, 0.0, 1.0), FVector(1200.0, 1000.0, 2.0), At.Rotation(), Tower.Floor, TEXT("ZoneFloor"), Folder);
	AddSign(At(150.0, 330.0, 0.0), FacingStart(At(150.0, 330.0, 0.0)), 100.0, LOCTEXT("TowerTitle", "TOWER 10 m"),
		LOCTEXT("TowerBody", "Walk into the ladder\nFalls hurt from 6 m, kill from 15 m"), Tower.Color, Folder);

	// The tower, a balcony at 5 m on its right, a diving board out of its back.
	constexpr double Height = 1000.0;
	AddBlock(At(600.0, 0.0, Height / 2.0), FVector(400.0, 400.0, Height), At.Rotation(), Tower.Material, TEXT("Tower"), Folder);
	AddBlock(At(600.0, 330.0, 250.0), FVector(400.0, 260.0, 500.0), At.Rotation(), Tower.Material, TEXT("Balcony_5m"), Folder);
	AddBlock(At(960.0, 0.0, Height - 10.0), FVector(320.0, 60.0, 20.0), At.Rotation(), Danger, TEXT("DivingBoard"), Folder);
	AddText(At(397.0, -130.0, Height - 60.0), Yaw + 180.0, LOCTEXT("TenMeters", "10 m"), 60.f, SignTitleWhite, Folder);
	AddText(At(397.0, 330.0, 440.0), Yaw + 180.0, LOCTEXT("FiveMeters", "5 m"), 50.f, SignTitleWhite, Folder);

	// The ladder on the face turned to the start.
	for (const double Side : { -55.0, 55.0 })
	{
		AddBlock(At(396.0, Side, Height / 2.0), FVector(8.0, 8.0, Height), At.Rotation(), Danger, TEXT("LadderRail"), Folder);
	}
	for (double Z = 30.0; Z < Height; Z += 30.0)
	{
		AddBlock(At(396.0, 0.0, Z), FVector(6.0, 110.0, 6.0), At.Rotation(), Danger, TEXT("LadderRung"), Folder);
	}
	if (ALBALadderVolume* Volume = Context.SpawnGeneratedActor<ALBALadderVolume>(World, FTransform(At.Rotation(), At(340.0, 0.0, Height / 2.0)), TEXT("Ladder"), Folder))
	{
		UCubeBuilder* Builder = NewObject<UCubeBuilder>();
		Builder->X = 120.0;
		Builder->Y = 140.0;
		Builder->Z = Height;
		UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
	}
	AddCrates(At(250.0, -380.0, 0.0), 200.0, 3, 23, Crate, Folder);
}

void FLBATestLevelImporter::BuildArena(const FVector& Origin, double Yaw)
{
	const FPlace At{ Origin, Yaw };
	const FName Folder(TEXT("Arena"));
	// Far enough from the wall to read the board on top of it.
	AddStation(TEXT("Arena"), 30, LOCTEXT("ArenaName", "Arena"),
		LOCTEXT("ArenaDesc", "E / X opens a sliding door. Left mouse / B: attack. The museum's enemies die from one hit, but touching them hurts."),
		At(-350.0, 0.0, 0.0), Yaw);

	// A walled square: the front wall with two sliding doors, low walls around (you see in, you can't jump in).
	constexpr double Front = 200.0, Back = 1200.0, HalfWidth = 500.0;
	constexpr double WallHeight = 300.0, LowWall = 130.0;
	constexpr double DoorWidth = 160.0, DoorHeight = 240.0;
	AddBlock(At((Front + Back) / 2.0, 0.0, 1.0), FVector(Back - Front, 2.0 * HalfWidth, 2.0), At.Rotation(), Arena.Floor, TEXT("ArenaFloor"), Folder);
	const double Doorways[] = { -250.0, 250.0 };
	double From = -HalfWidth;
	for (const double Doorway : Doorways)
	{
		const double To = Doorway - DoorWidth / 2.0;
		AddBlock(At(Front, (From + To) / 2.0, WallHeight / 2.0), FVector(30.0, To - From, WallHeight), At.Rotation(), Arena.Material, TEXT("FrontWall"), Folder);
		AddBlock(At(Front, Doorway, (DoorHeight + WallHeight) / 2.0), FVector(30.0, DoorWidth, WallHeight - DoorHeight), At.Rotation(), Arena.Material, TEXT("Lintel"), Folder);
		From = Doorway + DoorWidth / 2.0;
	}
	AddBlock(At(Front, (From + HalfWidth) / 2.0, WallHeight / 2.0), FVector(30.0, HalfWidth - From, WallHeight), At.Rotation(), Arena.Material, TEXT("FrontWall"), Folder);
	AddBlock(At(Back, 0.0, LowWall / 2.0), FVector(30.0, 2.0 * HalfWidth, LowWall), At.Rotation(), Arena.Material, TEXT("BackWall"), Folder);
	for (const double Side : { -HalfWidth, HalfWidth })
	{
		AddBlock(At((Front + Back) / 2.0, Side, LowWall / 2.0), FVector(Back - Front, 30.0, LowWall), At.Rotation(), Arena.Material, TEXT("SideWall"), Folder);
	}
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Doorways); ++Index)
	{
		ALBASceneDoor* Door = Context.SpawnGeneratedActor<ALBASceneDoor>(World,
			FTransform(At.Rotation(), At(Front, Doorways[Index], DoorHeight / 2.0), FVector(0.2, DoorWidth / 100.0, DoorHeight / 100.0)),
			Index == 0 ? TEXT("Door_Sideways") : TEXT("Door_Up"), Folder);
		if (Door)
		{
			Door->GetSpriteComponent()->SetStaticMesh(Cube);
			Door->GetSpriteComponent()->SetMaterial(0, Danger);
			Door->OpenOffset = Index == 0 ? At.Rotation().RotateVector(FVector(0.0, -DoorWidth - 10.0, 0.0)) : FVector(0.0, 0.0, DoorHeight - 5.0);
			Door->AutoCloseDistance = 500.0;
		}
	}
	AddControlsSign(At(Front - 20.0, 0.0, 0.0), Yaw + 180.0, WallHeight + 10.0, LOCTEXT("ArenaTitle", "ARENA"),
		{
			{ LOCTEXT("ArenaUse", "Open a door"), LBAControls::Use() },
			{ LOCTEXT("ArenaAttack", "Punch, kick"), LBAControls::Attack() },
		},
		LOCTEXT("ArenaBody", "Enemies hurt on touch"), Arena.Color, Folder);
	AddHazard(At(700.0, 0.0, 0.0), FVector2D(200.0, 200.0), Yaw, TEXT("BurningTile"), Folder);
	AddCrates(At(800.0, -300.0, 0.0), 150.0, 3, 37, Crate, Folder);

	// The museum's enemies inside, then two people who aren't. ALBASceneActor decides who is an enemy from the
	// scene's data (the rule is repeated here to pick them).
	const ULBASceneAsset* Scene = ULBASceneTravelSubsystem::FindScene(MeleeScene);
	if (!Scene)
	{
		Context.Warn(TEXT("Tests: no museum scene for the arena (import the Scenes layer)."));
		return;
	}
	TMap<int32, ULBAEntityAsset*> Entities;
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByPath(*Context.GetContentPath(TEXT("Characters/Entities")), Assets, true);
	for (const FAssetData& Asset : Assets)
	{
		if (ULBAEntityAsset* Entity = Cast<ULBAEntityAsset>(Asset.GetAsset()))
		{
			Entities.Add(Entity->EntityIndex, Entity);
		}
	}
	const ULBA2Settings* Settings = GetDefault<ULBA2Settings>();
	TArray<int32> Enemies, Others;
	for (int32 Index = 1; Index < Scene->Objects.Num(); ++Index)
	{
		const FLBASceneObjectData& Object = Scene->Objects[Index];
		ULBAEntityAsset* Entity = Entities.FindRef(Object.Entity);
		if (!Entity || Object.MoveMode == 7 || Object.HasFlag(LBASceneFlags::Sprite) || Object.HasFlag(LBASceneFlags::Invisible)
			|| Settings->HiddenSceneEntities.Contains(TSoftObjectPtr<ULBAEntityAsset>(Entity)) || !Entity->GetBodyMesh(Object.Body))
		{
			continue;
		}
		const bool bEnemy = Object.Armour < 51 && Object.LifePoints > 0 && !Object.HasFlag(LBASceneFlags::NoChoc)
			&& !Settings->NonEnemySceneEntities.Contains(TSoftObjectPtr<ULBAEntityAsset>(Entity));
		(bEnemy ? Enemies : Others).Add(Index);
	}
	// Enemies spread over the arena, the others by the back wall.
	const FVector2D EnemySpots[] = { { 450.0, -250.0 }, { 550.0, 280.0 }, { 900.0, 0.0 }, { 1000.0, -350.0 }, { 750.0, 380.0 }, { 1050.0, 300.0 } };
	const FVector2D OtherSpots[] = { { 1120.0, -150.0 }, { 1120.0, 150.0 } };
	auto Place = [&](int32 Index, const FVector2D& Spot, bool bEnemy)
	{
		const FLBASceneObjectData& Object = Scene->Objects[Index];
		ULBAEntityAsset* Entity = Entities.FindRef(Object.Entity);
		const FVector Feet = At(Spot.X, Spot.Y, 0.0);
		const FString Name = Object.Name.IsEmpty() ? Entity->GetName().RightChop(3) : Object.Name;
		ALBASceneActor* Actor = Context.SpawnGeneratedActor<ALBASceneActor>(World, FTransform(At.Rotation(0.0, 180.0), Feet), Name, Folder);
		if (!Actor)
		{
			return;
		}
		Actor->Scene = const_cast<ULBASceneAsset*>(Scene);
		Actor->ObjectIndex = Index;
		Actor->Entity = Entity;
		Actor->BodyType = Object.Body;
		Actor->AnimType = FMath::Max(Object.Anim, 0);
		Actor->TouchDamage = bEnemy ? EnemyTouchDamage : 0.f;
		Actor->ApplyBody();
		Actor->SetActorLocation(Feet + FVector(0.0, 0.0, Actor->GetOriginHeight()));
	};
	for (int32 SpotIndex = 0; SpotIndex < Enemies.Num() && SpotIndex < UE_ARRAY_COUNT(EnemySpots); ++SpotIndex)
	{
		Place(Enemies[SpotIndex], EnemySpots[SpotIndex], true);
	}
	for (int32 SpotIndex = 0; SpotIndex < Others.Num() && SpotIndex < UE_ARRAY_COUNT(OtherSpots); ++SpotIndex)
	{
		Place(Others[SpotIndex], OtherSpots[SpotIndex], false);
	}
	if (Enemies.IsEmpty())
	{
		Context.Warn(TEXT("Tests: the museum scene has no enemy to place (import the Characters layer)."));
	}
}

void FLBATestLevelImporter::BuildSlopes(const FVector& Origin, double Yaw)
{
	const FPlace At{ Origin, Yaw };
	const FName Folder(TEXT("Slopes"));
	AddStation(TEXT("Slopes"), 40, LOCTEXT("SlopesName", "Slopes"),
		LOCTEXT("SlopesDesc", "Ramps of 20 to 65 degrees: Twinsen walks up to 55, slides back on steeper ones."), Origin, Yaw);
	AddSign(At(150.0, -300.0, 0.0), FacingStart(At(150.0, -300.0, 0.0)), 100.0, LOCTEXT("SlopesTitle", "SLOPES"),
		LOCTEXT("SlopesBody", "Walkable up to 55 degrees"), Slopes.Color, Folder);

	const int32 Angles[] = { 20, 35, 45, 55, 65 };
	constexpr double Length = 500.0;
	AddBlock(At(550.0, 560.0, 1.0), FVector(800.0, 1500.0, 2.0), At.Rotation(), Slopes.Floor, TEXT("ZoneFloor"), Folder);
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Angles); ++Index)
	{
		const double Y = Index * 280.0;
		const double Radians = FMath::DegreesToRadians(double(Angles[Index]));
		// A slab tilted up from the floor at X = 300.
		const FVector Center = At(300.0 + FMath::Cos(Radians) * Length / 2.0, Y, FMath::Sin(Radians) * Length / 2.0 - 10.0);
		AddBlock(Center, FVector(Length, 220.0, 20.0), At.Rotation(Angles[Index]), Slopes.Material, FString::Printf(TEXT("Ramp_%ddeg"), Angles[Index]), Folder);
		AddText(At(230.0, Y, 30.0), Yaw + 180.0, FText::Format(LOCTEXT("Degrees", "{0} deg"), FText::AsNumber(Angles[Index])), 34.f, SignTitleWhite, Folder);
	}
}

void FLBATestLevelImporter::BuildDinofly(const FVector& Origin, double Yaw)
{
	const FPlace At{ Origin, Yaw };
	const FName Folder(TEXT("Dinofly"));
	AddStation(TEXT("Dinofly"), 50, LOCTEXT("DinoflyName", "Dinofly"),
		LOCTEXT("DinoflyDesc", "F / Y: summon and ride the Dinofly. Pump Space / A to climb, hold Shift / X to dive, tap Ctrl / B to brake. Crashes hurt it; a fall from the 25 m tower kills Twinsen."),
		Origin, Yaw);
	AddBlock(At(1200.0, 0.0, 1.0), FVector(2800.0, 2400.0, 2.0), At.Rotation(), Dinofly.Floor, TEXT("ZoneFloor"), Folder);
	AddControlsSign(At(200.0, 300.0, 0.0), FacingStart(At(200.0, 300.0, 0.0)), 100.0, LOCTEXT("DinoflyTitle", "DINOFLY"),
		{
			{ LOCTEXT("DinoflyRide", "Summon and ride, get off"), LBAControls::Dinofly() },
			{ LOCTEXT("DinoflySteer", "Steer"), LBAControls::Move() },
			{ LOCTEXT("DinoflyClimb", "Climb: pump it, faster climbs higher"), LBAControls::DinoflyClimb() },
			{ LOCTEXT("DinoflyDive", "Dive: hold, longer dives steeper"), LBAControls::DinoflyDive() },
			{ LOCTEXT("DinoflyBrake", "Brake: tap to land softly"), LBAControls::DinoflyBrake() },
		},
		LOCTEXT("DinoflyBody", "Crashes hurt. Gateways to the storm behind you."), Dinofly.Color, Folder);

	// A 25 m tower to land on (and fall from), pillars close together to thread, a wall to crash into.
	AddBlock(At(2200.0, -300.0, 1250.0), FVector(600.0, 600.0, 2500.0), At.Rotation(), Dinofly.Material, TEXT("Tower_25m"), Folder);
	AddText(At(1898.0, -300.0, 2400.0), Yaw + 180.0, LOCTEXT("TowerTop", "25 m"), 90.f, SignTitleWhite, Folder);
	const FVector2D Pillars[] = { { 1100.0, 700.0 }, { 1500.0, -900.0 }, { 2900.0, 600.0 }, { 3100.0, -1000.0 }, { 3700.0, -200.0 } };
	for (const FVector2D& Pillar : Pillars)
	{
		AddBlock(At(Pillar.X, Pillar.Y, 600.0), FVector(220.0, 220.0, 1200.0), At.Rotation(), Dinofly.Material, TEXT("Pillar_12m"), Folder);
	}
	AddBlock(At(2600.0, 1100.0, 200.0), FVector(1600.0, 60.0, 400.0), At.Rotation(), Danger, TEXT("CrashWall"), Folder);
	AddBlock(At(700.0, 0.0, 3.0), FVector(500.0, 500.0, 6.0), At.Rotation(), Hub, TEXT("LandingPad"), Folder);
	AddCrates(At(500.0, -600.0, 0.0), 250.0, 4, 51, Crate, Folder);

	// Where the game tries it best, behind the station.
	TArray<FLBATravelDestination> Destinations;
	const TArray<FLBAIslandInfo> Islands = ULBAIslandTravelSubsystem::FindIslands();
	auto HasIsland = [&Islands](FName Id) { return Islands.ContainsByPredicate([Id](const FLBAIslandInfo& Info) { return Info.Id == Id; }); };
	if (HasIsland(TEXT("CITADEL")))
	{
		Destinations.Add(FLBATravelDestination::ToIsland(TEXT("CITADEL")));
	}
	if (ULBASceneTravelSubsystem::FindScene(43))
	{
		Destinations.Add(FLBATravelDestination::ToScene(43, TEXT("CITABAU")));
	}
	if (HasIsland(TEXT("DESERT")))
	{
		Destinations.Add(FLBATravelDestination::ToIsland(TEXT("DESERT")));
	}
	AddGatewayRows(Destinations, At(-350.0, 0.0, 0.0), Yaw + 180.0, 3, LOCTEXT("DinoflyPlaces", "FLY THERE"), *(Folder.ToString() / TEXT("Gateways")));
}

AStaticMeshActor* FLBATestLevelImporter::AddBlock(const FVector& Center, const FVector& Size, const FRotator& Rotation, UMaterialInterface* Material, const FString& Label, const FName& Folder)
{
	AStaticMeshActor* Block = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Rotation, Center, Size / 100.0), Label, Folder);
	if (Block)
	{
		Block->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Block->GetStaticMeshComponent()->SetMaterial(0, Material);
	}
	return Block;
}

void FLBATestLevelImporter::AddHazard(const FVector& Center, const FVector2D& Size, double Yaw, const FString& Label, const FName& Folder)
{
	AddBlock(Center + FVector(0.0, 0.0, 2.0), FVector(Size.X, Size.Y, 4.0), FRotator(0.0, Yaw, 0.0), Danger, Label, Folder);
	// Pain volumes hurt the pawns whose center is inside (their physics volume): up to 1.2 m, above a standing
	// Twinsen's center (78 cm) but below it on anything a step higher.
	ALBAHitVolume* Volume = Context.SpawnGeneratedActor<ALBAHitVolume>(World, FTransform(FRotator(0.0, Yaw, 0.0), Center + FVector(0.0, 0.0, 60.0)), Label + TEXT("_Pain"), Folder);
	if (!Volume)
	{
		return;
	}
	UCubeBuilder* Builder = NewObject<UCubeBuilder>();
	Builder->X = Size.X;
	Builder->Y = Size.Y;
	Builder->Z = 120.0;
	UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
	Volume->Damage = HazardDamage;
	Volume->PainInterval = 0.5f;
	Volume->DamagePerSec = HazardDamage / Volume->PainInterval;
	Volume->bPainCausing = true;
}

void FLBATestLevelImporter::AddSign(const FVector& Foot, double Yaw, double Height, const FText& Title, const FText& Body, const FLinearColor& Color,
	const FName& Folder, float TitleSize, float BodySize)
{
	// Size the board to its text: about 0.6 of the size per title letter, 0.52 per body letter.
	TArray<FString> Lines;
	Body.ToString().ParseIntoArrayLines(Lines);
	int32 Longest = 0;
	for (const FString& Line : Lines)
	{
		Longest = FMath::Max(Longest, Line.Len());
	}
	constexpr double Margin = 28.0;
	const double Width = FMath::Max(Title.ToString().Len() * TitleSize * 0.62, Longest * BodySize * 0.52) + 2.0 * Margin;
	const double BoardHeight = Margin + TitleSize * 1.1 + Lines.Num() * BodySize * 1.25 + Margin;
	const FRotator Rotation(0.0, Yaw, 0.0);
	const FVector Front = Rotation.Vector();
	const FVector Top = Foot + FVector(0.0, 0.0, Height + BoardHeight);
	AddBlock(Foot + FVector(0.0, 0.0, Height + BoardHeight / 2.0) - Front * 8.0, FVector(10.0, Width, BoardHeight), Rotation, SignBoard,
		TEXT("Board_") + FLBAImportContext::SanitizeName(Title.ToString()), Folder);
	if (Height > 0.0)
	{
		AddBlock(Foot + FVector(0.0, 0.0, Height / 2.0) - Front * 8.0, FVector(14.0, 14.0, Height), Rotation, SignBoard, TEXT("Post"), Folder);
	}
	AddText(Top - FVector(0.0, 0.0, Margin + TitleSize * 0.5), Yaw, Title, TitleSize, Color, Folder);
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		AddText(Top - FVector(0.0, 0.0, Margin + TitleSize * 1.1 + (Index + 0.5) * BodySize * 1.25), Yaw, FText::FromString(Lines[Index]), BodySize, FLinearColor::White, Folder);
	}
}

void FLBATestLevelImporter::AddControlsSign(const FVector& Foot, double Yaw, double Height, const FText& Title, TArray<FLBAControlHint> Hints,
	const FText& Notes, const FLinearColor& Color, const FName& Folder, float TitleSize, float TextSize)
{
	ALBAControlsSign* Sign = Context.SpawnGeneratedActor<ALBAControlsSign>(World, FTransform(FRotator(0.0, Yaw, 0.0), Foot),
		TEXT("Sign_") + FLBAImportContext::SanitizeName(Title.ToString()), Folder);
	if (!Sign)
	{
		return;
	}
	Sign->Title = Title;
	Sign->TitleColor = Color;
	Sign->Hints = MoveTemp(Hints);
	Sign->Notes = Notes;
	Sign->PostHeight = Height;
	Sign->TitleSize = TitleSize;
	Sign->TextSize = TextSize;
	Sign->BoardMaterial = SignBoard;
	Sign->PanelMaterial = SignPanel;
	Sign->RerunConstructionScripts();
}

void FLBATestLevelImporter::AddText(const FVector& Location, double Yaw, const FText& Text, float Size, const FLinearColor& Color, const FName& Folder)
{
	if (ATextRenderActor* Actor = Context.SpawnGeneratedActor<ATextRenderActor>(World, FTransform(FRotator(0.0, Yaw, 0.0), Location),
		TEXT("Text_") + FLBAImportContext::SanitizeName(Text.ToString().Left(24)), Folder))
	{
		UTextRenderComponent* Render = Actor->GetTextRender();
		Render->SetText(Text);
		Render->SetHorizontalAlignment(EHTA_Center);
		Render->SetVerticalAlignment(EVRTA_TextCenter);
		Render->SetWorldSize(Size);
		Render->SetTextRenderColor(Color.ToFColor(true));
		if (SignText)
		{
			Render->SetTextMaterial(SignText);
		}
		Render->SetCastShadow(false);
	}
}

ALBATestStation* FLBATestLevelImporter::AddStation(FName Tag, int32 Order, const FText& Name, const FText& Description, const FVector& Feet, double Yaw)
{
	ALBATestStation* Station = Context.SpawnGeneratedActor<ALBATestStation>(World,
		FTransform(FRotator(0.0, Yaw, 0.0), Feet + FVector(0.0, 0.0, StationHalfHeight)), TEXT("Station_") + Tag.ToString(), TEXT("Stations"));
	if (Station)
	{
		Station->PlayerStartTag = Tag;
		Station->DisplayName = Name;
		Station->Description = Description;
		Station->Order = Order;
		// The zones have their own boards.
		Station->bShowSign = false;
		Station->RerunConstructionScripts();
		++NumStations;
	}
	return Station;
}

ALBATravelGateway* FLBATestLevelImporter::AddGateway(const FLBATravelDestination& Destination, const FVector& Feet, double Yaw, const FName& Folder)
{
	const FText Name = Destination.Describe();
	ALBATravelGateway* Gateway = Context.SpawnGeneratedActor<ALBATravelGateway>(World, FTransform(FRotator(0.0, Yaw, 0.0), Feet),
		TEXT("Gateway_") + FLBAImportContext::SanitizeName(Name.ToString()), Folder);
	if (!Gateway)
	{
		return nullptr;
	}
	Gateway->Destination = Destination;
	Gateway->RerunConstructionScripts();
	Gateway->GetPad()->SetMaterial(0, GatewayPad);
	// Its name on a small board above the pad.
	UTextRenderComponent* Sign = Gateway->GetSign();
	Sign->SetWorldSize(24.f);
	Sign->SetRelativeLocation(FVector(0.0, 0.0, 200.0));
	Sign->SetTextRenderColor(FColor::White);
	if (SignText)
	{
		Sign->SetTextMaterial(SignText);
	}
	const double Width = Name.ToString().Len() * 24.0 * 0.52 + 30.0;
	const FVector Front = FRotator(0.0, Yaw, 0.0).Vector();
	AddBlock(Feet + FVector(0.0, 0.0, 200.0) - Front * 8.0, FVector(10.0, Width, 44.0), FRotator(0.0, Yaw, 0.0), SignBoard, TEXT("GatewayBoard"), Folder);
	++NumGateways;
	return Gateway;
}

void FLBATestLevelImporter::AddGatewayRows(const TArray<FLBATravelDestination>& Destinations, const FVector& Origin, double Yaw, int32 PerRow, const FText& Heading, const FName& Folder)
{
	if (Destinations.IsEmpty())
	{
		return;
	}
	// Rows going away along Yaw, pads facing back toward where the player comes from.
	const FPlace At{ Origin, Yaw };
	const int32 Rows = (Destinations.Num() + PerRow - 1) / PerRow;
	for (int32 Index = 0; Index < Destinations.Num(); ++Index)
	{
		const int32 Row = Index / PerRow;
		const int32 InRow = FMath::Min(PerRow, Destinations.Num() - Row * PerRow);
		const double Y = ((Index % PerRow) - (InRow - 1) / 2.0) * 280.0;
		AddGateway(Destinations[Index], At(Row * 330.0, Y, 0.0), Yaw + 180.0, Folder);
	}
	AddSign(At(Rows * 330.0 - 80.0, 0.0, 0.0), Yaw + 180.0, 230.0, Heading, FText::GetEmpty(), FLinearColor(FColor(97, 206, 206)), Folder, 60.f);
}

void FLBATestLevelImporter::AddCrates(const FVector& Center, double Radius, int32 Count, int32 Seed, UMaterialInterface* Material, const FName& Folder)
{
	FRandomStream Random(Seed);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const double Size = Random.FRandRange(60.0, 120.0);
		const FVector Offset(Random.FRandRange(-Radius, Radius), Random.FRandRange(-Radius, Radius), Size / 2.0);
		AddBlock(Center + Offset, FVector(Size), FRotator(0.0, Random.FRandRange(0.0, 90.0), 0.0), Material, TEXT("Crate"), Folder);
	}
}

double FLBATestLevelImporter::FacingStart(const FVector& Location)
{
	return FMath::RadiansToDegrees(FMath::Atan2(-Location.Y, -Location.X));
}

#undef LOCTEXT_NAMESPACE
