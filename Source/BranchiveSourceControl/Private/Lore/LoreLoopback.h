// Copyright Branchive.
//
// Engine-independent, one-shot loopback HTTP listener for the Branchive OAuth
// callback (docs/INTEGRATIONS-AUTH-PKCE.md §2.1 / §2.4). Pure C++ + the platform
// BSD/Winsock socket API — NO Unreal Engine dependency — so the SAME translation
// unit the UE module ships is compiled + run standalone (Tests/standalone/
// auth_test.cpp) to prove the security-critical bind discipline zara re-verifies:
//
//   * Binds 127.0.0.1 ONLY (never 0.0.0.0, never a DNS name).
//   * EXCLUSIVE bind — NO SO_REUSEADDR/SO_REUSEPORT (Windows: SO_EXCLUSIVEADDRUSE)
//     so a second process / overlapping sign-in cannot share/hijack the socket.
//   * BIND-BEFORE-BROWSER — Bind() returns a confirmed-listening port; the caller
//     opens the system browser ONLY after Bind() succeeds. The auth URL is not
//     even computed earlier.
//   * Port range [PortMin,PortMax] tried in random order, each at most once;
//     range EXHAUSTED -> Bind() FAILS CLOSED (-1). NEVER a fallback to 0.0.0.0 or
//     an out-of-range port.
//   * One path only: GET /cb. Everything else 404s and does NOT consume the one-shot.
//   * ONE-SHOT: the first /cb is served and the listener stops.
//   * Timeout that REJECTS (not merely closes): AwaitCode returns bTimedOut=true so
//     the caller never hangs (the desktop f3a3ae2 fix, replicated).
//   * NO loopback `state`: PKCE S256 is the control on the fixed /cb hop (§2.1 / F2).
//
// The one-time `code` is NEVER logged by this class.
//
// Deliberately uses <winsock2.h>/BSD sockets rather than UE's HttpServer module:
// UE's module cannot be exercised in a standalone MSVC harness, and the review
// gate requires the bind-before-browser / range-exhaustion / timeout-rejects
// behaviour to be UNIT-TESTED. The request surface is a single one-shot GET whose
// query is parsed with a bounded, defensive scanner (never a general HTTP parser).
#pragma once

#include <string>

namespace BranchiveLore
{
	struct FLoopbackResult
	{
		bool        bOk = false;      // a one-time `code` was received on /cb
		std::string Code;             // the code (only meaningful when bOk) — never logged
		std::string Error;            // human-readable failure reason (safe to show/log)
		bool        bTimedOut = false;// the timeout fired — REJECT, treat as failure
	};

	class FLoopbackListener
	{
	public:
		FLoopbackListener(int InPortMin, int InPortMax);
		~FLoopbackListener();

		FLoopbackListener(const FLoopbackListener&) = delete;
		FLoopbackListener& operator=(const FLoopbackListener&) = delete;

		// Bind 127.0.0.1 exclusively on a free port in [PortMin,PortMax] and START
		// listening. Returns the bound port, or -1 on range exhaustion (FAIL CLOSED —
		// never a fallback). MUST be called (successfully) BEFORE opening the browser.
		int Bind();

		// The confirmed-bound port (valid only after Bind() returns > 0).
		int Port() const { return BoundPort; }

		// Block until the one-time /cb code arrives, an ?error= redirect, or TimeoutMs
		// elapses. One-shot; always Stop()s before returning. A timeout REJECTS.
		FLoopbackResult AwaitCode(int TimeoutMs);

		// Idempotently stop the listener. Safe to call multiple times.
		void Stop();

	private:
		int PortMin;
		int PortMax;
		int BoundPort = -1;

		// Platform socket handle stored width-safe as int64 (Winsock SOCKET is a
		// pointer-width handle; a POSIX fd is an int). -1 == none.
		long long ListenSock = -1;
	};
}
