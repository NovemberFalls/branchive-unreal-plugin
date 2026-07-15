// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// One worker per source-control operation. The provider dispatches to these by
// operation name (contract §5.3 mapping):
//   Connect      -> verify workspace + binary reachable
//   UpdateStatus -> `lore status --scan` + `lore lock query --json` (state + locks)
//   CheckOut     -> `lore lock acquire`                       (== UE checkout)
//   CheckIn      -> stage -> commit -> push -> release-own-locks (composite, §5.3)
//   MarkForAdd   -> `lore stage <file>`
//   Delete       -> `lore stage <file>`   (records the on-disk deletion)
//   Revert       -> `lore file reset` (+ release own lock)   (NOT revision revert)
//   Sync         -> `lore sync`                              (== pull)
#pragma once

#include "CoreMinimal.h"
#include "IBranchiveSourceControlWorker.h"
#include "BranchiveSourceControlState.h"
#include "BranchiveSourceControlRevision.h"

/** A resolved per-file state, applied to the provider cache on the game thread. */
struct FBranchiveResolvedFileState
{
	FString AbsFilename;
	EBranchiveWorkingCopyState::Type State = EBranchiveWorkingCopyState::Unknown;
	bool bLockedBySelf = false;
	bool bLockedByOther = false;
	FString LockOwner;
	bool bIsCurrent = true;
};

/** Base worker: carries the common results and the game-thread apply logic. */
class FBranchiveSourceControlWorkerBase : public IBranchiveSourceControlWorker
{
public:
	virtual bool UpdateStates() const override;

protected:
	/** Apply-on-game-thread payload, filled by Execute() on the worker thread. */
	mutable TArray<FBranchiveResolvedFileState> States;
	FString OutBranch;
	bool bOutIsCurrent = true;
	TArray<FString> AcquiredLocks; // add to provider's session-lock set
	TArray<FString> ReleasedLocks; // remove from provider's session-lock set

	/** History payload (abs filename -> newest-first revisions), applied in UpdateStates. */
	mutable TMap<FString, TArray<FBranchiveSourceControlRevisionRef>> HistoryByFile;
};

class FBranchiveConnectWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "Connect"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

class FBranchiveUpdateStatusWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "UpdateStatus"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

class FBranchiveCheckOutWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "CheckOut"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

class FBranchiveCheckInWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "CheckIn"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

class FBranchiveMarkForAddWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "MarkForAdd"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

class FBranchiveDeleteWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "Delete"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

class FBranchiveRevertWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "Revert"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

class FBranchiveSyncWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "Sync"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};

// ---- conflict resolution (contract §4.19 / §4.20) --------------------------

/** Shared base for the ours/theirs whole-side resolve workers. */
class FBranchiveResolveWorkerBase : public FBranchiveSourceControlWorkerBase
{
public:
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;

protected:
	/** "mine" (ours) or "theirs" (incoming). */
	virtual bool ResolveUsingTheirs() const = 0;
};

class FBranchiveResolveMineWorker : public FBranchiveResolveWorkerBase
{
public:
	virtual FName GetName() const override { return "BranchiveResolveMine"; }
protected:
	virtual bool ResolveUsingTheirs() const override { return false; }
};

class FBranchiveResolveTheirsWorker : public FBranchiveResolveWorkerBase
{
public:
	virtual FName GetName() const override { return "BranchiveResolveTheirs"; }
protected:
	virtual bool ResolveUsingTheirs() const override { return true; }
};

class FBranchiveAbortMergeWorker : public FBranchiveSourceControlWorkerBase
{
public:
	virtual FName GetName() const override { return "BranchiveAbortMerge"; }
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) override;
};
