// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpLandscapePatchPcgInteropTools.cpp
// ----------------------------------------------------------------------------
// SOMOLMCP-native Landscape Patch and PCG interop coverage.
//
// This file deliberately does not link against MCPClientToolset or optional
// UE 5.8-only plugin headers. Landscape Patch is invoked through reflection so
// the same binary remains compatible with UE 5.7, while PCG Mesh Partition is
// exposed through guarded UE 5.8 probe/plan/apply tools.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Landscape.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "LandscapeEditLayer.h"
#else
// LandscapeEditLayer.h is 5.5+.
#endif
#include "Materials/MaterialInterface.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "ScopedTransaction.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"


// FJsonObject::TryGetObjectField took const FString& until 5.4 introduced the
// FStringView overload. One accessor keeps both call sites version-neutral.
namespace { template<typename ObjT>
bool SomolTryGetObjectField(const ObjT& Obj, const FString& Field, const TSharedPtr<FJsonObject>*& Out)
{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	return Obj->TryGetObjectField(FStringView(*Field, Field.Len()), Out);
#else
	return Obj->TryGetObjectField(Field, Out);
#endif
}}

namespace UE::SOMOLMCP
{
namespace LandscapePatchPcgInteropTools
{
	struct FLandscapePatchPcgInteropSpec
	{
		FString ToolName;
		FString Description;
		FString Domain;
		FString Mode;
		TArray<FString> Plugins;
		TArray<FString> Modules;
		TArray<FString> SourceDirs;
		TArray<FString> Markers;
		TArray<FString> NodeClassPaths;
		TArray<FString> PlanSteps;
		TArray<FString> ReceiptRequirements;
		TArray<FString> ExistingCoverageTools;
		TArray<FString> FallbackTools;
		int32 MinMajor = 5;
		int32 MinMinor = 7;
		bool bRequireAllPlugins = true;
		bool bScanSources = true;
	};

	static FString CurrentEngineVersionString()
	{
		return FEngineVersion::Current().ToString();
	}

	static bool IsEngineAtLeast(const int32 Major, const int32 Minor)
	{
		const FEngineVersion Current = FEngineVersion::Current();
		if (Current.GetMajor() != Major)
		{
			return Current.GetMajor() > Major;
		}
		return Current.GetMinor() >= Minor;
	}

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
		Obj->SetStringField(TEXT("descriptor_file"), Plugin->GetDescriptorFileName());
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

	static void ScanFileForMarkerCount(const FString& FilePath, const FString& Marker, int32& InOutCount)
	{
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *FilePath))
		{
			return;
		}

