// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpNiagaraHLSLTools.cpp
// ----------------------------------------------------------------------------
// Niagara Custom HLSL authoring tools.
//
// UE 5.7 keeps UNiagaraNodeCustomHlsl and UNiagaraNodeWithDynamicPins as
// MinimalAPI. Calling SetCustomHlsl/GetCustomHlsl/RequestNewTypedPin/IsAddPin
// directly from this plugin compiles but fails to link. This file therefore
// uses only exported/common graph operations plus UPROPERTY reflection for the
// CustomHlsl field and manual signature rebuilds from pins.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"

#include "SololmcpErrorHelpers.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpWriteFlush.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace
{
	const FName CustomHlslPropertyName(TEXT("CustomHlsl"));
	const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));

	UNiagaraScript* LoadNiagaraScript(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
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

	UNiagaraGraph* GetScriptGraph(UNiagaraScript* Script)
	{
		if (!Script)
		{
			return nullptr;
		}
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		return Source ? Source->NodeGraph : nullptr;
	}

	bool ResolveScriptGraph(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		const TSharedRef<FJsonObject>& OutStructured,
		UNiagaraScript*& OutScript,
		UNiagaraGraph*& OutGraph,
		FString& OutAssetPath,
		FString& OutError)
	{
		if (!Arguments->TryGetStringField(TEXT("script_path"), OutAssetPath))
		{
			SololmcpError::MissingParam(OutStructured, TEXT("script_path"));
			OutError = TEXT("Missing script_path.");
			return false;
		}

		OutScript = LoadNiagaraScript(Context.Services, OutAssetPath, OutError);
		if (!OutScript)
		{
			SololmcpError::InvalidPath(OutStructured, OutAssetPath);
			return false;
		}
		OutGraph = GetScriptGraph(OutScript);
		if (!OutGraph)
		{
			SololmcpError::NotFound(OutStructured, TEXT("UNiagaraGraph"));
			OutError = TEXT("Niagara script has no valid UNiagaraGraph source.");
			return false;
		}
		return true;
	}

	bool IsAddPinLocal(const UEdGraphPin* Pin)
	{
		return Pin &&
			Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc &&
			Pin->PinType.PinSubCategory == DynamicAddPinSubCategory;
	}

	UEdGraphNode* FindNodeById(UNiagaraGraph* Graph, const FString& NodeId)
	{
		if (!Graph || NodeId.IsEmpty())
		{
			return nullptr;
		}

		FGuid Guid;
		const bool bGuid = FGuid::Parse(NodeId, Guid);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if ((bGuid && Node->NodeGuid == Guid) || Node->GetName() == NodeId)
			{
				return Node;
			}
		}
		return nullptr;
	}

	bool IsCustomHlslNode(const UEdGraphNode* Node)
	{
		return Node && Node->GetClass()->IsChildOf(UNiagaraNodeCustomHlsl::StaticClass());
	}

	FString TypeToString(const FNiagaraTypeDefinition& Type)
	{
		if (Type == FNiagaraTypeDefinition::GetParameterMapDef()) { return TEXT("parameter_map"); }
		if (Type == FNiagaraTypeDefinition::GetFloatDef()) { return TEXT("float"); }
		if (Type == FNiagaraTypeDefinition::GetBoolDef()) { return TEXT("bool"); }
		if (Type == FNiagaraTypeDefinition::GetIntDef()) { return TEXT("int"); }
		if (Type == FNiagaraTypeDefinition::GetVec2Def()) { return TEXT("vec2"); }
		if (Type == FNiagaraTypeDefinition::GetVec3Def()) { return TEXT("vec3"); }
		if (Type == FNiagaraTypeDefinition::GetVec4Def()) { return TEXT("vec4"); }
		if (Type == FNiagaraTypeDefinition::GetPositionDef()) { return TEXT("position"); }
		if (Type == FNiagaraTypeDefinition::GetColorDef()) { return TEXT("color"); }
		return Type.GetName();
	}

	bool TypeFromString(const FString& InType, FNiagaraTypeDefinition& OutType, FString& OutError)
	{
		const FString T = InType.TrimStartAndEnd().ToLower();
		if (T.IsEmpty() || T == TEXT("float")) { OutType = FNiagaraTypeDefinition::GetFloatDef(); return true; }
		if (T == TEXT("bool") || T == TEXT("boolean")) { OutType = FNiagaraTypeDefinition::GetBoolDef(); return true; }
		if (T == TEXT("int") || T == TEXT("integer") || T == TEXT("int32")) { OutType = FNiagaraTypeDefinition::GetIntDef(); return true; }
		if (T == TEXT("vec2") || T == TEXT("vector2") || T == TEXT("float2")) { OutType = FNiagaraTypeDefinition::GetVec2Def(); return true; }
		if (T == TEXT("vec3") || T == TEXT("vector") || T == TEXT("vector3") || T == TEXT("float3")) { OutType = FNiagaraTypeDefinition::GetVec3Def(); return true; }
		if (T == TEXT("vec4") || T == TEXT("vector4") || T == TEXT("float4")) { OutType = FNiagaraTypeDefinition::GetVec4Def(); return true; }
		if (T == TEXT("position")) { OutType = FNiagaraTypeDefinition::GetPositionDef(); return true; }
		if (T == TEXT("color") || T == TEXT("linearcolor")) { OutType = FNiagaraTypeDefinition::GetColorDef(); return true; }
		if (T == TEXT("parameter_map") || T == TEXT("parametermap") || T == TEXT("map")) { OutType = FNiagaraTypeDefinition::GetParameterMapDef(); return true; }
		OutError = FString::Printf(TEXT("Unsupported Niagara HLSL pin type '%s'."), *InType);
		return false;
	}

	bool ReadPinSpecs(const TSharedRef<FJsonObject>& Arguments, const FString& FieldName, TArray<FNiagaraVariable>& OutVariables, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Arguments->TryGetArrayField(FieldName, Array) || !Array)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				OutError = FString::Printf(TEXT("%s entries must be objects."), *FieldName);
				return false;
			}

			FString Name;
			if (!(*Obj)->TryGetStringField(TEXT("name"), Name) || Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s entries require a non-empty name."), *FieldName);
				return false;
			}

			FString TypeName = TEXT("float");
			(*Obj)->TryGetStringField(TEXT("type"), TypeName);
			FNiagaraTypeDefinition Type;
			if (!TypeFromString(TypeName, Type, OutError))
			{
				return false;
			}
			OutVariables.Add(FNiagaraVariable(Type, FName(*Name)));
		}
		return true;
	}

	void SetSignature(UNiagaraNodeFunctionCall* Node, const FString& Name, const TArray<FNiagaraVariable>& Inputs, const TArray<FNiagaraVariable>& Outputs)
	{
		if (!Node)
		{
			return;
		}
		Node->Signature.Name = FName(*Name);
		Node->Signature.Inputs = Inputs;
		Node->Signature.Outputs.Reset();
		for (const FNiagaraVariable& Output : Outputs)
		{
			Node->Signature.Outputs.Add(Output);
		}

		if (FStrProperty* DisplayNameProp = FindFProperty<FStrProperty>(Node->GetClass(), TEXT("FunctionDisplayName")))
		{
			DisplayNameProp->SetPropertyValue_InContainer(Node, Name);
		}
	}

	void RebuildSignatureFromPins(UNiagaraNodeFunctionCall* Node)
	{
		if (!Node)
		{
			return;
		}
		const UEdGraphSchema_Niagara* Schema = Cast<UEdGraphSchema_Niagara>(Node->GetSchema());
		if (!Schema)
		{
			return;
		}

		FNiagaraFunctionSignature Sig = Node->Signature;
		Sig.Inputs.Empty();
		Sig.Outputs.Empty();

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || IsAddPinLocal(Pin))
			{
				continue;
			}
			if (Pin->Direction == EGPD_Input)
			{
				Sig.Inputs.Add(Schema->PinToNiagaraVariable(Pin, true));
			}
			else if (Pin->Direction == EGPD_Output)
			{
				Sig.Outputs.Add(Schema->PinToNiagaraVariable(Pin, false));
			}
		}
		Node->Signature = Sig;
	}

	bool SetCustomHlslReflected(UEdGraphNode* Node, const FString& Code, FString& OutError)
	{
		if (!IsCustomHlslNode(Node))
		{
			OutError = TEXT("Node is not a UNiagaraNodeCustomHlsl.");
			return false;
		}
		FStrProperty* Prop = FindFProperty<FStrProperty>(Node->GetClass(), CustomHlslPropertyName);
		if (!Prop)
		{
			OutError = TEXT("CustomHlsl UPROPERTY was not found by reflection.");
			return false;
		}

		Node->Modify();
		Prop->SetPropertyValue_InContainer(Node, Code);
		FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
		Node->PostEditChangeProperty(ChangeEvent);
		return true;
	}

	bool GetCustomHlslReflected(const UEdGraphNode* Node, FString& OutCode, FString& OutError)
	{
		if (!IsCustomHlslNode(Node))
		{
			OutError = TEXT("Node is not a UNiagaraNodeCustomHlsl.");
			return false;
		}
		FStrProperty* Prop = FindFProperty<FStrProperty>(Node->GetClass(), CustomHlslPropertyName);
		if (!Prop)
		{
			OutError = TEXT("CustomHlsl UPROPERTY was not found by reflection.");
			return false;
		}
		OutCode = Prop->GetPropertyValue_InContainer(Node);
		return true;
	}

	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Pin ? Pin->PinName.ToString() : FString());
		Obj->SetStringField(TEXT("direction"), Pin && Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input"));
		Obj->SetBoolField(TEXT("is_add_pin"), IsAddPinLocal(Pin));
		if (Pin)
		{
			Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			Obj->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
			Obj->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
		}
		return Obj;
	}

	TSharedRef<FJsonObject> CustomNodeToJson(const UEdGraphNode* Node, bool bIncludeCode)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("node_id"), Node ? Node->NodeGuid.ToString() : FString());
		Obj->SetStringField(TEXT("node_name"), Node ? Node->GetName() : FString());
		Obj->SetStringField(TEXT("class"), Node ? Node->GetClass()->GetName() : FString());
		Obj->SetNumberField(TEXT("pos_x"), Node ? Node->NodePosX : 0);
		Obj->SetNumberField(TEXT("pos_y"), Node ? Node->NodePosY : 0);

		FString Code;
		FString Error;
		if (Node && GetCustomHlslReflected(Node, Code, Error))
		{
			Obj->SetNumberField(TEXT("code_length"), Code.Len());
			if (bIncludeCode)
			{
				Obj->SetStringField(TEXT("hlsl"), Code);
			}
		}
		else if (!Error.IsEmpty())
		{
			Obj->SetStringField(TEXT("read_error"), Error);
		}

		TArray<TSharedPtr<FJsonValue>> Pins;
		if (Node)
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
			}
		}
		Obj->SetArrayField(TEXT("pins"), Pins);

		if (const UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(Node))
		{
			Obj->SetStringField(TEXT("signature_name"), FunctionNode->Signature.Name.ToString());
			TArray<TSharedPtr<FJsonValue>> Inputs;
			for (const FNiagaraVariable& Input : FunctionNode->Signature.Inputs)
			{
				TSharedRef<FJsonObject> Var = MakeShared<FJsonObject>();
				Var->SetStringField(TEXT("name"), Input.GetName().ToString());
				Var->SetStringField(TEXT("type"), TypeToString(Input.GetType()));
				Inputs.Add(MakeShared<FJsonValueObject>(Var));
			}
			Obj->SetArrayField(TEXT("signature_inputs"), Inputs);

			TArray<TSharedPtr<FJsonValue>> Outputs;
			for (const FNiagaraVariableBase& Output : FunctionNode->Signature.Outputs)
			{
				TSharedRef<FJsonObject> Var = MakeShared<FJsonObject>();
				Var->SetStringField(TEXT("name"), Output.GetName().ToString());
				Var->SetStringField(TEXT("type"), TypeToString(Output.GetType()));
				Outputs.Add(MakeShared<FJsonValueObject>(Var));
			}
			Obj->SetArrayField(TEXT("signature_outputs"), Outputs);
		}
		return Obj;
	}

	void AddDiagnostic(TArray<TSharedPtr<FJsonValue>>& Diagnostics, const FString& Severity, const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Diag = MakeShared<FJsonObject>();
		Diag->SetStringField(TEXT("severity"), Severity);
		Diag->SetStringField(TEXT("code"), Code);
		Diag->SetStringField(TEXT("message"), Message);
		Diagnostics.Add(MakeShared<FJsonValueObject>(Diag));
	}

	TSharedRef<FJsonObject> ValidateHlslText(const FString& Code)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;

		if (Code.TrimStartAndEnd().IsEmpty())
		{
			AddDiagnostic(Diagnostics, TEXT("error"), TEXT("empty_code"), TEXT("Custom HLSL code is empty."));
			++ErrorCount;
		}

		int32 BraceDepth = 0;
		int32 ParenDepth = 0;
		int32 BracketDepth = 0;
		bool bInLineComment = false;
		bool bInBlockComment = false;
		bool bInString = false;
		for (int32 Index = 0; Index < Code.Len(); ++Index)
		{
			const TCHAR Ch = Code[Index];
			const TCHAR Next = Index + 1 < Code.Len() ? Code[Index + 1] : 0;
			if (bInLineComment)
			{
				if (Ch == TEXT('\n')) { bInLineComment = false; }
				continue;
			}
			if (bInBlockComment)
			{
				if (Ch == TEXT('*') && Next == TEXT('/')) { bInBlockComment = false; ++Index; }
				continue;
			}
			if (bInString)
			{
				if (Ch == TEXT('\\')) { ++Index; continue; }
				if (Ch == TEXT('"')) { bInString = false; }
				continue;
			}
			if (Ch == TEXT('/') && Next == TEXT('/')) { bInLineComment = true; ++Index; continue; }
			if (Ch == TEXT('/') && Next == TEXT('*')) { bInBlockComment = true; ++Index; continue; }
			if (Ch == TEXT('"')) { bInString = true; continue; }
			if (Ch == TEXT('{')) { ++BraceDepth; }
			else if (Ch == TEXT('}')) { --BraceDepth; }
			else if (Ch == TEXT('(')) { ++ParenDepth; }
			else if (Ch == TEXT(')')) { --ParenDepth; }
			else if (Ch == TEXT('[')) { ++BracketDepth; }
			else if (Ch == TEXT(']')) { --BracketDepth; }
			if (BraceDepth < 0 || ParenDepth < 0 || BracketDepth < 0)
			{
				AddDiagnostic(Diagnostics, TEXT("error"), TEXT("unbalanced_closer"), TEXT("HLSL contains a closing delimiter without a matching opener."));
				++ErrorCount;
				break;
			}
		}

		if (bInBlockComment)
		{
			AddDiagnostic(Diagnostics, TEXT("error"), TEXT("unterminated_block_comment"), TEXT("HLSL contains an unterminated block comment."));
			++ErrorCount;
		}
		if (bInString)
		{
			AddDiagnostic(Diagnostics, TEXT("warning"), TEXT("unterminated_string"), TEXT("HLSL appears to contain an unterminated string literal."));
			++WarningCount;
		}
		if (BraceDepth != 0 || ParenDepth != 0 || BracketDepth != 0)
		{
			AddDiagnostic(Diagnostics, TEXT("error"), TEXT("unbalanced_delimiters"), TEXT("HLSL braces, parentheses, or brackets are not balanced."));
			++ErrorCount;
		}
		if (Code.Contains(TEXT("#include")))
		{
			AddDiagnostic(Diagnostics, TEXT("warning"), TEXT("include_requires_mapping"), TEXT("#include requires a valid Niagara virtual or absolute include mapping."));
			++WarningCount;
		}

		Result->SetBoolField(TEXT("syntax_ok"), ErrorCount == 0);
		Result->SetNumberField(TEXT("error_count"), ErrorCount);
		Result->SetNumberField(TEXT("warning_count"), WarningCount);
		Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
		Result->SetStringField(TEXT("validation_mode"), TEXT("local_syntax_reflection_bridge"));
		return Result;
	}

	bool MaybeSave(FSololmcpEditorServices& Services, const TSharedRef<FJsonObject>& Arguments, const FString& ScriptPath, TSharedRef<FJsonObject>& OutStructured, FString& OutError)
	{
		const bool bSave = Arguments->HasTypedField<EJson::Boolean>(TEXT("save")) && Arguments->GetBoolField(TEXT("save"));
		OutStructured->SetBoolField(TEXT("save_requested"), bSave);
		if (!bSave)
		{
			return true;
		}
		const bool bSaved = Services.SaveAsset(ScriptPath, true, OutError);
		OutStructured->SetBoolField(TEXT("saved"), bSaved);
		return bSaved;
	}

	TSharedRef<FJsonObject> MakeEvidenceReceipt(const FString& Phase, const FString& Status, const FString& Reason)
	{
		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("phase"), Phase);
		Evidence->SetStringField(TEXT("status"), Status);
		Evidence->SetStringField(TEXT("reason"), Reason);
		return Evidence;
	}

	bool HlslValidationHasErrors(const TSharedRef<FJsonObject>& Validation)
	{
		bool bSyntaxOk = false;
		if (Validation->TryGetBoolField(TEXT("syntax_ok"), bSyntaxOk))
		{
			return !bSyntaxOk;
		}
		double ErrorCount = 0.0;
		return Validation->TryGetNumberField(TEXT("error_count"), ErrorCount) && ErrorCount > 0.0;
	}

	void AttachNiagaraStructuredFailure(
		TSharedRef<FJsonObject>& OutStructured,
		const FString& Code,
		const FString& Message,
		const FString& FailedGate)
	{
		TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetStringField(TEXT("schema"), TEXT("somol.niagara.structured_failure.v1"));
		Failure->SetStringField(TEXT("code"), Code);
		Failure->SetStringField(TEXT("message"), Message);
		Failure->SetStringField(TEXT("failed_gate"), FailedGate);
		Failure->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_hermes"));
		Failure->SetStringField(TEXT("retry_policy"), TEXT("fix_input_then_retry_once_after_compile_readback"));
		OutStructured->SetObjectField(TEXT("structured_failure"), Failure);
		OutStructured->SetStringField(TEXT("receipt_gate_status"), TEXT("failed_validation"));
		OutStructured->SetBoolField(TEXT("receipt_gate_complete"), false);
	}

	void AttachNiagaraSafePatchEvidence(TSharedRef<FJsonObject>& OutStructured, const FString& ScriptPath, const FString& MutationKind)
	{
		OutStructured->SetStringField(TEXT("script_path"), ScriptPath);
		OutStructured->SetStringField(TEXT("asset_path"), ScriptPath);
		OutStructured->SetStringField(TEXT("mutation_kind"), MutationKind);
		OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.niagara_hlsl_mutation_receipt.v2"));
		OutStructured->SetBoolField(TEXT("receipt_gate_complete"), false);
		OutStructured->SetStringField(TEXT("receipt_gate_status"), TEXT("pending_compile_readback_preview"));
		OutStructured->SetObjectField(TEXT("compile_diagnostics_evidence"), MakeEvidenceReceipt(
			TEXT("compile_diagnostics"),
			TEXT("required_after_mutation"),
			TEXT("Run niagara_compile_diagnostics after HLSL mutation for production acceptance.")));
		OutStructured->SetObjectField(TEXT("post_edit_readback_evidence"), MakeEvidenceReceipt(
			TEXT("post_edit_readback"),
			TEXT("required_after_mutation"),
			TEXT("Read back niagara_hlsl_inspect node_id/include_code=true and compare reflected code/signature before acceptance.")));
		OutStructured->SetObjectField(TEXT("preview_playback_evidence"), MakeEvidenceReceipt(
			TEXT("preview_playback"),
			TEXT("required_after_mutation"),
			TEXT("Capture niagara_preview_capture or niagara_runtime_snapshot after compile passes.")));
		OutStructured->SetObjectField(TEXT("rollback_evidence"), MakeEvidenceReceipt(
			TEXT("rollback"),
			TEXT("transaction_available"),
			TEXT("Mutation is wrapped in an editor transaction; save is explicit via save=true.")));
	}
}

