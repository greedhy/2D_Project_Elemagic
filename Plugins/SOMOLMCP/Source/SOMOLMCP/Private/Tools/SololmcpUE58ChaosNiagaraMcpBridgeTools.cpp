// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 Chaos, Niagara, and official-MCP interoperability bridge tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::SOMOLMCP
{
namespace UE58ChaosNiagaraMcpBridge
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
	for (const FString& Value : Values)
	{
		Out.Add(MakeShared<FJsonValueString>(Value));
	}
	return Out;
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
	}
	return Out;
}

static TSharedRef<FJsonObject> ModuleStatus(const FString& ModuleName)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), ModuleName);
	FString ModulePath;
	const bool bExists = ModuleExistsCompat(*ModuleName, &ModulePath);
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

static FString ContractDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58ChaosNiagaraMcpBridge"));
}

static bool WriteContract(const FBridgeSpec& Spec, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
{
	IFileManager::Get().MakeDirectory(*ContractDirectory(), true);
	const FString ContractId = FString::Printf(TEXT("%s_%s"), *Spec.Name, *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
	const FString Path = FPaths::Combine(ContractDirectory(), ContractId + TEXT(".json"));
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("tool"), Spec.Name);
	Root->SetStringField(TEXT("family"), Spec.Family);
	Root->SetStringField(TEXT("mode"), Spec.Mode);
	Root->SetBoolField(TEXT("ue58_or_later"), IsUE58OrLater());
	Root->SetArrayField(TEXT("required_receipts"), StringValues(Spec.RequiredReceipts));
	Root->SetObjectField(TEXT("arguments"), Args);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer) || !FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Error = FString::Printf(TEXT("Failed to write UE 5.8 Chaos/Niagara/MCP bridge contract: %s"), *Path);
		return false;
	}
	Out->SetStringField(TEXT("contract_id"), ContractId);
	Out->SetStringField(TEXT("contract_path"), Path);
	return true;
}

static bool ValidateReceipt(const FBridgeSpec& Spec, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
{
	const TSharedPtr<FJsonObject>* Receipt = nullptr;
	if (!Args->TryGetObjectField(TEXT("receipt"), Receipt) || !Receipt || !Receipt->IsValid())
	{
		Out->SetArrayField(TEXT("required_receipts"), StringValues(Spec.RequiredReceipts));
		Error = TEXT("receipt object is required for validation/readback.");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Checks;
	bool bAll = true;
	for (const FString& Field : Spec.RequiredReceipts)
	{
		const bool bPresent = (*Receipt)->HasField(Field);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("field"), Field);
		Row->SetBoolField(TEXT("present"), bPresent);
		Checks.Add(MakeShared<FJsonValueObject>(Row));
		bAll = bAll && bPresent;
	}
	Out->SetArrayField(TEXT("receipt_checks"), Checks);
	Out->SetBoolField(TEXT("receipt_valid"), bAll);
	if (!bAll)
	{
		Error = TEXT("receipt is missing one or more required fields.");
		return false;
	}
	return true;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("profile"), FSololmcpSchemaBuilder::String(TEXT("Optional simulation, graph, system, route, or interoperability profile id."))},
		{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Optional asset, graph, Niagara system, Chaos cache, or MCP route target."))},
		{TEXT("settings"), FSololmcpSchemaBuilder::Object({}, {})},
		{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Write an execution contract when true; mutation bridge tools always record a contract."))},
		{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {})}
	}, {});
}

static bool Execute(const FBridgeSpec& Spec, const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	(void)Context;
	Out->SetStringField(TEXT("tool"), Spec.Name);
	Out->SetStringField(TEXT("family"), Spec.Family);
	Out->SetStringField(TEXT("mode"), Spec.Mode);
	Out->SetArrayField(TEXT("required_receipts"), StringValues(Spec.RequiredReceipts));
	AddCapabilityStatus(Spec, Out);
	if (Spec.Mode.Contains(TEXT("validate")) || Spec.Mode.Contains(TEXT("audit")) || Spec.Mode.Contains(TEXT("capture")) || Spec.Mode.Contains(TEXT("inventory")) || Spec.Mode.Contains(TEXT("diff")) || Spec.Mode.Contains(TEXT("mapping")) || Spec.Mode.Contains(TEXT("report")))
	{
		if (Args->HasField(TEXT("receipt")) && !ValidateReceipt(Spec, Args, Out, Error))
		{
			return false;
		}
		Out->SetBoolField(TEXT("receipt_gate_ready"), true);
	}
	bool bExecute = false;
	Args->TryGetBoolField(TEXT("execute"), bExecute);
	if (bExecute || Spec.bMutation)
	{
		if (!IsUE58OrLater())
		{
			Error = TEXT("This UE 5.8 Chaos/Niagara/MCP bridge tool requires UE 5.8 or later.");
			return false;
		}
		if (!WriteContract(Spec, Args, Out, Error))
		{
			return false;
		}
		Out->SetStringField(TEXT("write_scope"), TEXT("contract_only_until_domain_executor_confirms_receipt"));
	}
	Summary = FString::Printf(TEXT("%s returned UE 5.8 %s/%s bridge evidence."), *Spec.Name, *Spec.Family, *Spec.Mode);
	return true;
}

