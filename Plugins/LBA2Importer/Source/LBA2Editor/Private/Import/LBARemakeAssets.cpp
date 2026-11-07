#include "Import/LBARemakeAssets.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Formats/LBAGltf.h"
#include "Formats/LBAIso.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Interfaces/IPluginManager.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBATypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** The remake authored its models at 0.75 brick units per unit. */
	constexpr double ModelScale = 1.0 / 0.75;
	/** Remake world units (WORLD_SCALE = 20 / 0x4000), in which its sprite replacements are authored. */
	constexpr double WorldUnit = 16384.0 / 20.0;

	/** Remake grid space: 1 unit = one brick (512 LBA units) on every axis, Z shifted by one brick. */
	FVector RemakeToUnreal(double X, double Y, double Z, float Scale)
	{
		return LBA::ToUnreal(X * LBA::BrickSizeXZ, Y * LBA::BrickSizeXZ, (Z + 1.0) * LBA::BrickSizeXZ, Scale);
	}

	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			FJsonSerializer::Deserialize(Reader, Object);
		}
		return Object;
	}

	/** Light of a bulb without an emissive color of its own: the remake's own lamplight (AA_point_light_yellow_1W). */
	const FLinearColor IncandescentColor(1.f, 0.754f, 0.323f);
	/** Emissive strength of a lampshade: fabric lit from inside, dimmer than the bulb (M_LBA_Glow's GlowStrength, 3). */
	constexpr float LampshadeGlow = 0.6f;
	/** Emissive strength of glowing water: its rings of color stay visible (at 3 it showed as plain white). */
	constexpr float PoolGlow = 0.8f;
	/** Emissive strength of the cosmodrome's blue glass: stays blue (at 3 it showed as white). */
	constexpr float BeaconGlow = 1.f;

	/** Glowing parts flatter than this (height / width) are lights set into a surface: they glow but give no light. */
	constexpr float MinBeaconHeight = 0.25f;

	enum class ELampPart : uint8 { None, Bulb, Shade, Pool, Beacon, Globe };

	ELampPart GetLampPart(const FLBAGltfPrimitive& Prim, const FLBAGltfMaterial* Material)
	{
		if (Prim.Node.Contains(TEXT("bulb")) || (Material && Material->Name.StartsWith(TEXT("keepMat_light"))))
		{
			return ELampPart::Bulb;
		}
		if (Material && Material->Name.StartsWith(TEXT("keepMat_water")))
		{
			// The Temple of Bu's magic water (its fonts and water piles): rings of pale cyan, glowing.
			return ELampPart::Pool;
		}
		if (Material && Material->Name.StartsWith(TEXT("keepMat_blue_light")))
		{
			// The Temple of Bu's cosmodrome: blue glass domes on pedestals, and studs along its landing floors.
			return ELampPart::Beacon;
		}
		if (Prim.Node == TEXT("crystal_ball"))
		{
			// The sewers', caves' and School of Magic's lamps: glowing blue glass balls on pillars (SE_007).
			return ELampPart::Globe;
		}
		return Material && Material->Name.StartsWith(TEXT("LampTexture")) ? ELampPart::Shade : ELampPart::None;
	}

	struct FLayoutVariant
	{
		int32 Layout = 0;
		int32 NX = 1, NY = 1, NZ = 1;
		TArray<int32> Blocks;
		FString File;
		int32 Orientation = 0;
	};
}

FLBARemakeAssets::FLBARemakeAssets(FLBAImportContext& InContext)
	: Context(InContext)
{
}

