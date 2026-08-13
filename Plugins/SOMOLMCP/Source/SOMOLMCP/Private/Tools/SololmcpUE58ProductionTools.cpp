// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpUE58ProductionTools.cpp
// ----------------------------------------------------------------------------
// UE 5.8-only production capability plans. These tools are intentionally
// reflection/filesystem/plugin probes plus structured execution plans; they do
// not link against UE 5.8 optional plugin headers so the same plugin still
// compiles and runs on UE 5.7.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"

namespace UE::SOMOLMCP
{
namespace UE58ProductionTools
{
	struct FUE58PlanSpec
	{
		FString ToolName;
		FString Description;
		FString Domain;
		TArray<FString> Plugins;
		TArray<FString> Modules;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
		TArray<FString> FallbackTools;
		bool bScanToolsets = false;
	};

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static bool IsUE58OrLater()
	{
		const FEngineVersion Current = FEngineVersion::Current();
		if (Current.GetMajor() != 5)
		{
			return Current.GetMajor() > 5;
		}
		return Current.GetMinor() >= 8;
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

	static TArray<FString> GetStringArrayField(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
		if (!Arguments->TryGetArrayField(FieldName, Raw) || !Raw)
		{
			return Values;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Raw)
		{
			if (Value.IsValid())
			{
				const FString StringValue = Value->AsString();
				if (!StringValue.IsEmpty())
				{
					Values.Add(StringValue);
				}
			}
		}
		return Values;
	}

	static TSharedRef<FJsonObject> PluginProbeJson(const FString& PluginName)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), PluginName);

		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		Obj->SetBoolField(TEXT("found"), Plugin.IsValid());
		if (!Plugin.IsValid())
		{
			Obj->SetBoolField(TEXT("enabled"), false);
			return Obj;
		}

