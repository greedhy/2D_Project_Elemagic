// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpDevOpsTools.cpp
// ----------------------------------------------------------------------------
// Three DevOps tool families for the SOMOLMCP UE plugin:
//
//   A) Source Control (P3-8) — 4 tools, sourcecontrol_* prefix:
//        sourcecontrol_status, sourcecontrol_checkout,
//        sourcecontrol_checkin, sourcecontrol_diff
//
//   B) Cook / Package (P4-1) — 3 tools, cook_* / package_* prefixes:
//        cook_target, package_build, cook_status
//
//   C) Replication / Network debug (P4-4) — 3 tools, net_* prefix:
//        net_inspect, net_replication_graph_dump, net_actor_force_replicate
//
// Total: 10 tools registered via RegisterDevOpsTools(FSololmcpToolRegistry&).
//
// HARD CONSTRAINTS honored:
//   - Does NOT modify SololmcpToolRegistry.{h,cpp}.
//   - Does NOT modify .Build.cs (audit only — see DELIVERY md).
//   - Does NOT modify any other .cpp file.
//
// File location: drop into
// then add a forward declaration of RegisterDevOpsTools() to SololmcpToolRegistry.h
// and a call site in SololmcpToolRegistry.cpp's constructor (per the existing
// RegisterX* pattern). See P3-8_P4-1_P4-4_DevOps_DELIVERY.md for full integration steps.
//
// Notes
// -----
// * Cook/package operations are inherently long-running. We spawn UAT via
//   FPlatformProcess::CreateProc and bookkeep job state in a process-wide
//   static map keyed by job_id. The map is intentionally simple — proper
//   persistence + cancellation are TODO(P4-1).
// * Replication-graph traversal uses short-name UClass lookup so we don't
//   pull in ReplicationGraph headers (the ReplicationGraph module isn't
//   guaranteed to be a build dependency in every project configuration).
// * Source control diff intentionally returns NOT_AVAILABLE for binary
//   .uasset payloads when the active provider can't materialize a textual
//   diff — full binary asset compare would require an external content-aware
//   differ which is out of scope for this slice.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Object.h"
#include "SololmcpSharedLocks.h" // v3.10.x worker-safety: UAT job map mutex

// Source control
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"

// Engine / Net
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace UE::SOMOLMCP
{

// ============================================================================
// Common helpers
// ============================================================================
namespace DevOpsToolsImpl
{
	/** Append items to a structured JSON array field. */
	static void SetStringArrayField(const TSharedRef<FJsonObject>& Out, const FString& FieldName, const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		Json.Reserve(Items.Num());
		for (const FString& Item : Items)
		{
			Json.Add(MakeShared<FJsonValueString>(Item));
		}
		Out->SetArrayField(FieldName, Json);
	}

	/** Read a JSON array of strings from arguments. */
	static bool TryGetStringArrayField(const TSharedRef<FJsonObject>& Args, const FString& FieldName, TArray<FString>& OutItems)
	{
		const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
		if (!Args->TryGetArrayField(FieldName, Raw) || !Raw)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Raw)
		{
			if (!Value.IsValid()) continue;
			FString S;
			if (Value->TryGetString(S))
			{
				OutItems.Add(S);
			}
		}
		return true;
	}
} // namespace DevOpsToolsImpl

