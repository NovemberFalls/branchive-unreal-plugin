// Copyright Branchive.
#include "Cloud/BranchiveCloudAuth.h"

#include "BranchiveSourceControlLog.h"
#include "BranchiveSourceControlModule.h"
#include "Cloud/BranchiveCloudPlatform.h"
#include "Lore/LoreAuthSeam.h"
#include "Lore/LoreConfigPin.h"
#include "Lore/LoreLoopback.h"
#include "Lore/LorePkce.h"
#include "Lore/LoreParse.h"
#include "Lore/LoreCli.h"

#include "Async/Async.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Logging/MessageLog.h"

#if !UE_SERVER
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

#define LOCTEXT_NAMESPACE "BranchiveSourceControl.Cloud"

namespace
{
	FBranchiveCloudAuth* GCloudAuth = nullptr;

	constexpr double kFallbackRefreshSeconds = 20.0 * 60.0; // 20 min until first real expiresIn
	constexpr double kMinRefreshSeconds = 60.0;
	constexpr int32  kLoopbackTimeoutMs = 45 * 1000;        // §2.1 — outlast Google consent
	constexpr int32  kUnrealPortMin = 47500;                // §2.1 UE range
	constexpr int32  kUnrealPortMax = 47599;

	// BUG1 budgets: the ambient-identity probe and the JWT-bearing `lore login` are
	// both remote-dialing; bound them so a slow server can never wedge the op.
	constexpr float  kAmbientProbeTimeoutSeconds = 6.0f;    // `lore lock query --json`
	constexpr float  kLoreLoginTimeoutSeconds    = 8.0f;    // `lore login ...`

	FString ToF(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }
	std::string ToU8(const FString& S) { return std::string(TCHAR_TO_UTF8(*S)); }
}

FBranchiveCloudAuth::FBranchiveCloudAuth()
{
	TokenStore = MakePlatformTokenStore();
	// One DPAPI/Keychain read at startup to seed the in-memory "have a bearer" flag so
	// the per-frame UI attributes never hit the OS keystore.
	bTokenPresent = TokenStore && !TokenStore->Load().empty();
	GCloudAuth = this;
}

FBranchiveCloudAuth::~FBranchiveCloudAuth()
{
	CancelRefresh();
	if (GCloudAuth == this)
	{
		GCloudAuth = nullptr;
	}
}

FBranchiveCloudAuth* FBranchiveCloudAuth::Get()
{
	return GCloudAuth;
}

// ── config resolution ───────────────────────────────────────────────────────

FString FBranchiveCloudAuth::BffBaseUrl() const
{
	FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("BRANCHIVE_BFF"));
	if (!Override.IsEmpty())
	{
		while (Override.EndsWith(TEXT("/"))) { Override.LeftChopInline(1); }
		return Override;
	}
	const FString Dev = FPlatformMisc::GetEnvironmentVariable(TEXT("LORE_GUI_DEV"));
	if (!Dev.IsEmpty())
	{
		return TEXT("http://127.0.0.1:8088"); // dev BFF (mirrors Desktop's dev switch)
	}
	return TEXT("https://app.branchive.io"); // production default
}

FString FBranchiveCloudAuth::CloudHost() const
{
	FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("BRANCHIVE_CLOUD_HOST"));
	if (!Override.IsEmpty())
	{
		return Override.TrimStartAndEnd().ToLower();
	}
	return ToF(BranchiveLore::BranchiveCloudHost()); // "vcs.branchive.io"
}

FString FBranchiveCloudAuth::ResolveLoreBinary() const
{
	FString Configured;
	if (FBranchiveSourceControlModule* Module = FBranchiveSourceControlModule::GetThreadSafe())
	{
		Configured = Module->AccessSettings().GetLoreBinaryPath();
	}
	return FLoreCli::ResolveBinaryPath(Configured);
}

// ── state queries ───────────────────────────────────────────────────────────

