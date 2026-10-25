#include "Import/LBAMaterialLibrary.h"

#include "Engine/Texture2D.h"
#include "Import/LBAImportContext.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"

namespace
{
	const TCHAR* MasterNames[] =
	{
		TEXT("M_LBA_Color"),
		TEXT("M_LBA_Translucent"),
		TEXT("M_LBA_Textured"),
		TEXT("M_LBA_TexturedMasked"),
		TEXT("M_LBA_Ground"),
		TEXT("M_LBA_Brick"),
		TEXT("M_LBA_Water"),
		TEXT("M_LBA_Invisible"),
		TEXT("M_LBA_TexturedTranslucent"),
		TEXT("M_LBA_Glow"),
		TEXT("M_LBA_TexturedGlow"),
	};
	static_assert(UE_ARRAY_COUNT(MasterNames) == uint8(ELBAMaterial::Count), "Keep MasterNames in sync");

	/** Small helper to lay out and wire material graphs without repeating the editing library calls. */
	struct FGraph
	{
		UMaterial* Material;
		int32 Column = -600;

		template <typename T>
		T* Add(int32 Y)
		{
			return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), Column, Y));
		}

		UMaterialExpressionScalarParameter* Scalar(const TCHAR* Name, float Default, int32 Y)
		{
			UMaterialExpressionScalarParameter* Param = Add<UMaterialExpressionScalarParameter>(Y);
			Param->ParameterName = Name;
			Param->DefaultValue = Default;
			return Param;
		}

		UMaterialExpressionConstant* Constant(float Value, int32 Y)
		{
			UMaterialExpressionConstant* Const = Add<UMaterialExpressionConstant>(Y);
			Const->R = Value;
			return Const;
		}

		UMaterialExpressionTextureSampleParameter2D* Texture(int32 Y)
		{
			UMaterialExpressionTextureSampleParameter2D* Tex = Add<UMaterialExpressionTextureSampleParameter2D>(Y);
			Tex->ParameterName = TEXT("Texture");
			Tex->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
			Tex->SamplerType = SAMPLERTYPE_Color;
			return Tex;
		}

		void Connect(UMaterialExpression* From, const TCHAR* Output, UMaterialExpression* To, const TCHAR* Input)
		{
			UMaterialEditingLibrary::ConnectMaterialExpressions(From, Output, To, Input);
		}

		void Out(UMaterialExpression* From, const TCHAR* Output, EMaterialProperty Property)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(From, Output, Property);
		}

		UMaterialExpression* Multiply(UMaterialExpression* A, const TCHAR* AOut, UMaterialExpression* B, const TCHAR* BOut, int32 Y)
		{
			UMaterialExpressionMultiply* Mul = Add<UMaterialExpressionMultiply>(Y);
			Connect(A, AOut, Mul, TEXT("A"));
			Connect(B, BOut, Mul, TEXT("B"));
			return Mul;
		}

		/**
		 * Shade = lerp(1, VertexAlpha * 2, BakedShading). Off by default: Unreal's lighting replaces the original's
		 * baked intensities, and the palette shading post-process needs the exact palette color as base color.
		 */
		UMaterialExpression* BakedShade(UMaterialExpressionVertexColor* VertexColor, int32 Y)
		{
			UMaterialExpressionConstant* One = Constant(1.f, Y);
			UMaterialExpressionConstant* Two = Constant(2.f, Y + 60);
			UMaterialExpression* Doubled = Multiply(VertexColor, TEXT("A"), Two, TEXT(""), Y + 60);
			UMaterialExpressionScalarParameter* Weight = Scalar(TEXT("BakedShading"), 0.f, Y + 120);
			UMaterialExpressionLinearInterpolate* Lerp = Add<UMaterialExpressionLinearInterpolate>(Y);
			Connect(One, TEXT(""), Lerp, TEXT("A"));
			Connect(Doubled, TEXT(""), Lerp, TEXT("B"));
			Connect(Weight, TEXT(""), Lerp, TEXT("Alpha"));
			return Lerp;
		}

		/**
		 * Vertex color RGB as a linear color. Mesh builds store vertex colors sRGB-encoded (ToFColor(true)) and
		 * shaders read the bytes as they are, so the palette color has to be decoded here.
		 */
		UMaterialExpression* VertexAlbedo(UMaterialExpressionVertexColor* VertexColor, int32 Y)
		{
			UMaterialExpressionCustom* Decode = Add<UMaterialExpressionCustom>(Y);
			Decode->Description = TEXT("sRGB to linear");
			Decode->OutputType = CMOT_Float3;
			Decode->Inputs.Reset();
			FCustomInput& Input = Decode->Inputs.AddDefaulted_GetRef();
			Input.InputName = TEXT("C");
			Input.Input.Connect(0, VertexColor);
			Decode->Code = TEXT("const float3 S = saturate(C.rgb);\nreturn select(S <= 0.04045, S / 12.92, pow((S + 0.055) / 1.055, 2.4));");
			return Decode;
		}

		void Surface(float Roughness, float Specular)
		{
			Out(Scalar(TEXT("Roughness"), Roughness, 600), TEXT(""), MP_Roughness);
			Out(Scalar(TEXT("Specular"), Specular, 680), TEXT(""), MP_Specular);
		}
	};
}

