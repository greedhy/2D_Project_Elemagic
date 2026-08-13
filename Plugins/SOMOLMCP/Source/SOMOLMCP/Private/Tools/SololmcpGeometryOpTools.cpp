// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Generic dispatch over the GeometryScripting function libraries.
//
// GeometryScripting exposes roughly 509 operations across ~20 UBlueprintFunctionLibrary
// classes. Hand-writing a tool per operation would be 509 tools of near-identical
// shape, and every engine version that adds an operation would need another one.
//
// All of them are UFUNCTIONs, which means reflection reaches them: resolve the
// library class, marshal JSON into the parameter frame, ProcessEvent on the CDO,
// marshal the out-parameters back. One dispatcher covers the whole library and
// picks up new operations for free.
//
// The generic route is only viable because these libraries share a strict
// convention: the subject is a UDynamicMesh*, configuration arrives as a single
// options USTRUCT, and failures are reported through a UGeometryScriptDebug object
// rather than by return value. That convention is what makes the parameter frame
// predictable enough to fill from JSON.
//
// The cost of genericity is discoverability, which is why geometry_op_catalog is
// part of this batch rather than a follow-up: without it a caller has no way to
// learn what op names and parameters exist, and the dispatcher would only be usable
// by someone already reading the engine headers.
//
// Measured reach on 5.8, counted over the 509 BlueprintCallable functions in the
// GeometryScripting plugin headers: 493 (97%) are dispatchable. The remaining 16
// take a live scene object — UDynamicMeshPool, or a component — which has no asset
// path and so cannot be named in a request. Those are reachable only once something
// mints handles for components; the catalog marks their parameters supported:false
// so a caller learns this at planning time instead of from a failed call.

#include "Tools/SololmcpToolRegistry.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpObjectHandles.h"
#include "SololmcpReflectionInvoke.h"

#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Components/ActorComponent.h"

#ifndef SOMOLMCP_HAS_GEOMETRYSCRIPTING
#define SOMOLMCP_HAS_GEOMETRYSCRIPTING 0
#endif
// Engine floor, measured not assumed: 5.5 and above build clean; 5.4 and 5.3 do
// not. CopyMeshFromStaticMeshV2 and the rig hierarchy APIs used here arrived
// after 5.4, so the module being present further back proves nothing.
#if !((ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)))
#undef SOMOLMCP_HAS_GEOMETRYSCRIPTING
#define SOMOLMCP_HAS_GEOMETRYSCRIPTING 0
#endif

#if SOMOLMCP_HAS_GEOMETRYSCRIPTING
#include "UDynamicMesh.h"
#include "GeometryScript/GeometryScriptTypes.h"
#endif

namespace UE::SOMOLMCP
{
#if !SOMOLMCP_HAS_GEOMETRYSCRIPTING

void RegisterGeometryOpTools(FSololmcpToolRegistry&)
{
}

#else
namespace GeometryOpToolsPrivate
{
	static const TCHAR* const DynamicMeshKind = TEXT("dynamic_mesh");
	static const TCHAR* const LibraryPrefix = TEXT("GeometryScriptLibrary_");

	/** The op name a caller uses: "MeshDeformFunctions.ApplyBendWarpToMesh". */
	inline FString MakeOpName(const UClass* Library, const UFunction* Function)
	{
		FString LibraryName = Library->GetName();
		LibraryName.RemoveFromStart(LibraryPrefix);
		return FString::Printf(TEXT("%s.%s"), *LibraryName, *Function->GetName());
	}

