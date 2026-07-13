// Copyright Branchive. See LoreCli.h.
#include "LoreCli.h"

#include "BranchiveSourceControlLog.h"
#include "LoreBinaryResolve.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#include <string>

namespace
{
	// True iff Candidate contains a path separator (i.e. it's a path, not a bare leaf name).
	bool HasPathSeparator(const FString& S)
	{
		return S.Contains(TEXT("/")) || S.Contains(TEXT("\\"));
	}

	// Resolve a candidate to an ABSOLUTE, existing file — or empty on failure (F-BIN).
	//  * A path (has a separator): normalise to absolute; require it to exist as a file.
	//  * A bare leaf name: search PATH and the known default install dir; NEVER return the
	//    bare name (a bare name could resolve against the spawn cwd = a workspace).
	FString ResolveExecutableCandidate(const FString& Candidate)
	{
		if (Candidate.IsEmpty())
		{
			return FString();
		}

		if (HasPathSeparator(Candidate))
		{
			const FString Full = FPaths::ConvertRelativePathToFull(Candidate);
			if (!FPaths::IsRelative(Full) && FPaths::FileExists(Full))
			{
				return Full;
			}
			return FString();
		}

		// Bare leaf name -> search directories. Build the candidate leaf names first.
		TArray<FString> Leaves;
		Leaves.Add(Candidate);
#if PLATFORM_WINDOWS
		if (!Candidate.EndsWith(TEXT(".exe"), ESearchCase::IgnoreCase))
		{
			Leaves.Add(Candidate + TEXT(".exe"));
		}
#endif

		// Directories to search, in order: every PATH entry, then the known default
		// install dir(s).
		TArray<FString> Dirs;
		const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
		if (!PathEnv.IsEmpty())
		{
#if PLATFORM_WINDOWS
			const TCHAR* const Sep = TEXT(";");
#else
			const TCHAR* const Sep = TEXT(":");
#endif
			PathEnv.ParseIntoArray(Dirs, Sep, /*InCullEmpty=*/true);
		}
#if PLATFORM_WINDOWS
		// Branchive Desktop's documented default lore.exe location (README / QA setup).
		Dirs.Add(TEXT("D:\\Lore\\bin"));
#endif

		for (FString Dir : Dirs)
		{
			Dir = Dir.TrimStartAndEnd().TrimQuotes();
			if (Dir.IsEmpty())
			{
				continue;
			}
			for (const FString& Leaf : Leaves)
			{
				const FString Full = FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, Leaf));
				if (!FPaths::IsRelative(Full) && FPaths::FileExists(Full))
				{
					return Full;
				}
			}
		}
		return FString();
	}
}

FString FLoreCliResult::Combined() const
{
	FString Out = StdOut;
	if (!StdErr.IsEmpty())
	{
		if (!Out.IsEmpty()) { Out += TEXT("\n"); }
		Out += StdErr;
	}
	return Out.TrimStartAndEnd();
}

FString FLoreCli::QuoteArg(const FString& Arg)
{
	// Windows CommandLineToArgvW quoting rules. Empty -> "".
	if (Arg.IsEmpty())
	{
		return TEXT("\"\"");
	}

	// If it needs no quoting (no whitespace or quote), pass through.
	bool bNeedsQuotes = false;
	for (const TCHAR C : Arg)
	{
		if (C == TEXT(' ') || C == TEXT('\t') || C == TEXT('\n') ||
		    C == TEXT('\v') || C == TEXT('"'))
		{
			bNeedsQuotes = true;
			break;
		}
	}
	if (!bNeedsQuotes)
	{
		return Arg;
	}

	FString Out = TEXT("\"");
	int32 Backslashes = 0;
	for (int32 i = 0; i < Arg.Len(); ++i)
	{
		const TCHAR C = Arg[i];
		if (C == TEXT('\\'))
		{
			++Backslashes;
			continue;
		}
		if (C == TEXT('"'))
		{
			// Escape all pending backslashes (they precede a quote), then the quote.
			for (int32 b = 0; b < Backslashes * 2 + 1; ++b) { Out += TEXT('\\'); }
			Out += TEXT('"');
			Backslashes = 0;
			continue;
		}
		// Emit pending backslashes literally.
		for (int32 b = 0; b < Backslashes; ++b) { Out += TEXT('\\'); }
		Backslashes = 0;
		Out += C;
	}
	// Trailing backslashes precede the closing quote -> double them.
	for (int32 b = 0; b < Backslashes * 2; ++b) { Out += TEXT('\\'); }
	Out += TEXT('"');
	return Out;
}

