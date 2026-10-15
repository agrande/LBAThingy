#include "Import/LBAAudioImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/SoundFactory.h"
#include "Formats/LBAHqr.h"
#include "Import/LBAImportContext.h"
#include "LBA2Runtime.h"
#include "Misc/PackageName.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

namespace
{
	void WriteU32(TArray<uint8>& Out, uint32 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 4); }
	void WriteU16(TArray<uint8>& Out, uint16 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 2); }

	void WritePcm16Wav(TArray<uint8>& Out, int32 Channels, int32 SampleRate, const TArray<int16>& Samples)
	{
		const uint32 DataBytes = Samples.Num() * 2;
		Out.Reset();
		Out.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
		WriteU32(Out, 36 + DataBytes);
		Out.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8);
		WriteU32(Out, 16);
		WriteU16(Out, 1);
		WriteU16(Out, uint16(Channels));
		WriteU32(Out, SampleRate);
		WriteU32(Out, SampleRate * Channels * 2);
		WriteU16(Out, uint16(Channels * 2));
		WriteU16(Out, 16);
		Out.Append(reinterpret_cast<const uint8*>("data"), 4);
		WriteU32(Out, DataBytes);
		Out.Append(reinterpret_cast<const uint8*>(Samples.GetData()), DataBytes);
	}

	/** Standard IMA ADPCM block decoder (WAVE_FORMAT_IMA_ADPCM). */
	void DecodeImaAdpcm(const uint8* Data, int32 Size, int32 Channels, int32 BlockAlign, TArray<int16>& Out)
	{
		static const int32 StepTable[89] =
		{
			7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
			130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
			1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
			7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
		};
		static const int32 IndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
		if (Channels != 1 || BlockAlign <= 4)
		{
			return;
		}
		for (int32 Block = 0; Block + 4 <= Size; Block += BlockAlign)
		{
			const int32 End = FMath::Min(Block + BlockAlign, Size);
			int32 Predictor = int16(Data[Block] | (Data[Block + 1] << 8));
			int32 Index = FMath::Clamp<int32>(Data[Block + 2], 0, 88);
			Out.Add(int16(Predictor));
			for (int32 At = Block + 4; At < End; ++At)
			{
				for (int32 Half = 0; Half < 2; ++Half)
				{
					const int32 Nibble = Half == 0 ? (Data[At] & 0x0F) : (Data[At] >> 4);
					const int32 Step = StepTable[Index];
					int32 Diff = Step >> 3;
					if (Nibble & 1) Diff += Step >> 2;
					if (Nibble & 2) Diff += Step >> 1;
					if (Nibble & 4) Diff += Step;
					Predictor = FMath::Clamp(Predictor + ((Nibble & 8) ? -Diff : Diff), -32768, 32767);
					Index = FMath::Clamp(Index + IndexTable[Nibble], 0, 88);
					Out.Add(int16(Predictor));
				}
			}
		}
	}
}

FLBAAudioImporter::FLBAAudioImporter(FLBAImportContext& InContext)
	: Context(InContext)
{
}

bool FLBAAudioImporter::ConvertTo16BitWav(const TArray<uint8>& In, TArray<uint8>& Out)
{
	if (In.Num() < 12 || FMemory::Memcmp(In.GetData(), "RIFF", 4) != 0)
	{
		return false;
	}
	FLBAReader R(In);
	int32 Format = 0, Channels = 1, SampleRate = 22050, BlockAlign = 0, Bits = 8;
	int32 DataAt = INDEX_NONE, DataSize = 0;
	for (int32 At = 12; At + 8 <= In.Num();)
	{
		const uint32 ChunkSize = R.U32At(At + 4);
		if (FMemory::Memcmp(In.GetData() + At, "fmt ", 4) == 0)
		{
			Format = R.U16At(At + 8);
			Channels = R.U16At(At + 10);
			SampleRate = int32(R.U32At(At + 12));
			BlockAlign = R.U16At(At + 20);
			Bits = R.U16At(At + 22);
		}
		else if (FMemory::Memcmp(In.GetData() + At, "data", 4) == 0)
		{
			DataAt = At + 8;
			DataSize = FMath::Min<int32>(int32(ChunkSize), In.Num() - DataAt);
			break;
		}
		At += 8 + int32(ChunkSize) + (ChunkSize & 1);
	}
	if (DataAt == INDEX_NONE || Channels < 1 || Channels > 2 || SampleRate <= 0)
	{
		return false;
	}

	TArray<int16> Samples;
	const uint8* Data = In.GetData() + DataAt;
	if (Format == 1 && Bits == 8)
	{
		Samples.SetNumUninitialized(DataSize);
		for (int32 I = 0; I < DataSize; ++I)
		{
			Samples[I] = int16((int32(Data[I]) - 128) << 8);
		}
	}
	else if (Format == 1 && Bits == 16)
	{
		Samples.SetNumUninitialized(DataSize / 2);
		FMemory::Memcpy(Samples.GetData(), Data, Samples.Num() * 2);
	}
	else if (Format == 0x11)
	{
		DecodeImaAdpcm(Data, DataSize, Channels, BlockAlign, Samples);
	}
	if (Samples.Num() == 0)
	{
		return false;
	}
	WritePcm16Wav(Out, Channels, SampleRate, Samples);
	return true;
}

USoundWave* FLBAAudioImporter::CreateSoundWave(FLBAImportContext& Context, const FString& Folder, const FString& Name, const TArray<uint8>& Wav16)
{
	if (USoundWave* Existing = Context.FindAsset<USoundWave>(Folder, Name))
	{
		return Existing;
	}
	USoundFactory* Factory = NewObject<USoundFactory>();
	Factory->bAutoCreateCue = false;
	UPackage* Package = CreatePackage(*(Folder / Name));
	const uint8* Begin = Wav16.GetData();
	UObject* Created = Factory->FactoryCreateBinary(USoundWave::StaticClass(), Package, FName(*Name), RF_Public | RF_Standalone, nullptr,
		TEXT("WAV"), Begin, Begin + Wav16.Num(), GWarn);
	USoundWave* Sound = Cast<USoundWave>(Created);
	if (Sound)
	{
		FAssetRegistryModule::AssetCreated(Sound);
		Context.MarkForSave(Sound);
	}
	return Sound;
}

USoundWave* FLBAAudioImporter::GetSample(int32 Index)
{
	if (USoundWave** Found = Cache.Find(Index))
	{
		return *Found;
	}
	USoundWave*& Result = Cache.Add(Index, nullptr);
	const FLBAHqrFile* Samples = Context.GetHqr(TEXT("SAMPLES.HQR"));
	TArray<uint8> Raw, Wav;
	if (!Samples || !Samples->GetEntry(Index, Raw) || !ConvertTo16BitWav(Raw, Wav))
	{
		return nullptr;
	}
	Result = CreateSoundWave(Context, Context.GetContentPath(TEXT("Audio/Samples")), FString::Printf(TEXT("SFX_%03d"), Index), Wav);
	return Result;
}

int32 FLBAAudioImporter::ImportAllSamples()
{
	const FLBAHqrFile* Samples = Context.GetHqr(TEXT("SAMPLES.HQR"));
	int32 Count = 0;
	for (int32 Index = 0; Samples && Index < Samples->Num(); ++Index)
	{
		Count += GetSample(Index) ? 1 : 0;
	}
	Context.Flush();
	return Count;
}