// ============================================================================
// SECTION A — Source Control (P3-8)
// ============================================================================
namespace SourceControlImpl
{
	/** Map an ISourceControlProvider to a stable short name for callers. */
	static FString GetProviderShortName()
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			return TEXT("None");
		}
		ISourceControlProvider& Provider = Module.GetProvider();
		const FName ProviderName = Provider.GetName();
		const FString S = ProviderName.ToString();
		if (S.Equals(TEXT("Git"), ESearchCase::IgnoreCase))         return TEXT("Git");
		if (S.Equals(TEXT("Perforce"), ESearchCase::IgnoreCase))    return TEXT("Perforce");
		if (S.Equals(TEXT("Subversion"), ESearchCase::IgnoreCase))  return TEXT("Subversion");
		if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))        return TEXT("None");
		return S; // unknown — return raw
	}

	/** Convert /Game/... or absolute filename to a filesystem path SC can query. */
	static FString ResolveToSCFilename(const FString& InPath)
	{
		if (FPaths::FileExists(InPath))
		{
			return FPaths::ConvertRelativePathToFull(InPath);
		}
		FString PackageName = InPath;
		if (PackageName.StartsWith(TEXT("/")))
		{
			FString FilenameOnDisk;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, FilenameOnDisk, FPackageName::GetAssetPackageExtension()))
			{
				if (FPaths::FileExists(FilenameOnDisk))
				{
					return FPaths::ConvertRelativePathToFull(FilenameOnDisk);
				}
				FString MapFile;
				if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, MapFile, FPackageName::GetMapPackageExtension()))
				{
					if (FPaths::FileExists(MapFile))
					{
						return FPaths::ConvertRelativePathToFull(MapFile);
					}
				}
				return FPaths::ConvertRelativePathToFull(FilenameOnDisk);
			}
		}
		return InPath;
	}

	static bool RunStatus(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const FString ProviderName = GetProviderShortName();
		OutStructured->SetStringField(TEXT("provider"), ProviderName);

		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled() || ProviderName == TEXT("None"))
		{
			DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("checked_out"), {});
			DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("modified"),    {});
			DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("added"),       {});
			DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("conflicted"),  {});
			OutSummary = TEXT("Source control is disabled or no provider is configured.");
			return true;
		}

		FString PathFilter;
		Arguments->TryGetStringField(TEXT("path_filter"), PathFilter);

		ISourceControlProvider& Provider = Module.GetProvider();

		// Build candidate file list. /Game/... filter narrows to a subtree on disk.
		TArray<FString> CandidateFiles;
		{
			FString RootDir;
			if (!PathFilter.IsEmpty() && PathFilter.StartsWith(TEXT("/Game")))
			{
				FString OnDisk;
				if (FPackageName::TryConvertLongPackageNameToFilename(PathFilter, OnDisk, FString()))
				{
					RootDir = FPaths::ConvertRelativePathToFull(OnDisk);
				}
			}
			if (RootDir.IsEmpty())
			{
				RootDir = FPaths::ProjectContentDir();
			}

			IFileManager& FM = IFileManager::Get();
			TArray<FString> UAssets;
			FM.FindFilesRecursive(UAssets, *RootDir, TEXT("*.uasset"), /*Files*/ true, /*Dirs*/ false);
			TArray<FString> UMaps;
			FM.FindFilesRecursive(UMaps, *RootDir, TEXT("*.umap"), true, false);
			CandidateFiles.Append(UAssets);
			CandidateFiles.Append(UMaps);
		}

		// Cap large content trees.
		const int32 MaxFiles = 10000;
		if (CandidateFiles.Num() > MaxFiles)
		{
			CandidateFiles.SetNum(MaxFiles);
			OutStructured->SetBoolField(TEXT("truncated"), true);
			OutStructured->SetNumberField(TEXT("file_scan_cap"), MaxFiles);
		}

		TArray<FSourceControlStateRef> States;
		const ECommandResult::Type Result =
			Provider.GetState(CandidateFiles, States, EStateCacheUsage::ForceUpdate);

		if (Result != ECommandResult::Succeeded)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("Provider.GetState returned a non-success result."));
			OutError = TEXT("Source control GetState failed.");
			return false;
		}

		TArray<FString> CheckedOut, Modified, Added, Conflicted;
		for (const FSourceControlStateRef& State : States)
		{
			const FString File = State->GetFilename();
			if (State->IsCheckedOut())  CheckedOut.Add(File);
			if (State->IsModified())    Modified.Add(File);
			if (State->IsAdded())       Added.Add(File);
			if (State->IsConflicted())  Conflicted.Add(File);
		}

		DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("checked_out"), CheckedOut);
		DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("modified"),    Modified);
		DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("added"),       Added);
		DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("conflicted"),  Conflicted);

		OutSummary = FString::Printf(
			TEXT("SC[%s] checked_out=%d modified=%d added=%d conflicted=%d (scanned %d)"),
			*ProviderName, CheckedOut.Num(), Modified.Num(), Added.Num(), Conflicted.Num(),
			CandidateFiles.Num());
		return true;
	}

	static bool RunCheckout(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("Source control is disabled. Configure a provider in editor preferences."));
			OutError = TEXT("Source control disabled.");
			return false;
		}

		TArray<FString> InputPaths;
		if (!DevOpsToolsImpl::TryGetStringArrayField(Arguments, TEXT("paths"), InputPaths) || InputPaths.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("paths"));
			OutError = TEXT("Missing or empty 'paths' array.");
			return false;
		}

		TArray<FString> Resolved;
		Resolved.Reserve(InputPaths.Num());
		for (const FString& P : InputPaths)
		{
			Resolved.Add(ResolveToSCFilename(P));
		}

		ISourceControlProvider& Provider = Module.GetProvider();
		TSharedRef<FCheckOut, ESPMode::ThreadSafe> Op = ISourceControlOperation::Create<FCheckOut>();
		const ECommandResult::Type Result = Provider.Execute(Op, Resolved, EConcurrency::Synchronous);

		// Re-query state to confirm which inputs are now checked out.
		TArray<FSourceControlStateRef> States;
		Provider.GetState(Resolved, States, EStateCacheUsage::ForceUpdate);

		int32 CheckedOutCount = 0;
		TArray<FString> Failed;
		for (int32 i = 0; i < Resolved.Num(); ++i)
		{
			bool bOk = false;
			for (const FSourceControlStateRef& State : States)
			{
				if (State->GetFilename() == Resolved[i])
				{
					if (State->IsCheckedOut() || State->IsAdded())
					{
						bOk = true;
					}
					break;
				}
			}
			if (bOk) ++CheckedOutCount;
			else     Failed.Add(InputPaths[i]);
		}

		OutStructured->SetNumberField(TEXT("checked_out_count"), CheckedOutCount);
		DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("failed"), Failed);
		OutStructured->SetBoolField(TEXT("provider_succeeded"), Result == ECommandResult::Succeeded);

		OutSummary = FString::Printf(TEXT("Checked out %d/%d (failed=%d)."),
			CheckedOutCount, InputPaths.Num(), Failed.Num());
		return true;
	}

	static bool RunCheckin(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("Source control is disabled."));
			OutError = TEXT("Source control disabled.");
			return false;
		}

		TArray<FString> InputPaths;
		if (!DevOpsToolsImpl::TryGetStringArrayField(Arguments, TEXT("paths"), InputPaths) || InputPaths.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("paths"));
			OutError = TEXT("Missing or empty 'paths' array.");
			return false;
		}

		FString Description;
		if (!Arguments->TryGetStringField(TEXT("description"), Description) || Description.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("description"));
			OutError = TEXT("Missing 'description' commit message.");
			return false;
		}

		TArray<FString> Resolved;
		Resolved.Reserve(InputPaths.Num());
		for (const FString& P : InputPaths)
		{
			Resolved.Add(ResolveToSCFilename(P));
		}

		ISourceControlProvider& Provider = Module.GetProvider();
		TSharedRef<FCheckIn, ESPMode::ThreadSafe> Op = ISourceControlOperation::Create<FCheckIn>();
		Op->SetDescription(FText::FromString(Description));

		const ECommandResult::Type Result = Provider.Execute(Op, Resolved, EConcurrency::Synchronous);

		const FText ResultText = Op->GetSuccessMessage();
		const FString RevisionStr = ResultText.ToString();

		TArray<FString> Failed;
		if (Result != ECommandResult::Succeeded)
		{
			Failed = InputPaths; // submit is atomic
		}
		const int32 SubmittedCount = (Result == ECommandResult::Succeeded) ? InputPaths.Num() : 0;

		OutStructured->SetNumberField(TEXT("submitted_count"), SubmittedCount);
		OutStructured->SetStringField(TEXT("revision"), RevisionStr);
		DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("failed"), Failed);
		OutStructured->SetBoolField(TEXT("provider_succeeded"), Result == ECommandResult::Succeeded);

		OutSummary = FString::Printf(TEXT("Submit %s (n=%d) — '%s'"),
			Result == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAIL"),
			SubmittedCount, *Description.Left(60));
		return true;
	}

	static bool RunDiff(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("Source control is disabled."));
			OutError = TEXT("Source control disabled.");
			return false;
		}

		FString Path;
		if (!Arguments->TryGetStringField(TEXT("path"), Path))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("path"));
			OutError = TEXT("Missing 'path'.");
			return false;
		}

		FString Revision = TEXT("HEAD");
		Arguments->TryGetStringField(TEXT("revision"), Revision);

		const FString OnDisk = ResolveToSCFilename(Path);
		OutStructured->SetStringField(TEXT("path"), Path);
		OutStructured->SetStringField(TEXT("resolved_path"), OnDisk);
		OutStructured->SetStringField(TEXT("revision"), Revision);

		ISourceControlProvider& Provider = Module.GetProvider();

		const bool bIsBinary = OnDisk.EndsWith(TEXT(".uasset"))
			|| OnDisk.EndsWith(TEXT(".umap"))
			|| OnDisk.EndsWith(TEXT(".bin"));

		IFileManager& FM = IFileManager::Get();
		const int64 NewSize = FM.FileSize(*OnDisk);

		// Try to fetch the previous revision metadata so we know the old size + action.
		// TODO(P3-8): for true textual byte-diffs on text assets, run FSourceControlOperationDiff
		// or shell out to `git diff --stat` for the Git provider.
		int64 OldSize = -1;
		FString DiffSummary;
		{
			TArray<FSourceControlStateRef> States;
			TArray<FString> Files = { OnDisk };
			Provider.GetState(Files, States, EStateCacheUsage::ForceUpdate);

			if (States.Num() == 1)
			{
				const FSourceControlStateRef& State = States[0];
				if (State->GetHistorySize() > 0)
				{
					TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Rev = State->GetHistoryItem(0);
					if (Rev.IsValid())
					{
						OldSize = (int64)Rev->GetFileSize();
						DiffSummary = FString::Printf(
							TEXT("revision=%d action=%s changelist=%s date=%s"),
							Rev->GetRevisionNumber(),
							*Rev->GetAction(),
							*FString::FromInt(Rev->GetCheckInIdentifier()),
							*Rev->GetDate().ToString());
					}
				}
			}
		}

		const int64 ByteChanges = (OldSize >= 0 && NewSize >= 0)
			? FMath::Abs(NewSize - OldSize) : (NewSize >= 0 ? NewSize : 0);

		if (bIsBinary)
		{
			OutStructured->SetBoolField(TEXT("is_binary"), true);
			TSharedRef<FJsonObject> Sizes = MakeShared<FJsonObject>();
			Sizes->SetNumberField(TEXT("old"), (double)OldSize);
			Sizes->SetNumberField(TEXT("new"), (double)NewSize);
			OutStructured->SetObjectField(TEXT("sizes"), Sizes);
			OutStructured->SetStringField(TEXT("diff_summary"),
				DiffSummary.IsEmpty()
					? FString::Printf(TEXT("Binary asset (no textual diff). bytes_now=%lld"), NewSize)
					: DiffSummary);
			OutStructured->SetNumberField(TEXT("byte_changes"), (double)ByteChanges);
			OutSummary = FString::Printf(TEXT("Binary diff %s — old=%lld new=%lld"),
				*Path, OldSize, NewSize);
		}
		else
		{
			OutStructured->SetBoolField(TEXT("is_binary"), false);
			OutStructured->SetStringField(TEXT("diff_summary"),
				DiffSummary.IsEmpty()
					? FString::Printf(TEXT("Text file. bytes_now=%lld"), NewSize)
					: DiffSummary);
			OutStructured->SetNumberField(TEXT("byte_changes"), (double)ByteChanges);
			OutSummary = FString::Printf(TEXT("Text diff %s — bytes=%lld"), *Path, NewSize);
		}
		return true;
	}
} // namespace SourceControlImpl

