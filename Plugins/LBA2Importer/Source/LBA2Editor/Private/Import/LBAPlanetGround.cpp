#include "Import/LBAPlanetGround.h"

#include "Engine/Texture2D.h"
#include "Formats/LBA1.h"
#include "Formats/LBAHqr.h"
#include "Formats/LBAIso.h"
#include "Formats/LBAXpl.h"
#include "Import/LBAImportContext.h"
#include "Import/LBAMaterialLibrary.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"

namespace LBAPlanetGroundPrivate
{
	/** The texture page of a *.ILE. */
	constexpr int32 PageEntry = 1;
	constexpr int32 PageSize = 256;

	/** A tile of a page (texels). */
	struct FTileRect
	{
		int32 X;
		int32 Y;
		int32 Size;
	};
	// Citabau's most used rock and grass, its pale stone and bare ground (for when there's no LBA1), the desert's sand.
	// Their wrap is about as smooth as their inside: they tile without help.
	constexpr FTileRect CitabauRock = { 128, 32, 64 };
	constexpr FTileRect CitabauPale = { 0, 32, 64 };
	constexpr FTileRect CitabauGrass = { 32, 96, 32 };
	constexpr FTileRect CitabauEarth = { 96, 32, 32 };
	constexpr FTileRect DesertSand = { 64, 64, 64 };

	/**
	 * LBA1's earth bricks (cell z * 3 + x), 3 across and 2 deep. It has two 3 x 3 layouts that fade from dark to light
	 * along z and back: a tile of whole layouts shows as dark bands from afar. These are the light rows where the two meet
	 * (first layout's last row, second's first), all equally bright. Its grass is finer and flatter than LBA2's leafy one,
	 * and natural ground should look like LBA2's: there Citabau's own tile serves.
	 */
	constexpr int32 EarthBricks = 553;
	constexpr int32 RowCells = 3;
	constexpr int32 RowsDeep = 2;
	/** LBA1's dark ground, a 3 x 3 layout, evenly lit. */
	constexpr int32 DarkBricks = 964;
	constexpr int32 DarkRowsDeep = 3;
	/** The composed tile, a whole number of those. Its world size is in LBAStubGround.ush. */
	constexpr int32 TileCells = 6;
	constexpr int32 TileSize = 256;
	/** A brick's top diamond on screen: half its width and height. */
	constexpr int32 HalfWidth = 24;
	constexpr int32 HalfHeight = 12;

	/** LBA1's palette ramps: its plants, their shadows (also its blue-grey stone) and its earth. */
	constexpr int32 PlantRamp = 2;
	constexpr int32 PlantShadowRamp = 11;
	constexpr int32 EarthRamp = 8;
	/**
	 * Those ramps' place in the home palette, and the steps the tile's middle goes to: the original's ground as seen on screen
	 * (sunny_47: grass 100,112,72, earth 88,60,32), brick art being drawn already shaded.
	 */
	constexpr int32 HomePlantRamp = 7;
	constexpr int32 HomePlantStep = 8;
	constexpr int32 HomeEarthRamp = 1;
	constexpr int32 HomeEarthStep = 7;
	/** The middle of LBA1's tiles of those (brightness), and how much of their contrast stays on the home ramp. */
	constexpr float PlantMiddle = 75.f;
	constexpr float PlantContrast = 0.6f;
	constexpr float EarthMiddle = 95.f;
	/** A plant's shadows are the shadow ramp's pixels this close to its own (pixels): further off, it's stone with moss on it. */
	constexpr int32 PlantShadowReach = 3;

	float Luma(const FColor& Color)
	{
		return 0.299f * Color.R + 0.587f * Color.G + 0.114f * Color.B;
	}

