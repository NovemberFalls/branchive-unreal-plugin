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
				// Cloud sign-in (auth spec): BFF HTTP client + JSON parsing.
				"HTTP",
				"Json",
				// Conflict-resolution menu glue: content-browser asset context menu.
				"AssetRegistry",
				"ContentBrowser",
			}
		);

		// The CLI-text parsers + the engine-independent Cloud sign-in core
		// (Lore/LoreParse.*, Lore/LoreErrors.*, Lore/LorePkce.*, Lore/LoreLoopback.*,
		// Lore/LoreConfigPin.*) are pure std C++ shared verbatim with standalone fixture
		// tests. Disable unity so UE's global macros (TEXT/check/min/max/...) can never
		// bleed into a std translation unit in a shared unity blob.
		bUseUnity = false;

		// This is a small, self-contained plugin; skip strict IWYU enforcement.
		IWYUSupport = IWYUSupport.None;

		// Platform security primitives for token-at-rest + the crypto-strong PKCE RNG,
		// and the loopback listener's sockets (auth spec §2.4). The core cpp files also
		// carry #pragma comment(lib, ...) for MSVC; these keep the link correct under
		// UBT and non-MSVC toolchains.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] {
				"Crypt32.lib", // DPAPI (CryptProtectData/CryptUnprotectData)
				"Bcrypt.lib",  // BCryptGenRandom (CSPRNG)
				"Ws2_32.lib",  // loopback listener sockets
			});
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.AddRange(new string[] {
				"Security",       // Keychain Services + SecRandomCopyBytes
				"CoreFoundation", // CFData/CFDictionary for the Keychain calls
			});
		}
	}
}
