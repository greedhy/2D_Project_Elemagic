// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 native Chaos Cache and Dataflow authoring through reflected engine types.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace ChaosDataflowAuthoring
{
static constexpr const TCHAR* ChaosCollectionClassPath = TEXT("/Script/ChaosCaching.ChaosCacheCollection");
static constexpr const TCHAR* ChaosCollectionFactoryPath = TEXT("/Script/ChaosCachingEditor.CacheCollectionFactory");
static constexpr const TCHAR* ChaosCacheClassPath = TEXT("/Script/ChaosCaching.ChaosCache");
static constexpr const TCHAR* ChaosManagerClassPath = TEXT("/Script/ChaosCaching.ChaosCacheManager");
static constexpr const TCHAR* DataflowClassPath = TEXT("/Script/DataflowEngine.Dataflow");
static constexpr const TCHAR* DataflowFactoryPath = TEXT("/Script/DataflowEditor.DataflowAssetFactory");
static constexpr const TCHAR* DataflowLibraryClassPath = TEXT("/Script/DataflowEngine.DataflowBlueprintLibrary");

static void Fail(TSharedRef<FJsonObject>& Out, FString& Error, const FString& Code,
	const FString& Message, const FString& Status = TEXT("failed"))
{
	Out->SetBoolField(TEXT("ok"), false);
	Out->SetStringField(TEXT("status"), Status);
	Out->SetStringField(TEXT("error_code"), Code);
	Out->SetStringField(TEXT("reason_code"), Code);
	Out->SetStringField(TEXT("message"), Message);
	Error = Message;
}

static FString ReceiptId(const FString& Prefix)
{
	return FString::Printf(TEXT("%s_%s"), *Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower());
}

static bool IsUE58OrLater()
{
	const FEngineVersion& Version = FEngineVersion::Current();
	return Version.GetMajor() > 5 || (Version.GetMajor() == 5 && Version.GetMinor() >= 8);
}

static UClass* ResolveNativeClass(const TCHAR* Path)
{
	if (UClass* Existing = FindObject<UClass>(nullptr, Path)) return Existing;
	return LoadObject<UClass>(nullptr, Path);
}

static bool RequireUE58(TSharedRef<FJsonObject>& Out, FString& Error)
{
	if (IsUE58OrLater()) return true;
	Fail(Out, Error, TEXT("ue58_required"), TEXT("This native Chaos/Dataflow authoring tool requires UE 5.8 or later."));
	return false;
}

static bool RequireConfirm(const TSharedRef<FJsonObject>& Args, const TCHAR* Field,
	TSharedRef<FJsonObject>& Out, FString& Error)
{
	bool bConfirmed = false;
	Args->TryGetBoolField(Field, bConfirmed);
	if (bConfirmed) return true;
	Fail(Out, Error, TEXT("blocked_requires_explicit_confirmation"),
		FString::Printf(TEXT("Dangerous or stateful operation requires %s=true."), Field), TEXT("blocked"));
	return false;
}

static bool SplitAssetPath(const FString& AssetPath, FString& PackagePath, FString& AssetName,
	TSharedRef<FJsonObject>& Out, FString& Error)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (!PackageName.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(PackageName))
	{
		Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("asset_path must be a valid package or object path under /Game/."));
		return false;
	}
	PackagePath = FPackageName::GetLongPackagePath(PackageName);
	AssetName = FPackageName::GetShortName(PackageName);
	return !PackagePath.IsEmpty() && !AssetName.IsEmpty();
}

static UObject* LoadTypedAsset(const FSololmcpToolExecutionContext& Context, const FString& AssetPath,
	const TCHAR* ExpectedClassPath, TSharedRef<FJsonObject>& Out, FString& Error)
{
	if (!AssetPath.StartsWith(TEXT("/Game/")))
	{
		Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("asset_path must be under /Game/."));
		return nullptr;
	}
	UClass* ExpectedClass = ResolveNativeClass(ExpectedClassPath);
	if (!ExpectedClass)
	{
		Fail(Out, Error, TEXT("native_class_unavailable"),
			FString::Printf(TEXT("Required UE 5.8 class is unavailable: %s"), ExpectedClassPath), TEXT("blocked"));
		return nullptr;
	}
	FString LoadError;
	UObject* Asset = Context.Services.LoadAsset(AssetPath, LoadError);
	if (!Asset || !Asset->IsA(ExpectedClass))
	{
		Fail(Out, Error, TEXT("asset_type_mismatch"), LoadError.IsEmpty()
			? FString::Printf(TEXT("%s is not a %s asset."), *AssetPath, ExpectedClassPath) : LoadError);
		return nullptr;
	}
	return Asset;
}

