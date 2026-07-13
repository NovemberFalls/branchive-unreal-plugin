// Copyright Branchive.
#include "BranchiveSourceControlOperations.h"

#include "BranchiveSourceControlCommand.h"
#include "BranchiveSourceControlConflictOperations.h"
#include "BranchiveSourceControlLog.h"
#include "BranchiveSourceControlModule.h"
#include "BranchiveSourceControlProvider.h"
#include "BranchiveSourceControlRevision.h"
#include "Lore/LoreCli.h"
#include "Lore/LoreParse.h"
#include "Lore/LoreErrors.h"

#include "SourceControlOperations.h"
#include "HAL/PlatformProcess.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "BranchiveSourceControl.Ops"

// ---------------------------------------------------------------- std <-> FString
namespace
{
	FString ToF(const std::string& S)
	{
		return FString(UTF8_TO_TCHAR(S.c_str()));
	}

	std::string ToU8(const FString& S)
	{
		return std::string(TCHAR_TO_UTF8(*S));
	}

	FString NormalizeRel(const FString& In)
	{
		FString R = In;
		R.ReplaceInline(TEXT("\\"), TEXT("/"));
		return R.ToLower();
	}

	FString MakeRepoRelative(const FString& AbsFile, const FString& RepoRoot)
	{
		FString Rel = FPaths::ConvertRelativePathToFull(AbsFile);
		FString Base = RepoRoot;
		if (!Base.EndsWith(TEXT("/")))
		{
			Base += TEXT("/");
		}
		FPaths::MakePathRelativeTo(Rel, *Base);
		return Rel;
	}

	EBranchiveWorkingCopyState::Type CodeToState(char Code)
	{
		switch (Code)
		{
		case 'A': return EBranchiveWorkingCopyState::Added;
		case 'D': return EBranchiveWorkingCopyState::Deleted;
		case 'M': return EBranchiveWorkingCopyState::Modified;
		default:  return EBranchiveWorkingCopyState::Modified;
		}
	}

	// Map a file-history status code to a human action string for UE's column.
	FString CodeToAction(char Code)
	{
		switch (Code)
		{
		case 'A': return TEXT("add");
		case 'M': return TEXT("edit");
		case 'D': return TEXT("delete");
		case 'R': return TEXT("rename");
		case 'C': return TEXT("copy");
		default:  return FString();
		}
	}

	// Parse Lore's RFC-2822-ish date ("Sun, 28 Jun 2026 10:24:06 +0000"). The
	// offset is treated as UTC (fixtures are all +0000). Returns FDateTime(0) on
	// failure — never throws.
	FDateTime ParseLoreDate(const FString& In)
	{
		FString S = In.TrimStartAndEnd();
		int32 Comma;
		if (S.FindChar(TEXT(','), Comma)) { S = S.Mid(Comma + 1).TrimStartAndEnd(); }

		TArray<FString> Tok;
		S.ParseIntoArray(Tok, TEXT(" "), /*CullEmpty=*/true);
		if (Tok.Num() < 4) { return FDateTime(0); }

		const int32 Day = FCString::Atoi(*Tok[0]);
		static const TCHAR* Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"),
			TEXT("May"), TEXT("Jun"), TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"),
			TEXT("Nov"), TEXT("Dec") };
		int32 Month = 0;
		for (int32 i = 0; i < 12; ++i) { if (Tok[1].StartsWith(Months[i])) { Month = i + 1; break; } }
		const int32 Year = FCString::Atoi(*Tok[2]);

		int32 Hour = 0, Min = 0, Sec = 0;
		if (Tok.Num() >= 4)
		{
			TArray<FString> HMS;
			Tok[3].ParseIntoArray(HMS, TEXT(":"), true);
			if (HMS.Num() >= 1) { Hour = FCString::Atoi(*HMS[0]); }
			if (HMS.Num() >= 2) { Min  = FCString::Atoi(*HMS[1]); }
			if (HMS.Num() >= 3) { Sec  = FCString::Atoi(*HMS[2]); }
		}

