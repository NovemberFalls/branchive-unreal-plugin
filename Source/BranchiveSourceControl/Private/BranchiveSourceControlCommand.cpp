// Copyright 2026 Bits, LLC. All Rights Reserved.
#include "BranchiveSourceControlCommand.h"

#include "IBranchiveSourceControlWorker.h"
#include "ISourceControlOperation.h"
#include "HAL/PlatformAtomics.h"

FBranchiveSourceControlCommand::FBranchiveSourceControlCommand(
	const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation,
	const TSharedRef<class IBranchiveSourceControlWorker, ESPMode::ThreadSafe>& InWorker,
	const FSourceControlOperationComplete& InOperationCompleteDelegate)
	: Operation(InOperation)
	, Worker(InWorker)
	, OperationCompleteDelegate(InOperationCompleteDelegate)
{
}

bool FBranchiveSourceControlCommand::DoWork()
{
	bCommandSuccessful = Worker->Execute(*this);
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
	return bCommandSuccessful;
}

void FBranchiveSourceControlCommand::Abandon()
{
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
}

void FBranchiveSourceControlCommand::DoThreadedWork()
{
	Concurrency = EConcurrency::Asynchronous;
	DoWork();
}

ECommandResult::Type FBranchiveSourceControlCommand::ReturnResults()
{
	// Copy any messages that the worker collected into the operation's result.
	for (const FString& Info : InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(Info));
	}
	for (const FString& Error : ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(Error));
	}

	const ECommandResult::Type Result = bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed;
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);
	return Result;
}
