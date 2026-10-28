#include "Import/LBAIsoImporter.h"

#include "Components/RectLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/RectLight.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Formats/LBAScene.h"
#include "GameFramework/PlayerStart.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBARoomPlan.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAGridPlanText.h"
#include "Import/LBAHollow.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBARemakeAssets.h"
#include "Import/LBAWallSurvey.h"
#include "LBA2ImporterSettings.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "LBA2Runtime.h"
#include "LBAGridPlanActor.h"
#include "LBATypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/AggregateGeom.h"

#define LOCTEXT_NAMESPACE "LBAIsoImporter"

namespace
{
	constexpr int32 SpriteW = FLBABrickSprite::Width;
	constexpr int32 SpriteH = FLBABrickSprite::Height;
	constexpr int32 CellPad = 2;
	constexpr int32 CellW = SpriteW + CellPad * 2;
	constexpr int32 CellH = SpriteH + CellPad * 2;
	/** Where each face of a brick sprite (top, +X, +Z) goes in its atlas cell: apart, with room to grow by a pixel. */
	const FIntPoint FaceOffsets[3] = { FIntPoint(2, 1), FIntPoint(3, 3), FIntPoint(1, 3) };

	/** The face (top, +X, +Z) a pixel of a brick sprite belongs to: above the lower edges of the top diamond, else its half. */
	int32 SpriteSlotOf(int32 X, int32 Y)
	{
		const float U = X + 0.5f, V = Y + 0.5f;
		return V <= 24.f - FMath::Abs(U - 24.f) * 0.5f ? 0 : U >= 24.f ? 1 : 2;
	}
	/** Opaque fraction from which a brick face counts as solid (not a see-through decoration or trim). */
	constexpr float SolidCoverage = 0.85f;
	/** Opaque fraction from which a brick face can be repeated up a raised wall without its holes showing (a full face
	 * measures 0.97: the face masks reach a little past the sprite's edges). */
	constexpr float OpaqueCoverage = 0.9f;

	/** Read access to a block of bricks (a whole grid or a single layout). */
	struct FCellQuery
	{
		FIntVector Size;
		TFunction<int32(int32 X, int32 Y, int32 Z)> Brick;
		TFunction<int32(int32 X, int32 Y, int32 Z)> Shape;

		bool Inside(int32 X, int32 Y, int32 Z) const
		{
			return X >= 0 && Y >= 0 && Z >= 0 && X < Size.X && Y < Size.Y && Z < Size.Z;
		}
		int32 BrickAt(int32 X, int32 Y, int32 Z) const { return Inside(X, Y, Z) ? Brick(X, Y, Z) : 0; }
		int32 ShapeAt(int32 X, int32 Y, int32 Z) const { return Inside(X, Y, Z) ? Shape(X, Y, Z) : 0; }
	};

	/** The cells of an iso grid: the art and collision shape of their layout block (or the invisible shape of cells without). */
	FCellQuery MakeGridQuery(const FLBAGrid& Grid, const FLBALibrary& Library, int32 Forbidden)
	{
		FCellQuery Query;
		Query.Size = FIntVector(LBA::GridSizeXZ, LBA::GridSizeY, LBA::GridSizeXZ);
		auto Block = [&Grid, &Library](int32 X, int32 Y, int32 Z) -> const FLBALayoutBlock*
		{
			const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
			if (!Library.Layouts.IsValidIndex(Cell.Layout))
			{
				return nullptr;
			}
			const FLBALayout& Layout = Library.Layouts[Cell.Layout];
			return Layout.Blocks.IsValidIndex(Cell.Block) ? &Layout.Blocks[Cell.Block] : nullptr;
		};
		Query.Brick = [Block, Forbidden](int32 X, int32 Y, int32 Z)
		{
			const FLBALayoutBlock* B = Block(X, Y, Z);
			return (B && B->Brick != Forbidden) ? int32(B->Brick) : 0;
		};
		Query.Shape = [Block, &Grid](int32 X, int32 Y, int32 Z)
		{
			if (const FLBALayoutBlock* B = Block(X, Y, Z))
			{
				return int32(B->Shape);
			}
			// Cells without a layout can still carry an invisible collision shape.
			const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
			return Cell.Layout < 0 ? int32(Cell.Block) : 0;
		};
		return Query;
	}

	FVector3f CornerPos(float X, float Y, float Z, float Scale)
	{
		return FVector3f(LBA::ToUnreal(X * LBA::BrickSizeXZ, Y * LBA::BrickSizeY, Z * LBA::BrickSizeXZ, Scale));
	}

	/**
	 * Faces of a brick in LBA brick units with their sprite coordinates (pixels, 48x38 sprite).
	 * Derived from the original iso projection: +X goes right/down on screen, +Z left/down, a brick is
	 * 24 px tall on its top diamond and 15 px on its vertical faces.
	 */
	struct FBrickFace
	{
		FIntVector Dir;
		FVector3f Corners[4];
		FVector2f Sprite[4];
	};

	const FBrickFace* GetBrickFaces()
	{
		static const FBrickFace Faces[6] =
		{
			// +Y top
			{ {0, 1, 0}, { {0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0} }, { {24, 0}, {0, 12}, {24, 24}, {48, 12} } },
			// -Y bottom (mirrors the top art)
			{ {0, -1, 0}, { {0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0} }, { {24, 0}, {0, 12}, {24, 24}, {48, 12} } },
			// +X (right half of the sprite)
			{ {1, 0, 0}, { {1, 1, 0}, {1, 1, 1}, {1, 0, 1}, {1, 0, 0} }, { {48, 12}, {24, 24}, {24, 39}, {48, 27} } },
			// -X (mirrors +X)
			{ {-1, 0, 0}, { {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {0, 0, 0} }, { {48, 12}, {24, 24}, {24, 39}, {48, 27} } },
			// +Z (left half of the sprite)
			{ {0, 0, 1}, { {0, 1, 1}, {1, 1, 1}, {1, 0, 1}, {0, 0, 1} }, { {0, 12}, {24, 24}, {24, 39}, {0, 27} } },
			// -Z (mirrors +Z)
			{ {0, 0, -1}, { {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 0, 0} }, { {0, 12}, {24, 24}, {24, 39}, {0, 27} } },
		};
		return Faces;
	}

	/** Index of the visible face whose art a hidden face mirrors: -Y -> top, -X -> +X, -Z -> +Z. */
	int32 FrontFaceOf(int32 FaceIndex)
	{
		return FaceIndex == 1 ? 0 : FaceIndex == 3 ? 2 : FaceIndex == 5 ? 4 : FaceIndex;
	}

	/** Index into FAtlas::FaceColors for a visible face (top, +X, +Z). */
	int32 ColorSlotOf(int32 FrontFace)
	{
		return FrontFace == 0 ? 0 : FrontFace == 2 ? 1 : 2;
	}

	/** KeepFace: whether a face (cell, face index) goes into this mesh; the others are left out. */
	void EmitVisual(const FCellQuery& Query, const FLBAIsoImporter::FAtlas& Atlas, FLBAMeshBuilder& Builder, int32 Section, float Scale,
		ELBABackFaceMode BackFaces, TFunctionRef<bool(const FIntVector&, int32)> KeepFace = [](const FIntVector&, int32) { return true; })
	{
		const FBrickFace* Faces = GetBrickFaces();
		int32 FillSection = INDEX_NONE;

		for (int32 Z = 0; Z < Query.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Query.Size.X; ++X)
			{
				for (int32 Y = 0; Y < Query.Size.Y; ++Y)
				{
					const int32 Brick = Query.BrickAt(X, Y, Z);
					const FIntPoint* Cell = Brick ? Atlas.Cells.Find(Brick) : nullptr;
					if (!Cell)
					{
						continue;
					}
					for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
					{
						const FBrickFace& Face = Faces[FaceIndex];
						if (Query.BrickAt(X + Face.Dir.X, Y + Face.Dir.Y, Z + Face.Dir.Z) != 0 || !KeepFace(FIntVector(X, Y, Z), FaceIndex))
						{
							continue;
						}
						const bool bHidden = FaceIndex == 1 || FaceIndex == 3 || FaceIndex == 5;
						if (bHidden && BackFaces == ELBABackFaceMode::None)
						{
							continue;
						}

						// Art source: by default the face itself (front faces) or the same brick's opposite face.
						const FIntPoint* SourceCell = Cell;
						int32 SourceBrick = Brick;
						bool bFill = false;
						if (bHidden && BackFaces == ELBABackFaceMode::Smart)
						{
							// Only solid faces get a back: thin decorations look better open than as floating cards.
							const int32 Slot = ColorSlotOf(FrontFaceOf(FaceIndex));
							auto Coverage = [&Atlas, Slot](int32 OfBrick)
							{
								const TStaticArray<float, 3>* Values = Atlas.FaceCoverage.Find(OfBrick);
								return Values ? (*Values)[Slot] : 0.f;
							};

							bool bMirrored = false;
							if (FaceIndex != 1)
							{
								// A wall is a brick with neighbors in the plane of the face (above/below or alongside).
								const FIntVector Side = FaceIndex == 3 ? FIntVector(0, 0, 1) : FIntVector(1, 0, 0);
								const int32 PlaneNeighbors = (Query.BrickAt(X, Y + 1, Z) != 0) + (Query.BrickAt(X, Y - 1, Z) != 0)
									+ (Query.BrickAt(X + Side.X, Y, Z + Side.Z) != 0) + (Query.BrickAt(X - Side.X, Y, Z - Side.Z) != 0);
								if (PlaneNeighbors >= 2)
								{
									// Walk through the wall thickness to its visible side and mirror that art.
									const FIntVector Through(-Face.Dir.X, 0, -Face.Dir.Z);
									FIntVector At(X, Y, Z);
									for (int32 Step = 0; Step < 64 && Query.BrickAt(At.X + Through.X, At.Y, At.Z + Through.Z) != 0; ++Step)
									{
										At += Through;
									}
									const int32 Far = Query.BrickAt(At.X, At.Y, At.Z);
									const FIntPoint* FarCell = Atlas.Cells.Find(Far);
									if (FarCell && Coverage(Far) >= SolidCoverage)
									{
										SourceCell = FarCell;
										SourceBrick = Far;
										bMirrored = true;
									}
								}
							}
							if (!bMirrored)
							{
								// Undersides and non-wall bricks: flat fill when solid, otherwise leave open.
								if (Coverage(Brick) < SolidCoverage)
								{
									continue;
								}
								bFill = true;
							}
						}

						const FBrickFace& ArtFace = Faces[FrontFaceOf(FaceIndex)];
						FLBAVertex V[4];
						const FVector3f Normal = FVector3f(LBA::DirToUnreal(Face.Dir.X, Face.Dir.Y, Face.Dir.Z));
						FVector4f FillColor(0.5f, 0.5f, 0.5f, 0.5f);
						if (bFill)
						{
							const TStaticArray<FColor, 3>* Colors = Atlas.FaceColors.Find(SourceBrick);
							const FColor Average = Colors ? (*Colors)[ColorSlotOf(FrontFaceOf(FaceIndex))] : FColor(128, 128, 128);
							FillColor = LBABodyConverter::ToVertexColor(Average, FaceIndex == 1 ? 0.35f : 0.45f);
							if (FillSection == INDEX_NONE)
							{
								FillSection = Builder.GetSection(TEXT("BrickFill"), Atlas.FillMaterial);
							}
						}
						for (int32 K = 0; K < 4; ++K)
						{
							V[K].Position = CornerPos(X + Face.Corners[K].X, Y + Face.Corners[K].Y, Z + Face.Corners[K].Z, Scale);
							V[K].Normal = Normal;
							V[K].Color = FillColor;
							const FIntPoint& Offset = FaceOffsets[ColorSlotOf(FrontFaceOf(FaceIndex))];
							V[K].UV = FVector2f((SourceCell->X + Offset.X + ArtFace.Sprite[K].X) / Atlas.Width, (SourceCell->Y + Offset.Y + ArtFace.Sprite[K].Y) / Atlas.Height);
						}
						const int32 Target = bFill ? FillSection : Section;
						Builder.AddTriangleFacing(Target, V[0], V[1], V[2], Normal);
						Builder.AddTriangleFacing(Target, V[0], V[2], V[3], Normal);
					}
				}
			}
		}
	}

	/** A wall finish: the wall layout rows (layout, row) the original stacks from the floor up (a dado, a rail, a body, a
	 * frieze, a cornice), laid from the floor, the top row right under the ceiling; where the wall is taller, the body is
	 * repeated: the row under the top in short finishes, else the row under the frieze, which stays under the cornice. */
	struct FWallpaper
	{
		TArray<FIntPoint> Rows;

		/** Finishes this tall keep their row under the top (a frieze) right under it. */
		static constexpr int32 MinFrieze = 5;

		/** The layout row at Above cells over the floor of a wall Height cells tall (INDEX_NONE layout: none). */
		FIntPoint GetRow(int32 Above, int32 Height) const
		{
			if (Rows.Num() == 0)
			{
				return FIntPoint(INDEX_NONE, 0);
			}
			const int32 Top = Rows.Num() - 1;
			// Rows laid down from the ceiling: the top, and the frieze under it.
			const int32 Crown = Rows.Num() >= MinFrieze ? 2 : 1;
			const int32 FromTop = Height - 1 - Above;
			if (Height > Rows.Num() && FromTop >= 0 && FromTop < Crown)
			{
				return Rows[Top - FromTop];
			}
			const int32 Body = FMath::Max(Top - Crown, 0);
			return Above == Height - 1 && Top > Body ? Rows[Top] : Rows[FMath::Clamp(Above, 0, Height > Rows.Num() ? Body : Top)];
		}
	};

	/** What dresses an enclosure besides the grid's own art: the wallpapers and the ceilings. */
	struct FEnclosureArt
	{
		const FLBALibrary* Library = nullptr;
		int32 ForbiddenBrick = 0;
		/** Per space: the wallpaper of its back walls facing X (0) and Z (1). */
		TArray<TStaticArray<FWallpaper, 2>> Wallpapers;
		/** The building's (the grid's) wallpaper, facing X and Z: the camera-side walls'. */
		TStaticArray<FWallpaper, 2> DefaultWallpapers;
		/** Per space: its wallpaper is a finish of its own, on its camera-side walls too. */
		TArray<bool> OwnFinish;
		/** Per space: the color of its walls where no wallpaper has art (its floor's). */
		TArray<FVector4f> SpaceColors;
		FVector4f CeilingColor = FVector4f(0.3f, 0.3f, 0.3f, 0.5f);
		UMaterialInterface* CeilingMaterial = nullptr;
		bool bCeiling = true;
	};

	/** Art for a face of the enclosure: a brick's art face (its atlas cell and face index, +X or +Z), or none (flat). */
	struct FFaceArt
	{
		const FIntPoint* Cell = nullptr;
		int32 Face = 2;
	};

	/** The brick of a layout row at a wall column (the layout tiled along the wall from the grid's origin), or 0. */
	int32 LayoutBrickAt(const FLBALibrary& Library, int32 ForbiddenBrick, const FIntPoint& Row, const FIntPoint& Column)
	{
		if (!Library.Layouts.IsValidIndex(Row.X))
		{
			return 0;
		}
		const FLBALayout& Layout = Library.Layouts[Row.X];
		const int32 Index = (FMath::Abs(Column.Y) % Layout.SizeZ) * Layout.SizeX * Layout.SizeY + FMath::Clamp(Row.Y, 0, Layout.SizeY - 1) * Layout.SizeX
			+ FMath::Abs(Column.X) % Layout.SizeX;
		const int32 Brick = Layout.Blocks.IsValidIndex(Index) ? int32(Layout.Blocks[Index].Brick) : 0;
		return Brick == ForbiddenBrick ? 0 : Brick;
	}

	/** A brick's art for a wall face along an axis (0: facing X, 1: facing Z), opaque enough to repeat: that side, else the other. */
	FFaceArt WallFaceArt(const FLBAIsoImporter::FAtlas& Atlas, int32 Brick, int32 Axis)
	{
		FFaceArt Art;
		const TStaticArray<float, 3>* Coverage = Brick ? Atlas.FaceCoverage.Find(Brick) : nullptr;
		if (!Coverage)
		{
			return Art;
		}
		for (const int32 Slot : { Axis == 0 ? 1 : 2, Axis == 0 ? 2 : 1 })
		{
			if ((*Coverage)[Slot] >= OpaqueCoverage)
			{
				Art.Cell = Atlas.Cells.Find(Brick);
				Art.Face = Slot == 1 ? 2 : 4;
				return Art;
			}
		}
		return Art;
	}

	/** Atlas UV of a point of a wall art face (A along it, B up it, 0..1), a little inside the face (its edge pixels repeated show as seams). */
	FVector2f ArtUV(const FLBAIsoImporter::FAtlas& Atlas, const FFaceArt& Art, float A, float B, float Inset = 0.1f)
	{
		const FBrickFace& Face = GetBrickFaces()[Art.Face];
		const FVector2f Center = (Face.Sprite[0] + Face.Sprite[1] + Face.Sprite[2] + Face.Sprite[3]) * 0.25f;
		// The +X and +Z faces' corners: 0 (A 0, B 1), 1 (A 1, B 1), 2 (A 1, B 0), 3 (A 0, B 0).
		const FVector2f Sprite = FMath::Lerp(FMath::Lerp(FMath::Lerp(Face.Sprite[3], Face.Sprite[0], B), FMath::Lerp(Face.Sprite[2], Face.Sprite[1], B), A), Center, Inset);
		const FIntPoint& Offset = FaceOffsets[ColorSlotOf(FrontFaceOf(Art.Face))];
		return FVector2f((Art.Cell->X + Offset.X + Sprite.X) / Atlas.Width, (Art.Cell->Y + Offset.Y + Sprite.Y) / Atlas.Height);
	}

	/**
	 * A finish for surfaces the original never drew (a roof, the rock over a rim): the wall the original crowned what bounds
	 * them with, as it stacks it. The most voted crown row (layout, row), and Depth rows of its layout from it down (one by
	 * default: the row under a crown is often a dark frieze, which repeated stripes a roof); along a wall, the layout's own
	 * columns.
	 */
	struct FCrownFinish
	{
		/** Rows from the top down. */
		TArray<FIntPoint> Rows;

		/** The row of the wall at a height (repeating from the top down). */
		FIntPoint RowAt(int32 FromTop) const
		{
			return Rows.Num() > 0 ? Rows[FromTop % Rows.Num()] : FIntPoint(INDEX_NONE, 0);
		}

