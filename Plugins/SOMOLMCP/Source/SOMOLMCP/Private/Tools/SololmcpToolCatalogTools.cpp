// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Bounded tool discovery for context-limited clients.
//
// This server registers ~4,800 tools. A stock MCP client opens a session by calling
// tools/list with no arguments, which here is 15.8 MB -- roughly 4.1 million tokens.
// No model can accept that, so out of the box the entire surface is undiscoverable.
//
// tools/list does support filtering (toolset, prefix, query, names_only, limit,
// cursor), but those are extensions a standard client never sends, and even the
// filtered forms do not solve the problem on their own: names_only over everything
// is still ~220k tokens, and names_only over the largest single toolset is ~98k.
//
// The constraint this is built against: a 256k-context model must be able to
// discover the *complete* surface, not a sample of it, and then construct correct
// calls. That needs three properties, and the third is the one usually missed:
//
//   bounded    every response has a hard size ceiling, so no call can blow the
//              context regardless of how many tools exist;
//   drillable  cheap breadth first, detail only for the handful actually needed;
//   provably   every listing reports `total` and `returned` and a cursor, so the
//   complete   client can tell whether it has seen everything rather than assuming
//              it has. A truncated list that looks complete is worse than an error,
//              because the model will confidently conclude a tool does not exist.
//
// Budget with this tool: ~2k tokens to see all groups, ~6k for a page of 300 names
// with summaries, ~1k for the full schemas of the few tools actually being called.
// Finding and correctly invoking a tool costs under 10k tokens instead of 4.1M.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::SOMOLMCP
{
namespace ToolCatalogPrivate
{
	/** Hard ceilings. These bound the response no matter what the caller asks for. */
	static constexpr int32 MaxNamesPerPage = 500;
	static constexpr int32 DefaultNamesPerPage = 300;
	static constexpr int32 MaxSchemasPerCall = 40;
	/** Summaries are truncated so a page's size stays predictable. */
	static constexpr int32 SummaryChars = 140;

	/**
	 * The group a tool belongs to: the first underscore-separated segment of its
	 * name, which is how this registry's naming already partitions the surface
	 * (asset_*, landscape_*, worldforge_*). Derived rather than curated so a new
	 * tool is grouped the moment it is registered, with no table to forget to update.
	 */
	inline FString GroupOf(const FString& ToolName)
	{
		int32 Underscore = INDEX_NONE;
		if (ToolName.FindChar(TEXT('_'), Underscore) && Underscore > 0)
		{
			return ToolName.Left(Underscore);
		}
		return ToolName;
	}

	/** First sentence of a description, truncated, with whitespace collapsed. */
	inline FString Summarize(const FString& Description)
	{
		FString Text = Description.TrimStartAndEnd();
		int32 Stop = INDEX_NONE;
		if (Text.FindChar(TEXT('.'), Stop) && Stop > 20)
		{
			Text = Text.Left(Stop + 1);
		}
		Text.ReplaceInline(TEXT("\n"), TEXT(" "));
		Text.ReplaceInline(TEXT("\r"), TEXT(" "));
		while (Text.Contains(TEXT("  ")))
		{
			Text.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		if (Text.Len() > SummaryChars)
		{
			Text = Text.Left(SummaryChars - 1) + TEXT("…");
		}
		return Text;
	}

	struct FToolRow
	{
		FString Name;
		FString Description;
		TSharedPtr<FJsonObject> Schema;
	};

	/** Flatten the registry's tool list once per call. */
	inline TArray<FToolRow> SnapshotTools(FSololmcpToolRegistry& Registry)
	{
		TArray<FToolRow> Rows;
		for (const TSharedPtr<FJsonValue>& Value : Registry.BuildToolsList())
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			FToolRow Row;
			Obj->TryGetStringField(TEXT("name"), Row.Name);
			if (Row.Name.IsEmpty())
			{
				continue;
			}
			Obj->TryGetStringField(TEXT("description"), Row.Description);
			const TSharedPtr<FJsonObject>* SchemaPtr = nullptr;
			if (Obj->TryGetObjectField(TEXT("inputSchema"), SchemaPtr) && SchemaPtr != nullptr)
			{
				Row.Schema = *SchemaPtr;
			}
			Rows.Add(MoveTemp(Row));
		}
		Rows.Sort([](const FToolRow& A, const FToolRow& B) { return A.Name < B.Name; });
		return Rows;
	}
} // namespace ToolCatalogPrivate

void RegisterToolCatalogTools(FSololmcpToolRegistry& Registry)
{
	using namespace ToolCatalogPrivate;

	// Captured by reference: the registry owns these tool definitions, so it
	// outlives every call into them.
	FSololmcpToolRegistry* RegistryPtr = &Registry;

	Registry.Register({
		TEXT("tool_catalog"),
		TEXT("Discover this server's tools within a bounded context budget. This server registers "
			 "thousands of tools and an unfiltered tools/list is far too large for any model, so "
			 "start here.\n"
			 "\n"
			 "mode=groups   every tool group with its count. Cheap and complete; call this first.\n"
			 "mode=names    tool names and one-line summaries for a group or a search, paged.\n"
			 "mode=schemas  full input schemas for up to 40 named tools — call this last, only for "
			 "the tools you are actually going to invoke.\n"
			 "\n"
			 "Every listing reports total, returned and next_cursor, so you can tell whether you "
			 "have seen everything rather than assuming it. Keep paging while next_cursor is "
			 "present; a tool you did not see is not proof it does not exist."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mode"), FSololmcpSchemaBuilder::WithDefaultString(
					FSololmcpSchemaBuilder::WithEnum(
						FSololmcpSchemaBuilder::String(TEXT("Which discovery step to run.")),
						{TEXT("groups"), TEXT("names"), TEXT("schemas")}),
					TEXT("groups"))},
				{TEXT("group"), FSololmcpSchemaBuilder::String(
					TEXT("mode=names: restrict to one group from mode=groups."))},
				{TEXT("query"), FSololmcpSchemaBuilder::String(
					TEXT("mode=names: case-insensitive substring match on name and description. "
						 "Combines with group."))},
				{TEXT("names"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Exact tool name.")),
					TEXT("mode=schemas: the tools to describe. Max 40."))},
				{TEXT("cursor"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("mode=names: index to resume from; pass the next_cursor you were given.")), 0)},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("mode=names: page size, capped at 500.")), DefaultNamesPerPage)}
			},
			{}),
		[RegistryPtr](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString Mode = TEXT("groups");
			Args->TryGetStringField(TEXT("mode"), Mode);

			const TArray<FToolRow> Rows = SnapshotTools(*RegistryPtr);
			OutStructured->SetNumberField(TEXT("server_tool_count"), Rows.Num());

			// ── groups ───────────────────────────────────────────────────────
			if (Mode.Equals(TEXT("groups"), ESearchCase::IgnoreCase))
			{
				TMap<FString, int32> Counts;
				for (const FToolRow& Row : Rows)
				{
					Counts.FindOrAdd(GroupOf(Row.Name))++;
				}
				TArray<FString> Names;
				Counts.GetKeys(Names);
				Names.Sort();

				TArray<TSharedPtr<FJsonValue>> Out;
				for (const FString& Name : Names)
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("group"), Name);
					Entry->SetNumberField(TEXT("tools"), Counts[Name]);
					Out.Add(MakeShared<FJsonValueObject>(Entry));
				}
				OutStructured->SetArrayField(TEXT("groups"), Out);
				OutStructured->SetNumberField(TEXT("group_count"), Out.Num());
				// No cursor: this listing is always complete in one response, which is
				// what makes it safe to reason from.
				OutStructured->SetBoolField(TEXT("complete"), true);
				OutStructured->SetStringField(TEXT("next_step"),
					TEXT("tool_catalog mode=names with group=<group> or query=<text>."));
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = FString::Printf(TEXT("%d group(s) covering %d tool(s)."), Out.Num(), Rows.Num());
				return true;
			}

			// ── names ────────────────────────────────────────────────────────
			if (Mode.Equals(TEXT("names"), ESearchCase::IgnoreCase))
			{
				FString Group;
				Args->TryGetStringField(TEXT("group"), Group);
				FString Query;
				Args->TryGetStringField(TEXT("query"), Query);
				int32 Cursor = 0;
				Args->TryGetNumberField(TEXT("cursor"), Cursor);
				Cursor = FMath::Max(0, Cursor);
				int32 Limit = DefaultNamesPerPage;
				Args->TryGetNumberField(TEXT("limit"), Limit);
				Limit = FMath::Clamp(Limit, 1, MaxNamesPerPage);

				TArray<const FToolRow*> Matched;
				for (const FToolRow& Row : Rows)
				{
					if (!Group.IsEmpty() && !GroupOf(Row.Name).Equals(Group, ESearchCase::IgnoreCase))
					{
						continue;
					}
					if (!Query.IsEmpty()
						&& !Row.Name.Contains(Query, ESearchCase::IgnoreCase)
						&& !Row.Description.Contains(Query, ESearchCase::IgnoreCase))
					{
						continue;
					}
					Matched.Add(&Row);
				}

				TArray<TSharedPtr<FJsonValue>> Out;
				for (int32 Index = Cursor; Index < Matched.Num() && Out.Num() < Limit; ++Index)
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), Matched[Index]->Name);
					Entry->SetStringField(TEXT("summary"), Summarize(Matched[Index]->Description));
					Out.Add(MakeShared<FJsonValueObject>(Entry));
				}
				const int32 NextCursor = Cursor + Out.Num();

				OutStructured->SetArrayField(TEXT("tools"), Out);
				OutStructured->SetNumberField(TEXT("total"), Matched.Num());
				OutStructured->SetNumberField(TEXT("returned"), Out.Num());
				OutStructured->SetNumberField(TEXT("cursor"), Cursor);
				const bool bComplete = NextCursor >= Matched.Num();
				OutStructured->SetBoolField(TEXT("complete"), bComplete);
				if (!bComplete)
				{
					OutStructured->SetNumberField(TEXT("next_cursor"), NextCursor);
					// Say it plainly. A model that stops at page one and concludes a
					// tool is absent will confidently build the wrong queue.
					OutStructured->SetStringField(TEXT("note"),
						FString::Printf(TEXT("%d of %d shown. Call again with cursor=%d to continue; "
											 "do not conclude a tool is missing until complete=true."),
							NextCursor, Matched.Num(), NextCursor));
				}
				OutStructured->SetStringField(TEXT("next_step"),
					TEXT("tool_catalog mode=schemas names=[…] for the tools you will call."));
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = FString::Printf(TEXT("%d of %d tool(s)%s."),
					Out.Num(), Matched.Num(), bComplete ? TEXT("; complete") : TEXT("; more available"));
				return true;
			}

			// ── schemas ──────────────────────────────────────────────────────
			if (Mode.Equals(TEXT("schemas"), ESearchCase::IgnoreCase))
			{
				const TArray<TSharedPtr<FJsonValue>>* Requested = nullptr;
				if (!Args->TryGetArrayField(TEXT("names"), Requested) || Requested == nullptr || Requested->Num() == 0)
				{
					SololmcpError::MissingParam(OutStructured, TEXT("names"));
					OutError = TEXT("mode=schemas needs names.");
					return false;
				}
				if (Requested->Num() > MaxSchemasPerCall)
				{
					SololmcpError::Set(OutStructured, TEXT("INVALID_PARAM"), TEXT("names"),
						FString::Printf(TEXT("Ask for at most %d schemas per call; full schemas are "
											 "large and the cap is what keeps this response bounded."),
							MaxSchemasPerCall));
					OutError = FString::Printf(TEXT("Too many names (%d)."), Requested->Num());
					return false;
				}

				TMap<FString, const FToolRow*> ByName;
				for (const FToolRow& Row : Rows)
				{
					ByName.Add(Row.Name, &Row);
				}

				TArray<TSharedPtr<FJsonValue>> Out;
				TArray<TSharedPtr<FJsonValue>> Unknown;
				for (const TSharedPtr<FJsonValue>& Value : *Requested)
				{
					FString Name;
					if (!Value.IsValid() || !Value->TryGetString(Name) || Name.IsEmpty())
					{
						continue;
					}
					const FToolRow* const* Found = ByName.Find(Name);
					if (Found == nullptr)
					{
						Unknown.Add(MakeShared<FJsonValueString>(Name));
						continue;
					}
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), (*Found)->Name);
					Entry->SetStringField(TEXT("description"), (*Found)->Description);
					if ((*Found)->Schema.IsValid())
					{
						Entry->SetObjectField(TEXT("input_schema"), (*Found)->Schema);
					}
					Out.Add(MakeShared<FJsonValueObject>(Entry));
				}

				OutStructured->SetArrayField(TEXT("tools"), Out);
				OutStructured->SetNumberField(TEXT("returned"), Out.Num());
				if (Unknown.Num() > 0)
				{
					// Named but absent is a real answer, not a failure: it tells the
					// client this tool is not on this engine or build.
					OutStructured->SetArrayField(TEXT("unknown"), Unknown);
					OutStructured->SetStringField(TEXT("unknown_note"),
						TEXT("These names are not registered here. Re-check with mode=names; a tool "
							 "can be absent on this engine version or build."));
				}
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = FString::Printf(TEXT("%d schema(s)%s."),
					Out.Num(), Unknown.Num() ? *FString::Printf(TEXT("; %d unknown"), Unknown.Num()) : TEXT(""));
				return true;
			}

			SololmcpError::Set(OutStructured, TEXT("INVALID_PARAM"), TEXT("mode"),
				TEXT("Expected groups, names or schemas."));
			OutError = FString::Printf(TEXT("Unknown mode '%s'."), *Mode);
			return false;
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
