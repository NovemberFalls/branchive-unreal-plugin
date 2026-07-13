// Copyright Branchive.
#include "BranchiveCloudPlatform.h"

#include "BranchiveSourceControlLog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
	// WindowsHWrapper.h includes <windows.h> the UE-correct way (WIN32_LEAN_AND_MEAN +
	// TEXT/macro restoration) so raw <windows.h> can't clash with UE's TEXT under /WX.
	#include "Windows/WindowsHWrapper.h"
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <wincrypt.h>
	#include <bcrypt.h>
	#include "Windows/HideWindowsPlatformTypes.h"
	#pragma comment(lib, "crypt32.lib")
	#pragma comment(lib, "bcrypt.lib")
#elif PLATFORM_MAC
	#include <Security/Security.h>
	#include <CoreFoundation/CoreFoundation.h>
#elif PLATFORM_LINUX
	#include <fcntl.h>
	#include <unistd.h>
	#include <sys/stat.h>
#endif

// ============================================================================
// Crypto-strong RNG
// ============================================================================
bool BranchiveCryptoRandomBytes(uint8* Buffer, int32 Count)
{
	if (Buffer == nullptr || Count <= 0)
	{
		return false;
	}
#if PLATFORM_WINDOWS
	const NTSTATUS St = BCryptGenRandom(nullptr, (PUCHAR)Buffer, (ULONG)Count,
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return St == 0; // STATUS_SUCCESS
#elif PLATFORM_MAC
	return SecRandomCopyBytes(kSecRandomDefault, (size_t)Count, Buffer) == errSecSuccess;
#else
	// Linux and other POSIX: /dev/urandom.
	FILE* F = fopen("/dev/urandom", "rb");
	if (!F)
	{
		return false;
	}
	const size_t Read = fread(Buffer, 1, (size_t)Count, F);
	fclose(F);
	return Read == (size_t)Count;
#endif
}

// ============================================================================
// Token-at-rest
// ============================================================================
namespace
{
	FString TokenFilePath()
	{
		const FString Dir = FPaths::Combine(FString(FPlatformProcess::UserSettingsDir()), TEXT("Branchive"));
		return FPaths::Combine(Dir, TEXT("ue-cloud-bearer.bin"));
	}

	void EnsureParentDir(const FString& FilePath)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);
	}

#if PLATFORM_WINDOWS
	// Windows DPAPI store — CryptProtectData ties the ciphertext to the current user
	// account (same OS primitive Electron's safeStorage uses on Windows). Parity with
	// Desktop's actual at-rest security level on this OS (§2.4).
	class FDpapiTokenStore : public BranchiveLore::ILoreTokenStore
	{
	public:
		void Store(const std::string& Token) override
		{
			DATA_BLOB In;
			In.pbData = (BYTE*)Token.data();
			In.cbData = (DWORD)Token.size();
			DATA_BLOB Out;
			FMemory::Memzero(&Out, sizeof(Out));
			if (!CryptProtectData(&In, L"branchive-cloud-bearer", nullptr, nullptr, nullptr,
				CRYPTPROTECT_UI_FORBIDDEN, &Out))
			{
				UE_LOG(LogBranchiveSourceControl, Warning, TEXT("Branchive: could not encrypt the cloud token at rest (DPAPI)."));
				return;
			}
			TArray<uint8> Cipher;
			Cipher.Append((const uint8*)Out.pbData, (int32)Out.cbData);
			LocalFree(Out.pbData);
			const FString Path = TokenFilePath();
			EnsureParentDir(Path);
			FFileHelper::SaveArrayToFile(Cipher, *Path);
		}

		std::string Load() override
		{
			TArray<uint8> Cipher;
			if (!FFileHelper::LoadFileToArray(Cipher, *TokenFilePath()) || Cipher.Num() == 0)
			{
				return std::string();
			}
			DATA_BLOB In;
			In.pbData = (BYTE*)Cipher.GetData();
			In.cbData = (DWORD)Cipher.Num();
			DATA_BLOB Out;
			FMemory::Memzero(&Out, sizeof(Out));
			if (!CryptUnprotectData(&In, nullptr, nullptr, nullptr, nullptr,
				CRYPTPROTECT_UI_FORBIDDEN, &Out))
			{
				return std::string();
			}
			std::string Token((const char*)Out.pbData, (size_t)Out.cbData);
			LocalFree(Out.pbData);
			return Token;
		}

		void Clear() override
		{
			IFileManager::Get().Delete(*TokenFilePath(), /*RequireExists=*/false);
		}

		bool IsPersistent() const override { return true; }
	};
