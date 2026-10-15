#include "Import/LBAIslandImporter.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Formats/LBABody.h"
#include "Formats/LBAIsland.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBATypes.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialEditingLibrary.h"

#define LOCTEXT_NAMESPACE "LBAIslandImporter"

namespace
{
	constexpr int32 CellsPerCube = 64;

	int32 FloorDiv(int32 A, int32 B)
	{
		return A >= 0 ? A / B : -((-A + B - 1) / B);
	}

	/** Height of a ground vertex in island-global vertex coordinates (sea level outside of sections). */
	int32 GlobalHeight(const FLBAIsland& Island, int32 GX, int32 GZ)
	{
		const int32 CX = FloorDiv(GX, CellsPerCube);
		const int32 CZ = FloorDiv(GZ, CellsPerCube);
		if (const FLBAIslandSection* Section = Island.FindSection(CX, CZ))
		{
			return Section->GetHeight(GX - CX * CellsPerCube, GZ - CZ * CellsPerCube);
		}
		return 0;
	}
}

FLBAIslandImporter::FLBAIslandImporter(FLBAImportContext& InContext)
	: Context(InContext)
{
}

TArray<FString> FLBAIslandImporter::FindIslands() const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Context.GetDataDirectory(), TEXT("*.ILE")), true, false);
	TArray<FString> Names;
	for (const FString& File : Files)
	{
		Names.Add(FPaths::GetBaseFilename(File).ToUpper());
	}
	Names.Sort();
	return Names;
}

int32 FLBAIslandImporter::ImportAll()
{
	const TArray<FString>& Filter = Context.GetSettings().IslandFilter;
	int32 Count = 0;
	for (const FString& Name : FindIslands())
	{
		if (Filter.Num() > 0 && !Filter.ContainsByPredicate([&Name](const FString& F) { return F.Equals(Name, ESearchCase::IgnoreCase); }))
		{
			continue;
		}
		if (Context.IsCancelled())
		{
			break;
		}
		if (ImportIsland(Name))
		{
			++Count;
		}
	}
	return Count;
}

bool FLBAIslandImporter::ImportIsland(const FString& IslandName)
{
	Context.ReportProgress(FText::Format(LOCTEXT("Island", "Importing island {0}"), FText::FromString(IslandName)));

	const FLBAHqrFile* Ile = Context.GetHqr(IslandName + TEXT(".ILE"));
	if (!Ile)
	{
		Context.Warn(FString::Printf(TEXT("Island %s: missing .ILE file"), *IslandName));
		return false;
	}

	FLBAIsland Island;
	if (!Island.Load(*Ile))
	{
		Context.Warn(FString::Printf(TEXT("Island %s: could not parse"), *IslandName));
		return false;
	}

	const FString IslandFolder = Context.GetContentPath(TEXT("Islands") / Island.Name);
	const FString TextureFolder = IslandFolder / TEXT("Textures");
	const FString MaterialFolder = IslandFolder / TEXT("Materials");

	// Ground texture page: palette index 0 is transparent so the vertex color shows through.
	UTexture2D* GroundTexture = Context.CreatePaletteTexture(TextureFolder, FString::Printf(TEXT("T_%s_Ground"), *Island.Name), 256, 256,
		[&Island](int32 X, int32 Y) { return Island.GroundTexture[Y * 256 + X]; }, true, true);
	FLBAMaterialLibrary& Materials = Context.GetMaterials();
	UMaterialInterface* GroundMaterial = Materials.GetTextured(ELBAMaterial::Ground, GroundTexture, MaterialFolder);
	UMaterialInterface* ColorMaterial = Materials.GetMaster(ELBAMaterial::Color);

	TArray<UStaticMesh*> DecorMeshes = BuildDecorMeshes(Island, IslandFolder);

	TArray<UStaticMesh*> GroundMeshes;
	for (const FLBAIslandSection& Section : Island.Sections)
	{
		GroundMeshes.Add(BuildGroundMesh(Island, Section, IslandFolder / TEXT("Ground"), GroundMaterial, ColorMaterial));
	}
	Context.Flush();

	if (Context.GetSettings().bCreateIslandLevels)
	{
		BuildLevel(Island, IslandFolder, GroundMeshes, DecorMeshes);
	}
	return true;
}