namespace
{
	FString MakeVersionStamp(int32 Version)
	{
		return FString::Printf(TEXT("Generated by the LBA2 importer (v%d). Edits are kept until a newer importer version rebuilds it."), Version);
	}
}

bool LBAMaterials::IsGeneratedVersion(const UMaterial* Material, int32 Version)
{
	const FString Stamp = MakeVersionStamp(Version);
	for (const UMaterialExpressionComment* Comment : Material->GetEditorComments())
	{
		if (Comment && Comment->Text == Stamp)
		{
			return true;
		}
	}
	return false;
}

void LBAMaterials::BeginRebuild(UMaterial* Material, int32 Version)
{
	UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
	// Some outputs outlive that (single layer water's: a second one fails the material's compile).
	for (UMaterialExpression* Expression : TArray<TObjectPtr<UMaterialExpression>>(Material->GetExpressions()))
	{
		UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
	}
	for (UMaterialExpressionComment* Comment : TArray<TObjectPtr<UMaterialExpressionComment>>(Material->GetEditorComments()))
	{
		Material->GetExpressionCollection().RemoveComment(Comment);
	}
	Material->BlendMode = BLEND_Opaque;
	Material->MaterialDomain = MD_Surface;
	AddVersionStamp(Material, Version);
}

void LBAMaterials::AddVersionStamp(UMaterial* Material, int32 Version)
{
	UMaterialExpressionComment* Stamp = NewObject<UMaterialExpressionComment>(Material);
	Stamp->Text = MakeVersionStamp(Version);
	Stamp->MaterialExpressionEditorX = -900;
	Stamp->MaterialExpressionEditorY = -200;
	Stamp->SizeX = 600;
	Stamp->SizeY = 80;
	Material->GetExpressionCollection().AddComment(Stamp);
}

UMaterial* LBAMaterials::EndRebuild(FLBAImportContext& Context, UMaterial* Material)
{
	if (Material->MaterialDomain == MD_Surface)
	{
		bool bNeedsRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bNeedsRecompile);
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_InstancedStaticMeshes, bNeedsRecompile);
	}
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Context.MarkForSave(Material);
	return Material;
}

FLBAMaterialLibrary::FLBAMaterialLibrary(FLBAImportContext& InContext)
	: Context(InContext)
{
}

