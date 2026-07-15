// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "LoreConfigPin.h"

#include <vector>

namespace
{
	// base64url (and tolerant base64) decode. Returns bytes; ignores '='; maps '-'/'+'
	// and '_'/'/' both ways. On any invalid char, stops (lenient — a JWT payload is
	// base64url and well-formed, but we never throw).
	std::string B64UrlDecode(const std::string& In)
	{
		auto Val = [](char c) -> int
		{
			if (c >= 'A' && c <= 'Z') return c - 'A';
			if (c >= 'a' && c <= 'z') return c - 'a' + 26;
			if (c >= '0' && c <= '9') return c - '0' + 52;
			if (c == '-' || c == '+') return 62;
			if (c == '_' || c == '/') return 63;
			return -1;
		};
		std::string Out;
		int Buf = 0, Bits = 0;
		for (char c : In)
		{
			if (c == '=' ) break;
			const int V = Val(c);
			if (V < 0) break;
			Buf = (Buf << 6) | V;
			Bits += 6;
			if (Bits >= 8)
			{
				Bits -= 8;
				Out.push_back(char((Buf >> Bits) & 0xFF));
			}
		}
		return Out;
	}

	// Extract a top-level JSON string value for Key from Json (naive scan, good enough
	// for a JWT payload's "sub"). Returns "" if not found or not a string. Handles the
	// common `\"` and `\\` escapes in the value.
	std::string JsonStringField(const std::string& Json, const std::string& Key)
	{
		const std::string Needle = "\"" + Key + "\"";
		size_t p = Json.find(Needle);
		if (p == std::string::npos) return std::string();
		p += Needle.size();
		// skip whitespace + ':'
		while (p < Json.size() && (Json[p] == ' ' || Json[p] == '\t' || Json[p] == '\r' || Json[p] == '\n')) ++p;
		if (p >= Json.size() || Json[p] != ':') return std::string();
		++p;
		while (p < Json.size() && (Json[p] == ' ' || Json[p] == '\t' || Json[p] == '\r' || Json[p] == '\n')) ++p;
		if (p >= Json.size() || Json[p] != '"') return std::string(); // only string subs
		++p;
		std::string Out;
		while (p < Json.size())
		{
			const char c = Json[p++];
			if (c == '\\' && p < Json.size())
			{
				const char e = Json[p++];
				switch (e)
				{
				case '"': Out.push_back('"'); break;
				case '\\': Out.push_back('\\'); break;
				case '/': Out.push_back('/'); break;
				case 'n': Out.push_back('\n'); break;
				case 'r': Out.push_back('\r'); break;
				case 't': Out.push_back('\t'); break;
				default: Out.push_back(e); break;
				}
			}
			else if (c == '"')
			{
				return Out;
			}
			else
			{
				Out.push_back(c);
			}
		}
		return std::string();
	}

	std::string TrimWs(const std::string& S)
	{
		size_t a = 0, b = S.size();
		while (a < b && (S[a] == ' ' || S[a] == '\t' || S[a] == '\r' || S[a] == '\n')) ++a;
		while (b > a && (S[b - 1] == ' ' || S[b - 1] == '\t' || S[b - 1] == '\r' || S[b - 1] == '\n')) --b;
		return S.substr(a, b - a);
	}

