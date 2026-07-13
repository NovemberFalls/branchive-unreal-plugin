// Copyright Branchive. Engine-independent error taxonomy — see LoreErrors.h.
#include "LoreErrors.h"
#include "LoreParse.h" // Trim / StartsWithNoCase / SplitLines

namespace BranchiveLore
{
	static bool ContainsNoCase(const std::string& Haystack, const char* Needle)
	{
		auto lower = [](char c) -> char
		{
			if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
			return c;
		};
		std::string h; h.reserve(Haystack.size());
		for (char c : Haystack) h.push_back(lower(c));
		std::string n;
		for (size_t i = 0; Needle[i]; ++i) n.push_back(lower(Needle[i]));
		if (n.empty()) return true;
		return h.find(n) != std::string::npos;
	}

	EErrorClass ClassifyError(int ExitCode, bool bSpawnFailed,
	                          const std::string& StdErr, const std::string& StdOut)
	{
		if (bSpawnFailed)
		{
			return EErrorClass::BinaryNotFound;
		}
		if (ExitCode == 0)
		{
			return EErrorClass::None;
		}

		// Prefer stderr text; fall back to stdout (contract §3.6 precedence).
		const std::string& Text = !Trim(StdErr).empty() ? StdErr : StdOut;

		// Priority order matters: several conditions share exit code 2, and
		// "not found" is a substring of "repository not found"/"address not
		// found"/"revision not found". Match the most specific first.
		if (ContainsNoCase(Text, "repository not found")) return EErrorClass::NotAWorkspace;
		// call.rs is the Rust source location the not-a-workspace error embeds.
		if (ContainsNoCase(Text, "src\\call.rs") || ContainsNoCase(Text, "src/call.rs"))
			return EErrorClass::NotAWorkspace;

		if (ContainsNoCase(Text, "address not found"))      return EErrorClass::HistoricalContentUnavailable;
		if (ContainsNoCase(Text, "nothing staged for commit")) return EErrorClass::EmptyCommit;
		if (ContainsNoCase(Text, "branch has diverged"))    return EErrorClass::DivergedBranch;
		if (ContainsNoCase(Text, "no remote configured"))   return EErrorClass::NoRemoteConfigured;

		// Foreign-lock push rejection — three known shapes (contract §7).
		if (ContainsNoCase(Text, "is locked by") ||
		    ContainsNoCase(Text, "failed to release the lock") ||
		    ContainsNoCase(Text, "not in a state required for the operation"))
			return EErrorClass::ForeignLock;

		if (ContainsNoCase(Text, "no auth endpoint available")) return EErrorClass::NoAuthEndpoint;

		if (ContainsNoCase(Text, "loading user token: no token stored") ||
		    ContainsNoCase(Text, "no token stored"))
			return EErrorClass::AuthMissing;

		if (ContainsNoCase(Text, "slow down") || ContainsNoCase(Text, "store overloaded"))
			return EErrorClass::StoreOverloaded;

		// Text-only signal for protected-branch is INSUFFICIENT by itself
		// (contract §7): the host must ALSO confirm the branch is protected via
		// the protect-state read. We surface it as NotAuthorized and let the
		// caller decide "protected" vs. a generic auth failure.
		if (ContainsNoCase(Text, "not authorized to access repository"))
			return EErrorClass::NotAuthorized;

		// clap's own generic argument-parsing error (a plugin bug).
		{
			const std::string T = Trim(Text);
			if (T.rfind("error: ", 0) == 0) return EErrorClass::CliArgumentError;
		}

		// Generic "not found" for a branch-scoped op (exit 13 typically).
		if (ContainsNoCase(Text, "not found")) return EErrorClass::BranchNotFound;

		return EErrorClass::Unknown;
	}

	bool HasMergeConflict(const std::string& CombinedOutput)
	{
		std::vector<std::string> Lines;
		SplitLines(CombinedOutput, Lines);
		for (const std::string& L : Lines)
		{
			if (StartsWithNoCase(Trim(L), "files in conflict:"))
			{
				return true;
			}
		}
		return false;
	}

	void ParseConflictedFiles(const std::string& CombinedOutput, std::vector<std::string>& Out)
	{
		std::vector<std::string> Lines;
		SplitLines(CombinedOutput, Lines);
		bool bInSection = false;
		for (const std::string& Raw : Lines)
		{
			const std::string L = Trim(Raw);
			if (!bInSection)
			{
				if (StartsWithNoCase(L, "files in conflict:")) bInSection = true;
				continue;
			}
			if (L.empty()) break; // blank line ends the section
			Out.push_back(L);
		}
	}

	std::string FriendlyMessage(EErrorClass Class)
	{
		switch (Class)
		{
		case EErrorClass::None:                         return "";
		case EErrorClass::BinaryNotFound:               return "The 'lore' CLI could not be found. Set its path in the Branchive revision control settings.";
		case EErrorClass::NotAWorkspace:                return "This folder is no longer a Lore workspace.";
		case EErrorClass::HistoricalContentUnavailable: return "That revision's content is unavailable right now. Try again once connected.";
		case EErrorClass::CliArgumentError:             return "Internal error: the plugin built an invalid command.";
		case EErrorClass::BranchNotFound:               return "That branch does not exist.";
		case EErrorClass::AuthMissing:                  return "You are signed out of Branchive Cloud. Sign in to continue.";
		case EErrorClass::NoAuthEndpoint:               return "The server has no authenticator configured.";
		case EErrorClass::NoRemoteConfigured:           return "This workspace needs to be linked to a server first.";
		case EErrorClass::ForeignLock:                  return "This file is locked by another user. Coordinate with them or ask an admin to release it.";
		case EErrorClass::DivergedBranch:               return "Someone else pushed to this branch first. Sync, then retry your check-in.";
		case EErrorClass::EmptyCommit:                  return "Nothing to check in.";
		case EErrorClass::NotAuthorized:                return "You are not authorized to push here. This branch may be protected — open a review instead.";
		case EErrorClass::StoreOverloaded:              return "The store is busy (large push). This will retry automatically.";
		case EErrorClass::Unknown:                      return "The operation failed.";
		}
		return "The operation failed.";
	}
}