#endif // PLATFORM_WINDOWS

#if PLATFORM_MAC
	// macOS Keychain Services store — same primitive Electron's safeStorage uses on
	// macOS (§2.4). Keyed by a stable service+account so a sign-in survives updates.
	class FKeychainTokenStore : public BranchiveLore::ILoreTokenStore
	{
	public:
		void Store(const std::string& Token) override
		{
			Clear(); // simplest correct: delete any prior item, then add
			CFDataRef Data = CFDataCreate(nullptr, (const UInt8*)Token.data(), (CFIndex)Token.size());
			// H4: kSecAttrAccessibleWhenUnlockedThisDeviceOnly — device-only (never migrates
			// to another device) AND non-syncable, so the Bearer stays OUT of iCloud Keychain.
			const void* Keys[] = { kSecClass, kSecAttrService, kSecAttrAccount, kSecAttrAccessible, kSecValueData };
			const void* Vals[] = { kSecClassGenericPassword, ServiceRef(), AccountRef(),
				kSecAttrAccessibleWhenUnlockedThisDeviceOnly, Data };
			CFDictionaryRef Query = CFDictionaryCreate(nullptr, Keys, Vals, 5,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			SecItemAdd(Query, nullptr);
			CFRelease(Query);
			CFRelease(Data);
		}

		std::string Load() override
		{
			const void* Keys[] = { kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit };
			const void* Vals[] = { kSecClassGenericPassword, ServiceRef(), AccountRef(), kCFBooleanTrue, kSecMatchLimitOne };
			CFDictionaryRef Query = CFDictionaryCreate(nullptr, Keys, Vals, 5,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFTypeRef Result = nullptr;
			const OSStatus St = SecItemCopyMatching(Query, &Result);
			CFRelease(Query);
			std::string Out;
			if (St == errSecSuccess && Result)
			{
				CFDataRef Data = (CFDataRef)Result;
				Out.assign((const char*)CFDataGetBytePtr(Data), (size_t)CFDataGetLength(Data));
				CFRelease(Result);
			}
			return Out;
		}

		void Clear() override
		{
			const void* Keys[] = { kSecClass, kSecAttrService, kSecAttrAccount };
			const void* Vals[] = { kSecClassGenericPassword, ServiceRef(), AccountRef() };
			CFDictionaryRef Query = CFDictionaryCreate(nullptr, Keys, Vals, 3,
				&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			SecItemDelete(Query);
			CFRelease(Query);
		}

		bool IsPersistent() const override { return true; }

	private:
		static CFStringRef ServiceRef() { return CFSTR("io.branchive.ue"); }
		static CFStringRef AccountRef() { return CFSTR("cloud-bearer"); }
	};
#endif // PLATFORM_MAC

	// Non-persistent, in-memory store: the FAIL-SAFE for any platform/context where
	// secure persistence isn't guaranteed. A bearer never touches disk; the user
	// re-authenticates next session. Used on Linux by default (viktor's binding
	// condition) and anywhere a headless/unattended context is detected.
	class FNonPersistentTokenStore : public BranchiveLore::ILoreTokenStore
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

#if PLATFORM_LINUX
	// A 0600 on-disk store — ONLY reachable by an explicit administrator opt-in
	// (BRANCHIVE_LINUX_TOKEN_STORE=file-0600), NEVER a default. A 0600 file is
	// owner-readable by any process running as the same OS user, so on a shared /
	// multi-tenant / headless box it is not a real boundary — hence it is opt-in only
	// and the default remains non-persistent (§2.4).
	class FFile0600TokenStore : public BranchiveLore::ILoreTokenStore
	{
	public:
		void Store(const std::string& Token) override
		{
			const FString Path = TokenFilePath();
			EnsureParentDir(Path);
			// H3: create the file 0600 ATOMICALLY (open(O_CREAT|O_EXCL, 0600)) so it is NEVER
			// momentarily world/group-readable, instead of the old write-then-chmod race. The
			// 0600 mode has no group/other bits, so umask can only ever clear more — never
			// widen. Remove any prior file first so O_EXCL owns a freshly created inode.
			const std::string PathU8(TCHAR_TO_UTF8(*Path));
			unlink(PathU8.c_str());
			const int Fd = open(PathU8.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
			if (Fd < 0)
			{
				UE_LOG(LogBranchiveSourceControl, Warning,
					TEXT("Branchive: could not create the owner-only (0600) cloud-token file."));
				return;
			}
			const char* Data = Token.data();
			size_t Remaining = Token.size();
			bool bOk = true;
			while (Remaining > 0)
			{
				const ssize_t Written = write(Fd, Data, Remaining);
				if (Written <= 0) { bOk = false; break; }
				Data += Written;
				Remaining -= (size_t)Written;
			}
			close(Fd);
			if (!bOk)
			{
				unlink(PathU8.c_str());
				UE_LOG(LogBranchiveSourceControl, Warning,
					TEXT("Branchive: failed writing the owner-only (0600) cloud-token file."));
			}
		}
		std::string Load() override
		{
			FString S;
			if (FFileHelper::LoadFileToString(S, *TokenFilePath()))
			{
				return std::string(TCHAR_TO_UTF8(*S));
			}
			return std::string();
		}
		void Clear() override
		{
			IFileManager::Get().Delete(*TokenFilePath(), /*RequireExists=*/false);
		}
		bool IsPersistent() const override { return true; }
	};
#endif // PLATFORM_LINUX
}

std::unique_ptr<BranchiveLore::ILoreTokenStore> MakePlatformTokenStore()
{
#if PLATFORM_WINDOWS
	return std::unique_ptr<BranchiveLore::ILoreTokenStore>(new FDpapiTokenStore());
#elif PLATFORM_MAC
	return std::unique_ptr<BranchiveLore::ILoreTokenStore>(new FKeychainTokenStore());
#elif PLATFORM_LINUX
	// DEFAULT: non-persistent (re-auth per session). A plugin cannot confirm
	// "single-user interactive desktop", so we fail safe (never a plaintext-ish file
	// on a box that might be shared/headless). An administrator can deliberately
	// enable the owner-only 0600 file for a workstation image via an env opt-in.
	const FString OptIn = FPlatformMisc::GetEnvironmentVariable(TEXT("BRANCHIVE_LINUX_TOKEN_STORE"));
	if (OptIn == TEXT("file-0600") && !FApp::IsUnattended() && !IsRunningCommandlet())
	{
		UE_LOG(LogBranchiveSourceControl, Warning,
			TEXT("Branchive: BRANCHIVE_LINUX_TOKEN_STORE=file-0600 set — the cloud token will be stored in a 0600 file, readable by any process running as your user. Do NOT use this on a shared or multi-tenant machine."));
		return std::unique_ptr<BranchiveLore::ILoreTokenStore>(new FFile0600TokenStore());
	}
	UE_LOG(LogBranchiveSourceControl, Log,
		TEXT("Branchive: cloud token storage on Linux is NON-PERSISTENT by default (re-auth per session). Set BRANCHIVE_LINUX_TOKEN_STORE=file-0600 on a trusted single-user workstation to persist it."));
	return std::unique_ptr<BranchiveLore::ILoreTokenStore>(new FNonPersistentTokenStore());
#else
	return std::unique_ptr<BranchiveLore::ILoreTokenStore>(new FNonPersistentTokenStore());
#endif
}