		// Validate BEFORE constructing — FDateTime's (Y,M,D,...) ctor check()s its
		// args and would assert on malformed input.
		if (!FDateTime::Validate(Year, Month, Day, Hour, Min, Sec, 0))
		{
			return FDateTime(0);
		}
		return FDateTime(Year, Month, Day, Hour, Min, Sec);
	}

	// Fallback mutex so a worker never dereferences a null WorkspaceMutex.
	static FCriticalSection GFallbackWorkspaceMutex;

	FCriticalSection* MutexFor(FBranchiveSourceControlCommand& Command)
	{
		return Command.WorkspaceMutex ? Command.WorkspaceMutex : &GFallbackWorkspaceMutex;
	}

	// Retry an op that can hit the transient "Store overloaded" R2 rate limit
	// (contract §7 runWithStoreBackoff): 6 attempts, 1500ms base, 30s cap.
	FLoreCliResult RunWithStoreBackoff(const FLoreCli& Cli, const TArray<FString>& Args)
	{
		BranchiveLore::FBackoff B;
		FLoreCliResult Res;
		int32 Delay = B.BaseDelayMs;
		for (int32 Attempt = 0; Attempt < B.MaxAttempts; ++Attempt)
		{
			Res = Cli.Run(Args);
			if (Res.Ok())
			{
				return Res;
			}
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
			if (Ec != BranchiveLore::EErrorClass::StoreOverloaded)
			{
				return Res;
			}
			UE_LOG(LogBranchiveSourceControl, Log, TEXT("Store overloaded; retrying push (attempt %d)."), Attempt + 1);
			FPlatformProcess::Sleep(FMath::Min(Delay, B.CapMs) / 1000.0f);
			Delay = FMath::Min(Delay * 2, B.CapMs);
		}
		return Res;
	}

	// Run `status --scan` + `lock query --json --branch <b>` and resolve the
	// per-file state for `Files`. Returns false if status itself failed.
	bool ComputeStates(const FLoreCli& Cli, const FString& RepoRoot, const TArray<FString>& Files,
	                   const TSet<FString>& SessionLocked,
	                   TArray<FBranchiveResolvedFileState>& OutStates, FString& OutBranch,
	                   bool& OutIsCurrent, FString& OutError)
	{
		const FLoreCliResult StatusRes = Cli.Run({ TEXT("status"), TEXT("--scan") });
		if (!StatusRes.Ok())
		{
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				StatusRes.ReturnCode, StatusRes.bSpawnFailed, ToU8(StatusRes.StdErr), ToU8(StatusRes.StdOut));
			OutError = ToF(BranchiveLore::FriendlyMessage(Ec));
			return false;
		}

		const BranchiveLore::FStatus St = BranchiveLore::ParseStatus(ToU8(StatusRes.StdOut));
		OutBranch = ToF(St.Branch);
		OutIsCurrent = !(St.SyncState == BranchiveLore::ESyncState::Behind ||
		                 St.SyncState == BranchiveLore::ESyncState::Diverged);

		// Lock query, scoped to the human branch name (contract §4.17 Rule 1).
		std::vector<BranchiveLore::FLock> Locks;
		{
			TArray<FString> LockArgs = { TEXT("lock"), TEXT("query"), TEXT("--json") };
			if (!OutBranch.IsEmpty())
			{
				LockArgs.Add(TEXT("--branch"));
				LockArgs.Add(OutBranch);
			}
			const FLoreCliResult LockRes = Cli.Run(LockArgs);
			// exit 0 even with the benign trailing stray events (contract §4.17 Rule 2).
			if (!LockRes.bSpawnFailed && LockRes.ReturnCode == 0)
			{
				Locks = BranchiveLore::ParseLocksJson(ToU8(LockRes.StdOut));
			}
		}

		// Build lookup maps keyed by normalized repo-relative path.
		TMap<FString, EBranchiveWorkingCopyState::Type> StateByRel;
		auto AddEntries = [&](const std::vector<BranchiveLore::FStatusEntry>& V, bool bConflicted, bool bUntracked)
		{
			for (const BranchiveLore::FStatusEntry& E : V)
			{
				const FString Rel = NormalizeRel(ToF(E.Path));
				EBranchiveWorkingCopyState::Type S = bConflicted ? EBranchiveWorkingCopyState::Conflicted
					: bUntracked ? EBranchiveWorkingCopyState::NotControlled
					: CodeToState(E.Code);
				StateByRel.Add(Rel, S); // later (higher-priority) adds overwrite
			}
		};
		// Priority low -> high (later overwrites): untracked < unstaged < staged < conflicts.
		AddEntries(St.Untracked, /*conflict*/false, /*untracked*/true);
		AddEntries(St.Unstaged,  false, false);
		AddEntries(St.Staged,    false, false);
		AddEntries(St.Conflicts, true,  false);

		TMap<FString, FString> LockOwnerByRel;
		for (const BranchiveLore::FLock& L : Locks)
		{
			LockOwnerByRel.Add(NormalizeRel(ToF(L.Path)), ToF(L.Owner));
		}

		for (const FString& Abs : Files)
		{
			FBranchiveResolvedFileState RS;
			RS.AbsFilename = Abs;
			RS.bIsCurrent = OutIsCurrent;

			const FString Rel = NormalizeRel(MakeRepoRelative(Abs, RepoRoot));
			if (const EBranchiveWorkingCopyState::Type* Found = StateByRel.Find(Rel))
			{
				RS.State = *Found;
			}
			else
			{
				RS.State = EBranchiveWorkingCopyState::Unchanged;
			}

			if (const FString* Owner = LockOwnerByRel.Find(Rel))
			{
				if (SessionLocked.Contains(Abs))
				{
					RS.bLockedBySelf = true;
				}
				else
				{
					RS.bLockedByOther = true;
					RS.LockOwner = *Owner;
				}
			}
			OutStates.Add(RS);
		}
		return true;
	}

	// Run `lore file history <absFile>` for each file and build newest-first
	// FBranchiveSourceControlRevision lists (contract §4.12). A per-file failure is
	// non-fatal: that file simply gets no history (never a fabricated entry).
	void ComputeHistory(const FLoreCli& Cli, const TArray<FString>& Files,
	                    TMap<FString, TArray<FBranchiveSourceControlRevisionRef>>& OutHistory)
	{
		for (const FString& Abs : Files)
		{
			const FLoreCliResult Res = Cli.Run({ TEXT("file"), TEXT("history"), Abs });
			if (!Res.Ok())
			{
				continue; // e.g. file never tracked — no history, no error banner
			}

			const std::vector<BranchiveLore::FFileRevisionEntry> Entries =
				BranchiveLore::ParseFileHistory(ToU8(Res.StdOut));

			TArray<FBranchiveSourceControlRevisionRef> Revs;
			Revs.Reserve((int32)Entries.size());
			for (const BranchiveLore::FFileRevisionEntry& E : Entries)
			{
				TSharedRef<FBranchiveSourceControlRevision, ESPMode::ThreadSafe> Rev =
					MakeShared<FBranchiveSourceControlRevision, ESPMode::ThreadSafe>();
				Rev->Filename = Abs;
				Rev->RevisionNumber = (int32)E.Revision;
				Rev->FullSignature = ToF(E.Signature);
				// Short label = revision number (stable, human-facing). The full hex
				// is preserved on FullSignature for `file diff --source`.
				Rev->ShortRevision = FString::Printf(TEXT("%lld"), E.Revision);
				Rev->Description = ToF(E.Message);
				Rev->UserName = FString();   // NEVER fabricate an author (contract §4.12 rule 5)
				Rev->ClientSpec = FString();
				Rev->Action = CodeToAction(E.Code);
				Rev->Date = ParseLoreDate(ToF(E.Date));
				Rev->PathToLoreBinary = Cli.GetBinaryPath();
				Rev->PathToRepositoryRoot = Cli.GetRepoPath();
				Revs.Add(Rev);
			}
			OutHistory.Add(Abs, MoveTemp(Revs));
		}
	}
}

