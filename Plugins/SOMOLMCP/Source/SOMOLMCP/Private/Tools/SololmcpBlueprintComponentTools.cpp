// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — Blueprint Component Panel + Batch Edit Tools
//
// Adds 6 tools (4 component panel + 2 batch edit):
//   blueprint_add_component
//   blueprint_remove_component
//   blueprint_set_component_property
//   blueprint_list_components
//   blueprint_batch_edit
//   blueprint_safe_patch
//   behaviortree_batch_edit
//
// IMPORTANT: This file is NOT auto-wired. To activate, the registry author must:
//   1. Forward-declare in SololmcpToolRegistry.h:
//        void RegisterBlueprintComponentTools(FSololmcpToolRegistry& Registry);
//   2. Call from FSololmcpToolRegistry::FSololmcpToolRegistry() in SololmcpToolRegistry.cpp:
//        RegisterBlueprintComponentTools(*this);
//
// TODO(P0-5): Confirm USCS_Node parent-child attach API on UE 5.7
//   (using AddChildNode / SetParent fallbacks below).
// TODO(P0-8): If `transform` arg specifies values for a non-USceneComponent class,
//   they are silently ignored. Consider an explicit error.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "Services/SololmcpEditorServices.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"

#define LOCTEXT_NAMESPACE "SOMOLMCP_BPComponent"

namespace UE::SOMOLMCP
{
	namespace
	{
		// ---------------- File-static helpers (mirrors SololmcpDomainTools.cpp) ----------------

		UBlueprint* LoadBlueprintAssetLocal(FSololmcpEditorServices& Services, const FString& AssetPath, FString& OutError)
		{
			UObject* Asset = Services.LoadAsset(AssetPath, OutError);
			if (!Asset)
			{
				return nullptr;
			}
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			{
				return Blueprint;
			}
			OutError = TEXT("Asset is not a Blueprint.");
			return nullptr;
		}

		/**
		 * Resolve a component class name. Accepts:
		 *   - Short names: "StaticMeshComponent", "PointLightComponent", "CameraComponent", "SpringArmComponent"
		 *   - Full object paths: "/Script/Engine.StaticMeshComponent"
		 *   - Class names with U-prefix: "UStaticMeshComponent"
		 */
		UClass* ResolveComponentClass(FSololmcpEditorServices& Services, const FString& InName, FString& OutError)
		{
			if (InName.IsEmpty())
			{
				OutError = TEXT("component_class is empty.");
				return nullptr;
			}

			// 1) Try as a full path via Services.
			UClass* Cls = Services.ResolveClass(InName, OutError);
			if (Cls && Cls->IsChildOf(UActorComponent::StaticClass()))
			{
				OutError.Reset();
				return Cls;
			}

			// 2) Try common short-name shortcuts via FindFirstObject<UClass>.
			FString ShortName = InName;
			if (ShortName.StartsWith(TEXT("U")) && ShortName.Len() > 1 && FChar::IsUpper(ShortName[1]))
			{
				ShortName = ShortName.RightChop(1);
			}
			OutError.Reset();
			Cls = FindFirstObject<UClass>(*ShortName, EFindFirstObjectOptions::EnsureIfAmbiguous);
			if (!Cls)
			{
				// Also try with "U" prefix
				Cls = FindFirstObject<UClass>(*(FString(TEXT("U")) + ShortName), EFindFirstObjectOptions::EnsureIfAmbiguous);
			}
			if (Cls && Cls->IsChildOf(UActorComponent::StaticClass()))
			{
				return Cls;
			}

			OutError = FString::Printf(TEXT("Failed to resolve component_class '%s' to a UActorComponent subclass."), *InName);
			return nullptr;
		}

		USCS_Node* FindSCSNodeByName(UBlueprint* Blueprint, const FString& ComponentName)
		{
			if (!Blueprint || !Blueprint->SimpleConstructionScript)
			{
				return nullptr;
			}
			const TArray<USCS_Node*> All = Blueprint->SimpleConstructionScript->GetAllNodes();
			for (USCS_Node* Node : All)
			{
				if (!Node)
				{
					continue;
				}
				if (Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return Node;
				}
			}
			return nullptr;
		}

		FString FindSCSParentName(UBlueprint* Blueprint, USCS_Node* Node)
		{
			if (!Blueprint || !Blueprint->SimpleConstructionScript || !Node)
			{
				return FString();
			}
			for (USCS_Node* Other : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Other && Other != Node && Other->GetChildNodes().Contains(Node))
				{
					return Other->GetVariableName().ToString();
				}
			}
			return FString();
		}

