// Copyright Branchive.
#include "BranchiveSourceControlRevision.h"

#include "BranchiveSourceControlLog.h"
#include "Lore/LoreCli.h"
#include "Lore/LoreParse.h"
#include "Lore/LoreErrors.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <string>
#include <vector>

namespace
{
	// Same 2 MB text-view cap the reference implementation uses (contract §4.13 step 2).
	constexpr int64 BranchiveTextViewMax = 2 * 1024 * 1024;

	std::string ToU8(const FString& S) { return std::string(TCHAR_TO_UTF8(*S)); }
	FString     ToF(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }

	// Split on '\n', stripping any trailing '\r' from each segment so the working
	// lines line up with the CLI's own \r-stripped diff lines (LoreParse::SplitLines).
	void SplitLinesNormalized(const std::string& Text, std::vector<std::string>& Out)
	{
		std::string Cur;
		for (char c : Text)
		{
			if (c == '\n') { Out.push_back(Cur); Cur.clear(); }
			else if (c == '\r') { /* swallow */ }
			else { Cur.push_back(c); }
		}
		Out.push_back(Cur); // trailing segment (may be empty if text ended with '\n')
	}

	std::string JoinLines(const std::vector<std::string>& Lines)
	{
		std::string Out;
		for (size_t i = 0; i < Lines.size(); ++i)
		{
			if (i > 0) { Out.push_back('\n'); }
			Out += Lines[i];
		}
		return Out;
	}

	// Returns true if the file is binary (NUL byte or a known-binary asset
	// extension) or too large to reconstruct via the text-diff path.
	bool IsBinaryOrTooLarge(const FString& AbsFile, const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() > BranchiveTextViewMax) { return true; }
		const FString Ext = FPaths::GetExtension(AbsFile).ToLower();
		if (Ext == TEXT("uasset") || Ext == TEXT("umap") || Ext == TEXT("ubulk") ||
		    Ext == TEXT("uexp") || Ext == TEXT("upk"))
		{
			return true;
		}
		for (uint8 B : Bytes)
		{
			if (B == 0) { return true; }
		}
		return false;
	}
}

bool FBranchiveSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type /*InConcurrency*/) const
{
	if (FullSignature.IsEmpty() || Filename.IsEmpty())
	{
		return false;
	}

	// 1. Read the CURRENT working-tree bytes (fs read, not a CLI call — §4.13 step 1).
	TArray<uint8> WorkingBytes;
	if (!FFileHelper::LoadFileToArray(WorkingBytes, *Filename))
	{
		return false; // file gone / unreadable
	}

	// 2. Binary / too-large guard (§4.13 step 2). Binary asset history cannot be
	//    reconstructed via the text-diff path — degrade honestly to "unavailable"
	//    rather than returning current bytes as if they were historical.
	if (IsBinaryOrTooLarge(Filename, WorkingBytes))
	{
		UE_LOG(LogBranchiveSourceControl, Log,
			TEXT("Historical content for '%s' @ rev %d is binary/too large; cannot reconstruct via file diff (contract §4.13)."),
			*Filename, RevisionNumber);
		return false;
	}

	FString WorkingStr;
	FFileHelper::BufferToString(WorkingStr, WorkingBytes.GetData(), WorkingBytes.Num());
	const std::string Working = ToU8(WorkingStr);

	// 3. `lore file diff --source <fullSig> <absFile>` (working tree is the target — §4.13 step 3).
	FLoreCli Cli(PathToLoreBinary, PathToRepositoryRoot);
	const FLoreCliResult Res = Cli.Run({ TEXT("file"), TEXT("diff"), TEXT("--source"), FullSignature, Filename });
	if (!Res.Ok())
	{
		// Includes the live exit-2 "Address not found" content-fetch failure
		// (§4.11/§4.13 step 6) — never propagate the raw Rust error to the UI.
		const BranchiveLore::EErrorClass Ec = BranchiveLore::ClassifyError(
			Res.ReturnCode, Res.bSpawnFailed, ToU8(Res.StdErr), ToU8(Res.StdOut));
		UE_LOG(LogBranchiveSourceControl, Log,
			TEXT("file diff --source failed for '%s' @ rev %d: %s"),
			*Filename, RevisionNumber, *ToF(BranchiveLore::FriendlyMessage(Ec)));
		return false;
	}

	// 4. Reconstruct the historical content (§4.13 steps 4-5).
	std::vector<std::string> WorkingLines;
	SplitLinesNormalized(Working, WorkingLines);

	const std::vector<BranchiveLore::FDiffFile> Files = BranchiveLore::ParseUnifiedDiff(ToU8(Res.StdOut));

	// Collect all hunks (there is only ever one file entry for `file diff`, but be
	// defensive). A binary marker here means the underlying content is binary after
	// all — bail rather than returning working bytes.
	std::vector<BranchiveLore::FDiffHunk> Hunks;
	for (const BranchiveLore::FDiffFile& F : Files)
	{
		if (F.bBinary) { return false; }
		for (const BranchiveLore::FDiffHunk& H : F.Hunks) { Hunks.push_back(H); }
	}

	std::string OldContent;
	if (Hunks.empty())
	{
		// Zero hunks => unchanged between <sig> and the working tree (§4.13 step 4).
		OldContent = Working;
	}
	else
	{
		OldContent = JoinLines(BranchiveLore::ReconstructOldContent(WorkingLines, Hunks));
	}

	// 5. Materialize to a temp file that KEEPS the asset's extension so UE's diff
	//    tooling can load it.
	if (InOutFilename.IsEmpty())
	{
		const FString DiffDir = FPaths::ConvertRelativePathToFull(FPaths::DiffDir());
		IFileManager::Get().MakeDirectory(*DiffDir, /*Tree=*/true);
		const FString Base = FPaths::GetBaseFilename(Filename);
		const FString Ext = FPaths::GetExtension(Filename);
		FString ShortSig = FullSignature.Left(8);
		FString Leaf = FString::Printf(TEXT("%s-rev%d-%s"), *Base, RevisionNumber, *ShortSig);
		if (!Ext.IsEmpty()) { Leaf += TEXT(".") + Ext; }
		InOutFilename = FPaths::Combine(DiffDir, Leaf);
	}

	if (!FFileHelper::SaveStringToFile(ToF(OldContent), *InOutFilename))
	{
		return false;
	}
	return true;
}

bool FBranchiveSourceControlRevision::GetAnnotated(TArray<FAnnotationLine>& /*OutLines*/) const
{
	// Lore has no per-line blame/annotation data, and no author anywhere in its
	// history output (contract §4.12 rule 5). Refuse rather than fabricate.
	return false;
}

bool FBranchiveSourceControlRevision::GetAnnotated(FString& InOutFilename) const
{
	// No annotation metadata available — best we can do is materialize the plain
	// revision content (same as Get()).
	return Get(InOutFilename);
}
