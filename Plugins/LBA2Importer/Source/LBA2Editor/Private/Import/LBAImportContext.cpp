#include "Import/LBAImportContext.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Formats/LBAXpl.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBARenderingAssets.h"
#include "Interfaces/IPluginManager.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBA2Settings.h"
#include "LBACharacter.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

const FName FLBAImportContext::GeneratedActorTag(TEXT("LBA2Imported"));

FLBAImportContext::FLBAImportContext(const ULBA2ImporterSettings& InSettings)
	: Settings(InSettings)
{
}

FLBAImportContext::~FLBAImportContext()
{
}

bool FLBAImportContext::Initialize(FString& OutError)
{
	DataDirectory = Settings.ResolveDataDirectory();
	if (DataDirectory.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Could not find LBA_BKG.HQR under '%s'. Point the importer at an installed copy of LBA2."), *Settings.GameDirectory.Path);
		return false;
	}
	ContentRoot = Settings.GetContentRoot();
	Scale = ULBA2Settings::GetWorldScale();

	if (!LoadPalette(OutError))
	{
		return false;
	}
	UsePalette(FLBAXpl::InteriorEntry);
	LoadCommunityNames();
	Materials = MakeUnique<FLBAMaterialLibrary>(*this);
	Rendering = MakeUnique<FLBARenderingAssets>(*this);
	UE_LOG(LogLBA2, Display, TEXT("LBA2 import: data in %s, content in %s, scale %.4f cm/unit"), *DataDirectory, *ContentRoot, Scale);
	return true;
}

FString FLBAImportContext::GetGameFilePath(const FString& FileName) const
{
	return FPaths::Combine(DataDirectory, FileName);
}

const FLBAHqrFile* FLBAImportContext::GetHqr(const FString& FileName)
{
	const FString Key = FileName.ToUpper();
	if (TUniquePtr<FLBAHqrFile>* Found = HqrCache.Find(Key))
	{
		return Found->IsValid() && (*Found)->IsOpen() ? Found->Get() : nullptr;
	}
	TUniquePtr<FLBAHqrFile> File = MakeUnique<FLBAHqrFile>();
	if (!File->Open(GetGameFilePath(FileName)))
	{
		File.Reset();
	}
	const FLBAHqrFile* Result = File.Get();
	HqrCache.Add(Key, MoveTemp(File));
	return Result;
}

bool FLBAImportContext::LoadPalette(FString& OutError)
{
	const FLBAHqrFile* Ress = GetHqr(TEXT("RESS.HQR"));
	const TArray<uint8> Data = Ress ? Ress->GetEntry(0) : TArray<uint8>();
	if (Data.Num() < 768)
	{
		OutError = TEXT("RESS.HQR is missing or has no palette.");
		return false;
	}
	Palette.SetNum(256);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		Palette[Index] = FColor(Data[Index * 3], Data[Index * 3 + 1], Data[Index * 3 + 2], 255);
	}
	return true;
}

void FLBAImportContext::LoadCommunityNames()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LBA2Importer"));
	if (!Plugin)
	{
		return;
	}
	const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/CommunityMetadata/lba2_names.json"));
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		return;
	}
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	FJsonSerializer::Deserialize(Reader, CommunityNames);
}

