// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 Control Rig Dynamics particle graph authoring tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#if SOMOLMCP_WITH_UE58_MESHPARTITION

#include "ControlRigBlueprintLegacy.h"
#include "Dom/JsonObject.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "RigDynamicsParticleExecution.h"
#include "RigVMEditorBlueprintLibrary.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMPin.h"

namespace UE::SOMOLMCP
{
namespace UE58ControlRigDynamics
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
static UControlRigBlueprint* LoadRig(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	FString& AssetPath,
	FString& Error)
{
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Error = TEXT("asset_path is required.");
		return nullptr;
	}
	UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, Error));
	if (!Rig && Error.IsEmpty()) Error = FString::Printf(TEXT("Control Rig asset was not found: %s"), *AssetPath);
	return Rig;
}

static URigVMController* GetController(UControlRigBlueprint* Rig, FString& Error)
{
	URigVMController* Controller = Rig ? URigVMEditorBlueprintLibrary::GetController(Rig) : nullptr;
	if (!Controller) Error = TEXT("RigVM controller is unavailable for the Control Rig asset.");
	return Controller;
}

static URigVMUnitNode* FindParticleNode(URigVMController* Controller, const FString& NodeName, FString& Error)
{
	URigVMGraph* Graph = Controller ? Controller->GetGraph() : nullptr;
	URigVMNode* Node = Graph ? Graph->FindNode(NodeName) : nullptr;
	if (!Node && Graph) Node = Graph->FindNodeByName(FName(*NodeName));
	URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node);
	if (!UnitNode || UnitNode->GetScriptStruct() != FRigUnit_SpawnDynamicsParticle::StaticStruct())
	{
		Error = FString::Printf(TEXT("Spawn Dynamics Particle node was not found: %s"), *NodeName);
		return nullptr;
	}
	return UnitNode;
}

static TSharedRef<FJsonObject> DescribeNode(URigVMUnitNode* Node)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_name"), Node->GetName());
	Result->SetStringField(TEXT("node_path"), Node->GetNodePath());
	Result->SetStringField(TEXT("struct_path"), Node->GetScriptStruct()->GetPathName());
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (URigVMPin* Pin : Node->GetPins())
	{
		if (!Pin) continue;
		TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("name"), Pin->GetName());
		PinJson->SetStringField(TEXT("path"), Pin->GetPinPath());
		PinJson->SetStringField(TEXT("cpp_type"), Pin->GetCPPType());
		PinJson->SetStringField(TEXT("default_value"), Pin->GetDefaultValue());
		Pins.Add(MakeShared<FJsonValueObject>(PinJson));
	}
	Result->SetArrayField(TEXT("pins"), Pins);
	Result->SetNumberField(TEXT("pin_count"), Pins.Num());
	return Result;
}

static bool CompileAndSave(
	const FSololmcpToolExecutionContext& Context,
	UControlRigBlueprint* Rig,
	const FString& AssetPath,
	TSharedRef<FJsonObject>& Out,
	FString& Error)
{
	FCompilerResultsLog CompileLog;
	FKismetEditorUtilities::CompileBlueprint(Rig, EBlueprintCompileOptions::None, &CompileLog);
	const bool bCompiled = Rig->Status != BS_Error && CompileLog.NumErrors == 0;
	Out->SetBoolField(TEXT("compiled"), bCompiled);
	Out->SetNumberField(TEXT("compile_errors"), CompileLog.NumErrors);
	Out->SetNumberField(TEXT("compile_warnings"), CompileLog.NumWarnings);
	if (!bCompiled)
	{
		Error = FString::Printf(TEXT("Control Rig Dynamics compile failed with %d error(s)."), CompileLog.NumErrors);
		return false;
	}
	if (!Context.Services.SaveAsset(AssetPath, false, Error)) return false;
	Out->SetBoolField(TEXT("saved"), true);
	return true;
}

