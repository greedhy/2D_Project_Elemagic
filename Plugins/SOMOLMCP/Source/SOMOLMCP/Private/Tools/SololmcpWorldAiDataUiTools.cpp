// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpWorldAiDataUiTools.cpp
// ----------------------------------------------------------------------------
// UE 5.7+ optional-domain probes and production plans for Chooser, PoseSearch,
// ZoneGraph, Mass, CommonUI, DataRegistry, and Enhanced Input. Optional plugin
// APIs are reached through guarded Python/reflection paths only.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
// AN-16 (2026-08-05): real PoseSearch database readback uses the Asset Registry
// plus reflection only; no compile-time dependency on the PoseSearch module.
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/TopLevelAssetPath.h"

namespace UE::SOMOLMCP
{
namespace WorldAiDataUiTools
{
	struct FWorldAiDataUiSpec
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

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static bool IsUE58OrLater()
	{
		const FEngineVersion Current = FEngineVersion::Current();
		if (Current.GetMajor() != 5)
		{
			return Current.GetMajor() > 5;
		}
		return Current.GetMinor() >= 8;
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

	static bool ProbeAvailability(const FWorldAiDataUiSpec& Spec, TSharedRef<FJsonObject>& Out, FString& OutStatus)
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
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Target asset, role, actor, widget, registry, or gameplay feature."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder to scan, default /Game."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Optional source/dependency asset paths."))},
			{TEXT("context"), FSololmcpSchemaBuilder::String(TEXT("Optional planner context such as locomotion, crowd, menu, or input mode."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum assets to inspect."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan/probe only; no editor mutation."))}
		});
	}

	static void SetBaseReceipt(const FWorldAiDataUiSpec& Spec, TSharedRef<FJsonObject>& Out)
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
			"            item = {'path': p, 'class': cls}\n"
			"            if hasattr(asset, 'get_editor_property'):\n"
			"                try:\n"
			"                    item['name'] = asset.get_name()\n"
			"                except Exception:\n"
			"                    pass\n"
			"            items.append(item)\n"
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

	static bool ExecuteSpec(
		const FWorldAiDataUiSpec& Spec,
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
		FString PlannerContext;
		Arguments->TryGetStringField(TEXT("target"), Target);
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		Arguments->TryGetStringField(TEXT("context"), PlannerContext);
		int32 MaxAssets = 200;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);

		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetStringField(TEXT("folder_path"), FolderPath);
		OutStructured->SetStringField(TEXT("context"), PlannerContext);
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

		const bool bOk = RunPythonWrapped(Context, BuildClassProbePython(Spec.Name, FolderPath, Spec.ClassNeedles, MaxAssets), OutStructured, OutSummary, OutError);
		if (bOk)
		{
			OutSummary = FString::Printf(TEXT("%s scanned %s for candidate assets."), *Spec.Name, *FolderPath);
		}
		return bOk;
	}

	static void RegisterSpec(FSololmcpToolRegistry& Registry, const FWorldAiDataUiSpec& Spec)
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

	struct FCommonUiP1Spec
	{
		FString Name;
		FString Description;
		FString Mode;
		bool bMutation = false;
		TArray<FString> FocusClasses;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
	};

	static TSharedRef<FJsonObject> CommonUiSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target CommonUI/UMG asset path."))},
			{TEXT("widget_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for target widget asset path."))},
			{TEXT("style_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for target style asset path."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder for class/style scans. Default /Game."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Compile/preview/input routing receipt to validate."))},
			{TEXT("require_compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Require compile evidence for receipt validation. Default true for production receipt tools."))},
			{TEXT("require_preview"), FSololmcpSchemaBuilder::Boolean(TEXT("Require preview/screenshot evidence for receipt validation. Default true for preview receipt."))},
			{TEXT("require_input_routing"), FSololmcpSchemaBuilder::Boolean(TEXT("Require input routing evidence for receipt validation. Default false."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation. P1 CommonUI authoring tools fail closed unless promoted to a dedicated writer."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return probe/plan only. Default true."))}
		});
	}

	static FString CommonUiTargetPath(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Path;
		if (Arguments->TryGetStringField(TEXT("target_asset"), Path) && !Path.IsEmpty())
		{
			return Path;
		}
		if (Arguments->TryGetStringField(TEXT("widget_path"), Path) && !Path.IsEmpty())
		{
			return Path;
		}
		if (Arguments->TryGetStringField(TEXT("style_path"), Path) && !Path.IsEmpty())
		{
			return Path;
		}
		Arguments->TryGetStringField(TEXT("target"), Path);
		return Path;
	}

	static TArray<TPair<FString, FString>> CommonUiClassCatalog()
	{
		return {
			{TEXT("CommonUserWidget"), TEXT("/Script/CommonUI.CommonUserWidget")},
			{TEXT("CommonActivatableWidget"), TEXT("/Script/CommonUI.CommonActivatableWidget")},
			{TEXT("CommonButtonBase"), TEXT("/Script/CommonUI.CommonButtonBase")},
			{TEXT("CommonBoundActionButton"), TEXT("/Script/CommonUI.CommonBoundActionButton")},
			{TEXT("CommonTextBlock"), TEXT("/Script/CommonUI.CommonTextBlock")},
			{TEXT("CommonRichTextBlock"), TEXT("/Script/CommonUI.CommonRichTextBlock")},
			{TEXT("CommonBorder"), TEXT("/Script/CommonUI.CommonBorder")},
			{TEXT("CommonTabListWidgetBase"), TEXT("/Script/CommonUI.CommonTabListWidgetBase")},
			{TEXT("CommonWidgetCarousel"), TEXT("/Script/CommonUI.CommonWidgetCarousel")},
			{TEXT("CommonVideoPlayer"), TEXT("/Script/CommonUI.CommonVideoPlayer")},
			{TEXT("CommonInputActionDataBase"), TEXT("/Script/CommonUI.CommonInputActionDataBase")},
			{TEXT("CommonButtonStyle"), TEXT("/Script/CommonUI.CommonButtonStyle")},
			{TEXT("CommonTextStyle"), TEXT("/Script/CommonUI.CommonTextStyle")},
			{TEXT("CommonBorderStyle"), TEXT("/Script/CommonUI.CommonBorderStyle")}
		};
	}

	static TSharedRef<FJsonObject> CommonUiClassStatusJson(const FSololmcpToolExecutionContext& Context, const FString& Id, const FString& ClassPath)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		FString ResolveError;
		UClass* Class = Context.Services.ResolveClass(ClassPath, ResolveError);
		Obj->SetStringField(TEXT("id"), Id);
		Obj->SetStringField(TEXT("classPath"), ClassPath);
		Obj->SetBoolField(TEXT("available"), Class != nullptr);
		if (Class)
		{
			Obj->SetStringField(TEXT("resolvedClass"), Class->GetPathName());
			Obj->SetStringField(TEXT("superClass"), Class->GetSuperClass() ? Class->GetSuperClass()->GetPathName() : FString());
			int32 PropertyCount = 0;
			for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				++PropertyCount;
			}
			int32 FunctionCount = 0;
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				++FunctionCount;
			}
			Obj->SetNumberField(TEXT("propertyCount"), PropertyCount);
			Obj->SetNumberField(TEXT("functionCount"), FunctionCount);
		}
		else if (!ResolveError.IsEmpty())
		{
			Obj->SetStringField(TEXT("resolveError"), ResolveError);
		}
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> CommonUiClassCatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusClasses = {})
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TPair<FString, FString>& Row : CommonUiClassCatalog())
		{
			if (!FocusClasses.IsEmpty() && !FocusClasses.Contains(Row.Key))
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(CommonUiClassStatusJson(Context, Row.Key, Row.Value)));
		}
		return Rows;
	}

	static TSharedRef<FJsonObject> CommonUiAssetSummaryJson(const FSololmcpToolExecutionContext& Context, const FString& AssetPath)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("requestedPath"), AssetPath);
		if (AssetPath.IsEmpty())
		{
			Obj->SetStringField(TEXT("status"), TEXT("not_requested"));
			return Obj;
		}
		FString LoadError;
		UObject* Asset = Context.Services.LoadAsset(AssetPath, LoadError);
		Obj->SetBoolField(TEXT("loaded"), Asset != nullptr);
		if (!Asset)
		{
			Obj->SetStringField(TEXT("status"), TEXT("asset_not_found"));
			if (!LoadError.IsEmpty())
			{
				Obj->SetStringField(TEXT("error"), LoadError);
			}
			return Obj;
		}
		Obj->SetStringField(TEXT("status"), TEXT("loaded"));
		Obj->SetStringField(TEXT("name"), Asset->GetName());
		Obj->SetStringField(TEXT("path"), Asset->GetPathName());
		Obj->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("package"), Asset->GetPackage() ? Asset->GetPackage()->GetName() : FString());
		Obj->SetBoolField(TEXT("packageDirty"), Asset->GetPackage() && Asset->GetPackage()->IsDirty());
		TArray<TSharedPtr<FJsonValue>> Properties;
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It && Count < 80; ++It, ++Count)
		{
			TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("name"), It->GetName());
			Property->SetStringField(TEXT("class"), It->GetClass()->GetName());
			Property->SetBoolField(TEXT("editable"), It->HasAnyPropertyFlags(CPF_Edit));
			Properties.Add(MakeShared<FJsonValueObject>(Property));
		}
		Obj->SetArrayField(TEXT("properties"), Properties);
		Obj->SetNumberField(TEXT("propertySampleCount"), Properties.Num());
		return Obj;
	}

	static void AddCommonUiCheck(TArray<TSharedPtr<FJsonValue>>& Checks, bool& bValid, const FString& Name, const bool bPass, const FString& Detail)
	{
		TSharedRef<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("pass"), bPass);
		Check->SetStringField(TEXT("detail"), Detail);
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		bValid &= bPass;
	}

	static bool CommonUiReceiptHasAny(const TSharedRef<FJsonObject>& Receipt, std::initializer_list<const TCHAR*> Fields)
	{
		for (const TCHAR* Field : Fields)
		{
			if (Receipt->HasField(Field))
			{
				FString StringValue;
				if (Receipt->TryGetStringField(Field, StringValue))
				{
					if (!StringValue.IsEmpty())
					{
						return true;
					}
					continue;
				}
				return true;
			}
		}
		return false;
	}

	static bool CommonUiReceiptBool(const TSharedRef<FJsonObject>& Receipt, const TCHAR* Field)
	{
		bool bValue = false;
		return Receipt->TryGetBoolField(Field, bValue) && bValue;
	}

	static bool ExecuteCommonUiReceiptTool(
		const FCommonUiP1Spec& Spec,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_missing_receipt"));
			OutError = TEXT("CommonUI receipt validation requires a receipt object.");
			return false;
		}
		const TSharedRef<FJsonObject> Receipt = ReceiptPtr->ToSharedRef();
		bool bRequireCompile = Spec.Name == TEXT("commonui_receipt_validate") || Spec.Name == TEXT("commonui_widget_compile_validate");
		bool bRequirePreview = Spec.Name == TEXT("commonui_preview_receipt");
		bool bRequireInput = false;
		Arguments->TryGetBoolField(TEXT("require_compile"), bRequireCompile);
		Arguments->TryGetBoolField(TEXT("require_preview"), bRequirePreview);
		Arguments->TryGetBoolField(TEXT("require_input_routing"), bRequireInput);

		TArray<TSharedPtr<FJsonValue>> Checks;
		bool bValid = true;
		const bool bHasTarget = CommonUiReceiptHasAny(Receipt, {TEXT("target_asset"), TEXT("widget_path"), TEXT("widget_blueprint_path"), TEXT("asset_path")});
		AddCommonUiCheck(Checks, bValid, TEXT("target_binding"), bHasTarget, bHasTarget ? TEXT("Target asset binding found.") : TEXT("Missing widget/style target asset binding."));
		if (bRequireCompile)
		{
			const bool bCompileOk = CommonUiReceiptBool(Receipt, TEXT("compile_ok")) || CommonUiReceiptBool(Receipt, TEXT("compileOk"));
			AddCommonUiCheck(Checks, bValid, TEXT("compile"), bCompileOk, bCompileOk ? TEXT("Compile evidence passed.") : TEXT("Missing compile_ok=true evidence."));
		}
		if (bRequirePreview)
		{
			const bool bPreviewOk = CommonUiReceiptHasAny(Receipt, {TEXT("preview"), TEXT("preview_receipt"), TEXT("screenshot"), TEXT("screenshot_path")});
			AddCommonUiCheck(Checks, bValid, TEXT("preview"), bPreviewOk, bPreviewOk ? TEXT("Preview/screenshot evidence found.") : TEXT("Missing preview or screenshot evidence."));
		}
		if (bRequireInput)
		{
			const bool bInputOk = CommonUiReceiptBool(Receipt, TEXT("input_routing_ok")) || CommonUiReceiptHasAny(Receipt, {TEXT("input_routing"), TEXT("input_receipt")});
			AddCommonUiCheck(Checks, bValid, TEXT("input_routing"), bInputOk, bInputOk ? TEXT("Input routing evidence found.") : TEXT("Missing input routing evidence."));
		}
		OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		OutStructured->SetBoolField(TEXT("valid"), bValid);
		OutStructured->SetArrayField(TEXT("checks"), Checks);
		OutSummary = bValid ? FString::Printf(TEXT("%s passed."), *Spec.Name) : FString::Printf(TEXT("%s failed closed."), *Spec.Name);
		return bValid;
	}

	static bool ExecuteCommonUiP1Tool(
		const FCommonUiP1Spec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("tool_name"), Spec.Name);
		OutStructured->SetStringField(TEXT("domain"), TEXT("umg"));
		OutStructured->SetStringField(TEXT("family"), TEXT("commonui_production"));
		OutStructured->SetStringField(TEXT("mode"), Spec.Mode);
		OutStructured->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_write_or_plan") : TEXT("read_or_validate"));
		OutStructured->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));
		FWorldAiDataUiSpec GateSpec{Spec.Name, Spec.Description, TEXT("umg"), TEXT("plan"), {TEXT("CommonUI")}, {TEXT("CommonUI")}, {}, {}, {}};
		FString Availability;
		ProbeAvailability(GateSpec, OutStructured, Availability);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		OutStructured->SetStringField(TEXT("target_asset"), CommonUiTargetPath(Arguments));
		OutStructured->SetArrayField(TEXT("class_catalog"), CommonUiClassCatalogJson(Context, Spec.FocusClasses));
		OutStructured->SetObjectField(TEXT("target_asset_summary"), CommonUiAssetSummaryJson(Context, CommonUiTargetPath(Arguments)));

		if (Spec.Mode == TEXT("receipt") || Spec.Mode == TEXT("compile_gate"))
		{
			if (Arguments->HasField(TEXT("receipt")))
			{
				return ExecuteCommonUiReceiptTool(Spec, Arguments, OutStructured, OutSummary, OutError);
			}
			OutStructured->SetStringField(TEXT("status"), TEXT("receipt_required"));
			OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			OutSummary = FString::Printf(TEXT("%s returned receipt requirements."), *Spec.Name);
			return true;
		}

		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		if (bExecute && Spec.bMutation)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_commonui_writer"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("use_umg_author_with_compile_preview_receipts_or_promote_specific_writer"));
			OutError = FString::Printf(TEXT("%s is a concrete CommonUI plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			OutSummary = OutError;
			return false;
		}
		OutStructured->SetStringField(TEXT("status"), Spec.Mode == TEXT("class_catalog") ? TEXT("completed") : TEXT("dry_run"));
		OutSummary = FString::Printf(TEXT("%s returned CommonUI %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TArray<FCommonUiP1Spec> CommonUiP1Specs()
	{
		const TArray<FString> CompileReq{TEXT("target asset"), TEXT("compile_ok=true"), TEXT("diagnostics with zero blocking errors")};
		const TArray<FString> PreviewReq{TEXT("target asset"), TEXT("preview or screenshot evidence"), TEXT("compile gate when production delivery")};
		return {
			{TEXT("commonui_widget_classes_list"), TEXT("Concrete CommonUI class catalog and availability probe."), TEXT("class_catalog"), false, {}, {}, {TEXT("class availability"), TEXT("plugin/module gate")}},
			{TEXT("commonui_button_style_inspect"), TEXT("Inspect a CommonButtonStyle asset or return style class probes."), TEXT("asset_inspect"), false, {TEXT("CommonButtonStyle"), TEXT("CommonButtonBase")}, {}, {TEXT("target style asset when validating production UI")}},
			{TEXT("commonui_tab_list_configure"), TEXT("Plan CommonTabListWidgetBase entries, buttons, and activation receipts."), TEXT("plan"), true, {TEXT("CommonTabListWidgetBase"), TEXT("CommonButtonBase")}, {TEXT("Resolve tab list widget."), TEXT("Resolve button style and tab descriptors."), TEXT("Bind activatable widget stack."), TEXT("Require compile, preview, and input routing receipts.")}, {TEXT("widget blueprint"), TEXT("tab descriptors"), TEXT("compile receipt"), TEXT("preview receipt")}},
			{TEXT("commonui_carousel_configure"), TEXT("Plan CommonWidgetCarousel child setup and navigation receipts."), TEXT("plan"), true, {TEXT("CommonWidgetCarousel")}, {TEXT("Resolve carousel widget."), TEXT("Plan child widget order and navigation actions."), TEXT("Require preview and input focus proof.")}, {TEXT("widget blueprint"), TEXT("child widget list"), TEXT("preview receipt"), TEXT("input routing receipt")}},
			{TEXT("commonui_video_player_configure"), TEXT("Plan CommonVideoPlayer media binding and preview receipts."), TEXT("plan"), true, {TEXT("CommonVideoPlayer")}, {TEXT("Resolve video player widget."), TEXT("Resolve MediaSource/MediaPlayer assets."), TEXT("Plan fallback image and playback policy."), TEXT("Require preview receipt.")}, PreviewReq},
			{TEXT("commonui_preview_receipt"), TEXT("Validate CommonUI preview/screenshot evidence."), TEXT("receipt"), false, {}, {}, PreviewReq},
			{TEXT("commonui_input_action_bind"), TEXT("Plan CommonUI input action binding and routing receipts."), TEXT("plan"), true, {TEXT("CommonInputActionDataBase"), TEXT("CommonBoundActionButton")}, {TEXT("Resolve input action data."), TEXT("Bind action to widget/button."), TEXT("Check platform trait and focus route."), TEXT("Require input routing receipt.")}, {TEXT("input action asset"), TEXT("widget target"), TEXT("input routing receipt")}},
			{TEXT("commonui_activatable_widget_create"), TEXT("Plan CommonActivatableWidget Blueprint creation."), TEXT("plan"), true, {TEXT("CommonActivatableWidget"), TEXT("CommonUserWidget")}, {TEXT("Validate target package."), TEXT("Use CommonActivatableWidget parent class."), TEXT("Compile generated widget Blueprint."), TEXT("Capture preview and navigation stack receipt.")}, {TEXT("target package"), TEXT("parent class"), TEXT("compile receipt"), TEXT("preview receipt")}},
			{TEXT("commonui_style_asset_create"), TEXT("Plan CommonUI style asset creation."), TEXT("plan"), true, {TEXT("CommonButtonStyle"), TEXT("CommonTextStyle"), TEXT("CommonBorderStyle")}, {TEXT("Select style class."), TEXT("Resolve fonts, brushes, margins, and color states."), TEXT("Create or update style asset in a locked path."), TEXT("Read back properties and preview.")}, {TEXT("style class"), TEXT("style asset path"), TEXT("property readback"), TEXT("preview receipt")}},
			{TEXT("commonui_text_style_inspect"), TEXT("Inspect a CommonTextStyle asset or return text style class probes."), TEXT("asset_inspect"), false, {TEXT("CommonTextStyle"), TEXT("CommonTextBlock"), TEXT("CommonRichTextBlock")}, {}, {TEXT("target text style asset when validating production UI")}},
			{TEXT("commonui_border_style_inspect"), TEXT("Inspect a CommonBorderStyle asset or return border style class probes."), TEXT("asset_inspect"), false, {TEXT("CommonBorderStyle"), TEXT("CommonBorder")}, {}, {TEXT("target border style asset when validating production UI")}},
			{TEXT("commonui_input_router_snapshot"), TEXT("Read CommonUI input routing class availability and handoff hints."), TEXT("runtime_probe"), false, {TEXT("CommonInputActionDataBase"), TEXT("CommonBoundActionButton")}, {TEXT("Use CommonUI runtime/input router in client QA."), TEXT("Attach focus path and platform trait evidence.")}, {TEXT("focus target"), TEXT("input action evidence")}},
			{TEXT("commonui_menu_stack_plan"), TEXT("Plan CommonActivatableWidget menu stack flow."), TEXT("plan"), true, {TEXT("CommonActivatableWidget")}, {TEXT("Define stack root and modal layers."), TEXT("Assign activatable widget classes."), TEXT("Plan back handling and focus restoration."), TEXT("Require compile, preview, and input route receipts.")}, {TEXT("stack definition"), TEXT("widget class list"), TEXT("compile receipt"), TEXT("input route receipt")}},
			{TEXT("commonui_widget_compile_validate"), TEXT("Validate CommonUI widget compile receipt or return compile requirements."), TEXT("compile_gate"), false, {}, {}, CompileReq},
			{TEXT("commonui_platform_traits_audit"), TEXT("Audit CommonUI platform trait readiness and class availability."), TEXT("runtime_probe"), false, {TEXT("CommonInputActionDataBase"), TEXT("CommonUserWidget")}, {TEXT("Check platform trait inputs."), TEXT("Check action glyph and focus mode requirements."), TEXT("Attach per-platform UI QA receipt.")}, {TEXT("platform list"), TEXT("trait decision"), TEXT("input glyph evidence")}},
			{TEXT("commonui_receipt_validate"), TEXT("Validate CommonUI production receipt before delivery."), TEXT("receipt"), false, {}, {}, {TEXT("target asset"), TEXT("compile_ok=true"), TEXT("optional preview evidence"), TEXT("optional input routing evidence")}}
		};
	}

	static void RegisterCommonUiP1Tools(FSololmcpToolRegistry& Registry)
	{
		for (const FCommonUiP1Spec& Spec : CommonUiP1Specs())
		{
			FSololmcpToolDefinition Def;
			Def.Name = Spec.Name;
			Def.Description = Spec.Description;
			Def.InputSchema = CommonUiSchema();
			Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
			Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return ExecuteCommonUiP1Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
			};
			Registry.Register(Def);
		}
	}

	struct FWorldAiP1Spec
	{
		FString Name;
		FString Description;
		FString Mode;
		FString Subdomain;
		bool bMutation = false;
		TArray<FString> FocusClasses;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
	};

	static TSharedRef<FJsonObject> WorldAiP1Schema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target SmartObject, StateTree, Mass config, spawner, or related asset."))},
			{TEXT("definition_path"), FSololmcpSchemaBuilder::String(TEXT("SmartObject definition asset path."))},
			{TEXT("state_tree_path"), FSololmcpSchemaBuilder::String(TEXT("StateTree asset path."))},
			{TEXT("entity_config"), FSololmcpSchemaBuilder::String(TEXT("Mass entity config asset path."))},
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Optional runtime actor label/name for snapshot tools."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Validation/compile/spawn/debug receipt to validate."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation. P1 World AI tools fail closed unless promoted to a dedicated writer."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return probe/plan only. Default true."))}
		});
	}

	static FString WorldAiTargetPath(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Path;
		for (const TCHAR* Field : {TEXT("target_asset"), TEXT("definition_path"), TEXT("state_tree_path"), TEXT("entity_config"), TEXT("target")})
		{
			if (Arguments->TryGetStringField(Field, Path) && !Path.IsEmpty())
			{
				return Path;
			}
		}
		return FString();
	}

	static TArray<TPair<FString, FString>> WorldAiClassCatalog()
	{
		return {
			{TEXT("SmartObjectDefinition"), TEXT("/Script/SmartObjectsModule.SmartObjectDefinition")},
			{TEXT("SmartObjectComponent"), TEXT("/Script/SmartObjectsModule.SmartObjectComponent")},
			{TEXT("SmartObjectSubsystem"), TEXT("/Script/SmartObjectsModule.SmartObjectSubsystem")},
			{TEXT("StateTree"), TEXT("/Script/StateTreeModule.StateTree")},
			{TEXT("StateTreeComponent"), TEXT("/Script/StateTreeModule.StateTreeComponent")},
			{TEXT("StateTreeTaskBlueprintBase"), TEXT("/Script/StateTreeModule.StateTreeTaskBlueprintBase")},
			{TEXT("StateTreeConditionBlueprintBase"), TEXT("/Script/StateTreeModule.StateTreeConditionBlueprintBase")},
			{TEXT("MassEntityConfigAsset"), TEXT("/Script/MassEntity.MassEntityConfigAsset")},
			{TEXT("MassSpawner"), TEXT("/Script/MassSpawner.MassSpawner")},
			{TEXT("MassProcessor"), TEXT("/Script/MassEntity.MassProcessor")},
			{TEXT("ZoneGraphData"), TEXT("/Script/ZoneGraph.ZoneGraphData")},
			{TEXT("ZoneShapeComponent"), TEXT("/Script/ZoneGraph.ZoneShapeComponent")}
		};
	}

	static TArray<TSharedPtr<FJsonValue>> WorldAiClassCatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusClasses = {})
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TPair<FString, FString>& Row : WorldAiClassCatalog())
		{
			if (!FocusClasses.IsEmpty() && !FocusClasses.Contains(Row.Key))
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(CommonUiClassStatusJson(Context, Row.Key, Row.Value)));
		}
		return Rows;
	}

	static bool ExecuteWorldAiReceiptTool(
		const FWorldAiP1Spec& Spec,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_missing_receipt"));
			OutError = TEXT("World AI receipt validation requires a receipt object.");
			return false;
		}
		const TSharedRef<FJsonObject> Receipt = ReceiptPtr->ToSharedRef();
		TArray<TSharedPtr<FJsonValue>> Checks;
		bool bValid = true;
		const bool bHasTarget = CommonUiReceiptHasAny(Receipt, {TEXT("target_asset"), TEXT("definition_path"), TEXT("state_tree_path"), TEXT("entity_config"), TEXT("spawner"), TEXT("asset_path")});
		AddCommonUiCheck(Checks, bValid, TEXT("target_binding"), bHasTarget, bHasTarget ? TEXT("Target binding found.") : TEXT("Missing target binding."));

		if (Spec.Subdomain == TEXT("state_tree"))
		{
			const bool bCompileOk = CommonUiReceiptBool(Receipt, TEXT("compile_ok")) || CommonUiReceiptBool(Receipt, TEXT("compileOk")) || CommonUiReceiptBool(Receipt, TEXT("validate_ok"));
			AddCommonUiCheck(Checks, bValid, TEXT("compile_or_validate"), bCompileOk, bCompileOk ? TEXT("StateTree compile/validation evidence passed.") : TEXT("Missing compile_ok or validate_ok evidence."));
		}
		else if (Spec.Subdomain == TEXT("mass"))
		{
			const bool bSpawnOk = CommonUiReceiptBool(Receipt, TEXT("spawn_ok")) || CommonUiReceiptBool(Receipt, TEXT("validate_ok")) || CommonUiReceiptHasAny(Receipt, {TEXT("simulation_snapshot"), TEXT("density_receipt")});
			AddCommonUiCheck(Checks, bValid, TEXT("mass_spawn_or_validate"), bSpawnOk, bSpawnOk ? TEXT("Mass spawn/validation evidence found.") : TEXT("Missing Mass spawn/validation evidence."));
		}
		else
		{
			const bool bValidateOk = CommonUiReceiptBool(Receipt, TEXT("validate_ok")) || CommonUiReceiptBool(Receipt, TEXT("runtime_snapshot_ok")) || CommonUiReceiptHasAny(Receipt, {TEXT("slot_readback"), TEXT("runtime_snapshot")});
			AddCommonUiCheck(Checks, bValid, TEXT("validate_or_readback"), bValidateOk, bValidateOk ? TEXT("SmartObject validation/readback evidence found.") : TEXT("Missing SmartObject validation/readback evidence."));
		}

		OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		OutStructured->SetBoolField(TEXT("valid"), bValid);
		OutStructured->SetArrayField(TEXT("checks"), Checks);
		OutSummary = bValid ? FString::Printf(TEXT("%s passed."), *Spec.Name) : FString::Printf(TEXT("%s failed closed."), *Spec.Name);
		return bValid;
	}

	static bool ExecuteWorldAiP1Tool(
		const FWorldAiP1Spec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("tool_name"), Spec.Name);
		OutStructured->SetStringField(TEXT("domain"), TEXT("world_ai"));
		OutStructured->SetStringField(TEXT("family"), TEXT("world_ai_smart_state_mass"));
		OutStructured->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		OutStructured->SetStringField(TEXT("mode"), Spec.Mode);
		OutStructured->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_or_runtime_write_plan") : TEXT("read_or_validate"));
		OutStructured->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));
		FWorldAiDataUiSpec GateSpec{Spec.Name, Spec.Description, TEXT("world_ai"), TEXT("plan"), {TEXT("SmartObjects"), TEXT("StateTree"), TEXT("MassEntity"), TEXT("MassAI"), TEXT("ZoneGraph")}, {TEXT("SmartObjectsModule"), TEXT("StateTreeModule"), TEXT("MassEntity")}, {}, {}, {}};
		FString Availability;
		ProbeAvailability(GateSpec, OutStructured, Availability);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		OutStructured->SetStringField(TEXT("target_asset"), WorldAiTargetPath(Arguments));
		OutStructured->SetArrayField(TEXT("class_catalog"), WorldAiClassCatalogJson(Context, Spec.FocusClasses));
		OutStructured->SetObjectField(TEXT("target_asset_summary"), CommonUiAssetSummaryJson(Context, WorldAiTargetPath(Arguments)));

		if (Spec.Mode == TEXT("receipt"))
		{
			if (Arguments->HasField(TEXT("receipt")))
			{
				return ExecuteWorldAiReceiptTool(Spec, Arguments, OutStructured, OutSummary, OutError);
			}
			OutStructured->SetStringField(TEXT("status"), TEXT("receipt_required"));
			OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			OutSummary = FString::Printf(TEXT("%s returned receipt requirements."), *Spec.Name);
			return true;
		}

		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		if (bExecute && Spec.bMutation)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_world_ai_writer"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_world_ai_writer_after_live_fixture"));
			OutError = FString::Printf(TEXT("%s is a concrete World AI plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			OutSummary = OutError;
			return false;
		}
		OutStructured->SetStringField(TEXT("status"), Spec.Mode == TEXT("class_catalog") || Spec.Mode == TEXT("snapshot") ? TEXT("completed") : TEXT("dry_run"));
		OutSummary = FString::Printf(TEXT("%s returned World AI %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TArray<FWorldAiP1Spec> WorldAiP1Specs()
	{
		const TArray<FString> SmartFocus{TEXT("SmartObjectDefinition"), TEXT("SmartObjectComponent"), TEXT("SmartObjectSubsystem")};
		const TArray<FString> StateFocus{TEXT("StateTree"), TEXT("StateTreeComponent"), TEXT("StateTreeTaskBlueprintBase"), TEXT("StateTreeConditionBlueprintBase")};
		const TArray<FString> MassFocus{TEXT("MassEntityConfigAsset"), TEXT("MassSpawner"), TEXT("MassProcessor"), TEXT("ZoneGraphData"), TEXT("ZoneShapeComponent")};
		const TArray<FString> SmartReq{TEXT("definition target"), TEXT("slot/behavior readback"), TEXT("validate_ok or runtime snapshot")};
		const TArray<FString> StateReq{TEXT("state tree target"), TEXT("compile_ok or validate_ok"), TEXT("state/transition readback")};
		const TArray<FString> MassReq{TEXT("entity config/spawner target"), TEXT("spawn or validation evidence"), TEXT("density/performance receipt")};
		return {
			{TEXT("smart_object_definition_create"), TEXT("Plan SmartObject definition creation."), TEXT("plan"), TEXT("smart_object"), true, SmartFocus, {TEXT("Validate package path."), TEXT("Resolve SmartObjectDefinition class."), TEXT("Plan slot and behavior schema."), TEXT("Require validation/readback receipt.")}, SmartReq},
			{TEXT("smart_object_definition_inspect"), TEXT("Inspect SmartObject definition readiness."), TEXT("inspect"), TEXT("smart_object"), false, SmartFocus, {}, SmartReq},
			{TEXT("smart_object_slot_add"), TEXT("Plan SmartObject slot add."), TEXT("plan"), TEXT("smart_object"), true, SmartFocus, {TEXT("Resolve definition asset."), TEXT("Plan slot transform/tags."), TEXT("Require slot readback.")}, SmartReq},
			{TEXT("smart_object_slot_remove"), TEXT("Plan SmartObject slot removal."), TEXT("plan"), TEXT("smart_object"), true, SmartFocus, {TEXT("Resolve definition asset."), TEXT("Find slot id/name."), TEXT("Require post-remove readback.")}, SmartReq},
			{TEXT("smart_object_slot_inspect"), TEXT("Inspect SmartObject slot schema/readiness."), TEXT("inspect"), TEXT("smart_object"), false, SmartFocus, {}, SmartReq},
			{TEXT("smart_object_query"), TEXT("Plan/read SmartObject query route."), TEXT("snapshot"), TEXT("smart_object"), false, SmartFocus, {TEXT("Resolve query origin/tags."), TEXT("Route through runtime subsystem when PIE/live world is available.")}, SmartReq},
			{TEXT("smart_object_claim"), TEXT("Plan SmartObject claim route."), TEXT("plan"), TEXT("smart_object"), true, SmartFocus, {TEXT("Resolve runtime subsystem."), TEXT("Validate claim requester."), TEXT("Require release/cleanup route.")}, SmartReq},
			{TEXT("smart_object_release"), TEXT("Plan SmartObject release route."), TEXT("plan"), TEXT("smart_object"), true, SmartFocus, {TEXT("Resolve claim handle."), TEXT("Release and read back runtime state.")}, SmartReq},
			{TEXT("smart_object_runtime_snapshot"), TEXT("Read SmartObject runtime snapshot contract."), TEXT("snapshot"), TEXT("smart_object"), false, SmartFocus, {}, SmartReq},
			{TEXT("smart_object_receipt_validate"), TEXT("Validate SmartObject production receipt."), TEXT("receipt"), TEXT("smart_object"), false, SmartFocus, {}, SmartReq},
			{TEXT("state_tree_asset_create"), TEXT("Plan StateTree asset creation."), TEXT("plan"), TEXT("state_tree"), true, StateFocus, {TEXT("Validate package path."), TEXT("Resolve StateTree schema."), TEXT("Plan states/tasks/conditions."), TEXT("Require compile receipt.")}, StateReq},
			{TEXT("state_tree_schema_inspect"), TEXT("Inspect StateTree schema classes."), TEXT("inspect"), TEXT("state_tree"), false, StateFocus, {}, StateReq},
			{TEXT("state_tree_task_add_plan"), TEXT("Plan StateTree task insertion."), TEXT("plan"), TEXT("state_tree"), true, StateFocus, {TEXT("Resolve state."), TEXT("Resolve task class."), TEXT("Plan property bindings."), TEXT("Require compile/readback.")}, StateReq},
			{TEXT("state_tree_condition_add_plan"), TEXT("Plan StateTree condition insertion."), TEXT("plan"), TEXT("state_tree"), true, StateFocus, {TEXT("Resolve transition/state."), TEXT("Resolve condition class."), TEXT("Plan bindings."), TEXT("Require compile/readback.")}, StateReq},
			{TEXT("state_tree_transition_add_plan"), TEXT("Plan StateTree transition insertion."), TEXT("plan"), TEXT("state_tree"), true, StateFocus, {TEXT("Resolve source and target states."), TEXT("Plan trigger/conditions."), TEXT("Require compile/readback.")}, StateReq},
			{TEXT("state_tree_compile"), TEXT("Validate StateTree compile receipt or return compile requirements."), TEXT("receipt"), TEXT("state_tree"), false, StateFocus, {}, StateReq},
			{TEXT("state_tree_debug_snapshot"), TEXT("Read StateTree debug snapshot contract."), TEXT("snapshot"), TEXT("state_tree"), false, StateFocus, {}, StateReq},
			{TEXT("state_tree_receipt_validate"), TEXT("Validate StateTree production receipt."), TEXT("receipt"), TEXT("state_tree"), false, StateFocus, {}, StateReq},
			{TEXT("mass_entity_config_create"), TEXT("Plan Mass entity config creation."), TEXT("plan"), TEXT("mass"), true, MassFocus, {TEXT("Validate package path."), TEXT("Resolve fragments/traits."), TEXT("Plan representation and movement fragments."), TEXT("Require validation receipt.")}, MassReq},
			{TEXT("mass_entity_config_inspect"), TEXT("Inspect Mass entity config readiness."), TEXT("inspect"), TEXT("mass"), false, MassFocus, {}, MassReq},
			{TEXT("mass_spawner_create"), TEXT("Plan Mass spawner actor/config creation."), TEXT("plan"), TEXT("mass"), true, MassFocus, {TEXT("Resolve target level."), TEXT("Resolve entity config."), TEXT("Plan spawn data and bounds."), TEXT("Require spawn and performance receipt.")}, MassReq},
			{TEXT("mass_spawner_spawn_data_set"), TEXT("Plan Mass spawner spawn-data update."), TEXT("plan"), TEXT("mass"), true, MassFocus, {TEXT("Resolve spawner."), TEXT("Plan spawn data update."), TEXT("Require readback.")}, MassReq},
			{TEXT("mass_crowd_lane_bind"), TEXT("Plan Mass crowd ZoneGraph lane binding."), TEXT("plan"), TEXT("mass"), true, MassFocus, {TEXT("Resolve ZoneGraph lanes."), TEXT("Resolve crowd entity config."), TEXT("Plan lane tags and density budget."), TEXT("Require preview/debug receipt.")}, MassReq},
			{TEXT("mass_zonegraph_sync_plan"), TEXT("Plan Mass/ZoneGraph synchronization."), TEXT("plan"), TEXT("mass"), true, MassFocus, {TEXT("Audit ZoneGraph data."), TEXT("Map lanes to Mass movement/crowd traits."), TEXT("Require debug preview.")}, MassReq},
			{TEXT("mass_processor_list"), TEXT("Read Mass processor class availability."), TEXT("class_catalog"), TEXT("mass"), false, MassFocus, {}, MassReq},
			{TEXT("mass_fragment_schema_get"), TEXT("Read Mass fragment/trait schema readiness."), TEXT("class_catalog"), TEXT("mass"), false, MassFocus, {}, MassReq},
			{TEXT("mass_simulation_snapshot"), TEXT("Read Mass simulation snapshot contract."), TEXT("snapshot"), TEXT("mass"), false, MassFocus, {}, MassReq},
			{TEXT("mass_crowd_density_plan"), TEXT("Plan Mass crowd density/performance budget."), TEXT("plan"), TEXT("mass"), true, MassFocus, {TEXT("Resolve target region."), TEXT("Estimate density and tick budget."), TEXT("Plan LOD/representation policy."), TEXT("Require performance receipt.")}, MassReq},
			{TEXT("mass_spawn_receipt_validate"), TEXT("Validate Mass spawn/deployment receipt."), TEXT("receipt"), TEXT("mass"), false, MassFocus, {}, MassReq},
			{TEXT("mass_entity_debug_snapshot"), TEXT("Read Mass entity debug snapshot contract."), TEXT("snapshot"), TEXT("mass"), false, MassFocus, {}, MassReq}
		};
	}

	static void RegisterWorldAiP1Tools(FSololmcpToolRegistry& Registry)
	{
		for (const FWorldAiP1Spec& Spec : WorldAiP1Specs())
		{
			FSololmcpToolDefinition Def;
			Def.Name = Spec.Name;
			Def.Description = Spec.Description;
			Def.InputSchema = WorldAiP1Schema();
			Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
			Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return ExecuteWorldAiP1Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
			};
			Registry.Register(Def);
		}
	}

	struct FMoverP1Spec
	{
		FString Name;
		FString Description;
		FString Mode;
		FString Subdomain;
		bool bMutation = false;
		TArray<FString> GatePlugins;
		TArray<FString> GateModules;
		TArray<FString> FocusClasses;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
	};

	static TSharedRef<FJsonObject> MoverP1Schema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Target actor label, name, or path."))},
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for target_actor."))},
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target Blueprint, component asset, config, PoseSearch, or related asset path."))},
			{TEXT("blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path for Mover component authoring."))},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Mover/NavMover/CharacterMover component name."))},
			{TEXT("movement_mode"), FSololmcpSchemaBuilder::String(TEXT("Movement mode name such as Walking, Falling, Flying, Swimming, or NavWalking."))},
			{TEXT("mode_name"), FSololmcpSchemaBuilder::String(TEXT("Alias for movement_mode."))},
			{TEXT("input_mapping_context"), FSololmcpSchemaBuilder::String(TEXT("Enhanced Input mapping context asset path."))},
			{TEXT("input_actions"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Enhanced Input action asset paths."))},
			{TEXT("prediction_seconds"), FSololmcpSchemaBuilder::Number(TEXT("Trajectory prediction horizon in seconds."))},
			{TEXT("sample_count"), FSololmcpSchemaBuilder::Integer(TEXT("Trajectory/debug sample count."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Mover validation/readback receipt to validate."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation. P1 Mover authoring tools fail closed unless promoted to a dedicated writer."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return probe/plan only. Default true."))}
		});
	}

	static FString MoverTargetActor(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Value;
		for (const TCHAR* Field : {TEXT("target_actor"), TEXT("actor"), TEXT("target")})
		{
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				return Value;
			}
		}
		return FString();
	}

	static FString MoverTargetAsset(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Value;
		for (const TCHAR* Field : {TEXT("target_asset"), TEXT("blueprint_path"), TEXT("asset_path")})
		{
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				return Value;
			}
		}
		return FString();
	}

	static FString MoverModeName(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Value;
		for (const TCHAR* Field : {TEXT("movement_mode"), TEXT("mode_name")})
		{
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				return Value;
			}
		}
		return FString();
	}

	static TArray<TPair<FString, FString>> MoverClassCatalog()
	{
		return {
			{TEXT("MoverComponent"), TEXT("/Script/Mover.MoverComponent")},
			{TEXT("CharacterMoverComponent"), TEXT("/Script/Mover.CharacterMoverComponent")},
			{TEXT("NavMoverComponent"), TEXT("/Script/Mover.NavMoverComponent")},
			{TEXT("MoverBlackboard"), TEXT("/Script/Mover.MoverBlackboard")},
			{TEXT("MoverSimulation"), TEXT("/Script/Mover.MoverSimulation")},
			{TEXT("MovementModeStateMachine"), TEXT("/Script/Mover.MovementModeStateMachine")},
			{TEXT("BaseMovementMode"), TEXT("/Script/Mover.BaseMovementMode")},
			{TEXT("BaseMovementModeTransition"), TEXT("/Script/Mover.BaseMovementModeTransition")},
			{TEXT("LayeredMoveLogic"), TEXT("/Script/Mover.LayeredMoveLogic")},
			{TEXT("MoverPoseSearchTrajectoryPredictor"), TEXT("/Script/Mover.MoverPoseSearchTrajectoryPredictor")},
			{TEXT("PathedPhysicsMoverComponent"), TEXT("/Script/Mover.PathedPhysicsMoverComponent")},
			{TEXT("MoverNetworkPredictionLiaisonComponent"), TEXT("/Script/Mover.MoverNetworkPredictionLiaisonComponent")},
			{TEXT("MoverStandaloneLiaisonComponent"), TEXT("/Script/Mover.MoverStandaloneLiaisonComponent")},
			{TEXT("PoseSearchDatabase"), TEXT("/Script/PoseSearch.PoseSearchDatabase")},
			{TEXT("PoseSearchSchema"), TEXT("/Script/PoseSearch.PoseSearchSchema")},
			{TEXT("PoseSearchTrajectoryLibrary"), TEXT("/Script/PoseSearch.PoseSearchTrajectoryLibrary")},
			{TEXT("InputAction"), TEXT("/Script/EnhancedInput.InputAction")},
			{TEXT("InputMappingContext"), TEXT("/Script/EnhancedInput.InputMappingContext")},
			{TEXT("ChaosCharacterMoverComponent"), TEXT("/Script/ChaosMover.ChaosCharacterMoverComponent")},
			{TEXT("ChaosMoverSimulation"), TEXT("/Script/ChaosMover.ChaosMoverSimulation")},
			{TEXT("ChaosWalkingMode"), TEXT("/Script/ChaosMover.ChaosWalkingMode")},
			{TEXT("ChaosFallingMode"), TEXT("/Script/ChaosMover.ChaosFallingMode")},
			{TEXT("ChaosFlyingMode"), TEXT("/Script/ChaosMover.ChaosFlyingMode")},
			{TEXT("ChaosSwimmingMode"), TEXT("/Script/ChaosMover.ChaosSwimmingMode")}
		};
	}

	static TArray<TSharedPtr<FJsonValue>> MoverClassCatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusClasses = {})
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TPair<FString, FString>& Row : MoverClassCatalog())
		{
			if (!FocusClasses.IsEmpty() && !FocusClasses.Contains(Row.Key))
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(CommonUiClassStatusJson(Context, Row.Key, Row.Value)));
		}
		return Rows;
	}

	static TSharedRef<FJsonObject> MoverActorSnapshotJson(const FSololmcpToolExecutionContext& Context, const FString& ActorId)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("requested_actor"), ActorId);
		if (ActorId.IsEmpty())
		{
			Obj->SetStringField(TEXT("status"), TEXT("not_requested"));
			return Obj;
		}

		FString ActorError;
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, ActorError);
		Obj->SetBoolField(TEXT("found"), Actor != nullptr);
		if (!Actor)
		{
			Obj->SetStringField(TEXT("status"), TEXT("actor_not_found"));
			if (!ActorError.IsEmpty())
			{
				Obj->SetStringField(TEXT("error"), ActorError);
			}
			return Obj;
		}

		Obj->SetStringField(TEXT("status"), TEXT("loaded"));
		Obj->SetStringField(TEXT("name"), Actor->GetName());
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("path"), Actor->GetPathName());
		Obj->SetStringField(TEXT("class"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		TArray<TSharedPtr<FJsonValue>> ComponentRows;
		int32 MoverComponentCount = 0;
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			const FString ClassPath = Component->GetClass() ? Component->GetClass()->GetPathName() : FString();
			const FString ClassName = Component->GetClass() ? Component->GetClass()->GetName() : FString();
			const bool bMoverLike = ClassName.Contains(TEXT("Mover"), ESearchCase::IgnoreCase) || ClassPath.Contains(TEXT("Mover"), ESearchCase::IgnoreCase);
			if (bMoverLike)
			{
				++MoverComponentCount;
			}
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Component->GetName());
			Row->SetStringField(TEXT("path"), Component->GetPathName());
			Row->SetStringField(TEXT("class"), ClassPath);
			Row->SetBoolField(TEXT("registered"), Component->IsRegistered());
			Row->SetBoolField(TEXT("active"), Component->IsActive());
			Row->SetBoolField(TEXT("mover_like"), bMoverLike);

			TArray<TSharedPtr<FJsonValue>> Properties;
			int32 PropertyCount = 0;
			if (Component->GetClass())
			{
				for (TFieldIterator<FProperty> It(Component->GetClass(), EFieldIteratorFlags::IncludeSuper); It && PropertyCount < 60; ++It, ++PropertyCount)
				{
					TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
					Property->SetStringField(TEXT("name"), It->GetName());
					Property->SetStringField(TEXT("class"), It->GetClass()->GetName());
					Property->SetBoolField(TEXT("editable"), It->HasAnyPropertyFlags(CPF_Edit));
					Properties.Add(MakeShared<FJsonValueObject>(Property));
				}
			}
			Row->SetArrayField(TEXT("property_sample"), Properties);
			Row->SetNumberField(TEXT("property_sample_count"), Properties.Num());
			ComponentRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Obj->SetArrayField(TEXT("components"), ComponentRows);
		Obj->SetNumberField(TEXT("component_count"), ComponentRows.Num());
		Obj->SetNumberField(TEXT("mover_component_count"), MoverComponentCount);
		return Obj;
	}

	static bool ExecuteMoverReceiptTool(
		const FMoverP1Spec& Spec,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_missing_receipt"));
			OutError = TEXT("Mover receipt validation requires a receipt object.");
			return false;
		}

		const TSharedRef<FJsonObject> Receipt = ReceiptPtr->ToSharedRef();
		TArray<TSharedPtr<FJsonValue>> Checks;
		bool bValid = true;
		const bool bHasTarget = CommonUiReceiptHasAny(Receipt, {TEXT("target_actor"), TEXT("actor"), TEXT("target_asset"), TEXT("blueprint_path"), TEXT("component_path"), TEXT("asset_path")});
		AddCommonUiCheck(Checks, bValid, TEXT("target_binding"), bHasTarget, bHasTarget ? TEXT("Target actor/asset binding found.") : TEXT("Missing target actor/asset binding."));

		if (Spec.Name == TEXT("mover_movement_mode_receipt"))
		{
			const bool bModeOk = CommonUiReceiptBool(Receipt, TEXT("movement_mode_ok")) || CommonUiReceiptHasAny(Receipt, {TEXT("mode_readback"), TEXT("movement_mode"), TEXT("current_mode")});
			AddCommonUiCheck(Checks, bValid, TEXT("movement_mode_readback"), bModeOk, bModeOk ? TEXT("Movement mode readback found.") : TEXT("Missing movement mode readback evidence."));
		}
		else
		{
			const bool bValidateOk = CommonUiReceiptBool(Receipt, TEXT("validate_ok")) || CommonUiReceiptBool(Receipt, TEXT("component_readback_ok")) || CommonUiReceiptHasAny(Receipt, {TEXT("component_readback"), TEXT("debug_snapshot"), TEXT("movement_mode_ok"), TEXT("trajectory_preview")});
			AddCommonUiCheck(Checks, bValid, TEXT("validate_or_readback"), bValidateOk, bValidateOk ? TEXT("Mover validation/readback evidence found.") : TEXT("Missing validate/readback evidence."));
		}

		OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		OutStructured->SetBoolField(TEXT("valid"), bValid);
		OutStructured->SetArrayField(TEXT("checks"), Checks);
		OutSummary = bValid ? FString::Printf(TEXT("%s passed."), *Spec.Name) : FString::Printf(TEXT("%s failed closed."), *Spec.Name);
		return bValid;
	}

	static bool ExecuteMoverP1Tool(
		const FMoverP1Spec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("tool_name"), Spec.Name);
		OutStructured->SetStringField(TEXT("domain"), TEXT("gameplay_movement"));
		OutStructured->SetStringField(TEXT("family"), TEXT("mover_authoring"));
		OutStructured->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		OutStructured->SetStringField(TEXT("mode"), Spec.Mode);
		OutStructured->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_or_runtime_write_plan") : TEXT("read_or_validate"));
		OutStructured->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));

		FWorldAiDataUiSpec GateSpec{Spec.Name, Spec.Description, TEXT("gameplay_movement"), TEXT("plan"), Spec.GatePlugins, Spec.GateModules, {}, {}, {}};
		FString Availability;
		ProbeAvailability(GateSpec, OutStructured, Availability);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		const FString TargetActor = MoverTargetActor(Arguments);
		const FString TargetAsset = MoverTargetAsset(Arguments);
		const FString MovementMode = MoverModeName(Arguments);
		FString ComponentName;
		Arguments->TryGetStringField(TEXT("component_name"), ComponentName);
		OutStructured->SetStringField(TEXT("target_actor"), TargetActor);
		OutStructured->SetStringField(TEXT("target_asset"), TargetAsset);
		OutStructured->SetStringField(TEXT("component_name"), ComponentName);
		OutStructured->SetStringField(TEXT("movement_mode"), MovementMode);
		OutStructured->SetArrayField(TEXT("input_actions"), StringArrayJson(GetStringArrayField(Arguments, TEXT("input_actions"))));
		FString MappingContext;
		Arguments->TryGetStringField(TEXT("input_mapping_context"), MappingContext);
		OutStructured->SetStringField(TEXT("input_mapping_context"), MappingContext);

		OutStructured->SetArrayField(TEXT("class_catalog"), MoverClassCatalogJson(Context, Spec.FocusClasses));
		OutStructured->SetObjectField(TEXT("target_actor_snapshot"), MoverActorSnapshotJson(Context, TargetActor));
		OutStructured->SetObjectField(TEXT("target_asset_summary"), CommonUiAssetSummaryJson(Context, TargetAsset));
		OutStructured->SetArrayField(TEXT("known_default_modes"), StringArrayJson({TEXT("Walking"), TEXT("Falling"), TEXT("Flying"), TEXT("Swimming"), TEXT("NavWalking"), TEXT("PathFollowing"), TEXT("PhysicsDriven"), TEXT("ChaosWalking"), TEXT("ChaosFalling")}));

		if (Spec.Mode == TEXT("receipt"))
		{
			if (Arguments->HasField(TEXT("receipt")))
			{
				return ExecuteMoverReceiptTool(Spec, Arguments, OutStructured, OutSummary, OutError);
			}
			OutStructured->SetStringField(TEXT("status"), TEXT("receipt_required"));
			OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			OutSummary = FString::Printf(TEXT("%s returned receipt requirements."), *Spec.Name);
			return true;
		}

		if (Spec.Name == TEXT("mover_trajectory_predict"))
		{
			double PredictionSeconds = 1.2;
			Arguments->TryGetNumberField(TEXT("prediction_seconds"), PredictionSeconds);
			int32 SampleCount = 8;
			Arguments->TryGetNumberField(TEXT("sample_count"), SampleCount);
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetNumberField(TEXT("prediction_seconds"), PredictionSeconds);
			Contract->SetNumberField(TEXT("sample_count"), SampleCount);
			Contract->SetStringField(TEXT("requires"), TEXT("MoverComponent or PoseSearch trajectory predictor readback before delivery."));
			Contract->SetArrayField(TEXT("sample_fields"), StringArrayJson({TEXT("time"), TEXT("position"), TEXT("facing"), TEXT("velocity"), TEXT("floor_hit")}));
			OutStructured->SetObjectField(TEXT("trajectory_contract"), Contract);
		}

		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));

		if (bExecute && Spec.bMutation)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_mover_writer"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_mover_writer_after_live_fixture_and_actor_lock"));
			OutError = FString::Printf(TEXT("%s is a concrete Mover plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			OutSummary = OutError;
			return false;
		}

		const bool bCompletedRead = Spec.Mode == TEXT("class_catalog") || Spec.Mode == TEXT("snapshot") || Spec.Mode == TEXT("debug") || Spec.Mode == TEXT("audit") || Spec.Mode == TEXT("inspect");
		OutStructured->SetStringField(TEXT("status"), bCompletedRead ? TEXT("completed") : TEXT("dry_run"));
		OutSummary = FString::Printf(TEXT("%s returned Mover %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TArray<FMoverP1Spec> MoverP1Specs()
	{
		const TArray<FString> BaseGatePlugins{TEXT("Mover")};
		const TArray<FString> BaseGateModules{TEXT("Mover")};
		const TArray<FString> PoseGatePlugins{TEXT("Mover"), TEXT("PoseSearch")};
		const TArray<FString> PoseGateModules{TEXT("Mover"), TEXT("PoseSearch")};
		const TArray<FString> InputGatePlugins{TEXT("Mover"), TEXT("EnhancedInput")};
		const TArray<FString> InputGateModules{TEXT("Mover"), TEXT("EnhancedInput")};
		const TArray<FString> BaseFocus{TEXT("MoverComponent"), TEXT("CharacterMoverComponent"), TEXT("NavMoverComponent"), TEXT("BaseMovementMode"), TEXT("MovementModeStateMachine")};
		const TArray<FString> PoseFocus{TEXT("MoverPoseSearchTrajectoryPredictor"), TEXT("PoseSearchDatabase"), TEXT("PoseSearchSchema"), TEXT("PoseSearchTrajectoryLibrary")};
		const TArray<FString> InputFocus{TEXT("InputAction"), TEXT("InputMappingContext")};
		const TArray<FString> ChaosFocus{TEXT("ChaosCharacterMoverComponent"), TEXT("ChaosMoverSimulation"), TEXT("ChaosWalkingMode"), TEXT("ChaosFallingMode"), TEXT("ChaosFlyingMode"), TEXT("ChaosSwimmingMode")};
		const TArray<FString> ComponentReq{TEXT("target actor or Blueprint"), TEXT("Mover component readback"), TEXT("mode/config validation receipt")};
		const TArray<FString> ModeReq{TEXT("target actor/component"), TEXT("movement mode readback"), TEXT("debug snapshot or validate_ok")};
		const TArray<FString> PredictReq{TEXT("target Mover component"), TEXT("trajectory sample output"), TEXT("PoseSearch predictor compatibility when used")};
		return {
			{TEXT("mover_component_attach"), TEXT("Plan MoverComponent attachment to an actor or Blueprint with actor lock/readback gates."), TEXT("plan"), TEXT("component"), true, BaseGatePlugins, BaseGateModules, BaseFocus, {TEXT("Resolve target actor or Blueprint."), TEXT("Resolve UMoverComponent class."), TEXT("Plan component add and ownership lock."), TEXT("Require component readback and movement-mode snapshot.")}, ComponentReq},
			{TEXT("mover_character_component_setup"), TEXT("Plan CharacterMoverComponent setup for controllable characters."), TEXT("plan"), TEXT("component"), true, BaseGatePlugins, BaseGateModules, BaseFocus, {TEXT("Resolve character Blueprint/actor."), TEXT("Select CharacterMoverComponent defaults."), TEXT("Plan collision, updated component, and input producer binding."), TEXT("Require component and mode readback.")}, ComponentReq},
			{TEXT("mover_nav_component_setup"), TEXT("Plan NavMoverComponent setup and navigation handoff."), TEXT("plan"), TEXT("nav"), true, BaseGatePlugins, BaseGateModules, {TEXT("NavMoverComponent"), TEXT("CharacterMoverComponent"), TEXT("MoverComponent")}, {TEXT("Resolve actor/Blueprint and nav movement consumer."), TEXT("Plan NavMoverComponent add."), TEXT("Bind requested velocity/path following handoff."), TEXT("Require nav/debug readback.")}, ComponentReq},
			{TEXT("mover_modes_list"), TEXT("List reflected Mover movement mode classes and version gates."), TEXT("class_catalog"), TEXT("mode"), false, BaseGatePlugins, BaseGateModules, {}, {}, {TEXT("class availability"), TEXT("plugin/module gate")}},
			{TEXT("mover_mode_set"), TEXT("Plan current/default Mover movement mode assignment."), TEXT("plan"), TEXT("mode"), true, BaseGatePlugins, BaseGateModules, BaseFocus, {TEXT("Resolve target Mover component."), TEXT("Resolve movement mode object/class."), TEXT("Plan mode set and rollback."), TEXT("Require current-mode readback.")}, ModeReq},
			{TEXT("mover_mode_config_get"), TEXT("Inspect Mover movement mode/config readiness and target snapshot."), TEXT("inspect"), TEXT("mode"), false, BaseGatePlugins, BaseGateModules, BaseFocus, {}, ModeReq},
			{TEXT("mover_mode_config_set"), TEXT("Plan Mover movement mode/config property patch."), TEXT("plan"), TEXT("mode"), true, BaseGatePlugins, BaseGateModules, BaseFocus, {TEXT("Resolve mode config object."), TEXT("Validate editable properties."), TEXT("Plan scoped property patch."), TEXT("Require post-patch readback.")}, ModeReq},
			{TEXT("mover_layered_move_queue"), TEXT("Plan Mover layered move queue insertion and rollback evidence."), TEXT("plan"), TEXT("layered_move"), true, BaseGatePlugins, BaseGateModules, {TEXT("LayeredMoveLogic"), TEXT("MoverComponent")}, {TEXT("Resolve Mover component."), TEXT("Resolve layered move logic class."), TEXT("Plan queue duration and priority."), TEXT("Require debug snapshot/readback.")}, {TEXT("target component"), TEXT("layered move payload"), TEXT("queue readback")}},
			{TEXT("mover_instant_effect_queue"), TEXT("Plan Mover instant movement effect queue insertion."), TEXT("plan"), TEXT("instant_effect"), true, BaseGatePlugins, BaseGateModules, {TEXT("MoverComponent"), TEXT("MoverSimulation")}, {TEXT("Resolve Mover component."), TEXT("Define velocity/teleport/launch effect payload."), TEXT("Plan execution timing."), TEXT("Require post-effect state snapshot.")}, {TEXT("target component"), TEXT("effect payload"), TEXT("state readback")}},
			{TEXT("mover_blackboard_inspect"), TEXT("Inspect Mover blackboard class and target component snapshot."), TEXT("inspect"), TEXT("blackboard"), false, BaseGatePlugins, BaseGateModules, {TEXT("MoverBlackboard"), TEXT("MoverComponent")}, {}, {TEXT("blackboard class availability"), TEXT("target snapshot when provided")}},
			{TEXT("mover_debug_snapshot"), TEXT("Read Mover debug snapshot contract for actor/component state."), TEXT("debug"), TEXT("debug"), false, BaseGatePlugins, BaseGateModules, BaseFocus, {TEXT("Collect target actor component inventory."), TEXT("Attach current mode, blackboard, and prediction data when available.")}, {TEXT("target actor/component"), TEXT("component snapshot")}},
			{TEXT("mover_trajectory_predict"), TEXT("Plan/read Mover trajectory prediction contract for gameplay and motion matching."), TEXT("plan"), TEXT("trajectory"), false, PoseGatePlugins, PoseGateModules, PoseFocus, {TEXT("Resolve Mover component and optional PoseSearch predictor."), TEXT("Choose prediction horizon and samples."), TEXT("Generate trajectory preview through dedicated runtime writer when available."), TEXT("Require trajectory readback.")}, PredictReq},
			{TEXT("mover_nav_avoidance_set"), TEXT("Plan NavMover avoidance/RVO configuration."), TEXT("plan"), TEXT("nav"), true, BaseGatePlugins, BaseGateModules, {TEXT("NavMoverComponent"), TEXT("MoverComponent")}, {TEXT("Resolve NavMover component."), TEXT("Plan avoidance flags/groups/radius."), TEXT("Check navigation consumer compatibility."), TEXT("Require nav readback.")}, {TEXT("nav mover component"), TEXT("avoidance settings"), TEXT("readback")}},
			{TEXT("mover_receipt_validate"), TEXT("Validate Mover production receipt before delivery."), TEXT("receipt"), TEXT("receipt"), false, BaseGatePlugins, BaseGateModules, BaseFocus, {}, {TEXT("target actor/asset"), TEXT("validate_ok or component_readback"), TEXT("debug snapshot or movement mode proof")}},
			{TEXT("mover_pose_search_trajectory_predictor_attach"), TEXT("Plan Mover PoseSearch trajectory predictor attachment."), TEXT("plan"), TEXT("pose_search"), true, PoseGatePlugins, PoseGateModules, PoseFocus, {TEXT("Resolve Mover component."), TEXT("Resolve MoverPoseSearchTrajectoryPredictor class."), TEXT("Bind predictor to motion matching graph/input."), TEXT("Require trajectory preview receipt.")}, PredictReq},
			{TEXT("mover_root_motion_toggle_plan"), TEXT("Plan root-motion toggling around Mover simulation/update order."), TEXT("plan"), TEXT("animation"), true, PoseGatePlugins, PoseGateModules, PoseFocus, {TEXT("Resolve animation graph or AnimNext/Mover bridge."), TEXT("Plan root-motion enable/disable timing."), TEXT("Check Mover tick dependency."), TEXT("Require runtime state and animation preview receipt.")}, {TEXT("target animation graph"), TEXT("Mover component"), TEXT("root-motion state readback")}},
			{TEXT("mover_motion_matching_setup_plan"), TEXT("Plan Mover plus PoseSearch motion-matching setup."), TEXT("plan"), TEXT("pose_search"), true, PoseGatePlugins, PoseGateModules, PoseFocus, {TEXT("Resolve skeleton, animation set, and PoseSearch database."), TEXT("Bind Mover trajectory predictor."), TEXT("Plan AnimBP/motion matching node handoff."), TEXT("Require database/preview receipts.")}, {TEXT("skeleton"), TEXT("PoseSearch database"), TEXT("Mover trajectory predictor"), TEXT("preview receipt")}},
			{TEXT("mover_component_state_snapshot"), TEXT("Read Mover component state snapshot contract."), TEXT("snapshot"), TEXT("component"), false, BaseGatePlugins, BaseGateModules, BaseFocus, {TEXT("Inspect actor components."), TEXT("Report Mover-like component count and class paths.")}, {TEXT("target actor when validating production actor")}},
			{TEXT("mover_input_mapping_plan"), TEXT("Plan Enhanced Input mapping for Mover-controlled actors."), TEXT("plan"), TEXT("input"), true, InputGatePlugins, InputGateModules, InputFocus, {TEXT("Resolve InputAction and InputMappingContext assets."), TEXT("Map move/look/jump/crouch actions to Mover input producer."), TEXT("Check platform/context priority conflicts."), TEXT("Require input smoke receipt.")}, {TEXT("input mapping context"), TEXT("input action list"), TEXT("receiver actor/Blueprint"), TEXT("input smoke receipt")}},
			{TEXT("mover_network_prediction_audit"), TEXT("Audit Mover network prediction/physics liaison readiness."), TEXT("audit"), TEXT("network"), false, BaseGatePlugins, BaseGateModules, {TEXT("MoverNetworkPredictionLiaisonComponent"), TEXT("MoverStandaloneLiaisonComponent"), TEXT("MoverComponent")}, {TEXT("Inspect component/backend class availability."), TEXT("Check target actor Mover-like components."), TEXT("Recommend standalone/network/physics backend receipt gate.")}, {TEXT("backend class availability"), TEXT("target component snapshot")}},
			{TEXT("mover_floor_query_debug"), TEXT("Read Mover floor query/debug utility availability and target snapshot."), TEXT("debug"), TEXT("floor"), false, BaseGatePlugins, BaseGateModules, {TEXT("MoverComponent"), TEXT("CharacterMoverComponent"), TEXT("ChaosCharacterMoverComponent")}, {TEXT("Resolve floor query utility class through Mover/ChaosMover reflection."), TEXT("Capture target component/floor debug contract."), TEXT("Require floor hit sample before delivery.")}, {TEXT("target component"), TEXT("floor hit sample or debug snapshot")}},
			{TEXT("mover_movement_mode_receipt"), TEXT("Validate Mover movement-mode readback receipt."), TEXT("receipt"), TEXT("receipt"), false, BaseGatePlugins, BaseGateModules, BaseFocus, {}, {TEXT("target actor/asset"), TEXT("movement_mode_ok or mode_readback"), TEXT("current mode evidence")}}
		};
	}

	static void RegisterMoverP1Tools(FSololmcpToolRegistry& Registry)
	{
		for (const FMoverP1Spec& Spec : MoverP1Specs())
		{
			FSololmcpToolDefinition Def;
			Def.Name = Spec.Name;
			Def.Description = Spec.Description;
			Def.InputSchema = MoverP1Schema();
			Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
			Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return ExecuteMoverP1Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
			};
			Registry.Register(Def);
		}
	}

	struct FUafCatalogRow
	{
		FString Id;
		FString ObjectPath;
		FString Kind;
		FString Category;
	};

	struct FUafAnimNextP1Spec
	{
		FString Name;
		FString Description;
		FString Mode;
		FString Subdomain;
		bool bMutation = false;
		TArray<FString> GatePlugins;
		TArray<FString> GateModules;
		TArray<FString> FocusIds;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
	};

	static TSharedRef<FJsonObject> UafAnimNextSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target UAF/AnimNext graph, shared variables, chooser, PoseSearch, or animation asset path."))},
			{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for target UAF/AnimNext graph asset path."))},
			{TEXT("animation_graph"), FSololmcpSchemaBuilder::String(TEXT("Alias for target UAF animation graph asset path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Target actor label/name for UAF component/runtime state plans."))},
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for target_actor."))},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Target UAF/AnimNext component name."))},
			{TEXT("entry_name"), FSololmcpSchemaBuilder::String(TEXT("AnimNext graph entry name."))},
			{TEXT("variable_name"), FSololmcpSchemaBuilder::String(TEXT("UAF/AnimNext variable name."))},
			{TEXT("trait_name"), FSololmcpSchemaBuilder::String(TEXT("UAF trait or RigVM unit name."))},
			{TEXT("template_name"), FSololmcpSchemaBuilder::String(TEXT("UAF graph template name."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Animation, skeleton, chooser, PoseSearch, Mover, or template dependency assets."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("UAF/AnimNext validation/readback receipt to validate."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation. P1 UAF/AnimNext tools fail closed unless promoted to a dedicated writer."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return probe/plan only. Default true."))}
		});
	}

	static FString UafTargetActor(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Value;
		for (const TCHAR* Field : {TEXT("target_actor"), TEXT("actor")})
		{
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				return Value;
			}
		}
		return FString();
	}

	static FString UafTargetAsset(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Value;
		for (const TCHAR* Field : {TEXT("target_asset"), TEXT("graph_path"), TEXT("animation_graph"), TEXT("asset_path"), TEXT("target")})
		{
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				return Value;
			}
		}
		return FString();
	}

	static TArray<FUafCatalogRow> UafCatalog()
	{
		return {
			{TEXT("UAFComponent"), TEXT("/Script/UAF.UAFComponent"), TEXT("class"), TEXT("runtime")},
			{TEXT("UAFRigVMAsset"), TEXT("/Script/UAF.UAFRigVMAsset"), TEXT("class"), TEXT("asset")},
			{TEXT("UAFSharedVariables"), TEXT("/Script/UAF.UAFSharedVariables"), TEXT("class"), TEXT("variables")},
			{TEXT("UAFSystem"), TEXT("/Script/UAF.UAFSystem"), TEXT("class"), TEXT("system")},
			{TEXT("AnimNextConfig"), TEXT("/Script/UAF.AnimNextConfig"), TEXT("class"), TEXT("settings")},
			{TEXT("UAFEngineSubsystem"), TEXT("/Script/UAF.UAFEngineSubsystem"), TEXT("class"), TEXT("runtime")},
			{TEXT("AnimNextWorldLibrary"), TEXT("/Script/UAF.AnimNextWorldLibrary"), TEXT("class"), TEXT("runtime")},
			{TEXT("UAFSkeletonUserData"), TEXT("/Script/UAF.UAFSkeletonUserData"), TEXT("class"), TEXT("skeleton")},
			{TEXT("UAFAnimGraph"), TEXT("/Script/UAFAnimGraph.UAFAnimGraph"), TEXT("class"), TEXT("graph")},
			{TEXT("UAFInjectionLibrary"), TEXT("/Script/UAFAnimGraph.UAFInjectionLibrary"), TEXT("class"), TEXT("injection")},
			{TEXT("InjectionCallbackProxy"), TEXT("/Script/UAFAnimGraph.InjectionCallbackProxy"), TEXT("class"), TEXT("injection")},
			{TEXT("PlayAnimCallbackProxy"), TEXT("/Script/UAFAnimGraph.PlayAnimCallbackProxy"), TEXT("class"), TEXT("play_anim")},
			{TEXT("AnimNextSkeletalMeshComponentLibrary"), TEXT("/Script/UAFAnimGraph.AnimNextSkeletalMeshComponentLibrary"), TEXT("class"), TEXT("runtime")},
			{TEXT("UAFGraphNodeTemplate"), TEXT("/Script/UAFAnimGraphUncookedOnly.UAFGraphNodeTemplate"), TEXT("class"), TEXT("template")},
			{TEXT("UAFGraphNodeTemplate_SequencePlayer"), TEXT("/Script/UAFAnimGraphUncookedOnly.UAFGraphNodeTemplate_SequencePlayer"), TEXT("class"), TEXT("template")},
			{TEXT("UAFGraphNodeTemplate_BlendSpacePlayer"), TEXT("/Script/UAFAnimGraphUncookedOnly.UAFGraphNodeTemplate_BlendSpacePlayer"), TEXT("class"), TEXT("template")},
			{TEXT("UAFGraphNodeTemplate_BlendByBool"), TEXT("/Script/UAFAnimGraphUncookedOnly.UAFGraphNodeTemplate_BlendByBool"), TEXT("class"), TEXT("template")},
			{TEXT("UAFGraphNodeTemplate_InputValue"), TEXT("/Script/UAFAnimGraphUncookedOnly.UAFGraphNodeTemplate_InputValue"), TEXT("class"), TEXT("template")},
			{TEXT("UAFGraphNodeTemplate_InjectionSite"), TEXT("/Script/UAFAnimGraphUncookedOnly.UAFGraphNodeTemplate_InjectionSite"), TEXT("class"), TEXT("template")},
			{TEXT("K2Node_AnimNextPlayAnim"), TEXT("/Script/UAFAnimGraphUncookedOnly.K2Node_AnimNextPlayAnim"), TEXT("class"), TEXT("blueprint")},
			{TEXT("K2Node_AnimNextComponentSetVariable"), TEXT("/Script/UAFUncookedOnly.K2Node_AnimNextComponentSetVariable"), TEXT("class"), TEXT("blueprint")},
			{TEXT("K2Node_UAFComponentGetVariable"), TEXT("/Script/UAFUncookedOnly.K2Node_UAFComponentGetVariable"), TEXT("class"), TEXT("blueprint")},
			{TEXT("K2Node_UAFComponentSetInputBinding"), TEXT("/Script/UAFUncookedOnly.K2Node_UAFComponentSetInputBinding"), TEXT("class"), TEXT("blueprint")},
			{TEXT("PoseSearchDatabase"), TEXT("/Script/PoseSearch.PoseSearchDatabase"), TEXT("class"), TEXT("pose_search")},
			{TEXT("PoseSearchSchema"), TEXT("/Script/PoseSearch.PoseSearchSchema"), TEXT("class"), TEXT("pose_search")},
			{TEXT("PoseSearchTrajectoryLibrary"), TEXT("/Script/PoseSearch.PoseSearchTrajectoryLibrary"), TEXT("class"), TEXT("pose_search")},
			{TEXT("UAFGraphNodeTemplate_MotionMatching"), TEXT("/Script/UAFPoseSearchUncookedOnly.UAFGraphNodeTemplate_MotionMatching"), TEXT("class"), TEXT("pose_search")},
			{TEXT("UNotifyState_PoseSearchSteerAlongTrajectory"), TEXT("/Script/UAFPoseSearch.UNotifyState_PoseSearchSteerAlongTrajectory"), TEXT("class"), TEXT("pose_search")},
			{TEXT("UPoseSearchFeatureChannel_DistanceFromAnimNextVar"), TEXT("/Script/UAFPoseSearch.PoseSearchFeatureChannel_DistanceFromAnimNextVar"), TEXT("class"), TEXT("pose_search")},
			{TEXT("UAFLayerStack"), TEXT("/Script/UAFLayering.UAFLayerStack"), TEXT("class"), TEXT("layering")},
			{TEXT("UAFLayer"), TEXT("/Script/UAFLayeringUncookedOnly.UAFLayer"), TEXT("class"), TEXT("layering")},
			{TEXT("UAFBaseLayer"), TEXT("/Script/UAFLayeringUncookedOnly.UAFBaseLayer"), TEXT("class"), TEXT("layering")},
			{TEXT("MassUAFTrait"), TEXT("/Script/UAFMass.MassUAFTrait"), TEXT("class"), TEXT("mass")},
			{TEXT("MassUAFSubsystem"), TEXT("/Script/UAFMass.MassUAFSubsystem"), TEXT("class"), TEXT("mass")},
			{TEXT("CharacterTrajectoryUAFTrait"), TEXT("/Script/UAFMass.CharacterTrajectoryUAFTrait"), TEXT("class"), TEXT("mass")},
			{TEXT("RigUnit_GenerateMoverTrajectory"), TEXT("/Script/MoverAnimNext.RigUnit_GenerateMoverTrajectory"), TEXT("struct"), TEXT("mover")},
			{TEXT("RigUnit_MoverToggleRootMotion"), TEXT("/Script/MoverAnimNext.RigUnit_MoverToggleRootMotion"), TEXT("struct"), TEXT("mover")},
			{TEXT("RigVMTrait_ModuleEventDependency_MoverComponentTickFunctions"), TEXT("/Script/MoverAnimNext.RigVMTrait_ModuleEventDependency_MoverComponentTickFunctions"), TEXT("struct"), TEXT("mover")},
			{TEXT("SequencePlayerTraitData"), TEXT("/Script/UAFAnimGraph.SequencePlayerTraitData"), TEXT("struct"), TEXT("trait")},
			{TEXT("InlineSubGraphTraitData"), TEXT("/Script/UAFAnimGraph.InlineSubGraphTraitData"), TEXT("struct"), TEXT("trait")},
			{TEXT("NotifyDispatcherTraitData"), TEXT("/Script/UAFAnimGraph.NotifyDispatcherTraitData"), TEXT("struct"), TEXT("trait")},
			{TEXT("StrafeWarpingTrait"), TEXT("/Script/UAFWarping.StrafeWarpingTrait"), TEXT("struct"), TEXT("warping")},
			{TEXT("OffsetRootBoneTrait"), TEXT("/Script/UAFWarping.OffsetRootBoneTrait"), TEXT("struct"), TEXT("warping")},
			{TEXT("OverrideRootMotionTrait"), TEXT("/Script/UAFWarping.OverrideRootMotionTrait"), TEXT("struct"), TEXT("warping")},
			{TEXT("RigUnit_UAFIKRetargeter"), TEXT("/Script/IKRigUAF.RigUnit_UAFIKRetargeter"), TEXT("struct"), TEXT("retarget")}
		};
	}

	static TSharedRef<FJsonObject> UafCatalogStatusJson(const FSololmcpToolExecutionContext& Context, const FUafCatalogRow& Row)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Row.Id);
		Obj->SetStringField(TEXT("objectPath"), Row.ObjectPath);
		Obj->SetStringField(TEXT("kind"), Row.Kind);
		Obj->SetStringField(TEXT("category"), Row.Category);
		if (Row.Kind == TEXT("struct"))
		{
			UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *Row.ObjectPath);
			if (!Struct)
			{
				Struct = LoadObject<UScriptStruct>(nullptr, *Row.ObjectPath);
			}
			Obj->SetBoolField(TEXT("available"), Struct != nullptr);
			if (Struct)
			{
				Obj->SetStringField(TEXT("resolvedStruct"), Struct->GetPathName());
				int32 PropertyCount = 0;
				for (TFieldIterator<FProperty> It(Struct); It; ++It)
				{
					++PropertyCount;
				}
				Obj->SetNumberField(TEXT("propertyCount"), PropertyCount);
			}
			return Obj;
		}

		return CommonUiClassStatusJson(Context, Row.Id, Row.ObjectPath);
	}

	static TArray<TSharedPtr<FJsonValue>> UafCatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusIds = {})
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FUafCatalogRow& Row : UafCatalog())
		{
			if (!FocusIds.IsEmpty() && !FocusIds.Contains(Row.Id) && !FocusIds.Contains(Row.Category))
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(UafCatalogStatusJson(Context, Row)));
		}
		return Rows;
	}

	static bool ExecuteUafReceiptTool(
		const FUafAnimNextP1Spec& Spec,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_missing_receipt"));
			OutError = TEXT("UAF/AnimNext receipt validation requires a receipt object.");
			return false;
		}

		const TSharedRef<FJsonObject> Receipt = ReceiptPtr->ToSharedRef();
		TArray<TSharedPtr<FJsonValue>> Checks;
		bool bValid = true;
		const bool bHasTarget = CommonUiReceiptHasAny(Receipt, {TEXT("target_asset"), TEXT("graph_path"), TEXT("animation_graph"), TEXT("target_actor"), TEXT("component_path"), TEXT("asset_path")});
		AddCommonUiCheck(Checks, bValid, TEXT("target_binding"), bHasTarget, bHasTarget ? TEXT("Target asset/actor binding found.") : TEXT("Missing target asset/actor binding."));

		if (Spec.Name == TEXT("animnext_graph_compile_validate"))
		{
			const bool bCompileOk = CommonUiReceiptBool(Receipt, TEXT("compile_ok")) || CommonUiReceiptBool(Receipt, TEXT("compileOk")) || CommonUiReceiptBool(Receipt, TEXT("validate_ok"));
			AddCommonUiCheck(Checks, bValid, TEXT("compile_or_validate"), bCompileOk, bCompileOk ? TEXT("Compile/validate evidence passed.") : TEXT("Missing compile_ok or validate_ok evidence."));
		}
		else if (Spec.Name == TEXT("uaf_validation_receipt"))
		{
			const bool bValidationOk = CommonUiReceiptBool(Receipt, TEXT("validation_ok")) || CommonUiReceiptBool(Receipt, TEXT("validate_ok")) || CommonUiReceiptHasAny(Receipt, {TEXT("diagnostics"), TEXT("graph_snapshot"), TEXT("runtime_state_snapshot")});
			AddCommonUiCheck(Checks, bValid, TEXT("validation_or_snapshot"), bValidationOk, bValidationOk ? TEXT("Validation/snapshot evidence found.") : TEXT("Missing validation or snapshot evidence."));
		}
		else
		{
			const bool bReadbackOk = CommonUiReceiptBool(Receipt, TEXT("validate_ok")) || CommonUiReceiptBool(Receipt, TEXT("compile_ok")) || CommonUiReceiptBool(Receipt, TEXT("runtime_state_ok")) || CommonUiReceiptHasAny(Receipt, {TEXT("graph_snapshot"), TEXT("parameter_schema"), TEXT("trait_catalog"), TEXT("runtime_state_snapshot"), TEXT("preview_receipt")});
			AddCommonUiCheck(Checks, bValid, TEXT("readback_or_runtime_proof"), bReadbackOk, bReadbackOk ? TEXT("Readback/runtime proof found.") : TEXT("Missing UAF/AnimNext readback or runtime proof."));
		}

		OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		OutStructured->SetBoolField(TEXT("valid"), bValid);
		OutStructured->SetArrayField(TEXT("checks"), Checks);
		OutSummary = bValid ? FString::Printf(TEXT("%s passed."), *Spec.Name) : FString::Printf(TEXT("%s failed closed."), *Spec.Name);
		return bValid;
	}

	static bool ExecuteUafAnimNextP1Tool(
		const FUafAnimNextP1Spec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const bool bVersionSatisfied = IsUE58OrLater();
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("tool_name"), Spec.Name);
		OutStructured->SetStringField(TEXT("domain"), TEXT("animation"));
		OutStructured->SetStringField(TEXT("family"), TEXT("uaf_animnext_bridge"));
		OutStructured->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		OutStructured->SetStringField(TEXT("mode"), Spec.Mode);
		OutStructured->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_or_runtime_write_plan") : TEXT("read_or_validate"));
		OutStructured->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));
		OutStructured->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		OutStructured->SetStringField(TEXT("minimum_engine_version"), TEXT("5.8.0"));
		OutStructured->SetBoolField(TEXT("version_satisfied"), bVersionSatisfied);

		FWorldAiDataUiSpec GateSpec{Spec.Name, Spec.Description, TEXT("animation"), TEXT("plan"), Spec.GatePlugins, Spec.GateModules, {}, {}, {}};
		FString Availability;
		ProbeAvailability(GateSpec, OutStructured, Availability);
		if (!bVersionSatisfied)
		{
			Availability = TEXT("requires_ue_5_8");
			OutStructured->SetStringField(TEXT("status"), Availability);
			OutStructured->SetBoolField(TEXT("available"), false);
		}

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		const FString TargetAsset = UafTargetAsset(Arguments);
		const FString TargetActor = UafTargetActor(Arguments);
		FString ComponentName;
		FString EntryName;
		FString VariableName;
		FString TraitName;
		FString TemplateName;
		Arguments->TryGetStringField(TEXT("component_name"), ComponentName);
		Arguments->TryGetStringField(TEXT("entry_name"), EntryName);
		Arguments->TryGetStringField(TEXT("variable_name"), VariableName);
		Arguments->TryGetStringField(TEXT("trait_name"), TraitName);
		Arguments->TryGetStringField(TEXT("template_name"), TemplateName);
		OutStructured->SetStringField(TEXT("target_asset"), TargetAsset);
		OutStructured->SetStringField(TEXT("target_actor"), TargetActor);
		OutStructured->SetStringField(TEXT("component_name"), ComponentName);
		OutStructured->SetStringField(TEXT("entry_name"), EntryName);
		OutStructured->SetStringField(TEXT("variable_name"), VariableName);
		OutStructured->SetStringField(TEXT("trait_name"), TraitName);
		OutStructured->SetStringField(TEXT("template_name"), TemplateName);
		OutStructured->SetArrayField(TEXT("asset_paths"), StringArrayJson(GetStringArrayField(Arguments, TEXT("asset_paths"))));
		OutStructured->SetArrayField(TEXT("class_and_struct_catalog"), UafCatalogJson(Context, Spec.FocusIds));
		OutStructured->SetObjectField(TEXT("target_asset_summary"), CommonUiAssetSummaryJson(Context, TargetAsset));
		OutStructured->SetObjectField(TEXT("target_actor_snapshot"), MoverActorSnapshotJson(Context, TargetActor));
		OutStructured->SetArrayField(TEXT("related_plugins"), StringArrayJson({
			TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("UAFAnimNode"), TEXT("UAFChooser"), TEXT("UAFPoseSearch"),
			TEXT("UAFWarping"), TEXT("UAFLayering"), TEXT("UAFMass"), TEXT("MoverAnimNext"), TEXT("PoseSearch"), TEXT("Chooser")
		}));
		OutStructured->SetArrayField(TEXT("ue57_fallback_tools"), StringArrayJson({
			TEXT("chooser_table_plan"), TEXT("pose_search_motion_matching_plan"), TEXT("animbp_locomotion_plan"),
			TEXT("mover_motion_matching_setup_plan"), TEXT("animation_asset_compat_diagnose")
		}));

		if (!bVersionSatisfied)
		{
			OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson({TEXT("Do not call UE 5.8 UAF/AnimNext interfaces on UE 5.7."), TEXT("Route to Chooser/PoseSearch/AnimBP fallback tools.")}));
			OutSummary = FString::Printf(TEXT("%s requires UE 5.8; current engine is %s."), *Spec.Name, *CurrentEngineVersionString());
			return true;
		}

		if (Spec.Mode == TEXT("receipt") || Spec.Mode == TEXT("compile_gate"))
		{
			if (Arguments->HasField(TEXT("receipt")))
			{
				return ExecuteUafReceiptTool(Spec, Arguments, OutStructured, OutSummary, OutError);
			}
			OutStructured->SetStringField(TEXT("status"), TEXT("receipt_required"));
			OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			OutSummary = FString::Printf(TEXT("%s returned receipt requirements."), *Spec.Name);
			return true;
		}

		if (Spec.Name == TEXT("animnext_parameter_schema_get"))
		{
			OutStructured->SetArrayField(TEXT("parameter_schema_fields"), StringArrayJson({
				TEXT("variable_name"), TEXT("value_type"), TEXT("binding_source"), TEXT("default_value"), TEXT("target_graph_entry"), TEXT("readback_required")
			}));
		}
		if (Spec.Name == TEXT("mover_animnext_trajectory_bridge_plan"))
		{
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetStringField(TEXT("trajectory_struct"), TEXT("/Script/MoverAnimNext.RigUnit_GenerateMoverTrajectory"));
			Contract->SetStringField(TEXT("root_motion_toggle_struct"), TEXT("/Script/MoverAnimNext.RigUnit_MoverToggleRootMotion"));
			Contract->SetArrayField(TEXT("required_receipts"), StringArrayJson({TEXT("Mover component readback"), TEXT("trajectory preview"), TEXT("UAF graph compile"), TEXT("runtime state snapshot")}));
			OutStructured->SetObjectField(TEXT("mover_animnext_contract"), Contract);
		}

		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));

		if (bExecute && Spec.bMutation)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_uaf_animnext_writer"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_uaf_animnext_writer_after_live_fixture_and_compile_receipt"));
			OutError = FString::Printf(TEXT("%s is a concrete UAF/AnimNext plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			OutSummary = OutError;
			return false;
		}

		const bool bCompletedRead = Spec.Mode == TEXT("inventory") || Spec.Mode == TEXT("class_catalog") || Spec.Mode == TEXT("snapshot") || Spec.Mode == TEXT("schema") || Spec.Mode == TEXT("audit") || Spec.Mode == TEXT("inspect");
		OutStructured->SetStringField(TEXT("status"), bCompletedRead ? TEXT("completed") : TEXT("dry_run"));
		OutSummary = FString::Printf(TEXT("%s returned UAF/AnimNext %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TArray<FUafAnimNextP1Spec> UafAnimNextP1Specs()
	{
		const TArray<FString> BasePlugins{TEXT("UAF"), TEXT("UAFAnimGraph")};
		const TArray<FString> BaseModules{TEXT("UAF"), TEXT("UAFAnimGraph")};
		const TArray<FString> MoverBridgePlugins{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("Mover"), TEXT("MoverAnimNext")};
		const TArray<FString> MoverBridgeModules{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("Mover"), TEXT("MoverAnimNext")};
		const TArray<FString> PosePlugins{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("UAFPoseSearch"), TEXT("PoseSearch")};
		const TArray<FString> PoseModules{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("UAFPoseSearch"), TEXT("PoseSearch")};
		const TArray<FString> WarpPlugins{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("UAFWarping")};
		const TArray<FString> WarpModules{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("UAFWarping")};
		const TArray<FString> LayerPlugins{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("UAFLayering")};
		const TArray<FString> LayerModules{TEXT("UAF"), TEXT("UAFAnimGraph"), TEXT("UAFLayering")};
		const TArray<FString> BaseFocus{TEXT("UAFComponent"), TEXT("UAFRigVMAsset"), TEXT("UAFSharedVariables"), TEXT("UAFAnimGraph"), TEXT("UAFInjectionLibrary"), TEXT("PlayAnimCallbackProxy")};
		const TArray<FString> GraphFocus{TEXT("graph"), TEXT("template"), TEXT("variables")};
		const TArray<FString> PoseFocus{TEXT("pose_search")};
		const TArray<FString> MoverFocus{TEXT("mover"), TEXT("UAFComponent"), TEXT("UAFAnimGraph")};
		const TArray<FString> CompileReq{TEXT("target UAF/AnimNext graph"), TEXT("compile_ok or validate_ok"), TEXT("diagnostics/readback")};
		const TArray<FString> RuntimeReq{TEXT("target actor/component"), TEXT("runtime state snapshot"), TEXT("variable or graph readback")};
		const TArray<FString> GraphReq{TEXT("target graph"), TEXT("entry/trait/template readback"), TEXT("compile or validation receipt")};
		return {
			{TEXT("mover_animnext_trajectory_bridge_plan"), TEXT("Plan UE 5.8 Mover-to-AnimNext trajectory and root-motion bridge."), TEXT("plan"), TEXT("mover_bridge"), true, MoverBridgePlugins, MoverBridgeModules, MoverFocus, {TEXT("Resolve Mover component and UAF graph target."), TEXT("Use MoverAnimNext trajectory/root-motion RigVM units."), TEXT("Plan tick dependency and root-motion order."), TEXT("Require graph compile, trajectory preview, and runtime snapshot.")}, {TEXT("Mover component"), TEXT("UAF graph"), TEXT("trajectory preview"), TEXT("runtime snapshot")}},
			{TEXT("animnext_graph_entries_list"), TEXT("List/read AnimNext graph entry and template class availability."), TEXT("class_catalog"), TEXT("graph"), false, BasePlugins, BaseModules, GraphFocus, {}, {TEXT("graph class availability"), TEXT("plugin/module gate")}},
			{TEXT("animnext_graph_entry_add"), TEXT("Plan AnimNext graph entry insertion."), TEXT("plan"), TEXT("graph"), true, BasePlugins, BaseModules, GraphFocus, {TEXT("Resolve target AnimNext graph asset."), TEXT("Resolve entry name and category."), TEXT("Plan graph entry insertion and rollback."), TEXT("Require entry readback and compile receipt.")}, GraphReq},
			{TEXT("uaf_capability_inventory"), TEXT("Inventory UE 5.8 UAF/AnimNext plugin, module, class, and struct gates."), TEXT("inventory"), TEXT("capability"), false, BasePlugins, BaseModules, {}, {}, {TEXT("version gate"), TEXT("plugin/module/class catalog")}},
			{TEXT("uaf_component_attach"), TEXT("Plan UAF component attachment to an actor or Blueprint."), TEXT("plan"), TEXT("component"), true, BasePlugins, BaseModules, BaseFocus, {TEXT("Resolve actor or Blueprint target."), TEXT("Resolve UAFComponent class."), TEXT("Plan component add and graph binding."), TEXT("Require component readback and runtime state snapshot.")}, RuntimeReq},
			{TEXT("uaf_variable_set"), TEXT("Plan UAF/AnimNext variable binding or value update."), TEXT("plan"), TEXT("variables"), true, BasePlugins, BaseModules, {TEXT("variables"), TEXT("blueprint")}, {TEXT("Resolve variable entry or shared variable asset."), TEXT("Validate variable type/value source."), TEXT("Plan scoped variable write."), TEXT("Require variable readback and compile/runtime receipt.")}, {TEXT("variable name"), TEXT("type/value"), TEXT("readback"), TEXT("compile or runtime receipt")}},
			{TEXT("uaf_injection_request"), TEXT("Plan UAF injection request/callback wiring."), TEXT("plan"), TEXT("injection"), true, BasePlugins, BaseModules, {TEXT("injection"), TEXT("blueprint")}, {TEXT("Resolve injection site/template."), TEXT("Plan callback proxy or graph injection route."), TEXT("Bind variables and payload asset."), TEXT("Require callback/runtime receipt.")}, {TEXT("injection site"), TEXT("payload"), TEXT("callback/runtime receipt")}},
			{TEXT("uaf_play_anim_request"), TEXT("Plan UAF play-animation request."), TEXT("plan"), TEXT("play_anim"), true, BasePlugins, BaseModules, {TEXT("play_anim"), TEXT("template")}, {TEXT("Resolve animation asset and UAF component."), TEXT("Choose play/callback proxy route."), TEXT("Plan blend/interrupt policy."), TEXT("Require runtime preview receipt.")}, {TEXT("animation asset"), TEXT("UAF component"), TEXT("runtime preview receipt")}},
			{TEXT("uaf_graph_template_apply"), TEXT("Plan UAF graph template application."), TEXT("plan"), TEXT("template"), true, BasePlugins, BaseModules, {TEXT("template"), TEXT("graph")}, {TEXT("Resolve template class and target graph."), TEXT("Plan template node/trait insertion."), TEXT("Bind source animation assets."), TEXT("Require graph readback and compile receipt.")}, GraphReq},
			{TEXT("uaf_warping_trait_plan"), TEXT("Plan UAF warping trait setup."), TEXT("plan"), TEXT("warping"), true, WarpPlugins, WarpModules, {TEXT("warping"), TEXT("graph")}, {TEXT("Resolve UAFWarping plugin gates."), TEXT("Select strafe/offset/root-motion warping trait."), TEXT("Plan target variables and root-motion interaction."), TEXT("Require graph compile and preview receipt.")}, {TEXT("warping trait"), TEXT("target graph"), TEXT("preview receipt")}},
			{TEXT("uaf_layering_setup_plan"), TEXT("Plan UAF layering and montage layer setup."), TEXT("plan"), TEXT("layering"), true, LayerPlugins, LayerModules, {TEXT("layering"), TEXT("graph")}, {TEXT("Resolve UAFLayering plugin gates."), TEXT("Define layer stack and blend policy."), TEXT("Bind montage/content providers."), TEXT("Require layer stack readback and preview receipt.")}, {TEXT("layer stack"), TEXT("blend policy"), TEXT("preview receipt")}},
			{TEXT("uaf_pose_search_bridge_plan"), TEXT("Plan UAF PoseSearch/motion-matching bridge."), TEXT("plan"), TEXT("pose_search"), true, PosePlugins, PoseModules, PoseFocus, {TEXT("Resolve PoseSearch database/schema."), TEXT("Resolve UAF motion-matching template."), TEXT("Plan trajectory/history inputs."), TEXT("Require database, graph compile, and preview receipts.")}, {TEXT("PoseSearch database"), TEXT("UAF graph"), TEXT("trajectory/history input"), TEXT("preview receipt")}},
			{TEXT("uaf_validation_receipt"), TEXT("Validate UAF/AnimNext validation receipt."), TEXT("receipt"), TEXT("receipt"), false, BasePlugins, BaseModules, BaseFocus, {}, {TEXT("target binding"), TEXT("validation_ok or graph/runtime snapshot"), TEXT("diagnostics")}},
			{TEXT("animnext_graph_compile_validate"), TEXT("Validate AnimNext graph compile receipt or return compile requirements."), TEXT("compile_gate"), TEXT("graph"), false, BasePlugins, BaseModules, GraphFocus, {}, CompileReq},
			{TEXT("animnext_parameter_schema_get"), TEXT("Read/plan AnimNext parameter and variable schema."), TEXT("schema"), TEXT("parameters"), false, BasePlugins, BaseModules, {TEXT("variables"), TEXT("graph")}, {TEXT("Resolve graph/shared variables target."), TEXT("Collect variable names/types/bindings."), TEXT("Return schema required by client-side schema repair.")}, {TEXT("parameter schema"), TEXT("target graph or shared variables")}},
			{TEXT("animnext_variable_set_plan"), TEXT("Plan AnimNext variable update."), TEXT("plan"), TEXT("variables"), true, BasePlugins, BaseModules, {TEXT("variables"), TEXT("blueprint")}, {TEXT("Resolve variable target."), TEXT("Validate type and binding source."), TEXT("Plan write/readback route."), TEXT("Require compile or runtime proof.")}, {TEXT("variable name"), TEXT("type/value"), TEXT("readback receipt")}},
			{TEXT("animnext_pose_graph_snapshot"), TEXT("Read AnimNext pose graph snapshot contract."), TEXT("snapshot"), TEXT("graph"), false, BasePlugins, BaseModules, GraphFocus, {TEXT("Inspect target graph asset summary."), TEXT("Return class/template catalog and receipt requirements.")}, {TEXT("target graph when validating production graph")}},
			{TEXT("uaf_animation_asset_bind"), TEXT("Plan animation asset binding into UAF graph/component."), TEXT("plan"), TEXT("asset_binding"), true, BasePlugins, BaseModules, {TEXT("play_anim"), TEXT("template"), TEXT("graph")}, {TEXT("Resolve animation and skeleton compatibility."), TEXT("Bind asset to graph/template variable."), TEXT("Plan preview and rollback."), TEXT("Require asset compatibility and preview receipt.")}, {TEXT("animation asset"), TEXT("skeleton compatibility"), TEXT("preview receipt")}},
			{TEXT("uaf_runtime_state_snapshot"), TEXT("Read UAF runtime state snapshot contract."), TEXT("snapshot"), TEXT("runtime"), false, BasePlugins, BaseModules, BaseFocus, {TEXT("Inspect actor components."), TEXT("Report UAF/Mover-like component inventory."), TEXT("Return runtime readback contract.")}, RuntimeReq},
			{TEXT("uaf_trait_catalog"), TEXT("Read UAF trait/template class and struct catalog."), TEXT("class_catalog"), TEXT("trait"), false, BasePlugins, BaseModules, {TEXT("trait"), TEXT("template"), TEXT("warping"), TEXT("layering"), TEXT("pose_search"), TEXT("mover")}, {}, {TEXT("trait catalog"), TEXT("plugin/module gate")}},
			{TEXT("uaf_trait_apply_plan"), TEXT("Plan UAF trait application to graph/template."), TEXT("plan"), TEXT("trait"), true, BasePlugins, BaseModules, {TEXT("trait"), TEXT("template"), TEXT("graph")}, {TEXT("Resolve trait/template class."), TEXT("Validate graph entry and variable dependencies."), TEXT("Plan trait add/update."), TEXT("Require graph readback and compile receipt.")}, GraphReq},
			{TEXT("uaf_receipt_validate"), TEXT("Validate UAF/AnimNext production receipt before delivery."), TEXT("receipt"), TEXT("receipt"), false, BasePlugins, BaseModules, BaseFocus, {}, {TEXT("target binding"), TEXT("compile/validate/readback"), TEXT("runtime or preview proof")}}
		};
	}

	static void RegisterUafAnimNextP1Tools(FSololmcpToolRegistry& Registry)
	{
		for (const FUafAnimNextP1Spec& Spec : UafAnimNextP1Specs())
		{
			FSololmcpToolDefinition Def;
			Def.Name = Spec.Name;
			Def.Description = Spec.Description;
			Def.InputSchema = UafAnimNextSchema();
			Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
			Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return ExecuteUafAnimNextP1Tool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
			};
			Registry.Register(Def);
		}
	}

	static TArray<FWorldAiDataUiSpec> Specs()
	{
		return {
			{TEXT("chooser_asset_probe"), TEXT("Scan content for Chooser table/assets for animation and gameplay selection routing."), TEXT("animation"), TEXT("class_probe"), {TEXT("Chooser")}, {TEXT("Chooser")}, {TEXT("Chooser")}, {}, {}},
			{TEXT("chooser_table_plan"), TEXT("Plan Chooser table authoring for role, weapon, locomotion, animation, or gameplay selection."), TEXT("animation"), TEXT("plan"), {TEXT("Chooser")}, {TEXT("Chooser")}, {}, {TEXT("Resolve chooser context objects and result type."), TEXT("Define rows, predicates, and fallback row."), TEXT("Bind source animation/gameplay assets."), TEXT("Require table readback and runtime selection smoke receipt.")}, {TEXT("chooser_table"), TEXT("context_schema"), TEXT("row_count"), TEXT("fallback_row"), TEXT("selection_receipt")}},
			{TEXT("pose_search_database_probe"), TEXT("Scan content for Pose Search databases, schemas, and motion-matching assets."), TEXT("animation"), TEXT("class_probe"), {TEXT("PoseSearch")}, {TEXT("PoseSearch")}, {TEXT("PoseSearch"), TEXT("MotionMatching")}, {}, {}},
			{TEXT("pose_search_motion_matching_plan"), TEXT("Plan Pose Search and motion-matching database usage for generated locomotion or animation tasks."), TEXT("animation"), TEXT("plan"), {TEXT("PoseSearch")}, {TEXT("PoseSearch")}, {}, {TEXT("Resolve skeleton and animation clips."), TEXT("Choose PoseSearch schema/database target."), TEXT("Plan indexing and AnimBP handoff."), TEXT("Require compatibility, database, and preview receipts.")}, {TEXT("skeleton"), TEXT("animation_set"), TEXT("pose_database"), TEXT("animbp_handoff"), TEXT("preview_receipt")}},
			{TEXT("zonegraph_asset_probe"), TEXT("Scan content for ZoneGraph data, lane profiles, and related world-AI assets."), TEXT("world_ai"), TEXT("class_probe"), {TEXT("ZoneGraph")}, {TEXT("ZoneGraph")}, {TEXT("ZoneGraph"), TEXT("ZoneShape"), TEXT("Lane")}, {}, {}},
			{TEXT("zonegraph_lane_authoring_plan"), TEXT("Plan ZoneGraph lane/tag/connectivity authoring for pedestrians, traffic, crowds, or Mass agents."), TEXT("world_ai"), TEXT("plan"), {TEXT("ZoneGraph")}, {TEXT("ZoneGraph")}, {}, {TEXT("Resolve target level and lane ownership locks."), TEXT("Define lane splines, tags, widths, and connectors."), TEXT("Validate Mass/crowd navigation consumers."), TEXT("Require lane debug preview and validation receipt.")}, {TEXT("target_level"), TEXT("lane_tags"), TEXT("connector_map"), TEXT("debug_preview"), TEXT("validation_receipt")}},
			{TEXT("mass_entity_config_probe"), TEXT("Scan content for Mass Entity configs, processors, spawners, and crowd assets."), TEXT("world_ai"), TEXT("class_probe"), {TEXT("MassEntity"), TEXT("MassGameplay"), TEXT("MassAI"), TEXT("MassCrowd")}, {TEXT("MassSpawner"), TEXT("MassCrowd")}, {TEXT("Mass"), TEXT("EntityConfig"), TEXT("MassSpawner"), TEXT("Crowd")}, {}, {}},
			{TEXT("mass_crowd_deployment_plan"), TEXT("Plan Mass crowd or entity deployment with lane, population, performance, and QA guardrails."), TEXT("world_ai"), TEXT("plan"), {TEXT("MassEntity"), TEXT("MassGameplay"), TEXT("MassAI"), TEXT("MassCrowd")}, {TEXT("MassSpawner"), TEXT("MassCrowd")}, {}, {TEXT("Resolve population budget and spawn regions."), TEXT("Bind Mass entity config, ZoneGraph lanes, and representation assets."), TEXT("Create density and tick-budget guardrails."), TEXT("Require generated actor/entity health and preview receipt.")}, {TEXT("population_budget"), TEXT("entity_config"), TEXT("zonegraph_binding"), TEXT("performance_budget"), TEXT("preview_receipt")}},
			{TEXT("commonui_asset_probe"), TEXT("Scan content for CommonUI widgets, activatable widgets, and input/data assets."), TEXT("umg"), TEXT("class_probe"), {TEXT("CommonUI")}, {TEXT("CommonUI")}, {TEXT("Common"), TEXT("CommonActivatable"), TEXT("CommonButton"), TEXT("CommonUserWidget")}, {}, {}},
			{TEXT("commonui_widget_plan"), TEXT("Plan CommonUI widget/menu-stack authoring with input routing and UMG preview gates."), TEXT("umg"), TEXT("plan"), {TEXT("CommonUI")}, {TEXT("CommonUI")}, {}, {TEXT("Resolve widget class and navigation stack."), TEXT("Bind input actions and platform traits."), TEXT("Plan data bindings and focus rules."), TEXT("Require widget compile, preview, and input routing receipts.")}, {TEXT("widget_blueprint"), TEXT("input_actions"), TEXT("navigation_stack"), TEXT("compile_receipt"), TEXT("preview_receipt")}},
			{TEXT("data_registry_source_probe"), TEXT("Scan content for Data Registry assets, sources, and table-backed gameplay data."), TEXT("gameplay_data"), TEXT("class_probe"), {TEXT("DataRegistry")}, {TEXT("DataRegistry")}, {TEXT("DataRegistry"), TEXT("DataTable"), TEXT("CurveTable")}, {}, {}},
			{TEXT("data_registry_lookup_plan"), TEXT("Plan Data Registry item lookup and source wiring for gameplay/content data retrieval."), TEXT("gameplay_data"), TEXT("plan"), {TEXT("DataRegistry")}, {TEXT("DataRegistry")}, {}, {TEXT("Resolve registry type and item key schema."), TEXT("Bind source tables or data assets."), TEXT("Plan lookup smoke and missing-key behavior."), TEXT("Require registry readback and validation receipt.")}, {TEXT("registry_id"), TEXT("source_tables"), TEXT("item_key_schema"), TEXT("lookup_receipt"), TEXT("missing_key_policy")}},
			{TEXT("enhanced_input_blueprint_binding_plan"), TEXT("Plan Enhanced Input action/context binding into Blueprint or C++ gameplay receivers."), TEXT("input"), TEXT("plan"), {TEXT("EnhancedInput")}, {TEXT("EnhancedInput")}, {}, {TEXT("Resolve InputAction and InputMappingContext assets."), TEXT("Plan receiver Blueprint/C++ binding points."), TEXT("Check conflicts and platform modifiers."), TEXT("Require readback, compile, and input smoke receipts.")}, {TEXT("input_actions"), TEXT("mapping_context"), TEXT("receiver_asset"), TEXT("conflict_check"), TEXT("compile_receipt")}}
		};
	}

	// ============================================================================
	// AN-16 (2026-08-05): pose_search_motion_matching_plan — real implementation.
	// ----------------------------------------------------------------------------
	// The generic Specs() plan branch only echoed plan_steps. This replacement
	// reads back real PoseSearchDatabase assets (schema / skeleton / properties
	// via reflection, no PoseSearch module dependency) and is fail-closed when
	// the PoseSearch plugin or module is unavailable. CacheTtlSeconds = 0 so a
	// database created after the first call is never served stale.
	// ============================================================================

	static TSharedRef<FJsonObject> PoseSearchObjectPropertyJson(UObject* Owner, FProperty* Prop)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Prop->GetName());
		Row->SetStringField(TEXT("type"), Prop->GetClass()->GetName());
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Obj = ObjProp->GetObjectPropertyValue_InContainer(Owner);
			Row->SetStringField(TEXT("value"), Obj ? Obj->GetPathName() : FString());
			Row->SetStringField(TEXT("object_class"), Obj && Obj->GetClass() ? Obj->GetClass()->GetPathName() : FString());
		}
		else if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			Row->SetStringField(TEXT("value"), StrProp->GetPropertyValue_InContainer(Owner));
		}
		else if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			Row->SetStringField(TEXT("value"), NameProp->GetPropertyValue_InContainer(Owner).ToString());
		}
		else if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			Row->SetBoolField(TEXT("value"), BoolProp->GetPropertyValue_InContainer(Owner));
		}
		else if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			Row->SetNumberField(TEXT("value"), IntProp->GetPropertyValue_InContainer(Owner));
		}
		else if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			Row->SetNumberField(TEXT("value"), FloatProp->GetPropertyValue_InContainer(Owner));
		}
		else if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			Row->SetNumberField(TEXT("value"), DoubleProp->GetPropertyValue_InContainer(Owner));
		}
		else if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
		{
			FScriptArrayHelper ArrHelper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Owner));
			Row->SetNumberField(TEXT("element_count"), ArrHelper.Num());
		}
		else if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			Row->SetStringField(TEXT("value"), FString::Printf(TEXT("struct(%s)"), *StructProp->Struct->GetName()));
		}
		return Row;
	}

	// Reads a PoseSearch schema object down to its skeleton object path/pointer.
	static void PoseSearchSchemaSkeleton(UObject* SchemaObj, FString& OutSchemaPath, FString& OutSkeletonPath, UObject*& OutSkeleton)
	{
		OutSkeleton = nullptr;
		if (!SchemaObj)
		{
			return;
		}
		OutSchemaPath = SchemaObj->GetPathName();
		for (TFieldIterator<FProperty> It(SchemaObj->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*It);
			if (!ObjProp)
			{
				continue;
			}
			const FString PropName = It->GetName();
			if (PropName != TEXT("Skeleton") && PropName != TEXT("SkeletonReference"))
			{
				continue;
			}
			UObject* Obj = ObjProp->GetObjectPropertyValue_InContainer(SchemaObj);
			if (Obj)
			{
				OutSkeleton = Obj;
				OutSkeletonPath = Obj->GetPathName();
			}
		}
	}

	static TSharedRef<FJsonObject> PoseSearchDatabaseSummaryJson(UObject* Database, const FString& SkeletonCheckPath)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("path"), Database->GetPathName());
		Row->SetStringField(TEXT("name"), Database->GetName());
		Row->SetStringField(TEXT("class"), Database->GetClass() ? Database->GetClass()->GetPathName() : FString());

		FString SchemaPath;
		FString SkeletonPath;
		UObject* SkeletonObj = nullptr;
		TArray<TSharedPtr<FJsonValue>> Props;
		for (TFieldIterator<FProperty> It(Database->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FString PropName = It->GetName();
			if (PropName.EndsWith(TEXT("_DEPRECATED")) || PropName.StartsWith(TEXT("__")))
			{
				continue;
			}
			TSharedRef<FJsonObject> PropJson = PoseSearchObjectPropertyJson(Database, *It);
			if (PropName == TEXT("Schema"))
			{
				UObject* SchemaObj = nullptr;
				if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*It))
				{
					SchemaObj = ObjProp->GetObjectPropertyValue_InContainer(Database);
				}
				PoseSearchSchemaSkeleton(SchemaObj, SchemaPath, SkeletonPath, SkeletonObj);
				if (!SchemaPath.IsEmpty())
				{
					PropJson->SetStringField(TEXT("schema_path"), SchemaPath);
				}
				if (!SkeletonPath.IsEmpty())
				{
					PropJson->SetStringField(TEXT("skeleton_path"), SkeletonPath);
				}
			}
			Props.Add(MakeShared<FJsonValueObject>(PropJson));
		}
		Row->SetArrayField(TEXT("properties"), Props);
		Row->SetNumberField(TEXT("property_count"), Props.Num());
		if (!SchemaPath.IsEmpty())
		{
			Row->SetStringField(TEXT("schema_path"), SchemaPath);
		}
		if (!SkeletonPath.IsEmpty())
		{
			Row->SetStringField(TEXT("skeleton_path"), SkeletonPath);
		}

		// Skeleton compatibility against the caller-requested skeleton path.
		TSharedRef<FJsonObject> Compatibility = MakeShared<FJsonObject>();
		Compatibility->SetBoolField(TEXT("checked"), !SkeletonCheckPath.IsEmpty());
		Compatibility->SetStringField(TEXT("requested_skeleton"), SkeletonCheckPath);
		Compatibility->SetStringField(TEXT("database_skeleton"), SkeletonPath);
		if (SkeletonCheckPath.IsEmpty())
		{
			Compatibility->SetStringField(TEXT("compatible"), TEXT("not_requested"));
			Compatibility->SetStringField(TEXT("reason"), TEXT("Pass a skeleton path via asset_paths to enable the compatibility check."));
		}
		else if (SkeletonPath.IsEmpty())
		{
			Compatibility->SetStringField(TEXT("compatible"), TEXT("unknown"));
			Compatibility->SetStringField(TEXT("reason"), TEXT("Database has no readable schema skeleton."));
		}
		else
		{
			UObject* CheckedSkeleton = LoadObject<UObject>(nullptr, *SkeletonCheckPath);
			const bool bMatch = CheckedSkeleton != nullptr && SkeletonObj != nullptr && CheckedSkeleton == SkeletonObj;
			Compatibility->SetStringField(TEXT("compatible"), bMatch ? TEXT("yes") : TEXT("no"));
			Compatibility->SetStringField(TEXT("reason"), bMatch ? TEXT("Schema skeleton matches the requested skeleton.") : TEXT("Schema skeleton differs from the requested skeleton."));
		}
		Row->SetObjectField(TEXT("skeleton_compatibility"), Compatibility);
		return Row;
	}

	static bool ExecutePoseSearchMotionMatchingPlan(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("tool_name"), TEXT("pose_search_motion_matching_plan"));
		OutStructured->SetStringField(TEXT("domain"), TEXT("animation"));
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("plan_with_real_readback"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("read_only"));

		// Fail closed unless the PoseSearch plugin and module are actually usable.
		FWorldAiDataUiSpec GateSpec{TEXT("pose_search_motion_matching_plan"), FString(), TEXT("animation"), TEXT("plan"), {TEXT("PoseSearch")}, {TEXT("PoseSearch")}, {}, {}, {}};
		FString Availability;
		ProbeAvailability(GateSpec, OutStructured, Availability);
		if (Availability != TEXT("available"))
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_requires_pose_search_plugin"));
			OutError = TEXT("PoseSearch plugin/module is not available; pose_search_motion_matching_plan is fail-closed.");
			return false;
		}

		FString FolderPath = TEXT("/Game");
		FString Target;
		Arguments->TryGetStringField(TEXT("target"), Target);
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		int32 MaxAssets = 200;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);

		// asset_paths may carry a skeleton path for the compatibility check.
		FString SkeletonCheckPath;
		{
			const TArray<FString> AssetPaths = GetStringArrayField(Arguments, TEXT("asset_paths"));
			for (const FString& AssetPath : AssetPaths)
			{
				if (AssetPath.Contains(TEXT("Skeleton")) || AssetPath.Contains(TEXT("SK_")))
				{
					SkeletonCheckPath = AssetPath;
					break;
				}
			}
			if (SkeletonCheckPath.IsEmpty() && AssetPaths.Num() > 0)
			{
				SkeletonCheckPath = AssetPaths[0];
			}
		}

		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		ARM.Get().GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/PoseSearch"), TEXT("PoseSearchDatabase")), Assets, true);

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FAssetData& AssetData : Assets)
		{
			if (Rows.Num() >= MaxAssets)
			{
				break;
			}
			if (!AssetData.GetObjectPathString().StartsWith(FolderPath))
			{
				continue;
			}
			UObject* Database = AssetData.GetAsset();
			if (!Database)
			{
				continue;
			}
			if (!Target.IsEmpty() && Database->GetPathName() != Target)
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(PoseSearchDatabaseSummaryJson(Database, SkeletonCheckPath)));
		}

		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetStringField(TEXT("folder_path"), FolderPath);
		OutStructured->SetNumberField(TEXT("max_assets"), MaxAssets);
		OutStructured->SetStringField(TEXT("skeleton_check_path"), SkeletonCheckPath);
		OutStructured->SetArrayField(TEXT("databases"), Rows);
		OutStructured->SetNumberField(TEXT("database_count"), Rows.Num());

		// AnimBP handoff and preview receipt requirements, now anchored to the
		// read-back database inventory instead of a generic plan echo.
		OutStructured->SetArrayField(TEXT("animbp_handoff"), StringArrayJson({
			TEXT("Bind the chosen database via UPoseSearchComponent or the AnimGraph PoseSearch node."),
			TEXT("Require AnimBP compile receipt and a preview pose readback from the selected database.")}));
		OutStructured->SetArrayField(TEXT("preview_receipt"), StringArrayJson({
			TEXT("PoseSearch preview scene readback"),
			TEXT("selected motion index"),
			TEXT("trajectory/history input evidence")}));

		OutStructured->SetStringField(TEXT("status"), TEXT("realized"));
		OutStructured->SetBoolField(TEXT("readback_verified"), Rows.Num() > 0);
		OutSummary = FString::Printf(TEXT("pose_search_motion_matching_plan realized: %d PoseSearch database(s) read back%s."), Rows.Num(), SkeletonCheckPath.IsEmpty() ? TEXT(" (no skeleton check requested)") : TEXT(""));
		return true;
	}

	static void RegisterPoseSearchMotionMatchingPlan(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Def;
		Def.Name = TEXT("pose_search_motion_matching_plan");
		Def.Description = TEXT("Realize Pose Search / motion-matching database usage: read back PoseSearch databases, schemas, skeletons, and animation-set evidence with fail-closed plugin gating.");
		Def.InputSchema = GenericSchema();
		Def.CacheTtlSeconds = 0; // Real readback must never be served stale.
		Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return ExecutePoseSearchMotionMatchingPlan(Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}

void RegisterWorldAiDataUiTools(FSololmcpToolRegistry& Registry)
{
	WorldAiDataUiTools::RegisterMoverP1Tools(Registry);
	WorldAiDataUiTools::RegisterUafAnimNextP1Tools(Registry);
	WorldAiDataUiTools::RegisterWorldAiP1Tools(Registry);
	WorldAiDataUiTools::RegisterCommonUiP1Tools(Registry);
	// AN-16 (2026-08-05): the real PoseSearch implementation must register first;
	// the generic Specs() loop below is ignored for this name by the registry's
	// keep-first duplicate rule, but the loop skips it explicitly for clarity.
	WorldAiDataUiTools::RegisterPoseSearchMotionMatchingPlan(Registry);
	for (const WorldAiDataUiTools::FWorldAiDataUiSpec& Spec : WorldAiDataUiTools::Specs())
	{
		if (Spec.Name == TEXT("pose_search_motion_matching_plan"))
		{
			continue;
		}
		WorldAiDataUiTools::RegisterSpec(Registry, Spec);
	}
}
}
