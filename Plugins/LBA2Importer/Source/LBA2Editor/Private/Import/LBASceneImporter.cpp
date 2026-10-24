#include "Import/LBASceneImporter.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Builders/CubeBuilder.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "Formats/LBAHqr.h"
#include "Formats/LBAXpl.h"
#include "GameFramework/PlayerStart.h"
#include "Interfaces/IPluginManager.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBAEntityAsset.h"
#include "LBASceneActor.h"
#include "LBASceneAsset.h"
#include "LBATypes.h"
#include "LBAZoneVolume.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "LBASceneImporter"

namespace
{
	constexpr int32 AtlasWidth = 2048;
	constexpr int32 AtlasPad = 2;

	FString ZoneTypeName(ELBAZoneType Type)
	{
		return StaticEnum<ELBAZoneType>()->GetNameStringByValue(int64(Type));
	}

	UClass* ZoneClass(ELBAZoneType Type)
	{
		switch (Type)
		{
		case ELBAZoneType::CubeChange: return ALBACubeChangeVolume::StaticClass();
		case ELBAZoneType::Camera: return ALBACameraZoneVolume::StaticClass();
		case ELBAZoneType::Scenario: return ALBAScenarioZoneVolume::StaticClass();
		case ELBAZoneType::Grid: return ALBAGridZoneVolume::StaticClass();
		case ELBAZoneType::Giver: return ALBAGiverZoneVolume::StaticClass();
		case ELBAZoneType::Message: return ALBAMessageZoneVolume::StaticClass();
		case ELBAZoneType::Ladder: return ALBALadderVolume::StaticClass();
		case ELBAZoneType::Escalator: return ALBAEscalatorVolume::StaticClass();
		case ELBAZoneType::Hit: return ALBAHitVolume::StaticClass();
		case ELBAZoneType::Rail: return ALBARailVolume::StaticClass();
		default: return nullptr;
		}
	}

	FSoftObjectPath LevelPath(const FString& PackageName)
	{
		return FSoftObjectPath(PackageName + TEXT(".") + FPackageName::GetShortName(PackageName));
	}

	/** One face of a sprite's box: corners, and the corners of the visible face whose art it shows (itself or the opposite one). */
	struct FBoxFace
	{
		FIntVector Dir;
		FVector Corners[4];
		FVector Art[4];
	};

	TArray<FBoxFace> GetBoxFaces(const FVector& Min, const FVector& Max)
	{
		auto P = [](double X, double Y, double Z) { return FVector(X, Y, Z); };
		const double X0 = Min.X, Y0 = Min.Y, Z0 = Min.Z, X1 = Max.X, Y1 = Max.Y, Z1 = Max.Z;
		TArray<FBoxFace> Faces;
		// Faces the isometric camera sees: top, +X (right half of the sprite) and +Z (left half); the others mirror them.
		Faces.Add({ { 0, 1, 0 }, { P(X0, Y1, Z0), P(X0, Y1, Z1), P(X1, Y1, Z1), P(X1, Y1, Z0) }, { P(X0, Y1, Z0), P(X0, Y1, Z1), P(X1, Y1, Z1), P(X1, Y1, Z0) } });
		Faces.Add({ { 0, -1, 0 }, { P(X0, Y0, Z0), P(X0, Y0, Z1), P(X1, Y0, Z1), P(X1, Y0, Z0) }, { P(X0, Y1, Z0), P(X0, Y1, Z1), P(X1, Y1, Z1), P(X1, Y1, Z0) } });
		Faces.Add({ { 1, 0, 0 }, { P(X1, Y1, Z0), P(X1, Y1, Z1), P(X1, Y0, Z1), P(X1, Y0, Z0) }, { P(X1, Y1, Z0), P(X1, Y1, Z1), P(X1, Y0, Z1), P(X1, Y0, Z0) } });
		Faces.Add({ { -1, 0, 0 }, { P(X0, Y1, Z0), P(X0, Y1, Z1), P(X0, Y0, Z1), P(X0, Y0, Z0) }, { P(X1, Y1, Z0), P(X1, Y1, Z1), P(X1, Y0, Z1), P(X1, Y0, Z0) } });
		Faces.Add({ { 0, 0, 1 }, { P(X0, Y1, Z1), P(X1, Y1, Z1), P(X1, Y0, Z1), P(X0, Y0, Z1) }, { P(X0, Y1, Z1), P(X1, Y1, Z1), P(X1, Y0, Z1), P(X0, Y0, Z1) } });
		Faces.Add({ { 0, 0, -1 }, { P(X0, Y1, Z0), P(X1, Y1, Z0), P(X1, Y0, Z0), P(X0, Y0, Z0) }, { P(X0, Y1, Z1), P(X1, Y1, Z1), P(X1, Y0, Z1), P(X0, Y0, Z1) } });
		return Faces;
	}
}

