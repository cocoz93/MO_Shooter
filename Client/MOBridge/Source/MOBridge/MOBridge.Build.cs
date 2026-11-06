using System.IO;
using UnrealBuildTool;

public class MOBridge : ModuleRules
{
	public MOBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Shared code lives in the MO_Shooter repo, not here: server, bots and this plugin build the same files (MOBShared*.cpp wrappers).
		string SharedDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../../../../../../Desktop/MyGit/MO_Shooter/Shared"));
		if (!Directory.Exists(SharedDir)) throw new BuildException("MO_Shooter/Shared not found: " + SharedDir);
		PrivateIncludePaths.Add(SharedDir);
		bUseUnity = false;	// each Shared .cpp has its own anonymous-namespace names
		FPSemantics = FPSemanticsMode.Precise;	// same floating point rules as the server build, or the prediction drifts by rounding

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AnimationCore",	// two-bone IK for the simple anim hands (MOBSimpleAnimInstance)
			"InputCore",
			"Chaos",
			"PhysicsCore",
			"Sockets",
			"Networking",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"GameplayMessageRuntime",
			"AIModule",
			"ModularGameplay",
			"MovieSceneCapture",
			"Slate",
			"SlateCore",
			"LyraGame",
		});
	}
}
