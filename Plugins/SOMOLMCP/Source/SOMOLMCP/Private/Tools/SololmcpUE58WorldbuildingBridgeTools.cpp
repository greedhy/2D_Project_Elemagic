// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 worldbuilding bridge tools: Fast Geometry Streaming,
// World Partition Insights, HLOD UX receipts, and Procedural Vegetation Editor.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"

namespace UE::SOMOLMCP
{
namespace UE58WorldbuildingBridge
{
struct FBridgeSpec
{
	FString Name;
	FString Family;
	FString Mode;
	TArray<FString> Plugins;
	TArray<FString> Modules;
	TArray<FString> RequiredReceipts;
	bool bMutation = false;
};

static bool IsUE58OrLater()
{
	const FEngineVersion& Version = FEngineVersion::Current();
	return Version.GetMajor() > 5 || (Version.GetMajor() == 5 && Version.GetMinor() >= 8);
}

static TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
}

static UWorld* CurrentEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

static TSharedRef<FJsonObject> PluginStatus(const FString& PluginName)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), PluginName);
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	Out->SetBoolField(TEXT("found"), Plugin.IsValid());
	if (Plugin.IsValid())
	{
		Out->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
		Out->SetStringField(TEXT("descriptor_path"), Plugin->GetDescriptorFileName());
		Out->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
	}
	return Out;
}

static TSharedRef<FJsonObject> ModuleStatus(const FString& ModuleName)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), ModuleName);
	FString ModulePath;
	const bool bExists = FModuleManager::Get().ModuleExists(*ModuleName, &ModulePath);
	Out->SetBoolField(TEXT("exists"), bExists);
	Out->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(*ModuleName)));
	if (bExists)
	{
		Out->SetStringField(TEXT("module_path"), ModulePath);
	}
	return Out;
}

static void AddCapabilityStatus(const FBridgeSpec& Spec, TSharedRef<FJsonObject>& Out)
{
	TArray<TSharedPtr<FJsonValue>> PluginRows;
	bool bPluginsReady = Spec.Plugins.IsEmpty();
	for (const FString& PluginName : Spec.Plugins)
	{
		TSharedRef<FJsonObject> Row = PluginStatus(PluginName);
		bool bFound = false;
		bool bEnabled = false;
		Row->TryGetBoolField(TEXT("found"), bFound);
		Row->TryGetBoolField(TEXT("enabled"), bEnabled);
		bPluginsReady = bPluginsReady || (bFound && bEnabled);
		PluginRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("plugins"), PluginRows);

	TArray<TSharedPtr<FJsonValue>> ModuleRows;
	bool bModulesReady = Spec.Modules.IsEmpty();
	for (const FString& ModuleName : Spec.Modules)
	{
		TSharedRef<FJsonObject> Row = ModuleStatus(ModuleName);
		bool bExists = false;
		Row->TryGetBoolField(TEXT("exists"), bExists);
		bModulesReady = bModulesReady || bExists;
		ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("modules"), ModuleRows);
	Out->SetBoolField(TEXT("ue58_or_later"), IsUE58OrLater());
	Out->SetBoolField(TEXT("plugins_ready"), bPluginsReady);
	Out->SetBoolField(TEXT("modules_ready"), bModulesReady);
	Out->SetBoolField(TEXT("available"), IsUE58OrLater() && bPluginsReady && bModulesReady);
}

static void AddWorldPartitionReadback(TSharedRef<FJsonObject>& Out)
{
	UWorld* World = CurrentEditorWorld();
	Out->SetBoolField(TEXT("has_editor_world"), World != nullptr);
	if (!World)
	{
		return;
	}
	Out->SetStringField(TEXT("world_name"), World->GetName());
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	Out->SetBoolField(TEXT("world_partition_enabled"), WorldPartition != nullptr);
	if (WorldPartition)
	{
		Out->SetStringField(TEXT("world_partition_name"), WorldPartition->GetName());
		Out->SetStringField(TEXT("world_partition_class"), WorldPartition->GetClass()->GetName());
		Out->SetStringField(TEXT("world_partition_runtime_hash_class"),
			WorldPartition->RuntimeHash ? WorldPartition->RuntimeHash->GetClass()->GetName() : TEXT(""));
	}
}

