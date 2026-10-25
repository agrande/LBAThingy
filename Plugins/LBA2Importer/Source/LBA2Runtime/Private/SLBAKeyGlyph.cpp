#include "SLBAKeyGlyph.h"

#include "Algo/AllOf.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "LBAKeyGlyph"

namespace
{
	const FLinearColor KeyFace(FColor(236, 236, 230));
	const FLinearColor KeyShadow(FColor(120, 120, 116));
	const FLinearColor KeyEdge(FColor(70, 70, 68));
	const FLinearColor KeyLabel(FColor(24, 24, 26));
	/** The mouse button pressed. */
	const FLinearColor MouseButton(FColor(255, 150, 30));
	const FLinearColor PadBody(FColor(34, 34, 38));
	const FLinearColor PadEdge(FColor(150, 150, 156));
	const FLinearColor PadLabel(FColor(240, 240, 240));
	const FLinearColor StickRing(FColor(92, 92, 98));
	const FLinearColor SeparatorColor(FColor(150, 150, 150));
	// Xbox face buttons.
	const FLinearColor ButtonA(FColor(110, 200, 60));
	const FLinearColor ButtonB(FColor(235, 70, 55));
	const FLinearColor ButtonX(FColor(60, 140, 235));
	const FLinearColor ButtonY(FColor(250, 200, 40));

	/** The same radius at all four corners (FVector4's one-value constructor only sets the first). */
	FVector4 Corners(float Radius)
	{
		return FVector4(Radius, Radius, Radius, Radius);
	}

	/** Slate fonts are sized in points at 96 DPI. */
	FSlateFontInfo GlyphFont(float PixelHeight)
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", FMath::Max(1, FMath::RoundToInt(PixelHeight * 0.75f)));
	}

	TSharedRef<SWidget> GlyphText(const FText& Text, float PixelHeight, const FLinearColor& Color)
	{
		return SNew(STextBlock).Text(Text).Font(GlyphFont(PixelHeight)).ColorAndOpacity(Color).Justification(ETextJustify::Center);
	}
}

const FSlateBrush* SLBAKeyGlyph::AddBrush(const FLinearColor& Fill, const FVector4& Radii, const FLinearColor& Outline, float OutlineWidth)
{
	return Brushes.Add_GetRef(MakeUnique<FSlateRoundedBoxBrush>(Fill, Radii, Outline, OutlineWidth)).Get();
}

