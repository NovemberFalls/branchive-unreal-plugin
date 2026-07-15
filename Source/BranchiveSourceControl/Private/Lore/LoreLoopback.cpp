// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "LoreLoopback.h"

#include <cstdint>
#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>
#include <random>

// --- platform sockets --------------------------------------------------------
#if defined(_WIN32)
	// Coexist with any (force-included) UE shared PCH: UE builds with
	// WIN32_LEAN_AND_MEAN so <windows.h> never drags in the legacy <winsock.h>;
	// we assert the same guards so <winsock2.h> is always the winsock header seen.
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef _WINSOCKAPI_
		#define _WINSOCKAPI_
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "ws2_32.lib")
	typedef SOCKET LoreSocket;
	static const LoreSocket kInvalidSock = INVALID_SOCKET;
#else
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <unistd.h>
	#include <errno.h>
	typedef int LoreSocket;
	static const LoreSocket kInvalidSock = -1;
#endif

namespace
{
#if defined(_WIN32)
	struct FWsaGuard
	{
		bool bOk = false;
		FWsaGuard()
		{
			WSADATA Data;
			bOk = (WSAStartup(MAKEWORD(2, 2), &Data) == 0);
		}
		~FWsaGuard() { if (bOk) WSACleanup(); }
	};
	inline void CloseSock(LoreSocket s) { if (s != kInvalidSock) closesocket(s); }
#else
	inline void CloseSock(LoreSocket s) { if (s != kInvalidSock) close(s); }
#endif

	inline LoreSocket AsSock(long long H) { return (LoreSocket)(intptr_t)H; }
	inline long long FromSock(LoreSocket S) { return (long long)(intptr_t)S; }

	std::string UrlDecode(const std::string& In)
	{
		std::string Out;
		Out.reserve(In.size());
		auto Hex = [](char c) -> int
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		for (size_t i = 0; i < In.size(); ++i)
		{
			const char c = In[i];
			if (c == '+')
			{
				Out.push_back(' ');
			}
			else if (c == '%' && i + 2 < In.size())
			{
				const int Hi = Hex(In[i + 1]);
				const int Lo = Hex(In[i + 2]);
				if (Hi >= 0 && Lo >= 0)
				{
					Out.push_back(char((Hi << 4) | Lo));
					i += 2;
				}
				else
				{
					Out.push_back(c);
				}
			}
			else
			{
				Out.push_back(c);
			}
		}
		return Out;
	}

	// Parse an application/x-www-form-urlencoded query into (last-value-wins) lookups
	// for exactly the two keys we care about. Bounded, defensive — never a general parser.
	void ParseQueryFor(const std::string& Query, std::string& OutCode, std::string& OutError)
	{
		size_t i = 0;
		while (i < Query.size())
		{
			size_t amp = Query.find('&', i);
			if (amp == std::string::npos) amp = Query.size();
			const std::string Pair = Query.substr(i, amp - i);
			const size_t eq = Pair.find('=');
			const std::string Key = (eq == std::string::npos) ? Pair : Pair.substr(0, eq);
			const std::string Val = (eq == std::string::npos) ? std::string() : Pair.substr(eq + 1);
			if (Key == "code") OutCode = UrlDecode(Val);
			else if (Key == "error") OutError = UrlDecode(Val);
			i = amp + 1;
		}
	}

	// Extract the request path + query from an HTTP request's first line:
	// "GET /cb?code=... HTTP/1.1". Returns path (before '?') and query (after '?').
	bool ParseRequestLine(const std::string& Request, std::string& OutPath, std::string& OutQuery)
	{
		// First line only.
		size_t eol = Request.find('\n');
		std::string Line = (eol == std::string::npos) ? Request : Request.substr(0, eol);
		if (!Line.empty() && Line.back() == '\r') Line.pop_back();

		// Method SP target SP version
		size_t sp1 = Line.find(' ');
		if (sp1 == std::string::npos) return false;
		size_t sp2 = Line.find(' ', sp1 + 1);
		if (sp2 == std::string::npos) return false;
		const std::string Target = Line.substr(sp1 + 1, sp2 - sp1 - 1);

		const size_t q = Target.find('?');
		if (q == std::string::npos)
		{
			OutPath = Target;
			OutQuery.clear();
		}
		else
		{
			OutPath = Target.substr(0, q);
			OutQuery = Target.substr(q + 1);
		}
		return true;
	}

