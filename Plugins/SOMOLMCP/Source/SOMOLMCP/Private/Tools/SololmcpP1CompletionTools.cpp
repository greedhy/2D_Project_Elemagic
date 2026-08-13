// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpP1CompletionTools.cpp
// ----------------------------------------------------------------------------
// P1 completion registry for production feature coverage.
//
// These wrappers close the P1 name/routing/version-gate surface without
// pretending every mutating editor operation already has a promoted concrete
// executor. The contract mirrors P0: read/probe tools return structured gate
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
namespace P1CompletionTools
{
	struct FP1ToolSpec
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

	static TSharedRef<FJsonObject> PluginGateJson(const FP1ToolSpec& Spec)
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

	static TSharedRef<FJsonObject> ReceiptRequirementsJson(const FP1ToolSpec& Spec)
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

	static TSharedRef<FJsonObject> SpecJson(const FP1ToolSpec& Spec)
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

	static bool RunP1Tool(
		const FP1ToolSpec& Spec,
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
		OutStructured->SetStringField(TEXT("status"), TEXT("p1_wrapper_ready"));
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		OutStructured->SetBoolField(TEXT("current_editor_ue58_or_later"), IsUE58OrLater());
		OutStructured->SetStringField(TEXT("current_engine_version"), CurrentEngineVersionString());
		OutStructured->SetObjectField(TEXT("spec"), SpecJson(Spec));
		OutStructured->SetArrayField(TEXT("next_steps"), StringArrayJson({
			TEXT("Use this wrapper name in P1 agent plans and queue receipts."),
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
			OutError = FString::Printf(TEXT("%s is registered as a P1 receipt-gated wrapper; concrete mutation executor must be promoted with live fixture proof before execute=true."), Spec.Name);
			OutSummary = OutError;
			return false;
		}

		OutSummary = FString::Printf(TEXT("%s P1 wrapper returned %s."), Spec.Name, bDryRun ? TEXT("dry-run execution contract") : TEXT("read/probe execution contract"));
		return true;
	}

	static void AddMany(
		TArray<FP1ToolSpec>& Out,
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

	static TArray<FP1ToolSpec> Specs()
	{
		TArray<FP1ToolSpec> Out;

		AddMany(Out, {
			TEXT("pcg_dynamic_mesh_nodes_catalog"), TEXT("pcg_spline_to_mesh_node_add"), TEXT("pcg_spawn_dynamic_mesh_node_add"),
			TEXT("pcg_set_dynamic_mesh_materials_node_add"), TEXT("pcg_save_dynamic_mesh_to_asset_node_add"), TEXT("pcg_mesh_to_dynamic_mesh_node_add"),
			TEXT("pcg_mesh_sampler_node_add"), TEXT("pcg_merge_dynamic_meshes_node_add"), TEXT("pcg_get_dynamic_mesh_materials_node_add"),
			TEXT("pcg_dynamic_mesh_pipeline_plan"), TEXT("pcg_dynamic_mesh_pipeline_validate"), TEXT("pcg_alembic_export_to_pcg_asset"),
			TEXT("pcg_alembic_standard_setup"), TEXT("pcg_external_data_asset_validate"), TEXT("pcg_external_data_mapping_plan"),
			TEXT("pcg_python_node_add"), TEXT("pcg_python_node_set_script_file"), TEXT("pcg_python_node_set_input_attribute"),
			TEXT("pcg_python_node_security_audit"), TEXT("pcg_python_node_dry_run"), TEXT("pcg_python_node_receipt"),
			TEXT("pcg_water_spline_node_add_v2"), TEXT("pcg_niagara_interop_node_catalog"), TEXT("pcg_nanite_assembly_builder_add"),
			TEXT("pcg_generated_dependency_readback"), TEXT("pcg_generated_actor_bounds_readback"), TEXT("pcg_spawned_actor_material_audit"),
			TEXT("pcg_biome_density_receipt_validate"), TEXT("pcg_tile_budget_estimate_v2"), TEXT("pcg_spatial_data_stats"),
			TEXT("pcg_point_data_schema_inspect"), TEXT("pcg_attribute_histogram"), TEXT("pcg_graph_param_set_v2"),
			TEXT("pcg_graph_param_snapshot"), TEXT("pcg_interop_failure_classify_v2"), TEXT("pcg_generated_mesh_asset_promote_plan"),
			TEXT("pcg_dynamic_mesh_preview_capture"), TEXT("pcg_interop_receipt_validate")
		}, TEXT("pcg_deep_interops"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("PCG,PCGGeometryScriptInterop,PCGPythonInterop,Water,Niagara"), TEXT("PCG"), false, true);

		AddMany(Out, {
			TEXT("pcg_fastgeo_interop_probe"), TEXT("pcg_mesh_partition_adapter_attach"), TEXT("pcg_mesh_partition_query_node_add"),
			TEXT("pcg_mesh_partition_projection_spawner_add"), TEXT("pcg_mesh_partition_patch_spawner_add"), TEXT("pcg_mesh_partition_health_audit")
		}, TEXT("pcg_mesh_partition_delta"), TEXT("pcg_author"), TEXT("5.8.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("PCGMeshPartitionInterop"), TEXT("PCG"), true, true);

		AddMany(Out, {
			TEXT("mesh_terrain_mode_probe"), TEXT("mesh_terrain_palette_list"), TEXT("mesh_terrain_create_asset_plan"),
			TEXT("mesh_terrain_sculpt_brush_catalog"), TEXT("mesh_terrain_apply_heightfield_to_mesh"), TEXT("mesh_terrain_bake_attribute_maps"),
			TEXT("mesh_terrain_convert_to_landscape_plan"), TEXT("mesh_terrain_preview_capture"), TEXT("mesh_terrain_material_layer_plan"),
			TEXT("mesh_terrain_collision_plan"), TEXT("mesh_terrain_lod_plan"), TEXT("mesh_terrain_receipt_validate")
		}, TEXT("mesh_terrain_mode"), TEXT("terrain_author"), TEXT("5.8.0"), TEXT("asset_write_or_plan"), TEXT("receipt_gated"), TEXT("MeshTerrainMode"), TEXT("MeshTerrainMode"), true, true);

		AddMany(Out, {
			TEXT("niagara_toolset_schema_get"), TEXT("niagara_system_topology_v2"), TEXT("niagara_stack_input_schema"),
			TEXT("niagara_module_schema_from_asset"), TEXT("niagara_dynamic_input_schema"), TEXT("niagara_user_vars_add_v2"),
			TEXT("niagara_module_add_v2"), TEXT("niagara_renderer_add_v2"), TEXT("niagara_stack_input_set_v2"),
			TEXT("niagara_stack_issues_get"), TEXT("niagara_stack_issue_fix_apply"), TEXT("niagara_bp_wrapper_create"),
			TEXT("niagara_emitter_template_apply_v2"), TEXT("niagara_parameter_binding_audit"), TEXT("niagara_data_interface_schema_get"),
			TEXT("niagara_sim_cache_capture_v2"), TEXT("niagara_preview_render_receipt"), TEXT("niagara_toolset_compile_validate"),
			TEXT("niagara_toolset_rollback_plan"), TEXT("niagara_toolset_receipt_validate")
		}, TEXT("niagara_toolset_upgrade"), TEXT("vfx_author"), TEXT("5.8.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("NiagaraToolsets,Niagara"), TEXT("Niagara"), true, true);

		AddMany(Out, {
			TEXT("mover_component_attach"), TEXT("mover_character_component_setup"), TEXT("mover_nav_component_setup"), TEXT("mover_modes_list"),
			TEXT("mover_mode_set"), TEXT("mover_mode_config_get"), TEXT("mover_mode_config_set"), TEXT("mover_layered_move_queue"),
			TEXT("mover_instant_effect_queue"), TEXT("mover_blackboard_inspect"), TEXT("mover_debug_snapshot"), TEXT("mover_trajectory_predict"),
			TEXT("mover_nav_avoidance_set"), TEXT("mover_receipt_validate"), TEXT("mover_pose_search_trajectory_predictor_attach"),
			TEXT("mover_root_motion_toggle_plan"), TEXT("mover_motion_matching_setup_plan"), TEXT("mover_component_state_snapshot"),
			TEXT("mover_input_mapping_plan"), TEXT("mover_network_prediction_audit"), TEXT("mover_floor_query_debug"), TEXT("mover_movement_mode_receipt")
		}, TEXT("mover_authoring"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("asset_write_or_runtime_plan"), TEXT("receipt_gated"), TEXT("Mover,PoseSearch"), TEXT("Mover"), false, true);

		AddMany(Out, {
			TEXT("mover_animnext_trajectory_bridge_plan"), TEXT("animnext_graph_entries_list"), TEXT("animnext_graph_entry_add"),
			TEXT("uaf_capability_inventory"), TEXT("uaf_component_attach"), TEXT("uaf_variable_set"), TEXT("uaf_injection_request"),
			TEXT("uaf_play_anim_request"), TEXT("uaf_graph_template_apply"), TEXT("uaf_warping_trait_plan"), TEXT("uaf_layering_setup_plan"),
			TEXT("uaf_pose_search_bridge_plan"), TEXT("uaf_validation_receipt"), TEXT("animnext_graph_compile_validate"),
			TEXT("animnext_parameter_schema_get"), TEXT("animnext_variable_set_plan"), TEXT("animnext_pose_graph_snapshot"),
			TEXT("uaf_animation_asset_bind"), TEXT("uaf_runtime_state_snapshot"), TEXT("uaf_trait_catalog"), TEXT("uaf_trait_apply_plan"),
			TEXT("uaf_receipt_validate")
		}, TEXT("uaf_animnext_bridge"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write_or_runtime_plan"), TEXT("receipt_gated"), TEXT("UAF,AnimNext"), TEXT("UAF,AnimNext"), true, true);

		AddMany(Out, {
			TEXT("metahuman_character_assets_list"), TEXT("metahuman_character_instance_inspect"), TEXT("metahuman_character_export_dcc"),
			TEXT("metahuman_character_export_dna"), TEXT("metahuman_character_export_geometry"), TEXT("metahuman_character_export_materials"),
			TEXT("metahuman_crowd_spawner_create"), TEXT("metahuman_crowd_actor_bind"), TEXT("metahuman_livelink_subject_config"),
			TEXT("metahuman_calibration_diagnostics_run"), TEXT("metahuman_asset_report"), TEXT("metahuman_receipt_validate"),
			TEXT("metahuman_body_type_inspect"), TEXT("metahuman_groom_asset_audit"), TEXT("metahuman_anim_blueprint_bind_plan"),
			TEXT("metahuman_preview_capture"), TEXT("metahuman_crowd_variation_plan"), TEXT("metahuman_crowd_mass_bridge_plan"),
			TEXT("mutable_customizable_object_inspect"), TEXT("mutable_instance_create"), TEXT("mutable_instance_parameter_list"),
			TEXT("mutable_instance_parameter_set"), TEXT("mutable_instance_generate_mesh"), TEXT("mutable_instance_update"),
			TEXT("mutable_population_asset_create"), TEXT("mutable_population_class_create"), TEXT("mutable_population_generate_preview"),
			TEXT("mutable_validation_run"), TEXT("mutable_compile_status"), TEXT("mutable_receipt_validate"), TEXT("mutable_material_variant_audit"),
			TEXT("mutable_texture_parameter_audit"), TEXT("mutable_lod_variant_plan"), TEXT("mutable_dependency_graph"),
			TEXT("mutable_population_randomize_plan"), TEXT("mutable_preview_receipt")
		}, TEXT("metahuman_mutable"), TEXT("character_designer"), TEXT("5.7.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("MetaHuman,Mutable,MutablePopulation"), TEXT("MetaHuman,Mutable"), false, true);

		AddMany(Out, {
			TEXT("cloth_asset_inspect"), TEXT("cloth_collection_schema_inspect"), TEXT("cloth_fabric_params_get"), TEXT("cloth_fabric_params_set"),
			TEXT("cloth_sim_pattern_list"), TEXT("cloth_render_pattern_list"), TEXT("cloth_seams_inspect"), TEXT("cloth_weightmap_paint_plan"),
			TEXT("cloth_dataflow_graph_create"), TEXT("cloth_dataflow_node_add"), TEXT("cloth_sim_preview_receipt"), TEXT("outfit_asset_create"),
			TEXT("outfit_source_add"), TEXT("outfit_size_variants_set"), TEXT("outfit_skin_weight_audit"), TEXT("outfit_receipt_validate"),
			TEXT("cloth_weightmap_audit_v2"), TEXT("cloth_collision_config_get"), TEXT("cloth_collision_config_set"), TEXT("cloth_sim_params_snapshot"),
			TEXT("cloth_lod_section_bind"), TEXT("cloth_material_binding_audit"), TEXT("cloth_dataflow_compile_validate"),
			TEXT("outfit_body_compat_audit"), TEXT("outfit_material_slot_map"), TEXT("outfit_preview_capture")
		}, TEXT("cloth_outfit_dataflow"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("ChaosClothAsset,Dataflow,Outfit"), TEXT("DataflowEngine"), true, true);

		AddMany(Out, {
			TEXT("water_body_create_v2"), TEXT("water_body_spline_set"), TEXT("water_zone_create_v2"), TEXT("water_material_set_v2"),
			TEXT("water_body_readback_snapshot"), TEXT("water_flowmap_generate_plan"), TEXT("water_river_lake_pipeline_plan"),
			TEXT("water_collision_nav_audit"), TEXT("landscape_texture_patch_create_v2"), TEXT("landscape_patch_component_inspect"),
			TEXT("landscape_patch_apply_receipt"), TEXT("landscape_patch_rollback_plan"), TEXT("landscape_patch_edit_layer_bind"),
			TEXT("landscape_patch_texture_readback"), TEXT("landscape_patch_visibility_mask_plan"), TEXT("landscape_patch_height_blend_plan"),
			TEXT("landscape_patch_material_layer_sync"), TEXT("water_landscape_receipt_validate")
		}, TEXT("water_landscape_patch"), TEXT("terrain_author"), TEXT("5.7.0"), TEXT("level_write_or_asset_write"), TEXT("receipt_gated"), TEXT("Water,LandscapePatch"), TEXT("Water,LandscapePatch"), false, true);

		AddMany(Out, {
			TEXT("smart_object_definition_create"), TEXT("smart_object_definition_inspect"), TEXT("smart_object_slot_add"),
			TEXT("smart_object_slot_remove"), TEXT("smart_object_slot_inspect"), TEXT("smart_object_query"), TEXT("smart_object_claim"),
			TEXT("smart_object_release"), TEXT("smart_object_runtime_snapshot"), TEXT("smart_object_receipt_validate"),
			TEXT("state_tree_asset_create"), TEXT("state_tree_schema_inspect"), TEXT("state_tree_task_add_plan"), TEXT("state_tree_condition_add_plan"),
			TEXT("state_tree_transition_add_plan"), TEXT("state_tree_compile"), TEXT("state_tree_debug_snapshot"), TEXT("state_tree_receipt_validate"),
			TEXT("mass_entity_config_create"), TEXT("mass_entity_config_inspect"), TEXT("mass_spawner_create"), TEXT("mass_spawner_spawn_data_set"),
			TEXT("mass_crowd_lane_bind"), TEXT("mass_zonegraph_sync_plan"), TEXT("mass_processor_list"), TEXT("mass_fragment_schema_get"),
			TEXT("mass_simulation_snapshot"), TEXT("mass_crowd_density_plan"), TEXT("mass_spawn_receipt_validate"), TEXT("mass_entity_debug_snapshot")
		}, TEXT("world_ai_smart_state_mass"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("asset_write_or_runtime_plan"), TEXT("receipt_gated"), TEXT("SmartObjects,StateTree,MassEntity,MassAI,ZoneGraph"), TEXT("SmartObjectsModule,StateTreeModule,MassEntity"), false, true);

		AddMany(Out, {
			TEXT("commonui_widget_classes_list"), TEXT("commonui_button_style_inspect"), TEXT("commonui_tab_list_configure"),
			TEXT("commonui_carousel_configure"), TEXT("commonui_video_player_configure"), TEXT("commonui_preview_receipt"),
			TEXT("commonui_input_action_bind"), TEXT("commonui_activatable_widget_create"), TEXT("commonui_style_asset_create"),
			TEXT("commonui_text_style_inspect"), TEXT("commonui_border_style_inspect"), TEXT("commonui_input_router_snapshot"),
			TEXT("commonui_menu_stack_plan"), TEXT("commonui_widget_compile_validate"), TEXT("commonui_platform_traits_audit"),
			TEXT("commonui_receipt_validate")
		}, TEXT("commonui_production"), TEXT("umg_author"), TEXT("5.7.0"), TEXT("asset_write_or_read"), TEXT("receipt_gated"), TEXT("CommonUI"), TEXT("CommonUI"), false, true);

		return Out;
	}

	static bool IsRegistered(const FSololmcpToolRegistry& Registry, const FString& Name)
	{
		TArray<FString> Names;
		Registry.GetRegisteredToolNamesSorted(Names);
		return Names.Contains(Name);
	}
}

void RegisterP1CompletionTools(FSololmcpToolRegistry& Registry)
{
	using namespace P1CompletionTools;
	for (const FP1ToolSpec& Spec : Specs())
	{
		if (IsRegistered(Registry, Spec.Name))
		{
			continue;
		}

		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = FString::Printf(TEXT("P1 %s wrapper for %s."), Spec.Family, Spec.AgentRole);
		Def.InputSchema = ToolInputSchema();
		Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return RunP1Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}
}
