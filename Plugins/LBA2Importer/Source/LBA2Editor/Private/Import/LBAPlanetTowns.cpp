#include "Import/LBAPlanetTowns.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Formats/LBA1.h"
#include "Formats/LBAIso.h"
#include "Import/LBABrickMesh.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"
#include "LBATypes.h"

namespace LBAPlanetTownsPrivate
{
	/** The districts' frame: LBA1's units in cm (/ 8, as LBA2's). */
	constexpr float Scale = 0.125f;
}

int32 LBAPlanetTowns::Build(FLBAImportContext& Context, UWorld* World, const FString& Folder, const FString& Island, const LBAPlanetDistricts::FDistricts& Districts,
	TConstArrayView<LBAPlanetDistricts::FPlaced> Placed, double Radius)
{
	using namespace LBAPlanetTownsPrivate;
	const LBA1::FData& Data = Districts.GetData();
	const FString TownFolder = Folder / TEXT("Towns");

	// The island's towns, and one atlas of all their bricks.
	TArray<LBAPlanetDistricts::FTown> Towns;
	TArray<const LBAPlanetDistricts::FPlaced*> TownPlaces;
	TSet<int32> Used;
	for (const LBAPlanetDistricts::FPlaced& District : Placed)
	{
		LBAPlanetDistricts::FTown Town;
		if (!Districts.MakeTown(District, Town))
		{
			Context.Warn(FString::Printf(TEXT("LBA1's %s on %s: its scenes' grids can't be read"), *District.Name, *Island));
			continue;
		}
		for (const int32 Brick : Town.Bricks)
		{
			if (Brick != 0)
			{
				Used.Add(Brick);
			}
		}
		Towns.Add(MoveTemp(Town));
		TownPlaces.Add(&District);
	}
	if (Towns.IsEmpty())
	{
		return 0;
	}
	TArray<int32> Bricks = Used.Array();
	Bricks.Sort();
	FLBAIsoImporter::FAtlas Atlas;
	TArray<FColor> Pixels;
	LBABrickMesh::MakeAtlas(Bricks, [&Data](int32 Brick, FLBABrickSprite& Sprite) { return Data.LoadBrick(Brick, Sprite); }, Data.GetPalette(), Atlas, Pixels);
	Atlas.Texture = Context.CreateTexture(TownFolder, FString::Printf(TEXT("T_LBA1_%s_Bricks"), *Island), Atlas.Width, Atlas.Height, Pixels, false);
	Atlas.Material = Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Atlas.Texture, TownFolder);
	Atlas.FillMaterial = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
	if (!Atlas.Texture || !Atlas.Material)
	{
		return 0;
	}

	// Per scene (the columns drawn from it): its collision from the bricks' shapes first (the render mesh cooks its own
	// from it), then its cubes.
	struct FPiece
	{
		int32 Town = 0;
		int32 Scene = 0;
		UStaticMesh* Collision = nullptr;
	};
	TArray<FPiece> Pieces;
	auto CellsOf = [](const LBAPlanetDistricts::FTown& Town)
	{
		LBABrickMesh::FCells Cells;
		Cells.Size = Town.Size;
		Cells.Brick = [&Town](int32 X, int32 Y, int32 Z) { return Town.Bricks[Town.Index(X, Y, Z)]; };
		Cells.Shape = [&Town](int32 X, int32 Y, int32 Z) { return int32(Town.Shapes[Town.Index(X, Y, Z)]); };
		return Cells;
	};
	for (int32 TownIndex = 0; TownIndex < Towns.Num(); ++TownIndex)
	{
		const LBAPlanetDistricts::FTown& Town = Towns[TownIndex];
		const LBABrickMesh::FCells Cells = CellsOf(Town);
		for (const int32 Scene : TownPlaces[TownIndex]->Scenes)
		{
			FLBAMeshBuilder Builder;
			LBABrickMesh::EmitCollision(Cells, Builder, Builder.GetSection(TEXT("Collision"), Context.GetMaterials().GetMaster(ELBAMaterial::Invisible)), Scale,
				[&Town, Scene](const FIntVector& Cell) { return Town.Scenes[Cell.Z * Town.Size.X + Cell.X] == Scene; });
			if (Builder.IsEmpty())
			{
				continue;
			}
			FMeshDescription Description;
			TArray<UMaterialInterface*> Materials;
			TArray<FName> Slots;
			Builder.BuildStatic(Description, Materials, Slots);
			FLBAStaticMeshOptions Options;
			Options.bComplexAsSimple = true;
			Options.bNoDistanceField = true;
			Pieces.Add({ TownIndex, Scene, Context.CreateStaticMesh(TownFolder / TEXT("Collision"), FString::Printf(TEXT("SM_LBA1_Scene_%03d_Collision"), Scene),
				MoveTemp(Description), Materials, Slots, Options) });
		}
	}
	Context.FinishPendingBuilds();

	int32 Built = 0;
	for (const FPiece& Piece : Pieces)
	{
		const LBAPlanetDistricts::FTown& Town = Towns[Piece.Town];
		const LBAPlanetDistricts::FPlaced& District = *TownPlaces[Piece.Town];
		FLBAMeshBuilder Builder;
		const int32 Scene = Piece.Scene;
		LBABrickMesh::EmitVisual(CellsOf(Town), Atlas, Builder, Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas.Material), Scale,
			Context.GetProjectSettings().BackFaces, [&Town, Scene](const FIntVector& Cell, int32) { return Town.Scenes[Cell.Z * Town.Size.X + Cell.X] == Scene; });
		if (Builder.IsEmpty())
		{
			continue;
		}
		const int32 Triangles = Builder.NumTriangles();
		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, Materials, Slots);
		FLBAStaticMeshOptions Options;
		Options.ComplexCollisionMesh = Piece.Collision;
		Options.bDeferBuild = false;
		UStaticMesh* Mesh = Context.CreateStaticMesh(TownFolder, FString::Printf(TEXT("SM_LBA1_Scene_%03d"), Scene), MoveTemp(Description), Materials, Slots, Options);
		if (!Mesh)
		{
			continue;
		}
		// Bent onto the planet, it goes down around it: culled by where it is seen, not by where it is.
		Mesh->SetNegativeBoundsExtension(FVector(0.0, 0.0, 2.0 * Radius));
		Mesh->CalculateExtendedBounds();
		Context.MarkForSave(Mesh);
		const FTransform Transform = FTransform(Town.GetOrigin()) * District.FrameToMap;
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, Transform, FString::Printf(TEXT("Town_%s_Scene_%03d"), *Island, Scene), TEXT("Stubs/Towns")))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(Mesh);
			// Brick art is drawn already shaded, as in the interiors (LBA::PredrawnArtStencil).
			Component->SetRenderCustomDepth(true);
			Component->SetCustomDepthStencilValue(LBA::PredrawnArtStencil);
			Actor->Tags.Add(ULBAPlanetAsset::StubTag);
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA1 town %s on %s: scene %d, %d triangles"), *District.Name, *Island, Scene, Triangles);
		++Built;
	}
	Context.Flush();
	return Built;
}
