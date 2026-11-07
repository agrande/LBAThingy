#include "Import/LBAPlanetTowns.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Formats/LBA1.h"
#include "Formats/LBAIso.h"
#include "Formats/LBAXpl.h"
#include "Import/LBABrickMesh.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBAPlanetGround.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"
#include "LBATypes.h"

namespace LBAPlanetTownsPrivate
{
	/** The districts' frame: LBA1's units in cm (/ 8, as LBA2's). */
	constexpr float Scale = 0.125f;
	/** Its cells (cm) across and up. */
	constexpr double CellCm = LBA1::CellSize * Scale;
	constexpr double StepCm = LBA1::CellHeight * Scale;
	/** How far (cells) a town's kinds of ground soften into each other. */
	constexpr int32 KindsSoften = 3;
}

void LBAPlanetTowns::EmitGround(const LBAPlanetDistricts::FDistricts& Districts, TConstArrayView<LBAPlanetDistricts::FPlaced> Placed, const FVector& Pivot, FLBAMeshBuilder& Builder, int32 Section)
{
	using namespace LBAPlanetTownsPrivate;
	for (const LBAPlanetDistricts::FPlaced& District : Placed)
	{
		LBAPlanetDistricts::FTown Town;
		if (!Districts.MakeTown(District, Town))
		{
			continue;
		}
		const LBA1::FData& Data = Districts.GetData();
		auto SharesAt = [&](int32 X, int32 Y, int32 Z) -> const LBAPlanetDistricts::FGroundShares*
		{
			return X >= 0 && Z >= 0 && Y >= 0 && X < Town.Size.X && Y < Town.Size.Y && Z < Town.Size.Z ? Districts.GetGroundShares(Town.Bricks[Town.Index(X, Y, Z)]) : nullptr;
		};
		// A corner's kinds: the ground bricks at its level within KindsSoften cells, the nearer the more. Their outlines lose LBA1's
		// staircases, and the material's noise makes them irregular.
		auto CornerAt = [&](int32 X, int32 Y, int32 Z)
		{
			float Grass = 0.f, Earth = 0.f, Dark = 0.f, Weights = 0.f;
			for (int32 DZ = -KindsSoften; DZ < KindsSoften; ++DZ)
			{
				for (int32 DX = -KindsSoften; DX < KindsSoften; ++DX)
				{
					if (const LBAPlanetDistricts::FGroundShares* Shares = SharesAt(X + DX, Y, Z + DZ))
					{
						const float Weight = float(FMath::Square(KindsSoften) - FMath::Square(DX + 0.5f) * 0.5f - FMath::Square(DZ + 0.5f) * 0.5f);
						Grass += Weight * Shares->Grass;
						Earth += Weight * Shares->Earth;
						Dark += Weight * Shares->Dark;
						Weights += Weight;
					}
				}
			}
			FLBAVertex Vertex;
			const FVector Frame = Town.GetOrigin() + FVector(X * CellCm, Z * CellCm, (Y + 1) * StepCm);
			Vertex.Position = FVector3f(District.FrameToMap.TransformPosition(Frame) - Pivot);
			Vertex.Normal = FVector3f::UpVector;
			const FLinearColor Linear = FLinearColor::FromSRGBColor(FColor(uint8(255.f * Grass / Weights + 0.5f), uint8(255.f * Earth / Weights + 0.5f), 0));
			Vertex.Color = FVector4f(Linear.R, Linear.G, 0.f, 0.f);
			Vertex.UV = FVector2f(Dark / Weights, 0.f);
			return Vertex;
		};
		for (int32 Z = 0; Z < Town.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Town.Size.X; ++X)
			{
				for (int32 Y = 0; Y < Town.Size.Y; ++Y)
				{
					if (!SharesAt(X, Y, Z) || (Y + 1 < Town.Size.Y && Town.Bricks[Town.Index(X, Y + 1, Z)] != 0 && !Data.IsBlankBrick(Town.Bricks[Town.Index(X, Y + 1, Z)])))
					{
						continue;
					}
					const FLBAVertex A = CornerAt(X, Y, Z);
					const FLBAVertex B = CornerAt(X + 1, Y, Z);
					const FLBAVertex C = CornerAt(X, Y, Z + 1);
					const FLBAVertex D = CornerAt(X + 1, Y, Z + 1);
					Builder.AddTriangleFacing(Section, A, B, D, FVector3f::UpVector);
					Builder.AddTriangleFacing(Section, A, D, C, FVector3f::UpVector);
				}
			}
		}
	}
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
	// LBA1's colors moved to the home island's palette (LBAPlanetGround): by ramp, and plants' shadows with their plants.
	const FLBAXpl* HomeXpl = Context.GetXpl(FLBAXpl::GetEntryForIsland(TEXT("CITABAU")));
	const TArray<FColor> Palette = HomeXpl ? LBAPlanetGround::MakeTownPalette(Data.GetPalette(), HomeXpl->Palette) : Data.GetPalette();
	LBABrickMesh::MakeAtlas(Bricks, [&Data](int32 Brick, FLBABrickSprite& Sprite)
	{
		if (!Data.LoadBrick(Brick, Sprite))
		{
			return false;
		}
		LBAPlanetGround::MovePlantShadows(Sprite);
		return true;
	}, Palette, Atlas, Pixels);
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
	auto CellsOf = [&Data](const LBAPlanetDistricts::FTown& Town)
	{
		LBABrickMesh::FCells Cells;
		Cells.Size = Town.Size;
		// A blank brick is only collision: the faces around it are seen.
		Cells.Brick = [&Town, &Data](int32 X, int32 Y, int32 Z)
		{
			const int32 Brick = Town.Bricks[Town.Index(X, Y, Z)];
			return Brick != 0 && !Data.IsBlankBrick(Brick) ? Brick : 0;
		};
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
			Context.GetProjectSettings().BackFaces, [&Town, &Districts, Scene](const FIntVector& Cell, int32)
			{
				return Town.Scenes[Cell.Z * Town.Size.X + Cell.X] == Scene && !Districts.GetGroundShares(Town.Bricks[Town.Index(Cell.X, Cell.Y, Cell.Z)]);
			});
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