bool FLBARemakeAssets::Initialize()
{
	const ULBA2ImporterSettings& Settings = Context.GetSettings();
	if (!Context.GetProjectSettings().bUseRemakeModels)
	{
		return false;
	}
	if (Settings.RemakeDirectory.Path.IsEmpty())
	{
		Context.Warn(TEXT("No lba2remake checkout configured (Remake Directory, or -RemakeDir=); interiors keep the original bricks."));
		return false;
	}
	const FString Root = Settings.RemakeDirectory.Path;
	ModelsDirectory = FPaths::Combine(Root, TEXT("www/models/lba2"));
	LayoutsMetadata = LoadJson(FPaths::Combine(Root, TEXT("www/metadata/lba2/layouts.json")));
	GridsMetadata = LoadJson(FPaths::Combine(Root, TEXT("www/metadata/lba2/grids.json")));
	bAvailable = LayoutsMetadata.IsValid() && FPaths::DirectoryExists(ModelsDirectory);
	auto LoadSpriteReplacements = [this](const FString& Path)
	{
		const TSharedPtr<FJsonObject> Replacements = LoadJson(Path);
		const TSharedPtr<FJsonObject>* Sprites = nullptr;
		if (Replacements.IsValid() && Replacements->TryGetObjectField(TEXT("sprites"), Sprites))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Sprites)->Values)
			{
				const TSharedPtr<FJsonObject> Value = Pair.Value->AsObject();
				FSpriteReplacement Replacement;
				Value->TryGetStringField(TEXT("file"), Replacement.File);
				Value->TryGetBoolField(TEXT("hide"), Replacement.bHide);
				Value->TryGetBoolField(TEXT("door"), Replacement.bDoor);
				if (Replacement.bHide || !Replacement.File.IsEmpty())
				{
					SpriteReplacements.Add(FCString::Atoi(*Pair.Key), Replacement);
				}
				if (Replacement.bDoor)
				{
					DoorModels.Add(Replacement.File);
				}
			}
		}
	};
	LoadSpriteReplacements(FPaths::Combine(Root, TEXT("www/metadata/lba2/model_replacements.json")));
	// The plugin's own models fill in for sprites the remake has none of.
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LBA2Importer")))
	{
		PluginModelsDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Models"));
		LoadSpriteReplacements(FPaths::Combine(PluginModelsDirectory, TEXT("model_replacements.json")));
	}
	if (!bAvailable)
	{
		Context.Warn(FString::Printf(TEXT("lba2remake checkout not found or incomplete at %s; interiors keep the original bricks."), *Root));
	}
	return bAvailable;
}

