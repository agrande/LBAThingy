#include "Import/LBAPlanetKit.h"

#include "Formats/LBA1.h"
#include "Formats/LBAIso.h"
#include "Import/LBABodyConverter.h"
#include "Import/LBAMeshBuilder.h"
#include "Interfaces/IPluginManager.h"
#include "LBA2Runtime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace LBAPlanetKitPrivate
{
	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			FJsonSerializer::Deserialize(Reader, Object);
		}
		return Object;
	}

	/** The remake's layout orientations (0..3) as turns, as its interiors are placed (FLBARemakeAssets::ProcessGrid). */
	constexpr double RemakeYaw[4] = { -90.0, -180.0, 90.0, 0.0 };
}

FString LBAPlanetKit::FTable::KeyOf(const FLBALayout& Layout)
{
	FString Key = FString::Printf(TEXT("%dx%dx%d:"), Layout.SizeX, Layout.SizeY, Layout.SizeZ);
	for (int32 Index = 0; Index < Layout.Blocks.Num(); ++Index)
	{
		if (Index)
		{
			Key += TEXT(",");
		}
		Key.AppendInt(Layout.Blocks[Index].Brick);
	}
	return Key;
}

int32 LBAPlanetKit::FTable::Find(const FLBALayout& Layout) const
{
	if (ByKey.IsEmpty())
	{
		return INDEX_NONE;
	}
	const int32* Found = ByKey.Find(KeyOf(Layout));
	return Found ? *Found : INDEX_NONE;
}

void LBAPlanetKit::FTable::Load(const LBA1::FData& Data, const FString& RemakeRoot)
{
	using namespace LBAPlanetKitPrivate;
	Models.Reset();
	ByKey.Reset();
	TMap<int32, FLBALibrary> Libraries;
	auto LayoutOf = [&](int32 Scene, int32 Layout) -> const FLBALayout*
	{
		FLBALibrary* Library = Libraries.Find(Scene);
		if (!Library)
		{
			Library = &Libraries.Add(Scene);
			if (!Data.LoadLibrary(Scene, *Library))
			{
				Library->Layouts.Reset();
			}
		}
		return Library->Layouts.IsValidIndex(Layout) ? &Library->Layouts[Layout] : nullptr;
	};
	auto Add = [&](const FLBALayout& Layout, FModel&& Model, bool bOverride)
	{
		const FString Key = KeyOf(Layout);
		if (ByKey.Contains(Key) && !bOverride)
		{
			return;
		}
		ByKey.Add(Key, Models.Add(MoveTemp(Model)));
	};

	// The remake's LBA1 replacements first: the table overrides them.
	const TSharedPtr<FJsonObject> Remake = RemakeRoot.IsEmpty() ? nullptr : LoadJson(FPaths::Combine(RemakeRoot, TEXT("www/metadata/lba1/layouts.json")));
	if (Remake.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Scene : Remake->Values)
		{
			const TSharedPtr<FJsonObject>* Layouts = nullptr;
			if (!Scene.Value->TryGetObject(Layouts))
			{
				continue;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Layouts)->Values)
			{
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				bool bReplace = false;
				FString File;
				double Orientation = 0.0;
				// "layout:size:blocks" variants aren't used by the remake's LBA1 scenes.
				if (Entry.Key.Contains(TEXT(":")) || !Entry.Value->TryGetObject(Fields) || !(*Fields)->TryGetBoolField(TEXT("replace"), bReplace) || !bReplace
					|| !(*Fields)->TryGetStringField(TEXT("file"), File))
				{
					continue;
				}
				(*Fields)->TryGetNumberField(TEXT("orientation"), Orientation);
				if (const FLBALayout* Layout = LayoutOf(FCString::Atoi(*Scene.Key), FCString::Atoi(*Entry.Key)))
				{
					FModel Model;
					Model.Name = FPaths::GetBaseFilename(File);
					Model.RemakeFile = TEXT("lba1/layouts") / File;
					Model.Yaw = RemakeYaw[int32(Orientation) & 3];
					Add(*Layout, MoveTemp(Model), false);
				}
			}
		}
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LBA2Importer"));
	const FString Path = Plugin ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Models/lba1_kit.json")) : FString();
	const TSharedPtr<FJsonObject> Table = LoadJson(Path);
	const TArray<TSharedPtr<FJsonValue>>* Pieces = nullptr;
	if (!Table.IsValid() || !Table->TryGetArrayField(TEXT("pieces"), Pieces))
	{
		UE_LOG(LogLBA2, Warning, TEXT("LBA1 kit: %s can't be read, LBA1's towns stay bricks"), *Path);
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Pieces)
	{
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		FString Example, Lba2, Fit, Turn;
		if (!Value->TryGetObject(Fields) || !(*Fields)->TryGetStringField(TEXT("example"), Example))
		{
			continue;
		}
		FModel Model;
		(*Fields)->TryGetStringField(TEXT("name"), Model.Name);
		FString Scene, Layout, Body;
		if ((*Fields)->TryGetStringField(TEXT("lba2"), Lba2) && Lba2.Split(TEXT(":"), &Model.Island, &Body))
		{
			Model.Body = FCString::Atoi(*Body);
		}
		(*Fields)->TryGetStringField(TEXT("remake"), Model.RemakeFile);
		(*Fields)->TryGetStringField(TEXT("primitive"), Model.Primitive);
		(*Fields)->TryGetNumberField(TEXT("yaw"), Model.Yaw);
		(*Fields)->TryGetNumberField(TEXT("scale"), Model.Scale);
		(*Fields)->TryGetNumberField(TEXT("every"), Model.Every);
		Model.bFitHeight = (*Fields)->TryGetStringField(TEXT("fit"), Fit) && Fit == TEXT("height");
		Model.bRandomTurn = (*Fields)->TryGetStringField(TEXT("turn"), Turn) && Turn == TEXT("random");
		const FLBALayout* Found = Example.Split(TEXT(":"), &Scene, &Layout) ? LayoutOf(FCString::Atoi(*Scene), FCString::Atoi(*Layout)) : nullptr;
		if (!Found || (Model.Body == INDEX_NONE && Model.RemakeFile.IsEmpty() && !IsPrimitive(Model.Primitive)))
		{
			UE_LOG(LogLBA2, Warning, TEXT("LBA1 kit: %s (example %s) has no piece or no model"), *Model.Name, *Example);
			continue;
		}
		Model.Size = FIntVector(Found->SizeX, Found->SizeY, Found->SizeZ);
		for (const auto& Block : Found->Blocks)
		{
			Model.Bricks.Add(Block.Brick);
		}
		Add(*Found, MoveTemp(Model), true);
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA1 kit: %d pieces drawn as models"), ByKey.Num());
}

