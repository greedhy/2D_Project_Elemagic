// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpBlueprintFlowTools.cpp
// ----------------------------------------------------------------------------
// Adds 7 new MCP tools for Blueprint control-flow node creation:
//   1. blueprint_add_for_loop_node       (StandardMacros: ForLoop)
//   2. blueprint_add_for_each_loop_node  (StandardMacros: ForEachLoop)
//   3. blueprint_add_while_loop_node     (StandardMacros: WhileLoop)
//   4. blueprint_add_switch_int_node     (UK2Node_SwitchInteger)
//   5. blueprint_add_switch_enum_node    (UK2Node_SwitchEnum, bound to enum)
//   6. blueprint_add_select_node         (UK2Node_Select with index pin type)
//   7. blueprint_add_event_node          (UK2Node_Event for parent-class
//                                         BlueprintImplementableEvents like
//                                         ReceiveBeginPlay, ReceiveTick, ...)
//
// Helpers (TryGetGraphAndBlueprint, GetBlueprintNodeLocationFromArguments,
// SpawnBlueprintNodeByClass, BlueprintNodeToJson) are file-static in
// SololmcpDomainTools.cpp, so they are duplicated here in the anonymous
// namespace per task instructions (cannot be externed from another TU).
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"

#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintEditorLibrary.h"
#include "Blueprint/DragDropOperation.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/MemberReference.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInterface.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Select.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"

#define LOCTEXT_NAMESPACE "SOMOLMCP_BlueprintFlow"

namespace UE::SOMOLMCP
{
	namespace
	{
		// --------------------------------------------------------------------
		// Helper duplicates (file-static counterparts live in SololmcpDomainTools.cpp)
		// --------------------------------------------------------------------

		FString PinDirectionToStringLocal(const EEdGraphPinDirection Direction)
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

		UEdGraphNode* GetPinOwningNodeSafe(const UEdGraphPin* Pin)
		{
			return Pin ? Pin->GetOwningNodeUnchecked() : nullptr;
		}

		bool IsPinOwnedByNode(const UEdGraphPin* Pin, const UEdGraphNode* ExpectedNode)
		{
			if (!Pin || !ExpectedNode || GetPinOwningNodeSafe(Pin) != ExpectedNode)
			{
				return false;
			}
			return ExpectedNode->Pins.Contains(const_cast<UEdGraphPin*>(Pin));
		}

		bool IsGraphPinStructurallyUsable(const UEdGraphPin* Pin)
		{
			const UEdGraphNode* Owner = GetPinOwningNodeSafe(Pin);
			return Owner && Owner->Pins.Contains(const_cast<UEdGraphPin*>(Pin));
		}

