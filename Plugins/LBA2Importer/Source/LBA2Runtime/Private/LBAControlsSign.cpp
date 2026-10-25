#include "LBAControlsSign.h"

#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Materials/MaterialInterface.h"
#include "SLBAKeyGlyph.h"
#include "Styling/CoreStyle.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "LBAControlsSign"

namespace
{
	/** Board margin around the panel and board depth (cm). */
	constexpr float SignMargin = 28.f;
	constexpr double BoardDepth = 10.0;
	/** The board's face, back from the actor; the panel just in front of it. */
	constexpr double BoardCenterX = -8.0;
	constexpr double PanelX = BoardCenterX + BoardDepth / 2.0 + 1.0;

	const FLinearColor DeviceColor(FColor(97, 206, 206));
	const FLinearColor NotesColor(FColor(205, 205, 200));

	FSlateFontInfo SignFont(const char* Style, float PixelHeight)
	{
		// Slate fonts are sized in points at 96 DPI.
		return FCoreStyle::GetDefaultFontStyle(Style, FMath::Max(1, FMath::RoundToInt(PixelHeight * 0.75f)));
	}

	FText DeviceName(bool bGamepad)
	{
		return bGamepad ? LOCTEXT("Gamepad", "GAMEPAD") : LOCTEXT("KeyboardMouse", "KEYBOARD & MOUSE");
	}
}

ALBAControlsSign::ALBAControlsSign()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	auto MakeBlock = [this](const TCHAR* Name)
	{
		UStaticMeshComponent* Block = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Block->SetupAttachment(RootComponent);
		Block->SetStaticMesh(CubeMesh.Object);
		return Block;
	};
	Post = MakeBlock(TEXT("Post"));
	Board = MakeBlock(TEXT("Board"));

	Panel = CreateDefaultSubobject<UWidgetComponent>(TEXT("Panel"));
	Panel->SetupAttachment(RootComponent);
	Panel->SetWidgetSpace(EWidgetSpace::World);
	Panel->SetBlendMode(EWidgetBlendMode::Transparent);
	Panel->SetTwoSided(false);
	Panel->SetDrawAtDesiredSize(false);
	Panel->SetPivot(FVector2D(0.5, 0.5));
	Panel->SetBackgroundColor(FLinearColor::Transparent);
	// Drawn again only when the content changes.
	Panel->SetManuallyRedraw(true);
	Panel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Panel->SetCastShadow(false);
}

void ALBAControlsSign::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Refresh();
}

void ALBAControlsSign::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// The panel is transient: a loaded sign builds it here (the editor doesn't always rerun the construction).
	if (!bBuilt && !IsTemplate())
	{
		Refresh();
	}
}

void ALBAControlsSign::BeginPlay()
{
	Super::BeginPlay();
	LayoutChangedHandle = ULBAControlsSubsystem::OnLayoutChanged().AddWeakLambda(this, [this]()
	{
		if (ULBAControlsSubsystem::GetLayout(this) != ShownLayout)
		{
			Refresh();
		}
	});
	if (ULBAControlsSubsystem::GetLayout(this) != ShownLayout)
	{
		Refresh();
	}
}

void ALBAControlsSign::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ULBAControlsSubsystem::OnLayoutChanged().Remove(LayoutChangedHandle);
	Super::EndPlay(EndPlayReason);
}

void ALBAControlsSign::Refresh()
{
	Post->SetMaterial(0, BoardMaterial);
	Board->SetMaterial(0, BoardMaterial);
	if (PanelMaterial)
	{
		Panel->SetMaterial(0, PanelMaterial);
	}
	// Measuring text needs Slate (not in commandlets): the board keeps its size until then.
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	ShownLayout = ULBAControlsSubsystem::GetLayout(this);
	TSharedRef<SWidget> Content = BuildContent(ShownLayout);
	Content->SlatePrepass(1.f);
	const FVector2D Pixels = Content->GetDesiredSize();

	const double Width = Pixels.X * PixelSize + 2.0 * SignMargin;
	const double Height = Pixels.Y * PixelSize + 2.0 * SignMargin;
	const double Middle = PostHeight + Height / 2.0;
	Board->SetRelativeLocation(FVector(BoardCenterX, 0.0, Middle));
	Board->SetRelativeScale3D(FVector(BoardDepth, Width, Height) / 100.0);
	Post->SetVisibility(PostHeight > 0.f);
	Post->SetRelativeLocation(FVector(BoardCenterX, 0.0, PostHeight / 2.0));
	Post->SetRelativeScale3D(FVector(14.0, 14.0, FMath::Max(PostHeight, 1.f)) / 100.0);

	Panel->SetRelativeLocation(FVector(PanelX, 0.0, Middle));
	Panel->SetRelativeScale3D(FVector(1.0, PixelSize, PixelSize));
	Panel->SetDrawSize(FVector2D(FMath::CeilToDouble(Pixels.X), FMath::CeilToDouble(Pixels.Y)));
	Panel->SetSlateWidget(Content);
	Panel->RequestRedraw();
	bBuilt = true;
}