void SLBAKeyGlyph::Construct(const FArguments& InArgs)
{
	const FKey& Key = InArgs._Key;
	const float H = InArgs._Height;
	TSharedPtr<SWidget> Glyph;
	if (Key == EKeys::Gamepad_FaceButton_Bottom)
	{
		Glyph = MakeFaceButton(LOCTEXT("A", "A"), ButtonA, H);
	}
	else if (Key == EKeys::Gamepad_FaceButton_Right)
	{
		Glyph = MakeFaceButton(LOCTEXT("B", "B"), ButtonB, H);
	}
	else if (Key == EKeys::Gamepad_FaceButton_Left)
	{
		Glyph = MakeFaceButton(LOCTEXT("X", "X"), ButtonX, H);
	}
	else if (Key == EKeys::Gamepad_FaceButton_Top)
	{
		Glyph = MakeFaceButton(LOCTEXT("Y", "Y"), ButtonY, H);
	}
	else if (Key == EKeys::Gamepad_LeftShoulder || Key == EKeys::Gamepad_RightShoulder)
	{
		Glyph = MakeShoulder(Key == EKeys::Gamepad_LeftShoulder ? LOCTEXT("LB", "LB") : LOCTEXT("RB", "RB"), false, H);
	}
	else if (Key == EKeys::Gamepad_LeftTrigger || Key == EKeys::Gamepad_LeftTriggerAxis)
	{
		Glyph = MakeShoulder(LOCTEXT("LT", "LT"), true, H);
	}
	else if (Key == EKeys::Gamepad_RightTrigger || Key == EKeys::Gamepad_RightTriggerAxis)
	{
		Glyph = MakeShoulder(LOCTEXT("RT", "RT"), true, H);
	}
	else if (Key == EKeys::Gamepad_Left2D || Key == EKeys::Gamepad_LeftX || Key == EKeys::Gamepad_LeftY || Key == EKeys::Gamepad_LeftThumbstick)
	{
		Glyph = MakeStick(LOCTEXT("L", "L"), H);
	}
	else if (Key == EKeys::Gamepad_Right2D || Key == EKeys::Gamepad_RightX || Key == EKeys::Gamepad_RightY || Key == EKeys::Gamepad_RightThumbstick)
	{
		Glyph = MakeStick(LOCTEXT("R", "R"), H);
	}
	else if (Key == EKeys::Gamepad_Special_Right || Key == EKeys::Gamepad_Special_Left)
	{
		Glyph = MakeSpecial(Key == EKeys::Gamepad_Special_Right, H);
	}
	else if (Key.IsMouseButton() || Key == EKeys::Mouse2D || Key == EKeys::MouseX || Key == EKeys::MouseY || Key == EKeys::MouseWheelAxis)
	{
		Glyph = MakeMouse(Key, H);
	}
	else if (Key.IsGamepadKey())
	{
		// The d-pad and the rest: a dark cap with the engine's name.
		Glyph = SNew(SBox).HeightOverride(H).MinDesiredWidth(H)
			[
				SNew(SBorder).BorderImage(AddBrush(PadBody, Corners(0.2f * H), PadEdge, 0.05f * H)).Padding(FMargin(0.2f * H, 0.f)).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					GlyphText(Key.GetDisplayName(false), 0.36f * H, PadLabel)
				]
			];
	}
	else
	{
		Glyph = MakeKeycap(GetKeyLabel(Key), H);
	}
	ChildSlot[Glyph.ToSharedRef()];
}

TSharedRef<SWidget> SLBAKeyGlyph::MakeKeycap(const FText& Label, float H)
{
	const bool bLetter = Label.ToString().Len() <= 1;
	const float Radius = 0.18f * H;
	// The space bar looks like one.
	const float MinWidth = Label.EqualTo(GetKeyLabel(EKeys::SpaceBar)) ? 2.6f * H : H;
	return SNew(SBox).HeightOverride(H).MinDesiredWidth(MinWidth)
		[
			SNew(SBorder).BorderImage(AddBrush(KeyShadow, Corners(Radius), KeyEdge, 0.04f * H)).Padding(FMargin(0.f, 0.f, 0.f, 0.12f * H))
			[
				SNew(SBorder).BorderImage(AddBrush(KeyFace, Corners(Radius), KeyEdge, 0.04f * H)).Padding(FMargin(0.22f * H, 0.f)).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					GlyphText(Label, (bLetter ? 0.5f : 0.36f) * H, KeyLabel)
				]
			]
		];
}

