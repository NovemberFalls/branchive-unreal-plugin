// Copyright 2026 Bits, LLC. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "Misc/IQueuedWork.h"

class IBranchiveSourceControlWorker;
class FBranchiveSourceControlProvider;

/**
 * Queued unit of work executed on the source-control thread pool (mirrors the
 * bundled Git provider's FGitSourceControlCommand).
 */
class FBranchiveSourceControlCommand : public IQueuedWork
{
public:
	FBranchiveSourceControlCommand(
		const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation,
		const TSharedRef<class IBranchiveSourceControlWorker, ESPMode::ThreadSafe>& InWorker,
		const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete());

	bool DoWork();
	virtual void Abandon() override;
	virtual void DoThreadedWork() override;
	ECommandResult::Type ReturnResults();

public:
	/** Resolved path to the `lore` binary. */
	FString PathToLoreBinary;

	/** Absolute path to the workspace root (== project dir); the CLI cwd. */
	FString PathToRepositoryRoot;

	/** The workspace's remote_url (from .lore/config.toml), may be empty. */
	FString RemoteUrl;

	/** Human branch name known at issue time (may be empty until a status runs). */
	FString BranchName;

	/**
	 * Per-workspace mutex. ALL mutating operations serialize on this
	 * (contract §3.5). Reads fold into the same lock for a simpler/safer default.
	 */
	FCriticalSection* WorkspaceMutex = nullptr;

	/** Files this session has an OWN lock on (snapshot, for own-vs-other display). */
	TSet<FString> SessionLockedFiles;

	/** Operation (inward params + outward results). */
	TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe> Operation;

	/** The worker that does the work. */
	TSharedRef<class IBranchiveSourceControlWorker, ESPMode::ThreadSafe> Worker;

	/** Completion delegate. */
	FSourceControlOperationComplete OperationCompleteDelegate;

	/** Set once the thread-pool has processed this command. */
	volatile int32 bExecuteProcessed = 0;

	/** Whether the command succeeded. */
	bool bCommandSuccessful = false;

	/** Auto-deleted in Tick() (asynchronous commands only). */
	bool bAutoDelete = false;

	/** Concurrency mode. */
	EConcurrency::Type Concurrency = EConcurrency::Synchronous;

	/** Files to operate on (absolute). */
	TArray<FString> Files;

	/** Info / error message storage. */
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
};