		/** The most voted row, and Depth rows of its layout from it down (fewer where the layout ends). */
		static FCrownFinish FromVotes(const TMap<FIntPoint, int32>& Votes, int32 Depth = 1)
		{
			FIntPoint Best(INDEX_NONE, 0);
			int32 BestCount = 0;
			for (const TPair<FIntPoint, int32>& Pair : Votes)
			{
				if (Pair.Value > BestCount || (Pair.Value == BestCount && (Pair.Key.X < Best.X || (Pair.Key.X == Best.X && Pair.Key.Y > Best.Y))))
				{
					Best = Pair.Key;
					BestCount = Pair.Value;
				}
			}
			FCrownFinish Finish;
			for (int32 Row = Best.Y; Best.X != INDEX_NONE && Row >= 0 && Finish.Rows.Num() < Depth; --Row)
			{
				Finish.Rows.Add(FIntPoint(Best.X, Row));
			}
			return Finish;
		}
	};

	/** Bricks across (and up) a finish's texture (FinishTile): it repeats every this many cells. */
	constexpr int32 FinishTileBricks = 16;

	/**
	 * A finish as a texture that tiles: FinishTileBricks x FinishTileBricks cells of wall, each a brick's wall face (its
	 * sheared diamond side unsheared to SpriteW/2 x 15 pixels: one cell wide, one high), the finish's rows stacked from the top. A
	 * curved surface textured by its position then keeps the art's size everywhere, however steep (atlas art mapped per
	 * cell stretches up a vault's spring). Only bricks with an opaque, not shadowed, wall face; else the mean.
	 * Amount pulls it toward a color (a roof toward the library's ceiling color: lighter than the walls, in their hue).
	 */
	TArray<FColor> RenderFinishTile(const FLBAIsoImporter::FAtlas& Atlas, const FLBALibrary& Library, int32 ForbiddenBrick, const FCrownFinish& Finish,
		const FColor& Toward, float Amount, int32& OutWidth, int32& OutHeight)
	{
		constexpr int32 FaceW = SpriteW / 2, FaceH = 15;
		OutWidth = FaceW * FinishTileBricks;
		OutHeight = FaceH * FinishTileBricks;
		TArray<FColor> Pixels;
		Pixels.Init(FColor(70, 70, 70), OutWidth * OutHeight);
		FLinearColor Sum = FLinearColor::Black;
		int32 Count = 0;
		TArray<bool> Done;
		Done.Init(false, FinishTileBricks * FinishTileBricks);
		// Each row's usable bricks along its layout: an opaque wall face at least half as bright as the row's brightest
		// (the shadowed sides a rock layout draws read as black stripes once repeated).
		TMap<FIntPoint, TArray<FFaceArt>> Usable;
		for (const FIntPoint& Row : Finish.Rows)
		{
			TArray<TPair<FFaceArt, float>> Bricks;
			float Brightest = 0.f;
			const int32 Span = Library.Layouts.IsValidIndex(Row.X) ? FMath::Max(Library.Layouts[Row.X].SizeX, Library.Layouts[Row.X].SizeZ) : 0;
			for (int32 K = 0; K < Span; ++K)
			{
				const int32 Brick = LayoutBrickAt(Library, ForbiddenBrick, Row, FIntPoint(K, K));
				const FFaceArt Art = WallFaceArt(Atlas, Brick, 0);
				const TStaticArray<FColor, 3>* Colors = Atlas.FaceColors.Find(Brick);
				if (Art.Cell && Colors)
				{
					const float Luminance = FLinearColor((*Colors)[ColorSlotOf(FrontFaceOf(Art.Face))]).GetLuminance();
					Bricks.Add(TPair<FFaceArt, float>(Art, Luminance));
					Brightest = FMath::Max(Brightest, Luminance);
				}
			}
			TArray<FFaceArt>& Kept = Usable.Add(Row);
			for (const TPair<FFaceArt, float>& Brick : Bricks)
			{
				if (Brick.Value >= Brightest * 0.5f)
				{
					Kept.Add(Brick.Key);
				}
			}
		}
		for (int32 J = 0; J < FinishTileBricks; ++J)
		{
			for (int32 I = 0; I < FinishTileBricks; ++I)
			{
				// The row at this height, else any row with usable bricks.
				const TArray<FFaceArt>* Bricks = Usable.Find(Finish.RowAt(J));
				for (const TPair<FIntPoint, TArray<FFaceArt>>& Pair : Usable)
				{
					Bricks = Bricks && Bricks->Num() > 0 ? Bricks : &Pair.Value;
				}
				if (!Bricks || Bricks->Num() == 0)
				{
					continue;
				}
				const FFaceArt Art = (*Bricks)[I % Bricks->Num()];
				Done[J * FinishTileBricks + I] = true;
				for (int32 V = 0; V < FaceH; ++V)
				{
					for (int32 U = 0; U < FaceW; ++U)
					{
						const FVector2f UV = ArtUV(Atlas, Art, (U + 0.5f) / FaceW, 1.f - (V + 0.5f) / FaceH, 0.f);
						const int32 SX = FMath::Clamp(FMath::FloorToInt(UV.X * Atlas.Width), 0, Atlas.Width - 1);
						const int32 SY = FMath::Clamp(FMath::FloorToInt(UV.Y * Atlas.Height), 0, Atlas.Height - 1);
						FColor Color = Atlas.Pixels.IsValidIndex(SY * Atlas.Width + SX) ? Atlas.Pixels[SY * Atlas.Width + SX] : FColor::Black;
						Color = FColor(FMath::Lerp(Color.R, Toward.R, Amount), FMath::Lerp(Color.G, Toward.G, Amount), FMath::Lerp(Color.B, Toward.B, Amount), 255);
						Pixels[(J * FaceH + V) * OutWidth + I * FaceW + U] = Color;
						Sum += FLinearColor(Color);
						++Count;
					}
				}
			}
		}
		const FColor Mean = Count > 0 ? (Sum / float(Count)).ToFColor(false) : FColor(70, 70, 70);
		for (int32 J = 0; J < FinishTileBricks; ++J)
		{
			for (int32 I = 0; I < FinishTileBricks; ++I)
			{
				for (int32 V = 0; V < FaceH && !Done[J * FinishTileBricks + I]; ++V)
				{
					for (int32 U = 0; U < FaceW; ++U)
					{
						Pixels[(J * FaceH + V) * OutWidth + I * FaceW + U] = Mean;
					}
				}
			}
		}
		return Pixels;
	}

	/** UV of a point (cells: X, Z columns, Y cells) in a finish's tile, for a surface facing along Normal (LBA axes). */
	FVector2f FinishUV(const FVector3f& At, const FVector3f& Normal)
	{
		const float Scale = 1.f / FinishTileBricks;
		if (FMath::Abs(Normal.Y) > 0.7f)
		{
			// Seen from below: two cells of wall per column (a column is twice as wide as a cell is high).
			return FVector2f(At.X, At.Z * 2.f) * Scale;
		}
		return FVector2f(FMath::Abs(Normal.X) > FMath::Abs(Normal.Z) ? At.Z : At.X, -At.Y) * Scale;
	}

	/**
	 * A height field as a mesh: Height(X, Z) in cells over a footprint of columns (the grid and Margin columns round it),
	 * sampled RoofSteps times per column, looking down (a roof, into the air under it) or up (a canyon's floor), textured
	 * with a finish's tile by position (FinishUV: from below or above where it is shallow, from the side where it is
	 * steep). Normals follow the curve.
	 */
	constexpr int32 RoofSteps = 2;
	void EmitHeightField(TFunctionRef<bool(const FIntPoint&)> Contains, int32 Margin, TFunctionRef<float(float, float)> Height, bool bUp,
		FLBAMeshBuilder& Builder, int32 Section, const FVector4f& Color, float Scale, TFunction<float(const FVector3f&)> Light = nullptr)
	{
		const int32 Size = FLBARoomPlan::SizeXZ + 2 * Margin;
		const int32 N = Size * RoofSteps + 1;
		TArray<float> Heights;
		Heights.Init(TNumericLimits<float>::Lowest(), N * N);
		auto ToCells = [Margin](int32 I) { return float(I) / RoofSteps - Margin; };
		auto HeightAt = [&](int32 I, int32 J)
		{
			I = FMath::Clamp(I, 0, N - 1);
			J = FMath::Clamp(J, 0, N - 1);
			float& Value = Heights[J * N + I];
			if (Value == TNumericLimits<float>::Lowest())
			{
				Value = Height(ToCells(I), ToCells(J));
			}
			return Value;
		};
		auto Cells = [&](int32 I, int32 J) { return FVector3f(ToCells(I), HeightAt(I, J), ToCells(J)); };
		auto Position = [&](int32 I, int32 J) { const FVector3f C = Cells(I, J); return CornerPos(C.X, C.Y, C.Z, Scale); };
		const FVector3f Facing = FVector3f(LBA::DirToUnreal(0, bUp ? 1 : -1, 0));
		auto NormalAt = [&](int32 I, int32 J)
		{
			FVector3f Normal = ((Position(I + 1, J) - Position(I - 1, J)) ^ (Position(I, J + 1) - Position(I, J - 1))).GetSafeNormal();
			return (Normal | Facing) < 0.f ? -Normal : Normal;
		};
		for (int32 Z = -Margin; Z < FLBARoomPlan::SizeXZ + Margin; ++Z)
		{
			for (int32 X = -Margin; X < FLBARoomPlan::SizeXZ + Margin; ++X)
			{
				if (!Contains(FIntPoint(X, Z)))
				{
					continue;
				}
				for (int32 SI = 0; SI < RoofSteps; ++SI)
				{
					for (int32 SJ = 0; SJ < RoofSteps; ++SJ)
					{
						static const FIntPoint Corners[4] = { FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(1, 1), FIntPoint(0, 1) };
						const int32 I0 = (X + Margin) * RoofSteps + SI, J0 = (Z + Margin) * RoofSteps + SJ;
						// The quad's slope in LBA axes (cells; a column is two cells wide), for its projection.
						const FVector3f C00 = Cells(I0, J0), C11 = Cells(I0 + 1, J0 + 1), C10 = Cells(I0 + 1, J0), C01 = Cells(I0, J0 + 1);
						const FVector3f Slope = FVector3f(-(C10.Y + C11.Y - C00.Y - C01.Y) * 0.5f, 2.f / RoofSteps, -(C01.Y + C11.Y - C00.Y - C10.Y) * 0.5f).GetSafeNormal();
						FLBAVertex V[4];
						for (int32 K = 0; K < 4; ++K)
						{
							const int32 I = I0 + Corners[K].X, J = J0 + Corners[K].Y;
							V[K].Position = Position(I, J);
							V[K].Normal = NormalAt(I, J);
							V[K].Color = Color;
							V[K].Color.W *= Light ? Light(Cells(I, J)) : 1.f;
							V[K].UV = FinishUV(Cells(I, J), Slope);
						}
						Builder.AddTriangleFacing(Section, V[0], V[1], V[2], Facing);
						Builder.AddTriangleFacing(Section, V[0], V[2], V[3], Facing);
					}
				}
			}
		}
	}

	/** A vault's roof (FLBAVault) looking down into the air under it. */
	void EmitRoof(const FLBAVault& Vault, TFunctionRef<float(float, float)> Height, FLBAMeshBuilder& Builder, int32 Section, const FVector4f& Color, float Scale)
	{
		EmitHeightField([&Vault](const FIntPoint& Column) { return Vault.Contains(Column); }, Vault.GetMargin(), Height, false, Builder, Section, Color, Scale);
	}

	/**
	 * The walls of a hollow (FLBAHollow), looking into the air under its roof, textured with the finish's tile:
	 * - on each side of a column of the hollow that is not under the roof, the rock (or masonry) rises from what the
	 *   original drew there (a rim, a rampart, a pillar; nothing: from the bottom of the grid, closing the void past a
	 *   floor's edge) up to the roof's edge; past an exit (beside a scene-change zone) they are dark, as the original shows it;
	 * - round a canyon, the rock rises from its floor to the roof (around what the original drew there), and under a
	 *   ledge from its floor up to the foot of the ledge.
	 */
	void EmitHollowWalls(const FLBAHollow& Hollow, TFunctionRef<float(float, float)> Height, TConstArrayView<FLBACellBox> Exits,
		FLBAMeshBuilder& Builder, int32 Section, int32 DarkSection, const FVector4f& Color, float Scale)
	{
		constexpr int32 Size = FLBARoomPlan::SizeXZ;
		constexpr int32 Margin = FLBAHollow::Margin;
		auto NearExit = [&Exits](const FIntPoint& Column)
		{
			return Exits.ContainsByPredicate([&Column](const FLBACellBox& Box)
			{
				return Column.X >= Box.Min.X - 1 && Column.X <= Box.Max.X + 1 && Column.Y >= Box.Min.Z - 1 && Column.Y <= Box.Max.Z + 1;
			});
		};
		const FVector4f Dark = LBABodyConverter::ToVertexColor(FColor(6, 6, 8), 0.5f);
		auto Floor = [&Hollow](float X, float Z) { return Hollow.GetCanyonFloor(X, Z); };
		for (int32 Z = -Margin; Z < Size + Margin; ++Z)
		{
			for (int32 X = -Margin; X < Size + Margin; ++X)
			{
				const FIntPoint Column(X, Z);
				if (!Hollow.IsUnderRoof(Column))
				{
					continue;
				}
				const bool bCanyon = Hollow.IsInCanyon(Column);
				const bool bExit = !bCanyon && NearExit(Column);
				for (int32 Side = 0; Side < 4; ++Side)
				{
					const FIntPoint Next = Column + FLBARoomPlan::Sides[Side];
					// The edge between the two columns, from its start along +Z (sides -X, +X) or +X (sides -Z, +Z).
					const bool bAlongZ = Side < 2;
					const float EdgeX = X + (Side == 1 ? 1.f : 0.f), EdgeZ = Z + (Side == 3 ? 1.f : 0.f);
					const FVector3f Normal(-FLBARoomPlan::Sides[Side].X, 0.f, -FLBARoomPlan::Sides[Side].Y);
					const FVector3f Inward = FVector3f(LBA::DirToUnreal(Normal.X, 0, Normal.Z));
					// A strip of wall along the edge between two heights.
					auto Strip = [&](TFunctionRef<float(float, float)> Bottom, TFunctionRef<float(float, float)> Top)
					{
						for (int32 Step = 0; Step < RoofSteps; ++Step)
						{
							const float A0 = float(Step) / RoofSteps, A1 = float(Step + 1) / RoofSteps;
							const FVector2f P0 = bAlongZ ? FVector2f(EdgeX, EdgeZ + A0) : FVector2f(EdgeX + A0, EdgeZ);
							const FVector2f P1 = bAlongZ ? FVector2f(EdgeX, EdgeZ + A1) : FVector2f(EdgeX + A1, EdgeZ);
							const float Bottom0 = Bottom(P0.X, P0.Y), Bottom1 = Bottom(P1.X, P1.Y);
							const float Top0 = Top(P0.X, P0.Y), Top1 = Top(P1.X, P1.Y);
							if (Top0 <= Bottom0 && Top1 <= Bottom1)
							{
								continue;
							}
							const FVector3f Corners[4] = { FVector3f(P0.X, FMath::Max(Top0, Bottom0), P0.Y), FVector3f(P1.X, FMath::Max(Top1, Bottom1), P1.Y),
								FVector3f(P1.X, Bottom1, P1.Y), FVector3f(P0.X, Bottom0, P0.Y) };
							FLBAVertex V[4];
							for (int32 K = 0; K < 4; ++K)
							{
								V[K].Position = CornerPos(Corners[K].X, Corners[K].Y, Corners[K].Z, Scale);
								V[K].Normal = Inward;
								V[K].Color = bExit ? Dark : Color;
								V[K].Color.W *= bCanyon ? Hollow.GetCanyonLight(Corners[K].X, Corners[K].Y, Corners[K].Z) : 1.f;
								V[K].UV = FinishUV(Corners[K], Normal);
							}
							const int32 Into = bExit ? DarkSection : Section;
							Builder.AddTriangleFacing(Into, V[0], V[1], V[2], Inward);
							Builder.AddTriangleFacing(Into, V[0], V[2], V[3], Inward);
						}
					};
					auto Level = [](float Value) { return [Value](float, float) { return Value; }; };
					if (Hollow.IsUnderRoof(Next))
					{
						// Under a ledge: from the canyon's floor up to what the original drew under its edge.
						const int32 Foot = Hollow.GetDrawnBottom(Next);
						if (bCanyon && !Hollow.IsInCanyon(Next) && Foot != INDEX_NONE)
						{
							Strip(Floor, Level(Foot));
						}
						continue;
					}
					const int32 Drawn = Hollow.GetDrawnTop(Next);
					if (bCanyon)
					{
						// Round the canyon: from its floor to the roof, around what the original drew there.
						if (Drawn == INDEX_NONE)
						{
							Strip(Floor, Height);
						}
						else
						{
							Strip(Floor, Level(Hollow.GetDrawnBottom(Next)));
							Strip(Level(Drawn + 1), Height);
						}
						continue;
					}
					// From what is drawn next to it (and over this column's own floor: its side shows below).
					const int32 Bottom = FMath::Max(Drawn != INDEX_NONE ? Drawn + 1 : 0, Hollow.GetFloor(Column) + 1);
					Strip(Level(Bottom), Height);
				}
			}
		}
	}

	/** A brick with art (some libraries stack bricks without any to block the camera-side walls). */
	bool HasBrickArt(const FLBAIsoImporter::FAtlas* Atlas, int32 Brick)
	{
		const TStaticArray<float, 3>* Coverage = Atlas ? Atlas->FaceCoverage.Find(Brick) : nullptr;
		return !Coverage || FMath::Max3((*Coverage)[0], (*Coverage)[1], (*Coverage)[2]) > 0.f;
	}

	/** Highest drawn cell of a column in [From, Below), or INDEX_NONE. */
	int32 TopDrawn(TFunctionRef<bool(int32, int32, int32)> IsDrawn, const FIntPoint& Column, int32 From, int32 Below)
	{
		for (int32 Y = FMath::Min(Below, LBA::GridSizeY) - 1; Y >= FMath::Max(From, 0); --Y)
		{
			if (IsDrawn(Column.X, Y, Column.Y))
			{
				return Y;
			}
		}
		return INDEX_NONE;
	}

