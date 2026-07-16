// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Standalone unit test for the engine-independent Branchive Cloud sign-in core
// (docs/INTEGRATIONS-AUTH-PKCE.md §2.4). Compiles WITHOUT Unreal Engine, against
// the EXACT same LorePkce.cpp / LoreLoopback.cpp / LoreConfigPin.cpp translation
// units the UE module builds, plus the header-only LoreAuthSeam.h / LoreTokenStore.h.
//
// Build+run:
//   integrations/unreal/Tests/standalone/run-auth.ps1   (MSVC cl.exe)
//   integrations/unreal/Tests/standalone/run-auth.sh     (g++/clang)
//
// Covers exactly what the review gate requires without a live BFF:
//   * PKCE S256 (RFC 7636 Appendix-B test vector) + base64url-no-pad.
//   * isCloudRemoteUrl accept/reject INCLUDING the userinfo (F1) cases.
//   * loopback bind-before-browser + range-exhaustion->fail + timeout-rejects + a
//     real /cb?code round-trip over a local socket.
//   * token-store round-trip behind the injectable ILoreTokenStore interface.
//   * workspace-pin rebind (F3) + JWT-sub decode.
//   * (Windows only) a DPAPI encrypt/decrypt round-trip — the at-rest primitive the
//     UE Windows token store uses.
#include "LorePkce.h"
#include "LoreAuthSeam.h"
#include "LoreConfigPin.h"
#include "LoreTokenStore.h"
#include "LoreLoopback.h"
#include "LoreBinaryResolve.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef _WINSOCKAPI_
		#define _WINSOCKAPI_
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <windows.h>
	#include <wincrypt.h>
	#pragma comment(lib, "ws2_32.lib")
	#pragma comment(lib, "crypt32.lib")
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <unistd.h>
#endif

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool cond, const std::string& what)
{
	if (cond) { ++g_pass; }
	else { ++g_fail; std::printf("  FAIL: %s\n", what.c_str()); }
}

// Send a one-line GET to 127.0.0.1:port and read a bit of the response (best-effort).
static void ClientGet(int port, const std::string& target)
{
#if defined(_WIN32)
	WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
	int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0)
	{
		std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
		send(s, req.data(), (int)req.size(), 0);
		char buf[512];
		recv(s, buf, sizeof(buf), 0); // drain so the server's send completes
	}
#if defined(_WIN32)
	closesocket(s); WSACleanup();
#else
	close(s);
#endif
}

