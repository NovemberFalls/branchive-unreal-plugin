// Copyright 2026 Bits, LLC. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "IBranchiveSourceControlWorker.h"
#include "Misc/EngineVersionComparison.h"

class FBranchiveSourceControlState;
class FBranchiveSourceControlCommand;

DECLARE_DELEGATE_RetVal(FBranchiveSourceControlWorkerRef, FGetBranchiveSourceControlWorker)

/**
 * The Branchive ISourceControlProvider. Registered as a "SourceControl" modular
 * feature so "Branchive" appears in UE's Revision Control provider dropdown.
 * Project-level (one workspace == the UE project directory).
 */
class FBranchiveSourceControlProvider : public ISourceControlProvider
{
public:
	FBranchiveSourceControlProvider() = default;

	// ---- ISourceControlProvider --------------------------------------------
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual TMap<EStatus, FString> GetStatus() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual const FName& GetName() const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override { return false; }
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRoot) override {}
	virtual int32 GetStateBranchIndex(const FString& InBranchName) const override { return INDEX_NONE; }
	virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanExecuteOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual bool CanCancelOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual void CancelOperation(const FSourceControlOperationRef& InOperation) override;
	virtual bool UsesLocalReadOnlyState() const override;
	virtual bool UsesChangelists() const override;
	virtual bool UsesUncontrolledChangelists() const override;
	virtual bool UsesCheckout() const override;
	virtual bool UsesFileRevisions() const override;
	virtual bool UsesSnapshots() const override;
	virtual bool AllowsDiffAgainstDepot() const override;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	// UE 5.8 made these two `final` in ISourceControlProvider (they now carry a
	// base default and are deprecated). On 5.4/5.6 they are pure-virtual, so we
	// must still override them there. Compile the override only pre-5.8.
	virtual TOptional<bool> IsAtLatestRevision() const override;
	virtual TOptional<int> GetNumLocalChanges() const override;
#else
	// UE 5.8 added four new pure-virtuals to ISourceControlProvider (and turned the
	// two above into `final`). HasChangesToSync / HasChangesToCheckIn are the direct
	// replacements for IsAtLatestRevision / GetNumLocalChanges. None of these four
	// exist pre-5.8, so they compile only on 5.8+.
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override;
	virtual bool UsesSoftRevertOnDelete() const override;
	virtual TOptional<bool> HasChangesToSync() const override;
	virtual TOptional<bool> HasChangesToCheckIn() const override;
#endif
	virtual void Tick() override;
	virtual TArray<TSharedRef<class ISourceControlLabel>> GetLabels(const FString& InMatchingSpec) const override;
	virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type InStateCacheUsage) override;
#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif

	using ISourceControlProvider::Execute;

	// ---- Branchive-specific ------------------------------------------------
	/** Resolve the lore binary + workspace root; read remote_url. */
	void CheckLoreAvailability();
	void CheckWorkspaceStatus();

	bool IsLoreAvailable() const { return bLoreAvailable; }
	const FString& GetPathToLoreBinary() const { return PathToLoreBinary; }
	const FString& GetPathToRepositoryRoot() const { return PathToRepositoryRoot; }
	const FString& GetRemoteUrl() const { return RemoteUrl; }
	const FString& GetBranchName() const { return BranchName; }
	void SetBranchName(const FString& In) { BranchName = In; }
	void SetIsCurrent(bool bIn) { bLocalBranchIsCurrent = bIn; }

	/** Session-owned locks (files WE locked this run — for own-vs-other display). */
	void AddSessionLock(const FString& AbsFile);
	void RemoveSessionLock(const FString& AbsFile);
	TSet<FString> GetSessionLockedFilesSnapshot() const;

	/** Per-workspace mutex all mutating ops serialize on (contract §3.5). */
	FCriticalSection& GetWorkspaceMutex() { return WorkspaceMutex; }

	/** State cache access used by workers on the game thread. */
	TSharedRef<FBranchiveSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& Filename);
	bool RemoveFileFromCache(const FString& Filename);

	void RegisterWorker(const FName& InName, const FGetBranchiveSourceControlWorker& InDelegate);

private:
	TSharedPtr<class IBranchiveSourceControlWorker, ESPMode::ThreadSafe> CreateWorker(const FName& InOperationName) const;
	ECommandResult::Type ExecuteSynchronousCommand(FBranchiveSourceControlCommand& InCommand, const FText& Task);
	ECommandResult::Type IssueCommand(FBranchiveSourceControlCommand& InCommand);
	void OutputCommandMessages(const FBranchiveSourceControlCommand& InCommand) const;

private:
	bool bLoreAvailable = false;
	bool bWorkspaceFound = false;
	bool bLocalBranchIsCurrent = true;

	FString PathToLoreBinary;
	FString PathToRepositoryRoot;
	FString RemoteUrl;
	FString BranchName;

	/** State cache. */
	TMap<FString, TSharedRef<class FBranchiveSourceControlState, ESPMode::ThreadSafe>> StateCache;

	/** Registered operation workers. */
	TMap<FName, FGetBranchiveSourceControlWorker> WorkersMap;

	/** In-flight commands. */
	TArray<FBranchiveSourceControlCommand*> CommandQueue;

	/** State-changed multicast. */
	FSourceControlStateChanged OnSourceControlStateChanged;

	/** Files locked by this session. */
	mutable FCriticalSection SessionLockMutex;
	TSet<FString> SessionLockedFiles;

	/** Serializes mutating ops per workspace. */
	FCriticalSection WorkspaceMutex;
};