UStaticMesh* FLBARemakeAssets::GetModelMesh(const FString& SubFolder, const FString& File)
{
	const FString Key = SubFolder / File;
	if (UStaticMesh** Found = MeshCache.Find(Key))
	{
		return *Found;
	}
	UStaticMesh*& Result = MeshCache.Add(Key, nullptr);

	FLBAGltfModel Model;
	FString Error;
	const FString PluginPath = FPaths::Combine(PluginModelsDirectory, SubFolder, File);
	if (!Model.Load(FPaths::FileExists(PluginPath) ? PluginPath : FPaths::Combine(ModelsDirectory, SubFolder, File), Error))
	{
		Context.Warn(FString::Printf(TEXT("Remake model %s: %s"), *Key, *Error));
		return nullptr;
	}

	const float Factor = float((SubFolder == TEXT("sprites") ? WorldUnit : ModelScale * LBA::BrickSizeXZ) * Context.GetScale());
	const bool bDoor = SubFolder == TEXT("sprites") && DoorModels.Contains(File);
	const FString Folder = Context.GetContentPath(TEXT("Remake") / SubFolder);
	const FString TextureFolder = Context.GetContentPath(TEXT("Remake/Textures"));
	FLBAMaterialLibrary& Materials = Context.GetMaterials();

	TArray<UTexture2D*> Textures;
	for (const FLBAGltfImage& Image : Model.Images)
	{
		UTexture2D* Texture = nullptr;
		if (Image.Pixels.Num() > 0)
		{
			const FString Name = TEXT("T_Remake_") + Image.Hash;
			Texture = Context.FindAsset<UTexture2D>(TextureFolder, Name);
			if (!Texture)
			{
				Texture = Context.CreateTexture(TextureFolder, Name, Image.Width, Image.Height, Image.Pixels, true);
			}
		}
		Textures.Add(Texture);
	}

	FLBAMeshBuilder Builder;
	FLBAStaticMeshOptions Options;
	TMap<int32, int32> SectionByMaterial;
	auto SectionFor = [&](int32 MaterialIndex, ELampPart Part) -> int32
	{
		const int32 Key = MaterialIndex * 6 + int32(Part);
		if (const int32* Found = SectionByMaterial.Find(Key))
		{
			return *Found;
		}
		const FLBAGltfMaterial* GltfMaterial = Model.Materials.IsValidIndex(MaterialIndex) ? &Model.Materials[MaterialIndex] : nullptr;
		UTexture2D* Texture = GltfMaterial && Textures.IsValidIndex(GltfMaterial->Image) ? Textures[GltfMaterial->Image] : nullptr;
		const bool bBlend = GltfMaterial && (GltfMaterial->AlphaMode == FLBAGltfMaterial::EAlpha::Blend || GltfMaterial->BaseColor.A < 0.99f);
		UMaterialInterface* Material;
		if (Part != ELampPart::None)
		{
			// Lit lamps glow (opaque: a lampshade is fabric lit from inside, not see-through).
			Material = Texture ? Materials.GetTextured(ELBAMaterial::TexturedGlow, Texture, Context.GetContentPath(TEXT("Remake/Materials")))
				: Materials.GetMaster(ELBAMaterial::Glow);
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
			if ((Part == ELampPart::Pool || Part == ELampPart::Beacon || Part == ELampPart::Globe) && !Texture)
			{
				Instance = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(Context.GetContentPath(TEXT("Remake/Materials")),
					Part == ELampPart::Pool ? TEXT("MI_Remake_PoolGlow") : TEXT("MI_Remake_BeaconGlow"));
				if (Instance)
				{
					UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Material);
					Context.MarkForSave(Instance);
					Material = Instance;
				}
			}
			if (Instance && Part != ELampPart::Bulb)
			{
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("GlowStrength"),
					Part == ELampPart::Shade ? LampshadeGlow : Part == ELampPart::Pool ? PoolGlow : BeaconGlow);
				UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			}
		}
		else if (Texture)
		{
			const ELBAMaterial Kind = bBlend ? ELBAMaterial::TexturedTranslucent
				: GltfMaterial->AlphaMode == FLBAGltfMaterial::EAlpha::Mask ? ELBAMaterial::TexturedMasked : ELBAMaterial::Textured;
			Material = Materials.GetTextured(Kind, Texture, Context.GetContentPath(TEXT("Remake/Materials")));
		}
		else
		{
			Material = Materials.GetMaster(bBlend ? ELBAMaterial::Translucent : bDoor ? ELBAMaterial::ColorClipped : ELBAMaterial::Color);
		}
		FString SlotName = GltfMaterial ? FLBAImportContext::SanitizeName(GltfMaterial->Name) : FString();
		if (SlotName.IsEmpty())
		{
			SlotName = FString::Printf(TEXT("Mat%d"), MaterialIndex);
		}
		const FName Slot(*FString::Printf(TEXT("%s_%d%s"), *SlotName, MaterialIndex, Part != ELampPart::None ? TEXT("_Glow") : TEXT("")));
		if (Part != ELampPart::None)
		{
			// The lamp's light sits inside: its bulb and shade must not block it.
			Options.NoShadowSlots.Add(Slot);
		}
		const int32 Section = Builder.GetSection(Slot, Material);
		SectionByMaterial.Add(Key, Section);
		return Section;
	};

	// Bounds and light color of each lamp part, by node.
	struct FLampPart
	{
		FBox3f Bounds = FBox3f(ForceInit);
		FLinearColor Color = IncandescentColor;
		/** Pools and beacons: sum of their parts' colors. */
		FLinearColor ColorSum = FLinearColor::Transparent;
	};
	TMap<FString, FLampPart> Bulbs, Shades, Pools, Beacons, Globes;

	for (const FLBAGltfPrimitive& Prim : Model.Primitives)
	{
		const FLBAGltfMaterial* PrimMaterial = Model.Materials.IsValidIndex(Prim.Material) ? &Model.Materials[Prim.Material] : nullptr;
		const ELampPart Part = GetLampPart(Prim, PrimMaterial);
		const int32 Section = SectionFor(Prim.Material, Part);
		FLampPart* Lamp = Part == ELampPart::Bulb ? &Bulbs.FindOrAdd(Prim.Node) : Part == ELampPart::Shade ? &Shades.FindOrAdd(Prim.Node)
			: Part == ELampPart::Pool ? &Pools.FindOrAdd(Prim.Node) : Part == ELampPart::Beacon ? &Beacons.FindOrAdd(Prim.Node)
			: Part == ELampPart::Globe ? &Globes.FindOrAdd(Prim.Node) : nullptr;
		const float Emissive = PrimMaterial ? FMath::Max3(PrimMaterial->Emissive.R, PrimMaterial->Emissive.G, PrimMaterial->Emissive.B) : 0.f;
		if (Lamp && Emissive > 0.f)
		{
			Lamp->Color = FLinearColor(PrimMaterial->Emissive.R / Emissive, PrimMaterial->Emissive.G / Emissive, PrimMaterial->Emissive.B / Emissive);
		}
		const FLinearColor Base = Model.Materials.IsValidIndex(Prim.Material) ? Model.Materials[Prim.Material].BaseColor : FLinearColor::White;
		if (Part == ELampPart::Pool || Part == ELampPart::Beacon || Part == ELampPart::Globe)
		{
			// The water and the glass shine in their own colors.
			Lamp->ColorSum += FLinearColor(Base.R, Base.G, Base.B, 1.f);
		}
		auto MakeVertex = [&](uint32 Index)
		{
			FLBAVertex V;
			const FVector3f& P = Prim.Positions[Index];
			// glTF and LBA share the same right-handed Y-up frame, so this is the same axis swap as LBA::ToUnreal.
			V.Position = FVector3f(P.X, P.Z, P.Y) * Factor;
			if (Prim.Normals.IsValidIndex(Index))
			{
				const FVector3f& N = Prim.Normals[Index];
				V.Normal = FVector3f(N.X, N.Z, N.Y);
			}
			if (Prim.UVs.IsValidIndex(Index))
			{
				V.UV = Prim.UVs[Index];
			}
			FVector4f Color(Base.R, Base.G, Base.B, 0.5f);
			if (Prim.Colors.IsValidIndex(Index))
			{
				Color.X *= Prim.Colors[Index].X;
				Color.Y *= Prim.Colors[Index].Y;
				Color.Z *= Prim.Colors[Index].Z;
			}
			V.Color = Color;
			return V;
		};
		for (int32 I = 0; I + 2 < Prim.Indices.Num(); I += 3)
		{
			FLBAVertex A = MakeVertex(Prim.Indices[I]);
			FLBAVertex B = MakeVertex(Prim.Indices[I + 1]);
			FLBAVertex C = MakeVertex(Prim.Indices[I + 2]);
			if (Prim.Normals.Num() == 0)
			{
				A.Normal = B.Normal = C.Normal = FLBAMeshBuilder::FrontNormal(A.Position, B.Position, C.Position).GetSafeNormal();
			}
			// glTF counter-clockwise front faces stay front-facing after the handedness-flipping axis swap.
			Builder.AddTriangle(Section, A, B, C);
			if (Lamp)
			{
				Lamp->Bounds += A.Position;
				Lamp->Bounds += B.Position;
				Lamp->Bounds += C.Position;
			}
		}
	}
	if (Builder.IsEmpty())
	{
		return nullptr;
	}

	// One light per bulb; lamps without a bulb shine from their shade's center.
	TArray<FLamp>& Lamps = LampCache.Add(Key);
	for (const TPair<FString, FLampPart>& Part : Bulbs.Num() > 0 ? Bulbs : Shades)
	{
		if (Part.Value.Bounds.IsValid)
		{
			Lamps.Add({ FVector(Part.Value.Bounds.GetCenter()), Part.Value.Color, Part.Value.Bounds.GetExtent().GetMin() });
		}
	}
	// Pools shine from the middle of their surface, as wide as it; beacons from the top of their glass (its lower half
	// sits in the pedestal's ring, which casts shadows), unless it is set flat into a surface; globes from their center.
	auto AddColored = [&Lamps](const TMap<FString, FLampPart>& Parts, FLamp::EKind Kind)
	{
		for (const TPair<FString, FLampPart>& Part : Parts)
		{
			const FLinearColor& Sum = Part.Value.ColorSum;
			const float Max = FMath::Max3(Sum.R, Sum.G, Sum.B);
			const FVector3f Extent = Part.Value.Bounds.GetExtent();
			const float Width = FMath::Min(Extent.X, Extent.Y);
			if (!Part.Value.Bounds.IsValid || Max <= 0.f || (Kind == FLamp::EKind::Beacon && Extent.Z < MinBeaconHeight * Width))
			{
				continue;
			}
			FVector3f Position = Part.Value.Bounds.GetCenter();
			if (Kind == FLamp::EKind::Beacon)
			{
				Position.Z = Part.Value.Bounds.Max.Z - Extent.Z * 0.5f;
			}
			Lamps.Add({ FVector(Position), FLinearColor(Sum.R / Max, Sum.G / Max, Sum.B / Max),
				Kind == FLamp::EKind::Pool ? Width : Extent.GetMin() * 0.5f, Kind });
		}
	};
	AddColored(Pools, FLamp::EKind::Pool);
	AddColored(Beacons, FLamp::EKind::Beacon);
	AddColored(Globes, FLamp::EKind::Globe);

	FMeshDescription Description;
	TArray<UMaterialInterface*> MeshMaterials;
	TArray<FName> Slots;
	Builder.BuildStatic(Description, MeshMaterials, Slots);
	// A door moves and pawns stand against it: a plain box, not its spikes and studs.
	Options.bComplexAsSimple = !bDoor;
	Options.bSimpleBox = bDoor;
	Result = Context.CreateStaticMesh(Folder, TEXT("SM_Remake_") + FLBAImportContext::SanitizeName(FPaths::GetBaseFilename(File)),
		MoveTemp(Description), MeshMaterials, Slots, Options);
	return Result;
}