FString FLBAImportContext::GetCommunityName(const FString& Table, int32 Index) const
{
	if (!CommunityNames.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (CommunityNames->TryGetObjectField(Table, Object) && Object)
	{
		FString Name;
		if ((*Object)->TryGetStringField(FString::FromInt(Index), Name))
		{
			return Name;
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (CommunityNames->TryGetArrayField(Table, Array) && Array && Array->IsValidIndex(Index))
	{
		FString Name;
		if ((*Array)[Index].IsValid() && (*Array)[Index]->TryGetString(Name))
		{
			return Name;
		}
	}
	return FString();
}

FString FLBAImportContext::GetCommunitySceneName(const FString& Table, int32 Scene, int32 Index) const
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	FString Name;
	if (CommunityNames.IsValid() && CommunityNames->TryGetObjectField(Table, Object) && Object
		&& (*Object)->TryGetArrayField(FString::FromInt(Scene), Array) && Array && Array->IsValidIndex(Index)
		&& (*Array)[Index].IsValid())
	{
		(*Array)[Index]->TryGetString(Name);
	}
	return Name;
}

FLBAMaterialLibrary& FLBAImportContext::GetMaterials()
{
	check(Materials.IsValid());
	return *Materials;
}

FLBARenderingAssets& FLBAImportContext::GetRendering()
{
	check(Rendering.IsValid());
	return *Rendering;
}

const FLBAXpl* FLBAImportContext::GetXpl(int32 XplEntry)
{
	if (TUniquePtr<FLBAXpl>* Found = XplCache.Find(XplEntry))
	{
		return Found->Get();
	}
	TUniquePtr<FLBAXpl> Xpl = MakeUnique<FLBAXpl>();
	const FLBAHqrFile* Ress = GetHqr(TEXT("RESS.HQR"));
	if (!Ress || !Xpl->Load(*Ress, XplEntry))
	{
		Xpl.Reset();
	}
	const FLBAXpl* Result = Xpl.Get();
	XplCache.Add(XplEntry, MoveTemp(Xpl));
	return Result;
}

const FLBAXpl* FLBAImportContext::UsePalette(int32 XplEntry)
{
	// The RESS palette loaded at startup (menus) stays active if the island palette is missing.
	if (const FLBAXpl* Xpl = GetXpl(XplEntry))
	{
		ActiveXpl = Xpl;
		Palette = Xpl->Palette;
	}
	else
	{
		Warn(FString::Printf(TEXT("RESS.HQR has no island palette %d, keeping the current palette"), XplEntry));
	}
	return ActiveXpl;
}

FString FLBAImportContext::SanitizeName(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len());
	bool bUpperNext = true;
	for (TCHAR Char : In)
	{
		if (FChar::IsAlnum(Char) || Char == TEXT('_'))
		{
			Out.AppendChar(bUpperNext ? FChar::ToUpper(Char) : Char);
			bUpperNext = false;
		}
		else if (Char != TEXT('\'') && Char != TEXT('.'))
		{
			bUpperNext = true;
		}
	}
	return Out;
}

// Assets ----------------------------------------------------------------------------------------------

UObject* FLBAImportContext::CreateOrReuseAsset(UClass* Class, const FString& Folder, const FString& Name, bool* bOutExisted)
{
	const FString PackageName = Folder / Name;
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (!Package && FPackageName::DoesPackageExist(PackageName))
	{
		Package = LoadPackage(nullptr, *PackageName, LOAD_None);
	}
	if (!Package)
	{
		Package = CreatePackage(*PackageName);
	}
	Package->FullyLoad();

	if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *Name))
	{
		if (Existing->IsA(Class))
		{
			if (bOutExisted)
			{
				*bOutExisted = true;
			}
			return Existing;
		}
		Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	}

	if (bOutExisted)
	{
		*bOutExisted = false;
	}
	UObject* Asset = NewObject<UObject>(Package, Class, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}

UObject* FLBAImportContext::DuplicateAsset(UObject* Source, const FString& Folder, const FString& Name)
{
	if (!Source)
	{
		return nullptr;
	}
	// Take over the path: an existing asset becomes a placeholder with the same name, replaced by the copy.
	bool bExisted = false;
	UObject* Existing = CreateOrReuseAsset(Source->GetClass(), Folder, Name, &bExisted);
	UPackage* Package = Existing->GetPackage();
	Existing->ClearFlags(RF_Public | RF_Standalone);
	Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	UObject* Copy = StaticDuplicateObject(Source, Package, FName(*Name));
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Copy);
	MarkForSave(Copy);
	return Copy;
}

