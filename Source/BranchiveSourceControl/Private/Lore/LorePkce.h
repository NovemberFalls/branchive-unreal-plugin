// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Engine-independent PKCE S256 primitives (RFC 7636) for Branchive Cloud
// sign-in (docs/INTEGRATIONS-AUTH-PKCE.md §2.4, §0). Pure C++/std — NO Unreal
// Engine dependency — so the exact same translation unit that ships in the UE
// module is compiled + run in the standalone MSVC/g++ harness against the
// canonical RFC 7636 Appendix-B test vector (Tests/standalone/auth_test.cpp).
//
// SECURITY:
//  - The `code_verifier` is a SECRET. It MUST NEVER be logged and the whole
//    point of PKCE is that it never leaves the process that generated it (it is
//    sent only to the BFF's POST /auth/token, over TLS in production).
//  - The RNG is NOT here: this file only ENCODES caller-supplied random bytes
//    (EncodeVerifier) and DERIVES the (non-secret) S256 challenge. The
//    crypto-strong RNG lives in the platform glue (OS CSPRNG) so this file stays
//    pure/testable while the entropy source stays a real CSPRNG in the editor.
//
// Keep this file free of <regex> and of any identifier UE turns into a macro
// (TEXT/check/min/max/PI/TRUE/FALSE...) so it is unity-safe in the UE module.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <array>

namespace BranchiveLore
{
	// base64url (RFC 4648 §5) WITHOUT padding — the encoding both the verifier and
	// the S256 challenge use in RFC 7636.
	std::string Base64UrlNoPad(const uint8_t* Data, size_t Len);

	// SHA-256 of an arbitrary byte buffer. Returns the 32-byte digest.
	std::array<uint8_t, 32> Sha256(const uint8_t* Data, size_t Len);

	// Encode caller-supplied random bytes into a PKCE `code_verifier`:
	// base64url-no-pad(bytes). 32 random bytes -> 43 chars, inside RFC 7636's
	// 43..128 range. The bytes MUST come from a crypto-strong RNG (OS CSPRNG).
	std::string EncodeVerifier(const uint8_t* RandomBytes, size_t Len);

	// Derive the S256 `code_challenge` from a `code_verifier`:
	// base64url-no-pad(SHA-256(ASCII(verifier))) — RFC 7636 §4.2. The challenge is
	// NOT secret (it travels in the browser auth URL); only the verifier is.
	std::string DeriveChallengeS256(const std::string& Verifier);
}