	const char* const kHtmlOk =
		"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Branchive</title></head>"
		"<body style=\"font:16px/1.6 system-ui,sans-serif;background:#0e1116;color:#eef2f7;"
		"display:flex;flex-direction:column;align-items:center;justify-content:center;"
		"height:100vh;margin:0;text-align:center;gap:12px\">"
		"<span style=\"font-size:20px;font-weight:600;color:#f4f6f9\">Branchive</span>"
		"<p style=\"margin:0;color:#aab4c2\">Signed in to Branchive. You may close this tab.</p></body></html>";

	void SendHttp(LoreSocket Sock, int Status, const char* Reason, const std::string& Body)
	{
		std::string Resp = "HTTP/1.1 " + std::to_string(Status) + " " + Reason + "\r\n";
		Resp += "Content-Type: text/html; charset=utf-8\r\n";
		Resp += "Content-Length: " + std::to_string(Body.size()) + "\r\n";
		Resp += "Connection: close\r\n\r\n";
		Resp += Body;
		size_t Sent = 0;
		while (Sent < Resp.size())
		{
			const int n = (int)send(Sock, Resp.data() + Sent, (int)(Resp.size() - Sent), 0);
			if (n <= 0) break;
			Sent += (size_t)n;
		}
	}
}

namespace BranchiveLore
{
	FLoopbackListener::FLoopbackListener(int InPortMin, int InPortMax)
		: PortMin(InPortMin), PortMax(InPortMax)
	{
	}

	FLoopbackListener::~FLoopbackListener()
	{
		Stop();
	}

	int FLoopbackListener::Bind()
	{
#if defined(_WIN32)
		static FWsaGuard GWsa; // process-lifetime; WSAStartup is ref-counted anyway
		if (!GWsa.bOk) return -1;
#endif
		if (PortMin > PortMax) return -1;

		// Random order across the whole range: try each candidate at most once
		// (bounded — no unbounded retry loop). §2.1.
		std::vector<int> Ports;
		Ports.reserve(size_t(PortMax - PortMin + 1));
		for (int p = PortMin; p <= PortMax; ++p) Ports.push_back(p);
		{
			std::random_device Rd;
			std::mt19937 Gen(Rd());
			std::shuffle(Ports.begin(), Ports.end(), Gen);
		}

		for (int Candidate : Ports)
		{
			LoreSocket Sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (Sock == kInvalidSock) continue;

			// EXCLUSIVE bind (M2): NEVER SO_REUSEADDR/SO_REUSEPORT. On Windows, assert
			// SO_EXCLUSIVEADDRUSE so no other socket (even one with SO_REUSEADDR) can
			// share/hijack this port. H2: if the option can't be set we CANNOT guarantee
			// exclusivity, so treat it as a bind failure for this candidate (fail closed).
#if defined(_WIN32)
			{
				BOOL Excl = TRUE;
				if (setsockopt(Sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&Excl, sizeof(Excl)) != 0)
				{
					CloseSock(Sock);
					continue;
				}
			}
#endif

			sockaddr_in Addr;
			std::memset(&Addr, 0, sizeof(Addr));
			Addr.sin_family = AF_INET;
			Addr.sin_port = htons((unsigned short)Candidate);
			// 127.0.0.1 literal ONLY — never INADDR_ANY (0.0.0.0), never a DNS name.
			Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			if (bind(Sock, (sockaddr*)&Addr, sizeof(Addr)) != 0)
			{
				CloseSock(Sock); // EADDRINUSE / bind failure — try the next port
				continue;
			}
			if (listen(Sock, 1) != 0)
			{
				CloseSock(Sock);
				continue;
			}

			ListenSock = FromSock(Sock);
			BoundPort = Candidate;
			return Candidate; // confirmed LISTENING before this returns
		}

		// Range exhausted — FAIL CLOSED. Never fall back to 0.0.0.0 / an out-of-range port.
		return -1;
	}

	FLoopbackResult FLoopbackListener::AwaitCode(int TimeoutMs)
	{
		FLoopbackResult Result;
		if (ListenSock < 0)
		{
			Result.Error = "Loopback listener is not bound.";
			return Result;
		}

		LoreSocket Listen = AsSock(ListenSock);

		// WALL-CLOCK deadline (std::steady_clock) — NOT std::clock(), which measures CPU
		// time: during select()/recv() the process sleeps and accrues ~no CPU time, so a
		// std::clock() deadline would never fire and the timeout would hang forever.
		const std::chrono::steady_clock::time_point Deadline =
			std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
		auto RemainingMs = [&]() -> int
		{
			const auto Now = std::chrono::steady_clock::now();
			if (Now >= Deadline) return 0;
			return (int)std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - Now).count();
		};

