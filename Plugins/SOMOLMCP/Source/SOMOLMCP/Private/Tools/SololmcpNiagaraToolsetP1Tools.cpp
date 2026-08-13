// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpNiagaraToolsetP1Tools.cpp
// ----------------------------------------------------------------------------
// UE 5.8 NiagaraToolsets P1 concrete probes, schema/topology plans, and receipt
// gates. The module compiles on UE 5.7 by avoiding direct NiagaraToolsets
// private headers and using plugin/module/class reflection only.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace NiagaraToolsetP1
{
	struct FCatalogRow
	{
		FString Id;
		FString ObjectPath;
		FString Kind;
		FString Category;
	};

	struct FSpec
	{
		FString Name;
		FString Description;
		FString Mode;
		FString Subdomain;
		bool bMutation = false;
		TArray<FString> FocusIds;
		TArray<FString> AssetNeedles;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
		TArray<FString> OfficialFunctions;
		TArray<FString> FallbackTools;
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

	static bool IsUE58OrLater()
	{
		const FEngineVersion Current = FEngineVersion::Current();
		return Current.GetMajor() > 5 || (Current.GetMajor() == 5 && Current.GetMinor() >= 8);
	}

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
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

	static bool BoolField(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field)
	{
		bool bValue = false;
		return Obj->TryGetBoolField(Field, bValue) && bValue;
	}

	static bool ProbeAvailability(TSharedRef<FJsonObject>& Out, FString& OutStatus)
	{
		const TArray<FString> Plugins{
			TEXT("NiagaraToolsets"), TEXT("Niagara"), TEXT("ToolsetRegistry")
		};
		const TArray<FString> Modules{
			TEXT("NiagaraToolsets"), TEXT("Niagara"), TEXT("NiagaraEditor"), TEXT("ToolsetRegistry")
		};

		TArray<TSharedPtr<FJsonValue>> PluginRows;
		bool bNiagaraToolsetsFound = false;
		bool bNiagaraToolsetsEnabled = false;
		bool bNiagaraFound = false;
		bool bNiagaraEnabled = false;
		bool bToolsetRegistryFound = false;
		bool bToolsetRegistryEnabled = false;
		for (const FString& PluginName : Plugins)
		{
			TSharedRef<FJsonObject> Row = PluginProbeJson(PluginName);
			const bool bFound = BoolField(Row, TEXT("found"));
			const bool bEnabled = BoolField(Row, TEXT("enabled"));
			if (PluginName == TEXT("NiagaraToolsets"))
			{
				bNiagaraToolsetsFound = bFound;
				bNiagaraToolsetsEnabled = bEnabled;
			}
			else if (PluginName == TEXT("Niagara"))
			{
				bNiagaraFound = bFound;
				bNiagaraEnabled = bEnabled;
			}
			else if (PluginName == TEXT("ToolsetRegistry"))
			{
				bToolsetRegistryFound = bFound;
				bToolsetRegistryEnabled = bEnabled;
			}
			PluginRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		TArray<TSharedPtr<FJsonValue>> ModuleRows;
		bool bNiagaraToolsetsModuleExists = false;
		bool bNiagaraModuleExists = false;
		bool bNiagaraEditorModuleExists = false;
		bool bToolsetRegistryModuleExists = false;
		for (const FString& ModuleName : Modules)
		{
			TSharedRef<FJsonObject> Row = ModuleProbeJson(ModuleName);
			const bool bExists = BoolField(Row, TEXT("exists"));
			if (ModuleName == TEXT("NiagaraToolsets"))
			{
				bNiagaraToolsetsModuleExists = bExists;
			}
			else if (ModuleName == TEXT("Niagara"))
			{
				bNiagaraModuleExists = bExists;
			}
			else if (ModuleName == TEXT("NiagaraEditor"))
			{
				bNiagaraEditorModuleExists = bExists;
			}
			else if (ModuleName == TEXT("ToolsetRegistry"))
			{
				bToolsetRegistryModuleExists = bExists;
			}
			ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		if (!IsUE58OrLater())
		{
			OutStatus = TEXT("requires_ue_5_8");
		}
		else if (!bNiagaraToolsetsFound)
		{
			OutStatus = TEXT("niagara_toolsets_plugin_missing");
		}
		else if (!bNiagaraFound || !bToolsetRegistryFound)
		{
			OutStatus = TEXT("dependency_plugin_missing");
		}
		else if (!bNiagaraToolsetsEnabled || !bNiagaraEnabled || !bToolsetRegistryEnabled)
		{
			OutStatus = TEXT("plugin_present_not_enabled");
		}
		else if (!bNiagaraToolsetsModuleExists)
		{
			OutStatus = TEXT("niagara_toolsets_module_missing");
		}
		else if (!bNiagaraModuleExists || !bNiagaraEditorModuleExists || !bToolsetRegistryModuleExists)
		{
			OutStatus = TEXT("dependency_module_missing");
		}
		else
		{
			OutStatus = TEXT("available");
		}

		Out->SetStringField(TEXT("status"), OutStatus);
		Out->SetBoolField(TEXT("available"), OutStatus == TEXT("available"));
		Out->SetBoolField(TEXT("niagara_toolsets_plugin_enabled"), bNiagaraToolsetsEnabled);
		Out->SetBoolField(TEXT("niagara_plugin_enabled"), bNiagaraEnabled);
		Out->SetBoolField(TEXT("toolset_registry_plugin_enabled"), bToolsetRegistryEnabled);
		Out->SetArrayField(TEXT("plugins"), PluginRows);
		Out->SetArrayField(TEXT("modules"), ModuleRows);
		return OutStatus == TEXT("available");
	}

	static TArray<FCatalogRow> Catalog()
	{
		return {
			{TEXT("NiagaraToolset"), TEXT("/Script/NiagaraToolsets.NiagaraToolset"), TEXT("class"), TEXT("toolset_base")},
			{TEXT("NiagaraToolset_System"), TEXT("/Script/NiagaraToolsets.NiagaraToolset_System"), TEXT("class"), TEXT("toolset_system")},
			{TEXT("NiagaraToolset_Component"), TEXT("/Script/NiagaraToolsets.NiagaraToolset_Component"), TEXT("class"), TEXT("toolset_component")},
			{TEXT("NiagaraToolset_Blueprint"), TEXT("/Script/NiagaraToolsets.NiagaraToolset_Blueprint"), TEXT("class"), TEXT("toolset_blueprint")},
			{TEXT("NiagaraToolset_Info"), TEXT("/Script/NiagaraToolsets.NiagaraToolset_Info"), TEXT("class"), TEXT("toolset_info")},
			{TEXT("NiagaraToolset_AsyncSystemCompileState"), TEXT("/Script/NiagaraToolsets.NiagaraToolset_AsyncSystemCompileState"), TEXT("class"), TEXT("toolset_async")},
			{TEXT("NiagaraToolset_AsyncStackIssues"), TEXT("/Script/NiagaraToolsets.NiagaraToolset_AsyncStackIssues"), TEXT("class"), TEXT("toolset_async")},
			{TEXT("NiagaraToolset_AsyncApplyStackIssueFixResult"), TEXT("/Script/NiagaraToolsets.NiagaraToolset_AsyncApplyStackIssueFixResult"), TEXT("class"), TEXT("toolset_async")},
			{TEXT("NiagaraSystem"), TEXT("/Script/Niagara.NiagaraSystem"), TEXT("class"), TEXT("niagara_asset")},
			{TEXT("NiagaraEmitter"), TEXT("/Script/Niagara.NiagaraEmitter"), TEXT("class"), TEXT("niagara_asset")},
			{TEXT("NiagaraScript"), TEXT("/Script/Niagara.NiagaraScript"), TEXT("class"), TEXT("niagara_asset")},
			{TEXT("NiagaraComponent"), TEXT("/Script/Niagara.NiagaraComponent"), TEXT("class"), TEXT("niagara_component")},
			{TEXT("NiagaraSpriteRendererProperties"), TEXT("/Script/Niagara.NiagaraSpriteRendererProperties"), TEXT("class"), TEXT("renderer")},
			{TEXT("NiagaraMeshRendererProperties"), TEXT("/Script/Niagara.NiagaraMeshRendererProperties"), TEXT("class"), TEXT("renderer")},
			{TEXT("NiagaraRibbonRendererProperties"), TEXT("/Script/Niagara.NiagaraRibbonRendererProperties"), TEXT("class"), TEXT("renderer")},
			{TEXT("NiagaraLightRendererProperties"), TEXT("/Script/Niagara.NiagaraLightRendererProperties"), TEXT("class"), TEXT("renderer")},
			{TEXT("NiagaraComponentRendererProperties"), TEXT("/Script/Niagara.NiagaraComponentRendererProperties"), TEXT("class"), TEXT("renderer")},
			{TEXT("NiagaraDataInterface"), TEXT("/Script/Niagara.NiagaraDataInterface"), TEXT("class"), TEXT("data_interface")},
			{TEXT("NiagaraDataInterfaceCurve"), TEXT("/Script/Niagara.NiagaraDataInterfaceCurve"), TEXT("class"), TEXT("data_interface")},
			{TEXT("NiagaraDataInterfaceTexture"), TEXT("/Script/Niagara.NiagaraDataInterfaceTexture"), TEXT("class"), TEXT("data_interface")},
			{TEXT("NiagaraDataInterfaceStaticMesh"), TEXT("/Script/Niagara.NiagaraDataInterfaceStaticMesh"), TEXT("class"), TEXT("data_interface")},
			{TEXT("NiagaraExt_SystemInfo"), TEXT("/Script/NiagaraEditor.NiagaraExt_SystemInfo"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_SystemTopology"), TEXT("/Script/NiagaraEditor.NiagaraExt_SystemTopology"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_EmitterTopology"), TEXT("/Script/NiagaraEditor.NiagaraExt_EmitterTopology"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_ModuleTopology"), TEXT("/Script/NiagaraEditor.NiagaraExt_ModuleTopology"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_StackInputSchema"), TEXT("/Script/NiagaraEditor.NiagaraExt_StackInputSchema"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_ModuleSchema"), TEXT("/Script/NiagaraEditor.NiagaraExt_ModuleSchema"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_DynamicInputSchema"), TEXT("/Script/NiagaraEditor.NiagaraExt_DynamicInputSchema"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_StackIssues"), TEXT("/Script/NiagaraEditor.NiagaraExt_StackIssues"), TEXT("struct"), TEXT("toolset_struct")},
			{TEXT("NiagaraExt_SystemCompileState"), TEXT("/Script/NiagaraEditor.NiagaraExt_SystemCompileState"), TEXT("struct"), TEXT("toolset_struct")}
		};
	}

	static TSharedRef<FJsonObject> StructStatusJson(const FCatalogRow& Row)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Row.Id);
		Obj->SetStringField(TEXT("objectPath"), Row.ObjectPath);
		Obj->SetStringField(TEXT("kind"), Row.Kind);
		Obj->SetStringField(TEXT("category"), Row.Category);

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
			TArray<TSharedPtr<FJsonValue>> Properties;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				++PropertyCount;
				if (Properties.Num() < 40)
				{
					TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
					Prop->SetStringField(TEXT("name"), It->GetName());
					Prop->SetStringField(TEXT("cppType"), It->GetCPPType());
					Properties.Add(MakeShared<FJsonValueObject>(Prop));
				}
			}
			Obj->SetNumberField(TEXT("propertyCount"), PropertyCount);
			Obj->SetArrayField(TEXT("properties"), Properties);
		}
		return Obj;
	}

	static TSharedRef<FJsonObject> ClassStatusJson(const FSololmcpToolExecutionContext& Context, const FCatalogRow& Row)
	{
		if (Row.Kind == TEXT("struct"))
		{
			return StructStatusJson(Row);
		}

		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Row.Id);
		Obj->SetStringField(TEXT("objectPath"), Row.ObjectPath);
		Obj->SetStringField(TEXT("kind"), Row.Kind);
		Obj->SetStringField(TEXT("category"), Row.Category);

		FString ResolveError;
		UClass* Class = Context.Services.ResolveClass(Row.ObjectPath, ResolveError);
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
			int32 AiCallableCount = 0;
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				++FunctionCount;
				if (It->HasMetaData(TEXT("AICallable")))
				{
					++AiCallableCount;
				}
			}
			Obj->SetNumberField(TEXT("propertyCount"), PropertyCount);
			Obj->SetNumberField(TEXT("functionCount"), FunctionCount);
			Obj->SetNumberField(TEXT("aiCallableFunctionCount"), AiCallableCount);
		}
		else if (!ResolveError.IsEmpty())
		{
			Obj->SetStringField(TEXT("resolveError"), ResolveError);
		}
		return Obj;
	}

	static bool FocusMatches(const FCatalogRow& Row, const TArray<FString>& FocusIds)
	{
		return FocusIds.IsEmpty() || FocusIds.Contains(Row.Id) || FocusIds.Contains(Row.Category);
	}

	static TArray<TSharedPtr<FJsonValue>> CatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusIds)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FCatalogRow& Row : Catalog())
		{
			if (!FocusMatches(Row, FocusIds))
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(ClassStatusJson(Context, Row)));
		}
		return Rows;
	}

	static bool FunctionMatches(UFunction* Function, const TArray<FString>& Needles)
	{
		if (!Function)
		{
			return false;
		}
		if (Needles.IsEmpty())
		{
			return Function->HasMetaData(TEXT("AICallable"));
		}
		const FString FunctionName = Function->GetName();
		for (const FString& Needle : Needles)
		{
			if (!Needle.IsEmpty() && FunctionName.Contains(Needle, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static TSharedRef<FJsonObject> FunctionJson(UClass* OwnerClass, UFunction* Function)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Function->GetName());
		Obj->SetStringField(TEXT("ownerClass"), OwnerClass ? OwnerClass->GetPathName() : FString());
		Obj->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
		Obj->SetBoolField(TEXT("blueprintCallable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		Obj->SetBoolField(TEXT("aiCallable"), Function->HasMetaData(TEXT("AICallable")));
		Obj->SetStringField(TEXT("category"), Function->GetMetaData(TEXT("Category")));

		TArray<TSharedPtr<FJsonValue>> Params;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			TSharedRef<FJsonObject> Param = MakeShared<FJsonObject>();
			Param->SetStringField(TEXT("name"), It->GetName());
			Param->SetStringField(TEXT("cppType"), It->GetCPPType());
			Param->SetBoolField(TEXT("return"), It->HasAnyPropertyFlags(CPF_ReturnParm));
			Param->SetBoolField(TEXT("out"), It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReturnParm));
			Params.Add(MakeShared<FJsonValueObject>(Param));
		}
		Obj->SetArrayField(TEXT("parameters"), Params);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> FunctionCatalogJson(
		const FSololmcpToolExecutionContext& Context,
		const TArray<FString>& FocusIds,
		const TArray<FString>& FunctionNeedles)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FCatalogRow& Row : Catalog())
		{
			if (Row.Kind != TEXT("class") || !FocusMatches(Row, FocusIds))
			{
				continue;
			}

			FString ResolveError;
			UClass* Class = Context.Services.ResolveClass(Row.ObjectPath, ResolveError);
			if (!Class)
			{
				continue;
			}

			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				UFunction* Function = *It;
				if (!FunctionMatches(Function, FunctionNeedles))
				{
					continue;
				}
				Rows.Add(MakeShared<FJsonValueObject>(FunctionJson(Class, Function)));
				if (Rows.Num() >= 100)
				{
					return Rows;
				}
			}
		}
		return Rows;
	}

	static TSharedRef<FJsonObject> AssetDataToJson(const FAssetData& AssetData)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		Obj->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
		Obj->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
		Obj->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());
		Obj->SetStringField(TEXT("classPath"), AssetData.AssetClassPath.ToString());
		TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
		for (const auto& TagPair : AssetData.TagsAndValues)
		{
			Tags->SetStringField(TagPair.Key.ToString(), TagPair.Value.GetValue());
		}
		Obj->SetObjectField(TEXT("tags"), Tags);
		return Obj;
	}

	static bool AssetMatches(const FAssetData& AssetData, const TArray<FString>& Needles)
	{
		if (Needles.IsEmpty())
		{
			return true;
		}
		const FString Haystack = FString::Printf(
			TEXT("%s %s %s %s"),
			*AssetData.AssetName.ToString(),
			*AssetData.GetObjectPathString(),
			*AssetData.PackagePath.ToString(),
			*AssetData.AssetClassPath.ToString());
		for (const FString& Needle : Needles)
		{
			if (!Needle.IsEmpty() && Haystack.Contains(Needle, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> AssetScanJson(const TArray<FString>& Needles, FString FolderPath, int32 MaxAssets)
	{
		if (FolderPath.IsEmpty())
		{
			FolderPath = TEXT("/Game");
		}
		if (MaxAssets <= 0)
		{
			MaxAssets = 50;
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*FolderPath));
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FAssetData& AssetData : Assets)
		{
			if (!AssetMatches(AssetData, Needles))
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(AssetDataToJson(AssetData)));
			if (Rows.Num() >= MaxAssets)
			{
				break;
			}
		}
		return Rows;
	}

	static TSharedRef<FJsonObject> AssetSummaryJson(const FSololmcpToolExecutionContext& Context, const FString& AssetPath)
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
			Obj->SetStringField(TEXT("status"), TEXT("load_failed"));
			Obj->SetStringField(TEXT("error"), LoadError);
			return Obj;
		}
		Obj->SetStringField(TEXT("status"), TEXT("loaded"));
		Obj->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("path"), Asset->GetPathName());
		int32 PropertyCount = 0;
		for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			++PropertyCount;
		}
		Obj->SetNumberField(TEXT("propertyCount"), PropertyCount);
		return Obj;
	}

	static bool ReceiptBool(const TSharedPtr<FJsonObject>& Receipt, const TCHAR* Field)
	{
		if (!Receipt.IsValid())
		{
			return false;
		}
		bool bValue = false;
		return Receipt->TryGetBoolField(Field, bValue) && bValue;
	}

	static bool ReceiptHasAny(const TSharedPtr<FJsonObject>& Receipt, const TArray<FString>& Fields)
	{
		if (!Receipt.IsValid())
		{
			return false;
		}
		for (const FString& Field : Fields)
		{
			FString StringValue;
			double NumberValue = 0.0;
			if ((Receipt->TryGetStringField(Field, StringValue) && !StringValue.IsEmpty()) || Receipt->HasTypedField<EJson::Object>(Field) || Receipt->HasTypedField<EJson::Array>(Field) || Receipt->TryGetNumberField(Field, NumberValue))
			{
				return true;
			}
			bool bBool = false;
			if (Receipt->TryGetBoolField(Field, bBool) && bBool)
			{
				return true;
			}
		}
		return false;
	}

	static void AddCheck(TArray<TSharedPtr<FJsonValue>>& Checks, bool& bValid, const FString& Name, bool bPass, const FString& Detail)
	{
		TSharedRef<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("pass"), bPass);
		Check->SetStringField(TEXT("detail"), Detail);
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		bValid = bValid && bPass;
	}

	static bool ExecuteReceiptTool(
		const FSpec& Spec,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		TSharedPtr<FJsonObject> Receipt;
		if (Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) && ReceiptPtr)
		{
			Receipt = *ReceiptPtr;
		}

		FString Target;
		if (!Arguments->TryGetStringField(TEXT("system_path"), Target) || Target.IsEmpty())
		{
			Arguments->TryGetStringField(TEXT("target_asset"), Target);
		}

		bool bValid = true;
		TArray<TSharedPtr<FJsonValue>> Checks;
		const bool bTargetBound = !Target.IsEmpty() || ReceiptHasAny(Receipt, {TEXT("system_path"), TEXT("target_asset"), TEXT("asset_path"), TEXT("project_path")});
		AddCheck(Checks, bValid, TEXT("target_binding"), bTargetBound, bTargetBound ? TEXT("Target binding found.") : TEXT("Missing Niagara target binding."));

		if (Spec.Mode == TEXT("compile_gate") || Spec.Name == TEXT("niagara_toolset_receipt_validate"))
		{
			const bool bCompileProof = ReceiptBool(Receipt, TEXT("compile_ok")) || ReceiptBool(Receipt, TEXT("validation_ok")) || ReceiptHasAny(Receipt, {TEXT("compile_state"), TEXT("compile_events"), TEXT("stack_issues"), TEXT("toolset_compile_validate")});
			AddCheck(Checks, bValid, TEXT("compile_or_validation"), bCompileProof, bCompileProof ? TEXT("Compile or validation evidence found.") : TEXT("Missing compile/validation evidence."));
		}
		if (Spec.Name == TEXT("niagara_preview_render_receipt") || Spec.Name == TEXT("niagara_toolset_receipt_validate"))
		{
			const bool bPreviewProof = ReceiptBool(Receipt, TEXT("preview_ok")) || ReceiptBool(Receipt, TEXT("screenshot_ok")) || ReceiptHasAny(Receipt, {TEXT("preview_path"), TEXT("screenshot_path"), TEXT("render_receipt"), TEXT("capture")});
			AddCheck(Checks, bValid, TEXT("preview_or_screenshot"), bPreviewProof, bPreviewProof ? TEXT("Preview/screenshot evidence found.") : TEXT("Missing preview/screenshot evidence."));
		}
		if (Spec.Name == TEXT("niagara_toolset_receipt_validate"))
		{
			const bool bReadbackProof = ReceiptBool(Receipt, TEXT("readback_ok")) || ReceiptHasAny(Receipt, {TEXT("post_edit_readback"), TEXT("topology"), TEXT("system_info"), TEXT("asset_readback")});
			AddCheck(Checks, bValid, TEXT("readback"), bReadbackProof, bReadbackProof ? TEXT("Readback evidence found.") : TEXT("Missing post-edit readback evidence."));
		}

		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("receipt_status"), bValid ? TEXT("accepted") : TEXT("failed_validation"));
		Out->SetArrayField(TEXT("checks"), Checks);
		Summary = FString::Printf(TEXT("%s receipt validation %s."), *Spec.Name, bValid ? TEXT("accepted") : TEXT("failed"));
		Error.Reset();
		return true;
	}

	static void SetPlanContract(const FSpec& Spec, const TSharedRef<FJsonObject>& Out)
	{
		TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
		Contract->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		Contract->SetStringField(TEXT("mode"), Spec.Mode);
		Contract->SetArrayField(TEXT("official_functions"), StringArrayJson(Spec.OfficialFunctions));
		Contract->SetArrayField(TEXT("fallback_tools"), StringArrayJson(Spec.FallbackTools));
		Contract->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		Contract->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		Contract->SetArrayField(TEXT("resource_locks"), StringArrayJson({TEXT("niagara_system_asset"), TEXT("niagara_stack"), TEXT("preview_or_compile_lane")}));
		Out->SetObjectField(TEXT("toolset_contract"), Contract);
	}

	static bool ExecuteTool(
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		FString SystemPath;
		if (!Arguments->TryGetStringField(TEXT("system_path"), SystemPath) || SystemPath.IsEmpty())
		{
			Arguments->TryGetStringField(TEXT("target_asset"), SystemPath);
		}
		FString ComponentPath;
		Arguments->TryGetStringField(TEXT("component_path"), ComponentPath);
		FString FolderPath;
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		int32 MaxAssets = 50;
		Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssets);
		const bool bExecute = BoolField(Arguments, TEXT("execute"));
		TArray<FString> ExtraNeedles = GetStringArrayField(Arguments, TEXT("asset_needles"));
		TArray<FString> Needles = Spec.AssetNeedles;
		Needles.Append(ExtraNeedles);

		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("tool_name"), Spec.Name);
		Out->SetStringField(TEXT("domain"), TEXT("niagara_toolset_upgrade"));
		Out->SetStringField(TEXT("family"), TEXT("niagara_toolset_upgrade"));
		Out->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		Out->SetStringField(TEXT("mode"), Spec.Mode);
		Out->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_write_plan") : TEXT("read_or_validate"));
		Out->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));
		Out->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		Out->SetStringField(TEXT("minimum_engine_version"), TEXT("5.8.0"));
		Out->SetBoolField(TEXT("version_satisfied"), IsUE58OrLater());
		Out->SetBoolField(TEXT("execute_requested"), bExecute);
		Out->SetStringField(TEXT("system_path"), SystemPath);
		Out->SetStringField(TEXT("component_path"), ComponentPath);
		Out->SetStringField(TEXT("folder_path"), FolderPath.IsEmpty() ? TEXT("/Game") : FolderPath);

		TSharedRef<FJsonObject> Gate = MakeShared<FJsonObject>();
		FString Availability;
		ProbeAvailability(Gate, Availability);
		Out->SetStringField(TEXT("availability_status"), Availability);
		Out->SetBoolField(TEXT("available"), Availability == TEXT("available"));
		Out->SetArrayField(TEXT("plugins"), Gate->GetArrayField(TEXT("plugins")));
		Out->SetArrayField(TEXT("modules"), Gate->GetArrayField(TEXT("modules")));
		Out->SetObjectField(TEXT("target_system_summary"), AssetSummaryJson(Context, SystemPath));
		Out->SetObjectField(TEXT("target_component_summary"), AssetSummaryJson(Context, ComponentPath));
		Out->SetArrayField(TEXT("class_catalog"), CatalogJson(Context, Spec.FocusIds));
		Out->SetArrayField(TEXT("function_catalog"), FunctionCatalogJson(Context, Spec.FocusIds, Spec.OfficialFunctions));
		Out->SetArrayField(TEXT("asset_scan"), AssetScanJson(Needles, FolderPath, MaxAssets));
		Out->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		Out->SetArrayField(TEXT("ue57_fallback_tools"), StringArrayJson(Spec.FallbackTools));
		SetPlanContract(Spec, Out);

		if (Spec.Name == TEXT("niagara_toolset_schema_get"))
		{
			TSharedRef<FJsonObject> SchemaRoutes = MakeShared<FJsonObject>();
			SchemaRoutes->SetArrayField(TEXT("system_schema"), StringArrayJson({TEXT("GetSystemSchema"), TEXT("GetSystemData"), TEXT("SetSystemData")}));
			SchemaRoutes->SetArrayField(TEXT("emitter_schema"), StringArrayJson({TEXT("GetEmitterSchema"), TEXT("GetEmitterData"), TEXT("SetEmitterData")}));
			SchemaRoutes->SetArrayField(TEXT("renderer_schema"), StringArrayJson({TEXT("GetRendererSchema"), TEXT("GetRendererData"), TEXT("SetRendererData")}));
			SchemaRoutes->SetArrayField(TEXT("data_interface_schema"), StringArrayJson({TEXT("GetDataInterfaceSchema")}));
			Out->SetObjectField(TEXT("schema_routes"), SchemaRoutes);
		}
		if (Spec.Name == TEXT("niagara_system_topology_v2"))
		{
			Out->SetArrayField(TEXT("topology_layers"), StringArrayJson({TEXT("system"), TEXT("emitters"), TEXT("scripts"), TEXT("modules"), TEXT("inputs"), TEXT("renderers"), TEXT("user_variables")}));
		}
		if (Spec.Name == TEXT("niagara_stack_input_schema") || Spec.Name == TEXT("niagara_stack_input_set_v2"))
		{
			Out->SetArrayField(TEXT("stack_input_value_modes"), StringArrayJson({TEXT("literal"), TEXT("enum"), TEXT("linked_parameter"), TEXT("hlsl_expression"), TEXT("data_interface"), TEXT("dynamic_input"), TEXT("unsupported")}));
		}
		if (Spec.Name == TEXT("niagara_renderer_add_v2"))
		{
			Out->SetArrayField(TEXT("known_renderer_classes"), StringArrayJson({TEXT("NiagaraSpriteRendererProperties"), TEXT("NiagaraMeshRendererProperties"), TEXT("NiagaraRibbonRendererProperties"), TEXT("NiagaraLightRendererProperties"), TEXT("NiagaraComponentRendererProperties")}));
		}
		if (Spec.Name == TEXT("niagara_data_interface_schema_get"))
		{
			Out->SetArrayField(TEXT("known_data_interface_classes"), StringArrayJson({TEXT("NiagaraDataInterface"), TEXT("NiagaraDataInterfaceCurve"), TEXT("NiagaraDataInterfaceTexture"), TEXT("NiagaraDataInterfaceStaticMesh")}));
		}
		if (Spec.Name == TEXT("niagara_toolset_rollback_plan"))
		{
			Out->SetArrayField(TEXT("rollback_steps"), StringArrayJson({TEXT("pre-edit topology snapshot"), TEXT("transaction boundary"), TEXT("targeted edit"), TEXT("post-edit compile"), TEXT("restore snapshot or undo on failure"), TEXT("post-rollback topology readback")}));
		}

		if (Spec.Mode == TEXT("receipt") || Spec.Mode == TEXT("compile_gate"))
		{
			return ExecuteReceiptTool(Spec, Arguments, Out, Summary, Error);
		}

		if (bExecute && Availability != TEXT("available"))
		{
			Out->SetBoolField(TEXT("success"), false);
			Out->SetStringField(TEXT("status"), Availability);
			Out->SetStringField(TEXT("failure_route"), TEXT("enable_ue58_niagara_toolsets_then_retry"));
			Error = FString::Printf(TEXT("%s cannot execute because NiagaraToolsets gate is %s."), *Spec.Name, *Availability);
			Summary = Error;
			return false;
		}
		if (bExecute && Spec.bMutation)
		{
			Out->SetBoolField(TEXT("success"), false);
			Out->SetStringField(TEXT("status"), TEXT("blocked_pending_niagara_toolset_writer"));
			Out->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_niagara_toolset_writer_after_live_fixture_and_receipt"));
			Error = FString::Printf(TEXT("%s blocked_pending_niagara_toolset_writer: concrete Niagara Toolset plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			Summary = Error;
			return false;
		}

		const bool bCompletedRead =
			Spec.Mode == TEXT("schema") ||
			Spec.Mode == TEXT("topology") ||
			Spec.Mode == TEXT("audit") ||
			Spec.Mode == TEXT("issues");
		Out->SetStringField(TEXT("status"), bCompletedRead ? TEXT("completed") : TEXT("dry_run"));
		Summary = FString::Printf(TEXT("%s returned NiagaraToolsets %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TArray<FSpec> Specs()
	{
		const TArray<FString> ToolsetSystemFocus{TEXT("toolset_system"), TEXT("niagara_asset"), TEXT("renderer"), TEXT("data_interface"), TEXT("toolset_struct")};
		const TArray<FString> ComponentFocus{TEXT("toolset_component"), TEXT("niagara_component"), TEXT("niagara_asset"), TEXT("toolset_struct")};
		const TArray<FString> BlueprintFocus{TEXT("toolset_blueprint"), TEXT("niagara_component"), TEXT("niagara_asset"), TEXT("toolset_struct")};
		const TArray<FString> InfoFocus{TEXT("toolset_info"), TEXT("toolset_system"), TEXT("niagara_asset"), TEXT("toolset_struct")};
		const TArray<FString> NiagaraNeedles{TEXT("Niagara"), TEXT("NS_"), TEXT("NE_"), TEXT("FX"), TEXT("Module"), TEXT("Emitter")};
		const TArray<FString> BaseFallback{TEXT("niagara_system_inspect"), TEXT("niagara_list_stack_modules"), TEXT("niagara_script_add_node"), TEXT("niagara_add_custom_node"), TEXT("niagara_toolset_receipt_validate")};
		const TArray<FString> Receipt{TEXT("target Niagara system/component"), TEXT("topology or schema readback"), TEXT("compile/stack issue validation"), TEXT("preview or screenshot evidence")};
		return {
			{TEXT("niagara_toolset_schema_get"), TEXT("Get UE 5.8 NiagaraToolsets schema/function/class inventory."), TEXT("schema"), TEXT("schema"), false, InfoFocus, NiagaraNeedles, {TEXT("Probe NiagaraToolsets plugin and ToolsetRegistry."), TEXT("Reflect AICallable schema functions."), TEXT("Return schema routing contract for AI authoring.")}, Receipt, {TEXT("GetSystemSchema"), TEXT("GetEmitterSchema"), TEXT("GetRendererSchema"), TEXT("GetDataInterfaceSchema"), TEXT("GetStackInputSchema"), TEXT("GetModuleSchema"), TEXT("GetDynamicInputSchema")}, BaseFallback},
			{TEXT("niagara_system_topology_v2"), TEXT("Inspect/plan Niagara system topology through Toolset contracts."), TEXT("topology"), TEXT("system_topology"), false, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Resolve system target."), TEXT("Read or plan system topology layers."), TEXT("Use topology before stack mutation.")}, Receipt, {TEXT("GetSystemInfo"), TEXT("GetSystemTopology"), TEXT("GetScriptStackTopology"), TEXT("GetEmitterTopology"), TEXT("GetModuleTopology"), TEXT("GetStackInputTopology")}, BaseFallback},
			{TEXT("niagara_stack_input_schema"), TEXT("Inspect Niagara stack input schema and value modes."), TEXT("schema"), TEXT("stack_input"), false, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Resolve stack input reference."), TEXT("Return input type, value mode, and compatible dynamic input contract.")}, Receipt, {TEXT("GetStackInputSchema"), TEXT("GetStackInputData"), TEXT("GetDynamicInputSchema"), TEXT("GetDynamicInputSchemaFromAsset")}, BaseFallback},
			{TEXT("niagara_module_schema_from_asset"), TEXT("Inspect Niagara module asset schema."), TEXT("schema"), TEXT("module_schema"), false, ToolsetSystemFocus, {TEXT("Niagara"), TEXT("Module"), TEXT("Script")}, {TEXT("Resolve module script asset."), TEXT("Return module input schema and add-module compatibility.")}, Receipt, {TEXT("GetModuleSchemaFromAsset"), TEXT("GetModuleSchema")}, BaseFallback},
			{TEXT("niagara_dynamic_input_schema"), TEXT("Inspect Niagara dynamic input schema."), TEXT("schema"), TEXT("dynamic_input"), false, ToolsetSystemFocus, {TEXT("Niagara"), TEXT("Dynamic"), TEXT("Module"), TEXT("Script")}, {TEXT("Resolve dynamic input asset or stack reference."), TEXT("Return compatible input schema and value modes.")}, Receipt, {TEXT("GetAvailableDynamicInputs"), TEXT("GetDynamicInputSchemaFromAsset"), TEXT("GetDynamicInputSchema")}, BaseFallback},
			{TEXT("niagara_user_vars_add_v2"), TEXT("Plan Niagara user variable add/update through Toolset contracts."), TEXT("plan"), TEXT("user_variables"), true, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Resolve target system."), TEXT("Plan variable type/default additions."), TEXT("Require user-variable readback and compile receipt.")}, Receipt, {TEXT("GetUserVariables"), TEXT("AddUserVariables"), TEXT("RemoveUserVariables")}, BaseFallback},
			{TEXT("niagara_module_add_v2"), TEXT("Plan Niagara module insertion through Toolset contracts."), TEXT("plan"), TEXT("module_add"), true, ToolsetSystemFocus, {TEXT("Niagara"), TEXT("Module"), TEXT("Script")}, {TEXT("Resolve target stack and module asset."), TEXT("Plan add/remove/enable route."), TEXT("Require topology readback and compile receipt.")}, Receipt, {TEXT("AddModule"), TEXT("RemoveModule"), TEXT("SetModuleEnabled"), TEXT("GetModuleTopology")}, BaseFallback},
			{TEXT("niagara_renderer_add_v2"), TEXT("Plan Niagara renderer insertion through Toolset contracts."), TEXT("plan"), TEXT("renderer_add"), true, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Resolve target emitter."), TEXT("Select renderer class."), TEXT("Plan renderer data/schema update and readback.")}, Receipt, {TEXT("GetRendererSchema"), TEXT("AddRenderer"), TEXT("RemoveRenderer"), TEXT("GetRendererData"), TEXT("SetRendererData")}, BaseFallback},
			{TEXT("niagara_stack_input_set_v2"), TEXT("Plan Niagara stack input value update."), TEXT("plan"), TEXT("stack_input_set"), true, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Resolve stack input reference."), TEXT("Plan literal, linked, HLSL, DataInterface, or dynamic input value."), TEXT("Require input readback and compile receipt.")}, Receipt, {TEXT("GetStackInputData"), TEXT("SetStackInputData")}, BaseFallback},
			{TEXT("niagara_stack_issues_get"), TEXT("Inspect Niagara stack issues through Toolset async status."), TEXT("issues"), TEXT("stack_issues"), false, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Resolve target system."), TEXT("Collect stack issues and compile state."), TEXT("Route errors to VFX QA.")}, Receipt, {TEXT("GetStackIssues"), TEXT("GetSystemCompileState")}, BaseFallback},
			{TEXT("niagara_stack_issue_fix_apply"), TEXT("Plan Niagara stack issue fix application."), TEXT("issue_fix"), TEXT("stack_issue_fix"), true, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Resolve issue_id and fix_id from prior stack issue readback."), TEXT("Plan fix application inside transaction."), TEXT("Require post-fix stack issues and compile receipt.")}, Receipt, {TEXT("ApplyStackIssueFix"), TEXT("GetStackIssues")}, BaseFallback},
			{TEXT("niagara_bp_wrapper_create"), TEXT("Plan Blueprint actor wrapper creation for Niagara system/component."), TEXT("plan"), TEXT("bp_wrapper"), true, BlueprintFocus, NiagaraNeedles, {TEXT("Resolve system/component source."), TEXT("Plan Blueprint asset path and parent class."), TEXT("Require Blueprint compile and component readback.")}, Receipt, {TEXT("ConstructNiagaraBPWrapperFromSystem"), TEXT("ConstructNiagaraBPWrapperFromComponent")}, {TEXT("blueprint_create"), TEXT("blueprint_add_component"), TEXT("niagara_component_set_asset"), TEXT("blueprint_compile")}},
			{TEXT("niagara_emitter_template_apply_v2"), TEXT("Plan Niagara emitter template application."), TEXT("plan"), TEXT("emitter_template"), true, ToolsetSystemFocus, {TEXT("Niagara"), TEXT("Emitter"), TEXT("Template")}, {TEXT("Resolve target system and emitter template."), TEXT("Plan AddEmitter/RemoveEmitter route."), TEXT("Require emitter topology readback and compile receipt.")}, Receipt, {TEXT("CreateNiagaraSystem"), TEXT("AddEmitter"), TEXT("RemoveEmitter"), TEXT("GetEmitterTopology")}, BaseFallback},
			{TEXT("niagara_parameter_binding_audit"), TEXT("Audit Niagara parameter binding/user-variable consistency."), TEXT("audit"), TEXT("parameter_binding"), false, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Read user variables and stack input topology."), TEXT("Compare expected parameters to renderer/module inputs."), TEXT("Return missing/unused binding hints.")}, Receipt, {TEXT("GetUserVariables"), TEXT("GetStackInputTopology"), TEXT("GetStackInputData"), TEXT("GetSystemTopology")}, BaseFallback},
			{TEXT("niagara_data_interface_schema_get"), TEXT("Inspect Niagara data interface schema and compatibility."), TEXT("schema"), TEXT("data_interface"), false, ToolsetSystemFocus, {TEXT("Niagara"), TEXT("DataInterface"), TEXT("Texture"), TEXT("StaticMesh")}, {TEXT("Resolve data interface class."), TEXT("Return schema and stack input compatibility.")}, Receipt, {TEXT("GetDataInterfaceSchema")}, BaseFallback},
			{TEXT("niagara_sim_cache_capture_v2"), TEXT("Plan Niagara simulation cache capture."), TEXT("plan"), TEXT("sim_cache"), true, ComponentFocus, NiagaraNeedles, {TEXT("Resolve component or system preview target."), TEXT("Plan capture duration/frame budget."), TEXT("Require sim cache asset readback and preview receipt.")}, Receipt, {TEXT("SetSystem"), TEXT("GetUserVariables"), TEXT("GetVariable")}, {TEXT("niagara_component_set_asset"), TEXT("editor_screenshot_capture"), TEXT("asset_save"), TEXT("niagara_toolset_receipt_validate")}},
			{TEXT("niagara_preview_render_receipt"), TEXT("Validate Niagara preview/render receipt."), TEXT("receipt"), TEXT("preview"), false, ToolsetSystemFocus, NiagaraNeedles, {}, {TEXT("target binding"), TEXT("preview screenshot or render receipt")}, {TEXT("GetSystemCompileState"), TEXT("GetStackIssues")}, BaseFallback},
			{TEXT("niagara_toolset_compile_validate"), TEXT("Validate Niagara Toolset compile/stack issue receipt."), TEXT("compile_gate"), TEXT("compile"), false, ToolsetSystemFocus, NiagaraNeedles, {}, {TEXT("target binding"), TEXT("compile state"), TEXT("stack issues")}, {TEXT("GetSystemCompileState"), TEXT("GetStackIssues")}, BaseFallback},
			{TEXT("niagara_toolset_rollback_plan"), TEXT("Build Niagara Toolset rollback plan."), TEXT("rollback_plan"), TEXT("rollback"), false, ToolsetSystemFocus, NiagaraNeedles, {TEXT("Snapshot topology and affected assets."), TEXT("Define transaction and restore route."), TEXT("Require post-rollback compile/readback.")}, Receipt, {TEXT("GetSystemInfo"), TEXT("GetSystemTopology"), TEXT("GetStackIssues")}, {TEXT("transaction_begin"), TEXT("transaction_abort"), TEXT("niagara_graph_restore_snapshot_full"), TEXT("niagara_toolset_receipt_validate")}},
			{TEXT("niagara_toolset_receipt_validate"), TEXT("Validate full Niagara Toolset production receipt."), TEXT("receipt"), TEXT("receipt"), false, ToolsetSystemFocus, NiagaraNeedles, {}, Receipt, {TEXT("GetSystemCompileState"), TEXT("GetStackIssues"), TEXT("GetSystemTopology")}, BaseFallback}
		};
	}

	static TSharedRef<FJsonObject> InputSchema()
	{
		TMap<FString, TSharedRef<FJsonObject>> Props;
		Props.Add(TEXT("system_path"), FSololmcpSchemaBuilder::String(TEXT("Target Niagara System asset path.")));
		Props.Add(TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for target Niagara asset path.")));
		Props.Add(TEXT("component_path"), FSololmcpSchemaBuilder::String(TEXT("Target Niagara Component object or asset path when applicable.")));
		Props.Add(TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Folder path for asset-registry scans.")));
		Props.Add(TEXT("module_asset"), FSololmcpSchemaBuilder::String(TEXT("Niagara module script asset path.")));
		Props.Add(TEXT("dynamic_input_asset"), FSololmcpSchemaBuilder::String(TEXT("Niagara dynamic input script asset path.")));
		Props.Add(TEXT("emitter_name"), FSololmcpSchemaBuilder::String(TEXT("Emitter name or template name.")));
		Props.Add(TEXT("renderer_class"), FSololmcpSchemaBuilder::String(TEXT("Renderer class path/name.")));
		Props.Add(TEXT("data_interface_class"), FSololmcpSchemaBuilder::String(TEXT("Data interface class path/name.")));
		Props.Add(TEXT("stack_ref"), FSololmcpSchemaBuilder::String(TEXT("Serialized Niagara stack item reference.")));
		Props.Add(TEXT("input_name"), FSololmcpSchemaBuilder::String(TEXT("Stack input or parameter name.")));
		Props.Add(TEXT("variable_name"), FSololmcpSchemaBuilder::String(TEXT("Niagara user variable name.")));
		Props.Add(TEXT("variable_type"), FSololmcpSchemaBuilder::String(TEXT("Niagara variable type name.")));
		Props.Add(TEXT("value"), FSololmcpSchemaBuilder::String(TEXT("Serialized input/user-variable value.")));
		Props.Add(TEXT("issue_id"), FSololmcpSchemaBuilder::String(TEXT("Stack issue id.")));
		Props.Add(TEXT("fix_id"), FSololmcpSchemaBuilder::String(TEXT("Stack issue fix id.")));
		Props.Add(TEXT("blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint wrapper target asset path.")));
		Props.Add(TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation execution. Mutating tools fail closed until dedicated writers are proven.")));
		Props.Add(TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum asset scan results.")));
		Props.Add(TEXT("asset_needles"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Additional asset scan needles.")));
		Props.Add(TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Receipt evidence for validation tools.")));
		return FSololmcpSchemaBuilder::Object(Props);
	}

	static void RegisterSpec(FSololmcpToolRegistry& Registry, const FSpec& Spec)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = Spec.Description;
		Def.InputSchema = InputSchema();
		Def.CacheTtlSeconds = Spec.bMutation ? 0 : 2;
		Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return ExecuteTool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}

void RegisterNiagaraToolsetP1Tools(FSololmcpToolRegistry& Registry)
{
	for (const NiagaraToolsetP1::FSpec& Spec : NiagaraToolsetP1::Specs())
	{
		NiagaraToolsetP1::RegisterSpec(Registry, Spec);
	}
}
}
