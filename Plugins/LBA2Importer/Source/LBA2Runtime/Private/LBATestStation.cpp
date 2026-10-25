#include "LBATestStation.h"

#include "Components/CapsuleComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "LBAIslandTravelSubsystem.h"
#include "UObject/AssetRegistryTagsContext.h"

const FName ALBATestStation::StationsTag(TEXT("LBATestStations"));

ALBATestStation::ALBATestStation(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Ahead of the arriving player, facing them.
	NameSign = CreateDefaultSubobject<UTextRenderComponent>(TEXT("NameSign"));
	NameSign->SetupAttachment(GetCapsuleComponent());
	NameSign->SetRelativeLocationAndRotation(FVector(350.0, 0.0, 180.0), FRotator(0.0, 180.0, 0.0));
	NameSign->SetHorizontalAlignment(EHTA_Center);
	NameSign->SetVerticalAlignment(EVRTA_TextBottom);
	NameSign->SetWorldSize(48.f);
	NameSign->SetTextRenderColor(FColor(255, 217, 89));

	DescriptionSign = CreateDefaultSubobject<UTextRenderComponent>(TEXT("DescriptionSign"));
	DescriptionSign->SetupAttachment(NameSign);
	DescriptionSign->SetRelativeLocation(FVector(0.0, 0.0, -8.0));
	DescriptionSign->SetHorizontalAlignment(EHTA_Center);
	DescriptionSign->SetVerticalAlignment(EVRTA_TextTop);
	DescriptionSign->SetWorldSize(18.f);
	DescriptionSign->SetTextRenderColor(FColor::White);
}

void ALBATestStation::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	NameSign->SetText(DisplayName);
	DescriptionSign->SetText(Description);
	NameSign->SetVisibility(bShowSign, true);
}

#if WITH_EDITOR
void ALBATestStation::AddLevelStationsTag(FAssetRegistryTagsContext Context)
{
	const UWorld* World = Cast<UWorld>(Context.GetObject());
	if (!World || !World->PersistentLevel)
	{
		return;
	}
	TArray<FLBATestStationInfo> Stations;
	for (const AActor* Actor : World->PersistentLevel->Actors)
	{
		if (const ALBATestStation* Station = Cast<ALBATestStation>(Actor))
		{
			Stations.Add(FLBATestStationInfo::FromStation(*Station));
		}
	}
	if (!Stations.IsEmpty())
	{
		Context.AddTag(UObject::FAssetRegistryTag(StationsTag, FLBATestStationInfo::ToTagValue(Stations), UObject::FAssetRegistryTag::TT_Hidden));
	}
}
#endif
