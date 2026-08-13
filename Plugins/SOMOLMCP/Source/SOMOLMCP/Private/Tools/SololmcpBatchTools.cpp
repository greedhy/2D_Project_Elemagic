// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpBatchTools.cpp
//
// v3.10 Phase D — Batch APIs.
//
// Five tools that take an ARRAY of items and process them all inside ONE
// game-thread enter, instead of forcing N separate MCP round-trips. This
// eliminates the per-call dispatch overhead for tight loops on the client
// side (e.g. when a Python script iterates over many actors and reads /
// writes a couple of properties on each one).
//
// Tools registered here:
//   - actor_get_properties_batch   (read N properties on M actors)
//   - actor_set_properties_batch   (write N properties on M actors)
//   - asset_query_batch_paths      (asset-registry existence + class lookup)
//   - actor_set_visibility_batch   (toggle hidden flag on M actors)
//   - actor_spawn_batch_lite       (spawn M actors of given class + transform)
//
// Each tool:
//   - Refuses any batch with > 1000 items (clear error).
//   - Runs the entire batch on the game thread under a single enter.
//   - Reports per-item ok/error so a single bad input does NOT abort the run.
//   - Emits exactly ONE summary log line at the end (never per-item).
//
//
// Registry wiring (see NOTES.md alongside this file):
//   - Add a forward decl  `void RegisterBatchTools(FSololmcpToolRegistry&);`
//     to SololmcpToolRegistry.h.
//   - Add `RegisterBatchTools(*this);` to the FSololmcpToolRegistry ctor in
//     SololmcpToolRegistry.cpp, after the other Register*Tools calls.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SoftObjectPath.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

#include "Math/Vector.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"

#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPBatch, Log, All);

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────

static constexpr int32 kMaxBatchSize  = 1000;
static constexpr int32 kToolTimeoutSec = 30;

// ─────────────────────────────────────────────────────────────────────────
// File-static helpers
// ─────────────────────────────────────────────────────────────────────────

// Convert an FVector into a JSON array [x, y, z].
static TSharedRef<FJsonValue> Vec3Json(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return MakeShared<FJsonValueArray>(Arr);
}

// Convert an FRotator into a JSON array [pitch, yaw, roll].
static TSharedRef<FJsonValue> RotJson(const FRotator& R)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Add(MakeShared<FJsonValueNumber>(R.Pitch));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Yaw));
	Arr.Add(MakeShared<FJsonValueNumber>(R.Roll));
	return MakeShared<FJsonValueArray>(Arr);
}

// Pull a 3-element numeric array out of a JSON value (array of numbers).
// Returns false if the value isn't an array of length 3.
static bool TryReadVec3(const TSharedPtr<FJsonValue>& Val, FVector& Out)
{
	if (!Val.IsValid() || Val->Type != EJson::Array)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& A = Val->AsArray();
	if (A.Num() != 3)
	{
		return false;
	}
	Out = FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber());
	return true;
}

static bool TryReadRot(const TSharedPtr<FJsonValue>& Val, FRotator& Out)
{
	if (!Val.IsValid() || Val->Type != EJson::Array)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& A = Val->AsArray();
	if (A.Num() != 3)
	{
		return false;
	}
	Out = FRotator(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber());
	return true;
}

// Look up an actor in the editor world by display label or object name.
// Returns nullptr if no actor matches. We accept either match because the
// caller may be referring to a pretty-printed label or the internal name.
static AActor* LookupActorByName(UWorld* World, const FString& Name)
{
	if (!World || Name.IsEmpty())
	{
		return nullptr;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		if (Actor->GetActorLabel() == Name || Actor->GetName() == Name)
		{
			return Actor;
		}
	}
	return nullptr;
}

// Resolve the editor world (or null if not available).
static UWorld* GetEditorWorldSafe()
{
	if (!GEditor)
	{
		return nullptr;
	}
	return GEditor->GetEditorWorldContext().World();
}

// Build a per-item failure JSON object: { ..., "ok": false, "error": "..." }
// Caller adds the identifying field (name / path) before / after as needed.
static TSharedRef<FJsonObject> MakeFailEntry(const FString& Error)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), false);
	Obj->SetStringField(TEXT("error"), Error);
	return Obj;
}

