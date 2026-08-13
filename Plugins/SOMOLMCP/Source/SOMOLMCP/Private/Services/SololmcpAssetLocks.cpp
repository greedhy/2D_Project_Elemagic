// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP Phase 3 — Per-asset reader/writer lock manager implementation.

#include "Services/SololmcpAssetLocks.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "Misc/DateTime.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPAssetLocks, Log, All);

namespace UE::SOMOLMCP
{
	namespace
	{
		// Spin back-off between TryLock attempts. 1ms is a balance between
		// responsiveness (low latency on contended-but-short locks) and CPU burn.
		constexpr float kSpinSleepSec = 0.001f;
	}

	FAssetLockManager& FAssetLockManager::Get()
	{
		// Meyers singleton — thread-safe init (C++11). Process-wide lifetime.
		static FAssetLockManager Instance;
		return Instance;
	}

	FString FAssetLockManager::NormalizeAssetPath(const FString& AssetPath)
	{
		// Strip the trailing ".AssetName" suffix so /Game/Foo/Bar and
		// /Game/Foo/Bar.Bar (and /Game/Foo/Bar.Bar:Sub) all hash to the same key.
		// We split on the FIRST '.' after the last '/' — that's the package/object
		// boundary in UE asset paths.
		int32 LastSlash = INDEX_NONE;
		AssetPath.FindLastChar(TEXT('/'), LastSlash);

		const int32 SearchStart = (LastSlash != INDEX_NONE) ? LastSlash : 0;
		int32 DotIndex = INDEX_NONE;
		for (int32 i = SearchStart; i < AssetPath.Len(); ++i)
		{
			if (AssetPath[i] == TEXT('.'))
			{
				DotIndex = i;
				break;
			}
		}

		FString Result = (DotIndex != INDEX_NONE) ? AssetPath.Left(DotIndex) : AssetPath;
		// Trim any trailing slash so /Game/Foo/ and /Game/Foo collapse together.
		while (Result.Len() > 1 && Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1, SOMOLMCP_NO_SHRINK);
		}
		return Result;
	}

	TSharedPtr<FAssetLockManager::FAssetLockEntry> FAssetLockManager::GetOrCreateEntry(const FString& AssetPath)
	{
		const FString Key = NormalizeAssetPath(AssetPath);
		FScopeLock MapGuard(&MapLock);
		if (TSharedPtr<FAssetLockEntry>* Existing = LockMap.Find(Key))
		{
			return *Existing;
		}
		TSharedPtr<FAssetLockEntry> NewEntry = MakeShared<FAssetLockEntry>();
		LockMap.Add(Key, NewEntry);
		return NewEntry;
	}

	TSharedPtr<FAssetLockManager::FAssetLockEntry> FAssetLockManager::FindEntry(const FString& AssetPath) const
	{
		const FString Key = NormalizeAssetPath(AssetPath);
		FScopeLock MapGuard(&MapLock);
		if (const TSharedPtr<FAssetLockEntry>* Existing = LockMap.Find(Key))
		{
			return *Existing;
		}
		return nullptr;
	}

	bool FAssetLockManager::AcquireWrite(const FString& AssetPath, double TimeoutSec)
	{
		TSharedPtr<FAssetLockEntry> Entry = GetOrCreateEntry(AssetPath);
		if (!Entry.IsValid())
		{
			return false;
		}

		const double StartTime = FPlatformTime::Seconds();
		const uint32 ThisThread = FPlatformTLS::GetCurrentThreadId();

		while (true)
		{
			if (Entry->Lock.TryWriteLock())
			{
				// Track diagnostics under the map lock — Entry->Lock is already
				// held exclusively, but ReaderThreads/WriterThread are diagnostic
				// metadata orthogonal to the FRWLock contract.
				FScopeLock MapGuard(&MapLock);
				Entry->WriterThread = ThisThread;
				return true;
			}

			const double Elapsed = FPlatformTime::Seconds() - StartTime;
			if (Elapsed >= TimeoutSec)
			{
				UE_LOG(LogSOMOLMCPAssetLocks, Warning,
					TEXT("AcquireWrite TIMEOUT after %.2fs on '%s' (normalized '%s')"),
					Elapsed, *AssetPath, *NormalizeAssetPath(AssetPath));
				return false;
			}
			FPlatformProcess::Sleep(kSpinSleepSec);
		}
	}

	void FAssetLockManager::ReleaseWrite(const FString& AssetPath)
	{
		TSharedPtr<FAssetLockEntry> Entry = FindEntry(AssetPath);
		if (!Entry.IsValid())
		{
			UE_LOG(LogSOMOLMCPAssetLocks, Warning,
				TEXT("ReleaseWrite called on unknown path '%s' — no-op"), *AssetPath);
			return;
		}

		{
			FScopeLock MapGuard(&MapLock);
			Entry->WriterThread = 0;
		}
		Entry->Lock.WriteUnlock();
	}

	bool FAssetLockManager::AcquireRead(const FString& AssetPath, double TimeoutSec)
	{
		TSharedPtr<FAssetLockEntry> Entry = GetOrCreateEntry(AssetPath);
		if (!Entry.IsValid())
		{
			return false;
		}

		const double StartTime = FPlatformTime::Seconds();
		const uint32 ThisThread = FPlatformTLS::GetCurrentThreadId();

		while (true)
		{
			if (Entry->Lock.TryReadLock())
			{
				FScopeLock MapGuard(&MapLock);
				Entry->ReaderThreads.Add(ThisThread);
				return true;
			}

			const double Elapsed = FPlatformTime::Seconds() - StartTime;
			if (Elapsed >= TimeoutSec)
			{
				UE_LOG(LogSOMOLMCPAssetLocks, Warning,
					TEXT("AcquireRead TIMEOUT after %.2fs on '%s' (normalized '%s')"),
					Elapsed, *AssetPath, *NormalizeAssetPath(AssetPath));
				return false;
			}
			FPlatformProcess::Sleep(kSpinSleepSec);
		}
	}

	void FAssetLockManager::ReleaseRead(const FString& AssetPath)
	{
		TSharedPtr<FAssetLockEntry> Entry = FindEntry(AssetPath);
		if (!Entry.IsValid())
		{
			UE_LOG(LogSOMOLMCPAssetLocks, Warning,
				TEXT("ReleaseRead called on unknown path '%s' — no-op"), *AssetPath);
			return;
		}

		{
			FScopeLock MapGuard(&MapLock);
			Entry->ReaderThreads.Remove(FPlatformTLS::GetCurrentThreadId());
		}
		Entry->Lock.ReadUnlock();
	}

	int32 FAssetLockManager::GetActiveReaderCount(const FString& AssetPath) const
	{
		TSharedPtr<FAssetLockEntry> Entry = FindEntry(AssetPath);
		if (!Entry.IsValid())
		{
			return 0;
		}
		FScopeLock MapGuard(&MapLock);
		return Entry->ReaderThreads.Num();
	}

	bool FAssetLockManager::HasActiveWriter(const FString& AssetPath) const
	{
		TSharedPtr<FAssetLockEntry> Entry = FindEntry(AssetPath);
		if (!Entry.IsValid())
		{
			return false;
		}
		FScopeLock MapGuard(&MapLock);
		return Entry->WriterThread != 0;
	}

	FString FAssetLockManager::DumpStats() const
	{
		FScopeLock MapGuard(&MapLock);
		FString Out = FString::Printf(TEXT("FAssetLockManager: %d entries\n"), LockMap.Num());
		for (const TPair<FString, TSharedPtr<FAssetLockEntry>>& Pair : LockMap)
		{
			if (!Pair.Value.IsValid())
			{
				continue;
			}
			Out += FString::Printf(
				TEXT("  '%s'  readers=%d  writer=%u\n"),
				*Pair.Key,
				Pair.Value->ReaderThreads.Num(),
				Pair.Value->WriterThread);
		}
		return Out;
	}
}