FLBASceneImporter::FLBASceneImporter(FLBAImportContext& InContext)
	: Context(InContext)
{
}

void FLBASceneImporter::PlaceInMenu()
{
	MenuPlaces.Reset();
	int32 NextOrder = 0;

	// Resources/CommunityMetadata/lba2_locations.json (Tools/export_locations.py): planets > islands > scenes, each
	// scene with the scenes it leads to. The planets and islands are the runtime's (ULBAIslandTravelSubsystem);
	// a scene's island is its own header's.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LBA2Importer"));
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (Plugin && FFileHelper::LoadFileToString(Json, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/CommunityMetadata/lba2_locations.json"))))
	{
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
	}
	TFunction<void(const TArray<TSharedPtr<FJsonValue>>&, int32)> AddNodes = [&](const TArray<TSharedPtr<FJsonValue>>& Nodes, int32 Parent)
	{
		for (const TSharedPtr<FJsonValue>& Value : Nodes)
		{
			const TSharedPtr<FJsonObject> Node = Value->AsObject();
			int32 Scene = INDEX_NONE;
			if (!Node || !Node->TryGetNumberField(TEXT("scene"), Scene))
			{
				continue;
			}
			FMenuPlace& Place = MenuPlaces.Add(Scene);
			Place.Parent = Parent;
			Place.Order = NextOrder++;
			Place.Name = Node->GetStringField(TEXT("name"));
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if (Node->TryGetArrayField(TEXT("children"), Children))
			{
				AddNodes(*Children, Scene);
			}
		}
	};
	const TArray<TSharedPtr<FJsonValue>>* Planets = nullptr;
	if (Root && Root->TryGetArrayField(TEXT("planets"), Planets))
	{
		for (const TSharedPtr<FJsonValue>& Planet : *Planets)
		{
			const TArray<TSharedPtr<FJsonValue>>* Islands = nullptr;
			if (Planet->AsObject() && Planet->AsObject()->TryGetArrayField(TEXT("islands"), Islands))
			{
				for (const TSharedPtr<FJsonValue>& Island : *Islands)
				{
					const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
					if (Island->AsObject() && Island->AsObject()->TryGetArrayField(TEXT("children"), Children))
					{
						AddNodes(*Children, INDEX_NONE);
					}
				}
			}
		}
	}
	else
	{
		Context.Warn(TEXT("Resources/CommunityMetadata/lba2_locations.json is missing: the travel menu lists only the scenes doors lead to."));
	}

	// Story scenes the tree misses go under the first listed scene with a door to them, after its other scenes.
	// Scenes no door leads to (the sea-travel cube) stay out of the menu.
	TArray<int32> Listed;
	MenuPlaces.GetKeys(Listed);
	Listed.Sort([this](int32 A, int32 B) { return MenuPlaces[A].Order < MenuPlaces[B].Order; });
	for (int32 Index = 0; Index < Listed.Num(); ++Index)
	{
		const FLBAScene* Source = Scenes.Find(Listed[Index]);
		if (!Source)
		{
			continue;
		}
		for (const FLBASceneZoneData& Zone : Source->Zones)
		{
			if (Zone.Type == ELBAZoneType::CubeChange && Zone.Num < LBAScenes::FirstDemoScene && Scenes.Contains(Zone.Num) && !MenuPlaces.Contains(Zone.Num))
			{
				FMenuPlace& Place = MenuPlaces.Add(Zone.Num);
				Place.Parent = Listed[Index];
				Place.Order = 100000 + Zone.Num;
				Place.Name = FString::Printf(TEXT("Scene %d"), Zone.Num);
				Listed.Add(Zone.Num);
			}
		}
	}
}

FString FLBASceneImporter::GetSceneName(int32 SceneIndex) const
{
	const FString Name = FLBAImportContext::SanitizeName(Context.GetCommunityName(TEXT("scenes"), SceneIndex));
	return Name.IsEmpty() ? FString::Printf(TEXT("Scene_%03d"), SceneIndex) : FString::Printf(TEXT("Scene_%03d_%s"), SceneIndex, *Name);
}

FString FLBASceneImporter::GetGridLevelPackage(int32 Grid) const
{
	// Same naming as FLBAIsoImporter::GetGridLevelName: the first scene using the grid names it.
	FString LevelName = FString::Printf(TEXT("L_Grid_%03d"), Grid);
	for (int32 Scene = 0; Scene < SceneMap.Num(); ++Scene)
	{
		if (!SceneMap[Scene].bIsIsland && SceneMap[Scene].SceneryIndex == Grid)
		{
			const FString Name = FLBAImportContext::SanitizeName(Context.GetCommunityName(TEXT("scenes"), Scene));
			if (!Name.IsEmpty())
			{
				LevelName = FString::Printf(TEXT("L_Grid_%03d_%s"), Grid, *Name);
			}
			break;
		}
	}
	return Context.GetContentPath(TEXT("Interiors/Grids/Levels")) / LevelName;
}

TArray<FString> FLBASceneImporter::GetSceneryLevels(const FLBAScene& Scene) const
{
	TArray<FString> Candidates;
	if (Scene.IsExterior())
	{
		for (const FString& Island : LBAScenes::GetIslandFiles(Scene.Island))
		{
			Candidates.Add(Context.GetContentPath(TEXT("Islands") / Island) / (TEXT("L_") + Island));
		}
	}
	else if (SceneMap.IsValidIndex(Scene.Index) && !SceneMap[Scene.Index].bIsIsland)
	{
		Candidates.Add(GetGridLevelPackage(SceneMap[Scene.Index].SceneryIndex));
	}
	TArray<FString> Levels;
	for (const FString& Candidate : Candidates)
	{
		if (FPackageName::DoesPackageExist(Candidate))
		{
			Levels.Add(Candidate);
		}
	}
	return Levels;
}

bool FLBASceneImporter::IsSameIslandCubeChange(const FLBAScene& Scene, const FLBASceneZoneData& Zone) const
{
	if (Zone.Type != ELBAZoneType::CubeChange || !Scene.IsExterior())
	{
		return false;
	}
	const FLBAScene* Target = Scenes.Find(Zone.Num);
	return Target && Target->IsExterior() && Target->Island == Scene.Island;
}

void FLBASceneImporter::LoadEntities()
{
	const FString Path = Context.GetContentPath(TEXT("Characters/Entities"));
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	Registry.ScanPathsSynchronous({ Path }, true);
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(*Path, Assets, true);
	for (const FAssetData& Asset : Assets)
	{
		if (ULBAEntityAsset* Entity = Cast<ULBAEntityAsset>(Asset.GetAsset()))
		{
			Entities.Add(Entity->EntityIndex, Entity);
		}
	}
	if (Entities.IsEmpty())
	{
		Context.Warn(TEXT("Scenes: no entity assets found; import the Characters layer first so scene objects get their bodies."));
	}
}

TArray<int32> FLBASceneImporter::GetObjectSprites(const FLBASceneObjectData& Object) const
{
	TArray<int32> Keys;
	if (!Object.HasFlag(LBASceneFlags::Sprite) || Object.Sprite < 0)
	{
		return Keys;
	}
	if (Object.HasFlag(LBASceneFlags::AnimatedSprite))
	{
		if (SpriteAnims.IsValidIndex(Object.SpriteAnim))
		{
			const FIntPoint Range = SpriteAnims[Object.SpriteAnim];
			for (int32 Frame = Range.X; Frame <= Range.Y; ++Frame)
			{
				Keys.Add(SpriteKey(ELBASpriteBank::Anim, Frame));
			}
		}
		if (Keys.IsEmpty())
		{
			Keys.Add(SpriteKey(ELBASpriteBank::Anim, Object.Sprite));
		}
		return Keys;
	}
	Keys.Add(SpriteKey(Object.Sprite >= 100 ? ELBASpriteBank::Sprites : ELBASpriteBank::Raw, Object.Sprite));
	return Keys;
}

void FLBASceneImporter::BuildSprites(const TSet<int32>& Keys)
{
	const FLBAHqrFile* Banks[3] = { Context.GetHqr(TEXT("SPRITES.HQR")), Context.GetHqr(TEXT("SPRIRAW.HQR")), Context.GetHqr(TEXT("ANIM3DS.HQR")) };
	const FLBAHqrFile* Ress = Context.GetHqr(TEXT("RESS.HQR"));
	if (!Ress)
	{
		return;
	}
	const TArray<FLBASpriteBox> Boxes[3] = { LBASprites::LoadBoxes(Ress->GetEntry(5)), LBASprites::LoadBoxes(Ress->GetEntry(8)), LBASprites::LoadBoxes(Ress->GetEntry(43)) };

	struct FImage
	{
		int32 Key = 0;
		FLBASprite Sprite;
		FLBASpriteBox Box;
		FIntPoint Cell = FIntPoint::ZeroValue;
	};
	TArray<FImage> Images;
	for (int32 Key : Keys)
	{
		const int32 Bank = Key >> 16;
		const int32 Index = Key & 0xFFFF;
		FImage Image;
		Image.Key = Key;
		if (!Banks[Bank] || !Image.Sprite.Decode(Banks[Bank]->GetEntry(Index), ELBASpriteBank(Bank) != ELBASpriteBank::Raw))
		{
			Context.Warn(FString::Printf(TEXT("Sprite %d of bank %d is unreadable"), Index, Bank));
			continue;
		}
		Image.Box = Boxes[Bank].IsValidIndex(Index) ? Boxes[Bank][Index] : FLBASpriteBox();
		Images.Add(MoveTemp(Image));
	}
	if (Images.IsEmpty())
	{
		return;
	}

	// Shelf packing, tallest first.
	Images.Sort([](const FImage& A, const FImage& B) { return A.Sprite.Height != B.Sprite.Height ? A.Sprite.Height > B.Sprite.Height : A.Key < B.Key; });
	FIntPoint Cursor(0, 0);
	int32 ShelfHeight = 0;
	for (FImage& Image : Images)
	{
		const FIntPoint Size(Image.Sprite.Width + AtlasPad * 2, Image.Sprite.Height + AtlasPad * 2);
		if (Cursor.X + Size.X > AtlasWidth)
		{
			Cursor = FIntPoint(0, Cursor.Y + ShelfHeight);
			ShelfHeight = 0;
		}
		Image.Cell = Cursor + FIntPoint(AtlasPad, AtlasPad);
		Cursor.X += Size.X;
		ShelfHeight = FMath::Max(ShelfHeight, Size.Y);
	}
	const FIntPoint AtlasSize(AtlasWidth, int32(FMath::RoundUpToPowerOfTwo(Cursor.Y + ShelfHeight)));

	const TArray<FColor>& Palette = Context.GetPalette();
	TArray<FColor> Pixels;
	Pixels.SetNumZeroed(AtlasSize.X * AtlasSize.Y);
	for (const FImage& Image : Images)
	{
		for (int32 Y = 0; Y < Image.Sprite.Height; ++Y)
		{
			for (int32 X = 0; X < Image.Sprite.Width; ++X)
			{
				const uint8 ColorIndex = Image.Sprite.Pixels[Y * Image.Sprite.Width + X];
				FColor Color = Palette[ColorIndex];
				Color.A = ColorIndex == 0 ? 0 : 255;
				Pixels[(Image.Cell.Y + Y) * AtlasSize.X + Image.Cell.X + X] = Color;
			}
		}
	}

	const FString Folder = Context.GetContentPath(TEXT("Sprites"));
	UTexture2D* Texture = Context.CreateTexture(Folder, TEXT("T_Sprites"), AtlasSize.X, AtlasSize.Y, Pixels, false);
	UMaterialInterface* Material = Texture ? Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Texture, Folder) : nullptr;
	if (!Material)
	{
		return;
	}
	for (const FImage& Image : Images)
	{
		if (UStaticMesh* Mesh = BuildSpriteMesh(Image.Key, Image.Sprite, Image.Box, Image.Cell, AtlasSize, Material))
		{
			SpriteMeshes.Add(Image.Key, Mesh);
		}
	}
	Context.Flush();
}

UStaticMesh* FLBASceneImporter::BuildSpriteMesh(int32 Key, const FLBASprite& Sprite, const FLBASpriteBox& Box, FIntPoint Cell, FIntPoint AtlasSize, UMaterialInterface* Material)
{
	const float Scale = Context.GetScale();
	const FVector2D Anchor(Box.ScreenOffset.X + Sprite.HotX, Box.ScreenOffset.Y + Sprite.HotY);
	auto UV = [&](const FVector& Lba)
	{
		const FVector2D Pixel = LBASprites::Project(Lba) - Anchor;
		return FVector2f((Cell.X + Pixel.X) / AtlasSize.X, (Cell.Y + Pixel.Y) / AtlasSize.Y);
	};

	FLBAMeshBuilder Builder;
	const int32 Section = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Material);
	auto AddQuad = [&](const FVector Corners[4], const FVector Art[4], const FVector3f& Normal)
	{
		FLBAVertex V[4];
		for (int32 K = 0; K < 4; ++K)
		{
			V[K].Position = FVector3f(LBA::ToUnreal(Corners[K], Scale));
			V[K].Normal = Normal;
			V[K].Color = FVector4f(0.5f, 0.5f, 0.5f, 0.5f);
			V[K].UV = UV(Art[K]);
		}
		if (FLBAMeshBuilder::FrontNormal(V[0].Position, V[1].Position, V[2].Position).SizeSquared() <= UE_SMALL_NUMBER)
		{
			return;
		}
		Builder.AddTriangleFacing(Section, V[0], V[1], V[2], Normal);
		Builder.AddTriangleFacing(Section, V[0], V[2], V[3], Normal);
	};

	const bool bHasBox = !Box.IsEmpty() && Box.Max.Y > Box.Min.Y;
	if (bHasBox)
	{
		for (const FBoxFace& Face : GetBoxFaces(FVector(Box.Min), FVector(Box.Max)))
		{
			AddQuad(Face.Corners, Face.Art, FVector3f(LBA::DirToUnreal(Face.Dir.X, Face.Dir.Y, Face.Dir.Z)));
		}
	}
	else
	{
		// No box: a card across the isometric view (along x - z, upright), showing the sprite exactly from that angle.
		const FVector Across = FVector(1.0, 0.0, -1.0) * (512.0 / 48.0);
		const FVector Up = FVector(0.0, 1.0, 0.0) * (256.0 / 15.0);
		const double X0 = Anchor.X, X1 = Anchor.X + Sprite.Width, Y0 = Anchor.Y, Y1 = Anchor.Y + Sprite.Height;
		const FVector Corners[4] = { Across * X0 - Up * Y0, Across * X1 - Up * Y0, Across * X1 - Up * Y1, Across * X0 - Up * Y1 };
		const FVector3f Normal = FVector3f(LBA::DirToUnreal(1.0, 0.0, 1.0).GetSafeNormal());
		AddQuad(Corners, Corners, Normal);
		const FVector3f Back = -Normal;
		AddQuad(Corners, Corners, Back);
	}
	if (Builder.IsEmpty())
	{
		return nullptr;
	}

	FMeshDescription Description;
	TArray<UMaterialInterface*> Materials;
	TArray<FName> Slots;
	Builder.BuildStatic(Description, Materials, Slots);
	FLBAStaticMeshOptions Options;
	if (bHasBox)
	{
		const FVector A = LBA::ToUnreal(FVector(Box.Min), Scale);
		const FVector B = LBA::ToUnreal(FVector(Box.Max), Scale);
		const FBox Collision(A.ComponentMin(B), A.ComponentMax(B));
		Options.SimpleCollision = [Collision](FKAggregateGeom& Geom)
		{
			const FVector Size = Collision.GetSize().ComponentMax(FVector(1.0));
			FKBoxElem Elem(Size.X, Size.Y, Size.Z);
			Elem.Center = Collision.GetCenter();
			Geom.BoxElems.Add(Elem);
		};
	}
	else
	{
		Options.bNoCollision = true;
	}

	static const TCHAR* Prefixes[3] = { TEXT("SM_Sprite"), TEXT("SM_SpriteRaw"), TEXT("SM_SpriteAnim") };
	const FString Name = FString::Printf(TEXT("%s_%03d"), Prefixes[Key >> 16], Key & 0xFFFF);
	return Context.CreateStaticMesh(Context.GetContentPath(TEXT("Sprites")), Name, MoveTemp(Description), Materials, Slots, Options);
}

