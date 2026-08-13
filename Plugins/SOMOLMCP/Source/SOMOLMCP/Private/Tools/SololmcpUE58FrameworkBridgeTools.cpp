// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 framework bridge tools: StateTree, Mass, Navigation, Iris,
// Mover, and unified input capability/contract/receipt coverage.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
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
namespace UE58FrameworkBridge
{
struct FBridgeSpec
{
	FString Name;
	FString Family;
	FString Mode;
	TArray<FString> Plugins;
	TArray<FString> Modules;
	TArray<FString> ClassHints;
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

static void AddAssetRegistrySample(const FBridgeSpec& Spec, const FString& PackagePrefix, TSharedRef<FJsonObject>& Out)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	if (!PackagePrefix.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PackagePrefix));
	}
	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FAssetData& Asset : Assets)
	{
		const FString ClassName = Asset.AssetClassPath.ToString();
		bool bMatch = Spec.ClassHints.IsEmpty();
		for (const FString& Hint : Spec.ClassHints)
		{
			if (ClassName.Contains(Hint) || Asset.AssetName.ToString().Contains(Hint))
			{
				bMatch = true;
				break;
			}
		}
		if (!bMatch)
		{
			continue;
		}
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetSoftObjectPath().ToString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("class"), ClassName);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		if (Rows.Num() >= 32)
		{
			break;
		}
	}
	Out->SetStringField(TEXT("asset_sample_prefix"), PackagePrefix);
	Out->SetArrayField(TEXT("asset_sample"), Rows);
	Out->SetNumberField(TEXT("asset_sample_count"), Rows.Num());
}

