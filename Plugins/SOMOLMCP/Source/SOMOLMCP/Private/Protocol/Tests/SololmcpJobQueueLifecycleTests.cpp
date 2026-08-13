// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// FIX-1/FIX-2/FIX-3 (job queue conn design 20260804) — NX11~NX14:
//   Lifecycle watchdog automation tests for the single MCP Job Runtime.
//   NX11 runtime-exceeded watchdog, NX12 blocked-TTL watchdog,
//   NX13 external-executor lease + jobs/heartbeat + lock release,
//   NX14 lock-wait timeout with diagnostics.
//   All tests drive the watchdog deterministically through the
//   FSololmcpJobService::Test* hooks (no wall-clock waiting, no TickJobs).

#include "Protocol/SololmcpJobService.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSololmcpNx11JobRuntimeWatchdogTest,
	"SOMOL.MCP.JobQueue.NX11.RuntimeWatchdog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpNx11JobRuntimeWatchdogTest::RunTest(const FString& Parameters)
{
	using namespace UE::SOMOLMCP;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	// One non-mutating step plus an idle explicit resource lock: the lock keeps
	// the job off the worker fast-path so it sits in the GameThread queue.
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Locks;
	TSharedRef<FJsonObject> Lock = MakeShared<FJsonObject>();
	Lock->SetStringField(TEXT("id"), TEXT("nx11-lock-") + Suffix);
	Lock->SetStringField(TEXT("mode"), TEXT("exclusive"));
	Locks.Add(MakeShared<FJsonValueObject>(Lock));
	Params->SetArrayField(TEXT("resource_locks"), Locks);
	TArray<TSharedPtr<FJsonValue>> Steps;
	TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("tool"), TEXT("mcp_status"));
	Step->SetObjectField(TEXT("arguments"), MakeShared<FJsonObject>());
	Steps.Add(MakeShared<FJsonValueObject>(Step));
	Params->SetArrayField(TEXT("steps"), Steps);

	TSharedRef<FJsonObject> SubmitResult = MakeShared<FJsonObject>();
	FString Error;
	TestTrue(TEXT("submit succeeds"),
		FSololmcpJobService::SubmitJob(Params, SubmitResult, Error));
	const FString JobId = SubmitResult->GetStringField(TEXT("job_id"));

	TestTrue(TEXT("test hook marks the job running with an expired runtime anchor"),
		FSololmcpJobService::TestMarkJobRunning(JobId));
	FSololmcpJobService::TestRunLifecycleSweep(FPlatformTime::Seconds());

	TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
	TestTrue(TEXT("job is readable after watchdog failure"),
		FSololmcpJobService::GetJob(JobId, Readback, Error));
	TestEqual(TEXT("watchdog marks the job failed"),
		Readback->GetStringField(TEXT("status")), FString(TEXT("failed")));
	TestEqual(TEXT("error code is E_JOB_RUNTIME_EXCEEDED"),
		Readback->GetStringField(TEXT("error_code")), FString(TEXT("E_JOB_RUNTIME_EXCEEDED")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSololmcpNx12JobBlockedTtlWatchdogTest,
	"SOMOL.MCP.JobQueue.NX12.BlockedTtlWatchdog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpNx12JobBlockedTtlWatchdogTest::RunTest(const FString& Parameters)
{
	using namespace UE::SOMOLMCP;

	// Mutating tool without _project_path/_instance_uuid/target_binding:
	// TargetGuard blocks the job fail-closed at submit time.
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Steps;
	TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("tool"), TEXT("save_all"));
	Step->SetObjectField(TEXT("arguments"), MakeShared<FJsonObject>());
	Steps.Add(MakeShared<FJsonValueObject>(Step));
	Params->SetArrayField(TEXT("steps"), Steps);

	TSharedRef<FJsonObject> SubmitResult = MakeShared<FJsonObject>();
	FString Error;
	TestTrue(TEXT("submit admits the blocked job"),
		FSololmcpJobService::SubmitJob(Params, SubmitResult, Error));
	const FString JobId = SubmitResult->GetStringField(TEXT("job_id"));
	TestEqual(TEXT("job is blocked by the target guard"),
		SubmitResult->GetStringField(TEXT("status")), FString(TEXT("blocked")));
	TestEqual(TEXT("block code is blocked_no_target_guard"),
		SubmitResult->GetStringField(TEXT("error_code")), FString(TEXT("blocked_no_target_guard")));

	// Sweep past the blocked TTL (default 1800s) without any resume.
	FSololmcpJobService::TestRunLifecycleSweep(FPlatformTime::Seconds() + 1900.0);

	TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
	TestTrue(TEXT("job is readable after watchdog failure"),
		FSololmcpJobService::GetJob(JobId, Readback, Error));
	TestEqual(TEXT("watchdog marks the blocked job failed"),
		Readback->GetStringField(TEXT("status")), FString(TEXT("failed")));
	TestEqual(TEXT("error code is E_BLOCKED_TTL_EXPIRED"),
		Readback->GetStringField(TEXT("error_code")), FString(TEXT("E_BLOCKED_TTL_EXPIRED")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSololmcpNx13ExternalJobLeaseTest,
	"SOMOL.MCP.JobQueue.NX13.ExternalJobLeaseHeartbeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpNx13ExternalJobLeaseTest::RunTest(const FString& Parameters)
{
	using namespace UE::SOMOLMCP;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString LockId = TEXT("nx13-lock-") + Suffix;
	const double BaseNow = FPlatformTime::Seconds();
	FString Error;

	// 1) External job with an exclusive lock goes stale when the lease expires.
	FString StaleJobId;
	bool bDeduplicated = false;
	TArray<FString> LockIds = { LockId };
	TestTrue(TEXT("external job is admitted with its exclusive lock"),
		FSololmcpJobService::CreateExternalJob(
			TEXT("nx13-crid-a-") + Suffix, TEXT("nx13.lease"), TEXT("{}"),
			LockIds, StaleJobId, bDeduplicated, Error));
	TestFalse(TEXT("new external job is not deduplicated"), bDeduplicated);

	FSololmcpJobService::TestRunLifecycleSweep(BaseNow + 400.0);
	TSharedRef<FJsonObject> StaleReadback = MakeShared<FJsonObject>();
	TestTrue(TEXT("stale job is readable"),
		FSololmcpJobService::GetJob(StaleJobId, StaleReadback, Error));
	TestEqual(TEXT("watchdog fails the stale external job"),
		StaleReadback->GetStringField(TEXT("status")), FString(TEXT("failed")));
	TestEqual(TEXT("error code is E_EXTERNAL_JOB_STALE"),
		StaleReadback->GetStringField(TEXT("error_code")), FString(TEXT("E_EXTERNAL_JOB_STALE")));

	// 2) The lock held by the stale job must be released for new admissions.
	FString ReclaimJobId;
	TestTrue(TEXT("exclusive lock is released after lease expiry"),
		FSololmcpJobService::CreateExternalJob(
			TEXT("nx13-crid-b-") + Suffix, TEXT("nx13.lease"), TEXT("{}"),
			LockIds, ReclaimJobId, bDeduplicated, Error));
	TestFalse(TEXT("reclaimed admission is a new job"), bDeduplicated);

	// 3) jobs/heartbeat keeps the lease alive inside the silent window.
	TSharedRef<FJsonObject> HeartbeatResult = MakeShared<FJsonObject>();
	TestTrue(TEXT("heartbeat renews the lease"),
		FSololmcpJobService::HeartbeatExternalJob(ReclaimJobId, HeartbeatResult, Error));
	TestTrue(TEXT("heartbeat reports a positive lease"),
		HeartbeatResult->GetNumberField(TEXT("lease_seconds")) > 0.0);
	FSololmcpJobService::TestRunLifecycleSweep(BaseNow + 100.0);
	TSharedRef<FJsonObject> AliveReadback = MakeShared<FJsonObject>();
	TestTrue(TEXT("heartbeated job is readable"),
		FSololmcpJobService::GetJob(ReclaimJobId, AliveReadback, Error));
	TestEqual(TEXT("heartbeated job stays running inside the lease"),
		AliveReadback->GetStringField(TEXT("status")), FString(TEXT("running")));

	// Cleanup: terminal update releases the reclaimed lock.
	TestTrue(TEXT("cleanup cancels the reclaimed external job"),
		FSololmcpJobService::UpdateExternalJob(
			ReclaimJobId, TEXT("cancelled"), 1.0, FString(), 0,
			FString(), FString(), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSololmcpNx14LockWaitTimeoutTest,
	"SOMOL.MCP.JobQueue.NX14.LockWaitTimeoutDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpNx14LockWaitTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace UE::SOMOLMCP;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString LockId = TEXT("nx14-lock-") + Suffix;
	const double BaseNow = FPlatformTime::Seconds();
	FString Error;

	// Holder: external job owns the lock exclusively; extend its lease so the
	// sweep at +950s only times out the waiter, not the holder.
	FString HolderJobId;
	bool bDeduplicated = false;
	TArray<FString> LockIds = { LockId };
	TestTrue(TEXT("holder external job acquires the exclusive lock"),
		FSololmcpJobService::CreateExternalJob(
			TEXT("nx14-crid-h-") + Suffix, TEXT("nx14.lockwait"), TEXT("{}"),
			LockIds, HolderJobId, bDeduplicated, Error));
	TestTrue(TEXT("test hook extends the holder lease"),
		FSololmcpJobService::TestSetExternalLeaseSec(HolderJobId, 5000.0));

	// Waiter: non-mutating single step with an explicit conflicting lock stays
	// queued; submit-time contention detection arms the lock-wait anchor.
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Locks;
	TSharedRef<FJsonObject> Lock = MakeShared<FJsonObject>();
	Lock->SetStringField(TEXT("id"), LockId);
	Lock->SetStringField(TEXT("mode"), TEXT("exclusive"));
	Locks.Add(MakeShared<FJsonValueObject>(Lock));
	Params->SetArrayField(TEXT("resource_locks"), Locks);
	TArray<TSharedPtr<FJsonValue>> Steps;
	TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("tool"), TEXT("mcp_status"));
	Step->SetObjectField(TEXT("arguments"), MakeShared<FJsonObject>());
	Steps.Add(MakeShared<FJsonValueObject>(Step));
	Params->SetArrayField(TEXT("steps"), Steps);

	TSharedRef<FJsonObject> SubmitResult = MakeShared<FJsonObject>();
	TestTrue(TEXT("waiter job is admitted while queued on the lock"),
		FSololmcpJobService::SubmitJob(Params, SubmitResult, Error));
	const FString WaiterJobId = SubmitResult->GetStringField(TEXT("job_id"));

	// Sweep past the lock-wait TTL (default 900s) while the holder is alive.
	FSololmcpJobService::TestRunLifecycleSweep(BaseNow + 1000.0);

	TSharedRef<FJsonObject> WaiterReadback = MakeShared<FJsonObject>();
	TestTrue(TEXT("waiter job is readable after watchdog failure"),
		FSololmcpJobService::GetJob(WaiterJobId, WaiterReadback, Error));
	TestEqual(TEXT("waiter fails with the lock-wait timeout code"),
		WaiterReadback->GetStringField(TEXT("status")), FString(TEXT("failed")));
	TestEqual(TEXT("error code is E_LOCK_WAIT_TIMEOUT"),
		WaiterReadback->GetStringField(TEXT("error_code")), FString(TEXT("E_LOCK_WAIT_TIMEOUT")));

	TSharedRef<FJsonObject> HolderReadback = MakeShared<FJsonObject>();
	TestTrue(TEXT("holder job is readable"),
		FSololmcpJobService::GetJob(HolderJobId, HolderReadback, Error));
	TestEqual(TEXT("holder survives the waiter timeout sweep"),
		HolderReadback->GetStringField(TEXT("status")), FString(TEXT("running")));

	// Cleanup: terminal update releases the holder lock.
	TestTrue(TEXT("cleanup cancels the holder external job"),
		FSololmcpJobService::UpdateExternalJob(
			HolderJobId, TEXT("cancelled"), 1.0, FString(), 0,
			FString(), FString(), Error));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
