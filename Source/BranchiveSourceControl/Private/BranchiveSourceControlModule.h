// Copyright Branchive.
//
// Branchive source control plugin for Unreal Engine 5 — wraps the `lore` CLI
// behind ISourceControlProvider so "Branchive" appears in the Revision Control
// provider dropdown. Implements the core loop per INTEGRATIONS-CONTRACT.md
// v2.0.0 §5.3: Connect, UpdateStatus (+ lock state), CheckOut = lock,
// CheckIn = commit->push->release, Revert = file reset, Sync = pull,
// MarkForAdd, Delete.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "BranchiveSourceControlProvider.h"
#include "BranchiveSourceControlSettings.h"

class FBranchiveSourceControlModule : public IModuleInterface
{
public:
	/** IModuleInterface */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FBranchiveSourceControlSettings& AccessSettings() { return Settings; }
	void SaveSettings();

	FBranchiveSourceControlProvider& GetProvider() { return Provider; }

	/** Convenience accessor (may return nullptr if the module is not loaded). */
	static FBranchiveSourceControlModule* GetThreadSafe();

private:
	FBranchiveSourceControlProvider Provider;
	FBranchiveSourceControlSettings Settings;
};