bool FBranchiveCloudAuth::IsSignedIn() const { return bTokenPresent; }

bool FBranchiveCloudAuth::IsTokenPersistent() const
{
	return TokenStore && TokenStore->IsPersistent();
}

FString FBranchiveCloudAuth::IdentityLabel() const
{
	FScopeLock Lock(&StateMutex);
	return bHasIdentity ? Identity.Display() : FString();
}

// ── sign-in / sign-out ──────────────────────────────────────────────────────

FString FBranchiveCloudAuth::BuildAuthUrl(const FString& Base, const FString& Challenge, const FString& RedirectUri)
{
	FString B = Base;
	while (B.EndsWith(TEXT("/"))) { B.LeftChopInline(1); }
	// F2: NO loopback `state` — PKCE S256 is the control on the fixed /cb hop (§2.1).
	return B + TEXT("/auth/login/google")
		+ TEXT("?client=unreal")
		+ TEXT("&code_challenge=") + FGenericPlatformHttp::UrlEncode(Challenge)
		+ TEXT("&code_challenge_method=S256")
		+ TEXT("&redirect_uri=") + FGenericPlatformHttp::UrlEncode(RedirectUri);
}

FString FBranchiveCloudAuth::SignInBlocking()
{
	// 1) PKCE: 32 crypto-strong random bytes -> verifier (SECRET, never logged).
	uint8 Random[32];
	if (!BranchiveCryptoRandomBytes(Random, sizeof(Random)))
	{
		return TEXT("Could not generate secure random data for sign-in (OS CSPRNG unavailable).");
	}
	const std::string VerifierStd = BranchiveLore::EncodeVerifier(Random, sizeof(Random));
	FMemory::Memzero(Random, sizeof(Random));
	const std::string ChallengeStd = BranchiveLore::DeriveChallengeS256(VerifierStd);

	// 2) BIND-BEFORE-BROWSER (§2.1/M2): bind + confirm listening BEFORE opening the
	// browser. The auth URL is not even computed until we hold a bound port.
	BranchiveLore::FLoopbackListener Listener(kUnrealPortMin, kUnrealPortMax);
	const int Port = Listener.Bind();
	if (Port < 0)
	{
		// Range exhausted — FAIL CLOSED (never a fallback to 0.0.0.0 / out-of-range).
		return TEXT("Couldn't start the local Branchive sign-in listener (ports 47500-47599 all in use). Check for a conflicting local process or a restrictive firewall/AV, then retry.");
	}
	UE_LOG(LogBranchiveSourceControl, Log, TEXT("Branchive sign-in: loopback listening on 127.0.0.1:%d"), Port); // no secrets

	const FString RedirectUri = FString::Printf(TEXT("http://127.0.0.1:%d/cb"), Port);
	const FString AuthUrl = BuildAuthUrl(BffBaseUrl(), ToF(ChallengeStd), RedirectUri);

	// Open the system browser ONLY after a confirmed bind. Marshal LaunchURL to the
	// game thread (some platforms require it) — fire-and-forget; the await is already armed.
	AsyncTask(ENamedThreads::GameThread, [AuthUrl]()
	{
		FString Err;
		FPlatformProcess::LaunchURL(*AuthUrl, nullptr, &Err);
	});

	// 3) Await the one-time code — 45s timeout that REJECTS (never hangs).
	const BranchiveLore::FLoopbackResult Cb = Listener.AwaitCode(kLoopbackTimeoutMs);
	if (Cb.bTimedOut)
	{
		return TEXT("Sign-in isn't available on this Branchive server yet, or wasn't completed. Run `lore login` from the command line instead, or check for a plugin/server update.");
	}
	if (!Cb.bOk)
	{
		return ToF(Cb.Error.empty() ? std::string("Sign-in was cancelled or failed.") : Cb.Error);
	}

	// 4) Exchange the code (+ verifier) for a Bearer; validate via /auth/me BEFORE persisting.
	const FBranchiveBffClient Bff(BffBaseUrl());
	const FTokenExchangeResult Tok = Bff.ExchangeCode(ToF(Cb.Code), ToF(VerifierStd));
	if (!Tok.bSuccess)
	{
		return Tok.Error.IsEmpty() ? TEXT("Sign-in token exchange failed.") : Tok.Error;
	}
	const FMeResult Me = Bff.Me(Tok.Bearer);
	if (!Me.bSuccess)
	{
		return Me.Error.IsEmpty() ? TEXT("Could not confirm your Branchive identity.") : Me.Error;
	}

	// 5) Persist the validated Bearer at rest + cache the identity + arm refresh.
	if (TokenStore)
	{
		TokenStore->Store(ToU8(Tok.Bearer));
	}
	bTokenPresent = true;
	{
		FScopeLock Lock(&StateMutex);
		Identity = Me.Identity;
		bHasIdentity = true;
	}
	ArmRefresh(kFallbackRefreshSeconds);
	return FString(); // success
}

