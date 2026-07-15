// Copyright 2026 Bits, LLC. All Rights Reserved.
//
// Pure, dependency-free predicate shared VERBATIM by the UE module (Lore/LoreCli.cpp)
// and the standalone auth test (Tests/standalone/auth_test.cpp). Header-only so it needs
// no build-script change and pulls in no UE or platform headers.
//
// F-BIN (zara): FLoreCli::Run spawns the `lore` binary via FPlatformProcess::ExecProcess
// with cwd = the workspace RepoPath. On Windows a BARE image name ("lore.exe") is
// resolved against a search order that can include the current directory BEFORE PATH, so
// a hostile repo that commits a `lore.exe` at its root would be executed — with the
// user's Lore JWT on the command line (credential exfil). The ONLY shape safe to spawn is
// an ABSOLUTE path; Run additionally requires the file to exist. This predicate is the
// absolute-path gate both the production spawn path and the test use.
#pragma once

#include <string>

namespace BranchiveLore
{
	// True iff Path is an ABSOLUTE path (one that pins the image to a specific location
	// rather than being resolved against a working directory / PATH). Accepts BOTH Windows
	// ("C:\\x", "C:/x", "\\\\server\\share\\x") and POSIX ("/usr/bin/x") absolute forms on
	// every host, so behaviour is uniform in tests and across platforms; the caller layers
	// an on-disk existence check on top for the current platform.
	//
	// A bare name ("lore", "lore.exe"), a dot-relative path ("./lore"), a rootless subpath
	// ("sub\\lore.exe"), a drive-RELATIVE path ("C:lore.exe") and a rootless leading-slash
	// path ("\\lore.exe") all return false — none may EVER be spawned with cwd set to a
	// workspace directory.
	inline bool IsAbsoluteBinaryPath(const std::string& Path)
	{
		if (Path.empty())
		{
			return false;
		}
		const unsigned char c0 = (unsigned char)Path[0];
		// POSIX absolute: a single leading '/'. (A lone leading '\\' is drive-relative on
		// Windows, so it is deliberately NOT accepted here.)
		if (c0 == '/')
		{
			return true;
		}
		// UNC: "\\server\share" or "//server/share" (two leading slashes, either kind).
		if (Path.size() >= 2)
		{
			const unsigned char c1 = (unsigned char)Path[1];
			if ((c0 == '\\' || c0 == '/') && (c1 == '\\' || c1 == '/'))
			{
				return true;
			}
		}
		// Windows drive-absolute: "<letter>:\" or "<letter>:/". "<letter>:name" WITHOUT the
		// separator is drive-RELATIVE (resolved against the drive's current dir) — rejected.
		if (Path.size() >= 3)
		{
			const bool bDriveLetter =
				(c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
			if (bDriveLetter && Path[1] == ':' && (Path[2] == '\\' || Path[2] == '/'))
			{
				return true;
			}
		}
		return false;
	}
}
