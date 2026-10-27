#include "Import/LBAWater.h"

#include "CoreGlobals.h"
#include "GerstnerWaterWaves.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBARenderingAssets.h"
#include "LBA2Runtime.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyOceanComponent.h"
#include "WaterMeshComponent.h"
#include "WaterSplineComponent.h"
#include "WaterTerrainComponent.h"
#include "WaterWaves.h"
#include "WaterZoneActor.h"

namespace
{
	/** Bump when the wave defaults below change: the wave assets are then reset, user edits are otherwise kept. */
	constexpr int32 WavesVersion = 1;
	const TCHAR* VersionKey = TEXT("LBA2PresetVersion");

	/**
	 * Scattering coefficient of the water (1/cm). The absorption is set per island for the water's color; together
	 * they set how deep you see: the seabed shows through the first meter or two, so only the shore's shallows get
	 * lighter (half the light gets through 2.5 m of water in blue, a third of a meter in red).
	 */
	constexpr float WaterScattering = 0.0025f;
	/** Brightest single-scattering albedo given to a channel: deep water can't scatter back all of its light. */
	constexpr float MaxWaterAlbedo = 0.9f;

	/**
	 * Light that deep water scatters back to the camera, per unit of albedo: the sun's (Schlick phase of the ocean
	 * material, anisotropy 0.1, light scattered back up towards a camera looking down) and the ambient light's
	 * (isotropic, AmbientLight being the irradiance over pi). Fresnel takes a few percent off.
	 */
	constexpr float SunBackscatterPhase = 0.06f;
	constexpr float AmbientScatterPhase = 0.25f;
	constexpr float Transmission = 0.97f;
	/**
	 * Open water's light relative to that model, measured against the original on Citabau's harbour (the model is
	 * exact for deep water; waves and reflections of the sky add about a fifth).
	 */
	constexpr float MeasuredWaterGain = 1.2f;
	/** Under a clear sky, the white clouds' reflections add a quarter more (measured on Citabau's harbour). */
	constexpr float ClearSkyWaterGain = 1.25f;

	/** Water mesh tiles (cm) and their quads per side (2^n): 25 cm quads up close. */
	constexpr float WaterTileSize = 400.f;
	constexpr int32 WaterTileTessellation = 4;
	/** Most water mesh tiles across a zone (r.Water.WaterMesh.MaxDimensionInTiles' default). */
	constexpr double MaxWaterTiles = 256.0;

	/**
	 * Most water info texels across a zone: an island's zone, and globe mode's, which covers all the sea the bent view
	 * shows (3 km for Twinsun: 1.5 m per texel). The texture is half float RGBA, 32 MB at 2048, and each update of it
	 * renders through full float targets of its size: 4096 went 130 MB over an 8 GB card's video memory budget.
	 */
	constexpr int32 MaxLevelWaterResolution = 2048;
	constexpr int32 MaxGlobeWaterResolution = 2048;

	/** Gerstner wave generator settings of a sea mood (cm, degrees). */
	struct FWaves
	{
		const TCHAR* Name;
		float MinWavelength;
		float MaxWavelength;
		float MinAmplitude;
		float MaxAmplitude;
		float WindAngle;
		float Spread;
		float SmallWaveSteepness;
		float LargeWaveSteepness;
	};

	// At LBA scale (a terrain cell is 64 cm, piers stand about a meter above the sea): a calm swell of a few
	// centimeters that keeps beaches and piers readable, and choppier, higher storm waves.
	constexpr FWaves CalmWaves = { TEXT("Waves_LBA_Calm"), 150.f, 1600.f, 0.8f, 9.f, -30.f, 100.f, 0.35f, 0.15f };
	constexpr FWaves StormWaves = { TEXT("Waves_LBA_Storm"), 250.f, 3600.f, 2.f, 30.f, -60.f, 150.f, 0.6f, 0.3f };

