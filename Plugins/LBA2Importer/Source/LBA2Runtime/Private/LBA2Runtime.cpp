#include "LBA2Runtime.h"

#include "LBATestStation.h"
#include "UObject/AssetRegistryTagsContext.h"

DEFINE_LOG_CATEGORY(LogLBA2);

void FLBA2RuntimeModule::StartupModule()
{
#if WITH_EDITOR
	// Levels list their test stations in the asset registry, for the travel menu.
	StationsTagHandle = UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.AddStatic(&ALBATestStation::AddLevelStationsTag);
#endif
}

void FLBA2RuntimeModule::ShutdownModule()
{
#if WITH_EDITOR
	UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.Remove(StationsTagHandle);
#endif
}

IMPLEMENT_MODULE(FLBA2RuntimeModule, LBA2Runtime)