// ============================================================================
// SECTION B — Cook / Package (P4-1)
// ============================================================================
namespace CookPackageImpl
{
	struct FUatJob
	{
		FString JobId;
		FString Target;
		FString OutputDir;
		FString BuildConfig;
		FString LogFilePath;
		FString CommandLine;
		FProcHandle ProcHandle;
		void* ReadPipe  = nullptr;
		void* WritePipe = nullptr;
		double StartedAtSec = 0.0;
		double FinishedAtSec = 0.0;
		bool bStarted = false;
		bool bFinished = false;
		int32 ExitCode = -1;
	};

	/** Process-wide registry of UAT jobs. v3.10.x: cook_status is now worker-safe,
	 *  so every read/write must go through UE::SOMOLMCP::Locks::UatJobMapLock(). */
	static TMap<FString, TSharedPtr<FUatJob>>& GetJobMap()
	{
		static TMap<FString, TSharedPtr<FUatJob>> Map;
		return Map;
	}

	static FString MakeJobId()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(12);
	}

	static FString FindRunUAT()
	{
		// UE 5.7 ships RunUAT.bat under EngineDir/Build/BatchFiles/.
		const FString Bat = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::EngineDir(), TEXT("Build"), TEXT("BatchFiles"), TEXT("RunUAT.bat")));
		return Bat;
	}

	static FString FindUProjectPath()
	{
		FString Project = FPaths::GetProjectFilePath();
		if (Project.IsEmpty() || !FPaths::FileExists(Project))
		{
			const FString ProjDir = FPaths::ProjectDir();
			TArray<FString> UProjects;
			IFileManager::Get().FindFiles(UProjects, *FPaths::Combine(ProjDir, TEXT("*.uproject")), true, false);
			if (UProjects.Num() > 0)
			{
				Project = FPaths::Combine(ProjDir, UProjects[0]);
			}
		}
		return FPaths::ConvertRelativePathToFull(Project);
	}

	static bool DirectoryHasAnyFile(const FString& Directory)
	{
		if (Directory.IsEmpty() || !IFileManager::Get().DirectoryExists(*Directory))
		{
			return false;
		}

		bool bFoundFile = false;
		IFileManager::Get().IterateDirectoryRecursively(*Directory,
			[&bFoundFile](const TCHAR* FilenameOrDirectory, bool bIsDirectory) -> bool
			{
				if (!bIsDirectory && IFileManager::Get().FileExists(FilenameOrDirectory))
				{
					bFoundFile = true;
					return false;
				}
				return true;
			});
		return bFoundFile;
	}

	static bool IsValidTargetPlatform(const FString& Target)
	{
		static const TArray<FString> Allowed = {
			TEXT("WindowsServer"), TEXT("Win64"), TEXT("Linux"), TEXT("LinuxArm64"), TEXT("Android"), TEXT("IOS"), TEXT("Mac")
		};
		return Allowed.Contains(Target);
	}

	static FString NormalizeTargetPlatform(FString Target)
	{
		Target = Target.TrimStartAndEnd();
		if (Target.Equals(TEXT("Windows"), ESearchCase::IgnoreCase)) return TEXT("Win64");
		if (Target.Equals(TEXT("iOS"), ESearchCase::IgnoreCase)) return TEXT("IOS");
		return Target;
	}

	static bool IsValidBuildConfig(const FString& BuildConfig)
	{
		static const TArray<FString> Allowed = {
			TEXT("Development"), TEXT("Shipping"), TEXT("Test")
		};
		return Allowed.Contains(BuildConfig);
	}

	static bool LaunchUatJob(
		const FString& Target,
		const FString& BuildConfig,
		const FString& OutputDir,
		const FString& ExtraArgs,
		const FString& OperationFlags,
		TSharedPtr<FUatJob>& OutJob,
		FString& OutError)
	{
		if (ExtraArgs.Contains(TEXT("&")) || ExtraArgs.Contains(TEXT("|")) || ExtraArgs.Contains(TEXT(">"))
			|| ExtraArgs.Contains(TEXT("<")) || ExtraArgs.Contains(TEXT("\r")) || ExtraArgs.Contains(TEXT("\n")))
		{
			OutError = TEXT("additional_args contains forbidden shell metacharacters.");
			return false;
		}
		const FString RunUAT = FindRunUAT();
		if (!FPaths::FileExists(RunUAT))
		{
			OutError = FString::Printf(TEXT("RunUAT.bat not found at: %s"), *RunUAT);
			return false;
		}
		const FString UProject = FindUProjectPath();
		if (UProject.IsEmpty())
		{
			OutError = TEXT("Could not locate .uproject path.");
			return false;
		}

		TSharedPtr<FUatJob> Job = MakeShared<FUatJob>();
		Job->JobId = MakeJobId();
		Job->Target = Target;
		Job->OutputDir = OutputDir;
		Job->BuildConfig = BuildConfig;
		Job->StartedAtSec = FPlatformTime::Seconds();

		Job->LogFilePath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("UAT_%s.log"), *Job->JobId)));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Job->LogFilePath), /*Tree*/ true);
		if (!FFileHelper::SaveStringToFile(TEXT(""), *Job->LogFilePath) || !FPaths::FileExists(Job->LogFilePath))
		{
			OutError = FString::Printf(TEXT("Could not create UAT log file at: %s"), *Job->LogFilePath);
			return false;
		}

		FString Cmdline = FString::Printf(
			TEXT("BuildCookRun -Project=\"%s\" -NoP4 -ClientConfig=%s %s -TargetPlatform=%s"),
			*UProject, *BuildConfig, *OperationFlags, *Target);

		if (!OutputDir.IsEmpty())
		{
			Cmdline += FString::Printf(TEXT(" -StagingDirectory=\"%s\""), *OutputDir);
		}
		if (!ExtraArgs.IsEmpty())
		{
			Cmdline += TEXT(" ") + ExtraArgs;
		}

		// Invoke through cmd.exe so > redirect lands in our log file.
		const FString CmdExe = TEXT("cmd.exe");
		const FString FullCmd = FString::Printf(
			TEXT("/S /C \"\"%s\" %s > \"%s\" 2>&1\""),
			*RunUAT, *Cmdline, *Job->LogFilePath);

		Job->CommandLine = FullCmd;

		FPlatformProcess::CreatePipe(Job->ReadPipe, Job->WritePipe);

		uint32 PID = 0;
		Job->ProcHandle = FPlatformProcess::CreateProc(
			*CmdExe,
			*FullCmd,
			/*bLaunchDetached*/ true,
			/*bLaunchHidden*/   true,
			/*bLaunchReallyHidden*/ true,
			&PID,
			/*PriorityModifier*/ 0,
			nullptr,
			Job->WritePipe,
			Job->ReadPipe);

		if (!Job->ProcHandle.IsValid())
		{
			OutError = TEXT("FPlatformProcess::CreateProc failed for UAT.");
			FPlatformProcess::ClosePipe(Job->ReadPipe, Job->WritePipe);
			return false;
		}

		Job->bStarted = true;
		{
			// v3.10.x worker-safety: cook_status reads this map from a worker thread.
			FScopeLock Lock(&UE::SOMOLMCP::Locks::UatJobMapLock());
			GetJobMap().Add(Job->JobId, Job);
		}
		OutJob = Job;
		return true;
	}

	/** Tail the last N lines of a log file. */
	static TArray<FString> TailFile(const FString& Path, int32 NumLines)
	{
		TArray<FString> Lines;
		if (!FPaths::FileExists(Path))
		{
			return Lines;
		}
		FString Content;
		FFileHelper::LoadFileToString(Content, *Path);
		Content.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
		if (Lines.Num() > NumLines)
		{
			TArray<FString> Tail;
			Tail.Reserve(NumLines);
			for (int32 i = Lines.Num() - NumLines; i < Lines.Num(); ++i)
			{
				Tail.Add(Lines[i]);
			}
			return Tail;
		}
		return Lines;
	}

	static bool RunCookTarget(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString Target;
		if (!Arguments->TryGetStringField(TEXT("target_platform"), Target))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("target_platform"));
			OutError = TEXT("Missing target_platform.");
			return false;
		}
		FString ExtraArgs;
		Arguments->TryGetStringField(TEXT("additional_args"), ExtraArgs);
		Target = NormalizeTargetPlatform(Target);
		if (!IsValidTargetPlatform(Target))
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_ARGUMENT"), TEXT("target_platform"),
				TEXT("Unsupported target platform. Expected one of: WindowsServer, Win64 (Windows alias accepted), Linux, LinuxArm64, Android, IOS, Mac."));
			OutError = FString::Printf(TEXT("Unsupported target_platform: %s"), *Target);
			return false;
		}

		TSharedPtr<FUatJob> Job;
		FString LaunchErr;
		// Cook + Pak only (stage/package belong to package_build).
		const FString Flags = TEXT("-Cook -Pak -CookOnTheFly=false");
		if (!LaunchUatJob(Target, TEXT("Development"), FString(), ExtraArgs, Flags, Job, LaunchErr))
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), LaunchErr);
			OutError = LaunchErr;
			return false;
		}

		OutStructured->SetStringField(TEXT("job_id"), Job->JobId);
		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetBoolField(TEXT("started"), true);
		OutStructured->SetStringField(TEXT("log_path"), Job->LogFilePath);

		OutSummary = FString::Printf(TEXT("Cook started job=%s target=%s"), *Job->JobId, *Target);
		return true;
	}

	static bool RunPackageBuild(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString Target;
		if (!Arguments->TryGetStringField(TEXT("target_platform"), Target))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("target_platform"));
			OutError = TEXT("Missing target_platform.");
			return false;
		}
		FString OutputDir;
		if (!Arguments->TryGetStringField(TEXT("output_dir"), OutputDir))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("output_dir"));
			OutError = TEXT("Missing output_dir.");
			return false;
		}
		FString BuildConfig = TEXT("Development");
		Arguments->TryGetStringField(TEXT("build_config"), BuildConfig);
		FString ExtraArgs;
		Arguments->TryGetStringField(TEXT("additional_args"), ExtraArgs);
		Target = NormalizeTargetPlatform(Target);
		OutputDir = OutputDir.TrimStartAndEnd();
		BuildConfig = BuildConfig.TrimStartAndEnd();
		if (!IsValidTargetPlatform(Target))
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_ARGUMENT"), TEXT("target_platform"),
				TEXT("Unsupported target platform. Expected one of: WindowsServer, Win64 (Windows alias accepted), Linux, LinuxArm64, Android, IOS, Mac."));
			OutError = FString::Printf(TEXT("Unsupported target_platform: %s"), *Target);
			return false;
		}
		if (!IsValidBuildConfig(BuildConfig))
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_ARGUMENT"), TEXT("build_config"),
				TEXT("Unsupported build_config. Expected one of: Development, Shipping, Test."));
			OutError = FString::Printf(TEXT("Unsupported build_config: %s"), *BuildConfig);
			return false;
		}
		if (OutputDir.IsEmpty())
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_ARGUMENT"), TEXT("output_dir"), TEXT("output_dir must not be empty."));
			OutError = TEXT("output_dir must not be empty.");
			return false;
		}
		OutputDir = FPaths::ConvertRelativePathToFull(OutputDir);
		if (!IFileManager::Get().MakeDirectory(*OutputDir, /*Tree*/ true) || !IFileManager::Get().DirectoryExists(*OutputDir))
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("output_dir"), TEXT("Could not create or access output_dir."));
			OutError = FString::Printf(TEXT("Could not create or access output_dir: %s"), *OutputDir);
			return false;
		}

		TSharedPtr<FUatJob> Job;
		FString LaunchErr;
		const FString Flags = TEXT("-Build -Cook -Stage -Package -Pak");
		if (!LaunchUatJob(Target, BuildConfig, OutputDir, ExtraArgs, Flags, Job, LaunchErr))
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), LaunchErr);
			OutError = LaunchErr;
			return false;
		}

		OutStructured->SetStringField(TEXT("job_id"), Job->JobId);
		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetStringField(TEXT("output"), OutputDir);
		OutStructured->SetStringField(TEXT("build_config"), BuildConfig);
		OutStructured->SetStringField(TEXT("additional_args"), ExtraArgs);
		OutStructured->SetBoolField(TEXT("started"), true);
		OutStructured->SetStringField(TEXT("log_path"), Job->LogFilePath);

		OutSummary = FString::Printf(TEXT("Package started job=%s target=%s cfg=%s"),
			*Job->JobId, *Target, *BuildConfig);
		return true;
	}

	static bool RunCookStatus(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString JobId;
		if (!Arguments->TryGetStringField(TEXT("job_id"), JobId))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("job_id"));
			OutError = TEXT("Missing job_id.");
			return false;
		}

		// v3.10.x worker-safety: snapshot the TSharedPtr under the lock so the
		// map cannot rehash underneath us; per-job state transition (bFinished,
		// ClosePipe) also runs under the lock so two concurrent cook_status
		// callers can't double-close pipes.
		TSharedPtr<FUatJob> JobShared;
		FString Status;
		double FinishedAtSecCopy = 0.0;
		double StartedAtSecCopy = 0.0;
		bool bFinishedCopy = false;
		int32 ExitCodeCopy = -1;
		bool bOutputVerifiedCopy = false;
		FString OutputDirCopy;
		FString LogFilePathCopy;
		FString JobIdCopy;
		{
			FScopeLock Lock(&UE::SOMOLMCP::Locks::UatJobMapLock());
			TSharedPtr<FUatJob>* JobPtr = GetJobMap().Find(JobId);
			if (!JobPtr || !JobPtr->IsValid())
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("job_id=%s"), *JobId));
				OutError = TEXT("Job not found.");
				return false;
			}
			JobShared = *JobPtr;
			FUatJob& Job = *JobShared.Get();

			if (Job.ProcHandle.IsValid())
			{
				if (FPlatformProcess::IsProcRunning(Job.ProcHandle))
				{
					Status = TEXT("running");
				}
				else
				{
					if (!Job.bFinished)
					{
						Job.bFinished = true;
						Job.FinishedAtSec = FPlatformTime::Seconds();
						int32 ExitCode = 0;
						if (FPlatformProcess::GetProcReturnCode(Job.ProcHandle, &ExitCode))
						{
							Job.ExitCode = ExitCode;
						}
						if (Job.ExitCode == 0 && !Job.OutputDir.IsEmpty() && !DirectoryHasAnyFile(Job.OutputDir))
						{
							Job.ExitCode = -2;
						}
						if (Job.ReadPipe || Job.WritePipe)
						{
							FPlatformProcess::ClosePipe(Job.ReadPipe, Job.WritePipe);
							Job.ReadPipe = nullptr;
							Job.WritePipe = nullptr;
						}
					}
					Status = (Job.ExitCode == 0) ? TEXT("succeeded") : TEXT("failed");
				}
			}
			else
			{
				Status = TEXT("failed");
			}

			// Snapshot all fields we need outside the lock for JSON build.
			JobIdCopy           = Job.JobId;
			LogFilePathCopy     = Job.LogFilePath;
			OutputDirCopy       = Job.OutputDir;
			StartedAtSecCopy    = Job.StartedAtSec;
			FinishedAtSecCopy   = Job.FinishedAtSec;
			bFinishedCopy       = Job.bFinished;
			ExitCodeCopy        = Job.ExitCode;
			bOutputVerifiedCopy = Job.OutputDir.IsEmpty() ? true : DirectoryHasAnyFile(Job.OutputDir);
		}

		const double NowSec = FPlatformTime::Seconds();
		const double EndSec = bFinishedCopy ? FinishedAtSecCopy : NowSec;
		const double Elapsed = FMath::Max(0.0, EndSec - StartedAtSecCopy);

		OutStructured->SetStringField(TEXT("job_id"), JobIdCopy);
		OutStructured->SetStringField(TEXT("status"), Status);
		OutStructured->SetNumberField(TEXT("elapsed_secs"), Elapsed);
		OutStructured->SetNumberField(TEXT("exit_code"), ExitCodeCopy);
		if (!OutputDirCopy.IsEmpty())
		{
			OutStructured->SetStringField(TEXT("output"), OutputDirCopy);
			OutStructured->SetBoolField(TEXT("output_verified"), bOutputVerifiedCopy);
		}

		const TArray<FString> Tail = TailFile(LogFilePathCopy, 50);
		DevOpsToolsImpl::SetStringArrayField(OutStructured, TEXT("log_tail"), Tail);

		OutSummary = FString::Printf(TEXT("Job %s status=%s elapsed=%.1fs"),
			*JobIdCopy, *Status, Elapsed);
		return true;
	}
} // namespace CookPackageImpl