	// H1 (zara): allowlist for a JWT `sub` we are about to embed VERBATIM into a TOML
	// string (identity = "<sub>"). Matches ^[A-Za-z0-9._@|:-]+$ — the charset real subs
	// use (opaque ids, "provider|id" forms, email-like) and NONE of the TOML-breaking
	// characters (quote, backslash, newline, '=', brackets, whitespace). Empty is rejected.
	bool IsAllowedSub(const std::string& S)
	{
		if (S.empty()) return false;
		for (unsigned char c : S)
		{
			const bool ok =
				(c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				(c >= '0' && c <= '9') ||
				c == '.' || c == '_' || c == '@' || c == '|' || c == ':' || c == '-';
			if (!ok) return false;
		}
		return true;
	}

	// A top-level `identity = "value"` assignment: capture the current value into Out.
	bool MatchTopLevelIdentity(const std::string& Raw, std::string& Out)
	{
		const std::string L = TrimWs(Raw);
		const std::string Key = "identity";
		if (L.size() < Key.size()) return false;
		if (L.compare(0, Key.size(), Key) != 0) return false;
		size_t p = Key.size();
		while (p < L.size() && (L[p] == ' ' || L[p] == '\t')) ++p;
		if (p >= L.size() || L[p] != '=') return false;
		++p;
		while (p < L.size() && (L[p] == ' ' || L[p] == '\t')) ++p;
		if (p >= L.size() || L[p] != '"') return false;
		++p;
		std::string Val;
		while (p < L.size() && L[p] != '"')
		{
			Val.push_back(L[p]);
			++p;
		}
		if (p >= L.size() || L[p] != '"') return false;
		Out = Val;
		return true;
	}

	bool MatchTopLevelRemoteUrl(const std::string& Raw)
	{
		const std::string L = TrimWs(Raw);
		const std::string Key = "remote_url";
		if (L.compare(0, Key.size(), Key) != 0) return false;
		size_t p = Key.size();
		while (p < L.size() && (L[p] == ' ' || L[p] == '\t')) ++p;
		return p < L.size() && L[p] == '=';
	}

	// Split into lines WITHOUT keeping terminators; the caller re-joins with a single
	// detected EOL (TOML is line-oriented and we only ever touch one line).
	std::vector<std::string> SplitLinesNoTerm(const std::string& Text)
	{
		std::vector<std::string> Lines;
		std::string Cur;
		for (size_t i = 0; i < Text.size(); ++i)
		{
			const char c = Text[i];
			if (c == '\n')
			{
				Lines.push_back(Cur);
				Cur.clear();
			}
			else if (c == '\r')
			{
				Lines.push_back(Cur);
				Cur.clear();
				if (i + 1 < Text.size() && Text[i + 1] == '\n') ++i; // CRLF
			}
			else
			{
				Cur.push_back(c);
			}
		}
		Lines.push_back(Cur); // trailing element (empty if the file ended with an EOL)
		return Lines;
	}

	std::string Join(const std::vector<std::string>& Lines, const std::string& Eol)
	{
		std::string Out;
		for (size_t i = 0; i < Lines.size(); ++i)
		{
			if (i) Out += Eol;
			Out += Lines[i];
		}
		return Out;
	}
}

namespace BranchiveLore
{
	std::string DecodeJwtSub(const std::string& Jwt)
	{
		// header.payload.signature — we want the payload (index 1).
		size_t d1 = Jwt.find('.');
		if (d1 == std::string::npos) return std::string();
		size_t d2 = Jwt.find('.', d1 + 1);
		const std::string Payload = (d2 == std::string::npos)
			? Jwt.substr(d1 + 1)
			: Jwt.substr(d1 + 1, d2 - d1 - 1);
		if (Payload.empty()) return std::string();
		const std::string Json = B64UrlDecode(Payload);
		if (Json.empty()) return std::string();
		return TrimWs(JsonStringField(Json, "sub"));
	}

	std::string RebindIdentityLine(const std::string& ConfigText, const std::string& Sub)
	{
		// H1 (defense-in-depth): the sub is embedded verbatim into `identity = "<sub>"`. A
		// hostile/malformed sub containing a quote or newline could break out of the TOML
		// string and inject top-level keys (e.g. override remote_url). Allowlist the charset
		// and NO-OP the rebind (write nothing) on any violation. The normal path is safe —
		// the sub is minted by our TLS BFF — so this only ever rejects a tampered token.
		// (Also rejects empty, preserving the prior empty-sub no-op contract.)
		if (!IsAllowedSub(Sub)) return ConfigText;

		const std::string Eol = (ConfigText.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
		const std::string NewLine = std::string("identity = \"") + Sub + "\"";

		std::vector<std::string> Lines = SplitLinesNoTerm(ConfigText);

		bool bInTable = false;
		int ReplacedIndex = -1;
		bool bAlreadyCorrect = false;
		int RemoteUrlIndex = -1;  // first top-level remote_url (precedes any table)
		int FirstTableIndex = -1; // first [section]

		for (int idx = 0; idx < (int)Lines.size(); ++idx)
		{
			const std::string& Raw = Lines[idx];
			const std::string L = TrimWs(Raw);
			if (!L.empty() && L[0] == '[')
			{
				if (FirstTableIndex < 0) FirstTableIndex = idx;
				bInTable = true;
				continue;
			}
			if (bInTable) continue; // only top-level (pre-first-table) keys are the pin
			if (RemoteUrlIndex < 0 && MatchTopLevelRemoteUrl(Raw))
			{
				RemoteUrlIndex = idx;
			}
			std::string CurVal;
			if (ReplacedIndex < 0 && MatchTopLevelIdentity(Raw, CurVal))
			{
				ReplacedIndex = idx;
				if (CurVal == Sub) bAlreadyCorrect = true;
			}
		}

		if (bAlreadyCorrect) return ConfigText;

		if (ReplacedIndex >= 0)
		{
			Lines[ReplacedIndex] = NewLine;
			return Join(Lines, Eol);
		}

		// APPEND (F3): insert into the TOP-LEVEL region — after remote_url, else before
		// the first table, else (no tables) at EOF.
		int InsertAt = -1;
		if (RemoteUrlIndex >= 0) InsertAt = RemoteUrlIndex + 1;
		else if (FirstTableIndex >= 0) InsertAt = FirstTableIndex;

		if (InsertAt < 0)
		{
			// No table headers anywhere: EOF is in the top-level region. Guard against a
			// stray trailing empty split element so we don't introduce a blank line.
			std::string Tail = ConfigText;
			while (!Tail.empty() && (Tail.back() == '\r' || Tail.back() == '\n')) Tail.pop_back();
			if (Tail.empty()) return NewLine + Eol;
			return Tail + Eol + NewLine + Eol;
		}

		Lines.insert(Lines.begin() + InsertAt, NewLine);
		return Join(Lines, Eol);
	}
}
