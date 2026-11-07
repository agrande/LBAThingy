#include "Import/LBAPlanetTowns.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Formats/LBA1.h"
#include "Formats/LBAIso.h"
#include "Formats/LBAXpl.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBABrickMesh.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "Import/LBAMeshBuilder.h"
#include "Import/LBAPlanetGround.h"
#include "Import/LBAPlanetKit.h"
#include "Import/LBARemakeAssets.h"
#include "Math/RandomStream.h"
#include "LBA2Runtime.h"
#include "LBAPlanet.h"
#include "LBATypes.h"

namespace LBAPlanetTownsPrivate
{
	/** The districts' frame: LBA1's units in cm (/ 8, as LBA2's). */
	constexpr float Scale = 0.125f;
	/** Its cells (cm) across and up. */
	constexpr double CellCm = LBA1::CellSize * Scale;
	constexpr double StepCm = LBA1::CellHeight * Scale;
	/** How far (cells) a town's kinds of ground soften into each other. */
	constexpr int32 KindsSoften = 3;

	/** A brick sprite's side, unsheared: one cell along, one up (pixels). */
	constexpr int32 FaceW = FLBABrickSprite::Width / 2;
	constexpr int32 FaceH = 15;

	/** The palette index of a brick sprite's side (0: +Z, the sprite's left half; 1: +X) at U along it and V down it. */
	uint8 SidePixel(const FLBABrickSprite& Sprite, int32 Side, int32 U, int32 V)
	{
		const int32 X = Side == 0 ? U : FaceW + U;
		const int32 Top = FMath::FloorToInt(12.f + (Side == 0 ? X : FLBABrickSprite::Width - 1 - X) * 0.5f);
		const int32 Index = (Top + V) * FLBABrickSprite::Width + X;
		return Sprite.Pixels.IsValidIndex(Index) ? Sprite.Pixels[Index] : 0;
	}

	/**
	 * A wall style's tile: one cell wide, a cell high per row, its rows from the bottom up. Every side of a wall shows the
	 * same art, the brighter of the sprites' two sides (the lit one: the engine shades the others), its holes the mean color.
	 */
	TArray<FColor> MakeWallTile(const LBA1::FData& Data, const TArray<FColor>& Palette, TConstArrayView<int32> Bricks, int32& OutWidth, int32& OutHeight)
	{
		TArray<FLBABrickSprite> Sprites;
		for (const int32 Brick : Bricks)
		{
			FLBABrickSprite& Sprite = Sprites.AddDefaulted_GetRef();
			if (Data.LoadBrick(Brick, Sprite))
			{
				LBAPlanetGround::MovePlantShadows(Sprite);
			}
			Sprite.Pixels.SetNumZeroed(FLBABrickSprite::Width * FLBABrickSprite::Height);
		}
		auto ColorOf = [&Palette](uint8 Index) { return Palette.IsValidIndex(Index) ? Palette[Index] : FColor::Black; };
		float Brightness[2] = {};
		for (int32 Side = 0; Side < 2; ++Side)
		{
			for (const FLBABrickSprite& Sprite : Sprites)
			{
				for (int32 V = 0; V < FaceH; ++V)
				{
					for (int32 U = 0; U < FaceW; ++U)
					{
						const uint8 Index = SidePixel(Sprite, Side, U, V);
						Brightness[Side] += Index ? FLinearColor(ColorOf(Index)).GetLuminance() : 0.f;
					}
				}
			}
		}
		const int32 Side = Brightness[1] > Brightness[0] ? 1 : 0;
		OutWidth = FaceW;
		OutHeight = FaceH * Sprites.Num();
		TArray<FColor> Pixels;
		Pixels.Init(FColor::Black, OutWidth * OutHeight);
		TBitArray<> Holes(false, Pixels.Num());
		FLinearColor Sum = FLinearColor::Black;
		int32 Count = 0;
		for (int32 Row = 0; Row < Sprites.Num(); ++Row)
		{
			for (int32 V = 0; V < FaceH; ++V)
			{
				for (int32 U = 0; U < FaceW; ++U)
				{
					const int32 At = ((Sprites.Num() - 1 - Row) * FaceH + V) * OutWidth + U;
					const uint8 Index = SidePixel(Sprites[Row], Side, U, V);
					if (Index == 0)
					{
						Holes[At] = true;
						continue;
					}
					Pixels[At] = ColorOf(Index);
					Sum += FLinearColor(Pixels[At]);
					++Count;
				}
			}
		}
		const FColor Mean = Count > 0 ? (Sum / float(Count)).ToFColor(false) : FColor(70, 70, 70);
		for (TConstSetBitIterator<> It(Holes); It; ++It)
		{
			Pixels[It.GetIndex()] = Mean;
		}
		return Pixels;
	}

