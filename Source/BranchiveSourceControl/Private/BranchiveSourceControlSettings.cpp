// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "BranchiveSourceControlSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "SourceControlHelpers.h"

namespace
{
	const TCHAR* SettingsSection = TEXT("BranchiveSourceControl.BranchiveSourceControlSettings");
}

FString FBranchiveSourceControlSettings::GetLoreBinaryPath() const
{
	FScopeLock ScopeLock(&CriticalSection);
	return LoreBinaryPath;
}

bool FBranchiveSourceControlSettings::SetLoreBinaryPath(const FString& InString)
{
	FScopeLock ScopeLock(&CriticalSection);
	const bool bChanged = !LoreBinaryPath.Equals(InString, ESearchCase::CaseSensitive);
	if (bChanged)
	{
		LoreBinaryPath = InString;
	}
	return bChanged;
}

void FBranchiveSourceControlSettings::LoadSettings()
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->GetString(SettingsSection, TEXT("LoreBinaryPath"), LoreBinaryPath, IniFile);
}

void FBranchiveSourceControlSettings::SaveSettings() const
{
	FScopeLock ScopeLock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->SetString(SettingsSection, TEXT("LoreBinaryPath"), *LoreBinaryPath, IniFile);
}
