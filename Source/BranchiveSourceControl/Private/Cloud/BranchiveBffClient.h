// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Synchronous, server-side HTTP client for the Branchive BFF auth endpoints
// (docs/INTEGRATIONS-AUTH-PKCE.md §3.x). Built on UE's HTTP module. Deliberately a
// NON-browser client (§3.3): it sets a descriptive User-Agent for log correlation
// but NO Origin/Sec-Fetch-Site header, so it lands in the BFF's non-browser CSRF
// allow path exactly like Desktop's Electron-main caller. It NEVER logs a token,
// code, verifier, or JWT.
//
// Each call BLOCKS until the request completes; it MUST be invoked off the game
// thread (the sign-in flow already runs on a background thread). The HTTP manager
// ticks on the game thread and fires completion there, unblocking the caller.
#pragma once

#include "CoreMinimal.h"

struct FBranchiveIdentity
{
	// Stable Branchive user id (the `/auth/me` identity.sub, e.g.
	// "usr_fbb8ccf7c60cc9cc20111338"). This is the SAME value the ambient `lore`
	// CLI reports as authUserInfo.userId, so it — NOT email — is the reliable key
	// for the "skip the redundant token mint" fast-path (BUG1): email may be blank
	// or differ between the BFF profile and the CLI's ambient identity.
	FString UserId;
	FString Name;
	FString Email;
	FString Handle;

	/** Short menu/notification label: "@handle" when present, else email/name. */
	FString Display() const
	{
		if (!Handle.IsEmpty()) return FString::Printf(TEXT("@%s"), *Handle);
		if (!Email.IsEmpty()) return Email;
		return Name.IsEmpty() ? TEXT("Branchive User") : Name;
	}
};

/** Common outcome flags shared by every BFF call. */
struct FBffOutcome
{
	bool bSuccess = false;      // 2xx and a well-formed body
	bool bUnauthorized = false; // HTTP 401 — session expired/revoked (flip signed-out state)
	FString Error;              // human-readable, NEVER contains a secret
};

struct FTokenExchangeResult : FBffOutcome
{
	FString Bearer;             // secret — never logged
	int64   ExpiresInSeconds = 0;
};

struct FMeResult : FBffOutcome
{
	FBranchiveIdentity Identity;
};

struct FLoreTokenResult : FBffOutcome
{
	FString Token;              // short-lived Lore JWT — secret, never logged
	FString AuthUrl;
	int64   ExpiresInSeconds = 0;
};

class FBranchiveBffClient
{
public:
	explicit FBranchiveBffClient(const FString& InBaseUrl);

	/** POST /auth/token { code, code_verifier } -> Bearer. */
	FTokenExchangeResult ExchangeCode(const FString& Code, const FString& CodeVerifier) const;

	/** GET /auth/me -> identity. 401 => bUnauthorized. */
	FMeResult Me(const FString& Bearer) const;

	/**
	 * POST /auth/lore-token { remoteUrl } -> short-lived Lore JWT.
	 * SECURITY (§4.0): the CALLER must have already passed IsCloudRemoteUrl on
	 * RemoteUrl before invoking this — this method does not re-gate.
	 */
	FLoreTokenResult LoreToken(const FString& Bearer, const FString& RemoteUrl) const;

	/** Best-effort POST /auth/logout — revokes this Bearer server-side. Never throws. */
	void Logout(const FString& Bearer) const;

	static FString DefaultUserAgent();

private:
	FString BaseUrl; // trailing '/' trimmed
};
