// Copyright 2026 Bits, LLC. All Rights Reserved.
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

class FBranchiveCloudAuth;
class FBranchiveSourceControlMenu;

class FBranchiveSourceControlModule : public IModuleInterface
{
public:
	/** IModuleInterface */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FBranchiveSourceControlSettings& AccessSettings() { return Settings; }
	void SaveSettings();

	FBranchiveSourceControlProvider& GetProvider() { return Provider; }

	/** The cloud sign-in service (may be null before startup / after shutdown). */
	FBranchiveCloudAuth* GetCloudAuth() const { return CloudAuth; }

	/** Convenience accessor (may return nullptr if the module is not loaded). */
	static FBranchiveSourceControlModule* GetThreadSafe();

private:
	/** Deferred to post-engine-init: register the conflict menu + restore a prior sign-in. */
	void OnPostEngineInit();

	FBranchiveSourceControlProvider Provider;
	FBranchiveSourceControlSettings Settings;

	// Raw-owned (deleted in ShutdownModule where the full types are visible).
	FBranchiveCloudAuth* CloudAuth = nullptr;
	FBranchiveSourceControlMenu* ConflictMenu = nullptr;

	FDelegateHandle PostEngineInitHandle;
};
