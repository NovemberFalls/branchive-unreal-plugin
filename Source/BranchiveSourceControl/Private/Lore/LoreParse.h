// Copyright Branchive. Licensed for the Branchive Unreal source control plugin.
//
// Engine-independent parsers for the `lore` CLI's textual output.
//
// This file deliberately depends on NOTHING from Unreal Engine (no FString, no
// CoreMinimal). Everything here is plain C++/std so that:
//   1. the exact same translation unit compiles inside the UE module, AND
//   2. it can be compiled + run in a tiny standalone harness against the golden
//      fixtures under integrations/contract/fixtures/ (see Tests/standalone/).
//
// Parse rules are a direct port of INTEGRATIONS-CONTRACT.md v2.0.0:
//   - ParseStatus  -> §4.1 (parseStatus)     incl. the load-bearing §4.1/6a trim.
//   - ParseLocksJson -> §4.17 (parseLocksJson) incl. the "<unknown>" sentinel and
//                       the benign trailing "No auth endpoint available" event.
//
// Keep this file free of `<regex>` (heavy compile) and of any identifier that
// UE turns into a macro (TEXT, check, PI, min/max, TRUE/FALSE...) so it stays
// safe inside a unity build.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace BranchiveLore
{
	// --- status -------------------------------------------------------------

	enum class ESection
	{
		None,
		Staged,
		Unstaged,
		Untracked,
		Conflicts
	};

	enum class ESyncState
	{
		Unknown,
		InSync,
		Ahead,
		Behind,
		Diverged
	};

	struct FStatusEntry
	{
		// Single-letter code as printed by the CLI: 'A' added, 'M' modified,
		// 'D' deleted, 'R' rename, 'C' copy. NOTE: an untracked file is reported
		// with 'A', not '?' (contract §4.1) — the SECTION, not the code, is what
		// distinguishes "untracked" from "staged add".
		char Code = '?';
		// Repo-relative path exactly as the CLI prints it, unconditionally trimmed
		// (contract §4.1 step 6a — every staged row carries a trailing space).
		std::string Path;
	};

	struct FStatus
	{
		std::string Branch;                 // human branch name, e.g. "main"
		long long   Revision = -1;          // -1 = not seen
		std::string Head;                   // local head signature (hex)

		bool        bHasRemoteRevision = false;
		long long   RemoteRevision = -1;
		std::string RemoteHead;

		ESyncState  SyncState = ESyncState::Unknown;

		bool        bPendingMerge = false;
		std::string PendingMergeRevision;   // incoming revision signature

		std::vector<FStatusEntry> Staged;
		std::vector<FStatusEntry> Unstaged;
		std::vector<FStatusEntry> Untracked;
		std::vector<FStatusEntry> Conflicts;

		bool InConflict() const { return !Conflicts.empty(); }
	};

	// Parse `lore status --scan` stdout. Never throws.
	FStatus ParseStatus(const std::string& StdoutText);

	// --- locks --------------------------------------------------------------

	// The literal sentinel the CLI emits for `owner` on an auth-less server
	// (contract §4.17). Never a real user id, never absent.
	extern const char* const UnknownOwnerSentinel; // "<unknown>"

	struct FLock
	{
		std::string Path;   // repo-relative path
		std::string Owner;  // raw JWT sub, OR the UnknownOwnerSentinel
		std::string Branch; // internal BranchId hash (NOT the human name)

		bool OwnerIsUnknown() const { return Owner == UnknownOwnerSentinel; }
	};

	// Parse `lore lock query --json ...` NDJSON stdout. Only "lockFileQuery"
	// events are emitted; every other line (including the benign second
	// "complete"/"log" event stream, contract §4.17 rule 2) is ignored. Never
	// throws — a line that fails to parse is silently skipped.
	std::vector<FLock> ParseLocksJson(const std::string& StdoutText);

	// --- small shared string helpers (exposed for tests) --------------------
	std::string Trim(const std::string& S);
	bool        StartsWithNoCase(const std::string& S, const char* Prefix);
	void        SplitLines(const std::string& Text, std::vector<std::string>& OutLines);
}
