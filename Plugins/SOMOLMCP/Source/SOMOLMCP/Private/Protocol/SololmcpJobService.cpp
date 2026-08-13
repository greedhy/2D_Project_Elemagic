// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Protocol/SololmcpJobService.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SololmcpObjectHandles.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/CriticalSection.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "Tools/SololmcpToolRegistry.h"
#include "Engine/Engine.h"
#if SOMOLMCP_WITH_WORLDFORGE
#include "WorldForgeCouplingRuntimeSubsystem.h"
#endif
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

#if PLATFORM_WINDOWS
// Phase 3F: SEH (__try/__except/GetExceptionCode/EXCEPTION_EXECUTE_HANDLER) needs <excpt.h>
#include "Windows/AllowWindowsPlatformTypes.h"
#include <excpt.h>
#include "HAL/PlatformStackWalk.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Phase 3E (MaxConcurrentJobs throttle):
//   Top-level config — tunable at runtime via console var `ue.somolmcp.MaxConcurrentJobs`.
//   GameThread tools defer in queue when InFlight >= GMaxConcurrentMcpJobs.
//   Worker-safe tools have their own (higher) cap of 16.
static int32 GMaxConcurrentMcpJobs = 4;
static int32 GMaxConcurrentMcpJobsWorker = 16;

// Atomic in-flight counters (manipulated from GameThread + worker threads).
//   GThreadInFlightGameThread: tools currently executing on GameThread (via TickJobs).
//   GThreadInFlightWorker: tools currently executing on TaskGraph background threads.
static FThreadSafeCounter GThreadInFlightGameThread;
static FThreadSafeCounter GThreadInFlightWorker;
static FThreadSafeCounter GSubmittedJobs;
static FThreadSafeCounter GWorkerSafeSingleStepSubmissions;
static FThreadSafeCounter GWorkerFallbackToGameThread;

static FAutoConsoleVariableRef CVarMaxConcurrentMcpJobs(
	TEXT("ue.somolmcp.MaxConcurrentJobs"),
	GMaxConcurrentMcpJobs,
	TEXT("Max concurrent GameThread MCP jobs in-flight at once (default 4)."),
	ECVF_Default);

static FAutoConsoleVariableRef CVarMaxConcurrentMcpJobsWorker(
	TEXT("ue.somolmcp.MaxConcurrentJobsWorker"),
	GMaxConcurrentMcpJobsWorker,
	TEXT("Max concurrent worker-thread MCP jobs in-flight at once (default 16)."),
	ECVF_Default);

// Phase 3F (SEH crash isolation):
//   Thread-local "we are inside a tool body" flag. Lets ensure() / log hooks
//   downstream tell whether a crash/log originated from inside MCP tool execution.
//   Read/write must remain on the same thread as the executing tool.
thread_local bool GMcpInToolExecution = false;

