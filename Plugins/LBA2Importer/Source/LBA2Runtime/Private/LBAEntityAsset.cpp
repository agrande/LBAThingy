#include "LBAEntityAsset.h"

#include "Engine/SkeletalMesh.h"

USkeletalMesh* ULBAEntityAsset::GetDefaultBodyMesh() const
{
	int32 BestType = TNumericLimits<int32>::Max();
	USkeletalMesh* Best = nullptr;
	for (const TPair<int32, FLBAEntityBody>& Pair : Bodies)
	{
		if (Pair.Value.Mesh && Pair.Key < BestType)
		{
			BestType = Pair.Key;
			Best = Pair.Value.Mesh;
		}
	}
	return Best;
}
