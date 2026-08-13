// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpP0CompletionTools.cpp
// ----------------------------------------------------------------------------
// P0 completion registry for planned production wrappers.
//
// This file closes the P0 tool-name surface without overriding existing concrete
// implementations. The wrappers are fail-closed by default: read/probe tools
// return structured gate data, while mutating tools require explicit execute=true
// plus later family-specific promotion before they can perform editor writes.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"

namespace UE::SOMOLMCP
{
namespace P0CompletionTools
{
	struct FP0ToolSpec
	{
		const TCHAR* Name;
		const TCHAR* Family;
		const TCHAR* AgentRole;
		const TCHAR* Version;
		const TCHAR* OperationClass;
		const TCHAR* SafetyClass;
		const TCHAR* RequiredPlugins;
		const TCHAR* RequiredModules;
		const TCHAR* Description;
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

	static TSharedRef<FJsonObject> PluginGateJson(const FP0ToolSpec& Spec)
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
		Obj->SetArrayField(TEXT("plugin_status"), PluginStatus);
		Obj->SetArrayField(TEXT("module_status"), ModuleStatus);
		return Obj;
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

	static TSharedRef<FJsonObject> ReceiptRequirementsJson(const FP0ToolSpec& Spec)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetArrayField(TEXT("required_for_all"), StringArrayJson({
			TEXT("target project / asset / object binding when applicable"),
			TEXT("version and plugin/module gate"),
			TEXT("preflight schema or capability snapshot"),
			TEXT("post-call readback or explicit not-executed state"),
			TEXT("structured failure route")
		}));
		if (Spec.bMutation)
		{
			Obj->SetArrayField(TEXT("required_for_execute"), StringArrayJson({
				TEXT("dry-run pass"),
				TEXT("resource lock"),
				TEXT("scoped transaction or rollback note"),
				TEXT("post-edit readback"),
				TEXT("compile / validate / QA receipt where relevant"),
				TEXT("save policy or rollback state")
			}));
		}
		else
		{
			Obj->SetArrayField(TEXT("required_for_execute"), StringArrayJson({
				TEXT("read-only response proof"),
				TEXT("staleness/cache note if data is cached")
			}));
		}
		return Obj;
	}

	static TSharedRef<FJsonObject> SpecJson(const FP0ToolSpec& Spec)
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
		Obj->SetStringField(TEXT("description"), Spec.Description);
		Obj->SetObjectField(TEXT("plugin_gate"), PluginGateJson(Spec));
		Obj->SetObjectField(TEXT("receipt_requirements"), ReceiptRequirementsJson(Spec));
		return Obj;
	}

