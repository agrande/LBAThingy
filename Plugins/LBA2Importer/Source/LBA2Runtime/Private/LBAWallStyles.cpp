#include "LBAWallStyles.h"

#include "Engine/StaticMesh.h"

int32 FLBAWallStyle::GetHeight() const
{
	int32 Height = 0;
	for (const TArray<FLBAWallSegment>* Part : { &Foot, &Body, &Top })
	{
		for (const FLBAWallSegment& Segment : *Part)
		{
			Height += Segment.Height;
		}
	}
	return Height;
}

bool FLBAWallStyle::Contains(int32 Layout, int32 Row) const
{
	auto Has = [Layout, Row](const TArray<FLBAWallSegment>& Part)
	{
		return Part.ContainsByPredicate([Layout, Row](const FLBAWallSegment& Segment) { return Segment.Contains(Layout, Row); });
	};
	return Has(Foot) || Has(Body) || Has(Top);
}

int32 ULBAWallStyleLibrary::FindStyle(TConstArrayView<FLBAWallStyle> Styles, int32 Layout, int32 Row, ELBAWallFacing Facing)
{
	int32 Best = INDEX_NONE;
	for (int32 Index = 0; Index < Styles.Num(); ++Index)
	{
		const FLBAWallStyle& Style = Styles[Index];
		if (!Style.Contains(Layout, Row))
		{
			continue;
		}
		if (Best == INDEX_NONE)
		{
			Best = Index;
			continue;
		}
		const FLBAWallStyle& Current = Styles[Best];
		const bool bFacing = Style.Facing == Facing, bCurrentFacing = Current.Facing == Facing;
		if (bFacing != bCurrentFacing ? bFacing : Style.Uses * Style.GetHeight() > Current.Uses * Current.GetHeight())
		{
			Best = Index;
		}
	}
	return Best;
}