// ----------------------------------------------------------- base UpdateStates
bool FBranchiveSourceControlWorkerBase::UpdateStates() const
{
	FBranchiveSourceControlModule* Module = FBranchiveSourceControlModule::GetThreadSafe();
	if (!Module)
	{
		return false;
	}
	FBranchiveSourceControlProvider& Provider = Module->GetProvider();

	if (!OutBranch.IsEmpty())
	{
		Provider.SetBranchName(OutBranch);
	}
	Provider.SetIsCurrent(bOutIsCurrent);

	for (const FString& F : AcquiredLocks)
	{
		Provider.AddSessionLock(F);
	}
	for (const FString& F : ReleasedLocks)
	{
		Provider.RemoveSessionLock(F);
	}

	for (const FBranchiveResolvedFileState& RS : States)
	{
		TSharedRef<FBranchiveSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(RS.AbsFilename);
		State->WorkingCopyState = RS.State;
		State->bLockedBySelf = RS.bLockedBySelf;
		State->bLockedByOther = RS.bLockedByOther;
		State->LockOwner = RS.LockOwner;
		State->bIsCurrent = RS.bIsCurrent;
		State->TimeStamp = FDateTime::Now();
	}

	// Apply any freshly-fetched history (contract §4.12).
	for (const TPair<FString, TArray<FBranchiveSourceControlRevisionRef>>& Pair : HistoryByFile)
	{
		TSharedRef<FBranchiveSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(Pair.Key);
		State->History = Pair.Value;
		State->TimeStamp = FDateTime::Now();
	}

	return States.Num() > 0 || AcquiredLocks.Num() > 0 || ReleasedLocks.Num() > 0 || HistoryByFile.Num() > 0;
}

