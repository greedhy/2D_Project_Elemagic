// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// ControlRig coverage — RigVM graph reflection and batch editing.
//
// SOMOLMCP could already add unit nodes, link pins and set pin defaults on a rig
// graph, but it could not *read* one: there was no way to see a node's pins, a
// pin's type or default, or what a pin is connected to. That makes the existing
// write tools usable only by a caller who already knows the graph — which an agent
// planning a queued edit does not.
//
// Two things close that gap:
//
//   Reflection. URigVMPin::GetPinPath() returns exactly the path string that
//   URigVMController::SetPinDefaultValue and AddLink take, so reading a graph
//   yields addresses that can be written back directly. Read, plan, batch-write
//   is a closed loop with no name guessing in between.
//
//   A catalog. rigvm_node_catalog enumerates the unit-node script structs this
//   editor actually has. The existing control_rig_graph_add_unit_node requires the
//   caller to already know a struct path and fails at execution time if it is
//   wrong — which, in a queued workload, means burning one of a handful of
//   concurrent game-thread slots to discover a typo. The catalog moves that check
//   to planning time.
//
// Batch variants exist for the same reason as elsewhere: N single edits cost N
// game-thread entries and leave N undo steps, while one batch costs one of each.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "Services/SololmcpEditorServices.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "Runtime/Launch/Resources/Version.h"

// UE 5.7 renamed both blueprint headers; see SololmcpRigHierarchyTools.cpp.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "ControlRigBlueprintLegacy.h"
#include "RigVMBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#include "RigVMBlueprint.h"
#endif

#include "ControlRigBlueprintEditorLibrary.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMCore/RigVMStruct.h"

namespace UE::SOMOLMCP
{
namespace RigVMGraphToolsPrivate
{
	/** Resolve the rig blueprint, its default graph and its controller together. */
	inline bool ResolveGraph(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		const TSharedRef<FJsonObject>& OutStructured,
		UControlRigBlueprint*& OutRig,
		URigVMGraph*& OutGraph,
		URigVMController*& OutController,
		FString& OutError)
	{
		OutRig = nullptr;
		OutGraph = nullptr;
		OutController = nullptr;

		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
			OutError = TEXT("Missing asset_path.");
			return false;
		}
		OutRig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
		if (OutRig == nullptr)
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a Control Rig Blueprint."), *AssetPath);
			}
			return false;
		}
		OutGraph = OutRig->GetDefaultModel();
		if (OutGraph == nullptr)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
				TEXT("The blueprint has no default RigVM model; it may need a recompile."));
			OutError = TEXT("RigVM model unavailable.");
			return false;
		}
		OutController = OutRig->GetController(OutGraph);
		if (OutController == nullptr)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
				TEXT("No RigVM controller for the default model."));
			OutError = TEXT("RigVM controller unavailable.");
			return false;
		}
		return true;
	}

	inline FString PinDirectionToString(const ERigVMPinDirection Direction)
	{
		switch (Direction)
		{
		case ERigVMPinDirection::Input:   return TEXT("input");
		case ERigVMPinDirection::Output:  return TEXT("output");
		case ERigVMPinDirection::IO:      return TEXT("io");
		case ERigVMPinDirection::Visible: return TEXT("visible");
		case ERigVMPinDirection::Hidden:  return TEXT("hidden");
		default:                          return TEXT("invalid");
		}
	}

	/** Describe a pin, recursing into sub-pins up to a depth so arrays and structs
	 *  do not blow up a response for a large graph. */
	inline TSharedRef<FJsonObject> DescribePin(const URigVMPin* Pin, const int32 Depth, const int32 MaxDepth)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Pin->GetName());
		Json->SetStringField(TEXT("pin_path"), Pin->GetPinPath());
		Json->SetStringField(TEXT("cpp_type"), Pin->GetCPPType());
		Json->SetStringField(TEXT("direction"), PinDirectionToString(Pin->GetDirection()));
		Json->SetStringField(TEXT("default_value"), Pin->GetDefaultValue());
		Json->SetBoolField(TEXT("is_array"), Pin->IsArray());
		Json->SetNumberField(TEXT("link_count"), Pin->GetLinks().Num());

		const TArray<URigVMPin*>& SubPins = Pin->GetSubPins();
		if (SubPins.Num() > 0)
		{
			Json->SetNumberField(TEXT("sub_pin_count"), SubPins.Num());
			if (Depth < MaxDepth)
			{
				TArray<TSharedPtr<FJsonValue>> SubJson;
				for (const URigVMPin* SubPin : SubPins)
				{
					if (SubPin != nullptr)
					{
						SubJson.Add(MakeShared<FJsonValueObject>(DescribePin(SubPin, Depth + 1, MaxDepth)));
					}
				}
				Json->SetArrayField(TEXT("sub_pins"), SubJson);
			}
			else
			{
				// Say why the recursion stopped, so an empty sub_pins is not read as
				// "this pin has no children".
				Json->SetBoolField(TEXT("sub_pins_truncated"), true);
			}
		}
		return Json;
	}

	inline TSharedRef<FJsonObject> DescribeNode(const URigVMNode* Node, const bool bIncludePins, const int32 MaxPinDepth)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Node->GetName());
		Json->SetStringField(TEXT("title"), Node->GetNodeTitle());
		Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Json->SetNumberField(TEXT("index"), Node->GetNodeIndex());

		const FVector2D Position = Node->GetPosition();
		TSharedRef<FJsonObject> PositionJson = MakeShared<FJsonObject>();
		PositionJson->SetNumberField(TEXT("x"), Position.X);
		PositionJson->SetNumberField(TEXT("y"), Position.Y);
		Json->SetObjectField(TEXT("position"), PositionJson);

		const TArray<URigVMPin*>& Pins = Node->GetPins();
		Json->SetNumberField(TEXT("pin_count"), Pins.Num());
		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> PinsJson;
			for (const URigVMPin* Pin : Pins)
			{
				if (Pin != nullptr)
				{
					PinsJson.Add(MakeShared<FJsonValueObject>(DescribePin(Pin, 0, MaxPinDepth)));
				}
			}
			Json->SetArrayField(TEXT("pins"), PinsJson);
		}
		return Json;
	}

	inline TSharedRef<FJsonObject> AssetArgSchema()
	{
		return FSololmcpSchemaBuilder::String(TEXT("Object path of the Control Rig Blueprint."));
	}
} // namespace RigVMGraphToolsPrivate