namespace UE::SOMOLMCP
{
// Phase 3A: Module-level Registry pointer set by TickJobs on first call.
// SubmitJob (no Registry param) needs this for worker-thread dispatch.
// Set on every TickJobs invocation (idempotent); guaranteed non-null after
// first frame post-startup. If null (e.g. SubmitJob called before any tick),
// worker fast-path falls back to GameThread queue.
static FSololmcpToolRegistry* GMcpRegistry = nullptr;

// Phase 3F (SEH crash isolation):
//   MSVC error C2712 forbids __try in functions that require C++ object unwinding,
//   so the SEH-only inner shim must take POD-only locals. Outer function does the
//   FString::Printf / UE_LOG in the failure branch where unwinding is allowed.
//   Wrapped in UE::SOMOLMCP namespace so FSololmcpToolRegistry resolves.
#if PLATFORM_WINDOWS && !PLATFORM_SEH_EXCEPTIONS_DISABLED
// Crash-context capture: GetExceptionInformation() is only usable inside the
// __except filter expression, so we memcpy the CONTEXT into a static buffer there
// and let the handler (and the outer guard) dump the crash-point callstack.
static CONTEXT GMcpCrashContext;
static bool CaptureCrashContext(EXCEPTION_POINTERS* InPointers)
{
	if (InPointers && InPointers->ContextRecord)
	{
		GMcpCrashContext = *InPointers->ContextRecord;
	}
	return true;
}

static bool ExecuteToolSehInner(
	FSololmcpToolRegistry& Registry,
	const FString& Tool,
	const TSharedRef<FJsonObject>& ResolvedArgs,
	TSharedRef<FJsonObject>& Structured,
	FString& Summary,
	FString& Error,
	uint32& OutExceptionCode,
	bool& OutHasContext)
{
	OutExceptionCode = 0;
	OutHasContext = false;
	// Foliage serialization (2026-08-05 audit): every foliage_* tool touches the
	// shared AInstancedFoliageActor; concurrent worker dispatch corrupted the
	// instance map and crashed the editor. Lock is taken OUTSIDE __try so the
	// C2712 POD-only rule holds, and released in both the success path and the
	// __except handler, so a crashed tool never leaks the mutex to other threads.
	if (IsFoliageActorTool(Tool))
	{
		LockFoliageActorMutex();
	}
	__try
	{
		const bool bResult = Registry.ExecuteTool(Tool, ResolvedArgs, Structured, Summary, Error);
		if (IsFoliageActorTool(Tool))
		{
			UnlockFoliageActorMutex();
		}
		return bResult;
	}
	__except (OutExceptionCode = GetExceptionCode(), OutHasContext = CaptureCrashContext(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
	{
		// Crash inside a foliage tool: release the mutex so later jobs are not starved.
		if (IsFoliageActorTool(Tool))
		{
			UnlockFoliageActorMutex();
		}
		return false;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
#if SOMOLMCP_WITH_WORLDFORGE
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSololmcpNx01WorldForgeJobPropagationTest,
	"SOMOL.MCP.WorldForge.NX01.JobCancellationPropagation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSololmcpNx01WorldForgeJobPropagationTest::RunTest(const FString& Parameters)
{
	using namespace UE::SOMOLMCP;
	if (!GEngine)
	{
		AddError(TEXT("GEngine is required for the WorldForge job propagation test."));
		return false;
	}
	UWorldForgeCouplingRuntimeSubsystem* Runtime =
		GEngine->GetEngineSubsystem<UWorldForgeCouplingRuntimeSubsystem>();
	if (!Runtime)
	{
		AddError(TEXT("WorldForge coupling runtime subsystem is unavailable."));
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	FString ExternalJobId;
	bool bDeduplicated = false;
	FString Error;
	TestTrue(
		TEXT("MCP authority creates the external job"),
		FSololmcpJobService::CreateExternalJob(
			TEXT("nx01-request-") + Suffix,
			TEXT("worldforge.nx01.propagation"),
			TEXT("{}"),
			{},
			ExternalJobId,
			bDeduplicated,
			Error));
	TestFalse(TEXT("new external job is not deduplicated"), bDeduplicated);

	FWorldForgeScheduledJob Stage;
	Stage.JobId = TEXT("nx01-stage-") + Suffix;
	Stage.GroupId = TEXT("nx01");
	Stage.TimeBasis = TEXT("profile.world_time");
	Stage.CadenceSeconds = 0.0;
	Stage.EstimatedCpuMs = 1.0;
	Stage.ProviderId = TEXT("provider.nx01.test");
	Stage.bRecurring = false;
	TestTrue(
		TEXT("existing scheduler authority creates the internal stage"),
		Runtime->GetLayerScheduler().Submit(Stage, Error));

	const FString DomainHandle =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString CancellationToken =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	TestTrue(
		TEXT("MCP job binds to scheduler stage and stateless domain handle"),
		FSololmcpJobService::BindWorldForgeSchedulerJob(
			ExternalJobId,
			Stage.JobId,
			DomainHandle,
			CancellationToken,
			TEXT("saved://worldforge/checkpoints/") + Suffix,
			Error));

	TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
	TestTrue(
		TEXT("mapping is visible from MCP readback"),
		FSololmcpJobService::GetJob(ExternalJobId, Readback, Error));
	const TSharedPtr<FJsonObject>* Mapping = nullptr;
	TestTrue(
		TEXT("readback includes worldforge mapping"),
		Readback->TryGetObjectField(TEXT("worldforge_job_mapping"), Mapping) &&
		Mapping && Mapping->IsValid());
	if (Mapping && Mapping->IsValid())
	{
		TestEqual(
			TEXT("domain handle mapping is exact"),
			(*Mapping)->GetStringField(TEXT("domain_job_handle")),
			DomainHandle);
	}

	TSharedRef<FJsonObject> CancelReadback = MakeShared<FJsonObject>();
	TestTrue(
		TEXT("MCP cancellation succeeds only after downstream propagation"),
		FSololmcpJobService::CancelJob(ExternalJobId, CancelReadback, Error));
	FWorldForgeScheduledJob CancelledStage;
	TestTrue(
		TEXT("scheduler stage remains discoverable"),
		Runtime->GetLayerScheduler().Find(Stage.JobId, CancelledStage));
	TestEqual(
		TEXT("scheduler stage is cancelled"),
		static_cast<uint8>(CancelledStage.State),
		static_cast<uint8>(EWorldForgeScheduledJobState::Cancelled));
	TestTrue(
		TEXT("domain stage token observes cancellation"),
		Runtime->GetLayerScheduler().IsStageCancellationRequested(CancellationToken));
	TestEqual(
		TEXT("MCP state is cancelled"),
		CancelReadback->GetStringField(TEXT("status")),
		FString(TEXT("cancelled")));
	return true;
}
#endif // SOMOLMCP_WITH_WORLDFORGE
#endif
#endif

// Phase 3F (SEH crash isolation):
//   Wrap Registry.ExecuteTool so access violations / divide-by-zero inside tool
//   bodies are caught WITHOUT terminating the editor.
static bool ExecuteToolWithSehGuard(
	FSololmcpToolRegistry& Registry,
	const FString& Tool,
	const TSharedRef<FJsonObject>& ResolvedArgs,
	TSharedRef<FJsonObject>& Structured,
	FString& Summary,
	FString& Error)
{
	GMcpInToolExecution = true;
	// === Global non-interactive modal guard (treats every MCP tool as headless) ===
	// Root cause this fixes: some editor tools (e.g. water_body_create_v2 inserting
	// a Landscape "Water" edit layer) raise a MODAL Slate dialog
	// (FSlateApplication::AddModalWindow). The agent runs headless, so nobody clicks
	// the button -> the GameThread blocks in the modal loop -> ALL subsequent MCP
	// control-plane calls (tools/list keepalive, jobs/get) time out -> the agent
	// wedges. The in-plugin editor_dialog_watchdog_tick CANNOT rescue this because
	// IT also needs the GameThread the modal is holding (a true deadlock).
	//
	// Setting GIsRunningUnattendedScript=true for the duration of the tool makes
	// FSlateApplication::AddModalWindow early-return WITHOUT entering the blocking
	// modal loop (Slate explicitly checks this global), and routes FMessageDialog to
	// its default answer. So a tool that would have popped a modal proceeds with the
	// default instead of freezing the editor. TGuardValue restores the prior value
	// after every tool, so interactive editor use between jobs is unaffected.
	TGuardValue<bool> UnattendedModalGuard(GIsRunningUnattendedScript, true);
	bool bSuccess = false;
#if PLATFORM_WINDOWS && !PLATFORM_SEH_EXCEPTIONS_DISABLED
	uint32 ExceptionCode = 0;
	bool bHasCrashContext = false;
	bSuccess = ExecuteToolSehInner(Registry, Tool, ResolvedArgs, Structured, Summary, Error, ExceptionCode, bHasCrashContext);
	if (!bSuccess && ExceptionCode != 0)
	{
		Error = FString::Printf(TEXT("FATAL_TOOL_CRASH: tool=%s SEH exception code=0x%X"), *Tool, ExceptionCode);
		UE_LOG(LogTemp, Error, TEXT("[seh] tool '%s' crashed with code 0x%X - caught, MCP continues"), *Tool, ExceptionCode);
		if (bHasCrashContext)
		{
			// Dump the crash-point registers + resolved RIP symbol (captured CONTEXT from the __except filter).
			UE_LOG(LogTemp, Error, TEXT("[seh] crash registers for '%s': RIP=0x%llX RSP=0x%llX RBP=0x%llX"),
				*Tool, (uint64)GMcpCrashContext.Rip, (uint64)GMcpCrashContext.Rsp, (uint64)GMcpCrashContext.Rbp);
			FProgramCounterSymbolInfoEx Symbol;
			FPlatformStackWalk::ProgramCounterToSymbolInfoEx((uint64)GMcpCrashContext.Rip, Symbol);
			UE_LOG(LogTemp, Error, TEXT("[seh] crash RIP symbol for '%s': module=%s function=%s file=%s line=%u offset=0x%llX"),
				*Tool, *Symbol.ModuleName, *Symbol.FunctionName, *Symbol.Filename, Symbol.LineNumber, (uint64)Symbol.OffsetInModule);
		}
	}
#else
	// Non-Windows / SEH disabled — plain call with the foliage serialization guard
	// (no crash-recovery, matching the pre-existing unprotected behavior).
	if (IsFoliageActorTool(Tool))
	{
		LockFoliageActorMutex();
	}
	bSuccess = Registry.ExecuteTool(Tool, ResolvedArgs, Structured, Summary, Error);
	if (IsFoliageActorTool(Tool))
	{
		UnlockFoliageActorMutex();
	}
#endif
	GMcpInToolExecution = false;
	return bSuccess;
}

	namespace
	{
		// Keep terminal receipts long enough to survive HTTP session rotation and
		// realistic unattended runs. Jobs are process-owned, never session-owned.
		constexpr int32 MaxTrackedJobs = 8192;
		constexpr int32 MaxJobTombstones = 8192;
		const FString GJobRuntimeInstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

		struct FSololmcpJobTombstone
		{
			FString Reason;
			double RemovedTimeSec = 0.0;
		};

		struct FSololmcpJobStep
		{
			FString Tool;
			FString Label;
			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
		};

		struct FSololmcpJobEvent
		{
			int32 Seq = 0;
			FString Type;
			FString Message;
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			FString TimestampIso;
		};

		// Audit round 9 (group D - phase 1 priority queue):
		//   优先级层级。低值 = 高优先。Urgent 永远先于其他级别且不参与 WRR 配额。
		//   插入到对应 priority FIFO 队列；TickJobs 用 WeightedRoundRobin (4:2:1) 决定下一步。
		enum class EJobPriority : uint8
		{
			Urgent = 0,
			High   = 1,
			Mid    = 2,
			Low    = 3,
		};

		struct FMcpJobResourceLock
		{
			FString Id;
			FString Mode = TEXT("exclusive");
			FString Reason;
		};

		struct FSololmcpJobState
		{
		FString JobId;
		FString ClientRequestId;
		FString TraceId;
		FString PlanLabel;
		FString Status = TEXT("queued");
		FString ErrorCode;
		FString ErrorMessage;
		FString ExternalResultJson;
		int64 ExternalOutputRevision = 0;
		double ExternalProgress = 0.0;
		int32 CurrentStep = 0;
		TArray<FSololmcpJobStep> Steps;
		TArray<FSololmcpJobEvent> Events;
		TArray<TSharedPtr<FJsonValue>> StepResults;
		int32 NextEventSeq = 1;
		// FIXED #8: 记录提交时间用于最旧优先修剪，而非依赖 GUID 字典序
		double SubmitTimeSec = 0.0;
		// 当前步骤执行中的实时日志（每行推送，jobs/get 时附带）
		TArray<FString> LiveLogLines;
		// jobs/get 客户端上次已读取到的日志行数（用于增量推送）
		int32 LiveLogSeenCount = 0;
		// Audit round 9 (group D - phase 1 priority queue):
		//   每个 Job 的调度优先级。SubmitJob 阶段从 params._priority 或工具名推断。
		EJobPriority Priority = EJobPriority::Mid;
		// WorldForge GeoTerrain v1.1 Job Envelope(contract 03 §14,WF-02-02).
		int64 MaximumRuntimeMs = 3600000;
		FString CheckpointPolicy = TEXT("none");
		FString EventDetail = TEXT("summary");
		int32 ContractPriority = 0;
		TArray<FMcpJobResourceLock> ResourceLocks;
		bool bResourceLocksAcquired = false;
		FString ResourceLockSource = TEXT("auto");
		FString BlockedLockId;
		FString BlockedByJobId;
		FString BlockedReason;
		int32 ResourceLockBlockCount = 0;
		double LastResourceLockEventSec = 0.0;
		double LastHeartbeatEventSec = 0.0;
		TArray<FString> TargetPaths;
		bool bRequiresTargetGuard = false;
		bool bTargetGuardSatisfied = true;
		bool bReplayRetryBindingRequired = false;
		FString TargetGuardStatus = TEXT("not_required");
		FString TargetGuardErrorCode;
		FString TargetGuardMessage;
		FString WaitingRequestId;
		FString WaitingMethod;
		FString WaitingReason;
		bool bWaitingForElicitation = false;
		bool bSyntheticElicitationJob = false;
		// NX-01-04 correlation fields. Lifecycle remains GJobs-owned; these are
		// immutable references to the existing WorldForge scheduler/domain job.
		FString WorldForgeSchedulerJobId;
		FString WorldForgeDomainJobHandle;
		FString WorldForgeStageCancellationToken;
		FString WorldForgeCheckpointRef;
		// FIX-1 (job queue conn design 20260804): lifecycle watchdog anchors.
		//   所有字段都是墙钟秒（FPlatformTime::Seconds()），0 表示未启用。
		double FirstRunningTimeSec = 0.0;  // 首次进入 running 的时刻（runtime 看门狗锚点）
		double BlockedSinceSec = 0.0;      // 进入 blocked 的时刻（blocked TTL 锚点）
		double LockWaitingSinceSec = 0.0;  // 开始等待资源锁的时刻（锁等待 TTL 锚点）
		double LastUpdateTimeSec = 0.0;    // 最近一次状态推进/心跳（外部 Job 租约锚点）
		double ExternalLeaseSec = 300.0;   // 外部执行器租约长度（heartbeat 可续期）
		bool bExternal = false;            // CreateExternalJob 创建的原生外部执行器作业
		};

		TMap<FString, FSololmcpJobState> GJobs;
		TMap<FString, FSololmcpJobTombstone> GJobTombstones;
		TMap<FString, FString> GRequestIdToJobId;
		TMap<FString, TArray<TPair<FString, FString>>> GActiveJobResourceLocks;
		TArray<double> GRecentGameThreadDurationsMs;
		TArray<bool> GRecentTerminalFailures;
		double GLastTickJobsTimeSec = 0.0;
		int64 GTickJobsCount = 0;
		int64 GResourceLockBlockedTicks = 0;
		FCriticalSection GJobTelemetryMutex;
		constexpr int32 MaxRecentTelemetrySamples = 128;

		// FIX-1/FIX-2/FIX-3 (job queue conn design 20260804): 生命周期看门狗 CVar。
		//   全部可用 `somolmcp.jobs.*` 在运行时调节，默认值保守。
		TAutoConsoleVariable<int32> CVarJobsBlockedTtlSec(
			TEXT("somolmcp.jobs.blocked_ttl_sec"), 1800,
			TEXT("Seconds a blocked job may stay blocked before the lifecycle watchdog fails it with E_BLOCKED_TTL_EXPIRED. 0 disables the check."),
			ECVF_Default);
		TAutoConsoleVariable<int32> CVarJobsLockWaitTtlSec(
			TEXT("somolmcp.jobs.lock_wait_ttl_sec"), 900,
			TEXT("Seconds a queued/running job may wait for resource locks before the lifecycle watchdog fails it with E_LOCK_WAIT_TIMEOUT. 0 disables the check."),
			ECVF_Default);
		TAutoConsoleVariable<int32> CVarJobsExternalStaleSec(
			TEXT("somolmcp.jobs.external_stale_sec"), 300,
			TEXT("Default lease seconds for external executor jobs. A lease expires ExternalStaleSec after the last progress update or jobs/heartbeat; the watchdog then fails the job with E_EXTERNAL_JOB_STALE and releases its locks. 0 disables the check."),
			ECVF_Default);
		TAutoConsoleVariable<float> CVarJobsLifecycleSweepIntervalSec(
			TEXT("somolmcp.jobs.lifecycle_sweep_interval_sec"), 1.0f,
			TEXT("Throttle interval for the lifecycle watchdog sweep inside TickJobs."),
			ECVF_Default);

		// Phase 3A (worker thread pool):
		//   GJobs / GRequestIdToJobId / queues are touched from BOTH GameThread (TickJobs,
		//   SubmitJob, GetJob, AwaitJob) AND TaskGraph worker threads (worker-safe tool
		//   completion path). Guard every read/write with this mutex. Lock scope is kept
		//   narrow so worker tools can run without the lock held.
		FCriticalSection GJobsMutex;

		// Phase 3A (worker thread pool):
		//   Worker-safe tool names. Mirrors the static set previously declared inside
		//   TickJobs (line ~570). Promoted to namespace-level so SubmitJob can route
		//   directly to TaskGraph and bypass the GameThread queue entirely.
		bool IsWorkerSafeTool(const FString& Tool)
		{
			static const TSet<FString> WorkerSafeTools = {
				// --- existing (pre-Phase D) ---
				TEXT("asset_query"),
				TEXT("log_get_lines"),
				TEXT("log_search"),
				TEXT("mcp_status"),
				TEXT("tools_list"),
				TEXT("tool_describe"),
				TEXT("tool_list_namespaces"),
				TEXT("plugin_status"),

				// --- v3.10 Phase D (high confidence, +24 tools) — see PHASE_D_WORKER_SAFE_AUDIT.md ---

				// Pure-JSON capability / catalog tools (no UE state access)
				TEXT("mcp_capabilities_get"),
				TEXT("tools_capabilities_describe"),
				TEXT("niagara_pipeline_templates_list"),
				TEXT("bt_template_list_available"),

				// Stub / mock tools whose body never touches UE state
				TEXT("fab_search"),
				TEXT("quixel_search"),
				TEXT("landscape_hole_punch"),
				TEXT("swarm_virtual_detachment_mock"),
				TEXT("editor_get_screenshot"),
				TEXT("pcg_snapshot_hash"),
				TEXT("pcg_snapshot_restore"),
				TEXT("asset_ingest_from_disk"),
				TEXT("asset_import"),
				TEXT("render_queue_list"),

				// Pure file-IO under Saved/Cooked or Config/ — no UObject touched
				TEXT("cook_pipeline_clean"),
				TEXT("cook_pipeline_size_report"),
				TEXT("cook_pipeline_set_chunk_assignment"),

				// FCriticalSection-guarded internal map reads (no UE state)
				TEXT("mcp_list_dynamic_tools"),
				TEXT("mcp_get_python_tool_source"),
				TEXT("mcp_unregister_python_tool"),

				// FSololmcpJobService internal-state reads via GJobsMutex
				TEXT("job_get"),
				TEXT("job_cancel"),

				// --- v3.10.2 Phase E re-audit (high confidence, +2) — PHASE_E_WORKER_SAFE_REAUDIT.md ---
				TEXT("project_settings_read"),       // GConfig + IProjectManager + FApp post-init immutables
				TEXT("transaction_list"),            // FCriticalSection-guarded by FRegistry::Mtx

				// --- v3.10.x Phase P1 (mutex-added + cached-pointer, +6) — PHASE_P1_WORKER_SAFE_COMPLETE.md ---
				TEXT("pcg_generation_budget_get"),       // BudgetStore now FCriticalSection-guarded (Locks::PcgBudgetStoreLock)
				TEXT("cook_status"),                     // GetJobMap now FCriticalSection-guarded (Locks::UatJobMapLock); per-job state transition serialized under same lock
				TEXT("cook_pipeline_validate_assets"),   // IAssetRegistry pre-cached at Server::Start; AR queries are RWLock-guarded
				TEXT("cook_pipeline_dependency_graph"),  // ditto — uses cached IAssetRegistry pointer
				TEXT("camera_anim_list"),                // ditto — uses cached IAssetRegistry pointer
				TEXT("project_maps_list"),               // ditto — uses cached IAssetRegistry pointer

				// --- v3.11.x Phase P3 (config-singleton + plugin-manager reads, +4) — PHASE_P3_WORKER_SAFE_V3.md ---
				TEXT("project_plugins_list"),       // IPluginManager::Get().GetDiscoveredPlugins() — singleton, post-init immutable
				TEXT("collision_channels_list"),    // UCollisionProfile::Get() — singleton, init-time-only mutation
				TEXT("collision_presets_list"),     // ditto — singleton
				TEXT("gameplay_tag_list"),          // UGameplayTagsManager::Get().RequestAllGameplayTags() — singleton, thread-safe

				// --- v3.14.x Worker-Safe Round 4 (plugin discovery reads, +4) ---
				TEXT("plugin_list_all"),            // IPluginManager read-only descriptor snapshot; mirrors project_plugins_list risk profile
				TEXT("plugin_inspect"),             // IPluginManager::FindPlugin + descriptor copy only
				TEXT("plugin_check_compatibility"), // descriptor/dependency read; no enable/disable or module load
				TEXT("plugin_recommend_for_role"),  // static role matrix + IPluginManager::FindPlugin status reads

				// --- v3.14.x Worker-Safe Round 4 follow-up (terrain pure JSON planning, +2) ---
				TEXT("terrain_spec_validate"),      // validates TerrainSpec JSON only; no world/landscape/object access
				TEXT("terrain_tile_plan"),          // deterministic grid math from JSON bounds; no UObject/GameThread state

				// --- v3.14.x Worker-Safe Round 4 follow-up (large-world pure planning, +3) ---
				TEXT("landscape_get_import_info"),          // deterministic landscape import math only; no world/landscape/object access
				TEXT("landscape_calculate_tile_layout"),    // pure tile-grid JSON planning from scalar args
				TEXT("landscape_generate_tile_heightmap"),  // deterministic noise array + base64 encoding; no UObject/GameThread state

				// --- v3.14.x Worker-Safe Round 4 follow-up (config read + debug alias guards, +4) ---
				TEXT("project_config_read_ini"),    // GConfig read-only section/key export; no UObject/world/editor mutation or file write
				TEXT("debug_get_callstack"),        // pure alias guard; returns guidance to blueprint_debug_get_call_stack only
				TEXT("debug_get_watched_values"),   // pure alias guard; returns guidance to blueprint_debug_get_watches only
				TEXT("debug_list_breakpoints"),     // pure alias guard; returns guidance to blueprint_debug_list_breakpoints only

				// --- v3.14.x Worker-Safe Round 4 follow-up (alias guards + JSON preview, +4) ---
				TEXT("audio_create_cue"),           // pure alias guard; returns guidance to audio_create_sound_cue only
				TEXT("umg_create_widget"),          // pure alias guard; returns guidance to umg_widget_blueprint_create only
				TEXT("debug_set_breakpoint"),       // pure alias guard; returns guidance to blueprint_debug_set_breakpoint only
				TEXT("niagara_batch_preview_diff"), // pure JSON operation preview; no UObject/world/editor mutation

				// --- v3.14.x Worker-Safe Round 4 task C (pure alias guards, +2) ---
				TEXT("landscape_sculpt"),           // pure alias guard; returns guidance to landscape_sculpt_brush only
				TEXT("abp_add_state"),              // pure alias guard; returns guidance to anim_blueprint_add_state only

				// --- v3.14.x Worker-Safe Round 4 task C follow-up (static JSON catalog, +1) ---
				TEXT("material_list_supported_properties"), // static property-name JSON catalog; no Context/Arguments/UE state

				// --- v3.15.x MCP execution planning (pure JSON classification, +3) ---
				TEXT("mcp_tool_execution_profile"), // tool-name/argument classifier only; no UObject/world/editor access
				TEXT("mcp_resource_lock_plan"),     // batch lock/conflict planner over JSON calls only
				TEXT("mcp_parallel_authoring_plan"), // greedy non-conflicting wave scheduler over inferred locks only
			};
			return WorkerSafeTools.Contains(Tool);
		}

		void AppendRecentDuration(const double DurationMs)
		{
			FScopeLock Lock(&GJobTelemetryMutex);
			GRecentGameThreadDurationsMs.Add(DurationMs);
			if (GRecentGameThreadDurationsMs.Num() > MaxRecentTelemetrySamples)
			{
				GRecentGameThreadDurationsMs.RemoveAt(0, GRecentGameThreadDurationsMs.Num() - MaxRecentTelemetrySamples);
			}
		}

		void AppendTerminalOutcome(const bool bFailed)
		{
			FScopeLock Lock(&GJobTelemetryMutex);
			GRecentTerminalFailures.Add(bFailed);
			if (GRecentTerminalFailures.Num() > MaxRecentTelemetrySamples)
			{
				GRecentTerminalFailures.RemoveAt(0, GRecentTerminalFailures.Num() - MaxRecentTelemetrySamples);
			}
		}

		void RecordTickJobsHeartbeat()
		{
			FScopeLock Lock(&GJobTelemetryMutex);
			GLastTickJobsTimeSec = FPlatformTime::Seconds();
			++GTickJobsCount;
		}

		double LastTickJobsAgeMs()
		{
			FScopeLock Lock(&GJobTelemetryMutex);
			if (GLastTickJobsTimeSec <= 0.0)
			{
				return -1.0;
			}
			return FMath::Max(0.0, (FPlatformTime::Seconds() - GLastTickJobsTimeSec) * 1000.0);
		}

		int64 TickJobsCount()
		{
			FScopeLock Lock(&GJobTelemetryMutex);
			return GTickJobsCount;
		}

		double RecentGameThreadP95Ms()
		{
			FScopeLock Lock(&GJobTelemetryMutex);
			if (GRecentGameThreadDurationsMs.Num() == 0)
			{
				return 0.0;
			}
			TArray<double> Sorted = GRecentGameThreadDurationsMs;
			Sorted.Sort();
			const int32 Index = FMath::Clamp(
				FMath::CeilToInt(static_cast<double>(Sorted.Num()) * 0.95) - 1,
				0,
				Sorted.Num() - 1);
			return Sorted[Index];
		}

		double RecentFailureRatio()
		{
			FScopeLock Lock(&GJobTelemetryMutex);
			if (GRecentTerminalFailures.Num() == 0)
			{
				return 0.0;
			}
			int32 Failed = 0;
			for (const bool bFailed : GRecentTerminalFailures)
			{
				if (bFailed)
				{
					++Failed;
				}
			}
			return static_cast<double>(Failed) / static_cast<double>(GRecentTerminalFailures.Num());
		}

		int32 CountWorkerSafeRegisteredTools(const TArray<FString>& RegisteredToolNames)
		{
			int32 Count = 0;
			for (const FString& ToolName : RegisteredToolNames)
			{
				if (IsWorkerSafeTool(ToolName))
				{
					++Count;
				}
			}
			return Count;
		}

		// Audit round 9 (group D - phase 1 priority queue):
		//   4 条 priority FIFO 队列，只存 JobId 顺序；Job 状态仍在 GJobs（TMap）。
		//   TickJobs 每帧按 WRR 弹出一个 JobId 推进一步，未完成则按原优先级 push 回队尾，
		//   确保短查询不被长任务饿死且单帧时间预算可控（12ms）。
		TArray<FString> GQueueUrgent;
		TArray<FString> GQueueHigh;
		TArray<FString> GQueueMid;
		TArray<FString> GQueueLow;

		// Audit round 9 (group D - phase 1 priority queue):
		//   字符串到 EJobPriority 的解析（params._priority 显式覆盖）。
		//   返回 false 表示客户端没有提供 _priority 字段（调用方应回退到工具名推断）。
		bool ParseExplicitPriority(const FString& Raw, EJobPriority& OutPriority)
		{
			const FString Lower = Raw.ToLower().TrimStartAndEnd();
			if (Lower.IsEmpty())                          { return false; }
			if (Lower == TEXT("urgent"))                  { OutPriority = EJobPriority::Urgent; return true; }
			if (Lower == TEXT("high") || Lower == TEXT("hi"))    { OutPriority = EJobPriority::High;   return true; }
			if (Lower == TEXT("mid")  || Lower == TEXT("normal")){ OutPriority = EJobPriority::Mid;    return true; }
			if (Lower == TEXT("low"))                     { OutPriority = EJobPriority::Low;    return true; }
			return false;
		}

		const TCHAR* PriorityToString(EJobPriority Priority)
		{
			switch (Priority)
			{
			case EJobPriority::Urgent: return TEXT("urgent");
			case EJobPriority::High:   return TEXT("high");
			case EJobPriority::Mid:    return TEXT("mid");
			case EJobPriority::Low:    return TEXT("low");
			default:                   return TEXT("mid");
			}
		}

		// Audit round 9 (group D - phase 1 priority queue):
		//   按工具名推断默认优先级。规则见 phase 1 audit 矩阵。
		//   传入空字符串时返回 Mid（保守缺省）。
		EJobPriority InferPriorityFromToolName(const FString& ToolNameIn)
		{
			const FString Name = ToolNameIn.ToLower();
			if (Name.IsEmpty()) { return EJobPriority::Mid; }

			// --- URGENT: 系统/取消/中止 ---
			if (Name == TEXT("server_stats") || Name == TEXT("jobs_cancel") || Name == TEXT("server/stats") || Name == TEXT("jobs/cancel"))
			{
				return EJobPriority::Urgent;
			}
			if (Name.EndsWith(TEXT("_kill")) || Name.EndsWith(TEXT("_abort")) || Name.EndsWith(TEXT("_cancel")))
			{
				return EJobPriority::Urgent;
			}

			// --- LOW: 长耗时重生成/编译/烘焙 ---
			if (Name.EndsWith(TEXT("_compile_all")) || Name.EndsWith(TEXT("_bake")) ||
				Name.EndsWith(TEXT("_generate"))     || Name.EndsWith(TEXT("_fill")) ||
				Name.EndsWith(TEXT("_refresh"))      || Name.EndsWith(TEXT("_async")) ||
				Name.StartsWith(TEXT("world_partition_")))
			{
				return EJobPriority::Low;
			}

			// --- HIGH: 读/查询 + 元数据 + 截图 ---
			if (Name.EndsWith(TEXT("_get"))      || Name.EndsWith(TEXT("_list"))      ||
				Name.EndsWith(TEXT("_query"))    || Name.EndsWith(TEXT("_inspect"))   ||
				Name.EndsWith(TEXT("_describe")) || Name.EndsWith(TEXT("_catalog"))   ||
				Name.EndsWith(TEXT("_explain"))  || Name.EndsWith(TEXT("_hash"))      ||
				Name.EndsWith(TEXT("_diff"))     || Name.EndsWith(TEXT("_preview")))
			{
				return EJobPriority::High;
			}
			if (Name == TEXT("tools_list") || Name == TEXT("tools/list") ||
				Name.StartsWith(TEXT("resources_")) || Name.StartsWith(TEXT("resources/")) ||
				Name.StartsWith(TEXT("completions_")) || Name.StartsWith(TEXT("completions/")))
			{
				return EJobPriority::High;
			}
			if (Name.StartsWith(TEXT("screenshot_")) || Name == TEXT("viewport_capture") ||
				Name.StartsWith(TEXT("editor_get_screenshot")))
			{
				return EJobPriority::High;
			}

			// --- MID: 默认（写/小操作/资产创建/单 BP 编译） ---
			return EJobPriority::Mid;
		}

		// Audit round 9 (group D - phase 1 priority queue):
		//   将 JobId 推入对应优先级队列尾部。
		void EnqueueJobByPriority(const FString& JobId, EJobPriority Priority)
		{
			switch (Priority)
			{
			case EJobPriority::Urgent: GQueueUrgent.Add(JobId); break;
			case EJobPriority::High:   GQueueHigh.Add(JobId);   break;
			case EJobPriority::Mid:    GQueueMid.Add(JobId);    break;
			case EJobPriority::Low:    GQueueLow.Add(JobId);    break;
			}
		}

		// Audit round 9 (group D - phase 1 priority queue):
		//   PruneOldJobsIfNeeded 移除终态 Job 时同步从所有队列剔除。终态 Job 本来就不会再被
		//   TickJobs 推进，留在队列里只是 GJobs.Find 会返回 nullptr 然后被 continue 掉的
		//   惰性垃圾。Prune 阶段顺手清掉以保持队列干净。
		void RemoveJobIdFromAllQueues(const FString& JobId)
		{
			GQueueUrgent.Remove(JobId);
			GQueueHigh.Remove(JobId);
			GQueueMid.Remove(JobId);
			GQueueLow.Remove(JobId);
		}





		FString NowIsoString()
		{
			return FDateTime::UtcNow().ToIso8601();
		}

		FString ResolveCorrelationId(const FSololmcpJobState& Job)
		{
			return Job.TraceId.IsEmpty() ? Job.JobId : Job.TraceId;
		}

		void EnrichCorrelationPayload(const FSololmcpJobState& Job, const TSharedRef<FJsonObject>& Payload)
		{
			const FString CorrelationId = ResolveCorrelationId(Job);
			Payload->SetStringField(TEXT("correlation_id"), CorrelationId);
			if (!Job.ClientRequestId.IsEmpty())
			{
				Payload->SetStringField(TEXT("client_request_id"), Job.ClientRequestId);
			}

			TSharedRef<FJsonObject> Correlation = MakeShared<FJsonObject>();
			Correlation->SetStringField(TEXT("job_id"), Job.JobId);
			Correlation->SetStringField(TEXT("correlation_id"), CorrelationId);
			if (!Job.ClientRequestId.IsEmpty())
			{
				Correlation->SetStringField(TEXT("client_request_id"), Job.ClientRequestId);
			}
			if (!Job.TraceId.IsEmpty())
			{
				Correlation->SetStringField(TEXT("trace_id"), Job.TraceId);
			}
			if (!Job.PlanLabel.IsEmpty())
			{
				Correlation->SetStringField(TEXT("plan_label"), Job.PlanLabel);
			}
			if (!Job.WorldForgeSchedulerJobId.IsEmpty())
			{
				TSharedRef<FJsonObject> WorldForge = MakeShared<FJsonObject>();
				WorldForge->SetStringField(TEXT("external_job_id"), Job.JobId);
				WorldForge->SetStringField(TEXT("scheduler_job_id"), Job.WorldForgeSchedulerJobId);
				WorldForge->SetStringField(TEXT("domain_job_handle"), Job.WorldForgeDomainJobHandle);
				WorldForge->SetStringField(TEXT("stage_cancellation_token"), Job.WorldForgeStageCancellationToken);
				if (!Job.WorldForgeCheckpointRef.IsEmpty())
				{
					WorldForge->SetStringField(TEXT("checkpoint_ref"), Job.WorldForgeCheckpointRef);
				}
				Correlation->SetObjectField(TEXT("worldforge_job_mapping"), WorldForge);
				Payload->SetObjectField(TEXT("worldforge_job_mapping"), WorldForge);
			}
			Payload->SetObjectField(TEXT("correlation"), Correlation);
		}

		void EnrichBasePayload(const FSololmcpJobState& Job, const TSharedRef<FJsonObject>& Payload)
		{
			Payload->SetStringField(TEXT("job_id"), Job.JobId);
			EnrichCorrelationPayload(Job, Payload);
			if (!Job.TraceId.IsEmpty())
			{
				Payload->SetStringField(TEXT("trace_id"), Job.TraceId);
			}
			if (!Job.PlanLabel.IsEmpty())
			{
				Payload->SetStringField(TEXT("plan_label"), Job.PlanLabel);
			}
		}

		bool IsTerminalJobStatus(const FString& Status)
		{
			return Status == TEXT("succeeded")
				|| Status == TEXT("failed")
				|| Status == TEXT("cancelled");
		}

		double ComputeJobProgress(const FSololmcpJobState& Job)
		{
			if (Job.Steps.IsEmpty() && !Job.ExternalResultJson.IsEmpty())
			{
				return Job.Status == TEXT("succeeded") ? 1.0 : FMath::Clamp(Job.ExternalProgress, 0.0, 1.0);
			}
			const int32 SafeTotalSteps = FMath::Max(1, Job.Steps.Num());
			if (Job.Status == TEXT("succeeded"))
			{
				return 1.0;
			}
			if (Job.Status == TEXT("blocked") || Job.Status == TEXT("failed") || Job.Status == TEXT("cancelled"))
			{
				return static_cast<double>(Job.CurrentStep) / static_cast<double>(SafeTotalSteps);
			}
			return static_cast<double>(Job.CurrentStep) / static_cast<double>(SafeTotalSteps);
		}

		FString JobClientStatus(const FSololmcpJobState& Job)
		{
			if (Job.Status == TEXT("succeeded"))
			{
				return TEXT("completed");
			}
			if (Job.Status == TEXT("failed"))
			{
				return TEXT("failed");
			}
			// FIX-04 (2026-07-22)：cancelled 独立成词，不再坍塌为 failed——
			// 用户主动取消与执行失败是两种语义，队列词汇统一后下游可区分。
			if (Job.Status == TEXT("cancelled"))
			{
				return TEXT("cancelled");
			}
			if (Job.Status == TEXT("blocked") || !Job.BlockedLockId.IsEmpty())
			{
				return TEXT("blocked");
			}
			if (Job.Status == TEXT("running"))
			{
				return TEXT("progress");
			}
			return TEXT("running");
		}

		TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		void AddProgressFields(const FSololmcpJobState& Job, const TSharedRef<FJsonObject>& Payload)
		{
			const double Progress = FMath::Clamp(ComputeJobProgress(Job), 0.0, 1.0);
			Payload->SetStringField(TEXT("status"), Job.Status);
			Payload->SetStringField(TEXT("client_status"), JobClientStatus(Job));
			Payload->SetStringField(TEXT("status_alias"), JobClientStatus(Job));
			Payload->SetNumberField(TEXT("current_step"), Job.CurrentStep);
			Payload->SetNumberField(TEXT("total_steps"), Job.Steps.Num());
			Payload->SetNumberField(TEXT("progress"), Progress);
			Payload->SetNumberField(TEXT("progress_percent"), FMath::RoundToInt(Progress * 100.0));
			Payload->SetBoolField(TEXT("terminal"), IsTerminalJobStatus(Job.Status));
		}

		void AddJobEvent(FSololmcpJobState& Job, const FString& Type, const FString& Message, const TSharedPtr<FJsonObject>& ExtraPayload = nullptr)
		{
			FSololmcpJobEvent Event;
			Event.Seq = Job.NextEventSeq++;
			Event.Type = Type;
			Event.Message = Message;
			Event.TimestampIso = NowIsoString();
			Event.Payload = MakeShared<FJsonObject>();
			EnrichBasePayload(Job, Event.Payload);
			AddProgressFields(Job, Event.Payload);
			if (ExtraPayload.IsValid())
			{
				for (const auto& Pair : ExtraPayload->Values)
				{
					Event.Payload->SetField(FString(*Pair.Key), Pair.Value);
				}
			}
			Job.Events.Add(Event);
		}

		void MaybeAddHeartbeatEvent(FSololmcpJobState& Job, const TCHAR* Reason = TEXT("poll"))
		{
			if (IsTerminalJobStatus(Job.Status))
			{
				return;
			}
			const double Now = FPlatformTime::Seconds();
			if (Job.LastHeartbeatEventSec > 0.0 && (Now - Job.LastHeartbeatEventSec) < 1.0)
			{
				return;
			}

			TSharedRef<FJsonObject> Heartbeat = MakeShared<FJsonObject>();
			Heartbeat->SetStringField(TEXT("reason"), Reason);
			Heartbeat->SetStringField(TEXT("heartbeat_utc"), NowIsoString());
			Heartbeat->SetNumberField(TEXT("heartbeat_interval_ms"), 1000);
			if (!Job.BlockedLockId.IsEmpty())
			{
				Heartbeat->SetStringField(TEXT("blocked_lock_id"), Job.BlockedLockId);
				Heartbeat->SetStringField(TEXT("blocked_by_job_id"), Job.BlockedByJobId);
				Heartbeat->SetStringField(TEXT("blocked_reason"), Job.BlockedReason);
			}
			AddJobEvent(Job, TEXT("heartbeat"), TEXT("Job heartbeat; client should keep polling."), Heartbeat);
			Job.LastHeartbeatEventSec = Now;
		}

		FString NormalizeResourceLockMode(const FString& Mode)
		{
			return Mode.Equals(TEXT("shared"), ESearchCase::IgnoreCase) ? TEXT("shared") : TEXT("exclusive");
		}

		bool IsExclusiveResourceLock(const FString& Mode)
		{
			return !Mode.Equals(TEXT("shared"), ESearchCase::IgnoreCase);
		}

		FString NormalizeResourceLockId(FString Id)
		{
			Id = Id.TrimStartAndEnd();
			Id.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			while (Id.Contains(TEXT("//")))
			{
				Id.ReplaceInline(TEXT("//"), TEXT("/"), ESearchCase::CaseSensitive);
			}
			if (Id.StartsWith(TEXT("package:/Game/")) || Id.StartsWith(TEXT("package:/Engine/")))
			{
				int32 DotIndex = INDEX_NONE;
				if (Id.FindChar(TEXT('.'), DotIndex))
				{
					Id = Id.Left(DotIndex);
				}
			}
			return Id.ToLower();
		}

		void AddJobResourceLock(TArray<FMcpJobResourceLock>& Locks, FString Id, const FString& Mode, const FString& Reason)
		{
			Id = NormalizeResourceLockId(Id);
			if (Id.IsEmpty())
			{
				return;
			}
			const FString NormalizedMode = NormalizeResourceLockMode(Mode);
			for (FMcpJobResourceLock& Existing : Locks)
			{
				if (Existing.Id == Id)
				{
					if (!IsExclusiveResourceLock(Existing.Mode) && IsExclusiveResourceLock(NormalizedMode))
					{
						Existing.Mode = TEXT("exclusive");
					}
					if (!Reason.IsEmpty() && !Existing.Reason.Contains(Reason))
					{
						Existing.Reason += Existing.Reason.IsEmpty() ? Reason : (TEXT("; ") + Reason);
					}
					return;
				}
			}
			FMcpJobResourceLock Lock;
			Lock.Id = Id;
			Lock.Mode = NormalizedMode;
			Lock.Reason = Reason;
			Locks.Add(Lock);
		}

		TArray<TSharedPtr<FJsonValue>> ResourceLocksToJson(const TArray<FMcpJobResourceLock>& Locks)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FMcpJobResourceLock& Lock : Locks)
			{
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("id"), Lock.Id);
				Obj->SetStringField(TEXT("mode"), Lock.Mode);
				Obj->SetStringField(TEXT("reason"), Lock.Reason);
				Values.Add(MakeShared<FJsonValueObject>(Obj));
			}
			return Values;
		}

		void ReleaseJobResourceLocks(FSololmcpJobState& Job)
		{
			if (!Job.bResourceLocksAcquired)
			{
				return;
			}
			for (const FMcpJobResourceLock& Lock : Job.ResourceLocks)
			{
				if (TArray<TPair<FString, FString>>* Holders = GActiveJobResourceLocks.Find(Lock.Id))
				{
					for (int32 Index = Holders->Num() - 1; Index >= 0; --Index)
					{
						if ((*Holders)[Index].Key == Job.JobId)
						{
							Holders->RemoveAt(Index);
						}
					}
					if (Holders->Num() == 0)
					{
						GActiveJobResourceLocks.Remove(Lock.Id);
					}
				}
			}
			Job.bResourceLocksAcquired = false;
		}

		bool TryAcquireJobResourceLocks(FSololmcpJobState& Job, FString& OutLockId, FString& OutHolderJobId, FString& OutReason)
		{
			if (Job.bResourceLocksAcquired || Job.ResourceLocks.Num() == 0)
			{
				return true;
			}

			for (const FMcpJobResourceLock& Lock : Job.ResourceLocks)
			{
				const TArray<TPair<FString, FString>>* Holders = GActiveJobResourceLocks.Find(Lock.Id);
				if (!Holders)
				{
					continue;
				}
				for (const TPair<FString, FString>& Holder : *Holders)
				{
					if (Holder.Key == Job.JobId)
					{
						continue;
					}
					if (IsExclusiveResourceLock(Lock.Mode) || IsExclusiveResourceLock(Holder.Value))
					{
						OutLockId = Lock.Id;
						OutHolderJobId = Holder.Key;
						OutReason = FString::Printf(TEXT("resource lock '%s' is held by job '%s'"), *Lock.Id, *Holder.Key);
						return false;
					}
				}
			}

			for (const FMcpJobResourceLock& Lock : Job.ResourceLocks)
			{
				TArray<TPair<FString, FString>>& Holders = GActiveJobResourceLocks.FindOrAdd(Lock.Id);
				bool bAlreadyHeld = false;
				for (const TPair<FString, FString>& Holder : Holders)
				{
					if (Holder.Key == Job.JobId)
					{
						bAlreadyHeld = true;
						break;
					}
				}
				if (!bAlreadyHeld)
				{
					Holders.Add(TPair<FString, FString>(Job.JobId, Lock.Mode));
				}
			}
			Job.bResourceLocksAcquired = true;
			Job.BlockedLockId.Empty();
			Job.BlockedByJobId.Empty();
			Job.BlockedReason.Empty();
			return true;
		}

		bool ToolNameLooksMutating(const FString& Lower)
		{
			static const TArray<FString> MutationTokens = {
				TEXT("add"), TEXT("apply"), TEXT("assign"), TEXT("attach"), TEXT("bind"),
				TEXT("build"), TEXT("compile"), TEXT("connect"), TEXT("create"), TEXT("delete"),
				TEXT("destroy"), TEXT("disable"), TEXT("disconnect"), TEXT("duplicate"), TEXT("enable"),
				TEXT("fill"), TEXT("generate"), TEXT("import"), TEXT("move"), TEXT("paint"),
				TEXT("place"), TEXT("remove"), TEXT("rename"), TEXT("repair"), TEXT("reset"),
				TEXT("resize"), TEXT("restore"), TEXT("save"), TEXT("set"), TEXT("spawn"),
				TEXT("start"), TEXT("stop"), TEXT("sync"), TEXT("update"), TEXT("write"),
				TEXT("mutate"), TEXT("rebuild"), TEXT("execute"), TEXT("commit"), TEXT("rollback"),
				TEXT("invalidate"), TEXT("cleanup"), TEXT("cancel"), TEXT("expand"), TEXT("split"),
				TEXT("stitch"), TEXT("merge"), TEXT("resection")
			};
			for (const FString& Token : MutationTokens)
			{
				if (Lower.StartsWith(Token + TEXT("_"))
					|| Lower.Contains(TEXT("_") + Token + TEXT("_"))
					|| Lower.EndsWith(TEXT("_") + Token))
				{
					return true;
				}
			}
			return false;
		}

		void AddPotentialJobTargetPath(const FString& RawValue, TArray<FString>& OutPaths)
		{
			FString Value = RawValue.TrimStartAndEnd();
			if (Value.IsEmpty())
			{
				return;
			}

			// Session handles name a live object rather than a path, so the guard would
			// otherwise see nothing to scope and block fail-closed -- which it did, for
			// every handle-addressed write tool. The registry records the asset each
			// handle was derived from, so resolve it here and let the guard scope the
			// job to that asset. This keeps the guard's purpose intact: an unresolvable
			// handle still contributes no target path and is still blocked.
			if (Value.StartsWith(TEXT("h_"), ESearchCase::CaseSensitive))
			{
				if (const FSololmcpObjectHandles::FEntry* Entry = FSololmcpObjectHandles::Get().Find(Value))
				{
					FString Origin = Entry->Origin;
					// Origins may carry a qualifier, e.g. "/Game/M.M#source_model LOD 0".
					int32 HashIndex = INDEX_NONE;
					if (Origin.FindChar(TEXT('#'), HashIndex))
					{
						Origin.LeftInline(HashIndex);
					}
					Origin.TrimStartAndEndInline();
					if (!Origin.IsEmpty() && Origin != Value)
					{
						AddPotentialJobTargetPath(Origin, OutPaths);
					}
				}
				return;
			}
			Value.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			if (Value.StartsWith(TEXT("Game/")) || Value.StartsWith(TEXT("Engine/")))
			{
				Value = TEXT("/") + Value;
			}
			const bool bUePath = Value.StartsWith(TEXT("/Game/")) || Value.StartsWith(TEXT("/Engine/"));
			const bool bFilePath = Value.Contains(TEXT(":/"))
				|| Value.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".gltf"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".jpg"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".jpeg"), ESearchCase::IgnoreCase)
				|| Value.EndsWith(TEXT(".wav"), ESearchCase::IgnoreCase);
			if (!bUePath && !bFilePath)
			{
				return;
			}
			int32 DotIndex = INDEX_NONE;
			if (bUePath && Value.FindChar(TEXT('.'), DotIndex))
			{
				Value = Value.Left(DotIndex);
			}
			if (!OutPaths.Contains(Value))
			{
				OutPaths.Add(Value);
			}
		}

		void ExtractJobTargetPaths(const TSharedRef<FJsonObject>& Args, TArray<FString>& OutPaths)
		{
			for (const auto& Pair : Args->Values)
			{
				const FString KeyLower = FString(*Pair.Key).ToLower();
				if (!KeyLower.Contains(TEXT("path"))
					&& !KeyLower.Contains(TEXT("asset"))
					&& !KeyLower.Contains(TEXT("package"))
					&& !KeyLower.Contains(TEXT("file"))
					&& !KeyLower.Contains(TEXT("source"))
					&& !KeyLower.Contains(TEXT("mesh"))
					&& !KeyLower.Contains(TEXT("landscape")))
				{
					continue;
				}
				if (!Pair.Value.IsValid())
				{
					continue;
				}
				if (Pair.Value->Type == EJson::String)
				{
					AddPotentialJobTargetPath(Pair.Value->AsString(), OutPaths);
				}
				else if (Pair.Value->Type == EJson::Array)
				{
					for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
					{
						if (Item.IsValid() && Item->Type == EJson::String)
						{
							AddPotentialJobTargetPath(Item->AsString(), OutPaths);
						}
					}
				}
			}
		}

		bool ObjectHasBindingString(const TSharedRef<FJsonObject>& Obj, const TCHAR* FieldName)
		{
			FString Value;
			return Obj->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty();
		}

		bool ObjectHasTargetBindingObject(const TSharedRef<FJsonObject>& Obj)
		{
			const TSharedPtr<FJsonObject>* BindingObj = nullptr;
			if ((Obj->TryGetObjectField(TEXT("target_binding"), BindingObj)
				|| Obj->TryGetObjectField(TEXT("_target_binding"), BindingObj)
				|| Obj->TryGetObjectField(TEXT("replay_target_binding"), BindingObj)
				|| Obj->TryGetObjectField(TEXT("retry_target_binding"), BindingObj))
				&& BindingObj && BindingObj->IsValid())
			{
				return ObjectHasBindingString((*BindingObj).ToSharedRef(), TEXT("_project_path"))
					|| ObjectHasBindingString((*BindingObj).ToSharedRef(), TEXT("project_path"))
					|| ObjectHasBindingString((*BindingObj).ToSharedRef(), TEXT("_instance_uuid"))
					|| ObjectHasBindingString((*BindingObj).ToSharedRef(), TEXT("instance_uuid"));
			}
			return false;
		}

		bool ObjectHasProjectOrInstanceBinding(const TSharedRef<FJsonObject>& Obj)
		{
			return ObjectHasBindingString(Obj, TEXT("_project_path"))
				|| ObjectHasBindingString(Obj, TEXT("project_path"))
				|| ObjectHasBindingString(Obj, TEXT("_instance_uuid"))
				|| ObjectHasBindingString(Obj, TEXT("instance_uuid"))
				|| ObjectHasTargetBindingObject(Obj);
		}

		bool ObjectHasReplayOrRetryMarker(const TSharedRef<FJsonObject>& Obj)
		{
			bool bMarker = false;
			if (Obj->TryGetBoolField(TEXT("is_replay"), bMarker) && bMarker)
			{
				return true;
			}
			if (Obj->TryGetBoolField(TEXT("is_retry"), bMarker) && bMarker)
			{
				return true;
			}
			FString Value;
			return Obj->TryGetStringField(TEXT("replay_of"), Value)
				|| Obj->TryGetStringField(TEXT("retry_of"), Value)
				|| Obj->HasField(TEXT("replay_target_binding"))
				|| Obj->HasField(TEXT("retry_target_binding"));
		}

		bool StepRequiresTargetGuard(const FSololmcpJobStep& Step)
		{
			const FString Lower = Step.Tool.ToLower();
			return ToolNameLooksMutating(Lower)
				|| Lower.Contains(TEXT("editor_dialog_respond"))
				|| Lower.Contains(TEXT("editor_dialog_safe_respond"))
				|| Lower.Contains(TEXT("editor_dialog_watchdog_tick"));
		}

		bool ValidateTargetGuardForJob(
			FSololmcpJobState& Job,
			const TSharedRef<FJsonObject>& Params,
			FString& OutCode,
			FString& OutMessage)
		{
			bool bRequiresGuard = false;
			bool bHasBinding = ObjectHasProjectOrInstanceBinding(Params);
			bool bReplayRetryBindingRequired = ObjectHasReplayOrRetryMarker(Params);
			bool bHasReplayRetryBindingObject = ObjectHasTargetBindingObject(Params);
			TArray<FString> AllTargetPaths;

			for (const FSololmcpJobStep& Step : Job.Steps)
			{
				if (!StepRequiresTargetGuard(Step))
				{
					continue;
				}
				bRequiresGuard = true;
				bHasBinding = bHasBinding || ObjectHasProjectOrInstanceBinding(Step.Arguments);
				bReplayRetryBindingRequired = bReplayRetryBindingRequired || ObjectHasReplayOrRetryMarker(Step.Arguments);
				bHasReplayRetryBindingObject = bHasReplayRetryBindingObject || ObjectHasTargetBindingObject(Step.Arguments);
				ExtractJobTargetPaths(Step.Arguments, AllTargetPaths);
			}

			Job.bRequiresTargetGuard = bRequiresGuard;
			Job.bReplayRetryBindingRequired = bReplayRetryBindingRequired;
			Job.TargetPaths = AllTargetPaths;
			if (!bRequiresGuard)
			{
				Job.bTargetGuardSatisfied = true;
				Job.TargetGuardStatus = TEXT("not_required");
				return true;
			}

			if (!bHasBinding)
			{
				OutCode = TEXT("blocked_no_target_guard");
				OutMessage = TEXT("UE write/editor task is missing _project_path/_instance_uuid or target_binding; blocked fail-closed before execution.");
				Job.bTargetGuardSatisfied = false;
				Job.TargetGuardStatus = OutCode;
				Job.TargetGuardErrorCode = OutCode;
				Job.TargetGuardMessage = OutMessage;
				return false;
			}

			bool bHasConcreteTargetOrScopedLock = AllTargetPaths.Num() > 0;
			for (const FMcpJobResourceLock& Lock : Job.ResourceLocks)
			{
				if (Lock.Id == TEXT("world:current") || Lock.Id == TEXT("editor_ui:modal") || Lock.Id.StartsWith(TEXT("package:")) || Lock.Id.StartsWith(TEXT("file:")))
				{
					bHasConcreteTargetOrScopedLock = true;
					break;
				}
			}

			if (!bHasConcreteTargetOrScopedLock)
			{
				OutCode = TEXT("blocked_no_target_guard");
				OutMessage = TEXT("UE write/editor task has no concrete target path or scoped resource lock; blocked fail-closed before execution.");
				Job.bTargetGuardSatisfied = false;
				Job.TargetGuardStatus = OutCode;
				Job.TargetGuardErrorCode = OutCode;
				Job.TargetGuardMessage = OutMessage;
				return false;
			}

			if (bReplayRetryBindingRequired && !bHasReplayRetryBindingObject)
			{
				OutCode = TEXT("blocked_replay_retry_target_binding_missing");
				OutMessage = TEXT("Replay/retry write task must include replay_target_binding or retry_target_binding; blocked fail-closed before execution.");
				Job.bTargetGuardSatisfied = false;
				Job.TargetGuardStatus = OutCode;
				Job.TargetGuardErrorCode = OutCode;
				Job.TargetGuardMessage = OutMessage;
				return false;
			}

			Job.bTargetGuardSatisfied = true;
			Job.TargetGuardStatus = TEXT("satisfied");
			return true;
		}

		void ParseExplicitResourceLocks(const TSharedRef<FJsonObject>& Obj, TArray<FMcpJobResourceLock>& OutLocks)
		{
			const TArray<TSharedPtr<FJsonValue>>* Locks = nullptr;
			if (!Obj->TryGetArrayField(TEXT("resource_locks"), Locks) || !Locks)
			{
				Obj->TryGetArrayField(TEXT("_resource_locks"), Locks);
			}
			if (!Locks)
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Locks)
			{
				const TSharedPtr<FJsonObject> LockObj = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!LockObj.IsValid())
				{
					continue;
				}
				FString Id;
				if (!LockObj->TryGetStringField(TEXT("id"), Id) || Id.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}
				FString Mode;
				LockObj->TryGetStringField(TEXT("mode"), Mode);
				FString Reason;
				LockObj->TryGetStringField(TEXT("reason"), Reason);
				AddJobResourceLock(OutLocks, Id, Mode, Reason.IsEmpty() ? TEXT("explicit jobs/submit resource lock") : Reason);
			}
		}

		TArray<FMcpJobResourceLock> InferJobResourceLocksFromSteps(const TArray<FSololmcpJobStep>& Steps)
		{
			TArray<FMcpJobResourceLock> Locks;
			for (const FSololmcpJobStep& Step : Steps)
			{
				const FString Lower = Step.Tool.ToLower();
				const bool bMutating = ToolNameLooksMutating(Lower);
				if (Lower.StartsWith(TEXT("worldforge_")))
				{
					static const TSet<FString> WfMutatingTools = {
						TEXT("worldforge_planet_frame_create"), TEXT("worldforge_georeference_create"),
						TEXT("worldforge_spherical_height_dataset_bind"), TEXT("worldforge_runtime_pages_materialize_region"),
						TEXT("worldforge_runtime_pages_release_region"), TEXT("worldforge_heightfield_blend_into_planet"),
						TEXT("worldforge_hydrology_carve_elevation"), TEXT("worldforge_lake_basin_conform"),
						TEXT("worldforge_water_surface_generate"), TEXT("worldforge_shoreline_transition_build"),
						TEXT("worldforge_region_terrain_import"), TEXT("worldforge_region_terrain_acceptance_report"),
						TEXT("worldforge_terrain_replacement_transaction")
					};
					const bool bWfMutating = WfMutatingTools.Contains(Lower);
					TSet<FString> FileLocks, PackageLocks, WorldLocks, ActorLocks, TxnLocks;
					auto ClassifyWfPath = [&](const FString& Key, const FString& Path)
					{
						if (Path.IsEmpty()) { return; }
						if (Key == TEXT("world_package")) { WorldLocks.Add(TEXT("world:") + Path); }
						else if (Key == TEXT("driver_actor_path") || Key.EndsWith(TEXT("_actor_path")) || Key.EndsWith(TEXT("_actor_paths"))) { ActorLocks.Add(TEXT("actor:") + Path); }
						else if (Key.StartsWith(TEXT("output")) || Key == TEXT("transaction_asset_path")) { PackageLocks.Add(TEXT("package:") + Path); }
						else { FileLocks.Add(TEXT("file:") + Path); }
					};
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Step.Arguments->Values)
					{
						if (!Pair.Value.IsValid()) { continue; }
						if (Pair.Key == TEXT("transaction_id"))
						{
							const FString TxnId = Pair.Value->AsString();
							if (!TxnId.IsEmpty()) { TxnLocks.Add(TEXT("terrain_replacement:") + TxnId); }
							continue;
						}
						if (Pair.Value->Type == EJson::String && (Pair.Key.EndsWith(TEXT("_path")) || Pair.Key == TEXT("world_package") || Pair.Key == TEXT("output_path")))
						{
							ClassifyWfPath(Pair.Key, Pair.Value->AsString());
						}
						else if (Pair.Value->Type == EJson::Array && Pair.Key.EndsWith(TEXT("_paths")))
						{
							for (const TSharedPtr<FJsonValue>& Entry : Pair.Value->AsArray())
							{
								if (Entry.IsValid() && Entry->Type == EJson::String) { ClassifyWfPath(Pair.Key, Entry->AsString()); }
							}
						}
					}
					auto AppendWfLocks = [&](const TSet<FString>& Set, const TCHAR* Mode, const TCHAR* Reason)
					{
						TArray<FString> Items = Set.Array();
						Items.Sort();
						for (const FString& Id : Items) { AddJobResourceLock(Locks, Id, Mode, Reason); }
					};
					AppendWfLocks(FileLocks, TEXT("shared"), TEXT("worldforge contract: dataset/definition file (shared)"));
					AppendWfLocks(PackageLocks, bWfMutating ? TEXT("exclusive") : TEXT("shared"), TEXT("worldforge contract: asset package"));
					AppendWfLocks(WorldLocks, bWfMutating ? TEXT("exclusive") : TEXT("shared"), TEXT("worldforge contract: world map"));
					AppendWfLocks(ActorLocks, bWfMutating ? TEXT("exclusive") : TEXT("shared"), TEXT("worldforge contract: driver actor"));
					AppendWfLocks(TxnLocks, TEXT("exclusive"), TEXT("worldforge contract: replacement transaction"));
					if (bWfMutating)
					{
						AddJobResourceLock(Locks, TEXT("save:global"), TEXT("exclusive"), TEXT("worldforge contract: save lane"));
					}
					continue;
				}
				TArray<FString> TargetPaths;
				ExtractJobTargetPaths(Step.Arguments, TargetPaths);
				for (const FString& Path : TargetPaths)
				{
					const FString LockId = (Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Engine/")))
						? TEXT("package:") + Path
						: TEXT("file:") + Path;
					AddJobResourceLock(Locks, LockId, bMutating ? TEXT("exclusive") : TEXT("shared"), TEXT("auto-inferred from step arguments"));
				}
				if (bMutating && (Lower.Contains(TEXT("actor")) || Lower.Contains(TEXT("level")) || Lower.Contains(TEXT("world")) || Lower.Contains(TEXT("landscape")) || Lower.Contains(TEXT("terrain")) || Lower.Contains(TEXT("foliage")) || Lower.Contains(TEXT("pcg_generate")) || Lower.Contains(TEXT("destruction_field"))))
				{
					AddJobResourceLock(Locks, TEXT("world:current"), TEXT("exclusive"), TEXT("auto-inferred world/level mutation"));
				}
				if (bMutating && (Lower.Contains(TEXT("landscape")) || Lower.Contains(TEXT("terrain"))))
				{
					AddJobResourceLock(Locks, TEXT("landscape:current"), TEXT("exclusive"), TEXT("auto-inferred landscape mutation"));
				}
				if (Lower.Contains(TEXT("pcg_generate")))
				{
					AddJobResourceLock(Locks, TEXT("pcg:generation"), TEXT("exclusive"), TEXT("auto-inferred PCG generation lane"));
				}
				if (Lower.Contains(TEXT("compile")) || Lower.Contains(TEXT("validate")))
				{
					FString Domain = TEXT("asset");
					if (Lower.Contains(TEXT("blueprint")) || Lower.StartsWith(TEXT("bp_"))) { Domain = TEXT("blueprint"); }
					else if (Lower.Contains(TEXT("umg")) || Lower.Contains(TEXT("widget"))) { Domain = TEXT("umg"); }
					else if (Lower.Contains(TEXT("material")) || Lower.Contains(TEXT("shader"))) { Domain = TEXT("shader"); }
					else if (Lower.Contains(TEXT("niagara"))) { Domain = TEXT("niagara"); }
					AddJobResourceLock(Locks, TEXT("compile:") + Domain, TEXT("exclusive"), TEXT("auto-inferred compile/validate lane"));
				}
				if (Lower.Contains(TEXT("save")) || Lower.Contains(TEXT("resave")))
				{
					AddJobResourceLock(Locks, TEXT("save:global"), TEXT("exclusive"), TEXT("auto-inferred save lane"));
				}
				if (Lower.Contains(TEXT("cook")) || Lower.Contains(TEXT("package")) || Lower.Contains(TEXT("editor_build")) || Lower.Contains(TEXT("build_lighting")) || Lower.Contains(TEXT("build_navigation")))
				{
					AddJobResourceLock(Locks, TEXT("build:global"), TEXT("exclusive"), TEXT("auto-inferred build/cook/package lane"));
				}
				if (Lower.Contains(TEXT("editor_dialog")) || Lower.Contains(TEXT("editor_mode")) || Lower.Contains(TEXT("viewport")) || Lower.Contains(TEXT("pie_")) || Lower.Contains(TEXT("input_")) || Lower.Contains(TEXT("embody")))
				{
					AddJobResourceLock(Locks, TEXT("editor_ui:modal"), TEXT("exclusive"), TEXT("auto-inferred editor UI lane"));
				}
			}
			return Locks;
		}

		TSharedRef<FJsonObject> TargetGuardToJson(const FSololmcpJobState& Job)
		{
			TSharedRef<FJsonObject> Guard = MakeShared<FJsonObject>();
			Guard->SetBoolField(TEXT("required"), Job.bRequiresTargetGuard);
			Guard->SetBoolField(TEXT("satisfied"), Job.bTargetGuardSatisfied);
			Guard->SetStringField(TEXT("status"), Job.TargetGuardStatus);
			Guard->SetBoolField(TEXT("replay_retry_binding_required"), Job.bReplayRetryBindingRequired);
			Guard->SetArrayField(TEXT("target_paths"), StringArrayToJson(Job.TargetPaths));
			if (!Job.TargetGuardErrorCode.IsEmpty())
			{
				Guard->SetStringField(TEXT("error_code"), Job.TargetGuardErrorCode);
			}
			if (!Job.TargetGuardMessage.IsEmpty())
			{
				Guard->SetStringField(TEXT("message"), Job.TargetGuardMessage);
			}
			return Guard;
		}

		TSharedRef<FJsonObject> BuildReceiptEnvelope(const FSololmcpJobState& Job)
		{
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("schema"), TEXT("somol.mcp_job_receipt_envelope:v2"));
			Receipt->SetStringField(TEXT("job_id"), Job.JobId);
			Receipt->SetStringField(TEXT("runtime_instance_id"), GJobRuntimeInstanceId);
			Receipt->SetStringField(TEXT("ownership"), TEXT("process_global_session_independent"));
			Receipt->SetStringField(TEXT("status"), Job.Status);
			Receipt->SetStringField(TEXT("client_status"), JobClientStatus(Job));
			Receipt->SetBoolField(TEXT("terminal"), IsTerminalJobStatus(Job.Status));
			Receipt->SetNumberField(TEXT("current_step"), Job.CurrentStep);
			Receipt->SetNumberField(TEXT("total_steps"), Job.Steps.Num());
			Receipt->SetNumberField(TEXT("progress"), FMath::Clamp(ComputeJobProgress(Job), 0.0, 1.0));
			Receipt->SetNumberField(TEXT("last_event_seq"), Job.NextEventSeq - 1);
			Receipt->SetStringField(TEXT("last_heartbeat_utc"), Job.Events.Num() > 0 ? Job.Events.Last().TimestampIso : NowIsoString());
			Receipt->SetObjectField(TEXT("target_guard"), TargetGuardToJson(Job));
			Receipt->SetArrayField(TEXT("resource_locks"), ResourceLocksToJson(Job.ResourceLocks));
			Receipt->SetBoolField(TEXT("resource_locks_acquired"), Job.bResourceLocksAcquired);
			Receipt->SetStringField(TEXT("resource_lock_source"), Job.ResourceLockSource);
			Receipt->SetBoolField(TEXT("blocked_modal_or_mcp_no_response"), Job.ErrorCode == TEXT("blocked_modal_or_mcp_no_response"));
			Receipt->SetBoolField(TEXT("waiting_for_elicitation"), Job.bWaitingForElicitation);
			if (Job.bWaitingForElicitation)
			{
				Receipt->SetStringField(TEXT("waiting_request_id"), Job.WaitingRequestId);
				Receipt->SetStringField(TEXT("waiting_method"), Job.WaitingMethod);
				Receipt->SetStringField(TEXT("waiting_reason"), Job.WaitingReason);
				Receipt->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
			}
			Receipt->SetNumberField(TEXT("maximum_runtime_ms"), static_cast<double>(Job.MaximumRuntimeMs));
			Receipt->SetStringField(TEXT("checkpoint_policy"), Job.CheckpointPolicy);
			Receipt->SetStringField(TEXT("event_detail"), Job.EventDetail);
			Receipt->SetNumberField(TEXT("priority"), Job.ContractPriority);
			Receipt->SetBoolField(TEXT("requires_post_edit_readback"), Job.bRequiresTargetGuard);
			Receipt->SetBoolField(TEXT("requires_validation_or_compile_evidence"), Job.bRequiresTargetGuard);
			Receipt->SetBoolField(TEXT("requires_preview_or_runtime_proof"), Job.bRequiresTargetGuard);
			Receipt->SetStringField(
				TEXT("failure_route"),
				(Job.Status == TEXT("blocked") || Job.Status == TEXT("failed")) ? TEXT("qa_inspector_and_hermes") : TEXT("none"));
			if (!Job.ErrorCode.IsEmpty())
			{
				Receipt->SetStringField(TEXT("error_code"), Job.ErrorCode);
				Receipt->SetStringField(TEXT("error_message"), Job.ErrorMessage);
			}
			return Receipt;
		}

		TSharedRef<FJsonObject> JobToJson(FSololmcpJobState& Job)
		{
			MaybeAddHeartbeatEvent(Job, TEXT("jobs_get"));
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("job_id"), Job.JobId);
			Result->SetStringField(TEXT("runtime_instance_id"), GJobRuntimeInstanceId);
			Result->SetStringField(TEXT("ownership"), TEXT("process_global_session_independent"));
			EnrichCorrelationPayload(Job, Result);
			Result->SetStringField(TEXT("status"), Job.Status);
			Result->SetStringField(TEXT("client_status"), JobClientStatus(Job));
			Result->SetStringField(TEXT("status_alias"), JobClientStatus(Job));
			Result->SetStringField(
				TEXT("effective_status"),
				(Job.Status == TEXT("queued") || Job.Status == TEXT("running")) ? TEXT("in_progress") : JobClientStatus(Job));
			Result->SetBoolField(TEXT("terminal"), IsTerminalJobStatus(Job.Status));
			Result->SetNumberField(TEXT("current_step"), Job.CurrentStep);
			Result->SetNumberField(TEXT("total_steps"), Job.Steps.Num());
			const double Progress = FMath::Clamp(ComputeJobProgress(Job), 0.0, 1.0);
			Result->SetNumberField(TEXT("progress"), Progress);
			Result->SetNumberField(TEXT("progress_percent"), FMath::RoundToInt(Progress * 100.0));
			Result->SetObjectField(TEXT("target_guard"), TargetGuardToJson(Job));
			Result->SetArrayField(TEXT("target_paths"), StringArrayToJson(Job.TargetPaths));
			Result->SetArrayField(TEXT("resource_locks"), ResourceLocksToJson(Job.ResourceLocks));
			Result->SetBoolField(TEXT("resource_locks_acquired"), Job.bResourceLocksAcquired);
			Result->SetStringField(TEXT("resource_lock_source"), Job.ResourceLockSource);
			Result->SetStringField(
				TEXT("scheduling_state"),
				Job.BlockedLockId.IsEmpty() ? (Job.bResourceLocksAcquired ? TEXT("locks_acquired") : TEXT("ready")) : TEXT("waiting_for_resource_lock"));
			Result->SetNumberField(TEXT("resource_lock_block_count"), Job.ResourceLockBlockCount);
			if (!Job.BlockedLockId.IsEmpty())
			{
				Result->SetStringField(TEXT("blocked_lock_id"), Job.BlockedLockId);
				Result->SetStringField(TEXT("blocked_by_job_id"), Job.BlockedByJobId);
				Result->SetStringField(TEXT("blocked_reason"), Job.BlockedReason);
			}
			if (Job.bWaitingForElicitation)
			{
				Result->SetBoolField(TEXT("waiting_for_elicitation"), true);
				Result->SetStringField(TEXT("waiting_request_id"), Job.WaitingRequestId);
				Result->SetStringField(TEXT("waiting_method"), Job.WaitingMethod);
				Result->SetStringField(TEXT("waiting_reason"), Job.WaitingReason);
				Result->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
			}
			if (!Job.TraceId.IsEmpty())
			{
				Result->SetStringField(TEXT("trace_id"), Job.TraceId);
			}
			if (!Job.PlanLabel.IsEmpty())
			{
				Result->SetStringField(TEXT("plan_label"), Job.PlanLabel);
			}
			Result->SetArrayField(TEXT("step_results"), Job.StepResults);
			Result->SetNumberField(TEXT("last_event_seq"), Job.NextEventSeq - 1);
			if (Job.Events.Num() > 0)
			{
				Result->SetStringField(TEXT("last_event_type"), Job.Events.Last().Type);
				Result->SetStringField(TEXT("last_event_message"), Job.Events.Last().Message);
				Result->SetStringField(TEXT("last_event_timestamp"), Job.Events.Last().TimestampIso);
			}
			if (!Job.ErrorCode.IsEmpty())
			{
				Result->SetStringField(TEXT("error_code"), Job.ErrorCode);
			}
			if (!Job.ErrorMessage.IsEmpty())
			{
				Result->SetStringField(TEXT("error_message"), Job.ErrorMessage);
			}
			if (!Job.ErrorCode.IsEmpty() || !Job.ErrorMessage.IsEmpty())
			{
				TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
				Error->SetStringField(TEXT("code"), Job.ErrorCode.IsEmpty() ? TEXT("UNKNOWN") : Job.ErrorCode);
				Error->SetStringField(TEXT("message"), Job.ErrorMessage);
				Error->SetBoolField(TEXT("retryable"), Job.Status != TEXT("cancelled"));
				Result->SetObjectField(TEXT("error"), Error);
			}
			Result->SetObjectField(TEXT("receipt_envelope"), BuildReceiptEnvelope(Job));

			// 增量推送实时日志行（只推送上次 jobs/get 之后新增的行）
			// live_log_lines: 新行数组，live_log_total: 当前总行数（用于客户端追踪进度）
			const int32 TotalLines = Job.LiveLogLines.Num();
			const int32 NewFrom = Job.LiveLogSeenCount;
			Result->SetNumberField(TEXT("live_log_total"), TotalLines);
			TArray<TSharedPtr<FJsonValue>> NewLines;
			for (int32 i = NewFrom; i < TotalLines; ++i)
			{
				NewLines.Add(MakeShared<FJsonValueString>(Job.LiveLogLines[i]));
			}
			Result->SetArrayField(TEXT("live_log_lines"), NewLines);
			// 更新客户端上次读取位置（下次 jobs/get 只推送新增行）
			Job.LiveLogSeenCount = TotalLines;

			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> JobEventsToJson(const FSololmcpJobState& Job, const int32 SinceSeq)
		{
			TArray<TSharedPtr<FJsonValue>> EventsJson;
			for (const FSololmcpJobEvent& Event : Job.Events)
			{
				if (Event.Seq <= SinceSeq)
				{
					continue;
				}
				TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
				EventJson->SetNumberField(TEXT("seq"), Event.Seq);
				EventJson->SetStringField(TEXT("type"), Event.Type);
				EventJson->SetStringField(TEXT("message"), Event.Message);
				EventJson->SetStringField(TEXT("timestamp"), Event.TimestampIso);
				EventJson->SetObjectField(TEXT("payload"), Event.Payload);
				EventsJson.Add(MakeShared<FJsonValueObject>(EventJson));
			}
			return EventsJson;
		}

		TSharedPtr<FJsonValue> ResolveTemplateString(const FString& Value, const FSololmcpJobState& Job);
		TSharedPtr<FJsonValue> ResolveValue(const TSharedPtr<FJsonValue>& InValue, const FSololmcpJobState& Job);

		TSharedRef<FJsonObject> ResolveObject(const TSharedRef<FJsonObject>& InObject, const FSololmcpJobState& Job)
		{
			TSharedRef<FJsonObject> OutObject = MakeShared<FJsonObject>();
			for (const auto& Pair : InObject->Values)
			{
				OutObject->SetField(FString(*Pair.Key), ResolveValue(Pair.Value, Job));
			}
			return OutObject;
		}

		TSharedPtr<FJsonValue> ResolveTemplateString(const FString& Value, const FSololmcpJobState& Job)
		{
			if (!Value.StartsWith(TEXT("{{")) || !Value.EndsWith(TEXT("}}")))
			{
				return MakeShared<FJsonValueString>(Value);
			}

			const FString Expr = Value.Mid(2, Value.Len() - 4).TrimStartAndEnd();
			TArray<FString> Parts;
			Expr.ParseIntoArray(Parts, TEXT("."), true);
			if (Parts.Num() < 3 || !Parts[0].Equals(TEXT("steps"), ESearchCase::IgnoreCase))
			{
				return MakeShared<FJsonValueString>(Value);
			}

			const int32 StepIndex = FCString::Atoi(*Parts[1]);
			if (!Job.StepResults.IsValidIndex(StepIndex) || !Job.StepResults[StepIndex].IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}

			const TSharedPtr<FJsonObject> StepObj = Job.StepResults[StepIndex]->AsObject();
			if (!StepObj.IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}

			if (Parts[2].Equals(TEXT("summary"), ESearchCase::IgnoreCase))
			{
				FString Summary;
				if (StepObj->TryGetStringField(TEXT("summary"), Summary))
				{
					return MakeShared<FJsonValueString>(Summary);
				}
				return MakeShared<FJsonValueNull>();
			}

			TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(StepObj.ToSharedRef());
			for (int32 i = 2; i < Parts.Num(); ++i)
			{
				if (!Current.IsValid() || Current->Type != EJson::Object)
				{
					return MakeShared<FJsonValueNull>();
				}

				const TSharedPtr<FJsonObject> Obj = Current->AsObject();
				if (!Obj.IsValid())
				{
					return MakeShared<FJsonValueNull>();
				}

				Current = Obj->TryGetField(Parts[i]);
				if (!Current.IsValid())
				{
					return MakeShared<FJsonValueNull>();
				}
			}
			return Current;
		}

		TSharedPtr<FJsonValue> ResolveValue(const TSharedPtr<FJsonValue>& InValue, const FSololmcpJobState& Job)
		{
			if (!InValue.IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}

			switch (InValue->Type)
			{
			case EJson::String:
				return ResolveTemplateString(InValue->AsString(), Job);
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Obj = InValue->AsObject();
				if (Obj.IsValid())
				{
					return MakeShared<FJsonValueObject>(ResolveObject(Obj.ToSharedRef(), Job));
				}
				return MakeShared<FJsonValueNull>();
			}
			case EJson::Array:
			{
				TArray<TSharedPtr<FJsonValue>> OutArray;
				for (const TSharedPtr<FJsonValue>& Item : InValue->AsArray())
				{
					OutArray.Add(ResolveValue(Item, Job));
				}
				return MakeShared<FJsonValueArray>(OutArray);
			}
			case EJson::Number:
				return MakeShared<FJsonValueNumber>(InValue->AsNumber());
			case EJson::Boolean:
				return MakeShared<FJsonValueBoolean>(InValue->AsBool());
			case EJson::Null:
			default:
				return MakeShared<FJsonValueNull>();
			}
		}

		// FIXED #8: 按 SubmitTime 升序排序（最旧优先），而非 GUID 字典序
		void PruneOldJobsIfNeeded()
		{
			if (GJobs.Num() <= MaxTrackedJobs)
			{
				return;
			}
			// 收集已终止的作业，按提交时间升序排列
			TArray<TPair<double, FString>> TerminalByTime;
			for (const TPair<FString, FSololmcpJobState>& Pair : GJobs)
			{
				const FString& S = Pair.Value.Status;
				if (IsTerminalJobStatus(S))
				{
					TerminalByTime.Add(TPair<double, FString>(Pair.Value.SubmitTimeSec, Pair.Key));
				}
			}
			TerminalByTime.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B)
			{
				return A.Key < B.Key; // 最旧优先
			});

			int32 ToRemove = GJobs.Num() - MaxTrackedJobs + 32;
			for (int32 i = 0; i < TerminalByTime.Num() && ToRemove > 0; ++i)
			{
				const FString& JobIdToRemove = TerminalByTime[i].Value;
				FSololmcpJobState* Removed = GJobs.Find(JobIdToRemove);
				if (Removed && !Removed->ClientRequestId.IsEmpty())
				{
					GRequestIdToJobId.Remove(Removed->ClientRequestId);
				}
				if (Removed)
				{
					ReleaseJobResourceLocks(*Removed);
				}
				// Audit round 9 (group D - phase 1 priority queue):
				//   修剪终态 Job 时也从优先级队列剔除（虽然它们已经不会被推进，
				//   但留着会让 TickJobs 多走一次 GJobs.Find=nullptr → continue 的死循环开销）。
				RemoveJobIdFromAllQueues(JobIdToRemove);
				GJobTombstones.Add(
					JobIdToRemove,
					FSololmcpJobTombstone{TEXT("JOB_PRUNED_CAPACITY"), FPlatformTime::Seconds()});
				GJobs.Remove(JobIdToRemove);
				--ToRemove;
			}

			// Tombstones make a pruned receipt distinguishable from a never-valid id,
			// but remain bounded so diagnostics cannot become a second leak.
			if (GJobTombstones.Num() > MaxJobTombstones)
			{
				TArray<TPair<double, FString>> TombstonesByTime;
				TombstonesByTime.Reserve(GJobTombstones.Num());
				for (const TPair<FString, FSololmcpJobTombstone>& Pair : GJobTombstones)
				{
					TombstonesByTime.Emplace(Pair.Value.RemovedTimeSec, Pair.Key);
				}
				TombstonesByTime.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B)
				{
					return A.Key < B.Key;
				});
				const int32 RemoveCount = GJobTombstones.Num() - MaxJobTombstones;
				for (int32 Index = 0; Index < RemoveCount; ++Index)
				{
					GJobTombstones.Remove(TombstonesByTime[Index].Value);
				}
			}
		}

		// FIX-3 (job queue conn design 20260804): 只读冲突探测。
		//   与 TryAcquireJobResourceLocks 第一个循环相同的冲突规则，但不获取锁：
		//   供 SubmitJob 提交期冲突检测和生命周期看门狗的锁等待超时前复查使用。
		bool DetectJobLockContention(const FSololmcpJobState& Job, FString& OutLockId, FString& OutHolderJobId, FString& OutReason)
		{
			for (const FMcpJobResourceLock& Lock : Job.ResourceLocks)
			{
				const TArray<TPair<FString, FString>>* Holders = GActiveJobResourceLocks.Find(Lock.Id);
				if (!Holders)
				{
					continue;
				}
				for (const TPair<FString, FString>& Holder : *Holders)
				{
					if (Holder.Key == Job.JobId)
					{
						continue;
					}
					if (IsExclusiveResourceLock(Lock.Mode) || IsExclusiveResourceLock(Holder.Value))
					{
						OutLockId = Lock.Id;
						OutHolderJobId = Holder.Key;
						OutReason = FString::Printf(TEXT("resource lock '%s' is held by job '%s'"), *Lock.Id, *Holder.Key);
						return true;
					}
				}
			}
			return false;
		}

		// FIX-3 (job queue conn design 20260804): 锁等待超时回执的诊断负载。
		//   逐个列出占用本 Job 需求锁的其他作业及其当前状态，便于客户端定位持有者。
		TArray<TSharedPtr<FJsonValue>> BuildLockDiagnostics(const FSololmcpJobState& Job)
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FMcpJobResourceLock& Lock : Job.ResourceLocks)
			{
				const TArray<TPair<FString, FString>>* Holders = GActiveJobResourceLocks.Find(Lock.Id);
				if (!Holders)
				{
					continue;
				}
				for (const TPair<FString, FString>& Holder : *Holders)
				{
					if (Holder.Key == Job.JobId)
					{
						continue;
					}
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("lock_id"), Lock.Id);
					Row->SetStringField(TEXT("holder_job_id"), Holder.Key);
					Row->SetStringField(TEXT("holder_mode"), Holder.Value);
					FString HolderStatus = TEXT("unknown");
					if (const FSololmcpJobState* HolderJob = GJobs.Find(Holder.Key))
					{
						HolderStatus = HolderJob->Status;
					}
					Row->SetStringField(TEXT("holder_status"), HolderStatus);
					Rows.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
			return Rows;
		}

		// FIX-1 (job queue conn design 20260804): 看门狗统一的终态失败出口。
		//   置 failed + 错误码，发 watchdog 事件（带恢复指引），释放资源锁、
		//   从优先级队列移除并计入失败遥测。调用方保证 Job 非终态。
		void FailJobWithWatchdog(FSololmcpJobState& Job, const FString& ErrorCode, const FString& Message, const TSharedPtr<FJsonObject>& ExtraPayload = nullptr)
		{
			if (IsTerminalJobStatus(Job.Status))
			{
				return;
			}
			Job.Status = TEXT("failed");
			Job.ErrorCode = ErrorCode;
			Job.ErrorMessage = Message;
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("error_code"), ErrorCode);
			Payload->SetBoolField(TEXT("watchdog"), true);
			if (ExtraPayload.IsValid())
			{
				for (const auto& Pair : ExtraPayload->Values)
				{
					Payload->SetField(FString(*Pair.Key), Pair.Value);
				}
			}
			AddJobEvent(Job, TEXT("error"), Message, Payload);
			AddJobEvent(Job, TEXT("failed"), Message, Payload);
			ReleaseJobResourceLocks(Job);
			RemoveJobIdFromAllQueues(Job.JobId);
			AppendTerminalOutcome(true);
		}

		// FIX-1/FIX-2/FIX-3 (job queue conn design 20260804): 生命周期看门狗清扫。
		//   ① 非外部 running 作业超 maximum_runtime_ms → E_JOB_RUNTIME_EXCEEDED
		//   ② blocked 作业超 blocked TTL → E_BLOCKED_TTL_EXPIRED
		//   ③ queued/running 等锁作业超锁等待 TTL（且复查后仍冲突）→ E_LOCK_WAIT_TIMEOUT
		//   ④ 外部执行器作业租约失效 → E_EXTERNAL_JOB_STALE（锁同步释放）
		//   每次清扫最多执行 64 个终态动作，避免单帧长尾。
		void SweepJobLifecycleInternal(const double NowSec)
		{
			FScopeLock Guard(&GJobsMutex);
			constexpr int32 MaxActionsPerSweep = 64;
			int32 Actions = 0;

			TArray<FString> JobIds;
			JobIds.Reserve(GJobs.Num());
			for (const TPair<FString, FSololmcpJobState>& Pair : GJobs)
			{
				JobIds.Add(Pair.Key);
			}

			const double BlockedTtlSec = static_cast<double>(CVarJobsBlockedTtlSec.GetValueOnGameThread());
			const double LockWaitTtlSec = static_cast<double>(CVarJobsLockWaitTtlSec.GetValueOnGameThread());
			const double ExternalStaleSec = static_cast<double>(CVarJobsExternalStaleSec.GetValueOnGameThread());

			for (const FString& JobId : JobIds)
			{
				if (Actions >= MaxActionsPerSweep)
				{
					break;
				}
				FSololmcpJobState* JobPtr = GJobs.Find(JobId);
				if (!JobPtr || IsTerminalJobStatus(JobPtr->Status))
				{
					continue;
				}
				FSololmcpJobState& Job = *JobPtr;

				// ① running 超 maximum_runtime_ms（同时覆盖卡死的 worker 快路径作业）
				if (!Job.bExternal && Job.Status == TEXT("running") && Job.FirstRunningTimeSec > 0.0)
				{
					const double RuntimeSec = NowSec - Job.FirstRunningTimeSec;
					const double LimitSec = static_cast<double>(Job.MaximumRuntimeMs) / 1000.0;
					if (LimitSec > 0.0 && RuntimeSec > LimitSec)
					{
						TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
						Extra->SetNumberField(TEXT("runtime_sec"), RuntimeSec);
						Extra->SetNumberField(TEXT("limit_sec"), LimitSec);
						Extra->SetStringField(TEXT("recovery"), TEXT("Resubmit with a larger maximum_runtime_ms or split the plan into smaller steps."));
						FailJobWithWatchdog(Job, TEXT("E_JOB_RUNTIME_EXCEEDED"),
							FString::Printf(TEXT("Job exceeded maximum runtime (%.1fs > %.1fs)."), RuntimeSec, LimitSec), Extra);
						++Actions;
						continue;
					}
				}

				// ② blocked 超 TTL（TargetGuard / elicitation 两种阻塞都有 BlockedSinceSec 锚点）
				if (Job.Status == TEXT("blocked") && BlockedTtlSec > 0.0 && Job.BlockedSinceSec > 0.0
					&& (NowSec - Job.BlockedSinceSec) > BlockedTtlSec)
				{
					TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
					Extra->SetNumberField(TEXT("blocked_sec"), NowSec - Job.BlockedSinceSec);
					Extra->SetStringField(TEXT("blocked_reason"), Job.BlockedReason);
					Extra->SetStringField(TEXT("recovery"), TEXT("Answer via jobs/resume for elicitation-blocked jobs, resubmit with the required target binding, or cancel the job."));
					FailJobWithWatchdog(Job, TEXT("E_BLOCKED_TTL_EXPIRED"),
						FString::Printf(TEXT("Job stayed blocked for %.1fs (ttl %.1fs)."), NowSec - Job.BlockedSinceSec, BlockedTtlSec), Extra);
					++Actions;
					continue;
				}

				// ③ 锁等待超时：复查冲突是否仍存在，已解除则清零锚点交给下次 Tick 拿锁
				if (!Job.bExternal
					&& (Job.Status == TEXT("queued") || Job.Status == TEXT("running"))
					&& Job.ResourceLocks.Num() > 0 && !Job.bResourceLocksAcquired
					&& LockWaitTtlSec > 0.0 && Job.LockWaitingSinceSec > 0.0)
				{
					FString ContendLockId;
					FString ContendHolderJobId;
					FString ContendReason;
					if (!DetectJobLockContention(Job, ContendLockId, ContendHolderJobId, ContendReason))
					{
						Job.LockWaitingSinceSec = 0.0;
						continue;
					}
					if ((NowSec - Job.LockWaitingSinceSec) > LockWaitTtlSec)
					{
						TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
						Extra->SetNumberField(TEXT("wait_sec"), NowSec - Job.LockWaitingSinceSec);
						Extra->SetStringField(TEXT("blocking_lock_id"), ContendLockId);
						Extra->SetStringField(TEXT("blocking_job_id"), ContendHolderJobId);
						Extra->SetArrayField(TEXT("blocking_locks"), BuildLockDiagnostics(Job));
						Extra->SetStringField(TEXT("recovery"), TEXT("Inspect blocking_locks to locate the holder job; cancel or wait for it, then resubmit."));
						FailJobWithWatchdog(Job, TEXT("E_LOCK_WAIT_TIMEOUT"),
							FString::Printf(TEXT("Job waited %.1fs for resource lock '%s' (ttl %.1fs)."),
								NowSec - Job.LockWaitingSinceSec, *ContendLockId, LockWaitTtlSec), Extra);
						++Actions;
					}
					continue;
				}

				// ④ 外部执行器租约失效：锚点 = 最近一次 progress/heartbeat
				if (Job.bExternal && ExternalStaleSec > 0.0)
				{
					const double LeaseSec = Job.ExternalLeaseSec > 0.0 ? Job.ExternalLeaseSec : ExternalStaleSec;
					const double AnchorSec = Job.LastUpdateTimeSec > 0.0 ? Job.LastUpdateTimeSec : Job.SubmitTimeSec;
					if ((NowSec - AnchorSec) > LeaseSec)
					{
						TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
						Extra->SetNumberField(TEXT("lease_sec"), LeaseSec);
						Extra->SetNumberField(TEXT("silent_sec"), NowSec - AnchorSec);
						Extra->SetStringField(TEXT("recovery"), TEXT("External executor sent no progress or jobs/heartbeat within the lease; verify the executor is alive, then resubmit or keep it alive with jobs/heartbeat."));
						FailJobWithWatchdog(Job, TEXT("E_EXTERNAL_JOB_STALE"),
							FString::Printf(TEXT("External job lease expired after %.1fs without progress or heartbeat (lease %.1fs)."),
								NowSec - AnchorSec, LeaseSec), Extra);
						++Actions;
					}
				}
			}
		}

		// FIX-1 (job queue conn design 20260804): TickJobs 末尾的节流入口。
		//   测试走 TestRunLifecycleSweep 直接调 Internal，绕过节流与墙钟依赖。
		void SweepJobLifecycle(const double NowSec)
		{
			static double GLastLifecycleSweepSec = 0.0;
			const double IntervalSec = FMath::Max(0.1, static_cast<double>(CVarJobsLifecycleSweepIntervalSec.GetValueOnGameThread()));
			if (GLastLifecycleSweepSec > 0.0 && (NowSec - GLastLifecycleSweepSec) < IntervalSec)
			{
				return;
			}
			GLastLifecycleSweepSec = NowSec;
			SweepJobLifecycleInternal(NowSec);
		}
	}

	void FSololmcpJobService::BuildCapabilitiesJobsObject(const TSharedRef<FJsonObject>& JobsObj)
	{
		JobsObj->SetBoolField(TEXT("supported"), true);
		JobsObj->SetBoolField(TEXT("events"), true);
		JobsObj->SetBoolField(TEXT("idempotentSubmit"), true);
		JobsObj->SetBoolField(TEXT("stepTemplateRefs"), true);
		JobsObj->SetBoolField(TEXT("mcpJobTools"), true);
		JobsObj->SetBoolField(TEXT("submitReturnsImmediately"), true);
		JobsObj->SetBoolField(TEXT("stillRunningSnapshot"), true);
		JobsObj->SetBoolField(TEXT("heartbeatEvents"), true);
		JobsObj->SetBoolField(TEXT("receiptEnvelope"), true);
		JobsObj->SetBoolField(TEXT("blockedStatus"), true);
		JobsObj->SetBoolField(TEXT("targetGuardFailClosed"), true);
		JobsObj->SetBoolField(TEXT("independentTickMonitor"), true);
		JobsObj->SetBoolField(TEXT("resourceLockScheduling"), true);
		JobsObj->SetBoolField(TEXT("resourceLockAutoInference"), true);
		JobsObj->SetBoolField(TEXT("elicitationBlockedResume"), true);
		JobsObj->SetStringField(TEXT("elicitationCreateMethod"), TEXT("jobs/elicit"));
		JobsObj->SetStringField(TEXT("elicitationResumeMethod"), TEXT("jobs/resume"));
		JobsObj->SetStringField(TEXT("resourceLockSchema"), TEXT("somol.mcp_resource_lock:v1"));
		JobsObj->SetStringField(TEXT("clientPattern"), TEXT("jobs/submit -> job_id; poll jobs/get or jobs/events until terminal status."));
		// Audit round 9 (group D - phase 1 priority queue):
		//   告诉客户端可以在 jobs/submit 的 params 里加 _priority ∈ {urgent,high,mid,low}。
		JobsObj->SetBoolField(TEXT("priorityScheduling"), true);
		JobsObj->SetStringField(TEXT("priorityField"), TEXT("_priority"));
		JobsObj->SetStringField(TEXT("priorityAlias"), TEXT("priority"));
		// FIX-1/FIX-2/FIX-3 (job queue conn design 20260804): 生命周期看门狗能力面。
		//   只新增能力描述字段与 jobs/heartbeat 方法，不新增同义 MCP 工具名。
		JobsObj->SetBoolField(TEXT("lifecycleWatchdog"), true);
		JobsObj->SetStringField(TEXT("externalJobHeartbeatMethod"), TEXT("jobs/heartbeat"));
		JobsObj->SetStringField(TEXT("watchdogErrorCodes"),
			TEXT("E_JOB_RUNTIME_EXCEEDED,E_BLOCKED_TTL_EXPIRED,E_LOCK_WAIT_TIMEOUT,E_EXTERNAL_JOB_STALE"));
		JobsObj->SetStringField(TEXT("ownership"), TEXT("process_global_session_independent"));
		JobsObj->SetStringField(TEXT("runtimeInstanceId"), GJobRuntimeInstanceId);
		JobsObj->SetNumberField(TEXT("maxTrackedJobs"), MaxTrackedJobs);
		JobsObj->SetNumberField(TEXT("maxTombstones"), MaxJobTombstones);
		JobsObj->SetStringField(TEXT("missingJobErrorCodes"), TEXT("JOB_NOT_FOUND,JOB_PRUNED_CAPACITY"));
		TArray<TSharedPtr<FJsonValue>> Levels;
		Levels.Add(MakeShared<FJsonValueString>(TEXT("urgent")));
		Levels.Add(MakeShared<FJsonValueString>(TEXT("high")));
		Levels.Add(MakeShared<FJsonValueString>(TEXT("mid")));
		Levels.Add(MakeShared<FJsonValueString>(TEXT("low")));
		JobsObj->SetArrayField(TEXT("priorityLevels"), Levels);
	}

	void FSololmcpJobService::BuildMetricsObject(const TSharedRef<FJsonObject>& JobsObj, const TArray<FString>& RegisteredToolNames)
	{
		int32 Queued = 0;
		int32 Running = 0;
		int32 Succeeded = 0;
		int32 Failed = 0;
		int32 Cancelled = 0;
		int32 Blocked = 0;
		int32 TrackedJobs = 0;
		int32 Tombstones = 0;
		int32 ResourceLockWaiting = 0;
		{
			FScopeLock JobsLock(&GJobsMutex);
			for (const TPair<FString, FSololmcpJobState>& Pair : GJobs)
			{
				const FString& Status = Pair.Value.Status;
				if (Status == TEXT("queued"))
				{
					++Queued;
				}
				else if (Status == TEXT("running"))
				{
					++Running;
				}
				else if (Status == TEXT("succeeded"))
				{
					++Succeeded;
				}
				else if (Status == TEXT("failed"))
				{
					++Failed;
				}
				else if (Status == TEXT("cancelled"))
				{
					++Cancelled;
				}
				else if (Status == TEXT("blocked"))
				{
					++Blocked;
				}
				if (!Pair.Value.BlockedLockId.IsEmpty())
				{
					++ResourceLockWaiting;
				}
			}
			TrackedJobs = GJobs.Num();
			Tombstones = GJobTombstones.Num();
		}

		const int32 WorkerSafeSubmissions = GWorkerSafeSingleStepSubmissions.GetValue();
		const int32 WorkerFallbacks = GWorkerFallbackToGameThread.GetValue();
		const double WorkerSafeSubmissionRatio = GSubmittedJobs.GetValue() > 0
			? static_cast<double>(WorkerSafeSubmissions) / static_cast<double>(GSubmittedJobs.GetValue())
			: 0.0;
		const double WorkerFallbackRatio = WorkerSafeSubmissions > 0
			? static_cast<double>(WorkerFallbacks) / static_cast<double>(WorkerSafeSubmissions)
			: 0.0;
		const double TickAgeMs = LastTickJobsAgeMs();

		JobsObj->SetNumberField(TEXT("current_game_thread_cap"), GMaxConcurrentMcpJobs);
		JobsObj->SetNumberField(TEXT("current_worker_cap"), GMaxConcurrentMcpJobsWorker);
		JobsObj->SetNumberField(TEXT("game_thread_in_flight"), GThreadInFlightGameThread.GetValue());
		JobsObj->SetNumberField(TEXT("worker_in_flight"), GThreadInFlightWorker.GetValue());
		JobsObj->SetNumberField(TEXT("queued_jobs"), Queued);
		JobsObj->SetNumberField(TEXT("running_jobs"), Running);
		JobsObj->SetNumberField(TEXT("succeeded_jobs"), Succeeded);
		JobsObj->SetNumberField(TEXT("failed_jobs"), Failed);
		JobsObj->SetNumberField(TEXT("cancelled_jobs"), Cancelled);
		JobsObj->SetNumberField(TEXT("blocked_jobs"), Blocked);
		JobsObj->SetNumberField(TEXT("tracked_jobs"), TrackedJobs);
		JobsObj->SetNumberField(TEXT("max_tracked_jobs"), MaxTrackedJobs);
		JobsObj->SetNumberField(TEXT("job_tombstones"), Tombstones);
		JobsObj->SetStringField(TEXT("runtime_instance_id"), GJobRuntimeInstanceId);
		JobsObj->SetNumberField(TEXT("total_submissions"), GSubmittedJobs.GetValue());
		JobsObj->SetNumberField(TEXT("tick_jobs_count"), static_cast<double>(TickJobsCount()));
		JobsObj->SetNumberField(TEXT("last_tick_jobs_age_ms"), TickAgeMs);
		JobsObj->SetBoolField(TEXT("job_monitor_tick_active"), TickAgeMs >= 0.0);
		JobsObj->SetNumberField(TEXT("worker_safe_tools"), CountWorkerSafeRegisteredTools(RegisteredToolNames));
		JobsObj->SetNumberField(TEXT("worker_safe_single_step_submissions"), WorkerSafeSubmissions);
		JobsObj->SetNumberField(TEXT("worker_fallback_to_game_thread"), WorkerFallbacks);
		JobsObj->SetNumberField(TEXT("worker_safe_submission_ratio"), WorkerSafeSubmissionRatio);
		JobsObj->SetNumberField(TEXT("worker_fallback_ratio"), WorkerFallbackRatio);
		JobsObj->SetNumberField(TEXT("active_resource_locks"), GActiveJobResourceLocks.Num());
		JobsObj->SetNumberField(TEXT("resource_lock_waiting_jobs"), ResourceLockWaiting);
		JobsObj->SetNumberField(TEXT("resource_lock_blocked_ticks"), static_cast<double>(GResourceLockBlockedTicks));
		JobsObj->SetNumberField(TEXT("game_thread_p95_ms"), RecentGameThreadP95Ms());
		JobsObj->SetNumberField(TEXT("recent_failure_ratio"), RecentFailureRatio());
		JobsObj->SetBoolField(TEXT("read_only"), true);
	}

	void FSololmcpJobService::BuildJobsSnapshotObject(const TSharedRef<FJsonObject>& JobsObj)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		int32 Queued = 0;
		int32 Running = 0;
		int32 Blocked = 0;
		int32 Succeeded = 0;
		int32 Failed = 0;
		int32 Cancelled = 0;

		FScopeLock JobsLock(&GJobsMutex);
		for (const TPair<FString, FSololmcpJobState>& Pair : GJobs)
		{
			const FSololmcpJobState& Job = Pair.Value;
			if (Job.Status == TEXT("queued")) { ++Queued; }
			else if (Job.Status == TEXT("running")) { ++Running; }
			else if (Job.Status == TEXT("blocked")) { ++Blocked; }
			else if (Job.Status == TEXT("succeeded")) { ++Succeeded; }
			else if (Job.Status == TEXT("failed")) { ++Failed; }
			else if (Job.Status == TEXT("cancelled")) { ++Cancelled; }

			if (Items.Num() < 128)
			{
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("job_id"), Job.JobId);
				Obj->SetStringField(TEXT("status"), Job.Status);
				Obj->SetStringField(TEXT("client_status"), JobClientStatus(Job));
				Obj->SetBoolField(TEXT("terminal"), IsTerminalJobStatus(Job.Status));
				Obj->SetNumberField(TEXT("current_step"), Job.CurrentStep);
				Obj->SetNumberField(TEXT("total_steps"), Job.Steps.Num());
				Obj->SetNumberField(TEXT("progress"), FMath::Clamp(ComputeJobProgress(Job), 0.0, 1.0));
				Obj->SetNumberField(TEXT("last_event_seq"), Job.NextEventSeq - 1);
				Obj->SetStringField(TEXT("priority"), PriorityToString(Job.Priority));
				Obj->SetArrayField(TEXT("resource_locks"), ResourceLocksToJson(Job.ResourceLocks));
				Obj->SetBoolField(TEXT("resource_locks_acquired"), Job.bResourceLocksAcquired);
				Obj->SetStringField(TEXT("resource_lock_source"), Job.ResourceLockSource);
				Obj->SetNumberField(TEXT("resource_lock_block_count"), Job.ResourceLockBlockCount);
				if (!Job.BlockedLockId.IsEmpty())
				{
					Obj->SetStringField(TEXT("blocked_lock_id"), Job.BlockedLockId);
					Obj->SetStringField(TEXT("blocked_by_job_id"), Job.BlockedByJobId);
					Obj->SetStringField(TEXT("blocked_reason"), Job.BlockedReason);
				}
				Obj->SetBoolField(TEXT("target_guard_required"), Job.bRequiresTargetGuard);
				Obj->SetBoolField(TEXT("target_guard_satisfied"), Job.bTargetGuardSatisfied);
				Obj->SetStringField(TEXT("target_guard_status"), Job.TargetGuardStatus);
				Obj->SetBoolField(TEXT("waiting_for_elicitation"), Job.bWaitingForElicitation);
				if (Job.bWaitingForElicitation)
				{
					Obj->SetStringField(TEXT("waiting_request_id"), Job.WaitingRequestId);
					Obj->SetStringField(TEXT("waiting_method"), Job.WaitingMethod);
					Obj->SetStringField(TEXT("waiting_reason"), Job.WaitingReason);
					Obj->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
				}
				if (!Job.PlanLabel.IsEmpty())
				{
					Obj->SetStringField(TEXT("plan_label"), Job.PlanLabel);
				}
				Items.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}

		JobsObj->SetStringField(TEXT("schema"), TEXT("somolmcp.jobs.snapshot:v1"));
		JobsObj->SetNumberField(TEXT("tracked_jobs"), GJobs.Num());
		JobsObj->SetNumberField(TEXT("total_submissions"), GSubmittedJobs.GetValue());
		JobsObj->SetNumberField(TEXT("queued"), Queued);
		JobsObj->SetNumberField(TEXT("running"), Running);
		JobsObj->SetNumberField(TEXT("blocked"), Blocked);
		JobsObj->SetNumberField(TEXT("succeeded"), Succeeded);
		JobsObj->SetNumberField(TEXT("failed"), Failed);
		JobsObj->SetNumberField(TEXT("cancelled"), Cancelled);
		JobsObj->SetArrayField(TEXT("jobs"), Items);
		JobsObj->SetBoolField(TEXT("truncated"), GJobs.Num() > Items.Num());
	}

	void FSololmcpJobService::TickJobs(FSololmcpToolRegistry& Registry)
	{
		// Phase 3A: stash registry pointer for SubmitJob worker fast-path.
		GMcpRegistry = &Registry;
		RecordTickJobsHeartbeat();
		// REENTRY GUARD (Apr 2026 round 2): pcg_job_poll(wait>0) / mcp_plan_run /
		// jobs/await tools call AwaitJob() inside ExecuteTool, and AwaitJob calls
		// TickJobs again to "advance one frame". When the inner TickJobs runs the
		// SAME JobId the outer is mid-iterating, both passes ++CurrentStep, so a
		// single user-step gets logged twice / Job.Status flips to succeeded
		// before the outer pass writes its result. The TMap fix below handled
		// the *crash* path; this guard handles the *correctness* path.
		// Static is fine — TickJobs is only called from the GameThread.
		static bool bTickInProgress = false;
		if (bTickInProgress)
		{
			return; // outer pass is still going; let it advance the same JobId
		}
		struct FTickGuard {
			FTickGuard()  { bTickInProgress = true; }
			~FTickGuard() { bTickInProgress = false; }
		} ReentryGuard;

		// CRASH FIX (Apr 2026): iterating GJobs by reference and calling
		// Registry.ExecuteTool inside the loop is unsafe — some tools (e.g.
		// pcg_generate_async) re-enter MCP and may SubmitJob, which adds to
		// GJobs and rehashes the TMap, invalidating any outstanding Pair
		// reference / pointer. The very next access trips:
		//   Assertion failed: Pair != nullptr
		//   [File: Engine/Source/Runtime/Core/Public/Containers/Map.h.inl] [Line: 635]
		// (observed: SOMOL.log 2026-04-26 16:19:26 during a smoke run).
		// Audit round 9 (group D - phase 1 priority queue):
		//   原 round-1 fix（snapshot keys + re-Find）在新设计中**自然安全**：
		//   每次循环只从队列弹出一个 JobId、推一步、再 push 回队尾。整个过程
		//   不持有 GJobs 的迭代器，re-Find(JobId) 仍是必要的（ExecuteTool 后
		//   GJobs 可能 rehash），所以 JobPtr 在 ExecuteTool 前后都重新查找。
		//
		// Audit round 9 (group D - phase 1 priority queue):
		//   单帧时间预算 12ms。WRR 配额：High=4, Mid=2, Low=1（quantum=7）。
		//   Urgent 队列优先全部清空，且不参与 WRR 配额。
		// Phase 3B (GameThread cooperative yield):
		//   Hard yield bound is 8ms — checked BETWEEN steps. One heavy tool can
		//   still overrun a single step, but we won't *start* another after the
		//   budget is consumed. Outer FrameBudgetSec=12ms remains as a soft cap.
		constexpr double FrameBudgetSec = 0.012; // 12ms 软上限
		constexpr double YieldBudgetSec = 0.008; // 8ms 硬上限 (Phase 3B)
		const double FrameStart = FPlatformTime::Seconds();
		static int32 GRRCounter = 0; // GameThread-only

		while ((FPlatformTime::Seconds() - FrameStart) < FrameBudgetSec)
		{
			// Phase 3B: cooperative yield — break BEFORE pulling next job if
			// the 8ms hard cap is exceeded. Remaining queued work picks up next
			// engine tick (TickJobs runs every frame).
			if ((FPlatformTime::Seconds() - FrameStart) >= YieldBudgetSec)
			{
				break;
			}

			// Phase 3E: GameThread throttle. If too many GameThread tools are
			// already in-flight, defer. Worker-safe tools never reach this loop
			// (they go through SubmitJob's AsyncTask path and have a separate cap).
			if (GThreadInFlightGameThread.GetValue() >= GMaxConcurrentMcpJobs)
			{
				break; // wait until next frame; in-flight work drains naturally
			}
			// --- 1) 选下一个 JobId：Urgent 永远优先，其他按 WRR (4:2:1) ---
			FString JobId;
			if (GQueueUrgent.Num() > 0)
			{
				JobId = GQueueUrgent[0];
				GQueueUrgent.RemoveAt(0);
			}
			else
			{
				const int32 Quantum = GRRCounter % 7;
				++GRRCounter;
				if (Quantum < 4 && GQueueHigh.Num() > 0)
				{
					JobId = GQueueHigh[0]; GQueueHigh.RemoveAt(0);
				}
				else if (Quantum < 6 && GQueueMid.Num() > 0)
				{
					JobId = GQueueMid[0]; GQueueMid.RemoveAt(0);
				}
				else if (GQueueLow.Num() > 0)
				{
					JobId = GQueueLow[0]; GQueueLow.RemoveAt(0);
				}
				// Fallback：当前 quantum 对应的桶为空，但其他桶可能还有任务，
				// 不要白白浪费这次循环 —— 按 High → Mid → Low 顺序兜底。
				else if (GQueueHigh.Num() > 0) { JobId = GQueueHigh[0]; GQueueHigh.RemoveAt(0); }
				else if (GQueueMid.Num()  > 0) { JobId = GQueueMid[0];  GQueueMid.RemoveAt(0);  }
				else if (GQueueLow.Num()  > 0) { JobId = GQueueLow[0];  GQueueLow.RemoveAt(0);  }
				else { break; } // 所有非 urgent 队列都空 → 没事可做
			}
			if (JobId.IsEmpty()) { break; }

			// --- 2) 校验 Job 仍存在 + 状态可推进 ---
			FSololmcpJobState* JobPtr = GJobs.Find(JobId);
			if (!JobPtr) { continue; } // 被 PruneOldJobsIfNeeded / CancelJob 删除了 → 丢弃这个出队
			FSololmcpJobState& Job = *JobPtr;
			const EJobPriority OriginalPriority = Job.Priority;
			MaybeAddHeartbeatEvent(Job, TEXT("tick"));

			if ((Job.Status == TEXT("queued") || Job.Status == TEXT("running"))
				&& Job.ResourceLocks.Num() > 0
				&& !Job.bResourceLocksAcquired)
			{
				FString BlockedLockId;
				FString BlockedByJobId;
				FString BlockedReason;
				if (!TryAcquireJobResourceLocks(Job, BlockedLockId, BlockedByJobId, BlockedReason))
				{
					++GResourceLockBlockedTicks;
					Job.BlockedLockId = BlockedLockId;
					Job.BlockedByJobId = BlockedByJobId;
					Job.BlockedReason = BlockedReason;
					++Job.ResourceLockBlockCount;
					// FIX-3 (job queue conn design 20260804): 首次被锁挡住时记录等待锚点，
					// 生命周期看门狗据此在锁等待 TTL 到期时把作业置为 failed 而非无限重入队。
					if (Job.LockWaitingSinceSec <= 0.0)
					{
						Job.LockWaitingSinceSec = FPlatformTime::Seconds();
					}
					const double Now = FPlatformTime::Seconds();
					if (Job.LastResourceLockEventSec <= 0.0 || (Now - Job.LastResourceLockEventSec) >= 2.0)
					{
						TSharedRef<FJsonObject> BlockPayload = MakeShared<FJsonObject>();
						BlockPayload->SetStringField(TEXT("lock_id"), BlockedLockId);
						BlockPayload->SetStringField(TEXT("blocked_by_job_id"), BlockedByJobId);
						BlockPayload->SetStringField(TEXT("reason"), BlockedReason);
						BlockPayload->SetNumberField(TEXT("block_count"), Job.ResourceLockBlockCount);
						AddJobEvent(Job, TEXT("blocked"), TEXT("Job is waiting for resource locks."), BlockPayload);
						Job.LastResourceLockEventSec = Now;
					}
					EnqueueJobByPriority(JobId, Job.Priority);
					(void)OriginalPriority;
					continue;
				}

				TSharedRef<FJsonObject> LockPayload = MakeShared<FJsonObject>();
				LockPayload->SetArrayField(TEXT("resource_locks"), ResourceLocksToJson(Job.ResourceLocks));
				LockPayload->SetStringField(TEXT("resource_lock_source"), Job.ResourceLockSource);
				AddJobEvent(Job, TEXT("status"), TEXT("Resource locks acquired."), LockPayload);
				Job.LockWaitingSinceSec = 0.0; // FIX-3: 冲突解除，清零锁等待锚点
			}

			if (Job.Status == TEXT("queued"))
			{
				Job.Status = TEXT("running");
				// FIX-1 (job queue conn design 20260804): runtime 看门狗锚点
				Job.FirstRunningTimeSec = FPlatformTime::Seconds();
				Job.LastUpdateTimeSec = Job.FirstRunningTimeSec;
				AddJobEvent(Job, TEXT("status"), TEXT("Job started."));
				AddJobEvent(Job, TEXT("running"), TEXT("Job started."));
			}
			if (Job.Status != TEXT("running"))
			{
				// failed / cancelled / succeeded → 不再 push 回队列，自然丢弃
				continue;
			}
			if (!Job.Steps.IsValidIndex(Job.CurrentStep))
			{
				ReleaseJobResourceLocks(Job);
				Job.Status = TEXT("succeeded");
				AppendTerminalOutcome(false);
				AddJobEvent(Job, TEXT("status"), TEXT("Job finished successfully."));
				AddJobEvent(Job, TEXT("completed"), TEXT("Job finished successfully."));
				continue;
			}

			// --- 3) 推进一步（与 round 1-8 行为完全一致） ---
			// Value-copy the step so Step survives any rehash that ExecuteTool may trigger.
			const FSololmcpJobStep StepCopy = Job.Steps[Job.CurrentStep];
			const TSharedRef<FJsonObject> ResolvedArgs = ResolveObject(StepCopy.Arguments, Job);

			// === Phase 2 (group threading-decouple): per-step path tagging ===
			//
			// We mirror the worker-safe set used by FSololmcpRouter::HandleToolsCall
			// so jobs/await + jobs/get show consistent metrics. Live Coding constraint:
			// no new top-level symbol; declare as function-static.
			//
			// DEFERRED to next full UBT build: actually off-load worker-safe job
			// steps onto a TaskGraph worker thread. That requires a TFuture<>-style
			// wait + re-entrancy-safe Job state machine (the `bTickInProgress`
			// reentry guard above assumes single-threaded execution). A first cut
			// for full UBT build: queue worker-safe steps onto AnyHiPriThreadHiPriTask,
			// retain the GameThread budget for editor-mutating steps.
			static const TSet<FString> WorkerSafeJobTools = {
				TEXT("asset_query"),
				TEXT("log_get_lines"),
				TEXT("log_search"),
				TEXT("mcp_status"),
				TEXT("tools_list"),
				TEXT("tool_describe"),
				TEXT("tool_list_namespaces"),
				TEXT("plugin_status"),
			};
			static const TSet<FString> HybridJobTools = {
				TEXT("asset_get_metadata"),
				TEXT("asset_query_advanced"),
			};
			static int32 GPhase2JobWorkerCount     = 0;
			static int32 GPhase2JobGameThreadCount = 0;
			static int32 GPhase2JobHybridCount     = 0;
			const TCHAR* JobPathTag = TEXT("gamethread");
			if (WorkerSafeJobTools.Contains(StepCopy.Tool))
			{
				JobPathTag = TEXT("worker");
				++GPhase2JobWorkerCount;
			}
			else if (HybridJobTools.Contains(StepCopy.Tool))
			{
				JobPathTag = TEXT("hybrid");
				++GPhase2JobHybridCount;
			}
			else
			{
				++GPhase2JobGameThreadCount;
			}
			// Suppress unused-variable warnings on shipping configs where the
			// counters aren't surfaced yet — they're inspected in mcp_status once
			// the next full UBT build adds a getter.
			(void)GPhase2JobWorkerCount; (void)GPhase2JobGameThreadCount; (void)GPhase2JobHybridCount;
			// Tracing — same shape as Router-side log so a single grep works.
			// LogSOMOLMCPTransport is file-static inside SololmcpTcpTransport.cpp
			// so it's not in scope here. We use UE_LOG with the engine's default
			// LogTemp category — DEFERRED: promote to LogSOMOLMCP when the next
			// full UBT build can include "SOMOLMCP.h" (adding includes is
			// disallowed under Live Coding rules).
			UE_LOG(LogTemp, Verbose, TEXT("[phase2] tool=%s path=%s (job-step)"), *StepCopy.Tool, JobPathTag);

			const double StepStart = FPlatformTime::Seconds();
			TSharedRef<FJsonObject> Structured = MakeShared<FJsonObject>();
			FString Summary;
			FString Error;
			// Phase 3E: bump GameThread in-flight counter around the tool call.
			GThreadInFlightGameThread.Increment();
			// Phase 3F: SEH-isolated tool execution. Access violation / divide-by-zero
			// inside the tool body is caught here; we report the error back to the
			// caller via the Job state machine instead of crashing the editor.
			const bool bSuccess = ExecuteToolWithSehGuard(Registry, StepCopy.Tool, ResolvedArgs, Structured, Summary, Error);
			FString DomainState;
			FString DomainErrorCode;
			const bool bSemanticCancellation =
				!bSuccess
				&& Structured->TryGetStringField(TEXT("state"), DomainState)
				&& Structured->TryGetStringField(TEXT("error_code"), DomainErrorCode)
				&& DomainState.Equals(TEXT("cancelled"), ESearchCase::IgnoreCase)
				&& DomainErrorCode.Equals(TEXT("E_CANCELLED"), ESearchCase::IgnoreCase);
			GThreadInFlightGameThread.Decrement();
			const double DurationMs = (FPlatformTime::Seconds() - StepStart) * 1000.0;
			AppendRecentDuration(DurationMs);
			const FSololmcpJobStep& Step = StepCopy;

			// CRITICAL: re-Find Job after ExecuteTool — original JobPtr may now be stale
			// (ExecuteTool can re-enter SubmitJob → GJobs rehash → 旧引用失效).
			JobPtr = GJobs.Find(JobId);
			if (!JobPtr) { continue; } // cancelled/removed while running
			// All accesses below MUST go through JobPtr->, never the older
			// `Job` reference (still bound to the pre-rehash address).

			TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
			StepResult->SetNumberField(TEXT("step_index"), JobPtr->CurrentStep);
			StepResult->SetStringField(TEXT("tool"), Step.Tool);
			if (!Step.Label.IsEmpty())
			{
				StepResult->SetStringField(TEXT("label"), Step.Label);
			}
			StepResult->SetObjectField(TEXT("arguments"), ResolvedArgs);
			StepResult->SetBoolField(TEXT("ok"), bSuccess);
			StepResult->SetStringField(TEXT("summary"), bSuccess ? Summary : Error);
			StepResult->SetNumberField(TEXT("duration_ms"), DurationMs);
			StepResult->SetObjectField(TEXT("result"), Structured);
			if (!bSuccess)
			{
				TSharedRef<FJsonObject> StepError = MakeShared<FJsonObject>();
				StepError->SetStringField(
					TEXT("code"),
					bSemanticCancellation ? TEXT("E_CANCELLED") : TEXT("STEP_FAILED"));
				StepError->SetStringField(TEXT("message"), Error);
				StepError->SetBoolField(TEXT("retryable"), !bSemanticCancellation);
				StepResult->SetObjectField(TEXT("error"), StepError);
			}
			JobPtr->StepResults.Add(MakeShared<FJsonValueObject>(StepResult));

			if (bSemanticCancellation)
			{
				ReleaseJobResourceLocks(*JobPtr);
				JobPtr->Status = TEXT("cancelled");
				AppendTerminalOutcome(false);
				JobPtr->ErrorCode = TEXT("E_CANCELLED");
				JobPtr->ErrorMessage = Error;
				TSharedRef<FJsonObject> CancelPayload = MakeShared<FJsonObject>();
				CancelPayload->SetNumberField(TEXT("step_index"), JobPtr->CurrentStep);
				CancelPayload->SetStringField(TEXT("tool"), Step.Tool);
				CancelPayload->SetStringField(TEXT("error_code"), TEXT("E_CANCELLED"));
				AddJobEvent(
					*JobPtr,
					TEXT("cancelled"),
					Error.IsEmpty() ? TEXT("Domain operation cancelled.") : Error,
					CancelPayload);
				continue;
			}

			if (!bSuccess)
			{
				ReleaseJobResourceLocks(*JobPtr);
				JobPtr->Status = TEXT("failed");
				AppendTerminalOutcome(true);
				JobPtr->ErrorCode = TEXT("STEP_FAILED");
				JobPtr->ErrorMessage = Error;
				TSharedRef<FJsonObject> ErrPay = MakeShared<FJsonObject>();
				ErrPay->SetNumberField(TEXT("step_index"), JobPtr->CurrentStep);
				ErrPay->SetStringField(TEXT("tool"), Step.Tool);
				if (!Step.Label.IsEmpty())
				{
					ErrPay->SetStringField(TEXT("label"), Step.Label);
				}
				const TSharedPtr<FJsonObject>* StepErrPtr = nullptr;
				if (StepResult->TryGetObjectField(TEXT("error"), StepErrPtr) && StepErrPtr && StepErrPtr->IsValid())
				{
					ErrPay->SetObjectField(TEXT("error"), *StepErrPtr);
				}
				AddJobEvent(*JobPtr, TEXT("error"), Error, ErrPay);
				AddJobEvent(*JobPtr, TEXT("failed"), Error, ErrPay);
				continue; // failed → 不 re-enqueue
			}

			TSharedRef<FJsonObject> StepPay = MakeShared<FJsonObject>();
			StepPay->SetNumberField(TEXT("step_index"), JobPtr->CurrentStep);
			StepPay->SetStringField(TEXT("tool"), Step.Tool);
			if (!Step.Label.IsEmpty())
			{
				StepPay->SetStringField(TEXT("label"), Step.Label);
			}
			StepPay->SetBoolField(TEXT("ok"), true);
			StepPay->SetNumberField(TEXT("duration_ms"), DurationMs);
			AddJobEvent(*JobPtr, TEXT("step"), Summary.IsEmpty() ? TEXT("Step completed.") : Summary, StepPay);
			AddJobEvent(*JobPtr, TEXT("progress"), Summary.IsEmpty() ? TEXT("Step completed.") : Summary, StepPay);

			// 将本步骤的 Python 日志追加到 LiveLogLines，供下次 jobs/get 增量推送
			// 格式：[Step N][ToolName] ✓ / ✗  + 逐行日志
			const FString StepIcon = bSuccess ? TEXT("✓") : TEXT("✗");
			JobPtr->LiveLogLines.Add(FString::Printf(TEXT("[Step %d][%s] %s"), JobPtr->CurrentStep + 1, *Step.Tool, *StepIcon));
			if (const TSharedPtr<FJsonObject> ResultObj = Structured; ResultObj.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* LogArr = nullptr;
				if (ResultObj->TryGetArrayField(TEXT("log"), LogArr) && LogArr)
				{
					for (const TSharedPtr<FJsonValue>& LogEntry : *LogArr)
					{
						if (!LogEntry.IsValid()) { continue; }
						const TSharedPtr<FJsonObject> EntryObj = LogEntry->AsObject();
						if (!EntryObj.IsValid()) { continue; }
						FString EntryType, LogText;
						EntryObj->TryGetStringField(TEXT("type"), EntryType);
						EntryObj->TryGetStringField(TEXT("text"), LogText);
						if (!LogText.TrimStartAndEnd().IsEmpty()
							&& (EntryType == TEXT("Output") || EntryType == TEXT("Warning") || EntryType == TEXT("Error")))
						{
							const FString Prefix = (EntryType == TEXT("Warning")) ? TEXT("⚠ ")
								: (EntryType == TEXT("Error")) ? TEXT("✖ ") : TEXT("");
							// 多行日志按行拆分追加
							TArray<FString> TextLines;
							LogText.ParseIntoArrayLines(TextLines, false);
							for (const FString& TLine : TextLines)
							{
								if (!TLine.TrimStartAndEnd().IsEmpty())
								{
									JobPtr->LiveLogLines.Add(Prefix + TLine);
								}
							}
						}
					}
				}
				// EvaluateStatement 模式：result 字段
				FString ResultStr;
				if (ResultObj->TryGetStringField(TEXT("result"), ResultStr) && !ResultStr.TrimStartAndEnd().IsEmpty())
				{
					TArray<FString> TextLines;
					ResultStr.ParseIntoArrayLines(TextLines, false);
					for (const FString& TLine : TextLines)
					{
						if (!TLine.TrimStartAndEnd().IsEmpty())
						{
							JobPtr->LiveLogLines.Add(TLine);
						}
					}
				}
			}

			// audit-U5 fix (P2): per-job LiveLogLines cap. The previous version
			// grew monotonically per job; a long-running asset-import job could
			// balloon to megabytes before the 256-job prune kicks in. Trim to
			// 2000 lines with a head marker so the diagnostic context is
			// preserved while bounding memory.
			{
				static constexpr int32 LIVE_LOG_MAX_LINES = 2000;
				if (JobPtr->LiveLogLines.Num() > LIVE_LOG_MAX_LINES)
				{
					const int32 ToDrop = JobPtr->LiveLogLines.Num() - LIVE_LOG_MAX_LINES + 1;
					JobPtr->LiveLogLines.RemoveAt(0, ToDrop);
					JobPtr->LiveLogLines.Insert(
						FString::Printf(TEXT("[…truncated %d earlier lines…]"), ToDrop),
						0);
				}
			}

			++JobPtr->CurrentStep;
			JobPtr->LastUpdateTimeSec = FPlatformTime::Seconds(); // FIX-1/FIX-2: 进度锚点

			// Audit round 9 (group D - phase 1 priority queue):
			//   只要本步执行后 Job 仍是 running（无论是「还有更多 step」还是「step 跑完但
			//   状态翻转 happens 在下次 Tick 的 IsValidIndex 分支」），就 push 回原优先级
			//   队列尾，让出执行权给同级别其他 Job。失败/取消/成功不再 enqueue，自然结束。
			//   注意用 JobPtr->Priority（理论上等于 OriginalPriority，但走 JobPtr 是
			//   round-2 安全规则——禁用 ExecuteTool 之前捕获的引用）。
			if (JobPtr->Status == TEXT("running"))
			{
				EnqueueJobByPriority(JobId, JobPtr->Priority);
			}
			(void)OriginalPriority; // 仅用于日后调试，避免编译器 unused 警告
		}

		// FIX-1 (job queue conn design 20260804): 生命周期看门狗清扫（节流）。
		//   放在帧预算循环之后：即使队列为空提前 break，看门狗依然每帧有机会运行，
		//   保证无新作业时 blocked/等锁/外部租约超时也能被回收。
		SweepJobLifecycle(FPlatformTime::Seconds());
	}

	bool FSololmcpJobService::SubmitJob(const TSharedRef<FJsonObject>& Params, TSharedRef<FJsonObject>& OutResult, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* StepsPtr = nullptr;
		if (!Params->TryGetArrayField(TEXT("steps"), StepsPtr) || !StepsPtr || StepsPtr->Num() == 0)
		{
			OutError = TEXT("Missing steps.");
			return false;
		}
		// audit-U6 fix (P2): bound steps[] so a malformed/malicious payload
		// can't allocate gigabytes of FSololmcpJobStep. The DAG planner
		// already keeps batches well below 64; this is a defensive ceiling.
		static constexpr int32 MAX_STEPS_PER_JOB = 64;
		if (StepsPtr->Num() > MAX_STEPS_PER_JOB)
		{
			OutError = FString::Printf(TEXT("steps[] too long (%d > %d limit)"),
				StepsPtr->Num(), MAX_STEPS_PER_JOB);
			return false;
		}

	FString ClientRequestId;
	Params->TryGetStringField(TEXT("client_request_id"), ClientRequestId);
	if (!ClientRequestId.IsEmpty())
	{
		if (const FString* ExistingJobId = GRequestIdToJobId.Find(ClientRequestId))
		{
			if (FSololmcpJobState* ExistingJob = GJobs.Find(*ExistingJobId))
			{
				// 仅对仍在进行中（queued/running）或已成功的 Job 做去重。
				// 对已失败/已取消的 Job 不做去重：让客户端重新提交一个新 Job。
				// 否则客户端重启后 counter 从 1 重置，会命中上一个进程遗留的失败 Job，
				// 导致 await 立即返回旧错误（如 "Unknown tool: get_ue_status"）。
				const bool bTerminal =
					ExistingJob->Status == TEXT("failed") ||
					ExistingJob->Status == TEXT("cancelled");
				if (!bTerminal)
				{
					OutResult = MakeShared<FJsonObject>();
					OutResult->SetStringField(TEXT("job_id"), ExistingJob->JobId);
					EnrichCorrelationPayload(*ExistingJob, OutResult);
					OutResult->SetBoolField(TEXT("deduplicated"), true);
					return true;
				}
				// Terminal job: fall through to create a fresh job with a new ID.
				// Clean up the stale mapping so the new job can register under the same key.
				GRequestIdToJobId.Remove(ClientRequestId);
			}
		}
	}

		PruneOldJobsIfNeeded();

	FSololmcpJobState Job;
	Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Job.ClientRequestId = ClientRequestId;
	Job.SubmitTimeSec = FPlatformTime::Seconds(); // FIXED #8: 记录提交时间
	Job.LastUpdateTimeSec = Job.SubmitTimeSec; // FIX-1/FIX-2: 进度锚点初值
	Params->TryGetStringField(TEXT("trace_id"), Job.TraceId);
	Params->TryGetStringField(TEXT("plan_label"), Job.PlanLabel);
	// GeoTerrain contract envelope fields are validated before queue insertion.
	{
		double MaxRuntime = 0.0;
		if (Params->TryGetNumberField(TEXT("maximum_runtime_ms"), MaxRuntime))
		{
			if (MaxRuntime < 1.0 || MaxRuntime > 86400000.0 || FMath::FloorToDouble(MaxRuntime) != MaxRuntime)
			{
				OutError = FString::Printf(TEXT("maximum_runtime_ms %.0f out of range 1..86400000."), MaxRuntime);
				return false;
			}
			Job.MaximumRuntimeMs = static_cast<int64>(MaxRuntime);
		}
		FString CheckpointPolicyValue;
		if (Params->TryGetStringField(TEXT("checkpoint_policy"), CheckpointPolicyValue) && !CheckpointPolicyValue.IsEmpty())
		{
			static const TSet<FString> Allowed = { TEXT("none"), TEXT("stage"), TEXT("page_batch") };
			if (!Allowed.Contains(CheckpointPolicyValue))
			{
				OutError = FString::Printf(TEXT("checkpoint_policy '%s' not in none|stage|page_batch."), *CheckpointPolicyValue);
				return false;
			}
			Job.CheckpointPolicy = CheckpointPolicyValue;
		}
		FString EventDetailValue;
		if (Params->TryGetStringField(TEXT("event_detail"), EventDetailValue) && !EventDetailValue.IsEmpty())
		{
			static const TSet<FString> Allowed = { TEXT("summary"), TEXT("page"), TEXT("finding") };
			if (!Allowed.Contains(EventDetailValue))
			{
				OutError = FString::Printf(TEXT("event_detail '%s' not in summary|page|finding."), *EventDetailValue);
				return false;
			}
			Job.EventDetail = EventDetailValue;
		}
	}

	ParseExplicitResourceLocks(Params, Job.ResourceLocks);

		for (const TSharedPtr<FJsonValue>& StepValue : *StepsPtr)
		{
			const TSharedPtr<FJsonObject> StepObj = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
			if (!StepObj.IsValid())
			{
				OutError = TEXT("Each step must be an object.");
				return false;
			}

			FSololmcpJobStep Step;
			const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
			StepObj->TryGetStringField(TEXT("label"), Step.Label);
			if (!StepObj->TryGetStringField(TEXT("tool"), Step.Tool) || Step.Tool.IsEmpty() ||
				!StepObj->TryGetObjectField(TEXT("arguments"), ArgsPtr) || !ArgsPtr || !ArgsPtr->IsValid())
			{
				OutError = TEXT("Each step requires tool and arguments.");
				return false;
			}
			Step.Arguments = ArgsPtr->ToSharedRef();
			Job.Steps.Add(Step);
			ParseExplicitResourceLocks(StepObj.ToSharedRef(), Job.ResourceLocks);
		}

		if (Job.ResourceLocks.Num() > 0)
		{
			Job.ResourceLockSource = TEXT("explicit");
		}
		else
		{
			Job.ResourceLocks = InferJobResourceLocksFromSteps(Job.Steps);
			Job.ResourceLockSource = Job.ResourceLocks.Num() > 0 ? TEXT("auto") : TEXT("none");
		}

		// FIX-3 (job queue conn design 20260804): 提交期冲突检测 —— 若所需锁在提交时
		//   已被占用，立即记录等待锚点与冲突信息，锁等待 TTL 从提交时刻开始计，
		//   不必等 TickJobs 第一次 block 事件；作业仍保持 queued 正常排队。
		if (Job.ResourceLocks.Num() > 0)
		{
			FString ContendLockId;
			FString ContendHolderJobId;
			FString ContendReason;
			if (DetectJobLockContention(Job, ContendLockId, ContendHolderJobId, ContendReason))
			{
				Job.LockWaitingSinceSec = FPlatformTime::Seconds();
				Job.BlockedLockId = ContendLockId;
				Job.BlockedByJobId = ContendHolderJobId;
				Job.BlockedReason = ContendReason;
			}
		}

		// Audit round 9 (group D - phase 1 priority queue):
		//   解析调度优先级 —— 显式 params._priority 覆盖 > 第一个 step 工具名推断 > 缺省 Mid。
		//   只看第一个 step（多 step plan 的「整体性质」由开头工具决定，足够实用）。
		double ContractPriorityValue = 0.0;
		const bool bHasContractPriority = Params->TryGetNumberField(TEXT("priority"), ContractPriorityValue);
		if (bHasContractPriority &&
			(ContractPriorityValue < -20.0 || ContractPriorityValue > 20.0 || FMath::FloorToDouble(ContractPriorityValue) != ContractPriorityValue))
		{
			OutError = FString::Printf(TEXT("priority %.0f out of range -20..20."), ContractPriorityValue);
			return false;
		}
		EJobPriority Priority = EJobPriority::Mid;
		FString ExplicitPriorityStr;
		if (bHasContractPriority)
		{
			const int32 V = static_cast<int32>(ContractPriorityValue);
			Job.ContractPriority = V;
			Priority = (V >= 11) ? EJobPriority::Urgent : (V >= 1) ? EJobPriority::High : (V >= -10) ? EJobPriority::Mid : EJobPriority::Low;
		}
		else
		{
			const bool bHasCompatPriority = Params->TryGetStringField(TEXT("priority"), ExplicitPriorityStr);
			if ((Params->TryGetStringField(TEXT("_priority"), ExplicitPriorityStr) || bHasCompatPriority) &&
				ParseExplicitPriority(ExplicitPriorityStr, Priority))
			{
				// 显式覆盖生效，不再推断。
			}
			else if (Job.Steps.Num() > 0)
			{
				Priority = InferPriorityFromToolName(Job.Steps[0].Tool);
			}
		}
		Job.Priority = Priority;

		AddJobEvent(Job, TEXT("status"), TEXT("Job queued."));
		FString TargetGuardCode;
		FString TargetGuardMessage;
		if (!ValidateTargetGuardForJob(Job, Params, TargetGuardCode, TargetGuardMessage))
		{
			Job.Status = TEXT("blocked");
			Job.BlockedSinceSec = FPlatformTime::Seconds(); // FIX-1: blocked TTL 锚点
			Job.ErrorCode = TargetGuardCode;
			Job.ErrorMessage = TargetGuardMessage;
			TSharedRef<FJsonObject> BlockPayload = MakeShared<FJsonObject>();
			BlockPayload->SetStringField(TEXT("code"), TargetGuardCode);
			BlockPayload->SetStringField(TEXT("message"), TargetGuardMessage);
			BlockPayload->SetObjectField(TEXT("target_guard"), TargetGuardToJson(Job));
			BlockPayload->SetArrayField(TEXT("resource_locks"), ResourceLocksToJson(Job.ResourceLocks));
			AddJobEvent(Job, TEXT("blocked"), TargetGuardMessage, BlockPayload);

			const FString BlockedJobId = Job.JobId;
			GSubmittedJobs.Increment();
			GJobs.Add(BlockedJobId, MoveTemp(Job));
			if (!ClientRequestId.IsEmpty())
			{
				GRequestIdToJobId.Add(ClientRequestId, BlockedJobId);
			}

			OutResult = MakeShared<FJsonObject>();
			OutResult->SetStringField(TEXT("job_id"), BlockedJobId);
			EnrichCorrelationPayload(GJobs[BlockedJobId], OutResult);
			OutResult->SetBoolField(TEXT("deduplicated"), false);
			OutResult->SetStringField(TEXT("status"), TEXT("blocked"));
			OutResult->SetStringField(TEXT("client_status"), TEXT("blocked"));
			OutResult->SetStringField(TEXT("error_code"), TargetGuardCode);
			OutResult->SetStringField(TEXT("error_message"), TargetGuardMessage);
			OutResult->SetObjectField(TEXT("target_guard"), TargetGuardToJson(GJobs[BlockedJobId]));
			OutResult->SetObjectField(TEXT("receipt_envelope"), BuildReceiptEnvelope(GJobs[BlockedJobId]));
			return true;
		}
		const FString NewJobId = Job.JobId;
		// Phase 3A (worker thread pool): single-step worker-safe jobs bypass the
		// GameThread queue entirely. Multi-step plans must stay on GameThread
		// because step-N arguments may template-reference step-(N-1) results,
		// which requires the FSololmcpJobState state-machine that TickJobs drives.
		const bool bWorkerSafeSingleStep =
			Job.Steps.Num() == 1 && IsWorkerSafeTool(Job.Steps[0].Tool) && Job.ResourceLocks.Num() == 0;
		GSubmittedJobs.Increment();
		if (bWorkerSafeSingleStep)
		{
			GWorkerSafeSingleStepSubmissions.Increment();
		}
		// Snapshot what we need on the worker BEFORE MoveTemp transfers Job.
		FSololmcpJobStep WorkerStepSnapshot;
		if (bWorkerSafeSingleStep)
		{
			WorkerStepSnapshot = Job.Steps[0];
		}
		GJobs.Add(NewJobId, MoveTemp(Job));
		if (!ClientRequestId.IsEmpty())
		{
			GRequestIdToJobId.Add(ClientRequestId, NewJobId);
		}

		if (bWorkerSafeSingleStep)
		{
			// Phase 3A/E: also need a live registry pointer (set by TickJobs on first
			// run). If TickJobs hasn't run yet OR throttle saturated → GameThread queue.
			FSololmcpToolRegistry* RegistryPtr = GMcpRegistry;
			if (!RegistryPtr || GThreadInFlightWorker.GetValue() >= GMaxConcurrentMcpJobsWorker)
			{
				GWorkerFallbackToGameThread.Increment();
				EnqueueJobByPriority(NewJobId, Priority);
			}
			else
			{
				GThreadInFlightWorker.Increment();
				// Mark running early so a polling client doesn't see "queued" forever
				// if the worker hasn't started yet.
				if (FSololmcpJobState* JobNow = GJobs.Find(NewJobId))
				{
					JobNow->Status = TEXT("running");
					// FIX-1: runtime 看门狗锚点（worker 快路径不经 TickJobs 的 queued→running 分支）
					JobNow->FirstRunningTimeSec = FPlatformTime::Seconds();
					JobNow->LastUpdateTimeSec = JobNow->FirstRunningTimeSec;
					AddJobEvent(*JobNow, TEXT("status"), TEXT("Job started (worker thread)."));
					AddJobEvent(*JobNow, TEXT("running"), TEXT("Job started (worker thread)."));
				}

				// Registry lifetime is module-scope, set by Router.
				// audit-U1 fix (P1): the previous version captured RegistryPtr
				// raw and dereferenced unconditionally on the worker thread.
				// If the module was unloaded (plugin reload / shutdown / PIE
				// edge case) between capture and use, `*RegistryPtr` UAFs.
				// SEH guard catches the access violation and produces STEP_FAILED
				// — but we can do better: (a) defensive null-check on the worker
				// avoids touching freed memory in the common case, (b) the
				// GameThread post-flight handler (below) re-checks `GMcpRegistry`
				// equality and rejects results from a stale registry generation.
				AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
					[NewJobId, WorkerStepSnapshot, RegistryPtr]()
				{
					const double StepStart = FPlatformTime::Seconds();
					TSharedRef<FJsonObject> Structured = MakeShared<FJsonObject>();
					FString Summary;
					FString Error;
					bool bSuccess = false;
					if (RegistryPtr == nullptr)
					{
						Error = TEXT("registry_unavailable_worker_started_post_shutdown");
					}
					else
					{
						// Phase 3F: SEH-isolated worker-thread tool call.
						bSuccess = ExecuteToolWithSehGuard(
							*RegistryPtr,
							WorkerStepSnapshot.Tool,
							WorkerStepSnapshot.Arguments,
							Structured,
							Summary,
							Error);
					}
					const double DurationMs = (FPlatformTime::Seconds() - StepStart) * 1000.0;

					// Marshal result back to GameThread to mutate Job state safely
					// (avoids mutex-vs-rehash hazard on GJobs from worker thread).
					// audit-U1: capture WorkerRegistry so the GameThread side can
					// detect a registry swap (plugin reload) between submit and
					// result delivery and discard the result rather than commit it.
					AsyncTask(ENamedThreads::GameThread,
						[NewJobId, WorkerTool = WorkerStepSnapshot.Tool,
						 WorkerLabel = WorkerStepSnapshot.Label,
						 WorkerArgs = WorkerStepSnapshot.Arguments,
						 WorkerRegistry = RegistryPtr,
						 Structured, Summary, Error, DurationMs, bSuccess]()
					{
						FSololmcpJobState* JobPtr = GJobs.Find(NewJobId);
						GThreadInFlightWorker.Decrement();
						if (!JobPtr) { return; } // cancelled / pruned
						// If the client cancelled while we ran, respect it.
						if (JobPtr->Status == TEXT("cancelled")) { return; }
						// audit-U1: registry was swapped under us (module reload
						// while job was in flight) — the worker's structured
						// output may reference now-freed UObjects. Fail clean.
						if (GMcpRegistry != WorkerRegistry)
						{
							UE_LOG(LogTemp, Warning,
								TEXT("[SOMOLMCP] discarding job %s result: registry swapped during worker run"),
								*NewJobId);
							JobPtr->Status = TEXT("failed");
							AddJobEvent(*JobPtr, TEXT("error"),
								TEXT("registry_swapped_during_worker_run"));
							return;
						}

						TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
						StepResult->SetNumberField(TEXT("step_index"), JobPtr->CurrentStep);
						StepResult->SetStringField(TEXT("tool"), WorkerTool);
						if (!WorkerLabel.IsEmpty())
						{
							StepResult->SetStringField(TEXT("label"), WorkerLabel);
						}
						StepResult->SetObjectField(TEXT("arguments"), WorkerArgs);
						StepResult->SetBoolField(TEXT("ok"), bSuccess);
						StepResult->SetStringField(TEXT("summary"), bSuccess ? Summary : Error);
						StepResult->SetNumberField(TEXT("duration_ms"), DurationMs);
						StepResult->SetObjectField(TEXT("result"), Structured);
						if (!bSuccess)
						{
							TSharedRef<FJsonObject> StepError = MakeShared<FJsonObject>();
							StepError->SetStringField(TEXT("code"), TEXT("STEP_FAILED"));
							StepError->SetStringField(TEXT("message"), Error);
							StepError->SetBoolField(TEXT("retryable"), true);
							StepResult->SetObjectField(TEXT("error"), StepError);
						}
						JobPtr->StepResults.Add(MakeShared<FJsonValueObject>(StepResult));
						++JobPtr->CurrentStep;

						if (bSuccess)
						{
							JobPtr->Status = TEXT("succeeded");
							AppendTerminalOutcome(false);
							AddJobEvent(*JobPtr, TEXT("status"), TEXT("Job finished successfully (worker)."));
							AddJobEvent(*JobPtr, TEXT("completed"), TEXT("Job finished successfully (worker)."));
						}
						else
						{
							JobPtr->Status = TEXT("failed");
							AppendTerminalOutcome(true);
							JobPtr->ErrorCode = TEXT("STEP_FAILED");
							JobPtr->ErrorMessage = Error;
							TSharedRef<FJsonObject> ErrPay = MakeShared<FJsonObject>();
							ErrPay->SetStringField(TEXT("tool"), WorkerTool);
							AddJobEvent(*JobPtr, TEXT("error"), Error, ErrPay);
							AddJobEvent(*JobPtr, TEXT("failed"), Error, ErrPay);
						}
					});
				});
				UE_LOG(LogTemp, Verbose, TEXT("[phase3a] dispatched worker-safe tool='%s' job=%s"),
					*WorkerStepSnapshot.Tool, *NewJobId);
			}
		}
		else
		{
			// Audit round 9 (group D - phase 1 priority queue):
			//   Submit 后立即入对应优先级队列。TickJobs 会按 WRR 弹出推进。
			EnqueueJobByPriority(NewJobId, Priority);
		}

		OutResult = MakeShared<FJsonObject>();
		OutResult->SetStringField(TEXT("job_id"), NewJobId);
		EnrichCorrelationPayload(GJobs[NewJobId], OutResult);
		OutResult->SetBoolField(TEXT("deduplicated"), false);
		OutResult->SetArrayField(TEXT("resource_locks"), ResourceLocksToJson(GJobs[NewJobId].ResourceLocks));
		OutResult->SetStringField(TEXT("resource_lock_source"), GJobs[NewJobId].ResourceLockSource);
		// Audit round 9 (group D - phase 1 priority queue):
		//   把分配的优先级回传给客户端，方便 plan 调试 / log 关联。
		{
			const TCHAR* PrioStr = TEXT("mid");
			switch (Priority)
			{
			case EJobPriority::Urgent: PrioStr = TEXT("urgent"); break;
			case EJobPriority::High:   PrioStr = TEXT("high");   break;
			case EJobPriority::Mid:    PrioStr = TEXT("mid");    break;
			case EJobPriority::Low:    PrioStr = TEXT("low");    break;
			}
			OutResult->SetStringField(TEXT("priority"), PrioStr);
		}
		if (!GJobs[NewJobId].TraceId.IsEmpty())
		{
			OutResult->SetStringField(TEXT("trace_id"), GJobs[NewJobId].TraceId);
		}
		return true;
	}

	bool FSololmcpJobService::PublishCompletedExternalJob(
		const FString& ClientRequestId,
		const FString& PlanLabel,
		const bool bSucceeded,
		const FString& PayloadJson,
		const int64 OutputRevision,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		FString& OutJobId,
		bool& bOutDeduplicated,
		FString& OutError)
	{
		FScopeLock Guard(&GJobsMutex);
		OutJobId.Reset();
		bOutDeduplicated = false;
		if (ClientRequestId.IsEmpty() || PlanLabel.IsEmpty() || PayloadJson.IsEmpty() || OutputRevision < 0)
		{
			OutError = TEXT("external job requires client_request_id, plan_label, payload and non-negative revision");
			return false;
		}
		if (const FString* ExistingId = GRequestIdToJobId.Find(ClientRequestId))
		{
			if (const FSololmcpJobState* Existing = GJobs.Find(*ExistingId);
				Existing && Existing->PlanLabel == PlanLabel && Existing->Status == TEXT("succeeded"))
			{
				OutJobId = *ExistingId;
				bOutDeduplicated = true;
				return true;
			}
		}
		PruneOldJobsIfNeeded();
		FSololmcpJobState Job;
		Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Job.ClientRequestId = ClientRequestId;
		Job.PlanLabel = PlanLabel;
		Job.SubmitTimeSec = FPlatformTime::Seconds();
		Job.Status = bSucceeded ? TEXT("succeeded") : TEXT("failed");
		Job.ErrorCode = bSucceeded ? FString() : ErrorCode;
		Job.ErrorMessage = bSucceeded ? FString() : ErrorMessage;
		Job.ExternalResultJson = PayloadJson;
		Job.ExternalOutputRevision = OutputRevision;
		AddJobEvent(Job, TEXT("status"), TEXT("External provider job admitted by the existing Job Runtime."));
		TSharedRef<FJsonObject> EventPayload = MakeShared<FJsonObject>();
		EventPayload->SetStringField(TEXT("plan_label"), PlanLabel);
		EventPayload->SetNumberField(TEXT("output_revision"), OutputRevision);
		AddJobEvent(Job, bSucceeded ? TEXT("completed") : TEXT("failed"),
			bSucceeded ? TEXT("External provider job completed.") : ErrorMessage, EventPayload);
		OutJobId = Job.JobId;
		GSubmittedJobs.Increment();
		GJobs.Add(Job.JobId, MoveTemp(Job));
		GRequestIdToJobId.Add(ClientRequestId, OutJobId);
		return true;
	}

	bool FSololmcpJobService::CreateExternalJob(
		const FString& ClientRequestId,
		const FString& PlanLabel,
		const FString& InitialPayloadJson,
		const TArray<FString>& ExclusiveResourceLockIds,
		FString& OutJobId,
		bool& bOutDeduplicated,
		FString& OutError)
	{
		FScopeLock Guard(&GJobsMutex);
		OutJobId.Reset();
		bOutDeduplicated = false;
		if (ClientRequestId.IsEmpty() || PlanLabel.IsEmpty() || InitialPayloadJson.IsEmpty())
		{
			OutError = TEXT("external job requires client_request_id, plan_label and initial payload");
			return false;
		}
		if (const FString* ExistingId = GRequestIdToJobId.Find(ClientRequestId))
		{
			if (const FSololmcpJobState* Existing = GJobs.Find(*ExistingId); Existing && Existing->PlanLabel == PlanLabel)
			{
				OutJobId = *ExistingId;
				bOutDeduplicated = true;
				return true;
			}
		}
		PruneOldJobsIfNeeded();
		FSololmcpJobState Job;
		Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Job.ClientRequestId = ClientRequestId;
		Job.PlanLabel = PlanLabel;
		Job.SubmitTimeSec = FPlatformTime::Seconds();
		Job.Status = TEXT("running");
		// FIX-2 (job queue conn design 20260804): 外部执行器作业租约锚点。
		//   无 progress/heartbeat 更新超过租约后由生命周期看门狗置 failed 并释放锁。
		Job.bExternal = true;
		Job.FirstRunningTimeSec = Job.SubmitTimeSec;
		Job.LastUpdateTimeSec = Job.SubmitTimeSec;
		Job.ExternalLeaseSec = FMath::Max(1.0, static_cast<double>(CVarJobsExternalStaleSec.GetValueOnGameThread()));
		Job.ExternalResultJson = InitialPayloadJson;
		Job.ExternalProgress = 0.0;
		Job.ResourceLockSource = ExclusiveResourceLockIds.IsEmpty() ? TEXT("none") : TEXT("external_explicit");
		for (const FString& LockId : ExclusiveResourceLockIds)
		{
			AddJobResourceLock(Job.ResourceLocks, LockId, TEXT("exclusive"), TEXT("native external executor"));
		}
		FString BlockedLockId;
		FString BlockedByJobId;
		FString BlockedReason;
		if (!TryAcquireJobResourceLocks(Job, BlockedLockId, BlockedByJobId, BlockedReason))
		{
			OutError = BlockedReason.IsEmpty() ? TEXT("external job resource lock unavailable") : BlockedReason;
			return false;
		}
		AddJobEvent(Job, TEXT("status"), TEXT("Native external executor admitted by the existing Job Runtime."));
		OutJobId = Job.JobId;
		GSubmittedJobs.Increment();
		GJobs.Add(Job.JobId, MoveTemp(Job));
		GRequestIdToJobId.Add(ClientRequestId, OutJobId);
		return true;
	}

	bool FSololmcpJobService::UpdateExternalJob(
		const FString& JobId,
		const FString& Status,
		const double Progress,
		const FString& PayloadJson,
		const int64 OutputRevision,
		const FString& ErrorCode,
		const FString& ErrorMessage,
		FString& OutError)
	{
		FScopeLock Guard(&GJobsMutex);
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job || Job->ExternalResultJson.IsEmpty())
		{
			OutError = TEXT("external job not found");
			return false;
		}
		if (Status != TEXT("running") && Status != TEXT("succeeded") && Status != TEXT("failed") && Status != TEXT("cancelled"))
		{
			OutError = TEXT("external status must be running, succeeded, failed or cancelled");
			return false;
		}
		if (IsTerminalJobStatus(Job->Status))
		{
			return Job->Status == Status;
		}