static void AddAssetRegistrySample(const TArray<FString>& ClassHints, const FString& PackagePrefix, TSharedRef<FJsonObject>& Out)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByPath(FName(*PackagePrefix), Assets, true);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 Matched = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (Rows.Num() >= 12)
		{
			break;
		}
		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		bool bClassMatch = ClassHints.IsEmpty();
		for (const FString& Hint : ClassHints)
		{
			if (ClassName.Contains(Hint) || Asset.AssetName.ToString().Contains(Hint))
			{
				bClassMatch = true;
				break;
			}
		}
		if (!bClassMatch)
		{
			continue;
		}
		++Matched;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetSoftObjectPath().ToString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("class"), ClassName);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetStringField(TEXT("asset_sample_prefix"), PackagePrefix);
	Out->SetNumberField(TEXT("asset_sample_match_count"), Matched);
	Out->SetArrayField(TEXT("asset_sample"), Rows);
}

static bool ValidateReceipt(
	const FBridgeSpec& Spec,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Error)
{
	const TSharedPtr<FJsonObject>* Receipt = nullptr;
	if (!Args->TryGetObjectField(TEXT("receipt"), Receipt) || !Receipt || !Receipt->IsValid())
	{
		Error = TEXT("receipt object is required for this validation/readback bridge tool.");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Missing;
	for (const FString& Field : Spec.RequiredReceipts)
	{
		if (!(*Receipt)->HasField(Field))
		{
			Missing.Add(MakeShared<FJsonValueString>(Field));
		}
	}
	Out->SetArrayField(TEXT("missing_receipt_fields"), Missing);
	Out->SetBoolField(TEXT("receipt_valid"), Missing.IsEmpty());
	if (!Missing.IsEmpty())
	{
		Error = TEXT("receipt is missing required fields.");
		return false;
	}
	return true;
}

static bool WriteContract(
	const FBridgeSpec& Spec,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Error)
{
	TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
	Contract->SetStringField(TEXT("schema"), TEXT("somol.ue58.worldbuilding_bridge.contract.v1"));
	Contract->SetStringField(TEXT("tool"), Spec.Name);
	Contract->SetStringField(TEXT("family"), Spec.Family);
	Contract->SetStringField(TEXT("mode"), Spec.Mode);
	Contract->SetStringField(TEXT("created_at"), FDateTime::UtcNow().ToIso8601());
	Contract->SetArrayField(TEXT("required_receipts"), StringValues(Spec.RequiredReceipts));
	Contract->SetObjectField(TEXT("arguments"), Args);
	AddWorldPartitionReadback(Contract);

	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Contract, Writer);

	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58WorldbuildingBridge"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString FileName = FString::Printf(TEXT("%s_%s.json"), *Spec.Name, *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
	const FString Path = FPaths::Combine(Dir, FileName);
	if (!FFileHelper::SaveStringToFile(Json, *Path))
	{
		Error = FString::Printf(TEXT("Failed to write bridge contract: %s"), *Path);
		return false;
	}
	Out->SetStringField(TEXT("contract_path"), Path);
	return true;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Optional package prefix for asset sampling, default /Game."))},
		{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Optional target region, asset, world, or HLOD selector."))},
		{TEXT("settings"), FSololmcpSchemaBuilder::Object({}, {})},
		{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Write an execution contract when true; mutation bridge tools always record a contract."))},
		{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {})}
	}, {});
}