	UWaterWavesAsset* GetWavesAsset(FLBAImportContext& Context, const FWaves& Settings)
	{
		bool bExisted = false;
		UWaterWavesAsset* Asset = Context.CreateOrReuseAsset<UWaterWavesAsset>(Context.GetContentPath(TEXT("Rendering/Water")), Settings.Name, &bExisted);
		if (!Asset)
		{
			return nullptr;
		}
		FMetaData& MetaData = Asset->GetPackage()->GetMetaData();
		const FString* Stamp = MetaData.FindValue(Asset, VersionKey);
		if (bExisted && Asset->GetWaterWaves() && Stamp && FCString::Atoi(**Stamp) >= WavesVersion)
		{
			// Keep user edits.
			return Asset;
		}

		UGerstnerWaterWaves* Waves = NewObject<UGerstnerWaterWaves>(Asset, MakeUniqueObjectName(Asset, UGerstnerWaterWaves::StaticClass(), TEXT("GerstnerWaves")));
		UGerstnerWaterWaveGeneratorSimple* Generator = NewObject<UGerstnerWaterWaveGeneratorSimple>(Waves, TEXT("Generator"));
		Generator->NumWaves = 16;
		Generator->MinWavelength = Settings.MinWavelength;
		Generator->MaxWavelength = Settings.MaxWavelength;
		Generator->MinAmplitude = Settings.MinAmplitude;
		Generator->MaxAmplitude = Settings.MaxAmplitude;
		Generator->WindAngleDeg = Settings.WindAngle;
		Generator->DirectionAngularSpreadDeg = Settings.Spread;
		Generator->SmallWaveSteepness = Settings.SmallWaveSteepness;
		Generator->LargeWaveSteepness = Settings.LargeWaveSteepness;
		Waves->GerstnerWaveGenerator = Generator;
		Waves->RecomputeWaves(false);
		Asset->SetWaterWaves(Waves);
		MetaData.SetValue(Asset, VersionKey, *LexToString(WavesVersion));
		Context.MarkForSave(Asset);
		return Asset;
	}

	/**
	 * Bump to regenerate the clipped water materials (e.g. after an engine upgrade changed the Water plugin's).
	 * v2: globe mode's bend.
	 */
	constexpr int32 ClippedWaterVersion = 2;

	/**
	 * Unreal's ocean material, clipped where its water zone has no water (inside the coastline). Unreal's only turns
	 * see-through there: those pixels still count as water in the G-buffer, so the cartoon water would paint them, as
	 * blocky patches where the water mesh's tiles cross the coast. Derived from the Water plugin's material chain (copied
	 * once per version): its own depth mask, which goes to 0 where the water info texture has no water, also feeds the
	 * opacity mask, which the ocean's masked blend mode clips at a third. It bends with globe mode's view like the
	 * generated masters (no offset without a planet).
	 */
	UMaterialInterface* GetClippedOceanMaterial(FLBAImportContext& Context)
	{
		const FString Folder = Context.GetContentPath(TEXT("Rendering/Water"));
		const TCHAR* MaterialName = TEXT("M_LBA_Water");
		const TCHAR* OceanName = TEXT("MI_LBA_Water_Ocean");
		UMaterial* Existing = Context.FindAsset<UMaterial>(Folder, MaterialName);
		UMaterialInstanceConstant* ExistingOcean = Context.FindAsset<UMaterialInstanceConstant>(Folder, OceanName);
		if (Existing && ExistingOcean && LBAMaterials::IsGeneratedVersion(Existing, ClippedWaterVersion))
		{
			return ExistingOcean;
		}

		UMaterial* Source = LoadObject<UMaterial>(nullptr, TEXT("/Water/Materials/WaterSurface/Water_Material.Water_Material"));
		UMaterialInstanceConstant* SourceInst = LoadObject<UMaterialInstanceConstant>(nullptr, TEXT("/Water/Materials/WaterSurface/Water_Material_Inst.Water_Material_Inst"));
		UMaterialInstanceConstant* SourceOcean = LoadObject<UMaterialInstanceConstant>(nullptr, TEXT("/Water/Materials/WaterSurface/Water_Material_Ocean.Water_Material_Ocean"));
		if (!Source || !SourceInst || !SourceOcean || SourceInst->Parent != Source || SourceOcean->Parent != SourceInst)
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 water: the Water plugin's ocean material chain changed, using its ocean material unclipped"));
			return SourceOcean;
		}