	/** The cells of a grid as the room plan reads them (FLBARoomPlan). */
	FLBAGridCells MakePlanCells(const FCellQuery& Query, const FLBAGrid& Grid, const FLBALibrary& Library, const FLBAIsoImporter::FAtlas* Atlas,
		const TSet<int32>& FrameLayouts)
	{
		FLBAGridCells Cells;
		Cells.Shape = [&Query](int32 X, int32 Y, int32 Z) { return Query.ShapeAt(X, Y, Z); };
		Cells.Brick = [&Query](int32 X, int32 Y, int32 Z) { return Query.BrickAt(X, Y, Z); };
		Cells.Layout = [&Grid, &Library](int32 X, int32 Y, int32 Z)
		{
			const int32 Layout = Grid.Get(X, Y, Z).Layout;
			return Library.Layouts.IsValidIndex(Layout) ? Layout : INDEX_NONE;
		};
		Cells.LayoutBase = [&Grid, &Library](int32 X, int32 Y, int32 Z)
		{
			const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
			return Y - Library.Layouts[Cell.Layout].GetBlockCoords(Cell.Block).Y;
		};
		Cells.HasArt = [Atlas](int32 Brick) { return HasBrickArt(Atlas, Brick); };
		Cells.IsFrame = [&Grid, &FrameLayouts](int32 X, int32 Y, int32 Z) { return FrameLayouts.Contains(int32(Grid.Get(X, Y, Z).Layout)); };
		return Cells;
	}

	/**
	 * A column of air the enclosure closes: a room's, a connector's or a flight's column (FLBARoomPlan), from the cell
	 * over its floor (Floor + 1) to the first cell above its air (Top). Saved with the plan.
	 */
	using FAirColumn = FLBAGridPlanAir;

	/**
	 * The air of a grid's plan, column by column:
	 * - Rooms' columns, up to their space's ceiling, or the underside of a higher storey's floor over them (a gallery).
	 * - Connectors' columns (not pockets), up to their ceiling or the first solid cell over them (a lintel, a gallery);
	 *   in a gap of a wall's line (a doorway), at most DoorHeight cells over the floor (where the camera-side walls were
	 *   cut down, the original has no lintel left). An exit's columns past its doorway (the passage out, closed by the
	 *   original's invisible plug) are not enclosed: OutBeyondExits.
	 * - Flights' columns, from their step.
	 */
	TArray<FAirColumn> BuildAir(const FLBARoomPlan& Plan, int32 DoorHeight, TSet<FIntPoint>& OutBeyondExits)
	{
		// First cell above the air of a column from Floor: Ceiling, or the first solid cell over what stands on the floor
		// when bAnyCap, or when it is a higher storey's floor.
		auto AirTop = [&Plan](const FIntPoint& Column, int32 Floor, int32 Ceiling, bool bAnyCap)
		{
			int32 Y = Floor + 1;
			while (Y < Ceiling && Plan.IsSolid(Column.X, Y, Column.Y))
			{
				++Y;
			}
			for (; Y < Ceiling; ++Y)
			{
				if (Plan.IsSolid(Column.X, Y, Column.Y))
				{
					const bool bStorey = Plan.GetFloorLevels(Column).ContainsByPredicate([Y](int32 Level) { return Level >= Y; });
					return bAnyCap || bStorey ? Y : Ceiling;
				}
			}
			return Ceiling;
		};
		TArray<FAirColumn> Air;
		const TArray<FLBAPlanRoom>& Rooms = Plan.GetRooms();
		for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
		{
			const FLBAPlanRoom& Room = Rooms[RoomIndex];
			for (int32 X = Room.Min.X; X <= Room.Max.X; ++X)
			{
				for (int32 Z = Room.Min.Y; Z <= Room.Max.Y; ++Z)
				{
					FAirColumn& Column = Air.AddDefaulted_GetRef();
					Column.Column = FIntPoint(X, Z);
					Column.Level = Column.Floor = Room.Level;
					Column.Top = AirTop(Column.Column, Room.Level, Room.Ceiling, false);
					Column.Space = Room.Space;
					Column.Room = RoomIndex;
				}
			}
		}
		const TArray<FLBAPlanConnector>& Connectors = Plan.GetConnectors();
		for (int32 ConnectorIndex = 0; ConnectorIndex < Connectors.Num(); ++ConnectorIndex)
		{
			const FLBAPlanConnector& Connector = Connectors[ConnectorIndex];
			if (Connector.Kind == ELBAConnector::Pocket)
			{
				continue;
			}
			// Its walls take the wallpaper of its highest room (stairs end in it).
			int32 Space = INDEX_NONE, Highest = TNumericLimits<int32>::Lowest();
			for (const int32 Room : Connector.Rooms)
			{
				if (Rooms[Room].Ceiling > Highest)
				{
					Highest = Rooms[Room].Ceiling;
					Space = Rooms[Room].Space;
				}
			}
			for (const FIntVector& Piece : Connector.Columns)
			{
				const FIntPoint Column(Piece.X, Piece.Z);
				FAirColumn Cell;
				Cell.Column = Column;
				Cell.Space = Space;
				Cell.Connector = ConnectorIndex;
				if (Piece.Y != INDEX_NONE)
				{
					Cell.Level = Cell.Floor = Piece.Y;
				}
				else
				{
					// A flight's column: from its step, judged at the highest level under it.
					Cell.Floor = 0;
					for (const FLBAPlanFlight& Flight : Plan.GetFlights())
					{
						for (const FIntVector& Step : Flight.Cells)
						{
							Cell.Floor = Step.X == Column.X && Step.Z == Column.Y ? FMath::Max(Cell.Floor, Step.Y) : Cell.Floor;
						}
					}
					Cell.Level = Connector.Levels[0];
					for (const int32 Level : Connector.Levels)
					{
						Cell.Level = Level <= Cell.Floor ? Level : Cell.Level;
					}
				}
				const FLBAPlanLevel* Level = Plan.FindLevel(Cell.Level);
				const bool bDoorway = Piece.Y != INDEX_NONE && Level && Level->Gaps.Contains(Column);
				if (Connector.Kind == ELBAConnector::Exit && !bDoorway
					&& !Connector.Rooms.ContainsByPredicate([&](int32 Room)
					{
						for (const FIntPoint& Side : FLBARoomPlan::Sides)
						{
							if (Rooms[Room].Contains(Column + Side))
							{
								return true;
							}
						}
						return false;
					}))
				{
					OutBeyondExits.Add(Column);
					continue;
				}
				Cell.Top = AirTop(Column, Cell.Floor, Connector.Ceiling, true);
				if (bDoorway)
				{
					Cell.Top = FMath::Min(Cell.Top, Cell.Floor + 1 + DoorHeight);
				}
				Air.Add(Cell);
			}
		}
		return Air;
	}

	/**
	 * The enclosure of a grid's plan (BuildAir), as faces looking into its air (a camera outside sees in). Each air cell,
	 * on each of its four sides, closes on what is next to it unless that is air too:
	 * - a wall of its level: a wall face (raising the walls the original cut down for the camera),
	 * - air that stops lower (a doorway's header, a lower ceiling next door): a step face,
	 * - a solid cell (the side of a gallery's floor, of a lintel), or the air above what rises from its floor where no
	 *   one's air is (a raised floor no room stands on, a frame, a nook behind furniture): a wall face,
	 * - nothing from its floor up (a drop, the void): open, nothing built;
	 * - past an exit's doorway, away from its room, or around it where nothing is: closed in the dark the original shows
	 *   there;
	 * - a slope (a flight's step): never closed, it is walked on; over a slope, the walls beside it close from the step's
	 *   own cell.
	 * On the sides the original drew (-X, -Z of the air: the cells' +X, +Z faces) its opaque art stays (but the top row of
	 * the wallpaper, its cornice, where it now sits mid-wall); art that is not opaque is backed by the wallpaper and drawn
	 * again in front of it. Back walls take their space's wallpaper; the camera-side walls, whose inside the original
	 * never drew, the building's default one. Over each air column, a ceiling (where the space has one: always under a
	 * lintel or a gallery), and over what its walls close on (raised walls, headers).
	 * Wall faces all lie a little in front of the walls' planes (WallInset, over the remake's models set in the walls), at
	 * one depth, so no crack opens between them; the grid's faces they cover go to OutCoveredFaces (cell, face index), off
	 * the grid's mesh, and the cells they paint over to OutCovered (the frames there come off).
	 */
	void EmitEnclosure(const FLBARoomPlan& Plan, TConstArrayView<FAirColumn> Air, const TSet<FIntPoint>& BeyondExits,
		TFunctionRef<bool(int32, int32, int32)> IsDrawn, TFunctionRef<bool(int32, int32, int32)> IsSlope, TFunctionRef<FIntPoint(int32, int32, int32)> LayoutRowAt,
		const FLBAIsoImporter::FAtlas& Atlas,
		const FEnclosureArt& Art, const FLBAVaultShape& VaultShape, TFunctionRef<UMaterialInterface*(const FCrownFinish&, bool)> MakeFinishMaterial, FLBAMeshBuilder& Builder, FLBAMeshBuilder& CeilingBuilder, float Scale,
		TSet<FIntVector>& OutCovered, TSet<FIntVector4>& OutCoveredFaces)
	{
		const FBrickFace* Faces = GetBrickFaces();
		const int32 BrickSection = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas.Material);
		int32 FillSection = INDEX_NONE;
		const int32 CeilingSection = CeilingBuilder.GetSection(TEXT("Ceiling"), Art.CeilingMaterial ? Art.CeilingMaterial : Atlas.FillMaterial);
		// Wall faces sit a little in front of the walls' planes; art drawn again over the wallpaper behind it, in front of them.
		constexpr float WallInset = -0.5f;
		constexpr float DecalInset = -1.f;
		// The face of the neighbor toward the air on each side (a cell at the air's -X shows its +X face, and so on).
		static const int32 FaceOfSide[4] = { 2, 3, 4, 5 };
		// -LBA2EnclosureDebug: flat colors by part (back walls red, camera-side walls orange, steps and headers yellow,
		// sides of solid cells purple, past exits gray, ceilings green).
		enum class EPart : uint8 { Wall, CameraSide, Step, Solid, Exit, Ceiling };
		static const FColor PartColors[] = { FColor(220, 40, 40), FColor(240, 130, 30), FColor(240, 210, 40), FColor(160, 60, 220),
			FColor(140, 140, 140), FColor(40, 200, 60) };
		const bool bDebugParts = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebug"));
		// -LBA2EnclosureDebugArt: walls by their art: wallpaper cyan, its foot green, flat color magenta.
		static const FColor ArtColors[] = { FColor(40, 200, 220), FColor(40, 200, 60), FColor(255, 0, 255) };
		const bool bDebugArt = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebugArt"));
		int32 Stats[3] = {};

		// Vaulted spaces: the columns under their vault.
		TMap<int32, TBitArray<>> VaultFootprints;
		// Which air column holds each cell.
		TMap<FIntVector, int32> AirAt;
		TMap<FIntPoint, TArray<int32, TInlineAllocator<2>>> AirOfColumn;
		for (int32 Index = 0; Index < Air.Num(); ++Index)
		{
			AirOfColumn.FindOrAdd(Air[Index].Column).Add(Index);
			for (int32 Y = Air[Index].Floor + 1; Y < Air[Index].Top; ++Y)
			{
				AirAt.FindOrAdd(FIntVector(Air[Index].Column.X, Y, Air[Index].Column.Y), Index);
			}
		}

		// The brick of a layout row at a wall column (the layout tiled along the wall from the grid's origin).
		auto LayoutBrick = [&Art](int32 LayoutIndex, int32 Y, const FIntPoint& Column)
		{
			if (!Art.Library || !Art.Library->Layouts.IsValidIndex(LayoutIndex))
			{
				return 0;
			}
			const FLBALayout& Layout = Art.Library->Layouts[LayoutIndex];
			const int32 Index = (Column.Y % Layout.SizeZ) * Layout.SizeX * Layout.SizeY + FMath::Clamp(Y, 0, Layout.SizeY - 1) * Layout.SizeX + Column.X % Layout.SizeX;
			const int32 Brick = Layout.Blocks.IsValidIndex(Index) ? int32(Layout.Blocks[Index].Brick) : 0;
			return Brick == Art.ForbiddenBrick ? 0 : Brick;
		};
		// A brick's art for a wall face, opaque enough to repeat: the side its layout faces (a wall panel only has art on
		// the side the iso camera saw), else the face's own side, else the other.
		auto WallArt = [&Atlas](int32 Brick, int32 FaceIndex, int32 FacingSlot, int32& OutArtFace) -> const FIntPoint*
		{
			const TStaticArray<float, 3>* Coverage = Brick ? Atlas.FaceCoverage.Find(Brick) : nullptr;
			if (!Coverage)
			{
				return nullptr;
			}
			const int32 Slot = ColorSlotOf(FrontFaceOf(FaceIndex));
			const int32 Other = 3 - Slot;
			if (FacingSlot != 0 && (*Coverage)[FacingSlot] >= OpaqueCoverage)
			{
				OutArtFace = FacingSlot == 1 ? 2 : 4;
			}
			else if ((*Coverage)[Slot] >= OpaqueCoverage)
			{
				OutArtFace = FrontFaceOf(FaceIndex);
			}
			else if ((*Coverage)[Other] >= OpaqueCoverage)
			{
				OutArtFace = Other == 1 ? 2 : 4;
			}
			else
			{
				return nullptr;
			}
			return Atlas.Cells.Find(Brick);
		};
		auto FacingSlotOf = [&Art](int32 LayoutIndex)
		{
			if (!Art.Library || !Art.Library->Layouts.IsValidIndex(LayoutIndex))
			{
				return 0;
			}
			const FLBALayout& Layout = Art.Library->Layouts[LayoutIndex];
			return Layout.SizeX == 1 && Layout.SizeZ > 1 ? 1 : Layout.SizeZ == 1 && Layout.SizeX > 1 ? 2 : 0;
		};

		auto EmitQuad = [&](const FIntVector& Cell, int32 FaceIndex, const FIntPoint* SourceCell, int32 ArtFaceIndex, const FVector4f& Color,
			float Inset, bool bCeilingMesh, float SampleInset = 0.1f)
		{
			const FBrickFace& Face = Faces[FaceIndex];
			const FBrickFace& ArtFace = Faces[ArtFaceIndex];
			const FVector2f ArtCenter = (ArtFace.Sprite[0] + ArtFace.Sprite[1] + ArtFace.Sprite[2] + ArtFace.Sprite[3]) * 0.25f;
			const FVector3f Normal = FVector3f(LBA::DirToUnreal(Face.Dir.X, Face.Dir.Y, Face.Dir.Z));
			// Wall faces stand in front of their walls by -Inset: they overhang their sides as much, so two of them meeting
			// at a wall's corner leave no slit (the overhang hides behind the other face in a room's corners).
			const FIntVector Along = Face.Dir.Y != 0 ? FIntVector::ZeroValue : Face.Dir.X != 0 ? FIntVector(0, 0, 1) : FIntVector(1, 0, 0);
			const FVector3f AlongDir = FVector3f(LBA::DirToUnreal(Along.X, Along.Y, Along.Z));
			FLBAVertex V[4];
			for (int32 K = 0; K < 4; ++K)
			{
				const FVector3f Corner(Cell.X + Face.Corners[K].X, Cell.Y + Face.Corners[K].Y, Cell.Z + Face.Corners[K].Z);
				const float Side = (Along.X ? Face.Corners[K].X : Face.Corners[K].Z) > 0.5f ? 1.f : -1.f;
				V[K].Position = CornerPos(Corner.X, Corner.Y, Corner.Z, Scale) - Normal * Inset + (bCeilingMesh ? FVector3f::ZeroVector : AlongDir * (Side * FMath::Abs(Inset)));
				V[K].Normal = Normal;
				V[K].Color = SourceCell ? FVector4f(0.5f, 0.5f, 0.5f, 0.5f) : Color;
				if (SourceCell)
				{
					// Sampled a little inside the face: its edge pixels repeated up a wall show as seams.
					const FVector2f Sprite = FMath::Lerp(ArtFace.Sprite[K], ArtCenter, SampleInset);
					const FIntPoint& Offset = FaceOffsets[ColorSlotOf(FrontFaceOf(ArtFaceIndex))];
					V[K].UV = FVector2f((SourceCell->X + Offset.X + Sprite.X) / Atlas.Width, (SourceCell->Y + Offset.Y + Sprite.Y) / Atlas.Height);
				}
				else
				{
					// Ceiling materials tile once per cell.
					V[K].UV = Face.Dir.Y != 0 ? FVector2f(Corner.X, Corner.Z) : FVector2f(Face.Dir.X != 0 ? Corner.Z : Corner.X, Corner.Y);
				}
			}
			int32 Section = bCeilingMesh ? CeilingSection : SourceCell ? BrickSection : FillSection;
			if (Section == INDEX_NONE)
			{
				Section = FillSection = Builder.GetSection(TEXT("BrickFill"), Atlas.FillMaterial);
			}
			FLBAMeshBuilder& Into = bCeilingMesh ? CeilingBuilder : Builder;
			Into.AddTriangleFacing(Section, V[0], V[1], V[2], Normal);
			Into.AddTriangleFacing(Section, V[0], V[2], V[3], Normal);
		};