		/** An unconstrained JSON Schema node: omitting "type" intentionally accepts every JSON kind. */
		TSharedRef<FJsonObject> AnyJsonSchema(const FString& Description)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("description"), Description);
			return Schema;
		}

		/** Try to parse a JSON `transform` object {location:[x,y,z], rotation:[p,y,r], scale:[x,y,z]}. */
		bool TryParseTransformObject(const TSharedPtr<FJsonObject>& Obj, FTransform& OutTransform)
		{
			if (!Obj.IsValid())
			{
				return false;
			}
			OutTransform = FTransform::Identity;
			auto ReadVec3 = [](const TSharedPtr<FJsonObject>& O, const TCHAR* Field, double& X, double& Y, double& Z) -> bool
			{
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				if (O->TryGetArrayField(Field, Arr) && Arr && Arr->Num() == 3)
				{
					X = (*Arr)[0]->AsNumber();
					Y = (*Arr)[1]->AsNumber();
					Z = (*Arr)[2]->AsNumber();
					return true;
				}
				return false;
			};
			double X = 0, Y = 0, Z = 0;
			if (ReadVec3(Obj, TEXT("location"), X, Y, Z))
			{
				OutTransform.SetLocation(FVector(X, Y, Z));
			}
			if (ReadVec3(Obj, TEXT("rotation"), X, Y, Z))
			{
				// rotation array order: [p, y, r] per spec → Pitch, Yaw, Roll
				OutTransform.SetRotation(FRotator((float)X, (float)Y, (float)Z).Quaternion());
			}
			if (ReadVec3(Obj, TEXT("scale"), X, Y, Z))
			{
				OutTransform.SetScale3D(FVector(X, Y, Z));
			}
			return true;
		}

		TSharedRef<FJsonObject> RelativeTransformToJson(const USceneComponent* SceneComponent)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (!SceneComponent)
			{
				return Obj;
			}
			const FVector Loc = SceneComponent->GetRelativeLocation();
			const FRotator Rot = SceneComponent->GetRelativeRotation();
			const FVector Scale = SceneComponent->GetRelativeScale3D();
			auto VecObj = [](double X, double Y, double Z) -> TSharedRef<FJsonObject>
			{
				TSharedRef<FJsonObject> V = MakeShared<FJsonObject>();
				V->SetNumberField(TEXT("x"), X);
				V->SetNumberField(TEXT("y"), Y);
				V->SetNumberField(TEXT("z"), Z);
				return V;
			};
			Obj->SetObjectField(TEXT("location"), VecObj(Loc.X, Loc.Y, Loc.Z));
			Obj->SetObjectField(TEXT("rotation"), VecObj(Rot.Pitch, Rot.Yaw, Rot.Roll));
			Obj->SetObjectField(TEXT("scale"), VecObj(Scale.X, Scale.Y, Scale.Z));
			return Obj;
		}

		bool RelativeTransformMatches(const USceneComponent* SceneComponent, const FTransform& Expected)
		{
			if (!SceneComponent)
			{
				return false;
			}
			return SceneComponent->GetRelativeLocation().Equals(Expected.GetLocation(), 0.01) &&
				SceneComponent->GetRelativeRotation().Equals(Expected.GetRotation().Rotator(), 0.01) &&
				SceneComponent->GetRelativeScale3D().Equals(Expected.GetScale3D(), 0.0001);
		}

		/** Get the live template object for an SCS node (used for both reads and writes). */
		UObject* GetSCSTemplate(USCS_Node* Node, UBlueprint* Blueprint)
		{
			if (!Node)
			{
				return nullptr;
			}
			// GetActualComponentTemplate returns the most-derived template (handling inherited overrides).
			if (Blueprint && Blueprint->GeneratedClass)
			{
				if (UActorComponent* Tpl = Node->GetActualComponentTemplate(Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass)))
				{
					return Tpl;
				}
			}
			return Node->ComponentTemplate;
		}

		/** Build a JSON summary {name, class, parent, properties_summary} for one SCS node. */
		TSharedRef<FJsonObject> SCSNodeToJsonSummary(USCS_Node* Node, UBlueprint* Blueprint, int32 PropertyLimit = 8)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (!Node)
			{
				return Obj;
			}
			Obj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());

			UObject* Template = GetSCSTemplate(Node, Blueprint);
			if (Template)
			{
				Obj->SetStringField(TEXT("class"), Template->GetClass()->GetName());
			}
			else if (Node->ComponentClass)
			{
				Obj->SetStringField(TEXT("class"), Node->ComponentClass->GetName());
			}

			// Parent — walk SCS to find which node owns this as a child, fall back to the explicit parent name fields.
			FString ParentName;
			if (Blueprint && Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* Other : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (!Other || Other == Node)
					{
						continue;
					}
					if (Other->GetChildNodes().Contains(Node))
					{
						ParentName = Other->GetVariableName().ToString();
						break;
					}
				}
			}
			Obj->SetStringField(TEXT("parent"), ParentName);

			// Properties summary — top N visible UPROPERTYs with stringified current value.
			TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
			int32 Count = 0;
			if (Template)
			{
				for (TFieldIterator<FProperty> It(Template->GetClass()); It && Count < PropertyLimit; ++It)
				{
					FProperty* Prop = *It;
					if (!Prop)
					{
						continue;
					}
					// Skip hidden / transient / native-only.
					if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DisableEditOnInstance))
					{
						continue;
					}
					if (!Prop->HasAnyPropertyFlags(CPF_Edit))
					{
						continue;
					}
					FString ValueStr;
					Prop->ExportText_Direct(ValueStr, Prop->ContainerPtrToValuePtr<void>(Template), Prop->ContainerPtrToValuePtr<void>(Template), nullptr, PPF_None);
					if (ValueStr.Len() > 256)
					{
						ValueStr = ValueStr.Left(253) + TEXT("...");
					}
					Props->SetStringField(Prop->GetName(), ValueStr);
					++Count;
				}
			}
			Obj->SetObjectField(TEXT("properties_summary"), Props);
			return Obj;
		}

		// ---------------- Batch executor (shared by blueprint_batch_edit / behaviortree_batch_edit) ----------------

		bool ExecuteBatch(FSololmcpToolRegistry& Registry,
			const TSharedRef<FJsonObject>& Arguments,
			const FString& AllowedPrefix,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("operations"), Operations) || !Operations)
			{
				OutError = TEXT("Missing operations array.");
				return false;
			}

			const bool bContinueOnError = Arguments->HasTypedField<EJson::Boolean>(TEXT("continue_on_error"))
				&& Arguments->GetBoolField(TEXT("continue_on_error"));

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 SucceededCount = 0;
			int32 FailedCount = 0;

			for (int32 Index = 0; Index < Operations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> OpObject =
					(*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;

				TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
				StepResult->SetNumberField(TEXT("index"), Index);

				if (!OpObject.IsValid())
				{
					StepResult->SetBoolField(TEXT("ok"), false);
					StepResult->SetStringField(TEXT("error"), TEXT("operation entry is not an object"));
					Results.Add(MakeShared<FJsonValueObject>(StepResult));
					++FailedCount;
					if (!bContinueOnError) { break; }
					continue;
				}

				FString ToolName;
				const TSharedPtr<FJsonObject>* OpArgsPtr = nullptr;
				if (!OpObject->TryGetStringField(TEXT("tool"), ToolName)
					|| !OpObject->TryGetObjectField(TEXT("arguments"), OpArgsPtr)
					|| !OpArgsPtr || !OpArgsPtr->IsValid())
				{
					StepResult->SetBoolField(TEXT("ok"), false);
					StepResult->SetStringField(TEXT("error"), TEXT("operation must include tool (string) and arguments (object)"));
					Results.Add(MakeShared<FJsonValueObject>(StepResult));
					++FailedCount;
					if (!bContinueOnError) { break; }
					continue;
				}

				StepResult->SetStringField(TEXT("tool"), ToolName);

				// Prefix gate to prevent abuse: only same-domain tools can be called.
				if (!ToolName.StartsWith(AllowedPrefix))
				{
					StepResult->SetBoolField(TEXT("ok"), false);
					StepResult->SetStringField(TEXT("error"),
						FString::Printf(TEXT("tool '%s' is not allowed in this batch (must start with '%s')"),
							*ToolName, *AllowedPrefix));
					Results.Add(MakeShared<FJsonValueObject>(StepResult));
					++FailedCount;
					if (!bContinueOnError) { break; }
					continue;
				}

				// Disallow recursive batch calls to avoid pathological depth.
				if (ToolName == TEXT("blueprint_batch_edit") || ToolName == TEXT("behaviortree_batch_edit"))
				{
					StepResult->SetBoolField(TEXT("ok"), false);
					StepResult->SetStringField(TEXT("error"), TEXT("nested batch edit is not allowed"));
					Results.Add(MakeShared<FJsonValueObject>(StepResult));
					++FailedCount;
					if (!bContinueOnError) { break; }
					continue;
				}

				TSharedRef<FJsonObject> StepStructured = MakeShared<FJsonObject>();
				FString StepSummary;
				FString StepError;
				bool bStepOk = false;

				// Each op runs in its own try-equivalent — UE typically disables exceptions, so we rely on
				// ExecuteTool's bool return contract.
				bStepOk = Registry.ExecuteTool(ToolName, OpArgsPtr->ToSharedRef(), StepStructured, StepSummary, StepError);

				StepResult->SetBoolField(TEXT("ok"), bStepOk);
				StepResult->SetStringField(TEXT("summary"), StepSummary);
				if (!StepError.IsEmpty())
				{
					StepResult->SetStringField(TEXT("error"), StepError);
				}
				StepResult->SetObjectField(TEXT("result"), StepStructured);
				Results.Add(MakeShared<FJsonValueObject>(StepResult));

				if (bStepOk)
				{
					++SucceededCount;
				}
				else
				{
					++FailedCount;
					if (!bContinueOnError)
					{
						break;
					}
				}
			}

			OutStructured->SetNumberField(TEXT("succeeded"), SucceededCount);
			OutStructured->SetNumberField(TEXT("failed"), FailedCount);
			OutStructured->SetArrayField(TEXT("results"), Results);
			OutSummary = FString::Printf(TEXT("Executed %d operations (%d succeeded, %d failed)."),
				Results.Num(), SucceededCount, FailedCount);

			if (FailedCount > 0 && OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Batch completed with %d failure(s)."), FailedCount);
			}
			return FailedCount == 0;
		}

		bool IsBlueprintSafePatchAllowedTool(const FString& ToolName)
		{
			return ToolName == TEXT("blueprint_add_component")
				|| ToolName == TEXT("blueprint_remove_component")
				|| ToolName == TEXT("blueprint_set_component_property");
		}

		bool IsSafePatchOperationIdValid(const FString& OperationId)
		{
			if (OperationId.IsEmpty() || OperationId.Len() > 128)
			{
				return false;
			}

			for (const TCHAR Ch : OperationId)
			{
				const bool bOk =
					(Ch >= 'A' && Ch <= 'Z')
					|| (Ch >= 'a' && Ch <= 'z')
					|| (Ch >= '0' && Ch <= '9')
					|| Ch == '_'
					|| Ch == '-'
					|| Ch == '.'
					|| Ch == ':';
				if (!bOk)
				{
					return false;
				}
			}
			return true;
		}

		bool ValidateBlueprintSafePatch(
			FSololmcpEditorServices& Services,
			const TSharedRef<FJsonObject>& Arguments,
			FString& OutAssetPath,
			FString& OutOperationId,
			TArray<TSharedPtr<FJsonValue>>& OutPlan,
			FString& OutError)
		{
			FString ContractVersion;
			if (!Arguments->TryGetStringField(TEXT("contract_version"), ContractVersion)
				|| ContractVersion != TEXT("blueprint_safe_patch.v1"))
			{
				OutError = TEXT("Invalid contract_version. Expected 'blueprint_safe_patch.v1'.");
				return false;
			}

			if (!Arguments->TryGetStringField(TEXT("operation_id"), OutOperationId)
				|| !IsSafePatchOperationIdValid(OutOperationId))
			{
				OutError = TEXT("Invalid operation_id. Use 1-128 chars: A-Z a-z 0-9 _ - . :");
				return false;
			}

			if (!Arguments->TryGetStringField(TEXT("asset_path"), OutAssetPath)
				|| OutAssetPath.IsEmpty()
				|| !OutAssetPath.StartsWith(TEXT("/Game/")))
			{
				OutError = TEXT("Invalid root asset_path. Expected a non-empty /Game/... Blueprint asset path.");
				return false;
			}

			FString LoadError;
			if (!LoadBlueprintAssetLocal(Services, OutAssetPath, LoadError))
			{
				OutError = FString::Printf(TEXT("Invalid root asset_path '%s': %s"), *OutAssetPath, *LoadError);
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("operations"), Operations) || !Operations)
			{
				OutError = TEXT("Missing operations array.");
				return false;
			}
			if (Operations->Num() <= 0)
			{
				OutError = TEXT("operations must contain at least one operation.");
				return false;
			}

			for (int32 Index = 0; Index < Operations->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> OpObject =
					(*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
				if (!OpObject.IsValid())
				{
					OutError = FString::Printf(TEXT("operations[%d] must be an object."), Index);
					return false;
				}

				FString ToolName;
				const TSharedPtr<FJsonObject>* OpArgsPtr = nullptr;
				if (!OpObject->TryGetStringField(TEXT("tool"), ToolName)
					|| !OpObject->TryGetObjectField(TEXT("arguments"), OpArgsPtr)
					|| !OpArgsPtr || !OpArgsPtr->IsValid())
				{
					OutError = FString::Printf(TEXT("operations[%d] must include tool (string) and arguments (object)."), Index);
					return false;
				}

				if (!IsBlueprintSafePatchAllowedTool(ToolName))
				{
					OutError = FString::Printf(TEXT("operations[%d] tool '%s' is not allowed by blueprint_safe_patch."), Index, *ToolName);
					return false;
				}

				FString OperationAssetPath;
				if (!(*OpArgsPtr)->TryGetStringField(TEXT("asset_path"), OperationAssetPath)
					|| OperationAssetPath != OutAssetPath)
				{
					OutError = FString::Printf(TEXT("operations[%d].arguments.asset_path must match root asset_path '%s'."), Index, *OutAssetPath);
					return false;
				}

				TSharedRef<FJsonObject> PlanEntry = MakeShared<FJsonObject>();
				PlanEntry->SetNumberField(TEXT("index"), Index);
				PlanEntry->SetStringField(TEXT("tool"), ToolName);
				PlanEntry->SetStringField(TEXT("asset_path"), OperationAssetPath);
				PlanEntry->SetStringField(TEXT("status"), TEXT("validated"));
				OutPlan.Add(MakeShared<FJsonValueObject>(PlanEntry));
			}

			return true;
		}
	} // namespace anon

	// ============================================================================
	// Public registration
	// ============================================================================

	void RegisterBlueprintComponentTools(FSololmcpToolRegistry& Registry)
	{
		// ----------------------------------------------------------------------
		// 1) blueprint_add_component
		// ----------------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_add_component"),
			TEXT("Add a component (SCS node) to a Blueprint. Optionally attach to a parent node and apply a transform."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UBlueprint asset path, e.g. /Game/MyBP"))},
					{TEXT("component_class"), FSololmcpSchemaBuilder::String(TEXT("Short name (StaticMeshComponent) or full path (/Script/Engine.StaticMeshComponent)"))},
					{TEXT("component_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("parent_component"), FSololmcpSchemaBuilder::String(TEXT("Parent SCS node variable name. Defaults to root."))},
					{TEXT("transform"), FSololmcpSchemaBuilder::Object({
						{TEXT("location"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number())},
						{TEXT("rotation"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number())},
						{TEXT("scale"),    FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number())}
					})}
				},
				{TEXT("asset_path"), TEXT("component_class"), TEXT("component_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ClassName, ComponentName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)
					|| !Arguments->TryGetStringField(TEXT("component_class"), ClassName)
					|| !Arguments->TryGetStringField(TEXT("component_name"), ComponentName))
				{
					OutError = TEXT("Missing asset_path, component_class, or component_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAssetLocal(Context.Services, AssetPath, OutError);
				if (!Blueprint) { return false; }

				if (!Blueprint->SimpleConstructionScript)
				{
					OutError = TEXT("Blueprint has no SimpleConstructionScript (not an Actor BP?).");
					return false;
				}

				UClass* ComponentClass = ResolveComponentClass(Context.Services, ClassName, OutError);
				if (!ComponentClass) { return false; }

				if (FindSCSNodeByName(Blueprint, ComponentName))
				{
					OutError = FString::Printf(TEXT("A component named '%s' already exists."), *ComponentName);
					return false;
				}

				FString ParentName;
				Arguments->TryGetStringField(TEXT("parent_component"), ParentName);
				const TSharedPtr<FJsonObject>* TransformObjPtr = nullptr;
				const bool bHasTransformArg = Arguments->TryGetObjectField(TEXT("transform"), TransformObjPtr) && TransformObjPtr && TransformObjPtr->IsValid();
				FTransform RequestedTransform = FTransform::Identity;
				const bool bTransformParsed = bHasTransformArg ? TryParseTransformObject(*TransformObjPtr, RequestedTransform) : false;
				if (bHasTransformArg && !ComponentClass->IsChildOf(USceneComponent::StaticClass()))
				{
					OutError = FString::Printf(TEXT("transform was provided but component_class '%s' is not a USceneComponent subclass."), *ClassName);
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPAddComponent", "SOMOLMCP Add Blueprint Component"));
				Blueprint->Modify();
				Blueprint->SimpleConstructionScript->Modify();

				USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
				USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
				if (!NewNode)
				{
					OutError = TEXT("Failed to create SCS node.");
					return false;
				}

				// Attach: prefer named parent → root → top-level.
				FString AttachedParent;
				if (!ParentName.IsEmpty())
				{
					if (USCS_Node* ParentNode = FindSCSNodeByName(Blueprint, ParentName))
					{
						ParentNode->Modify();
						ParentNode->AddChildNode(NewNode);
						AttachedParent = ParentName;
					}
					else
					{
						OutError = FString::Printf(TEXT("parent_component '%s' was not found."), *ParentName);
						return false;
					}
				}
				else
				{
					// Try to attach to scene root, else top-level.
					USCS_Node* RootNode = SCS->GetDefaultSceneRootNode();
					if (RootNode && NewNode->ComponentClass && NewNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
					{
						RootNode->Modify();
						RootNode->AddChildNode(NewNode);
						AttachedParent = RootNode->GetVariableName().ToString();
					}
					else
					{
						SCS->AddNode(NewNode);
						AttachedParent = TEXT("");
					}
				}

				// Apply transform if it's a USceneComponent.
				if (bHasTransformArg)
				{
					if (bTransformParsed)
					{
						if (USceneComponent* SceneTpl = Cast<USceneComponent>(NewNode->ComponentTemplate))
						{
							SceneTpl->Modify();
							SceneTpl->SetRelativeLocation(RequestedTransform.GetLocation());
							SceneTpl->SetRelativeRotation(RequestedTransform.GetRotation().Rotator());
							SceneTpl->SetRelativeScale3D(RequestedTransform.GetScale3D());
						}
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				USCS_Node* VerifiedNode = FindSCSNodeByName(Blueprint, ComponentName);
				if (!VerifiedNode)
				{
					OutError = FString::Printf(TEXT("Component '%s' was not present after creation."), *ComponentName);
					return false;
				}
				if (!ParentName.IsEmpty() && FindSCSParentName(Blueprint, VerifiedNode) != ParentName)
				{
					OutError = FString::Printf(TEXT("Component '%s' was not attached to requested parent '%s'."), *ComponentName, *ParentName);
					return false;
				}

				OutStructured->SetStringField(TEXT("component_name"), ComponentName);
				OutStructured->SetStringField(TEXT("class"), ComponentClass->GetName());
				OutStructured->SetStringField(TEXT("parent"), AttachedParent);
				const FString ActualParent = FindSCSParentName(Blueprint, VerifiedNode);
				USceneComponent* VerifiedSceneTemplate = Cast<USceneComponent>(GetSCSTemplate(VerifiedNode, Blueprint));
				TSharedRef<FJsonObject> AttachReceipt = MakeShared<FJsonObject>();
				AttachReceipt->SetStringField(TEXT("requested_parent"), ParentName);
				AttachReceipt->SetStringField(TEXT("actual_parent"), ActualParent);
				AttachReceipt->SetBoolField(TEXT("attached_to_requested_parent"), ParentName.IsEmpty() || ActualParent.Equals(ParentName, ESearchCase::IgnoreCase));
				AttachReceipt->SetBoolField(TEXT("transform_requested"), bHasTransformArg);
				AttachReceipt->SetBoolField(TEXT("transform_parsed"), bTransformParsed);
				AttachReceipt->SetBoolField(TEXT("transform_applicable"), VerifiedSceneTemplate != nullptr);
				AttachReceipt->SetBoolField(TEXT("transform_verified"), !bHasTransformArg || RelativeTransformMatches(VerifiedSceneTemplate, RequestedTransform));
				AttachReceipt->SetObjectField(TEXT("actual_relative_transform"), RelativeTransformToJson(VerifiedSceneTemplate));
				OutStructured->SetObjectField(TEXT("attach_transform_receipt"), AttachReceipt);
				OutSummary = FString::Printf(TEXT("Added component '%s' (%s) to %s."),
					*ComponentName, *ComponentClass->GetName(), *AssetPath);
				return true;
			}
		, nullptr
		, 0
		});

		// ----------------------------------------------------------------------
		// 2) blueprint_remove_component
		// ----------------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_remove_component"),
			TEXT("Remove a component (SCS node) from a Blueprint by name."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("component_name"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("component_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ComponentName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)
					|| !Arguments->TryGetStringField(TEXT("component_name"), ComponentName))
				{
					OutError = TEXT("Missing asset_path or component_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAssetLocal(Context.Services, AssetPath, OutError);
				if (!Blueprint) { return false; }
				if (!Blueprint->SimpleConstructionScript)
				{
					OutError = TEXT("Blueprint has no SimpleConstructionScript.");
					return false;
				}

				USCS_Node* Target = FindSCSNodeByName(Blueprint, ComponentName);
				if (!Target)
				{
					OutError = FString::Printf(TEXT("Component '%s' not found."), *ComponentName);
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("BPRemoveComponent", "SOMOLMCP Remove Blueprint Component"));
				Blueprint->Modify();
				Blueprint->SimpleConstructionScript->Modify();
				Blueprint->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Target);

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (FindSCSNodeByName(Blueprint, ComponentName))
				{
					OutError = FString::Printf(TEXT("Component '%s' was still present after removal."), *ComponentName);
					return false;
				}

				OutStructured->SetBoolField(TEXT("removed"), true);
				OutStructured->SetStringField(TEXT("component_name"), ComponentName);
				OutSummary = FString::Printf(TEXT("Removed component '%s'."), *ComponentName);
				return true;
			}
		, nullptr
		, 0
		});

		// ----------------------------------------------------------------------
		// 3) blueprint_set_component_property
		// ----------------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_set_component_property"),
			TEXT("Set a reflected Blueprint component-template property from typed JSON. Supports arrays, sets, maps, object/soft references, structs, enums, and scalar properties; validates before the transaction and returns post-apply readback."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("component_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("property_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("value"), AnyJsonSchema(TEXT("Any JSON value. Arrays map to TArray/TSet; maps accept an object for string-like keys or [{key,value}]; structs accept objects; object and soft references accept asset paths or null."))}
				},
				{TEXT("asset_path"), TEXT("component_name"), TEXT("property_name"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ComponentName, PropertyName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)
					|| !Arguments->TryGetStringField(TEXT("component_name"), ComponentName)
					|| !Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
				{
					OutError = TEXT("Missing asset_path, component_name, or property_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAssetLocal(Context.Services, AssetPath, OutError);
				if (!Blueprint) { return false; }

				USCS_Node* Node = FindSCSNodeByName(Blueprint, ComponentName);
				if (!Node)
				{
					OutError = FString::Printf(TEXT("Component '%s' not found."), *ComponentName);
					return false;
				}

				UObject* TemplateObj = GetSCSTemplate(Node, Blueprint);
				if (!TemplateObj)
				{
					OutError = TEXT("Component template object is null.");
					return false;
				}

				FProperty* Property = TemplateObj->GetClass()->FindPropertyByName(FName(*PropertyName));
				if (!Property)
				{
					OutError = FString::Printf(TEXT("Property '%s' not found on class %s."),
						*PropertyName, *TemplateObj->GetClass()->GetName());
					return false;
				}

				// Pull the JSON value field — must exist (declared required above).
				const TSharedPtr<FJsonValue> ValField = Arguments->TryGetField(TEXT("value"));
				if (!ValField.IsValid())
				{
					OutError = TEXT("Missing 'value' field.");
					return false;
				}
				void* ActualValuePtr = Property->ContainerPtrToValuePtr<void>(TemplateObj);
				FString OldValueStr;
				Property->ExportText_Direct(OldValueStr,
					ActualValuePtr,
					ActualValuePtr,
					nullptr, PPF_None);

				// Construct and validate in isolated initialized property storage. A
				// nested conversion failure therefore cannot partially mutate the asset.
				FDefaultConstructedPropertyElement Candidate(Property);
				FString ValidationError;
				if (!Context.Services.ApplyJsonValueToProperty(Candidate.GetObjAddress(), Property, ValField, ValidationError))
				{
					OutError = FString::Printf(TEXT("Invalid value for %s.%s (%s): %s"),
						*ComponentName, *PropertyName, *Property->GetCPPType(), *ValidationError);
					return false;
				}

				FString CandidateValueStr;
				Property->ExportText_Direct(CandidateValueStr,
					Candidate.GetObjAddress(), Candidate.GetObjAddress(), nullptr, PPF_None);
				FDefaultConstructedPropertyElement PreviousValue(Property);
				Property->CopyCompleteValue(PreviousValue.GetObjAddress(), ActualValuePtr);

				FScopedTransaction Transaction(LOCTEXT("BPSetComponentProperty", "SOMOLMCP Set Blueprint Component Property"));
				Blueprint->Modify();
				TemplateObj->Modify();
				TemplateObj->PreEditChange(Property);
				Property->CopyCompleteValue(ActualValuePtr, Candidate.GetObjAddress());
				FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
				TemplateObj->PostEditChangeProperty(ChangeEvent);

				FString NewValueStr;
				Property->ExportText_Direct(NewValueStr,
					ActualValuePtr,
					ActualValuePtr,
					nullptr, PPF_None);
				const bool bReadbackMatches = Property->Identical(ActualValuePtr, Candidate.GetObjAddress(), PPF_None);
				if (!bReadbackMatches)
				{
					Property->CopyCompleteValue(ActualValuePtr, PreviousValue.GetObjAddress());
					TemplateObj->PostEditChange();
					Transaction.Cancel();
					OutError = FString::Printf(TEXT("Readback mismatch for %s.%s after typed assignment (candidate='%s', readback='%s'); change rolled back."),
						*ComponentName, *PropertyName, *CandidateValueStr, *NewValueStr);
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				OutStructured->SetStringField(TEXT("property"), PropertyName);
				OutStructured->SetStringField(TEXT("property_type"), Property->GetCPPType());
				OutStructured->SetField(TEXT("requested_value"), ValField);
				OutStructured->SetStringField(TEXT("old_value"), OldValueStr);
				OutStructured->SetStringField(TEXT("new_value"), NewValueStr);
				OutStructured->SetStringField(TEXT("readback_export"), NewValueStr);
				OutStructured->SetBoolField(TEXT("validated_before_transaction"), true);
				OutStructured->SetBoolField(TEXT("readback_matches"), true);
				OutStructured->SetBoolField(TEXT("changed"), OldValueStr != NewValueStr);
				OutSummary = FString::Printf(TEXT("Set %s.%s : %s → %s"),
					*ComponentName, *PropertyName, *OldValueStr, *NewValueStr);
				return true;
			}
		, nullptr
		, 0
		});

		// ----------------------------------------------------------------------
		// 4) blueprint_list_components
		// ----------------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_list_components"),
			TEXT("List all SCS components on a Blueprint with class, parent, and a 5-10 item property summary."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}
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
				UBlueprint* Blueprint = LoadBlueprintAssetLocal(Context.Services, AssetPath, OutError);
				if (!Blueprint) { return false; }

				TArray<TSharedPtr<FJsonValue>> Components;
				if (Blueprint->SimpleConstructionScript)
				{
					for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
					{
						if (!Node) { continue; }
						Components.Add(MakeShared<FJsonValueObject>(SCSNodeToJsonSummary(Node, Blueprint, 8)));
					}
				}
				OutStructured->SetArrayField(TEXT("components"), Components);
				OutStructured->SetNumberField(TEXT("count"), Components.Num());
				OutSummary = FString::Printf(TEXT("Listed %d components."), Components.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ----------------------------------------------------------------------
		// 5) blueprint_batch_edit
		// ----------------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_batch_edit"),
			TEXT("Run multiple blueprint_* operations in sequence. Each entry: {tool, arguments}. Tools must start with 'blueprint_'."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional context hint; not enforced."))},
					{TEXT("operations"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("tool"), FSololmcpSchemaBuilder::String()},
						{TEXT("arguments"), FSololmcpSchemaBuilder::Object({})}
					}, {TEXT("tool"), TEXT("arguments")}))},
					{TEXT("continue_on_error"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("operations")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return ExecuteBatch(Registry, Arguments, TEXT("blueprint_"), OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 0
		});

		// ----------------------------------------------------------------------
		// 6) blueprint_safe_patch
		// ----------------------------------------------------------------------
		Registry.Register({
			TEXT("blueprint_safe_patch"),
			TEXT("Validate and optionally apply a constrained Blueprint component patch. dry_run defaults to true; only whitelisted component write tools are allowed."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("contract_version"), FSololmcpSchemaBuilder::String(TEXT("Must be 'blueprint_safe_patch.v1'."))},
					{TEXT("operation_id"), FSololmcpSchemaBuilder::String(TEXT("Idempotency/audit id: A-Z a-z 0-9 _ - . :"))},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Root Blueprint asset path. All operations must target this exact asset."))},
					{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Defaults to true. When true, validates and returns a plan without executing write tools."))},
					{TEXT("operations"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("tool"), FSololmcpSchemaBuilder::String(TEXT("Allowed: blueprint_add_component, blueprint_remove_component, blueprint_set_component_property"))},
						{TEXT("arguments"), FSololmcpSchemaBuilder::Object({})}
					}, {TEXT("tool"), TEXT("arguments")}))},
					{TEXT("continue_on_error"), FSololmcpSchemaBuilder::Boolean(TEXT("Apply mode only; defaults to false."))}
				},
				{TEXT("contract_version"), TEXT("operation_id"), TEXT("asset_path"), TEXT("operations")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const bool bDryRun = Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run"))
					? Arguments->GetBoolField(TEXT("dry_run"))
					: true;

				FString AssetPath;
				FString OperationId;
				TArray<TSharedPtr<FJsonValue>> Plan;
				if (!ValidateBlueprintSafePatch(Context.Services, Arguments, AssetPath, OperationId, Plan, OutError))
				{
					return false;
				}

				OutStructured->SetStringField(TEXT("contract_version"), TEXT("blueprint_safe_patch.v1"));
				OutStructured->SetStringField(TEXT("operation_id"), OperationId);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
				OutStructured->SetArrayField(TEXT("plan"), Plan);

				if (bDryRun)
				{
					OutStructured->SetBoolField(TEXT("applied"), false);
					OutStructured->SetStringField(TEXT("compile_status"), TEXT("not_attempted_dry_run"));
					OutStructured->SetStringField(TEXT("compile_diagnostics_status"), TEXT("not_attempted_dry_run"));
					OutStructured->SetBoolField(TEXT("compile_placeholder"), false);
					OutSummary = FString::Printf(TEXT("Validated blueprint_safe_patch dry run for %s (%d operation(s)); no write tools executed."),
						*AssetPath, Plan.Num());
					return true;
				}

				TSharedRef<FJsonObject> BatchResult = MakeShared<FJsonObject>();
				FString BatchSummary;
				FString BatchError;
				const bool bPatchOk = ExecuteBatch(Registry, Arguments, TEXT("blueprint_"), BatchResult, BatchSummary, BatchError);
				OutStructured->SetBoolField(TEXT("applied"), bPatchOk);
				OutStructured->SetObjectField(TEXT("patch_result"), BatchResult);
				OutStructured->SetStringField(TEXT("patch_summary"), BatchSummary);
				if (!BatchError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("patch_error"), BatchError);
				}

				if (!bPatchOk)
				{
					OutError = BatchError.IsEmpty() ? TEXT("blueprint_safe_patch apply failed.") : BatchError;
					OutSummary = BatchSummary;
					return false;
				}

				TSharedRef<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
				CompileArgs->SetStringField(TEXT("asset_path"), AssetPath);
				TSharedRef<FJsonObject> CompileResult = MakeShared<FJsonObject>();
				FString CompileSummary;
				FString CompileError;
				const bool bCompileOk = Registry.ExecuteTool(TEXT("blueprint_compile"), CompileArgs, CompileResult, CompileSummary, CompileError);
				OutStructured->SetObjectField(TEXT("compile"), CompileResult);
				OutStructured->SetStringField(TEXT("compile_summary"), CompileSummary);
				if (!CompileError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("compile_error"), CompileError);
				}

				if (!bCompileOk)
				{
					if (CompileError.StartsWith(TEXT("Unknown tool:")))
					{
						OutStructured->SetStringField(TEXT("compile_status"), TEXT("placeholder_compile_tool_unavailable"));
						OutStructured->SetStringField(TEXT("compile_diagnostics_status"), TEXT("compile_tool_unavailable"));
						OutStructured->SetBoolField(TEXT("compile_placeholder"), true);
						OutStructured->SetStringField(TEXT("compile_placeholder_note"), TEXT("Patch was applied, but no blueprint_compile tool was registered, so no compile diagnostics were produced."));
						OutSummary = FString::Printf(TEXT("Applied blueprint_safe_patch for %s; compile tool unavailable placeholder returned."), *AssetPath);
						return true;
					}

					OutStructured->SetStringField(TEXT("compile_status"), TEXT("failed"));
					OutStructured->SetStringField(TEXT("compile_diagnostics_status"), TEXT("failed"));
					OutStructured->SetBoolField(TEXT("compile_placeholder"), false);
					OutError = CompileError.IsEmpty() ? TEXT("blueprint_safe_patch apply succeeded but compile failed.") : CompileError;
					OutSummary = FString::Printf(TEXT("Applied blueprint_safe_patch for %s, but compile failed."), *AssetPath);
					return false;
				}

				OutStructured->SetStringField(TEXT("compile_status"), TEXT("attempted"));
				OutStructured->SetStringField(TEXT("compile_diagnostics_status"), TEXT("attempted"));
				OutStructured->SetBoolField(TEXT("compile_placeholder"), false);
				OutSummary = FString::Printf(TEXT("Applied blueprint_safe_patch for %s (%d operation(s)) and compiled."), *AssetPath, Plan.Num());
				return true;
			}
		, nullptr
		, 0
		});

		// ----------------------------------------------------------------------
		// 7) behaviortree_batch_edit
		// ----------------------------------------------------------------------
		Registry.Register({
			TEXT("behaviortree_batch_edit"),
			TEXT("Run multiple behaviortree_* operations in sequence. Each entry: {tool, arguments}. Tools must start with 'behaviortree_'."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional context hint; not enforced."))},
					{TEXT("operations"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("tool"), FSololmcpSchemaBuilder::String()},
						{TEXT("arguments"), FSololmcpSchemaBuilder::Object({})}
					}, {TEXT("tool"), TEXT("arguments")}))},
					{TEXT("continue_on_error"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("operations")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return ExecuteBatch(Registry, Arguments, TEXT("behaviortree_"), OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 0
		});
	}

} // namespace UE::SOMOLMCP

#undef LOCTEXT_NAMESPACE
