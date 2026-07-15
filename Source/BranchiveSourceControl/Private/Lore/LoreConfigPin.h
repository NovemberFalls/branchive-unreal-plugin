// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Engine-independent workspace identity-pin primitives (docs/INTEGRATIONS-AUTH-PKCE.md
// §4.2/§4.3). Pure C++/std — the exact TU the UE module ships is also compiled +
// run standalone (Tests/standalone/auth_test.cpp).
//
// After a `lore login` under the plugin's own account we rebind the workspace's
// `.lore/config.toml` top-level `identity = "<sub>"` pin so ops attribute to the
// signed-in user (never a stale ambient identity).
//
// F3 (zara): the CLI reads `identity` as a TOP-LEVEL key. A line placed after a
// `[table]` header parses as `<table>.identity` — an unknown key the CLI silently
// drops, so the pin becomes a no-op (wrong-account attribution on a multi-account
// machine). RebindIdentityLine therefore inserts a NEW pin right AFTER the
// top-level `remote_url` line (which always precedes any table), never at EOF
// inside a table. Line-based, EOL-preserving, no cross-line edit.
#pragma once

#include <string>

namespace BranchiveLore
{
	// Decode the `sub` claim from a Lore JWT WITHOUT verifying its signature (we
	// only need the subject to rebind the pin; authenticity is loreserver's job).
	// Returns "" on a malformed token or a missing/blank string `sub`. NEVER logs
	// the token; only the (non-secret) sub is returned.
	std::string DecodeJwtSub(const std::string& Jwt);

	// Rebind ConfigText's top-level `identity = "<sub>"` line to Sub, returning the
	// new file text. Rewrites ONLY a top-level (pre-first-[table]) identity line,
	// preserves the file's EOL style (CRLF vs LF), and — when no identity line
	// exists — INSERTS one right after the top-level `remote_url` line (F3), else
	// before the first `[section]`, else at EOF (safe only when there are no
	// tables). A no-op (returns ConfigText unchanged) when the pin already equals
	// Sub, or when Sub is empty. Pure/total — never touches the filesystem.
	std::string RebindIdentityLine(const std::string& ConfigText, const std::string& Sub);
}
