// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 virtual production and media bridge tools.

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
namespace UE58VirtualProductionMediaBridge
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
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58VirtualProductionMediaBridge"));
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
		Error = FString::Printf(TEXT("Failed to write UE 5.8 VP/media bridge contract: %s"), *Path);
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
		{TEXT("profile"), FSololmcpSchemaBuilder::String(TEXT("Optional session, device, media source, stream, or VP profile id."))},
		{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Optional asset, component, source, layer, sequence, or device target."))},
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
	if (Spec.Mode.Contains(TEXT("validate")) || Spec.Mode.Contains(TEXT("readback")) || Spec.Mode.Contains(TEXT("status")) || Spec.Mode.Contains(TEXT("probe")))
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
			Error = TEXT("This UE 5.8 VP/media bridge tool requires UE 5.8 or later.");
			return false;
		}
		if (!WriteContract(Spec, Args, Out, Error))
		{
			return false;
		}
		Out->SetStringField(TEXT("write_scope"), TEXT("contract_only_until_vp_or_media_executor_confirms_receipt"));
	}
	Summary = FString::Printf(TEXT("%s returned UE 5.8 %s/%s bridge evidence."), *Spec.Name, *Spec.Family, *Spec.Mode);
	return true;
}

static TArray<FBridgeSpec> Specs()
{
	const TArray<FString> MocapPlugins{TEXT("LiveLink"), TEXT("TakeRecorder")};
	const TArray<FString> MocapModules{TEXT("LiveLinkInterface"), TEXT("TakeRecorder"), TEXT("MovieScene")};
	const TArray<FString> ComposurePlugins{TEXT("Composure")};
	const TArray<FString> ComposureModules{TEXT("Composure"), TEXT("ComposureLayersEditor"), TEXT("MediaAssets")};
	const TArray<FString> DisplayPlugins{TEXT("nDisplay"), TEXT("RivermaxMedia"), TEXT("LiveLinkHub")};
	const TArray<FString> DisplayModules{TEXT("DisplayCluster"), TEXT("MediaIOCore"), TEXT("LiveLinkInterface")};
	const TArray<FString> VCamPlugins{TEXT("VirtualCamera"), TEXT("VCamCore")};
	const TArray<FString> VCamModules{TEXT("VCamCore"), TEXT("CinematicCamera"), TEXT("TakeRecorder")};
	const TArray<FString> MediaPlugins{TEXT("MediaPlayerEditor"), TEXT("ImgMedia")};
	const TArray<FString> MediaModules{TEXT("MediaAssets"), TEXT("MediaUtils"), TEXT("ImgMedia"), TEXT("MediaPlayerEditor")};
	TArray<FBridgeSpec> Out;
	auto AddMocap = [&Out, &MocapPlugins, &MocapModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("mocap"), Mode, MocapPlugins, MocapModules, Receipts, bMutation}); };
	auto AddComposure = [&Out, &ComposurePlugins, &ComposureModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("composure"), Mode, ComposurePlugins, ComposureModules, Receipts, bMutation}); };
	auto AddDisplay = [&Out, &DisplayPlugins, &DisplayModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("display_cluster_io"), Mode, DisplayPlugins, DisplayModules, Receipts, bMutation}); };
	auto AddVCam = [&Out, &VCamPlugins, &VCamModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("vcam_take_recorder"), Mode, VCamPlugins, VCamModules, Receipts, bMutation}); };
	auto AddMedia = [&Out, &MediaPlugins, &MediaModules](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation){ Out.Add({Name, TEXT("media_pipeline"), Mode, MediaPlugins, MediaModules, Receipts, bMutation}); };
	AddMocap(TEXT("mocap_manager_session_create"), TEXT("session_create"), {TEXT("session"), TEXT("devices"), TEXT("readback")}, true);
	AddMocap(TEXT("mocap_manager_device_bind"), TEXT("device_bind"), {TEXT("session"), TEXT("device"), TEXT("readback")}, true);
	AddMocap(TEXT("mocap_manager_capture_start"), TEXT("capture_start"), {TEXT("session"), TEXT("capture_state"), TEXT("timecode")}, true);
	AddMocap(TEXT("mocap_manager_capture_stop"), TEXT("capture_stop"), {TEXT("session"), TEXT("capture_state"), TEXT("artifact")}, true);
	AddMocap(TEXT("mocap_manager_recording_readback"), TEXT("recording_readback"), {TEXT("session"), TEXT("takes"), TEXT("diagnostics")}, false);
	AddComposure(TEXT("composure_comp_create"), TEXT("comp_create"), {TEXT("comp"), TEXT("layers"), TEXT("readback")}, true);
	AddComposure(TEXT("composure_layer_add"), TEXT("layer_add"), {TEXT("comp"), TEXT("layer"), TEXT("readback")}, true);
	AddComposure(TEXT("composure_keyer_configure"), TEXT("keyer_configure"), {TEXT("comp"), TEXT("keyer"), TEXT("readback")}, true);
	AddComposure(TEXT("composure_preview_capture"), TEXT("preview_capture"), {TEXT("comp"), TEXT("image_artifact"), TEXT("diagnostics")}, true);
	AddDisplay(TEXT("ndisplay_auto_exposure_configure"), TEXT("exposure_configure"), {TEXT("cluster"), TEXT("settings"), TEXT("readback")}, true);
	AddDisplay(TEXT("ndisplay_auto_exposure_validate"), TEXT("exposure_validate"), {TEXT("cluster"), TEXT("render_result"), TEXT("diagnostics")}, false);
	AddDisplay(TEXT("rivermax_linux_capability_probe"), TEXT("probe"), {TEXT("capabilities"), TEXT("platform_status")}, false);
	AddDisplay(TEXT("rivermax_linux_stream_configure"), TEXT("stream_configure"), {TEXT("stream"), TEXT("settings"), TEXT("readback")}, true);
	AddDisplay(TEXT("rivermax_linux_stream_validate"), TEXT("stream_validate"), {TEXT("stream"), TEXT("diagnostics"), TEXT("metrics")}, false);
	AddDisplay(TEXT("livelink_hub_device_register"), TEXT("device_register"), {TEXT("device"), TEXT("registration"), TEXT("readback")}, true);
	AddDisplay(TEXT("livelink_hub_timecode_configure"), TEXT("timecode_configure"), {TEXT("source"), TEXT("timecode"), TEXT("readback")}, true);
	AddDisplay(TEXT("livelink_hub_timestep_configure"), TEXT("timestep_configure"), {TEXT("source"), TEXT("timestep"), TEXT("readback")}, true);
	AddDisplay(TEXT("livelink_hub_video_monitor_start"), TEXT("monitor_start"), {TEXT("source"), TEXT("status"), TEXT("metrics")}, true);
	AddDisplay(TEXT("livelink_hub_video_monitor_stop"), TEXT("monitor_stop"), {TEXT("source"), TEXT("status")}, true);
	AddVCam(TEXT("vcam_attach_constraint_add"), TEXT("constraint_add"), {TEXT("vcam"), TEXT("constraint"), TEXT("readback")}, true);
	AddVCam(TEXT("vcam_attach_constraint_update"), TEXT("constraint_update"), {TEXT("vcam"), TEXT("constraint"), TEXT("readback")}, true);
	AddVCam(TEXT("vcam_per_axis_attachment_configure"), TEXT("attachment_configure"), {TEXT("vcam"), TEXT("axis_settings"), TEXT("readback")}, true);
	AddVCam(TEXT("take_recorder_spawnable_reference_configure"), TEXT("spawnable_reference_configure"), {TEXT("take"), TEXT("spawnable"), TEXT("readback")}, true);
	AddMedia(TEXT("tiled_mipmap_video_source_create"), TEXT("source_create"), {TEXT("source"), TEXT("tiles"), TEXT("readback")}, true);
	AddMedia(TEXT("tiled_mipmap_video_encode"), TEXT("encode"), {TEXT("source"), TEXT("job_id"), TEXT("queued")}, true);
	AddMedia(TEXT("tiled_mipmap_video_validate"), TEXT("validate"), {TEXT("source"), TEXT("artifact"), TEXT("diagnostics")}, false);
	AddMedia(TEXT("video_transcoder_profile_create"), TEXT("profile_create"), {TEXT("profile"), TEXT("settings"), TEXT("readback")}, true);
	AddMedia(TEXT("video_transcoder_job_submit"), TEXT("job_submit"), {TEXT("job_id"), TEXT("profile"), TEXT("queued")}, true);
	AddMedia(TEXT("video_transcoder_job_status_get"), TEXT("status_get"), {TEXT("job_id"), TEXT("status"), TEXT("progress")}, false);
	AddMedia(TEXT("video_transcoder_job_cancel"), TEXT("job_cancel"), {TEXT("job_id"), TEXT("status")}, true);
	AddMedia(TEXT("video_transcoder_output_validate"), TEXT("validate"), {TEXT("job_id"), TEXT("output"), TEXT("diagnostics")}, false);
	AddMedia(TEXT("media_viewer_session_open"), TEXT("session_open"), {TEXT("session"), TEXT("sources"), TEXT("readback")}, true);
	AddMedia(TEXT("media_viewer_source_add"), TEXT("source_add"), {TEXT("session"), TEXT("source"), TEXT("readback")}, true);
	AddMedia(TEXT("media_viewer_compare_configure"), TEXT("compare_configure"), {TEXT("session"), TEXT("compare_settings"), TEXT("readback")}, true);
	AddMedia(TEXT("media_viewer_playback_control"), TEXT("playback_control"), {TEXT("session"), TEXT("state"), TEXT("readback")}, true);
	AddMedia(TEXT("media_viewer_capture"), TEXT("capture"), {TEXT("session"), TEXT("image_artifact"), TEXT("diagnostics")}, true);
	AddMedia(TEXT("media_viewer_session_close"), TEXT("session_close"), {TEXT("session"), TEXT("closed"), TEXT("readback")}, true);
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

void RegisterUE58VirtualProductionMediaBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58VirtualProductionMediaBridge::FBridgeSpec& Spec : UE58VirtualProductionMediaBridge::Specs())
	{
		UE58VirtualProductionMediaBridge::RegisterSpec(Registry, Spec);
	}
}
}