int main()
{
	using namespace BranchiveLore;
	const std::string host = "vcs.branchive.io";

	// ---- PKCE S256: RFC 7636 Appendix-B canonical vector -------------------
	{
		std::printf("pkce/rfc7636-appendix-b\n");
		const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
		const std::string challenge = DeriveChallengeS256(verifier);
		Check(challenge == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
			"S256 challenge matches RFC 7636 Appendix-B vector");

		// base64url-no-pad known vectors ("" -> "", "f" -> "Zg", "fo" -> "Zm8", "foo" -> "Zm9v").
		Check(Base64UrlNoPad(nullptr, 0) == "", "b64url empty");
		Check(Base64UrlNoPad((const uint8_t*)"f", 1) == "Zg", "b64url 'f'");
		Check(Base64UrlNoPad((const uint8_t*)"fo", 2) == "Zm8", "b64url 'fo'");
		Check(Base64UrlNoPad((const uint8_t*)"foo", 3) == "Zm9v", "b64url 'foo'");
		Check(Base64UrlNoPad((const uint8_t*)"foob", 4) == "Zm9vYg", "b64url 'foob'");

		// SHA-256("abc") known digest.
		auto d = Sha256((const uint8_t*)"abc", 3);
		static const uint8_t want[32] = {
			0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
			0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
		Check(std::memcmp(d.data(), want, 32) == 0, "SHA-256('abc') matches FIPS-180 vector");

		// EncodeVerifier: 32 bytes -> 43 chars, base64url charset, no padding.
		std::vector<uint8_t> rnd(32, 0);
		for (int i = 0; i < 32; ++i) rnd[i] = (uint8_t)(i * 7 + 1);
		const std::string v = EncodeVerifier(rnd.data(), rnd.size());
		Check(v.size() == 43, "verifier from 32 bytes is 43 chars (RFC 43..128)");
		bool charsetOk = true, hasPad = false;
		for (char c : v)
		{
			const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
			if (!ok) charsetOk = false;
			if (c == '=') hasPad = true;
		}
		Check(charsetOk && !hasPad, "verifier is base64url with no padding");
	}

	// ---- isCloudRemoteUrl: exact-host gate incl. userinfo (F1) --------------
	{
		std::printf("isCloudRemoteUrl gate (incl. userinfo F1)\n");
		// accepts
		Check(IsCloudRemoteUrl("lore://vcs.branchive.io", host), "exact host -> true");
		Check(IsCloudRemoteUrl("lore://vcs.branchive.io:41337", host), "host with port -> true");
		Check(IsCloudRemoteUrl("lore://vcs.branchive.io/some/repo", host), "host with path -> true");
		Check(IsCloudRemoteUrl("LORE://VCS.BRANCHIVE.IO:41337/x", host), "case-insensitive -> true");

		// rejects: lookalikes a substring/endsWith check would wrongly accept
		Check(!IsCloudRemoteUrl("lore://evil.com:41337", host), "evil host -> false");
		Check(!IsCloudRemoteUrl("lore://vcs.branchive.io.evil.com", host), "dot-suffix host -> false");
		Check(!IsCloudRemoteUrl("lore://notvcs.branchive.io", host), "prefix host -> false");
		Check(!IsCloudRemoteUrl("lore://evilvcs.branchive.io", host), "prefix host 2 -> false");
		Check(!IsCloudRemoteUrl("lore://xvcs.branchive.iox", host), "substring host -> false");

		// rejects: the userinfo smuggle (F1) — the whole point of this task item.
		Check(!IsCloudRemoteUrl("lore://vcs.branchive.io:41337@evil.com", host),
			"userinfo smuggle host:port@evil -> false");
		Check(!IsCloudRemoteUrl("lore://vcs.branchive.io@evil.com", host),
			"userinfo host@evil -> false");
		Check(!IsCloudRemoteUrl("lore://user@vcs.branchive.io", host),
			"userinfo user@cloud -> false (we never emit userinfo)");
		Check(!IsCloudRemoteUrl("lore://user:pass@vcs.branchive.io:41337/r", host),
			"userinfo user:pass@cloud -> false");

		// rejects: scheme / empty / malformed / blank configured host (fail closed)
		Check(!IsCloudRemoteUrl("https://vcs.branchive.io", host), "wrong scheme -> false");
		Check(!IsCloudRemoteUrl("", host), "empty -> false");
		Check(!IsCloudRemoteUrl("lore://", host), "no authority -> false");
		Check(!IsCloudRemoteUrl("lore:///path-no-host", host), "no host -> false");
		Check(!IsCloudRemoteUrl("lore:vcs.branchive.io", host), "no // -> false");
		Check(!IsCloudRemoteUrl("lore://vcs.branchive.io", ""), "blank cloud host -> false");
		Check(!IsCloudRemoteUrl("lore://vcs.branchive.io", "   "), "whitespace cloud host -> false");
	}

	// ---- F-BIN: only an ABSOLUTE binary path may be spawned ----------------
	{
		std::printf("binary path gate (F-BIN): bare/relative refused, absolute accepted\n");
		// REFUSED — a bare/relative image name would resolve against the spawn cwd
		// (= a workspace) on Windows, so a repo-committed lore.exe could run with the JWT.
		Check(!IsAbsoluteBinaryPath("lore.exe"),       "bare 'lore.exe' -> refused");
		Check(!IsAbsoluteBinaryPath("lore"),           "bare 'lore' -> refused");
		Check(!IsAbsoluteBinaryPath("./lore"),         "dot-relative './lore' -> refused");
		Check(!IsAbsoluteBinaryPath(".\\lore.exe"),    "dot-relative '.\\lore.exe' -> refused");
		Check(!IsAbsoluteBinaryPath("sub/dir/lore"),   "relative subpath -> refused");
		Check(!IsAbsoluteBinaryPath("sub\\lore.exe"),  "relative backslash subpath -> refused");
		Check(!IsAbsoluteBinaryPath("C:lore.exe"),     "drive-RELATIVE 'C:lore.exe' -> refused");
		Check(!IsAbsoluteBinaryPath("\\lore.exe"),     "rootless leading-backslash -> refused");
		Check(!IsAbsoluteBinaryPath(""),               "empty -> refused");
		// ACCEPTED — absolute forms (Windows drive, forward-slash drive, UNC, POSIX).
		Check(IsAbsoluteBinaryPath("C:\\Tools\\lore.exe"),      "drive-absolute backslash -> accepted");
		Check(IsAbsoluteBinaryPath("C:/Tools/lore.exe"),        "drive-absolute forward-slash -> accepted");
		Check(IsAbsoluteBinaryPath("\\\\srv\\share\\lore.exe"), "UNC -> accepted");
		Check(IsAbsoluteBinaryPath("/usr/local/bin/lore"),      "POSIX-absolute -> accepted");
	}

	// ---- loopback: bind-before-browser + range-exhaustion + timeout + code --
	{
		std::printf("loopback: bind-before-browser\n");
		FLoopbackListener r(47500, 47599);
		const int port = r.Bind();
		Check(port >= 47500 && port <= 47599, "bind returns a confirmed-listening port in the UE range");
		// The socket is accepting connections the instant Bind() returns -> safe to open the browser.
		Check(r.Port() == port, "Port() reflects the bound port");
		r.Stop();
	}

	{
		std::printf("loopback: range exhaustion FAILS closed (no fallback)\n");
		FLoopbackListener first(47500, 47599);
		const int taken = first.Bind();
		Check(taken > 0, "first listener bound");
		// A second listener constrained to exactly the taken port must FAIL to bind
		// (exclusive bind, no SO_REUSEADDR) rather than silently binding elsewhere/0.0.0.0.
		FLoopbackListener second(taken, taken);
		const int p2 = second.Bind();
		Check(p2 == -1, "single-port range already in use -> Bind() returns -1 (fail closed)");
		first.Stop();
	}

	{
		std::printf("loopback: timeout REJECTS (does not hang)\n");
		FLoopbackListener r(47500, 47599);
		const int port = r.Bind();
		Check(port > 0, "bound for timeout test");
		const auto t0 = std::chrono::steady_clock::now();
		FLoopbackResult res = r.AwaitCode(200); // no callback ever arrives
		const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - t0).count();
		Check(res.bTimedOut && !res.bOk, "no callback -> bTimedOut, not bOk (REJECT)");
		Check(elapsedMs < 5000, "timeout returned promptly (did not hang)");
	}

	{
		std::printf("loopback: happy path /cb?code round-trip\n");
		FLoopbackListener r(47500, 47599);
		const int port = r.Bind();
		Check(port > 0, "bound for happy-path test");
		std::thread client([port]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			ClientGet(port, "/cb?code=the-code-42");
		});
		FLoopbackResult res = r.AwaitCode(5000);
		client.join();
		Check(res.bOk && !res.bTimedOut, "code callback -> bOk");
		Check(res.Code == "the-code-42", "the one-time code is parsed from /cb?code=");
	}

	{
		std::printf("loopback: ?error= is rejected; non-/cb 404s (no one-shot consumed)\n");
		FLoopbackListener r(47500, 47599);
		const int port = r.Bind();
		Check(port > 0, "bound for error test");
		std::thread client([port]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			ClientGet(port, "/nope?code=x");                // 404, does NOT consume the one-shot
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			ClientGet(port, "/cb?error=access_denied");     // the real callback: an error
		});
		FLoopbackResult res = r.AwaitCode(5000);
		client.join();
		Check(!res.bOk && !res.bTimedOut, "error redirect -> not ok, not timeout");
		Check(res.Error.find("access_denied") != std::string::npos, "error reason surfaced");
	}

	// ---- token store: round-trip behind the injectable interface -----------
	{
		std::printf("token store: injectable interface round-trip\n");
		FInMemoryTokenStore store;
		ILoreTokenStore& iface = store; // exercised through the interface, like the real flow
		Check(iface.Load() == "", "empty before store");
		iface.Store("BEARER-XYZ");
		Check(iface.Load() == "BEARER-XYZ", "load returns the stored bearer");
		iface.Store("BEARER-2"); // replace
		Check(iface.Load() == "BEARER-2", "store replaces the previous value");
		iface.Clear();
		Check(iface.Load() == "", "clear erases the bearer");
		Check(iface.IsPersistent() == false, "in-memory store reports non-persistent");
	}

	// ---- workspace pin rebind (F3) + JWT sub decode ------------------------
	{
		std::printf("config pin: rebind (F3) + jwt sub decode\n");

		// A JWT with payload {"sub":"user-123","name":"x"} (header/sig are dummies).
		// payload b64url = eyJzdWIiOiJ1c2VyLTEyMyIsIm5hbWUiOiJ4In0
		const std::string jwt = "aaaa.eyJzdWIiOiJ1c2VyLTEyMyIsIm5hbWUiOiJ4In0.bbbb";
		Check(DecodeJwtSub(jwt) == "user-123", "decodeJwtSub extracts sub");
		Check(DecodeJwtSub("garbage") == "", "decodeJwtSub on malformed -> empty");
		Check(DecodeJwtSub("") == "", "decodeJwtSub on empty -> empty");

		// Existing top-level identity line is replaced in place, EOL preserved (CRLF).
		{
			const std::string cfg = "remote_url = \"lore://vcs.branchive.io\"\r\nidentity = \"old\"\r\n[remote]\r\nx = 1\r\n";
			const std::string out = RebindIdentityLine(cfg, "new-sub");
			Check(out.find("identity = \"new-sub\"") != std::string::npos, "identity replaced in place");
			Check(out.find("identity = \"old\"") == std::string::npos, "old identity gone");
			Check(out.find("\r\n") != std::string::npos, "CRLF EOL preserved");
			Check(out.find("[remote]") != std::string::npos, "table header preserved");
		}

		// No identity line: INSERT right after top-level remote_url, NEVER inside a table (F3).
		{
			const std::string cfg = "remote_url = \"lore://vcs.branchive.io\"\n[remote]\nfoo = \"bar\"\n";
			const std::string out = RebindIdentityLine(cfg, "sub-9");
			const size_t idPos = out.find("identity = \"sub-9\"");
			const size_t tablePos = out.find("[remote]");
			Check(idPos != std::string::npos, "identity inserted");
			Check(idPos < tablePos, "identity inserted ABOVE the [remote] table (F3: top-level, not nested)");
			const size_t remotePos = out.find("remote_url");
			Check(remotePos < idPos, "identity inserted AFTER remote_url");
		}

		// Already-correct pin -> no-op (unchanged text).
		{
			const std::string cfg = "remote_url = \"x\"\nidentity = \"same\"\n";
			Check(RebindIdentityLine(cfg, "same") == cfg, "already-correct pin is a no-op");
		}

		// Empty sub -> no-op (never write an empty pin).
		{
			const std::string cfg = "remote_url = \"x\"\n";
			Check(RebindIdentityLine(cfg, "") == cfg, "empty sub is a no-op");
		}

		// H1: a sub carrying a TOML-breakout char (quote / newline / backslash / '=' /
		// space) is rejected by the allowlist -> rebind is a NO-OP so it can't inject keys.
		{
			const std::string cfg  = "remote_url = \"lore://vcs.branchive.io\"\nidentity = \"old\"\n";
			const std::string evil = "x\"\nremote_url = \"lore://evil.example\"\n#";
			Check(RebindIdentityLine(cfg, evil) == cfg,            "sub with quote+newline -> no-op (H1)");
			Check(RebindIdentityLine(cfg, "has space") == cfg,    "sub with space -> no-op (H1)");
			Check(RebindIdentityLine(cfg, "back\\slash") == cfg,  "sub with backslash -> no-op (H1)");
			Check(RebindIdentityLine(cfg, "eq=sign") == cfg,      "sub with '=' -> no-op (H1)");
			Check(RebindIdentityLine(cfg, "br[ack]et") == cfg,    "sub with brackets -> no-op (H1)");
			// A legitimate sub charset (provider|id, punctuation, email-like) STILL rebinds.
			const std::string good = "google-oauth2|1234.56_7@user:tag-9";
			const std::string out  = RebindIdentityLine(cfg, good);
			Check(out.find("identity = \"" + good + "\"") != std::string::npos,
				"allowed sub charset still rebinds (H1)");
			Check(out.find("identity = \"old\"") == std::string::npos, "old identity gone (H1)");
		}
	}