static bool Execute(
	const FString& Name,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Args,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FString AssetPath;
	UControlRigBlueprint* Rig = LoadRig(Context, Args, AssetPath, Error);
	if (!Rig) return false;
	URigVMController* Controller = GetController(Rig, Error);
	if (!Controller) return false;
	Out->SetStringField(TEXT("asset_path"), AssetPath);

	if (Name == TEXT("control_rig_particle_dynamics_node_add"))
	{
		double X = 0.0;
		double Y = 0.0;
		Args->TryGetNumberField(TEXT("position_x"), X);
		Args->TryGetNumberField(TEXT("position_y"), Y);
		URigVMUnitNode* Node = Controller->AddUnitNode(
			FRigUnit_SpawnDynamicsParticle::StaticStruct(),
			FRigUnit::GetMethodName(),
			FVector2D(static_cast<float>(X), static_cast<float>(Y)),
			FString(), true, false);
		if (!Node)
		{
			Error = TEXT("Failed to add the UE 5.8 Spawn Dynamics Particle node.");
			return false;
		}
		FString ParticleName;
		if (Args->TryGetStringField(TEXT("particle_name"), ParticleName) && !ParticleName.IsEmpty())
		{
			if (!Controller->SetPinDefaultValue(Node->GetName() + TEXT(".ParticleComponentName"), ParticleName, true, true, false, false, true))
			{
				Error = TEXT("Failed to set ParticleComponentName on the new dynamics node.");
				return false;
			}
		}
		Out->SetObjectField(TEXT("node"), DescribeNode(Node));
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Added, compiled, saved, and read back dynamics particle node %s."), *Node->GetName());
		return true;
	}
	if (Name == TEXT("control_rig_particle_dynamics_configure"))
	{
		FString NodeName;
		if (!Args->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
		{
			Error = TEXT("node_name is required.");
			return false;
		}
		URigVMUnitNode* Node = FindParticleNode(Controller, NodeName, Error);
		if (!Node) return false;
		const TSharedPtr<FJsonObject>* PinValues = nullptr;
		if (!Args->TryGetObjectField(TEXT("pin_values"), PinValues) || !PinValues || (*PinValues)->Values.IsEmpty())
		{
			Error = TEXT("pin_values must contain at least one node-relative pin value string.");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Updated;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PinValues)->Values)
		{
			FString Value;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Value))
			{
				Error = FString::Printf(TEXT("pin_values.%s must be a string."), *Pair.Key);
				return false;
			}
			if (!Node->FindPin(Pair.Key))
			{
				Error = FString::Printf(TEXT("Pin is unavailable on the dynamics particle node: %s"), *Pair.Key);
				return false;
			}
			const FString PinPath = Node->GetName() + TEXT(".") + Pair.Key;
			if (!Controller->SetPinDefaultValue(PinPath, Value, true, true, false, false, true))
			{
				Error = FString::Printf(TEXT("Failed to set dynamics particle pin: %s"), *PinPath);
				return false;
			}
			Updated.Add(MakeShared<FJsonValueString>(PinPath));
		}
		Out->SetArrayField(TEXT("updated_pins"), Updated);
		Out->SetObjectField(TEXT("node"), DescribeNode(Node));
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Configured %d dynamics particle pin(s), compiled, saved, and read back the node."), Updated.Num());
		return true;
	}
	if (Name == TEXT("control_rig_particle_dynamics_validate"))
	{
		int32 ParticleNodeCount = 0;
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (URigVMNode* Node : Controller->GetGraph()->GetNodes())
		{
			if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
			{
				if (UnitNode->GetScriptStruct() == FRigUnit_SpawnDynamicsParticle::StaticStruct())
				{
					++ParticleNodeCount;
					Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(UnitNode)));
				}
			}
		}
		Out->SetNumberField(TEXT("particle_node_count"), ParticleNodeCount);
		Out->SetArrayField(TEXT("particle_nodes"), Nodes);
		if (!CompileAndSave(Context, Rig, AssetPath, Out, Error)) return false;
		Summary = FString::Printf(TEXT("Validated and compiled Control Rig with %d Spawn Dynamics Particle node(s)."), ParticleNodeCount);
		return true;
	}

	Error = FString::Printf(TEXT("Unsupported UE 5.8 Control Rig Dynamics tool: %s"), *Name);
	return false;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Control Rig Blueprint asset path."))},
		{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Existing Spawn Dynamics Particle RigVM node name or path."))},
		{TEXT("particle_name"), FSololmcpSchemaBuilder::String(TEXT("Optional component name written to the new dynamics particle node."))},
		{TEXT("position_x"), FSololmcpSchemaBuilder::Number(TEXT("Graph node X position."))},
		{TEXT("position_y"), FSololmcpSchemaBuilder::Number(TEXT("Graph node Y position."))},
		{TEXT("pin_values"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Map of node-relative pin paths to RigVM default-value strings."))}
	});
}
#endif
}

void RegisterUE58ControlRigDynamicsTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	static const TCHAR* Names[] = {
		TEXT("control_rig_particle_dynamics_node_add"),
		TEXT("control_rig_particle_dynamics_configure"),
		TEXT("control_rig_particle_dynamics_validate")
	};
	for (const TCHAR* NamePtr : Names)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = FString::Printf(TEXT("UE 5.8 Control Rig Dynamics particle graph transaction: %s"), *Name);
		Def.InputSchema = UE58ControlRigDynamics::Schema();
		Def.CacheTtlSeconds = 0;
		Def.Execute = [Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return UE58ControlRigDynamics::Execute(Name, Context, Args, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
#endif
}
}
#else
namespace UE::SOMOLMCP
{
void RegisterUE58ControlRigDynamicsTools(FSololmcpToolRegistry&)
{
}
}
#endif
