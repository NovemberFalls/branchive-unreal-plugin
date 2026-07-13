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

	// ---- working-copy classification: Check-Out vs Mark-for-Add (§5.3) ------
	// The IA_Test in-editor smoke bug lived here: a brand-new asset that is absent
	// from `status --scan` AND not yet on disk MUST classify NotControlled (=> UE
	// Mark for Add => `stage`), NOT Unchanged (=> UE Check Out => `lock acquire` on a
	// path the repo has never seen => "That branch does not exist" => save aborts).
	{
		std::printf("classify: new/untracked vs tracked-clean vs unknown\n");

		// Mirror of the UE FBranchiveSourceControlState predicates so the gate asserts
		// the actual Check-Out-vs-Add contract, not just the raw enum. Kept in lockstep
		// with BranchiveSourceControlState.cpp (IsSourceControlled/CanCheckout/CanAdd).
		// Locks are absent in every case below, so CanCheckout == IsSourceControlled.
		auto UeIsSourceControlled = [](EWorkingClass c) { return c != EWorkingClass::NotControlled; };
		auto UeCanAdd            = [](EWorkingClass c) { return c == EWorkingClass::NotControlled; };
		auto UeCanCheckout       = [&](EWorkingClass c) { return UeIsSourceControlled(c); };

		// Fixture with an UNTRACKED file (third.txt, code 'A') + an unstaged edit (base.txt, 'M').
		FStatus su = ParseStatus(ReadFile(P("status/with-unstaged-changes.stdout.txt")));

		// (1) Untracked/'A' -> NotControlled -> Mark for Add (regardless of on-disk).
		EWorkingClass cUntracked = ClassifyWorkingCopy(su, "third.txt", /*onDisk*/true);
		Check(cUntracked == EWorkingClass::NotControlled, "untracked 'A' -> NotControlled");
		Check(!UeIsSourceControlled(cUntracked) && UeCanAdd(cUntracked) && !UeCanCheckout(cUntracked),
		      "untracked -> !IsSourceControlled, CanAdd, !CanCheckout");

		// (2) Tracked-clean (NOT in status, present on disk) -> Unchanged -> Check Out.
		EWorkingClass cClean = ClassifyWorkingCopy(su, "Content/IA_Primary.uasset", /*onDisk*/true);
		Check(cClean == EWorkingClass::Unchanged, "on-disk + not-in-status -> Unchanged (tracked-clean)");
		Check(UeIsSourceControlled(cClean) && !UeCanAdd(cClean) && UeCanCheckout(cClean),
		      "tracked-clean -> IsSourceControlled, !CanAdd, CanCheckout");

		// (3) Brand-new/unknown (NOT in status, NOT on disk) -> NotControlled -> Mark for Add.
		//     THIS is the IA_Test case: previously defaulted to Unchanged (the bug).
		EWorkingClass cNew = ClassifyWorkingCopy(su, "Content/IA_Test.uasset", /*onDisk*/false);
		Check(cNew == EWorkingClass::NotControlled, "not-on-disk + not-in-status -> NotControlled (new asset)");
		Check(!UeIsSourceControlled(cNew) && UeCanAdd(cNew) && !UeCanCheckout(cNew),
		      "new asset -> !IsSourceControlled, CanAdd, !CanCheckout (Mark for Add on save)");

		// (4) Unstaged modify -> Modified (source-controlled, not a new add).
		EWorkingClass cMod = ClassifyWorkingCopy(su, "base.txt", /*onDisk*/true);
		Check(cMod == EWorkingClass::Modified, "unstaged 'M' -> Modified");
		Check(UeIsSourceControlled(cMod) && !UeCanAdd(cMod), "modified -> IsSourceControlled, !CanAdd");

		// (5) Staged add -> Added (already in source control's added state, CanCheckIn).
		FStatus ss = ParseStatus(ReadFile(P("status/with-staged-changes.stdout.txt")));
		EWorkingClass cStagedAdd = ClassifyWorkingCopy(ss, "third.txt", /*onDisk*/true);
		Check(cStagedAdd == EWorkingClass::Added, "staged 'A' -> Added");
		Check(UeIsSourceControlled(cStagedAdd), "staged add -> IsSourceControlled");
		EWorkingClass cStagedMod = ClassifyWorkingCopy(ss, "base.txt", /*onDisk*/true);
		Check(cStagedMod == EWorkingClass::Modified, "staged 'M' -> Modified");

		// (6) Conflict wins over everything (highest priority section).
		FStatus sc = ParseStatus(ReadFile(P("status/with-conflict.stdout.txt")));
		EWorkingClass cConf = ClassifyWorkingCopy(sc, "base.txt", /*onDisk*/true);
		Check(cConf == EWorkingClass::Conflicted, "conflict -> Conflicted");

		// (7) Path matching is case-insensitive + slash-normalized (Windows paths).
		EWorkingClass cCase = ClassifyWorkingCopy(su, "THIRD.TXT", /*onDisk*/true);
		Check(cCase == EWorkingClass::NotControlled, "case-insensitive path match (untracked)");
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

	// ---- ambient identity: authUserInfo parse + skip-mint decision (BUG1) --
	{
		std::printf("ambient identity (authUserInfo) + skip-mint decision\n");

		// Flat shape: {"tagName":"authUserInfo","userId":"usr_...","email":"..."}
		const std::string flat =
			"{\"tagName\":\"lockFileQueryBegin\",\"data\":{\"count\":0}}\n"
			"{\"tagName\":\"authUserInfo\",\"userId\":\"usr_fbb8ccf7c60cc9cc20111338\",\"email\":\"lenboord@gmail.com\"}\n"
			"{\"tagName\":\"complete\",\"data\":{\"status\":0}}\n";
		FAuthUserInfo a = ParseAuthUserInfo(flat);
		Check(a.bFound, "authUserInfo found (flat)");
		Check(a.UserId == "usr_fbb8ccf7c60cc9cc20111338", "userId extracted (flat)");
		Check(a.Email == "lenboord@gmail.com", "email extracted (flat)");

		// Nested "data" shape — ExtractJsonString scans the whole line, so it still works.
		const std::string nested =
			"{\"tagName\":\"authUserInfo\",\"data\":{\"userId\":\"usr_9\",\"email\":\"User@Example.com\"}}\n";
		FAuthUserInfo b = ParseAuthUserInfo(nested);
		Check(b.bFound && b.Email == "User@Example.com", "authUserInfo found (nested data)");

		// The auth-less server fixture emits NO authUserInfo -> not found.
		FAuthUserInfo none = ParseAuthUserInfo(ReadFile(P("lock-query/json-with-locks.stdout.txt")));
		Check(!none.bFound, "auth-less lock-query has no authUserInfo (bFound=false)");

		// Decision (email-only back-compat overload): same identity (case-insensitive
		// email) -> SKIP the mint.
		Check(AmbientMatchesSignedIn(a, "lenboord@gmail.com"), "exact email match -> skip mint");
		Check(AmbientMatchesSignedIn(b, "user@example.com"), "case-insensitive email match -> skip mint");
		// Different identity, or missing data -> DO mint (fall through).
		Check(!AmbientMatchesSignedIn(a, "someone-else@gmail.com"), "different email -> do not skip");
		Check(!AmbientMatchesSignedIn(none, "lenboord@gmail.com"), "no ambient identity -> do not skip");
		Check(!AmbientMatchesSignedIn(a, ""), "empty signed-in email -> do not skip");
		FAuthUserInfo noEmail; noEmail.bFound = true; noEmail.UserId = "usr_x";
		Check(!AmbientMatchesSignedIn(noEmail, "lenboord@gmail.com"), "ambient without email -> do not skip");

		// ---- BUG1 (0.3.4): match by STABLE USER ID, email as fallback -------
		// This is the actual fix: the live hang was an EMAIL compare that failed
		// (the /auth/me identity may carry no email, or a different one) even though
		// the ambient CLI id and the signed-in id were the SAME "usr_..." token.

		// (a) Same id -> SKIP, even when the signed-in EMAIL is blank (the exact prod
		//     case that used to hang: @handle-only /auth/me, no email).
		Check(AmbientMatchesSignedIn(a, "usr_fbb8ccf7c60cc9cc20111338", ""),
		      "id==id with blank signed-in email -> skip mint (the fix)");

		// (b) Same id -> SKIP even if the emails DIFFER (id is authoritative).
		Check(AmbientMatchesSignedIn(a, "usr_fbb8ccf7c60cc9cc20111338", "stale-different@x.com"),
		      "id==id with differing email -> skip (id is primary)");

		// (c) Different id -> DO mint, even if the EMAILS happen to match (id wins).
		Check(!AmbientMatchesSignedIn(a, "usr_someone_else", "lenboord@gmail.com"),
		      "id!=id must NOT skip even when emails match (id is authoritative)");

		// (d) Id missing on the signed-in side -> fall back to email (still skips).
		Check(AmbientMatchesSignedIn(a, "", "LENBOORD@gmail.com"),
		      "no signed-in id -> email fallback (case-insensitive) -> skip");

		// (e) Id missing on the AMBIENT side -> fall back to email.
		FAuthUserInfo idlessAmbient; idlessAmbient.bFound = true; idlessAmbient.Email = "user@example.com";
		Check(AmbientMatchesSignedIn(idlessAmbient, "usr_signed_in", "user@example.com"),
		      "ambient has no id -> email fallback -> skip");
		Check(!AmbientMatchesSignedIn(idlessAmbient, "usr_signed_in", "other@example.com"),
		      "ambient has no id + email differs -> do not skip");

		// (f) Both keys empty / not found -> never skip.
		Check(!AmbientMatchesSignedIn(a, "", ""), "no id and no email -> do not skip");
		Check(!AmbientMatchesSignedIn(none, "usr_x", "user@example.com"),
		      "ambient not found -> do not skip regardless of signed-in keys");

		// ---- BUG2 (0.3.5): the RESTORE race — id-populated-before-decide ------
		// The v0.3.4 skip decision is correct, but on a REOPENED editor the signed-in
		// identity was still being restored ASYNCHRONOUSLY (RestoreOnStartup's /auth/me
		// on a detached thread) when the first op ran. With a PROD /auth/me that carries
		// only the @handle + the stable usr_... id and NO email, the state at decision
		// time was: SignedInUserId="" (not restored yet) AND SignedInEmail="". So the
		// SAME ambient identity produced a MINT (the hanging /auth/lore-token), every op.
		//
		// The fix (EnsureIdentityLoaded) does a synchronous /auth/me before the decision
		// so UserId is populated on the FIRST op. This models the exact before/after: the
		// SAME ambient identity `a` flips from MINT to SKIP once the id is loaded.
		const std::string RestoredUserId = "usr_fbb8ccf7c60cc9cc20111338"; // == a.UserId
		// BEFORE the fix (identity not yet restored: id AND email both blank) -> MINT.
		Check(!AmbientMatchesSignedIn(a, "", ""),
		      "restore race: id+email blank at decision time -> MINT (the 0.3.4 bug)");
		// AFTER the fix (EnsureIdentityLoaded populated UserId via /auth/me) -> SKIP,
		// even though the signed-in EMAIL is still blank (prod @handle-only identity).
		Check(AmbientMatchesSignedIn(a, RestoredUserId, ""),
		      "restore fixed: UserId populated (email still blank) -> SKIP (no /auth/lore-token)");
		// And the skip does NOT depend on email being present at all — id alone decides.
		Check(AmbientMatchesSignedIn(a, RestoredUserId, "anything@else.com"),
		      "restore fixed: id decides regardless of email -> SKIP");
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

	// ---- file history: multi-revision (newest-first, full-hex, null author) -
	{
		std::printf("file-history/multi-revision\n");
		std::vector<FFileRevisionEntry> H = ParseFileHistory(ReadFile(P("file-history/multi-revision.stdout.txt")));
		Check(H.size() == 3, "3 revisions parsed");
		if (H.size() == 3)
		{
			// Order is PRESERVED newest-first: 5, 4, 1.
			Check(H[0].Revision == 5 && H[1].Revision == 4 && H[2].Revision == 1, "order 5,4,1 (newest-first)");
			// Full 64-hex signatures (NOT truncated).
			Check(H[0].Signature == "3d66ac479f70415e58a5fdec00586253a6631e8e4b3ae81f6e02c6391d6cc086", "rev5 full-hex signature");
			Check(H[0].Signature.size() == 64, "signature is full 64-hex");
			Check(H[2].Signature == "ea7e12941c66872ff80ca235566516e379b72d8f815c429f7bf3a9099451e490", "rev1 full-hex signature");
			// Per-entry code captured (M/M/A) from the leading status line.
			Check(H[0].Code == 'M' && H[1].Code == 'M' && H[2].Code == 'A', "codes M,M,A");
			// Branch id hash (NOT a human name).
			Check(H[0].Branch == "e726318bbc3fd75ac8733a7e030cc35b", "rev5 branch id hash");
			// Date captured verbatim (RFC-2822-ish), NOT parsed here.
			Check(H[0].Date == "Sun, 28 Jun 2026 10:24:06 +0000", "rev5 date verbatim");
			// Multi-line message accumulated; blank indented line dropped.
			Check(H[0].Message.find("Fixes errors") != std::string::npos, "rev5 message has 'Fixes errors'");
			Check(H[2].Message == "Initial import of MP_CPP", "rev1 message exact");
			// NULL AUTHOR: the struct carries NO author field at all — there is
			// nothing to assert-equal because the contract forbids fabricating one.
			// (FFileRevisionEntry deliberately has no author/user member — §4.12 rule 5.)
			Check(H[0].MergeParents.empty(), "rev5 has no merge parents (non-merge)");
		}
	}

	// ---- file history: single-revision (one entry, code A, full-hex) --------
	{
		std::printf("file-history/single-revision\n");
		std::vector<FFileRevisionEntry> H = ParseFileHistory(ReadFile(P("file-history/single-revision.stdout.txt")));
		Check(H.size() == 1, "1 revision parsed");
		if (H.size() == 1)
		{
			Check(H[0].Revision == 1, "single rev number 1");
			Check(H[0].Code == 'A', "single rev code A (initial add)");
			Check(H[0].Signature.size() == 64, "single rev full-hex signature");
			Check(H[0].Message == "Initial import of MP_CPP", "single rev message exact");
		}
	}

	// ---- conflict argv: resolve mine/theirs + abort (§4.19 / §4.20) ---------
	{
		std::printf("conflict argv (resolve/abort)\n");
		// The absolute file path from the real conflict-resolve/mine fixture argv.
		const std::string AbsFile =
			"C:\\Users\\lenbo\\AppData\\Local\\Temp\\e2e-fixtures-8ee801b2\\ws-a\\base.txt";

		std::vector<std::string> Mine = BuildConflictResolveArgv(EConflictOp::Merge, EConflictSide::Mine, AbsFile);
		const std::vector<std::string> WantMine = { "branch", "merge", "resolve", "mine", AbsFile };
		Check(Mine == WantMine, "resolve mine argv == branch merge resolve mine <file>");

		std::vector<std::string> Theirs = BuildConflictResolveArgv(EConflictOp::Merge, EConflictSide::Theirs, AbsFile);
		const std::vector<std::string> WantTheirs = { "branch", "merge", "resolve", "theirs", AbsFile };
		Check(Theirs == WantTheirs, "resolve theirs argv == branch merge resolve theirs <file>");

		std::vector<std::string> Abort = BuildConflictAbortArgv(EConflictOp::Merge);
		const std::vector<std::string> WantAbort = { "branch", "merge", "abort" };
		Check(Abort == WantAbort, "abort argv == branch merge abort");

		// Cherry-pick / revert prefixes (contract completeness, §4.19).
		std::vector<std::string> Cp = BuildConflictResolveArgv(EConflictOp::CherryPick, EConflictSide::Mine, AbsFile);
		const std::vector<std::string> WantCp = { "revision", "cherry-pick", "resolve", "mine", AbsFile };
		Check(Cp == WantCp, "cherry-pick resolve prefix");
	}

	// ---- diff parse + historical reconstruction (§4.11 / §4.13) -------------
	{
		std::printf("file-diff/reconstructed-historical-content\n");
		std::vector<FDiffFile> Files = ParseUnifiedDiff(ReadFile(P("file-diff/reconstructed-historical-content.stdout.txt")));
		Check(Files.size() == 1, "one diff file entry");
		if (Files.size() == 1)
		{
			Check(!Files[0].bBinary, "text file (not binary)");
			Check(Files[0].Path == "base.txt", "diff path base.txt (@1 decoration stripped)");
			Check(Files[0].Hunks.size() == 1, "one hunk");
			if (Files[0].Hunks.size() == 1)
			{
				const FDiffHunk& Hh = Files[0].Hunks[0];
				Check(Hh.NewStart == 1, "hunk newStart 1");
				Check(Hh.NewCount == 1, "hunk newCount 1 (one add)");
				Check(Hh.OldLines.size() == 1 && Hh.OldLines[0] == "base file v1", "old side == 'base file v1'");

				// Reverse-apply to the WORKING content (the '+' / new side) to get the
				// historical (source@1) content back.
				std::vector<std::string> Working = { "base file - main branch DIFFERENT edit" };
				std::vector<std::string> Old = ReconstructOldContent(Working, Files[0].Hunks);
				Check(Old.size() == 1 && Old[0] == "base file v1", "reconstructed old content == 'base file v1'");
			}
		}

		// Binary diff -> synthesized binary entry, no hunks (§4.11 rule 5).
		std::vector<FDiffFile> Bin = ParseUnifiedDiff(ReadFile(P("diff/binary-file.stdout.txt")));
		bool bFoundBinary = false;
		for (const FDiffFile& F : Bin) { if (F.bBinary) { bFoundBinary = true; } }
		Check(bFoundBinary, "binary-file diff yields a binary entry");
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
