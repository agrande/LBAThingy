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
#include "Import/LBARoomModel.h"
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
	 * cornice), laid from the floor, the row under the top (the body) repeated where the wall is taller, the top row right
	 * under the ceiling. */
	struct FWallpaper
	{
		TArray<FIntPoint> Rows;

		/** The layout row at Above cells over the floor of a wall Height cells tall (INDEX_NONE layout: none). */
		FIntPoint GetRow(int32 Above, int32 Height) const
		{
			if (Rows.Num() == 0)
			{
				return FIntPoint(INDEX_NONE, 0);
			}
			const int32 Top = Rows.Num() - 1;
			const int32 Body = FMath::Max(Top - 1, 0);
			return Above == Height - 1 && Top > Body ? Rows[Top] : Rows[FMath::Clamp(Above, 0, Body)];
		}
	};

	/** What dresses an enclosure besides the grid's own art: each room's wallpaper and the ceilings. */
	struct FEnclosureArt
	{
		const FLBALibrary* Library = nullptr;
		int32 ForbiddenBrick = 0;
		/** Per room: the wallpaper of its walls facing X (0) and Z (1). */
		TArray<TStaticArray<FWallpaper, 2>> Wallpapers;
		/** Per room: the color of its walls where no wallpaper has art (its floor's). */
		TArray<FVector4f> RoomColors;
		FVector4f CeilingColor = FVector4f(0.3f, 0.3f, 0.3f, 0.5f);
		UMaterialInterface* CeilingMaterial = nullptr;
		bool bCeiling = true;
		int32 DoorHeight = 7;
	};

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

	/** First cell above a doorway's opening: its lintel, or DoorHeight over its floor where the original has none. */
	int32 DoorTopOf(const FLBADoorway& Doorway, int32 Floor, int32 DoorHeight)
	{
		return Floor + 1 + (Doorway.ClearHeight != INDEX_NONE ? Doorway.ClearHeight : DoorHeight);
	}

	/**
	 * The enclosure of a grid's rooms (FLBARoomModel), as faces looking into them (a camera outside sees in):
	 * - Each side of a room's footprint on a wall, up to the room's ceiling: from above what the original drew on the
	 *   sides it drew (-X, -Z of the room: the walls' +X, +Z faces), from the floor on the others (the inner sides of the
	 *   walls facing the camera, never drawn).
	 * - Over doorways, from the top of the opening; under the top of the opening and on the sides of the passage, the
	 *   same; the far end of an exit's passage closed in the dark. Toward another room, where its ceiling is lower; on
	 *   another room's wall that rises from a step (a lower floor's view of it), from the top of the step.
	 * - A ceiling over each footprint column, with risers where it steps (stairs).
	 * Walls take their room's wallpaper (a layout row of its back walls, repeated, the foot row at the floor).
	 * Wall faces all lie a little in front of the walls' planes (WallInset, over the remake's models set in the walls), at
	 * one depth, so no step between faces opens a crack; the grid's own faces they cover go to OutCoveredFaces (cell,
	 * face index), off the grid's mesh (into the Original look's frames mesh). Art that is not opaque is backed by the
	 * wallpaper and drawn again in front of it.
	 * OutCovered: the grid cells the walls stand on (the frames there come off).
	 */
	void EmitEnclosure(const FLBARoomModel& Model, TConstArrayView<FLBADoorway> Doorways, TFunctionRef<bool(int32, int32, int32)> IsDrawn,
		TFunctionRef<FIntPoint(int32, int32, int32)> LayoutRowAt, const FLBAIsoImporter::FAtlas& Atlas, const FEnclosureArt& Art,
		FLBAMeshBuilder& Builder, FLBAMeshBuilder& CeilingBuilder, float Scale, TSet<FIntVector>& OutCovered, TSet<FIntVector4>& OutCoveredFaces)
	{
		const FBrickFace* Faces = GetBrickFaces();
		const int32 BrickSection = Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas.Material);
		int32 FillSection = INDEX_NONE;
		const int32 CeilingSection = CeilingBuilder.GetSection(TEXT("Ceiling"), Art.CeilingMaterial ? Art.CeilingMaterial : Atlas.FillMaterial);
		// Wall faces sit a little in front of the walls' planes; art drawn again over the wallpaper behind it, in front of them.
		constexpr float WallInset = -0.5f;
		constexpr float DecalInset = -1.f;
		// Fraction of the way to its center each corner of a wallpaper face's art is sampled from (EmitQuad's SampleInset,
		// about 2 pixels).
		// Faces toward -Sides[Side] (a wall at the room's -X shows its +X face, and so on).
		static const int32 FaceOfSide[4] = { 2, 3, 4, 5 };
		// -LBA2EnclosureDebug: flat colors by part (walls red, inner sides of the camera-side walls orange, over doorways
		// yellow, doorway passages gray, toward other rooms purple, ceilings green, risers blue).
		enum class EPart : uint8 { Wall, CameraSide, Header, Passage, Step, Ceiling, Riser };
		static const FColor PartColors[] = { FColor(220, 40, 40), FColor(240, 130, 30), FColor(240, 210, 40), FColor(140, 140, 140),
			FColor(160, 60, 220), FColor(40, 200, 60), FColor(40, 80, 230) };
		const bool bDebugParts = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebug"));
		// -LBA2EnclosureDebugArt: walls by their art: wallpaper cyan, its foot green, flat color magenta.
		static const FColor ArtColors[] = { FColor(40, 200, 220), FColor(40, 200, 60), FColor(255, 0, 255) };
		const bool bDebugArt = FParse::Param(FCommandLine::Get(), TEXT("LBA2EnclosureDebugArt"));
		int32 Stats[3] = {};

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
			FLBAVertex V[4];
			for (int32 K = 0; K < 4; ++K)
			{
				const FVector3f Corner(Cell.X + Face.Corners[K].X, Cell.Y + Face.Corners[K].Y, Cell.Z + Face.Corners[K].Z);
				V[K].Position = CornerPos(Corner.X, Corner.Y, Corner.Z, Scale) - Normal * Inset;
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

		// The faces of a wall standing on Wall, cells [From, To), looking into the room at Wall - Sides[Side], in Room's
		// wallpaper laid from Floor up to the room's Ceiling there; bBehind: behind the grid's art of the cells.
		auto EmitWall = [&](const FIntPoint& Wall, int32 Side, int32 From, int32 To, int32 Room, int32 Floor, int32 Ceiling, EPart Part,
			bool bBehind = false)
		{
			const int32 FaceIndex = FaceOfSide[Side];
			const FWallpaper& Wallpaper = Art.Wallpapers[Room][Side / 2];
			for (int32 Y = FMath::Max(From, 0); Y < To; ++Y)
			{
				const FIntVector Cell(Wall.X, Y, Wall.Y);
				OutCoveredFaces.Add(FIntVector4(Cell.X, Cell.Y, Cell.Z, FaceIndex));
				const float Inset = WallInset;
				if (bBehind)
				{
					// The cell's own art, again, over the wallpaper.
					const int32 Brick = Model.GetBrick(Wall.X, Y, Wall.Y);
					if (const FIntPoint* Decal = Brick && !bDebugParts && !bDebugArt ? Atlas.Cells.Find(Brick) : nullptr)
					{
						EmitQuad(Cell, FaceIndex, Decal, FrontFaceOf(FaceIndex), FVector4f(0.5f, 0.5f, 0.5f, 0.5f), DecalInset, false, 0.f);
					}
				}
				else
				{
					OutCovered.Add(Cell);
				}
				if (bDebugParts)
				{
					EmitQuad(Cell, FaceIndex, nullptr, FaceIndex, LBABodyConverter::ToVertexColor(PartColors[int32(Part)], 0.5f), Inset, false);
					continue;
				}
				const int32 Above = Y - (Floor + 1);
				const FIntPoint Row = Wallpaper.GetRow(Above, Ceiling - (Floor + 1));
				const bool bFoot = Above == 0;
				int32 ArtFace = FrontFaceOf(FaceIndex);
				const FIntPoint* Source = Row.X != INDEX_NONE ? WallArt(LayoutBrick(Row.X, Row.Y, Wall), FaceIndex, FacingSlotOf(Row.X), ArtFace) : nullptr;
				const int32 Kind = Source ? (bFoot ? 1 : 0) : 2;
				++Stats[Kind];
				if (bDebugArt)
				{
					EmitQuad(Cell, FaceIndex, nullptr, FaceIndex, LBABodyConverter::ToVertexColor(ArtColors[Kind], 0.5f), Inset, false);
					continue;
				}
				EmitQuad(Cell, FaceIndex, Source, ArtFace, Art.RoomColors[Room], Inset, false);
			}
		};
		// Faces of the ceiling mesh: the ceiling over a column, a riser where it steps up from a neighbor's.
		auto EmitCeilingFace = [&](const FIntVector& Cell, int32 FaceIndex, EPart Part)
		{
			const FVector4f Color = bDebugParts ? LBABodyConverter::ToVertexColor(PartColors[int32(Part)], 0.5f) : Art.CeilingColor;
			EmitQuad(Cell, FaceIndex, nullptr, FaceIndex, Color, 0.f, true);
		};

		const TArray<FLBARoom>& Rooms = Model.GetRooms();
		TMap<int32, int32> DoorwayRooms;
		for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
		{
			const FLBARoom& Room = Rooms[RoomIndex];
			for (const FLBARoomEdge& Edge : Room.Edges)
			{
				const FIntPoint Next = Edge.Column + FLBARoomModel::Sides[Edge.Side];
				const int32 Ceiling = Room.GetCeiling(Edge.Column);
				const int32 Floor = Room.GetFloor(Edge.Column);
				// The sides the original drew: the room's -X and -Z (the walls' +X and +Z faces).
				const bool bDrawnSide = Edge.Side == 0 || Edge.Side == 2;
				switch (Edge.Kind)
				{
				case ELBARoomEdge::Wall:
				{
					int32 From = Floor + 1;
					if (bDrawnSide)
					{
						// Above what the original drew; over its top row too where that is the top of the room's wallpaper
						// (its cornice) under a taller room: the wallpaper's own top goes under the ceiling.
						const int32 Drawn = TopDrawn(IsDrawn, Next, Floor + 1, Ceiling);
						const FWallpaper& Wallpaper = Art.Wallpapers[RoomIndex][Edge.Side / 2];
						const bool bCornice = Drawn != INDEX_NONE && Drawn + 1 < Ceiling && Wallpaper.Rows.Num() >= 2
							&& LayoutRowAt(Next.X, Drawn, Next.Y) == Wallpaper.Rows.Last();
						From = FMath::Max(Floor + 1, bCornice ? Drawn : Drawn + 1);
						// Below it, the cells the original left undrawn (hidden behind furniture: invisible collision), and
						// behind the art that is not opaque (frames, curves, sconces: the next room shows through them).
						const int32 Slot = ColorSlotOf(FrontFaceOf(FaceOfSide[Edge.Side]));
						for (int32 Y = Floor + 1; Y < From; ++Y)
						{
							const TStaticArray<float, 3>* Coverage = Atlas.FaceCoverage.Find(Model.GetBrick(Next.X, Y, Next.Y));
							if (!IsDrawn(Next.X, Y, Next.Y))
							{
								EmitWall(Next, Edge.Side, Y, Y + 1, RoomIndex, Floor, Ceiling, EPart::Wall);
							}
							else if (Coverage && (*Coverage)[Slot] < OpaqueCoverage)
							{
								EmitWall(Next, Edge.Side, Y, Y + 1, RoomIndex, Floor, Ceiling, EPart::Wall, true);
							}
						}
					}
					EmitWall(Next, Edge.Side, From, Ceiling, RoomIndex, Floor, Ceiling, bDrawnSide ? EPart::Wall : EPart::CameraSide);
					break;
				}
				case ELBARoomEdge::Door:
				{
					DoorwayRooms.FindOrAdd(Edge.Other, RoomIndex);
					const TPair<int32, int32>* Door = Model.FindDoorColumn(Next);
					const int32 Top = DoorTopOf(Doorways[Edge.Other], Door->Value, Art.DoorHeight);
					const int32 From = bDrawnSide ? FMath::Max(Top, TopDrawn(IsDrawn, Next, Top, Ceiling) + 1) : Top;
					EmitWall(Next, Edge.Side, From, Ceiling, RoomIndex, Floor, Ceiling, EPart::Header);
					break;
				}
				case ELBARoomEdge::Rise:
					EmitWall(Next, Edge.Side, Floor + 1 + Model.GetRun(Next.X, Floor, Next.Y), Ceiling, RoomIndex, Floor, Ceiling, EPart::Step);
					break;
				case ELBARoomEdge::Room:
				{
					const int32 Other = Rooms[Edge.Other].GetCeiling(Next);
					if (Other < Ceiling)
					{
						EmitWall(Next, Edge.Side, Other, Ceiling, RoomIndex, Floor, Ceiling, EPart::Step);
					}
					break;
				}
				default:
					break;
				}
			}
			if (!Art.bCeiling)
			{
				continue;
			}
			for (const FIntPoint& Column : Room.Footprint)
			{
				const int32 Ceiling = Room.GetCeiling(Column);
				EmitCeilingFace(FIntVector(Column.X, Ceiling, Column.Y), 1, EPart::Ceiling);
				for (int32 Side = 0; Side < 4; ++Side)
				{
					const FIntPoint Next = Column + FLBARoomModel::Sides[Side];
					if (!Room.Footprint.Contains(Next))
					{
						continue;
					}
					for (int32 Y = Room.GetCeiling(Next); Y < Ceiling; ++Y)
					{
						EmitCeilingFace(FIntVector(Next.X, Y, Next.Y), FaceOfSide[Side], EPart::Riser);
					}
				}
			}
		}

		// Doorway passages next to the rooms: closed over the opening, and on their sides up to it.
		for (int32 DoorwayIndex = 0; DoorwayIndex < Doorways.Num(); ++DoorwayIndex)
		{
			const int32* Room = DoorwayRooms.Find(DoorwayIndex);
			if (!Room)
			{
				continue;
			}
			for (const FIntVector& Cell : Doorways[DoorwayIndex].Cells)
			{
				const FIntPoint Column(Cell.X, Cell.Z);
				const int32 Top = DoorTopOf(Doorways[DoorwayIndex], Cell.Y, Art.DoorHeight);
				// Under its lintel too: the grid only has the faces the iso camera saw, not the lintel's underside.
				const bool bLintel = Top < LBA::GridSizeY && IsDrawn(Column.X, Top, Column.Y);
				if (Top < LBA::GridSizeY && (Art.bCeiling || bLintel))
				{
					if (!bLintel)
					{
						OutCovered.Add(FIntVector(Column.X, Top, Column.Y));
					}
					EmitCeilingFace(FIntVector(Column.X, Top, Column.Y), 1, EPart::Passage);
				}
				for (int32 Side = 0; Side < 4; ++Side)
				{
					// The passage's sides, like a room's walls; its far end (an exit) closed in the dark the original
					// shows past its doors.
					const FIntPoint Flank = Column + FLBARoomModel::Sides[Side];
					if (Model.FindDoorColumn(Flank)
						|| Rooms.ContainsByPredicate([&Flank](const FLBARoom& Other) { return Other.Footprint.Contains(Flank); }))
					{
						continue;
					}
					if (Side / 2 == Doorways[DoorwayIndex].Axis / 2)
					{
						for (int32 Y = Cell.Y + 1; Y < Top; ++Y)
						{
							const FColor Color = bDebugParts ? PartColors[int32(EPart::Passage)] : FColor(6, 6, 8);
							OutCoveredFaces.Add(FIntVector4(Flank.X, Y, Flank.Y, FaceOfSide[Side]));
							EmitQuad(FIntVector(Flank.X, Y, Flank.Y), FaceOfSide[Side], nullptr, FaceOfSide[Side], LBABodyConverter::ToVertexColor(Color, 0.5f), WallInset, false);
						}
						continue;
					}
					if (!Model.IsWall(Flank, Cell.Y))
					{
						continue;
					}
					const bool bDrawnSide = Side == 0 || Side == 2;
					const int32 From = bDrawnSide ? FMath::Max(Cell.Y + 1, TopDrawn(IsDrawn, Flank, Cell.Y + 1, Top) + 1) : Cell.Y + 1;
					EmitWall(Flank, Side, From, Top, *Room, Cell.Y, Rooms[*Room].Ceiling, EPart::Passage);
				}
			}
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

TArray<FLBADoorway> FLBAIsoImporter::FindDoorways(int32 GridIndex, int32 LibraryIndex, const FLBARoomAnalysis& Rooms) const
{
	FLBADoorwayHints Hints;
	if (const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(LibraryIndex))
	{
		Hints.Jambs.Append(LibraryEnclosure->DoorwayJambs);
		Hints.Thresholds.Append(LibraryEnclosure->DoorwayThresholds);
	}
	if (const TArray<FLBACellBox>* Zones = DoorZones.Find(GridIndex))
	{
		Hints.Zones = *Zones;
	}
	return Rooms.FindDoorways(Hints);
}

const TSet<int32>& FLBAIsoImporter::GetWallBricks(int32 LibraryIndex)
{
	if (const TSet<int32>* Found = WallBricks.Find(LibraryIndex))
	{
		return *Found;
	}
	// Rooms found with the height rules in every grid of the library tell its walls' bricks.
	FLBAWallBrickCounts Counts;
	const FLBALibrary* Library = GetLibrary(LibraryIndex);
	const FAtlas* Atlas = GetAtlas(LibraryIndex);
	auto HasArt = [Atlas](int32 Brick) { return HasBrickArt(Atlas, Brick); };
	for (int32 GridIndex = 0; GridIndex < Header.GetNumGrids() && Library; ++GridIndex)
	{
		FLBAGrid Grid;
		if (!Grid.Load(Bkg->GetEntry(Header.GridStart + GridIndex), GridIndex) || Grid.LibraryIndex != LibraryIndex)
		{
			continue;
		}
		const FCellQuery Query = MakeGridQuery(Grid, *Library, Header.ForbiddenBrick);
		auto Shape = [&Query](int32 X, int32 Y, int32 Z) { return Query.ShapeAt(X, Y, Z); };
		auto Brick = [&Query](int32 X, int32 Y, int32 Z) { return Query.BrickAt(X, Y, Z); };
		const FLBARoomAnalysis Rooms(Shape, Brick, [&Grid](int32 X, int32 Y, int32 Z) { return int32(Grid.Get(X, Y, Z).Layout); });
		const TArray<FLBADoorway> Doorways = FindDoorways(GridIndex, LibraryIndex, Rooms);
		const FLBARoomModel Model(Shape, Brick, HasArt, Doorways, Context.GetProjectSettings().GetEnclosure(LibraryIndex, GridIndex).RoomHeight, TSet<int32>());
		Model.CountBricks(Counts);
	}
	TSet<int32>& Result = WallBricks.Add(LibraryIndex, Counts.GetWallBricks());
	UE_LOG(LogLBA2, Display, TEXT("Library %d: %d wall bricks, %d furniture bricks"), LibraryIndex, Result.Num(), Counts.Furniture.Num());
	return Result;
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

	// Rooms and doorways, and the enclosure that closes the rooms for the Enhanced look.
	UStaticMesh* EnclosureMesh = nullptr;
	UStaticMesh* CeilingMesh = nullptr;
	// The enclosed rooms: the box of their air (cells) and their floor's color, for their bounce light.
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
		const FLBARoomAnalysis Rooms(
			[&Query](int32 X, int32 Y, int32 Z) { return Query.ShapeAt(X, Y, Z); },
			[&Query](int32 X, int32 Y, int32 Z) { return Query.BrickAt(X, Y, Z); },
			[&Grid](int32 X, int32 Y, int32 Z) { return int32(Grid.Get(X, Y, Z).Layout); });
		const TArray<FLBADoorway> Doorways = FindDoorways(GridIndex, Grid.LibraryIndex, Rooms);
		LogRooms(GridIndex, Rooms, Doorways, DoorZones.FindRef(GridIndex));

		TSet<int32> FrameLayouts;
		if (const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex))
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
		// invisible, so the topmost piece drawn under MinCutCollision invisible cells (frames aside) is the cut (a slice
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
					if (Invisible >= FLBARoomModel::CutCollision)
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
		const FLBAEnclosureSettings Settings = Context.GetProjectSettings().GetEnclosure(Grid.LibraryIndex, GridIndex);
		const FLBALibraryEnclosure* LibraryEnclosure = Context.GetProjectSettings().FindLibraryEnclosure(Grid.LibraryIndex);
		if (Settings.bEnclose)
		{
			const FLBARoomModel Model(
				[&Query](int32 X, int32 Y, int32 Z) { return Query.ShapeAt(X, Y, Z); },
				[&Query](int32 X, int32 Y, int32 Z) { return Query.BrickAt(X, Y, Z); },
				[Atlas](int32 Brick) { return HasBrickArt(Atlas, Brick); },
				Doorways, Settings.RoomHeight, GetWallBricks(Grid.LibraryIndex));
			UE_LOG(LogLBA2, Display, TEXT("Grid %d rooms:\n%s"), GridIndex, *Model.Describe());
			const TArray<FLBARoom>& ModelRooms = Model.GetRooms();

			FEnclosureArt Art;
			Art.Library = Library;
			Art.ForbiddenBrick = Forbidden;
			Art.bCeiling = Settings.bCeiling;
			Art.DoorHeight = Settings.DoorHeight;
			if (LibraryEnclosure)
			{
				Art.CeilingColor = LBABodyConverter::ToVertexColor(LibraryEnclosure->CeilingColor, 0.5f);
				Art.CeilingMaterial = LibraryEnclosure->CeilingMaterial.LoadSynchronous();
			}

			// Each room's wallpaper, per orientation of its walls: at each height over the floor, the wall layout row drawn
			// most there on its back walls (the sides the original drew; wall panels only: one cell thin, opaque on the
			// side they face), up to the highest drawn; else the room's other orientation's, else the grid's.
			using FVotes = TMap<FIntPoint, int32>;
			using FProfileVotes = TArray<FVotes>;
			TArray<TStaticArray<FProfileVotes, 2>> Votes;
			Votes.SetNum(ModelRooms.Num());
			TStaticArray<FProfileVotes, 2> GridVotes;
			for (int32 RoomIndex = 0; RoomIndex < ModelRooms.Num(); ++RoomIndex)
			{
				const FLBARoom& Room = ModelRooms[RoomIndex];
				for (const FLBARoomEdge& Edge : Room.Edges)
				{
					if (Edge.Kind != ELBARoomEdge::Wall || (Edge.Side != 0 && Edge.Side != 2))
					{
						continue;
					}
					const int32 Axis = Edge.Side / 2;
					const int32 Slot = Axis == 0 ? 1 : 2;
					const FIntPoint Wall = Edge.Column + FLBARoomModel::Sides[Edge.Side];
					const int32 Floor = Room.GetFloor(Edge.Column);
					for (int32 Y = Floor + 1; Y < Room.GetCeiling(Edge.Column) && Y < LBA::GridSizeY; ++Y)
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
						const int32 Above = Y - (Floor + 1);
						for (FProfileVotes* Profile : { &Votes[RoomIndex][Axis], &GridVotes[Axis] })
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
			auto MakeWallpaper = [&MostVoted](const FProfileVotes& Profile)
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
				return Wallpaper;
			};
			auto Choose = [&MakeWallpaper](const TStaticArray<FProfileVotes, 2>& Room, const TStaticArray<FProfileVotes, 2>& Whole, int32 Axis)
			{
				for (const FProfileVotes* Profile : { &Room[Axis], &Room[1 - Axis], &Whole[Axis], &Whole[1 - Axis] })
				{
					if (Profile->Num() > 0)
					{
						return MakeWallpaper(*Profile);
					}
				}
				return FWallpaper();
			};
			const int32 Override = LibraryEnclosure && Library->Layouts.IsValidIndex(LibraryEnclosure->WallpaperLayout) ? LibraryEnclosure->WallpaperLayout : INDEX_NONE;
			for (int32 RoomIndex = 0; RoomIndex < ModelRooms.Num(); ++RoomIndex)
			{
				TStaticArray<FWallpaper, 2>& Wallpapers = Art.Wallpapers.AddDefaulted_GetRef();
				FString Chosen;
				for (int32 Axis = 0; Axis < 2; ++Axis)
				{
					FWallpaper& Wallpaper = Wallpapers[Axis];
					if (Override != INDEX_NONE)
					{
						for (int32 Row = 0; Row < Library->Layouts[Override].SizeY; ++Row)
						{
							Wallpaper.Rows.Add(FIntPoint(Override, Row));
						}
					}
					else
					{
						Wallpaper = Choose(Votes[RoomIndex], GridVotes, Axis);
					}
					Chosen += Axis == 0 ? TEXT(" X") : TEXT(" Z");
					for (const FIntPoint& Row : Wallpaper.Rows)
					{
						Chosen += FString::Printf(TEXT(" L%d/%d"), Row.X, Row.Y);
					}
				}
				UE_LOG(LogLBA2, Log, TEXT("WALLPAPER grid %d room %d:%s"), GridIndex, RoomIndex, *Chosen);
			}

			// Walls where no wallpaper has art take their room's floor color; bounce lights too.
			RoomBoxes.SetNum(ModelRooms.Num());
			for (int32 RoomIndex = 0; RoomIndex < ModelRooms.Num(); ++RoomIndex)
			{
				const FLBARoom& Room = ModelRooms[RoomIndex];
				FRoomBox& Box = RoomBoxes[RoomIndex];
				int32 Count = 0;
				for (const TPair<FIntPoint, int32>& Floor : Room.Floors)
				{
					if (const TStaticArray<FColor, 3>* Colors = Atlas->FaceColors.Find(Query.BrickAt(Floor.Key.X, Floor.Value, Floor.Key.Y)))
					{
						Box.Floor += FLinearColor::FromSRGBColor((*Colors)[0]);
						++Count;
					}
				}
				Box.Floor /= float(FMath::Max(Count, 1));
				Art.RoomColors.Add(LBABodyConverter::ToVertexColor(Count > 0 ? Box.Floor.ToFColor(true) : FColor(128, 128, 128), 0.35f));
				for (const FIntPoint& Column : Room.Footprint)
				{
					Box.Min = Box.Min.ComponentMin(FIntVector(Column.X, Room.Level + 1, Column.Y));
					Box.Max = Box.Max.ComponentMax(FIntVector(Column.X, Room.GetCeiling(Column) - 1, Column.Y));
				}
			}

			FLBAMeshBuilder EnclosureBuilder, CeilingBuilder;
			TSet<FIntVector> Covered;
			auto LayoutRowAt = [&Grid, Library](int32 X, int32 Y, int32 Z)
			{
				const FLBAGridCell& Cell = Grid.Get(X, Y, Z);
				return Library->Layouts.IsValidIndex(Cell.Layout) ? FIntPoint(Cell.Layout, Library->Layouts[Cell.Layout].GetBlockCoords(Cell.Block).Y) : FIntPoint(INDEX_NONE, 0);
			};
			EmitEnclosure(Model, Doorways, IsDrawn, LayoutRowAt, *Atlas, Art, EnclosureBuilder, CeilingBuilder, Scale, Covered, CoveredFaces);

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

			if (CeilingBuilder.NumTriangles() > 0)
			{
				FMeshDescription Description;
				TArray<UMaterialInterface*> Materials;
				TArray<FName> Slots;
				CeilingBuilder.BuildStatic(Description, Materials, Slots);
				FLBAStaticMeshOptions Options;
				Options.bComplexAsSimple = true;
				Options.bTwoSidedDistanceField = true;
				CeilingMesh = Context.CreateStaticMesh(Context.GetContentPath(TEXT("Interiors/Grids/Enclosure")), FString::Printf(TEXT("SM_Grid_%03d_Ceiling"), GridIndex),
					MoveTemp(Description), Materials, Slots, Options);
			}
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
			UE_LOG(LogLBA2, Display, TEXT("Grid %d enclosure: %d rooms, %d triangles, %d frame cells removed"), GridIndex, ModelRooms.Num(),
				EnclosureBuilder.NumTriangles() + CeilingBuilder.NumTriangles(), RemovedFrames.Num());
			{
				// The model, for the plugin's Tools (room_model.py, raycast_leaks.py): its rooms, "C x z room floor ceiling"
				// per footprint column, "E x z side kind" per border side, "W x z level" per wall column.
				FString Dump = Model.Describe();
				for (int32 RoomIndex = 0; RoomIndex < ModelRooms.Num(); ++RoomIndex)
				{
					const FLBARoom& Room = ModelRooms[RoomIndex];
					for (const FIntPoint& Column : Room.Footprint)
					{
						Dump += FString::Printf(TEXT("C %d %d %d %d %d\n"), Column.X, Column.Y, RoomIndex, Room.GetFloor(Column), Room.GetCeiling(Column));
					}
					for (const FLBARoomEdge& Edge : Room.Edges)
					{
						Dump += FString::Printf(TEXT("E %d %d %d %d\n"), Edge.Column.X, Edge.Column.Y, Edge.Side, int32(Edge.Kind));
					}
				}
				for (const TPair<FIntPoint, int32>& Wall : Model.GetWalls())
				{
					Dump += FString::Printf(TEXT("W %d %d %d\n"), Wall.Key.X, Wall.Key.Y, Wall.Value);
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
