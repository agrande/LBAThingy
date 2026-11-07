#include "Import/LBARenderingAssets.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "Formats/LBAXpl.h"
#include "Import/LBADither3DTextures.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "LBAStormWind.h"
#include "LBATypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionParticleDirection.h"
#include "Materials/MaterialExpressionParticlePositionWS.h"
#include "Materials/MaterialExpressionParticleRandom.h"
#include "Materials/MaterialExpressionParticleRelativeTime.h"
#include "Materials/MaterialExpressionParticleSize.h"
#include "Materials/MaterialExpressionPreviousFrameSwitch.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "TextureCompiler.h"

namespace
{
	constexpr int32 LutSize = 64;
	constexpr int32 DitherRecursion = 3;

	/** Generated texture settings shared by the lookup tables: exact texel values, never streamed or resized. */
	void SetupDataTexture(UTexture* Texture, TextureFilter Filter)
	{
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Grayscale;
		Texture->CompressionNone = true;
		Texture->Filter = Filter;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->LODGroup = TEXTUREGROUP_ColorLookupTable;
		Texture->NeverStream = true;
	}

	/**
	 * The rain materials' density map (RainDensityMap, sampled at mip 0 in both vertex and pixel shaders): white, so
	 * full rain, until a level sets its own (see LBAEnvironment).
	 */
	UMaterialExpression* AddRainDensityMap(UMaterial* Material, int32 X, int32 Y)
	{
		UMaterialExpressionTextureObjectParameter* Node = Cast<UMaterialExpressionTextureObjectParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), X, Y));
		Node->ParameterName = TEXT("RainDensityMap");
		Node->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		Node->AutoSetSampleType();
		Node->Group = TEXT("Rain");
		return Node;
	}

	/** Feeds a custom node's input from its vector parameter's RGBA pin rather than the default RGB one. */
	void ConnectRgba(UMaterialExpressionCustom* Custom, const TCHAR* InputName)
	{
		constexpr int32 RgbaOutput = 5;
		for (FCustomInput& Input : Custom->Inputs)
		{
			if (Input.InputName == InputName && Input.Input.Expression)
			{
				Input.Input.Connect(RgbaOutput, Input.Input.Expression);
			}
		}
	}

	template <typename T>
	T* PrepareTexture(FLBAImportContext& Context, const FString& Folder, const FString& Name)
	{
		T* Texture = Context.CreateOrReuseAsset<T>(Folder, Name);
		if (Texture && Texture->IsCompiling())
		{
			FTextureCompilingManager::Get().FinishCompilation({ Texture });
		}
		if (Texture)
		{
			Texture->PreEditChange(nullptr);
		}
		return Texture;
	}
}

FLBARenderingAssets::FLBARenderingAssets(FLBAImportContext& InContext)
	: Context(InContext)
{
}

FString FLBARenderingAssets::GetFolder() const
{
	return Context.GetContentPath(TEXT("Rendering"));
}

bool LBAMaterials::ResetInstanceIfOutdated(UMaterialInstanceConstant* Instance, UMaterialInterface* Parent, int32 Version)
{
	float Stamp = 0.f;
	const bool bCurrent = Instance->Parent == Parent
		&& Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("PresetVersion")), Stamp, true)
		&& Stamp >= float(Version);
	if (bCurrent)
	{
		return false;
	}
	UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(Instance);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("PresetVersion"), float(Version));
	return true;
}

UMaterialInterface* FLBARenderingAssets::GetLook(ELBALookMode Look)
{
	// Bump when the defaults below change: the looks are then reset, user edits are otherwise kept.
	constexpr int32 Version = 8;

	UMaterialInterface*& Cached = Looks[uint8(Look)];
	if (Cached)
	{
		return Cached;
	}
	UMaterial* Parent = GetMaster();
	if (!Parent)
	{
		return nullptr;
	}

	const bool bEnhanced = Look == ELBALookMode::Enhanced;
	UMaterialInstanceConstant* Instance = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(GetFolder(),
		bEnhanced ? TEXT("MI_LBA_Look_Enhanced") : TEXT("MI_LBA_Look_Original"));
	if (!Instance)
	{
		return Cached = Parent;
	}
	if (LBAMaterials::ResetInstanceIfOutdated(Instance, Parent, Version) && bEnhanced)
	{
		// The master's defaults are the original look. The enhanced one bends the light for deeper shadows and
		// brighter highlights, mixes rows further apart with bigger dots so the dither reads as a comic screen, tints
		// shadows cool and highlights warm (islands replace the shadow tint with their sky color), and adds color.
		auto Scalar = [Instance](const TCHAR* Name, float Value)
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Name, Value);
		};
		auto Vector = [Instance](const TCHAR* Name, const FLinearColor& Value)
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, Name, Value);
		};
		Scalar(TEXT("LightContrast"), 1.5f);
		Scalar(TEXT("RowStep"), 2.f);
		Scalar(TEXT("LightTint"), 0.35f);
		Scalar(TEXT("Exposure"), 0.f);
		Scalar(TEXT("Contrast"), 1.15f);
		Scalar(TEXT("Saturation"), 1.2f);
		Vector(TEXT("ShadowTint"), FLinearColor(0.55f, 0.65f, 1.f));
		Scalar(TEXT("ShadowTintAmount"), 0.25f);
		Vector(TEXT("HighlightTint"), FLinearColor(1.f, 0.85f, 0.6f));
		Scalar(TEXT("HighlightTintAmount"), 0.15f);
		Scalar(TEXT("DotPattern"), 0.f);
		Scalar(TEXT("HeightFogAmount"), 1.f);
		Scalar(TEXT("DitherAmount"), 0.4f);
		Scalar(TEXT("DotSpacing"), 3.f);
		Scalar(TEXT("DotContrast"), 1.5f);
		Scalar(TEXT("DotSizeVariability"), 0.5f);
	}
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Context.MarkForSave(Instance);
	return Cached = Instance;
}

UMaterialInterface* FLBARenderingAssets::GetPaletteShading(const FLBAXpl& Xpl, ELBALookMode Look)
{
	// Bump when the generated parameters below change.
	constexpr int32 Version = 1;

	if (UMaterialInterface** Found = Instances.Find({ Xpl.Entry, Look }))
	{
		return *Found;
	}
	UMaterialInterface* Parent = GetLook(Look);
	if (!Parent)
	{
		return nullptr;
	}

	const bool bEnhanced = Look == ELBALookMode::Enhanced;
	const FString Name = FLBAXpl::GetName(Xpl.Entry);
	UTexture2D* Shades = CreateShadesTexture(Xpl, TEXT("T_LBA_Shades_") + Name);
	UVolumeTexture* Lut = CreatePaletteLut(Xpl, TEXT("VT_LBA_PaletteLut_") + Name);

	UMaterialInstanceConstant* Instance = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(GetFolder(),
		(bEnhanced ? TEXT("MI_LBA_Enhanced_") : TEXT("MI_LBA_Original_")) + Name);
	if (!Instance)
	{
		return Parent;
	}
	// The original look is fully generated; the enhanced one keeps per island tweaks.
	bool bReset = true;
	if (bEnhanced)
	{
		bReset = LBAMaterials::ResetInstanceIfOutdated(Instance, Parent, Version);
	}
	else
	{
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
		UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(Instance);
	}
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Shades"), Shades);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("PaletteLut"), Lut);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("NeutralRow"), float(Xpl.ShadeNormalLevel));
	if (bEnhanced && bReset)
	{
		// Shadows take the hue of the island's sky, which is what lights them.
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("ShadowTint"), FLinearColor::FromSRGBColor(Xpl.Palette[Xpl.FogColor]));
	}
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Context.MarkForSave(Instance);
	Instances.Add({ Xpl.Entry, Look }, Instance);
	return Instance;
}