static TSharedRef<FJsonObject> MakeOkEntry()
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), true);
	return Obj;
}

static FString BatchStatusFromCounts(const int32 NumOk, const int32 NumErr)
{
	return NumOk > 0 && NumErr > 0 ? TEXT("partial_success") : (NumErr > 0 ? TEXT("failed") : TEXT("success"));
}

static bool FinishOkErrBatch(
	const TCHAR* ToolName,
	const int32 NumOk,
	const int32 NumErr,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutError)
{
	OutStructured->SetStringField(TEXT("status"), BatchStatusFromCounts(NumOk, NumErr));
	if (NumOk == 0 && NumErr > 0)
	{
		OutError = FString::Printf(TEXT("%s failed all items (%d err)"), ToolName, NumErr);
		return false;
	}
	return true;
}

// Common preflight: must be on game thread; items array must exist; size cap.
static bool PreflightBatch(
	const TSharedRef<FJsonObject>& Arguments,
	const TCHAR* ItemsField,
	const TArray<TSharedPtr<FJsonValue>>*& OutItems,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Batch tool must be invoked on the game thread.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	if (!Arguments->TryGetArrayField(ItemsField, OutItems) || !OutItems)
	{
		OutError = FString::Printf(TEXT("Missing required argument: %s"), ItemsField);
		SololmcpError::MissingParam(OutStructured, ItemsField);
		return false;
	}

	if (OutItems->Num() > kMaxBatchSize)
	{
		OutError = FString::Printf(
			TEXT("Batch size %d exceeds limit of %d. Split into multiple calls."),
			OutItems->Num(), kMaxBatchSize);
		SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), ItemsField, OutError);
		return false;
	}

	return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Tool 1: actor_get_properties_batch
// ─────────────────────────────────────────────────────────────────────────