namespace LBAPlanetKitPrivate
{
	/** A generated model's parts: flat-shaded bars, rounded boxes and rings, all in one vertex-colored section. */
	struct FShapes
	{
		FLBAMeshBuilder& Builder;
		int32 Section;
		/** Built running along x; a piece running along z is mirrored across x = y, which keeps faces facing out. */
		bool bSwap = false;

		FVector3f Place(const FVector3f& P) const { return bSwap ? FVector3f(P.Y, P.X, P.Z) : P; }

		void Triangle(const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& NA, const FVector3f& NB, const FVector3f& NC,
			const FVector4f& Color) const
		{
			FLBAVertex V[3];
			const FVector3f P[3] = { A, B, C };
			const FVector3f N[3] = { NA, NB, NC };
			for (int32 K = 0; K < 3; ++K)
			{
				V[K].Position = Place(P[K]);
				V[K].Normal = Place(N[K]);
				V[K].Color = Color;
			}
			Builder.AddTriangleFacing(Section, V[0], V[1], V[2], V[0].Normal + V[1].Normal + V[2].Normal);
		}

		/** A flat quad (corners in order around it) facing Normal. */
		void Quad(const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D, const FVector3f& Normal, const FVector4f& Color) const
		{
			Triangle(A, B, C, Normal, Normal, Normal, Color);
			Triangle(A, C, D, Normal, Normal, Normal, Color);
		}