// ------------------------------------------------------------------ Connect
bool FBranchiveConnectWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// Verify the workspace responds. A raw TCP pre-flight to the lore:// port
	// (serverPing, contract §7) is a documented follow-up; v1 proves reachability
	// by a real status call and classifies the failure honestly.
	FString Error;
	const bool bOk = ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
		Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
	if (!bOk)
	{
		Command.ErrorMessages.Add(Error);
	}
	return bOk;
}

// ------------------------------------------------------------- UpdateStatus
bool FBranchiveUpdateStatusWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	FString Error;
	const bool bOk = ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
		Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
	if (!bOk)
	{
		Command.ErrorMessages.Add(Error);
	}

	// History-capable path: when the editor requests history (Content Browser ->
	// Revision Control -> History, which runs FUpdateStatus with SetUpdateHistory
	// (true)), populate each file's revision list from `lore file history` (§4.12).
	if (bOk && Command.Operation->GetName() == "UpdateStatus")
	{
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Op = StaticCastSharedRef<FUpdateStatus>(Command.Operation);
		if (Op->ShouldUpdateHistory())
		{
			ComputeHistory(Cli, Command.Files, HistoryByFile);
		}
	}
	return bOk;
}

// ---------------------------------------------------------------- CheckOut
bool FBranchiveCheckOutWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	if (Command.Files.Num() == 0)
	{
		return true; // nothing to check out
	}
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// CheckOut == lock acquire (contract §5.3 / §4.15).
	TArray<FString> Args = { TEXT("lock"), TEXT("acquire") };
	Args.Append(Command.Files);
	const FLoreCliResult Res = Cli.Run(Args);
	if (!Res.Ok())
	{
		const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
			Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
		Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
		return false;
	}

	AcquiredLocks = Command.Files;

	// Recompute states treating the just-acquired files as our own locks.
	TSet<FString> Augmented = Command.SessionLockedFiles;
	Augmented.Append(TSet<FString>(Command.Files));
	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files, Augmented,
		States, OutBranch, bOutIsCurrent, Error);
	return true;
}