		// A face of the cell at (Next, Y) toward the air column (on its side Side), in the wallpaper laid from the air's
		// floor to its top: its space's on the sides the original drew, the building's on the camera-side walls.
		auto EmitWall = [&](const FAirColumn& At, const FIntPoint& Next, int32 Side, int32 Y, EPart Part)
		{
			const int32 FaceIndex = FaceOfSide[Side];
			const bool bDrawnSide = Side == 0 || Side == 2;
			const FIntVector Cell(Next.X, Y, Next.Y);
			const bool bOwnFinish = Art.OwnFinish.IsValidIndex(At.Space) && Art.OwnFinish[At.Space];
			const FWallpaper& Wallpaper = (bDrawnSide || bOwnFinish) && Art.Wallpapers.IsValidIndex(At.Space) ? Art.Wallpapers[At.Space][Side / 2] : Art.DefaultWallpapers[Side / 2];
			bool bDecal = false;
			if (bDrawnSide && IsDrawn(Next.X, Y, Next.Y))
			{
				// The original's art stays, but its cornice (and the frieze under it) where the wall now goes on above it:
				// the wallpaper puts them back under the ceiling. The wallpaper's top row, or the top rows of any wall panel
				// as tall as the wallpaper's (its variants: a wall's end, a corner).
				const TStaticArray<float, 3>* Coverage = Atlas.FaceCoverage.Find(Plan.GetBrick(Next.X, Y, Next.Y));
				const bool bOpaque = Coverage && (*Coverage)[ColorSlotOf(FrontFaceOf(FaceIndex))] >= OpaqueCoverage;
				const FIntPoint OwnRow = LayoutRowAt(Next.X, Y, Next.Y);
				const FLBALayout* OwnLayout = Art.Library && Art.Library->Layouts.IsValidIndex(OwnRow.X) ? &Art.Library->Layouts[OwnRow.X] : nullptr;
				const FLBALayout* TopLayout = Art.Library && Wallpaper.Rows.Num() > 0 && Art.Library->Layouts.IsValidIndex(Wallpaper.Rows.Last().X)
					? &Art.Library->Layouts[Wallpaper.Rows.Last().X] : nullptr;
				const int32 Crown = Wallpaper.Rows.Num() >= FWallpaper::MinFrieze ? 2 : 1;
				const int32 DrawnTop = TopDrawn(IsDrawn, Next, At.Floor + 1, At.Top);
				const bool bPanelTop = OwnLayout && TopLayout && (OwnLayout->SizeX == 1 || OwnLayout->SizeZ == 1)
					&& OwnLayout->SizeY == TopLayout->SizeY && OwnRow.Y >= OwnLayout->SizeY - Crown;
				const bool bCornice = Wallpaper.Rows.Num() >= 2 && DrawnTop + 1 < At.Top && Y > DrawnTop - Crown
					&& (OwnRow == Wallpaper.Rows.Last() || bPanelTop);
				if (bOpaque && !bCornice)
				{
					return;
				}
				bDecal = !bOpaque;
			}
			OutCoveredFaces.Add(FIntVector4(Cell.X, Cell.Y, Cell.Z, FaceIndex));
			if (bDecal)
			{
				// The cell's own art, again, over the wallpaper.
				const int32 Brick = Plan.GetBrick(Next.X, Y, Next.Y);
				if (const FIntPoint* Source = !bDebugParts && !bDebugArt ? Atlas.Cells.Find(Brick) : nullptr)
				{
					EmitQuad(Cell, FaceIndex, Source, FrontFaceOf(FaceIndex), FVector4f(0.5f, 0.5f, 0.5f, 0.5f), DecalInset, false, 0.f);
				}
			}
			else
			{
				OutCovered.Add(Cell);
			}
			if (bDebugParts)
			{
				EmitQuad(Cell, FaceIndex, nullptr, FaceIndex, LBABodyConverter::ToVertexColor(PartColors[int32(Part)], 0.5f), WallInset, false);
				return;
			}
			if (Part == EPart::Exit)
			{
				EmitQuad(Cell, FaceIndex, nullptr, FaceIndex, LBABodyConverter::ToVertexColor(FColor(6, 6, 8), 0.5f), WallInset, false);
				return;
			}
			const int32 Above = Y - (At.Floor + 1);
			const FIntPoint Row = Wallpaper.GetRow(Above, At.Top - (At.Floor + 1));
			int32 ArtFace = FrontFaceOf(FaceIndex);
			const FIntPoint* Source = Row.X != INDEX_NONE ? WallArt(LayoutBrick(Row.X, Row.Y, Next), FaceIndex, FacingSlotOf(Row.X), ArtFace) : nullptr;
			const int32 Kind = Source ? (Above == 0 ? 1 : 0) : 2;
			++Stats[Kind];
			if (bDebugArt)
			{
				EmitQuad(Cell, FaceIndex, nullptr, FaceIndex, LBABodyConverter::ToVertexColor(ArtColors[Kind], 0.5f), WallInset, false);
				return;
			}
			const FVector4f Flat = Art.SpaceColors.IsValidIndex(At.Space) ? Art.SpaceColors[At.Space] : FVector4f(0.5f, 0.5f, 0.5f, 0.5f);
			EmitQuad(Cell, FaceIndex, Source, ArtFace, Flat, WallInset, false);
		};

		// The top of what the air closes on (a wall raised to the ceiling, a doorway's header), between two airs: closed under
		// the ceiling (the faces on its sides leave a slot).
		TSet<FIntVector> Caps;
		auto Cap = [&](const FAirColumn& At, const FIntPoint& Next, int32 Y)
		{
			const FIntVector Cell(Next.X, At.Top, Next.Y);
			if (Y + 1 == At.Top && Art.bCeiling && At.Top < LBA::GridSizeY && !Plan.IsSolid(Cell.X, Cell.Y, Cell.Z) && !AirAt.Contains(Cell) && !Caps.Contains(Cell))
			{
				Caps.Add(Cell);
				const FVector4f Color = bDebugParts ? LBABodyConverter::ToVertexColor(PartColors[int32(EPart::Ceiling)], 0.5f) : Art.CeilingColor;
				EmitQuad(Cell, 1, nullptr, 1, Color, 0.f, true);
			}
		};
		for (const FAirColumn& At : Air)
		{
			const FLBAPlanLevel* Level = Plan.FindLevel(At.Level);
			const bool bExit = At.Connector != INDEX_NONE && Plan.GetConnectors()[At.Connector].Kind == ELBAConnector::Exit;
			// An exit's doorway (an opening in a wall's line): its side facing away from its room (the original's plug
			// right behind it) closes in the dark like the rest past it.
			int32 Outward = INDEX_NONE;
			const bool bExitDoorway = bExit && Level && Level->Gaps.Contains(At.Column);
			for (int32 Side = 0; Side < 4 && bExitDoorway; ++Side)
			{
				if (Plan.FindRoom(At.Level, At.Column + FLBARoomPlan::Sides[Side]) != INDEX_NONE)
				{
					Outward = Side ^ 1;
				}
			}
			// Over a slope (a flight's step), the walls beside it close from the step's own cell, open above the slope.
			const bool bOnSlope = IsSlope(At.Column.X, At.Floor, At.Column.Y);
			for (int32 Side = 0; Side < 4; ++Side)
			{
				const FIntPoint Next = At.Column + FLBARoomPlan::Sides[Side];
				const bool bWall = Level && Level->Get(Next) == ELBAColumn::Wall;
				const bool bPastExit = bExit && (BeyondExits.Contains(Next) || Side == Outward);
				// What rises from the air's floor beside it (a slope there is the next step up, walked on).
				const bool bRising = Plan.IsSolid(Next.X, At.Floor + 1, Next.Y) && !IsSlope(Next.X, At.Floor + 1, Next.Y);
				for (int32 Y = bOnSlope ? At.Floor : At.Floor + 1; Y < At.Top; ++Y)
				{
					// Slopes off the walls are walked on: never closed.
					if (AirAt.Contains(FIntVector(Next.X, Y, Next.Y)) || (!bWall && IsSlope(Next.X, Y, Next.Y)))
					{
						continue;
					}
					if (Y == At.Floor && (bExit || (!bWall && !Plan.IsSolid(Next.X, Y, Next.Y))))
					{
						continue;
					}
					if (bPastExit || (bExit && !bWall && !Plan.IsSolid(Next.X, Y, Next.Y)))
					{
						EmitWall(At, Next, Side, Y, EPart::Exit);
					}
					else if (bWall)
					{
						EmitWall(At, Next, Side, Y, Side == 0 || Side == 2 ? EPart::Wall : EPart::CameraSide);
						Cap(At, Next, Y);
					}
					else if (AirOfColumn.FindRef(Next).ContainsByPredicate([&](int32 Index) { return Air[Index].Floor < Y && Air[Index].Top <= Y; }))
					{
						EmitWall(At, Next, Side, Y, EPart::Step);
						Cap(At, Next, Y);
					}
					else if (Plan.IsSolid(Next.X, Y, Next.Y) || bRising)
					{
						EmitWall(At, Next, Side, Y, EPart::Solid);
						Cap(At, Next, Y);
					}
				}
			}
			// The ceiling: under the cell over the air (a lintel's, a gallery's underside), else where the space has one.
			const bool bUnder = At.Top < LBA::GridSizeY && Plan.IsSolid(At.Column.X, At.Top, At.Column.Y);
			if (At.Top < LBA::GridSizeY && AirAt.Contains(FIntVector(At.Column.X, At.Top, At.Column.Y)))
			{
				continue;
			}
			// A vaulted space's air that reaches its ceiling (its rooms, the connectors of its height): under its vault (below).
			if (!VaultShape.IsFlat() && Art.bCeiling && !bUnder && Plan.GetSpaces().IsValidIndex(At.Space) && At.Top == Plan.GetSpaces()[At.Space].Ceiling)
			{
				TBitArray<>& Footprint = VaultFootprints.FindOrAdd(At.Space);
				if (Footprint.Num() == 0)
				{
					Footprint.Init(false, FLBARoomPlan::SizeXZ * FLBARoomPlan::SizeXZ);
				}
				Footprint[FLBARoomPlan::ToColumnIndex(At.Column)] = true;
				continue;
			}
			if (Art.bCeiling || bUnder)
			{
				if (!IsDrawn(At.Column.X, At.Top, At.Column.Y))
				{
					OutCovered.Add(FIntVector(At.Column.X, At.Top, At.Column.Y));
				}
				const FVector4f Color = bDebugParts ? LBABodyConverter::ToVertexColor(PartColors[int32(EPart::Ceiling)], 0.5f) : Art.CeilingColor;
				EmitQuad(FIntVector(At.Column.X, At.Top, At.Column.Y), 1, nullptr, 1, Color, 0.f, true);
			}
		}
		// The vaults: each vaulted space's roof springs from its ceiling height at its walls and rises inward (FLBAVault), in
		// the stone the walls around them are crowned with (the rows drawn on top of them); never through what is over it
		// (a higher storey).
		TMap<FIntPoint, int32> CrownVotes;
		for (const TPair<int32, TBitArray<>>& Pair : VaultFootprints)
		{
			for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
			{
				const FIntPoint Column(Index % FLBARoomPlan::SizeXZ, Index / FLBARoomPlan::SizeXZ);
				for (int32 Side = 0; Side < 4 && Pair.Value[Index]; ++Side)
				{
					const FIntPoint Next = Column + FLBARoomPlan::Sides[Side];
					const FLBAPlanLevel* Level = Plan.FindLevel(Plan.GetSpaces()[Pair.Key].Level);
					if (!FLBARoomPlan::IsInGrid(Next) || Pair.Value[FLBARoomPlan::ToColumnIndex(Next)] || !Level || Level->Get(Next) != ELBAColumn::Wall)
					{
						continue;
					}
					const int32 Top = TopDrawn(IsDrawn, Next, 0, LBA::GridSizeY);
					for (int32 Y = Top; Y >= FMath::Max(Top - 1, 0) && Top != INDEX_NONE; --Y)
					{
						const TStaticArray<float, 3>* Coverage = Atlas.FaceCoverage.Find(Plan.GetBrick(Next.X, Y, Next.Y));
						if (IsDrawn(Next.X, Y, Next.Y) && Coverage && FMath::Max((*Coverage)[1], (*Coverage)[2]) >= OpaqueCoverage)
						{
							++CrownVotes.FindOrAdd(LayoutRowAt(Next.X, Y, Next.Y));
						}
					}
				}
			}
		}
		UMaterialInterface* VaultMaterial = nullptr;
		for (const TPair<int32, TBitArray<>>& Pair : VaultFootprints)
		{
			const FLBAPlanSpace& Space = Plan.GetSpaces()[Pair.Key];
			const FLBAVault Vault(Pair.Value, VaultShape, Pair.Key);
			TArray<int32> Overhead;
			Overhead.Init(TNumericLimits<int32>::Max(), FLBARoomPlan::SizeXZ * FLBARoomPlan::SizeXZ);
			for (int32 Index = 0; Index < Overhead.Num(); ++Index)
			{
				for (int32 Y = Space.Ceiling; Y < LBA::GridSizeY && Pair.Value[Index]; ++Y)
				{
					if (Plan.IsSolid(Index % FLBARoomPlan::SizeXZ, Y, Index / FLBARoomPlan::SizeXZ))
					{
						Overhead[Index] = Y;
						break;
					}
				}
			}
			auto Height = [&](float X, float Z)
			{
				// At most the lowest thing over the columns meeting at the point.
				float Cap = TNumericLimits<float>::Max();
				for (const FIntPoint& Corner : { FIntPoint(0, 0), FIntPoint(-1, 0), FIntPoint(0, -1), FIntPoint(-1, -1) })
				{
					const FIntPoint Column(FMath::FloorToInt(X + 0.01f) + Corner.X, FMath::FloorToInt(Z + 0.01f) + Corner.Y);
					if (Vault.Contains(Column) && FMath::Abs(Column.X + 0.5f - X) < 1.f && FMath::Abs(Column.Y + 0.5f - Z) < 1.f)
					{
						Cap = FMath::Min(Cap, float(Overhead[FLBARoomPlan::ToColumnIndex(Column)]));
					}
				}
				return FMath::Min(Space.Ceiling + Vault.GetRise(X, Z), Cap);
			};
			if (!VaultMaterial)
			{
				VaultMaterial = bDebugParts || bDebugArt ? Atlas.FillMaterial : MakeFinishMaterial(FCrownFinish::FromVotes(CrownVotes), true);
			}
			const FVector4f Color = bDebugParts ? LBABodyConverter::ToVertexColor(PartColors[int32(EPart::Ceiling)], 0.5f)
				: bDebugArt ? LBABodyConverter::ToVertexColor(FColor(255, 0, 255), 0.5f) : FVector4f(0.5f, 0.5f, 0.5f, 0.5f);
			EmitRoof(Vault, Height, CeilingBuilder, CeilingBuilder.GetSection(TEXT("Roof"), VaultMaterial), Color, Scale);
		}
		UE_LOG(LogLBA2, Display, TEXT("Enclosure wall faces: %d wallpaper, %d wallpaper foot, %d flat"), Stats[0], Stats[1], Stats[2]);
	}
	/** Height of a ramp (0..1 of a brick) at a cell corner; see ReajustPos in OBJECT.CPP. */
	float RampHeight(int32 Ramp, float LX, float LZ)
	{
		switch (Ramp)
		{
		case 2: return LX;
		case 3: return LZ;
		case 4: return 1.f - LZ;
		case 5: return 1.f - LX;
		default: return 1.f;
		}
	}

	/** Corner heights H[dx][dz] and diagonal of a collision shape. */
	void ShapeCorners(int32 Shape, float H[2][2], bool& bMainDiagonal)
	{
		bMainDiagonal = true;
		int32 RampA = Shape, RampB = Shape; // A: side holding corner (0,1) or (0,0); B: the other side
		switch (Shape)
		{
		case 6: RampA = 3; RampB = 2; break;
		case 7: RampA = 5; RampB = 4; break;
		case 10: RampA = 2; RampB = 3; break;
		case 11: RampA = 4; RampB = 5; break;
		case 8: RampA = 4; RampB = 2; bMainDiagonal = false; break;
		case 9: RampA = 5; RampB = 3; bMainDiagonal = false; break;
		case 12: RampA = 2; RampB = 4; bMainDiagonal = false; break;
		case 13: RampA = 3; RampB = 5; bMainDiagonal = false; break;
		default: break;
		}
		if (Shape >= 6 && Shape <= 13)
		{
			if (bMainDiagonal)
			{
				// Split along (0,0)-(1,1): A holds (0,1), B holds (1,0).
				H[0][0] = RampHeight(RampA, 0, 0);
				H[1][1] = RampHeight(RampA, 1, 1);
				H[0][1] = RampHeight(RampA, 0, 1);
				H[1][0] = RampHeight(RampB, 1, 0);
			}
			else
			{
				// Split along (1,0)-(0,1): A holds (0,0), B holds (1,1).
				H[1][0] = RampHeight(RampA, 1, 0);
				H[0][1] = RampHeight(RampA, 0, 1);
				H[0][0] = RampHeight(RampA, 0, 0);
				H[1][1] = RampHeight(RampB, 1, 1);
			}
			return;
		}
		for (int32 DX = 0; DX < 2; ++DX)
		{
			for (int32 DZ = 0; DZ < 2; ++DZ)
			{
				H[DX][DZ] = (Shape >= 2 && Shape <= 5) ? RampHeight(Shape, DX, DZ) : 1.f;
			}
		}
	}

	/** Complex collision: exact block and ramp surfaces, hidden faces between full blocks removed. */
	void EmitCollision(const FCellQuery& Query, FLBAMeshBuilder& Builder, int32 Section, float Scale)
	{
		auto Solid = [&Query](int32 X, int32 Y, int32 Z) { return Query.ShapeAt(X, Y, Z) == 1; };
		auto Quad = [&](const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D, const FVector3f& Outward)
		{
			FLBAVertex V[4];
			V[0].Position = A; V[1].Position = B; V[2].Position = C; V[3].Position = D;
			for (FLBAVertex& Vertex : V)
			{
				Vertex.Normal = Outward;
			}
			if (FLBAMeshBuilder::FrontNormal(A, B, C).SizeSquared() > UE_SMALL_NUMBER)
			{
				Builder.AddTriangleFacing(Section, V[0], V[1], V[2], Outward);
			}
			if (FLBAMeshBuilder::FrontNormal(A, C, D).SizeSquared() > UE_SMALL_NUMBER)
			{
				Builder.AddTriangleFacing(Section, V[0], V[2], V[3], Outward);
			}
		};

		for (int32 Z = 0; Z < Query.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Query.Size.X; ++X)
			{
				for (int32 Y = 0; Y < Query.Size.Y; ++Y)
				{
					const int32 Shape = Query.ShapeAt(X, Y, Z);
					if (Shape <= 0 || Shape > 13)
					{
						continue;
					}
					float H[2][2];
					bool bMainDiagonal;
					ShapeCorners(Shape, H, bMainDiagonal);
					auto Top = [&](int32 DX, int32 DZ) { return CornerPos(X + DX, Y + H[DX][DZ], Z + DZ, Scale); };
					auto Bottom = [&](int32 DX, int32 DZ) { return CornerPos(X + DX, Y, Z + DZ, Scale); };
					const FVector3f Up = FVector3f(LBA::DirToUnreal(0, 1, 0));

					// Top surface.
					if (Shape != 1 || !Solid(X, Y + 1, Z))
					{
						if (bMainDiagonal)
						{
							Quad(Top(0, 0), Top(0, 1), Top(1, 1), Top(1, 1), Up);
							Quad(Top(0, 0), Top(1, 1), Top(1, 0), Top(1, 0), Up);
						}
						else
						{
							Quad(Top(0, 0), Top(1, 0), Top(0, 1), Top(0, 1), Up);
							Quad(Top(1, 0), Top(1, 1), Top(0, 1), Top(0, 1), Up);
						}
					}
					// Bottom.
					if (!Solid(X, Y - 1, Z))
					{
						Quad(Bottom(0, 0), Bottom(1, 0), Bottom(1, 1), Bottom(0, 1), -Up);
					}
					// Sides, following the surface heights along each edge.
					struct FEdge { int32 DX0, DZ0, DX1, DZ1; FIntVector Dir; };
					static const FEdge Edges[4] =
					{
						{ 1, 0, 1, 1, { 1, 0, 0 } },
						{ 0, 0, 0, 1, { -1, 0, 0 } },
						{ 0, 1, 1, 1, { 0, 0, 1 } },
						{ 0, 0, 1, 0, { 0, 0, -1 } },
					};
					for (const FEdge& Edge : Edges)
					{
						if (Solid(X + Edge.Dir.X, Y, Z + Edge.Dir.Z))
						{
							continue;
						}
						if (H[Edge.DX0][Edge.DZ0] <= 0.f && H[Edge.DX1][Edge.DZ1] <= 0.f)
						{
							continue;
						}
						const FVector3f Outward = FVector3f(LBA::DirToUnreal(Edge.Dir.X, 0, Edge.Dir.Z));
						Quad(Bottom(Edge.DX0, Edge.DZ0), Top(Edge.DX0, Edge.DZ0), Top(Edge.DX1, Edge.DZ1), Bottom(Edge.DX1, Edge.DZ1), Outward);
					}
				}
			}
		}
	}

	/** Simple collision for kit pieces: one box per full block, one convex hull per ramp. */
	void BuildSimpleCollision(const FCellQuery& Query, FKAggregateGeom& Geom, float Scale)
	{
		const FVector Size = FVector(CornerPos(1, 1, 1, Scale));
		for (int32 Z = 0; Z < Query.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Query.Size.X; ++X)
			{
				// Merge vertical runs of full blocks into a single box.
				int32 RunStart = INDEX_NONE;
				for (int32 Y = 0; Y <= Query.Size.Y; ++Y)
				{
					const int32 Shape = Y < Query.Size.Y ? Query.ShapeAt(X, Y, Z) : 0;
					if (Shape == 1)
					{
						if (RunStart == INDEX_NONE)
						{
							RunStart = Y;
						}
						continue;
					}
					if (RunStart != INDEX_NONE)
					{
						const FVector Min(CornerPos(X, RunStart, Z, Scale));
						const FVector Max(CornerPos(X + 1, Y, Z + 1, Scale));
						const FBox Box(Min.ComponentMin(Max), Min.ComponentMax(Max));
						FKBoxElem Elem(Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z);
						Elem.Center = Box.GetCenter();
						Geom.BoxElems.Add(Elem);
						RunStart = INDEX_NONE;
					}
					if (Shape >= 2 && Shape <= 13 && Y < Query.Size.Y)
					{
						float H[2][2];
						bool bMainDiagonal;
						ShapeCorners(Shape, H, bMainDiagonal);
						FKConvexElem Convex;
						for (int32 DX = 0; DX < 2; ++DX)
						{
							for (int32 DZ = 0; DZ < 2; ++DZ)
							{
								Convex.VertexData.Add(FVector(CornerPos(X + DX, Y, Z + DZ, Scale)));
								Convex.VertexData.Add(FVector(CornerPos(X + DX, Y + FMath::Max(H[DX][DZ], 0.02f), Z + DZ, Scale)));
							}
						}
						Convex.UpdateElemBox();
						Geom.ConvexElems.Add(Convex);
					}
				}
			}
		}
		(void)Size;
	}

	FCellQuery MakeLayoutQuery(const FLBALayout& Layout, int32 ForbiddenBrick)
	{
		FCellQuery Query;
		Query.Size = FIntVector(Layout.SizeX, Layout.SizeY, Layout.SizeZ);
		const FLBALayout* L = &Layout;
		auto Index = [L](int32 X, int32 Y, int32 Z) { return Z * L->SizeX * L->SizeY + Y * L->SizeX + X; };
		Query.Brick = [L, Index, ForbiddenBrick](int32 X, int32 Y, int32 Z)
		{
			const int32 Brick = L->Blocks[Index(X, Y, Z)].Brick;
			return Brick == ForbiddenBrick ? 0 : Brick;
		};
		Query.Shape = [L, Index](int32 X, int32 Y, int32 Z) { return int32(L->Blocks[Index(X, Y, Z)].Shape); };
		return Query;
	}
}