void FBranchiveCloudAuth::SignInAsync()
{
	if (bSignInInFlight.AtomicSet(true))
	{
		return; // a sign-in is already running
	}
	Async(EAsyncExecution::Thread, [this]()
	{
		const FString Err = SignInBlocking();
		bSignInInFlight = false;
		if (Err.IsEmpty())
		{
			FString Msg = FString::Printf(TEXT("Signed in as %s."), *IdentityLabel());
			if (!IsTokenPersistent())
			{
				Msg += TEXT(" (This session's token is not persisted on this platform — you will re-authenticate next session.)");
			}
			NotifyGameThread(TEXT("Signed in to Branchive"), Msg, /*bError=*/false);
		}
		else
		{
			NotifyGameThread(TEXT("Branchive sign-in failed"), Err, /*bError=*/true);
		}
	});
}

void FBranchiveCloudAuth::SignOut()
{
	FString Bearer;
	if (TokenStore)
	{
		Bearer = ToF(TokenStore->Load());
	}
	const FString Base = BffBaseUrl();
	if (!Bearer.IsEmpty())
	{
		// Best-effort server-side revoke, off the game thread (it's a network call).
		Async(EAsyncExecution::Thread, [Base, Bearer]()
		{
			FBranchiveBffClient(Base).Logout(Bearer);
		});
	}
	ClearLocalSession();
	NotifyGameThread(TEXT("Signed out of Branchive"), TEXT("Your local Branchive session was cleared."), /*bError=*/false);
}

void FBranchiveCloudAuth::RestoreOnStartup()
{
	if (!IsSignedIn())
	{
		return;
	}
	Async(EAsyncExecution::Thread, [this]()
	{
		FString Bearer = TokenStore ? ToF(TokenStore->Load()) : FString();
		if (Bearer.IsEmpty())
		{
			return;
		}
		const FMeResult Me = FBranchiveBffClient(BffBaseUrl()).Me(Bearer);
		if (Me.bUnauthorized)
		{
			ClearLocalSession(); // never opens a browser
			return;
		}
		if (Me.bSuccess)
		{
			{
				FScopeLock Lock(&StateMutex);
				Identity = Me.Identity;
				bHasIdentity = true;
			}
			ArmRefresh(kFallbackRefreshSeconds);
		}
	});
}

// ── §4: CLI wiring behind the H1 exact-host gate ────────────────────────────