void FLBARemakeAssets::ApplyPatches(FLBAGrid& Grid, FGridResult& Out) const
{
	const TSharedPtr<FJsonObject>* GridMeta = nullptr;
	const TSharedPtr<FJsonObject>* Patches = nullptr;
	if (!bAvailable || !GridsMetadata.IsValid() || !GridsMetadata->TryGetObjectField(FString::FromInt(Grid.Index), GridMeta)
		|| !(*GridMeta)->TryGetObjectField(TEXT("patches"), Patches))
	{
		return;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Patch : (*Patches)->Values)
	{
		TArray<FString> Parts;
		Patch.Key.ParseIntoArray(Parts, TEXT("x"));
		if (Parts.Num() != 3)
		{
			continue;
		}
		const FIntVector Cell(FCString::Atoi(*Parts[0]), FCString::Atoi(*Parts[1]), FCString::Atoi(*Parts[2]));
		if (!Grid.IsInside(Cell.X, Cell.Y, Cell.Z))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Value = Patch.Value->AsObject();
		double Layout = -1, Block = 0;
		if (Value->TryGetNumberField(TEXT("layout"), Layout) && Layout >= 0)
		{
			Value->TryGetNumberField(TEXT("block"), Block);
			FLBAGridCell& GridCell = Grid.Cells[FLBAGrid::CellIndex(Cell.X, Cell.Y, Cell.Z)];
			GridCell.Layout = int16(Layout);
			GridCell.Block = uint8(Block);
			Out.PatchedCells.Add(Cell, FIntPoint(int32(Layout), int32(Block)));
		}
		bool bHide = false;
		if (Value->TryGetBoolField(TEXT("hide"), bHide) && bHide)
		{
			Out.HiddenCells.Add(Cell);
		}
	}
}