		UMaterial* Material = Context.DuplicateAsset(Source, Folder, MaterialName);
		UMaterialEditorOnlyData* Graph = Material ? Material->GetEditorOnlyData() : nullptr;
		if (!Graph)
		{
			return SourceOcean;
		}
		UMaterialExpressionMaterialFunctionCall* DepthMask = nullptr;
		int32 MaskOutput = INDEX_NONE;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			if (Call && Call->MaterialFunction && Call->MaterialFunction->GetName() == TEXT("WaterOpacityMaskFromDepth"))
			{
				DepthMask = Call;
				MaskOutput = Call->FunctionOutputs.IndexOfByPredicate([](const FFunctionExpressionOutput& Output) { return Output.Output.OutputName == TEXT("Mask"); });
			}
		}
		if (!Graph->MaterialAttributes.Expression || !DepthMask || MaskOutput == INDEX_NONE)
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA2 water: the Water plugin's material has no depth mask any more, using its ocean material unclipped"));
			return SourceOcean;
		}

		UMaterialExpressionSetMaterialAttributes* Clip = NewObject<UMaterialExpressionSetMaterialAttributes>(Material);
		Clip->Inputs[0].Connect(Graph->MaterialAttributes.OutputIndex, Graph->MaterialAttributes.Expression);
		Clip->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(MP_OpacityMask));
		Clip->Inputs.AddDefaulted_GetRef().Connect(MaskOutput, DepthMask);

		// Globe mode's bend, like every generated material: added to the waves, and the normals turned with it.
		UMaterialParameterCollection* Planet = Context.GetRendering().GetPlanetParameters();
		UMaterialExpressionGetMaterialAttributes* Own = NewObject<UMaterialExpressionGetMaterialAttributes>(Material);
		Own->MaterialAttributes.Connect(Graph->MaterialAttributes.OutputIndex, Graph->MaterialAttributes.Expression);
		const int32 OwnOffset = Own->CreateOrGetOutputAttribute(MP_WorldPositionOffset);
		const int32 OwnNormal = Own->CreateOrGetOutputAttribute(MP_Normal);
		Material->GetExpressionCollection().AddExpression(Own);
		UMaterialExpressionAdd* Offset = Cast<UMaterialExpressionAdd>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionAdd::StaticClass(), -250, 1000));
		Offset->A.Connect(OwnOffset, Own);
		Offset->B.Connect(0, LBAMaterials::MakePlanetBend(Material, Planet));
		Clip->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(MP_WorldPositionOffset));
		Clip->Inputs.AddDefaulted_GetRef().Connect(0, Offset);
		Clip->AttributeSetTypes.Add(FMaterialAttributeDefinitionMap::GetID(MP_Normal));
		Clip->Inputs.AddDefaulted_GetRef().Connect(0, LBAMaterials::MakePlanetBentNormal(Material, Planet, Own, OwnNormal));

		Clip->MaterialExpressionEditorX = Graph->MaterialAttributes.Expression->MaterialExpressionEditorX + 300;
		Clip->MaterialExpressionEditorY = Graph->MaterialAttributes.Expression->MaterialExpressionEditorY;
		Own->MaterialExpressionEditorX = Clip->MaterialExpressionEditorX;
		Own->MaterialExpressionEditorY = Clip->MaterialExpressionEditorY + 300;
		Material->GetExpressionCollection().AddExpression(Clip);
		Graph->MaterialAttributes.Connect(0, Clip);
		LBAMaterials::AddVersionStamp(Material, ClippedWaterVersion);
		UMaterialEditingLibrary::RecompileMaterial(Material);

		UMaterialInstanceConstant* Inst = Context.DuplicateAsset(SourceInst, Folder, TEXT("MI_LBA_Water"));
		UMaterialEditingLibrary::SetMaterialInstanceParent(Inst, Material);
		UMaterialEditingLibrary::UpdateMaterialInstance(Inst);
		UMaterialInstanceConstant* Ocean = Context.DuplicateAsset(SourceOcean, Folder, OceanName);
		UMaterialEditingLibrary::SetMaterialInstanceParent(Ocean, Inst);
		UMaterialEditingLibrary::UpdateMaterialInstance(Ocean);
		return Ocean;
	}

	/** Ocean material instance with the island's water coefficients (Absorption: 1/coefficient in cm, Scattering: coefficient in 1/m). */
	UMaterialInterface* MakeWaterMaterial(FLBAImportContext& Context, const FString& Folder, const FString& Name, UMaterialInterface* Parent,
		const FLinearColor& Absorption, const FLinearColor& Scattering)
	{
		UMaterialInstanceConstant* Instance = Parent ? Context.CreateOrReuseAsset<UMaterialInstanceConstant>(Folder, Name) : nullptr;
		if (!Instance)
		{
			return Parent;
		}
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
		UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(Instance);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("Absorption"), Absorption);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("Scattering"), Scattering);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Context.MarkForSave(Instance);
		return Instance;
	}
}

