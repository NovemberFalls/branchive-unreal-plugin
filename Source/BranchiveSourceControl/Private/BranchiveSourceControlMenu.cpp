// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "BranchiveSourceControlMenu.h"

#include "BranchiveSourceControlConflictOperations.h"
#include "BranchiveSourceControlLog.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Modules/ModuleManager.h"
#include "SourceControlHelpers.h"

#define LOCTEXT_NAMESPACE "BranchiveSourceControl.Menu"

namespace
{
	const FName BranchiveProviderName("Branchive");

	// Map selected assets -> on-disk filenames (the CLI operates on absolute paths).
	TArray<FString> AssetsToFilenames(const TArray<FAssetData>& Assets)
	{
		TArray<FString> Filenames;
		Filenames.Reserve(Assets.Num());
		for (const FAssetData& Asset : Assets)
		{
			const FString PackageName = Asset.PackageName.ToString();
			const FString Filename = USourceControlHelpers::PackageFilename(PackageName);
			if (!Filename.IsEmpty())
			{
				Filenames.Add(Filename);
			}
		}
		return Filenames;
	}

	// The subset of Filenames that the active provider currently reports as conflicted.
	TArray<FString> ConflictedSubset(const TArray<FString>& Filenames)
	{
		TArray<FString> Conflicted;
		if (Filenames.Num() == 0)
		{
			return Conflicted;
		}
		ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
		TArray<FSourceControlStateRef> States;
		if (Provider.GetState(Filenames, States, EStateCacheUsage::Use) != ECommandResult::Succeeded)
		{
			return Conflicted;
		}
		for (const FSourceControlStateRef& State : States)
		{
			if (State->IsConflicted())
			{
				Conflicted.Add(State->GetFilename());
			}
		}
		return Conflicted;
	}

	void DispatchResolve(TArray<FString> Files, bool bTheirs)
	{
		if (Files.Num() == 0)
		{
			return;
		}
		ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
		if (bTheirs)
		{
			Provider.Execute(ISourceControlOperation::Create<FBranchiveResolveTheirs>(), Files);
		}
		else
		{
			Provider.Execute(ISourceControlOperation::Create<FBranchiveResolveMine>(), Files);
		}
	}

	void DispatchAbort(TArray<FString> Files)
	{
		// `lore branch merge abort` ignores the file list (whole-merge op); pass the
		// conflicted files anyway so the states refresh.
		ISourceControlModule::Get().GetProvider().Execute(
			ISourceControlOperation::Create<FBranchiveAbortMerge>(), Files);
	}

	void BuildConflictSection(FMenuBuilder& MenuBuilder, TArray<FString> ConflictedFiles)
	{
		MenuBuilder.BeginSection("BranchiveConflict", LOCTEXT("BranchiveConflictHeading", "Branchive — Resolve Conflict"));
		{
			MenuBuilder.AddMenuEntry(
				LOCTEXT("ResolveMine", "Resolve Using Mine (Ours)"),
				LOCTEXT("ResolveMineTip", "Keep the current-branch (ours) side of each conflicted file, then check in to complete the merge."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateStatic(&DispatchResolve, ConflictedFiles, /*bTheirs=*/false)));

			MenuBuilder.AddMenuEntry(
				LOCTEXT("ResolveTheirs", "Resolve Using Theirs (Incoming)"),
				LOCTEXT("ResolveTheirsTip", "Keep the incoming (theirs) side of each conflicted file, then check in to complete the merge."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateStatic(&DispatchResolve, ConflictedFiles, /*bTheirs=*/true)));

			MenuBuilder.AddMenuEntry(
				LOCTEXT("AbortMerge", "Abort Merge"),
				LOCTEXT("AbortMergeTip", "Abort the in-progress merge and revert the working tree to its pre-merge state."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateStatic(&DispatchAbort, ConflictedFiles)));
		}
		MenuBuilder.EndSection();
	}
}

void FBranchiveSourceControlMenu::Register()
{
	if (bRegistered)
	{
		return;
	}
	FContentBrowserModule* CBModule = FModuleManager::Get().LoadModulePtr<FContentBrowserModule>("ContentBrowser");
	if (!CBModule)
	{
		return; // no content browser (non-editor context) — nothing to extend
	}
	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = CBModule->GetAllAssetViewContextMenuExtenders();
	Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(
		this, &FBranchiveSourceControlMenu::OnExtendAssetSelectionMenu));
	ContentBrowserExtenderHandle = Extenders.Last().GetHandle();
	bRegistered = true;
}

void FBranchiveSourceControlMenu::Unregister()
{
	if (!bRegistered)
	{
		return;
	}
	if (FContentBrowserModule* CBModule = FModuleManager::Get().GetModulePtr<FContentBrowserModule>("ContentBrowser"))
	{
		TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = CBModule->GetAllAssetViewContextMenuExtenders();
		Extenders.RemoveAll([this](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
		{
			return Delegate.GetHandle() == ContentBrowserExtenderHandle;
		});
	}
	ContentBrowserExtenderHandle.Reset();
	bRegistered = false;
}

TSharedRef<FExtender> FBranchiveSourceControlMenu::OnExtendAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	// Only when Branchive is the active provider — never hijack another SCC's menu.
	if (!ISourceControlModule::Get().IsEnabled() ||
		ISourceControlModule::Get().GetProvider().GetName() != BranchiveProviderName)
	{
		return Extender;
	}

	const TArray<FString> Filenames = AssetsToFilenames(SelectedAssets);
	const TArray<FString> Conflicted = ConflictedSubset(Filenames);
	if (Conflicted.Num() == 0)
	{
		return Extender; // nothing conflicted — no menu entries
	}

	Extender->AddMenuExtension(
		"CommonAssetActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateStatic(&BuildConflictSection, Conflicted));
	return Extender;
}

#undef LOCTEXT_NAMESPACE