TSharedRef<SWidget> SLBAKeyGlyph::MakeMouse(const FKey& Key, float H)
{
	const float W = 0.68f * H;
	const float Line = 0.05f * H;
	const float Top = 0.44f * H;
	const float Round = 0.5f * W;
	auto Button = [&](bool bPressed, const FVector4& Radii) -> TSharedRef<SWidget>
	{
		return SNew(SBorder).BorderImage(AddBrush(bPressed ? MouseButton : FLinearColor::Transparent, Radii)).Padding(0.f);
	};
	const bool bLeft = Key == EKeys::LeftMouseButton;
	const bool bRight = Key == EKeys::RightMouseButton;
	const bool bMiddle = Key == EKeys::MiddleMouseButton || Key == EKeys::MouseWheelAxis;
	return SNew(SBox).WidthOverride(W).HeightOverride(H)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SBorder).BorderImage(AddBrush(KeyFace, Corners(Round), KeyEdge, Line))
			]
			// The buttons: the pressed one lit, a line between them and the body.
			+ SOverlay::Slot().VAlign(VAlign_Top).Padding(Line)
			[
				SNew(SBox).HeightOverride(Top - Line)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f)[Button(bLeft, FVector4(Round - Line, 0.f, 0.f, 0.f))]
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(Line)[SNew(SBorder).BorderImage(AddBrush(KeyEdge, Corners(0.f)))]]
					+ SHorizontalBox::Slot().FillWidth(1.f)[Button(bRight, FVector4(0.f, Round - Line, 0.f, 0.f))]
				]
			]
			+ SOverlay::Slot().VAlign(VAlign_Top).Padding(FMargin(0.f, Top, 0.f, 0.f))
			[
				SNew(SBox).HeightOverride(Line)[SNew(SBorder).BorderImage(AddBrush(KeyEdge, Corners(0.f)))]
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(FMargin(0.f, 0.12f * H, 0.f, 0.f))
			[
				SNew(SBox).WidthOverride(0.16f * H).HeightOverride(0.22f * H)
				[
					SNew(SBorder).BorderImage(AddBrush(bMiddle ? MouseButton : KeyFace, Corners(0.08f * H), KeyEdge, Line))
				]
			]
		];
}

TSharedRef<SWidget> SLBAKeyGlyph::MakeFaceButton(const FText& Letter, const FLinearColor& Color, float H)
{
	return SNew(SBox).WidthOverride(H).HeightOverride(H)
		[
			SNew(SBorder).BorderImage(AddBrush(PadBody, Corners(0.5f * H), Color, 0.08f * H)).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.f)
			[
				GlyphText(Letter, 0.56f * H, Color)
			]
		];
}

TSharedRef<SWidget> SLBAKeyGlyph::MakeShoulder(const FText& Label, bool bTrigger, float H)
{
	// Bumpers wide and flat, triggers taller and rounded on top.
	const float Width = bTrigger ? 1.1f * H : 1.5f * H;
	const float Height = bTrigger ? H : 0.72f * H;
	const FVector4 Radii = bTrigger ? FVector4(0.5f * H, 0.5f * H, 0.14f * H, 0.14f * H) : FVector4(0.36f * H, 0.36f * H, 0.12f * H, 0.12f * H);
	return SNew(SBox).HeightOverride(H).VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(Width).HeightOverride(Height)
			[
				SNew(SBorder).BorderImage(AddBrush(PadBody, Radii, PadEdge, 0.05f * H)).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.f)
				[
					GlyphText(Label, 0.38f * H, PadLabel)
				]
			]
		];
}

TSharedRef<SWidget> SLBAKeyGlyph::MakeStick(const FText& Letter, float H)
{
	const float Cap = 0.68f * H;
	return SNew(SBox).WidthOverride(H).HeightOverride(H)
		[
			SNew(SBorder).BorderImage(AddBrush(StickRing, Corners(0.5f * H), PadEdge, 0.04f * H)).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.f)
			[
				SNew(SBox).WidthOverride(Cap).HeightOverride(Cap)
				[
					SNew(SBorder).BorderImage(AddBrush(PadBody, Corners(0.5f * Cap))).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.f)
					[
						GlyphText(Letter, 0.42f * H, PadLabel)
					]
				]
			]
		];
}

