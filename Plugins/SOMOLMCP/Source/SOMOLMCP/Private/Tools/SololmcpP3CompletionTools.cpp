// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpP3CompletionTools.cpp
// ----------------------------------------------------------------------------
// P3 completion registry for long-tail and experimental coverage.
//
// These wrappers close the P3 name/routing/version-gate surface without
// claiming that experimental editor mutations already have promoted concrete
// executors. The contract mirrors P0/P1/P2: read/probe tools return structured
// gate data; mutating tools are dry-run/receipt-gated until family-specific
// live fixture proof promotes the executor.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"

#include <initializer_list>

namespace UE::SOMOLMCP
{
namespace P3CompletionTools
{
	struct FP3ToolSpec
	{
		const TCHAR* Name;
		const TCHAR* Family;
		const TCHAR* AgentRole;
		const TCHAR* Version;
		const TCHAR* OperationClass;
		const TCHAR* SafetyClass;
		const TCHAR* RequiredPlugins;
		const TCHAR* RequiredModules;
		bool bUE58Only = false;
		bool bMutation = false;
	};

	static TArray<FString> SplitCsv(const FString& Csv)
	{
		TArray<FString> Values;
		Csv.ParseIntoArray(Values, TEXT(","), true);
		for (FString& Value : Values)
		{
			Value.TrimStartAndEndInline();
		}
		Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
		return Values;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		Json.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		return Json;
	}

