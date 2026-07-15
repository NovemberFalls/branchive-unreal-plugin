// Copyright 2026 Bits, LLC. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

namespace ETextCommit { enum Type : int; }

/**
 * Settings widget shown in the Revision Control login window: the path to the
 * `lore` binary, read-only workspace/remote/branch info, and the Branchive Cloud
 * sign-in surface (§3.4 "connection-status affordance" + §0 user-initiated sign-in)
 * — a live signed-in identity label plus a Sign in / Sign out button.
 */
class SBranchiveSourceControlSettings : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBranchiveSourceControlSettings) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FText GetBinaryPathText() const;
	void OnBinaryPathCommitted(const FText& InText, ETextCommit::Type InCommitType);

	FText GetWorkspaceText() const;
	FText GetRemoteText() const;
	FText GetBranchText() const;
	FText GetContractText() const;

	// ── Branchive Cloud sign-in ──
	FText GetCloudStatusText() const;   // "Signed in as @handle" / "Signed out"
	FText GetCloudServerText() const;   // the BFF base URL the flow targets
	FText GetSignInButtonText() const;  // "Sign in to Branchive" / "Sign out"
	bool  IsSignInButtonEnabled() const;
	FReply OnSignInOutClicked();
};