void FBranchiveCloudAuth::EnsureCloudAuth(const FString& RemoteUrl, const FString& RepoPath)
{
	// §4.0 H1 — the EXACT-HOST gate. Self-hosted / local / hostile remote => hard
	// no-op: no POST /auth/lore-token, no `lore login`. This is what prevents a
	// hostile workspace's remote_url from ever harvesting the user's identity JWT.
	if (!BranchiveLore::IsCloudRemoteUrl(ToU8(RemoteUrl), ToU8(CloudHost())))
	{
		return;
	}
	if (!TokenStore)
	{
		return;
	}
	const std::string BearerStd = TokenStore->Load();
	if (BearerStd.empty())
	{
		return; // not signed in — fall back to the CLI's ambient auth (v1)
	}
	const FString Bearer = ToF(BearerStd);

	// BUG1 fast-path — SKIP the whole mint when the CLI is ALREADY authenticated as
	// the signed-in user. The mint's only job is to re-attribute `lore` ops to that
	// identity; if the ambient CLI identity already matches, the /auth/lore-token +
	// `lore login` round-trip is pure redundant latency (and the exact thing that
	// hung 30s in the smoke test). Best-effort: a failed probe just falls through.
	if (AmbientIdentityMatchesSignedIn(RepoPath))
	{
		UE_LOG(LogBranchiveSourceControl, Verbose,
			TEXT("Branchive ensureCloudAuth: CLI already authenticated as the signed-in user; skipping the token mint."));
		return;
	}

	const FLoreTokenResult Lt = FBranchiveBffClient(BffBaseUrl()).LoreToken(Bearer, RemoteUrl);
	if (Lt.bUnauthorized)
	{
		HandleBackground401();
		return;
	}
	if (!Lt.bSuccess)
	{
		// BUG1 best-effort — a slow/unavailable /auth/lore-token must NEVER fail the
		// op. The ambient CLI auth already does the real work (lock/commit/push); the
		// mint is only an attribution nicety. Warn (so the prod BFF timeout is visible)
		// and CONTINUE — the op proceeds against the ambient auth.
		UE_LOG(LogBranchiveSourceControl, Warning,
			TEXT("Branchive ensureCloudAuth: /auth/lore-token unavailable (%s); continuing on the ambient CLI auth."),
			*Lt.Error);
		return;
	}

	RunLoreLogin(RemoteUrl, Lt.AuthUrl, Lt.Token, RepoPath);
	RebindWorkspacePin(RepoPath, Lt.Token);

	{
		FScopeLock Lock(&StateMutex);
		LastCloudRemote = RemoteUrl;
		LastCloudRepoPath = RepoPath;
	}
	ArmRefresh(ComputeRefreshDelaySeconds(Lt.ExpiresInSeconds));
}

bool FBranchiveCloudAuth::AmbientIdentityMatchesSignedIn(const FString& RepoPath) const
{
	// Need a signed-in identity (email) to compare the ambient CLI identity against.
	FString SignedInEmail;
	{
		FScopeLock Lock(&StateMutex);
		if (!bHasIdentity)
		{
			return false;
		}
		SignedInEmail = Identity.Email;
	}
	if (SignedInEmail.IsEmpty())
	{
		return false;
	}

	// Probe the CLI's ambient identity cheaply. An authenticated `lore lock query
	// --json` carries an "authUserInfo" event with the logged-in user's id + email.
	// Bounded so a slow server can't stall the op; NOT --branch (a bare query is
	// enough to surface authUserInfo, and needs no resolved branch).
	FLoreCli Cli(ResolveLoreBinary(), RepoPath);
	const FLoreCliResult Res = Cli.Run(
		{ TEXT("lock"), TEXT("query"), TEXT("--json") },
		/*bAppendRepository=*/true, /*TimeoutSeconds=*/kAmbientProbeTimeoutSeconds);
	if (Res.bSpawnFailed)
	{
		return false;
	}

	const BranchiveLore::FAuthUserInfo Info = BranchiveLore::ParseAuthUserInfo(ToU8(Res.StdOut));
	return BranchiveLore::AmbientMatchesSignedIn(Info, ToU8(SignedInEmail));
}

