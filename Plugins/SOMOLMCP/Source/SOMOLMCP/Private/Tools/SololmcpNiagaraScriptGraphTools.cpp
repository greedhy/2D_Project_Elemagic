// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — Niagara Script Graph node-level editing tools (P1-4).
//
// New tools added (niagara_script_* prefix):
//   niagara_script_add_node         — Spawn a UNiagaraNode_X subclass into a UNiagaraGraph.
//   niagara_script_connect_pins     — Try-create a connection between two pins on the schema.
//   niagara_script_disconnect_pins  — Break a connection between two pins on the schema.
//   niagara_script_set_node_property — ImportText_Direct on a UPROPERTY of a graph node.
//   niagara_script_layout_graph     — Auto-layout fallback (grid layout) over all nodes.
//   niagara_script_delete_node      — Delete one graph node with a disposable-target guard.
//   niagara_graph_restore_snapshot_full — Restore layout, links, pin defaults, and delete extra nodes.
//
// IMPORTANT: This file is intentionally SELF-CONTAINED. It does NOT modify
// SololmcpDomainTools.cpp / SololmcpToolRegistry.cpp / SOMOLMCP.Build.cs.
// Helpers that exist as file-static / anonymous-namespace functions in
// SololmcpDomainTools.cpp / SololmcpBlueprintComponentTools.cpp are
// re-implemented here in this file's anonymous namespace.
//
// Build dependencies verified in SOMOLMCP.Build.cs:
//   "Niagara", "NiagaraCore", "NiagaraEditor"  -- present.
//   "NiagaraEditorWidgets" is NOT in deps; we avoid it. (Auto-layout falls
//   back to a manual grid; FNiagaraEditorUtilities::AutoLayout is referenced
//   under #if 0 with a TODO so a future maintainer can flip it on if/when
//   that module dep is added.)

#include "Tools/SololmcpToolRegistry.h"
#include "SOMOLMCP.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

// --- Niagara runtime + editor headers ---
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMessages.h"
#include "NiagaraShared.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemImpl.h"
#include "NiagaraTypes.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEditorCommon.h"
#include "Runtime/Launch/Resources/Version.h"

// Concrete node classes — these live in the NiagaraEditor module.
// TODO(P1-4): if any of these headers are private to NiagaraEditor in this UE
// 5.7 branch, the corresponding `case` in CreateNodeOfShortClass will need to
// fall back to NewObject<UNiagaraNode>() with a base class.
#include "NiagaraNodeOp.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeReadDataSet.h"
#include "NiagaraNodeWriteDataSet.h"

// Graph editor + schema
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_Niagara.h"

// JSON / property
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

namespace UE::SOMOLMCP
{
namespace
{
	// ===================================================================
	// Local helpers (kept self-contained per-file to avoid touching the
	// massive SololmcpDomainTools.cpp).
	// ===================================================================

