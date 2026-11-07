#pragma once

#include "CoreMinimal.h"

struct FLBALayout;
namespace LBA1
{
	class FData;
}

/**
 * Globe mode: the kit of LBA1's towns (Phase 3 of LBA1_ISLANDS_PLAN.md). A recurring piece of LBA1's towns (a layout: a
 * lamppost, a barrel, a tree) is drawn as a 3D model instead of as its bricks, which keep only its collision.
 *
 * A piece is known by its size and bricks, the same in every scene's library (each scene numbers its layouts its own way).
 * The models come from:
 *   - Resources/Models/lba1_kit.json: LBA2's island decor (SM_<ISLAND>_Decor_<NNN>, the islands' import) or remake models,
 *     each naming one example of its piece ("scene:layout", as Tools/lba1_kit_survey.py prints them on pieces_big.png);
 *   - the remake's own LBA1 replacements (www/metadata/lba1/layouts.json: its barrel and box), wherever their piece is.
 * The table wins over the remake. Tools/lba2_decor_sheet.py draws the islands' decor bodies to choose from.
 */
namespace LBAPlanetKit
{
	/** A model standing for a piece, placed on its footprint's center, on its lowest cell. */
	struct FModel
	{
		FString Name;
		/** LBA2's island decor: the island (CITADEL) and its body (.OBL entry). */
		FString Island;
		int32 Body = INDEX_NONE;
		/** Or a remake model, under www/models (lba1/layouts/CI_0_layout_70_barrel.glb). */
		FString RemakeFile;
		/** Turn (degrees, about up) and scale. */
		double Yaw = 0.0;
		double Scale = 1.0;
		/** Scaled (uniformly, times Scale) to the piece's height, its bottom on the piece's floor. */
		bool bFitHeight = false;
		/** One copy per this many cells along the piece's longer side (0: one copy), each turned at random when bRandomTurn. */
		double Every = 0.0;
		bool bRandomTurn = false;
	};

	class FTable
	{
	public:
		/** Reads the table and, when RemakeRoot (an lba2remake checkout) is set, the remake's LBA1 replacements. */
		void Load(const LBA1::FData& Data, const FString& RemakeRoot);

		/** The model standing for a layout (any scene's), or INDEX_NONE: it stays bricks. */
		int32 Find(const FLBALayout& Layout) const;
		const FModel& GetModel(int32 Index) const { return Models[Index]; }
		int32 Num() const { return Models.Num(); }

		/** A piece's key: its size and bricks ("1x10x1:512,513,..."). */
		static FString KeyOf(const FLBALayout& Layout);

	private:
		TArray<FModel> Models;
		TMap<FString, int32> ByKey;
	};
}