FName FLBAMaterialLibrary::GetSlotName(ELBAMaterial Kind)
{
	switch (Kind)
	{
	case ELBAMaterial::Color: return TEXT("Color");
	case ELBAMaterial::Translucent: return TEXT("Translucent");
	case ELBAMaterial::Ground: return TEXT("Ground");
	case ELBAMaterial::Water: return TEXT("Water");
	case ELBAMaterial::Brick: return TEXT("Bricks");
	case ELBAMaterial::Glow: return TEXT("Glow");
	default: return TEXT("Textured");
	}
}

UMaterial* FLBAMaterialLibrary::GetMaster(ELBAMaterial Kind)
{
	UMaterial*& Master = Masters[uint8(Kind)];
	if (!Master)
	{
		Master = CreateMaster(Kind);
	}
	return Master;
}

UMaterial* FLBAMaterialLibrary::CreateMaster(ELBAMaterial Kind)
{
	const FString Folder = Context.GetContentPath(TEXT("Materials"));
	const FString Name = MasterNames[uint8(Kind)];

	// Bump when a graph below changes. v2: BakedShading off by default, opaque textured sea, sky. v3: sky brightness.
	// v4: vertex colors decoded from sRGB. v5: duller sea. v6: cloud clearance.
	// v7: no specular on palette surfaces (the original has none, and it would read as light in the palette shading).
	// v8: unshaded sea, like the original's. v9: textured glow (lit windows).
	constexpr int32 Version = 9;

	bool bExisted = false;
	UMaterial* Material = Context.CreateOrReuseAsset<UMaterial>(Folder, Name, &bExisted);
	if (!Material || (bExisted && LBAMaterials::IsGeneratedVersion(Material, Version)))
	{
		// Keep user edits to existing masters.
		return Material;
	}
	LBAMaterials::BeginRebuild(Material, Version);

	FGraph G{ Material };

	switch (Kind)
	{
	case ELBAMaterial::Color:
	{
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(0);
		G.Column = -300;
		UMaterialExpression* Albedo = G.VertexAlbedo(VC, 0);
		UMaterialExpression* Shade = G.BakedShade(VC, 200);
		G.Column = -150;
		G.Out(G.Multiply(Albedo, TEXT(""), Shade, TEXT(""), 0), TEXT(""), MP_BaseColor);
		G.Surface(1.f, 0.f);
		break;
	}
	case ELBAMaterial::Glow:
	{
		// The glow is light beyond the brightest shade row for the palette shading, which brightens that shade.
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(0);
		G.Column = -300;
		UMaterialExpression* Albedo = G.VertexAlbedo(VC, 0);
		G.Column = -150;
		G.Out(Albedo, TEXT(""), MP_BaseColor);
		G.Out(G.Multiply(Albedo, TEXT(""), G.Scalar(TEXT("GlowStrength"), 3.f, 200), TEXT(""), 200), TEXT(""), MP_EmissiveColor);
		G.Surface(1.f, 0.f);
		break;
	}
	case ELBAMaterial::Translucent:
	{
		Material->BlendMode = BLEND_Translucent;
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(0);
		G.Column = -300;
		G.Out(G.VertexAlbedo(VC, 0), TEXT(""), MP_BaseColor);
		G.Out(G.Scalar(TEXT("Opacity"), 0.45f, 200), TEXT(""), MP_Opacity);
		G.Surface(0.15f, 0.6f);
		break;
	}
	case ELBAMaterial::TexturedGlow:
	{
		// Like M_LBA_Glow, on the texels the texture's alpha marks, a little dimmer: lit windows keep their panes.
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(260);
		G.Column = -450;
		UMaterialExpression* Albedo = G.VertexAlbedo(VC, 160);
		G.Column = -300;
		UMaterialExpression* Tinted = G.Multiply(Tex, TEXT("RGB"), Albedo, TEXT(""), 0);
		UMaterialExpression* Mask = G.Multiply(Tex, TEXT("A"), G.Scalar(TEXT("GlowStrength"), 1.5f, 400), TEXT(""), 400);
		G.Column = -150;
		G.Out(Tinted, TEXT(""), MP_BaseColor);
		G.Out(G.Multiply(Tinted, TEXT(""), Mask, TEXT(""), 200), TEXT(""), MP_EmissiveColor);
		G.Surface(1.f, 0.f);
		break;
	}
	case ELBAMaterial::Textured:
	case ELBAMaterial::TexturedMasked:
	{
		const bool bMasked = Kind == ELBAMaterial::TexturedMasked;
		Material->BlendMode = bMasked ? BLEND_Masked : BLEND_Opaque;
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(260);
		G.Column = -450;
		UMaterialExpression* Albedo = G.VertexAlbedo(VC, 160);
		G.Column = -300;
		UMaterialExpression* Tinted = G.Multiply(Tex, TEXT("RGB"), Albedo, TEXT(""), 0);
		UMaterialExpression* Shade = G.BakedShade(VC, 300);
		G.Column = -150;
		G.Out(G.Multiply(Tinted, TEXT(""), Shade, TEXT(""), 0), TEXT(""), MP_BaseColor);
		if (bMasked)
		{
			G.Out(Tex, TEXT("A"), MP_OpacityMask);
		}
		G.Surface(1.f, 0.f);
		break;
	}
	case ELBAMaterial::Ground:
	{
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(260);
		G.Column = -450;
		UMaterialExpression* Albedo = G.VertexAlbedo(VC, 160);
		G.Column = -300;
		UMaterialExpressionLinearInterpolate* Mix = G.Add<UMaterialExpressionLinearInterpolate>(0);
		G.Connect(Albedo, TEXT(""), Mix, TEXT("A"));
		G.Connect(Tex, TEXT("RGB"), Mix, TEXT("B"));
		G.Connect(Tex, TEXT("A"), Mix, TEXT("Alpha"));
		UMaterialExpression* Shade = G.BakedShade(VC, 300);
		G.Column = -150;
		G.Out(G.Multiply(Mix, TEXT(""), Shade, TEXT(""), 0), TEXT(""), MP_BaseColor);
		G.Surface(1.f, 0.f);
		break;
	}
	case ELBAMaterial::Brick:
	{
		// Brick sprites already contain hand-painted lighting; a little emissive keeps them readable.
		Material->BlendMode = BLEND_Masked;
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		G.Column = -300;
		G.Out(Tex, TEXT("RGB"), MP_BaseColor);
		G.Out(G.Multiply(Tex, TEXT("RGB"), G.Scalar(TEXT("SpriteEmissive"), 0.2f, 300), TEXT(""), 260), TEXT(""), MP_EmissiveColor);
		G.Out(Tex, TEXT("A"), MP_OpacityMask);
		G.Surface(1.f, 0.f);
		break;
	}
	case ELBAMaterial::Water:
	{
		// The original sea is an opaque, unshaded texture page tiled every 8192 units (1024 cm at the default scale),
		// slowly drifting here to stand in for its animated wave strips.
		Material->SetShadingModel(MSM_Unlit);
		G.Column = -1200;
		UMaterialExpressionWorldPosition* WorldPosition = G.Add<UMaterialExpressionWorldPosition>(0);
		UMaterialExpressionComponentMask* XY = G.Add<UMaterialExpressionComponentMask>(100);
		XY->R = true;
		XY->G = true;
		XY->B = false;
		XY->A = false;
		G.Connect(WorldPosition, TEXT(""), XY, TEXT(""));
		UMaterialExpressionDivide* Tiled = G.Add<UMaterialExpressionDivide>(0);
		G.Connect(XY, TEXT(""), Tiled, TEXT("A"));
		G.Connect(G.Scalar(TEXT("TileSize"), 1024.f, 200), TEXT(""), Tiled, TEXT("B"));
		G.Column = -900;
		UMaterialExpressionTime* Time = G.Add<UMaterialExpressionTime>(300);
		UMaterialExpressionVectorParameter* Drift = G.Add<UMaterialExpressionVectorParameter>(400);
		Drift->ParameterName = TEXT("Drift");
		Drift->DefaultValue = FLinearColor(0.01f, 0.004f, 0.f, 0.f);
		UMaterialExpressionComponentMask* DriftXY = G.Add<UMaterialExpressionComponentMask>(400);
		DriftXY->R = true;
		DriftXY->G = true;
		DriftXY->B = false;
		DriftXY->A = false;
		G.Connect(Drift, TEXT(""), DriftXY, TEXT(""));
		G.Column = -750;
		UMaterialExpressionAdd* UV = G.Add<UMaterialExpressionAdd>(0);
		G.Connect(Tiled, TEXT(""), UV, TEXT("A"));
		G.Connect(G.Multiply(Time, TEXT(""), DriftXY, TEXT(""), 300), TEXT(""), UV, TEXT("B"));
		G.Column = -500;
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		G.Connect(UV, TEXT(""), Tex, TEXT("UVs"));
		UMaterialExpressionVectorParameter* Tint = G.Add<UMaterialExpressionVectorParameter>(260);
		Tint->ParameterName = TEXT("WaterColor");
		Tint->DefaultValue = FLinearColor::White;
		G.Column = -250;
		G.Out(G.Multiply(Tex, TEXT("RGB"), Tint, TEXT(""), 0), TEXT(""), MP_EmissiveColor);
		break;
	}
	case ELBAMaterial::TexturedTranslucent:
	{
		Material->BlendMode = BLEND_Translucent;
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(260);
		G.Column = -450;
		UMaterialExpression* Albedo = G.VertexAlbedo(VC, 160);
		G.Column = -300;
		G.Out(G.Multiply(Tex, TEXT("RGB"), Albedo, TEXT(""), 0), TEXT(""), MP_BaseColor);
		G.Out(G.Multiply(Tex, TEXT("A"), G.Scalar(TEXT("Opacity"), 0.6f, 300), TEXT(""), 200), TEXT(""), MP_Opacity);
		G.Surface(0.2f, 0.5f);
		break;
	}
	case ELBAMaterial::Invisible:
	{
		Material->BlendMode = BLEND_Masked;
		G.Out(G.Constant(0.f, 0), TEXT(""), MP_OpacityMask);
		break;
	}
	default:
		break;
	}

	return LBAMaterials::EndRebuild(Context, Material);
}

