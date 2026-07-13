// Copyright Branchive.
//
// FBranchiveSourceControlRevision — one entry in a file's revision history
// (contract §4.12). Populates the Content Browser's "History" panel and drives
// "Diff Against Previous Revision" / "Diff Against Depot" by materializing a
// historical revision's bytes to a temp file (Get(), contract §4.13).
//
// HONEST LIMITATIONS (documented, not fabricated):
//   * There is NO author/identity anywhere in `file history` output (contract
//     §4.12 rule 5). GetUserName() therefore ALWAYS returns "" — never a guess.
//   * There is no "cat file at revision" verb; historical content is
//     reconstructed by reverse-applying `lore file diff --source <sig>` hunks to
//     the working tree (contract §4.13). That path only works for TEXT files —
//     a binary asset (.uasset/.umap, or any file with NUL bytes / >2 MB) cannot
//     be reconstructed and Get() returns false (an honest "unavailable"), rather
//     than silently handing back the CURRENT bytes as if they were historical.
#pragma once

#include "CoreMinimal.h"
#include "ISourceControlRevision.h"

class FBranchiveSourceControlRevision : public ISourceControlRevision
{
public:
	// ---- ISourceControlRevision -------------------------------------------
	virtual bool Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const override;
	virtual bool GetAnnotated(TArray<FAnnotationLine>& OutLines) const override;
	virtual bool GetAnnotated(FString& InOutFilename) const override;
	virtual const FString& GetFilename() const override { return Filename; }
	virtual int32 GetRevisionNumber() const override { return RevisionNumber; }
	virtual const FString& GetRevision() const override { return ShortRevision; }
	virtual const FString& GetDescription() const override { return Description; }
	virtual const FString& GetUserName() const override { return UserName; }        // ALWAYS "" (no author, §4.12)
	virtual const FString& GetClientSpec() const override { return ClientSpec; }    // ""
	virtual const FString& GetAction() const override { return Action; }
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetBranchSource() const override { return nullptr; }
	virtual const FDateTime& GetDate() const override { return Date; }
	virtual int32 GetCheckInIdentifier() const override { return RevisionNumber; }
	virtual int32 GetFileSize() const override { return FileSize; }

public:
	/** Absolute path of the asset this revision belongs to. */
	FString Filename;

	/** Revision NUMBER (Lore's `Revision : N`). */
	int32 RevisionNumber = 0;

	/** Full hex signature — the real check-in identifier used for `file diff --source`. */
	FString FullSignature;

	/** Short, human-facing revision label shown in the History column. */
	FString ShortRevision;

	/** Commit description (never carries an author). */
	FString Description;

	/** ALWAYS empty — contract §4.12 rule 5: no author exists; do not fabricate. */
	FString UserName;

	/** ALWAYS empty (no clientspec concept). */
	FString ClientSpec;

	/** Human action ("add"/"edit"/"delete"), derived from the per-entry code. May be "". */
	FString Action;

	/** Revision date. */
	FDateTime Date = FDateTime(0);

	/** Best-effort file size at this revision (0 = unknown; we do not have per-revision size). */
	int32 FileSize = 0;

	/** CLI coordinates used by Get() to reconstruct historical content. */
	FString PathToLoreBinary;
	FString PathToRepositoryRoot;
};

typedef TSharedRef<FBranchiveSourceControlRevision, ESPMode::ThreadSafe> FBranchiveSourceControlRevisionRef;
