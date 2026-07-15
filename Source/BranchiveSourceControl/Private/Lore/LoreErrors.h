// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Engine-independent classification of `lore` CLI failures, per
// INTEGRATIONS-CONTRACT.md v2.0.0 §7 (error taxonomy) and §3.6 (why exit code
// alone is NOT a reliable discriminator — always match stderr text).
//
// Pure C++/std, no UE dependency (see LoreParse.h header for why).
#pragma once

#include <string>
#include <vector>

namespace BranchiveLore
{
	enum class EErrorClass
	{
		None,                          // exit 0, no failure
		BinaryNotFound,                // spawn ENOENT — set by the caller, not by text
		NotAWorkspace,                 // exit 45, "Repository not found" / call.rs
		HistoricalContentUnavailable,  // exit 2,  "Address not found"
		CliArgumentError,              // exit 2,  clap "error: ..." (a plugin bug)
		BranchNotFound,                // exit 13, "[Error] Not found"
		AuthMissing,                   // "no token stored" / not connected + token
		NoAuthEndpoint,                // benign "No auth endpoint available" (server config)
		NoRemoteConfigured,            // exit 14, "No remote configured"
		ForeignLock,                   // "is locked by" / "failed to release the lock"
		DivergedBranch,                // exit 255, "Branch has diverged"
		EmptyCommit,                   // exit 21, "Nothing staged for commit"
		NotAuthorized,                 // "not authorized to access repository" (maybe protected)
		StoreOverloaded,               // "slow down" / "store overloaded" (transient)
		Unknown                        // non-zero, unclassified
	};

	// Classify a failed CLI invocation. `bSpawnFailed` is the ENOENT/binary-not-
	// found signal the process layer sets (never inferable from text).
	// A successful op (ExitCode == 0) returns EErrorClass::None UNLESS a merge
	// conflict is present — callers must additionally consult HasMergeConflict()
	// for the exit-0-but-conflicted case (contract §4.10/§7).
	EErrorClass ClassifyError(int ExitCode, bool bSpawnFailed,
	                          const std::string& StdErr, const std::string& StdOut);

	// A merge/cherry-pick/revert conflict is exit 0 + "Files in conflict:" in the
	// combined stdout+stderr (contract §4.10 parseMergeConflicts). This is a
	// SUCCESSFUL outcome that needs follow-up, never an error banner.
	bool HasMergeConflict(const std::string& CombinedOutput);

	// Extract the conflicted relative paths that follow a "Files in conflict:"
	// header (one per line until the next blank line).
	void ParseConflictedFiles(const std::string& CombinedOutput, std::vector<std::string>& Out);

	// A short, user-facing sentence for a class. Never the raw Rust-internals
	// stderr string (contract §7 "never show this to the user").
	std::string FriendlyMessage(EErrorClass Class);

	// Transient store-overload retry defaults (contract §7 runWithStoreBackoff).
	struct FBackoff
	{
		int   MaxAttempts = 6;
		int   BaseDelayMs = 1500;
		int   CapMs       = 30000;
	};
}