	static bool RunP0Tool(
		const FP0ToolSpec& Spec,
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
		OutStructured->SetStringField(TEXT("status"), TEXT("planned_wrapper_ready"));
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		OutStructured->SetBoolField(TEXT("current_editor_ue58_or_later"), IsUE58OrLater());
		OutStructured->SetStringField(TEXT("current_engine_version"), CurrentEngineVersionString());
		OutStructured->SetObjectField(TEXT("spec"), SpecJson(Spec));

		TArray<FString> NextSteps = {
			TEXT("Use this wrapper name in agent plans and queue receipts."),
			TEXT("Check plugin/module gate before execution."),
			TEXT("Attach target binding and receipt evidence.")
		};
		if (Spec.bMutation)
		{
			NextSteps.Add(TEXT("Run dry-run first; concrete mutation remains receipt-gated."));
		}
		OutStructured->SetArrayField(TEXT("next_steps"), StringArrayJson(NextSteps));

		if (Spec.bUE58Only && !IsUE58OrLater())
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("requires_ue_5_8"));
			OutStructured->SetBoolField(TEXT("success"), true);
			OutSummary = FString::Printf(TEXT("%s requires UE 5.8; current engine is %s."), Spec.Name, *CurrentEngineVersionString());
			return true;
		}

		if (bExecute && Spec.bMutation)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_concrete_executor"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("promote_family_specific_executor_after_live_fixture"));
			OutError = FString::Printf(TEXT("%s is registered as a P0 receipt-gated wrapper; concrete mutation executor must be promoted with live fixture proof before execute=true."), Spec.Name);
			OutSummary = OutError;
			return false;
		}

		OutSummary = FString::Printf(TEXT("%s P0 wrapper returned %s."), Spec.Name, bDryRun ? TEXT("dry-run execution contract") : TEXT("read/probe execution contract"));
		return true;
	}

	static TArray<FP0ToolSpec> Specs()
	{
		return {
			// GeometryScript first wave - UE 5.7+ plugin/module gated.
			{TEXT("geometry_dynamic_mesh_create"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Create a transient/durable dynamic mesh asset contract."), false, true},
			{TEXT("geometry_dynamic_mesh_load_static_mesh"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Load static mesh geometry into a dynamic mesh work item."), false, false},
			{TEXT("geometry_dynamic_mesh_save_static_mesh"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Save dynamic mesh output into a static mesh asset."), false, true},
			{TEXT("geometry_dynamic_mesh_inspect"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Inspect dynamic mesh bounds, triangles, and attributes."), false, false},
			{TEXT("geometry_primitive_box"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Generate a box primitive into dynamic mesh pipeline."), false, true},
			{TEXT("geometry_primitive_sphere"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Generate a sphere primitive into dynamic mesh pipeline."), false, true},
			{TEXT("geometry_primitive_cylinder"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Generate a cylinder primitive into dynamic mesh pipeline."), false, true},
			{TEXT("geometry_primitive_plane"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Generate a plane primitive into dynamic mesh pipeline."), false, true},
			{TEXT("geometry_boolean_v2"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Plan/apply boolean mesh operation with rollback receipt."), false, true},
			{TEXT("geometry_remesh_v2"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Plan/apply remesh operation with quality limits."), false, true},
			{TEXT("geometry_simplify_v2"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Plan/apply mesh simplification with target triangle count."), false, true},
			{TEXT("geometry_normals_recompute"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Recompute mesh normals/tangents contract."), false, true},
			{TEXT("geometry_uv_generate"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Generate UVs for dynamic/static mesh asset."), false, true},
			{TEXT("geometry_collision_generate"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Generate mesh collision contract."), false, true},
			{TEXT("geometry_vertex_color_bake"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Bake vertex colors into mesh."), false, true},
			{TEXT("geometry_mesh_repair_v2"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Repair mesh topology defects."), false, true},
			{TEXT("geometry_bounds_readback"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Read back mesh bounds after operation."), false, false},
			{TEXT("geometry_material_slots_sync"), TEXT("geometryscript"), TEXT("mesh_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Sync material slots after mesh operation."), false, true},
			{TEXT("geometry_asset_roundtrip_receipt"), TEXT("geometryscript"), TEXT("qa_inspector"), TEXT("5.7.0"), TEXT("read"), TEXT("receipt_validate"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Validate dynamic/static mesh roundtrip receipt."), false, false},
			{TEXT("geometry_operation_rollback_plan"), TEXT("geometryscript"), TEXT("qa_inspector"), TEXT("5.7.0"), TEXT("plan"), TEXT("rollback_plan"), TEXT("GeometryScripting"), TEXT("GeometryScriptingCore"), TEXT("Build rollback plan for geometry operation."), false, false},

			// Sequencer and MRQ - UE 5.7+.
			{TEXT("sequencer_binding_add_v2"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Add a possessable/spawnable binding to a LevelSequence."), false, true},
			{TEXT("sequencer_track_add_v2"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Add a track to a sequence binding or master sequence."), false, true},
			{TEXT("sequencer_section_add_v2"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Add a section to a Sequencer track."), false, true},
			{TEXT("sequencer_channel_keys_list"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("List keys on a Sequencer channel."), false, false},
			{TEXT("sequencer_channel_key_add"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Add a key to a Sequencer channel."), false, true},
			{TEXT("sequencer_channel_key_remove"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Remove a key from a Sequencer channel."), false, true},
			{TEXT("sequencer_marker_add"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Add a marker to a LevelSequence."), false, true},
			{TEXT("sequencer_folder_add"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Add an organizational folder to Sequencer."), false, true},
			{TEXT("sequencer_camera_cut_add_v2"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Add a camera cut section with readback."), false, true},
			{TEXT("sequencer_playback_range_set_v2"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Set playback range on LevelSequence."), false, true},
			{TEXT("sequencer_export_snapshot"), TEXT("sequencer"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("SequencerScripting"), TEXT("SequencerScripting"), TEXT("Export structured Sequencer snapshot."), false, false},
			{TEXT("mrq_queue_list"), TEXT("mrq"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("MovieRenderPipeline"), TEXT("MovieRenderPipelineCore"), TEXT("List Movie Render Queue jobs."), false, false},
			{TEXT("mrq_job_create"), TEXT("mrq"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("MovieRenderPipeline"), TEXT("MovieRenderPipelineCore"), TEXT("Create MRQ job contract."), false, true},
			{TEXT("mrq_job_configure"), TEXT("mrq"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("MovieRenderPipeline"), TEXT("MovieRenderPipelineCore"), TEXT("Configure MRQ job settings."), false, true},
			{TEXT("mrq_render_status"), TEXT("mrq"), TEXT("camera_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("MovieRenderPipeline"), TEXT("MovieRenderPipelineCore"), TEXT("Read MRQ render status."), false, false},
			{TEXT("mrq_output_validate"), TEXT("mrq"), TEXT("qa_inspector"), TEXT("5.7.0"), TEXT("read"), TEXT("receipt_validate"), TEXT("MovieRenderPipeline"), TEXT("MovieRenderPipelineCore"), TEXT("Validate MRQ output receipt."), false, false},

			// PCG baseline interops.
			{TEXT("pcg_alembic_export_to_asset"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PCG,PCGExternalDataInterop"), TEXT("PCG"), TEXT("Export Alembic/external data into PCG asset contract."), false, true},
			{TEXT("pcg_dynamic_mesh_node_add"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PCG,PCGGeometryScriptInterop"), TEXT("PCG"), TEXT("Add PCG DynamicMesh interop node."), false, true},
			{TEXT("pcg_python_node_add_safe"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("sandboxed_receipt"), TEXT("PCG,PCGPythonInterop"), TEXT("PCG"), TEXT("Add sandboxed PCG Python node."), false, true},
			{TEXT("pcg_water_spline_node_add"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PCG,Water"), TEXT("PCG"), TEXT("Add water spline PCG interop node."), false, true},
			{TEXT("pcg_niagara_interop_node_add"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PCG,Niagara,PCGNiagaraInterop"), TEXT("PCG,Niagara"), TEXT("Add PCG/Niagara interop node."), false, true},
			{TEXT("pcg_nanite_assembly_builder_node_add"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PCG,NaniteDisplacedMesh"), TEXT("PCG"), TEXT("Add Nanite assembly builder node."), false, true},
			{TEXT("pcg_instanced_actors_resource_audit"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("PCG,InstancedActors"), TEXT("PCG"), TEXT("Audit PCG instanced actors resources."), false, false},
			{TEXT("pcg_interop_plugin_gate_report"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("PCG"), TEXT("PCG"), TEXT("Report PCG interop plugin gates."), false, false},
			{TEXT("pcg_interop_dry_run_receipt"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("plan"), TEXT("dry_run"), TEXT("PCG"), TEXT("PCG"), TEXT("Build PCG interop dry-run receipt."), false, false},
			{TEXT("pcg_interop_readback_snapshot"), TEXT("pcg_interop"), TEXT("pcg_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("PCG"), TEXT("PCG"), TEXT("Read back PCG interop graph state."), false, false},

			// ControlRig core.
			{TEXT("control_rig_hierarchy_inspect_v2"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Inspect ControlRig hierarchy."), false, false},
			{TEXT("control_rig_bone_add_v2"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Add ControlRig bone."), false, true},
			{TEXT("control_rig_control_add_v2"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Add ControlRig control."), false, true},
			{TEXT("control_rig_null_add_v2"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Add ControlRig null."), false, true},
			{TEXT("control_rig_controller_link_add"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Add ControlRig controller link."), false, true},
			{TEXT("control_rig_selection_set_create"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Create ControlRig selection set."), false, true},
			{TEXT("control_rig_sequencer_key_add"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("ControlRig,SequencerScripting"), TEXT("ControlRig,SequencerScripting"), TEXT("Add Sequencer key for ControlRig channel."), false, true},
			{TEXT("control_rig_pose_bake"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Bake ControlRig pose."), false, true},
			{TEXT("control_rig_compile_validate"), TEXT("control_rig"), TEXT("qa_inspector"), TEXT("5.7.0"), TEXT("read"), TEXT("receipt_validate"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Compile/validate ControlRig asset."), false, false},
			{TEXT("control_rig_graph_snapshot"), TEXT("control_rig"), TEXT("animation_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Snapshot ControlRig graph."), false, false},
			{TEXT("control_rig_rollback_plan"), TEXT("control_rig"), TEXT("qa_inspector"), TEXT("5.7.0"), TEXT("plan"), TEXT("rollback_plan"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Build ControlRig rollback plan."), false, false},
			{TEXT("control_rig_receipt_validate"), TEXT("control_rig"), TEXT("qa_inspector"), TEXT("5.7.0"), TEXT("read"), TEXT("receipt_validate"), TEXT("ControlRig"), TEXT("ControlRig"), TEXT("Validate ControlRig receipt."), false, false},

			// UE 5.8 Toolsets: automation, dataflow, UMG, Slate, PhysicsAsset.
			{TEXT("automation_discover_tests"), TEXT("automation_toolset"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("AutomationTestToolset"), TEXT("AutomationController"), TEXT("Discover automation tests."), true, false},
			{TEXT("automation_list_tests"), TEXT("automation_toolset"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("AutomationTestToolset"), TEXT("AutomationController"), TEXT("List automation tests."), true, false},
			{TEXT("automation_run_tests"), TEXT("automation_toolset"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("editor_test"), TEXT("receipt_gated"), TEXT("AutomationTestToolset"), TEXT("AutomationController"), TEXT("Run selected automation tests."), true, true},
			{TEXT("automation_get_results"), TEXT("automation_toolset"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("AutomationTestToolset"), TEXT("AutomationController"), TEXT("Get automation test results."), true, false},
			{TEXT("automation_get_status"), TEXT("automation_toolset"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("AutomationTestToolset"), TEXT("AutomationController"), TEXT("Get automation run status."), true, false},
			{TEXT("automation_stop_tests"), TEXT("automation_toolset"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("runtime_state"), TEXT("guarded_action"), TEXT("AutomationTestToolset"), TEXT("AutomationController"), TEXT("Stop automation test run."), true, true},

			{TEXT("dataflow_graph_create"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Create dataflow graph asset."), true, true},
			{TEXT("dataflow_graph_structure"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Read dataflow graph structure."), true, false},
			{TEXT("dataflow_node_types_list"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("List dataflow node types."), true, false},
			{TEXT("dataflow_node_schema"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Read dataflow node schema."), true, false},
			{TEXT("dataflow_node_add"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Add dataflow node."), true, true},
			{TEXT("dataflow_node_update"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Update dataflow node."), true, true},
			{TEXT("dataflow_node_info"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Read dataflow node info."), true, false},
			{TEXT("dataflow_node_reposition"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Reposition dataflow node."), true, true},
			{TEXT("dataflow_node_remove"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Remove dataflow node."), true, true},
			{TEXT("dataflow_pins_connect"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Connect dataflow pins."), true, true},
			{TEXT("dataflow_pins_disconnect"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Disconnect dataflow pins."), true, true},
			{TEXT("dataflow_variables_list"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("List dataflow variables."), true, false},
			{TEXT("dataflow_variable_add"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Add dataflow variable."), true, true},
			{TEXT("dataflow_variable_remove"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Remove dataflow variable."), true, true},
			{TEXT("dataflow_variable_set"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Set dataflow variable."), true, true},
			{TEXT("dataflow_comment_add"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Add dataflow comment."), true, true},
			{TEXT("dataflow_comment_remove"), TEXT("dataflow_toolset"), TEXT("mesh_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("Dataflow"), TEXT("DataflowEngine"), TEXT("Remove dataflow comment."), true, true},

			{TEXT("umg_widget_blueprint_create_v2"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Create widget blueprint through UMG toolset route."), true, true},
			{TEXT("umg_widget_add_v2"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Add widget to widget tree."), true, true},
			{TEXT("umg_named_slot_set_content"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Set named slot content."), true, true},
			{TEXT("umg_widgets_list_v2"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("List widgets in blueprint tree."), true, false},
			{TEXT("umg_named_slots_list"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("List named slots."), true, false},
			{TEXT("umg_widget_classes_list"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("List creatable widget classes."), true, false},
			{TEXT("umg_widget_move_v2"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Move widget in tree."), true, true},
			{TEXT("umg_widget_remove_v2"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Remove widget from tree."), true, true},
			{TEXT("umg_widget_rename_v2"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Rename widget."), true, true},
			{TEXT("umg_widget_set_as_variable"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Set widget variable flag."), true, true},
			{TEXT("umg_widget_blueprint_reparent"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Reparent widget blueprint."), true, true},
			{TEXT("umg_widget_blueprint_compile_v2"), TEXT("umg_toolset"), TEXT("umg_author"), TEXT("5.8.0"), TEXT("editor_build"), TEXT("receipt_gated"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Compile widget blueprint with diagnostics."), true, true},
			{TEXT("umg_toolset_receipt_validate"), TEXT("umg_toolset"), TEXT("qa_inspector"), TEXT("5.8.0"), TEXT("read"), TEXT("receipt_validate"), TEXT("UMGToolSet,UMG"), TEXT("UMG,UMGEditor"), TEXT("Validate UMG toolset receipt."), true, false},

			{TEXT("slate_snapshot"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Snapshot Slate widget tree."), true, false},
			{TEXT("slate_windows"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("List Slate windows."), true, false},
			{TEXT("slate_screenshot"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("capture"), TEXT("guarded_capture"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Capture Slate screenshot."), true, false},
			{TEXT("slate_wait_for"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("read"), TEXT("watch"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Wait for Slate selector."), true, false},
			{TEXT("slate_click_safe"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("ui_action"), TEXT("modal_safe"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Safely click Slate widget."), true, true},
			{TEXT("slate_hover"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("ui_action"), TEXT("modal_safe"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Hover Slate widget."), true, true},
			{TEXT("slate_type_safe"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("ui_action"), TEXT("modal_safe"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Safely type text into Slate widget."), true, true},
			{TEXT("slate_press_key_safe"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("ui_action"), TEXT("modal_safe"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Safely press key in Slate context."), true, true},
			{TEXT("slate_select_option"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("ui_action"), TEXT("modal_safe"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Select Slate option."), true, true},
			{TEXT("slate_drag_safe"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("ui_action"), TEXT("modal_safe"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Safely drag Slate widget."), true, true},
			{TEXT("slate_observe"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("read"), TEXT("watch"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Start Slate observer."), true, false},
			{TEXT("slate_unobserve"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("read"), TEXT("watch"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Stop Slate observer."), true, false},
			{TEXT("slate_observers_list"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("List Slate observers."), true, false},
			{TEXT("slate_fill_form_safe"), TEXT("slate_inspector"), TEXT("editor_pilot"), TEXT("5.8.0"), TEXT("ui_action"), TEXT("modal_safe"), TEXT("SlateInspectorToolset"), TEXT("Slate,SlateCore"), TEXT("Safely fill Slate form."), true, true},

			{TEXT("physics_asset_create_from_mesh"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Create physics asset from mesh."), true, true},
			{TEXT("physics_asset_bodies_list"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("List physics asset bodies."), true, false},
			{TEXT("physics_asset_body_shapes"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("List body shapes."), true, false},
			{TEXT("physics_asset_set_sphere"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Set sphere shape."), true, true},
			{TEXT("physics_asset_set_capsule"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Set capsule shape."), true, true},
			{TEXT("physics_asset_set_box"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Set box shape."), true, true},
			{TEXT("physics_asset_shape_remove"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Remove body shape."), true, true},
			{TEXT("physics_asset_body_add"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Add physics body."), true, true},
			{TEXT("physics_asset_body_remove"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Remove physics body."), true, true},
			{TEXT("physics_asset_body_mode_get"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Get body mode."), true, false},
			{TEXT("physics_asset_body_mode_set"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Set body mode."), true, true},
			{TEXT("physics_asset_mass_scale_get"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Get body mass scale."), true, false},
			{TEXT("physics_asset_mass_scale_set"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Set body mass scale."), true, true},
			{TEXT("physics_asset_constraints_list"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("read"), TEXT("readonly"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("List physics constraints."), true, false},
			{TEXT("physics_asset_constraint_add"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Add physics constraint."), true, true},
			{TEXT("physics_asset_constraint_limits_set"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Set physics constraint limits."), true, true},
			{TEXT("physics_asset_constraint_remove"), TEXT("physics_asset_toolset"), TEXT("animation_author"), TEXT("5.8.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("PhysicsAssetToolset"), TEXT("PhysicsAssetEditor"), TEXT("Remove physics constraint."), true, true},

			// Gameplay production basics.
			{TEXT("gameplay_tags_list"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayTags"), TEXT("GameplayTags"), TEXT("List gameplay tags."), false, false},
			{TEXT("gameplay_tag_info"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayTags"), TEXT("GameplayTags"), TEXT("Read gameplay tag info."), false, false},
			{TEXT("gameplay_tag_add"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("config_write"), TEXT("receipt_gated"), TEXT("GameplayTags"), TEXT("GameplayTags"), TEXT("Add gameplay tag to project config."), false, true},
			{TEXT("gameplay_tag_remove"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("config_write"), TEXT("receipt_gated"), TEXT("GameplayTags"), TEXT("GameplayTags"), TEXT("Remove gameplay tag from project config."), false, true},
			{TEXT("gameplay_tag_rename"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("config_write"), TEXT("receipt_gated"), TEXT("GameplayTags"), TEXT("GameplayTags"), TEXT("Rename gameplay tag."), false, true},
			{TEXT("gameplay_tag_referencers"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayTags"), TEXT("GameplayTags"), TEXT("Find gameplay tag referencers."), false, false},
			{TEXT("game_feature_list"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameFeatures"), TEXT("GameFeatures"), TEXT("List game feature plugins."), false, false},
			{TEXT("game_feature_find_data"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameFeatures"), TEXT("GameFeatures"), TEXT("Find game feature data assets."), false, false},
			{TEXT("game_feature_actions_get"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameFeatures"), TEXT("GameFeatures"), TEXT("Read game feature actions."), false, false},
			{TEXT("game_feature_plugin_create"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GameFeatures"), TEXT("GameFeatures"), TEXT("Create game feature plugin contract."), false, true},
			{TEXT("gas_attribute_sets_find"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayAbilities"), TEXT("GameplayAbilities"), TEXT("Find GAS attribute sets."), false, false},
			{TEXT("gas_attributes_list"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayAbilities"), TEXT("GameplayAbilities"), TEXT("List GAS attributes."), false, false},
			{TEXT("gas_attribute_info"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayAbilities"), TEXT("GameplayAbilities"), TEXT("Read GAS attribute info."), false, false},
			{TEXT("gas_gameplay_effects_list"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayAbilities"), TEXT("GameplayAbilities"), TEXT("List gameplay effects."), false, false},
			{TEXT("gas_gameplay_effect_modifier_add_v2"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GameplayAbilities"), TEXT("GameplayAbilities"), TEXT("Add modifier to GameplayEffect."), false, true},
			{TEXT("gameplay_cue_list"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayAbilities,GameplayCueEditor"), TEXT("GameplayAbilities"), TEXT("List gameplay cues."), false, false},
			{TEXT("gameplay_cue_create_notify"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("asset_write"), TEXT("receipt_gated"), TEXT("GameplayAbilities,GameplayCueEditor"), TEXT("GameplayAbilities"), TEXT("Create gameplay cue notify asset."), false, true},
			{TEXT("gameplay_cue_tags_without_notifies"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("GameplayAbilities,GameplayCueEditor"), TEXT("GameplayAbilities"), TEXT("Find gameplay cue tags missing notifies."), false, false},
			{TEXT("world_condition_schema_list"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("WorldConditions"), TEXT("WorldConditions"), TEXT("List WorldCondition schemas."), false, false},
			{TEXT("world_condition_asset_inspect"), TEXT("gameplay"), TEXT("gameplay_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("WorldConditions"), TEXT("WorldConditions"), TEXT("Inspect WorldCondition asset."), false, false},
			{TEXT("livecoding_status_get"), TEXT("devops"), TEXT("devops_author"), TEXT("5.7.0"), TEXT("read"), TEXT("readonly"), TEXT("LiveCoding"), TEXT("LiveCoding"), TEXT("Get Live Coding status."), false, false},
			{TEXT("livecoding_compile_request"), TEXT("devops"), TEXT("devops_author"), TEXT("5.7.0"), TEXT("editor_build"), TEXT("guarded_action"), TEXT("LiveCoding"), TEXT("LiveCoding"), TEXT("Request Live Coding compile."), false, true}
		};
	}

	static bool IsRegistered(const FSololmcpToolRegistry& Registry, const FString& Name)
	{
		TArray<FString> Names;
		Registry.GetRegisteredToolNamesSorted(Names);
		return Names.Contains(Name);
	}
}

void RegisterP0CompletionTools(FSololmcpToolRegistry& Registry)
{
	using namespace P0CompletionTools;
	for (const FP0ToolSpec& Spec : Specs())
	{
		if (IsRegistered(Registry, Spec.Name))
		{
			continue;
		}

		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = FString::Printf(TEXT("P0 %s wrapper: %s"), Spec.Family, Spec.Description);
		Def.InputSchema = ToolInputSchema();
		Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return RunP0Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}
}
