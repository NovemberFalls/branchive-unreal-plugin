// Copyright Branchive.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

namespace ETextCommit { enum Type : int; }

/**
 * Minimal settings widget shown in the Revision Control login window: the path
 * to the `lore` binary, plus read-only workspace/remote/branch info.
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
};
