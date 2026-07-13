// Copyright Branchive. See LoreCli.h.
#include "LoreCli.h"

#include "BranchiveSourceControlLog.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

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
	if (!ConfiguredPath.IsEmpty())
	{
		return ConfiguredPath;
	}
	// LORE_BIN — the reference implementation's own override convention (§3.3).
	const FString EnvBin = FPlatformMisc::GetEnvironmentVariable(TEXT("LORE_BIN"));
	if (!EnvBin.IsEmpty())
	{
		return EnvBin;
	}
	// Fall back to a bare name resolved via PATH.
#if PLATFORM_WINDOWS
	return TEXT("lore.exe");
#else
	return TEXT("lore");
#endif
}

FLoreCliResult FLoreCli::Run(const TArray<FString>& Args, bool bAppendRepository) const
{
	FLoreCliResult Result;

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

	UE_LOG(LogBranchiveSourceControl, Verbose, TEXT("lore %s (cwd=%s)"), *Params, *Cwd);

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
