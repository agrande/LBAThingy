using UnrealBuildTool;

public class LBA2Shaders : ModuleRules
{
	public LBA2Shaders(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"Projects",
		});
	}
}
