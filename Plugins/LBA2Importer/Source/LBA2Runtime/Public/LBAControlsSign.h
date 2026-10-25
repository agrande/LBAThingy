#pragma once

#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "LBAControls.h"
#include "LBAControlsSign.generated.h"

class SWidget;
class UMaterialInterface;
class UStaticMeshComponent;
class UWidgetComponent;

/** One line of a controls sign: the keys, then what they do. */
USTRUCT(BlueprintType)
struct LBA2RUNTIME_API FLBAControlHint
{
	GENERATED_BODY()

	/** What the keys do ("Jump", "Climb: pump it"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA")
	FText Verb;

	/** Keys of every device (LBAControls lists them); each layout shows its own, a line without any is left out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LBA")
	TArray<FKey> Keys;
};

/**
 * A board on a post explaining controls with the glyphs of the device in use: keyboard and mouse or gamepad, switching
 * as soon as the player picks up the other one (ULBAControlsSubsystem). The editor shows both, gamepad on the left,
 * keyboard and mouse on the right. Title, control lines, then notes.
 *
 * The board is sized to its content. The panel is a world-space widget of the sign's own Slate glyphs (SLBAKeyGlyph);
 * PanelMaterial draws it after the post-process so the palette shading leaves it alone.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ALBAControlsSign : public AActor
{
	GENERATED_BODY()

public:
	ALBAControlsSign();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	FText Title;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	FLinearColor TitleColor = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	TArray<FLBAControlHint> Hints;

	/** Lines under the controls. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign", meta = (MultiLine = true))
	FText Notes;

	/** Height of the board's bottom above the actor (cm); no post when 0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign", meta = (ClampMin = 0, Units = "cm"))
	float PostHeight = 100.f;

	/** Title and text letter heights (cm); key glyphs are 1.5 times the text. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign", meta = (ClampMin = 1, Units = "cm"))
	float TitleSize = 70.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign", meta = (ClampMin = 1, Units = "cm"))
	float TextSize = 26.f;

	/** Size of one pixel of the panel's texture (cm): smaller is sharper. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign", meta = (ClampMin = 0.05, Units = "cm"))
	float PixelSize = 0.4f;

	/** Board and post. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	TObjectPtr<UMaterialInterface> BoardMaterial;

	/** The panel's widget material; empty uses the engine's one-sided translucent widget material. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	TObjectPtr<UMaterialInterface> PanelMaterial;

	/** Rebuilds the panel for the current layout and resizes the board to it. */
	UFUNCTION(BlueprintCallable, Category = "LBA|Sign")
	void Refresh();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	TObjectPtr<UStaticMeshComponent> Post;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	TObjectPtr<UStaticMeshComponent> Board;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LBA|Sign")
	TObjectPtr<UWidgetComponent> Panel;

private:
	TSharedRef<SWidget> BuildContent(ELBAControlsLayout Layout) const;

	FDelegateHandle LayoutChangedHandle;
	ELBAControlsLayout ShownLayout = ELBAControlsLayout::Both;
	bool bBuilt = false;
};