UObject* FLBAImportContext::FindAsset(UClass* Class, const FString& Folder, const FString& Name) const
{
	const FString ObjectPath = Folder / Name + TEXT(".") + Name;
	return StaticLoadObject(Class, nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

void FLBAImportContext::MarkForSave(UObject* Asset)
{
	if (Asset)
	{
		Asset->MarkPackageDirty();
		PendingSaves.AddUnique(Asset);
	}
}

bool FLBAImportContext::SavePackageFor(UObject* Asset)
{
	UPackage* Package = Asset->GetPackage();
	const bool bIsMap = Asset->IsA<UWorld>();
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(),
		bIsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());

	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	Args.Error = GWarn;
	const bool bSaved = UPackage::SavePackage(Package, Asset, *Filename, Args);
	if (!bSaved)
	{
		Warn(FString::Printf(TEXT("Failed to save %s"), *Package->GetName()));
	}
	return bSaved;
}

void FLBAImportContext::Flush()
{
	FinishPendingBuilds();
	FAssetCompilingManager::Get().FinishAllCompilation();
	for (const TWeakObjectPtr<UObject>& Weak : PendingSaves)
	{
		if (UObject* Asset = Weak.Get())
		{
			SavePackageFor(Asset);
		}
	}
	PendingSaves.Reset();
}

UTexture2D* FLBAImportContext::CreatePaletteTexture(const FString& Folder, const FString& Name, int32 Width, int32 Height,
	TFunctionRef<uint8(int32 X, int32 Y)> GetIndex, bool bColorKey, bool bWrap)
{
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(Width * Height);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		for (int32 X = 0; X < Width; ++X)
		{
			const uint8 Index = GetIndex(X, Y);
			FColor Color = Palette[Index];
			Color.A = (bColorKey && Index == 0) ? 0 : 255;
			Pixels[Y * Width + X] = Color;
		}
	}
	return CreateTexture(Folder, Name, Width, Height, Pixels, bWrap);
}

UTexture2D* FLBAImportContext::CreateTexture(const FString& Folder, const FString& Name, int32 Width, int32 Height,
	const TArray<FColor>& Pixels, bool bWrap, bool bSRGB)
{
	UTexture2D* Texture = CreateOrReuseAsset<UTexture2D>(Folder, Name);
	if (!Texture)
	{
		return nullptr;
	}
	if (Texture->IsCompiling())
	{
		FTextureCompilingManager::Get().FinishCompilation({ Texture });
	}
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	Texture->SRGB = bSRGB;
	Texture->CompressionSettings = TC_Default;
	Texture->CompressionNone = true;
	Texture->Filter = Settings.bPixelArtFiltering ? TF_Nearest : TF_Default;
	Texture->AddressX = bWrap ? TA_Wrap : TA_Clamp;
	Texture->AddressY = bWrap ? TA_Wrap : TA_Clamp;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->MipGenSettings = FMath::IsPowerOfTwo(Width) && FMath::IsPowerOfTwo(Height) ? TMGS_FromTextureGroup : TMGS_NoMipmaps;
	Texture->PostEditChange();
	MarkForSave(Texture);
	return Texture;
}

