// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP Phase 3 — Per-asset reader/writer lock manager.
//
// Goal: serialize concurrent operations on the SAME asset path while keeping
// operations on DIFFERENT asset paths fully parallel. Multiple readers may
// hold a path simultaneously; a writer is exclusive per path.
//
// This was introduced after editor crashes caused by concurrent editor-mutating
// ops on the same asset (e.g. ForceDeleteObjects ensure failure on
// /Game/__TestPCG/PG_LC_Test). Always prefer the RAII guards below over direct
// Acquire/Release calls.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeRWLock.h"
#include "Templates/SharedPointer.h"

namespace UE::SOMOLMCP
{
	/**
	 * Per-asset reader/writer lock manager.
	 *
	 * Keys are normalized package paths (e.g. "/Game/Foo/Bar"); the ".AssetName"
	 * suffix is stripped so "/Game/Foo/Bar" and "/Game/Foo/Bar.Bar" share one lock.
	 *
	 * Use FAssetReadScope / FAssetWriteScope (RAII) in callers — direct
	 * Acquire/Release is exposed only for diagnostics and testing.
	 */
	class SOMOLMCP_API FAssetLockManager
	{
	public:
		static FAssetLockManager& Get();

		/**
		 * Acquire exclusive write lock for the given asset path.
		 * Spins on TryWriteLock with a 1ms back-off until acquired or timed out.
		 * @return false on timeout — caller should fail the tool gracefully.
		 */
		bool AcquireWrite(const FString& AssetPath, double TimeoutSec = 10.0);
		void ReleaseWrite(const FString& AssetPath);

		/** Acquire shared read lock for the given asset path. */
		bool AcquireRead(const FString& AssetPath, double TimeoutSec = 10.0);
		void ReleaseRead(const FString& AssetPath);

		/** Diagnostics — number of threads currently holding a read lock for this path. */
		int32 GetActiveReaderCount(const FString& AssetPath) const;

		/** Diagnostics — true if any thread is currently holding a write lock for this path. */
		bool HasActiveWriter(const FString& AssetPath) const;

		/** Human-readable dump of every entry in the lock map (for debugging / stats endpoint). */
		FString DumpStats() const;

		/**
		 * Normalize an asset path to its package-only form (strip trailing ".AssetName").
		 * Exposed publicly for callers that want the same key the lock manager uses.
		 */
		static FString NormalizeAssetPath(const FString& AssetPath);

	private:
		struct FAssetLockEntry
		{
			FRWLock Lock;
			// Holder thread IDs — diagnostic only; protected by the outer MapLock when mutated.
			TSet<uint32> ReaderThreads;
			uint32 WriterThread = 0;
		};

		/** Find or create the entry for a path. Returns a stable pointer (TSharedPtr keeps it alive). */
		TSharedPtr<FAssetLockEntry> GetOrCreateEntry(const FString& AssetPath);

		/** Find an existing entry without creating one. Returns nullptr if absent. */
		TSharedPtr<FAssetLockEntry> FindEntry(const FString& AssetPath) const;

		mutable FCriticalSection MapLock;	// protects LockMap itself (not the per-entry FRWLock)
		TMap<FString, TSharedPtr<FAssetLockEntry>> LockMap;
	};

	/** RAII exclusive (writer) lock guard. Check bAcquired before assuming the lock is held. */
	struct FAssetWriteScope
	{
		FString Path;
		bool bAcquired = false;

		FAssetWriteScope(const FString& InPath, double TimeoutSec = 10.0)
			: Path(InPath)
		{
			bAcquired = FAssetLockManager::Get().AcquireWrite(InPath, TimeoutSec);
		}

		~FAssetWriteScope()
		{
			if (bAcquired)
			{
				FAssetLockManager::Get().ReleaseWrite(Path);
			}
		}

		FAssetWriteScope(const FAssetWriteScope&) = delete;
		FAssetWriteScope& operator=(const FAssetWriteScope&) = delete;
	};

	/** RAII shared (reader) lock guard. Check bAcquired before assuming the lock is held. */
	struct FAssetReadScope
	{
		FString Path;
		bool bAcquired = false;

		FAssetReadScope(const FString& InPath, double TimeoutSec = 10.0)
			: Path(InPath)
		{
			bAcquired = FAssetLockManager::Get().AcquireRead(InPath, TimeoutSec);
		}

		~FAssetReadScope()
		{
			if (bAcquired)
			{
				FAssetLockManager::Get().ReleaseRead(Path);
			}
		}

		FAssetReadScope(const FAssetReadScope&) = delete;
		FAssetReadScope& operator=(const FAssetReadScope&) = delete;
	};
}
