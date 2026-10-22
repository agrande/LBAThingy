#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "LBAWallStyles.generated.h"

class UStaticMesh;

/** Which side of a wall a style dresses. */
UENUM(BlueprintType)
enum class ELBAWallFacing : uint8
{
	/** The inside of a room (the back walls the iso camera looks at). */
	Indoor,
	/** The outside of a building (the camera-side walls seen from beyond, facades over lower ground). */
	Outdoor,
};

/** Rows of a layout stacked in a wall: From is its first row (layouts are often cropped or stretched). */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAWallSegment
{
	GENERATED_BODY()

	/** Layout index in the library. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	int32 Layout = 0;

	/** First row of the layout used. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	int32 From = 0;

	/** Rows used (cells it adds to the wall's height). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	int32 Height = 1;

	bool Contains(int32 InLayout, int32 Row) const { return Layout == InLayout && (Row < 0 || (Row >= From && Row < From + Height)); }

	/** Width of the layout along the wall, in cells: the period of its pattern. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	int32 Width = 1;

	/** The layout's kit piece (SM_LibNN_Layout_MMM), when kit pieces are imported. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	TSoftObjectPtr<UStaticMesh> Mesh;
};

/**
 * How the original builds a full wall from layout rows, bottom up: a foot, a body (the rows it repeats to make a wall
 * taller, or its tallest panel) and a top (caps, cornices). The walls the original cut down for the camera are these
 * styles cropped: they go on with the rest of their own layouts.
 */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAWallStyle
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	ELBAWallFacing Facing = ELBAWallFacing::Indoor;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	TArray<FLBAWallSegment> Foot;

	/** Repeated as often as the wall's height needs (at least once). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	TArray<FLBAWallSegment> Body;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	TArray<FLBAWallSegment> Top;

	/** Wall columns built this way in the original grids (how common the style is). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wall")
	int32 Uses = 0;

	/** Grids it was found in. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wall")
	TArray<int32> Grids;

	/** Height of the wall as the original builds it, in cells (the body once). */
	int32 GetHeight() const;
	/** Whether a layout's row (any row: -1) is in the style. */
	bool Contains(int32 Layout, int32 Row = -1) const;
};

/**
 * The wall styles of one layout library, found in its grids by the importer (DA_LibNN_WallStyles). The enclosure
 * raises the cut walls with them, and they are the vocabulary for building new rooms from the library's layouts.
 */
UCLASS(BlueprintType)
class LBA2RUNTIME_API ULBAWallStyleLibrary : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Layout library index in LBA_BKG.HQR. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wall")
	int32 Library = 0;

	/** Most used first, per facing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wall")
	TArray<FLBAWallStyle> Styles;

	/**
	 * The style containing a layout's row (any row: -1), INDEX_NONE if none: of the given facing if any, the most
	 * walls of it (uses times height: a full wall over its cut-down versions).
	 */
	int32 FindStyle(int32 Layout, int32 Row, ELBAWallFacing Facing) const { return FindStyle(Styles, Layout, Row, Facing); }
	static int32 FindStyle(TConstArrayView<FLBAWallStyle> Styles, int32 Layout, int32 Row, ELBAWallFacing Facing);

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("LBAWallStyles"), GetFName());
	}
};
