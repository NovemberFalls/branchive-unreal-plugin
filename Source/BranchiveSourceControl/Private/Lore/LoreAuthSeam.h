// Copyright Branchive.
//
// ============================================================================
// AUTH SEAM (Wave-2b) — DOCUMENTED STUB. No listener is opened this run.
// ============================================================================
//
// The core source-control operations in this plugin run against the *ambient*
// `lore` CLI auth store (whatever identity the machine is already logged in as,
// exactly like Branchive Desktop) and do NOT depend on any in-plugin sign-in.
// Cloud PKCE sign-in is a later wave. This header captures the exact contract
// the sign-in code MUST honor when it is built, per docs/INTEGRATIONS-AUTH-PKCE.md,
// and implements the ONE piece that is a pure string check and is
// security-critical enough to land now: the isCloudRemoteUrl gate (§4.0).
//
// ---- Wave-2b PKCE flow (spec §0–§2, NOT implemented here) -----------------
//  - System browser (FPlatformProcess::LaunchURL) + a ONE-SHOT loopback HTTP
//    listener on 127.0.0.1 (or [::1]) in the UE5-assigned port range
//    47500–47599 (spec §2.1/§2.4). Recommended host: UE's own HTTPServer module
//    (Runtime/HTTPServer), not hand-rolled FSocket accept/parse.
//  - Bind-then-launch: the listener MUST be confirmed listening BEFORE the auth
//    URL is opened. Exclusive-address bind (NO SO_REUSEADDR/SO_REUSEPORT).
//    EADDRINUSE -> retry within 47500–47599, then FAIL CLOSED (never bind
//    0.0.0.0 or a port outside the range). 45s timeout that REJECTS the pending
//    code future (not merely closes the socket — this was a real shipped bug).
//  - Token-at-rest is per-platform C++:
//      * Windows: DPAPI (CryptProtectData/CryptUnprotectData) under
//        #if PLATFORM_WINDOWS — parity with Electron safeStorage.
//      * macOS:   Keychain Services (Security.framework) under #if PLATFORM_MAC.
//      * Linux:   SPLIT BY DEPLOYMENT CONTEXT (viktor M4/OQ3, BINDING):
//          - interactive single-user desktop: 0600 file + undismissable warning
//            is acceptable for v1.
//          - headless / CI / multi-tenant: a warning is NOT sufficient. MUST
//            fail-safe to NON-PERSISTENT storage (re-auth every session) OR an
//            explicit admin-driven opt-in. Plaintext/obfuscated on disk is
//            never acceptable. When single-user-interactive context cannot be
//            confirmed, DEFAULT to non-persistent.
//  - Session longevity: reuse Desktop's TTL/3 background Lore-JWT refresh timer
//    (spec §3.4). Never auto-launch the browser from that timer.
//
// ---- The pattern, ONLY safe when IsCloudRemoteUrl(remoteUrl) is true -------
//   1. plugin holds a valid Branchive Bearer.
//   2. IsCloudRemoteUrl(remoteUrl) MUST be true — else STOP (no lore-token, no login).
//   3. POST /auth/lore-token { remoteUrl } -> { token, authUrl, expiresIn }.
//   4. lore login <remoteUrl> --auth-url <authUrl> --token-type lore
//        --token <JWT> --non-interactive        (execFile-style, never a shell)
//   5. proceed with the actual lore op.
#pragma once

#include <string>

namespace BranchiveLore
{
	// The known Branchive Cloud host. When Cloud sign-in ships this should be the
	// same literal BRANCHIVE_CLOUD_HOST the Desktop client uses.
	// (Left as a compile-time seam; the value is not load-bearing for the
	// core ops, which run against the ambient auth store.)
	inline const char* BranchiveCloudHost() { return "vcs.branchive.io"; }

	// ---------------------------------------------------------------------------
	// SECURITY-CRITICAL, MUST-REPLICATE (auth spec §4.0, zara H1).
	//
	// EXACT-HOST match against the known Branchive Cloud host. Never endsWith,
	// never includes/substring, never an unanchored regex. This is what prevents
	// the plugin from ever handing a real, victim-identity JWT to whatever server
	// a (possibly hostile) workspace's .lore/config.toml remote_url points at.
	//
	// Returns true ONLY when remoteUrl is lore://<CloudHost>[:port][/...].
	// A false result means: do NOTHING auth-related (self-hosted / local / hostile).
	// ---------------------------------------------------------------------------
	inline bool IsCloudRemoteUrl(const std::string& RemoteUrl, const std::string& CloudHost)
	{
		// Parse the host out of lore://HOST[:port][/...], case-insensitively.
		// Mirrors Desktop's /^lore:\/\/([^/:]+)/i then exact lowercase compare.
		const std::string Scheme = "lore://";
		if (RemoteUrl.size() < Scheme.size()) return false;
		// case-insensitive scheme check
		for (size_t i = 0; i < Scheme.size(); ++i)
		{
			char a = RemoteUrl[i];
			if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
			if (a != Scheme[i]) return false;
		}
		std::string Host;
		for (size_t i = Scheme.size(); i < RemoteUrl.size(); ++i)
		{
			char c = RemoteUrl[i];
			if (c == '/' || c == ':') break;
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			Host.push_back(c);
		}
		std::string Want;
		for (char c : CloudHost)
		{
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			Want.push_back(c);
		}
		return !Host.empty() && Host == Want; // EXACT host equality
	}
}
