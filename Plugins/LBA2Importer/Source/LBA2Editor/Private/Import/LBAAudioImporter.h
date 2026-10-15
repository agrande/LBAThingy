#pragma once

#include "CoreMinimal.h"

class FLBAImportContext;
class USoundWave;

/**
 * Sound effects (SAMPLES.HQR) -> USoundWave assets under <Root>/Audio/Samples.
 * LBA2 stores RIFF WAV files (mostly 8-bit PCM, a few IMA ADPCM); they are converted to 16-bit PCM.
 */
class FLBAAudioImporter
{
public:
	explicit FLBAAudioImporter(FLBAImportContext& InContext);

	/** Imports (or returns the already imported) sample. Null for empty entries. */
	USoundWave* GetSample(int32 Index);

	int32 ImportAllSamples();

	/** Converts any LBA WAV payload to a 16-bit PCM WAV file. Returns false if unsupported. */
	static bool ConvertTo16BitWav(const TArray<uint8>& In, TArray<uint8>& Out);

	static USoundWave* CreateSoundWave(FLBAImportContext& Context, const FString& Folder, const FString& Name, const TArray<uint8>& Wav16);

private:
	FLBAImportContext& Context;
	TMap<int32, USoundWave*> Cache;
};
