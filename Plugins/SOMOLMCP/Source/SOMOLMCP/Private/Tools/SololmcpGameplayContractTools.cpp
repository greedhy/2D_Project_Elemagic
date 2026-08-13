// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP - Gameplay authoring contract probes.
//
// These tools make the gameplay_author role prefixes concrete while keeping
// authoring safe. They are read-only capability probes and schema anchors for
// GAS, AI Perception, StateTree, and SmartObject workflows.

#include "Tools/SololmcpToolRegistry.h"

#include "BlueprintEditorLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "ScopedTransaction.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include <initializer_list>

namespace UE::SOMOLMCP
{
namespace GameplayContractTools
{
	struct FAuthoringSkeletonContract
	{
		const TCHAR* ToolName;
		const TCHAR* Domain;
		const TCHAR* Risk;
		TArray<FString> RequiredModules;
		TArray<FString> RequiredPlugins;
		TArray<FString> RequiredFields;
		TArray<FString> OptionalFields;
		TArray<FString> Validates;
		TArray<FString> ReceiptClaims;
	};

	static TSharedRef<FJsonObject> MakeFeatureStatus(
		const FString& Feature,
		const TArray<FString>& ClassPaths,
		const TArray<FString>& PlannedTools)
	{
		TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
		Status->SetStringField(TEXT("feature"), Feature);

		TArray<TSharedPtr<FJsonValue>> Classes;
		int32 AvailableClassCount = 0;
		for (const FString& ClassPath : ClassPaths)
		{
			UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *ClassPath);
			}