void RegisterNiagaraHLSLTools(FSololmcpToolRegistry& Registry)
{
	using SB = FSololmcpSchemaBuilder;
	const TSharedRef<FJsonObject> PinSpecSchema = SB::Object({
		{TEXT("name"), SB::String(TEXT("Pin / Niagara variable name."))},
		{TEXT("type"), SB::String(TEXT("float, bool, int, vec2, vec3, vec4, color, position, or parameter_map."))}
	}, {TEXT("name")});

	Registry.Register({
		TEXT("niagara_hlsl_add_custom_node"),
		TEXT("Create a Niagara Custom HLSL node without linking MinimalAPI setters; writes code through UPROPERTY reflection."),
		SB::Object({
			{TEXT("script_path"), SB::String(TEXT("Niagara script asset path."))},
			{TEXT("hlsl"), SB::String(TEXT("Custom HLSL code."))},
			{TEXT("node_name"), SB::String(TEXT("Optional signature/display name."))},
			{TEXT("inputs"), SB::Array(PinSpecSchema, TEXT("Optional input pin declarations."))},
			{TEXT("outputs"), SB::Array(PinSpecSchema, TEXT("Optional output pin declarations."))},
			{TEXT("pos_x"), SB::Number(TEXT("Editor graph X position."))},
			{TEXT("pos_y"), SB::Number(TEXT("Editor graph Y position."))},
			{TEXT("save"), SB::Boolean(TEXT("Save the script asset after mutation."))}
		}, {TEXT("script_path"), TEXT("hlsl")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString Hlsl;
			if (!Arguments->TryGetStringField(TEXT("hlsl"), Hlsl))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("hlsl"));
				OutError = TEXT("Missing hlsl.");
				return false;
			}

			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			FString ScriptPath;
			if (!ResolveScriptGraph(Context, Arguments, OutStructured, Script, Graph, ScriptPath, OutError))
			{
				return false;
			}

			TSharedRef<FJsonObject> LocalValidation = ValidateHlslText(Hlsl);
			OutStructured->SetObjectField(TEXT("validation"), LocalValidation);
			if (HlslValidationHasErrors(LocalValidation))
			{
				AttachNiagaraStructuredFailure(
					OutStructured,
					TEXT("HLSL_LOCAL_VALIDATION_FAILED"),
					TEXT("Custom HLSL local syntax validation failed; no graph mutation was applied."),
					TEXT("local_hlsl_validation"));
				OutError = TEXT("Custom HLSL local syntax validation failed.");
				return false;
			}

			TArray<FNiagaraVariable> Inputs;
			TArray<FNiagaraVariable> Outputs;
			if (!ReadPinSpecs(Arguments, TEXT("inputs"), Inputs, OutError) ||
				!ReadPinSpecs(Arguments, TEXT("outputs"), Outputs, OutError))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_PIN_SPEC"), TEXT("inputs_outputs"), OutError);
				return false;
			}

			FString NodeName = TEXT("Custom Hlsl");
			Arguments->TryGetStringField(TEXT("node_name"), NodeName);
			if (Outputs.IsEmpty())
			{
				Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("CustomHLSLOutput")));
			}

			double PosX = 0.0;
			double PosY = 0.0;
			Arguments->TryGetNumberField(TEXT("pos_x"), PosX);
			Arguments->TryGetNumberField(TEXT("pos_y"), PosY);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraHlslAddCustomNode", "SOMOLMCP Add Niagara Custom HLSL Node"));
			Script->Modify();
			Graph->Modify();

			UNiagaraNodeCustomHlsl* Node = NewObject<UNiagaraNodeCustomHlsl>(Graph);
			SetSignature(Node, NodeName, Inputs, Outputs);
			Graph->AddNode(Node, /*bUserAction*/ false, /*bSelectNewNode*/ false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			Node->NodePosX = static_cast<int32>(PosX);
			Node->NodePosY = static_cast<int32>(PosY);

			if (!SetCustomHlslReflected(Node, Hlsl, OutError))
			{
				SololmcpError::Set(OutStructured, TEXT("REFLECTION_WRITE_FAILED"), TEXT("hlsl"), OutError);
				return false;
			}
			RebuildSignatureFromPins(Node);
			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetBoolField(TEXT("reflection_bridge"), true);
			AttachNiagaraSafePatchEvidence(OutStructured, ScriptPath, TEXT("add_custom_hlsl_node"));
			OutStructured->SetObjectField(TEXT("node"), CustomNodeToJson(Node, false));
			OutStructured->SetObjectField(TEXT("validation"), LocalValidation);
			FString Readback;
			const bool bReadbackOk = GetCustomHlslReflected(Node, Readback, OutError) && Readback == Hlsl;
			TSharedRef<FJsonObject> ReadbackEvidence = MakeShared<FJsonObject>();
			ReadbackEvidence->SetStringField(TEXT("phase"), TEXT("post_edit_readback"));
			ReadbackEvidence->SetStringField(TEXT("status"), bReadbackOk ? TEXT("verified") : TEXT("failed_validation"));
			ReadbackEvidence->SetBoolField(TEXT("verified"), bReadbackOk);
			ReadbackEvidence->SetNumberField(TEXT("expected_length"), Hlsl.Len());
			ReadbackEvidence->SetNumberField(TEXT("actual_length"), Readback.Len());
			OutStructured->SetObjectField(TEXT("post_edit_readback"), ReadbackEvidence);
			if (!bReadbackOk)
			{
				AttachNiagaraStructuredFailure(
					OutStructured,
					TEXT("HLSL_READBACK_MISMATCH"),
					TEXT("Custom HLSL reflected readback did not match the requested code after mutation."),
					TEXT("post_edit_readback"));
				return false;
			}
			if (!MaybeSave(Context.Services, Arguments, ScriptPath, OutStructured, OutError))
			{
				return false;
			}

			OutSummary = FString::Printf(TEXT("Added Niagara Custom HLSL node '%s'."), *Node->NodeGuid.ToString());
			return true;
		}
	});

	Registry.Register({
		TEXT("niagara_hlsl_set_code"),
		TEXT("Set code on an existing Niagara Custom HLSL node through UPROPERTY reflection."),
		SB::Object({
			{TEXT("script_path"), SB::String()},
			{TEXT("node_id"), SB::String(TEXT("NodeGuid or UObject name."))},
			{TEXT("hlsl"), SB::String()},
			{TEXT("save"), SB::Boolean(TEXT("Save the script asset after mutation."))}
		}, {TEXT("script_path"), TEXT("node_id"), TEXT("hlsl")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString NodeId;
			FString Hlsl;
			if (!Arguments->TryGetStringField(TEXT("node_id"), NodeId)) { SololmcpError::MissingParam(OutStructured, TEXT("node_id")); OutError = TEXT("Missing node_id."); return false; }
			if (!Arguments->TryGetStringField(TEXT("hlsl"), Hlsl)) { SololmcpError::MissingParam(OutStructured, TEXT("hlsl")); OutError = TEXT("Missing hlsl."); return false; }

			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			FString ScriptPath;
			if (!ResolveScriptGraph(Context, Arguments, OutStructured, Script, Graph, ScriptPath, OutError))
			{
				return false;
			}

			TSharedRef<FJsonObject> LocalValidation = ValidateHlslText(Hlsl);
			OutStructured->SetObjectField(TEXT("validation"), LocalValidation);
			if (HlslValidationHasErrors(LocalValidation))
			{
				AttachNiagaraStructuredFailure(
					OutStructured,
					TEXT("HLSL_LOCAL_VALIDATION_FAILED"),
					TEXT("Custom HLSL local syntax validation failed; no graph mutation was applied."),
					TEXT("local_hlsl_validation"));
				OutError = TEXT("Custom HLSL local syntax validation failed.");
				return false;
			}

			UEdGraphNode* Node = FindNodeById(Graph, NodeId);
			if (!IsCustomHlslNode(Node))
			{
				SololmcpError::NotFound(OutStructured, TEXT("Niagara Custom HLSL node"));
				OutError = TEXT("Niagara Custom HLSL node was not found.");
				return false;
			}

			FString Before;
			GetCustomHlslReflected(Node, Before, OutError);
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraHlslSetCode", "SOMOLMCP Set Niagara Custom HLSL Code"));
			Script->Modify();
			Graph->Modify();
			if (!SetCustomHlslReflected(Node, Hlsl, OutError))
			{
				SololmcpError::Set(OutStructured, TEXT("REFLECTION_WRITE_FAILED"), TEXT("hlsl"), OutError);
				return false;
			}
			RebuildSignatureFromPins(Cast<UNiagaraNodeFunctionCall>(Node));
			Graph->NotifyGraphChanged();
			SololmcpWriteFlush::EnsureFlushed(Script);

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetBoolField(TEXT("reflection_bridge"), true);
			AttachNiagaraSafePatchEvidence(OutStructured, ScriptPath, TEXT("set_custom_hlsl_code"));
			OutStructured->SetNumberField(TEXT("before_length"), Before.Len());
			OutStructured->SetNumberField(TEXT("after_length"), Hlsl.Len());
			OutStructured->SetObjectField(TEXT("node"), CustomNodeToJson(Node, false));
			OutStructured->SetObjectField(TEXT("validation"), LocalValidation);
			FString Readback;
			const bool bReadbackOk = GetCustomHlslReflected(Node, Readback, OutError) && Readback == Hlsl;
			TSharedRef<FJsonObject> ReadbackEvidence = MakeShared<FJsonObject>();
			ReadbackEvidence->SetStringField(TEXT("phase"), TEXT("post_edit_readback"));
			ReadbackEvidence->SetStringField(TEXT("status"), bReadbackOk ? TEXT("verified") : TEXT("failed_validation"));
			ReadbackEvidence->SetBoolField(TEXT("verified"), bReadbackOk);
			ReadbackEvidence->SetNumberField(TEXT("expected_length"), Hlsl.Len());
			ReadbackEvidence->SetNumberField(TEXT("actual_length"), Readback.Len());
			OutStructured->SetObjectField(TEXT("post_edit_readback"), ReadbackEvidence);
			if (!bReadbackOk)
			{
				AttachNiagaraStructuredFailure(
					OutStructured,
					TEXT("HLSL_READBACK_MISMATCH"),
					TEXT("Custom HLSL reflected readback did not match the requested code after mutation."),
					TEXT("post_edit_readback"));
				return false;
			}
			if (!MaybeSave(Context.Services, Arguments, ScriptPath, OutStructured, OutError))
			{
				return false;
			}

			OutSummary = FString::Printf(TEXT("Updated Niagara Custom HLSL node '%s'."), *NodeId);
			return true;
		}
	});

	Registry.Register({
		TEXT("niagara_hlsl_inspect"),
		TEXT("Inspect Niagara Custom HLSL nodes and their reflected code/signature/pins."),
		SB::Object({
			{TEXT("script_path"), SB::String()},
			{TEXT("node_id"), SB::String(TEXT("Optional NodeGuid or UObject name."))},
			{TEXT("include_code"), SB::Boolean(TEXT("Include full HLSL code; default true."))}
		}, {TEXT("script_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UNiagaraScript* Script = nullptr;
			UNiagaraGraph* Graph = nullptr;
			FString ScriptPath;
			if (!ResolveScriptGraph(Context, Arguments, OutStructured, Script, Graph, ScriptPath, OutError))
			{
				return false;
			}
			const bool bIncludeCode = !Arguments->HasTypedField<EJson::Boolean>(TEXT("include_code")) || Arguments->GetBoolField(TEXT("include_code"));

			TArray<TSharedPtr<FJsonValue>> Nodes;
			FString NodeId;
			if (Arguments->TryGetStringField(TEXT("node_id"), NodeId) && !NodeId.IsEmpty())
			{
				UEdGraphNode* Node = FindNodeById(Graph, NodeId);
				if (!IsCustomHlslNode(Node))
				{
					SololmcpError::NotFound(OutStructured, TEXT("Niagara Custom HLSL node"));
					OutError = TEXT("Niagara Custom HLSL node was not found.");
					return false;
				}
				Nodes.Add(MakeShared<FJsonValueObject>(CustomNodeToJson(Node, bIncludeCode)));
			}
			else
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (IsCustomHlslNode(Node))
					{
						Nodes.Add(MakeShared<FJsonValueObject>(CustomNodeToJson(Node, bIncludeCode)));
					}
				}
			}

			OutStructured->SetStringField(TEXT("script_path"), ScriptPath);
			OutStructured->SetStringField(TEXT("asset_path"), ScriptPath);
			OutStructured->SetNumberField(TEXT("custom_hlsl_node_count"), Nodes.Num());
			OutStructured->SetBoolField(TEXT("reflection_bridge"), true);
			OutStructured->SetArrayField(TEXT("nodes"), Nodes);
			OutSummary = FString::Printf(TEXT("Inspected %d Niagara Custom HLSL node(s)."), Nodes.Num());
			return true;
		}
		, nullptr
		, 5
	});

	Registry.Register({
		TEXT("niagara_hlsl_validate"),
		TEXT("Validate Custom HLSL text or the code stored on a Niagara Custom HLSL node. Local syntax validation; use niagara_compile_diagnostics for full VM compile messages."),
		SB::Object({
			{TEXT("hlsl"), SB::String(TEXT("Optional direct HLSL text."))},
			{TEXT("script_path"), SB::String(TEXT("Required when validating an existing node."))},
			{TEXT("node_id"), SB::String(TEXT("Required when validating an existing node."))}
		}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString Hlsl;
			if (!Arguments->TryGetStringField(TEXT("hlsl"), Hlsl))
			{
				FString NodeId;
				if (!Arguments->TryGetStringField(TEXT("node_id"), NodeId))
				{
					SololmcpError::MissingParam(OutStructured, TEXT("hlsl"));
					OutError = TEXT("Provide hlsl or script_path + node_id.");
					return false;
				}
				UNiagaraScript* Script = nullptr;
				UNiagaraGraph* Graph = nullptr;
				FString ScriptPath;
				if (!ResolveScriptGraph(Context, Arguments, OutStructured, Script, Graph, ScriptPath, OutError))
				{
					return false;
				}
				UEdGraphNode* Node = FindNodeById(Graph, NodeId);
				if (!GetCustomHlslReflected(Node, Hlsl, OutError))
				{
					SololmcpError::Set(OutStructured, TEXT("REFLECTION_READ_FAILED"), TEXT("node_id"), OutError);
					return false;
				}
				OutStructured->SetStringField(TEXT("node_id"), NodeId);
				OutStructured->SetStringField(TEXT("script_path"), ScriptPath);
				OutStructured->SetStringField(TEXT("asset_path"), ScriptPath);
			}

			TSharedRef<FJsonObject> Validation = ValidateHlslText(Hlsl);
			OutStructured->SetBoolField(TEXT("ok"), Validation->GetBoolField(TEXT("syntax_ok")));
			OutStructured->SetNumberField(TEXT("code_length"), Hlsl.Len());
			OutStructured->SetObjectField(TEXT("validation"), Validation);
			OutSummary = Validation->GetBoolField(TEXT("syntax_ok"))
				? TEXT("Niagara Custom HLSL local syntax validation passed.")
				: TEXT("Niagara Custom HLSL local syntax validation found errors.");
			return Validation->GetBoolField(TEXT("syntax_ok"));
		}
		, nullptr
		, 5
	});
}
} // namespace UE::SOMOLMCP