static FString ContractDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58FrameworkBridge"));
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
		Error = FString::Printf(TEXT("Failed to write UE 5.8 framework bridge contract: %s"), *Path);
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
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional target asset/package path."))},
		{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Optional package prefix for asset sampling, default /Game."))},
		{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Optional target selector such as state, processor, route, mover fixture, or device id."))},
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
	FString PackagePrefix;
	Args->TryGetStringField(TEXT("package_path"), PackagePrefix);
	if (PackagePrefix.IsEmpty())
	{
		PackagePrefix = TEXT("/Game");
	}
	AddAssetRegistrySample(Spec, PackagePrefix, Out);
	if (Spec.Mode.Contains(TEXT("validate")) || Spec.Mode.Contains(TEXT("readback")) || Spec.Mode.Contains(TEXT("snapshot")) || Spec.Mode.Contains(TEXT("audit")) || Spec.Name.EndsWith(TEXT("_validate")) || Spec.Name.EndsWith(TEXT("_readback")))
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
			Error = TEXT("This UE 5.8 framework bridge tool requires UE 5.8 or later.");
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

static void AddStateTree(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("StateTree")};
	const TArray<FString> Modules{TEXT("StateTreeModule"), TEXT("StateTreeEditorModule")};
	const TArray<FString> Classes{TEXT("StateTree")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("statetree"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("statetree_starting_state_get"), TEXT("readback"), {TEXT("asset_path"), TEXT("starting_state")}, false);
	Add(TEXT("statetree_starting_state_set"), TEXT("configure"), {TEXT("asset_path"), TEXT("starting_state"), TEXT("compile_receipt")}, true);
	Add(TEXT("statetree_compiler_extension_list"), TEXT("catalog"), {TEXT("asset_path"), TEXT("extensions")}, false);
	Add(TEXT("statetree_compiler_extension_add"), TEXT("extension_add"), {TEXT("asset_path"), TEXT("extension"), TEXT("compile_receipt")}, true);
	Add(TEXT("statetree_compiler_extension_remove"), TEXT("extension_remove"), {TEXT("asset_path"), TEXT("extension"), TEXT("compile_receipt")}, true);
	Add(TEXT("statetree_compiler_extension_validate"), TEXT("validate"), {TEXT("asset_path"), TEXT("extensions"), TEXT("diagnostics")}, false);
}

static void AddMass(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("MassEntity"), TEXT("MassAI"), TEXT("MassGameplay")};
	const TArray<FString> Modules{TEXT("MassEntity"), TEXT("MassSpawner"), TEXT("MassSimulation"), TEXT("MassAIBehavior")};
	const TArray<FString> Classes{TEXT("Mass"), TEXT("MassEntity"), TEXT("MassProcessor")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("mass"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("mass_module_inventory_58"), TEXT("inventory"), {TEXT("modules"), TEXT("processors"), TEXT("fragments")}, false);
	Add(TEXT("mass_processor_configure_58"), TEXT("configure"), {TEXT("processor"), TEXT("settings"), TEXT("readback")}, true);
	Add(TEXT("mass_runtime_scheduler_snapshot"), TEXT("snapshot"), {TEXT("world"), TEXT("scheduler"), TEXT("processors")}, false);
	Add(TEXT("mass_scale_benchmark_58"), TEXT("benchmark"), {TEXT("entity_count"), TEXT("timing"), TEXT("memory")}, true);
	Add(TEXT("mass_modular_dependency_audit"), TEXT("audit"), {TEXT("dependencies"), TEXT("missing"), TEXT("recommendations")}, false);
}

static void AddNavigation(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("NavigationSystem")};
	const TArray<FString> Modules{TEXT("NavigationSystem"), TEXT("Navmesh"), TEXT("AIModule")};
	const TArray<FString> Classes{TEXT("StaticMesh"), TEXT("Recast"), TEXT("Nav")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("navigation"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("navigation_per_mesh_walkable_get"), TEXT("readback"), {TEXT("asset_path"), TEXT("walkable_settings")}, false);
	Add(TEXT("navigation_per_mesh_walkable_set"), TEXT("configure"), {TEXT("asset_path"), TEXT("walkable_settings"), TEXT("readback")}, true);
	Add(TEXT("navigation_per_mesh_recast_readback"), TEXT("readback"), {TEXT("asset_path"), TEXT("recast_area"), TEXT("navmesh_receipt")}, false);
	Add(TEXT("navigation_per_mesh_runtime_validate"), TEXT("validate"), {TEXT("world"), TEXT("runtime_result"), TEXT("diagnostics")}, false);
}

static void AddIris(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("Iris")};
	const TArray<FString> Modules{TEXT("IrisCore"), TEXT("ReplicationSystem"), TEXT("NetCore")};
	const TArray<FString> Classes{TEXT("Replication"), TEXT("Net"), TEXT("Iris")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("iris"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("iris_capability_probe"), TEXT("probe"), {TEXT("capabilities"), TEXT("modules")}, false);
	Add(TEXT("iris_replication_descriptor_inspect"), TEXT("inspect"), {TEXT("class"), TEXT("descriptor"), TEXT("fragments")}, false);
	Add(TEXT("iris_replication_descriptor_configure"), TEXT("configure"), {TEXT("class"), TEXT("descriptor"), TEXT("readback")}, true);
	Add(TEXT("iris_filter_configure"), TEXT("configure"), {TEXT("filter"), TEXT("settings"), TEXT("readback")}, true);
	Add(TEXT("iris_prioritizer_configure"), TEXT("configure"), {TEXT("prioritizer"), TEXT("settings"), TEXT("readback")}, true);
	Add(TEXT("iris_runtime_connection_snapshot"), TEXT("snapshot"), {TEXT("connections"), TEXT("replication_state")}, false);
	Add(TEXT("iris_runtime_replication_validate"), TEXT("validate"), {TEXT("connections"), TEXT("replication_result"), TEXT("diagnostics")}, false);
	Add(TEXT("iris_diagnostics_capture"), TEXT("capture"), {TEXT("diagnostics"), TEXT("artifact_path")}, true);
}

static void AddMover(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("Mover")};
	const TArray<FString> Modules{TEXT("Mover"), TEXT("NetworkPrediction"), TEXT("PhysicsCore")};
	const TArray<FString> Classes{TEXT("Mover"), TEXT("Movement"), TEXT("Pawn")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("mover"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("mover_live_fixture_create"), TEXT("fixture_create"), {TEXT("asset_path"), TEXT("pawn"), TEXT("saved_assets")}, true);
	Add(TEXT("mover_network_prediction_live_test"), TEXT("validate"), {TEXT("fixture"), TEXT("prediction_result"), TEXT("diagnostics")}, true);
	Add(TEXT("mover_physics_interop_validate"), TEXT("validate"), {TEXT("fixture"), TEXT("physics_result"), TEXT("diagnostics")}, false);
	Add(TEXT("mover_animation_sync_validate"), TEXT("validate"), {TEXT("fixture"), TEXT("animation_sync"), TEXT("diagnostics")}, false);
	Add(TEXT("mover_restart_resume_receipt"), TEXT("validate"), {TEXT("fixture"), TEXT("restart_resume"), TEXT("diagnostics")}, false);
}

static void AddUnifiedInput(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("EnhancedInput"), TEXT("CommonUI")};
	const TArray<FString> Modules{TEXT("EnhancedInput"), TEXT("CommonUI"), TEXT("InputCore")};
	const TArray<FString> Classes{TEXT("Input"), TEXT("CommonUI"), TEXT("InputAction")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("unified_input"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("unified_input_capability_probe"), TEXT("probe"), {TEXT("capabilities"), TEXT("devices")}, false);
	Add(TEXT("unified_input_route_create"), TEXT("route_create"), {TEXT("route_id"), TEXT("mapping"), TEXT("readback")}, true);
	Add(TEXT("unified_input_route_update"), TEXT("route_update"), {TEXT("route_id"), TEXT("mapping"), TEXT("readback")}, true);
	Add(TEXT("unified_input_commonui_bridge_configure"), TEXT("configure"), {TEXT("route_id"), TEXT("commonui_readback")}, true);
	Add(TEXT("unified_input_enhanced_input_bridge_configure"), TEXT("configure"), {TEXT("route_id"), TEXT("enhanced_input_readback")}, true);
	Add(TEXT("unified_input_conflict_audit"), TEXT("audit"), {TEXT("routes"), TEXT("conflicts"), TEXT("recommendations")}, false);
	Add(TEXT("unified_input_device_readback"), TEXT("readback"), {TEXT("devices"), TEXT("bindings")}, false);
	Add(TEXT("unified_input_runtime_smoke"), TEXT("validate"), {TEXT("route_id"), TEXT("runtime_result"), TEXT("diagnostics")}, true);
}

static TArray<FBridgeSpec> Specs()
{
	TArray<FBridgeSpec> Out;
	AddStateTree(Out);
	AddMass(Out);
	AddNavigation(Out);
	AddIris(Out);
	AddMover(Out);
	AddUnifiedInput(Out);
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

void RegisterUE58FrameworkBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58FrameworkBridge::FBridgeSpec& Spec : UE58FrameworkBridge::Specs())
	{
		UE58FrameworkBridge::RegisterSpec(Registry, Spec);
	}
}
}