UMaterialInterface* FLBAMaterialLibrary::GetTextured(ELBAMaterial Kind, UTexture2D* Texture, const FString& Folder)
{
	if (!Texture)
	{
		return GetMaster(ELBAMaterial::Color);
	}
	const TPair<uint8, UTexture2D*> Key(uint8(Kind), Texture);
	if (UMaterialInterface** Found = Instances.Find(Key))
	{
		return *Found;
	}

	FString BaseName = Texture->GetName();
	BaseName.RemoveFromStart(TEXT("T_"));
	// Glow textures of islands already end with _Glow; remake textures are shared with their unlit instances.
	const FString Suffix = Kind == ELBAMaterial::TexturedMasked ? TEXT("_Masked")
		: Kind == ELBAMaterial::TexturedTranslucent ? TEXT("_Translucent")
		: Kind == ELBAMaterial::TexturedGlow && !BaseName.EndsWith(TEXT("_Glow")) ? TEXT("_Glow") : FString();
	const FString Name = FString::Printf(TEXT("MI_%s%s"), *BaseName, *Suffix);

	UMaterialInstanceConstant* Instance = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(Folder, Name);
	if (!Instance)
	{
		return GetMaster(Kind);
	}
	UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, GetMaster(Kind));
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Texture"), Texture);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Context.MarkForSave(Instance);
	Instances.Add(Key, Instance);
	return Instance;
}