	/**
	 * A town's walls (FTown::WallStyle) drawn from one scene's columns, as few quads as cover their open faces (a face is open
	 * where Cells, the bricks drawn, has none beside it): per side and slice, cells of the same style and phase merged into
	 * rectangles. Their UVs tile the style's texture by position (a cell along, a row up), so merged faces keep the art's size.
	 */
	void EmitWalls(const LBAPlanetDistricts::FTown& Town, const LBABrickMesh::FCells& Cells, int32 Scene, TFunctionRef<int32(int32 Style)> SectionOf,
		FLBAMeshBuilder& Builder)
	{
		// The face's normal axis and sign, and the axes along it and up it (cell axes: 0 x, 1 y up, 2 z).
		struct FSide
		{
			int32 N, U, V, Sign;
		};
		const FSide Sides[6] = { { 0, 2, 1, 1 }, { 0, 2, 1, -1 }, { 2, 0, 1, 1 }, { 2, 0, 1, -1 }, { 1, 0, 2, 1 }, { 1, 0, 2, -1 } };
		auto Position = [](const FIntVector& Cell) { return FVector3f(Cell.X * CellCm, Cell.Z * CellCm, Cell.Y * StepCm); };
		TArray<int32> Mask;
		for (const FSide& Side : Sides)
		{
			const int32 SizeU = Town.Size[Side.U];
			const int32 SizeV = Town.Size[Side.V];
			const bool bVertical = Side.N != 1;
			FIntVector Axis = FIntVector::ZeroValue;
			Axis[Side.N] = Side.Sign;
			const FVector3f Normal(float(Axis.X), float(Axis.Z), float(Axis.Y));
			Mask.SetNumUninitialized(SizeU * SizeV);
			for (int32 Slice = 0; Slice < Town.Size[Side.N]; ++Slice)
			{
				// A key per open wall face: its style and phase (its row less its height, so rows line up up the merged face).
				bool bAny = false;
				for (int32 V = 0; V < SizeV; ++V)
				{
					for (int32 U = 0; U < SizeU; ++U)
					{
						FIntVector Cell;
						Cell[Side.N] = Slice;
						Cell[Side.U] = U;
						Cell[Side.V] = V;
						const int32 Index = Town.Index(Cell.X, Cell.Y, Cell.Z);
						const int32 Style = Town.WallStyle[Index];
						int32 Key = INDEX_NONE;
						if (Style != INDEX_NONE && Town.Scenes[Cell.Z * Town.Size.X + Cell.X] == Scene
							&& Cells.BrickAt(Cell.X + Axis.X, Cell.Y + Axis.Y, Cell.Z + Axis.Z) == 0)
						{
							const int32 Period = Town.WallStyles[Style].Num();
							const int32 Phase = bVertical ? ((int32(Town.WallRow[Index]) - Cell.Y) % Period + Period) % Period : 0;
							Key = Style * 256 + Phase;
							bAny = true;
						}
						Mask[V * SizeU + U] = Key;
					}
				}
				for (int32 V = 0; bAny && V < SizeV; ++V)
				{
					for (int32 U = 0; U < SizeU; ++U)
					{
						const int32 Key = Mask[V * SizeU + U];
						if (Key == INDEX_NONE)
						{
							continue;
						}
						int32 Width = 1;
						while (U + Width < SizeU && Mask[V * SizeU + U + Width] == Key)
						{
							++Width;
						}
						int32 Height = 1;
						for (bool bRow = true; bRow && V + Height < SizeV; Height += bRow ? 1 : 0)
						{
							for (int32 K = 0; K < Width && bRow; ++K)
							{
								bRow = Mask[(V + Height) * SizeU + U + K] == Key;
							}
						}
						for (int32 J = 0; J < Height; ++J)
						{
							for (int32 K = 0; K < Width; ++K)
							{
								Mask[(V + J) * SizeU + U + K] = INDEX_NONE;
							}
						}
						const int32 Style = Key / 256;
						const float Period = float(Town.WallStyles[Style].Num());
						const float Phase = float(Key % 256);
						FLBAVertex Corners[4];
						const FIntPoint Steps[4] = { { 0, 0 }, { Width, 0 }, { Width, Height }, { 0, Height } };
						for (int32 C = 0; C < 4; ++C)
						{
							FIntVector Corner;
							Corner[Side.N] = Slice + (Side.Sign > 0 ? 1 : 0);
							Corner[Side.U] = U + Steps[C].X;
							Corner[Side.V] = V + Steps[C].Y;
							Corners[C].Position = Position(Corner);
							Corners[C].Normal = Normal;
							// The tile's rows run down the texture: row r's bottom is at -r / Period.
							Corners[C].UV = FVector2f(float(Corner[Side.U]), -(Corner[Side.V] + Phase) / Period);
						}
						const int32 Section = SectionOf(Style);
						Builder.AddTriangleFacing(Section, Corners[0], Corners[1], Corners[2], Normal);
						Builder.AddTriangleFacing(Section, Corners[0], Corners[2], Corners[3], Normal);
					}
				}
			}
		}
	}

	/** A ramp: the collision shapes 2 to 5, rising one step over a cell toward +x (2), +z (3), -z (4) or -x (5). */
	bool IsRamp(int32 Shape)
	{
		return Shape >= 2 && Shape <= 5;
	}

	/** A ramp's height (0..1 of a step) at a point of its cell, as its collision (LBAIsoImporter's RampHeight). */
	float RampHeight(int32 Shape, float X, float Z)
	{
		return Shape == 2 ? X : Shape == 3 ? Z : Shape == 4 ? 1.f - Z : Shape == 5 ? 1.f - X : 1.f;
	}

	/** Ramps 4 and 5 face the iso camera (from +x, +z): their sprites hold the slope's art. 2 and 3 it saw edge on. */
	bool FacesCamera(int32 Shape)
	{
		return Shape == 4 || Shape == 5;
	}

	/** A point of a ramp's cell (x, z) along its slope (0 low, 1 high) and across it; and back for another ramp. */
	FVector2f AlongRamp(int32 Shape, float X, float Z)
	{
		return Shape == 2 ? FVector2f(X, Z) : Shape == 3 ? FVector2f(Z, X) : Shape == 4 ? FVector2f(1.f - Z, X) : FVector2f(1.f - X, Z);
	}
	FVector2f FromAlongRamp(int32 Shape, const FVector2f& Along)
	{
		return Shape == 2 ? Along : Shape == 3 ? FVector2f(Along.Y, Along.X) : Shape == 4 ? FVector2f(Along.Y, 1.f - Along.X) : FVector2f(1.f - Along.X, Along.Y);
	}