		/** A bar from A to B, Thick across and Wide (default Thick) toward Side, any direction not along it. */
		void Bar(const FVector3f& A, const FVector3f& B, float Thick, const FVector3f& Side, const FVector4f& Color, float Wide = 0.f) const
		{
			const FVector3f D = (B - A).GetSafeNormal();
			const FVector3f U = (Side - D * FVector3f::DotProduct(Side, D)).GetSafeNormal() * 0.5f * (Wide > 0.f ? Wide : Thick);
			const FVector3f V = FVector3f::CrossProduct(D, U).GetSafeNormal() * 0.5f * Thick;
			const FVector3f Corners[4] = { U + V, -U + V, -U - V, U - V };
			for (int32 K = 0; K < 4; ++K)
			{
				const FVector3f& C0 = Corners[K];
				const FVector3f& C1 = Corners[(K + 1) % 4];
				Quad(A + C0, A + C1, B + C1, B + C0, (C0 + C1).GetSafeNormal(), Color);
			}
			Quad(A + Corners[0], A + Corners[1], A + Corners[2], A + Corners[3], -D, Color);
			Quad(B + Corners[0], B + Corners[1], B + Corners[2], B + Corners[3], D, Color);
		}

		/**
		 * A box of half size Half around Center with its edges rounded by Radius: a cube's faces cut at Radius from each
		 * edge, each point pushed out from the inner box (Half - Radius) by Radius, normals smooth.
		 */
		void RoundedBox(const FVector3f& Center, const FVector3f& Half, float Radius, const FVector4f& Color) const
		{
			const FVector3f Inner = (Half - FVector3f(Radius)).ComponentMax(FVector3f::ZeroVector);
			auto Point = [&](const FVector3f& OnCube, FVector3f& Normal)
			{
				const FVector3f Core = OnCube.BoundToBox(-Inner, Inner);
				Normal = (OnCube - Core).GetSafeNormal();
				return Center + Core + Normal * Radius;
			};
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const int32 AxisU = (Axis + 1) % 3, AxisV = (Axis + 2) % 3;
				const float SU[4] = { -Half[AxisU], -Inner[AxisU], Inner[AxisU], Half[AxisU] };
				const float SV[4] = { -Half[AxisV], -Inner[AxisV], Inner[AxisV], Half[AxisV] };
				for (const float Sign : { -1.f, 1.f })
				{
					for (int32 I = 0; I < 3; ++I)
					{
						for (int32 J = 0; J < 3; ++J)
						{
							FVector3f P[4], N[4];
							const int32 DU[4] = { 0, 1, 1, 0 }, DV[4] = { 0, 0, 1, 1 };
							for (int32 K = 0; K < 4; ++K)
							{
								FVector3f OnCube;
								OnCube[Axis] = Sign * Half[Axis];
								OnCube[AxisU] = SU[I + DU[K]];
								OnCube[AxisV] = SV[J + DV[K]];
								P[K] = Point(OnCube, N[K]);
							}
							if ((P[0] - P[2]).IsNearlyZero() || (P[1] - P[3]).IsNearlyZero())
							{
								continue;
							}
							Triangle(P[0], P[1], P[2], N[0], N[1], N[2], Color);
							Triangle(P[0], P[2], P[3], N[0], N[2], N[3], Color);
						}
					}
				}
			}
		}

