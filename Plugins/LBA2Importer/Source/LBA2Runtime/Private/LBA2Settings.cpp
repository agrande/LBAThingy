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
	Boat = Entity(TEXT("DA_Ferry"));
	for (const TCHAR* Name : { TEXT("DA_TwinsenNormal"), TEXT("DA_TwinsenAthletic"), TEXT("DA_TwinsenAggressive"), TEXT("DA_TwinsenDiscreet"),
		TEXT("DA_TwinsenProtopack"), TEXT("DA_TwinsenAndZoe"), TEXT("DA_TwinsenHorn"), TEXT("DA_TwinsenSpacesuitIndoorsNormal"),
		TEXT("DA_TwinsenSuperJetpack"), TEXT("DA_TwinsenSpacesuitIndoorsAthletic"), TEXT("DA_TwinsenSpacesuitOutdoorsNormal"),
		TEXT("DA_TwinsenSpacesuitOutdoorsAthletic"), TEXT("DA_TwinsenCar"), TEXT("DA_TwinsenElectrocuted"), TEXT("DA_ZoeAndTwinsen"),
		TEXT("DA_TwinsenWithCake"), TEXT("DA_TwinsenWithBalloons"), TEXT("DA_GloumCarryingTwinsen"), TEXT("DA_MosquibeeAndTwinsen"),
		TEXT("DA_Dinofly"), TEXT("DA_MecaPenguin"), TEXT("DA_NitroMecaPenguin"),
		TEXT("DA_Ferry") })
	{
		HiddenSceneEntities.Add(Entity(Name));
	}
	for (const TCHAR* Name : { TEXT("DA_FrancoAlly"), TEXT("DA_JoeTheElf"), TEXT("DA_SociableQuetch"), TEXT("DA_LoveTwinsunSupClerk") })
	{
		NonEnemySceneEntities.Add(Entity(Name));
	}
	for (const TCHAR* Level : { TEXT("/Game/LBA2/Characters/L_CharacterGallery"), TEXT("/Game/ThirdPerson/Lvl_ThirdPerson"),
		TEXT("/Game/Variant_Platforming/Lvl_Platforming"), TEXT("/Game/Variant_Combat/Lvl_Combat"), TEXT("/Game/Variant_SideScrolling/Lvl_SideScrolling") })
	{
		SandboxLevels.Add(TSoftObjectPtr<UWorld>(FSoftObjectPath(FString::Printf(TEXT("%s.%s"), Level, *FPackageName::GetShortName(Level)))));
	}
}