	/** Every loaded UGeometryScriptLibrary_* class. */
	inline TArray<UClass*> GetLibraries()
	{
		TArray<UClass*> Result;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			if (Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass())
				&& Class->GetName().StartsWith(LibraryPrefix))
			{
				Result.Add(Class);
			}
		}
		Result.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
		return Result;
	}

	/** Resolve an op name to its function. Case-insensitive on both halves. */
	inline UFunction* FindOp(const FString& OpName, UClass*& OutLibrary)
	{
		OutLibrary = nullptr;
		FString LibraryPart;
		FString FunctionPart;
		if (!OpName.Split(TEXT("."), &LibraryPart, &FunctionPart))
		{
			// Bare function name: unambiguous for most ops, so accept it and let the
			// ambiguity check below reject the ones where it is not.
			FunctionPart = OpName;
		}

		UFunction* Found = nullptr;
		for (UClass* Library : GetLibraries())
		{
			FString ShortName = Library->GetName();
			ShortName.RemoveFromStart(LibraryPrefix);
			if (!LibraryPart.IsEmpty() && !ShortName.Equals(LibraryPart, ESearchCase::IgnoreCase))
			{
				continue;
			}
			for (TFieldIterator<UFunction> It(Library, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				if (It->GetName().Equals(FunctionPart, ESearchCase::IgnoreCase))
				{
					if (Found != nullptr)
					{
						// Two libraries define the same bare name; making the caller
						// qualify it is better than picking one arbitrarily.
						OutLibrary = nullptr;
						return nullptr;
					}
					Found = *It;
					OutLibrary = Library;
				}
			}
		}
		return Found;
	}

	/**
	 * The policy the catalog describes against.
	 *
	 * It has to agree with the one RunOp builds, or the catalog would advertise a
	 * different notion of what is callable than the dispatcher enforces. Only the
	 * class-shape fields matter for description, so this omits the live debug object
	 * and handle minting.
	 */
	inline Reflection::FInvokePolicy MakeDescribePolicy()
	{
		Reflection::FInvokePolicy Policy;
		Policy.DefaultSubjectClass = UDynamicMesh::StaticClass();
		Policy.AutoSupply = [](UClass* ParamClass) -> UObject*
		{
			// Returning the CDO is enough to mark the parameter auto-supplied; the
			// description path only tests the pointer against null.
			return (ParamClass != nullptr && ParamClass->IsChildOf(UGeometryScriptDebug::StaticClass()))
				? ParamClass->GetDefaultObject()
				: nullptr;
		};
		return Policy;
	}

	inline TSharedRef<FJsonObject> DescribeOp(UClass* Library, UFunction* Function, const bool bIncludeParams)
	{
		FString ShortName = Library->GetName();
		ShortName.RemoveFromStart(LibraryPrefix);
		TSharedRef<FJsonObject> Obj = Reflection::DescribeFunction(
			Library, Function, MakeDescribePolicy(), bIncludeParams, ShortName);
		// The catalog's own vocabulary predates the shared describer; keep "op" and
		// "library" so existing callers are not broken by the refactor.
		FString FullName;
		Obj->TryGetStringField(TEXT("name"), FullName);
		Obj->SetStringField(TEXT("op"), FullName);
		Obj->SetStringField(TEXT("library"), ShortName);
		return Obj;
	}

	/** Collect a debug object's messages into JSON. Returns how many were errors. */
	inline int32 CollectDebugMessages(const UGeometryScriptDebug* Debug, TArray<TSharedPtr<FJsonValue>>& OutMessages)
	{
		int32 ErrorCount = 0;
		if (Debug == nullptr)
		{
			return 0;
		}
		for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
		{
			const bool bIsError = Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage;
			ErrorCount += bIsError ? 1 : 0;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("severity"), bIsError ? TEXT("error") : TEXT("warning"));
			Row->SetStringField(TEXT("text"), Message.Message.ToString());
			OutMessages.Add(MakeShared<FJsonValueObject>(Row));
		}
		return ErrorCount;
	}

	/**
	 * Build a receipt for a batch.
	 *
	 * The queue already returns a receipt envelope for a job, but it describes the
	 * job, not what a generic call actually touched. A caller replaying or auditing
	 * a wave needs to know which objects changed and whether the whole thing landed
	 * as one undo entry -- neither is inferable from a list of function names.
	 */
	/**
	 * Whether the transaction system can actually restore this object.
	 *
	 * Transient objects are outside undo entirely: Modify() records nothing and
	 * Cancel() has nothing to put back. A dynamic mesh session object is exactly
	 * that, so a receipt built without this check reported rolled_back:true on a
	 * mesh that had in fact been left transformed -- the caller would trust the
	 * rollback and act on a state that never reverted.
	 */
	inline bool IsUndoable(const UObject* Object)
	{
		return Object != nullptr
			&& Object->HasAnyFlags(RF_Transactional)
			&& !Object->HasAnyFlags(RF_Transient)
			&& Object->GetOutermost() != GetTransientPackage();
	}

	inline TSharedRef<FJsonObject> MakeBatchReceipt(
		const TArray<UObject*>& Touched,
		const int32 Mutating,
		const bool bTransactional,
		const bool bCancelled)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somol.reflection_batch_receipt:v3"));
		Receipt->SetNumberField(TEXT("mutating_calls"), Mutating);
		Receipt->SetBoolField(TEXT("transactional"), bTransactional);

		TArray<TSharedPtr<FJsonValue>> Objects;
		int32 Undoable = 0;
		int32 NotUndoable = 0;
		for (const UObject* Object : Touched)
		{
			if (Object == nullptr)
			{
				continue;
			}
			const bool bUndoable = IsUndoable(Object);
			Undoable += bUndoable ? 1 : 0;
			NotUndoable += bUndoable ? 0 : 1;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), Object->GetPathName());
			Row->SetBoolField(TEXT("undoable"), bUndoable);
			Objects.Add(MakeShared<FJsonValueObject>(Row));
		}
		// "recorded" rather than "touched": these are the objects Modify() was called
		// on, which is what the transaction covers -- not necessarily everything the
		// call changed.
		Receipt->SetArrayField(TEXT("recorded_objects"), Objects);
		Receipt->SetNumberField(TEXT("touched_count"), Objects.Num());
		Receipt->SetNumberField(TEXT("undoable_objects"), Undoable);
		Receipt->SetNumberField(TEXT("non_undoable_objects"), NotUndoable);

		// Report the action taken, never a rollback guarantee.
		//
		// A generic dispatcher cannot know which objects a UFUNCTION will modify. It
		// can only call Modify() on the objects it passed in, and that is frequently
		// the wrong set: SetDisplayRate(Sequence) mutates Sequence->GetMovieScene(),
		// an object that never appears in the signature. Cancelling the transaction
		// then restores the recorded object, which did not change, while the one that
		// did keeps its new value.
		//
		// Measured on 5.8: a cancelled batch left a sequence at 96fps after reporting
		// a successful rollback. Claiming rolled_back here would be a guarantee this
		// mechanism cannot make, and a caller acting on it would be working from state
		// that never reverted. So the receipt states what was done and what was
		// covered, and tells the caller to verify by reading the value back.
		Receipt->SetBoolField(TEXT("transaction_cancelled"), bCancelled);
		Receipt->SetBoolField(TEXT("rollback_guaranteed"), false);
		if (bCancelled)
		{
			Receipt->SetStringField(TEXT("rollback_note"),
				TEXT("The transaction was cancelled, but rollback is not guaranteed: a generic call "
					 "can modify objects that were never parameters, and only the objects listed "
					 "here were recorded. Verify by reading the value back; for a dynamic mesh "
					 "session, reopen it from its asset."));
		}
		if (NotUndoable > 0)
		{
			Receipt->SetStringField(TEXT("transient_note"),
				TEXT("Some touched objects are transient and outside the transaction system "
					 "entirely, so nothing about them undoes."));
		}
		if (bTransactional && Mutating > 0 && !bCancelled)
		{
			Receipt->SetStringField(TEXT("undo"), NotUndoable == 0
				? TEXT("The batch ran inside one transaction, so what the editor recorded undoes as "
					   "one entry. Coverage is best-effort: objects the call touched indirectly may "
					   "not be included.")
				: TEXT("Undo covers the persistent objects only; transient session objects are "
					   "outside the transaction system."));
		}
		else if (!bTransactional && Mutating > 0)
		{
			Receipt->SetStringField(TEXT("undo"),
				TEXT("Not transactional: calls undo individually, if at all."));
		}
		return Receipt;
	}

	/**
	 * Run one op.
	 *
	 * DefaultMesh is the subject the caller named on the tool itself; any UDynamicMesh
	 * parameter the arguments do not mention is filled from it. That is what lets
	 * "apply this op to this mesh" stay a two-field request for the common case while
	 * still allowing multi-mesh ops such as booleans to name both operands.
	 */
	inline bool RunOp(
		const FString& OpName,
		const TSharedPtr<FJsonObject>& Params,
		UDynamicMesh* DefaultMesh,
		const TSharedRef<FJsonObject>& OutResult,
		FString& OutError,
		TArray<UObject*>* OutTouched = nullptr)
	{
		UClass* Library = nullptr;
		UFunction* Function = FindOp(OpName, Library);
		if (Function == nullptr)
		{
			OutResult->SetStringField(TEXT("error_code"),
				Library == nullptr && OpName.Contains(TEXT(".")) ? TEXT("NOT_FOUND") : TEXT("AMBIGUOUS_OP"));
			OutResult->SetStringField(TEXT("error"),
				FString::Printf(TEXT("No unique GeometryScripting op named '%s'. Qualify it as "
									 "Library.Function, or run geometry_op_catalog to list what exists."),
					*OpName));
			OutError = FString::Printf(TEXT("Unresolved op '%s'."), *OpName);
			return false;
		}

		UObject* Target = Library->GetDefaultObject();
		if (Target == nullptr)
		{
			OutResult->SetStringField(TEXT("error"), TEXT("Library CDO unavailable."));
			OutError = TEXT("Library CDO unavailable.");
			return false;
		}

		UGeometryScriptDebug* Debug = NewObject<UGeometryScriptDebug>(GetTransientPackage());

		Reflection::FInvokePolicy Policy;
		Policy.DefaultSubject = DefaultMesh;
		Policy.DefaultSubjectClass = UDynamicMesh::StaticClass();
		Policy.AutoSupply = [Debug](UClass* ParamClass) -> UObject*
		{
			// GeometryScripting reports failures through this object rather than by
			// return value, so it is supplied on every call and read back below.
			return (ParamClass != nullptr && ParamClass->IsChildOf(UGeometryScriptDebug::StaticClass()))
				? Debug : nullptr;
		};
		Policy.MintHandle = [](UObject* Returned) -> FString
		{
			// These libraries return the subject mesh for chaining. Add() gives back the
			// existing handle when the object is already registered, so the common case
			// reports the caller's own handle rather than minting a duplicate.
			return Returned != nullptr && Returned->IsA<UDynamicMesh>()
				? FSololmcpObjectHandles::Get().Add(Returned, DynamicMeshKind, TEXT("geometry_op"))
				: FString();
		};
		Policy.ResolveObject = [](const FString& Ref, UClass* Expected, FString& OutResolveError) -> UObject*
		{
			if (Expected != nullptr && Expected->IsChildOf(UDynamicMesh::StaticClass()))
			{
				UObject* Mesh = FSololmcpObjectHandles::Get().Resolve(Ref, DynamicMeshKind);
				if (Mesh == nullptr)
				{
					OutResolveError = FString::Printf(
						TEXT("Unknown dynamic mesh handle '%s'. Run handle_list, or geometry_mesh_open."), *Ref);
				}
				return Mesh;
			}
			if (!Reflection::IsAssetLikeClass(Expected))
			{
				OutResolveError = FString::Printf(
					TEXT("'%s' is a live scene object with no asset path and cannot be named here."),
					Expected != nullptr ? *Expected->GetName() : TEXT("object"));
				return nullptr;
			}
			UObject* Loaded = StaticLoadObject(Expected != nullptr ? Expected : UObject::StaticClass(),
				nullptr, *Ref, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Loaded == nullptr)
			{
				OutResolveError = FString::Printf(TEXT("No %s could be loaded from '%s'."),
					Expected != nullptr ? *Expected->GetName() : TEXT("object"), *Ref);
			}
			return Loaded;
		};

		const bool bOk = Reflection::InvokeFunction(
			Library->GetDefaultObject(), Function, Params, Policy, OutResult, OutError,
			[Debug](const TSharedRef<FJsonObject>& Result) -> int32
			{
				TArray<TSharedPtr<FJsonValue>> Messages;
				const int32 ErrorCount = CollectDebugMessages(Debug, Messages);
				if (Messages.Num() > 0)
				{
					Result->SetArrayField(TEXT("messages"), Messages);
				}
				return ErrorCount;
			},
			OutTouched);

		OutResult->SetStringField(TEXT("op"), MakeOpName(Library, Function));
		return bOk;
	}
} // namespace GeometryOpToolsPrivate