FLBAWaterLook LBAWater::ComputeLook(const FLBAEnvironmentPreset& Preset, const FLBAEnvironmentLights& Lights)
{
	// Water color: deep water scatters back Albedo * Light, Albedo = scattering / (scattering + absorption) per
	// channel. The albedo follows the sea color's hue, as bright as the light allows: the original's sea is a bright
	// painted texture, real water is darker, so the palette shading gets the remaining factor as the water's neutral
	// light.
	const FLinearColor Light = Preset.SunColor * (Lights.SunIntensity * SunBackscatterPhase) + FLinearColor::White * (Lights.AmbientLight * AmbientScatterPhase);
	FLinearColor Albedo(Preset.SeaColor.R / FMath::Max(Light.R, 1e-4f), Preset.SeaColor.G / FMath::Max(Light.G, 1e-4f), Preset.SeaColor.B / FMath::Max(Light.B, 1e-4f));
	const float AlbedoScale = FMath::Min(1.f, MaxWaterAlbedo / FMath::Max(FMath::Max3(Albedo.R, Albedo.G, Albedo.B), 1e-4f));
	Albedo *= AlbedoScale;
	auto Absorption = [](float ChannelAlbedo)
	{
		return FMath::Max(WaterScattering * (1.f / FMath::Clamp(ChannelAlbedo, 1e-3f, 1.f) - 1.f), 1e-6f);
	};
	FLBAWaterLook Look;
	Look.Albedo = FLinearColor(Albedo.R, Albedo.G, Albedo.B, 1.f);
	Look.Scattering = FLinearColor(WaterScattering, WaterScattering, WaterScattering, 1.f);
	Look.Absorption = FLinearColor(Absorption(Albedo.R), Absorption(Albedo.G), Absorption(Albedo.B), 1.f);
	const float WaterGain = MeasuredWaterGain * (Preset.ClearSkyColor.IsSet() ? ClearSkyWaterGain : 1.f);
	Look.NeutralLight = AlbedoScale * Transmission * WaterGain / FMath::Max(Preset.SeaLight, 0.05f);
	return Look;
}

