// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// **P3-7**: Transaction tools — group multiple MCP tool calls into a single FScopedTransaction
// so failures auto-rollback as a unit (and the entire group shows as ONE Undo step in the editor).
//
// 4 user-facing tools registered: transaction_begin / end / abort / list

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpTransactionScope.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::SOMOLMCP
{
	void RegisterTransactionTools(FSololmcpToolRegistry& Registry)
	{
		// ── transaction_begin ────────────────────────────────────────────────
		Registry.Register({
			TEXT("transaction_begin"),
			TEXT("Begin a named transaction. All Modify() calls until transaction_end/abort become a single undo step. P3-7."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("tx_id"), FSololmcpSchemaBuilder::String()},
					{TEXT("description"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("tx_id"), TEXT("description")}
			),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString TxId, Description;
				if (!Args->TryGetStringField(TEXT("tx_id"), TxId) || TxId.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("tx_id"));
					OutError = TEXT("Missing tx_id."); return false;
				}
				if (!Args->TryGetStringField(TEXT("description"), Description) || Description.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("description"));
					OutError = TEXT("Missing description."); return false;
				}

				const bool bWasAlreadyOpen = SololmcpTransaction::FRegistry::Get().IsOpen(TxId);
				const bool bNewlyCreated = SololmcpTransaction::FRegistry::Get().Begin(TxId, Description);
				const bool bIsOpenAfterBegin = SololmcpTransaction::FRegistry::Get().IsOpen(TxId);
				if (!bIsOpenAfterBegin)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("tx_id"),
						TEXT("Transaction registry did not report the transaction as open after begin."));
					OutStructured->SetStringField(TEXT("tx_id"), TxId);
					OutStructured->SetBoolField(TEXT("ok"), false);
					OutError = FString::Printf(TEXT("Failed to begin transaction '%s'."), *TxId);
					return false;
				}

				OutStructured->SetStringField(TEXT("tx_id"), TxId);
				OutStructured->SetStringField(TEXT("description"), Description);
				OutStructured->SetBoolField(TEXT("was_already_open"), bWasAlreadyOpen);
				OutStructured->SetBoolField(TEXT("newly_created"), bNewlyCreated);
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = FString::Printf(TEXT("Transaction '%s' %s: %s"),
					*TxId,
					bNewlyCreated ? TEXT("opened") : TEXT("already open"),
					*Description);
				return true;
			},
			nullptr,
			0
		});

		// ── transaction_end ──────────────────────────────────────────────────
		Registry.Register({
			TEXT("transaction_end"),
			TEXT("Close a named transaction normally. Becomes 1 undo entry containing all changes since transaction_begin. P3-7."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("tx_id"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("tx_id")}
			),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString TxId;
				if (!Args->TryGetStringField(TEXT("tx_id"), TxId) || TxId.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("tx_id"));
					OutError = TEXT("Missing tx_id."); return false;
				}

				const int32 Ops = SololmcpTransaction::FRegistry::Get().GetOpCount(TxId);
				const bool bClosed = SololmcpTransaction::FRegistry::Get().End(TxId);

				if (!bClosed)
				{
					SololmcpError::NotFound(OutStructured, TxId);
					OutError = FString::Printf(TEXT("No open transaction with tx_id '%s'."), *TxId);
					return false;
				}

				const bool bStillOpen = SololmcpTransaction::FRegistry::Get().IsOpen(TxId);
				if (bStillOpen)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("tx_id"),
						TEXT("Transaction registry still reports the transaction as open after end."));
					OutStructured->SetStringField(TEXT("tx_id"), TxId);
					OutStructured->SetNumberField(TEXT("ops"), Ops);
					OutStructured->SetBoolField(TEXT("ok"), false);
					OutError = FString::Printf(TEXT("Failed to close transaction '%s'."), *TxId);
					return false;
				}

				OutStructured->SetStringField(TEXT("tx_id"), TxId);
				OutStructured->SetNumberField(TEXT("ops"), Ops);
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = FString::Printf(TEXT("Closed transaction '%s' (%d ops, 1 undo step)."), *TxId, Ops);
				return true;
			},
			nullptr,
			0
		});

		// ── transaction_abort ────────────────────────────────────────────────
		Registry.Register({
			TEXT("transaction_abort"),
			TEXT("Abort a named transaction. Cancels FScopedTransaction → all Modify() changes since begin are rolled back. P3-7."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("tx_id"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("tx_id")}
			),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString TxId;
				if (!Args->TryGetStringField(TEXT("tx_id"), TxId) || TxId.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("tx_id"));
					OutError = TEXT("Missing tx_id."); return false;
				}

				const int32 Ops = SololmcpTransaction::FRegistry::Get().GetOpCount(TxId);
				const bool bAborted = SololmcpTransaction::FRegistry::Get().Abort(TxId);

				if (!bAborted)
				{
					SololmcpError::NotFound(OutStructured, TxId);
					OutError = FString::Printf(TEXT("No open transaction with tx_id '%s'."), *TxId);
					return false;
				}

				const bool bStillOpen = SololmcpTransaction::FRegistry::Get().IsOpen(TxId);
				if (bStillOpen)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("tx_id"),
						TEXT("Transaction registry still reports the transaction as open after abort."));
					OutStructured->SetStringField(TEXT("tx_id"), TxId);
					OutStructured->SetNumberField(TEXT("ops_aborted"), Ops);
					OutStructured->SetBoolField(TEXT("ok"), false);
					OutError = FString::Printf(TEXT("Failed to abort transaction '%s'."), *TxId);
					return false;
				}

				OutStructured->SetStringField(TEXT("tx_id"), TxId);
				OutStructured->SetNumberField(TEXT("ops_aborted"), Ops);
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = FString::Printf(TEXT("Aborted transaction '%s' (%d ops rolled back)."), *TxId, Ops);
				return true;
			},
			nullptr,
			0
		});

		// ── transaction_list ─────────────────────────────────────────────────
		Registry.Register({
			TEXT("transaction_list"),
			TEXT("List all currently-open named transactions. Useful for AI to know what transactions are pending. P3-7."),
			FSololmcpSchemaBuilder::Object({}, {}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
			   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
			{
				const TArray<FString> OpenIds = SololmcpTransaction::FRegistry::Get().ListOpen();
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FString& Id : OpenIds)
				{
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("tx_id"), Id);
					Item->SetStringField(TEXT("description"), SololmcpTransaction::FRegistry::Get().GetDescription(Id));
					Item->SetNumberField(TEXT("ops"), SololmcpTransaction::FRegistry::Get().GetOpCount(Id));
					Arr.Add(MakeShared<FJsonValueObject>(Item));
				}
				OutStructured->SetArrayField(TEXT("open"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("%d open transactions."), Arr.Num());
				return true;
			},
			nullptr,
			0  // never cache list result
		});
	}
}
