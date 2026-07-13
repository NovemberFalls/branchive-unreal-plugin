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

	// ------------------------------------------------------------- file history

	// Match a "Key   : value" field line (already trimmed). On match, returns true
	// and fills Value with everything after the first ':' (trimmed).
	static bool MatchField(const std::string& Trimmed, const char* Key, std::string& Value)
	{
		if (!StartsWithNoCase(Trimmed, Key)) return false;
		// Ensure what follows Key is only whitespace up to a ':'.
		size_t i = 0;
		while (Key[i] != '\0') ++i;              // i = len(Key)
		size_t p = i;
		while (p < Trimmed.size() && std::isspace(static_cast<unsigned char>(Trimmed[p]))) ++p;
		if (p >= Trimmed.size() || Trimmed[p] != ':') return false;
		Value = Trim(Trimmed.substr(p + 1));
		return true;
	}

	// Is `Trimmed` a bare status-code marker line ("M", "A", or "A <path>")? If so
	// return its code letter, else '\0'. These precede each revision block and are
	// NOT part of it (contract §4.12) — we capture only the code to fill Action.
	static char CodeMarker(const std::string& Trimmed)
	{
		if (Trimmed.empty()) return '\0';
		const char C = Trimmed[0];
		const bool bIsCode = (C == 'A' || C == 'M' || C == 'D' || C == 'R' || C == 'C');
		if (!bIsCode) return '\0';
		if (Trimmed.size() == 1) return C;                       // just "M"
		if (std::isspace(static_cast<unsigned char>(Trimmed[1]))) return C; // "A <path>"
		return '\0';
	}

	std::vector<FFileRevisionEntry> ParseFileHistory(const std::string& StdoutText)
	{
		std::vector<FFileRevisionEntry> Out;
		std::vector<std::string> Lines;
		SplitLines(StdoutText, Lines);

		FFileRevisionEntry Cur;
		bool bHave = false;
		char PendingCode = '\0';

		auto Flush = [&]()
		{
			if (bHave) { Out.push_back(Cur); }
			Cur = FFileRevisionEntry();
			bHave = false;
		};

		for (const std::string& Raw : Lines)
		{
			const std::string T = Trim(Raw);

			std::string Val;
			// A new revision block begins here (rule 1).
			if (MatchField(T, "Revision", Val))
			{
				Flush();
				Cur = FFileRevisionEntry();
				bHave = true;
				Cur.Revision = std::strtoll(Val.c_str(), nullptr, 10);
				Cur.Code = PendingCode;   // the code line that preceded this block
				PendingCode = '\0';
				continue;
			}

			if (bHave)
			{
				if (MatchField(T, "Signature", Val)) { Cur.Signature = Val; continue; }
				if (MatchField(T, "Address",   Val)) { Cur.Address   = Val; continue; }
				if (MatchField(T, "Branch",    Val)) { Cur.Branch    = Val; continue; }
				if (MatchField(T, "Date",      Val)) { Cur.Date      = Val; continue; }
				if (MatchField(T, "Merge",     Val))
				{
					SplitWhitespace(Val, Cur.MergeParents);
					continue;
				}

				// Message line: indented (raw starts with whitespace) AND non-blank
				// after trimming (rule 4). This naturally excludes the column-0
				// "M"/"A <path>" code lines below.
				if (!Raw.empty() && std::isspace(static_cast<unsigned char>(Raw[0])) && !T.empty())
				{
					if (!Cur.Message.empty()) { Cur.Message.push_back('\n'); }
					Cur.Message += T;
					continue;
				}
			}

			// Not a field, not a message line: it may be the code marker that
			// precedes the NEXT block (rule: skipped from any block, code captured).
			const char Code = CodeMarker(T);
			if (Code != '\0') { PendingCode = Code; }
		}
		Flush();
		return Out;
	}

	// -------------------------------------------------------------------- diff

	// Parse "@@ -a[,b] +c[,d] @@..." — fills OldStart/NewStart (defensive: 0 if the
	// shape is unexpected). Counts are recomputed from content lines, not the header.
	static void ParseHunkHeader(const std::string& Line, long long& OldStart, long long& NewStart)
	{
		OldStart = 0;
		NewStart = 0;
		size_t p = Line.find('-');
		if (p != std::string::npos)
		{
			OldStart = std::strtoll(Line.c_str() + p + 1, nullptr, 10);
		}
		size_t q = Line.find('+', (p == std::string::npos ? 0 : p));
		if (q != std::string::npos)
		{
			NewStart = std::strtoll(Line.c_str() + q + 1, nullptr, 10);
		}
	}

	std::vector<FDiffFile> ParseUnifiedDiff(const std::string& StdoutText)
	{
		std::vector<FDiffFile> Out;
		std::vector<std::string> Lines;
		SplitLines(StdoutText, Lines);

		FDiffFile* CurFile = nullptr;
		FDiffHunk* CurHunk = nullptr;
		std::string PendingName;  // last bare filename line (rule 6) for binary attach

		auto EndHunk = [&]() { CurHunk = nullptr; };

		for (const std::string& Line : Lines)
		{
			// "--- <oldRef>" starts a new file entry (rule 1).
			if (Line.rfind("--- ", 0) == 0)
			{
				Out.push_back(FDiffFile());
				CurFile = &Out.back();
				CurHunk = nullptr;
				continue;
			}
			// "+++ <newRef>" sets the file's path (rule 2).
			if (Line.rfind("+++ ", 0) == 0)
			{
				if (CurFile)
				{
					std::string Ref = Trim(Line.substr(4));
					// strip a trailing "@..." decoration
					size_t at = Ref.find('@');
					if (at != std::string::npos) { Ref = Ref.substr(0, at); }
					Ref = Trim(Ref);
					if (Ref == "/dev/null") { /* deleted: keep OLD-side path if we had one */ }
					else { CurFile->Path = Ref; }
				}
				continue;
			}
			// Hunk header (rule 3).
			if (Line.rfind("@@", 0) == 0)
			{
				if (CurFile)
				{
					CurFile->Hunks.push_back(FDiffHunk());
					CurHunk = &CurFile->Hunks.back();
					ParseHunkHeader(Line, CurHunk->OldStart, CurHunk->NewStart);
				}
				continue;
			}
			// Content lines within a hunk (rule 4).
			if (CurHunk && !Line.empty() && (Line[0] == ' ' || Line[0] == '+' || Line[0] == '-'))
			{
				const char M = Line[0];
				const std::string Body = Line.substr(1);
				if (M == ' ')      { CurHunk->OldLines.push_back(Body); CurHunk->NewCount += 1; }
				else if (M == '-') { CurHunk->OldLines.push_back(Body); }
				else /* '+' */     { CurHunk->NewCount += 1; }
				continue;
			}
			// "\ No newline at end of file" — silently skipped (rule 4).
			if (!Line.empty() && Line[0] == '\\')
			{
				continue;
			}
			// Binary marker (rule 5): synthesize a binary file entry.
			if (StartsWithNoCase(Trim(Line), "binary files differ"))
			{
				FDiffFile BinF;
				BinF.bBinary = true;
				BinF.Path = PendingName;
				Out.push_back(BinF);
				CurFile = nullptr;
				CurHunk = nullptr;
				continue;
			}
			// Any other line ends the current hunk; if non-empty it becomes the
			// pending candidate filename (rule 6).
			EndHunk();
			const std::string Tt = Trim(Line);
			if (!Tt.empty()) { PendingName = Tt; }
		}
		return Out;
	}

	std::vector<std::string> ReconstructOldContent(const std::vector<std::string>& WorkingLines,
	                                               const std::vector<FDiffHunk>& Hunks)
	{
		std::vector<std::string> Lines = WorkingLines;

		// Sort hunk indices by NewStart DESCENDING so earlier splices don't shift
		// the line numbers later (lower) hunks were computed against (§4.13 step 5).
		std::vector<size_t> Order;
		for (size_t i = 0; i < Hunks.size(); ++i) { Order.push_back(i); }
		for (size_t a = 0; a + 1 < Order.size(); ++a)
		{
			for (size_t b = a + 1; b < Order.size(); ++b)
			{
				if (Hunks[Order[b]].NewStart > Hunks[Order[a]].NewStart)
				{
					size_t t = Order[a]; Order[a] = Order[b]; Order[b] = t;
				}
			}
		}

		for (size_t oi = 0; oi < Order.size(); ++oi)
		{
			const FDiffHunk& H = Hunks[Order[oi]];
			long long Start = H.NewStart - 1;      // 1-based -> 0-based
			if (Start < 0) { Start = 0; }
			if (Start > static_cast<long long>(Lines.size())) { Start = static_cast<long long>(Lines.size()); }

			long long RemoveN = H.NewCount;
			if (RemoveN < 0) { RemoveN = 0; }
			if (Start + RemoveN > static_cast<long long>(Lines.size()))
			{
				RemoveN = static_cast<long long>(Lines.size()) - Start;
			}

			std::vector<std::string> Next;
			Next.reserve(Lines.size() + H.OldLines.size());
			for (long long i = 0; i < Start; ++i) { Next.push_back(Lines[static_cast<size_t>(i)]); }
			for (const std::string& L : H.OldLines) { Next.push_back(L); }
			for (long long i = Start + RemoveN; i < static_cast<long long>(Lines.size()); ++i)
			{
				Next.push_back(Lines[static_cast<size_t>(i)]);
			}
			Lines.swap(Next);
		}
		return Lines;
	}

	// ----------------------------------------------------- conflict argv builders

	static void AppendOpPrefix(EConflictOp Op, std::vector<std::string>& Out)
	{
		switch (Op)
		{
		case EConflictOp::CherryPick: Out.push_back("revision"); Out.push_back("cherry-pick"); break;
		case EConflictOp::Revert:     Out.push_back("revision"); Out.push_back("revert");      break;
		case EConflictOp::Merge:
		default:                      Out.push_back("branch");   Out.push_back("merge");       break;
		}
	}

	std::vector<std::string> BuildConflictResolveArgv(EConflictOp Op, EConflictSide Side,
	                                                  const std::string& AbsFile)
	{
		std::vector<std::string> Out;
		AppendOpPrefix(Op, Out);
		Out.push_back("resolve");
		Out.push_back(Side == EConflictSide::Theirs ? "theirs" : "mine");
		Out.push_back(AbsFile);
		return Out;
	}

	std::vector<std::string> BuildConflictAbortArgv(EConflictOp Op)
	{
		std::vector<std::string> Out;
		AppendOpPrefix(Op, Out);
		Out.push_back("abort");
		return Out;
	}
}