UStaticMesh* FLBAImportContext::CreateStaticMesh(const FString& Folder, const FString& Name, FMeshDescription&& MeshDescription,
	const TArray<UMaterialInterface*>& InMaterials, const TArray<FName>& SlotNames, const FLBAStaticMeshOptions& Options)
{
	UStaticMesh* Mesh = CreateOrReuseAsset<UStaticMesh>(Folder, Name);
	if (!Mesh)
	{
		return nullptr;
	}
	if (Mesh->IsCompiling())
	{
		FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });
	}

	Mesh->PreEditChange(nullptr);
	Mesh->GetStaticMaterials().Reset();
	for (int32 Index = 0; Index < InMaterials.Num(); ++Index)
	{
		const FName Slot = SlotNames.IsValidIndex(Index) ? SlotNames[Index] : FName(*FString::Printf(TEXT("Slot%d"), Index));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(InMaterials[Index], Slot, Slot));
	}

	Mesh->SetNumSourceModels(1);
	FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(0);
	SourceModel.BuildSettings.bRecomputeNormals = false;
	SourceModel.BuildSettings.bRecomputeTangents = true;
	SourceModel.BuildSettings.bUseMikkTSpace = true;
	SourceModel.BuildSettings.bGenerateLightmapUVs = false;
	SourceModel.BuildSettings.bRemoveDegenerates = true;
	SourceModel.BuildSettings.bUseFullPrecisionUVs = true;

	Mesh->CreateMeshDescription(0, MoveTemp(MeshDescription));
	Mesh->CommitMeshDescription(0);

	// Polygon group N always maps to material N.
	FMeshSectionInfoMap& SectionInfo = Mesh->GetSectionInfoMap();
	SectionInfo.Clear();
	for (int32 Index = 0; Index < InMaterials.Num(); ++Index)
	{
		FMeshSectionInfo Info(Index);
		Info.bCastShadow = !(SlotNames.IsValidIndex(Index) && Options.NoShadowSlots.Contains(SlotNames[Index]));
		SectionInfo.Set(0, Index, Info);
	}

	Mesh->CreateBodySetup();
	UBodySetup* BodySetup = Mesh->GetBodySetup();
	BodySetup->AggGeom.EmptyElements();
	BodySetup->CollisionTraceFlag = CTF_UseDefault;
	Mesh->ComplexCollisionMesh = Options.ComplexCollisionMesh;
	if (Options.bNoCollision)
	{
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	}
	else if (Options.bComplexAsSimple || Options.ComplexCollisionMesh)
	{
		BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	}
	if (Options.bSimpleBox && !Options.bNoCollision)
	{
		const FMeshDescription* Stored = Mesh->GetMeshDescription(0);
		const FBox Box = Stored ? FBox(Stored->ComputeBoundingBox()) : FBox(ForceInit);
		if (Box.IsValid)
		{
			FKBoxElem BoxElem(Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z);
			BoxElem.Center = Box.GetCenter();
			BodySetup->AggGeom.BoxElems.Add(BoxElem);
		}
	}
	if (Options.SimpleCollision && !Options.bNoCollision)
	{
		Options.SimpleCollision(BodySetup->AggGeom);
	}
	BodySetup->InvalidatePhysicsData();

	if (Options.bDeferBuild)
	{
		PendingMeshBuilds.AddUnique(Mesh);
	}
	else
	{
		Mesh->Build(true);
		Mesh->PostEditChange();
	}
	MarkForSave(Mesh);
	return Mesh;
}

void FLBAImportContext::FinishPendingBuilds()
{
	if (PendingMeshBuilds.Num() == 0)
	{
		return;
	}
	UStaticMesh::FBuildParameters Params;
	Params.bInSilent = true;
	UStaticMesh::BatchBuild(PendingMeshBuilds, Params);
	FAssetCompilingManager::Get().FinishAllCompilation();
	for (UStaticMesh* Mesh : PendingMeshBuilds)
	{
		if (Mesh->GetBodySetup())
		{
			Mesh->GetBodySetup()->CreatePhysicsMeshes();
		}
		Mesh->MarkPackageDirty();
	}
	PendingMeshBuilds.Reset();
}

// Levels ----------------------------------------------------------------------------------------------

UWorld* FLBAImportContext::BeginLevel(const FString& Folder, const FString& Name)
{
	return LoadLevel(Folder / Name, Name, true);
}

UWorld* FLBAImportContext::OpenLevel(const FString& PackageName)
{
	if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
	{
		return nullptr;
	}
	return LoadLevel(PackageName, FPackageName::GetShortName(PackageName), false);
}