		/** A ring around Center in the x-z plane (its axis along y): Major to the tube's middle, the tube Minor in radius. */
		void Ring(const FVector3f& Center, float Major, float Minor, int32 Around, int32 Tube, TFunctionRef<FVector4f(float Turn)> ColorAt) const
		{
			auto Point = [&](int32 I, int32 J, FVector3f& Normal)
			{
				const float A = 2.f * PI * I / Around, B = 2.f * PI * J / Tube;
				const FVector3f Out(FMath::Cos(A), 0.f, FMath::Sin(A));
				Normal = Out * FMath::Cos(B) + FVector3f(0.f, FMath::Sin(B), 0.f);
				return Center + Out * Major + Normal * Minor;
			};
			for (int32 I = 0; I < Around; ++I)
			{
				const FVector4f Color = ColorAt((I + 0.5f) / Around);
				for (int32 J = 0; J < Tube; ++J)
				{
					FVector3f P[4], N[4];
					P[0] = Point(I, J, N[0]);
					P[1] = Point(I + 1, J, N[1]);
					P[2] = Point(I + 1, J + 1, N[2]);
					P[3] = Point(I, J + 1, N[3]);
					Triangle(P[0], P[1], P[2], N[0], N[1], N[2], Color);
					Triangle(P[0], P[2], P[3], N[0], N[2], N[3], Color);
				}
			}
		}
	};

	/**
	 * A piece's colors from its bricks' LBA1 ramps: Main, the ramp drawing most of its lit pixels, and Accent, a clearly
	 * different second ramp (else Main), each at the step that 80 % of its pixels are at or below. Lit by the engine, a
	 * model shows darker than the bricks' predrawn art, so the bright end of the ramp is the one to match. (A sack at the
	 * median step rendered darker but no less green, (23,50,32): the hue is the palette shading's, as on lit grass.)
	 */
	void PieceColors(const LBAPlanetKit::FModel& Model, const LBA1::FData& Data, const TArray<FColor>& Palette, FColor& Main, FColor& Accent)
	{
		int32 Counts[256] = {};
		TSet<int32> Seen;
		for (const int32 Brick : Model.Bricks)
		{
			FLBABrickSprite Sprite;
			if (Brick == 0 || Seen.Contains(Brick) || Data.IsBlankBrick(Brick) || !Data.LoadBrick(Brick, Sprite))
			{
				continue;
			}
			Seen.Add(Brick);
			for (const uint8 Index : Sprite.Pixels)
			{
				++Counts[Index];
			}
		}
		Counts[0] = 0;
		int32 Ramps[16] = {};
		// Pixels weighted by their step: the ramp drawing the lit surfaces wins over the one drawing their shadows (a sack
		// seen along its length shows more shadow than lit cloth).
		int64 Lit[16] = {};
		int32 Total = 0;
		for (int32 Index = 0; Index < 256; ++Index)
		{
			Ramps[Index >> 4] += Counts[Index];
			Lit[Index >> 4] += int64(Counts[Index]) * ((Index & 15) + 1);
			Total += Counts[Index];
		}
		int32 First = 0;
		for (int32 Ramp = 1; Ramp < 16; ++Ramp)
		{
			First = Lit[Ramp] > Lit[First] ? Ramp : First;
		}
		auto ColorOf = [&](int32 Ramp)
		{
			int32 Below = 0;
			for (int32 RampStep = 0; RampStep < 16; ++RampStep)
			{
				Below += Counts[Ramp * 16 + RampStep];
				if (Below >= 0.8 * Ramps[Ramp] && Palette.IsValidIndex(Ramp * 16 + RampStep))
				{
					return Palette[Ramp * 16 + RampStep];
				}
			}
			return FColor::White;
		};
		Main = ColorOf(First);
		// The accent: of the other ramps holding at least 8 % of the pixels, the one whose color is furthest from Main (the
		// buoy's teal bands, not the grey that shades its white), when it is clearly different.
		Accent = Main;
		int32 Furthest = 60 * 60;
		for (int32 Ramp = 0; Ramp < 16; ++Ramp)
		{
			if (Ramp == First || Ramps[Ramp] < 0.08 * Total)
			{
				continue;
			}
			const FColor Color = ColorOf(Ramp);
			const int32 Distance = FMath::Square(Color.R - Main.R) + FMath::Square(Color.G - Main.G) + FMath::Square(Color.B - Main.B);
			if (Distance > Furthest)
			{
				Furthest = Distance;
				Accent = Color;
			}
		}
	}
}

bool LBAPlanetKit::IsPrimitive(const FString& Kind)
{
	return Kind == TEXT("fence") || Kind == TEXT("railing") || Kind == TEXT("sack") || Kind == TEXT("buoy");
}

