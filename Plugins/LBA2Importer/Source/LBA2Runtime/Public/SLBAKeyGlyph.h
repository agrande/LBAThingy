#pragma once

#include "Brushes/SlateRoundedBoxBrush.h"
#include "InputCoreTypes.h"
#include "Widgets/SCompoundWidget.h"

/**
 * A key drawn as players know it, no texture needed: keyboard keycaps, the mouse with its pressed button, gamepad face
 * buttons in their colors (Xbox layout), bumpers, triggers, sticks, menu and view buttons. Height is the glyph's height
 * in Slate units; wide keys (Space, bumpers) grow sideways.
 */
class LBA2RUNTIME_API SLBAKeyGlyph : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLBAKeyGlyph) : _Height(48.f) {}
		SLATE_ARGUMENT(FKey, Key)
		SLATE_ARGUMENT(float, Height)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * Glyphs of alternative keys, separated by slashes. W, A, S and D together are drawn as one cluster.
	 */
	static TSharedRef<SWidget> MakeKeys(TConstArrayView<FKey> Keys, float Height);

	/** A keyboard key's short label ("Space", "Shift", "E"). */
	static FText GetKeyLabel(const FKey& Key);

private:
	TSharedRef<SWidget> MakeKeycap(const FText& Label, float Height);
	TSharedRef<SWidget> MakeMouse(const FKey& Key, float Height);
	TSharedRef<SWidget> MakeFaceButton(const FText& Letter, const FLinearColor& Color, float Height);
	TSharedRef<SWidget> MakeShoulder(const FText& Label, bool bTrigger, float Height);
	TSharedRef<SWidget> MakeStick(const FText& Letter, float Height);
	TSharedRef<SWidget> MakeSpecial(bool bMenu, float Height);

	/** Brushes live as long as the widget drawing them. */
	const FSlateBrush* AddBrush(const FLinearColor& Fill, const FVector4& Radii, const FLinearColor& Outline = FLinearColor::Transparent, float OutlineWidth = 0.f);
	TArray<TUniquePtr<FSlateRoundedBoxBrush>> Brushes;
};
