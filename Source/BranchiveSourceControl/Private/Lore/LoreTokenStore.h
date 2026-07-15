// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Engine-independent token-at-rest INTERFACE (docs/INTEGRATIONS-AUTH-PKCE.md §2.4).
// The Branchive Bearer is a secret; it is NEVER logged and never written to a
// plaintext file (see the platform impls in the UE module: DPAPI on Windows,
// Keychain on macOS, non-persistent-by-default on Linux per viktor's binding
// condition). This header is the seam that makes the sign-in flow unit-testable
// without a booted editor — a headless test injects FInMemoryTokenStore.
#pragma once

#include <string>

namespace BranchiveLore
{
	// The Branchive Bearer at rest. Store(...) replaces any previous value;
	// Load() returns "" when none; Clear() erases it.
	class ILoreTokenStore
	{
	public:
		virtual ~ILoreTokenStore() = default;
		virtual void Store(const std::string& Token) = 0;
		virtual std::string Load() = 0;
		virtual void Clear() = 0;

		// True when this store actually persists across process restarts. Linux may
		// return false (non-persistent fail-safe): the UI can then tell the user a
		// re-auth is needed next session, and headless/CI targets never write a token.
		virtual bool IsPersistent() const = 0;
	};

	// Headless test double — in-memory only. NEVER used in production.
	class FInMemoryTokenStore : public ILoreTokenStore
	{
	public:
		void Store(const std::string& Token) override { Value = Token; bHas = true; }
		std::string Load() override { return bHas ? Value : std::string(); }
		void Clear() override { Value.clear(); bHas = false; }
		bool IsPersistent() const override { return false; }

	private:
		std::string Value;
		bool bHas = false;
	};
}
