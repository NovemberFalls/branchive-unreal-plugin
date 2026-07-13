// Copyright Branchive.
//
// Branchive Cloud sign-in coordinator (docs/INTEGRATIONS-AUTH-PKCE.md §0, §2.4,
// §3.4, §4). One Bearer/identity per running editor, shared across the project —
// exactly like Desktop holds one session per app.
//
// Responsibilities:
//  - SignIn / SignOut: PKCE S256 + one-shot loopback (FLoopbackListener) +
//    token-at-rest (platform ILoreTokenStore) + GET /auth/me identity.
//  - EnsureCloudAuth: the §4.0 H1 EXACT-HOST gate (IsCloudRemoteUrl), then
//    POST /auth/lore-token + `lore login`, then the §4.3 workspace-pin rebind —
//    the CLI wiring that makes `lore` ops run as the signed-in user. Called before
//    remote-dialing CLI ops.
//  - Background refresh (§3.4): a TTL/3 timer that keeps the Lore JWT fresh and
//    slides the Branchive session; on a background 401 it flips a "signed out"
//    state and OFFERS re-auth — it NEVER auto-launches the browser from the timer.
//
// SECURITY: no secret (verifier / Bearer / Lore JWT / code) is ever logged. The
// isCloudRemoteUrl gate and the loopback bind discipline live in the standalone-
// tested engine-independent core (Lore/LoreAuthSeam.h, Lore/LoreLoopback.*).
#pragma once

#include "CoreMinimal.h"
#include "Cloud/BranchiveBffClient.h"
#include "Lore/LoreTokenStore.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeBool.h"

#include <memory>

class FBranchiveCloudAuth
{
public:
	FBranchiveCloudAuth();
	~FBranchiveCloudAuth();

	FBranchiveCloudAuth(const FBranchiveCloudAuth&) = delete;
	FBranchiveCloudAuth& operator=(const FBranchiveCloudAuth&) = delete;

	/** The module-owned instance (may be null if the module is not loaded). */
	static FBranchiveCloudAuth* Get();

	// ── state queries (game thread; for the Slate UI) ───────────────────────
	bool IsSignedIn() const;
	FString IdentityLabel() const;      // "@handle" / email / name, or empty when signed out
	bool IsTokenPersistent() const;     // false on Linux non-persistent -> UI can warn
	FString BffBaseUrl() const;
	FString CloudHost() const;

	// ── sign-in / sign-out ──────────────────────────────────────────────────
	/** Fire-and-forget sign-in on a background thread, with editor notifications. */
	void SignInAsync();
	/** Sign out: revoke Bearer (best-effort), clear at-rest + in-memory, stop refresh. */
	void SignOut();

	/** Best-effort restore of a prior sign-in on editor startup (validates via /auth/me). */
	void RestoreOnStartup();

	// ── §4: CLI wiring behind the H1 exact-host gate ────────────────────────
	/**
	 * Make subsequent `lore` ops on RepoPath run as the signed-in user against
	 * RemoteUrl. NO-OP unless RemoteUrl passes IsCloudRemoteUrl (self-hosted /
	 * local / hostile => hard no-op, no network, no login). Best-effort: any
	 * failure (incl. "not signed in") falls back to the CLI's ambient auth rather
	 * than breaking the op. SAFE to call from a worker thread.
	 */
	void EnsureCloudAuth(const FString& RemoteUrl, const FString& RepoPath);

	// ── refresh math (exposed for the unit-flavored asserts) ────────────────
	static double ComputeRefreshDelaySeconds(int64 ExpiresInSeconds);

private:
	// The blocking PKCE flow. Runs on a background thread; returns an error string
	// (empty on success). Populates identity + token store on success.
	FString SignInBlocking();

	void ArmRefresh(double DelaySeconds);
	void CancelRefresh();
	bool OnRefreshTick(float);   // game thread; kicks the background refresh work
	void RefreshWork();          // background thread
	void HandleBackground401();
	void ClearLocalSession();

	FString ResolveLoreBinary() const;
	void RunLoreLogin(const FString& RemoteUrl, const FString& AuthUrl, const FString& Jwt, const FString& RepoPath) const;
	void RebindWorkspacePin(const FString& RepoPath, const FString& Jwt) const;

	// The actual token mint (POST /auth/lore-token -> `lore login` -> §4.3 pin rebind ->
	// arm refresh). BUG1: run OFF the worker thread from EnsureCloudAuth so it never
	// blocks a source-control op; guarded by bMintInFlight so only one runs at a time.
	void MintCloudTokenBlocking(const FString& RemoteUrl, const FString& RepoPath, const FString& Bearer);

	// BUG2 (v0.3.5): GUARANTEE the signed-in identity — especially Identity.UserId — is
	// loaded before the skip decision runs. RestoreOnStartup refreshes /auth/me, but it
	// does so on a DETACHED background thread with no ordering vs the source-control ops
	// UE fires at editor startup: a reopened ("restored") session's first op routinely
	// beats it, so Identity.UserId is still empty when AmbientIdentityMatchesSignedIn
	// runs — the id-match can't succeed (and prod /auth/me carries only @handle, so the
	// email fallback can't either), the skip never fires, and the op falls through to the
	// HANGING /auth/lore-token mint every time. This does a SYNCHRONOUS /auth/me (the fast,
	// working endpoint — only /auth/lore-token hangs) to populate UserId when it is missing,
	// so the skip can fire on the very first op. No-op (zero network) once the identity is
	// cached. MUST be called only OFF the game thread (EnsureCloudAuth already is).
	void EnsureIdentityLoaded();

	// BUG1 fast-path: true when the CLI's AMBIENT identity already matches the
	// signed-in user — matched by STABLE USER ID (the /auth/me identity.sub ==
	// the CLI's authUserInfo.userId), with email only as a fallback. When true, the
	// /auth/lore-token mint + `lore login` would be a redundant round-trip to the same
	// identity. Best-effort — any probe failure returns false (fall through to the
	// non-blocking background mint).
	bool AmbientIdentityMatchesSignedIn(const FString& RepoPath) const;

	static FString BuildAuthUrl(const FString& Base, const FString& Challenge, const FString& RedirectUri);
	void NotifyGameThread(const FString& Title, const FString& Message, bool bError) const;

private:
	std::unique_ptr<BranchiveLore::ILoreTokenStore> TokenStore;

	mutable FCriticalSection StateMutex;
	FBranchiveIdentity Identity;   // guarded by StateMutex
	bool bHasIdentity = false;     // guarded by StateMutex
	FString LastCloudRemote;       // guarded by StateMutex
	FString LastCloudRepoPath;     // guarded by StateMutex

	FTSTicker::FDelegateHandle RefreshHandle;
	FThreadSafeBool bSignInInFlight{ false };
	// BUG1: at most one background token mint runs per editor (EnsureCloudAuth fires it
	// fire-and-forget so the op never waits on the mint). Cleared when the mint finishes.
	FThreadSafeBool bMintInFlight{ false };

	// In-memory cache of "a bearer is at rest" so the per-frame Slate UI attributes
	// never hit the OS keystore (DPAPI/Keychain). Seeded once in the ctor.
	FThreadSafeBool bTokenPresent{ false };
};
