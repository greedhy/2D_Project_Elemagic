// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpMeshTerrainModeP1Tools.cpp
// ----------------------------------------------------------------------------
// UE 5.8 Mesh Terrain Mode / MeshPartition P1 concrete probes, plans, and
// receipt gates. This file intentionally avoids direct MeshTerrainMode and
// MeshPartition headers so the plugin still compiles on UE 5.7.
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
#include "Math/UnrealMathUtility.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace MeshTerrainModeP1
{
	struct FCatalogRow
	{
		FString Id;
		FString ObjectPath;
		FString Kind;
		FString Category;
	};

	struct FPaletteTool
	{
		FString Submode;
		FString Palette;
		FString Command;
		FString ToolId;
		bool bExperimental = false;
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
		TArray<FString> FunctionNeedles;
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

	static bool BoolField(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field)
	{
		bool bValue = false;
		return Obj->TryGetBoolField(Field, bValue) && bValue;
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
			if (!Value.IsValid())
			{
				continue;
			}
			const FString StringValue = Value->AsString();
			if (!StringValue.IsEmpty())
			{
				Values.Add(StringValue);
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
		Obj->SetNumberField(TEXT("enabled_by_default_value"), static_cast<int32>(Desc.EnabledByDefault));
		Obj->SetBoolField(TEXT("experimental"), Desc.bIsExperimentalVersion);
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
			TEXT("MeshTerrainMode"),
			TEXT("MeshPartition"),
			TEXT("MeshModelingToolset"),
			TEXT("MeshModelingToolsetExp"),
			TEXT("MeshLODToolset"),
			TEXT("ToolPresets"),
			TEXT("ModelingToolsEditorMode"),
			TEXT("GeometryScripting")
		};
		const TArray<FString> Modules{
			TEXT("MeshTerrainMode"),
			TEXT("MeshPartition"),
			TEXT("MeshPartitionEditor"),
			TEXT("MeshPartitionModelingToolset"),
			TEXT("MeshModelingTools"),
			TEXT("MeshModelingToolsExp"),
			TEXT("MeshLODToolset"),
			TEXT("ModelingToolsEditorMode"),
			TEXT("GeometryScriptingEditor")
		};

		TArray<TSharedPtr<FJsonValue>> PluginRows;
		bool bMeshTerrainFound = false;
		bool bMeshTerrainEnabled = false;
		bool bMeshPartitionFound = false;
		bool bMeshPartitionEnabled = false;
		for (const FString& PluginName : Plugins)
		{
			TSharedRef<FJsonObject> Row = PluginProbeJson(PluginName);
			const bool bFound = BoolField(Row, TEXT("found"));
			const bool bEnabled = BoolField(Row, TEXT("enabled"));
			if (PluginName == TEXT("MeshTerrainMode"))
			{
				bMeshTerrainFound = bFound;
				bMeshTerrainEnabled = bEnabled;
			}
			else if (PluginName == TEXT("MeshPartition"))
			{
				bMeshPartitionFound = bFound;
				bMeshPartitionEnabled = bEnabled;
			}
			PluginRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		TArray<TSharedPtr<FJsonValue>> ModuleRows;
		bool bMeshTerrainModuleExists = false;
		bool bMeshPartitionModuleExists = false;
		bool bMeshPartitionEditorModuleExists = false;
		bool bMeshPartitionToolsetModuleExists = false;
		for (const FString& ModuleName : Modules)
		{
			TSharedRef<FJsonObject> Row = ModuleProbeJson(ModuleName);
			const bool bExists = BoolField(Row, TEXT("exists"));
			if (ModuleName == TEXT("MeshTerrainMode"))
			{
				bMeshTerrainModuleExists = bExists;
			}
			else if (ModuleName == TEXT("MeshPartition"))
			{
				bMeshPartitionModuleExists = bExists;
			}
			else if (ModuleName == TEXT("MeshPartitionEditor"))
			{
				bMeshPartitionEditorModuleExists = bExists;
			}
			else if (ModuleName == TEXT("MeshPartitionModelingToolset"))
			{
				bMeshPartitionToolsetModuleExists = bExists;
			}
			ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		if (!IsUE58OrLater())
		{
			OutStatus = TEXT("requires_ue_5_8");
		}
		else if (!bMeshTerrainFound)
		{
			OutStatus = TEXT("mesh_terrain_mode_plugin_missing");
		}
		else if (!bMeshPartitionFound)
		{
			OutStatus = TEXT("mesh_partition_plugin_missing");
		}
		else if (!bMeshTerrainEnabled || !bMeshPartitionEnabled)
		{
			OutStatus = TEXT("plugin_present_not_enabled");
		}
		else if (!bMeshTerrainModuleExists)
		{
			OutStatus = TEXT("mesh_terrain_mode_module_missing");
		}
		else if (!bMeshPartitionModuleExists || !bMeshPartitionEditorModuleExists || !bMeshPartitionToolsetModuleExists)
		{
			OutStatus = TEXT("dependency_module_missing");
		}
		else
		{
			OutStatus = TEXT("available");
		}

		Out->SetStringField(TEXT("status"), OutStatus);
		Out->SetBoolField(TEXT("available"), OutStatus == TEXT("available"));
		Out->SetBoolField(TEXT("mesh_terrain_mode_enabled"), bMeshTerrainEnabled);
		Out->SetBoolField(TEXT("mesh_partition_enabled"), bMeshPartitionEnabled);
		Out->SetArrayField(TEXT("plugins"), PluginRows);
		Out->SetArrayField(TEXT("modules"), ModuleRows);
		return OutStatus == TEXT("available");
	}

	static TArray<FCatalogRow> Catalog()
	{
		return {
			{TEXT("MeshTerrainMode"), TEXT("/Script/MeshTerrainMode.MeshTerrainMode"), TEXT("class"), TEXT("mode")},
			{TEXT("MeshTerrainModeSettings"), TEXT("/Script/MeshTerrainMode.MeshTerrainModeSettings"), TEXT("class"), TEXT("settings")},
			{TEXT("MeshTerrainModeCustomizationSettings"), TEXT("/Script/MeshTerrainMode.MeshTerrainModeCustomizationSettings"), TEXT("class"), TEXT("settings")},
			{TEXT("MeshTerrainModeEditableToolPaletteConfig"), TEXT("/Script/MeshTerrainMode.MeshTerrainModeEditableToolPaletteConfig"), TEXT("class"), TEXT("palette")},
			{TEXT("MeshTerrainModeSelectionInteraction"), TEXT("/Script/MeshTerrainMode.MeshTerrainModeSelectionInteraction"), TEXT("class"), TEXT("selection")},
			{TEXT("MeshPartition"), TEXT("/Script/MeshPartition.MeshPartition"), TEXT("class"), TEXT("partition")},
			{TEXT("MeshPartitionComponent"), TEXT("/Script/MeshPartition.MeshPartitionComponent"), TEXT("class"), TEXT("partition")},
			{TEXT("MeshPartitionDefinition"), TEXT("/Script/MeshPartition.MeshPartitionDefinition"), TEXT("class"), TEXT("definition")},
			{TEXT("MeshPartitionStaticMeshComponent"), TEXT("/Script/MeshPartition.MeshPartitionStaticMeshComponent"), TEXT("class"), TEXT("static_mesh")},
			{TEXT("MeshPartitionCollisionComponent"), TEXT("/Script/MeshPartition.MeshPartitionCollisionComponent"), TEXT("class"), TEXT("collision")},
			{TEXT("MeshPartitionCompiledSection"), TEXT("/Script/MeshPartition.MeshPartitionCompiledSection"), TEXT("class"), TEXT("compiled_section")},
			{TEXT("MeshPartitionSettings"), TEXT("/Script/MeshPartition.Settings"), TEXT("class"), TEXT("settings")},
			{TEXT("MeshPartitionEditorComponent"), TEXT("/Script/MeshPartitionEditor.MeshPartitionEditorComponent"), TEXT("class"), TEXT("editor_component")},
			{TEXT("MeshPartitionEditorSubsystem"), TEXT("/Script/MeshPartitionEditor.MeshPartitionEditorSubsystem"), TEXT("class"), TEXT("editor_subsystem")},
			{TEXT("MeshPartitionEditorWorldSubsystem"), TEXT("/Script/MeshPartitionEditor.MeshPartitionEditorWorldSubsystem"), TEXT("class"), TEXT("editor_subsystem")},
			{TEXT("MeshPartitionModifierActor"), TEXT("/Script/MeshPartitionEditor.ModifierActor"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionModifierComponent"), TEXT("/Script/MeshPartitionEditor.ModifierComponent"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionMeshProviderModifier"), TEXT("/Script/MeshPartitionEditor.MeshProviderModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionTexturePatchModifier"), TEXT("/Script/MeshPartitionEditor.TexturePatchModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionSplineModifier"), TEXT("/Script/MeshPartitionEditor.SplineModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionPatchModifier"), TEXT("/Script/MeshPartitionEditor.PatchModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionRemeshModifier"), TEXT("/Script/MeshPartitionEditor.RemeshModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionSplineRemeshModifier"), TEXT("/Script/MeshPartitionEditor.SplineRemeshModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionBooleanModifier"), TEXT("/Script/MeshPartitionEditor.BooleanModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionNoiseModifier"), TEXT("/Script/MeshPartitionEditor.NoiseModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionProjectSculptLayersModifier"), TEXT("/Script/MeshPartitionEditor.ProjectMeshLayersModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionWeightUtilityModifier"), TEXT("/Script/MeshPartitionEditor.WeightUtilityModifier"), TEXT("class"), TEXT("modifier")},
			{TEXT("MeshPartitionHeightmapImportTool"), TEXT("/Script/MeshPartitionModelingToolset.HeightmapImportTool"), TEXT("class"), TEXT("heightmap")},
			{TEXT("MeshPartitionHeightmapImportPropertySet"), TEXT("/Script/MeshPartitionModelingToolset.HeightmapImportPropertySet"), TEXT("class"), TEXT("heightmap")},
			{TEXT("MeshPartitionHeightSculptTool"), TEXT("/Script/MeshPartitionModelingToolset.HeightSculptTool"), TEXT("class"), TEXT("sculpt")},
			{TEXT("MeshPartitionAttributePaintTool"), TEXT("/Script/MeshPartitionModelingToolset.AttributePaintTool"), TEXT("class"), TEXT("paint")},
			{TEXT("MeshPartitionCreateMeshTool"), TEXT("/Script/MeshPartitionModelingToolset.CreateMeshTool"), TEXT("class"), TEXT("create")},
			{TEXT("MeshPartitionCreateRectangleMeshTool"), TEXT("/Script/MeshPartitionModelingToolset.CreateRectangleMeshTool"), TEXT("class"), TEXT("create")},
			{TEXT("MeshPartitionConvertTool"), TEXT("/Script/MeshPartitionModelingToolset.ConvertTool"), TEXT("class"), TEXT("edit")},
			{TEXT("MeshPartitionSplitTool"), TEXT("/Script/MeshPartitionModelingToolset.SplitTool"), TEXT("class"), TEXT("edit")},
			{TEXT("MeshPartitionStitchTool"), TEXT("/Script/MeshPartitionModelingToolset.StitchTool"), TEXT("class"), TEXT("edit")},
			{TEXT("MeshPartitionExpandTool"), TEXT("/Script/MeshPartitionModelingToolset.ExpandTool"), TEXT("class"), TEXT("edit")},
			{TEXT("MeshPartitionMergeTool"), TEXT("/Script/MeshPartitionModelingToolset.MergeTool"), TEXT("class"), TEXT("edit")},
			{TEXT("MeshPartitionResectionTool"), TEXT("/Script/MeshPartitionModelingToolset.MeshPartitionResectionTool"), TEXT("class"), TEXT("edit")}
		};
	}

	static bool FocusMatches(const FCatalogRow& Row, const TArray<FString>& FocusIds)
	{
		return FocusIds.IsEmpty() || FocusIds.Contains(Row.Id) || FocusIds.Contains(Row.Category);
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
		if (!Class)
		{
			if (!ResolveError.IsEmpty())
			{
				Obj->SetStringField(TEXT("resolveError"), ResolveError);
			}
			return Obj;
		}

		Obj->SetStringField(TEXT("resolvedClass"), Class->GetPathName());
		Obj->SetStringField(TEXT("superClass"), Class->GetSuperClass() ? Class->GetSuperClass()->GetPathName() : FString());
		int32 PropertyCount = 0;
		TArray<TSharedPtr<FJsonValue>> Properties;
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			++PropertyCount;
			if (Properties.Num() < 32)
			{
				TSharedRef<FJsonObject> Prop = MakeShared<FJsonObject>();
				Prop->SetStringField(TEXT("name"), It->GetName());
				Prop->SetStringField(TEXT("cppType"), It->GetCPPType());
				Prop->SetBoolField(TEXT("editable"), It->HasAnyPropertyFlags(CPF_Edit));
				Properties.Add(MakeShared<FJsonValueObject>(Prop));
			}
		}
		int32 FunctionCount = 0;
		for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			++FunctionCount;
		}
		Obj->SetNumberField(TEXT("propertyCount"), PropertyCount);
		Obj->SetNumberField(TEXT("functionCount"), FunctionCount);
		Obj->SetArrayField(TEXT("properties"), Properties);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> CatalogJson(const FSololmcpToolExecutionContext& Context, const TArray<FString>& FocusIds)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FCatalogRow& Row : Catalog())
		{
			if (FocusMatches(Row, FocusIds))
			{
				Rows.Add(MakeShared<FJsonValueObject>(ClassStatusJson(Context, Row)));
			}
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
			return Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Function->HasMetaData(TEXT("AICallable"));
		}
		const FString Name = Function->GetName();
		for (const FString& Needle : Needles)
		{
			if (!Needle.IsEmpty() && Name.Contains(Needle, ESearchCase::IgnoreCase))
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
		const TArray<FString>& Needles)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FCatalogRow& Row : Catalog())
		{
			if (!FocusMatches(Row, FocusIds))
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
				if (!FunctionMatches(Function, Needles))
				{
					continue;
				}
				Rows.Add(MakeShared<FJsonValueObject>(FunctionJson(Class, Function)));
				if (Rows.Num() >= 80)
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
		MaxAssets = FMath::Clamp(MaxAssets <= 0 ? 50 : MaxAssets, 1, 500);

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
		return Obj;
	}

	static TArray<FPaletteTool> Palette()
	{
		return {
			{TEXT("create"), TEXT("Create"), TEXT("BeginCreateMegaMeshRectangleTool"), TEXT("create_rectangle_mesh"), false},
			{TEXT("create"), TEXT("Create"), TEXT("BeginHeightmapImport"), TEXT("heightmap_import"), false},
			{TEXT("create"), TEXT("Create"), TEXT("BeginDrawSplineTool"), TEXT("draw_spline"), false},
			{TEXT("create"), TEXT("Create"), TEXT("BeginPatternTool"), TEXT("pattern"), true},
			{TEXT("edit"), TEXT("Edit"), TEXT("BeginConvertMegaMeshTool"), TEXT("convert_mesh_partition"), false},
			{TEXT("edit"), TEXT("Edit"), TEXT("BeginExpandMegaMeshTool"), TEXT("expand_mesh_partition"), false},
			{TEXT("edit"), TEXT("Edit"), TEXT("BeginSplitMegaMeshTool"), TEXT("split_mesh_partition"), false},
			{TEXT("edit"), TEXT("Edit"), TEXT("BeginStitchMegaMeshTool"), TEXT("stitch_mesh_partition"), false},
			{TEXT("edit"), TEXT("Edit"), TEXT("BeginMergeMegaMeshTool"), TEXT("merge_mesh_partition"), false},
			{TEXT("edit"), TEXT("Edit"), TEXT("BeginResectionMeshTool"), TEXT("resection_mesh_partition"), false},
			{TEXT("edit"), TEXT("General"), TEXT("BeginEditPivotTool"), TEXT("edit_pivot"), false},
			{TEXT("edit"), TEXT("General"), TEXT("BeginBakeTransformTool"), TEXT("bake_transform"), false},
			{TEXT("edit"), TEXT("General"), TEXT("BeginAttributeEditorTool"), TEXT("attribute_editor"), false},
			{TEXT("edit"), TEXT("General"), TEXT("BeginMeshInspectorTool"), TEXT("mesh_inspector"), false},
			{TEXT("edit"), TEXT("General"), TEXT("BeginDuplicateMeshesTool"), TEXT("duplicate_meshes"), true},
			{TEXT("modifiers"), TEXT("Displace"), TEXT("Place MeshProjectModifier"), TEXT("modifier_mesh_project"), false},
			{TEXT("modifiers"), TEXT("Displace"), TEXT("Place TexturePatchModifier"), TEXT("modifier_texture_patch"), false},
			{TEXT("modifiers"), TEXT("Displace"), TEXT("Place SplineModifier"), TEXT("modifier_spline"), false},
			{TEXT("modifiers"), TEXT("Displace"), TEXT("Place ProjectSculptLayersModifier"), TEXT("modifier_sculpt_layers"), false},
			{TEXT("modifiers"), TEXT("Displace"), TEXT("Place LatticeModifier"), TEXT("modifier_lattice"), true},
			{TEXT("modifiers"), TEXT("Displace"), TEXT("Place NoiseModifier"), TEXT("modifier_noise"), false},
			{TEXT("modifiers"), TEXT("Displace"), TEXT("Place PatchModifier"), TEXT("modifier_patch"), true},
			{TEXT("modifiers"), TEXT("Other"), TEXT("Place BooleanModifier"), TEXT("modifier_boolean"), false},
			{TEXT("modifiers"), TEXT("Other"), TEXT("Place RemeshModifier"), TEXT("modifier_remesh"), false},
			{TEXT("modifiers"), TEXT("Other"), TEXT("Place SplineRemeshModifier"), TEXT("modifier_spline_remesh"), false},
			{TEXT("paint"), TEXT("Paint"), TEXT("BeginMeshAttributePaintTool"), TEXT("attribute_paint"), false},
			{TEXT("paint"), TEXT("Paint"), TEXT("BeginMeshVertexPaintTool"), TEXT("vertex_paint"), false},
			{TEXT("paint"), TEXT("Attributes"), TEXT("BeginAttributeEditorTool"), TEXT("attribute_editor"), false},
			{TEXT("sculpt"), TEXT("Sculpt"), TEXT("BeginSculptMeshOffsetBrushTool"), TEXT("sculpt_offset"), false},
			{TEXT("sculpt"), TEXT("Sculpt"), TEXT("BeginSculptMeshMoveBrushTool"), TEXT("sculpt_move"), false},
			{TEXT("sculpt"), TEXT("Sculpt"), TEXT("BeginSculptMeshSmoothBrushTool"), TEXT("sculpt_smooth"), false},
			{TEXT("sculpt"), TEXT("Sculpt"), TEXT("BeginSculptMeshPinchBrushTool"), TEXT("sculpt_pinch"), false},
			{TEXT("sculpt"), TEXT("Sculpt"), TEXT("BeginSculptMeshFlattenBrushTool"), TEXT("sculpt_flatten"), false},
			{TEXT("sculpt"), TEXT("Sculpt"), TEXT("BeginSculptMeshEraseLayerTool"), TEXT("sculpt_erase_layer"), false},
			{TEXT("sculpt"), TEXT("Height Sculpt"), TEXT("BeginHeightSculptBrushTool"), TEXT("height_sculpt"), false},
			{TEXT("sculpt"), TEXT("Height Sculpt"), TEXT("BeginHeightSmoothBrushTool"), TEXT("height_smooth"), false},
			{TEXT("sculpt"), TEXT("Height Sculpt"), TEXT("BeginHeightFlattenBrushTool"), TEXT("height_flatten"), false},
			{TEXT("sculpt"), TEXT("Height Sculpt"), TEXT("BeginZeroHeightBrushTool"), TEXT("height_zero"), false},
			{TEXT("sculpt"), TEXT("Height Sculpt"), TEXT("BeginSlopeErodeBrushTool"), TEXT("height_slope_erode"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddBoxPrimitiveTool"), TEXT("shape_box"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddSpherePrimitiveTool"), TEXT("shape_sphere"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddCylinderPrimitiveTool"), TEXT("shape_cylinder"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddCapsulePrimitiveTool"), TEXT("shape_capsule"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddConePrimitiveTool"), TEXT("shape_cone"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddTorusPrimitiveTool"), TEXT("shape_torus"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddRectanglePrimitiveTool"), TEXT("shape_rectangle"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddDiscPrimitiveTool"), TEXT("shape_disc"), false},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddArrowPrimitiveTool"), TEXT("shape_arrow"), true},
			{TEXT("shapes"), TEXT("Add Modifiers"), TEXT("BeginAddStairsPrimitiveTool"), TEXT("shape_stairs"), true}
		};
	}

	static TArray<TSharedPtr<FJsonValue>> PaletteJson(const TArray<FString>& FilterSubmodes)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FPaletteTool& Tool : Palette())
		{
			if (!FilterSubmodes.IsEmpty() && !FilterSubmodes.Contains(Tool.Submode))
			{
				continue;
			}
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("submode"), Tool.Submode);
			Obj->SetStringField(TEXT("palette"), Tool.Palette);
			Obj->SetStringField(TEXT("command"), Tool.Command);
			Obj->SetStringField(TEXT("tool_id"), Tool.ToolId);
			Obj->SetBoolField(TEXT("experimental"), Tool.bExperimental);
			Rows.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Rows;
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
		const bool bTarget = ReceiptHasAny(Receipt, {
			TEXT("target_asset"), TEXT("mesh_partition_asset"), TEXT("mesh_partition_actor"), TEXT("target_binding"), TEXT("target_level")
		});
		AddCheck(Checks, bValid, TEXT("target_binding"), bTarget, bTarget ? TEXT("Target binding found.") : TEXT("Missing MeshTerrain target binding."));

		const bool bGate = ReceiptBool(Receipt, TEXT("version_satisfied")) || ReceiptHasAny(Receipt, {TEXT("availability_status"), TEXT("plugin_gate"), TEXT("class_catalog")});
		AddCheck(Checks, bValid, TEXT("version_or_plugin_gate"), bGate, bGate ? TEXT("Version/plugin gate evidence found.") : TEXT("Missing version/plugin gate evidence."));

		const bool bReadback = ReceiptBool(Receipt, TEXT("readback_ok")) || ReceiptBool(Receipt, TEXT("validation_ok")) || ReceiptHasAny(Receipt, {
			TEXT("mesh_partition_readback"), TEXT("class_catalog"), TEXT("asset_scan"), TEXT("component_snapshot"), TEXT("modifier_snapshot")
		});
		AddCheck(Checks, bValid, TEXT("readback_or_validation"), bReadback, bReadback ? TEXT("Readback/validation evidence found.") : TEXT("Missing MeshTerrain readback/validation evidence."));

		const bool bPreview = ReceiptBool(Receipt, TEXT("preview_ok")) || ReceiptHasAny(Receipt, {TEXT("screenshot"), TEXT("preview_image"), TEXT("preview_receipt"), TEXT("capture_path")});
		if (Spec.Name == TEXT("mesh_terrain_preview_capture") || Spec.Name == TEXT("mesh_terrain_receipt_validate"))
		{
			AddCheck(Checks, bValid, TEXT("preview_evidence"), bPreview, bPreview ? TEXT("Preview evidence found.") : TEXT("Missing preview evidence."));
		}

		Out->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetArrayField(TEXT("checks"), Checks);
		if (!bValid)
		{
			Error = FString::Printf(TEXT("%s failed MeshTerrain receipt validation."), *Spec.Name);
			Summary = Error;
			return false;
		}
		Summary = FString::Printf(TEXT("%s passed."), *Spec.Name);
		return true;
	}

	// MT plan→route (2026-08-05): every *plan tool now attaches a routing alias
	// to a real tool with live evidence (the same fallback family the file
	// already publishes), with pass-through argument mapping and preconditions.
	// The plan remains dry-run (execute=true is still fail-closed pending a
	// dedicated MeshTerrain writer), but it is no longer a bare plan echo.
	static TSharedRef<FJsonObject> RoutingPlanJson(const FSpec& Spec, const TSharedRef<FJsonObject>& Arguments)
	{
		struct FRoute
		{
			FString Primary;
			TArray<FString> Fallbacks;
			TArray<TPair<FString, FString>> ArgumentMap; // target_field <- source_field
			FString Precondition;
		};
		FRoute Route;
		if (Spec.Name == TEXT("mesh_terrain_create_asset_plan"))
		{
			Route = {TEXT("landscape_create"), {TEXT("terrain_geomorphology_plan"), TEXT("pcg_graph_validate")},
				{{TEXT("target_level"), TEXT("target_level")}, {TEXT("folder_path"), TEXT("folder_path")}, {TEXT("mesh_partition_asset"), TEXT("target_asset")}},
				TEXT("MeshTerrainMode gate available; otherwise route through Landscape creation.")};
		}
		else if (Spec.Name == TEXT("mesh_terrain_apply_heightfield_to_mesh"))
		{
			Route = {TEXT("landscape_import_heightmap"), {TEXT("landscape_texture_patch_create_v2"), TEXT("terrain_geomorphology_plan")},
				{{TEXT("heightmap_path"), TEXT("heightmap_path")}, {TEXT("target_level"), TEXT("target_level")}, {TEXT("heightmap_asset"), TEXT("target_asset")}},
				TEXT("Heightmap source must be a texture or raw file readable by the import route.")};
		}
		else if (Spec.Name == TEXT("mesh_terrain_bake_attribute_maps"))
		{
			Route = {TEXT("terrain_geomorphology_plan"), {TEXT("landscape_layer_paint"), TEXT("pcg_graph_validate")},
				{{TEXT("target_asset"), TEXT("target_asset")}, {TEXT("material_asset"), TEXT("material_asset")}},
				TEXT("Bake channels (height/normal/slope/weight) are read back through the geomorphology route; texture outputs require a material writer.")};
		}
		else if (Spec.Name == TEXT("mesh_terrain_convert_to_landscape_plan"))
		{
			Route = {TEXT("landscape_create"), {TEXT("landscape_import_heightmap"), TEXT("landscape_layer_paint"), TEXT("landscape_texture_patch_create_v2")},
				{{TEXT("target_level"), TEXT("target_level")}, {TEXT("heightmap_path"), TEXT("heightmap_path")}},
				TEXT("Snapshot mesh terrain bounds/height channel before conversion; Landscape fallback is the supported path.")};
		}
		else if (Spec.Name == TEXT("mesh_terrain_material_layer_plan"))
		{
			Route = {TEXT("landscape_layer_paint"), {TEXT("pcg_graph_validate"), TEXT("terrain_geomorphology_plan")},
				{{TEXT("target_asset"), TEXT("target_asset")}, {TEXT("material_asset"), TEXT("material_asset")}},
				TEXT("Material/weight-layer evidence is read back from the target asset; compile receipts gate delivery.")};
		}
		else if (Spec.Name == TEXT("mesh_terrain_collision_plan"))
		{
			Route = {TEXT("landscape_create"), {TEXT("staticmesh_generate_lods"), TEXT("terrain_geomorphology_plan")},
				{{TEXT("target_level"), TEXT("target_level")}, {TEXT("target_asset"), TEXT("target_asset")}},
				TEXT("Collision is produced by the partition/landscape builder; navmesh rebuild requires the navigation route.")};
		}
		else if (Spec.Name == TEXT("mesh_terrain_lod_plan"))
		{
			Route = {TEXT("staticmesh_generate_lods"), {TEXT("pcg_graph_validate"), TEXT("terrain_geomorphology_plan")},
				{{TEXT("target_asset"), TEXT("target_asset")}},
				TEXT("LOD budgets apply to generated static meshes; Nanite descriptor audit remains read-only.")};
		}

		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("route_kind"), Route.Primary.IsEmpty() ? TEXT("no_route") : TEXT("real_tool_alias"));
		Plan->SetStringField(TEXT("primary_tool"), Route.Primary);
		Plan->SetArrayField(TEXT("fallback_tools"), StringArrayJson(Route.Fallbacks));
		TArray<TSharedPtr<FJsonValue>> ArgRows;
		for (const TPair<FString, FString>& Pair : Route.ArgumentMap)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("target_field"), Pair.Key);
			Row->SetStringField(TEXT("source_field"), Pair.Value);
			const bool bPresent = Arguments->HasField(Pair.Value);
			Row->SetBoolField(TEXT("present"), bPresent);
			if (bPresent)
			{
				FString Value;
				if (Arguments->TryGetStringField(Pair.Value, Value))
				{
					Row->SetStringField(TEXT("value"), Value);
				}
			}
			ArgRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Plan->SetArrayField(TEXT("argument_map"), ArgRows);
		Plan->SetStringField(TEXT("precondition"), Route.Precondition);
		return Plan;
	}

	static void AddModeSpecificFields(const FSpec& Spec, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out)
	{
		if (Spec.Name == TEXT("mesh_terrain_mode_probe"))
		{
			Out->SetArrayField(TEXT("known_submodes"), StringArrayJson({TEXT("create"), TEXT("edit"), TEXT("modifiers"), TEXT("paint"), TEXT("sculpt"), TEXT("shapes")}));
			Out->SetArrayField(TEXT("core_assets"), StringArrayJson({TEXT("MeshPartitionDefinition"), TEXT("MeshPartition actor"), TEXT("MeshPartitionEditorComponent"), TEXT("ModifierActor"), TEXT("CompiledSection")}));
		}
		if (Spec.Name == TEXT("mesh_terrain_palette_list") || Spec.Name == TEXT("mesh_terrain_sculpt_brush_catalog"))
		{
			TArray<FString> Filter;
			if (Spec.Name == TEXT("mesh_terrain_sculpt_brush_catalog"))
			{
				Filter.Add(TEXT("sculpt"));
			}
			Out->SetArrayField(TEXT("palette"), PaletteJson(Filter));
		}
		if (Spec.Name == TEXT("mesh_terrain_create_asset_plan"))
		{
			Out->SetArrayField(TEXT("creation_routes"), StringArrayJson({
				TEXT("Create rectangle mesh partition"),
				TEXT("Import heightmap through MeshPartition heightmap importer"),
				TEXT("Create spline-driven mesh terrain"),
				TEXT("Use Modeling Tools shapes as source modifiers")
			}));
		}
		if (Spec.Name == TEXT("mesh_terrain_apply_heightfield_to_mesh"))
		{
			Out->SetArrayField(TEXT("heightfield_contract"), StringArrayJson({
				TEXT("heightmap texture or raw file"),
				TEXT("world bounds / scale"),
				TEXT("mesh partition definition"),
				TEXT("resolution budget"),
				TEXT("material and collision readback")
			}));
		}
		if (Spec.Name == TEXT("mesh_terrain_bake_attribute_maps"))
		{
			Out->SetArrayField(TEXT("attribute_map_outputs"), StringArrayJson({TEXT("height"), TEXT("normal"), TEXT("slope"), TEXT("curvature"), TEXT("weight channels"), TEXT("material masks")}));
		}
		if (Spec.Name == TEXT("mesh_terrain_convert_to_landscape_plan"))
		{
			Out->SetArrayField(TEXT("conversion_fallbacks"), StringArrayJson({TEXT("landscape_import_heightmap"), TEXT("landscape_paint_layer"), TEXT("landscape_texture_patch_create_v2"), TEXT("pcg_mesh_partition_adapter_attach")}));
		}
		if (Spec.Name == TEXT("mesh_terrain_material_layer_plan"))
		{
			Out->SetArrayField(TEXT("material_channels"), StringArrayJson({TEXT("base color"), TEXT("normal"), TEXT("roughness"), TEXT("height/displacement"), TEXT("weight layers"), TEXT("runtime virtual texture")}));
		}
		if (Spec.Name == TEXT("mesh_terrain_collision_plan"))
		{
			Out->SetArrayField(TEXT("collision_routes"), StringArrayJson({TEXT("MeshPartitionCollisionComponent"), TEXT("compiled section collision"), TEXT("static mesh simple/complex collision"), TEXT("navmesh rebuild receipt")}));
		}
		if (Spec.Name == TEXT("mesh_terrain_lod_plan"))
		{
			Out->SetArrayField(TEXT("lod_routes"), StringArrayJson({TEXT("MeshLODToolset simplify"), TEXT("static mesh LOD generation"), TEXT("partition section budget"), TEXT("Nanite/static mesh descriptor audit")}));
		}
		if (Spec.Mode == TEXT("plan"))
		{
			Out->SetObjectField(TEXT("routing_plan"), RoutingPlanJson(Spec, Arguments));
		}
	}

	static bool ExecuteTool(const FSpec& Spec, const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		FString TargetAsset;
		for (const TCHAR* Field : {TEXT("target_asset"), TEXT("mesh_partition_asset"), TEXT("heightmap_asset"), TEXT("material_asset"), TEXT("asset_path"), TEXT("target")})
		{
			if (Arguments->TryGetStringField(Field, TargetAsset) && !TargetAsset.IsEmpty())
			{
				break;
			}
		}
		FString TargetLevel;
		Arguments->TryGetStringField(TEXT("target_level"), TargetLevel);
		FString FolderPath;
		Arguments->TryGetStringField(TEXT("folder_path"), FolderPath);
		FString HeightmapPath;
		Arguments->TryGetStringField(TEXT("heightmap_path"), HeightmapPath);
		FString MaterialPath;
		Arguments->TryGetStringField(TEXT("material_asset"), MaterialPath);
		FString BrushId;
		Arguments->TryGetStringField(TEXT("brush_id"), BrushId);
		FString Submode;
		Arguments->TryGetStringField(TEXT("submode"), Submode);
		int32 MaxAssets = 50;
		double MaxAssetsRaw = 50.0;
		if (Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssetsRaw))
		{
			MaxAssets = static_cast<int32>(MaxAssetsRaw);
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);

		TArray<FString> Needles = Spec.AssetNeedles;
		Needles.Append(GetStringArrayField(Arguments, TEXT("asset_needles")));

		Out->SetBoolField(TEXT("success"), true);
		Out->SetStringField(TEXT("tool_name"), Spec.Name);
		Out->SetStringField(TEXT("domain"), TEXT("mesh_terrain_mode"));
		Out->SetStringField(TEXT("family"), TEXT("mesh_terrain_mode"));
		Out->SetStringField(TEXT("subdomain"), Spec.Subdomain);
		Out->SetStringField(TEXT("mode"), Spec.Mode);
		Out->SetStringField(TEXT("operation_class"), Spec.bMutation ? TEXT("asset_or_editor_write_plan") : TEXT("read_or_validate"));
		Out->SetStringField(TEXT("safety_class"), Spec.bMutation ? TEXT("receipt_gated") : TEXT("read_only"));
		Out->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		Out->SetStringField(TEXT("minimum_engine_version"), TEXT("5.8.0"));
		Out->SetBoolField(TEXT("version_satisfied"), IsUE58OrLater());
		Out->SetBoolField(TEXT("execute_requested"), bExecute);
		Out->SetStringField(TEXT("target_asset"), TargetAsset);
		Out->SetStringField(TEXT("target_level"), TargetLevel);
		Out->SetStringField(TEXT("folder_path"), FolderPath.IsEmpty() ? TEXT("/Game") : FolderPath);
		Out->SetStringField(TEXT("heightmap_path"), HeightmapPath);
		Out->SetStringField(TEXT("material_asset"), MaterialPath);
		Out->SetStringField(TEXT("brush_id"), BrushId);
		Out->SetStringField(TEXT("submode_filter"), Submode);

		FString Availability;
		ProbeAvailability(Out, Availability);
		Out->SetStringField(TEXT("availability_status"), Availability);
		Out->SetObjectField(TEXT("target_asset_summary"), AssetSummaryJson(Context, TargetAsset));
		Out->SetObjectField(TEXT("heightmap_asset_summary"), AssetSummaryJson(Context, HeightmapPath));
		Out->SetObjectField(TEXT("material_asset_summary"), AssetSummaryJson(Context, MaterialPath));
		Out->SetArrayField(TEXT("class_catalog"), CatalogJson(Context, Spec.FocusIds));
		Out->SetArrayField(TEXT("function_catalog"), FunctionCatalogJson(Context, Spec.FocusIds, Spec.FunctionNeedles));
		Out->SetArrayField(TEXT("asset_scan"), AssetScanJson(Needles, FolderPath, MaxAssets));
		Out->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		Out->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		Out->SetArrayField(TEXT("ue57_fallback_tools"), StringArrayJson({
			TEXT("landscape_create"),
			TEXT("landscape_import_heightmap"),
			TEXT("landscape_layer_paint"),
			TEXT("landscape_texture_patch_create_v2"),
			TEXT("staticmesh_generate_lods"),
			TEXT("pcg_graph_validate"),
			TEXT("terrain_geomorphology_plan")
		}));
		AddModeSpecificFields(Spec, Arguments, Out);

		if (!IsUE58OrLater())
		{
			Out->SetStringField(TEXT("status"), TEXT("requires_ue_5_8"));
			Summary = FString::Printf(TEXT("%s requires UE 5.8; current engine is %s."), *Spec.Name, *CurrentEngineVersionString());
			return true;
		}

		if (Spec.Mode == TEXT("receipt"))
		{
			return ExecuteReceiptTool(Spec, Arguments, Out, Summary, Error);
		}

		if (bExecute && Availability != TEXT("available"))
		{
			Out->SetBoolField(TEXT("success"), false);
			Out->SetStringField(TEXT("status"), Availability);
			Out->SetStringField(TEXT("failure_route"), TEXT("enable_ue58_mesh_terrain_mode_then_retry"));
			Error = FString::Printf(TEXT("%s cannot execute because MeshTerrainMode gate is %s."), *Spec.Name, *Availability);
			Summary = Error;
			return false;
		}
		if (bExecute && Spec.bMutation)
		{
			Out->SetBoolField(TEXT("success"), false);
			Out->SetStringField(TEXT("status"), TEXT("blocked_pending_mesh_terrain_writer"));
			Out->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_mesh_terrain_writer_after_live_fixture_and_receipt"));
			Error = FString::Printf(TEXT("%s blocked_pending_mesh_terrain_writer: concrete MeshTerrain plan/gate tool; execute=true is blocked until a dedicated writer has live fixture proof."), *Spec.Name);
			Summary = Error;
			return false;
		}

		const bool bCompletedRead = Spec.Mode == TEXT("probe") || Spec.Mode == TEXT("palette") || Spec.Mode == TEXT("brush_catalog") || Spec.Mode == TEXT("audit");
		// MT plan→route (2026-08-05): plan tools are no longer bare dry-run echoes;
		// they carry real class/function/asset readback plus a routing alias to a
		// live-evidence fallback tool. execute=true remains fail-closed by design.
		const bool bRealizedPlan = Spec.Mode == TEXT("plan");
		Out->SetStringField(TEXT("status"), bCompletedRead ? TEXT("completed") : (bRealizedPlan ? TEXT("realized_route") : TEXT("dry_run")));
		Summary = FString::Printf(TEXT("%s returned MeshTerrainMode %s evidence."), *Spec.Name, *Spec.Mode);
		return true;
	}

	static TSharedRef<FJsonObject> InputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Target MeshPartition, StaticMesh, material, or related asset path."))},
			{TEXT("mesh_partition_asset"), FSololmcpSchemaBuilder::String(TEXT("Target MeshPartition asset or actor path."))},
			{TEXT("heightmap_asset"), FSololmcpSchemaBuilder::String(TEXT("Heightmap texture/asset path."))},
			{TEXT("heightmap_path"), FSololmcpSchemaBuilder::String(TEXT("Heightmap file or asset path for heightfield planning."))},
			{TEXT("material_asset"), FSololmcpSchemaBuilder::String(TEXT("Material asset path for terrain layer planning."))},
			{TEXT("target_level"), FSololmcpSchemaBuilder::String(TEXT("Target map/level path."))},
			{TEXT("folder_path"), FSololmcpSchemaBuilder::String(TEXT("Content folder to scan, default /Game."))},
			{TEXT("submode"), FSololmcpSchemaBuilder::String(TEXT("Optional submode filter: create/edit/modifiers/paint/sculpt/shapes."))},
			{TEXT("brush_id"), FSololmcpSchemaBuilder::String(TEXT("Brush/tool id for sculpt or modifier planning."))},
			{TEXT("resolution"), FSololmcpSchemaBuilder::Integer(TEXT("Requested mesh/heightmap resolution."))},
			{TEXT("world_size_cm"), FSololmcpSchemaBuilder::Integer(TEXT("Requested world size in centimeters."))},
			{TEXT("asset_needles"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Additional asset scan needles."))},
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Receipt evidence for validation tools."))},
			{TEXT("max_assets"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum assets to inspect."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Request mutation. P1 MeshTerrain tools fail closed until dedicated writers have live fixture proof."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return probe/plan only. Default true."))}
		});
	}

	static TArray<FSpec> Specs()
	{
		const TArray<FString> ModeFocus{TEXT("mode"), TEXT("settings"), TEXT("palette"), TEXT("selection")};
		const TArray<FString> PartitionFocus{TEXT("partition"), TEXT("definition"), TEXT("editor_component"), TEXT("editor_subsystem"), TEXT("compiled_section")};
		const TArray<FString> CreateFocus{TEXT("create"), TEXT("heightmap"), TEXT("partition"), TEXT("definition")};
		const TArray<FString> SculptFocus{TEXT("sculpt"), TEXT("paint"), TEXT("modifier"), TEXT("editor_component")};
		const TArray<FString> ModifierFocus{TEXT("modifier"), TEXT("heightmap"), TEXT("collision"), TEXT("static_mesh"), TEXT("compiled_section")};
		const TArray<FString> Needles{TEXT("MeshTerrain"), TEXT("MeshPartition"), TEXT("Heightmap"), TEXT("Terrain"), TEXT("Landscape"), TEXT("Material"), TEXT("StaticMesh")};
		const TArray<FString> Receipt{TEXT("target MeshTerrain/MeshPartition binding"), TEXT("UE 5.8 plugin/module gate"), TEXT("class/schema/readback snapshot"), TEXT("preview or validation receipt")};
		return {
			{TEXT("mesh_terrain_mode_probe"), TEXT("Probe UE 5.8 Mesh Terrain Mode and MeshPartition gates."), TEXT("probe"), TEXT("gate"), false, ModeFocus, Needles, {TEXT("Probe MeshTerrainMode, MeshPartition, and Modeling Tools plugins/modules."), TEXT("Reflect mode/settings/partition classes."), TEXT("Return fallback route for UE 5.7.")}, Receipt, {TEXT("Get"), TEXT("Set"), TEXT("Can")}},
			{TEXT("mesh_terrain_palette_list"), TEXT("List Mesh Terrain Mode submodes and tool palette commands from UE 5.8 source contracts."), TEXT("palette"), TEXT("palette"), false, ModeFocus, Needles, {TEXT("Return create/edit/modifiers/paint/sculpt/shapes palette entries."), TEXT("Mark experimental tools separately for agent routing.")}, Receipt, {TEXT("Tool"), TEXT("Palette"), TEXT("Submode")}},
			{TEXT("mesh_terrain_create_asset_plan"), TEXT("Plan MeshPartition/MeshTerrain asset creation."), TEXT("plan"), TEXT("create"), true, CreateFocus, Needles, {TEXT("Resolve target level, package path, and mesh partition definition."), TEXT("Choose rectangle, heightmap import, or spline/source-mesh route."), TEXT("Require post-create readback and preview receipt.")}, Receipt, {TEXT("Create"), TEXT("Generate"), TEXT("Import")}},
			{TEXT("mesh_terrain_sculpt_brush_catalog"), TEXT("Return Mesh Terrain sculpt and height sculpt brush catalog."), TEXT("brush_catalog"), TEXT("sculpt"), false, SculptFocus, Needles, {TEXT("Return sculpt and height-sculpt tool ids."), TEXT("Map brush ids to MeshPartition/Modeling Tools classes where available.")}, Receipt, {TEXT("Sculpt"), TEXT("Brush"), TEXT("Height")}},
			{TEXT("mesh_terrain_apply_heightfield_to_mesh"), TEXT("Plan heightfield-to-mesh terrain application."), TEXT("plan"), TEXT("heightfield"), true, CreateFocus, Needles, {TEXT("Resolve heightmap source."), TEXT("Plan MeshPartition heightmap import or texture patch route."), TEXT("Require bounds/resolution/material/collision readback.")}, Receipt, {TEXT("Heightmap"), TEXT("Import"), TEXT("Texture")}},
			{TEXT("mesh_terrain_bake_attribute_maps"), TEXT("Plan MeshTerrain attribute-map baking."), TEXT("plan"), TEXT("attribute_bake"), true, ModifierFocus, Needles, {TEXT("Resolve mesh terrain target."), TEXT("Plan bake outputs for height/normal/slope/weight channels."), TEXT("Require texture asset readback.")}, Receipt, {TEXT("Bake"), TEXT("Attribute"), TEXT("Channel")}},
			{TEXT("mesh_terrain_convert_to_landscape_plan"), TEXT("Plan MeshTerrain-to-Landscape conversion/fallback."), TEXT("plan"), TEXT("landscape_conversion"), true, ModifierFocus, Needles, {TEXT("Snapshot mesh terrain bounds and height channel."), TEXT("Plan Landscape import/patch fallback."), TEXT("Require Landscape layer/material/readback receipt.")}, Receipt, {TEXT("Convert"), TEXT("Landscape"), TEXT("Height")}},
			{TEXT("mesh_terrain_preview_capture"), TEXT("Validate/plan MeshTerrain preview capture."), TEXT("receipt"), TEXT("preview"), false, PartitionFocus, Needles, {}, {TEXT("target binding"), TEXT("preview screenshot"), TEXT("MeshPartition readback")}, {TEXT("Preview"), TEXT("Capture")}},
			{TEXT("mesh_terrain_material_layer_plan"), TEXT("Plan MeshTerrain material and layer setup."), TEXT("plan"), TEXT("materials"), true, ModifierFocus, {TEXT("MeshPartition"), TEXT("Material"), TEXT("Layer"), TEXT("RVT")}, {TEXT("Resolve material asset and channel contract."), TEXT("Plan MeshPartition material expressions and weight layers."), TEXT("Require material compile/readback.")}, Receipt, {TEXT("Material"), TEXT("Layer"), TEXT("Channel")}},
			{TEXT("mesh_terrain_collision_plan"), TEXT("Plan MeshTerrain collision/nav setup."), TEXT("plan"), TEXT("collision"), true, ModifierFocus, Needles, {TEXT("Resolve compiled sections and collision components."), TEXT("Plan simple/complex collision and navmesh rebuild."), TEXT("Require collision/nav readback receipt.")}, Receipt, {TEXT("Collision"), TEXT("Body"), TEXT("Physics")}},
			{TEXT("mesh_terrain_lod_plan"), TEXT("Plan MeshTerrain LOD/Nanite/static mesh section budget."), TEXT("plan"), TEXT("lod"), true, ModifierFocus, Needles, {TEXT("Resolve section descriptors."), TEXT("Plan LOD simplification and section budgets."), TEXT("Require LOD/statistics readback.")}, Receipt, {TEXT("LOD"), TEXT("Simplify"), TEXT("Nanite")}},
			{TEXT("mesh_terrain_receipt_validate"), TEXT("Validate MeshTerrain production receipt."), TEXT("receipt"), TEXT("receipt"), false, PartitionFocus, Needles, {}, Receipt, {TEXT("Validate"), TEXT("Status")}}
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

void RegisterMeshTerrainModeP1Tools(FSololmcpToolRegistry& Registry)
{
	for (const MeshTerrainModeP1::FSpec& Spec : MeshTerrainModeP1::Specs())
	{
		MeshTerrainModeP1::RegisterOne(Registry, Spec);
	}
}
}