int32 FLBASceneImporter::ImportAll()
{
	const FLBAHqrFile* SceneFile = Context.GetHqr(TEXT("SCENE.HQR"));
	if (!SceneFile)
	{
		Context.Warn(TEXT("SCENE.HQR not found"));
		return 0;
	}
	Context.UsePalette(FLBAXpl::InteriorEntry);

	if (const FLBAHqrFile* Bkg = Context.GetHqr(TEXT("LBA_BKG.HQR")))
	{
		FLBABkgHeader Header;
		if (Header.Load(*Bkg))
		{
			SceneMap = LBAIso::LoadSceneMap(Bkg->GetEntry(Header.GetSceneMapIndex()));
		}
	}
	if (const FLBAHqrFile* Anims = Context.GetHqr(TEXT("ANIM3DS.HQR")))
	{
		// The last entry lists the animations: 4-character name, first frame, last frame.
		FLBAReader R(Anims->GetEntry(Anims->Num() - 1));
		for (int32 At = 0; At + 8 <= R.GetSize(); At += 8)
		{
			SpriteAnims.Add(FIntPoint(R.S16At(At + 4), R.S16At(At + 6)));
		}
	}

	// Parse every scene first: zones need their target's header, sprites are packed into one atlas.
	const ULBA2ImporterSettings& Settings = Context.GetSettings();
	TArray<int32> Selected;
	for (int32 Index = 0; Index + 1 < SceneFile->Num(); ++Index)
	{
		FLBAScene Scene;
		if (!Scene.Load(SceneFile->GetEntry(Index + 1), Index))
		{
			Context.Warn(FString::Printf(TEXT("Scene %d is unreadable"), Index));
			continue;
		}
		Scenes.Add(Index, MoveTemp(Scene));
		const bool bDemo = Index >= LBAScenes::FirstDemoScene;
		if ((Settings.SceneFilter.Num() == 0 && (!bDemo || Context.GetProjectSettings().bImportDemoScenes)) || Settings.SceneFilter.Contains(Index))
		{
			Selected.Add(Index);
		}
	}

	LoadEntities();
	PlaceInMenu();

	TSet<int32> SpriteKeys;
	for (int32 Index : Selected)
	{
		for (const FLBASceneObjectData& Object : Scenes[Index].Objects)
		{
			SpriteKeys.Append(GetObjectSprites(Object));
		}
	}
	Context.ReportProgress(LOCTEXT("Sprites", "Building sprites"));
	BuildSprites(SpriteKeys);

	// Scenes, then their sub-level entries per scenery level. Two exterior scenes on the same cube (the sea-travel
	// scene, demo copies): the first one is loaded with the island, the others wait unloaded.
	TMap<FString, TArray<FStreamedScene>> Streamed;
	TSet<FIntVector> TakenCubes;
	int32 Count = 0;
	for (int32 Index : Selected)
	{
		if (Context.IsCancelled())
		{
			break;
		}
		const FLBAScene& Scene = Scenes[Index];
		Context.ReportProgress(FText::Format(LOCTEXT("Scene", "Importing scene {0}"), FText::FromString(GetSceneName(Index))), 10.f / FMath::Max(Selected.Num(), 1));
		const TArray<FString> SceneryLevels = GetSceneryLevels(Scene);
		if (SceneryLevels.IsEmpty())
		{
			Context.Warn(FString::Printf(TEXT("Scene %d: its %s level is not imported; the scene level is created but not streamed anywhere."),
				Index, Scene.IsExterior() ? TEXT("island") : TEXT("interior")));
		}
		FString LevelPackage;
		if (!ImportScene(Scene, SceneryLevels, LevelPackage))
		{
			continue;
		}
		++Count;
		if (LevelPackage.IsEmpty())
		{
			continue;
		}
		bool bLoaded = Index < LBAScenes::FirstDemoScene;
		if (Scene.IsExterior())
		{
			bool bAlreadyTaken = false;
			TakenCubes.Add(FIntVector(Scene.Island, Scene.CubeX, Scene.CubeY), &bAlreadyTaken);
			bLoaded = bLoaded && !bAlreadyTaken;
		}
		for (const FString& SceneryLevel : SceneryLevels)
		{
			Streamed.FindOrAdd(SceneryLevel).Add({ LevelPackage, Index, bLoaded });
		}
	}

	for (const TPair<FString, TArray<FStreamedScene>>& Pair : Streamed)
	{
		AddStreamedScenes(Pair.Key, Pair.Value);
	}
	UE_LOG(LogLBA2, Display, TEXT("Scenes: %d objects with a 3D body placed (%d without a mesh for their body type), %d sprite meshes"),
		NumObjects, NumMissingBodies, SpriteMeshes.Num());
	return Count;
}

