// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 character-animation bridge tools: Sequencer autobake, Animation Mixer,
// retargeting/RigMapper, Mutable 5.8 aliases, and Live Link Face stream receipts.

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
namespace UE58AnimationBridge
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

static void AddAssetProbe(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out)
{
	FString AssetPath;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return;
	}
	FString Error;
	UObject* Asset = Context.Services.LoadAsset(AssetPath, Error);
	TSharedRef<FJsonObject> Probe = MakeShared<FJsonObject>();
	Probe->SetStringField(TEXT("asset_path"), AssetPath);
	Probe->SetBoolField(TEXT("loaded"), Asset != nullptr);
	if (Asset)
	{
		Probe->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
		Probe->SetStringField(TEXT("object_path"), Asset->GetPathName());
	}
	else
	{
		Probe->SetStringField(TEXT("error"), Error);
	}
	Out->SetObjectField(TEXT("asset_probe"), Probe);
}

static void AddAssetRegistrySample(const TArray<FString>& ClassNeedles, const FString& PackagePrefix, TSharedRef<FJsonObject>& Out)
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
		bool bMatch = ClassNeedles.IsEmpty();
		for (const FString& Needle : ClassNeedles)
		{
			if (ClassName.Contains(Needle) || Asset.AssetName.ToString().Contains(Needle))
			{
				bMatch = true;
				break;
			}
		}
		if (!bMatch) continue;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("class"), ClassName);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		if (Rows.Num() >= 32) break;
	}
	Out->SetArrayField(TEXT("asset_sample"), Rows);
	Out->SetNumberField(TEXT("asset_sample_count"), Rows.Num());
}