namespace
{
	/** Water info texels across a zone of that size (cm): about half a meter each, up to MaxResolution. */
	int32 GetZoneResolution(double Size, int32 MaxResolution)
	{
		return FMath::Clamp(int32(FMath::RoundUpToPowerOfTwo(uint32(Size / 50.0))), 256, MaxResolution);
	}

	/**
	 * A water zone over Bounds: its water info texture (ground depth, for the shore) gets about half a meter per texel,
	 * up to MaxResolution texels across.
	 */
	AWaterZone* SpawnZone(FLBAImportContext& Context, UWorld* World, const FBox2D& Bounds, int32 MaxResolution, UMaterialInterface* FarMaterial,
		const FName& Folder)
	{
		const FVector2D Center = Bounds.GetCenter();
		const FVector2D Extent = Bounds.GetSize();
		AWaterZone* Zone = Context.SpawnGeneratedActorWithFactory<AWaterZone>(World, FTransform(FVector(Center, 0.0)), TEXT("WaterZone"), Folder);
		if (!Zone)
		{
			return nullptr;
		}
		Zone->SetZoneExtent(Extent);
		const double Size = FMath::Max(Extent.X, Extent.Y);
		const int32 Resolution = GetZoneResolution(Size, MaxResolution);
		Zone->SetRenderTargetResolution(FIntPoint(Resolution, Resolution));
		Zone->SetFarMeshMaterial(FarMaterial);
		if (UWaterMeshComponent* Mesh = Zone->GetWaterMeshComponent())
		{
			if (!FarMaterial)
			{
				Mesh->FarDistanceMeshExtent = 0.f;
			}
			// The water mesh leaves out only whole tiles inside the coastline: small tiles follow it closely. Fewer quads
			// per tile and LOD distances scaled back up keep the mesh about as dense as with the default 24 m tiles. It
			// takes at most MaxWaterTiles across: larger zones get larger tiles with more quads each, as small.
			int32 Doublings = 0;
			while (WaterTileSize * double(1 << Doublings) * MaxWaterTiles < Size)
			{
				++Doublings;
			}
			const float TileSize = WaterTileSize * float(1 << Doublings);
			const float DefaultTileSize = Mesh->GetTileSize();
			Mesh->SetTileSize(TileSize);
			if (FIntProperty* Tessellation = FindFProperty<FIntProperty>(UWaterMeshComponent::StaticClass(), TEXT("TessellationFactor")))
			{
				Tessellation->SetPropertyValue_InContainer(Mesh, WaterTileTessellation + Doublings);
			}
			if (FFloatProperty* LODScale = FindFProperty<FFloatProperty>(UWaterMeshComponent::StaticClass(), TEXT("LODScale")))
			{
				LODScale->SetPropertyValue_InContainer(Mesh, DefaultTileSize / TileSize);
			}
		}
		return Zone;
	}

