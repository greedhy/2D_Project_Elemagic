// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Elemagic : ModuleRules
{
	public Elemagic(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput",
			"GameplayAbilities", "ModelViewViewModel", "UMG" , "Paper2D" ,"Niagara"});

		PrivateDependencyModuleNames.AddRange(new string[] { "GameplayTags", "GameplayTasks", "NavigationSystem",
			"Niagara", "AIModule"  ,"Slate", "SlateCore"  });

        // Uncomment if you are using Slate UI
        // PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		//,"GameplayTags", "GameplayTasks", "NavigationSystem","Niagara", "AIModule"  ,"Slate", "SlateCore"

		
        // Uncomment if you are using online features
        // PrivateDependencyModuleNames.Add("OnlineSubsystem");

        // To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
    }
}