TArray<UStaticMesh*> FLBAIslandImporter::BuildDecorMeshes(const FLBAIsland& Island, const FString& IslandFolder)
{
	TArray<UStaticMesh*> Meshes;
	const FLBAHqrFile* Obl = Context.GetHqr(Island.Name + TEXT(".OBL"));
	if (!Obl)
	{
		Context.Warn(FString::Printf(TEXT("Island %s: missing .OBL file, no decor"), *Island.Name));
		return Meshes;
	}

	const FString TextureFolder = IslandFolder / TEXT("Textures");
	const FString MaterialFolder = IslandFolder / TEXT("Materials");
	TMap<FString, UTexture2D*> RegionTextures;
	FLBAMaterialLibrary& Materials = Context.GetMaterials();

	FLBABodyConvertParams Params;
	Params.Scale = Context.GetScale();
	Params.Palette = &Context.GetPalette();
	Params.ColorMaterial = Materials.GetMaster(ELBAMaterial::Color);
	Params.TranslucentMaterial = Materials.GetMaster(ELBAMaterial::Translucent);
	Params.GetTexturedMaterial = [&](const FLBATextureRegion& Region, bool bMasked) -> UMaterialInterface*
	{
		UTexture2D*& Texture = RegionTextures.FindOrAdd(Region.GetKey());
		if (!Texture)
		{
			Texture = Context.CreatePaletteTexture(TextureFolder, FString::Printf(TEXT("T_%s_Obj_%s"), *Island.Name, *Region.GetKey()),
				Region.Width, Region.Height,
				[&Island, Region](int32 X, int32 Y) { return Island.ObjectTexture[((Region.Y + Y) & 255) * 256 + ((Region.X + X) & 255)]; },
				true, true);
		}
		return Materials.GetTextured(bMasked ? ELBAMaterial::TexturedMasked : ELBAMaterial::Textured, Texture, MaterialFolder);
	};

	for (int32 Index = 0; Index < Obl->Num(); ++Index)
	{
		FLBABody Body;
		FLBAMeshBuilder Builder;
		Params.DebugIndex = Index;
		if (!Body.Parse(Obl->GetEntry(Index)) || !LBABodyConverter::Convert(Body, Params, Builder))
		{
			Meshes.Add(nullptr);
			continue;
		}

		FMeshDescription Description;
		TArray<UMaterialInterface*> MeshMaterials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, MeshMaterials, Slots);

		FLBAStaticMeshOptions Options;
		Options.bComplexAsSimple = true;
		Meshes.Add(Context.CreateStaticMesh(IslandFolder / TEXT("Decor"), FString::Printf(TEXT("SM_%s_Decor_%03d"), *Island.Name, Index),
			MoveTemp(Description), MeshMaterials, Slots, Options));
	}
	return Meshes;
}