#if defined(_WIN32)
	// ---- (Windows) DPAPI encrypt/decrypt round-trip — the at-rest primitive -
	{
		std::printf("token store: Windows DPAPI round-trip (at-rest primitive)\n");
		const std::string secret = "BEARER-super-secret-value-\xF0\x9F\x94\x92";
		DATA_BLOB in; in.pbData = (BYTE*)secret.data(); in.cbData = (DWORD)secret.size();
		DATA_BLOB out; std::memset(&out, 0, sizeof(out));
		const BOOL enc = CryptProtectData(&in, L"branchive-cloud-bearer", nullptr, nullptr, nullptr,
			CRYPTPROTECT_UI_FORBIDDEN, &out);
		Check(enc != 0, "CryptProtectData encrypts");
		if (enc)
		{
			// ciphertext must not contain the plaintext
			std::string cipher((char*)out.pbData, out.cbData);
			Check(cipher.find("BEARER-super-secret") == std::string::npos, "ciphertext hides the plaintext");
			DATA_BLOB dec; std::memset(&dec, 0, sizeof(dec));
			const BOOL ok = CryptUnprotectData(&out, nullptr, nullptr, nullptr, nullptr,
				CRYPTPROTECT_UI_FORBIDDEN, &dec);
			Check(ok != 0, "CryptUnprotectData decrypts");
			if (ok)
			{
				std::string round((char*)dec.pbData, dec.cbData);
				Check(round == secret, "DPAPI round-trip recovers the exact bearer");
				LocalFree(dec.pbData);
			}
			LocalFree(out.pbData);
		}
	}
#endif

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