#if SOMOLMCP_WITH_WORLDFORGE
		if (!Job->WorldForgeSchedulerJobId.IsEmpty() && IsTerminalJobStatus(Status))
		{
			UWorldForgeCouplingRuntimeSubsystem* Runtime = GEngine
				? GEngine->GetEngineSubsystem<UWorldForgeCouplingRuntimeSubsystem>()
				: nullptr;
			if (!Runtime)
			{
				OutError = TEXT("E_WORLDFORGE_RUNTIME_UNAVAILABLE");
				return false;
			}
			FString SchedulerError;
			bool bPropagated = false;
			if (Status == TEXT("succeeded"))
			{
				bPropagated = Runtime->GetLayerScheduler().Complete(
					Job->WorldForgeSchedulerJobId,
					FPlatformTime::Seconds(),
					SchedulerError);
			}
			else if (Status == TEXT("failed"))
			{
				bPropagated = Runtime->GetLayerScheduler().Fail(
					Job->WorldForgeSchedulerJobId, SchedulerError);
			}
			else
			{
				FWorldForgeJobCorrelation Ignored;
				bPropagated = Runtime->GetLayerScheduler().CancelByExternalJobId(
					JobId, Ignored, SchedulerError);
			}
			if (!bPropagated)
			{
				OutError = FString::Printf(
					TEXT("E_WORLDFORGE_STATE_PROPAGATION_FAILED:%s"),
					*SchedulerError);
				return false;
			}
		}
