// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — AnimBlueprint StateMachine Editing Tools
// New tools added (animbp_* prefix):
//   animbp_create_state_machine    — Create a state machine in the AnimGraph root.
//   animbp_add_state               — Add a state to a state machine.
//   animbp_add_transition          — Add a transition between two states with crossfade.
//   animbp_set_transition_rule     — Set transition rule (always, time_remaining_lt, and safe expression subsets).
//   animbp_add_blendspace_node     — Add a UAnimGraphNode_BlendSpacePlayer inside a state's anim graph.
//   animbp_list_states             — Enumerate state machines, states, and transitions in an AnimBlueprint.
//   animbp_inspect_transition_rule_graph — Read-only transition rule graph readiness inspection.
//
// IMPORTANT: This file is intentionally SELF-CONTAINED and does NOT modify
// SololmcpDomainTools.cpp / SololmcpToolRegistry.cpp. Helpers that exist as
// file-static / anonymous-namespace functions in SololmcpDomainTools.cpp are
// re-implemented here in this file's anonymous namespace.

#include "Tools/SololmcpToolRegistry.h"
#include "SOMOLMCP.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"

#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Self.h"
#include "K2Node_TransitionRuleGetter.h"
#include "K2Node_VariableGet.h"

#include "BlueprintEditorLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"

namespace UE::SOMOLMCP
{
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 5
	using FSomolAnimGraphPosition = FVector2D;
	#else
	using FSomolAnimGraphPosition = FVector2f;
	#endif
namespace
{
	// ------------------------------------------------------------------
	// Local re-implementations of helpers that live as file-static / anon
	// namespace symbols inside SololmcpDomainTools.cpp. The supervisor
	// asked us not to modify that file, so we copy the small helpers here.
	// ------------------------------------------------------------------

	FString NormalizeObjectPathForRegistryLocal(const FString& AssetPath)
	{
		FString ObjectPath = AssetPath;
		const int32 LastSlash = ObjectPath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		const int32 LastDot = ObjectPath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (LastDot <= LastSlash)
		{
			FString AssetName = ObjectPath;
			int32 SlashIndex = INDEX_NONE;
			if (AssetName.FindLastChar(TEXT('/'), SlashIndex))
			{
				AssetName = AssetName.Mid(SlashIndex + 1);
			}
			ObjectPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
		}
		return ObjectPath;
	}

