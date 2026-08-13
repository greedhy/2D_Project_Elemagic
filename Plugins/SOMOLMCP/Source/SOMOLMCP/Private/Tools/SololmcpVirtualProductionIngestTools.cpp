// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpVirtualProductionIngestTools.cpp
// ----------------------------------------------------------------------------
// UE 5.7+ virtual-production, remote-control, variants, LiveLink, camera
// calibration, USD/Datasmith planning and probe tools. Optional plugin APIs are
// only reached through guarded Python/reflection paths.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"

namespace UE::SOMOLMCP
{
namespace VirtualProductionIngestTools
{
	struct FOptionalProductionSpec
	{
		FString Name;
		FString Description;
		FString Domain;
		FString Mode;
		TArray<FString> Plugins;
		TArray<FString> Modules;
		TArray<FString> ClassNeedles;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
	};

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

	static TArray<FString> GetStringArrayField(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName)
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
		if (!Arguments->TryGetArrayField(FieldName, Raw) || !Raw)
		{
			return Values;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Raw)
		{
			if (Value.IsValid())
			{
				const FString StringValue = Value->AsString();
				if (!StringValue.IsEmpty())
				{
					Values.Add(StringValue);
				}
			}
		}
		return Values;
	}

	static FString PyQuote(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Value.ReplaceInline(TEXT("'"), TEXT("\\'"));
		Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		return FString::Printf(TEXT("'%s'"), *Value);
	}

	static FString PyList(const TArray<FString>& Values)
	{
		TArray<FString> Quoted;
		for (const FString& Value : Values)
		{
			Quoted.Add(PyQuote(Value));
		}
		return FString::Printf(TEXT("[%s]"), *FString::Join(Quoted, TEXT(",")));
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
			return Obj;
		}
		const FPluginDescriptor& Desc = Plugin->GetDescriptor();
		Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
		Obj->SetStringField(TEXT("friendly_name"), Desc.FriendlyName);
		Obj->SetStringField(TEXT("version_name"), Desc.VersionName);
		Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
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

