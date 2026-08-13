// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UE::SOMOLMCP
{
	class FSololmcpToolRegistry;

	struct FSololmcpJobService
	{
		static void TickJobs(FSololmcpToolRegistry& Registry);

		static bool SubmitJob(const TSharedRef<FJsonObject>& Params, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		// Imports a synchronously materialized composition result into the
		// existing Job Runtime. This is not a second queue/state machine: status,
		// cancel, events, deduplication and receipt reads all remain GJobs-owned.
		static bool PublishCompletedExternalJob(
			const FString& ClientRequestId,
			const FString& PlanLabel,
			bool bSucceeded,
			const FString& PayloadJson,
			int64 OutputRevision,
			const FString& ErrorCode,
			const FString& ErrorMessage,
			FString& OutJobId,
			bool& bOutDeduplicated,
			FString& OutError);
		// Admits a long-running native/external executor into the same GJobs map
		// and resource-lock scheduler used by jobs/submit. No second job store.
		static bool CreateExternalJob(
			const FString& ClientRequestId,
			const FString& PlanLabel,
			const FString& InitialPayloadJson,
			const TArray<FString>& ExclusiveResourceLockIds,
			FString& OutJobId,
			bool& bOutDeduplicated,
			FString& OutError);
		static bool UpdateExternalJob(
			const FString& JobId,
			const FString& Status,
			double Progress,
			const FString& PayloadJson,
			int64 OutputRevision,
			const FString& ErrorCode,
			const FString& ErrorMessage,
			FString& OutError);
		static bool GetExternalJobResult(
			const FString& JobId,
			bool& bOutSucceeded,
			FString& OutPayloadJson,
			int64& OutRevision,
			FString& OutStatus,
			FString& OutErrorCode);
		static bool FindExternalJobByClientRequestId(
			const FString& ClientRequestId,
			FString& OutJobId,
			FString& OutPayloadJson,
			int64& OutRevision);
		// FIX-2 (job queue conn design 20260804): refresh the external-executor
		// lease (jobs/heartbeat). The lease also renews on every UpdateExternalJob.
		static bool HeartbeatExternalJob(const FString& JobId, TSharedRef<FJsonObject>& OutResult, FString& OutError);
#if WITH_DEV_AUTOMATION_TESTS
		// Lifecycle-watchdog automation hooks (NX11-NX14). Deterministic: they
		// bypass TickJobs throttling and wall-clock dependencies.
		static bool TestMarkJobRunning(const FString& JobId);
		static bool TestSetExternalLeaseSec(const FString& JobId, double LeaseSec);
		static void TestRunLifecycleSweep(double NowSec);
#endif
		// Attaches an existing WorldForge scheduler stage to the MCP-owned job.
		// The association is stored on the existing job and scheduler records;
		// it does not create another queue or lifecycle authority.
		static bool BindWorldForgeSchedulerJob(
			const FString& JobId,
			const FString& SchedulerJobId,
			const FString& DomainJobHandle,
			const FString& StageCancellationToken,
			const FString& CheckpointRef,
			FString& OutError);
		static bool CreateBlockedElicitationJob(const FString& RequestId, const FString& Reason, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		static bool BlockJobForElicitation(const FString& JobId, const FString& RequestId, const FString& Reason, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		static bool ResumeJobWithElicitation(const FString& JobId, const FString& RequestId, const TSharedPtr<FJsonObject>& Response, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		static bool GetJob(const FString& JobId, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		static bool AwaitJob(FSololmcpToolRegistry& Registry, const FString& JobId, int32 TimeoutMs, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		static bool CancelJob(const FString& JobId, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		// audit-U4 fix (P2): worker-visible cancellation poll. Tools that
		// implement cooperative cancellation (e.g. long batch loops) can
		// call this from a worker thread between sub-operations and return
		// early when their job has been cancelled. Existing tools that
		// don't call this are unchanged.
		static bool IsJobCancelled(const FString& JobId);
		static bool PollEvents(FSololmcpToolRegistry& Registry, const FString& JobId, int32 SinceSeq, int32 WaitMs, TSharedRef<FJsonObject>& OutResult, FString& OutError);
		static void CollectProgressNotifications(TMap<FString, int32>& InOutLastSeqByJob, TArray<TSharedPtr<FJsonObject>>& OutNotifications);

		static void BuildCapabilitiesJobsObject(const TSharedRef<FJsonObject>& JobsObj);
		static void BuildMetricsObject(const TSharedRef<FJsonObject>& JobsObj, const TArray<FString>& RegisteredToolNames);
		static void BuildJobsSnapshotObject(const TSharedRef<FJsonObject>& JobsObj);
	};
}
