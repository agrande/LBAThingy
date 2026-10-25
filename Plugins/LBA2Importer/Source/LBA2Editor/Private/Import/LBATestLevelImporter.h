#pragma once

#include "CoreMinimal.h"
#include "LBAIslandTravelSubsystem.h"

class AStaticMeshActor;
class ALBATestStation;
class ALBATravelGateway;
class FLBAImportContext;
class UMaterialInterface;
class UStaticMesh;
class UWorld;
struct FLBAControlHint;

/**
 * Tests layer: builds the feature test map, <Content Root>/Tests/L_FeatureTest (ULBA2Settings::FeatureLevel), a tight
 * course seen whole from the start: jumps over a floor that burns, a 10 m ladder tower to fall from, an arena behind
 * sliding doors whose enemies hurt on touch, slopes, and the Dinofly's field around a 25 m tower. Behind the start,
 * gateways (ALBATravelGateway) lead to every island, places of the game and the sandboxes; the Dinofly's to where the
 * game tries it best.
 *
 * Built for reading at a glance: every zone has its own color, dangers are red and yellow, signs are dark boards whose
 * text ignores light and the palette shading (M_LBA_SignText), and the sun lights the faces turned to the start.
 * Each zone has a station (ALBATestStation) the travel menu goes to.
 *
 * Re-importing rebuilds only what the layer generated: actors added by hand stay. Import Characters and Scenes first
 * (enemies come from the museum's scene, gateway signs name imported places).
 */
class FLBATestLevelImporter
{
public:
	explicit FLBATestLevelImporter(FLBAImportContext& InContext);

	/** Builds the test levels; returns how many. */
	int32 ImportAll();

private:
	void BuildFeatureLevel();
	void CreateMaterials();

	void BuildStart();
	void BuildJumps(const FVector& Origin, double Yaw);
	void BuildTower(const FVector& Origin, double Yaw);
	void BuildArena(const FVector& Origin, double Yaw);
	void BuildSlopes(const FVector& Origin, double Yaw);
	void BuildDinofly(const FVector& Origin, double Yaw);

	/** A box, Size in cm around Center. */
	AStaticMeshActor* AddBlock(const FVector& Center, const FVector& Size, const FRotator& Rotation, UMaterialInterface* Material, const FString& Label, const FName& Folder);
	/** A floor that hurts (ALBAHitVolume) with the danger stripes on it, Size around Center at floor level. */
	void AddHazard(const FVector& Center, const FVector2D& Size, double Yaw, const FString& Label, const FName& Folder);
	/**
	 * A dark board on a post with a title in Color and lines of body text, facing Yaw (readable from there), its bottom
	 * Height above Foot (no post when 0).
	 */
	void AddSign(const FVector& Foot, double Yaw, double Height, const FText& Title, const FText& Body, const FLinearColor& Color,
		const FName& Folder, float TitleSize = 70.f, float BodySize = 26.f);
	/** A sign explaining controls with the glyphs of the device in use (ALBAControlsSign), Notes under them. */
	void AddControlsSign(const FVector& Foot, double Yaw, double Height, const FText& Title, TArray<FLBAControlHint> Hints, const FText& Notes,
		const FLinearColor& Color, const FName& Folder, float TitleSize = 70.f, float TextSize = 26.f);
	/** Text that ignores light (M_LBA_SignText), readable from the side Yaw faces. */
	void AddText(const FVector& Location, double Yaw, const FText& Text, float Size, const FLinearColor& Color, const FName& Folder);
	ALBATestStation* AddStation(FName Tag, int32 Order, const FText& Name, const FText& Description, const FVector& Feet, double Yaw);
	ALBATravelGateway* AddGateway(const FLBATravelDestination& Destination, const FVector& Feet, double Yaw, const FName& Folder);
	/** Gateways in rows of PerRow going away along Yaw, facing back, with a sign behind them. */
	void AddGatewayRows(const TArray<FLBATravelDestination>& Destinations, const FVector& Origin, double Yaw, int32 PerRow, const FText& Heading, const FName& Folder);
	/** Crates of a few sizes, around Center within Radius (cm), seeded for the same mess every import. */
	void AddCrates(const FVector& Center, double Radius, int32 Count, int32 Seed, UMaterialInterface* Material, const FName& Folder);

	/** Yaw looking from Location back to the start. */
	static double FacingStart(const FVector& Location);

	FLBAImportContext& Context;
	UWorld* World = nullptr;
	UStaticMesh* Cube = nullptr;
	int32 NumStations = 0;
	int32 NumGateways = 0;

	/** Generated materials (Tests/Materials). */
	UMaterialInterface* SignText = nullptr;
	UMaterialInterface* SignPanel = nullptr;
	UMaterialInterface* SignBoard = nullptr;
	UMaterialInterface* Ground = nullptr;
	UMaterialInterface* Hub = nullptr;
	UMaterialInterface* Danger = nullptr;
	UMaterialInterface* Crate = nullptr;
	UMaterialInterface* GatewayPad = nullptr;
	struct FZone
	{
		FLinearColor Color;
		UMaterialInterface* Material = nullptr;
		UMaterialInterface* Floor = nullptr;
	};
	FZone Jumps, Tower, Arena, Slopes, Dinofly;
};