static bool Execute(
	const FBridgeSpec& Spec,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	(void)Context;
	Out->SetStringField(TEXT("tool"), Spec.Name);
	Out->SetStringField(TEXT("family"), Spec.Family);
	Out->SetStringField(TEXT("mode"), Spec.Mode);
	Out->SetArrayField(TEXT("required_receipts"), StringValues(Spec.RequiredReceipts));
	AddCapabilityStatus(Spec, Out);
	AddWorldPartitionReadback(Out);

	FString PackagePrefix;
	Args->TryGetStringField(TEXT("package_path"), PackagePrefix);
	if (PackagePrefix.IsEmpty())
	{
		PackagePrefix = TEXT("/Game");
	}
	if (Spec.Family == TEXT("fast_geometry_streaming"))
	{
		AddAssetRegistrySample({TEXT("StaticMesh"), TEXT("Nanite"), TEXT("GeometryCache"), TEXT("LevelInstance")}, PackagePrefix, Out);
	}
	else if (Spec.Family == TEXT("pve"))
	{
		AddAssetRegistrySample({TEXT("ProceduralVegetation"), TEXT("FoliageType"), TEXT("PCG"), TEXT("StaticMesh")}, PackagePrefix, Out);
	}
	else if (Spec.Family == TEXT("hlod"))
	{
		AddAssetRegistrySample({TEXT("HLOD"), TEXT("HLODLayer"), TEXT("WorldPartition")}, PackagePrefix, Out);
	}

	if (Spec.Mode == TEXT("validate") || Spec.Name.EndsWith(TEXT("_readback")) || Spec.Name.EndsWith(TEXT("_receipt")) || Spec.Name.EndsWith(TEXT("_inspect")) || Spec.Name.EndsWith(TEXT("_audit")))
	{
		if (Args->HasField(TEXT("receipt")))
		{
			if (!ValidateReceipt(Spec, Args, Out, Error)) return false;
		}
		Out->SetBoolField(TEXT("receipt_gate_ready"), true);
	}

	bool bExecute = false;
	Args->TryGetBoolField(TEXT("execute"), bExecute);
	if (bExecute || Spec.bMutation)
	{
		if (!IsUE58OrLater())
		{
			Error = TEXT("This UE 5.8 worldbuilding bridge tool requires UE 5.8 or later.");
			return false;
		}
		if (!WriteContract(Spec, Args, Out, Error)) return false;
		Out->SetStringField(TEXT("write_scope"), TEXT("contract_only_until_plugin_specific_executor_confirms_receipt"));
	}

	Summary = FString::Printf(TEXT("%s returned UE 5.8 %s/%s bridge evidence."), *Spec.Name, *Spec.Family, *Spec.Mode);
	return true;
}

