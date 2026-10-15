#include "LBA2ImportRunner.h"

#include "Import/LBAImportContext.h"
#include "Import/LBAIslandImporter.h"
#include "Import/LBAIsoImporter.h"
#include "Import/LBAAudioImporter.h"
#include "Import/LBACharacterImporter.h"
#include "LBA2Runtime.h"
#include "Misc/ScopedSlowTask.h"

#define LOCTEXT_NAMESPACE "LBA2ImportRunner"

FString FLBA2ImportRunner::LayerToString(ELBAImportLayer Layer)
{
	return StaticEnum<ELBAImportLayer>()->GetNameStringByValue(int64(Layer));
}

bool FLBA2ImportRunner::LayerFromString(const FString& In, ELBAImportLayer& Out)
{
	const int64 Value = StaticEnum<ELBAImportLayer>()->GetValueByNameString(In.TrimStartAndEnd());
	if (Value == INDEX_NONE)
	{
		return false;
	}
	Out = ELBAImportLayer(Value);
	return true;
}

TArray<ELBAImportLayer> FLBA2ImportRunner::AllLayers()
{
	TArray<ELBAImportLayer> Layers;
	const UEnum* Enum = StaticEnum<ELBAImportLayer>();
	for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
	{
		Layers.Add(ELBAImportLayer(Enum->GetValueByIndex(Index)));
	}
	return Layers;
}

bool FLBA2ImportRunner::Run(const ULBA2ImporterSettings& Settings, const TArray<ELBAImportLayer>& Layers, FString& OutSummary)
{
	FLBAImportContext Context(Settings);
	FString Error;
	if (!Context.Initialize(Error))
	{
		OutSummary = Error;
		UE_LOG(LogLBA2, Error, TEXT("%s"), *Error);
		return false;
	}

	FScopedSlowTask SlowTask(float(Layers.Num()) * 10.f, LOCTEXT("Importing", "Importing LBA2 data..."));
	SlowTask.MakeDialog(true);
	Context.SetSlowTask(&SlowTask);

	TArray<FString> Lines;
	const double StartTime = FPlatformTime::Seconds();
	for (ELBAImportLayer Layer : Layers)
	{
		if (Context.IsCancelled())
		{
			Lines.Add(TEXT("Cancelled."));
			break;
		}
		switch (Layer)
		{
		case ELBAImportLayer::Islands:
		{
			FLBAIslandImporter Importer(Context);
			const int32 Count = Importer.ImportAll();
			Lines.Add(FString::Printf(TEXT("Islands: %d imported"), Count));
			break;
		}
		case ELBAImportLayer::Interiors:
		{
			FLBAIsoImporter Importer(Context);
			const int32 Count = Importer.ImportAll();
			Lines.Add(FString::Printf(TEXT("Interiors: %d grids imported"), Count));
			break;
		}
		case ELBAImportLayer::Characters:
		{
			FLBAAudioImporter Audio(Context);
			FLBACharacterImporter Importer(Context, Audio);
			const int32 Count = Importer.ImportAll();
			Lines.Add(FString::Printf(TEXT("Characters: %d entities imported"), Count));
			break;
		}
		case ELBAImportLayer::Audio:
		{
			FLBAAudioImporter Audio(Context);
			const int32 Count = Audio.ImportAllSamples();
			Lines.Add(FString::Printf(TEXT("Audio: %d sound effects imported"), Count));
			break;
		}
		default:
			Lines.Add(FString::Printf(TEXT("%s: not implemented yet"), *LayerToString(Layer)));
			break;
		}
		Context.Flush();
	}

	Lines.Add(FString::Printf(TEXT("Done in %.1fs with %d warning(s). Assets are in %s."),
		FPlatformTime::Seconds() - StartTime, Context.GetNumWarnings(), *Context.GetContentRoot()));
	OutSummary = FString::Join(Lines, TEXT("\n"));
	UE_LOG(LogLBA2, Display, TEXT("%s"), *OutSummary);
	return true;
}

#undef LOCTEXT_NAMESPACE