void RegisterRigVMGraphTools(FSololmcpToolRegistry& Registry)
{
	using namespace RigVMGraphToolsPrivate;

	// ── rigvm_node_inspect ─────────────────────────────────────────────────
	Registry.Register({
		TEXT("rigvm_node_inspect"),
		TEXT("Inspect a RigVM node: title, class, position and every pin with its type, direction, "
			 "default value and link count. The returned pin_path values are the exact addresses "
			 "rigvm_pin_default_set_batch and rigvm_link_add_batch take."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("node_name"), FSololmcpSchemaBuilder::String(
					TEXT("Node name. Omit to describe every node in the graph."))},
				{TEXT("include_pins"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Include full pin detail.")), true)},
				{TEXT("max_pin_depth"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("How deep to recurse into struct and array sub-pins.")), 2)}
			},
			{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UControlRigBlueprint* Rig = nullptr;
			URigVMGraph* Graph = nullptr;
			URigVMController* Controller = nullptr;
			if (!ResolveGraph(Context, Args, OutStructured, Rig, Graph, Controller, OutError))
			{
				return false;
			}

			bool bIncludePins = true;
			Args->TryGetBoolField(TEXT("include_pins"), bIncludePins);
			int32 MaxPinDepth = 2;
			Args->TryGetNumberField(TEXT("max_pin_depth"), MaxPinDepth);
			MaxPinDepth = FMath::Clamp(MaxPinDepth, 0, 8);

			FString NodeName;
			const bool bSingle = Args->TryGetStringField(TEXT("node_name"), NodeName) && !NodeName.IsEmpty();

			TArray<TSharedPtr<FJsonValue>> Rows;
			for (URigVMNode* Node : Graph->GetNodes())
			{
				if (Node == nullptr)
				{
					continue;
				}
				if (bSingle && Node->GetName() != NodeName)
				{
					continue;
				}
				Rows.Add(MakeShared<FJsonValueObject>(DescribeNode(Node, bIncludePins, MaxPinDepth)));
			}

			if (bSingle && Rows.Num() == 0)
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("node_name"),
					TEXT("Call this tool without node_name to list every node in the graph."));
				OutError = FString::Printf(TEXT("No node named '%s'."), *NodeName);
				return false;
			}

			OutStructured->SetArrayField(TEXT("nodes"), Rows);
			OutStructured->SetNumberField(TEXT("node_count"), Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = bSingle
				? FString::Printf(TEXT("Inspected node '%s'."), *NodeName)
				: FString::Printf(TEXT("Inspected %d node(s)."), Rows.Num());
			return true;
		},
		nullptr,
		5
	});

	// ── rigvm_node_catalog ─────────────────────────────────────────────────
	Registry.Register({
		TEXT("rigvm_node_catalog"),
		TEXT("List the RigVM unit-node script structs this editor has loaded, with the exact struct "
			 "paths control_rig_graph_add_unit_node expects. Call this while planning a queued graph "
			 "build so a wrong struct path is caught before it occupies a job slot."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("filter"), FSololmcpSchemaBuilder::String(
					TEXT("Case-insensitive substring match against the struct name. Omit for all."))},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Maximum entries to return.")), 300)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
		{
			FString Filter;
			Args->TryGetStringField(TEXT("filter"), Filter);
			int32 Limit = 300;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			Limit = FMath::Clamp(Limit, 1, 5000);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Matched = 0;
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				UScriptStruct* Struct = *It;
				// Unit nodes are the structs deriving from FRigVMStruct; the base
				// itself is not spawnable.
				if (Struct == FRigVMStruct::StaticStruct() || !Struct->IsChildOf(FRigVMStruct::StaticStruct()))
				{
					continue;
				}
				const FString Name = Struct->GetName();
				if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				++Matched;
				if (Rows.Num() >= Limit)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("struct_name"), Name);
				Row->SetStringField(TEXT("struct_path"), Struct->GetPathName());
				const FString Tooltip = Struct->GetMetaData(TEXT("ToolTip"));
				if (!Tooltip.IsEmpty())
				{
					Row->SetStringField(TEXT("tooltip"), Tooltip.Left(300));
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			Rows.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				FString NameA;
				FString NameB;
				A->AsObject()->TryGetStringField(TEXT("struct_name"), NameA);
				B->AsObject()->TryGetStringField(TEXT("struct_name"), NameB);
				return NameA < NameB;
			});

			OutStructured->SetArrayField(TEXT("unit_nodes"), Rows);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("matched"), Matched);
			OutStructured->SetBoolField(TEXT("truncated"), Matched > Rows.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d of %d RigVM unit node struct(s)."), Rows.Num(), Matched);
			return true;
		},
		nullptr,
		120
	});

	// ── rigvm_pin_default_set_batch ────────────────────────────────────────
	Registry.Register({
		TEXT("rigvm_pin_default_set_batch"),
		TEXT("Set many pin default values in ONE game-thread entry, under a single undo step. "
			 "Use the pin_path values returned by rigvm_node_inspect. Per-item results are returned "
			 "so a partial failure is diagnosable without replaying the wave."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("items"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("pin_path"), FSololmcpSchemaBuilder::String(
								TEXT("Pin path, e.g. MyNode.Value."))},
							{TEXT("default_value"), FSololmcpSchemaBuilder::String(
								TEXT("Value in RigVM's string form, as returned by rigvm_node_inspect."))}
						},
						{TEXT("pin_path"), TEXT("default_value")}),
					TEXT("Pin defaults to apply, in order."))},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Abort the remaining items on first failure.")), false)}
			},
			{TEXT("asset_path"), TEXT("items")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UControlRigBlueprint* Rig = nullptr;
			URigVMGraph* Graph = nullptr;
			URigVMController* Controller = nullptr;
			if (!ResolveGraph(Context, Args, OutStructured, Rig, Graph, Controller, OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Args->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("items"));
				OutError = TEXT("Missing items array.");
				return false;
			}
			bool bStopOnError = false;
			Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

			const FScopedTransaction Transaction(
				NSLOCTEXT("SOMOLMCP", "RigVMSetPinDefaultBatch", "SOMOLMCP Set RigVM Pin Defaults"));

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Applied = 0;
			int32 Failed = 0;
			int32 Skipped = 0;

			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);

				if (bStopOnError && Failed > 0)
				{
					Row->SetStringField(TEXT("status"), TEXT("skipped"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Skipped;
					continue;
				}

				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item) || Item == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("item_not_object"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				FString PinPath;
				FString Value;
				(*Item)->TryGetStringField(TEXT("pin_path"), PinPath);
				(*Item)->TryGetStringField(TEXT("default_value"), Value);
				Row->SetStringField(TEXT("pin_path"), PinPath);

				if (PinPath.IsEmpty())
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("missing_pin_path"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}
				if (Graph->FindPin(PinPath) == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("pin_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				if (Controller->SetPinDefaultValue(PinPath, Value, /*bResizeArrays=*/true, /*bSetupUndoRedo=*/true))
				{
					Row->SetStringField(TEXT("status"), TEXT("ok"));
					// Read back: RigVM normalizes and may clamp values, so echoing the
					// request would hide a value that did not land as asked.
					if (const URigVMPin* Pin = Graph->FindPin(PinPath))
					{
						Row->SetStringField(TEXT("applied_value"), Pin->GetDefaultValue());
					}
					++Applied;
				}
				else
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("controller_rejected_value"));
					++Failed;
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Items->Num());
			OutStructured->SetNumberField(TEXT("applied"), Applied);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetNumberField(TEXT("skipped"), Skipped);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetNumberField(TEXT("undo_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(
				TEXT("Set %d/%d pin default(s) in one game-thread entry and one undo step (%d failed, %d skipped)."),
				Applied, Items->Num(), Failed, Skipped);
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d of %d pin defaults failed."), Failed, Items->Num());
				return false;
			}
			return true;
		},
		nullptr,
		0
	});

	// ── rigvm_link_edit_batch ──────────────────────────────────────────────
	Registry.Register({
		TEXT("rigvm_link_edit_batch"),
		TEXT("Add and break many RigVM links in ONE game-thread entry, under a single undo step. "
			 "Rewiring a graph one link at a time costs a game-thread entry and an undo step each, "
			 "and leaves the graph in an intermediate state between calls."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("items"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("action"), FSololmcpSchemaBuilder::String(
								TEXT("Whether to connect or disconnect."), {TEXT("add"), TEXT("break")})},
							{TEXT("output_pin_path"), FSololmcpSchemaBuilder::String(
								TEXT("Source pin path."))},
							{TEXT("input_pin_path"), FSololmcpSchemaBuilder::String(
								TEXT("Destination pin path."))}
						},
						{TEXT("action"), TEXT("output_pin_path"), TEXT("input_pin_path")}),
					TEXT("Link edits to apply, in order. Breaks before adds is usually what a rewire wants."))},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Abort the remaining items on first failure.")), false)}
			},
			{TEXT("asset_path"), TEXT("items")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UControlRigBlueprint* Rig = nullptr;
			URigVMGraph* Graph = nullptr;
			URigVMController* Controller = nullptr;
			if (!ResolveGraph(Context, Args, OutStructured, Rig, Graph, Controller, OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Args->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("items"));
				OutError = TEXT("Missing items array.");
				return false;
			}
			bool bStopOnError = false;
			Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

			const FScopedTransaction Transaction(
				NSLOCTEXT("SOMOLMCP", "RigVMEditLinksBatch", "SOMOLMCP Edit RigVM Links"));

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Added = 0;
			int32 Broken = 0;
			int32 Failed = 0;
			int32 Skipped = 0;

			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);

				if (bStopOnError && Failed > 0)
				{
					Row->SetStringField(TEXT("status"), TEXT("skipped"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Skipped;
					continue;
				}

				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item) || Item == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("item_not_object"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				FString Action;
				FString OutputPath;
				FString InputPath;
				(*Item)->TryGetStringField(TEXT("action"), Action);
				(*Item)->TryGetStringField(TEXT("output_pin_path"), OutputPath);
				(*Item)->TryGetStringField(TEXT("input_pin_path"), InputPath);
				Row->SetStringField(TEXT("action"), Action);
				Row->SetStringField(TEXT("output_pin_path"), OutputPath);
				Row->SetStringField(TEXT("input_pin_path"), InputPath);

				const bool bAdd = Action.Equals(TEXT("add"), ESearchCase::IgnoreCase);
				const bool bBreak = Action.Equals(TEXT("break"), ESearchCase::IgnoreCase);
				if ((!bAdd && !bBreak) || OutputPath.IsEmpty() || InputPath.IsEmpty())
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("missing_or_invalid_fields"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}
				if (Graph->FindPin(OutputPath) == nullptr || Graph->FindPin(InputPath) == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("pin_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				const bool bOk = bAdd
					? Controller->AddLink(OutputPath, InputPath, /*bSetupUndoRedo=*/true)
					: Controller->BreakLink(OutputPath, InputPath, /*bSetupUndoRedo=*/true);

				if (bOk)
				{
					Row->SetStringField(TEXT("status"), TEXT("ok"));
					bAdd ? ++Added : ++Broken;
				}
				else
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					// RigVM refuses links that would create a cycle or mismatch types;
					// naming that is more useful than a bare false.
					Row->SetStringField(TEXT("failure"), bAdd
						? TEXT("link_rejected_type_mismatch_or_cycle")
						: TEXT("link_not_present"));
					++Failed;
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Items->Num());
			OutStructured->SetNumberField(TEXT("added"), Added);
			OutStructured->SetNumberField(TEXT("broken"), Broken);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetNumberField(TEXT("skipped"), Skipped);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetNumberField(TEXT("undo_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(
				TEXT("Added %d and broke %d link(s) in one game-thread entry (%d failed, %d skipped)."),
				Added, Broken, Failed, Skipped);
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d of %d link edits failed."), Failed, Items->Num());
				return false;
			}
			return true;
		},
		nullptr,
		0
	});

	// ── rigvm_graph_snapshot ───────────────────────────────────────────────
	Registry.Register({
		TEXT("rigvm_graph_snapshot"),
		TEXT("Export the whole RigVM graph — nodes, pins and links — as one JSON document. "
			 "Take a snapshot before a queued graph edit so the result can be diffed against it, "
			 "which is the only way to tell a no-op edit from a successful one."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), AssetArgSchema()},
				{TEXT("include_pins"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Include per-node pin detail.")), true)},
				{TEXT("max_pin_depth"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Sub-pin recursion depth.")), 1)}
			},
			{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UControlRigBlueprint* Rig = nullptr;
			URigVMGraph* Graph = nullptr;
			URigVMController* Controller = nullptr;
			if (!ResolveGraph(Context, Args, OutStructured, Rig, Graph, Controller, OutError))
			{
				return false;
			}

			bool bIncludePins = true;
			Args->TryGetBoolField(TEXT("include_pins"), bIncludePins);
			int32 MaxPinDepth = 1;
			Args->TryGetNumberField(TEXT("max_pin_depth"), MaxPinDepth);
			MaxPinDepth = FMath::Clamp(MaxPinDepth, 0, 8);

			TArray<TSharedPtr<FJsonValue>> NodesJson;
			for (const URigVMNode* Node : Graph->GetNodes())
			{
				if (Node != nullptr)
				{
					NodesJson.Add(MakeShared<FJsonValueObject>(DescribeNode(Node, bIncludePins, MaxPinDepth)));
				}
			}

			TArray<TSharedPtr<FJsonValue>> LinksJson;
			for (const URigVMLink* Link : Graph->GetLinks())
			{
				if (Link == nullptr)
				{
					continue;
				}
				const URigVMPin* Source = Link->GetSourcePin();
				const URigVMPin* Target = Link->GetTargetPin();
				if (Source == nullptr || Target == nullptr)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("output_pin_path"), Source->GetPinPath());
				Row->SetStringField(TEXT("input_pin_path"), Target->GetPinPath());
				LinksJson.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetStringField(TEXT("asset_path"), Rig->GetPathName());
			OutStructured->SetArrayField(TEXT("nodes"), NodesJson);
			OutStructured->SetArrayField(TEXT("links"), LinksJson);
			OutStructured->SetNumberField(TEXT("node_count"), NodesJson.Num());
			OutStructured->SetNumberField(TEXT("link_count"), LinksJson.Num());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Snapshot of '%s': %d node(s), %d link(s)."),
				*Rig->GetName(), NodesJson.Num(), LinksJson.Num());
			return true;
		},
		nullptr,
		5
	});
}

} // namespace UE::SOMOLMCP
