#include "Import/LBAIsoImporter.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Formats/LBAScene.h"
#include "GameFramework/PlayerStart.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBAEnclosureCells.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBARemakeAssets.h"
#include "Import/LBAWallSurvey.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
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

	void EmitVisual(const FCellQuery& Query, const FLBAIsoImporter::FAtlas& Atlas, FLBAMeshBuilder& Builder, int32 Section, float Scale,
		ELBABackFaceMode BackFaces)
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
						if (Query.BrickAt(X + Face.Dir.X, Y + Face.Dir.Y, Z + Face.Dir.Z) != 0)
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
							V[K].UV = FVector2f((SourceCell->X + CellPad + ArtFace.Sprite[K].X) / Atlas.Width, (SourceCell->Y + CellPad + ArtFace.Sprite[K].Y) / Atlas.Height);
						}
						const int32 Target = bFill ? FillSection : Section;
						Builder.AddTriangleFacing(Target, V[0], V[1], V[2], Normal);
						Builder.AddTriangleFacing(Target, V[0], V[2], V[3], Normal);
					}
				}
			}
		}
	}

	/** What dresses an enclosure besides the grid's own art: the library's wall styles and the ceilings. */
	struct FEnclosureArt
	{
		const FLBAGrid* Grid = nullptr;
		const FLBALibrary* Library = nullptr;
		int32 ForbiddenBrick = 0;
		TConstArrayView<FLBAWallStyle> Styles;
		/** Per space: the style of the sides of its walls the original never drew (INDEX_NONE: none). */
		TArray<int32> RoomStyles;
		FVector4f CeilingColor = FVector4f(0.3f, 0.3f, 0.3f, 0.5f);
		UMaterialInterface* CeilingMaterial = nullptr;
		bool bWallTops = true;
		/** Debug: a line per wall face, "W x y z face kind layout layoutY wallColumnX wallColumnZ wallTop". */
		FString* FaceDump = nullptr;
	};

	/** The layout and the row of it at Index in a stack of wall segments. */
	bool SegmentCell(TConstArrayView<FLBAWallSegment> Segments, int32 Index, int32& OutLayout, int32& OutY)
	{
		for (const FLBAWallSegment& Segment : Segments)
		{
			if (Index < Segment.Height)
			{
				OutLayout = Segment.Layout;
				OutY = Segment.From + Index;
				return true;
			}
			Index -= Segment.Height;
		}
		return false;
	}

	int32 SegmentsHeight(TConstArrayView<FLBAWallSegment> Segments)
	{
		int32 Height = 0;
		for (const FLBAWallSegment& Segment : Segments)
		{
			Height += Segment.Height;
		}
		return Height;
	}

	/**
	 * Cell K (from the foot) of a wall Height cells tall built in a style: the foot, the body repeated, and the top in
	 * its last cells (bTops) when the wall is taller than its foot and top.
	 */
	bool StyleCell(const FLBAWallStyle& Style, int32 K, int32 Height, bool bTops, int32& OutLayout, int32& OutY, bool& bOutTop)
	{
		const int32 FootHeight = SegmentsHeight(Style.Foot), BodyHeight = SegmentsHeight(Style.Body), TopHeight = SegmentsHeight(Style.Top);
		bOutTop = bTops && TopHeight > 0 && Height - TopHeight > FootHeight && K >= Height - TopHeight;
		if (bOutTop)
		{
			return SegmentCell(Style.Top, K - (Height - TopHeight), OutLayout, OutY);
		}
		if (K < 0)
		{
			return false;
		}
		if (K < FootHeight)
		{
			return SegmentCell(Style.Foot, K, OutLayout, OutY);
		}
		return BodyHeight > 0 && SegmentCell(Style.Body, (K - FootHeight) % BodyHeight, OutLayout, OutY);
	}

	/** Where a layout's row Y sits in a style, counted from its foot. */
	int32 StyleIndexOf(const FLBAWallStyle& Style, int32 Layout, int32 Y)
	{
		int32 Index = 0;
		for (const TArray<FLBAWallSegment>* Part : { &Style.Foot, &Style.Body })
		{
			for (const FLBAWallSegment& Segment : *Part)
			{
				if (Segment.Contains(Layout, Y))
				{
					return Index + Y - Segment.From;
				}
				Index += Segment.Height;
			}
		}
		// In the top: what goes on above it is the body again.
		return Index - 1;
	}

	/**
	 * The enclosure's faces: each side of a filled cell that touches a room's air, facing into the room.
	 * - Walls on the sides the original drew (+X, +Z) and raised partitions go on with the wall they stand on, as the
	 *   original builds it: the rest of its own layout (the cut walls are cropped layouts), then its wall style (the
	 *   body repeated, the top under the ceiling).
	 * - Walls on the other sides (the inner sides of the walls facing the camera, the walls above doorways) take their
	 *   room's style, from the floor up.
	 * - Ceilings and risers take the library's ceiling color or material.
	 * Where no style applies, a wall repeats the first opaque brick down the wall it stands on, or takes its color.
	 */
	void EmitEnclosure(const FLBAEnclosureCells& Cells, const FLBARoomAnalysis& Rooms, const FCellQuery& Query,
		TFunctionRef<bool(int32, int32, int32)> IsDrawn, const FLBAIsoImporter::FAtlas& Atlas, const FEnclosureArt& Art, FLBAMeshBuilder& Builder, float Scale)
	{
		const FBrickFace* Faces = GetBrickFaces();
		TArray<FVector4f> RoomColors;
		for (const TArray<FIntVector>& Space : Rooms.GetSpaces())
		{
			FLinearColor Sum = FLinearColor::Black;
			int32 Count = 0;
			for (const FIntVector& Floor : Space)
			{
				if (const TStaticArray<FColor, 3>* Colors = Atlas.FaceColors.Find(Query.BrickAt(Floor.X, Floor.Y, Floor.Z)))
				{
					Sum += FLinearColor::FromSRGBColor((*Colors)[0]);
					++Count;
				}
			}
			const FColor Average = Count > 0 ? (Sum / float(Count)).ToFColor(true) : FColor(128, 128, 128);
			RoomColors.Add(LBABodyConverter::ToVertexColor(Average, 0.35f));
		}

		const int32 BrickSection = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas.Material);
		int32 FillSection = INDEX_NONE;
		const int32 CeilingSection = Art.CeilingMaterial ? Builder.GetSection(TEXT("Ceiling"), Art.CeilingMaterial) : INDEX_NONE;
		// Backings sit just behind the wall's face, so the grid's own back face wins where it has one; the art under a
		// see-through top (a cap) as far behind it.
		constexpr float BackingInset = 1.f;
		// Fraction of the way to its center each corner of a face's art is sampled from (about 2 pixels).
		constexpr float ArtInset = 0.1f;
		// A top with less art than this on its face is left out (a thin trim).
		constexpr float MinTopCoverage = 0.3f;
		// -LBA2EnclosureDebug: flat colors by part (walls red, backings gray, ceilings green, risers blue, above doorways
		// yellow, partitions and stubs orange).
		const bool bDebugParts = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebug"));
		// -LBA2EnclosureDebugArt: walls in flat colors by where their art comes from: their own wall's style green, their
		// room's style cyan, a style without art for them yellow, the first opaque brick down the wall orange, none magenta.
		const bool bDebugArt = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebugArt"));
		static const FColor PartColors[] = { FColor(220, 40, 40), FColor(140, 140, 140), FColor(40, 200, 60), FColor(40, 80, 230), FColor(240, 210, 40), FColor(240, 130, 30) };

		// The brick of a layout cell in a wall, from the instance of the layout whose origin column is Origin.
		auto LayoutBrick = [&Art](int32 LayoutIndex, int32 Y, const FIntPoint& Origin, const FIntPoint& Column)
		{
			if (!Art.Library || !Art.Library->Layouts.IsValidIndex(LayoutIndex))
			{
				return 0;
			}
			const FLBALayout& Layout = Art.Library->Layouts[LayoutIndex];
			const int32 X = ((Column.X - Origin.X) % Layout.SizeX + Layout.SizeX) % Layout.SizeX;
			const int32 Z = ((Column.Y - Origin.Y) % Layout.SizeZ + Layout.SizeZ) % Layout.SizeZ;
			const int32 Index = Z * Layout.SizeX * Layout.SizeY + FMath::Clamp(Y, 0, Layout.SizeY - 1) * Layout.SizeX + X;
			const int32 Brick = Layout.Blocks.IsValidIndex(Index) ? int32(Layout.Blocks[Index].Brick) : 0;
			return Brick == Art.ForbiddenBrick ? 0 : Brick;
		};
		// A brick's art for a wall face, with at least MinCoverage of it opaque: the side the layout faces (FacingSlot: a
		// wall panel only has art on the side the iso camera saw, its narrow sides show a plain edge), else the face's
		// own side, else the other.
		auto WallArt = [&Atlas](int32 Brick, int32 FaceIndex, float MinCoverage, int32& OutArtFace, int32 FacingSlot = 0) -> const FIntPoint*
		{
			const TStaticArray<float, 3>* Coverage = Brick ? Atlas.FaceCoverage.Find(Brick) : nullptr;
			if (!Coverage)
			{
				return nullptr;
			}
			const int32 Slot = ColorSlotOf(FrontFaceOf(FaceIndex));
			const int32 Other = 3 - Slot;
			if (FacingSlot != 0 && (*Coverage)[FacingSlot] >= MinCoverage)
			{
				OutArtFace = FacingSlot == 1 ? 2 : 4;
			}
			else if ((*Coverage)[Slot] >= MinCoverage && (*Coverage)[Slot] >= (*Coverage)[Other] * 0.5f)
			{
				OutArtFace = FrontFaceOf(FaceIndex);
			}
			else if ((*Coverage)[Other] >= MinCoverage)
			{
				OutArtFace = Other == 1 ? 2 : 4;
			}
			else
			{
				return nullptr;
			}
			return Atlas.Cells.Find(Brick);
		};
		// The side a layout faces when it is a wall panel (one cell thin, wider the other way): 1 for +X, 2 for +Z, else 0.
		auto FacingSlotOf = [&Art](int32 LayoutIndex)
		{
			if (!Art.Library || !Art.Library->Layouts.IsValidIndex(LayoutIndex))
			{
				return 0;
			}
			const FLBALayout& Layout = Art.Library->Layouts[LayoutIndex];
			return Layout.SizeX == 1 && Layout.SizeZ > 1 ? 1 : Layout.SizeZ == 1 && Layout.SizeX > 1 ? 2 : 0;
		};

		auto EmitQuad = [&](const FIntVector& Cell, const FBrickFace& Face, int32 Section, const FIntPoint* SourceCell, int32 ArtFaceIndex,
			const FVector4f& Color, float Inset)
		{
			const FBrickFace& ArtFace = Faces[ArtFaceIndex];
			// The art is sampled a little inside the face: its edge pixels are often transparent, and repeated up a
			// wall they show as seams.
			const FVector2f ArtCenter = (ArtFace.Sprite[0] + ArtFace.Sprite[1] + ArtFace.Sprite[2] + ArtFace.Sprite[3]) * 0.25f;
			const FVector3f Normal = FVector3f(LBA::DirToUnreal(Face.Dir.X, Face.Dir.Y, Face.Dir.Z));
			FLBAVertex V[4];
			for (int32 K = 0; K < 4; ++K)
			{
				const FVector3f Corner(Cell.X + Face.Corners[K].X, Cell.Y + Face.Corners[K].Y, Cell.Z + Face.Corners[K].Z);
				V[K].Position = CornerPos(Corner.X, Corner.Y, Corner.Z, Scale) - Normal * Inset;
				V[K].Normal = Normal;
				V[K].Color = SourceCell ? FVector4f(0.5f, 0.5f, 0.5f, 0.5f) : Color;
				if (SourceCell)
				{
					const FVector2f Sprite = FMath::Lerp(ArtFace.Sprite[K], ArtCenter, ArtInset);
					V[K].UV = FVector2f((SourceCell->X + CellPad + Sprite.X) / Atlas.Width, (SourceCell->Y + CellPad + Sprite.Y) / Atlas.Height);
				}
				else
				{
					// Ceiling materials tile once per cell.
					V[K].UV = Face.Dir.Y != 0 ? FVector2f(Corner.X, Corner.Z) : FVector2f(Face.Dir.X != 0 ? Corner.Z : Corner.X, Corner.Y);
				}
			}
			if (Section == INDEX_NONE)
			{
				if (FillSection == INDEX_NONE)
				{
					FillSection = Builder.GetSection(TEXT("BrickFill"), Atlas.FillMaterial);
				}
				Section = FillSection;
			}
			Builder.AddTriangleFacing(Section, V[0], V[1], V[2], Normal);
			Builder.AddTriangleFacing(Section, V[0], V[2], V[3], Normal);
		};

		// Wall faces by where their art comes from (see -LBA2EnclosureDebugArt).
		int32 Stats[5] = {};
		static const FColor ArtColors[] = { FColor(40, 200, 60), FColor(40, 200, 220), FColor(240, 210, 40), FColor(240, 130, 30), FColor(255, 0, 255) };
		auto EmitCell = [&](const FIntVector& Cell, bool bBacking)
		{
			const ELBAEnclosurePart Part = Cells.Parts.FindRef(Cell);
			for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
			{
				const FBrickFace& Face = Faces[FaceIndex];
				const FIntVector Next(Cell.X + Face.Dir.X, Cell.Y + Face.Dir.Y, Cell.Z + Face.Dir.Z);
				const int32* Space = Cells.Air.Find(Next);
				const bool bFacesAir = Space != nullptr;
				if (!Space && IsDrawn(Next.X, Next.Y, Next.Z) && !Cells.Backing.Contains(Next))
				{
					// A brick touching the room only has the faces the iso camera saw: seen through its other sides
					// (the top of a partition under the ceiling), the enclosure behind it must be drawn (backed bricks
					// are already closed by their backing).
					for (int32 Side = 0; Side < 6 && !Space; ++Side)
					{
						Space = Cells.Air.Find(Next + FIntVector(Faces[Side].Dir.X, Faces[Side].Dir.Y, Faces[Side].Dir.Z));
					}
				}
				if (!Space)
				{
					continue;
				}
				const float Inset = bBacking ? BackingInset : 0.f;

				if (bDebugParts)
				{
					EmitQuad(Cell, Face, INDEX_NONE, nullptr, FaceIndex, LBABodyConverter::ToVertexColor(PartColors[int32(Part)], 0.5f), Inset);
					continue;
				}
				if (Face.Dir.Y != 0 || Part == ELBAEnclosurePart::Ceiling || Part == ELBAEnclosurePart::Riser)
				{
					EmitQuad(Cell, Face, CeilingSection, nullptr, FaceIndex, Art.CeilingColor, Inset);
					continue;
				}

				// Fallback: the art of the first opaque brick down the wall it stands on (its top brick is often a
				// see-through trim), else that wall's color. Its own column first, then along the wall (cut partitions
				// often stand on collision, with a trim on top), then the wall in front of it (an undrawn band behind
				// a stub). The first drawn brick found is the wall this one goes on with.
				const int32 Slot = ColorSlotOf(FrontFaceOf(FaceIndex));
				const FIntPoint* SourceCell = nullptr;
				int32 ArtFaceIndex = FrontFaceOf(FaceIndex);
				FVector4f Color = RoomColors[*Space];
				FIntPoint WallColumn(INDEX_NONE, INDEX_NONE);
				int32 WallTop = INDEX_NONE;
				{
					const FIntPoint Own(Cell.X, Cell.Z);
					const FIntPoint Along(Face.Dir.Z, Face.Dir.X);
					bool bColored = false;
					for (const FIntPoint Column : { Own, Own + Along, Own - Along, Own + Along * 2, Own - Along * 2, Own + FIntPoint(Face.Dir.X, Face.Dir.Z) })
					{
						int32 Y = bBacking ? Cell.Y : Cell.Y - 1;
						while (Y >= 0 && Cells.Fill.Contains(FIntVector(Column.X, Y, Column.Y)))
						{
							--Y;
						}
						for (; Y >= 0 && Query.ShapeAt(Column.X, Y, Column.Y) != 0 && !SourceCell; --Y)
						{
							if (!IsDrawn(Column.X, Y, Column.Y))
							{
								continue;
							}
							if (WallTop == INDEX_NONE)
							{
								WallColumn = Column;
								WallTop = Y;
							}
							const int32 Brick = Query.BrickAt(Column.X, Y, Column.Y);
							const TStaticArray<FColor, 3>* Colors = Atlas.FaceColors.Find(Brick);
							if (Colors && !bColored)
							{
								Color = LBABodyConverter::ToVertexColor((*Colors)[Slot], 0.45f);
								bColored = true;
							}
							SourceCell = WallArt(Brick, FaceIndex, OpaqueCoverage, ArtFaceIndex,
								Art.Grid ? FacingSlotOf(Art.Grid->Get(Column.X, Y, Column.Y).Layout) : 0);
						}
						if (SourceCell)
						{
							break;
						}
					}
				}

				// The wall style this face is built in, and where the face is in it: Foot is the cell the style starts
				// at, Height the cells it has up to the ceiling.
				const FLBAWallStyle* Style = nullptr;
				bool bRoomStyle = false;
				FIntPoint Origin(0, 0);
				int32 Foot = 0, Height = 0;
				int32 AirBottom = Next.Y, AirTop = Next.Y;
				if (bFacesAir)
				{
					while (Cells.Air.Contains(FIntVector(Next.X, AirBottom - 1, Next.Z)))
					{
						--AirBottom;
					}
					while (Cells.Air.Contains(FIntVector(Next.X, AirTop + 1, Next.Z)))
					{
						++AirTop;
					}
				}
				const bool bDrawnSide = Face.Dir.X > 0 || Face.Dir.Z > 0;
				if (bFacesAir && WallTop != INDEX_NONE && Art.Grid && Art.Library && (bDrawnSide || Part == ELBAEnclosurePart::Partition))
				{
					const FLBAGridCell& Top = Art.Grid->Get(WallColumn.X, WallTop, WallColumn.Y);
					const FIntVector Block = Art.Library->Layouts.IsValidIndex(Top.Layout) ? Art.Library->Layouts[Top.Layout].GetBlockCoords(Top.Block) : FIntVector::ZeroValue;
					const int32 StyleIndex = Art.Library->Layouts.IsValidIndex(Top.Layout)
						? ULBAWallStyleLibrary::FindStyle(Art.Styles, Top.Layout, Block.Y, ELBAWallFacing::Indoor) : INDEX_NONE;
					if (StyleIndex != INDEX_NONE)
					{
						Style = &Art.Styles[StyleIndex];
						Foot = WallTop - StyleIndexOf(*Style, Top.Layout, Block.Y);
						Height = AirTop - Foot + 1;
						Origin = FIntPoint(WallColumn.X - Block.X, WallColumn.Y - Block.Z);
					}
				}
				// The other sides, and walls whose top is in no style (the original cut Twinsen's partitions down to a
				// slice of their top band over invisible collision): the room's style from its floor up.
				if (!Style && bFacesAir && Art.RoomStyles.IsValidIndex(*Space) && Art.Styles.IsValidIndex(Art.RoomStyles[*Space]))
				{
					// From the room's floor, which may be further down than the air in front (over a door frame, a
					// cupboard): the first floor under it in its column, else next to it.
					int32 Floor = INDEX_NONE;
					for (const FIntPoint Offset : { FIntPoint(0, 0), FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
					{
						for (int32 Y = AirBottom - 1; Y >= 0 && Floor == INDEX_NONE; --Y)
						{
							if (Rooms.IsRoomFloor(FIntVector(Next.X + Offset.X, Y, Next.Z + Offset.Y)))
							{
								Floor = Y;
							}
						}
						if (Floor != INDEX_NONE)
						{
							break;
						}
					}
					bRoomStyle = true;
					Style = &Art.Styles[Art.RoomStyles[*Space]];
					Foot = Floor != INDEX_NONE ? Floor + 1 : AirBottom;
					Height = AirTop - Foot + 1;
				}
				int32 LayoutIndex = INDEX_NONE, LayoutY = 0;
				bool bTop = false, bStyled = false;
				int32 ArtKind = 0;
				const FIntPoint Own(Cell.X, Cell.Z);
				if (Style && StyleCell(*Style, Cell.Y - Foot, Height, Art.bWallTops, LayoutIndex, LayoutY, bTop))
				{
					int32 StyleArtFace = ArtFaceIndex;
					const int32 Brick = LayoutBrick(LayoutIndex, LayoutY, Origin, Own);
					if (const FIntPoint* StyleArt = WallArt(Brick, FaceIndex, OpaqueCoverage, StyleArtFace, FacingSlotOf(LayoutIndex)))
					{
						ArtKind = bRoomStyle ? 1 : 0;
						bStyled = true;
						SourceCell = StyleArt;
						ArtFaceIndex = StyleArtFace;
					}
					else if (bTop)
					{
						// A see-through top (a cap, a cornice) in front of the body.
						int32 BodyLayout = INDEX_NONE, BodyY = 0;
						bool bBodyTop = false;
						int32 BodyArtFace = ArtFaceIndex;
						if (StyleCell(*Style, Cell.Y - Foot, Height, false, BodyLayout, BodyY, bBodyTop))
						{
							if (const FIntPoint* BodyArt = WallArt(LayoutBrick(BodyLayout, BodyY, Origin, Own), FaceIndex, OpaqueCoverage, BodyArtFace, FacingSlotOf(BodyLayout)))
							{
								SourceCell = BodyArt;
								ArtFaceIndex = BodyArtFace;
							}
						}
						if (const FIntPoint* TopArt = !bDebugArt ? WallArt(Brick, FaceIndex, MinTopCoverage, StyleArtFace, FacingSlotOf(LayoutIndex)) : nullptr)
						{
							++Stats[bRoomStyle ? 1 : 0];
							EmitQuad(Cell, Face, BrickSection, TopArt, StyleArtFace, Color, Inset);
							EmitQuad(Cell, Face, SourceCell ? BrickSection : INDEX_NONE, SourceCell, ArtFaceIndex, Color, Inset + BackingInset);
							continue;
						}
					}
				}
				if (!bStyled)
				{
					ArtKind = Style ? 2 : SourceCell ? 3 : 4;
				}
				++Stats[ArtKind];
				if (Art.FaceDump)
				{
					*Art.FaceDump += FString::Printf(TEXT("W %d %d %d %d %d %d %d %d %d %d\n"), Cell.X, Cell.Y, Cell.Z, FaceIndex, ArtKind, LayoutIndex, LayoutY,
						WallColumn.X, WallColumn.Y, WallTop);
				}
				if (bDebugArt)
				{
					SourceCell = nullptr;
					Color = LBABodyConverter::ToVertexColor(ArtColors[ArtKind], 0.5f);
				}
				EmitQuad(Cell, Face, SourceCell ? BrickSection : INDEX_NONE, SourceCell, ArtFaceIndex, Color, Inset);
			}
		};
		for (const FIntVector& Cell : Cells.Fill)
		{
			EmitCell(Cell, false);
		}
		for (const FIntVector& Cell : Cells.Backing)
		{
			EmitCell(Cell, true);
		}
		UE_LOG(LogLBA2, Display, TEXT("Enclosure wall faces: %d own style, %d room style, %d style without art, %d fallback brick, %d flat"),
			Stats[0], Stats[1], Stats[2], Stats[3], Stats[4]);
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

		// Copy the sprite and repeat its border pixels into the padding to limit filtering bleed.
		for (int32 Y = -CellPad; Y < SpriteH + CellPad; ++Y)
		{
			for (int32 X = -CellPad; X < SpriteW + CellPad; ++X)
			{
				const uint8 ColorIndex = Sprite.Pixels[FMath::Clamp(Y, 0, SpriteH - 1) * SpriteW + FMath::Clamp(X, 0, SpriteW - 1)];
				FColor Color = Palette[ColorIndex];
				Color.A = ColorIndex == 0 ? 0 : 255;
				Pixels[(Cell.Y + CellPad + Y) * Atlas.Width + Cell.X + CellPad + X] = Color;
			}
		}
	}

	const FString Folder = Context.GetContentPath(FString::Printf(TEXT("Interiors/Libraries/Lib_%02d"), LibraryIndex));
	Atlas.Texture = Context.CreateTexture(Folder, FString::Printf(TEXT("T_Lib%02d_Bricks"), LibraryIndex), Atlas.Width, Atlas.Height, Pixels, false);
	Atlas.Material = Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Atlas.Texture, Folder);
	Atlas.FillMaterial = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
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
	Styles = Detector.Finish(LibraryEnclosure ? TSet<int32>(LibraryEnclosure->IgnoredWallLayouts) : TSet<int32>());

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

