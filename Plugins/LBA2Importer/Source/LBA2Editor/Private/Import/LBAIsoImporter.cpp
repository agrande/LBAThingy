#include "Import/LBAIsoImporter.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBAEnvironment.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBARemakeAssets.h"
#include "LBA2ImporterSettings.h"
#include "LBA2Runtime.h"
#include "LBATypes.h"
#include "PhysicsEngine/AggregateGeom.h"

#define LOCTEXT_NAMESPACE "LBAIsoImporter"

namespace
{
	constexpr int32 SpriteW = FLBABrickSprite::Width;
	constexpr int32 SpriteH = FLBABrickSprite::Height;
	constexpr int32 CellPad = 2;
	constexpr int32 CellW = SpriteW + CellPad * 2;
	constexpr int32 CellH = SpriteH + CellPad * 2;

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
							constexpr float SolidCoverage = 0.85f;
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
		EmitVisual(Query, *Atlas, Builder, Section, Scale, Context.GetSettings().BackFaces);
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

	FCellQuery VisualQuery = Query;
	if (Remade.HiddenCells.Num() > 0)
	{
		const TSet<FIntVector>* Hidden = &Remade.HiddenCells;
		VisualQuery.Brick = [BaseBrick = Query.Brick, Hidden](int32 X, int32 Y, int32 Z)
		{
			return Hidden->Contains(FIntVector(X, Y, Z)) ? 0 : BaseBrick(X, Y, Z);
		};
	}

	FLBAMeshBuilder Builder;
	EmitVisual(VisualQuery, *Atlas, Builder, Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas->Material), Scale, Context.GetSettings().BackFaces);
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

	if (!Context.GetSettings().bCreateInteriorLevels || !Mesh)
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
	const ULBA2ImporterSettings& Settings = Context.GetSettings();

	TArray<int32> Grids;
	for (int32 Grid = 0; Grid < Header.GetNumGrids(); ++Grid)
	{
		if (Settings.GridFilter.Num() == 0 || Settings.GridFilter.Contains(Grid))
		{
			Grids.Add(Grid);
		}
	}

	if (Settings.bCreateLayoutKitMeshes)
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