	static bool ProbeAvailability(const FOptionalProductionSpec& Spec, TSharedRef<FJsonObject>& Out, FString& OutStatus)
	{
		TArray<TSharedPtr<FJsonValue>> PluginsJson;
		bool bAnyPluginFound = Spec.Plugins.IsEmpty();
		bool bAllPluginsEnabled = true;
		for (const FString& PluginName : Spec.Plugins)
		{
			TSharedRef<FJsonObject> Probe = PluginProbeJson(PluginName);
			bool bFound = false;
			bool bEnabled = false;
			Probe->TryGetBoolField(TEXT("found"), bFound);
			Probe->TryGetBoolField(TEXT("enabled"), bEnabled);
			bAnyPluginFound = bAnyPluginFound || bFound;
			bAllPluginsEnabled = bAllPluginsEnabled && bEnabled;
			PluginsJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		TArray<TSharedPtr<FJsonValue>> ModulesJson;
		bool bAllModulesExist = true;
		for (const FString& ModuleName : Spec.Modules)
		{
			TSharedRef<FJsonObject> Probe = ModuleProbeJson(ModuleName);
			bool bExists = false;
			Probe->TryGetBoolField(TEXT("exists"), bExists);
			bAllModulesExist = bAllModulesExist && bExists;
			ModulesJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		if (!bAnyPluginFound)
		{
			OutStatus = TEXT("plugin_missing");
		}
		else if (!bAllModulesExist)
		{
			OutStatus = TEXT("module_missing");
		}
		else if (!bAllPluginsEnabled)
		{
			OutStatus = TEXT("plugin_present_not_enabled");
		}
		else
		{
			OutStatus = TEXT("available");
		}

		Out->SetStringField(TEXT("status"), OutStatus);
		Out->SetBoolField(TEXT("available"), OutStatus == TEXT("available"));
		Out->SetArrayField(TEXT("plugins"), PluginsJson);
		Out->SetArrayField(TEXT("modules"), ModulesJson);
		return OutStatus == TEXT("available");
	}

	static TSharedRef<FJsonObject> GenericSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Target preset, actor, level, session, or import path."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder to scan, default /Game."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Optional source asset paths."))},
			{TEXT("source_file"), FSololmcpSchemaBuilder::String(TEXT("Optional external file path for ingest planning."))},
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Optional destination or capture output path."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum assets or subjects to inspect."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan/probe only; no editor mutation."))}
		});
	}

	static void SetBaseReceipt(const FOptionalProductionSpec& Spec, TSharedRef<FJsonObject>& Out)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetBoolField(TEXT("read_only"), true);
		Out->SetStringField(TEXT("tool_name"), Spec.Name);
		Out->SetStringField(TEXT("domain"), Spec.Domain);
		Out->SetStringField(TEXT("operation_class"), Spec.Mode == TEXT("plan") ? TEXT("query") : TEXT("query"));
		Out->SetStringField(TEXT("safety_class"), TEXT("read_only"));
	}

	static FString BuildClassProbePython(const FString& ToolName, const FString& FolderPath, const TArray<FString>& ClassNeedles, const int32 MaxAssets)
	{
		return FString::Printf(TEXT(
			"import unreal, json\n"
			"folder = %s or '/Game'\n"
			"needles = [n.lower() for n in %s]\n"
			"max_assets = %d\n"
			"items = []\n"
			"paths = []\n"
			"if hasattr(unreal, 'EditorAssetLibrary'):\n"
			"    paths = list(unreal.EditorAssetLibrary.list_assets(folder, True, False))\n"
			"for p in paths:\n"
			"    if max_assets > 0 and len(items) >= max_assets:\n"
			"        break\n"
			"    try:\n"
			"        asset = unreal.EditorAssetLibrary.load_asset(p)\n"
			"        if not asset:\n"
			"            continue\n"
			"        cls = asset.get_class().get_name()\n"
			"        if any(n in cls.lower() or n in p.lower() for n in needles):\n"
			"            items.append({'path': p, 'class': cls})\n"
			"    except Exception as exc:\n"
			"        items.append({'path': p, 'error': str(exc)})\n"
			"print('__SOMOLMCP_JSON__' + json.dumps({'tool': %s, 'folder_path': folder, 'items': items, 'count': len(items), 'class_needles': needles}, default=str))\n"),
			*PyQuote(FolderPath),
			*PyList(ClassNeedles),
			MaxAssets,
			*PyQuote(ToolName));
	}

	static FString BuildLiveLinkProbePython()
	{
		return TEXT(
			"import unreal, json\n"
			"result = {'tool':'live_link_subjects_probe','status':'attempted','subjects':[],'api':'LiveLinkBlueprintLibrary'}\n"
			"try:\n"
			"    lib = unreal.LiveLinkBlueprintLibrary if hasattr(unreal, 'LiveLinkBlueprintLibrary') else None\n"
			"    if not lib:\n"
			"        result['status'] = 'python_api_missing'\n"
			"    else:\n"
			"        result['available_methods'] = [m for m in dir(lib) if 'subject' in m.lower() or 'live' in m.lower()]\n"
			"        for method in ['get_live_link_subject_names', 'get_subject_names', 'get_live_link_enabled_subject_names']:\n"
			"            if hasattr(lib, method):\n"
			"                try:\n"
			"                    result['subjects'] = [str(x) for x in getattr(lib, method)()]\n"
			"                    result['method'] = method\n"
			"                    result['status'] = 'completed_with_receipt'\n"
			"                    break\n"
			"                except Exception as exc:\n"
			"                    result.setdefault('method_errors', []).append({'method': method, 'error': str(exc)})\n"
			"except Exception as exc:\n"
			"    result['status'] = 'error'\n"
			"    result['error'] = str(exc)\n"
			"print('__SOMOLMCP_JSON__' + json.dumps(result, default=str))\n");
	}

	static FString BuildVpHealthPython(const FString& FolderPath, const int32 MaxAssets)
	{
		return FString::Printf(TEXT(
			"import unreal, json\n"
			"folder = %s or '/Game'\n"
			"max_assets = %d\n"
			"result = {'tool':'vp_capture_health_probe','folder_path':folder,'subjects':[],'take_assets':[],'remote_control_assets':[],'variant_assets':[]}\n"
			"try:\n"
			"    lib = unreal.LiveLinkBlueprintLibrary if hasattr(unreal, 'LiveLinkBlueprintLibrary') else None\n"
			"    if lib and hasattr(lib, 'get_live_link_subject_names'):\n"
			"        result['subjects'] = [str(x) for x in lib.get_live_link_subject_names()]\n"
			"except Exception as exc:\n"
			"    result['livelink_error'] = str(exc)\n"
			"needles = {'take_assets':['take','levelsequence'], 'remote_control_assets':['remotecontrol'], 'variant_assets':['variant','levelvariantsets']}\n"
			"if hasattr(unreal, 'EditorAssetLibrary'):\n"
			"    for p in list(unreal.EditorAssetLibrary.list_assets(folder, True, False)):\n"
			"        if max_assets > 0 and sum(len(v) for v in result.values() if isinstance(v, list)) >= max_assets:\n"
			"            break\n"
			"        try:\n"
			"            a = unreal.EditorAssetLibrary.load_asset(p)\n"
			"            if not a:\n"
			"                continue\n"
			"            cls = a.get_class().get_name().lower()\n"
			"            for key, ns in needles.items():\n"
			"                if any(n in cls or n in p.lower() for n in ns):\n"
			"                    result[key].append({'path': p, 'class': a.get_class().get_name()})\n"
			"        except Exception:\n"
			"            pass\n"
			"print('__SOMOLMCP_JSON__' + json.dumps(result, default=str))\n"),
			*PyQuote(FolderPath),
			MaxAssets);
	}

	static bool RunPythonWrapped(
		const FSololmcpToolExecutionContext& Context,
		const FString& Code,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		TSharedRef<FJsonObject> PythonResult = MakeShared<FJsonObject>();
		FString PythonSummary;
		FString PythonError;
		const bool bOk = Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, PythonResult, PythonSummary, PythonError);
		OutStructured->SetObjectField(TEXT("python"), PythonResult);
		OutStructured->SetStringField(TEXT("python_summary"), PythonSummary);
		if (!PythonError.IsEmpty())
		{
			OutStructured->SetStringField(TEXT("python_error"), PythonError);
		}
		if (!bOk)
		{
			OutSummary = PythonSummary;
			OutError = PythonError.IsEmpty() ? TEXT("Python execution failed.") : PythonError;
			return false;
		}
		return true;
	}

	static bool ExecuteSpec(
		const FOptionalProductionSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		SetBaseReceipt(Spec, OutStructured);
		FString Status;
		ProbeAvailability(Spec, OutStructured, Status);

		FString Target;
		FString FolderPath = TEXT("/Game");
		FString SourceFile;
		FString OutputPath;
		Arguments->TryGetStringField(TEXT("target"), Target);
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		Arguments->TryGetStringField(TEXT("source_file"), SourceFile);
		Arguments->TryGetStringField(TEXT("output_path"), OutputPath);
		int32 MaxAssets = 200;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);

		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetStringField(TEXT("folder_path"), FolderPath);
		OutStructured->SetStringField(TEXT("source_file"), SourceFile);
		OutStructured->SetStringField(TEXT("output_path"), OutputPath);
		OutStructured->SetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetArrayField(TEXT("asset_paths"), StringArrayJson(GetStringArrayField(Arguments, TEXT("asset_paths"))));

		if (Spec.Mode == TEXT("plan"))
		{
			OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
			OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			OutSummary = FString::Printf(TEXT("%s returned a %s production plan."), *Spec.Name, *Status);
			return true;
		}

		FString Code;
		if (Spec.Mode == TEXT("livelink_probe"))
		{
			Code = BuildLiveLinkProbePython();
		}
		else if (Spec.Mode == TEXT("vp_health_probe"))
		{
			Code = BuildVpHealthPython(FolderPath, MaxAssets);
		}
		else
		{
			Code = BuildClassProbePython(Spec.Name, FolderPath, Spec.ClassNeedles, MaxAssets);
		}

		const bool bOk = RunPythonWrapped(Context, Code, OutStructured, OutSummary, OutError);
		if (bOk)
		{
			OutSummary = FString::Printf(TEXT("%s completed %s."), *Spec.Name, *Spec.Mode);
		}
		return bOk;
	}

	static void RegisterSpec(FSololmcpToolRegistry& Registry, const FOptionalProductionSpec& Spec)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = Spec.Description;
		Def.InputSchema = GenericSchema();
		Def.CacheTtlSeconds = Spec.Mode == TEXT("plan") ? 15 : 30;
		Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return ExecuteSpec(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	static TArray<FOptionalProductionSpec> Specs()
	{
		return {
			{TEXT("remote_control_preset_plan"), TEXT("Plan Remote Control preset exposure for scene parameters, material controls, and live preview automation."), TEXT("remote_control"), TEXT("plan"), {TEXT("RemoteControl")}, {TEXT("RemoteControl")}, {}, {TEXT("Resolve target actors/materials."), TEXT("Create or choose Remote Control preset."), TEXT("Expose whitelisted properties only."), TEXT("Require readback of exposed fields and blocked dangerous actions.")}, {TEXT("preset_path"), TEXT("exposed_properties"), TEXT("target_binding"), TEXT("readback_receipt")}},
			{TEXT("remote_control_preset_probe"), TEXT("Scan content for Remote Control preset assets and return a guarded routing receipt."), TEXT("remote_control"), TEXT("class_probe"), {TEXT("RemoteControl")}, {TEXT("RemoteControl")}, {TEXT("RemoteControlPreset"), TEXT("RemoteControl")}, {}, {}},
			{TEXT("variant_manager_level_variant_sets_plan"), TEXT("Plan LevelVariantSets authoring for product, lighting, layout, or material variants."), TEXT("variants"), TEXT("plan"), {TEXT("VariantManager"), TEXT("VariantManagerContent")}, {TEXT("VariantManagerContent")}, {}, {TEXT("Resolve LevelVariantSets target."), TEXT("Group variants by asset ownership lock."), TEXT("Capture actor/material properties."), TEXT("Require activation and screenshot receipt per variant.")}, {TEXT("level_variant_sets"), TEXT("variant_sets"), TEXT("captured_properties"), TEXT("activation_receipt"), TEXT("screenshot_receipt")}},
			{TEXT("variant_manager_asset_probe"), TEXT("Scan content for Variant Manager assets and candidate LevelVariantSets."), TEXT("variants"), TEXT("class_probe"), {TEXT("VariantManager"), TEXT("VariantManagerContent")}, {TEXT("VariantManagerContent")}, {TEXT("Variant"), TEXT("LevelVariantSets")}, {}, {}},
			{TEXT("live_link_subjects_probe"), TEXT("Probe current LiveLink subjects and available Python subject APIs for virtual-production routing."), TEXT("virtual_production"), TEXT("livelink_probe"), {TEXT("LiveLink"), TEXT("LiveLinkCamera"), TEXT("LiveLinkLens")}, {TEXT("LiveLink")}, {}, {}, {}},
			{TEXT("camera_calibration_lens_file_probe"), TEXT("Scan content for Camera Calibration LensFile and calibration-related assets."), TEXT("virtual_production"), TEXT("class_probe"), {TEXT("CameraCalibration"), TEXT("CameraCalibrationCore")}, {TEXT("CameraCalibrationEditor")}, {TEXT("LensFile"), TEXT("CameraCalibration"), TEXT("NodalOffset")}, {}, {}},
			{TEXT("datasmith_usd_import_plan"), TEXT("Plan Datasmith/USD scene ingest with hierarchy, material, and dependency receipts."), TEXT("asset_ingest"), TEXT("plan"), {TEXT("DatasmithImporter"), TEXT("DatasmithContent"), TEXT("USDImporter"), TEXT("USDCore")}, {TEXT("DatasmithImporter"), TEXT("USDStage")}, {}, {TEXT("Classify source file type and importer plugin."), TEXT("Choose import destination and overwrite policy."), TEXT("Preserve hierarchy/material dependencies when possible."), TEXT("Require dependency graph, material count, and preview receipt.")}, {TEXT("source_file"), TEXT("destination_path"), TEXT("import_options"), TEXT("dependency_graph"), TEXT("preview_receipt")}},
			{TEXT("usd_stage_asset_probe"), TEXT("Scan content for USD Stage and USD-related assets for scene ingest or reconstruction handoff."), TEXT("asset_ingest"), TEXT("class_probe"), {TEXT("USDImporter"), TEXT("USDCore")}, {TEXT("USDStage")}, {TEXT("Usd"), TEXT("USDStage"), TEXT("UsdStage")}, {}, {}},
			{TEXT("virtual_camera_session_plan"), TEXT("Plan virtual camera capture setup including LiveLink, lens calibration, Take Recorder, and Sequencer handoff."), TEXT("virtual_production"), TEXT("plan"), {TEXT("LiveLink"), TEXT("CameraCalibration"), TEXT("Takes")}, {TEXT("LiveLink")}, {}, {TEXT("Probe LiveLink subject freshness."), TEXT("Resolve lens file and camera actor."), TEXT("Bind Take Recorder source plan."), TEXT("Require recorded take and camera preview receipt.")}, {TEXT("live_link_subject"), TEXT("lens_file"), TEXT("camera_actor"), TEXT("take_asset"), TEXT("preview_receipt")}},
			{TEXT("vp_capture_health_probe"), TEXT("Probe virtual-production capture readiness across LiveLink subjects, take assets, Remote Control presets, and variants."), TEXT("virtual_production"), TEXT("vp_health_probe"), {TEXT("LiveLink"), TEXT("Takes"), TEXT("RemoteControl"), TEXT("VariantManagerContent")}, {TEXT("LiveLink")}, {}, {}, {}}
		};
	}
}

void RegisterVirtualProductionIngestTools(FSololmcpToolRegistry& Registry)
{
	for (const VirtualProductionIngestTools::FOptionalProductionSpec& Spec : VirtualProductionIngestTools::Specs())
	{
		VirtualProductionIngestTools::RegisterSpec(Registry, Spec);
	}
}
}
