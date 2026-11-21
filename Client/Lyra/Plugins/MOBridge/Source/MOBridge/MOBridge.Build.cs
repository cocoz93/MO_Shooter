using System.IO;
using UnrealBuildTool;

public class MOBridge : ModuleRules
{
	public MOBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Shared code lives at the MO_Shooter repo root, not in this plugin: server, bots and this plugin build the same files (MOBShared*.cpp wrappers).
		string SharedDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../../../../../Shared"));
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
			"EnhancedInput",	// reads the order of the action bindings — Lyra calls same-frame presses in that order (press order L-5-c)
			"ApplicationCore",	// the key test sends its keys from my user's input device (IPlatformInputDeviceMapper) — Lyra filters input by platform user
			"Chaos",
			"PhysicsCore",
			"Sockets",
			"Networking",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"GameplayMessageRuntime",
			"AIModule",
			"NavigationSystem",	// reads the walkable-floor data for the bots (MOBNavDumpSubsystem)
			"ModularGameplay",
			"MovieSceneCapture",
			"Slate",
			"SlateCore",
			"CommonUI",		// the content probe reads the UI input mode (Menu = game keys do not reach the player) in the post-game phase
			"CommonInput",
			"LyraGame",
		});
	}
}
