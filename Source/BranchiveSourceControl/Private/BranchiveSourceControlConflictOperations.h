// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Branchive-specific source-control operations for conflict resolution.
//
// WHY CUSTOM OPS: Lore's conflict model is Perforce-style WHOLE-SIDE-per-file —
// keep MINE (ours / current branch) or THEIRS (incoming) — NOT an arbitrary
// 3-way hand-merge (contract §4.19; UE assets are binary anyway, so a text
// merge UI would be meaningless). UE5's stock FResolve operation carries no
// mine/theirs selector, so we expose the choice as three explicit, clearly-named
// operations a host menu (or an automated flow) can dispatch:
//
//   FBranchiveResolveMine   -> lore branch merge resolve mine   <absFile>   (§4.19)
//   FBranchiveResolveTheirs -> lore branch merge resolve theirs <absFile>   (§4.19)
//   FBranchiveAbortMerge    -> lore branch merge abort                      (§4.20)
//
// Completing the merge after every conflicted file is resolved is just an
// ordinary CheckIn/commit (contract §4.21) — no dedicated op needed.
//
// This plugin only ever PRODUCES a merge conflict (Sync -> pending merge), so
// the argv prefix is always `branch merge`. The cherry-pick / revert prefixes
// exist in the engine-independent core (LoreParse) for contract completeness,
// but are not reachable from this plugin's own operations.
#pragma once

#include "CoreMinimal.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperationBase.h"

#define LOCTEXT_NAMESPACE "BranchiveSourceControl.ConflictOps"

/** Keep the CURRENT-branch (ours) side of each conflicted file. */
class FBranchiveResolveMine : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "BranchiveResolveMine"; }
	virtual FText GetInProgressString() const override
	{
		return LOCTEXT("Branchive_ResolveMine", "Resolving conflict(s) using Mine (Ours)...");
	}
};

/** Keep the INCOMING (theirs) side of each conflicted file. */
class FBranchiveResolveTheirs : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "BranchiveResolveTheirs"; }
	virtual FText GetInProgressString() const override
	{
		return LOCTEXT("Branchive_ResolveTheirs", "Resolving conflict(s) using Theirs (Incoming)...");
	}
};

/** Abort the in-progress merge, reverting the working tree to its pre-merge state. */
class FBranchiveAbortMerge : public FSourceControlOperationBase
{
public:
	virtual FName GetName() const override { return "BranchiveAbortMerge"; }
	virtual FText GetInProgressString() const override
	{
		return LOCTEXT("Branchive_AbortMerge", "Aborting merge...");
	}
};

#undef LOCTEXT_NAMESPACE
