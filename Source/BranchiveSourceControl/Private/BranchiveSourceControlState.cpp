// Copyright Branchive.
#include "BranchiveSourceControlState.h"

#include "Lore/LoreParse.h" // UnknownOwnerSentinel

#if SOURCE_CONTROL_WITH_SLATE
#include "RevisionControlStyle/RevisionControlStyle.h"
#endif

#define LOCTEXT_NAMESPACE "BranchiveSourceControl.State"

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FBranchiveSourceControlState::GetHistoryItem(int32 HistoryIndex) const
{
	if (History.IsValidIndex(HistoryIndex))
	{
		return History[HistoryIndex];
	}
	return nullptr;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FBranchiveSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (const FBranchiveSourceControlRevisionRef& Rev : History)
	{
		if (Rev->GetRevisionNumber() == RevisionNumber)
		{
			return Rev;
		}
	}
	return nullptr;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FBranchiveSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const FBranchiveSourceControlRevisionRef& Rev : History)
	{
		// Match the short label OR a prefix of the full signature (UE may pass either).
		if (Rev->GetRevision() == InRevision || Rev->FullSignature.StartsWith(InRevision))
		{
			return Rev;
		}
	}
	return nullptr;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FBranchiveSourceControlState::GetCurrentRevision() const
{
	// Newest-first ordering => index 0 is the tip revision this file is at.
	if (History.Num() > 0)
	{
		return History[0];
	}
	return nullptr;
}

bool FBranchiveSourceControlState::IsSourceControlled() const
{
	return WorkingCopyState != EBranchiveWorkingCopyState::Unknown
	    && WorkingCopyState != EBranchiveWorkingCopyState::NotControlled
	    && WorkingCopyState != EBranchiveWorkingCopyState::Ignored;
}

bool FBranchiveSourceControlState::IsModified() const
{
	return WorkingCopyState == EBranchiveWorkingCopyState::Modified
	    || WorkingCopyState == EBranchiveWorkingCopyState::Conflicted;
}

bool FBranchiveSourceControlState::IsCheckedOut() const
{
	// UE "checked out" == we hold the Lore lock (contract §5.3 CheckOut = lock).
	return bLockedBySelf;
}

bool FBranchiveSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (bLockedByOther)
	{
		if (Who)
		{
			// Render the auth-less-server sentinel gracefully (contract §4.17).
			*Who = (LockOwner == UTF8_TO_TCHAR(BranchiveLore::UnknownOwnerSentinel))
				? LOCTEXT("UnidentifiedSession", "an unidentified session").ToString()
				: LockOwner;
		}
		return true;
	}
	return false;
}

bool FBranchiveSourceControlState::CanCheckout() const
{
	// Can acquire a lock if the file is known and not already locked by anyone.
	return WorkingCopyState != EBranchiveWorkingCopyState::Unknown
	    && !bLockedBySelf && !bLockedByOther;
}

bool FBranchiveSourceControlState::CanCheckIn() const
{
	if (bLockedByOther)
	{
		return false;
	}
	return bLockedBySelf
	    || WorkingCopyState == EBranchiveWorkingCopyState::Modified
	    || WorkingCopyState == EBranchiveWorkingCopyState::Added
	    || WorkingCopyState == EBranchiveWorkingCopyState::Deleted;
}

bool FBranchiveSourceControlState::CanDelete() const
{
	return IsSourceControlled() && !bLockedByOther;
}

bool FBranchiveSourceControlState::CanRevert() const
{
	return bLockedBySelf
	    || WorkingCopyState == EBranchiveWorkingCopyState::Modified
	    || WorkingCopyState == EBranchiveWorkingCopyState::Added
	    || WorkingCopyState == EBranchiveWorkingCopyState::Deleted
	    || WorkingCopyState == EBranchiveWorkingCopyState::Conflicted;
}

#if SOURCE_CONTROL_WITH_SLATE
FSlateIcon FBranchiveSourceControlState::GetIcon() const
{
	const FName StyleSet = FRevisionControlStyleManager::GetStyleSetName();

	if (WorkingCopyState == EBranchiveWorkingCopyState::Conflicted)
	{
		return FSlateIcon(StyleSet, "RevisionControl.Conflicted");
	}
	if (bLockedByOther)
	{
		return FSlateIcon(StyleSet, "RevisionControl.CheckedOutByOtherUser");
	}
	if (bLockedBySelf)
	{
		return FSlateIcon(StyleSet, "RevisionControl.CheckedOut");
	}
	switch (WorkingCopyState)
	{
	case EBranchiveWorkingCopyState::Added:
		return FSlateIcon(StyleSet, "RevisionControl.OpenForAdd");
	case EBranchiveWorkingCopyState::Deleted:
		return FSlateIcon(StyleSet, "RevisionControl.MarkedForDelete");
	default:
		break;
	}
	if (!bIsCurrent)
	{
		return FSlateIcon(StyleSet, "RevisionControl.NotAtHeadRevision");
	}
	return FSlateIcon();
}
#endif // SOURCE_CONTROL_WITH_SLATE

FText FBranchiveSourceControlState::GetDisplayName() const
{
	if (WorkingCopyState == EBranchiveWorkingCopyState::Conflicted)
	{
		return LOCTEXT("Conflicted", "Conflicted");
	}
	if (bLockedByOther)
	{
		FString Who;
		IsCheckedOutOther(&Who);
		return FText::Format(LOCTEXT("CheckedOutOther", "Locked by {0}"), FText::FromString(Who));
	}
	if (bLockedBySelf)
	{
		return LOCTEXT("CheckedOut", "Checked out");
	}
	switch (WorkingCopyState)
	{
	case EBranchiveWorkingCopyState::Added:         return LOCTEXT("Added", "Added");
	case EBranchiveWorkingCopyState::Deleted:       return LOCTEXT("Deleted", "Marked for delete");
	case EBranchiveWorkingCopyState::Modified:      return LOCTEXT("Modified", "Modified");
	case EBranchiveWorkingCopyState::NotControlled: return LOCTEXT("NotControlled", "Not under revision control");
	case EBranchiveWorkingCopyState::Unchanged:     return LOCTEXT("Unchanged", "Unchanged");
	case EBranchiveWorkingCopyState::Ignored:       return LOCTEXT("Ignored", "Ignored");
	default:                                        return LOCTEXT("Unknown", "Unknown");
	}
}

FText FBranchiveSourceControlState::GetDisplayTooltip() const
{
	if (bLockedByOther)
	{
		FString Who;
		IsCheckedOutOther(&Who);
		return FText::Format(LOCTEXT("CheckedOutOtherTip", "This file is locked by {0}. Coordinate before editing."), FText::FromString(Who));
	}
	if (!bIsCurrent)
	{
		return LOCTEXT("NotCurrentTip", "This file is not at the latest revision. Sync to update.");
	}
	return GetDisplayName();
}

#undef LOCTEXT_NAMESPACE