void FBranchiveCloudAuth::RunLoreLogin(const FString& RemoteUrl, const FString& AuthUrl, const FString& Jwt, const FString& RepoPath) const
{
	// `lore login <remoteUrl> --auth-url <authUrl> --token-type lore --token <JWT>
	// --non-interactive` — execFile-style (FLoreCli spawns a child process, NEVER a
	// shell), so the JWT never touches a shell/history. NOT --repository (login is
	// not a repo op), so bAppendRepository=false.
	FLoreCli Cli(ResolveLoreBinary(), RepoPath);
	const TArray<FString> Args = {
		TEXT("login"), RemoteUrl,
		TEXT("--auth-url"), AuthUrl,
		TEXT("--token-type"), TEXT("lore"),
		TEXT("--token"), Jwt,
		TEXT("--non-interactive"),
	};
	// BUG1 — bound `lore login` (remote-dialing, JWT-bearing): on a wedged server it
	// is terminated at the budget instead of blocking the worker thread indefinitely.
	const FLoreCliResult Res = Cli.Run(Args, /*bAppendRepository=*/false, /*TimeoutSeconds=*/kLoreLoginTimeoutSeconds);
	if (!Res.Ok())
	{
		// NEVER log Args (they carry the JWT). stderr does not carry the token.
		UE_LOG(LogBranchiveSourceControl, Warning,
			TEXT("Branchive: `lore login` failed (exit %d): %s"), Res.ReturnCode, *Res.StdErr.Left(300));
	}
}

void FBranchiveCloudAuth::RebindWorkspacePin(const FString& RepoPath, const FString& Jwt) const
{
	// §4.3: rebind the workspace's TOP-LEVEL identity pin to the token's sub so ops
	// attribute to the signed-in user. Line-based, EOL-preserving, F3-safe insert.
	const std::string Sub = BranchiveLore::DecodeJwtSub(ToU8(Jwt));
	if (Sub.empty())
	{
		return;
	}
	const FString ConfigPath = FPaths::Combine(RepoPath, TEXT(".lore"), TEXT("config.toml"));
	FString Current;
	if (!FFileHelper::LoadFileToString(Current, *ConfigPath))
	{
		return;
	}
	const std::string Rebound = BranchiveLore::RebindIdentityLine(ToU8(Current), Sub);
	const FString ReboundF = ToF(Rebound);
	if (!ReboundF.Equals(Current, ESearchCase::CaseSensitive))
	{
		FFileHelper::SaveStringToFile(ReboundF, *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

// ── §3.4 background refresh ──────────────────────────────────────────────────

double FBranchiveCloudAuth::ComputeRefreshDelaySeconds(int64 ExpiresInSeconds)
{
	if (ExpiresInSeconds <= 0)
	{
		return kFallbackRefreshSeconds;
	}
	const double Third = double(ExpiresInSeconds) / 3.0;
	return FMath::Max(Third, kMinRefreshSeconds);
}

void FBranchiveCloudAuth::ArmRefresh(double DelaySeconds)
{
	CancelRefresh();
	RefreshHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FBranchiveCloudAuth::OnRefreshTick), (float)DelaySeconds);
}

void FBranchiveCloudAuth::CancelRefresh()
{
	if (RefreshHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshHandle);
		RefreshHandle.Reset();
	}
}

bool FBranchiveCloudAuth::OnRefreshTick(float)
{
	// One-shot ticker: kick the actual refresh onto a background thread (network +
	// `lore login`), which re-arms the next tick itself. Return false so this handle
	// does not repeat.
	Async(EAsyncExecution::Thread, [this]() { RefreshWork(); });
	return false;
}

