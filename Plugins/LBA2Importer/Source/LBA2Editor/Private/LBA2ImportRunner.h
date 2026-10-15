#pragma once

#include "CoreMinimal.h"
#include "LBA2ImporterSettings.h"

/** Runs one import pass over the selected layers. */
class FLBA2ImportRunner
{
public:
	static bool Run(const ULBA2ImporterSettings& Settings, const TArray<ELBAImportLayer>& Layers, FString& OutSummary);

	static FString LayerToString(ELBAImportLayer Layer);
	static bool LayerFromString(const FString& In, ELBAImportLayer& Out);
	static TArray<ELBAImportLayer> AllLayers();
};
