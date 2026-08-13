// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 developer-iteration bridge tools: incremental cook,
// Zen cooked output store, and Horde performance report coverage.

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
namespace UE58DeveloperIterationBridge
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

static FString ContractDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58DeveloperIterationBridge"));
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
		Error = FString::Printf(TEXT("Failed to write UE 5.8 developer iteration bridge contract: %s"), *Path);
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
		{TEXT("profile"), FSololmcpSchemaBuilder::String(TEXT("Optional profile, cook target, store id, or report id."))},
		{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Optional package prefix or target package."))},
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
	if (Spec.Mode.Contains(TEXT("validate")) || Spec.Mode.Contains(TEXT("status")) || Spec.Mode.Contains(TEXT("readback")) || Spec.Mode.Contains(TEXT("probe")))
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
			Error = TEXT("This UE 5.8 developer iteration bridge tool requires UE 5.8 or later.");
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
	const TArray<FString> CookPlugins{TEXT("CookOnTheFly"), TEXT("AssetManagerEditor")};
	const TArray<FString> CookModules{TEXT("CookOnTheFly"), TEXT("UnrealEd"), TEXT("AssetRegistry")};
	const TArray<FString> ZenPlugins{TEXT("Zen"), TEXT("CookOnTheFly")};
	const TArray<FString> ZenModules{TEXT("Zen"), TEXT("ZenServer"), TEXT("CookOnTheFly")};
	const TArray<FString> HordePlugins{TEXT("Horde")};
	const TArray<FString> HordeModules{TEXT("Horde"), TEXT("Analytics"), TEXT("Json")};
	TArray<FBridgeSpec> Out;
	auto AddCook = [&Out, &CookPlugins, &CookModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("incremental_cook"), Mode, CookPlugins, CookModules, Receipts, bMutation});
	};
	auto AddZen = [&Out, &ZenPlugins, &ZenModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("zen_cooked_output_store"), Mode, ZenPlugins, ZenModules, Receipts, bMutation});
	};
	auto AddHorde = [&Out, &HordePlugins, &HordeModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("horde_performance_report"), Mode, HordePlugins, HordeModules, Receipts, bMutation});
	};
	AddCook(TEXT("incremental_cook_capability_probe"), TEXT("probe"), {TEXT("capabilities"), TEXT("platforms")}, false);
	AddCook(TEXT("incremental_cook_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddCook(TEXT("incremental_cook_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddCook(TEXT("incremental_cook_submit"), TEXT("submit"), {TEXT("job_id"), TEXT("target"), TEXT("queued")}, true);
	AddCook(TEXT("incremental_cook_status_get"), TEXT("status_get"), {TEXT("job_id"), TEXT("status"), TEXT("progress")}, false);
	AddCook(TEXT("incremental_cook_changed_package_readback"), TEXT("readback"), {TEXT("job_id"), TEXT("changed_packages"), TEXT("dependency_graph")}, false);
	AddCook(TEXT("incremental_cook_artifact_validate"), TEXT("validate"), {TEXT("job_id"), TEXT("artifacts"), TEXT("diagnostics")}, false);
	AddZen(TEXT("zen_cooked_output_store_capability_probe"), TEXT("probe"), {TEXT("capabilities"), TEXT("store_status")}, false);
	AddZen(TEXT("zen_cooked_output_store_configure"), TEXT("configure"), {TEXT("store_id"), TEXT("settings"), TEXT("readback")}, true);
	AddZen(TEXT("zen_cooked_output_store_start"), TEXT("start"), {TEXT("store_id"), TEXT("process"), TEXT("status")}, true);
	AddZen(TEXT("zen_cooked_output_store_stop"), TEXT("stop"), {TEXT("store_id"), TEXT("status")}, true);
	AddZen(TEXT("zen_cooked_output_store_status"), TEXT("status_get"), {TEXT("store_id"), TEXT("status"), TEXT("metrics")}, false);
	AddZen(TEXT("zen_cooked_output_store_cache_invalidate"), TEXT("invalidate"), {TEXT("store_id"), TEXT("invalidated_keys"), TEXT("readback")}, true);
	AddZen(TEXT("zen_cooked_output_store_artifact_readback"), TEXT("readback"), {TEXT("store_id"), TEXT("artifacts"), TEXT("hashes")}, false);
	AddHorde(TEXT("horde_performance_report_configure"), TEXT("configure"), {TEXT("report_id"), TEXT("settings"), TEXT("readback")}, true);
	AddHorde(TEXT("horde_performance_report_submit"), TEXT("submit"), {TEXT("report_id"), TEXT("submitted"), TEXT("artifact_path")}, true);
	AddHorde(TEXT("horde_performance_report_status_get"), TEXT("status_get"), {TEXT("report_id"), TEXT("status"), TEXT("progress")}, false);
	AddHorde(TEXT("horde_performance_report_artifact_validate"), TEXT("validate"), {TEXT("report_id"), TEXT("artifacts"), TEXT("diagnostics")}, false);
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

void RegisterUE58DeveloperIterationBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58DeveloperIterationBridge::FBridgeSpec& Spec : UE58DeveloperIterationBridge::Specs())
	{
		UE58DeveloperIterationBridge::RegisterSpec(Registry, Spec);
	}
}
}