	/** Where the iso camera drew a point of a cell (x, y up, z, 0..1) in its brick's sprite (pixels; as the cube faces map). */
	FVector2f IsoPixel(float X, float Y, float Z)
	{
		return FVector2f(24.f + 24.f * X - 24.f * Z, 12.f * X + 12.f * Z + 15.f * (1.f - Y));
	}

	/**
	 * Ramps drawn as slopes (rather than cubes, whose faces stretch a slope's art into a sawtooth). Their sprites, whole, in an
	 * atlas of their own: a slope's art spans the sprite's top and sides, which the brick atlas packs apart.
	 */
	struct FSlopeAtlas
	{
		static constexpr int32 Pad = 2;
		static constexpr int32 CellW = FLBABrickSprite::Width + 2 * Pad;
		static constexpr int32 CellH = FLBABrickSprite::Height + 2 * Pad;
		TMap<int32, FIntPoint> Cells;
		int32 Width = 0;
		int32 Height = 0;
		UMaterialInterface* Material = nullptr;
		/** A ramp facing away (brick * 16 + shape): the facing ramp whose art its slope shows (brick, shape). */
		TMap<int32, FIntPoint> Partners;

		/** Atlas UV of a point of a brick's sprite (pixels). */
		FVector2f UV(const FIntPoint& Cell, const FVector2f& Pixel) const
		{
			return FVector2f((Cell.X + Pad + Pixel.X) / Width, (Cell.Y + Pad + Pixel.Y) / Height);
		}
	};

