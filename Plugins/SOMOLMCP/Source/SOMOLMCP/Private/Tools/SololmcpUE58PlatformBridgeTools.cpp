// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 platform bridge tools: desktop/mobile/XR target profiles,
// renderer settings, packaging validation, and remote-device receipts.

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
namespace UE58PlatformBridge
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
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58PlatformBridge"));
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
		Error = FString::Printf(TEXT("Failed to write UE 5.8 platform bridge contract: %s"), *Path);
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
		{TEXT("profile"), FSololmcpSchemaBuilder::String(TEXT("Optional platform/profile/device identifier."))},
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
	if (Spec.Mode.Contains(TEXT("validate")) || Spec.Mode.Contains(TEXT("get")) || Spec.Mode.Contains(TEXT("probe")))
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
			Error = TEXT("This UE 5.8 platform bridge tool requires UE 5.8 or later.");
			return false;
		}
		if (!WriteContract(Spec, Args, Out, Error))
		{
			return false;
		}
		Out->SetStringField(TEXT("write_scope"), TEXT("contract_only_until_platform_executor_confirms_receipt"));
	}
	Summary = FString::Printf(TEXT("%s returned UE 5.8 %s/%s bridge evidence."), *Spec.Name, *Spec.Family, *Spec.Mode);
	return true;
}

static TArray<FBridgeSpec> Specs()
{
	const TArray<FString> DesktopPlugins{TEXT("WindowsTargetPlatform")};
	const TArray<FString> DesktopModules{TEXT("WindowsTargetPlatform"), TEXT("DesktopPlatform"), TEXT("TargetPlatform")};
	const TArray<FString> MobilePlugins{TEXT("AndroidPlatformEditor"), TEXT("IOSPlatformEditor")};
	const TArray<FString> MobileModules{TEXT("AndroidPlatformEditor"), TEXT("IOSPlatformEditor"), TEXT("TargetPlatform"), TEXT("Renderer")};
	const TArray<FString> RemotePlugins{TEXT("UnrealRemote2"), TEXT("RemoteControl")};
	const TArray<FString> RemoteModules{TEXT("RemoteControl"), TEXT("RemoteControlLogic")};
	const TArray<FString> XRPlugins{TEXT("OpenXR"), TEXT("PICOXR")};
	const TArray<FString> XRModules{TEXT("OpenXRHMD"), TEXT("HeadMountedDisplay")};
	TArray<FBridgeSpec> Out;
	auto AddDesktop = [&Out, &DesktopPlugins, &DesktopModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("platform_desktop"), Mode, DesktopPlugins, DesktopModules, Receipts, bMutation});
	};
	auto AddMobile = [&Out, &MobilePlugins, &MobileModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("platform_mobile"), Mode, MobilePlugins, MobileModules, Receipts, bMutation});
	};
	auto AddRemote = [&Out, &RemotePlugins, &RemoteModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("platform_remote"), Mode, RemotePlugins, RemoteModules, Receipts, bMutation});
	};
	auto AddXR = [&Out, &XRPlugins, &XRModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("platform_xr"), Mode, XRPlugins, XRModules, Receipts, bMutation});
	};
	AddDesktop(TEXT("windows_arm64_target_profile_create"), TEXT("profile_create"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddDesktop(TEXT("windows_arm64ec_target_profile_create"), TEXT("profile_create"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddDesktop(TEXT("windows_arm_build_validate"), TEXT("validate"), {TEXT("profile"), TEXT("build_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("mobile_renderer_58_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddMobile(TEXT("mobile_renderer_58_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddMobile(TEXT("mobile_renderer_58_render_validate"), TEXT("validate"), {TEXT("profile"), TEXT("render_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("android_58_sdk_profile_validate"), TEXT("validate"), {TEXT("sdk"), TEXT("ndk"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("android_58_target_profile_apply"), TEXT("profile_apply"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddMobile(TEXT("android_58_package_validate"), TEXT("validate"), {TEXT("profile"), TEXT("package_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("preview_platform_profile_create"), TEXT("profile_create"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddMobile(TEXT("preview_platform_profile_update"), TEXT("profile_update"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddMobile(TEXT("preview_platform_render_validate"), TEXT("validate"), {TEXT("profile"), TEXT("render_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("ios_keyboard_mouse_support_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddMobile(TEXT("ios_keyboard_mouse_support_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddMobile(TEXT("ios_keyboard_mouse_runtime_validate"), TEXT("validate"), {TEXT("profile"), TEXT("runtime_result"), TEXT("diagnostics")}, false);
	AddDesktop(TEXT("steam_frame_capability_probe"), TEXT("probe"), {TEXT("capabilities"), TEXT("platform_status")}, false);
	AddDesktop(TEXT("steam_frame_target_profile_apply"), TEXT("profile_apply"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddDesktop(TEXT("steam_frame_runtime_validate"), TEXT("validate"), {TEXT("profile"), TEXT("runtime_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("ios_shader_model6_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddMobile(TEXT("ios_shader_model6_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddMobile(TEXT("ios_shader_model6_compile_validate"), TEXT("validate"), {TEXT("profile"), TEXT("compile_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("mobile_preview_platform_profile_apply"), TEXT("profile_apply"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddMobile(TEXT("mobile_preview_platform_validate"), TEXT("validate"), {TEXT("profile"), TEXT("runtime_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("mobile_gesture_event_configure"), TEXT("configure"), {TEXT("profile"), TEXT("gesture_settings"), TEXT("readback")}, true);
	AddMobile(TEXT("mobile_gesture_event_runtime_test"), TEXT("validate"), {TEXT("profile"), TEXT("gesture_result"), TEXT("diagnostics")}, false);
	AddMobile(TEXT("ios_application_extension_create"), TEXT("extension_create"), {TEXT("extension"), TEXT("settings"), TEXT("readback")}, true);
	AddMobile(TEXT("ios_application_extension_configure"), TEXT("extension_configure"), {TEXT("extension"), TEXT("settings"), TEXT("readback")}, true);
	AddMobile(TEXT("ios_application_extension_build_validate"), TEXT("validate"), {TEXT("extension"), TEXT("build_result"), TEXT("diagnostics")}, false);
	AddRemote(TEXT("unreal_remote_device_register"), TEXT("device_register"), {TEXT("device"), TEXT("registration"), TEXT("readback")}, true);
	AddRemote(TEXT("unreal_remote_device_pair_validate"), TEXT("validate"), {TEXT("device"), TEXT("pairing_result"), TEXT("diagnostics")}, false);
	AddRemote(TEXT("unreal_remote_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	AddRemote(TEXT("unreal_remote_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	AddRemote(TEXT("unreal_remote_session_validate"), TEXT("validate"), {TEXT("device"), TEXT("session_result"), TEXT("diagnostics")}, false);
	AddXR(TEXT("openxr_stereo_layer_underlay_configure"), TEXT("configure"), {TEXT("profile"), TEXT("layer_settings"), TEXT("readback")}, true);
	AddXR(TEXT("pico_openxr_controller_profile_apply"), TEXT("profile_apply"), {TEXT("profile"), TEXT("controller_settings"), TEXT("readback")}, true);
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

void RegisterUE58PlatformBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58PlatformBridge::FBridgeSpec& Spec : UE58PlatformBridge::Specs())
	{
		UE58PlatformBridge::RegisterSpec(Registry, Spec);
	}
}
}