TSharedRef<SWidget> SLBAKeyGlyph::MakeSpecial(bool bMenu, float H)
{
	// Menu: three lines. View: two overlapping windows.
	const float Size = 0.8f * H;
	const float Line = 0.06f * H;
	TSharedPtr<SWidget> Mark;
	if (bMenu)
	{
		TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Lines->AddSlot().AutoHeight().Padding(FMargin(0.f, Index == 0 ? 0.f : 0.07f * H, 0.f, 0.f))
				[
					SNew(SBox).WidthOverride(0.36f * H).HeightOverride(Line)[SNew(SBorder).BorderImage(AddBrush(PadLabel, Corners(0.5f * Line)))]
				];
		}
		Mark = Lines;
	}
	else
	{
		auto Window = [&](const FMargin& Offset) -> TSharedRef<SWidget>
		{
			return SNew(SBox).Padding(Offset)
				[
					SNew(SBox).WidthOverride(0.24f * H).HeightOverride(0.2f * H)
					[
						SNew(SBorder).BorderImage(AddBrush(PadBody, Corners(0.03f * H), PadLabel, Line))
					]
				];
		};
		Mark = SNew(SOverlay)
			+ SOverlay::Slot()[Window(FMargin(0.f, 0.f, 0.1f * H, 0.1f * H))]
			+ SOverlay::Slot()[Window(FMargin(0.1f * H, 0.1f * H, 0.f, 0.f))];
	}
	return SNew(SBox).HeightOverride(H).VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(Size).HeightOverride(Size)
			[
				SNew(SBorder).BorderImage(AddBrush(PadBody, Corners(0.5f * Size), PadEdge, 0.05f * H)).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(0.f)
				[
					Mark.ToSharedRef()
				]
			]
		];
}

FText SLBAKeyGlyph::GetKeyLabel(const FKey& Key)
{
	if (Key == EKeys::SpaceBar)
	{
		return LOCTEXT("Space", "Space");
	}
	if (Key == EKeys::LeftShift || Key == EKeys::RightShift)
	{
		return LOCTEXT("Shift", "Shift");
	}
	if (Key == EKeys::LeftControl || Key == EKeys::RightControl)
	{
		return LOCTEXT("Ctrl", "Ctrl");
	}
	if (Key == EKeys::LeftAlt || Key == EKeys::RightAlt)
	{
		return LOCTEXT("Alt", "Alt");
	}
	if (Key == EKeys::Escape)
	{
		return LOCTEXT("Esc", "Esc");
	}
	if (Key == EKeys::Enter)
	{
		return LOCTEXT("Enter", "Enter");
	}
	return Key.GetDisplayName(false);
}

TSharedRef<SWidget> SLBAKeyGlyph::MakeKeys(TConstArrayView<FKey> Keys, float Height)
{
	TArray<FKey> Rest(Keys.GetData(), Keys.Num());
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	auto AddSeparator = [&]()
	{
		if (Row->NumSlots() > 0)
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.12f * Height, 0.f))
				[
					GlyphText(LOCTEXT("Or", "/"), 0.5f * Height, SeparatorColor)
				];
		}
	};
	// W A S D as the keyboard shows them: W above the other three.
	const FKey Cluster[] = { EKeys::W, EKeys::A, EKeys::S, EKeys::D };
	const bool bCluster = Algo::AllOf(Cluster, [&Rest](const FKey& Key) { return Rest.Contains(Key); });
	if (bCluster)
	{
		const float Small = 0.54f * Height;
		const float Gap = 0.04f * Height;
		TSharedRef<SHorizontalBox> Bottom = SNew(SHorizontalBox);
		for (int32 Index = 1; Index < 4; ++Index)
		{
			Bottom->AddSlot().AutoWidth().Padding(FMargin(Index > 1 ? Gap : 0.f, 0.f, 0.f, 0.f))[SNew(SLBAKeyGlyph).Key(Cluster[Index]).Height(Small)];
			Rest.Remove(Cluster[Index]);
		}
		Rest.Remove(Cluster[0]);
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 0.f, 0.f, Gap))[SNew(SLBAKeyGlyph).Key(Cluster[0]).Height(Small)]
				+ SVerticalBox::Slot().AutoHeight()[Bottom]
			];
	}
	for (const FKey& Key : Rest)
	{
		AddSeparator();
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SLBAKeyGlyph).Key(Key).Height(Height)];
	}
	return Row;
}

#undef LOCTEXT_NAMESPACE
