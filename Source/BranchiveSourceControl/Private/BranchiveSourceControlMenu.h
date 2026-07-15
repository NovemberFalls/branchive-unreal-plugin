// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Surfaces the (already-wired, prior-tier) whole-side conflict operations in the
// Content Browser asset context menu (contract §4.19/§4.20): for a CONFLICTED asset
// it offers "Resolve Using Mine (Ours)", "Resolve Using Theirs", and "Abort Merge",
// dispatching the existing FBranchiveResolveMine / FBranchiveResolveTheirs /
// FBranchiveAbortMerge operations through the active provider.
//
// Uses the stable Content Browser asset-view context-menu extender delegate
// (public API, identical across UE 5.4/5.6) rather than a version-specific tool-menu
// hook name. Entries appear ONLY when Branchive is the active provider AND at least
// one selected asset is currently conflicted.
#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"

class FExtender;
struct FAssetData;

class FBranchiveSourceControlMenu
{
public:
	/** Register the content-browser extender. Safe to call once the editor is up. */
	void Register();
	/** Remove the extender. Safe to call multiple times. */
	void Unregister();

private:
	TSharedRef<FExtender> OnExtendAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets);

	FDelegateHandle ContentBrowserExtenderHandle;
	bool bRegistered = false;
};
