// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpP2CompletionTools.cpp
// ----------------------------------------------------------------------------
// P2 completion registry for broad editor orchestration.
//
// These wrappers close the P2 name/routing/version-gate surface without
// claiming that every broad editor mutation already has a promoted concrete
// executor. The contract mirrors P0/P1: read/probe tools return structured gate
// data; mutating tools are dry-run/receipt-gated until family-specific live
// fixture proof promotes the executor.
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
namespace P2CompletionTools
{
	struct FP2ToolSpec
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

	static TSharedRef<FJsonObject> PluginGateJson(const FP2ToolSpec& Spec)
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

	static TSharedRef<FJsonObject> ReceiptRequirementsJson(const FP2ToolSpec& Spec)
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

	static TSharedRef<FJsonObject> SpecJson(const FP2ToolSpec& Spec)
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

	static bool RunP2Tool(
		const FP2ToolSpec& Spec,
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
		OutStructured->SetStringField(TEXT("status"), TEXT("p2_wrapper_ready"));
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		OutStructured->SetBoolField(TEXT("current_editor_ue58_or_later"), IsUE58OrLater());
		OutStructured->SetStringField(TEXT("current_engine_version"), CurrentEngineVersionString());
		OutStructured->SetObjectField(TEXT("spec"), SpecJson(Spec));
		OutStructured->SetArrayField(TEXT("next_steps"), StringArrayJson({
			TEXT("Use this wrapper name in P2 agent plans and queue receipts."),
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
			OutError = FString::Printf(TEXT("%s is registered as a P2 receipt-gated wrapper; concrete mutation executor must be promoted with live fixture proof before execute=true."), Spec.Name);
			OutSummary = OutError;
			return false;
		}

		OutSummary = FString::Printf(TEXT("%s P2 wrapper returned %s."), Spec.Name, bDryRun ? TEXT("dry-run execution contract") : TEXT("read/probe execution contract"));
		return true;
	}

	static void AddMany(
		TArray<FP2ToolSpec>& Out,
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

	static TArray<FP2ToolSpec> Specs()
	{
		TArray<FP2ToolSpec> Out;

		AddMany(Out, {
			TEXT("sequencer_asset_create_v3"), TEXT("sequencer_asset_inspect_v3"), TEXT("sequencer_bind_actor_v3"),
			TEXT("sequencer_binding_remove_v3"), TEXT("sequencer_binding_replace_actor"), TEXT("sequencer_binding_roles_audit"),
			TEXT("sequencer_spawnable_create"), TEXT("sequencer_possessable_create"), TEXT("sequencer_track_catalog"),
			TEXT("sequencer_track_add_transform"), TEXT("sequencer_track_add_event"), TEXT("sequencer_track_add_audio"),
			TEXT("sequencer_track_add_material"), TEXT("sequencer_track_add_visibility"), TEXT("sequencer_track_add_property"),
			TEXT("sequencer_track_remove_v2"), TEXT("sequencer_section_create_v3"), TEXT("sequencer_section_trim"),
			TEXT("sequencer_section_split"), TEXT("sequencer_section_move"), TEXT("sequencer_section_overlap_audit"),
			TEXT("sequencer_channel_catalog"), TEXT("sequencer_channel_keys_import"), TEXT("sequencer_channel_keys_export"),
			TEXT("sequencer_channel_key_tangent_set"), TEXT("sequencer_channel_key_interpolate_set"), TEXT("sequencer_keyframe_batch_add"),
			TEXT("sequencer_marker_batch_add"), TEXT("sequencer_folder_move_binding"), TEXT("sequencer_subsequence_add"),
			TEXT("sequencer_camera_binding_create_v3"), TEXT("sequencer_camera_cut_track_rebuild"), TEXT("sequencer_camera_cut_validate"),
			TEXT("sequencer_shot_track_create"), TEXT("sequencer_shot_section_add"), TEXT("sequencer_time_warp_plan"),
			TEXT("sequencer_easing_apply"), TEXT("sequencer_eval_template_refresh"), TEXT("sequencer_director_bp_open"),
			TEXT("sequencer_event_endpoint_create"), TEXT("sequencer_event_payload_set"), TEXT("sequencer_playback_settings_set"),
			TEXT("sequencer_display_rate_set"), TEXT("sequencer_tick_resolution_set"), TEXT("sequencer_render_preview_plan"),
			TEXT("sequencer_mrq_job_from_sequence"), TEXT("sequencer_mrq_preset_apply"), TEXT("sequencer_mrq_queue_submit"),
			TEXT("sequencer_mrq_job_cancel"), TEXT("sequencer_mrq_job_poll"), TEXT("sequencer_thumbnail_capture"),
			TEXT("sequencer_preview_screenshot"), TEXT("sequencer_dependency_audit"), TEXT("sequencer_compile_validate_v3"),
			TEXT("sequencer_receipt_validate_v3")
		}, TEXT("sequencer_broad_toolset"), TEXT("camera_author"), TEXT("5.8.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("SequencerScripting,MovieRenderPipeline,LevelSequenceEditor"), TEXT("LevelSequence,MovieRenderPipelineCore"), true, true);

		AddMany(Out, {
			TEXT("control_rig_asset_create_v3"), TEXT("control_rig_asset_inspect_v3"), TEXT("control_rig_hierarchy_snapshot_v3"),
			TEXT("control_rig_bone_chain_add"), TEXT("control_rig_control_shape_set"), TEXT("control_rig_control_limits_set"),
			TEXT("control_rig_space_add_v3"), TEXT("control_rig_parent_set_v3"), TEXT("control_rig_mirror_settings_get"),
			TEXT("control_rig_mirror_apply_plan"), TEXT("control_rig_graph_unit_catalog"), TEXT("control_rig_graph_node_add_v3"),
			TEXT("control_rig_graph_node_remove_v3"), TEXT("control_rig_graph_pin_schema"), TEXT("control_rig_graph_pins_connect_v3"),
			TEXT("control_rig_graph_pins_disconnect_v3"), TEXT("control_rig_graph_variable_add"), TEXT("control_rig_graph_variable_set"),
			TEXT("control_rig_graph_comment_add"), TEXT("control_rig_graph_layout_plan"), TEXT("control_rig_function_library_import"),
			TEXT("control_rig_vm_bytecode_snapshot"), TEXT("control_rig_vm_compile_v3"), TEXT("control_rig_pose_library_create"),
			TEXT("control_rig_pose_asset_apply"), TEXT("control_rig_pose_asset_capture"), TEXT("control_rig_sequencer_binding_create_v3"),
			TEXT("control_rig_sequencer_control_key_batch"), TEXT("control_rig_bake_to_control_rig_v3"), TEXT("control_rig_bake_to_anim_sequence_v3"),
			TEXT("control_rig_snapper_plan"), TEXT("control_rig_tween_keys_apply"), TEXT("control_rig_retarget_plan"),
			TEXT("control_rig_ik_setup_plan"), TEXT("control_rig_fk_ik_switch_plan"), TEXT("control_rig_metadata_get"),
			TEXT("control_rig_metadata_set"), TEXT("control_rig_preview_mesh_set"), TEXT("control_rig_debug_draw_snapshot"),
			TEXT("control_rig_validation_report_v3"), TEXT("control_rig_rollback_plan_v3"), TEXT("control_rig_receipt_validate_v3"),
			TEXT("control_rig_template_apply_plan"), TEXT("control_rig_modular_rig_bridge_plan"), TEXT("control_rig_graph_diff")
		}, TEXT("control_rig_broad_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("ControlRig,ControlRigEditor"), TEXT("ControlRig"), true, true);

		AddMany(Out, {
			TEXT("behavior_tree_asset_create_v2"), TEXT("behavior_tree_inspect_v2"), TEXT("behavior_tree_node_catalog"),
			TEXT("behavior_tree_node_add_plan"), TEXT("behavior_tree_node_update_plan"), TEXT("behavior_tree_node_remove_plan"),
			TEXT("behavior_tree_service_attach_plan"), TEXT("behavior_tree_decorator_attach_plan"), TEXT("behavior_tree_task_blueprint_create"),
			TEXT("behavior_tree_blackboard_bind"), TEXT("blackboard_asset_create_v2"), TEXT("blackboard_keys_list_v2"),
			TEXT("blackboard_key_add_v2"), TEXT("blackboard_key_update_v2"), TEXT("blackboard_key_remove_v2"),
			TEXT("state_tree_asset_create_v2"), TEXT("state_tree_schema_set_v2"), TEXT("state_tree_state_add_plan"),
			TEXT("state_tree_task_bind_plan"), TEXT("state_tree_condition_bind_plan"), TEXT("conversation_graph_create"),
			TEXT("conversation_graph_inspect"), TEXT("conversation_node_add_plan"), TEXT("conversation_link_add_plan"),
			TEXT("ai_graph_receipt_validate")
		}, TEXT("bt_statetree_conversation"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("AIModule,StateTree,GameplayStateTree,Conversation"), TEXT("AIModule,StateTreeModule"), false, true);

		AddMany(Out, {
			TEXT("teds_availability_probe"), TEXT("teds_table_catalog"), TEXT("teds_row_query"), TEXT("teds_row_inspect"),
			TEXT("teds_column_catalog"), TEXT("teds_column_schema"), TEXT("teds_actor_table_snapshot"), TEXT("teds_world_partition_table_snapshot"),
			TEXT("teds_outliner_table_snapshot"), TEXT("teds_selection_table_snapshot"), TEXT("teds_asset_table_snapshot"),
			TEXT("teds_dirty_rows_audit"), TEXT("teds_row_tag_add_plan"), TEXT("teds_row_tag_remove_plan"),
			TEXT("teds_blackboard_sync_snapshot"), TEXT("teds_query_to_asset_set"), TEXT("teds_query_receipt_validate"),
			TEXT("teds_cache_freshness_audit")
		}, TEXT("editor_data_storage_teds"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("editor_read_or_plan"), TEXT("receipt_gated"), TEXT("EditorDataStorage"), TEXT("EditorDataStorage"), true, true);

		AddMany(Out, {
			TEXT("material_validation_run_v2"), TEXT("material_validation_report_get"), TEXT("material_validation_issue_classify"),
			TEXT("material_validation_fix_plan"), TEXT("material_asset_wizard_probe"), TEXT("material_asset_wizard_instance_plan"),
			TEXT("material_asset_wizard_layered_material_plan"), TEXT("material_usage_audit_v2"), TEXT("material_validation_receipt_validate"),
			TEXT("material_asset_wizard_receipt_validate")
		}, TEXT("material_validation_wizard"), TEXT("material_author"), TEXT("5.8.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("MaterialValidation,MaterialAssetWizard"), TEXT("MaterialEditor"), true, true);

		AddMany(Out, {
			TEXT("insights_trace_session_start"), TEXT("insights_trace_session_stop"), TEXT("insights_trace_session_status"),
			TEXT("insights_niagara_capture_plan"), TEXT("insights_audio_capture_plan"), TEXT("insights_render_trace_capture_plan"),
			TEXT("insights_render_graph_snapshot"), TEXT("insights_mass_trace_capture_plan"), TEXT("insights_chaosvd_capture_plan"),
			TEXT("insights_network_prediction_capture_plan"), TEXT("insights_cpu_timing_snapshot"), TEXT("insights_gpu_timing_snapshot"),
			TEXT("insights_memory_snapshot"), TEXT("insights_trace_export_plan"), TEXT("insights_trace_receipt_validate"),
			TEXT("insights_capture_failure_classify")
		}, TEXT("insights_profiling_receipts"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("editor_read_or_capture"), TEXT("receipt_gated"), TEXT("TraceInsights,TimingInsights,RenderGraphInsights,ChaosVD"), TEXT("TraceServices"), true, true);

		AddMany(Out, {
			TEXT("sandboxed_editing_probe"), TEXT("sandboxed_asset_write_plan"), TEXT("sandboxed_asset_diff_preview"),
			TEXT("sandboxed_asset_commit"), TEXT("sandboxed_asset_revert"), TEXT("sandboxed_editing_receipt_validate")
		}, TEXT("sandboxed_editing_file_sandbox"), TEXT("file_ops"), TEXT("5.8.0"), TEXT("asset_write_plan"), TEXT("receipt_gated"), TEXT("SandboxedEditing,FileSandbox"), TEXT("UnrealEd"), true, true);

		AddMany(Out, {
			TEXT("semantic_search_index_probe"), TEXT("semantic_search_asset_query"), TEXT("semantic_search_asset_neighbors"),
			TEXT("semantic_search_result_explain"), TEXT("semantic_search_index_freshness_audit"), TEXT("semantic_search_receipt_validate")
		}, TEXT("semantic_search_asset_intelligence"), TEXT("resource_finder"), TEXT("5.8.0"), TEXT("editor_read"), TEXT("readonly_or_receipt"), TEXT("SemanticSearch"), TEXT("AssetRegistry"), true, false);

		AddMany(Out, {
			TEXT("audio_modulation_bus_create"), TEXT("audio_modulation_bus_inspect"), TEXT("audio_modulation_parameter_create"),
			TEXT("audio_modulation_patch_plan"), TEXT("audio_synesthesia_analyzer_create"), TEXT("audio_synesthesia_analysis_run"),
			TEXT("audio_capture_config_create"), TEXT("audio_capture_record_plan"), TEXT("audio_widget_meter_create_plan"),
			TEXT("audio_gameplay_volume_create"), TEXT("audio_submix_effect_chain_inspect"), TEXT("audio_submix_effect_chain_set_plan"),
			TEXT("audio_soundscape_palette_plan"), TEXT("audio_mixer_snapshot"), TEXT("audio_asset_loudness_analyze"),
			TEXT("audio_asset_loop_point_audit"), TEXT("audio_spatialization_config_plan"), TEXT("audio_receipt_validate")
		}, TEXT("audio_production_wrappers"), TEXT("audio_author"), TEXT("5.7.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("AudioModulation,AudioSynesthesia,Soundscape,AudioWidgets"), TEXT("AudioMixer,Engine"), false, true);

		AddMany(Out, {
			TEXT("asset_metadata_get_v2"), TEXT("asset_metadata_set_v2"), TEXT("asset_metadata_bulk_apply"),
			TEXT("asset_duplicate_v2"), TEXT("asset_rename_v2"), TEXT("asset_move_v2"), TEXT("asset_consolidate_plan"),
			TEXT("asset_redirectors_fixup_v2"), TEXT("asset_dependency_graph_v2"), TEXT("asset_referencers_list_v2"),
			TEXT("asset_collection_create"), TEXT("asset_collection_add_items"), TEXT("asset_collection_remove_items"),
			TEXT("asset_batch_import_task_plan"), TEXT("asset_import_task_execute_safe"), TEXT("asset_import_receipt_validate"),
			TEXT("actor_group_create"), TEXT("actor_group_add"), TEXT("actor_group_remove"), TEXT("actor_group_ungroup"),
			TEXT("editor_utility_widget_run_safe"), TEXT("editor_utility_blueprint_run_safe"), TEXT("asset_editor_open_v2"),
			TEXT("asset_editor_close_v2"), TEXT("asset_editor_focus_v2"), TEXT("asset_editor_dirty_state"),
			TEXT("asset_editor_save_safe"), TEXT("asset_editor_compile_active"), TEXT("editor_subsystem_catalog"),
			TEXT("editor_subsystem_call_plan"), TEXT("editor_transaction_snapshot"), TEXT("editor_asset_scripting_receipt_validate")
		}, TEXT("editor_subsystems_asset_scripting"), TEXT("asset_manager"), TEXT("5.7.0"), TEXT("asset_write_or_editor_write"), TEXT("receipt_gated"), TEXT("EditorScriptingUtilities,EditorUtilityWidget"), TEXT("UnrealEd,AssetRegistry"), false, true);

		return Out;
	}

	static bool IsRegistered(const FSololmcpToolRegistry& Registry, const FString& Name)
	{
		TArray<FString> Names;
		Registry.GetRegisteredToolNamesSorted(Names);
		return Names.Contains(Name);
	}
}

void RegisterP2CompletionTools(FSololmcpToolRegistry& Registry)
{
	using namespace P2CompletionTools;
	for (const FP2ToolSpec& Spec : Specs())
	{
		if (IsRegistered(Registry, Spec.Name))
		{
			continue;
		}

		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = FString::Printf(TEXT("P2 %s wrapper for %s."), Spec.Family, Spec.AgentRole);
		Def.InputSchema = ToolInputSchema();
		Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return RunP2Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}
}
