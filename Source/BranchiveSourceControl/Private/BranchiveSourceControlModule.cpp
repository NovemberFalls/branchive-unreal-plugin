// Copyright Branchive.
#include "BranchiveSourceControlModule.h"

#include "BranchiveSourceControlLog.h"
#include "BranchiveSourceControlOperations.h"
#include "Features/IModularFeatures.h"
#include "Misc/App.h"
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

	Settings.LoadSettings();

	// Bind our provider to the editor as a modular feature so it appears in the
	// Revision Control provider dropdown (contract / UEPlasticPlugin pattern).
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &Provider);

	UE_LOG(LogBranchiveSourceControl, Log, TEXT("Branchive source control module started (contract 2.0.0)."));
}

void FBranchiveSourceControlModule::ShutdownModule()
{
	Provider.Close();
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &Provider);
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