static TArray<FBridgeSpec> Specs()
{
	const TArray<FString> ChaosPlugins{TEXT("ChaosSolverPlugin"), TEXT("ChaosCaching"), TEXT("Dataflow"), TEXT("ChaosVehiclesPlugin"), TEXT("ChaosClothAsset")};
	const TArray<FString> ChaosModules{TEXT("Chaos"), TEXT("ChaosSolverEngine"), TEXT("ChaosCaching"), TEXT("DataflowCore"), TEXT("GeometryCollectionEngine")};
	const TArray<FString> NiagaraPlugins{TEXT("Niagara")};
	const TArray<FString> NiagaraModules{TEXT("Niagara"), TEXT("NiagaraEditor"), TEXT("NiagaraCore")};
	const TArray<FString> McpPlugins{TEXT("SOMOLMCP")};
	const TArray<FString> McpModules{TEXT("SOMOLMCP"), TEXT("Json"), TEXT("Sockets")};
	TArray<FBridgeSpec> Out;
	auto AddChaos = [&Out, &ChaosPlugins, &ChaosModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("chaos_58"), Mode, ChaosPlugins, ChaosModules, Receipts, bMutation}); };
	auto AddNiagara = [&Out, &NiagaraPlugins, &NiagaraModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("niagara_58"), Mode, NiagaraPlugins, NiagaraModules, Receipts, bMutation}); };
	auto AddMcp = [&Out, &McpPlugins, &McpModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("official_mcp_interop"), Mode, McpPlugins, McpModules, Receipts, bMutation}); };
	AddChaos(TEXT("chaos_58_feature_delta_report"), TEXT("feature_delta_report"), {TEXT("features"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_destruction_58_settings_patch"), TEXT("settings_patch"), {TEXT("patch"), TEXT("readback"), TEXT("rollback")}, true);
	AddChaos(TEXT("chaos_destruction_58_live_validate"), TEXT("live_validate"), {TEXT("scene"), TEXT("simulation_result"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_dataflow_58_node_catalog"), TEXT("node_catalog"), {TEXT("nodes"), TEXT("schemas"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_dataflow_58_graph_mutate"), TEXT("graph_mutate"), {TEXT("graph"), TEXT("mutation"), TEXT("readback")}, true);
	AddChaos(TEXT("chaos_dataflow_58_compile_validate"), TEXT("compile_validate"), {TEXT("graph"), TEXT("compile_result"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_visual_debugger_capture_start"), TEXT("capture_start"), {TEXT("session"), TEXT("status"), TEXT("target")}, true);
	AddChaos(TEXT("chaos_visual_debugger_capture_stop"), TEXT("capture_stop"), {TEXT("session"), TEXT("artifact"), TEXT("status")}, true);
	AddChaos(TEXT("chaos_visual_debugger_snapshot"), TEXT("snapshot"), {TEXT("session"), TEXT("snapshot"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_cache_58_create"), TEXT("cache_create"), {TEXT("cache"), TEXT("settings"), TEXT("readback")}, true);
	AddChaos(TEXT("chaos_cache_58_record"), TEXT("cache_record"), {TEXT("cache"), TEXT("recording"), TEXT("artifact")}, true);
	AddChaos(TEXT("chaos_cache_58_playback_validate"), TEXT("playback_validate"), {TEXT("cache"), TEXT("playback_result"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_hair_58_settings_patch"), TEXT("hair_settings_patch"), {TEXT("patch"), TEXT("readback"), TEXT("rollback")}, true);
	AddChaos(TEXT("chaos_hair_58_simulation_validate"), TEXT("hair_simulation_validate"), {TEXT("asset"), TEXT("simulation_result"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_flesh_58_settings_patch"), TEXT("flesh_settings_patch"), {TEXT("patch"), TEXT("readback"), TEXT("rollback")}, true);
	AddChaos(TEXT("chaos_flesh_58_simulation_validate"), TEXT("flesh_simulation_validate"), {TEXT("asset"), TEXT("simulation_result"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_cloth_58_settings_patch"), TEXT("cloth_settings_patch"), {TEXT("patch"), TEXT("readback"), TEXT("rollback")}, true);
	AddChaos(TEXT("chaos_cloth_58_simulation_validate"), TEXT("cloth_simulation_validate"), {TEXT("asset"), TEXT("simulation_result"), TEXT("diagnostics")}, false);
	AddChaos(TEXT("chaos_modular_vehicle_58_configure"), TEXT("vehicle_configure"), {TEXT("vehicle"), TEXT("settings"), TEXT("readback")}, true);
	AddChaos(TEXT("chaos_modular_vehicle_58_runtime_validate"), TEXT("vehicle_runtime_validate"), {TEXT("vehicle"), TEXT("runtime_result"), TEXT("diagnostics")}, false);
	AddNiagara(TEXT("niagara_58_feature_delta_report"), TEXT("feature_delta_report"), {TEXT("features"), TEXT("diagnostics")}, false);
	AddNiagara(TEXT("niagara_58_module_compatibility_audit"), TEXT("module_compatibility_audit"), {TEXT("system"), TEXT("modules"), TEXT("diagnostics")}, false);
	AddNiagara(TEXT("niagara_58_system_upgrade_plan"), TEXT("upgrade_plan"), {TEXT("system"), TEXT("plan"), TEXT("risk")}, false);
	AddNiagara(TEXT("niagara_58_system_upgrade_execute"), TEXT("upgrade_execute"), {TEXT("system"), TEXT("plan"), TEXT("readback")}, true);
	AddNiagara(TEXT("niagara_58_compile_regression_validate"), TEXT("compile_regression_validate"), {TEXT("system"), TEXT("compile_result"), TEXT("diagnostics")}, false);
	AddNiagara(TEXT("niagara_58_runtime_preview_receipt"), TEXT("runtime_preview_receipt"), {TEXT("system"), TEXT("preview_artifact"), TEXT("diagnostics")}, false);
	AddNiagara(TEXT("niagara_58_performance_capture"), TEXT("performance_capture"), {TEXT("system"), TEXT("metrics"), TEXT("artifact")}, false);
	AddNiagara(TEXT("niagara_58_visual_regression_qa"), TEXT("visual_regression_qa"), {TEXT("system"), TEXT("image_artifact"), TEXT("qa_result")}, false);
	AddMcp(TEXT("ue58_official_mcp_server_inventory"), TEXT("server_inventory"), {TEXT("servers"), TEXT("capabilities"), TEXT("diagnostics")}, false);
	AddMcp(TEXT("ue58_official_mcp_capability_diff"), TEXT("capability_diff"), {TEXT("official"), TEXT("somolmcp"), TEXT("diff")}, false);
	AddMcp(TEXT("ue58_official_mcp_tool_mapping_get"), TEXT("tool_mapping_get"), {TEXT("mapping"), TEXT("coverage"), TEXT("diagnostics")}, false);
	AddMcp(TEXT("ue58_official_mcp_route_execute"), TEXT("route_execute"), {TEXT("route"), TEXT("result"), TEXT("receipt")}, true);
	AddMcp(TEXT("ue58_official_mcp_route_receipt_validate"), TEXT("receipt_validate"), {TEXT("route"), TEXT("receipt"), TEXT("diagnostics")}, false);
	return Out;
}

static void RegisterSpec(FSololmcpToolRegistry& Registry, const FBridgeSpec& Spec)
{
	FSololmcpToolDefinition Def;
	Def.Name = Spec.Name;
	Def.Description = FString::Printf(TEXT("UE 5.8 %s %s bridge, capability, contract, and receipt tool."), *Spec.Family, *Spec.Mode);
	Def.InputSchema = Schema();
	Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
	Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
	{
		return Execute(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
	};
	Registry.Register(Def);
}
}

void RegisterUE58ChaosNiagaraMcpBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58ChaosNiagaraMcpBridge::FBridgeSpec& Spec : UE58ChaosNiagaraMcpBridge::Specs())
	{
		UE58ChaosNiagaraMcpBridge::RegisterSpec(Registry, Spec);
	}
}
}