	static bool IsUE58OrLater()
	{
		const FEngineVersion Current = FEngineVersion::Current();
		return Current.GetMajor() > 5 || (Current.GetMajor() == 5 && Current.GetMinor() >= 8);
	}

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static TSharedRef<FJsonObject> ToolInputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return execution plan and gates without mutating editor state. Default true."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request actual execution for promoted concrete wrappers. Default false."))},
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target asset path for asset-writing tools."))},
			{TEXT("target_object_path"), FSololmcpSchemaBuilder::String(TEXT("Target object path for editor/object tools."))},
			{TEXT("target_level"), FSololmcpSchemaBuilder::String(TEXT("Target level/map path for level-writing tools."))},
			{TEXT("receipt_id"), FSololmcpSchemaBuilder::String(TEXT("Optional upstream receipt id for long queue correlation."))},
			{TEXT("args"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Tool-specific arguments."))}
		});
	}

	static TSharedRef<FJsonObject> PluginGateJson(const FP3ToolSpec& Spec)
	{
		const TArray<FString> Plugins = SplitCsv(Spec.RequiredPlugins);
		const TArray<FString> Modules = SplitCsv(Spec.RequiredModules);

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("required_plugins"), StringArrayJson(Plugins));
		Obj->SetArrayField(TEXT("required_modules"), StringArrayJson(Modules));

		TArray<TSharedPtr<FJsonValue>> PluginStatus;
		for (const FString& PluginName : Plugins)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			Row->SetStringField(TEXT("name"), PluginName);
			Row->SetBoolField(TEXT("found"), Plugin.IsValid());
			Row->SetBoolField(TEXT("enabled"), Plugin.IsValid() && Plugin->IsEnabled());
			PluginStatus.Add(MakeShared<FJsonValueObject>(Row));
		}
		Obj->SetArrayField(TEXT("plugin_status"), PluginStatus);

		TArray<TSharedPtr<FJsonValue>> ModuleStatus;
		for (const FString& ModuleName : Modules)
		{
			FString ModulePath;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), ModuleName);
			Row->SetBoolField(TEXT("exists"), ModuleExistsCompat(*ModuleName, &ModulePath));
			Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(*ModuleName)));
			if (!ModulePath.IsEmpty())
			{
				Row->SetStringField(TEXT("module_file"), ModulePath);
			}
			ModuleStatus.Add(MakeShared<FJsonValueObject>(Row));
		}
		Obj->SetArrayField(TEXT("module_status"), ModuleStatus);
		return Obj;
	}

	static TSharedRef<FJsonObject> ReceiptRequirementsJson(const FP3ToolSpec& Spec)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("required_for_all"), StringArrayJson({
			TEXT("target binding where applicable"),
			TEXT("version and plugin/module gate"),
			TEXT("preflight capability or schema snapshot"),
			TEXT("post-call readback or explicit not-executed state"),
			TEXT("structured failure route")
		}));
		if (Spec.bMutation)
		{
			Obj->SetArrayField(TEXT("required_for_execute"), StringArrayJson({
				TEXT("dry-run pass"),
				TEXT("resource lock"),
				TEXT("transaction or rollback note"),
				TEXT("post-edit readback"),
				TEXT("compile / validate / preview / QA receipt when relevant"),
				TEXT("save policy or rollback state")
			}));
		}
		else
		{
			Obj->SetArrayField(TEXT("required_for_execute"), StringArrayJson({
				TEXT("read-only response proof"),
				TEXT("cache/staleness note if cached")
			}));
		}
		return Obj;
	}

	static TSharedRef<FJsonObject> SpecJson(const FP3ToolSpec& Spec)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Spec.Name);
		Obj->SetStringField(TEXT("family"), Spec.Family);
		Obj->SetStringField(TEXT("agent_role"), Spec.AgentRole);
		Obj->SetStringField(TEXT("min_engine_version"), Spec.Version);
		Obj->SetBoolField(TEXT("ue58_only"), Spec.bUE58Only);
		Obj->SetStringField(TEXT("operation_class"), Spec.OperationClass);
		Obj->SetStringField(TEXT("safety_class"), Spec.SafetyClass);
		Obj->SetBoolField(TEXT("mutation"), Spec.bMutation);
		Obj->SetObjectField(TEXT("plugin_gate"), PluginGateJson(Spec));
		Obj->SetObjectField(TEXT("receipt_requirements"), ReceiptRequirementsJson(Spec));
		return Obj;
	}

	static bool RunP3Tool(
		const FP3ToolSpec& Spec,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);

		OutStructured->SetStringField(TEXT("tool_name"), Spec.Name);
		OutStructured->SetStringField(TEXT("status"), TEXT("p3_wrapper_ready"));
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		OutStructured->SetBoolField(TEXT("current_editor_ue58_or_later"), IsUE58OrLater());
		OutStructured->SetStringField(TEXT("current_engine_version"), CurrentEngineVersionString());
		OutStructured->SetObjectField(TEXT("spec"), SpecJson(Spec));
		OutStructured->SetArrayField(TEXT("next_steps"), StringArrayJson({
			TEXT("Use this wrapper name in P3 agent plans and queue receipts."),
			TEXT("Check version/plugin/module gate before execution."),
			TEXT("Attach target binding and receipt evidence."),
			TEXT("Promote concrete executor only after family-specific live fixture proof.")
		}));

		if (Spec.bUE58Only && !IsUE58OrLater())
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("requires_ue_5_8"));
			OutSummary = FString::Printf(TEXT("%s requires UE 5.8; current engine is %s."), Spec.Name, *CurrentEngineVersionString());
			return true;
		}

		if (bExecute && Spec.bMutation)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_concrete_executor"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("promote_family_specific_executor_after_live_fixture"));
			OutError = FString::Printf(TEXT("%s is registered as a P3 receipt-gated wrapper; concrete mutation executor must be promoted with live fixture proof before execute=true."), Spec.Name);
			OutSummary = OutError;
			return false;
		}

		OutSummary = FString::Printf(TEXT("%s P3 wrapper returned %s."), Spec.Name, bDryRun ? TEXT("dry-run execution contract") : TEXT("read/probe execution contract"));
		return true;
	}

	static void AddMany(
		TArray<FP3ToolSpec>& Out,
		std::initializer_list<const TCHAR*> Names,
		const TCHAR* Family,
		const TCHAR* Role,
		const TCHAR* Version,
		const TCHAR* OperationClass,
		const TCHAR* SafetyClass,
		const TCHAR* Plugins,
		const TCHAR* Modules,
		bool bUE58Only,
		bool bMutation)
	{
		for (const TCHAR* Name : Names)
		{
			Out.Add({Name, Family, Role, Version, OperationClass, SafetyClass, Plugins, Modules, bUE58Only, bMutation});
		}
	}

	static TArray<FP3ToolSpec> Specs()
	{
		TArray<FP3ToolSpec> Out;

		AddMany(Out, {
			TEXT("nne_runtime_catalog_probe"), TEXT("nne_model_asset_create_plan"), TEXT("nne_model_asset_inspect"),
			TEXT("nne_model_input_schema"), TEXT("nne_model_output_schema"), TEXT("nne_model_validate_runtime"),
			TEXT("nne_model_inference_dry_run"), TEXT("nne_model_batch_inference_plan"), TEXT("neural_rendering_feature_probe"),
			TEXT("neural_rendering_material_plan"), TEXT("mlflow_run_snapshot_get"), TEXT("tensorboard_log_snapshot_get")
		}, TEXT("nne_neural_rendering_ml"), TEXT("external_tools"), TEXT("5.8.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("NNE,NeuralRendering,MLDeformerFramework"), TEXT("NNE"), true, true);

		AddMany(Out, {
			TEXT("media_profile_list_v2"), TEXT("media_source_create_plan"), TEXT("media_source_inspect_v2"),
			TEXT("media_player_create_plan"), TEXT("media_player_open_test"), TEXT("media_texture_bind_plan"),
			TEXT("media_plate_actor_create_plan"), TEXT("media_capture_device_probe"), TEXT("rtsp_stream_probe"),
			TEXT("live_link_capture_source_plan"), TEXT("ki_pro_capture_plan"), TEXT("capture_ingest_receipt_validate")
		}, TEXT("media_codecs_capture_baseline"), TEXT("media_author"), TEXT("5.7.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("MediaFrameworkUtilities,MediaPlate,LiveLink"), TEXT("MediaAssets"), false, true);

		AddMany(Out, {
			TEXT("ue58_apv_codec_probe"), TEXT("ue58_av1_encode_profile_probe")
		}, TEXT("media_codecs_capture_ue58_delta"), TEXT("media_author"), TEXT("5.8.0"), TEXT("editor_read"), TEXT("readonly_or_receipt"), TEXT("APVMedia,AVCodecs"), TEXT("MediaAssets"), true, false);

		AddMany(Out, {
			TEXT("mesh_partition_actor_create_plan"), TEXT("mesh_partition_actor_inspect"), TEXT("mesh_partition_section_list"),
			TEXT("mesh_partition_section_bounds"), TEXT("mesh_partition_modifier_catalog"), TEXT("mesh_partition_modifier_apply_plan"),
			TEXT("mesh_partition_cache_snapshot"), TEXT("mesh_partition_cache_invalidate_plan"), TEXT("mesh_partition_generated_asset_readback"),
			TEXT("mesh_partition_receipt_validate"), TEXT("mesh_terrain_tool_command_catalog"), TEXT("mesh_terrain_tool_command_dry_run"),
			TEXT("mesh_terrain_deep_sculpt_plan"), TEXT("mesh_terrain_patch_project_plan"), TEXT("mesh_terrain_weightmap_projection_plan"),
			TEXT("mesh_terrain_convert_receipt_validate"), TEXT("mesh_terrain_deep_preview_capture"), TEXT("mesh_terrain_rollback_receipt")
		}, TEXT("mesh_partition_mesh_terrain_deep"), TEXT("terrain_author"), TEXT("5.8.0"), TEXT("asset_write_or_plan"), TEXT("receipt_gated"), TEXT("MeshPartition,MeshTerrainMode"), TEXT("MeshTerrainMode"), true, true);

		AddMany(Out, {
			TEXT("derived_data_build_controller_probe"), TEXT("derived_data_build_request_create"), TEXT("derived_data_build_status_get"),
			TEXT("derived_data_build_cancel"), TEXT("derived_data_build_queue_snapshot"), TEXT("derived_data_build_receipt_validate")
		}, TEXT("derived_data_build_controller"), TEXT("devops_author"), TEXT("5.8.0"), TEXT("editor_read_or_build"), TEXT("receipt_gated"), TEXT("DerivedDataBuildController"), TEXT("DerivedDataCache"), true, true);

		AddMany(Out, {
			TEXT("ue_rpc_base_probe"), TEXT("ue_rpc_endpoint_catalog"), TEXT("automation_controller_rpc_status"),
			TEXT("automation_controller_rpc_run_plan"), TEXT("automation_controller_rpc_result_snapshot"), TEXT("automation_controller_rpc_cancel")
		}, TEXT("rpcbase_automation_controller_rpc"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("editor_read_or_automation"), TEXT("receipt_gated"), TEXT("RPCBase,AutomationControllerRpc"), TEXT("AutomationController"), true, true);

		AddMany(Out, {
			TEXT("plugin_api_stability_audit"), TEXT("preview_api_guard_report"), TEXT("private_header_dependency_report"),
			TEXT("module_load_failure_classify"), TEXT("wrapper_generation_fixture_create"), TEXT("wrapper_generation_fixture_validate"),
			TEXT("source_symbol_index_build_plan"), TEXT("source_symbol_index_query"), TEXT("source_uclass_callable_diff"),
			TEXT("source_blueprint_callable_gap_report"), TEXT("source_tool_name_collision_audit"), TEXT("source_receipt_contract_lint"),
			TEXT("source_version_guard_lint"), TEXT("source_plugin_gate_lint"), TEXT("source_agent_role_mapping_lint"),
			TEXT("source_wrapper_risk_rank"), TEXT("experimental_tool_promotion_plan"), TEXT("experimental_tool_retirement_plan")
		}, TEXT("experimental_diagnostics_source_index"), TEXT("cpp_author"), TEXT("5.8.0"), TEXT("editor_read_or_plan"), TEXT("receipt_gated"), TEXT("ToolMenus,UnrealEd"), TEXT("UnrealEd"), true, true);

		return Out;
	}

	static bool IsRegistered(const FSololmcpToolRegistry& Registry, const FString& Name)
	{
		TArray<FString> Names;
		Registry.GetRegisteredToolNamesSorted(Names);
		return Names.Contains(Name);
	}
}

void RegisterP3CompletionTools(FSololmcpToolRegistry& Registry)
{
	using namespace P3CompletionTools;
	for (const FP3ToolSpec& Spec : Specs())
	{
		if (IsRegistered(Registry, Spec.Name))
		{
			continue;
		}

		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = FString::Printf(TEXT("P3 %s wrapper for %s."), Spec.Family, Spec.AgentRole);
		Def.InputSchema = ToolInputSchema();
		Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return RunP3Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}
}
