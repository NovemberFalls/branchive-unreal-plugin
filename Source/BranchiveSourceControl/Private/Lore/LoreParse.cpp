// Copyright Branchive. Engine-independent parsers — see LoreParse.h.
#include "LoreParse.h"

#include <cctype>
#include <cstdlib>

namespace BranchiveLore
{
	const char* const UnknownOwnerSentinel = "<unknown>";

	// ------------------------------------------------------------------ helpers

	std::string Trim(const std::string& S)
	{
		size_t B = 0;
		size_t E = S.size();
		while (B < E && std::isspace(static_cast<unsigned char>(S[B]))) ++B;
		while (E > B && std::isspace(static_cast<unsigned char>(S[E - 1]))) --E;
		return S.substr(B, E - B);
	}

	static char LowerAscii(char C)
	{
		return static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
	}

	bool StartsWithNoCase(const std::string& S, const char* Prefix)
	{
		size_t i = 0;
		for (; Prefix[i] != '\0'; ++i)
		{
			if (i >= S.size()) return false;
			if (LowerAscii(S[i]) != LowerAscii(Prefix[i])) return false;
		}
		return true;
	}

	void SplitLines(const std::string& Text, std::vector<std::string>& OutLines)
	{
		std::string Cur;
		for (size_t i = 0; i < Text.size(); ++i)
		{
			const char c = Text[i];
			if (c == '\n')
			{
				OutLines.push_back(Cur);
				Cur.clear();
			}
			else if (c == '\r')
			{
				// swallow; the paired '\n' (if any) flushes the line
			}
			else
			{
				Cur.push_back(c);
			}
		}
		if (!Cur.empty())
		{
			OutLines.push_back(Cur);
		}
	}

	// Split a line on runs of ASCII whitespace.
	static void SplitWhitespace(const std::string& Line, std::vector<std::string>& Out)
	{
		std::string Cur;
		for (char c : Line)
		{
			if (std::isspace(static_cast<unsigned char>(c)))
			{
				if (!Cur.empty()) { Out.push_back(Cur); Cur.clear(); }
			}
			else
			{
				Cur.push_back(c);
			}
		}
		if (!Cur.empty()) Out.push_back(Cur);
	}

	// ------------------------------------------------------------------ status

	// Strip a trailing " (M)!" / " (M)" original-code annotation the CLI appends
	// in merge/conflict states (contract §4.1 step 6), then unconditionally trim.
	static std::string StripSuffixAndTrim(const std::string& In)
	{
		// Strip a trailing " (X)" or " (X)!" annotation (single status letter X).
		// Trim first so a trailing space before the annotation doesn't hide it.
		std::string P = Trim(In);
		if (P.size() >= 4)
		{
			const size_t n = P.size();
			const bool bBang = (P[n - 1] == '!');
			const size_t closeIdx = bBang ? n - 2 : n - 1;
			if (closeIdx >= 3 && P[closeIdx] == ')' && P[closeIdx - 2] == '(' && P[closeIdx - 3] == ' ')
			{
				P = P.substr(0, closeIdx - 3);
			}
		}
		return Trim(P);
	}