		while (true)
		{
			int Rem = RemainingMs();
			if (Rem <= 0)
			{
				// REJECT (not merely close) — the caller never hangs (f3a3ae2 fix).
				Result.bTimedOut = true;
				Result.Error = "Timed out waiting for the Branchive sign-in callback.";
				break;
			}

			fd_set Rf;
			FD_ZERO(&Rf);
			FD_SET(Listen, &Rf);
			timeval Tv;
			Tv.tv_sec = Rem / 1000;
			Tv.tv_usec = (Rem % 1000) * 1000;

#if defined(_WIN32)
			const int Ready = select(0, &Rf, nullptr, nullptr, &Tv);
#else
			const int Ready = select((int)Listen + 1, &Rf, nullptr, nullptr, &Tv);
#endif
			if (Ready == 0)
			{
				Result.bTimedOut = true;
				Result.Error = "Timed out waiting for the Branchive sign-in callback.";
				break;
			}
			if (Ready < 0)
			{
#if !defined(_WIN32)
				if (errno == EINTR) continue;
#endif
				Result.Error = "Loopback select() failed.";
				break;
			}

			LoreSocket Client = accept(Listen, nullptr, nullptr);
			if (Client == kInvalidSock)
			{
				continue; // transient — keep waiting within the deadline
			}

			// Bounded read of the request (we only need the first line's query).
			std::string Request;
			{
				// Per-recv timeout so a stalled client can't hold us past the deadline.
				int RRem = RemainingMs();
				if (RRem < 1) RRem = 1;
#if defined(_WIN32)
				DWORD RcvTo = (DWORD)RRem;
				setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&RcvTo, sizeof(RcvTo));
#else
				timeval RcvTo;
				RcvTo.tv_sec = RRem / 1000;
				RcvTo.tv_usec = (RRem % 1000) * 1000;
				setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&RcvTo, sizeof(RcvTo));
#endif
				char Buf[2048];
				const int MaxTotal = 8192; // request-line + headers cap; we only parse line 1
				while ((int)Request.size() < MaxTotal)
				{
					const int n = (int)recv(Client, Buf, (int)sizeof(Buf), 0);
					if (n <= 0) break;
					Request.append(Buf, (size_t)n);
					if (Request.find("\r\n") != std::string::npos || Request.find('\n') != std::string::npos)
					{
						break; // have the request line — enough to route + parse the query
					}
				}
			}

			std::string Path, Query;
			if (!ParseRequestLine(Request, Path, Query))
			{
				SendHttp(Client, 400, "Bad Request", "Bad request. You may close this tab.");
				CloseSock(Client);
				continue; // not a valid request; keep waiting (does NOT consume the one-shot)
			}

			if (Path != "/cb")
			{
				SendHttp(Client, 404, "Not Found", "Not found. You may close this tab.");
				CloseSock(Client);
				continue; // everything else 404s and does NOT consume the one-shot
			}

			// /cb — the one-shot. Parse the query (F2: NO loopback `state` — PKCE is the
			// control on this fixed hop). ?error= is surfaced; a missing code is rejected.
			std::string Code, Err;
			ParseQueryFor(Query, Code, Err);

			if (!Err.empty())
			{
				SendHttp(Client, 200, "OK", "Sign-in was cancelled or failed. You may close this tab.");
				Result.Error = "Branchive sign-in error: " + Err;
			}
			else if (!Code.empty())
			{
				SendHttp(Client, 200, "OK", kHtmlOk);
				Result.bOk = true;
				Result.Code = Code; // NB: the code itself is never logged
			}
			else
			{
				SendHttp(Client, 200, "OK", "No authorization code was returned. You may close this tab.");
				Result.Error = "No authorization code in the redirect.";
			}
			CloseSock(Client);
			break; // one-shot: stop after the first /cb
		}

		Stop();
		return Result;
	}

	void FLoopbackListener::Stop()
	{
		if (ListenSock >= 0)
		{
			CloseSock(AsSock(ListenSock));
			ListenSock = -1;
		}
	}
}