static TArray<FBridgeSpec> Specs()
{
	const TArray<FString> FastGeomPlugins{TEXT("FastGeoStreaming"), TEXT("WorldPartitionHLODUtilities"), TEXT("Nanite")};
	const TArray<FString> FastGeomModules{TEXT("Engine"), TEXT("Renderer"), TEXT("GeometryCollectionEngine"), TEXT("WorldPartitionEditor")};
	const TArray<FString> WPPlugins{TEXT("WorldPartitionHLODUtilities"), TEXT("WorldPartitionEditor")};
	const TArray<FString> WPModules{TEXT("Engine"), TEXT("UnrealEd"), TEXT("WorldPartitionEditor")};
	const TArray<FString> HlodPlugins{TEXT("WorldPartitionHLODUtilities")};
	const TArray<FString> HlodModules{TEXT("Engine"), TEXT("UnrealEd"), TEXT("WorldPartitionHLODUtilities"), TEXT("WorldPartitionEditor")};
	const TArray<FString> PvePlugins{TEXT("ProceduralVegetationEditor"), TEXT("Foliage"), TEXT("PCG"), TEXT("GeometryScripting")};
	const TArray<FString> PveModules{TEXT("Engine"), TEXT("UnrealEd"), TEXT("Foliage"), TEXT("PCG"), TEXT("GeometryScriptingCore")};

	TArray<FBridgeSpec> Out;
	auto Add = [&Out](const FString& Name, const FString& Family, const FString& Mode, const TArray<FString>& Plugins, const TArray<FString>& Modules, const TArray<FString>& Receipts, bool bMutation)
	{
		Out.Add({Name, Family, Mode, Plugins, Modules, Receipts, bMutation});
	};

	Add(TEXT("fast_geometry_streaming_capability_probe"), TEXT("fast_geometry_streaming"), TEXT("probe"), FastGeomPlugins, FastGeomModules, {TEXT("plugins"), TEXT("modules")}, false);
	Add(TEXT("fast_geometry_streaming_settings_get"), TEXT("fast_geometry_streaming"), TEXT("settings_get"), FastGeomPlugins, FastGeomModules, {TEXT("settings_readback")}, false);
	Add(TEXT("fast_geometry_streaming_settings_set"), TEXT("fast_geometry_streaming"), TEXT("settings_set"), FastGeomPlugins, FastGeomModules, {TEXT("settings_readback"), TEXT("target_binding")}, true);
	Add(TEXT("fast_geometry_streaming_product_build"), TEXT("fast_geometry_streaming"), TEXT("product_build"), FastGeomPlugins, FastGeomModules, {TEXT("product_manifest"), TEXT("source_assets"), TEXT("saved_artifacts")}, true);
	Add(TEXT("fast_geometry_streaming_product_inspect"), TEXT("fast_geometry_streaming"), TEXT("inspect"), FastGeomPlugins, FastGeomModules, {TEXT("product_manifest"), TEXT("dependency_readback")}, false);
	Add(TEXT("fast_geometry_streaming_runtime_load_test"), TEXT("fast_geometry_streaming"), TEXT("runtime_load_test"), FastGeomPlugins, FastGeomModules, {TEXT("load_request"), TEXT("loaded_assets"), TEXT("timing")}, true);
	Add(TEXT("fast_geometry_streaming_runtime_unload_test"), TEXT("fast_geometry_streaming"), TEXT("runtime_unload_test"), FastGeomPlugins, FastGeomModules, {TEXT("unload_request"), TEXT("unloaded_assets"), TEXT("timing")}, true);
	Add(TEXT("fast_geometry_streaming_budget_audit"), TEXT("fast_geometry_streaming"), TEXT("audit"), FastGeomPlugins, FastGeomModules, {TEXT("budget"), TEXT("asset_costs"), TEXT("violations")}, false);
	Add(TEXT("fast_geometry_streaming_visual_receipt"), TEXT("fast_geometry_streaming"), TEXT("validate"), FastGeomPlugins, FastGeomModules, {TEXT("viewport_capture"), TEXT("visible_assets"), TEXT("qa_receipt")}, false);

	Add(TEXT("world_partition_insights_session_start"), TEXT("world_partition_insights"), TEXT("session_start"), WPPlugins, WPModules, {TEXT("session_id"), TEXT("world_partition"), TEXT("baseline")}, true);
	Add(TEXT("world_partition_insights_session_stop"), TEXT("world_partition_insights"), TEXT("session_stop"), WPPlugins, WPModules, {TEXT("session_id"), TEXT("final_snapshot")}, true);
	Add(TEXT("world_partition_insights_snapshot"), TEXT("world_partition_insights"), TEXT("snapshot"), WPPlugins, WPModules, {TEXT("world_partition"), TEXT("cells"), TEXT("loaded_state")}, false);
	Add(TEXT("world_partition_insights_cell_query"), TEXT("world_partition_insights"), TEXT("cell_query"), WPPlugins, WPModules, {TEXT("query"), TEXT("matched_cells"), TEXT("actor_descriptors")}, false);
	Add(TEXT("world_partition_insights_streaming_anomaly_audit"), TEXT("world_partition_insights"), TEXT("audit"), WPPlugins, WPModules, {TEXT("streaming_sources"), TEXT("anomalies"), TEXT("blocking_cells")}, false);
	Add(TEXT("world_partition_insights_report_export"), TEXT("world_partition_insights"), TEXT("report_export"), WPPlugins, WPModules, {TEXT("report_path"), TEXT("session_id"), TEXT("snapshot")}, true);

	Add(TEXT("hlod_selection_build"), TEXT("hlod"), TEXT("selection_build"), HlodPlugins, HlodModules, {TEXT("selection"), TEXT("hlod_jobs"), TEXT("build_receipt")}, true);
	Add(TEXT("hlod_selected_region_build"), TEXT("hlod"), TEXT("region_build"), HlodPlugins, HlodModules, {TEXT("region"), TEXT("hlod_jobs"), TEXT("build_receipt")}, true);
	Add(TEXT("hlod_perceptual_difference_settings_get"), TEXT("hlod"), TEXT("settings_get"), HlodPlugins, HlodModules, {TEXT("settings_readback")}, false);
	Add(TEXT("hlod_perceptual_difference_settings_set"), TEXT("hlod"), TEXT("settings_set"), HlodPlugins, HlodModules, {TEXT("settings_readback"), TEXT("target_binding")}, true);
	Add(TEXT("hlod_level_direct_build"), TEXT("hlod"), TEXT("level_direct_build"), HlodPlugins, HlodModules, {TEXT("level"), TEXT("build_receipt"), TEXT("saved_packages")}, true);
	Add(TEXT("hlod_actor_delta_readback"), TEXT("hlod"), TEXT("readback"), HlodPlugins, HlodModules, {TEXT("actors"), TEXT("delta"), TEXT("packages")}, false);
	Add(TEXT("hlod_ux_build_receipt"), TEXT("hlod"), TEXT("validate"), HlodPlugins, HlodModules, {TEXT("build_receipt"), TEXT("viewport_capture"), TEXT("qa_receipt")}, false);

	Add(TEXT("pve_capability_probe"), TEXT("pve"), TEXT("probe"), PvePlugins, PveModules, {TEXT("plugins"), TEXT("modules")}, false);
	Add(TEXT("pve_asset_create"), TEXT("pve"), TEXT("asset_create"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("schema"), TEXT("saved_asset")}, true);
	Add(TEXT("pve_graph_inspect"), TEXT("pve"), TEXT("inspect"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("nodes"), TEXT("edges")}, false);
	Add(TEXT("pve_graph_node_catalog"), TEXT("pve"), TEXT("catalog"), PvePlugins, PveModules, {TEXT("node_catalog"), TEXT("pin_schema")}, false);
	Add(TEXT("pve_graph_node_add"), TEXT("pve"), TEXT("node_add"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("node_id"), TEXT("readback")}, true);
	Add(TEXT("pve_graph_node_update"), TEXT("pve"), TEXT("node_update"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("node_id"), TEXT("readback")}, true);
	Add(TEXT("pve_graph_node_remove"), TEXT("pve"), TEXT("node_remove"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("node_removed"), TEXT("readback")}, true);
	Add(TEXT("pve_graph_pin_connect"), TEXT("pve"), TEXT("pin_connect"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("from_pin"), TEXT("to_pin"), TEXT("readback")}, true);
	Add(TEXT("pve_graph_pin_disconnect"), TEXT("pve"), TEXT("pin_disconnect"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("pin"), TEXT("readback")}, true);
	Add(TEXT("pve_graph_compile_validate"), TEXT("pve"), TEXT("validate"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("compile_ok"), TEXT("diagnostics")}, false);
	Add(TEXT("pve_graph_execute"), TEXT("pve"), TEXT("execute"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("generated_count"), TEXT("execution_receipt")}, true);
	Add(TEXT("pve_graph_save_reload"), TEXT("pve"), TEXT("save_reload"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("saved_asset"), TEXT("reload_readback")}, true);
	Add(TEXT("pve_export_settings_configure"), TEXT("pve"), TEXT("export_settings"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("export_settings"), TEXT("readback")}, true);
	Add(TEXT("pve_export_static_mesh"), TEXT("pve"), TEXT("export_static_mesh"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("static_meshes"), TEXT("saved_assets")}, true);
	Add(TEXT("pve_export_nanite_validate"), TEXT("pve"), TEXT("validate"), PvePlugins, PveModules, {TEXT("static_meshes"), TEXT("nanite_state"), TEXT("diagnostics")}, false);
	Add(TEXT("pve_dynamic_wind_configure"), TEXT("pve"), TEXT("wind_configure"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("wind_settings"), TEXT("readback")}, true);
	Add(TEXT("pve_foliage_type_handoff"), TEXT("pve"), TEXT("foliage_handoff"), PvePlugins, PveModules, {TEXT("foliage_types"), TEXT("handoff_receipt"), TEXT("target_binding")}, true);
	Add(TEXT("pve_pcg_handoff"), TEXT("pve"), TEXT("pcg_handoff"), PvePlugins, PveModules, {TEXT("pcg_graph"), TEXT("handoff_receipt"), TEXT("target_binding")}, true);
	Add(TEXT("pve_preview_capture"), TEXT("pve"), TEXT("preview"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("viewport_capture"), TEXT("visible_result")}, false);
	Add(TEXT("pve_delivery_receipt_validate"), TEXT("pve"), TEXT("validate"), PvePlugins, PveModules, {TEXT("asset_path"), TEXT("saved_asset"), TEXT("preview"), TEXT("handoff_receipt")}, false);
	return Out;
}

static void RegisterSpec(FSololmcpToolRegistry& Registry, const FBridgeSpec& Spec)
{
	FSololmcpToolDefinition Def;
	Def.Name = Spec.Name;
	Def.Description = FString::Printf(TEXT("UE 5.8 %s %s bridge, capability, contract, and receipt tool."), *Spec.Family, *Spec.Mode);
	Def.InputSchema = Schema();
	Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
	Def.Execute = [Spec](
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		return Execute(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
	};
	Registry.Register(Def);
}
}

void RegisterUE58WorldbuildingBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58WorldbuildingBridge::FBridgeSpec& Spec : UE58WorldbuildingBridge::Specs())
	{
		UE58WorldbuildingBridge::RegisterSpec(Registry, Spec);
	}
}
}