	FStatus ParseStatus(const std::string& StdoutText)
	{
		FStatus Out;
		std::vector<std::string> Lines;
		SplitLines(StdoutText, Lines);

		ESection Section = ESection::None;
		ESyncState PendingSync = ESyncState::Unknown;

		for (const std::string& Raw : Lines)
		{
			// Do NOT trim the raw line here: leading-space matters for section
			// membership detection below. But most matches use a trimmed copy.
			const std::string Line = Raw;

			if (StartsWithNoCase(Line, "Warning:"))
			{
				continue;
			}

			// On branch <name> revision <n> -> <head>
			if (StartsWithNoCase(Line, "On branch "))
			{
				std::vector<std::string> T;
				SplitWhitespace(Line, T);
				// T = [On, branch, <name>, revision, <n>, ->, <head>]
				for (size_t i = 0; i < T.size(); ++i)
				{
					if (T[i] == "branch" && i + 1 < T.size()) Out.Branch = T[i + 1];
					if (T[i] == "revision" && i + 1 < T.size())
					{
						Out.Revision = std::strtoll(T[i + 1].c_str(), nullptr, 10);
					}
					if (T[i] == "->" && i + 1 < T.size()) Out.Head = T[i + 1];
				}
				continue;
			}

			// Remote revision <n> -> <head>
			if (StartsWithNoCase(Line, "Remote revision"))
			{
				std::vector<std::string> T;
				SplitWhitespace(Line, T);
				// T = [Remote, revision, <n>, ->, <head>]
				if (T.size() >= 3)
				{
					Out.bHasRemoteRevision = true;
					Out.RemoteRevision = std::strtoll(T[2].c_str(), nullptr, 10);
				}
				for (size_t i = 0; i < T.size(); ++i)
				{
					if (T[i] == "->" && i + 1 < T.size()) Out.RemoteHead = T[i + 1];
				}
				continue;
			}

			// Sync-state lines.
			if (StartsWithNoCase(Line, "Local branch in sync with remote"))       { PendingSync = ESyncState::InSync;   continue; }
			if (StartsWithNoCase(Line, "Local branch is ahead of remote"))        { PendingSync = ESyncState::Ahead;    continue; }
			if (StartsWithNoCase(Line, "Local branch is behind remote"))          { PendingSync = ESyncState::Behind;   continue; }
			if (StartsWithNoCase(Line, "Local branch has diverged"))              { PendingSync = ESyncState::Diverged; continue; }

			// Pending merge, incoming revision <sig>
			if (StartsWithNoCase(Line, "Pending merge"))
			{
				std::vector<std::string> T;
				SplitWhitespace(Line, T);
				for (size_t i = 0; i < T.size(); ++i)
				{
					if (T[i] == "revision" && i + 1 < T.size())
					{
						Out.bPendingMerge = true;
						Out.PendingMergeRevision = T[i + 1];
					}
				}
				continue;
			}

			// Section headers (exact-ish, case-insensitive).
			if (StartsWithNoCase(Line, "Changes staged for commit:"))     { Section = ESection::Staged;    continue; }
			if (StartsWithNoCase(Line, "Changes not staged for commit:")) { Section = ESection::Unstaged;  continue; }
			if (StartsWithNoCase(Line, "Untracked files:"))               { Section = ESection::Untracked; continue; }
			if (StartsWithNoCase(Line, "Changes in conflict:"))           { Section = ESection::Conflicts; continue; }
			// "Tracked changes:" footer clears the section. "No tracked changes"
			// is a benign footer that can co-occur with a conflict (contract
			// §4.1 step 5) — it also just clears (no rows follow it anyway).
			if (StartsWithNoCase(Line, "Tracked changes")) { Section = ESection::None; continue; }
			if (StartsWithNoCase(Line, "No tracked changes")) { Section = ESection::None; continue; }

			// A status row within an active section: <CODE><ws><path>
			if (Section != ESection::None)
			{
				// Find first non-space to locate the code letter (rows may be
				// indented, e.g. conflict rows "M  base.txt (M)!").
				const std::string L = Trim(Raw);
				if (L.size() >= 2)
				{
					const char Code = L[0];
					const bool bIsCode = (Code == 'A' || Code == 'M' || Code == 'D' ||
					                      Code == 'R' || Code == 'C' || Code == '?');
					if (bIsCode && std::isspace(static_cast<unsigned char>(L[1])))
					{
						std::string Rest = L.substr(1);
						std::string Path = StripSuffixAndTrim(Rest);
						// Skip an untracked-directory summary row (trailing '/').
						if (!Path.empty() && Path.back() != '/' && Path.back() != '\\')
						{
							FStatusEntry E;
							E.Code = Code;
							E.Path = Path;
							switch (Section)
							{
							case ESection::Staged:    Out.Staged.push_back(E);    break;
							case ESection::Unstaged:  Out.Unstaged.push_back(E);  break;
							case ESection::Untracked: Out.Untracked.push_back(E); break;
							case ESection::Conflicts: Out.Conflicts.push_back(E); break;
							default: break;
							}
						}
						continue;
					}
				}
			}
		}

		// Contract §4.1 step 2: with no remote revision line, syncState is forced
		// to Unknown regardless of any other line seen.
		Out.SyncState = Out.bHasRemoteRevision ? PendingSync : ESyncState::Unknown;
		return Out;
	}

	// ------------------------------------------------------------------- locks

	// Extract a JSON string value for `"key":"..."` from a flat NDJSON object.
	// Returns true and fills Out on success. Minimal unescaping of the common
	// escapes; sufficient for the flat lock rows the CLI emits (contract §4.17).
	static bool ExtractJsonString(const std::string& Line, const char* Key, std::string& Out)
	{
		std::string Pat = std::string("\"") + Key + "\":\"";
		size_t p = Line.find(Pat);
		if (p == std::string::npos) return false;
		p += Pat.size();
		std::string V;
		for (size_t i = p; i < Line.size(); ++i)
		{
			char c = Line[i];
			if (c == '\\')
			{
				if (i + 1 >= Line.size()) break;
				char n = Line[++i];
				switch (n)
				{
				case 'n': V.push_back('\n'); break;
				case 't': V.push_back('\t'); break;
				case '"': V.push_back('"');  break;
				case '\\': V.push_back('\\'); break;
				case '/': V.push_back('/');  break;
				default:  V.push_back(n);    break;
				}
			}
			else if (c == '"')
			{
				Out = V;
				return true;
			}
			else
			{
				V.push_back(c);
			}
		}
		return false;
	}

	std::vector<FLock> ParseLocksJson(const std::string& StdoutText)
	{
		std::vector<FLock> Out;
		std::vector<std::string> Lines;
		SplitLines(StdoutText, Lines);

		for (const std::string& Raw : Lines)
		{
			const std::string Line = Trim(Raw);
			if (Line.empty()) continue;

			// Only real lock rows. The trailing quote in the literal below is
			// what distinguishes "lockFileQuery" from "lockFileQueryBegin".
			if (Line.find("\"tagName\":\"lockFileQuery\"") == std::string::npos)
			{
				continue;
			}

			std::string Path, Owner, Branch;
			// Per parse rule: skip a row whose path or owner isn't a string.
			if (!ExtractJsonString(Line, "path", Path)) continue;
			if (!ExtractJsonString(Line, "owner", Owner)) continue;
			ExtractJsonString(Line, "branch", Branch); // optional -> "" if absent

			FLock L;
			L.Path = Path;
			L.Owner = Owner;   // may be the "<unknown>" sentinel; handled by consumers
			L.Branch = Branch;
			Out.push_back(L);
		}
		return Out;
	}
}
