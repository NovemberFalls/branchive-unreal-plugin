// Copyright Branchive. Licensed for the Branchive Unreal source control plugin.

using UnrealBuildTool;

public class BranchiveSourceControl : ModuleRules
{
	public BranchiveSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"Slate",
				"SlateCore",
				"InputCore",
				"SourceControl",
			}
		);

		// The CLI-text parsers (Lore/LoreParse.*, Lore/LoreErrors.*) are pure
		// std C++ shared verbatim with a standalone fixture test. Disable unity
		// so UE's global macros (TEXT/check/min/max/...) can never bleed into a
		// std translation unit in a shared unity blob.
		bUseUnity = false;

		// This is a small, self-contained plugin; skip strict IWYU enforcement.
		IWYUSupport = IWYUSupport.None;
	}
}
