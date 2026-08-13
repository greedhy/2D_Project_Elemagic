// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 editor UX and Motion Design bridge tools.
// These tools expose capability/readback/contract gates without hard-linking
// unstable editor or Avalanche implementation internals into the core plugin.

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
namespace UE58EditorMotionBridge
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
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58EditorMotionBridge"));
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
		Error = FString::Printf(TEXT("Failed to write UE 5.8 editor/motion bridge contract: %s"), *Path);
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
		{TEXT("profile"), FSololmcpSchemaBuilder::String(TEXT("Optional editor profile, shortcut id, sandbox id, scene id, or Motion Design scene id."))},
		{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Optional editor, asset, actor, material, or sequence target."))},
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
	if (Spec.Mode.Contains(TEXT("validate")) || Spec.Mode.Contains(TEXT("get")) || Spec.Mode.Contains(TEXT("list")) || Spec.Mode.Contains(TEXT("inspect")) || Spec.Mode.Contains(TEXT("report")))
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
			Error = TEXT("This UE 5.8 editor/motion bridge tool requires UE 5.8 or later.");
			return false;
		}
		if (!WriteContract(Spec, Args, Out, Error))
		{
			return false;
		}
		Out->SetStringField(TEXT("write_scope"), TEXT("contract_only_until_editor_or_motion_executor_confirms_receipt"));
	}
	Summary = FString::Printf(TEXT("%s returned UE 5.8 %s/%s bridge evidence."), *Spec.Name, *Spec.Family, *Spec.Mode);
	return true;
}

static TArray<FBridgeSpec> Specs()
{
	const TArray<FString> EditorPlugins{TEXT("EditorScriptingUtilities"), TEXT("ToolMenus")};
	const TArray<FString> EditorModules{TEXT("UnrealEd"), TEXT("LevelEditor"), TEXT("EditorFramework"), TEXT("ToolMenus"), TEXT("ContentBrowser")};
	const TArray<FString> PreviewPlugins{TEXT("AssetTools")};
	const TArray<FString> PreviewModules{TEXT("UnrealEd"), TEXT("AssetTools"), TEXT("AdvancedPreviewScene"), TEXT("MaterialEditor")};
	const TArray<FString> MotionPlugins{TEXT("Avalanche"), TEXT("MotionDesign")};
	const TArray<FString> MotionModules{TEXT("Avalanche"), TEXT("AvalancheEditor"), TEXT("LevelSequence"), TEXT("MovieScene")};
	TArray<FBridgeSpec> Out;
	auto AddEditor = [&Out, &EditorPlugins, &EditorModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("editor_ux"), Mode, EditorPlugins, EditorModules, Receipts, bMutation});
	};
	auto AddPreview = [&Out, &PreviewPlugins, &PreviewModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("editor_preview"), Mode, PreviewPlugins, PreviewModules, Receipts, bMutation});
	};
	auto AddMotion = [&Out, &MotionPlugins, &MotionModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("motion_design"), Mode, MotionPlugins, MotionModules, Receipts, bMutation});
	};
	AddEditor(TEXT("editor_gizmo_system_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddEditor(TEXT("editor_gizmo_system_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddEditor(TEXT("editor_gizmo_context_override"), TEXT("context_override"), {TEXT("context"), TEXT("readback")}, true);
	AddPreview(TEXT("preview_scene_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddPreview(TEXT("preview_scene_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddPreview(TEXT("preview_scene_profile_create"), TEXT("profile_create"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddPreview(TEXT("material_preview_editor_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddPreview(TEXT("material_preview_editor_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddPreview(TEXT("material_preview_editor_capture"), TEXT("capture"), {TEXT("target"), TEXT("image_artifact"), TEXT("diagnostics")}, true);
	AddEditor(TEXT("shortcut_manager_binding_get"), TEXT("binding_get"), {TEXT("shortcut"), TEXT("binding_readback")}, false);
	AddEditor(TEXT("shortcut_manager_binding_set"), TEXT("binding_set"), {TEXT("shortcut"), TEXT("binding_readback"), TEXT("conflict_audit")}, true);
	AddEditor(TEXT("shortcut_manager_conflict_audit"), TEXT("conflict_audit"), {TEXT("shortcuts"), TEXT("conflicts"), TEXT("diagnostics")}, false);
	AddEditor(TEXT("editor_preferences_patch_58"), TEXT("preferences_patch"), {TEXT("patch"), TEXT("readback"), TEXT("rollback")}, true);
	AddEditor(TEXT("orthographic_viewport_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddEditor(TEXT("orthographic_viewport_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddEditor(TEXT("content_browser_context_action_execute"), TEXT("context_action"), {TEXT("target"), TEXT("action"), TEXT("readback")}, true);
	AddEditor(TEXT("details_panel_favorite_add"), TEXT("favorite_add"), {TEXT("target"), TEXT("favorites_readback")}, true);
	AddEditor(TEXT("details_panel_favorite_remove"), TEXT("favorite_remove"), {TEXT("target"), TEXT("favorites_readback")}, true);
	AddEditor(TEXT("details_panel_favorites_list"), TEXT("favorites_list"), {TEXT("favorites_readback")}, false);
	AddEditor(TEXT("editor_sandbox_session_create"), TEXT("sandbox_create"), {TEXT("session_id"), TEXT("root"), TEXT("readback")}, true);
	AddEditor(TEXT("editor_sandbox_session_list"), TEXT("sandbox_list"), {TEXT("sessions")}, false);
	AddEditor(TEXT("editor_sandbox_session_close"), TEXT("sandbox_close"), {TEXT("session_id"), TEXT("closed"), TEXT("readback")}, true);
	AddMotion(TEXT("motion_design_58_feature_delta_report"), TEXT("feature_delta_report"), {TEXT("features"), TEXT("diagnostics")}, false);
	AddMotion(TEXT("motion_design_avalanche_scene_create"), TEXT("scene_create"), {TEXT("scene"), TEXT("actors"), TEXT("readback")}, true);
	AddMotion(TEXT("motion_design_avalanche_scene_inspect"), TEXT("scene_inspect"), {TEXT("scene"), TEXT("readback")}, false);
	AddMotion(TEXT("motion_design_avalanche_actor_add"), TEXT("actor_add"), {TEXT("scene"), TEXT("actor"), TEXT("readback")}, true);
	AddMotion(TEXT("motion_design_avalanche_material_bind"), TEXT("material_bind"), {TEXT("scene"), TEXT("material"), TEXT("readback")}, true);
	AddMotion(TEXT("motion_design_avalanche_sequence_bind"), TEXT("sequence_bind"), {TEXT("scene"), TEXT("sequence"), TEXT("readback")}, true);
	AddMotion(TEXT("motion_design_avalanche_compile_validate"), TEXT("compile_validate"), {TEXT("scene"), TEXT("compile_result"), TEXT("diagnostics")}, false);
	AddMotion(TEXT("motion_design_avalanche_render_receipt"), TEXT("render_receipt"), {TEXT("scene"), TEXT("image_artifact"), TEXT("diagnostics")}, false);
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

void RegisterUE58EditorMotionBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58EditorMotionBridge::FBridgeSpec& Spec : UE58EditorMotionBridge::Specs())
	{
		UE58EditorMotionBridge::RegisterSpec(Registry, Spec);
	}
}
}