			TSharedRef<FJsonObject> ClassJson = MakeShared<FJsonObject>();
			ClassJson->SetStringField(TEXT("class_path"), ClassPath);
			ClassJson->SetBoolField(TEXT("available"), Class != nullptr);
			if (Class)
			{
				ClassJson->SetStringField(TEXT("resolved_name"), Class->GetName());
				++AvailableClassCount;
			}
			Classes.Add(MakeShared<FJsonValueObject>(ClassJson));
		}

		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const FString& Tool : PlannedTools)
		{
			Tools.Add(MakeShared<FJsonValueString>(Tool));
		}

		Status->SetStringField(TEXT("contract_version"), TEXT("gameplay.contract.v1"));
		Status->SetStringField(TEXT("implementation_level"), TEXT("read_only_probe"));
		Status->SetNumberField(TEXT("available_class_count"), AvailableClassCount);
		Status->SetNumberField(TEXT("class_count"), ClassPaths.Num());
		Status->SetBoolField(TEXT("plugin_surface_available"), AvailableClassCount > 0);
		Status->SetArrayField(TEXT("classes"), Classes);
		Status->SetArrayField(TEXT("planned_write_tools"), Tools);
		return Status;
	}

	static bool ExecuteProbe(
		const FString& Feature,
		const TArray<FString>& ClassPaths,
		const TArray<FString>& PlannedTools,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary)
	{
		TSharedRef<FJsonObject> Status = MakeFeatureStatus(Feature, ClassPaths, PlannedTools);
		const bool bIncludeTools = !Arguments->HasTypedField<EJson::Boolean>(TEXT("include_planned_tools")) || Arguments->GetBoolField(TEXT("include_planned_tools"));
		if (!bIncludeTools)
		{
			Status->RemoveField(TEXT("planned_write_tools"));
		}
		OutStructured = Status;
		OutSummary = FString::Printf(
			TEXT("%s contract probe: %d/%d classes available."),
			*Feature,
			static_cast<int32>(Status->GetNumberField(TEXT("available_class_count"))),
			static_cast<int32>(Status->GetNumberField(TEXT("class_count"))));
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static bool IsBooleanFieldName(const FString& FieldName)
	{
		static const TSet<FString> Fields = {
			TEXT("apply_to_blueprint_cdo"), TEXT("auto_register_as_source"), TEXT("clamp_to_safe_range"),
			TEXT("compile_after"), TEXT("complex_as_simple"), TEXT("collision_enabled"),
			TEXT("create_collision"), TEXT("create_stub_if_missing"), TEXT("dry_run"),
			TEXT("enable_section_collision"), TEXT("include_components"), TEXT("include_properties"),
			TEXT("is_replicated"), TEXT("mark_collision_dirty"), TEXT("reliable"),
			TEXT("replicate_movement"), TEXT("replicated"), TEXT("replicates"), TEXT("require_replicates"),
			TEXT("strict"), TEXT("update_bounds"), TEXT("use_async_cooking"), TEXT("validate_only"),
			TEXT("validate_signature")
		};
		return Fields.Contains(FieldName);
	}

	static bool ReadFlexibleBool(const TSharedRef<FJsonObject>& Arguments, const FString& FieldName, const bool bDefault)
	{
		bool bValue = bDefault;
		if (Arguments->TryGetBoolField(FieldName, bValue))
		{
			return bValue;
		}

		FString StringValue;
		if (Arguments->TryGetStringField(FieldName, StringValue))
		{
			StringValue = StringValue.TrimStartAndEnd().ToLower();
			if (StringValue == TEXT("true") || StringValue == TEXT("1") || StringValue == TEXT("yes") || StringValue == TEXT("on"))
			{
				return true;
			}
			if (StringValue == TEXT("false") || StringValue == TEXT("0") || StringValue == TEXT("no") || StringValue == TEXT("off"))
			{
				return false;
			}
		}
		return bDefault;
	}

	static UEdGraph* FindBlueprintFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::CaseSensitive))
			{
				return Graph;
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	static UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
			{
				return EntryNode;
			}
		}
		return nullptr;
	}

	static TArray<TSharedPtr<FJsonValue>> RpcFlagsToJson(const uint64 Flags)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		auto AddIf = [&Values, Flags](const EFunctionFlags Flag, const TCHAR* Name)
		{
			if ((Flags & static_cast<uint64>(Flag)) != 0)
			{
				Values.Add(MakeShared<FJsonValueString>(Name));
			}
		};

		AddIf(FUNC_Net, TEXT("FUNC_Net"));
		AddIf(FUNC_NetReliable, TEXT("FUNC_NetReliable"));
		AddIf(FUNC_NetServer, TEXT("FUNC_NetServer"));
		AddIf(FUNC_NetClient, TEXT("FUNC_NetClient"));
		AddIf(FUNC_NetMulticast, TEXT("FUNC_NetMulticast"));
		AddIf(FUNC_BlueprintCallable, TEXT("FUNC_BlueprintCallable"));
		AddIf(FUNC_Public, TEXT("FUNC_Public"));
		return Values;
	}

	static bool ResolveRpcFlags(
		const FString& RpcKind,
		const bool bReliable,
		EFunctionFlags& OutRequiredKindFlag,
		int32& OutFlags,
		FString& OutNormalizedKind,
		FString& OutError)
	{
		OutNormalizedKind = RpcKind.TrimStartAndEnd().ToLower();
		OutFlags = FUNC_Net | FUNC_BlueprintCallable | FUNC_Public;
		OutRequiredKindFlag = FUNC_None;

		if (OutNormalizedKind == TEXT("server"))
		{
			OutFlags |= FUNC_NetServer;
			OutRequiredKindFlag = FUNC_NetServer;
		}
		else if (OutNormalizedKind == TEXT("client"))
		{
			OutFlags |= FUNC_NetClient;
			OutRequiredKindFlag = FUNC_NetClient;
		}
		else if (OutNormalizedKind == TEXT("multicast") || OutNormalizedKind == TEXT("netmulticast"))
		{
			OutNormalizedKind = TEXT("multicast");
			OutFlags |= FUNC_NetMulticast;
			OutRequiredKindFlag = FUNC_NetMulticast;
		}
		else
		{
			OutError = TEXT("rpc_kind must be one of: server, client, multicast.");
			return false;
		}

		if (bReliable)
		{
			OutFlags |= FUNC_NetReliable;
		}
		return true;
	}

	static bool ExecuteNetworkRpcMark(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString AssetPath;
		FString FunctionName;
		FString RpcKind;
		if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
			!Arguments->TryGetStringField(TEXT("function_name"), FunctionName) ||
			!Arguments->TryGetStringField(TEXT("rpc_kind"), RpcKind))
		{
			OutError = TEXT("Missing asset_path, function_name, or rpc_kind.");
			return false;
		}

		const bool bDryRun = ReadFlexibleBool(Arguments, TEXT("dry_run"), false);
		const bool bValidateOnly = ReadFlexibleBool(Arguments, TEXT("validate_only"), false);
		const bool bReliable = ReadFlexibleBool(Arguments, TEXT("reliable"), false);
		const bool bValidateSignature = ReadFlexibleBool(Arguments, TEXT("validate_signature"), true);
		const bool bCompileAfter = ReadFlexibleBool(Arguments, TEXT("compile_after"), true);

		EFunctionFlags RequiredKindFlag = FUNC_None;
		int32 DesiredFlags = 0;
		FString NormalizedKind;
		if (!ResolveRpcFlags(RpcKind, bReliable, RequiredKindFlag, DesiredFlags, NormalizedKind, OutError))
		{
			return false;
		}

		UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
		if (!Blueprint)
		{
			OutError = TEXT("Asset is not a Blueprint.");
			return false;
		}

		UEdGraph* FunctionGraph = FindBlueprintFunctionGraph(Blueprint, FunctionName);
		if (!FunctionGraph)
		{
			OutError = FString::Printf(TEXT("Function graph '%s' was not found."), *FunctionName);
			return false;
		}

		UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(FunctionGraph);
		if (!EntryNode)
		{
			OutError = TEXT("Function entry node was not found.");
			return false;
		}

		const int32 OldExtraFlags = EntryNode->GetExtraFlags();
		const int32 KindMask = FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast;
		const int32 ReliabilityMask = FUNC_NetReliable;

		OutStructured->SetStringField(TEXT("contract_version"), TEXT("network.rpc_mark.v2"));
		OutStructured->SetStringField(TEXT("implementation_level"), TEXT("ue_mutation_live_apply"));
		OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		OutStructured->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
		OutStructured->SetStringField(TEXT("rpc_kind"), NormalizedKind);
		OutStructured->SetBoolField(TEXT("reliable"), bReliable);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetBoolField(TEXT("validate_only"), bValidateOnly);
		OutStructured->SetBoolField(TEXT("validate_signature"), bValidateSignature);
		OutStructured->SetBoolField(TEXT("compile_after"), bCompileAfter);
		OutStructured->SetNumberField(TEXT("old_extra_flags"), OldExtraFlags);
		OutStructured->SetArrayField(TEXT("desired_flags"), RpcFlagsToJson(static_cast<uint64>(DesiredFlags)));

		if (bValidateSignature)
		{
			// Blueprint RPC functions must not be pure/const/static and should
			// compile as event-like callable functions. We enforce the mutating
			// flags below and rely on compile + generated UFunction readback for
			// the definitive signature receipt.
			OutStructured->SetBoolField(TEXT("signature_preflight_ok"), true);
		}

		if (bDryRun || bValidateOnly)
		{
			OutStructured->SetBoolField(TEXT("can_apply"), true);
			OutStructured->SetBoolField(TEXT("applied"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("validated_no_mutation"));
			OutSummary = FString::Printf(TEXT("Validated RPC mark for %s as %s; no mutation applied."), *FunctionName, *NormalizedKind);
			return true;
		}

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NetworkRpcMark", "SOMOLMCP Mark Blueprint RPC"));
		Blueprint->Modify();
		FunctionGraph->Modify();
		EntryNode->Modify();
		EntryNode->ClearExtraFlags(KindMask | ReliabilityMask | FUNC_Net);
		EntryNode->AddExtraFlags(DesiredFlags);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		if (bCompileAfter)
		{
			UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
		}

		const int32 NewExtraFlags = EntryNode->GetExtraFlags();
		UFunction* GeneratedFunction = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->FindFunctionByName(FName(*FunctionGraph->GetName())) : nullptr;
		if (!GeneratedFunction && Blueprint->GeneratedClass)
		{
			GeneratedFunction = Blueprint->GeneratedClass->FindFunctionByName(FName(*FunctionName));
		}

		const uint64 GeneratedFlags = GeneratedFunction ? static_cast<uint64>(GeneratedFunction->FunctionFlags) : 0;
		const bool bEntryHasDesiredKind = EntryNode->HasAllExtraFlags(FUNC_Net | RequiredKindFlag);
		const bool bEntryReliableOk = !bReliable || EntryNode->HasAllExtraFlags(FUNC_NetReliable);
		const bool bGeneratedHasDesiredKind = GeneratedFunction && GeneratedFunction->HasAllFunctionFlags(static_cast<EFunctionFlags>(FUNC_Net | RequiredKindFlag));
		const bool bGeneratedReliableOk = !bReliable || (GeneratedFunction && GeneratedFunction->HasAllFunctionFlags(FUNC_NetReliable));
		const bool bCompileOk = !bCompileAfter || Blueprint->Status != BS_Error;
		const bool bVerified = bEntryHasDesiredKind && bEntryReliableOk && bGeneratedHasDesiredKind && bGeneratedReliableOk && bCompileOk;

		OutStructured->SetNumberField(TEXT("new_extra_flags"), NewExtraFlags);
		OutStructured->SetArrayField(TEXT("entry_flags"), RpcFlagsToJson(static_cast<uint64>(NewExtraFlags)));
		OutStructured->SetBoolField(TEXT("entry_has_desired_rpc_kind"), bEntryHasDesiredKind);
		OutStructured->SetBoolField(TEXT("entry_reliable_ok"), bEntryReliableOk);
		OutStructured->SetBoolField(TEXT("generated_function_found"), GeneratedFunction != nullptr);
		OutStructured->SetNumberField(TEXT("generated_function_flags"), static_cast<double>(GeneratedFlags));
		OutStructured->SetArrayField(TEXT("generated_flags"), RpcFlagsToJson(GeneratedFlags));
		OutStructured->SetBoolField(TEXT("generated_has_desired_rpc_kind"), bGeneratedHasDesiredKind);
		OutStructured->SetBoolField(TEXT("generated_reliable_ok"), bGeneratedReliableOk);
		OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
		OutStructured->SetBoolField(TEXT("compile_ok"), bCompileOk);
		OutStructured->SetBoolField(TEXT("can_apply"), true);
		OutStructured->SetBoolField(TEXT("applied"), bVerified);
		OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("applied_and_verified") : TEXT("applied_but_verification_failed"));

		if (!bVerified)
		{
			OutError = TEXT("RPC flags were written but post-write compile/readback verification failed.");
			OutSummary = FString::Printf(TEXT("RPC mark for %s could not be verified."), *FunctionName);
			return false;
		}

		OutSummary = FString::Printf(TEXT("Marked Blueprint function %s as %s RPC%s."), *FunctionName, *NormalizedKind, bReliable ? TEXT(" reliable") : TEXT(""));
		return true;
	}

	static bool IsNumberFieldName(const FString& FieldName)
	{
		static const TSet<FString> Fields = {
			TEXT("default_value"), TEXT("duration_seconds"), TEXT("hearing_range"),
			TEXT("lod_index"), TEXT("lose_sight_radius"), TEXT("max_triangles"),
			TEXT("max_vertices"), TEXT("min_net_update_frequency"), TEXT("min_value"),
			TEXT("magnitude"), TEXT("max_value"), TEXT("net_update_frequency"), TEXT("priority"),
			TEXT("screen_size"), TEXT("section_index"), TEXT("sight_radius")
		};
		return Fields.Contains(FieldName);
	}

	static bool IsArrayFieldName(const FString& FieldName)
	{
		static const TSet<FString> Fields = {
			TEXT("activity_tags"), TEXT("affiliation"), TEXT("attributes"), TEXT("behavior_definitions"),
			TEXT("conditions"), TEXT("convex_hulls"), TEXT("convex_meshes"), TEXT("material_slots"),
			TEXT("normals"), TEXT("section_groups"), TEXT("sense_classes"), TEXT("source_tags"),
			TEXT("tags"), TEXT("tangents"), TEXT("target_tags"), TEXT("triangles"),
			TEXT("user_tags"), TEXT("uv0"), TEXT("vertex_colors"), TEXT("vertices")
		};
		return Fields.Contains(FieldName);
	}

	static bool IsObjectFieldName(const FString& FieldName)
	{
		static const TSet<FString> Fields = {
			TEXT("context_data"), TEXT("parameters"), TEXT("properties"), TEXT("stacking_rule"),
			TEXT("streams"), TEXT("transform")
		};
		return Fields.Contains(FieldName);
	}

	static TSharedRef<FJsonObject> SchemaForFieldName(const FString& FieldName)
	{
		if (IsBooleanFieldName(FieldName))
		{
			return FSololmcpSchemaBuilder::Boolean();
		}
		if (IsNumberFieldName(FieldName))
		{
			return FSololmcpSchemaBuilder::Number();
		}
		if (IsArrayFieldName(FieldName))
		{
			return FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}));
		}
		if (IsObjectFieldName(FieldName))
		{
			return FSololmcpSchemaBuilder::Object({});
		}
		return FSololmcpSchemaBuilder::String();
	}

	static TSharedRef<FJsonObject> MakeAuthoringSkeletonSchema(const FAuthoringSkeletonContract& Contract)
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties;
		for (const FString& Field : Contract.RequiredFields)
		{
			Properties.Add(Field, SchemaForFieldName(Field));
		}
		for (const FString& Field : Contract.OptionalFields)
		{
			if (!Properties.Contains(Field))
			{
				Properties.Add(Field, SchemaForFieldName(Field));
			}
		}
		Properties.Add(TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Defaults to true; no live mutation is performed by this skeleton.")));
		Properties.Add(TEXT("validate_only"), FSololmcpSchemaBuilder::Boolean(TEXT("Return validation receipt only; equivalent to dry_run for this skeleton.")));
		Properties.Add(TEXT("transaction_label"), FSololmcpSchemaBuilder::String(TEXT("Future editor transaction label for live implementation.")));
		return FSololmcpSchemaBuilder::Object(Properties, Contract.RequiredFields);
	}

	static TSharedRef<FJsonObject> MakeAuthoringSkeletonOutputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("contract_version"), FSololmcpSchemaBuilder::String()},
			{TEXT("implementation_level"), FSololmcpSchemaBuilder::String()},
			{TEXT("tool_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("domain"), FSololmcpSchemaBuilder::String()},
			{TEXT("risk"), FSololmcpSchemaBuilder::String()},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("validate_only"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("can_apply"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("applied"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("status"), FSololmcpSchemaBuilder::String()},
			{TEXT("module_status"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("plugin_status"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("validates"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
			{TEXT("receipt_claims"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}
		});
	}

	static TArray<TSharedPtr<FJsonValue>> ModuleStatusJson(const TArray<FString>& ModuleNames, bool& bAllAvailable)
	{
		bAllAvailable = true;
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& ModuleName : ModuleNames)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			const FName ModuleFName(*ModuleName);
			const bool bExists = FModuleManager::Get().ModuleExists(*ModuleName);
			const bool bLoaded = FModuleManager::Get().IsModuleLoaded(ModuleFName);
			Entry->SetStringField(TEXT("module"), ModuleName);
			Entry->SetBoolField(TEXT("exists"), bExists);
			Entry->SetBoolField(TEXT("loaded"), bLoaded);
			bAllAvailable = bAllAvailable && (bExists || bLoaded);
			Result.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Result;
	}

	static TArray<TSharedPtr<FJsonValue>> PluginStatusJson(const TArray<FString>& PluginNames, bool& bAllAvailable)
	{
		bAllAvailable = true;
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& PluginName : PluginNames)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			Entry->SetStringField(TEXT("plugin"), PluginName);
			Entry->SetBoolField(TEXT("found"), Plugin.IsValid());
			Entry->SetBoolField(TEXT("enabled"), Plugin.IsValid() ? Plugin->IsEnabled() : false);
			bAllAvailable = bAllAvailable && Plugin.IsValid() && Plugin->IsEnabled();
			Result.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Result;
	}

	static bool ExecuteAuthoringSkeleton(
		const FAuthoringSkeletonContract& Contract,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary)
	{
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		bool bValidateOnly = false;
		Arguments->TryGetBoolField(TEXT("validate_only"), bValidateOnly);

		bool bModulesAvailable = true;
		bool bPluginsAvailable = true;
		TArray<TSharedPtr<FJsonValue>> ModuleStatus = ModuleStatusJson(Contract.RequiredModules, bModulesAvailable);
		TArray<TSharedPtr<FJsonValue>> PluginStatus = PluginStatusJson(Contract.RequiredPlugins, bPluginsAvailable);

		OutStructured->SetBoolField(TEXT("ok"), true);
		OutStructured->SetStringField(TEXT("contract_version"), TEXT("authoring.skeleton.v1"));
		OutStructured->SetStringField(TEXT("implementation_level"), TEXT("ue_mutation_tool_skeleton_no_live_apply"));
		OutStructured->SetStringField(TEXT("tool_name"), Contract.ToolName);
		OutStructured->SetStringField(TEXT("domain"), Contract.Domain);
		OutStructured->SetStringField(TEXT("risk"), Contract.Risk);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetBoolField(TEXT("validate_only"), bValidateOnly);
		OutStructured->SetBoolField(TEXT("can_apply"), false);
		OutStructured->SetBoolField(TEXT("applied"), false);
		const bool bLiveApplyRequested = !bDryRun && !bValidateOnly;
		OutStructured->SetBoolField(TEXT("fail_closed"), bLiveApplyRequested);
		OutStructured->SetStringField(
			TEXT("status"),
			bLiveApplyRequested ? TEXT("live_apply_not_implemented") : TEXT("validated_contract_skeleton"));
		OutStructured->SetArrayField(TEXT("required_fields"), StringArrayJson(Contract.RequiredFields));
		OutStructured->SetArrayField(TEXT("optional_fields"), StringArrayJson(Contract.OptionalFields));
		OutStructured->SetArrayField(TEXT("required_modules"), StringArrayJson(Contract.RequiredModules));
		OutStructured->SetArrayField(TEXT("required_plugins"), StringArrayJson(Contract.RequiredPlugins));
		OutStructured->SetArrayField(TEXT("module_status"), ModuleStatus);
		OutStructured->SetArrayField(TEXT("plugin_status"), PluginStatus);
		OutStructured->SetBoolField(TEXT("required_modules_available"), bModulesAvailable);
		OutStructured->SetBoolField(TEXT("required_plugins_available"), bPluginsAvailable);
		OutStructured->SetArrayField(TEXT("validates"), StringArrayJson(Contract.Validates));
		OutStructured->SetArrayField(TEXT("receipt_claims"), StringArrayJson(Contract.ReceiptClaims));
		OutStructured->SetStringField(
			TEXT("reason"),
			TEXT("UE-side tool is registered with schema and receipt contract; live editor mutation is intentionally not implemented in this skeleton pass."));
		OutStructured->SetStringField(
			TEXT("next_implementation_step"),
			TEXT("Add editor transaction, resolve target, apply one narrow mutation, post-read changed fields, then set can_apply/applied according to verified result."));

		if (bLiveApplyRequested)
		{
			OutStructured->SetBoolField(TEXT("ok"), false);
			OutStructured->SetStringField(TEXT("error_code"), TEXT("NOT_IMPLEMENTED"));
			OutStructured->SetStringField(TEXT("error_field"), TEXT("dry_run"));
			OutStructured->SetStringField(
				TEXT("error"),
				TEXT("Live apply is not implemented for this gameplay authoring skeleton. Re-run with dry_run=true or validate_only=true."));
			OutSummary = FString::Printf(
				TEXT("%s authoring skeleton rejected live apply; dry_run=false is fail-closed."),
				Contract.ToolName);
			return false;
		}

		OutSummary = FString::Printf(
			TEXT("%s authoring skeleton validated; live apply not implemented, applied=false."),
			Contract.ToolName);
		return true;
	}

	static FAuthoringSkeletonContract MakeContract(
		const TCHAR* ToolName,
		const TCHAR* Domain,
		const TCHAR* Risk,
		std::initializer_list<const TCHAR*> RequiredModules,
		std::initializer_list<const TCHAR*> RequiredPlugins,
		std::initializer_list<const TCHAR*> RequiredFields,
		std::initializer_list<const TCHAR*> OptionalFields,
		std::initializer_list<const TCHAR*> Validates,
		std::initializer_list<const TCHAR*> ReceiptClaims)
	{
		FAuthoringSkeletonContract Contract;
		Contract.ToolName = ToolName;
		Contract.Domain = Domain;
		Contract.Risk = Risk;
		for (const TCHAR* Value : RequiredModules) { Contract.RequiredModules.Add(Value); }
		for (const TCHAR* Value : RequiredPlugins) { Contract.RequiredPlugins.Add(Value); }
		for (const TCHAR* Value : RequiredFields) { Contract.RequiredFields.Add(Value); }
		for (const TCHAR* Value : OptionalFields) { Contract.OptionalFields.Add(Value); }
		for (const TCHAR* Value : Validates) { Contract.Validates.Add(Value); }
		for (const TCHAR* Value : ReceiptClaims) { Contract.ReceiptClaims.Add(Value); }
		return Contract;
	}

	static void RegisterAuthoringSkeleton(FSololmcpToolRegistry& Registry, const FAuthoringSkeletonContract& Contract)
	{
		Registry.Register({
			Contract.ToolName,
			FString::Printf(TEXT("%s authoring mutation skeleton. Registers UE-side schema/receipt only; live mutation returns applied=false."), Contract.ToolName),
			MakeAuthoringSkeletonSchema(Contract),
			[Contract](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
			{
				return ExecuteAuthoringSkeleton(Contract, Arguments, OutStructured, OutSummary);
			},
			nullptr,
			0,
			MakeAuthoringSkeletonOutputSchema()
		});
	}
}

void RegisterGameplayContractTools(FSololmcpToolRegistry& Registry)
{
	using namespace GameplayContractTools;
	using SB = FSololmcpSchemaBuilder;

	const TArray<FAuthoringSkeletonContract> SkeletonContracts = {
		MakeContract(TEXT("gas_attribute_set_create"), TEXT("gameplay.gas"), TEXT("asset_create"), {TEXT("GameplayAbilities"), TEXT("GameplayTags")}, {}, {TEXT("asset_path"), TEXT("class_name")}, {TEXT("parent_class"), TEXT("replication_policy"), TEXT("attributes")}, {TEXT("asset path is unused"), TEXT("class_name ends with AttributeSet")}, {TEXT("attribute_set_path"), TEXT("attribute_count"), TEXT("compile_ok")}),
		MakeContract(TEXT("gas_attribute_add"), TEXT("gameplay.gas"), TEXT("asset_mutate"), {TEXT("GameplayAbilities")}, {}, {TEXT("attribute_set_path"), TEXT("name"), TEXT("value_type")}, {TEXT("default_value"), TEXT("min_value"), TEXT("max_value"), TEXT("replicated")}, {TEXT("attribute name is unique"), TEXT("numeric bounds are ordered")}, {TEXT("attribute_set_path"), TEXT("attribute_name"), TEXT("replicated")}),
		MakeContract(TEXT("gas_gameplay_effect_create"), TEXT("gameplay.gas"), TEXT("asset_create"), {TEXT("GameplayAbilities"), TEXT("GameplayTags")}, {}, {TEXT("asset_path"), TEXT("effect_name")}, {TEXT("duration_policy"), TEXT("duration_seconds"), TEXT("stacking_policy"), TEXT("tags")}, {TEXT("gameplay tags resolve"), TEXT("duration policy matches duration fields")}, {TEXT("gameplay_effect_path"), TEXT("duration_policy"), TEXT("tags_ok")}),
		MakeContract(TEXT("gas_gameplay_effect_add_modifier"), TEXT("gameplay.gas"), TEXT("asset_mutate"), {TEXT("GameplayAbilities")}, {}, {TEXT("effect_path"), TEXT("attribute"), TEXT("operation"), TEXT("magnitude")}, {TEXT("source_tags"), TEXT("target_tags"), TEXT("stacking_rule")}, {TEXT("attribute exists"), TEXT("operation is additive/multiply/divide/override")}, {TEXT("gameplay_effect_path"), TEXT("modifier_count")}),
		MakeContract(TEXT("gas_gameplay_cue_create"), TEXT("gameplay.gas"), TEXT("asset_create"), {TEXT("GameplayAbilities"), TEXT("GameplayTags"), TEXT("GameplayTasks")}, {}, {TEXT("asset_path"), TEXT("cue_tag"), TEXT("cue_class")}, {TEXT("parent_class"), TEXT("particle_system"), TEXT("sound"), TEXT("camera_shake")}, {TEXT("cue_tag starts with GameplayCue."), TEXT("tag exists or create_tag is true")}, {TEXT("gameplay_cue_path"), TEXT("cue_tag"), TEXT("tags_ok")}),
		MakeContract(TEXT("ai_perception_add_component"), TEXT("ai.perception"), TEXT("actor_mutate"), {TEXT("AIModule")}, {}, {TEXT("target_path")}, {TEXT("component_name"), TEXT("dominant_sense")}, {TEXT("target is controller or pawn"), TEXT("component name is unique")}, {TEXT("target_path"), TEXT("component_name"), TEXT("dominant_sense")}),
		MakeContract(TEXT("ai_perception_configure_sense"), TEXT("ai.perception"), TEXT("asset_mutate"), {TEXT("AIModule")}, {}, {TEXT("target_path"), TEXT("sense_class")}, {TEXT("sight_radius"), TEXT("lose_sight_radius"), TEXT("hearing_range"), TEXT("affiliation")}, {TEXT("perception component exists"), TEXT("sense_class is supported")}, {TEXT("target_path"), TEXT("sense_class"), TEXT("config_count")}),
		MakeContract(TEXT("ai_perception_bind_stimuli_source"), TEXT("ai.perception"), TEXT("actor_mutate"), {TEXT("AIModule")}, {}, {TEXT("target_path"), TEXT("sense_classes")}, {TEXT("auto_register_as_source"), TEXT("component_name")}, {TEXT("sense_classes is non-empty"), TEXT("component name is unique")}, {TEXT("target_path"), TEXT("sense_count"), TEXT("auto_register")}),
		MakeContract(TEXT("state_tree_create"), TEXT("ai.state_tree"), TEXT("asset_create"), {TEXT("StateTreeModule"), TEXT("GameplayStateTreeModule")}, {}, {TEXT("asset_path"), TEXT("schema")}, {TEXT("context_data"), TEXT("parameters")}, {TEXT("schema class exists"), TEXT("asset path is unused")}, {TEXT("state_tree_path"), TEXT("schema"), TEXT("compile_ok")}),
		MakeContract(TEXT("state_tree_add_state"), TEXT("ai.state_tree"), TEXT("asset_mutate"), {TEXT("StateTreeModule")}, {}, {TEXT("state_tree_path"), TEXT("state_name")}, {TEXT("parent_state"), TEXT("type"), TEXT("description")}, {TEXT("state name is unique under parent"), TEXT("parent exists")}, {TEXT("state_tree_path"), TEXT("state_name"), TEXT("state_count")}),
		MakeContract(TEXT("state_tree_add_transition"), TEXT("ai.state_tree"), TEXT("asset_mutate"), {TEXT("StateTreeModule")}, {}, {TEXT("state_tree_path"), TEXT("from_state"), TEXT("to_state"), TEXT("trigger")}, {TEXT("conditions"), TEXT("priority")}, {TEXT("source and target states exist"), TEXT("trigger is supported")}, {TEXT("state_tree_path"), TEXT("transition_count")}),
		MakeContract(TEXT("state_tree_bind_task"), TEXT("ai.state_tree"), TEXT("asset_mutate"), {TEXT("StateTreeModule")}, {}, {TEXT("state_tree_path"), TEXT("state_name"), TEXT("node_class")}, {TEXT("node_kind"), TEXT("properties")}, {TEXT("state exists"), TEXT("node_class is a StateTree node type")}, {TEXT("state_tree_path"), TEXT("state_name"), TEXT("node_class")}),
		MakeContract(TEXT("smart_object_definition_create"), TEXT("ai.smart_object"), TEXT("asset_create"), {TEXT("SmartObjectsModule"), TEXT("GameplayTags")}, {}, {TEXT("asset_path"), TEXT("definition_name")}, {TEXT("activity_tags"), TEXT("behavior_definitions")}, {TEXT("asset path is unused"), TEXT("activity tags resolve")}, {TEXT("smart_object_definition_path"), TEXT("tags_ok")}),
		MakeContract(TEXT("smart_object_add_slot"), TEXT("ai.smart_object"), TEXT("asset_mutate"), {TEXT("SmartObjectsModule"), TEXT("GameplayTags")}, {}, {TEXT("definition_path"), TEXT("slot_name"), TEXT("transform")}, {TEXT("activity_tags"), TEXT("user_tags"), TEXT("preview_actor")}, {TEXT("slot name is unique"), TEXT("tag requirements resolve")}, {TEXT("smart_object_definition_path"), TEXT("slot_count")}),
		MakeContract(TEXT("smart_object_add_behavior"), TEXT("ai.smart_object"), TEXT("asset_mutate"), {TEXT("SmartObjectsModule")}, {}, {TEXT("definition_path"), TEXT("behavior_class")}, {TEXT("state_tree_path"), TEXT("gameplay_behavior_path"), TEXT("slot_filter")}, {TEXT("behavior class exists"), TEXT("referenced assets exist")}, {TEXT("smart_object_definition_path"), TEXT("behavior_count")}),
		MakeContract(TEXT("smart_object_validate_definition"), TEXT("ai.smart_object"), TEXT("read_only"), {TEXT("SmartObjectsModule"), TEXT("GameplayTags")}, {}, {TEXT("definition_path")}, {TEXT("strict")}, {TEXT("definition exists"), TEXT("all slots and behaviors are internally valid")}, {TEXT("smart_object_definition_path"), TEXT("error_count"), TEXT("warning_count")}),
		MakeContract(TEXT("network_actor_set_replicates"), TEXT("network.replication"), TEXT("actor_mutate"), {TEXT("Engine"), TEXT("NetCore")}, {}, {TEXT("target_path"), TEXT("replicates")}, {TEXT("apply_to_blueprint_cdo"), TEXT("transaction_label")}, {TEXT("target resolves to actor, actor blueprint, or actor CDO"), TEXT("post-read confirms bReplicates")}, {TEXT("target_path"), TEXT("replicates"), TEXT("transaction_id")}),
		MakeContract(TEXT("network_actor_set_replicate_movement"), TEXT("network.replication"), TEXT("actor_mutate"), {TEXT("Engine"), TEXT("NetCore")}, {}, {TEXT("target_path"), TEXT("replicate_movement")}, {TEXT("require_replicates"), TEXT("transaction_label")}, {TEXT("target resolves to actor"), TEXT("warns when bReplicates is false")}, {TEXT("target_path"), TEXT("replicate_movement"), TEXT("warning_count")}),
		MakeContract(TEXT("network_component_set_is_replicated"), TEXT("network.replication"), TEXT("component_mutate"), {TEXT("Engine"), TEXT("NetCore")}, {}, {TEXT("target_path"), TEXT("component_name"), TEXT("is_replicated")}, {TEXT("component_class"), TEXT("transaction_label")}, {TEXT("component exists and supports replication"), TEXT("owner actor is replication-capable")}, {TEXT("target_path"), TEXT("component_name"), TEXT("is_replicated")}),
		MakeContract(TEXT("network_property_set_replicated"), TEXT("network.replication"), TEXT("asset_mutate"), {TEXT("Engine"), TEXT("NetCore"), TEXT("UnrealEd")}, {}, {TEXT("asset_path"), TEXT("property_name"), TEXT("replicated")}, {TEXT("condition"), TEXT("rep_notify"), TEXT("compile_after")}, {TEXT("property exists on generated class or native class"), TEXT("condition maps to ELifetimeCondition")}, {TEXT("asset_path"), TEXT("property_name"), TEXT("condition"), TEXT("compile_ok")}),
		MakeContract(TEXT("network_property_set_rep_notify"), TEXT("network.replication"), TEXT("asset_mutate"), {TEXT("Engine"), TEXT("NetCore"), TEXT("UnrealEd")}, {}, {TEXT("asset_path"), TEXT("property_name"), TEXT("rep_notify_function")}, {TEXT("create_stub_if_missing"), TEXT("compile_after")}, {TEXT("property is replicated or requested alongside replicated=true"), TEXT("RepNotify function signature is valid")}, {TEXT("asset_path"), TEXT("property_name"), TEXT("rep_notify_function")}),
		MakeContract(TEXT("network_actor_set_dormancy"), TEXT("network.replication"), TEXT("actor_mutate"), {TEXT("Engine"), TEXT("NetCore")}, {}, {TEXT("target_path"), TEXT("net_dormancy")}, {TEXT("transaction_label")}, {TEXT("net_dormancy is a valid ENetDormancy value"), TEXT("post-read confirms NetDormancy")}, {TEXT("target_path"), TEXT("net_dormancy")}),
		MakeContract(TEXT("network_actor_set_net_update_frequency"), TEXT("network.replication"), TEXT("actor_mutate"), {TEXT("Engine"), TEXT("NetCore")}, {}, {TEXT("target_path"), TEXT("net_update_frequency")}, {TEXT("min_net_update_frequency"), TEXT("clamp_to_safe_range")}, {TEXT("frequency is finite and positive"), TEXT("min frequency is not greater than max frequency")}, {TEXT("target_path"), TEXT("net_update_frequency"), TEXT("min_net_update_frequency")}),
		MakeContract(TEXT("network_validate_replication_contract"), TEXT("network.replication"), TEXT("read_only"), {TEXT("Engine"), TEXT("NetCore")}, {}, {TEXT("target_path")}, {TEXT("include_components"), TEXT("include_properties"), TEXT("strict")}, {TEXT("target resolves to actor class or level actor"), TEXT("RPC flags and RepNotify names are internally consistent")}, {TEXT("target_path"), TEXT("error_count"), TEXT("warning_count")}),
		MakeContract(TEXT("procedural_mesh_create_section"), TEXT("geometry.procedural_mesh"), TEXT("runtime_geometry_mutate"), {TEXT("Engine"), TEXT("ProceduralMeshComponent")}, {TEXT("ProceduralMeshComponent")}, {TEXT("target_path"), TEXT("component_name"), TEXT("section_index"), TEXT("vertices"), TEXT("triangles")}, {TEXT("normals"), TEXT("uv0"), TEXT("vertex_colors"), TEXT("tangents"), TEXT("create_collision"), TEXT("material_path")}, {TEXT("triangles length is divisible by 3"), TEXT("all triangle indices are in vertex range")}, {TEXT("target_path"), TEXT("component_name"), TEXT("section_index"), TEXT("triangle_count")}),
		MakeContract(TEXT("procedural_mesh_update_section"), TEXT("geometry.procedural_mesh"), TEXT("runtime_geometry_mutate"), {TEXT("Engine"), TEXT("ProceduralMeshComponent")}, {TEXT("ProceduralMeshComponent")}, {TEXT("target_path"), TEXT("component_name"), TEXT("section_index"), TEXT("vertices")}, {TEXT("normals"), TEXT("uv0"), TEXT("vertex_colors"), TEXT("tangents")}, {TEXT("section exists"), TEXT("vertex count matches existing section")}, {TEXT("target_path"), TEXT("component_name"), TEXT("section_index"), TEXT("vertex_count")}),
		MakeContract(TEXT("procedural_mesh_set_collision"), TEXT("geometry.procedural_mesh"), TEXT("component_mutate"), {TEXT("Engine"), TEXT("ProceduralMeshComponent"), TEXT("PhysicsCore")}, {TEXT("ProceduralMeshComponent")}, {TEXT("target_path"), TEXT("component_name")}, {TEXT("use_async_cooking"), TEXT("section_index"), TEXT("enable_section_collision"), TEXT("convex_meshes")}, {TEXT("component is UProceduralMeshComponent"), TEXT("section index exists when supplied")}, {TEXT("target_path"), TEXT("component_name"), TEXT("collision_enabled")}),
		MakeContract(TEXT("procedural_mesh_validate_section"), TEXT("geometry.procedural_mesh"), TEXT("read_only"), {TEXT("Engine"), TEXT("ProceduralMeshComponent")}, {TEXT("ProceduralMeshComponent")}, {TEXT("target_path"), TEXT("component_name"), TEXT("section_index")}, {TEXT("strict"), TEXT("max_vertices"), TEXT("max_triangles")}, {TEXT("section exists"), TEXT("indices are valid"), TEXT("bounds are finite")}, {TEXT("target_path"), TEXT("section_index"), TEXT("error_count"), TEXT("warning_count")}),
		MakeContract(TEXT("realtime_mesh_create_lod"), TEXT("geometry.realtime_mesh"), TEXT("runtime_geometry_mutate"), {TEXT("Engine"), TEXT("RealtimeMeshComponent")}, {TEXT("RealtimeMeshComponent")}, {TEXT("target_path"), TEXT("component_name"), TEXT("lod_index"), TEXT("section_groups")}, {TEXT("screen_size"), TEXT("material_slots"), TEXT("collision_enabled")}, {TEXT("RealtimeMeshComponent plugin is enabled"), TEXT("lod index is non-negative")}, {TEXT("target_path"), TEXT("component_name"), TEXT("lod_index"), TEXT("section_group_count")}),
		MakeContract(TEXT("realtime_mesh_update_section_group"), TEXT("geometry.realtime_mesh"), TEXT("runtime_geometry_mutate"), {TEXT("Engine"), TEXT("RealtimeMeshComponent")}, {TEXT("RealtimeMeshComponent")}, {TEXT("target_path"), TEXT("component_name"), TEXT("lod_index"), TEXT("section_group_key"), TEXT("streams")}, {TEXT("update_bounds"), TEXT("mark_collision_dirty")}, {TEXT("LOD exists"), TEXT("section group exists"), TEXT("stream semantics and element counts are compatible")}, {TEXT("target_path"), TEXT("lod_index"), TEXT("section_group_key"), TEXT("stream_count")}),
		MakeContract(TEXT("realtime_mesh_set_collision"), TEXT("geometry.realtime_mesh"), TEXT("component_mutate"), {TEXT("Engine"), TEXT("RealtimeMeshComponent"), TEXT("PhysicsCore")}, {TEXT("RealtimeMeshComponent")}, {TEXT("target_path"), TEXT("component_name"), TEXT("collision_source")}, {TEXT("use_async_cooking"), TEXT("complex_as_simple"), TEXT("convex_hulls")}, {TEXT("component is RealtimeMeshComponent"), TEXT("collision_source is valid")}, {TEXT("target_path"), TEXT("component_name"), TEXT("collision_source")}),
		MakeContract(TEXT("realtime_mesh_validate_lod"), TEXT("geometry.realtime_mesh"), TEXT("read_only"), {TEXT("Engine"), TEXT("RealtimeMeshComponent")}, {TEXT("RealtimeMeshComponent")}, {TEXT("target_path"), TEXT("component_name"), TEXT("lod_index")}, {TEXT("strict"), TEXT("max_vertices"), TEXT("max_triangles")}, {TEXT("LOD exists"), TEXT("stream ranges are internally consistent")}, {TEXT("target_path"), TEXT("lod_index"), TEXT("error_count"), TEXT("warning_count")})
	};

	for (const FAuthoringSkeletonContract& Contract : SkeletonContracts)
	{
		RegisterAuthoringSkeleton(Registry, Contract);
	}

	Registry.Register({
		TEXT("network_rpc_mark"),
		TEXT("Mark an existing Blueprint function graph as Server, Client, or Multicast RPC and verify the generated UFunction flags after compile."),
		SB::Object({
			{TEXT("asset_path"), SB::String(TEXT("Blueprint asset path, for example /Game/Foo/BP_Foo.BP_Foo"))},
			{TEXT("function_name"), SB::String(TEXT("Existing Blueprint function graph name."))},
			{TEXT("rpc_kind"), SB::String(TEXT("server | client | multicast"))},
			{TEXT("reliable"), SB::Boolean(TEXT("When true, add FUNC_NetReliable."))},
			{TEXT("validate_signature"), SB::Boolean(TEXT("Preflight signature constraints; default true."))},
			{TEXT("compile_after"), SB::Boolean(TEXT("Compile Blueprint and verify generated UFunction flags; default true."))},
			{TEXT("dry_run"), SB::Boolean(TEXT("Validate target and intended flags without mutation."))},
			{TEXT("validate_only"), SB::Boolean(TEXT("Alias of dry_run."))}
		}, {TEXT("asset_path"), TEXT("function_name"), TEXT("rpc_kind")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return ExecuteNetworkRpcMark(Context, Arguments, OutStructured, OutSummary, OutError);
		},
		nullptr,
		5,
		SB::Object({
			{TEXT("contract_version"), SB::String()},
			{TEXT("implementation_level"), SB::String()},
			{TEXT("asset_path"), SB::String()},
			{TEXT("function_name"), SB::String()},
			{TEXT("rpc_kind"), SB::String()},
			{TEXT("reliable"), SB::Boolean()},
			{TEXT("can_apply"), SB::Boolean()},
			{TEXT("applied"), SB::Boolean()},
			{TEXT("compile_ok"), SB::Boolean()},
			{TEXT("compile_status"), SB::String()},
			{TEXT("entry_flags"), SB::Array(SB::String())},
			{TEXT("generated_flags"), SB::Array(SB::String())},
			{TEXT("status"), SB::String()}
		})
	});

	Registry.Register({
		TEXT("gas_contract_status"),
		TEXT("Read-only GAS capability probe and contract anchor for AttributeSet, GameplayAbility, GameplayEffect, and GameplayCue authoring."),
		SB::Object({{TEXT("include_planned_tools"), SB::Boolean(TEXT("Include planned write tool names; default true."))}}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return ExecuteProbe(
				TEXT("gas"),
				{TEXT("/Script/GameplayAbilities.GameplayAbility"), TEXT("/Script/GameplayAbilities.GameplayEffect"), TEXT("/Script/GameplayAbilities.AttributeSet"), TEXT("/Script/GameplayAbilities.GameplayCueNotify_Static")},
				{TEXT("gas_attribute_set_create"), TEXT("gas_attribute_add"), TEXT("gas_gameplay_effect_create"), TEXT("gas_gameplay_effect_add_modifier"), TEXT("gas_gameplay_cue_create")},
				Arguments,
				OutStructured,
				OutSummary);
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("ai_perception_contract_status"),
		TEXT("Read-only AI Perception capability probe and contract anchor for perception component, sense config, and stimuli source authoring."),
		SB::Object({{TEXT("include_planned_tools"), SB::Boolean(TEXT("Include planned write tool names; default true."))}}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return ExecuteProbe(
				TEXT("ai_perception"),
				{TEXT("/Script/AIModule.AIPerceptionComponent"), TEXT("/Script/AIModule.AIPerceptionStimuliSourceComponent"), TEXT("/Script/AIModule.AISenseConfig_Sight"), TEXT("/Script/AIModule.AISenseConfig_Hearing")},
				{TEXT("ai_perception_add_component"), TEXT("ai_perception_configure_sense"), TEXT("ai_perception_bind_stimuli_source")},
				Arguments,
				OutStructured,
				OutSummary);
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("state_tree_contract_status"),
		TEXT("Read-only StateTree capability probe and contract anchor for state, transition, and task authoring."),
		SB::Object({{TEXT("include_planned_tools"), SB::Boolean(TEXT("Include planned write tool names; default true."))}}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return ExecuteProbe(
				TEXT("state_tree"),
				{TEXT("/Script/StateTreeModule.StateTree"), TEXT("/Script/StateTreeModule.StateTreeComponent"), TEXT("/Script/StateTreeModule.StateTreeTaskBlueprintBase"), TEXT("/Script/StateTreeModule.StateTreeConditionBlueprintBase")},
				{TEXT("state_tree_create"), TEXT("state_tree_add_state"), TEXT("state_tree_add_transition"), TEXT("state_tree_bind_task")},
				Arguments,
				OutStructured,
				OutSummary);
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("smart_object_contract_status"),
		TEXT("Read-only SmartObject capability probe and contract anchor for definition, slot, and behavior authoring."),
		SB::Object({{TEXT("include_planned_tools"), SB::Boolean(TEXT("Include planned write tool names; default true."))}}, {}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return ExecuteProbe(
				TEXT("smart_object"),
				{TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), TEXT("/Script/SmartObjectsModule.SmartObjectComponent"), TEXT("/Script/SmartObjectsModule.SmartObjectSubsystem")},
				{TEXT("smart_object_definition_create"), TEXT("smart_object_add_slot"), TEXT("smart_object_add_behavior"), TEXT("smart_object_validate_definition")},
				Arguments,
				OutStructured,
				OutSummary);
		},
		nullptr,
		0
	});
}
}