	UNiagaraScript* LoadNiagaraScriptLocal(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
	{
		UObject* Asset = Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UNiagaraScript* Script = Cast<UNiagaraScript>(Asset);
		if (!Script)
		{
			OutError = TEXT("Asset is not a UNiagaraScript.");
		}
		return Script;
	}

	UNiagaraGraph* GetGraphFromScriptLocal(UNiagaraScript* Script)
	{
		if (!Script)
		{
			return nullptr;
		}
		UNiagaraScriptSourceBase* SourceBase = Script->GetLatestSource();
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
		if (!Source)
		{
			return nullptr;
		}
		return Source->NodeGraph;
	}

	bool ResolveScriptAndGraph(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		const TSharedRef<FJsonObject>& OutStructured,
		UNiagaraScript*& OutScript,
		UNiagaraGraph*& OutGraph,
		FString& OutError)
	{
		FString AssetPath;
		if (!Arguments->TryGetStringField(TEXT("script_path"), AssetPath))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("script_path"));
			OutError = TEXT("Missing script_path.");
			return false;
		}
		OutScript = LoadNiagaraScriptLocal(Context.Services, AssetPath, OutError);
		if (!OutScript)
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			return false;
		}
		OutGraph = GetGraphFromScriptLocal(OutScript);
		if (!OutGraph)
		{
			SololmcpError::NotFound(OutStructured, TEXT("UNiagaraGraph (script source)"));
			OutError = TEXT("Niagara script has no valid UNiagaraGraph (UNiagaraScriptSource).");
			return false;
		}
		return true;
	}

	// We allow callers to refer to a node by either:
	//   (a) the NodeGuid string (canonical), or
	//   (b) the UEdGraphNode UObject GetName() (fallback, matches list_script_graph_nodes "name" field).
	UEdGraphNode* FindNodeByIdLocal(UNiagaraGraph* Graph, const FString& NodeId)
	{
		if (!Graph || NodeId.IsEmpty())
		{
			return nullptr;
		}
		FGuid AsGuid;
		const bool bIsGuid = FGuid::Parse(NodeId, AsGuid);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (bIsGuid && Node->NodeGuid == AsGuid)
			{
				return Node;
			}
			if (Node->GetName().Equals(NodeId, ESearchCase::IgnoreCase))
			{
				return Node;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection RequiredDir, bool bRequireDir)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (bRequireDir && Pin->Direction != RequiredDir)
			{
				continue;
			}
			if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) ||
				Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool PinsAreLinked(const UEdGraphPin* A, const UEdGraphPin* B)
	{
		return A && B && A->GetOwningNodeUnchecked() && B->GetOwningNodeUnchecked() &&
			A->LinkedTo.Contains(B) && B->LinkedTo.Contains(A);
	}

	UEdGraphNode* GetPinOwningNodeSafe(const UEdGraphPin* Pin)
	{
		return Pin ? Pin->GetOwningNodeUnchecked() : nullptr;
	}

	bool IsPinOwnedByNode(const UEdGraphPin* Pin, const UEdGraphNode* ExpectedNode)
	{
		return Pin && ExpectedNode && GetPinOwningNodeSafe(Pin) == ExpectedNode &&
			ExpectedNode->Pins.Contains(const_cast<UEdGraphPin*>(Pin));
	}

	FString NiagaraLinkKey(const FString& FromNodeId, const FString& FromPinName, const FString& ToNodeId, const FString& ToPinName)
	{
		return FString::Printf(TEXT("%s:%s>%s:%s"), *FromNodeId, *FromPinName, *ToNodeId, *ToPinName);
	}

	FString NiagaraLinkKey(const UEdGraphPin* FromPin, const UEdGraphPin* ToPin)
	{
		UEdGraphNode* FromOwner = GetPinOwningNodeSafe(FromPin);
		UEdGraphNode* ToOwner = GetPinOwningNodeSafe(ToPin);
		if (!FromPin || !ToPin || !FromOwner || !ToOwner)
		{
			return FString();
		}
		return NiagaraLinkKey(
			FromOwner->NodeGuid.ToString(),
			FromPin->PinName.ToString(),
			ToOwner->NodeGuid.ToString(),
			ToPin->PinName.ToString());
	}

	FString JsonArrayHash(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		FString Serialized;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		FJsonSerializer::Serialize(Values, Writer);
		return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Serialized));
	}

	TSharedRef<FJsonObject> NiagaraPinToExplainJson(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Pin)
		{
			return Result;
		}
		Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Result->SetStringField(TEXT("display_name"), Pin->GetDisplayName().ToString());
		Result->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		Result->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
		Result->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		Result->SetStringField(TEXT("autogenerated_default_value"), Pin->AutogeneratedDefaultValue);
		Result->SetNumberField(TEXT("linked_count"), Pin->LinkedTo.Num());
		if (Pin->DefaultObject)
		{
			Result->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Result->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> LinkedPins;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
			if (!LinkedPin || !LinkedOwner)
			{
				continue;
			}
			TSharedRef<FJsonObject> LinkedJson = MakeShared<FJsonObject>();
			LinkedJson->SetStringField(TEXT("node_id"), LinkedOwner->NodeGuid.ToString());
			LinkedJson->SetStringField(TEXT("node_name"), LinkedOwner->GetName());
			LinkedJson->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
			LinkedPins.Add(MakeShared<FJsonValueObject>(LinkedJson));
		}
		Result->SetArrayField(TEXT("linked_pins"), LinkedPins);
		return Result;
	}

	TSharedRef<FJsonObject> NiagaraNodeToExplainJson(UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Node)
		{
			return Result;
		}
		const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : TEXT("Unknown");
		Result->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
		Result->SetStringField(TEXT("name"), Node->GetName());
		Result->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Result->SetStringField(TEXT("class"), ClassName);
		Result->SetNumberField(TEXT("x"), Node->NodePosX);
		Result->SetNumberField(TEXT("y"), Node->NodePosY);
		Result->SetBoolField(TEXT("is_module"), ClassName.Contains(TEXT("FunctionCall")) || ClassName.Contains(TEXT("Module")));
		Result->SetBoolField(TEXT("is_emitter_node"), ClassName.Contains(TEXT("Emitter")));
		Result->SetBoolField(TEXT("is_renderer_node"), ClassName.Contains(TEXT("Renderer")));
		Result->SetBoolField(TEXT("is_user_parameter"), ClassName.Contains(TEXT("Input")));

		TArray<TSharedPtr<FJsonValue>> Pins;
		TArray<TSharedPtr<FJsonValue>> References;
		TSet<FString> SeenReferences;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			Pins.Add(MakeShared<FJsonValueObject>(NiagaraPinToExplainJson(Pin)));
			if (Pin->PinType.PinSubCategoryObject.IsValid())
			{
				const FString RefPath = Pin->PinType.PinSubCategoryObject->GetPathName();
				if (!RefPath.IsEmpty() && !SeenReferences.Contains(RefPath))
				{
					SeenReferences.Add(RefPath);
					TSharedRef<FJsonObject> RefJson = MakeShared<FJsonObject>();
					RefJson->SetStringField(TEXT("path"), RefPath);
					RefJson->SetStringField(TEXT("source"), FString::Printf(TEXT("pin:%s"), *Pin->PinName.ToString()));
					References.Add(MakeShared<FJsonValueObject>(RefJson));
				}
			}
		}
		Result->SetArrayField(TEXT("pins"), Pins);
		Result->SetArrayField(TEXT("referenced_assets"), References);
		return Result;
	}

	FString BuildNiagaraGraphCanonicalSignature(UNiagaraGraph* Graph)
	{
		TArray<FString> NodeRows;
		TArray<FString> LinkRows;
		TArray<FString> PinRows;
		if (!Graph)
		{
			return FString();
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : TEXT("Unknown");
			NodeRows.Add(FString::Printf(TEXT("N|%s|%s|%s|%d|%d"),
				*Node->NodeGuid.ToString(),
				*Node->GetName(),
				*ClassName,
				Node->NodePosX,
				Node->NodePosY));
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				FString DefaultObjectPath;
				if (Pin->DefaultObject)
				{
					DefaultObjectPath = Pin->DefaultObject->GetPathName();
				}
				PinRows.Add(FString::Printf(TEXT("P|%s|%s|%s|%s|%s|%s"),
					*Node->NodeGuid.ToString(),
					*Pin->PinName.ToString(),
					Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"),
					*Pin->DefaultValue,
					*Pin->AutogeneratedDefaultValue,
					*DefaultObjectPath));
				if (Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !GetPinOwningNodeSafe(LinkedPin))
					{
						continue;
					}
					LinkRows.Add(NiagaraLinkKey(Pin, LinkedPin));
				}
			}
		}
		NodeRows.Sort();
		LinkRows.Sort();
		PinRows.Sort();
		return FString::Join(NodeRows, TEXT("\n")) +
			TEXT("\n--pins--\n") + FString::Join(PinRows, TEXT("\n")) +
			TEXT("\n--links--\n") + FString::Join(LinkRows, TEXT("\n"));
	}

	FString BuildNiagaraGraphHash(UNiagaraGraph* Graph)
	{
		const FString Canonical = BuildNiagaraGraphCanonicalSignature(Graph);
		return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Canonical));
	}

	TSharedRef<FJsonObject> BuildNiagaraGraphSnapshotJson(UNiagaraScript* Script, UNiagaraGraph* Graph)
	{
		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Links;
		TSet<FString> SeenLinks;
		if (Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				Nodes.Add(MakeShared<FJsonValueObject>(NiagaraNodeToExplainJson(Node)));
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
						if (!LinkedPin || !LinkedOwner)
						{
							continue;
						}
						const FString LinkId = NiagaraLinkKey(Pin, LinkedPin);
						if (LinkId.IsEmpty() || SeenLinks.Contains(LinkId))
						{
							continue;
						}
						SeenLinks.Add(LinkId);
						TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
						LinkJson->SetStringField(TEXT("id"), LinkId);
						LinkJson->SetStringField(TEXT("from"), Node->NodeGuid.ToString());
						LinkJson->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
						LinkJson->SetStringField(TEXT("to"), LinkedOwner->NodeGuid.ToString());
						LinkJson->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
						LinkJson->SetStringField(TEXT("kind"), TEXT("data"));
						Links.Add(MakeShared<FJsonValueObject>(LinkJson));
					}
				}
			}
		}
		Snapshot->SetStringField(TEXT("schema"), TEXT("somol.niagara_graph_snapshot.v1"));
		Snapshot->SetStringField(TEXT("asset_path"), Script ? Script->GetPathName() : FString());
		Snapshot->SetStringField(TEXT("script_path"), Script ? Script->GetPathName() : FString());
		Snapshot->SetStringField(TEXT("graph_hash"), BuildNiagaraGraphHash(Graph));
		Snapshot->SetNumberField(TEXT("node_count"), Nodes.Num());
		Snapshot->SetNumberField(TEXT("link_count"), Links.Num());
		Snapshot->SetArrayField(TEXT("nodes"), Nodes);
		Snapshot->SetArrayField(TEXT("links"), Links);
		return Snapshot;
	}

	TSharedRef<FJsonObject> MakeNiagaraTargetBinding(UNiagaraScript* Script, UNiagaraGraph* Graph)
	{
		TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("script_path"), Script ? Script->GetPathName() : FString());
		Binding->SetStringField(TEXT("asset_path"), Script ? Script->GetPathName() : FString());
		Binding->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : FString());
		Binding->SetStringField(TEXT("asset_class"), Script && Script->GetClass() ? Script->GetClass()->GetName() : FString());
		Binding->SetStringField(TEXT("package_path"), Script && Script->GetPackage() ? Script->GetPackage()->GetName() : FString());
		return Binding;
	}

	TSharedRef<FJsonObject> MakeNiagaraSnapshotSummary(const TSharedRef<FJsonObject>& Snapshot)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("graph_hash"), Snapshot->GetStringField(TEXT("graph_hash")));
		Summary->SetNumberField(TEXT("node_count"), Snapshot->GetIntegerField(TEXT("node_count")));
		Summary->SetNumberField(TEXT("link_count"), Snapshot->GetIntegerField(TEXT("link_count")));
		return Summary;
	}

	TSharedRef<FJsonObject> MakeNiagaraGateEvidence(const FString& Phase, const FString& Status, const FString& Tool, const FString& Detail)
	{
		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("phase"), Phase);
		Evidence->SetStringField(TEXT("status"), Status);
		Evidence->SetStringField(TEXT("tool"), Tool);
		Evidence->SetStringField(TEXT("detail"), Detail);
		return Evidence;
	}

	TSharedRef<FJsonObject> MakeNiagaraStructuredFailure(const FString& Code, const FString& Detail, const FString& FailedGate)
	{
		TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetStringField(TEXT("schema"), TEXT("somol.niagara.structured_failure.v1"));
		Failure->SetStringField(TEXT("code"), Code);
		Failure->SetStringField(TEXT("detail"), Detail);
		Failure->SetStringField(TEXT("failed_gate"), FailedGate);
		Failure->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_hermes"));
		Failure->SetStringField(TEXT("retry_policy"), TEXT("fix_asset_then_retry_compile_and_preview_once"));
		return Failure;
	}

	void AttachNiagaraGraphEditReceipt(
		TSharedRef<FJsonObject>& OutStructured,
		UNiagaraScript* Script,
		UNiagaraGraph* Graph,
		const TSharedRef<FJsonObject>& BeforeSnapshot,
		const TSharedRef<FJsonObject>& AfterSnapshot,
		const FString& Operation,
		const FString& ReadbackField,
		const bool bReadbackOk)
	{
		OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.niagara_graph_edit_receipt.v1"));
		OutStructured->SetStringField(TEXT("operation"), Operation);
		OutStructured->SetObjectField(TEXT("target_binding"), MakeNiagaraTargetBinding(Script, Graph));
		OutStructured->SetObjectField(TEXT("pre_edit_graph_summary"), MakeNiagaraSnapshotSummary(BeforeSnapshot));
		OutStructured->SetObjectField(TEXT("graph_summary"), MakeNiagaraSnapshotSummary(AfterSnapshot));
		TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
		Readback->SetStringField(TEXT("field"), ReadbackField);
		Readback->SetBoolField(TEXT("verified"), bReadbackOk);
		Readback->SetStringField(TEXT("tool"), TEXT("niagara_graph_snapshot_readback"));
		OutStructured->SetObjectField(TEXT("post_edit_readback"), Readback);
		OutStructured->SetStringField(TEXT("rollback_hint"), TEXT("Use niagara_graph_restore_snapshot with pre_edit_snapshot on disposable targets, or editor transaction/undo before saving."));
		OutStructured->SetStringField(TEXT("compile_gate"), TEXT("Run niagara_compile_diagnostics before production acceptance."));
		OutStructured->SetObjectField(TEXT("compile_diagnostics_evidence"), MakeNiagaraGateEvidence(
			TEXT("compile_diagnostics"),
			TEXT("required_after_mutation"),
			TEXT("niagara_compile_diagnostics"),
			TEXT("Require compile_passed=true before accepting this graph mutation.")));
		OutStructured->SetObjectField(TEXT("preview_playback_evidence"), MakeNiagaraGateEvidence(
			TEXT("preview_playback"),
			TEXT("required_after_compile"),
			TEXT("niagara_runtime_snapshot or niagara_preview_capture"),
			TEXT("Capture runtime/preview evidence after compile passes before production acceptance.")));
		OutStructured->SetBoolField(TEXT("receipt_gate_complete"), false);
		OutStructured->SetStringField(TEXT("receipt_gate_status"), bReadbackOk ? TEXT("pending_compile_and_preview") : TEXT("failed_readback"));
		OutStructured->SetStringField(TEXT("diagnostic_code"), bReadbackOk ? TEXT("ok") : TEXT("post_edit_readback_failed"));
		if (!bReadbackOk)
		{
			OutStructured->SetObjectField(TEXT("structured_failure"), MakeNiagaraStructuredFailure(
				TEXT("POST_EDIT_READBACK_FAILED"),
				TEXT("The graph mutation did not satisfy the immediate graph snapshot readback check."),
				TEXT("post_edit_readback")));
		}
	}

	bool IsSafeDisposableNiagaraRollbackPath(const FString& AssetPath)
	{
		return AssetPath.StartsWith(TEXT("/Game/SOMOLMCP/DisposableRollbackProof/Niagara"), ESearchCase::IgnoreCase) ||
			AssetPath.StartsWith(TEXT("/Game/SOMOLMCP/DisposableRollbackProof/"), ESearchCase::IgnoreCase) ||
			AssetPath.StartsWith(TEXT("/Game/SOMOLMCP/Disposable"), ESearchCase::IgnoreCase);
	}

	bool MaybeSaveNiagaraScriptLocal(FSololmcpEditorServices& Services, const TSharedRef<FJsonObject>& Arguments, const FString& ScriptPath, TSharedRef<FJsonObject>& OutStructured, FString& OutError)
	{
		const bool bSave = Arguments->HasTypedField<EJson::Boolean>(TEXT("save")) && Arguments->GetBoolField(TEXT("save"));
		OutStructured->SetBoolField(TEXT("save_requested"), bSave);
		if (!bSave)
		{
			return true;
		}
		FString SavePath = ScriptPath;
		int32 SubobjectSeparator = INDEX_NONE;
		if (SavePath.FindChar(TEXT(':'), SubobjectSeparator))
		{
			SavePath = SavePath.Left(SubobjectSeparator);
		}
		OutStructured->SetStringField(TEXT("save_target_path"), SavePath);
		const bool bSaved = Services.SaveAsset(SavePath, true, OutError);
		OutStructured->SetBoolField(TEXT("saved"), bSaved);
		return bSaved;
	}

	UNiagaraSystem* LoadNiagaraSystemLocal(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
	{
		UObject* Asset = Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
		if (!System)
		{
			OutError = TEXT("Asset is not a UNiagaraSystem.");
		}
		return System;
	}

	TSharedRef<FJsonObject> NiagaraVariableTypeJson(const FNiagaraVariable& Variable)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Variable.GetName().ToString());
		Result->SetStringField(TEXT("type"), Variable.GetType().GetName());
		return Result;
	}

	TSharedRef<FJsonObject> BuildNiagaraSystemAuthoringSnapshotJson(UNiagaraSystem* System)
	{
		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Emitters;
		TArray<TSharedPtr<FJsonValue>> UserParameters;
		if (System)
		{
			const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				const FNiagaraEmitterHandle& Handle = Handles[Index];
				TSharedRef<FJsonObject> EmitterJson = MakeShared<FJsonObject>();
				EmitterJson->SetNumberField(TEXT("index"), Index);
				EmitterJson->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphens));
				EmitterJson->SetStringField(TEXT("name"), Handle.GetName().ToString());
				EmitterJson->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
				const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
				if (EmitterData)
				{
					EmitterJson->SetStringField(TEXT("sim_target"), EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("gpu") : TEXT("cpu"));
					EmitterJson->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
					EmitterJson->SetNumberField(TEXT("renderer_count"), EmitterData->GetRenderers().Num());
				}
				Emitters.Add(MakeShared<FJsonValueObject>(EmitterJson));
			}

			TArray<FNiagaraVariable> Parameters;
			System->GetExposedParameters().GetUserParameters(Parameters);
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				UserParameters.Add(MakeShared<FJsonValueObject>(NiagaraVariableTypeJson(Parameter)));
			}
		}
		Snapshot->SetStringField(TEXT("schema"), TEXT("somol.niagara_system_authoring_snapshot.v1"));
		Snapshot->SetStringField(TEXT("system_asset_path"), System ? System->GetPathName() : FString());
		Snapshot->SetArrayField(TEXT("emitters"), Emitters);
		Snapshot->SetArrayField(TEXT("user_parameters"), UserParameters);
		Snapshot->SetNumberField(TEXT("emitter_count"), Emitters.Num());
		Snapshot->SetNumberField(TEXT("user_parameter_count"), UserParameters.Num());
		Snapshot->SetStringField(TEXT("snapshot_hash"), JsonArrayHash(Emitters) + TEXT(".") + JsonArrayHash(UserParameters));
		Snapshot->SetStringField(TEXT("captured_at_utc"), FDateTime::UtcNow().ToIso8601());
		return Snapshot;
	}

	FString NiagaraCompileSeverityToString(FNiagaraCompileEventSeverity Severity)
	{
		switch (Severity)
		{
		case FNiagaraCompileEventSeverity::Error: return TEXT("error");
		case FNiagaraCompileEventSeverity::Warning: return TEXT("warning");
		case FNiagaraCompileEventSeverity::Display: return TEXT("display");
		case FNiagaraCompileEventSeverity::Log: return TEXT("log");
		default: return TEXT("unknown");
		}
	}

	void AddNiagaraDiagnosticMessage(TArray<TSharedPtr<FJsonValue>>& Messages, const FString& Severity, const FString& Code, const FString& Text)
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("severity"), Severity);
		Message->SetStringField(TEXT("code"), Code);
		Message->SetStringField(TEXT("text"), Text);
		Messages.Add(MakeShared<FJsonValueObject>(Message));
	}

	void AddNiagaraBlockingReason(TArray<TSharedPtr<FJsonValue>>& Reasons, const FString& Code, const FString& Detail)
	{
		TSharedRef<FJsonObject> Reason = MakeShared<FJsonObject>();
		Reason->SetStringField(TEXT("code"), Code);
		Reason->SetStringField(TEXT("detail"), Detail);
		Reasons.Add(MakeShared<FJsonValueObject>(Reason));
	}

	FString SummarizeNiagaraBlockingReasons(const TArray<TSharedPtr<FJsonValue>>& Reasons)
	{
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Value : Reasons)
		{
			const TSharedPtr<FJsonObject> Reason = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Reason.IsValid())
			{
				continue;
			}
			FString Code;
			FString Detail;
			Reason->TryGetStringField(TEXT("code"), Code);
			Reason->TryGetStringField(TEXT("detail"), Detail);
			Parts.Add(Detail.IsEmpty() ? Code : FString::Printf(TEXT("%s: %s"), *Code, *Detail));
		}
		return Parts.Num() == 0 ? FString() : FString::Join(Parts, TEXT("; "));
	}

	bool GetNiagaraBoolArg(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName, bool bDefaultValue)
	{
		return Arguments->HasTypedField<EJson::Boolean>(FieldName) ? Arguments->GetBoolField(FieldName) : bDefaultValue;
	}

	double GetNiagaraNumberArg(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName, double DefaultValue, double MinValue, double MaxValue)
	{
		double Value = DefaultValue;
		Arguments->TryGetNumberField(FieldName, Value);
		if (Value < MinValue)
		{
			return MinValue;
		}
		if (Value > MaxValue)
		{
			return MaxValue;
		}
		return Value;
	}

	FString GetNiagaraStringArg(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName, const FString& DefaultValue)
	{
		FString Value;
		return Arguments->TryGetStringField(FieldName, Value) && !Value.IsEmpty() ? Value : DefaultValue;
	}

	bool NiagaraCompileDiagnosticsPassed(const TSharedRef<FJsonObject>& Diagnostics)
	{
		bool bCompilePassed = false;
		if (Diagnostics->TryGetBoolField(TEXT("compile_passed"), bCompilePassed))
		{
			return bCompilePassed;
		}
		FString CompileStatus;
		return Diagnostics->TryGetStringField(TEXT("compile_status"), CompileStatus)
			&& CompileStatus.Equals(TEXT("passed"), ESearchCase::IgnoreCase);
	}

	struct FNiagaraSnapshotLink
	{
		FString FromNodeId;
		FString FromPinName;
		FString ToNodeId;
		FString ToPinName;
		FString Key;
		UEdGraphPin* FromPin = nullptr;
		UEdGraphPin* ToPin = nullptr;
	};

	struct FNiagaraSnapshotPinDefault
	{
		FString NodeId;
		FString PinName;
		FString Direction;
		FString DefaultValue;
		FString AutogeneratedDefaultValue;
		bool bHasDefaultValue = false;
		bool bHasAutogeneratedDefaultValue = false;
		UEdGraphNode* Node = nullptr;
		UEdGraphPin* Pin = nullptr;
	};

	bool NodeIsInSnapshot(const TSet<FString>& SnapshotNodeIds, const UEdGraphNode* Node)
	{
		return Node &&
			(SnapshotNodeIds.Contains(Node->NodeGuid.ToString()) ||
				SnapshotNodeIds.Contains(Node->GetName()));
	}

	void ParseSnapshotPinDefaults(
		const TSharedPtr<FJsonObject>& NodeObj,
		const FString& NodeId,
		TArray<FNiagaraSnapshotPinDefault>& OutPinDefaults)
	{
		if (!NodeObj.IsValid() || NodeId.IsEmpty())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
		if (!NodeObj->TryGetArrayField(TEXT("pins"), PinValues) || !PinValues)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& PinValue : *PinValues)
		{
			TSharedPtr<FJsonObject> PinObj = PinValue.IsValid() ? PinValue->AsObject() : nullptr;
			if (!PinObj.IsValid())
			{
				continue;
			}
			FNiagaraSnapshotPinDefault PinDefault;
			PinDefault.NodeId = NodeId;
			if (!PinObj->TryGetStringField(TEXT("name"), PinDefault.PinName) || PinDefault.PinName.IsEmpty())
			{
				continue;
			}
			PinObj->TryGetStringField(TEXT("direction"), PinDefault.Direction);
			PinDefault.bHasDefaultValue = PinObj->TryGetStringField(TEXT("default_value"), PinDefault.DefaultValue);
			PinDefault.bHasAutogeneratedDefaultValue = PinObj->TryGetStringField(TEXT("autogenerated_default_value"), PinDefault.AutogeneratedDefaultValue);
			if (PinDefault.bHasDefaultValue || PinDefault.bHasAutogeneratedDefaultValue)
			{
				OutPinDefaults.Add(PinDefault);
			}
		}
	}

	bool ResolveSnapshotPinDefaultEndpoints(
		UNiagaraGraph* Graph,
		TArray<FNiagaraSnapshotPinDefault>& PinDefaults,
		TArray<TSharedPtr<FJsonValue>>& OutMissingPins)
	{
		for (FNiagaraSnapshotPinDefault& PinDefault : PinDefaults)
		{
			PinDefault.Node = FindNodeByIdLocal(Graph, PinDefault.NodeId);
			const FString DirNorm = PinDefault.Direction.TrimStartAndEnd().ToLower();
			const bool bRequireDir = DirNorm == TEXT("input") || DirNorm == TEXT("output");
			const EEdGraphPinDirection RequiredDir = DirNorm == TEXT("output") ? EGPD_Output : EGPD_Input;
			PinDefault.Pin = FindPinByName(PinDefault.Node, PinDefault.PinName, RequiredDir, bRequireDir);
			if (!PinDefault.Node || !PinDefault.Pin)
			{
				TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
				Missing->SetStringField(TEXT("node_id"), PinDefault.NodeId);
				Missing->SetStringField(TEXT("pin_name"), PinDefault.PinName);
				Missing->SetStringField(TEXT("direction"), PinDefault.Direction);
				OutMissingPins.Add(MakeShared<FJsonValueObject>(Missing));
			}
		}
		return OutMissingPins.Num() == 0;
	}

	int32 RestoreSnapshotPinDefaults(
		const TArray<FNiagaraSnapshotPinDefault>& PinDefaults,
		const UEdGraphSchema* Schema)
	{
		int32 Restored = 0;
		for (const FNiagaraSnapshotPinDefault& PinDefault : PinDefaults)
		{
			if (!PinDefault.Node || !PinDefault.Pin)
			{
				continue;
			}

			bool bChanged = false;
			PinDefault.Node->Modify();
			PinDefault.Pin->Modify();
			if (PinDefault.bHasDefaultValue && PinDefault.Pin->DefaultValue != PinDefault.DefaultValue)
			{
				if (Schema)
				{
					Schema->TrySetDefaultValue(*PinDefault.Pin, PinDefault.DefaultValue);
				}
				if (PinDefault.Pin->DefaultValue != PinDefault.DefaultValue)
				{
					PinDefault.Pin->DefaultValue = PinDefault.DefaultValue;
				}
				bChanged = true;
			}
			if (PinDefault.bHasAutogeneratedDefaultValue &&
				PinDefault.Pin->AutogeneratedDefaultValue != PinDefault.AutogeneratedDefaultValue)
			{
				PinDefault.Pin->AutogeneratedDefaultValue = PinDefault.AutogeneratedDefaultValue;
				bChanged = true;
			}
			if (bChanged)
			{
				PinDefault.Node->PinDefaultValueChanged(PinDefault.Pin);
				++Restored;
			}
		}
		return Restored;
	}

	bool RunNiagaraGraphRestoreSnapshot(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError,
		const FString& ToolName)
	{
		const TSharedPtr<FJsonObject>* SnapshotPtr = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("snapshot"), SnapshotPtr) || !SnapshotPtr || !(*SnapshotPtr).IsValid())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("snapshot"));
			OutError = TEXT("Missing snapshot object.");
			return false;
		}
		const TSharedPtr<FJsonObject> Snapshot = *SnapshotPtr;

		UNiagaraScript* Script = nullptr;
		UNiagaraGraph* Graph = nullptr;
		if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
		{
			return false;
		}
		const FString ScriptPath = Script ? Script->GetPathName() : FString();

		bool bAllowDisposableWrite = false;
		Arguments->TryGetBoolField(TEXT("allow_disposable_write"), bAllowDisposableWrite);
		bool bAllowProductionRestore = false;
		Arguments->TryGetBoolField(TEXT("allow_production_restore"), bAllowProductionRestore);
		const bool bSafeDisposablePath = IsSafeDisposableNiagaraRollbackPath(ScriptPath);
		if (!bAllowProductionRestore && (!bAllowDisposableWrite || !bSafeDisposablePath))
		{
			SololmcpError::Set(OutStructured, TEXT("UNSAFE_TARGET"), TEXT("script_path"),
				TEXT("Restore is blocked unless allow_disposable_write=true and the script is under /Game/SOMOLMCP/DisposableRollbackProof/Niagara* (or allow_production_restore=true is explicitly supplied)."));
			OutStructured->SetBoolField(TEXT("allow_disposable_write"), bAllowDisposableWrite);
			OutStructured->SetBoolField(TEXT("safe_disposable_path"), bSafeDisposablePath);
			OutStructured->SetBoolField(TEXT("allow_production_restore"), bAllowProductionRestore);
			OutError = TEXT("Niagara graph restore blocked by disposable target guard.");
			return false;
		}

		bool bRestoreLayout = true;
		bool bRestoreLinks = false;
		bool bRestorePinDefaults = ToolName == TEXT("niagara_graph_restore_snapshot_full");
		bool bDeleteExtraNodes = ToolName == TEXT("niagara_graph_restore_snapshot_full");
		Arguments->TryGetBoolField(TEXT("restore_layout"), bRestoreLayout);
		Arguments->TryGetBoolField(TEXT("restore_links"), bRestoreLinks);
		Arguments->TryGetBoolField(TEXT("restore_pin_defaults"), bRestorePinDefaults);
		Arguments->TryGetBoolField(TEXT("delete_extra_nodes"), bDeleteExtraNodes);
		if (ToolName == TEXT("niagara_graph_restore_snapshot_full") && !Arguments->HasTypedField<EJson::Boolean>(TEXT("restore_links")))
		{
			bRestoreLinks = true;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!Snapshot->TryGetArrayField(TEXT("nodes"), NodeValues) || !NodeValues)
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_SNAPSHOT"), TEXT("snapshot.nodes"), TEXT("Snapshot must contain a nodes array."));
			OutError = TEXT("Snapshot nodes array is missing.");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> MissingNodes;
		TMap<FString, FVector2D> SnapshotNodePositions;
		TSet<FString> SnapshotNodeIds;
		TArray<FNiagaraSnapshotPinDefault> SnapshotPinDefaults;
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
		{
			TSharedPtr<FJsonObject> NodeObj = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
			if (!NodeObj.IsValid())
			{
				continue;
			}
			FString NodeId;
			NodeObj->TryGetStringField(TEXT("id"), NodeId);
			if (NodeId.IsEmpty())
			{
				NodeObj->TryGetStringField(TEXT("node_id"), NodeId);
			}
			if (NodeId.IsEmpty())
			{
				continue;
			}
			SnapshotNodeIds.Add(NodeId);
			double X = 0.0;
			double Y = 0.0;
			NodeObj->TryGetNumberField(TEXT("x"), X);
			NodeObj->TryGetNumberField(TEXT("y"), Y);
			SnapshotNodePositions.Add(NodeId, FVector2D(X, Y));
			if (bRestorePinDefaults)
			{
				ParseSnapshotPinDefaults(NodeObj, NodeId, SnapshotPinDefaults);
			}
			if (!FindNodeByIdLocal(Graph, NodeId))
			{
				TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
				Missing->SetStringField(TEXT("node_id"), NodeId);
				MissingNodes.Add(MakeShared<FJsonValueObject>(Missing));
			}
		}
		if (MissingNodes.Num() > 0)
		{
			SololmcpError::Set(OutStructured, TEXT("SNAPSHOT_NODE_MISSING"), TEXT("snapshot.nodes"),
				TEXT("One or more snapshot nodes no longer exist. This safe restore can restore layout/links for existing nodes, but will not recreate arbitrary Niagara node classes."));
			OutStructured->SetArrayField(TEXT("missing_nodes"), MissingNodes);
			OutError = TEXT("Snapshot contains nodes that are missing from the current graph.");
			return false;
		}

		TArray<FNiagaraSnapshotLink> ExpectedLinks;
		TSet<FString> ExpectedLinkKeys;
		if (bRestoreLinks)
		{
			const TArray<TSharedPtr<FJsonValue>>* LinkValues = nullptr;
			if (Snapshot->TryGetArrayField(TEXT("links"), LinkValues) && LinkValues)
			{
				for (const TSharedPtr<FJsonValue>& LinkValue : *LinkValues)
				{
					TSharedPtr<FJsonObject> LinkObj = LinkValue.IsValid() ? LinkValue->AsObject() : nullptr;
					if (!LinkObj.IsValid())
					{
						continue;
					}
					FNiagaraSnapshotLink Link;
					LinkObj->TryGetStringField(TEXT("from"), Link.FromNodeId);
					LinkObj->TryGetStringField(TEXT("from_pin"), Link.FromPinName);
					LinkObj->TryGetStringField(TEXT("to"), Link.ToNodeId);
					LinkObj->TryGetStringField(TEXT("to_pin"), Link.ToPinName);
					if (Link.FromNodeId.IsEmpty() || Link.ToNodeId.IsEmpty() || Link.FromPinName.IsEmpty() || Link.ToPinName.IsEmpty())
					{
						continue;
					}
					UEdGraphNode* FromNode = FindNodeByIdLocal(Graph, Link.FromNodeId);
					UEdGraphNode* ToNode = FindNodeByIdLocal(Graph, Link.ToNodeId);
					Link.FromPin = FindPinByName(FromNode, Link.FromPinName, EGPD_Output, false);
					Link.ToPin = FindPinByName(ToNode, Link.ToPinName, EGPD_Input, false);
					Link.Key = NiagaraLinkKey(Link.FromNodeId, Link.FromPinName, Link.ToNodeId, Link.ToPinName);
					if (!Link.FromPin || !Link.ToPin)
					{
						TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
						Missing->SetStringField(TEXT("link"), Link.Key);
						MissingNodes.Add(MakeShared<FJsonValueObject>(Missing));
						continue;
					}
					ExpectedLinkKeys.Add(Link.Key);
					ExpectedLinks.Add(Link);
				}
			}
			if (MissingNodes.Num() > 0)
			{
				SololmcpError::Set(OutStructured, TEXT("SNAPSHOT_LINK_ENDPOINT_MISSING"), TEXT("snapshot.links"),
					TEXT("One or more snapshot link endpoints are missing; no restore mutation was attempted."));
				OutStructured->SetArrayField(TEXT("missing_link_endpoints"), MissingNodes);
				OutError = TEXT("Snapshot link endpoint missing.");
				return false;
			}
		}

		const FString PreHash = BuildNiagaraGraphHash(Graph);
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		TArray<TSharedPtr<FJsonValue>> MissingPins;
		if (bRestorePinDefaults && !ResolveSnapshotPinDefaultEndpoints(Graph, SnapshotPinDefaults, MissingPins))
		{
			SololmcpError::Set(OutStructured, TEXT("SNAPSHOT_PIN_MISSING"), TEXT("snapshot.nodes[].pins"),
				TEXT("One or more snapshot pin defaults could not be resolved; no restore mutation was attempted."));
			OutStructured->SetArrayField(TEXT("missing_pins"), MissingPins);
			OutError = TEXT("Snapshot pin endpoint missing.");
			return false;
		}
		int32 LayoutRestored = 0;
		int32 RemovedExtraLinks = 0;
		int32 AddedMissingLinks = 0;
		int32 DeletedExtraNodes = 0;
		int32 PinDefaultsRestored = 0;

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraGraphRestoreSnapshot", "SOMOLMCP Restore Niagara Graph Snapshot"));
		Script->Modify();
		Graph->Modify();

		if (bDeleteExtraNodes)
		{
			TArray<UEdGraphNode*> NodesToDelete;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !NodeIsInSnapshot(SnapshotNodeIds, Node))
				{
					NodesToDelete.Add(Node);
				}
			}
			for (UEdGraphNode* Node : NodesToDelete)
			{
				if (!Node)
				{
					continue;
				}
				Node->Modify();
				Node->BreakAllNodeLinks();
				Graph->RemoveNode(Node);
				++DeletedExtraNodes;
			}
		}

		if (bRestoreLayout)
		{
			for (const TPair<FString, FVector2D>& Pair : SnapshotNodePositions)
			{
				UEdGraphNode* Node = FindNodeByIdLocal(Graph, Pair.Key);
				if (!Node)
				{
					continue;
				}
				const int32 NewX = static_cast<int32>(Pair.Value.X);
				const int32 NewY = static_cast<int32>(Pair.Value.Y);
				if (Node->NodePosX != NewX || Node->NodePosY != NewY)
				{
					Node->Modify();
					Node->NodePosX = NewX;
					Node->NodePosY = NewY;
					++LayoutRestored;
				}
			}
		}

		if (bRestorePinDefaults)
		{
			PinDefaultsRestored = RestoreSnapshotPinDefaults(SnapshotPinDefaults, Schema);
		}

		if (bRestoreLinks)
		{
			TArray<TPair<UEdGraphPin*, UEdGraphPin*>> LinksToBreak;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !SnapshotNodeIds.Contains(Node->NodeGuid.ToString()))
				{
					continue;
				}
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
						if (!LinkedPin || !LinkedOwner || !SnapshotNodeIds.Contains(LinkedOwner->NodeGuid.ToString()))
						{
							continue;
						}
						const FString ExistingKey = NiagaraLinkKey(Pin, LinkedPin);
						if (!ExpectedLinkKeys.Contains(ExistingKey))
						{
							LinksToBreak.Add(TPair<UEdGraphPin*, UEdGraphPin*>(Pin, LinkedPin));
						}
					}
				}
			}
			for (const TPair<UEdGraphPin*, UEdGraphPin*>& Pair : LinksToBreak)
			{
				if (Schema)
				{
					Schema->BreakSinglePinLink(Pair.Key, Pair.Value);
				}
				else if (Pair.Key && Pair.Value)
				{
					Pair.Key->BreakLinkTo(Pair.Value);
				}
				++RemovedExtraLinks;
			}
			for (const FNiagaraSnapshotLink& Link : ExpectedLinks)
			{
				if (PinsAreLinked(Link.FromPin, Link.ToPin))
				{
					continue;
				}
				if (Schema && Schema->TryCreateConnection(Link.FromPin, Link.ToPin) && PinsAreLinked(Link.FromPin, Link.ToPin))
				{
					++AddedMissingLinks;
				}
			}
		}

		Graph->NotifyGraphChanged();
		SololmcpWriteFlush::EnsureFlushed(Script);

		const FString PostHash = BuildNiagaraGraphHash(Graph);
		OutStructured->SetStringField(TEXT("schema"), TEXT("somol.niagara_graph_restore_snapshot.v1"));
		OutStructured->SetStringField(TEXT("tool_name"), ToolName);
		OutStructured->SetStringField(TEXT("script_path"), ScriptPath);
		OutStructured->SetStringField(TEXT("pre_hash"), PreHash);
		OutStructured->SetStringField(TEXT("post_hash"), PostHash);
		OutStructured->SetBoolField(TEXT("allow_disposable_write"), bAllowDisposableWrite);
		OutStructured->SetBoolField(TEXT("safe_disposable_path"), bSafeDisposablePath);
		OutStructured->SetBoolField(TEXT("allow_production_restore"), bAllowProductionRestore);
		OutStructured->SetBoolField(TEXT("restore_layout"), bRestoreLayout);
		OutStructured->SetBoolField(TEXT("restore_links"), bRestoreLinks);
		OutStructured->SetBoolField(TEXT("restore_pin_defaults"), bRestorePinDefaults);
		OutStructured->SetBoolField(TEXT("delete_extra_nodes"), bDeleteExtraNodes);
		OutStructured->SetNumberField(TEXT("layout_restored"), LayoutRestored);
		OutStructured->SetNumberField(TEXT("deleted_extra_nodes"), DeletedExtraNodes);
		OutStructured->SetNumberField(TEXT("pin_defaults_restored"), PinDefaultsRestored);
		OutStructured->SetNumberField(TEXT("removed_extra_links"), RemovedExtraLinks);
		OutStructured->SetNumberField(TEXT("added_missing_links"), AddedMissingLinks);
		FString SnapshotHash;
		Snapshot->TryGetStringField(TEXT("graph_hash"), SnapshotHash);
		if (!SnapshotHash.IsEmpty())
		{
			OutStructured->SetStringField(TEXT("snapshot_graph_hash"), SnapshotHash);
			OutStructured->SetBoolField(TEXT("post_hash_matches_snapshot_hash"), SnapshotHash == PostHash);
		}
		OutStructured->SetStringField(TEXT("status"), TEXT("restored"));
		OutStructured->SetObjectField(TEXT("post_snapshot"), BuildNiagaraGraphSnapshotJson(Script, Graph));
		if (!MaybeSaveNiagaraScriptLocal(Context.Services, Arguments, ScriptPath, OutStructured, OutError))
		{
			return false;
		}
		OutSummary = FString::Printf(TEXT("Restored Niagara graph snapshot for '%s' (layout=%d, deleted_nodes=%d, pin_defaults=%d, removed_links=%d, added_links=%d)."),
			*Script->GetName(), LayoutRestored, DeletedExtraNodes, PinDefaultsRestored, RemovedExtraLinks, AddedMissingLinks);
		return true;
	}

	FVector2D GetNodeLocationFromArguments(const TSharedRef<FJsonObject>& Arguments)
	{
		double X = 0.0;
		double Y = 0.0;
		if (Arguments->HasTypedField<EJson::Number>(TEXT("pos_x")))
		{
			X = Arguments->GetNumberField(TEXT("pos_x"));
		}
		if (Arguments->HasTypedField<EJson::Number>(TEXT("pos_y")))
		{
			Y = Arguments->GetNumberField(TEXT("pos_y"));
		}
		return FVector2D(X, Y);
	}

	bool ResolveNiagaraValueType(const FString& TypeName, FNiagaraTypeDefinition& OutType)
	{
		const FString Normalized = TypeName.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("float") || Normalized == TEXT("number"))
		{
			OutType = FNiagaraTypeDefinition::GetFloatDef();
			return true;
		}
		if (Normalized == TEXT("int") || Normalized == TEXT("int32"))
		{
			OutType = FNiagaraTypeDefinition::GetIntDef();
			return true;
		}
		if (Normalized == TEXT("bool") || Normalized == TEXT("boolean"))
		{
			OutType = FNiagaraTypeDefinition::GetBoolDef();
			return true;
		}
		if (Normalized == TEXT("color") || Normalized == TEXT("linearcolor"))
		{
			OutType = FNiagaraTypeDefinition::GetColorDef();
			return true;
		}
		if (Normalized == TEXT("position"))
		{
			OutType = FNiagaraTypeDefinition::GetPositionDef();
			return true;
		}
		if (Normalized == TEXT("parameter_map") || Normalized == TEXT("parametermap"))
		{
			OutType = FNiagaraTypeDefinition::GetParameterMapDef();
			return true;
		}
		return false;
	}

	bool ResolveNiagaraScriptUsage(const FString& UsageName, ENiagaraScriptUsage& OutUsage)
	{
		const FString Normalized = UsageName.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("function")) OutUsage = ENiagaraScriptUsage::Function;
		else if (Normalized == TEXT("module")) OutUsage = ENiagaraScriptUsage::Module;
		else if (Normalized == TEXT("dynamic_input")) OutUsage = ENiagaraScriptUsage::DynamicInput;
		else if (Normalized == TEXT("system_spawn")) OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
		else if (Normalized == TEXT("system_update")) OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
		else if (Normalized == TEXT("emitter_spawn")) OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
		else if (Normalized == TEXT("emitter_update")) OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
		else if (Normalized == TEXT("particle_spawn")) OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
		else if (Normalized == TEXT("particle_update")) OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
		else if (Normalized == TEXT("particle_event")) OutUsage = ENiagaraScriptUsage::ParticleEventScript;
		else if (Normalized == TEXT("simulation_stage")) OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript;
		else return false;
		return true;
	}

	/** Convert any TSharedPtr<FJsonValue> to a string suitable for FProperty::ImportText_Direct.
	 *  Mirrors the helper in SololmcpBlueprintComponentTools.cpp. */
	FString JsonValueToImportString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
			return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Null:
			return TEXT("None");
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
			if (Arr.Num() == 3 && Arr[0].IsValid() && Arr[0]->Type == EJson::Number)
			{
				return FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"),
					Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
			}
			if (Arr.Num() == 4 && Arr[0].IsValid() && Arr[0]->Type == EJson::Number)
			{
				return FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"),
					Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber(), Arr[3]->AsNumber());
			}
			FString OutString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
			FJsonSerializer::Serialize(Arr, Writer);
			return OutString;
		}
		case EJson::Object:
		{
			FString OutString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
			FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
			return OutString;
		}
		default:
			return FString();
		}
	}

	/** Resolve and instantiate a UNiagaraNode subclass by short class name. The short
	 *  names match the user-facing taxonomy in the tool spec; we map them to concrete
	 *  UNiagaraNode_X classes where they exist in this UE 5.7 build. The created node
	 *  is constructed inside the supplied Graph but NOT yet added to it — caller does
	 *  AddNode + CreateNewGuid + AllocateDefaultPins.
	 *
	 *  Returns nullptr (and sets OutError) for unknown class shortnames. */
	bool ResolveNiagaraOpNameLocal(const FString& RequestedOpRaw, FName& OutOpName)
	{
		FString Requested = RequestedOpRaw;
		Requested.TrimStartAndEndInline();
		Requested.RemoveFromStart(TEXT("Op"), ESearchCase::IgnoreCase);
		Requested.ReplaceInline(TEXT(" "), TEXT(""));
		Requested.ReplaceInline(TEXT("_"), TEXT(""));
		Requested.ReplaceInline(TEXT("-"), TEXT(""));
		const FString Norm = Requested.ToLower();

		static const TMap<FString, FName> OpNames = {
			{TEXT("add"), FName(TEXT("Numeric::Add"))},
			{TEXT("+"), FName(TEXT("Numeric::Add"))},
			{TEXT("subtract"), FName(TEXT("Numeric::Subtract"))},
			{TEXT("sub"), FName(TEXT("Numeric::Subtract"))},
			{TEXT("minus"), FName(TEXT("Numeric::Subtract"))},
			{TEXT("multiply"), FName(TEXT("Numeric::Mul"))},
			{TEXT("mul"), FName(TEXT("Numeric::Mul"))},
			{TEXT("*"), FName(TEXT("Numeric::Mul"))},
			{TEXT("divide"), FName(TEXT("Numeric::Div"))},
			{TEXT("div"), FName(TEXT("Numeric::Div"))},
			{TEXT("/"), FName(TEXT("Numeric::Div"))},
			{TEXT("multiplyadd"), FName(TEXT("Numeric::Madd"))},
			{TEXT("madd"), FName(TEXT("Numeric::Madd"))},
			{TEXT("lerp"), FName(TEXT("Numeric::Lerp"))},
			{TEXT("reciprocal"), FName(TEXT("Numeric::Reciprocal"))},
			{TEXT("rcp"), FName(TEXT("Numeric::Reciprocal"))},
			{TEXT("sqrt"), FName(TEXT("Numeric::Sqrt"))},
			{TEXT("rsqrt"), FName(TEXT("Numeric::Rsqrt"))},
			{TEXT("abs"), FName(TEXT("Numeric::Abs"))},
			{TEXT("negate"), FName(TEXT("Numeric::Negate"))},
			{TEXT("one_minus"), FName(TEXT("Numeric::OneMinus"))},
			{TEXT("oneminus"), FName(TEXT("Numeric::OneMinus"))}
		};
		if (const FName* Found = OpNames.Find(Norm))
		{
			OutOpName = *Found;
			return true;
		}
		return false;
	}

	UNiagaraNode* CreateNodeOfShortClass(UNiagaraGraph* Graph, const FString& ShortClass, const TSharedPtr<FJsonObject>& ExtraProps, FString& OutError)
	{
		if (!Graph)
		{
			OutError = TEXT("Graph is null.");
			return nullptr;
		}
		const FString Norm = ShortClass.ToLower();

		// --- OpAdd / OpMul / OpXxx ---  treat anything starting with "op" as a UNiagaraNodeOp.
		if (Norm.StartsWith(TEXT("op")) || Norm == TEXT("op"))
		{
			UNiagaraNodeOp* Node = NewObject<UNiagaraNodeOp>(Graph);
			FString RequestedOp;
			if (ExtraProps.IsValid()) ExtraProps->TryGetStringField(TEXT("op_name"), RequestedOp);
			if (RequestedOp.IsEmpty() && Norm.Len() > 2) RequestedOp = ShortClass.RightChop(2);
			FName ResolvedOpName;
			if (!ResolveNiagaraOpNameLocal(RequestedOp, ResolvedOpName))
			{
				OutError = FString::Printf(TEXT("Unsupported Niagara operation '%s'. Supported op_name values include Add, Subtract, Mul, Div, Madd, Lerp, Reciprocal, Sqrt, Rsqrt, Abs, Negate, OneMinus."), *RequestedOp);
				return nullptr;
			}
			Node->OpName = ResolvedOpName;
			return Node;
		}

		// --- FunctionCall ---
		if (Norm == TEXT("functioncall") || Norm == TEXT("function_call"))
		{
			UNiagaraNodeFunctionCall* Node = NewObject<UNiagaraNodeFunctionCall>(Graph);
			if (ExtraProps.IsValid())
			{
				FString FunctionPath;
				if (ExtraProps->TryGetStringField(TEXT("function_path"), FunctionPath))
				{
					// Try to load the script being called (a UNiagaraScript module/function).
					if (UNiagaraScript* Callee = LoadObject<UNiagaraScript>(nullptr, *FunctionPath))
					{
						// TODO(P1-4): UE 5.5+ migrated FunctionScript from a raw
						// UNiagaraScript* to FVersionedNiagaraScript on
						// UNiagaraNodeFunctionCall. We assign via the legacy-named
						// member; if compilation fails on this branch, switch to:
						//   Node->FunctionScript = FVersionedNiagaraScript{Callee, FGuid()};
						// or use Node->SetFunctionScript(Callee) where available.
						Node->FunctionScript = Callee;
					}
					else
					{
						// TODO(P1-4): also support FunctionDisplayName /
						// FunctionScriptAssetObjectPath for cross-version name resolution.
						OutError = FString::Printf(TEXT("function_path '%s' could not be loaded as UNiagaraScript."), *FunctionPath);
					}
				}
			}
			return Node;
		}

		// --- Input ---
		if (Norm == TEXT("input") || Norm == TEXT("inputnode"))
		{
			UNiagaraNodeInput* Node = NewObject<UNiagaraNodeInput>(Graph);
			FString InputName = TEXT("Input");
			FString TypeName = TEXT("float");
			if (ExtraProps.IsValid())
			{
				ExtraProps->TryGetStringField(TEXT("input_name"), InputName);
				ExtraProps->TryGetStringField(TEXT("type"), TypeName);
			}
			FNiagaraTypeDefinition Type;
			if (!ResolveNiagaraValueType(TypeName, Type))
			{
				OutError = FString::Printf(TEXT("Unsupported Niagara input type '%s'."), *TypeName);
				return nullptr;
			}
			Node->Input = FNiagaraVariable(Type, FName(*InputName));
			Node->Usage = ENiagaraInputNodeUsage::Parameter;
			return Node;
		}

		// --- Output ---
		if (Norm == TEXT("output") || Norm == TEXT("outputnode"))
		{
			UNiagaraNodeOutput* Node = NewObject<UNiagaraNodeOutput>(Graph);
			FString OutputName = TEXT("Output");
			FString TypeName = TEXT("float");
			FString UsageName = TEXT("function");
			if (ExtraProps.IsValid())
			{
				ExtraProps->TryGetStringField(TEXT("output_name"), OutputName);
				ExtraProps->TryGetStringField(TEXT("type"), TypeName);
				ExtraProps->TryGetStringField(TEXT("usage"), UsageName);
			}
			FNiagaraTypeDefinition Type;
			ENiagaraScriptUsage Usage;
			if (!ResolveNiagaraValueType(TypeName, Type) || !ResolveNiagaraScriptUsage(UsageName, Usage))
			{
				OutError = FString::Printf(TEXT("Unsupported Niagara output type '%s' or usage '%s'."), *TypeName, *UsageName);
				return nullptr;
			}
			Node->Outputs.Add(FNiagaraVariable(Type, FName(*OutputName)));
			Node->SetUsage(Usage);
			Node->SetUsageId(FGuid::NewGuid());
			return Node;
		}

		// --- NumericConstant ---
		// UE 5.7 does not ship a class named UNiagaraNodeNumericConstant; numeric
		// constants are authored via UNiagaraNodeInput with a literal FNiagaraVariable
		// + UNiagaraNodeInput::Usage = ENiagaraInputNodeUsage::Parameter and a default
		// value. We synthesize that here.
		if (Norm == TEXT("numericconstant") || Norm == TEXT("numeric_constant") || Norm == TEXT("constant"))
		{
			UNiagaraNodeInput* Node = NewObject<UNiagaraNodeInput>(Graph);
			FString ConstantName = TEXT("Constant");
			FString TypeName = TEXT("float");
			double NumericValue = 0.0;
			bool bBoolValue = false;
			if (ExtraProps.IsValid())
			{
				ExtraProps->TryGetStringField(TEXT("input_name"), ConstantName);
				ExtraProps->TryGetStringField(TEXT("type"), TypeName);
				ExtraProps->TryGetNumberField(TEXT("value"), NumericValue);
				ExtraProps->TryGetBoolField(TEXT("value"), bBoolValue);
			}
			const FString NormalizedType = TypeName.ToLower();
			if (NormalizedType == TEXT("float") || NormalizedType == TEXT("double") || NormalizedType == TEXT("number"))
			{
				Node->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(*ConstantName));
				Node->Input.SetValue<float>(static_cast<float>(NumericValue));
			}
			else if (NormalizedType == TEXT("int") || NormalizedType == TEXT("int32") || NormalizedType == TEXT("integer"))
			{
				Node->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), FName(*ConstantName));
				Node->Input.SetValue<int32>(static_cast<int32>(NumericValue));
			}
			else if (NormalizedType == TEXT("bool") || NormalizedType == TEXT("boolean"))
			{
				Node->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), FName(*ConstantName));
				Node->Input.SetValue<bool>(bBoolValue || NumericValue != 0.0);
			}
			else
			{
				OutError = FString::Printf(TEXT("NumericConstant supports float, int, or bool; received '%s'."), *TypeName);
				return nullptr;
			}
			Node->Usage = ENiagaraInputNodeUsage::Parameter;
			return Node;
		}

		// --- DataInterface ---
		// In UE 5.7 a DI is added to the graph as a UNiagaraNodeInput whose Input variable
		// holds a UNiagaraDataInterface CDO reference. We create the bare node and let
		// callers configure the DI class via set_node_property.
		if (Norm == TEXT("datainterface") || Norm == TEXT("data_interface") || Norm == TEXT("di"))
		{
			UNiagaraNodeInput* Node = NewObject<UNiagaraNodeInput>(Graph);
			if (ExtraProps.IsValid())
			{
				FString DIClassPath;
				ExtraProps->TryGetStringField(TEXT("di_class"), DIClassPath);
				if (!DIClassPath.IsEmpty())
				{
					UClass* DIClass = LoadClass<UNiagaraDataInterface>(nullptr, *DIClassPath);
					if (!DIClass || !DIClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
					{
						OutError = FString::Printf(TEXT("di_class '%s' is not a UNiagaraDataInterface class."), *DIClassPath);
						return nullptr;
					}
					FString InputName = TEXT("DataInterface");
					ExtraProps->TryGetStringField(TEXT("input_name"), InputName);
					UNiagaraDataInterface* DataInterface = NewObject<UNiagaraDataInterface>(Graph, DIClass, NAME_None, RF_Transactional);
					Node->Input = FNiagaraVariable(FNiagaraTypeDefinition(DIClass), FName(*InputName));
					Node->Usage = ENiagaraInputNodeUsage::Parameter;
					// UNiagaraNodeInput is MinimalAPI in UE 5.7; SetDataInterface is
					// declared public but not always exported for plugin linkers.
					// Set the reflected UPROPERTY instead so BuildPlugin stays portable
					// across installed-engine builds.
					if (FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(UNiagaraNodeInput::StaticClass(), TEXT("DataInterface")))
					{
						DataInterfaceProperty->SetObjectPropertyValue_InContainer(Node, DataInterface);
					}
					else
					{
						OutError = TEXT("UNiagaraNodeInput.DataInterface property is not available for reflective assignment.");
						return nullptr;
					}
				}
			}
			return Node;
		}

		// --- ReadDataSet / WriteDataSet ---
		if (Norm == TEXT("readdataset") || Norm == TEXT("read_data_set"))
		{
			return NewObject<UNiagaraNodeReadDataSet>(Graph);
		}
		if (Norm == TEXT("writedataset") || Norm == TEXT("write_data_set"))
		{
			return NewObject<UNiagaraNodeWriteDataSet>(Graph);
		}

		OutError = FString::Printf(TEXT("Unknown node_class_short '%s'. Known: NumericConstant, OpAdd/Op*, FunctionCall, Input, Output, DataInterface, ReadDataSet, WriteDataSet."), *ShortClass);
		return nullptr;
	}
} // anonymous namespace

