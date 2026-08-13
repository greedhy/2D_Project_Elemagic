// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 native PCG authoring transactions that rely only on public PCG APIs.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Runtime/Launch/Resources/Version.h"

// These tools need PCG, nothing more. They used to be gated on
// SOMOLMCP_WITH_UE58_MESHPARTITION, a flag for an unrelated module that this file
// never references -- which compiled all 41 of them out of every engine without
// MeshPartition, 5.7 included. One function inside genuinely needs a 5.8-only PCG
// API; that one is gated where it is used rather than at the top of the file.
#ifndef SOMOLMCP_HAS_PCG
#define SOMOLMCP_HAS_PCG 0
#endif

#if SOMOLMCP_HAS_PCG

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EdMode.h"
#include "HAL/FileManager.h"
#include "InteractiveToolManager.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "PCGCommon.h"
#include "PCGComponent.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#define SOMOLMCP_PCG_HAS_WORLDOBJECT_EXEC_SOURCE 1
#include "PCGDefaultWorldObjectExecutionSource.h"
#else
#define SOMOLMCP_PCG_HAS_WORLDOBJECT_EXEC_SOURCE 0
#endif
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"

// The PCG user-parameter hierarchy editor types (UPropertyBagHierarchyRoot and
// friends) arrived with 5.8's StructUtilsEditor. Four of this file's tools use them;
// the other ~37 do not, which is why this is scoped to those four rather than to the
// whole file the way the old blanket gate was.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#define SOMOLMCP_PCG_HAS_PARAM_HIERARCHY 1
#include "HierarchyEditor/PropertyBagHierarchyViewModel.h"
#else
#define SOMOLMCP_PCG_HAS_PARAM_HIERARCHY 0
#endif
#include "Elements/PCGSceneCapture.h"
#include "Subsystems/PCGSubsystem.h"
#include "DataHierarchyViewModelBase.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::SOMOLMCP
{
namespace Pcg58Native
{
#if SOMOLMCP_HAS_PCG
static UPCGGraph* LoadGraph(const TSharedRef<FJsonObject>& Arguments, FString& Error)
{
	FString GraphPath;
	if (!Arguments->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty())
	{
		Error = TEXT("graph_path is required.");
		return nullptr;
	}
	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *GraphPath);
	if (!Graph)
	{
		Error = FString::Printf(TEXT("PCG graph was not found: %s"), *GraphPath);
	}
	return Graph;
}

static TSharedRef<FJsonObject> GraphSnapshot(UPCGGraph* Graph)
{
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("graph_path"), Graph->GetPathName());
	Snapshot->SetStringField(TEXT("package_name"), Graph->GetOutermost()->GetName());
	Snapshot->SetStringField(TEXT("graph_usage"), StaticEnum<EPCGGraphUsage>()->GetNameStringByValue(static_cast<int64>(Graph->GetGraphUsage())));
	Snapshot->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
	Snapshot->SetNumberField(TEXT("embedded_subgraph_count"), Graph->GetEmbeddedSubgraphs().Num());
	int32 MarkedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const UPCGNode* Node : Graph->GetNodes())
	{
		if (!Node) continue;
		const UPCGSettings* Settings = Node->GetSettings();
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
		Row->SetStringField(TEXT("node_path"), Node->GetPathName());
		Row->SetStringField(TEXT("settings_class"), Settings ? Settings->GetClass()->GetPathName() : FString());
		const bool bMarked = Settings && Settings->IsMarkedForManualEditing();
		Row->SetBoolField(TEXT("marked_for_manual_editing"), bMarked);
		if (bMarked) ++MarkedCount;
		Nodes.Add(MakeShared<FJsonValueObject>(Row));
	}
	Snapshot->SetNumberField(TEXT("manual_edit_node_count"), MarkedCount);
	Snapshot->SetArrayField(TEXT("nodes"), Nodes);
	return Snapshot;
}

static bool SaveGraph(UPCGGraph* Graph, TSharedRef<FJsonObject>& Receipt, FString& Error)
{
	UPackage* Package = Graph ? Graph->GetOutermost() : nullptr;
	if (!Package || Package == GetTransientPackage() || !FPackageName::IsValidLongPackageName(Package->GetName()))
	{
		Error = TEXT("PCG graph is not backed by a persistent package.");
		return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, Graph, *Filename, SaveArgs);
	const bool bExists = IFileManager::Get().FileExists(*Filename);
	Receipt->SetBoolField(TEXT("saved"), bSaved);
	Receipt->SetBoolField(TEXT("package_file_exists"), bExists);
	Receipt->SetStringField(TEXT("package_filename"), Filename);
	if (!bSaved || !bExists)
	{
		Error = FString::Printf(TEXT("Failed to save PCG graph package %s."), *Package->GetName());
		return false;
	}
	return true;
}

static TSharedRef<FJsonObject> Receipt(const FString& Tool, UPCGGraph* Graph)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("somolmcp.pcg58_native_receipt.v1"));
	Result->SetStringField(TEXT("tool"), Tool);
	Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Result->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	if (Graph) Result->SetObjectField(TEXT("graph_readback"), GraphSnapshot(Graph));
	return Result;
}

static bool MatchesNode(const UPCGNode* Node, const FString& RequestedTitle, int32 RequestedIndex, int32 Index)
{
	if (!Node) return false;
	if (RequestedIndex >= 0) return RequestedIndex == Index;
	return RequestedTitle.IsEmpty() || Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString().Equals(RequestedTitle, ESearchCase::IgnoreCase);
}

