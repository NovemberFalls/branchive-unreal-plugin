// Copyright Branchive.
//
// In-editor automation test for the engine-independent Lore parsers. Runs the
// SAME LoreParse/LoreErrors code the provider uses, over byte-faithful copies of
// the golden fixtures (contract v2.0.0 §8). The exhaustive fixture coverage lives
// in the standalone harness (Tests/standalone/parser_test.cpp), which reads the
// real fixture files; this test gives an in-engine smoke check runnable via the
// Automation window ("Branchive.Parser.*") or `-ExecCmds="Automation RunTests Branchive"`.
#include "Misc/AutomationTest.h"
#include "Lore/LoreParse.h"
#include "Lore/LoreErrors.h"
#include "Lore/LoreAuthSeam.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBranchiveStatusParserTest,
	"Branchive.Parser.Status", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBranchiveStatusParserTest::RunTest(const FString&)
{
	using namespace BranchiveLore;

	// Byte-faithful copy of fixtures/status/with-staged-changes.stdout.txt.
	const std::string Staged =
		"Repository 019f594c768c73f3afd94604c36b6742\n"
		"On branch main revision 1 -> 83d7fce768c39418ad38786a4d4bb08746d5c83a4e5acbdd7cd200f61cf17b40\n"
		"Remote revision 0 -> 0000000000000000000000000000000000000000000000000000000000000000\n"
		"Local branch is ahead of remote\n"
		"Changes staged for commit:\n"
		"M base.txt \n"   // NOTE: trailing space is intentional (contract §4.1 step 6a)
		"A third.txt \n"
		"Tracked changes: 1 added, 1 modified\n";

	FStatus S = ParseStatus(Staged);
	TestEqual(TEXT("branch"), FString(S.Branch.c_str()), FString(TEXT("main")));
	TestTrue(TEXT("syncState==ahead"), S.SyncState == ESyncState::Ahead);
	TestEqual(TEXT("staged count"), (int32)S.Staged.size(), 2);
	if (S.Staged.size() == 2)
	{
		// The load-bearing trim: no trailing space survives.
		TestEqual(TEXT("staged[0] trimmed"), FString(S.Staged[0].Path.c_str()), FString(TEXT("base.txt")));
		TestEqual(TEXT("staged[1] trimmed"), FString(S.Staged[1].Path.c_str()), FString(TEXT("third.txt")));
	}

	// Conflict fixture: exit-0 conflict, suffix stripped, footer coexists.
	const std::string Conflict =
		"Repository 019f594c768c73f3afd94604c36b6742\n"
		"On branch main revision 6 -> edb502a652f8b55baa5a5617dc6dbc4fd372f9471560558724f9eb5c584423b6\n"
		"Remote revision 0 -> 0000000000000000000000000000000000000000000000000000000000000000\n"
		"Local branch is ahead of remote\n"
		"Pending merge, incoming revision f2b326cfdda10bd9c6a1dc98df4267298e8b23db9beea92638ca82edfa56f07a\n"
		"Changes in conflict:\n"
		"M  base.txt (M)!\n"
		"No tracked changes\n";
	FStatus C = ParseStatus(Conflict);
	TestTrue(TEXT("in conflict"), C.InConflict());
	TestTrue(TEXT("pending merge"), C.bPendingMerge);
	if (C.Conflicts.size() == 1)
	{
		TestEqual(TEXT("conflict path stripped"), FString(C.Conflicts[0].Path.c_str()), FString(TEXT("base.txt")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBranchiveLockParserTest,
	"Branchive.Parser.Locks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBranchiveLockParserTest::RunTest(const FString&)
{
	using namespace BranchiveLore;

	// Byte-faithful copy of fixtures/lock-query/json-with-locks.stdout.txt.
	const std::string Ndjson =
		"{\"tagName\":\"lockFileQueryBegin\",\"data\":{\"count\":1}}\n"
		"{\"tagName\":\"lockFileQuery\",\"data\":{\"branch\":\"e726318bbc3fd75ac8733a7e030cc35b\",\"path\":\"base.txt\",\"owner\":\"<unknown>\",\"lockedAt\":1783909622321}}\n"
		"{\"tagName\":\"complete\",\"data\":{\"status\":0,\"error\":{\"errorCode\":0,\"message\":\"\",\"traceLocations\":[]}}}\n"
		"{\"tagName\":\"log\",\"data\":{\"level\":\"error\",\"message\":\"No auth endpoint available\"}}\n"
		"{\"tagName\":\"complete\",\"data\":{\"status\":-1,\"error\":{\"errorCode\":-1,\"message\":\"No auth endpoint available\"}}}\n";

	std::vector<FLock> Locks = ParseLocksJson(Ndjson);
	TestEqual(TEXT("one lock (stray events ignored)"), (int32)Locks.size(), 1);
	if (Locks.size() == 1)
	{
		TestEqual(TEXT("path"), FString(Locks[0].Path.c_str()), FString(TEXT("base.txt")));
		TestEqual(TEXT("owner sentinel"), FString(Locks[0].Owner.c_str()), FString(TEXT("<unknown>")));
		TestTrue(TEXT("OwnerIsUnknown"), Locks[0].OwnerIsUnknown());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBranchiveErrorTaxonomyTest,
	"Branchive.Parser.Errors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBranchiveErrorTaxonomyTest::RunTest(const FString&)
{
	using namespace BranchiveLore;
	TestTrue(TEXT("empty commit"),   ClassifyError(21, false, "[Error] Nothing staged for commit", "") == EErrorClass::EmptyCommit);
	TestTrue(TEXT("diverged"),       ClassifyError(255, false, "[Error] Branch has diverged, sync to merge remote changes", "") == EErrorClass::DivergedBranch);
	TestTrue(TEXT("not a workspace"),ClassifyError(45, false, "[Error] Repository not found: X\n  at lore\\src\\call.rs:237:19", "") == EErrorClass::NotAWorkspace);
	TestTrue(TEXT("foreign lock"),   ClassifyError(1, false, "Push rejected: 'a.uasset' is locked by 'user-123'", "") == EErrorClass::ForeignLock);

	// Security gate.
	const std::string Host = "vcs.branchive.io";
	TestTrue(TEXT("cloud host"),  IsCloudRemoteUrl("lore://vcs.branchive.io:41337", Host));
	TestTrue(TEXT("evil host"), !IsCloudRemoteUrl("lore://vcs.branchive.io.evil.com", Host));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