	/**
	 * The slope atlas of an island's ramps (Ramps: brick * 16 + shape), its colors as the brick atlas's; each sprite grown by
	 * its padding so a slope's edges, on the sprite's outline, don't sample transparency. A ramp facing away is given the
	 * facing ramp (the mirror shape first) whose mean color is nearest its own.
	 */
	TArray<FColor> MakeSlopeAtlas(const LBA1::FData& Data, const TArray<FColor>& Palette, const TSet<int32>& Ramps, FSlopeAtlas& Atlas)
	{
		TArray<int32> Bricks;
		for (const int32 Ramp : Ramps)
		{
			Bricks.AddUnique(Ramp / 16);
		}
		Bricks.Sort();
		const int32 Columns = FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(float(Bricks.Num()))));
		Atlas.Width = Columns * FSlopeAtlas::CellW;
		Atlas.Height = FMath::Max(1, FMath::DivideAndRoundUp(Bricks.Num(), Columns)) * FSlopeAtlas::CellH;
		TArray<FColor> Pixels;
		Pixels.Init(FColor(0, 0, 0, 0), Atlas.Width * Atlas.Height);
		TMap<int32, FLinearColor> Means;
		for (int32 Index = 0; Index < Bricks.Num(); ++Index)
		{
			const FIntPoint Cell((Index % Columns) * FSlopeAtlas::CellW, (Index / Columns) * FSlopeAtlas::CellH);
			Atlas.Cells.Add(Bricks[Index], Cell);
			FLBABrickSprite Sprite;
			if (!Data.LoadBrick(Bricks[Index], Sprite))
			{
				continue;
			}
			LBAPlanetGround::MovePlantShadows(Sprite);
			FLinearColor Sum = FLinearColor::Black;
			int32 Count = 0;
			for (int32 Y = 0; Y < FLBABrickSprite::Height; ++Y)
			{
				for (int32 X = 0; X < FLBABrickSprite::Width; ++X)
				{
					const uint8 Pixel = Sprite.Pixels.IsValidIndex(Y * FLBABrickSprite::Width + X) ? Sprite.Pixels[Y * FLBABrickSprite::Width + X] : 0;
					if (Pixel != 0 && Palette.IsValidIndex(Pixel))
					{
						FColor Color = Palette[Pixel];
						Color.A = 255;
						Pixels[(Cell.Y + FSlopeAtlas::Pad + Y) * Atlas.Width + Cell.X + FSlopeAtlas::Pad + X] = Color;
						Sum += FLinearColor(Color);
						++Count;
					}
				}
			}
			Means.Add(Bricks[Index], Count > 0 ? Sum / float(Count) : FLinearColor::Black);
			// Grown by the padding, a pixel at a time.
			for (int32 Grow = 0; Grow < FSlopeAtlas::Pad; ++Grow)
			{
				const TArray<FColor> Before = Pixels;
				for (int32 Y = 0; Y < FSlopeAtlas::CellH; ++Y)
				{
					for (int32 X = 0; X < FSlopeAtlas::CellW; ++X)
					{
						FColor& Here = Pixels[(Cell.Y + Y) * Atlas.Width + Cell.X + X];
						for (const FIntPoint Step : { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) })
						{
							const FIntPoint At(X + Step.X, Y + Step.Y);
							if (Here.A == 0 && At.X >= 0 && At.Y >= 0 && At.X < FSlopeAtlas::CellW && At.Y < FSlopeAtlas::CellH
								&& Before[(Cell.Y + At.Y) * Atlas.Width + Cell.X + At.X].A != 0)
							{
								Here = Before[(Cell.Y + At.Y) * Atlas.Width + Cell.X + At.X];
							}
						}
					}
				}
			}
		}
		for (const int32 Ramp : Ramps)
		{
			const int32 Brick = Ramp / 16;
			const int32 Shape = Ramp % 16;
			if (FacesCamera(Shape))
			{
				continue;
			}
			const int32 Mirror = Shape == 2 ? 5 : 4;
			FIntPoint Best(INDEX_NONE, INDEX_NONE);
			float BestDistance = MAX_flt;
			for (const int32 Other : Ramps)
			{
				const int32 OtherShape = Other % 16;
				if (!FacesCamera(OtherShape))
				{
					continue;
				}
				const FLinearColor Difference = Means.FindRef(Brick) - Means.FindRef(Other / 16);
				const float Distance = FMath::Square(Difference.R) + FMath::Square(Difference.G) + FMath::Square(Difference.B) + (OtherShape == Mirror ? 0.f : 1e-4f);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					Best = FIntPoint(Other / 16, OtherShape);
				}
			}
			if (Best.X != INDEX_NONE)
			{
				Atlas.Partners.Add(Ramp, Best);
			}
		}
		return Pixels;
	}

	/**
	 * A scene's ramps as slopes: the top along the collision's surface, its art where the iso camera saw it (a facing ramp's
	 * own sprite, or its partner's read along the slope); the sides, where open, a flat fill of the brick's side color.
	 */
	void EmitSlopes(const LBAPlanetDistricts::FTown& Town, const LBABrickMesh::FCells& Cells, int32 Scene, const FSlopeAtlas& Slopes,
		const FLBAIsoImporter::FAtlas& Atlas, FLBAMeshBuilder& Builder)
	{
		int32 Section = INDEX_NONE;
		int32 FillSection = INDEX_NONE;
		auto Position = [](float X, float Y, float Z) { return FVector3f(X * CellCm, Z * CellCm, Y * StepCm); };
		for (int32 Z = 0; Z < Town.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Town.Size.X; ++X)
			{
				if (Town.Scenes[Z * Town.Size.X + X] != Scene)
				{
					continue;
				}
				for (int32 Y = 0; Y < Town.Size.Y; ++Y)
				{
					const int32 Brick = Cells.BrickAt(X, Y, Z);
					const int32 Shape = Cells.ShapeAt(X, Y, Z);
					const int32 Index = Town.Index(X, Y, Z);
					if (!Brick || !IsRamp(Shape) || Town.WallStyle[Index] != INDEX_NONE)
					{
						continue;
					}
					FIntPoint Art(Brick, Shape);
					const FIntPoint* Partner = FacesCamera(Shape) ? nullptr : Slopes.Partners.Find(Brick * 16 + Shape);
					if (Partner)
					{
						Art = *Partner;
					}
					const FIntPoint* ArtCell = Slopes.Cells.Find(Art.X);
					if (!ArtCell)
					{
						continue;
					}
					// The slope: corners (0,0), (1,0), (1,1), (0,1) in x, z.
					const FVector2f Steps[4] = { { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } };
					FLBAVertex Top[4];
					const FVector2f Gradient(RampHeight(Shape, 1.f, 0.f) - RampHeight(Shape, 0.f, 0.f), RampHeight(Shape, 0.f, 1.f) - RampHeight(Shape, 0.f, 0.f));
					const FVector3f Normal = FVector3f(-Gradient.X * StepCm / CellCm, -Gradient.Y * StepCm / CellCm, 1.f).GetSafeNormal();
					for (int32 C = 0; C < 4; ++C)
					{
						const float H = RampHeight(Shape, Steps[C].X, Steps[C].Y);
						Top[C].Position = Position(X + Steps[C].X, Y + H, Z + Steps[C].Y);
						Top[C].Normal = Normal;
						const FVector2f ArtXZ = Art.Y == Shape ? Steps[C] : FromAlongRamp(Art.Y, AlongRamp(Shape, Steps[C].X, Steps[C].Y));
						Top[C].UV = Slopes.UV(*ArtCell, IsoPixel(ArtXZ.X, RampHeight(Art.Y, ArtXZ.X, ArtXZ.Y), ArtXZ.Y));
					}
					Section = Section != INDEX_NONE ? Section : Builder.GetSection(TEXT("Slopes"), Slopes.Material);
					Builder.AddTriangleFacing(Section, Top[0], Top[1], Top[2], Normal);
					Builder.AddTriangleFacing(Section, Top[0], Top[2], Top[3], Normal);

					// The open sides, following the slope's heights along their top edge.
					const TStaticArray<FColor, 3>* Colors = Atlas.FaceColors.Find(Brick);
					struct FSide
					{
						FIntVector Dir;
						FVector2f A, B;
					};
					const FSide Sides[4] = { { { 1, 0, 0 }, { 1.f, 0.f }, { 1.f, 1.f } }, { { -1, 0, 0 }, { 0.f, 0.f }, { 0.f, 1.f } },
						{ { 0, 0, 1 }, { 0.f, 1.f }, { 1.f, 1.f } }, { { 0, 0, -1 }, { 0.f, 0.f }, { 1.f, 0.f } } };
					for (const FSide& Side : Sides)
					{
						if (Cells.BrickAt(X + Side.Dir.X, Y, Z + Side.Dir.Z) != 0)
						{
							continue;
						}
						const float HA = RampHeight(Shape, Side.A.X, Side.A.Y);
						const float HB = RampHeight(Shape, Side.B.X, Side.B.Y);
						if (HA + HB <= 0.f)
						{
							continue;
						}
						const FVector3f Outward(float(Side.Dir.X), float(Side.Dir.Z), 0.f);
						const FColor Color = Colors ? (*Colors)[Side.Dir.X != 0 ? 1 : 2] : FColor(128, 128, 128);
						FLBAVertex V[4];
						V[0].Position = Position(X + Side.A.X, Y, Z + Side.A.Y);
						V[1].Position = Position(X + Side.B.X, Y, Z + Side.B.Y);
						V[2].Position = Position(X + Side.B.X, Y + HB, Z + Side.B.Y);
						V[3].Position = Position(X + Side.A.X, Y + HA, Z + Side.A.Y);
						for (FLBAVertex& Vertex : V)
						{
							Vertex.Normal = Outward;
							Vertex.Color = LBABodyConverter::ToVertexColor(Color, 0.45f);
						}
						FillSection = FillSection != INDEX_NONE ? FillSection : Builder.GetSection(TEXT("BrickFill"), Atlas.FillMaterial);
						if (HB > 0.f)
						{
							Builder.AddTriangleFacing(FillSection, V[0], V[1], V[2], Outward);
						}
						if (HA > 0.f)
						{
							Builder.AddTriangleFacing(FillSection, V[0], HB > 0.f ? V[2] : V[1], V[3], Outward);
						}
					}
				}
			}
		}
	}
}

