#pragma once

#include "Blueprint/UserWidget.h"
#include "LBAIslandTravelSubsystem.h"
#include "LBASceneTravelSubsystem.h"
#include "LBAIslandMenu.generated.h"

class UButton;
class UHorizontalBox;
class UScrollBox;
class UTextBlock;
class UTexture2D;
class UVerticalBox;
class ULBAIslandMenu;

/** What a menu button does. */
UENUM()
enum class ELBAMenuAction : uint8
{
	/** Shows a planet's islands. */
	Planet,
	/** Shows an island's scenes. */
	Island,
	/** Goes to an island level's start. */
	IslandStart,
	/** Goes to a scene's hero start. */
	Scene,
	/** Shows the test levels and sandboxes. */
	TestsTab,
	/** Shows a test level's stations. */
	TestLevel,
	/** Goes to a test level's start. */
	TestLevelStart,
	/** Goes to a station of the test level shown. */
	Station,
	/** Editor: plays from the place last picked. */
	Play,
	Quit,
};

/** Forwards one button's click to the menu with what the button does (UButton::OnClicked has no parameters). */
UCLASS()
class ULBAIslandMenuButtonHandler : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TObjectPtr<ULBAIslandMenu> Menu;

	ELBAMenuAction Action = ELBAMenuAction::Quit;

	/** ELBAPlanet, index into the menu's islands or test levels, scene number, or index into the shown level's stations. */
	int32 Value = INDEX_NONE;

	UFUNCTION()
	void HandleClicked();
};

/** A button of the menu, and what decides how it is drawn. */
USTRUCT()
struct FLBAMenuButton
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UButton> Button;

	UPROPERTY()
	TObjectPtr<UTextBlock> Label;

	UPROPERTY()
	TObjectPtr<ULBAIslandMenuButtonHandler> Handler;

	/** The planet or island the menu shows. */
	bool bActive = false;

	/** Where the player is. */
	bool bCurrent = false;

	/** Tabs (planets, islands) grey out unless shown, focused or current. */
	bool bTab = false;

	/** Drawn in the feature color when not current (the feature test map). */
	bool bFeature = false;
};

/**
 * Travel menu shown by ALBAPlayerController (M / gamepad Start), laid out like lba2remake's teleport menu: planets
 * along the top, the selected planet's islands on the left, and the selected island's scenes on the right (its
 * areas, each with the interiors it opens on). Picking an island goes to its level's start, picking a scene to that
 * scene's hero start. After the planets, the Tests tab lists the test levels and sandboxes, the feature test map first,
 * each with its stations. Playable with the mouse, the keyboard (arrows, Enter, Q / E for tabs) or a gamepad (d-pad,
 * A, LB / RB). Esc, B, M or Start close it.
 * The layout is built in C++, so it needs no asset; a Blueprint subclass can restyle it through the colors and
 * icons below. The editor's level selector (LBA2Editor) is a subclass that opens levels in the editor instead.
 */
UCLASS(Blueprintable)
class LBA2RUNTIME_API ULBAIslandMenu : public UUserWidget
{
	GENERATED_BODY()

public:
	ULBAIslandMenu(const FObjectInitializer& ObjectInitializer);

	/** Panel behind everything. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor PanelColor = FLinearColor(0.f, 0.f, 0.f, 0.92f);

	/** Panel outline and the line under the planets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor BorderColor = FLinearColor(FColor(97, 206, 206));

	/** Background of the scene list. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor TreeColor = FLinearColor(FColor(45, 45, 45));

	/** Background of the focused or hovered button. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor FocusColor = FLinearColor(FColor(32, 162, 255, 128));

	/** Background of the planet and island shown. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor ActiveColor = FLinearColor(FColor(32, 162, 255, 40));

	/** Text of planets and islands neither shown nor focused. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor IdleTextColor = FLinearColor(FColor(130, 130, 130));

	/** Text of where the player is (planet, island, scene). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor CurrentColor = FLinearColor(1.f, 0.85f, 0.35f, 1.f);

	/** Text of the feature test map, and the outline of its button. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	FLinearColor FeatureColor = FLinearColor(FColor(97, 206, 206));

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	TMap<ELBAPlanet, TSoftObjectPtr<UTexture2D>> PlanetIcons;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	TSoftObjectPtr<UTexture2D> IslandIcon;

	/** Exterior scenes (an area of an island). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	TSoftObjectPtr<UTexture2D> SectionIcon;

	/** Interior scenes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	TSoftObjectPtr<UTexture2D> BuildingIcon;

	/** The Tests tab and the feature test map. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Style")
	TSoftObjectPtr<UTexture2D> TestsIcon;

	/** Icons the menu loads by path (the LBA2Editor module adds them to cooks). */
	TArray<FSoftObjectPath> GetIconPaths() const;

