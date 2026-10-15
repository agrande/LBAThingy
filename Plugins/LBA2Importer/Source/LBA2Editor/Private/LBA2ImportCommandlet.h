#pragma once

#include "Commandlets/Commandlet.h"
#include "LBA2ImportCommandlet.generated.h"

/**
 * Headless import:
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=LBA2Import [-GameDir="C:/GOG Games/Little Big Adventure 2"]
 *       [-Layers=Islands,Interiors,Characters,...] [-Islands=CITABAU,DESERT] [-Grids=0,1,2] [-Root=/Game/LBA2]
 */
UCLASS()
class ULBA2ImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	ULBA2ImportCommandlet();
	virtual int32 Main(const FString& Params) override;
};
