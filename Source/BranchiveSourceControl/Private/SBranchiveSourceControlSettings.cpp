// Copyright Branchive.
#include "SBranchiveSourceControlSettings.h"

#include "BranchiveSourceControlModule.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SGridPanel.h"

#define LOCTEXT_NAMESPACE "BranchiveSourceControl.Settings"

void SBranchiveSourceControlSettings::Construct(const FArguments& InArgs)
{
	const float Pad = 4.0f;

	ChildSlot
	[
		SNew(SBorder)
		.Padding(FMargin(6.0f))
		[
			SNew(SGridPanel)
			.FillColumn(1, 1.0f)

			// Row 0: lore binary path (editable).
			+ SGridPanel::Slot(0, 0).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("BinaryPathLabel", "Path to 'lore' binary"))
			]
			+ SGridPanel::Slot(1, 0).Padding(Pad)
			[
				SNew(SEditableTextBox)
				.Text(this, &SBranchiveSourceControlSettings::GetBinaryPathText)
				.HintText(LOCTEXT("BinaryPathHint", "Leave empty to use LORE_BIN / PATH"))
				.OnTextCommitted(this, &SBranchiveSourceControlSettings::OnBinaryPathCommitted)
			]

			// Row 1: workspace (read-only).
			+ SGridPanel::Slot(0, 1).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("WorkspaceLabel", "Workspace"))
			]
			+ SGridPanel::Slot(1, 1).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(this, &SBranchiveSourceControlSettings::GetWorkspaceText)
			]

			// Row 2: remote (read-only).
			+ SGridPanel::Slot(0, 2).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("RemoteLabel", "Remote"))
			]
			+ SGridPanel::Slot(1, 2).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(this, &SBranchiveSourceControlSettings::GetRemoteText)
			]

			// Row 3: branch (read-only).
			+ SGridPanel::Slot(0, 3).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("BranchLabel", "Branch"))
			]
			+ SGridPanel::Slot(1, 3).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(this, &SBranchiveSourceControlSettings::GetBranchText)
			]

			// Row 4: contract version (read-only).
			+ SGridPanel::Slot(0, 4).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("ContractLabel", "Integration contract"))
			]
			+ SGridPanel::Slot(1, 4).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(this, &SBranchiveSourceControlSettings::GetContractText)
			]
		]
	];
}

FText SBranchiveSourceControlSettings::GetBinaryPathText() const
{
	if (FBranchiveSourceControlModule* Module = FBranchiveSourceControlModule::GetThreadSafe())
	{
		return FText::FromString(Module->AccessSettings().GetLoreBinaryPath());
	}
	return FText::GetEmpty();
}

void SBranchiveSourceControlSettings::OnBinaryPathCommitted(const FText& InText, ETextCommit::Type)
{
	if (FBranchiveSourceControlModule* Module = FBranchiveSourceControlModule::GetThreadSafe())
	{
		if (Module->AccessSettings().SetLoreBinaryPath(InText.ToString().TrimStartAndEnd()))
		{
			Module->SaveSettings();
			// Re-resolve the binary / workspace for the new setting.
			Module->GetProvider().CheckLoreAvailability();
		}
	}
}

FText SBranchiveSourceControlSettings::GetWorkspaceText() const
{
	if (FBranchiveSourceControlModule* Module = FBranchiveSourceControlModule::GetThreadSafe())
	{
		return FText::FromString(Module->GetProvider().GetPathToRepositoryRoot());
	}
	return FText::GetEmpty();
}

FText SBranchiveSourceControlSettings::GetRemoteText() const
{
	if (FBranchiveSourceControlModule* Module = FBranchiveSourceControlModule::GetThreadSafe())
	{
		const FString Remote = Module->GetProvider().GetRemoteUrl();
		return FText::FromString(Remote.IsEmpty() ? TEXT("(local only)") : Remote);
	}
	return FText::GetEmpty();
}

FText SBranchiveSourceControlSettings::GetBranchText() const
{
	if (FBranchiveSourceControlModule* Module = FBranchiveSourceControlModule::GetThreadSafe())
	{
		const FString Branch = Module->GetProvider().GetBranchName();
		return FText::FromString(Branch.IsEmpty() ? TEXT("(unknown)") : Branch);
	}
	return FText::GetEmpty();
}

FText SBranchiveSourceControlSettings::GetContractText() const
{
	return LOCTEXT("ContractVersion", "2.0.0");
}

#undef LOCTEXT_NAMESPACE
