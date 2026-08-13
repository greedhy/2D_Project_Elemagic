// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpVersionedCapabilityTools.cpp
// ----------------------------------------------------------------------------
// Read-only capability probes for UE 5.7 baseline expansion and UE 5.8-only
// official MCP / ToolsetRegistry surfaces.
//
// These tools intentionally do not link against optional engine plugin modules.
// They only inspect discovered plugins, module existence, and filesystem markers,
// so the same SOMOLMCP build can run on UE 5.7 while exposing fail-closed facts
// for UE 5.8-specific APIs.
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
#include "ProjectDescriptor.h"

namespace UE::SOMOLMCP
{
namespace VersionedCapabilityTools
{
	struct FCapabilitySpec
	{
		FString ToolName;
		FString Description;
		FString Domain;
		int32 MinMajor = 5;
		int32 MinMinor = 7;
		int32 MinPatch = 0;
		bool bSafeForUE57 = true;
		TArray<FString> PluginNames;
		TArray<FString> ModuleNames;
		TArray<FString> RecommendedNextTools;
		TArray<FString> CapabilityNotes;
		bool bScanToolsets = false;
	};

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static bool IsCurrentEngineAtLeast(const int32 Major, const int32 Minor, const int32 Patch)
	{
		const FEngineVersion Current = FEngineVersion::Current();
		if (Current.GetMajor() != Major)
		{
			return Current.GetMajor() > Major;
		}
		if (Current.GetMinor() != Minor)
		{
			return Current.GetMinor() > Minor;
		}
		return Current.GetPatch() >= Patch;
	}

	static FString MinimumVersionString(const FCapabilitySpec& Spec)
	{
		return FString::Printf(TEXT("%d.%d.%d"), Spec.MinMajor, Spec.MinMinor, Spec.MinPatch);
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

	static TSharedRef<FJsonObject> PluginProbeJson(const FString& PluginName)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), PluginName);

		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		Obj->SetBoolField(TEXT("found"), Plugin.IsValid());
		if (!Plugin.IsValid())
		{
			Obj->SetBoolField(TEXT("enabled"), false);
			Obj->SetArrayField(TEXT("descriptor_modules"), TArray<TSharedPtr<FJsonValue>>());
			return Obj;
		}

		const FPluginDescriptor& Desc = Plugin->GetDescriptor();
		Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
		Obj->SetStringField(TEXT("friendly_name"), Desc.FriendlyName);
		Obj->SetStringField(TEXT("version_name"), Desc.VersionName);
		Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
		Obj->SetStringField(TEXT("descriptor_file"), Plugin->GetDescriptorFileName());
		Obj->SetBoolField(TEXT("can_contain_content"), Desc.bCanContainContent);
		Obj->SetBoolField(TEXT("is_beta"), Desc.bIsBetaVersion);
		Obj->SetBoolField(TEXT("is_experimental"), Desc.bIsExperimentalVersion);

		TArray<TSharedPtr<FJsonValue>> ModulesJson;
		for (const FModuleDescriptor& Module : Desc.Modules)
		{
			TSharedRef<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
			const FString ModuleName = Module.Name.ToString();
			ModuleObj->SetStringField(TEXT("name"), ModuleName);
			ModuleObj->SetBoolField(TEXT("module_exists"), FModuleManager::Get().ModuleExists(*ModuleName));
			ModuleObj->SetBoolField(TEXT("module_loaded"), FModuleManager::Get().IsModuleLoaded(FName(*ModuleName)));
			ModulesJson.Add(MakeShared<FJsonValueObject>(ModuleObj));
		}
		Obj->SetArrayField(TEXT("descriptor_modules"), ModulesJson);
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

