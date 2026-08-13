// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 production bridge tools: Audio Insights / waveform / MetaSound,
// Interchange + USD/FBX import flow, and Movie Render Graph receipts.

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
namespace UE58ProductionBridge
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
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58ProductionBridge"));
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
		Error = FString::Printf(TEXT("Failed to write UE 5.8 production bridge contract: %s"), *Path);
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
		{TEXT("source_path"), FSololmcpSchemaBuilder::String(TEXT("Optional source file path for import/export style tools."))},
		{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Optional target selector such as graph node, audio region, render job, or pipeline id."))},
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
	if (Spec.Mode == TEXT("validate") || Spec.Mode == TEXT("readback") || Spec.Mode == TEXT("status_get") || Spec.Mode == TEXT("visual_qa") || Spec.Mode == TEXT("audit") || Spec.Name.EndsWith(TEXT("_validate")) || Spec.Name.EndsWith(TEXT("_readback")))
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
			Error = TEXT("This UE 5.8 production bridge tool requires UE 5.8 or later.");
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

static void AddAudio(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("AudioInsights"), TEXT("MetaSound"), TEXT("AudioSynesthesia"), TEXT("WaveformEditor")};
	const TArray<FString> Modules{TEXT("AudioInsights"), TEXT("MetasoundEngine"), TEXT("MetasoundEditor"), TEXT("AudioEditor"), TEXT("AudioMixer")};
	const TArray<FString> Classes{TEXT("SoundWave"), TEXT("MetaSound"), TEXT("SoundCue"), TEXT("Audio")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("audio"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("audio_insights_session_start"), TEXT("session_start"), {TEXT("session_id"), TEXT("trace_config")}, true);
	Add(TEXT("audio_insights_session_stop"), TEXT("session_stop"), {TEXT("session_id"), TEXT("trace_snapshot")}, true);
	Add(TEXT("audio_insights_trace_snapshot"), TEXT("readback"), {TEXT("session_id"), TEXT("trace_snapshot"), TEXT("timing")}, false);
	Add(TEXT("audio_insights_anomaly_audit"), TEXT("audit"), {TEXT("trace_snapshot"), TEXT("anomalies"), TEXT("recommendations")}, false);
	Add(TEXT("audio_insights_report_export"), TEXT("export"), {TEXT("report_path"), TEXT("trace_snapshot")}, true);
	Add(TEXT("audio_subtitle_asset_create"), TEXT("asset_create"), {TEXT("asset_path"), TEXT("subtitle_schema"), TEXT("saved_asset")}, true);
	Add(TEXT("audio_subtitle_entry_add"), TEXT("entry_add"), {TEXT("asset_path"), TEXT("entry_id"), TEXT("readback")}, true);
	Add(TEXT("audio_subtitle_entry_update"), TEXT("entry_update"), {TEXT("asset_path"), TEXT("entry_id"), TEXT("readback")}, true);
	Add(TEXT("audio_subtitle_timing_validate"), TEXT("validate"), {TEXT("asset_path"), TEXT("timing_report"), TEXT("diagnostics")}, false);
	Add(TEXT("audio_subtitle_runtime_preview"), TEXT("preview"), {TEXT("asset_path"), TEXT("preview_capture"), TEXT("subtitle_readback")}, true);
	Add(TEXT("waveform_editor_session_open"), TEXT("session_open"), {TEXT("session_id"), TEXT("sound_wave"), TEXT("waveform_readback")}, true);
	Add(TEXT("waveform_editor_region_select"), TEXT("region_select"), {TEXT("session_id"), TEXT("region"), TEXT("waveform_readback")}, true);
	Add(TEXT("waveform_editor_gain_apply"), TEXT("gain_apply"), {TEXT("session_id"), TEXT("region"), TEXT("gain_db"), TEXT("preview")}, true);
	Add(TEXT("waveform_editor_pitch_shift"), TEXT("pitch_shift"), {TEXT("session_id"), TEXT("region"), TEXT("semitones"), TEXT("preview")}, true);
	Add(TEXT("waveform_editor_time_stretch"), TEXT("time_stretch"), {TEXT("session_id"), TEXT("region"), TEXT("ratio"), TEXT("preview")}, true);
	Add(TEXT("waveform_editor_multichannel_edit"), TEXT("multichannel_edit"), {TEXT("session_id"), TEXT("channels"), TEXT("preview")}, true);
	Add(TEXT("waveform_editor_preview_play"), TEXT("preview_play"), {TEXT("session_id"), TEXT("playback_state"), TEXT("audition_receipt")}, true);
	Add(TEXT("waveform_editor_commit"), TEXT("commit"), {TEXT("asset_path"), TEXT("saved_asset"), TEXT("diff")}, true);
	Add(TEXT("waveform_editor_rollback"), TEXT("rollback"), {TEXT("asset_path"), TEXT("rollback_receipt")}, true);
	Add(TEXT("metasound_node_configuration_create"), TEXT("node_config_create"), {TEXT("asset_path"), TEXT("node_config"), TEXT("readback")}, true);
	Add(TEXT("metasound_node_configuration_apply"), TEXT("node_config_apply"), {TEXT("asset_path"), TEXT("node_config"), TEXT("compile_receipt")}, true);
	Add(TEXT("metasound_template_create"), TEXT("template_create"), {TEXT("asset_path"), TEXT("template_schema"), TEXT("saved_asset")}, true);
	Add(TEXT("metasound_template_instantiate"), TEXT("template_instantiate"), {TEXT("template_path"), TEXT("asset_path"), TEXT("saved_asset")}, true);
	Add(TEXT("metasound_channel_agnostic_type_validate"), TEXT("validate"), {TEXT("asset_path"), TEXT("type_report"), TEXT("diagnostics")}, false);
	Add(TEXT("metasound_58_compile_receipt"), TEXT("validate"), {TEXT("asset_path"), TEXT("compile_ok"), TEXT("diagnostics")}, false);
	Add(TEXT("sequencer_audio_track_58_configure"), TEXT("sequencer_track_configure"), {TEXT("sequence_path"), TEXT("track_id"), TEXT("readback")}, true);
	Add(TEXT("sequencer_audio_waveform_readback"), TEXT("readback"), {TEXT("sequence_path"), TEXT("waveform"), TEXT("track_readback")}, false);
}

static void AddContentPipeline(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("Interchange"), TEXT("InterchangeEditor"), TEXT("USDImporter"), TEXT("DatasmithImporter")};
	const TArray<FString> Modules{TEXT("InterchangeCore"), TEXT("InterchangeEngine"), TEXT("InterchangeEditor"), TEXT("UnrealUSDWrapper"), TEXT("UnrealEd")};
	const TArray<FString> Classes{TEXT("Interchange"), TEXT("StaticMesh"), TEXT("Texture"), TEXT("Material"), TEXT("AnimSequence")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("content_pipeline"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("interchange_import_dialog_settings_get"), TEXT("settings_get"), {TEXT("settings_readback")}, false);
	Add(TEXT("interchange_import_dialog_settings_set"), TEXT("settings_set"), {TEXT("settings_readback"), TEXT("target_binding")}, true);
	Add(TEXT("interchange_import_pipeline_inspect"), TEXT("inspect"), {TEXT("pipeline"), TEXT("steps"), TEXT("options")}, false);
	Add(TEXT("interchange_import_pipeline_configure"), TEXT("configure"), {TEXT("pipeline"), TEXT("settings"), TEXT("readback")}, true);
	Add(TEXT("interchange_usd_import"), TEXT("import"), {TEXT("source_path"), TEXT("destination_path"), TEXT("imported_assets")}, true);
	Add(TEXT("interchange_usd_reimport"), TEXT("reimport"), {TEXT("asset_path"), TEXT("source_path"), TEXT("reimport_receipt")}, true);
	Add(TEXT("interchange_usd_receipt_validate"), TEXT("validate"), {TEXT("imported_assets"), TEXT("dependency_readback"), TEXT("diagnostics")}, false);
	Add(TEXT("usd_asset_pregeneration_configure"), TEXT("configure"), {TEXT("source_path"), TEXT("pregeneration_settings"), TEXT("readback")}, true);
	Add(TEXT("usd_asset_pregeneration_execute"), TEXT("execute"), {TEXT("source_path"), TEXT("generated_assets"), TEXT("timing")}, true);
	Add(TEXT("usd_asset_pregeneration_readback"), TEXT("readback"), {TEXT("generated_assets"), TEXT("dependency_readback")}, false);
	Add(TEXT("fbx_import_performance_capture"), TEXT("capture"), {TEXT("source_path"), TEXT("timing"), TEXT("memory")}, true);
	Add(TEXT("fbx_import_performance_report"), TEXT("report"), {TEXT("report_path"), TEXT("timing"), TEXT("asset_counts")}, false);
	Add(TEXT("fbx_import_batch_58_validate"), TEXT("validate"), {TEXT("batch"), TEXT("imported_assets"), TEXT("diagnostics")}, false);
}

static void AddMovieRenderGraph(TArray<FBridgeSpec>& Out)
{
	const TArray<FString> Plugins{TEXT("MovieRenderPipeline"), TEXT("MovieRenderPipelineRenderPasses"), TEXT("MovieRenderPipelineEditor")};
	const TArray<FString> Modules{TEXT("MovieRenderPipelineCore"), TEXT("MovieRenderPipelineEditor"), TEXT("LevelSequence"), TEXT("Sequencer")};
	const TArray<FString> Classes{TEXT("MoviePipeline"), TEXT("MovieRender"), TEXT("LevelSequence"), TEXT("MovieGraph")};
	auto Add = [&Out, &Plugins, &Modules, &Classes](const TCHAR* Name, const TCHAR* Mode, TArray<FString> Receipts, bool bMutation)
	{
		Out.Add({Name, TEXT("movie_render_graph"), Mode, Plugins, Modules, Classes, Receipts, bMutation});
	};
	Add(TEXT("movie_render_graph_asset_create"), TEXT("asset_create"), {TEXT("asset_path"), TEXT("graph_schema"), TEXT("saved_asset")}, true);
	Add(TEXT("movie_render_graph_inspect"), TEXT("inspect"), {TEXT("asset_path"), TEXT("nodes"), TEXT("edges")}, false);
	Add(TEXT("movie_render_graph_node_catalog"), TEXT("catalog"), {TEXT("node_catalog"), TEXT("pin_schema")}, false);
	Add(TEXT("movie_render_graph_node_add"), TEXT("node_add"), {TEXT("asset_path"), TEXT("node_id"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_node_update"), TEXT("node_update"), {TEXT("asset_path"), TEXT("node_id"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_node_remove"), TEXT("node_remove"), {TEXT("asset_path"), TEXT("node_removed"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_pin_connect"), TEXT("pin_connect"), {TEXT("asset_path"), TEXT("from_pin"), TEXT("to_pin"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_pin_disconnect"), TEXT("pin_disconnect"), {TEXT("asset_path"), TEXT("pin"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_compile_validate"), TEXT("validate"), {TEXT("asset_path"), TEXT("compile_ok"), TEXT("diagnostics")}, false);
	Add(TEXT("movie_render_graph_light_modifier_add"), TEXT("light_modifier_add"), {TEXT("asset_path"), TEXT("modifier_id"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_light_modifier_configure"), TEXT("light_modifier_configure"), {TEXT("asset_path"), TEXT("modifier_id"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_accumulation_dof_configure"), TEXT("dof_configure"), {TEXT("asset_path"), TEXT("dof_settings"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_ndisplay_configure"), TEXT("ndisplay_configure"), {TEXT("asset_path"), TEXT("ndisplay_settings"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_basic_queue_configure"), TEXT("queue_configure"), {TEXT("asset_path"), TEXT("queue_settings"), TEXT("readback")}, true);
	Add(TEXT("movie_render_graph_job_submit"), TEXT("job_submit"), {TEXT("job_id"), TEXT("queue"), TEXT("submitted")}, true);
	Add(TEXT("movie_render_graph_job_status_get"), TEXT("status_get"), {TEXT("job_id"), TEXT("status"), TEXT("progress")}, false);
	Add(TEXT("movie_render_graph_job_cancel"), TEXT("job_cancel"), {TEXT("job_id"), TEXT("cancelled")}, true);
	Add(TEXT("movie_render_graph_output_artifact_readback"), TEXT("readback"), {TEXT("job_id"), TEXT("output_artifacts"), TEXT("metadata")}, false);
	Add(TEXT("movie_render_graph_visual_qa"), TEXT("visual_qa"), {TEXT("output_artifacts"), TEXT("reference"), TEXT("qa_receipt")}, false);
}

static TArray<FBridgeSpec> Specs()
{
	TArray<FBridgeSpec> Out;
	AddAudio(Out);
	AddContentPipeline(Out);
	AddMovieRenderGraph(Out);
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

void RegisterUE58ProductionBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58ProductionBridge::FBridgeSpec& Spec : UE58ProductionBridge::Specs())
	{
		UE58ProductionBridge::RegisterSpec(Registry, Spec);
	}
}
}
