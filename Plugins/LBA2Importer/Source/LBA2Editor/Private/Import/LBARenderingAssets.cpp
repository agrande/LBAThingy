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

UMaterialInterface* FLBARenderingAssets::GetPaletteShading(const FLBAXpl& Xpl)
{
	if (UMaterialInterface** Found = Instances.Find(Xpl.Entry))
	{
		return *Found;
	}
	UMaterial* Parent = GetMaster();
	if (!Parent)
	{
		return nullptr;
	}

	const FString Name = FLBAXpl::GetName(Xpl.Entry);
	UTexture2D* Shades = CreateShadesTexture(Xpl, TEXT("T_LBA_Shades_") + Name);
	UVolumeTexture* Lut = CreatePaletteLut(Xpl, TEXT("VT_LBA_PaletteLut_") + Name);

	UMaterialInstanceConstant* Instance = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(GetFolder(), TEXT("MI_LBA_PaletteShading_") + Name);
	if (!Instance)
	{
		return Parent;
	}
	UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(Instance);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Shades"), Shades);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("PaletteLut"), Lut);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("NeutralRow"), float(Xpl.ShadeNormalLevel));
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Context.MarkForSave(Instance);
	Instances.Add(Xpl.Entry, Instance);
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
	constexpr int32 Version = 7;

	CreateDitherTextures();

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
	auto Scalar = [&](const TCHAR* Name, float Default, int32 Y, const TCHAR* Group)
	{
		UMaterialExpressionScalarParameter* Node = Cast<UMaterialExpressionScalarParameter>(Add(UMaterialExpressionScalarParameter::StaticClass(), -600, Y));
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
		int32 OutputIndex;
	};
	UTexture2D* DefaultShades = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	UMaterialExpressionVectorParameter* FogColor = Cast<UMaterialExpressionVectorParameter>(Add(UMaterialExpressionVectorParameter::StaticClass(), -600, 960));
	FogColor->ParameterName = TEXT("FogColor");
	FogColor->DefaultValue = FLinearColor(0.45f, 0.6f, 0.8f);
	FogColor->Group = TEXT("Fog");
	const FInput Inputs[] =
	{
		{ TEXT("SceneColor"), SceneTexture(PPI_PostProcessInput0, 0), 0 },
		{ TEXT("BaseColor"), SceneTexture(PPI_BaseColor, 120), 0 },
		{ TEXT("Depth"), SceneTexture(PPI_SceneDepth, 240), 0 },
		{ TEXT("Stencil"), SceneTexture(PPI_CustomStencil, 360), 0 },
		{ TEXT("Lut"), TextureObject(TEXT("PaletteLut"), DitherTexture, 480), 0 },
		{ TEXT("Shades"), TextureObject(TEXT("Shades"), DefaultShades, 600), 0 },
		{ TEXT("Dither"), TextureObject(TEXT("Dither3D"), DitherTexture, 720), 0 },
		{ TEXT("DitherRamp"), TextureObject(TEXT("DitherRamp"), DitherRamp, 840), 0 },
		{ TEXT("Strength"), Scalar(TEXT("Strength"), 1.f, 0, TEXT("Palette")), 0 },
		{ TEXT("NeutralRow"), Scalar(TEXT("NeutralRow"), 12.f, -320, TEXT("Palette")), 0 },
		{ TEXT("NeutralLight"), Scalar(TEXT("NeutralLight"), 1.f, -240, TEXT("Palette")), 0 },
		{ TEXT("ShadeContrast"), Scalar(TEXT("ShadeContrast"), 1.f, -160, TEXT("Palette")), 0 },
		{ TEXT("KeepLuminance"), Scalar(TEXT("KeepLuminance"), 0.f, -80, TEXT("Palette")), 0 },
		{ TEXT("LightTint"), Scalar(TEXT("LightTint"), 0.3f, 80, TEXT("Palette")), 0 },
		{ TEXT("SkyDepth"), Scalar(TEXT("SkyDepth"), 1.0e7f, 160, TEXT("Palette")), 0 },
		{ TEXT("DebugView"), Scalar(TEXT("DebugView"), 0.f, 720, TEXT("Debug")), 0 },
		{ TEXT("FogColor"), FogColor, 0 },
		{ TEXT("FogStart"), Scalar(TEXT("FogStart"), 0.f, 1040, TEXT("Fog")), 0 },
		{ TEXT("FogEnd"), Scalar(TEXT("FogEnd"), 0.f, 1120, TEXT("Fog")), 0 },
		{ TEXT("DitherAmount"), Scalar(TEXT("DitherAmount"), 1.f, 240, TEXT("Dither")), 0 },
		{ TEXT("DotSpacing"), Scalar(TEXT("DotSpacing"), 2.f, 320, TEXT("Dither")), 0 },
		{ TEXT("DotContrast"), Scalar(TEXT("DotContrast"), 1.5f, 400, TEXT("Dither")), 0 },
		{ TEXT("DotSizeVariability"), Scalar(TEXT("DotSizeVariability"), 0.f, 480, TEXT("Dither")), 0 },
		{ TEXT("StretchSmoothness"), Scalar(TEXT("StretchSmoothness"), 1.f, 560, TEXT("Dither")), 0 },
		{ TEXT("ExcludedStencil"), Scalar(TEXT("ExcludedStencil"), float(LBA::MovingObjectStencil), 640, TEXT("Dither")), 0 },
	};
	for (const FInput& Input : Inputs)
	{
		FCustomInput& CustomInput = Custom->Inputs.AddDefaulted_GetRef();
		CustomInput.InputName = Input.Name;
		CustomInput.Input.Connect(Input.OutputIndex, Input.Expression);
	}

	Custom->Code = FString::Printf(TEXT(
		"FLBAPaletteShadingSettings S;\n"
		"S.Strength = Strength;\n"
		"S.NeutralRow = NeutralRow;\n"
		"S.NeutralLight = max(NeutralLight, 0.01);\n"
		"S.ShadeContrast = ShadeContrast;\n"
		"S.KeepLuminance = KeepLuminance;\n"
		"S.DitherAmount = DitherAmount;\n"
		"S.LightTint = LightTint;\n"
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
		"return LBAPaletteShading(Parameters, SceneColor.rgb, BaseColor.rgb, Depth.r, Stencil.r, Lut, Shades,\n"
		"    Dither, DitherSampler, DitherRamp, DitherRampSampler, S);"), DitherDotsPerSide);

	UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
	return LBAMaterials::EndRebuild(Context, Material);
}