static bool Tool_ActorGetPropertiesBatch(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!PreflightBatch(Arguments, TEXT("items"), Items, OutStructured, OutError))
	{
		return false;
	}

	UWorld* World = GetEditorWorldSafe();
	if (!World)
	{
		OutError = TEXT("No editor world available.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 NumOk = 0;
	int32 NumErr = 0;

	for (const TSharedPtr<FJsonValue>& ItemVal : *Items)
	{
		if (!ItemVal.IsValid() || ItemVal->Type != EJson::Object)
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("item is not a JSON object"));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}
		const TSharedPtr<FJsonObject>& Item = ItemVal->AsObject();

		FString Name;
		if (!Item->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("missing 'name'"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* PropsArr = nullptr;
		if (!Item->TryGetArrayField(TEXT("properties"), PropsArr) || !PropsArr)
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("missing 'properties' array"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		AActor* Actor = LookupActorByName(World, Name);
		if (!Actor)
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("actor not found"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		TSharedRef<FJsonObject> PropsOut = MakeShared<FJsonObject>();
		for (const TSharedPtr<FJsonValue>& PropVal : *PropsArr)
		{
			if (!PropVal.IsValid() || PropVal->Type != EJson::String)
			{
				continue;
			}
			const FString PropName = PropVal->AsString();
			if (PropName == TEXT("location"))
			{
				PropsOut->SetField(PropName, Vec3Json(Actor->GetActorLocation()));
			}
			else if (PropName == TEXT("rotation"))
			{
				PropsOut->SetField(PropName, RotJson(Actor->GetActorRotation()));
			}
			else if (PropName == TEXT("scale"))
			{
				PropsOut->SetField(PropName, Vec3Json(Actor->GetActorScale3D()));
			}
			else if (PropName == TEXT("hidden"))
			{
				PropsOut->SetBoolField(PropName, Actor->IsHidden() || Actor->IsHiddenEd());
			}
			else if (PropName == TEXT("tags"))
			{
				TArray<TSharedPtr<FJsonValue>> TagsArr;
				for (const FName& Tag : Actor->Tags)
				{
					TagsArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				PropsOut->SetArrayField(PropName, TagsArr);
			}
			else
			{
				PropsOut->SetStringField(PropName, TEXT("not_supported"));
			}
		}

		TSharedRef<FJsonObject> Entry = MakeOkEntry();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetObjectField(TEXT("props"), PropsOut);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
		++NumOk;
	}

	OutStructured->SetArrayField(TEXT("results"), Results);
	OutStructured->SetNumberField(TEXT("count"), Results.Num());
	OutStructured->SetNumberField(TEXT("ok_count"), NumOk);
	OutStructured->SetNumberField(TEXT("error_count"), NumErr);

	OutSummary = FString::Printf(
		TEXT("actor_get_properties_batch: %d items (%d ok, %d err)"),
		Results.Num(), NumOk, NumErr);
	UE_LOG(LogSOMOLMCPBatch, Log, TEXT("%s"), *OutSummary);
	return FinishOkErrBatch(TEXT("actor_get_properties_batch"), NumOk, NumErr, OutStructured, OutError);
}

// ─────────────────────────────────────────────────────────────────────────
// Tool 2: actor_set_properties_batch
// ─────────────────────────────────────────────────────────────────────────

static bool Tool_ActorSetPropertiesBatch(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!PreflightBatch(Arguments, TEXT("items"), Items, OutStructured, OutError))
	{
		return false;
	}

	UWorld* World = GetEditorWorldSafe();
	if (!World)
	{
		OutError = TEXT("No editor world available.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 NumOk = 0;
	int32 NumErr = 0;

	for (const TSharedPtr<FJsonValue>& ItemVal : *Items)
	{
		if (!ItemVal.IsValid() || ItemVal->Type != EJson::Object)
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("item is not a JSON object"));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}
		const TSharedPtr<FJsonObject>& Item = ItemVal->AsObject();

		FString Name;
		if (!Item->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("missing 'name'"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
		if (!Item->TryGetObjectField(TEXT("properties"), PropsObjPtr) || !PropsObjPtr || !PropsObjPtr->IsValid())
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("missing 'properties' object"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}
		const TSharedPtr<FJsonObject>& PropsObj = *PropsObjPtr;

		AActor* Actor = LookupActorByName(World, Name);
		if (!Actor)
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("actor not found"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		bool bAnyChanged = false;
		FString PerItemErr;
		for (const auto& KV : PropsObj->Values)
		{
			const FString K(*KV.Key);
			const TSharedPtr<FJsonValue>& V = KV.Value;
			if (K == TEXT("location"))
			{
				FVector NewLoc;
				if (!TryReadVec3(V, NewLoc))
				{
					PerItemErr = TEXT("location must be [x,y,z]");
					break;
				}
				Actor->SetActorLocation(NewLoc);
				bAnyChanged = true;
			}
			else if (K == TEXT("rotation"))
			{
				FRotator NewRot;
				if (!TryReadRot(V, NewRot))
				{
					PerItemErr = TEXT("rotation must be [pitch,yaw,roll]");
					break;
				}
				Actor->SetActorRotation(NewRot);
				bAnyChanged = true;
			}
			else if (K == TEXT("scale"))
			{
				FVector NewScale;
				if (!TryReadVec3(V, NewScale))
				{
					PerItemErr = TEXT("scale must be [x,y,z]");
					break;
				}
				Actor->SetActorScale3D(NewScale);
				bAnyChanged = true;
			}
			else if (K == TEXT("hidden"))
			{
				if (!V.IsValid() || V->Type != EJson::Boolean)
				{
					PerItemErr = TEXT("hidden must be bool");
					break;
				}
				Actor->SetActorHiddenInGame(V->AsBool());
				bAnyChanged = true;
			}
			else if (K == TEXT("tags"))
			{
				if (!V.IsValid() || V->Type != EJson::Array)
				{
					PerItemErr = TEXT("tags must be array of strings");
					break;
				}
				TArray<FName> NewTags;
				bool bTagOk = true;
				for (const TSharedPtr<FJsonValue>& TagVal : V->AsArray())
				{
					if (!TagVal.IsValid() || TagVal->Type != EJson::String)
					{
						bTagOk = false;
						break;
					}
					NewTags.Add(FName(*TagVal->AsString()));
				}
				if (!bTagOk)
				{
					PerItemErr = TEXT("tags must be array of strings");
					break;
				}
				Actor->Tags = NewTags;
				bAnyChanged = true;
			}
			else
			{
				// Unknown property — record and keep going (do not abort the item).
				// But we DO want the caller to know we didn't apply it.
				if (PerItemErr.IsEmpty())
				{
					PerItemErr = FString::Printf(TEXT("not_supported: %s"), *K);
				}
			}
		}

		if (bAnyChanged)
		{
			Actor->MarkPackageDirty();
		}

		if (!PerItemErr.IsEmpty())
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(PerItemErr);
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
		}
		else
		{
			TSharedRef<FJsonObject> Entry = MakeOkEntry();
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumOk;
		}
	}

	OutStructured->SetArrayField(TEXT("results"), Results);
	OutStructured->SetNumberField(TEXT("count"), Results.Num());
	OutStructured->SetNumberField(TEXT("ok_count"), NumOk);
	OutStructured->SetNumberField(TEXT("error_count"), NumErr);

	OutSummary = FString::Printf(
		TEXT("actor_set_properties_batch: %d items (%d ok, %d err)"),
		Results.Num(), NumOk, NumErr);
	UE_LOG(LogSOMOLMCPBatch, Log, TEXT("%s"), *OutSummary);
	return FinishOkErrBatch(TEXT("actor_set_properties_batch"), NumOk, NumErr, OutStructured, OutError);
}

// ─────────────────────────────────────────────────────────────────────────
// Tool 3: asset_query_batch_paths
// ─────────────────────────────────────────────────────────────────────────

static bool Tool_AssetQueryBatchPaths(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Batch tool must be invoked on the game thread.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("paths"), Paths) || !Paths)
	{
		OutError = TEXT("Missing required argument: paths");
		SololmcpError::MissingParam(OutStructured, TEXT("paths"));
		return false;
	}

	if (Paths->Num() > kMaxBatchSize)
	{
		OutError = FString::Printf(
			TEXT("Batch size %d exceeds limit of %d. Split into multiple calls."),
			Paths->Num(), kMaxBatchSize);
		SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("paths"), OutError);
		return false;
	}

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 NumExist = 0;
	int32 NumMissing = 0;
	int32 NumInvalid = 0;

	for (const TSharedPtr<FJsonValue>& PathVal : *Paths)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		if (!PathVal.IsValid() || PathVal->Type != EJson::String)
		{
			Entry->SetStringField(TEXT("path"), TEXT(""));
			Entry->SetBoolField(TEXT("exists"), false);
			Entry->SetStringField(TEXT("error"), TEXT("path is not a string"));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumInvalid;
			continue;
		}
		const FString Path = PathVal->AsString();
		Entry->SetStringField(TEXT("path"), Path);

		const FSoftObjectPath ObjPath(Path);
		const FAssetData Data = AssetRegistry.GetAssetByObjectPath(ObjPath);

		if (Data.IsValid())
		{
			Entry->SetBoolField(TEXT("exists"), true);
			// UE 5.7 always has AssetClassPath (FAssetData::AssetClass deprecated since 5.1).
			Entry->SetStringField(TEXT("class"), Data.AssetClassPath.GetAssetName().ToString());
			++NumExist;
		}
		else
		{
			Entry->SetBoolField(TEXT("exists"), false);
			++NumMissing;
		}
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	OutStructured->SetArrayField(TEXT("results"), Results);
	OutStructured->SetNumberField(TEXT("count"), Results.Num());
	OutStructured->SetNumberField(TEXT("exists_count"), NumExist);
	OutStructured->SetNumberField(TEXT("missing_count"), NumMissing);
	OutStructured->SetNumberField(TEXT("invalid_count"), NumInvalid);
	OutStructured->SetStringField(TEXT("status"),
		NumInvalid > 0 && NumInvalid == Results.Num() ? TEXT("failed") :
		(NumInvalid > 0 ? TEXT("partial_success") : TEXT("success")));

	OutSummary = FString::Printf(
		TEXT("asset_query_batch_paths: %d items (%d exist, %d missing, %d invalid)"),
		Results.Num(), NumExist, NumMissing, NumInvalid);
	UE_LOG(LogSOMOLMCPBatch, Log, TEXT("%s"), *OutSummary);
	if (NumInvalid > 0 && NumInvalid == Results.Num())
	{
		OutError = FString::Printf(TEXT("asset_query_batch_paths failed all items (%d invalid)"), NumInvalid);
		return false;
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Tool 4: actor_set_visibility_batch
// ─────────────────────────────────────────────────────────────────────────

static bool Tool_ActorSetVisibilityBatch(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!PreflightBatch(Arguments, TEXT("items"), Items, OutStructured, OutError))
	{
		return false;
	}

	UWorld* World = GetEditorWorldSafe();
	if (!World)
	{
		OutError = TEXT("No editor world available.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 NumOk = 0;
	int32 NumErr = 0;

	for (const TSharedPtr<FJsonValue>& ItemVal : *Items)
	{
		if (!ItemVal.IsValid() || ItemVal->Type != EJson::Object)
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("item is not a JSON object"));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}
		const TSharedPtr<FJsonObject>& Item = ItemVal->AsObject();

		FString Name;
		if (!Item->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("missing 'name'"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		bool bHidden = false;
		if (!Item->TryGetBoolField(TEXT("hidden"), bHidden))
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("missing or non-bool 'hidden'"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		AActor* Actor = LookupActorByName(World, Name);
		if (!Actor)
		{
			TSharedRef<FJsonObject> Entry = MakeFailEntry(TEXT("actor not found"));
			Entry->SetStringField(TEXT("name"), Name);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			++NumErr;
			continue;
		}

		Actor->SetActorHiddenInGame(bHidden);
		Actor->MarkPackageDirty();

		TSharedRef<FJsonObject> Entry = MakeOkEntry();
		Entry->SetStringField(TEXT("name"), Name);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
		++NumOk;
	}

	OutStructured->SetArrayField(TEXT("results"), Results);
	OutStructured->SetNumberField(TEXT("count"), Results.Num());
	OutStructured->SetNumberField(TEXT("ok_count"), NumOk);
	OutStructured->SetNumberField(TEXT("error_count"), NumErr);

	OutSummary = FString::Printf(
		TEXT("actor_set_visibility_batch: %d items (%d ok, %d err)"),
		Results.Num(), NumOk, NumErr);
	UE_LOG(LogSOMOLMCPBatch, Log, TEXT("%s"), *OutSummary);
	return FinishOkErrBatch(TEXT("actor_set_visibility_batch"), NumOk, NumErr, OutStructured, OutError);
}

// ─────────────────────────────────────────────────────────────────────────
// Tool 5: actor_spawn_batch_lite
// ─────────────────────────────────────────────────────────────────────────

static bool Tool_ActorSpawnBatchLite(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!PreflightBatch(Arguments, TEXT("items"), Items, OutStructured, OutError))
	{
		return false;
	}

	UWorld* World = GetEditorWorldSafe();
	if (!World)
	{
		OutError = TEXT("No editor world available.");
		SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 NumOk = 0;
	int32 NumErr = 0;

	for (const TSharedPtr<FJsonValue>& ItemVal : *Items)
	{
		if (!ItemVal.IsValid() || ItemVal->Type != EJson::Object)
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeFailEntry(TEXT("item is not a JSON object"))));
			++NumErr;
			continue;
		}
		const TSharedPtr<FJsonObject>& Item = ItemVal->AsObject();

		FString ClassPath;
		if (!Item->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeFailEntry(TEXT("missing 'class_path'"))));
			++NumErr;
			continue;
		}

		// Optional fields with safe defaults.
		FVector  Loc(0, 0, 0);
		FRotator Rot(0, 0, 0);
		FString  DesiredName;

		if (const TSharedPtr<FJsonValue> LocVal = Item->TryGetField(TEXT("location")))
		{
			if (!TryReadVec3(LocVal, Loc))
			{
				Results.Add(MakeShared<FJsonValueObject>(MakeFailEntry(TEXT("location must be [x,y,z]"))));
				++NumErr;
				continue;
			}
		}
		if (const TSharedPtr<FJsonValue> RotVal = Item->TryGetField(TEXT("rotation")))
		{
			if (!TryReadRot(RotVal, Rot))
			{
				Results.Add(MakeShared<FJsonValueObject>(MakeFailEntry(TEXT("rotation must be [pitch,yaw,roll]"))));
				++NumErr;
				continue;
			}
		}
		Item->TryGetStringField(TEXT("name"), DesiredName);

		UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
		if (!ActorClass)
		{
			Results.Add(MakeShared<FJsonValueObject>(
				MakeFailEntry(FString::Printf(TEXT("could not load class '%s'"), *ClassPath))));
			++NumErr;
			continue;
		}

		FTransform Xform(Rot, Loc, FVector(1, 1, 1));
		FActorSpawnParameters SpawnParams;
		if (!DesiredName.IsEmpty())
		{
			SpawnParams.Name = FName(*DesiredName);
			SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		}
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		// SpawnActor non-templated overload: takes UClass*, transform pointer, params.
		// Returning AActor* directly — works for any UClass derived from AActor.
		AActor* NewActor = World->SpawnActor(ActorClass, &Xform, SpawnParams);
		if (!NewActor)
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeFailEntry(TEXT("SpawnActor returned null"))));
			++NumErr;
			continue;
		}

		if (!DesiredName.IsEmpty())
		{
			NewActor->SetActorLabel(DesiredName);
		}
		NewActor->MarkPackageDirty();

		TSharedRef<FJsonObject> Entry = MakeOkEntry();
		Entry->SetStringField(TEXT("name"), NewActor->GetActorLabel());
		Entry->SetStringField(TEXT("internal_name"), NewActor->GetName());
		Results.Add(MakeShared<FJsonValueObject>(Entry));
		++NumOk;
	}

	OutStructured->SetArrayField(TEXT("results"), Results);
	OutStructured->SetNumberField(TEXT("count"), Results.Num());
	OutStructured->SetNumberField(TEXT("ok_count"), NumOk);
	OutStructured->SetNumberField(TEXT("error_count"), NumErr);

	OutSummary = FString::Printf(
		TEXT("actor_spawn_batch_lite: %d items (%d ok, %d err)"),
		Results.Num(), NumOk, NumErr);
	UE_LOG(LogSOMOLMCPBatch, Log, TEXT("%s"), *OutSummary);
	return FinishOkErrBatch(TEXT("actor_spawn_batch_lite"), NumOk, NumErr, OutStructured, OutError);
}