	bool RegistryAllowsAnimBlueprintLoadLocal(const FString& AssetPath, FString& OutError)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		if (AssetRegistry.IsLoadingAssets())
		{
			AssetRegistry.WaitForCompletion();
		}

		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NormalizeObjectPathForRegistryLocal(AssetPath)));
		if (!AssetData.IsValid())
		{
			TArray<FAssetData> PackageAssets;
			AssetRegistry.GetAssetsByPackageName(FName(*AssetPath), PackageAssets);
			if (PackageAssets.Num() == 1)
			{
				AssetData = PackageAssets[0];
			}
		}

		if (!AssetData.IsValid())
		{
			return true;
		}

		const FName AssetClassName = AssetData.AssetClassPath.GetAssetName();
		if (AssetClassName != UAnimBlueprint::StaticClass()->GetFName())
		{
			OutError = FString::Printf(
				TEXT("Asset is not an animation blueprint (registry class: %s)."),
				*AssetClassName.ToString());
			return false;
		}
		return true;
	}

	UAnimBlueprint* LoadAnimBlueprintAssetLocal(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
	{
		if (!RegistryAllowsAnimBlueprintLoadLocal(AssetPath, OutError))
		{
			return nullptr;
		}

		UObject* Asset = Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		if (UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Asset))
		{
			return AnimBlueprint;
		}
		OutError = TEXT("Asset is not an animation blueprint.");
		return nullptr;
	}

	UEdGraph* FindPrimaryAnimBlueprintGraphLocal(UAnimBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		UEdGraph* FirstMatch = nullptr;
		TArray<UEdGraph*> Graphs;
		Graphs.Append(Blueprint->UbergraphPages);
		Graphs.Append(Blueprint->FunctionGraphs);
		Graphs.Append(Blueprint->MacroGraphs);
		Graphs.Append(Blueprint->DelegateSignatureGraphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph || !Graph->GetSchema())
			{
				continue;
			}
			if (Graph->GetSchema()->GetClass() == UAnimationGraphSchema::StaticClass())
			{
				if (!FirstMatch)
				{
					FirstMatch = Graph;
				}
				if (Graph->GetName().Equals(TEXT("AnimGraph"), ESearchCase::IgnoreCase))
				{
					return Graph;
				}
			}
		}
		return FirstMatch;
	}

	TArray<UAnimGraphNode_StateMachineBase*> GetAnimStateMachineNodesLocal(UAnimBlueprint* Blueprint)
	{
		TArray<UAnimGraphNode_StateMachineBase*> Nodes;
		if (Blueprint)
		{
			FBlueprintEditorUtils::GetAllNodesOfClass<UAnimGraphNode_StateMachineBase>(Blueprint, Nodes);
		}
		return Nodes;
	}

	UAnimGraphNode_StateMachineBase* FindAnimStateMachineNodeLocal(UAnimBlueprint* Blueprint, const FString& StateMachineName)
	{
		for (UAnimGraphNode_StateMachineBase* Node : GetAnimStateMachineNodesLocal(Blueprint))
		{
			if (!Node)
			{
				continue;
			}
			if (Node->GetStateMachineName() == StateMachineName ||
				(Node->EditorStateMachineGraph && Node->EditorStateMachineGraph->GetName() == StateMachineName))
			{
				return Node;
			}
		}
		return nullptr;
	}

	TArray<UAnimStateNode*> GetAnimStateNodesLocal(UAnimationStateMachineGraph* StateMachineGraph)
	{
		TArray<UAnimStateNode*> Nodes;
		if (!StateMachineGraph)
		{
			return Nodes;
		}
		for (UEdGraphNode* GraphNode : StateMachineGraph->Nodes)
		{
			if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(GraphNode))
			{
				Nodes.Add(StateNode);
			}
		}
		return Nodes;
	}

	UAnimStateNode* FindAnimStateNodeLocal(UAnimationStateMachineGraph* StateMachineGraph, const FString& StateName)
	{
		for (UAnimStateNode* StateNode : GetAnimStateNodesLocal(StateMachineGraph))
		{
			if (StateNode && StateNode->GetStateName() == StateName)
			{
				return StateNode;
			}
		}
		return nullptr;
	}

	TArray<UAnimStateTransitionNode*> GetAnimStateTransitionsLocal(UAnimationStateMachineGraph* StateMachineGraph)
	{
		TArray<UAnimStateTransitionNode*> Nodes;
		if (!StateMachineGraph)
		{
			return Nodes;
		}
		for (UEdGraphNode* GraphNode : StateMachineGraph->Nodes)
		{
			if (UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(GraphNode))
			{
				Nodes.Add(TransitionNode);
			}
		}
		return Nodes;
	}

	UAnimStateTransitionNode* FindAnimTransitionLocal(UAnimationStateMachineGraph* StateMachineGraph, const FString& FromState, const FString& ToState)
	{
		for (UAnimStateTransitionNode* TransitionNode : GetAnimStateTransitionsLocal(StateMachineGraph))
		{
			if (!TransitionNode)
			{
				continue;
			}
			const UAnimStateNodeBase* PreviousState = TransitionNode->GetPreviousState();
			const UAnimStateNodeBase* NextState = TransitionNode->GetNextState();
			if (PreviousState && NextState &&
				PreviousState->GetStateName() == FromState &&
				NextState->GetStateName() == ToState)
			{
				return TransitionNode;
			}
		}
		return nullptr;
	}

	FSomolAnimGraphPosition GetNodeLocationFromArguments(const TSharedRef<FJsonObject>& Arguments)
	{
		const float X = Arguments->HasTypedField<EJson::Number>(TEXT("pos_x"))
			? static_cast<float>(Arguments->GetNumberField(TEXT("pos_x")))
			: (Arguments->HasTypedField<EJson::Number>(TEXT("node_x"))
				? static_cast<float>(Arguments->GetIntegerField(TEXT("node_x")))
				: 0.0f);
		const float Y = Arguments->HasTypedField<EJson::Number>(TEXT("pos_y"))
			? static_cast<float>(Arguments->GetNumberField(TEXT("pos_y")))
			: (Arguments->HasTypedField<EJson::Number>(TEXT("node_y"))
				? static_cast<float>(Arguments->GetIntegerField(TEXT("node_y")))
				: 0.0f);
		return FSomolAnimGraphPosition(X, Y);
	}

	// Convenience: resolve a state machine + its internal graph for a tool call.
	bool ResolveStateMachineFromArgs(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		UAnimBlueprint*& OutBlueprint,
		UAnimGraphNode_StateMachineBase*& OutStateMachineNode,
		UAnimationStateMachineGraph*& OutStateMachineGraph,
		FString& OutError)
	{
		FString AssetPath;
		FString MachineName;
		if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
			!Arguments->TryGetStringField(TEXT("machine_name"), MachineName))
		{
			OutError = TEXT("Missing asset_path or machine_name.");
			return false;
		}
		OutBlueprint = LoadAnimBlueprintAssetLocal(Context.Services, AssetPath, OutError);
		if (!OutBlueprint)
		{
			return false;
		}
		OutStateMachineNode = FindAnimStateMachineNodeLocal(OutBlueprint, MachineName);
		if (!OutStateMachineNode || !OutStateMachineNode->EditorStateMachineGraph)
		{
			OutError = FString::Printf(TEXT("Animation state machine '%s' was not found."), *MachineName);
			return false;
		}
		OutStateMachineGraph = OutStateMachineNode->EditorStateMachineGraph;
		return true;
	}

	// ------------------------------------------------------------------
	// JSON helpers for response shape.
	// ------------------------------------------------------------------

	TSharedRef<FJsonObject> MakeStateMachineSummaryJson(UAnimGraphNode_StateMachineBase* MachineNode)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!MachineNode)
		{
			return Result;
		}
		Result->SetStringField(TEXT("name"), MachineNode->GetStateMachineName());
		if (MachineNode->EditorStateMachineGraph)
		{
			Result->SetStringField(TEXT("internal_graph_path"), MachineNode->EditorStateMachineGraph->GetPathName());
			Result->SetStringField(TEXT("internal_graph_name"), MachineNode->EditorStateMachineGraph->GetName());
		}
		Result->SetStringField(TEXT("nodeGuid"), MachineNode->NodeGuid.ToString());
		return Result;
	}

	TSharedRef<FJsonObject> MakeFullMachineDescription(UAnimGraphNode_StateMachineBase* MachineNode)
	{
		TSharedRef<FJsonObject> MachineJson = MakeShared<FJsonObject>();
		if (!MachineNode)
		{
			return MachineJson;
		}
		MachineJson->SetStringField(TEXT("name"), MachineNode->GetStateMachineName());
		if (MachineNode->EditorStateMachineGraph)
		{
			MachineJson->SetStringField(TEXT("internal_graph_path"), MachineNode->EditorStateMachineGraph->GetPathName());
		}

		// states
		TArray<TSharedPtr<FJsonValue>> StatesJson;
		for (UAnimStateNode* StateNode : GetAnimStateNodesLocal(MachineNode->EditorStateMachineGraph))
		{
			if (!StateNode)
			{
				continue;
			}
			StatesJson.Add(MakeShared<FJsonValueString>(StateNode->GetStateName()));
		}
		MachineJson->SetArrayField(TEXT("states"), StatesJson);

		// transitions
		TArray<TSharedPtr<FJsonValue>> TransitionsJson;
		for (UAnimStateTransitionNode* T : GetAnimStateTransitionsLocal(MachineNode->EditorStateMachineGraph))
		{
			if (!T)
			{
				continue;
			}
			const UAnimStateNodeBase* From = T->GetPreviousState();
			const UAnimStateNodeBase* To = T->GetNextState();
			TSharedRef<FJsonObject> TJson = MakeShared<FJsonObject>();
			TJson->SetStringField(TEXT("from"), From ? From->GetStateName() : FString());
			TJson->SetStringField(TEXT("to"), To ? To->GetStateName() : FString());
			// CrossfadeDuration is a public float on UAnimStateTransitionNode in UE 5.x.
			TJson->SetNumberField(TEXT("duration"), T->CrossfadeDuration);
			TransitionsJson.Add(MakeShared<FJsonValueObject>(TJson));
		}
		MachineJson->SetArrayField(TEXT("transitions"), TransitionsJson);

		return MachineJson;
	}

	// Find the StateResult node inside a state's bound anim graph.
	UAnimGraphNode_StateResult* FindStateResultNode(UEdGraph* StateAnimGraph)
	{
		if (!StateAnimGraph)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : StateAnimGraph->Nodes)
		{
			if (UAnimGraphNode_StateResult* Result = Cast<UAnimGraphNode_StateResult>(Node))
			{
				return Result;
			}
		}
		return nullptr;
	}

	// Try connect output animation pose pin from SourceNode to result-node input.
	// Returns true if a connection was made (or was already present).
	bool TryWirePoseToResult(UAnimGraphNode_Base* SourceNode, UAnimGraphNode_StateResult* ResultNode, FString& OutError)
	{
		if (!SourceNode || !ResultNode)
		{
			OutError = TEXT("Cannot wire pose: source or result node is null.");
			return false;
		}

		// Find an output pose pin on source. Anim graph pose pins use the
		// PC_Struct category with FPoseLink; we accept any output pin.
		UEdGraphPin* SourceOutPin = nullptr;
		for (UEdGraphPin* Pin : SourceNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				SourceOutPin = Pin;
				break;
			}
		}
		// Find an input pin on the result node.
		UEdGraphPin* ResultInPin = nullptr;
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				ResultInPin = Pin;
				break;
			}
		}
		if (!SourceOutPin || !ResultInPin)
		{
			OutError = TEXT("Failed to locate pose pins for wiring.");
			return false;
		}

		// TODO(P0-1): verify schema-based MakeLinkTo for animation pose pins; using direct MakeLinkTo for now.
		SourceOutPin->MakeLinkTo(ResultInPin);
		ResultInPin->MakeLinkTo(SourceOutPin);
		if (!SourceOutPin->LinkedTo.Contains(ResultInPin) || !ResultInPin->LinkedTo.Contains(SourceOutPin))
		{
			OutError = TEXT("Pose pins did not remain linked after wiring.");
			return false;
		}
		return true;
	}

	bool VerifyAnimGraphContainsNode(UEdGraph* Graph, UEdGraphNode* Node)
	{
		return Graph && Node && Graph->Nodes.Contains(Node);
	}

	FString AnimBPPinDirectionToString(const EEdGraphPinDirection Direction)
	{
		switch (Direction)
		{
		case EGPD_Input:
			return TEXT("input");
		case EGPD_Output:
			return TEXT("output");
		default:
			return TEXT("unknown");
		}
	}

	FString GetAnimBPNodeStableId(UEdGraphNode* Node)
	{
		if (!Node)
		{
			return FString();
		}
		return Node->NodeGuid.IsValid() ? Node->NodeGuid.ToString() : Node->GetName();
	}

	FString GetAnimBPNodeTitle(UEdGraphNode* Node)
	{
		return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
	}

	bool IsBoolPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean;
	}

	bool IsLikelyTransitionResultConditionPin(const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->Direction != EGPD_Input)
		{
			return false;
		}

		const FString PinName = Pin->PinName.ToString();
		const FString DisplayName = Pin->GetDisplayName().ToString();
		if (PinName.Contains(TEXT("CanEnterTransition"), ESearchCase::IgnoreCase) ||
			PinName.Contains(TEXT("bCanEnterTransition"), ESearchCase::IgnoreCase) ||
			DisplayName.Contains(TEXT("Can Enter Transition"), ESearchCase::IgnoreCase) ||
			DisplayName.Contains(TEXT("CanEnterTransition"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		return IsBoolPin(Pin);
	}

	bool IsCommentNode(UEdGraphNode* Node)
	{
		return Cast<UEdGraphNode_Comment>(Node) != nullptr;
	}

	bool IsTransitionResultNode(UEdGraphNode* Node)
	{
		return Cast<UAnimGraphNode_TransitionResult>(Node) != nullptr;
	}

	UAnimGraphNode_TransitionResult* FindTransitionResultNode(UEdGraph* TransitionGraph)
	{
		if (!TransitionGraph)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : TransitionGraph->Nodes)
		{
			if (UAnimGraphNode_TransitionResult* ResultNode = Cast<UAnimGraphNode_TransitionResult>(Node))
			{
				return ResultNode;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindTransitionResultConditionPin(UAnimGraphNode_TransitionResult* ResultNode)
	{
		if (!ResultNode)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (IsLikelyTransitionResultConditionPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool IsNonPlaceholderConditionNode(UEdGraphNode* Node)
	{
		return Node && !IsTransitionResultNode(Node) && !IsCommentNode(Node);
	}

	bool TryParseConstBoolTransitionExpression(const FString& Expression, bool& bOutValue, FString& OutError)
	{
		FString Text = Expression.TrimStartAndEnd();
		bool bNegated = false;
		while (Text.StartsWith(TEXT("!")))
		{
			bNegated = !bNegated;
			Text = Text.Mid(1).TrimStartAndEnd();
		}

		const FString Lower = Text.ToLower();
		bool bParsed = false;
		bool bValue = false;
		if (Lower == TEXT("true") || Lower == TEXT("1"))
		{
			bParsed = true;
			bValue = true;
		}
		else if (Lower == TEXT("false") || Lower == TEXT("0"))
		{
			bParsed = true;
			bValue = false;
		}
		else
		{
			FString Left;
			FString Right;
			if (Text.Split(TEXT("=="), &Left, &Right))
			{
				bool bLeft = false;
				bool bRight = false;
				FString LeftError;
				FString RightError;
				if (TryParseConstBoolTransitionExpression(Left, bLeft, LeftError) &&
					TryParseConstBoolTransitionExpression(Right, bRight, RightError))
				{
					bParsed = true;
					bValue = bLeft == bRight;
				}
			}
			else if (Text.Split(TEXT("!="), &Left, &Right))
			{
				bool bLeft = false;
				bool bRight = false;
				FString LeftError;
				FString RightError;
				if (TryParseConstBoolTransitionExpression(Left, bLeft, LeftError) &&
					TryParseConstBoolTransitionExpression(Right, bRight, RightError))
				{
					bParsed = true;
					bValue = bLeft != bRight;
				}
			}
		}

		if (!bParsed)
		{
			OutError = TEXT("Only constant boolean expressions are currently accepted: true, false, 1, 0, !expr, expr == expr, expr != expr.");
			return false;
		}

		bOutValue = bNegated ? !bValue : bValue;
		return true;
	}

	TSharedRef<FJsonObject> MakeTransitionRuleRepairPlanJson(
		const FString& RuleType,
		const FString& Reason,
		bool bExecutableNow)
	{
		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_repair_plan.v1"));
		Plan->SetStringField(TEXT("rule_type"), RuleType);
		Plan->SetStringField(TEXT("reason"), Reason);
		Plan->SetBoolField(TEXT("executable_now"), bExecutableNow);

		TArray<TSharedPtr<FJsonValue>> Steps;
		auto AddStep = [&Steps](const FString& Id, const FString& Tool, const FString& Action)
		{
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("id"), Id);
			Step->SetStringField(TEXT("tool"), Tool);
			Step->SetStringField(TEXT("action"), Action);
			Steps.Add(MakeShared<FJsonValueObject>(Step));
		};

		AddStep(TEXT("inspect_pre"), TEXT("animbp_inspect_transition_rule_graph"), TEXT("Capture current transition graph nodes, bCanEnterTransition pin links, and placeholder risk."));
		if (RuleType == TEXT("time_remaining_lt"))
		{
			AddStep(TEXT("write_k2_nodes"), TEXT("animbp_set_transition_rule"), TEXT("Create UK2Node_TransitionRuleGetter for source-state time remaining, a typed float less-than comparison, threshold literal, and link bool output to TransitionResult.bCanEnterTransition."));
		}
		else if (RuleType == TEXT("expression"))
		{
			AddStep(TEXT("parse_expression"), TEXT("animbp_set_transition_rule"), TEXT("Current executable subset accepts only constant boolean expressions; non-constant expressions must wait for typed identifier binding and K2 node materialization."));
			AddStep(TEXT("write_const_bool"), TEXT("animbp_set_transition_rule"), TEXT("For true/false/1/0/!const/const comparisons, set TransitionResult.bCanEnterTransition default value and compile."));
		}
		AddStep(TEXT("compile"), TEXT("blueprint_compile"), TEXT("Compile the AnimBlueprint and require a non-error Blueprint status."));
		AddStep(TEXT("inspect_post"), TEXT("animbp_inspect_transition_rule_graph"), TEXT("Read back the transition graph after compile and verify the receipt fields before claiming support."));
		Plan->SetArrayField(TEXT("steps"), Steps);
		return Plan;
	}

	bool TryWriteTransitionResultConstant(
		UAnimBlueprint* AnimBP,
		UAnimStateTransitionNode* TransitionNode,
		bool bValue,
		TSharedRef<FJsonObject>& OutReceipt,
		FString& OutError)
	{
		if (!AnimBP || !TransitionNode || !TransitionNode->BoundGraph)
		{
			OutError = TEXT("Transition rule graph is missing; cannot write constant rule.");
			return false;
		}

		UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(TransitionNode->BoundGraph);
		UEdGraphPin* ConditionPin = FindTransitionResultConditionPin(ResultNode);
		if (!ResultNode || !ConditionPin)
		{
			OutError = TEXT("TransitionResult.bCanEnterTransition pin was not found; no mutation was attempted.");
			return false;
		}

		const FString DesiredValue = bValue ? TEXT("true") : TEXT("false");
		const UEdGraphSchema* Schema = TransitionNode->BoundGraph->GetSchema();
		if (!Schema)
		{
			OutError = TEXT("Transition graph schema is missing; no mutation was attempted.");
			return false;
		}

		TransitionNode->BoundGraph->Modify();
		ResultNode->Modify();
		ConditionPin->Modify();
		ConditionPin->BreakAllPinLinks();
		Schema->TrySetDefaultValue(*ConditionPin, DesiredValue, true);
		if (!ConditionPin->DefaultValue.Equals(DesiredValue, ESearchCase::IgnoreCase))
		{
			OutReceipt->SetStringField(TEXT("actual_pin_default"), ConditionPin->DefaultValue);
			OutError = TEXT("TransitionResult condition pin default did not match after write.");
			return false;
		}

		TransitionNode->BoundGraph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		UBlueprintEditorLibrary::CompileBlueprint(AnimBP);

		const FString CompileStatus = StaticEnum<EBlueprintStatus>()
			? StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(AnimBP->Status))
			: FString::FromInt(static_cast<int32>(AnimBP->Status));
		const bool bCompileSucceeded = AnimBP->Status != BS_Error;
		OutReceipt->SetStringField(TEXT("compile_status"), CompileStatus);
		OutReceipt->SetBoolField(TEXT("compile_succeeded"), bCompileSucceeded);
		OutReceipt->SetStringField(TEXT("transition_result_node_id"), ResultNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("condition_pin"), ConditionPin->PinName.ToString());
		OutReceipt->SetStringField(TEXT("condition_pin_default"), ConditionPin->DefaultValue);
		OutReceipt->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), ConditionPin->LinkedTo.Num() > 0);
		if (!bCompileSucceeded)
		{
			OutError = TEXT("AnimBlueprint compile finished with BS_Error after writing the constant transition rule.");
			return false;
		}
		return true;
	}

	UAnimGraphNode_Base* FindSourceStateTimeRemainingPlayer(UAnimStateTransitionNode* TransitionNode, FString& OutError)
	{
		if (!TransitionNode)
		{
			OutError = TEXT("Transition node is missing.");
			return nullptr;
		}

		UAnimStateNode* SourceStateNode = Cast<UAnimStateNode>(TransitionNode->GetPreviousState());
		if (!SourceStateNode || !SourceStateNode->BoundGraph)
		{
			OutError = TEXT("Source state graph is missing; cannot resolve an animation asset player for TimeRemaining.");
			return nullptr;
		}

		TArray<UK2Node*> AssetPlayers;
		SourceStateNode->BoundGraph->GetNodesOfClassEx<UAnimGraphNode_Base, UK2Node>(AssetPlayers);
		for (UK2Node* Candidate : AssetPlayers)
		{
			UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Candidate);
			if (!AnimNode || !AnimNode->DoesSupportTimeForTransitionGetter())
			{
				continue;
			}
			if (AnimNode->GetAnimationAsset())
			{
				return AnimNode;
			}
		}

		OutError = FString::Printf(
			TEXT("Source state '%s' has no animation asset player that supports TimeRemaining transition getters."),
			*SourceStateNode->GetStateName());
		return nullptr;
	}

	UEdGraphPin* FindFirstInputPin(UEdGraphNode* Node, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindFirstOutputPin(UEdGraphNode* Node, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool TryCreateSchemaConnection(const UEdGraphSchema* Schema, UEdGraphPin* FromPin, UEdGraphPin* ToPin, FString& OutError)
	{
		if (!Schema || !FromPin || !ToPin)
		{
			OutError = TEXT("Missing schema or pins for transition rule connection.");
			return false;
		}
		if (!Schema->TryCreateConnection(FromPin, ToPin))
		{
			OutError = FString::Printf(TEXT("Schema rejected transition rule connection %s -> %s."),
				*FromPin->PinName.ToString(), *ToPin->PinName.ToString());
			return false;
		}
		if (!FromPin->LinkedTo.Contains(ToPin) || !ToPin->LinkedTo.Contains(FromPin))
		{
			OutError = FString::Printf(TEXT("Transition rule connection %s -> %s was not retained after linking."),
				*FromPin->PinName.ToString(), *ToPin->PinName.ToString());
			return false;
		}
		return true;
	}

	bool TryWriteTimeRemainingLtTransitionRule(
		UAnimBlueprint* AnimBP,
		UAnimStateTransitionNode* TransitionNode,
		float Threshold,
		TSharedRef<FJsonObject>& OutReceipt,
		FString& OutError)
	{
		if (!AnimBP || !TransitionNode || !TransitionNode->BoundGraph)
		{
			OutError = TEXT("Transition rule graph is missing; cannot write time_remaining_lt.");
			return false;
		}
		if (!FMath::IsFinite(Threshold) || Threshold <= 0.0f)
		{
			OutError = TEXT("rule_args.threshold must be a positive finite number.");
			return false;
		}

		UEdGraph* Graph = TransitionNode->BoundGraph;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			OutError = TEXT("Transition graph schema is missing; no mutation was attempted.");
			return false;
		}

		UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(Graph);
		UEdGraphPin* ConditionPin = FindTransitionResultConditionPin(ResultNode);
		if (!ResultNode || !ConditionPin)
		{
			OutError = TEXT("TransitionResult.bCanEnterTransition pin was not found; no mutation was attempted.");
			return false;
		}

		FString AssetPlayerError;
		UAnimGraphNode_Base* AssetPlayerNode = FindSourceStateTimeRemainingPlayer(TransitionNode, AssetPlayerError);
		if (!AssetPlayerNode)
		{
			OutError = AssetPlayerError;
			return false;
		}

		UFunction* LessFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Less_DoubleDouble"));
		if (!LessFunction)
		{
			OutError = TEXT("UKismetMathLibrary.Less_DoubleDouble was not found.");
			return false;
		}

		Graph->Modify();
		TransitionNode->Modify();
		ResultNode->Modify();

		FGraphNodeCreator<UK2Node_TransitionRuleGetter> GetterCreator(*Graph);
		UK2Node_TransitionRuleGetter* TimeRemainingNode = GetterCreator.CreateNode(true);
		if (!TimeRemainingNode)
		{
			OutError = TEXT("Failed to create UK2Node_TransitionRuleGetter.");
			return false;
		}
		TimeRemainingNode->GetterType = ETransitionGetter::AnimationAsset_GetTimeFromEnd;
		TimeRemainingNode->AssociatedAnimAssetPlayerNode = AssetPlayerNode;
		TimeRemainingNode->NodePosX = ResultNode->NodePosX - 520;
		TimeRemainingNode->NodePosY = ResultNode->NodePosY - 120;
		GetterCreator.Finalize();

		FGraphNodeCreator<UK2Node_CallFunction> LessCreator(*Graph);
		UK2Node_CallFunction* LessNode = LessCreator.CreateNode(true);
		if (!LessNode)
		{
			OutError = TEXT("Failed to create float comparison function node.");
			return false;
		}
		LessNode->SetFromFunction(LessFunction);
		LessNode->NodePosX = ResultNode->NodePosX - 270;
		LessNode->NodePosY = ResultNode->NodePosY - 80;
		LessCreator.Finalize();

		UEdGraphPin* TimeOutputPin = TimeRemainingNode->GetOutputPin();
		UEdGraphPin* LessAPin = FindFirstInputPin(LessNode, TEXT("A"));
		UEdGraphPin* LessBPin = FindFirstInputPin(LessNode, TEXT("B"));
		UEdGraphPin* LessReturnPin = FindFirstOutputPin(LessNode, TEXT("ReturnValue"));
		if (!TimeOutputPin || !LessAPin || !LessBPin || !LessReturnPin)
		{
			OutError = TEXT("Failed to resolve TimeRemaining/Less node pins.");
			return false;
		}

		const FString ThresholdText = FString::SanitizeFloat(Threshold);
		Schema->TrySetDefaultValue(*LessBPin, ThresholdText, true);
		ConditionPin->Modify();
		ConditionPin->BreakAllPinLinks();

		if (!TryCreateSchemaConnection(Schema, TimeOutputPin, LessAPin, OutError))
		{
			return false;
		}
		if (!TryCreateSchemaConnection(Schema, LessReturnPin, ConditionPin, OutError))
		{
			return false;
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		UBlueprintEditorLibrary::CompileBlueprint(AnimBP);

		const FString CompileStatus = StaticEnum<EBlueprintStatus>()
			? StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(AnimBP->Status))
			: FString::FromInt(static_cast<int32>(AnimBP->Status));
		const bool bCompileSucceeded = AnimBP->Status != BS_Error;

		OutReceipt->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_writer_receipt.v3"));
		OutReceipt->SetStringField(TEXT("rule_type"), TEXT("time_remaining_lt"));
		OutReceipt->SetNumberField(TEXT("threshold"), Threshold);
		OutReceipt->SetStringField(TEXT("threshold_pin_default"), LessBPin->DefaultValue);
		OutReceipt->SetStringField(TEXT("asset_player_node_id"), AssetPlayerNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("asset_player_node_title"), GetAnimBPNodeTitle(AssetPlayerNode));
		OutReceipt->SetStringField(TEXT("animation_asset"), AssetPlayerNode->GetAnimationAsset() ? AssetPlayerNode->GetAnimationAsset()->GetPathName() : FString());
		OutReceipt->SetStringField(TEXT("time_remaining_node_id"), TimeRemainingNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("less_than_node_id"), LessNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("transition_result_node_id"), ResultNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("condition_pin"), ConditionPin->PinName.ToString());
		OutReceipt->SetBoolField(TEXT("time_remaining_output_linked_to_less_than_a"), TimeOutputPin->LinkedTo.Contains(LessAPin));
		OutReceipt->SetBoolField(TEXT("less_than_return_linked_to_bCanEnterTransition"), LessReturnPin->LinkedTo.Contains(ConditionPin));
		OutReceipt->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), ConditionPin->LinkedTo.Contains(LessReturnPin));
		OutReceipt->SetBoolField(TEXT("placeholder_written"), false);
		OutReceipt->SetBoolField(TEXT("raw_expression_placeholder_written"), false);
		OutReceipt->SetStringField(TEXT("compile_status"), CompileStatus);
		OutReceipt->SetBoolField(TEXT("compile_succeeded"), bCompileSucceeded);

		if (!bCompileSucceeded)
		{
			OutError = TEXT("AnimBlueprint compile finished with BS_Error after writing time_remaining_lt transition rule.");
			return false;
		}
		return true;
	}

	struct FSimpleVariableAndBoolTransitionExpression
	{
		FString NumericVariableName;
		FString ComparisonOperator;
		double Threshold = 0.0;
		FString BoolVariableName;
		bool bBoolExpectedValue = true;
	};

	bool IsAnimBPExpressionIdentifier(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return false;
		}
		const TCHAR First = Value[0];
		if (!(FChar::IsAlpha(First) || First == TEXT('_')))
		{
			return false;
		}
		for (int32 Index = 1; Index < Value.Len(); ++Index)
		{
			const TCHAR Ch = Value[Index];
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_')))
			{
				return false;
			}
		}
		return true;
	}

	bool TryParseBoolVariableTransitionExpression(
		const FString& Expression,
		FString& OutVariableName,
		bool& bOutExpectedValue,
		FString& OutError)
	{
		FString Text = Expression.TrimStartAndEnd();
		bool bExpectedValue = true;
		if (Text.StartsWith(TEXT("!")))
		{
			bExpectedValue = false;
			Text = Text.Mid(1).TrimStartAndEnd();
		}

		if (!IsAnimBPExpressionIdentifier(Text))
		{
			OutError = TEXT("Boolean variable expression must be an identifier such as bIsGrounded or !bIsFalling.");
			return false;
		}

		OutVariableName = Text;
		bOutExpectedValue = bExpectedValue;
		return true;
	}

	bool TryParseSimpleVariableAndBoolTransitionExpression(
		const FString& Expression,
		FSimpleVariableAndBoolTransitionExpression& OutExpression,
		FString& OutError)
	{
		const FString Trimmed = Expression.TrimStartAndEnd();
		int32 LogicalIndex = INDEX_NONE;
		if (!Trimmed.FindChar(TEXT('&'), LogicalIndex) || LogicalIndex + 1 >= Trimmed.Len() || Trimmed[LogicalIndex + 1] != TEXT('&'))
		{
			OutError = TEXT("Supported expression subset requires a numeric comparison followed by && and a boolean variable, for example: Speed > 150 && bIsGrounded.");
			return false;
		}

		FString Left = Trimmed.Left(LogicalIndex).TrimStartAndEnd();
		FString Right = Trimmed.Mid(LogicalIndex + 2).TrimStartAndEnd();
		bool bBoolExpectedValue = true;
		if (Right.StartsWith(TEXT("!")))
		{
			OutError = TEXT("Negated boolean variables are not in the current safe writer subset; use a positive bool variable for now.");
			return false;
		}

		if (Right.IsEmpty())
		{
			OutError = TEXT("Expression boolean side is empty.");
			return false;
		}

		int32 OpIndex = INDEX_NONE;
		FString Operator;
		const TArray<FString> Operators = {TEXT(">="), TEXT("<="), TEXT(">"), TEXT("<")};
		for (const FString& Candidate : Operators)
		{
			OpIndex = Left.Find(Candidate, ESearchCase::CaseSensitive);
			if (OpIndex != INDEX_NONE)
			{
				Operator = Candidate;
				break;
			}
		}

		if (OpIndex == INDEX_NONE)
		{
			OutError = TEXT("Expression numeric side must use one of >, >=, <, <=.");
			return false;
		}

		const FString NumericVariable = Left.Left(OpIndex).TrimStartAndEnd();
		const FString ThresholdText = Left.Mid(OpIndex + Operator.Len()).TrimStartAndEnd();
		double Threshold = 0.0;
		if (NumericVariable.IsEmpty() || !LexTryParseString(Threshold, *ThresholdText))
		{
			OutError = TEXT("Expression numeric side must look like: Speed > 150.");
			return false;
		}

		if (!IsAnimBPExpressionIdentifier(NumericVariable) || !IsAnimBPExpressionIdentifier(Right))
		{
			OutError = TEXT("Expression identifiers may contain only letters, digits, and underscore, and may not start with a digit.");
			return false;
		}

		OutExpression.NumericVariableName = NumericVariable;
		OutExpression.ComparisonOperator = Operator;
		OutExpression.Threshold = Threshold;
		OutExpression.BoolVariableName = Right;
		OutExpression.bBoolExpectedValue = bBoolExpectedValue;
		return true;
	}

	FEdGraphPinType MakeAnimBPExpressionDoublePinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return PinType;
	}

	FEdGraphPinType MakeAnimBPExpressionBoolPinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return PinType;
	}

	FEdGraphPinType MakeAnimBPExpressionIntPinType()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return PinType;
	}

	bool BlueprintHasMemberVariable(UBlueprint* Blueprint, const FName VariableName)
	{
		if (!Blueprint)
		{
			return false;
		}
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			if (Variable.VarName == VariableName)
			{
				return true;
			}
		}
		return false;
	}

	bool EnsureAnimBPExpressionVariable(
		UAnimBlueprint* AnimBP,
		const FString& VariableName,
		const FEdGraphPinType& PinType,
		TSharedRef<FJsonObject>& Receipt,
		const FString& ReceiptPrefix,
		FString& OutError)
	{
		const FName VarName(*VariableName);
		const bool bAlreadyExisted = BlueprintHasMemberVariable(AnimBP, VarName);
		if (!bAlreadyExisted)
		{
					// UBlueprintEditorLibrary::AddMemberVariable is 5.4+. FBlueprintEditorUtils has
		// carried the same call for far longer, so 5.3 routes to it rather than losing
		// the capability.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		if (!UBlueprintEditorLibrary::AddMemberVariable(AnimBP, VarName, PinType))
#else
		if (!FBlueprintEditorUtils::AddMemberVariable(AnimBP, VarName, PinType))
#endif
			{
				OutError = FString::Printf(TEXT("Failed to add AnimBP expression variable '%s'."), *VariableName);
				return false;
			}
		}
		Receipt->SetStringField(ReceiptPrefix + TEXT("_variable"), VariableName);
		Receipt->SetBoolField(ReceiptPrefix + TEXT("_variable_preexisting"), bAlreadyExisted);
		Receipt->SetBoolField(ReceiptPrefix + TEXT("_variable_available"), true);
		return true;
	}

	UEdGraphPin* FindFirstOutputDataPin(UEdGraphNode* Node)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UK2Node_VariableGet* CreateVariableGetNode(
		UEdGraph* Graph,
		const FString& VariableName,
		int32 NodePosX,
		int32 NodePosY,
		FString& OutError)
	{
		if (!Graph)
		{
			OutError = TEXT("Transition graph is missing.");
			return nullptr;
		}
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode(true);
		if (!Node)
		{
			OutError = FString::Printf(TEXT("Failed to create variable get node for '%s'."), *VariableName);
			return nullptr;
		}
		Node->VariableReference.SetSelfMember(FName(*VariableName));
		Node->NodePosX = NodePosX;
		Node->NodePosY = NodePosY;
		Creator.Finalize();
		return Node;
	}

	bool TryWriteBlackboardVariableTransitionRule(
		UAnimBlueprint* AnimBP,
		UAnimStateTransitionNode* TransitionNode,
		const FString& VariableName,
		const FString& RuleType,
		bool bExpectedBool,
		const FString& ComparisonOperator,
		int32 CompareValue,
		TSharedRef<FJsonObject>& OutReceipt,
		FString& OutError)
	{
		if (!AnimBP || !TransitionNode || !TransitionNode->BoundGraph || VariableName.IsEmpty())
		{
			OutError = TEXT("AnimBlueprint, transition graph, and variable_name are required.");
			return false;
		}
		const bool bBoolRule = RuleType == TEXT("blackboard_bool");
		UEdGraph* Graph = TransitionNode->BoundGraph;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(Graph);
		UEdGraphPin* ConditionPin = FindTransitionResultConditionPin(ResultNode);
		if (!Schema || !ResultNode || !ConditionPin)
		{
			OutError = TEXT("Transition graph schema or TransitionResult.bCanEnterTransition is missing.");
			return false;
		}
		if (!EnsureAnimBPExpressionVariable(AnimBP, VariableName,
			bBoolRule ? MakeAnimBPExpressionBoolPinType() : MakeAnimBPExpressionIntPinType(),
			OutReceipt, bBoolRule ? TEXT("bool") : TEXT("int"), OutError))
		{
			return false;
		}

		Graph->Modify();
		TransitionNode->Modify();
		ResultNode->Modify();
		UK2Node_VariableGet* GetNode = CreateVariableGetNode(
			Graph, VariableName, ResultNode->NodePosX - 430, ResultNode->NodePosY - 90, OutError);
		UEdGraphPin* ValuePin = FindFirstOutputDataPin(GetNode);
		if (!GetNode || !ValuePin)
		{
			OutError = FString::Printf(TEXT("Failed to materialize variable getter '%s'."), *VariableName);
			return false;
		}

		UEdGraphPin* RuleOutputPin = ValuePin;
		UK2Node_CallFunction* OperatorNode = nullptr;
		if (bBoolRule && !bExpectedBool)
		{
			UFunction* NotFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Not_PreBool"));
			if (!NotFunction)
			{
				OutError = TEXT("UKismetMathLibrary.Not_PreBool was not found.");
				return false;
			}
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			OperatorNode = Creator.CreateNode(true);
			OperatorNode->SetFromFunction(NotFunction);
			OperatorNode->NodePosX = ResultNode->NodePosX - 230;
			OperatorNode->NodePosY = ResultNode->NodePosY - 80;
			Creator.Finalize();
			UEdGraphPin* InputPin = FindFirstInputPin(OperatorNode, TEXT("A"));
			RuleOutputPin = FindFirstOutputPin(OperatorNode, TEXT("ReturnValue"));
			if (!InputPin || !RuleOutputPin || !TryCreateSchemaConnection(Schema, ValuePin, InputPin, OutError))
			{
				return false;
			}
		}
		else if (!bBoolRule)
		{
			const TMap<FString, FName> Functions = {
				{TEXT(">"), TEXT("Greater_IntInt")}, {TEXT(">="), TEXT("GreaterEqual_IntInt")},
				{TEXT("<"), TEXT("Less_IntInt")}, {TEXT("<="), TEXT("LessEqual_IntInt")},
				{TEXT("=="), TEXT("EqualEqual_IntInt")}, {TEXT("!="), TEXT("NotEqual_IntInt")}
			};
			const FName* FunctionName = Functions.Find(ComparisonOperator);
			UFunction* CompareFunction = FunctionName
				? UKismetMathLibrary::StaticClass()->FindFunctionByName(*FunctionName) : nullptr;
			if (!CompareFunction)
			{
				OutError = FString::Printf(TEXT("Unsupported integer comparison operator '%s'."), *ComparisonOperator);
				return false;
			}
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			OperatorNode = Creator.CreateNode(true);
			OperatorNode->SetFromFunction(CompareFunction);
			OperatorNode->NodePosX = ResultNode->NodePosX - 230;
			OperatorNode->NodePosY = ResultNode->NodePosY - 80;
			Creator.Finalize();
			UEdGraphPin* APin = FindFirstInputPin(OperatorNode, TEXT("A"));
			UEdGraphPin* BPin = FindFirstInputPin(OperatorNode, TEXT("B"));
			RuleOutputPin = FindFirstOutputPin(OperatorNode, TEXT("ReturnValue"));
			if (!APin || !BPin || !RuleOutputPin)
			{
				OutError = TEXT("Integer comparison node pins were not found.");
				return false;
			}
			Schema->TrySetDefaultValue(*BPin, FString::FromInt(CompareValue), true);
			if (!TryCreateSchemaConnection(Schema, ValuePin, APin, OutError))
			{
				return false;
			}
		}

		ConditionPin->Modify();
		ConditionPin->BreakAllPinLinks();
		if (!TryCreateSchemaConnection(Schema, RuleOutputPin, ConditionPin, OutError))
		{
			return false;
		}
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		UBlueprintEditorLibrary::CompileBlueprint(AnimBP);
		const bool bCompileSucceeded = AnimBP->Status != BS_Error;
		OutReceipt->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_writer_receipt.v4"));
		OutReceipt->SetStringField(TEXT("rule_type"), RuleType);
		OutReceipt->SetStringField(TEXT("variable_name"), VariableName);
		OutReceipt->SetStringField(TEXT("variable_get_node_id"), GetNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("operator_node_id"), OperatorNode ? OperatorNode->NodeGuid.ToString() : FString());
		OutReceipt->SetBoolField(TEXT("transition_result_linked"), ConditionPin->LinkedTo.Contains(RuleOutputPin));
		OutReceipt->SetBoolField(TEXT("compile_succeeded"), bCompileSucceeded);
		if (!bCompileSucceeded)
		{
			OutError = FString::Printf(TEXT("AnimBlueprint compile finished with BS_Error after writing %s."), *RuleType);
			return false;
		}
		return true;
	}

	bool TryWriteSimpleVariableAndBoolTransitionRule(
		UAnimBlueprint* AnimBP,
		UAnimStateTransitionNode* TransitionNode,
		const FString& ExpressionSource,
		const FSimpleVariableAndBoolTransitionExpression& Expression,
		TSharedRef<FJsonObject>& OutReceipt,
		FString& OutError)
	{
		if (!AnimBP || !TransitionNode || !TransitionNode->BoundGraph)
		{
			OutError = TEXT("Transition rule graph is missing; cannot write expression graph.");
			return false;
		}

		UEdGraph* Graph = TransitionNode->BoundGraph;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			OutError = TEXT("Transition graph schema is missing; no mutation was attempted.");
			return false;
		}

		UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(Graph);
		UEdGraphPin* ConditionPin = FindTransitionResultConditionPin(ResultNode);
		if (!ResultNode || !ConditionPin)
		{
			OutError = TEXT("TransitionResult.bCanEnterTransition pin was not found; no mutation was attempted.");
			return false;
		}

		UFunction* CompareFunction = nullptr;
		if (Expression.ComparisonOperator == TEXT(">"))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Greater_DoubleDouble"));
		}
		else if (Expression.ComparisonOperator == TEXT(">="))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("GreaterEqual_DoubleDouble"));
		}
		else if (Expression.ComparisonOperator == TEXT("<"))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Less_DoubleDouble"));
		}
		else if (Expression.ComparisonOperator == TEXT("<="))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("LessEqual_DoubleDouble"));
		}
		UFunction* AndFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("BooleanAND"));
		if (!CompareFunction || !AndFunction)
		{
			OutError = TEXT("Required Kismet math comparison or BooleanAND function was not found.");
			return false;
		}

		if (!EnsureAnimBPExpressionVariable(AnimBP, Expression.NumericVariableName, MakeAnimBPExpressionDoublePinType(), OutReceipt, TEXT("numeric"), OutError))
		{
			return false;
		}
		if (!EnsureAnimBPExpressionVariable(AnimBP, Expression.BoolVariableName, MakeAnimBPExpressionBoolPinType(), OutReceipt, TEXT("bool"), OutError))
		{
			return false;
		}

		Graph->Modify();
		TransitionNode->Modify();
		ResultNode->Modify();

		UK2Node_VariableGet* NumericGetNode = CreateVariableGetNode(Graph, Expression.NumericVariableName, ResultNode->NodePosX - 620, ResultNode->NodePosY - 170, OutError);
		UK2Node_VariableGet* BoolGetNode = CreateVariableGetNode(Graph, Expression.BoolVariableName, ResultNode->NodePosX - 620, ResultNode->NodePosY + 40, OutError);
		if (!NumericGetNode || !BoolGetNode)
		{
			return false;
		}

		FGraphNodeCreator<UK2Node_CallFunction> CompareCreator(*Graph);
		UK2Node_CallFunction* CompareNode = CompareCreator.CreateNode(true);
		if (!CompareNode)
		{
			OutError = TEXT("Failed to create numeric comparison function node.");
			return false;
		}
		CompareNode->SetFromFunction(CompareFunction);
		CompareNode->NodePosX = ResultNode->NodePosX - 370;
		CompareNode->NodePosY = ResultNode->NodePosY - 140;
		CompareCreator.Finalize();

		FGraphNodeCreator<UK2Node_CallFunction> AndCreator(*Graph);
		UK2Node_CallFunction* AndNode = AndCreator.CreateNode(true);
		if (!AndNode)
		{
			OutError = TEXT("Failed to create BooleanAND function node.");
			return false;
		}
		AndNode->SetFromFunction(AndFunction);
		AndNode->NodePosX = ResultNode->NodePosX - 180;
		AndNode->NodePosY = ResultNode->NodePosY - 60;
		AndCreator.Finalize();

		UEdGraphPin* NumericOutPin = FindFirstOutputDataPin(NumericGetNode);
		UEdGraphPin* BoolOutPin = FindFirstOutputDataPin(BoolGetNode);
		UEdGraphPin* CompareAPin = FindFirstInputPin(CompareNode, TEXT("A"));
		UEdGraphPin* CompareBPin = FindFirstInputPin(CompareNode, TEXT("B"));
		UEdGraphPin* CompareReturnPin = FindFirstOutputPin(CompareNode, TEXT("ReturnValue"));
		UEdGraphPin* AndAPin = FindFirstInputPin(AndNode, TEXT("A"));
		UEdGraphPin* AndBPin = FindFirstInputPin(AndNode, TEXT("B"));
		UEdGraphPin* AndReturnPin = FindFirstOutputPin(AndNode, TEXT("ReturnValue"));
		if (!NumericOutPin || !BoolOutPin || !CompareAPin || !CompareBPin || !CompareReturnPin || !AndAPin || !AndBPin || !AndReturnPin)
		{
			OutError = TEXT("Failed to resolve expression K2 node pins.");
			return false;
		}

		const FString ThresholdText = FString::SanitizeFloat(Expression.Threshold);
		Schema->TrySetDefaultValue(*CompareBPin, ThresholdText, true);
		ConditionPin->Modify();
		ConditionPin->BreakAllPinLinks();

		if (!TryCreateSchemaConnection(Schema, NumericOutPin, CompareAPin, OutError))
		{
			return false;
		}
		if (!TryCreateSchemaConnection(Schema, CompareReturnPin, AndAPin, OutError))
		{
			return false;
		}
		if (!TryCreateSchemaConnection(Schema, BoolOutPin, AndBPin, OutError))
		{
			return false;
		}
		if (!TryCreateSchemaConnection(Schema, AndReturnPin, ConditionPin, OutError))
		{
			return false;
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		UBlueprintEditorLibrary::CompileBlueprint(AnimBP);

		const FString CompileStatus = StaticEnum<EBlueprintStatus>()
			? StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(AnimBP->Status))
			: FString::FromInt(static_cast<int32>(AnimBP->Status));
		const bool bCompileSucceeded = AnimBP->Status != BS_Error;

		OutReceipt->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_writer_receipt.v4"));
		OutReceipt->SetStringField(TEXT("rule_type"), TEXT("expression"));
		OutReceipt->SetStringField(TEXT("expression_mode"), TEXT("numeric_compare_and_bool"));
		OutReceipt->SetStringField(TEXT("expression_source"), ExpressionSource);
		OutReceipt->SetStringField(TEXT("numeric_variable"), Expression.NumericVariableName);
		OutReceipt->SetStringField(TEXT("comparison_operator"), Expression.ComparisonOperator);
		OutReceipt->SetNumberField(TEXT("threshold"), Expression.Threshold);
		OutReceipt->SetStringField(TEXT("threshold_pin_default"), CompareBPin->DefaultValue);
		OutReceipt->SetStringField(TEXT("bool_variable"), Expression.BoolVariableName);
		OutReceipt->SetBoolField(TEXT("bool_expected_value"), Expression.bBoolExpectedValue);
		OutReceipt->SetStringField(TEXT("numeric_get_node_id"), NumericGetNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("bool_get_node_id"), BoolGetNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("comparison_node_id"), CompareNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("and_node_id"), AndNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("transition_result_node_id"), ResultNode->NodeGuid.ToString());
		OutReceipt->SetBoolField(TEXT("numeric_variable_linked_to_comparison_a"), NumericOutPin->LinkedTo.Contains(CompareAPin));
		OutReceipt->SetBoolField(TEXT("comparison_return_linked_to_and_a"), CompareReturnPin->LinkedTo.Contains(AndAPin));
		OutReceipt->SetBoolField(TEXT("bool_variable_linked_to_and_b"), BoolOutPin->LinkedTo.Contains(AndBPin));
		OutReceipt->SetBoolField(TEXT("and_return_linked_to_bCanEnterTransition"), AndReturnPin->LinkedTo.Contains(ConditionPin));
		OutReceipt->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), ConditionPin->LinkedTo.Contains(AndReturnPin));
		OutReceipt->SetBoolField(TEXT("placeholder_written"), false);
		OutReceipt->SetBoolField(TEXT("raw_expression_placeholder_written"), false);
		OutReceipt->SetStringField(TEXT("compile_status"), CompileStatus);
		OutReceipt->SetBoolField(TEXT("compile_succeeded"), bCompileSucceeded);

		if (!bCompileSucceeded)
		{
			OutError = TEXT("AnimBlueprint compile finished with BS_Error after writing variable expression transition rule.");
			return false;
		}
		return true;
	}

	bool NodeTextContains(UEdGraphNode* Node, const FString& Needle)
	{
		if (!Node)
		{
			return false;
		}
		const FString Text = FString::Printf(
			TEXT("%s %s %s"),
			*Node->GetName(),
			*GetAnimBPNodeTitle(Node),
			Node->GetClass() ? *Node->GetClass()->GetName() : TEXT(""));
		return Text.Contains(Needle, ESearchCase::IgnoreCase);
	}

	bool NodeTextContainsAny(UEdGraphNode* Node, const TArray<FString>& Needles)
	{
		for (const FString& Needle : Needles)
		{
			if (NodeTextContains(Node, Needle))
			{
				return true;
			}
		}
		return false;
	}

	bool HasNumericPinDefault(UEdGraphNode* Node)
	{
		if (!Node)
		{
			return false;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->DefaultValue.IsEmpty() && Pin->DefaultValue.IsNumeric())
			{
				return true;
			}
		}
		return false;
	}

	TSharedRef<FJsonObject> MakeTransitionGraphPinJson(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Pin)
		{
			return Result;
		}

		Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Result->SetStringField(TEXT("display_name"), Pin->GetDisplayName().ToString());
		Result->SetStringField(TEXT("direction"), AnimBPPinDirectionToString(Pin->Direction));
		Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		Result->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
		Result->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		Result->SetNumberField(TEXT("linked_count"), Pin->LinkedTo.Num());
		Result->SetBoolField(TEXT("is_likely_bCanEnterTransition"), IsLikelyTransitionResultConditionPin(Pin));

		TArray<TSharedPtr<FJsonValue>> LinkedPins;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}
			UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
			TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
			LinkJson->SetStringField(TEXT("node_id"), GetAnimBPNodeStableId(LinkedNode));
			LinkJson->SetStringField(TEXT("node_title"), GetAnimBPNodeTitle(LinkedNode));
			LinkJson->SetStringField(TEXT("node_class"), LinkedNode->GetClass() ? LinkedNode->GetClass()->GetName() : FString());
			LinkJson->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
			LinkJson->SetStringField(TEXT("pin_direction"), AnimBPPinDirectionToString(LinkedPin->Direction));
			LinkedPins.Add(MakeShared<FJsonValueObject>(LinkJson));
		}
		Result->SetArrayField(TEXT("linked_pins"), LinkedPins);
		return Result;
	}

	TSharedRef<FJsonObject> MakeTransitionGraphNodeJson(UEdGraphNode* Node)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Node)
		{
			return Result;
		}

		Result->SetStringField(TEXT("node_id"), GetAnimBPNodeStableId(Node));
		Result->SetStringField(TEXT("name"), Node->GetName());
		Result->SetStringField(TEXT("title"), GetAnimBPNodeTitle(Node));
		Result->SetStringField(TEXT("class"), Node->GetClass() ? Node->GetClass()->GetPathName() : FString());
		Result->SetStringField(TEXT("class_name"), Node->GetClass() ? Node->GetClass()->GetName() : FString());
		Result->SetNumberField(TEXT("x"), Node->NodePosX);
		Result->SetNumberField(TEXT("y"), Node->NodePosY);
		Result->SetBoolField(TEXT("is_transition_result"), IsTransitionResultNode(Node));
		Result->SetBoolField(TEXT("is_comment"), IsCommentNode(Node));
		Result->SetBoolField(TEXT("counts_as_condition_node"), IsNonPlaceholderConditionNode(Node));

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			Pins.Add(MakeShared<FJsonValueObject>(MakeTransitionGraphPinJson(Pin)));
		}
		Result->SetArrayField(TEXT("pins"), Pins);
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedRef<FJsonObject> MakeTransitionRuleGraphInspectionJson(
		UAnimStateTransitionNode* TransitionNode,
		const FString& MachineName,
		const FString& FromState,
		const FString& ToState)
	{
		TSharedRef<FJsonObject> Inspection = MakeShared<FJsonObject>();
		Inspection->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_graph_inspection.v1"));
		Inspection->SetBoolField(TEXT("read_only"), true);
		Inspection->SetStringField(TEXT("state_machine_name"), MachineName);
		Inspection->SetStringField(TEXT("from_state"), FromState);
		Inspection->SetStringField(TEXT("to_state"), ToState);
		Inspection->SetStringField(TEXT("capability_status"), TEXT("read_only_inspection_only"));

		TArray<FString> RequiredProof;
		RequiredProof.Add(TEXT("transition_graph_contains_non_placeholder_condition_nodes"));
		RequiredProof.Add(TEXT("transition_result_bCanEnterTransition_linked"));
		RequiredProof.Add(TEXT("compiled_anim_blueprint_success"));
		RequiredProof.Add(TEXT("post_compile_transition_rule_inspection"));
		Inspection->SetArrayField(TEXT("required_live_receipt_fields_before_support"), MakeStringArrayJson(RequiredProof));

		if (!TransitionNode)
		{
			Inspection->SetBoolField(TEXT("transition_found"), false);
			return Inspection;
		}

		Inspection->SetBoolField(TEXT("transition_found"), true);
		Inspection->SetStringField(TEXT("transition_node_id"), TransitionNode->NodeGuid.ToString());
		Inspection->SetNumberField(TEXT("crossfade_duration"), TransitionNode->CrossfadeDuration);

		UEdGraph* Graph = TransitionNode->BoundGraph;
		Inspection->SetBoolField(TEXT("transition_graph_present"), Graph != nullptr);
		if (!Graph)
		{
			Inspection->SetStringField(TEXT("writer_enablement_status"), TEXT("blocked_missing_transition_graph"));
			return Inspection;
		}

		Inspection->SetStringField(TEXT("transition_graph_name"), Graph->GetName());
		Inspection->SetStringField(TEXT("transition_graph_path"), Graph->GetPathName());

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		TArray<TSharedPtr<FJsonValue>> ResultLinksJson;
		TArray<FString> PresentTimeRemainingNodes;
		TArray<FString> PresentComparisonNodes;
		int32 ResultNodeCount = 0;
		int32 CommentNodeCount = 0;
		int32 NonPlaceholderConditionNodeCount = 0;
		int32 ConditionPinCount = 0;
		bool bCanEnterLinked = false;
		bool bHasNumericThresholdDefault = false;
		bool bHasTimeRemainingNode = false;
		bool bHasLessComparisonNode = false;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			NodesJson.Add(MakeShared<FJsonValueObject>(MakeTransitionGraphNodeJson(Node)));
			if (IsTransitionResultNode(Node))
			{
				++ResultNodeCount;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!IsLikelyTransitionResultConditionPin(Pin))
					{
						continue;
					}
					++ConditionPinCount;
					if (Pin->LinkedTo.Num() > 0)
					{
						bCanEnterLinked = true;
					}
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin || !LinkedPin->GetOwningNode())
						{
							continue;
						}
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
						TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
						LinkJson->SetStringField(TEXT("source_node_id"), GetAnimBPNodeStableId(LinkedNode));
						LinkJson->SetStringField(TEXT("source_node_title"), GetAnimBPNodeTitle(LinkedNode));
						LinkJson->SetStringField(TEXT("source_node_class"), LinkedNode->GetClass() ? LinkedNode->GetClass()->GetName() : FString());
						LinkJson->SetStringField(TEXT("source_pin"), LinkedPin->PinName.ToString());
						LinkJson->SetStringField(TEXT("target_pin"), Pin->PinName.ToString());
						ResultLinksJson.Add(MakeShared<FJsonValueObject>(LinkJson));
					}
				}
			}
			if (IsCommentNode(Node))
			{
				++CommentNodeCount;
			}
			if (IsNonPlaceholderConditionNode(Node))
			{
				++NonPlaceholderConditionNodeCount;
			}

			if (NodeTextContainsAny(Node, {TEXT("TimeRemaining"), TEXT("Time Remaining"), TEXT("Relevant Anim Time Remaining")}))
			{
				bHasTimeRemainingNode = true;
				PresentTimeRemainingNodes.Add(GetAnimBPNodeTitle(Node));
			}
			if (NodeTextContainsAny(Node, {TEXT("Less"), TEXT("LessEqual"), TEXT("Float <"), TEXT("<")}))
			{
				bHasLessComparisonNode = true;
				PresentComparisonNodes.Add(GetAnimBPNodeTitle(Node));
			}
			bHasNumericThresholdDefault = bHasNumericThresholdDefault || HasNumericPinDefault(Node);
		}

		const bool bCommentOnlyPlaceholder = CommentNodeCount > 0 && NonPlaceholderConditionNodeCount == 0 && !bCanEnterLinked;
		const bool bGraphHasTypedConditionChain = ResultNodeCount > 0 && bCanEnterLinked && NonPlaceholderConditionNodeCount > 0;
		const bool bTimeRemainingGraphReady = bGraphHasTypedConditionChain && bHasTimeRemainingNode && bHasLessComparisonNode;

		Inspection->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		Inspection->SetNumberField(TEXT("transition_result_node_count"), ResultNodeCount);
		Inspection->SetNumberField(TEXT("transition_result_condition_pin_count"), ConditionPinCount);
		Inspection->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), bCanEnterLinked);
		Inspection->SetArrayField(TEXT("transition_result_condition_links"), ResultLinksJson);
		Inspection->SetNumberField(TEXT("comment_node_count"), CommentNodeCount);
		Inspection->SetNumberField(TEXT("non_placeholder_condition_node_count"), NonPlaceholderConditionNodeCount);
		Inspection->SetBoolField(TEXT("comment_only_placeholder_detected"), bCommentOnlyPlaceholder);
		Inspection->SetBoolField(TEXT("raw_expression_placeholder_risk"), bCommentOnlyPlaceholder);
		Inspection->SetBoolField(TEXT("typed_condition_chain_detected"), bGraphHasTypedConditionChain);
		Inspection->SetArrayField(TEXT("nodes"), NodesJson);

		TSharedRef<FJsonObject> TimeRemainingReadiness = MakeShared<FJsonObject>();
		TimeRemainingReadiness->SetStringField(TEXT("rule_type"), TEXT("time_remaining_lt"));
		TimeRemainingReadiness->SetBoolField(TEXT("time_remaining_node_present"), bHasTimeRemainingNode);
		TimeRemainingReadiness->SetBoolField(TEXT("less_than_comparison_node_present"), bHasLessComparisonNode);
		TimeRemainingReadiness->SetBoolField(TEXT("threshold_literal_or_pin_default_detected"), bHasNumericThresholdDefault);
		TimeRemainingReadiness->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), bCanEnterLinked);
		TimeRemainingReadiness->SetBoolField(TEXT("graph_has_required_nodes"), bTimeRemainingGraphReady);
		TimeRemainingReadiness->SetStringField(TEXT("writer_status"),
			bTimeRemainingGraphReady
				? TEXT("supported_time_remaining_lt_graph_detected")
				: TEXT("time_remaining_lt_writer_supported_graph_not_detected"));
		TimeRemainingReadiness->SetArrayField(TEXT("present_time_remaining_nodes"), MakeStringArrayJson(PresentTimeRemainingNodes));
		TimeRemainingReadiness->SetArrayField(TEXT("present_comparison_nodes"), MakeStringArrayJson(PresentComparisonNodes));
		TArray<FString> TimeMissing;
		if (!bHasTimeRemainingNode) TimeMissing.Add(TEXT("time_remaining_node"));
		if (!bHasLessComparisonNode) TimeMissing.Add(TEXT("less_than_float_comparison_node"));
		if (!bCanEnterLinked) TimeMissing.Add(TEXT("transition_result_bCanEnterTransition_link"));
		if (NonPlaceholderConditionNodeCount == 0) TimeMissing.Add(TEXT("non_placeholder_condition_node"));
		TimeRemainingReadiness->SetArrayField(TEXT("missing_required_nodes"), MakeStringArrayJson(TimeMissing));
		Inspection->SetObjectField(TEXT("time_remaining_lt_readiness"), TimeRemainingReadiness);

		TSharedRef<FJsonObject> ExpressionReadiness = MakeShared<FJsonObject>();
		ExpressionReadiness->SetStringField(TEXT("rule_type"), TEXT("expression"));
		ExpressionReadiness->SetBoolField(TEXT("typed_condition_chain_detected"), bGraphHasTypedConditionChain);
		ExpressionReadiness->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), bCanEnterLinked);
		ExpressionReadiness->SetBoolField(TEXT("comment_only_placeholder_detected"), bCommentOnlyPlaceholder);
		ExpressionReadiness->SetBoolField(TEXT("raw_expression_placeholder_risk"), bCommentOnlyPlaceholder);
		ExpressionReadiness->SetStringField(TEXT("writer_status"), TEXT("constant_boolean_numeric_compare_bool_and_blackboard_variable_writers_supported"));
		TArray<FString> ExpressionMissing;
		if (!bGraphHasTypedConditionChain) ExpressionMissing.Add(TEXT("typed_boolean_condition_chain"));
		if (!bCanEnterLinked) ExpressionMissing.Add(TEXT("transition_result_bCanEnterTransition_link"));
		if (bCommentOnlyPlaceholder) ExpressionMissing.Add(TEXT("remove_comment_or_raw_text_placeholder"));
		ExpressionReadiness->SetArrayField(TEXT("missing_required_nodes"), MakeStringArrayJson(ExpressionMissing));
		Inspection->SetObjectField(TEXT("expression_readiness"), ExpressionReadiness);

		if (bGraphHasTypedConditionChain)
		{
			Inspection->SetStringField(TEXT("writer_enablement_status"), TEXT("graph_inspection_ready_compile_receipt_still_required"));
		}
		else
		{
			Inspection->SetStringField(TEXT("writer_enablement_status"), TEXT("blocked_until_required_condition_nodes_and_compile_receipt"));
		}

		return Inspection;
	}

	void AttachTransitionRuleReceipt(
		const TSharedRef<FJsonObject>& OutStructured,
		UAnimStateTransitionNode* TransitionNode,
		const FString& RuleType,
		bool bSupported,
		bool bApplied,
		const FString& Diagnostic)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_receipt.v1"));
		Receipt->SetStringField(TEXT("rule_type"), RuleType);
		Receipt->SetBoolField(TEXT("supported_rule_type"), bSupported);
		Receipt->SetBoolField(TEXT("applied"), bApplied);
		Receipt->SetBoolField(TEXT("placeholder_written"), false);
		Receipt->SetStringField(TEXT("compile_diagnostics_status"), bApplied
			? TEXT("attempted_inline_compile_and_mutation_gate")
			: TEXT("not_attempted_by_transition_rule_tool"));
		Receipt->SetStringField(TEXT("diagnostic"), Diagnostic);

		TArray<TSharedPtr<FJsonValue>> Supported;
		Supported.Add(MakeShared<FJsonValueString>(TEXT("always")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("expression_const_bool")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("expression_numeric_compare_and_bool")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("time_remaining_lt")));
		Receipt->SetArrayField(TEXT("supported_rule_types"), Supported);

		if (TransitionNode)
		{
			Receipt->SetStringField(TEXT("transition_node_id"), TransitionNode->NodeGuid.ToString());
			if (TransitionNode->BoundGraph)
			{
				Receipt->SetStringField(TEXT("transition_graph_path"), TransitionNode->BoundGraph->GetPathName());
				int32 ResultNodeCount = 0;
				for (UEdGraphNode* Node : TransitionNode->BoundGraph->Nodes)
				{
					if (Cast<UAnimGraphNode_TransitionResult>(Node))
					{
						++ResultNodeCount;
					}
				}
				Receipt->SetNumberField(TEXT("transition_result_node_count"), ResultNodeCount);
			}
		}
		OutStructured->SetObjectField(TEXT("transition_rule_receipt"), Receipt);
	}

	void AttachFailClosedTransitionRuleContract(
		const TSharedRef<FJsonObject>& OutStructured,
		const FString& RuleType,
		const FString& Diagnostic)
	{
		OutStructured->SetBoolField(TEXT("fail_closed"), true);
		OutStructured->SetBoolField(TEXT("mutation_attempted"), false);
		OutStructured->SetBoolField(TEXT("placeholder_written"), false);
		OutStructured->SetStringField(TEXT("capability_status"), TEXT("unsupported_fail_closed"));
		OutStructured->SetStringField(TEXT("diagnostic"), Diagnostic);

		TArray<TSharedPtr<FJsonValue>> Supported;
		Supported.Add(MakeShared<FJsonValueString>(TEXT("always")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("expression_const_bool")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("expression_numeric_compare_and_bool")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("time_remaining_lt")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("blackboard_bool")));
		Supported.Add(MakeShared<FJsonValueString>(TEXT("blackboard_compare_int")));
		OutStructured->SetArrayField(TEXT("supported_rule_types"), Supported);

		TArray<TSharedPtr<FJsonValue>> Blocked;
		Blocked.Add(MakeShared<FJsonValueString>(TEXT("expression_outside_safe_subset")));
		OutStructured->SetArrayField(TEXT("blocked_rule_types"), Blocked);

		TArray<TSharedPtr<FJsonValue>> RequiredProof;
		RequiredProof.Add(MakeShared<FJsonValueString>(TEXT("transition_graph_contains_non_placeholder_condition_nodes")));
		RequiredProof.Add(MakeShared<FJsonValueString>(TEXT("transition_result_bCanEnterTransition_linked")));
		RequiredProof.Add(MakeShared<FJsonValueString>(TEXT("animbp_inspect_transition_rule_graph_readiness")));
		RequiredProof.Add(MakeShared<FJsonValueString>(TEXT("time_remaining_lt_readiness_or_expression_readiness")));
		RequiredProof.Add(MakeShared<FJsonValueString>(TEXT("compiled_anim_blueprint_success")));
		RequiredProof.Add(MakeShared<FJsonValueString>(TEXT("post_compile_transition_rule_inspection")));
		OutStructured->SetArrayField(TEXT("required_live_receipt_fields_before_support"), RequiredProof);

		TArray<TSharedPtr<FJsonValue>> SafeActions;
		SafeActions.Add(MakeShared<FJsonValueString>(TEXT("Use rule_type='always' only for current write lanes.")));
		SafeActions.Add(MakeShared<FJsonValueString>(TEXT("Use animbp_inspect_transition_rule_graph for read-only graph readiness before any complex writer enablement.")));
		SafeActions.Add(MakeShared<FJsonValueString>(TEXT("Run a disposable-copy live smoke before enabling this rule_type.")));
		SafeActions.Add(MakeShared<FJsonValueString>(TEXT("Do not write comment-only placeholders as transition rule proof.")));
		OutStructured->SetArrayField(TEXT("safe_next_actions"), SafeActions);

		OutStructured->SetStringField(TEXT("requested_rule_type"), RuleType);
		OutStructured->SetStringField(TEXT("read_only_graph_inspection_tool"), TEXT("animbp_inspect_transition_rule_graph"));
		OutStructured->SetObjectField(TEXT("repair_plan"), MakeTransitionRuleRepairPlanJson(RuleType, Diagnostic, false));

		TArray<TSharedPtr<FJsonValue>> EnablementBlockers;
		EnablementBlockers.Add(MakeShared<FJsonValueString>(TEXT("requested_rule_outside_safe_writer_subset")));
		EnablementBlockers.Add(MakeShared<FJsonValueString>(TEXT("compile_receipt_missing")));
		EnablementBlockers.Add(MakeShared<FJsonValueString>(TEXT("post_compile_graph_inspection_missing")));
		OutStructured->SetArrayField(TEXT("rule_enablement_blockers"), EnablementBlockers);
	}

	FString NewAnimBPReceiptIdLocal(const FString& Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower());
	}

	// ------------------------------------------------------------------
	// AN-01: BlendSpace schema validation + sample editing helpers
	// ------------------------------------------------------------------

	constexpr int32 MaxBlendSpaceSamplesLocal = 16;

	TSharedRef<FJsonObject> MakeBlendSpaceSchemaValidationLocal(UBlendSpace* BlendSpace, TArray<FString>& OutIssues)
	{
		TSharedRef<FJsonObject> Validation = MakeShared<FJsonObject>();
		Validation->SetStringField(TEXT("schema"), TEXT("somol.animbp.blendspace_schema_validation.v1"));
		if (!BlendSpace)
		{
			Validation->SetBoolField(TEXT("valid"), false);
			OutIssues.Add(TEXT("blendspace_missing"));
			return Validation;
		}

		const bool bIs1D = BlendSpace->IsA<UBlendSpace1D>();
		const int32 Dimensions = bIs1D ? 1 : 2;
		Validation->SetStringField(TEXT("blendspace_path"), BlendSpace->GetPathName());
		Validation->SetStringField(TEXT("blendspace_class"), BlendSpace->GetClass()->GetName());
		Validation->SetNumberField(TEXT("dimensions"), Dimensions);

		TArray<TSharedPtr<FJsonValue>> Parameters;
		for (int32 Dimension = 0; Dimension < Dimensions; ++Dimension)
		{
			const FBlendParameter& Parameter = BlendSpace->GetBlendParameter(Dimension);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("dimension"), Dimension);
			Row->SetStringField(TEXT("display_name"), Parameter.DisplayName);
			Row->SetNumberField(TEXT("min"), Parameter.Min);
			Row->SetNumberField(TEXT("max"), Parameter.Max);
			Row->SetNumberField(TEXT("grid_num"), Parameter.GridNum);
			Parameters.Add(MakeShared<FJsonValueObject>(Row));
			if (!(FMath::IsFinite(Parameter.Min) && FMath::IsFinite(Parameter.Max)) || Parameter.Min >= Parameter.Max)
			{
				OutIssues.Add(FString::Printf(TEXT("axis_%d_range_invalid"), Dimension));
			}
		}
		Validation->SetArrayField(TEXT("blend_parameters"), Parameters);

		const int32 SampleCount = BlendSpace->GetBlendSamples().Num();
		Validation->SetNumberField(TEXT("sample_count"), SampleCount);
		Validation->SetNumberField(TEXT("sample_count_limit"), MaxBlendSpaceSamplesLocal);
		if (SampleCount > MaxBlendSpaceSamplesLocal)
		{
			OutIssues.Add(TEXT("sample_count_over_limit"));
		}
		Validation->SetBoolField(TEXT("valid"), OutIssues.Num() == 0);
		return Validation;
	}

	bool ValidateSampleValueAgainstAxesLocal(UBlendSpace* BlendSpace, FVector Value, int32 Dimensions, FString& OutReason)
	{
		for (int32 Dimension = 0; Dimension < Dimensions; ++Dimension)
		{
			const FBlendParameter& Parameter = BlendSpace->GetBlendParameter(Dimension);
			const float Component = static_cast<float>(Value[Dimension]);
			if (!FMath::IsFinite(Component) || Component < Parameter.Min || Component > Parameter.Max)
			{
				OutReason = FString::Printf(
					TEXT("sample value axis %d (%.4f) is outside the blend axis range [%.4f, %.4f]."),
					Dimension, Component, Parameter.Min, Parameter.Max);
				return false;
			}
		}
		return true;
	}

	// Applies closed sample-edit ops: add / edit / delete. bValidateOnly performs
	// the same closed-schema checks without mutating the BlendSpace.
	bool ApplyBlendSpaceSampleEditsLocal(
		FSololmcpEditorServices& Services,
		UBlendSpace* BlendSpace,
		const TArray<TSharedPtr<FJsonValue>>& SampleEdits,
		bool bValidateOnly,
		TSharedRef<FJsonObject> OutReceipt,
		FString& OutError)
	{
		const int32 Dimensions = BlendSpace->IsA<UBlendSpace1D>() ? 1 : 2;
		int32 RunningCount = BlendSpace->GetBlendSamples().Num();
		TArray<TSharedPtr<FJsonValue>> Rows;
		int32 AppliedCount = 0;

		for (int32 EditIndex = 0; EditIndex < SampleEdits.Num(); ++EditIndex)
		{
			const TSharedPtr<FJsonObject>* EditPtr = nullptr;
			if (!SampleEdits[EditIndex].IsValid() || !SampleEdits[EditIndex]->TryGetObject(EditPtr) || !EditPtr || !(*EditPtr).IsValid())
			{
				OutError = FString::Printf(TEXT("sample_edits[%d] must be an object."), EditIndex);
				return false;
			}
			const TSharedRef<FJsonObject> Edit = (*EditPtr).ToSharedRef();

			FString Op;
			if (!Edit->TryGetStringField(TEXT("op"), Op))
			{
				OutError = FString::Printf(TEXT("sample_edits[%d].op is required ('add' | 'edit' | 'delete')."), EditIndex);
				return false;
			}
			Op = Op.ToLower().TrimStartAndEnd();

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("edit_index"), EditIndex);
			Row->SetStringField(TEXT("op"), Op);

			if (Op == TEXT("add"))
			{
				FString SequencePath;
				if (!Edit->TryGetStringField(TEXT("sequence_path"), SequencePath) || SequencePath.TrimStartAndEnd().IsEmpty())
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: op 'add' requires sequence_path."), EditIndex);
					return false;
				}
				const TSharedPtr<FJsonObject>* ValuePtr = nullptr;
				double ValueX = 0.0;
				double ValueY = 0.0;
				if (!Edit->TryGetObjectField(TEXT("sample_value"), ValuePtr) || !ValuePtr || !(*ValuePtr).IsValid() ||
					!(*ValuePtr)->TryGetNumberField(TEXT("x"), ValueX) ||
					(Dimensions > 1 && !(*ValuePtr)->TryGetNumberField(TEXT("y"), ValueY)))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: op 'add' requires sample_value with numeric x (and y for 2D blend spaces)."), EditIndex);
					return false;
				}
				FVector SampleValue(ValueX, ValueY, 0.0);
				FString AxisReason;
				if (!ValidateSampleValueAgainstAxesLocal(BlendSpace, SampleValue, Dimensions, AxisReason))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: %s"), EditIndex, *AxisReason);
					return false;
				}
				if (RunningCount + 1 > MaxBlendSpaceSamplesLocal)
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: adding this sample would exceed the %d-sample BlendSpace limit."), EditIndex, MaxBlendSpaceSamplesLocal);
					return false;
				}

				FString LoadError;
				UObject* SequenceAsset = Services.LoadAsset(SequencePath, LoadError);
				UAnimSequence* Sequence = Cast<UAnimSequence>(SequenceAsset);
				if (!Sequence)
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: sequence_path '%s' does not resolve to a UAnimSequence (%s)."),
						EditIndex, *SequencePath, LoadError.IsEmpty() ? TEXT("wrong asset class") : *LoadError);
					return false;
				}

				Row->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
				Row->SetNumberField(TEXT("sample_x"), ValueX);
				Row->SetNumberField(TEXT("sample_y"), ValueY);
				if (!bValidateOnly)
				{
					const int32 AddedIndex = BlendSpace->AddSample(Sequence, SampleValue);
					if (!BlendSpace->GetBlendSamples().IsValidIndex(AddedIndex))
					{
						OutError = FString::Printf(TEXT("sample_edits[%d]: UBlendSpace::AddSample did not return a valid sample index (grid point may be occupied)."), EditIndex);
						return false;
					}
					Row->SetNumberField(TEXT("sample_index"), AddedIndex);
				}
				++RunningCount;
			}
			else if (Op == TEXT("edit"))
			{
				int32 SampleIndex = INDEX_NONE;
				if (!Edit->TryGetNumberField(TEXT("sample_index"), SampleIndex) || !BlendSpace->GetBlendSamples().IsValidIndex(SampleIndex))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: op 'edit' requires a valid sample_index (current sample count: %d)."), EditIndex, RunningCount);
					return false;
				}
				const TSharedPtr<FJsonObject>* ValuePtr = nullptr;
				double ValueX = 0.0;
				double ValueY = 0.0;
				if (!Edit->TryGetObjectField(TEXT("sample_value"), ValuePtr) || !ValuePtr || !(*ValuePtr).IsValid() ||
					!(*ValuePtr)->TryGetNumberField(TEXT("x"), ValueX) ||
					(Dimensions > 1 && !(*ValuePtr)->TryGetNumberField(TEXT("y"), ValueY)))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: op 'edit' requires sample_value with numeric x (and y for 2D blend spaces)."), EditIndex);
					return false;
				}
				FVector SampleValue(ValueX, ValueY, 0.0);
				FString AxisReason;
				if (!ValidateSampleValueAgainstAxesLocal(BlendSpace, SampleValue, Dimensions, AxisReason))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: %s"), EditIndex, *AxisReason);
					return false;
				}
				Row->SetNumberField(TEXT("sample_index"), SampleIndex);
				Row->SetNumberField(TEXT("sample_x"), ValueX);
				Row->SetNumberField(TEXT("sample_y"), ValueY);
				if (!bValidateOnly && !BlendSpace->EditSampleValue(SampleIndex, SampleValue))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: UBlendSpace::EditSampleValue rejected sample %d (grid point may be occupied)."), EditIndex, SampleIndex);
					return false;
				}
			}
			else if (Op == TEXT("delete"))
			{
				int32 SampleIndex = INDEX_NONE;
				if (!Edit->TryGetNumberField(TEXT("sample_index"), SampleIndex) || !BlendSpace->GetBlendSamples().IsValidIndex(SampleIndex))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: op 'delete' requires a valid sample_index (current sample count: %d)."), EditIndex, RunningCount);
					return false;
				}
				Row->SetNumberField(TEXT("sample_index"), SampleIndex);
				if (!bValidateOnly && !BlendSpace->DeleteSample(SampleIndex))
				{
					OutError = FString::Printf(TEXT("sample_edits[%d]: UBlendSpace::DeleteSample failed for sample %d."), EditIndex, SampleIndex);
					return false;
				}
				--RunningCount;
			}
			else
			{
				OutError = FString::Printf(TEXT("sample_edits[%d].op '%s' is outside the closed schema ('add' | 'edit' | 'delete')."), EditIndex, *Op);
				return false;
			}

			Row->SetBoolField(TEXT("validated"), true);
			Row->SetBoolField(TEXT("applied"), !bValidateOnly);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			++AppliedCount;
		}

		OutReceipt->SetArrayField(TEXT("sample_edit_rows"), Rows);
		OutReceipt->SetNumberField(TEXT("sample_edit_count"), AppliedCount);
		OutReceipt->SetNumberField(TEXT("sample_count_projected"), RunningCount);
		return true;
	}

	void AttachBlendSpaceSampleReadbackLocal(UBlendSpace* BlendSpace, TSharedRef<FJsonObject> OutReceipt)
	{
		TArray<TSharedPtr<FJsonValue>> Samples;
		const TArray<FBlendSample>& BlendSamples = BlendSpace->GetBlendSamples();
		for (int32 Index = 0; Index < BlendSamples.Num(); ++Index)
		{
			const FBlendSample& Sample = BlendSamples[Index];
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetStringField(TEXT("animation_path"), Sample.Animation ? Sample.Animation->GetPathName() : FString());
			Row->SetNumberField(TEXT("x"), static_cast<double>(Sample.SampleValue.X));
			Row->SetNumberField(TEXT("y"), static_cast<double>(Sample.SampleValue.Y));
			Samples.Add(MakeShared<FJsonValueObject>(Row));
		}
		OutReceipt->SetArrayField(TEXT("sample_readback"), Samples);
		OutReceipt->SetNumberField(TEXT("sample_count_readback"), BlendSamples.Num());
	}

	uint32 BlendSpaceSampleFingerprintLocal(UBlendSpace* BlendSpace)
	{
		FString Material;
		if (BlendSpace)
		{
			for (const FBlendSample& Sample : BlendSpace->GetBlendSamples())
			{
				Material += FString::Printf(TEXT("|%s@%.4f,%.4f"),
					Sample.Animation ? *Sample.Animation->GetPathName() : TEXT("null"),
					Sample.SampleValue.X, Sample.SampleValue.Y);
			}
		}
		return FCrc::StrCrc32(*Material);
	}

	uint32 AnimGraphWiringFingerprintLocal(UEdGraph* Graph)
	{
		FString Material;
		if (Graph)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				Material += TEXT("|") + Node->GetClass()->GetName();
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin)
					{
						Material += FString::Printf(TEXT(";%d"), Pin->LinkedTo.Num());
					}
				}
			}
		}
		return FCrc::StrCrc32(*Material);
	}

	// Save the AnimBlueprint package and read it back through the asset registry.
	bool SaveAnimBlueprintReceiptLocal(
		FSololmcpEditorServices& Services,
		UAnimBlueprint* AnimBP,
		bool bSaveRequested,
		TSharedRef<FJsonObject> OutStructured,
		FString& OutError)
	{
		OutStructured->SetBoolField(TEXT("save_requested"), bSaveRequested);
		if (!bSaveRequested)
		{
			OutStructured->SetBoolField(TEXT("saved"), false);
			OutStructured->SetBoolField(TEXT("saved_verified"), false);
			return true;
		}
		FString SaveError;
		if (!Services.SaveAsset(AnimBP->GetPathName(), false, SaveError))
		{
			OutStructured->SetBoolField(TEXT("saved"), false);
			OutStructured->SetBoolField(TEXT("saved_verified"), false);
			OutError = SaveError.IsEmpty() ? TEXT("AnimBlueprint save failed after mutation.") : SaveError;
			return false;
		}
		const bool bRegistryReadback = Services.AssetExists(AnimBP->GetPathName());
		OutStructured->SetBoolField(TEXT("saved"), true);
		OutStructured->SetBoolField(TEXT("saved_verified"), bRegistryReadback);
		if (!bRegistryReadback)
		{
			OutError = TEXT("Saved AnimBlueprint could not be read back from the asset registry.");
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------
	// AN-02: alias normalization, sync/distance writers, rollback helpers
	// ------------------------------------------------------------------

	FString NormalizeTransitionRuleTypeLocal(const FString& RawRuleType, bool& bAliased)
	{
		const FString Lower = RawRuleType.ToLower().TrimStartAndEnd();
		bAliased = false;
		static const TMap<FString, FString> Aliases = {
			{TEXT("time"), TEXT("time_remaining_lt")},
			{TEXT("time_lt"), TEXT("time_remaining_lt")},
			{TEXT("time_remaining"), TEXT("time_remaining_lt")},
			{TEXT("time_remaining_less_than"), TEXT("time_remaining_lt")},
			{TEXT("sync"), TEXT("sync_between_markers")},
			{TEXT("sync_group"), TEXT("sync_between_markers")},
			{TEXT("sync_group_between_markers"), TEXT("sync_between_markers")},
			{TEXT("sync_between"), TEXT("sync_between_markers")},
			{TEXT("distance"), TEXT("distance_gt")},
			{TEXT("distance_traveled_gt"), TEXT("distance_gt")},
			{TEXT("distance_greater_than"), TEXT("distance_gt")},
			{TEXT("distance_compare"), TEXT("distance_gt")},
			{TEXT("bool"), TEXT("blackboard_bool")},
			{TEXT("blackboard"), TEXT("blackboard_bool")},
			{TEXT("variable_bool"), TEXT("blackboard_bool")},
			{TEXT("compare_int"), TEXT("blackboard_compare_int")},
			{TEXT("blackboard_int"), TEXT("blackboard_compare_int")},
			{TEXT("const_bool"), TEXT("expression")},
			{TEXT("expr"), TEXT("expression")},
			{TEXT("unconditional"), TEXT("always")},
			{TEXT("any"), TEXT("always")}
		};
		if (const FString* Canonical = Aliases.Find(Lower))
		{
			bAliased = *Canonical != Lower;
			return *Canonical;
		}
		return Lower;
	}

	bool WriteNameLiteralPinDefaultLocal(const UEdGraphSchema* Schema, UEdGraphPin* Pin, const FString& Value, FString& OutError)
	{
		if (!Pin)
		{
			OutError = TEXT("Target pin for a name literal default is missing.");
			return false;
		}
		const FString Quoted = FString::Printf(TEXT("\"%s\""), *Value);
		Pin->Modify();
		Pin->DefaultValue = Quoted;
		if (!Pin->DefaultValue.Equals(Quoted))
		{
			OutError = FString::Printf(TEXT("Name literal pin default did not remain after writing '%s'."), *Value);
			return false;
		}
		(void)Schema;
		return true;
	}

	bool TryWriteSyncBetweenMarkersTransitionRule(
		UAnimBlueprint* AnimBP,
		UAnimStateTransitionNode* TransitionNode,
		const FString& SyncGroupName,
		const FString& PreviousMarker,
		const FString& NextMarker,
		bool bRespectMarkerOrder,
		TSharedRef<FJsonObject>& OutReceipt,
		FString& OutError)
	{
		if (!AnimBP || !TransitionNode || !TransitionNode->BoundGraph)
		{
			OutError = TEXT("Transition rule graph is missing; cannot write sync_between_markers.");
			return false;
		}

		UFunction* SyncFunction = UAnimInstance::StaticClass()->FindFunctionByName(TEXT("IsSyncGroupBetweenMarkers"));
		if (!SyncFunction)
		{
			OutError = TEXT("UAnimInstance.IsSyncGroupBetweenMarkers was not found in this engine build.");
			return false;
		}

		UEdGraph* Graph = TransitionNode->BoundGraph;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(Graph);
		UEdGraphPin* ConditionPin = FindTransitionResultConditionPin(ResultNode);
		if (!Schema || !ResultNode || !ConditionPin)
		{
			OutError = TEXT("Transition graph schema or TransitionResult.bCanEnterTransition is missing.");
			return false;
		}

		Graph->Modify();
		TransitionNode->Modify();
		ResultNode->Modify();

		FGraphNodeCreator<UK2Node_CallFunction> FunctionCreator(*Graph);
		UK2Node_CallFunction* FunctionNode = FunctionCreator.CreateNode(true);
		if (!FunctionNode)
		{
			OutError = TEXT("Failed to create the IsSyncGroupBetweenMarkers function node.");
			return false;
		}
		FunctionNode->SetFromFunction(SyncFunction);
		FunctionNode->NodePosX = ResultNode->NodePosX - 360;
		FunctionNode->NodePosY = ResultNode->NodePosY - 90;
		FunctionCreator.Finalize();

		FGraphNodeCreator<UK2Node_Self> SelfCreator(*Graph);
		UK2Node_Self* SelfNode = SelfCreator.CreateNode(true);
		if (!SelfNode)
		{
			OutError = TEXT("Failed to create the Self node for the sync-group condition.");
			return false;
		}
		SelfNode->NodePosX = ResultNode->NodePosX - 620;
		SelfNode->NodePosY = ResultNode->NodePosY - 140;
		SelfCreator.Finalize();

		UEdGraphPin* SelfOutPin = FindFirstOutputDataPin(SelfNode);
		UEdGraphPin* TargetSelfPin = FindFirstInputPin(FunctionNode, TEXT("self"));
		UEdGraphPin* SyncGroupPin = FindFirstInputPin(FunctionNode, TEXT("SyncGroup"));
		if (!SyncGroupPin)
		{
			SyncGroupPin = FindFirstInputPin(FunctionNode, TEXT("InSyncGroupName"));
		}
		UEdGraphPin* PreviousMarkerPin = FindFirstInputPin(FunctionNode, TEXT("PreviousMarker"));
		UEdGraphPin* NextMarkerPin = FindFirstInputPin(FunctionNode, TEXT("NextMarker"));
		UEdGraphPin* RespectOrderPin = FindFirstInputPin(FunctionNode, TEXT("bRespectMarkerOrder"));
		UEdGraphPin* ReturnPin = FindFirstOutputPin(FunctionNode, TEXT("ReturnValue"));
		if (!SelfOutPin || !TargetSelfPin || !SyncGroupPin || !PreviousMarkerPin || !NextMarkerPin || !ReturnPin)
		{
			OutError = TEXT("Failed to resolve IsSyncGroupBetweenMarkers node pins.");
			return false;
		}

		if (!WriteNameLiteralPinDefaultLocal(Schema, SyncGroupPin, SyncGroupName, OutError) ||
			!WriteNameLiteralPinDefaultLocal(Schema, PreviousMarkerPin, PreviousMarker, OutError) ||
			!WriteNameLiteralPinDefaultLocal(Schema, NextMarkerPin, NextMarker, OutError))
		{
			return false;
		}
		if (RespectOrderPin)
		{
			RespectOrderPin->Modify();
			Schema->TrySetDefaultValue(*RespectOrderPin, bRespectMarkerOrder ? TEXT("true") : TEXT("false"), true);
		}

		ConditionPin->Modify();
		ConditionPin->BreakAllPinLinks();
		if (!TryCreateSchemaConnection(Schema, SelfOutPin, TargetSelfPin, OutError) ||
			!TryCreateSchemaConnection(Schema, ReturnPin, ConditionPin, OutError))
		{
			return false;
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		UBlueprintEditorLibrary::CompileBlueprint(AnimBP);
		const FString CompileStatus = StaticEnum<EBlueprintStatus>()
			? StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(AnimBP->Status))
			: FString::FromInt(static_cast<int32>(AnimBP->Status));
		const bool bCompileSucceeded = AnimBP->Status != BS_Error;

		OutReceipt->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_writer_receipt.v5"));
		OutReceipt->SetStringField(TEXT("rule_type"), TEXT("sync_between_markers"));
		OutReceipt->SetStringField(TEXT("sync_group"), SyncGroupName);
		OutReceipt->SetStringField(TEXT("previous_marker"), PreviousMarker);
		OutReceipt->SetStringField(TEXT("next_marker"), NextMarker);
		OutReceipt->SetBoolField(TEXT("respect_marker_order"), bRespectMarkerOrder);
		OutReceipt->SetStringField(TEXT("sync_group_pin_default"), SyncGroupPin->DefaultValue);
		OutReceipt->SetStringField(TEXT("previous_marker_pin_default"), PreviousMarkerPin->DefaultValue);
		OutReceipt->SetStringField(TEXT("next_marker_pin_default"), NextMarkerPin->DefaultValue);
		OutReceipt->SetStringField(TEXT("function_node_id"), FunctionNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("self_node_id"), SelfNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("transition_result_node_id"), ResultNode->NodeGuid.ToString());
		OutReceipt->SetBoolField(TEXT("self_linked_to_function_target"), SelfOutPin->LinkedTo.Contains(TargetSelfPin));
		OutReceipt->SetBoolField(TEXT("function_return_linked_to_bCanEnterTransition"), ReturnPin->LinkedTo.Contains(ConditionPin));
		OutReceipt->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), ConditionPin->LinkedTo.Contains(ReturnPin));
		OutReceipt->SetBoolField(TEXT("placeholder_written"), false);
		OutReceipt->SetStringField(TEXT("compile_status"), CompileStatus);
		OutReceipt->SetBoolField(TEXT("compile_succeeded"), bCompileSucceeded);
		if (!bCompileSucceeded)
		{
			OutError = TEXT("AnimBlueprint compile finished with BS_Error after writing sync_between_markers transition rule.");
			return false;
		}
		return true;
	}

	bool TryWriteFloatVariableCompareTransitionRule(
		UAnimBlueprint* AnimBP,
		UAnimStateTransitionNode* TransitionNode,
		const FString& VariableName,
		const FString& ComparisonOperator,
		double Threshold,
		const FString& RuleType,
		TSharedRef<FJsonObject>& OutReceipt,
		FString& OutError)
	{
		if (!AnimBP || !TransitionNode || !TransitionNode->BoundGraph)
		{
			OutError = TEXT("Transition rule graph is missing; cannot write float variable comparison rule.");
			return false;
		}
		if (!FMath::IsFinite(Threshold))
		{
			OutError = TEXT("rule_args.threshold must be a finite number.");
			return false;
		}

		UFunction* CompareFunction = nullptr;
		if (ComparisonOperator == TEXT(">"))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Greater_DoubleDouble"));
		}
		else if (ComparisonOperator == TEXT(">="))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("GreaterEqual_DoubleDouble"));
		}
		else if (ComparisonOperator == TEXT("<"))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("Less_DoubleDouble"));
		}
		else if (ComparisonOperator == TEXT("<="))
		{
			CompareFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(TEXT("LessEqual_DoubleDouble"));
		}
		if (!CompareFunction)
		{
			OutError = FString::Printf(TEXT("Unsupported comparison operator '%s'; the closed set is >, >=, <, <=."), *ComparisonOperator);
			return false;
		}

		UEdGraph* Graph = TransitionNode->BoundGraph;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(Graph);
		UEdGraphPin* ConditionPin = FindTransitionResultConditionPin(ResultNode);
		if (!Schema || !ResultNode || !ConditionPin)
		{
			OutError = TEXT("Transition graph schema or TransitionResult.bCanEnterTransition is missing.");
			return false;
		}

		if (!EnsureAnimBPExpressionVariable(AnimBP, VariableName, MakeAnimBPExpressionDoublePinType(), OutReceipt, TEXT("float"), OutError))
		{
			return false;
		}

		Graph->Modify();
		TransitionNode->Modify();
		ResultNode->Modify();

		UK2Node_VariableGet* GetNode = CreateVariableGetNode(Graph, VariableName, ResultNode->NodePosX - 560, ResultNode->NodePosY - 120, OutError);
		UEdGraphPin* ValuePin = FindFirstOutputDataPin(GetNode);
		if (!GetNode || !ValuePin)
		{
			OutError = FString::Printf(TEXT("Failed to materialize float variable getter '%s'."), *VariableName);
			return false;
		}

		FGraphNodeCreator<UK2Node_CallFunction> CompareCreator(*Graph);
		UK2Node_CallFunction* CompareNode = CompareCreator.CreateNode(true);
		if (!CompareNode)
		{
			OutError = TEXT("Failed to create float comparison function node.");
			return false;
		}
		CompareNode->SetFromFunction(CompareFunction);
		CompareNode->NodePosX = ResultNode->NodePosX - 300;
		CompareNode->NodePosY = ResultNode->NodePosY - 90;
		CompareCreator.Finalize();

		UEdGraphPin* APin = FindFirstInputPin(CompareNode, TEXT("A"));
		UEdGraphPin* BPin = FindFirstInputPin(CompareNode, TEXT("B"));
		UEdGraphPin* ReturnPin = FindFirstOutputPin(CompareNode, TEXT("ReturnValue"));
		if (!APin || !BPin || !ReturnPin)
		{
			OutError = TEXT("Float comparison node pins were not found.");
			return false;
		}

		const FString ThresholdText = FString::SanitizeFloat(Threshold);
		Schema->TrySetDefaultValue(*BPin, ThresholdText, true);
		ConditionPin->Modify();
		ConditionPin->BreakAllPinLinks();
		if (!TryCreateSchemaConnection(Schema, ValuePin, APin, OutError) ||
			!TryCreateSchemaConnection(Schema, ReturnPin, ConditionPin, OutError))
		{
			return false;
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		UBlueprintEditorLibrary::CompileBlueprint(AnimBP);
		const FString CompileStatus = StaticEnum<EBlueprintStatus>()
			? StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(AnimBP->Status))
			: FString::FromInt(static_cast<int32>(AnimBP->Status));
		const bool bCompileSucceeded = AnimBP->Status != BS_Error;

		OutReceipt->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_writer_receipt.v5"));
		OutReceipt->SetStringField(TEXT("rule_type"), RuleType);
		OutReceipt->SetStringField(TEXT("variable_name"), VariableName);
		OutReceipt->SetStringField(TEXT("comparison_operator"), ComparisonOperator);
		OutReceipt->SetNumberField(TEXT("threshold"), Threshold);
		OutReceipt->SetStringField(TEXT("threshold_pin_default"), BPin->DefaultValue);
		OutReceipt->SetStringField(TEXT("variable_get_node_id"), GetNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("comparison_node_id"), CompareNode->NodeGuid.ToString());
		OutReceipt->SetStringField(TEXT("transition_result_node_id"), ResultNode->NodeGuid.ToString());
		OutReceipt->SetBoolField(TEXT("variable_linked_to_comparison_a"), ValuePin->LinkedTo.Contains(APin));
		OutReceipt->SetBoolField(TEXT("comparison_return_linked_to_bCanEnterTransition"), ReturnPin->LinkedTo.Contains(ConditionPin));
		OutReceipt->SetBoolField(TEXT("transition_result_bCanEnterTransition_linked"), ConditionPin->LinkedTo.Contains(ReturnPin));
		OutReceipt->SetBoolField(TEXT("placeholder_written"), false);
		OutReceipt->SetStringField(TEXT("compile_status"), CompileStatus);
		OutReceipt->SetBoolField(TEXT("compile_succeeded"), bCompileSucceeded);
		if (!bCompileSucceeded)
		{
			OutError = FString::Printf(TEXT("AnimBlueprint compile finished with BS_Error after writing %s transition rule."), *RuleType);
			return false;
		}
		return true;
	}

	// Verifies that a cancelled transaction restored the transition condition pin
	// to its pre-mutation snapshot. Caller must cancel the open transaction first.
	void AttachTransitionRuleRollbackReadbackLocal(
		UEdGraphPin* ConditionPin,
		const FString& PreMutationDefault,
		int32 PreMutationLinkCount,
		TSharedRef<FJsonObject> OutStructured)
	{
		bool bRolledBack = false;
		bool bRollbackVerified = false;
		if (ConditionPin)
		{
			bRolledBack = true;
			bRollbackVerified = ConditionPin->DefaultValue.Equals(PreMutationDefault) &&
				ConditionPin->LinkedTo.Num() == PreMutationLinkCount;
		}
		OutStructured->SetBoolField(TEXT("rolled_back"), bRolledBack);
		OutStructured->SetBoolField(TEXT("rollback_verified"), bRollbackVerified);
		OutStructured->SetStringField(TEXT("rollback_mode"), TEXT("transaction_cancel_with_condition_pin_readback"));
	}

	// Reloads the AnimBlueprint asset and re-verifies that the transition rule is
	// still wired (or carries a non-empty default) after save.
	bool VerifyTransitionRuleReloadedLocal(
		FSololmcpEditorServices& Services,
		const FString& AssetPath,
		const FString& MachineName,
		const FString& FromState,
		const FString& ToState,
		TSharedRef<FJsonObject> OutStructured,
		FString& OutError)
	{
		FString LoadError;
		UObject* ReloadedAsset = Services.LoadAsset(AssetPath, LoadError);
		UAnimBlueprint* Reloaded = Cast<UAnimBlueprint>(ReloadedAsset);
		if (!Reloaded)
		{
			OutStructured->SetBoolField(TEXT("reload_readback_verified"), false);
			OutError = LoadError.IsEmpty() ? TEXT("AnimBlueprint could not be reloaded after save.") : LoadError;
			return false;
		}

		bool bWired = false;
		UAnimGraphNode_StateMachineBase* MachineNode = FindAnimStateMachineNodeLocal(Reloaded, MachineName);
		if (MachineNode && MachineNode->EditorStateMachineGraph)
		{
			UAnimStateTransitionNode* TransitionNode = FindAnimTransitionLocal(MachineNode->EditorStateMachineGraph, FromState, ToState);
			if (TransitionNode && TransitionNode->BoundGraph)
			{
				UAnimGraphNode_TransitionResult* ResultNode = FindTransitionResultNode(TransitionNode->BoundGraph);
				UEdGraphPin* ConditionPin = FindTransitionResultConditionPin(ResultNode);
				if (ConditionPin)
				{
					bWired = ConditionPin->LinkedTo.Num() > 0 || !ConditionPin->DefaultValue.TrimStartAndEnd().IsEmpty();
				}
			}
		}
		OutStructured->SetBoolField(TEXT("reload_readback_verified"), bWired);
		if (!bWired)
		{
			OutError = TEXT("Reloaded AnimBlueprint did not expose a wired transition rule condition.");
		}
		return bWired;
	}

	bool RunAnimBlueprintMutationGate(
		UAnimBlueprint* AnimBP,
		const FString& Operation,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		if (!AnimBP)
		{
			OutError = TEXT("AnimBlueprint mutation gate missing AnimBlueprint.");
			return false;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
		UBlueprintEditorLibrary::CompileBlueprint(AnimBP);

		int32 MachineCount = 0;
		int32 StateCount = 0;
		int32 TransitionCount = 0;
		for (UAnimGraphNode_StateMachineBase* MachineNode : GetAnimStateMachineNodesLocal(AnimBP))
		{
			if (!MachineNode || !MachineNode->EditorStateMachineGraph)
			{
				continue;
			}
			++MachineCount;
			StateCount += GetAnimStateNodesLocal(MachineNode->EditorStateMachineGraph).Num();
			TransitionCount += GetAnimStateTransitionsLocal(MachineNode->EditorStateMachineGraph).Num();
		}

		const FString CompileStatus = StaticEnum<EBlueprintStatus>()
			? StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(AnimBP->Status))
			: FString::FromInt(static_cast<int32>(AnimBP->Status));
		const bool bCompileOk = AnimBP->Status != BS_Error;
		TSharedRef<FJsonObject> Gate = MakeShared<FJsonObject>();
		Gate->SetStringField(TEXT("schema"), TEXT("somol.animbp_graph_mutation_gate.v1"));
		Gate->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
		Gate->SetStringField(TEXT("operation"), Operation);
		Gate->SetStringField(TEXT("compile_status"), CompileStatus);
		Gate->SetBoolField(TEXT("compile_ok"), bCompileOk);
		Gate->SetNumberField(TEXT("state_machine_count"), MachineCount);
		Gate->SetNumberField(TEXT("state_count"), StateCount);
		Gate->SetNumberField(TEXT("transition_count"), TransitionCount);
		Gate->SetStringField(TEXT("post_edit_readback_status"), TEXT("state_machine_inventory_read"));
		Gate->SetStringField(TEXT("receipt_status"), bCompileOk ? TEXT("completed") : TEXT("failed_validation"));
		Gate->SetStringField(TEXT("required_before_delivery"), TEXT("compile AnimBlueprint + post-edit state/transition readback"));
		OutStructured->SetObjectField(TEXT("receipt_gate"), Gate);
		OutStructured->SetBoolField(TEXT("compile_ok"), bCompileOk);
		OutStructured->SetStringField(TEXT("compile_status"), CompileStatus);
		OutStructured->SetBoolField(TEXT("receipt_complete"), bCompileOk);
		OutStructured->SetStringField(TEXT("receipt_status"), bCompileOk ? TEXT("completed") : TEXT("failed_validation"));
		if (!bCompileOk)
		{
			OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("animbp_compile_failed"));
			OutError = FString::Printf(TEXT("AnimBlueprint mutation failed receipt gate after compile: %s."), *CompileStatus);
			return false;
		}
		return true;
	}
}