	/** Brightness of LBA1's ground, its rows of bricks from FirstBrick composed as the game draws them. */
	bool ComposeBricks(const LBA1::FData& Data, int32 FirstBrick, int32 Deep, TArray<float>& OutLuma)
	{
		// Copies all around the middle tile, drawn back to front: each brick overlaps its neighbours by a pixel.
		constexpr int32 Min = -TileCells;
		constexpr int32 Max = 2 * TileCells;
		const int32 OriginX = (Max - Min) * HalfWidth;
		const int32 OriginY = -2 * Min * HalfHeight;
		const int32 CanvasWidth = OriginX + (Max - Min) * HalfWidth + FLBABrickSprite::Width;
		const int32 CanvasHeight = OriginY + 2 * Max * HalfHeight + FLBABrickSprite::Height;
		TArray<uint8> Canvas;
		Canvas.Init(0, CanvasWidth * CanvasHeight);
		TMap<int32, FLBABrickSprite> Sprites;
		for (int32 Sum = 2 * Min; Sum < 2 * Max; ++Sum)
		{
			for (int32 X = Min; X < Max; ++X)
			{
				const int32 Z = Sum - X;
				if (Z < Min || Z >= Max)
				{
					continue;
				}
				const int32 LX = ((X % TileCells) + TileCells) % TileCells;
				const int32 LZ = ((Z % TileCells) + TileCells) % TileCells;
				const int32 Brick = FirstBrick + (LZ % Deep) * RowCells + LX % RowCells;
				FLBABrickSprite* Sprite = Sprites.Find(Brick);
				if (!Sprite)
				{
					Sprite = &Sprites.Add(Brick);
					if (!Data.LoadBrick(Brick, *Sprite))
					{
						return false;
					}
				}
				const int32 AtX = OriginX + (X - Z) * HalfWidth;
				const int32 AtY = OriginY + (X + Z) * HalfHeight;
				for (int32 Y = 0; Y < FLBABrickSprite::Height; ++Y)
				{
					for (int32 PX = 0; PX < FLBABrickSprite::Width; ++PX)
					{
						const int32 Pixel = Y * FLBABrickSprite::Width + PX;
						if (Sprite->Pixels.IsValidIndex(Pixel) && Sprite->Pixels[Pixel] != 0)
						{
							Canvas[(AtY + Y) * CanvasWidth + AtX + PX] = Sprite->Pixels[Pixel];
						}
					}
				}
			}
		}
		// The middle tile, back from iso to a square: U along X, V along Z.
		const TArray<FColor> Palette = Data.GetPalette();
		OutLuma.SetNumUninitialized(TileSize * TileSize);
		for (int32 V = 0; V < TileSize; ++V)
		{
			for (int32 U = 0; U < TileSize; ++U)
			{
				const double CellU = (U + 0.5) / TileSize * TileCells;
				const double CellV = (V + 0.5) / TileSize * TileCells;
				const int32 PX = FMath::FloorToInt(OriginX + HalfWidth + (CellU - CellV) * HalfWidth);
				const int32 PY = FMath::FloorToInt(OriginY + (CellU + CellV) * HalfHeight);
				OutLuma[V * TileSize + U] = Luma(Palette[Canvas[PY * CanvasWidth + PX]]);
			}
		}
		return true;
	}

	/**
	 * Brightness placed on the palette's ramp holding the color nearest to Target: the pattern's median on that color, the
	 * rest relative to it, each pixel an exact palette color.
	 */
	TArray<FColor> OnRamp(TArray<float> Bright, const TArray<FColor>& Palette, const FVector3f& Target)
	{
		int32 Nearest = 1;
		float NearestDistance = TNumericLimits<float>::Max();
		for (int32 Entry = 1; Entry < 256; ++Entry)
		{
			const FColor& Color = Palette[Entry];
			const float Distance = 0.3f * FMath::Square(Color.R - Target.X) + 0.59f * FMath::Square(Color.G - Target.Y) + 0.11f * FMath::Square(Color.B - Target.Z);
			if (Distance < NearestDistance)
			{
				NearestDistance = Distance;
				Nearest = Entry;
			}
		}
		const int32 Ramp = Nearest / 16 * 16;
		TArray<float> Sorted = Bright;
		Sorted.Sort();
		const float Median = FMath::Max(Sorted[Sorted.Num() / 2], 1.f);
		const float Wanted = Luma(Palette[Nearest]);
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Bright.Num());
		for (int32 Pixel = 0; Pixel < Bright.Num(); ++Pixel)
		{
			const float Goal = Wanted + (Bright[Pixel] - Median) * Wanted / Median;
			int32 Step = 0;
			float StepDistance = TNumericLimits<float>::Max();
			for (int32 Candidate = 0; Candidate < 16; ++Candidate)
			{
				const float Distance = FMath::Abs(Luma(Palette[Ramp + Candidate]) - Goal);
				if (Distance < StepDistance)
				{
					StepDistance = Distance;
					Step = Candidate;
				}
			}
			Pixels[Pixel] = Palette[Ramp + Step];
			Pixels[Pixel].A = 255;
		}
		return Pixels;
	}

	/** A tile's texels of a page, as palette indices, repeated Copies times across. */
	TArray<uint8> CropTile(const TArray<uint8>& Page, const FTileRect& Rect, int32 Copies = 1)
	{
		TArray<uint8> Indices;
		Indices.SetNumUninitialized(FMath::Square(Rect.Size * Copies));
		for (int32 Y = 0; Y < Rect.Size * Copies; ++Y)
		{
			for (int32 X = 0; X < Rect.Size * Copies; ++X)
			{
				Indices[Y * Rect.Size * Copies + X] = Page[(Rect.Y + Y % Rect.Size) * PageSize + Rect.X + X % Rect.Size];
			}
		}
		return Indices;
	}

	TArray<FColor> ToColors(const TArray<uint8>& Indices, const TArray<FColor>& Palette)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Indices.Num());
		for (int32 Pixel = 0; Pixel < Indices.Num(); ++Pixel)
		{
			Pixels[Pixel] = Palette[Indices[Pixel]];
			Pixels[Pixel].A = 255;
		}
		return Pixels;
	}

	TArray<float> ToLuma(const TArray<uint8>& Indices, const TArray<FColor>& Palette)
	{
		TArray<float> Bright;
		Bright.SetNumUninitialized(Indices.Num());
		for (int32 Pixel = 0; Pixel < Indices.Num(); ++Pixel)
		{
			Bright[Pixel] = Luma(Palette[Indices[Pixel]]);
		}
		return Bright;
	}
}

