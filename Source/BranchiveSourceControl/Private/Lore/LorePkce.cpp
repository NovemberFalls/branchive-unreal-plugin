// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "LorePkce.h"

#include <vector>

namespace
{
	const char* const kB64UrlAlphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

	// --- SHA-256 (FIPS 180-4), compact + self-contained -----------------------
	inline uint32_t Ror(uint32_t X, uint32_t N) { return (X >> N) | (X << (32 - N)); }

	const uint32_t kSha256K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};
}

namespace BranchiveLore
{
	std::string Base64UrlNoPad(const uint8_t* Data, size_t Len)
	{
		std::string Out;
		Out.reserve(((Len + 2) / 3) * 4);
		size_t i = 0;
		for (; i + 3 <= Len; i += 3)
		{
			const uint32_t N = (uint32_t(Data[i]) << 16) | (uint32_t(Data[i + 1]) << 8) | uint32_t(Data[i + 2]);
			Out.push_back(kB64UrlAlphabet[(N >> 18) & 0x3F]);
			Out.push_back(kB64UrlAlphabet[(N >> 12) & 0x3F]);
			Out.push_back(kB64UrlAlphabet[(N >> 6) & 0x3F]);
			Out.push_back(kB64UrlAlphabet[N & 0x3F]);
		}
		const size_t Rem = Len - i;
		if (Rem == 1)
		{
			const uint32_t N = uint32_t(Data[i]) << 16;
			Out.push_back(kB64UrlAlphabet[(N >> 18) & 0x3F]);
			Out.push_back(kB64UrlAlphabet[(N >> 12) & 0x3F]);
		}
		else if (Rem == 2)
		{
			const uint32_t N = (uint32_t(Data[i]) << 16) | (uint32_t(Data[i + 1]) << 8);
			Out.push_back(kB64UrlAlphabet[(N >> 18) & 0x3F]);
			Out.push_back(kB64UrlAlphabet[(N >> 12) & 0x3F]);
			Out.push_back(kB64UrlAlphabet[(N >> 6) & 0x3F]);
		}
		return Out;
	}

	std::array<uint8_t, 32> Sha256(const uint8_t* Data, size_t Len)
	{
		uint32_t H[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
		};

		// Pre-processing: append 0x80, pad with zeros, append 64-bit big-endian length.
		const uint64_t BitLen = uint64_t(Len) * 8ull;
		const size_t TotalNoLen = Len + 1;                          // + 0x80
		const size_t PadZeros = (56 - (TotalNoLen % 64) + 64) % 64; // to 56 mod 64
		const size_t Total = TotalNoLen + PadZeros + 8;

		// Unsigned byte buffer (NOT std::string) so the 0x80 pad byte and the length
		// bytes never trip signed-char constant-truncation warnings (C4310 under /WX).
		std::vector<uint8_t> Buf;
		Buf.reserve(Total);
		Buf.insert(Buf.end(), Data, Data + Len);
		Buf.push_back(static_cast<uint8_t>(0x80));
		Buf.insert(Buf.end(), PadZeros, static_cast<uint8_t>(0));
		for (int s = 56; s >= 0; s -= 8)
		{
			Buf.push_back(static_cast<uint8_t>((BitLen >> s) & 0xFF));
		}

		const uint8_t* P = Buf.data();
		for (size_t Off = 0; Off < Total; Off += 64)
		{
			uint32_t W[64];
			for (int t = 0; t < 16; ++t)
			{
				const size_t j = Off + size_t(t) * 4;
				W[t] = (uint32_t(P[j]) << 24) | (uint32_t(P[j + 1]) << 16) | (uint32_t(P[j + 2]) << 8) | uint32_t(P[j + 3]);
			}
			for (int t = 16; t < 64; ++t)
			{
				const uint32_t s0 = Ror(W[t - 15], 7) ^ Ror(W[t - 15], 18) ^ (W[t - 15] >> 3);
				const uint32_t s1 = Ror(W[t - 2], 17) ^ Ror(W[t - 2], 19) ^ (W[t - 2] >> 10);
				W[t] = W[t - 16] + s0 + W[t - 7] + s1;
			}

			uint32_t a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
			for (int t = 0; t < 64; ++t)
			{
				const uint32_t S1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
				const uint32_t Ch = (e & f) ^ (~e & g);
				const uint32_t T1 = h + S1 + Ch + kSha256K[t] + W[t];
				const uint32_t S0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
				const uint32_t Maj = (a & b) ^ (a & c) ^ (b & c);
				const uint32_t T2 = S0 + Maj;
				h = g; g = f; f = e; e = d + T1; d = c; c = b; b = a; a = T1 + T2;
			}
			H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
		}

		std::array<uint8_t, 32> Digest{};
		for (int i = 0; i < 8; ++i)
		{
			Digest[i * 4 + 0] = uint8_t((H[i] >> 24) & 0xFF);
			Digest[i * 4 + 1] = uint8_t((H[i] >> 16) & 0xFF);
			Digest[i * 4 + 2] = uint8_t((H[i] >> 8) & 0xFF);
			Digest[i * 4 + 3] = uint8_t(H[i] & 0xFF);
		}
		return Digest;
	}

	std::string EncodeVerifier(const uint8_t* RandomBytes, size_t Len)
	{
		return Base64UrlNoPad(RandomBytes, Len);
	}

	std::string DeriveChallengeS256(const std::string& Verifier)
	{
		// ASCII(verifier) per RFC 7636 §4.2 — the verifier's charset is already ASCII.
		const std::array<uint8_t, 32> Digest =
			Sha256(reinterpret_cast<const uint8_t*>(Verifier.data()), Verifier.size());
		return Base64UrlNoPad(Digest.data(), Digest.size());
	}
}