bool LBAPlanetKit::BuildPrimitive(const FModel& Model, const LBA1::FData& Data, const TArray<FColor>& Palette, double CellCm, double StepCm,
	FLBAMeshBuilder& Builder, int32 Section)
{
	using namespace LBAPlanetKitPrivate;
	if (!IsPrimitive(Model.Primitive))
	{
		return false;
	}
	FColor MainColor, AccentColor;
	PieceColors(Model, Data, Palette, MainColor, AccentColor);
	const FVector4f Main = LBABodyConverter::ToVertexColor(MainColor);
	const FVector4f Accent = LBABodyConverter::ToVertexColor(AccentColor);

	const FShapes Shapes{ Builder, Section, Model.Size.Z > Model.Size.X };
	const float KitCell = float(CellCm);
	// Along the piece (Long), across it (Wide) and up (High), in cm.
	const float Long = KitCell * FMath::Max(Model.Size.X, Model.Size.Z);
	const float Wide = KitCell * FMath::Min(Model.Size.X, Model.Size.Z);
	const float High = float(StepCm) * Model.Size.Y;
	const FVector3f AlongX(1.f, 0.f, 0.f);
	const FVector3f AcrossY(0.f, 1.f, 0.f);

	if (Model.Primitive == TEXT("fence"))
	{
		// Pickets about 5 a cell (LBA1's: 19 over 4 cells), on two rails behind them.
		const int32 Pickets = FMath::Max(2, FMath::RoundToInt(Long / (0.21f * KitCell)));
		const float Picket = 0.09f * KitCell, Depth = 0.05f * KitCell, Rail = 0.06f * KitCell;
		for (int32 K = 0; K < Pickets; ++K)
		{
			const float X = -0.5f * Long + Long * (K + 0.5f) / Pickets;
			Shapes.Bar(FVector3f(X, 0.f, 0.f), FVector3f(X, 0.f, 0.94f * High), Depth, AlongX, Main, Picket);
		}
		for (const float Height : { 0.28f, 0.72f })
		{
			const float Y = 0.5f * (Depth + Rail);
			Shapes.Bar(FVector3f(-0.5f * Long, Y, Height * High), FVector3f(0.5f * Long, Y, Height * High), Rail, AcrossY, Main);
		}
	}
	else if (Model.Primitive == TEXT("railing"))
	{
		// LBA1's harbour railings: three bars a cell, straight up for 60 % of the height, then bent over away from the
		// iso camera (to -y, as their sprites draw it), on two rails.
		const int32 Bars = FMath::Max(1, FMath::RoundToInt(3.f * Long / KitCell));
		const float Thick = 0.07f * KitCell, Rail = 0.075f * KitCell;
		const float Straight = 0.6f * High, Bend = 0.4f * High;
		constexpr int32 Arc = 4;
		constexpr float ArcEnd = 80.f;
		for (int32 K = 0; K < Bars; ++K)
		{
			const float X = -0.5f * Long + Long * (K + 0.5f) / Bars;
			FVector3f Last(X, 0.f, 0.f);
			FVector3f Next(X, 0.f, Straight);
			Shapes.Bar(Last, Next, Thick, AcrossY, Main);
			for (int32 ArcStep = 1; ArcStep <= Arc; ++ArcStep)
			{
				const float Angle = FMath::DegreesToRadians(ArcEnd * ArcStep / Arc);
				Last = Next;
				Next = FVector3f(X, -Bend * (1.f - FMath::Cos(Angle)), Straight + Bend * FMath::Sin(Angle));
				Shapes.Bar(Last, Next, Thick, AlongX, Main);
			}
		}
		for (const float Height : { 0.14f, 0.5f })
		{
			Shapes.Bar(FVector3f(-0.5f * Long, 0.f, Height * High), FVector3f(0.5f * Long, 0.f, Height * High), Rail, FVector3f::UpVector, Main);
		}
	}
	else if (Model.Primitive == TEXT("sack"))
	{
		// A filled sack lying along the piece: nearly its whole footprint, rounded almost to its full height.
		const FVector3f Half(0.47f * Long, 0.43f * Wide, 0.5f * High);
		Shapes.RoundedBox(FVector3f(0.f, 0.f, Half.Z), Half, 0.8f * FMath::Min(Half.Z, Half.Y), Main);
	}
	else if (Model.Primitive == TEXT("buoy"))
	{
		// An upright ring along the piece's long side, as high as the piece, with four bands on its diagonals.
		const float Outer = 0.48f * FMath::Min(Long, High);
		const float Minor = 0.22f * Outer;
		Shapes.Ring(FVector3f(0.f, 0.f, 0.5f * High), Outer - Minor, Minor, 32, 8, [&](float Turn)
		{
			const float Quarter = FMath::Fmod(Turn * 4.f, 1.f);
			return FMath::Abs(Quarter - 0.5f) < 0.09f ? Accent : Main;
		});
	}
	UE_LOG(LogLBA2, Display, TEXT("LBA1 kit: %s generated (%s, %d x %d x %d), colors %s and %s"), *Model.Name, *Model.Primitive, Model.Size.X,
		Model.Size.Y, Model.Size.Z, *MainColor.ToHex(), *AccentColor.ToHex());
	return !Builder.IsEmpty();
}
