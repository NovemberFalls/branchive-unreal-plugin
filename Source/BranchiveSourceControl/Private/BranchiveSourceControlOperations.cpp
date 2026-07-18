// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "BranchiveSourceControlOperations.h"

#include "BranchiveSourceControlCommand.h"
#include "BranchiveSourceControlConflictOperations.h"
#include "BranchiveSourceControlLog.h"
#include "BranchiveSourceControlModule.h"
#include "BranchiveSourceControlProvider.h"
#include "BranchiveSourceControlRevision.h"
#include "Cloud/BranchiveCloudAuth.h"
#include "Lore/LoreCli.h"
#include "Lore/LoreParse.h"
#include "Lore/LoreErrors.h"

#include "SourceControlOperations.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/EngineVersionComparison.h"
#include "Async/Async.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "AssetRegistry/AssetRegistryModule.h"

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

	// §4: before a remote-dialing op, refresh the Lore JWT for THIS op's remote — but
	// ONLY behind the §4.0 exact-host cloud gate (inside EnsureCloudAuth). A no-op
	// unless the user is signed in to Branchive Cloud AND this workspace's remote is
	// the cloud host; a self-hosted / local / hostile remote gets a hard no-op, and a
	// signed-out user falls back to the CLI's ambient auth. Best-effort — never breaks
	// the op. Runs on the worker thread, serialized by the workspace mutex the caller holds.
	void MaybeEnsureCloudAuth(FBranchiveSourceControlCommand& Command)
	{
		if (FBranchiveCloudAuth* Cloud = FBranchiveCloudAuth::Get())
		{
			Cloud->EnsureCloudAuth(Command.RemoteUrl, Command.PathToRepositoryRoot);
		}
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

	// Map the engine-independent classification (LoreParse) onto the UE state enum.
	// The classification decision itself lives in BranchiveLore::ClassifyWorkingCopy
	// (single source of truth, unit-tested standalone) — this is a pure 1:1 relabel.
	EBranchiveWorkingCopyState::Type MapClass(BranchiveLore::EWorkingClass C)
	{
		switch (C)
		{
		case BranchiveLore::EWorkingClass::Unchanged:     return EBranchiveWorkingCopyState::Unchanged;
		case BranchiveLore::EWorkingClass::Added:         return EBranchiveWorkingCopyState::Added;
		case BranchiveLore::EWorkingClass::Deleted:       return EBranchiveWorkingCopyState::Deleted;
		case BranchiveLore::EWorkingClass::Modified:      return EBranchiveWorkingCopyState::Modified;
		case BranchiveLore::EWorkingClass::Conflicted:    return EBranchiveWorkingCopyState::Conflicted;
		case BranchiveLore::EWorkingClass::NotControlled: return EBranchiveWorkingCopyState::NotControlled;
		default:                                          return EBranchiveWorkingCopyState::Unknown;
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
		// The CLI's OWN identity, surfaced by the same `lock query --json` stream as an
		// "authUserInfo" event (v0.3.9): lets own-vs-other resolve by SERVER TRUTH
		// (lock.owner == my usr_ id) instead of the per-session memory that made a
		// user's own leftover lock from a previous editor session read as someone
		// else's ("GA_Primary keeps locking on us", live 2026-07-18). Empty on
		// auth-less servers — the session-set fallback below still covers those.
		FString SelfUserId;
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
				const BranchiveLore::FAuthUserInfo Self = BranchiveLore::ParseAuthUserInfo(ToU8(LockRes.StdOut));
				if (Self.bFound)
				{
					SelfUserId = ToF(Self.UserId);
				}
			}
		}

		// Lock owner lookup, keyed by normalized repo-relative path.
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

			// Single source of truth for Check-Out-vs-Mark-for-Add (ClassifyWorkingCopy).
			// The on-disk probe is what keeps a brand-new asset (absent from `status
			// --scan` AND not yet written) OUT of the "controlled" bucket, so UE marks it
			// for add instead of trying to `lock acquire` a nonexistent branch (the
			// IA_Test smoke). Cost: one stat per file NOT already listed as a pending
			// change — negligible next to the `status --scan` that just walked the whole
			// working tree, and far cheaper than a per-file `file history` probe (which we
			// deliberately do NOT run during a bulk UpdateStatus over 10k+ assets).
			const bool bOnDisk = FPaths::FileExists(Abs);
			RS.State = MapClass(BranchiveLore::ClassifyWorkingCopy(St, ToU8(Rel), bOnDisk));

			if (const FString* Owner = LockOwnerByRel.Find(Rel))
			{
				// Own-vs-other by SERVER TRUTH first (owner id == my usr_ id from the
				// same query's authUserInfo), session memory second (auth-less servers
				// surface no identity — the pre-0.3.9 behavior is the fallback there).
				// This is what makes a leftover own lock from a PREVIOUS editor session
				// finally read as "checked out by me" instead of a foreign lock.
				const bool bOwnByServer = !SelfUserId.IsEmpty() && *Owner == SelfUserId;
				if (bOwnByServer || SessionLocked.Contains(Abs))
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

	// --------------------------------------------------------- editor-view reconcile
	// After the Delete worker removes a working-copy .uasset from disk (the physical
	// removal that lets `lore stage` record the deletion — the SAME thing the bundled
	// providers do: FGitDeleteWorker runs `git rm`, FPerforceDeleteWorker runs
	// `p4 delete`, both of which remove the working file), reconcile UE's in-editor
	// view so the deleted asset can neither GHOST in the Content Browser nor RESURRECT
	// on a subsequent "Save All".
	//
	// Why this is needed (verified against UE 5.6 engine source — NOT guessed):
	//   * A Content-Browser delete runs ObjectTools::DeleteObjectsInternal, which FIRST
	//     tells the AssetRegistry the package is gone (FAssetRegistryModule::
	//     PackageDeleted) and UNLOADS the package + GCs, and only THEN calls the
	//     provider's FDelete worker. For a source-controlled file it does NOT delete the
	//     file itself — it batches the path and expects the provider to remove it (which
	//     is why removing the file in the worker is correct and matches Git/Perforce).
	//     In the common case the package is already gone from memory here, so the loop
	//     below finds nothing and does nothing.
	//   * BUT if the package cannot be unloaded because it is still referenced in memory
	//     (very common for an Input Action referenced by a loaded Input Mapping Context /
	//     Enhanced-Input project settings / the open level), UE re-marks the SURVIVING
	//     package PKG_NewlyCreated the instant it sees the file gone (ObjectTools.cpp,
	//     the "mark newly created" pass immediately after FDelete). That surviving
	//     newly-created package IS the ghost the user saw, and "Save All" then writes it
	//     back to disk — so the delete "won't stick". (This UE behaviour is provider-
	//     agnostic; Git/Perforce would hit it too. v0.3.6 removed the file exactly like
	//     them, but never reconciled UE's view for this un-unloadable case.)
	//
	// We NEVER touch UObjects from the worker thread. The reconcile is queued to the
	// game thread and runs AFTER DeleteObjectsInternal returns (its FDelete call and the
	// re-mark pass are one synchronous stack), so it reliably reverses UE's re-mark.
	void ReconcileEditorViewAfterDelete(const TArray<FString>& AbsFiles)
	{
		// Convert filenames -> long package names here (pure string work, thread-safe);
		// only the UObject/AssetRegistry touches are deferred to the game thread.
		TArray<FString> PackageNames;
		PackageNames.Reserve(AbsFiles.Num());
		for (const FString& Abs : AbsFiles)
		{
			FString PackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(Abs, PackageName) && !PackageName.IsEmpty())
			{
				PackageNames.Add(MoveTemp(PackageName));
			}
		}
		if (PackageNames.Num() == 0)
		{
			return;
		}

		AsyncTask(ENamedThreads::GameThread, [PackageNames = MoveTemp(PackageNames)]()
		{
			FAssetRegistryModule* RegistryModule =
				FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));

			for (const FString& PackageName : PackageNames)
			{
				UPackage* Package = FindPackage(nullptr, *PackageName);
				if (Package == nullptr)
				{
					continue; // Unloaded cleanly (the common case) — nothing to reconcile.
				}

				// Drop each surviving asset from the registry so it stops surfacing as a
				// ghost in the Content Browser (ObjectTools already called PackageDeleted
				// before FDelete, but the PKG_NewlyCreated re-mark re-surfaced it).
				if (RegistryModule != nullptr)
				{
					TArray<UObject*> ObjectsInPackage;
					// UE 5.8 deprecated the bool `bIncludeNestedObjects` overload in favour
					// of EGetObjectsFlags (Rocket promotes the deprecation to an error).
					// false maps to EGetObjectsFlags::None. The enum overload does not exist
					// pre-5.8, so keep the bool call there.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
					GetObjectsWithPackage(Package, ObjectsInPackage, /*bIncludeNestedObjects=*/false);
#else
					GetObjectsWithPackage(Package, ObjectsInPackage, EGetObjectsFlags::None);
#endif
					for (UObject* Obj : ObjectsInPackage)
					{
						if (Obj != nullptr && Obj->IsAsset())
						{
							RegistryModule->Get().AssetDeleted(Obj);
						}
					}
				}

				// Reverse UE's "newly created" re-mark and clear dirty so "Save All"
				// skips the package instead of re-writing the file we just deleted.
				Package->ClearPackageFlags(PKG_NewlyCreated);
				Package->SetDirtyFlag(false);
			}
		});
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
	MaybeEnsureCloudAuth(Command);
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
	MaybeEnsureCloudAuth(Command);
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// BUG3 — classify first. CheckOut == lock acquire (§5.3 / §4.15), but a lock
	// only applies to a TRACKED file. A brand-new/untracked asset (e.g. the IA_Test
	// that produced "Unable to Check Out ... That branch does not exist" in the
	// smoke) must be Mark-for-Add'ed instead — `lock acquire` on a file the repo
	// doesn't know returns a confusing "not found". One status scan partitions them.
	TArray<FBranchiveResolvedFileState> Classified;
	FString ScanBranch; bool bScanCurrent = true; FString ScanErr;
	const bool bScanned = ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
		Command.SessionLockedFiles, Classified, ScanBranch, bScanCurrent, ScanErr);

	TArray<FString> TrackedFiles;   // -> lock acquire (real check out)
	TArray<FString> UntrackedFiles; // -> stage (mark for add)
	if (bScanned)
	{
		for (const FBranchiveResolvedFileState& RS : Classified)
		{
			if (RS.State == EBranchiveWorkingCopyState::NotControlled)
			{
				UntrackedFiles.Add(RS.AbsFilename);
			}
			else
			{
				TrackedFiles.Add(RS.AbsFilename);
			}
		}
	}
	else
	{
		// Scan failed — can't classify; preserve the prior behavior (treat as tracked).
		TrackedFiles = Command.Files;
	}

	// Mark-for-add the untracked ones (`stage <file>`, §4.2). A new file has nothing
	// to lock against yet — the lock is acquired at first check-in.
	if (UntrackedFiles.Num() > 0)
	{
		TArray<FString> AddArgs = { TEXT("stage") };
		AddArgs.Append(UntrackedFiles);
		const FLoreCliResult AddRes = Cli.Run(AddArgs);
		if (!AddRes.Ok())
		{
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				AddRes.ReturnCode, AddRes.bSpawnFailed, ToU8(AddRes.StdErr), ToU8(AddRes.StdOut));
			Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
			return false;
		}
		Command.InfoMessages.Add(TEXT("New asset(s) marked for add — a lock is acquired at first check-in."));
	}

	// Lock-acquire the tracked ones (the real "check out").
	if (TrackedFiles.Num() > 0)
	{
		TArray<FString> Args = { TEXT("lock"), TEXT("acquire") };
		Args.Append(TrackedFiles);
		const FLoreCliResult Res = Cli.Run(Args);
		if (!Res.Ok())
		{
			const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
				Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
			Command.ErrorMessages.Add(ToF(BranchiveLore::FriendlyMessage(Ec)));
			return false;
		}
		AcquiredLocks = TrackedFiles;
	}

	// Recompute states treating the just-acquired (tracked) files as our own locks.
	TSet<FString> Augmented = Command.SessionLockedFiles;
	Augmented.Append(TSet<FString>(TrackedFiles));
	FString Error;
	ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files, Augmented,
		States, OutBranch, bOutIsCurrent, Error);
	return true;
}

