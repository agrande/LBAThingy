using UnrealBuildTool;

public class LBA2Editor : ModuleRules
{
	public LBA2Editor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"LBA2Runtime",
			"UnrealEd",
			"LevelEditor",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UMG",
			"InputCore",
			"PropertyEditor",
			"DesktopPlatform",
			"DeveloperSettings",
			"Projects",
			"Json",
			"AssetTools",
			"AssetRegistry",
			"MeshDescription",
			"StaticMeshDescription",
			"SkeletalMeshDescription",
			"SkeletalMeshUtilitiesCommon",
			"MaterialEditor",
			"PhysicsCore",
			"AudioEditor",
			"AnimationCore",
			"ImageWrapper",
			"AnimationBlueprintLibrary",
			"Water",
			"Niagara",
			"RenderCore",
		});
	}
}