		int32 SearchFrom = 0;
		while (true)
		{
			const int32 FoundAt = Contents.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (FoundAt == INDEX_NONE)
			{
				break;
			}
			++InOutCount;
			SearchFrom = FoundAt + Marker.Len();
		}
	}

	static TSharedRef<FJsonObject> SourceInventoryJson(const FLandscapePatchPcgInteropSpec& Spec)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> DirsJson;
		TMap<FString, int32> MarkerCounts;
		for (const FString& Marker : Spec.Markers)
		{
			MarkerCounts.Add(Marker, 0);
		}

		int32 HeaderFileCount = 0;
		int32 CppFileCount = 0;
		int32 UpluginFileCount = 0;
		TArray<TSharedPtr<FJsonValue>> SampleFiles;

		for (const FString& RelativeDir : Spec.SourceDirs)
		{
			const FString AbsoluteDir = FPaths::Combine(FPaths::EnginePluginsDir(), RelativeDir);
			TSharedRef<FJsonObject> DirObj = MakeShared<FJsonObject>();
			DirObj->SetStringField(TEXT("relative_dir"), RelativeDir);
			DirObj->SetStringField(TEXT("absolute_dir"), AbsoluteDir);
			DirObj->SetBoolField(TEXT("exists"), IFileManager::Get().DirectoryExists(*AbsoluteDir));
			DirsJson.Add(MakeShared<FJsonValueObject>(DirObj));

			if (!IFileManager::Get().DirectoryExists(*AbsoluteDir))
			{
				continue;
			}

			TArray<FString> HeaderFiles;
			IFileManager::Get().FindFilesRecursive(HeaderFiles, *AbsoluteDir, TEXT("*.h"), true, false);
			HeaderFileCount += HeaderFiles.Num();
			TArray<FString> SourceFiles;
			IFileManager::Get().FindFilesRecursive(SourceFiles, *AbsoluteDir, TEXT("*.cpp"), true, false);
			CppFileCount += SourceFiles.Num();
			TArray<FString> PluginFiles;
			IFileManager::Get().FindFilesRecursive(PluginFiles, *AbsoluteDir, TEXT("*.uplugin"), true, false);
			UpluginFileCount += PluginFiles.Num();

			TArray<FString> ScanFiles;
			IFileManager::Get().FindFilesRecursive(ScanFiles, *AbsoluteDir, TEXT("*.h"), true, false);
			TArray<FString> CppFiles;
			IFileManager::Get().FindFilesRecursive(CppFiles, *AbsoluteDir, TEXT("*.cpp"), true, false);
			ScanFiles.Append(CppFiles);
			ScanFiles.Sort();

			for (const FString& FilePath : ScanFiles)
			{
				if (SampleFiles.Num() < 24)
				{
					SampleFiles.Add(MakeShared<FJsonValueString>(FilePath));
				}
				for (const FString& Marker : Spec.Markers)
				{
					int32& Count = MarkerCounts.FindOrAdd(Marker);
					ScanFileForMarkerCount(FilePath, Marker, Count);
				}
			}
		}

		TSharedRef<FJsonObject> MarkersObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : MarkerCounts)
		{
			MarkersObj->SetNumberField(Pair.Key, Pair.Value);
		}

		Root->SetArrayField(TEXT("source_dirs"), DirsJson);
		Root->SetNumberField(TEXT("header_file_count"), HeaderFileCount);
		Root->SetNumberField(TEXT("cpp_file_count"), CppFileCount);
		Root->SetNumberField(TEXT("uplugin_file_count"), UpluginFileCount);
		Root->SetObjectField(TEXT("marker_counts"), MarkersObj);
		Root->SetArrayField(TEXT("sample_files"), SampleFiles);
		return Root;
	}

	static TSharedRef<FJsonObject> PlanInputSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("target"), FSololmcpSchemaBuilder::String(TEXT("Target landscape, PCG graph, actor, or asset path for the plan/probe."))},
			{TEXT("pcg_graph_path"), FSololmcpSchemaBuilder::String(TEXT("Optional PCG graph path for graph-authoring plans."))},
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Optional landscape actor label/name/path for landscape patch plans."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan only. These plan/probe tools do not mutate editor state."))},
			{TEXT("include_source_inventory"), FSololmcpSchemaBuilder::Boolean(TEXT("Include marker counts from the local engine source tree."))},
			{TEXT("notes"), FSololmcpSchemaBuilder::String(TEXT("Operator or agent notes echoed into the receipt."))}
		});
	}

	static bool RunInteropPlan(
		const FLandscapePatchPcgInteropSpec& Spec,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		const bool bVersionSatisfied = IsEngineAtLeast(Spec.MinMajor, Spec.MinMinor);
		bool bDryRun = true;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		bool bIncludeSourceInventory = true;
		Arguments->TryGetBoolField(TEXT("include_source_inventory"), bIncludeSourceInventory);

		FString Target;
		Arguments->TryGetStringField(TEXT("target"), Target);
		FString GraphPath;
		Arguments->TryGetStringField(TEXT("pcg_graph_path"), GraphPath);
		FString Landscape;
		Arguments->TryGetStringField(TEXT("landscape"), Landscape);
		FString Notes;
		Arguments->TryGetStringField(TEXT("notes"), Notes);

		TArray<TSharedPtr<FJsonValue>> PluginJson;
		int32 PluginsFound = 0;
		int32 PluginsEnabled = 0;
		for (const FString& PluginName : Spec.Plugins)
		{
			TSharedRef<FJsonObject> Probe = PluginProbeJson(PluginName);
			bool bFound = false;
			bool bEnabled = false;
			Probe->TryGetBoolField(TEXT("found"), bFound);
			Probe->TryGetBoolField(TEXT("enabled"), bEnabled);
			PluginsFound += bFound ? 1 : 0;
			PluginsEnabled += bEnabled ? 1 : 0;
			PluginJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		TArray<TSharedPtr<FJsonValue>> ModuleJson;
		int32 ModulesFound = 0;
		for (const FString& ModuleName : Spec.Modules)
		{
			TSharedRef<FJsonObject> Probe = ModuleProbeJson(ModuleName);
			bool bExists = false;
			Probe->TryGetBoolField(TEXT("exists"), bExists);
			ModulesFound += bExists ? 1 : 0;
			ModuleJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		const bool bPluginSatisfied = Spec.Plugins.IsEmpty() ||
			(Spec.bRequireAllPlugins ? PluginsEnabled == Spec.Plugins.Num() : PluginsEnabled > 0);
		const bool bModuleSatisfied = Spec.Modules.IsEmpty() || ModulesFound == Spec.Modules.Num();
		const bool bAvailable = bVersionSatisfied && bPluginSatisfied && bModuleSatisfied;

		FString Status;
		if (!bVersionSatisfied)
		{
			Status = FString::Printf(TEXT("requires_ue_%d_%d"), Spec.MinMajor, Spec.MinMinor);
		}
		else if (!bPluginSatisfied)
		{
			Status = PluginsFound > 0 ? TEXT("plugin_present_not_enabled") : TEXT("plugin_missing");
		}
		else if (!bModuleSatisfied)
		{
			Status = TEXT("module_missing");
		}
		else
		{
			Status = TEXT("available_plan_ready");
		}

		TArray<TSharedPtr<FJsonValue>> NodePlans;
		for (const FString& NodeClassPath : Spec.NodeClassPaths)
		{
			TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("node_class_path"), NodeClassPath);
			Node->SetStringField(TEXT("lookup_tool"), TEXT("pcg_node_catalog_lookup"));
			Node->SetStringField(TEXT("add_tool"), TEXT("pcg_graph_add_node"));
			Node->SetStringField(TEXT("validate_tool"), TEXT("pcg_graph_validate"));
			NodePlans.Add(MakeShared<FJsonValueObject>(Node));
		}

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetBoolField(TEXT("read_only"), true);
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("query"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("read_only"));
		OutStructured->SetStringField(TEXT("tool_name"), Spec.ToolName);
		OutStructured->SetStringField(TEXT("domain"), Spec.Domain);
		OutStructured->SetStringField(TEXT("mode"), Spec.Mode);
		OutStructured->SetStringField(TEXT("status"), Status);
		OutStructured->SetBoolField(TEXT("available"), bAvailable);
		OutStructured->SetBoolField(TEXT("version_satisfied"), bVersionSatisfied);
		OutStructured->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		OutStructured->SetStringField(TEXT("minimum_engine_version"), FString::Printf(TEXT("%d.%d.0"), Spec.MinMajor, Spec.MinMinor));
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetStringField(TEXT("target"), Target);
		OutStructured->SetStringField(TEXT("pcg_graph_path"), GraphPath);
		OutStructured->SetStringField(TEXT("landscape"), Landscape);
		OutStructured->SetStringField(TEXT("notes"), Notes);
		OutStructured->SetArrayField(TEXT("plugins"), PluginJson);
		OutStructured->SetArrayField(TEXT("modules"), ModuleJson);
		OutStructured->SetArrayField(TEXT("node_plan"), NodePlans);
		OutStructured->SetArrayField(TEXT("plan_steps"), StringArrayJson(Spec.PlanSteps));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson(Spec.ReceiptRequirements));
		OutStructured->SetArrayField(TEXT("existing_coverage_tools"), StringArrayJson(Spec.ExistingCoverageTools));
		OutStructured->SetArrayField(TEXT("fallback_tools"), StringArrayJson(Spec.FallbackTools));
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));

		if (Spec.bScanSources && bIncludeSourceInventory)
		{
			OutStructured->SetObjectField(TEXT("source_inventory"), SourceInventoryJson(Spec));
		}

		OutSummary = FString::Printf(TEXT("%s: %s on UE %s."), *Spec.ToolName, *Status, *CurrentEngineVersionString());
		return true;
	}

	static void AddReceiptIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		int32& ErrorCount,
		int32& WarningCount,
		const FString& Severity,
		const FString& Code,
		const FString& Message,
		const FString& Hint = FString())
	{
		TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		Issue->SetStringField(TEXT("hint"), Hint);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
		if (Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			++ErrorCount;
		}
		else if (Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
		{
			++WarningCount;
		}
	}

	static bool JsonBoolTrue(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
	{
		bool bValue = false;
		return Obj.IsValid() && Obj->TryGetBoolField(Field, bValue) && bValue;
	}

	static bool JsonHasString(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
	{
		FString Value;
		return Obj.IsValid() && Obj->TryGetStringField(Field, Value) && !Value.TrimStartAndEnd().IsEmpty();
	}

	static bool JsonHasObject(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		return Obj.IsValid()
			&& SomolTryGetObjectField(Obj, Field, Child)
			&& Child
			&& Child->IsValid();
	}

	static bool JsonHasArray(const TSharedPtr<FJsonObject>& Obj, const FString& Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(Field, Values) && Values && Values->Num() > 0;
	}

	static bool PluginsEnabledFromArray(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& RequiredPlugins)
	{
		const TArray<TSharedPtr<FJsonValue>>* Plugins = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(TEXT("plugins"), Plugins) || !Plugins)
		{
			return false;
		}
		TSet<FString> Enabled;
		for (const TSharedPtr<FJsonValue>& Value : *Plugins)
		{
			const TSharedPtr<FJsonObject> PluginObj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!PluginObj.IsValid())
			{
				continue;
			}
			FString Name;
			bool bEnabled = false;
			if (PluginObj->TryGetStringField(TEXT("name"), Name) &&
				PluginObj->TryGetBoolField(TEXT("enabled"), bEnabled) &&
				bEnabled)
			{
				Enabled.Add(Name);
			}
		}
		for (const FString& Required : RequiredPlugins)
		{
			if (!Enabled.Contains(Required))
			{
				return false;
			}
		}
		return true;
	}

	static bool ExecuteChildTool(
		FSololmcpToolRegistry& Registry,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Args,
		TArray<TSharedPtr<FJsonValue>>& Steps,
		TSharedPtr<FJsonObject>& OutPayload,
		FString& OutError,
		const bool bFatal = true)
	{
		TSharedRef<FJsonObject> StepOut = MakeShared<FJsonObject>();
		FString StepSummary;
		FString StepError;
		const bool bOk = Registry.ExecuteTool(ToolName, Args, StepOut, StepSummary, StepError);

		TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("tool"), ToolName);
		Step->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("failed"));
		Step->SetStringField(TEXT("summary"), StepSummary);
		Step->SetStringField(TEXT("error"), StepError);
		Step->SetObjectField(TEXT("result"), StepOut);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
		OutPayload = StepOut;

		if (!bOk && bFatal)
		{
			OutError = FString::Printf(TEXT("%s failed: %s"), *ToolName, *StepError);
			return false;
		}
		return bOk;
	}

	struct FMeshPartitionApplyNode
	{
		FString Role;
		FString LabelSuffix;
		FString ClassPath;
		FString PropertiesField;
	};

	static const TArray<FMeshPartitionApplyNode>& MeshPartitionApplyNodes()
	{
		static const TArray<FMeshPartitionApplyNode> Nodes = {
			{TEXT("query"), TEXT("Query"), TEXT("/Script/PCGMeshPartitionInterop.PCGQuerySettings"), TEXT("query_properties")},
			{TEXT("write"), TEXT("Write"), TEXT("/Script/PCGMeshPartitionInterop.PCGWriteSettings"), TEXT("write_properties")},
			{TEXT("sculpt_layer"), TEXT("SculptLayerWrite"), TEXT("/Script/PCGMeshPartitionInterop.PCGSculptLayerWriteSettings"), TEXT("sculpt_layer_properties")},
			{TEXT("projection_spawner"), TEXT("ProjectionSpawner"), TEXT("/Script/PCGMeshPartitionInterop.PCGProjectionSpawnerSettings"), TEXT("projection_spawner_properties")},
			{TEXT("patch_spawner"), TEXT("PatchSpawner"), TEXT("/Script/PCGMeshPartitionInterop.PCGPatchInstanceSpawnerSettings"), TEXT("patch_spawner_properties")}
		};
		return Nodes;
	}

	static bool MeshPartitionRoleSelected(const FString& Mode, const TSet<FString>& ExplicitRoles, const FString& Role)
	{
		if (ExplicitRoles.Num() > 0)
		{
			return ExplicitRoles.Contains(Role);
		}
		if (Mode.Equals(TEXT("query_only"), ESearchCase::IgnoreCase))
		{
			return Role.Equals(TEXT("query"), ESearchCase::IgnoreCase);
		}
		if (Mode.Equals(TEXT("query_write"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("write"), ESearchCase::IgnoreCase))
		{
			return Role.Equals(TEXT("query"), ESearchCase::IgnoreCase) || Role.Equals(TEXT("write"), ESearchCase::IgnoreCase);
		}
		if (Mode.Equals(TEXT("query_sculpt"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("sculpt"), ESearchCase::IgnoreCase))
		{
			return Role.Equals(TEXT("query"), ESearchCase::IgnoreCase) || Role.Equals(TEXT("sculpt_layer"), ESearchCase::IgnoreCase);
		}
		if (Mode.Equals(TEXT("projection"), ESearchCase::IgnoreCase))
		{
			return Role.Equals(TEXT("query"), ESearchCase::IgnoreCase) || Role.Equals(TEXT("projection_spawner"), ESearchCase::IgnoreCase);
		}
		if (Mode.Equals(TEXT("patch"), ESearchCase::IgnoreCase))
		{
			return Role.Equals(TEXT("query"), ESearchCase::IgnoreCase) || Role.Equals(TEXT("patch_spawner"), ESearchCase::IgnoreCase);
		}
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> MeshPartitionNodePlanJson(const TArray<FMeshPartitionApplyNode>& Nodes, const FString& NodePrefix)
	{
		TArray<TSharedPtr<FJsonValue>> Plan;
		for (const FMeshPartitionApplyNode& Node : Nodes)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("role"), Node.Role);
			Item->SetStringField(TEXT("node_label"), NodePrefix + TEXT("_") + Node.LabelSuffix);
			Item->SetStringField(TEXT("node_class_path"), Node.ClassPath);
			Item->SetStringField(TEXT("properties_field"), Node.PropertiesField);
			Plan.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Plan;
	}

	static bool MeshPartitionInteropAvailable(TSharedRef<FJsonObject>& OutStructured, FString& OutStatus)
	{
		const bool bVersionSatisfied = IsEngineAtLeast(5, 8);
		TArray<TSharedPtr<FJsonValue>> PluginJson;
		int32 PluginsEnabled = 0;
		for (const FString& PluginName : {FString(TEXT("PCGMeshPartitionInterop")), FString(TEXT("MeshPartition"))})
		{
			TSharedRef<FJsonObject> Probe = PluginProbeJson(PluginName);
			bool bEnabled = false;
			Probe->TryGetBoolField(TEXT("enabled"), bEnabled);
			PluginsEnabled += bEnabled ? 1 : 0;
			PluginJson.Add(MakeShared<FJsonValueObject>(Probe));
		}

		TArray<TSharedPtr<FJsonValue>> ModuleJson;
		TSharedRef<FJsonObject> Module = ModuleProbeJson(TEXT("PCGMeshPartitionInteropEditor"));
		bool bModuleExists = false;
		Module->TryGetBoolField(TEXT("exists"), bModuleExists);
		ModuleJson.Add(MakeShared<FJsonValueObject>(Module));

		const bool bPluginsEnabled = PluginsEnabled == 2;
		const bool bAvailable = bVersionSatisfied && bPluginsEnabled && bModuleExists;
		if (!bVersionSatisfied)
		{
			OutStatus = TEXT("requires_ue_5_8");
		}
		else if (!bPluginsEnabled)
		{
			OutStatus = TEXT("plugin_not_enabled");
		}
		else if (!bModuleExists)
		{
			OutStatus = TEXT("module_missing");
		}
		else
		{
			OutStatus = TEXT("available");
		}

		OutStructured->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		OutStructured->SetStringField(TEXT("minimum_engine_version"), TEXT("5.8.0"));
		OutStructured->SetBoolField(TEXT("version_satisfied"), bVersionSatisfied);
		OutStructured->SetBoolField(TEXT("available"), bAvailable);
		OutStructured->SetStringField(TEXT("availability_status"), OutStatus);
		OutStructured->SetArrayField(TEXT("plugins"), PluginJson);
		OutStructured->SetArrayField(TEXT("modules"), ModuleJson);
		return bAvailable;
	}

	static bool Tool_Ue58PcgMeshPartitionGraphApply(
		FSololmcpToolRegistry& Registry,
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		OutStructured->SetStringField(TEXT("schema"), TEXT("somol.ue58_pcg_mesh_partition_graph_apply_receipt.v1"));
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("create"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("mutating"));
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));

		FString AvailabilityStatus;
		const bool bAvailable = MeshPartitionInteropAvailable(OutStructured, AvailabilityStatus);

		FString TargetGraphPath;
		Arguments->TryGetStringField(TEXT("target_graph_path"), TargetGraphPath);
		if (TargetGraphPath.IsEmpty())
		{
			Arguments->TryGetStringField(TEXT("pcg_graph_path"), TargetGraphPath);
		}

		FString PackagePath;
		FString AssetName;
		Arguments->TryGetStringField(TEXT("package_path"), PackagePath);
		Arguments->TryGetStringField(TEXT("asset_name"), AssetName);
		if (TargetGraphPath.IsEmpty() && !PackagePath.IsEmpty() && !AssetName.IsEmpty())
		{
			TargetGraphPath = PackagePath.EndsWith(TEXT("/"))
				? PackagePath + AssetName
				: PackagePath + TEXT("/") + AssetName;
		}

		FString Mode = TEXT("query_write");
		Arguments->TryGetStringField(TEXT("mode"), Mode);
		FString NodePrefix = TEXT("MeshPartition");
		Arguments->TryGetStringField(TEXT("node_prefix"), NodePrefix);

		TSet<FString> ExplicitRoles;
		const TArray<TSharedPtr<FJsonValue>>* RoleArray = nullptr;
		if (Arguments->TryGetArrayField(TEXT("node_roles"), RoleArray) && RoleArray)
		{
			for (const TSharedPtr<FJsonValue>& Value : *RoleArray)
			{
				if (Value.IsValid())
				{
					ExplicitRoles.Add(Value->AsString());
				}
			}
		}

		TArray<FMeshPartitionApplyNode> SelectedNodes;
		for (const FMeshPartitionApplyNode& Node : MeshPartitionApplyNodes())
		{
			if (MeshPartitionRoleSelected(Mode, ExplicitRoles, Node.Role))
			{
				SelectedNodes.Add(Node);
			}
		}

		bool bDryRun = false;
		Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
		bool bAllowMutation = false;
		Arguments->TryGetBoolField(TEXT("allow_mutation"), bAllowMutation);
		bool bCreateIfMissing = false;
		Arguments->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);
		bool bSnapshotBefore = true;
		Arguments->TryGetBoolField(TEXT("snapshot_before"), bSnapshotBefore);
		bool bValidateAfter = true;
		Arguments->TryGetBoolField(TEXT("validate_after"), bValidateAfter);
		bool bRollbackOnFailure = true;
		Arguments->TryGetBoolField(TEXT("rollback_on_failure"), bRollbackOnFailure);

		OutStructured->SetStringField(TEXT("target_graph_path"), TargetGraphPath);
		OutStructured->SetStringField(TEXT("mode"), Mode);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetBoolField(TEXT("allow_mutation"), bAllowMutation);
		OutStructured->SetBoolField(TEXT("create_if_missing"), bCreateIfMissing);
		OutStructured->SetBoolField(TEXT("snapshot_before"), bSnapshotBefore);
		OutStructured->SetBoolField(TEXT("validate_after"), bValidateAfter);
		OutStructured->SetArrayField(TEXT("node_plan"), MeshPartitionNodePlanJson(SelectedNodes, NodePrefix));
		OutStructured->SetArrayField(TEXT("receipt_requirements"), StringArrayJson({
			TEXT("target_graph_path"),
			TEXT("version_satisfied"),
			TEXT("plugins"),
			TEXT("nodes_added"),
			TEXT("post_validate"),
			TEXT("snapshot_asset_path_or_dry_run"),
			TEXT("rollback_status")
		}));

		if (!bAvailable)
		{
			OutStructured->SetBoolField(TEXT("mutation_performed"), false);
			OutStructured->SetBoolField(TEXT("receipt_complete"), false);
			OutStructured->SetStringField(TEXT("receipt_status"), AvailabilityStatus);
			OutError = FString::Printf(TEXT("UE 5.8 Mesh Partition interop unavailable: %s"), *AvailabilityStatus);
			return false;
		}
		if (TargetGraphPath.IsEmpty())
		{
			OutError = TEXT("Missing target_graph_path or package_path + asset_name.");
			OutStructured->SetBoolField(TEXT("receipt_complete"), false);
			OutStructured->SetStringField(TEXT("receipt_status"), TEXT("missing_target_graph_path"));
			return false;
		}
		if (SelectedNodes.Num() == 0)
		{
			OutError = TEXT("No Mesh Partition node roles selected.");
			OutStructured->SetBoolField(TEXT("receipt_complete"), false);
			OutStructured->SetStringField(TEXT("receipt_status"), TEXT("no_nodes_selected"));
			return false;
		}
		if (bDryRun || !bAllowMutation)
		{
			OutStructured->SetBoolField(TEXT("read_only"), true);
			OutStructured->SetBoolField(TEXT("mutation_performed"), false);
			OutStructured->SetBoolField(TEXT("would_mutate"), true);
			OutStructured->SetBoolField(TEXT("receipt_complete"), true);
			OutStructured->SetStringField(TEXT("receipt_status"), bDryRun ? TEXT("dry_run_ready") : TEXT("blocked_until_allow_mutation_true"));
			OutSummary = FString::Printf(TEXT("UE5.8 Mesh Partition graph apply %s for '%s' (%d node role(s))."),
				bDryRun ? TEXT("dry-run") : TEXT("blocked"),
				*TargetGraphPath,
				SelectedNodes.Num());
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Steps;
		TSharedPtr<FJsonObject> ChildOut;
		FString SnapshotPath;
		bool bCreatedGraph = false;
		bool bMutationPerformed = false;
		bool bRollbackAttempted = false;
		bool bRollbackSucceeded = false;

		if (bCreateIfMissing)
		{
			if (PackagePath.IsEmpty() || AssetName.IsEmpty())
			{
				OutError = TEXT("create_if_missing requires package_path and asset_name.");
				OutStructured->SetBoolField(TEXT("receipt_complete"), false);
				OutStructured->SetStringField(TEXT("receipt_status"), TEXT("missing_create_graph_args"));
				return false;
			}
			TSharedRef<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
			CreateArgs->SetStringField(TEXT("package_path"), PackagePath);
			CreateArgs->SetStringField(TEXT("asset_name"), AssetName);
			if (!ExecuteChildTool(Registry, TEXT("pcg_graph_create"), CreateArgs, Steps, ChildOut, OutError, true))
			{
				OutStructured->SetArrayField(TEXT("steps"), Steps);
				OutStructured->SetBoolField(TEXT("receipt_complete"), false);
				OutStructured->SetStringField(TEXT("receipt_status"), TEXT("graph_create_failed"));
				return false;
			}
			bCreatedGraph = true;
		}
		else if (bSnapshotBefore)
		{
			TSharedRef<FJsonObject> SnapshotArgs = MakeShared<FJsonObject>();
			SnapshotArgs->SetStringField(TEXT("asset_path"), TargetGraphPath);
			SnapshotArgs->SetStringField(TEXT("tag"), TEXT("ue58_mesh_partition_pre_apply"));
			if (!ExecuteChildTool(Registry, TEXT("pcg_graph_snapshot"), SnapshotArgs, Steps, ChildOut, OutError, true))
			{
				OutStructured->SetArrayField(TEXT("steps"), Steps);
				OutStructured->SetBoolField(TEXT("receipt_complete"), false);
				OutStructured->SetStringField(TEXT("receipt_status"), TEXT("snapshot_failed"));
				return false;
			}
			if (ChildOut.IsValid())
			{
				ChildOut->TryGetStringField(TEXT("snapshot_asset_path"), SnapshotPath);
			}
		}

		int32 NodesAdded = 0;
		int32 PropertiesApplied = 0;
		TArray<TSharedPtr<FJsonValue>> AddedNodes;
		for (const FMeshPartitionApplyNode& Node : SelectedNodes)
		{
			const FString NodeLabel = NodePrefix + TEXT("_") + Node.LabelSuffix;
			TSharedRef<FJsonObject> AddArgs = MakeShared<FJsonObject>();
			AddArgs->SetStringField(TEXT("asset_path"), TargetGraphPath);
			AddArgs->SetStringField(TEXT("node_class_path"), Node.ClassPath);
			AddArgs->SetStringField(TEXT("node_label"), NodeLabel);
			if (!ExecuteChildTool(Registry, TEXT("pcg_graph_add_node"), AddArgs, Steps, ChildOut, OutError, true))
			{
				break;
			}
			++NodesAdded;
			bMutationPerformed = true;

			TSharedRef<FJsonObject> Added = MakeShared<FJsonObject>();
			Added->SetStringField(TEXT("role"), Node.Role);
			Added->SetStringField(TEXT("node_label"), NodeLabel);
			Added->SetStringField(TEXT("node_class_path"), Node.ClassPath);
			AddedNodes.Add(MakeShared<FJsonValueObject>(Added));

			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (SomolTryGetObjectField(Arguments, Node.PropertiesField, Properties) && Properties && Properties->IsValid())
			{
				TSharedRef<FJsonObject> PropsArgs = MakeShared<FJsonObject>();
				PropsArgs->SetStringField(TEXT("asset_path"), TargetGraphPath);
				PropsArgs->SetStringField(TEXT("node"), NodeLabel);
				PropsArgs->SetObjectField(TEXT("properties"), *Properties);
				if (ExecuteChildTool(Registry, TEXT("pcg_graph_set_node_property"), PropsArgs, Steps, ChildOut, OutError, false))
				{
					++PropertiesApplied;
				}
			}
		}

		int32 ConnectionsAttempted = 0;
		int32 ConnectionsCreated = 0;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (Arguments->TryGetArrayField(TEXT("edges"), Edges) && Edges)
		{
			for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
			{
				const TSharedPtr<FJsonObject> Edge = EdgeValue.IsValid() ? EdgeValue->AsObject() : nullptr;
				if (!Edge.IsValid())
				{
					continue;
				}
				FString Source;
				FString Target;
				if (!(Edge->TryGetStringField(TEXT("source_pin_path"), Source) || Edge->TryGetStringField(TEXT("from"), Source)) ||
					!(Edge->TryGetStringField(TEXT("target_pin_path"), Target) || Edge->TryGetStringField(TEXT("to"), Target)))
				{
					continue;
				}
				TSharedRef<FJsonObject> ConnectArgs = MakeShared<FJsonObject>();
				ConnectArgs->SetStringField(TEXT("asset_path"), TargetGraphPath);
				ConnectArgs->SetStringField(TEXT("source_pin_path"), Source);
				ConnectArgs->SetStringField(TEXT("target_pin_path"), Target);
				++ConnectionsAttempted;
				if (ExecuteChildTool(Registry, TEXT("pcg_graph_connect"), ConnectArgs, Steps, ChildOut, OutError, false))
				{
					++ConnectionsCreated;
				}
			}
		}

		bool bValidateOk = true;
		if (bValidateAfter)
		{
			TSharedRef<FJsonObject> ValidateArgs = MakeShared<FJsonObject>();
			ValidateArgs->SetStringField(TEXT("asset_path"), TargetGraphPath);
			bValidateOk = ExecuteChildTool(Registry, TEXT("pcg_graph_validate"), ValidateArgs, Steps, ChildOut, OutError, false);
			if (ChildOut.IsValid())
			{
				OutStructured->SetObjectField(TEXT("post_validate"), ChildOut);
			}
		}

		const bool bNeedRollback = bRollbackOnFailure && !bValidateOk && !SnapshotPath.IsEmpty();
		if (bNeedRollback)
		{
			bRollbackAttempted = true;
			TSharedRef<FJsonObject> RestoreArgs = MakeShared<FJsonObject>();
			RestoreArgs->SetStringField(TEXT("snapshot_asset_path"), SnapshotPath);
			RestoreArgs->SetStringField(TEXT("target_asset_path"), TargetGraphPath);
			RestoreArgs->SetBoolField(TEXT("force"), true);
			bRollbackSucceeded = ExecuteChildTool(Registry, TEXT("pcg_graph_restore"), RestoreArgs, Steps, ChildOut, OutError, false);
		}

		const bool bComplete = NodesAdded == SelectedNodes.Num() && (!bValidateAfter || bValidateOk);
		OutStructured->SetBoolField(TEXT("read_only"), false);
		OutStructured->SetBoolField(TEXT("created_graph"), bCreatedGraph);
		OutStructured->SetBoolField(TEXT("mutation_performed"), bMutationPerformed);
		OutStructured->SetStringField(TEXT("snapshot_asset_path"), SnapshotPath);
		OutStructured->SetNumberField(TEXT("nodes_requested"), SelectedNodes.Num());
		OutStructured->SetNumberField(TEXT("nodes_added"), NodesAdded);
		OutStructured->SetArrayField(TEXT("added_nodes"), AddedNodes);
		OutStructured->SetNumberField(TEXT("properties_applied"), PropertiesApplied);
		OutStructured->SetNumberField(TEXT("connections_attempted"), ConnectionsAttempted);
		OutStructured->SetNumberField(TEXT("connections_created"), ConnectionsCreated);
		OutStructured->SetBoolField(TEXT("post_validate_passed"), bValidateOk);
		OutStructured->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
		OutStructured->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
		OutStructured->SetArrayField(TEXT("steps"), Steps);
		OutStructured->SetBoolField(TEXT("receipt_complete"), bComplete && !bNeedRollback);
		OutStructured->SetStringField(TEXT("receipt_status"), bComplete && !bNeedRollback ? TEXT("completed") : TEXT("failed_validation"));
		OutStructured->SetStringField(TEXT("next_recommended_tool"), TEXT("ue58_pcg_mesh_partition_receipt_validate"));

		OutSummary = FString::Printf(TEXT("UE5.8 Mesh Partition graph apply: graph=%s nodes=%d/%d validate=%s rollback=%s."),
			*TargetGraphPath,
			NodesAdded,
			SelectedNodes.Num(),
			bValidateOk ? TEXT("pass") : TEXT("fail"),
			bRollbackAttempted ? (bRollbackSucceeded ? TEXT("succeeded") : TEXT("failed")) : TEXT("not_needed"));

		if (!bComplete || bNeedRollback)
		{
			OutError = OutSummary;
			return false;
		}
		return true;
	}

	static bool Tool_Ue58PcgMeshPartitionReceiptValidate(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		TSharedPtr<FJsonObject> Receipt;
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			Receipt = Arguments;
		}
		else
		{
			Receipt = *ReceiptPtr;
		}

		bool bStrict = true;
		Arguments->TryGetBoolField(TEXT("strict"), bStrict);

		TArray<TSharedPtr<FJsonValue>> Issues;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;

		if (!JsonHasString(Receipt, TEXT("target_graph_path")) && !JsonHasString(Receipt, TEXT("pcg_graph_path")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("missing_target_graph_path"), TEXT("Receipt must bind the target PCG graph."), TEXT("Include target_graph_path from the apply tool."));
		}
		if (!JsonBoolTrue(Receipt, TEXT("version_satisfied")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("version_not_satisfied"), TEXT("UE 5.8 Mesh Partition tools must prove engine version >= 5.8."), TEXT("Use ue58_pcg_mesh_partition_capability_probe before apply."));
		}
		if (!PluginsEnabledFromArray(Receipt, {TEXT("PCGMeshPartitionInterop"), TEXT("MeshPartition")}))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("plugins_not_enabled"), TEXT("PCGMeshPartitionInterop and MeshPartition must be enabled in receipt evidence."), TEXT("Enable plugins in .uproject and restart the editor."));
		}
		if (!JsonBoolTrue(Receipt, TEXT("dry_run")) && !JsonBoolTrue(Receipt, TEXT("mutation_performed")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("no_mutation_or_dry_run"), TEXT("Receipt must be either dry_run or a proven mutation."), TEXT("Set dry_run=true for planning or allow_mutation=true for graph apply."));
		}
		if (JsonBoolTrue(Receipt, TEXT("mutation_performed")) && !JsonHasString(Receipt, TEXT("snapshot_asset_path")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, bStrict ? TEXT("error") : TEXT("warning"), TEXT("missing_snapshot"), TEXT("Mutating apply should capture a pre-edit PCG graph snapshot."), TEXT("Run with snapshot_before=true or provide rollback evidence."));
		}
		if (JsonBoolTrue(Receipt, TEXT("mutation_performed")) && !JsonBoolTrue(Receipt, TEXT("post_validate_passed")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("post_validate_failed_or_missing"), TEXT("Mutating Mesh Partition graph apply must pass pcg_graph_validate."), TEXT("Inspect post_validate and repair graph pins/properties before generate."));
		}
		if (JsonBoolTrue(Receipt, TEXT("rollback_attempted")) && !JsonBoolTrue(Receipt, TEXT("rollback_succeeded")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("rollback_failed"), TEXT("Rollback was attempted but did not succeed."), TEXT("Quarantine this graph and route to QA/Hermes."));
		}
		if (!JsonHasArray(Receipt, TEXT("added_nodes")) && !JsonHasArray(Receipt, TEXT("node_plan")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("warning"), TEXT("missing_node_evidence"), TEXT("No node evidence found."), TEXT("Include added_nodes or node_plan for auditability."));
		}

		const bool bAccepted = ErrorCount == 0;
		OutStructured->SetStringField(TEXT("schema"), TEXT("somol.ue58_pcg_mesh_partition_receipt_gate.v1"));
		OutStructured->SetBoolField(TEXT("accepted"), bAccepted);
		OutStructured->SetStringField(TEXT("receipt_status"), bAccepted ? TEXT("accepted") : TEXT("failed_validation"));
		OutStructured->SetNumberField(TEXT("error_count"), ErrorCount);
		OutStructured->SetNumberField(TEXT("warning_count"), WarningCount);
		OutStructured->SetArrayField(TEXT("issues"), Issues);
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		OutSummary = FString::Printf(TEXT("UE5.8 Mesh Partition receipt gate: %s (%d errors, %d warnings)."),
			bAccepted ? TEXT("accepted") : TEXT("failed"),
			ErrorCount,
			WarningCount);
		return true;
	}

	static bool Tool_PcgTerrainProductionReceiptGate(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		TSharedPtr<FJsonObject> Receipt;
		const TSharedPtr<FJsonObject>* ReceiptPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptPtr) || !ReceiptPtr || !ReceiptPtr->IsValid())
		{
			Receipt = Arguments;
		}
		else
		{
			Receipt = *ReceiptPtr;
		}

		bool bRequirePreview = true;
		bool bRequireHealth = true;
		bool bRequireDryRun = true;
		bool bRequireTileCap = true;
		bool bRequireValidation = true;
		Arguments->TryGetBoolField(TEXT("require_preview"), bRequirePreview);
		Arguments->TryGetBoolField(TEXT("require_health_audit"), bRequireHealth);
		Arguments->TryGetBoolField(TEXT("require_dry_run"), bRequireDryRun);
		Arguments->TryGetBoolField(TEXT("require_tile_cap"), bRequireTileCap);
		Arguments->TryGetBoolField(TEXT("require_validation"), bRequireValidation);

		TArray<TSharedPtr<FJsonValue>> Issues;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;

		if (!JsonHasString(Receipt, TEXT("target_graph_path")) &&
			!JsonHasString(Receipt, TEXT("graph_path")) &&
			!JsonHasString(Receipt, TEXT("asset_path")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("missing_graph_binding"), TEXT("PCG/terrain production receipt must bind the graph or target asset."), TEXT("Include graph_path/target_graph_path and target project binding in the outer job receipt."));
		}
		if (bRequireValidation && !JsonBoolTrue(Receipt, TEXT("post_validate_passed")) && !JsonHasObject(Receipt, TEXT("validate_receipt")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("missing_validate"), TEXT("Missing pcg_graph_validate evidence."), TEXT("Run pcg_graph_validate before generate and attach the structured output."));
		}
		if (bRequireDryRun && !JsonHasObject(Receipt, TEXT("dry_run_receipt")) && !JsonHasObject(Receipt, TEXT("dry_run")) && !JsonHasObject(Receipt, TEXT("dry_run_calibration")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("missing_dry_run"), TEXT("Missing dry-run or calibration evidence."), TEXT("Attach pcg_dry_run or pcg_dry_run_calibration_receipt output."));
		}
		if (bRequireTileCap && !JsonHasObject(Receipt, TEXT("tile_cap_policy")) && !JsonHasObject(Receipt, TEXT("tile_cap_receipt")) && !JsonHasArray(Receipt, TEXT("tiles")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("missing_tile_cap"), TEXT("Missing tile cap / tile execution evidence."), TEXT("Use pcg_tile_cap_guard, pcg_partition_preview, or pcg_tile_generation_status."));
		}
		if (bRequireHealth && !JsonHasObject(Receipt, TEXT("health_audit")) && !JsonHasString(Receipt, TEXT("health_status")) && !JsonHasObject(Receipt, TEXT("generated_actor_health_audit")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("missing_generated_health"), TEXT("Missing generated actor health/provenance audit."), TEXT("Run pcg_generated_actor_health_audit and pcg_generated_dependency_graph after generation."));
		}
		if (bRequirePreview && !JsonHasObject(Receipt, TEXT("preview_receipt")) && !JsonHasString(Receipt, TEXT("screenshot_path")) && !JsonHasString(Receipt, TEXT("thumbnail_path")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("missing_preview"), TEXT("Missing screenshot/thumbnail/preview evidence."), TEXT("Capture editor_get_screenshot or terrain preview export evidence before delivery."));
		}
		if (JsonBoolTrue(Receipt, TEXT("rollback_attempted")) && !JsonBoolTrue(Receipt, TEXT("rollback_succeeded")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("error"), TEXT("rollback_failed"), TEXT("Rollback failed or is unclear."), TEXT("Stop the lane and route to QA/Hermes."));
		}
		if (!JsonHasString(Receipt, TEXT("cleanup_status")) && !JsonBoolTrue(Receipt, TEXT("cleanup_attempted")))
		{
			AddReceiptIssue(Issues, ErrorCount, WarningCount, TEXT("warning"), TEXT("cleanup_unclear"), TEXT("Cleanup status is not explicit."), TEXT("Disposable test assets should report cleanup_attempted and cleanup result."));
		}

		const bool bAccepted = ErrorCount == 0;
		OutStructured->SetStringField(TEXT("schema"), TEXT("somol.pcg_terrain_production_receipt_gate.v1"));
		OutStructured->SetBoolField(TEXT("accepted"), bAccepted);
		OutStructured->SetStringField(TEXT("receipt_status"), bAccepted ? TEXT("accepted") : TEXT("failed_validation"));
		OutStructured->SetNumberField(TEXT("error_count"), ErrorCount);
		OutStructured->SetNumberField(TEXT("warning_count"), WarningCount);
		OutStructured->SetArrayField(TEXT("issues"), Issues);
		OutStructured->SetArrayField(TEXT("required_evidence"), StringArrayJson({
			TEXT("target_graph_or_asset_binding"),
			TEXT("pcg_graph_validate"),
			TEXT("dry_run_or_calibration"),
			TEXT("tile_cap_policy"),
			TEXT("generated_actor_health"),
			TEXT("preview_evidence"),
			TEXT("rollback_or_cleanup_status")
		}));
		OutSummary = FString::Printf(TEXT("PCG/Terrain production gate: %s (%d errors, %d warnings)."),
			bAccepted ? TEXT("accepted") : TEXT("failed"),
			ErrorCount,
			WarningCount);
		return true;
	}

	static bool Tool_McpP1P2LandscapePcgCapabilityReport(
		const FSololmcpToolExecutionContext&,
		const TSharedRef<FJsonObject>&,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString&)
	{
		TArray<TSharedPtr<FJsonValue>> Completed;
		auto AddCompleted = [&Completed](const FString& Area, const TArray<FString>& Tools)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("area"), Area);
			Item->SetArrayField(TEXT("tools"), StringArrayJson(Tools));
			Completed.Add(MakeShared<FJsonValueObject>(Item));
		};

		AddCompleted(TEXT("ue58_mesh_partition_execution"), {
			TEXT("ue58_pcg_mesh_partition_graph_apply"),
			TEXT("ue58_pcg_mesh_partition_receipt_validate"),
			TEXT("ue58_pcg_mesh_partition_capability_probe")
		});
		AddCompleted(TEXT("pcg_terrain_delivery_gates"), {
			TEXT("pcg_terrain_production_receipt_gate"),
			TEXT("pcg_dry_run_calibration_receipt"),
			TEXT("pcg_tile_cap_guard"),
			TEXT("pcg_generated_actor_health_audit")
		});
		AddCompleted(TEXT("landscape_patch_execution"), {
			TEXT("landscape_patch_edit_layer_create"),
			TEXT("landscape_circle_patch_create"),
			TEXT("landscape_texture_patch_create"),
			TEXT("landscape_patch_stack_inspect")
		});

		OutStructured->SetStringField(TEXT("schema"), TEXT("somol.mcp_p1_p2_landscape_pcg_capability_report.v1"));
		OutStructured->SetStringField(TEXT("engine_version"), CurrentEngineVersionString());
		OutStructured->SetArrayField(TEXT("completed_p1_p2_slices"), Completed);
		OutStructured->SetArrayField(TEXT("remaining_live_boundaries"), StringArrayJson({
			TEXT("UE5.8 Mesh Partition mutating live smoke with generated-section readback"),
			TEXT("Large-area PCG generate calibration across forest/grass/rock/riverbank/alpine fixtures"),
			TEXT("Terrain visual QA screenshot/heightmap preview for production maps"),
			TEXT("Mixed read/write/poll live mission board lasting hours")
		}));
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		OutSummary = TEXT("Reported MCP P1/P2 Landscape/PCG capability state.");
		return true;
	}

	static ALandscape* ResolveLandscape(const FSololmcpEditorServices& Services, const FString& LandscapeId, FString& OutError)
	{
		if (LandscapeId.IsEmpty())
		{
			OutError = TEXT("Missing required argument: landscape");
			return nullptr;
		}

		AActor* Actor = Services.FindActorByLabelOrName(LandscapeId, OutError);
		if (!Actor)
		{
			return nullptr;
		}

		ALandscape* Landscape = Cast<ALandscape>(Actor);
		if (!Landscape)
		{
			OutError = FString::Printf(TEXT("Actor '%s' is not an ALandscape."), *LandscapeId);
		}
		return Landscape;
	}

	static bool SetFloatProperty(UObject* Obj, const FName PropertyName, const double Value)
	{
		if (!Obj)
		{
			return false;
		}
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropertyName);
		if (!Prop)
		{
			return false;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(Value));
			return true;
		}
		if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			DoubleProp->SetPropertyValue(ValuePtr, Value);
			return true;
		}
		return false;
	}

	static bool SetBoolProperty(UObject* Obj, const FName PropertyName, const bool bValue)
	{
		if (!Obj)
		{
			return false;
		}
		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Obj->GetClass()->FindPropertyByName(PropertyName)))
		{
			BoolProp->SetPropertyValue_InContainer(Obj, bValue);
			return true;
		}
		return false;
	}

	static bool InvokeSetLandscapeFunction(UObject* Obj, ALandscape* Landscape, FString& OutError)
	{
		if (!Obj || !Landscape)
		{
			OutError = TEXT("Missing patch component or landscape for SetLandscape.");
			return false;
		}
		UFunction* Fn = Obj->FindFunction(TEXT("SetLandscape"));
		if (!Fn)
		{
			OutError = TEXT("Patch component does not expose SetLandscape.");
			return false;
		}
		struct FSetLandscapeParams
		{
			ALandscape* NewLandscape = nullptr;
		};
		FSetLandscapeParams Params;
		Params.NewLandscape = Landscape;
		Obj->ProcessEvent(Fn, &Params);
		return true;
	}

	static bool InvokeSetEditLayerGuidFunction(UObject* Obj, const FGuid& Guid, FString& OutError)
	{
		if (!Obj || !Guid.IsValid())
		{
			OutError = TEXT("Missing patch component or valid edit layer GUID.");
			return false;
		}
		UFunction* Fn = Obj->FindFunction(TEXT("SetEditLayerGuid"));
		if (!Fn)
		{
			OutError = TEXT("Patch component does not expose SetEditLayerGuid.");
			return false;
		}
		struct FSetGuidParams
		{
			FGuid GuidIn;
		};
		FSetGuidParams Params;
		Params.GuidIn = Guid;
		Obj->ProcessEvent(Fn, &Params);
		return true;
	}

	static bool SetEnumProperty(UObject* Obj, const FName PropertyName, const FString& EnumValueName)
	{
		if (!Obj || EnumValueName.IsEmpty())
		{
			return false;
		}

		FProperty* Prop = Obj->GetClass()->FindPropertyByName(PropertyName);
		if (!Prop)
		{
			return false;
		}

		UEnum* Enum = nullptr;
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			Enum = EnumProp->GetEnum();
			Prop = EnumProp->GetUnderlyingProperty();
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			Enum = ByteProp->Enum;
		}

		if (!Enum)
		{
			return false;
		}

		FString QualifiedValue = EnumValueName;
		if (!QualifiedValue.Contains(TEXT("::")))
		{
			QualifiedValue = FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *EnumValueName);
		}

		int64 RawValue = Enum->GetValueByNameString(QualifiedValue);
		if (RawValue == INDEX_NONE)
		{
			RawValue = Enum->GetValueByNameString(EnumValueName);
		}
		if (RawValue == INDEX_NONE)
		{
			return false;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
		if (FNumericProperty* NumericProp = CastField<FNumericProperty>(Prop))
		{
			NumericProp->SetIntPropertyValue(ValuePtr, RawValue);
			return true;
		}
		return false;
	}

	static void InvokeRequestLandscapeUpdate(UActorComponent* Component);

	static bool ConfigurePatchComponentBinding(
		UActorComponent* Component,
		ALandscape* Landscape,
		const FString& LayerName,
		const bool bPatchEnabled,
		const bool bRequestUpdate,
		FString& OutError)
	{
		if (!Component || !Landscape)
		{
			OutError = TEXT("Missing patch component or landscape.");
			return false;
		}
		if (LayerName.IsEmpty())
		{
			OutError = TEXT("Missing patch edit layer name.");
			return false;
		}

		// 5.6 exposed edit layers as typed UObjects, which is what lets the patch class be
		// verified. Through 5.5 they are untyped FLandscapeLayer structs: the layer can be
		// found and its Guid bound, but there is no class to check, so that check is
		// skipped rather than faked.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		ULandscapeEditLayerBase* EditLayer = Landscape->GetEditLayer(FName(*LayerName));
		if (!EditLayer)
		{
			OutError = FString::Printf(TEXT("Patch edit layer '%s' was not found on target landscape."), *LayerName);
			return false;
		}
		if (!EditLayer->GetClass()->GetPathName().Contains(TEXT("/Script/LandscapePatch.")))
		{
			OutError = FString::Printf(TEXT("Edit layer '%s' is not a LandscapePatch edit layer."), *LayerName);
			return false;
		}
		const FGuid EditLayerGuid = EditLayer->GetGuid();
#else
		const FLandscapeLayer* EditLayer = Landscape->GetLayer(FName(*LayerName));
		if (!EditLayer)
		{
			OutError = FString::Printf(TEXT("Patch edit layer '%s' was not found on target landscape."), *LayerName);
			return false;
		}
		const FGuid EditLayerGuid = EditLayer->Guid;
#endif

		// Keep unattended creation fail-fast: a newly registered enabled patch can force an
		// expensive landscape layer merge on large maps. Agents must opt in to enabling/updating.
		SetBoolProperty(Component, TEXT("bIsEnabled"), false);
		if (!InvokeSetLandscapeFunction(Component, Landscape, OutError))
		{
			return false;
		}
		if (!InvokeSetEditLayerGuidFunction(Component, EditLayerGuid, OutError))
		{
			return false;
		}
		SetBoolProperty(Component, TEXT("bIsEnabled"), bPatchEnabled);
		if (bRequestUpdate)
		{
			InvokeRequestLandscapeUpdate(Component);
		}
		return true;
	}

	static void InvokeRequestLandscapeUpdate(UActorComponent* Component)
	{
		if (!Component)
		{
			return;
		}
		if (UFunction* RequestFn = Component->FindFunction(TEXT("RequestLandscapeUpdate")))
		{
			struct FRequestParams
			{
				bool bInUserTriggeredUpdate = false;
			};
			FRequestParams Params;
			Component->ProcessEvent(RequestFn, &Params);
		}
	}

	static void InvokeSetVector2DFunction(UObject* Obj, const TCHAR* FunctionName, const FVector2D& Value)
	{
		if (!Obj)
		{
			return;
		}
		if (UFunction* Fn = Obj->FindFunction(FunctionName))
		{
			struct FVector2DParam
			{
				FVector2D Value;
			};
			FVector2DParam Params;
			Params.Value = Value;
			Obj->ProcessEvent(Fn, &Params);
		}
	}

	static void InvokeSetTextureFunction(UObject* Obj, const TCHAR* FunctionName, UTexture* Texture)
	{
		if (!Obj || !Texture)
		{
			return;
		}
		if (UFunction* Fn = Obj->FindFunction(FunctionName))
		{
			struct FTextureParam
			{
				UTexture* TextureIn = nullptr;
			};
			FTextureParam Params;
			Params.TextureIn = Texture;
			Obj->ProcessEvent(Fn, &Params);
		}
	}

	static UClass* ResolvePatchClass(const FSololmcpEditorServices& Services, const FString& ClassPath, UClass* ExpectedBase, FString& OutError)
	{
		UClass* Class = Services.ResolveClass(ClassPath, OutError);
		if (!Class)
		{
			return nullptr;
		}
		if (ExpectedBase && !Class->IsChildOf(ExpectedBase))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a child of '%s'."), *ClassPath, *ExpectedBase->GetName());
			return nullptr;
		}
		return Class;
	}

	static int32 EnsurePatchEditLayer(ALandscape* Landscape, UClass* PatchLayerClass, const FName LayerName, const bool bIgnoreLimit, FString& OutError)
	{
		if (!Landscape || !PatchLayerClass || LayerName.IsNone())
		{
			OutError = TEXT("Missing landscape, patch layer class, or layer name.");
			return INDEX_NONE;
		}

		// Two independent boundaries here, which is why they are not one gate:
		//   * the typed edit-layer lookup (GetEditLayer -> ULandscapeEditLayerBase) is 5.6+
		//   * CreateLayer taking a layer class and an ignore-limit flag is 5.5+
		// 5.5 therefore looks the layer up as an untyped FLandscapeLayer but still creates
		// it with the patch class.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		if (ULandscapeEditLayerBase* ExistingLayer = Landscape->GetEditLayer(LayerName))
		{
			if (!ExistingLayer->IsA(PatchLayerClass))
			{
				OutError = FString::Printf(TEXT("Existing layer '%s' is not a Landscape Patch Edit Layer."), *LayerName.ToString());
				return INDEX_NONE;
			}
			return Landscape->GetLayerIndex(LayerName);
		}
#else
		// Untyped through 5.5: the layer can be found and reused, but there is no class
		// on it to verify against PatchLayerClass.
		if (Landscape->GetLayer(LayerName) != nullptr)
		{
			return Landscape->GetLayerIndex(LayerName);
		}
#endif

		// CreateLayer grew in two steps: 5.5 added the edit-layer class, 5.6 added the
		// ignore-count-limit flag. Three engine tiers, so three calls.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		TSubclassOf<ULandscapeEditLayerBase> LayerClass;
		LayerClass = PatchLayerClass;
		const int32 LayerIndex = Landscape->CreateLayer(LayerName, LayerClass, bIgnoreLimit);
#elif ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 5
		TSubclassOf<ULandscapeEditLayerBase> LayerClass;
		LayerClass = PatchLayerClass;
		(void)bIgnoreLimit;   // no ignore-limit parameter on 5.5
		const int32 LayerIndex = Landscape->CreateLayer(LayerName, LayerClass);
#else
		// Through 5.4 CreateLayer takes only a name: no class to request, no ignore-limit.
		(void)PatchLayerClass;
		(void)bIgnoreLimit;
		const int32 LayerIndex = Landscape->CreateLayer(LayerName);
#endif
		if (LayerIndex == INDEX_NONE)
		{
			OutError = TEXT("Failed to create Landscape Patch Edit Layer.");
		}
		return LayerIndex;
	}

	static AActor* SpawnPatchHostActor(const FSololmcpEditorServices& Services, const TSharedRef<FJsonObject>& Arguments, const FString& DefaultLabel, FString& OutError)
	{
		UWorld* World = Services.GetEditorWorld(OutError);
		if (!World)
		{
			return nullptr;
		}

		FVector Location = FVector::ZeroVector;
		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && LocationObj->IsValid())
		{
			FSololmcpEditorServices::JsonToVector(*LocationObj, Location);
		}

		FRotator Rotation = FRotator::ZeroRotator;
		const TSharedPtr<FJsonObject>* RotationObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("rotation"), RotationObj) && RotationObj && RotationObj->IsValid())
		{
			FSololmcpEditorServices::JsonToRotator(*RotationObj, Rotation);
		}

		FString ActorLabel = DefaultLabel;
		Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel);
		if (ActorLabel.IsEmpty())
		{
			ActorLabel = DefaultLabel;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Rotation, Location), SpawnParams);
		if (!Actor)
		{
			OutError = TEXT("Failed to spawn patch host actor.");
			return nullptr;
		}

