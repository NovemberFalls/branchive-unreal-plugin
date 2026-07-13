// Copyright Branchive.
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
