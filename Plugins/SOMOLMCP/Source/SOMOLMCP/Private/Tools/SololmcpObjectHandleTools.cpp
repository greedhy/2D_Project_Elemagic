// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Session handle management.
//
// Handles name live editor objects that have no asset path — see
// SololmcpObjectHandles.h for why that is necessary. Because every handle pins its
// object against garbage collection, a long queued session needs a way to see what
// it is holding and to let go; these three tools are that.
//
// Minting happens in the tool families that produce the objects (sequencer
// sections, constraint objects, transient graphs). Only inspection and release are
// generic, so only those live here.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpObjectHandles.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::SOMOLMCP
{
void RegisterObjectHandleTools(FSololmcpToolRegistry& Registry)
{
	// ── handle_list ────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("handle_list"),
		TEXT("List live session handles: what each one points at, where it came from and when it "
			 "was created. Every handle pins its object against garbage collection, so this is also "
			 "how a long-running session checks what it is still holding."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("kind"), FSololmcpSchemaBuilder::String(
					TEXT("Restrict to one handle kind, e.g. movie_scene_section. Omit for all."))}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			FSololmcpObjectHandles& Handles = FSololmcpObjectHandles::Get();

			// Prune first so the listing cannot report a handle whose object is gone.
			const int32 Pruned = Handles.PruneDead();

			FString KindFilter;
			Args->TryGetStringField(TEXT("kind"), KindFilter);

			TArray<TSharedPtr<FJsonValue>> Rows;
			TMap<FString, int32> ByKind;
			for (const TPair<FString, FSololmcpObjectHandles::FEntry>& Pair : Handles.GetEntries())
			{
				const FSololmcpObjectHandles::FEntry& Entry = Pair.Value;
				ByKind.FindOrAdd(Entry.Kind)++;
				if (!KindFilter.IsEmpty() && Entry.Kind != KindFilter)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("handle"), Pair.Key);
				Row->SetStringField(TEXT("kind"), Entry.Kind);
				Row->SetStringField(TEXT("origin"), Entry.Origin);
				Row->SetStringField(TEXT("created_at"), Entry.CreatedAt.ToIso8601());
				if (const UObject* Object = Entry.Object.Get())
				{
					Row->SetStringField(TEXT("object_name"), Object->GetName());
					Row->SetStringField(TEXT("object_class"), Object->GetClass()->GetPathName());
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			TSharedRef<FJsonObject> KindCounts = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Pair : ByKind)
			{
				KindCounts->SetNumberField(Pair.Key, Pair.Value);
			}

			OutStructured->SetArrayField(TEXT("handles"), Rows);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("total_live"), Handles.GetEntries().Num());
			OutStructured->SetObjectField(TEXT("count_by_kind"), KindCounts);
			OutStructured->SetNumberField(TEXT("pruned_dead"), Pruned);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d live handle(s)%s."),
				Handles.GetEntries().Num(),
				Pruned > 0 ? *FString::Printf(TEXT("; pruned %d dead"), Pruned) : TEXT(""));
			return true;
		},
		nullptr,
		0
	});

	// ── handle_inspect ─────────────────────────────────────────────────────
	Registry.Register({
		TEXT("handle_inspect"),
		TEXT("Check whether a handle is still valid and what it points at. Use this before a queued "
			 "wave that reuses handles from an earlier wave, so a stale handle is caught at planning "
			 "time rather than mid-wave."),
		FSololmcpSchemaBuilder::Object(
			{{TEXT("handle"), FSololmcpSchemaBuilder::String(TEXT("Handle to inspect."))}},
			{TEXT("handle")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString Handle;
			if (!Args->TryGetStringField(TEXT("handle"), Handle) || Handle.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("handle"));
				OutError = TEXT("Missing handle.");
				return false;
			}

			const FSololmcpObjectHandles::FEntry* Entry = FSololmcpObjectHandles::Get().Find(Handle);
			OutStructured->SetStringField(TEXT("handle"), Handle);
			if (Entry == nullptr)
			{
				// Not an error: asking whether a handle is still good is the point of
				// this tool, and "no" is a useful answer rather than a failure.
				OutStructured->SetBoolField(TEXT("valid"), false);
				OutStructured->SetStringField(TEXT("reason"), TEXT("unknown_handle"));
				OutStructured->SetStringField(TEXT("suggestion"),
					TEXT("Handles are per editor session. Re-derive it from the tool that minted it."));
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = FString::Printf(TEXT("Handle '%s' is not live."), *Handle);
				return true;
			}

			const UObject* Object = Entry->Object.Get();
			OutStructured->SetBoolField(TEXT("valid"), Object != nullptr);
			OutStructured->SetStringField(TEXT("kind"), Entry->Kind);
			OutStructured->SetStringField(TEXT("origin"), Entry->Origin);
			OutStructured->SetStringField(TEXT("created_at"), Entry->CreatedAt.ToIso8601());
			if (Object != nullptr)
			{
				OutStructured->SetStringField(TEXT("object_name"), Object->GetName());
				OutStructured->SetStringField(TEXT("object_class"), Object->GetClass()->GetPathName());
			}
			else
			{
				OutStructured->SetStringField(TEXT("reason"), TEXT("object_destroyed"));
			}
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = Object != nullptr
				? FString::Printf(TEXT("Handle '%s' -> %s (%s)."), *Handle, *Object->GetName(), *Entry->Kind)
				: FString::Printf(TEXT("Handle '%s' is registered but its object was destroyed."), *Handle);
			return true;
		},
		nullptr,
		0
	});

	// ── handle_release ─────────────────────────────────────────────────────
	Registry.Register({
		TEXT("handle_release"),
		TEXT("Release handles so their objects can be garbage collected. Pass a handle, a kind to "
			 "release a whole family, or all=true to clear everything. Call this between queued "
			 "waves; handles held for the life of an editor session pin objects indefinitely."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("handle"), FSololmcpSchemaBuilder::String(TEXT("A single handle to release."))},
				{TEXT("kind"), FSololmcpSchemaBuilder::String(
					TEXT("Release every handle of this kind."))},
				{TEXT("all"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Release every live handle.")), false)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FSololmcpObjectHandles& Handles = FSololmcpObjectHandles::Get();

			bool bAll = false;
			Args->TryGetBoolField(TEXT("all"), bAll);
			FString Handle;
			Args->TryGetStringField(TEXT("handle"), Handle);
			FString Kind;
			Args->TryGetStringField(TEXT("kind"), Kind);

			if (!bAll && Handle.IsEmpty() && Kind.IsEmpty())
			{
				// Releasing everything is a bigger action than releasing one thing, so
				// it has to be asked for rather than being what an empty request does.
				SololmcpError::Set(OutStructured, TEXT("MISSING_PARAM"), TEXT("handle"),
					TEXT("Pass handle, kind, or all=true. An empty request does not release everything."));
				OutError = TEXT("Nothing specified to release.");
				return false;
			}

			int32 Released = 0;
			if (bAll)
			{
				Released = Handles.ReleaseKind(FString());
				OutStructured->SetStringField(TEXT("scope"), TEXT("all"));
			}
			else if (!Kind.IsEmpty())
			{
				Released = Handles.ReleaseKind(Kind);
				OutStructured->SetStringField(TEXT("scope"), TEXT("kind"));
				OutStructured->SetStringField(TEXT("kind"), Kind);
			}
			else
			{
				Released = Handles.Release(Handle) ? 1 : 0;
				OutStructured->SetStringField(TEXT("scope"), TEXT("handle"));
				OutStructured->SetStringField(TEXT("handle"), Handle);
			}

			OutStructured->SetNumberField(TEXT("released"), Released);
			OutStructured->SetNumberField(TEXT("remaining"), Handles.GetEntries().Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Released %d handle(s); %d still live."),
				Released, Handles.GetEntries().Num());
			return true;
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
