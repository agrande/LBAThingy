using UnrealBuildTool;

public class LBA2Runtime : ModuleRules
{
	public LBA2Runtime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"AnimGraphRuntime",
			"AnimationCore",
			"InputCore",
			"EnhancedInput",
			"Water",
			"Niagara",
			"UMG",
			"Slate",
			"SlateCore",
		});

		// Globe mode's debug command reading material compile errors (lba.Globe.SeaErrors).
		PrivateDependencyModuleNames.Add("RHI");
	}
}