void LBAPlanetTowns::EmitGround(const LBAPlanetDistricts::FDistricts& Districts, const LBAPlanetKit::FTable& Kit, TConstArrayView<LBAPlanetDistricts::FPlaced> Placed, const FVector& Pivot,
	FLBAMeshBuilder& Builder, int32 Section)
{
	using namespace LBAPlanetTownsPrivate;
	for (const LBAPlanetDistricts::FPlaced& District : Placed)
	{
		LBAPlanetDistricts::FTown Town;
		if (!Districts.MakeTown(District, Town, &Kit))
		{
			continue;
		}
		const LBA1::FData& Data = Districts.GetData();
		auto SharesAt = [&](int32 X, int32 Y, int32 Z) -> const LBAPlanetDistricts::FGroundShares*
		{
			return X >= 0 && Z >= 0 && Y >= 0 && X < Town.Size.X && Y < Town.Size.Y && Z < Town.Size.Z ? Districts.GetGroundShares(Town.Bricks[Town.Index(X, Y, Z)]) : nullptr;
		};
		// A corner's kinds: the ground bricks at its level within KindsSoften cells, the nearer the more. Their outlines lose LBA1's
		// staircases, and the material's noise makes them irregular.
		auto CornerAt = [&](int32 X, int32 Y, int32 Z)
		{
			float Grass = 0.f, Earth = 0.f, Dark = 0.f, Weights = 0.f;
			for (int32 DZ = -KindsSoften; DZ < KindsSoften; ++DZ)
			{
				for (int32 DX = -KindsSoften; DX < KindsSoften; ++DX)
				{
					if (const LBAPlanetDistricts::FGroundShares* Shares = SharesAt(X + DX, Y, Z + DZ))
					{
						const float Weight = float(FMath::Square(KindsSoften) - FMath::Square(DX + 0.5f) * 0.5f - FMath::Square(DZ + 0.5f) * 0.5f);
						Grass += Weight * Shares->Grass;
						Earth += Weight * Shares->Earth;
						Dark += Weight * Shares->Dark;
						Weights += Weight;
					}
				}
			}
			FLBAVertex Vertex;
			const FVector Frame = Town.GetOrigin() + FVector(X * CellCm, Z * CellCm, (Y + 1) * StepCm);
			Vertex.Position = FVector3f(District.FrameToMap.TransformPosition(Frame) - Pivot);
			Vertex.Normal = FVector3f::UpVector;
			const FLinearColor Linear = FLinearColor::FromSRGBColor(FColor(uint8(255.f * Grass / Weights + 0.5f), uint8(255.f * Earth / Weights + 0.5f), 0));
			Vertex.Color = FVector4f(Linear.R, Linear.G, 0.f, 0.f);
			Vertex.UV = FVector2f(Dark / Weights, 0.f);
			return Vertex;
		};
		for (int32 Z = 0; Z < Town.Size.Z; ++Z)
		{
			for (int32 X = 0; X < Town.Size.X; ++X)
			{
				for (int32 Y = 0; Y < Town.Size.Y; ++Y)
				{
					// Under a brick, no ground; under a kit model, ground.
					const int32 Above = Y + 1 < Town.Size.Y ? Town.Index(X, Y + 1, Z) : INDEX_NONE;
					if (!SharesAt(X, Y, Z) || (Above != INDEX_NONE && Town.Bricks[Above] != 0 && !Data.IsBlankBrick(Town.Bricks[Above]) && !Town.Replaced[Above]))
					{
						continue;
					}
					const FLBAVertex A = CornerAt(X, Y, Z);
					const FLBAVertex B = CornerAt(X + 1, Y, Z);
					const FLBAVertex C = CornerAt(X, Y, Z + 1);
					const FLBAVertex D = CornerAt(X + 1, Y, Z + 1);
					Builder.AddTriangleFacing(Section, A, B, D, FVector3f::UpVector);
					Builder.AddTriangleFacing(Section, A, D, C, FVector3f::UpVector);
				}
			}
		}
	}
}

