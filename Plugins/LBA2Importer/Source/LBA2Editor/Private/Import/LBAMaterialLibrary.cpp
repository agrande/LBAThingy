#include "Import/LBAMaterialLibrary.h"

#include "Engine/Texture2D.h"
#include "Import/LBAImportContext.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
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

		/** Shade = lerp(1, VertexAlpha * 2, BakedShading). */
		UMaterialExpression* BakedShade(UMaterialExpressionVertexColor* VertexColor, int32 Y)
		{
			UMaterialExpressionConstant* One = Constant(1.f, Y);
			UMaterialExpressionConstant* Two = Constant(2.f, Y + 60);
			UMaterialExpression* Doubled = Multiply(VertexColor, TEXT("A"), Two, TEXT(""), Y + 60);
			UMaterialExpressionScalarParameter* Weight = Scalar(TEXT("BakedShading"), 0.6f, Y + 120);
			UMaterialExpressionLinearInterpolate* Lerp = Add<UMaterialExpressionLinearInterpolate>(Y);
			Connect(One, TEXT(""), Lerp, TEXT("A"));
			Connect(Doubled, TEXT(""), Lerp, TEXT("B"));
			Connect(Weight, TEXT(""), Lerp, TEXT("Alpha"));
			return Lerp;
		}

		void Surface(float Roughness, float Specular)
		{
			Out(Scalar(TEXT("Roughness"), Roughness, 600), TEXT(""), MP_Roughness);
			Out(Scalar(TEXT("Specular"), Specular, 680), TEXT(""), MP_Specular);
		}
	};
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

	bool bExisted = false;
	UMaterial* Material = Context.CreateOrReuseAsset<UMaterial>(Folder, Name, &bExisted);
	if (!Material || bExisted)
	{
		// Keep user edits to existing masters.
		return Material;
	}

	FGraph G{ Material };
	Material->BlendMode = BLEND_Opaque;

	switch (Kind)
	{
	case ELBAMaterial::Color:
	{
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(0);
		G.Column = -300;
		UMaterialExpression* Shade = G.BakedShade(VC, 200);
		G.Out(G.Multiply(VC, TEXT(""), Shade, TEXT(""), 0), TEXT(""), MP_BaseColor);
		G.Surface(0.8f, 0.3f);
		break;
	}
	case ELBAMaterial::Translucent:
	{
		Material->BlendMode = BLEND_Translucent;
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(0);
		G.Out(VC, TEXT(""), MP_BaseColor);
		G.Out(G.Scalar(TEXT("Opacity"), 0.45f, 200), TEXT(""), MP_Opacity);
		G.Surface(0.15f, 0.6f);
		break;
	}
	case ELBAMaterial::Textured:
	case ELBAMaterial::TexturedMasked:
	{
		const bool bMasked = Kind == ELBAMaterial::TexturedMasked;
		Material->BlendMode = bMasked ? BLEND_Masked : BLEND_Opaque;
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(260);
		G.Column = -300;
		UMaterialExpression* Tinted = G.Multiply(Tex, TEXT("RGB"), VC, TEXT(""), 0);
		UMaterialExpression* Shade = G.BakedShade(VC, 300);
		G.Column = -150;
		G.Out(G.Multiply(Tinted, TEXT(""), Shade, TEXT(""), 0), TEXT(""), MP_BaseColor);
		if (bMasked)
		{
			G.Out(Tex, TEXT("A"), MP_OpacityMask);
		}
		G.Surface(0.8f, 0.3f);
		break;
	}
	case ELBAMaterial::Ground:
	{
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(260);
		G.Column = -300;
		UMaterialExpressionLinearInterpolate* Mix = G.Add<UMaterialExpressionLinearInterpolate>(0);
		G.Connect(VC, TEXT(""), Mix, TEXT("A"));
		G.Connect(Tex, TEXT("RGB"), Mix, TEXT("B"));
		G.Connect(Tex, TEXT("A"), Mix, TEXT("Alpha"));
		UMaterialExpression* Shade = G.BakedShade(VC, 300);
		G.Column = -150;
		G.Out(G.Multiply(Mix, TEXT(""), Shade, TEXT(""), 0), TEXT(""), MP_BaseColor);
		G.Surface(0.9f, 0.2f);
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
		G.Surface(0.9f, 0.2f);
		break;
	}
	case ELBAMaterial::Water:
	{
		Material->BlendMode = BLEND_Translucent;
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		UMaterialExpressionVectorParameter* Color = G.Add<UMaterialExpressionVectorParameter>(0);
		Color->ParameterName = TEXT("WaterColor");
		Color->DefaultValue = FLinearColor(0.02f, 0.12f, 0.22f);
		G.Out(Color, TEXT(""), MP_BaseColor);
		G.Out(G.Scalar(TEXT("Opacity"), 0.85f, 200), TEXT(""), MP_Opacity);
		G.Surface(0.05f, 0.7f);
		break;
	}
	case ELBAMaterial::TexturedTranslucent:
	{
		Material->BlendMode = BLEND_Translucent;
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		UMaterialExpressionTextureSampleParameter2D* Tex = G.Texture(0);
		UMaterialExpressionVertexColor* VC = G.Add<UMaterialExpressionVertexColor>(260);
		G.Column = -300;
		G.Out(G.Multiply(Tex, TEXT("RGB"), VC, TEXT(""), 0), TEXT(""), MP_BaseColor);
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

	bool bNeedsRecompile = false;
	UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_SkeletalMesh, bNeedsRecompile);
	UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_InstancedStaticMeshes, bNeedsRecompile);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	Context.MarkForSave(Material);
	return Material;
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
	const FString Suffix = Kind == ELBAMaterial::TexturedMasked ? TEXT("_Masked")
		: Kind == ELBAMaterial::TexturedTranslucent ? TEXT("_Translucent") : FString();
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