UTexture2D* FLBARenderingAssets::CreateShadesTexture(const FLBAXpl& Xpl, const FString& Name)
{
	// 256 x 18: the 16 shade rows of fog level 0, then the palette itself. sRGB bytes in a linear texture so the
	// shader reads exact palette values.
	constexpr int32 PaletteRow = FLBAXpl::NumShadeRows;
	constexpr int32 BrightnessRow = PaletteRow + 1;
	constexpr int32 Height = BrightnessRow + 1;
	TArray<FColor> Pixels;
	Pixels.Init(FColor::Black, 256 * Height);
	for (int32 Row = 0; Row < BrightnessRow; ++Row)
	{
		for (int32 Index = 0; Index < 256; ++Index)
		{
			const int32 Shaded = Row < FLBAXpl::NumShadeRows ? Xpl.GetShade(Index, Row) : Index;
			Pixels[Row * 256 + Index] = Xpl.Palette[Shaded];
		}
	}
	// Last row, first 16 texels: the palette's typical brightness in each shade row relative to the unshaded color
	// (median over its colors, linear luminance / BrightnessScale, stored sRGB), for pre-drawn art (LBAPaletteShading.ush).
	constexpr float BrightnessScale = 4.f;
	auto Luma = [&Xpl](int32 Index)
	{
		const FLinearColor C = FLinearColor::FromSRGBColor(Xpl.Palette[Index]);
		return 0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B;
	};
	for (int32 Row = 0; Row < FLBAXpl::NumShadeRows; ++Row)
	{
		TArray<float> Ratios;
		for (int32 Index = 0; Index < 256; ++Index)
		{
			if (Luma(Index) > 0.01f)
			{
				Ratios.Add(Luma(Xpl.GetShade(Index, Row)) / Luma(Index));
			}
		}
		Ratios.Sort();
		const float Median = Ratios.IsEmpty() ? float(Row) / float(Xpl.ShadeNormalLevel) : Ratios[Ratios.Num() / 2];
		Pixels[BrightnessRow * 256 + Row] = FLinearColor(FMath::Clamp(Median / BrightnessScale, 0.f, 1.f), 0.f, 0.f).ToFColor(true);
	}

	UTexture2D* Texture = PrepareTexture<UTexture2D>(Context, GetFolder(), Name);
	if (!Texture)
	{
		return nullptr;
	}
	Texture->Source.Init(256, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
	SetupDataTexture(Texture, TF_Nearest);
	Texture->CompressionSettings = TC_VectorDisplacementmap;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->PostEditChange();
	Context.MarkForSave(Texture);
	return Texture;
}

UVolumeTexture* FLBARenderingAssets::CreatePaletteLut(const FLBAXpl& Xpl, const FString& Name)
{
	// Nearest palette color (weighted sRGB distance) of every 6-bit-per-channel color. Index 0 is the color key
	// (black, also at the bottom of the ramps), so it is skipped.
	TArray<uint8> Data;
	Data.SetNumUninitialized(LutSize * LutSize * LutSize);
	ParallelFor(LutSize, [&](int32 B)
	{
		for (int32 G = 0; G < LutSize; ++G)
		{
			for (int32 R = 0; R < LutSize; ++R)
			{
				const FIntVector Color(R * 255 / (LutSize - 1), G * 255 / (LutSize - 1), B * 255 / (LutSize - 1));
				int32 Best = 0;
				int32 BestDistance = MAX_int32;
				for (int32 Index = 1; Index < 256; ++Index)
				{
					const FColor& P = Xpl.Palette[Index];
					const int32 DR = int32(P.R) - Color.X;
					const int32 DG = int32(P.G) - Color.Y;
					const int32 DB = int32(P.B) - Color.Z;
					const int32 Distance = 2 * DR * DR + 4 * DG * DG + 3 * DB * DB;
					if (Distance < BestDistance)
					{
						BestDistance = Distance;
						Best = Index;
					}
				}
				Data[(B * LutSize + G) * LutSize + R] = uint8(Best);
			}
		}
	});

	UVolumeTexture* Texture = PrepareTexture<UVolumeTexture>(Context, GetFolder(), Name);
	if (!Texture)
	{
		return nullptr;
	}
	Texture->Source.Init(LutSize, LutSize, LutSize, 1, TSF_G8, Data.GetData());
	SetupDataTexture(Texture, TF_Nearest);
	Texture->AddressMode = TA_Clamp;
	Texture->PostEditChange();
	Context.MarkForSave(Texture);
	return Texture;
}

void FLBARenderingAssets::CreateHalftoneRamp()
{
	// Threshold of the halftone spot function (LBAHalftoneSpot in LBAHalftone.ush) above which a given fraction of
	// the screen lies, from the sorted spot values of one cell.
	constexpr int32 Samples = 512;
	constexpr int32 Width = 256;
	TArray<float> Spots;
	Spots.SetNumUninitialized(Samples * Samples);
	for (int32 Y = 0; Y < Samples; ++Y)
	{
		for (int32 X = 0; X < Samples; ++X)
		{
			const float CX = FMath::Cos(2.f * UE_PI * (X + 0.5f) / Samples);
			const float CY = FMath::Cos(2.f * UE_PI * (Y + 0.5f) / Samples);
			Spots[Y * Samples + X] = 0.5f + 0.5f * CX * CY;
		}
	}
	Spots.Sort();

	TArray<uint16> Ramp;
	Ramp.SetNumUninitialized(Width);
	for (int32 I = 0; I < Width; ++I)
	{
		const float Coverage = float(I) / (Width - 1);
		const int32 Below = FMath::Clamp(FMath::RoundToInt((1.f - Coverage) * Spots.Num()), 0, Spots.Num() - 1);
		Ramp[I] = uint16(FMath::RoundToInt(FMath::Clamp(Spots[Below], 0.f, 1.f) * 65535.f));
	}

	HalftoneRamp = PrepareTexture<UTexture2D>(Context, GetFolder(), TEXT("T_LBA_HalftoneRamp"));
	if (HalftoneRamp)
	{
		HalftoneRamp->Source.Init(Width, 1, 1, 1, TSF_G16, reinterpret_cast<const uint8*>(Ramp.GetData()));
		SetupDataTexture(HalftoneRamp, TF_Bilinear);
		HalftoneRamp->AddressX = TA_Clamp;
		HalftoneRamp->AddressY = TA_Clamp;
		HalftoneRamp->PostEditChange();
		Context.MarkForSave(HalftoneRamp);
	}
}

void FLBARenderingAssets::CreateDitherTextures()
{
	const FLBADither3DPattern Pattern = FLBADither3DPattern::Build(DitherRecursion);
	DitherDotsPerSide = Pattern.DotsPerSide;

	DitherTexture = PrepareTexture<UVolumeTexture>(Context, GetFolder(), TEXT("VT_LBA_Dither3D"));
	if (DitherTexture)
	{
		DitherTexture->Source.Init(Pattern.Size, Pattern.Size, Pattern.Layers, 1, TSF_G8, Pattern.Dots.GetData());
		SetupDataTexture(DitherTexture, TF_Bilinear);
		DitherTexture->AddressMode = TA_Wrap;
		DitherTexture->PostEditChange();
		Context.MarkForSave(DitherTexture);
	}

	DitherRamp = PrepareTexture<UTexture2D>(Context, GetFolder(), TEXT("T_LBA_DitherRamp"));
	if (DitherRamp)
	{
		DitherRamp->Source.Init(Pattern.Size, 1, 1, 1, TSF_G16, reinterpret_cast<const uint8*>(Pattern.Ramp.GetData()));
		SetupDataTexture(DitherRamp, TF_Bilinear);
		DitherRamp->AddressX = TA_Clamp;
		DitherRamp->AddressY = TA_Clamp;
		DitherRamp->PostEditChange();
		Context.MarkForSave(DitherRamp);
	}
}

UMaterial* FLBARenderingAssets::GetMaster()
{
	if (!Master)
	{
		Master = CreateMaster();
	}
	return Master;
}

UMaterial* FLBARenderingAssets::CreateMaster()
{
	// Bump when the graph below changes: existing masters are rebuilt in place, user edits are otherwise kept.
	// v31: globe mode's bend (MPC_LBA_Planet's PlanetBend and PlanetView). v32: pre-drawn art (ArtStencil, CustomDepth).
	// v33: globe mode's sun of each surface's place (MPC_LBA_Planet's PlanetSun*). v35: day sky high up (PlanetSkyDay*).
	// v36: globe mode's sea by place, ice and turquoise (SeaRegionTexture, SeaRegions, PlanetSea*), and its own sunset.
	constexpr int32 Version = 36;

	CreateDitherTextures();
	UMaterialParameterCollection* PlanetCollection = GetPlanetParameters();
	CreateHalftoneRamp();

	bool bExisted = false;
	UMaterial* Material = Context.CreateOrReuseAsset<UMaterial>(GetFolder(), TEXT("M_LBA_PaletteShading"), &bExisted);
	if (!Material || (bExisted && LBAMaterials::IsGeneratedVersion(Material, Version) && !bPlanetParametersChanged))
	{
		return Material;
	}
	LBAMaterials::BeginRebuild(Material, Version);

	Material->MaterialDomain = MD_PostProcess;
	Material->BlendableLocation = BL_SceneColorAfterDOF;

	auto Add = [Material](UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	};
	auto SceneTexture = [&](ESceneTextureId Id, int32 Y)
	{
		UMaterialExpressionSceneTexture* Node = Cast<UMaterialExpressionSceneTexture>(Add(UMaterialExpressionSceneTexture::StaticClass(), -900, Y));
		Node->SceneTextureId = Id;
		return Node;
	};
	auto TextureObject = [&](const TCHAR* Name, UTexture* Default, int32 Y)
	{
		UMaterialExpressionTextureObjectParameter* Node = Cast<UMaterialExpressionTextureObjectParameter>(
			Add(UMaterialExpressionTextureObjectParameter::StaticClass(), -900, Y));
		Node->ParameterName = Name;
		Node->Texture = Default;
		Node->AutoSetSampleType();
		Node->Group = TEXT("Textures");
		return Node;
	};
	// Parameters are laid out in one column, group by group.
	int32 ParameterY = -400;
	auto Scalar = [&](const TCHAR* Name, float Default, const TCHAR* Group)
	{
		UMaterialExpressionScalarParameter* Node = Cast<UMaterialExpressionScalarParameter>(Add(UMaterialExpressionScalarParameter::StaticClass(), -600, ParameterY));
		ParameterY += 80;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = Group;
		return Node;
	};
	auto Vector = [&](const TCHAR* Name, const FLinearColor& Default, const TCHAR* Group)
	{
		UMaterialExpressionVectorParameter* Node = Cast<UMaterialExpressionVectorParameter>(Add(UMaterialExpressionVectorParameter::StaticClass(), -600, ParameterY));
		ParameterY += 160;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = Group;
		return Node;
	};

	UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Add(UMaterialExpressionCustom::StaticClass(), -250, 0));
	Custom->Description = TEXT("LBA palette shading");
	Custom->OutputType = CMOT_Float3;
	Custom->IncludeFilePaths = { TEXT("/Plugin/LBA2Importer/Private/LBAPaletteShading.ush") };
	Custom->Inputs.Reset();

	struct FInput
	{
		const TCHAR* Name;
		UMaterialExpression* Expression;
	};
	// Globe mode's bend of the view (MPC_LBA_Planet, LBAPlanet.ush).
	auto Planet = [&](const TCHAR* Name, int32 Y) -> UMaterialExpression*
	{
		UMaterialExpressionCollectionParameter* Node = Cast<UMaterialExpressionCollectionParameter>(Add(UMaterialExpressionCollectionParameter::StaticClass(), -900, Y));
		Node->Collection = PlanetCollection;
		Node->ParameterName = Name;
		Node->ParameterId = PlanetCollection ? PlanetCollection->GetParameterId(Name) : FGuid();
		return Node;
	};
	// Defaults are the original look; MI_LBA_Look_Enhanced overrides them (FLBARenderingAssets::GetLook).
	UTexture2D* DefaultShades = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	const FInput Inputs[] =
	{
		{ TEXT("SceneColor"), SceneTexture(PPI_PostProcessInput0, 0) },
		{ TEXT("BaseColor"), SceneTexture(PPI_BaseColor, 120) },
		{ TEXT("Depth"), SceneTexture(PPI_SceneDepth, 240) },
		{ TEXT("Stencil"), SceneTexture(PPI_CustomStencil, 360) },
		{ TEXT("CustomDepth"), SceneTexture(PPI_CustomDepth, 400) },
		{ TEXT("ShadingModel"), SceneTexture(PPI_ShadingModelID, 420) },
		{ TEXT("WorldNormal"), SceneTexture(PPI_WorldNormal, 440) },
		{ TEXT("Lut"), TextureObject(TEXT("PaletteLut"), DitherTexture, 480) },
		{ TEXT("Shades"), TextureObject(TEXT("Shades"), DefaultShades, 600) },
		{ TEXT("Dither"), TextureObject(TEXT("Dither3D"), DitherTexture, 720) },
		{ TEXT("DitherRamp"), TextureObject(TEXT("DitherRamp"), DitherRamp, 840) },
		{ TEXT("HalftoneRamp"), TextureObject(TEXT("HalftoneRamp"), HalftoneRamp, 960) },
		// The island's sea texture, set per level (LBAEnvironment), for cartoon water.
		{ TEXT("SeaTexture"), TextureObject(TEXT("SeaTexture"), LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/Black.Black")), 1080) },
		// Globe mode's sea by place: the island's sea texture recolored toward ice and turquoise (set per level,
		// LBAEnvironment), and the planet's mask of where (set in play, ULBAPlanetSubsystem).
		{ TEXT("SeaRegionTexture"), TextureObject(TEXT("SeaRegionTexture"), LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/Black.Black")), 1100) },
		{ TEXT("SeaRegions"), TextureObject(TEXT("SeaRegions"), LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/Black.Black")), 1140) },
		{ TEXT("PlanetBend"), Planet(TEXT("PlanetBend"), 1200) },
		{ TEXT("PlanetView"), Planet(TEXT("PlanetView"), 1280) },
		{ TEXT("PlanetSunPlaces"), Planet(TEXT("PlanetSunPlaces"), 1360) },
		{ TEXT("PlanetSunAngles"), Planet(TEXT("PlanetSunAngles"), 1440) },
		{ TEXT("PlanetSunLight"), Planet(TEXT("PlanetSunLight"), 1520) },
		{ TEXT("PlanetSunTint"), Planet(TEXT("PlanetSunTint"), 1600) },
		{ TEXT("PlanetSunAmbient"), Planet(TEXT("PlanetSunAmbient"), 1680) },
		{ TEXT("PlanetSunCamera"), Planet(TEXT("PlanetSunCamera"), 1760) },
		{ TEXT("PlanetSkyDayZenith"), Planet(TEXT("PlanetSkyDayZenith"), 1840) },
		{ TEXT("PlanetSkyDayHorizon"), Planet(TEXT("PlanetSkyDayHorizon"), 1920) },
		{ TEXT("PlanetSeaFrame"), Planet(TEXT("PlanetSeaFrame"), 2000) },
		{ TEXT("PlanetSeaIce"), Planet(TEXT("PlanetSeaIce"), 2080) },
		{ TEXT("PlanetSeaTurquoise"), Planet(TEXT("PlanetSeaTurquoise"), 2160) },
		{ TEXT("PlanetSeaSunset"), Planet(TEXT("PlanetSeaSunset"), 2240) },
		{ TEXT("Strength"), Scalar(TEXT("Strength"), 1.f, TEXT("Palette")) },
		{ TEXT("NeutralRow"), Scalar(TEXT("NeutralRow"), 12.f, TEXT("Palette")) },
		{ TEXT("NeutralLight"), Scalar(TEXT("NeutralLight"), 1.f, TEXT("Palette")) },
		{ TEXT("ShadeContrast"), Scalar(TEXT("ShadeContrast"), 1.f, TEXT("Palette")) },
		{ TEXT("KeepLuminance"), Scalar(TEXT("KeepLuminance"), 0.f, TEXT("Palette")) },
		{ TEXT("LightTint"), Scalar(TEXT("LightTint"), 0.3f, TEXT("Palette")) },
		{ TEXT("SkyDepth"), Scalar(TEXT("SkyDepth"), 1.0e7f, TEXT("Palette")) },
		{ TEXT("LightContrast"), Scalar(TEXT("LightContrast"), 1.f, TEXT("Grade")) },
		{ TEXT("ContrastPivot"), Scalar(TEXT("ContrastPivot"), 1.f, TEXT("Grade")) },
		{ TEXT("RowStep"), Scalar(TEXT("RowStep"), 1.f, TEXT("Grade")) },
		{ TEXT("Exposure"), Scalar(TEXT("Exposure"), 0.f, TEXT("Grade")) },
		{ TEXT("Contrast"), Scalar(TEXT("Contrast"), 1.f, TEXT("Grade")) },
		{ TEXT("Saturation"), Scalar(TEXT("Saturation"), 1.f, TEXT("Grade")) },
		{ TEXT("ShadowTint"), Vector(TEXT("ShadowTint"), FLinearColor(0.55f, 0.65f, 1.f), TEXT("Grade")) },
		{ TEXT("ShadowTintAmount"), Scalar(TEXT("ShadowTintAmount"), 0.f, TEXT("Grade")) },
		{ TEXT("HighlightTint"), Vector(TEXT("HighlightTint"), FLinearColor(1.f, 0.85f, 0.6f), TEXT("Grade")) },
		{ TEXT("HighlightTintAmount"), Scalar(TEXT("HighlightTintAmount"), 0.f, TEXT("Grade")) },
		{ TEXT("DitherAmount"), Scalar(TEXT("DitherAmount"), 1.f, TEXT("Dither")) },
		{ TEXT("DotPattern"), Scalar(TEXT("DotPattern"), 0.f, TEXT("Dither")) },
		{ TEXT("DotSpacing"), Scalar(TEXT("DotSpacing"), 2.f, TEXT("Dither")) },
		{ TEXT("DotContrast"), Scalar(TEXT("DotContrast"), 1.5f, TEXT("Dither")) },
		{ TEXT("DotSizeVariability"), Scalar(TEXT("DotSizeVariability"), 0.f, TEXT("Dither")) },
		{ TEXT("StretchSmoothness"), Scalar(TEXT("StretchSmoothness"), 1.f, TEXT("Dither")) },
		{ TEXT("ExcludedStencil"), Scalar(TEXT("ExcludedStencil"), float(LBA::MovingObjectStencil), TEXT("Dither")) },
		{ TEXT("ArtStencil"), Scalar(TEXT("ArtStencil"), float(LBA::PredrawnArtStencil), TEXT("Palette")) },
		// The sea's color and neutral light are set per level (LBAEnvironment).
		{ TEXT("WaterColor"), Vector(TEXT("WaterColor"), FLinearColor::Black, TEXT("Water")) },
		{ TEXT("WaterNeutralLight"), Scalar(TEXT("WaterNeutralLight"), 1.f, TEXT("Water")) },
		{ TEXT("WaterLightTint"), Scalar(TEXT("WaterLightTint"), 0.35f, TEXT("Water")) },
		{ TEXT("WaterStrength"), Scalar(TEXT("WaterStrength"), 1.f, TEXT("Water")) },
		{ TEXT("WaterHighlightRange"), Scalar(TEXT("WaterHighlightRange"), 0.2f, TEXT("Water")) },
		// Cartoon water, cm and seconds; the sea texture's tile size is set per level.
		{ TEXT("SeaTileSize"), Scalar(TEXT("SeaTileSize"), 1024.f, TEXT("Water Cartoon")) },
		{ TEXT("SeaDrift"), Vector(TEXT("SeaDrift"), FLinearColor(6.f, 2.5f, 0.f), TEXT("Water Cartoon")) },
		{ TEXT("SeaCellSize"), Scalar(TEXT("SeaCellSize"), 1.f, TEXT("Water Cartoon")) },
		{ TEXT("SeaVariation"), Scalar(TEXT("SeaVariation"), 1.f, TEXT("Water Cartoon")) },
		{ TEXT("SeaTransition"), Scalar(TEXT("SeaTransition"), 0.7f, TEXT("Water Cartoon")) },
		{ TEXT("SeaWobble"), Scalar(TEXT("SeaWobble"), 60.f, TEXT("Water Cartoon")) },
		{ TEXT("WaterSmoothing"), Scalar(TEXT("WaterSmoothing"), 80.f, TEXT("Water Cartoon")) },
		{ TEXT("FoamWidth"), Scalar(TEXT("FoamWidth"), 60.f, TEXT("Water Cartoon")) },
		{ TEXT("FoamSpeed"), Scalar(TEXT("FoamSpeed"), 0.35f, TEXT("Water Cartoon")) },
		{ TEXT("FoamBrightness"), Scalar(TEXT("FoamBrightness"), 1.f, TEXT("Water Cartoon")) },
		{ TEXT("SparkleLight"), Scalar(TEXT("SparkleLight"), 1.8f, TEXT("Water Cartoon")) },
		{ TEXT("FogColor"), Vector(TEXT("FogColor"), FLinearColor(0.45f, 0.6f, 0.8f), TEXT("Fog")) },
		{ TEXT("FogStart"), Scalar(TEXT("FogStart"), 0.f, TEXT("Fog")) },
		{ TEXT("FogEnd"), Scalar(TEXT("FogEnd"), 0.f, TEXT("Fog")) },
		{ TEXT("HeightFogAmount"), Scalar(TEXT("HeightFogAmount"), 0.f, TEXT("Fog")) },
		// Copy of the level's height fog (FLBAHeightFogParameters), kept in sync with the fog actor during play.
		{ TEXT("HeightFogDensity"), Scalar(TEXT("HeightFogDensity"), 0.f, TEXT("Height Fog")) },
		{ TEXT("HeightFogFalloff"), Scalar(TEXT("HeightFogFalloff"), 0.0002f, TEXT("Height Fog")) },
		{ TEXT("HeightFogHeight"), Scalar(TEXT("HeightFogHeight"), 0.f, TEXT("Height Fog")) },
		{ TEXT("HeightFogStart"), Scalar(TEXT("HeightFogStart"), 0.f, TEXT("Height Fog")) },
		{ TEXT("HeightFogMaxOpacity"), Scalar(TEXT("HeightFogMaxOpacity"), 1.f, TEXT("Height Fog")) },
		{ TEXT("HeightFogInscatter"), Vector(TEXT("HeightFogInscatter"), FLinearColor::Black, TEXT("Height Fog")) },
		{ TEXT("HeightFogSun"), Vector(TEXT("HeightFogSun"), FLinearColor::Black, TEXT("Height Fog")) },
		{ TEXT("HeightFogSunExponent"), Scalar(TEXT("HeightFogSunExponent"), 4.f, TEXT("Height Fog")) },
		{ TEXT("HeightFogSunStart"), Scalar(TEXT("HeightFogSunStart"), 10000.f, TEXT("Height Fog")) },
		{ TEXT("HeightFogVolumetricDistance"), Scalar(TEXT("HeightFogVolumetricDistance"), 0.f, TEXT("Height Fog")) },
		{ TEXT("HeightFogPhaseG"), Scalar(TEXT("HeightFogPhaseG"), 0.2f, TEXT("Height Fog")) },
		{ TEXT("HeightFogVolumetricStart"), Scalar(TEXT("HeightFogVolumetricStart"), 0.f, TEXT("Height Fog")) },
		{ TEXT("DebugView"), Scalar(TEXT("DebugView"), 0.f, TEXT("Debug")) },
		// Not used by the shader: stamps generated instances (LBAMaterials::ResetInstanceIfOutdated).
		{ TEXT("PresetVersion"), Scalar(TEXT("PresetVersion"), 0.f, TEXT("Debug")) },
	};
	for (const FInput& Input : Inputs)
	{
		FCustomInput& CustomInput = Custom->Inputs.AddDefaulted_GetRef();
		CustomInput.InputName = Input.Name;
		CustomInput.Input.Connect(0, Input.Expression);
	}

	Custom->Code = FString::Printf(TEXT(
		"FLBAPaletteShadingSettings S;\n"
		"S.Strength = Strength;\n"
		"S.NeutralRow = NeutralRow;\n"
		"S.NeutralLight = max(NeutralLight, 0.01);\n"
		"S.ShadeContrast = ShadeContrast;\n"
		"S.LightContrast = max(LightContrast, 0.05);\n"
		"S.ContrastPivot = ContrastPivot;\n"
		"S.RowStep = RowStep;\n"
		"S.KeepLuminance = KeepLuminance;\n"
		"S.DitherAmount = DitherAmount;\n"
		"S.DotPattern = DotPattern;\n"
		"S.LightTint = LightTint;\n"
		"S.ShadowTint = ShadowTint.rgb;\n"
		"S.ShadowTintAmount = ShadowTintAmount;\n"
		"S.HighlightTint = HighlightTint.rgb;\n"
		"S.HighlightTintAmount = HighlightTintAmount;\n"
		"S.Exposure = Exposure;\n"
		"S.Contrast = max(Contrast, 0.05);\n"
		"S.Saturation = Saturation;\n"
		"S.ExcludedStencil = ExcludedStencil;\n"
		"S.ArtStencil = ArtStencil;\n"
		"S.WaterColor = WaterColor.rgb;\n"
		"S.WaterNeutralLight = max(WaterNeutralLight, 0.01);\n"
		"S.WaterLightTint = WaterLightTint;\n"
		"S.WaterStrength = WaterStrength;\n"
		"S.WaterHighlightRange = WaterHighlightRange;\n"
		"S.Cartoon.TileSize = SeaTileSize;\n"
		"S.Cartoon.Drift = SeaDrift.xy;\n"
		"S.Cartoon.CellSize = SeaCellSize;\n"
		"S.Cartoon.Variation = SeaVariation;\n"
		"S.Cartoon.Transition = SeaTransition;\n"
		"S.Cartoon.Wobble = SeaWobble;\n"
		"S.Cartoon.Smoothing = WaterSmoothing;\n"
		"S.Cartoon.FoamWidth = FoamWidth;\n"
		"S.Cartoon.FoamSpeed = FoamSpeed;\n"
		"S.Cartoon.FoamBrightness = FoamBrightness;\n"
		"S.Cartoon.SparkleLight = SparkleLight;\n"
		"S.SkyDepth = SkyDepth;\n"
		"S.DebugView = DebugView;\n"
		"S.FogColor = FogColor.rgb;\n"
		"S.FogStart = FogStart;\n"
		"S.FogEnd = FogEnd;\n"
		"S.HeightFogAmount = HeightFogAmount;\n"
		"S.HeightFog.Density = HeightFogDensity;\n"
		"S.HeightFog.HeightFalloff = HeightFogFalloff;\n"
		"S.HeightFog.Height = HeightFogHeight;\n"
		"S.HeightFog.StartDistance = HeightFogStart;\n"
		"S.HeightFog.MaxOpacity = HeightFogMaxOpacity;\n"
		"S.HeightFog.Inscattering = HeightFogInscatter.rgb;\n"
		"S.HeightFog.SunInscattering = HeightFogSun.rgb;\n"
		"S.HeightFog.SunExponent = HeightFogSunExponent;\n"
		"S.HeightFog.SunStartDistance = HeightFogSunStart;\n"
		"S.HeightFog.VolumetricStart = HeightFogVolumetricStart;\n"
		"S.HeightFog.VolumetricDistance = HeightFogVolumetricDistance;\n"
		"S.HeightFog.VolumetricPhaseG = HeightFogPhaseG;\n"
		"S.Dither.DotsPerSide = %d;\n"
		"S.Dither.ScaleExp = max(DotSpacing, 1.0) * 8.0;\n"
		"S.Dither.SizeVariability = DotSizeVariability;\n"
		"S.Dither.Contrast = DotContrast;\n"
		"S.Dither.StretchSmoothness = StretchSmoothness;\n"
		"S.Halftone.DotPixels = max(DotSpacing, 1.0);\n"
		"S.Halftone.Contrast = DotContrast;\n"
		"S.Planet.Bend = PlanetBend;\n"
		"S.Planet.View = PlanetView.xyz;\n"
		"S.Sun.Places = PlanetSunPlaces;\n"
		"S.Sun.Angles = PlanetSunAngles;\n"
		"S.Sun.Light = PlanetSunLight;\n"
		"S.Sun.Tint = PlanetSunTint;\n"
		"S.Sun.Ambient = PlanetSunAmbient;\n"
		"S.Sun.Camera = PlanetSunCamera;\n"
		"S.Sun.SkyZenith = PlanetSkyDayZenith;\n"
		"S.Sun.SkyHorizon = PlanetSkyDayHorizon;\n"
		"S.SeaRegions.Frame = PlanetSeaFrame;\n"
		"S.SeaRegions.Ice = PlanetSeaIce;\n"
		"S.SeaRegions.Turquoise = PlanetSeaTurquoise;\n"
		"S.SeaRegions.Sunset = PlanetSeaSunset;\n"
		"return LBAPaletteShading(Parameters, SceneColor.rgb, BaseColor.rgb, Depth.r, Stencil.r, CustomDepth.r, ShadingModel.r, WorldNormal.rgb, Lut, Shades, SeaTexture, SeaRegionTexture, SeaRegions,\n"
		"    Dither, DitherSampler, DitherRamp, DitherRampSampler, HalftoneRamp, HalftoneRampSampler, S);"), DitherDotsPerSide);

	UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
	return LBAMaterials::EndRebuild(Context, Material);
}

UMaterial* FLBARenderingAssets::GetRainMaster()
{
	if (!RainMaster)
	{
		RainMaster = CreateRainMaster();
	}
	return RainMaster;
}

UMaterial* FLBARenderingAssets::CreateRainMaster()
{
	// Bump when the graph below changes: existing masters are rebuilt in place, user edits are otherwise kept.
	constexpr int32 Version = 7;

	bool bExisted = false;
	UMaterial* Material = Context.CreateOrReuseAsset<UMaterial>(GetFolder(), TEXT("M_LBA_Rain"), &bExisted);
	if (!Material || (bExisted && LBAMaterials::IsGeneratedVersion(Material, Version)))
	{
		return Material;
	}
	LBAMaterials::BeginRebuild(Material, Version);

	// After tonemapping (and TSR): thin rings stay crisp, in display colors.
	Material->MaterialDomain = MD_PostProcess;
	Material->BlendableLocation = BL_SceneColorAfterTonemapping;

	auto Add = [Material](UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	};
	auto SceneTexture = [&](ESceneTextureId Id, int32 Y)
	{
		UMaterialExpressionSceneTexture* Node = Cast<UMaterialExpressionSceneTexture>(Add(UMaterialExpressionSceneTexture::StaticClass(), -900, Y));
		Node->SceneTextureId = Id;
		return Node;
	};
	int32 ParameterY = 0;
	auto Scalar = [&](const TCHAR* Name, float Default, const TCHAR* Group)
	{
		UMaterialExpressionScalarParameter* Node = Cast<UMaterialExpressionScalarParameter>(Add(UMaterialExpressionScalarParameter::StaticClass(), -600, ParameterY));
		ParameterY += 80;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = Group;
		return Node;
	};
	auto Vector = [&](const TCHAR* Name, const FLinearColor& Default, const TCHAR* Group)
	{
		UMaterialExpressionVectorParameter* Node = Cast<UMaterialExpressionVectorParameter>(Add(UMaterialExpressionVectorParameter::StaticClass(), -600, ParameterY));
		ParameterY += 160;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = Group;
		return Node;
	};

	UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Add(UMaterialExpressionCustom::StaticClass(), -250, 0));
	Custom->Description = TEXT("LBA rain splashes");
	Custom->OutputType = CMOT_Float3;
	Custom->IncludeFilePaths = { TEXT("/Plugin/LBA2Importer/Private/LBARain.ush") };
	Custom->Inputs.Reset();

	struct FInput
	{
		const TCHAR* Name;
		UMaterialExpression* Expression;
	};
	// The colors are display (sRGB) colors; levels set their palette's.
	const FInput Inputs[] =
	{
		{ TEXT("SceneColor"), SceneTexture(PPI_PostProcessInput0, -200) },
		{ TEXT("Depth"), SceneTexture(PPI_SceneDepth, -80) },
		{ TEXT("RainAmount"), Scalar(TEXT("RainAmount"), 0.5f, TEXT("Rain")) },
		{ TEXT("Opacity"), Scalar(TEXT("Opacity"), 0.6f, TEXT("Rain")) },
		{ TEXT("SplashRate"), Scalar(TEXT("SplashRate"), 40.f, TEXT("Splashes")) },
		{ TEXT("SplashRadius"), Scalar(TEXT("SplashRadius"), 6.f, TEXT("Splashes")) },
		{ TEXT("SplashDuration"), Scalar(TEXT("SplashDuration"), 0.25f, TEXT("Splashes")) },
		{ TEXT("SplashRange"), Scalar(TEXT("SplashRange"), 2500.f, TEXT("Splashes")) },
		{ TEXT("SplashColor"), Vector(TEXT("SplashColor"), FLinearColor(0.53f, 0.53f, 0.47f), TEXT("Splashes")) },
		{ TEXT("DensityMap"), AddRainDensityMap(Material, -900, 40) },
		{ TEXT("DensityBounds"), Vector(TEXT("RainDensityBounds"), FLinearColor::Transparent, TEXT("Rain")) },
	};
	for (const FInput& Input : Inputs)
	{
		FCustomInput& CustomInput = Custom->Inputs.AddDefaulted_GetRef();
		CustomInput.InputName = Input.Name;
		CustomInput.Input.Connect(0, Input.Expression);
	}
	ConnectRgba(Custom, TEXT("DensityBounds"));

	Custom->Code = TEXT(
		"FLBARainSettings R;\n"
		"R.Amount = RainAmount;\n"
		"R.Opacity = Opacity;\n"
		"R.SplashRate = SplashRate;\n"
		"R.SplashRadius = max(SplashRadius, 0.5);\n"
		"R.SplashDuration = max(SplashDuration, 0.01);\n"
		"R.SplashRange = SplashRange;\n"
		"R.SplashColor = SplashColor.rgb;\n"
		"R.DensityBounds = DensityBounds;\n"
		"return LBARain(GetViewportUV(Parameters), SceneColor.rgb, Depth.r, DensityMap, DensityMapSampler, R);");

	UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
	return LBAMaterials::EndRebuild(Context, Material);
}

UMaterialParameterCollection* FLBARenderingAssets::GetWindParameters()
{
	if (WindParameters)
	{
		return WindParameters;
	}
	WindParameters = Context.CreateOrReuseAsset<UMaterialParameterCollection>(GetFolder(), TEXT("MPC_LBA_Wind"));
	if (!WindParameters)
	{
		return nullptr;
	}
	// Defaults all zero: no storm, no wind. Existing parameters keep their ids (materials refer to them by id).
	bool bChanged = false;
	for (const FName Name : FLBAStormWind::ShaderParameterNames)
	{
		if (!WindParameters->GetVectorParameterByName(Name))
		{
			FCollectionVectorParameter& Parameter = WindParameters->VectorParameters.AddDefaulted_GetRef();
			Parameter.ParameterName = Name;
			Parameter.DefaultValue = FLinearColor::Transparent;
			bChanged = true;
		}
	}
	if (bChanged)
	{
		WindParameters->PostEditChange();
		Context.MarkForSave(WindParameters);
	}
	return WindParameters;
}

UMaterialParameterCollection* FLBARenderingAssets::GetPlanetParameters()
{
	if (PlanetParameters)
	{
		return PlanetParameters;
	}
	PlanetParameters = Context.CreateOrReuseAsset<UMaterialParameterCollection>(GetFolder(), TEXT("MPC_LBA_Planet"));
	if (!PlanetParameters)
	{
		return nullptr;
	}
	// Defaults: no planet (LBAPlanet.ush). Existing parameters keep their ids (materials refer to them by id).
	const TPair<const TCHAR*, FLinearColor> Defaults[] =
	{
		{ TEXT("PlanetBend"), FLinearColor(200000.f, 0.f, 0.f, 0.f) },
		{ TEXT("PlanetView"), FLinearColor::Transparent },
		// Last frame, for motion vectors: the camera, and the sea disc's move since.
		{ TEXT("PlanetViewPrevious"), FLinearColor::Transparent },
		{ TEXT("PlanetSeaMove"), FLinearColor::Transparent },
		// The sky the sea reflects below the flat horizon (LBAPlanetSeaSkyBelow).
		{ TEXT("PlanetSkyBelow"), FLinearColor::Transparent },
		// The sun of each surface's place (LBAPlanetSunRelight; PlanetSunTint's alpha 0: off).
		{ TEXT("PlanetSunPlaces"), FLinearColor::Transparent },
		{ TEXT("PlanetSunAngles"), FLinearColor::Transparent },
		{ TEXT("PlanetSunLight"), FLinearColor::Transparent },
		{ TEXT("PlanetSunTint"), FLinearColor::Transparent },
		{ TEXT("PlanetSunAmbient"), FLinearColor::Transparent },
		{ TEXT("PlanetSunCamera"), FLinearColor::Transparent },
		// High up, the day's sky the palette shading shows (PlanetSkyDayZenith's alpha 0: off).
		{ TEXT("PlanetSkyDayZenith"), FLinearColor::Transparent },
		{ TEXT("PlanetSkyDayHorizon"), FLinearColor::Transparent },
		// The sea by place: ice toward the belt, turquoise around the desert, its own sunset (LBASeaRegionLevel,
		// LBAPlanetSeaSunset; PlanetSeaIce's and PlanetSeaTurquoise's z 0, PlanetSeaSunset's alpha 0: off).
		{ TEXT("PlanetSeaFrame"), FLinearColor::Transparent },
		{ TEXT("PlanetSeaIce"), FLinearColor::Transparent },
		{ TEXT("PlanetSeaTurquoise"), FLinearColor::Transparent },
		{ TEXT("PlanetSeaSunset"), FLinearColor::Transparent },
	};
	// Parameters dropped from the list go (only generated materials use the collection).
	bool bChanged = PlanetParameters->VectorParameters.RemoveAll([&Defaults](const FCollectionVectorParameter& Parameter)
	{
		for (const TPair<const TCHAR*, FLinearColor>& Default : Defaults)
		{
			if (Parameter.ParameterName == Default.Key)
			{
				return false;
			}
		}
		return true;
	}) > 0;
	for (const TPair<const TCHAR*, FLinearColor>& Default : Defaults)
	{
		if (!PlanetParameters->GetVectorParameterByName(Default.Key))
		{
			FCollectionVectorParameter& Parameter = PlanetParameters->VectorParameters.AddDefaulted_GetRef();
			Parameter.ParameterName = Default.Key;
			Parameter.DefaultValue = Default.Value;
			bChanged = true;
		}
	}
	if (bChanged)
	{
		PlanetParameters->PostEditChange();
		Context.MarkForSave(PlanetParameters);
		// Materials compiled against the old layout lose their planet (the palette shading turned off at sea): the
		// ones using it are rebuilt.
		bPlanetParametersChanged = true;
	}
	return PlanetParameters;
}

UMaterial* FLBARenderingAssets::GetPlanetSeaMaster()
{
	if (PlanetSeaMaster)
	{
		return PlanetSeaMaster;
	}
	// Bump when the graph below changes: existing masters are rebuilt in place, user edits are otherwise kept.
	// v6: a flat disc under the camera, bent like the rest of the world.
	constexpr int32 Version = 6;

	bool bExisted = false;
	UMaterial* Material = Context.CreateOrReuseAsset<UMaterial>(GetFolder(), TEXT("M_LBA_PlanetSea"), &bExisted);
	if (!Material || (bExisted && LBAMaterials::IsGeneratedVersion(Material, Version) && !bPlanetParametersChanged))
	{
		return PlanetSeaMaster = Material;
	}
	LBAMaterials::BeginRebuild(Material, Version);
	UMaterialParameterCollection* Collection = GetPlanetParameters();

	// Single layer water like the islands' Unreal ocean, so the palette shading draws it as cartoon water. The disc is
	// bent like the rest of the world by its world position offset; its normal (world space) is the bent sea's, with
	// ripples.
	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = BLEND_Opaque;
	Material->SetShadingModel(MSM_SingleLayerWater);
	Material->bTangentSpaceNormal = false;

	auto Add = [Material](UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	};
	auto Planet = [&](const TCHAR* Name, int32 Y) -> UMaterialExpression*
	{
		UMaterialExpressionCollectionParameter* Node = Cast<UMaterialExpressionCollectionParameter>(Add(UMaterialExpressionCollectionParameter::StaticClass(), -900, Y));
		Node->Collection = Collection;
		Node->ParameterName = Name;
		Node->ParameterId = Collection ? Collection->GetParameterId(Name) : FGuid();
		return Node;
	};
	int32 ParameterY = 600;
	auto Scalar = [&](const TCHAR* Name, float Default)
	{
		UMaterialExpressionScalarParameter* Node = Cast<UMaterialExpressionScalarParameter>(Add(UMaterialExpressionScalarParameter::StaticClass(), -600, ParameterY));
		ParameterY += 80;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = TEXT("Sea");
		return Node;
	};
	auto Vector = [&](const TCHAR* Name, const FLinearColor& Default)
	{
		UMaterialExpressionVectorParameter* Node = Cast<UMaterialExpressionVectorParameter>(Add(UMaterialExpressionVectorParameter::StaticClass(), -600, ParameterY));
		ParameterY += 160;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = TEXT("Sea");
		return Node;
	};
	auto MakeCustom = [&](const TCHAR* Description, int32 Y, std::initializer_list<TPair<const TCHAR*, UMaterialExpression*>> Inputs, const TCHAR* Code)
	{
		UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Add(UMaterialExpressionCustom::StaticClass(), -250, Y));
		Custom->Description = Description;
		Custom->OutputType = CMOT_Float3;
		Custom->IncludeFilePaths = { TEXT("/Plugin/LBA2Importer/Private/LBAPlanet.ush") };
		Custom->Inputs.Reset();
		for (const TPair<const TCHAR*, UMaterialExpression*>& Input : Inputs)
		{
			FCustomInput& CustomInput = Custom->Inputs.AddDefaulted_GetRef();
			CustomInput.InputName = Input.Key;
			CustomInput.Input.Connect(0, Input.Value);
		}
		Custom->Code = Code;
		return Custom;
	};

	UMaterialExpression* Bend = Planet(TEXT("PlanetBend"), -800);
	UMaterialExpression* View = Planet(TEXT("PlanetView"), -720);
	UMaterialExpression* ViewPrevious = Planet(TEXT("PlanetViewPrevious"), -1200);
	// A position relative to a camera, subtracted in double precision.
	auto FromCamera = [&](UMaterialExpression* Position, UMaterialExpression* Camera, int32 Y) -> UMaterialExpression*
	{
		UMaterialExpressionComponentMask* XYZ = Cast<UMaterialExpressionComponentMask>(Add(UMaterialExpressionComponentMask::StaticClass(), -750, Y + 80));
		XYZ->R = XYZ->G = XYZ->B = true;
		XYZ->A = false;
		XYZ->Input.Connect(0, Camera);
		UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Add(UMaterialExpressionSubtract::StaticClass(), -600, Y));
		Subtract->A.Connect(0, Position);
		Subtract->B.Connect(0, XYZ);
		return Subtract;
	};

	// The disc stays under the camera: its motion vectors (which TSR and motion blur rely on) follow the sea, not the
	// disc (LBAPlanetSeaPreviousOffset). In the last frame's branch, the world position is the vertex's last frame's.
	UMaterialExpression* Vertex = Add(UMaterialExpressionWorldPosition::StaticClass(), -900, -900);
	UMaterialExpressionCustom* Offset = MakeCustom(TEXT("LBA planet bend"), -600,
		{ { TEXT("FromView"), FromCamera(Vertex, View, -760) }, { TEXT("View"), View }, { TEXT("Bend"), Bend } },
		TEXT("return LBAPlanetBendOffset(FromView, View.xyz, Bend);"));
	UMaterialExpressionCustom* PreviousOffset = MakeCustom(TEXT("LBA planet sea bend (last frame)"), -1100,
		{ { TEXT("FromViewPrevious"), FromCamera(Vertex, ViewPrevious, -1160) }, { TEXT("ViewPrevious"), ViewPrevious },
		  { TEXT("SeaMove"), Planet(TEXT("PlanetSeaMove"), -1040) }, { TEXT("Bend"), Bend } },
		TEXT("return LBAPlanetSeaPreviousOffset(FromViewPrevious, ViewPrevious.xyz, SeaMove.xyz, Bend);"));
	UMaterialExpressionPreviousFrameSwitch* Switch = Cast<UMaterialExpressionPreviousFrameSwitch>(Add(UMaterialExpressionPreviousFrameSwitch::StaticClass(), -100, -800));
	Switch->CurrentFrame.Connect(0, Offset);
	Switch->PreviousFrame.Connect(0, PreviousOffset);
	UMaterialEditingLibrary::ConnectMaterialProperty(Switch, TEXT(""), MP_WorldPositionOffset);

	// The pixel's flat position (before the bend): the bent sea's up there, ripples laid out on the flat sea.
	UMaterialExpressionWorldPosition* Flat = Cast<UMaterialExpressionWorldPosition>(Add(UMaterialExpressionWorldPosition::StaticClass(), -900, 100));
	Flat->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	UMaterialExpressionCustom* Normal = MakeCustom(TEXT("LBA planet sea normal"), 0,
		{ { TEXT("FromView"), FromCamera(Flat, View, 120) }, { TEXT("Flat"), Flat }, { TEXT("Time"), Add(UMaterialExpressionTime::StaticClass(), -900, 260) },
		  { TEXT("Bend"), Bend }, { TEXT("RippleSlope"), Scalar(TEXT("RippleSlope"), 0.05f) } },
		TEXT("return LBAPlanetSeaNormal(FromView, Flat.xy, Time, Bend, RippleSlope);"));
	UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT(""), MP_Normal);
	UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(TEXT("Roughness"), 0.06f), TEXT(""), MP_Roughness);

	// Water coefficients (1/cm), set per planet from its home island's sea (LBAWater::ComputeLook).
	UMaterialExpressionSingleLayerWaterMaterialOutput* Water = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(
		Add(UMaterialExpressionSingleLayerWaterMaterialOutput::StaticClass(), 0, 600));
	Water->ScatteringCoefficients.Connect(0, Vector(TEXT("Scattering"), FLinearColor(0.0025f, 0.0025f, 0.0025f)));
	Water->AbsorptionCoefficients.Connect(0, Vector(TEXT("Absorption"), FLinearColor(0.02f, 0.004f, 0.002f)));
	Water->PhaseG.Connect(0, Scalar(TEXT("PhaseG"), 0.1f));
	Water->ColorScaleBehindWater.Connect(0, Scalar(TEXT("ColorScaleBehindWater"), 1.f));

	return PlanetSeaMaster = LBAMaterials::EndRebuild(Context, Material);
}

UMaterial* FLBARenderingAssets::GetRainDropMaster()
{
	if (!RainDropMaster)
	{
		RainDropMaster = CreateRainDropMaster();
	}
	return RainDropMaster;
}

UMaterial* FLBARenderingAssets::CreateRainDropMaster()
{
	// Bump when the graph below changes: existing masters are rebuilt in place, user edits are otherwise kept.
	constexpr int32 Version = 9;

	bool bExisted = false;
	UMaterial* Material = Context.CreateOrReuseAsset<UMaterial>(GetFolder(), TEXT("M_LBA_RainDrop"), &bExisted);
	if (!Material || (bExisted && LBAMaterials::IsGeneratedVersion(Material, Version)))
	{
		return Material;
	}
	LBAMaterials::BeginRebuild(Material, Version);

	// Unlit streaks composited after motion blur, so after the palette shading (BL_SceneColorAfterDOF), which would
	// read them as light on the surfaces behind. They fade in the fog themselves (Unreal's fog is the enhanced look's
	// only), and stay out of the velocity buffer (TSR would smear them).
	Material->MaterialDomain = MD_Surface;
	Material->BlendMode = BLEND_Translucent;
	Material->SetShadingModel(MSM_Unlit);
	Material->TranslucencyPass = MTP_AfterMotionBlur;
	Material->bUseTranslucencyVertexFog = false;
	Material->bOutputTranslucentVelocity = false;
	bool bNeedsRecompile = false;
	UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_NiagaraSprites, bNeedsRecompile);

	auto Add = [Material](UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	};
	auto Connect = [](UMaterialExpression* From, const TCHAR* Output, UMaterialExpression* To, const TCHAR* Input)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(From, Output, To, Input);
	};
	auto Subtract = [&](UMaterialExpression* A, UMaterialExpression* B, int32 Y)
	{
		UMaterialExpression* Node = Add(UMaterialExpressionSubtract::StaticClass(), -600, Y);
		Connect(A, TEXT(""), Node, TEXT("A"));
		Connect(B, TEXT(""), Node, TEXT("B"));
		return Node;
	};
	int32 ParameterY = 0;
	auto Scalar = [&](const TCHAR* Name, float Default)
	{
		UMaterialExpressionScalarParameter* Node = Cast<UMaterialExpressionScalarParameter>(Add(UMaterialExpressionScalarParameter::StaticClass(), -600, ParameterY));
		ParameterY += 80;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = TEXT("Rain");
		return Node;
	};
	auto Vector = [&](const TCHAR* Name, const FLinearColor& Default)
	{
		UMaterialExpressionVectorParameter* Node = Cast<UMaterialExpressionVectorParameter>(Add(UMaterialExpressionVectorParameter::StaticClass(), -600, ParameterY));
		ParameterY += 160;
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		Node->Group = TEXT("Rain");
		return Node;
	};
	auto MakeCustom = [&](const TCHAR* Description, ECustomMaterialOutputType Output, int32 Y, std::initializer_list<TPair<const TCHAR*, UMaterialExpression*>> Inputs, const TCHAR* Code)
	{
		UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Add(UMaterialExpressionCustom::StaticClass(), -250, Y));
		Custom->Description = Description;
		Custom->OutputType = Output;
		Custom->IncludeFilePaths = { TEXT("/Plugin/LBA2Importer/Private/LBARain.ush") };
		Custom->Inputs.Reset();
		for (const TPair<const TCHAR*, UMaterialExpression*>& Input : Inputs)
		{
			FCustomInput& CustomInput = Custom->Inputs.AddDefaulted_GetRef();
			CustomInput.InputName = Input.Key;
			CustomInput.Input.Connect(0, Input.Value);
		}
		Custom->Code = Code;
		return Custom;
	};

	UMaterialExpression* Camera = Add(UMaterialExpressionCameraPositionWS::StaticClass(), -900, -400);
	UMaterialExpression* Particle = Add(UMaterialExpressionParticlePositionWS::StaticClass(), -900, -320);
	UMaterialExpression* Position = Add(UMaterialExpressionWorldPosition::StaticClass(), -900, -240);
	UMaterialExpression* Direction = Add(UMaterialExpressionParticleDirection::StaticClass(), -900, -160);
	UMaterialExpression* Size = Add(UMaterialExpressionParticleSize::StaticClass(), -900, -80);
	UMaterialExpression* UV = Add(UMaterialExpressionTextureCoordinate::StaticClass(), -900, 0);
	UMaterialExpression* Random = Add(UMaterialExpressionParticleRandom::StaticClass(), -900, 80);
	UMaterialExpression* RelativeTime = Add(UMaterialExpressionParticleRelativeTime::StaticClass(), -900, 160);
	UMaterialExpression* MinPixels = Scalar(TEXT("MinPixels"), 1.5f);

	// How visible the drop is where it is: the level's rain density against the drop's own random threshold.
	UMaterialExpressionCustom* Visibility = MakeCustom(TEXT("LBA rain drop visibility"), CMOT_Float1, -700,
		{
			{ TEXT("Particle"), Particle },
			{ TEXT("Random"), Random },
			{ TEXT("DensityMap"), AddRainDensityMap(Material, -900, 240) },
			{ TEXT("DensityBounds"), Vector(TEXT("RainDensityBounds"), FLinearColor::Transparent) },
		},
		TEXT("return LBARainDropVisibility(LBARainDensity(DensityMap, DensityMapSampler, Particle.xy, DensityBounds), Random);"));
	ConnectRgba(Visibility, TEXT("DensityBounds"));

	// Vertex: the streak trailing the drop along its path, at least MinPixels wide.
	UMaterialExpressionCustom* Offset = MakeCustom(TEXT("LBA rain drop offset"), CMOT_Float3, -400,
		{
			{ TEXT("ToCamera"), Subtract(Camera, Particle, -400) },
			{ TEXT("VertexOffset"), Subtract(Position, Particle, -320) },
			{ TEXT("Direction"), Direction },
			{ TEXT("Size"), Size },
			{ TEXT("MinPixels"), MinPixels },
			{ TEXT("Visibility"), Visibility },
		},
		TEXT("return LBARainDropOffset(ToCamera, VertexOffset, Direction, Size, MinPixels, Visibility);"));
	UMaterialEditingLibrary::ConnectMaterialProperty(Offset, TEXT(""), MP_WorldPositionOffset);

	// Pixel: defaults follow the original's rain (RAIN.CPP) colors; levels set their palette's, linear.
	UMaterialExpressionCustom* Drop = MakeCustom(TEXT("LBA rain drop"), CMOT_Float4, 0,
		{
			{ TEXT("UV"), UV },
			{ TEXT("ToCamera"), Subtract(Camera, Position, -240) },
			{ TEXT("Size"), Size },
			{ TEXT("MinPixels"), MinPixels },
			{ TEXT("NearColor"), Vector(TEXT("NearColor"), FLinearColor(0.25f, 0.25f, 0.19f)) },
			{ TEXT("FarColor"), Vector(TEXT("FarColor"), FLinearColor(0.025f, 0.025f, 0.015f)) },
			{ TEXT("ColorDistance"), Scalar(TEXT("ColorDistance"), 6000.f) },
			{ TEXT("Opacity"), Scalar(TEXT("Opacity"), 1.f) },
			{ TEXT("TrailFade"), Scalar(TEXT("TrailFade"), 0.4f) },
			{ TEXT("FogDensity"), Scalar(TEXT("FogDensity"), 0.f) },
			{ TEXT("FogStart"), Scalar(TEXT("FogStart"), 0.f) },
			{ TEXT("Visibility"), Visibility },
			{ TEXT("RelativeTime"), RelativeTime },
			{ TEXT("LifeFade"), Scalar(TEXT("LifeFade"), 0.1f) },
		},
		TEXT("return LBARainDrop(UV, ToCamera, Size.x, MinPixels, NearColor.rgb, FarColor.rgb, ColorDistance, Opacity, TrailFade, FogDensity, FogStart, ")
		TEXT("Visibility, RelativeTime, LifeFade);"));
	UMaterialExpressionComponentMask* Rgb = Cast<UMaterialExpressionComponentMask>(Add(UMaterialExpressionComponentMask::StaticClass(), -100, 0));
	Rgb->R = Rgb->G = Rgb->B = true;
	Rgb->A = false;
	Connect(Drop, TEXT(""), Rgb, TEXT(""));
	UMaterialExpressionComponentMask* Alpha = Cast<UMaterialExpressionComponentMask>(Add(UMaterialExpressionComponentMask::StaticClass(), -100, 120));
	Alpha->R = Alpha->G = Alpha->B = false;
	Alpha->A = true;
	Connect(Drop, TEXT(""), Alpha, TEXT(""));
	UMaterialEditingLibrary::ConnectMaterialProperty(Rgb, TEXT(""), MP_EmissiveColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Alpha, TEXT(""), MP_Opacity);
	return LBAMaterials::EndRebuild(Context, Material);
}
