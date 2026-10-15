#include "Formats/LBAGltf.h"

#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	struct FGlbContext
	{
		TSharedPtr<FJsonObject> Json;
		TArray<uint8> Bin;

		const TArray<TSharedPtr<FJsonValue>>& Array(const TCHAR* Name) const
		{
			static const TArray<TSharedPtr<FJsonValue>> Empty;
			const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
			return Json->TryGetArrayField(Name, Out) && Out ? *Out : Empty;
		}

		/** Returns a pointer to the bytes of a buffer view (only the GLB BIN chunk is supported). */
		const uint8* View(int32 ViewIndex, int32& OutLength, int32& OutStride) const
		{
			const TArray<TSharedPtr<FJsonValue>>& Views = Array(TEXT("bufferViews"));
			if (!Views.IsValidIndex(ViewIndex))
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject> V = Views[ViewIndex]->AsObject();
			const int32 Offset = V->HasField(TEXT("byteOffset")) ? int32(V->GetNumberField(TEXT("byteOffset"))) : 0;
			OutLength = int32(V->GetNumberField(TEXT("byteLength")));
			OutStride = V->HasField(TEXT("byteStride")) ? int32(V->GetNumberField(TEXT("byteStride"))) : 0;
			if (Offset < 0 || Offset + OutLength > Bin.Num())
			{
				return nullptr;
			}
			return Bin.GetData() + Offset;
		}

		/** Reads an accessor as floats (normalized integer types are converted). */
		bool ReadFloats(int32 AccessorIndex, int32& OutComponents, TArray<float>& Out) const
		{
			const TArray<TSharedPtr<FJsonValue>>& Accessors = Array(TEXT("accessors"));
			if (!Accessors.IsValidIndex(AccessorIndex))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> A = Accessors[AccessorIndex]->AsObject();
			const FString Type = A->GetStringField(TEXT("type"));
			OutComponents = Type == TEXT("SCALAR") ? 1 : Type == TEXT("VEC2") ? 2 : Type == TEXT("VEC3") ? 3 : Type == TEXT("VEC4") ? 4 : 0;
			const int32 ComponentType = int32(A->GetNumberField(TEXT("componentType")));
			const int32 Count = int32(A->GetNumberField(TEXT("count")));
			const bool bNormalized = A->HasField(TEXT("normalized")) && A->GetBoolField(TEXT("normalized"));
			const int32 ComponentSize = (ComponentType == 5126 || ComponentType == 5125) ? 4 : (ComponentType == 5123 || ComponentType == 5122) ? 2 : 1;
			if (OutComponents == 0 || !A->HasField(TEXT("bufferView")))
			{
				return false;
			}
			int32 Length = 0, Stride = 0;
			const uint8* Data = View(int32(A->GetNumberField(TEXT("bufferView"))), Length, Stride);
			if (!Data)
			{
				return false;
			}
			const int32 AccessorOffset = A->HasField(TEXT("byteOffset")) ? int32(A->GetNumberField(TEXT("byteOffset"))) : 0;
			const int32 ElementSize = ComponentSize * OutComponents;
			Stride = Stride > 0 ? Stride : ElementSize;
			if (AccessorOffset + (Count - 1) * Stride + ElementSize > Length)
			{
				return false;
			}
			Out.SetNumUninitialized(Count * OutComponents);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const uint8* Element = Data + AccessorOffset + Index * Stride;
				for (int32 C = 0; C < OutComponents; ++C)
				{
					const uint8* P = Element + C * ComponentSize;
					float Value = 0.f;
					switch (ComponentType)
					{
					case 5126: { float F; FMemory::Memcpy(&F, P, 4); Value = F; break; }
					case 5125: { uint32 U; FMemory::Memcpy(&U, P, 4); Value = float(U); break; }
					case 5123: { uint16 U; FMemory::Memcpy(&U, P, 2); Value = bNormalized ? U / 65535.f : float(U); break; }
					case 5122: { int16 S; FMemory::Memcpy(&S, P, 2); Value = bNormalized ? FMath::Max(S / 32767.f, -1.f) : float(S); break; }
					case 5121: Value = bNormalized ? *P / 255.f : float(*P); break;
					case 5120: Value = bNormalized ? FMath::Max(int8(*P) / 127.f, -1.f) : float(int8(*P)); break;
					default: break;
					}
					Out[Index * OutComponents + C] = Value;
				}
			}
			return true;
		}

		bool ReadIndices(int32 AccessorIndex, TArray<uint32>& Out) const
		{
			const TArray<TSharedPtr<FJsonValue>>& Accessors = Array(TEXT("accessors"));
			if (!Accessors.IsValidIndex(AccessorIndex))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> A = Accessors[AccessorIndex]->AsObject();
			const int32 ComponentType = int32(A->GetNumberField(TEXT("componentType")));
			const int32 Count = int32(A->GetNumberField(TEXT("count")));
			const int32 Size = ComponentType == 5125 ? 4 : ComponentType == 5123 ? 2 : 1;
			int32 Length = 0, Stride = 0;
			const uint8* Data = View(int32(A->GetNumberField(TEXT("bufferView"))), Length, Stride);
			const int32 AccessorOffset = A->HasField(TEXT("byteOffset")) ? int32(A->GetNumberField(TEXT("byteOffset"))) : 0;
			if (!Data || AccessorOffset + Count * Size > Length)
			{
				return false;
			}
			Out.SetNumUninitialized(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const uint8* P = Data + AccessorOffset + Index * Size;
				uint32 Value = 0;
				FMemory::Memcpy(&Value, P, Size);
				Out[Index] = Value;
			}
			return true;
		}
	};

	FMatrix44d LocalMatrix(const TSharedPtr<FJsonObject>& Node)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Node->TryGetArrayField(TEXT("matrix"), Values) && Values->Num() == 16)
		{
			// Column-major in glTF; FMatrix is row-vector so the layouts match element for element.
			FMatrix44d M;
			for (int32 I = 0; I < 16; ++I)
			{
				M.M[I / 4][I % 4] = (*Values)[I]->AsNumber();
			}
			return M;
		}
		FVector3d T(0), S(1);
		FQuat4d R = FQuat4d::Identity;
		if (Node->TryGetArrayField(TEXT("translation"), Values) && Values->Num() == 3)
		{
			T = FVector3d((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
		}
		if (Node->TryGetArrayField(TEXT("rotation"), Values) && Values->Num() == 4)
		{
			R = FQuat4d((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber(), (*Values)[3]->AsNumber());
		}
		if (Node->TryGetArrayField(TEXT("scale"), Values) && Values->Num() == 3)
		{
			S = FVector3d((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
		}
		// The quaternion math is handedness-agnostic: it rotates vectors the same way in glTF space.
		return FTransform3d(R, T, S).ToMatrixWithScale();
	}
}

bool FLBAGltfModel::Load(const FString& Path, FString& OutError)
{
	TArray<uint8> File;
	if (!FFileHelper::LoadFileToArray(File, *Path) || File.Num() < 20)
	{
		OutError = TEXT("cannot read file");
		return false;
	}
	uint32 Magic, Version, Length;
	FMemory::Memcpy(&Magic, File.GetData(), 4);
	FMemory::Memcpy(&Version, File.GetData() + 4, 4);
	FMemory::Memcpy(&Length, File.GetData() + 8, 4);
	if (Magic != 0x46546C67 || Version != 2)
	{
		OutError = TEXT("not a glTF 2.0 binary file");
		return false;
	}

	FGlbContext Ctx;
	int32 At = 12;
	while (At + 8 <= File.Num())
	{
		uint32 ChunkLength, ChunkType;
		FMemory::Memcpy(&ChunkLength, File.GetData() + At, 4);
		FMemory::Memcpy(&ChunkType, File.GetData() + At + 4, 4);
		const int32 DataStart = At + 8;
		if (DataStart + int32(ChunkLength) > File.Num())
		{
			break;
		}
		if (ChunkType == 0x4E4F534A) // JSON
		{
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(File.GetData() + DataStart), ChunkLength);
			const FString Text(Converter.Length(), Converter.Get());
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			FJsonSerializer::Deserialize(Reader, Ctx.Json);
		}
		else if (ChunkType == 0x004E4942) // BIN
		{
			Ctx.Bin.Append(File.GetData() + DataStart, ChunkLength);
		}
		At = DataStart + int32(ChunkLength);
	}
	if (!Ctx.Json.IsValid())
	{
		OutError = TEXT("invalid JSON chunk");
		return false;
	}

	// Images (embedded in buffer views).
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	for (const TSharedPtr<FJsonValue>& Value : Ctx.Array(TEXT("images")))
	{
		FLBAGltfImage& Image = Images.AddDefaulted_GetRef();
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj->HasField(TEXT("bufferView")))
		{
			continue;
		}
		int32 ViewLength = 0, Stride = 0;
		const uint8* Data = Ctx.View(int32(Obj->GetNumberField(TEXT("bufferView"))), ViewLength, Stride);
		if (!Data)
		{
			continue;
		}
		const EImageFormat Format = ImageWrapperModule.DetectImageFormat(Data, ViewLength);
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(Format);
		TArray<uint8> Raw;
		if (Wrapper.IsValid() && Wrapper->SetCompressed(Data, ViewLength) && Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
		{
			Image.Width = int32(Wrapper->GetWidth());
			Image.Height = int32(Wrapper->GetHeight());
			Image.Pixels.SetNumUninitialized(Image.Width * Image.Height);
			FMemory::Memcpy(Image.Pixels.GetData(), Raw.GetData(), FMath::Min(Raw.Num(), Image.Pixels.Num() * 4));
			Image.Hash = FMD5::HashBytes(Data, ViewLength).Left(12);
		}
	}

	TArray<int32> TextureToImage;
	for (const TSharedPtr<FJsonValue>& Value : Ctx.Array(TEXT("textures")))
	{
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		TextureToImage.Add(Obj->HasField(TEXT("source")) ? int32(Obj->GetNumberField(TEXT("source"))) : INDEX_NONE);
	}

	for (const TSharedPtr<FJsonValue>& Value : Ctx.Array(TEXT("materials")))
	{
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		FLBAGltfMaterial& Material = Materials.AddDefaulted_GetRef();
		Obj->TryGetStringField(TEXT("name"), Material.Name);
		const TSharedPtr<FJsonObject>* Pbr = nullptr;
		if (Obj->TryGetObjectField(TEXT("pbrMetallicRoughness"), Pbr))
		{
			const TArray<TSharedPtr<FJsonValue>>* Factor = nullptr;
			if ((*Pbr)->TryGetArrayField(TEXT("baseColorFactor"), Factor) && Factor->Num() == 4)
			{
				Material.BaseColor = FLinearColor((*Factor)[0]->AsNumber(), (*Factor)[1]->AsNumber(), (*Factor)[2]->AsNumber(), (*Factor)[3]->AsNumber());
			}
			const TSharedPtr<FJsonObject>* TextureInfo = nullptr;
			if ((*Pbr)->TryGetObjectField(TEXT("baseColorTexture"), TextureInfo))
			{
				const int32 Texture = int32((*TextureInfo)->GetNumberField(TEXT("index")));
				Material.Image = TextureToImage.IsValidIndex(Texture) ? TextureToImage[Texture] : INDEX_NONE;
			}
		}
		FString AlphaMode;
		if (Obj->TryGetStringField(TEXT("alphaMode"), AlphaMode))
		{
			Material.AlphaMode = AlphaMode == TEXT("BLEND") ? FLBAGltfMaterial::EAlpha::Blend
				: AlphaMode == TEXT("MASK") ? FLBAGltfMaterial::EAlpha::Mask : FLBAGltfMaterial::EAlpha::Opaque;
		}
		const TSharedPtr<FJsonObject>* Extensions = nullptr;
		Material.bUnlit = Obj->TryGetObjectField(TEXT("extensions"), Extensions) && (*Extensions)->HasField(TEXT("KHR_materials_unlit"));
	}

	// Walk the default scene, baking node transforms.
	const TArray<TSharedPtr<FJsonValue>>& Nodes = Ctx.Array(TEXT("nodes"));
	const TArray<TSharedPtr<FJsonValue>>& Meshes = Ctx.Array(TEXT("meshes"));
	TArray<int32> Roots;
	const TArray<TSharedPtr<FJsonValue>>& Scenes = Ctx.Array(TEXT("scenes"));
	const int32 SceneIndex = Ctx.Json->HasField(TEXT("scene")) ? int32(Ctx.Json->GetNumberField(TEXT("scene"))) : 0;
	if (Scenes.IsValidIndex(SceneIndex))
	{
		const TArray<TSharedPtr<FJsonValue>>* SceneNodes = nullptr;
		if (Scenes[SceneIndex]->AsObject()->TryGetArrayField(TEXT("nodes"), SceneNodes))
		{
			for (const TSharedPtr<FJsonValue>& N : *SceneNodes)
			{
				Roots.Add(int32(N->AsNumber()));
			}
		}
	}

	TFunction<void(int32, const FMatrix44d&, int32)> Visit = [&](int32 NodeIndex, const FMatrix44d& Parent, int32 Depth)
	{
		if (!Nodes.IsValidIndex(NodeIndex) || Depth > 64)
		{
			return;
		}
		const TSharedPtr<FJsonObject> Node = Nodes[NodeIndex]->AsObject();
		const FMatrix44d World = LocalMatrix(Node) * Parent;
		const FMatrix44d NormalMatrix = World.Inverse().GetTransposed();

		if (Node->HasField(TEXT("mesh")))
		{
			const int32 MeshIndex = int32(Node->GetNumberField(TEXT("mesh")));
			const TArray<TSharedPtr<FJsonValue>>* Prims = nullptr;
			if (Meshes.IsValidIndex(MeshIndex) && Meshes[MeshIndex]->AsObject()->TryGetArrayField(TEXT("primitives"), Prims))
			{
				for (const TSharedPtr<FJsonValue>& PrimValue : *Prims)
				{
					const TSharedPtr<FJsonObject> Prim = PrimValue->AsObject();
					const int32 Mode = Prim->HasField(TEXT("mode")) ? int32(Prim->GetNumberField(TEXT("mode"))) : 4;
					const TSharedPtr<FJsonObject>* Attributes = nullptr;
					if (Mode != 4 || !Prim->TryGetObjectField(TEXT("attributes"), Attributes))
					{
						continue;
					}
					FLBAGltfPrimitive Out;
					Out.Material = Prim->HasField(TEXT("material")) ? int32(Prim->GetNumberField(TEXT("material"))) : INDEX_NONE;

					int32 Components = 0;
					TArray<float> Floats;
					if (!(*Attributes)->HasField(TEXT("POSITION")) || !Ctx.ReadFloats(int32((*Attributes)->GetNumberField(TEXT("POSITION"))), Components, Floats) || Components != 3)
					{
						continue;
					}
					const int32 NumVertices = Floats.Num() / 3;
					for (int32 V = 0; V < NumVertices; ++V)
					{
						Out.Positions.Add(FVector3f(FVector3d(World.TransformPosition(FVector3d(Floats[V * 3], Floats[V * 3 + 1], Floats[V * 3 + 2])))));
					}
					if ((*Attributes)->HasField(TEXT("NORMAL")) && Ctx.ReadFloats(int32((*Attributes)->GetNumberField(TEXT("NORMAL"))), Components, Floats) && Components == 3)
					{
						for (int32 V = 0; V < NumVertices && V * 3 + 2 < Floats.Num(); ++V)
						{
							Out.Normals.Add(FVector3f(FVector3d(NormalMatrix.TransformVector(FVector3d(Floats[V * 3], Floats[V * 3 + 1], Floats[V * 3 + 2]))).GetSafeNormal()));
						}
					}
					if ((*Attributes)->HasField(TEXT("TEXCOORD_0")) && Ctx.ReadFloats(int32((*Attributes)->GetNumberField(TEXT("TEXCOORD_0"))), Components, Floats) && Components == 2)
					{
						for (int32 V = 0; V < NumVertices && V * 2 + 1 < Floats.Num(); ++V)
						{
							Out.UVs.Add(FVector2f(Floats[V * 2], Floats[V * 2 + 1]));
						}
					}
					if ((*Attributes)->HasField(TEXT("COLOR_0")) && Ctx.ReadFloats(int32((*Attributes)->GetNumberField(TEXT("COLOR_0"))), Components, Floats) && Components >= 3)
					{
						for (int32 V = 0; V < NumVertices && V * Components + Components - 1 < Floats.Num(); ++V)
						{
							Out.Colors.Add(FVector4f(Floats[V * Components], Floats[V * Components + 1], Floats[V * Components + 2], Components == 4 ? Floats[V * Components + 3] : 1.f));
						}
					}
					if (Prim->HasField(TEXT("indices")))
					{
						Ctx.ReadIndices(int32(Prim->GetNumberField(TEXT("indices"))), Out.Indices);
					}
					else
					{
						for (int32 V = 0; V < NumVertices; ++V)
						{
							Out.Indices.Add(V);
						}
					}
					Out.Indices.SetNum(Out.Indices.Num() - Out.Indices.Num() % 3);
					bool bValid = true;
					for (uint32 I : Out.Indices)
					{
						bValid &= int32(I) < NumVertices;
					}
					if (bValid && Out.Indices.Num() > 0)
					{
						Primitives.Add(MoveTemp(Out));
					}
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("children"), Children))
		{
			for (const TSharedPtr<FJsonValue>& Child : *Children)
			{
				Visit(int32(Child->AsNumber()), World, Depth + 1);
			}
		}
	};
	for (int32 Root : Roots)
	{
		Visit(Root, FMatrix44d::Identity, 0);
	}

	if (Primitives.Num() == 0)
	{
		OutError = TEXT("no triangle meshes");
		return false;
	}
	return true;
}
