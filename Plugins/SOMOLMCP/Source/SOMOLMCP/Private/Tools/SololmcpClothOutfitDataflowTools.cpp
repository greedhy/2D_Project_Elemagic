// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpClothOutfitDataflowTools.cpp
// ----------------------------------------------------------------------------
// UE 5.8 Cloth / Outfit / Dataflow P1 concrete probes, production plans, and
// receipt gates. The module compiles on UE 5.7 by avoiding direct headers from
// ChaosClothAsset, ChaosOutfitAsset, and Dataflow optional plugins.
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
namespace ClothOutfitDataflowP1
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

	static bool ProbeAvailability(TSharedRef<FJsonObject>& Out, FString& OutStatus)
	{
		const TArray<FString> Plugins{
			TEXT("ChaosClothAsset"), TEXT("ChaosOutfitAsset"), TEXT("ChaosClothAssetDataflowNodes"), TEXT("Dataflow"), TEXT("MutableClothing")
		};
		const TArray<FString> Modules{
			TEXT("ChaosClothAssetEngine"), TEXT("ChaosClothAssetTools"), TEXT("ChaosOutfitAssetEngine"),
			TEXT("ChaosOutfitAssetDataflowNodes"), TEXT("DataflowEnginePlugin"), TEXT("DataflowEditor")
		};

		TArray<TSharedPtr<FJsonValue>> PluginRows;
		bool bAnyPluginFound = false;
		bool bAnyPluginEnabled = false;
		for (const FString& PluginName : Plugins)
		{
			TSharedRef<FJsonObject> Row = PluginProbeJson(PluginName);
			bool bFound = false;
			bool bEnabled = false;
			Row->TryGetBoolField(TEXT("found"), bFound);
			Row->TryGetBoolField(TEXT("enabled"), bEnabled);
			bAnyPluginFound = bAnyPluginFound || bFound;
			bAnyPluginEnabled = bAnyPluginEnabled || bEnabled;
			PluginRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		TArray<TSharedPtr<FJsonValue>> ModuleRows;
		bool bAnyModuleExists = false;
		for (const FString& ModuleName : Modules)
		{
			TSharedRef<FJsonObject> Row = ModuleProbeJson(ModuleName);
			bool bExists = false;
			Row->TryGetBoolField(TEXT("exists"), bExists);
			bAnyModuleExists = bAnyModuleExists || bExists;
			ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		if (!IsUE58OrLater())
		{
			OutStatus = TEXT("requires_ue_5_8");
		}
		else if (!bAnyPluginFound)
		{
			OutStatus = TEXT("plugin_missing");
		}
		else if (!bAnyModuleExists)
		{
			OutStatus = TEXT("module_missing");
		}
		else if (!bAnyPluginEnabled)
		{
			OutStatus = TEXT("plugin_present_not_enabled");
		}
		else
		{
			OutStatus = TEXT("available");
		}

		Out->SetStringField(TEXT("status"), OutStatus);
		Out->SetBoolField(TEXT("available"), OutStatus == TEXT("available"));
		Out->SetArrayField(TEXT("plugins"), PluginRows);
		Out->SetArrayField(TEXT("modules"), ModuleRows);
		return OutStatus == TEXT("available");
	}

	static TArray<FCatalogRow> Catalog()
	{
		return {
			{TEXT("ChaosClothAsset"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothAsset"), TEXT("class"), TEXT("cloth_asset")},
			{TEXT("ChaosClothAssetBase"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothAssetBase"), TEXT("class"), TEXT("cloth_asset")},
			{TEXT("ChaosClothComponent"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothComponent"), TEXT("class"), TEXT("cloth_runtime")},
			{TEXT("ChaosClothAssetInteractor"), TEXT("/Script/ChaosClothAssetEngine.ChaosClothAssetInteractor"), TEXT("class"), TEXT("cloth_runtime")},
			{TEXT("ClothAssetInteractorDataAsset"), TEXT("/Script/ChaosClothAssetEngine.ClothAssetInteractorDataAsset"), TEXT("class"), TEXT("cloth_runtime")},
			{TEXT("ChaosClothAssetFactory"), TEXT("/Script/ChaosClothAssetTools.ChaosClothAssetFactory"), TEXT("class"), TEXT("cloth_editor")},
			{TEXT("ClothAssetEditorSkeletalMeshConverter"), TEXT("/Script/ChaosClothAssetTools.ClothAssetEditorSkeletalMeshConverter"), TEXT("class"), TEXT("cloth_editor")},
			{TEXT("ClothingAssetToChaosClothAssetExporter"), TEXT("/Script/ChaosClothAssetTools.ClothingAssetToChaosClothAssetExporter"), TEXT("class"), TEXT("cloth_editor")},
			{TEXT("ChaosOutfitAsset"), TEXT("/Script/ChaosOutfitAssetEngine.ChaosOutfitAsset"), TEXT("class"), TEXT("outfit")},
			{TEXT("ChaosOutfit"), TEXT("/Script/ChaosOutfitAssetEngine.ChaosOutfit"), TEXT("class"), TEXT("outfit")},
			{TEXT("ChaosOutfitAssetBodyUserData"), TEXT("/Script/ChaosOutfitAssetEngine.ChaosOutfitAssetBodyUserData"), TEXT("class"), TEXT("outfit")},
			{TEXT("ChaosOutfitAssetFactory"), TEXT("/Script/ChaosOutfitAssetEditor.ChaosOutfitAssetFactory"), TEXT("class"), TEXT("outfit_editor")},
			{TEXT("DataflowActor"), TEXT("/Script/DataflowEnginePlugin.DataflowActor"), TEXT("class"), TEXT("dataflow_runtime")},
			{TEXT("DataflowComponent"), TEXT("/Script/DataflowEnginePlugin.DataflowComponent"), TEXT("class"), TEXT("dataflow_runtime")},
			{TEXT("DataflowAssetFactory"), TEXT("/Script/DataflowEditor.DataflowAssetFactory"), TEXT("class"), TEXT("dataflow_editor")},
			{TEXT("DataflowEditorBlueprintLibrary"), TEXT("/Script/DataflowEditor.DataflowEditorBlueprintLibrary"), TEXT("class"), TEXT("dataflow_editor")},
			{TEXT("DataflowSchema"), TEXT("/Script/DataflowEditor.DataflowSchema"), TEXT("class"), TEXT("dataflow_editor")},
			{TEXT("DataflowSimulationSettings"), TEXT("/Script/DataflowEditor.DataflowSimulationSettings"), TEXT("class"), TEXT("dataflow_sim")},
			{TEXT("DataflowClothSimRenderSettings"), TEXT("/Script/ChaosClothAssetDataflowNodes.DataflowClothSimRenderSettings"), TEXT("class"), TEXT("cloth_dataflow")},
			{TEXT("ChaosClothAssetDatasmithClothAssetFactory"), TEXT("/Script/ChaosClothAssetDataflowNodes.ChaosClothAssetDatasmithClothAssetFactory"), TEXT("class"), TEXT("cloth_dataflow")},
			{TEXT("DataflowEditorWeightMapPaintTool"), TEXT("/Script/DataflowEditor.DataflowEditorWeightMapPaintTool"), TEXT("class"), TEXT("weightmap")},
			{TEXT("DataflowEditorSkinWeightsPaintTool"), TEXT("/Script/DataflowEditor.DataflowEditorSkinWeightsPaintTool"), TEXT("class"), TEXT("skin_weight")},
			{TEXT("DataflowMeshSelectionTool"), TEXT("/Script/DataflowEditor.DataflowMeshSelectionTool"), TEXT("class"), TEXT("mesh_selection")}
		};
	}

	static TSharedRef<FJsonObject> ClassStatusJson(const FSololmcpToolExecutionContext& Context, const FCatalogRow& Row)
	{
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

	static TArray<TSharedPtr<FJsonValue>> CatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusIds)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FCatalogRow& Row : Catalog())
		{
			if (!FocusIds.IsEmpty() && !FocusIds.Contains(Row.Id) && !FocusIds.Contains(Row.Category))
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(ClassStatusJson(Context, Row)));
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
		const FString Haystack = FString::Printf(TEXT("%s %s %s %s"), *AssetData.AssetName.ToString(), *AssetData.GetObjectPathString(), *AssetData.PackagePath.ToString(), *AssetData.AssetClassPath.ToString());
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
		if (Asset->GetClass())
		{
			int32 Count = 0;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It && Count < 80; ++It, ++Count)
			{
				TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
				Property->SetStringField(TEXT("name"), It->GetName());
				Property->SetStringField(TEXT("class"), It->GetClass()->GetName());
				Property->SetBoolField(TEXT("editable"), It->HasAnyPropertyFlags(CPF_Edit));
				Properties.Add(MakeShared<FJsonValueObject>(Property));
			}
		}
		Obj->SetArrayField(TEXT("properties"), Properties);
		Obj->SetNumberField(TEXT("propertySampleCount"), Properties.Num());
		return Obj;
	}

	static FString TargetAsset(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Value;
		for (const TCHAR* Field : {TEXT("target_asset"), TEXT("cloth_asset"), TEXT("outfit_asset"), TEXT("dataflow_asset"), TEXT("asset_path"), TEXT("target")})
		{
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				return Value;
			}
		}
		return FString();
	}

	static TSharedRef<FJsonObject> InputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target cloth, outfit, dataflow, skeletal mesh, or material asset path."))},
			{TEXT("cloth_asset"), FSololmcpSchemaBuilder::String(TEXT("Chaos cloth asset path."))},
			{TEXT("outfit_asset"), FSololmcpSchemaBuilder::String(TEXT("Chaos outfit asset path."))},
			{TEXT("dataflow_asset"), FSololmcpSchemaBuilder::String(TEXT("Dataflow asset path."))},
			{TEXT("source_mesh"), FSololmcpSchemaBuilder::String(TEXT("Source skeletal/static mesh asset path."))},
			{TEXT("body_asset"), FSololmcpSchemaBuilder::String(TEXT("Target body/skeleton compatibility asset path."))},
			{TEXT("material_asset"), FSololmcpSchemaBuilder::String(TEXT("Material asset path for binding audits."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder to scan, default /Game."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Dependency assets."))},
			{TEXT("node_type"), FSololmcpSchemaBuilder::String(TEXT("Dataflow node type/name."))},
			{TEXT("weightmap_name"), FSololmcpSchemaBuilder::String(TEXT("Cloth/Dataflow weightmap name."))},
			{TEXT("fabric_param"), FSololmcpSchemaBuilder::String(TEXT("Fabric parameter name."))},
			{TEXT("fabric_value"), FSololmcpSchemaBuilder::String(TEXT("Fabric parameter value."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Receipt to validate."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum assets to inspect."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation. P1 cloth/outfit tools fail closed until dedicated writers have live fixture proof."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return probe/plan only. Default true."))}
		});
	}

	static bool ReceiptBool(const TSharedRef<FJsonObject>& Receipt, const TCHAR* FieldName)
	{
		bool bValue = false;
		return Receipt->TryGetBoolField(FieldName, bValue) && bValue;
	}

	static bool ReceiptHasAny(const TSharedRef<FJsonObject>& Receipt, const TArray<FString>& Fields)
	{
		for (const FString& Field : Fields)
		{
			if (Receipt->HasField(Field))
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
		bValid &= bPass;
	}

	static bool ExecuteReceiptTool(const FSpec& Spec, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			Out->SetStringField(TEXT("status"), TEXT("receipt_required"));
			Out->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			Summary = FString::Printf(TEXT("%s returned receipt requirements."), *Spec.Name);
			return true;
		}

		const TSharedRef<FJsonObject> Receipt = ReceiptPtr->ToSharedRef();
		TArray<TSharedPtr<FJsonValue>> Checks;
		bool bValid = true;
		const bool bTarget = ReceiptHasAny(Receipt, {TEXT("target_asset"), TEXT("cloth_asset"), TEXT("outfit_asset"), TEXT("dataflow_asset"), TEXT("asset_path"), TEXT("target_binding")});
		AddCheck(Checks, bValid, TEXT("target_binding"), bTarget, bTarget ? TEXT("Target binding found.") : TEXT("Missing target binding."));
		if (Spec.Name == TEXT("cloth_sim_preview_receipt") || Spec.Name == TEXT("outfit_preview_capture"))
		{
			const bool bPreview = ReceiptBool(Receipt, TEXT("preview_ok")) || ReceiptHasAny(Receipt, {TEXT("screenshot"), TEXT("preview_image"), TEXT("render_receipt"), TEXT("simulation_preview")});
			AddCheck(Checks, bValid, TEXT("preview_evidence"), bPreview, bPreview ? TEXT("Preview evidence found.") : TEXT("Missing preview evidence."));
		}
		else
		{
			const bool bProof = ReceiptBool(Receipt, TEXT("compile_ok")) || ReceiptBool(Receipt, TEXT("validation_ok")) || ReceiptBool(Receipt, TEXT("simulation_ok")) || ReceiptHasAny(Receipt, {
				TEXT("dataflow_compile"), TEXT("cloth_readback"), TEXT("outfit_readback"), TEXT("weightmap_readback"), TEXT("material_slot_map"), TEXT("skin_weight_audit")
			});
			AddCheck(Checks, bValid, TEXT("compile_validation_or_readback"), bProof, bProof ? TEXT("Compile/validation/readback evidence found.") : TEXT("Missing compile/validation/readback evidence."));
		}
		Out->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetArrayField(TEXT("checks"), Checks);
		Summary = bValid ? FString::Printf(TEXT("%s passed."), *Spec.Name) : FString::Printf(TEXT("%s failed closed."), *Spec.Name);
		return bValid;
	}

	static bool ExecuteTool(const FSpec& Spec, const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("tool_name"), Spec.Name);
		Out->SetStringField(TEXT("domain"), TEXT("cloth_outfit_dataflow"));
		Out->SetStringField(TEXT("family"), TEXT("cloth_outfit_dataflow"));
		Out->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		Out->SetStringField(TEXT("mode"), Spec.Mode);
		Out->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_or_editor_write_plan") : TEXT("read_or_validate"));
		Out->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));
		Out->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		Out->SetStringField(TEXT("minimum_engine_version"), TEXT("5.8.0"));
		Out->SetBoolField(TEXT("version_satisfied"), IsUE58OrLater());

		FString Availability;
		ProbeAvailability(Out, Availability);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		FString FolderPath;
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		int32 MaxAssets = 50;
		double MaxAssetsRaw = 50.0;
		if (Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssetsRaw))
		{
			MaxAssets = FMath::Clamp(static_cast<int32>(MaxAssetsRaw), 1, 500);
		}
		FString NodeType;
		FString WeightmapName;
		FString FabricParam;
		FString FabricValue;
		Arguments->TryGetStringField(TEXT("node_type"), NodeType);
		Arguments->TryGetStringField(TEXT("weightmap_name"), WeightmapName);
		Arguments->TryGetStringField(TEXT("fabric_param"), FabricParam);
		Arguments->TryGetStringField(TEXT("fabric_value"), FabricValue);
		const FString AssetPath = TargetAsset(Arguments);
		Out->SetBoolField(TEXT("execute_requested"), bExecute);
		Out->SetStringField(TEXT("target_asset"), AssetPath);
		Out->SetStringField(TEXT("folder_path"), FolderPath.IsEmpty() ? TEXT("/Game") : FolderPath);
		Out->SetStringField(TEXT("node_type"), NodeType);
		Out->SetStringField(TEXT("weightmap_name"), WeightmapName);
		Out->SetStringField(TEXT("fabric_param"), FabricParam);
		Out->SetStringField(TEXT("fabric_value"), FabricValue);
		Out->SetArrayField(TEXT("asset_paths"), StringArrayJson(GetStringArrayField(Arguments, TEXT("asset_paths"))));
		Out->SetObjectField(TEXT("target_asset_summary"), AssetSummaryJson(Context, AssetPath));
		Out->SetArrayField(TEXT("class_catalog"), CatalogJson(Context, Spec.FocusIds));
		Out->SetArrayField(TEXT("asset_scan"), AssetScanJson(Spec.AssetNeedles, FolderPath, MaxAssets));
		Out->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		Out->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		Out->SetArrayField(TEXT("ue57_fallback_tools"), StringArrayJson({TEXT("cloth_inspect"), TEXT("cloth_set_simulation"), TEXT("skeletal_mesh_list_lods"), TEXT("material_slot_audit")}));

		if (!IsUE58OrLater())
		{
			Out->SetArrayField(TEXT("plan_steps"), StringArrayJson({TEXT("Do not call UE 5.8 Cloth/Outfit/Dataflow APIs on UE 5.7."), TEXT("Route to legacy cloth and skeletal mesh audit tools.")}));
			Summary = FString::Printf(TEXT("%s requires UE 5.8; current engine is %s."), *Spec.Name, *CurrentEngineVersionString());
			return true;
		}

		if (Spec.Mode == TEXT("receipt") || Spec.Mode == TEXT("compile_gate"))
		{
			return ExecuteReceiptTool(Spec, Arguments, Out, Summary, Error);
		}
		if (Spec.Mode == TEXT("schema"))
		{
			Out->SetArrayField(TEXT("schema_fields"), StringArrayJson({TEXT("cloth_groups"), TEXT("sim_patterns"), TEXT("render_patterns"), TEXT("weightmaps"), TEXT("seams"), TEXT("fabric_params"), TEXT("collision"), TEXT("materials")}));
		}
		if (Spec.Mode == TEXT("dataflow"))
		{
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetArrayField(TEXT("node_categories"), StringArrayJson({TEXT("import"), TEXT("selection"), TEXT("weightmap"), TEXT("simulation"), TEXT("outfit"), TEXT("render"), TEXT("export")}));
			Contract->SetArrayField(TEXT("required_receipts"), StringArrayJson({TEXT("dataflow graph snapshot"), TEXT("node readback"), TEXT("compile/validate receipt"), TEXT("preview screenshot")}));
			Out->SetObjectField(TEXT("dataflow_contract"), Contract);
		}
		if (Spec.Mode == TEXT("outfit"))
		{
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetArrayField(TEXT("required_bindings"), StringArrayJson({TEXT("source mesh"), TEXT("body compatibility asset"), TEXT("cloth asset"), TEXT("material slot map"), TEXT("skin weights")}));
			Contract->SetArrayField(TEXT("required_receipts"), StringArrayJson({TEXT("outfit asset readback"), TEXT("body compatibility audit"), TEXT("preview screenshot")}));
			Out->SetObjectField(TEXT("outfit_contract"), Contract);
		}
		if (Spec.Mode == TEXT("audit") || Spec.Mode == TEXT("snapshot"))
		{
			Out->SetArrayField(TEXT("audit_fields"), StringArrayJson({TEXT("lods"), TEXT("sections"), TEXT("materials"), TEXT("weightmaps"), TEXT("collision"), TEXT("fabric"), TEXT("simulation"), TEXT("dataflow")}));
		}

		if (bExecute && Availability != TEXT("available"))
		{
			Out->SetBoolField(TEXT("success"), false);
			Out->SetStringField(TEXT("status"), Availability);
			Out->SetStringField(TEXT("failure_route"), TEXT("enable_ue58_cloth_outfit_dataflow_plugins_then_retry"));
			Error = FString::Printf(TEXT("%s cannot execute because Cloth/Outfit/Dataflow gate is %s."), *Spec.Name, *Availability);
			Summary = Error;
			return false;
		}
		if (bExecute && Spec.bMutation)
		{
			Out->SetBoolField(TEXT("success"), false);
			Out->SetStringField(TEXT("status"), TEXT("blocked_pending_cloth_outfit_dataflow_writer"));
			Out->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_cloth_outfit_dataflow_writer_after_live_fixture_and_receipt"));
			Error = FString::Printf(TEXT("%s blocked_pending_cloth_outfit_dataflow_writer: concrete Cloth/Outfit/Dataflow plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			Summary = Error;
			return false;
		}

		const bool bCompletedRead = Spec.Mode == TEXT("inspect") || Spec.Mode == TEXT("schema") || Spec.Mode == TEXT("audit") || Spec.Mode == TEXT("snapshot") || Spec.Mode == TEXT("compile_gate");
		Out->SetStringField(TEXT("status"), bCompletedRead ? TEXT("completed") : TEXT("dry_run"));
		Summary = FString::Printf(TEXT("%s returned Cloth/Outfit/Dataflow %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TArray<FSpec> Specs()
	{
		const TArray<FString> ClothFocus{TEXT("cloth_asset"), TEXT("cloth_runtime"), TEXT("cloth_editor"), TEXT("cloth_dataflow")};
		const TArray<FString> OutfitFocus{TEXT("outfit"), TEXT("outfit_editor"), TEXT("cloth_asset"), TEXT("skin_weight")};
		const TArray<FString> DataflowFocus{TEXT("dataflow_runtime"), TEXT("dataflow_editor"), TEXT("cloth_dataflow"), TEXT("weightmap"), TEXT("mesh_selection")};
		const TArray<FString> Needles{TEXT("Cloth"), TEXT("Outfit"), TEXT("Dataflow"), TEXT("Chaos")};
		const TArray<FString> ClothReceipt{TEXT("target cloth/outfit/dataflow asset"), TEXT("readback or graph snapshot"), TEXT("compile/validate/preview receipt")};
		return {
			{TEXT("cloth_asset_inspect"), TEXT("Inspect Chaos cloth asset, component, and editor class gates."), TEXT("inspect"), TEXT("cloth_asset"), false, ClothFocus, Needles, {TEXT("Resolve cloth asset target."), TEXT("Return class catalog, asset summary, and scan evidence.")}, ClothReceipt},
			{TEXT("cloth_collection_schema_inspect"), TEXT("Inspect cloth collection schema contract."), TEXT("schema"), TEXT("collection_schema"), false, ClothFocus, Needles, {TEXT("Return collection schema fields and class gates."), TEXT("Use schema before fabric, seams, pattern, and weightmap writes.")}, ClothReceipt},
			{TEXT("cloth_fabric_params_get"), TEXT("Read/plan cloth fabric parameter snapshot."), TEXT("snapshot"), TEXT("fabric"), false, ClothFocus, Needles, {TEXT("Resolve fabric parameter target."), TEXT("Return snapshot/readback requirements.")}, ClothReceipt},
			{TEXT("cloth_fabric_params_set"), TEXT("Plan cloth fabric parameter update."), TEXT("plan"), TEXT("fabric"), true, ClothFocus, Needles, {TEXT("Resolve parameter name/value."), TEXT("Plan scoped update, readback, and preview.")}, ClothReceipt},
			{TEXT("cloth_sim_pattern_list"), TEXT("List/plan cloth simulation pattern readback."), TEXT("snapshot"), TEXT("sim_pattern"), false, ClothFocus, Needles, {TEXT("Resolve sim pattern source."), TEXT("Return pattern readback contract.")}, ClothReceipt},
			{TEXT("cloth_render_pattern_list"), TEXT("List/plan cloth render pattern readback."), TEXT("snapshot"), TEXT("render_pattern"), false, ClothFocus, Needles, {TEXT("Resolve render pattern source."), TEXT("Return render pattern/material readback contract.")}, ClothReceipt},
			{TEXT("cloth_seams_inspect"), TEXT("Inspect cloth seams and sewing constraints."), TEXT("audit"), TEXT("seams"), false, ClothFocus, Needles, {TEXT("Resolve seam data target."), TEXT("Return seam audit contract and preview requirements.")}, ClothReceipt},
			{TEXT("cloth_weightmap_paint_plan"), TEXT("Plan Dataflow cloth weightmap paint/update."), TEXT("dataflow"), TEXT("weightmap"), true, DataflowFocus, Needles, {TEXT("Resolve weightmap and Dataflow target."), TEXT("Plan paint/update route."), TEXT("Require weightmap readback and preview.")}, ClothReceipt},
			{TEXT("cloth_dataflow_graph_create"), TEXT("Plan cloth Dataflow graph creation."), TEXT("dataflow"), TEXT("graph_create"), true, DataflowFocus, Needles, {TEXT("Resolve package and source mesh."), TEXT("Plan Dataflow asset graph creation."), TEXT("Require graph readback and compile receipt.")}, ClothReceipt},
			{TEXT("cloth_dataflow_node_add"), TEXT("Plan cloth Dataflow node insertion."), TEXT("dataflow"), TEXT("node_add"), true, DataflowFocus, Needles, {TEXT("Resolve Dataflow asset and node type."), TEXT("Plan node insertion and wiring."), TEXT("Require graph snapshot and compile receipt.")}, ClothReceipt},
			{TEXT("cloth_sim_preview_receipt"), TEXT("Validate cloth simulation preview receipt."), TEXT("receipt"), TEXT("preview"), false, ClothFocus, Needles, {}, {TEXT("target binding"), TEXT("simulation preview screenshot"), TEXT("cloth readback")}},
			{TEXT("outfit_asset_create"), TEXT("Plan Chaos outfit asset creation."), TEXT("outfit"), TEXT("outfit_create"), true, OutfitFocus, Needles, {TEXT("Resolve source mesh/body/cloth inputs."), TEXT("Plan outfit package creation."), TEXT("Require outfit readback and preview.")}, ClothReceipt},
			{TEXT("outfit_source_add"), TEXT("Plan source addition to outfit asset."), TEXT("outfit"), TEXT("outfit_source"), true, OutfitFocus, Needles, {TEXT("Resolve outfit and source asset."), TEXT("Plan source add and compatibility checks.")}, ClothReceipt},
			{TEXT("outfit_size_variants_set"), TEXT("Plan outfit size variant setup."), TEXT("outfit"), TEXT("size_variants"), true, OutfitFocus, Needles, {TEXT("Resolve body sizes and variant names."), TEXT("Plan deterministic variant mapping."), TEXT("Require readback and preview.")}, ClothReceipt},
			{TEXT("outfit_skin_weight_audit"), TEXT("Audit outfit skin weights."), TEXT("audit"), TEXT("skin_weight"), false, OutfitFocus, Needles, {TEXT("Resolve outfit/body mesh."), TEXT("Return skin weight audit contract.")}, ClothReceipt},
			{TEXT("outfit_receipt_validate"), TEXT("Validate outfit production receipt."), TEXT("receipt"), TEXT("receipt"), false, OutfitFocus, Needles, {}, ClothReceipt},
			{TEXT("cloth_weightmap_audit_v2"), TEXT("Audit cloth weightmaps."), TEXT("audit"), TEXT("weightmap"), false, DataflowFocus, Needles, {TEXT("Resolve weightmap sources."), TEXT("Return weightmap readback and preview requirements.")}, ClothReceipt},
			{TEXT("cloth_collision_config_get"), TEXT("Read cloth collision config snapshot."), TEXT("snapshot"), TEXT("collision"), false, ClothFocus, Needles, {TEXT("Resolve collision target."), TEXT("Return collision config readback contract.")}, ClothReceipt},
			{TEXT("cloth_collision_config_set"), TEXT("Plan cloth collision config update."), TEXT("plan"), TEXT("collision"), true, ClothFocus, Needles, {TEXT("Resolve collision settings."), TEXT("Plan scoped update and simulation preview.")}, ClothReceipt},
			{TEXT("cloth_sim_params_snapshot"), TEXT("Read cloth simulation parameter snapshot."), TEXT("snapshot"), TEXT("simulation"), false, ClothFocus, Needles, {TEXT("Resolve sim params target."), TEXT("Return simulation parameter contract.")}, ClothReceipt},
			{TEXT("cloth_lod_section_bind"), TEXT("Plan cloth LOD/section binding."), TEXT("plan"), TEXT("lod_section"), true, ClothFocus, Needles, {TEXT("Resolve LOD and section mapping."), TEXT("Plan material/section binding and readback.")}, ClothReceipt},
			{TEXT("cloth_material_binding_audit"), TEXT("Audit cloth material bindings."), TEXT("audit"), TEXT("materials"), false, ClothFocus, {TEXT("Cloth"), TEXT("Material"), TEXT("Outfit")}, {TEXT("Resolve material slots."), TEXT("Return material slot map and preview requirements.")}, ClothReceipt},
			{TEXT("cloth_dataflow_compile_validate"), TEXT("Validate cloth Dataflow compile receipt."), TEXT("compile_gate"), TEXT("compile"), false, DataflowFocus, Needles, {}, ClothReceipt},
			{TEXT("outfit_body_compat_audit"), TEXT("Audit outfit/body compatibility."), TEXT("audit"), TEXT("body_compat"), false, OutfitFocus, Needles, {TEXT("Resolve body/user data and outfit."), TEXT("Return compatibility audit contract.")}, ClothReceipt},
			{TEXT("outfit_material_slot_map"), TEXT("Plan/read outfit material slot map."), TEXT("audit"), TEXT("material_slots"), false, OutfitFocus, {TEXT("Outfit"), TEXT("Material")}, {TEXT("Resolve outfit asset."), TEXT("Return slot map/readback requirements.")}, ClothReceipt},
			{TEXT("outfit_preview_capture"), TEXT("Validate/plan outfit preview capture."), TEXT("receipt"), TEXT("preview"), false, OutfitFocus, Needles, {}, {TEXT("target binding"), TEXT("preview screenshot"), TEXT("outfit/material/body readback")}}
		};
	}

	static void RegisterOne(FSololmcpToolRegistry& Registry, const FSpec& Spec)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = Spec.Description;
		Def.InputSchema = InputSchema();
		Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
		Def.Execute = [Spec](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return ExecuteTool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}

void RegisterClothOutfitDataflowP1Tools(FSololmcpToolRegistry& Registry)
{
	for (const ClothOutfitDataflowP1::FSpec& Spec : ClothOutfitDataflowP1::Specs())
	{
		ClothOutfitDataflowP1::RegisterOne(Registry, Spec);
	}
}
}