	/** Gives keyboard/gamepad focus to where the player is (their scene or island), or the first island. */
	void FocusDefaultButton();

	/** Scrolls where the player is into view, without taking focus. */
	void ScrollToCurrent();

	void OnButtonClicked(ELBAMenuAction Action, int32 Value);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	/** Goes there: the game travels (and closes the menu), the editor opens the level. */
	virtual void GoTo(const FLBATravelDestination& Destination);

	/** Where the player is, to show their island, scene and station. False if unknown. */
	virtual bool GetViewLocation(FVector& OutLocation) const;

	/** The scene the player is in, -1 if unknown. */
	virtual int32 GetCurrentScene() const;

	/** Editor: plays from the place last picked. */
	virtual void Play() {}

	/**
	 * Set by the editor's level selector: it lives in an editor tab, so it shows Play instead of Quit, never closes, and
	 * only takes keyboard focus when clicked (the rest of the editor keeps it).
	 */
	bool bEditorMode = false;

private:
	void BuildTree();
	void BuildIslandList();
	void BuildSceneTree();
	void BuildTestLevelList();
	void BuildStationList();
	void AddScenes(const TMap<int32, TArray<const FLBASceneMenuEntry*>>& Children, int32 Parent, int32 Depth);
	/** Subtitle: a smaller grey line under the label. */
	FLBAMenuButton& AddButton(TArray<FLBAMenuButton>& List, ELBAMenuAction Action, int32 Value, const FText& Label, const TSoftObjectPtr<UTexture2D>& Icon,
		bool bTab, float Indent = 0.f, const FText& Subtitle = FText::GetEmpty());

	void SelectPlanet(ELBAPlanet Planet);
	void SelectIsland(int32 Island);
	void ShowTests();
	void SelectTestLevel(int32 Level);
	/** Planets then Tests, step tabs to the right (negative: left). */
	void CycleTab(int32 Step);
	/** Where the player is (their scene, station or level start), else the island or test level shown. */
	UButton* FindDefaultButton();
	void ScrollTo(UButton* Target);
	void FocusButton(UButton* Button);
	bool HasMenuFocus(const UButton* Button) const;
	/** Every button: tabs, islands or test levels, scenes or stations, footer. */
	void ForEachButton(TFunctionRef<void(FLBAMenuButton&)> Visit);
	void Close();

	UPROPERTY(Transient)
	TArray<FLBAIslandInfo> Islands;

	TArray<ELBAPlanet> Planets;
	TArray<FLBASceneMenuEntry> Scenes;

	UPROPERTY(Transient)
	TArray<FLBATestLevelInfo> TestLevels;

	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> IslandList;

	UPROPERTY(Transient)
	TObjectPtr<UScrollBox> SceneTree;

	UPROPERTY(Transient)
	TArray<FLBAMenuButton> PlanetButtons;

	UPROPERTY(Transient)
	TArray<FLBAMenuButton> IslandButtons;

	UPROPERTY(Transient)
	TArray<FLBAMenuButton> SceneButtons;

	UPROPERTY(Transient)
	TArray<FLBAMenuButton> FooterButtons;

	TWeakObjectPtr<UButton> LastFocused;
	TOptional<FVector2D> LastCursor;

	ELBAPlanet SelectedPlanet = ELBAPlanet::Twinsun;
	int32 SelectedIsland = INDEX_NONE;
	/** Island level the player is in (index into Islands), -1 in an interior. */
	int32 CurrentIsland = INDEX_NONE;
	/** Island the player is on or in (an interior's island), -1 if unknown. */
	int32 HomeIsland = INDEX_NONE;
	int32 CurrentScene = INDEX_NONE;

	/** The Tests tab is shown instead of a planet. */
	bool bShowTests = false;
	/** Index into TestLevels. */
	int32 SelectedTestLevel = INDEX_NONE;
	/** Test level the player is in, -1 elsewhere. */
	int32 CurrentTestLevel = INDEX_NONE;
	/** The current test level's station the player is at (closest within reach), None if none. */
	FName CurrentStation;
};