#endif
		Job->Status = Status;
		Job->LastUpdateTimeSec = FPlatformTime::Seconds(); // FIX-2: 每次有效更新都续租
		Job->ExternalProgress = FMath::Clamp(Progress, 0.0, 1.0);
		Job->ExternalResultJson = PayloadJson.IsEmpty() ? Job->ExternalResultJson : PayloadJson;
		Job->ExternalOutputRevision = FMath::Max<int64>(0, OutputRevision);
		Job->ErrorCode = ErrorCode;
		Job->ErrorMessage = ErrorMessage;
		TSharedRef<FJsonObject> EventPayload = MakeShared<FJsonObject>();
		EventPayload->SetNumberField(TEXT("progress"), Job->ExternalProgress);
		EventPayload->SetNumberField(TEXT("output_revision"), static_cast<double>(Job->ExternalOutputRevision));
		AddJobEvent(*Job, IsTerminalJobStatus(Status) ? Status : TEXT("progress"), ErrorMessage.IsEmpty() ? TEXT("External executor state updated.") : ErrorMessage, EventPayload);
		if (IsTerminalJobStatus(Status))
		{
			ReleaseJobResourceLocks(*Job);
		}
		return true;
	}

	bool FSololmcpJobService::HeartbeatExternalJob(const FString& JobId, TSharedRef<FJsonObject>& OutResult, FString& OutError)
	{
		// FIX-2 (job queue conn design 20260804): 外部执行器租约续期（jobs/heartbeat）。
		//   长耗时执行器只需周期性发送心跳即可避免被看门狗判死，锁不释放。
		FScopeLock Guard(&GJobsMutex);
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job || !Job->bExternal)
		{
			OutError = TEXT("external job not found");
			return false;
		}
		if (IsTerminalJobStatus(Job->Status))
		{
			OutError = FString::Printf(TEXT("external job already terminal: %s"), *Job->Status);
			return false;
		}
		Job->LastUpdateTimeSec = FPlatformTime::Seconds();
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetBoolField(TEXT("ok"), true);
		OutResult->SetStringField(TEXT("job_id"), JobId);
		OutResult->SetNumberField(TEXT("lease_seconds"), Job->ExternalLeaseSec);
		return true;
	}