// ------------------------------------------------------------------ CheckIn
bool FBranchiveCheckInWorker::Execute(FBranchiveSourceControlCommand& Command)
{
	FScopeLock Lock(MutexFor(Command));
	MaybeEnsureCloudAuth(Command);
	FLoreCli Cli(Command.PathToLoreBinary, Command.PathToRepositoryRoot);

	// ---- 0a. case-only rename guard (v0.3.9) --------------------------------
	// The lore CLI cannot represent Windows case-only renames safely: `stage`
	// hard-fails on them as moves, and one that slips through as a delete+add
	// pair produces a revision whose server-materialized state BREAKS
	// `revision history` in every clone that pulls it (live 2026-07-18: rev
	// "15. Activate Abilities by Tags" permanently poisoned the web commit
	// graph). Refuse the check-in up front — two distinct submit paths that are
	// case-insensitively equal can only be that pattern.
	{
		TMap<FString, FString> LowerToOriginal;
		for (const FString& F : Command.Files)
		{
			const FString LowerF = F.ToLower();
			if (const FString* Prev = LowerToOriginal.Find(LowerF))
			{
				if (*Prev != F)
				{
					Command.ErrorMessages.Add(FString::Printf(
						TEXT("This check-in renames a file or folder only by letter case (\"%s\" vs \"%s\"), which Lore cannot represent safely yet. Rename via an intermediate name instead (Name -> NameTmp, check in, then NameTmp -> FinalName)."),
						**Prev, *F));
					return false; // nothing staged, locks intact
				}
			}
			else
			{
				LowerToOriginal.Add(LowerF, F);
			}
		}
	}

	// ---- 1. stage the exact files being checked in (explicit list, §5.3) ----
	{
		TArray<FString> StageArgs = { TEXT("stage") };
		if (Command.Files.Num() > 0)
		{
			// Stale-path filter (v0.3.9): after an in-editor folder rename, UE's
			// cached states can still submit paths whose PARENT DIRECTORY no longer
			// exists — `lore stage` hard-fails on those with "[Error] Invalid path"
			// (exit 255) and the whole check-in died with a generic message (live
			// 2026-07-18). A missing FILE in an existing directory is fine (that is
			// how deletes stage — benign no-op if untracked); only a vanished parent
			// is unstageable, so drop exactly those, loudly.
			TArray<FString> Stageable;
			for (const FString& F : Command.Files)
			{
				if (FPaths::DirectoryExists(FPaths::GetPath(F)))
				{
					Stageable.Add(F);
				}
				else
				{
					Command.InfoMessages.Add(FString::Printf(
						TEXT("Skipped \"%s\" — its folder no longer exists on disk (moved or renamed). Refresh the view to pick up the new location."), *F));
				}
			}
			if (Stageable.Num() == 0)
			{
				Command.InfoMessages.Add(TEXT("Nothing to check in — every selected file's folder has moved. Refresh and retry."));
				FString Error;
				ComputeStates(Cli, Command.PathToRepositoryRoot, Command.Files,
					Command.SessionLockedFiles, States, OutBranch, bOutIsCurrent, Error);
				return true; // benign no-op, same posture as the empty-commit path below
			}
			StageArgs.Append(Stageable);
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
	// Files we actually hold: our session's locks PLUS (v0.3.9) any lock on a
	// committed file whose server-side owner is US (authUserInfo id from `lock
	// query --json`) — a lock carried over from a PREVIOUS editor session was
	// invisible to the session-set and silently survived every check-in, which
	// is how "GA_Primary keeps locking on us" happened. Still never touches
	// locks on files OUTSIDE this check-in, and never other users' locks.
	{
		TSet<FString> ServerOwnedRel;
		{
			const FLoreCliResult LockRes = Cli.Run({ TEXT("lock"), TEXT("query"), TEXT("--json") });
			if (!LockRes.bSpawnFailed && LockRes.ReturnCode == 0)
			{
				const BranchiveLore::FAuthUserInfo Self = BranchiveLore::ParseAuthUserInfo(ToU8(LockRes.StdOut));
				if (Self.bFound && !Self.UserId.empty())
				{
					for (const BranchiveLore::FLock& L : BranchiveLore::ParseLocksJson(ToU8(LockRes.StdOut)))
					{
						if (L.Owner == Self.UserId)
						{
							ServerOwnedRel.Add(NormalizeRel(ToF(L.Path)));
						}
					}
				}
			}
		}
		TArray<FString> ToRelease;
		for (const FString& F : Command.Files)
		{
			const bool bSessionHeld = Command.SessionLockedFiles.Contains(F);
			const bool bServerHeld = ServerOwnedRel.Contains(NormalizeRel(MakeRepoRelative(F, Command.PathToRepositoryRoot)));
			if (bSessionHeld || bServerHeld)
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

	// The Delete worker itself must remove the working-copy file from disk BEFORE
	// staging — exactly like the bundled UE providers: FGitDeleteWorker runs `git rm`
	// and FPerforceDeleteWorker runs `p4 delete`, BOTH of which remove the working-copy
	// file. UE's ObjectTools::DeleteObjectsInternal deliberately does NOT delete the
	// file for a source-controlled asset; it unloads the package + updates the
	// AssetRegistry first, then batches the path to FDelete and expects the provider to
	// remove it. So removing the file here is correct and matches the reference
	// providers (the earlier "the editor deletes the file on disk first" assumption was
	// wrong — the file stayed on disk tracked-clean and the delete was never recorded).
	//
	// LIVE CLI FINDING (lore 0.8.4+283, verified in a disposable workspace):
	//   * `stage <absFile>` on a file that is STILL on disk is a no-op — stdout
	//     "No changes staged", `status --scan` shows NO `D` row. THIS is the bug.
	//   * `stage <absFile>` on a MISSING file records the delete — stdout
	//     "Staging 1 files (... 1 deleted ...)", `status --scan` shows `D <path>`,
	//     and a subsequent commit removes it from the repo (file history ends in `D`).
	// So: physically delete each file first, THEN stage so the `D` is captured.
	// Deleting-if-present is safe under ANY editor ordering — if the editor (or a
	// concurrent op) already removed the file, the delete is a harmless no-op and the
	// stage still records the deletion.
	//
	// AFTER the delete is recorded we reconcile UE's editor view (see
	// ReconcileEditorViewAfterDelete) so a deleted asset whose package UE could not
	// unload does not ghost in the Content Browser or resurrect on "Save All".
	IFileManager& FileManager = IFileManager::Get();
	for (const FString& File : Command.Files)
	{
		if (FileManager.FileExists(*File))
		{
			// EvenReadOnly: a synced-but-unlocked asset can be read-only on disk.
			// RequireExists=false + Quiet: tolerate a prior/concurrent removal.
			FileManager.Delete(*File, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		}
	}

	// With the file(s) gone from disk, staging the explicit paths records each as a
	// `D` deletion (verified live) — no `. --scan` needed, and staging only the
	// intended files avoids over-staging unrelated working-tree changes.
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

	// The file(s) are now off disk and the deletion is recorded to Lore. Reconcile UE's
	// editor view (game thread) so an un-unloadable deleted asset can neither ghost in
	// the Content Browser nor resurrect on "Save All". No-op when the package unloaded
	// cleanly (the common case).
	ReconcileEditorViewAfterDelete(Command.Files);

	// Recompute: the file is off disk AND shows as staged `D`, so ClassifyWorkingCopy
	// returns Deleted (=> "Marked for delete", CanCheckIn true) — and it stays Deleted
	// across status refreshes (a missing file is never reclassified tracked-clean).
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
	MaybeEnsureCloudAuth(Command);
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