UMaterialInterface* LBAPlanetGround::Build(FLBAImportContext& Context, const LBA1::FData* LBA1Data, const FString& Folder)
{
	using namespace LBAPlanetGroundPrivate;

	const FLBAHqrFile* Citabau = Context.GetHqr(TEXT("CITABAU.ILE"));
	const FLBAHqrFile* Desert = Context.GetHqr(TEXT("DESERT.ILE"));
	const FLBAXpl* HomeXpl = Context.GetXpl(FLBAXpl::GetEntryForIsland(TEXT("CITABAU")));
	const FLBAXpl* DesertXpl = Context.GetXpl(FLBAXpl::GetEntryForIsland(TEXT("DESERT")));
	TArray<uint8> CitabauPage, DesertPage;
	if (!Citabau || !Desert || !HomeXpl || !DesertXpl || !Citabau->GetEntry(PageEntry, CitabauPage) || !Desert->GetEntry(PageEntry, DesertPage)
		|| CitabauPage.Num() < PageSize * PageSize || DesertPage.Num() < PageSize * PageSize)
	{
		return nullptr;
	}
	const TArray<FColor>& Home = HomeXpl->Palette;

	auto Make = [&](const TCHAR* Kind, const TArray<FColor>& Pixels)
	{
		const int32 Size = FMath::RoundToInt(FMath::Sqrt(double(Pixels.Num())));
		return Context.CreateTexture(Folder, FString::Printf(TEXT("T_LBA_StubGround_%s"), Kind), Size, Size, Pixels, true);
	};
	// LBA1's earth when it's there (Citabau's bare ground otherwise, four copies: LBA1's tile is the larger), on LBA2's ramp.
	TArray<float> EarthBright;
	if (!LBA1Data || !ComposeBricks(*LBA1Data, EarthBricks, RowsDeep, EarthBright))
	{
		EarthBright = ToLuma(CropTile(CitabauPage, CitabauEarth, 4), Home);
	}
	UTexture2D* Earth = Make(TEXT("Earth"), OnRamp(MoveTemp(EarthBright), Home, EarthColor));
	// LBA1's dark ground (Citabau's rock on its ramp when there's no LBA1: no town uses it then).
	TArray<float> DarkBright;
	if (!LBA1Data || !ComposeBricks(*LBA1Data, DarkBricks, DarkRowsDeep, DarkBright))
	{
		DarkBright = ToLuma(CropTile(CitabauPage, CitabauRock), Home);
	}
	UTexture2D* Dark = Make(TEXT("Dark"), OnRamp(MoveTemp(DarkBright), Home, DarkColor));
	UTexture2D* Grass = Make(TEXT("Grass"), ToColors(CropTile(CitabauPage, CitabauGrass), Home));
	UTexture2D* Rock = Make(TEXT("Rock"), ToColors(CropTile(CitabauPage, CitabauRock), Home));
	UTexture2D* Pale = Make(TEXT("Pale"), ToColors(CropTile(CitabauPage, CitabauPale), Home));
	UTexture2D* Sand = Make(TEXT("Sand"), OnRamp(ToLuma(CropTile(DesertPage, DesertSand), DesertXpl->Palette), Home, SandColor));

	UMaterialInstanceConstant* Instance = Context.CreateOrReuseAsset<UMaterialInstanceConstant>(Folder, TEXT("MI_LBA_StubGround"));
	if (!Instance)
	{
		return nullptr;
	}
	UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Context.GetMaterials().GetMaster(ELBAMaterial::StubGround));
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Grass"), Grass);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Earth"), Earth);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Rock"), Rock);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Sand"), Sand);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Pale"), Pale);
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Dark"), Dark);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Context.MarkForSave(Instance);
	return Instance;
}