// ─────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────

void RegisterBatchTools(FSololmcpToolRegistry& Registry)
{
	// 1. actor_get_properties_batch
	Registry.Register({
		TEXT("actor_get_properties_batch"),
		TEXT("Read multiple properties on multiple actors in a SINGLE game-thread enter. "
		     "Pass `items: [{name, properties: [\"location\"|\"rotation\"|\"scale\"|\"hidden\"|\"tags\", ...]}, ...]`. "
		     "Returns `results: [{name, ok, props?, error?}]`. Per-item failure does not abort the batch. "
		     "Max 1000 items per call. Unknown property names yield `\"not_supported\"` for that key."),
		FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("items"),
					FSololmcpSchemaBuilder::Array(
						FSololmcpSchemaBuilder::Object(
							{
								{ TEXT("name"),       FSololmcpSchemaBuilder::String(TEXT("Actor label or internal name.")) },
								{ TEXT("properties"), FSololmcpSchemaBuilder::Array(
									FSololmcpSchemaBuilder::String(TEXT("Property key (location|rotation|scale|hidden|tags).")),
									TEXT("List of property names to read.")) },
							},
							{ TEXT("name"), TEXT("properties") }),
						TEXT("Per-actor read requests.")) },
			},
			{ TEXT("items") }),
		&Tool_ActorGetPropertiesBatch,
		nullptr,
		kToolTimeoutSec
	});

	// 2. actor_set_properties_batch
	Registry.Register({
		TEXT("actor_set_properties_batch"),
		TEXT("Write multiple properties on multiple actors in a SINGLE game-thread enter. "
		     "Pass `items: [{name, properties: {location?: [x,y,z], rotation?: [p,y,r], scale?: [x,y,z], hidden?: bool, tags?: [str,...]}}, ...]`. "
		     "Returns `results: [{name, ok, error?}]`. Per-item failure does not abort the batch. "
		     "Marks each modified actor's package dirty. Max 1000 items per call."),
		FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("items"),
					FSololmcpSchemaBuilder::Array(
						FSololmcpSchemaBuilder::Object(
							{
								{ TEXT("name"),       FSololmcpSchemaBuilder::String(TEXT("Actor label or internal name.")) },
								{ TEXT("properties"), FSololmcpSchemaBuilder::Object({}, {},
									TEXT("Map of property name -> new value (location/rotation/scale/hidden/tags).")) },
							},
							{ TEXT("name"), TEXT("properties") }),
						TEXT("Per-actor write requests.")) },
			},
			{ TEXT("items") }),
		&Tool_ActorSetPropertiesBatch,
		nullptr,
		kToolTimeoutSec
	});

	// 3. asset_query_batch_paths
	Registry.Register({
		TEXT("asset_query_batch_paths"),
		TEXT("Asset-registry existence + class lookup for a batch of object paths in a SINGLE pass. "
		     "Pass `paths: [\"/Game/Foo\", ...]`. Returns `results: [{path, exists, class?, error?}]`. "
		     "Max 1000 paths per call."),
		FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("paths"),
					FSololmcpSchemaBuilder::Array(
						FSololmcpSchemaBuilder::String(TEXT("Asset object path (e.g. /Game/Foo).")),
						TEXT("Asset paths to look up.")) },
			},
			{ TEXT("paths") }),
		&Tool_AssetQueryBatchPaths,
		nullptr,
		kToolTimeoutSec
	});

	// 4. actor_set_visibility_batch
	Registry.Register({
		TEXT("actor_set_visibility_batch"),
		TEXT("Toggle the in-game hidden flag on multiple actors in a SINGLE game-thread enter. "
		     "Pass `items: [{name, hidden: bool}, ...]`. Returns `results: [{name, ok, error?}]`. "
		     "Marks each modified actor's package dirty. Max 1000 items per call."),
		FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("items"),
					FSololmcpSchemaBuilder::Array(
						FSololmcpSchemaBuilder::Object(
							{
								{ TEXT("name"),   FSololmcpSchemaBuilder::String(TEXT("Actor label or internal name.")) },
								{ TEXT("hidden"), FSololmcpSchemaBuilder::Boolean(TEXT("New hidden-in-game value.")) },
							},
							{ TEXT("name"), TEXT("hidden") }),
						TEXT("Per-actor visibility requests.")) },
			},
			{ TEXT("items") }),
		&Tool_ActorSetVisibilityBatch,
		nullptr,
		kToolTimeoutSec
	});

	// 5. actor_spawn_batch_lite
	Registry.Register({
		TEXT("actor_spawn_batch_lite"),
		TEXT("Spawn multiple actors in a SINGLE game-thread enter. "
		     "Pass `items: [{class_path: \"/Script/Engine.StaticMeshActor\", location?: [x,y,z], rotation?: [p,y,r], name?: \"...\"}, ...]`. "
		     "Returns `results: [{ok, name?, internal_name?, error?}]`. Per-item failure (e.g. bad class_path) does not abort the batch. "
		     "Max 1000 items per call."),
		FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("items"),
					FSololmcpSchemaBuilder::Array(
						FSololmcpSchemaBuilder::Object(
							{
								{ TEXT("class_path"), FSololmcpSchemaBuilder::String(TEXT("Full UE class path (e.g. /Script/Engine.StaticMeshActor).")) },
								{ TEXT("location"),   FSololmcpSchemaBuilder::Array(
									FSololmcpSchemaBuilder::Number(TEXT("Component value.")),
									TEXT("Optional [x,y,z]; default [0,0,0].")) },
								{ TEXT("rotation"),   FSololmcpSchemaBuilder::Array(
									FSololmcpSchemaBuilder::Number(TEXT("Component value.")),
									TEXT("Optional [pitch,yaw,roll]; default [0,0,0].")) },
								{ TEXT("name"),       FSololmcpSchemaBuilder::String(TEXT("Optional desired actor label / internal name.")) },
							},
							{ TEXT("class_path") }),
						TEXT("Per-actor spawn requests.")) },
			},
			{ TEXT("items") }),
		&Tool_ActorSpawnBatchLite,
		nullptr,
		kToolTimeoutSec
	});
}

} // namespace UE::SOMOLMCP