static bool SaveAndReadback(const FSololmcpToolExecutionContext& Context, UObject* Asset,
	TSharedRef<FJsonObject>& Out, FString& Error)
{
	if (!Asset || !Asset->GetOutermost() || !Asset->GetOutermost()->GetName().StartsWith(TEXT("/Game/")))
	{
		Fail(Out, Error, TEXT("non_persistent_asset"), TEXT("Mutation target is not a persistent /Game asset."));
		return false;
	}
	Asset->MarkPackageDirty();
	FString SaveError;
	if (!Context.Services.SaveAsset(Asset->GetPathName(), false, SaveError))
	{
		Fail(Out, Error, TEXT("asset_save_failed"), SaveError.IsEmpty() ? TEXT("Asset save failed.") : SaveError);
		return false;
	}
	FString ReadbackError;
	UObject* Readback = Context.Services.LoadAsset(Asset->GetPathName(), ReadbackError);
	if (!Readback || Readback->GetClass() != Asset->GetClass())
	{
		Fail(Out, Error, TEXT("asset_readback_failed"), ReadbackError.IsEmpty()
			? TEXT("Saved asset could not be read back with the expected class.") : ReadbackError);
		return false;
	}
	Out->SetBoolField(TEXT("saved"), true);
	Out->SetBoolField(TEXT("readback_verified"), true);
	Out->SetStringField(TEXT("asset_path"), Readback->GetPathName());
	Out->SetStringField(TEXT("asset_class"), Readback->GetClass()->GetPathName());
	return true;
}

static FString ExportProperty(UObject* Object, const FName Name)
{
	if (!Object) return FString();
	if (FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name))
	{
		FString Value;
		Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
		return Value;
	}
	return FString();
}

static bool SetEnumProperty(UObject* Object, const FName Name, const FString& Requested, FString& Error)
{
	FProperty* Property = Object ? FindFProperty<FProperty>(Object->GetClass(), Name) : nullptr;
	UEnum* Enum = nullptr;
	FNumericProperty* Underlying = nullptr;
	if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		Enum = EnumProperty->GetEnum();
		Underlying = EnumProperty->GetUnderlyingProperty();
	}
	else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		Enum = ByteProperty->Enum;
		Underlying = ByteProperty;
	}
	if (!Enum || !Underlying)
	{
		Error = FString::Printf(TEXT("Enum property '%s' is unavailable on %s."), *Name.ToString(),
			Object ? *Object->GetClass()->GetPathName() : TEXT("null"));
		return false;
	}
	int64 Value = Enum->GetValueByNameString(Requested, EGetByNameFlags::None);
	if (Value == INDEX_NONE)
	{
		Value = Enum->GetValueByName(FName(*Requested), EGetByNameFlags::None);
	}
	if (Value == INDEX_NONE)
	{
		Error = FString::Printf(TEXT("'%s' is not valid for enum %s."), *Requested, *Enum->GetPathName());
		return false;
	}
	Underlying->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Object), Value);
	return true;
}

static bool SetBoolProperty(UObject* Object, const FName Name, bool Value, FString& Error)
{
	FBoolProperty* Property = Object ? FindFProperty<FBoolProperty>(Object->GetClass(), Name) : nullptr;
	if (!Property)
	{
		Error = FString::Printf(TEXT("Boolean property '%s' is unavailable."), *Name.ToString());
		return false;
	}
	Property->SetPropertyValue_InContainer(Object, Value);
	return true;
}

static bool SetFloatProperty(UObject* Object, const FName Name, double Value, FString& Error)
{
	FNumericProperty* Property = Object ? FindFProperty<FNumericProperty>(Object->GetClass(), Name) : nullptr;
	if (!Property || !Property->IsFloatingPoint())
	{
		Error = FString::Printf(TEXT("Floating-point property '%s' is unavailable."), *Name.ToString());
		return false;
	}
	Property->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Object), Value);
	return true;
}

static bool SetObjectProperty(UObject* Object, const FName Name, UObject* Value, FString& Error)
{
	FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
	if (!Property || (Value && !Value->IsA(Property->PropertyClass)))
	{
		Error = FString::Printf(TEXT("Object property '%s' is unavailable or rejects %s."), *Name.ToString(),
			Value ? *Value->GetClass()->GetPathName() : TEXT("null"));
		return false;
	}
	Property->SetObjectPropertyValue_InContainer(Object, Value);
	return true;
}

static TSharedRef<FJsonObject> ChaosCollectionSnapshot(UObject* Collection)
{
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("asset_path"), Collection ? Collection->GetPathName() : FString());
	Snapshot->SetStringField(TEXT("interpolation_mode"), ExportProperty(Collection, TEXT("InterpolationMode")));
	TArray<TSharedPtr<FJsonValue>> Caches;
	if (Collection)
	{
		if (FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Collection->GetClass(), TEXT("Caches")))
		{
			FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Collection));
			FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
			for (int32 Index = 0; Inner && Index < Helper.Num(); ++Index)
			{
				UObject* Cache = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index));
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);
				Row->SetStringField(TEXT("name"), Cache ? Cache->GetName() : FString());
				Row->SetStringField(TEXT("path"), Cache ? Cache->GetPathName() : FString());
				Row->SetStringField(TEXT("class"), Cache ? Cache->GetClass()->GetPathName() : FString());
				Caches.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
	}
	Snapshot->SetNumberField(TEXT("cache_count"), Caches.Num());
	Snapshot->SetArrayField(TEXT("caches"), Caches);
	return Snapshot;
}