void FBranchiveCloudAuth::RefreshWork()
{
	if (!TokenStore)
	{
		return;
	}
	const std::string BearerStd = TokenStore->Load();
	if (BearerStd.empty())
	{
		return; // signed out — stop (do not re-arm)
	}
	const FString Bearer = ToF(BearerStd);
	const FBranchiveBffClient Bff(BffBaseUrl());

	// Slide the 12h session + validate.
	const FMeResult Me = Bff.Me(Bearer);
	if (Me.bUnauthorized)
	{
		HandleBackground401();
		return; // do NOT re-arm — the user must re-auth
	}
	if (Me.bSuccess)
	{
		FScopeLock Lock(&StateMutex);
		Identity = Me.Identity;
		bHasIdentity = true;
	}

	FString Remote, RepoPath;
	{
		FScopeLock Lock(&StateMutex);
		Remote = LastCloudRemote;
		RepoPath = LastCloudRepoPath;
	}

	double NextDelay = kFallbackRefreshSeconds;
	if (!Remote.IsEmpty() && BranchiveLore::IsCloudRemoteUrl(ToU8(Remote), ToU8(CloudHost())))
	{
		const FLoreTokenResult Lt = Bff.LoreToken(Bearer, Remote);
		if (Lt.bUnauthorized)
		{
			HandleBackground401();
			return;
		}
		if (Lt.bSuccess)
		{
			RunLoreLogin(Remote, Lt.AuthUrl, Lt.Token, RepoPath);
			RebindWorkspacePin(RepoPath, Lt.Token);
			NextDelay = ComputeRefreshDelaySeconds(Lt.ExpiresInSeconds);
		}
	}
	ArmRefresh(NextDelay);
}

void FBranchiveCloudAuth::HandleBackground401()
{
	// A background 401 (expired past the 14-day cap, or revoked elsewhere). Flip to a
	// clearly signed-out state and OFFER re-auth via a notification — NEVER auto-launch
	// the browser from the timer (§3.4: a surprise popup looks like phishing).
	ClearLocalSession();
	NotifyGameThread(TEXT("Signed out of Branchive Cloud"),
		TEXT("Your Branchive session expired or was revoked. Open Revision Control settings and sign in again to keep working with the cloud."),
		/*bError=*/false);
}

void FBranchiveCloudAuth::ClearLocalSession()
{
	if (TokenStore)
	{
		TokenStore->Clear();
	}
	bTokenPresent = false;
	{
		FScopeLock Lock(&StateMutex);
		Identity = FBranchiveIdentity();
		bHasIdentity = false;
		LastCloudRemote.Empty();
		LastCloudRepoPath.Empty();
	}
	CancelRefresh();
}

// ── notifications ───────────────────────────────────────────────────────────

void FBranchiveCloudAuth::NotifyGameThread(const FString& Title, const FString& Message, bool bError) const
{
	const FString FullLog = FString::Printf(TEXT("Branchive Cloud: %s — %s"), *Title, *Message);
	if (bError)
	{
		UE_LOG(LogBranchiveSourceControl, Warning, TEXT("%s"), *FullLog);
	}
	else
	{
		UE_LOG(LogBranchiveSourceControl, Log, TEXT("%s"), *FullLog);
	}

	const FString TitleCopy = Title;
	const FString MessageCopy = Message;
	AsyncTask(ENamedThreads::GameThread, [TitleCopy, MessageCopy, bError]()
	{
		FMessageLog SourceControlLog("SourceControl");
		if (bError)
		{
			SourceControlLog.Warning(FText::FromString(FString::Printf(TEXT("%s — %s"), *TitleCopy, *MessageCopy)));
		}
		else
		{
			SourceControlLog.Info(FText::FromString(FString::Printf(TEXT("%s — %s"), *TitleCopy, *MessageCopy)));
		}
#if !UE_SERVER
		if (FSlateApplication::IsInitialized())
		{
			FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("%s\n%s"), *TitleCopy, *MessageCopy)));
			Info.ExpireDuration = bError ? 8.0f : 5.0f;
			Info.bUseSuccessFailIcons = true;
			TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
			if (Item.IsValid())
			{
				Item->SetCompletionState(bError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
			}
		}
#endif
	});
}

#undef LOCTEXT_NAMESPACE