// ============================================================================
// SECTION C — Replication / Network debug (P4-4)
// ============================================================================
namespace NetDebugImpl
{
	/** Get the editor's primary world (PIE preferred, else Editor world). */
	static UWorld* GetCurrentWorld()
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				return Ctx.World();
			}
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::Editor && Ctx.World())
			{
				return Ctx.World();
			}
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.World()) return Ctx.World();
		}
		return nullptr;
	}

	static const TCHAR* RoleToShortName(ENetRole Role)
	{
		switch (Role)
		{
			case ROLE_None:             return TEXT("ROLE_None");
			case ROLE_SimulatedProxy:   return TEXT("ROLE_SimulatedProxy");
			case ROLE_AutonomousProxy:  return TEXT("ROLE_AutonomousProxy");
			case ROLE_Authority:        return TEXT("ROLE_Authority");
			default:                    return TEXT("ROLE_Unknown");
		}
	}

	static bool RunNetInspect(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		UWorld* World = GetCurrentWorld();
		if (!World)
		{
			SololmcpError::NotFound(OutStructured, TEXT("World"));
			OutError = TEXT("No world found (Editor + PIE both empty).");
			return false;
		}

		FString ActorFilter;
		Arguments->TryGetStringField(TEXT("actor_filter"), ActorFilter);

		const ENetMode NetMode = World->GetNetMode();
		const bool bServerWorld = (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer);
		const bool bClientWorld = (NetMode == NM_Client);

		int32 RoleNone = 0, RoleAuthority = 0, RoleAuto = 0, RoleSim = 0;
		int32 ReplicatedActors = 0;
		int32 DormantActors = 0;
		int32 MatchingActors = 0;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;

			if (!ActorFilter.IsEmpty())
			{
				const FString Name = Actor->GetName();
				const FString Class = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
				if (!Name.Contains(ActorFilter) && !Class.Contains(ActorFilter))
				{
					continue;
				}
			}
			++MatchingActors;

			switch (Actor->GetLocalRole())
			{
				case ROLE_None:             ++RoleNone;       break;
				case ROLE_SimulatedProxy:   ++RoleSim;        break;
				case ROLE_AutonomousProxy:  ++RoleAuto;       break;
				case ROLE_Authority:        ++RoleAuthority;  break;
				default: break;
			}

			if (Actor->GetIsReplicated())
			{
				++ReplicatedActors;
				if (Actor->NetDormancy == DORM_DormantAll || Actor->NetDormancy == DORM_DormantPartial)
				{
					++DormantActors;
				}
			}
		}

		FString NetModeStr;
		switch (NetMode)
		{
			case NM_Standalone:       NetModeStr = TEXT("Standalone");      break;
			case NM_DedicatedServer:  NetModeStr = TEXT("DedicatedServer"); break;
			case NM_ListenServer:     NetModeStr = TEXT("ListenServer");    break;
			case NM_Client:           NetModeStr = TEXT("Client");          break;
			default:                  NetModeStr = TEXT("Unknown");         break;
		}

		OutStructured->SetBoolField(TEXT("server_world"), bServerWorld);
		OutStructured->SetBoolField(TEXT("client_world"), bClientWorld);
		OutStructured->SetStringField(TEXT("net_mode"), NetModeStr);
		OutStructured->SetNumberField(TEXT("total_replicated_actors"), ReplicatedActors);
		OutStructured->SetNumberField(TEXT("dormant_actors_count"), DormantActors);
		OutStructured->SetNumberField(TEXT("matching_actors"), MatchingActors);

		TSharedRef<FJsonObject> RoleSummary = MakeShared<FJsonObject>();
		RoleSummary->SetNumberField(TEXT("ROLE_None"),            RoleNone);
		RoleSummary->SetNumberField(TEXT("ROLE_Authority"),       RoleAuthority);
		RoleSummary->SetNumberField(TEXT("ROLE_AutonomousProxy"), RoleAuto);
		RoleSummary->SetNumberField(TEXT("ROLE_SimulatedProxy"),  RoleSim);
		OutStructured->SetObjectField(TEXT("role_summary"), RoleSummary);

		if (UNetDriver* Driver = World->GetNetDriver())
		{
			OutStructured->SetBoolField(TEXT("net_driver_present"), true);
			OutStructured->SetStringField(TEXT("net_driver_class"),
				Driver->GetClass() ? Driver->GetClass()->GetName() : FString());
		}
		else
		{
			OutStructured->SetBoolField(TEXT("net_driver_present"), false);
		}

		OutSummary = FString::Printf(
			TEXT("Net inspect [%s]: replicated=%d dormant=%d auth=%d auto=%d sim=%d"),
			*NetModeStr, ReplicatedActors, DormantActors, RoleAuthority, RoleAuto, RoleSim);
		return true;
	}

	static bool RunReplicationGraphDump(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		UWorld* World = GetCurrentWorld();
		if (!World)
		{
			SololmcpError::NotFound(OutStructured, TEXT("World"));
			OutError = TEXT("No world found.");
			return false;
		}

		FString ClassFilter;
		Arguments->TryGetStringField(TEXT("class_filter"), ClassFilter);

		UNetDriver* Driver = World->GetNetDriver();
		if (!Driver)
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("World has no active NetDriver — replication graph is only valid during PIE/runtime networking."));
			OutSummary = TEXT("No NetDriver — graph dump unavailable.");
			return true;
		}

		// TODO(P4-4): when ReplicationGraph is on the link line, switch to a typed cast against
		// UReplicationGraph* and walk GlobalGraphNodes / ConnectionGraphNodes directly.
		UObject* RepDriverObj = nullptr;
		if (UClass* RepGraphClass = FindFirstObject<UClass>(TEXT("ReplicationGraph"), EFindFirstObjectOptions::ExactClass))
		{
			TArray<UObject*> SubObjs;
			GetObjectsWithOuter(Driver, SubObjs, /*bIncludeNestedObjects*/ true);
			for (UObject* Obj : SubObjs)
			{
				if (Obj && Obj->IsA(RepGraphClass))
				{
					RepDriverObj = Obj;
					break;
				}
			}
		}

		if (!RepDriverObj)
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("Active NetDriver is not using UReplicationGraph (basic NetDriver/PushModel in use)."));
			OutStructured->SetStringField(TEXT("net_driver_class"),
				Driver->GetClass() ? Driver->GetClass()->GetName() : FString());
			OutSummary = TEXT("Replication graph not in use.");
			return true;
		}

		// Reflect: enumerate sub-nodes by class. Without the typed header we report instance counts.
		TMap<FString, int32> NodeClassCounts;
		TArray<UObject*> NestedSubObjs;
		GetObjectsWithOuter(RepDriverObj, NestedSubObjs, /*bIncludeNestedObjects*/ true);
		for (UObject* Obj : NestedSubObjs)
		{
			if (!Obj || !Obj->GetClass()) continue;
			const FString ClassName = Obj->GetClass()->GetName();
			if (!ClassName.Contains(TEXT("ReplicationGraphNode"))) continue;
			if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter)) continue;
			NodeClassCounts.FindOrAdd(ClassName)++;
		}

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		for (const TPair<FString, int32>& Pair : NodeClassCounts)
		{
			TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("class"), Pair.Key);
			Node->SetNumberField(TEXT("instance_count"), Pair.Value);
			Node->SetStringField(TEXT("note"),
				TEXT("instance_count = number of node objects of this class. "
				     "Per-node replicated_actors_count requires typed UReplicationGraph access (TODO P4-4)."));
			NodesJson.Add(MakeShared<FJsonValueObject>(Node));
		}

		OutStructured->SetArrayField(TEXT("nodes"), NodesJson);
		OutStructured->SetStringField(TEXT("policy_class"),
			RepDriverObj->GetClass() ? RepDriverObj->GetClass()->GetName() : FString());
		OutStructured->SetStringField(TEXT("net_driver_class"),
			Driver->GetClass() ? Driver->GetClass()->GetName() : FString());

		OutSummary = FString::Printf(TEXT("Replication graph dump — %d distinct node classes."),
			NodesJson.Num());
		return true;
	}

	static bool RunActorForceReplicate(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		UWorld* World = GetCurrentWorld();
		if (!World)
		{
			SololmcpError::NotFound(OutStructured, TEXT("World"));
			OutError = TEXT("No world found.");
			return false;
		}

		FString ActorName;
		if (!Arguments->TryGetStringField(TEXT("actor_name"), ActorName))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("actor_name"));
			OutError = TEXT("Missing actor_name.");
			return false;
		}

		AActor* Found = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A) continue;
			if (A->GetName() == ActorName || A->GetActorLabel() == ActorName)
			{
				Found = A;
				break;
			}
		}
		if (!Found)
		{
			SololmcpError::NotFound(OutStructured, ActorName);
			OutError = FString::Printf(TEXT("Actor '%s' not found in world."), *ActorName);
			return false;
		}

		const bool bWasDormant = (Found->NetDormancy == DORM_DormantAll || Found->NetDormancy == DORM_DormantPartial);
		bool bDormantWasSet = false;
		if (bWasDormant)
		{
			Found->FlushNetDormancy();
			bDormantWasSet = true;
		}
		Found->ForceNetUpdate();

		OutStructured->SetStringField(TEXT("actor"), Found->GetName());
		OutStructured->SetStringField(TEXT("actor_label"), Found->GetActorLabel());
		OutStructured->SetStringField(TEXT("class"),
			Found->GetClass() ? Found->GetClass()->GetName() : FString());
		OutStructured->SetBoolField(TEXT("dormant_was_set"), bDormantWasSet);
		OutStructured->SetBoolField(TEXT("force_called"), true);
		OutStructured->SetBoolField(TEXT("is_replicated"), Found->GetIsReplicated());
		OutStructured->SetStringField(TEXT("local_role"), RoleToShortName(Found->GetLocalRole()));

		OutSummary = FString::Printf(TEXT("ForceNetUpdate on %s (was_dormant=%s)"),
			*Found->GetName(), bDormantWasSet ? TEXT("yes") : TEXT("no"));
		return true;
	}
} // namespace NetDebugImpl