FLBAIsoImporter::FLBAIsoImporter(FLBAImportContext& InContext)
	: Context(InContext)
{
}

FLBAIsoImporter::~FLBAIsoImporter() = default;

bool FLBAIsoImporter::LoadHeader()
{
	Bkg = Context.GetHqr(TEXT("LBA_BKG.HQR"));
	if (!Bkg || !Header.Load(*Bkg))
	{
		Context.Warn(TEXT("LBA_BKG.HQR is missing or unreadable"));
		return false;
	}
	SceneMap = LBAIso::LoadSceneMap(Bkg->GetEntry(Header.GetSceneMapIndex()));
	return true;
}

const FLBALibrary* FLBAIsoImporter::GetLibrary(int32 LibraryIndex)
{
	if (const TUniquePtr<FLBALibrary>* Found = Libraries.Find(LibraryIndex))
	{
		return Found->Get();
	}
	TUniquePtr<FLBALibrary> Library = MakeUnique<FLBALibrary>();
	if (!Library->Load(Bkg->GetEntry(Header.LibraryStart + LibraryIndex), LibraryIndex))
	{
		Context.Warn(FString::Printf(TEXT("Library %d is unreadable"), LibraryIndex));
		Library.Reset();
	}
	return Libraries.Add(LibraryIndex, MoveTemp(Library)).Get();
}

const FLBAIsoImporter::FAtlas* FLBAIsoImporter::GetAtlas(int32 LibraryIndex)
{
	if (const TUniquePtr<FAtlas>* Found = Atlases.Find(LibraryIndex))
	{
		return Found->Get();
	}
	const FLBALibrary* Library = GetLibrary(LibraryIndex);
	if (!Library)
	{
		return nullptr;
	}

	TArray<int32> Bricks;
	for (const FLBALayout& Layout : Library->Layouts)
	{
		for (const FLBALayoutBlock& Block : Layout.Blocks)
		{
			if (Block.Brick != 0 && Block.Brick != Header.ForbiddenBrick)
			{
				Bricks.AddUnique(Block.Brick);
			}
		}
	}
	Bricks.Sort();

	FAtlas Atlas;
	Atlas.Width = 2048;
	const int32 PerRow = Atlas.Width / CellW;
	const int32 Rows = FMath::DivideAndRoundUp(FMath::Max(Bricks.Num(), 1), PerRow);
	Atlas.Height = FMath::RoundUpToPowerOfTwo(Rows * CellH);

	const TArray<FColor>& Palette = Context.GetPalette();
	TArray<FColor> Pixels;
	Pixels.SetNumZeroed(Atlas.Width * Atlas.Height);
	FLBABrickSprite Sprite;
	for (int32 Index = 0; Index < Bricks.Num(); ++Index)
	{
		const FIntPoint Cell((Index % PerRow) * CellW, (Index / PerRow) * CellH);
		Atlas.Cells.Add(Bricks[Index], Cell);
		if (!Sprite.Decode(Bkg->GetEntry(Header.BrickStart + Bricks[Index] - 1)))
		{
			continue;
		}
		// Average color of each visible face, used to fill hidden faces that have no art of their own.
		{
			FLinearColor Sums[3] = { FLinearColor::Transparent, FLinearColor::Transparent, FLinearColor::Transparent };
			int32 Counts[3] = { 0, 0, 0 };
			int32 Areas[3] = { 0, 0, 0 };
			for (int32 Y = 0; Y < SpriteH; ++Y)
			{
				for (int32 X = 0; X < SpriteW; ++X)
				{
					const uint8 ColorIndex = Sprite.Pixels[Y * SpriteW + X];
					const float U = X + 0.5f, V = Y + 0.5f;
					int32 Slot = INDEX_NONE;
					if (FMath::Abs(U - 24.f) / 24.f + FMath::Abs(V - 12.f) / 12.f <= 1.f)
					{
						Slot = 0;
					}
					else if (U >= 24.f && V >= 24.f - (U - 24.f) * 0.5f && V <= 39.f - (U - 24.f) * 0.5f)
					{
						Slot = 1;
					}
					else if (U < 24.f && V >= 12.f + U * 0.5f && V <= 27.f + U * 0.5f)
					{
						Slot = 2;
					}
					if (Slot == INDEX_NONE)
					{
						continue;
					}
					++Areas[Slot];
					if (ColorIndex != 0)
					{
						Sums[Slot] += FLinearColor::FromSRGBColor(Palette[ColorIndex]);
						++Counts[Slot];
					}
				}
			}
			TStaticArray<FColor, 3> Colors;
			TStaticArray<float, 3> Coverage;
			for (int32 Slot = 0; Slot < 3; ++Slot)
			{
				Colors[Slot] = Counts[Slot] > 0 ? (Sums[Slot] / float(Counts[Slot])).ToFColor(true) : FColor(128, 128, 128);
				Coverage[Slot] = Areas[Slot] > 0 ? float(Counts[Slot]) / float(Areas[Slot]) : 0.f;
			}
			Atlas.FaceColors.Add(Bricks[Index], Colors);
			Atlas.FaceCoverage.Add(Bricks[Index], Coverage);
		}

		// The sprite's faces apart (FaceOffsets), each grown by its own pixels so that filtering and mip levels at its
		// edges see the face, not the transparent pixels around it nor the next face (as lba2remake does): first the
		// transparent pixels along the edges of solid faces (the original's neighbors covered them), then one pixel out.
		const TStaticArray<float, 3>& Coverage = Atlas.FaceCoverage[Bricks[Index]];
		TArray<int8> Owner;
		Owner.Init(INDEX_NONE, CellW * CellH);
		auto At = [&](int32 X, int32 Y) -> FColor& { return Pixels[(Cell.Y + Y) * Atlas.Width + Cell.X + X]; };
		for (int32 Y = 0; Y < SpriteH; ++Y)
		{
			for (int32 X = 0; X < SpriteW; ++X)
			{
				const int32 Slot = SpriteSlotOf(X, Y);
				const uint8 ColorIndex = Sprite.Pixels[Y * SpriteW + X];
				const FIntPoint To = FIntPoint(X, Y) + FaceOffsets[Slot];
				FColor Color = Palette[ColorIndex];
				Color.A = ColorIndex == 0 ? 0 : 255;
				At(To.X, To.Y) = Color;
				Owner[To.Y * CellW + To.X] = int8(Slot);
			}
		}
		static const FIntPoint Around[4] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
		for (const bool bOutward : { false, true })
		{
			TArray<TPair<FIntPoint, FColor>> Fills;
			for (int32 Y = 0; Y < CellH; ++Y)
			{
				for (int32 X = 0; X < CellW; ++X)
				{
					const int32 Slot = Owner[Y * CellW + X];
					if (bOutward ? Slot != INDEX_NONE : Slot == INDEX_NONE || At(X, Y).A != 0 || Coverage[Slot] < SolidCoverage)
					{
						continue;
					}
					for (const FIntPoint& Step : Around)
					{
						const FIntPoint From(X + Step.X, Y + Step.Y);
						if (From.X >= 0 && From.Y >= 0 && From.X < CellW && From.Y < CellH && At(From.X, From.Y).A != 0
							&& (bOutward || Owner[From.Y * CellW + From.X] == Slot))
						{
							Fills.Emplace(FIntPoint(X, Y), At(From.X, From.Y));
							break;
						}
					}
				}
			}
			for (const TPair<FIntPoint, FColor>& Fill : Fills)
			{
				At(Fill.Key.X, Fill.Key.Y) = Fill.Value;
			}
		}
	}

	if (bPlansOnly)
	{
		return Atlases.Add(LibraryIndex, MakeUnique<FAtlas>(MoveTemp(Atlas))).Get();
	}
	const FString Folder = Context.GetContentPath(FString::Printf(TEXT("Interiors/Libraries/Lib_%02d"), LibraryIndex));
	Atlas.Texture = Context.CreateTexture(Folder, FString::Printf(TEXT("T_Lib%02d_Bricks"), LibraryIndex), Atlas.Width, Atlas.Height, Pixels, false);
	Atlas.Material = Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Atlas.Texture, Folder);
	Atlas.FillMaterial = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
	Atlas.Pixels = MoveTemp(Pixels);
	return Atlases.Add(LibraryIndex, MakeUnique<FAtlas>(MoveTemp(Atlas))).Get();
}

void FLBAIsoImporter::BuildKitMeshes(int32 LibraryIndex)
{
	const FLBALibrary* Library = GetLibrary(LibraryIndex);
	const FAtlas* Atlas = GetAtlas(LibraryIndex);
	if (!Library || !Atlas)
	{
		return;
	}
	const FString Folder = Context.GetContentPath(FString::Printf(TEXT("Interiors/Libraries/Lib_%02d"), LibraryIndex));
	const float Scale = Context.GetScale();

	for (const FLBALayout& Layout : Library->Layouts)
	{
		const FCellQuery Query = MakeLayoutQuery(Layout, Header.ForbiddenBrick);
		FLBAMeshBuilder Builder;
		const int32 Section = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas->Material);
		EmitVisual(Query, *Atlas, Builder, Section, Scale, Context.GetProjectSettings().BackFaces);
		if (Builder.IsEmpty())
		{
			continue;
		}
		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, Materials, Slots);

		FLBAStaticMeshOptions Options;
		Options.SimpleCollision = [Query, Scale](FKAggregateGeom& Geom) { BuildSimpleCollision(Query, Geom, Scale); };
		Context.CreateStaticMesh(Folder / TEXT("Layouts"), FString::Printf(TEXT("SM_Lib%02d_Layout_%03d"), LibraryIndex, Layout.Index),
			MoveTemp(Description), Materials, Slots, Options);
	}
	Context.Flush();
}

const TArray<FLBAWallStyle>& FLBAIsoImporter::GetWallStyles(int32 LibraryIndex)
{
	if (const TArray<FLBAWallStyle>* Found = WallStyles.Find(LibraryIndex))
	{
		return *Found;
	}
	TArray<FLBAWallStyle>& Styles = WallStyles.Add(LibraryIndex);
	const FLBALibrary* Library = GetLibrary(LibraryIndex);
	const FAtlas* Atlas = GetAtlas(LibraryIndex);
	if (!Library || !Atlas)
	{
		return Styles;
	}
	const int32 Forbidden = Header.ForbiddenBrick;
	auto Coverage = [Atlas, Forbidden](int32 Brick, int32 Slot)
	{
		const TStaticArray<float, 3>* Values = Brick != 0 && Brick != Forbidden ? Atlas->FaceCoverage.Find(Brick) : nullptr;
		return Values ? (*Values)[Slot] : 0.f;
	};
	// Every grid of the library, whatever the grid filter: the styles are the library's.
	FLBAWallStyleDetector Detector(*Library);
	for (int32 GridIndex = 0; GridIndex < Header.GetNumGrids(); ++GridIndex)
	{
		const TArray<uint8> Data = Bkg->GetEntry(Header.GridStart + GridIndex);
		FLBAGrid Grid;
		if (Data.Num() > 0 && Data[0] == LibraryIndex && Grid.Load(Data, GridIndex))
		{
			Detector.AddRuns(GridIndex, LBAWallSurvey::FindRuns(Grid, *Library, Coverage, OpaqueCoverage));
		}
	}
	const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(LibraryIndex);
	// Rows compare by the average color of their bricks' facing sides (sRGB, summed over the row's width).
	auto RowDistance = [Library, Atlas, Forbidden](int32 LayoutIndex, int32 RowA, int32 RowB)
	{
		const FLBALayout& Layout = Library->Layouts[LayoutIndex];
		const int32 Slot = Layout.SizeX == 1 ? 1 : 2;
		float Distance = 0.f;
		for (int32 Z = 0; Z < Layout.SizeZ; ++Z)
		{
			for (int32 X = 0; X < Layout.SizeX; ++X)
			{
				auto ColorOf = [&](int32 Row)
				{
					const int32 Brick = Layout.Blocks[Z * Layout.SizeX * Layout.SizeY + Row * Layout.SizeX + X].Brick;
					const TStaticArray<FColor, 3>* Colors = Brick != Forbidden ? Atlas->FaceColors.Find(Brick) : nullptr;
					return Colors ? FVector3f((*Colors)[Slot].R, (*Colors)[Slot].G, (*Colors)[Slot].B) : FVector3f::ZeroVector;
				};
				Distance += FVector3f::Distance(ColorOf(RowA), ColorOf(RowB));
			}
		}
		return Distance / float(Layout.SizeX * Layout.SizeZ);
	};
	Styles = Detector.Finish(LibraryEnclosure ? TSet<int32>(LibraryEnclosure->IgnoredWallLayouts) : TSet<int32>(), RowDistance);

	const FString Folder = Context.GetContentPath(FString::Printf(TEXT("Interiors/Libraries/Lib_%02d"), LibraryIndex));
	const bool bKit = Context.GetProjectSettings().bCreateLayoutKitMeshes;
	FString Summary;
	for (FLBAWallStyle& Style : Styles)
	{
		FString Line = Style.Facing == ELBAWallFacing::Indoor ? TEXT("indoor") : TEXT("outdoor");
		for (TArray<FLBAWallSegment>* Part : { &Style.Foot, &Style.Body, &Style.Top })
		{
			for (FLBAWallSegment& Segment : *Part)
			{
				if (bKit)
				{
					Segment.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(FString::Printf(TEXT("%s/Layouts/SM_Lib%02d_Layout_%03d.SM_Lib%02d_Layout_%03d"),
						*Folder, LibraryIndex, Segment.Layout, LibraryIndex, Segment.Layout)));
				}
				Line += FString::Printf(TEXT(" %sL%d.%d-%d"), Part == &Style.Body ? TEXT("*") : TEXT(""), Segment.Layout, Segment.From, Segment.From + Segment.Height - 1);
			}
		}
		Summary += FString::Printf(TEXT("\n  %dx %s"), Style.Uses, *Line);
	}
	UE_LOG(LogLBA2, Display, TEXT("Library %d: %d wall styles (layout.rows, *body)%s"), LibraryIndex, Styles.Num(), *Summary);

	if (ULBAWallStyleLibrary* Asset = Context.CreateOrReuseAsset<ULBAWallStyleLibrary>(Folder, FString::Printf(TEXT("DA_Lib%02d_WallStyles"), LibraryIndex)))
	{
		Asset->Library = LibraryIndex;
		Asset->Styles = Styles;
		Context.MarkForSave(Asset);
	}
	return Styles;
}

