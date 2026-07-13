// Copyright Branchive.
#include "BranchiveSourceControlProvider.h"

#include "BranchiveSourceControlCommand.h"
#include "BranchiveSourceControlLog.h"
#include "BranchiveSourceControlModule.h"
#include "BranchiveSourceControlState.h"
#include "Lore/LoreCli.h"
#include "SBranchiveSourceControlSettings.h"

#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "ScopedSourceControlProgress.h"
#include "Logging/MessageLog.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/QueuedThreadPool.h"
#include "Modules/ModuleManager.h"
#include "HAL/PlatformProcess.h"

#define LOCTEXT_NAMESPACE "BranchiveSourceControl"

static FName ProviderName("Branchive");

void FBranchiveSourceControlProvider::Init(bool /*bForceConnection*/)
{
	CheckLoreAvailability();
	CheckWorkspaceStatus();
}

void FBranchiveSourceControlProvider::CheckLoreAvailability()
{
	FBranchiveSourceControlModule& Module = FModuleManager::LoadModuleChecked<FBranchiveSourceControlModule>("BranchiveSourceControl");
	const FString Configured = Module.AccessSettings().GetLoreBinaryPath();
	PathToLoreBinary = FLoreCli::ResolveBinaryPath(Configured);

	// Confirm the binary actually runs (`lore --version`). Never a shell.
	FLoreCli Cli(PathToLoreBinary, FString());
	const FLoreCliResult Result = Cli.Run({ TEXT("--version") }, /*bAppendRepository=*/false);
	bLoreAvailable = !Result.bSpawnFailed;
	if (!bLoreAvailable)
	{
		UE_LOG(LogBranchiveSourceControl, Warning,
			TEXT("Could not run the 'lore' binary at '%s'. Set the path in Revision Control settings."), *PathToLoreBinary);
	}
}

void FBranchiveSourceControlProvider::CheckWorkspaceStatus()
{
	PathToRepositoryRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	PathToRepositoryRoot.RemoveFromEnd(TEXT("/"));

	// A folder is a Lore workspace iff it has a .lore/ subdirectory (contract §2.1).
	const FString DotLore = FPaths::Combine(PathToRepositoryRoot, TEXT(".lore"));
	bWorkspaceFound = FPaths::DirectoryExists(DotLore);

	RemoteUrl.Empty();
	if (bWorkspaceFound)
	{
		// Read remote_url from .lore/config.toml with a plain key read (contract §2.2)
		// — a top-level key above the first [section].
		const FString ConfigPath = FPaths::Combine(DotLore, TEXT("config.toml"));
		FString ConfigText;
		if (FFileHelper::LoadFileToString(ConfigText, *ConfigPath))
		{
			TArray<FString> Lines;
			ConfigText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
			for (const FString& Raw : Lines)
			{
				FString Line = Raw.TrimStartAndEnd();
				if (Line.StartsWith(TEXT("[")))
				{
					break; // reached the first [section]; stop.
				}
				if (Line.StartsWith(TEXT("remote_url")))
				{
					int32 Q1, Q2;
					if (Line.FindChar(TEXT('"'), Q1) && Line.FindLastChar(TEXT('"'), Q2) && Q2 > Q1)
					{
						RemoteUrl = Line.Mid(Q1 + 1, Q2 - Q1 - 1);
					}
					break;
				}
			}
		}
	}

	if (!bWorkspaceFound)
	{
		UE_LOG(LogBranchiveSourceControl, Warning, TEXT("'%s' is not a Lore workspace (no .lore/ folder)."), *PathToRepositoryRoot);
	}
}

void FBranchiveSourceControlProvider::Close()
{
	StateCache.Empty();
	bLoreAvailable = false;
	bWorkspaceFound = false;
	BranchName.Empty();
	{
		FScopeLock Lock(&SessionLockMutex);
		SessionLockedFiles.Empty();
	}
}

TSharedRef<FBranchiveSourceControlState, ESPMode::ThreadSafe> FBranchiveSourceControlProvider::GetStateInternal(const FString& Filename)
{
	if (TSharedRef<FBranchiveSourceControlState, ESPMode::ThreadSafe>* State = StateCache.Find(Filename))
	{
		return *State;
	}
	TSharedRef<FBranchiveSourceControlState, ESPMode::ThreadSafe> NewState = MakeShared<FBranchiveSourceControlState, ESPMode::ThreadSafe>(Filename);
	StateCache.Add(Filename, NewState);
	return NewState;
}

bool FBranchiveSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	return StateCache.Remove(Filename) > 0;
}