	/**
	 * An ocean in Zone, its coastline left dry: filling the zone from Location, or only Cell when it is valid. Without a
	 * coastline, a 2 m loop around Location (a point on land) is the only dry place. Location, Coastline and Cell are
	 * in cm, in the world turned by Yaw (degrees), and so is the ocean.
	 */
	void SpawnOcean(FLBAImportContext& Context, UWorld* World, AWaterZone* Zone, const FVector2D& Location, const TArray<FVector2D>& Coastline,
		const FBox2D& Cell, UMaterialInterface* Material, UWaterWavesAsset* WavesAsset, const FString& Label, const FName& Folder, double Yaw = 0.0)
	{
		// Water bodies build their render data (the meshes the water zone draws and renders into its water info) only
		// when the application can render, and save it with the level. Import commandlets can't, so they would save an
		// invisible ocean: let the water plugin build it anyway.
		TGuardValue<bool> AllowRenderData(PRIVATE_GAllowCommandletRendering, true);

		// A cell's rectangle is centered on its ocean.
		const FVector2D Origin = Cell.bIsValid ? Cell.GetCenter() : Location;
		AWaterBodyOcean* Ocean = Context.SpawnGeneratedActorWithFactory<AWaterBodyOcean>(World,
			FTransform(FRotator(0.0, Yaw, 0.0), FVector(Origin.GetRotated(Yaw), 0.0)), Label, Folder);
		UWaterBodyOceanComponent* Body = Ocean ? Cast<UWaterBodyOceanComponent>(Ocean->GetWaterBodyComponent()) : nullptr;
		if (!Body)
		{
			return;
		}
		// Coastline: straight segments between the given points (local to the ocean), else a 2 m loop on land.
		// (Linear from the start: curved points would overshoot and loop.)
		TArray<FVector2D> Coast;
		for (const FVector2D& Point : Coastline)
		{
			Coast.Add(Point - Origin);
		}
		if (Coast.Num() < 3)
		{
			constexpr double Loop = 100.0;
			const FVector2D Land = Location - Origin;
			Coast = { Land + FVector2D(Loop, -Loop), Land + FVector2D(Loop, Loop), Land + FVector2D(-Loop, Loop), Land + FVector2D(-Loop, -Loop) };
		}
		TArray<FSplinePoint> Points;
		for (int32 Index = 0; Index < Coast.Num(); ++Index)
		{
			Points.Emplace(float(Index), FVector(Coast[Index], 0.0), ESplinePointType::Linear);
		}
		UWaterSplineComponent* Spline = Body->GetWaterSpline();
		Spline->ClearSplinePoints(false);
		Spline->AddPoints(Points, false);
		Spline->UpdateSpline();
		Spline->K2_SynchronizeAndBroadcastDataChange();
		Body->SetWaterMaterial(Material);
		// The water info texture holds water exactly up to the coastline (Unreal grows it 20 m inland by default), so the
		// clipped water material stops there.
		Body->ShapeDilation = 0.f;
		if (WavesAsset)
		{
			UWaterWavesAssetReference* Waves = NewObject<UWaterWavesAssetReference>(Ocean, TEXT("OceanWaterWaves"));
			Waves->SetWaterWavesAsset(WavesAsset);
			Ocean->SetWaterWaves(Waves);
		}
		FOnWaterBodyChangedParams Changed;
		Changed.bShapeOrPositionChanged = true;
		Body->UpdateAll(Changed);
		Body->UpdateWaterZones();
		if (Cell.bIsValid)
		{
			// The ocean's rectangle is centered on the cell rather than on its zone.
			if (FBoolProperty* Centered = FindFProperty<FBoolProperty>(UWaterBodyOceanComponent::StaticClass(), TEXT("bCenterOnWaterZone")))
			{
				Centered->SetPropertyValue_InContainer(Body, false);
			}
			Body->SetCollisionExtents(FVector(Cell.GetExtent(), Body->GetCollisionExtents().Z));
			Body->SetOceanExtent(Cell.GetSize());
		}
		else
		{
			Body->SetCollisionExtents(FVector(Zone->GetZoneExtent() / 2.0, Body->GetCollisionExtents().Z));
			Body->FillWaterZoneWithOcean();
		}
		Body->UpdateWaterBodyRenderData();
	}
}