TSharedRef<SWidget> ALBAControlsSign::BuildContent(ELBAControlsLayout Layout) const
{
	// Sizes in panel pixels.
	const float Text = TextSize / PixelSize;
	const float Glyph = 1.5f * Text;
	const float Gap = 0.6f * Text;
	const bool bBoth = Layout == ELBAControlsLayout::Both;
	const bool bGamepad = Layout == ELBAControlsLayout::Gamepad;

	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	TSharedRef<SHorizontalBox> TitleRow = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Title).Font(SignFont("Bold", TitleSize / PixelSize)).ColorAndOpacity(TitleColor)
		];
	if (!bBoth && !Hints.IsEmpty())
	{
		// Which device the glyphs are for, for whoever watches without seeing the player's hands.
		TitleRow->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(2.f * Gap, 0.f, 0.f, 0.f))
			[
				SNew(STextBlock).Text(DeviceName(bGamepad)).Font(SignFont("Bold", 0.7f * Text)).ColorAndOpacity(DeviceColor)
			];
	}
	Box->AddSlot().AutoHeight()[TitleRow];

	// Keys then verb; both devices: gamepad keys, verb, keyboard and mouse keys.
	TSharedRef<SGridPanel> Grid = SNew(SGridPanel);
	int32 Row = 0;
	auto AddVerb = [&](int32 Column, const FText& Verb, EHorizontalAlignment Align)
	{
		Grid->AddSlot(Column, Row).VAlign(VAlign_Center).HAlign(Align).Padding(FMargin(Gap, 0.2f * Gap))
			[
				SNew(STextBlock).Text(Verb).Font(SignFont("Regular", Text)).ColorAndOpacity(FLinearColor::White)
			];
	};
	auto AddKeys = [&](int32 Column, const TArray<FKey>& Keys, EHorizontalAlignment Align)
	{
		if (!Keys.IsEmpty())
		{
			Grid->AddSlot(Column, Row).VAlign(VAlign_Center).HAlign(Align).Padding(FMargin(0.f, 0.2f * Gap))[SLBAKeyGlyph::MakeKeys(Keys, Glyph)];
		}
	};
	if (bBoth && !Hints.IsEmpty())
	{
		for (int32 Column : { 0, 2 })
		{
			Grid->AddSlot(Column, Row).HAlign(Column == 0 ? HAlign_Right : HAlign_Left).VAlign(VAlign_Bottom).Padding(FMargin(0.f, 0.f, 0.f, 0.3f * Gap))
				[
					SNew(STextBlock).Text(DeviceName(Column == 0)).Font(SignFont("Bold", 0.6f * Text)).ColorAndOpacity(DeviceColor)
				];
		}
		++Row;
	}
	for (const FLBAControlHint& Hint : Hints)
	{
		const TArray<FKey> PadKeys = LBAControls::ForDevice(Hint.Keys, true);
		const TArray<FKey> DeskKeys = LBAControls::ForDevice(Hint.Keys, false);
		if (bBoth)
		{
			AddKeys(0, PadKeys, HAlign_Right);
			AddVerb(1, Hint.Verb, HAlign_Center);
			AddKeys(2, DeskKeys, HAlign_Left);
		}
		else
		{
			const TArray<FKey>& Keys = bGamepad ? PadKeys : DeskKeys;
			if (Keys.IsEmpty())
			{
				continue;
			}
			AddKeys(0, Keys, HAlign_Right);
			AddVerb(1, Hint.Verb, HAlign_Left);
		}
		++Row;
	}
	if (Row > 0)
	{
		Box->AddSlot().AutoHeight().HAlign(bBoth ? HAlign_Center : HAlign_Left).Padding(FMargin(0.f, Gap, 0.f, 0.f))[Grid];
	}
	if (!Notes.IsEmpty())
	{
		Box->AddSlot().AutoHeight().Padding(FMargin(0.f, Gap, 0.f, 0.f))
			[
				SNew(STextBlock).Text(Notes).Font(SignFont("Regular", 0.85f * Text)).ColorAndOpacity(NotesColor)
			];
	}
	return Box;
}

#undef LOCTEXT_NAMESPACE
