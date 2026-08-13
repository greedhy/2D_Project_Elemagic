// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpHlodForceRebuildShim.h"

#include "Protocol/SololmcpJobService.h"
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpSchemaBuilder.h"

#include "CoreMinimal.h"
#include "Engine/World.h"

namespace UE::SOMOLMCP
{
namespace
{
	using SB = FSololmcpSchemaBuilder;

	static UWorld* ResolveWorldForHlodForceRebuild(
		const FSololmcpEditorServices& Services,
		const FString& WorldPath,
		FString& OutError)
	{
		UWorld* EditorWorld = Services.GetEditorWorld(OutError);
		if (WorldPath.IsEmpty())
		{
			return EditorWorld;
		}

		if (EditorWorld)
		{
			const FString EditorPath = EditorWorld->GetPathName();
			if (EditorPath == WorldPath || EditorWorld->GetName() == WorldPath || EditorPath.StartsWith(WorldPath))
			{
				return EditorWorld;
			}
		}

		if (UWorld* LoadedWorld = LoadObject<UWorld>(nullptr, *WorldPath))
		{
			return LoadedWorld;
		}

		OutError = FString::Printf(TEXT("World '%s' not found / not loaded."), *WorldPath);
		return EditorWorld;
	}

	static bool ConsoleTextLooksLikeError(const FString& Text)
	{
		return Text.Contains(TEXT("error"), ESearchCase::IgnoreCase)
			|| Text.Contains(TEXT("failed"), ESearchCase::IgnoreCase)
			|| Text.Contains(TEXT("unknown command"), ESearchCase::IgnoreCase);
	}

	static void SetFailureQualityFields(
		const TSharedRef<FJsonObject>& Out,
		const FString& Dispatch,
		const FString& ConsoleStdout,
		const FString& ConsoleStderr,
		const FString& Error,
		const FString& ErrorHint,
		const FString& NextAction)
	{
		Out->SetStringField(TEXT("dispatch"), Dispatch);
		Out->SetStringField(TEXT("console_stdout"), ConsoleStdout);
		Out->SetStringField(TEXT("console_stderr"), ConsoleStderr);
		Out->SetStringField(TEXT("error_hint"), ErrorHint);
		Out->SetStringField(TEXT("next_action"), NextAction);
		Out->SetStringField(TEXT("Error"), Error);
		Out->SetStringField(TEXT("error"), Error);
	}

	static TArray<TSharedPtr<FJsonValue>> EchoCellFilterBox(const TSharedRef<FJsonObject>& Args, bool& bOutHasFilterBox)
	{
		TArray<TSharedPtr<FJsonValue>> EchoBox;
		const TArray<TSharedPtr<FJsonValue>>* BoxArray = nullptr;
		bOutHasFilterBox = Args->TryGetArrayField(TEXT("cell_filter_box"), BoxArray) && BoxArray != nullptr;
		if (!bOutHasFilterBox)
		{
			return EchoBox;
		}

		for (const TSharedPtr<FJsonValue>& Value : *BoxArray)
		{
			EchoBox.Add(MakeShared<FJsonValueNumber>(Value.IsValid() ? Value->AsNumber() : 0.0));
		}
		return EchoBox;
	}