FString FLBAIsoImporter::GetGridLevelName(int32 GridIndex) const
{
	for (int32 Scene = 0; Scene < SceneMap.Num(); ++Scene)
	{
		if (!SceneMap[Scene].bIsIsland && SceneMap[Scene].SceneryIndex == GridIndex)
		{
			const FString Name = FLBAImportContext::SanitizeName(Context.GetCommunityName(TEXT("scenes"), Scene));
			if (!Name.IsEmpty())
			{
				return FString::Printf(TEXT("L_Grid_%03d_%s"), GridIndex, *Name);
			}
		}
	}
	return FString::Printf(TEXT("L_Grid_%03d"), GridIndex);
}

void FLBAIsoImporter::LoadDoorZones()
{
	DoorZones.Reset();
	const FLBAHqrFile* SceneFile = Context.GetHqr(TEXT("SCENE.HQR"));
	if (!SceneFile)
	{
		return;
	}
	auto ToCell = [](int32 Value, int32 Size)
	{
		return FMath::FloorToInt(double(Value) / Size);
	};
	for (int32 Index = 0; Index + 1 < SceneFile->Num() && SceneMap.IsValidIndex(Index); ++Index)
	{
		FLBAScene Scene;
		if (SceneMap[Index].bIsIsland || !Scene.Load(SceneFile->GetEntry(Index + 1), Index))
		{
			continue;
		}
		for (const FLBASceneZoneData& Zone : Scene.Zones)
		{
			if (Zone.Type != ELBAZoneType::CubeChange)
			{
				continue;
			}
			FLBACellBox& Box = DoorZones.FindOrAdd(SceneMap[Index].SceneryIndex).AddDefaulted_GetRef();
			Box.Min = FIntVector(ToCell(FMath::Min(Zone.Min.X, Zone.Max.X), LBA::BrickSizeXZ), ToCell(FMath::Min(Zone.Min.Y, Zone.Max.Y), LBA::BrickSizeY),
				ToCell(FMath::Min(Zone.Min.Z, Zone.Max.Z), LBA::BrickSizeXZ));
			Box.Max = FIntVector(ToCell(FMath::Max(Zone.Min.X, Zone.Max.X), LBA::BrickSizeXZ), ToCell(FMath::Max(Zone.Min.Y, Zone.Max.Y), LBA::BrickSizeY),
				ToCell(FMath::Max(Zone.Min.Z, Zone.Max.Z), LBA::BrickSizeXZ));
		}
	}
}

const TSet<int32>& FLBAIsoImporter::GetWallBricks(int32 LibraryIndex)
{
	if (const TSet<int32>* Found = WallBricks.Find(LibraryIndex))
	{
		return *Found;
	}
	// Plans made with the height rules alone in every grid of the library tell its walls' bricks.
	FLBAWallBrickCounts Counts;
	TSet<int32> FrameLayouts;
	if (const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(LibraryIndex))
	{
		FrameLayouts.Append(LibraryEnclosure->CutFrames);
	}
	const FLBALibrary* Library = GetLibrary(LibraryIndex);
	const FAtlas* Atlas = GetAtlas(LibraryIndex);
	for (int32 GridIndex = 0; GridIndex < Header.GetNumGrids() && Library; ++GridIndex)
	{
		FLBAGrid Grid;
		if (!Grid.Load(Bkg->GetEntry(Header.GridStart + GridIndex), GridIndex) || Grid.LibraryIndex != LibraryIndex)
		{
			continue;
		}
		const FCellQuery Query = MakeGridQuery(Grid, *Library, Header.ForbiddenBrick);
		const FLBARoomPlan Plan(MakePlanCells(Query, Grid, *Library, Atlas, FrameLayouts), nullptr, DoorZones.FindRef(GridIndex),
			Context.GetProjectSettings().GetEnclosure(LibraryIndex, GridIndex).RoomHeight);
		Plan.CountBricks(Counts);
	}
	TSet<int32>& Result = WallBricks.Add(LibraryIndex, Counts.GetWallBricks());
	UE_LOG(LogLBA2, Display, TEXT("Library %d: %d wall bricks, %d furniture bricks"), LibraryIndex, Result.Num(), Counts.Furniture.Num());
	return Result;
}

struct FLBAIsoImporter::FGridPlanParts
{
	TUniquePtr<FLBARoomPlan> Rooms;
	TUniquePtr<FLBAHollow> Hollow;
	/** An exit's columns past its doorway, left open (BuildAir). */
	TSet<FIntPoint> BeyondExits;
};

bool FLBAIsoImporter::BuildGridPlan(int32 GridIndex, const FLBAGrid& Grid, FGridPlanParts& Parts, FLBAGridPlan& Out)
{
	const FLBALibrary* Library = GetLibrary(Grid.LibraryIndex);
	const FAtlas* Atlas = GetAtlas(Grid.LibraryIndex);
	if (!Library || !Atlas)
	{
		return false;
	}
	const FLBAEnclosureSettings Settings = Context.GetProjectSettings().GetEnclosure(Grid.LibraryIndex, GridIndex);
	TSet<int32> FrameLayouts;
	if (const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex))
	{
		FrameLayouts.Append(LibraryEnclosure->CutFrames);
	}
	const FCellQuery Query = MakeGridQuery(Grid, *Library, Header.ForbiddenBrick);
	const FLBAGridCells Cells = MakePlanCells(Query, Grid, *Library, Atlas, FrameLayouts);

	Out = FLBAGridPlan();
	Out.Grid = GridIndex;
	Out.Name = GetGridLevelName(GridIndex);
	Out.Library = Grid.LibraryIndex;
	Out.bEnclosed = Settings.bEnclose;
	Out.Structure = Settings.Structure;
	Out.RoomHeight = Settings.RoomHeight;
	Out.DoorHeight = Settings.DoorHeight;
	if (Settings.Structure == ELBAEnclosureStructure::Hollow)
	{
		// A hollow in the rock (FLBAHollow): one roof over the walkable floors, rock walls up to it from what bounds them.
		FLBAHollow::FSettings HollowSettings;
		HollowSettings.Clear = Settings.RoomHeight;
		HollowSettings.SpringReach = Settings.SpringReach;
		HollowSettings.Shape = FLBAVaultShape{ Settings.VaultRadius, Settings.VaultRise, Settings.VaultNoise };
		HollowSettings.Seed = GridIndex;
		if (const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex))
		{
			HollowSettings.PillarLayouts.Append(LibraryEnclosure->Pillars);
			HollowSettings.RailingLayouts.Append(LibraryEnclosure->Railings);
		}
		Parts.Hollow = MakeUnique<FLBAHollow>(Cells, HollowSettings, DoorZones.FindRef(GridIndex));
		Parts.Hollow->ToGridPlan(Out.Hollow);
		return true;
	}
	// Grids left open (open-air ones) get a plan too: to look at, not built.
	Parts.Rooms = MakeUnique<FLBARoomPlan>(Cells, &GetWallBricks(Grid.LibraryIndex), DoorZones.FindRef(GridIndex), Settings.RoomHeight);
	Parts.Rooms->ToGridPlan(Out);
	Out.Air = BuildAir(*Parts.Rooms, Settings.DoorHeight, Parts.BeyondExits);
	Out.BeyondExits = Parts.BeyondExits.Array();
	Out.BeyondExits.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
	return true;
}

bool FLBAIsoImporter::BeginPlans()
{
	bPlansOnly = true;
	if (!LoadHeader())
	{
		return false;
	}
	LoadDoorZones();
	// The import plans grids with the remake's fixes of their cells (grids.json patches): so do the plans.
	Remake = MakeUnique<FLBARemakeAssets>(Context);
	Remake->Initialize();
	return true;
}

bool FLBAIsoImporter::MakeGridPlan(int32 GridIndex, FLBAGridPlan& Out)
{
	FLBAGrid Grid;
	if (!Bkg || !Grid.Load(Bkg->GetEntry(Header.GridStart + GridIndex), GridIndex))
	{
		return false;
	}
	if (Remake && Remake->IsAvailable())
	{
		FLBARemakeAssets::FGridResult Patched;
		Remake->ApplyPatches(Grid, Patched);
	}
	FGridPlanParts Parts;
	return BuildGridPlan(GridIndex, Grid, Parts, Out);
}

