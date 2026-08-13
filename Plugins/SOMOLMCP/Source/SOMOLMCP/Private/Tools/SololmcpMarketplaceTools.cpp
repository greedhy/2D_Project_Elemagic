// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpMarketplaceTools.cpp - v3.7.1 - Fab + Quixel marketplace auth gates.
// ----------------------------------------------------------------------------
// Both fab_search and quixel_search register as real MCP tools so callers get
// a graceful structured error instead of method_not_found. Epic's Fab /
// Megascans APIs require account auth that is not wired into this plugin, so
// these tools fail closed instead of pretending an unauthenticated search found
// zero assets.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::SOMOLMCP
{
	namespace
	{
		static bool RunFabSearchStub(
			const FSololmcpToolExecutionContext& /*Context*/,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString Query;
			Arguments->TryGetStringField(TEXT("query"), Query);

			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("unsupported_auth_required"));
			OutStructured->SetStringField(TEXT("error_code"), TEXT("EPIC_AUTH_NOT_CONFIGURED"));
			OutStructured->SetBoolField(TEXT("auth_required"), true);
			OutStructured->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
			OutStructured->SetStringField(
				TEXT("note"),
				TEXT("Fab search requires Epic account authentication, which is not configured. "
				     "No search was performed; do not interpret results=[] as no marketplace matches."));
			OutStructured->SetStringField(TEXT("query"), Query);
			OutError = TEXT("fab_search requires Epic OAuth; Epic auth is not configured.");
			OutSummary = FString::Printf(TEXT("fab_search unavailable (Epic auth not configured; query=%s)."), *Query);
			return false;
		}

		static bool RunQuixelSearchStub(
			const FSololmcpToolExecutionContext& /*Context*/,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString Query;
			Arguments->TryGetStringField(TEXT("query"), Query);

			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("unsupported_auth_required"));
			OutStructured->SetStringField(TEXT("error_code"), TEXT("EPIC_AUTH_NOT_CONFIGURED"));
			OutStructured->SetBoolField(TEXT("auth_required"), true);
			OutStructured->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
			OutStructured->SetStringField(
				TEXT("note"),
				TEXT("Quixel Megascans search requires Epic/Fab account authentication, which is not configured. "
				     "No search was performed; do not interpret results=[] as no Megascans matches."));
			OutStructured->SetStringField(TEXT("query"), Query);
			OutError = TEXT("quixel_search requires Epic OAuth; Epic auth is not configured.");
			OutSummary = FString::Printf(TEXT("quixel_search unavailable (Epic auth not configured; query=%s)."), *Query);
			return false;
		}
	}

	void RegisterMarketplaceTools(FSololmcpToolRegistry& Registry)
	{
		Registry.Register({
			TEXT("fab_search"),
			TEXT("Search Epic's Fab marketplace for assets by free-form query. "
			     "Fails with EPIC_AUTH_NOT_CONFIGURED until Epic OAuth is wired."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("query"), FSololmcpSchemaBuilder::String(TEXT("Search keywords, e.g. \"medieval sword\"."))}
				},
				{TEXT("query")}),
			&RunFabSearchStub
		});

		Registry.Register({
			TEXT("quixel_search"),
			TEXT("Search Quixel Megascans (now part of Fab) for scan-based assets by query. "
			     "Fails with EPIC_AUTH_NOT_CONFIGURED until Epic OAuth is wired."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("query"), FSololmcpSchemaBuilder::String(TEXT("Search keywords, e.g. \"mossy rock\"."))}
				},
				{TEXT("query")}),
			&RunQuixelSearchStub
		});
	}
}