	static bool SubmitTrackedHlodDispatchJob(
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		TSharedRef<FJsonObject> StepArgs = MakeShared<FJsonObject>();

		FString WorldPath;
		if (Args->TryGetStringField(TEXT("world_path"), WorldPath))
		{
			StepArgs->SetStringField(TEXT("world_path"), WorldPath);
		}

		const TArray<TSharedPtr<FJsonValue>>* BoxArray = nullptr;
		if (Args->TryGetArrayField(TEXT("cell_filter_box"), BoxArray) && BoxArray)
		{
			TArray<TSharedPtr<FJsonValue>> BoxCopy;
			for (const TSharedPtr<FJsonValue>& Value : *BoxArray)
			{
				BoxCopy.Add(MakeShared<FJsonValueNumber>(Value.IsValid() ? Value->AsNumber() : 0.0));
			}
			StepArgs->SetArrayField(TEXT("cell_filter_box"), BoxCopy);
		}

		// Prevent the queued step from recursively submitting another job.
		StepArgs->SetBoolField(TEXT("tracked_job"), false);

		TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("tool"), TEXT("worldpartition_hlod_force_rebuild"));
		Step->SetStringField(TEXT("label"), TEXT("dispatch_hlod_rebuild"));
		Step->SetObjectField(TEXT("arguments"), StepArgs);

		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(Step));

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("steps"), Steps);
		Params->SetStringField(TEXT("plan_label"), TEXT("worldpartition_hlod_force_rebuild_dispatch"));
		Params->SetStringField(TEXT("_priority"), TEXT("low"));

		FString ClientRequestId;
		if (Args->TryGetStringField(TEXT("client_request_id"), ClientRequestId) && !ClientRequestId.IsEmpty())
		{
			Params->SetStringField(TEXT("client_request_id"), ClientRequestId);
		}
		FString TraceId;
		if (Args->TryGetStringField(TEXT("trace_id"), TraceId) && !TraceId.IsEmpty())
		{
			Params->SetStringField(TEXT("trace_id"), TraceId);
		}

		if (!FSololmcpJobService::SubmitJob(Params, Out, Error))
		{
			return false;
		}

		FString JobId;
		Out->TryGetStringField(TEXT("job_id"), JobId);
		Out->SetStringField(TEXT("status"), TEXT("queued"));
		Out->SetBoolField(TEXT("async"), true);
		Out->SetBoolField(TEXT("completed"), false);
		Out->SetStringField(TEXT("dispatch"), TEXT("wp.HLOD.RebuildHLODs"));
		Out->SetStringField(TEXT("job_poll_method"), TEXT("jobs/get"));
		Out->SetStringField(TEXT("job_events_method"), TEXT("jobs/events"));
		Out->SetStringField(TEXT("job_tracks"), TEXT("dispatch_only"));
		Out->SetStringField(TEXT("note"),
			TEXT("tracked_job=true returns an MCP job_id for the HLOD rebuild dispatch step only. "
			     "UE's wp.HLOD.RebuildHLODs command remains internally async and does not expose a completion handle to this shim."));

		Summary = JobId.IsEmpty()
			? TEXT("HLOD rebuild dispatch job submitted.")
			: FString::Printf(TEXT("HLOD rebuild dispatch job submitted: %s"), *JobId);
		return true;
	}
}

