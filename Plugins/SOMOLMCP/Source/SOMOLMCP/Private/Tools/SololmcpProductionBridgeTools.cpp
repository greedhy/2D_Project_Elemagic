// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpProductionBridgeTools.cpp
// ----------------------------------------------------------------------------
// UE 5.7+ optional production bridge tools for automation, functional tests,
// Motion Design/Avalanche, PCG-Niagara routing, and Editor Utility workflows.
// These are guarded probe/plan tools and avoid optional plugin C++ headers.
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
namespace ProductionBridgeTools
{
	struct FProductionBridgeSpec
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

	static void ProbeAvailability(const FProductionBridgeSpec& Spec, TSharedRef<FJsonObject>& Out, FString& OutStatus)
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
	}

	static TSharedRef<FJsonObject> GenericSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Target test, scene, graph, widget, utility, or mission id."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder to scan, default /Game."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Optional input assets."))},
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Optional report, render, or artifact output path."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum assets or test names to inspect."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan/probe only; no editor mutation."))}
		});
	}

	static void SetBaseReceipt(const FProductionBridgeSpec& Spec, TSharedRef<FJsonObject>& Out)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetBoolField(TEXT("read_only"), true);
		Out->SetStringField(TEXT("tool_name"), Spec.Name);
		Out->SetStringField(TEXT("domain"), Spec.Domain);
		Out->SetStringField(TEXT("operation_class"), TEXT("query"));
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
			"print('__SOMOLMCP_JSON__' + json.dumps({'tool': %s, 'folder_path': folder, 'class_needles': needles, 'items': items, 'count': len(items)}, default=str))\n"),
			*PyQuote(FolderPath),
			*PyList(ClassNeedles),
			MaxAssets,
			*PyQuote(ToolName));
	}

	static FString BuildAutomationCatalogPython(const int32 MaxAssets)
	{
		return FString::Printf(TEXT(
			"import unreal, json\n"
			"max_items = %d\n"
			"result = {'tool':'automation_test_catalog_probe','status':'attempted','tests':[],'api':'AutomationLibrary'}\n"
			"lib = unreal.AutomationLibrary if hasattr(unreal, 'AutomationLibrary') else None\n"
			"if not lib:\n"
			"    result['status'] = 'python_api_missing'\n"
			"else:\n"
			"    result['available_methods'] = [m for m in dir(lib) if 'test' in m.lower() or 'automation' in m.lower()]\n"
			"    for method in ['get_enabled_test_names', 'get_test_names', 'get_automation_test_names']:\n"
			"        if hasattr(lib, method):\n"
			"            try:\n"
			"                names = [str(x) for x in getattr(lib, method)()]\n"
			"                result['tests'] = names[:max_items] if max_items > 0 else names\n"
			"                result['method'] = method\n"
			"                result['status'] = 'completed_with_receipt'\n"
			"                break\n"
			"            except Exception as exc:\n"
			"                result.setdefault('method_errors', []).append({'method': method, 'error': str(exc)})\n"
			"print('__SOMOLMCP_JSON__' + json.dumps(result, default=str))\n"),
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
		const FProductionBridgeSpec& Spec,
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
		FString OutputPath;
		Arguments->TryGetStringField(TEXT("target"), Target);
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		Arguments->TryGetStringField(TEXT("output_path"), OutputPath);
		int32 MaxAssets = 200;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);

		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetStringField(TEXT("folder_path"), FolderPath);
		OutStructured->SetStringField(TEXT("output_path"), OutputPath);
		OutStructured->SetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetArrayField(TEXT("asset_paths"), StringArrayJson(GetStringArrayField(Arguments, TEXT("asset_paths"))));

		if (Spec.Mode == TEXT("plan"))
		{
			OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
			OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			OutSummary = FString::Printf(TEXT("%s returned a %s production bridge plan."), *Spec.Name, *Status);
			return true;
		}

		const FString Code = Spec.Mode == TEXT("automation_catalog")
			? BuildAutomationCatalogPython(MaxAssets)
			: BuildClassProbePython(Spec.Name, FolderPath, Spec.ClassNeedles, MaxAssets);
		const bool bOk = RunPythonWrapped(Context, Code, OutStructured, OutSummary, OutError);
		if (bOk)
		{
			OutSummary = FString::Printf(TEXT("%s completed %s."), *Spec.Name, *Spec.Mode);
		}
		return bOk;
	}

	static void RegisterSpec(FSololmcpToolRegistry& Registry, const FProductionBridgeSpec& Spec)
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

	static TArray<FProductionBridgeSpec> Specs()
	{
		return {
			{TEXT("automation_test_catalog_probe"), TEXT("Probe AutomationLibrary test catalog APIs for unattended QA routing."), TEXT("qa"), TEXT("automation_catalog"), {TEXT("PythonAutomationTest"), TEXT("FunctionalTestingEditor")}, {TEXT("PythonAutomationTest"), TEXT("FunctionalTestingEditor")}, {}, {}, {}},
			{TEXT("automation_test_run_plan"), TEXT("Plan unattended automation-test execution with report artifacts and failure routing."), TEXT("qa"), TEXT("plan"), {TEXT("PythonAutomationTest"), TEXT("FunctionalTestingEditor")}, {TEXT("FunctionalTestingEditor")}, {}, {TEXT("Resolve test names or filter."), TEXT("Create non-PIE/editor-safe execution lane."), TEXT("Capture log/report output path."), TEXT("Route failures to QA/Hermes before delivery." )}, {TEXT("test_filter"), TEXT("execution_lane"), TEXT("report_path"), TEXT("failure_receipt")}},
			{TEXT("functional_test_asset_probe"), TEXT("Scan content for Functional Test assets and test-map candidates."), TEXT("qa"), TEXT("class_probe"), {TEXT("FunctionalTestingEditor")}, {TEXT("FunctionalTestingEditor")}, {TEXT("FunctionalTest"), TEXT("AutomationTest"), TEXT("Test")}, {}, {}},
			{TEXT("gauntlet_perf_session_plan"), TEXT("Plan Gauntlet or automated performance sessions for long-running generated scenes."), TEXT("qa"), TEXT("plan"), {TEXT("Gauntlet"), TEXT("AutomatedPerfTesting")}, {TEXT("Gauntlet")}, {}, {TEXT("Resolve test controller and map."), TEXT("Define warmup/runtime/sample windows."), TEXT("Capture FPS, hitch, memory, and log artifacts."), TEXT("Block release on budget violations.")}, {TEXT("controller"), TEXT("map"), TEXT("perf_budget"), TEXT("report_path"), TEXT("budget_receipt")}},
			{TEXT("widget_automation_test_plan"), TEXT("Plan widget/UI automation coverage for UMG/CommonUI generated interfaces."), TEXT("umg"), TEXT("plan"), {TEXT("WidgetAutomationTests"), TEXT("CommonUI")}, {TEXT("WidgetAutomationTests")}, {}, {TEXT("Resolve widget target and interaction script."), TEXT("Bind screenshots and focus/input assertions."), TEXT("Run through automation or preview harness."), TEXT("Require visual and interaction receipts.")}, {TEXT("widget_path"), TEXT("interaction_steps"), TEXT("screenshot_receipt"), TEXT("focus_receipt")}},
			{TEXT("motion_design_scene_plan"), TEXT("Plan Motion Design/Avalanche scene creation for broadcast graphics and generated UI scenes."), TEXT("motion_design"), TEXT("plan"), {TEXT("Avalanche")}, {TEXT("Avalanche")}, {}, {TEXT("Resolve scene target, text/shape/media inputs, and sequence handoff."), TEXT("Plan Avalanche actors/material/style bindings."), TEXT("Bind Remote Control or Sequencer if needed."), TEXT("Require MRQ/viewport proof render and property readback.")}, {TEXT("scene_target"), TEXT("actor_plan"), TEXT("style_bindings"), TEXT("sequence_handoff"), TEXT("render_receipt")}},
			{TEXT("avalanche_asset_probe"), TEXT("Scan content for Avalanche/Motion Design assets, scenes, tags, and transitions."), TEXT("motion_design"), TEXT("class_probe"), {TEXT("Avalanche")}, {TEXT("Avalanche")}, {TEXT("Avalanche"), TEXT("MotionDesign"), TEXT("Motion"), TEXT("Transition")}, {}, {}},
			{TEXT("avalanche_remote_control_bridge_plan"), TEXT("Plan Avalanche and Remote Control bridge wiring for live adjustable generated broadcast scenes."), TEXT("motion_design"), TEXT("plan"), {TEXT("Avalanche"), TEXT("RemoteControl")}, {TEXT("AvalancheRemoteControl"), TEXT("RemoteControl")}, {}, {TEXT("Resolve Avalanche scene and remote-control preset."), TEXT("Expose only whitelisted scene/material/text controls."), TEXT("Define live-preview update cadence and lock ownership."), TEXT("Require property readback and preview receipt.")}, {TEXT("avalanche_scene"), TEXT("remote_control_preset"), TEXT("exposed_controls"), TEXT("readback_receipt")}},
			{TEXT("pcg_niagara_interop_plan"), TEXT("Plan PCG-to-Niagara interop for generated VFX driven by PCG points or attributes."), TEXT("pcg_vfx"), TEXT("plan"), {TEXT("PCG"), TEXT("Niagara"), TEXT("PCGNiagaraInterop")}, {TEXT("PCG"), TEXT("Niagara")}, {}, {TEXT("Validate PCG graph and generated attributes."), TEXT("Map attributes to Niagara user parameters or data interfaces."), TEXT("Apply tile-cap and dry-run budget guards."), TEXT("Require PCG generation and Niagara preview receipts.")}, {TEXT("pcg_graph"), TEXT("niagara_system"), TEXT("attribute_map"), TEXT("dry_run_receipt"), TEXT("preview_receipt")}},
			{TEXT("pcg_niagara_attribute_bridge_plan"), TEXT("Plan attribute schema mapping between PCG generated data and Niagara emitters."), TEXT("pcg_vfx"), TEXT("plan"), {TEXT("PCG"), TEXT("Niagara"), TEXT("PCGNiagaraInterop")}, {TEXT("PCG"), TEXT("Niagara")}, {}, {TEXT("Inspect PCG output attributes."), TEXT("Inspect Niagara parameters and expected types."), TEXT("Build coercion/default/fallback map."), TEXT("Block execution if required attributes are missing.")}, {TEXT("pcg_attributes"), TEXT("niagara_parameters"), TEXT("type_map"), TEXT("missing_attribute_policy")}},
			{TEXT("editor_utility_widget_asset_probe"), TEXT("Scan content for Editor Utility Widget/Blueprint assets that can support batch authoring lanes."), TEXT("editor_automation"), TEXT("class_probe"), {TEXT("Blutility"), TEXT("EditorScriptingUtilities")}, {TEXT("Blutility"), TEXT("EditorScriptingUtilities")}, {TEXT("EditorUtility"), TEXT("Blutility"), TEXT("UtilityWidget")}, {}, {}},
			{TEXT("editor_utility_batch_action_plan"), TEXT("Plan Editor Utility based batch actions with target guards, locks, and rollback receipts."), TEXT("editor_automation"), TEXT("plan"), {TEXT("Blutility"), TEXT("EditorScriptingUtilities")}, {TEXT("Blutility"), TEXT("EditorScriptingUtilities")}, {}, {TEXT("Resolve utility asset and callable entry point."), TEXT("Bind target assets/project guard and resource locks."), TEXT("Require dry-run, snapshot, and rollback plan."), TEXT("Run only through guarded editor-write lane.")}, {TEXT("utility_asset"), TEXT("target_assets"), TEXT("dry_run_receipt"), TEXT("rollback_plan"), TEXT("post_readback")}}
		};
	}
}

void RegisterProductionBridgeTools(FSololmcpToolRegistry& Registry)
{
	for (const ProductionBridgeTools::FProductionBridgeSpec& Spec : ProductionBridgeTools::Specs())
	{
		ProductionBridgeTools::RegisterSpec(Registry, Spec);
	}
}
}
