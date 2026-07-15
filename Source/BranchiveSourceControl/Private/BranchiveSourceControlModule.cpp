// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "BranchiveSourceControlModule.h"

#include "BranchiveSourceControlLog.h"
#include "BranchiveSourceControlMenu.h"
#include "BranchiveSourceControlOperations.h"
#include "Cloud/BranchiveCloudAuth.h"
#include "Features/IModularFeatures.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogBranchiveSourceControl);

#define LOCTEXT_NAMESPACE "BranchiveSourceControl"

template <typename WorkerType>
static FBranchiveSourceControlWorkerRef CreateWorker()
{
	return MakeShared<WorkerType, ESPMode::ThreadSafe>();
}

void FBranchiveSourceControlModule::StartupModule()
{
	// One worker per source-control operation (contract §5.3).
	Provider.RegisterWorker("Connect",      FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveConnectWorker>));
	Provider.RegisterWorker("UpdateStatus", FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveUpdateStatusWorker>));
	Provider.RegisterWorker("CheckOut",     FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveCheckOutWorker>));   // = lock acquire
	Provider.RegisterWorker("CheckIn",      FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveCheckInWorker>));    // = commit+push+release
	Provider.RegisterWorker("MarkForAdd",   FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveMarkForAddWorker>));
	Provider.RegisterWorker("Delete",       FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveDeleteWorker>));
	Provider.RegisterWorker("Revert",       FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveRevertWorker>));     // = file reset
	Provider.RegisterWorker("Sync",         FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveSyncWorker>));       // = pull

	// Conflict resolution — whole-side ours/theirs, Perforce-style (contract §4.19/§4.20).
	Provider.RegisterWorker("BranchiveResolveMine",   FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveResolveMineWorker>));   // = branch merge resolve mine
	Provider.RegisterWorker("BranchiveResolveTheirs", FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveResolveTheirsWorker>)); // = branch merge resolve theirs
	Provider.RegisterWorker("BranchiveAbortMerge",    FGetBranchiveSourceControlWorker::CreateStatic(&CreateWorker<FBranchiveAbortMergeWorker>));    // = branch merge abort

	Settings.LoadSettings();

	// Bind our provider to the editor as a modular feature so it appears in the
	// Revision Control provider dropdown (contract / UEPlasticPlugin pattern).
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &Provider);

	// Branchive Cloud sign-in service (auth spec). Created here; the conflict menu
	// registration + prior-session restore are deferred to post-engine-init (the
	// content browser / Slate are not ready this early).
	CloudAuth = new FBranchiveCloudAuth();
	ConflictMenu = new FBranchiveSourceControlMenu();
	// UE 5.8 deprecated the direct `OnPostEngineInit` member in favour of the
	// GetOnPostEngineInit() accessor (which does not exist pre-5.8).
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FBranchiveSourceControlModule::OnPostEngineInit);
#else
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FBranchiveSourceControlModule::OnPostEngineInit);
#endif

	UE_LOG(LogBranchiveSourceControl, Log, TEXT("Branchive source control module started (contract 2.0.0)."));
}

void FBranchiveSourceControlModule::OnPostEngineInit()
{
	if (ConflictMenu)
	{
		ConflictMenu->Register();
	}
	if (CloudAuth)
	{
		CloudAuth->RestoreOnStartup();
	}
}

void FBranchiveSourceControlModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
#else
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
#endif
		PostEngineInitHandle.Reset();
	}

	if (ConflictMenu)
	{
		ConflictMenu->Unregister();
		delete ConflictMenu;
		ConflictMenu = nullptr;
	}

	Provider.Close();
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &Provider);

	if (CloudAuth)
	{
		delete CloudAuth; // dtor cancels the refresh timer + clears the static accessor
		CloudAuth = nullptr;
	}
}

void FBranchiveSourceControlModule::SaveSettings()
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}
	Settings.SaveSettings();
}

FBranchiveSourceControlModule* FBranchiveSourceControlModule::GetThreadSafe()
{
	return FModuleManager::GetModulePtr<FBranchiveSourceControlModule>("BranchiveSourceControl");
}

IMPLEMENT_MODULE(FBranchiveSourceControlModule, BranchiveSourceControl);

#undef LOCTEXT_NAMESPACE
