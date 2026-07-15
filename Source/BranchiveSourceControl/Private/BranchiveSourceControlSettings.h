// Copyright 2026 Bits, LLC. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

/**
 * Persisted, editable settings for the Branchive provider. Stored in the
 * editor's SourceControlSettings.ini. The only genuinely user-configurable
 * value for v1 is the path to the `lore` binary (contract §3.3 — the CLI's
 * install location is not standardized; never assume PATH or a fixed path).
 */
class FBranchiveSourceControlSettings
{
public:
	/** Path to the `lore` binary (may be empty -> resolve via LORE_BIN / PATH). */
	FString GetLoreBinaryPath() const;
	bool SetLoreBinaryPath(const FString& InString);

	void LoadSettings();
	void SaveSettings() const;

private:
	mutable FCriticalSection CriticalSection;
	FString LoreBinaryPath;
};