	static bool ScanFileForMarkerCount(const FString& FilePath, const FString& Marker, int32& InOutCount)
	{
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *FilePath))
		{
			return false;
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
		return true;
	}

	static TSharedRef<FJsonObject> ToolsetInventoryJson()
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		const FString ToolsetsDir = FPaths::Combine(FPaths::EnginePluginsDir(), TEXT("Experimental"), TEXT("Toolsets"));
		Obj->SetStringField(TEXT("toolsets_dir"), ToolsetsDir);
		Obj->SetBoolField(TEXT("toolsets_dir_exists"), IFileManager::Get().DirectoryExists(*ToolsetsDir));

		TArray<TSharedPtr<FJsonValue>> ToolsetJson;
		int32 PluginCount = 0;
		int32 HeaderFileCount = 0;
		int32 PythonFileCount = 0;
		int32 AICallableMarkerCount = 0;
		int32 ToolsetDefinitionMarkerCount = 0;

		if (IFileManager::Get().DirectoryExists(*ToolsetsDir))
		{
			TArray<FString> PluginFiles;
			IFileManager::Get().FindFilesRecursive(PluginFiles, *ToolsetsDir, TEXT("*.uplugin"), true, false);
			PluginFiles.Sort();
			PluginCount = PluginFiles.Num();

			for (const FString& PluginFile : PluginFiles)
			{
				TSharedRef<FJsonObject> ToolsetObj = MakeShared<FJsonObject>();
				ToolsetObj->SetStringField(TEXT("descriptor_file"), PluginFile);
				ToolsetObj->SetStringField(TEXT("name"), FPaths::GetBaseFilename(PluginFile));
				ToolsetObj->SetStringField(TEXT("relative_path"), PluginFile.Replace(*FPaths::EnginePluginsDir(), TEXT("")));
				ToolsetJson.Add(MakeShared<FJsonValueObject>(ToolsetObj));
			}

			TArray<FString> HeaderFiles;
			IFileManager::Get().FindFilesRecursive(HeaderFiles, *ToolsetsDir, TEXT("*.h"), true, false);
			HeaderFileCount = HeaderFiles.Num();
			for (const FString& HeaderFile : HeaderFiles)
			{
				ScanFileForMarkerCount(HeaderFile, TEXT("AICallable"), AICallableMarkerCount);
			}

			TArray<FString> PythonFiles;
			IFileManager::Get().FindFilesRecursive(PythonFiles, *ToolsetsDir, TEXT("*.py"), true, false);
			PythonFileCount = PythonFiles.Num();
			for (const FString& PythonFile : PythonFiles)
			{
				ScanFileForMarkerCount(PythonFile, TEXT("ToolsetDefinition"), ToolsetDefinitionMarkerCount);
			}
		}

		Obj->SetNumberField(TEXT("toolset_plugin_count"), PluginCount);
		Obj->SetNumberField(TEXT("header_file_count"), HeaderFileCount);
		Obj->SetNumberField(TEXT("python_file_count"), PythonFileCount);
		Obj->SetNumberField(TEXT("aicallable_marker_count"), AICallableMarkerCount);
		Obj->SetNumberField(TEXT("toolset_definition_marker_count"), ToolsetDefinitionMarkerCount);
		Obj->SetArrayField(TEXT("toolsets"), ToolsetJson);
		return Obj;
	}

	static bool RunCapabilityProbe(
		const FCapabilitySpec& Spec,
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& /*Arguments*/,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& /*OutError*/)
	{
		const bool bVersionSatisfied = IsCurrentEngineAtLeast(Spec.MinMajor, Spec.MinMinor, Spec.MinPatch);

		TArray<TSharedPtr<FJsonValue>> PluginJson;
		bool bAnyPluginFound = Spec.PluginNames.IsEmpty();
		bool bAllNamedPluginsFound = true;
		bool bAllNamedPluginsEnabled = true;
		for (const FString& PluginName : Spec.PluginNames)
		{
			TSharedRef<FJsonObject> Probe = PluginProbeJson(PluginName);
			bool bFound = false;
			bool bEnabled = false;
			Probe->TryGetBoolField(TEXT("found"), bFound);
			Probe->TryGetBoolField(TEXT("enabled"), bEnabled);
			bAnyPluginFound = bAnyPluginFound || bFound;
			bAllNamedPluginsFound = bAllNamedPluginsFound && bFound;
			bAllNamedPluginsEnabled = bAllNamedPluginsEnabled && bEnabled;
			PluginJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		TArray<TSharedPtr<FJsonValue>> ModuleJson;
		bool bAllModulesExist = true;
		for (const FString& ModuleName : Spec.ModuleNames)
		{
			TSharedRef<FJsonObject> Probe = ModuleProbeJson(ModuleName);
			bool bExists = false;
			Probe->TryGetBoolField(TEXT("exists"), bExists);
			bAllModulesExist = bAllModulesExist && bExists;
			ModuleJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		FString Status;
		if (!bVersionSatisfied)
		{
			Status = FString::Printf(TEXT("requires_ue_%d_%d"), Spec.MinMajor, Spec.MinMinor);
		}
		else if (!bAnyPluginFound)
		{
			Status = TEXT("plugin_missing");
		}
		else if (!bAllModulesExist)
		{
			Status = TEXT("module_missing");
		}
		else if (!bAllNamedPluginsEnabled)
		{
			Status = TEXT("plugin_present_not_enabled");
		}
		else
		{
			Status = TEXT("available_probe_only");
		}

		const bool bAvailable = bVersionSatisfied && bAnyPluginFound && bAllModulesExist && bAllNamedPluginsEnabled;

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("query"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("read_only"));
		OutStructured->SetStringField(TEXT("tool_name"), Spec.ToolName);
		OutStructured->SetStringField(TEXT("domain"), Spec.Domain);
		OutStructured->SetStringField(TEXT("status"), Status);
		OutStructured->SetBoolField(TEXT("available"), bAvailable);
		OutStructured->SetBoolField(TEXT("version_satisfied"), bVersionSatisfied);
		OutStructured->SetBoolField(TEXT("safe_for_ue57_baseline"), Spec.bSafeForUE57);
		OutStructured->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		OutStructured->SetStringField(TEXT("minimum_engine_version"), MinimumVersionString(Spec));
		OutStructured->SetBoolField(TEXT("any_plugin_found"), bAnyPluginFound);
		OutStructured->SetBoolField(TEXT("all_named_plugins_found"), bAllNamedPluginsFound);
		OutStructured->SetBoolField(TEXT("all_named_plugins_enabled"), bAllNamedPluginsEnabled);
		OutStructured->SetBoolField(TEXT("all_modules_exist"), bAllModulesExist);
		OutStructured->SetArrayField(TEXT("plugins"), PluginJson);
		OutStructured->SetArrayField(TEXT("modules"), ModuleJson);
		OutStructured->SetArrayField(TEXT("recommended_next_tools"), StringArrayJson(Spec.RecommendedNextTools));
		OutStructured->SetArrayField(TEXT("capability_notes"), StringArrayJson(Spec.CapabilityNotes));

		if (Spec.bScanToolsets)
		{
			OutStructured->SetObjectField(TEXT("toolset_inventory"), ToolsetInventoryJson());
		}

		OutSummary = FString::Printf(
			TEXT("%s: %s on UE %s (min %s)."),
			*Spec.ToolName,
			*Status,
			*CurrentEngineVersionString(),
			*MinimumVersionString(Spec));
		return true;
	}

	static void RegisterSpec(FSololmcpToolRegistry& Registry, const FCapabilitySpec& Spec)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Spec.ToolName;
		Def.Description = Spec.Description;
		Def.InputSchema = FSololmcpSchemaBuilder::Object({});
		Def.CacheTtlSeconds = 30;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return RunCapabilityProbe(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	static TArray<FCapabilitySpec> BaselineSpecs()
	{
		return {
			{TEXT("enhanced_input_capability_probe"), TEXT("Probe UE 5.7+ Enhanced Input availability for MCP input mapping and player-control authoring."), TEXT("input"), 5, 7, 0, true, {TEXT("EnhancedInput")}, {TEXT("EnhancedInput")}, {TEXT("plugin_inspect"), TEXT("project_settings_get")}, {TEXT("Candidate expansion: create/map InputAction and InputMappingContext assets, then bind actions through Blueprint or C++.")}},
			{TEXT("data_validation_capability_probe"), TEXT("Probe UE 5.7+ Data Validation availability for asset QA gates before MCP delivery."), TEXT("qa"), 5, 7, 0, true, {TEXT("DataValidation")}, {TEXT("DataValidation")}, {TEXT("plugin_inspect"), TEXT("asset_dependency_graph")}, {TEXT("Candidate expansion: run validation over selected assets, folders, or delivery manifests and return fail-closed QA receipts.")}},
			{TEXT("movie_render_queue_capability_probe"), TEXT("Probe UE 5.7+ Movie Render Queue availability for cinematic preview and visual QA capture."), TEXT("cinematic"), 5, 7, 0, true, {TEXT("MovieRenderPipeline")}, {TEXT("MovieRenderPipelineCore")}, {TEXT("sequence_add_camera_cuts_track"), TEXT("editor_get_screenshot")}, {TEXT("Candidate expansion: create MRQ jobs, apply output presets, render stills/sequences, and attach render receipts.")}},
			{TEXT("take_recorder_capability_probe"), TEXT("Probe UE 5.7+ Take Recorder availability for recording actor, LiveLink, and gameplay performances."), TEXT("cinematic"), 5, 7, 0, true, {TEXT("Takes")}, {TEXT("TakeRecorder")}, {TEXT("plugin_inspect"), TEXT("sequence_add_master_track")}, {TEXT("Candidate expansion: create take presets, record sources, and hand captured takes to Sequencer agents.")}},
			{TEXT("remote_control_capability_probe"), TEXT("Probe UE 5.7+ Remote Control availability for exposing scene parameters to MCP and external agents."), TEXT("remote_control"), 5, 7, 0, true, {TEXT("RemoteControl")}, {TEXT("RemoteControl")}, {TEXT("plugin_inspect"), TEXT("project_settings_get")}, {TEXT("Candidate expansion: create remote-control presets, expose actor/material properties, and drive live preview parameters.")}},
			{TEXT("variant_manager_capability_probe"), TEXT("Probe UE 5.7+ Variant Manager availability for authoring scene/product variants."), TEXT("variants"), 5, 7, 0, true, {TEXT("VariantManager"), TEXT("VariantManagerContent")}, {TEXT("VariantManagerContent")}, {TEXT("plugin_inspect"), TEXT("asset_find_references")}, {TEXT("Candidate expansion: create LevelVariantSets, variant sets, bindings, captured properties, and activation QA receipts.")}},
			{TEXT("chooser_capability_probe"), TEXT("Probe UE 5.7+ Chooser availability for animation/gameplay selection tables."), TEXT("animation"), 5, 7, 0, true, {TEXT("Chooser")}, {TEXT("Chooser")}, {TEXT("plugin_inspect"), TEXT("animbp_locomotion_plan")}, {TEXT("Candidate expansion: create chooser tables, rows, context objects, and animation selection contracts.")}},
			{TEXT("pose_search_capability_probe"), TEXT("Probe UE 5.7+ Pose Search availability for motion matching databases and animation retrieval."), TEXT("animation"), 5, 7, 0, true, {TEXT("PoseSearch")}, {TEXT("PoseSearch")}, {TEXT("plugin_inspect"), TEXT("animation_asset_compat_diagnose")}, {TEXT("Candidate expansion: create/search pose databases, validate schemas, and wire motion-matching AnimBP assets.")}},
			{TEXT("zonegraph_capability_probe"), TEXT("Probe UE 5.7+ ZoneGraph availability for lanes, traffic, crowd paths, and Mass navigation."), TEXT("world_ai"), 5, 7, 0, true, {TEXT("ZoneGraph")}, {TEXT("ZoneGraph")}, {TEXT("plugin_inspect"), TEXT("world_partition_status_lite")}, {TEXT("Candidate expansion: author zone graph lanes, tags, connectors, and Mass navigation validation receipts.")}},
			{TEXT("mass_capability_probe"), TEXT("Probe UE 5.7+ Mass Entity/GamePlay/AI/Crowd availability for scalable crowd and simulation authoring."), TEXT("world_ai"), 5, 7, 0, true, {TEXT("MassEntity"), TEXT("MassGameplay"), TEXT("MassAI"), TEXT("MassCrowd")}, {TEXT("MassSpawner"), TEXT("MassCrowd")}, {TEXT("plugin_inspect"), TEXT("pcg_generated_actor_health_audit")}, {TEXT("Candidate expansion: create Mass entity configs, spawners, processors, crowd lanes, and population receipts.")}},
			{TEXT("live_link_capability_probe"), TEXT("Probe UE 5.7+ Live Link availability for mocap, camera, lens, and virtual production streams."), TEXT("virtual_production"), 5, 7, 0, true, {TEXT("LiveLink"), TEXT("LiveLinkCamera"), TEXT("LiveLinkLens")}, {TEXT("LiveLink")}, {TEXT("plugin_inspect"), TEXT("camera_create_sequence_from_coverage_plan")}, {TEXT("Candidate expansion: inspect subjects, bind LiveLink controllers, record takes, and validate stream freshness.")}},
			{TEXT("camera_calibration_capability_probe"), TEXT("Probe UE 5.7+ Camera Calibration availability for lens files, nodal offsets, and virtual camera matching."), TEXT("virtual_production"), 5, 7, 0, true, {TEXT("CameraCalibration"), TEXT("CameraCalibrationCore")}, {TEXT("CameraCalibrationEditor")}, {TEXT("plugin_inspect"), TEXT("pcg_camera_bridge_plan")}, {TEXT("Candidate expansion: create lens files, calibration steps, camera rigs, and reconstruction camera receipts.")}},
			{TEXT("datasmith_usd_capability_probe"), TEXT("Probe UE 5.7+ Datasmith and USD importer availability for external DCC/scene ingest."), TEXT("asset_ingest"), 5, 7, 0, true, {TEXT("DatasmithImporter"), TEXT("DatasmithContent"), TEXT("USDImporter"), TEXT("USDCore")}, {TEXT("DatasmithImporter"), TEXT("USDStage")}, {TEXT("asset_ingest_from_disk"), TEXT("plugin_inspect")}, {TEXT("Candidate expansion: import Datasmith/USD scenes, preserve hierarchy/materials, and generate dependency receipts.")}},
			{TEXT("commonui_capability_probe"), TEXT("Probe UE 5.7+ CommonUI availability for production-grade UI widgets and input routing."), TEXT("umg"), 5, 7, 0, true, {TEXT("CommonUI")}, {TEXT("CommonUI")}, {TEXT("umg_capability_audit"), TEXT("plugin_inspect")}, {TEXT("Candidate expansion: create CommonActivatableWidget flows, input actions, menu stacks, and preview receipts.")}},
			{TEXT("data_registry_capability_probe"), TEXT("Probe UE 5.7+ Data Registry availability for structured gameplay/content lookup tables."), TEXT("gameplay_data"), 5, 7, 0, true, {TEXT("DataRegistry")}, {TEXT("DataRegistry")}, {TEXT("dataasset_get_property"), TEXT("plugin_inspect")}, {TEXT("Candidate expansion: create registry assets, table sources, item lookups, and validation receipts.")}},
			{TEXT("motion_design_capability_probe"), TEXT("Probe UE 5.7+ Motion Design/Avalanche availability for broadcast graphics and procedural UI scenes."), TEXT("motion_design"), 5, 7, 0, true, {TEXT("Avalanche")}, {TEXT("Avalanche")}, {TEXT("plugin_inspect"), TEXT("umg_capability_audit")}, {TEXT("Candidate expansion: create Motion Design scenes, text/shape actors, transition logic, and MRQ proof renders.")}},
			{TEXT("pcg_niagara_interop_capability_probe"), TEXT("Probe PCG/Niagara interop readiness; UE 5.8 has a dedicated PCGNiagaraInterop plugin but UE 5.7 can still report baseline state safely."), TEXT("pcg_vfx"), 5, 7, 0, true, {TEXT("PCG"), TEXT("Niagara"), TEXT("PCGNiagaraInterop")}, {TEXT("PCG"), TEXT("Niagara")}, {TEXT("pcg_node_catalog_lookup"), TEXT("niagara_hlsl_validate_custom_node")}, {TEXT("Candidate expansion: validate PCG-to-Niagara handoff availability and choose version-safe execution paths.")}}
		};
	}

	static TArray<FCapabilitySpec> UE58Specs()
	{
		return {
			{TEXT("ue58_official_mcp_capability_probe"), TEXT("Probe UE 5.8-only official ModelContextProtocol plugin presence and version gate."), TEXT("ue58_official_mcp"), 5, 8, 0, false, {TEXT("ModelContextProtocol")}, {TEXT("ModelContextProtocol"), TEXT("ModelContextProtocolEditor")}, {TEXT("plugin_inspect"), TEXT("ue58_toolset_registry_probe")}, {TEXT("UE 5.7 returns requires_ue_5_8; do not call official MCP classes from 5.7 builds.")}},
			{TEXT("ue58_toolset_registry_probe"), TEXT("Probe UE 5.8-only ToolsetRegistry plugin presence and version gate."), TEXT("ue58_toolsets"), 5, 8, 0, false, {TEXT("ToolsetRegistry")}, {TEXT("ToolsetRegistry")}, {TEXT("ue58_toolset_callable_inventory"), TEXT("plugin_inspect")}, {TEXT("Use this as the gate before any 5.8 ToolsetRegistry integration.")}},
			{TEXT("ue58_all_toolsets_probe"), TEXT("Probe UE 5.8 Experimental/Toolsets bundle presence without linking official toolset modules."), TEXT("ue58_toolsets"), 5, 8, 0, false, {TEXT("AllToolsets"), TEXT("AIModuleToolset"), TEXT("AnimationAssistantToolset"), TEXT("AutomationTestToolset"), TEXT("GASToolsets"), TEXT("NiagaraToolsets"), TEXT("UMGToolSet")}, {}, {TEXT("ue58_toolset_callable_inventory"), TEXT("plugin_list_all")}, {TEXT("Reports discovered plugin descriptors only; actual callable import should stay version gated.")}, true},
			{TEXT("ue58_toolset_callable_inventory"), TEXT("Scan UE 5.8 Experimental/Toolsets files for AICallable and ToolsetDefinition markers."), TEXT("ue58_toolsets"), 5, 8, 0, false, {TEXT("ToolsetRegistry"), TEXT("AllToolsets")}, {TEXT("ToolsetRegistry")}, {TEXT("ue58_toolset_registry_probe"), TEXT("plugin_inspect")}, {TEXT("Read-only filesystem scan; useful for comparing official callable surface against SOMOLMCP coverage.")}, true},
			{TEXT("ue58_metahuman_crowd_capability_probe"), TEXT("Probe UE 5.8 MetaHuman Crowd plugin presence for high-density human crowd scene generation."), TEXT("metahuman"), 5, 8, 0, false, {TEXT("MetaHumanCrowd")}, {TEXT("MetaHumanCrowd")}, {TEXT("mass_capability_probe"), TEXT("plugin_inspect")}, {TEXT("UE 5.8-specific production candidate: MetaHuman crowd assets plus Mass crowd deployment receipts.")}},
			{TEXT("ue58_metahuman_runtime_capability_probe"), TEXT("Probe UE 5.8 MetaHuman runtime/character plugin presence for generated character workflows."), TEXT("metahuman"), 5, 8, 0, false, {TEXT("MetaHumanRuntime"), TEXT("MetaHumanCharacter"), TEXT("MetaHumanCharacterUAF")}, {TEXT("MetaHumanCharacter")}, {TEXT("plugin_inspect"), TEXT("character_animation_pipeline")}, {TEXT("UE 5.8-specific candidate: character assembly, UAF animation handoff, and preview receipts.")}},
			{TEXT("ue58_uaf_animation_capability_probe"), TEXT("Probe UE 5.8 UAF animation plugin presence for next-generation animation authoring paths."), TEXT("animation"), 5, 8, 0, false, {TEXT("UAF"), TEXT("UAFChooser"), TEXT("UAFPoseSearch"), TEXT("UAFMass")}, {TEXT("UAFChooser"), TEXT("UAFPoseSearch")}, {TEXT("chooser_capability_probe"), TEXT("pose_search_capability_probe")}, {TEXT("UAF is version-gated; use UE 5.7 Chooser/PoseSearch paths when unavailable.")}},
			{TEXT("ue58_mass_character_trajectory_capability_probe"), TEXT("Probe UE 5.8 Mass character trajectory module availability for richer crowd movement planning."), TEXT("world_ai"), 5, 8, 0, false, {TEXT("MassGameplay"), TEXT("MassAI")}, {TEXT("MassCharacterTrajectory")}, {TEXT("mass_capability_probe"), TEXT("zonegraph_capability_probe")}, {TEXT("MassCharacterTrajectory appears in UE 5.8 MassGameplay descriptors; gate usage per module existence.")}},
			{TEXT("ue58_niagara_insights_capability_probe"), TEXT("Probe UE 5.8 Niagara Insights plugin presence for VFX profiling and diagnostics."), TEXT("vfx"), 5, 8, 0, false, {TEXT("NiagaraInsights")}, {TEXT("NiagaraInsights")}, {TEXT("niagara_hlsl_validate_custom_node"), TEXT("plugin_inspect")}, {TEXT("Candidate expansion: collect Niagara profiling/diagnostic receipts after generated effects.")}},
			{TEXT("ue58_pcg_niagara_interop_capability_probe"), TEXT("Probe UE 5.8 PCGNiagaraInterop plugin presence for direct PCG-to-Niagara workflows."), TEXT("pcg_vfx"), 5, 8, 0, false, {TEXT("PCGNiagaraInterop")}, {TEXT("PCGNiagaraInterop")}, {TEXT("pcg_niagara_interop_capability_probe"), TEXT("plugin_inspect")}, {TEXT("Gate UE 5.8 interop APIs separately from UE 5.7 baseline PCG and Niagara tools.")}},
			{TEXT("ue58_audio_insights_runtime_capability_probe"), TEXT("Probe UE 5.8 audio/live-production plugin deltas useful for recording and QA routing."), TEXT("audio"), 5, 8, 0, false, {TEXT("LiveLinkSoundDevice"), TEXT("LiveLinkOBSDevice"), TEXT("LiveLinkKiProDevice")}, {}, {TEXT("live_link_capability_probe"), TEXT("plugin_list_all")}, {TEXT("These 5.8 live-production plugins can support richer audio/video capture lanes when present.")}},
			{TEXT("ue58_material_avalanche_delta_probe"), TEXT("Probe UE 5.8 Motion Design/Avalanche material module delta for generated broadcast scenes."), TEXT("motion_design"), 5, 8, 0, false, {TEXT("Avalanche")}, {TEXT("AvalancheMaterial")}, {TEXT("motion_design_capability_probe"), TEXT("plugin_inspect")}, {TEXT("UE 5.8 Avalanche adds AvalancheMaterial; keep 5.7 material paths separate.")}},
			{TEXT("ue58_livelink_device_delta_probe"), TEXT("Probe UE 5.8 LiveLink device plugin deltas for virtual production capture planning."), TEXT("virtual_production"), 5, 8, 0, false, {TEXT("LiveLinkGenericRecordingDevice"), TEXT("LiveLinkOBSDevice"), TEXT("LiveLinkSoundDevice"), TEXT("LiveLinkKiProDevice")}, {}, {TEXT("live_link_capability_probe"), TEXT("take_recorder_capability_probe")}, {TEXT("Use as a routing fact for capture/recording agents; 5.7 should fall back to baseline LiveLink/Takes.")}}
		};
	}
}

void RegisterVersionedCapabilityTools(FSololmcpToolRegistry& Registry)
{
	for (const VersionedCapabilityTools::FCapabilitySpec& Spec : VersionedCapabilityTools::BaselineSpecs())
	{
		VersionedCapabilityTools::RegisterSpec(Registry, Spec);
	}
	for (const VersionedCapabilityTools::FCapabilitySpec& Spec : VersionedCapabilityTools::UE58Specs())
	{
		VersionedCapabilityTools::RegisterSpec(Registry, Spec);
	}
}
}