void FBranchiveSourceControlProvider::AddSessionLock(const FString& AbsFile)
{
	FScopeLock Lock(&SessionLockMutex);
	SessionLockedFiles.Add(AbsFile);
}

void FBranchiveSourceControlProvider::RemoveSessionLock(const FString& AbsFile)
{
	FScopeLock Lock(&SessionLockMutex);
	SessionLockedFiles.Remove(AbsFile);
}

TSet<FString> FBranchiveSourceControlProvider::GetSessionLockedFilesSnapshot() const
{
	FScopeLock Lock(&SessionLockMutex);
	return SessionLockedFiles;
}

FText FBranchiveSourceControlProvider::GetStatusText() const
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("WorkspacePath"), FText::FromString(PathToRepositoryRoot));
	Args.Add(TEXT("RemoteUrl"), FText::FromString(RemoteUrl.IsEmpty() ? TEXT("(local only)") : RemoteUrl));
	Args.Add(TEXT("BranchName"), FText::FromString(BranchName.IsEmpty() ? TEXT("(unknown)") : BranchName));
	return FText::Format(
		LOCTEXT("ProviderStatusText", "Provider: Branchive\nWorkspace: {WorkspacePath}\nRemote: {RemoteUrl}\nBranch: {BranchName}\nContract: 2.0.0"),
		Args);
}

TMap<ISourceControlProvider::EStatus, FString> FBranchiveSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, (IsEnabled() && IsAvailable()) ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Workspace, PathToRepositoryRoot);
	Result.Add(EStatus::WorkspacePath, PathToRepositoryRoot);
	Result.Add(EStatus::Remote, RemoteUrl);
	Result.Add(EStatus::Branch, BranchName);
	Result.Add(EStatus::ScmVersion, TEXT("lore"));
	Result.Add(EStatus::PluginVersion, TEXT("0.3.5 (contract 2.0.0)"));
	return Result;
}

bool FBranchiveSourceControlProvider::IsEnabled() const
{
	return bWorkspaceFound;
}

bool FBranchiveSourceControlProvider::IsAvailable() const
{
	return bWorkspaceFound && bLoreAvailable;
}

const FName& FBranchiveSourceControlProvider::GetName() const
{
	return ProviderName;
}

ECommandResult::Type FBranchiveSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	const TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		Execute(ISourceControlOperation::Create<FUpdateStatus>(), AbsoluteFiles);
	}

	for (const FString& AbsoluteFile : AbsoluteFiles)
	{
		OutState.Add(GetStateInternal(AbsoluteFile));
	}
	return ECommandResult::Succeeded;
}

ECommandResult::Type FBranchiveSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>&, TArray<FSourceControlChangelistStateRef>&, EStateCacheUsage::Type)
{
	return ECommandResult::Failed; // changelists are not modeled (UsesChangelists == false)
}

TArray<FSourceControlStateRef> FBranchiveSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	for (const auto& CacheItem : StateCache)
	{
		FSourceControlStateRef State = CacheItem.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

FDelegateHandle FBranchiveSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FBranchiveSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

ECommandResult::Type FBranchiveSourceControlProvider::Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr /*InChangelist*/, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
	// Only "Connect" is allowed while the workspace is not enabled.
	if (!IsEnabled() && InOperation->GetName() != "Connect")
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	const TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	TSharedPtr<IBranchiveSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("OperationName"), FText::FromName(InOperation->GetName()));
		Arguments.Add(TEXT("ProviderName"), FText::FromName(GetName()));
		const FText Message = FText::Format(LOCTEXT("UnsupportedOperation", "Operation '{OperationName}' not supported by revision control provider '{ProviderName}'"), Arguments);
		FMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	FBranchiveSourceControlCommand* Command = new FBranchiveSourceControlCommand(InOperation, Worker.ToSharedRef(), InOperationCompleteDelegate);
	Command->Files = AbsoluteFiles;
	Command->PathToLoreBinary = PathToLoreBinary;
	Command->PathToRepositoryRoot = PathToRepositoryRoot;
	Command->RemoteUrl = RemoteUrl;
	Command->BranchName = BranchName;
	Command->WorkspaceMutex = &WorkspaceMutex;
	Command->SessionLockedFiles = GetSessionLockedFilesSnapshot();

	if (InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;
		return ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString());
	}
	Command->bAutoDelete = true;
	return IssueCommand(*Command);
}

bool FBranchiveSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	return WorkersMap.Find(InOperation->GetName()) != nullptr;
}

bool FBranchiveSourceControlProvider::CanCancelOperation(const FSourceControlOperationRef&) const
{
	return false; // v1: spawn-per-op is not cancelable (see heartbeat/cancel deferral).
}