UStaticMesh* FLBAIslandImporter::BuildGroundMesh(const FLBAIsland& Island, const FLBAIslandSection& Section, const FString& Folder,
	UMaterialInterface* GroundMaterial, UMaterialInterface* ColorMaterial)
{
	const float Scale = Context.GetScale();
	const TArray<FColor>& Palette = Context.GetPalette();
	const int32 BaseX = Section.CubeX * CellsPerCube;
	const int32 BaseZ = Section.CubeZ * CellsPerCube;

	auto VertexNormal = [&](int32 X, int32 Z)
	{
		const int32 GX = BaseX + X;
		const int32 GZ = BaseZ + Z;
		const double DhDx = double(GlobalHeight(Island, GX + 1, GZ) - GlobalHeight(Island, GX - 1, GZ)) / (2.0 * LBA::BrickSizeXZ);
		const double DhDz = double(GlobalHeight(Island, GX, GZ + 1) - GlobalHeight(Island, GX, GZ - 1)) / (2.0 * LBA::BrickSizeXZ);
		return FVector3f(LBA::DirToUnreal(-DhDx, 1.0, -DhDz).GetSafeNormal());
	};

	FLBAMeshBuilder Builder;
	const int32 TexturedSection = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Ground), GroundMaterial);
	const int32 ColoredSection = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color), ColorMaterial);

	for (int32 Z = 0; Z < CellsPerCube; ++Z)
	{
		for (int32 X = 0; X < CellsPerCube; ++X)
		{
			const int32 Cell = (Z * CellsPerCube + X) * 2;
			const int32 Sens = Section.Triangles[Cell].Sens();
			for (int32 TriIndex = 0; TriIndex < 2; ++TriIndex)
			{
				const FLBAGroundTriangle& Tri = Section.Triangles[Cell + TriIndex];
				if (!Tri.IsVisible())
				{
					continue;
				}

				FIntPoint Corners[3];
				FLBAIsland::GetTriangleCorners(Sens, TriIndex, Corners);

				bool bSeaLevel = true;
				FLBAVertex V[3];
				for (int32 K = 0; K < 3; ++K)
				{
					const int32 VX = X + Corners[K].X;
					const int32 VZ = Z + Corners[K].Y;
					const int32 Height = Section.GetHeight(VX, VZ);
					bSeaLevel &= Height == 0;
					V[K].Position = FVector3f(LBA::ToUnreal(VX * LBA::BrickSizeXZ, Height, VZ * LBA::BrickSizeXZ, Scale));
					V[K].Normal = VertexNormal(VX, VZ);
					const float Light = FMath::Clamp(Section.GetIntensity(VX, VZ) / 24.f, 0.f, 1.f);
					const FColor Albedo = Tri.IsColored() ? Palette[Tri.Bank() * 16 + 11] : FColor::Black;
					V[K].Color = LBABodyConverter::ToVertexColor(Albedo, Light);
				}

				// Liquid at sea level is replaced by the level's water plane.
				if (bSeaLevel && Tri.CodeJeu() != 0)
				{
					continue;
				}

				int32 Target = ColoredSection;
				if (Tri.IsTextured())
				{
					const int32 TexBase = Tri.TexIndex() * 3;
					if (Section.TexCoords.IsValidIndex(TexBase + 2))
					{
						for (int32 K = 0; K < 3; ++K)
						{
							V[K].UV = Section.TexCoords[TexBase + K] / 256.f;
						}
						Target = TexturedSection;
					}
				}
				Builder.AddTriangleFacing(Target, V[0], V[1], V[2], FVector3f::UpVector);
			}
		}
	}

	if (Builder.IsEmpty())
	{
		return nullptr;
	}

	FMeshDescription Description;
	TArray<UMaterialInterface*> MeshMaterials;
	TArray<FName> Slots;
	Builder.BuildStatic(Description, MeshMaterials, Slots);

	FLBAStaticMeshOptions Options;
	Options.bComplexAsSimple = true;
	return Context.CreateStaticMesh(Folder, FString::Printf(TEXT("SM_%s_Ground_%02d_%02d"), *Island.Name, Section.CubeX, Section.CubeZ),
		MoveTemp(Description), MeshMaterials, Slots, Options);
}