// ------------------------------------------------------------------ CheckIn
bool FBranchiveCheckInWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// ---- 1. stage the exact files being checked in (explicit list, §5.3) ----
	{
		TArray<FString> StageArgs = { TEXT("stage") };
		if (Command.Files.Num() > 0)
		{
			StageArgs.Append(Command.Files);
		}
		else
		{
			StageArgs.Add(TEXT("."));
			StageArgs.Add(TEXT("--scan"));
		}
		const FLoreCliResult StageRes = Cli.Run(StageArgs);
		if (!StageRes.Ok())
		{
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				StageRes.ReturnCode, StageRes.bSpawnFailed, ToU8(StageRes.StdErr), ToU8(StageRes.StdOut));
			Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
			return false; // leave lock state intact (contract §5.3)
		}
	}

	// ---- 2. commit (message is POSITIONAL; --non-interactive, §4.4) ---------
	FString Description = TEXT("Checked in from Unreal Editor");
	{
		TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckInOp = StaticCastSharedRef<FCheckIn>(Command.Operation);
		const FString Desc = CheckInOp->GetDescription().ToString().TrimStartAndEnd();
		if (!Desc.IsEmpty())
		{
			Description = Desc;
		}
	}
	{
		const FLoreCliResult CommitRes = Cli.Run({ TEXT("commit"), Description, TEXT("--non-interactive") });
		if (!CommitRes.Ok())
		{
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				CommitRes.ReturnCode, CommitRes.bSpawnFailed, ToU8(CommitRes.StdErr), ToU8(CommitRes.StdOut));
			if (Ec == BranchiveLore::EErrorClass::EmptyCommit)
			{
				// Benign no-op: nothing actually changed. NOT a CheckIn failure
				// (contract §5.3). Do not push or release locks — nothing landed.
				Command.InfoMessages.Add(TEXT("Nothing to check in."));
				FString Error;
				ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
					Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
				return true;
			}
			Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
			return false; // leave lock state intact
		}
	}

	// ---- 3. push (recommended/progress-capable form, §4.5) ------------------
	{
		TArray<FString> PushArgs = { TEXT("branch"), TEXT("push") };
		if (!Command.BranchName.IsEmpty())
		{
			PushArgs.Add(Command.BranchName);
		}
		PushArgs.Add(TEXT("-P"));
		PushArgs.Add(TEXT("--log-level"));
		PushArgs.Add(TEXT("debug"));

		const FLoreCliResult PushRes = RunWithStoreBackoff(Cli, PushArgs);
		if (!PushRes.Ok())
		{
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				PushRes.ReturnCode, PushRes.bSpawnFailed, ToU8(PushRes.StdErr), ToU8(PushRes.StdOut));
			// Diverged (§7): map to UE Sync ("someone pushed first — sync then retry").
			// Foreign lock / protected / other: friendly message. DO NOT release
			// locks (the change did not land) — the user keeps their lock (§5.3).
			Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
			return false;
		}
	}

	// ---- 4. release locks on EXACTLY the files just committed (§5.3) --------
	// Only files we actually hold (intersection with our session locks) —
	// never blanket-release the user's other, unrelated checkouts.
	{
		TArray<FString> ToRelease;
		for (const FString& F : Command.Files)
		{
			if (Command.SessionLockedFiles.Contains(F))
			{
				ToRelease.Add(F);
			}
		}
		if (ToRelease.Num() > 0)
		{
			TArray<FString> ReleaseArgs = { TEXT("lock"), TEXT("release") };
			ReleaseArgs.Append(ToRelease);
			const FLoreCliResult ReleaseRes = Cli.Run(ReleaseArgs);
			if (ReleaseRes.Ok())
			{
				ReleasedLocks = ToRelease;
			}
			else
			{
				// The commit+push landed; a failed release is a warning, not a
				// check-in failure. The next status poll reconciles lock state.
				Command.InfoMessages.Add(TEXT("Checked in, but releasing the lock reported an error; it will reconcile on the next refresh."));
			}
		}
	}

	// Recompute states, treating released files as no longer self-locked.
	TSet<FString> Remaining = Command.SessionLockedFiles;
	for (const FString& F : ReleasedLocks)
	{
		Remaining.Remove(F);
	}
	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files, Remaining,
		States, OutBranch, bOutIsCurrent, Error);
	return true;
}

// --------------------------------------------------------------- MarkForAdd
bool FBranchiveMarkForAddWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	if (Command.Files.Num() == 0)
	{
		return true;
	}
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// There is no `lore add`; `stage <file>` marks a new file for add (§4.2).
	TArray<FString> Args = { TEXT("stage") };
	Args.Append(Command.Files);
	const FLoreCliResult Res = Cli.Run(Args);
	if (!Res.Ok())
	{
		const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
			Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
		Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
		return false;
	}

	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
		Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
	return true;
}

// ------------------------------------------------------------------- Delete
bool FBranchiveDeleteWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	if (Command.Files.Num() == 0)
	{
		return true;
	}
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// The editor deletes the file on disk first; staging it records the delete.
	TArray<FString> Args = { TEXT("stage") };
	Args.Append(Command.Files);
	const FLoreCliResult Res = Cli.Run(Args);
	if (!Res.Ok())
	{
		const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
			Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
		Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
		return false;
	}

	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
		Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
	return true;
}

