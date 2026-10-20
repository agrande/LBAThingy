#include "LBA2ImportCommandlet.h"

#include "LBA2ImporterSettings.h"
#include "LBA2ImportRunner.h"
#include "LBA2Runtime.h"

ULBA2ImportCommandlet::ULBA2ImportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 ULBA2ImportCommandlet::Main(const FString& Params)
{
	ULBA2ImporterSettings* Settings = GetMutableDefault<ULBA2ImporterSettings>();

	FString Value;
	if (FParse::Value(*Params, TEXT("GameDir="), Value, false))
	{
		Settings->GameDirectory.Path = Value;
	}
	Settings->AutoDetectGameDirectory();
	if (FParse::Value(*Params, TEXT("RemakeDir="), Value, false))
	{
		Settings->RemakeDirectory.Path = Value;
	}
	if (FParse::Param(*Params, TEXT("NoRemake")))
	{
		Settings->bUseRemakeModels = false;
	}
	if (FParse::Value(*Params, TEXT("Root="), Value, false))
	{
		Settings->ContentRoot.Path = Value;
	}
	if (FParse::Value(*Params, TEXT("Islands="), Value, false))
	{
		Value.ParseIntoArray(Settings->IslandFilter, TEXT(","));
	}
	if (FParse::Value(*Params, TEXT("Grids="), Value, false))
	{
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		Settings->GridFilter.Reset();
		for (const FString& Part : Parts)
		{
			Settings->GridFilter.Add(FCString::Atoi(*Part));
		}
	}

	if (FParse::Value(*Params, TEXT("Scenes="), Value, false))
	{
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		Settings->SceneFilter.Reset();
		for (const FString& Part : Parts)
		{
			Settings->SceneFilter.Add(FCString::Atoi(*Part));
		}
	}
	if (FParse::Param(*Params, TEXT("DemoScenes")))
	{
		Settings->bImportDemoScenes = true;
	}

	if (FParse::Value(*Params, TEXT("Entities="), Value, false))
	{
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		Settings->EntityFilter.Reset();
		for (const FString& Part : Parts)
		{
			Settings->EntityFilter.Add(FCString::Atoi(*Part));
		}
	}

	TArray<ELBAImportLayer> Layers;
	if (FParse::Value(*Params, TEXT("Layers="), Value, false))
	{
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		for (const FString& Part : Parts)
		{
			ELBAImportLayer Layer;
			if (FLBA2ImportRunner::LayerFromString(Part, Layer))
			{
				Layers.Add(Layer);
			}
			else
			{
				UE_LOG(LogLBA2, Error, TEXT("Unknown layer '%s'"), *Part);
				return 1;
			}
		}
	}
	else
	{
		Layers = FLBA2ImportRunner::AllLayers();
	}

	FString Summary;
	return FLBA2ImportRunner::Run(*Settings, Layers, Summary) ? 0 : 1;
}
