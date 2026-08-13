// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

// Session handles for live editor objects.
//
// A large part of the editor API is keyed by objects that have no stable address
// outside a running editor: UMovieSceneSection, UTickableConstraint,
// FMovieSceneBindingProxy, UInterchangeBaseNodeContainer, transient graphs. They
// are not assets, so there is no path to pass over the wire, and they are not
// values, so they cannot be serialized into a request. Roughly 70% of
// UControlRigSequencerEditorLibrary is unreachable for exactly this reason.
//
// The fix is the pattern already proven by the Interchange container tools: a tool
// that obtains such an object registers it here and returns a short handle; later
// tools accept the handle. TStrongObjectPtr keeps the object off the GC's reclaim
// list for as long as the client holds it, which is the whole point — a raw
// pointer stashed across MCP calls would dangle at the next collection.
//
// Handles are per editor session and deliberately not persisted: they name a live
// object, and a handle that outlived its editor would be a lie. Clients that need
// to survive a restart should address assets by path instead.
//
// Cost of the mechanism: every retained object is pinned. Tools that mint handles
// should document a release path, and long queued sessions should call the release
// tool between waves rather than accumulating handles for the life of the editor.

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::SOMOLMCP
{
	/**
	 * Process-wide registry of handle -> live UObject.
	 *
	 * Not thread-safe by design: every MCP tool that touches UObjects already runs
	 * on the game thread, and adding a lock here would imply a safety this registry
	 * cannot actually provide for the objects it holds.
	 */
	class FSololmcpObjectHandles
	{
	public:
		struct FEntry
		{
			TStrongObjectPtr<UObject> Object;
			/// What kind of thing this is, in the vocabulary of the owning tool family
			/// (for example "movie_scene_section"). Used for listing and for rejecting
			/// a handle passed to the wrong tool.
			FString Kind;
			/// Where it came from, so a stale handle can be re-derived by the caller.
			FString Origin;
			FDateTime CreatedAt;
		};

		static FSololmcpObjectHandles& Get()
		{
			static FSololmcpObjectHandles Instance;
			return Instance;
		}

		/** Register an object and return its handle. Returns empty for null. */
		FString Add(UObject* Object, const FString& Kind, const FString& Origin)
		{
			if (Object == nullptr)
			{
				return FString();
			}
			// Re-registering the same object returns the existing handle rather than
			// pinning it twice; a client that lists twice should not leak.
			for (const TPair<FString, FEntry>& Pair : Entries)
			{
				if (Pair.Value.Object.Get() == Object)
				{
					return Pair.Key;
				}
			}

			const FString Handle = FString::Printf(TEXT("h_%s_%s"),
				*Kind.Left(6).ToLower(),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10).ToLower());

			FEntry Entry;
			Entry.Object = TStrongObjectPtr<UObject>(Object);
			Entry.Kind = Kind;
			Entry.Origin = Origin;
			Entry.CreatedAt = FDateTime::UtcNow();
			Entries.Add(Handle, MoveTemp(Entry));
			return Handle;
		}

		/** Resolve a handle, optionally requiring a kind. Null when unknown or mismatched. */
		UObject* Resolve(const FString& Handle, const FString& ExpectedKind = FString()) const
		{
			const FEntry* Entry = Entries.Find(Handle);
			if (Entry == nullptr)
			{
				return nullptr;
			}
			if (!ExpectedKind.IsEmpty() && Entry->Kind != ExpectedKind)
			{
				return nullptr;
			}
			return Entry->Object.Get();
		}

		/** Typed convenience wrapper around Resolve. */
		template <typename T>
		T* ResolveTyped(const FString& Handle, const FString& ExpectedKind = FString()) const
		{
			return Cast<T>(Resolve(Handle, ExpectedKind));
		}

		const FEntry* Find(const FString& Handle) const
		{
			return Entries.Find(Handle);
		}

		bool Release(const FString& Handle)
		{
			return Entries.Remove(Handle) > 0;
		}

		/** Release every handle of a kind, or all handles when Kind is empty. */
		int32 ReleaseKind(const FString& Kind)
		{
			if (Kind.IsEmpty())
			{
				const int32 Count = Entries.Num();
				Entries.Reset();
				return Count;
			}
			TArray<FString> Doomed;
			for (const TPair<FString, FEntry>& Pair : Entries)
			{
				if (Pair.Value.Kind == Kind)
				{
					Doomed.Add(Pair.Key);
				}
			}
			for (const FString& Handle : Doomed)
			{
				Entries.Remove(Handle);
			}
			return Doomed.Num();
		}

		/** Drop handles whose object went away despite the strong pointer (torn-down
		 *  editor, forcibly destroyed object). Returns how many were dropped. */
		int32 PruneDead()
		{
			TArray<FString> Doomed;
			for (const TPair<FString, FEntry>& Pair : Entries)
			{
				if (!Pair.Value.Object.IsValid())
				{
					Doomed.Add(Pair.Key);
				}
			}
			for (const FString& Handle : Doomed)
			{
				Entries.Remove(Handle);
			}
			return Doomed.Num();
		}

		const TMap<FString, FEntry>& GetEntries() const
		{
			return Entries;
		}

	private:
		TMap<FString, FEntry> Entries;
	};
}