void FLBAIsoImporter::LogRooms(int32 GridIndex, const FLBARoomAnalysis& Rooms, const TArray<FLBADoorway>& Doorways, const TArray<FLBACellBox>& Zones)
{
	int32 Exits = 0;
	int32 Stubs = 0;
	for (const FLBADoorway& Doorway : Doorways)
	{
		Exits += Doorway.bExit;
		Stubs += Doorway.IsInStub();
	}
	int32 ZonesAtDoorways = 0;
	for (const FLBACellBox& Zone : Zones)
	{
		ZonesAtDoorways += Doorways.ContainsByPredicate([&Zone](const FLBADoorway& Doorway) { return FLBARoomAnalysis::IsNear(Doorway.Cells, Zone); });
	}
	UE_LOG(LogLBA2, Display, TEXT("Grid %d: %d spaces, %d doorways (%d exits, %d between rooms, %d in cut-down walls), %d/%d door zones at a doorway"),
		GridIndex, Rooms.GetSpaces().Num(), Doorways.Num(), Exits, Doorways.Num() - Exits, Stubs, ZonesAtDoorways, Zones.Num());
	static const TCHAR* EndNames[] = { TEXT("room"), TEXT("plug"), TEXT("edge"), TEXT("void"), TEXT("drop"), TEXT("blocked") };
	for (const FLBADoorway& Doorway : Doorways)
	{
		TArray<FString> Signals;
		if (EnumHasAnyFlags(Doorway.Signals, ELBADoorwaySignal::Lintel)) { Signals.Add(TEXT("lintel")); }
		if (EnumHasAnyFlags(Doorway.Signals, ELBADoorwaySignal::Jamb)) { Signals.Add(TEXT("jamb")); }
		if (EnumHasAnyFlags(Doorway.Signals, ELBADoorwaySignal::Threshold)) { Signals.Add(TEXT("threshold")); }
		if (EnumHasAnyFlags(Doorway.Signals, ELBADoorwaySignal::Zone)) { Signals.Add(TEXT("zone")); }
		const FIntVector& First = Doorway.Cells[0];
		UE_LOG(LogLBA2, Verbose, TEXT("DOORWAY %d %s %s %c w%d f%d c%d n%d (%d,%d,%d) %s"), GridIndex, Doorway.bExit ? TEXT("exit") : TEXT("interior"),
			EndNames[int32(Doorway.End)], Doorway.Axis == 0 ? TEXT('X') : TEXT('Z'), Doorway.Width, Doorway.FlankHeight, Doorway.ClearHeight,
			Doorway.Cells.Num(), First.X, First.Y, First.Z, *FString::Join(Signals, TEXT("+")));
	}
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

	FCellQuery Query;
	Query.Size = FIntVector(LBA::GridSizeXZ, LBA::GridSizeY, LBA::GridSizeXZ);
	const int32 Forbidden = Header.ForbiddenBrick;
	auto Block = [&Grid, Library](int32 X, int32 Y, int32 Z) -> const FLBALayoutBlock*
	{
		const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
		if (!Library->Layouts.IsValidIndex(Cell.Layout))
		{
			return nullptr;
		}
		const FLBALayout& Layout = Library->Layouts[Cell.Layout];
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

	// Rooms and doorways, and the enclosure that closes the rooms for the Enhanced look.
	UStaticMesh* EnclosureMesh = nullptr;
	// The frames of the cut walls that the enclosure replaces: off the grid's mesh, in a mesh of their own.
	TSet<FIntVector> RemovedFrames;
	{
		const FLBARoomAnalysis Rooms(
			[&Query](int32 X, int32 Y, int32 Z) { return Query.ShapeAt(X, Y, Z); },
			[&Query](int32 X, int32 Y, int32 Z) { return Query.BrickAt(X, Y, Z); },
			[&Grid](int32 X, int32 Y, int32 Z) { return int32(Grid.Get(X, Y, Z).Layout); });
		FLBADoorwayHints Hints;
		if (const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex))
		{
			Hints.Jambs.Append(LibraryEnclosure->DoorwayJambs);
			Hints.Thresholds.Append(LibraryEnclosure->DoorwayThresholds);
		}
		if (const TArray<FLBACellBox>* Zones = DoorZones.Find(GridIndex))
		{
			Hints.Zones = *Zones;
		}
		const TArray<FLBADoorway> Doorways = Rooms.FindDoorways(Hints);
		LogRooms(GridIndex, Rooms, Doorways, Hints.Zones);

		TSet<int32> FrameLayouts;
		if (const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex))
		{
			FrameLayouts.Append(LibraryEnclosure->CutFrames);
		}
		auto IsFrame = [&Grid, &FrameLayouts](int32 X, int32 Y, int32 Z)
		{
			return FrameLayouts.Contains(int32(Grid.Get(X, Y, Z).Layout));
		};
		// Drawn: a brick with art (some libraries stack bricks without any to block the camera-side walls) that is not
		// a frame of the cut walls.
		auto IsDrawn = [&Query, &Atlas, &IsFrame](int32 X, int32 Y, int32 Z)
		{
			const int32 Brick = Query.BrickAt(X, Y, Z);
			if (Brick == 0 || IsFrame(X, Y, Z))
			{
				return false;
			}
			const TStaticArray<float, 3>* Coverage = Atlas->FaceCoverage.Find(Brick);
			return !Coverage || FMath::Max3((*Coverage)[0], (*Coverage)[1], (*Coverage)[2]) > 0.f;
		};
		const FLBAEnclosureCells Enclosure = FLBAEnclosureCells::Build(Rooms, Doorways, Context.GetProjectSettings().GetEnclosure(Grid.LibraryIndex, GridIndex), IsDrawn);
		for (const FIntVector& Cell : Enclosure.Fill)
		{
			if (Query.BrickAt(Cell.X, Cell.Y, Cell.Z) != 0 && IsFrame(Cell.X, Cell.Y, Cell.Z))
			{
				RemovedFrames.Add(Cell);
			}
		}
		const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex);
		FEnclosureArt Art;
		Art.Grid = &Grid;
		Art.Library = Library;
		Art.ForbiddenBrick = Forbidden;
		Art.Styles = GetWallStyles(Grid.LibraryIndex);
		if (LibraryEnclosure)
		{
			Art.CeilingColor = LBABodyConverter::ToVertexColor(LibraryEnclosure->CeilingColor, 0.5f);
			Art.CeilingMaterial = LibraryEnclosure->CeilingMaterial.LoadSynchronous();
			Art.bWallTops = LibraryEnclosure->bWallTops;
		}
		{
			// Each room's style: the one its back walls are built in the most (in cells), or the library's choice.
			const int32 Preferred = LibraryEnclosure && LibraryEnclosure->IndoorWallLayout != INDEX_NONE
				? ULBAWallStyleLibrary::FindStyle(Art.Styles, LibraryEnclosure->IndoorWallLayout, -1, ELBAWallFacing::Indoor) : INDEX_NONE;
			TMap<FIntVector, int32> FloorSpaces;
			for (int32 Space = 0; Space < Rooms.GetSpaces().Num(); ++Space)
			{
				for (const FIntVector& Floor : Rooms.GetSpaces()[Space])
				{
					FloorSpaces.Add(Floor, Space);
				}
			}
			TArray<TMap<int32, int32>> Votes;
			Votes.SetNum(Rooms.GetSpaces().Num());
			TMap<int32, int32> GridVotes;
			auto Coverage = [Atlas, Forbidden](int32 Brick, int32 Slot)
			{
				const TStaticArray<float, 3>* Values = Brick != 0 && Brick != Forbidden ? Atlas->FaceCoverage.Find(Brick) : nullptr;
				return Values ? (*Values)[Slot] : 0.f;
			};
			for (const FLBAWallRun& Run : LBAWallSurvey::FindRuns(Grid, *Library, Coverage, OpaqueCoverage))
			{
				if (Run.Facing != ELBAWallFacing::Indoor || !Run.IsSolid())
				{
					continue;
				}
				for (const FLBAWallCell& WallCell : Run.Cells)
				{
					const int32 StyleIndex = ULBAWallStyleLibrary::FindStyle(Art.Styles, WallCell.Layout, WallCell.Block.Y, ELBAWallFacing::Indoor);
					if (StyleIndex == INDEX_NONE || Art.Styles[StyleIndex].Facing != ELBAWallFacing::Indoor)
					{
						continue;
					}
					++GridVotes.FindOrAdd(StyleIndex);
					if (const int32* Space = FloorSpaces.Find(FIntVector(Run.Column.X + Run.Side.X, Run.Foot - 1, Run.Column.Y + Run.Side.Y)))
					{
						++Votes[*Space].FindOrAdd(StyleIndex);
					}
				}
			}
			auto MostVoted = [](const TMap<int32, int32>& Counts)
			{
				int32 Best = INDEX_NONE, BestCount = 0;
				for (const TPair<int32, int32>& Pair : Counts)
				{
					if (Pair.Value > BestCount || (Pair.Value == BestCount && Pair.Key < Best))
					{
						Best = Pair.Key;
						BestCount = Pair.Value;
					}
				}
				return Best;
			};
			// Rooms without back walls of their own take the grid's, else the library's most used.
			int32 Fallback = MostVoted(GridVotes);
			if (Fallback == INDEX_NONE && Art.Styles.Num() > 0 && Art.Styles[0].Facing == ELBAWallFacing::Indoor)
			{
				Fallback = 0;
			}
			for (const TMap<int32, int32>& Counts : Votes)
			{
				const int32 Voted = MostVoted(Counts);
				Art.RoomStyles.Add(Preferred != INDEX_NONE ? Preferred : Voted != INDEX_NONE ? Voted : Fallback);
			}
		}
		FString FaceDump;
		const bool bDebugDump = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebug")) || FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebugArt"));
		Art.FaceDump = bDebugDump ? &FaceDump : nullptr;
		FLBAMeshBuilder EnclosureBuilder;
		EmitEnclosure(Enclosure, Rooms, Query, IsDrawn, *Atlas, Art, EnclosureBuilder, Context.GetScale());
		if (!EnclosureBuilder.IsEmpty())
		{
			FMeshDescription Description;
			TArray<UMaterialInterface*> Materials;
			TArray<FName> Slots;
			EnclosureBuilder.BuildStatic(Description, Materials, Slots);
			FLBAStaticMeshOptions Options;
			Options.bComplexAsSimple = true;
			Options.bTwoSidedDistanceField = true;
			EnclosureMesh = Context.CreateStaticMesh(Context.GetContentPath(TEXT("Interiors/Grids/Enclosure")), FString::Printf(TEXT("SM_Grid_%03d_Enclosure"), GridIndex),
				MoveTemp(Description), Materials, Slots, Options);
		}
		UE_LOG(LogLBA2, Display, TEXT("Grid %d enclosure: %d cells filled, %d triangles, %d frame cells removed"), GridIndex, Enclosure.Fill.Num(),
			EnclosureBuilder.NumTriangles(), RemovedFrames.Num());
		if (bDebugDump)
		{
			// Its cells, for Saved/LookDev/tools/leaks.py: "F x y z part", "B x y z", "A x y z space", and its wall faces.
			FString Dump = FaceDump;
			for (const FIntVector& Cell : Enclosure.Fill)
			{
				Dump += FString::Printf(TEXT("F %d %d %d %d\n"), Cell.X, Cell.Y, Cell.Z, int32(Enclosure.Parts.FindRef(Cell)));
			}
			for (const FIntVector& Cell : Enclosure.Backing)
			{
				Dump += FString::Printf(TEXT("B %d %d %d\n"), Cell.X, Cell.Y, Cell.Z);
			}
			for (const TPair<FIntVector, int32>& Pair : Enclosure.Air)
			{
				Dump += FString::Printf(TEXT("A %d %d %d %d\n"), Pair.Key.X, Pair.Key.Y, Pair.Key.Z, Pair.Value);
			}
			FFileHelper::SaveStringToFile(Dump, *(FPaths::ProjectSavedDir() / FString::Printf(TEXT("LookDev/enclosure/cells/grid%03d.txt"), GridIndex)));
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

	// The removed frames, for the Original look (its open rooms keep them).
	UStaticMesh* FramesMesh = nullptr;
	if (RemovedFrames.Num() > 0)
	{
		FCellQuery FramesQuery = Query;
		const TSet<FIntVector>* Frames = &RemovedFrames;
		FramesQuery.Brick = [BaseBrick = Query.Brick, Frames](int32 X, int32 Y, int32 Z)
		{
			return Frames->Contains(FIntVector(X, Y, Z)) ? BaseBrick(X, Y, Z) : 0;
		};
		FLBAMeshBuilder FramesBuilder;
		EmitVisual(FramesQuery, *Atlas, FramesBuilder, FramesBuilder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas->Material), Scale,
			Context.GetProjectSettings().BackFaces);
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
	EmitVisual(VisualQuery, *Atlas, Builder, Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas->Material), Scale, Context.GetProjectSettings().BackFaces);
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
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, Placement.Transform, Placement.Label, TEXT("Grid/RemakeModels")))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(Placement.Mesh);
			// The grid's brick collision already covers these.
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			if (Preset.RemakeLamps.Light > 0.f)
			{
				for (const FLBARemakeAssets::FLamp& Lamp : Placement.Lamps)
				{
					LBAEnvironment::SpawnDecorLight(Context, Actor, Preset.RemakeLamps, Lamp.Position, Lamp.Color, Lamp.SourceRadius);
				}
			}
		}
	}
	// A light from nearly overhead: brick floors get about the neutral light (the interior preset is calibrated for it).
	LBAEnvironment::Spawn(Context, World, Preset, FRotator(-84.0, 45.0, 0.0));

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