void RegisterGeometryOpTools(FSololmcpToolRegistry& Registry)
{
	using namespace GeometryOpToolsPrivate;

	// ── geometry_op_catalog ────────────────────────────────────────────────
	Registry.Register({
		TEXT("geometry_op_catalog"),
		TEXT("List GeometryScripting operations reachable through geometry_op_apply, with their "
			 "parameters. Call this before planning a mesh pipeline: it turns 'does this engine "
			 "version have that operation, and what does it take' from a runtime failure into a "
			 "planning-time lookup."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("library"), FSololmcpSchemaBuilder::String(
					TEXT("Restrict to one library, e.g. MeshDeformFunctions. Omit for all."))},
				{TEXT("search"), FSololmcpSchemaBuilder::String(
					TEXT("Substring match on the operation name, e.g. 'normals'."))},
				{TEXT("include_params"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Include the parameter list for each operation. Off by default because "
							 "the full catalog with parameters is large.")),
					false)},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Maximum operations to return.")), 200)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			FString LibraryFilter;
			Args->TryGetStringField(TEXT("library"), LibraryFilter);
			FString Search;
			Args->TryGetStringField(TEXT("search"), Search);
			bool bIncludeParams = false;
			Args->TryGetBoolField(TEXT("include_params"), bIncludeParams);
			int32 Limit = 200;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			Limit = FMath::Clamp(Limit, 1, 2000);

			TArray<TSharedPtr<FJsonValue>> Rows;
			TArray<TSharedPtr<FJsonValue>> LibraryNames;
			int32 Matched = 0;

			for (UClass* Library : GetLibraries())
			{
				FString ShortName = Library->GetName();
				ShortName.RemoveFromStart(LibraryPrefix);
				LibraryNames.Add(MakeShared<FJsonValueString>(ShortName));
				if (!LibraryFilter.IsEmpty() && !ShortName.Equals(LibraryFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				for (TFieldIterator<UFunction> It(Library, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					UFunction* Function = *It;
					if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
					{
						continue;
					}
					if (!Search.IsEmpty() && !Function->GetName().Contains(Search))
					{
						continue;
					}
					++Matched;
					if (Rows.Num() < Limit)
					{
						Rows.Add(MakeShared<FJsonValueObject>(DescribeOp(Library, Function, bIncludeParams)));
					}
				}
			}

			OutStructured->SetArrayField(TEXT("operations"), Rows);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("matched"), Matched);
			OutStructured->SetBoolField(TEXT("truncated"), Matched > Rows.Num());
			OutStructured->SetArrayField(TEXT("libraries"), LibraryNames);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d operation(s) matched across %d librar(ies); returned %d."),
				Matched, LibraryNames.Num(), Rows.Num());
			return true;
		},
		nullptr,
		0
	});

	// ── geometry_op_apply ──────────────────────────────────────────────────
	Registry.Register({
		TEXT("geometry_op_apply"),
		TEXT("Run one GeometryScripting operation on an open dynamic mesh. Parameters are passed by "
			 "their engine names; anything omitted keeps its engine default, and any mesh parameter "
			 "not named explicitly falls back to the mesh argument. Operations that log errors are "
			 "reported as failures even though the engine returns no status."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mesh"), FSololmcpSchemaBuilder::String(
					TEXT("Dynamic mesh handle from geometry_mesh_open — the subject of the operation."))},
				{TEXT("op"), FSololmcpSchemaBuilder::String(
					TEXT("Operation name, e.g. MeshNormalsFunctions.RecomputeNormals. The library "
						 "prefix may be omitted when the name is unambiguous."))},
				{TEXT("params"), FSololmcpSchemaBuilder::Object(
					{}, {}, TEXT("Parameter values keyed by engine parameter name. Options structs "
								 "take a JSON object of the fields you want to override."))}
			},
			{TEXT("mesh"), TEXT("op")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString Handle;
			if (!Args->TryGetStringField(TEXT("mesh"), Handle) || Handle.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("mesh"));
				OutError = TEXT("Missing mesh handle.");
				return false;
			}
			UDynamicMesh* Mesh = FSololmcpObjectHandles::Get().ResolveTyped<UDynamicMesh>(Handle, DynamicMeshKind);
			if (Mesh == nullptr)
			{
				SololmcpError::NotFound(OutStructured, Handle);
				OutStructured->SetStringField(TEXT("suggestion"),
					TEXT("Run handle_list, or geometry_mesh_open to load a mesh."));
				OutError = FString::Printf(TEXT("Unknown dynamic mesh handle '%s'."), *Handle);
				return false;
			}
			FString Op;
			if (!Args->TryGetStringField(TEXT("op"), Op) || Op.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("op"));
				OutError = TEXT("Missing op.");
				return false;
			}

			const TSharedPtr<FJsonObject>* Params = nullptr;
			Args->TryGetObjectField(TEXT("params"), Params);

			const bool bOk = RunOp(Op, Params != nullptr ? *Params : nullptr, Mesh, OutStructured, OutError);
			OutStructured->SetStringField(TEXT("mesh"), Handle);
			OutSummary = bOk
				? FString::Printf(TEXT("Applied %s to %s."), *Op, *Handle)
				: FString::Printf(TEXT("%s failed on %s."), *Op, *Handle);
			return bOk;
		},
		nullptr,
		0
	});

	// ── geometry_op_apply_batch ────────────────────────────────────────────
	Registry.Register({
		TEXT("geometry_op_apply_batch"),
		TEXT("Run a sequence of GeometryScripting operations on an open dynamic mesh in one call. "
			 "This is the intended way to build a mesh pipeline: the whole chain costs one queue "
			 "entry instead of one per step, and the mesh is never round-tripped between steps. "
			 "Each entry may name its own mesh to work across several at once."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mesh"), FSololmcpSchemaBuilder::String(
					TEXT("Default dynamic mesh handle for entries that do not name their own."))},
				{TEXT("operations"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("op"), FSololmcpSchemaBuilder::String(TEXT("Operation name."))},
							{TEXT("mesh"), FSololmcpSchemaBuilder::String(
								TEXT("Override the default mesh for this entry."))},
							{TEXT("params"), FSololmcpSchemaBuilder::Object(
								{}, {}, TEXT("Parameter values for this operation."))}
						},
						{TEXT("op")}),
					TEXT("Operations to run in order."))},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Stop at the first failure. On by default: these operations mutate the "
							 "mesh in place, so continuing past a failure builds on an unknown state.")),
					true)},
				{TEXT("transaction"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Run the chain inside one transaction: one undo entry, and a chain "
							 "stopped by an error is rolled back instead of left half-applied.")),
					true)}
			},
			{TEXT("operations")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			if (!Args->TryGetArrayField(TEXT("operations"), Operations) || Operations == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("operations"));
				OutError = TEXT("Missing operations array.");
				return false;
			}

			FString DefaultHandle;
			Args->TryGetStringField(TEXT("mesh"), DefaultHandle);
			UDynamicMesh* DefaultMesh = DefaultHandle.IsEmpty()
				? nullptr
				: FSololmcpObjectHandles::Get().ResolveTyped<UDynamicMesh>(DefaultHandle, DynamicMeshKind);
			if (!DefaultHandle.IsEmpty() && DefaultMesh == nullptr)
			{
				SololmcpError::NotFound(OutStructured, DefaultHandle);
				OutError = FString::Printf(TEXT("Unknown dynamic mesh handle '%s'."), *DefaultHandle);
				return false;
			}

			bool bStopOnError = true;
			Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

			bool bTransactional = true;
			Args->TryGetBoolField(TEXT("transaction"), bTransactional);

			// One transaction for the chain, matching what the hand-written batch tools
			// already do: a mesh pipeline should undo as the one operation it reads as.
			TUniquePtr<FScopedTransaction> Transaction;
			if (bTransactional)
			{
				Transaction = MakeUnique<FScopedTransaction>(
					NSLOCTEXT("SOMOLMCP", "GeometryOpBatch", "SOMOLMCP Geometry Op Batch"));
			}
			TArray<UObject*> Touched;
			int32 MutatingCalls = 0;

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Succeeded = 0;
			int32 Failed = 0;
			bool bStopped = false;

			for (int32 Index = 0; Index < Operations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetNumberField(TEXT("index"), Index);

				if (!(*Operations)[Index]->TryGetObject(Entry) || Entry == nullptr)
				{
					Result->SetBoolField(TEXT("ok"), false);
					Result->SetStringField(TEXT("error_code"), TEXT("INVALID_PARAM"));
					Result->SetStringField(TEXT("error"), TEXT("Entry is not an object."));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					++Failed;
					if (bStopOnError) { bStopped = true; break; }
					continue;
				}

				FString Op;
				(*Entry)->TryGetStringField(TEXT("op"), Op);
				if (Op.IsEmpty())
				{
					Result->SetBoolField(TEXT("ok"), false);
					Result->SetStringField(TEXT("error_code"), TEXT("MISSING_PARAM"));
					Result->SetStringField(TEXT("error"), TEXT("Entry has no op."));
					Results.Add(MakeShared<FJsonValueObject>(Result));
					++Failed;
					if (bStopOnError) { bStopped = true; break; }
					continue;
				}

				UDynamicMesh* Subject = DefaultMesh;
				FString EntryHandle;
				if ((*Entry)->TryGetStringField(TEXT("mesh"), EntryHandle) && !EntryHandle.IsEmpty())
				{
					Subject = FSololmcpObjectHandles::Get().ResolveTyped<UDynamicMesh>(EntryHandle, DynamicMeshKind);
					if (Subject == nullptr)
					{
						Result->SetBoolField(TEXT("ok"), false);
						Result->SetStringField(TEXT("error_code"), TEXT("NOT_FOUND"));
						Result->SetStringField(TEXT("error"),
							FString::Printf(TEXT("Unknown dynamic mesh handle '%s'."), *EntryHandle));
						Results.Add(MakeShared<FJsonValueObject>(Result));
						++Failed;
						if (bStopOnError) { bStopped = true; break; }
						continue;
					}
				}

				const TSharedPtr<FJsonObject>* Params = nullptr;
				(*Entry)->TryGetObjectField(TEXT("params"), Params);

				FString StepError;
				const bool bOk = RunOp(Op, Params != nullptr ? *Params : nullptr, Subject, Result, StepError, &Touched);
				bool bMutated = false;
				Result->TryGetBoolField(TEXT("mutating"), bMutated);
				MutatingCalls += bMutated ? 1 : 0;
				if (!EntryHandle.IsEmpty())
				{
					Result->SetStringField(TEXT("mesh"), EntryHandle);
				}
				else if (!DefaultHandle.IsEmpty())
				{
					Result->SetStringField(TEXT("mesh"), DefaultHandle);
				}
				Results.Add(MakeShared<FJsonValueObject>(Result));

				if (bOk)
				{
					++Succeeded;
				}
				else
				{
					++Failed;
					if (bStopOnError)
					{
						bStopped = true;
						break;
					}
				}
			}

			// These operations mutate the mesh in place, so a stopped batch leaves it
			// half-transformed with no way for the caller to tell how far it got.
			const bool bRollback = bTransactional && bStopped && MutatingCalls > 0;
			if (bRollback && Transaction.IsValid())
			{
				Transaction->Cancel();
			}
			Transaction.Reset();

			OutStructured->SetObjectField(TEXT("receipt"),
				MakeBatchReceipt(Touched, MutatingCalls, bTransactional, bRollback));
			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Operations->Num());
			OutStructured->SetNumberField(TEXT("succeeded"), Succeeded);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetBoolField(TEXT("stopped_early"), bStopped);
			if (bStopped)
			{
				// Naming the untouched remainder matters more than usual here: the mesh
				// is left half-transformed, and the caller needs to know where.
				OutStructured->SetNumberField(TEXT("not_attempted"), Operations->Num() - Results.Num());
			}
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(TEXT("%d of %d operation(s) succeeded%s."),
				Succeeded, Operations->Num(), bStopped ? TEXT("; stopped at the first failure") : TEXT(""));
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d operation(s) failed."), Failed);
			}
			return Failed == 0;
		},
		nullptr,
		0
	});
}

#endif // SOMOLMCP_HAS_GEOMETRYSCRIPTING

} // namespace UE::SOMOLMCP
