// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpSharedLocks.h - v3.10.x worker-safety shared mutexes & cached pointers
// ----------------------------------------------------------------------------
// Single source of truth for cross-tool synchronization primitives that exist
// solely to allow GET-shaped tools to be promoted to the worker-safe whitelist
// (`SololmcpJobService.cpp::IsWorkerSafeTool`).
//
// Each accessor is a Meyers-style singleton (function-local static) so there
// is no static initialization order fiasco across translation units. Tools
// that touch a shared static container MUST grab the matching lock around
// every read/write - both the GET (read) and the sibling SET (write).
//
// Background: per Phase E re-audit (PHASE_E_WORKER_SAFE_REAUDIT.md) the GET
// tools `pcg_generation_budget_get` and `cook_status` race with their sibling
// writers (`pcg_generation_budget_set`, `cook_target`, `package_build`) when
// the GET runs from a TaskGraph worker thread. Adding a single FCriticalSection
// per shared container closes the race and makes the GET promotable.
// ----------------------------------------------------------------------------

#pragma once

#include "HAL/CriticalSection.h"

class IAssetRegistry;

namespace UE::SOMOLMCP::Locks
{
	/** PCG generation budget store mutex.
	 *  Guards `BudgetStore()` in SololmcpPcgEnhancementTools.cpp.
	 *  Held by: pcg_generation_budget_set (write), pcg_generation_budget_get (read),
	 *  AttachBudgetStatus helper used by pcg_dry_run.
	 */
	inline FCriticalSection& PcgBudgetStoreLock()
	{
		static FCriticalSection Lock;
		return Lock;
	}

	/** UAT job map mutex.
	 *  Guards `GetJobMap()` in SololmcpDevOpsTools.cpp.
	 *  Held by: cook_target / package_build (write - Add only),
	 *  cook_status (read + per-job state-transition write).
	 */
	inline FCriticalSection& UatJobMapLock()
	{
		static FCriticalSection Lock;
		return Lock;
	}
}

namespace UE::SOMOLMCP
{
	/** Cached IAssetRegistry pointer captured at FSololmcpServer::Start (game
	 *  thread, after the AssetRegistry module is loaded by the editor).
	 *  Worker-safe tools read this pointer instead of calling
	 *  FModuleManager::LoadModuleChecked, which is documented game-thread-only.
	 *
	 *  AssetRegistry queries themselves (GetAssets / GetAssetsByClass /
	 *  GetDependencies / GetReferencers) are RWLock-guarded inside
	 *  FAssetRegistryState and are safe to call from any thread.
	 *
	 *  Definition lives in SololmcpServer.cpp.
	 */
	extern IAssetRegistry* GSololmcpCachedAssetRegistry;
}
