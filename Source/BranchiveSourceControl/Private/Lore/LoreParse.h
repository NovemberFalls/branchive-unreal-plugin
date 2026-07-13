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

	// --- ambient CLI identity (BUG1: skip the redundant token mint) ---------

	// The CLI's ambient authenticated identity, as carried by the "authUserInfo"
	// NDJSON event an AUTHENTICATED cloud op (e.g. `lore lock query --json`) emits.
	struct FAuthUserInfo
	{
		bool        bFound = false; // an "authUserInfo" event was present
		std::string UserId;         // e.g. "usr_fbb8ccf7c60cc9cc20111338"
		std::string Email;          // e.g. "user@example.com"
	};

	// Locate the first "authUserInfo" NDJSON event and extract its userId/email.
	// Tolerant of field nesting (flat OR under a "data" object) — it only requires
	// that "userId"/"email" appear as string values on that line. bFound=false when
	// the CLI is not cloud-authenticated (an auth-less server emits no such event).
	// Never throws.
	FAuthUserInfo ParseAuthUserInfo(const std::string& StdoutText);

	// The "skip the /auth/lore-token mint" decision (BUG1): true iff the CLI's
	// ambient identity is present AND its email matches the signed-in user's email
	// (case-insensitive). Empty on either side => false (fall through to the mint).
	// Pure + engine-independent so the decision is unit-testable without a live BFF.
	bool AmbientMatchesSignedIn(const FAuthUserInfo& Ambient, const std::string& SignedInEmail);

	// --- file history (§4.12) ----------------------------------------------

	struct FFileRevisionEntry
	{
		// Revision NUMBER as printed (`Revision  : 5`). -1 = not seen.
		long long   Revision = -1;
		// Full hex signature — this IS the changelist/check-in identifier UE keys on.
		std::string Signature;
		// Content address (`<hex>-<hex>`), used only for diagnostics.
		std::string Address;
		// Internal BranchId hash (NOT the human branch name).
		std::string Branch;
		// Raw date string, e.g. "Sun, 28 Jun 2026 10:24:06 +0000" (RFC-2822-ish).
		std::string Date;
		// Commit description (possibly multi-line, joined with '\n'). NEVER carries
		// an author — contract §4.12 rule 5 (there is no author field anywhere in
		// this output). Consumers MUST NOT fabricate one.
		std::string Message;
		// Per-entry status code that precedes the block ('A' add, 'M' edit, 'D'
		// delete, ...). May be '\0' if the CLI didn't print one.
		char        Code = '\0';
		// Foreign merge-parent signatures (0, 1 or 2). Empty for a non-merge revision.
		std::vector<std::string> MergeParents;
	};

	// Parse `lore file history <absFile>` stdout. Newest-first order is PRESERVED
	// (the parser does not reorder). Never throws. There is deliberately NO author
	// field — see FFileRevisionEntry::Message.
	std::vector<FFileRevisionEntry> ParseFileHistory(const std::string& StdoutText);

	// --- unified diff (§4.11) + historical reconstruction (§4.13) ----------

	struct FDiffHunk
	{
		long long                OldStart = 0;   // 1-based start on the OLD (source) side
		long long                NewStart = 0;   // 1-based start on the NEW (working) side
		// OLD-side lines = context + deletions (marker char stripped), in order.
		std::vector<std::string> OldLines;
		// How many lines this hunk occupies on the NEW/working side (context + adds).
		long long                NewCount = 0;
	};

	struct FDiffFile
	{
		std::string             Path;            // NEW-side path (OLD-side if new is /dev/null)
		bool                    bBinary = false; // "Binary files differ"
		std::vector<FDiffHunk>  Hunks;
	};

	// Parse a standard unified diff (as `lore diff` / `lore file diff` print).
	// Direct port of the reference parseDiff() (contract §4.11). Never throws.
	std::vector<FDiffFile> ParseUnifiedDiff(const std::string& StdoutText);

	// Reverse-apply a (working-tree-vs-<source>) diff's hunks to the CURRENT
	// working-tree lines to reconstruct the <source> revision's content
	// (contract §4.13 step 5): hunks are applied LAST-TO-FIRST by NewStart so
	// earlier splices don't shift later line numbers; each hunk replaces its
	// NewCount working-side lines with its OldLines. Zero hunks => unchanged =>
	// the working lines are returned verbatim. Never throws.
	std::vector<std::string> ReconstructOldContent(const std::vector<std::string>& WorkingLines,
	                                               const std::vector<FDiffHunk>& Hunks);

	// --- conflict resolve/abort argv (§4.19 / §4.20) -----------------------

	// Which in-progress operation a conflict belongs to. This plugin only ever
	// PRODUCES a merge conflict (via Sync -> pending merge), so Merge is the
	// default; the other prefixes exist for contract-faithful argv coverage.
	enum class EConflictOp { Merge, CherryPick, Revert };

	// Whole-side-per-file resolution — Perforce-style ours/theirs, NOT a 3-way
	// hand-merge (Lore's model; UE assets are binary anyway).
	enum class EConflictSide { Mine, Theirs };

	// Build the resolve argv (WITHOUT the trailing "--repository <path>", which the
	// process layer appends): e.g. {"branch","merge","resolve","mine","<absFile>"}.
	std::vector<std::string> BuildConflictResolveArgv(EConflictOp Op, EConflictSide Side,
	                                                  const std::string& AbsFile);

	// Build the abort argv (WITHOUT "--repository"): e.g. {"branch","merge","abort"}.
	std::vector<std::string> BuildConflictAbortArgv(EConflictOp Op);

	// --- small shared string helpers (exposed for tests) --------------------
	std::string Trim(const std::string& S);
	bool        StartsWithNoCase(const std::string& S, const char* Prefix);
	void        SplitLines(const std::string& Text, std::vector<std::string>& OutLines);
}
