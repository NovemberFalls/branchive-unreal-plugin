// Copyright Branchive.
//
// Thin UE wrapper that spawns the `lore` CLI as a child process — NEVER a shell
// (contract §3.1). v1 uses spawn-per-op via FPlatformProcess::ExecProcess with
// the workspace path as cwd (contract §3.2). The persistent-CLI-shell
// optimization (one long-lived `lore` process) is a documented future
// optimization, deliberately not used for v1 simplicity (contract §3, and the
// UEPlasticPlugin reference which offers both).
#pragma once

#include "CoreMinimal.h"

/** Result of one `lore` invocation. */
struct FLoreCliResult
{
	int32   ReturnCode = -1;
	FString StdOut;
	FString StdErr;
	/** True if the process could not be spawned at all (binary not found). */
	bool    bSpawnFailed = false;

	bool Ok() const { return !bSpawnFailed && ReturnCode == 0; }

	/** stdout + "\n" + stderr, trimmed — used for conflict-marker scans. */
	FString Combined() const;
};

/**
 * Spawns `lore` with an argv array. `--repository <RepoPath>` is appended as the
 * LAST argument automatically (contract §3.2) unless bAppendRepository is false.
 */
class FLoreCli
{
public:
	FLoreCli(const FString& InBinaryPath, const FString& InRepoPath)
		: BinaryPath(InBinaryPath), RepoPath(InRepoPath)
	{
	}

	/**
	 * Blocking run. Safe to call from a background (worker) thread.
	 *
	 * TimeoutSeconds <= 0 (the default) runs the classic un-bounded ExecProcess path.
	 * TimeoutSeconds > 0 spawns via CreateProc and TERMINATES the child if it exceeds
	 * the budget — used for the remote-dialing, JWT-bearing `lore login` so a slow
	 * server can never wedge a source-control op (BUG1). On timeout the result carries
	 * ReturnCode = -1 (bSpawnFailed stays false), i.e. Ok() == false.
	 */
	FLoreCliResult Run(const TArray<FString>& Args, bool bAppendRepository = true, float TimeoutSeconds = 0.0f) const;

	const FString& GetBinaryPath() const { return BinaryPath; }
	const FString& GetRepoPath() const { return RepoPath; }

	/**
	 * Resolve the lore binary path: explicit setting -> LORE_BIN env -> "lore"/"lore.exe"
	 * searched on PATH + the known default install dir. ALWAYS returns an ABSOLUTE path to
	 * an existing file, or EMPTY on failure — NEVER a bare/relative name (F-BIN). An empty
	 * result causes FLoreCli::Run to refuse to spawn.
	 */
	static FString ResolveBinaryPath(const FString& ConfiguredPath);

	/** Quote a single argument for a Windows-style command line (also correct
	 *  enough for UE's own re-splitting on other platforms). */
	static FString QuoteArg(const FString& Arg);

private:
	FString BinaryPath;
	FString RepoPath;
};