TArray<FColor> LBAPlanetGround::MakeTownPalette(const TArray<FColor>& LBA1Palette, const TArray<FColor>& Home)
{
	using namespace LBAPlanetGroundPrivate;
	constexpr int32 Steps = 16;
	auto Weighted = [](const FColor& A, const FColor& B)
	{
		return 0.3f * FMath::Square(float(A.R) - B.R) + 0.59f * FMath::Square(float(A.G) - B.G) + 0.11f * FMath::Square(float(A.B) - B.B);
	};
	auto StepAsBright = [&Home](int32 Ramp, float Goal)
	{
		int32 Best = 0;
		float BestDistance = TNumericLimits<float>::Max();
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const float Distance = FMath::Abs(Luma(Home[Ramp * Steps + Step]) - Goal);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Step;
			}
		}
		return Ramp * Steps + Best;
	};
	TArray<FColor> Out = LBA1Palette;
	// The home palette's first ramp and last are the engine's own colors, not art's.
	constexpr int32 FirstRamp = 1;
	constexpr int32 LastRamp = 14;
	for (int32 Ramp = 0; Ramp < 16; ++Ramp)
	{
		// LBA1's first two ramps aren't ramps (its fire and flash colors): each color to the home palette's nearest.
		if (Ramp < 2)
		{
			for (int32 Entry = FMath::Max(Ramp * Steps, 1); Entry < (Ramp + 1) * Steps; ++Entry)
			{
				int32 Best = FirstRamp * Steps;
				for (int32 Other = FirstRamp * Steps; Other < (LastRamp + 1) * Steps; ++Other)
				{
					Best = Weighted(Home[Other], LBA1Palette[Entry]) < Weighted(Home[Best], LBA1Palette[Entry]) ? Other : Best;
				}
				Out[Entry] = Home[Best];
			}
			continue;
		}
		// The ramp whose colors, as bright, are nearest to these.
		int32 Target = Ramp == PlantRamp ? HomePlantRamp : Ramp == EarthRamp ? HomeEarthRamp : INDEX_NONE;
		if (Target == INDEX_NONE)
		{
			float BestCost = TNumericLimits<float>::Max();
			for (int32 Candidate = FirstRamp; Candidate <= LastRamp; ++Candidate)
			{
				float Cost = 0.f;
				for (int32 Step = 0; Step < Steps; ++Step)
				{
					const FColor& Color = LBA1Palette[Ramp * Steps + Step];
					Cost += Weighted(Home[StepAsBright(Candidate, Luma(Color))], Color);
				}
				if (Cost < BestCost)
				{
					BestCost = Cost;
					Target = Candidate;
				}
			}
		}
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			float Goal = Luma(LBA1Palette[Ramp * Steps + Step]);
			if (Ramp == PlantRamp)
			{
				const float Middle = Luma(Home[HomePlantRamp * Steps + HomePlantStep]);
				Goal = Middle + (Goal - PlantMiddle) * PlantContrast * Middle / PlantMiddle;
			}
			else if (Ramp == EarthRamp)
			{
				const float Middle = Luma(Home[HomeEarthRamp * Steps + HomeEarthStep]);
				Goal = Middle + (Goal - EarthMiddle) * Middle / EarthMiddle;
			}
			Out[Ramp * Steps + Step] = Home[StepAsBright(Target, Goal)];
		}
	}
	for (FColor& Color : Out)
	{
		Color.A = 255;
	}
	return Out;
}

void LBAPlanetGround::MovePlantShadows(FLBABrickSprite& Sprite)
{
	using namespace LBAPlanetGroundPrivate;
	const int32 Width = FLBABrickSprite::Width;
	const int32 Height = FLBABrickSprite::Height;
	for (int32 Pass = 0; Pass < PlantShadowReach; ++Pass)
	{
		TArray<int32> Moves;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				if (Sprite.Pixels[Y * Width + X] / 16 != PlantShadowRamp)
				{
					continue;
				}
				bool bByPlant = false;
				for (int32 DY = FMath::Max(Y - 1, 0); DY <= FMath::Min(Y + 1, Height - 1); ++DY)
				{
					for (int32 DX = FMath::Max(X - 1, 0); DX <= FMath::Min(X + 1, Width - 1); ++DX)
					{
						const uint8 Other = Sprite.Pixels[DY * Width + DX];
						bByPlant = bByPlant || (Other != 0 && Other / 16 == PlantRamp);
					}
				}
				if (bByPlant)
				{
					Moves.Add(Y * Width + X);
				}
			}
		}
		for (const int32 Pixel : Moves)
		{
			Sprite.Pixels[Pixel] = uint8(PlantRamp * 16 + Sprite.Pixels[Pixel] % 16);
		}
	}
}