int32 LBAPlanetTowns::Build(FLBAImportContext& Context, UWorld* World, const FString& Folder, const FString& Island, const LBAPlanetDistricts::FDistricts& Districts,
	const LBAPlanetKit::FTable& Kit, TConstArrayView<LBAPlanetDistricts::FPlaced> Placed, double Radius)
{
	using namespace LBAPlanetTownsPrivate;
	const LBA1::FData& Data = Districts.GetData();
	const FString TownFolder = Folder / TEXT("Towns");

	// The island's towns, and one atlas of all their bricks.
	TArray<LBAPlanetDistricts::FTown> Towns;
	TArray<const LBAPlanetDistricts::FPlaced*> TownPlaces;
	TSet<int32> Used;
	// The ramps drawn as slopes: brick * 16 + shape.
	TSet<int32> Ramps;
	for (const LBAPlanetDistricts::FPlaced& District : Placed)
	{
		LBAPlanetDistricts::FTown Town;
		if (!Districts.MakeTown(District, Town, &Kit))
		{
			Context.Warn(FString::Printf(TEXT("LBA1's %s on %s: its scenes' grids can't be read"), *District.Name, *Island));
			continue;
		}
		for (int32 Index = 0; Index < Town.Bricks.Num(); ++Index)
		{
			if (Town.Bricks[Index] != 0 && !Town.Replaced[Index] && Town.WallStyle[Index] == INDEX_NONE)
			{
				Used.Add(Town.Bricks[Index]);
				if (IsRamp(Town.Shapes[Index]) && !Data.IsBlankBrick(Town.Bricks[Index]))
				{
					Ramps.Add(Town.Bricks[Index] * 16 + Town.Shapes[Index]);
				}
			}
		}
		Towns.Add(MoveTemp(Town));
		TownPlaces.Add(&District);
	}
	if (Towns.IsEmpty())
	{
		return 0;
	}
	TArray<int32> Bricks = Used.Array();
	Bricks.Sort();
	FLBAIsoImporter::FAtlas Atlas;
	TArray<FColor> Pixels;
	// LBA1's colors moved to the home island's palette (LBAPlanetGround): by ramp, and plants' shadows with their plants.
	const FLBAXpl* HomeXpl = Context.GetXpl(FLBAXpl::GetEntryForIsland(TEXT("CITABAU")));
	const TArray<FColor> Palette = HomeXpl ? LBAPlanetGround::MakeTownPalette(Data.GetPalette(), HomeXpl->Palette) : Data.GetPalette();
	LBABrickMesh::MakeAtlas(Bricks, [&Data](int32 Brick, FLBABrickSprite& Sprite)
	{
		if (!Data.LoadBrick(Brick, Sprite))
		{
			return false;
		}
		LBAPlanetGround::MovePlantShadows(Sprite);
		return true;
	}, Palette, Atlas, Pixels);
	Atlas.Texture = Context.CreateTexture(TownFolder, FString::Printf(TEXT("T_LBA1_%s_Bricks"), *Island), Atlas.Width, Atlas.Height, Pixels, false);
	Atlas.Material = Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Atlas.Texture, TownFolder);
	Atlas.FillMaterial = Context.GetMaterials().GetMaster(ELBAMaterial::Color);
	if (!Atlas.Texture || !Atlas.Material)
	{
		return 0;
	}
	FSlopeAtlas Slopes;
	if (Ramps.Num() > 0)
	{
		const TArray<FColor> SlopePixels = MakeSlopeAtlas(Data, Palette, Ramps, Slopes);
		if (UTexture2D* Texture = Context.CreateTexture(TownFolder, FString::Printf(TEXT("T_LBA1_%s_Slopes"), *Island), Slopes.Width, Slopes.Height, SlopePixels, false))
		{
			Slopes.Material = Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Texture, TownFolder);
		}
	}

	// Per scene (the columns drawn from it): its collision from the bricks' shapes first (the render mesh cooks its own
	// from it), then its cubes.
	struct FPiece
	{
		int32 Town = 0;
		int32 Scene = 0;
		UStaticMesh* Collision = nullptr;
	};
	TArray<FPiece> Pieces;
	auto CellsOf = [&Data](const LBAPlanetDistricts::FTown& Town, bool bVisual)
	{
		LBABrickMesh::FCells Cells;
		Cells.Size = Town.Size;
		// A blank brick is only collision: the faces around it are seen. So is a kit piece's brick.
		Cells.Brick = [&Town, &Data, bVisual](int32 X, int32 Y, int32 Z)
		{
			const int32 Index = Town.Index(X, Y, Z);
			const int32 Brick = Town.Bricks[Index];
			return Brick != 0 && !Data.IsBlankBrick(Brick) && !(bVisual && Town.Replaced[Index]) ? Brick : 0;
		};
		Cells.Shape = [&Town](int32 X, int32 Y, int32 Z) { return int32(Town.Shapes[Town.Index(X, Y, Z)]); };
		return Cells;
	};
	for (int32 TownIndex = 0; TownIndex < Towns.Num(); ++TownIndex)
	{
		const LBAPlanetDistricts::FTown& Town = Towns[TownIndex];
		const LBABrickMesh::FCells Cells = CellsOf(Town, false);
		for (const int32 Scene : TownPlaces[TownIndex]->Scenes)
		{
			FLBAMeshBuilder Builder;
			LBABrickMesh::EmitCollision(Cells, Builder, Builder.GetSection(TEXT("Collision"), Context.GetMaterials().GetMaster(ELBAMaterial::Invisible)), Scale,
				[&Town, Scene](const FIntVector& Cell) { return Town.Scenes[Cell.Z * Town.Size.X + Cell.X] == Scene; });
			if (Builder.IsEmpty())
			{
				continue;
			}
			FMeshDescription Description;
			TArray<UMaterialInterface*> Materials;
			TArray<FName> Slots;
			Builder.BuildStatic(Description, Materials, Slots);
			FLBAStaticMeshOptions Options;
			Options.bComplexAsSimple = true;
			Options.bNoDistanceField = true;
			Pieces.Add({ TownIndex, Scene, Context.CreateStaticMesh(TownFolder / TEXT("Collision"), FString::Printf(TEXT("SM_LBA1_Scene_%03d_Collision"), Scene),
				MoveTemp(Description), Materials, Slots, Options) });
		}
	}
	Context.FinishPendingBuilds();

	// The walls' materials, one per style over the island's towns (T_LBA1_<ISLAND>_Wall_<first brick>_<rows>).
	TMap<FString, UMaterialInterface*> WallMaterials;
	auto WallMaterialOf = [&](const TArray<int32>& Bricks) -> UMaterialInterface*
	{
		const FString Key = FString::JoinBy(Bricks, TEXT(","), [](int32 Brick) { return FString::FromInt(Brick); });
		if (UMaterialInterface** Found = WallMaterials.Find(Key))
		{
			return *Found;
		}
		int32 Width = 0, Height = 0;
		const TArray<FColor> TilePixels = MakeWallTile(Data, Palette, Bricks, Width, Height);
		UTexture2D* Texture = Context.CreateTexture(TownFolder, FString::Printf(TEXT("T_LBA1_%s_Wall_%d_%d"), *Island, Bricks[0], Bricks.Num()), Width, Height, TilePixels, true);
		UMaterialInterface* Material = Texture ? Context.GetMaterials().GetTextured(ELBAMaterial::Brick, Texture, TownFolder) : Atlas.FillMaterial;
		return WallMaterials.Add(Key, Material);
	};

	int32 Built = 0;
	int32 WallTriangles = 0;
	for (const FPiece& Piece : Pieces)
	{
		const LBAPlanetDistricts::FTown& Town = Towns[Piece.Town];
		const LBAPlanetDistricts::FPlaced& District = *TownPlaces[Piece.Town];
		FLBAMeshBuilder Builder;
		const int32 Scene = Piece.Scene;
		const LBABrickMesh::FCells Visual = CellsOf(Town, true);
		LBABrickMesh::EmitVisual(Visual, Atlas, Builder, Builder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Brick), Atlas.Material), Scale,
			Context.GetProjectSettings().BackFaces, [&Town, &Districts, &Slopes, Scene](const FIntVector& Cell, int32)
			{
				const int32 Index = Town.Index(Cell.X, Cell.Y, Cell.Z);
				return Town.Scenes[Cell.Z * Town.Size.X + Cell.X] == Scene && !Districts.GetGroundShares(Town.Bricks[Index]) && Town.WallStyle[Index] == INDEX_NONE
					&& !(Slopes.Material && IsRamp(Town.Shapes[Index]));
			});
		if (Slopes.Material)
		{
			EmitSlopes(Town, Visual, Scene, Slopes, Atlas, Builder);
		}
		const int32 BrickTriangles = Builder.NumTriangles();
		EmitWalls(Town, Visual, Scene, [&](int32 Style)
		{
			UMaterialInterface* Material = WallMaterialOf(Town.WallStyles[Style]);
			return Builder.GetSection(FName(Material->GetName()), Material);
		}, Builder);
		WallTriangles += Builder.NumTriangles() - BrickTriangles;
		if (Builder.IsEmpty())
		{
			continue;
		}
		const int32 Triangles = Builder.NumTriangles();
		FMeshDescription Description;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> Slots;
		Builder.BuildStatic(Description, Materials, Slots);
		FLBAStaticMeshOptions Options;
		Options.ComplexCollisionMesh = Piece.Collision;
		Options.bDeferBuild = false;
		UStaticMesh* Mesh = Context.CreateStaticMesh(TownFolder, FString::Printf(TEXT("SM_LBA1_Scene_%03d"), Scene), MoveTemp(Description), Materials, Slots, Options);
		if (!Mesh)
		{
			continue;
		}
		// Bent onto the planet, it goes down around it: culled by where it is seen, not by where it is.
		Mesh->SetNegativeBoundsExtension(FVector(0.0, 0.0, 2.0 * Radius));
		Mesh->CalculateExtendedBounds();
		Context.MarkForSave(Mesh);
		const FTransform Transform = FTransform(Town.GetOrigin()) * District.FrameToMap;
		if (AStaticMeshActor* Actor = Context.SpawnGeneratedActor<AStaticMeshActor>(World, Transform, FString::Printf(TEXT("Town_%s_Scene_%03d"), *Island, Scene), TEXT("Stubs/Towns")))
		{
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetStaticMesh(Mesh);
			// Brick art is drawn already shaded, as in the interiors (LBA::PredrawnArtStencil).
			Component->SetRenderCustomDepth(true);
			Component->SetCustomDepthStencilValue(LBA::PredrawnArtStencil);
			Actor->Tags.Add(ULBAPlanetAsset::StubTag);
		}
		UE_LOG(LogLBA2, Display, TEXT("LBA1 town %s on %s: scene %d, %d triangles"), *District.Name, *Island, Scene, Triangles);
		++Built;
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA1 towns on %s: %d wall styles, %d wall triangles"), *Island, WallMaterials.Num(), WallTriangles);

	// The kit's models, instanced per model over the island's towns.
	FLBARemakeAssets Remake(Context);
	bool bRemakeTried = false;
	bool bRemake = false;
	TMap<int32, UInstancedStaticMeshComponent*> Instancers;
	for (int32 TownIndex = 0; TownIndex < Towns.Num(); ++TownIndex)
	{
		const LBAPlanetDistricts::FTown& Town = Towns[TownIndex];
		const LBAPlanetDistricts::FPlaced& District = *TownPlaces[TownIndex];
		for (const LBAPlanetDistricts::FTown::FPiece& Piece : Town.Pieces)
		{
			const LBAPlanetKit::FModel& Model = Kit.GetModel(Piece.Model);
			UInstancedStaticMeshComponent** Instancer = Instancers.Find(Piece.Model);
			if (!Instancer)
			{
				UStaticMesh* Mesh = nullptr;
				if (!Model.Primitive.IsEmpty())
				{
					// Generated once per island, in the piece's frame, colored like its bricks (SM_LBA1_<ISLAND>_Kit_<name>).
					FLBAMeshBuilder KitBuilder;
					const int32 KitSection = KitBuilder.GetSection(FLBAMaterialLibrary::GetSlotName(ELBAMaterial::Color), Atlas.FillMaterial);
					if (LBAPlanetKit::BuildPrimitive(Model, Data, Palette, CellCm, StepCm, KitBuilder, KitSection))
					{
						FMeshDescription Description;
						TArray<UMaterialInterface*> Materials;
						TArray<FName> Slots;
						KitBuilder.BuildStatic(Description, Materials, Slots);
						FLBAStaticMeshOptions Options;
						Options.bNoCollision = true;
						Options.bDeferBuild = false;
						Options.bNoDistanceField = true;
						Mesh = Context.CreateStaticMesh(TownFolder / TEXT("Kit"), FString::Printf(TEXT("SM_LBA1_%s_Kit_%s"), *Island, *FLBAImportContext::SanitizeName(Model.Name)),
							MoveTemp(Description), Materials, Slots, Options);
						if (Mesh)
						{
							Mesh->SetNegativeBoundsExtension(FVector(0.0, 0.0, 2.0 * Radius));
							Mesh->CalculateExtendedBounds();
							Context.MarkForSave(Mesh);
						}
					}
				}
				else if (Model.Body != INDEX_NONE)
				{
					Mesh = Context.FindAsset<UStaticMesh>(Context.GetContentPath(TEXT("Islands") / Model.Island / TEXT("Decor")), FString::Printf(TEXT("SM_%s_Decor_%03d"), *Model.Island, Model.Body));
				}
				else
				{
					if (!bRemakeTried)
					{
						bRemakeTried = true;
						bRemake = Remake.Initialize();
					}
					Mesh = bRemake ? Remake.GetModelMesh(FPaths::GetPath(Model.RemakeFile), FPaths::GetCleanFilename(Model.RemakeFile)) : nullptr;
				}
				UInstancedStaticMeshComponent* Component = nullptr;
				if (!Mesh)
				{
					Context.Warn(FString::Printf(TEXT("LBA1 kit on %s: no mesh for %s (%s), it stays bricks"), *Island, *Model.Name,
						!Model.Primitive.IsEmpty() ? *FString::Printf(TEXT("a generated %s"), *Model.Primitive)
						: Model.Body != INDEX_NONE ? *FString::Printf(TEXT("%s decor %d: import the island first"), *Model.Island, Model.Body) : *Model.RemakeFile));
				}
				else if (AActor* Actor = Context.SpawnGeneratedActor<AActor>(World, FTransform::Identity,
					FString::Printf(TEXT("Kit_%s_%s"), *Island, *FLBAImportContext::SanitizeName(Model.Name)), TEXT("Stubs/Kit")))
				{
					Component = NewObject<UInstancedStaticMeshComponent>(Actor, TEXT("Instances"));
					Component->SetStaticMesh(Mesh);
					Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					Actor->SetRootComponent(Component);
					Actor->AddInstanceComponent(Component);
					Component->RegisterComponent();
					Actor->Tags.Add(ULBAPlanetAsset::StubTag);
				}
				Instancer = &Instancers.Add(Piece.Model, Component);
			}
			if (!*Instancer)
			{
				continue;
			}
			// On the footprint's center and the piece's lowest cell; copies spread along its longer side.
			const FBox Box = (*Instancer)->GetStaticMesh()->GetBoundingBox();
			const double Size = Model.Scale * (Model.bFitHeight ? Piece.Size.Y * StepCm / FMath::Max(Box.GetSize().Z, 1.0) : 1.0);
			const bool bAlongX = Piece.Size.X >= Piece.Size.Z;
			const int32 Copies = Model.Every > 0.0 ? FMath::Max(1, FMath::RoundToInt(FMath::Max(Piece.Size.X, Piece.Size.Z) / Model.Every)) : 1;
			FRandomStream Random(HashCombine(GetTypeHash(Piece.Low), GetTypeHash(Piece.Scene)));
			for (int32 Copy = 0; Copy < Copies; ++Copy)
			{
				const double Along = (Copy + 0.5) / Copies;
				// A fitted model sits on the piece's floor: some bodies' pivots are far under them (Citadel's rock 2: 2.25 m).
				const FVector Frame = Town.GetOrigin() + FVector((Piece.Low.X + Piece.Size.X * (bAlongX ? Along : 0.5)) * CellCm,
					(Piece.Low.Z + Piece.Size.Z * (bAlongX ? 0.5 : Along)) * CellCm, Piece.Low.Y * StepCm - (Model.bFitHeight ? Box.Min.Z * Size : 0.0));
				const double Yaw = Model.Yaw + (Model.bRandomTurn ? Random.FRandRange(0.0, 360.0) : 0.0);
				(*Instancer)->AddInstance(FTransform(FRotator(0.0, Yaw, 0.0), Frame, FVector(Size)) * District.FrameToMap, true);
			}
		}
	}
	for (const TPair<int32, UInstancedStaticMeshComponent*>& Pair : Instancers)
	{
		if (Pair.Value)
		{
			UE_LOG(LogLBA2, Display, TEXT("LBA1 kit on %s: %s, %d copies"), *Island, *Kit.GetModel(Pair.Key).Name, Pair.Value->GetInstanceCount());
		}
	}
	Context.Flush();
	return Built;
}
