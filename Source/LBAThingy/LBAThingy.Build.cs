// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LBAThingy : ModuleRules
{
	public LBAThingy(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"LBAThingy",
			"LBAThingy/Variant_Platforming",
			"LBAThingy/Variant_Platforming/Animation",
			"LBAThingy/Variant_Combat",
			"LBAThingy/Variant_Combat/AI",
			"LBAThingy/Variant_Combat/Animation",
			"LBAThingy/Variant_Combat/Gameplay",
			"LBAThingy/Variant_Combat/Interfaces",
			"LBAThingy/Variant_Combat/UI",
			"LBAThingy/Variant_SideScrolling",
			"LBAThingy/Variant_SideScrolling/AI",
			"LBAThingy/Variant_SideScrolling/Gameplay",
			"LBAThingy/Variant_SideScrolling/Interfaces",
			"LBAThingy/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
