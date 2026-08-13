// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpValidationRenderTools.cpp
// ----------------------------------------------------------------------------
// UE 5.7+ validation, render-queue, and take-recorder production helpers.
// Optional plugin APIs are reached through guarded Python/reflection paths so
// this file stays compile-safe across UE 5.7 and UE 5.8.
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
namespace ValidationRenderTools
{
	struct FToolSpec
	{
		FString Name;
		FString Description;
		FString Domain;
		FString OperationClass;
		TArray<FString> Plugins;
		TArray<FString> Modules;
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

	static bool ProbeAvailability(const FToolSpec& Spec, TSharedRef<FJsonObject>& Out, FString& OutStatus)
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

		Out->SetArrayField(TEXT("plugins"), PluginsJson);
		Out->SetArrayField(TEXT("modules"), ModulesJson);
		Out->SetBoolField(TEXT("available"), OutStatus == TEXT("available"));
		Out->SetStringField(TEXT("status"), OutStatus);
		return OutStatus == TEXT("available");
	}

	static void SetBaseReceipt(const FToolSpec& Spec, TSharedRef<FJsonObject>& Out)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetBoolField(TEXT("read_only"), true);
		Out->SetStringField(TEXT("tool_name"), Spec.Name);
		Out->SetStringField(TEXT("domain"), Spec.Domain);
		Out->SetStringField(TEXT("operation_class"), Spec.OperationClass);
		Out->SetStringField(TEXT("safety_class"), TEXT("read_only"));
	}

	static TSharedRef<FJsonObject> AssetListSchema(const FString& Description)
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Asset paths to process."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder such as /Game/Maps or /Game/Characters."))},
			{TEXT("recursive"), FSololmcpSchemaBuilder::Boolean(TEXT("Include subfolders when folder_path is used."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum assets to inspect or validate."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Preview the target set and gates without calling the optional plugin API."))}
		}, {}, Description);
	}

	static FString BuildAssetValidationPython(const TArray<FString>& AssetPaths, const bool bDryRun)
	{
		return FString::Printf(TEXT(
			"import unreal, json\n"
			"paths = %s\n"
			"dry_run = %s\n"
			"result = {'tool':'data_validation_run_assets','status':'dry_run' if dry_run else 'attempted','asset_paths':paths,'missing':[],'validated':[],'api':'EditorValidatorSubsystem'}\n"
			"assets = []\n"
			"for p in paths:\n"
			"    a = unreal.EditorAssetLibrary.load_asset(p) if hasattr(unreal, 'EditorAssetLibrary') else None\n"
			"    if a:\n"
			"        assets.append(a)\n"
			"    else:\n"
			"        result['missing'].append(p)\n"
			"if dry_run:\n"
			"    result['loaded_count'] = len(assets)\n"
			"else:\n"
			"    subsystem = unreal.get_editor_subsystem(unreal.EditorValidatorSubsystem) if hasattr(unreal, 'EditorValidatorSubsystem') else None\n"
			"    if not subsystem:\n"
			"        result['status'] = 'python_api_missing'\n"
			"    else:\n"
			"        methods = [m for m in dir(subsystem) if 'valid' in m.lower()]\n"
			"        result['available_methods'] = methods\n"
			"        for a in assets:\n"
			"            item = {'path': a.get_path_name(), 'class': a.get_class().get_name()}\n"
			"            try:\n"
			"                if hasattr(subsystem, 'validate_loaded_asset'):\n"
			"                    item['result'] = str(subsystem.validate_loaded_asset(a))\n"
			"                elif hasattr(subsystem, 'validate_assets'):\n"
			"                    item['result'] = str(subsystem.validate_assets([a]))\n"
			"                else:\n"
			"                    item['result'] = 'no_validate_method'\n"
			"            except Exception as exc:\n"
			"                item['error'] = str(exc)\n"
			"            result['validated'].append(item)\n"
			"        result['status'] = 'completed_with_receipt'\n"
			"print('__SOMOLMCP_JSON__' + json.dumps(result, default=str))\n"),
			*PyList(AssetPaths),
			bDryRun ? TEXT("True") : TEXT("False"));
	}

	static FString BuildFolderValidationPython(const FString& FolderPath, const bool bRecursive, const int32 MaxAssets, const bool bDryRun)
	{
		return FString::Printf(TEXT(
			"import unreal, json\n"
			"folder = %s\n"
			"recursive = %s\n"
			"max_assets = %d\n"
			"dry_run = %s\n"
			"paths = []\n"
			"missing = []\n"
			"validated = []\n"
			"if hasattr(unreal, 'EditorAssetLibrary'):\n"
			"    paths = list(unreal.EditorAssetLibrary.list_assets(folder, recursive, False))\n"
			"if max_assets > 0:\n"
			"    paths = paths[:max_assets]\n"
			"assets = []\n"
			"for p in paths:\n"
			"    a = unreal.EditorAssetLibrary.load_asset(p) if hasattr(unreal, 'EditorAssetLibrary') else None\n"
			"    if a:\n"
			"        assets.append(a)\n"
			"    else:\n"
			"        missing.append(p)\n"
			"status = 'dry_run' if dry_run else 'attempted'\n"
			"if not dry_run:\n"
			"    subsystem = unreal.get_editor_subsystem(unreal.EditorValidatorSubsystem) if hasattr(unreal, 'EditorValidatorSubsystem') else None\n"
			"    if not subsystem:\n"
			"        status = 'python_api_missing'\n"
			"    else:\n"
			"        for a in assets:\n"
			"            item = {'path': a.get_path_name(), 'class': a.get_class().get_name()}\n"
			"            try:\n"
			"                if hasattr(subsystem, 'validate_loaded_asset'):\n"
			"                    item['result'] = str(subsystem.validate_loaded_asset(a))\n"
			"                elif hasattr(subsystem, 'validate_assets'):\n"
			"                    item['result'] = str(subsystem.validate_assets([a]))\n"
			"                else:\n"
			"                    item['result'] = 'no_validate_method'\n"
			"            except Exception as exc:\n"
			"                item['error'] = str(exc)\n"
			"            validated.append(item)\n"
			"        status = 'completed_with_receipt'\n"
			"print('__SOMOLMCP_JSON__' + json.dumps({'tool':'data_validation_run_folder','status':status,'folder_path': folder, 'recursive': recursive, 'max_assets': max_assets, 'asset_paths': paths, 'asset_count': len(paths), 'missing': missing, 'validated': validated}, default=str))\n"),
			*PyQuote(FolderPath),
			bRecursive ? TEXT("True") : TEXT("False"),
			MaxAssets,
			bDryRun ? TEXT("True") : TEXT("False"));
	}

	static FString BuildClassProbePython(const FString& ToolName, const FString& FolderPath, const TArray<FString>& ClassNeedles, const int32 MaxAssets)
	{
		return FString::Printf(TEXT(
			"import unreal, json\n"
			"folder = %s\n"
			"needles = [n.lower() for n in %s]\n"
			"max_assets = %d\n"
			"items = []\n"
			"paths = []\n"
			"if hasattr(unreal, 'EditorAssetLibrary'):\n"
			"    paths = list(unreal.EditorAssetLibrary.list_assets(folder or '/Game', True, False))\n"
			"for p in paths:\n"
			"    if max_assets > 0 and len(items) >= max_assets:\n"
			"        break\n"
			"    try:\n"
			"        a = unreal.EditorAssetLibrary.load_asset(p)\n"
			"        if not a:\n"
			"            continue\n"
			"        cls = a.get_class().get_name()\n"
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

	static bool RunDataValidationAssets(
		const FToolSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		SetBaseReceipt(Spec, OutStructured);
		FString Status;
		const bool bAvailable = ProbeAvailability(Spec, OutStructured, Status);
		const TArray<FString> AssetPaths = GetStringArrayField(Arguments, TEXT("asset_paths"));
		bool bDryRun = false;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetArrayField(TEXT("asset_paths"), StringArrayJson(AssetPaths));
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		if (!bAvailable)
		{
			OutStructured->SetBoolField(TEXT("blocked"), true);
			OutSummary = FString::Printf(TEXT("%s blocked: %s."), *Spec.Name, *Status);
			return true;
		}
		if (AssetPaths.IsEmpty())
		{
			OutStructured->SetBoolField(TEXT("blocked"), true);
			OutStructured->SetStringField(TEXT("blocked_reason"), TEXT("asset_paths is empty"));
			OutSummary = TEXT("No assets were provided for validation.");
			return true;
		}
		const bool bOk = RunPythonWrapped(Context, BuildAssetValidationPython(AssetPaths, bDryRun), OutStructured, OutSummary, OutError);
		if (bOk)
		{
			OutSummary = FString::Printf(TEXT("%s produced a validation receipt for %d asset(s)."), *Spec.Name, AssetPaths.Num());
		}
		return bOk;
	}

	static bool RunDataValidationFolder(
		const FToolSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		SetBaseReceipt(Spec, OutStructured);
		FString Status;
		const bool bAvailable = ProbeAvailability(Spec, OutStructured, Status);
		FString FolderPath = TEXT("/Game");
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		bool bRecursive = true;
		Arguments->TryGetBoolField(TEXT("recursive"), bRecursive);
		int32 MaxAssets = 200;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		bool bDryRun = false;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetStringField(TEXT("folder_path"), FolderPath);
		OutStructured->SetBoolField(TEXT("recursive"), bRecursive);
		OutStructured->SetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		if (!bAvailable)
		{
			OutStructured->SetBoolField(TEXT("blocked"), true);
			OutSummary = FString::Printf(TEXT("%s blocked: %s."), *Spec.Name, *Status);
			return true;
		}

		const bool bOk = RunPythonWrapped(Context, BuildFolderValidationPython(FolderPath, bRecursive, MaxAssets, bDryRun), OutStructured, OutSummary, OutError);
		if (bOk)
		{
			OutSummary = FString::Printf(TEXT("%s produced a folder validation receipt for %s."), *Spec.Name, *FolderPath);
		}
		return bOk;
	}

	static bool RunPlanTool(
		const FToolSpec& Spec,
		const TSharedRef<FJsonObject>& Arguments,
		const TArray<FString>& PlanSteps,
		const TArray<FString>& ReceiptRequirements,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary)
	{
		SetBaseReceipt(Spec, OutStructured);
		FString Status;
		ProbeAvailability(Spec, OutStructured, Status);
		FString Target;
		Arguments->TryGetStringField(TEXT("target"), Target);
		FString OutputPath;
		Arguments->TryGetStringField(TEXT("output_path"), OutputPath);
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		int32 MaxAssets = 0;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetStringField(TEXT("output_path"), OutputPath);
		OutStructured->SetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetArrayField(TEXT("asset_paths"), StringArrayJson(GetStringArrayField(Arguments, TEXT("asset_paths"))));
		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(ReceiptRequirements));
		OutSummary = FString::Printf(TEXT("%s returned a %s plan."), *Spec.Name, *Status);
		return true;
	}

	static bool RunClassProbeTool(
		const FToolSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		const TArray<FString>& ClassNeedles,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		SetBaseReceipt(Spec, OutStructured);
		FString Status;
		ProbeAvailability(Spec, OutStructured, Status);
		FString FolderPath = TEXT("/Game");
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		int32 MaxAssets = 200;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetStringField(TEXT("folder_path"), FolderPath);
		OutStructured->SetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetArrayField(TEXT("class_needles"), StringArrayJson(ClassNeedles));
		const bool bOk = RunPythonWrapped(Context, BuildClassProbePython(Spec.Name, FolderPath, ClassNeedles, MaxAssets), OutStructured, OutSummary, OutError);
		if (bOk)
		{
			OutSummary = FString::Printf(TEXT("%s scanned %s for matching assets."), *Spec.Name, *FolderPath);
		}
		return bOk;
	}
}

void RegisterValidationRenderTools(FSololmcpToolRegistry& Registry)
{
	using namespace ValidationRenderTools;

	const FToolSpec DataValidationAssets{
		TEXT("data_validation_run_assets"),
		TEXT("Run or dry-run UE Data Validation over explicit asset paths and return a structured QA receipt."),
		TEXT("qa"),
		TEXT("execute"),
		{TEXT("DataValidation")},
		{TEXT("DataValidation")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = DataValidationAssets.Name;
		Def.Description = DataValidationAssets.Description;
		Def.InputSchema = AssetListSchema(TEXT("Validate explicit assets with UE Data Validation when available."));
		Def.Execute = [DataValidationAssets](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return RunDataValidationAssets(DataValidationAssets, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	const FToolSpec DataValidationFolder{
		TEXT("data_validation_run_folder"),
		TEXT("Enumerate a content folder for UE Data Validation and emit a chunkable validation receipt."),
		TEXT("qa"),
		TEXT("execute"),
		{TEXT("DataValidation")},
		{TEXT("DataValidation")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = DataValidationFolder.Name;
		Def.Description = DataValidationFolder.Description;
		Def.InputSchema = AssetListSchema(TEXT("Validate or enumerate a folder for Data Validation chunking."));
		Def.Execute = [DataValidationFolder](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return RunDataValidationFolder(DataValidationFolder, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	const FToolSpec DataValidationPlan{
		TEXT("data_validation_asset_status_plan"),
		TEXT("Plan a fail-closed asset validation gate for delivery manifests before save/build handoff."),
		TEXT("qa"),
		TEXT("query"),
		{TEXT("DataValidation")},
		{TEXT("DataValidation")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = DataValidationPlan.Name;
		Def.Description = DataValidationPlan.Description;
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Assets that must have validation status evidence."))},
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Delivery, manifest, or QA target name."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Always true for this planning tool."))}
		});
		Def.CacheTtlSeconds = 15;
		Def.Execute = [DataValidationPlan](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return RunPlanTool(
				DataValidationPlan,
				Arguments,
				{TEXT("Resolve asset manifest paths."), TEXT("Chunk assets by package and editor-write lock."), TEXT("Run data_validation_run_assets for each chunk."), TEXT("Block delivery if any asset has missing, invalid, or unparsed status.")},
				{TEXT("target_asset_paths"), TEXT("validation_result_per_asset"), TEXT("missing_assets"), TEXT("blocked_or_passed_status")},
				OutStructured,
				OutSummary);
		};
		Registry.Register(Def);
	}

	const FToolSpec MrqPlan{
		TEXT("movie_render_queue_job_plan"),
		TEXT("Plan a Movie Render Queue job for cinematic preview, visual QA, or delivery render evidence."),
		TEXT("cinematic"),
		TEXT("query"),
		{TEXT("MovieRenderPipeline")},
		{TEXT("MovieRenderPipelineCore")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = MrqPlan.Name;
		Def.Description = MrqPlan.Description;
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Level sequence or render target asset."))},
			{TEXT("preset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Movie Pipeline preset asset."))},
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Render output directory."))},
			{TEXT("quality_gate"), FSololmcpSchemaBuilder::String(TEXT("preview, final, visual_qa, or keyframe_compare."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan only."))}
		});
		Def.CacheTtlSeconds = 15;
		Def.Execute = [MrqPlan](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return RunPlanTool(
				MrqPlan,
				Arguments,
				{TEXT("Validate Movie Render Queue plugin and target sequence."), TEXT("Resolve preset or default preview settings."), TEXT("Bind output path and frame range."), TEXT("Schedule render lane lock and visual QA receipt.")},
				{TEXT("sequence_path"), TEXT("preset_path"), TEXT("output_path"), TEXT("frame_range"), TEXT("render_receipt"), TEXT("visual_qa_receipt")},
				OutStructured,
				OutSummary);
		};
		Registry.Register(Def);
	}

	const FToolSpec MrqProbe{
		TEXT("movie_render_queue_preset_probe"),
		TEXT("Scan content assets for Movie Render Pipeline queue/config presets useful for render planning."),
		TEXT("cinematic"),
		TEXT("query"),
		{TEXT("MovieRenderPipeline")},
		{TEXT("MovieRenderPipelineCore")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = MrqProbe.Name;
		Def.Description = MrqProbe.Description;
		Def.InputSchema = AssetListSchema(TEXT("Scan for MRQ queue/config assets."));
		Def.CacheTtlSeconds = 15;
		Def.Execute = [MrqProbe](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return RunClassProbeTool(MrqProbe, Context, Arguments, {TEXT("MoviePipeline"), TEXT("MovieRender"), TEXT("RenderQueue")}, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	const FToolSpec TakePlan{
		TEXT("take_recorder_source_plan"),
		TEXT("Plan Take Recorder source bindings for actors, LiveLink subjects, cameras, and gameplay capture."),
		TEXT("cinematic"),
		TEXT("query"),
		{TEXT("Takes")},
		{TEXT("TakeRecorder")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = TakePlan.Name;
		Def.Description = TakePlan.Description;
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Take preset, sequence, or recording session target."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor, sequence, or source asset references."))},
			{TEXT("quality_gate"), FSololmcpSchemaBuilder::String(TEXT("recording, replay, camera, mocap, or audio_sync."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan only."))}
		});
		Def.CacheTtlSeconds = 15;
		Def.Execute = [TakePlan](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return RunPlanTool(
				TakePlan,
				Arguments,
				{TEXT("Probe Take Recorder availability."), TEXT("Resolve source actors, LiveLink subjects, and camera bindings."), TEXT("Create recording lane lock and target take naming plan."), TEXT("Require generated LevelSequence and playback receipt.")},
				{TEXT("source_bindings"), TEXT("take_name"), TEXT("timecode_or_frame_range"), TEXT("recording_asset"), TEXT("playback_receipt")},
				OutStructured,
				OutSummary);
		};
		Registry.Register(Def);
	}

	const FToolSpec TakeProbe{
		TEXT("take_recorder_preset_probe"),
		TEXT("Scan content assets for Take Recorder presets and take-related LevelSequence assets."),
		TEXT("cinematic"),
		TEXT("query"),
		{TEXT("Takes")},
		{TEXT("TakeRecorder")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = TakeProbe.Name;
		Def.Description = TakeProbe.Description;
		Def.InputSchema = AssetListSchema(TEXT("Scan for Take Recorder presets and take assets."));
		Def.CacheTtlSeconds = 15;
		Def.Execute = [TakeProbe](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return RunClassProbeTool(TakeProbe, Context, Arguments, {TEXT("TakePreset"), TEXT("TakeRecorder"), TEXT("LevelSequence")}, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	const FToolSpec OutputGuard{
		TEXT("render_queue_output_guard_plan"),
		TEXT("Plan output-path, overwrite, disk-budget, and visual-QA guards before unattended render queue execution."),
		TEXT("cinematic"),
		TEXT("query"),
		{TEXT("MovieRenderPipeline")},
		{TEXT("MovieRenderPipelineCore")}
	};
	{
		FSololmcpToolDefinition Def;
		Def.Name = OutputGuard.Name;
		Def.Description = OutputGuard.Description;
		Def.InputSchema = FSololmcpSchemaBuilder::Object({
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Render job, sequence, or mission id."))},
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Output path to guard."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum expected output frames/files."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan only."))}
		});
		Def.CacheTtlSeconds = 15;
		Def.Execute = [OutputGuard](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			return RunPlanTool(
				OutputGuard,
				Arguments,
				{TEXT("Resolve output directory and naming pattern."), TEXT("Block unsafe overwrite by default."), TEXT("Estimate frame/file count and disk budget."), TEXT("Attach render receipt and screenshot/keyframe QA requirements.")},
				{TEXT("output_path"), TEXT("overwrite_policy"), TEXT("disk_budget"), TEXT("expected_files"), TEXT("render_receipt"), TEXT("visual_qa_receipt")},
				OutStructured,
				OutSummary);
		};
		Registry.Register(Def);
	}
}
}