UWorld* FLBAImportContext::LoadLevel(const FString& PackageName, const FString& Name, bool bRegenerate)
{

	// Never rebuild the level that is open in the editor from under the user.
	if (GEditor && !IsRunningCommandlet())
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			if (EditorWorld->GetPackage()->GetName() == PackageName)
			{
				Warn(FString::Printf(TEXT("%s is open in the editor; open another level to re-import it."), *PackageName));
				return nullptr;
			}
		}
	}

	UWorld* World = nullptr;
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (!Package && FPackageName::DoesPackageExist(PackageName))
	{
		Package = LoadPackage(nullptr, *PackageName, LOAD_None);
	}
	if (Package)
	{
		World = UWorld::FindWorldInPackage(Package);
	}

	const UWorld::InitializationValues InitValues = UWorld::InitializationValues()
		.ShouldSimulatePhysics(false)
		.EnableTraceCollision(true)
		.CreateNavigation(false)
		.CreateAISystem(false)
		.AllowAudioPlayback(false)
		.CreatePhysicsScene(true)
		.CreateWorldPartition(false);

	if (World)
	{
		if (!World->bIsWorldInitialized)
		{
			World->WorldType = EWorldType::Inactive;
			World->InitWorld(InitValues);
		}
		TArray<AActor*> ToDestroy;
		for (TActorIterator<AActor> It(World); It && bRegenerate; ++It)
		{
			if (It->Tags.Contains(GeneratedActorTag))
			{
				ToDestroy.Add(*It);
			}
		}
		for (AActor* Actor : ToDestroy)
		{
			World->EditorDestroyActor(Actor, false);
		}
	}
	else
	{
		if (!Package)
		{
			Package = CreatePackage(*PackageName);
		}
		World = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*Name), Package, false, ERHIFeatureLevel::Num, &InitValues);
		if (!World)
		{
			Warn(FString::Printf(TEXT("Could not create level %s"), *PackageName));
			return nullptr;
		}
		World->SetFlags(RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(World);
	}
	if (AWorldSettings* WorldSettings = World->GetWorldSettings())
	{
		WorldSettings->DefaultGameMode = Settings.bPlayAsTwinsen ? ALBAGameMode::StaticClass() : nullptr;
	}
	OwnedWorlds.Add(World);
	return World;
}

void FLBAImportContext::EndLevel(UWorld* World)
{
	if (!World)
	{
		return;
	}
	Flush();
	World->MarkPackageDirty();
	SavePackageFor(World);
	if (OwnedWorlds.Remove(World) > 0 && World->WorldType == EWorldType::Inactive)
	{
		World->DestroyWorld(false);
	}
}

AActor* FLBAImportContext::SpawnGeneratedActor(UWorld* World, UClass* Class, const FTransform& Transform, const FString& Label, const FName& Folder)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.bNoFail = true;
	AActor* Actor = World->SpawnActor(Class, &Transform, Params);
	TagGeneratedActor(Actor, Label, Folder);
	return Actor;
}

AActor* FLBAImportContext::SpawnGeneratedActorWithFactory(UWorld* World, UClass* Class, const FTransform& Transform, const FString& Label, const FName& Folder)
{
	UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryForActorClass(Class) : nullptr;
	if (!Factory)
	{
		return SpawnGeneratedActor(World, Class, Transform, Label, Folder);
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.bNoFail = true;
	AActor* Actor = Factory->CreateActor(nullptr, World->PersistentLevel, Transform, Params);
	TagGeneratedActor(Actor, Label, Folder);
	return Actor;
}

void FLBAImportContext::TagGeneratedActor(AActor* Actor, const FString& Label, const FName& Folder)
{
	if (!Actor)
	{
		return;
	}
	Actor->Tags.AddUnique(GeneratedActorTag);
	if (!Label.IsEmpty())
	{
		Actor->SetActorLabel(Label, false);
	}
	if (!Folder.IsNone())
	{
		Actor->SetFolderPath(Folder);
	}
}

// Progress --------------------------------------------------------------------------------------------

void FLBAImportContext::ReportProgress(const FText& Message, float Work)
{
	UE_LOG(LogLBA2, Display, TEXT("%s"), *Message.ToString());
	if (SlowTask)
	{
		SlowTask->EnterProgressFrame(Work, Message);
	}
}

bool FLBAImportContext::IsCancelled() const
{
	return SlowTask && SlowTask->ShouldCancel();
}

void FLBAImportContext::Warn(const FString& Message)
{
	++NumWarnings;
	UE_LOG(LogLBA2, Warning, TEXT("%s"), *Message);
}
