// Copyright Branchive.
#pragma once

#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class FBranchiveSourceControlCommand;

/** One worker class per source-control operation (contract §5.3 mapping). */
class IBranchiveSourceControlWorker
{
public:
	virtual ~IBranchiveSourceControlWorker() = default;

	/** Operation name this worker handles (matches ISourceControlOperation::GetName). */
	virtual FName GetName() const = 0;

	/** The real work. Runs on a background (thread-pool) thread. */
	virtual bool Execute(FBranchiveSourceControlCommand& InCommand) = 0;

	/** Apply results to the provider's state cache. Always on the game thread. */
	virtual bool UpdateStates() const = 0;
};

typedef TSharedRef<IBranchiveSourceControlWorker, ESPMode::ThreadSafe> FBranchiveSourceControlWorkerRef;