#if WITH_DEV_AUTOMATION_TESTS
	// FIX-1/FIX-2/FIX-3 (job queue conn design 20260804): NX11-NX14 自动化测试钩子。
	//   绕过 TickJobs 节流与墙钟依赖，用确定性状态驱动生命周期看门狗。
	bool FSololmcpJobService::TestMarkJobRunning(const FString& JobId)
	{
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job || IsTerminalJobStatus(Job->Status))
		{
			return false;
		}
		Job->Status = TEXT("running");
		// 锚点回拨到超出默认 maximum_runtime_ms（1h）之前，令下一次清扫必然判超时
		const double LimitSec = static_cast<double>(Job->MaximumRuntimeMs) / 1000.0;
		Job->FirstRunningTimeSec = FPlatformTime::Seconds() - (LimitSec + 60.0);
		Job->LastUpdateTimeSec = Job->FirstRunningTimeSec;
		return true;
	}

	bool FSololmcpJobService::TestSetExternalLeaseSec(const FString& JobId, const double LeaseSec)
	{
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job || !Job->bExternal || IsTerminalJobStatus(Job->Status))
		{
			return false;
		}
		Job->ExternalLeaseSec = LeaseSec;
		return true;
	}

	void FSololmcpJobService::TestRunLifecycleSweep(const double NowSec)
	{
		SweepJobLifecycleInternal(NowSec);
	}