static TSharedRef<FJsonObject> DataflowSnapshot(UObject* Asset)
{
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("asset_path"), Asset ? Asset->GetPathName() : FString());
	Snapshot->SetStringField(TEXT("dataflow_type"), ExportProperty(Asset, TEXT("Type")));
	Snapshot->SetStringField(TEXT("reference_asset"), ExportProperty(Asset, TEXT("ReferenceAsset")));
	Snapshot->SetStringField(TEXT("variables"), ExportProperty(Asset, TEXT("Variables")));
	TArray<TSharedPtr<FJsonValue>> Nodes;
	int32 LinkCount = 0;
	if (UEdGraph* Graph = Cast<UEdGraph>(Asset))
	{
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Node->GetName());
			Row->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Row->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			Row->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			Row->SetNumberField(TEXT("pin_count"), Node->Pins.Num());
			for (const UEdGraphPin* Pin : Node->Pins) if (Pin) LinkCount += Pin->LinkedTo.Num();
			Nodes.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Snapshot->SetNumberField(TEXT("node_count"), Nodes.Num());
	Snapshot->SetNumberField(TEXT("connection_count"), LinkCount / 2);
	Snapshot->SetArrayField(TEXT("nodes"), Nodes);
	return Snapshot;
}

static bool SaveCurrentLevel(TSharedRef<FJsonObject>& Out, FString& Error)
{
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		Fail(Out, Error, TEXT("editor_world_unavailable"), TEXT("No active editor world is available."), TEXT("blocked"));
		return false;
	}
	if (!FEditorFileUtils::SaveCurrentLevel())
	{
		Fail(Out, Error, TEXT("level_save_failed"), TEXT("The current level could not be saved after Chaos Cache mutation."));
		return false;
	}
	UPackage* Package = GEditor->GetEditorWorldContext().World()->GetOutermost();
	Out->SetBoolField(TEXT("saved"), Package && !Package->IsDirty());
	Out->SetBoolField(TEXT("readback_verified"), Package && !Package->IsDirty());
	Out->SetStringField(TEXT("level_package"), Package ? Package->GetName() : FString());
	return Package && !Package->IsDirty();
}

static UPrimitiveComponent* FindPrimitiveComponent(AActor* Actor, const FString& ComponentName)
{
	if (!Actor) return nullptr;
	TInlineComponentArray<UPrimitiveComponent*> Components(Actor);
	for (UPrimitiveComponent* Component : Components)
	{
		if (Component && (ComponentName.IsEmpty() || Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)))
		{
			return Component;
		}
	}
	return nullptr;
}

static bool InvokeObservedComponentBind(UObject* Manager, UPrimitiveComponent* Component, const FName CacheName,
	bool bTransferSimulationFlag, FString& Error)
{
	UFunction* Function = Manager ? Manager->FindFunction(TEXT("FindOrAddObservedComponent")) : nullptr;
	if (!Function)
	{
		Error = TEXT("ChaosCacheManager.FindOrAddObservedComponent is unavailable.");
		return false;
	}
	FStructOnScope Params(Function);
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
		if (Property->GetFName() == TEXT("InComponent"))
		{
			CastFieldChecked<FObjectPropertyBase>(Property)->SetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), Component);
		}
		else if (Property->GetFName() == TEXT("CacheName"))
		{
			CastFieldChecked<FNameProperty>(Property)->SetPropertyValue(Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), CacheName);
		}
		else if (Property->GetFName() == TEXT("bTransferSimulationFlag"))
		{
			CastFieldChecked<FBoolProperty>(Property)->SetPropertyValue(Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), bTransferSimulationFlag);
		}
	}
	Manager->ProcessEvent(Function, Params.GetStructMemory());
	return true;
}

static bool InvokeDataflowEvaluate(UObject* Dataflow, UObject* Target, bool& bResult, FString& Error)
{
	UClass* LibraryClass = ResolveNativeClass(DataflowLibraryClassPath);
	UObject* Library = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
	UFunction* Function = Library ? Library->FindFunction(TEXT("EvaluateDataflow")) : nullptr;
	if (!Function)
	{
		Error = TEXT("UDataflowBlueprintLibrary.EvaluateDataflow is unavailable.");
		return false;
	}
	FStructOnScope Params(Function);
	FBoolProperty* ReturnProperty = nullptr;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			ReturnProperty = CastField<FBoolProperty>(Property);
		}
		else if (Property->GetFName() == TEXT("Dataflow"))
		{
			CastFieldChecked<FObjectPropertyBase>(Property)->SetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), Dataflow);
		}
		else if (Property->GetFName() == TEXT("AssetToUpdate"))
		{
			CastFieldChecked<FObjectPropertyBase>(Property)->SetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), Target);
		}
	}
	Library->ProcessEvent(Function, Params.GetStructMemory());
	bResult = ReturnProperty && ReturnProperty->GetPropertyValue(ReturnProperty->ContainerPtrToValuePtr<void>(Params.GetStructMemory()));
	return true;
}