void FBranchiveSourceControlProvider::CancelOperation(const FSourceControlOperationRef&) {}

bool FBranchiveSourceControlProvider::UsesLocalReadOnlyState() const { return false; } // contract §5.3 (deferred)
bool FBranchiveSourceControlProvider::UsesChangelists() const { return false; }
bool FBranchiveSourceControlProvider::UsesUncontrolledChangelists() const { return false; }
bool FBranchiveSourceControlProvider::UsesCheckout() const { return true; }             // contract §5.3 (lock == checkout)
bool FBranchiveSourceControlProvider::UsesFileRevisions() const { return true; }
bool FBranchiveSourceControlProvider::UsesSnapshots() const { return false; }
bool FBranchiveSourceControlProvider::AllowsDiffAgainstDepot() const { return true; }
TOptional<bool> FBranchiveSourceControlProvider::IsAtLatestRevision() const { return TOptional<bool>(); }
TOptional<int> FBranchiveSourceControlProvider::GetNumLocalChanges() const { return TOptional<int>(); }

TSharedPtr<IBranchiveSourceControlWorker, ESPMode::ThreadSafe> FBranchiveSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	if (const FGetBranchiveSourceControlWorker* Operation = WorkersMap.Find(InOperationName))
	{
		return Operation->Execute();
	}
	return nullptr;
}

void FBranchiveSourceControlProvider::RegisterWorker(const FName& InName, const FGetBranchiveSourceControlWorker& InDelegate)
{
	WorkersMap.Add(InName, InDelegate);
}

void FBranchiveSourceControlProvider::OutputCommandMessages(const FBranchiveSourceControlCommand& InCommand) const
{
	FMessageLog SourceControlLog("SourceControl");
	for (const FString& Error : InCommand.ErrorMessages)
	{
		SourceControlLog.Error(FText::FromString(Error));
	}
	for (const FString& Info : InCommand.InfoMessages)
	{
		SourceControlLog.Info(FText::FromString(Info));
	}
}

void FBranchiveSourceControlProvider::Tick()
{
	bool bStatesUpdated = false;
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FBranchiveSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.bExecuteProcessed)
		{
			CommandQueue.RemoveAt(CommandIndex);

			bStatesUpdated |= Command.Worker->UpdateStates();
			OutputCommandMessages(Command);
			Command.ReturnResults();

			if (Command.bAutoDelete)
			{
				delete &Command;
			}
			// Only one command per tick to avoid concurrent modification of the
			// queue from a completion delegate.
			break;
		}
	}

	if (bStatesUpdated)
	{
		OnSourceControlStateChanged.Broadcast();
	}
}

TArray<TSharedRef<ISourceControlLabel>> FBranchiveSourceControlProvider::GetLabels(const FString&) const
{
	return TArray<TSharedRef<ISourceControlLabel>>();
}

TArray<FSourceControlChangelistRef> FBranchiveSourceControlProvider::GetChangelists(EStateCacheUsage::Type)
{
	return TArray<FSourceControlChangelistRef>();
}

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<class SWidget> FBranchiveSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SBranchiveSourceControlSettings);
}
#endif

ECommandResult::Type FBranchiveSourceControlProvider::ExecuteSynchronousCommand(FBranchiveSourceControlCommand& InCommand, const FText& Task)
{
	ECommandResult::Type Result = ECommandResult::Failed;
	{
		FScopedSourceControlProgress Progress(Task);

		IssueCommand(InCommand);

		while (!InCommand.bExecuteProcessed)
		{
			Tick();
			Progress.Tick();
			FPlatformProcess::Sleep(0.01f);
		}
		Tick();

		if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
	}

	check(!InCommand.bAutoDelete);
	if (CommandQueue.Contains(&InCommand))
	{
		CommandQueue.Remove(&InCommand);
	}
	delete &InCommand;
	return Result;
}

ECommandResult::Type FBranchiveSourceControlProvider::IssueCommand(FBranchiveSourceControlCommand& InCommand)
{
	if (GThreadPool != nullptr)
	{
		GThreadPool->AddQueuedWork(&InCommand);
		CommandQueue.Add(&InCommand);
		return ECommandResult::Succeeded;
	}

	const FText Message(LOCTEXT("NoSCCThreads", "There are no threads available to process the revision control command."));
	FMessageLog("SourceControl").Error(Message);
	InCommand.bCommandSuccessful = false;
	InCommand.Operation->AddErrorMessge(Message);
	return InCommand.ReturnResults();
}

#undef LOCTEXT_NAMESPACE