bool FLBAIsoImporter::ImportGrid(int32 GridIndex)
{
	FLBAGrid Grid;
	if (!Grid.Load(Bkg->GetEntry(Header.GridStart + GridIndex), GridIndex))
	{
		Context.Warn(FString::Printf(TEXT("Grid %d is unreadable"), GridIndex));
		return false;
	}
	const FLBALibrary* Library = GetLibrary(Grid.LibraryIndex);
	const FAtlas* Atlas = GetAtlas(Grid.LibraryIndex);
	if (!Library || !Atlas)
	{
		return false;
	}

	// Community 3D models replace some layouts; their bricks are hidden but keep their collision.
	FLBARemakeAssets::FGridResult Remade;
	if (Remake && Remake->IsAvailable())
	{
		Remade = Remake->ProcessGrid(Grid, *Library);
	}

	const int32 Forbidden = Header.ForbiddenBrick;
	const FCellQuery Query = MakeGridQuery(Grid, *Library, Forbidden);

	const float Scale = Context.GetScale();
	const FString Folder = Context.GetContentPath(TEXT("Interiors/Grids"));
	const FString MeshName = FString::Printf(TEXT("SM_Grid_%03d"), GridIndex);

	FLBAMeshBuilder CollisionBuilder;
	EmitCollision(Query, CollisionBuilder, CollisionBuilder.GetSection(TEXT("Collision"), Context.GetMaterials().GetMaster(ELBAMaterial::Invisible)), Scale);
	UStaticMesh* CollisionMesh = nullptr;
	if (!CollisionBuilder.IsEmpty())
	{
		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		CollisionBuilder.BuildStatic(Description, Materials, Slots);
		FLBAStaticMeshOptions Options;
		Options.bComplexAsSimple = true;
		CollisionMesh = Context.CreateStaticMesh(Folder / TEXT("Collision"), MeshName + TEXT("_Collision"), MoveTemp(Description), Materials, Slots, Options);
		// The render mesh cooks its collision from this mesh, so it must be fully built first.
		Context.FinishPendingBuilds();
	}

	// The plan of the grid's rooms (saved: DA_Grid_NNN_Plan, and as text in the plugin's Tests/Plans), and the enclosure
	// that closes them for the Enhanced look.
	FLBAGridPlan GridPlan;
	FGridPlanParts Parts;
	ULBAGridPlanAsset* PlanAsset = nullptr;
	if (BuildGridPlan(GridIndex, Grid, Parts, GridPlan))
	{
		PlanAsset = Context.CreateOrReuseAsset<ULBAGridPlanAsset>(Context.GetContentPath(TEXT("Interiors/Grids/Plans")), FString::Printf(TEXT("DA_Grid_%03d_Plan"), GridIndex));
		if (PlanAsset)
		{
			const FString LevelName = GetGridLevelName(GridIndex);
			PlanAsset->Plan = GridPlan;
			PlanAsset->Level = TSoftObjectPtr<UWorld>(FSoftObjectPath(Folder / TEXT("Levels") / LevelName + TEXT(".") + LevelName));
			Context.MarkForSave(PlanAsset);
		}
		FFileHelper::SaveStringToFile(LBAGridPlanText::Write(GridPlan), *LBAGridPlanText::GetPath(GridIndex), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	UStaticMesh* EnclosureMesh = nullptr;
	UStaticMesh* CeilingMesh = nullptr;
	// The enclosed spaces: the box of their air (cells) and their floor's color, for their bounce light.
	struct FRoomBox
	{
		FIntVector Min = FIntVector(TNumericLimits<int32>::Max());
		FIntVector Max = FIntVector(TNumericLimits<int32>::Lowest());
		FLinearColor Floor = FLinearColor::Black;
	};
	TArray<FRoomBox> RoomBoxes;
	// The frames of the cut walls that the enclosure replaces: off the grid's mesh, in a mesh of their own.
	TSet<FIntVector> RemovedFrames;
	// The grid's faces the enclosure covers (cell, face index): off its mesh, with the frames for the Original look.
	TSet<FIntVector4> CoveredFaces;
	{
		const FLBAEnclosureSettings Settings = Context.GetProjectSettings().GetEnclosure(Grid.LibraryIndex, GridIndex);
		const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex);
		TSet<int32> FrameLayouts;
		if (LibraryEnclosure)
		{
			FrameLayouts.Append(LibraryEnclosure->CutFrames);
		}
		// A layout instance: its layout and the grid cell of its block (0, 0, 0).
		auto InstanceOf = [&Grid, Library](int32 X, int32 Y, int32 Z)
		{
			const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
			if (!Library->Layouts.IsValidIndex(Cell.Layout))
			{
				return FIntVector4(INDEX_NONE, 0, 0, 0);
			}
			const FIntVector Block = Library->Layouts[Cell.Layout].GetBlockCoords(Cell.Block);
			return FIntVector4(Cell.Layout, X - Block.X, Y - Block.Y, Z - Block.Z);
		};
		// The cuts through walls: where the original cut a wall down for the camera it keeps the wall's collision above,
		// invisible, so the topmost piece drawn under CutCollision invisible cells (frames aside) is the cut (a slice
		// of a band, dark stone), not a wall's face. Its instance counts as a frame.
		TSet<FIntVector4> CutInstances;
		for (int32 Z = 0; Z < LBA::GridSizeXZ; ++Z)
		{
			for (int32 X = 0; X < LBA::GridSizeXZ; ++X)
			{
				int32 Invisible = 0;
				for (int32 Y = LBA::GridSizeY - 1; Y >= 0; --Y)
				{
					const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
					if (Cell.Layout < 0)
					{
						Invisible = Cell.Block != 0 ? Invisible + 1 : 0;
						continue;
					}
					if (FrameLayouts.Contains(int32(Cell.Layout)) || Query.BrickAt(X, Y, Z) == 0)
					{
						continue;
					}
					if (Invisible >= FLBARoomPlan::CutCollision)
					{
						CutInstances.Add(InstanceOf(X, Y, Z));
					}
					Invisible = 0;
				}
			}
		}
		auto IsFrame = [&Grid, &FrameLayouts, &CutInstances, &InstanceOf](int32 X, int32 Y, int32 Z)
		{
			return FrameLayouts.Contains(int32(Grid.Get(X, Y, Z).Layout)) || (CutInstances.Num() > 0 && CutInstances.Contains(InstanceOf(X, Y, Z)));
		};
		// Drawn: a brick with art (some libraries stack bricks without any to block the camera-side walls) that is not
		// a frame of the cut walls.
		auto IsDrawn = [&Query, &Atlas, &IsFrame](int32 X, int32 Y, int32 Z)
		{
			const int32 Brick = Query.BrickAt(X, Y, Z);
			return Brick != 0 && !IsFrame(X, Y, Z) && HasBrickArt(Atlas, Brick);
		};
		// The library's wall styles (DA_LibNN_WallStyles: the vocabulary of a future wall builder).
		GetWallStyles(Grid.LibraryIndex);
		// The enclosure's meshes: walls (casting shadows) and ceilings (letting the overhead light in).
		auto CreateEnclosureMeshes = [&](const FLBAMeshBuilder& EnclosureBuilder, const FLBAMeshBuilder& CeilingBuilder)
		{
			for (const TPair<const FLBAMeshBuilder*, UStaticMesh**>& Pair : { TPair<const FLBAMeshBuilder*, UStaticMesh**>(&CeilingBuilder, &CeilingMesh),
				TPair<const FLBAMeshBuilder*, UStaticMesh**>(&EnclosureBuilder, &EnclosureMesh) })
			{
				if (Pair.Key->IsEmpty())
				{
					continue;
				}
				FMeshDescription Description;
				TArray<UMaterialInterface*> Materials;
				TArray<FName> Slots;
				Pair.Key->BuildStatic(Description, Materials, Slots);
				FLBAStaticMeshOptions Options;
				Options.bComplexAsSimple = true;
				Options.bTwoSidedDistanceField = true;
				const FString Name = Pair.Key == &CeilingBuilder ? FString::Printf(TEXT("SM_Grid_%03d_Ceiling"), GridIndex) : FString::Printf(TEXT("SM_Grid_%03d_Enclosure"), GridIndex);
				*Pair.Value = Context.CreateStaticMesh(Context.GetContentPath(TEXT("Interiors/Grids/Enclosure")), Name,
					MoveTemp(Description), Materials, Slots, Options);
			}
		};
		const FLBAVaultShape VaultShape{ Settings.VaultRadius, Settings.VaultRise, Settings.VaultNoise };
		// A finish's tiling texture and material (T_Grid_NNN_Finish): the art of curved surfaces (roofs, a hollow's walls).
		// Roofs are pulled toward the library's ceiling color (as its flat ceilings are: lighter than the walls, in their hue).
		auto MakeFinishMaterial = [&](const FCrownFinish& Finish, bool bRoof) -> UMaterialInterface*
		{
			constexpr float RoofLightening = 0.4f;
			int32 Width = 0, Height = 0;
			const FColor Ceiling = LibraryEnclosure ? LibraryEnclosure->CeilingColor : FColor(150, 140, 125);
			const TArray<FColor> Pixels = RenderFinishTile(*Atlas, *Library, Forbidden, Finish, Ceiling, bRoof ? RoofLightening : 0.f, Width, Height);
			const FString EnclosureFolder = Context.GetContentPath(TEXT("Interiors/Grids/Enclosure"));
			const FString Name = FString::Printf(TEXT("T_Grid_%03d_%s"), GridIndex, bRoof ? TEXT("Roof") : TEXT("Finish"));
			if (FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureTiles")))
			{
				// -LBA2EnclosureTiles: the tile as a PPM image (Saved/LookDev/enclosure/tiles), to look at.
				TArray<uint8> Image;
				const FString Header = FString::Printf(TEXT("P6 %d %d 255\n"), Width, Height);
				Image.Append(reinterpret_cast<const uint8*>(TCHAR_TO_ANSI(*Header)), Header.Len());
				for (const FColor& Pixel : Pixels)
				{
					Image.Append({ Pixel.R, Pixel.G, Pixel.B });
				}
				FFileHelper::SaveArrayToFile(Image, *(FPaths::ProjectSavedDir() / TEXT("LookDev/enclosure/tiles") / (Name + TEXT(".ppm"))));
			}
			UTexture2D* Texture = Context.CreateTexture(EnclosureFolder, Name, Width, Height, Pixels, true);
			UMaterialInterface* Result = Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Texture, EnclosureFolder);
			// Shaded by the vertex alpha: a canyon falls into darkness (EmitHollowWalls, its floor).
			if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Result))
			{
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("BakedShading"), 1.f);
				UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			}
			return Result;
		};
		if (Settings.bEnclose && Parts.Hollow)
		{
			// A hollow in the rock (FLBAHollow): one roof over the walkable floors, rock walls up to it from what bounds them.
			const FLBAHollow& Hollow = *Parts.Hollow;
			UE_LOG(LogLBA2, Display, TEXT("Grid %d %s"), GridIndex, *Hollow.Describe());
			if (!Hollow.IsEmpty())
			{
				// The rock: the rows drawn on top of what bounds the hollow (its rims, the crown of its walls), a few mixed.
				TMap<FIntPoint, int32> Votes;
				for (int32 Index = 0; Index < FLBARoomPlan::SizeXZ * FLBARoomPlan::SizeXZ; ++Index)
				{
					const FIntPoint Column(Index % FLBARoomPlan::SizeXZ, Index / FLBARoomPlan::SizeXZ);
					for (int32 Side = 0; Side < 4 && Hollow.IsUnderRoof(Column); ++Side)
					{
						const FIntPoint Next = Column + FLBARoomPlan::Sides[Side];
						const int32 Top = Hollow.GetDrawnTop(Next);
						for (int32 Y = Top; Y >= FMath::Max(Top - 1, 0) && !Hollow.IsUnderRoof(Next) && Top != INDEX_NONE; --Y)
						{
							const TStaticArray<float, 3>* Coverage = Atlas->FaceCoverage.Find(Query.BrickAt(Next.X, Y, Next.Y));
							const FLBAGridCell& Cell = Grid.Get(Next.X, Y, Next.Y);
							if (IsDrawn(Next.X, Y, Next.Y) && Coverage && FMath::Max((*Coverage)[1], (*Coverage)[2]) >= OpaqueCoverage)
							{
								++Votes.FindOrAdd(FIntPoint(Cell.Layout, Library->Layouts[Cell.Layout].GetBlockCoords(Cell.Block).Y));
							}
						}
					}
				}
				const FCrownFinish Rock = FCrownFinish::FromVotes(Votes);
				FString Rows;
				for (const FIntPoint& Row : Rock.Rows)
				{
					Rows += FString::Printf(TEXT(" L%d/%d"), Row.X, Row.Y);
				}
				UE_LOG(LogLBA2, Log, TEXT("HOLLOW grid %d rock:%s"), GridIndex, *Rows);

				const bool bDebugParts = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebug"));
				const bool bPlain = bDebugParts || FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebugArt"));
				FLBAMeshBuilder EnclosureBuilder, CeilingBuilder;
				auto Height = [&Hollow](float X, float Z) { return Hollow.GetRoof(X, Z); };
				UMaterialInterface* Material = bPlain ? Atlas->FillMaterial : MakeFinishMaterial(Rock, false);
				UMaterialInterface* RoofMaterial = bPlain ? Atlas->FillMaterial : MakeFinishMaterial(Rock, true);
				const FVector4f Neutral(0.5f, 0.5f, 0.5f, 0.5f);
				const FVector4f RoofColor = bDebugParts ? LBABodyConverter::ToVertexColor(FColor(40, 200, 60), 0.5f) : bPlain ? LBABodyConverter::ToVertexColor(FColor(255, 0, 255), 0.5f) : Neutral;
				const FVector4f WallColor = bDebugParts ? LBABodyConverter::ToVertexColor(FColor(220, 40, 40), 0.5f) : RoofColor;
				EmitRoof(Hollow.GetVault(), Height, CeilingBuilder, CeilingBuilder.GetSection(TEXT("Roof"), RoofMaterial), RoofColor, Scale);
				EmitHollowWalls(Hollow, Height, DoorZones.FindRef(GridIndex), EnclosureBuilder, EnclosureBuilder.GetSection(TEXT("Finish"), Material),
					EnclosureBuilder.GetSection(TEXT("BrickFill"), Atlas->FillMaterial), WallColor, Scale);
				// The canyons' floors, deep under the ledges.
				EmitHeightField([&Hollow](const FIntPoint& Column) { return Hollow.IsInCanyon(Column); }, FLBAHollow::Margin,
					[&Hollow](float X, float Z) { return Hollow.GetCanyonFloor(X, Z); }, true, EnclosureBuilder, EnclosureBuilder.GetSection(TEXT("Finish"), Material), WallColor, Scale,
					[&Hollow](const FVector3f& At) { return Hollow.GetCanyonLight(At.X, At.Y, At.Z); });
				CreateEnclosureMeshes(EnclosureBuilder, CeilingBuilder);
				UE_LOG(LogLBA2, Display, TEXT("Grid %d enclosure: hollow, %d triangles"), GridIndex, EnclosureBuilder.NumTriangles() + CeilingBuilder.NumTriangles());

				// One bounce light, from halfway up to the roof's lowest spring (lighting the roof as its lit floors would, not the
				// undersides of what stands low under it; at the spring itself it glared on a cistern's vault); the hollow for the
				// plugin's Tools: "C x z 0 floor top" per floor column.
				FRoomBox& Box = RoomBoxes.AddDefaulted_GetRef();
				int32 FloorCount = 0;
				FString Dump = Hollow.Describe();
				for (int32 Index = 0; Index < FLBARoomPlan::SizeXZ * FLBARoomPlan::SizeXZ; ++Index)
				{
					const FIntPoint Column(Index % FLBARoomPlan::SizeXZ, Index / FLBARoomPlan::SizeXZ);
					const int32 Floor = Hollow.GetFloor(Column);
					if (!Hollow.IsUnderRoof(Column) || Floor == INDEX_NONE)
					{
						continue;
					}
					const int32 Top = FMath::FloorToInt(Hollow.GetRoof(Column.X + 0.5f, Column.Y + 0.5f));
					Dump += FString::Printf(TEXT("C %d %d 0 %d %d\n"), Column.X, Column.Y, Floor, Top);
					Box.Min = Box.Min.ComponentMin(FIntVector(Column.X, FMath::FloorToInt(Hollow.GetSpring(Column.X + 0.5f, Column.Y + 0.5f)) - Settings.RoomHeight / 2, Column.Y));
					Box.Max = Box.Max.ComponentMax(FIntVector(Column.X, Top - 1, Column.Y));
					if (const TStaticArray<FColor, 3>* Colors = Atlas->FaceColors.Find(Query.BrickAt(Column.X, Floor, Column.Y)))
					{
						Box.Floor += FLinearColor::FromSRGBColor((*Colors)[0]);
						++FloorCount;
					}
				}
				Box.Floor /= float(FMath::Max(FloorCount, 1));
				// Over the canyons too: their roof and the top of their far side catch the light, their depths stay dark.
				for (int32 Z = -FLBAHollow::Margin; Z < FLBARoomPlan::SizeXZ + FLBAHollow::Margin; ++Z)
				{
					for (int32 X = -FLBAHollow::Margin; X < FLBARoomPlan::SizeXZ + FLBAHollow::Margin; ++X)
					{
						if (Hollow.IsInCanyon(FIntPoint(X, Z)) && Box.Min.X <= Box.Max.X)
						{
							Box.Min.X = FMath::Min(Box.Min.X, X);
							Box.Min.Z = FMath::Min(Box.Min.Z, Z);
							Box.Max.X = FMath::Max(Box.Max.X, X);
							Box.Max.Z = FMath::Max(Box.Max.Z, Z);
						}
					}
				}
				FFileHelper::SaveStringToFile(Dump, *(FPaths::ProjectSavedDir() / FString::Printf(TEXT("LookDev/enclosure/cells/grid%03d.txt"), GridIndex)));
			}
		}
		else if (Settings.bEnclose && Parts.Rooms)
		{
			const FLBARoomPlan& Plan = *Parts.Rooms;
			UE_LOG(LogLBA2, Display, TEXT("Grid %d plan:\n%s"), GridIndex, *Plan.Describe());
			const TArray<FLBAPlanRoom>& Rooms = Plan.GetRooms();
			const TArray<FLBAPlanSpace>& Spaces = Plan.GetSpaces();
			const TSet<FIntPoint>& BeyondExits = Parts.BeyondExits;
			const TArray<FAirColumn>& Air = GridPlan.Air;

			FEnclosureArt Art;
			Art.Library = Library;
			Art.ForbiddenBrick = Forbidden;
			Art.bCeiling = Settings.bCeiling;
			if (LibraryEnclosure)
			{
				Art.CeilingColor = LBABodyConverter::ToVertexColor(LibraryEnclosure->CeilingColor, 0.5f);
				Art.CeilingMaterial = LibraryEnclosure->CeilingMaterial.LoadSynchronous();
			}

			// Wallpapers: at each height over the floor, the wall layout row drawn most there on the back walls the
			// original drew (wall panels only: one cell thin, opaque on the side they face; no frames), per space and
			// orientation of its walls (else its other orientation's, else the building's), and over the whole grid for
			// the building's default.
			using FVotes = TMap<FIntPoint, int32>;
			using FProfileVotes = TArray<FVotes>;
			TArray<TStaticArray<FProfileVotes, 2>> Votes;
			Votes.SetNum(Spaces.Num());
			TStaticArray<FProfileVotes, 2> GridVotes;
			for (const FAirColumn& At : Air)
			{
				const FLBAPlanLevel* Level = At.Room != INDEX_NONE ? Plan.FindLevel(At.Level) : nullptr;
				for (int32 Side = 0; Side < 4 && Level; Side += 2)
				{
					const FIntPoint Wall = At.Column + FLBARoomPlan::Sides[Side];
					if (Level->Get(Wall) != ELBAColumn::Wall)
					{
						continue;
					}
					const int32 Axis = Side / 2;
					const int32 Slot = Axis == 0 ? 1 : 2;
					for (int32 Y = At.Floor + 1; Y < At.Top; ++Y)
					{
						if (!IsDrawn(Wall.X, Y, Wall.Y))
						{
							continue;
						}
						const FLBAGridCell& Cell = Grid.Get(Wall.X, Y, Wall.Y);
						const FLBALayout& Layout = Library->Layouts[Cell.Layout];
						const bool bPanel = Axis == 0 ? Layout.SizeX == 1 && Layout.SizeZ > 1 : Layout.SizeZ == 1 && Layout.SizeX > 1;
						const TStaticArray<float, 3>* Coverage = Atlas->FaceCoverage.Find(Query.BrickAt(Wall.X, Y, Wall.Y));
						if (!bPanel || !Coverage || (*Coverage)[Slot] < OpaqueCoverage)
						{
							continue;
						}
						const FIntPoint Key(Cell.Layout, Layout.GetBlockCoords(Cell.Block).Y);
						const int32 Above = Y - (At.Floor + 1);
						for (FProfileVotes* Profile : { &Votes[At.Space][Axis], &GridVotes[Axis] })
						{
							if (Profile->Num() <= Above)
							{
								Profile->SetNum(Above + 1);
							}
							++(*Profile)[Above].FindOrAdd(Key);
						}
					}
				}
			}
			auto MostVoted = [](const FVotes& Candidates)
			{
				FIntPoint Best(INDEX_NONE, 0);
				int32 BestCount = 0;
				for (const TPair<FIntPoint, int32>& Pair : Candidates)
				{
					if (Pair.Value > BestCount || (Pair.Value == BestCount && (Pair.Key.X < Best.X || (Pair.Key.X == Best.X && Pair.Key.Y < Best.Y))))
					{
						Best = Pair.Key;
						BestCount = Pair.Value;
					}
				}
				return Best;
			};
			// The rows of a profile: at each height, the most drawn row of the layout drawn most over all heights (the wall,
			// not a window whose foot shows where the wall's is hidden behind furniture), else the most drawn row; heights
			// nobody voted for (hidden) take the row under them, else above.
			auto MakeWallpaper = [&MostVoted, Library](const FProfileVotes& Profile)
			{
				TMap<int32, int32> LayoutVotes;
				for (const FVotes& AtHeight : Profile)
				{
					for (const TPair<FIntPoint, int32>& Pair : AtHeight)
					{
						LayoutVotes.FindOrAdd(Pair.Key.X) += Pair.Value;
					}
				}
				int32 Main = INDEX_NONE;
				for (const TPair<int32, int32>& Pair : LayoutVotes)
				{
					if (Main == INDEX_NONE || Pair.Value > LayoutVotes[Main] || (Pair.Value == LayoutVotes[Main] && Pair.Key < Main))
					{
						Main = Pair.Key;
					}
				}
				FWallpaper Wallpaper;
				for (const FVotes& AtHeight : Profile)
				{
					const FVotes OfMain = AtHeight.FilterByPredicate([Main](const TPair<FIntPoint, int32>& Pair) { return Pair.Key.X == Main; });
					const FIntPoint Best = MostVoted(OfMain.Num() > 0 ? OfMain : AtHeight);
					const FIntPoint Row = Best.X != INDEX_NONE || Wallpaper.Rows.Num() == 0 ? Best : Wallpaper.Rows.Last();
					Wallpaper.Rows.Add(Row);
				}
				for (int32 Index = Wallpaper.Rows.Num() - 2; Index >= 0; --Index)
				{
					if (Wallpaper.Rows[Index].X == INDEX_NONE)
					{
						Wallpaper.Rows[Index] = Wallpaper.Rows[Index + 1];
					}
				}
				// Walls the original only drew up to their dado (partitions it cut low) go on as their layout does: a
				// profile shorter than its top layout gets the rest of that layout's rows.
				if (Wallpaper.Rows.Num() > 0 && Library->Layouts.IsValidIndex(Wallpaper.Rows.Last().X))
				{
					const FIntPoint Last = Wallpaper.Rows.Last();
					const int32 LayoutHeight = Library->Layouts[Last.X].SizeY;
					if (Wallpaper.Rows.Num() < LayoutHeight)
					{
						for (int32 Row = Last.Y + 1; Row < LayoutHeight; ++Row)
						{
							Wallpaper.Rows.Add(FIntPoint(Last.X, Row));
						}
					}
				}
				return Wallpaper;
			};
			// A space's (its own walls, else the building's), or the building's (Space null).
			auto Choose = [&MakeWallpaper, &GridVotes](const TStaticArray<FProfileVotes, 2>* Space, int32 Axis)
			{
				const TStaticArray<FProfileVotes, 2>* Candidates[2] = { Space, &GridVotes };
				for (const TStaticArray<FProfileVotes, 2>* Candidate : Candidates)
				{
					for (const int32 Which : { Axis, 1 - Axis })
					{
						if (Candidate && (*Candidate)[Which].Num() > 0)
						{
							return MakeWallpaper((*Candidate)[Which]);
						}
					}
				}
				return FWallpaper();
			};
			const int32 Override = LibraryEnclosure && Library->Layouts.IsValidIndex(LibraryEnclosure->WallpaperLayout) ? LibraryEnclosure->WallpaperLayout : INDEX_NONE;
			auto Describe = [](const TStaticArray<FWallpaper, 2>& Wallpapers)
			{
				FString Chosen;
				for (int32 Axis = 0; Axis < 2; ++Axis)
				{
					Chosen += Axis == 0 ? TEXT(" X") : TEXT(" Z");
					for (const FIntPoint& Row : Wallpapers[Axis].Rows)
					{
						Chosen += FString::Printf(TEXT(" L%d/%d"), Row.X, Row.Y);
					}
				}
				return Chosen;
			};
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				if (Override != INDEX_NONE)
				{
					for (int32 Row = 0; Row < Library->Layouts[Override].SizeY; ++Row)
					{
						Art.DefaultWallpapers[Axis].Rows.Add(FIntPoint(Override, Row));
					}
				}
				else
				{
					Art.DefaultWallpapers[Axis] = Choose(nullptr, Axis);
				}
			}
			UE_LOG(LogLBA2, Log, TEXT("WALLPAPER grid %d default:%s"), GridIndex, *Describe(Art.DefaultWallpapers));
			auto LayoutsOf = [](const TStaticArray<FWallpaper, 2>& Wallpapers)
			{
				TSet<int32> Layouts;
				for (const FWallpaper& Wallpaper : Wallpapers)
				{
					for (const FIntPoint& Row : Wallpaper.Rows)
					{
						Layouts.Add(Row.X);
					}
				}
				return Layouts;
			};
			const TSet<int32> BuildingLayouts = LayoutsOf(Art.DefaultWallpapers);
			for (int32 SpaceIndex = 0; SpaceIndex < Spaces.Num(); ++SpaceIndex)
			{
				TStaticArray<FWallpaper, 2>& Wallpapers = Art.Wallpapers.AddDefaulted_GetRef();
				for (int32 Axis = 0; Axis < 2; ++Axis)
				{
					Wallpapers[Axis] = Override != INDEX_NONE ? Art.DefaultWallpapers[Axis] : Choose(&Votes[SpaceIndex], Axis);
				}
				// A room with a finish of its own (its back walls all one wallpaper the building's walls don't use, like the
				// nursery being papered anew) has it all round, on its camera-side walls too.
				const TSet<int32> Own = LayoutsOf(Wallpapers);
				Art.OwnFinish.Add(Override == INDEX_NONE && Own.Num() == 1 && BuildingLayouts.Intersect(Own).Num() == 0);
				UE_LOG(LogLBA2, Log, TEXT("WALLPAPER grid %d space %d:%s%s"), GridIndex, SpaceIndex, *Describe(Wallpapers), Art.OwnFinish.Last() ? TEXT(" (own finish)") : TEXT(""));
			}

			// Walls where no wallpaper has art take their space's floor color; bounce lights too.
			RoomBoxes.SetNum(Spaces.Num());
			TArray<int32> FloorCounts;
			FloorCounts.SetNumZeroed(Spaces.Num());
			for (const FAirColumn& At : Air)
			{
				if (At.Room == INDEX_NONE)
				{
					continue;
				}
				FRoomBox& Box = RoomBoxes[At.Space];
				if (Plan.FindLevel(At.Level)->Get(At.Column) == ELBAColumn::Floor)
				{
					if (const TStaticArray<FColor, 3>* Colors = Atlas->FaceColors.Find(Query.BrickAt(At.Column.X, At.Floor, At.Column.Y)))
					{
						Box.Floor += FLinearColor::FromSRGBColor((*Colors)[0]);
						++FloorCounts[At.Space];
					}
				}
				Box.Min = Box.Min.ComponentMin(FIntVector(At.Column.X, At.Floor + 1, At.Column.Y));
				Box.Max = Box.Max.ComponentMax(FIntVector(At.Column.X, At.Top - 1, At.Column.Y));
			}
			for (int32 SpaceIndex = 0; SpaceIndex < Spaces.Num(); ++SpaceIndex)
			{
				FRoomBox& Box = RoomBoxes[SpaceIndex];
				Box.Floor /= float(FMath::Max(FloorCounts[SpaceIndex], 1));
				Art.SpaceColors.Add(LBABodyConverter::ToVertexColor(FloorCounts[SpaceIndex] > 0 ? Box.Floor.ToFColor(true) : FColor(128, 128, 128), 0.35f));
			}

			FLBAMeshBuilder EnclosureBuilder, CeilingBuilder;
			TSet<FIntVector> Covered;
			auto LayoutRowAt = [&Grid, Library](int32 X, int32 Y, int32 Z)
			{
				const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
				return Library->Layouts.IsValidIndex(Cell.Layout) ? FIntPoint(Cell.Layout, Library->Layouts[Cell.Layout].GetBlockCoords(Cell.Block).Y) : FIntPoint(INDEX_NONE, 0);
			};
			auto IsSlope = [&Query](int32 X, int32 Y, int32 Z) { return Query.ShapeAt(X, Y, Z) > 1; };
			EmitEnclosure(Plan, Air, BeyondExits, IsDrawn, IsSlope, LayoutRowAt, *Atlas, Art, VaultShape, MakeFinishMaterial, EnclosureBuilder, CeilingBuilder, Scale, Covered, CoveredFaces);

			// Frames come off whole: an instance any cell of which the enclosure covers (no half curves left).
			TSet<FIntVector4> RemovedInstances;
			for (const FIntVector& Cell : Covered)
			{
				if (Query.BrickAt(Cell.X, Cell.Y, Cell.Z) != 0 && IsFrame(Cell.X, Cell.Y, Cell.Z))
				{
					RemovedInstances.Add(InstanceOf(Cell.X, Cell.Y, Cell.Z));
				}
			}
			for (int32 Z = 0; Z < LBA::GridSizeXZ && RemovedInstances.Num() > 0; ++Z)
			{
				for (int32 X = 0; X < LBA::GridSizeXZ; ++X)
				{
					for (int32 Y = 0; Y < LBA::GridSizeY; ++Y)
					{
						if (Query.BrickAt(X, Y, Z) != 0 && RemovedInstances.Contains(InstanceOf(X, Y, Z)))
						{
							RemovedFrames.Add(FIntVector(X, Y, Z));
						}
					}
				}
			}

			CreateEnclosureMeshes(EnclosureBuilder, CeilingBuilder);
			UE_LOG(LogLBA2, Display, TEXT("Grid %d enclosure: %d rooms, %d spaces, %d triangles, %d frame cells removed"), GridIndex, Rooms.Num(), Spaces.Num(),
				EnclosureBuilder.NumTriangles() + CeilingBuilder.NumTriangles(), RemovedFrames.Num());
			{
				// The plan, for the plugin's Tools (side_poses.py, raycast_leaks.py): the plan's text, then the air: "C x z room
				// floor top" per room column, "K x z connector floor top" per connector column, "W x z level rule" per wall.
				FString Dump = Plan.Describe();
				for (const FAirColumn& At : Air)
				{
					Dump += FString::Printf(TEXT("%s %d %d %d %d %d\n"), At.Room != INDEX_NONE ? TEXT("C") : TEXT("K"), At.Column.X, At.Column.Y,
						At.Room != INDEX_NONE ? At.Room : At.Connector, At.Floor, At.Top);
				}
				for (const int32 Height : Plan.GetLevels())
				{
					const FLBAPlanLevel* Level = Plan.FindLevel(Height);
					for (int32 Index = 0; Index < Level->Columns.Num(); ++Index)
					{
						if (Level->Columns[Index] == ELBAColumn::Wall)
						{
							Dump += FString::Printf(TEXT("W %d %d %d %d\n"), Index % LBA::GridSizeXZ, Index / LBA::GridSizeXZ, Height, int32(Level->Rules[Index]));
						}
					}
				}
				FFileHelper::SaveStringToFile(Dump, *(FPaths::ProjectSavedDir() / FString::Printf(TEXT("LookDev/enclosure/cells/grid%03d.txt"), GridIndex)));
			}
		}
	}
	FCellQuery VisualQuery = Query;
	if (Remade.HiddenCells.Num() > 0 || RemovedFrames.Num() > 0)
	{
		const TSet<FIntVector>* Hidden = &Remade.HiddenCells;
		const TSet<FIntVector>* Frames = &RemovedFrames;
		VisualQuery.Brick = [BaseBrick = Query.Brick, Hidden, Frames](int32 X, int32 Y, int32 Z)
		{
			const FIntVector Cell(X, Y, Z);
			return Hidden->Contains(Cell) || Frames->Contains(Cell) ? 0 : BaseBrick(X, Y, Z);
		};
	}

	// The removed frames and the faces the enclosure covers, for the Original look (its open rooms keep them).
	UStaticMesh* FramesMesh = nullptr;
	if (RemovedFrames.Num() > 0 || CoveredFaces.Num() > 0)
	{
		FLBAMeshBuilder FramesBuilder;
		EmitVisual(Query, *Atlas, FramesBuilder, FramesBuilder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas->Material), Scale,
			Context.GetProjectSettings().BackFaces, [&RemovedFrames, &CoveredFaces](const FIntVector& Cell, int32 FaceIndex)
			{
				return RemovedFrames.Contains(Cell) || CoveredFaces.Contains(FIntVector4(Cell.X, Cell.Y, Cell.Z, FaceIndex));
			});
		if (!FramesBuilder.IsEmpty())
		{
			FMeshDescription Description;
			TArray<UMaterialInterface*> Materials;
			TArray<FName> Slots;
			FramesBuilder.BuildStatic(Description, Materials, Slots);
			FramesMesh = Context.CreateStaticMesh(Context.GetContentPath(TEXT("Interiors/Grids/Enclosure")), FString::Printf(TEXT("SM_Grid_%03d_Frames"), GridIndex),
				MoveTemp(Description), Materials, Slots, FLBAStaticMeshOptions());
		}
	}

	FLBAMeshBuilder Builder;
	EmitVisual(VisualQuery, *Atlas, Builder, Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas->Material), Scale,
		Context.GetProjectSettings().BackFaces, [&CoveredFaces](const FIntVector& Cell, int32 FaceIndex)
		{
			return !CoveredFaces.Contains(FIntVector4(Cell.X, Cell.Y, Cell.Z, FaceIndex));
		});
	if (Builder.IsEmpty())
	{
		return false;
	}
	FMeshDescription Description;
	TArray<UMaterialInterface*> Materials;
	TArray<FName> Slots;
	Builder.BuildStatic(Description, Materials, Slots);
	FLBAStaticMeshOptions Options;
	Options.ComplexCollisionMesh = CollisionMesh;
	UStaticMesh* Mesh = Context.CreateStaticMesh(Folder, MeshName, MoveTemp(Description), Materials, Slots, Options);
	Context.Flush();

	if (!Context.GetProjectSettings().bCreateInteriorLevels || !Mesh)
	{
		return true;
	}

	UWorld* World = Context.BeginLevel(Folder / TEXT("Levels"), GetGridLevelName(GridIndex));
	if (!World)
	{
		return true;
	}
	if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform::Identity, TEXT("Grid"), TEXT("Grid")))
	{
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	}
	// The plan, drawn over the grid in the editor (Show > Developer > LBA2 Grid Plan).
	if (ALBAGridPlanActor* Actor = PlanAsset ? Context.SpawnGeneratedActor<ALBAGridPlanActor>(World, FTransform::Identity, TEXT("GridPlan"), TEXT("Grid")) : nullptr)
	{
		Actor->PlanView->Plan = PlanAsset;
	}
	if (EnclosureMesh)
	{
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform::Identity, TEXT("Enclosure"), TEXT("Grid")))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(EnclosureMesh);
			// Its faces look into the rooms: a camera outside sees in, yet the walls and ceilings still shade the rooms.
			Component->bCastShadowAsTwoSided = true;
			// Twinsen bumps into the raised walls; the camera never does.
			Component->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			Actor->Tags.Add(TEXT("LBAEnclosure"));
		}
	}
	if (CeilingMesh)
	{
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform::Identity, TEXT("EnclosureCeiling"), TEXT("Grid")))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(CeilingMesh);
			// The interior's light comes from overhead (the original's): through the ceilings it lights the rooms, which
			// bounce it onto their walls and ceilings, as a lit room would look.
			Component->SetCastShadow(false);
			Component->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			Actor->Tags.Add(TEXT("LBAEnclosure"));
		}
	}
	if (FramesMesh)
	{
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, FTransform::Identity, TEXT("EnclosureFrames"), TEXT("Grid")))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(FramesMesh);
			// Shown instead of the enclosure, with the Original look (not switched yet: the enclosure shows in both).
			Component->SetVisibility(false);
			// The grid's collision already covers them.
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Actor->Tags.Add(TEXT("LBAEnclosureFrames"));
		}
	}
	const FLBAEnvironmentPreset Preset = FLBAEnvironmentPreset::Interior();
	for (const FLBARemakeAssets::FPlacement& Placement : Remade.Placements)
	{
		// Models of the frames the enclosure removed (the remake's wooden rims of the cut walls) go with the frames.
		const bool bFrame = Placement.Cells.ContainsByPredicate([&RemovedFrames](const FIntVector& Cell) { return RemovedFrames.Contains(Cell); });
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, Placement.Transform, Placement.Label,
			bFrame ? TEXT("Grid/EnclosureFrames") : TEXT("Grid/RemakeModels")))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(Placement.Mesh);
			if (bFrame)
			{
				Component->SetVisibility(false);
				Actor->Tags.Add(TEXT("LBAEnclosureFrames"));
			}
			// The grid's brick collision already covers these.
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			for (const FLBARemakeAssets::FLamp& Lamp : Placement.Lamps)
			{
				const FLBADecorLight& Light = Lamp.Kind == FLBARemakeAssets::FLamp::EKind::Pool ? Preset.RemakePools
					: Lamp.Kind == FLBARemakeAssets::FLamp::EKind::Beacon ? Preset.RemakeBeacons
					: Lamp.Kind == FLBARemakeAssets::FLamp::EKind::Globe ? Preset.RemakeGlobes : Preset.RemakeLamps;
				if (Light.Light > 0.f)
				{
					LBAEnvironment::SpawnDecorLight(Context, Actor, Light, Lamp.Position, Lamp.Color, Lamp.SourceRadius);
				}
			}
		}
	}
	// A light from nearly overhead: brick floors get about the neutral light (the interior preset is calibrated for it).
	LBAEnvironment::Spawn(Context, World, Preset, FRotator(-84.0, 45.0, 0.0));

	// The enclosed rooms' bounce light: the overhead light goes through their ceilings (they cast no shadow) onto
	// their floors, which light the walls and ceilings back. Lumen alone gets little of it under a ceiling (the sky
	// light is shut out), so each room gets a soft fill from its floor up, the floor's color, as bright as the floor
	// would bounce: a panel of luminance albedo * E / pi over the room, times BounceCalibration (the palette shading
	// and the light units make the floor's light read darker than E: set by eye on lib 0's houses).
	constexpr float BounceCalibration = 20.f;
	const float Bounce = Context.GetProjectSettings().RoomBounceLight * BounceCalibration;
	float SunLux = 0.f;
	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		if (It->GetActorLabel() == TEXT("Sun"))
		{
			SunLux = It->GetLightComponent()->Intensity;
		}
	}
	for (int32 Space = 0; Space < RoomBoxes.Num() && CeilingMesh && Bounce > 0.f && SunLux > 0.f; ++Space)
	{
		const FRoomBox& Box = RoomBoxes[Space];
		if (Box.Min.X > Box.Max.X || Box.Max.Y - Box.Min.Y < 2)
		{
			continue;
		}
		const FVector Low = FVector(CornerPos(Box.Min.X, Box.Min.Y, Box.Min.Z, Scale));
		const FVector High = FVector(CornerPos(Box.Max.X + 1, Box.Max.Y + 1, Box.Max.Z + 1, Scale));
		const FVector Center((Low.X + High.X) * 0.5, (Low.Y + High.Y) * 0.5, Low.Z + 10.0);
		// Facing up: the light's +X turned to +Z, its width along Y, its height along X.
		if (ARectLight* Actor = Context.SpawnGeneratedActor<ARectLight>(World, FTransform(FRotator(90.0, 0.0, 0.0), Center),
			FString::Printf(TEXT("RoomBounce_%d"), Space), TEXT("Grid")))
		{
			URectLightComponent* Light = CastChecked<URectLightComponent>(Actor->GetLightComponent());
			const float Width = float(High.Y - Low.Y), Height = float(High.X - Low.X);
			// Bright floors (sand, tiles) would flood the room: real rooms' ceilings rarely get more than a fair bounce.
			const float Albedo = FMath::Clamp(Box.Floor.GetLuminance(), 0.05f, 0.35f);
			Light->SetMobility(EComponentMobility::Movable);
			Light->SetIntensityUnits(ELightUnits::Candelas);
			Light->SetIntensity(Bounce * Albedo * SunLux * (Width / 100.f) * (Height / 100.f) / UE_PI);
			Light->SetLightColor(Box.Floor / FMath::Max(Box.Floor.GetMax(), 1e-3f));
			Light->SetSourceWidth(Width);
			Light->SetSourceHeight(Height);
			Light->SetAttenuationRadius(FVector::Distance(Low, High) * 1.2f);
			Light->SetCastShadows(false);
			Light->SetVolumetricScatteringIntensity(0.f);
			Actor->Tags.Add(TEXT("LBAEnclosure"));
		}
	}

	// Fallback player start (scenes provide the real hero position): a low floor near the middle of the content.
	{
		FIntVector ContentMin(64, 25, 64), ContentMax(0, 0, 0);
		for (int32 Z = 0; Z < 64; ++Z)
		{
			for (int32 X = 0; X < 64; ++X)
			{
				for (int32 Y = 0; Y < 25; ++Y)
				{
					if (Query.Brick(X, Y, Z) != 0)
					{
						ContentMin = ContentMin.ComponentMin(FIntVector(X, Y, Z));
						ContentMax = ContentMax.ComponentMax(FIntVector(X, Y, Z));
					}
				}
			}
		}
		const FIntVector Mid = (ContentMin + ContentMax) / 2;
		FIntVector Best(Mid.X, 0, Mid.Z);
		int32 BestScore = TNumericLimits<int32>::Max();
		for (int32 Z = 0; Z < 64; ++Z)
		{
			for (int32 X = 0; X < 64; ++X)
			{
				for (int32 Y = 0; Y < 21; ++Y)
				{
					if (Query.Shape(X, Y, Z) == 1 && Query.Shape(X, Y + 1, Z) == 0 && Query.Shape(X, Y + 2, Z) == 0
						&& Query.Shape(X, Y + 3, Z) == 0 && Query.Shape(X, Y + 4, Z) == 0)
					{
						const int32 Score = FMath::Square(X - Mid.X) + FMath::Square(Z - Mid.Z) + FMath::Square(Y * 4);
						if (Score < BestScore)
						{
							BestScore = Score;
							Best = FIntVector(X, Y + 1, Z);
						}
					}
				}
			}
		}
		const FVector Location = FVector(CornerPos(Best.X + 0.5f, Best.Y, Best.Z + 0.5f, Scale)) + FVector(0, 0, 100);
		Context.SpawnGeneratedActor<APlayerStart>(World, FTransform(Location), TEXT("PlayerStart"), NAME_None);
	}
	Context.EndLevel(World);
	return true;
}