void RegisterHlodForceRebuildShim(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("worldpartition_hlod_force_rebuild"),
		TEXT("Force-rebuild HLOD actors for the world (optional cell_filter_box). "
		     "Dispatches wp.HLOD.RebuildHLODs and returns detailed failure diagnostics. "
		     "Optional tracked_job=true returns an MCP job_id for dispatch tracking."),
		SB::Object({
			{TEXT("world_path"), SB::String(TEXT("Optional level path. Empty = current editor world."))},
			{TEXT("cell_filter_box"), SB::Array(SB::Number(),
				TEXT("Optional [x_min, y_min, x_max, y_max] world coords. Omit to rebuild all."))},
			{TEXT("tracked_job"), SB::Boolean(TEXT("Optional. When true, return a pollable MCP job_id for the dispatch step."))},
			{TEXT("client_request_id"), SB::String(TEXT("Optional idempotency key used when tracked_job=true."))},
			{TEXT("trace_id"), SB::String(TEXT("Optional correlation id used when tracked_job=true."))}
		}, {}),
		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& Out,
		   FString& Summary,
		   FString& Error) -> bool
		{
			const FString Command = TEXT("wp.HLOD.RebuildHLODs");

			bool bTrackedJob = false;
			if (Args->TryGetBoolField(TEXT("tracked_job"), bTrackedJob) && bTrackedJob)
			{
				return SubmitTrackedHlodDispatchJob(Args, Out, Summary, Error);
			}

			FString WorldPath;
			Args->TryGetStringField(TEXT("world_path"), WorldPath);

			FString ResolveError;
			UWorld* World = ResolveWorldForHlodForceRebuild(Context.Services, WorldPath, ResolveError);
			const FString WorldName = World ? World->GetPathName() : TEXT("<no world>");

			bool bHasFilterBox = false;
			TArray<TSharedPtr<FJsonValue>> EchoBox = EchoCellFilterBox(Args, bHasFilterBox);

			if (!World)
			{
				Error = ResolveError.IsEmpty()
					? FString::Printf(TEXT("World '%s' was not found."), *WorldPath)
					: ResolveError;
				Out->SetStringField(TEXT("world"), WorldName);
				Out->SetArrayField(TEXT("cell_filter_box"), EchoBox);
				Out->SetStringField(TEXT("status"), TEXT("world_not_found"));
				Out->SetBoolField(TEXT("async"), false);
				SololmcpError::NotFound(Out, WorldPath.IsEmpty() ? TEXT("<current editor world>") : WorldPath);
				SetFailureQualityFields(
					Out,
					Command,
					TEXT(""),
					TEXT(""),
					Error,
					TEXT("No editor world or requested world could be resolved before dispatching the HLOD rebuild command."),
					TEXT("Open or load the target World Partition level, or pass a valid /Game/... world_path, then call worldpartition_hlod_force_rebuild again."));
				return false;
			}

			if (bHasFilterBox)
			{
				Out->SetStringField(TEXT("world"), WorldName);
				Out->SetArrayField(TEXT("cell_filter_box"), EchoBox);
				Out->SetStringField(TEXT("status"), TEXT("unsupported_filter_box"));
				Out->SetBoolField(TEXT("async"), false);
				SololmcpError::Set(
					Out,
					TEXT("UNSUPPORTED"),
					TEXT("cell_filter_box"),
					TEXT("cell_filter_box cannot be honored by the console rebuild command."));
				Error = TEXT("cell_filter_box is not supported for force rebuild.");
				SetFailureQualityFields(
					Out,
					Command,
					TEXT(""),
					TEXT(""),
					Error,
					TEXT("The only safe in-editor dispatch path is wp.HLOD.RebuildHLODs, which rebuilds globally and cannot honor a spatial filter box."),
					TEXT("Omit cell_filter_box for a full async rebuild, or use a filter-aware editor/Python workflow outside this shim."));
				return false;
			}

			TSharedRef<FJsonObject> ConsoleResult = MakeShared<FJsonObject>();
			FString ConsoleSummary;
			FString ConsoleError;
			const bool bExecOk = Context.Services.ExecuteConsole(Command, ConsoleResult, ConsoleSummary, ConsoleError);
			FString ConsoleStdout;
			ConsoleResult->TryGetStringField(TEXT("output"), ConsoleStdout);

			if (!bExecOk || !ConsoleError.IsEmpty() || ConsoleTextLooksLikeError(ConsoleStdout))
			{
				Out->SetStringField(TEXT("world"), WorldName);
				Out->SetArrayField(TEXT("cell_filter_box"), EchoBox);
				Out->SetStringField(TEXT("status"), TEXT("exec_failed"));
				Out->SetBoolField(TEXT("async"), false);
				Out->SetBoolField(TEXT("completed"), false);
				Out->SetBoolField(TEXT("dispatched"), bExecOk);
				SololmcpError::Set(
					Out,
					TEXT("OPERATION_FAILED"),
					TEXT("dispatch"),
					TEXT("HLOD rebuild console dispatch failed."));
				Error = !ConsoleError.IsEmpty()
					? ConsoleError
					: FString::Printf(TEXT("HLOD rebuild console dispatch failed for '%s'. stdout='%s'."), *Command, *ConsoleStdout);
				SetFailureQualityFields(
					Out,
					Command,
					ConsoleStdout,
					ConsoleError,
					Error,
					TEXT("UE rejected the HLOD rebuild dispatch or wrote error-like text while executing the console command."),
					TEXT("Verify World Partition/HLOD editor modules are enabled, check the Output Log for the command above, then retry from the editor console or rerun this tool after fixing the reported issue."));
				return false;
			}

			Out->SetStringField(TEXT("world"), WorldName);
			Out->SetArrayField(TEXT("cell_filter_box"), EchoBox);
			Out->SetNumberField(TEXT("cells_processed"), 0);
			Out->SetNumberField(TEXT("hlod_actors_created"), 0);
			Out->SetStringField(TEXT("status"), TEXT("triggered_async"));
			Out->SetBoolField(TEXT("async"), true);
			Out->SetBoolField(TEXT("completed"), false);
			Out->SetStringField(TEXT("dispatch"), Command);
			Out->SetStringField(TEXT("note"),
				TEXT("Rebuild runs asynchronously; cell_filter_box not respected by console command. "
				     "Monitor Output Log for progress; use python_exec for filter-aware rebuilds."));

			Summary = TEXT("HLOD rebuild dispatched (async; completion not verified).");
			return true;
		},
		nullptr,
		0
	});
}
}
