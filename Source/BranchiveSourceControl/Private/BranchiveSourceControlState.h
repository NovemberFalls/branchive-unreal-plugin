// Copyright Branchive.
#pragma once

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "ISourceControlRevision.h"

/** Working-copy state for a single file, derived from `lore status --scan`. */
namespace EBranchiveWorkingCopyState
{
	enum Type
	{
		Unknown,
		Unchanged,     // tracked, clean
		Added,         // new file staged/untracked (Lore code 'A')
		Deleted,       // 'D'
		Modified,      // 'M'
		Conflicted,    // in "Changes in conflict:"
		NotControlled, // exists on disk, not tracked and not staged
		Ignored,
	};
}

/**
 * FBranchiveSourceControlState maps Lore's status + lock state onto UE's
 * ISourceControlState. The headline is IsCheckedOutOther() — a FOREIGN Lore lock
 * drives the Content Browser's "Checked out by <user>" icon (contract §5.3).
 */
class FBranchiveSourceControlState : public ISourceControlState
{
public:
	explicit FBranchiveSourceControlState(const FString& InLocalFilename)
		: LocalFilename(InLocalFilename)
	{
	}

	// ---- ISourceControlState (history — deferred for v1) -------------------
	virtual int32 GetHistorySize() const override { return 0; }
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32) const override { return nullptr; }
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32) const override { return nullptr; }
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString&) const override { return nullptr; }
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override { return nullptr; }
	virtual FResolveInfo GetResolveInfo() const override { return PendingResolveInfo; }

#if SOURCE_CONTROL_WITH_SLATE
	virtual FSlateIcon GetIcon() const override;
#endif
	virtual FText GetDisplayName() const override;
	virtual FText GetDisplayTooltip() const override;
	virtual const FString& GetFilename() const override { return LocalFilename; }
	virtual const FDateTime& GetTimeStamp() const override { return TimeStamp; }

	// ---- check-in / check-out ----------------------------------------------
	virtual bool CanCheckIn() const override;
	virtual bool CanCheckout() const override;
	virtual bool IsCheckedOut() const override;
	virtual bool IsCheckedOutOther(FString* Who = nullptr) const override;

	// ---- other-branch concepts (not modeled for v1) ------------------------
	virtual bool IsCheckedOutInOtherBranch(const FString& = FString()) const override { return false; }
	virtual bool IsModifiedInOtherBranch(const FString& = FString()) const override { return false; }
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override
	{
		return IsCheckedOutInOtherBranch(CurrentBranch) || IsModifiedInOtherBranch(CurrentBranch);
	}
	virtual TArray<FString> GetCheckedOutBranches() const override { return TArray<FString>(); }
	virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
	virtual bool GetOtherBranchHeadModification(FString&, FString&, int32&) const override { return false; }

	// ---- working-copy predicates -------------------------------------------
	virtual bool IsCurrent() const override { return bIsCurrent; }
	virtual bool IsSourceControlled() const override;
	virtual bool IsAdded() const override { return WorkingCopyState == EBranchiveWorkingCopyState::Added; }
	virtual bool IsDeleted() const override { return WorkingCopyState == EBranchiveWorkingCopyState::Deleted; }
	virtual bool IsIgnored() const override { return WorkingCopyState == EBranchiveWorkingCopyState::Ignored; }
	virtual bool CanEdit() const override { return !bLockedByOther; }
	virtual bool CanDelete() const override;
	virtual bool IsUnknown() const override { return WorkingCopyState == EBranchiveWorkingCopyState::Unknown; }
	virtual bool IsModified() const override;
	virtual bool CanAdd() const override { return WorkingCopyState == EBranchiveWorkingCopyState::NotControlled; }
	virtual bool IsConflicted() const override { return WorkingCopyState == EBranchiveWorkingCopyState::Conflicted; }
	virtual bool CanRevert() const override;

public:
	/** Absolute path on disk. */
	FString LocalFilename;

	/** Working-copy state from `lore status --scan`. */
	EBranchiveWorkingCopyState::Type WorkingCopyState = EBranchiveWorkingCopyState::Unknown;

	/** True if some Lore lock exists on this file held by the CURRENT session. */
	bool bLockedBySelf = false;
	/** True if a FOREIGN Lore lock exists on this file. */
	bool bLockedByOther = false;
	/** The foreign lock's owner (raw sub, or the "<unknown>" sentinel). */
	FString LockOwner;

	/** Up-to-date with the remote (false when behind/diverged). */
	bool bIsCurrent = true;

	/** Populated only while a merge conflict is pending (deferred UI for v1). */
	FResolveInfo PendingResolveInfo;

	/** Timestamp of the last state update. */
	FDateTime TimeStamp = FDateTime::Now();
};