int32 FLBAIsoImporter::ImportAll()
{
	if (!LoadHeader())
	{
		return 0;
	}
	Remake = MakeUnique<FLBARemakeAssets>(Context);
	Remake->Initialize();
	LoadDoorZones();
	const ULBA2ImporterSettings& Settings = Context.GetSettings();

	TArray<int32> Grids;
	for (int32 Grid = 0; Grid < Header.GetNumGrids(); ++Grid)
	{
		if (Settings.GridFilter.Num() == 0 || Settings.GridFilter.Contains(Grid))
		{
			Grids.Add(Grid);
		}
	}

	if (Context.GetProjectSettings().bCreateLayoutKitMeshes)
	{
		TSet<int32> UsedLibraries;
		for (int32 Grid : Grids)
		{
			const TArray<uint8> Data = Bkg->GetEntry(Header.GridStart + Grid);
			if (Data.Num() > 0)
			{
				UsedLibraries.Add(Data[0]);
			}
		}
		for (int32 LibraryIndex : UsedLibraries)
		{
			if (Context.IsCancelled())
			{
				break;
			}
			Context.ReportProgress(FText::Format(LOCTEXT("Kit", "Building kit pieces of library {0}"), LibraryIndex));
			BuildKitMeshes(LibraryIndex);
		}
	}

	int32 Count = 0;
	for (int32 Grid : Grids)
	{
		if (Context.IsCancelled())
		{
			break;
		}
		Context.ReportProgress(FText::Format(LOCTEXT("Grid", "Importing interior grid {0}"), Grid));
		Count += ImportGrid(Grid) ? 1 : 0;
	}
	return Count;
}

#undef LOCTEXT_NAMESPACE