FLBARemakeAssets::FGridResult FLBARemakeAssets::ProcessGrid(FLBAGrid& Grid, const FLBALibrary& Library)
{
	FGridResult Out;
	if (!bAvailable)
	{
		return Out;
	}
	const float Scale = Context.GetScale();

	// Per-grid patches (grids.json), then extra props.
	ApplyPatches(Grid, Out);
	const TSet<FIntVector> PatchHidden = Out.HiddenCells;
	const TSharedPtr<FJsonObject>* GridMeta = nullptr;
	if (GridsMetadata.IsValid() && GridsMetadata->TryGetObjectField(FString::FromInt(Grid.Index), GridMeta))
	{
		const TArray<TSharedPtr<FJsonValue>>* Models = nullptr;
		if ((*GridMeta)->TryGetArrayField(TEXT("models"), Models))
		{
			for (const TSharedPtr<FJsonValue>& ModelValue : *Models)
			{
				const TSharedPtr<FJsonObject> Model = ModelValue->AsObject();
				const FString Name = Model->GetStringField(TEXT("name"));
				UStaticMesh* Mesh = GetModelMesh(TEXT("layouts"), Name);
				const TSharedPtr<FJsonObject>* Position = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Quat = nullptr;
				if (!Mesh || !Model->TryGetObjectField(TEXT("position"), Position) || !Model->TryGetArrayField(TEXT("quaternion"), Quat) || Quat->Num() != 4)
				{
					continue;
				}
				const FVector Location = RemakeToUnreal((*Position)->GetNumberField(TEXT("x")), (*Position)->GetNumberField(TEXT("y")),
					(*Position)->GetNumberField(TEXT("z")), Scale);
				// Same conversion as Unreal's glTF importer (right-handed Y-up -> left-handed Z-up).
				const FQuat Rotation(-(*Quat)[0]->AsNumber(), -(*Quat)[2]->AsNumber(), -(*Quat)[1]->AsNumber(), (*Quat)[3]->AsNumber());
				Out.Placements.Add({ Mesh, FTransform(Rotation.GetNormalized(), Location), FPaths::GetBaseFilename(Name), GetModelLamps(TEXT("layouts"), Name) });
			}
		}
	}

	// Layout replacements and variants (layouts.json), largest first like the remake.
	TArray<FLayoutVariant> Variants;
	TSet<int32> SuppressedLayouts;
	const TSharedPtr<FJsonObject>* LibraryMeta = nullptr;
	if (LayoutsMetadata->TryGetObjectField(FString::FromInt(Library.Index), LibraryMeta))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*LibraryMeta)->Values)
		{
			const TSharedPtr<FJsonObject> Data = Entry.Value->AsObject();
			if (!Data.IsValid())
			{
				continue;
			}
			FString LayoutPart, Rest;
			const bool bIsVariant = Entry.Key.Split(TEXT(":"), &LayoutPart, &Rest);
			const int32 LayoutIndex = FCString::Atoi(bIsVariant ? *LayoutPart : *Entry.Key);

			bool bFlag = false;
			if (Data->TryGetBoolField(TEXT("suppress"), bFlag) && bFlag)
			{
				SuppressedLayouts.Add(LayoutIndex);
			}
			if (!Data->TryGetBoolField(TEXT("replace"), bFlag) || !bFlag)
			{
				continue;
			}

			FLayoutVariant Variant;
			Variant.Layout = LayoutIndex;
			Variant.File = Data->GetStringField(TEXT("file"));
			double Orientation = 0;
			Data->TryGetNumberField(TEXT("orientation"), Orientation);
			Variant.Orientation = int32(Orientation);
			if (bIsVariant)
			{
				FString Size, Blocks;
				Rest.Split(TEXT(":"), &Size, &Blocks);
				TArray<FString> Dims, BlockParts;
				Size.ParseIntoArray(Dims, TEXT("x"));
				Blocks.ParseIntoArray(BlockParts, TEXT(","));
				if (Dims.Num() != 3)
				{
					continue;
				}
				Variant.NX = FCString::Atoi(*Dims[0]);
				Variant.NY = FCString::Atoi(*Dims[1]);
				Variant.NZ = FCString::Atoi(*Dims[2]);
				for (const FString& B : BlockParts)
				{
					Variant.Blocks.Add(FCString::Atoi(*B));
				}
				// A key may list fewer blocks than its size: the remake reads the missing ones as "any brick".
				const int32 CellCount = Variant.NX * Variant.NY * Variant.NZ;
				while (Variant.Blocks.Num() < CellCount)
				{
					Variant.Blocks.Add(-1);
				}
			}
			else if (Library.Layouts.IsValidIndex(LayoutIndex))
			{
				const FLBALayout& Layout = Library.Layouts[LayoutIndex];
				Variant.NX = Layout.SizeX;
				Variant.NY = Layout.SizeY;
				Variant.NZ = Layout.SizeZ;
				for (const FLBALayoutBlock& Block : Layout.Blocks)
				{
					Variant.Blocks.Add(Block.Brick);
				}
			}
			else
			{
				continue;
			}
			if (Variant.Blocks.Num() == Variant.NX * Variant.NY * Variant.NZ)
			{
				Variants.Add(MoveTemp(Variant));
			}
		}
	}
	Variants.StableSort([](const FLayoutVariant& A, const FLayoutVariant& B) { return A.NX * A.NY * A.NZ > B.NX * B.NY * B.NZ; });

	auto BrickAt = [&Grid, &Library](int32 X, int32 Y, int32 Z, int32& OutLayout) -> int32
	{
		const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
		OutLayout = Cell.Layout;
		if (!Library.Layouts.IsValidIndex(Cell.Layout) || !Library.Layouts[Cell.Layout].Blocks.IsValidIndex(Cell.Block))
		{
			OutLayout = INDEX_NONE;
			return INDEX_NONE;
		}
		return Library.Layouts[Cell.Layout].Blocks[Cell.Block].Brick;
	};

	static const double AngleMapping[4] = { UE_HALF_PI, UE_PI, -UE_HALF_PI, 0.0 };
	for (const FLayoutVariant& Variant : Variants)
	{
		for (int32 Z = 0; Z < 64; ++Z)
		{
			for (int32 X = 0; X < 64; ++X)
			{
				for (int32 Y = 0; Y < 25; ++Y)
				{
					if (Grid.Get(X, Y, Z).Layout != Variant.Layout)
					{
						continue;
					}
					// (X, Y, Z) is the max-x, min-y, max-z corner of the candidate instance.
					bool bMatch = true;
					for (int32 DZ = 0; DZ < Variant.NZ && bMatch; ++DZ)
					{
						for (int32 DX = 0; DX < Variant.NX && bMatch; ++DX)
						{
							for (int32 DY = 0; DY < Variant.NY && bMatch; ++DY)
							{
								const FIntVector Cell(X - DX, Y + DY, Z - DZ);
								const int32 Expected = Variant.Blocks[(Variant.NX - DX - 1) + DY * Variant.NX + (Variant.NZ - DZ - 1) * Variant.NX * Variant.NY];
								if (Expected == -1)
								{
									continue;
								}
								if (!Grid.IsInside(Cell.X, Cell.Y, Cell.Z) || Out.HiddenCells.Contains(Cell) || PatchHidden.Contains(Cell))
								{
									bMatch = false;
									break;
								}
								int32 CellLayout;
								const int32 Brick = BrickAt(Cell.X, Cell.Y, Cell.Z, CellLayout);
								bMatch = CellLayout == Variant.Layout && Brick == Expected;
							}
						}
					}
					if (!bMatch)
					{
						continue;
					}
					TArray<FIntVector> Cells;
					for (int32 DZ = 0; DZ < Variant.NZ; ++DZ)
					{
						for (int32 DX = 0; DX < Variant.NX; ++DX)
						{
							for (int32 DY = 0; DY < Variant.NY; ++DY)
							{
								Out.HiddenCells.Add(FIntVector(X - DX, Y + DY, Z - DZ));
								Cells.Emplace(X - DX, Y + DY, Z - DZ);
							}
						}
					}
					if (UStaticMesh* Mesh = GetModelMesh(TEXT("layouts"), Variant.File))
					{
						const double GX = X - Variant.NX * 0.5 + 1.0;
						const double GY = Y * 0.5;
						const double GZ = (Z - 1) - Variant.NZ * 0.5 + 1.0;
						const double Yaw = -FMath::RadiansToDegrees(AngleMapping[Variant.Orientation & 3]);
						Out.Placements.Add({ Mesh, FTransform(FRotator(0, Yaw, 0), RemakeToUnreal(GX, GY, GZ, Scale)), FPaths::GetBaseFilename(Variant.File),
							GetModelLamps(TEXT("layouts"), Variant.File), MoveTemp(Cells) });
					}
				}
			}
		}
	}

	if (SuppressedLayouts.Num() > 0)
	{
		for (int32 Z = 0; Z < 64; ++Z)
		{
			for (int32 X = 0; X < 64; ++X)
			{
				for (int32 Y = 0; Y < 25; ++Y)
				{
					if (SuppressedLayouts.Contains(Grid.Get(X, Y, Z).Layout))
					{
						Out.HiddenCells.Add(FIntVector(X, Y, Z));
					}
				}
			}
		}
	}
	return Out;
}
