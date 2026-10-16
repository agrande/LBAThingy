#include "Import/LBARenderingAssets.h"

#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "Formats/LBAXpl.h"
#include "Import/LBADither3DTextures.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "LBATypes.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
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
	constexpr int32 Version = 3;

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
		Scalar(TEXT("LightContrast"), 1.8f);
		Scalar(TEXT("RowStep"), 2.f);
		Scalar(TEXT("LightTint"), 0.6f);
		Scalar(TEXT("Exposure"), 0.f);
		Scalar(TEXT("Contrast"), 1.15f);
		Scalar(TEXT("Saturation"), 1.2f);
		Vector(TEXT("ShadowTint"), FLinearColor(0.55f, 0.65f, 1.f));
		Scalar(TEXT("ShadowTintAmount"), 0.25f);
		Vector(TEXT("HighlightTint"), FLinearColor(1.f, 0.85f, 0.6f));
		Scalar(TEXT("HighlightTintAmount"), 0.3f);
		Scalar(TEXT("DotPattern"), 0.f);
		Scalar(TEXT("DitherAmount"), 0.7f);
		Scalar(TEXT("DotSpacing"), 4.f);
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
	// 256 x 17: the 16 shade rows of fog level 0, then the palette itself. sRGB bytes in a linear texture so the
	// shader reads exact palette values.
	constexpr int32 Height = FLBAXpl::NumShadeRows + 1;
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(256 * Height);
	for (int32 Row = 0; Row < Height; ++Row)
	{
		for (int32 Index = 0; Index < 256; ++Index)
		{
			const int32 Shaded = Row < FLBAXpl::NumShadeRows ? Xpl.GetShade(Index, Row) : Index;
			Pixels[Row * 256 + Index] = Xpl.Palette[Shaded];
		}
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
	constexpr int32 Version = 10;

	CreateDitherTextures();
	CreateHalftoneRamp();

	bool bExisted = false;
	UMaterial* Material = Context.CreateOrReuseAsset<UMaterial>(GetFolder(), TEXT("M_LBA_PaletteShading"), &bExisted);
	if (!Material || (bExisted && LBAMaterials::IsGeneratedVersion(Material, Version)))
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
	// Defaults are the original look; MI_LBA_Look_Enhanced overrides them (FLBARenderingAssets::GetLook).
	UTexture2D* DefaultShades = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	const FInput Inputs[] =
	{
		{ TEXT("SceneColor"), SceneTexture(PPI_PostProcessInput0, 0) },
		{ TEXT("BaseColor"), SceneTexture(PPI_BaseColor, 120) },
		{ TEXT("Depth"), SceneTexture(PPI_SceneDepth, 240) },
		{ TEXT("Stencil"), SceneTexture(PPI_CustomStencil, 360) },
		{ TEXT("Lut"), TextureObject(TEXT("PaletteLut"), DitherTexture, 480) },
		{ TEXT("Shades"), TextureObject(TEXT("Shades"), DefaultShades, 600) },
		{ TEXT("Dither"), TextureObject(TEXT("Dither3D"), DitherTexture, 720) },
		{ TEXT("DitherRamp"), TextureObject(TEXT("DitherRamp"), DitherRamp, 840) },
		{ TEXT("HalftoneRamp"), TextureObject(TEXT("HalftoneRamp"), HalftoneRamp, 960) },
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
		{ TEXT("FogColor"), Vector(TEXT("FogColor"), FLinearColor(0.45f, 0.6f, 0.8f), TEXT("Fog")) },
		{ TEXT("FogStart"), Scalar(TEXT("FogStart"), 0.f, TEXT("Fog")) },
		{ TEXT("FogEnd"), Scalar(TEXT("FogEnd"), 0.f, TEXT("Fog")) },
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
		"S.SkyDepth = SkyDepth;\n"
		"S.DebugView = DebugView;\n"
		"S.FogColor = FogColor.rgb;\n"
		"S.FogStart = FogStart;\n"
		"S.FogEnd = FogEnd;\n"
		"S.Dither.DotsPerSide = %d;\n"
		"S.Dither.ScaleExp = max(DotSpacing, 1.0) * 8.0;\n"
		"S.Dither.SizeVariability = DotSizeVariability;\n"
		"S.Dither.Contrast = DotContrast;\n"
		"S.Dither.StretchSmoothness = StretchSmoothness;\n"
		"S.Halftone.DotPixels = max(DotSpacing, 1.0);\n"
		"S.Halftone.Contrast = DotContrast;\n"
		"return LBAPaletteShading(Parameters, SceneColor.rgb, BaseColor.rgb, Depth.r, Stencil.r, Lut, Shades,\n"
		"    Dither, DitherSampler, DitherRamp, DitherRampSampler, HalftoneRamp, HalftoneRampSampler, S);"), DitherDotsPerSide);

	UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
	return LBAMaterials::EndRebuild(Context, Material);
}
