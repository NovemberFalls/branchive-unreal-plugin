// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Per-platform security primitives for Branchive Cloud sign-in (auth spec §2.4):
//   * BranchiveCryptoRandomBytes — crypto-strong RNG for the PKCE verifier
//     (Windows BCryptGenRandom / Apple SecRandomCopyBytes / Linux /dev/urandom).
//   * MakePlatformTokenStore — token-at-rest, per-OS:
//       Windows: DPAPI (CryptProtectData/CryptUnprotectData).
//       macOS:   Keychain Services (Security.framework).
//       Linux:   FAIL-SAFE to NON-PERSISTENT (re-auth per session) unless an
//                administrator explicitly opts in — never a plaintext file with a
//                mere warning on a shared/headless box (viktor's binding condition).
#pragma once

#include "CoreMinimal.h"
#include "Lore/LoreTokenStore.h"

#include <memory>

/**
 * Fill Buffer with Count crypto-strong random bytes. Returns false if the OS CSPRNG
 * is unavailable (the caller MUST then abort sign-in — never fall back to a weak RNG
 * for the PKCE verifier).
 */
bool BranchiveCryptoRandomBytes(uint8* Buffer, int32 Count);

/**
 * The platform token-at-rest store. Never null — on a platform/context where secure
 * persistence can't be guaranteed (Linux without an explicit admin opt-in), returns a
 * NON-PERSISTENT (in-memory) store so a bearer never lands unprotected on disk.
 */
std::unique_ptr<BranchiveLore::ILoreTokenStore> MakePlatformTokenStore();
