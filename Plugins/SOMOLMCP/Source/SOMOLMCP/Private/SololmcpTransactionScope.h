// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once
// **P3-7**: Named-transaction registry — group multiple MCP tool calls into a single
// FScopedTransaction so failures auto-rollback as a unit (and the entire group shows
// as ONE Undo step in the editor).
//
// Usage from a tool handler:
//   if (TxIdMaybe.IsSet()) {
//     SololmcpTransaction::FRegistry::Get().IncrementOpCount(TxIdMaybe.GetValue());
//   }
//
// Lifecycle (managed by transaction_begin / transaction_end / transaction_abort tools):
//   1. transaction_begin(tx_id="my_op", description="Build PBR material") → starts FScopedTransaction
//   2. (other tools call .Modify() on objects; UE auto-records into the active transaction)
//   3. transaction_end(tx_id) → drops the FScopedTransaction normally → 1 undo entry
//   4. OR transaction_abort(tx_id) → calls FScopedTransaction::Cancel() → all changes rolled back

#include "CoreMinimal.h"
#include "ScopedTransaction.h"
#include "Templates/SharedPointer.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"

namespace SololmcpTransaction
{
	struct FNamedTransaction
	{
		TSharedPtr<FScopedTransaction> Scope;
		FString Description;
		int32 OpCount = 0;
		bool bAborted = false;
	};

	/** Singleton registry of open transactions. Thread-safe via FCriticalSection. */
	class FRegistry
	{
	public:
		static FRegistry& Get()
		{
			static FRegistry Instance;
			return Instance;
		}

		/** Begin a named transaction. Returns true if newly created; false if already open (idempotent). */
		bool Begin(const FString& TxId, const FString& Description)
		{
			FScopeLock Lock(&Mtx);
			if (Open.Contains(TxId))
			{
				return false;
			}
			FNamedTransaction Tx;
			Tx.Description = Description;
			Tx.Scope = MakeShareable(new FScopedTransaction(FText::FromString(Description)));
			Open.Add(TxId, MoveTemp(Tx));
			return true;
		}

		/** End normally — commits the FScopedTransaction (becomes 1 undo step). */
		bool End(const FString& TxId)
		{
			FScopeLock Lock(&Mtx);
			if (FNamedTransaction* Tx = Open.Find(TxId))
			{
				Tx->Scope.Reset(); // drop FScopedTransaction → commits
				Open.Remove(TxId);
				return true;
			}
			return false;
		}

		/** Abort — rolls back all changes via FScopedTransaction::Cancel(). */
		bool Abort(const FString& TxId)
		{
			FScopeLock Lock(&Mtx);
			if (FNamedTransaction* Tx = Open.Find(TxId))
			{
				if (Tx->Scope.IsValid())
				{
					Tx->Scope->Cancel();
				}
				Tx->Scope.Reset();
				Tx->bAborted = true;
				Open.Remove(TxId);
				return true;
			}
			return false;
		}

		int32 GetOpCount(const FString& TxId) const
		{
			FScopeLock Lock(&Mtx);
			if (const FNamedTransaction* Tx = Open.Find(TxId))
			{
				return Tx->OpCount;
			}
			return -1;
		}

		void IncrementOpCount(const FString& TxId)
		{
			FScopeLock Lock(&Mtx);
			if (FNamedTransaction* Tx = Open.Find(TxId))
			{
				Tx->OpCount++;
			}
		}

		bool IsOpen(const FString& TxId) const
		{
			FScopeLock Lock(&Mtx);
			return Open.Contains(TxId);
		}

		TArray<FString> ListOpen() const
		{
			FScopeLock Lock(&Mtx);
			TArray<FString> Out;
			Open.GetKeys(Out);
			return Out;
		}

		FString GetDescription(const FString& TxId) const
		{
			FScopeLock Lock(&Mtx);
			if (const FNamedTransaction* Tx = Open.Find(TxId))
			{
				return Tx->Description;
			}
			return FString();
		}

	private:
		mutable FCriticalSection Mtx;
		TMap<FString, FNamedTransaction> Open;
	};
} // namespace SololmcpTransaction
