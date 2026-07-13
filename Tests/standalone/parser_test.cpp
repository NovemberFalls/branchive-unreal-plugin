// Copyright Branchive.
//
// Standalone fixture test for the engine-independent Lore CLI parsers. Compiles
// WITHOUT Unreal Engine, against the exact same LoreParse.cpp / LoreErrors.cpp
// translation units the UE module builds, and runs them over the REAL golden
// fixtures under integrations/contract/fixtures/ (contract v2.0.0 §8).
//
// Build+run (from anywhere):
//   integrations/unreal/Tests/standalone/run.sh        (g++/clang, Git Bash)
//   integrations/unreal/Tests/standalone/run.ps1        (MSVC cl.exe)
//
// argv[1] = absolute path to integrations/contract/fixtures
#include "LoreParse.h"
#include "LoreErrors.h"
#include "LoreAuthSeam.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool cond, const std::string& what)
{
	if (cond) { ++g_pass; }
	else { ++g_fail; std::printf("  FAIL: %s\n", what.c_str()); }
}

static std::string ReadFile(const std::string& path)
{
	std::ifstream f(path.c_str(), std::ios::binary);
	if (!f) { std::printf("  (could not open %s)\n", path.c_str()); return std::string(); }
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: parser_test <fixtures-dir>\n");
		return 2;
	}
	const std::string Fix = argv[1];
	auto P = [&](const char* rel) { return Fix + "/" + rel; };

	using namespace BranchiveLore;

	// ---- status: staged (LOAD-BEARING trailing-space trim, §4.1 step 6a) ----
	{
		std::printf("status/with-staged-changes\n");
		FStatus s = ParseStatus(ReadFile(P("status/with-staged-changes.stdout.txt")));
		Check(s.Branch == "main", "staged.branch==main");
		Check(s.Revision == 1, "staged.revision==1");
		Check(s.bHasRemoteRevision && s.RemoteRevision == 0, "staged.remoteRev==0");
		Check(s.SyncState == ESyncState::Ahead, "staged.syncState==ahead");
		Check(s.Staged.size() == 2, "staged has 2 rows");
		if (s.Staged.size() == 2)
		{
			// The fixture rows carry a literal trailing space; the parser MUST trim it.
			Check(s.Staged[0].Path == "base.txt", "staged[0]=='base.txt' (no trailing space)");
			Check(s.Staged[1].Path == "third.txt", "staged[1]=='third.txt' (no trailing space)");
			Check(s.Staged[0].Code == 'M' && s.Staged[1].Code == 'A', "staged codes M,A");
		}
		Check(!s.InConflict(), "staged not in conflict");
	}

	// ---- status: unstaged + untracked (untracked code is 'A', not '?') ------
	{
		std::printf("status/with-unstaged-changes\n");
		FStatus s = ParseStatus(ReadFile(P("status/with-unstaged-changes.stdout.txt")));
		Check(s.Unstaged.size() == 1 && s.Unstaged[0].Path == "base.txt", "unstaged base.txt");
		Check(s.Unstaged.size() == 1 && s.Unstaged[0].Code == 'M', "unstaged code M");
		Check(s.Untracked.size() == 1 && s.Untracked[0].Path == "third.txt", "untracked third.txt");
		Check(s.Untracked.size() == 1 && s.Untracked[0].Code == 'A', "untracked code is 'A'");
	}

	// ---- status: conflict (exit-0 conflict; suffix strip; footer coexists) --
	{
		std::printf("status/with-conflict\n");
		FStatus s = ParseStatus(ReadFile(P("status/with-conflict.stdout.txt")));
		Check(s.InConflict(), "conflict detected");
		Check(s.Conflicts.size() == 1 && s.Conflicts[0].Path == "base.txt", "conflict base.txt (suffix stripped)");
		Check(s.bPendingMerge, "pending merge flagged");
	}

	// ---- status: clean (no remote line -> syncState forced Unknown) ---------
	{
		std::printf("status/clean-workspace\n");
		FStatus s = ParseStatus(ReadFile(P("status/clean-workspace.stdout.txt")));
		Check(s.Branch == "main" && s.Revision == 7, "clean branch/rev");
		Check(!s.bHasRemoteRevision, "clean has no remote revision");
		Check(s.SyncState == ESyncState::Unknown, "clean syncState==unknown (no remote)");
		Check(s.Staged.empty() && s.Unstaged.empty() && s.Untracked.empty() && s.Conflicts.empty(), "clean has no rows");
	}

	// ---- status: remote in-sync -------------------------------------------
	{
		std::printf("status/with-remote-sync-state\n");
		FStatus s = ParseStatus(ReadFile(P("status/with-remote-sync-state.stdout.txt")));
		Check(s.bHasRemoteRevision && s.RemoteRevision == 8, "sync remoteRev==8");
		Check(s.SyncState == ESyncState::InSync, "syncState==in-sync");
	}

	// ---- locks: with a lock (owner sentinel; ignore stray events) ----------
	{
		std::printf("lock-query/json-with-locks\n");
		std::vector<FLock> locks = ParseLocksJson(ReadFile(P("lock-query/json-with-locks.stdout.txt")));
		Check(locks.size() == 1, "exactly 1 lock (Begin + stray complete/log ignored)");
		if (locks.size() == 1)
		{
			Check(locks[0].Path == "base.txt", "lock path base.txt");
			Check(locks[0].Owner == "<unknown>", "lock owner is the <unknown> sentinel");
			Check(locks[0].OwnerIsUnknown(), "OwnerIsUnknown() true");
			Check(locks[0].Branch == "e726318bbc3fd75ac8733a7e030cc35b", "lock branch is the BranchId hash");
		}
	}

	// ---- locks: empty ------------------------------------------------------
	{
		std::printf("lock-query/json-scoped-empty\n");
		std::vector<FLock> locks = ParseLocksJson(ReadFile(P("lock-query/json-scoped-empty.stdout.txt")));
		Check(locks.empty(), "no locks in empty query");
	}

	// ---- error taxonomy (§7) ----------------------------------------------
	{
		std::printf("error taxonomy\n");
		std::string s;
		s = ReadFile(P("commit/nothing-to-commit-error.stderr.txt"));
		Check(ClassifyError(21, false, s, "") == EErrorClass::EmptyCommit, "exit21 -> EmptyCommit");

		s = ReadFile(P("push/diverged-rejected.stderr.txt"));
		Check(ClassifyError(255, false, s, "") == EErrorClass::DivergedBranch, "exit255 -> DivergedBranch");

		s = ReadFile(P("status/not-a-workspace.stderr.txt"));
		Check(ClassifyError(45, false, s, "") == EErrorClass::NotAWorkspace, "exit45 -> NotAWorkspace");

		// exit 0 is always None (unless a merge-conflict marker, checked separately).
		Check(ClassifyError(0, false, "whatever", "") == EErrorClass::None, "exit0 -> None");

		// Spawn failure classifies as BinaryNotFound regardless of text.
		Check(ClassifyError(-1, true, "", "") == EErrorClass::BinaryNotFound, "spawnFail -> BinaryNotFound");
	}

	// ---- merge-conflict marker (exit 0 + "Files in conflict:") -------------
	{
		std::printf("merge-conflict marker\n");
		const std::string combined = "Merged files, 0 updated, 0 deleted, 0 merged, 1 conflicted\nFiles in conflict:\nbase.txt\n";
		Check(HasMergeConflict(combined), "HasMergeConflict true");
		std::vector<std::string> cf;
		ParseConflictedFiles(combined, cf);
		Check(cf.size() == 1 && cf[0] == "base.txt", "conflicted file parsed");
	}

	// ---- SECURITY: isCloudRemoteUrl exact-host gate (auth §4.0) ------------
	{
		std::printf("isCloudRemoteUrl gate\n");
		const std::string host = "vcs.branchive.io";
		Check(IsCloudRemoteUrl("lore://vcs.branchive.io:41337", host), "cloud host with port -> true");
		Check(IsCloudRemoteUrl("lore://vcs.branchive.io", host), "cloud host no port -> true");
		Check(IsCloudRemoteUrl("LORE://VCS.BRANCHIVE.IO:41337", host), "case-insensitive -> true");
		Check(!IsCloudRemoteUrl("lore://evil.com:41337", host), "evil host -> false");
		Check(!IsCloudRemoteUrl("lore://vcs.branchive.io.evil.com", host), "suffix-append host -> false");
		Check(!IsCloudRemoteUrl("lore://notvcs.branchive.io", host), "prefix host -> false");
		Check(!IsCloudRemoteUrl("", host), "empty -> false");
		Check(!IsCloudRemoteUrl("https://vcs.branchive.io", host), "wrong scheme -> false");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