#if WITH_EDITOR
		Actor->SetActorLabel(ActorLabel);
#endif

		USceneComponent* Root = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass(), TEXT("Root"), RF_Transactional);
		Root->CreationMethod = EComponentCreationMethod::Instance;
		Actor->AddInstanceComponent(Root);
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		return Actor;
	}

	static UActorComponent* AddPatchComponent(AActor* Actor, UClass* ComponentClass, const FString& ComponentName, FString& OutError)
	{
		if (!Actor || !ComponentClass)
		{
			OutError = TEXT("Missing patch host actor or component class.");
			return nullptr;
		}

		UActorComponent* Component = NewObject<UActorComponent>(Actor, ComponentClass, *ComponentName, RF_Transactional);
		if (!Component)
		{
			OutError = TEXT("Failed to create patch component.");
			return nullptr;
		}

		Component->CreationMethod = EComponentCreationMethod::Instance;
		Actor->AddInstanceComponent(Component);
		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			SceneComponent->AttachToComponent(Actor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
			SceneComponent->SetRelativeTransform(FTransform::Identity);
		}
		return Component;
	}

	static bool RegisterPatchComponent(UActorComponent* Component, FString& OutError)
	{
		if (!Component)
		{
			OutError = TEXT("Missing patch component to register.");
			return false;
		}
		if (!Component->IsRegistered())
		{
			Component->RegisterComponent();
		}
		return true;
	}

	static bool Tool_LandscapePatchEditLayerCreate(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString LandscapeId;
		FString LayerNameString;
		if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) ||
			!Arguments->TryGetStringField(TEXT("layer_name"), LayerNameString) ||
			LayerNameString.IsEmpty())
		{
			OutError = TEXT("Missing required arguments: landscape and layer_name.");
			return false;
		}

		ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
		if (!Landscape)
		{
			return false;
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		UClass* PatchLayerClass = ResolvePatchClass(
			Context.Services,
			TEXT("/Script/LandscapePatch.LandscapePatchEditLayer"),
			ULandscapeEditLayerBase::StaticClass(),
			OutError);
		if (!PatchLayerClass)
		{
			return false;
		}

		const bool bIgnoreLimit = Arguments->HasTypedField<EJson::Boolean>(TEXT("ignore_limit")) ? Arguments->GetBoolField(TEXT("ignore_limit")) : false;
		const FName LayerName(*LayerNameString);

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapePatchEditLayerCreate", "SOMOLMCP Create Landscape Patch Edit Layer"));
		Landscape->Modify();
		const int32 LayerIndex = EnsurePatchEditLayer(Landscape, PatchLayerClass, LayerName, bIgnoreLimit, OutError);
		if (LayerIndex == INDEX_NONE)
		{
			return false;
		}

		if (Arguments->HasTypedField<EJson::Boolean>(TEXT("set_editing_layer")) && Arguments->GetBoolField(TEXT("set_editing_layer")))
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
			if (ULandscapeEditLayerBase* Layer = Landscape->GetEditLayer(LayerName))
			{
				Landscape->SetEditingLayer(Layer->GetGuid());
			}
#else
			if (const FLandscapeLayer* Layer = Landscape->GetLayer(LayerName))
			{
				Landscape->SetEditingLayer(Layer->Guid);
			}
#endif
		}

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("create"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("mutating"));
		OutStructured->SetObjectField(TEXT("landscape"), FSololmcpEditorServices::MakeActorReference(Landscape));
		OutStructured->SetStringField(TEXT("layer_name"), LayerNameString);
		OutStructured->SetNumberField(TEXT("layer_index"), LayerIndex);
		OutStructured->SetStringField(TEXT("layer_class"), PatchLayerClass->GetPathName());
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		OutSummary = FString::Printf(TEXT("Created or reused Landscape Patch Edit Layer '%s'."), *LayerNameString);
		return true;
#else
		// LandscapePatchEditLayer does not exist before 5.5 -- there is no class to
		// create, so this is refused with a reason rather than silently degraded.
		OutError = TEXT("NOT_AVAILABLE_ON_ENGINE: Landscape Patch edit layers require UE 5.5 or later; "
			"this engine version has no LandscapePatchEditLayer class.");
		return false;