static FString ContractDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("UE58AnimationBridge"));
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
		Error = FString::Printf(TEXT("Failed to write UE 5.8 animation bridge contract: %s"), *Path);
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
		Error = TEXT("receipt object is required for validation.");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Checks;
	bool bAll = true;
	for (const FString& Field : Spec.RequiredReceipts)
	{
		bool bPresent = (*Receipt)->HasField(Field);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("field"), Field);
		Row->SetBoolField(TEXT("present"), bPresent);
		Checks.Add(MakeShared<FJsonValueObject>(Row));
		bAll = bAll && bPresent;
	}
	Out->SetArrayField(TEXT("checks"), Checks);
	Out->SetBoolField(TEXT("valid"), bAll);
	if (!bAll)
	{
		Error = TEXT("Receipt is missing one or more required UE 5.8 animation bridge fields.");
		return false;
	}
	return true;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional UE asset path to inspect or target."))},
		{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Optional LevelSequence path."))},
		{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Optional destination /Game path."))},
		{TEXT("subject_name"), FSololmcpSchemaBuilder::String(TEXT("Optional Live Link subject name."))},
		{TEXT("source"), FSololmcpSchemaBuilder::String(TEXT("Optional source asset, skeleton, rig, or stream."))},
		{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Optional target asset, skeleton, rig, or stream."))},
		{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Write an execution contract when true; receipt validation tools still require receipt."))},
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
	Out->SetStringField(TEXT("tool"), Spec.Name);
	Out->SetStringField(TEXT("family"), Spec.Family);
	Out->SetStringField(TEXT("mode"), Spec.Mode);
	Out->SetArrayField(TEXT("required_receipts"), StringValues(Spec.RequiredReceipts));
	AddCapabilityStatus(Spec, Out);
	AddAssetProbe(Context, Args, Out);

	FString PackagePrefix;
	Args->TryGetStringField(TEXT("package_path"), PackagePrefix);
	if (PackagePrefix.IsEmpty())
	{
		PackagePrefix = TEXT("/Game");
	}
	if (Spec.Family == TEXT("mutable58"))
	{
		AddAssetRegistrySample({TEXT("CustomizableObject"), TEXT("Mutable"), TEXT("Population")}, PackagePrefix, Out);
	}
	else if (Spec.Family == TEXT("retarget") || Spec.Family == TEXT("rigmapper"))
	{
		AddAssetRegistrySample({TEXT("IKRig"), TEXT("IKRetargeter"), TEXT("RigMapper"), TEXT("Skeleton")}, PackagePrefix, Out);
	}

	if (Spec.Mode == TEXT("validate") || Spec.Name.EndsWith(TEXT("_readback")) || Spec.Name.EndsWith(TEXT("_health_get")) || Spec.Name.EndsWith(TEXT("_preview_receipt_58")))
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
			Error = TEXT("This UE 5.8 animation bridge tool requires UE 5.8 or later.");
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
	const TArray<FString> SequencerPlugins{TEXT("SequencerAnimMixerToolset"), TEXT("AnimationAssistantToolset")};
	const TArray<FString> SequencerModules{TEXT("SequencerAnimMixerToolset"), TEXT("AnimationAssistantToolset"), TEXT("LevelSequenceEditor")};
	const TArray<FString> MixerPlugins{TEXT("SequencerAnimMixerToolset"), TEXT("AnimationLayering"), TEXT("BlendStack")};
	const TArray<FString> MixerModules{TEXT("SequencerAnimMixerToolset"), TEXT("AnimationLayering"), TEXT("BlendStack")};
	const TArray<FString> RetargetPlugins{TEXT("IKRig"), TEXT("RigMapper")};
	const TArray<FString> RetargetModules{TEXT("IKRig"), TEXT("IKRigEditor"), TEXT("RigMapper")};
	const TArray<FString> RigMapperPlugins{TEXT("RigMapper"), TEXT("RigMapperOp")};
	const TArray<FString> RigMapperModules{TEXT("RigMapper"), TEXT("RigMapperEditor")};
	const TArray<FString> MutablePlugins{TEXT("Mutable"), TEXT("MutablePopulation"), TEXT("MutableDataflow")};
	const TArray<FString> MutableModules{TEXT("CustomizableObject"), TEXT("CustomizableObjectEditor"), TEXT("MutableValidation"), TEXT("CustomizableObjectPopulation")};
	const TArray<FString> LiveLinkPlugins{TEXT("MetaHumanLiveLink"), TEXT("LiveLinkFaceImporter"), TEXT("LiveLink")};
	const TArray<FString> LiveLinkModules{TEXT("LiveLinkInterface"), TEXT("LiveLink"), TEXT("MetaHumanLiveLinkSource")};
	const TArray<FString> MetaHumanCrowdPlugins{TEXT("MetaHumanCharacter"), TEXT("MetaHumanCrowd"), TEXT("MassEntity"), TEXT("MutablePopulation")};
	const TArray<FString> MetaHumanCrowdModules{TEXT("MetaHumanCharacter"), TEXT("MetaHumanCrowd"), TEXT("MassEntity"), TEXT("MassSpawner"), TEXT("CustomizableObjectPopulation")};
	const TArray<FString> MetaHumanAnimatorPlugins{TEXT("MetaHumanAnimationTools"), TEXT("MetaHumanLiveLink"), TEXT("LiveLinkFaceImporter")};
	const TArray<FString> MetaHumanAnimatorModules{TEXT("MetaHumanAnimationTools"), TEXT("MetaHumanLiveLinkSource"), TEXT("LiveLinkInterface"), TEXT("AudioDrivenAnimation")};

	TArray<FBridgeSpec> Out;
	auto Add = [&Out](const FString& Name, const FString& Family, const FString& Mode, const TArray<FString>& Plugins, const TArray<FString>& Modules, const TArray<FString>& Receipts, bool bMutation)
	{
		Out.Add({Name, Family, Mode, Plugins, Modules, Receipts, bMutation});
	};

	Add(TEXT("sequencer_autobake_configure"), TEXT("sequencer_autobake"), TEXT("configure"), SequencerPlugins, SequencerModules, {TEXT("sequence_path"), TEXT("settings_readback"), TEXT("target_binding")}, true);
	Add(TEXT("sequencer_autobake_toggle"), TEXT("sequencer_autobake"), TEXT("toggle"), SequencerPlugins, SequencerModules, {TEXT("sequence_path"), TEXT("enabled_readback")}, true);
	Add(TEXT("sequencer_autobake_execute"), TEXT("sequencer_autobake"), TEXT("execute"), SequencerPlugins, SequencerModules, {TEXT("sequence_path"), TEXT("baked_channels"), TEXT("saved_asset"), TEXT("compile_or_eval_ok")}, true);
	Add(TEXT("sequencer_autobake_readback"), TEXT("sequencer_autobake"), TEXT("readback"), SequencerPlugins, SequencerModules, {TEXT("sequence_path"), TEXT("baked_channels"), TEXT("frame_range")}, false);

	Add(TEXT("animation_mixer_create"), TEXT("animation_mixer"), TEXT("create"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("layers"), TEXT("saved_asset")}, true);
	Add(TEXT("animation_mixer_layer_add"), TEXT("animation_mixer"), TEXT("layer_add"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("layer_id"), TEXT("blend_mode")}, true);
	Add(TEXT("animation_mixer_layer_update"), TEXT("animation_mixer"), TEXT("layer_update"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("layer_id"), TEXT("readback")}, true);
	Add(TEXT("animation_mixer_layer_remove"), TEXT("animation_mixer"), TEXT("layer_remove"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("layer_removed"), TEXT("readback")}, true);
	Add(TEXT("animation_mixer_blend_mask_bind"), TEXT("animation_mixer"), TEXT("blend_mask"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("mask_binding"), TEXT("skeleton_readback")}, true);
	Add(TEXT("animation_mixer_modifier_add"), TEXT("animation_mixer"), TEXT("modifier"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("modifier"), TEXT("readback")}, true);
	Add(TEXT("animation_mixer_transition_set"), TEXT("animation_mixer"), TEXT("transition"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("transition_rule"), TEXT("readback")}, true);
	Add(TEXT("animation_mixer_compile_validate"), TEXT("animation_mixer"), TEXT("validate"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("compile_ok"), TEXT("diagnostics")}, false);
	Add(TEXT("animation_mixer_bake"), TEXT("animation_mixer"), TEXT("bake"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("baked_sequence"), TEXT("saved_asset")}, true);
	Add(TEXT("animation_mixer_preview_receipt"), TEXT("animation_mixer"), TEXT("validate"), MixerPlugins, MixerModules, {TEXT("mixer_asset"), TEXT("preview"), TEXT("qa_receipt")}, false);

	Add(TEXT("retarget_foot_definition_get"), TEXT("retarget"), TEXT("get"), RetargetPlugins, RetargetModules, {TEXT("ik_rig"), TEXT("foot_chains")}, false);
	Add(TEXT("retarget_foot_definition_set"), TEXT("retarget"), TEXT("set"), RetargetPlugins, RetargetModules, {TEXT("ik_rig"), TEXT("foot_chains"), TEXT("saved_asset")}, true);
	Add(TEXT("retarget_foot_definition_validate"), TEXT("retarget"), TEXT("validate"), RetargetPlugins, RetargetModules, {TEXT("ik_rig"), TEXT("foot_chains"), TEXT("diagnostics")}, false);
	Add(TEXT("retarget_override_set_create"), TEXT("retarget"), TEXT("override_create"), RetargetPlugins, RetargetModules, {TEXT("retargeter"), TEXT("override_set"), TEXT("saved_asset")}, true);
	Add(TEXT("retarget_override_set_update"), TEXT("retarget"), TEXT("override_update"), RetargetPlugins, RetargetModules, {TEXT("retargeter"), TEXT("override_set"), TEXT("readback")}, true);
	Add(TEXT("retarget_override_set_apply"), TEXT("retarget"), TEXT("override_apply"), RetargetPlugins, RetargetModules, {TEXT("retargeter"), TEXT("source_animation"), TEXT("target_animation")}, true);
	Add(TEXT("retarget_override_set_validate"), TEXT("retarget"), TEXT("validate"), RetargetPlugins, RetargetModules, {TEXT("retargeter"), TEXT("override_set"), TEXT("diagnostics")}, false);

	Add(TEXT("rigmapper_capability_probe"), TEXT("rigmapper"), TEXT("probe"), RigMapperPlugins, RigMapperModules, {TEXT("plugins"), TEXT("modules")}, false);
	Add(TEXT("rigmapper_mapping_create"), TEXT("rigmapper"), TEXT("mapping_create"), RigMapperPlugins, RigMapperModules, {TEXT("mapping_asset"), TEXT("source_rig"), TEXT("target_rig")}, true);
	Add(TEXT("rigmapper_mapping_update"), TEXT("rigmapper"), TEXT("mapping_update"), RigMapperPlugins, RigMapperModules, {TEXT("mapping_asset"), TEXT("readback")}, true);
	Add(TEXT("rigmapper_mapping_auto_resolve"), TEXT("rigmapper"), TEXT("auto_resolve"), RigMapperPlugins, RigMapperModules, {TEXT("mapping_asset"), TEXT("resolved_pairs"), TEXT("unresolved_pairs")}, true);
	Add(TEXT("rigmapper_retarget_execute"), TEXT("rigmapper"), TEXT("retarget_execute"), RigMapperPlugins, RigMapperModules, {TEXT("mapping_asset"), TEXT("source_animation"), TEXT("target_animation")}, true);
	Add(TEXT("rigmapper_retarget_validate"), TEXT("rigmapper"), TEXT("validate"), RigMapperPlugins, RigMapperModules, {TEXT("mapping_asset"), TEXT("diagnostics"), TEXT("preview")}, false);

	Add(TEXT("mutable_58_feature_delta_report"), TEXT("mutable58"), TEXT("report"), MutablePlugins, MutableModules, {TEXT("feature_delta"), TEXT("class_catalog")}, false);
	Add(TEXT("mutable_graph_inspect_58"), TEXT("mutable58"), TEXT("inspect"), MutablePlugins, MutableModules, {TEXT("customizable_object"), TEXT("graph_nodes"), TEXT("parameters")}, false);
	Add(TEXT("mutable_graph_mutate_58"), TEXT("mutable58"), TEXT("mutate"), MutablePlugins, MutableModules, {TEXT("customizable_object"), TEXT("mutation"), TEXT("readback")}, true);
	Add(TEXT("mutable_compile_validate_58"), TEXT("mutable58"), TEXT("validate"), MutablePlugins, MutableModules, {TEXT("customizable_object"), TEXT("compile_ok"), TEXT("diagnostics")}, false);
	Add(TEXT("mutable_preview_receipt_58"), TEXT("mutable58"), TEXT("validate"), MutablePlugins, MutableModules, {TEXT("customizable_object"), TEXT("preview"), TEXT("generated_mesh_or_instance")}, false);

	Add(TEXT("livelink_face_video_stream_capability_probe"), TEXT("livelink_face"), TEXT("probe"), LiveLinkPlugins, LiveLinkModules, {TEXT("plugins"), TEXT("modules")}, false);
	Add(TEXT("livelink_face_video_stream_configure"), TEXT("livelink_face"), TEXT("configure"), LiveLinkPlugins, LiveLinkModules, {TEXT("subject_name"), TEXT("source_config"), TEXT("role")}, true);
	Add(TEXT("livelink_face_video_stream_start"), TEXT("livelink_face"), TEXT("start"), LiveLinkPlugins, LiveLinkModules, {TEXT("subject_name"), TEXT("stream_started"), TEXT("health")}, true);
	Add(TEXT("livelink_face_video_stream_stop"), TEXT("livelink_face"), TEXT("stop"), LiveLinkPlugins, LiveLinkModules, {TEXT("subject_name"), TEXT("stream_stopped"), TEXT("health")}, true);
	Add(TEXT("livelink_face_video_stream_health_get"), TEXT("livelink_face"), TEXT("health"), LiveLinkPlugins, LiveLinkModules, {TEXT("subject_name"), TEXT("health"), TEXT("last_frame")}, false);

	Add(TEXT("metahuman_crowd_collection_create"), TEXT("metahuman_crowd"), TEXT("collection_create"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("collection_asset"), TEXT("members"), TEXT("saved_asset")}, true);
	Add(TEXT("metahuman_crowd_collection_component_add"), TEXT("metahuman_crowd"), TEXT("component_add"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("collection_asset"), TEXT("component"), TEXT("readback")}, true);
	Add(TEXT("metahuman_crowd_collection_component_remove"), TEXT("metahuman_crowd"), TEXT("component_remove"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("collection_asset"), TEXT("component_removed"), TEXT("readback")}, true);
	Add(TEXT("metahuman_crowd_procedural_assemble"), TEXT("metahuman_crowd"), TEXT("procedural_assemble"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("collection_asset"), TEXT("variation_manifest"), TEXT("preview")}, true);
	Add(TEXT("metahuman_crowd_mass_configure"), TEXT("metahuman_crowd"), TEXT("mass_configure"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("entity_config"), TEXT("processors"), TEXT("readback")}, true);
	Add(TEXT("metahuman_crowd_ism_transition_configure"), TEXT("metahuman_crowd"), TEXT("ism_transition"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("transition_policy"), TEXT("distance_bands"), TEXT("performance_receipt")}, true);
	Add(TEXT("metahuman_crowd_runtime_spawn"), TEXT("metahuman_crowd"), TEXT("runtime_spawn"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("spawn_request"), TEXT("spawned_count"), TEXT("snapshot")}, true);
	Add(TEXT("metahuman_crowd_runtime_readback"), TEXT("metahuman_crowd"), TEXT("runtime_readback"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("spawned_count"), TEXT("identity_sample"), TEXT("performance_sample")}, false);
	Add(TEXT("metahuman_crowd_performance_receipt"), TEXT("metahuman_crowd"), TEXT("validate"), MetaHumanCrowdPlugins, MetaHumanCrowdModules, {TEXT("crowd_size"), TEXT("frame_budget"), TEXT("perf_stats")}, false);

	Add(TEXT("metahuman_animator_platform_capability_probe"), TEXT("metahuman_animator"), TEXT("probe"), MetaHumanAnimatorPlugins, MetaHumanAnimatorModules, {TEXT("plugins"), TEXT("modules")}, false);
	Add(TEXT("metahuman_animator_capture_import"), TEXT("metahuman_animator"), TEXT("capture_import"), MetaHumanAnimatorPlugins, MetaHumanAnimatorModules, {TEXT("capture_asset"), TEXT("media_source"), TEXT("frame_count")}, true);
	Add(TEXT("metahuman_animator_solve_submit"), TEXT("metahuman_animator"), TEXT("solve_submit"), MetaHumanAnimatorPlugins, MetaHumanAnimatorModules, {TEXT("solve_job"), TEXT("input_capture"), TEXT("target_identity")}, true);
	Add(TEXT("metahuman_animator_solve_status_get"), TEXT("metahuman_animator"), TEXT("solve_status"), MetaHumanAnimatorPlugins, MetaHumanAnimatorModules, {TEXT("solve_job"), TEXT("status"), TEXT("progress")}, false);
	Add(TEXT("metahuman_animator_curve_quality_audit"), TEXT("metahuman_animator"), TEXT("quality_audit"), MetaHumanAnimatorPlugins, MetaHumanAnimatorModules, {TEXT("curve_asset"), TEXT("quality_metrics"), TEXT("diagnostics")}, false);
	Add(TEXT("metahuman_animator_audio_solve_configure"), TEXT("metahuman_animator"), TEXT("audio_solve"), MetaHumanAnimatorPlugins, MetaHumanAnimatorModules, {TEXT("audio_asset"), TEXT("solve_settings"), TEXT("readback")}, true);
	Add(TEXT("metahuman_animator_export_validate"), TEXT("metahuman_animator"), TEXT("validate"), MetaHumanAnimatorPlugins, MetaHumanAnimatorModules, {TEXT("animation_asset"), TEXT("curve_asset"), TEXT("export_receipt")}, false);
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

void RegisterUE58AnimationBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const UE58AnimationBridge::FBridgeSpec& Spec : UE58AnimationBridge::Specs())
	{
		UE58AnimationBridge::RegisterSpec(Registry, Spec);
	}
}
}