void FLBAIslandImporter::BuildLevel(const FLBAIsland& Island, const FString& IslandFolder, const TArray<UStaticMesh*>& GroundMeshes,
	const TArray<UStaticMesh*>& DecorMeshes)
{
	UWorld* World = Context.BeginLevel(IslandFolder, TEXT("L_") + Island.Name);
	if (!World)
	{
		return;
	}
	const float Scale = Context.GetScale();

	FBox IslandBounds(ForceInit);
	for (int32 Index = 0; Index < Island.Sections.Num(); ++Index)
	{
		const FLBAIslandSection& Section = Island.Sections[Index];
		const FIntVector Origin = Section.GetOrigin();
		const FVector Location = LBA::ToUnreal(Origin.X, Origin.Y, Origin.Z, Scale);
		IslandBounds += Location;
		IslandBounds += LBA::ToUnreal(Origin.X + LBA::IslandCubeSize, 0, Origin.Z + LBA::IslandCubeSize, Scale);

		if (UStaticMesh* Mesh = GroundMeshes.IsValidIndex(Index) ? GroundMeshes[Index] : nullptr)
		{
			if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(Location),
				FString::Printf(TEXT("Ground_%02d_%02d"), Section.CubeX, Section.CubeZ), TEXT("Ground")))
			{
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			}
		}

		const FName SectionFolder(*FString::Printf(TEXT("Decor/Section_%02d_%02d"), Section.CubeX, Section.CubeZ));
		const FName ConditionalFolder(*FString::Printf(TEXT("Decor/Section_%02d_%02d/Conditional"), Section.CubeX, Section.CubeZ));
		for (int32 DecorIndex = 0; DecorIndex < Section.Decors.Num(); ++DecorIndex)
		{
			const FLBAIslandDecor& Decor = Section.Decors[DecorIndex];
			UStaticMesh* Mesh = DecorMeshes.IsValidIndex(Decor.Body) ? DecorMeshes[Decor.Body] : nullptr;
			if (!Mesh)
			{
				continue;
			}
			const FIntVector P = Origin + Decor.Position;
			const FTransform Transform(LBA::BetaToRotator(Decor.Beta), LBA::ToUnreal(P.X, P.Y, P.Z, Scale));
			const bool bConditional = Decor.ConditionVar != 0;
			AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, Transform,
				FString::Printf(TEXT("Decor_%03d"), Decor.Body), bConditional ? ConditionalFolder : SectionFolder);
			if (Actor)
			{
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				if (bConditional)
				{
					Actor->Tags.Add(*FString::Printf(TEXT("LBA_Var_%d=%d"), Decor.ConditionVar, Decor.ConditionValue));
				}
			}
		}
	}

	// Sea: a large translucent plane at LBA height 0, plus an invisible floor just below so the player can wade.
	const FLBAEnvironmentPreset Preset = FLBAEnvironmentPreset::ForIsland(Island.Name);
	if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
	{
		const FBox SeaBounds = IslandBounds.ExpandBy(FVector(LBA::IslandCubeSize * Scale * 6.0, LBA::IslandCubeSize * Scale * 6.0, 0.0));
		const FVector Center = SeaBounds.GetCenter();
		const FVector Size = SeaBounds.GetSize();
		const FVector PlaneScale(Size.X / 100.0, Size.Y / 100.0, 1.0);

		UMaterialInstanceConstant* WaterMaterial = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(IslandFolder / TEXT("Materials"),
			FString::Printf(TEXT("MI_%s_Water"), *Island.Name));
		if (WaterMaterial)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(WaterMaterial, Context.GetMaterials().GetMaster(ELBAMaterial::Water));
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(WaterMaterial, TEXT("WaterColor"), Preset.WaterColor);
			UMaterialEditingLibrary::UpdateMaterialInstance(WaterMaterial);
			Context.MarkForSave(WaterMaterial);
		}

		if (AStaticMeshActor* Sea = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(FRotator::ZeroRotator, FVector(Center.X, Center.Y, 0.0), PlaneScale),
			TEXT("Sea"), TEXT("Environment")))
		{
			UStaticMeshComponent* Component = Sea->GetStaticMeshComponent();
			Component->SetStaticMesh(Plane);
			Component->SetMaterial(0, WaterMaterial);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCastShadow(false);
		}
		if (AStaticMeshActor* Floor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform(FRotator::ZeroRotator, FVector(Center.X, Center.Y, -60.0), PlaneScale),
			TEXT("SeaFloor"), TEXT("Environment")))
		{
			UStaticMeshComponent* Component = Floor->GetStaticMeshComponent();
			Component->SetStaticMesh(Plane);
			Component->SetVisibility(false);
			Component->SetHiddenInGame(true);
			Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		}
	}

	const FLBAIslandSection& FirstSection = Island.Sections[0];
	LBAEnvironment::Spawn(Context, World, Preset, LBAEnvironment::SunRotationFromLbaAngles(FirstSection.LightAlpha, FirstSection.LightBeta));

	// Player start: highest ground point of the section closest to the island center.
	{
		const FVector Center = IslandBounds.GetCenter();
		const FLBAIslandSection* Best = nullptr;
		double BestDistance = TNumericLimits<double>::Max();
		for (const FLBAIslandSection& Section : Island.Sections)
		{
			const FIntVector O = Section.GetOrigin();
			const FVector SectionCenter = LBA::ToUnreal(O.X + LBA::IslandCubeSize / 2, 0, O.Z + LBA::IslandCubeSize / 2, Scale);
			const double Distance = FVector::DistSquared2D(SectionCenter, Center);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = &Section;
			}
		}
		if (Best)
		{
			int32 BestX = 32, BestZ = 32, BestHeight = Best->GetHeight(32, 32);
			for (int32 Z = 16; Z <= 48; ++Z)
			{
				for (int32 X = 16; X <= 48; ++X)
				{
					if (Best->GetHeight(X, Z) > BestHeight)
					{
						BestHeight = Best->GetHeight(X, Z);
						BestX = X;
						BestZ = Z;
					}
				}
			}
			const FIntVector O = Best->GetOrigin();
			const FVector Location = LBA::ToUnreal(O.X + BestX * LBA::BrickSizeXZ, BestHeight, O.Z + BestZ * LBA::BrickSizeXZ, Scale) + FVector(0, 0, 120);
			Context.SpawnGeneratedActor<APlayerStart>(World, FTransform(Location), TEXT("PlayerStart"), NAME_None);
		}
	}

	Context.EndLevel(World);
}

#undef LOCTEXT_NAMESPACE