#endif

	bool FSololmcpJobService::GetExternalJobResult(
		const FString& JobId,
		bool& bOutSucceeded,
		FString& OutPayloadJson,
		int64& OutRevision,
		FString& OutStatus,
		FString& OutErrorCode)
	{
		FScopeLock Guard(&GJobsMutex);
		const FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job || Job->ExternalResultJson.IsEmpty())
		{
			OutErrorCode = TEXT("E_JOB_NOT_FOUND");
			return false;
		}
		OutStatus = Job->Status;
		bOutSucceeded = Job->Status == TEXT("succeeded");
		OutPayloadJson = Job->ExternalResultJson;
		OutRevision = Job->ExternalOutputRevision;
		OutErrorCode = Job->ErrorCode;
		return bOutSucceeded;
	}

	bool FSololmcpJobService::FindExternalJobByClientRequestId(
		const FString& ClientRequestId,
		FString& OutJobId,
		FString& OutPayloadJson,
		int64& OutRevision)
	{
		FScopeLock Guard(&GJobsMutex);
		const FString* JobId = GRequestIdToJobId.Find(ClientRequestId);
		const FSololmcpJobState* Job = JobId ? GJobs.Find(*JobId) : nullptr;
		if (!Job || Job->Status != TEXT("succeeded") || Job->ExternalResultJson.IsEmpty())
		{
			return false;
		}
		OutJobId = *JobId;
		OutPayloadJson = Job->ExternalResultJson;
		OutRevision = Job->ExternalOutputRevision;
		return true;
	}

	bool FSololmcpJobService::BindWorldForgeSchedulerJob(
		const FString& JobId,
		const FString& SchedulerJobId,
		const FString& DomainJobHandle,
		const FString& StageCancellationToken,
		const FString& CheckpointRef,
		FString& OutError)
	{
#if SOMOLMCP_WITH_WORLDFORGE
		if (!GEngine)
		{
			OutError = TEXT("E_WORLDFORGE_RUNTIME_UNAVAILABLE");
			return false;
		}
		UWorldForgeCouplingRuntimeSubsystem* Runtime =
			GEngine->GetEngineSubsystem<UWorldForgeCouplingRuntimeSubsystem>();
		if (!Runtime)
		{
			OutError = TEXT("E_WORLDFORGE_RUNTIME_UNAVAILABLE");
			return false;
		}

		{
			FScopeLock Guard(&GJobsMutex);
			const FSololmcpJobState* Job = GJobs.Find(JobId);
			if (!Job || IsTerminalJobStatus(Job->Status))
			{
				OutError = TEXT("E_MCP_JOB_NOT_BINDABLE");
				return false;
			}
			if (!Job->WorldForgeSchedulerJobId.IsEmpty())
			{
				const bool bSame =
					Job->WorldForgeSchedulerJobId == SchedulerJobId &&
					Job->WorldForgeDomainJobHandle == DomainJobHandle &&
					Job->WorldForgeStageCancellationToken == StageCancellationToken;
				OutError = bSame ? FString() : TEXT("E_JOB_CORRELATION_IMMUTABLE");
				return bSame;
			}
		}

		FWorldForgeJobCorrelation Correlation;
		Correlation.ExternalJobId = JobId;
		Correlation.SchedulerJobId = SchedulerJobId;
		Correlation.DomainJobHandle = DomainJobHandle;
		Correlation.StageCancellationToken = StageCancellationToken;
		Correlation.CheckpointRef = CheckpointRef;
		if (!Runtime->GetLayerScheduler().BindCorrelation(
				SchedulerJobId, Correlation, OutError))
		{
			return false;
		}
		if (!Runtime->GetLayerScheduler().Start(SchedulerJobId, OutError))
		{
			return false;
		}

		FScopeLock Guard(&GJobsMutex);
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job || IsTerminalJobStatus(Job->Status))
		{
			OutError = TEXT("E_MCP_JOB_NOT_BINDABLE");
			return false;
		}
		Job->WorldForgeSchedulerJobId = SchedulerJobId;
		Job->WorldForgeDomainJobHandle = DomainJobHandle;
		Job->WorldForgeStageCancellationToken = StageCancellationToken;
		Job->WorldForgeCheckpointRef = CheckpointRef;
		TSharedRef<FJsonObject> EventPayload = MakeShared<FJsonObject>();
		EventPayload->SetStringField(TEXT("scheduler_job_id"), SchedulerJobId);
		EventPayload->SetStringField(TEXT("domain_job_handle"), DomainJobHandle);
		EventPayload->SetStringField(TEXT("stage_cancellation_token"), StageCancellationToken);
		AddJobEvent(*Job, TEXT("worldforge_job_bound"),
			TEXT("MCP job bound to existing WorldForge scheduler stage."), EventPayload);
		OutError.Reset();
		return true;