		bool EnsurePinSafeForMutation(
			UEdGraphNode* ExpectedNode,
			UEdGraphPin* Pin,
			const FString& PinLabel,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutError)
		{
			if (!IsPinOwnedByNode(Pin, ExpectedNode))
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("damaged_pin_ownership"));
				OutStructured->SetStringField(TEXT("pin_label"), PinLabel);
				OutStructured->SetStringField(TEXT("expected_node_guid"), ExpectedNode ? ExpectedNode->NodeGuid.ToString() : FString());
				OutStructured->SetStringField(TEXT("actual_node_guid"), GetPinOwningNodeSafe(Pin) ? GetPinOwningNodeSafe(Pin)->NodeGuid.ToString() : FString());
				OutError = TEXT("Blueprint pin is damaged or no longer owned by the requested node; mutation was blocked before touching UE graph links.");
				return false;
			}

			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!IsGraphPinStructurallyUsable(LinkedPin))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("damaged_pin_links"));
					OutStructured->SetStringField(TEXT("pin_label"), PinLabel);
					OutStructured->SetStringField(TEXT("node_guid"), ExpectedNode->NodeGuid.ToString());
					OutStructured->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
					OutError = TEXT("Blueprint pin has a damaged linked pin; mutation was blocked to avoid an editor assertion.");
					return false;
				}
			}
			return true;
		}

		TSharedRef<FJsonObject> BlueprintPinToJson(const UEdGraphPin* Pin)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Pin)
			{
				return Result;
			}

			Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
			Result->SetStringField(TEXT("direction"), PinDirectionToStringLocal(Pin->Direction));
			Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			Result->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
			Result->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
			Result->SetStringField(TEXT("defaultObject"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
			Result->SetStringField(TEXT("persistentGuid"), Pin->PersistentGuid.ToString());
			if (Pin->PinType.PinSubCategoryObject.IsValid())
			{
				Result->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject->GetPathName());
			}
			Result->SetNumberField(TEXT("linked_count"), Pin->LinkedTo.Num());
			TArray<TSharedPtr<FJsonValue>> LinkedPins;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
				if (!LinkedPin || !LinkedOwner)
				{
					continue;
				}
				TSharedRef<FJsonObject> LinkedJson = MakeShared<FJsonObject>();
				LinkedJson->SetStringField(TEXT("node_guid"), LinkedOwner->NodeGuid.ToString());
				LinkedJson->SetStringField(TEXT("node_title"), LinkedOwner->GetNodeTitle(ENodeTitleType::ListView).ToString());
				LinkedJson->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
				LinkedJson->SetStringField(TEXT("direction"), PinDirectionToStringLocal(LinkedPin->Direction));
				LinkedPins.Add(MakeShared<FJsonValueObject>(LinkedJson));
			}
			Result->SetArrayField(TEXT("linked_pins"), LinkedPins);
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintNodeToJson(UEdGraphNode* Node)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Node)
			{
				return Result;
			}

			Result->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
			Result->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
			Result->SetStringField(TEXT("id"), Node->NodeGuid.IsValid() ? Node->NodeGuid.ToString() : Node->GetName());
			Result->SetStringField(TEXT("name"), Node->GetName());
			Result->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Result->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			Result->SetNumberField(TEXT("x"), Node->NodePosX);
			Result->SetNumberField(TEXT("y"), Node->NodePosY);

			if (Node->GetGraph())
			{
				Result->SetStringField(TEXT("graph"), Node->GetGraph()->GetName());
			}

			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				Pins.Add(MakeShared<FJsonValueObject>(BlueprintPinToJson(Pin)));
			}
			Result->SetArrayField(TEXT("pins"), Pins);
			return Result;
		}

		TSharedRef<FJsonObject> MakeBlueprintTargetBinding(UBlueprint* Blueprint, UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
			Binding->SetStringField(TEXT("asset_path"), Blueprint ? Blueprint->GetPathName() : FString());
			Binding->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : FString());
			Binding->SetStringField(TEXT("asset_class"), Blueprint && Blueprint->GetClass() ? Blueprint->GetClass()->GetName() : FString());
			Binding->SetStringField(TEXT("package_path"), Blueprint && Blueprint->GetPackage() ? Blueprint->GetPackage()->GetName() : FString());
			return Binding;
		}

		TSharedRef<FJsonObject> MakeBlueprintGraphSummary(UBlueprint* Blueprint, UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
			int32 NodeCount = 0;
			int32 PinCount = 0;
			int32 LinkCount = 0;
			if (Graph)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node)
					{
						continue;
					}
					++NodeCount;
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin)
						{
							continue;
						}
						++PinCount;
						if (Pin->Direction == EGPD_Output)
						{
							LinkCount += Pin->LinkedTo.Num();
						}
					}
				}
			}
			Summary->SetStringField(TEXT("asset_path"), Blueprint ? Blueprint->GetPathName() : FString());
			Summary->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : FString());
			Summary->SetNumberField(TEXT("node_count"), NodeCount);
			Summary->SetNumberField(TEXT("pin_count"), PinCount);
			Summary->SetNumberField(TEXT("link_count"), LinkCount);
			return Summary;
		}

		bool RunBlueprintGraphMutationGate(
			UBlueprint* Blueprint,
			UEdGraph* Graph,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutError);

		void AttachBlueprintGraphEditReceipt(
			TSharedRef<FJsonObject>& OutStructured,
			UBlueprint* Blueprint,
			UEdGraph* Graph,
			const FString& Operation,
			const FString& ReadbackField,
			const bool bReadbackOk)
		{
			OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.blueprint_graph_edit_receipt.v1"));
			OutStructured->SetStringField(TEXT("operation"), Operation);
			OutStructured->SetObjectField(TEXT("target_binding"), MakeBlueprintTargetBinding(Blueprint, Graph));
			OutStructured->SetObjectField(TEXT("graph_summary"), MakeBlueprintGraphSummary(Blueprint, Graph));
			TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
			Readback->SetStringField(TEXT("field"), ReadbackField);
			Readback->SetBoolField(TEXT("verified"), bReadbackOk);
			Readback->SetStringField(TEXT("tool"), TEXT("immediate_k2_pin_readback"));
			OutStructured->SetObjectField(TEXT("post_edit_readback"), Readback);
			OutStructured->SetStringField(TEXT("rollback_hint"), TEXT("Use the editor transaction/undo stack before save, or revert with blueprint_break_single_pin_link / blueprint_connect_pins using captured pin refs."));
			OutStructured->SetStringField(TEXT("compile_gate"), TEXT("Enforced inline by receipt_gate: RefreshAllNodes + compile diagnostics + repair issue scan."));
			OutStructured->SetStringField(TEXT("diagnostic_code"), bReadbackOk ? TEXT("ok") : TEXT("post_edit_readback_failed"));
		}

		UEdGraph* FindBlueprintGraphByNameLocal(UBlueprint* Blueprint, const FString& GraphName)
		{
			if (!Blueprint)
			{
				return nullptr;
			}

			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (Graph && (Graph->GetName() == GraphName || Graph->GetFName().ToString() == GraphName))
				{
					return Graph;
				}
			}
			return nullptr;
		}

		bool TryGetGraphAndBlueprint(
			FSololmcpEditorServices& Services,
			const FString& AssetPath,
			const FString& GraphName,
			UBlueprint*& OutBlueprint,
			UEdGraph*& OutGraph,
			FString& OutError)
		{
			OutBlueprint = nullptr;
			OutGraph = nullptr;

			UObject* Asset = Services.LoadAsset(AssetPath, OutError);
			if (!Asset)
			{
				return false;
			}

			OutBlueprint = Cast<UBlueprint>(Asset);
			if (!OutBlueprint)
			{
				OutError = TEXT("Asset is not a Blueprint.");
				return false;
			}

			OutGraph = FindBlueprintGraphByNameLocal(OutBlueprint, GraphName);
			if (!OutGraph)
			{
				OutError = TEXT("Blueprint graph was not found.");
				return false;
			}
			return true;
		}

		FVector2f GetBlueprintNodeLocationFromArguments(const TSharedRef<FJsonObject>& Arguments)
		{
			// Accept both `pos_x/pos_y` (per task spec) and `node_x/node_y` (existing convention).
			int32 NodeX = 0;
			int32 NodeY = 0;
			if (Arguments->HasTypedField<EJson::Number>(TEXT("pos_x")))
			{
				NodeX = Arguments->GetIntegerField(TEXT("pos_x"));
			}
			else if (Arguments->HasTypedField<EJson::Number>(TEXT("node_x")))
			{
				NodeX = Arguments->GetIntegerField(TEXT("node_x"));
			}
			if (Arguments->HasTypedField<EJson::Number>(TEXT("pos_y")))
			{
				NodeY = Arguments->GetIntegerField(TEXT("pos_y"));
			}
			else if (Arguments->HasTypedField<EJson::Number>(TEXT("node_y")))
			{
				NodeY = Arguments->GetIntegerField(TEXT("node_y"));
			}
			return FVector2f(static_cast<float>(NodeX), static_cast<float>(NodeY));
		}

		UEdGraphNode* SpawnBlueprintNodeByClass(
			UEdGraph* Graph,
			TSubclassOf<UEdGraphNode> NodeClass,
			const FVector2f& Location,
			const UBlueprintNodeSpawner::FCustomizeNodeDelegate& CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate())
		{
			if (!Graph || !*NodeClass)
			{
				return nullptr;
			}

			UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(NodeClass, nullptr, CustomizeNodeDelegate);
			return NodeSpawner ? NodeSpawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(Location)) : nullptr;
		}

		bool VerifyGraphContainsNode(UEdGraph* Graph, UEdGraphNode* Node)
		{
			return Graph && Node && Graph->Nodes.Contains(Node);
		}

		bool JsonArrayToStringSet(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName, TSet<FString>& OutValues)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Arguments->TryGetArrayField(FieldName, Values) || !Values)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString StringValue;
				if (Value.IsValid() && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
				{
					OutValues.Add(StringValue);
				}
			}
			return OutValues.Num() > 0;
		}

		void CollectBlueprintNodesForT3dExport(
			UEdGraph* Graph,
			const TSharedRef<FJsonObject>& Arguments,
			TSet<UObject*>& OutNodes,
			TArray<TSharedPtr<FJsonValue>>& OutNodeReceipts)
		{
			if (!Graph)
			{
				return;
			}

			TSet<FString> RequestedGuids;
			TSet<FString> RequestedNames;
			const bool bHasGuidFilter = JsonArrayToStringSet(Arguments, TEXT("node_guids"), RequestedGuids);
			const bool bHasNameFilter = JsonArrayToStringSet(Arguments, TEXT("node_names"), RequestedNames);

			bool bAllNodes = true;
			Arguments->TryGetBoolField(TEXT("all_nodes"), bAllNodes);
			if (bHasGuidFilter || bHasNameFilter)
			{
				bAllNodes = false;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				const FString Guid = Node->NodeGuid.ToString();
				const FString Name = Node->GetName();
				const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
				const bool bMatches = bAllNodes || RequestedGuids.Contains(Guid) || RequestedNames.Contains(Name) || RequestedNames.Contains(Title);
				if (!bMatches)
				{
					continue;
				}

				OutNodes.Add(Node);
				OutNodeReceipts.Add(MakeShared<FJsonValueObject>(BlueprintNodeToJson(Node)));
			}
		}

		bool HasExplicitImportPosition(const TSharedRef<FJsonObject>& Arguments)
		{
			return Arguments->HasTypedField<EJson::Number>(TEXT("pos_x")) ||
				Arguments->HasTypedField<EJson::Number>(TEXT("pos_y")) ||
				Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ||
				Arguments->HasTypedField<EJson::Number>(TEXT("node_y"));
		}

		void MoveImportedNodesToCenter(const TSet<UEdGraphNode*>& ImportedNodes, const FVector2f& TargetCenter)
		{
			if (ImportedNodes.Num() <= 0)
			{
				return;
			}

			FVector2f Average(0.0f, 0.0f);
			for (UEdGraphNode* Node : ImportedNodes)
			{
				if (Node)
				{
					Average.X += Node->NodePosX;
					Average.Y += Node->NodePosY;
				}
			}
			const float InvCount = 1.0f / static_cast<float>(ImportedNodes.Num());
			Average.X *= InvCount;
			Average.Y *= InvCount;

			for (UEdGraphNode* Node : ImportedNodes)
			{
				if (!Node)
				{
					continue;
				}

				Node->NodePosX = static_cast<int32>((static_cast<float>(Node->NodePosX) - Average.X) + TargetCenter.X);
				Node->NodePosY = static_cast<int32>((static_cast<float>(Node->NodePosY) - Average.Y) + TargetCenter.Y);
				Node->SnapToGrid(16);
			}
		}

		TSharedRef<FJsonObject> MakeBlueprintT3dGraphSchema(const bool bForImport)
		{
			TMap<FString, TSharedRef<FJsonObject>> Properties = {
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UBlueprint asset path, e.g. /Game/BP_MyActor"))},
				{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Blueprint graph name, e.g. EventGraph"))}
			};
			if (bForImport)
			{
				Properties.Add(TEXT("t3d"), FSololmcpSchemaBuilder::String(TEXT("Blueprint node copy text exported by FEdGraphUtilities.")));
				Properties.Add(TEXT("pos_x"), FSololmcpSchemaBuilder::Integer(TEXT("Optional target center X for imported nodes.")));
				Properties.Add(TEXT("pos_y"), FSololmcpSchemaBuilder::Integer(TEXT("Optional target center Y for imported nodes.")));
				return FSololmcpSchemaBuilder::Object(Properties, {TEXT("asset_path"), TEXT("graph_name"), TEXT("t3d")});
			}

			Properties.Add(TEXT("all_nodes"), FSololmcpSchemaBuilder::Boolean(TEXT("Export every node in the graph. Default true unless filters are provided.")));
			Properties.Add(TEXT("node_guids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Node GUID to include."))));
			Properties.Add(TEXT("node_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Node object name or list-view title to include."))));
			return FSololmcpSchemaBuilder::Object(Properties, {TEXT("asset_path"), TEXT("graph_name")});
		}

		// --------------------------------------------------------------------
		// Macro loop helper — load a named macro graph from StandardMacros and
		// spawn a UK2Node_MacroInstance bound to it.
		// --------------------------------------------------------------------

		// TODO(P0-3): verify this is the canonical StandardMacros path on UE5.7.4.
		const TCHAR* GStandardMacrosPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");

		UEdGraph* LoadStandardMacroGraph(const FString& MacroName, FString& OutError)
		{
			UBlueprint* StandardMacrosBP = LoadObject<UBlueprint>(nullptr, GStandardMacrosPath);
			if (!StandardMacrosBP)
			{
				OutError = TEXT("Failed to load StandardMacros blueprint.");
				return nullptr;
			}

			TArray<UEdGraph*> AllGraphs;
			StandardMacrosBP->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (Graph && Graph->GetFName().ToString() == MacroName)
				{
					return Graph;
				}
			}

			OutError = FString::Printf(TEXT("Macro graph '%s' not found in StandardMacros."), *MacroName);
			return nullptr;
		}

		bool SpawnStandardMacroLoop(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			const TCHAR* MacroName,
			const FText& TransactionDesc,
			const TCHAR* SummaryText,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString AssetPath;
			FString GraphName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
			{
				OutError = TEXT("Missing asset_path or graph_name.");
				return false;
			}

			UBlueprint* Blueprint = nullptr;
			UEdGraph* Graph = nullptr;
			if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
			{
				return false;
			}

			UEdGraph* MacroGraph = LoadStandardMacroGraph(MacroName, OutError);
			if (!MacroGraph)
			{
				return false;
			}

			const FScopedTransaction Transaction(TransactionDesc);
			Blueprint->Modify();

			UEdGraphNode* Node = SpawnBlueprintNodeByClass(
				Graph,
				UK2Node_MacroInstance::StaticClass(),
				GetBlueprintNodeLocationFromArguments(Arguments),
				UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda([MacroGraph](UEdGraphNode* NewNode, bool /*bIsTemplateNode*/)
				{
					if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(NewNode))
					{
						MacroNode->SetMacroGraph(MacroGraph);
						MacroNode->ReconstructNode();
					}
				}));

			if (!Node)
			{
				OutError = TEXT("Failed to spawn macro loop node.");
				return false;
			}
			if (!VerifyGraphContainsNode(Graph, Node))
			{
				OutError = TEXT("Macro loop node was not present in the graph after creation.");
				return false;
			}
			if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
			{
				if (MacroNode->GetMacroGraph() != MacroGraph)
				{
					OutError = FString::Printf(TEXT("Macro loop node was not bound to StandardMacros.%s."), MacroName);
					return false;
				}
			}
			else
			{
				OutError = TEXT("Spawned node was not a UK2Node_MacroInstance.");
				return false;
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			OutStructured = BlueprintNodeToJson(Node);
			AttachBlueprintGraphEditReceipt(
				OutStructured,
				Blueprint,
				Graph,
				TEXT("add_macro_loop_node"),
				TEXT("node_verified"),
				true);
			if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
			{
				return false;
			}
			OutSummary = SummaryText;
			return true;
		}

		// --------------------------------------------------------------------
		// Common schema for tools taking just asset_path + graph_name + pos.
		// --------------------------------------------------------------------

		TSharedRef<FJsonObject> MakeBasicGraphPosSchema()
		{
			return FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name")});
		}

		// --------------------------------------------------------------------
		// blueprint_add_select_node — set index pin type from `pin_type` arg.
		// --------------------------------------------------------------------

		bool ConfigureSelectNodeIndexType(UK2Node_Select* SelectNode, const FString& PinTypeStr, FString& OutError)
		{
			if (!SelectNode)
			{
				return false;
			}

			FEdGraphPinType NewType;
			NewType.ContainerType = EPinContainerType::None;
			NewType.bIsReference = false;

			const FString Lower = PinTypeStr.ToLower();
			if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32"))
			{
				NewType.PinCategory = UEdGraphSchema_K2::PC_Int;
			}
			else if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
			{
				NewType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			}
			else if (Lower == TEXT("string"))
			{
				NewType.PinCategory = UEdGraphSchema_K2::PC_String;
			}
			else if (Lower == TEXT("float") || Lower == TEXT("real") || Lower == TEXT("double"))
			{
				NewType.PinCategory = UEdGraphSchema_K2::PC_Real;
				NewType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			}
			else
			{
				OutError = FString::Printf(TEXT("Unsupported select pin_type '%s'. Use int|bool|string|float."), *PinTypeStr);
				return false;
			}

			// TODO(P0-3): UK2Node_Select exposes a private IndexPinType; use the
			// schema's ChangePinType path on the index pin instead. ReconstructNode
			// then rebuilds option/return pins with the matching type.
			if (UEdGraphPin* IndexPin = SelectNode->GetIndexPin())
			{
				IndexPin->PinType = NewType;
				if (const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(SelectNode->GetSchema()))
				{
					Schema->ForceVisualizationCacheClear();
				}
			}
			SelectNode->ReconstructNode();
			return true;
		}

		// --------------------------------------------------------------------
		// blueprint_add_event_node helpers.
		// --------------------------------------------------------------------

		UFunction* FindOverridableEventFunction(UClass* OwnerClass, const FName EventName)
		{
			if (!OwnerClass || EventName.IsNone())
			{
				return nullptr;
			}

			// Walk parent chain looking for a matching UFunction.
			for (UClass* Cls = OwnerClass; Cls; Cls = Cls->GetSuperClass())
			{
				if (UFunction* Func = Cls->FindFunctionByName(EventName))
				{
					return Func;
				}
			}
			return nullptr;
		}

		bool IsAlreadyPlacedEvent(UEdGraph* Graph, UClass* SignatureOwner, const FName EventName)
		{
			if (!Graph)
			{
				return false;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
				{
					if (EventNode->EventReference.GetMemberName() == EventName)
					{
						return true;
					}
				}
			}
			return false;
		}

		bool IsExecPin(const UEdGraphPin* Pin)
		{
			return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		}

		FString GetNodeStableId(const UEdGraphNode* Node)
		{
			if (!Node)
			{
				return FString();
			}
			return Node->NodeGuid.IsValid() ? Node->NodeGuid.ToString() : Node->GetName();
		}

		FString GetCompactNodeTitle(const UEdGraphNode* Node)
		{
			return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
		}

		void CountNodePins(const UEdGraphNode* Node, int32& OutExecIn, int32& OutExecOut, int32& OutDataIn, int32& OutDataOut)
		{
			OutExecIn = 0;
			OutExecOut = 0;
			OutDataIn = 0;
			OutDataOut = 0;
			if (!Node)
			{
				return;
			}

			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				const bool bExec = IsExecPin(Pin);
				if (Pin->Direction == EGPD_Input)
				{
					bExec ? ++OutExecIn : ++OutDataIn;
				}
				else if (Pin->Direction == EGPD_Output)
				{
					bExec ? ++OutExecOut : ++OutDataOut;
				}
			}
		}

		bool IsExecutionEntryNode(const UEdGraphNode* Node)
		{
			if (!Node)
			{
				return false;
			}

			int32 ExecIn = 0;
			int32 ExecOut = 0;
			int32 DataIn = 0;
			int32 DataOut = 0;
			CountNodePins(Node, ExecIn, ExecOut, DataIn, DataOut);
			if (ExecOut <= 0 || ExecIn > 0)
			{
				return false;
			}

			const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();
			return ClassName.Contains(TEXT("Event")) || ClassName.Contains(TEXT("FunctionEntry")) || ClassName.Contains(TEXT("Tunnel"));
		}

		TSharedRef<FJsonObject> BlueprintNodeToCompactSummary(UEdGraphNode* Node, int32& InOutExecLinkCount)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Node)
			{
				return Result;
			}

			int32 ExecIn = 0;
			int32 ExecOut = 0;
			int32 DataIn = 0;
			int32 DataOut = 0;
			CountNodePins(Node, ExecIn, ExecOut, DataIn, DataOut);

			Result->SetStringField(TEXT("id"), GetNodeStableId(Node));
			Result->SetStringField(TEXT("title"), GetCompactNodeTitle(Node));
			Result->SetStringField(TEXT("type"), Node->GetClass() ? Node->GetClass()->GetName() : TEXT("Unknown"));
			Result->SetNumberField(TEXT("x"), Node->NodePosX);
			Result->SetNumberField(TEXT("y"), Node->NodePosY);
			Result->SetNumberField(TEXT("exec_in"), ExecIn);
			Result->SetNumberField(TEXT("exec_out"), ExecOut);
			Result->SetNumberField(TEXT("data_in"), DataIn);
			Result->SetNumberField(TEXT("data_out"), DataOut);
			Result->SetStringField(TEXT("comment"), Node->NodeComment);
			const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();
			Result->SetBoolField(TEXT("is_comment_box"), ClassName.Contains(TEXT("Comment")));
			Result->SetBoolField(TEXT("is_reroute"), ClassName.Contains(TEXT("Knot")) || ClassName.Contains(TEXT("Reroute")));
			Result->SetBoolField(TEXT("is_collapsed_graph"), ClassName.Contains(TEXT("Composite")) || ClassName.Contains(TEXT("Collapse")));

			TArray<TSharedPtr<FJsonValue>> Pins;
			TArray<TSharedPtr<FJsonValue>> DataLinks;
			TArray<TSharedPtr<FJsonValue>> ReferencedAssets;
			TSet<FString> SeenReferences;

			TArray<TSharedPtr<FJsonValue>> ExecLinks;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				TSharedRef<FJsonObject> PinJson = BlueprintPinToJson(Pin);
				PinJson->SetStringField(TEXT("shape"), IsExecPin(Pin) ? TEXT("exec") : TEXT("data"));
				PinJson->SetStringField(TEXT("display_name"), Pin->GetDisplayName().ToString());
				PinJson->SetNumberField(TEXT("linked_count"), Pin->LinkedTo.Num());
				TArray<TSharedPtr<FJsonValue>> LinkedPins;
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
					if (!LinkedPin || !LinkedOwner)
					{
						continue;
					}
					TSharedRef<FJsonObject> LinkedJson = MakeShared<FJsonObject>();
					LinkedJson->SetStringField(TEXT("node_id"), GetNodeStableId(LinkedOwner));
					LinkedJson->SetStringField(TEXT("node_title"), GetCompactNodeTitle(LinkedOwner));
					LinkedJson->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
					LinkedPins.Add(MakeShared<FJsonValueObject>(LinkedJson));

					if (!IsExecPin(Pin) && Pin->Direction == EGPD_Output)
					{
						TSharedRef<FJsonObject> DataLinkJson = MakeShared<FJsonObject>();
						DataLinkJson->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
						DataLinkJson->SetStringField(TEXT("to_node"), GetNodeStableId(LinkedOwner));
						DataLinkJson->SetStringField(TEXT("to_title"), GetCompactNodeTitle(LinkedOwner));
						DataLinkJson->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
						DataLinkJson->SetStringField(TEXT("pin_category"), Pin->PinType.PinCategory.ToString());
						DataLinks.Add(MakeShared<FJsonValueObject>(DataLinkJson));
					}
				}
				PinJson->SetArrayField(TEXT("linked_pins"), LinkedPins);
				Pins.Add(MakeShared<FJsonValueObject>(PinJson));

				if (Pin->PinType.PinSubCategoryObject.IsValid())
				{
					const FString RefPath = Pin->PinType.PinSubCategoryObject->GetPathName();
					if (!RefPath.IsEmpty() && !SeenReferences.Contains(RefPath))
					{
						SeenReferences.Add(RefPath);
						TSharedRef<FJsonObject> RefJson = MakeShared<FJsonObject>();
						RefJson->SetStringField(TEXT("path"), RefPath);
						RefJson->SetStringField(TEXT("source"), FString::Printf(TEXT("pin:%s"), *Pin->PinName.ToString()));
						ReferencedAssets.Add(MakeShared<FJsonValueObject>(RefJson));
					}
				}

				if (!IsExecPin(Pin) || Pin->Direction != EGPD_Output)
				{
					continue;
				}

				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
					if (!IsExecPin(LinkedPin) || !LinkedOwner)
					{
						continue;
					}

					TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
					LinkJson->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
					LinkJson->SetStringField(TEXT("to_node"), GetNodeStableId(LinkedOwner));
					LinkJson->SetStringField(TEXT("to_title"), GetCompactNodeTitle(LinkedOwner));
					LinkJson->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
					ExecLinks.Add(MakeShared<FJsonValueObject>(LinkJson));
					++InOutExecLinkCount;
				}
			}
			Result->SetArrayField(TEXT("pins"), Pins);
			Result->SetArrayField(TEXT("exec_links"), ExecLinks);
			Result->SetArrayField(TEXT("data_links"), DataLinks);
			Result->SetArrayField(TEXT("referenced_assets"), ReferencedAssets);
			return Result;
		}

		TSharedRef<FJsonObject> BlueprintGraphToCompactSummary(UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
			if (!Graph)
			{
				return GraphJson;
			}

			TArray<TSharedPtr<FJsonValue>> Nodes;
			TArray<TSharedPtr<FJsonValue>> EntryNodes;
			int32 ExecLinkCount = 0;

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}

				Nodes.Add(MakeShared<FJsonValueObject>(BlueprintNodeToCompactSummary(Node, ExecLinkCount)));
				if (IsExecutionEntryNode(Node))
				{
					EntryNodes.Add(MakeShared<FJsonValueString>(GetNodeStableId(Node)));
				}
			}

			GraphJson->SetStringField(TEXT("name"), Graph->GetName());
			GraphJson->SetStringField(TEXT("class"), Graph->GetClass() ? Graph->GetClass()->GetName() : TEXT("Unknown"));
			GraphJson->SetNumberField(TEXT("node_count"), Nodes.Num());
			GraphJson->SetNumberField(TEXT("exec_link_count"), ExecLinkCount);
			GraphJson->SetArrayField(TEXT("entry_nodes"), EntryNodes);
			GraphJson->SetArrayField(TEXT("nodes"), Nodes);
			return GraphJson;
		}

		UEdGraphNode* FindNodeByStableIdOrTitle(UEdGraph* Graph, const FString& NodeId, const FString& NodeTitle)
		{
			if (!Graph)
			{
				return nullptr;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (!NodeId.IsEmpty() && GetNodeStableId(Node) == NodeId)
				{
					return Node;
				}
				if (!NodeTitle.IsEmpty() && GetCompactNodeTitle(Node).Equals(NodeTitle, ESearchCase::IgnoreCase))
				{
					return Node;
				}
			}
			return nullptr;
		}

		struct FExecutionFlowFrame
		{
			UEdGraphNode* Node = nullptr;
			FString ViaPin;
			TArray<FString> PathNodeIds;
			TArray<FString> PathLabels;
			TSet<FString> Visited;
		};

		void TraceExecutionFlowFromNode(
			UEdGraphNode* StartNode,
			const int32 MaxDepth,
			const int32 MaxPaths,
			TArray<TSharedPtr<FJsonValue>>& OutPaths,
			int32& OutTruncatedAtDepth,
			int32& OutTruncatedAtPathLimit)
		{
			if (!StartNode || MaxDepth <= 0 || MaxPaths <= 0)
			{
				return;
			}

			TArray<FExecutionFlowFrame> Stack;
			FExecutionFlowFrame Initial;
			Initial.Node = StartNode;
			Stack.Add(Initial);

			while (!Stack.IsEmpty() && OutPaths.Num() < MaxPaths)
			{
				FExecutionFlowFrame Frame = Stack.Pop(SOMOLMCP_NO_SHRINK);
				if (!Frame.Node)
				{
					continue;
				}

				const FString NodeId = GetNodeStableId(Frame.Node);
				if (Frame.Visited.Contains(NodeId))
				{
					Frame.PathNodeIds.Add(NodeId);
					Frame.PathLabels.Add(GetCompactNodeTitle(Frame.Node));
					TSharedRef<FJsonObject> PathJson = MakeShared<FJsonObject>();
					PathJson->SetArrayField(TEXT("node_ids"), [&Frame]()
					{
						TArray<TSharedPtr<FJsonValue>> Values;
						for (const FString& Id : Frame.PathNodeIds)
						{
							Values.Add(MakeShared<FJsonValueString>(Id));
						}
						return Values;
					}());
					PathJson->SetArrayField(TEXT("labels"), [&Frame]()
					{
						TArray<TSharedPtr<FJsonValue>> Values;
						for (const FString& Label : Frame.PathLabels)
						{
							Values.Add(MakeShared<FJsonValueString>(Label));
						}
						return Values;
					}());
					PathJson->SetBoolField(TEXT("cycle_detected"), true);
					OutPaths.Add(MakeShared<FJsonValueObject>(PathJson));
					continue;
				}

				Frame.Visited.Add(NodeId);
				Frame.PathNodeIds.Add(NodeId);
				Frame.PathLabels.Add(GetCompactNodeTitle(Frame.Node));
				if (Frame.PathNodeIds.Num() >= MaxDepth)
				{
					++OutTruncatedAtDepth;
					TSharedRef<FJsonObject> PathJson = MakeShared<FJsonObject>();
					TArray<TSharedPtr<FJsonValue>> NodeIds;
					TArray<TSharedPtr<FJsonValue>> Labels;
					for (const FString& Id : Frame.PathNodeIds)
					{
						NodeIds.Add(MakeShared<FJsonValueString>(Id));
					}
					for (const FString& Label : Frame.PathLabels)
					{
						Labels.Add(MakeShared<FJsonValueString>(Label));
					}
					PathJson->SetArrayField(TEXT("node_ids"), NodeIds);
					PathJson->SetArrayField(TEXT("labels"), Labels);
					PathJson->SetBoolField(TEXT("truncated_at_depth"), true);
					OutPaths.Add(MakeShared<FJsonValueObject>(PathJson));
					continue;
				}

				TArray<TPair<FString, UEdGraphNode*>> NextNodes;
				for (const UEdGraphPin* Pin : Frame.Node->Pins)
				{
					if (!IsExecPin(Pin) || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* LinkedOwner = GetPinOwningNodeSafe(LinkedPin);
						if (IsExecPin(LinkedPin) && LinkedOwner)
						{
							NextNodes.Add(TPair<FString, UEdGraphNode*>(Pin->PinName.ToString(), LinkedOwner));
						}
					}
				}

				if (NextNodes.IsEmpty())
				{
					TSharedRef<FJsonObject> PathJson = MakeShared<FJsonObject>();
					TArray<TSharedPtr<FJsonValue>> NodeIds;
					TArray<TSharedPtr<FJsonValue>> Labels;
					for (const FString& Id : Frame.PathNodeIds)
					{
						NodeIds.Add(MakeShared<FJsonValueString>(Id));
					}
					for (const FString& Label : Frame.PathLabels)
					{
						Labels.Add(MakeShared<FJsonValueString>(Label));
					}
					PathJson->SetArrayField(TEXT("node_ids"), NodeIds);
					PathJson->SetArrayField(TEXT("labels"), Labels);
					OutPaths.Add(MakeShared<FJsonValueObject>(PathJson));
					continue;
				}

				for (int32 Index = NextNodes.Num() - 1; Index >= 0; --Index)
				{
					if (OutPaths.Num() + Stack.Num() >= MaxPaths)
					{
						++OutTruncatedAtPathLimit;
						break;
					}

					FExecutionFlowFrame NextFrame = Frame;
					NextFrame.Node = NextNodes[Index].Value;
					NextFrame.ViaPin = NextNodes[Index].Key;
					Stack.Add(NextFrame);
				}
			}

			if (!Stack.IsEmpty())
			{
				OutTruncatedAtPathLimit += Stack.Num();
			}
		}

		TSharedRef<FJsonObject> ExplainBlueprintGraphMinimal(
			UBlueprint* Blueprint,
			UEdGraph* Graph,
			const int32 MaxDepth,
			const int32 MaxPaths)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Blueprint || !Graph)
			{
				return Result;
			}

			TSharedRef<FJsonObject> Summary = BlueprintGraphToCompactSummary(Graph);
			TArray<UEdGraphNode*> EntryNodes;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (IsExecutionEntryNode(Node))
				{
					EntryNodes.Add(Node);
				}
			}

			TArray<TSharedPtr<FJsonValue>> EntryNodeIds;
			TArray<TSharedPtr<FJsonValue>> Paths;
			int32 TruncatedAtDepth = 0;
			int32 TruncatedAtPathLimit = 0;
			for (UEdGraphNode* EntryNode : EntryNodes)
			{
				if (!EntryNode || Paths.Num() >= MaxPaths)
				{
					continue;
				}
				EntryNodeIds.Add(MakeShared<FJsonValueString>(GetNodeStableId(EntryNode)));
				TraceExecutionFlowFromNode(
					EntryNode,
					MaxDepth,
					MaxPaths - Paths.Num(),
					Paths,
					TruncatedAtDepth,
					TruncatedAtPathLimit);
			}

			TArray<TSharedPtr<FJsonValue>> Notes;
			if (EntryNodes.Num() == 0)
			{
				Notes.Add(MakeShared<FJsonValueString>(TEXT("No execution entry node was detected in this graph.")));
			}
			if (TruncatedAtDepth > 0 || TruncatedAtPathLimit > 0)
			{
				Notes.Add(MakeShared<FJsonValueString>(TEXT("Execution flow output was truncated by max_depth or max_paths.")));
			}

			Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
			Result->SetStringField(TEXT("graph_name"), Graph->GetName());
			Result->SetObjectField(TEXT("summary"), Summary);
			Result->SetArrayField(TEXT("entry_nodes"), EntryNodeIds);
			Result->SetArrayField(TEXT("execution_paths"), Paths);
			Result->SetNumberField(TEXT("path_count"), Paths.Num());
			Result->SetNumberField(TEXT("max_depth"), MaxDepth);
			Result->SetNumberField(TEXT("max_paths"), MaxPaths);
			Result->SetNumberField(TEXT("truncated_at_depth_count"), TruncatedAtDepth);
			Result->SetNumberField(TEXT("truncated_at_path_limit_count"), TruncatedAtPathLimit);
			Result->SetArrayField(TEXT("notes"), Notes);
			return Result;
		}

		UEdGraphNode* FindBlueprintNodeByGuidLocal(UBlueprint* Blueprint, const FString& NodeGuidString)
		{
			if (!Blueprint)
			{
				return nullptr;
			}

			FGuid NodeGuid;
			if (!FGuid::Parse(NodeGuidString, NodeGuid))
			{
				return nullptr;
			}

			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node && Node->NodeGuid == NodeGuid)
					{
						return Node;
					}
				}
			}
			return nullptr;
		}

		bool DoesPinMatchNameOrGuid(const UEdGraphPin* Pin, const FString& PinName, const FString& PinGuidString)
		{
			if (!Pin)
			{
				return false;
			}

			FGuid PinGuid;
			if (!PinGuidString.IsEmpty() && FGuid::Parse(PinGuidString, PinGuid) && Pin->PersistentGuid == PinGuid)
			{
				return true;
			}

			return !PinName.IsEmpty() &&
				(Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) ||
				 Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase));
		}

		UEdGraphPin* FindNodePinLocal(
			UEdGraphNode* Node,
			const FString& PinName,
			const FString& PinGuidString = FString(),
			const TOptional<EEdGraphPinDirection>& Direction = {})
		{
			if (!Node)
			{
				return nullptr;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || (Direction.IsSet() && Pin->Direction != Direction.GetValue()))
				{
					continue;
				}
				if (DoesPinMatchNameOrGuid(Pin, PinName, PinGuidString))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> MakePinRefJson(const UEdGraphPin* Pin)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Pin)
			{
				return Result;
			}
			UEdGraphNode* Owner = GetPinOwningNodeSafe(Pin);
			Result->SetStringField(TEXT("node_guid"), Owner ? Owner->NodeGuid.ToString() : FString());
			Result->SetStringField(TEXT("node_title"), Owner ? Owner->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
			Result->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
			Result->SetStringField(TEXT("display_name"), Pin->GetDisplayName().ToString());
			Result->SetStringField(TEXT("pin_guid"), Pin->PersistentGuid.ToString());
			Result->SetStringField(TEXT("direction"), PinDirectionToStringLocal(Pin->Direction));
			Result->SetStringField(TEXT("shape"), IsExecPin(Pin) ? TEXT("exec") : TEXT("data"));
			Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			Result->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
			if (Pin->PinType.PinSubCategoryObject.IsValid())
			{
				Result->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
			}
			Result->SetStringField(TEXT("default_value"), Pin->DefaultValue);
			Result->SetStringField(TEXT("default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
			Result->SetNumberField(TEXT("linked_count"), Pin->LinkedTo.Num());
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> MakeAvailablePinList(UEdGraphNode* Node, const TOptional<EEdGraphPinDirection>& Direction = {})
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			if (!Node)
			{
				return Pins;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && (!Direction.IsSet() || Pin->Direction == Direction.GetValue()))
				{
					Pins.Add(MakeShared<FJsonValueObject>(MakePinRefJson(Pin)));
				}
			}
			return Pins;
		}

		TSharedRef<FJsonObject> MakePinFailureDiagnostic(
			UEdGraphNode* Node,
			const FString& RequestedPin,
			const FString& RequestedGuid,
			const TOptional<EEdGraphPinDirection>& Direction)
		{
			TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
			Diagnostic->SetStringField(TEXT("requested_pin_name"), RequestedPin);
			Diagnostic->SetStringField(TEXT("requested_pin_guid"), RequestedGuid);
			Diagnostic->SetStringField(TEXT("requested_direction"), Direction.IsSet() ? PinDirectionToStringLocal(Direction.GetValue()) : TEXT("any"));
			Diagnostic->SetObjectField(TEXT("node"), BlueprintNodeToJson(Node));
			Diagnostic->SetArrayField(TEXT("available_pins"), MakeAvailablePinList(Node, Direction));
			return Diagnostic;
		}

		bool ResolveConnectionPins(
			UBlueprint* Blueprint,
			const TSharedRef<FJsonObject>& Arguments,
			UEdGraphNode*& OutFromNode,
			UEdGraphPin*& OutFromPin,
			UEdGraphNode*& OutToNode,
			UEdGraphPin*& OutToPin,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutError)
		{
			FString FromNodeGuid;
			FString FromPinName;
			FString ToNodeGuid;
			FString ToPinName;
			if (!Arguments->TryGetStringField(TEXT("from_node_guid"), FromNodeGuid) ||
				!Arguments->TryGetStringField(TEXT("from_pin_name"), FromPinName) ||
				!Arguments->TryGetStringField(TEXT("to_node_guid"), ToNodeGuid) ||
				!Arguments->TryGetStringField(TEXT("to_pin_name"), ToPinName))
			{
				OutError = TEXT("Missing pin connection arguments.");
				return false;
			}

			OutFromNode = FindBlueprintNodeByGuidLocal(Blueprint, FromNodeGuid);
			OutToNode = FindBlueprintNodeByGuidLocal(Blueprint, ToNodeGuid);
			if (!OutFromNode || !OutToNode)
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("node_not_found"));
				OutStructured->SetStringField(TEXT("from_node_guid"), FromNodeGuid);
				OutStructured->SetStringField(TEXT("to_node_guid"), ToNodeGuid);
				OutError = TEXT("Blueprint node was not found.");
				return false;
			}
			if (OutFromNode->GetGraph() != OutToNode->GetGraph())
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("nodes_in_different_graphs"));
				OutStructured->SetStringField(TEXT("from_graph"), OutFromNode->GetGraph() ? OutFromNode->GetGraph()->GetName() : FString());
				OutStructured->SetStringField(TEXT("to_graph"), OutToNode->GetGraph() ? OutToNode->GetGraph()->GetName() : FString());
				OutError = TEXT("Blueprint pins must belong to nodes in the same graph.");
				return false;
			}

			FString FromPinGuid;
			FString ToPinGuid;
			Arguments->TryGetStringField(TEXT("from_pin_guid"), FromPinGuid);
			Arguments->TryGetStringField(TEXT("to_pin_guid"), ToPinGuid);
			OutFromPin = FindNodePinLocal(OutFromNode, FromPinName, FromPinGuid, EGPD_Output);
			OutToPin = FindNodePinLocal(OutToNode, ToPinName, ToPinGuid, EGPD_Input);
			if (!OutFromPin || !OutToPin)
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("pin_not_found"));
				if (!OutFromPin)
				{
					OutStructured->SetObjectField(TEXT("from_pin_diagnostic"), MakePinFailureDiagnostic(OutFromNode, FromPinName, FromPinGuid, EGPD_Output));
				}
				if (!OutToPin)
				{
					OutStructured->SetObjectField(TEXT("to_pin_diagnostic"), MakePinFailureDiagnostic(OutToNode, ToPinName, ToPinGuid, EGPD_Input));
				}
				OutError = TEXT("Blueprint pin was not found.");
				return false;
			}
			return true;
		}

		bool TryAddFunctionCallNode(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			const FString& DefaultClassPath,
			const FString& DefaultFunctionName,
			const FText& TransactionText,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString AssetPath;
			FString GraphName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
			{
				OutError = TEXT("Missing asset_path or graph_name.");
				return false;
			}

			FString ClassPath = DefaultClassPath;
			FString FunctionName = DefaultFunctionName;
			Arguments->TryGetStringField(TEXT("class_path"), ClassPath);
			Arguments->TryGetStringField(TEXT("function_name"), FunctionName);
			if (ClassPath.IsEmpty() || FunctionName.IsEmpty())
			{
				OutError = TEXT("Missing class_path or function_name.");
				return false;
			}

			UBlueprint* Blueprint = nullptr;
			UEdGraph* Graph = nullptr;
			if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
			{
				return false;
			}

			UClass* OwnerClass = Context.Services.ResolveClass(ClassPath, OutError);
			if (!OwnerClass)
			{
				return false;
			}
			UFunction* Function = OwnerClass->FindFunctionByName(*FunctionName);
			if (!Function)
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("function_not_found"));
				OutStructured->SetStringField(TEXT("class_path"), OwnerClass->GetPathName());
				OutStructured->SetStringField(TEXT("function_name"), FunctionName);
				OutError = TEXT("Function was not found on class.");
				return false;
			}

			const FScopedTransaction Transaction(TransactionText);
			Blueprint->Modify();
			UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(Function);
			UEdGraphNode* Node = Spawner ? Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(GetBlueprintNodeLocationFromArguments(Arguments))) : nullptr;
			if (!Node || !VerifyGraphContainsNode(Graph, Node))
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("node_spawn_failed"));
				OutError = TEXT("Failed to spawn function call node.");
				return false;
			}

			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (CallNode->GetTargetFunction() != Function)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("function_binding_mismatch"));
					OutStructured->SetStringField(TEXT("expected_function"), Function->GetPathName());
					OutStructured->SetStringField(TEXT("actual_function"), CallNode->GetTargetFunction() ? CallNode->GetTargetFunction()->GetPathName() : FString());
					OutError = TEXT("Function call node failed binding verification.");
					return false;
				}
			}

			FString InString;
			if (FunctionName == TEXT("PrintString") && Arguments->TryGetStringField(TEXT("in_string"), InString))
			{
				if (UEdGraphPin* InStringPin = FindNodePinLocal(Node, TEXT("InString"), FString(), EGPD_Input))
				{
					const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
					if (Schema)
					{
						Schema->TrySetDefaultValue(*InStringPin, InString, true);
					}
				}
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			Graph->NotifyGraphChanged();

			OutStructured = BlueprintNodeToJson(Node);
			OutStructured->SetStringField(TEXT("class_path"), OwnerClass->GetPathName());
			OutStructured->SetStringField(TEXT("function_name"), FunctionName);
			OutStructured->SetBoolField(TEXT("node_verified"), true);
			if (FunctionName == TEXT("PrintString"))
			{
					OutStructured->SetBoolField(TEXT("print_string_data_pin_present"), FindNodePinLocal(Node, TEXT("InString"), FString(), EGPD_Input) != nullptr);
			}
			AttachBlueprintGraphEditReceipt(
				OutStructured,
				Blueprint,
				Graph,
				TEXT("add_function_call_node"),
				TEXT("node_verified"),
				true);
			if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
			{
				return false;
			}
			OutSummary = FString::Printf(TEXT("Added blueprint function call node %s.%s."), *OwnerClass->GetName(), *FunctionName);
			return true;
		}

		TSharedRef<FJsonObject> BuildBlueprintCompileReceipt(UBlueprint* Blueprint)
		{
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			if (!Blueprint)
			{
				return Receipt;
			}
			Receipt->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
			Receipt->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
			Receipt->SetBoolField(TEXT("has_generated_class"), Blueprint->GeneratedClass != nullptr);
			Receipt->SetBoolField(TEXT("has_skeleton_class"), Blueprint->SkeletonGeneratedClass != nullptr);
			Receipt->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());

			TArray<TSharedPtr<FJsonValue>> Graphs;
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			int32 TotalNodes = 0;
			int32 TotalLinkedPins = 0;
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
				int32 LinkedPins = 0;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node)
					{
						continue;
					}
					++TotalNodes;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin)
						{
							LinkedPins += Pin->LinkedTo.Num();
						}
					}
				}
				TotalLinkedPins += LinkedPins;
				GraphJson->SetStringField(TEXT("name"), Graph->GetName());
				GraphJson->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
				GraphJson->SetNumberField(TEXT("linked_pin_refs"), LinkedPins);
				Graphs.Add(MakeShared<FJsonValueObject>(GraphJson));
			}
			Receipt->SetNumberField(TEXT("graph_count"), Graphs.Num());
			Receipt->SetNumberField(TEXT("node_count"), TotalNodes);
			Receipt->SetNumberField(TEXT("linked_pin_refs"), TotalLinkedPins);
			Receipt->SetArrayField(TEXT("graphs"), Graphs);
			return Receipt;
		}

		FString PinTypeToBriefString(const FEdGraphPinType& PinType)
		{
			FString Brief = PinType.PinCategory.ToString();
			if (!PinType.PinSubCategory.IsNone())
			{
				Brief += TEXT(":");
				Brief += PinType.PinSubCategory.ToString();
			}
			if (PinType.PinSubCategoryObject.IsValid())
			{
				Brief += TEXT(" ");
				Brief += PinType.PinSubCategoryObject->GetPathName();
			}
			if (PinType.ContainerType != EPinContainerType::None)
			{
				Brief += FString::Printf(TEXT(" container=%d"), static_cast<int32>(PinType.ContainerType));
			}
			return Brief;
		}

		bool BlueprintHasMemberVariable(UBlueprint* Blueprint, const FName VariableName, FBPVariableDescription* OutVariable = nullptr)
		{
			if (!Blueprint || VariableName.IsNone())
			{
				return false;
			}
			for (FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				if (Variable.VarName == VariableName)
				{
					if (OutVariable)
					{
						*OutVariable = Variable;
					}
					return true;
				}
			}
			return false;
		}

		UObject* ResolveBlueprintPinTypeObject(const FString& ObjectPath)
		{
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}
			return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		}

		bool TrySplitBlueprintGenericInner(const FString& TypeName, const FString& Prefix, FString& OutInner)
		{
			const FString Normalized = TypeName.TrimStartAndEnd();
			if (!Normalized.StartsWith(Prefix, ESearchCase::IgnoreCase) || !Normalized.EndsWith(TEXT(">")))
			{
				return false;
			}
			OutInner = Normalized.Mid(Prefix.Len(), Normalized.Len() - Prefix.Len() - 1).TrimStartAndEnd();
			return !OutInner.IsEmpty();
		}

		bool TrySplitBlueprintMapTypes(const FString& TypeName, FString& OutKey, FString& OutValue)
		{
			FString Inner;
			if (!TrySplitBlueprintGenericInner(TypeName, TEXT("map<"), Inner))
			{
				return false;
			}
			int32 Depth = 0;
			for (int32 Index = 0; Index < Inner.Len(); ++Index)
			{
				const TCHAR Ch = Inner[Index];
				if (Ch == TCHAR('<')) { ++Depth; }
				else if (Ch == TCHAR('>')) { --Depth; }
				else if (Ch == TCHAR(',') && Depth == 0)
				{
					OutKey = Inner.Left(Index).TrimStartAndEnd();
					OutValue = Inner.Mid(Index + 1).TrimStartAndEnd();
					return !OutKey.IsEmpty() && !OutValue.IsEmpty();
				}
			}
			return false;
		}

		bool MakeRepairPinType(const FString& TypeName, const FString& TypeObjectPath, FEdGraphPinType& OutPinType, FString& OutError)
		{
			OutPinType = FEdGraphPinType();
			OutPinType.ContainerType = EPinContainerType::None;
			OutPinType.bIsReference = false;

			const FString Normalized = TypeName.TrimStartAndEnd();
			const FString Lower = Normalized.ToLower();
			FString InnerType;
			if (TrySplitBlueprintGenericInner(Normalized, TEXT("array<"), InnerType) ||
				TrySplitBlueprintGenericInner(Normalized, TEXT("set<"), InnerType))
			{
				FEdGraphPinType InnerPinType;
				if (!MakeRepairPinType(InnerType, FString(), InnerPinType, OutError))
				{
					return false;
				}
				OutPinType = InnerPinType;
				OutPinType.ContainerType = Lower.StartsWith(TEXT("array<")) ? EPinContainerType::Array : EPinContainerType::Set;
				return true;
			}

			FString KeyTypeName;
			FString ValueTypeName;
			if (TrySplitBlueprintMapTypes(Normalized, KeyTypeName, ValueTypeName))
			{
				FEdGraphPinType KeyType;
				FEdGraphPinType ValueType;
				if (!MakeRepairPinType(KeyTypeName, FString(), KeyType, OutError) ||
					!MakeRepairPinType(ValueTypeName, FString(), ValueType, OutError))
				{
					return false;
				}
				OutPinType = KeyType;
				OutPinType.ContainerType = EPinContainerType::Map;
				OutPinType.PinValueType.TerminalCategory = ValueType.PinCategory;
				OutPinType.PinValueType.TerminalSubCategory = ValueType.PinSubCategory;
				OutPinType.PinValueType.TerminalSubCategoryObject = ValueType.PinSubCategoryObject;
				return true;
			}
			if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
				return true;
			}
			if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
				return true;
			}
			if (Lower == TEXT("int64"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
				return true;
			}
			if (Lower == TEXT("float") || Lower == TEXT("real") || Lower == TEXT("double"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
				OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
				return true;
			}
			if (Lower == TEXT("string"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
				return true;
			}
			if (Lower == TEXT("name"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
				return true;
			}
			if (Lower == TEXT("text"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
				return true;
			}
			if (Lower == TEXT("vector"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
				return true;
			}
			if (Lower == TEXT("rotator"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
				return true;
			}
			if (Lower == TEXT("transform"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
				return true;
			}
			if (Lower == TEXT("datatable") || Lower == TEXT("data_table"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = UDataTable::StaticClass();
				return true;
			}
			if (Lower == TEXT("texture2d") || Lower == TEXT("texture"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = UTexture2D::StaticClass();
				return true;
			}
			if (Lower == TEXT("render_target") || Lower == TEXT("rendertarget2d") || Lower == TEXT("texture_render_target_2d"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = UTextureRenderTarget2D::StaticClass();
				return true;
			}
			if (Lower == TEXT("material") || Lower == TEXT("material_interface"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				OutPinType.PinSubCategoryObject = UMaterialInterface::StaticClass();
				return true;
			}

			FString GenericObjectPath;
			if (TrySplitBlueprintGenericInner(Normalized, TEXT("object<"), GenericObjectPath))
			{
				return MakeRepairPinType(TEXT("object"), GenericObjectPath, OutPinType, OutError);
			}
			if (TrySplitBlueprintGenericInner(Normalized, TEXT("class<"), GenericObjectPath))
			{
				return MakeRepairPinType(TEXT("class"), GenericObjectPath, OutPinType, OutError);
			}
			if (TrySplitBlueprintGenericInner(Normalized, TEXT("struct<"), GenericObjectPath))
			{
				return MakeRepairPinType(TEXT("struct"), GenericObjectPath, OutPinType, OutError);
			}
			if (TrySplitBlueprintGenericInner(Normalized, TEXT("enum<"), GenericObjectPath))
			{
				return MakeRepairPinType(TEXT("enum"), GenericObjectPath, OutPinType, OutError);
			}
			if (TrySplitBlueprintGenericInner(Normalized, TEXT("softobject<"), GenericObjectPath) || Lower == TEXT("soft_object"))
			{
				const FString Path = GenericObjectPath.IsEmpty() ? TypeObjectPath : GenericObjectPath;
				if (UClass* Class = LoadObject<UClass>(nullptr, *Path))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
					OutPinType.PinSubCategoryObject = Class;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve soft object class '%s'."), *Path);
				return false;
			}
			GenericObjectPath.Reset();
			if (TrySplitBlueprintGenericInner(Normalized, TEXT("softclass<"), GenericObjectPath) || Lower == TEXT("soft_class"))
			{
				const FString Path = GenericObjectPath.IsEmpty() ? TypeObjectPath : GenericObjectPath;
				if (UClass* Class = LoadObject<UClass>(nullptr, *Path))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
					OutPinType.PinSubCategoryObject = Class;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve soft class '%s'."), *Path);
				return false;
			}

			UObject* TypeObject = ResolveBlueprintPinTypeObject(TypeObjectPath);
			if (Lower == TEXT("struct"))
			{
				if (UScriptStruct* Struct = Cast<UScriptStruct>(TypeObject))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					OutPinType.PinSubCategoryObject = Struct;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve struct type object '%s'."), *TypeObjectPath);
				return false;
			}
			if (Lower == TEXT("object"))
			{
				if (UClass* Class = Cast<UClass>(TypeObject))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
					OutPinType.PinSubCategoryObject = Class;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve object class '%s'."), *TypeObjectPath);
				return false;
			}
			if (Lower == TEXT("class"))
			{
				if (UClass* Class = Cast<UClass>(TypeObject))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
					OutPinType.PinSubCategoryObject = Class;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve class type '%s'."), *TypeObjectPath);
				return false;
			}
			if (Lower == TEXT("enum"))
			{
				if (UEnum* Enum = Cast<UEnum>(TypeObject))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
					OutPinType.PinSubCategoryObject = Enum;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve enum type '%s'."), *TypeObjectPath);
				return false;
			}
			if (Lower == TEXT("interface"))
			{
				if (UClass* Class = Cast<UClass>(TypeObject))
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
					OutPinType.PinSubCategoryObject = Class;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve interface class '%s'."), *TypeObjectPath);
				return false;
			}
			if (Lower == TEXT("delegate") || Lower == TEXT("multicast_delegate"))
			{
				if (UFunction* Signature = Cast<UFunction>(TypeObject))
				{
					OutPinType.PinCategory = Lower == TEXT("delegate") ? UEdGraphSchema_K2::PC_Delegate : UEdGraphSchema_K2::PC_MCDelegate;
					OutPinType.PinSubCategoryObject = Signature;
					return true;
				}
				OutError = FString::Printf(TEXT("Failed to resolve delegate signature function '%s'."), *TypeObjectPath);
				return false;
			}
			if (Lower == TEXT("wildcard"))
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
				return true;
			}

			OutError = FString::Printf(TEXT("Unsupported repair variable type '%s'."), *TypeName);
			return false;
		}

		bool IsPinTypeObjectRequiredButMissing(const FEdGraphPinType& PinType)
		{
			const FName Category = PinType.PinCategory;
			const bool bRequiresObject =
				Category == UEdGraphSchema_K2::PC_Struct ||
				Category == UEdGraphSchema_K2::PC_Object ||
				Category == UEdGraphSchema_K2::PC_Class ||
				Category == UEdGraphSchema_K2::PC_Interface ||
				Category == UEdGraphSchema_K2::PC_SoftObject ||
				Category == UEdGraphSchema_K2::PC_SoftClass;
			return bRequiresObject && !PinType.PinSubCategoryObject.IsValid();
		}

		bool IsSimpleBlueprintIdentifier(const FString& Name)
		{
			if (Name.IsEmpty())
			{
				return false;
			}
			const TCHAR First = Name[0];
			if (!(FChar::IsAlpha(First) || First == TEXT('_')))
			{
				return false;
			}
			for (int32 Index = 1; Index < Name.Len(); ++Index)
			{
				const TCHAR Ch = Name[Index];
				if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_')))
				{
					return false;
				}
			}
			return true;
		}

		bool IsReservedBlueprintGraphNameForFunctionPlan(const FString& Name)
		{
			return Name.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) ||
				Name.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase) ||
				Name.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase) ||
				Name.StartsWith(TEXT("UberGraph"), ESearchCase::IgnoreCase);
		}

		void AddBlueprintContractFinding(
			TArray<TSharedPtr<FJsonValue>>& Findings,
			const FString& Severity,
			const FString& Code,
			const FString& Field,
			const FString& Detail)
		{
			TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
			Finding->SetStringField(TEXT("severity"), Severity);
			Finding->SetStringField(TEXT("code"), Code);
			Finding->SetStringField(TEXT("field"), Field);
			Finding->SetStringField(TEXT("detail"), Detail);
			Findings.Add(MakeShared<FJsonValueObject>(Finding));
		}

		bool ValidateBlueprintContractType(
			const TSharedPtr<FJsonObject>& Item,
			const FString& FieldPrefix,
			TArray<TSharedPtr<FJsonValue>>& Findings,
			const FString& TypeField = TEXT("type"),
			const FString& TypeObjectPathField = TEXT("type_object_path"))
		{
			if (!Item.IsValid())
			{
				AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_contract_item"), FieldPrefix, TEXT("Contract item is not an object."));
				return false;
			}

			FString TypeName;
			if (!Item->TryGetStringField(TypeField, TypeName) || TypeName.TrimStartAndEnd().IsEmpty())
			{
				AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("missing_type"), FieldPrefix + TEXT(".") + TypeField, TEXT("Blueprint creation contract item must declare an explicit type."));
				return false;
			}

			FString TypeObjectPath;
			Item->TryGetStringField(TypeObjectPathField, TypeObjectPath);
			FEdGraphPinType PinType;
			FString TypeError;
			if (!MakeRepairPinType(TypeName.TrimStartAndEnd(), TypeObjectPath.TrimStartAndEnd(), PinType, TypeError))
			{
				AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("unresolved_type"), FieldPrefix, TypeError);
				return false;
			}
			if (IsPinTypeObjectRequiredButMissing(PinType))
			{
				AddBlueprintContractFinding(
					Findings,
					TEXT("error"),
					TEXT("type_object_missing"),
					FieldPrefix + TEXT(".") + TypeObjectPathField,
					FString::Printf(TEXT("Type '%s' resolves to %s but has no type object."), *TypeName, *PinTypeToBriefString(PinType)));
				return false;
			}
			return true;
		}

		void ValidateBlueprintContractNamedArray(
			const TSharedRef<FJsonObject>& Arguments,
			const FString& FieldName,
			TSet<FString>& Names,
			TArray<TSharedPtr<FJsonValue>>& Findings,
			const bool bValidateType = false)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Arguments->TryGetArrayField(FieldName, Items) || !Items)
			{
				return;
			}

			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& ItemValue = (*Items)[Index];
				const TSharedPtr<FJsonObject>* ItemObjPtr = nullptr;
				const FString Prefix = FString::Printf(TEXT("%s[%d]"), *FieldName, Index);
				if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObjPtr) || !ItemObjPtr || !ItemObjPtr->IsValid())
				{
					AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_contract_item"), Prefix, TEXT("Expected an object."));
					continue;
				}
				const TSharedPtr<FJsonObject> Item = *ItemObjPtr;

				FString Name;
				Item->TryGetStringField(TEXT("name"), Name);
				if (!IsSimpleBlueprintIdentifier(Name))
				{
					AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_identifier"), Prefix + TEXT(".name"), TEXT("Name must be a non-empty Blueprint identifier: [A-Za-z_][A-Za-z0-9_]*."));
				}
				if (!Name.IsEmpty())
				{
					if (Names.Contains(Name))
					{
						AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("duplicate_name"), Prefix + TEXT(".name"), FString::Printf(TEXT("Duplicate Blueprint member name '%s'."), *Name));
					}
					Names.Add(Name);
				}
				if (FieldName.Equals(TEXT("functions"), ESearchCase::IgnoreCase) && IsReservedBlueprintGraphNameForFunctionPlan(Name))
				{
					AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("reserved_function_graph_name"), Prefix + TEXT(".name"), FString::Printf(TEXT("'%s' is not a user function graph name."), *Name));
				}
				if (bValidateType)
				{
					ValidateBlueprintContractType(Item, Prefix, Findings);
				}
			}
		}

		void AddMigrationIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& IssueClass,
			UEdGraphNode* Node,
			const UEdGraphPin* Pin,
			const FString& Detail)
		{
			TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), Severity);
			Issue->SetStringField(TEXT("class"), IssueClass);
			Issue->SetStringField(TEXT("detail"), Detail);
			if (Node)
			{
				Issue->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
				Issue->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
				Issue->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Issue->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
			}
			if (Pin)
			{
				Issue->SetObjectField(TEXT("pin"), MakePinRefJson(Pin));
			}
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		int32 ScanBlueprintMigrationIssues(UBlueprint* Blueprint, TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			if (!Blueprint)
			{
				return 0;
			}

			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node)
					{
						continue;
					}

					const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					if (NodeTitle.Contains(TEXT("unknown struct"), ESearchCase::IgnoreCase) ||
						NodeTitle.Contains(TEXT("<unknown"), ESearchCase::IgnoreCase))
					{
						AddMigrationIssue(Issues, TEXT("error"), TEXT("stale_unknown_struct_node"), Node, nullptr, NodeTitle);
					}

					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin)
						{
							continue;
						}
						if (IsPinTypeObjectRequiredButMissing(Pin->PinType))
						{
							AddMigrationIssue(
								Issues,
								TEXT("error"),
								TEXT("stale_unknown_type_pin"),
								Node,
								Pin,
								FString::Printf(TEXT("Pin type requires an object but none is resolved: %s"), *PinTypeToBriefString(Pin->PinType)));
						}
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (!LinkedPin)
							{
								AddMigrationIssue(Issues, TEXT("error"), TEXT("stale_null_link"), Node, Pin, TEXT("Pin contains a null linked pin reference."));
								continue;
							}
							if (!LinkedPin->LinkedTo.Contains(Pin))
							{
								AddMigrationIssue(Issues, TEXT("warning"), TEXT("one_way_pin_link"), Node, Pin, TEXT("Linked pin does not reference this pin on readback."));
							}
						}
					}
				}
			}
			return Issues.Num();
		}

		FString CompilerSeverityToString(const EMessageSeverity::Type Severity)
		{
			switch (Severity)
			{
			case EMessageSeverity::Error:
				return TEXT("error");
			case EMessageSeverity::PerformanceWarning:
				return TEXT("performance_warning");
			case EMessageSeverity::Warning:
				return TEXT("warning");
			case EMessageSeverity::Info:
			default:
				return TEXT("info");
			}
		}

		FString ClassifyCompilerMessage(const FString& MessageText)
		{
			const FString Lower = MessageText.ToLower();
			if (Lower.Contains(TEXT("ui style")) || Lower.Contains(TEXT("uistyle")) || Lower.Contains(TEXT("widget style")) || Lower.Contains(TEXT("slate widget style")))
			{
				return TEXT("ui_style_bad_struct");
			}
			if (Lower.Contains(TEXT("data table")) || Lower.Contains(TEXT("datatable")) || Lower.Contains(TEXT("getdatatablerow")))
			{
				return TEXT("datatable_row");
			}
			if (Lower.Contains(TEXT("unknown structure")) || Lower.Contains(TEXT("bad or unknown structure")) || Lower.Contains(TEXT("unknown struct")))
			{
				return TEXT("bad_struct_pin");
			}
			if (Lower.Contains(TEXT("return node")) || Lower.Contains(TEXT("returnnode")) || Lower.Contains(TEXT("function result")))
			{
				return TEXT("function_return");
			}
			if (Lower.Contains(TEXT("pin")) && (Lower.Contains(TEXT("link")) || Lower.Contains(TEXT("connect"))))
			{
				return TEXT("pin_link");
			}
			if (Lower.Contains(TEXT("deprecated")) || Lower.Contains(TEXT("refresh")))
			{
				return TEXT("stale_node");
			}
			return TEXT("compiler_message");
		}

		TSharedRef<FJsonObject> CompilerMessageToJson(const TSharedRef<FTokenizedMessage>& Message)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const FString Text = Message->ToText().ToString();
			Result->SetStringField(TEXT("severity"), CompilerSeverityToString(Message->GetSeverity()));
			Result->SetStringField(TEXT("text"), Text);
			Result->SetStringField(TEXT("message"), Text);
			Result->SetStringField(TEXT("classification"), ClassifyCompilerMessage(Text));
			Result->SetStringField(TEXT("identifier"), Message->GetIdentifier().ToString());
			return Result;
		}

		UEdGraphPin* FindExecutionPinLocal(UEdGraphNode* Node, const EEdGraphPinDirection Direction)
		{
			if (!Node)
			{
				return nullptr;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (IsExecPin(Pin) && Pin->Direction == Direction)
				{
					return Pin;
				}
			}
			return nullptr;
		}

		bool IsGetDataTableRowNode(const UEdGraphNode* Node)
		{
			if (!Node)
			{
				return false;
			}
			if (Cast<UK2Node_GetDataTableRow>(Node))
			{
				return true;
			}
			const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();
			const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			return ClassName.Contains(TEXT("GetDataTableRow"), ESearchCase::IgnoreCase) ||
				Title.Contains(TEXT("Get Data Table Row"), ESearchCase::IgnoreCase) ||
				Title.Contains(TEXT("GetDataTableRow"), ESearchCase::IgnoreCase) ||
				Title.Contains(TEXT("获得数据表行"), ESearchCase::IgnoreCase);
		}

		UEdGraphPin* FindGetDataTableRowDataTablePin(UEdGraphNode* Node)
		{
			if (!Node)
			{
				return nullptr;
			}
			if (UK2Node_GetDataTableRow* RowNode = Cast<UK2Node_GetDataTableRow>(Node))
			{
				if (UEdGraphPin* Pin = RowNode->GetDataTablePin())
				{
					return Pin;
				}
			}
			if (UEdGraphPin* Pin = FindNodePinLocal(Node, TEXT("DataTable"), FString(), EGPD_Input))
			{
				return Pin;
			}
			return FindNodePinLocal(Node, TEXT("Data Table"), FString(), EGPD_Input);
		}

		UEdGraphPin* FindGetDataTableRowRowNamePin(UEdGraphNode* Node)
		{
			if (!Node)
			{
				return nullptr;
			}
			if (UK2Node_GetDataTableRow* RowNode = Cast<UK2Node_GetDataTableRow>(Node))
			{
				if (UEdGraphPin* Pin = RowNode->GetRowNamePin())
				{
					return Pin;
				}
			}
			if (UEdGraphPin* Pin = FindNodePinLocal(Node, TEXT("RowName"), FString(), EGPD_Input))
			{
				return Pin;
			}
			return FindNodePinLocal(Node, TEXT("Row Name"), FString(), EGPD_Input);
		}

		UEdGraphPin* FindGetDataTableRowResultPin(UEdGraphNode* Node)
		{
			if (!Node)
			{
				return nullptr;
			}
			if (UK2Node_GetDataTableRow* RowNode = Cast<UK2Node_GetDataTableRow>(Node))
			{
				if (UEdGraphPin* Pin = RowNode->GetResultPin())
				{
					return Pin;
				}
			}
			if (UEdGraphPin* Pin = FindNodePinLocal(Node, TEXT("ReturnValue"), FString(), EGPD_Output))
			{
				return Pin;
			}
			return FindNodePinLocal(Node, TEXT("Out Row"), FString(), EGPD_Output);
		}

		UObject* LoadObjectAssetForBlueprintTool(FSololmcpEditorServices& Services, const FString& ObjectPath, FString& OutError)
		{
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}
			FString LoadError;
			if (UObject* Asset = Services.LoadAsset(ObjectPath, LoadError))
			{
				return Asset;
			}
			if (UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
			{
				OutError.Reset();
				return Asset;
			}
			OutError = LoadError.IsEmpty()
				? FString::Printf(TEXT("Failed to load asset object '%s'."), *ObjectPath)
				: LoadError;
			return nullptr;
		}

		TSharedRef<FJsonObject> BuildGetDataTableRowDiagnosticJson(UEdGraphNode* Node)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Node)
			{
				return Result;
			}
			Result->SetObjectField(TEXT("node"), BlueprintNodeToJson(Node));
			Result->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());

			UEdGraphPin* DataTablePin = FindGetDataTableRowDataTablePin(Node);
			UEdGraphPin* RowNamePin = FindGetDataTableRowRowNamePin(Node);
			UEdGraphPin* ResultPin = FindGetDataTableRowResultPin(Node);
			Result->SetObjectField(TEXT("data_table_pin"), MakePinRefJson(DataTablePin));
			Result->SetObjectField(TEXT("row_name_pin"), MakePinRefJson(RowNamePin));
			Result->SetObjectField(TEXT("return_value_pin"), MakePinRefJson(ResultPin));
			Result->SetStringField(TEXT("data_table_object"), DataTablePin && DataTablePin->DefaultObject ? DataTablePin->DefaultObject->GetPathName() : FString());
			Result->SetStringField(TEXT("row_name"), RowNamePin ? RowNamePin->DefaultValue : FString());
			Result->SetStringField(TEXT("return_struct"), ResultPin && ResultPin->PinType.PinSubCategoryObject.IsValid() ? ResultPin->PinType.PinSubCategoryObject->GetPathName() : FString());
			Result->SetBoolField(TEXT("data_table_missing"), DataTablePin && DataTablePin->LinkedTo.Num() == 0 && DataTablePin->DefaultObject == nullptr);
			return Result;
		}

		int32 CountGraphExecLinks(UEdGraph* Graph)
		{
			int32 Count = 0;
			if (!Graph)
			{
				return Count;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (IsExecPin(Pin) && Pin->Direction == EGPD_Output)
					{
						Count += Pin->LinkedTo.Num();
					}
				}
			}
			return Count;
		}

		UK2Node_FunctionEntry* FindPrimaryFunctionEntryNode(UEdGraph* Graph)
		{
			if (!Graph)
			{
				return nullptr;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					return Entry;
				}
			}
			return nullptr;
		}

		void CollectFunctionResultNodes(UEdGraph* Graph, TArray<UK2Node_FunctionResult*>& OutResults)
		{
			OutResults.Reset();
			if (!Graph)
			{
				return;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
				{
					OutResults.Add(ResultNode);
				}
			}
		}

		bool IsBlueprintFunctionGraphRequiringTerminator(const UBlueprint* Blueprint, const UEdGraph* Graph)
		{
			if (!Blueprint || !Graph)
			{
				return false;
			}
			const FString GraphName = Graph->GetName();
			if (GraphName.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase) ||
				GraphName.Equals(TEXT("ConstructionScript"), ESearchCase::IgnoreCase) ||
				GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) ||
				GraphName.StartsWith(TEXT("UberGraph"), ESearchCase::IgnoreCase))
			{
				return false;
			}
			for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
			{
				if (FunctionGraph == Graph)
				{
					return true;
				}
			}
			return false;
		}

		TSharedRef<FJsonObject> BuildFunctionGraphTerminatorDiagnostic(UEdGraph* Graph)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Graph)
			{
				return Result;
			}
			UK2Node_FunctionEntry* EntryNode = FindPrimaryFunctionEntryNode(Graph);
			TArray<UK2Node_FunctionResult*> ResultNodes;
			CollectFunctionResultNodes(Graph, ResultNodes);
			UEdGraphPin* EntryExec = FindExecutionPinLocal(EntryNode, EGPD_Output);

			Result->SetStringField(TEXT("graph"), Graph->GetName());
			Result->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			Result->SetNumberField(TEXT("exec_link_count"), CountGraphExecLinks(Graph));
			Result->SetStringField(TEXT("entry_node_guid"), EntryNode ? EntryNode->NodeGuid.ToString() : FString());
			Result->SetNumberField(TEXT("result_node_count"), ResultNodes.Num());
			Result->SetNumberField(TEXT("entry_exec_link_count"), EntryExec ? EntryExec->LinkedTo.Num() : 0);
			Result->SetBoolField(TEXT("has_function_entry"), EntryNode != nullptr);
			Result->SetBoolField(TEXT("has_function_result"), ResultNodes.Num() > 0);
			Result->SetBoolField(TEXT("missing_terminator"), EntryNode && EntryExec && EntryExec->LinkedTo.Num() == 0 && ResultNodes.Num() == 0);
			return Result;
		}

		void AddBlueprintRepairIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& IssueClass,
			UEdGraphNode* Node,
			UEdGraphPin* Pin,
			const FString& Detail,
			const FString& RecommendedTool,
			const bool bCanAutoFix)
		{
			TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), Severity);
			Issue->SetStringField(TEXT("class"), IssueClass);
			Issue->SetStringField(TEXT("detail"), Detail);
			Issue->SetStringField(TEXT("recommended_tool"), RecommendedTool);
			Issue->SetBoolField(TEXT("can_autofix"), bCanAutoFix);
			if (Node)
			{
				Issue->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
				Issue->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
				Issue->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				Issue->SetStringField(TEXT("node_class"), Node->GetClass() ? Node->GetClass()->GetPathName() : FString());
			}
			if (Pin)
			{
				Issue->SetObjectField(TEXT("pin"), MakePinRefJson(Pin));
			}
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}

		bool IsSuspiciousStructObjectPath(const UObject* Object)
		{
			if (!Object)
			{
				return false;
			}
			const FString Path = Object->GetPathName();
			return Path.Contains(TEXT("REINST"), ESearchCase::IgnoreCase) ||
				Path.Contains(TEXT("SKEL_"), ESearchCase::IgnoreCase) ||
				Path.Contains(TEXT("TRASHCLASS"), ESearchCase::IgnoreCase) ||
				Path.Contains(TEXT("HOTRELOADED"), ESearchCase::IgnoreCase);
		}

		int32 ScanBlueprintRepairIssues(UBlueprint* Blueprint, TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			if (!Blueprint)
			{
				return 0;
			}

			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph)
				{
					continue;
				}

				if (IsBlueprintFunctionGraphRequiringTerminator(Blueprint, Graph))
				{
					TSharedRef<FJsonObject> FunctionDiag = BuildFunctionGraphTerminatorDiagnostic(Graph);
					bool bMissingTerminator = false;
					if (FunctionDiag->TryGetBoolField(TEXT("missing_terminator"), bMissingTerminator) && bMissingTerminator)
					{
						AddBlueprintRepairIssue(
							Issues,
							TEXT("warning"),
							TEXT("function_graph_missing_terminator"),
							FindPrimaryFunctionEntryNode(Graph),
							FindExecutionPinLocal(FindPrimaryFunctionEntryNode(Graph), EGPD_Output),
							TEXT("Function graph has a FunctionEntry exec output with no terminal ReturnNode/readback path."),
							TEXT("blueprint_finalize_function_graph"),
							true);
					}
				}

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node)
					{
						continue;
					}

					if (IsGetDataTableRowNode(Node))
					{
						UEdGraphPin* DataTablePin = FindGetDataTableRowDataTablePin(Node);
						if (!DataTablePin)
						{
							AddBlueprintRepairIssue(
								Issues,
								TEXT("error"),
								TEXT("getdatatable_missing_datatable_pin"),
								Node,
								nullptr,
								TEXT("GetDataTableRow node did not expose a DataTable input pin."),
								TEXT("blueprint_get_node_details"),
								false);
						}
						else if (DataTablePin->LinkedTo.Num() == 0 && DataTablePin->DefaultObject == nullptr)
						{
							AddBlueprintRepairIssue(
								Issues,
								TEXT("error"),
								TEXT("getdatatable_missing_table"),
								Node,
								DataTablePin,
								TEXT("GetDataTableRow DataTable pin is unlinked and has no default DataTable object."),
								TEXT("blueprint_set_getdatatable_node_table"),
								true);
						}
						else if (DataTablePin->LinkedTo.Num() > 0)
						{
							AddBlueprintRepairIssue(
								Issues,
								TEXT("info"),
								TEXT("getdatatable_linked_table_requires_upstream_validation"),
								Node,
								DataTablePin,
								TEXT("GetDataTableRow receives DataTable through a link; validate upstream RowHandle/struct pins when compiler still reports DataTableRow errors."),
								TEXT("blueprint_find_nodes"),
								false);
						}
					}

					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin)
						{
							continue;
						}
						if (IsPinTypeObjectRequiredButMissing(Pin->PinType))
						{
							AddBlueprintRepairIssue(
								Issues,
								TEXT("error"),
								TEXT("bad_struct_pin_missing_type_object"),
								Node,
								Pin,
								FString::Printf(TEXT("Pin requires a type object but none is resolved: %s"), *PinTypeToBriefString(Pin->PinType)),
								TEXT("blueprint_set_node_pin_type"),
								false);
						}
						else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && IsSuspiciousStructObjectPath(Pin->PinType.PinSubCategoryObject.Get()))
						{
							AddBlueprintRepairIssue(
								Issues,
								TEXT("warning"),
								TEXT("bad_struct_pin_stale_type_object"),
								Node,
								Pin,
								FString::Printf(TEXT("Pin points at a transient/stale struct object: %s"), *PinTypeToBriefString(Pin->PinType)),
								TEXT("blueprint_set_node_pin_type"),
								false);
						}
						else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
						{
							const FString PinTypeBrief = PinTypeToBriefString(Pin->PinType);
							const bool bLooksLikeUiStyle =
								PinTypeBrief.Contains(TEXT("UI Style"), ESearchCase::IgnoreCase) ||
								PinTypeBrief.Contains(TEXT("UIStyle"), ESearchCase::IgnoreCase) ||
								PinTypeBrief.Contains(TEXT("WidgetStyle"), ESearchCase::IgnoreCase) ||
								PinTypeBrief.Contains(TEXT("SlateWidgetStyle"), ESearchCase::IgnoreCase);
							const bool bLooksBad =
								PinTypeBrief.Contains(TEXT("unknown"), ESearchCase::IgnoreCase) ||
								PinTypeBrief.Contains(TEXT("REINST"), ESearchCase::IgnoreCase) ||
								PinTypeBrief.Contains(TEXT("SKEL_"), ESearchCase::IgnoreCase) ||
								PinTypeBrief.Contains(TEXT("TRASHCLASS"), ESearchCase::IgnoreCase) ||
								PinTypeBrief.Contains(TEXT("HOTRELOADED"), ESearchCase::IgnoreCase);
							if (bLooksLikeUiStyle && bLooksBad)
							{
								AddBlueprintRepairIssue(
									Issues,
									TEXT("error"),
									TEXT("ui_style_bad_struct_type"),
									Node,
									Pin,
									FString::Printf(TEXT("Pin references a UI Style struct type that must compile-clean before delivery: %s"), *PinTypeBrief),
									TEXT("blueprint_set_node_pin_type"),
									false);
							}
						}

						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (!LinkedPin)
							{
								continue;
							}
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
								LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
								Pin->PinType.PinSubCategoryObject.IsValid() &&
								LinkedPin->PinType.PinSubCategoryObject.IsValid() &&
								Pin->PinType.PinSubCategoryObject.Get() != LinkedPin->PinType.PinSubCategoryObject.Get())
							{
								AddBlueprintRepairIssue(
									Issues,
									TEXT("warning"),
									TEXT("bad_struct_pin_link_type_mismatch"),
									Node,
									Pin,
									FString::Printf(TEXT("Linked struct pins do not reference the same struct type: %s -> %s"),
										*PinTypeToBriefString(Pin->PinType),
										*PinTypeToBriefString(LinkedPin->PinType)),
									TEXT("blueprint_break_single_pin_link"),
									false);
							}
						}
					}
				}
			}
			return Issues.Num();
		}

		bool IsBlockingBlueprintRepairIssue(const TSharedPtr<FJsonValue>& IssueValue)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			if (!IssueValue.IsValid() || !IssueValue->TryGetObject(IssueObj) || !IssueObj || !IssueObj->IsValid())
			{
				return false;
			}

			FString Severity;
			FString IssueClass;
			(*IssueObj)->TryGetStringField(TEXT("severity"), Severity);
			(*IssueObj)->TryGetStringField(TEXT("class"), IssueClass);
			return Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase) ||
				IssueClass.Equals(TEXT("getdatatable_missing_table"), ESearchCase::IgnoreCase) ||
				IssueClass.Equals(TEXT("ui_style_bad_struct_type"), ESearchCase::IgnoreCase);
		}

		bool RunBlueprintGraphMutationGate(
			UBlueprint* Blueprint,
			UEdGraph* Graph,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutError)
		{
			if (!Blueprint)
			{
				OutError = TEXT("Blueprint mutation gate missing Blueprint.");
				return false;
			}

			Blueprint->Modify();
			FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			if (Graph)
			{
				Graph->NotifyGraphChanged();
			}

			FCompilerResultsLog ResultsLog;
			ResultsLog.bSilentMode = true;
			ResultsLog.bAnnotateMentionedNodes = true;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

			TArray<TSharedPtr<FJsonValue>> Messages;
			for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
			{
				Messages.Add(MakeShared<FJsonValueObject>(CompilerMessageToJson(Message)));
			}

			TArray<TSharedPtr<FJsonValue>> RepairIssues;
			ScanBlueprintRepairIssues(Blueprint, RepairIssues);
			int32 BlockingIssueCount = 0;
			for (const TSharedPtr<FJsonValue>& IssueValue : RepairIssues)
			{
				if (IsBlockingBlueprintRepairIssue(IssueValue))
				{
					++BlockingIssueCount;
				}
			}

			const bool bCompileOk = Blueprint->Status != BS_Error && ResultsLog.NumErrors == 0;
			const bool bGateOk = bCompileOk && BlockingIssueCount == 0;
			TSharedRef<FJsonObject> Gate = MakeShared<FJsonObject>();
			Gate->SetStringField(TEXT("schema"), TEXT("somol.blueprint_graph_mutation_gate.v1"));
			Gate->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
			Gate->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : FString());
			Gate->SetStringField(TEXT("refresh_status"), TEXT("attempted"));
			Gate->SetStringField(TEXT("compile_diagnostics_status"), bCompileOk ? TEXT("passed") : TEXT("failed"));
			Gate->SetBoolField(TEXT("compile_ok"), bCompileOk);
			Gate->SetNumberField(TEXT("compiler_error_count"), ResultsLog.NumErrors);
			Gate->SetNumberField(TEXT("compiler_warning_count"), ResultsLog.NumWarnings);
			Gate->SetArrayField(TEXT("compiler_messages"), Messages);
			Gate->SetArrayField(TEXT("repair_issues"), RepairIssues);
			Gate->SetNumberField(TEXT("repair_issue_count"), RepairIssues.Num());
			Gate->SetNumberField(TEXT("blocking_repair_issue_count"), BlockingIssueCount);
			Gate->SetStringField(TEXT("receipt_status"), bGateOk ? TEXT("completed") : TEXT("failed_validation"));
			Gate->SetStringField(TEXT("required_before_delivery"), TEXT("refresh_all_nodes + compile diagnostics + graph readback"));
			OutStructured->SetObjectField(TEXT("receipt_gate"), Gate);
			OutStructured->SetBoolField(TEXT("receipt_complete"), bGateOk);
			OutStructured->SetStringField(TEXT("receipt_status"), bGateOk ? TEXT("completed") : TEXT("failed_validation"));
			OutStructured->SetBoolField(TEXT("refresh_all_nodes_attempted"), true);
			OutStructured->SetBoolField(TEXT("compile_ok"), bCompileOk);
			OutStructured->SetNumberField(TEXT("compiler_error_count"), ResultsLog.NumErrors);
			OutStructured->SetNumberField(TEXT("blocking_repair_issue_count"), BlockingIssueCount);

			if (!bGateOk)
			{
				OutStructured->SetStringField(TEXT("diagnostic_code"), bCompileOk ? TEXT("blueprint_repair_gate_failed") : TEXT("blueprint_compile_failed"));
				OutStructured->SetStringField(TEXT("failure_class"), BlockingIssueCount > 0 ? TEXT("known_blueprint_delivery_blocker") : TEXT("compile_diagnostics_failed"));
				OutError = FString::Printf(
					TEXT("Blueprint mutation failed receipt gate after refresh/compile: compile_errors=%d blocking_repair_issues=%d."),
					ResultsLog.NumErrors,
					BlockingIssueCount);
				return false;
			}
			return true;
		}

		void AddNodeBindingDetails(TSharedRef<FJsonObject> NodeJson, UEdGraphNode* Node)
		{
			if (!Node)
			{
				return;
			}
			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				UFunction* Function = CallNode->GetTargetFunction();
				NodeJson->SetStringField(TEXT("target_function"), Function ? Function->GetPathName() : FString());
				NodeJson->SetStringField(TEXT("target_function_name"), Function ? Function->GetName() : FString());
				NodeJson->SetStringField(TEXT("target_owner_class"), Function && Function->GetOuter() ? Function->GetOuter()->GetPathName() : FString());
			}
			if (IsGetDataTableRowNode(Node))
			{
				NodeJson->SetObjectField(TEXT("getdatatable_diagnostic"), BuildGetDataTableRowDiagnosticJson(Node));
			}
		}

		TSharedRef<FJsonObject> BlueprintNodeDetailsToJson(UEdGraphNode* Node)
		{
			TSharedRef<FJsonObject> Result = BlueprintNodeToJson(Node);
			if (!Node)
			{
				return Result;
			}
			Result->SetStringField(TEXT("graph_name"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
			Result->SetNumberField(TEXT("exec_link_count_in_graph"), Node->GetGraph() ? CountGraphExecLinks(Node->GetGraph()) : 0);
			AddNodeBindingDetails(Result, Node);
			return Result;
		}

		bool NodeMatchesFindRequest(
			UEdGraphNode* Node,
			const FString& QueryLower,
			const FString& ClassNameLower,
			const FString& FunctionNameLower,
			const FString& PinNameLower)
		{
			if (!Node)
			{
				return false;
			}
			const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			const FString NodeClass = Node->GetClass() ? Node->GetClass()->GetPathName() : FString();
			bool bMatched = QueryLower.IsEmpty() ||
				NodeTitle.ToLower().Contains(QueryLower) ||
				Node->GetName().ToLower().Contains(QueryLower) ||
				NodeClass.ToLower().Contains(QueryLower) ||
				Node->NodeComment.ToLower().Contains(QueryLower);

			if (!ClassNameLower.IsEmpty())
			{
				bMatched = bMatched && NodeClass.ToLower().Contains(ClassNameLower);
			}

			if (!FunctionNameLower.IsEmpty())
			{
				const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
				const UFunction* Function = CallNode ? CallNode->GetTargetFunction() : nullptr;
				const FString FunctionName = Function ? Function->GetName().ToLower() : FString();
				const FString FunctionPath = Function ? Function->GetPathName().ToLower() : FString();
				bMatched = bMatched && (FunctionName.Contains(FunctionNameLower) || FunctionPath.Contains(FunctionNameLower) || NodeTitle.ToLower().Contains(FunctionNameLower));
			}

			if (!PinNameLower.IsEmpty())
			{
				bool bPinMatched = false;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin &&
						(Pin->PinName.ToString().ToLower().Contains(PinNameLower) ||
						 Pin->GetDisplayName().ToString().ToLower().Contains(PinNameLower)))
					{
						bPinMatched = true;
						break;
					}
				}
				bMatched = bMatched && bPinMatched;
			}
			return bMatched;
		}
	} // namespace

	void RegisterBlueprintFlowTools(FSololmcpToolRegistry& Registry)
	{
		Registry.Register({
			TEXT("blueprint_action_catalog"),
			TEXT("Query Unreal's Blueprint Action Database for graph-compatible node actions. Returns stable action ids, node classes, owners, menu metadata, and template pin schemas so any public K2 action can be planned without guessing node names."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Blueprint asset used as compatibility context."))},
				{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Optional graph name; required when asset_path is supplied for strict compatibility filtering."))},
				{TEXT("query"), FSololmcpSchemaBuilder::String(TEXT("Optional case-insensitive filter over menu name, category, keywords, owner, and node class."))},
				{TEXT("offset"), FSololmcpSchemaBuilder::Integer(TEXT("Result offset; default 0."))},
				{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum results; default 100, hard cap 500."))},
				{TEXT("include_pin_schema"), FSololmcpSchemaBuilder::Boolean(TEXT("Include template node pins; default true."))}
			}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString Query;
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				Arguments->TryGetStringField(TEXT("query"), Query);
				const FString QueryLower = Query.TrimStartAndEnd().ToLower();
				const int32 Offset = Arguments->HasTypedField<EJson::Number>(TEXT("offset")) ? FMath::Max(0, Arguments->GetIntegerField(TEXT("offset"))) : 0;
				const int32 MaxResults = Arguments->HasTypedField<EJson::Number>(TEXT("max_results")) ? FMath::Clamp(Arguments->GetIntegerField(TEXT("max_results")), 1, 500) : 100;
				const bool bIncludePins = !Arguments->HasTypedField<EJson::Boolean>(TEXT("include_pin_schema")) || Arguments->GetBoolField(TEXT("include_pin_schema"));

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!AssetPath.IsEmpty())
				{
					if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
					{
						return false;
					}
				}

				FBlueprintActionFilter Filter;
				if (Blueprint) { Filter.Context.Blueprints.Add(Blueprint); }
				if (Graph) { Filter.Context.Graphs.Add(Graph); }
				if (Blueprint && Blueprint->GeneratedClass) { FBlueprintActionFilter::AddUnique(Filter.TargetClasses, Blueprint->GeneratedClass); }

				TArray<TSharedPtr<FJsonValue>> Results;
				int32 CompatibleCount = 0;
				int32 MatchedCount = 0;
				const FBlueprintActionDatabase::FActionRegistry& Actions = FBlueprintActionDatabase::Get().GetAllActions();
				for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : Actions)
				{
					UObject* Owner = Pair.Key.ResolveObjectPtr();
					for (UBlueprintNodeSpawner* Spawner : Pair.Value)
					{
						if (!Spawner || !Spawner->NodeClass) { continue; }
						FBlueprintActionInfo ActionInfo(Owner, Spawner);
						if ((Blueprint || Graph) && Filter.IsFiltered(ActionInfo)) { continue; }
						++CompatibleCount;

						const FBlueprintActionUiSpec& Ui = Spawner->PrimeDefaultUiSpec(Graph);
						const FString OwnerPath = Owner ? Owner->GetPathName() : FString();
						const FString NodeClassPath = Spawner->NodeClass->GetPathName();
						const FString SearchText = (Ui.MenuName.ToString() + TEXT(" ") + Ui.Category.ToString() + TEXT(" ") + Ui.Keywords.ToString() + TEXT(" ") + OwnerPath + TEXT(" ") + NodeClassPath).ToLower();
						if (!QueryLower.IsEmpty() && !SearchText.Contains(QueryLower)) { continue; }
						const int32 MatchIndex = MatchedCount++;
						if (MatchIndex < Offset || Results.Num() >= MaxResults) { continue; }

						TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("action_id"), Spawner->GetSpawnerSignature().AsGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
						Item->SetStringField(TEXT("signature"), Spawner->GetSpawnerSignature().ToString());
						Item->SetStringField(TEXT("menu_name"), Ui.MenuName.ToString());
						Item->SetStringField(TEXT("category"), Ui.Category.ToString());
						Item->SetStringField(TEXT("tooltip"), Ui.Tooltip.ToString());
						Item->SetStringField(TEXT("keywords"), Ui.Keywords.ToString());
						Item->SetStringField(TEXT("node_class"), NodeClassPath);
						Item->SetStringField(TEXT("spawner_class"), Spawner->GetClass()->GetPathName());
						Item->SetStringField(TEXT("owner"), OwnerPath);

						if (bIncludePins)
						{
							TArray<TSharedPtr<FJsonValue>> Pins;
							if (UEdGraphNode* TemplateNode = Spawner->GetTemplateNode(Graph))
							{
								for (const UEdGraphPin* Pin : TemplateNode->Pins)
								{
									Pins.Add(MakeShared<FJsonValueObject>(BlueprintPinToJson(Pin)));
								}
							}
							Item->SetArrayField(TEXT("pins"), Pins);
						}
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
				}

				OutStructured->SetStringField(TEXT("schema"), TEXT("somol.blueprint.action_catalog.v1"));
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("graph_name"), GraphName);
				OutStructured->SetStringField(TEXT("query"), Query);
				OutStructured->SetNumberField(TEXT("compatible_count"), CompatibleCount);
				OutStructured->SetNumberField(TEXT("matched_count"), MatchedCount);
				OutStructured->SetNumberField(TEXT("offset"), Offset);
				OutStructured->SetNumberField(TEXT("returned_count"), Results.Num());
				OutStructured->SetArrayField(TEXT("actions"), Results);
				OutSummary = FString::Printf(TEXT("Blueprint Action Database returned %d of %d matching compatible actions."), Results.Num(), MatchedCount);
				return true;
			},
			nullptr,
			15
		});

		Registry.Register({
			TEXT("blueprint_spawn_action_node"),
			TEXT("Spawn any graph-compatible Blueprint action by the stable action_id returned from blueprint_action_catalog. The action is compatibility-filtered, transaction protected, reconstructed, compiled, and read back before success."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("action_id"), FSololmcpSchemaBuilder::String(TEXT("Stable action id from blueprint_action_catalog."))},
				{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()}
			}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("action_id")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString ActionId;
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				Arguments->TryGetStringField(TEXT("action_id"), ActionId);
				ActionId = ActionId.TrimStartAndEnd().ToLower();

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError)) { return false; }

				FBlueprintActionFilter Filter;
				Filter.Context.Blueprints.Add(Blueprint);
				Filter.Context.Graphs.Add(Graph);
				if (Blueprint->GeneratedClass) { FBlueprintActionFilter::AddUnique(Filter.TargetClasses, Blueprint->GeneratedClass); }

				UBlueprintNodeSpawner* Match = nullptr;
				UObject* MatchOwner = nullptr;
				const FBlueprintActionDatabase::FActionRegistry& Actions = FBlueprintActionDatabase::Get().GetAllActions();
				for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Pair : Actions)
				{
					UObject* Owner = Pair.Key.ResolveObjectPtr();
					for (UBlueprintNodeSpawner* Spawner : Pair.Value)
					{
						if (!Spawner || !Spawner->NodeClass) { continue; }
						const FString CandidateId = Spawner->GetSpawnerSignature().AsGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
						if (CandidateId != ActionId) { continue; }
						FBlueprintActionInfo ActionInfo(Owner, Spawner);
						if (Filter.IsFiltered(ActionInfo))
						{
							OutError = TEXT("Requested Blueprint action is not compatible with the target Blueprint/graph context.");
							return false;
						}
						Match = Spawner;
						MatchOwner = Owner;
						break;
					}
					if (Match) { break; }
				}
				if (!Match)
				{
					OutError = FString::Printf(TEXT("Blueprint action_id '%s' was not found. Refresh blueprint_action_catalog and retry with its current id."), *ActionId);
					return false;
				}

				const FVector2f Location = GetBlueprintNodeLocationFromArguments(Arguments);
				const FScopedTransaction Transaction(LOCTEXT("BPSpawnActionNode", "SOMOLMCP Spawn Blueprint Action Node"));
				Blueprint->Modify();
				Graph->Modify();
				UEdGraphNode* Node = Match->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(Location));
				if (!VerifyGraphContainsNode(Graph, Node))
				{
					OutError = TEXT("Blueprint action spawner did not create a readable node in the target graph.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError)) { return false; }
				OutStructured->SetStringField(TEXT("action_id"), ActionId);
				OutStructured->SetStringField(TEXT("action_owner"), MatchOwner ? MatchOwner->GetPathName() : FString());
				OutStructured->SetObjectField(TEXT("node"), BlueprintNodeToJson(Node));
				AttachBlueprintGraphEditReceipt(OutStructured, Blueprint, Graph, TEXT("spawn_action_node"), TEXT("graph_contains_node"), true);
				OutSummary = FString::Printf(TEXT("Spawned Blueprint action %s as %s in %s.%s and passed compile/readback."), *ActionId, *Node->GetClass()->GetName(), *AssetPath, *GraphName);
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_asset_factory_create"),
			TEXT("Create a verified Blueprint asset through the correct Unreal factory: normal class, interface, function library, macro library, Animation Blueprint, Widget Blueprint, Editor Utility Blueprint, or Editor Utility Widget. Saves, reloads, compiles, and returns an asset-kind receipt."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Destination /Game package path including asset name."))},
				{TEXT("asset_kind"), FSololmcpSchemaBuilder::String(TEXT("normal | interface | function_library | macro_library | animation | widget | editor_utility_blueprint | editor_utility_widget"))},
				{TEXT("parent_class_path"), FSololmcpSchemaBuilder::String(TEXT("Parent class for normal/animation/editor utility assets."))},
				{TEXT("target_skeleton_path"), FSololmcpSchemaBuilder::String(TEXT("Required for a non-template Animation Blueprint."))},
				{TEXT("preview_skeletal_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Animation Blueprint preview skeletal mesh."))},
				{TEXT("animation_template"), FSololmcpSchemaBuilder::Boolean(TEXT("Create an Animation Blueprint template without a target skeleton."))},
				{TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicitly replace a compatible existing target asset; default false."))}
			}, {TEXT("asset_path"), TEXT("asset_kind")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString Kind;
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
				Arguments->TryGetStringField(TEXT("asset_kind"), Kind);
				Kind = Kind.TrimStartAndEnd().ToLower();
				int32 Slash = INDEX_NONE;
				if (!AssetPath.FindLastChar(TEXT('/'), Slash) || Slash <= 0 || !AssetPath.StartsWith(TEXT("/Game/")))
				{
					OutError = TEXT("asset_path must be a /Game package path including the asset name.");
					return false;
				}
				const FString PackagePath = AssetPath.Left(Slash);
				const FString AssetName = AssetPath.RightChop(Slash + 1);
				if (!IsSimpleBlueprintIdentifier(AssetName))
				{
					OutError = TEXT("Blueprint asset name is not a valid identifier.");
					return false;
				}

				FString AssetClassPath = UBlueprint::StaticClass()->GetPathName();
				FString FactoryClassPath;
				TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
				FString ParentClassPath;
				Arguments->TryGetStringField(TEXT("parent_class_path"), ParentClassPath);
				if (Kind == TEXT("normal"))
				{
					FactoryClassPath = TEXT("/Script/UnrealEd.BlueprintFactory");
					if (ParentClassPath.IsEmpty()) { ParentClassPath = TEXT("/Script/Engine.Actor"); }
					Overrides->SetStringField(TEXT("ParentClass"), ParentClassPath);
					Overrides->SetBoolField(TEXT("bSkipClassPicker"), true);
				}
				else if (Kind == TEXT("interface"))
				{
					FactoryClassPath = TEXT("/Script/UnrealEd.BlueprintInterfaceFactory");
				}
				else if (Kind == TEXT("function_library"))
				{
					FactoryClassPath = TEXT("/Script/UnrealEd.BlueprintFunctionLibraryFactory");
				}
				else if (Kind == TEXT("macro_library"))
				{
					FactoryClassPath = TEXT("/Script/UnrealEd.BlueprintMacroFactory");
					if (!ParentClassPath.IsEmpty()) { Overrides->SetStringField(TEXT("ParentClass"), ParentClassPath); }
					Overrides->SetBoolField(TEXT("bSkipClassPicker"), true);
				}
				else if (Kind == TEXT("animation"))
				{
					AssetClassPath = TEXT("/Script/Engine.AnimBlueprint");
					FactoryClassPath = TEXT("/Script/UnrealEd.AnimBlueprintFactory");
					if (ParentClassPath.IsEmpty()) { ParentClassPath = TEXT("/Script/Engine.AnimInstance"); }
					Overrides->SetStringField(TEXT("ParentClass"), ParentClassPath);
					const bool bTemplate = Arguments->HasTypedField<EJson::Boolean>(TEXT("animation_template")) && Arguments->GetBoolField(TEXT("animation_template"));
					Overrides->SetBoolField(TEXT("bTemplate"), bTemplate);
					FString SkeletonPath;
					Arguments->TryGetStringField(TEXT("target_skeleton_path"), SkeletonPath);
					if (!bTemplate && SkeletonPath.IsEmpty())
					{
						OutError = TEXT("target_skeleton_path is required for a non-template Animation Blueprint.");
						return false;
					}
					if (!SkeletonPath.IsEmpty()) { Overrides->SetStringField(TEXT("TargetSkeleton"), SkeletonPath); }
					FString PreviewMeshPath;
					Arguments->TryGetStringField(TEXT("preview_skeletal_mesh_path"), PreviewMeshPath);
					if (!PreviewMeshPath.IsEmpty()) { Overrides->SetStringField(TEXT("PreviewSkeletalMesh"), PreviewMeshPath); }
				}
				else if (Kind == TEXT("widget"))
				{
					AssetClassPath = TEXT("/Script/UMGEditor.WidgetBlueprint");
					FactoryClassPath = TEXT("/Script/UMGEditor.WidgetBlueprintFactory");
				}
				else if (Kind == TEXT("editor_utility_blueprint"))
				{
					AssetClassPath = TEXT("/Script/Blutility.EditorUtilityBlueprint");
					FactoryClassPath = TEXT("/Script/Blutility.EditorUtilityBlueprintFactory");
					if (!ParentClassPath.IsEmpty()) { Overrides->SetStringField(TEXT("ParentClass"), ParentClassPath); }
				}
				else if (Kind == TEXT("editor_utility_widget"))
				{
					AssetClassPath = TEXT("/Script/Blutility.EditorUtilityWidgetBlueprint");
					FactoryClassPath = TEXT("/Script/Blutility.EditorUtilityWidgetBlueprintFactory");
				}
				else
				{
					OutError = FString::Printf(TEXT("Unsupported blueprint asset_kind '%s'."), *Kind);
					return false;
				}

				const bool bReplace = Arguments->HasTypedField<EJson::Boolean>(TEXT("replace_existing")) && Arguments->GetBoolField(TEXT("replace_existing"));
				const FScopedTransaction Transaction(LOCTEXT("BPAssetFactoryCreate", "SOMOLMCP Create Blueprint Asset by Factory"));
				UObject* Created = Context.Services.CreateAsset(PackagePath, AssetName, AssetClassPath, FactoryClassPath, Overrides, OutError, bReplace);
				UBlueprint* Blueprint = Cast<UBlueprint>(Created);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Blueprint factory did not return a UBlueprint asset.") : OutError;
					return false;
				}
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
				if (Blueprint->Status == BS_Error)
				{
					OutError = TEXT("Created Blueprint failed its first compile; asset was not accepted as complete.");
					return false;
				}
				if (!Context.Services.SaveAsset(Blueprint->GetPathName(), false, OutError)) { return false; }
				FString ReloadError;
				UBlueprint* Reloaded = Cast<UBlueprint>(Context.Services.LoadAsset(Blueprint->GetPathName(), ReloadError));
				if (!Reloaded || Reloaded->Status == BS_Error)
				{
					OutError = FString::Printf(TEXT("Blueprint save/reload validation failed: %s"), *ReloadError);
					return false;
				}

				OutStructured->SetStringField(TEXT("schema"), TEXT("somol.blueprint.asset_factory_receipt.v1"));
				OutStructured->SetStringField(TEXT("asset_path"), Reloaded->GetPathName());
				OutStructured->SetStringField(TEXT("asset_kind"), Kind);
				OutStructured->SetStringField(TEXT("asset_class"), Reloaded->GetClass()->GetPathName());
				OutStructured->SetStringField(TEXT("factory_class"), FactoryClassPath);
				OutStructured->SetStringField(TEXT("parent_class"), Reloaded->ParentClass ? Reloaded->ParentClass->GetPathName() : FString());
				OutStructured->SetStringField(TEXT("generated_class"), Reloaded->GeneratedClass ? Reloaded->GeneratedClass->GetPathName() : FString());
				OutStructured->SetNumberField(TEXT("compile_status"), static_cast<int32>(Reloaded->Status));
				OutStructured->SetBoolField(TEXT("saved"), true);
				OutStructured->SetBoolField(TEXT("reloaded"), true);
				OutStructured->SetBoolField(TEXT("receipt_complete"), true);
				OutSummary = FString::Printf(TEXT("Created, compiled, saved, and reloaded %s Blueprint %s."), *Kind, *Reloaded->GetPathName());
				return true;
			}
		});

		// ----------------------------------------------------------------
		// P0 Blueprint generic K2 authoring overrides.
		// Registered after the legacy domain tools, so these definitions
		// replace the older generic Blueprint implementations without
		// touching SololmcpDomainTools.cpp.
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_first_pass_creation_contract_gate"),
			TEXT("Dry-run validate a Blueprint creation plan before the first write. Fails closed if asset path, parent class, variables, functions, nodes, links, or pin types are incomplete, so Agents cannot create a bad Blueprint and repair it later."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Planned Blueprint long package asset path, e.g. /Game/SOMRes/BP_MyActor."))},
					{TEXT("parent_class_path"), FSololmcpSchemaBuilder::String(TEXT("Planned parent class path/name, e.g. /Script/Engine.Actor."))},
					{TEXT("variables"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Variable name."))},
						{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("Primitive, struct/object/class/enum/interface/delegate, Texture2D/RenderTarget/material alias, soft reference, or nested array<>, set<>, map<key,value> type."))},
						{TEXT("type_object_path"), FSololmcpSchemaBuilder::String(TEXT("Required by non-generic struct/object/class/enum/interface/delegate forms, e.g. /Script/Engine.Actor."))}
					}), TEXT("Planned member variables."))},
					{TEXT("functions"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("User function graph name; reserved EventGraph/UserConstructionScript names are rejected."))},
						{TEXT("return_type"), FSololmcpSchemaBuilder::String(TEXT("Optional return type."))},
						{TEXT("return_type_object_path"), FSololmcpSchemaBuilder::String(TEXT("Optional return type object path."))}
					}), TEXT("Planned user functions."))},
					{TEXT("events"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Event/custom event name."))}
					}), TEXT("Planned events/custom events."))},
					{TEXT("nodes"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("id"), FSololmcpSchemaBuilder::String(TEXT("Stable local node id used by planned links."))},
						{TEXT("graph"), FSololmcpSchemaBuilder::String(TEXT("Target graph name."))},
						{TEXT("tool"), FSololmcpSchemaBuilder::String(TEXT("Resolved MCP tool name. Mutating Blueprint work must resolve to blueprint_* tools."))},
						{TEXT("node_type"), FSololmcpSchemaBuilder::String(TEXT("Node class/type/function descriptor."))}
					}), TEXT("Planned nodes."))},
					{TEXT("links"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("from_node"), FSololmcpSchemaBuilder::String(TEXT("Source planned node id."))},
						{TEXT("from_pin"), FSololmcpSchemaBuilder::String(TEXT("Source pin name."))},
						{TEXT("from_shape"), FSololmcpSchemaBuilder::String(TEXT("Optional pin shape: exec or data."))},
						{TEXT("to_node"), FSololmcpSchemaBuilder::String(TEXT("Target planned node id."))},
						{TEXT("to_pin"), FSololmcpSchemaBuilder::String(TEXT("Target pin name."))},
						{TEXT("to_shape"), FSololmcpSchemaBuilder::String(TEXT("Optional pin shape: exec or data."))}
					}), TEXT("Planned links."))},
					{TEXT("pin_defaults"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("node"), FSololmcpSchemaBuilder::String(TEXT("Planned node id."))},
						{TEXT("pin"), FSololmcpSchemaBuilder::String(TEXT("Pin name."))},
						{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("Optional type to validate before setting default."))},
						{TEXT("type_object_path"), FSololmcpSchemaBuilder::String(TEXT("Optional type object path."))},
						{TEXT("value"), FSololmcpSchemaBuilder::String(TEXT("Default value string."))}
					}), TEXT("Planned pin default assignments."))}
				},
				{TEXT("asset_path"), TEXT("parent_class_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParentClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("parent_class_path"), ParentClassPath))
				{
					OutError = TEXT("Missing asset_path or parent_class_path.");
					return false;
				}
				AssetPath = AssetPath.TrimStartAndEnd();
				ParentClassPath = ParentClassPath.TrimStartAndEnd();

				TArray<TSharedPtr<FJsonValue>> Findings;
				FString PackagePath;
				FString AssetName;
				int32 LastSlash = INDEX_NONE;
				if (!AssetPath.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
				{
					AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_asset_path"), TEXT("asset_path"), TEXT("asset_path must be in format /Game/Folder/BP_Name."));
				}
				else
				{
					PackagePath = AssetPath.Left(LastSlash);
					AssetName = AssetPath.RightChop(LastSlash + 1);
					if (!PackagePath.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase) && !PackagePath.Equals(TEXT("/Game"), ESearchCase::IgnoreCase))
					{
						AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("asset_path_outside_game"), TEXT("asset_path"), TEXT("Blueprint first-pass creation must target a /Game package path."));
					}
					if (!FPackageName::IsValidLongPackageName(PackagePath) || !IsSimpleBlueprintIdentifier(AssetName))
					{
						AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_package_or_asset_name"), TEXT("asset_path"), TEXT("Package path must be valid and asset name must be a Blueprint identifier."));
					}
					if (!AssetName.StartsWith(TEXT("BP_"), ESearchCase::CaseSensitive) &&
						!AssetName.StartsWith(TEXT("BPI_"), ESearchCase::CaseSensitive) &&
						!AssetName.StartsWith(TEXT("BFL_"), ESearchCase::CaseSensitive) &&
						!AssetName.StartsWith(TEXT("ABP_"), ESearchCase::CaseSensitive) &&
						!AssetName.StartsWith(TEXT("WBP_"), ESearchCase::CaseSensitive))
					{
						AddBlueprintContractFinding(Findings, TEXT("warning"), TEXT("blueprint_prefix_missing"), TEXT("asset_path"), TEXT("Recommended Blueprint asset prefixes are BP_/BPI_/BFL_/ABP_/WBP_."));
					}
				}

				FString ClassResolveError;
				UClass* ParentClass = Context.Services.ResolveClass(ParentClassPath, ClassResolveError);
				if (!ParentClass)
				{
					AddBlueprintContractFinding(
						Findings,
						TEXT("error"),
						TEXT("parent_class_not_found"),
						TEXT("parent_class_path"),
						ClassResolveError.IsEmpty() ? TEXT("Parent class could not be resolved.") : ClassResolveError);
				}

				TSet<FString> MemberNames;
				ValidateBlueprintContractNamedArray(Arguments, TEXT("variables"), MemberNames, Findings, true);
				ValidateBlueprintContractNamedArray(Arguments, TEXT("functions"), MemberNames, Findings, false);
				ValidateBlueprintContractNamedArray(Arguments, TEXT("events"), MemberNames, Findings, false);

				const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
				if (Arguments->TryGetArrayField(TEXT("functions"), Functions) && Functions)
				{
					for (int32 Index = 0; Index < Functions->Num(); ++Index)
					{
						const TSharedPtr<FJsonValue>& ItemValue = (*Functions)[Index];
						const TSharedPtr<FJsonObject>* ItemObjPtr = nullptr;
						if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObjPtr) || !ItemObjPtr || !ItemObjPtr->IsValid())
						{
							continue;
						}
						const TSharedPtr<FJsonObject> Item = *ItemObjPtr;
						FString ReturnType;
						if (Item->TryGetStringField(TEXT("return_type"), ReturnType) && !ReturnType.TrimStartAndEnd().IsEmpty())
						{
							ValidateBlueprintContractType(Item, FString::Printf(TEXT("functions[%d].return_type"), Index), Findings, TEXT("return_type"), TEXT("return_type_object_path"));
						}
					}
				}

				TSet<FString> NodeIds;
				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if (Arguments->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
				{
					for (int32 Index = 0; Index < Nodes->Num(); ++Index)
					{
						const FString Prefix = FString::Printf(TEXT("nodes[%d]"), Index);
						const TSharedPtr<FJsonValue>& ItemValue = (*Nodes)[Index];
						const TSharedPtr<FJsonObject>* ItemObjPtr = nullptr;
						if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObjPtr) || !ItemObjPtr || !ItemObjPtr->IsValid())
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_contract_item"), Prefix, TEXT("Expected an object."));
							continue;
						}
						const TSharedPtr<FJsonObject> Item = *ItemObjPtr;
						FString Id;
						FString Graph;
						FString ToolName;
						Item->TryGetStringField(TEXT("id"), Id);
						Item->TryGetStringField(TEXT("graph"), Graph);
						Item->TryGetStringField(TEXT("tool"), ToolName);
						if (Id.TrimStartAndEnd().IsEmpty())
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("missing_node_id"), Prefix + TEXT(".id"), TEXT("Every planned node must have a stable id before links are authored."));
						}
						else if (NodeIds.Contains(Id))
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("duplicate_node_id"), Prefix + TEXT(".id"), FString::Printf(TEXT("Duplicate planned node id '%s'."), *Id));
						}
						else
						{
							NodeIds.Add(Id);
						}
						if (Graph.TrimStartAndEnd().IsEmpty())
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("missing_graph"), Prefix + TEXT(".graph"), TEXT("Every node write must declare the target graph before first mutation."));
						}
						if (!ToolName.TrimStartAndEnd().IsEmpty() && !ToolName.StartsWith(TEXT("blueprint_"), ESearchCase::IgnoreCase))
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("non_blueprint_tool_for_blueprint_mutation"), Prefix + TEXT(".tool"), FString::Printf(TEXT("Blueprint graph mutation must use blueprint_* MCP tools, not '%s'."), *ToolName));
						}
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
				if (Arguments->TryGetArrayField(TEXT("links"), Links) && Links)
				{
					for (int32 Index = 0; Index < Links->Num(); ++Index)
					{
						const FString Prefix = FString::Printf(TEXT("links[%d]"), Index);
						const TSharedPtr<FJsonValue>& ItemValue = (*Links)[Index];
						const TSharedPtr<FJsonObject>* ItemObjPtr = nullptr;
						if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObjPtr) || !ItemObjPtr || !ItemObjPtr->IsValid())
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_contract_item"), Prefix, TEXT("Expected an object."));
							continue;
						}
						const TSharedPtr<FJsonObject> Item = *ItemObjPtr;
						FString FromNode;
						FString FromPin;
						FString FromShape;
						FString ToNode;
						FString ToPin;
						FString ToShape;
						Item->TryGetStringField(TEXT("from_node"), FromNode);
						Item->TryGetStringField(TEXT("from_pin"), FromPin);
						Item->TryGetStringField(TEXT("from_shape"), FromShape);
						Item->TryGetStringField(TEXT("to_node"), ToNode);
						Item->TryGetStringField(TEXT("to_pin"), ToPin);
						Item->TryGetStringField(TEXT("to_shape"), ToShape);
						if (FromNode.TrimStartAndEnd().IsEmpty() || ToNode.TrimStartAndEnd().IsEmpty() || FromPin.TrimStartAndEnd().IsEmpty() || ToPin.TrimStartAndEnd().IsEmpty())
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("incomplete_link_endpoint"), Prefix, TEXT("Links must declare from_node/from_pin/to_node/to_pin before first mutation."));
						}
						if (Nodes && Nodes->Num() > 0)
						{
							if (!FromNode.IsEmpty() && !NodeIds.Contains(FromNode))
							{
								AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("unknown_from_node"), Prefix + TEXT(".from_node"), FString::Printf(TEXT("Link references unknown source node '%s'."), *FromNode));
							}
							if (!ToNode.IsEmpty() && !NodeIds.Contains(ToNode))
							{
								AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("unknown_to_node"), Prefix + TEXT(".to_node"), FString::Printf(TEXT("Link references unknown target node '%s'."), *ToNode));
							}
						}
						const bool bFromExec = FromShape.Equals(TEXT("exec"), ESearchCase::IgnoreCase);
						const bool bToExec = ToShape.Equals(TEXT("exec"), ESearchCase::IgnoreCase);
						const bool bFromData = FromShape.Equals(TEXT("data"), ESearchCase::IgnoreCase);
						const bool bToData = ToShape.Equals(TEXT("data"), ESearchCase::IgnoreCase);
						if ((bFromExec && !bToExec) || (bToExec && !bFromExec) || (bFromData && bToExec) || (bToData && bFromExec))
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("pin_shape_mismatch"), Prefix, TEXT("Exec pins may only connect to exec pins; data pins may only connect to compatible data pins."));
						}
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* PinDefaults = nullptr;
				if (Arguments->TryGetArrayField(TEXT("pin_defaults"), PinDefaults) && PinDefaults)
				{
					for (int32 Index = 0; Index < PinDefaults->Num(); ++Index)
					{
						const FString Prefix = FString::Printf(TEXT("pin_defaults[%d]"), Index);
						const TSharedPtr<FJsonValue>& ItemValue = (*PinDefaults)[Index];
						const TSharedPtr<FJsonObject>* ItemObjPtr = nullptr;
						if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObjPtr) || !ItemObjPtr || !ItemObjPtr->IsValid())
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("invalid_contract_item"), Prefix, TEXT("Expected an object."));
							continue;
						}
						const TSharedPtr<FJsonObject> Item = *ItemObjPtr;
						FString Node;
						FString Pin;
						FString TypeName;
						Item->TryGetStringField(TEXT("node"), Node);
						Item->TryGetStringField(TEXT("pin"), Pin);
						Item->TryGetStringField(TEXT("type"), TypeName);
						if (Node.TrimStartAndEnd().IsEmpty() || Pin.TrimStartAndEnd().IsEmpty())
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("incomplete_pin_default_target"), Prefix, TEXT("Pin default writes must declare node and pin before first mutation."));
						}
						if (!Node.IsEmpty() && Nodes && Nodes->Num() > 0 && !NodeIds.Contains(Node))
						{
							AddBlueprintContractFinding(Findings, TEXT("error"), TEXT("unknown_pin_default_node"), Prefix + TEXT(".node"), FString::Printf(TEXT("Pin default references unknown node '%s'."), *Node));
						}
						if (!TypeName.TrimStartAndEnd().IsEmpty())
						{
							ValidateBlueprintContractType(Item, Prefix, Findings);
						}
					}
				}

				int32 ErrorCount = 0;
				int32 WarningCount = 0;
				for (const TSharedPtr<FJsonValue>& FindingValue : Findings)
				{
					const TSharedPtr<FJsonObject>* FindingObj = nullptr;
					if (!FindingValue.IsValid() || !FindingValue->TryGetObject(FindingObj) || !FindingObj || !FindingObj->IsValid())
					{
						continue;
					}
					FString Severity;
					(*FindingObj)->TryGetStringField(TEXT("severity"), Severity);
					if (Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
					{
						++ErrorCount;
					}
					else if (Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
					{
						++WarningCount;
					}
				}

				auto CountArray = [&Arguments](const FString& FieldName) -> int32
				{
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					return (Arguments->TryGetArrayField(FieldName, Values) && Values) ? Values->Num() : 0;
				};

				const bool bGateOk = ErrorCount == 0;
				OutStructured->SetStringField(TEXT("schema"), TEXT("somol.blueprint.first_pass_creation_contract_gate.v1"));
				OutStructured->SetBoolField(TEXT("dry_run"), true);
				OutStructured->SetBoolField(TEXT("required_before_first_mutation"), true);
				OutStructured->SetBoolField(TEXT("gate_ok"), bGateOk);
				OutStructured->SetBoolField(TEXT("mutation_allowed"), bGateOk);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("package_path"), PackagePath);
				OutStructured->SetStringField(TEXT("asset_name"), AssetName);
				OutStructured->SetStringField(TEXT("parent_class_path"), ParentClassPath);
				OutStructured->SetStringField(TEXT("parent_class"), ParentClass ? ParentClass->GetPathName() : FString());
				OutStructured->SetNumberField(TEXT("error_count"), ErrorCount);
				OutStructured->SetNumberField(TEXT("warning_count"), WarningCount);
				OutStructured->SetNumberField(TEXT("variable_count"), CountArray(TEXT("variables")));
				OutStructured->SetNumberField(TEXT("function_count"), CountArray(TEXT("functions")));
				OutStructured->SetNumberField(TEXT("event_count"), CountArray(TEXT("events")));
				OutStructured->SetNumberField(TEXT("node_count"), CountArray(TEXT("nodes")));
				OutStructured->SetNumberField(TEXT("link_count"), CountArray(TEXT("links")));
				OutStructured->SetNumberField(TEXT("pin_default_count"), CountArray(TEXT("pin_defaults")));
				OutStructured->SetArrayField(TEXT("findings"), Findings);
				OutStructured->SetStringField(TEXT("no_repair_loop_policy"), TEXT("The creation plan must pass before any write; failed contracts block mutation instead of creating a bad Blueprint and repairing it later."));
				OutStructured->SetBoolField(TEXT("receipt_complete"), bGateOk);

				if (!bGateOk)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("blueprint_first_pass_contract_failed"));
					OutError = FString::Printf(TEXT("Blueprint first-pass creation contract failed for %s: %d error(s), %d warning(s). No Blueprint mutation is allowed."), *AssetPath, ErrorCount, WarningCount);
					return false;
				}

				OutSummary = FString::Printf(TEXT("Blueprint first-pass creation contract passed for %s: %d variable(s), %d node(s), %d link(s). Mutation allowed."), *AssetPath, CountArray(TEXT("variables")), CountArray(TEXT("nodes")), CountArray(TEXT("links")));
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_create"),
			TEXT("Create a Blueprint asset with a specified parent class, then enforce RefreshAllNodes + compile diagnostics + repair-gate validation before saving or reporting success."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Long package asset path, e.g. /Game/SOMRes/BP_MyActor."))},
					{TEXT("parent_class_path"), FSololmcpSchemaBuilder::String(TEXT("Parent class path/name, e.g. /Script/Engine.Actor."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after verified compile gate. Defaults to true."))},
					{TEXT("allow_unique_name"), FSololmcpSchemaBuilder::Boolean(TEXT("Generate a unique asset name instead of overwriting. Defaults to true."))},
					{TEXT("delete_on_failed_gate"), FSololmcpSchemaBuilder::Boolean(TEXT("Delete the newly-created asset when compile/repair gate fails. Defaults to true."))}
				},
				{TEXT("asset_path"), TEXT("parent_class_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParentClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("parent_class_path"), ParentClassPath))
				{
					OutError = TEXT("Missing asset_path or parent_class_path.");
					return false;
				}

				bool bSave = true;
				bool bAllowUniqueName = true;
				bool bDeleteOnFailedGate = true;
				Arguments->TryGetBoolField(TEXT("save"), bSave);
				Arguments->TryGetBoolField(TEXT("allow_unique_name"), bAllowUniqueName);
				Arguments->TryGetBoolField(TEXT("delete_on_failed_gate"), bDeleteOnFailedGate);

				FString PackagePath;
				FString AssetName;
				int32 LastSlash = INDEX_NONE;
				if (!AssetPath.FindLastChar(TEXT('/'), LastSlash) || LastSlash <= 0)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("invalid_asset_path"));
					OutError = TEXT("asset_path must be in format /Game/Folder/BlueprintName.");
					return false;
				}
				PackagePath = AssetPath.Left(LastSlash);
				AssetName = AssetPath.RightChop(LastSlash + 1);
				if (AssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackagePath))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("invalid_asset_path"));
					OutStructured->SetStringField(TEXT("package_path"), PackagePath);
					OutStructured->SetStringField(TEXT("asset_name"), AssetName);
					OutError = TEXT("asset_path package/name is invalid.");
					return false;
				}

				FString ClassResolveError;
				UClass* ParentClass = Context.Services.ResolveClass(ParentClassPath, ClassResolveError);
				if (!ParentClass)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("parent_class_not_found"));
					OutStructured->SetStringField(TEXT("parent_class_path"), ParentClassPath);
					OutStructured->SetStringField(TEXT("class_resolve_error"), ClassResolveError);
					OutError = ClassResolveError.IsEmpty() ? TEXT("Parent class could not be resolved.") : ClassResolveError;
					return false;
				}

				FString EffectiveName = AssetName;
				const FString RequestedAssetPath = PackagePath / AssetName;
				if (Context.Services.AssetExists(RequestedAssetPath))
				{
					if (!bAllowUniqueName)
					{
						OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("asset_already_exists"));
						OutStructured->SetStringField(TEXT("asset_path"), RequestedAssetPath);
						OutError = TEXT("Blueprint asset already exists and allow_unique_name=false; refusing to overwrite.");
						return false;
					}
					EffectiveName = Context.Services.GenerateUniqueAssetName(PackagePath, AssetName);
				}
				const FString EffectiveAssetPath = PackagePath / EffectiveName;
				if (Context.Services.AssetExists(EffectiveAssetPath))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("effective_asset_already_exists"));
					OutStructured->SetStringField(TEXT("asset_path"), EffectiveAssetPath);
					OutError = TEXT("Effective Blueprint asset path already exists; refusing to overwrite.");
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BlueprintCreateVerified", "SOMOLMCP Create Verified Blueprint"));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				UBlueprint* Blueprint = UBlueprintEditorLibrary::CreateBlueprintAssetWithParent(EffectiveAssetPath, ParentClass);
#else
				// UBlueprintEditorLibrary::CreateBlueprintAssetWithParent is 5.4+. On 5.3 the same
				// asset is produced through FKismetEditorUtilities, which needs the package created
				// and the asset registered explicitly -- that is exactly what the 5.4 helper wraps.
				UBlueprint* Blueprint = nullptr;
				if (UPackage* BlueprintPackage = CreatePackage(*EffectiveAssetPath))
				{
					const FString NewBlueprintAssetName = FPackageName::GetLongPackageAssetName(EffectiveAssetPath);
					Blueprint = FKismetEditorUtilities::CreateBlueprint(
						ParentClass, BlueprintPackage, FName(*NewBlueprintAssetName), BPTYPE_Normal,
						UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
					if (Blueprint)
					{
						FAssetRegistryModule::AssetCreated(Blueprint);
						BlueprintPackage->MarkPackageDirty();
					}
				}
#endif
				if (!Blueprint)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("create_blueprint_failed"));
					OutStructured->SetStringField(TEXT("asset_path"), EffectiveAssetPath);
					OutStructured->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
					OutError = FString::Printf(TEXT("Failed to create blueprint at '%s' with parent '%s'."), *EffectiveAssetPath, *ParentClass->GetPathName());
					return false;
				}

				Blueprint->Modify();
				Blueprint->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Blueprint);
				FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				FCompilerResultsLog ResultsLog;
				ResultsLog.bSilentMode = true;
				ResultsLog.bAnnotateMentionedNodes = true;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

				TArray<TSharedPtr<FJsonValue>> Messages;
				for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
				{
					Messages.Add(MakeShared<FJsonValueObject>(CompilerMessageToJson(Message)));
				}

				TArray<TSharedPtr<FJsonValue>> RepairIssues;
				ScanBlueprintRepairIssues(Blueprint, RepairIssues);
				int32 BlockingIssueCount = 0;
				for (const TSharedPtr<FJsonValue>& IssueValue : RepairIssues)
				{
					if (IsBlockingBlueprintRepairIssue(IssueValue))
					{
						++BlockingIssueCount;
					}
				}

				const FString CreatedPath = Blueprint->GetPathName();
				const bool bParentVerified = Blueprint->ParentClass == ParentClass;
				const bool bCompileOk = Blueprint->Status != BS_Error && ResultsLog.NumErrors == 0;
				const bool bGateOk = bParentVerified && bCompileOk && BlockingIssueCount == 0 && Blueprint->GeneratedClass != nullptr && Blueprint->SkeletonGeneratedClass != nullptr;

				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				OutStructured->SetStringField(TEXT("schema"), TEXT("somol.blueprint.create_verified_receipt.v1"));
				OutStructured->SetStringField(TEXT("requested_asset_path"), RequestedAssetPath);
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutStructured->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
				OutStructured->SetBoolField(TEXT("unique_name_used"), EffectiveName != AssetName);
				OutStructured->SetStringField(TEXT("effective_asset_name"), EffectiveName);
				OutStructured->SetBoolField(TEXT("parent_verified"), bParentVerified);
				OutStructured->SetBoolField(TEXT("refresh_all_nodes_attempted"), true);
				OutStructured->SetBoolField(TEXT("compile_ok"), bCompileOk);
				OutStructured->SetNumberField(TEXT("compiler_error_count"), ResultsLog.NumErrors);
				OutStructured->SetNumberField(TEXT("compiler_warning_count"), ResultsLog.NumWarnings);
				OutStructured->SetArrayField(TEXT("compiler_messages"), Messages);
				OutStructured->SetArrayField(TEXT("repair_issues"), RepairIssues);
				OutStructured->SetNumberField(TEXT("repair_issue_count"), RepairIssues.Num());
				OutStructured->SetNumberField(TEXT("blocking_repair_issue_count"), BlockingIssueCount);
				OutStructured->SetBoolField(TEXT("save_requested"), bSave);
				OutStructured->SetStringField(TEXT("receipt_status"), bGateOk ? TEXT("completed") : TEXT("failed_validation"));
				OutStructured->SetBoolField(TEXT("receipt_complete"), bGateOk);
				OutStructured->SetStringField(TEXT("required_before_delivery"), TEXT("parent readback + generated class + skeleton class + RefreshAllNodes + compile diagnostics zero errors + repair issue gate"));

				if (!bGateOk)
				{
					FString DeleteErr;
					const bool bDeleted = bDeleteOnFailedGate && Context.Services.DeleteAsset(CreatedPath, DeleteErr);
					OutStructured->SetStringField(TEXT("diagnostic_code"), !bParentVerified ? TEXT("blueprint_parent_mismatch_after_create") : TEXT("blueprint_create_compile_gate_failed"));
					OutStructured->SetBoolField(TEXT("delete_on_failed_gate"), bDeleteOnFailedGate);
					OutStructured->SetBoolField(TEXT("cleanup_deleted_new_asset"), bDeleted);
					if (!DeleteErr.IsEmpty())
					{
						OutStructured->SetStringField(TEXT("cleanup_error"), DeleteErr);
					}
					OutError = FString::Printf(
						TEXT("Verified Blueprint create failed gate for %s: parent_verified=%s compile_errors=%d blocking_repair_issues=%d. New asset cleanup=%s."),
						*CreatedPath,
						bParentVerified ? TEXT("true") : TEXT("false"),
						ResultsLog.NumErrors,
						BlockingIssueCount,
						bDeleted ? TEXT("deleted") : TEXT("not_deleted"));
					return false;
				}

				bool bSaved = false;
				FString SaveErr;
				if (bSave)
				{
					bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
					OutStructured->SetBoolField(TEXT("saved"), bSaved);
					if (!bSaved)
					{
						OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("blueprint_create_save_failed"));
						OutStructured->SetStringField(TEXT("save_error"), SaveErr);
						OutStructured->SetStringField(TEXT("receipt_status"), TEXT("failed_validation"));
						OutStructured->SetBoolField(TEXT("receipt_complete"), false);
						OutError = SaveErr.IsEmpty() ? TEXT("Verified Blueprint create passed compile gate but SaveAsset failed.") : SaveErr;
						return false;
					}
					FString ReloadErr;
					UBlueprint* ReloadedBlueprint = Cast<UBlueprint>(Context.Services.LoadAsset(CreatedPath, ReloadErr));
					const bool bReloadVerified = ReloadedBlueprint && ReloadedBlueprint->ParentClass == ParentClass && ReloadedBlueprint->GeneratedClass != nullptr;
					OutStructured->SetBoolField(TEXT("reload_verified"), bReloadVerified);
					if (!bReloadVerified)
					{
						OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("blueprint_create_reload_failed"));
						OutStructured->SetStringField(TEXT("reload_error"), ReloadErr);
						OutStructured->SetStringField(TEXT("receipt_status"), TEXT("failed_validation"));
						OutStructured->SetBoolField(TEXT("receipt_complete"), false);
						OutError = FString::Printf(TEXT("Verified Blueprint create saved but reload verification failed: %s."), *CreatedPath);
						return false;
					}
				}
				else
				{
					OutStructured->SetBoolField(TEXT("saved"), false);
					OutStructured->SetBoolField(TEXT("reload_verified"), false);
				}

				OutSummary = FString::Printf(TEXT("Created verified Blueprint %s with zero compile errors."), *CreatedPath);
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_compile_diagnostics"),
			TEXT("Compile or inspect a Blueprint and return compiler log diagnostics with graph/node/pin repair hints."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))},
					{TEXT("compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Run FKismetEditorUtilities::CompileBlueprint and capture FCompilerResultsLog. Defaults to true."))},
					{TEXT("refresh_first"), FSololmcpSchemaBuilder::Boolean(TEXT("Run RefreshAllNodes before compiling. Defaults to false."))},
					{TEXT("max_messages"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum compiler messages to return. Defaults to 100."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}

				bool bCompile = true;
				bool bRefreshFirst = false;
				int32 MaxMessages = 100;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);
				Arguments->TryGetBoolField(TEXT("refresh_first"), bRefreshFirst);
				Arguments->TryGetNumberField(TEXT("max_messages"), MaxMessages);
				MaxMessages = FMath::Clamp(MaxMessages, 1, 1000);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				if (bRefreshFirst)
				{
					Blueprint->Modify();
					FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				}

				FCompilerResultsLog ResultsLog;
				ResultsLog.bSilentMode = true;
				ResultsLog.bAnnotateMentionedNodes = true;
				if (bCompile)
				{
					FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
				}

				TArray<TSharedPtr<FJsonValue>> Messages;
				for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
				{
					if (Messages.Num() >= MaxMessages)
					{
						break;
					}
					Messages.Add(MakeShared<FJsonValueObject>(CompilerMessageToJson(Message)));
				}

				TArray<TSharedPtr<FJsonValue>> RepairIssues;
				ScanBlueprintRepairIssues(Blueprint, RepairIssues);

				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetBoolField(TEXT("compile_requested"), bCompile);
				OutStructured->SetBoolField(TEXT("refresh_requested"), bRefreshFirst);
				OutStructured->SetBoolField(TEXT("compile_ok"), Blueprint->Status != BS_Error && ResultsLog.NumErrors == 0);
				OutStructured->SetNumberField(TEXT("compiler_error_count"), ResultsLog.NumErrors);
				OutStructured->SetNumberField(TEXT("compiler_warning_count"), ResultsLog.NumWarnings);
				OutStructured->SetNumberField(TEXT("compiler_message_count"), ResultsLog.Messages.Num());
				OutStructured->SetNumberField(TEXT("returned_message_count"), Messages.Num());
				OutStructured->SetArrayField(TEXT("compiler_messages"), Messages);
				OutStructured->SetArrayField(TEXT("repair_issues"), RepairIssues);
				OutStructured->SetNumberField(TEXT("repair_issue_count"), RepairIssues.Num());
				OutStructured->SetStringField(TEXT("diagnostic_scope"), TEXT("compiler_log_graph_node_pin"));
				OutStructured->SetBoolField(TEXT("modal_watchdog_required_on_timeout"), true);
				OutStructured->SetBoolField(TEXT("receipt_complete"), true);
				OutSummary = FString::Printf(
					TEXT("Blueprint compile diagnostics collected for %s: %d error(s), %d warning(s), %d repair issue(s)."),
					*Blueprint->GetPathName(),
					ResultsLog.NumErrors,
					ResultsLog.NumWarnings,
					RepairIssues.Num());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_repair_compile_gate"),
			TEXT("Run a safe deterministic Blueprint repair pass: dialog-safe refresh, compile diagnostics, repair issue scan, optional save only when zero errors remain."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))},
					{TEXT("save_if_clean"), FSololmcpSchemaBuilder::Boolean(TEXT("Save only when compile diagnostics and repair gate are clean. Defaults to false."))},
					{TEXT("structural_refresh"), FSololmcpSchemaBuilder::Boolean(TEXT("Mark structurally modified after RefreshAllNodes. Defaults to true."))},
					{TEXT("max_messages"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum compiler messages to return. Defaults to 200."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}

				bool bSaveIfClean = false;
				bool bStructuralRefresh = true;
				int32 MaxMessages = 200;
				Arguments->TryGetBoolField(TEXT("save_if_clean"), bSaveIfClean);
				Arguments->TryGetBoolField(TEXT("structural_refresh"), bStructuralRefresh);
				Arguments->TryGetNumberField(TEXT("max_messages"), MaxMessages);
				MaxMessages = FMath::Clamp(MaxMessages, 1, 1000);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				const FString StatusBefore = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
				TSharedRef<FJsonObject> BeforeReceipt = BuildBlueprintCompileReceipt(Blueprint);

				const FScopedTransaction Transaction(LOCTEXT("BlueprintRepairCompileGate", "SOMOLMCP Repair Blueprint Compile Gate"));
				Blueprint->Modify();
				FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
				if (bStructuralRefresh)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				}
				else
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				}

				FCompilerResultsLog ResultsLog;
				ResultsLog.bSilentMode = true;
				ResultsLog.bAnnotateMentionedNodes = true;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);

				TArray<TSharedPtr<FJsonValue>> Messages;
				for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
				{
					if (Messages.Num() >= MaxMessages)
					{
						break;
					}
					Messages.Add(MakeShared<FJsonValueObject>(CompilerMessageToJson(Message)));
				}

				TArray<TSharedPtr<FJsonValue>> RepairIssues;
				ScanBlueprintRepairIssues(Blueprint, RepairIssues);
				int32 BlockingIssueCount = 0;
				bool bHasGetDataTableMissingTable = false;
				bool bHasUiStyleBadStructType = false;
				bool bHasBadStructObject = false;
				bool bHasBadStructLink = false;
				bool bHasFunctionGraphMissingTerminator = false;
				for (const TSharedPtr<FJsonValue>& IssueValue : RepairIssues)
				{
					if (IsBlockingBlueprintRepairIssue(IssueValue))
					{
						++BlockingIssueCount;
					}
					const TSharedPtr<FJsonObject>* IssueObj = nullptr;
					if (IssueValue.IsValid() && IssueValue->TryGetObject(IssueObj) && IssueObj && IssueObj->IsValid())
					{
						FString IssueClass;
						(*IssueObj)->TryGetStringField(TEXT("class"), IssueClass);
						bHasGetDataTableMissingTable |= IssueClass.Equals(TEXT("getdatatable_missing_table"), ESearchCase::IgnoreCase);
						bHasUiStyleBadStructType |= IssueClass.Equals(TEXT("ui_style_bad_struct_type"), ESearchCase::IgnoreCase);
						bHasBadStructObject |= IssueClass.Equals(TEXT("bad_struct_pin_missing_type_object"), ESearchCase::IgnoreCase);
						bHasBadStructObject |= IssueClass.Equals(TEXT("bad_struct_pin_suspicious_type_object"), ESearchCase::IgnoreCase);
						bHasBadStructLink |= IssueClass.Equals(TEXT("bad_struct_pin_link_type_mismatch"), ESearchCase::IgnoreCase);
						bHasFunctionGraphMissingTerminator |= IssueClass.Equals(TEXT("function_graph_missing_terminator"), ESearchCase::IgnoreCase);
					}
				}

				TArray<TSharedPtr<FJsonValue>> NextRepairTasks;
				auto AddNextRepairTask = [&NextRepairTasks](const FString& Step, const FString& Tool, const FString& Reason)
				{
					TSharedRef<FJsonObject> Task = MakeShared<FJsonObject>();
					Task->SetStringField(TEXT("step"), Step);
					Task->SetStringField(TEXT("tool"), Tool);
					Task->SetStringField(TEXT("reason"), Reason);
					NextRepairTasks.Add(MakeShared<FJsonValueObject>(Task));
				};
				if (ResultsLog.NumErrors > 0 || BlockingIssueCount > 0)
				{
					AddNextRepairTask(TEXT("inspect_compile_messages"), TEXT("blueprint_compile_diagnostics"), TEXT("Read exact compiler messages and mentioned nodes before changing graph state."));
					AddNextRepairTask(TEXT("inspect_graph"), TEXT("blueprint_get_nodes / blueprint_graph_explain"), TEXT("Locate nodes and pins referenced by diagnostics."));
					if (bHasGetDataTableMissingTable)
					{
						AddNextRepairTask(TEXT("repair_getdatatable_defaults"), TEXT("blueprint_set_getdatatable_node_table"), TEXT("Assign a concrete DataTable default object and verify RowStruct readback before reconnecting dependent pins."));
					}
					if (bHasFunctionGraphMissingTerminator)
					{
						AddNextRepairTask(TEXT("finalize_function_graphs"), TEXT("blueprint_finalize_function_graph"), TEXT("Only function graphs may receive FunctionResult terminators; event, construction, and macro graphs are rejected by the tool gate."));
					}
					if (bHasUiStyleBadStructType || bHasBadStructObject)
					{
						AddNextRepairTask(TEXT("identify_expected_struct_types"), TEXT("blueprint_compile_diagnostics / blueprint_get_node_details / asset_query"), TEXT("Resolve the intended UScriptStruct path for each unknown/stale struct pin before mutating the graph."));
						AddNextRepairTask(TEXT("retype_unknown_struct_pins"), TEXT("blueprint_set_node_pin_type"), TEXT("Retype migrated wildcard or unknown struct pins with the resolved struct path, reconstruct the node, then compile."));
						AddNextRepairTask(TEXT("repair_struct_or_variable_types"), TEXT("blueprint_rebuild_member_variable / blueprint_rebind_variable_node / blueprint_refresh_all_nodes"), TEXT("Repair missing variables, stale struct pins, REINST/SKEL references, or wrong variable-node bindings before reconnecting execution flow."));
					}
					if (bHasBadStructLink)
					{
						AddNextRepairTask(TEXT("repair_struct_links"), TEXT("blueprint_break_single_pin_link / blueprint_connect_pins"), TEXT("Break mismatched struct pin links, then reconnect only after pin compatibility and struct identity are verified."));
					}
					AddNextRepairTask(TEXT("repair_variables_or_types"), TEXT("blueprint_rebuild_member_variable / blueprint_rebind_variable_node"), TEXT("Repair missing variables, stale struct pins, or bad variable-node bindings when diagnostics mention them."));
					AddNextRepairTask(TEXT("repair_links"), TEXT("blueprint_break_pin_links / blueprint_break_single_pin_link / blueprint_connect_pins"), TEXT("Remove stale or invalid links, then reconnect only after pin compatibility is known."));
					AddNextRepairTask(TEXT("recompile_gate"), TEXT("blueprint_repair_compile_gate"), TEXT("Repeat this gate after targeted repairs; do not save or deliver until zero errors remain."));
				}

				const bool bCompileOk = Blueprint->Status != BS_Error && ResultsLog.NumErrors == 0;
				const bool bGateOk = bCompileOk && BlockingIssueCount == 0;
				const FString StatusAfter = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				OutStructured->SetStringField(TEXT("schema"), TEXT("somol.blueprint.repair_compile_gate_receipt.v1"));
				OutStructured->SetObjectField(TEXT("before"), BeforeReceipt);
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("status_before"), StatusBefore);
				OutStructured->SetStringField(TEXT("status_after"), StatusAfter);
				OutStructured->SetBoolField(TEXT("refresh_all_nodes_attempted"), true);
				OutStructured->SetBoolField(TEXT("structural_refresh"), bStructuralRefresh);
				OutStructured->SetBoolField(TEXT("compile_ok"), bCompileOk);
				OutStructured->SetNumberField(TEXT("compiler_error_count"), ResultsLog.NumErrors);
				OutStructured->SetNumberField(TEXT("compiler_warning_count"), ResultsLog.NumWarnings);
				OutStructured->SetNumberField(TEXT("compiler_message_count"), ResultsLog.Messages.Num());
				OutStructured->SetNumberField(TEXT("returned_message_count"), Messages.Num());
				OutStructured->SetArrayField(TEXT("compiler_messages"), Messages);
				OutStructured->SetArrayField(TEXT("repair_issues"), RepairIssues);
				OutStructured->SetNumberField(TEXT("repair_issue_count"), RepairIssues.Num());
				OutStructured->SetNumberField(TEXT("blocking_repair_issue_count"), BlockingIssueCount);
				OutStructured->SetArrayField(TEXT("next_repair_tasks"), NextRepairTasks);
				OutStructured->SetBoolField(TEXT("save_if_clean"), bSaveIfClean);
				OutStructured->SetStringField(TEXT("receipt_status"), bGateOk ? TEXT("completed") : TEXT("failed_validation"));
				OutStructured->SetBoolField(TEXT("receipt_complete"), bGateOk);
				OutStructured->SetStringField(TEXT("required_before_delivery"), TEXT("RefreshAllNodes + compile diagnostics zero errors + repair issue gate"));

				if (bGateOk && bSaveIfClean)
				{
					FString SaveErr;
					const bool bSaved = Context.Services.SaveAsset(Blueprint->GetPathName(), false, SaveErr);
					OutStructured->SetBoolField(TEXT("saved"), bSaved);
					if (!bSaved)
					{
						OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("blueprint_repair_save_failed"));
						OutStructured->SetStringField(TEXT("save_error"), SaveErr);
						OutStructured->SetStringField(TEXT("receipt_status"), TEXT("failed_validation"));
						OutStructured->SetBoolField(TEXT("receipt_complete"), false);
						OutError = SaveErr.IsEmpty() ? TEXT("Blueprint repair gate passed but SaveAsset failed.") : SaveErr;
						return false;
					}
				}
				else
				{
					OutStructured->SetBoolField(TEXT("saved"), false);
				}

				if (!bGateOk)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), bCompileOk ? TEXT("blueprint_repair_gate_failed") : TEXT("blueprint_compile_failed"));
					OutError = FString::Printf(
						TEXT("Blueprint repair gate still failed for %s: compile_errors=%d blocking_repair_issues=%d."),
						*Blueprint->GetPathName(),
						ResultsLog.NumErrors,
						BlockingIssueCount);
					return false;
				}

				OutSummary = FString::Printf(TEXT("Blueprint repair gate passed for %s with zero compile errors."), *Blueprint->GetPathName());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_get_nodes"),
			TEXT("Return Blueprint graph nodes from all graphs or one graph. This override supports BlueprintFunctionLibrary assets."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Optional graph name; all graphs when omitted."))},
					{TEXT("include_pins"), FSololmcpSchemaBuilder::Boolean(TEXT("Include full pin arrays. Defaults to true."))},
					{TEXT("max_nodes"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum nodes to return. Defaults to 1000."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				bool bIncludePins = true;
				int32 MaxNodes = 1000;
				Arguments->TryGetBoolField(TEXT("include_pins"), bIncludePins);
				Arguments->TryGetNumberField(TEXT("max_nodes"), MaxNodes);
				MaxNodes = FMath::Clamp(MaxNodes, 1, 10000);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				FString GraphName;
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				TArray<UEdGraph*> Graphs;
				if (!GraphName.IsEmpty())
				{
					UEdGraph* Graph = FindBlueprintGraphByNameLocal(Blueprint, GraphName);
					if (!Graph)
					{
						OutError = FString::Printf(TEXT("Graph '%s' not found."), *GraphName);
						return false;
					}
					Graphs.Add(Graph);
				}
				else
				{
					Blueprint->GetAllGraphs(Graphs);
				}

				TArray<TSharedPtr<FJsonValue>> Nodes;
				TArray<TSharedPtr<FJsonValue>> GraphSummaries;
				bool bTruncated = false;
				for (UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
					GraphJson->SetStringField(TEXT("name"), Graph->GetName());
					GraphJson->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
					GraphSummaries.Add(MakeShared<FJsonValueObject>(GraphJson));

					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node)
						{
							continue;
						}
						if (Nodes.Num() >= MaxNodes)
						{
							bTruncated = true;
							break;
						}
						TSharedRef<FJsonObject> NodeJson = BlueprintNodeDetailsToJson(Node);
						if (!bIncludePins)
						{
							NodeJson->RemoveField(TEXT("pins"));
						}
						Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
					}
				}

				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetArrayField(TEXT("graphs"), GraphSummaries);
				OutStructured->SetArrayField(TEXT("nodes"), Nodes);
				OutStructured->SetNumberField(TEXT("graph_count"), Graphs.Num());
				OutStructured->SetNumberField(TEXT("node_count"), Nodes.Num());
				OutStructured->SetBoolField(TEXT("truncated"), bTruncated);
				OutStructured->SetBoolField(TEXT("blueprint_function_library_supported"), true);
				OutSummary = FString::Printf(TEXT("Read %d Blueprint node(s) across %d graph(s)."), Nodes.Num(), Graphs.Num());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_get_node_details"),
			TEXT("Read one Blueprint node by guid/id with reliable schema, pins, links, and call/DataTable diagnostics."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_guid"), FSololmcpSchemaBuilder::String(TEXT("Node GUID. Alias: node_id."))},
					{TEXT("node_id"), FSololmcpSchemaBuilder::String(TEXT("Alias for node_guid."))},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Optional graph limiter."))},
					{TEXT("node_title"), FSololmcpSchemaBuilder::String(TEXT("Optional title fallback."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				FString NodeGuid;
				FString NodeTitle;
				FString GraphName;
				Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid);
				if (NodeGuid.IsEmpty())
				{
					Arguments->TryGetStringField(TEXT("node_id"), NodeGuid);
				}
				Arguments->TryGetStringField(TEXT("node_title"), NodeTitle);
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				if (NodeGuid.IsEmpty() && NodeTitle.IsEmpty())
				{
					OutError = TEXT("Missing node_guid/node_id or node_title.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				UEdGraphNode* Node = nullptr;
				if (!GraphName.IsEmpty())
				{
					UEdGraph* Graph = FindBlueprintGraphByNameLocal(Blueprint, GraphName);
					if (!Graph)
					{
						OutError = FString::Printf(TEXT("Graph '%s' not found."), *GraphName);
						return false;
					}
					Node = FindNodeByStableIdOrTitle(Graph, NodeGuid, NodeTitle);
				}
				else if (!NodeGuid.IsEmpty())
				{
					Node = FindBlueprintNodeByGuidLocal(Blueprint, NodeGuid);
				}
				if (!Node && !NodeTitle.IsEmpty())
				{
					TArray<UEdGraph*> Graphs;
					Blueprint->GetAllGraphs(Graphs);
					for (UEdGraph* Graph : Graphs)
					{
						Node = FindNodeByStableIdOrTitle(Graph, FString(), NodeTitle);
						if (Node)
						{
							break;
						}
					}
				}
				if (!Node)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("node_not_found"));
					OutStructured->SetStringField(TEXT("requested_node_guid"), NodeGuid);
					OutStructured->SetStringField(TEXT("requested_node_title"), NodeTitle);
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}

				OutStructured = BlueprintNodeDetailsToJson(Node);
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetBoolField(TEXT("node_details_schema_repaired"), true);
				OutSummary = FString::Printf(TEXT("Read Blueprint node details for %s."), *Node->NodeGuid.ToString());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_find_nodes"),
			TEXT("Find Blueprint nodes by text, class, target function, or pin name across all graphs."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Optional graph limiter."))},
					{TEXT("query"), FSololmcpSchemaBuilder::String(TEXT("Text query over title/name/class/comment."))},
					{TEXT("class_name"), FSololmcpSchemaBuilder::String(TEXT("Optional class/path contains filter."))},
					{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("Optional target function contains filter."))},
					{TEXT("pin_name"), FSololmcpSchemaBuilder::String(TEXT("Optional pin/display name contains filter."))},
					{TEXT("include_pins"), FSololmcpSchemaBuilder::Boolean(TEXT("Include full pin arrays. Defaults to false."))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum results. Defaults to 100."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				FString GraphName;
				FString Query;
				FString ClassName;
				FString FunctionName;
				FString PinName;
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				Arguments->TryGetStringField(TEXT("query"), Query);
				Arguments->TryGetStringField(TEXT("class_name"), ClassName);
				Arguments->TryGetStringField(TEXT("function_name"), FunctionName);
				Arguments->TryGetStringField(TEXT("pin_name"), PinName);

				bool bIncludePins = false;
				int32 MaxResults = 100;
				Arguments->TryGetBoolField(TEXT("include_pins"), bIncludePins);
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 10000);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				TArray<UEdGraph*> Graphs;
				if (!GraphName.IsEmpty())
				{
					UEdGraph* Graph = FindBlueprintGraphByNameLocal(Blueprint, GraphName);
					if (!Graph)
					{
						OutError = FString::Printf(TEXT("Graph '%s' not found."), *GraphName);
						return false;
					}
					Graphs.Add(Graph);
				}
				else
				{
					Blueprint->GetAllGraphs(Graphs);
				}

				TArray<TSharedPtr<FJsonValue>> Matches;
				bool bTruncated = false;
				const FString QueryLower = Query.ToLower();
				const FString ClassNameLower = ClassName.ToLower();
				const FString FunctionNameLower = FunctionName.ToLower();
				const FString PinNameLower = PinName.ToLower();
				for (UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node || !NodeMatchesFindRequest(Node, QueryLower, ClassNameLower, FunctionNameLower, PinNameLower))
						{
							continue;
						}
						if (Matches.Num() >= MaxResults)
						{
							bTruncated = true;
							break;
						}
						TSharedRef<FJsonObject> NodeJson = BlueprintNodeDetailsToJson(Node);
						if (!bIncludePins)
						{
							NodeJson->RemoveField(TEXT("pins"));
						}
						Matches.Add(MakeShared<FJsonValueObject>(NodeJson));
					}
				}

				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetArrayField(TEXT("matches"), Matches);
				OutStructured->SetNumberField(TEXT("match_count"), Matches.Num());
				OutStructured->SetBoolField(TEXT("truncated"), bTruncated);
				OutStructured->SetStringField(TEXT("query"), Query);
				OutStructured->SetStringField(TEXT("class_name"), ClassName);
				OutStructured->SetStringField(TEXT("function_name"), FunctionName);
				OutStructured->SetStringField(TEXT("pin_name"), PinName);
				OutSummary = FString::Printf(TEXT("Found %d Blueprint node match(es)."), Matches.Num());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_scan_bad_struct_pins"),
			TEXT("Scan a Blueprint for bad/stale struct pins, GetDataTableRow blank table pins, and empty function graph terminators."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("include_info"), FSololmcpSchemaBuilder::Boolean(TEXT("Include informational DataTableRow upstream-validation issues. Defaults to true."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				bool bIncludeInfo = true;
				Arguments->TryGetBoolField(TEXT("include_info"), bIncludeInfo);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> Issues;
				ScanBlueprintRepairIssues(Blueprint, Issues);
				if (!bIncludeInfo)
				{
					TArray<TSharedPtr<FJsonValue>> Filtered;
					for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
					{
						const TSharedPtr<FJsonObject>* IssueObj = nullptr;
						if (IssueValue.IsValid() && IssueValue->TryGetObject(IssueObj) && IssueObj && IssueObj->IsValid())
						{
							FString Severity;
							(*IssueObj)->TryGetStringField(TEXT("severity"), Severity);
							if (!Severity.Equals(TEXT("info"), ESearchCase::IgnoreCase))
							{
								Filtered.Add(IssueValue);
							}
						}
					}
					Issues = Filtered;
				}

				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetArrayField(TEXT("issues"), Issues);
				OutStructured->SetNumberField(TEXT("issue_count"), Issues.Num());
				OutStructured->SetStringField(TEXT("diagnostic_scope"), TEXT("bad_struct_pin_scanner_datatable_function_terminator"));
				OutSummary = FString::Printf(TEXT("Scanned Blueprint repair issues for %s: %d issue(s)."), *Blueprint->GetPathName(), Issues.Num());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_set_getdatatable_node_table"),
			TEXT("Transactionally set a GetDataTableRow node DataTable default object, validate row struct/row name, refresh node pins, and optionally compile."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_table_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("row_name"), FSololmcpSchemaBuilder::String(TEXT("Optional row name to validate/set."))},
					{TEXT("break_existing_links"), FSololmcpSchemaBuilder::Boolean(TEXT("Break existing DataTable pin links before setting the default. Defaults to false."))},
					{TEXT("allow_missing_row"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow row_name not present in table. Defaults to false."))},
					{TEXT("compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Compile after mutation. Defaults to true."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save asset after a successful repair. Defaults to false."))}
				},
				{TEXT("asset_path"), TEXT("node_guid"), TEXT("data_table_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuid;
				FString DataTablePath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("data_table_path"), DataTablePath))
				{
					OutError = TEXT("Missing asset_path, node_guid, or data_table_path.");
					return false;
				}
				FString RowName;
				Arguments->TryGetStringField(TEXT("row_name"), RowName);
				bool bBreakExistingLinks = false;
				bool bAllowMissingRow = false;
				bool bCompile = true;
				bool bSave = false;
				Arguments->TryGetBoolField(TEXT("break_existing_links"), bBreakExistingLinks);
				Arguments->TryGetBoolField(TEXT("allow_missing_row"), bAllowMissingRow);
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);
				Arguments->TryGetBoolField(TEXT("save"), bSave);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuidLocal(Blueprint, NodeGuid);
				if (!Node)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("node_not_found"));
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				if (!IsGetDataTableRowNode(Node))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("not_getdatatable_node"));
					OutStructured->SetObjectField(TEXT("node"), BlueprintNodeDetailsToJson(Node));
					OutError = TEXT("Node is not a GetDataTableRow node.");
					return false;
				}
				UEdGraphPin* DataTablePin = FindGetDataTableRowDataTablePin(Node);
				if (!DataTablePin)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_pin_not_found"));
					OutStructured->SetObjectField(TEXT("node"), BlueprintNodeDetailsToJson(Node));
					OutError = TEXT("GetDataTableRow DataTable pin was not found.");
					return false;
				}
				if (DataTablePin->LinkedTo.Num() > 0 && !bBreakExistingLinks)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_pin_has_links"));
					OutStructured->SetObjectField(TEXT("data_table_pin"), MakePinRefJson(DataTablePin));
					OutStructured->SetStringField(TEXT("failure_hint"), TEXT("Set break_existing_links=true only after confirming the upstream RowHandle link is stale."));
					OutError = TEXT("DataTable pin already has links; refusing to overwrite default object without break_existing_links=true.");
					return false;
				}

				FString LoadError;
				UDataTable* DataTable = Cast<UDataTable>(LoadObjectAssetForBlueprintTool(Context.Services, DataTablePath, LoadError));
				if (!DataTable)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_asset_not_found"));
					OutError = LoadError.IsEmpty() ? TEXT("data_table_path did not resolve to a UDataTable.") : LoadError;
					return false;
				}
				if (!DataTable->RowStruct)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_missing_row_struct"));
					OutError = TEXT("DataTable has no RowStruct.");
					return false;
				}

				UEdGraphPin* ExistingRowNamePin = FindGetDataTableRowRowNamePin(Node);
				if (RowName.IsEmpty() && ExistingRowNamePin)
				{
					RowName = ExistingRowNamePin->DefaultValue;
				}
				const bool bRowNameProvided = !RowName.IsEmpty();
				const bool bRowExists = !bRowNameProvided || DataTable->GetRowMap().Contains(FName(*RowName));
				if (!bRowExists && !bAllowMissingRow)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_row_not_found"));
					OutStructured->SetStringField(TEXT("row_name"), RowName);
					OutStructured->SetStringField(TEXT("data_table"), DataTable->GetPathName());
					OutError = TEXT("Requested row_name was not found in the DataTable.");
					return false;
				}

				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("graph_schema_not_k2"));
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				TSharedRef<FJsonObject> Before = BuildGetDataTableRowDiagnosticJson(Node);
				const FScopedTransaction Transaction(LOCTEXT("BPSetGetDataTableRowTable", "SOMOLMCP Set GetDataTableRow DataTable"));
				Blueprint->Modify();
				Node->Modify();
				if (bBreakExistingLinks && DataTablePin->LinkedTo.Num() > 0)
				{
					Schema->BreakPinLinks(*DataTablePin, true);
				}

				Schema->TrySetDefaultObject(*DataTablePin, DataTable, true);
				if (UK2Node_GetDataTableRow* RowNode = Cast<UK2Node_GetDataTableRow>(Node))
				{
					RowNode->PinDefaultValueChanged(DataTablePin);
					RowNode->OnDataTableRowListChanged(DataTable);
					RowNode->ReconstructNode();
				}

				DataTablePin = FindGetDataTableRowDataTablePin(Node);
				if (DataTablePin && DataTablePin->DefaultObject != DataTable)
				{
					Schema->TrySetDefaultObject(*DataTablePin, DataTable, true);
				}

				if (bRowNameProvided)
				{
					if (UEdGraphPin* RowNamePin = FindGetDataTableRowRowNamePin(Node))
					{
						Schema->TrySetDefaultValue(*RowNamePin, RowName, true);
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (Node->GetGraph())
				{
					Node->GetGraph()->NotifyGraphChanged();
				}

				FCompilerResultsLog ResultsLog;
				ResultsLog.bSilentMode = true;
				ResultsLog.bAnnotateMentionedNodes = true;
				if (bCompile)
				{
					FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
				}

				UEdGraphPin* DataTablePinAfter = FindGetDataTableRowDataTablePin(Node);
				UEdGraphPin* ResultPinAfter = FindGetDataTableRowResultPin(Node);
				const bool bDefaultObjectVerified = DataTablePinAfter && DataTablePinAfter->DefaultObject == DataTable;
				const bool bReturnStructMatches = ResultPinAfter &&
					ResultPinAfter->PinType.PinSubCategoryObject.IsValid() &&
					ResultPinAfter->PinType.PinSubCategoryObject.Get() == DataTable->RowStruct;

				TArray<TSharedPtr<FJsonValue>> Messages;
				for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
				{
					Messages.Add(MakeShared<FJsonValueObject>(CompilerMessageToJson(Message)));
				}

				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				OutStructured->SetObjectField(TEXT("before"), Before);
				OutStructured->SetObjectField(TEXT("after"), BuildGetDataTableRowDiagnosticJson(Node));
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
				OutStructured->SetStringField(TEXT("data_table"), DataTable->GetPathName());
				OutStructured->SetStringField(TEXT("row_struct"), DataTable->RowStruct->GetPathName());
				OutStructured->SetStringField(TEXT("row_name"), RowName);
				OutStructured->SetBoolField(TEXT("row_name_checked"), bRowNameProvided);
				OutStructured->SetBoolField(TEXT("row_exists"), bRowExists);
				OutStructured->SetBoolField(TEXT("broke_existing_links"), bBreakExistingLinks);
				OutStructured->SetBoolField(TEXT("default_object_verified"), bDefaultObjectVerified);
				OutStructured->SetBoolField(TEXT("return_struct_matches_datatable"), bReturnStructMatches);
				OutStructured->SetBoolField(TEXT("compile_requested"), bCompile);
				OutStructured->SetBoolField(TEXT("compile_ok"), !bCompile || (Blueprint->Status != BS_Error && ResultsLog.NumErrors == 0));
				OutStructured->SetArrayField(TEXT("compiler_messages"), Messages);
				OutStructured->SetNumberField(TEXT("compiler_error_count"), ResultsLog.NumErrors);
				OutStructured->SetNumberField(TEXT("compiler_warning_count"), ResultsLog.NumWarnings);
				OutStructured->SetBoolField(TEXT("transactional_repair"), true);

				if (!bDefaultObjectVerified)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_default_object_readback_failed"));
					OutError = TEXT("DataTable default object readback failed.");
					return false;
				}
				if (!bReturnStructMatches)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_return_struct_mismatch"));
					OutError = TEXT("GetDataTableRow return struct did not match the DataTable RowStruct after repair.");
					return false;
				}
				if (bCompile && (Blueprint->Status == BS_Error || ResultsLog.NumErrors > 0))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("datatable_repair_compile_failed"));
					OutError = TEXT("GetDataTableRow DataTable repair applied, but Blueprint still fails compile.");
					return false;
				}
				if (!RunBlueprintGraphMutationGate(Blueprint, Node->GetGraph(), OutStructured, OutError))
				{
					return false;
				}
				if (bSave)
				{
					FString SaveErr;
					const bool bSaved = Context.Services.SaveAsset(AssetPath, false, SaveErr);
					OutStructured->SetBoolField(TEXT("saved"), bSaved);
					if (!bSaved)
					{
						OutError = SaveErr.IsEmpty() ? TEXT("Failed to save Blueprint after GetDataTableRow repair.") : SaveErr;
						return false;
					}
				}
				else
				{
					OutStructured->SetBoolField(TEXT("saved"), false);
				}

				OutSummary = FString::Printf(TEXT("Set GetDataTableRow DataTable on %s.%s to %s."),
					*Blueprint->GetPathName(),
					Node->GetGraph() ? *Node->GetGraph()->GetName() : TEXT(""),
					*DataTable->GetPathName());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_finalize_function_graph"),
			TEXT("Ensure a function graph has a FunctionResult terminator and connect FunctionEntry exec to it when the entry is otherwise unlinked."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Function graph name. Alias: function_name."))},
					{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("Alias for graph_name."))},
					{TEXT("compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Compile after finalizing. Defaults to true."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after successful finalize/compile. Defaults to false."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				if (!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
				{
					Arguments->TryGetStringField(TEXT("function_name"), GraphName);
				}
				if (GraphName.IsEmpty())
				{
					OutError = TEXT("Missing graph_name or function_name.");
					return false;
				}
				bool bCompile = true;
				bool bSave = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);
				Arguments->TryGetBoolField(TEXT("save"), bSave);

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				if (!IsBlueprintFunctionGraphRequiringTerminator(Blueprint, Graph))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("not_a_function_graph"));
					OutStructured->SetObjectField(TEXT("graph"), BlueprintGraphToCompactSummary(Graph));
					OutStructured->SetStringField(TEXT("asset_path"), Blueprint ? Blueprint->GetPathName() : AssetPath);
					OutStructured->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : GraphName);
					OutStructured->SetBoolField(TEXT("mutation_blocked"), true);
					OutError = TEXT("Graph is not a Blueprint function graph; refusing to add FunctionResult to event, construction, or macro graphs.");
					return false;
				}

				UK2Node_FunctionEntry* EntryNode = FindPrimaryFunctionEntryNode(Graph);
				if (!EntryNode)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("function_entry_not_found"));
					OutStructured->SetObjectField(TEXT("graph"), BlueprintGraphToCompactSummary(Graph));
					OutError = TEXT("FunctionEntry node was not found in the graph.");
					return false;
				}

				TSharedRef<FJsonObject> Before = BuildFunctionGraphTerminatorDiagnostic(Graph);
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
				if (!Schema)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("graph_schema_not_k2"));
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				TArray<UK2Node_FunctionResult*> ResultNodes;
				CollectFunctionResultNodes(Graph, ResultNodes);
				UK2Node_FunctionResult* ResultNode = ResultNodes.Num() > 0 ? ResultNodes[0] : nullptr;
				bool bCreatedReturnNode = false;
				bool bConnectedEntryToReturn = false;

				const FScopedTransaction Transaction(LOCTEXT("BPFinalizeFunctionGraph", "SOMOLMCP Finalize Blueprint Function Graph"));
				Blueprint->Modify();
				Graph->Modify();
				EntryNode->Modify();

				if (!ResultNode)
				{
					FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Graph);
					ResultNode = ResultCreator.CreateNode();
					ResultNode->FunctionReference = EntryNode->FunctionReference;
					if (ResultNode->FunctionReference.GetMemberName().IsNone())
					{
						ResultNode->FunctionReference.SetSelfMember(Graph->GetFName());
					}
					ResultNode->NodePosX = EntryNode->NodePosX + EntryNode->NodeWidth + 256;
					ResultNode->NodePosY = EntryNode->NodePosY;
					ResultCreator.Finalize();
					bCreatedReturnNode = ResultNode != nullptr;
				}

				UEdGraphPin* EntryExec = FindExecutionPinLocal(EntryNode, EGPD_Output);
				UEdGraphPin* ResultExec = FindExecutionPinLocal(ResultNode, EGPD_Input);
				if (EntryExec && ResultExec && !EntryExec->LinkedTo.Contains(ResultExec))
				{
					if (EntryExec->LinkedTo.Num() == 0)
					{
						EntryNode->Modify();
						ResultNode->Modify();
						bConnectedEntryToReturn = Schema->TryCreateConnection(EntryExec, ResultExec);
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Graph->NotifyGraphChanged();

				FCompilerResultsLog ResultsLog;
				ResultsLog.bSilentMode = true;
				ResultsLog.bAnnotateMentionedNodes = true;
				if (bCompile)
				{
					FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
				}

				TArray<TSharedPtr<FJsonValue>> Messages;
				for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
				{
					Messages.Add(MakeShared<FJsonValueObject>(CompilerMessageToJson(Message)));
				}

				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				OutStructured->SetObjectField(TEXT("before"), Before);
				OutStructured->SetObjectField(TEXT("after"), BuildFunctionGraphTerminatorDiagnostic(Graph));
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("graph_name"), Graph->GetName());
				OutStructured->SetStringField(TEXT("entry_node_guid"), EntryNode->NodeGuid.ToString());
				OutStructured->SetStringField(TEXT("return_node_guid"), ResultNode ? ResultNode->NodeGuid.ToString() : FString());
				OutStructured->SetBoolField(TEXT("created_return_node"), bCreatedReturnNode);
				OutStructured->SetBoolField(TEXT("connected_entry_to_return"), bConnectedEntryToReturn);
				OutStructured->SetBoolField(TEXT("function_graph_finalized"), ResultNode != nullptr && (EntryExec == nullptr || EntryExec->LinkedTo.Num() > 0));
				OutStructured->SetBoolField(TEXT("compile_requested"), bCompile);
				OutStructured->SetBoolField(TEXT("compile_ok"), !bCompile || (Blueprint->Status != BS_Error && ResultsLog.NumErrors == 0));
				OutStructured->SetArrayField(TEXT("compiler_messages"), Messages);
				OutStructured->SetNumberField(TEXT("compiler_error_count"), ResultsLog.NumErrors);
				OutStructured->SetNumberField(TEXT("compiler_warning_count"), ResultsLog.NumWarnings);

				bool bFinalized = false;
				OutStructured->TryGetBoolField(TEXT("function_graph_finalized"), bFinalized);
				if (!bFinalized)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("function_graph_finalizer_readback_failed"));
					OutError = TEXT("Function graph finalizer did not produce a connected terminator readback.");
					return false;
				}
				if (bCompile && (Blueprint->Status == BS_Error || ResultsLog.NumErrors > 0))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("function_graph_finalizer_compile_failed"));
					OutError = TEXT("Function graph finalizer ran, but Blueprint still fails compile.");
					return false;
				}
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				if (bSave)
				{
					FString SaveErr;
					const bool bSaved = Context.Services.SaveAsset(AssetPath, false, SaveErr);
					OutStructured->SetBoolField(TEXT("saved"), bSaved);
					if (!bSaved)
					{
						OutError = SaveErr.IsEmpty() ? TEXT("Failed to save Blueprint after function graph finalize.") : SaveErr;
						return false;
					}
				}
				else
				{
					OutStructured->SetBoolField(TEXT("saved"), false);
				}

				OutSummary = FString::Printf(TEXT("Finalized Blueprint function graph %s.%s."), *Blueprint->GetPathName(), *Graph->GetName());
				return true;
			},
			nullptr,
			5
		});

		Registry.Register({
			TEXT("blueprint_compile"),
			TEXT("Compile a Blueprint and return a K2 authoring receipt with graph/link readback and failure diagnostics."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				const FString CompileStatus = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
				const bool bCompileOk = Blueprint->Status != BS_Error;
				OutStructured->SetBoolField(TEXT("compile_ok"), bCompileOk);
				OutStructured->SetBoolField(TEXT("receipt_complete"), true);
				OutStructured->SetStringField(TEXT("diagnostic_scope"), TEXT("status_graph_link_receipt"));
				if (!bCompileOk)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("blueprint_compile_failed"));
					OutStructured->SetStringField(TEXT("failure_hint"), TEXT("Use blueprint_read_graph_summary or blueprint_graph_explain to inspect broken nodes and pin links, then rerun blueprint_compile."));
					OutError = FString::Printf(TEXT("Blueprint compile failed: %s (%s)."), *AssetPath, *CompileStatus);
					return false;
				}

				OutSummary = FString::Printf(TEXT("Compiled blueprint %s (%s)."), *AssetPath, *CompileStatus);
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_refresh_all_nodes"),
			TEXT("Refresh all K2 nodes in a Blueprint, then optionally compile and save. Useful for migrated assets with stale pins, unknown structs, or reconstructed nodes."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))},
					{TEXT("compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Compile after refreshing nodes. Defaults to true."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save asset after refresh/compile. Defaults to false."))},
					{TEXT("structural"), FSololmcpSchemaBuilder::Boolean(TEXT("Mark structurally modified instead of ordinary modified. Defaults to false."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				bool bCompile = true;
				bool bSave = false;
				bool bStructural = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);
				Arguments->TryGetBoolField(TEXT("save"), bSave);
				Arguments->TryGetBoolField(TEXT("structural"), bStructural);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				const FString StatusBefore = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
				const int32 NodesBefore = BuildBlueprintCompileReceipt(Blueprint)->GetIntegerField(TEXT("node_count"));

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRefreshAllNodes", "SOMOLMCP Refresh All Blueprint Nodes"));
				Blueprint->Modify();
				FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
				if (bStructural)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				}
				else
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				}

				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				}

				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				const FString StatusAfter = StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status));
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("status_before"), StatusBefore);
				OutStructured->SetStringField(TEXT("status_after"), StatusAfter);
				OutStructured->SetNumberField(TEXT("nodes_before"), NodesBefore);
				OutStructured->SetBoolField(TEXT("refreshed_all_nodes"), true);
				OutStructured->SetBoolField(TEXT("compile_requested"), bCompile);
				OutStructured->SetBoolField(TEXT("save_requested"), bSave);
				OutStructured->SetBoolField(TEXT("structural_requested"), bStructural);
				OutStructured->SetBoolField(TEXT("compile_ok"), !bCompile || Blueprint->Status != BS_Error);
				OutStructured->SetBoolField(TEXT("receipt_complete"), true);

				if (bSave)
				{
					FString SaveErr;
					const bool bSaved = Context.Services.SaveAsset(AssetPath, false, SaveErr);
					OutStructured->SetBoolField(TEXT("saved"), bSaved);
					if (!bSaved)
					{
						OutStructured->SetStringField(TEXT("save_error"), SaveErr);
						OutError = SaveErr.IsEmpty() ? TEXT("Failed to save Blueprint after refresh.") : SaveErr;
						return false;
					}
				}
				else
				{
					OutStructured->SetBoolField(TEXT("saved"), false);
				}

				if (bCompile && Blueprint->Status == BS_Error)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("blueprint_refresh_compile_failed"));
					OutStructured->SetStringField(TEXT("failure_hint"), TEXT("RefreshAllNodes ran, but compile still failed. Inspect missing variables, bad casts, unknown structs, or delegate signatures before saving."));
					OutError = FString::Printf(TEXT("Blueprint refresh ran but compile still failed: %s (%s)."), *AssetPath, *StatusAfter);
					return false;
				}

				OutSummary = FString::Printf(TEXT("Refreshed Blueprint nodes for %s (%s -> %s)."), *AssetPath, *StatusBefore, *StatusAfter);
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_migration_repair_plan"),
			TEXT("Inspect a migrated Blueprint for stale unknown type pins, one-way links, parent-class state, and compile status. Returns a dry-run repair plan for Agent/Hermes routing."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))},
					{TEXT("compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Compile before inspection. Defaults to true."))},
					{TEXT("refresh_first"), FSololmcpSchemaBuilder::Boolean(TEXT("Run RefreshAllNodes before compile/inspection. Defaults to false."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				bool bCompile = true;
				bool bRefreshFirst = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);
				Arguments->TryGetBoolField(TEXT("refresh_first"), bRefreshFirst);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				if (bRefreshFirst)
				{
					Blueprint->Modify();
					FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				}
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				}

				TArray<TSharedPtr<FJsonValue>> Issues;
				ScanBlueprintMigrationIssues(Blueprint, Issues);

				TArray<TSharedPtr<FJsonValue>> Plan;
				auto AddPlan = [&Plan](const FString& Step, const FString& Tool, const FString& Reason)
				{
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("step"), Step);
					Item->SetStringField(TEXT("tool"), Tool);
					Item->SetStringField(TEXT("reason"), Reason);
					Plan.Add(MakeShared<FJsonValueObject>(Item));
				};
				AddPlan(TEXT("watchdog_before"), TEXT("editor_dialog_watchdog_tick"), TEXT("Close blocking compile/recovery modals before mutation."));
				AddPlan(TEXT("refresh_nodes"), TEXT("blueprint_refresh_all_nodes"), TEXT("Reconstruct stale K2 pins and node signatures."));
				if (Issues.Num() > 0 || Blueprint->Status == BS_Error)
				{
					AddPlan(TEXT("repair_variables"), TEXT("blueprint_rebuild_member_variable"), TEXT("Restore missing member variables or bad variable types called out by diagnostics."));
					AddPlan(TEXT("repair_links"), TEXT("blueprint_break_pin_links / blueprint_break_single_pin_link"), TEXT("Remove stale unknown pins or one-way pin links, then reconnect with blueprint_connect_pins."));
					AddPlan(TEXT("compile_diagnostics"), TEXT("blueprint_compile_diagnostics"), TEXT("Collect compiler messages and classify remaining failure class."));
				}
				AddPlan(TEXT("compile"), TEXT("blueprint_compile"), TEXT("Verify final compile status."));
				AddPlan(TEXT("watchdog_after"), TEXT("editor_dialog_watchdog_tick"), TEXT("Ensure no modal remains after compile."));

				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutStructured->SetBoolField(TEXT("compile_ok"), Blueprint->Status != BS_Error);
				OutStructured->SetBoolField(TEXT("refreshed_first"), bRefreshFirst);
				OutStructured->SetArrayField(TEXT("issues"), Issues);
				OutStructured->SetArrayField(TEXT("repair_plan"), Plan);
				OutStructured->SetStringField(TEXT("diagnostic_scope"), TEXT("migration_graph_repair_plan"));
				OutSummary = FString::Printf(TEXT("Blueprint migration repair plan built for %s: %d issue(s)."), *AssetPath, Issues.Num());
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_rebuild_member_variable"),
			TEXT("Atomically ensure or rebuild a Blueprint member variable with a requested type, then optionally refresh, compile, and save."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))},
					{TEXT("variable_name"), FSololmcpSchemaBuilder::String(TEXT("Variable name to ensure/rebuild."))},
					{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("bool|int|float|string|name|text|vector|rotator|transform|datatable|struct|object|class|enum"))},
					{TEXT("type_object_path"), FSololmcpSchemaBuilder::String(TEXT("Required for struct/object/class/enum types."))},
					{TEXT("category"), FSololmcpSchemaBuilder::String(TEXT("Optional variable category."))},
					{TEXT("default_value"), FSololmcpSchemaBuilder::String(TEXT("Optional default value; best-effort metadata/default preservation."))},
					{TEXT("force_rebuild"), FSololmcpSchemaBuilder::Boolean(TEXT("Remove an existing variable before re-adding. Defaults to false."))},
					{TEXT("compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Compile after repair. Defaults to true."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after repair. Defaults to false."))}
				},
				{TEXT("asset_path"), TEXT("variable_name"), TEXT("type")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				FString TypeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("variable_name"), VariableName) ||
					!Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing asset_path, variable_name, or type.");
					return false;
				}

				FString TypeObjectPath;
				FString Category;
				FString DefaultValue;
				Arguments->TryGetStringField(TEXT("type_object_path"), TypeObjectPath);
				Arguments->TryGetStringField(TEXT("category"), Category);
				Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);

				bool bForceRebuild = false;
				bool bCompile = true;
				bool bSave = false;
				Arguments->TryGetBoolField(TEXT("force_rebuild"), bForceRebuild);
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);
				Arguments->TryGetBoolField(TEXT("save"), bSave);

				FEdGraphPinType PinType;
				if (!MakeRepairPinType(TypeName, TypeObjectPath, PinType, OutError))
				{
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				const FName VarName(*VariableName);
				FBPVariableDescription ExistingVariable;
				const bool bHadExisting = BlueprintHasMemberVariable(Blueprint, VarName, &ExistingVariable);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRebuildMemberVariable", "SOMOLMCP Rebuild Blueprint Member Variable"));
				Blueprint->Modify();
				if (bHadExisting && bForceRebuild)
				{
					FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VarName);
				}

				bool bAdded = false;
				bool bChangedType = false;
				if (!bHadExisting || bForceRebuild)
				{
							// UBlueprintEditorLibrary::AddMemberVariable is 5.4+. FBlueprintEditorUtils has
		// carried the same call for far longer, so 5.3 routes to it rather than losing
		// the capability.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		if (!UBlueprintEditorLibrary::AddMemberVariable(Blueprint, VarName, PinType))
#else
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType))
#endif
					{
						OutError = FString::Printf(TEXT("Failed to add member variable '%s'."), *VariableName);
						return false;
					}
					bAdded = true;
				}
				else if (ExistingVariable.VarType != PinType)
				{
					FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, VarName, PinType);
					bChangedType = true;
				}

				if (!Category.IsEmpty())
				{
					FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, static_cast<const UStruct*>(nullptr), FText::FromString(Category));
				}
				if (!DefaultValue.IsEmpty())
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, static_cast<const UStruct*>(nullptr), TEXT("SOMOLMCP_DefaultValue"), DefaultValue);
				}

				FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				}

				FBPVariableDescription ReadbackVariable;
				const bool bExistsAfter = BlueprintHasMemberVariable(Blueprint, VarName, &ReadbackVariable);
				OutStructured = BuildBlueprintCompileReceipt(Blueprint);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("variable_name"), VariableName);
				OutStructured->SetStringField(TEXT("requested_type"), TypeName);
				OutStructured->SetStringField(TEXT("requested_type_object_path"), TypeObjectPath);
				OutStructured->SetStringField(TEXT("readback_type"), bExistsAfter ? PinTypeToBriefString(ReadbackVariable.VarType) : FString());
				OutStructured->SetBoolField(TEXT("had_existing"), bHadExisting);
				OutStructured->SetBoolField(TEXT("force_rebuild"), bForceRebuild);
				OutStructured->SetBoolField(TEXT("added"), bAdded);
				OutStructured->SetBoolField(TEXT("changed_type"), bChangedType);
				OutStructured->SetBoolField(TEXT("exists_after"), bExistsAfter);
				OutStructured->SetBoolField(TEXT("compile_requested"), bCompile);
				OutStructured->SetBoolField(TEXT("compile_ok"), !bCompile || Blueprint->Status != BS_Error);

				if (!bExistsAfter)
				{
					OutError = FString::Printf(TEXT("Member variable '%s' was not present after rebuild."), *VariableName);
					return false;
				}
				if (bSave)
				{
					FString SaveErr;
					const bool bSaved = Context.Services.SaveAsset(AssetPath, false, SaveErr);
					OutStructured->SetBoolField(TEXT("saved"), bSaved);
					if (!bSaved)
					{
						OutError = SaveErr.IsEmpty() ? TEXT("Failed to save Blueprint after member variable rebuild.") : SaveErr;
						return false;
					}
				}
				else
				{
					OutStructured->SetBoolField(TEXT("saved"), false);
				}
				if (bCompile && Blueprint->Status == BS_Error)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("blueprint_rebuild_member_variable_compile_failed"));
					OutError = FString::Printf(TEXT("Rebuilt member variable but Blueprint still fails compile: %s"), *AssetPath);
					return false;
				}

				OutSummary = FString::Printf(TEXT("Ensured Blueprint member variable '%s' on %s."), *VariableName, *AssetPath);
				return true;
			}
		});

		Registry.Register({
			TEXT("umg_drag_drop_repair"),
			TEXT("Repair and verify a UMG DragDropOperation Blueprint parent, then refresh/compile dependent widgets when requested."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("operation_asset_path"), FSololmcpSchemaBuilder::String(TEXT("DragDropOperation Blueprint path."))},
					{TEXT("dependent_widget_path"), FSololmcpSchemaBuilder::String(TEXT("Optional widget Blueprint that references the operation."))},
					{TEXT("compile"), FSololmcpSchemaBuilder::Boolean(TEXT("Compile operation and dependent widget. Defaults to true."))},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save changed assets. Defaults to false."))}
				},
				{TEXT("operation_asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString OperationPath;
				if (!Arguments->TryGetStringField(TEXT("operation_asset_path"), OperationPath))
				{
					OutError = TEXT("Missing operation_asset_path.");
					return false;
				}
				FString DependentWidgetPath;
				Arguments->TryGetStringField(TEXT("dependent_widget_path"), DependentWidgetPath);

				bool bCompile = true;
				bool bSave = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);
				Arguments->TryGetBoolField(TEXT("save"), bSave);

				UObject* OperationAsset = Context.Services.LoadAsset(OperationPath, OutError);
				UBlueprint* OperationBlueprint = Cast<UBlueprint>(OperationAsset);
				if (!OperationBlueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("operation_asset_path is not a Blueprint.") : OutError;
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> Steps;
				auto AddStep = [&Steps](const FString& Step, const FString& Status, const FString& Detail)
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("step"), Step);
					Obj->SetStringField(TEXT("status"), Status);
					Obj->SetStringField(TEXT("detail"), Detail);
					Steps.Add(MakeShared<FJsonValueObject>(Obj));
				};

				const UClass* ParentBefore = OperationBlueprint->ParentClass;
				bool bReparented = false;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgDragDropRepair", "SOMOLMCP Repair UMG Drag Drop Operation"));
				OperationBlueprint->Modify();
				if (!OperationBlueprint->ParentClass || !OperationBlueprint->ParentClass->IsChildOf(UDragDropOperation::StaticClass()))
				{
					UBlueprintEditorLibrary::ReparentBlueprint(OperationBlueprint, UDragDropOperation::StaticClass());
					bReparented = true;
					AddStep(TEXT("reparent_operation"), TEXT("applied"), TEXT("Operation Blueprint was reparented to /Script/UMG.DragDropOperation."));
				}
				else
				{
					AddStep(TEXT("reparent_operation"), TEXT("skipped"), TEXT("Operation Blueprint already inherits UDragDropOperation."));
				}

				FBlueprintEditorUtils::RefreshAllNodes(OperationBlueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(OperationBlueprint);
				}

				UBlueprint* DependentWidgetBlueprint = nullptr;
				if (!DependentWidgetPath.IsEmpty())
				{
					FString LoadErr;
					DependentWidgetBlueprint = Cast<UBlueprint>(Context.Services.LoadAsset(DependentWidgetPath, LoadErr));
					if (!DependentWidgetBlueprint)
					{
						AddStep(TEXT("dependent_widget"), TEXT("failed"), LoadErr.IsEmpty() ? TEXT("Dependent widget is not a Blueprint.") : LoadErr);
					}
					else
					{
						DependentWidgetBlueprint->Modify();
						FBlueprintEditorUtils::RefreshAllNodes(DependentWidgetBlueprint);
						if (bCompile)
						{
							UBlueprintEditorLibrary::CompileBlueprint(DependentWidgetBlueprint);
						}
						AddStep(TEXT("dependent_widget_refresh"), TEXT("applied"), TEXT("Dependent widget nodes refreshed and optionally compiled."));
					}
				}

				const bool bOperationOk = OperationBlueprint->ParentClass && OperationBlueprint->ParentClass->IsChildOf(UDragDropOperation::StaticClass());
				const bool bOperationCompileOk = !bCompile || OperationBlueprint->Status != BS_Error;
				const bool bDependentCompileOk = !DependentWidgetBlueprint || !bCompile || DependentWidgetBlueprint->Status != BS_Error;

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("operation_asset_path"), OperationPath);
				OutStructured->SetStringField(TEXT("parent_before"), ParentBefore ? ParentBefore->GetPathName() : FString());
				OutStructured->SetStringField(TEXT("parent_after"), OperationBlueprint->ParentClass ? OperationBlueprint->ParentClass->GetPathName() : FString());
				OutStructured->SetBoolField(TEXT("reparented"), bReparented);
				OutStructured->SetBoolField(TEXT("inherits_drag_drop_operation"), bOperationOk);
				OutStructured->SetStringField(TEXT("operation_compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(OperationBlueprint->Status)));
				OutStructured->SetStringField(TEXT("dependent_widget_path"), DependentWidgetPath);
				if (DependentWidgetBlueprint)
				{
					OutStructured->SetStringField(TEXT("dependent_compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(DependentWidgetBlueprint->Status)));
				}
				OutStructured->SetArrayField(TEXT("steps"), Steps);
				OutStructured->SetBoolField(TEXT("compile_ok"), bOperationCompileOk && bDependentCompileOk);

				if (bSave)
				{
					FString SaveErr;
					const bool bSavedOperation = Context.Services.SaveAsset(OperationPath, false, SaveErr);
					OutStructured->SetBoolField(TEXT("operation_saved"), bSavedOperation);
					if (!bSavedOperation)
					{
						OutError = SaveErr.IsEmpty() ? TEXT("Failed to save DragDropOperation Blueprint.") : SaveErr;
						return false;
					}
					if (DependentWidgetBlueprint && !DependentWidgetPath.IsEmpty())
					{
						FString WidgetSaveErr;
						const bool bSavedWidget = Context.Services.SaveAsset(DependentWidgetPath, false, WidgetSaveErr);
						OutStructured->SetBoolField(TEXT("dependent_saved"), bSavedWidget);
						if (!bSavedWidget)
						{
							OutError = WidgetSaveErr.IsEmpty() ? TEXT("Failed to save dependent widget Blueprint.") : WidgetSaveErr;
							return false;
						}
					}
				}

				if (!bOperationOk)
				{
					OutError = TEXT("DragDropOperation Blueprint parent repair failed.");
					return false;
				}
				if (!bOperationCompileOk || !bDependentCompileOk)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("umg_drag_drop_repair_compile_failed"));
					OutError = TEXT("UMG drag/drop repair ran, but one or more Blueprints still fail compile.");
					return false;
				}

				OutSummary = FString::Printf(TEXT("UMG drag/drop repair verified for %s."), *OperationPath);
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_add_function_graph"),
			TEXT("Add a function graph to a Blueprint and return graph readback receipt."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("function_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Alias for function_name."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString FunctionName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				if (!Arguments->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
				{
					Arguments->TryGetStringField(TEXT("graph_name"), FunctionName);
				}
				if (FunctionName.IsEmpty())
				{
					OutError = TEXT("Missing function_name or graph_name.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				if (FindBlueprintGraphByNameLocal(Blueprint, FunctionName))
				{
					OutError = FString::Printf(TEXT("Blueprint graph '%s' already exists."), *FunctionName);
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPAddFunctionGraphP0", "SOMOLMCP Add Blueprint Function Graph"));
				Blueprint->Modify();
				UEdGraph* Graph = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, FunctionName);
				if (!Graph || !FindBlueprintGraphByNameLocal(Blueprint, FunctionName))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("function_graph_create_failed"));
					OutError = TEXT("Failed to add function graph.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("graph_name"), Graph->GetName());
				OutStructured->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetPathName());
				OutStructured->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
				OutStructured->SetBoolField(TEXT("graph_verified"), true);
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("add_function_graph"),
					TEXT("graph_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(TEXT("Added blueprint function graph '%s'."), *Graph->GetName());
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_add_function_call_node"),
			TEXT("Add a K2 function call node to a Blueprint graph with binding and pin readback diagnostics."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("class_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("function_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("class_path"), TEXT("function_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return TryAddFunctionCallNode(
					Context,
					Arguments,
					FString(),
					FString(),
					LOCTEXT("BPAddFunctionCallP0", "SOMOLMCP Add Blueprint Function Call Node"),
					OutStructured,
					OutSummary,
					OutError);
			}
		});

		Registry.Register({
			TEXT("blueprint_add_print_string_node"),
			TEXT("Add a Kismet PrintString call node and verify the InString data pin exists."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("in_string"), FSololmcpSchemaBuilder::String(TEXT("Optional default value for PrintString InString."))},
					{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const FString ClassPath = UKismetSystemLibrary::StaticClass()->GetPathName();
				const bool bOk = TryAddFunctionCallNode(
					Context,
					Arguments,
					ClassPath,
					TEXT("PrintString"),
					LOCTEXT("BPAddPrintStringP0", "SOMOLMCP Add Blueprint PrintString Node"),
					OutStructured,
					OutSummary,
					OutError);
				bool bPrintStringDataPinPresent = false;
				OutStructured->TryGetBoolField(TEXT("print_string_data_pin_present"), bPrintStringDataPinPresent);
				if (bOk && !bPrintStringDataPinPresent)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("print_string_missing_instring_pin"));
					OutError = TEXT("PrintString node did not expose the InString data pin.");
					return false;
				}
				return bOk;
			}
		});

		Registry.Register({
			TEXT("blueprint_add_node"),
			TEXT("Generic K2 node creation dispatcher for function_call and print_string nodes."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_kind"), FSololmcpSchemaBuilder::String(TEXT("function_call | print_string"))},
					{TEXT("class_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("function_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("in_string"), FSololmcpSchemaBuilder::String()},
					{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("node_kind")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString NodeKind;
				Arguments->TryGetStringField(TEXT("node_kind"), NodeKind);
				NodeKind = NodeKind.ToLower();
				if (NodeKind == TEXT("print_string") || NodeKind == TEXT("printstring"))
				{
					return TryAddFunctionCallNode(
						Context,
						Arguments,
						UKismetSystemLibrary::StaticClass()->GetPathName(),
						TEXT("PrintString"),
						LOCTEXT("BPAddGenericPrintStringP0", "SOMOLMCP Add Generic PrintString Node"),
						OutStructured,
						OutSummary,
						OutError);
				}
				if (NodeKind == TEXT("function_call") || NodeKind == TEXT("call_function"))
				{
					return TryAddFunctionCallNode(
						Context,
						Arguments,
						FString(),
						FString(),
						LOCTEXT("BPAddGenericFunctionCallP0", "SOMOLMCP Add Generic Function Call Node"),
						OutStructured,
						OutSummary,
						OutError);
				}
				OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("unsupported_node_kind"));
				OutStructured->SetStringField(TEXT("supported_node_kinds"), TEXT("function_call, print_string"));
				OutError = FString::Printf(TEXT("Unsupported node_kind '%s'."), *NodeKind);
				return false;
			}
		});

		Registry.Register({
			TEXT("blueprint_connect_pins"),
			TEXT("Connect Blueprint exec or data pins with schema diagnostics and readback verification."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("from_node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("from_pin_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("from_pin_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("to_node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("to_pin_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("to_pin_guid"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("from_node_guid"), TEXT("from_pin_name"), TEXT("to_node_guid"), TEXT("to_pin_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				UEdGraphNode* FromNode = nullptr;
				UEdGraphNode* ToNode = nullptr;
				UEdGraphPin* FromPin = nullptr;
				UEdGraphPin* ToPin = nullptr;
				if (!ResolveConnectionPins(Blueprint, Arguments, FromNode, FromPin, ToNode, ToPin, OutStructured, OutError))
				{
					return false;
				}

				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(FromNode->GetGraph() ? FromNode->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("graph_schema_not_k2"));
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
				if (Response.Response == CONNECT_RESPONSE_DISALLOW)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("pin_connection_disallowed"));
					OutStructured->SetStringField(TEXT("schema_message"), Response.Message.ToString());
					OutStructured->SetObjectField(TEXT("from_pin"), MakePinRefJson(FromPin));
					OutStructured->SetObjectField(TEXT("to_pin"), MakePinRefJson(ToPin));
					OutError = FString::Printf(TEXT("Blueprint pin connection disallowed: %s"), *Response.Message.ToString());
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPConnectPinsP0", "SOMOLMCP Connect Blueprint Pins"));
				Blueprint->Modify();
				FromNode->Modify();
				ToNode->Modify();
				if (!Schema->TryCreateConnection(FromPin, ToPin))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("try_create_connection_failed"));
					OutStructured->SetStringField(TEXT("schema_message"), Response.Message.ToString());
					OutStructured->SetObjectField(TEXT("from_pin"), MakePinRefJson(FromPin));
					OutStructured->SetObjectField(TEXT("to_pin"), MakePinRefJson(ToPin));
					OutError = TEXT("Failed to connect blueprint pins.");
					return false;
				}
				if (!FromPin->LinkedTo.Contains(ToPin) || !ToPin->LinkedTo.Contains(FromPin))
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("connection_readback_failed"));
					OutStructured->SetObjectField(TEXT("from_pin"), MakePinRefJson(FromPin));
					OutStructured->SetObjectField(TEXT("to_pin"), MakePinRefJson(ToPin));
					OutError = TEXT("Blueprint pin connection readback failed.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				FromNode->GetGraph()->NotifyGraphChanged();
				OutStructured->SetObjectField(TEXT("from_pin"), MakePinRefJson(FromPin));
				OutStructured->SetObjectField(TEXT("to_pin"), MakePinRefJson(ToPin));
				OutStructured->SetObjectField(TEXT("from_node"), BlueprintNodeToJson(FromNode));
				OutStructured->SetObjectField(TEXT("to_node"), BlueprintNodeToJson(ToNode));
				OutStructured->SetStringField(TEXT("connection_shape"), IsExecPin(FromPin) ? TEXT("exec") : TEXT("data"));
				OutStructured->SetBoolField(TEXT("connection_verified"), true);
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					FromNode->GetGraph(),
					TEXT("connect_pins"),
					TEXT("connection_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, FromNode->GetGraph(), OutStructured, OutError))
				{
					return false;
				}
				OutSummary = TEXT("Connected blueprint pins with readback verification.");
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_break_pin_links"),
			TEXT("Break all links on one Blueprint pin and return link-removal readback."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("pin_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("pin_guid"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuid;
				FString PinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing pin unlink arguments.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuidLocal(Blueprint, NodeGuid);
				if (!Node)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("node_not_found"));
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				FString PinGuid;
				Arguments->TryGetStringField(TEXT("pin_guid"), PinGuid);
				UEdGraphPin* Pin = FindNodePinLocal(Node, PinName, PinGuid);
				if (!Pin)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("pin_not_found"));
					OutStructured->SetObjectField(TEXT("pin_diagnostic"), MakePinFailureDiagnostic(Node, PinName, PinGuid, TOptional<EEdGraphPinDirection>()));
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				if (!EnsurePinSafeForMutation(Node, Pin, TEXT("pin"), OutStructured, OutError))
				{
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("graph_schema_not_k2"));
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				UEdGraph* Graph = Node->GetGraph();
				const int32 LinksBefore = Pin->LinkedTo.Num();
				TSharedRef<FJsonObject> PinBefore = MakePinRefJson(Pin);
				const FScopedTransaction Transaction(LOCTEXT("BPBreakPinLinksP0", "SOMOLMCP Break Blueprint Pin Links"));
				Blueprint->Modify();
				Node->Modify();
				Schema->BreakPinLinks(*Pin, true);
				Pin = FindNodePinLocal(Node, PinName, PinGuid);
				const bool bPinResolvedAfter = IsPinOwnedByNode(Pin, Node);
				const int32 LinksAfter = bPinResolvedAfter ? Pin->LinkedTo.Num() : 0;
				if (bPinResolvedAfter && LinksAfter > 0)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("break_links_readback_failed"));
					OutStructured->SetObjectField(TEXT("pin"), MakePinRefJson(Pin));
					OutError = TEXT("Blueprint pin break readback failed.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (Graph)
				{
					Graph->NotifyGraphChanged();
				}
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("operation"), TEXT("break_pin_links"));
				TSharedRef<FJsonObject> NodeRef = MakeShared<FJsonObject>();
				NodeRef->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
				NodeRef->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				NodeRef->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
				OutStructured->SetObjectField(TEXT("node"), NodeRef);
				OutStructured->SetObjectField(TEXT("pin_before"), PinBefore);
				if (bPinResolvedAfter)
				{
					OutStructured->SetObjectField(TEXT("pin_after"), MakePinRefJson(Pin));
				}
				OutStructured->SetNumberField(TEXT("links_before"), LinksBefore);
				OutStructured->SetNumberField(TEXT("links_after"), LinksAfter);
				OutStructured->SetBoolField(TEXT("pin_resolved_after_mutation"), bPinResolvedAfter);
				OutStructured->SetStringField(TEXT("readback_mode"), bPinResolvedAfter ? TEXT("resolved_pin_after_mutation") : TEXT("pin_rebuilt_or_removed_after_mutation"));
				OutStructured->SetBoolField(TEXT("links_cleared_verified"), true);
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("break_pin_links"),
					TEXT("links_cleared_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = TEXT("Broke blueprint pin links with readback verification.");
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_break_single_pin_link"),
			TEXT("Break one specific Blueprint pin link and verify both sides no longer reference each other."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("source_node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("source_pin_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("target_node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("target_pin_name"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("source_node_guid"), TEXT("source_pin_name"), TEXT("target_node_guid"), TEXT("target_pin_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString SourceNodeGuid;
				FString SourcePinName;
				FString TargetNodeGuid;
				FString TargetPinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("source_node_guid"), SourceNodeGuid) ||
					!Arguments->TryGetStringField(TEXT("source_pin_name"), SourcePinName) ||
					!Arguments->TryGetStringField(TEXT("target_node_guid"), TargetNodeGuid) ||
					!Arguments->TryGetStringField(TEXT("target_pin_name"), TargetPinName))
				{
					OutError = TEXT("Missing single pin unlink arguments.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}
				UEdGraphNode* SourceNode = FindBlueprintNodeByGuidLocal(Blueprint, SourceNodeGuid);
				UEdGraphNode* TargetNode = FindBlueprintNodeByGuidLocal(Blueprint, TargetNodeGuid);
				if (!SourceNode || !TargetNode)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("node_not_found"));
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				UEdGraphPin* SourcePin = FindNodePinLocal(SourceNode, SourcePinName);
				UEdGraphPin* TargetPin = FindNodePinLocal(TargetNode, TargetPinName);
				if (!SourcePin || !TargetPin)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("pin_not_found"));
					if (!SourcePin)
					{
						OutStructured->SetObjectField(TEXT("source_pin_diagnostic"), MakePinFailureDiagnostic(SourceNode, SourcePinName, FString(), TOptional<EEdGraphPinDirection>()));
					}
					if (!TargetPin)
					{
						OutStructured->SetObjectField(TEXT("target_pin_diagnostic"), MakePinFailureDiagnostic(TargetNode, TargetPinName, FString(), TOptional<EEdGraphPinDirection>()));
					}
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				if (!EnsurePinSafeForMutation(SourceNode, SourcePin, TEXT("source_pin"), OutStructured, OutError) ||
					!EnsurePinSafeForMutation(TargetNode, TargetPin, TEXT("target_pin"), OutStructured, OutError))
				{
					return false;
				}
				const bool bLinkPresentBefore = SourcePin->LinkedTo.Contains(TargetPin) || TargetPin->LinkedTo.Contains(SourcePin);
				TSharedRef<FJsonObject> SourcePinBefore = MakePinRefJson(SourcePin);
				TSharedRef<FJsonObject> TargetPinBefore = MakePinRefJson(TargetPin);
				if (!bLinkPresentBefore)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("link_not_present"));
					OutStructured->SetObjectField(TEXT("source_pin"), SourcePinBefore);
					OutStructured->SetObjectField(TEXT("target_pin"), TargetPinBefore);
					OutError = TEXT("Requested Blueprint pin link was not present.");
					return false;
				}
				UEdGraph* Graph = SourceNode->GetGraph();
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph ? Graph->GetSchema() : nullptr);
				if (!Schema)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("graph_schema_not_k2"));
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPBreakSinglePinLinkP0", "SOMOLMCP Break Blueprint Single Pin Link"));
				Blueprint->Modify();
				SourceNode->Modify();
				TargetNode->Modify();
				Schema->BreakSinglePinLink(SourcePin, TargetPin);
				SourcePin = FindNodePinLocal(SourceNode, SourcePinName);
				TargetPin = FindNodePinLocal(TargetNode, TargetPinName);
				const bool bSourceResolvedAfter = IsPinOwnedByNode(SourcePin, SourceNode);
				const bool bTargetResolvedAfter = IsPinOwnedByNode(TargetPin, TargetNode);
				const bool bStillLinkedAfter = bSourceResolvedAfter && bTargetResolvedAfter &&
					(SourcePin->LinkedTo.Contains(TargetPin) || TargetPin->LinkedTo.Contains(SourcePin));
				if (bStillLinkedAfter)
				{
					OutStructured->SetStringField(TEXT("diagnostic_code"), TEXT("break_single_link_readback_failed"));
					OutStructured->SetObjectField(TEXT("source_pin"), MakePinRefJson(SourcePin));
					OutStructured->SetObjectField(TEXT("target_pin"), MakePinRefJson(TargetPin));
					OutError = TEXT("Blueprint single pin break readback failed.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (Graph)
				{
					Graph->NotifyGraphChanged();
				}
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("operation"), TEXT("break_single_pin_link"));
				TSharedRef<FJsonObject> SourceNodeRef = MakeShared<FJsonObject>();
				SourceNodeRef->SetStringField(TEXT("guid"), SourceNode->NodeGuid.ToString());
				SourceNodeRef->SetStringField(TEXT("title"), SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				TSharedRef<FJsonObject> TargetNodeRef = MakeShared<FJsonObject>();
				TargetNodeRef->SetStringField(TEXT("guid"), TargetNode->NodeGuid.ToString());
				TargetNodeRef->SetStringField(TEXT("title"), TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				OutStructured->SetObjectField(TEXT("source_node"), SourceNodeRef);
				OutStructured->SetObjectField(TEXT("target_node"), TargetNodeRef);
				OutStructured->SetObjectField(TEXT("source_pin_before"), SourcePinBefore);
				OutStructured->SetObjectField(TEXT("target_pin_before"), TargetPinBefore);
				if (bSourceResolvedAfter)
				{
					OutStructured->SetObjectField(TEXT("source_pin_after"), MakePinRefJson(SourcePin));
				}
				if (bTargetResolvedAfter)
				{
					OutStructured->SetObjectField(TEXT("target_pin_after"), MakePinRefJson(TargetPin));
				}
				OutStructured->SetBoolField(TEXT("source_pin_resolved_after_mutation"), bSourceResolvedAfter);
				OutStructured->SetBoolField(TEXT("target_pin_resolved_after_mutation"), bTargetResolvedAfter);
				OutStructured->SetStringField(TEXT("readback_mode"), (bSourceResolvedAfter && bTargetResolvedAfter) ? TEXT("resolved_pins_after_mutation") : TEXT("pin_rebuilt_or_removed_after_mutation"));
				OutStructured->SetBoolField(TEXT("link_removed_verified"), true);
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("break_single_pin_link"),
					TEXT("link_removed_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = TEXT("Broke one blueprint pin link with readback verification.");
				return true;
			}
		});

		// ----------------------------------------------------------------
		// H4. blueprint_export_nodes_t3d / blueprint_import_nodes_t3d
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_export_nodes_t3d"),
			TEXT("Export Blueprint graph nodes as UE copy/T3D text. Supports all_nodes or node_guids/node_names filters."),
			MakeBlueprintT3dGraphSchema(/*bForImport*/ false),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				TSet<UObject*> NodesToExport;
				TArray<TSharedPtr<FJsonValue>> NodeReceipts;
				CollectBlueprintNodesForT3dExport(Graph, Arguments, NodesToExport, NodeReceipts);
				if (NodesToExport.Num() <= 0)
				{
					OutError = TEXT("No Blueprint nodes matched the export request.");
					return false;
				}

				FString ExportedText;
				FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);
				if (ExportedText.IsEmpty())
				{
					OutError = TEXT("FEdGraphUtilities exported an empty node T3D payload.");
					return false;
				}

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("graph_name"), Graph->GetName());
				OutStructured->SetNumberField(TEXT("node_count"), NodesToExport.Num());
				OutStructured->SetStringField(TEXT("t3d"), ExportedText);
				OutStructured->SetNumberField(TEXT("t3d_length"), ExportedText.Len());
				OutStructured->SetArrayField(TEXT("nodes"), NodeReceipts);
				OutSummary = FString::Printf(
					TEXT("Exported %d Blueprint node(s) from %s.%s as T3D text."),
					NodesToExport.Num(),
					*AssetPath,
					*Graph->GetName());
				return true;
			}
		});

		Registry.Register({
			TEXT("blueprint_import_nodes_t3d"),
			TEXT("Import UE copy/T3D Blueprint node text into a Blueprint graph. Optional pos_x/pos_y recenters the imported nodes."),
			MakeBlueprintT3dGraphSchema(/*bForImport*/ true),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString T3dText;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("t3d"), T3dText))
				{
					OutError = TEXT("Missing asset_path, graph_name, or t3d.");
					return false;
				}
				if (T3dText.IsEmpty())
				{
					OutError = TEXT("t3d must not be empty.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				if (!FEdGraphUtilities::CanImportNodesFromText(Graph, T3dText))
				{
					OutError = TEXT("FEdGraphUtilities rejected the supplied Blueprint node T3D text for this graph.");
					return false;
				}

				TSet<UEdGraphNode*> ImportedNodes;
				const FScopedTransaction Transaction(LOCTEXT("BPImportNodesT3D", "SOMOLMCP Import Blueprint Nodes T3D"));
				Blueprint->Modify();
				Graph->Modify();
				FEdGraphUtilities::ImportNodesFromText(Graph, T3dText, ImportedNodes);
				if (ImportedNodes.Num() <= 0)
				{
					OutError = TEXT("No Blueprint nodes were imported from the supplied T3D text.");
					return false;
				}

				const bool bRecentered = HasExplicitImportPosition(Arguments);
				if (bRecentered)
				{
					MoveImportedNodesToCenter(ImportedNodes, GetBlueprintNodeLocationFromArguments(Arguments));
				}

				bool bNeedsStructuralModify = false;
				TArray<TSharedPtr<FJsonValue>> ImportedNodeReceipts;
				for (UEdGraphNode* Node : ImportedNodes)
				{
					if (!Node)
					{
						continue;
					}
					Node->CreateNewGuid();
					ImportedNodeReceipts.Add(MakeShared<FJsonValueObject>(BlueprintNodeToJson(Node)));
					if (const UK2Node* K2Node = Cast<UK2Node>(Node))
					{
						bNeedsStructuralModify = bNeedsStructuralModify || K2Node->NodeCausesStructuralBlueprintChange();
					}
				}

				if (bNeedsStructuralModify)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				}
				else
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				}
				Graph->NotifyGraphChanged();

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("graph_name"), Graph->GetName());
				OutStructured->SetNumberField(TEXT("imported_node_count"), ImportedNodes.Num());
				OutStructured->SetArrayField(TEXT("imported_nodes"), ImportedNodeReceipts);
				OutStructured->SetBoolField(TEXT("recentered"), bRecentered);
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("import_nodes_t3d"),
					TEXT("imported_node_count"),
					ImportedNodes.Num() > 0);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(
					TEXT("Imported %d Blueprint node(s) into %s.%s from T3D text."),
					ImportedNodes.Num(),
					*AssetPath,
					*Graph->GetName());
				return true;
			}
		});

		// ----------------------------------------------------------------
		// H3-1. blueprint_read_graph_summary
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_read_graph_summary"),
			TEXT("Read a low-token summary of one or all Blueprint graphs: compact nodes, exec pins, and exec links."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Optional graph name. If omitted, all graphs are summarized."))}
				},
				{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = OutError.IsEmpty() ? TEXT("Asset is not a Blueprint.") : OutError;
					return false;
				}

				TArray<UEdGraph*> GraphsToRead;
				FString GraphName;
				if (Arguments->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
				{
					UEdGraph* Graph = FindBlueprintGraphByNameLocal(Blueprint, GraphName);
					if (!Graph)
					{
						OutError = FString::Printf(TEXT("Graph '%s' not found."), *GraphName);
						return false;
					}
					GraphsToRead.Add(Graph);
				}
				else
				{
					Blueprint->GetAllGraphs(GraphsToRead);
				}

				TArray<TSharedPtr<FJsonValue>> Graphs;
				int32 TotalNodes = 0;
				int32 TotalExecLinks = 0;
				for (UEdGraph* Graph : GraphsToRead)
				{
					if (!Graph)
					{
						continue;
					}

					TSharedRef<FJsonObject> GraphJson = BlueprintGraphToCompactSummary(Graph);
					int32 NodeCount = 0;
					int32 ExecLinkCount = 0;
					GraphJson->TryGetNumberField(TEXT("node_count"), NodeCount);
					GraphJson->TryGetNumberField(TEXT("exec_link_count"), ExecLinkCount);
					TotalNodes += NodeCount;
					TotalExecLinks += ExecLinkCount;
					Graphs.Add(MakeShared<FJsonValueObject>(GraphJson));
				}

				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetArrayField(TEXT("graphs"), Graphs);
				OutStructured->SetNumberField(TEXT("graph_count"), Graphs.Num());
				OutStructured->SetNumberField(TEXT("total_nodes"), TotalNodes);
				OutStructured->SetNumberField(TEXT("total_exec_links"), TotalExecLinks);
				OutSummary = FString::Printf(TEXT("Read Blueprint graph summary: %d graph(s), %d node(s), %d exec link(s)."),
					Graphs.Num(), TotalNodes, TotalExecLinks);
				return true;
			},
			nullptr,
			5
		});

		// ----------------------------------------------------------------
		// H3-2. blueprint_get_execution_flow
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_get_execution_flow"),
			TEXT("Trace compact Blueprint execution paths along exec pins from entry nodes or a selected start node."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("start_node_id"), FSololmcpSchemaBuilder::String(TEXT("Optional node guid/id from blueprint_read_graph_summary."))},
					{TEXT("start_node_title"), FSololmcpSchemaBuilder::String(TEXT("Optional node title fallback when start_node_id is omitted."))},
					{TEXT("max_depth"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("max_paths"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				int32 MaxDepth = 64;
				int32 MaxPaths = 64;
				Arguments->TryGetNumberField(TEXT("max_depth"), MaxDepth);
				Arguments->TryGetNumberField(TEXT("max_paths"), MaxPaths);
				MaxDepth = FMath::Clamp(MaxDepth, 1, 256);
				MaxPaths = FMath::Clamp(MaxPaths, 1, 512);

				TArray<UEdGraphNode*> StartNodes;
				FString StartNodeId;
				FString StartNodeTitle;
				Arguments->TryGetStringField(TEXT("start_node_id"), StartNodeId);
				Arguments->TryGetStringField(TEXT("start_node_title"), StartNodeTitle);
				if (!StartNodeId.IsEmpty() || !StartNodeTitle.IsEmpty())
				{
					UEdGraphNode* StartNode = FindNodeByStableIdOrTitle(Graph, StartNodeId, StartNodeTitle);
					if (!StartNode)
					{
						OutError = TEXT("Requested start node was not found.");
						return false;
					}
					StartNodes.Add(StartNode);
				}
				else
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (IsExecutionEntryNode(Node))
						{
							StartNodes.Add(Node);
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> StartNodeIds;
				TArray<TSharedPtr<FJsonValue>> Paths;
				int32 TruncatedAtDepth = 0;
				int32 TruncatedAtPathLimit = 0;
				for (UEdGraphNode* StartNode : StartNodes)
				{
					if (!StartNode || Paths.Num() >= MaxPaths)
					{
						continue;
					}
					StartNodeIds.Add(MakeShared<FJsonValueString>(GetNodeStableId(StartNode)));
					TraceExecutionFlowFromNode(
						StartNode,
						MaxDepth,
						MaxPaths - Paths.Num(),
						Paths,
						TruncatedAtDepth,
						TruncatedAtPathLimit);
				}

				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("graph_name"), Graph->GetName());
				OutStructured->SetArrayField(TEXT("start_nodes"), StartNodeIds);
				OutStructured->SetArrayField(TEXT("paths"), Paths);
				OutStructured->SetNumberField(TEXT("path_count"), Paths.Num());
				OutStructured->SetNumberField(TEXT("max_depth"), MaxDepth);
				OutStructured->SetNumberField(TEXT("max_paths"), MaxPaths);
				OutStructured->SetNumberField(TEXT("truncated_at_depth_count"), TruncatedAtDepth);
				OutStructured->SetNumberField(TEXT("truncated_at_path_limit_count"), TruncatedAtPathLimit);
				OutSummary = FString::Printf(TEXT("Traced Blueprint execution flow for '%s': %d start node(s), %d path(s)."),
					*Graph->GetName(), StartNodes.Num(), Paths.Num());
				return true;
			},
			nullptr,
			5
		});

		// ----------------------------------------------------------------
		// H3-3. blueprint_graph_explain
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_graph_explain"),
			TEXT("Explain a Blueprint graph with compact node summary, entry nodes, execution paths, and truncation notes."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("max_depth"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("max_paths"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				int32 MaxDepth = 64;
				int32 MaxPaths = 64;
				Arguments->TryGetNumberField(TEXT("max_depth"), MaxDepth);
				Arguments->TryGetNumberField(TEXT("max_paths"), MaxPaths);
				MaxDepth = FMath::Clamp(MaxDepth, 1, 256);
				MaxPaths = FMath::Clamp(MaxPaths, 1, 512);

				OutStructured = ExplainBlueprintGraphMinimal(Blueprint, Graph, MaxDepth, MaxPaths);
				int32 PathCount = 0;
				OutStructured->TryGetNumberField(TEXT("path_count"), PathCount);
				OutSummary = FString::Printf(
					TEXT("Explained Blueprint graph '%s': %d execution path(s)."),
					*Graph->GetName(),
					PathCount);
				return true;
			},
			nullptr,
			5
		});

		// ----------------------------------------------------------------
		// 1. blueprint_add_for_loop_node
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_for_loop_node"),
			TEXT("Add a ForLoop macro node (StandardMacros.ForLoop) to a blueprint graph."),
			MakeBasicGraphPosSchema(),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return SpawnStandardMacroLoop(
					Context, Arguments,
					TEXT("ForLoop"),
					LOCTEXT("BPAddForLoop", "SOMOLMCP Add Blueprint ForLoop Node"),
					TEXT("Added blueprint ForLoop node."),
					OutStructured, OutSummary, OutError);
			}
		});

		// ----------------------------------------------------------------
		// 2. blueprint_add_for_each_loop_node
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_for_each_loop_node"),
			TEXT("Add a ForEachLoop macro node (StandardMacros.ForEachLoop) to a blueprint graph."),
			MakeBasicGraphPosSchema(),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return SpawnStandardMacroLoop(
					Context, Arguments,
					TEXT("ForEachLoop"),
					LOCTEXT("BPAddForEachLoop", "SOMOLMCP Add Blueprint ForEachLoop Node"),
					TEXT("Added blueprint ForEachLoop node."),
					OutStructured, OutSummary, OutError);
			}
		});

		// ----------------------------------------------------------------
		// 3. blueprint_add_while_loop_node
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_while_loop_node"),
			TEXT("Add a WhileLoop macro node (StandardMacros.WhileLoop) to a blueprint graph."),
			MakeBasicGraphPosSchema(),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return SpawnStandardMacroLoop(
					Context, Arguments,
					TEXT("WhileLoop"),
					LOCTEXT("BPAddWhileLoop", "SOMOLMCP Add Blueprint WhileLoop Node"),
					TEXT("Added blueprint WhileLoop node."),
					OutStructured, OutSummary, OutError);
			}
		});

		// ----------------------------------------------------------------
		// 4. blueprint_add_switch_int_node
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_switch_int_node"),
			TEXT("Add a Switch on Int node (UK2Node_SwitchInteger) to a blueprint graph."),
			MakeBasicGraphPosSchema(),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPAddSwitchInt", "SOMOLMCP Add Blueprint Switch Int Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(
					Graph,
					UK2Node_SwitchInteger::StaticClass(),
					GetBlueprintNodeLocationFromArguments(Arguments));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn switch int node.");
					return false;
				}
				if (!VerifyGraphContainsNode(Graph, Node))
				{
					OutError = TEXT("Switch int node was not present in the graph after creation.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("add_switch_int_node"),
					TEXT("node_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = TEXT("Added blueprint switch int node.");
				return true;
			}
		});

		// ----------------------------------------------------------------
		// 5. blueprint_add_switch_enum_node
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_switch_enum_node"),
			TEXT("Add a Switch on Enum node (UK2Node_SwitchEnum) bound to a UEnum at the given path."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("enum_path"), FSololmcpSchemaBuilder::String(TEXT("e.g. /Game/Enums/E_State.E_State"))},
					{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("enum_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString EnumPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("enum_path"), EnumPath))
				{
					OutError = TEXT("Missing asset_path, graph_name, or enum_path.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				UEnum* TargetEnum = Cast<UEnum>(StaticLoadObject(UEnum::StaticClass(), nullptr, *EnumPath));
				if (!TargetEnum)
				{
					OutError = FString::Printf(TEXT("Failed to load UEnum at '%s'."), *EnumPath);
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPAddSwitchEnum", "SOMOLMCP Add Blueprint Switch Enum Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(
					Graph,
					UK2Node_SwitchEnum::StaticClass(),
					GetBlueprintNodeLocationFromArguments(Arguments),
					UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda([TargetEnum](UEdGraphNode* NewNode, bool /*bIsTemplateNode*/)
					{
						if (UK2Node_SwitchEnum* SwitchNode = Cast<UK2Node_SwitchEnum>(NewNode))
						{
						#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 5
							// UE 5.5 declares SetEnum on a MinimalAPI class without exporting it.
							// Mirror the engine implementation so packaged plugins can still author
							// fully initialized enum switch nodes without an unresolved import.
							SwitchNode->Enum = TargetEnum;
							SwitchNode->EnumEntries.Reset();
							SwitchNode->EnumFriendlyNames.Reset();
							if (TargetEnum)
							{
								TargetEnum->ConditionalPostLoad();
								for (int32 EnumIndex = 0; EnumIndex < TargetEnum->NumEnums() - 1; ++EnumIndex)
								{
									const bool bHidden = TargetEnum->HasMetaData(TEXT("Hidden"), EnumIndex)
										|| TargetEnum->HasMetaData(TEXT("Spacer"), EnumIndex);
									if (!bHidden)
									{
										SwitchNode->EnumEntries.Add(FName(*TargetEnum->GetNameStringByIndex(EnumIndex)));
										SwitchNode->EnumFriendlyNames.Add(TargetEnum->GetDisplayNameTextByIndex(EnumIndex));
									}
								}
							}
						#else
							SwitchNode->SetEnum(TargetEnum);
						#endif
							SwitchNode->ReconstructNode();
						}
					}));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn switch enum node.");
					return false;
				}
				if (!VerifyGraphContainsNode(Graph, Node) || !Cast<UK2Node_SwitchEnum>(Node))
				{
					OutError = FString::Printf(TEXT("Switch enum node failed post-write verification for enum '%s'."), *EnumPath);
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetStringField(TEXT("enum_path"), TargetEnum->GetPathName());
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("add_switch_enum_node"),
					TEXT("node_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(TEXT("Added blueprint switch enum node bound to %s."), *TargetEnum->GetName());
				return true;
			}
		});

		// ----------------------------------------------------------------
		// 6. blueprint_add_select_node
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_select_node"),
			TEXT("Add a Select node (UK2Node_Select) with the given index pin type (int|bool|string|float)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("pin_type"), FSololmcpSchemaBuilder::String(TEXT("int|bool|string|float"),
						{TEXT("int"), TEXT("bool"), TEXT("string"), TEXT("float")})},
					{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("pin_type")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString PinTypeStr;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("pin_type"), PinTypeStr))
				{
					OutError = TEXT("Missing asset_path, graph_name, or pin_type.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPAddSelect", "SOMOLMCP Add Blueprint Select Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(
					Graph,
					UK2Node_Select::StaticClass(),
					GetBlueprintNodeLocationFromArguments(Arguments));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn select node.");
					return false;
				}

				FString ConfigError;
				if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Node))
				{
					if (!ConfigureSelectNodeIndexType(SelectNode, PinTypeStr, ConfigError))
					{
						OutError = ConfigError;
						return false;
					}
				}

				if (!Cast<UK2Node_Select>(Node))
				{
					OutError = TEXT("Spawned node was not a UK2Node_Select.");
					return false;
				}
				if (!VerifyGraphContainsNode(Graph, Node))
				{
					OutError = TEXT("Select node was not present in the graph after creation.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetStringField(TEXT("requested_pin_type"), PinTypeStr);
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("add_select_node"),
					TEXT("node_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = TEXT("Added blueprint select node.");
				return true;
			}
		});

		// ----------------------------------------------------------------
		// 7. blueprint_add_event_node
		// ----------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_event_node"),
			TEXT("Add a parent-class event node (UK2Node_Event) like ReceiveBeginPlay, ReceiveTick, "
				 "ReceiveActorBeginOverlap, ReceiveHit, or any BlueprintImplementableEvent on the parent class."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Defaults to 'EventGraph' if omitted."))},
					{TEXT("event_name"), FSololmcpSchemaBuilder::String(TEXT("UFunction name on parent class, e.g. ReceiveBeginPlay."))},
					{TEXT("pos_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("pos_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("event_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString EventName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("event_name"), EventName) ||
					EventName.IsEmpty())
				{
					OutError = TEXT("Missing asset_path or event_name.");
					return false;
				}

				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
				{
					GraphName = TEXT("EventGraph");
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				UClass* ParentClass = Blueprint->ParentClass;
				if (!ParentClass)
				{
					OutError = TEXT("Blueprint has no parent class.");
					return false;
				}

				const FName EventFName(*EventName);
				UFunction* TargetFunction = FindOverridableEventFunction(ParentClass, EventFName);
				if (!TargetFunction)
				{
					OutError = FString::Printf(
						TEXT("UFunction '%s' not found on parent class '%s' or its supers."),
						*EventName,
						*ParentClass->GetName());
					return false;
				}

				// Resolve the class that actually owns the function — this is what
				// EventReference.MemberParent should point at.
				UClass* SignatureOwner = TargetFunction->GetOwnerClass();
				if (!SignatureOwner)
				{
					SignatureOwner = ParentClass;
				}

				if (IsAlreadyPlacedEvent(Graph, SignatureOwner, EventFName))
				{
					OutError = FString::Printf(TEXT("Event '%s' is already placed in graph '%s'."), *EventName, *GraphName);
					return false;
				}

				const FVector2f Location = GetBlueprintNodeLocationFromArguments(Arguments);

				const FScopedTransaction Transaction(LOCTEXT("BPAddEventNode", "SOMOLMCP Add Blueprint Event Node"));
				Blueprint->Modify();
				Graph->Modify();

				// TODO(P0-3): UBlueprintEventNodeSpawner is the conventional path; if the
				// SetExternalMember/SetFromField API is unavailable in this UE build we
				// fall back to direct UK2Node_Event construction below.
				UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
				if (!EventNode)
				{
					OutError = TEXT("Failed to construct UK2Node_Event.");
					return false;
				}

				EventNode->CreateNewGuid();
				EventNode->NodePosX = static_cast<int32>(Location.X);
				EventNode->NodePosY = static_cast<int32>(Location.Y);
				EventNode->bOverrideFunction = true;
				EventNode->EventReference.SetExternalMember(EventFName, SignatureOwner);

				Graph->AddNode(EventNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
				EventNode->PostPlacedNewNode();
				EventNode->AllocateDefaultPins();
				EventNode->ReconstructNode();
				if (!VerifyGraphContainsNode(Graph, EventNode) ||
					EventNode->EventReference.GetMemberName() != EventFName)
				{
					OutError = FString::Printf(TEXT("Event node '%s' failed post-write verification."), *EventName);
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				OutStructured = BlueprintNodeToJson(EventNode);
				OutStructured->SetStringField(TEXT("event_name"), EventName);
				OutStructured->SetStringField(TEXT("signature_owner"), SignatureOwner->GetPathName());
				AttachBlueprintGraphEditReceipt(
					OutStructured,
					Blueprint,
					Graph,
					TEXT("add_event_node"),
					TEXT("node_verified"),
					true);
				if (!RunBlueprintGraphMutationGate(Blueprint, Graph, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(TEXT("Added blueprint event node '%s'."), *EventName);
				return true;
			}
		});
	}
} // namespace UE::SOMOLMCP

#undef LOCTEXT_NAMESPACE