// ------------------------------------------------------------------- Revert
bool FBranchiveRevertWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	if (Command.Files.Num() == 0)
	{
		return true;
	}
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// UE Revert = discard local edits = FILE-LEVEL `lore file reset` (NOT
	// `revision revert`) — contract §4.14 / §5.3.
	TArray<FString> Args = { TEXT("file"), TEXT("reset") };
	Args.Append(Command.Files);
	const FLoreCliResult Res = Cli.Run(Args);
	if (!Res.Ok())
	{
		const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
			Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
		Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
		return false;
	}

	// Discarding a checkout releases the lock (Perforce/UE parity). Best-effort,
	// scoped to our own session locks only. (Contract §5.3 specifies reset for
	// the discard; releasing our own lock keeps UE's checkout UI consistent.)
	TArray<FString> ToRelease;
	for (const FString& F : Command.Files)
	{
		if (Command.SessionLockedFiles.Contains(F))
		{
			ToRelease.Add(F);
		}
	}
	if (ToRelease.Num() > 0)
	{
		TArray<FString> ReleaseArgs = { TEXT("lock"), TEXT("release") };
		ReleaseArgs.Append(ToRelease);
		const FLoreCliResult ReleaseRes = Cli.Run(ReleaseArgs);
		if (ReleaseRes.Ok())
		{
			ReleasedLocks = ToRelease;
		}
	}

	TSet<FString> Remaining = Command.SessionLockedFiles;
	for (const FString& F : ReleasedLocks)
	{
		Remaining.Remove(F);
	}
	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files, Remaining,
		States, OutBranch, bOutIsCurrent, Error);
	return true;
}

// --------------------------------------------------------------------- Sync
bool FBranchiveSyncWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// UE Sync = pull = bare `lore sync` (no revision argument) — contract §5.3 / §4.6.
	const FLoreCliResult Res = Cli.Run({ TEXT("sync") });
	if (!Res.Ok())
	{
		const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
			Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
		Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
		return false;
	}

	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
		Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
	return true;
}

// ------------------------------------------------- Resolve (ours / theirs)
bool FBranchiveResolveWorkerBase::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	if (Command.Files.Num() == 0)
	{
		return true; // nothing selected to resolve
	}
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// Whole-side-per-file, Perforce-style (contract §4.19). This plugin only ever
	// produces a MERGE conflict (Sync -> pending merge), so the op is Merge.
	const BranchiveLore::EConflictSide SideEnum = ResolveUsingTheirs()
		? BranchiveLore::EConflictSide::Theirs
		: BranchiveLore::EConflictSide::Mine;

	bool bAllOk = true;
	int32 ResolvedCount = 0;
	for (const FString& File : Command.Files)
	{
		const std::vector<std::string> Argv =
			BranchiveLore::BuildConflictResolveArgv(BranchiveLore::EConflictOp::Merge, SideEnum, ToU8(File));
		TArray<FString> Args;
		Args.Reserve((int32)Argv.size());
		for (const std::string& A : Argv) { Args.Add(ToF(A)); }

		// Success == exit 0 with EMPTY stdout AND stderr (contract §4.19 live finding).
		const FLoreCliResult Res = Cli.Run(Args);
		if (!Res.Ok())
		{
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
			Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
			bAllOk = false;
			continue; // try to resolve as many of the selected files as possible
		}
		++ResolvedCount;
	}

	if (ResolvedCount > 0)
	{
		Command.InfoMessages.Add(TEXT("Resolved conflict(s) — check in to complete the merge."));
	}

	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
		Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
	return bAllOk;
}

// ------------------------------------------------------------ Abort merge
bool FBranchiveAbortMergeWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// `lore branch merge abort` — reverts the working tree to its pre-merge state
	// (contract §4.20). No files argument.
	const std::vector<std::string> Argv =
		BranchiveLore::BuildConflictAbortArgv(BranchiveLore::EConflictOp::Merge);
	TArray<FString> Args;
	Args.Reserve((int32)Argv.size());
	for (const std::string& A : Argv) { Args.Add(ToF(A)); }

	const FLoreCliResult Res = Cli.Run(Args);
	if (!Res.Ok())
	{
		const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
			Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
		Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
		return false;
	}

	Command.InfoMessages.Add(TEXT("Merge aborted — the working tree was reverted to its pre-merge state."));

	// Re-scan every cached file: the conflict is gone and content reverted.
	TArray<FString> Refresh = Command.Files;
	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Refresh,
		Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
	return true;
}

#undef LOCTEXT_NAMESPACE