#endif
	}

	static bool CreatePatchComponentCommon(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		const FString& ComponentClassPath,
		const FString& DefaultActorLabel,
		const FString& ComponentName,
		UActorComponent*& OutComponent,
		AActor*& OutActor,
		ALandscape*& OutLandscape,
		FString& OutLayerName,
		FString& OutError)
	{
		FString LandscapeId;
		if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) ||
			!Arguments->TryGetStringField(TEXT("layer_name"), OutLayerName) ||
			OutLayerName.IsEmpty())
		{
			OutError = TEXT("Missing required arguments: landscape and layer_name.");
			return false;
		}

		OutLandscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
		if (!OutLandscape)
		{
			return false;
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		UClass* PatchLayerClass = ResolvePatchClass(
			Context.Services,
			TEXT("/Script/LandscapePatch.LandscapePatchEditLayer"),
			ULandscapeEditLayerBase::StaticClass(),
			OutError);
		if (!PatchLayerClass)
		{
			return false;
		}

		const bool bIgnoreLimit = Arguments->HasTypedField<EJson::Boolean>(TEXT("ignore_limit")) ? Arguments->GetBoolField(TEXT("ignore_limit")) : false;
		const int32 LayerIndex = EnsurePatchEditLayer(OutLandscape, PatchLayerClass, FName(*OutLayerName), bIgnoreLimit, OutError);
		if (LayerIndex == INDEX_NONE)
		{
			return false;
		}

		UClass* ComponentClass = ResolvePatchClass(Context.Services, ComponentClassPath, UActorComponent::StaticClass(), OutError);
		if (!ComponentClass)
		{
			return false;
		}

		OutActor = SpawnPatchHostActor(Context.Services, Arguments, DefaultActorLabel, OutError);
		if (!OutActor)
		{
			return false;
		}

		OutComponent = AddPatchComponent(OutActor, ComponentClass, ComponentName, OutError);
		if (!OutComponent)
		{
			return false;
		}

		return true;
#else
		// LandscapePatchEditLayer does not exist before 5.5 -- there is no class to
		// create, so this is refused with a reason rather than silently degraded.
		OutError = TEXT("NOT_AVAILABLE_ON_ENGINE: Landscape Patch edit layers require UE 5.5 or later; "
			"this engine version has no LandscapePatchEditLayer class.");
		return false;
#endif
	}

	static bool Tool_LandscapeCirclePatchCreate(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		UActorComponent* Component = nullptr;
		AActor* Actor = nullptr;
		ALandscape* Landscape = nullptr;
		FString LayerName;

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeCirclePatchCreate", "SOMOLMCP Create Landscape Circle Patch"));
		if (!CreatePatchComponentCommon(
			Context,
			Arguments,
			TEXT("/Script/LandscapePatch.LandscapeCircleHeightPatch"),
			TEXT("SOMOL_CircleHeightPatch"),
			TEXT("CircleHeightPatch"),
			Component,
			Actor,
			Landscape,
			LayerName,
			OutError))
		{
			return false;
		}

		double Radius = 500.0;
		Arguments->TryGetNumberField(TEXT("radius"), Radius);
		double Falloff = 500.0;
		Arguments->TryGetNumberField(TEXT("falloff"), Falloff);
		double Priority = 0.0;
		const bool bHasPriority = Arguments->TryGetNumberField(TEXT("priority"), Priority);
		const bool bEditVisibility = Arguments->HasTypedField<EJson::Boolean>(TEXT("edit_visibility")) ? Arguments->GetBoolField(TEXT("edit_visibility")) : false;
		const bool bExclusiveRadius = Arguments->HasTypedField<EJson::Boolean>(TEXT("exclusive_radius")) ? Arguments->GetBoolField(TEXT("exclusive_radius")) : false;
		const bool bPatchEnabled = Arguments->HasTypedField<EJson::Boolean>(TEXT("enabled")) ? Arguments->GetBoolField(TEXT("enabled")) : false;
		const bool bRequestUpdate = Arguments->HasTypedField<EJson::Boolean>(TEXT("request_update")) ? Arguments->GetBoolField(TEXT("request_update")) : false;

		Component->Modify();
		SetFloatProperty(Component, TEXT("Radius"), Radius);
		SetFloatProperty(Component, TEXT("Falloff"), Falloff);
		SetBoolProperty(Component, TEXT("bEditVisibility"), bEditVisibility);
		SetBoolProperty(Component, TEXT("bExclusiveRadius"), bExclusiveRadius);
		if (bHasPriority)
		{
			SetFloatProperty(Component, TEXT("Priority"), Priority);
		}

		if (!ConfigurePatchComponentBinding(Component, Landscape, LayerName, bPatchEnabled, bRequestUpdate, OutError))
		{
			return false;
		}
		if (!RegisterPatchComponent(Component, OutError))
		{
			return false;
		}
		Actor->MarkPackageDirty();

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("create"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("mutating"));
		OutStructured->SetObjectField(TEXT("actor"), FSololmcpEditorServices::MakeActorReference(Actor));
		OutStructured->SetObjectField(TEXT("landscape"), FSololmcpEditorServices::MakeActorReference(Landscape));
		OutStructured->SetStringField(TEXT("component_path"), Component->GetPathName());
		OutStructured->SetStringField(TEXT("layer_name"), LayerName);
		OutStructured->SetNumberField(TEXT("radius"), Radius);
		OutStructured->SetNumberField(TEXT("falloff"), Falloff);
		OutStructured->SetBoolField(TEXT("edit_visibility"), bEditVisibility);
		OutStructured->SetBoolField(TEXT("exclusive_radius"), bExclusiveRadius);
		OutStructured->SetBoolField(TEXT("enabled"), bPatchEnabled);
		OutStructured->SetBoolField(TEXT("request_update"), bRequestUpdate);
		OutStructured->SetStringField(TEXT("update_policy"), bRequestUpdate ? TEXT("explicit_update_requested") : TEXT("safe_no_update_default"));
		OutStructured->SetStringField(TEXT("next_recommended_tool"), TEXT("landscape_patch_stack_inspect"));
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		OutSummary = FString::Printf(TEXT("Created Landscape Circle Height Patch on layer '%s'."), *LayerName);
		return true;
	}

	static bool Tool_LandscapeTexturePatchCreate(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		UActorComponent* Component = nullptr;
		AActor* Actor = nullptr;
		ALandscape* Landscape = nullptr;
		FString LayerName;

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeTexturePatchCreate", "SOMOLMCP Create Landscape Texture Patch"));
		if (!CreatePatchComponentCommon(
			Context,
			Arguments,
			TEXT("/Script/LandscapePatch.LandscapeTexturePatch"),
			TEXT("SOMOL_TexturePatch"),
			TEXT("TexturePatch"),
			Component,
			Actor,
			Landscape,
			LayerName,
			OutError))
		{
			return false;
		}

		double CoverageX = 1000.0;
		double CoverageY = 1000.0;
		Arguments->TryGetNumberField(TEXT("coverage_x"), CoverageX);
		Arguments->TryGetNumberField(TEXT("coverage_y"), CoverageY);
		double ResolutionX = 256.0;
		double ResolutionY = 256.0;
		Arguments->TryGetNumberField(TEXT("resolution_x"), ResolutionX);
		Arguments->TryGetNumberField(TEXT("resolution_y"), ResolutionY);
		double Falloff = 100.0;
		Arguments->TryGetNumberField(TEXT("falloff"), Falloff);
		double Priority = 0.0;
		const bool bHasPriority = Arguments->TryGetNumberField(TEXT("priority"), Priority);
		const bool bPatchEnabled = Arguments->HasTypedField<EJson::Boolean>(TEXT("enabled")) ? Arguments->GetBoolField(TEXT("enabled")) : false;
		const bool bRequestUpdate = Arguments->HasTypedField<EJson::Boolean>(TEXT("request_update")) ? Arguments->GetBoolField(TEXT("request_update")) : false;

		FString BlendMode;
		Arguments->TryGetStringField(TEXT("blend_mode"), BlendMode);
		FString FalloffMode;
		Arguments->TryGetStringField(TEXT("falloff_mode"), FalloffMode);
		FString HeightSourceMode;
		Arguments->TryGetStringField(TEXT("height_source_mode"), HeightSourceMode);

		Component->Modify();
		InvokeSetVector2DFunction(Component, TEXT("SetUnscaledCoverage"), FVector2D(CoverageX, CoverageY));
		InvokeSetVector2DFunction(Component, TEXT("SetResolution"), FVector2D(ResolutionX, ResolutionY));
		SetFloatProperty(Component, TEXT("Falloff"), Falloff);
		if (bHasPriority)
		{
			SetFloatProperty(Component, TEXT("Priority"), Priority);
		}
		SetEnumProperty(Component, TEXT("BlendMode"), BlendMode);
		SetEnumProperty(Component, TEXT("FalloffMode"), FalloffMode);
		SetEnumProperty(Component, TEXT("HeightSourceMode"), HeightSourceMode);

		FString HeightTextureAsset;
		if (Arguments->TryGetStringField(TEXT("height_texture_asset"), HeightTextureAsset) && !HeightTextureAsset.IsEmpty())
		{
			UObject* TextureObj = Context.Services.LoadAsset(HeightTextureAsset, OutError);
			UTexture* Texture = Cast<UTexture>(TextureObj);
			if (!Texture)
			{
				OutError = FString::Printf(TEXT("height_texture_asset '%s' did not resolve to a UTexture."), *HeightTextureAsset);
				return false;
			}
			SetEnumProperty(Component, TEXT("HeightSourceMode"), TEXT("TextureAsset"));
			InvokeSetTextureFunction(Component, TEXT("SetHeightTextureAsset"), Texture);
		}

		if (!ConfigurePatchComponentBinding(Component, Landscape, LayerName, bPatchEnabled, bRequestUpdate, OutError))
		{
			return false;
		}
		if (!RegisterPatchComponent(Component, OutError))
		{
			return false;
		}
		Actor->MarkPackageDirty();

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("create"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("mutating"));
		OutStructured->SetObjectField(TEXT("actor"), FSololmcpEditorServices::MakeActorReference(Actor));
		OutStructured->SetObjectField(TEXT("landscape"), FSololmcpEditorServices::MakeActorReference(Landscape));
		OutStructured->SetStringField(TEXT("component_path"), Component->GetPathName());
		OutStructured->SetStringField(TEXT("layer_name"), LayerName);
		OutStructured->SetNumberField(TEXT("coverage_x"), CoverageX);
		OutStructured->SetNumberField(TEXT("coverage_y"), CoverageY);
		OutStructured->SetNumberField(TEXT("resolution_x"), ResolutionX);
		OutStructured->SetNumberField(TEXT("resolution_y"), ResolutionY);
		OutStructured->SetStringField(TEXT("height_texture_asset"), HeightTextureAsset);
		OutStructured->SetBoolField(TEXT("enabled"), bPatchEnabled);
		OutStructured->SetBoolField(TEXT("request_update"), bRequestUpdate);
		OutStructured->SetStringField(TEXT("update_policy"), bRequestUpdate ? TEXT("explicit_update_requested") : TEXT("safe_no_update_default"));
		OutStructured->SetStringField(TEXT("next_recommended_tool"), TEXT("landscape_patch_stack_inspect"));
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		OutSummary = FString::Printf(TEXT("Created Landscape Texture Patch on layer '%s'."), *LayerName);
		return true;
	}

	static bool Tool_LandscapePatchStackInspect(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString LandscapeId;
		Arguments->TryGetStringField(TEXT("landscape"), LandscapeId);
		ALandscape* FilterLandscape = nullptr;
		if (!LandscapeId.IsEmpty())
		{
			FilterLandscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
			if (!FilterLandscape)
			{
				return false;
			}
		}

		// audit-U7 fix (P2): scope the component scan to the editor world's
		// actors instead of iterating EVERY UActorComponent across ALL worlds
		// (PIE + Editor + inactive). On multi-million-component scenes the
		// old `for (TObjectIterator<UActorComponent> It; It; ++It)` pass
		// dominated the tool budget. Walking the active editor world's actors
		// keeps work proportional to the user-visible scene.
		TArray<TSharedPtr<FJsonValue>> ComponentsJson;
		UWorld* IterWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		TArray<UActorComponent*> CandidateComponents;
		if (IterWorld)
		{
			TArray<UActorComponent*> ActorComps;
			for (TActorIterator<AActor> ActorIt(IterWorld); ActorIt; ++ActorIt)
			{
				if (AActor* Actor = *ActorIt)
				{
					ActorComps.Reset();
					Actor->GetComponents(ActorComps, /*bIncludeFromChildActors=*/false);
					CandidateComponents.Append(ActorComps);
				}
			}
		}
		for (UActorComponent* Component : CandidateComponents)
		{
			if (!Component || !Component->GetWorld() || !Component->GetClass()->GetPathName().Contains(TEXT("/Script/LandscapePatch.")))
			{
				continue;
			}

			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("component_path"), Component->GetPathName());
			Item->SetStringField(TEXT("component_class"), Component->GetClass()->GetPathName());
			if (AActor* Owner = Component->GetOwner())
			{
				Item->SetObjectField(TEXT("owner"), FSololmcpEditorServices::MakeActorReference(Owner));
			}

			UObject* LandscapeObject = nullptr;
			if (FObjectPropertyBase* LandscapeProp = CastField<FObjectPropertyBase>(Component->GetClass()->FindPropertyByName(TEXT("Landscape"))))
			{
				LandscapeObject = LandscapeProp->GetObjectPropertyValue_InContainer(Component);
			}
			ALandscape* ComponentLandscape = Cast<ALandscape>(LandscapeObject);
			if (FilterLandscape && ComponentLandscape != FilterLandscape)
			{
				continue;
			}
			if (ComponentLandscape)
			{
				Item->SetObjectField(TEXT("landscape"), FSololmcpEditorServices::MakeActorReference(ComponentLandscape));
			}

			if (FNameProperty* LayerNameProp = CastField<FNameProperty>(Component->GetClass()->FindPropertyByName(TEXT("DetailPanelLayerName"))))
			{
				Item->SetStringField(TEXT("detail_panel_layer_name"), LayerNameProp->GetPropertyValue_InContainer(Component).ToString());
			}
			if (FStructProperty* GuidProp = CastField<FStructProperty>(Component->GetClass()->FindPropertyByName(TEXT("EditLayerGuid"))))
			{
				if (GuidProp->Struct && GuidProp->Struct->GetName() == TEXT("Guid"))
				{
					const FGuid* Guid = GuidProp->ContainerPtrToValuePtr<FGuid>(Component);
					Item->SetStringField(TEXT("edit_layer_guid"), Guid ? Guid->ToString() : FString());
				}
			}
			if (FDoubleProperty* PriorityProp = CastField<FDoubleProperty>(Component->GetClass()->FindPropertyByName(TEXT("Priority"))))
			{
				Item->SetNumberField(TEXT("priority"), PriorityProp->GetPropertyValue_InContainer(Component));
			}
			ComponentsJson.Add(MakeShared<FJsonValueObject>(Item));
		}

		OutStructured->SetBoolField(TEXT("success"), true);
		OutStructured->SetStringField(TEXT("operation_class"), TEXT("query"));
		OutStructured->SetStringField(TEXT("safety_class"), TEXT("read_only"));
		OutStructured->SetStringField(TEXT("landscape_filter"), LandscapeId);
		OutStructured->SetNumberField(TEXT("patch_component_count"), ComponentsJson.Num());
		OutStructured->SetArrayField(TEXT("patch_components"), ComponentsJson);
		OutStructured->SetBoolField(TEXT("mcp_client_toolset_connected"), false);
		OutStructured->SetStringField(TEXT("mcp_client_toolset_policy"), TEXT("excluded_by_user_request"));
		OutSummary = FString::Printf(TEXT("Found %d LandscapePatch component(s)."), ComponentsJson.Num());
		return true;
	}

	static void RegisterPlanSpec(FSololmcpToolRegistry& Registry, const FLandscapePatchPcgInteropSpec& Spec)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Spec.ToolName;
		Def.Description = Spec.Description;
		Def.InputSchema = PlanInputSchema();
		Def.CacheTtlSeconds = 30;
		Def.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return RunInteropPlan(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Def);
	}

	static TArray<FLandscapePatchPcgInteropSpec> PlanSpecs()
	{
		return {
			{
				TEXT("landscape_patch_edit_layer_plan"),
				TEXT("Read-only plan for the SOMOLMCP-native LandscapePatch edit-layer workflow that replaces deprecated PatchManager routes on UE 5.7+."),
				TEXT("landscape_patch"),
				TEXT("plan"),
				{TEXT("LandscapePatch")},
				{TEXT("LandscapePatch")},
				{TEXT("Editor/LandscapePatch/Source")},
				{TEXT("ULandscapePatchEditLayer"), TEXT("AssignToLandscape"), TEXT("Patch manager is deprecated")},
				{},
				{TEXT("Create or reuse a ULandscapePatchEditLayer via landscape_patch_edit_layer_create."), TEXT("Create patch components with the Circle or Texture patch tools."), TEXT("Inspect stack priority and binding readback."), TEXT("Capture screenshot/height readback before delivery.")},
				{TEXT("target_landscape"), TEXT("patch_edit_layer"), TEXT("component_binding"), TEXT("stack_readback"), TEXT("screenshot_or_height_readback")},
				{TEXT("landscape_patch_edit_layer_create"), TEXT("landscape_patch_stack_inspect"), TEXT("landscape_list_layers")},
				{TEXT("landscape_create_layer"), TEXT("landscape_apply_heightmap_patch")}
			},
			{
				TEXT("landscape_texture_patch_plan"),
				TEXT("Read-only plan for a LandscapeTexturePatch component for non-destructive height/weight/visibility patching on UE 5.7+."),
				TEXT("landscape_patch"),
				TEXT("plan"),
				{TEXT("LandscapePatch")},
				{TEXT("LandscapePatch")},
				{TEXT("Editor/LandscapePatch/Source")},
				{TEXT("ULandscapeTexturePatch"), TEXT("SetUnscaledCoverage"), TEXT("SetResolution"), TEXT("SetHeightTextureAsset"), TEXT("CreateWeightPatch")},
				{},
				{TEXT("Resolve landscape and patch edit layer."), TEXT("Create texture patch component with coverage/resolution/falloff."), TEXT("Bind height texture or internal render target mode."), TEXT("Request landscape update and inspect stack.")},
				{TEXT("component_path"), TEXT("coverage"), TEXT("resolution"), TEXT("height_or_weight_source"), TEXT("stack_readback"), TEXT("screenshot")},
				{TEXT("landscape_texture_patch_create"), TEXT("landscape_patch_stack_inspect"), TEXT("landscape_get_height_region")},
				{TEXT("landscape_apply_heightmap_patch"), TEXT("landscape_set_weight_region")}
			},
			{
				TEXT("landscape_circle_height_patch_plan"),
				TEXT("Read-only plan for a LandscapeCircleHeightPatch for circular flatten/raise/lake/visibility masks without using deprecated PatchManager."),
				TEXT("landscape_patch"),
				TEXT("plan"),
				{TEXT("LandscapePatch")},
				{TEXT("LandscapePatch")},
				{TEXT("Editor/LandscapePatch/Source")},
				{TEXT("ULandscapeCircleHeightPatch"), TEXT("Radius"), TEXT("Falloff"), TEXT("bEditVisibility"), TEXT("bExclusiveRadius")},
				{},
				{TEXT("Resolve target landscape and patch edit layer."), TEXT("Create circle patch at requested world transform."), TEXT("Set radius/falloff/visibility mode."), TEXT("Request update and read back patch stack.")},
				{TEXT("component_path"), TEXT("radius"), TEXT("falloff"), TEXT("visibility_mode"), TEXT("stack_readback"), TEXT("screenshot")},
				{TEXT("landscape_circle_patch_create"), TEXT("landscape_patch_stack_inspect"), TEXT("landscape_get_height_region")},
				{TEXT("landscape_flatten_brush"), TEXT("landscape_lake_basin_world")}
			},
			{
				TEXT("landscape_patch_workflow_coverage_report"),
				TEXT("Report SOMOLMCP coverage for LandscapePatch versus existing direct heightmap/weightmap Landscape tools."),
				TEXT("landscape_patch"),
				TEXT("coverage"),
				{TEXT("LandscapePatch")},
				{TEXT("LandscapePatch"), TEXT("LandscapePatchEditorOnly")},
				{TEXT("Editor/LandscapePatch/Source")},
				{TEXT("ULandscapePatchEditLayer"), TEXT("ULandscapeTexturePatch"), TEXT("ULandscapeCircleHeightPatch"), TEXT("ADEPRECATED_LandscapePatchManager")},
				{},
				{TEXT("Baseline Landscape create/import/sculpt tools remain primary for deterministic terrain."), TEXT("LandscapePatch tools add non-destructive edit-layer patching."), TEXT("PatchManager is treated as deprecated from 5.7 onward."), TEXT("MCPClientToolset is not connected.")},
				{TEXT("coverage_status"), TEXT("version_boundary"), TEXT("tool_mapping"), TEXT("missing_live_smoke")},
				{TEXT("terrain_landscape_create_from_spec"), TEXT("landscape_patch_edit_layer_create"), TEXT("landscape_circle_patch_create"), TEXT("landscape_texture_patch_create")},
				{TEXT("landscape_apply_heightmap_patch"), TEXT("landscape_set_height_region"), TEXT("landscape_paint_layer")}
			},
			{
				TEXT("pcg_interop_capability_probe"),
				TEXT("Probe UE 5.7+ PCG interop plugins and expose the SOMOLMCP graph-node routes that cover them."),
				TEXT("pcg_interop"),
				TEXT("probe"),
				{TEXT("PCGGeometryScriptInterop"), TEXT("PCGExternalDataInterop"), TEXT("PCGPythonInterop"), TEXT("PCGWaterInterop"), TEXT("PCGNiagaraInterop"), TEXT("PCGInstancedActorsInterop"), TEXT("PCGNaniteAssembliesInterop"), TEXT("PCGFastGeoInterop"), TEXT("PCGBiomeCore")},
				{TEXT("PCG")},
				{TEXT("PCGInterops"), TEXT("Experimental/PCGInterops"), TEXT("Experimental/PCGBiomeCore/Source")},
				{TEXT("UPCG"), TEXT("Settings"), TEXT("PCG_Overridable"), TEXT("GetDefaultNodeTitle")},
				{},
				{TEXT("Probe all PCG interop plugin descriptors."), TEXT("Use pcg_node_catalog_lookup to confirm live UPCGSettings classes."), TEXT("Author nodes through pcg_graph_add_node, pcg_graph_set_node_property, pcg_graph_connect."), TEXT("Validate/dry-run before generate.")},
				{TEXT("plugin_probe"), TEXT("node_catalog_lookup"), TEXT("graph_validate"), TEXT("dry_run_receipt")},
				{TEXT("pcg_node_catalog_lookup"), TEXT("pcg_graph_add_node"), TEXT("pcg_graph_validate"), TEXT("pcg_dry_run_calibration_receipt")},
				{TEXT("pcg_graph_template_apply")},
				5,
				7,
				false
			},
			{
				TEXT("pcg_geometry_script_interop_plan"),
				TEXT("Read-only plan for PCG GeometryScript dynamic-mesh graph nodes through SOMOLMCP generic PCG graph authoring."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGGeometryScriptInterop")},
				{TEXT("PCGGeometryScriptInterop")},
				{TEXT("PCGInterops/PCGGeometryScriptInterop/Source")},
				{TEXT("UPCGCreateEmptyDynamicMeshSettings"), TEXT("UPCGMeshToDynamicMeshSettings"), TEXT("UPCGSpawnDynamicMeshSettings"), TEXT("UPCGSaveDynamicMeshToAssetSettings")},
				{TEXT("/Script/PCGGeometryScriptInterop.PCGCreateEmptyDynamicMeshSettings"), TEXT("/Script/PCGGeometryScriptInterop.PCGMeshToDynamicMeshSettings"), TEXT("/Script/PCGGeometryScriptInterop.PCGMergeDynamicMeshesSettings"), TEXT("/Script/PCGGeometryScriptInterop.PCGSpawnDynamicMeshSettings"), TEXT("/Script/PCGGeometryScriptInterop.PCGSaveDynamicMeshToAssetSettings")},
				{TEXT("Lookup dynamic mesh nodes."), TEXT("Create/merge/transform/save mesh path."), TEXT("Validate pins and dry-run budget."), TEXT("Save generated mesh asset and inspect dependency graph.")},
				{TEXT("node_class_paths"), TEXT("pin_compat"), TEXT("validate_receipt"), TEXT("generated_mesh_asset")},
				{TEXT("pcg_node_catalog_lookup"), TEXT("pcg_graph_add_node"), TEXT("pcg_graph_connect"), TEXT("pcg_graph_validate")},
				{TEXT("staticmesh_generate_lods"), TEXT("asset_dependency_graph")}
			},
			{
				TEXT("pcg_external_data_alembic_plan"),
				TEXT("Read-only plan for PCG ExternalData Alembic ingestion and attribute mapping without relying on MCPClientToolset."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGExternalDataInterop")},
				{TEXT("PCGExternalDataInterop"), TEXT("PCGExternalDataInteropEditor")},
				{TEXT("PCGInterops/PCGExternalDataInterop/Source")},
				{TEXT("UPCGLoadAlembicSettings"), TEXT("UPCGLoadAlembicFunctionLibrary"), TEXT("ExportAlembicFileToPCG")},
				{TEXT("/Script/PCGExternalDataInterop.PCGLoadAlembicSettings")},
				{TEXT("Resolve Alembic source file."), TEXT("Create/load PCG data asset."), TEXT("Map attributes to PCG selectors."), TEXT("Validate graph and artifact registry receipt.")},
				{TEXT("source_file"), TEXT("attribute_mapping"), TEXT("pcg_data_asset"), TEXT("validation_receipt")},
				{TEXT("asset_ingest_from_disk"), TEXT("pcg_node_catalog_lookup"), TEXT("pcg_graph_add_node")},
				{TEXT("datasmith_import_plan"), TEXT("usd_stage_import_plan")}
			},
			{
				TEXT("pcg_python_script_node_plan"),
				TEXT("Read-only plan for PCGPythonInterop Execute Python Script nodes with strict unattended safety and validation gates."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGPythonInterop")},
				{TEXT("PCGPythonInteropEditor")},
				{TEXT("PCGInterops/PCGPythonInterop/Source")},
				{TEXT("UPCGExecutePythonScriptSettings"), TEXT("EPCGPythonScriptInputMethod")},
				{TEXT("/Script/PCGPythonInteropEditor.PCGExecutePythonScriptSettings")},
				{TEXT("Validate Python plugin availability."), TEXT("Insert Execute Python Script node with source constraints."), TEXT("Require schema repair and dry-run before generate."), TEXT("Block destructive filesystem or editor actions in unattended queues.")},
				{TEXT("python_source"), TEXT("safety_review"), TEXT("pin_validation"), TEXT("dry_run_receipt")},
				{TEXT("pcg_graph_add_node"), TEXT("tool_schema_repair_candidates"), TEXT("python_exec")},
				{TEXT("unreal_call")}
			},
			{
				TEXT("pcg_water_spline_interop_plan"),
				TEXT("Read-only plan for PCGWaterInterop water-spline reads for rivers, lakes, shoreline vegetation, and path masks."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGWaterInterop"), TEXT("Water")},
				{TEXT("PCGWaterInterop")},
				{TEXT("Experimental/PCGInterops/PCGWaterInterop/Source")},
				{TEXT("UPCGGetWaterSplineSettings"), TEXT("UPCGWaterSplineData")},
				{TEXT("/Script/PCGWaterInterop.PCGGetWaterSplineSettings")},
				{TEXT("Resolve WaterBody actors."), TEXT("Add Get Water Spline node."), TEXT("Route spline data to sampler/filter/scatter nodes."), TEXT("Validate shoreline density and screenshot receipt.")},
				{TEXT("water_actor"), TEXT("spline_node"), TEXT("graph_validate"), TEXT("preview_receipt")},
				{TEXT("pcg_graph_add_node"), TEXT("pcg_graph_connect"), TEXT("landscape_lake_basin_world")},
				{TEXT("landscape_spline_create")}
			},
			{
				TEXT("pcg_niagara_data_channel_interop_plan"),
				TEXT("Read-only plan for PCGNiagaraInterop Write To Niagara Data Channel nodes using SOMOLMCP PCG/Niagara validation tools."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGNiagaraInterop"), TEXT("Niagara")},
				{TEXT("PCGNiagaraInterop")},
				{TEXT("Experimental/PCGInterops/PCGNiagaraInterop/Source")},
				{TEXT("UPCGWriteToNiagaraDataChannelSettings"), TEXT("NiagaraDataChannel")},
				{TEXT("/Script/PCGNiagaraInterop.PCGWriteToNiagaraDataChannelSettings")},
				{TEXT("Validate PCG point attributes."), TEXT("Add Write To Niagara Data Channel node."), TEXT("Map attributes to Niagara channel schema."), TEXT("Compile Niagara and preview VFX receipt.")},
				{TEXT("attribute_map"), TEXT("data_channel"), TEXT("pcg_validate"), TEXT("niagara_compile"), TEXT("preview_receipt")},
				{TEXT("pcg_graph_add_node"), TEXT("pcg_attribute_inspect"), TEXT("niagara_hlsl_validate_custom_node")},
				{TEXT("ue58_pcg_niagara_interop_plan")}
			},
			{
				TEXT("pcg_instanced_actors_interop_plan"),
				TEXT("Read-only plan for PCG InstancedActors spawning for large crowds/biomes with actor index and health receipts."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGInstancedActorsInterop"), TEXT("InstancedActors")},
				{TEXT("PCGInstancedActorsInterop")},
				{TEXT("Experimental/PCGInterops/PCGInstancedActorsInterop/Source")},
				{TEXT("UPCGSpawnInstancedActorsSettings"), TEXT("UPCGInstancedActorsManagedResource")},
				{TEXT("/Script/PCGInstancedActorsInterop.PCGSpawnInstancedActorsSettings")},
				{TEXT("Resolve actor templates/configs."), TEXT("Add Spawn Instanced Actors node."), TEXT("Apply tile cap and seed plan."), TEXT("Index spawned actors and audit health.")},
				{TEXT("actor_template"), TEXT("tile_cap"), TEXT("seed"), TEXT("spawned_actor_index"), TEXT("health_audit")},
				{TEXT("pcg_graph_add_node"), TEXT("pcg_tile_cap_guard"), TEXT("pcg_spawned_actor_index"), TEXT("pcg_generated_actor_health_audit")},
				{TEXT("pcg_graph_add_static_mesh_spawner")}
			},
			{
				TEXT("pcg_nanite_assembly_interop_plan"),
				TEXT("Read-only plan for PCG Nanite Assemblies builder nodes for generated modular structures and large scene sets."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGNaniteAssembliesInterop")},
				{TEXT("PCGNaniteAssembliesInterop")},
				{TEXT("Experimental/PCGInterops/PCGNaniteAssembliesInterop/Source")},
				{TEXT("UPCGNaniteAssemblyStaticMeshBuilderSettings")},
				{TEXT("/Script/PCGNaniteAssembliesInterop.PCGNaniteAssemblyStaticMeshBuilderSettings")},
				{TEXT("Gather modular mesh inputs."), TEXT("Add Nanite assembly builder node."), TEXT("Validate generated mesh asset and Nanite flag."), TEXT("Capture viewport proof.")},
				{TEXT("mesh_inputs"), TEXT("assembly_asset"), TEXT("nanite_enabled"), TEXT("preview_receipt")},
				{TEXT("pcg_graph_add_node"), TEXT("staticmesh_enable_nanite"), TEXT("staticmesh_inspect")},
				{TEXT("pcg_geometry_script_interop_plan")}
			},
			{
				TEXT("pcg_fastgeo_interop_plan"),
				TEXT("Read-only plan for PCG FastGeo container use for high-volume generated geometry with explicit budget gates."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGFastGeoInterop")},
				{TEXT("PCGFastGeoInterop")},
				{TEXT("Experimental/PCGInterops/PCGFastGeoInterop/Source")},
				{TEXT("UPCGManagedFastGeoContainer")},
				{},
				{TEXT("Probe FastGeo support."), TEXT("Prefer PCG graph generation with budget/tile caps."), TEXT("Index generated containers/resources."), TEXT("Require health audit and preview receipt.")},
				{TEXT("budget"), TEXT("tile_cap"), TEXT("resource_index"), TEXT("health_audit")},
				{TEXT("pcg_generation_budget_set"), TEXT("pcg_tile_cap_guard"), TEXT("pcg_generated_dependency_graph")},
				{TEXT("pcg_generated_actor_health_audit")}
			},
			{
				TEXT("pcg_biome_graph_plan"),
				TEXT("Read-only plan for PCGBiomeCore-driven biome graph handoff using SOMOLMCP PCG templates, masks, and dry-run calibration."),
				TEXT("pcg_interop"),
				TEXT("plan"),
				{TEXT("PCGBiomeCore")},
				{TEXT("PCGBiomeCore")},
				{TEXT("Experimental/PCGBiomeCore/Source")},
				{TEXT("PCGBiomeCore")},
				{},
				{TEXT("Probe PCGBiomeCore availability."), TEXT("Translate Scene IR biome layers to PCG regions."), TEXT("Apply foliage/rock/water templates."), TEXT("Run dry-run calibration and generated actor health audit.")},
				{TEXT("biome_layers"), TEXT("pcg_regions"), TEXT("dry_run_receipt"), TEXT("health_audit")},
				{TEXT("pcg_graph_template_apply"), TEXT("terrain_biome_layer_validate_plan"), TEXT("pcg_dry_run_calibration_receipt")},
				{TEXT("foliage_author"), TEXT("terrain_fill_tile_plan_landscape_weights_height_based")}
			},
			{
				TEXT("ue58_pcg_mesh_partition_capability_probe"),
				TEXT("Probe UE 5.8 PCGMeshPartitionInterop availability and list the new Mesh Partition PCG node surface."),
				TEXT("ue58_pcg_mesh_partition"),
				TEXT("probe"),
				{TEXT("PCGMeshPartitionInterop"), TEXT("MeshPartition")},
				{TEXT("PCGMeshPartitionInterop"), TEXT("PCGMeshPartitionInteropEditor")},
				{TEXT("Experimental/PCGMeshPartitionInterop/Source")},
				{TEXT("UPCGQuerySettings"), TEXT("UPCGWriteSettings"), TEXT("UPCGSculptLayerWriteSettings"), TEXT("UPCGProjectionSpawnerSettings"), TEXT("UPCGPatchInstanceSpawnerSettings"), TEXT("UPCGAdapterComponent")},
				{},
				{TEXT("Probe UE 5.8 MeshPartition interop plugin."), TEXT("Inventory Query/Write/Sculpt/Projection/Patch spawner classes."), TEXT("Return fail-closed on UE 5.7."), TEXT("Use generic PCG graph authoring after node catalog confirms classes.")},
				{TEXT("engine_version"), TEXT("plugin_probe"), TEXT("source_inventory"), TEXT("ue58_only_flag")},
				{TEXT("pcg_node_catalog_lookup"), TEXT("pcg_graph_add_node"), TEXT("pcg_graph_validate")},
				{TEXT("pcg_geometry_script_interop_plan")},
				5,
				8
			},
			{
				TEXT("ue58_pcg_mesh_partition_graph_plan"),
				TEXT("Read-only plan for a UE 5.8 Mesh Partition PCG graph using query/write/sculpt/projection/spawner nodes."),
				TEXT("ue58_pcg_mesh_partition"),
				TEXT("plan"),
				{TEXT("PCGMeshPartitionInterop"), TEXT("MeshPartition")},
				{TEXT("PCGMeshPartitionInterop"), TEXT("PCGMeshPartitionInteropEditor")},
				{TEXT("Experimental/PCGMeshPartitionInterop/Source")},
				{TEXT("Mega Mesh Query"), TEXT("MegaMesh Write"), TEXT("MegaMesh Sculpt Layer Write"), TEXT("Mega Mesh Projection Instance Spawner"), TEXT("Mega Mesh Patch Instance Spawner")},
				{TEXT("/Script/PCGMeshPartitionInterop.PCGQuerySettings"), TEXT("/Script/PCGMeshPartitionInterop.PCGWriteSettings"), TEXT("/Script/PCGMeshPartitionInterop.PCGSculptLayerWriteSettings"), TEXT("/Script/PCGMeshPartitionInterop.PCGProjectionSpawnerSettings"), TEXT("/Script/PCGMeshPartitionInterop.PCGPatchInstanceSpawnerSettings")},
				{TEXT("Resolve MeshPartition/MegaMesh target."), TEXT("Add Query node for sampled mesh data."), TEXT("Add Write or Sculpt Layer Write node for modifier output."), TEXT("Add Projection/Patch spawner nodes as needed."), TEXT("Validate pins, dry-run, and capture preview receipt.")},
				{TEXT("mesh_partition_actor"), TEXT("node_class_paths"), TEXT("pin_validation"), TEXT("dry_run_receipt"), TEXT("preview_receipt")},
				{TEXT("ue58_pcg_mesh_partition_capability_probe"), TEXT("pcg_node_catalog_lookup"), TEXT("pcg_graph_add_node"), TEXT("pcg_graph_validate")},
				{TEXT("pcg_geometry_script_interop_plan")},
				5,
				8
			},
			{
				TEXT("ue58_pcg_mesh_partition_query_plan"),
				TEXT("Read-only plan for the UE 5.8 Mesh Partition PCG Query node route for sampling MegaMesh/MeshPartition data."),
				TEXT("ue58_pcg_mesh_partition"),
				TEXT("plan"),
				{TEXT("PCGMeshPartitionInterop"), TEXT("MeshPartition")},
				{TEXT("PCGMeshPartitionInterop")},
				{TEXT("Experimental/PCGMeshPartitionInterop/Source")},
				{TEXT("UPCGQuerySettings"), TEXT("Mega Mesh Query")},
				{TEXT("/Script/PCGMeshPartitionInterop.PCGQuerySettings")},
				{TEXT("Resolve target MeshPartition actor."), TEXT("Add Query node."), TEXT("Connect output to sampler/filter/spawner path."), TEXT("Validate spatial pins.")},
				{TEXT("mesh_partition_actor"), TEXT("query_node"), TEXT("spatial_output"), TEXT("validate_receipt")},
				{TEXT("pcg_node_catalog_lookup"), TEXT("pcg_graph_add_node"), TEXT("pcg_pin_compat_validate")},
				{TEXT("pcg_geometry_script_interop_plan")},
				5,
				8
			},
			{
				TEXT("ue58_pcg_mesh_partition_write_plan"),
				TEXT("Read-only plan for the UE 5.8 Mesh Partition PCG Write node route for writing positions/channels back into MegaMesh modifiers."),
				TEXT("ue58_pcg_mesh_partition"),
				TEXT("plan"),
				{TEXT("PCGMeshPartitionInterop"), TEXT("MeshPartition")},
				{TEXT("PCGMeshPartitionInterop")},
				{TEXT("Experimental/PCGMeshPartitionInterop/Source")},
				{TEXT("UPCGWriteSettings"), TEXT("MegaMesh Write"), TEXT("AffectedMegaMesh"), TEXT("Channels")},
				{TEXT("/Script/PCGMeshPartitionInterop.PCGWriteSettings")},
				{TEXT("Resolve affected MegaMesh."), TEXT("Add Write node."), TEXT("Map source/destination position attributes."), TEXT("Constrain to bounds and validate.")},
				{TEXT("affected_megamesh"), TEXT("channels"), TEXT("attribute_map"), TEXT("validate_receipt")},
				{TEXT("pcg_graph_add_node"), TEXT("pcg_graph_set_node_property"), TEXT("pcg_graph_validate")},
				{TEXT("pcg_geometry_script_interop_plan")},
				5,
				8
			},
			{
				TEXT("ue58_pcg_mesh_partition_sculpt_layer_plan"),
				TEXT("Read-only plan for the UE 5.8 Mesh Partition PCG Sculpt Layer Write node route for layer-based terrain/mesh deformation."),
				TEXT("ue58_pcg_mesh_partition"),
				TEXT("plan"),
				{TEXT("PCGMeshPartitionInterop"), TEXT("MeshPartition")},
				{TEXT("PCGMeshPartitionInterop")},
				{TEXT("Experimental/PCGMeshPartitionInterop/Source")},
				{TEXT("UPCGSculptLayerWriteSettings"), TEXT("MegaMesh Sculpt Layer Write"), TEXT("UProjectMeshLayersModifier")},
				{TEXT("/Script/PCGMeshPartitionInterop.PCGSculptLayerWriteSettings")},
				{TEXT("Resolve sculpt layer target."), TEXT("Add Sculpt Layer Write node."), TEXT("Map attributes/channels and priority."), TEXT("Validate and capture deformation proof.")},
				{TEXT("sculpt_layer"), TEXT("channels"), TEXT("priority"), TEXT("preview_receipt")},
				{TEXT("pcg_graph_add_node"), TEXT("pcg_graph_set_node_property"), TEXT("pcg_graph_validate")},
				{TEXT("landscape_patch_edit_layer_plan")},
				5,
				8
			},
			{
				TEXT("ue58_pcg_mesh_partition_adapter_plan"),
				TEXT("Read-only plan for the UE 5.8 Mesh Partition PCG Adapter component route that publishes built MeshPartition data for PCG sampling."),
				TEXT("ue58_pcg_mesh_partition"),
				TEXT("plan"),
				{TEXT("PCGMeshPartitionInterop"), TEXT("MeshPartition")},
				{TEXT("PCGMeshPartitionInteropEditor")},
				{TEXT("Experimental/PCGMeshPartitionInterop/Source")},
				{TEXT("UPCGAdapterComponent"), TEXT("UPCGDataComponent"), TEXT("PostBuildSectionMesh")},
				{},
				{TEXT("Attach Mesh Partition PCG Adapter component to modifier stack."), TEXT("Trigger/build MeshPartition sections."), TEXT("Confirm PCGDataComponent on compiled sections."), TEXT("Run Query node readback.")},
				{TEXT("adapter_component"), TEXT("compiled_section"), TEXT("pcg_data_component"), TEXT("query_readback")},
				{TEXT("plugin_inspect"), TEXT("ue58_pcg_mesh_partition_query_plan"), TEXT("pcg_node_catalog_lookup")},
				{TEXT("pcg_geometry_script_interop_plan")},
				5,
				8
			},
			{
				TEXT("ue58_pcg_interops_coverage_report"),
				TEXT("Report PCG interop coverage split into UE 5.7 baseline interops and UE 5.8-only MeshPartition interop."),
				TEXT("pcg_interop"),
				TEXT("coverage"),
				{TEXT("PCGGeometryScriptInterop"), TEXT("PCGExternalDataInterop"), TEXT("PCGPythonInterop"), TEXT("PCGWaterInterop"), TEXT("PCGNiagaraInterop"), TEXT("PCGInstancedActorsInterop"), TEXT("PCGNaniteAssembliesInterop"), TEXT("PCGFastGeoInterop"), TEXT("PCGBiomeCore"), TEXT("PCGMeshPartitionInterop")},
				{TEXT("PCG")},
				{TEXT("PCGInterops"), TEXT("Experimental/PCGInterops"), TEXT("Experimental/PCGBiomeCore/Source"), TEXT("Experimental/PCGMeshPartitionInterop/Source")},
				{TEXT("UPCG"), TEXT("Settings"), TEXT("PCGMeshPartition"), TEXT("Mesh Partition")},
				{},
				{TEXT("UE 5.7 baseline interops are covered by generic graph authoring plus the new plan/probe tools."), TEXT("UE 5.8-only MeshPartition is guarded by engine version and plugin/module probes."), TEXT("Mutating MeshPartition graph application is exposed through ue58_pcg_mesh_partition_graph_apply; generated-section live receipt remains the promotion gate."), TEXT("MCPClientToolset remains excluded.")},
				{TEXT("baseline_57_tools"), TEXT("ue58_only_tools"), TEXT("coverage_status"), TEXT("pending_live_smoke")},
				{TEXT("pcg_interop_capability_probe"), TEXT("ue58_pcg_mesh_partition_capability_probe"), TEXT("ue58_pcg_mesh_partition_graph_apply"), TEXT("pcg_node_catalog_lookup"), TEXT("pcg_graph_add_node")},
				{TEXT("pcg_graph_template_apply")},
				5,
				7,
				false
			}
		};
	}

	static TSharedRef<FJsonObject> VectorJson(const FVector& Value)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedRef<FJsonObject> BoxJson(const FBox& Box)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Box.IsValid != 0);
		Obj->SetObjectField(TEXT("min"), VectorJson(Box.IsValid ? Box.Min : FVector::ZeroVector));
		Obj->SetObjectField(TEXT("max"), VectorJson(Box.IsValid ? Box.Max : FVector::ZeroVector));
		Obj->SetObjectField(TEXT("center"), VectorJson(Box.IsValid ? Box.GetCenter() : FVector::ZeroVector));
		Obj->SetObjectField(TEXT("extent"), VectorJson(Box.IsValid ? Box.GetExtent() : FVector::ZeroVector));
		Obj->SetObjectField(TEXT("size"), VectorJson(Box.IsValid ? Box.GetSize() : FVector::ZeroVector));
		return Obj;
	}

	static TSharedRef<FJsonObject> RotatorJson(const FRotator& Value)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static FString WaterClassPathFromType(const FString& InType)
	{
		FString Type = InType;
		Type.TrimStartAndEndInline();
		if (Type.StartsWith(TEXT("/Script/")))
		{
			return Type;
		}
		if (Type.StartsWith(TEXT("A")) && Type.Contains(TEXT("WaterBody")))
		{
			Type.RightChopInline(1);
		}
		const FString Lower = Type.ToLower();
		if (Lower == TEXT("river") || Lower == TEXT("waterbodyriver"))
		{
			return TEXT("/Script/Water.WaterBodyRiver");
		}
		if (Lower == TEXT("lake") || Lower == TEXT("waterbodylake"))
		{
			return TEXT("/Script/Water.WaterBodyLake");
		}
		if (Lower == TEXT("ocean") || Lower == TEXT("waterbodyocean"))
		{
			return TEXT("/Script/Water.WaterBodyOcean");
		}
		if (Lower == TEXT("custom") || Lower == TEXT("waterbodycustom"))
		{
			return TEXT("/Script/Water.WaterBodyCustom");
		}
		if (Lower == TEXT("zone") || Lower == TEXT("waterzone"))
		{
			return TEXT("/Script/Water.WaterZone");
		}
		return FString::Printf(TEXT("/Script/Water.%s"), *Type);
	}

	static UClass* ResolveWaterActorClass(const FSololmcpEditorServices& Services, const FString& TypeOrPath, FString& OutClassPath, FString& OutError)
	{
		OutClassPath = WaterClassPathFromType(TypeOrPath);
		UClass* Class = Services.ResolveClass(OutClassPath, OutError);
		if (!Class)
		{
			return nullptr;
		}
		if (!Class->IsChildOf(AActor::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Water class '%s' is not an Actor class."), *OutClassPath);
			return nullptr;
		}
		return Class;
	}

	static bool IsWaterActor(const AActor* Actor)
	{
		if (!Actor)
		{
			return false;
		}
		const FString ClassName = Actor->GetClass()->GetName();
		const FString ClassPath = Actor->GetClass()->GetPathName();
		return ClassName.Contains(TEXT("WaterBody")) || ClassName.Contains(TEXT("WaterZone")) || ClassPath.Contains(TEXT("/Script/Water."));
	}

	static bool IsWaterZoneActor(const AActor* Actor)
	{
		if (!Actor)
		{
			return false;
		}
		const FString ClassName = Actor->GetClass()->GetName();
		const FString ClassPath = Actor->GetClass()->GetPathName();
		return ClassName.Contains(TEXT("WaterZone")) || ClassPath.Contains(TEXT("WaterZone"));
	}

	static bool IsWaterBrushManagerRiskClass(const UClass* Class, const FString& ResolvedClassPath, const FString& WaterType)
	{
		if (!Class)
		{
			return false;
		}
		const FString ClassName = Class->GetName();
		const FString ClassPath = ResolvedClassPath.IsEmpty() ? Class->GetPathName() : ResolvedClassPath;
		const bool bIsWaterZone = ClassName.Contains(TEXT("WaterZone")) || ClassPath.Contains(TEXT("WaterZone"));
		if (bIsWaterZone)
		{
			return false;
		}
		const FString TypeLower = WaterType.ToLower();
		return ClassName.Contains(TEXT("WaterBody"))
			|| ClassPath.Contains(TEXT("WaterBody"))
			|| TypeLower.Contains(TEXT("lake"))
			|| TypeLower.Contains(TEXT("river"))
			|| TypeLower.Contains(TEXT("ocean"))
			|| TypeLower.Contains(TEXT("custom"));
	}

	static TArray<TSharedPtr<FJsonValue>> WaterBodySafeAlternativesJson()
	{
		TArray<TSharedPtr<FJsonValue>> Alternatives;
		Alternatives.Add(MakeShared<FJsonValueString>(TEXT("Call water_body_create_v2 with execute=false for class and transform validation.")));
		Alternatives.Add(MakeShared<FJsonValueString>(TEXT("Use water_zone_create_v2 when only a WaterZone actor is needed.")));
		Alternatives.Add(MakeShared<FJsonValueString>(TEXT("Run actual WaterBody spawn only in a dedicated write lane with allow_water_brush_manager=true and watchdog coverage.")));
		return Alternatives;
	}

	static TSharedRef<FJsonObject> ComponentBriefJson(const UActorComponent* Component)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Component ? Component->GetName() : FString());
		Obj->SetStringField(TEXT("class"), Component ? Component->GetClass()->GetPathName() : FString());
		Obj->SetBoolField(TEXT("active"), Component && Component->IsActive());
		Obj->SetBoolField(TEXT("canEverAffectNavigation"), Component && Component->CanEverAffectNavigation());
		if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
		{
			Obj->SetObjectField(TEXT("relativeLocation"), VectorJson(Scene->GetRelativeLocation()));
			Obj->SetObjectField(TEXT("relativeRotation"), RotatorJson(Scene->GetRelativeRotation()));
			Obj->SetObjectField(TEXT("relativeScale"), VectorJson(Scene->GetRelativeScale3D()));
		}
		if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			Obj->SetStringField(TEXT("collisionEnabled"), StaticEnum<ECollisionEnabled::Type>()->GetNameStringByValue(static_cast<int64>(Primitive->GetCollisionEnabled())));
			Obj->SetStringField(TEXT("collisionProfile"), Primitive->GetCollisionProfileName().ToString());
			Obj->SetBoolField(TEXT("generateOverlapEvents"), Primitive->GetGenerateOverlapEvents());
		}
		return Obj;
	}

	static TSharedRef<FJsonObject> WaterActorJson(const AActor* Actor, const bool bIncludeComponents)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Actor)
		{
			Obj->SetBoolField(TEXT("valid"), false);
			return Obj;
		}
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetStringField(TEXT("name"), Actor->GetName());
		Obj->SetStringField(TEXT("path"), Actor->GetPathName());
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
		Obj->SetObjectField(TEXT("location"), VectorJson(Actor->GetActorLocation()));
		Obj->SetObjectField(TEXT("rotation"), RotatorJson(Actor->GetActorRotation()));
		Obj->SetObjectField(TEXT("scale"), VectorJson(Actor->GetActorScale3D()));
		Obj->SetBoolField(TEXT("isWaterActor"), IsWaterActor(Actor));
		if (bIncludeComponents)
		{
			TArray<TSharedPtr<FJsonValue>> Components;
			TInlineComponentArray<UActorComponent*> ActorComponents;
			Actor->GetComponents(ActorComponents);
			for (const UActorComponent* Component : ActorComponents)
			{
				Components.Add(MakeShared<FJsonValueObject>(ComponentBriefJson(Component)));
			}
			Obj->SetArrayField(TEXT("components"), Components);
			Obj->SetNumberField(TEXT("componentCount"), Components.Num());
		}
		return Obj;
	}

	static bool TryGetActorArgument(const TSharedRef<FJsonObject>& Arguments, FString& OutActorId)
	{
		return Arguments->TryGetStringField(TEXT("actor"), OutActorId)
			|| Arguments->TryGetStringField(TEXT("target_actor"), OutActorId)
			|| Arguments->TryGetStringField(TEXT("actor_label"), OutActorId)
			|| Arguments->TryGetStringField(TEXT("label"), OutActorId);
	}

	static FString SplineCoordinateSpaceName(const ESplineCoordinateSpace::Type Space)
	{
		return Space == ESplineCoordinateSpace::Local ? TEXT("local") : TEXT("world");
	}

	static ESplineCoordinateSpace::Type ReadSplineCoordinateSpace(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Space;
		if (Arguments->TryGetStringField(TEXT("coordinate_space"), Space) || Arguments->TryGetStringField(TEXT("space"), Space))
		{
			return Space.Equals(TEXT("local"), ESearchCase::IgnoreCase) ? ESplineCoordinateSpace::Local : ESplineCoordinateSpace::World;
		}
		return ESplineCoordinateSpace::World;
	}

	static ESplinePointType::Type SplinePointTypeFromString(const FString& RawType)
	{
		if (RawType.Equals(TEXT("linear"), ESearchCase::IgnoreCase))
		{
			return ESplinePointType::Linear;
		}
		if (RawType.Equals(TEXT("constant"), ESearchCase::IgnoreCase))
		{
			return ESplinePointType::Constant;
		}
		if (RawType.Equals(TEXT("curve_clamped"), ESearchCase::IgnoreCase) || RawType.Equals(TEXT("curveclamped"), ESearchCase::IgnoreCase))
		{
			return ESplinePointType::CurveClamped;
		}
		if (RawType.Equals(TEXT("curve_custom_tangent"), ESearchCase::IgnoreCase) || RawType.Equals(TEXT("custom"), ESearchCase::IgnoreCase))
		{
			return ESplinePointType::CurveCustomTangent;
		}
		return ESplinePointType::Curve;
	}

	static FString SplinePointTypeToString(const ESplinePointType::Type Type)
	{
		switch (Type)
		{
		case ESplinePointType::Linear:
			return TEXT("linear");
		case ESplinePointType::Constant:
			return TEXT("constant");
		case ESplinePointType::CurveClamped:
			return TEXT("curve_clamped");
		case ESplinePointType::CurveCustomTangent:
			return TEXT("curve_custom_tangent");
		case ESplinePointType::Curve:
		default:
			return TEXT("curve");
		}
	}

	struct FSololmcpSplinePointSpec
	{
		FVector Location = FVector::ZeroVector;
		FVector ArriveTangent = FVector::ZeroVector;
		FVector LeaveTangent = FVector::ZeroVector;
		ESplinePointType::Type Type = ESplinePointType::Curve;
		bool bHasLocation = false;
		bool bHasArriveTangent = false;
		bool bHasLeaveTangent = false;
		bool bHasType = false;
	};

	static bool TryReadVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FVector& OutVector)
	{
		const TSharedPtr<FJsonObject>* VecObj = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(FieldName, VecObj) && VecObj && VecObj->IsValid())
		{
			return FSololmcpEditorServices::JsonToVector(*VecObj, OutVector);
		}
		return false;
	}

	static bool ReadSplinePointSpec(const TSharedPtr<FJsonObject>& Obj, FSololmcpSplinePointSpec& OutSpec, FString& OutError, const bool bRequireLocation)
	{
		if (!Obj.IsValid())
		{
			OutError = TEXT("Spline point entry must be an object.");
			return false;
		}
		OutSpec.bHasLocation = TryReadVectorField(Obj, TEXT("location"), OutSpec.Location)
			|| TryReadVectorField(Obj, TEXT("point"), OutSpec.Location)
			|| TryReadVectorField(Obj, TEXT("position"), OutSpec.Location);
		if (bRequireLocation && !OutSpec.bHasLocation)
		{
			OutError = TEXT("Spline point requires location {x,y,z}.");
			return false;
		}
		OutSpec.bHasArriveTangent = TryReadVectorField(Obj, TEXT("arrive_tangent"), OutSpec.ArriveTangent);
		OutSpec.bHasLeaveTangent = TryReadVectorField(Obj, TEXT("leave_tangent"), OutSpec.LeaveTangent);
		FString TypeString;
		if (Obj->TryGetStringField(TEXT("type"), TypeString) || Obj->TryGetStringField(TEXT("point_type"), TypeString))
		{
			OutSpec.Type = SplinePointTypeFromString(TypeString);
			OutSpec.bHasType = true;
		}
		return true;
	}

	static TSharedRef<FJsonObject> SplinePointJson(const USplineComponent* Spline, const int32 Index, const ESplineCoordinateSpace::Type Space)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Spline || Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
		{
			Obj->SetBoolField(TEXT("valid"), false);
			Obj->SetNumberField(TEXT("index"), Index);
			return Obj;
		}
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetNumberField(TEXT("index"), Index);
		Obj->SetObjectField(TEXT("location"), VectorJson(Spline->GetLocationAtSplinePoint(Index, Space)));
		Obj->SetObjectField(TEXT("arrive_tangent"), VectorJson(Spline->GetArriveTangentAtSplinePoint(Index, Space)));
		Obj->SetObjectField(TEXT("leave_tangent"), VectorJson(Spline->GetLeaveTangentAtSplinePoint(Index, Space)));
		Obj->SetObjectField(TEXT("rotation"), RotatorJson(Spline->GetRotationAtSplinePoint(Index, Space)));
		Obj->SetObjectField(TEXT("scale"), VectorJson(Spline->GetScaleAtSplinePoint(Index)));
		Obj->SetStringField(TEXT("type"), SplinePointTypeToString(Spline->GetSplinePointType(Index)));
		return Obj;
	}

	static TSharedRef<FJsonObject> SplineComponentJson(const USplineComponent* Spline, const ESplineCoordinateSpace::Type Space)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Spline)
		{
			Obj->SetBoolField(TEXT("valid"), false);
			return Obj;
		}
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetStringField(TEXT("name"), Spline->GetName());
		Obj->SetStringField(TEXT("path"), Spline->GetPathName());
		Obj->SetStringField(TEXT("class"), Spline->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("coordinateSpace"), SplineCoordinateSpaceName(Space));
		Obj->SetNumberField(TEXT("pointCount"), Spline->GetNumberOfSplinePoints());
		Obj->SetNumberField(TEXT("splineLength"), Spline->GetSplineLength());
		Obj->SetBoolField(TEXT("closedLoop"), Spline->IsClosedLoop());
		TArray<TSharedPtr<FJsonValue>> Points;
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
		{
			Points.Add(MakeShared<FJsonValueObject>(SplinePointJson(Spline, Index, Space)));
		}
		Obj->SetArrayField(TEXT("points"), Points);
		return Obj;
	}

	static USplineComponent* FindBestSplineComponent(AActor* Actor, FString& OutError, const FString& ComponentName = FString())
	{
		if (!Actor)
		{
			OutError = TEXT("Actor is null.");
			return nullptr;
		}
		TArray<USplineComponent*> Splines;
		Actor->GetComponents<USplineComponent>(Splines);
		if (Splines.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Actor '%s' has no SplineComponent or WaterSplineComponent."), *Actor->GetActorLabel());
			return nullptr;
		}
		if (!ComponentName.IsEmpty())
		{
			for (USplineComponent* Spline : Splines)
			{
				if (Spline && (Spline->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
					|| Spline->GetPathName().Equals(ComponentName, ESearchCase::IgnoreCase)
					|| Spline->GetClass()->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)))
				{
					return Spline;
				}
			}
			OutError = FString::Printf(TEXT("Spline component '%s' was not found on actor '%s'."), *ComponentName, *Actor->GetActorLabel());
			return nullptr;
		}
		for (USplineComponent* Spline : Splines)
		{
			const FString ClassName = Spline ? Spline->GetClass()->GetName() : FString();
			if (Spline && ClassName.Contains(TEXT("WaterSpline")))
			{
				return Spline;
			}
		}
		return Splines[0];
	}

	static bool CollectSplinePointSpecs(const USplineComponent* Spline, const ESplineCoordinateSpace::Type Space, TArray<FSololmcpSplinePointSpec>& OutSpecs)
	{
		if (!Spline)
		{
			return false;
		}
		OutSpecs.Reset();
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
		{
			FSololmcpSplinePointSpec Spec;
			Spec.Location = Spline->GetLocationAtSplinePoint(Index, Space);
			Spec.ArriveTangent = Spline->GetArriveTangentAtSplinePoint(Index, Space);
			Spec.LeaveTangent = Spline->GetLeaveTangentAtSplinePoint(Index, Space);
			Spec.Type = Spline->GetSplinePointType(Index);
			Spec.bHasLocation = true;
			Spec.bHasArriveTangent = true;
			Spec.bHasLeaveTangent = true;
			Spec.bHasType = true;
			OutSpecs.Add(Spec);
		}
		return true;
	}

	static bool ApplySplinePointSpecs(USplineComponent* Spline, const TArray<FSololmcpSplinePointSpec>& Specs, const ESplineCoordinateSpace::Type Space, const bool bClosedLoop, FString& OutError)
	{
		if (!Spline)
		{
			OutError = TEXT("Spline component is null.");
			return false;
		}
		if (Specs.Num() < 2)
		{
			OutError = TEXT("At least two spline points are required for reliable river/spline authoring.");
			return false;
		}
		Spline->Modify();
		Spline->ClearSplinePoints(false);
		for (const FSololmcpSplinePointSpec& Spec : Specs)
		{
			Spline->AddSplinePoint(Spec.Location, Space, false);
			const int32 Index = Spline->GetNumberOfSplinePoints() - 1;
			Spline->SetSplinePointType(Index, Spec.Type, false);
			if (Spec.bHasArriveTangent || Spec.bHasLeaveTangent)
			{
				const FVector Arrive = Spec.bHasArriveTangent ? Spec.ArriveTangent : FVector::ZeroVector;
				const FVector Leave = Spec.bHasLeaveTangent ? Spec.LeaveTangent : FVector::ZeroVector;
				Spline->SetTangentsAtSplinePoint(Index, Arrive, Leave, Space, false);
			}
		}
		Spline->SetClosedLoop(bClosedLoop, false);
		Spline->UpdateSpline();
		Spline->MarkPackageDirty();
		Spline->PostEditChange();
		return true;
	}

	static TSharedRef<FJsonObject> PropertyBriefJson(UObject* Owner, FProperty* Property)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("owner"), Owner ? Owner->GetPathName() : FString());
		Obj->SetStringField(TEXT("ownerClass"), Owner && Owner->GetClass() ? Owner->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("name"), Property ? Property->GetName() : FString());
		Obj->SetStringField(TEXT("cppType"), Property ? Property->GetCPPType() : FString());
		Obj->SetBoolField(TEXT("editable"), Property && Property->HasAnyPropertyFlags(CPF_Edit));
		if (Owner && Property)
		{
			FString Exported;
			Property->ExportText_InContainer(0, Exported, Owner, Owner, Owner, PPF_None);
			Obj->SetStringField(TEXT("value"), Exported);
		}
		return Obj;
	}

	static bool TryReadVectorLikeProperty(UObject* Owner, FProperty* Property, FVector& OutVector)
	{
		if (!Owner || !Property)
		{
			return false;
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			void* ValuePtr = StructProperty->ContainerPtrToValuePtr<void>(Owner);
			if (StructProperty->Struct == TBaseStructure<FVector>::Get())
			{
				OutVector = *static_cast<FVector*>(ValuePtr);
				return true;
			}
			if (StructProperty->Struct == TBaseStructure<FVector2D>::Get())
			{
				const FVector2D Value = *static_cast<FVector2D*>(ValuePtr);
				OutVector = FVector(Value.X, Value.Y, 0.0);
				return true;
			}
		}
		return false;
	}

	static bool FindWaterZoneExtentVector(AActor* Actor, FVector& OutExtent, TSharedPtr<FJsonObject>& OutProperty, FString& OutOwnerPath)
	{
		if (!Actor)
		{
			return false;
		}
		TArray<UObject*> Owners;
		Owners.Add(Actor);
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component)
			{
				Owners.Add(Component);
			}
		}
		for (UObject* Owner : Owners)
		{
			if (!Owner || !Owner->GetClass())
			{
				continue;
			}
			for (TFieldIterator<FProperty> It(Owner->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				const FString Name = Property ? Property->GetName() : FString();
				if (!Property || !(Name.Equals(TEXT("ZoneExtent"), ESearchCase::IgnoreCase)
					|| Name.Equals(TEXT("Extent"), ESearchCase::IgnoreCase)
					|| Name.Contains(TEXT("ZoneExtent"), ESearchCase::IgnoreCase)))
				{
					continue;
				}
				FVector Candidate;
				if (TryReadVectorLikeProperty(Owner, Property, Candidate))
				{
					OutExtent = FVector(FMath::Abs(Candidate.X), FMath::Abs(Candidate.Y), FMath::Abs(Candidate.Z));
					OutProperty = PropertyBriefJson(Owner, Property);
					OutOwnerPath = Owner->GetPathName();
					return true;
				}
			}
		}
		return false;
	}

	static FBox BuildActorAuditBounds(AActor* Actor, const bool bUseWaterZoneExtentFallback, bool& bUsedZoneExtent, TSharedPtr<FJsonObject>& OutExtentProperty)
	{
		bUsedZoneExtent = false;
		OutExtentProperty.Reset();
		if (!Actor)
		{
			return FBox(EForceInit::ForceInit);
		}
		FBox Bounds = Actor->GetComponentsBoundingBox(true);
		if (bUseWaterZoneExtentFallback && IsWaterZoneActor(Actor))
		{
			FVector Extent;
			FString OwnerPath;
			if (FindWaterZoneExtentVector(Actor, Extent, OutExtentProperty, OwnerPath) && (Extent.X > 0.0 || Extent.Y > 0.0))
			{
				const FVector SafeExtent(FMath::Max(Extent.X, 1.0), FMath::Max(Extent.Y, 1.0), FMath::Max(Extent.Z, 100.0));
				Bounds = FBox::BuildAABB(Actor->GetActorLocation(), SafeExtent);
				if (OutExtentProperty.IsValid())
				{
					OutExtentProperty->SetStringField(TEXT("extentOwnerPath"), OwnerPath);
				}
				bUsedZoneExtent = true;
			}
		}
		if (!Bounds.IsValid)
		{
			Bounds = FBox::BuildAABB(Actor->GetActorLocation(), FVector(100.0, 100.0, 100.0));
		}
		return Bounds;
	}

	static TSharedRef<FJsonObject> ActorBoundsAuditJson(AActor* Actor, const bool bUseWaterZoneExtentFallback)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		bool bUsedZoneExtent = false;
		TSharedPtr<FJsonObject> ExtentProperty;
		const FBox Bounds = BuildActorAuditBounds(Actor, bUseWaterZoneExtentFallback, bUsedZoneExtent, ExtentProperty);
		Obj->SetObjectField(TEXT("bounds"), BoxJson(Bounds));
		Obj->SetBoolField(TEXT("usedWaterZoneExtentFallback"), bUsedZoneExtent);
		if (ExtentProperty.IsValid())
		{
			Obj->SetObjectField(TEXT("waterZoneExtentProperty"), ExtentProperty.ToSharedRef());
		}
		return Obj;
	}

	static UObject* ResolvePropertyOwner(AActor* Actor, const FString& ComponentName, FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("Actor is null.");
			return nullptr;
		}
		if (ComponentName.IsEmpty() || ComponentName.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
		{
			return Actor;
		}
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component && (Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
				|| Component->GetPathName().Equals(ComponentName, ESearchCase::IgnoreCase)
				|| Component->GetClass()->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)))
			{
				return Component;
			}
		}
		OutError = FString::Printf(TEXT("Component '%s' was not found on actor '%s'."), *ComponentName, *Actor->GetActorLabel());
		return nullptr;
	}

	static void ReadTransformArguments(const TSharedRef<FJsonObject>& Arguments, FVector& OutLocation, FRotator& OutRotation)
	{
		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && LocationObj->IsValid())
		{
			FSololmcpEditorServices::JsonToVector(*LocationObj, OutLocation);
		}
		const TSharedPtr<FJsonObject>* RotationObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("rotation"), RotationObj) && RotationObj && RotationObj->IsValid())
		{
			FSololmcpEditorServices::JsonToRotator(*RotationObj, OutRotation);
		}
	}

	static bool Tool_WaterBodyCreateV2(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString WaterType = TEXT("Lake");
		Arguments->TryGetStringField(TEXT("water_type"), WaterType);
		Arguments->TryGetStringField(TEXT("body_type"), WaterType);
		Arguments->TryGetStringField(TEXT("class_path"), WaterType);
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		bool bAllowWaterBrushManager = false;
		Arguments->TryGetBoolField(TEXT("allow_water_brush_manager"), bAllowWaterBrushManager);
		bool bAllowEditorSideEffects = false;
		Arguments->TryGetBoolField(TEXT("allow_editor_side_effects"), bAllowEditorSideEffects);
		bAllowWaterBrushManager = bAllowWaterBrushManager || bAllowEditorSideEffects;

		FString ClassPath;
		FString ClassError;
		UClass* Class = ResolveWaterActorClass(Context.Services, WaterType, ClassPath, ClassError);
		const bool bMaySpawnWaterBrushManager = IsWaterBrushManagerRiskClass(Class, ClassPath, WaterType);
		Out->SetStringField(TEXT("operation"), TEXT("water_body_create_v2"));
		Out->SetStringField(TEXT("waterType"), WaterType);
		Out->SetStringField(TEXT("classPath"), ClassPath);
		Out->SetBoolField(TEXT("execute"), bExecute);
		Out->SetBoolField(TEXT("classAvailable"), Class != nullptr);
		Out->SetBoolField(TEXT("allowWaterBrushManager"), bAllowWaterBrushManager);
		Out->SetBoolField(TEXT("maySpawnWaterBrushManager"), bMaySpawnWaterBrushManager);
		Out->SetBoolField(TEXT("unattendedSafe"), !bExecute || !bMaySpawnWaterBrushManager || bAllowWaterBrushManager);
		if (!ClassError.IsEmpty())
		{
			Out->SetStringField(TEXT("classError"), ClassError);
		}
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), Class ? TEXT("dry_run") : TEXT("blocked_missing_water_class"));
			Out->SetBoolField(TEXT("requires_execute"), Class != nullptr);
			Sum = FString::Printf(TEXT("Water body create dry-run for %s: classAvailable=%s."), *WaterType, Class ? TEXT("true") : TEXT("false"));
			return true;
		}
		if (!Class)
		{
			Err = ClassError.IsEmpty() ? TEXT("Water actor class not available. Ensure Water plugin is enabled.") : ClassError;
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_water_class"));
			return false;
		}
		if (bMaySpawnWaterBrushManager && !bAllowWaterBrushManager)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_requires_water_brush_manager_ack"));
			Out->SetBoolField(TEXT("requires_allow_water_brush_manager"), true);
			Out->SetArrayField(TEXT("safeAlternatives"), WaterBodySafeAlternativesJson());
			Err = TEXT("water_body_create_v2 execute=true for WaterBody classes can spawn WaterBrushManager and update landscape brushes; pass allow_water_brush_manager=true only in a guarded write lane.");
			return false;
		}
		UWorld* World = Context.Services.GetEditorWorld(Err);
		if (!World)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_no_world"));
			return false;
		}

		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		ReadTransformArguments(Arguments, Location, Rotation);
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterBodyCreateV2", "SOMOLMCP Create Water Body v2"));
		AActor* Actor = World->SpawnActor<AActor>(Class, FTransform(Rotation, Location), SpawnParams);
		if (!Actor)
		{
			Err = TEXT("Failed to spawn water body actor.");
			Out->SetStringField(TEXT("status"), TEXT("failed_spawn"));
			return false;
		}
		FString Label;
		if (Arguments->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
		{
			Actor->SetActorLabel(Label);
		}
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, true));
		Sum = FString::Printf(TEXT("Spawned water actor '%s'."), *Actor->GetActorLabel());
		return true;
	}

	static bool Tool_WaterZoneCreateV2(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		TSharedRef<FJsonObject> ForwardArgs = MakeShared<FJsonObject>();
		for (const auto& Pair : Arguments->Values)
		{
			ForwardArgs->SetField(Pair.Key, Pair.Value);
		}
		ForwardArgs->SetStringField(TEXT("class_path"), TEXT("/Script/Water.WaterZone"));
		return Tool_WaterBodyCreateV2(Context, ForwardArgs, Out, Sum, Err);
	}

	static bool Tool_WaterBodyReadbackSnapshot(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		bool bIncludeComponents = true;
		Arguments->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
		Out->SetStringField(TEXT("operation"), TEXT("water_body_readback_snapshot"));
		FString ActorId;
		if (TryGetActorArgument(Arguments, ActorId) && !ActorId.IsEmpty())
		{
			AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
			if (!Actor)
			{
				Out->SetStringField(TEXT("status"), TEXT("actor_not_found"));
				return false;
			}
			Out->SetStringField(TEXT("status"), IsWaterActor(Actor) ? TEXT("completed") : TEXT("not_water_actor"));
			Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, bIncludeComponents));
			Sum = FString::Printf(TEXT("Read back water actor snapshot for '%s'."), *Actor->GetActorLabel());
			return true;
		}

		UWorld* World = Context.Services.GetEditorWorld(Err);
		if (!World)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_no_world"));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Actors;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (IsWaterActor(Actor))
			{
				Actors.Add(MakeShared<FJsonValueObject>(WaterActorJson(Actor, bIncludeComponents)));
			}
		}
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetArrayField(TEXT("actors"), Actors);
		Out->SetNumberField(TEXT("actorCount"), Actors.Num());
		Sum = FString::Printf(TEXT("Read back %d water actor(s)."), Actors.Num());
		return true;
	}

	static bool ReadSplinePointArray(const TSharedRef<FJsonObject>& Arguments, TArray<FSololmcpSplinePointSpec>& OutSpecs, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Arguments->TryGetArrayField(TEXT("points"), Points) || !Points)
		{
			OutError = TEXT("Missing points array.");
			return false;
		}
		OutSpecs.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Points)
		{
			FSololmcpSplinePointSpec Spec;
			if (!ReadSplinePointSpec(Value.IsValid() ? Value->AsObject() : nullptr, Spec, OutError, true))
			{
				return false;
			}
			OutSpecs.Add(Spec);
		}
		if (OutSpecs.Num() < 2)
		{
			OutError = TEXT("At least two points are required.");
			return false;
		}
		return true;
	}

	static bool ResolveActorAndSpline(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Err, AActor*& OutActor, USplineComponent*& OutSpline)
	{
		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId) || ActorId.IsEmpty())
		{
			Err = TEXT("Missing actor/target_actor/label.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_actor"));
			return false;
		}
		OutActor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (!OutActor)
		{
			Out->SetStringField(TEXT("status"), TEXT("actor_not_found"));
			return false;
		}
		FString ComponentName;
		Arguments->TryGetStringField(TEXT("component"), ComponentName);
		Arguments->TryGetStringField(TEXT("spline_component"), ComponentName);
		OutSpline = FindBestSplineComponent(OutActor, Err, ComponentName);
		if (!OutSpline)
		{
			Out->SetStringField(TEXT("status"), TEXT("spline_not_found"));
			return false;
		}
		return true;
	}

	static bool AllowsWaterSplineMutation(const TSharedRef<FJsonObject>& Arguments)
	{
		bool bAllow = false;
		Arguments->TryGetBoolField(TEXT("allow_water_spline_mutation"), bAllow);
		bool bAllowBrush = false;
		Arguments->TryGetBoolField(TEXT("allow_water_brush_manager"), bAllowBrush);
		bool bAllowSideEffects = false;
		Arguments->TryGetBoolField(TEXT("allow_editor_side_effects"), bAllowSideEffects);
		return bAllow || bAllowBrush || bAllowSideEffects;
	}

	static bool Tool_WaterBodySplineGet(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		Out->SetStringField(TEXT("operation"), TEXT("water_body_spline_get"));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("spline"), SplineComponentJson(Spline, Space));
		Sum = FString::Printf(TEXT("Read %d spline point(s) from '%s'."), Spline->GetNumberOfSplinePoints(), *Actor->GetActorLabel());
		return true;
	}

	static bool Tool_WaterBodySplineSet(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		TArray<FSololmcpSplinePointSpec> Specs;
		if (!ReadSplinePointArray(Arguments, Specs, Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_invalid_points"));
			return false;
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		bool bClosedLoop = Spline->IsClosedLoop();
		Arguments->TryGetBoolField(TEXT("closed_loop"), bClosedLoop);
		Arguments->TryGetBoolField(TEXT("is_closed_loop"), bClosedLoop);
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		Out->SetStringField(TEXT("operation"), TEXT("water_body_spline_set"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("before"), SplineComponentJson(Spline, Space));
		Out->SetBoolField(TEXT("execute"), bExecute);
		Out->SetBoolField(TEXT("allowWaterSplineMutation"), AllowsWaterSplineMutation(Arguments));
		Out->SetNumberField(TEXT("plannedPointCount"), Specs.Num());
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Water spline set dry-run for '%s' with %d point(s)."), *Actor->GetActorLabel(), Specs.Num());
			return true;
		}
		if (IsWaterActor(Actor) && !AllowsWaterSplineMutation(Arguments))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_requires_allow_water_spline_mutation"));
			Err = TEXT("WaterBody spline mutation can rebuild water meshes/brushes; pass allow_water_spline_mutation=true in a guarded write lane.");
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterBodySplineSet", "SOMOLMCP Set Water Body Spline"));
		if (!ApplySplinePointSpecs(Spline, Specs, Space, bClosedLoop, Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_apply"));
			return false;
		}
		Actor->Modify();
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("after"), SplineComponentJson(Spline, Space));
		Sum = FString::Printf(TEXT("Updated water spline '%s' to %d point(s)."), *Actor->GetActorLabel(), Spline->GetNumberOfSplinePoints());
		return true;
	}

	static bool Tool_WaterBodySplinePointAdd(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		FSololmcpSplinePointSpec NewPoint;
		const TSharedPtr<FJsonObject>* PointObj = nullptr;
		const TSharedPtr<FJsonObject> PointSource = Arguments->TryGetObjectField(TEXT("point"), PointObj) && PointObj ? *PointObj : Arguments;
		if (!ReadSplinePointSpec(PointSource, NewPoint, Err, true))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_invalid_point"));
			return false;
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		TArray<FSololmcpSplinePointSpec> Specs;
		CollectSplinePointSpecs(Spline, Space, Specs);
		double RawIndex = -1.0;
		const bool bHasIndex = Arguments->TryGetNumberField(TEXT("index"), RawIndex);
		const int32 InsertIndex = bHasIndex ? FMath::Clamp(static_cast<int32>(RawIndex), 0, Specs.Num()) : Specs.Num();
		Specs.Insert(NewPoint, InsertIndex);
		Out->SetStringField(TEXT("operation"), TEXT("water_body_spline_point_add"));
		Out->SetObjectField(TEXT("before"), SplineComponentJson(Spline, Space));
		Out->SetNumberField(TEXT("insertIndex"), InsertIndex);
		Out->SetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Water spline point add dry-run for '%s' at index %d."), *Actor->GetActorLabel(), InsertIndex);
			return true;
		}
		if (IsWaterActor(Actor) && !AllowsWaterSplineMutation(Arguments))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_requires_allow_water_spline_mutation"));
			Err = TEXT("Pass allow_water_spline_mutation=true to mutate WaterBody spline points.");
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterBodySplinePointAdd", "SOMOLMCP Add Water Body Spline Point"));
		if (!ApplySplinePointSpecs(Spline, Specs, Space, Spline->IsClosedLoop(), Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_apply"));
			return false;
		}
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("after"), SplineComponentJson(Spline, Space));
		Sum = FString::Printf(TEXT("Added water spline point at index %d."), InsertIndex);
		return true;
	}

	static bool Tool_WaterBodySplinePointUpdate(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		double RawIndex = -1.0;
		if (!Arguments->TryGetNumberField(TEXT("index"), RawIndex))
		{
			Err = TEXT("Missing index.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_index"));
			return false;
		}
		const int32 Index = static_cast<int32>(RawIndex);
		if (Index < 0 || Index >= Spline->GetNumberOfSplinePoints())
		{
			Err = TEXT("Spline point index out of range.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_index_out_of_range"));
			return false;
		}
		FSololmcpSplinePointSpec Patch;
		const TSharedPtr<FJsonObject>* PointObj = nullptr;
		const TSharedPtr<FJsonObject> PointSource = Arguments->TryGetObjectField(TEXT("point"), PointObj) && PointObj ? *PointObj : Arguments;
		if (!ReadSplinePointSpec(PointSource, Patch, Err, false))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_invalid_point"));
			return false;
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		TArray<FSololmcpSplinePointSpec> Specs;
		CollectSplinePointSpecs(Spline, Space, Specs);
		if (Patch.bHasLocation) { Specs[Index].Location = Patch.Location; Specs[Index].bHasLocation = true; }
		if (Patch.bHasArriveTangent) { Specs[Index].ArriveTangent = Patch.ArriveTangent; Specs[Index].bHasArriveTangent = true; }
		if (Patch.bHasLeaveTangent) { Specs[Index].LeaveTangent = Patch.LeaveTangent; Specs[Index].bHasLeaveTangent = true; }
		if (Patch.bHasType) { Specs[Index].Type = Patch.Type; Specs[Index].bHasType = true; }
		Out->SetStringField(TEXT("operation"), TEXT("water_body_spline_point_update"));
		Out->SetObjectField(TEXT("before"), SplineComponentJson(Spline, Space));
		Out->SetNumberField(TEXT("index"), Index);
		Out->SetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Water spline point update dry-run for '%s' index %d."), *Actor->GetActorLabel(), Index);
			return true;
		}
		if (IsWaterActor(Actor) && !AllowsWaterSplineMutation(Arguments))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_requires_allow_water_spline_mutation"));
			Err = TEXT("Pass allow_water_spline_mutation=true to mutate WaterBody spline points.");
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterBodySplinePointUpdate", "SOMOLMCP Update Water Body Spline Point"));
		if (!ApplySplinePointSpecs(Spline, Specs, Space, Spline->IsClosedLoop(), Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_apply"));
			return false;
		}
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("after"), SplineComponentJson(Spline, Space));
		Sum = FString::Printf(TEXT("Updated water spline point %d."), Index);
		return true;
	}

	static bool Tool_WaterBodySplinePointRemove(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		double RawIndex = -1.0;
		if (!Arguments->TryGetNumberField(TEXT("index"), RawIndex))
		{
			Err = TEXT("Missing index.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_index"));
			return false;
		}
		const int32 Index = static_cast<int32>(RawIndex);
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		TArray<FSololmcpSplinePointSpec> Specs;
		CollectSplinePointSpecs(Spline, Space, Specs);
		if (Index < 0 || Index >= Specs.Num())
		{
			Err = TEXT("Spline point index out of range.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_index_out_of_range"));
			return false;
		}
		if (Specs.Num() <= 2)
		{
			Err = TEXT("Refusing to remove point: at least two points are required.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_minimum_point_count"));
			return false;
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		Specs.RemoveAt(Index);
		Out->SetStringField(TEXT("operation"), TEXT("water_body_spline_point_remove"));
		Out->SetObjectField(TEXT("before"), SplineComponentJson(Spline, Space));
		Out->SetNumberField(TEXT("index"), Index);
		Out->SetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Water spline point remove dry-run for '%s' index %d."), *Actor->GetActorLabel(), Index);
			return true;
		}
		if (IsWaterActor(Actor) && !AllowsWaterSplineMutation(Arguments))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_requires_allow_water_spline_mutation"));
			Err = TEXT("Pass allow_water_spline_mutation=true to mutate WaterBody spline points.");
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterBodySplinePointRemove", "SOMOLMCP Remove Water Body Spline Point"));
		if (!ApplySplinePointSpecs(Spline, Specs, Space, Spline->IsClosedLoop(), Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_apply"));
			return false;
		}
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("after"), SplineComponentJson(Spline, Space));
		Sum = FString::Printf(TEXT("Removed water spline point %d."), Index);
		return true;
	}

	static bool Tool_WaterBodyComponentSchema(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId) || ActorId.IsEmpty())
		{
			Err = TEXT("Missing actor/target_actor/label.");
			return false;
		}
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (!Actor)
		{
			Out->SetStringField(TEXT("status"), TEXT("actor_not_found"));
			return false;
		}
		FString ComponentName;
		Arguments->TryGetStringField(TEXT("component"), ComponentName);
		FString Filter;
		Arguments->TryGetStringField(TEXT("property_filter"), Filter);
		bool bIncludeReadOnly = false;
		Arguments->TryGetBoolField(TEXT("include_readonly"), bIncludeReadOnly);
		TArray<UObject*> Owners;
		if (UObject* ExplicitOwner = ResolvePropertyOwner(Actor, ComponentName, Err))
		{
			Owners.Add(ExplicitOwner);
		}
		else
		{
			return false;
		}
		if (ComponentName.IsEmpty())
		{
			TInlineComponentArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				if (Component && (Component->GetClass()->GetName().Contains(TEXT("Water"))
					|| Component->GetClass()->GetName().Contains(TEXT("Spline"))
					|| Component->GetClass()->GetPathName().Contains(TEXT("/Script/Water."))))
				{
					Owners.Add(Component);
				}
			}
		}
		TArray<TSharedPtr<FJsonValue>> OwnerRows;
		int32 PropertyCount = 0;
		for (UObject* Owner : Owners)
		{
			TSharedRef<FJsonObject> OwnerObj = MakeShared<FJsonObject>();
			OwnerObj->SetStringField(TEXT("owner"), Owner ? Owner->GetPathName() : FString());
			OwnerObj->SetStringField(TEXT("class"), Owner && Owner->GetClass() ? Owner->GetClass()->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Properties;
			for (TFieldIterator<FProperty> It(Owner->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}
				const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
				if (!bEditable && !bIncludeReadOnly)
				{
					continue;
				}
				if (!Filter.IsEmpty() && !Property->GetName().Contains(Filter, ESearchCase::IgnoreCase)
					&& !Property->GetCPPType().Contains(Filter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				Properties.Add(MakeShared<FJsonValueObject>(PropertyBriefJson(Owner, Property)));
				PropertyCount++;
			}
			OwnerObj->SetArrayField(TEXT("properties"), Properties);
			OwnerObj->SetNumberField(TEXT("propertyCount"), Properties.Num());
			OwnerRows.Add(MakeShared<FJsonValueObject>(OwnerObj));
		}
		Out->SetStringField(TEXT("operation"), TEXT("water_body_component_schema"));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetArrayField(TEXT("owners"), OwnerRows);
		Out->SetNumberField(TEXT("ownerCount"), OwnerRows.Num());
		Out->SetNumberField(TEXT("propertyCount"), PropertyCount);
		Sum = FString::Printf(TEXT("Water property schema read %d editable propertie(s)."), PropertyCount);
		return true;
	}

	static bool Tool_WaterBodyPropertySetV2(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId) || ActorId.IsEmpty())
		{
			Err = TEXT("Missing actor/target_actor/label.");
			return false;
		}
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (!Actor)
		{
			Out->SetStringField(TEXT("status"), TEXT("actor_not_found"));
			return false;
		}
		FString ComponentName;
		Arguments->TryGetStringField(TEXT("component"), ComponentName);
		UObject* Owner = ResolvePropertyOwner(Actor, ComponentName, Err);
		if (!Owner)
		{
			Out->SetStringField(TEXT("status"), TEXT("component_not_found"));
			return false;
		}
		FString PropertyName;
		Arguments->TryGetStringField(TEXT("property"), PropertyName);
		Arguments->TryGetStringField(TEXT("property_name"), PropertyName);
		if (PropertyName.IsEmpty())
		{
			Err = TEXT("Missing property/property_name.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_property"));
			return false;
		}
		FProperty* Property = Owner->GetClass()->FindPropertyByName(*PropertyName);
		if (!Property)
		{
			Err = FString::Printf(TEXT("Property '%s' was not found on %s."), *PropertyName, *Owner->GetClass()->GetName());
			Out->SetStringField(TEXT("status"), TEXT("property_not_found"));
			return false;
		}
		bool bExecute = false;
		bool bAllowNonEdit = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		Arguments->TryGetBoolField(TEXT("allow_non_edit_properties"), bAllowNonEdit);
		TSharedPtr<FJsonValue> Value = Arguments->TryGetField(TEXT("value"));
		if (!Value.IsValid())
		{
			if (TSharedPtr<FJsonValue> StringValue = Arguments->TryGetField(TEXT("value_string")); StringValue.IsValid())
			{
				Value = StringValue;
			}
			else if (TSharedPtr<FJsonValue> NumberValue = Arguments->TryGetField(TEXT("value_number")); NumberValue.IsValid())
			{
				Value = NumberValue;
			}
			else if (TSharedPtr<FJsonValue> BoolValue = Arguments->TryGetField(TEXT("value_bool")); BoolValue.IsValid())
			{
				Value = BoolValue;
			}
			else if (TSharedPtr<FJsonValue> ObjectValue = Arguments->TryGetField(TEXT("value_object")); ObjectValue.IsValid())
			{
				Value = ObjectValue;
			}
		}
		Out->SetStringField(TEXT("operation"), TEXT("water_body_property_set_v2"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("property"), PropertyBriefJson(Owner, Property));
		Out->SetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Water property set dry-run for %s.%s."), *Owner->GetName(), *PropertyName);
			return true;
		}
		if (!Value.IsValid())
		{
			Err = TEXT("Missing value.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_value"));
			return false;
		}
		if (!bAllowNonEdit && !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			Err = TEXT("Property is not CPF_Edit; pass allow_non_edit_properties=true only with explicit target knowledge.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_non_edit_property"));
			return false;
		}
		TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetField(PropertyName, Value);
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterBodyPropertySetV2", "SOMOLMCP Set Water Body Property v2"));
		Owner->Modify();
		if (!Context.Services.ApplyProperties(Owner, Props, Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_apply"));
			return false;
		}
		Owner->MarkPackageDirty();
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("after"), PropertyBriefJson(Owner, Property));
		Sum = FString::Printf(TEXT("Set water property %s.%s."), *Owner->GetName(), *PropertyName);
		return true;
	}

	static bool Tool_WaterBodyRebuild(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId) || ActorId.IsEmpty())
		{
			Err = TEXT("Missing actor/target_actor/label.");
			return false;
		}
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (!Actor)
		{
			Out->SetStringField(TEXT("status"), TEXT("actor_not_found"));
			return false;
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		bool bAllowSideEffects = false;
		Arguments->TryGetBoolField(TEXT("allow_editor_side_effects"), bAllowSideEffects);
		Arguments->TryGetBoolField(TEXT("allow_water_brush_manager"), bAllowSideEffects);
		Out->SetStringField(TEXT("operation"), TEXT("water_body_rebuild"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, true));
		Out->SetBoolField(TEXT("execute"), bExecute);
		Out->SetBoolField(TEXT("allowEditorSideEffects"), bAllowSideEffects);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Water actor rebuild dry-run for '%s'."), *Actor->GetActorLabel());
			return true;
		}
		if (IsWaterActor(Actor) && !bAllowSideEffects)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_requires_editor_side_effect_ack"));
			Err = TEXT("Water actor rebuild can update render state, construction scripts, and WaterBrush/WaterZone state; pass allow_editor_side_effects=true in a guarded write lane.");
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterBodyRebuild", "SOMOLMCP Rebuild Water Actor"));
		Actor->Modify();
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}
			Component->Modify();
			if (USceneComponent* Scene = Cast<USceneComponent>(Component))
			{
				Scene->UpdateComponentToWorld();
			}
			if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
			{
				Primitive->MarkRenderStateDirty();
			}
			Component->PostEditChange();
			Component->MarkPackageDirty();
		}
		Actor->RerunConstructionScripts();
		Actor->PostEditChange();
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("after"), WaterActorJson(Actor, true));
		Sum = FString::Printf(TEXT("Rebuilt water actor '%s'."), *Actor->GetActorLabel());
		return true;
	}

	static bool Tool_WaterZoneExtentSet(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId) || ActorId.IsEmpty())
		{
			Err = TEXT("Missing actor/target_actor/label.");
			return false;
		}
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (!Actor)
		{
			Out->SetStringField(TEXT("status"), TEXT("actor_not_found"));
			return false;
		}
		TArray<UObject*> Owners;
		Owners.Add(Actor);
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component)
			{
				Owners.Add(Component);
			}
		}
		FProperty* ExtentProperty = nullptr;
		UObject* ExtentOwner = nullptr;
		for (UObject* Owner : Owners)
		{
			for (TFieldIterator<FProperty> It(Owner->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				const FString Name = Property ? Property->GetName() : FString();
				if (Property && Property->HasAnyPropertyFlags(CPF_Edit)
					&& (Name.Equals(TEXT("ZoneExtent"), ESearchCase::IgnoreCase)
						|| Name.Equals(TEXT("Extent"), ESearchCase::IgnoreCase)
						|| Name.Contains(TEXT("ZoneExtent"), ESearchCase::IgnoreCase)))
				{
					ExtentProperty = Property;
					ExtentOwner = Owner;
					break;
				}
			}
			if (ExtentProperty)
			{
				break;
			}
		}
		Out->SetStringField(TEXT("operation"), TEXT("water_zone_extent_set"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, true));
		Out->SetBoolField(TEXT("propertyAvailable"), ExtentProperty != nullptr);
		if (ExtentProperty)
		{
			Out->SetObjectField(TEXT("extentProperty"), PropertyBriefJson(ExtentOwner, ExtentProperty));
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		Out->SetBoolField(TEXT("execute"), bExecute);
		if (!ExtentProperty || !ExtentOwner)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_extent_property_not_found"));
			Err = TEXT("No editable WaterZone extent property was found via reflection.");
			return false;
		}
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = TEXT("WaterZone extent dry-run found an editable extent property.");
			return true;
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		Arguments->TryGetNumberField(TEXT("x"), X);
		Arguments->TryGetNumberField(TEXT("y"), Y);
		Arguments->TryGetNumberField(TEXT("z"), Z);
		const TSharedPtr<FJsonObject>* ExtentObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("extent"), ExtentObj) && ExtentObj && ExtentObj->IsValid())
		{
			(*ExtentObj)->TryGetNumberField(TEXT("x"), X);
			(*ExtentObj)->TryGetNumberField(TEXT("y"), Y);
			(*ExtentObj)->TryGetNumberField(TEXT("z"), Z);
			(*ExtentObj)->TryGetNumberField(TEXT("X"), X);
			(*ExtentObj)->TryGetNumberField(TEXT("Y"), Y);
			(*ExtentObj)->TryGetNumberField(TEXT("Z"), Z);
		}
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterZoneExtentSet", "SOMOLMCP Set WaterZone Extent"));
		ExtentOwner->Modify();
		if (FStructProperty* StructProperty = CastField<FStructProperty>(ExtentProperty))
		{
			void* ValuePtr = StructProperty->ContainerPtrToValuePtr<void>(ExtentOwner);
			if (StructProperty->Struct == TBaseStructure<FVector2D>::Get())
			{
				*static_cast<FVector2D*>(ValuePtr) = FVector2D(X, Y);
			}
			else if (StructProperty->Struct == TBaseStructure<FVector>::Get())
			{
				*static_cast<FVector*>(ValuePtr) = FVector(X, Y, Z);
			}
			else
			{
				Err = FString::Printf(TEXT("Unsupported extent struct type '%s'."), *StructProperty->Struct->GetName());
				Out->SetStringField(TEXT("status"), TEXT("blocked_unsupported_extent_type"));
				return false;
			}
		}
		else
		{
			Err = TEXT("Extent property is not a vector struct.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_unsupported_extent_type"));
			return false;
		}
		ExtentOwner->PostEditChange();
		ExtentOwner->MarkPackageDirty();
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("after"), PropertyBriefJson(ExtentOwner, ExtentProperty));
		Sum = FString::Printf(TEXT("Set WaterZone extent to %.0f x %.0f."), X, Y);
		return true;
	}

	static TSharedRef<FJsonObject> WaterActorBoundsAuditRow(AActor* Actor)
	{
		TSharedRef<FJsonObject> Row = ActorBoundsAuditJson(Actor, IsWaterZoneActor(Actor));
		TArray<TSharedPtr<FJsonValue>> SplineRows;
		if (Actor)
		{
			TArray<USplineComponent*> Splines;
			Actor->GetComponents<USplineComponent>(Splines);
			for (USplineComponent* Spline : Splines)
			{
				if (!Spline)
				{
					continue;
				}
				FBox SplineBounds(EForceInit::ForceInit);
				for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
				{
					SplineBounds += Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World);
				}
				TSharedRef<FJsonObject> SplineRow = MakeShared<FJsonObject>();
				SplineRow->SetStringField(TEXT("name"), Spline->GetName());
				SplineRow->SetStringField(TEXT("class"), Spline->GetClass()->GetPathName());
				SplineRow->SetNumberField(TEXT("pointCount"), Spline->GetNumberOfSplinePoints());
				SplineRow->SetNumberField(TEXT("splineLength"), Spline->GetSplineLength());
				SplineRow->SetBoolField(TEXT("closedLoop"), Spline->IsClosedLoop());
				SplineRow->SetObjectField(TEXT("pointBounds"), BoxJson(SplineBounds));
				SplineRows.Add(MakeShared<FJsonValueObject>(SplineRow));
			}
		}
		Row->SetArrayField(TEXT("splines"), SplineRows);
		Row->SetNumberField(TEXT("splineCount"), SplineRows.Num());
		Row->SetBoolField(TEXT("isWaterActor"), IsWaterActor(Actor));
		Row->SetBoolField(TEXT("isWaterZone"), IsWaterZoneActor(Actor));
		return Row;
	}

	static bool Tool_WaterBodyBoundsAudit(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		Out->SetStringField(TEXT("operation"), TEXT("water_body_bounds_audit"));
		FString ActorId;
		if (TryGetActorArgument(Arguments, ActorId) && !ActorId.IsEmpty())
		{
			AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
			if (!Actor)
			{
				Out->SetStringField(TEXT("status"), TEXT("actor_not_found"));
				return false;
			}
			Out->SetStringField(TEXT("status"), IsWaterActor(Actor) ? TEXT("completed") : TEXT("completed_non_water_actor"));
			Out->SetObjectField(TEXT("audit"), WaterActorBoundsAuditRow(Actor));
			Sum = FString::Printf(TEXT("Audited bounds for '%s'."), *Actor->GetActorLabel());
			return true;
		}

		UWorld* World = Context.Services.GetEditorWorld(Err);
		if (!World)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_no_world"));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (IsWaterActor(Actor))
			{
				Rows.Add(MakeShared<FJsonValueObject>(WaterActorBoundsAuditRow(Actor)));
			}
		}
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetArrayField(TEXT("actors"), Rows);
		Out->SetNumberField(TEXT("actorCount"), Rows.Num());
		Sum = FString::Printf(TEXT("Audited bounds for %d water actor(s)."), Rows.Num());
		return true;
	}

	static bool Tool_WaterZoneCoverageAudit(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		UWorld* World = Context.Services.GetEditorWorld(Err);
		if (!World)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_no_world"));
			return false;
		}
		FString ZoneFilter;
		Arguments->TryGetStringField(TEXT("zone_actor"), ZoneFilter);
		Arguments->TryGetStringField(TEXT("water_zone"), ZoneFilter);
		if (ZoneFilter.IsEmpty())
		{
			TryGetActorArgument(Arguments, ZoneFilter);
		}

		TArray<AActor*> Zones;
		TArray<AActor*> Bodies;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (IsWaterZoneActor(Actor))
			{
				if (ZoneFilter.IsEmpty()
					|| Actor->GetActorLabel().Equals(ZoneFilter, ESearchCase::IgnoreCase)
					|| Actor->GetName().Equals(ZoneFilter, ESearchCase::IgnoreCase)
					|| Actor->GetPathName().Equals(ZoneFilter, ESearchCase::IgnoreCase))
				{
					Zones.Add(Actor);
				}
			}
			else if (IsWaterActor(Actor))
			{
				Bodies.Add(Actor);
			}
		}

		TArray<TSharedPtr<FJsonValue>> ZoneRows;
		int32 TotalCoveredBodies = 0;
		for (AActor* Zone : Zones)
		{
			bool bUsedZoneExtent = false;
			TSharedPtr<FJsonObject> ExtentProperty;
			const FBox ZoneBounds = BuildActorAuditBounds(Zone, true, bUsedZoneExtent, ExtentProperty);
			TSharedRef<FJsonObject> ZoneRow = MakeShared<FJsonObject>();
			ZoneRow->SetObjectField(TEXT("zone"), WaterActorJson(Zone, false));
			ZoneRow->SetObjectField(TEXT("bounds"), BoxJson(ZoneBounds));
			ZoneRow->SetBoolField(TEXT("usedWaterZoneExtentFallback"), bUsedZoneExtent);
			if (ExtentProperty.IsValid())
			{
				ZoneRow->SetObjectField(TEXT("waterZoneExtentProperty"), ExtentProperty.ToSharedRef());
			}
			TArray<TSharedPtr<FJsonValue>> BodyRows;
			for (AActor* Body : Bodies)
			{
				bool bUnused = false;
				TSharedPtr<FJsonObject> UnusedProperty;
				const FBox BodyBounds = BuildActorAuditBounds(Body, false, bUnused, UnusedProperty);
				const FVector BodyCenter = BodyBounds.IsValid ? BodyBounds.GetCenter() : (Body ? Body->GetActorLocation() : FVector::ZeroVector);
				const bool bCenterInside = ZoneBounds.IsValid && ZoneBounds.IsInside(BodyCenter);
				const bool bBoundsOverlap = ZoneBounds.IsValid && BodyBounds.IsValid && ZoneBounds.Intersect(BodyBounds);
				if (bCenterInside || bBoundsOverlap)
				{
					TotalCoveredBodies++;
				}
				TSharedRef<FJsonObject> BodyRow = MakeShared<FJsonObject>();
				BodyRow->SetObjectField(TEXT("actor"), WaterActorJson(Body, false));
				BodyRow->SetObjectField(TEXT("bounds"), BoxJson(BodyBounds));
				BodyRow->SetBoolField(TEXT("centerInside"), bCenterInside);
				BodyRow->SetBoolField(TEXT("boundsOverlap"), bBoundsOverlap);
				BodyRow->SetStringField(TEXT("coverageStatus"), (bCenterInside || bBoundsOverlap) ? TEXT("covered") : TEXT("outside"));
				BodyRows.Add(MakeShared<FJsonValueObject>(BodyRow));
			}
			ZoneRow->SetArrayField(TEXT("waterBodies"), BodyRows);
			ZoneRow->SetNumberField(TEXT("waterBodyCount"), BodyRows.Num());
			ZoneRows.Add(MakeShared<FJsonValueObject>(ZoneRow));
		}

		Out->SetStringField(TEXT("operation"), TEXT("water_zone_coverage_audit"));
		Out->SetStringField(TEXT("status"), Zones.Num() > 0 ? TEXT("completed") : TEXT("completed_no_water_zone"));
		Out->SetArrayField(TEXT("zones"), ZoneRows);
		Out->SetNumberField(TEXT("zoneCount"), Zones.Num());
		Out->SetNumberField(TEXT("waterBodyCount"), Bodies.Num());
		Out->SetNumberField(TEXT("coveredBodyMemberships"), TotalCoveredBodies);
		Sum = FString::Printf(TEXT("Audited %d WaterZone actor(s) against %d water body actor(s)."), Zones.Num(), Bodies.Num());
		return true;
	}

	static bool Tool_WaterFlowmapGeneratePlan(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		TArray<TSharedPtr<FJsonValue>> SegmentRows;
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints() - 1; ++Index)
		{
			const FVector A = Spline->GetLocationAtSplinePoint(Index, Space);
			const FVector B = Spline->GetLocationAtSplinePoint(Index + 1, Space);
			const FVector Delta = B - A;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("segment"), Index);
			Row->SetObjectField(TEXT("start"), VectorJson(A));
			Row->SetObjectField(TEXT("end"), VectorJson(B));
			Row->SetObjectField(TEXT("direction"), VectorJson(Delta.GetSafeNormal()));
			Row->SetNumberField(TEXT("length"), Delta.Size());
			SegmentRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetStringField(TEXT("operation"), TEXT("water_flowmap_generate_plan"));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("spline"), SplineComponentJson(Spline, Space));
		Out->SetArrayField(TEXT("segments"), SegmentRows);
		Out->SetArrayField(TEXT("nextTools"), StringArrayJson({TEXT("water_body_spline_get"), TEXT("water_body_property_set_v2"), TEXT("water_material_set_v2"), TEXT("water_landscape_receipt_validate")}));
		Sum = FString::Printf(TEXT("Generated flowmap vector plan from %d water spline segment(s)."), SegmentRows.Num());
		return true;
	}

	struct FFlowSegment
	{
		FVector A = FVector::ZeroVector;
		FVector B = FVector::ZeroVector;
		FVector Dir = FVector::ForwardVector;
		float LengthSq = 1.0f;
	};

	static float DistanceSqPointToSegment2D(const FVector& Point, const FFlowSegment& Segment, FVector& OutDir)
	{
		const FVector AB = Segment.B - Segment.A;
		const float LenSq = FMath::Max(AB.X * AB.X + AB.Y * AB.Y, 1.0f);
		const float T = FMath::Clamp(((Point.X - Segment.A.X) * AB.X + (Point.Y - Segment.A.Y) * AB.Y) / LenSq, 0.0f, 1.0f);
		const FVector Closest(Segment.A.X + AB.X * T, Segment.A.Y + AB.Y * T, 0.0f);
		OutDir = Segment.Dir;
		return FVector::DistSquared2D(Point, Closest);
	}

	static bool Tool_WaterFlowmapTextureCreate(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		FString OutputPath;
		Arguments->TryGetStringField(TEXT("output_path"), OutputPath);
		Arguments->TryGetStringField(TEXT("asset_path"), OutputPath);
		int32 Width = 256;
		int32 Height = 256;
		double WidthNum = 0.0;
		double HeightNum = 0.0;
		if (Arguments->TryGetNumberField(TEXT("width"), WidthNum))
		{
			Width = FMath::Clamp(static_cast<int32>(WidthNum), 16, 4096);
		}
		if (Arguments->TryGetNumberField(TEXT("height"), HeightNum))
		{
			Height = FMath::Clamp(static_cast<int32>(HeightNum), 16, 4096);
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		Out->SetStringField(TEXT("operation"), TEXT("water_flowmap_texture_create"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("spline"), SplineComponentJson(Spline, ESplineCoordinateSpace::World));
		Out->SetStringField(TEXT("outputPath"), OutputPath);
		Out->SetNumberField(TEXT("width"), Width);
		Out->SetNumberField(TEXT("height"), Height);
		Out->SetBoolField(TEXT("execute"), bExecute);

		TArray<FFlowSegment> Segments;
		FBox Bounds(EForceInit::ForceInit);
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
		{
			Bounds += Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World);
		}
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints() - 1; ++Index)
		{
			FFlowSegment Segment;
			Segment.A = Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World);
			Segment.B = Spline->GetLocationAtSplinePoint(Index + 1, ESplineCoordinateSpace::World);
			const FVector Delta = Segment.B - Segment.A;
			Segment.Dir = Delta.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
			Segment.LengthSq = FMath::Max(Delta.SizeSquared2D(), 1.0f);
			Segments.Add(Segment);
		}
		Out->SetNumberField(TEXT("segmentCount"), Segments.Num());
		Out->SetObjectField(TEXT("boundsMin"), VectorJson(Bounds.IsValid ? Bounds.Min : FVector::ZeroVector));
		Out->SetObjectField(TEXT("boundsMax"), VectorJson(Bounds.IsValid ? Bounds.Max : FVector::ZeroVector));
		if (Segments.Num() == 0)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_no_spline_segments"));
			Err = TEXT("Spline requires at least two points to generate a flowmap texture.");
			return false;
		}
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Water flowmap texture dry-run for '%s' with %d segment(s)."), *Actor->GetActorLabel(), Segments.Num());
			return true;
		}
		if (OutputPath.IsEmpty())
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_output_path"));
			Err = TEXT("output_path is required when execute=true.");
			return false;
		}
		FString PackagePath = OutputPath;
		int32 DotIndex = INDEX_NONE;
		if (PackagePath.FindChar(TEXT('.'), DotIndex))
		{
			PackagePath = PackagePath.Left(DotIndex);
		}
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_invalid_output_path"));
			Err = FString::Printf(TEXT("Invalid output_path '%s'."), *OutputPath);
			return false;
		}
		if (Context.Services.AssetExists(PackagePath) || Context.Services.AssetExists(PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath)))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_asset_exists"));
			Err = FString::Printf(TEXT("Output texture already exists: %s"), *PackagePath);
			return false;
		}

		const FVector Extent = Bounds.IsValid ? Bounds.GetExtent() : FVector(1000.0, 1000.0, 0.0);
		const FVector Center = Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector;
		const float MinX = Center.X - FMath::Max(Extent.X, 50.0f);
		const float MaxX = Center.X + FMath::Max(Extent.X, 50.0f);
		const float MinY = Center.Y - FMath::Max(Extent.Y, 50.0f);
		const float MaxY = Center.Y + FMath::Max(Extent.Y, 50.0f);

		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const float U = Width > 1 ? static_cast<float>(X) / static_cast<float>(Width - 1) : 0.0f;
				const float V = Height > 1 ? static_cast<float>(Y) / static_cast<float>(Height - 1) : 0.0f;
				const FVector WorldPoint(FMath::Lerp(MinX, MaxX, U), FMath::Lerp(MinY, MaxY, V), 0.0f);
				float BestDist = TNumericLimits<float>::Max();
				FVector BestDir = FVector::ForwardVector;
				for (const FFlowSegment& Segment : Segments)
				{
					FVector Dir;
					const float Dist = DistanceSqPointToSegment2D(WorldPoint, Segment, Dir);
					if (Dist < BestDist)
					{
						BestDist = Dist;
						BestDir = Dir;
					}
				}
				FColor& Pixel = Pixels[Y * Width + X];
				Pixel.R = static_cast<uint8>(FMath::Clamp((BestDir.X * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
				Pixel.G = static_cast<uint8>(FMath::Clamp((BestDir.Y * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
				Pixel.B = static_cast<uint8>(FMath::Clamp(FMath::Sqrt(BestDist) / 1000.0f * 255.0f, 0.0f, 255.0f));
				Pixel.A = 255;
			}
		}

		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_create_package"));
			Err = FString::Printf(TEXT("CreatePackage failed for %s."), *PackagePath);
			return false;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
		if (!Texture)
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_create_texture"));
			Err = TEXT("NewObject<UTexture2D> failed.");
			return false;
		}
#if WITH_EDITORONLY_DATA
		Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
#else
		Out->SetStringField(TEXT("status"), TEXT("blocked_requires_editoronly_data"));
		Err = TEXT("water_flowmap_texture_create requires editor-only texture source data.");
		return false;
#endif
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_VectorDisplacementmap;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Texture);
		const FString CreatedPath = Texture->GetPathName();
		FString SaveError;
		const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveError);
		Out->SetBoolField(TEXT("saved"), bSaved);
		if (!bSaved)
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_save"));
			Out->SetStringField(TEXT("saveError"), SaveError);
			Err = FString::Printf(TEXT("Failed to save flowmap texture '%s': %s"), *CreatedPath, *SaveError);
			return false;
		}
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetStringField(TEXT("assetPath"), CreatedPath);
		Out->SetStringField(TEXT("encoding"), TEXT("R=flow_x[-1..1], G=flow_y[-1..1], B=nearest_segment_distance/1000cm, A=255"));
		Sum = FString::Printf(TEXT("Created water flowmap texture '%s' (%dx%d)."), *CreatedPath, Width, Height);
		return true;
	}

	static bool Tool_WaterRiverLakePipelinePlan(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString WaterType = TEXT("River");
		Arguments->TryGetStringField(TEXT("water_type"), WaterType);
		Arguments->TryGetStringField(TEXT("body_type"), WaterType);
		FString Label;
		Arguments->TryGetStringField(TEXT("label"), Label);
		FString MaterialPath;
		Arguments->TryGetStringField(TEXT("material_path"), MaterialPath);
		const bool bHasPoints = Arguments->HasField(TEXT("points"));
		TArray<TSharedPtr<FJsonValue>> Steps;
		auto AddStep = [&Steps](const FString& Tool, const FString& Purpose)
		{
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("tool"), Tool);
			Step->SetStringField(TEXT("purpose"), Purpose);
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		};
		AddStep(TEXT("water_body_create_v2"), TEXT("Create guarded WaterBody actor in a write lane."));
		if (bHasPoints)
		{
			AddStep(TEXT("water_body_spline_set"), TEXT("Apply river/lake spline points with allow_water_spline_mutation gate."));
		}
		AddStep(TEXT("water_zone_create_v2"), TEXT("Ensure WaterZone coverage exists."));
		AddStep(TEXT("water_body_readback_snapshot"), TEXT("Read back actor, components, class, and transform."));
		if (!MaterialPath.IsEmpty())
		{
			AddStep(TEXT("water_material_set_v2"), TEXT("Bind target water material to editable material properties."));
		}
		AddStep(TEXT("water_collision_nav_audit"), TEXT("Audit water collision and nav relevance."));
		AddStep(TEXT("water_flowmap_generate_plan"), TEXT("Derive segment flow vectors from spline."));
		AddStep(TEXT("water_flowmap_texture_create"), TEXT("Optionally bake a flow-vector Texture2D asset from the spline."));
		AddStep(TEXT("water_body_rebuild"), TEXT("Guarded construction/render-state refresh after spline/material edits."));
		AddStep(TEXT("water_landscape_receipt_validate"), TEXT("Validate final receipt before delivery."));
		Out->SetStringField(TEXT("operation"), TEXT("water_river_lake_pipeline_plan"));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetStringField(TEXT("waterType"), WaterType);
		Out->SetStringField(TEXT("label"), Label);
		Out->SetBoolField(TEXT("hasSplinePoints"), bHasPoints);
		Out->SetArrayField(TEXT("steps"), Steps);
		Out->SetArrayField(TEXT("requiredGates"), StringArrayJson({TEXT("target_project_bound"), TEXT("allow_water_brush_manager_for_spawn"), TEXT("allow_water_spline_mutation_for_spline_edit"), TEXT("readback_snapshot"), TEXT("collision_nav_audit"), TEXT("receipt_validate")}));
		Sum = FString::Printf(TEXT("Built water river/lake pipeline plan with %d step(s)."), Steps.Num());
		return true;
	}

	static bool Tool_SplineActorCreate(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString Label = TEXT("SOMOLMCP_Spline");
		Arguments->TryGetStringField(TEXT("actor_name"), Label);
		Arguments->TryGetStringField(TEXT("label"), Label);
		TArray<FSololmcpSplinePointSpec> Specs;
		if (Arguments->HasField(TEXT("points")) && !ReadSplinePointArray(Arguments, Specs, Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_invalid_points"));
			return false;
		}
		if (Specs.Num() == 0)
		{
			FSololmcpSplinePointSpec A;
			A.Location = FVector::ZeroVector;
			A.bHasLocation = true;
			FSololmcpSplinePointSpec B;
			B.Location = FVector(500.0, 0.0, 0.0);
			B.bHasLocation = true;
			Specs.Add(A);
			Specs.Add(B);
		}
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		bool bClosedLoop = false;
		Arguments->TryGetBoolField(TEXT("closed_loop"), bClosedLoop);
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		Out->SetStringField(TEXT("operation"), TEXT("spline_actor_create"));
		Out->SetStringField(TEXT("label"), Label);
		Out->SetNumberField(TEXT("plannedPointCount"), Specs.Num());
		Out->SetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Spline actor create dry-run for '%s'."), *Label);
			return true;
		}
		UWorld* World = Context.Services.GetEditorWorld(Err);
		if (!World)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_no_world"));
			return false;
		}
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		ReadTransformArguments(Arguments, Location, Rotation);
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SplineActorCreate", "SOMOLMCP Create Spline Actor"));
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Rotation, Location));
		if (!Actor)
		{
			Err = TEXT("Failed to spawn spline actor.");
			Out->SetStringField(TEXT("status"), TEXT("failed_spawn"));
			return false;
		}
		Actor->SetActorLabel(Label);
		USplineComponent* Spline = NewObject<USplineComponent>(Actor, USplineComponent::StaticClass(), TEXT("SOMOLMCPSpline"), RF_Transactional);
		Actor->SetRootComponent(Spline);
		Spline->RegisterComponent();
		Actor->AddInstanceComponent(Spline);
		if (!ApplySplinePointSpecs(Spline, Specs, Space, bClosedLoop, Err))
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_apply"));
			return false;
		}
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, true));
		Out->SetObjectField(TEXT("spline"), SplineComponentJson(Spline, Space));
		Sum = FString::Printf(TEXT("Created spline actor '%s' with %d point(s)."), *Label, Spline->GetNumberOfSplinePoints());
		return true;
	}

	static bool Tool_SplineActorGetPoints(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		Out->SetStringField(TEXT("operation"), TEXT("spline_actor_get_points"));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("spline"), SplineComponentJson(Spline, Space));
		Sum = FString::Printf(TEXT("Read spline actor '%s' with %d point(s)."), *Actor->GetActorLabel(), Spline->GetNumberOfSplinePoints());
		return true;
	}

	static bool Tool_SplineActorSamplePoints(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		const ESplineCoordinateSpace::Type Space = ReadSplineCoordinateSpace(Arguments);
		double SampleCountRaw = 16.0;
		Arguments->TryGetNumberField(TEXT("sample_count"), SampleCountRaw);
		Arguments->TryGetNumberField(TEXT("samples"), SampleCountRaw);
		double DistanceInterval = 0.0;
		Arguments->TryGetNumberField(TEXT("distance_interval"), DistanceInterval);
		Arguments->TryGetNumberField(TEXT("spacing"), DistanceInterval);
		double MaxSamplesRaw = 1024.0;
		Arguments->TryGetNumberField(TEXT("max_samples"), MaxSamplesRaw);

		const float SplineLength = Spline->GetSplineLength();
		const int32 MaxSamples = FMath::Clamp(static_cast<int32>(MaxSamplesRaw), 2, 8192);
		int32 SampleCount = FMath::Clamp(static_cast<int32>(SampleCountRaw), 2, MaxSamples);
		if (DistanceInterval > 0.0)
		{
			SampleCount = FMath::Clamp(FMath::FloorToInt(SplineLength / static_cast<float>(DistanceInterval)) + 1, 2, MaxSamples);
		}

		TArray<TSharedPtr<FJsonValue>> Samples;
		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			const float Alpha = SampleCount > 1 ? static_cast<float>(Index) / static_cast<float>(SampleCount - 1) : 0.0f;
			const float Distance = SplineLength * Alpha;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetNumberField(TEXT("distance"), Distance);
			Row->SetNumberField(TEXT("alpha"), Alpha);
			Row->SetObjectField(TEXT("location"), VectorJson(Spline->GetLocationAtDistanceAlongSpline(Distance, Space)));
			Row->SetObjectField(TEXT("direction"), VectorJson(Spline->GetDirectionAtDistanceAlongSpline(Distance, Space)));
			Row->SetObjectField(TEXT("rotation"), RotatorJson(Spline->GetRotationAtDistanceAlongSpline(Distance, Space)));
			Row->SetObjectField(TEXT("scale"), VectorJson(Spline->GetScaleAtDistanceAlongSpline(Distance)));
			Samples.Add(MakeShared<FJsonValueObject>(Row));
		}

		Out->SetStringField(TEXT("operation"), TEXT("spline_actor_sample_points"));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("spline"), SplineComponentJson(Spline, Space));
		Out->SetArrayField(TEXT("samples"), Samples);
		Out->SetNumberField(TEXT("sampleCount"), Samples.Num());
		Out->SetNumberField(TEXT("splineLength"), SplineLength);
		Out->SetNumberField(TEXT("distanceInterval"), DistanceInterval);
		Out->SetNumberField(TEXT("maxSamples"), MaxSamples);
		Sum = FString::Printf(TEXT("Sampled %d point(s) from spline actor '%s'."), Samples.Num(), *Actor->GetActorLabel());
		return true;
	}

	static bool Tool_SplineActorSetPoints(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		const bool bResult = Tool_WaterBodySplineSet(Context, Arguments, Out, Sum, Err);
		Out->SetStringField(TEXT("operation"), TEXT("spline_actor_set_points"));
		return bResult;
	}

	static bool Tool_SplineActorAddMesh(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		AActor* Actor = nullptr;
		USplineComponent* Spline = nullptr;
		if (!ResolveActorAndSpline(Context, Arguments, Out, Err, Actor, Spline))
		{
			return false;
		}
		FString MeshPath;
		Arguments->TryGetStringField(TEXT("mesh_path"), MeshPath);
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		Out->SetStringField(TEXT("operation"), TEXT("spline_actor_add_mesh"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetObjectField(TEXT("spline"), SplineComponentJson(Spline, ESplineCoordinateSpace::World));
		Out->SetStringField(TEXT("meshPath"), MeshPath);
		Out->SetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Sum = FString::Printf(TEXT("Spline mesh add dry-run for '%s' across %d segment(s)."), *Actor->GetActorLabel(), FMath::Max(0, Spline->GetNumberOfSplinePoints() - 1));
			return true;
		}
		if (MeshPath.IsEmpty())
		{
			Err = TEXT("mesh_path is required when execute=true.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_mesh_path"));
			return false;
		}
		UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(MeshPath, Err));
		if (!Mesh)
		{
			if (Err.IsEmpty())
			{
				Err = FString::Printf(TEXT("Failed to load static mesh '%s'."), *MeshPath);
			}
			Out->SetStringField(TEXT("status"), TEXT("blocked_mesh_load_failed"));
			return false;
		}
		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SplineActorAddMesh", "SOMOLMCP Add Spline Mesh Components"));
		int32 Created = 0;
		for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints() - 1; ++Index)
		{
			FVector Start;
			FVector StartTangent;
			FVector End;
			FVector EndTangent;
			Spline->GetLocationAndTangentAtSplinePoint(Index, Start, StartTangent, ESplineCoordinateSpace::Local);
			Spline->GetLocationAndTangentAtSplinePoint(Index + 1, End, EndTangent, ESplineCoordinateSpace::Local);
			USplineMeshComponent* MeshComponent = NewObject<USplineMeshComponent>(Actor, USplineMeshComponent::StaticClass(), NAME_None, RF_Transactional);
			if (!MeshComponent)
			{
				continue;
			}
			Actor->AddInstanceComponent(MeshComponent);
			MeshComponent->SetMobility(EComponentMobility::Movable);
			MeshComponent->SetStaticMesh(Mesh);
			MeshComponent->SetStartAndEnd(Start, StartTangent, End, EndTangent, false);
			MeshComponent->AttachToComponent(Spline, FAttachmentTransformRules::KeepRelativeTransform);
			MeshComponent->RegisterComponent();
			MeshComponent->MarkPackageDirty();
			Created++;
		}
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), Created > 0 ? TEXT("completed") : TEXT("no_segments"));
		Out->SetNumberField(TEXT("createdComponentCount"), Created);
		Sum = FString::Printf(TEXT("Added %d spline mesh component(s)."), Created);
		return Created > 0;
	}

	struct FMaterialCandidate
	{
		UObject* Owner = nullptr;
		FObjectPropertyBase* Property = nullptr;
		FString OwnerKind;
	};

	static void CollectMaterialCandidates(UObject* Owner, const FString& OwnerKind, const FString& PropertyFilter, TArray<FMaterialCandidate>& OutCandidates)
	{
		if (!Owner)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Owner->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*It);
			if (!ObjProp || !ObjProp->PropertyClass || !ObjProp->PropertyClass->IsChildOf(UMaterialInterface::StaticClass()))
			{
				continue;
			}
			const FString PropName = ObjProp->GetName();
			if (!PropertyFilter.IsEmpty() && PropName != PropertyFilter)
			{
				continue;
			}
			if (PropertyFilter.IsEmpty() && !PropName.Contains(TEXT("Material")))
			{
				continue;
			}
			OutCandidates.Add({Owner, ObjProp, OwnerKind});
		}
	}

	static TSharedRef<FJsonObject> MaterialCandidateJson(const FMaterialCandidate& Candidate)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("owner"), Candidate.Owner ? Candidate.Owner->GetPathName() : FString());
		Obj->SetStringField(TEXT("ownerClass"), Candidate.Owner ? Candidate.Owner->GetClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("ownerKind"), Candidate.OwnerKind);
		Obj->SetStringField(TEXT("property"), Candidate.Property ? Candidate.Property->GetName() : FString());
		Obj->SetBoolField(TEXT("editable"), Candidate.Property && Candidate.Property->HasAnyPropertyFlags(CPF_Edit));
		if (Candidate.Owner && Candidate.Property)
		{
			UObject* Current = Candidate.Property->GetObjectPropertyValue_InContainer(Candidate.Owner);
			Obj->SetStringField(TEXT("current"), Current ? Current->GetPathName() : FString());
		}
		return Obj;
	}

	static bool Tool_WaterMaterialSetV2(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId) || ActorId.IsEmpty())
		{
			Err = TEXT("Missing actor/target_actor/label.");
			return false;
		}
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (!Actor)
		{
			return false;
		}
		FString MaterialPath;
		Arguments->TryGetStringField(TEXT("material_path"), MaterialPath);
		FString PropertyFilter;
		Arguments->TryGetStringField(TEXT("property_name"), PropertyFilter);
		bool bIncludeComponents = true;
		bool bExecute = false;
		bool bAllowNonEditProperties = false;
		Arguments->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		Arguments->TryGetBoolField(TEXT("allow_non_edit_properties"), bAllowNonEditProperties);

		TArray<FMaterialCandidate> Candidates;
		CollectMaterialCandidates(Actor, TEXT("actor"), PropertyFilter, Candidates);
		if (bIncludeComponents)
		{
			TInlineComponentArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Component : Components)
			{
				CollectMaterialCandidates(Component, TEXT("component"), PropertyFilter, Candidates);
			}
		}

		TArray<TSharedPtr<FJsonValue>> CandidateJson;
		for (const FMaterialCandidate& Candidate : Candidates)
		{
			CandidateJson.Add(MakeShared<FJsonValueObject>(MaterialCandidateJson(Candidate)));
		}
		Out->SetStringField(TEXT("operation"), TEXT("water_material_set_v2"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetStringField(TEXT("materialPath"), MaterialPath);
		Out->SetStringField(TEXT("propertyFilter"), PropertyFilter);
		Out->SetBoolField(TEXT("execute"), bExecute);
		Out->SetArrayField(TEXT("candidates"), CandidateJson);
		Out->SetNumberField(TEXT("candidateCount"), Candidates.Num());
		if (!bExecute)
		{
			Out->SetStringField(TEXT("status"), Candidates.Num() > 0 ? TEXT("dry_run") : TEXT("no_material_properties"));
			Out->SetBoolField(TEXT("requires_execute"), Candidates.Num() > 0);
			Sum = FString::Printf(TEXT("Water material dry-run found %d candidate material propertie(s)."), Candidates.Num());
			return true;
		}
		if (MaterialPath.IsEmpty())
		{
			Err = TEXT("material_path is required when execute=true.");
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_material"));
			return false;
		}
		UMaterialInterface* Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, Err));
		if (!Material)
		{
			if (Err.IsEmpty())
			{
				Err = FString::Printf(TEXT("Failed to load material interface: %s"), *MaterialPath);
			}
			Out->SetStringField(TEXT("status"), TEXT("blocked_material_load_failed"));
			return false;
		}

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "WaterMaterialSetV2", "SOMOLMCP Set Water Material v2"));
		int32 Changed = 0;
		for (const FMaterialCandidate& Candidate : Candidates)
		{
			if (!Candidate.Owner || !Candidate.Property)
			{
				continue;
			}
			if (!bAllowNonEditProperties && !Candidate.Property->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			Candidate.Owner->Modify();
			Candidate.Property->SetObjectPropertyValue_InContainer(Candidate.Owner, Material);
			Candidate.Owner->MarkPackageDirty();
			Changed++;
		}
		Actor->MarkPackageDirty();
		Out->SetStringField(TEXT("status"), Changed > 0 ? TEXT("completed") : TEXT("no_editable_material_properties"));
		Out->SetNumberField(TEXT("changedCount"), Changed);
		Sum = FString::Printf(TEXT("Set water material on %d propertie(s)."), Changed);
		return Changed > 0;
	}

	static bool Tool_WaterCollisionNavAudit(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId) || ActorId.IsEmpty())
		{
			Err = TEXT("Missing actor/target_actor/label.");
			return false;
		}
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (!Actor)
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Components;
		int32 PrimitiveCount = 0;
		int32 NavRelevantCount = 0;
		int32 CollisionEnabledCount = 0;
		TInlineComponentArray<UActorComponent*> ActorComponents;
		Actor->GetComponents(ActorComponents);
		for (UActorComponent* Component : ActorComponents)
		{
			TSharedRef<FJsonObject> Row = ComponentBriefJson(Component);
			if (Component && Component->CanEverAffectNavigation())
			{
				NavRelevantCount++;
			}
			if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
			{
				PrimitiveCount++;
				if (Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
				{
					CollisionEnabledCount++;
				}
			}
			Components.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetStringField(TEXT("operation"), TEXT("water_collision_nav_audit"));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
		Out->SetArrayField(TEXT("components"), Components);
		Out->SetNumberField(TEXT("componentCount"), Components.Num());
		Out->SetNumberField(TEXT("primitiveCount"), PrimitiveCount);
		Out->SetNumberField(TEXT("collisionEnabledCount"), CollisionEnabledCount);
		Out->SetNumberField(TEXT("navRelevantCount"), NavRelevantCount);
		Sum = FString::Printf(TEXT("Water collision/nav audit: primitives=%d collisionEnabled=%d navRelevant=%d."), PrimitiveCount, CollisionEnabledCount, NavRelevantCount);
		return true;
	}

	static bool Tool_LandscapeTexturePatchCreateV2(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		bool bExecute = false;
		Arguments->TryGetBoolField(TEXT("execute"), bExecute);
		if (!bExecute)
		{
			Out->SetStringField(TEXT("operation"), TEXT("landscape_texture_patch_create_v2"));
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetBoolField(TEXT("requires_execute"), true);
			Out->SetArrayField(TEXT("delegatesTo"), StringArrayJson({TEXT("landscape_texture_patch_create"), TEXT("landscape_patch_stack_inspect")}));
			Sum = TEXT("Landscape texture patch v2 dry-run. Set execute=true to call the concrete patch creator.");
			return true;
		}
		return Tool_LandscapeTexturePatchCreate(Context, Arguments, Out, Sum, Err);
	}

	static bool Tool_LandscapePatchComponentInspect(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		return Tool_LandscapePatchStackInspect(Context, Arguments, Out, Sum, Err);
	}

	static void AddReceiptCheck(TArray<TSharedPtr<FJsonValue>>& Checks, const FString& Name, const bool bPass, const FString& Detail)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Name);
		Row->SetBoolField(TEXT("pass"), bPass);
		Row->SetStringField(TEXT("detail"), Detail);
		Checks.Add(MakeShared<FJsonValueObject>(Row));
	}

	static bool Tool_WaterLandscapeReceiptValidate(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
	{
		const TSharedPtr<FJsonObject>* Receipt = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), Receipt) || !Receipt || !Receipt->IsValid())
		{
			Err = TEXT("Missing receipt object.");
			return false;
		}
		bool bRequireReadback = true;
		Arguments->TryGetBoolField(TEXT("require_readback"), bRequireReadback);
		FString ExpectedStatus = TEXT("completed");
		Arguments->TryGetStringField(TEXT("expected_status"), ExpectedStatus);

		TArray<TSharedPtr<FJsonValue>> Checks;
		bool bValid = true;
		FString Status;
		(*Receipt)->TryGetStringField(TEXT("status"), Status);
		const bool bStatusOk = ExpectedStatus.IsEmpty() || Status == ExpectedStatus;
		bValid &= bStatusOk;
		AddReceiptCheck(Checks, TEXT("status"), bStatusOk, FString::Printf(TEXT("status='%s', expected='%s'."), *Status, *ExpectedStatus));

		FString ActorId;
		if (!TryGetActorArgument(Arguments, ActorId))
		{
			(*Receipt)->TryGetStringField(TEXT("actor"), ActorId);
			(*Receipt)->TryGetStringField(TEXT("actorLabel"), ActorId);
			(*Receipt)->TryGetStringField(TEXT("label"), ActorId);
		}
		if (bRequireReadback)
		{
			FString LookupError;
			AActor* Actor = ActorId.IsEmpty() ? nullptr : Context.Services.FindActorByLabelOrName(ActorId, LookupError);
			const bool bReadbackOk = Actor != nullptr && IsWaterActor(Actor);
			bValid &= bReadbackOk;
			AddReceiptCheck(Checks, TEXT("water_actor_readback"), bReadbackOk, bReadbackOk ? Actor->GetPathName() : LookupError);
			if (Actor)
			{
				Out->SetObjectField(TEXT("actor"), WaterActorJson(Actor, false));
			}
		}
		Out->SetStringField(TEXT("operation"), TEXT("water_landscape_receipt_validate"));
		Out->SetStringField(TEXT("status"), bValid ? TEXT("completed") : TEXT("failed_validation"));
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetArrayField(TEXT("checks"), Checks);
		Sum = bValid ? TEXT("Water/landscape receipt validation passed.") : TEXT("Water/landscape receipt validation failed.");
		return bValid;
	}
}

