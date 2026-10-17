#include "LBA2Settings.h"

#include "LBAEntityAsset.h"

ULBA2Settings::ULBA2Settings()
{
	auto Entity = [](const TCHAR* Name)
	{
		return TSoftObjectPtr<ULBAEntityAsset>(FSoftObjectPath(FString::Printf(TEXT("/Game/LBA2/Characters/Entities/%s.%s"), Name, Name)));
	};
	HeroNormal = Entity(TEXT("DA_TwinsenNormal"));
	HeroAthletic = Entity(TEXT("DA_TwinsenAthletic"));
	HeroAggressive = Entity(TEXT("DA_TwinsenAggressive"));
	HeroDiscreet = Entity(TEXT("DA_TwinsenDiscreet"));
	Dinofly = Entity(TEXT("DA_Dinofly"));
}