		const FPluginDescriptor& Desc = Plugin->GetDescriptor();
		Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
		Obj->SetStringField(TEXT("friendly_name"), Desc.FriendlyName);
		Obj->SetStringField(TEXT("version_name"), Desc.VersionName);
		Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
		Obj->SetStringField(TEXT("descriptor_file"), Plugin->GetDescriptorFileName());
		return Obj;
	}

	static TSharedRef<FJsonObject> ModuleProbeJson(const FString& ModuleName)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		FString ModulePath;
		const bool bExists = ModuleExistsCompat(*ModuleName, &ModulePath);
		Obj->SetStringField(TEXT("name"), ModuleName);
		Obj->SetBoolField(TEXT("exists"), bExists);
		Obj->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(*ModuleName)));
		if (!ModulePath.IsEmpty())
		{
			Obj->SetStringField(TEXT("module_file"), ModulePath);
		}
		return Obj;
	}

	static void ScanFileForMarkerCount(const FString& FilePath, const FString& Marker, int32& InOutCount)
	{
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *FilePath))
		{
			return;
		}

		int32 SearchFrom = 0;
		while (true)
		{
			const int32 FoundAt = Contents.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (FoundAt == INDEX_NONE)
			{
				break;
			}
			++InOutCount;
			SearchFrom = FoundAt + Marker.Len();
		}
	}

	static TSharedRef<FJsonObject> ToolsetMarkerInventoryJson()
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		const FString ToolsetsDir = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("Experimental"), TEXT("Toolsets"));
		Obj->SetStringField(TEXT("toolsets_dir"), ToolsetsDir);
		Obj->SetBoolField(TEXT("toolsets_dir_exists"), IFileManager::Get().DirectoryExists(*ToolsetsDir));

		int32 PluginCount = 0;
		int32 HeaderFileCount = 0;
		int32 PythonFileCount = 0;
		int32 AICallableMarkerCount = 0;
		int32 ToolsetDefinitionMarkerCount = 0;
		int32 ExcludedToolsetCount = 0;
		TArray<TSharedPtr<FJsonValue>> ToolsetJson;
		TArray<TSharedPtr<FJsonValue>> ExcludedToolsetJson;

		if (IFileManager::Get().DirectoryExists(*ToolsetsDir))
		{
			TArray<FString> PluginFiles;
			IFileManager::Get().FindFilesRecursive(PluginFiles, *ToolsetsDir, TEXT("*.uplugin"), true, false);
			PluginFiles.Sort();
			for (const FString& PluginFile : PluginFiles)
			{
				const FString ToolsetName = FPaths::GetBaseFilename(PluginFile);
				if (ToolsetName.Equals(TEXT("MCPClientToolset"), ESearchCase::IgnoreCase))
				{
					++ExcludedToolsetCount;
					TSharedRef<FJsonObject> ExcludedObj = MakeShared<FJsonObject>();
					ExcludedObj->SetStringField(TEXT("name"), ToolsetName);
					ExcludedObj->SetStringField(TEXT("descriptor_file"), PluginFile);
					ExcludedObj->SetStringField(TEXT("reason"), TEXT("excluded_by_user_request"));
					ExcludedToolsetJson.Add(MakeShared<FJsonValueObject>(ExcludedObj));
					continue;
				}

				TSharedRef<FJsonObject> ToolsetObj = MakeShared<FJsonObject>();
				ToolsetObj->SetStringField(TEXT("name"), ToolsetName);
				ToolsetObj->SetStringField(TEXT("descriptor_file"), PluginFile);
				ToolsetJson.Add(MakeShared<FJsonValueObject>(ToolsetObj));
			}
			PluginCount = ToolsetJson.Num();

			TArray<FString> HeaderFiles;
			IFileManager::Get().FindFilesRecursive(HeaderFiles, *ToolsetsDir, TEXT("*.h"), true, false);
			for (const FString& HeaderFile : HeaderFiles)
			{
				if (HeaderFile.Contains(TEXT("\\MCPClientToolset\\")) || HeaderFile.Contains(TEXT("/MCPClientToolset/")))
				{
					continue;
				}
				++HeaderFileCount;
				ScanFileForMarkerCount(HeaderFile, TEXT("AICallable"), AICallableMarkerCount);
			}

			TArray<FString> PythonFiles;
			IFileManager::Get().FindFilesRecursive(PythonFiles, *ToolsetsDir, TEXT("*.py"), true, false);
			for (const FString& PythonFile : PythonFiles)
			{
				if (PythonFile.Contains(TEXT("\\MCPClientToolset\\")) || PythonFile.Contains(TEXT("/MCPClientToolset/")))
				{
					continue;
				}
				++PythonFileCount;
				ScanFileForMarkerCount(PythonFile, TEXT("ToolsetDefinition"), ToolsetDefinitionMarkerCount);
			}
		}

		Obj->SetNumberField(TEXT("toolset_plugin_count"), PluginCount);
		Obj->SetNumberField(TEXT("excluded_toolset_count"), ExcludedToolsetCount);
		Obj->SetNumberField(TEXT("header_file_count"), HeaderFileCount);
		Obj->SetNumberField(TEXT("python_file_count"), PythonFileCount);
		Obj->SetNumberField(TEXT("aicallable_marker_count"), AICallableMarkerCount);
		Obj->SetNumberField(TEXT("toolset_definition_marker_count"), ToolsetDefinitionMarkerCount);
		Obj->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		Obj->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		Obj->SetArrayField(TEXT("toolsets"), ToolsetJson);
		Obj->SetArrayField(TEXT("excluded_toolsets"), ExcludedToolsetJson);
		return Obj;
	}

	static TSharedRef<FJsonObject> PlanInputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Target asset, level, sequence, subject, session, or output path for the planned UE 5.8 operation."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Optional source or dependency asset paths."))},
			{TEXT("max_items"), FSololmcpSchemaBuilder::Integer(TEXT("Optional item/crowd/frame cap for planning and QA budget."))},
			{TEXT("quality_gate"), FSololmcpSchemaBuilder::String(TEXT("Expected QA gate, for example compile, preview, render, validate, or receipt."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan only. UE 5.8 production tools currently remain dry-run/read-only gates."))},
			{TEXT("notes"), FSololmcpSchemaBuilder::String(TEXT("Operator or agent notes to echo into the receipt envelope."))}
		});
	}

	static bool RunUE58Plan(
		const FUE58PlanSpec& Spec,
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& /*OutError*/)
	{
		const bool bVersionSatisfied = IsUE58OrLater();

		TArray<TSharedPtr<FJsonValue>> PluginJson;
		bool bAnyPluginFound = Spec.Plugins.IsEmpty();
		bool bAllPluginsEnabled = true;
		for (const FString& PluginName : Spec.Plugins)
		{
			TSharedRef<FJsonObject> Probe = PluginProbeJson(PluginName);
			bool bFound = false;
			bool bEnabled = false;
			Probe->TryGetBoolField(TEXT("found"), bFound);
			Probe->TryGetBoolField(TEXT("enabled"), bEnabled);
			bAnyPluginFound = bAnyPluginFound || bFound;
			bAllPluginsEnabled = bAllPluginsEnabled && bEnabled;
			PluginJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		TArray<TSharedPtr<FJsonValue>> ModuleJson;
		bool bAllModulesExist = true;
		for (const FString& ModuleName : Spec.Modules)
		{
			TSharedRef<FJsonObject> Probe = ModuleProbeJson(ModuleName);
			bool bExists = false;
			Probe->TryGetBoolField(TEXT("exists"), bExists);
			bAllModulesExist = bAllModulesExist && bExists;
			ModuleJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		FString Target;
		Arguments->TryGetStringField(TEXT("target"), Target);
		FString QualityGate;
		Arguments->TryGetStringField(TEXT("quality_gate"), QualityGate);
		FString Notes;
		Arguments->TryGetStringField(TEXT("notes"), Notes);
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		int32 MaxItems = 0;
		Arguments->TryGetNumberField(TEXT("max_items"), MaxItems);
		const TArray<FString> AssetPaths = GetStringArrayField(Arguments, TEXT("asset_paths"));

		FString Status;
		if (!bVersionSatisfied)
		{
			Status = TEXT("requires_ue_5_8");
		}
		else if (!bAnyPluginFound)
		{
			Status = TEXT("plugin_missing");
		}
		else if (!bAllModulesExist)
		{
			Status = TEXT("module_missing");
		}
		else if (!bAllPluginsEnabled)
		{
			Status = TEXT("plugin_present_not_enabled");
		}
		else
		{
			Status = TEXT("available_plan_ready");
		}

		const bool bAvailable = bVersionSatisfied && bAnyPluginFound && bAllModulesExist && bAllPluginsEnabled;

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("query"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("read_only"));
		OutStructured->SetStringField(TEXT("tool_name"), Spec.ToolName);
		OutStructured->SetStringField(TEXT("domain"), Spec.Domain);
		OutStructured->SetStringField(TEXT("status"), Status);
		OutStructured->SetBoolField(TEXT("available"), bAvailable);
		OutStructured->SetBoolField(TEXT("version_satisfied"), bVersionSatisfied);
		OutStructured->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		OutStructured->SetStringField(TEXT("minimum_engine_version"), TEXT("5.8.0"));
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetNumberField(TEXT("max_items"), MaxItems);
		OutStructured->SetStringField(TEXT("quality_gate"), QualityGate);
		OutStructured->SetStringField(TEXT("notes"), Notes);
		OutStructured->SetArrayField(TEXT("asset_paths"), StringArrayJson(AssetPaths));
		OutStructured->SetArrayField(TEXT("plugins"), PluginJson);
		OutStructured->SetArrayField(TEXT("modules"), ModuleJson);
		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		OutStructured->SetArrayField(TEXT("fallback_tools"), StringArrayJson(Spec.FallbackTools));

		if (Spec.bScanToolsets)
		{
			OutStructured->SetObjectField(TEXT("toolset_inventory"), ToolsetMarkerInventoryJson());
		}

		OutSummary = FString::Printf(TEXT("%s: %s on UE %s."), *Spec.ToolName, *Status, *CurrentEngineVersionString());
		return true;
	}

	static void RegisterPlanSpec(FSololmcpToolRegistry& Registry, const FUE58PlanSpec& Spec)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Spec.ToolName;
		Def.Description = Spec.Description;
		Def.InputSchema = PlanInputSchema();
		Def.CacheTtlSeconds = 15;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return RunUE58Plan(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	static TArray<FUE58PlanSpec> PlanSpecs()
	{
		return {
			{
				TEXT("ue58_official_mcp_import_plan"),
				TEXT("Build a fail-closed plan for using UE 5.8 official ModelContextProtocol surfaces without connecting to the official MCP server."),
				TEXT("ue58_official_mcp"),
				{TEXT("ModelContextProtocol")},
				{TEXT("ModelContextProtocol"), TEXT("ModelContextProtocolEditor")},
				{TEXT("Probe official MCP plugin and modules."), TEXT("Map official callable domains to SOMOLMCP tools."), TEXT("Keep SOMOLMCP transport/auth/queue as the execution authority."), TEXT("Record gaps as version-gated fallback routes.")},
				{TEXT("engine_version"), TEXT("official_mcp_plugin_probe"), TEXT("route_map"), TEXT("fallback_tools"), TEXT("no_official_connection_required")},
				{TEXT("ue58_official_mcp_capability_probe"), TEXT("mcp_dispatch_plan_preview"), TEXT("plugin_inspect")}
			},
			{
				TEXT("ue58_toolset_callable_binding_plan"),
				TEXT("Plan extraction and binding of UE 5.8 ToolsetRegistry AICallable surfaces into SOMOLMCP-safe tool wrappers, excluding MCPClientToolset."),
				TEXT("ue58_toolsets"),
				{TEXT("ToolsetRegistry"), TEXT("AllToolsets")},
				{TEXT("ToolsetRegistry")},
				{TEXT("Scan Toolsets descriptors and AICallable markers, excluding MCPClientToolset."), TEXT("Classify callables by read/write/editor-build lane."), TEXT("Generate wrapper candidates with UE 5.8 guards."), TEXT("Require dry-run and receipt envelope before mutating wrappers are exposed.")},
				{TEXT("toolset_inventory"), TEXT("mcp_client_toolset_policy"), TEXT("aicallable_marker_count"), TEXT("schema_candidates"), TEXT("version_guard"), TEXT("receipt_gate")},
				{TEXT("ue58_toolset_callable_inventory"), TEXT("tool_schema_repair_candidates"), TEXT("plugin_list_all")},
				true
			},
			{
				TEXT("ue58_toolset_schema_extract_plan"),
				TEXT("Plan schema extraction for UE 5.8 ToolsetDefinition Python/C++ files, excluding MCPClientToolset and keeping UE 5.7 builds isolated from 5.8-only classes."),
				TEXT("ue58_toolsets"),
				{TEXT("ToolsetRegistry"), TEXT("AllToolsets")},
				{TEXT("ToolsetRegistry")},
				{TEXT("Inventory ToolsetDefinition markers outside MCPClientToolset."), TEXT("Extract callable names, argument hints, and safety class."), TEXT("Emit client schema repair candidates."), TEXT("Mark generated tools as UE 5.8-only until live smoke passes.")},
				{TEXT("toolset_definition_marker_count"), TEXT("mcp_client_toolset_policy"), TEXT("source_files"), TEXT("generated_schema_candidates"), TEXT("ue58_only_flag")},
				{TEXT("ue58_toolset_callable_inventory"), TEXT("tool_schema_repair_candidates")},
				true
			},
			{
				TEXT("ue58_metahuman_crowd_deployment_plan"),
				TEXT("Plan UE 5.8 MetaHuman Crowd deployment with Mass/ZoneGraph/PCG handoff and crowd-size guardrails."),
				TEXT("metahuman"),
				{TEXT("MetaHumanCrowd"), TEXT("MassCrowd"), TEXT("ZoneGraph")},
				{TEXT("MetaHumanCrowd"), TEXT("MassCrowd"), TEXT("ZoneGraph")},
				{TEXT("Validate target level and crowd budget."), TEXT("Resolve MetaHuman crowd assets and Mass entity configs."), TEXT("Bind ZoneGraph lanes and spawn regions."), TEXT("Schedule preview screenshot and performance receipt.")},
				{TEXT("target_level"), TEXT("crowd_budget"), TEXT("mass_config"), TEXT("zonegraph_receipt"), TEXT("preview_screenshot")},
				{TEXT("mass_crowd_deployment_plan"), TEXT("zonegraph_lane_authoring_plan"), TEXT("pcg_generated_actor_health_audit")}
			},
			{
				TEXT("ue58_metahuman_character_assembly_plan"),
				TEXT("Plan UE 5.8 MetaHuman runtime character assembly from generated character specs and animation handoff data."),
				TEXT("metahuman"),
				{TEXT("MetaHumanRuntime"), TEXT("MetaHumanCharacter"), TEXT("MetaHumanCharacterUAF")},
				{TEXT("MetaHumanCharacter")},
				{TEXT("Resolve source character spec and mesh/material dependencies."), TEXT("Plan MetaHuman character asset assembly."), TEXT("Attach UAF or baseline AnimBP animation path."), TEXT("Require preview, skeleton compatibility, and save receipt.")},
				{TEXT("character_spec"), TEXT("asset_manifest"), TEXT("animation_binding"), TEXT("preview_receipt"), TEXT("save_receipt")},
				{TEXT("character_animation_pipeline"), TEXT("animation_asset_compat_diagnose"), TEXT("asset_dependency_graph")}
			},
			{
				TEXT("ue58_uaf_animation_graph_plan"),
				TEXT("Plan UE 5.8 UAF animation graph/chooser/pose-search workflow with UE 5.7 fallback routes."),
				TEXT("animation"),
				{TEXT("UAF"), TEXT("UAFChooser"), TEXT("UAFPoseSearch")},
				{TEXT("UAFChooser"), TEXT("UAFPoseSearch")},
				{TEXT("Probe UAF modules."), TEXT("Resolve Chooser and PoseSearch inputs."), TEXT("Plan animation graph wiring and transition gates."), TEXT("Fallback to baseline Chooser/PoseSearch/AnimBP if UAF is unavailable.")},
				{TEXT("anim_graph_target"), TEXT("chooser_table"), TEXT("pose_database"), TEXT("compile_diagnostics"), TEXT("fallback_route")},
				{TEXT("chooser_table_plan"), TEXT("pose_search_motion_matching_plan"), TEXT("animbp_locomotion_plan")}
			},
			{
				TEXT("ue58_mass_character_trajectory_plan"),
				TEXT("Plan UE 5.8 MassCharacterTrajectory usage for richer crowd navigation and animation trajectory receipts."),
				TEXT("world_ai"),
				{TEXT("MassGameplay"), TEXT("MassAI"), TEXT("MassCrowd")},
				{TEXT("MassCharacterTrajectory")},
				{TEXT("Probe MassCharacterTrajectory module."), TEXT("Resolve crowd agents, lanes, and steering constraints."), TEXT("Plan trajectory sampling and debug visualization."), TEXT("Gate delivery on ZoneGraph and preview evidence.")},
				{TEXT("trajectory_module"), TEXT("zonegraph_asset"), TEXT("agent_count"), TEXT("debug_preview"), TEXT("qa_receipt")},
				{TEXT("mass_crowd_deployment_plan"), TEXT("zonegraph_lane_authoring_plan"), TEXT("terrain_pathability_audit")}
			},
			{
				TEXT("ue58_niagara_insights_capture_plan"),
				TEXT("Plan UE 5.8 Niagara Insights profiling capture for generated VFX systems and QA receipts."),
				TEXT("vfx"),
				{TEXT("NiagaraInsights"), TEXT("Niagara")},
				{TEXT("NiagaraInsights"), TEXT("Niagara")},
				{TEXT("Resolve Niagara system and preview world."), TEXT("Plan profiling capture duration and output path."), TEXT("Collect compile, parameter, and perf warning receipts."), TEXT("Fallback to HLSL validation and screenshot preview if Insights is unavailable.")},
				{TEXT("niagara_system"), TEXT("capture_duration"), TEXT("profile_output"), TEXT("compile_diagnostics"), TEXT("preview_receipt")},
				{TEXT("niagara_hlsl_validate_custom_node"), TEXT("niagara_system_compile"), TEXT("editor_get_screenshot")}
			},
			{
				TEXT("ue58_pcg_niagara_interop_plan"),
				TEXT("Plan UE 5.8 PCGNiagaraInterop handoff from generated PCG points to Niagara systems."),
				TEXT("pcg_vfx"),
				{TEXT("PCGNiagaraInterop"), TEXT("PCG"), TEXT("Niagara")},
				{TEXT("PCGNiagaraInterop"), TEXT("PCG"), TEXT("Niagara")},
				{TEXT("Validate PCG graph and generated attributes."), TEXT("Map attributes to Niagara user parameters."), TEXT("Run dry-run/tile-cap before preview."), TEXT("Require generated actor health and VFX preview receipt.")},
				{TEXT("pcg_graph"), TEXT("niagara_system"), TEXT("attribute_map"), TEXT("dry_run_receipt"), TEXT("preview_receipt")},
				{TEXT("pcg_graph_validate"), TEXT("pcg_dry_run_calibration_receipt"), TEXT("niagara_hlsl_validate_custom_node")}
			},
			{
				TEXT("ue58_livelink_device_capture_plan"),
				TEXT("Plan UE 5.8 LiveLink device capture sessions for OBS, sound, KiPro, and generic recording devices."),
				TEXT("virtual_production"),
				{TEXT("LiveLinkGenericRecordingDevice"), TEXT("LiveLinkOBSDevice"), TEXT("LiveLinkSoundDevice"), TEXT("LiveLinkKiProDevice"), TEXT("LiveLink")},
				{TEXT("LiveLink")},
				{TEXT("Probe device plugins and subjects."), TEXT("Plan capture source bindings and Take Recorder handoff."), TEXT("Define freshness/timecode checks."), TEXT("Require recording asset and subject-health receipt.")},
				{TEXT("device_plugins"), TEXT("live_link_subjects"), TEXT("take_recorder_preset"), TEXT("timecode_check"), TEXT("capture_receipt")},
				{TEXT("live_link_subjects_probe"), TEXT("take_recorder_source_plan"), TEXT("take_recorder_preset_probe")}
			},
			{
				TEXT("ue58_motion_design_avalanche_scene_plan"),
				TEXT("Plan UE 5.8 Avalanche/Motion Design scene assembly for generated broadcast graphics and material deltas."),
				TEXT("motion_design"),
				{TEXT("Avalanche")},
				{TEXT("Avalanche"), TEXT("AvalancheMaterial")},
				{TEXT("Probe Avalanche and material modules."), TEXT("Resolve text, shape, media, and sequence targets."), TEXT("Plan material/style binding."), TEXT("Require MRQ or viewport proof render receipt.")},
				{TEXT("motion_design_scene"), TEXT("material_delta"), TEXT("sequence_binding"), TEXT("render_receipt")},
				{TEXT("motion_design_capability_probe"), TEXT("movie_render_queue_job_plan"), TEXT("umg_capability_audit")}
			},
			{
				TEXT("ue58_official_mcp_route_guard"),
				TEXT("Guard an agent-requested UE 5.8 official MCP/toolset route and return the SOMOLMCP fallback path for UE 5.7 installs."),
				TEXT("ue58_route_guard"),
				{TEXT("ModelContextProtocol"), TEXT("ToolsetRegistry")},
				{},
				{TEXT("Check engine version before route execution."), TEXT("Check official MCP/toolset plugin presence."), TEXT("Return fail-closed status on UE 5.7."), TEXT("Prefer SOMOLMCP equivalent tools when official route is unavailable.")},
				{TEXT("requested_route"), TEXT("engine_version"), TEXT("route_status"), TEXT("fallback_tools"), TEXT("blocked_reason")},
				{TEXT("ue58_official_mcp_capability_probe"), TEXT("ue58_toolset_registry_probe"), TEXT("mcp_dispatch_plan_preview")}
			}
		};
	}
}

void RegisterUE58ProductionTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58ProductionTools::FUE58PlanSpec& Spec : UE58ProductionTools::PlanSpecs())
	{
		UE58ProductionTools::RegisterPlanSpec(Registry, Spec);
	}
}
}