ULBASceneAsset* FLBASceneImporter::ImportScene(const FLBAScene& Scene, const TArray<FString>& SceneryLevels, FString& OutLevelPackage)
{
	const FString Name = GetSceneName(Scene.Index);
	const FString Folder = Context.GetContentPath(TEXT("Scenes"));
	ULBASceneAsset* Asset = Context.CreateOrReuseAsset<ULBASceneAsset>(Folder, TEXT("DA_") + Name);
	if (!Asset)
	{
		return nullptr;
	}
	const float Scale = Context.GetScale();
	Asset->SceneIndex = Scene.Index;
	Asset->DisplayName = Context.GetCommunityName(TEXT("scenes"), Scene.Index);
	Asset->bExterior = Scene.IsExterior();
	Asset->Island = Scene.Island;
	Asset->Cube = FIntPoint(Scene.CubeX, Scene.CubeY);
	Asset->Grid = !Scene.IsExterior() && SceneMap.IsValidIndex(Scene.Index) ? SceneMap[Scene.Index].SceneryIndex : INDEX_NONE;
	Asset->bDemo = Scene.Index >= LBAScenes::FirstDemoScene;
	const FMenuPlace* MenuPlace = Asset->bDemo ? nullptr : MenuPlaces.Find(Scene.Index);
	Asset->MenuParent = MenuPlace ? MenuPlace->Parent : INDEX_NONE;
	Asset->MenuOrder = MenuPlace ? MenuPlace->Order : INDEX_NONE;
	Asset->MenuName = MenuPlace ? MenuPlace->Name : FString();
	Asset->WorldScale = Scale;
	Asset->Origin = Scene.IsExterior() ? LBA::ToUnreal(Scene.CubeX * LBA::IslandCubeSize, 0, Scene.CubeY * LBA::IslandCubeSize, Scale) : FVector::ZeroVector;
	Asset->Ambience = Scene.Ambience;
	Asset->Objects = Scene.Objects;
	for (int32 Index = 0; Index < Asset->Objects.Num(); ++Index)
	{
		const FString ObjectName = Context.GetCommunitySceneName(TEXT("sceneActors"), Scene.Index, Index);
		if (!ObjectName.IsEmpty())
		{
			Asset->Objects[Index].Name = ObjectName;
		}
	}
	Asset->Zones = Scene.Zones;
	Asset->Points = Scene.Points;
	Asset->SceneryLevels.Reset();
	for (const FString& Level : SceneryLevels)
	{
		Asset->SceneryLevels.Add(TSoftObjectPtr<UWorld>(LevelPath(Level)));
	}

	const FString LevelName = TEXT("L_") + Name;
	if (UWorld* World = Context.BeginLevel(Folder, LevelName))
	{
		OutLevelPackage = Folder / LevelName;
		Asset->Level = TSoftObjectPtr<UWorld>(LevelPath(OutLevelPackage));
		PopulateLevel(World, Scene, Asset);
		Context.MarkForSave(Asset);
		Context.EndLevel(World);
	}
	else
	{
		Context.MarkForSave(Asset);
	}
	return Asset;
}