FString FLoreCli::ResolveBinaryPath(const FString& ConfiguredPath)
{
	// F-BIN: resolve to an ABSOLUTE, existing file or FAIL (empty). A bare/relative name
	// must never reach FPlatformProcess::ExecProcess with cwd = a workspace directory.
	// Priority: explicit plugin setting -> LORE_BIN env (§3.3) -> a bare name searched on
	// PATH + the known default install dir. In EVERY branch the result is an absolute path
	// to an existing file, or empty (which FLoreCli::Run then refuses to spawn).
	if (!ConfiguredPath.IsEmpty())
	{
		return ResolveExecutableCandidate(ConfiguredPath);
	}
	const FString EnvBin = FPlatformMisc::GetEnvironmentVariable(TEXT("LORE_BIN"));
	if (!EnvBin.IsEmpty())
	{
		return ResolveExecutableCandidate(EnvBin);
	}
#if PLATFORM_WINDOWS
	return ResolveExecutableCandidate(TEXT("lore.exe"));
#else
	return ResolveExecutableCandidate(TEXT("lore"));
#endif
}

FLoreCliResult FLoreCli::Run(const TArray<FString>& Args, bool bAppendRepository, float TimeoutSeconds) const
{
	FLoreCliResult Result;

	// F-BIN: REFUSE to spawn unless the binary is an ABSOLUTE path to an existing file.
	// FPlatformProcess::ExecProcess runs with cwd = RepoPath (a workspace), and on Windows
	// a bare/relative image name can resolve against that cwd BEFORE PATH — a hostile repo
	// committing a `lore.exe` at its root would then run with the user's Lore JWT on the
	// command line. This refusal is unconditional and therefore also guards the
	// JWT-bearing `lore login` invocation.
	if (BinaryPath.IsEmpty()
		|| !BranchiveLore::IsAbsoluteBinaryPath(std::string(TCHAR_TO_UTF8(*BinaryPath)))
		|| !FPaths::FileExists(BinaryPath))
	{
		Result.bSpawnFailed = true;
		Result.ReturnCode = -1;
		Result.StdErr = FString::Printf(
			TEXT("Refusing to launch lore: '%s' is not an absolute path to an existing file."), *BinaryPath);
		UE_LOG(LogBranchiveSourceControl, Error,
			TEXT("Branchive: refusing to launch the lore binary — resolved path '%s' is not an absolute, existing file. ")
			TEXT("Set an absolute path in Revision Control settings or via the LORE_BIN environment variable."),
			*BinaryPath);
		return Result;
	}

	// Build the argv, appending "--repository <abs path>" last (§3.2).
	TArray<FString> FullArgs = Args;
	if (bAppendRepository && !RepoPath.IsEmpty())
	{
		FullArgs.Add(TEXT("--repository"));
		FullArgs.Add(RepoPath);
	}

	// Join into a properly-quoted command line. We never build a shell string.
	FString Params;
	for (int32 i = 0; i < FullArgs.Num(); ++i)
	{
		if (i > 0) { Params += TEXT(" "); }
		Params += QuoteArg(FullArgs[i]);
	}

	const FString Cwd = RepoPath.IsEmpty() ? FString() : RepoPath;

	// F-LOG: build a REDACTED command line for the log. The real argv (Params, above) is
	// unchanged — only this log string is sanitized. Any value following a secret-bearing
	// flag (`--token <JWT>`, `--auth-url <url>`) becomes "<redacted>", and an inline
	// "--flag=value" form is redacted after the '='. This keeps the login JWT out of logs.
	auto IsSecretFlag = [](const FString& A) -> bool
	{
		return A.Equals(TEXT("--token"), ESearchCase::IgnoreCase)
			|| A.Equals(TEXT("--auth-url"), ESearchCase::IgnoreCase);
	};
	FString RedactedLog;
	bool bRedactNextValue = false;
	for (int32 i = 0; i < FullArgs.Num(); ++i)
	{
		if (i > 0) { RedactedLog += TEXT(" "); }
		const FString& A = FullArgs[i];
		if (bRedactNextValue)
		{
			RedactedLog += TEXT("<redacted>");
			bRedactNextValue = false;
			continue;
		}
		FString Flag, Value;
		if (A.Split(TEXT("="), &Flag, &Value) && IsSecretFlag(Flag))
		{
			RedactedLog += Flag + TEXT("=<redacted>");
			continue;
		}
		RedactedLog += A;
		if (IsSecretFlag(A))
		{
			bRedactNextValue = true;
		}
	}

	UE_LOG(LogBranchiveSourceControl, Verbose, TEXT("lore %s (cwd=%s)"), *RedactedLog, *Cwd);

	// BUG1 — bounded path: spawn with pipes and TERMINATE the child if it blows its
	// budget, so a slow remote can never wedge the (worker-thread) op. Used only when
	// a caller passes TimeoutSeconds > 0 (currently the JWT-bearing `lore login`).
	if (TimeoutSeconds > 0.0f)
	{
		void* OutRead = nullptr;  void* OutWrite = nullptr;
		void* ErrRead = nullptr;  void* ErrWrite = nullptr;
		if (!FPlatformProcess::CreatePipe(OutRead, OutWrite) ||
			!FPlatformProcess::CreatePipe(ErrRead, ErrWrite))
		{
			if (OutRead || OutWrite) { FPlatformProcess::ClosePipe(OutRead, OutWrite); }
			if (ErrRead || ErrWrite) { FPlatformProcess::ClosePipe(ErrRead, ErrWrite); }
			Result.bSpawnFailed = true;
			Result.ReturnCode = -1;
			Result.StdErr = TEXT("Could not create pipes for a bounded lore invocation.");
			UE_LOG(LogBranchiveSourceControl, Error, TEXT("Branchive: CreatePipe failed for a bounded lore run."));
			return Result;
		}

		uint32 ProcId = 0;
		FProcHandle Proc = FPlatformProcess::CreateProc(
			*BinaryPath, *Params,
			/*bLaunchDetached=*/false, /*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true,
			&ProcId, /*PriorityModifier=*/0,
			Cwd.IsEmpty() ? nullptr : *Cwd,
			/*PipeWriteChild=*/OutWrite, /*PipeReadChild=*/nullptr, /*PipeStdErrChild=*/ErrWrite);

		if (!Proc.IsValid())
		{
			FPlatformProcess::ClosePipe(OutRead, OutWrite);
			FPlatformProcess::ClosePipe(ErrRead, ErrWrite);
			Result.bSpawnFailed = true;
			Result.ReturnCode = -1;
			Result.StdErr = FString::Printf(TEXT("Failed to launch '%s'"), *BinaryPath);
			UE_LOG(LogBranchiveSourceControl, Error, TEXT("Failed to launch lore binary '%s'"), *BinaryPath);
			return Result;
		}

		FString OutStdT, OutErrT;
		const double Deadline = FPlatformTime::Seconds() + double(TimeoutSeconds);
		bool bTimedOut = false;
		while (FPlatformProcess::IsProcRunning(Proc))
		{
			OutStdT += FPlatformProcess::ReadPipe(OutRead);
			OutErrT += FPlatformProcess::ReadPipe(ErrRead);
			if (FPlatformTime::Seconds() > Deadline)
			{
				FPlatformProcess::TerminateProc(Proc, /*KillTree=*/true);
				bTimedOut = true;
				break;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		// Drain whatever is still buffered after exit/terminate.
		OutStdT += FPlatformProcess::ReadPipe(OutRead);
		OutErrT += FPlatformProcess::ReadPipe(ErrRead);

		if (bTimedOut)
		{
			Result.bSpawnFailed = false; // it launched fine — it was just too slow
			Result.ReturnCode = -1;      // Ok() == false -> callers treat as a failed op
			Result.StdOut = OutStdT;
			Result.StdErr = OutErrT.IsEmpty()
				? FString::Printf(TEXT("lore timed out after %.0fs"), TimeoutSeconds)
				: OutErrT;
			UE_LOG(LogBranchiveSourceControl, Warning,
				TEXT("Branchive: a lore invocation exceeded its %.0fs budget and was terminated."), TimeoutSeconds);
		}
		else
		{
			int32 TimedReturnCode = -1;
			FPlatformProcess::GetProcReturnCode(Proc, &TimedReturnCode);
			Result.ReturnCode = TimedReturnCode;
			Result.StdOut = OutStdT;
			Result.StdErr = OutErrT;
		}

		FPlatformProcess::CloseProc(Proc);
		FPlatformProcess::ClosePipe(OutRead, OutWrite);
		FPlatformProcess::ClosePipe(ErrRead, ErrWrite);
		return Result;
	}

	int32 ReturnCode = -1;
	FString OutStd, OutErr;
	const bool bLaunched = FPlatformProcess::ExecProcess(
		*BinaryPath,
		*Params,
		&ReturnCode,
		&OutStd,
		&OutErr,
		Cwd.IsEmpty() ? nullptr : *Cwd);

	if (!bLaunched)
	{
		// Could not spawn the process at all — treat as binary-not-found (§3.6.1).
		Result.bSpawnFailed = true;
		Result.ReturnCode = -1;
		Result.StdErr = FString::Printf(TEXT("Failed to launch '%s'"), *BinaryPath);
		UE_LOG(LogBranchiveSourceControl, Error, TEXT("Failed to launch lore binary '%s'"), *BinaryPath);
		return Result;
	}

	Result.ReturnCode = ReturnCode;
	Result.StdOut = OutStd;
	Result.StdErr = OutErr;
	return Result;
}
