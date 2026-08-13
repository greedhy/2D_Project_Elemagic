// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpCharacterCustomizationTools.cpp
// ----------------------------------------------------------------------------
// UE 5.7+ MetaHuman / Mutable P1 concrete probes, production plans, and
// receipt gates. Optional plugin APIs are reached through runtime reflection and
// asset-registry evidence only; this file intentionally avoids direct headers
// from MetaHuman, Mutable, RigLogic, or their experimental UE 5.8 modules.
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
namespace CharacterCustomizationP1
{
	struct FCatalogRow
	{
		FString Id;
		FString ObjectPath;
		FString Kind;
		FString Category;
	};

	struct FCharacterCustomizationSpec
	{
		FString Name;
		FString Description;
		FString Mode;
		FString Subdomain;
		bool bMutation = false;
		TArray<FString> GatePlugins;
		TArray<FString> GateModules;
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

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
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

	static bool ProbeAvailability(
		const TArray<FString>& Plugins,
		const TArray<FString>& Modules,
		TSharedRef<FJsonObject>& Out,
		FString& OutStatus)
	{
		TArray<TSharedPtr<FJsonValue>> PluginsJson;
		bool bAnyPluginFound = Plugins.IsEmpty();
		bool bAnyPluginEnabled = Plugins.IsEmpty();
		for (const FString& PluginName : Plugins)
		{
			TSharedRef<FJsonObject> Probe = PluginProbeJson(PluginName);
			bool bFound = false;
			bool bEnabled = false;
			Probe->TryGetBoolField(TEXT("found"), bFound);
			Probe->TryGetBoolField(TEXT("enabled"), bEnabled);
			bAnyPluginFound = bAnyPluginFound || bFound;
			bAnyPluginEnabled = bAnyPluginEnabled || bEnabled;
			PluginsJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		TArray<TSharedPtr<FJsonValue>> ModulesJson;
		bool bAnyModuleExists = Modules.IsEmpty();
		for (const FString& ModuleName : Modules)
		{
			TSharedRef<FJsonObject> Probe = ModuleProbeJson(ModuleName);
			bool bExists = false;
			Probe->TryGetBoolField(TEXT("exists"), bExists);
			bAnyModuleExists = bAnyModuleExists || bExists;
			ModulesJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		if (!bAnyPluginFound)
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
		Out->SetArrayField(TEXT("plugins"), PluginsJson);
		Out->SetArrayField(TEXT("modules"), ModulesJson);
		return OutStatus == TEXT("available");
	}

	static TSharedRef<FJsonObject> ClassStatusJson(const FSololmcpToolExecutionContext& Context, const FCatalogRow& Row)
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

	static TArray<FCatalogRow> Catalog()
	{
		return {
			{TEXT("MetaHumanCharacter"), TEXT("/Script/MetaHumanCharacter.MetaHumanCharacter"), TEXT("class"), TEXT("metahuman_character")},
			{TEXT("MetaHumanCharacterInstance"), TEXT("/Script/MetaHumanCharacterPalette.MetaHumanCharacterInstance"), TEXT("class"), TEXT("metahuman_character")},
			{TEXT("MetaHumanCollection"), TEXT("/Script/MetaHumanCharacterPalette.MetaHumanCollection"), TEXT("class"), TEXT("metahuman_character")},
			{TEXT("MetaHumanWardrobeItem"), TEXT("/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem"), TEXT("class"), TEXT("wardrobe")},
			{TEXT("MetaHumanCharacterExportBlueprintLibrary"), TEXT("/Script/MetaHumanCharacterEditor.MetaHumanCharacterExportBlueprintLibrary"), TEXT("class"), TEXT("export")},
			{TEXT("MetaHumanCharacterInstanceBlueprintLibrary"), TEXT("/Script/MetaHumanCharacterPaletteEditor.MetaHumanCharacterInstanceBlueprintLibrary"), TEXT("class"), TEXT("character_instance")},
			{TEXT("MetaHumanCharacterInstanceParameterBlueprintLibrary"), TEXT("/Script/MetaHumanCharacterPaletteEditor.MetaHumanCharacterInstanceParameterBlueprintLibrary"), TEXT("class"), TEXT("character_parameters")},
			{TEXT("MetaHumanAssetReport"), TEXT("/Script/MetaHumanSDKEditor.MetaHumanAssetReport"), TEXT("class"), TEXT("asset_report")},
			{TEXT("MetaHumanAssetManager"), TEXT("/Script/MetaHumanSDKEditor.MetaHumanAssetManager"), TEXT("class"), TEXT("asset_report")},
			{TEXT("MetaHumanVerificationRuleCollection"), TEXT("/Script/MetaHumanSDKEditor.MetaHumanVerificationRuleCollection"), TEXT("class"), TEXT("asset_report")},
			{TEXT("MetaHumanComponentUE"), TEXT("/Script/MetaHumanSDKRuntime.MetaHumanComponentUE"), TEXT("class"), TEXT("runtime_component")},
			{TEXT("MetaHumanComponentBase"), TEXT("/Script/MetaHumanSDKRuntime.MetaHumanComponentBase"), TEXT("class"), TEXT("runtime_component")},
			{TEXT("MetaHumanLocalLiveLinkSubjectSettings"), TEXT("/Script/MetaHumanLocalLiveLinkSource.MetaHumanLocalLiveLinkSubjectSettings"), TEXT("class"), TEXT("livelink")},
			{TEXT("MetaHumanVideoLiveLinkSourceFactory"), TEXT("/Script/MetaHumanLocalLiveLinkSource.MetaHumanVideoLiveLinkSourceFactory"), TEXT("class"), TEXT("livelink")},
			{TEXT("MetaHumanCalibrationGenerator"), TEXT("/Script/MetaHumanCalibrationGenerator.MetaHumanCalibrationGenerator"), TEXT("class"), TEXT("calibration")},
			{TEXT("MetaHumanCalibrationBatchLibrary"), TEXT("/Script/MetaHumanCalibrationGenerator.MetaHumanCalibrationBatchLibrary"), TEXT("class"), TEXT("calibration")},
			{TEXT("MetaHumanDiagnosticsBasedSelector"), TEXT("/Script/MetaHumanCalibrationDiagnostics.MetaHumanDiagnosticsBasedSelector"), TEXT("class"), TEXT("diagnostics")},
			{TEXT("MetaHumanCrowdAnimationConfig"), TEXT("/Script/MetaHumanCrowdEditor.MetaHumanCrowdAnimationConfig"), TEXT("class"), TEXT("crowd")},
			{TEXT("MetaHumanMassUAFProcessor"), TEXT("/Script/MetaHumanCrowd.MetaHumanMassUAFProcessor"), TEXT("class"), TEXT("crowd_mass")},
			{TEXT("MetaHumanMassUpdateActorIdentityProcessor"), TEXT("/Script/MetaHumanCrowd.MetaHumanMassUpdateActorIdentityProcessor"), TEXT("class"), TEXT("crowd_mass")},
			{TEXT("DNA"), TEXT("/Script/RigLogicModule.DNA"), TEXT("class"), TEXT("dna")},
			{TEXT("DNAAsset"), TEXT("/Script/RigLogicModule.DNAAsset"), TEXT("class"), TEXT("dna")},
			{TEXT("DNAAssetUserData"), TEXT("/Script/RigLogicModule.DNAAssetUserData"), TEXT("class"), TEXT("dna")},
			{TEXT("DNAImporterLibrary"), TEXT("/Script/RigLogicEditor.DNAImporterLibrary"), TEXT("class"), TEXT("dna_import_export")},
			{TEXT("CustomizableObject"), TEXT("/Script/CustomizableObject.CustomizableObject"), TEXT("class"), TEXT("mutable")},
			{TEXT("CustomizableObjectInstance"), TEXT("/Script/CustomizableObject.CustomizableObjectInstance"), TEXT("class"), TEXT("mutable_instance")},
			{TEXT("CustomizableObjectInstanceUsage"), TEXT("/Script/CustomizableObject.CustomizableObjectInstanceUsage"), TEXT("class"), TEXT("mutable_instance")},
			{TEXT("CustomizableObjectInstanceUserData"), TEXT("/Script/CustomizableObject.CustomizableObjectInstanceUserData"), TEXT("class"), TEXT("mutable_instance")},
			{TEXT("CustomizableObjectSystem"), TEXT("/Script/CustomizableObject.CustomizableObjectSystem"), TEXT("class"), TEXT("mutable_runtime")},
			{TEXT("CustomizableSkeletalComponent"), TEXT("/Script/CustomizableObject.CustomizableSkeletalComponent"), TEXT("class"), TEXT("mutable_runtime")},
			{TEXT("CustomizableSkeletalMeshActor"), TEXT("/Script/CustomizableObject.CustomizableSkeletalMeshActor"), TEXT("class"), TEXT("mutable_runtime")},
			{TEXT("CustomizableObjectSkeletalMesh"), TEXT("/Script/CustomizableObject.CustomizableObjectSkeletalMesh"), TEXT("class"), TEXT("mutable_runtime")},
			{TEXT("CustomizableObjectExtension"), TEXT("/Script/CustomizableObject.CustomizableObjectExtension"), TEXT("class"), TEXT("mutable_extension")},
			{TEXT("CustomizableObjectFactory"), TEXT("/Script/CustomizableObjectEditor.CustomizableObjectFactory"), TEXT("class"), TEXT("mutable_editor")},
			{TEXT("CustomizableObjectInstanceFactory"), TEXT("/Script/CustomizableObjectEditor.CustomizableObjectInstanceFactory"), TEXT("class"), TEXT("mutable_editor")},
			{TEXT("CustomizableObjectEditorFunctionLibrary"), TEXT("/Script/CustomizableObjectEditor.CustomizableObjectEditorFunctionLibrary"), TEXT("class"), TEXT("mutable_editor")},
			{TEXT("MutableValidationSettings"), TEXT("/Script/MutableValidation.MutableValidationSettings"), TEXT("class"), TEXT("mutable_validation")},
			{TEXT("CustomizableObjectValidationCommandlet"), TEXT("/Script/MutableValidation.CustomizableObjectValidationCommandlet"), TEXT("class"), TEXT("mutable_validation")},
			{TEXT("CustomizableObjectBulkValidationCommandlet"), TEXT("/Script/MutableValidation.CustomizableObjectBulkValidationCommandlet"), TEXT("class"), TEXT("mutable_validation")},
			{TEXT("AssetValidator_CustomizableObjects"), TEXT("/Script/MutableValidation.AssetValidator_CustomizableObjects"), TEXT("class"), TEXT("mutable_validation")},
			{TEXT("AssetValidator_ReferencedCustomizableObjects"), TEXT("/Script/MutableValidation.AssetValidator_ReferencedCustomizableObjects"), TEXT("class"), TEXT("mutable_validation")},
			{TEXT("CustomizableObjectPopulation"), TEXT("/Script/CustomizableObjectPopulation.CustomizableObjectPopulation"), TEXT("class"), TEXT("mutable_population")},
			{TEXT("CustomizableObjectPopulationClass"), TEXT("/Script/CustomizableObjectPopulation.CustomizableObjectPopulationClass"), TEXT("class"), TEXT("mutable_population")},
			{TEXT("CustomizableObjectPopulationGenerator"), TEXT("/Script/CustomizableObjectPopulation.CustomizableObjectPopulationGenerator"), TEXT("class"), TEXT("mutable_population")},
			{TEXT("CustomizableObjectPopulationFactory"), TEXT("/Script/CustomizableObjectPopulationEditor.CustomizableObjectPopulationFactory"), TEXT("class"), TEXT("mutable_population_editor")},
			{TEXT("CustomizableObjectPopulationClassFactory"), TEXT("/Script/CustomizableObjectPopulationEditor.CustomizableObjectPopulationClassFactory"), TEXT("class"), TEXT("mutable_population_editor")}
		};
	}

	static TArray<TSharedPtr<FJsonValue>> CatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusIds = {})
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

	static bool AssetMatchesNeedles(const FAssetData& AssetData, const TArray<FString>& Needles)
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

		TArray<TSharedPtr<FJsonValue>> Rows;
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*FolderPath));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);
		for (const FAssetData& AssetData : Assets)
		{
			if (!AssetMatchesNeedles(AssetData, Needles))
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
		for (const TCHAR* Field : {TEXT("target_asset"), TEXT("character_asset"), TEXT("customizable_object"), TEXT("instance_asset"), TEXT("population_asset"), TEXT("asset_path"), TEXT("target")})
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
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target MetaHuman, Mutable, DNA, population, or character asset path."))},
			{TEXT("character_asset"), FSololmcpSchemaBuilder::String(TEXT("Alias for MetaHuman character asset path."))},
			{TEXT("customizable_object"), FSololmcpSchemaBuilder::String(TEXT("Mutable CustomizableObject asset path."))},
			{TEXT("instance_asset"), FSololmcpSchemaBuilder::String(TEXT("Mutable instance asset path."))},
			{TEXT("population_asset"), FSololmcpSchemaBuilder::String(TEXT("Mutable population asset path."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder to scan, default /Game."))},
			{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Dependency assets such as skeletal meshes, grooms, DNA, materials, textures, outfits, or poses."))},
			{TEXT("parameter_name"), FSololmcpSchemaBuilder::String(TEXT("Mutable/MetaHuman parameter name."))},
			{TEXT("parameter_value"), FSololmcpSchemaBuilder::String(TEXT("Mutable/MetaHuman parameter value serialized as string."))},
			{TEXT("subject_name"), FSololmcpSchemaBuilder::String(TEXT("LiveLink subject name or source label."))},
			{TEXT("body_type"), FSololmcpSchemaBuilder::String(TEXT("MetaHuman body type or compatibility target."))},
			{TEXT("export_format"), FSololmcpSchemaBuilder::String(TEXT("Export format such as dcc, dna, geometry, materials."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Receipt to validate."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum assets to inspect."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation. P1 character customization tools fail closed until dedicated writers have live fixture proof."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return probe/plan only. Default true."))}
		});
	}

	static bool ReceiptBool(const TSharedRef<FJsonObject>& Receipt, const TCHAR* FieldName)
	{
		bool bValue = false;
		return Receipt->TryGetBoolField(FieldName, bValue) && bValue;
	}

	static bool ReceiptHasAny(const TSharedRef<FJsonObject>& Receipt, const TArray<FString>& FieldNames)
	{
		for (const FString& FieldName : FieldNames)
		{
			if (Receipt->HasField(FieldName))
			{
				return true;
			}
		}
		return false;
	}

	static void AddCheck(TArray<TSharedPtr<FJsonValue>>& Checks, bool& bValid, const FString& Name, const bool bPass, const FString& Detail)
	{
		TSharedRef<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("pass"), bPass);
		Check->SetStringField(TEXT("detail"), Detail);
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		bValid &= bPass;
	}

	static bool ExecuteReceiptTool(
		const FCharacterCustomizationSpec& Spec,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			OutStructured->SetStringField(TEXT("status"), TEXT("receipt_required"));
			OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
			OutSummary = FString::Printf(TEXT("%s returned receipt requirements."), *Spec.Name);
			return true;
		}

		const TSharedRef<FJsonObject> Receipt = ReceiptPtr->ToSharedRef();
		TArray<TSharedPtr<FJsonValue>> Checks;
		bool bValid = true;
		const bool bHasTarget = ReceiptHasAny(Receipt, {
			TEXT("target_asset"), TEXT("character_asset"), TEXT("customizable_object"), TEXT("instance_asset"),
			TEXT("population_asset"), TEXT("asset_path"), TEXT("target_binding")
		});
		AddCheck(Checks, bValid, TEXT("target_binding"), bHasTarget, bHasTarget ? TEXT("Target binding found.") : TEXT("Missing target binding."));

		if (Spec.Name == TEXT("metahuman_receipt_validate"))
		{
			const bool bReportOrExport = ReceiptBool(Receipt, TEXT("verification_ok")) || ReceiptBool(Receipt, TEXT("export_ok")) || ReceiptHasAny(Receipt, {
				TEXT("asset_report"), TEXT("export_receipt"), TEXT("dna_export"), TEXT("geometry_export"), TEXT("materials_export"), TEXT("preview_receipt")
			});
			AddCheck(Checks, bValid, TEXT("report_export_or_preview"), bReportOrExport, bReportOrExport ? TEXT("MetaHuman report/export/preview evidence found.") : TEXT("Missing MetaHuman report/export/preview evidence."));
		}
		else if (Spec.Name == TEXT("mutable_preview_receipt"))
		{
			const bool bPreview = ReceiptBool(Receipt, TEXT("preview_ok")) || ReceiptHasAny(Receipt, {TEXT("screenshot"), TEXT("preview_image"), TEXT("render_receipt"), TEXT("preview_receipt")});
			AddCheck(Checks, bValid, TEXT("preview_evidence"), bPreview, bPreview ? TEXT("Preview evidence found.") : TEXT("Missing preview screenshot/render evidence."));
		}
		else
		{
			const bool bMutableProof = ReceiptBool(Receipt, TEXT("compile_ok")) || ReceiptBool(Receipt, TEXT("validation_ok")) || ReceiptBool(Receipt, TEXT("generation_ok")) || ReceiptBool(Receipt, TEXT("update_ok")) || ReceiptHasAny(Receipt, {
				TEXT("compile_receipt"), TEXT("validation_receipt"), TEXT("generated_mesh"), TEXT("parameter_readback"), TEXT("dependency_graph"), TEXT("preview_receipt")
			});
			AddCheck(Checks, bValid, TEXT("compile_validation_or_generation"), bMutableProof, bMutableProof ? TEXT("Mutable compile/validation/generation evidence found.") : TEXT("Missing Mutable compile/validation/generation evidence."));
		}

		OutStructured->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		OutStructured->SetBoolField(TEXT("valid"), bValid);
		OutStructured->SetArrayField(TEXT("checks"), Checks);
		OutSummary = bValid ? FString::Printf(TEXT("%s passed."), *Spec.Name) : FString::Printf(TEXT("%s failed closed."), *Spec.Name);
		return bValid;
	}

	static bool ExecuteTool(
		const FCharacterCustomizationSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("tool_name"), Spec.Name);
		OutStructured->SetStringField(TEXT("domain"), TEXT("character_customization"));
		OutStructured->SetStringField(TEXT("family"), TEXT("metahuman_mutable"));
		OutStructured->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		OutStructured->SetStringField(TEXT("mode"), Spec.Mode);
		OutStructured->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_or_runtime_write_plan") : TEXT("read_or_validate"));
		OutStructured->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));
		OutStructured->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		OutStructured->SetStringField(TEXT("minimum_engine_version"), TEXT("5.7.0"));

		FString Availability;
		ProbeAvailability(Spec.GatePlugins, Spec.GateModules, OutStructured, Availability);

		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		OutStructured->SetBoolField(TEXT("execute_requested"), bExecute);
		const FString AssetPath = TargetAsset(Arguments);
		FString FolderPath;
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		int32 MaxAssets = 50;
		double MaxAssetsRaw = 50.0;
		if (Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssetsRaw))
		{
			MaxAssets = FMath::Clamp(static_cast<int32>(MaxAssetsRaw), 1, 500);
		}
		FString ParameterName;
		FString ParameterValue;
		FString SubjectName;
		FString BodyType;
		FString ExportFormat;
		Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName);
		Arguments->TryGetStringField(TEXT("parameter_value"), ParameterValue);
		Arguments->TryGetStringField(TEXT("subject_name"), SubjectName);
		Arguments->TryGetStringField(TEXT("body_type"), BodyType);
		Arguments->TryGetStringField(TEXT("export_format"), ExportFormat);

		OutStructured->SetStringField(TEXT("target_asset"), AssetPath);
		OutStructured->SetStringField(TEXT("folder_path"), FolderPath.IsEmpty() ? TEXT("/Game") : FolderPath);
		OutStructured->SetStringField(TEXT("parameter_name"), ParameterName);
		OutStructured->SetStringField(TEXT("parameter_value"), ParameterValue);
		OutStructured->SetStringField(TEXT("subject_name"), SubjectName);
		OutStructured->SetStringField(TEXT("body_type"), BodyType);
		OutStructured->SetStringField(TEXT("export_format"), ExportFormat);
		OutStructured->SetArrayField(TEXT("asset_paths"), StringArrayJson(GetStringArrayField(Arguments, TEXT("asset_paths"))));
		OutStructured->SetObjectField(TEXT("target_asset_summary"), AssetSummaryJson(Context, AssetPath));
		OutStructured->SetArrayField(TEXT("class_catalog"), CatalogJson(Context, Spec.FocusIds));
		OutStructured->SetArrayField(TEXT("asset_scan"), AssetScanJson(Spec.AssetNeedles, FolderPath, MaxAssets));
		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		OutStructured->SetArrayField(TEXT("related_plugins"), StringArrayJson({
			TEXT("MetaHuman"), TEXT("MetaHumanSDK"), TEXT("MetaHumanCharacter"), TEXT("MetaHumanCrowd"), TEXT("MetaHumanLiveLink"),
			TEXT("MetaHumanCalibrationProcessing"), TEXT("MetaHumanCalibrationDiagnostics"), TEXT("RigLogic"), TEXT("Mutable"), TEXT("MutablePopulation")
		}));

		if (Spec.Mode == TEXT("receipt"))
		{
			return ExecuteReceiptTool(Spec, Arguments, OutStructured, OutSummary, OutError);
		}

		if (Spec.Mode == TEXT("parameters"))
		{
			OutStructured->SetArrayField(TEXT("parameter_schema_fields"), StringArrayJson({
				TEXT("name"), TEXT("type"), TEXT("default_value"), TEXT("allowed_values"), TEXT("runtime_mutable"), TEXT("readback_required")
			}));
		}
		if (Spec.Mode == TEXT("export"))
		{
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetArrayField(TEXT("supported_exports"), StringArrayJson({TEXT("dcc"), TEXT("dna"), TEXT("geometry"), TEXT("materials")}));
			Contract->SetArrayField(TEXT("required_receipts"), StringArrayJson({TEXT("target MetaHuman character"), TEXT("export path"), TEXT("file/object readback"), TEXT("verification report")}));
			OutStructured->SetObjectField(TEXT("export_contract"), Contract);
		}
		if (Spec.Mode == TEXT("crowd"))
		{
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetArrayField(TEXT("bridge_tools"), StringArrayJson({TEXT("mass_entity_config_create"), TEXT("mass_spawner_create"), TEXT("uaf_component_attach"), TEXT("mutable_population_randomize_plan")}));
			Contract->SetArrayField(TEXT("required_receipts"), StringArrayJson({TEXT("variation manifest"), TEXT("Mass/Mover/UAF binding plan"), TEXT("preview screenshot"), TEXT("spawn budget")}));
			OutStructured->SetObjectField(TEXT("crowd_contract"), Contract);
		}
		if (Spec.Mode == TEXT("dependency_graph"))
		{
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetArrayField(TEXT("nodes"), StringArrayJson({TEXT("CustomizableObject"), TEXT("Instance"), TEXT("SkeletalMesh"), TEXT("Material"), TEXT("Texture"), TEXT("PopulationClass"), TEXT("GeneratedMesh")}));
			Contract->SetArrayField(TEXT("edges"), StringArrayJson({TEXT("uses"), TEXT("generates"), TEXT("overrides"), TEXT("depends_on"), TEXT("validates")}));
			OutStructured->SetObjectField(TEXT("dependency_graph_contract"), Contract);
		}
		if (Spec.Mode == TEXT("validation") || Spec.Mode == TEXT("compile"))
		{
			TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
			Contract->SetArrayField(TEXT("checks"), StringArrayJson({TEXT("plugin/module gate"), TEXT("asset load"), TEXT("class catalog"), TEXT("compile or commandlet result"), TEXT("post-validation readback")}));
			Contract->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_character_customization_writer"));
			OutStructured->SetObjectField(TEXT("validation_contract"), Contract);
		}

		if (bExecute && Availability != TEXT("available"))
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), Availability);
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("enable_required_character_customization_plugins_then_retry"));
			OutError = FString::Printf(TEXT("%s cannot execute because the character-customization plugin gate is %s."), *Spec.Name, *Availability);
			OutSummary = OutError;
			return false;
		}

		if (bExecute && Spec.bMutation)
		{
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_character_customization_writer"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_metahuman_mutable_writer_after_live_fixture_and_receipt"));
			OutError = FString::Printf(TEXT("%s blocked_pending_character_customization_writer: concrete MetaHuman/Mutable plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			OutSummary = OutError;
			return false;
		}

		const bool bCompletedRead = Spec.Mode == TEXT("inventory") || Spec.Mode == TEXT("inspect") || Spec.Mode == TEXT("report") || Spec.Mode == TEXT("audit") || Spec.Mode == TEXT("parameters") || Spec.Mode == TEXT("dependency_graph") || Spec.Mode == TEXT("compile");
		OutStructured->SetStringField(TEXT("status"), bCompletedRead ? TEXT("completed") : TEXT("dry_run"));
		OutSummary = FString::Printf(TEXT("%s returned MetaHuman/Mutable %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TArray<FCharacterCustomizationSpec> Specs()
	{
		const TArray<FString> MetaPlugins{
			TEXT("MetaHuman"), TEXT("MetaHumanSDK"), TEXT("MetaHumanCharacter"), TEXT("MetaHumanLiveLink"),
			TEXT("MetaHumanCalibrationProcessing"), TEXT("MetaHumanCalibrationDiagnostics"), TEXT("RigLogic")
		};
		const TArray<FString> MetaModules{
			TEXT("MetaHumanCharacter"), TEXT("MetaHumanCharacterPalette"), TEXT("MetaHumanSDKRuntime"), TEXT("MetaHumanSDKEditor"),
			TEXT("MetaHumanCalibrationGenerator"), TEXT("MetaHumanCalibrationDiagnostics"), TEXT("RigLogicModule")
		};
		const TArray<FString> CrowdPlugins{TEXT("MetaHumanCrowd"), TEXT("MetaHumanCharacter"), TEXT("MutablePopulation"), TEXT("MassEntity")};
		const TArray<FString> CrowdModules{TEXT("MetaHumanCrowd"), TEXT("MetaHumanCrowdEditor"), TEXT("CustomizableObjectPopulation"), TEXT("MassEntity")};
		const TArray<FString> LiveLinkPlugins{TEXT("MetaHumanLiveLink"), TEXT("LiveLink"), TEXT("MetaHumanSDK")};
		const TArray<FString> LiveLinkModules{TEXT("MetaHumanLiveLinkSource"), TEXT("MetaHumanLocalLiveLinkSource"), TEXT("LiveLinkInterface")};
		const TArray<FString> MutablePlugins{TEXT("Mutable"), TEXT("MutablePopulation")};
		const TArray<FString> MutableModules{TEXT("CustomizableObject"), TEXT("CustomizableObjectEditor"), TEXT("MutableValidation"), TEXT("CustomizableObjectPopulation")};
		const TArray<FString> MetaFocus{TEXT("metahuman_character"), TEXT("character_instance"), TEXT("asset_report"), TEXT("runtime_component"), TEXT("dna")};
		const TArray<FString> ExportFocus{TEXT("export"), TEXT("dna_import_export"), TEXT("dna"), TEXT("metahuman_character")};
		const TArray<FString> CrowdFocus{TEXT("crowd"), TEXT("crowd_mass"), TEXT("mutable_population"), TEXT("runtime_component")};
		const TArray<FString> MutableFocus{TEXT("mutable"), TEXT("mutable_instance"), TEXT("mutable_runtime"), TEXT("mutable_validation")};
		const TArray<FString> PopulationFocus{TEXT("mutable_population"), TEXT("mutable_population_editor"), TEXT("mutable_instance")};
		const TArray<FString> MetaNeedles{TEXT("MetaHuman"), TEXT("DNA"), TEXT("Groom"), TEXT("Wardrobe")};
		const TArray<FString> MutableNeedles{TEXT("CustomizableObject"), TEXT("Mutable"), TEXT("Population")};
		const TArray<FString> MetaReceipt{TEXT("target MetaHuman character"), TEXT("asset report/export/readback"), TEXT("preview or verification evidence")};
		const TArray<FString> MutableReceipt{TEXT("target CustomizableObject or instance"), TEXT("parameter/generation/compile readback"), TEXT("validation or preview receipt")};

		return {
			{TEXT("metahuman_character_assets_list"), TEXT("List MetaHuman, DNA, groom, wardrobe, and character assets under a folder."), TEXT("inventory"), TEXT("assets"), false, MetaPlugins, MetaModules, MetaFocus, MetaNeedles, {TEXT("Scan folder with asset-registry needles."), TEXT("Return MetaHuman/RigLogic class catalog."), TEXT("Use results to seed asset manifest and character tasks.")}, MetaReceipt},
			{TEXT("metahuman_character_instance_inspect"), TEXT("Inspect a MetaHuman character or character instance asset."), TEXT("inspect"), TEXT("character_instance"), false, MetaPlugins, MetaModules, MetaFocus, MetaNeedles, {TEXT("Load target character or instance."), TEXT("Sample editable properties and class availability."), TEXT("Return readback contract for downstream export or preview.")}, MetaReceipt},
			{TEXT("metahuman_character_export_dcc"), TEXT("Plan MetaHuman DCC export with receipt gates."), TEXT("export"), TEXT("dcc_export"), true, MetaPlugins, MetaModules, ExportFocus, MetaNeedles, {TEXT("Resolve target MetaHuman character."), TEXT("Select DCC export route."), TEXT("Plan export folder and verification readback."), TEXT("Require export receipt before delivery.")}, MetaReceipt},
			{TEXT("metahuman_character_export_dna"), TEXT("Plan MetaHuman DNA export/import verification."), TEXT("export"), TEXT("dna_export"), true, MetaPlugins, MetaModules, ExportFocus, {TEXT("DNA"), TEXT("MetaHuman")}, {TEXT("Resolve RigLogic DNA asset/user-data."), TEXT("Plan DNA export or import route."), TEXT("Require DNA file/object readback and verification.")}, MetaReceipt},
			{TEXT("metahuman_character_export_geometry"), TEXT("Plan MetaHuman geometry export."), TEXT("export"), TEXT("geometry_export"), true, MetaPlugins, MetaModules, ExportFocus, MetaNeedles, {TEXT("Resolve skeletal mesh and body/head assets."), TEXT("Plan geometry export and LOD mapping."), TEXT("Require file/object readback.")}, MetaReceipt},
			{TEXT("metahuman_character_export_materials"), TEXT("Plan MetaHuman material and texture export."), TEXT("export"), TEXT("material_export"), true, MetaPlugins, MetaModules, ExportFocus, {TEXT("MetaHuman"), TEXT("Material"), TEXT("Texture")}, {TEXT("Resolve character material slots."), TEXT("Plan texture/material export set."), TEXT("Require material slot readback and file/object proof.")}, MetaReceipt},
			{TEXT("metahuman_crowd_spawner_create"), TEXT("Plan MetaHuman crowd spawner creation."), TEXT("crowd"), TEXT("crowd_spawner"), true, CrowdPlugins, CrowdModules, CrowdFocus, {TEXT("MetaHuman"), TEXT("Crowd"), TEXT("Population")}, {TEXT("Resolve crowd variation source assets."), TEXT("Plan Mass/Spawner/DataLayer target."), TEXT("Bind character variation manifest."), TEXT("Require spawn budget and preview receipt.")}, MetaReceipt},
			{TEXT("metahuman_crowd_actor_bind"), TEXT("Plan MetaHuman crowd actor identity and animation binding."), TEXT("crowd"), TEXT("crowd_actor_bind"), true, CrowdPlugins, CrowdModules, CrowdFocus, {TEXT("MetaHuman"), TEXT("Crowd")}, {TEXT("Resolve spawned actor or crowd entity target."), TEXT("Plan identity/body/outfit binding."), TEXT("Require readback and runtime snapshot.")}, MetaReceipt},
			{TEXT("metahuman_livelink_subject_config"), TEXT("Plan MetaHuman LiveLink subject/source configuration."), TEXT("livelink"), TEXT("livelink"), true, LiveLinkPlugins, LiveLinkModules, {TEXT("livelink"), TEXT("runtime_component")}, {TEXT("MetaHuman"), TEXT("LiveLink")}, {TEXT("Resolve LiveLink subject name."), TEXT("Plan source/settings object and role mapping."), TEXT("Require subject snapshot and preview receipt.")}, MetaReceipt},
			{TEXT("metahuman_calibration_diagnostics_run"), TEXT("Plan MetaHuman calibration diagnostics."), TEXT("validation"), TEXT("calibration"), true, MetaPlugins, MetaModules, {TEXT("calibration"), TEXT("diagnostics")}, {TEXT("MetaHuman"), TEXT("Calibration")}, {TEXT("Resolve calibration input assets."), TEXT("Select diagnostics selector/settings."), TEXT("Require diagnostics report and failure route.")}, MetaReceipt},
			{TEXT("metahuman_asset_report"), TEXT("Build a MetaHuman asset report from available assets/classes."), TEXT("report"), TEXT("asset_report"), false, MetaPlugins, MetaModules, MetaFocus, MetaNeedles, {TEXT("Scan character/DNA/groom/wardrobe assets."), TEXT("Return verification class catalog."), TEXT("Route failures to QA before export/deploy.")}, MetaReceipt},
			{TEXT("metahuman_receipt_validate"), TEXT("Validate a MetaHuman production/export/preview receipt."), TEXT("receipt"), TEXT("receipt"), false, MetaPlugins, MetaModules, MetaFocus, MetaNeedles, {}, MetaReceipt},
			{TEXT("metahuman_body_type_inspect"), TEXT("Inspect MetaHuman body-type and compatibility planning inputs."), TEXT("inspect"), TEXT("body_type"), false, MetaPlugins, MetaModules, {TEXT("metahuman_character"), TEXT("wardrobe")}, MetaNeedles, {TEXT("Resolve target character/body type."), TEXT("List body/wardrobe class availability."), TEXT("Return outfit/body compatibility receipt requirements.")}, MetaReceipt},
			{TEXT("metahuman_groom_asset_audit"), TEXT("Audit MetaHuman groom assets and binding dependencies."), TEXT("audit"), TEXT("groom"), false, MetaPlugins, MetaModules, {TEXT("metahuman_character"), TEXT("asset_report")}, {TEXT("Groom"), TEXT("Hair"), TEXT("MetaHuman")}, {TEXT("Scan groom and character assets."), TEXT("Check asset report/verification classes."), TEXT("Require preview and dependency readback for delivery.")}, MetaReceipt},
			{TEXT("metahuman_anim_blueprint_bind_plan"), TEXT("Plan MetaHuman animation Blueprint binding."), TEXT("plan"), TEXT("animation"), true, MetaPlugins, MetaModules, {TEXT("runtime_component"), TEXT("metahuman_character"), TEXT("dna")}, MetaNeedles, {TEXT("Resolve skeleton, DNA, animation BP, and character target."), TEXT("Plan binding and compatibility checks."), TEXT("Require compile, animation preview, and runtime snapshot.")}, MetaReceipt},
			{TEXT("metahuman_preview_capture"), TEXT("Plan MetaHuman preview capture."), TEXT("preview"), TEXT("preview"), true, MetaPlugins, MetaModules, MetaFocus, MetaNeedles, {TEXT("Resolve character/instance and preview scene."), TEXT("Plan camera/light/thumbnail or screenshot capture."), TEXT("Require preview screenshot receipt.")}, MetaReceipt},
			{TEXT("metahuman_crowd_variation_plan"), TEXT("Plan MetaHuman crowd variations."), TEXT("crowd"), TEXT("crowd_variation"), true, CrowdPlugins, CrowdModules, CrowdFocus, {TEXT("MetaHuman"), TEXT("Population"), TEXT("Wardrobe")}, {TEXT("Resolve body/head/outfit/groom pools."), TEXT("Plan deterministic variation seeds."), TEXT("Require manifest and preview receipt.")}, MetaReceipt},
			{TEXT("metahuman_crowd_mass_bridge_plan"), TEXT("Plan MetaHuman Crowd to Mass bridge."), TEXT("crowd"), TEXT("crowd_mass"), true, CrowdPlugins, CrowdModules, CrowdFocus, {TEXT("MetaHuman"), TEXT("Mass"), TEXT("Crowd")}, {TEXT("Resolve Mass entity config and crowd processors."), TEXT("Plan UAF/animation/runtime identity bridge."), TEXT("Require Mass snapshot and preview receipt.")}, MetaReceipt},
			{TEXT("mutable_customizable_object_inspect"), TEXT("Inspect a Mutable CustomizableObject asset."), TEXT("inspect"), TEXT("customizable_object"), false, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {TEXT("Load CustomizableObject target."), TEXT("Return class/property sample and related Mutable catalog."), TEXT("Use result as manifest seed.")}, MutableReceipt},
			{TEXT("mutable_instance_create"), TEXT("Plan Mutable instance asset creation."), TEXT("plan"), TEXT("instance_create"), true, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {TEXT("Resolve CustomizableObject source."), TEXT("Plan instance package path and default parameters."), TEXT("Require instance readback and validation receipt.")}, MutableReceipt},
			{TEXT("mutable_instance_parameter_list"), TEXT("List/plan Mutable instance parameter schema."), TEXT("parameters"), TEXT("parameters"), false, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {TEXT("Resolve CustomizableObject or instance."), TEXT("Return parameter schema contract."), TEXT("Require readback before parameter writes.")}, MutableReceipt},
			{TEXT("mutable_instance_parameter_set"), TEXT("Plan Mutable instance parameter update."), TEXT("plan"), TEXT("parameter_set"), true, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {TEXT("Resolve instance and parameter name/value."), TEXT("Validate allowed type/value."), TEXT("Plan scoped write and readback."), TEXT("Require update/preview receipt.")}, MutableReceipt},
			{TEXT("mutable_instance_generate_mesh"), TEXT("Plan Mutable generated mesh update."), TEXT("plan"), TEXT("generate_mesh"), true, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {TEXT("Resolve instance usage/component."), TEXT("Plan generate/update request."), TEXT("Require generated mesh/material readback and preview.")}, MutableReceipt},
			{TEXT("mutable_instance_update"), TEXT("Plan Mutable instance update."), TEXT("plan"), TEXT("instance_update"), true, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {TEXT("Resolve instance/update scope."), TEXT("Plan update queue and cache invalidation."), TEXT("Require update receipt and preview.")}, MutableReceipt},
			{TEXT("mutable_population_asset_create"), TEXT("Plan Mutable population asset creation."), TEXT("population"), TEXT("population_asset"), true, MutablePlugins, MutableModules, PopulationFocus, MutableNeedles, {TEXT("Resolve CustomizableObject source and folder."), TEXT("Plan population asset/package creation."), TEXT("Require asset readback and validation receipt.")}, MutableReceipt},
			{TEXT("mutable_population_class_create"), TEXT("Plan Mutable population class creation."), TEXT("population"), TEXT("population_class"), true, MutablePlugins, MutableModules, PopulationFocus, MutableNeedles, {TEXT("Resolve population asset and class parameters."), TEXT("Plan class package and constraints."), TEXT("Require class readback and randomization preview.")}, MutableReceipt},
			{TEXT("mutable_population_generate_preview"), TEXT("Plan Mutable population preview generation."), TEXT("preview"), TEXT("population_preview"), true, MutablePlugins, MutableModules, PopulationFocus, MutableNeedles, {TEXT("Resolve population class/seed."), TEXT("Plan sample generation."), TEXT("Require preview screenshot and generated instance manifest.")}, MutableReceipt},
			{TEXT("mutable_validation_run"), TEXT("Plan/run-gate Mutable validation."), TEXT("validation"), TEXT("validation"), true, MutablePlugins, MutableModules, {TEXT("mutable_validation"), TEXT("mutable")}, MutableNeedles, {TEXT("Resolve validation target."), TEXT("Select commandlet or editor-validator route."), TEXT("Require diagnostics and failure route.")}, MutableReceipt},
			{TEXT("mutable_compile_status"), TEXT("Read/plan Mutable compile status."), TEXT("compile"), TEXT("compile"), false, MutablePlugins, MutableModules, {TEXT("mutable"), TEXT("mutable_validation")}, MutableNeedles, {TEXT("Resolve CustomizableObject target."), TEXT("Return compile status contract and validation class catalog."), TEXT("Require compile_ok before delivery.")}, MutableReceipt},
			{TEXT("mutable_receipt_validate"), TEXT("Validate Mutable production receipt."), TEXT("receipt"), TEXT("receipt"), false, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {}, MutableReceipt},
			{TEXT("mutable_material_variant_audit"), TEXT("Audit Mutable material variant dependencies."), TEXT("audit"), TEXT("material_variant"), false, MutablePlugins, MutableModules, MutableFocus, {TEXT("CustomizableObject"), TEXT("Material"), TEXT("Texture")}, {TEXT("Scan target and material/texture dependencies."), TEXT("Return variant audit contract."), TEXT("Require material slot readback before delivery.")}, MutableReceipt},
			{TEXT("mutable_texture_parameter_audit"), TEXT("Audit Mutable texture parameters."), TEXT("audit"), TEXT("texture_parameter"), false, MutablePlugins, MutableModules, MutableFocus, {TEXT("CustomizableObject"), TEXT("Texture")}, {TEXT("Resolve texture parameters and source textures."), TEXT("Plan compression/sRGB/channel checks."), TEXT("Require texture readback and preview.")}, MutableReceipt},
			{TEXT("mutable_lod_variant_plan"), TEXT("Plan Mutable LOD variants."), TEXT("plan"), TEXT("lod_variant"), true, MutablePlugins, MutableModules, MutableFocus, {TEXT("CustomizableObject"), TEXT("LOD")}, {TEXT("Resolve generated skeletal mesh and LOD sources."), TEXT("Plan LOD/material variant mapping."), TEXT("Require mesh LOD readback and preview.")}, MutableReceipt},
			{TEXT("mutable_dependency_graph"), TEXT("Build Mutable dependency graph contract."), TEXT("dependency_graph"), TEXT("dependency_graph"), false, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {TEXT("Resolve CustomizableObject, instance, generated mesh, material, texture, and population assets."), TEXT("Return graph nodes/edges contract."), TEXT("Use graph for resource locks and QA.")}, MutableReceipt},
			{TEXT("mutable_population_randomize_plan"), TEXT("Plan Mutable population randomization."), TEXT("population"), TEXT("population_randomize"), true, MutablePlugins, MutableModules, PopulationFocus, MutableNeedles, {TEXT("Resolve population class and seed."), TEXT("Plan deterministic randomization constraints."), TEXT("Require generated sample manifest and preview receipt.")}, MutableReceipt},
			{TEXT("mutable_preview_receipt"), TEXT("Validate Mutable preview receipt or return preview requirements."), TEXT("receipt"), TEXT("preview_receipt"), false, MutablePlugins, MutableModules, MutableFocus, MutableNeedles, {}, {TEXT("target binding"), TEXT("screenshot/render preview"), TEXT("generated mesh or parameter readback")}}
		};
	}

	static void RegisterSpec(FSololmcpToolRegistry& Registry, const FCharacterCustomizationSpec& Spec)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Spec.Name;
		Def.Description = Spec.Description;
		Def.InputSchema = InputSchema();
		Def.CacheTtlSeconds = Spec.bMutation ? 0 : 30;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return ExecuteTool(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}
}

void RegisterCharacterCustomizationP1Tools(FSololmcpToolRegistry& Registry)
{
	for (const CharacterCustomizationP1::FCharacterCustomizationSpec& Spec : CharacterCustomizationP1::Specs())
	{
		CharacterCustomizationP1::RegisterSpec(Registry, Spec);
	}
}
}