void FLBASceneImporter::PopulateLevel(UWorld* World, const FLBAScene& Scene, ULBASceneAsset* Asset)
{
	// Hero start, tagged so the game mode can tell scene starts apart.
	if (APlayerStart* Start = Context.SpawnGeneratedActor<APlayerStart>(World,
		FTransform(Asset->ToWorld(Scene.Objects[0].Position) + FVector(0.0, 0.0, 95.0)), TEXT("HeroStart"), TEXT("Scene")))
	{
		Start->PlayerStartTag = *FString::Printf(TEXT("LBAScene_%03d"), Scene.Index);
	}

	for (int32 Index = 1; Index < Scene.Objects.Num(); ++Index)
	{
		const FLBASceneObjectData& Object = Scene.Objects[Index];
		// Object 1 is the meca-penguin's slot in every scene: the original keeps it dead until Twinsen uses one.
		if (Object.MoveMode == 7)
		{
			continue;
		}
		const FString CommunityName = Context.GetCommunitySceneName(TEXT("sceneActors"), Scene.Index, Index);
		const FString Label = FString::Printf(TEXT("%02d_%s"), Index, CommunityName.IsEmpty() ? TEXT("Object") : *CommunityName);
		const FVector Feet = Asset->ToWorld(Object.Position);

		if (Object.HasFlag(LBASceneFlags::Sprite))
		{
			TArray<UStaticMesh*> Frames;
			for (int32 Key : GetObjectSprites(Object))
			{
				if (UStaticMesh* Mesh = SpriteMeshes.FindRef(Key))
				{
					Frames.Add(Mesh);
				}
			}
			ALBASceneSprite* Actor = Context.SpawnGeneratedActor<ALBASceneSprite>(World, FTransform(Feet), Label, TEXT("Scene/Sprites"));
			if (!Actor)
			{
				continue;
			}
			Actor->Scene = Asset;
			Actor->ObjectIndex = Index;
			Actor->Sprite = Object.Sprite;
			Actor->bStartHidden = Object.HasFlag(LBASceneFlags::Invisible);
			if (Object.HasFlag(LBASceneFlags::AnimatedSprite) && Frames.Num() > 1)
			{
				Actor->Frames.Append(Frames);
				Actor->FramesPerSecond = Object.SpriteAnimFps > 0 ? float(Object.SpriteAnimFps) : 10.f;
			}
			Actor->GetSpriteComponent()->SetStaticMesh(Frames.IsEmpty() ? nullptr : Frames[0]);
			continue;
		}

		ALBASceneActor* Actor = Context.SpawnGeneratedActor<ALBASceneActor>(World,
			FTransform(FRotator(0.0, ULBASceneAsset::BetaToActorYaw(Object.Beta), 0.0), Feet), Label, TEXT("Scene/Objects"));
		if (!Actor)
		{
			continue;
		}
		Actor->Scene = Asset;
		Actor->ObjectIndex = Index;
		Actor->Entity = Entities.FindRef(Object.Entity);
		Actor->BodyType = Object.Body;
		Actor->AnimType = FMath::Max(Object.Anim, 0);
		Actor->bStartHidden = Object.HasFlag(LBASceneFlags::Invisible);
		Actor->bFallable = Object.HasFlag(LBASceneFlags::Fallable);
		Actor->ApplyBody();
		Actor->SetActorLocation(Feet + FVector(0.0, 0.0, Actor->GetOriginHeight()));
		if (Object.Entity >= 0 && !Actor->Entity)
		{
			Context.Warn(FString::Printf(TEXT("Scene %d object %d: entity %d is not imported"), Scene.Index, Index, Object.Entity));
		}
		++NumObjects;
		if (Actor->Entity && Object.Body >= 0 && !Actor->Entity->GetBodyMesh(Object.Body))
		{
			UE_LOG(LogLBA2, Verbose, TEXT("Scene %d object %d: entity %d has no body %d"), Scene.Index, Index, Object.Entity, Object.Body);
			++NumMissingBodies;
		}
	}

	for (int32 Index = 0; Index < Scene.Zones.Num(); ++Index)
	{
		const FLBASceneZoneData& Zone = Scene.Zones[Index];
		if (IsSameIslandCubeChange(Scene, Zone))
		{
			continue;
		}
		UClass* Class = ZoneClass(Zone.Type);
		if (!Class)
		{
			continue;
		}
		const FVector A = Asset->ToWorld(Zone.Min);
		const FVector B = Asset->ToWorld(Zone.Max);
		const FBox Box(A.ComponentMin(B), A.ComponentMax(B) + FVector(1.0));
		const FString CommunityName = Context.GetCommunitySceneName(TEXT("sceneZones"), Scene.Index, Index);
		const FString Label = FString::Printf(TEXT("Zone%02d_%s"), Index, CommunityName.IsEmpty() ? *ZoneTypeName(Zone.Type) : *CommunityName);
		AVolume* Volume = Cast<AVolume>(Context.SpawnGeneratedActor(World, Class, FTransform(Box.GetCenter()), Label,
			*(TEXT("Scene/Zones/") + ZoneTypeName(Zone.Type))));
		if (!Volume)
		{
			continue;
		}
		UCubeBuilder* Builder = NewObject<UCubeBuilder>();
		const FVector Size = Box.GetSize();
		Builder->X = Size.X;
		Builder->Y = Size.Y;
		Builder->Z = Size.Z;
		UActorFactory::CreateBrushForVolumeActor(Volume, Builder);

		if (ALBAZoneVolume* ZoneVolume = Cast<ALBAZoneVolume>(Volume))
		{
			ZoneVolume->Scene = Asset;
			ZoneVolume->ZoneIndex = Index;
			ZoneVolume->InitFromZone(Zone);
		}
		else if (ALBAHitVolume* Hit = Cast<ALBAHitVolume>(Volume))
		{
			Hit->Scene = Asset;
			Hit->ZoneIndex = Index;
			Hit->InitFromZone(Zone);
		}
	}

	for (int32 Index = 0; Index < Scene.Points.Num(); ++Index)
	{
		if (ATargetPoint* Point = Context.SpawnGeneratedActor<ATargetPoint>(World, FTransform(Asset->ToWorld(Scene.Points[Index])),
			FString::Printf(TEXT("Point_%02d"), Index), TEXT("Scene/Points")))
		{
			Point->Tags.Add(*FString::Printf(TEXT("LBAPoint_%02d"), Index));
		}
	}
}