#else
		OutError = TEXT("E_WORLDFORGE_RUNTIME_UNAVAILABLE");
		return false;
#endif
	}

	bool FSololmcpJobService::CreateBlockedElicitationJob(
		const FString& RequestId,
		const FString& Reason,
		TSharedRef<FJsonObject>& OutResult,
		FString& OutError)
	{
		if (RequestId.IsEmpty())
		{
			OutError = TEXT("Missing request_id.");
			return false;
		}

		PruneOldJobsIfNeeded();

		FSololmcpJobState Job;
		Job.JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Job.SubmitTimeSec = FPlatformTime::Seconds();
		Job.PlanLabel = TEXT("elicitation_wait");
		Job.Status = TEXT("blocked");
		Job.BlockedSinceSec = FPlatformTime::Seconds(); // FIX-1: blocked TTL 锚点
		Job.ErrorCode = TEXT("WAITING_FOR_ELICITATION");
		Job.ErrorMessage = Reason.IsEmpty() ? TEXT("Waiting for client elicitation response.") : Reason;
		Job.WaitingRequestId = RequestId;
		Job.WaitingMethod = TEXT("elicitation/create");
		Job.WaitingReason = Job.ErrorMessage;
		Job.bWaitingForElicitation = true;
		Job.bSyntheticElicitationJob = true;

		AddJobEvent(Job, TEXT("status"), TEXT("Synthetic elicitation job created."));
		TSharedRef<FJsonObject> BlockPayload = MakeShared<FJsonObject>();
		BlockPayload->SetStringField(TEXT("request_id"), RequestId);
		BlockPayload->SetStringField(TEXT("method"), Job.WaitingMethod);
		BlockPayload->SetStringField(TEXT("reason"), Job.WaitingReason);
		BlockPayload->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
		AddJobEvent(Job, TEXT("blocked"), Job.WaitingReason, BlockPayload);

		const FString JobId = Job.JobId;
		GSubmittedJobs.Increment();
		GJobs.Add(JobId, MoveTemp(Job));

		OutResult = JobToJson(GJobs[JobId]);
		OutResult->SetStringField(TEXT("request_id"), RequestId);
		OutResult->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
		return true;
	}

	bool FSololmcpJobService::BlockJobForElicitation(
		const FString& JobId,
		const FString& RequestId,
		const FString& Reason,
		TSharedRef<FJsonObject>& OutResult,
		FString& OutError)
	{
		if (JobId.IsEmpty())
		{
			OutError = TEXT("Missing job_id.");
			return false;
		}
		if (RequestId.IsEmpty())
		{
			OutError = TEXT("Missing request_id.");
			return false;
		}

		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job)
		{
			OutError = TEXT("Job not found");
			return false;
		}
		if (IsTerminalJobStatus(Job->Status))
		{
			OutError = FString::Printf(TEXT("Cannot block terminal job: %s"), *Job->Status);
			return false;
		}

		ReleaseJobResourceLocks(*Job);
		RemoveJobIdFromAllQueues(JobId);
		Job->Status = TEXT("blocked");
		Job->BlockedSinceSec = FPlatformTime::Seconds(); // FIX-1: blocked TTL 锚点
		Job->ErrorCode = TEXT("WAITING_FOR_ELICITATION");
		Job->ErrorMessage = Reason.IsEmpty() ? TEXT("Waiting for client elicitation response.") : Reason;
		Job->WaitingRequestId = RequestId;
		Job->WaitingMethod = TEXT("elicitation/create");
		Job->WaitingReason = Job->ErrorMessage;
		Job->bWaitingForElicitation = true;
		Job->BlockedReason = Job->WaitingReason;

		TSharedRef<FJsonObject> BlockPayload = MakeShared<FJsonObject>();
		BlockPayload->SetStringField(TEXT("request_id"), RequestId);
		BlockPayload->SetStringField(TEXT("method"), Job->WaitingMethod);
		BlockPayload->SetStringField(TEXT("reason"), Job->WaitingReason);
		BlockPayload->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
		AddJobEvent(*Job, TEXT("blocked"), Job->WaitingReason, BlockPayload);

		OutResult = JobToJson(*Job);
		OutResult->SetStringField(TEXT("request_id"), RequestId);
		OutResult->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
		return true;
	}

	bool FSololmcpJobService::ResumeJobWithElicitation(
		const FString& JobId,
		const FString& RequestId,
		const TSharedPtr<FJsonObject>& Response,
		TSharedRef<FJsonObject>& OutResult,
		FString& OutError)
	{
		if (JobId.IsEmpty())
		{
			OutError = TEXT("Missing job_id.");
			return false;
		}

		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job)
		{
			OutError = TEXT("Job not found");
			return false;
		}
		if (!Job->bWaitingForElicitation)
		{
			OutError = TEXT("Job is not waiting for elicitation.");
			return false;
		}
		if (!RequestId.IsEmpty() && Job->WaitingRequestId != RequestId)
		{
			OutError = FString::Printf(TEXT("request_id mismatch: job waits for %s"), *Job->WaitingRequestId);
			return false;
		}
		if (Job->Status != TEXT("blocked"))
		{
			OutError = FString::Printf(TEXT("Job is not blocked: %s"), *Job->Status);
			return false;
		}

		TSharedRef<FJsonObject> ResumePayload = MakeShared<FJsonObject>();
		ResumePayload->SetStringField(TEXT("request_id"), Job->WaitingRequestId);
		ResumePayload->SetStringField(TEXT("method"), Job->WaitingMethod);
		if (Response.IsValid())
		{
			ResumePayload->SetObjectField(TEXT("response"), Response);
		}
		AddJobEvent(*Job, TEXT("elicitation_resolved"), TEXT("Client elicitation response received."), ResumePayload);

		Job->bWaitingForElicitation = false;
		Job->WaitingRequestId.Reset();
		Job->WaitingMethod.Reset();
		Job->WaitingReason.Reset();
		Job->BlockedReason.Reset();
		Job->BlockedSinceSec = 0.0; // FIX-1: 恢复后清零 blocked TTL 锚点
		Job->ErrorCode.Reset();
		Job->ErrorMessage.Reset();

		if (Job->bSyntheticElicitationJob)
		{
			Job->Status = TEXT("succeeded");
			AddJobEvent(*Job, TEXT("completed"), TEXT("Synthetic elicitation job completed."));
		}
		else
		{
			Job->Status = TEXT("queued");
			AddJobEvent(*Job, TEXT("status"), TEXT("Job resumed after elicitation."));
			EnqueueJobByPriority(JobId, Job->Priority);
		}

		OutResult = JobToJson(*Job);
		return true;
	}

	bool FSololmcpJobService::GetJob(const FString& JobId, TSharedRef<FJsonObject>& OutResult, FString& OutError)
	{
		FScopeLock Guard(&GJobsMutex);
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job)
		{
			if (const FSololmcpJobTombstone* Tombstone = GJobTombstones.Find(JobId))
			{
				OutError = FString::Printf(
					TEXT("%s: job receipt was pruned from runtime instance %s"),
					*Tombstone->Reason,
					*GJobRuntimeInstanceId);
			}
			else
			{
				OutError = FString::Printf(
					TEXT("JOB_NOT_FOUND: id is unknown to runtime instance %s"),
					*GJobRuntimeInstanceId);
			}
			return false;
		}
		OutResult = JobToJson(*Job);
		return true;
	}

	bool FSololmcpJobService::AwaitJob(FSololmcpToolRegistry& Registry, const FString& JobId, int32 TimeoutMs, TSharedRef<FJsonObject>& OutResult, FString& OutError)
	{
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job)
		{
			OutError = TEXT("Job not found");
			return false;
		}

		// FIXED #C3: 服务端等待上限收紧到 5s，单帧时间片缩到 50ms，避免 GameThread 长时间阻塞导致 UE Editor 卡帧。
		//
		// === 设计契约 ===
		// AwaitJob 不再是"等到 Job 完成"的同步原语；它是一个"短时阻塞窗口（最多 5s）+ 把控制权还给客户端"。
		// TickJobs 的真正驱动者是 SOMOLMCP 的主循环 / 客户端 polling —— 本函数内的 TickJobs 调用通常会被
		// `bTickInProgress` 重入门 no-op 掉（这是预期行为：AwaitJob 只 sleep / 让步，不抢着推进）。
		//
		// === 客户端推荐使用模式（polling pattern）===
		//   1) jobs/submit  -> 立即拿到 job_id（不要等）
		//   2) 周期性调用 jobs/get（或 jobs/await，TimeoutMs 取小值如 1000~5000）轮询直到 status==done/failed/cancelled
		//   3) 当 jobs/await 返回 still_running=true 时，**这不是错误**；客户端应继续下一轮 polling
		//
		// === 返回值约定 ===
		//   - Job 已 succeeded/failed/cancelled  -> bSuccess=true, OutResult=JobJson（无 still_running 字段）
		//   - 5s 内 Job 仍未完成                 -> bSuccess=true, OutResult.still_running=true（不是 timed_out / 不是失败）
		//   - TimeoutMs==0                       -> 立即快照返回（不进入 sleep 循环），UE polling 客户端友好
		constexpr int32 ServerMaxAwaitMs = 5000; // 5s 上限（C3 修复：从 120s 收紧）
		constexpr int32 MaxSliceMs       = 50;   // 单次最大 sleep 时间片（C3 修复：从 100ms 收紧），让 GameThread 有机会处理 input
		const int32 ClampedTimeout = FMath::Clamp(TimeoutMs, 0, ServerMaxAwaitMs);

		// TimeoutMs==0：客户端只想要一次快照，不要 sleep；直接走到末尾返回当前 Job 状态。
		// （still_running 标志在 Job 还在 queued/running 时也会被打上，让客户端知道要继续 polling。）
		const double Start = FPlatformTime::Seconds();
		while (ClampedTimeout > 0 && Job && (Job->Status == TEXT("queued") || Job->Status == TEXT("running")))
		{
			const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
			if (ElapsedMs > static_cast<double>(ClampedTimeout))
			{
				break; // 超过 5s 窗口：跳出循环，由下方统一打 still_running 标志返回
			}

			// 尝试推进 Job 一步（重入时会被 bTickInProgress no-op，这是预期行为：客户端是真正的 driver）
			TickJobs(Registry);

			// 重新查找（TickJobs 可能修改状态）
			Job = GJobs.Find(JobId);
			if (!Job)
			{
				break;
			}

			// 如果 Job 仍在运行，短暂休眠（不超过剩余时间 / 2，最多 MaxSliceMs=50ms）
			if (Job->Status == TEXT("queued") || Job->Status == TEXT("running"))
			{
				const double Remaining = static_cast<double>(ClampedTimeout) - (FPlatformTime::Seconds() - Start) * 1000.0;
				if (Remaining <= 0.0)
				{
					break;
				}
				const float SleepSec = static_cast<float>(FMath::Min(static_cast<double>(MaxSliceMs), Remaining * 0.5)) / 1000.0f;
				FPlatformProcess::Sleep(SleepSec);
				Job = GJobs.Find(JobId);
			}
		}

		if (!Job)
		{
			OutError = TEXT("Job not found");
			return false;
		}

		OutResult = JobToJson(*Job);
		// 如果窗口结束 Job 仍未到终态，标记 still_running 让客户端继续 polling（这不是错误）。
		// 注意：不再设置 timed_out=true —— "服务端等待窗口到期" 不等于 "Job 失败"。
		if (Job->Status == TEXT("queued") || Job->Status == TEXT("running") || Job->Status == TEXT("blocked"))
		{
			OutResult->SetBoolField(TEXT("still_running"), true);
			OutResult->SetStringField(TEXT("status"), Job->Status);
			OutResult->SetNumberField(TEXT("step"), Job->CurrentStep);
			OutResult->SetStringField(
				TEXT("hint"),
				Job->Status == TEXT("blocked")
					? TEXT("Job is blocked; client should inspect receipt_envelope and resume/cancel after resolving the blocker.")
					: TEXT("Job still running; client should poll jobs/get or call jobs/await again."));
		}
		return true;
	}

	// audit-U4 fix (P2): worker-visible cancellation set + helper. Existing
	// tools execute synchronously inside ExecuteToolWithSehGuard and can't
	// be interrupted mid-call, but they CAN cooperatively poll this set
	// between sub-operations (e.g. inside a long batch loop). When a tool
	// observes its job_id here it should return early with a "cancelled"
	// error; the GameThread marshal-back already discards the result. The
	// previous behaviour was that in-flight workers burned a thread-pool
	// slot all the way to natural completion — a cancel-storm of 16 long
	// jobs could exhaust the worker pool (cap=16) for the duration.
	static FCriticalSection GCancelledJobsMutex;
	static TSet<FString> GCancelledJobIds;
	static void AddCancelledJobId(const FString& JobId)
	{
		FScopeLock Lock(&GCancelledJobsMutex);
		GCancelledJobIds.Add(JobId);
	}
	// Future tools call this from worker thread between sub-operations.
	// Kept public via header in a follow-up if/when wired through.
	bool FSololmcpJobService::IsJobCancelled(const FString& JobId)
	{
		FScopeLock Lock(&GCancelledJobsMutex);
		return GCancelledJobIds.Contains(JobId);
	}

	bool FSololmcpJobService::CancelJob(const FString& JobId, TSharedRef<FJsonObject>& OutResult, FString& OutError)
	{
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job)
		{
			OutError = TEXT("Job not found");
			return false;
		}
		if (Job->Status == TEXT("queued") || Job->Status == TEXT("running") || Job->Status == TEXT("blocked"))
		{
#if SOMOLMCP_WITH_WORLDFORGE
			if (!Job->WorldForgeSchedulerJobId.IsEmpty())
			{
				UWorldForgeCouplingRuntimeSubsystem* Runtime = GEngine
					? GEngine->GetEngineSubsystem<UWorldForgeCouplingRuntimeSubsystem>()
					: nullptr;
				if (!Runtime)
				{
					OutError = TEXT("E_WORLDFORGE_RUNTIME_UNAVAILABLE");
					return false;
				}
				FWorldForgeJobCorrelation Correlation;
				FString SchedulerError;
				if (!Runtime->GetLayerScheduler().CancelByExternalJobId(
						JobId, Correlation, SchedulerError))
				{
					OutError = FString::Printf(
						TEXT("E_WORLDFORGE_CANCEL_PROPAGATION_FAILED:%s"),
						*SchedulerError);
					return false;
				}
				if (Correlation.StageCancellationToken !=
					Job->WorldForgeStageCancellationToken)
				{
					OutError = TEXT("E_WORLDFORGE_CANCEL_TOKEN_MISMATCH");
					return false;
				}
			}
#endif
			ReleaseJobResourceLocks(*Job);
			Job->Status = TEXT("cancelled");
			Job->ErrorCode = TEXT("CANCELLED");
			Job->ErrorMessage = TEXT("Cancelled by client.");
			Job->bWaitingForElicitation = false;
			AddJobEvent(*Job, TEXT("status"), TEXT("Job cancelled."));
			// audit-U4: also publish to the worker-visible cancellation set so
			// cooperatively-polling tools can short-circuit.
			AddCancelledJobId(JobId);
			// Audit round 9 (group D - phase 1 priority queue):
			//   立即从优先级队列剔除。TickJobs 在弹出时也会用 status 检查兜底，
			//   但提前清理可避免无谓的 pop+drop 浪费一个 WRR 槽位。
			RemoveJobIdFromAllQueues(JobId);
		}
		OutResult = JobToJson(*Job);
		return true;
	}

	bool FSololmcpJobService::PollEvents(
		FSololmcpToolRegistry& Registry,
		const FString& JobId,
		const int32 SinceSeq,
		const int32 WaitMs,
		TSharedRef<FJsonObject>& OutResult,
		FString& OutError)
	{
		FSololmcpJobState* Job = GJobs.Find(JobId);
		if (!Job)
		{
			OutError = TEXT("Job not found");
			return false;
		}

		const double Start = FPlatformTime::Seconds();
		while (WaitMs > 0 && Job)
		{
			MaybeAddHeartbeatEvent(*Job, TEXT("jobs_events"));
			bool bHasNewEvent = false;
			for (const FSololmcpJobEvent& Event : Job->Events)
			{
				if (Event.Seq > SinceSeq)
				{
					bHasNewEvent = true;
					break;
				}
			}
			if (bHasNewEvent || (Job->Status != TEXT("queued") && Job->Status != TEXT("running")))
			{
				break;
			}

			if ((FPlatformTime::Seconds() - Start) * 1000.0 > WaitMs)
			{
				break;
			}
			TickJobs(Registry);
			FPlatformProcess::Sleep(0.02f);
			Job = GJobs.Find(JobId);
		}

		if (!Job)
		{
			OutError = TEXT("Job not found");
			return false;
		}

		OutResult = MakeShared<FJsonObject>();
		OutResult->SetStringField(TEXT("job_id"), JobId);
		OutResult->SetArrayField(TEXT("events"), JobEventsToJson(*Job, SinceSeq));
		OutResult->SetNumberField(TEXT("next_seq"), Job->NextEventSeq);
		OutResult->SetStringField(TEXT("status"), Job->Status);
		OutResult->SetStringField(TEXT("client_status"), JobClientStatus(*Job));
		OutResult->SetNumberField(TEXT("progress"), FMath::Clamp(ComputeJobProgress(*Job), 0.0, 1.0));
		OutResult->SetObjectField(TEXT("receipt_envelope"), BuildReceiptEnvelope(*Job));
		return true;
	}

	void FSololmcpJobService::CollectProgressNotifications(
		TMap<FString, int32>& InOutLastSeqByJob,
		TArray<TSharedPtr<FJsonObject>>& OutNotifications)
	{
		for (TPair<FString, FSololmcpJobState>& Pair : GJobs)
		{
			FSololmcpJobState& Job = Pair.Value;
			const int32 LastSeen = InOutLastSeqByJob.FindRef(Job.JobId);
			const int32 LatestSeq = Job.NextEventSeq - 1;
			if (LatestSeq <= LastSeen)
			{
				continue;
			}

			const double Progress = FMath::Clamp(ComputeJobProgress(Job), 0.0, 1.0);
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("job_id"), Job.JobId);
			Payload->SetStringField(TEXT("status"), Job.Status);
			Payload->SetStringField(TEXT("client_status"), JobClientStatus(Job));
			Payload->SetBoolField(TEXT("terminal"), IsTerminalJobStatus(Job.Status));
			Payload->SetNumberField(TEXT("current_step"), Job.CurrentStep);
			Payload->SetNumberField(TEXT("total_steps"), Job.Steps.Num());
			Payload->SetNumberField(TEXT("progress"), Progress);
			Payload->SetNumberField(TEXT("progress_percent"), FMath::RoundToInt(Progress * 100.0));
			Payload->SetNumberField(TEXT("last_event_seq"), LatestSeq);
			if (Job.Events.Num() > 0)
			{
				const FSololmcpJobEvent& Event = Job.Events.Last();
				Payload->SetStringField(TEXT("event_type"), Event.Type);
				Payload->SetStringField(TEXT("message"), Event.Message);
				Payload->SetStringField(TEXT("timestamp"), Event.TimestampIso);
				Payload->SetObjectField(TEXT("event_payload"), Event.Payload);
			}
			Payload->SetObjectField(TEXT("receipt_envelope"), BuildReceiptEnvelope(Job));
			OutNotifications.Add(Payload);
			InOutLastSeqByJob.Add(Job.JobId, LatestSeq);
		}
	}
}