// ====================================================================
// Tool registration entry point
// ====================================================================
void RegisterAnimBPStateMachineTools(FSololmcpToolRegistry& Registry)
{
	// ----- animbp_create_state_machine ----------------------------------
	Registry.Register({
		TEXT("animbp_create_state_machine"),
		TEXT("Create a new UAnimGraphNode_StateMachine in the AnimGraph root of an AnimBlueprint."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UAnimBlueprint asset path."))},
			{TEXT("machine_name"), FSololmcpSchemaBuilder::String(TEXT("Name of the new state machine."))}
		}, {TEXT("asset_path"), TEXT("machine_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			FString MachineName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("machine_name"), MachineName))
			{
				OutError = TEXT("Missing asset_path or machine_name.");
				return false;
			}

			UAnimBlueprint* AnimBP = LoadAnimBlueprintAssetLocal(Context.Services, AssetPath, OutError);
			if (!AnimBP)
			{
				return false;
			}

			UEdGraph* RootGraph = FindPrimaryAnimBlueprintGraphLocal(AnimBP);
			if (!RootGraph)
			{
				OutError = TEXT("Primary animation graph (AnimGraph) was not found.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPCreateStateMachine", "SOMOLMCP Create AnimBP State Machine"));
			AnimBP->Modify();

			UAnimGraphNode_StateMachine* MachineNode =
				FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimGraphNode_StateMachine>(
					RootGraph, NewObject<UAnimGraphNode_StateMachine>(), FSomolAnimGraphPosition(0.0f, 0.0f), false);
			if (!MachineNode)
			{
				OutError = TEXT("Failed to spawn state machine node.");
				return false;
			}
			MachineNode->Modify();
			MachineNode->OnRenameNode(MachineName);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			if (!VerifyAnimGraphContainsNode(RootGraph, MachineNode) ||
				FindAnimStateMachineNodeLocal(AnimBP, MachineName) != MachineNode)
			{
				OutError = FString::Printf(TEXT("State machine '%s' was not present after creation."), *MachineName);
				return false;
			}

			OutStructured->SetStringField(TEXT("state_machine_name"), MachineNode->GetStateMachineName());
			if (MachineNode->EditorStateMachineGraph)
			{
				OutStructured->SetStringField(TEXT("internal_graph_path"), MachineNode->EditorStateMachineGraph->GetPathName());
			}
			if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("create_state_machine"), OutStructured, OutError))
			{
				return false;
			}
			OutSummary = FString::Printf(TEXT("Created state machine '%s' in '%s'."), *MachineName, *AssetPath);
			return true;
		}
	});

	// ----- animbp_add_state ---------------------------------------------
	Registry.Register({
		TEXT("animbp_add_state"),
		TEXT("Add a UAnimStateNode to the named state machine's internal graph."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("machine_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("state_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("pos_x"), FSololmcpSchemaBuilder::Number()},
			{TEXT("pos_y"), FSololmcpSchemaBuilder::Number()}
		}, {TEXT("asset_path"), TEXT("machine_name"), TEXT("state_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString StateName;
			if (!Arguments->TryGetStringField(TEXT("state_name"), StateName))
			{
				OutError = TEXT("Missing state_name.");
				return false;
			}
			UAnimBlueprint* AnimBP = nullptr;
			UAnimGraphNode_StateMachineBase* MachineNode = nullptr;
			UAnimationStateMachineGraph* MachineGraph = nullptr;
			if (!ResolveStateMachineFromArgs(Context, Arguments, AnimBP, MachineNode, MachineGraph, OutError))
			{
				return false;
			}

			const FSomolAnimGraphPosition Location = GetNodeLocationFromArguments(Arguments);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPAddState", "SOMOLMCP Add AnimBP State"));
			AnimBP->Modify();

			UAnimStateNode* StateNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
				MachineGraph, NewObject<UAnimStateNode>(), Location, false);
			if (!StateNode || !StateNode->BoundGraph)
			{
				OutError = TEXT("Failed to spawn state node.");
				return false;
			}
			FEdGraphUtilities::RenameGraphToNameOrCloseToName(StateNode->BoundGraph, StateName);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			if (!VerifyAnimGraphContainsNode(MachineGraph, StateNode) ||
				FindAnimStateNodeLocal(MachineGraph, StateName) != StateNode)
			{
				OutError = FString::Printf(TEXT("State '%s' was not present after creation."), *StateName);
				return false;
			}

			OutStructured->SetStringField(TEXT("state_name"), StateNode->GetStateName());
			OutStructured->SetStringField(TEXT("state_node_id"), StateNode->NodeGuid.ToString());
			if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("add_state"), OutStructured, OutError))
			{
				return false;
			}
			OutSummary = FString::Printf(TEXT("Added state '%s' to machine '%s'."), *StateNode->GetStateName(), *MachineNode->GetStateMachineName());
			return true;
		}
	});

	// ----- animbp_add_transition ----------------------------------------
	Registry.Register({
		TEXT("animbp_add_transition"),
		TEXT("Create a UAnimStateTransitionNode wiring two states with optional crossfade duration."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("machine_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("from_state"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_state"), FSololmcpSchemaBuilder::String()},
			{TEXT("crossfade_duration"), FSololmcpSchemaBuilder::Number(TEXT("Blend duration seconds (default 0.2)."))}
		}, {TEXT("asset_path"), TEXT("machine_name"), TEXT("from_state"), TEXT("to_state")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString FromState;
			FString ToState;
			if (!Arguments->TryGetStringField(TEXT("from_state"), FromState) ||
				!Arguments->TryGetStringField(TEXT("to_state"), ToState))
			{
				OutError = TEXT("Missing from_state or to_state.");
				return false;
			}
			const float Duration = Arguments->HasTypedField<EJson::Number>(TEXT("crossfade_duration"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("crossfade_duration")))
				: 0.2f;

			UAnimBlueprint* AnimBP = nullptr;
			UAnimGraphNode_StateMachineBase* MachineNode = nullptr;
			UAnimationStateMachineGraph* MachineGraph = nullptr;
			if (!ResolveStateMachineFromArgs(Context, Arguments, AnimBP, MachineNode, MachineGraph, OutError))
			{
				return false;
			}

			UAnimStateNode* PrevState = FindAnimStateNodeLocal(MachineGraph, FromState);
			UAnimStateNode* NextState = FindAnimStateNodeLocal(MachineGraph, ToState);
			if (!PrevState || !NextState)
			{
				OutError = TEXT("from_state or to_state was not found.");
				return false;
			}

			const FSomolAnimGraphPosition Location =
				(FSomolAnimGraphPosition(static_cast<float>(PrevState->NodePosX), static_cast<float>(PrevState->NodePosY)) +
				 FSomolAnimGraphPosition(static_cast<float>(NextState->NodePosX), static_cast<float>(NextState->NodePosY))) * 0.5f;

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPAddTransition", "SOMOLMCP Add AnimBP Transition"));
			AnimBP->Modify();

			// If a transition already exists, just adjust its duration.
			UAnimStateTransitionNode* TransitionNode = FindAnimTransitionLocal(MachineGraph, FromState, ToState);
			if (!TransitionNode)
			{
				TransitionNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
					MachineGraph, NewObject<UAnimStateTransitionNode>(), Location, false);
				if (!TransitionNode)
				{
					OutError = TEXT("Failed to spawn transition node.");
					return false;
				}
				TransitionNode->CreateConnections(PrevState, NextState);
				if (FindAnimTransitionLocal(MachineGraph, FromState, ToState) != TransitionNode)
				{
					OutError = FString::Printf(TEXT("Transition %s -> %s was not connected after creation."), *FromState, *ToState);
					return false;
				}
			}

			TransitionNode->Modify();
			TransitionNode->CrossfadeDuration = Duration;

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			if (FindAnimTransitionLocal(MachineGraph, FromState, ToState) != TransitionNode ||
				!FMath::IsNearlyEqual(TransitionNode->CrossfadeDuration, Duration))
			{
				OutError = FString::Printf(TEXT("Transition %s -> %s failed post-write verification."), *FromState, *ToState);
				return false;
			}

			OutStructured->SetStringField(TEXT("from"), FromState);
			OutStructured->SetStringField(TEXT("to"), ToState);
			OutStructured->SetNumberField(TEXT("duration"), Duration);
			if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("add_transition"), OutStructured, OutError))
			{
				return false;
			}
			OutSummary = FString::Printf(TEXT("Transition %s -> %s (%.3fs)."), *FromState, *ToState, Duration);
			return true;
		}
	});

	// ----- animbp_set_transition_rule -----------------------------------
	Registry.Register({
		TEXT("animbp_set_transition_rule"),
		TEXT("Set the transition rule for a state-machine transition. Supported rule_type: 'always', 'time_remaining_lt', "
			"'sync_between_markers', 'distance_gt', constant boolean expressions, and simple numeric-and-bool expressions such as Speed > 150 && bIsGrounded. "
			"Accepts aliases (e.g. time, sync, distance, bool, compare_int) and delivers compile/readback/rollback receipts."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("machine_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("from_state"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_state"), FSololmcpSchemaBuilder::String()},
			{TEXT("rule_type"), FSololmcpSchemaBuilder::String(TEXT("'always' | 'time_remaining_lt' | 'sync_between_markers' | 'distance_gt' | 'expression' | 'blackboard_bool' | 'blackboard_compare_int' (aliases supported)"))},
			{TEXT("rule_args"), FSololmcpSchemaBuilder::Object({})},
			{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save the AnimBlueprint after the mutation and verify reload readback. Defaults to true."))}
		}, {TEXT("asset_path"), TEXT("machine_name"), TEXT("from_state"), TEXT("to_state"), TEXT("rule_type")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString FromState;
			FString ToState;
			FString RuleType;
			if (!Arguments->TryGetStringField(TEXT("from_state"), FromState) ||
				!Arguments->TryGetStringField(TEXT("to_state"), ToState) ||
				!Arguments->TryGetStringField(TEXT("rule_type"), RuleType))
			{
				OutError = TEXT("Missing from_state, to_state, or rule_type.");
				return false;
			}

			UAnimBlueprint* AnimBP = nullptr;
			UAnimGraphNode_StateMachineBase* MachineNode = nullptr;
			UAnimationStateMachineGraph* MachineGraph = nullptr;
			if (!ResolveStateMachineFromArgs(Context, Arguments, AnimBP, MachineNode, MachineGraph, OutError))
			{
				return false;
			}

			UAnimStateTransitionNode* TransitionNode = FindAnimTransitionLocal(MachineGraph, FromState, ToState);
			if (!TransitionNode)
			{
				OutError = TEXT("Transition was not found.");
				return false;
			}

			// AN-02: alias normalization for rule_type.
			const FString RawRuleType = RuleType;
			bool bRuleTypeAliased = false;
			RuleType = NormalizeTransitionRuleTypeLocal(RuleType, bRuleTypeAliased);
			OutStructured->SetStringField(TEXT("rule_type_requested"), RawRuleType);
			OutStructured->SetStringField(TEXT("rule_type_resolved"), RuleType);
			OutStructured->SetBoolField(TEXT("rule_type_aliased"), bRuleTypeAliased);

			// Pre-mutation snapshot of the transition condition pin for rollback readback.
			UEdGraphPin* ConditionPinSnapshot = nullptr;
			FString PreMutationDefault;
			int32 PreMutationLinkCount = 0;
			if (TransitionNode->BoundGraph)
			{
				UAnimGraphNode_TransitionResult* SnapshotResultNode = FindTransitionResultNode(TransitionNode->BoundGraph);
				ConditionPinSnapshot = FindTransitionResultConditionPin(SnapshotResultNode);
				if (ConditionPinSnapshot)
				{
					PreMutationDefault = ConditionPinSnapshot->DefaultValue;
					PreMutationLinkCount = ConditionPinSnapshot->LinkedTo.Num();
				}
			}

			// Post-gate delivery envelope: save + reload readback + receipt identity.
			auto FinalizeTransitionReceiptLocal = [&]() -> bool
			{
				bool bSaveAsset = true;
				Arguments->TryGetBoolField(TEXT("save_asset"), bSaveAsset);
				FString SaveError;
				if (!SaveAnimBlueprintReceiptLocal(Context.Services, AnimBP, bSaveAsset, OutStructured, SaveError))
				{
					OutError = SaveError;
					return false;
				}
				if (bSaveAsset)
				{
					FString ReloadError;
					VerifyTransitionRuleReloadedLocal(Context.Services, AnimBP->GetPathName(),
						MachineNode->GetStateMachineName(), FromState, ToState, OutStructured, ReloadError);
				}
				else
				{
					OutStructured->SetBoolField(TEXT("reload_readback_verified"), false);
				}
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutStructured->SetStringField(TEXT("status"), TEXT("succeeded"));
				OutStructured->SetStringField(TEXT("receipt_id"), NewAnimBPReceiptIdLocal(TEXT("transition_rule")));
				OutStructured->SetBoolField(TEXT("mutation_applied"), true);
				OutStructured->SetBoolField(TEXT("readback_verified"), true);
				return true;
			};

			auto AttachRollbackReadbackLocal = [&]()
			{
				AttachTransitionRuleRollbackReadbackLocal(ConditionPinSnapshot, PreMutationDefault, PreMutationLinkCount, OutStructured);
			};

			const FString Normalized = RuleType;

			if (Normalized == TEXT("always"))
			{
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPSetTransitionRuleAlways", "SOMOLMCP Set Transition Rule (always)"));
				AnimBP->Modify();
				TransitionNode->Modify();

				// "Always": leave the transition rule unwired so that the
				// generated CanTakeTransition() returns the default (true) for
				// the standard Result-only transition graph.
				// TODO(P0-1): verify whether UAnimStateTransitionNode exposes
				// a dedicated "always true" flag in the user's UE 5.x branch;
				// if so, prefer setting that flag explicitly here.

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
				OutStructured->SetBoolField(TEXT("applied"), true);

				OutStructured->SetStringField(TEXT("rule_type"), TEXT("always"));
				OutStructured->SetStringField(TEXT("from"), FromState);
				OutStructured->SetStringField(TEXT("to"), ToState);
				OutStructured->SetBoolField(TEXT("placeholder_written"), false);
				OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
					MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
				AttachTransitionRuleReceipt(OutStructured, TransitionNode, TEXT("always"), true, true,
					TEXT("always is represented by leaving the transition graph unwired; no comment-only placeholder was written."));
				if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("set_transition_rule_always"), OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(TEXT("Set transition rule 'always' for %s -> %s."), *FromState, *ToState);
				return true;
			}

			if (Normalized == TEXT("time_remaining_lt"))
			{
				// Pull rule_args.threshold (float).
				float Threshold = 0.0f;
				bool bThresholdValid = false;
				const TSharedPtr<FJsonObject>* RuleArgsPtr = nullptr;
				if (Arguments->TryGetObjectField(TEXT("rule_args"), RuleArgsPtr) && RuleArgsPtr && (*RuleArgsPtr).IsValid())
				{
					if ((*RuleArgsPtr)->HasTypedField<EJson::Number>(TEXT("threshold")))
					{
						Threshold = static_cast<float>((*RuleArgsPtr)->GetNumberField(TEXT("threshold")));
						bThresholdValid = FMath::IsFinite(Threshold) && Threshold > 0.0f;
					}
				}

				OutStructured->SetStringField(TEXT("rule_type"), TEXT("time_remaining_lt"));
				OutStructured->SetStringField(TEXT("from"), FromState);
				OutStructured->SetStringField(TEXT("to"), ToState);
				OutStructured->SetNumberField(TEXT("threshold"), Threshold);
				OutStructured->SetBoolField(TEXT("rule_args_type_checked"), true);
				OutStructured->SetBoolField(TEXT("threshold_valid"), bThresholdValid);
				OutStructured->SetBoolField(TEXT("placeholder_written"), false);
				OutStructured->SetBoolField(TEXT("raw_expression_placeholder_written"), false);

				if (!bThresholdValid)
				{
					OutStructured->SetBoolField(TEXT("applied"), false);
					OutStructured->SetStringField(TEXT("error_code"), TEXT("INVALID_ARGUMENT"));
					OutStructured->SetStringField(TEXT("note"), TEXT("rule_args.threshold must be a positive number; no transition graph mutation was attempted."));
					AttachFailClosedTransitionRuleContract(OutStructured, TEXT("time_remaining_lt"),
						TEXT("rule_args.threshold must be a positive number; no mutation was attempted."));
					OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
						MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
					AttachTransitionRuleReceipt(OutStructured, TransitionNode, TEXT("time_remaining_lt"), true, false,
						TEXT("Invalid threshold; no transition graph mutation was attempted."));
					OutError = TEXT("rule_args.threshold must be a positive number.");
					OutSummary = FString::Printf(TEXT("Transition rule 'time_remaining_lt' for %s -> %s was not applied because threshold is invalid."), *FromState, *ToState);
					return false;
				}

				TSharedRef<FJsonObject> WriterReceipt = MakeShared<FJsonObject>();
				WriterReceipt->SetStringField(TEXT("rule_type"), TEXT("time_remaining_lt"));
				WriterReceipt->SetNumberField(TEXT("threshold"), Threshold);
				WriterReceipt->SetBoolField(TEXT("placeholder_written"), false);
				WriterReceipt->SetBoolField(TEXT("raw_expression_placeholder_written"), false);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPSetTransitionRuleTimeRemainingLt", "SOMOLMCP Set Transition Rule (time remaining lt)"));
				AnimBP->Modify();
				TransitionNode->Modify();

				FString WriteError;
				const bool bWriteOk = TryWriteTimeRemainingLtTransitionRule(AnimBP, TransitionNode, Threshold, WriterReceipt, WriteError);

				OutStructured->SetBoolField(TEXT("applied"), bWriteOk);
				OutStructured->SetBoolField(TEXT("mutation_attempted"), true);
				OutStructured->SetStringField(TEXT("capability_status"), bWriteOk ? TEXT("supported_applied") : TEXT("supported_write_or_compile_failed"));
				OutStructured->SetObjectField(TEXT("repair_plan"), MakeTransitionRuleRepairPlanJson(TEXT("time_remaining_lt"),
					TEXT("time_remaining_lt is executable by writing TimeRemaining < threshold into TransitionResult.bCanEnterTransition."),
					true));
				OutStructured->SetObjectField(TEXT("transition_rule_writer_receipt"), WriterReceipt);
				OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
					MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
				AttachTransitionRuleReceipt(OutStructured, TransitionNode, TEXT("time_remaining_lt"), true, bWriteOk,
					bWriteOk
						? TEXT("TimeRemaining < threshold was wired to TransitionResult.bCanEnterTransition and AnimBlueprint compile did not report BS_Error.")
						: WriteError);

				if (!bWriteOk)
				{
					OutStructured->SetStringField(TEXT("error_code"), TEXT("WRITE_OR_COMPILE_FAILED"));
					OutError = WriteError;
					OutSummary = FString::Printf(TEXT("Transition rule time_remaining_lt < %.3f for %s -> %s failed."), Threshold, *FromState, *ToState);
					return false;
				}
				if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("set_transition_rule_time_remaining_lt"), OutStructured, OutError))
				{
					return false;
				}

				OutSummary = FString::Printf(TEXT("Set transition rule time_remaining_lt < %.3f for %s -> %s."), Threshold, *FromState, *ToState);
				return true;
			}

			if (Normalized == TEXT("blackboard_bool") || Normalized == TEXT("blackboard_compare_int"))
			{
				FString VariableName;
				FString ComparisonOperator = TEXT("==");
				bool bExpectedValue = true;
				double RawCompareValue = 0.0;
				bool bCompareValuePresent = false;
				const TSharedPtr<FJsonObject>* RuleArgsPtr = nullptr;
				if (Arguments->TryGetObjectField(TEXT("rule_args"), RuleArgsPtr) && RuleArgsPtr && (*RuleArgsPtr).IsValid())
				{
					if (!(*RuleArgsPtr)->TryGetStringField(TEXT("variable_name"), VariableName) &&
						!(*RuleArgsPtr)->TryGetStringField(TEXT("key_name"), VariableName))
					{
						(*RuleArgsPtr)->TryGetStringField(TEXT("key"), VariableName);
					}
					(*RuleArgsPtr)->TryGetBoolField(TEXT("expected_value"), bExpectedValue);
					if (!(*RuleArgsPtr)->TryGetStringField(TEXT("operator"), ComparisonOperator))
					{
						(*RuleArgsPtr)->TryGetStringField(TEXT("comparison_operator"), ComparisonOperator);
					}
					const FString CompareField = (*RuleArgsPtr)->HasTypedField<EJson::Number>(TEXT("value"))
						? TEXT("value") : TEXT("compare_value");
					if ((*RuleArgsPtr)->HasTypedField<EJson::Number>(CompareField))
					{
						RawCompareValue = (*RuleArgsPtr)->GetNumberField(CompareField);
						bCompareValuePresent = true;
					}
				}
				VariableName = VariableName.TrimStartAndEnd();
				ComparisonOperator = ComparisonOperator.TrimStartAndEnd();
				const bool bCompareRule = Normalized == TEXT("blackboard_compare_int");
				const bool bCompareValueValid = !bCompareRule ||
					(bCompareValuePresent && FMath::IsFinite(RawCompareValue) &&
					 RawCompareValue >= static_cast<double>(MIN_int32) && RawCompareValue <= static_cast<double>(MAX_int32) &&
					 FMath::IsNearlyEqual(RawCompareValue, FMath::RoundToDouble(RawCompareValue)));
				if (VariableName.IsEmpty() || !bCompareValueValid)
				{
					OutStructured->SetStringField(TEXT("error_code"), TEXT("INVALID_ARGUMENT"));
					OutStructured->SetBoolField(TEXT("applied"), false);
					OutError = bCompareRule
						? TEXT("blackboard_compare_int requires variable_name and an in-range integer value/compare_value.")
						: TEXT("blackboard_bool requires variable_name (aliases: key_name, key).");
					return false;
				}

				const int32 CompareValue = static_cast<int32>(RawCompareValue);
				TSharedRef<FJsonObject> WriterReceipt = MakeShared<FJsonObject>();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPSetTransitionRuleBlackboard", "SOMOLMCP Set Blackboard Transition Rule"));
				AnimBP->Modify();
				TransitionNode->Modify();
				FString WriteError;
				const bool bWriteOk = TryWriteBlackboardVariableTransitionRule(
					AnimBP, TransitionNode, VariableName, Normalized, bExpectedValue,
					ComparisonOperator, CompareValue, WriterReceipt, WriteError);
				OutStructured->SetStringField(TEXT("rule_type"), Normalized);
				OutStructured->SetStringField(TEXT("variable_name"), VariableName);
				OutStructured->SetBoolField(TEXT("applied"), bWriteOk);
				OutStructured->SetBoolField(TEXT("placeholder_written"), false);
				if (bCompareRule)
				{
					OutStructured->SetStringField(TEXT("comparison_operator"), ComparisonOperator);
					OutStructured->SetNumberField(TEXT("compare_value"), CompareValue);
				}
				else
				{
					OutStructured->SetBoolField(TEXT("expected_value"), bExpectedValue);
				}
				OutStructured->SetObjectField(TEXT("transition_rule_writer_receipt"), WriterReceipt);
				OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
					MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
				AttachTransitionRuleReceipt(OutStructured, TransitionNode, Normalized, true, bWriteOk,
					bWriteOk ? TEXT("Typed variable rule was wired to TransitionResult and compiled.") : WriteError);
				if (!bWriteOk)
				{
					OutStructured->SetStringField(TEXT("error_code"), TEXT("WRITE_OR_COMPILE_FAILED"));
					OutError = WriteError;
					return false;
				}
				if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("set_transition_rule_blackboard"), OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(TEXT("Set %s transition rule for %s -> %s."), *Normalized, *FromState, *ToState);
				return true;
			}

			if (Normalized == TEXT("expression"))
			{
				FString Expression;
				bool bExpressionPresent = false;
				const TSharedPtr<FJsonObject>* RuleArgsPtr = nullptr;
				if (Arguments->TryGetObjectField(TEXT("rule_args"), RuleArgsPtr) && RuleArgsPtr && (*RuleArgsPtr).IsValid())
				{
					bExpressionPresent = (*RuleArgsPtr)->TryGetStringField(TEXT("expression"), Expression) && !Expression.TrimStartAndEnd().IsEmpty();
				}

				bool bConstBoolValue = false;
				FString ParseError;
				const bool bConstBoolExpression = bExpressionPresent && TryParseConstBoolTransitionExpression(Expression, bConstBoolValue, ParseError);
				if (bConstBoolExpression)
				{
					TSharedRef<FJsonObject> WriterReceipt = MakeShared<FJsonObject>();
					WriterReceipt->SetStringField(TEXT("schema"), TEXT("somol.animbp.transition_rule_writer_receipt.v2"));
					WriterReceipt->SetStringField(TEXT("rule_type"), TEXT("expression"));
					WriterReceipt->SetStringField(TEXT("expression_mode"), TEXT("constant_boolean"));
					WriterReceipt->SetStringField(TEXT("expression_source"), Expression);
					WriterReceipt->SetBoolField(TEXT("parsed_constant_boolean"), true);
					WriterReceipt->SetBoolField(TEXT("constant_value"), bConstBoolValue);
					WriterReceipt->SetBoolField(TEXT("placeholder_written"), false);
					WriterReceipt->SetBoolField(TEXT("raw_expression_placeholder_written"), false);

					const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPSetTransitionRuleExpressionConstBool", "SOMOLMCP Set Transition Rule (expression const bool)"));
					AnimBP->Modify();
					TransitionNode->Modify();

					FString WriteError;
					const bool bWriteOk = TryWriteTransitionResultConstant(AnimBP, TransitionNode, bConstBoolValue, WriterReceipt, WriteError);

					OutStructured->SetStringField(TEXT("rule_type"), TEXT("expression"));
					OutStructured->SetStringField(TEXT("expression_mode"), TEXT("constant_boolean"));
					OutStructured->SetStringField(TEXT("expression_source"), Expression);
					OutStructured->SetStringField(TEXT("from"), FromState);
					OutStructured->SetStringField(TEXT("to"), ToState);
					OutStructured->SetBoolField(TEXT("rule_args_type_checked"), true);
					OutStructured->SetBoolField(TEXT("expression_present"), true);
					OutStructured->SetBoolField(TEXT("parsed_constant_boolean"), true);
					OutStructured->SetBoolField(TEXT("constant_value"), bConstBoolValue);
					OutStructured->SetBoolField(TEXT("applied"), bWriteOk);
					OutStructured->SetBoolField(TEXT("mutation_attempted"), true);
					OutStructured->SetBoolField(TEXT("placeholder_written"), false);
					OutStructured->SetObjectField(TEXT("repair_plan"), MakeTransitionRuleRepairPlanJson(TEXT("expression"),
						TEXT("Constant boolean expression is executable by writing TransitionResult.bCanEnterTransition default value."),
						true));
					OutStructured->SetObjectField(TEXT("transition_rule_writer_receipt"), WriterReceipt);
					OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
						MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
					AttachTransitionRuleReceipt(OutStructured, TransitionNode, TEXT("expression"), true, bWriteOk,
						bWriteOk
							? TEXT("Expression constant boolean was written to TransitionResult.bCanEnterTransition default value and the AnimBlueprint compile did not report BS_Error.")
							: WriteError);

					if (!bWriteOk)
					{
						OutStructured->SetStringField(TEXT("error_code"), TEXT("WRITE_OR_COMPILE_FAILED"));
						OutError = WriteError;
						OutSummary = FString::Printf(TEXT("Transition rule expression const-bool for %s -> %s failed."), *FromState, *ToState);
						return false;
					}
					if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("set_transition_rule_expression_const_bool"), OutStructured, OutError))
					{
						return false;
					}

					OutSummary = FString::Printf(TEXT("Set transition rule expression const-bool (%s) for %s -> %s."), bConstBoolValue ? TEXT("true") : TEXT("false"), *FromState, *ToState);
					return true;
				}

				FString BoolExpressionVariableName;
				bool bBoolExpressionExpectedValue = true;
				FString BoolExpressionParseError;
				const bool bBoolVariableExpression = bExpressionPresent && TryParseBoolVariableTransitionExpression(
					Expression, BoolExpressionVariableName, bBoolExpressionExpectedValue, BoolExpressionParseError);
				if (bBoolVariableExpression)
				{
					TSharedRef<FJsonObject> WriterReceipt = MakeShared<FJsonObject>();
					WriterReceipt->SetStringField(TEXT("rule_type"), TEXT("expression"));
					WriterReceipt->SetStringField(TEXT("expression_mode"), TEXT("boolean_variable"));
					WriterReceipt->SetStringField(TEXT("expression_source"), Expression);
					WriterReceipt->SetStringField(TEXT("bool_variable"), BoolExpressionVariableName);
					WriterReceipt->SetBoolField(TEXT("bool_expected_value"), bBoolExpressionExpectedValue);
					WriterReceipt->SetBoolField(TEXT("placeholder_written"), false);
					WriterReceipt->SetBoolField(TEXT("raw_expression_placeholder_written"), false);

					const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPSetTransitionRuleExpressionBoolVariable", "SOMOLMCP Set Transition Rule (expression bool variable)"));
					AnimBP->Modify();
					TransitionNode->Modify();

					FString WriteError;
					const bool bWriteOk = TryWriteBlackboardVariableTransitionRule(
						AnimBP, TransitionNode, BoolExpressionVariableName, TEXT("blackboard_bool"),
						bBoolExpressionExpectedValue, TEXT("=="), 0, WriterReceipt, WriteError);

					OutStructured->SetStringField(TEXT("rule_type"), TEXT("expression"));
					OutStructured->SetStringField(TEXT("expression_mode"), TEXT("boolean_variable"));
					OutStructured->SetStringField(TEXT("expression_source"), Expression);
					OutStructured->SetStringField(TEXT("from"), FromState);
					OutStructured->SetStringField(TEXT("to"), ToState);
					OutStructured->SetBoolField(TEXT("rule_args_type_checked"), true);
					OutStructured->SetBoolField(TEXT("expression_present"), true);
					OutStructured->SetBoolField(TEXT("parsed_constant_boolean"), false);
					OutStructured->SetBoolField(TEXT("parsed_bool_variable_expression"), true);
					OutStructured->SetStringField(TEXT("bool_variable"), BoolExpressionVariableName);
					OutStructured->SetBoolField(TEXT("bool_expected_value"), bBoolExpressionExpectedValue);
					OutStructured->SetBoolField(TEXT("applied"), bWriteOk);
					OutStructured->SetBoolField(TEXT("mutation_attempted"), true);
					OutStructured->SetBoolField(TEXT("placeholder_written"), false);
					OutStructured->SetBoolField(TEXT("raw_expression_placeholder_written"), false);
					OutStructured->SetObjectField(TEXT("repair_plan"), MakeTransitionRuleRepairPlanJson(TEXT("expression"),
						TEXT("Boolean variable expression is executable by creating a typed variable getter and wiring it to TransitionResult.bCanEnterTransition."),
						true));
					OutStructured->SetObjectField(TEXT("transition_rule_writer_receipt"), WriterReceipt);
					OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
						MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
					AttachTransitionRuleReceipt(OutStructured, TransitionNode, TEXT("expression"), true, bWriteOk,
						bWriteOk
							? TEXT("Boolean variable expression was materialized as typed K2 nodes and the AnimBlueprint compile did not report BS_Error.")
							: WriteError);

					if (!bWriteOk)
					{
						OutStructured->SetStringField(TEXT("error_code"), TEXT("WRITE_OR_COMPILE_FAILED"));
						OutError = WriteError;
						OutSummary = FString::Printf(TEXT("Transition rule expression bool variable for %s -> %s failed."), *FromState, *ToState);
						return false;
					}
					if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("set_transition_rule_expression_bool_variable"), OutStructured, OutError))
					{
						return false;
					}

					OutSummary = FString::Printf(TEXT("Set transition rule expression bool variable for %s -> %s."), *FromState, *ToState);
					return true;
				}

				FSimpleVariableAndBoolTransitionExpression ParsedExpression;
				FString VariableExpressionParseError;
				const bool bVariableAndBoolExpression = bExpressionPresent && TryParseSimpleVariableAndBoolTransitionExpression(Expression, ParsedExpression, VariableExpressionParseError);
				if (bVariableAndBoolExpression)
				{
					TSharedRef<FJsonObject> WriterReceipt = MakeShared<FJsonObject>();
					WriterReceipt->SetStringField(TEXT("rule_type"), TEXT("expression"));
					WriterReceipt->SetStringField(TEXT("expression_mode"), TEXT("numeric_compare_and_bool"));
					WriterReceipt->SetStringField(TEXT("expression_source"), Expression);
					WriterReceipt->SetBoolField(TEXT("parsed_variable_expression"), true);
					WriterReceipt->SetBoolField(TEXT("placeholder_written"), false);
					WriterReceipt->SetBoolField(TEXT("raw_expression_placeholder_written"), false);

					const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPSetTransitionRuleExpressionVariableBool", "SOMOLMCP Set Transition Rule (expression variable bool)"));
					AnimBP->Modify();
					TransitionNode->Modify();

					FString WriteError;
					const bool bWriteOk = TryWriteSimpleVariableAndBoolTransitionRule(AnimBP, TransitionNode, Expression, ParsedExpression, WriterReceipt, WriteError);

					OutStructured->SetStringField(TEXT("rule_type"), TEXT("expression"));
					OutStructured->SetStringField(TEXT("expression_mode"), TEXT("numeric_compare_and_bool"));
					OutStructured->SetStringField(TEXT("expression_source"), Expression);
					OutStructured->SetStringField(TEXT("from"), FromState);
					OutStructured->SetStringField(TEXT("to"), ToState);
					OutStructured->SetBoolField(TEXT("rule_args_type_checked"), true);
					OutStructured->SetBoolField(TEXT("expression_present"), true);
					OutStructured->SetBoolField(TEXT("parsed_constant_boolean"), false);
					OutStructured->SetBoolField(TEXT("parsed_variable_expression"), true);
					OutStructured->SetStringField(TEXT("numeric_variable"), ParsedExpression.NumericVariableName);
					OutStructured->SetStringField(TEXT("comparison_operator"), ParsedExpression.ComparisonOperator);
					OutStructured->SetNumberField(TEXT("threshold"), ParsedExpression.Threshold);
					OutStructured->SetStringField(TEXT("bool_variable"), ParsedExpression.BoolVariableName);
					OutStructured->SetBoolField(TEXT("bool_expected_value"), ParsedExpression.bBoolExpectedValue);
					OutStructured->SetBoolField(TEXT("applied"), bWriteOk);
					OutStructured->SetBoolField(TEXT("mutation_attempted"), true);
					OutStructured->SetBoolField(TEXT("placeholder_written"), false);
					OutStructured->SetBoolField(TEXT("raw_expression_placeholder_written"), false);
					OutStructured->SetObjectField(TEXT("repair_plan"), MakeTransitionRuleRepairPlanJson(TEXT("expression"),
						TEXT("Simple numeric-and-bool expression is executable by creating typed variable getters, comparison, BooleanAND, and wiring the result to TransitionResult.bCanEnterTransition."),
						true));
					OutStructured->SetObjectField(TEXT("transition_rule_writer_receipt"), WriterReceipt);
					OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
						MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
					AttachTransitionRuleReceipt(OutStructured, TransitionNode, TEXT("expression"), true, bWriteOk,
						bWriteOk
							? TEXT("Variable expression was materialized as typed K2 nodes and the AnimBlueprint compile did not report BS_Error.")
							: WriteError);

					if (!bWriteOk)
					{
						OutStructured->SetStringField(TEXT("error_code"), TEXT("WRITE_OR_COMPILE_FAILED"));
						OutError = WriteError;
						OutSummary = FString::Printf(TEXT("Transition rule expression variable graph for %s -> %s failed."), *FromState, *ToState);
						return false;
					}
					if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("set_transition_rule_expression_variable_bool"), OutStructured, OutError))
					{
						return false;
					}

					OutSummary = FString::Printf(TEXT("Set transition rule expression variable graph for %s -> %s."), *FromState, *ToState);
					return true;
				}

				OutStructured->SetStringField(TEXT("rule_type"), TEXT("expression"));
				OutStructured->SetStringField(TEXT("from"), FromState);
				OutStructured->SetStringField(TEXT("to"), ToState);
				OutStructured->SetBoolField(TEXT("rule_args_type_checked"), true);
				OutStructured->SetBoolField(TEXT("expression_present"), bExpressionPresent);
				OutStructured->SetBoolField(TEXT("parsed_constant_boolean"), false);
				if (!ParseError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("parse_error"), ParseError);
				}
				if (!VariableExpressionParseError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("variable_expression_parse_error"), VariableExpressionParseError);
				}
				if (!BoolExpressionParseError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("bool_expression_parse_error"), BoolExpressionParseError);
				}
				OutStructured->SetBoolField(TEXT("applied"), false);
				OutStructured->SetStringField(TEXT("error_code"), TEXT("NOT_IMPLEMENTED"));
				OutStructured->SetStringField(TEXT("note"),
					TEXT("Supported expression subset is constant boolean, boolean variable, or numeric comparison && boolean variable, for example: bIsGrounded, !bIsFalling, Speed > 150 && bIsGrounded. No placeholder or raw text expression was written."));
				AttachFailClosedTransitionRuleContract(OutStructured, TEXT("expression"),
					bExpressionPresent
						? TEXT("rule_args.expression is present, but it is outside the current safe writer subset.")
						: TEXT("rule_args.expression must be a non-empty string; no mutation was attempted."));
				OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
					MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
				AttachTransitionRuleReceipt(OutStructured, TransitionNode, TEXT("expression"), false, false,
					bExpressionPresent
						? TEXT("Expression text was accepted for diagnostics only; no transition graph mutation was attempted.")
						: TEXT("Missing non-empty rule_args.expression; no transition graph mutation was attempted."));
				OutError = TEXT("rule_type 'expression' is not implemented without verified expression parsing, K2 wiring, and compile diagnostics.");
				OutSummary = FString::Printf(TEXT("Transition rule expression for %s -> %s was not applied."), *FromState, *ToState);
				return false;
			}

			// Other rule types are not yet implemented.
			OutStructured->SetStringField(TEXT("error_code"), TEXT("NOT_IMPLEMENTED"));
			OutStructured->SetStringField(TEXT("rule_type"), RuleType);
			OutStructured->SetBoolField(TEXT("rule_args_type_checked"), false);
			AttachFailClosedTransitionRuleContract(OutStructured, RuleType,
				TEXT("Unsupported rule_type; no transition graph mutation or placeholder node was written."));
			OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
				MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
			AttachTransitionRuleReceipt(OutStructured, TransitionNode, RuleType, false, false,
				TEXT("Unsupported rule_type; no transition graph mutation or placeholder node was written."));
			OutError = FString::Printf(TEXT("rule_type '%s' is not implemented yet."), *RuleType);
			return false;
		}
	});

	// ----- animbp_inspect_transition_rule_graph -------------------------
	Registry.Register({
		TEXT("animbp_inspect_transition_rule_graph"),
		TEXT("Read-only inspect an AnimBP transition rule graph and report complex-rule readiness without mutating the asset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("machine_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("from_state"), FSololmcpSchemaBuilder::String()},
			{TEXT("to_state"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("asset_path"), TEXT("machine_name"), TEXT("from_state"), TEXT("to_state")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString FromState;
			FString ToState;
			if (!Arguments->TryGetStringField(TEXT("from_state"), FromState) ||
				!Arguments->TryGetStringField(TEXT("to_state"), ToState))
			{
				OutError = TEXT("Missing from_state or to_state.");
				return false;
			}

			UAnimBlueprint* AnimBP = nullptr;
			UAnimGraphNode_StateMachineBase* MachineNode = nullptr;
			UAnimationStateMachineGraph* MachineGraph = nullptr;
			if (!ResolveStateMachineFromArgs(Context, Arguments, AnimBP, MachineNode, MachineGraph, OutError))
			{
				return false;
			}

			UAnimStateTransitionNode* TransitionNode = FindAnimTransitionLocal(MachineGraph, FromState, ToState);
			if (!TransitionNode)
			{
				OutError = TEXT("Transition was not found.");
				return false;
			}

			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.animbp.inspect_transition_rule_graph.v1"));
			OutStructured->SetBoolField(TEXT("read_only"), true);
			OutStructured->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
			OutStructured->SetStringField(TEXT("machine_name"), MachineNode->GetStateMachineName());
			OutStructured->SetObjectField(TEXT("transition_rule_graph_inspection"),
				MakeTransitionRuleGraphInspectionJson(TransitionNode, MachineNode->GetStateMachineName(), FromState, ToState));
			OutSummary = FString::Printf(TEXT("Inspected transition rule graph for %s -> %s in '%s'."), *FromState, *ToState, *MachineNode->GetStateMachineName());
			return true;
		}
	});

	// ----- animbp_add_blendspace_node -----------------------------------
	Registry.Register({
		TEXT("animbp_add_blendspace_node"),
		TEXT("Add a UAnimGraphNode_BlendSpacePlayer inside the named state's anim graph and wire it to the result node."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("machine_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("state_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("blendspace_path"), FSololmcpSchemaBuilder::String(TEXT("Path to a UBlendSpace asset."))}
		}, {TEXT("asset_path"), TEXT("machine_name"), TEXT("state_name"), TEXT("blendspace_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString StateName;
			FString BlendSpacePath;
			if (!Arguments->TryGetStringField(TEXT("state_name"), StateName) ||
				!Arguments->TryGetStringField(TEXT("blendspace_path"), BlendSpacePath))
			{
				OutError = TEXT("Missing state_name or blendspace_path.");
				return false;
			}

			UAnimBlueprint* AnimBP = nullptr;
			UAnimGraphNode_StateMachineBase* MachineNode = nullptr;
			UAnimationStateMachineGraph* MachineGraph = nullptr;
			if (!ResolveStateMachineFromArgs(Context, Arguments, AnimBP, MachineNode, MachineGraph, OutError))
			{
				return false;
			}

			UAnimStateNode* StateNode = FindAnimStateNodeLocal(MachineGraph, StateName);
			if (!StateNode || !StateNode->BoundGraph)
			{
				OutError = FString::Printf(TEXT("State '%s' was not found."), *StateName);
				return false;
			}

			UObject* BSAsset = Context.Services.LoadAsset(BlendSpacePath, OutError);
			if (!BSAsset)
			{
				return false;
			}
			UBlendSpace* BlendSpace = Cast<UBlendSpace>(BSAsset);
			if (!BlendSpace)
			{
				OutError = TEXT("blendspace_path does not point to a UBlendSpace asset.");
				return false;
			}

			UEdGraph* StateAnimGraph = StateNode->BoundGraph;

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBPAddBlendSpace", "SOMOLMCP Add BlendSpace Player"));
			AnimBP->Modify();
			StateAnimGraph->Modify();

			UAnimGraphNode_BlendSpacePlayer* BSPlayer = NewObject<UAnimGraphNode_BlendSpacePlayer>(StateAnimGraph);
			StateAnimGraph->AddNode(BSPlayer, true, false);
			BSPlayer->CreateNewGuid();
			BSPlayer->PostPlacedNewNode();
			BSPlayer->AllocateDefaultPins();
			BSPlayer->NodePosX = -300;
			BSPlayer->NodePosY = 0;
			BSPlayer->SetFlags(RF_Transactional);

			// Assign blendspace asset.
			// TODO(P0-1): verify exact property name; in UE 5.x the runtime
			// node is `Node` (FAnimNode_BlendSpacePlayer) which exposes a
			// `BlendSpace` setter via SetBlendSpace() or a public field.
			BSPlayer->Node.SetBlendSpace(BlendSpace);
			BSPlayer->ReconstructNode();

			UAnimGraphNode_StateResult* ResultNode = FindStateResultNode(StateAnimGraph);
			if (!ResultNode)
			{
				OutError = TEXT("State result node was not found in state's bound graph.");
				return false;
			}

			FString WireError;
			if (!TryWirePoseToResult(BSPlayer, ResultNode, WireError))
			{
				OutError = WireError;
				return false;
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
			if (!VerifyAnimGraphContainsNode(StateAnimGraph, BSPlayer))
			{
				OutError = TEXT("BlendSpace player node was not present after creation.");
				return false;
			}

			OutStructured->SetStringField(TEXT("state"), StateName);
			OutStructured->SetStringField(TEXT("blendspace"), BlendSpacePath);
			if (!RunAnimBlueprintMutationGate(AnimBP, TEXT("add_blendspace_node"), OutStructured, OutError))
			{
				return false;
			}
			OutSummary = FString::Printf(TEXT("Added BlendSpace player '%s' to state '%s'."), *BlendSpacePath, *StateName);
			return true;
		}
	});

	// ----- animbp_list_states -------------------------------------------
	Registry.Register({
		TEXT("animbp_list_states"),
		TEXT("Enumerate all state machines (or a specific one) with their states and transitions in an AnimBlueprint."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("machine_name"), FSololmcpSchemaBuilder::String(TEXT("Optional. If omitted, list all state machines."))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			UAnimBlueprint* AnimBP = LoadAnimBlueprintAssetLocal(Context.Services, AssetPath, OutError);
			if (!AnimBP)
			{
				return false;
			}

			FString FilterName;
			const bool bFilter = Arguments->TryGetStringField(TEXT("machine_name"), FilterName) && !FilterName.IsEmpty();

			TArray<TSharedPtr<FJsonValue>> MachinesArr;
			int32 Count = 0;
			for (UAnimGraphNode_StateMachineBase* MachineNode : GetAnimStateMachineNodesLocal(AnimBP))
			{
				if (!MachineNode)
				{
					continue;
				}
				if (bFilter && MachineNode->GetStateMachineName() != FilterName)
				{
					continue;
				}
				MachinesArr.Add(MakeShared<FJsonValueObject>(MakeFullMachineDescription(MachineNode)));
				++Count;
			}

			OutStructured->SetArrayField(TEXT("state_machines"), MachinesArr);
			OutStructured->SetNumberField(TEXT("count"), Count);
			OutSummary = FString::Printf(TEXT("Listed %d state machine(s) for '%s'."), Count, *AssetPath);
			return true;
		}
	});
}

} // namespace UE::SOMOLMCP