static TSharedRef<FJsonObject> AssetPathSchema(const FString& Description)
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(Description, {}, 1, 1024)}
	}, {TEXT("asset_path")}, FString(), false);
}

static void MarkSuccess(TSharedRef<FJsonObject>& Out, const FString& Tool, UObject* Target, bool bMutation)
{
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("status"), TEXT("succeeded"));
	Out->SetStringField(TEXT("tool"), Tool);
	Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Out->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	if (Target) Out->SetStringField(TEXT("target"), Target->GetPathName());
	Out->SetBoolField(TEXT("mutation_applied"), bMutation);
	if (bMutation) Out->SetStringField(TEXT("receipt_id"), ReceiptId(Tool));
}
}

void RegisterChaosDataflowAuthoringTools(FSololmcpToolRegistry& Registry)
{
// Capability, not version: this needs the module, and the module ships on
// engines below 5.8 too. Whether the API matches is what the build decides.
#ifndef SOMOLMCP_HAS_DATAFLOWCORE
#define SOMOLMCP_HAS_DATAFLOWCORE 0
#endif
#define SOMOLMCP_CHAOSDF_AVAILABLE (SOMOLMCP_HAS_DATAFLOWCORE)

#if SOMOLMCP_CHAOSDF_AVAILABLE
	using namespace ChaosDataflowAuthoring;

	Registry.Register({TEXT("chaos_cache_collection_native_create"),
		TEXT("Create a native UE 5.8 Chaos Cache Collection without replacing existing content; save and read it back."),
		AssetPathSchema(TEXT("New Chaos Cache Collection package path under /Game/.")),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!RequireUE58(Out, Error)) return false;
			FString PackagePath, AssetName;
			if (!SplitAssetPath(Args->GetStringField(TEXT("asset_path")), PackagePath, AssetName, Out, Error)) return false;
			UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, ChaosCollectionClassPath,
				ChaosCollectionFactoryPath, nullptr, Error, false);
			if (!Asset)
			{
				Fail(Out, Error, TEXT("chaos_cache_collection_create_failed"), Error);
				return false;
			}
			if (!SaveAndReadback(Context, Asset, Out, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_cache_collection_native_create"), Asset, true);
			Out->SetObjectField(TEXT("collection_readback"), ChaosCollectionSnapshot(Asset));
			Summary = FString::Printf(TEXT("Created native Chaos Cache Collection %s."), *Asset->GetPathName());
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_cache_collection_native_inspect"),
		TEXT("Inspect a UE 5.8 Chaos Cache Collection, interpolation policy, embedded caches, and native class identity."),
		AssetPathSchema(TEXT("Chaos Cache Collection path under /Game/.")),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!RequireUE58(Out, Error)) return false;
			UObject* Asset = LoadTypedAsset(Context, Args->GetStringField(TEXT("asset_path")), ChaosCollectionClassPath, Out, Error);
			if (!Asset) return false;
			MarkSuccess(Out, TEXT("chaos_cache_collection_native_inspect"), Asset, false);
			Out->SetObjectField(TEXT("collection_readback"), ChaosCollectionSnapshot(Asset));
			Summary = FString::Printf(TEXT("Inspected native Chaos Cache Collection %s."), *Asset->GetPathName());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("chaos_cache_entry_native_ensure"),
		TEXT("Ensure one named native UChaosCache exists inside a collection, then save and verify the embedded-object readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Chaos Cache Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("cache_name"), FSololmcpSchemaBuilder::String(TEXT("Unique embedded cache name."), {}, 1, 128, TEXT("^[A-Za-z][A-Za-z0-9_]*$"))}
		}, {TEXT("asset_path"), TEXT("cache_name")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UObject* Collection = LoadTypedAsset(Context, Args->GetStringField(TEXT("asset_path")), ChaosCollectionClassPath, Out, Error);
			if (!Collection) return false;
			UClass* CacheClass = ResolveNativeClass(ChaosCacheClassPath);
			FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Collection->GetClass(), TEXT("Caches"));
			FObjectPropertyBase* Inner = ArrayProperty ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
			if (!CacheClass || !ArrayProperty || !Inner)
			{
				Fail(Out, Error, TEXT("chaos_cache_native_api_unavailable"), TEXT("Chaos cache class or collection Caches array is unavailable."), TEXT("blocked"));
				return false;
			}
			const FName CacheName(*Args->GetStringField(TEXT("cache_name")));
			FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Collection));
			UObject* Cache = nullptr;
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				UObject* Candidate = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index));
				if (Candidate && Candidate->GetFName() == CacheName) { Cache = Candidate; break; }
			}
			bool bCreated = false;
			if (!Cache)
			{
				FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "EnsureChaosCache", "SOMOLMCP Ensure Chaos Cache"));
				Collection->Modify();
				Cache = NewObject<UObject>(Collection, CacheClass, CacheName, RF_Transactional);
				const int32 NewIndex = Helper.AddValue();
				Inner->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), Cache);
				bCreated = true;
			}
			if (!SaveAndReadback(Context, Collection, Out, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_cache_entry_native_ensure"), Collection, bCreated);
			Out->SetStringField(TEXT("cache_name"), Cache->GetName());
			Out->SetStringField(TEXT("cache_path"), Cache->GetPathName());
			Out->SetBoolField(TEXT("created"), bCreated);
			Out->SetObjectField(TEXT("collection_readback"), ChaosCollectionSnapshot(Collection));
			Summary = FString::Printf(TEXT("Chaos cache '%s' %s in %s."), *Cache->GetName(), bCreated ? TEXT("created") : TEXT("already existed"), *Collection->GetPathName());
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_cache_interpolation_native_set"),
		TEXT("Set a Chaos Cache Collection interpolation mode using its native reflected enum; save and verify readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Chaos Cache Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("interpolation_mode"), FSololmcpSchemaBuilder::String(TEXT("Native EChaosCacheInterpolationMode value, such as QuatInterp or DualQuatInterp."), {}, 1, 64)}
		}, {TEXT("asset_path"), TEXT("interpolation_mode")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UObject* Asset = LoadTypedAsset(Context, Args->GetStringField(TEXT("asset_path")), ChaosCollectionClassPath, Out, Error);
			if (!Asset) return false;
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SetChaosCacheInterpolation", "SOMOLMCP Set Chaos Cache Interpolation"));
			Asset->Modify();
			if (!SetEnumProperty(Asset, TEXT("InterpolationMode"), Args->GetStringField(TEXT("interpolation_mode")), Error))
			{
				Transaction.Cancel(); Fail(Out, Error, TEXT("invalid_interpolation_mode"), Error); return false;
			}
			if (!SaveAndReadback(Context, Asset, Out, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_cache_interpolation_native_set"), Asset, true);
			Out->SetStringField(TEXT("interpolation_mode_readback"), ExportProperty(Asset, TEXT("InterpolationMode")));
			Summary = FString::Printf(TEXT("Updated Chaos cache interpolation for %s."), *Asset->GetPathName());
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_cache_manager_native_bind"),
		TEXT("Bind a Chaos Cache Collection to an explicitly targeted ChaosCacheManager Actor; save the level and read back the binding."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("manager_actor"), FSololmcpSchemaBuilder::String(TEXT("Unique ChaosCacheManager actor label, name, or path."), {}, 1, 1024)},
			{TEXT("collection_path"), FSololmcpSchemaBuilder::String(TEXT("Chaos Cache Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("confirm_write"), FSololmcpSchemaBuilder::Boolean(TEXT("Required explicit world-write confirmation."))}
		}, {TEXT("manager_actor"), TEXT("collection_path"), TEXT("confirm_write")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!RequireConfirm(Args, TEXT("confirm_write"), Out, Error)) return false;
			UObject* Collection = LoadTypedAsset(Context, Args->GetStringField(TEXT("collection_path")), ChaosCollectionClassPath, Out, Error);
			if (!Collection) return false;
			AActor* Manager = Context.Services.FindActorByLabelOrName(Args->GetStringField(TEXT("manager_actor")), Error);
			UClass* ManagerClass = ResolveNativeClass(ChaosManagerClassPath);
			if (!Manager || !ManagerClass || !Manager->IsA(ManagerClass))
			{
				Fail(Out, Error, TEXT("chaos_cache_manager_not_found"), Error.IsEmpty() ? TEXT("Target actor is not a ChaosCacheManager.") : Error); return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BindChaosCacheCollection", "SOMOLMCP Bind Chaos Cache Collection"));
			Manager->Modify();
			if (!SetObjectProperty(Manager, TEXT("CacheCollection"), Collection, Error))
			{
				Transaction.Cancel(); Fail(Out, Error, TEXT("cache_collection_bind_failed"), Error); return false;
			}
			Manager->PostEditChange();
			if (!SaveCurrentLevel(Out, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_cache_manager_native_bind"), Manager, true);
			Out->SetStringField(TEXT("collection_readback"), ExportProperty(Manager, TEXT("CacheCollection")));
			Summary = FString::Printf(TEXT("Bound %s to %s."), *Collection->GetPathName(), *Manager->GetActorLabel());
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_cache_manager_native_configure"),
		TEXT("Configure native ChaosCacheManager mode, start policy, and time with explicit confirmation and level readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("manager_actor"), FSololmcpSchemaBuilder::String(TEXT("Unique ChaosCacheManager actor label, name, or path."), {}, 1, 1024)},
			{TEXT("cache_mode"), FSololmcpSchemaBuilder::String(TEXT("None, Play, or Record."), {}, 1, 32)},
			{TEXT("start_mode"), FSololmcpSchemaBuilder::String(TEXT("Timed or Triggered."), {}, 1, 32)},
			{TEXT("start_time"), FSololmcpSchemaBuilder::Number(TEXT("Non-negative cache start time in seconds."), 0.0, 86400.0)},
			{TEXT("start_on_begin_play"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("confirm_write"), FSololmcpSchemaBuilder::Boolean(TEXT("Required explicit world-write confirmation."))}
		}, {TEXT("manager_actor"), TEXT("cache_mode"), TEXT("start_mode"), TEXT("confirm_write")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!RequireConfirm(Args, TEXT("confirm_write"), Out, Error)) return false;
			AActor* Manager = Context.Services.FindActorByLabelOrName(Args->GetStringField(TEXT("manager_actor")), Error);
			UClass* ManagerClass = ResolveNativeClass(ChaosManagerClassPath);
			if (!Manager || !ManagerClass || !Manager->IsA(ManagerClass))
			{
				Fail(Out, Error, TEXT("chaos_cache_manager_not_found"), Error.IsEmpty() ? TEXT("Target actor is not a ChaosCacheManager.") : Error); return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ConfigureChaosCacheManager", "SOMOLMCP Configure Chaos Cache Manager"));
			Manager->Modify();
			if (!SetEnumProperty(Manager, TEXT("CacheMode"), Args->GetStringField(TEXT("cache_mode")), Error) ||
				!SetEnumProperty(Manager, TEXT("StartMode"), Args->GetStringField(TEXT("start_mode")), Error) ||
				(Args->HasField(TEXT("start_time")) && !SetFloatProperty(Manager, TEXT("StartTime"), Args->GetNumberField(TEXT("start_time")), Error)) ||
				(Args->HasField(TEXT("start_on_begin_play")) && !SetBoolProperty(Manager, TEXT("bStartOnBeginPlay"), Args->GetBoolField(TEXT("start_on_begin_play")), Error)))
			{
				Transaction.Cancel(); Fail(Out, Error, TEXT("chaos_cache_manager_configure_failed"), Error); return false;
			}
			Manager->PostEditChange();
			if (!SaveCurrentLevel(Out, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_cache_manager_native_configure"), Manager, true);
			Out->SetStringField(TEXT("cache_mode_readback"), ExportProperty(Manager, TEXT("CacheMode")));
			Out->SetStringField(TEXT("start_mode_readback"), ExportProperty(Manager, TEXT("StartMode")));
			Out->SetStringField(TEXT("start_time_readback"), ExportProperty(Manager, TEXT("StartTime")));
			Out->SetStringField(TEXT("start_on_begin_play_readback"), ExportProperty(Manager, TEXT("bStartOnBeginPlay")));
			Summary = FString::Printf(TEXT("Configured ChaosCacheManager %s."), *Manager->GetActorLabel());
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_cache_observed_component_native_bind"),
		TEXT("Bind one explicitly targeted primitive component to a ChaosCacheManager through the native BlueprintCallable API."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("manager_actor"), FSololmcpSchemaBuilder::String(TEXT("Unique ChaosCacheManager actor label, name, or path."), {}, 1, 1024)},
			{TEXT("component_actor"), FSololmcpSchemaBuilder::String(TEXT("Unique owner actor label, name, or path."), {}, 1, 1024)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional primitive component object name."))},
			{TEXT("cache_name"), FSololmcpSchemaBuilder::String(TEXT("Cache key for the observed component."), {}, 1, 128, TEXT("^[A-Za-z][A-Za-z0-9_]*$"))},
			{TEXT("transfer_simulation_flag"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("confirm_write"), FSololmcpSchemaBuilder::Boolean(TEXT("Required explicit world-write confirmation."))}
		}, {TEXT("manager_actor"), TEXT("component_actor"), TEXT("cache_name"), TEXT("confirm_write")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!RequireConfirm(Args, TEXT("confirm_write"), Out, Error)) return false;
			AActor* Manager = Context.Services.FindActorByLabelOrName(Args->GetStringField(TEXT("manager_actor")), Error);
			UClass* ManagerClass = ResolveNativeClass(ChaosManagerClassPath);
			if (!Manager || !ManagerClass || !Manager->IsA(ManagerClass))
			{
				Fail(Out, Error, TEXT("chaos_cache_manager_not_found"), Error.IsEmpty() ? TEXT("Target actor is not a ChaosCacheManager.") : Error); return false;
			}
			AActor* Owner = Context.Services.FindActorByLabelOrName(Args->GetStringField(TEXT("component_actor")), Error);
			FString ComponentName; Args->TryGetStringField(TEXT("component_name"), ComponentName);
			UPrimitiveComponent* Component = FindPrimitiveComponent(Owner, ComponentName);
			if (!Owner || !Component)
			{
				Fail(Out, Error, TEXT("primitive_component_not_found"), TEXT("No matching primitive component was found on the explicitly targeted actor.")); return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BindChaosObservedComponent", "SOMOLMCP Bind Chaos Observed Component"));
			Manager->Modify(); Component->Modify();
			const bool bTransfer = Args->HasField(TEXT("transfer_simulation_flag")) && Args->GetBoolField(TEXT("transfer_simulation_flag"));
			if (!InvokeObservedComponentBind(Manager, Component, FName(*Args->GetStringField(TEXT("cache_name"))), bTransfer, Error))
			{
				Transaction.Cancel(); Fail(Out, Error, TEXT("observed_component_bind_failed"), Error); return false;
			}
			if (!SaveCurrentLevel(Out, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_cache_observed_component_native_bind"), Manager, true);
			Out->SetStringField(TEXT("component_path"), Component->GetPathName());
			Out->SetStringField(TEXT("cache_name"), Args->GetStringField(TEXT("cache_name")));
			Out->SetStringField(TEXT("observed_components_readback"), ExportProperty(Manager, TEXT("ObservedComponents")));
			Summary = FString::Printf(TEXT("Bound %s to Chaos cache '%s'."), *Component->GetPathName(), *Args->GetStringField(TEXT("cache_name")));
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_cache_authoring_receipt_validate"),
		TEXT("Validate saved/read-back evidence from a native Chaos Cache authoring operation."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("receipt_id"), FSololmcpSchemaBuilder::String({}, {}, 1, 160)},
			{TEXT("status"), FSololmcpSchemaBuilder::String({}, {}, 1, 32)},
			{TEXT("mutation_applied"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("saved"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("readback_verified"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("target"), FSololmcpSchemaBuilder::String({}, {}, 1, 1024)}
		}, {TEXT("receipt_id"), TEXT("status"), TEXT("mutation_applied"), TEXT("saved"), TEXT("readback_verified"), TEXT("target")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const FString Status = Args->GetStringField(TEXT("status"));
			const bool bValid = (Status == TEXT("succeeded") || Status == TEXT("completed")) && Args->GetBoolField(TEXT("mutation_applied")) &&
				Args->GetBoolField(TEXT("saved")) && Args->GetBoolField(TEXT("readback_verified")) && !Args->GetStringField(TEXT("target")).IsEmpty();
			if (!bValid) { Fail(Out, Error, TEXT("chaos_cache_receipt_rejected"), TEXT("Receipt does not prove a saved, read-back native Chaos Cache mutation.")); return false; }
			Out->SetBoolField(TEXT("ok"), true); Out->SetBoolField(TEXT("valid"), true); Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), Args->GetStringField(TEXT("receipt_id")));
			Summary = TEXT("Native Chaos Cache authoring receipt accepted."); return true;
		}, nullptr, 15});

	Registry.Register({TEXT("chaos_dataflow_asset_native_create"),
		TEXT("Create and configure a native UE 5.8 Dataflow asset without replacement; save and verify graph readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("New Dataflow package path under /Game/."), {}, 1, 1024)},
			{TEXT("dataflow_type"), FSololmcpSchemaBuilder::String(TEXT("Construction or Simulation."), {}, 1, 32)},
			{TEXT("reference_asset"), FSololmcpSchemaBuilder::String(TEXT("Optional /Game asset used as Dataflow reference authority."))}
		}, {TEXT("asset_path"), TEXT("dataflow_type")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!RequireUE58(Out, Error)) return false;
			FString PackagePath, AssetName;
			if (!SplitAssetPath(Args->GetStringField(TEXT("asset_path")), PackagePath, AssetName, Out, Error)) return false;
			UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, DataflowClassPath, DataflowFactoryPath, nullptr, Error, false);
			if (!Asset) { Fail(Out, Error, TEXT("dataflow_asset_create_failed"), Error); return false; }
			if (!SetEnumProperty(Asset, TEXT("Type"), Args->GetStringField(TEXT("dataflow_type")), Error))
			{
				Fail(Out, Error, TEXT("dataflow_type_invalid"), Error); return false;
			}
			FString ReferencePath;
			if (Args->TryGetStringField(TEXT("reference_asset"), ReferencePath) && !ReferencePath.IsEmpty())
			{
				if (!ReferencePath.StartsWith(TEXT("/Game/"))) { Fail(Out, Error, TEXT("invalid_reference_asset"), TEXT("reference_asset must be under /Game/.")); return false; }
				FString LoadError; UObject* Reference = Context.Services.LoadAsset(ReferencePath, LoadError);
				if (!Reference || !SetObjectProperty(Asset, TEXT("ReferenceAsset"), Reference, Error))
				{
					Fail(Out, Error, TEXT("dataflow_reference_bind_failed"), LoadError.IsEmpty() ? Error : LoadError); return false;
				}
			}
			Asset->PostEditChange();
			if (!SaveAndReadback(Context, Asset, Out, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_dataflow_asset_native_create"), Asset, true);
			Out->SetObjectField(TEXT("graph_readback"), DataflowSnapshot(Asset));
			Summary = FString::Printf(TEXT("Created native Dataflow asset %s."), *Asset->GetPathName()); return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_dataflow_asset_native_inspect"),
		TEXT("Inspect native Dataflow type, reference, variables, graph nodes, pins, and connection counts."),
		AssetPathSchema(TEXT("Dataflow asset path under /Game/.")),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UObject* Asset = LoadTypedAsset(Context, Args->GetStringField(TEXT("asset_path")), DataflowClassPath, Out, Error);
			if (!Asset) return false;
			MarkSuccess(Out, TEXT("chaos_dataflow_asset_native_inspect"), Asset, false);
			Out->SetObjectField(TEXT("graph_readback"), DataflowSnapshot(Asset));
			Summary = FString::Printf(TEXT("Inspected native Dataflow asset %s."), *Asset->GetPathName()); return true;
		}, nullptr, 15});

	Registry.Register({TEXT("chaos_dataflow_evaluate_compile_diagnostics"),
		TEXT("Evaluate a native Dataflow graph against an explicitly bound target, save both assets, and return compile/evaluation diagnostics."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Dataflow asset path under /Game/."), {}, 1, 1024)},
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Explicit /Game asset updated by Dataflow terminal nodes."), {}, 1, 1024)},
			{TEXT("confirm_evaluate_and_save"), FSololmcpSchemaBuilder::Boolean(TEXT("Required because evaluation may mutate the target asset."))}
		}, {TEXT("asset_path"), TEXT("target_asset"), TEXT("confirm_evaluate_and_save")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (!RequireConfirm(Args, TEXT("confirm_evaluate_and_save"), Out, Error)) return false;
			UObject* Dataflow = LoadTypedAsset(Context, Args->GetStringField(TEXT("asset_path")), DataflowClassPath, Out, Error);
			if (!Dataflow) return false;
			const FString TargetPath = Args->GetStringField(TEXT("target_asset"));
			if (!TargetPath.StartsWith(TEXT("/Game/"))) { Fail(Out, Error, TEXT("invalid_target_asset"), TEXT("target_asset must be under /Game/.")); return false; }
			FString LoadError; UObject* Target = Context.Services.LoadAsset(TargetPath, LoadError);
			if (!Target) { Fail(Out, Error, TEXT("dataflow_target_not_found"), LoadError); return false; }
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "EvaluateDataflow", "SOMOLMCP Evaluate Dataflow"));
			Dataflow->Modify(); Target->Modify();
			bool bEvaluationResult = false;
			if (!InvokeDataflowEvaluate(Dataflow, Target, bEvaluationResult, Error) || !bEvaluationResult)
			{
				Transaction.Cancel(); Fail(Out, Error, TEXT("dataflow_compile_or_evaluate_failed"), Error.IsEmpty()
					? TEXT("Native Dataflow evaluation returned false. Inspect terminal nodes, links, variables, and target compatibility.") : Error); return false;
			}
			if (!SaveAndReadback(Context, Dataflow, Out, Error)) return false;
			TSharedRef<FJsonObject> TargetSave = MakeShared<FJsonObject>();
			if (!SaveAndReadback(Context, Target, TargetSave, Error)) return false;
			MarkSuccess(Out, TEXT("chaos_dataflow_evaluate_compile_diagnostics"), Dataflow, true);
			Out->SetBoolField(TEXT("compile_evaluate_succeeded"), true);
			Out->SetStringField(TEXT("target_asset"), Target->GetPathName());
			Out->SetObjectField(TEXT("graph_readback"), DataflowSnapshot(Dataflow));
			Out->SetObjectField(TEXT("target_save_readback"), TargetSave);
			Out->SetArrayField(TEXT("diagnostics"), {});
			Summary = FString::Printf(TEXT("Dataflow %s evaluated and saved target %s."), *Dataflow->GetPathName(), *Target->GetPathName()); return true;
		}, nullptr, 1});

	Registry.Register({TEXT("chaos_dataflow_authoring_receipt_validate"),
		TEXT("Validate native Dataflow creation or compile/evaluation evidence before downstream Chaos authoring continues."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("receipt_id"), FSololmcpSchemaBuilder::String({}, {}, 1, 160)},
			{TEXT("status"), FSololmcpSchemaBuilder::String({}, {}, 1, 32)},
			{TEXT("mutation_applied"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("saved"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("readback_verified"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("target"), FSololmcpSchemaBuilder::String({}, {}, 1, 1024)}
		}, {TEXT("receipt_id"), TEXT("status"), TEXT("mutation_applied"), TEXT("saved"), TEXT("readback_verified"), TEXT("target")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const FString Status = Args->GetStringField(TEXT("status"));
			const bool bValid = (Status == TEXT("succeeded") || Status == TEXT("completed")) && Args->GetBoolField(TEXT("mutation_applied")) &&
				Args->GetBoolField(TEXT("saved")) && Args->GetBoolField(TEXT("readback_verified")) && Args->GetStringField(TEXT("target")).StartsWith(TEXT("/Game/"));
			if (!bValid) { Fail(Out, Error, TEXT("dataflow_receipt_rejected"), TEXT("Receipt does not prove a saved, read-back native Dataflow mutation.")); return false; }
			Out->SetBoolField(TEXT("ok"), true); Out->SetBoolField(TEXT("valid"), true); Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), Args->GetStringField(TEXT("receipt_id")));
			Summary = TEXT("Native Dataflow authoring receipt accepted."); return true;
		}, nullptr, 15});
#else
	(void)Registry;
#endif
}
}