static UPCGNode* ResolveNode(UPCGGraph* Graph, const TSharedRef<FJsonObject>& Arguments, FString& Error)
{
	FString NodeTitle;
	Arguments->TryGetStringField(TEXT("node_title"), NodeTitle);
	int32 NodeIndex = -1;
	Arguments->TryGetNumberField(TEXT("node_index"), NodeIndex);
	for (int32 Index = 0; Index < Graph->GetNodes().Num(); ++Index)
	{
		UPCGNode* Node = Graph->GetNodes()[Index];
		if (MatchesNode(Node, NodeTitle, NodeIndex, Index)) return Node;
	}
	Error = FString::Printf(TEXT("PCG node was not found (title='%s', index=%d)."), *NodeTitle, NodeIndex);
	return nullptr;
}

static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid()) return FString();
	if (Value->Type == EJson::String) return Value->AsString();
	if (Value->Type == EJson::Boolean) return Value->AsBool() ? TEXT("True") : TEXT("False");
	if (Value->Type == EJson::Number) return FString::SanitizeFloat(Value->AsNumber());
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
	return Text;
}

static TSharedRef<FJsonObject> EditableProperties(UObject* Object)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Object) return Result;
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient)) continue;
		FString Value;
		Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
		Result->SetStringField(Property->GetName(), Value);
	}
	return Result;
}

static bool PatchEditableProperties(UObject* Object, const TSharedPtr<FJsonObject>& Patch, TArray<FString>& Changed, FString& Error)
{
	if (!Object || !Patch.IsValid())
	{
		Error = TEXT("properties object is required for this mutation.");
		return false;
	}
	Object->Modify();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Patch->Values)
	{
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *Pair.Key);
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient))
		{
			Error = FString::Printf(TEXT("Editable property '%s' was not found on %s."), *Pair.Key, *Object->GetClass()->GetPathName());
			return false;
		}
		const FString ImportText = JsonValueToImportText(Pair.Value);
		if (!Property->ImportText_Direct(*ImportText, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None))
		{
			Error = FString::Printf(TEXT("Property '%s' rejected '%s'."), *Pair.Key, *ImportText);
			return false;
		}
		FPropertyChangedEvent Event(Property);
		Object->PostEditChangeProperty(Event);
		Changed.Add(Pair.Key);
	}
	return true;
}

static UClass* ResolveSettingsClass(const TSharedRef<FJsonObject>& Arguments, const TArray<FString>& Defaults, FString& Error)
{
	TArray<FString> Candidates;
	FString ExplicitClass;
	if (Arguments->TryGetStringField(TEXT("settings_class"), ExplicitClass) && !ExplicitClass.IsEmpty()) Candidates.Add(ExplicitClass);
	Candidates.Append(Defaults);
	for (const FString& Candidate : Candidates)
	{
		UClass* Class = LoadObject<UClass>(nullptr, *Candidate);
		if (!Class) Class = FindObject<UClass>(nullptr, *Candidate);
		if (Class && Class->IsChildOf(UPCGSettings::StaticClass())) return Class;
	}
	Error = FString::Printf(TEXT("No registered UPCGSettings class resolved from: %s"), *FString::Join(Candidates, TEXT(", ")));
	return nullptr;
}

static UPCGNode* AddSettingsNode(UPCGGraph* Graph, UClass* SettingsClass, const TSharedPtr<FJsonObject>& Properties,
	TSharedRef<FJsonObject>& OperationReceipt, FString& Error)
{
	if (!Graph || !SettingsClass) return nullptr;
	Graph->Modify();
	UPCGSettings* Settings = nullptr;
	UPCGNode* Node = Graph->AddNodeOfType(SettingsClass, Settings);
	if (!Node || !Settings)
	{
		Error = FString::Printf(TEXT("UPCGGraph::AddNodeOfType failed for %s."), *SettingsClass->GetPathName());
		return nullptr;
	}
	TArray<FString> Changed;
	if (Properties.IsValid() && !PatchEditableProperties(Settings, Properties, Changed, Error)) return nullptr;
	Graph->MarkPackageDirty();
	OperationReceipt->SetStringField(TEXT("created_node_path"), Node->GetPathName());
	OperationReceipt->SetStringField(TEXT("created_settings_class"), SettingsClass->GetPathName());
	TArray<TSharedPtr<FJsonValue>> ChangedValues;
	for (const FString& Item : Changed) ChangedValues.Add(MakeShared<FJsonValueString>(Item));
	OperationReceipt->SetArrayField(TEXT("changed_properties"), ChangedValues);
	return Node;
}

static UPCGGraph* ResolveEmbeddedGraph(UPCGGraph* Graph, const TSharedRef<FJsonObject>& Arguments, FString& Error)
{
	FString Name;
	Arguments->TryGetStringField(TEXT("embedded_name"), Name);
	for (UPCGGraph* Embedded : Graph->GetEmbeddedSubgraphs())
	{
		if (Embedded && (Name.IsEmpty() || Embedded->GetName().Equals(Name, ESearchCase::IgnoreCase))) return Embedded;
	}
	Error = FString::Printf(TEXT("Embedded PCG graph was not found: %s"), *Name);
	return nullptr;
}

#if SOMOLMCP_PCG_HAS_PARAM_HIERARCHY
static UHierarchyElement* FindHierarchyElement(UHierarchyElement* Root, const FString& Name)
{
	if (!Root) return nullptr;
	if (Root->ToString().Equals(Name, ESearchCase::IgnoreCase)) return Root;
	for (const TObjectPtr<UHierarchyElement>& Child : Root->GetChildren())
	{
		if (UHierarchyElement* Found = FindHierarchyElement(Child.Get(), Name)) return Found;
	}
	return nullptr;
}
#endif

