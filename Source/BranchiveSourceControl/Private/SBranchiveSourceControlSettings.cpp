// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "SBranchiveSourceControlSettings.h"

#include "BranchiveSourceControlModule.h"
#include "Cloud/BranchiveCloudAuth.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
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

			// Row 5: Branchive Cloud sign-in status (read-only, live).
			+ SGridPanel::Slot(0, 5).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("CloudLabel", "Branchive Cloud"))
			]
			+ SGridPanel::Slot(1, 5).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(this, &SBranchiveSourceControlSettings::GetCloudStatusText)
			]

			// Row 6: sign-in server (read-only).
			+ SGridPanel::Slot(0, 6).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("CloudServerLabel", "Sign-in server"))
			]
			+ SGridPanel::Slot(1, 6).Padding(Pad).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(this, &SBranchiveSourceControlSettings::GetCloudServerText)
			]

			// Row 7: the Sign in / Sign out button (user-initiated — §0/§3.4).
			+ SGridPanel::Slot(1, 7).Padding(Pad).HAlign(HAlign_Left)
			[
				SNew(SButton)
				.Text(this, &SBranchiveSourceControlSettings::GetSignInButtonText)
				.IsEnabled(this, &SBranchiveSourceControlSettings::IsSignInButtonEnabled)
				.OnClicked(this, &SBranchiveSourceControlSettings::OnSignInOutClicked)
				.ToolTipText(LOCTEXT("CloudSignInTooltip", "Sign in to Branchive Cloud in your web browser (PKCE). Not required for a self-hosted or local-only workspace."))
			]
		]
	];
}

FText SBranchiveSourceControlSettings::GetCloudStatusText() const
{
	FBranchiveCloudAuth* Cloud = FBranchiveCloudAuth::Get();
	if (!Cloud)
	{
		return LOCTEXT("CloudUnavailable", "(unavailable)");
	}
	if (!Cloud->IsSignedIn())
	{
		return LOCTEXT("CloudSignedOut", "Signed out");
	}
	const FString Label = Cloud->IdentityLabel();
	FText Base = Label.IsEmpty()
		? LOCTEXT("CloudSignedIn", "Signed in")
		: FText::Format(LOCTEXT("CloudSignedInAs", "Signed in as {0}"), FText::FromString(Label));
	if (!Cloud->IsTokenPersistent())
	{
		return FText::Format(
			LOCTEXT("CloudSignedInNonPersistent", "{0}  (session not persisted on this platform — re-auth next session)"),
			Base);
	}
	return Base;
}

FText SBranchiveSourceControlSettings::GetCloudServerText() const
{
	if (FBranchiveCloudAuth* Cloud = FBranchiveCloudAuth::Get())
	{
		return FText::FromString(Cloud->BffBaseUrl());
	}
	return FText::GetEmpty();
}

FText SBranchiveSourceControlSettings::GetSignInButtonText() const
{
	FBranchiveCloudAuth* Cloud = FBranchiveCloudAuth::Get();
	if (Cloud && Cloud->IsSignedIn())
	{
		return LOCTEXT("CloudSignOutBtn", "Sign out of Branchive");
	}
	return LOCTEXT("CloudSignInBtn", "Sign in to Branchive");
}

bool SBranchiveSourceControlSettings::IsSignInButtonEnabled() const
{
	return FBranchiveCloudAuth::Get() != nullptr;
}

FReply SBranchiveSourceControlSettings::OnSignInOutClicked()
{
	if (FBranchiveCloudAuth* Cloud = FBranchiveCloudAuth::Get())
	{
		if (Cloud->IsSignedIn())
		{
			Cloud->SignOut();
		}
		else
		{
			Cloud->SignInAsync();
		}
	}
	return FReply::Handled();
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