// =====================================================================
// Tool registration entry point
// =====================================================================
void RegisterNiagaraScriptGraphTools(FSololmcpToolRegistry& Registry)
{
	// ----- niagara_graph_explain -----------------------------------------
	Registry.Register({
		TEXT("niagara_graph_explain"),
		TEXT("Read-only Niagara graph explain receipt: script nodes/pins/links plus stable schema anchors for emitters, modules, renderers, user parameters, and asset references."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path. Alias of script_path."))},
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path."))},
			{TEXT("include_graph"), FSololmcpSchemaBuilder::Boolean(TEXT("Include node/pin graph details (default true)."))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("script_path"), AssetPath))
			{
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			}
			if (AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path or script_path.");
				return false;
			}

			UNiagaraScript* Script = LoadNiagaraScriptLocal(Context.Services, AssetPath, OutError);
			if (!Script)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}
			UNiagaraGraph* Graph = GetGraphFromScriptLocal(Script);
			if (!Graph)
			{
				SololmcpError::NotFound(OutStructured, TEXT("UNiagaraGraph (script source)"));
				OutError = TEXT("Niagara script has no valid UNiagaraGraph.");
				return false;
			}

			bool bIncludeGraph = true;
			Arguments->TryGetBoolField(TEXT("include_graph"), bIncludeGraph);

			TArray<TSharedPtr<FJsonValue>> Nodes;
			TArray<TSharedPtr<FJsonValue>> Links;
			TArray<TSharedPtr<FJsonValue>> Modules;
			TArray<TSharedPtr<FJsonValue>> Renderers;
			TArray<TSharedPtr<FJsonValue>> UserParameters;
			TArray<TSharedPtr<FJsonValue>> References;
			TSet<FString> SeenLinks;
			TSet<FString> SeenReferences;
			if (bIncludeGraph)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node)
					{
						continue;
					}
					TSharedRef<FJsonObject> NodeJson = NiagaraNodeToExplainJson(Node);
					Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
					const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();
					if (ClassName.Contains(TEXT("FunctionCall")) || ClassName.Contains(TEXT("Module")))
					{
						Modules.Add(MakeShared<FJsonValueObject>(NodeJson));
					}
					if (ClassName.Contains(TEXT("Renderer")))
					{
						Renderers.Add(MakeShared<FJsonValueObject>(NodeJson));
					}
					if (ClassName.Contains(TEXT("Input")))
					{
						UserParameters.Add(MakeShared<FJsonValueObject>(NodeJson));
					}
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin)
						{
							continue;
						}
						if (Pin->PinType.PinSubCategoryObject.IsValid())
						{
							const FString RefPath = Pin->PinType.PinSubCategoryObject->GetPathName();
							if (!RefPath.IsEmpty() && !SeenReferences.Contains(RefPath))
							{
								SeenReferences.Add(RefPath);
								TSharedRef<FJsonObject> RefJson = MakeShared<FJsonObject>();
								RefJson->SetStringField(TEXT("path"), RefPath);
								RefJson->SetStringField(TEXT("source"), FString::Printf(TEXT("%s.%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString()));
								References.Add(MakeShared<FJsonValueObject>(RefJson));
							}
						}
						if (Pin->Direction != EGPD_Output)
						{
							continue;
						}
						for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
							if (!LinkedPin || !LinkedOwner)
							{
								continue;
							}
							const FString LinkId = FString::Printf(TEXT("%s:%s>%s:%s"),
								*Node->NodeGuid.ToString(), *Pin->PinName.ToString(),
								*LinkedOwner->NodeGuid.ToString(), *LinkedPin->PinName.ToString());
							if (SeenLinks.Contains(LinkId))
							{
								continue;
							}
							SeenLinks.Add(LinkId);
							TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
							LinkJson->SetStringField(TEXT("id"), LinkId);
							LinkJson->SetStringField(TEXT("from"), Node->NodeGuid.ToString());
							LinkJson->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
							LinkJson->SetStringField(TEXT("to"), LinkedOwner->NodeGuid.ToString());
							LinkJson->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
							LinkJson->SetStringField(TEXT("kind"), TEXT("data"));
							Links.Add(MakeShared<FJsonValueObject>(LinkJson));
						}
					}
				}
			}

			OutStructured->SetStringField(TEXT("asset_path"), Script->GetPathName());
			OutStructured->SetStringField(TEXT("schema_version"), TEXT("niagara_graph_explain.v1.read_only"));
			OutStructured->SetBoolField(TEXT("read_only"), true);
			OutStructured->SetArrayField(TEXT("nodes"), Nodes);
			OutStructured->SetArrayField(TEXT("links"), Links);
			OutStructured->SetArrayField(TEXT("emitters"), TArray<TSharedPtr<FJsonValue>>());
			OutStructured->SetArrayField(TEXT("modules"), Modules);
			OutStructured->SetArrayField(TEXT("renderers"), Renderers);
			OutStructured->SetArrayField(TEXT("user_parameters"), UserParameters);
			OutStructured->SetArrayField(TEXT("referenced_assets"), References);
			OutStructured->SetStringField(TEXT("system_scope_note"), TEXT("This skeleton is intentionally read-only. UNiagaraSystem emitter stack extraction can extend emitters/renderers without changing the receipt schema."));
			OutSummary = FString::Printf(TEXT("Explained Niagara script graph '%s': %d node(s), %d link(s)."), *Script->GetName(), Nodes.Num(), Links.Num());
			return true;
		}
	});

	// ----- niagara_graph_snapshot ---------------------------------------
	Registry.Register({
		TEXT("niagara_graph_snapshot"),
		TEXT("Read-only Niagara script graph rollback snapshot with deterministic graph hash, nodes, links, layout, pins, and referenced asset hints."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path."))},
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Alias of script_path."))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("script_path"), AssetPath))
			{
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			}
			if (AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("script_path"));
				OutError = TEXT("Missing script_path or asset_path.");
				return false;
			}

			UNiagaraScript* Script = LoadNiagaraScriptLocal(Context.Services, AssetPath, OutError);
			if (!Script)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}
			UNiagaraGraph* Graph = GetGraphFromScriptLocal(Script);
			if (!Graph)
			{
				SololmcpError::NotFound(OutStructured, TEXT("UNiagaraGraph (script source)"));
				OutError = TEXT("Niagara script has no valid UNiagaraGraph.");
				return false;
			}

			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.niagara_graph_snapshot_tool.v1"));
			OutStructured->SetBoolField(TEXT("read_only"), true);
			OutStructured->SetStringField(TEXT("script_path"), Script->GetPathName());
			OutStructured->SetObjectField(TEXT("snapshot"), BuildNiagaraGraphSnapshotJson(Script, Graph));
			OutSummary = FString::Printf(TEXT("Captured Niagara graph snapshot for '%s'."), *Script->GetName());
			return true;
		}
	});

	// ----- niagara_system_authoring_snapshot -----------------------------
	Registry.Register({
		TEXT("niagara_system_authoring_snapshot"),
		TEXT("Read-only Niagara system authoring snapshot for rollback/preview: emitter order, enabled state, renderer counts, user parameter names/types, and a stable snapshot hash."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraSystem asset path."))},
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Alias of system_asset_path."))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("system_asset_path"), AssetPath))
			{
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			}
			if (AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("system_asset_path"));
				OutError = TEXT("Missing system_asset_path or asset_path.");
				return false;
			}

			UNiagaraSystem* System = LoadNiagaraSystemLocal(Context.Services, AssetPath, OutError);
			if (!System)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			OutStructured->SetBoolField(TEXT("read_only"), true);
			OutStructured->SetObjectField(TEXT("snapshot"), BuildNiagaraSystemAuthoringSnapshotJson(System));
			OutSummary = FString::Printf(TEXT("Captured Niagara system authoring snapshot for '%s'."), *System->GetName());
			return true;
		}
	});

	// ----- niagara_graph_restore_snapshot --------------------------------
	Registry.Register({
		TEXT("niagara_graph_restore_snapshot"),
		TEXT("Restore a Niagara script graph from a snapshot. Safe default: requires allow_disposable_write=true and a /Game/SOMOLMCP/Disposable* target; can restore layout and optionally links."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path."))},
			{TEXT("snapshot"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Snapshot from niagara_graph_snapshot or niagara_graph_explain-compatible nodes/links."))},
			{TEXT("allow_disposable_write"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for normal restore."))},
			{TEXT("allow_production_restore"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicit override for production assets; off by default."))},
			{TEXT("restore_layout"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore node coordinates; default true."))},
			{TEXT("restore_links"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore links among snapshot nodes; default false."))},
			{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after restore; default false."))}
		}, {TEXT("script_path"), TEXT("snapshot")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return RunNiagaraGraphRestoreSnapshot(Context, Arguments, OutStructured, OutSummary, OutError, TEXT("niagara_graph_restore_snapshot"));
		}
	});

	// ----- niagara_graph_restore_snapshot_full ---------------------------
	Registry.Register({
		TEXT("niagara_graph_restore_snapshot_full"),
		TEXT("Full disposable Niagara graph restore from a snapshot: restore layout, links, pin defaults, and delete nodes that were added after the snapshot. Production restore requires allow_production_restore=true."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path."))},
			{TEXT("snapshot"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Snapshot from niagara_graph_snapshot."))},
			{TEXT("allow_disposable_write"), FSololmcpSchemaBuilder::Boolean(TEXT("Required unless allow_production_restore=true."))},
			{TEXT("allow_production_restore"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicit override for production assets; off by default."))},
			{TEXT("restore_layout"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore node coordinates; default true."))},
			{TEXT("restore_links"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore links among snapshot nodes; default true."))},
			{TEXT("restore_pin_defaults"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore pin DefaultValue/AutogeneratedDefaultValue from snapshot; default true."))},
			{TEXT("delete_extra_nodes"), FSololmcpSchemaBuilder::Boolean(TEXT("Delete graph nodes not present in snapshot; default true."))},
			{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after restore; default false."))}
		}, {TEXT("script_path"), TEXT("snapshot")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return RunNiagaraGraphRestoreSnapshot(Context, Arguments, OutStructured, OutSummary, OutError, TEXT("niagara_graph_restore_snapshot_full"));
		}
	});

	// ----- niagara_system_restore_snapshot -------------------------------
	Registry.Register({
		TEXT("niagara_system_restore_snapshot"),
		TEXT("Rollback-surface alias for Niagara graph snapshots used by systems. Accepts script_path + snapshot and applies the same disposable-target guard as niagara_graph_restore_snapshot."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path to restore. System-level emitter stack restore is intentionally not inferred."))},
			{TEXT("snapshot"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Snapshot from niagara_graph_snapshot or compatible nodes/links."))},
			{TEXT("allow_disposable_write"), FSololmcpSchemaBuilder::Boolean(TEXT("Required for normal restore."))},
			{TEXT("allow_production_restore"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicit override for production assets; off by default."))},
			{TEXT("restore_layout"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore node coordinates; default true."))},
			{TEXT("restore_links"), FSololmcpSchemaBuilder::Boolean(TEXT("Restore links among snapshot nodes; default false."))},
			{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after restore; default false."))}
		}, {TEXT("script_path"), TEXT("snapshot")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			return RunNiagaraGraphRestoreSnapshot(Context, Arguments, OutStructured, OutSummary, OutError, TEXT("niagara_system_restore_snapshot"));
		}
	});

	// ----- niagara_script_add_node ---------------------------------------
	Registry.Register({
		TEXT("niagara_script_add_node"),
		TEXT("Add a UNiagaraNode subclass to a UNiagaraScript's graph. Supports short class names: NumericConstant, OpAdd/Op*, FunctionCall, Input, Output, DataInterface, ReadDataSet, WriteDataSet."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path."))},
			{TEXT("node_class_short"), FSololmcpSchemaBuilder::String(TEXT("Short class name (NumericConstant|OpAdd|FunctionCall|Input|Output|DataInterface|...)."))},
			{TEXT("pos_x"), FSololmcpSchemaBuilder::Number()},
			{TEXT("pos_y"), FSololmcpSchemaBuilder::Number()},
			{TEXT("extra_props"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Per-class extra args (function_path, value, input_name, di_class, ...)."))}
		}, {TEXT("script_path"), TEXT("node_class_short")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString ShortClass;
			if (!Arguments->TryGetStringField(TEXT("node_class_short"), ShortClass))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("node_class_short"));
				OutError = TEXT("Missing node_class_short.");
				return false;
			}
			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
			{
				return false;
			}

			TSharedPtr<FJsonObject> ExtraProps;
			TryGetObjectField(Arguments, TEXT("extra_props"), ExtraProps);

			const FVector2D Location = GetNodeLocationFromArguments(Arguments);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraScriptAddNode", "SOMOLMCP Add Niagara Script Node"));
			Script->Modify();
			Graph->Modify();

			UNiagaraNode* Node = CreateNodeOfShortClass(Graph, ShortClass, ExtraProps, OutError);
			if (!Node)
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("node_class_short"), OutError);
				return false;
			}

			// Standard EdGraphNode add-pattern.
			Graph->AddNode(Node, /*bUserAction*/ false, /*bSelectNewNode*/ false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			Node->NodePosX = static_cast<int32>(Location.X);
			Node->NodePosY = static_cast<int32>(Location.Y);

			if (!Graph->Nodes.Contains(Node) || !Node->NodeGuid.IsValid())
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("node_class_short"),
					TEXT("Niagara node was constructed but was not present in the graph after AddNode."));
				OutError = TEXT("Niagara node add validation failed.");
				return false;
			}

			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);
			if (!MaybeSaveNiagaraScriptLocal(Context.Services, Arguments, Script->GetPathName(), OutStructured, OutError))
			{
				return false;
			}

			UNiagaraNode* ReadbackNode = Cast<UNiagaraNode>(FindNodeByIdLocal(Graph, Node->NodeGuid.ToString()));
			if (!ReadbackNode || ReadbackNode->GetClass() != Node->GetClass())
			{
				SololmcpError::Set(OutStructured, TEXT("READBACK_FAILED"), TEXT("node_class_short"),
					TEXT("Niagara node was not recoverable by its GUID after mutation flush/save."));
				OutError = TEXT("Niagara node readback failed after add.");
				return false;
			}

			OutStructured->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			OutStructured->SetStringField(TEXT("node_name"), Node->GetName());
			OutStructured->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			OutStructured->SetNumberField(TEXT("pos_x"), Node->NodePosX);
			OutStructured->SetNumberField(TEXT("pos_y"), Node->NodePosY);
			OutStructured->SetNumberField(TEXT("pin_count"), ReadbackNode->Pins.Num());
			OutStructured->SetBoolField(TEXT("readback_verified"), true);
			OutSummary = FString::Printf(TEXT("Added Niagara node '%s' to '%s'."), *Node->GetClass()->GetName(), *Script->GetName());
			return true;
		}
	});

	// ----- niagara_script_connect_pins -----------------------------------
	Registry.Register({
		TEXT("niagara_script_connect_pins"),
		TEXT("Connect two pins on Niagara graph nodes via UEdGraphSchema_Niagara::TryCreateConnection."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("from_node_id"), FSololmcpSchemaBuilder::String(TEXT("NodeGuid (preferred) or UObject name."))},
			{TEXT("from_pin_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_node_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_pin_name"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("script_path"), TEXT("from_node_id"), TEXT("from_pin_name"), TEXT("to_node_id"), TEXT("to_pin_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString FromNodeId, FromPinName, ToNodeId, ToPinName;
			if (!Arguments->TryGetStringField(TEXT("from_node_id"), FromNodeId)) { SololmcpError::MissingParam(OutStructured, TEXT("from_node_id")); OutError = TEXT("Missing from_node_id."); return false; }
			if (!Arguments->TryGetStringField(TEXT("from_pin_name"), FromPinName)) { SololmcpError::MissingParam(OutStructured, TEXT("from_pin_name")); OutError = TEXT("Missing from_pin_name."); return false; }
			if (!Arguments->TryGetStringField(TEXT("to_node_id"), ToNodeId)) { SololmcpError::MissingParam(OutStructured, TEXT("to_node_id")); OutError = TEXT("Missing to_node_id."); return false; }
			if (!Arguments->TryGetStringField(TEXT("to_pin_name"), ToPinName)) { SololmcpError::MissingParam(OutStructured, TEXT("to_pin_name")); OutError = TEXT("Missing to_pin_name."); return false; }

			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
			{
				return false;
			}

			UEdGraphNode* FromNode = FindNodeByIdLocal(Graph, FromNodeId);
			UEdGraphNode* ToNode = FindNodeByIdLocal(Graph, ToNodeId);
			if (!FromNode) { SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("from_node '%s'"), *FromNodeId)); OutError = TEXT("from_node_id not found."); return false; }
			if (!ToNode) { SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("to_node '%s'"), *ToNodeId)); OutError = TEXT("to_node_id not found."); return false; }

			// Output-from on the source, Input-to on the destination.
			UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName, EGPD_Output, /*bRequireDir*/ true);
			UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName, EGPD_Input, /*bRequireDir*/ true);
			// Allow callers to pass either direction (Niagara graphs sometimes invert).
			if (!FromPin) { FromPin = FindPinByName(FromNode, FromPinName, EGPD_Output, /*bRequireDir*/ false); }
			if (!ToPin) { ToPin = FindPinByName(ToNode, ToPinName, EGPD_Input, /*bRequireDir*/ false); }
			if (!FromPin) { SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("from_pin '%s'"), *FromPinName)); OutError = TEXT("from_pin not found."); return false; }
			if (!ToPin) { SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("to_pin '%s'"), *ToPinName)); OutError = TEXT("to_pin not found."); return false; }

			const UEdGraphSchema* Schema = Graph->GetSchema();
			if (!Schema)
			{
				SololmcpError::NotFound(OutStructured, TEXT("graph schema"));
				OutError = TEXT("UNiagaraGraph has no schema.");
				return false;
			}

			TSharedRef<FJsonObject> BeforeSnapshot = BuildNiagaraGraphSnapshotJson(Script, Graph);
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraScriptConnectPins", "SOMOLMCP Connect Niagara Pins"));
			Script->Modify();
			Graph->Modify();
			FromNode->Modify();
			ToNode->Modify();

			// TODO(P1-4): Niagara's TryCreateConnection in 5.7 returns bool and applies the
			// connection on success. Some 5.x branches expose a CanCreateConnection check
			// first; we trust TryCreateConnection's internal validation here.
			const bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);
			if (!bConnected || !PinsAreLinked(FromPin, ToPin))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("UEdGraphSchema_Niagara::TryCreateConnection returned false (incompatible types?)."));
				OutStructured->SetBoolField(TEXT("connected_verified"), false);
				OutError = bConnected ? TEXT("TryCreateConnection did not produce a verified bidirectional pin link.") : TEXT("TryCreateConnection failed.");
				return false;
			}

			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetBoolField(TEXT("connected_verified"), true);
			OutStructured->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *FromNode->NodeGuid.ToString(), *FromPin->PinName.ToString()));
			OutStructured->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *ToNode->NodeGuid.ToString(), *ToPin->PinName.ToString()));
			TSharedRef<FJsonObject> AfterSnapshot = BuildNiagaraGraphSnapshotJson(Script, Graph);
			OutStructured->SetObjectField(TEXT("pre_edit_snapshot"), BeforeSnapshot);
			OutStructured->SetObjectField(TEXT("post_edit_snapshot"), AfterSnapshot);
			AttachNiagaraGraphEditReceipt(
				OutStructured,
				Script,
				Graph,
				BeforeSnapshot,
				AfterSnapshot,
				TEXT("connect_pins"),
				TEXT("connected_verified"),
				true);
			OutSummary = TEXT("Connected Niagara graph pins.");
			return true;
		}
	});

	// ----- niagara_script_disconnect_pins --------------------------------
	Registry.Register({
		TEXT("niagara_script_disconnect_pins"),
		TEXT("Disconnect a specific link between two pins on Niagara graph nodes (no-op if not connected)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("from_node_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("from_pin_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_node_id"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_pin_name"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("script_path"), TEXT("from_node_id"), TEXT("from_pin_name"), TEXT("to_node_id"), TEXT("to_pin_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString FromNodeId, FromPinName, ToNodeId, ToPinName;
			if (!Arguments->TryGetStringField(TEXT("from_node_id"), FromNodeId)) { SololmcpError::MissingParam(OutStructured, TEXT("from_node_id")); OutError = TEXT("Missing from_node_id."); return false; }
			if (!Arguments->TryGetStringField(TEXT("from_pin_name"), FromPinName)) { SololmcpError::MissingParam(OutStructured, TEXT("from_pin_name")); OutError = TEXT("Missing from_pin_name."); return false; }
			if (!Arguments->TryGetStringField(TEXT("to_node_id"), ToNodeId)) { SololmcpError::MissingParam(OutStructured, TEXT("to_node_id")); OutError = TEXT("Missing to_node_id."); return false; }
			if (!Arguments->TryGetStringField(TEXT("to_pin_name"), ToPinName)) { SololmcpError::MissingParam(OutStructured, TEXT("to_pin_name")); OutError = TEXT("Missing to_pin_name."); return false; }

			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
			{
				return false;
			}

			UEdGraphNode* FromNode = FindNodeByIdLocal(Graph, FromNodeId);
			UEdGraphNode* ToNode = FindNodeByIdLocal(Graph, ToNodeId);
			if (!FromNode || !ToNode)
			{
				SololmcpError::NotFound(OutStructured, TEXT("from_node or to_node"));
				OutError = TEXT("from_node_id or to_node_id not found.");
				return false;
			}

			UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName, EGPD_Output, /*bRequireDir*/ false);
			UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName, EGPD_Input, /*bRequireDir*/ false);
			if (!FromPin || !ToPin)
			{
				SololmcpError::NotFound(OutStructured, TEXT("from_pin or to_pin"));
				OutError = TEXT("Pin not found.");
				return false;
			}
			if (!PinsAreLinked(FromPin, ToPin))
			{
				SololmcpError::Set(OutStructured, TEXT("NO_OP"), TEXT(""),
					TEXT("Pins were not connected before disconnect; refusing to report a successful mutation."));
				OutStructured->SetBoolField(TEXT("disconnected_verified"), false);
				OutError = TEXT("Pins are not connected.");
				return false;
			}

			const UEdGraphSchema* Schema = Graph->GetSchema();
			TSharedRef<FJsonObject> BeforeSnapshot = BuildNiagaraGraphSnapshotJson(Script, Graph);
			const FString FromPinRefBefore = FString::Printf(TEXT("%s.%s"), *FromNode->NodeGuid.ToString(), *FromPin->PinName.ToString());
			const FString ToPinRefBefore = FString::Printf(TEXT("%s.%s"), *ToNode->NodeGuid.ToString(), *ToPin->PinName.ToString());
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraScriptDisconnectPins", "SOMOLMCP Disconnect Niagara Pins"));
			Script->Modify();
			Graph->Modify();
			FromNode->Modify();
			ToNode->Modify();

			// TODO(P1-4): Schema->BreakSinglePinLink covers only one half of the bidirectional link
			// in some UE branches; we explicitly call it on both pins in either order to be safe.
			if (Schema)
			{
				Schema->BreakSinglePinLink(FromPin, ToPin);
			}
			else
			{
				// Fallback: direct pin manipulation.
				FromPin->BreakLinkTo(ToPin);
				ToPin->BreakLinkTo(FromPin);
			}
			FromPin = FindPinByName(FromNode, FromPinName, EGPD_Output, /*bRequireDir*/ false);
			ToPin = FindPinByName(ToNode, ToPinName, EGPD_Input, /*bRequireDir*/ false);
			const bool bFromResolvedAfter = IsPinOwnedByNode(FromPin, FromNode);
			const bool bToResolvedAfter = IsPinOwnedByNode(ToPin, ToNode);
			const bool bStillLinkedAfter = bFromResolvedAfter && bToResolvedAfter && PinsAreLinked(FromPin, ToPin);
			if (bStillLinkedAfter)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("Pin link still exists after disconnect attempt."));
				OutStructured->SetBoolField(TEXT("disconnected_verified"), false);
				OutError = TEXT("Disconnect validation failed.");
				return false;
			}

			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetBoolField(TEXT("disconnected_verified"), true);
			OutStructured->SetStringField(TEXT("from"), FromPinRefBefore);
			OutStructured->SetStringField(TEXT("to"), ToPinRefBefore);
			OutStructured->SetBoolField(TEXT("from_pin_resolved_after_mutation"), bFromResolvedAfter);
			OutStructured->SetBoolField(TEXT("to_pin_resolved_after_mutation"), bToResolvedAfter);
			OutStructured->SetStringField(TEXT("readback_mode"), (bFromResolvedAfter && bToResolvedAfter) ? TEXT("resolved_pins_after_mutation") : TEXT("pin_rebuilt_or_removed_after_mutation"));
			TSharedRef<FJsonObject> AfterSnapshot = BuildNiagaraGraphSnapshotJson(Script, Graph);
			OutStructured->SetObjectField(TEXT("pre_edit_snapshot"), BeforeSnapshot);
			OutStructured->SetObjectField(TEXT("post_edit_snapshot"), AfterSnapshot);
			AttachNiagaraGraphEditReceipt(
				OutStructured,
				Script,
				Graph,
				BeforeSnapshot,
				AfterSnapshot,
				TEXT("disconnect_pins"),
				TEXT("disconnected_verified"),
				true);
			OutSummary = TEXT("Disconnected Niagara graph pins.");
			return true;
		}
	});

	// ----- niagara_script_delete_node ------------------------------------
	Registry.Register({
		TEXT("niagara_script_delete_node"),
		TEXT("Delete one node from a Niagara script graph with the same disposable-target guard used by restore. Intended for rollback cleanup and disposable smoke assets."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraScript asset path."))},
			{TEXT("node_id"), FSololmcpSchemaBuilder::String(TEXT("NodeGuid (preferred) or UObject name."))},
			{TEXT("allow_disposable_write"), FSololmcpSchemaBuilder::Boolean(TEXT("Required unless allow_production_delete=true."))},
			{TEXT("allow_production_delete"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicit override for production assets; off by default."))},
			{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after deletion; default false."))}
		}, {TEXT("script_path"), TEXT("node_id")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
			{
				return false;
			}

			FString NodeId;
			if (!Arguments->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("node_id"));
				OutError = TEXT("Missing node_id.");
				return false;
			}

			const FString ScriptPath = Script ? Script->GetPathName() : FString();
			bool bAllowDisposableWrite = false;
			Arguments->TryGetBoolField(TEXT("allow_disposable_write"), bAllowDisposableWrite);
			bool bAllowProductionDelete = false;
			Arguments->TryGetBoolField(TEXT("allow_production_delete"), bAllowProductionDelete);
			const bool bSafeDisposablePath = IsSafeDisposableNiagaraRollbackPath(ScriptPath);
			if (!bAllowProductionDelete && (!bAllowDisposableWrite || !bSafeDisposablePath))
			{
				SololmcpError::Set(OutStructured, TEXT("UNSAFE_TARGET"), TEXT("script_path"),
					TEXT("Node deletion is blocked unless allow_disposable_write=true and the script is under /Game/SOMOLMCP/Disposable* (or allow_production_delete=true is explicitly supplied)."));
				OutStructured->SetBoolField(TEXT("allow_disposable_write"), bAllowDisposableWrite);
				OutStructured->SetBoolField(TEXT("safe_disposable_path"), bSafeDisposablePath);
				OutStructured->SetBoolField(TEXT("allow_production_delete"), bAllowProductionDelete);
				OutError = TEXT("Niagara node deletion blocked by disposable target guard.");
				return false;
			}

			UEdGraphNode* Node = FindNodeByIdLocal(Graph, NodeId);
			if (!Node)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("node '%s'"), *NodeId));
				OutError = TEXT("node_id not found.");
				return false;
			}

			const FString PreHash = BuildNiagaraGraphHash(Graph);
			const FString DeletedGuid = Node->NodeGuid.ToString();
			const FString DeletedName = Node->GetName();
			const FString DeletedClass = Node->GetClass() ? Node->GetClass()->GetName() : TEXT("Unknown");
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraScriptDeleteNode", "SOMOLMCP Delete Niagara Script Node"));
			Script->Modify();
			Graph->Modify();
			Node->Modify();
			Node->BreakAllNodeLinks();
			Graph->RemoveNode(Node);
			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			if (FindNodeByIdLocal(Graph, DeletedGuid) || FindNodeByIdLocal(Graph, DeletedName))
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("node_id"), TEXT("Node still exists after Graph->RemoveNode."));
				OutError = TEXT("Niagara node delete validation failed.");
				return false;
			}

			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.niagara_script_delete_node.v1"));
			OutStructured->SetStringField(TEXT("script_path"), ScriptPath);
			OutStructured->SetStringField(TEXT("deleted_node_id"), DeletedGuid);
			OutStructured->SetStringField(TEXT("deleted_node_name"), DeletedName);
			OutStructured->SetStringField(TEXT("deleted_node_class"), DeletedClass);
			OutStructured->SetStringField(TEXT("pre_hash"), PreHash);
			OutStructured->SetStringField(TEXT("post_hash"), BuildNiagaraGraphHash(Graph));
			OutStructured->SetBoolField(TEXT("allow_disposable_write"), bAllowDisposableWrite);
			OutStructured->SetBoolField(TEXT("safe_disposable_path"), bSafeDisposablePath);
			OutStructured->SetBoolField(TEXT("allow_production_delete"), bAllowProductionDelete);
			OutStructured->SetObjectField(TEXT("post_snapshot"), BuildNiagaraGraphSnapshotJson(Script, Graph));
			if (!MaybeSaveNiagaraScriptLocal(Context.Services, Arguments, ScriptPath, OutStructured, OutError))
			{
				return false;
			}
			OutSummary = FString::Printf(TEXT("Deleted Niagara node '%s' from '%s'."), *DeletedName, *Script->GetName());
			return true;
		}
	});

	// ----- niagara_script_set_pin_default --------------------------------
	Registry.Register({
		TEXT("niagara_script_set_pin_default"),
		TEXT("Set a Niagara graph pin default value through the graph schema and verify the exported pin default changed. Useful for module inputs, parameters, and Data Interface node pins represented in the script graph."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("node_id"), FSololmcpSchemaBuilder::String(TEXT("NodeGuid (preferred) or UObject name."))},
			{TEXT("pin_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("direction"), FSololmcpSchemaBuilder::String(TEXT("input|output|any; default any."))},
			{TEXT("value"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Any JSON scalar/object; stringified before schema default import."))},
			{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after mutation; default false."))}
		}, {TEXT("script_path"), TEXT("node_id"), TEXT("pin_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString NodeId, PinName, Direction;
			if (!Arguments->TryGetStringField(TEXT("node_id"), NodeId)) { SololmcpError::MissingParam(OutStructured, TEXT("node_id")); OutError = TEXT("Missing node_id."); return false; }
			if (!Arguments->TryGetStringField(TEXT("pin_name"), PinName)) { SololmcpError::MissingParam(OutStructured, TEXT("pin_name")); OutError = TEXT("Missing pin_name."); return false; }
			Arguments->TryGetStringField(TEXT("direction"), Direction);
			TSharedPtr<FJsonValue> ValueField = Arguments->TryGetField(TEXT("value"));
			if (!ValueField.IsValid())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("value"));
				OutError = TEXT("Missing value.");
				return false;
			}

			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
			{
				return false;
			}

			UEdGraphNode* Node = FindNodeByIdLocal(Graph, NodeId);
			if (!Node)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("node '%s'"), *NodeId));
				OutError = TEXT("node_id not found.");
				return false;
			}

			const FString DirNorm = Direction.TrimStartAndEnd().ToLower();
			const bool bRequireDir = DirNorm == TEXT("input") || DirNorm == TEXT("output");
			const EEdGraphPinDirection RequiredDir = DirNorm == TEXT("output") ? EGPD_Output : EGPD_Input;
			UEdGraphPin* Pin = FindPinByName(Node, PinName, RequiredDir, bRequireDir);
			if (!Pin)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("pin '%s'"), *PinName));
				OutError = TEXT("pin_name not found.");
				return false;
			}

			const FString OldDefault = Pin->DefaultValue;
			const FString NewDefault = JsonValueToImportString(ValueField);
			const UEdGraphSchema* Schema = Graph->GetSchema();
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraScriptSetPinDefault", "SOMOLMCP Set Niagara Pin Default"));
			Script->Modify();
			Graph->Modify();
			Node->Modify();
			Pin->Modify();

			bool bSchemaApplied = false;
			if (Schema)
			{
				Schema->TrySetDefaultValue(*Pin, NewDefault);
				bSchemaApplied = true;
			}
			if (!bSchemaApplied)
			{
				Pin->DefaultValue = NewDefault;
			}
			Node->PinDefaultValueChanged(Pin);
			Node->ReconstructNode();
			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			OutStructured->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			OutStructured->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
			OutStructured->SetStringField(TEXT("old_default_value"), OldDefault);
			OutStructured->SetStringField(TEXT("new_default_value"), Pin->DefaultValue);
			OutStructured->SetBoolField(TEXT("schema_applied"), bSchemaApplied);
			OutStructured->SetBoolField(TEXT("changed"), OldDefault != Pin->DefaultValue);
			if (!MaybeSaveNiagaraScriptLocal(Context.Services, Arguments, Script->GetPathName(), OutStructured, OutError))
			{
				return false;
			}
			if (OldDefault == Pin->DefaultValue && OldDefault != NewDefault)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"), TEXT("Pin default write did not verify after schema/default fallback."));
				OutError = TEXT("Niagara pin default write did not verify.");
				return false;
			}
			OutSummary = FString::Printf(TEXT("Set Niagara pin default %s.%s."), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
			return true;
		}
	});

	// ----- niagara_script_set_node_property ------------------------------
	Registry.Register({
		TEXT("niagara_script_set_node_property"),
		TEXT("Set a UPROPERTY on a Niagara graph node via FProperty::ImportText_Direct."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("node_id"), FSololmcpSchemaBuilder::String(TEXT("NodeGuid (preferred) or UObject name."))},
			{TEXT("property_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("value"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Any JSON. Will be stringified for ImportText_Direct."))}
		}, {TEXT("script_path"), TEXT("node_id"), TEXT("property_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString NodeId, PropertyName;
			if (!Arguments->TryGetStringField(TEXT("node_id"), NodeId)) { SololmcpError::MissingParam(OutStructured, TEXT("node_id")); OutError = TEXT("Missing node_id."); return false; }
			if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName)) { SololmcpError::MissingParam(OutStructured, TEXT("property_name")); OutError = TEXT("Missing property_name."); return false; }

			TSharedPtr<FJsonValue> ValueField = Arguments->TryGetField(TEXT("value"));
			if (!ValueField.IsValid())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("value"));
				OutError = TEXT("Missing value.");
				return false;
			}

			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
			{
				return false;
			}

			UEdGraphNode* Node = FindNodeByIdLocal(Graph, NodeId);
			if (!Node)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("node '%s'"), *NodeId));
				OutError = TEXT("node_id not found.");
				return false;
			}

			FProperty* Property = Node->GetClass()->FindPropertyByName(FName(*PropertyName));
			if (!Property)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("property '%s' on %s"), *PropertyName, *Node->GetClass()->GetName()));
				OutError = TEXT("property_name not found on node class.");
				return false;
			}

			// Capture old value (best-effort ExportText).
			FString OldValueStr;
			Property->ExportText_Direct(OldValueStr,
				Property->ContainerPtrToValuePtr<void>(Node),
				Property->ContainerPtrToValuePtr<void>(Node),
				nullptr, PPF_None);

			const FString Stringified = JsonValueToImportString(ValueField);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraScriptSetNodeProperty", "SOMOLMCP Set Niagara Node Property"));
			Script->Modify();
			Graph->Modify();
			Node->Modify();

			// TODO(P1-4): for some compound Niagara properties (FNiagaraVariable, FNiagaraTypeDefinition)
			// ImportText_Direct expects a precise (TypeName=X,Name="Y") string format. Callers
			// may need to pre-format value when targeting those properties.
			const TCHAR* ImportResult = Property->ImportText_Direct(*Stringified,
				Property->ContainerPtrToValuePtr<void>(Node),
				/*Owner*/ Node, PPF_None);
			if (ImportResult == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"),
					FString::Printf(TEXT("ImportText_Direct could not parse '%s' as %s."), *Stringified, *Property->GetCPPType()));
				OutError = TEXT("ImportText_Direct failed.");
				return false;
			}

			// Re-export to capture the actual value as written.
			FString NewValueStr;
			Property->ExportText_Direct(NewValueStr,
				Property->ContainerPtrToValuePtr<void>(Node),
				Property->ContainerPtrToValuePtr<void>(Node),
				nullptr, PPF_None);
			if (NewValueStr == OldValueStr && Stringified != OldValueStr)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"),
					TEXT("Property import returned success but exported value did not change."));
				OutStructured->SetStringField(TEXT("old"), OldValueStr);
				OutStructured->SetStringField(TEXT("attempted_new"), Stringified);
				OutError = TEXT("Niagara property write did not verify.");
				return false;
			}

			Node->ReconstructNode();
			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			OutStructured->SetStringField(TEXT("property"), PropertyName);
			OutStructured->SetStringField(TEXT("old"), OldValueStr);
			OutStructured->SetStringField(TEXT("new"), NewValueStr);
			OutStructured->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			OutSummary = FString::Printf(TEXT("Set %s.%s = %s."), *Node->GetClass()->GetName(), *PropertyName, *NewValueStr);
			return true;
		}
	});

	// ----- niagara_script_layout_graph -----------------------------------
	Registry.Register({
		TEXT("niagara_script_layout_graph"),
		TEXT("Auto-layout the Niagara graph. Currently uses a deterministic grid; FNiagaraEditorUtilities::AutoLayout would require NiagaraEditorWidgets."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("script_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("step_x"), FSololmcpSchemaBuilder::Number(TEXT("Horizontal node spacing (default 320)."))},
			{TEXT("step_y"), FSololmcpSchemaBuilder::Number(TEXT("Vertical node spacing (default 220)."))},
			{TEXT("columns"), FSololmcpSchemaBuilder::Integer(TEXT("Grid columns (default 4)."))}
		}, {TEXT("script_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			if (!ResolveScriptAndGraph(Context, Arguments, OutStructured, Script, Graph, OutError))
			{
				return false;
			}

			const float StepX = Arguments->HasTypedField<EJson::Number>(TEXT("step_x"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("step_x"))) : 320.f;
			const float StepY = Arguments->HasTypedField<EJson::Number>(TEXT("step_y"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("step_y"))) : 220.f;
			int32 Columns = Arguments->HasTypedField<EJson::Number>(TEXT("columns"))
				? static_cast<int32>(Arguments->GetIntegerField(TEXT("columns"))) : 4;
			if (Columns < 1) { Columns = 1; }

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraScriptLayoutGraph", "SOMOLMCP Auto-Layout Niagara Graph"));
			Script->Modify();
			Graph->Modify();

#if 0
			// TODO(P1-4): the production-grade path would invoke
			//   FNiagaraEditorUtilities::AutoLayout(Graph)
			// (or whichever helper the user's UE 5.7 branch ships under
			// NiagaraEditor/NiagaraEditorWidgets). NiagaraEditorWidgets is not
			// listed in SOMOLMCP.Build.cs's PrivateDependencyModuleNames, so we
			// fall back to a manual grid below to remain compileable without
			// touching .Build.cs.
			FNiagaraEditorUtilities::AutoLayout(Graph);
#endif

			int32 NodeCount = 0;
			int32 RepositionedCount = 0;
			int32 Index = 0;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				++NodeCount;
				const int32 Col = Index % Columns;
				const int32 Row = Index / Columns;
				const int32 NewX = static_cast<int32>(Col * StepX);
				const int32 NewY = static_cast<int32>(Row * StepY);
				if (Node->NodePosX != NewX || Node->NodePosY != NewY)
				{
					Node->Modify();
					Node->NodePosX = NewX;
					Node->NodePosY = NewY;
					++RepositionedCount;
				}
				++Index;
			}
			if (NodeCount == 0)
			{
				SololmcpError::Set(OutStructured, TEXT("NO_OP"), TEXT("script_path"),
					TEXT("Niagara graph contains no nodes to lay out."));
				OutStructured->SetNumberField(TEXT("node_count"), 0);
				OutStructured->SetNumberField(TEXT("repositioned_count"), 0);
				OutError = TEXT("No Niagara graph nodes to lay out.");
				return false;
			}

			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			OutStructured->SetNumberField(TEXT("node_count"), NodeCount);
			OutStructured->SetNumberField(TEXT("repositioned_count"), RepositionedCount);
			OutStructured->SetNumberField(TEXT("columns"), Columns);
			OutStructured->SetNumberField(TEXT("step_x"), StepX);
			OutStructured->SetNumberField(TEXT("step_y"), StepY);
			OutStructured->SetStringField(TEXT("status"), RepositionedCount > 0 ? TEXT("updated") : TEXT("no_op"));
			OutSummary = FString::Printf(TEXT("Laid out %d Niagara node(s) (%d repositioned)."), NodeCount, RepositionedCount);
			return true;
		}
	});

	// ----- niagara_compile_diagnostics ----------------------------------
	Registry.Register({
		TEXT("niagara_compile_diagnostics"),
		TEXT("Compile or poll a Niagara system and return structured diagnostics. Validation failures are reported as compile_status/blocking_reasons instead of MCP tool failures."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraSystem asset path."))},
			{TEXT("force"), FSololmcpSchemaBuilder::Boolean(TEXT("Force a compile request when no compile is already outstanding; default false."))},
			{TEXT("request_compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Request/flush compile work before collecting diagnostics; default true."))},
			{TEXT("wait_for_completion"), FSololmcpSchemaBuilder::Boolean(TEXT("Wait for compile completion before readback; default true."))},
			{TEXT("wait_mode"), FSololmcpSchemaBuilder::String(TEXT("blocking|poll; default blocking. blocking uses UNiagaraSystem::WaitForCompilationComplete(false progress)."))},
			{TEXT("include_gpu_shaders"), FSololmcpSchemaBuilder::Boolean(TEXT("Include GPU shader compilation in outstanding/wait checks; default true."))},
			{TEXT("refresh_before_compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Refresh system parameter/order caches before compile; default follows request_compile."))},
			{TEXT("refresh_after_compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Refresh execution/render order after compile readback; default true."))},
			{TEXT("max_wait_seconds"), FSololmcpSchemaBuilder::Number(TEXT("Bounded poll wait budget when wait_mode=poll; default 30, max 300."))},
			{TEXT("poll_interval_seconds"), FSololmcpSchemaBuilder::Number(TEXT("Poll interval when wait_mode=poll; default 0.125."))},
			{TEXT("max_compile_events"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("max_asset_messages"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("include_vm_compile_events"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("include_asset_messages"), FSololmcpSchemaBuilder::Boolean()}
		}, {TEXT("system_asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString SystemAssetPath;
			if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("system_asset_path"));
				OutError = TEXT("Missing system_asset_path.");
				return false;
			}

			UNiagaraSystem* System = LoadNiagaraSystemLocal(Context.Services, SystemAssetPath, OutError);
			if (!System)
			{
				SololmcpError::InvalidPath(OutStructured, SystemAssetPath);
				return false;
			}

			const bool bForce = GetNiagaraBoolArg(Arguments, TEXT("force"), false);
			const bool bRequestCompile = GetNiagaraBoolArg(Arguments, TEXT("request_compile"), true);
			const bool bWait = GetNiagaraBoolArg(Arguments, TEXT("wait_for_completion"), true);
			const bool bIncludeGpuShaders = GetNiagaraBoolArg(Arguments, TEXT("include_gpu_shaders"), true);
			const bool bRefreshBeforeCompile = GetNiagaraBoolArg(Arguments, TEXT("refresh_before_compile"), bRequestCompile);
			const bool bRefreshAfterCompile = GetNiagaraBoolArg(Arguments, TEXT("refresh_after_compile"), true);
			const bool bIncludeVm = GetNiagaraBoolArg(Arguments, TEXT("include_vm_compile_events"), true);
			const bool bIncludeAssetMessages = GetNiagaraBoolArg(Arguments, TEXT("include_asset_messages"), true);
			FString WaitMode = GetNiagaraStringArg(Arguments, TEXT("wait_mode"), TEXT("blocking")).ToLower();
			if (WaitMode != TEXT("blocking") && WaitMode != TEXT("poll"))
			{
				WaitMode = TEXT("blocking");
			}

			int32 MaxVmEvents = 256;
			Arguments->TryGetNumberField(TEXT("max_compile_events"), MaxVmEvents);
			int32 MaxAssetMessages = 64;
			Arguments->TryGetNumberField(TEXT("max_asset_messages"), MaxAssetMessages);
			const double MaxWaitSeconds = GetNiagaraNumberArg(Arguments, TEXT("max_wait_seconds"), 30.0, 0.0, 300.0);
			const double PollIntervalSeconds = GetNiagaraNumberArg(Arguments, TEXT("poll_interval_seconds"), 0.125, 0.01, 5.0);

			const double StartedAtSeconds = FPlatformTime::Seconds();
			const bool bOutstandingBefore = System->HasOutstandingCompilationRequests(bIncludeGpuShaders);
			const bool bReadyBefore = System->IsReadyToRun();
			bool bRefreshedBeforeCompile = false;
			if (bRefreshBeforeCompile)
			{
				System->GraphSourceChanged();
#if ENGINE_MAJOR_VERSION < 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 8)
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					System->RefreshSystemParametersFromEmitter(Handle);
				}
#endif
				System->ComputeEmittersExecutionOrder();
				System->ComputeRenderersDrawOrder();
				bRefreshedBeforeCompile = true;
			}

			bool bRequested = false;
			FString RequestSkippedReason;
			if (bRequestCompile)
			{
				const bool bOutstandingAfterRefresh = System->HasOutstandingCompilationRequests(bIncludeGpuShaders);
				#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			const bool bHasActiveCompileAfterRefresh = System->HasActiveCompilations();
#else
			// UNiagaraSystem::HasActiveCompilations is 5.4+; 5.3 exposes no equivalent query.
			const bool bHasActiveCompileAfterRefresh = false;
#endif
				if (bOutstandingAfterRefresh && bHasActiveCompileAfterRefresh && !bForce)
				{
					RequestSkippedReason = TEXT("compile_already_outstanding");
				}
				else
				{
					bRequested = System->RequestCompile(bForce);
					if (!bRequested)
					{
						RequestSkippedReason = TEXT("request_compile_returned_false");
					}
				}
			}
			else
			{
				RequestSkippedReason = TEXT("request_compile_false");
			}

			int32 PollCount = 0;
			bool bPollCompleted = false;
			bool bBlockingWaitUsed = false;
			if (bWait)
			{
				if (WaitMode == TEXT("blocking"))
				{
					System->WaitForCompilationComplete(bIncludeGpuShaders, false);
					bBlockingWaitUsed = true;
					bPollCompleted = !System->HasOutstandingCompilationRequests(bIncludeGpuShaders);
				}
				else
				{
					const double DeadlineSeconds = FPlatformTime::Seconds() + MaxWaitSeconds;
					do
					{
						const bool bFlushPendingRequest = bRequestCompile;
						bPollCompleted = System->PollForCompilationComplete(bFlushPendingRequest) || bPollCompleted;
						++PollCount;
						if (!System->HasOutstandingCompilationRequests(bIncludeGpuShaders))
						{
							break;
						}
						FPlatformProcess::Sleep(PollIntervalSeconds);
					}
					while (FPlatformTime::Seconds() < DeadlineSeconds);
				}
			}

			if (bRefreshAfterCompile)
			{
				System->ComputeEmittersExecutionOrder();
				System->ComputeRenderersDrawOrder();
			}

			const bool bHasOutstandingCompilation = System->HasOutstandingCompilationRequests(bIncludeGpuShaders);
			#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
			const bool bHasActiveCompilations = System->HasActiveCompilations();
#else
			// UNiagaraSystem::HasActiveCompilations is 5.4+; 5.3 exposes no equivalent query.
			const bool bHasActiveCompilations = false;
#endif
			const bool bCompleted = !bHasOutstandingCompilation;
			const bool bReady = System->IsReadyToRun();

			TArray<TSharedPtr<FJsonValue>> Messages;
			TArray<TSharedPtr<FJsonValue>> BlockingReasons;
			if (!RequestSkippedReason.IsEmpty())
			{
				AddNiagaraDiagnosticMessage(Messages, TEXT("info"), RequestSkippedReason, TEXT("Compile request was not queued by this diagnostics call."));
			}
			if (!bCompleted)
			{
				AddNiagaraBlockingReason(BlockingReasons, TEXT("compile_pending"), TEXT("Niagara compilation is still outstanding after the requested wait strategy."));
				AddNiagaraDiagnosticMessage(Messages, TEXT("warning"), TEXT("compile_pending"), TEXT("Compilation not completed yet."));
			}
			if (!bReady)
			{
				AddNiagaraBlockingReason(BlockingReasons, TEXT("not_ready_to_run"), TEXT("UNiagaraSystem::IsReadyToRun() is false after compile diagnostics."));
				AddNiagaraDiagnosticMessage(Messages, TEXT("error"), TEXT("not_ready_to_run"), TEXT("Niagara system is not ready to run after compile diagnostics."));
			}

			TArray<TSharedPtr<FJsonValue>> VmEventsJson;
			int32 VmErrors = 0;
			int32 VmWarnings = 0;
#if WITH_EDITORONLY_DATA
			if (bIncludeVm)
			{
				System->ForEachScript([&](UNiagaraScript* Script)
				{
					if (!Script || MaxVmEvents <= 0)
					{
						return;
					}
					const FNiagaraVMExecutableData& VM = Script->GetVMExecutableData();
					const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
					const FString UsageStr = UsageEnum ? UsageEnum->GetNameStringByValue(static_cast<int64>(Script->GetUsage())) : FString();
					for (const FNiagaraCompileEvent& Event : VM.LastCompileEvents)
					{
						if (MaxVmEvents <= 0)
						{
							break;
						}
						TSharedRef<FJsonObject> EventJson = MakeShared<FJsonObject>();
						EventJson->SetStringField(TEXT("source"), TEXT("vm_compile_event"));
						EventJson->SetStringField(TEXT("severity"), NiagaraCompileSeverityToString(Event.Severity));
						EventJson->SetStringField(TEXT("text"), Event.Message);
						if (!Event.ShortDescription.IsEmpty())
						{
							EventJson->SetStringField(TEXT("short_description"), Event.ShortDescription);
						}
						if (Event.NodeGuid.IsValid())
						{
							EventJson->SetStringField(TEXT("node_guid"), Event.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						}
						if (Event.PinGuid.IsValid())
						{
							EventJson->SetStringField(TEXT("pin_guid"), Event.PinGuid.ToString(EGuidFormats::DigitsWithHyphens));
						}
						TArray<TSharedPtr<FJsonValue>> StackGuids;
						for (const FGuid& StackGuid : Event.StackGuids)
						{
							if (StackGuid.IsValid())
							{
								StackGuids.Add(MakeShared<FJsonValueString>(StackGuid.ToString(EGuidFormats::DigitsWithHyphens)));
							}
						}
						if (StackGuids.Num() > 0)
						{
							EventJson->SetArrayField(TEXT("stack_guids"), StackGuids);
						}
						EventJson->SetStringField(TEXT("script_usage"), UsageStr);
						EventJson->SetStringField(TEXT("script_path"), Script->GetPathName());
						VmEventsJson.Add(MakeShared<FJsonValueObject>(EventJson));
						--MaxVmEvents;
						if (Event.Severity == FNiagaraCompileEventSeverity::Error)
						{
							++VmErrors;
						}
						else if (Event.Severity == FNiagaraCompileEventSeverity::Warning)
						{
							++VmWarnings;
						}
					}
				});
			}
#endif
			if (VmErrors > 0)
			{
				AddNiagaraBlockingReason(BlockingReasons, TEXT("vm_compile_errors"), FString::Printf(TEXT("%d Niagara VM compile error(s) were reported."), VmErrors));
			}

			TArray<TSharedPtr<FJsonValue>> AssetMessagesJson;
#if WITH_EDITORONLY_DATA
			if (bIncludeAssetMessages)
			{
				const TMap<FGuid, TObjectPtr<UNiagaraMessageDataBase>>& RawMessages = System->GetMessageStore().GetMessages();
				for (const TPair<FGuid, TObjectPtr<UNiagaraMessageDataBase>>& Pair : RawMessages)
				{
					if (MaxAssetMessages <= 0)
					{
						break;
					}
					UNiagaraMessageDataBase* Base = Pair.Value.Get();
					UNiagaraMessageData* MessageData = Base ? Cast<UNiagaraMessageData>(Base) : nullptr;
					if (!MessageData)
					{
						continue;
					}
					TSharedRef<const INiagaraMessage> Message = MessageData->GenerateNiagaraMessage();
					TSharedRef<FJsonObject> MessageJson = MakeShared<FJsonObject>();
					MessageJson->SetStringField(TEXT("source"), TEXT("asset_message_store"));
					MessageJson->SetStringField(TEXT("message_key"), Pair.Key.ToString(EGuidFormats::DigitsWithHyphens));
					MessageJson->SetStringField(TEXT("title"), Message->GenerateMessageTitle().ToString());
					MessageJson->SetStringField(TEXT("text"), Message->GenerateMessageText().ToString());
					MessageJson->SetStringField(TEXT("topic"), Message->GetMessageTopic().ToString());
					AssetMessagesJson.Add(MakeShared<FJsonValueObject>(MessageJson));
					--MaxAssetMessages;
				}
			}
#endif

			const bool bCompilePassed = bCompleted && bReady && VmErrors == 0;
			FString CompileStatus = TEXT("passed");
			if (VmErrors > 0)
			{
				CompileStatus = TEXT("failed_vm_errors");
			}
			else if (!bCompleted)
			{
				CompileStatus = TEXT("pending");
			}
			else if (!bReady)
			{
				CompileStatus = TEXT("not_ready");
			}

			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.niagara_compile_diagnostics.v2"));
			OutStructured->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
			OutStructured->SetStringField(TEXT("resolved_system_asset_path"), System->GetPathName());
			OutStructured->SetBoolField(TEXT("force"), bForce);
			OutStructured->SetBoolField(TEXT("request_compile"), bRequestCompile);
			OutStructured->SetBoolField(TEXT("requested"), bRequested);
			if (!RequestSkippedReason.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("request_skipped_reason"), RequestSkippedReason);
			}
			OutStructured->SetBoolField(TEXT("wait_for_completion"), bWait);
			OutStructured->SetStringField(TEXT("wait_mode"), WaitMode);
			OutStructured->SetBoolField(TEXT("blocking_wait_used"), bBlockingWaitUsed);
			OutStructured->SetNumberField(TEXT("poll_count"), PollCount);
			OutStructured->SetBoolField(TEXT("poll_completed"), bPollCompleted);
			OutStructured->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - StartedAtSeconds);
			OutStructured->SetBoolField(TEXT("include_gpu_shaders"), bIncludeGpuShaders);
			OutStructured->SetBoolField(TEXT("refreshed_before_compile"), bRefreshedBeforeCompile);
			OutStructured->SetBoolField(TEXT("refreshed_after_compile"), bRefreshAfterCompile);
			OutStructured->SetBoolField(TEXT("outstanding_before"), bOutstandingBefore);
			OutStructured->SetBoolField(TEXT("ready_to_run_before"), bReadyBefore);
			OutStructured->SetBoolField(TEXT("completed"), bCompleted);
			OutStructured->SetBoolField(TEXT("ready_to_run"), bReady);
			OutStructured->SetBoolField(TEXT("has_outstanding_compilation"), bHasOutstandingCompilation);
			OutStructured->SetBoolField(TEXT("has_active_compilations"), bHasActiveCompilations);
			OutStructured->SetStringField(TEXT("compile_status"), CompileStatus);
			OutStructured->SetBoolField(TEXT("compile_passed"), bCompilePassed);
			OutStructured->SetArrayField(TEXT("blocking_reasons"), BlockingReasons);
			OutStructured->SetArrayField(TEXT("messages"), Messages);
			OutStructured->SetArrayField(TEXT("vm_compile_events"), VmEventsJson);
			OutStructured->SetArrayField(TEXT("asset_messages"), AssetMessagesJson);
			OutStructured->SetNumberField(TEXT("vm_compile_event_error_count"), VmErrors);
			OutStructured->SetNumberField(TEXT("vm_compile_event_warning_count"), VmWarnings);
			OutStructured->SetNumberField(TEXT("message_count"), Messages.Num());
			OutStructured->SetStringField(TEXT("diagnostic_summary"), bCompilePassed
				? TEXT("Niagara compile diagnostics passed.")
				: SummarizeNiagaraBlockingReasons(BlockingReasons));
			OutStructured->SetBoolField(TEXT("receipt_gate_complete"), bCompilePassed);
			OutStructured->SetStringField(TEXT("receipt_gate_status"), bCompilePassed ? TEXT("compile_passed") : TEXT("failed_validation"));
			if (!bCompilePassed)
			{
				OutStructured->SetObjectField(TEXT("structured_failure"), MakeNiagaraStructuredFailure(
					TEXT("NIAGARA_COMPILE_DIAGNOSTICS_FAILED"),
					SummarizeNiagaraBlockingReasons(BlockingReasons),
					TEXT("compile_diagnostics")));
			}

			OutSummary = bCompilePassed
				? TEXT("Niagara compile diagnostics passed.")
				: TEXT("Niagara compile diagnostics returned structured blocking reasons.");
			return true;
		}
	});

	// ----- niagara_authoring_acceptance_check ----------------------------
	Registry.Register({
		TEXT("niagara_authoring_acceptance_check"),
		TEXT("Aggregate Niagara authoring proof after complex mutations: system snapshot, optional script graph explain, compile diagnostics, and optional runtime preview snapshot."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String(TEXT("UNiagaraSystem asset path for compile diagnostics and system snapshot."))},
			{TEXT("script_path"), FSololmcpSchemaBuilder::String(TEXT("Optional UNiagaraScript path for graph explain."))},
			{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Optional preview actor for niagara_runtime_snapshot."))},
			{TEXT("force_compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Force Niagara compile diagnostics; default true."))},
			{TEXT("request_compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Request/flush compile work; default true."))},
			{TEXT("wait_for_completion"), FSololmcpSchemaBuilder::Boolean(TEXT("Wait for compile completion; default true."))},
			{TEXT("wait_mode"), FSololmcpSchemaBuilder::String(TEXT("blocking|poll; forwarded to niagara_compile_diagnostics."))},
			{TEXT("include_gpu_shaders"), FSololmcpSchemaBuilder::Boolean(TEXT("Include GPU shader compile completion in the acceptance gate; default true."))},
			{TEXT("require_runtime_preview"), FSololmcpSchemaBuilder::Boolean(TEXT("Require runtime preview/readback evidence. Default true for live-gate acceptance."))}
		}, {TEXT("system_asset_path")}),

		[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString SystemAssetPath;
			if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("system_asset_path"));
				OutError = TEXT("Missing system_asset_path.");
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> BlockingReasons;
			TSharedRef<FJsonObject> SnapshotArgs = MakeShared<FJsonObject>();
			SnapshotArgs->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
			TSharedRef<FJsonObject> SnapshotOut = MakeShared<FJsonObject>();
			FString SnapshotSummary;
			FString SnapshotError;
			const bool bSnapshotOk = Registry.ExecuteTool(TEXT("niagara_system_authoring_snapshot"), SnapshotArgs, SnapshotOut, SnapshotSummary, SnapshotError);
			OutStructured->SetBoolField(TEXT("system_snapshot_ok"), bSnapshotOk);
			OutStructured->SetObjectField(TEXT("system_snapshot"), SnapshotOut);
			if (!SnapshotError.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("system_snapshot_error"), SnapshotError);
			}
			if (!bSnapshotOk)
			{
				AddNiagaraBlockingReason(BlockingReasons, TEXT("system_snapshot_failed"), SnapshotError.IsEmpty() ? TEXT("niagara_system_authoring_snapshot did not return a readable snapshot.") : SnapshotError);
			}

			FString ScriptPath;
			bool bGraphExplainRequested = false;
			bool bGraphExplainOk = true;
			if (Arguments->TryGetStringField(TEXT("script_path"), ScriptPath) && !ScriptPath.IsEmpty())
			{
				bGraphExplainRequested = true;
				TSharedRef<FJsonObject> ExplainArgs = MakeShared<FJsonObject>();
				ExplainArgs->SetStringField(TEXT("script_path"), ScriptPath);
				TSharedRef<FJsonObject> ExplainOut = MakeShared<FJsonObject>();
				FString ExplainSummary;
				FString ExplainError;
				bGraphExplainOk = Registry.ExecuteTool(TEXT("niagara_graph_explain"), ExplainArgs, ExplainOut, ExplainSummary, ExplainError);
				OutStructured->SetBoolField(TEXT("graph_explain_ok"), bGraphExplainOk);
				OutStructured->SetObjectField(TEXT("graph_explain"), ExplainOut);
				if (!ExplainError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("graph_explain_error"), ExplainError);
				}
				if (!bGraphExplainOk)
				{
					AddNiagaraBlockingReason(BlockingReasons, TEXT("graph_explain_failed"), ExplainError.IsEmpty() ? TEXT("niagara_graph_explain did not return readable graph evidence.") : ExplainError);
				}
			}
			OutStructured->SetBoolField(TEXT("graph_explain_requested"), bGraphExplainRequested);

			TSharedRef<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
			CompileArgs->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
			CompileArgs->SetBoolField(TEXT("force"), Arguments->HasTypedField<EJson::Boolean>(TEXT("force_compile")) ? Arguments->GetBoolField(TEXT("force_compile")) : true);
			CompileArgs->SetBoolField(TEXT("request_compile"), GetNiagaraBoolArg(Arguments, TEXT("request_compile"), true));
			CompileArgs->SetBoolField(TEXT("wait_for_completion"), Arguments->HasTypedField<EJson::Boolean>(TEXT("wait_for_completion")) ? Arguments->GetBoolField(TEXT("wait_for_completion")) : true);
			CompileArgs->SetStringField(TEXT("wait_mode"), GetNiagaraStringArg(Arguments, TEXT("wait_mode"), TEXT("blocking")));
			CompileArgs->SetBoolField(TEXT("include_gpu_shaders"), GetNiagaraBoolArg(Arguments, TEXT("include_gpu_shaders"), true));
			TSharedRef<FJsonObject> CompileOut = MakeShared<FJsonObject>();
			FString CompileSummary;
			FString CompileError;
			const bool bCompileOk = Registry.ExecuteTool(TEXT("niagara_compile_diagnostics"), CompileArgs, CompileOut, CompileSummary, CompileError);
			const bool bCompilePassed = bCompileOk && NiagaraCompileDiagnosticsPassed(CompileOut);
			FString CompileStatus;
			CompileOut->TryGetStringField(TEXT("compile_status"), CompileStatus);
			OutStructured->SetBoolField(TEXT("compile_tool_ok"), bCompileOk);
			OutStructured->SetBoolField(TEXT("compile_passed"), bCompilePassed);
			if (!CompileStatus.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("compile_status"), CompileStatus);
			}
			OutStructured->SetObjectField(TEXT("compile"), CompileOut);
			if (!CompileError.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("compile_error"), CompileError);
			}
			if (!bCompileOk)
			{
				AddNiagaraBlockingReason(BlockingReasons, TEXT("compile_diagnostics_tool_failed"), CompileError.IsEmpty() ? TEXT("niagara_compile_diagnostics did not execute successfully.") : CompileError);
			}
			else if (!bCompilePassed)
			{
				FString DiagnosticSummary;
				CompileOut->TryGetStringField(TEXT("diagnostic_summary"), DiagnosticSummary);
				AddNiagaraBlockingReason(BlockingReasons, CompileStatus.IsEmpty() ? TEXT("compile_not_accepted") : CompileStatus, DiagnosticSummary.IsEmpty() ? TEXT("Compile diagnostics did not satisfy the acceptance gate.") : DiagnosticSummary);
			}

			FString Actor;
			const bool bRequireRuntimePreview = GetNiagaraBoolArg(Arguments, TEXT("require_runtime_preview"), true);
			bool bRuntimePreviewOk = !bRequireRuntimePreview;
			bool bRuntimePreviewRequested = false;
			if (Arguments->TryGetStringField(TEXT("actor"), Actor) && !Actor.IsEmpty())
			{
				bRuntimePreviewRequested = true;
				TSharedRef<FJsonObject> RuntimeArgs = MakeShared<FJsonObject>();
				RuntimeArgs->SetStringField(TEXT("actor"), Actor);
				TSharedRef<FJsonObject> RuntimeOut = MakeShared<FJsonObject>();
				FString RuntimeSummary;
				FString RuntimeError;
				bRuntimePreviewOk = Registry.ExecuteTool(TEXT("niagara_runtime_snapshot"), RuntimeArgs, RuntimeOut, RuntimeSummary, RuntimeError);
				OutStructured->SetBoolField(TEXT("runtime_preview_ok"), bRuntimePreviewOk);
				OutStructured->SetObjectField(TEXT("runtime_preview"), RuntimeOut);
				if (!RuntimeError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("runtime_preview_error"), RuntimeError);
				}
				if (!bRuntimePreviewOk)
				{
					AddNiagaraBlockingReason(BlockingReasons, TEXT("runtime_preview_failed"), RuntimeError.IsEmpty() ? TEXT("niagara_runtime_snapshot did not return runtime preview evidence.") : RuntimeError);
				}
			}
			else if (bRequireRuntimePreview)
			{
				AddNiagaraBlockingReason(BlockingReasons, TEXT("runtime_preview_missing"), TEXT("A live-gate acceptance receipt requires actor plus niagara_runtime_snapshot or niagara_preview_capture evidence."));
			}

			const bool bAccepted = bSnapshotOk && bGraphExplainOk && bCompilePassed && bRuntimePreviewOk;
			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.niagara_authoring_acceptance_check.v2"));
			OutStructured->SetBoolField(TEXT("accepted"), bAccepted);
			OutStructured->SetStringField(TEXT("acceptance_status"), bAccepted ? TEXT("accepted") : TEXT("failed_validation"));
			OutStructured->SetStringField(TEXT("status"), bAccepted ? TEXT("accepted") : TEXT("failed_validation"));
			OutStructured->SetBoolField(TEXT("require_runtime_preview"), bRequireRuntimePreview);
			OutStructured->SetBoolField(TEXT("runtime_preview_requested"), bRuntimePreviewRequested);
			OutStructured->SetBoolField(TEXT("receipt_gate_complete"), bAccepted);
			OutStructured->SetStringField(TEXT("receipt_gate_status"), bAccepted ? TEXT("accepted") : TEXT("failed_validation"));
			OutStructured->SetArrayField(TEXT("blocking_reasons"), BlockingReasons);
			OutStructured->SetStringField(TEXT("diagnostic_summary"), bAccepted
				? TEXT("Niagara authoring acceptance check passed.")
				: SummarizeNiagaraBlockingReasons(BlockingReasons));
			if (!bAccepted)
			{
				OutStructured->SetObjectField(TEXT("structured_failure"), MakeNiagaraStructuredFailure(
					TEXT("NIAGARA_AUTHORING_GATE_FAILED"),
					SummarizeNiagaraBlockingReasons(BlockingReasons),
					TEXT("compile_readback_preview_acceptance")));
			}
			OutSummary = bAccepted
				? TEXT("Niagara authoring acceptance check passed.")
				: TEXT("Niagara authoring acceptance check returned structured blocking evidence.");
			return true;
		}
	});
}

} // namespace UE::SOMOLMCP
