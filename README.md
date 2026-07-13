# Branchive — Unreal Engine 5 Revision Control plugin

A native `ISourceControlProvider` that makes **Branchive** appear in Unreal
Editor's *Revision Control → Provider* dropdown. It wraps the `lore` CLI
(Epic's Lore VCS client; Branchive runs a server-side fork) and implements the
core check-out / check-in loop with **server-enforced per-file locking** — the
headline feature, since UE's "Check Out" *is* exclusive locking.

Implements **Branchive integrations contract 2.0.0**
(`docs/INTEGRATIONS-CONTRACT.md`). This is the C++/UE5 sibling of the JetBrains
(Kotlin) and VS Code (TypeScript) plugins; the three share only that contract
and the golden fixtures under `integrations/contract/fixtures/`.

## Status (v0.2.0 — core loop + history / diff / conflict resolve)

| UE operation | Lore mapping (contract §5.3) | State |
|---|---|---|
| `Connect` | verify `.lore/` workspace + `status --scan` reachable | wired |
| `UpdateStatus` / `GetState` | `status --scan` + `lock query --json --branch <b>` | wired |
| `CheckOut` | `lock acquire` (== UE exclusive checkout) | wired |
| `CheckIn` | **composite**: `stage` → `commit` → `branch push` → release own locks | wired |
| `Revert` | `file reset` (discard local edits) + release own lock — **NOT** `revision revert` | wired |
| `Sync` | `sync` (pull) | wired |
| `MarkForAdd` | `stage <file>` | wired |
| `Delete` | `stage <file>` (records the on-disk deletion) | wired |
| `UpdateStatus(bUpdateHistory=true)` → History panel | `file history` (§4.12) → `FBranchiveSourceControlRevision` list | **wired** (Content Browser → Revision Control → History) |
| Diff against revision | `file diff --source <sig>` (§4.13) — reverse-apply hunks → temp file `Get()` | **wired for text**; binary assets (`.uasset`/`.umap`) degrade to "unavailable" (no cat-at-revision verb; never fabricates bytes) |
| `BranchiveResolveMine` / `BranchiveResolveTheirs` | `branch merge resolve mine\|theirs <file>` (§4.19) | **wired** (op + worker); whole-side ours/theirs only — Perforce-style, no 3-way hand-merge |
| `BranchiveAbortMerge` | `branch merge abort` (§4.20) | **wired**; completion = ordinary `CheckIn`/commit (§4.21) |
| Conflict resolve *context-menu* Slate entries | (dispatch the ops above) | **thin UI glue remaining** — ops are dispatchable now; state exposes `IsConflicted()` |
| Changelists | — | **not used** (UsesChangelists=false) |
| Cloud sign-in (PKCE) | loopback 47500–47599 + DPAPI/Keychain | **documented seam** (Wave-2b) — see `Private/Lore/LoreAuthSeam.h` |

Capability flags (contract §5.3): `UsesCheckout()=true`, `UsesChangelists()=false`,
`UsesFileRevisions()=true`, `UsesLocalReadOnlyState()=false` (deferred),
`UsesUncontrolledChangelists()=false`.

## Architecture

Project-level plugin, single `UncookedOnly` module, `LoadingPhase
EarliestPossible`, one async worker per operation on `GThreadPool` (the
canonical SRombauts-UEPlasticPlugin / bundled-Git-provider pattern). v1 uses
**spawn-per-op** (`FPlatformProcess::ExecProcess`, never a shell); a persistent
CLI shell is a documented future optimization.

```
BranchiveSourceControl.uplugin
Source/BranchiveSourceControl/
  BranchiveSourceControl.Build.cs
  Private/
    BranchiveSourceControlModule.*      IModuleInterface; RegisterModularFeature("SourceControl", &Provider)
    BranchiveSourceControlProvider.*    ISourceControlProvider (Init/Close/Tick/Execute/GetState/capabilities)
    BranchiveSourceControlState.*       ISourceControlState (IsCheckedOut / IsCheckedOutOther → "Locked by <user>"; history via GetHistoryItem)
    BranchiveSourceControlRevision.*    ISourceControlRevision (one revision; Get() reconstructs historical bytes for diff — §4.12/§4.13)
    BranchiveSourceControlConflictOperations.h  FBranchiveResolveMine/Theirs + FBranchiveAbortMerge (§4.19/§4.20)
    BranchiveSourceControlCommand.*     IQueuedWork async command
    BranchiveSourceControlOperations.*  one worker class per op (core loop + history fetch + resolve/abort)
    BranchiveSourceControlSettings.*    lore binary path (SourceControlSettings.ini)
    SBranchiveSourceControlSettings.*   minimal Slate settings widget
    Lore/
      LoreCli.*        spawn-per-op (ExecProcess, cwd=workspace, argv per §3.2, quoting)
      LoreParse.*      ENGINE-INDEPENDENT parsers: status (§4.1) + lock NDJSON (§4.17)
      LoreErrors.*     ENGINE-INDEPENDENT error taxonomy (§7)
      LoreAuthSeam.*   Wave-2b PKCE seam + the security-critical isCloudRemoteUrl gate (§4.0)
    Tests/BranchiveParserFixtureTest.cpp  in-editor automation tests ("Branchive.Parser.*")
Tests/standalone/
  parser_test.cpp   standalone fixture test (shares LoreParse.cpp/LoreErrors.cpp; reads real fixtures)
  run.sh / run.ps1
```

`Lore/LoreParse.*` and `Lore/LoreErrors.*` are **pure std C++** (no UE types) so
the exact same translation units are (a) compiled into the module and (b)
compiled + run standalone against the golden fixtures. `bUseUnity=false` keeps
UE's global macros from bleeding into those std TUs.

### The CheckIn composite (contract §5.3 + §7 error handling)

`CheckIn` runs, as one atomic user action:
1. `stage` the exact files being checked in (explicit list).
2. `commit "<changelist description>" --non-interactive` (message is positional).
3. `branch push <branch> -P --log-level debug` (with store-overload backoff).
4. `lock release` **only** the just-committed files we hold (never a blanket release).

Error handling: an **empty commit** (exit 21, `Nothing staged`) is a benign
no-op — not a failure — and skips push/release. A **diverged push** (exit 255,
`Branch has diverged`) surfaces as "someone pushed first — Sync then retry" and
leaves your lock intact. If stage/commit/push fail, locks are **not** released,
so the whole CheckIn can be retried without losing your exclusive lock.

## Requirements

- Unreal Engine **5.6** (primary) or **5.4** (compile-compat; the ISourceControl
  API is stable 5.4–5.6, so no version shims were needed).
- The `lore` binary. Set its path in *Revision Control → Branchive settings*, or
  via the `LORE_BIN` environment variable, or have `lore` on `PATH`.
- A Lore workspace: the UE project directory must contain a `.lore/` folder.

## Build

Plugin binaries are engine-version-specific, so build once per engine version
with `RunUAT BuildPlugin`:

```
"C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\RunUAT.bat" ^
  BuildPlugin ^
  -Plugin="<abs path>\BranchiveSourceControl.uplugin" ^
  -Package="C:\bpkg56" ^
  -TargetPlatforms=Win64 -Rocket
```

> Use a **short** `-Package` path (e.g. `C:\bpkg56`). UBT enforces a path-length
> limit on generated intermediates; a deep output path fails in
> `ActionGraph.CheckPathLengths` before any `.cpp` compiles.

Requires the MSVC C++ toolchain (Visual Studio 2022 / Build Tools with the
"Desktop development with C++" workload).

## Tests

**Standalone fixture test** (no engine needed) — compiles the real
`LoreParse.cpp` / `LoreErrors.cpp` and runs them over the golden fixtures:

```
# Git Bash (g++/clang):
integrations/unreal/Tests/standalone/run.sh
# MSVC (from a VS x64 Native Tools prompt):
powershell integrations/unreal/Tests/standalone/run.ps1
```

**In-editor automation tests** — `Branchive.Parser.Status`,
`Branchive.Parser.Locks`, `Branchive.Parser.Errors` in the Session Frontend
Automation window, or `-ExecCmds="Automation RunTests Branchive"`.

## Security seam (auth Wave-2b)

Core ops run against the ambient `lore` auth store and do not require in-plugin
sign-in. When Cloud PKCE sign-in ships, it MUST honor
`docs/INTEGRATIONS-AUTH-PKCE.md`: loopback listener 47500–47599, bind-then-launch,
exclusive-address, 45s reject-the-future timeout; DPAPI (Windows) / Keychain
(macOS) / on Linux **fail-safe to non-persistent when single-user-interactive
context can't be confirmed**. The single highest-severity control — the
**exact-host** `isCloudRemoteUrl` gate (§4.0) that prevents leaking the signed-in
user's JWT to a hostile workspace's `remote_url` — is already implemented and
unit-tested in `Private/Lore/LoreAuthSeam.h`.