void FLBASceneImporter::AddStreamedScenes(const FString& SceneryLevel, const TArray<FStreamedScene>& InScenes)
{
	UWorld* World = Context.OpenLevel(SceneryLevel);
	if (!World)
	{
		Context.Warn(FString::Printf(TEXT("Could not open %s to add its scenes"), *SceneryLevel));
		return;
	}

	// Replace earlier entries of the same scenes (their names may have changed).
	const FString Prefix = Context.GetContentPath(TEXT("Scenes")) / TEXT("L_Scene_");
	TSet<int32> SceneIndices;
	for (const FStreamedScene& Scene : InScenes)
	{
		SceneIndices.Add(Scene.SceneIndex);
	}
	TArray<ULevelStreaming*> ToRemove;
	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		FString Package = Streaming ? Streaming->GetWorldAssetPackageName() : FString();
		if (Package.RemoveFromStart(Prefix) && SceneIndices.Contains(FCString::Atoi(*Package.Left(3))))
		{
			ToRemove.Add(Streaming);
		}
	}
	for (ULevelStreaming* Streaming : ToRemove)
	{
		World->RemoveStreamingLevel(Streaming);
	}

	for (const FStreamedScene& Scene : InScenes)
	{
		UClass* Class = Scene.bInitiallyLoaded ? ULevelStreamingAlwaysLoaded::StaticClass() : ULevelStreamingDynamic::StaticClass();
		ULevelStreaming* Streaming = NewObject<ULevelStreaming>(World, Class, NAME_None, RF_NoFlags);
		Streaming->SetWorldAssetByPackageName(FName(*Scene.LevelPackage));
		Streaming->LevelColor = FLinearColor::IntToDistinctColor(Scene.SceneIndex, 0.8f, 0.9f);
		if (ULevelStreamingDynamic* Dynamic = Cast<ULevelStreamingDynamic>(Streaming))
		{
			Dynamic->bInitiallyLoaded = false;
			Dynamic->bInitiallyVisible = false;
		}
		World->AddStreamingLevel(Streaming);
	}
	Context.EndLevel(World);
}

#undef LOCTEXT_NAMESPACE