void RegisterLandscapePatchPcgInteropTools(FSololmcpToolRegistry& Registry)
{
	using namespace LandscapePatchPcgInteropTools;

	Registry.Register({
		TEXT("landscape_patch_capability_probe"),
		TEXT("Probe LandscapePatch plugin coverage for SOMOLMCP-native patch edit-layer, circle patch, and texture patch routes."),
		PlanInputSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
		{
			const FLandscapePatchPcgInteropSpec Spec{
				TEXT("landscape_patch_capability_probe"),
				TEXT("Probe LandscapePatch plugin coverage for SOMOLMCP-native patch edit-layer, circle patch, and texture patch routes."),
				TEXT("landscape_patch"),
				TEXT("probe"),
				{TEXT("LandscapePatch")},
				{TEXT("LandscapePatch"), TEXT("LandscapePatchEditorOnly")},
				{TEXT("Editor/LandscapePatch/Source")},
				{TEXT("ULandscapePatchEditLayer"), TEXT("ULandscapePatchComponent"), TEXT("ULandscapeTexturePatch"), TEXT("ULandscapeCircleHeightPatch"), TEXT("AssignToLandscape"), TEXT("Patch manager is deprecated")},
				{},
				{TEXT("Probe plugin/module presence."), TEXT("Confirm patch edit-layer route instead of deprecated patch manager."), TEXT("Expose SOMOLMCP-native create tools for Circle and Texture patch components.")},
				{TEXT("engine_version"), TEXT("plugin_probe"), TEXT("module_probe"), TEXT("source_inventory"), TEXT("mcp_client_toolset_policy")},
				{TEXT("landscape_patch_edit_layer_create"), TEXT("landscape_circle_patch_create"), TEXT("landscape_texture_patch_create"), TEXT("landscape_patch_stack_inspect")},
				{TEXT("landscape_apply_heightmap_patch"), TEXT("landscape_set_height_region"), TEXT("landscape_paint_layer")}
			};
			return RunInteropPlan(Spec, Context, Arguments, Out, Sum, Err);
		},
		nullptr,
		30
	});

	Registry.Register({
		TEXT("landscape_patch_edit_layer_create"),
		TEXT("Create or reuse a Landscape Patch Edit Layer on a target landscape using SOMOLMCP-native reflection, not MCPClientToolset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor label/name/path."))},
			{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("Patch edit layer name."))},
			{TEXT("ignore_limit"), FSololmcpSchemaBuilder::Boolean(TEXT("Pass through to ALandscape::CreateLayer."))},
			{TEXT("set_editing_layer"), FSololmcpSchemaBuilder::Boolean(TEXT("Set the new patch layer as active editing layer."))}
		}, {TEXT("landscape"), TEXT("layer_name")}),
		Tool_LandscapePatchEditLayerCreate
	});

	Registry.Register({
		TEXT("landscape_circle_patch_create"),
		TEXT("Create a LandscapeCircleHeightPatch component, bind it to a Landscape Patch Edit Layer, and request a landscape update."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor label/name/path."))},
			{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("Patch edit layer name, created if absent."))},
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Optional host actor label."))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("rotation"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("radius"), FSololmcpSchemaBuilder::Number(TEXT("Circle radius in world units. Default 500."))},
			{TEXT("falloff"), FSololmcpSchemaBuilder::Number(TEXT("Falloff distance in world units. Default 500."))},
			{TEXT("edit_visibility"), FSololmcpSchemaBuilder::Boolean(TEXT("Patch visibility layer instead of height."))},
			{TEXT("exclusive_radius"), FSololmcpSchemaBuilder::Boolean(TEXT("Only vertices inside the circle get alpha 1."))},
			{TEXT("priority"), FSololmcpSchemaBuilder::Number(TEXT("Optional patch priority."))},
			{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable patch after creation. Default false to avoid unattended landscape merge stalls."))},
			{TEXT("request_update"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicitly request a landscape update after binding. Default false."))},
			{TEXT("ignore_limit"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow creating another edit layer past landscape limit."))}
		}, {TEXT("landscape"), TEXT("layer_name")}),
		Tool_LandscapeCirclePatchCreate
	});

	Registry.Register({
		TEXT("landscape_texture_patch_create"),
		TEXT("Create a LandscapeTexturePatch component, set coverage/resolution/basic height texture options, bind it to a patch edit layer, and request update."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor label/name/path."))},
			{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("Patch edit layer name, created if absent."))},
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Optional host actor label."))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("rotation"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("coverage_x"), FSololmcpSchemaBuilder::Number(TEXT("Unscaled patch coverage X in world units. Default 1000."))},
			{TEXT("coverage_y"), FSololmcpSchemaBuilder::Number(TEXT("Unscaled patch coverage Y in world units. Default 1000."))},
			{TEXT("resolution_x"), FSololmcpSchemaBuilder::Number(TEXT("Internal texture resolution X. Default 256."))},
			{TEXT("resolution_y"), FSololmcpSchemaBuilder::Number(TEXT("Internal texture resolution Y. Default 256."))},
			{TEXT("falloff"), FSololmcpSchemaBuilder::Number(TEXT("Patch falloff. Default 100."))},
			{TEXT("blend_mode"), FSololmcpSchemaBuilder::String(TEXT("AlphaBlend | Additive | Min | Max."))},
			{TEXT("falloff_mode"), FSololmcpSchemaBuilder::String(TEXT("Circle | RoundedRectangle."))},
			{TEXT("height_source_mode"), FSololmcpSchemaBuilder::String(TEXT("None | InternalTexture | TextureBackedRenderTarget | TextureAsset."))},
			{TEXT("height_texture_asset"), FSololmcpSchemaBuilder::String(TEXT("Optional UTexture asset path; sets HeightSourceMode=TextureAsset."))},
			{TEXT("priority"), FSololmcpSchemaBuilder::Number(TEXT("Optional patch priority."))},
			{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable patch after creation. Default false to avoid unattended landscape merge stalls."))},
			{TEXT("request_update"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicitly request a landscape update after binding. Default false."))},
			{TEXT("ignore_limit"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow creating another edit layer past landscape limit."))}
		}, {TEXT("landscape"), TEXT("layer_name")}),
		Tool_LandscapeTexturePatchCreate
	});

	Registry.Register({
		TEXT("landscape_patch_stack_inspect"),
		TEXT("Inspect live LandscapePatch components, bindings, edit layer GUIDs, and priorities. Optional landscape filter."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Optional landscape actor label/name/path filter."))}
		}),
		Tool_LandscapePatchStackInspect,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("ue58_pcg_mesh_partition_graph_apply"),
		TEXT("UE 5.8-only guarded Mesh Partition PCG graph apply. Adds Query/Write/Sculpt/Projection/Patch nodes through SOMOLMCP's existing PCG graph tools, captures snapshot/validate/rollback receipt evidence, and never uses MCPClientToolset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("target_graph_path"), FSololmcpSchemaBuilder::String(TEXT("Existing or newly created PCG graph path."))},
			{TEXT("pcg_graph_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for target_graph_path."))},
			{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Package path used when create_if_missing=true, e.g. /Game/SOMOLMCP/DisposablePCG."))},
			{TEXT("asset_name"), FSololmcpSchemaBuilder::String(TEXT("Asset name used when create_if_missing=true."))},
			{TEXT("create_if_missing"), FSololmcpSchemaBuilder::Boolean(TEXT("Create a PCG graph before adding nodes."))},
			{TEXT("allow_mutation"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for actual graph mutation. Omit/false for a blocked dry plan."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Return the node plan without mutating."))},
			{TEXT("mode"), FSololmcpSchemaBuilder::String(TEXT("query_only | query_write | query_sculpt | projection | patch | full_preview. Default query_write."))},
			{TEXT("node_roles"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Explicit role: query/write/sculpt_layer/projection_spawner/patch_spawner")) )},
			{TEXT("node_prefix"), FSololmcpSchemaBuilder::String(TEXT("Label prefix for inserted nodes. Default MeshPartition."))},
			{TEXT("query_properties"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("write_properties"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("sculpt_layer_properties"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("projection_spawner_properties"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("patch_spawner_properties"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("edges"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Optional edges: {from/source_pin_path, to/target_pin_path}."))},
			{TEXT("snapshot_before"), FSololmcpSchemaBuilder::Boolean(TEXT("Take pcg_graph_snapshot before mutating existing graph. Default true."))},
			{TEXT("validate_after"), FSololmcpSchemaBuilder::Boolean(TEXT("Run pcg_graph_validate after node insertion. Default true."))},
			{TEXT("rollback_on_failure"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore snapshot if validation fails. Default true."))}
		}),
		[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err)
		{
			return Tool_Ue58PcgMeshPartitionGraphApply(Registry, Context, Arguments, Out, Sum, Err);
		},
		nullptr,
		0
	});

	Registry.Register({
		TEXT("ue58_pcg_mesh_partition_receipt_validate"),
		TEXT("Validate a UE 5.8 Mesh Partition graph apply receipt before allowing generate or delivery claims."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("strict"), FSololmcpSchemaBuilder::Boolean(TEXT("Treat missing snapshot evidence as error. Default true."))}
		}),
		Tool_Ue58PcgMeshPartitionReceiptValidate,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("pcg_terrain_production_receipt_gate"),
		TEXT("Fail-closed gate for PCG/terrain production receipts. Requires graph binding, validate, dry-run/calibration, tile-cap, generated actor health, preview, and rollback/cleanup evidence by default."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("require_preview"), FSololmcpSchemaBuilder::Boolean(TEXT("Require screenshot/thumbnail/preview evidence. Default true."))},
			{TEXT("require_health_audit"), FSololmcpSchemaBuilder::Boolean(TEXT("Require generated actor health/provenance audit. Default true."))},
			{TEXT("require_dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Require dry-run/calibration evidence. Default true."))},
			{TEXT("require_tile_cap"), FSololmcpSchemaBuilder::Boolean(TEXT("Require tile-cap/tile status evidence. Default true."))},
			{TEXT("require_validation"), FSololmcpSchemaBuilder::Boolean(TEXT("Require pcg_graph_validate evidence. Default true."))}
		}),
		Tool_PcgTerrainProductionReceiptGate,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_create_v2"),
		TEXT("P1 concrete Water body creator for UE 5.7/5.8. Dry-run by default; WaterBody execute is fail-closed unless allow_water_brush_manager=true because UE may spawn WaterBrushManager and update landscape brushes."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("water_type"), FSololmcpSchemaBuilder::String(TEXT("River | Lake | Ocean | Custom | WaterBodyRiver | /Script/Water.WaterBodyRiver. Default Lake."))},
			{TEXT("body_type"), FSololmcpSchemaBuilder::String(TEXT("Alias for water_type."))},
			{TEXT("class_path"), FSololmcpSchemaBuilder::String(TEXT("Explicit Water actor class path."))},
			{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label."))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("rotation"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, only validates class and spawn plan."))},
			{TEXT("allow_water_brush_manager"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. Required for WaterBody execute because UE may create WaterBrushManager and mutate landscape brushes."))},
			{TEXT("allow_editor_side_effects"), FSololmcpSchemaBuilder::Boolean(TEXT("Alias for allow_water_brush_manager."))}
		}),
		Tool_WaterBodyCreateV2,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_zone_create_v2"),
		TEXT("P1 concrete WaterZone creator for UE 5.7/5.8. Dry-run by default; execute=true spawns a WaterZone actor and returns readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label."))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("rotation"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, only validates class and spawn plan."))},
			{TEXT("allow_water_brush_manager"), FSololmcpSchemaBuilder::Boolean(TEXT("Accepted for schema compatibility; WaterZone is normally unattended-safe."))}
		}),
		Tool_WaterZoneCreateV2,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_readback_snapshot"),
		TEXT("P1 concrete readback for WaterBody/WaterZone actors. With no actor argument, lists all live water actors in the editor world."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("include_components"), FSololmcpSchemaBuilder::Boolean(TEXT("Include component rows. Default true."))}
		}),
		Tool_WaterBodyReadbackSnapshot,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_spline_get"),
		TEXT("UE 5.7+ concrete WaterBody/WaterSpline readback. Resolves WaterSplineComponent or the first SplineComponent and returns point/tangent/length data."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterBody actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional spline component name/path/class."))},
			{TEXT("coordinate_space"), FSololmcpSchemaBuilder::String(TEXT("world | local. Default world."))}
		}, {TEXT("actor")}),
		Tool_WaterBodySplineGet,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("water_body_spline_set"),
		TEXT("UE 5.7+ concrete WaterBody/WaterSpline point setter. Dry-run by default; execute=true requires allow_water_spline_mutation for WaterBody actors."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterBody or spline actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional spline component name/path/class."))},
			{TEXT("points"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
				// Schema-clarity fix: location/tangents were empty Object({}) so the
				// LLM had no hint they need {x,y,z} and produced points without it,
				// failing "Spline point requires location {x,y,z}". Declare x/y/z
				// explicitly so both the model and the schema validator see the shape.
				{TEXT("location"), FSololmcpSchemaBuilder::Object({
					{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("World X (cm)"))},
					{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("World Y (cm)"))},
					{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("World Z (cm)"))}
				}, {TEXT("x"), TEXT("y"), TEXT("z")})},
				{TEXT("arrive_tangent"), FSololmcpSchemaBuilder::Object({
					{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("Tangent X"))},
					{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("Tangent Y"))},
					{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("Tangent Z"))}
				})},
				{TEXT("leave_tangent"), FSololmcpSchemaBuilder::Object({
					{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("Tangent X"))},
					{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("Tangent Y"))},
					{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("Tangent Z"))}
				})},
				{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("curve | linear | constant | curve_clamped | curve_custom_tangent"))}
			}))},
			{TEXT("closed_loop"), FSololmcpSchemaBuilder::Boolean(TEXT("Set spline closed loop."))},
			{TEXT("coordinate_space"), FSololmcpSchemaBuilder::String(TEXT("world | local. Default world."))},
			{TEXT("allow_water_spline_mutation"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for WaterBody execute=true."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor"), TEXT("points")}),
		Tool_WaterBodySplineSet,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_spline_point_add"),
		TEXT("UE 5.7+ concrete WaterBody/WaterSpline point insert. Dry-run by default; execute=true requires allow_water_spline_mutation for WaterBody actors."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterBody or spline actor label/name/path."))},
			{TEXT("index"), FSololmcpSchemaBuilder::Number(TEXT("Optional insert index. Defaults to append."))},
			{TEXT("point"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("allow_water_spline_mutation"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for WaterBody execute=true."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor")}),
		Tool_WaterBodySplinePointAdd,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_spline_point_update"),
		TEXT("UE 5.7+ concrete WaterBody/WaterSpline point update by index. Dry-run by default; execute=true requires allow_water_spline_mutation for WaterBody actors."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterBody or spline actor label/name/path."))},
			{TEXT("index"), FSololmcpSchemaBuilder::Number(TEXT("Point index."))},
			{TEXT("point"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("allow_water_spline_mutation"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for WaterBody execute=true."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor"), TEXT("index")}),
		Tool_WaterBodySplinePointUpdate,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_spline_point_remove"),
		TEXT("UE 5.7+ concrete WaterBody/WaterSpline point removal by index. Keeps at least two points. Dry-run by default."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterBody or spline actor label/name/path."))},
			{TEXT("index"), FSololmcpSchemaBuilder::Number(TEXT("Point index."))},
			{TEXT("allow_water_spline_mutation"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for WaterBody execute=true."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor"), TEXT("index")}),
		Tool_WaterBodySplinePointRemove,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_material_set_v2"),
		TEXT("P1 concrete Water material property binder. Dry-run lists editable material properties; execute=true sets material_path on matching properties."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Water actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Material or material instance asset path."))},
			{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact material property name. When omitted, all material-named properties are candidates."))},
			{TEXT("include_components"), FSololmcpSchemaBuilder::Boolean(TEXT("Inspect actor components as material owners. Default true."))},
			{TEXT("allow_non_edit_properties"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow setting non-Edit UPROPERTY material fields. Default false."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns candidate material properties only."))}
		}, {TEXT("actor")}),
		Tool_WaterMaterialSetV2,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_collision_nav_audit"),
		TEXT("P1 concrete Water collision/navigation audit for components, collision profiles, overlap flags, and nav relevance."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Water actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))}
		}, {TEXT("actor")}),
		Tool_WaterCollisionNavAudit,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_component_schema"),
		TEXT("UE 5.7+ reflection schema/readback for WaterBody, WaterZone, WaterSpline, and related component properties."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Water actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional component name/path/class, or 'actor'."))},
			{TEXT("property_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional property name/type substring."))},
			{TEXT("include_readonly"), FSololmcpSchemaBuilder::Boolean(TEXT("Include non-edit properties. Default false."))}
		}, {TEXT("actor")}),
		Tool_WaterBodyComponentSchema,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("water_body_property_set_v2"),
		TEXT("UE 5.7+ safe reflected property setter for WaterBody/WaterZone actors and components. Dry-run by default; execute=true requires editable property unless explicitly allowed."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Water actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional component name/path/class, or 'actor'."))},
			{TEXT("property"), FSololmcpSchemaBuilder::String(TEXT("Property name."))},
			{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("Alias for property."))},
			{TEXT("value"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Generic JSON value. Use typed aliases below when schema-strict clients need primitive values."))},
			{TEXT("value_string"), FSololmcpSchemaBuilder::String(TEXT("Primitive string value alias."))},
			{TEXT("value_number"), FSololmcpSchemaBuilder::Number(TEXT("Primitive number value alias."))},
			{TEXT("value_bool"), FSololmcpSchemaBuilder::Boolean(TEXT("Primitive boolean value alias."))},
			{TEXT("value_object"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Object/struct value alias."))},
			{TEXT("allow_non_edit_properties"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor"), TEXT("property")}),
		Tool_WaterBodyPropertySetV2,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_rebuild"),
		TEXT("UE 5.7+ guarded WaterBody/WaterZone construction and render-state refresh after spline/material/property edits. Dry-run by default."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Water actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("allow_editor_side_effects"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for execute=true on Water actors."))},
			{TEXT("allow_water_brush_manager"), FSololmcpSchemaBuilder::Boolean(TEXT("Alias for allow_editor_side_effects."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor")}),
		Tool_WaterBodyRebuild,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_zone_rebuild"),
		TEXT("Alias of water_body_rebuild for WaterZone actors. Refreshes construction scripts/components/render state behind an explicit side-effect gate."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterZone actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("allow_editor_side_effects"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for execute=true."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor")}),
		Tool_WaterBodyRebuild,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_zone_extent_set"),
		TEXT("UE 5.7+ reflected WaterZone extent setter. Finds editable ZoneExtent/Extent vector property, supports dry-run/readback, and posts editor change on execute."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterZone actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("extent"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("Extent X if extent object is omitted."))},
			{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("Extent Y if extent object is omitted."))},
			{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("Extent Z for FVector extent fields."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor")}),
		Tool_WaterZoneExtentSet,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_body_bounds_audit"),
		TEXT("UE 5.7+ read-only bounds audit for WaterBody/WaterZone actors, including spline point bounds and WaterZone extent fallback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Optional water actor label/name/path. When omitted, audits all live water actors."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))}
		}),
		Tool_WaterBodyBoundsAudit,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("water_zone_coverage_audit"),
		TEXT("UE 5.7+ read-only WaterZone coverage audit. Compares WaterZone bounds/extent against live WaterBody actors and reports covered/outside memberships."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("zone_actor"), FSololmcpSchemaBuilder::String(TEXT("Optional WaterZone label/name/path filter."))},
			{TEXT("water_zone"), FSololmcpSchemaBuilder::String(TEXT("Alias for zone_actor."))},
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for zone_actor when a generic client only supports actor."))}
		}),
		Tool_WaterZoneCoverageAudit,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("water_flowmap_generate_plan"),
		TEXT("UE 5.7+ concrete flow-vector planning from a WaterBody/WaterSpline. Produces per-segment directions and receipt steps; does not write texture assets yet."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterBody actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional spline component."))},
			{TEXT("coordinate_space"), FSololmcpSchemaBuilder::String(TEXT("world | local. Default world."))}
		}, {TEXT("actor")}),
		Tool_WaterFlowmapGeneratePlan,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("water_flowmap_texture_create"),
		TEXT("UE 5.7+ flowmap Texture2D baker from a WaterBody/WaterSpline or generic SplineComponent. Encodes R/G as XY flow direction, B as nearest-segment distance, A=255. Dry-run by default."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("WaterBody or spline actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional spline component."))},
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Output Texture2D asset path, e.g. /Game/SOMOLMCP/Flow/T_River_Flow"))},
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for output_path."))},
			{TEXT("width"), FSololmcpSchemaBuilder::Number(TEXT("Texture width. Clamped 16..4096. Default 256."))},
			{TEXT("height"), FSololmcpSchemaBuilder::Number(TEXT("Texture height. Clamped 16..4096. Default 256."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor")}),
		Tool_WaterFlowmapTextureCreate,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_river_lake_pipeline_plan"),
		TEXT("UE 5.7+ river/lake production pipeline planner linking create, spline set, zone, material, audit, flowmap plan/texture bake, rebuild, and receipt validation tools."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("water_type"), FSololmcpSchemaBuilder::String(TEXT("River | Lake | Ocean | Custom. Default River."))},
			{TEXT("body_type"), FSololmcpSchemaBuilder::String(TEXT("Alias for water_type."))},
			{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Optional target label."))},
			{TEXT("points"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Optional material asset path."))}
		}),
		Tool_WaterRiverLakePipelinePlan,
		nullptr,
		10
	});

	Registry.Register({
		TEXT("spline_actor_create"),
		TEXT("UE 5.7+ generic spline actor creator for rivers, roads, camera rails, PCG corridors, and greybox guide paths. Dry-run by default."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Actor label."))},
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Alias for label."))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("rotation"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("points"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("closed_loop"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))},
			{TEXT("coordinate_space"), FSololmcpSchemaBuilder::String(TEXT("world | local. Default world."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}),
		Tool_SplineActorCreate,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("spline_actor_get_points"),
		TEXT("UE 5.7+ generic spline actor readback with point/tangent/length data."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Spline actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional spline component name/path/class."))},
			{TEXT("coordinate_space"), FSololmcpSchemaBuilder::String(TEXT("world | local. Default world."))}
		}, {TEXT("actor")}),
		Tool_SplineActorGetPoints,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("spline_actor_sample_points"),
		TEXT("UE 5.7+ generic spline sampler for roads, rivers, camera rails, PCG corridors, and flowmap planning. Returns uniformly spaced transforms along the spline."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Spline actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional spline component name/path/class."))},
			{TEXT("sample_count"), FSololmcpSchemaBuilder::Number(TEXT("Uniform sample count. Default 16."))},
			{TEXT("samples"), FSololmcpSchemaBuilder::Number(TEXT("Alias for sample_count."))},
			{TEXT("distance_interval"), FSololmcpSchemaBuilder::Number(TEXT("Optional spacing in world units; overrides sample_count."))},
			{TEXT("spacing"), FSololmcpSchemaBuilder::Number(TEXT("Alias for distance_interval."))},
			{TEXT("max_samples"), FSololmcpSchemaBuilder::Number(TEXT("Safety cap. Default 1024, clamped 2..8192."))},
			{TEXT("coordinate_space"), FSololmcpSchemaBuilder::String(TEXT("world | local. Default world."))}
		}, {TEXT("actor")}),
		Tool_SplineActorSamplePoints,
		nullptr,
		5
	});

	Registry.Register({
		TEXT("spline_actor_set_points"),
		TEXT("UE 5.7+ generic spline actor point setter. Reuses the guarded spline implementation; non-Water actors do not require water mutation acknowledgement."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Spline actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("points"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))},
			{TEXT("closed_loop"), FSololmcpSchemaBuilder::Boolean(TEXT("Set spline closed loop."))},
			{TEXT("coordinate_space"), FSololmcpSchemaBuilder::String(TEXT("world | local. Default world."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor"), TEXT("points")}),
		Tool_SplineActorSetPoints,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("spline_actor_add_mesh"),
		TEXT("UE 5.7+ generic spline mesh component authoring. Adds one USplineMeshComponent per spline segment on execute=true."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Spline actor label/name/path."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("mesh_path"), FSololmcpSchemaBuilder::String(TEXT("UStaticMesh asset path."))},
			{TEXT("component"), FSololmcpSchemaBuilder::String(TEXT("Optional spline component name/path/class."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false."))}
		}, {TEXT("actor"), TEXT("mesh_path")}),
		Tool_SplineActorAddMesh,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("landscape_texture_patch_create_v2"),
		TEXT("P1 concrete Landscape Texture Patch v2 wrapper. Dry-run by default; execute=true delegates to the proven landscape_texture_patch_create route."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor label/name/path."))},
			{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("Patch edit layer name, created if absent."))},
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Optional host actor label."))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("rotation"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("coverage_x"), FSololmcpSchemaBuilder::Number(TEXT("Unscaled patch coverage X in world units. Default 1000."))},
			{TEXT("coverage_y"), FSololmcpSchemaBuilder::Number(TEXT("Unscaled patch coverage Y in world units. Default 1000."))},
			{TEXT("resolution_x"), FSololmcpSchemaBuilder::Number(TEXT("Internal texture resolution X. Default 256."))},
			{TEXT("resolution_y"), FSololmcpSchemaBuilder::Number(TEXT("Internal texture resolution Y. Default 256."))},
			{TEXT("falloff"), FSololmcpSchemaBuilder::Number(TEXT("Patch falloff. Default 100."))},
			{TEXT("blend_mode"), FSololmcpSchemaBuilder::String(TEXT("AlphaBlend | Additive | Min | Max."))},
			{TEXT("falloff_mode"), FSololmcpSchemaBuilder::String(TEXT("Circle | RoundedRectangle."))},
			{TEXT("height_source_mode"), FSololmcpSchemaBuilder::String(TEXT("None | InternalTexture | TextureBackedRenderTarget | TextureAsset."))},
			{TEXT("height_texture_asset"), FSololmcpSchemaBuilder::String(TEXT("Optional UTexture asset path; sets HeightSourceMode=TextureAsset."))},
			{TEXT("priority"), FSololmcpSchemaBuilder::Number(TEXT("Optional patch priority."))},
			{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable patch after creation. Default false."))},
			{TEXT("request_update"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicitly request a landscape update after binding. Default false."))},
			{TEXT("ignore_limit"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow creating another edit layer past landscape limit."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Default false. When false, returns a plan only."))}
		}),
		Tool_LandscapeTexturePatchCreateV2,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("landscape_patch_component_inspect"),
		TEXT("P1 concrete Landscape Patch component readback. Delegates to the live patch stack inspector with optional landscape filter."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Optional landscape actor label/name/path filter."))}
		}),
		Tool_LandscapePatchComponentInspect,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("water_landscape_receipt_validate"),
		TEXT("P1 concrete receipt validator for Water/Landscape Patch operations. Checks status and optional actor readback evidence."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("receipt"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Optional water actor label/name/path for readback."))},
			{TEXT("target_actor"), FSololmcpSchemaBuilder::String(TEXT("Alias for actor."))},
			{TEXT("expected_status"), FSololmcpSchemaBuilder::String(TEXT("Expected receipt status. Default completed."))},
			{TEXT("require_readback"), FSololmcpSchemaBuilder::Boolean(TEXT("Require live water actor readback. Default true."))}
		}, {TEXT("receipt")}),
		Tool_WaterLandscapeReceiptValidate,
		nullptr,
		0
	});

	Registry.Register({
		TEXT("mcp_p1_p2_landscape_pcg_capability_report"),
		TEXT("Summarize MCP-side P1/P2 Landscape/PCG coverage and remaining live-proof boundaries."),
		FSololmcpSchemaBuilder::Object({}),
		Tool_McpP1P2LandscapePcgCapabilityReport,
		nullptr,
		30
	});

	for (const FLandscapePatchPcgInteropSpec& Spec : PlanSpecs())
	{
		RegisterPlanSpec(Registry, Spec);
	}
}
}