static bool Execute(const FString& Name, const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	Out->SetStringField(TEXT("tool"), Name);
	Out->SetStringField(TEXT("implementation"), TEXT("ue58_native_pcg_public_api"));
	if (Name == TEXT("pcg_manual_editing_capability_probe"))
	{
		Out->SetBoolField(TEXT("available"), true);
		Out->SetBoolField(TEXT("persistent_node_marker_supported"), true);
		Out->SetBoolField(TEXT("embedded_subgraphs_supported"), true);
		Out->SetBoolField(TEXT("graph_usage_supported"), true);
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = TEXT("UE 5.8 native PCG manual-edit marker, embedded-subgraph, and graph-usage APIs are available.");
		return true;
	}

	UPCGGraph* Graph = LoadGraph(Arguments, Error);
	if (!Graph) return false;
	TSharedRef<FJsonObject> OpReceipt = Receipt(Name, Graph);
	OpReceipt->SetObjectField(TEXT("pre_snapshot"), GraphSnapshot(Graph));

	if (Name == TEXT("pcg_manual_override_list"))
	{
		Out->SetObjectField(TEXT("graph"), GraphSnapshot(Graph));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = TEXT("Returned persistent UE 5.8 PCG manual-edit node markers.");
		return true;
	}
	if (Name == TEXT("pcg_manual_override_restore"))
	{
		FString NodeTitle;
		Arguments->TryGetStringField(TEXT("node_title"), NodeTitle);
		int32 NodeIndex = -1;
		Arguments->TryGetNumberField(TEXT("node_index"), NodeIndex);
		int32 Changed = 0;
		for (int32 Index = 0; Index < Graph->GetNodes().Num(); ++Index)
		{
			UPCGNode* Node = Graph->GetNodes()[Index];
			if (!MatchesNode(Node, NodeTitle, NodeIndex, Index)) continue;
			if (UPCGSettings* Settings = Node->GetSettings(); Settings && Settings->IsMarkedForManualEditing())
			{
				Settings->Modify();
				Settings->SetMarkedForManualEditing(false);
				++Changed;
			}
		}
		Graph->Modify();
		Graph->MarkPackageDirty();
		OpReceipt->SetNumberField(TEXT("restored_node_count"), Changed);
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_manual_override_commit"))
	{
		Graph->MarkPackageDirty();
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_embedded_subgraph_create"))
	{
		Graph->Modify();
		UPCGGraph* Embedded = Graph->AddNewEmbeddedSubgraph();
		if (!Embedded)
		{
			Error = TEXT("UPCGGraph::AddNewEmbeddedSubgraph returned null.");
			return false;
		}
		FString RequestedName;
		if (Arguments->TryGetStringField(TEXT("name"), RequestedName) && !RequestedName.IsEmpty())
		{
			Embedded->Rename(*RequestedName, Graph, REN_DontCreateRedirectors);
		}
		Graph->MarkPackageDirty();
		OpReceipt->SetStringField(TEXT("embedded_graph_path"), Embedded->GetPathName());
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_embedded_subgraph_open"))
	{
		TArray<TSharedPtr<FJsonValue>> EmbeddedRows;
		for (UPCGGraph* Embedded : Graph->GetEmbeddedSubgraphs())
		{
			if (!Embedded) continue;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Embedded->GetName());
			Row->SetStringField(TEXT("path"), Embedded->GetPathName());
			Row->SetNumberField(TEXT("node_count"), Embedded->GetNodes().Num());
			EmbeddedRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetArrayField(TEXT("embedded_subgraphs"), EmbeddedRows);
	}
	else if (Name == TEXT("pcg_embedded_subgraph_compile_validate"))
	{
		int32 NullGraphs = 0;
		int32 NullNodes = 0;
		for (UPCGGraph* Embedded : Graph->GetEmbeddedSubgraphs())
		{
			if (!Embedded) { ++NullGraphs; continue; }
			for (UPCGNode* Node : Embedded->GetNodes()) if (!Node || !Node->GetSettings()) ++NullNodes;
		}
		OpReceipt->SetNumberField(TEXT("null_embedded_graphs"), NullGraphs);
		OpReceipt->SetNumberField(TEXT("null_nodes_or_settings"), NullNodes);
		OpReceipt->SetBoolField(TEXT("structure_valid"), NullGraphs == 0 && NullNodes == 0);
		if (NullGraphs != 0 || NullNodes != 0)
		{
			Error = TEXT("Embedded subgraph structure validation failed.");
			return false;
		}
	}
	else if (Name == TEXT("pcg_embedded_subgraph_save_reload"))
	{
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_graph_usage_set"))
	{
		FString Usage;
		if (!Arguments->TryGetStringField(TEXT("usage"), Usage))
		{
			Error = TEXT("usage is required: standard, asset, or level.");
			return false;
		}
		EPCGGraphUsage Value = EPCGGraphUsage::Standard;
		if (Usage.Equals(TEXT("asset"), ESearchCase::IgnoreCase)) Value = EPCGGraphUsage::Asset;
		else if (Usage.Equals(TEXT("level"), ESearchCase::IgnoreCase)) Value = EPCGGraphUsage::Level;
		else if (!Usage.Equals(TEXT("standard"), ESearchCase::IgnoreCase))
		{
			Error = TEXT("usage must be standard, asset, or level.");
			return false;
		}
		Graph->Modify();
		Graph->GraphUsageContext = Value;
		Graph->MarkPackageDirty();
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_parameter_hierarchy_readback"))
	{
		UObject* Hierarchy = Graph->GetUserParameterHierarchyRoot();
		Out->SetBoolField(TEXT("hierarchy_present"), Hierarchy != nullptr);
		Out->SetStringField(TEXT("hierarchy_path"), Hierarchy ? Hierarchy->GetPathName() : FString());
		Out->SetStringField(TEXT("hierarchy_class"), Hierarchy ? Hierarchy->GetClass()->GetPathName() : FString());
	}
	else if (Name == TEXT("pcg_manual_override_receipt_validate"))
	{
		const TSharedPtr<FJsonObject>* ReceiptObject = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("receipt"), ReceiptObject) || !ReceiptObject || !ReceiptObject->IsValid())
		{
			Error = TEXT("receipt object is required.");
			return false;
		}
		bool bSaved = false;
		(*ReceiptObject)->TryGetBoolField(TEXT("saved"), bSaved);
		FString Schema;
		(*ReceiptObject)->TryGetStringField(TEXT("schema"), Schema);
		const bool bValid = bSaved && Schema == TEXT("somolmcp.pcg58_native_receipt.v1");
		Out->SetBoolField(TEXT("valid"), bValid);
		if (!bValid)
		{
			Error = TEXT("PCG 5.8 receipt lacks native schema or save evidence.");
			return false;
		}
	}
	else if (Name == TEXT("pcg_manual_override_select") || Name == TEXT("pcg_manual_override_exclude") || Name == TEXT("pcg_manual_override_modify"))
	{
		UPCGNode* Node = ResolveNode(Graph, Arguments, Error);
		if (!Node || !Node->GetSettings()) return false;
		UPCGSettingsInterface* Settings = Node->GetSettingsInterface();
		if (!Settings)
		{
			Error = TEXT("Selected PCG node has no settings interface.");
			return false;
		}
		Settings->Modify();
		if (Name == TEXT("pcg_manual_override_select"))
		{
			Settings->SetTemporaryManualEditingEnabled(true);
			Settings->SetMarkedForManualEditing(true);
		}
		else if (Name == TEXT("pcg_manual_override_exclude"))
		{
			Settings->SetEnabled(false);
			Settings->SetMarkedForManualEditing(true);
		}
		else
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			Arguments->TryGetObjectField(TEXT("properties"), Properties);
			TArray<FString> Changed;
			if (!PatchEditableProperties(Settings, Properties ? *Properties : nullptr, Changed, Error)) return false;
			Settings->SetMarkedForManualEditing(true);
			OpReceipt->SetNumberField(TEXT("changed_property_count"), Changed.Num());
		}
		Graph->Modify();
		Graph->MarkPackageDirty();
		OpReceipt->SetStringField(TEXT("node_path"), Node->GetPathName());
		OpReceipt->SetBoolField(TEXT("enabled"), Settings->bEnabled);
		OpReceipt->SetBoolField(TEXT("marked_for_manual_editing"), Settings->IsMarkedForManualEditing());
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_manual_override_diff"))
	{
		UPCGNode* Node = ResolveNode(Graph, Arguments, Error);
		if (!Node || !Node->GetSettingsInterface()) return false;
		const UPCGSettingsInterface* Settings = Node->GetSettingsInterface();
		TSharedRef<FJsonObject> Current = MakeShared<FJsonObject>();
		Current->SetBoolField(TEXT("enabled"), Settings->bEnabled);
		Current->SetBoolField(TEXT("marked_for_manual_editing"), Settings->IsMarkedForManualEditing());
		Current->SetObjectField(TEXT("properties"), EditableProperties(const_cast<UPCGSettingsInterface*>(Settings)));
		Out->SetObjectField(TEXT("current"), Current);
		const TSharedPtr<FJsonObject>* Baseline = nullptr;
		const bool bHasBaseline = Arguments->TryGetObjectField(TEXT("baseline"), Baseline) && Baseline && Baseline->IsValid();
		Out->SetBoolField(TEXT("baseline_supplied"), bHasBaseline);
		if (bHasBaseline)
		{
			FString CurrentText, BaselineText;
			FJsonSerializer::Serialize(Current, TJsonWriterFactory<>::Create(&CurrentText));
			FJsonSerializer::Serialize((*Baseline).ToSharedRef(), TJsonWriterFactory<>::Create(&BaselineText));
			Out->SetBoolField(TEXT("different"), CurrentText != BaselineText);
		}
	}
	else if (Name == TEXT("pcg_complex_attribute_schema_create") || Name == TEXT("pcg_complex_attribute_array_constant_add")
		|| Name == TEXT("pcg_complex_attribute_struct_constant_add") || Name == TEXT("pcg_complex_attribute_set_constant_add")
		|| Name == TEXT("pcg_complex_attribute_map_constant_add") || Name == TEXT("pcg_complex_attribute_extract")
		|| Name == TEXT("pcg_complex_attribute_array_operation_add"))
	{
		TArray<FString> Defaults;
		if (Name == TEXT("pcg_complex_attribute_extract"))
		{
			Defaults = {TEXT("/Script/PCG.PCGExtractMemberFromStructSettings"), TEXT("/Script/PCG.PCGExtractAttributeSettings")};
		}
		else if (Name == TEXT("pcg_complex_attribute_array_operation_add"))
		{
			Defaults = {TEXT("/Script/PCG.PCGMetadataArrayOperationSettings")};
		}
		else if (Name == TEXT("pcg_complex_attribute_schema_create"))
		{
			Defaults = {TEXT("/Script/PCG.PCGCreateAttributeSetSettings")};
		}
		else
		{
			Defaults = {TEXT("/Script/PCG.PCGCreateComplexConstantSettings"), TEXT("/Script/PCG.PCGAddComplexConstantSettings")};
		}
		UClass* Class = ResolveSettingsClass(Arguments, Defaults, Error);
		if (!Class) return false;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		Arguments->TryGetObjectField(TEXT("properties"), Properties);
		if (!AddSettingsNode(Graph, Class, Properties ? *Properties : nullptr, OpReceipt, Error)) return false;
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_complex_attribute_pin_validate") || Name == TEXT("pcg_complex_attribute_execution_readback"))
	{
		UPCGNode* Node = ResolveNode(Graph, Arguments, Error);
		if (!Node || !Node->GetSettings()) return false;
		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (const UPCGPin* Pin : Node->GetInputPins()) if (Pin) Inputs.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
		for (const UPCGPin* Pin : Node->GetOutputPins()) if (Pin) Outputs.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
		Out->SetArrayField(TEXT("input_pins"), Inputs);
		Out->SetArrayField(TEXT("output_pins"), Outputs);
		Out->SetBoolField(TEXT("settings_resolved"), true);
		Out->SetObjectField(TEXT("settings_properties"), EditableProperties(Node->GetSettings()));
	}
	else if (Name == TEXT("pcg_embedded_subgraph_node_add"))
	{
		UPCGGraph* Embedded = ResolveEmbeddedGraph(Graph, Arguments, Error);
		if (!Embedded) return false;
		UClass* Class = ResolveSettingsClass(Arguments, {TEXT("/Script/PCG.PCGTrivialSettings")}, Error);
		if (!Class) return false;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		Arguments->TryGetObjectField(TEXT("properties"), Properties);
		if (!AddSettingsNode(Embedded, Class, Properties ? *Properties : nullptr, OpReceipt, Error)) return false;
		Graph->MarkPackageDirty();
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_embedded_subgraph_interface_set"))
	{
		UPCGGraph* Embedded = ResolveEmbeddedGraph(Graph, Arguments, Error);
		if (!Embedded) return false;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		Arguments->TryGetObjectField(TEXT("properties"), Properties);
		TArray<FString> Changed;
		if (!PatchEditableProperties(Embedded, Properties ? *Properties : nullptr, Changed, Error)) return false;
		Embedded->MarkPackageDirty();
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_embedded_subgraph_inline") || Name == TEXT("pcg_embedded_subgraph_extract"))
	{
		UPCGGraph* Embedded = ResolveEmbeddedGraph(Graph, Arguments, Error);
		if (!Embedded) return false;
		if (Name == TEXT("pcg_embedded_subgraph_inline"))
		{
			int32 Added = 0;
			for (UPCGNode* SourceNode : Embedded->GetNodes())
			{
				if (!SourceNode || !SourceNode->GetSettings()) continue;
				UPCGSettings* Copy = nullptr;
				if (Graph->AddNodeCopy(SourceNode->GetSettings(), Copy)) ++Added;
			}
			OpReceipt->SetNumberField(TEXT("inlined_node_count"), Added);
		}
		else
		{
			TMap<UPCGGraph*, UPCGGraph*> Duplicates;
			Graph->DuplicateEmbeddedSubgraphs({Embedded}, Duplicates);
			UPCGGraph* Copy = Duplicates.FindRef(Embedded);
			if (!Copy) { Error = TEXT("DuplicateEmbeddedSubgraphs did not return an extracted copy."); return false; }
			FString ExtractedName;
			if (Arguments->TryGetStringField(TEXT("name"), ExtractedName) && !ExtractedName.IsEmpty()) Copy->Rename(*ExtractedName, Graph, REN_DontCreateRedirectors);
			OpReceipt->SetStringField(TEXT("extracted_embedded_path"), Copy->GetPathName());
		}
		Graph->Modify();
		Graph->MarkPackageDirty();
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_parameter_hierarchy_category_create") || Name == TEXT("pcg_parameter_hierarchy_group_create")
		|| Name == TEXT("pcg_parameter_hierarchy_sort") || Name == TEXT("pcg_parameter_hierarchy_description_set"))
	{
#if !SOMOLMCP_PCG_HAS_PARAM_HIERARCHY
		Error = TEXT("NOT_AVAILABLE_ON_ENGINE: PCG user-parameter hierarchy editing needs the "
					 "StructUtilsEditor hierarchy types added in UE 5.8.");
		return false;
#else
		UPropertyBagHierarchyRoot* Root = Cast<UPropertyBagHierarchyRoot>(Graph->GetUserParameterHierarchyRoot());
		if (!Root) { Error = TEXT("PCG graph has no UPropertyBagHierarchyRoot."); return false; }
		Root->Modify();
		FString ItemName;
		Arguments->TryGetStringField(TEXT("name"), ItemName);
		if (Name == TEXT("pcg_parameter_hierarchy_category_create"))
		{
			if (ItemName.IsEmpty()) { Error = TEXT("name is required."); return false; }
			UPropertyBagHierarchyCategory* Category = Root->AddChild<UPropertyBagHierarchyCategory>();
			Category->SetCategoryName(FName(*ItemName));
		}
		else if (Name == TEXT("pcg_parameter_hierarchy_group_create"))
		{
			if (ItemName.IsEmpty()) { Error = TEXT("name is required."); return false; }
			UPropertyBagHierarchySection* Section = NewObject<UPropertyBagHierarchySection>(Root);
			Section->SetSectionName(FName(*ItemName));
			Section->SetFlags(RF_Transactional);
			Root->GetSectionDataMutable().Add(Section);
		}
		else if (Name == TEXT("pcg_parameter_hierarchy_sort"))
		{
			Root->StableSortChildren([](const UHierarchyElement& A, const UHierarchyElement& B) { return A.ToString() < B.ToString(); }, true);
			Root->GetSectionDataMutable().StableSort([](const UHierarchySection& A, const UHierarchySection& B) { return A.ToString() < B.ToString(); });
		}
		else
		{
			FString Description;
			Arguments->TryGetStringField(TEXT("description"), Description);
			UHierarchyElement* Element = FindHierarchyElement(Root, ItemName);
			if (!Element)
			{
				for (const TObjectPtr<UHierarchySection>& Section : Root->GetSectionData())
				{
					if (Section && (Section->GetSectionName().ToString().Equals(ItemName, ESearchCase::IgnoreCase)
						|| Section->ToString().Equals(ItemName, ESearchCase::IgnoreCase)))
					{
						Element = Section.Get();
						break;
					}
				}
			}
			if (!Element) { Error = FString::Printf(TEXT("Hierarchy element was not found: %s"), *ItemName); return false; }
			if (UHierarchySection* Section = Cast<UHierarchySection>(Element)) Section->SetTooltip(FText::FromString(Description));
			else
			{
				FProperty* Tooltip = FindFProperty<FProperty>(Element->GetClass(), TEXT("Tooltip"));
				if (!Tooltip || !Tooltip->ImportText_Direct(*Description, Tooltip->ContainerPtrToValuePtr<void>(Element), Element, PPF_None))
				{ Error = TEXT("Hierarchy element does not expose a writable Tooltip."); return false; }
			}
		}
		Root->OnHierarchyModified.Broadcast();
		Graph->ForceNotificationForEditor(EPCGChangeType::Structural);
		Graph->MarkPackageDirty();
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
#endif
	}
	else if (Name == TEXT("pcg_gpu_primitive_data_spawner_configure") || Name == TEXT("pcg_hlsl_output_attribute_create"))
	{
		const TArray<FString> Defaults = Name == TEXT("pcg_gpu_primitive_data_spawner_configure")
			? TArray<FString>{TEXT("/Script/PCG.PCGStaticMeshSpawnerSettings")}
			: TArray<FString>{TEXT("/Script/PCG.PCGCustomHLSLSettings")};
		UClass* Class = ResolveSettingsClass(Arguments, Defaults, Error);
		if (!Class) return false;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		Arguments->TryGetObjectField(TEXT("properties"), Properties);
		if (!AddSettingsNode(Graph, Class, Properties ? *Properties : nullptr, OpReceipt, Error)) return false;
		OpReceipt->SetBoolField(TEXT("gpu_capable_settings"), true);
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_actorless_runtime_generation_configure"))
	{
		Graph->Modify();
		FProperty* Property = FindFProperty<FProperty>(Graph->GetClass(), TEXT("bUseActorComponentlessGeneration"));
		if (!Property)
		{
			Error = TEXT("UE 5.8 graph does not expose bUseActorComponentlessGeneration.");
			return false;
		}
		bool bEnabled = true;
		Arguments->TryGetBoolField(TEXT("enabled"), bEnabled);
		const FString Value = bEnabled ? TEXT("True") : TEXT("False");
		if (!Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(Graph), Graph, PPF_None))
		{
			Error = TEXT("Failed to set actor-componentless generation on the PCG graph.");
			return false;
		}
		Graph->PostEditChange();
		Graph->MarkPackageDirty();
		const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property);
		OpReceipt->SetBoolField(TEXT("actor_componentless_generation"), BoolProperty && BoolProperty->GetPropertyValue_InContainer(Graph));
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_actorless_runtime_generation_validate"))
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(Graph->GetClass(), TEXT("bUseActorComponentlessGeneration"));
		Out->SetBoolField(TEXT("actor_componentless_generation"), Property && Property->GetPropertyValue_InContainer(Graph));
		Out->SetBoolField(TEXT("graph_usage_valid"), Graph->GetGraphUsage() != EPCGGraphUsage::Standard);
		Out->SetStringField(TEXT("graph_usage"), StaticEnum<EPCGGraphUsage>()->GetNameStringByValue(static_cast<int64>(Graph->GetGraphUsage())));
	}
	else if (Name == TEXT("pcg_scene_capture_input_transform_set"))
	{
		UPCGNode* Node = nullptr;
		for (UPCGNode* Candidate : Graph->GetNodes())
		{
			if (Candidate && Cast<UPCGSceneCaptureSettings>(Candidate->GetSettings())) { Node = Candidate; break; }
		}
		UPCGSceneCaptureSettings* Settings = Node ? Cast<UPCGSceneCaptureSettings>(Node->GetSettings()) : nullptr;
		if (!Settings)
		{
			UPCGSettings* Base = nullptr;
			Node = Graph->AddNodeOfType(UPCGSceneCaptureSettings::StaticClass(), Base);
			Settings = Cast<UPCGSceneCaptureSettings>(Base);
		}
		if (!Node || !Settings) { Error = TEXT("Failed to create UPCGSceneCaptureSettings."); return false; }
		Settings->Modify();
		Settings->OrientationMode = EPCGSceneCaptureOrientationMode::Explicit;
		const TArray<TSharedPtr<FJsonValue>>* Location = nullptr;
		if (Arguments->TryGetArrayField(TEXT("location"), Location) && Location && Location->Num() == 3)
			Settings->CaptureLocation = FVector((*Location)[0]->AsNumber(), (*Location)[1]->AsNumber(), (*Location)[2]->AsNumber());
		const TArray<TSharedPtr<FJsonValue>>* Extents = nullptr;
		if (Arguments->TryGetArrayField(TEXT("half_extents"), Extents) && Extents && Extents->Num() == 3)
			Settings->CaptureHalfExtents = FVector((*Extents)[0]->AsNumber(), (*Extents)[1]->AsNumber(), (*Extents)[2]->AsNumber());
		double Yaw = 0.0, Pitch = -90.0, Roll = 0.0;
		Arguments->TryGetNumberField(TEXT("yaw"), Yaw);
		Arguments->TryGetNumberField(TEXT("pitch"), Pitch);
		Arguments->TryGetNumberField(TEXT("roll"), Roll);
		Settings->CaptureRotation = FRotator(Pitch, Yaw, Roll).Quaternion();
		Graph->MarkPackageDirty();
		OpReceipt->SetStringField(TEXT("scene_capture_node"), Node->GetPathName());
		if (!SaveGraph(Graph, OpReceipt, Error)) return false;
	}
	else if (Name == TEXT("pcg_level_graph_execute") || Name == TEXT("pcg_gpu_runtime_scatter_benchmark"))
	{
		if (!GEditor) { Error = TEXT("GEditor is unavailable."); return false; }
		UPCGSubsystem* Subsystem = UPCGSubsystem::GetSubsystemForCurrentWorld();
		if (!Subsystem) { Error = TEXT("No active editor-world UPCGSubsystem."); return false; }
		if (Name == TEXT("pcg_level_graph_execute") && Graph->GetGraphUsage() != EPCGGraphUsage::Level)
		{
			Error = TEXT("pcg_level_graph_execute requires graph usage Level.");
			return false;
		}
#if SOMOLMCP_PCG_HAS_WORLDOBJECT_EXEC_SOURCE
		FPCGDefaultWorldObjectExecutionSourceParams Params;
		Params.GraphInterface = Graph;
		Params.WorldObject = Subsystem;
		Params.bFireAndForgetExecution = true;
		UPCGDefaultWorldObjectExecutionSource* Source = IPCGBaseSubsystem::CreateExecutionSource<UPCGDefaultWorldObjectExecutionSource>(Params);
		if (!Source) { Error = TEXT("Failed to create UE 5.8 PCG default world execution source."); return false; }
#else
		// 5.7 and earlier have UPCGDefaultExecutionSource, whose params carry neither
		// WorldObject nor bFireAndForgetExecution, so there is no equivalent call.
		// Failing with the capability contract is better than silently substituting a
		// different execution path that would not do what the caller asked.
		Error = TEXT("NOT_AVAILABLE_ON_ENGINE: this needs the PCG world-object execution "
					 "source added in UE 5.8; this engine only has the base execution source.");
		return false;
#endif
		Source->Generate();
		Out->SetStringField(TEXT("status"), TEXT("running"));
		Out->SetNumberField(TEXT("task_id"), static_cast<double>(Source->GetCurrentGenerationTask()));
		Out->SetStringField(TEXT("execution_source"), Source->GetPathName());
		Summary = FString::Printf(TEXT("%s scheduled through the UE 5.8 PCG world subsystem."), *Name);
		return true;
	}
	else if (Name == TEXT("pcg_query_toolmode_execute") || Name == TEXT("pcg_isolate_toolmode_execute"))
	{
		if (!GEditor || !GEngine) { Error = TEXT("Editor engine is unavailable."); return false; }
		UWorld* World = GEditor->GetEditorWorldContext().World();
		const FString ToolId = Name == TEXT("pcg_query_toolmode_execute") ? TEXT("QueryTool") : TEXT("IsolateTool");
		const FEditorModeID ModeId(TEXT("EM_PCGEditorMode"));
		GLevelEditorModeTools().ActivateMode(ModeId, false);
		UEdMode* Mode = GLevelEditorModeTools().GetActiveScriptableMode(ModeId);
		UEditorInteractiveToolsContext* ToolsContext = Mode ? Mode->GetInteractiveToolsContext() : nullptr;
		UInteractiveToolManager* Manager = ToolsContext ? ToolsContext->ToolManager : nullptr;
		if (!ToolsContext || !Manager)
		{
			Error = TEXT("PCG editor mode did not expose an interactive tools context.");
			return false;
		}
		if (Manager->HasActiveTool(EToolSide::Left))
		{
			ToolsContext->EndTool(EToolShutdownType::Cancel);
		}
		// PCG editor tools finish activation on the next editor tick. Selecting the
		// registered builder is the synchronous acceptance point; do not report a
		// false failure merely because the tool instance has not been built yet.
		if (!Manager->SelectActiveToolType(EToolSide::Left, ToolId))
		{
			Error = FString::Printf(TEXT("PCG editor mode has no registered tool named %s."), *ToolId);
			return false;
		}
		Manager->ActivateTool(EToolSide::Left);
		GEngine->Exec(World, *FString::Printf(TEXT("pcg.tool.SetGraph %s"), *Graph->GetPathName()));

		UInteractiveTool* ActiveTool = Manager->GetActiveTool(EToolSide::Left);
		const FString ActiveToolName = Manager->GetActiveToolName(EToolSide::Left);
		if (!ActiveTool || ActiveToolName != ToolId)
		{
			Out->SetStringField(TEXT("status"), TEXT("queued"));
			Out->SetStringField(TEXT("tool_id"), ToolId);
			Out->SetBoolField(TEXT("activation_deferred"), true);
			Out->SetStringField(TEXT("active_tool_name"), ActiveToolName);
			Out->SetStringField(TEXT("interaction_required"), TEXT("viewport_click_after_activation"));
			Summary = FString::Printf(TEXT("PCG %s activation is queued for the next editor tick."), *ToolId);
			return true;
		}
		Out->SetStringField(TEXT("status"), TEXT("running"));
		Out->SetStringField(TEXT("tool_id"), ToolId);
		Out->SetStringField(TEXT("active_tool_name"), ActiveToolName);
		Out->SetStringField(TEXT("active_tool_class"), ActiveTool->GetClass()->GetPathName());
		Out->SetStringField(TEXT("interaction_required"), TEXT("viewport_click"));
		Summary = FString::Printf(TEXT("PCG %s is active and waiting for viewport input."), *ToolId);
		return true;
	}
	else
	{
		Error = FString::Printf(TEXT("No UE 5.8 native PCG route for %s."), *Name);
		return false;
	}

	OpReceipt->SetObjectField(TEXT("post_readback"), GraphSnapshot(Graph));
	OpReceipt->SetStringField(TEXT("status"), TEXT("completed"));
	Out->SetObjectField(TEXT("receipt"), OpReceipt);
	Out->SetStringField(TEXT("status"), TEXT("completed"));
	Summary = FString::Printf(TEXT("%s completed through UE 5.8 public PCG APIs."), *Name);
	return true;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path."))},
		{TEXT("node_title"), FSololmcpSchemaBuilder::String(TEXT("Optional exact node title."))},
		{TEXT("node_index"), FSololmcpSchemaBuilder::Integer(TEXT("Optional zero-based node index."))},
		{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Embedded subgraph name."))},
		{TEXT("embedded_name"), FSololmcpSchemaBuilder::String(TEXT("Embedded subgraph name."))},
		{TEXT("settings_class"), FSololmcpSchemaBuilder::String(TEXT("Optional registered UPCGSettings class path override."))},
		{TEXT("properties"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Editable property patch applied with Unreal reflection."))},
		{TEXT("baseline"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional manual override baseline for diff."))},
		{TEXT("description"), FSololmcpSchemaBuilder::String(TEXT("Hierarchy item description or tooltip."))},
		{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable or disable the requested feature."))},
		{TEXT("location"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number(TEXT("Coordinate in cm.")), TEXT("Explicit XYZ location."))},
		{TEXT("half_extents"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number(TEXT("Extent in cm.")), TEXT("Explicit XYZ half extents."))},
		{TEXT("yaw"), FSololmcpSchemaBuilder::Number(TEXT("Yaw in degrees."))},
		{TEXT("pitch"), FSololmcpSchemaBuilder::Number(TEXT("Pitch in degrees."))},
		{TEXT("roll"), FSololmcpSchemaBuilder::Number(TEXT("Roll in degrees."))},
		{TEXT("usage"), FSololmcpSchemaBuilder::String(TEXT("standard, asset, or level."), {TEXT("standard"), TEXT("asset"), TEXT("level")})},
		{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Native PCG 5.8 receipt to validate."))}
	});
}
#endif
}

void RegisterPcg58NativeTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_HAS_PCG
	static const TCHAR* Names[] = {
		TEXT("pcg_manual_editing_capability_probe"), TEXT("pcg_manual_override_list"),
		TEXT("pcg_manual_override_restore"), TEXT("pcg_manual_override_commit"),
		TEXT("pcg_manual_override_receipt_validate"), TEXT("pcg_embedded_subgraph_create"),
		TEXT("pcg_embedded_subgraph_open"), TEXT("pcg_embedded_subgraph_compile_validate"),
		TEXT("pcg_embedded_subgraph_save_reload"), TEXT("pcg_graph_usage_set"),
		TEXT("pcg_parameter_hierarchy_readback"),
		TEXT("pcg_manual_override_select"), TEXT("pcg_manual_override_exclude"), TEXT("pcg_manual_override_modify"),
		TEXT("pcg_manual_override_diff"), TEXT("pcg_complex_attribute_schema_create"),
		TEXT("pcg_complex_attribute_array_constant_add"), TEXT("pcg_complex_attribute_struct_constant_add"),
		TEXT("pcg_complex_attribute_set_constant_add"), TEXT("pcg_complex_attribute_map_constant_add"),
		TEXT("pcg_complex_attribute_extract"), TEXT("pcg_complex_attribute_array_operation_add"),
		TEXT("pcg_complex_attribute_pin_validate"), TEXT("pcg_complex_attribute_execution_readback"),
		TEXT("pcg_embedded_subgraph_node_add"), TEXT("pcg_embedded_subgraph_interface_set"),
		TEXT("pcg_embedded_subgraph_inline"), TEXT("pcg_embedded_subgraph_extract"),
		TEXT("pcg_parameter_hierarchy_category_create"), TEXT("pcg_parameter_hierarchy_group_create"),
		TEXT("pcg_parameter_hierarchy_sort"), TEXT("pcg_parameter_hierarchy_description_set"),
		TEXT("pcg_gpu_primitive_data_spawner_configure"), TEXT("pcg_gpu_runtime_scatter_benchmark"),
		TEXT("pcg_actorless_runtime_generation_configure"), TEXT("pcg_actorless_runtime_generation_validate"),
		TEXT("pcg_scene_capture_input_transform_set"), TEXT("pcg_hlsl_output_attribute_create"),
		TEXT("pcg_level_graph_execute"), TEXT("pcg_query_toolmode_execute"), TEXT("pcg_isolate_toolmode_execute")
	};
	for (const TCHAR* NamePtr : Names)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = FString::Printf(TEXT("UE 5.8 native PCG transaction: %s"), *Name);
		Def.InputSchema = Pcg58Native::Schema();
		const bool bMutation = !(Name.EndsWith(TEXT("_probe")) || Name.EndsWith(TEXT("_list")) || Name.EndsWith(TEXT("_open")) || Name.EndsWith(TEXT("_validate")) || Name.EndsWith(TEXT("_readback")));
		Def.CacheTtlSeconds = bMutation ? 0 : 2;
		Def.Execute = [Name](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return Pcg58Native::Execute(Name, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
#endif
}
}
#else
namespace UE::SOMOLMCP
{
void RegisterPcg58NativeTools(FSololmcpToolRegistry&)
{
}
}
#endif