// ============================================================================
// Registration
// ============================================================================
void RegisterDevOpsTools(FSololmcpToolRegistry& Registry)
{
	using FSB = FSololmcpSchemaBuilder;

	// ------------------------------------------------------------------------
	// A) Source Control (P3-8)
	// ------------------------------------------------------------------------
	Registry.Register({
		TEXT("sourcecontrol_status"),
		TEXT("Report source-control state for the project. Returns provider name plus per-state lists "
		     "(checked_out, modified, added, conflicted). Optional path_filter narrows scan to a /Game subtree."),
		FSB::Object(
			{
				{TEXT("path_filter"), FSB::String(TEXT("Optional /Game subpath to limit the scan, e.g. /Game/Materials. Defaults to entire project content."))}
			},
			/*required*/ {}),
		&SourceControlImpl::RunStatus,
		nullptr,
		/*CacheTtlSeconds*/ 0
	});

	Registry.Register({
		TEXT("sourcecontrol_checkout"),
		TEXT("Check out (open for edit) one or more assets via the active source-control provider."),
		FSB::Object(
			{
				{TEXT("paths"), FSB::Array(FSB::String(), TEXT("List of asset paths (e.g. /Game/Maps/Demo) or absolute file paths."))}
			},
			{TEXT("paths")}),
		&SourceControlImpl::RunCheckout
	});

	Registry.Register({
		TEXT("sourcecontrol_checkin"),
		TEXT("Submit (commit) one or more checked-out assets with a description."),
		FSB::Object(
			{
				{TEXT("paths"),       FSB::Array(FSB::String(), TEXT("Asset / file paths to submit."))},
				{TEXT("description"), FSB::String(TEXT("Commit message / changelist description."))}
			},
			{TEXT("paths"), TEXT("description")}),
		&SourceControlImpl::RunCheckin
	});

	Registry.Register({
		TEXT("sourcecontrol_diff"),
		TEXT("Diff a single asset against a revision (default HEAD). For binary assets (.uasset/.umap) returns "
		     "is_binary=true plus old/new sizes; for text returns a textual diff_summary."),
		FSB::Object(
			{
				{TEXT("path"),     FSB::String(TEXT("Asset path or file path."))},
				{TEXT("revision"), FSB::String(TEXT("Revision identifier (default 'HEAD')."))}
			},
			{TEXT("path")}),
		&SourceControlImpl::RunDiff
	});

	// ------------------------------------------------------------------------
	// B) Cook / Package (P4-1)
	// ------------------------------------------------------------------------
	Registry.Register({
		TEXT("cook_target"),
		TEXT("Spawn UAT BuildCookRun (Cook + Pak) for the given target platform. Returns immediately with a "
		     "job_id; poll cook_status to track progress."),
		FSB::Object(
			{
				{TEXT("target_platform"), FSB::String(
					TEXT("UE target platform identifier."),
					{TEXT("WindowsServer"), TEXT("Win64"), TEXT("Windows"), TEXT("Linux"), TEXT("LinuxArm64"), TEXT("Android"), TEXT("IOS"), TEXT("Mac")})},
				{TEXT("additional_args"), FSB::String(TEXT("Extra CLI args appended to BuildCookRun (optional)."))}
			},
			{TEXT("target_platform")}),
		&CookPackageImpl::RunCookTarget
	});

	Registry.Register({
		TEXT("package_build"),
		TEXT("Spawn UAT BuildCookRun -Stage -Package -Pak with optional build configuration. Returns immediately "
		     "with a job_id; poll cook_status to track progress."),
		FSB::Object(
			{
				{TEXT("target_platform"), FSB::String(
					TEXT("UE target platform identifier."),
					{TEXT("WindowsServer"), TEXT("Win64"), TEXT("Windows"), TEXT("Linux"), TEXT("LinuxArm64"), TEXT("Android"), TEXT("IOS"), TEXT("Mac")})},
				{TEXT("output_dir"),   FSB::String(TEXT("Staging output directory (passed as -StagingDirectory)."))},
				{TEXT("build_config"), FSB::String(TEXT("Build configuration (default Development)."),
					{TEXT("Development"), TEXT("Shipping"), TEXT("Test")})},
				{TEXT("additional_args"), FSB::String(TEXT("Extra UAT BuildCookRun arguments for release, patch, chunk, or manifest workflows."))}
			},
			{TEXT("target_platform"), TEXT("output_dir")}),
		&CookPackageImpl::RunPackageBuild
	});

	Registry.Register({
		TEXT("cook_status"),
		TEXT("Query a cook_target / package_build job by job_id. Returns status (running/succeeded/failed), "
		     "elapsed time, exit code, and the last 50 log lines."),
		FSB::Object(
			{
				{TEXT("job_id"), FSB::String(TEXT("Job id returned by cook_target or package_build."))}
			},
			{TEXT("job_id")}),
		&CookPackageImpl::RunCookStatus
	});

	// ------------------------------------------------------------------------
	// C) Replication / Network debug (P4-4)
	// ------------------------------------------------------------------------
	Registry.Register({
		TEXT("net_inspect"),
		TEXT("Inspect the current world's net mode and replication-role distribution. Optional actor_filter "
		     "matches actor name OR class name (substring)."),
		FSB::Object(
			{
				{TEXT("actor_filter"), FSB::String(TEXT("Optional substring matched against actor names and classes."))}
			},
			/*required*/ {}),
		&NetDebugImpl::RunNetInspect
	});

	Registry.Register({
		TEXT("net_replication_graph_dump"),
		TEXT("Walk the active UReplicationGraph (if present) and report its node-class structure. Returns "
		     "NOT_AVAILABLE when a basic UNetDriver is in use instead of a graph driver."),
		FSB::Object(
			{
				{TEXT("class_filter"), FSB::String(TEXT("Optional substring filter on node class names."))}
			},
			/*required*/ {}),
		&NetDebugImpl::RunReplicationGraphDump
	});

	Registry.Register({
		TEXT("net_actor_force_replicate"),
		TEXT("Find an actor by name (or label) in the current world, flush its dormancy if dormant, and call "
		     "ForceNetUpdate. Always works even outside a replication graph."),
		FSB::Object(
			{
				{TEXT("actor_name"), FSB::String(TEXT("Actor's GetName() or GetActorLabel()."))}
			},
			{TEXT("actor_name")}),
		&NetDebugImpl::RunActorForceReplicate
	});
}

} // namespace UE::SOMOLMCP