FLBAWaterSea LBAWater::Spawn(FLBAImportContext& Context, UWorld* World, const FLBAWaterSettings& Settings, const FLBAEnvironmentPreset& Preset,
	const FLBAEnvironmentLights& Lights)
{
	const FName Folder(TEXT("Environment"));

	// Unreal's ocean material takes the absorption as a distance (cm) and the scattering in 1/m (alpha).
	const FLBAWaterLook Look = ComputeLook(Preset, Lights);
	const FLinearColor Absorption(1.f / Look.Absorption.R, 1.f / Look.Absorption.G, 1.f / Look.Absorption.B, 1.f);
	const FLinearColor Scattering(1.f, 1.f, 1.f, WaterScattering * 1000.f);
	FLBAWaterSea Sea;
	Sea.NeutralLight = Look.NeutralLight;
	UE_LOG(LogLBA2, Display, TEXT("LBA2 water %s: sea %s, albedo %s, absorption distance %s cm, neutral light %.2f"), *Settings.Name,
		*Preset.SeaColor.ToString(), *Look.Albedo.ToString(), *Absorption.ToString(), Sea.NeutralLight);

	Sea.Material = MakeWaterMaterial(Context, Settings.MaterialFolder, FString::Printf(TEXT("MI_%s_Ocean"), *Settings.Name),
		GetClippedOceanMaterial(Context), Absorption, Scattering);
	UMaterialInterface* FarMaterial = MakeWaterMaterial(Context, Settings.MaterialFolder, FString::Printf(TEXT("MI_%s_OceanFar"), *Settings.Name),
		LoadObject<UMaterialInterface>(nullptr, TEXT("/Water/Materials/WaterSurface/Water_FarMesh.Water_FarMesh")), Absorption, Scattering);
	Sea.Waves = GetWavesAsset(Context, Preset.bStormSea ? StormWaves : CalmWaves);

	// The zone covers the island and the sea around it; the ocean goes on beyond it to the horizon (its far mesh).
	AWaterZone* Zone = SpawnZone(Context, World, FBox2D(FVector2D(Settings.Bounds.Min), FVector2D(Settings.Bounds.Max)), MaxLevelWaterResolution,
		FarMaterial, Folder);
	if (!Zone)
	{
		return Sea;
	}
	for (AActor* Terrain : Settings.TerrainActors)
	{
		if (Terrain)
		{
			UWaterTerrainComponent* Component = NewObject<UWaterTerrainComponent>(Terrain, TEXT("WaterTerrain"));
			Terrain->AddInstanceComponent(Component);
			Component->RegisterComponent();
		}
	}
	SpawnOcean(Context, World, Zone, Settings.LandPoint, Settings.Coastline, FBox2D(ForceInit), Sea.Material, Sea.Waves, TEXT("Ocean"), Folder);
	Zone->MarkForRebuild(EWaterZoneRebuildFlags::All);
	return Sea;
}

void LBAWater::SpawnGlobeSea(FLBAImportContext& Context, UWorld* World, const FLBAGlobeSeaSettings& Settings)
{
	const FName Folder(TEXT("Sea"));
	AWaterZone* Zone = SpawnZone(Context, World, Settings.Bounds, MaxGlobeWaterResolution, nullptr, Folder);
	if (!Zone)
	{
		return;
	}
	for (const FLBAGlobeSeaSettings::FIsland& Island : Settings.Islands)
	{
		if (!Island.Cell.bIsValid)
		{
			continue;
		}
		// Without a coastline, the ocean's dry loop goes in its cell's corner, out at sea.
		TArray<FVector2D> Coastline;
		for (const FVector2D& Point : Island.Coastline)
		{
			Coastline.Add(Point.GetRotated(-Settings.CellYaw));
		}
		SpawnOcean(Context, World, Zone, Island.Cell.Min + FVector2D(200.0), Coastline, Island.Cell, Settings.Material, Settings.Waves,
			TEXT("Ocean_") + Island.Name, Folder, Settings.CellYaw);
	}
	Zone->MarkForRebuild(EWaterZoneRebuildFlags::All);
	UE_LOG(LogLBA2, Display, TEXT("LBA2 globe sea: zone %.0f x %.0f m around (%.0f, %.0f) m, %d texels across, %d islands"),
		Settings.Bounds.GetSize().X / 100.0, Settings.Bounds.GetSize().Y / 100.0, Settings.Bounds.GetCenter().X / 100.0,
		Settings.Bounds.GetCenter().Y / 100.0, GetZoneResolution(Settings.Bounds.GetSize().GetMax(), MaxGlobeWaterResolution), Settings.Islands.Num());
}
