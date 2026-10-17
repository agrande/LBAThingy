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
			"InputCore",
			"EnhancedInput",
			"Water",
		});
	}
}
