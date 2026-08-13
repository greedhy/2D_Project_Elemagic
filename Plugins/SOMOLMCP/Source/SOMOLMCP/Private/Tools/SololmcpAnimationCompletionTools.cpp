// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Native C++ P3 animation completion surface. No Python execution is used.

#include "Tools/SololmcpAnimationCompletionTools.h"

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/BlendProfile.h"
#include "AnimationBlueprintLibrary.h"
#include "AnimationEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraph/RigVMEdGraph.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Crc.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
// FC upgrade 2026-08-05: real AnimBP/RigVM native executors for the
// previously fail-closed animation completion tools. AnimGraph editor headers
// are safe (AnimGraph/AnimationBlueprintEditor deps), UAF/AnimNext assets are
// reached only through URigVMBlueprint (engine base class) and reflection.
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "RigVMCore/RigVM.h"
// URigVMBlueprint is used unconditionally below. UE 5.7 renamed the header that
// defines it from RigVMBlueprint.h to RigVMBlueprintLegacy.h, and on 5.4-5.6 it
// also arrived transitively through RigVMEditorBlueprintLibrary.h. 5.3 has neither
// route, so the definition is included explicitly on both sides of the rename
// rather than relied on second-hand.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "RigVMBlueprintLegacy.h"
#else
#include "RigVMBlueprint.h"
#endif
// RigVMEditorBlueprintLibrary first shipped in UE 5.4. On 5.3 the same controller
// is reachable as a member of URigVMBlueprint, so the capability is not lost.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#define SOMOLMCP_HAS_RIGVM_EDITOR_BP_LIBRARY 1
#include "RigVMEditorBlueprintLibrary.h"
#else
#define SOMOLMCP_HAS_RIGVM_EDITOR_BP_LIBRARY 0
#endif

namespace UE::SOMOLMCP
{
namespace AnimationCompletion
{
	/** Resolve the RigVM controller across the 5.3 / 5.4+ API split. */
	inline URigVMController* ResolveRigVMController(URigVMBlueprint* RigBP)
	{
		if (RigBP == nullptr)
		{
			return nullptr;
		}
#if SOMOLMCP_HAS_RIGVM_EDITOR_BP_LIBRARY
		return URigVMEditorBlueprintLibrary::GetController(RigBP);
#else
		return RigBP->GetController();
#endif
	}

	enum class EAction : uint8
	{
		CreateAsset,
		BlendAdd,
		BlendUpdate,
		BlendRemove,
		BlendList,
		BlendSnap,
		BlendValidate,
		GraphNodeAdd,
		GraphNodeRemove,
		GraphConnect,
		SemanticFailClosed,
		RootMotionSet,
		RootMotionInspect,
		AdditiveConfigure,
		CompressionSet,
		CompressionApply,
		Resample,
		Crop,
		TimeStretch,
		DistanceCurveGenerate,
		DistanceCurveValidate,
		CompileReceipt
	};

	struct FSpec
	{
		const TCHAR* Name;
		const TCHAR* Family;
		const TCHAR* Description;
		EAction Action;
		const TCHAR* ExpectedClassNeedle;
		const TCHAR* RequiredModules;
		const TCHAR* DefaultAssetClass;
		const TCHAR* DefaultFactoryClass;
		bool bMutation;
		bool bCompile;
	};

	static TArray<FString> SplitCsv(const TCHAR* Csv)
	{
		TArray<FString> Values;
		FString(Csv ? Csv : TEXT("")).ParseIntoArray(Values, TEXT(","), true);
		for (FString& Value : Values)
		{
			Value.TrimStartAndEndInline();
		}
		Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
		return Values;
	}

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static FString FingerprintObject(const UObject* Object)
	{
		if (!Object)
		{
			return TEXT("null");
		}
		FString Material = Object->GetPathName() + TEXT("|") + Object->GetClass()->GetPathName();
		if (const UPackage* Package = Object->GetOutermost())
		{
			Material += FString::Printf(TEXT("|dirty=%d"), Package->IsDirty() ? 1 : 0);
		}
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Object->GetClass()); It && Count < 128; ++It, ++Count)
		{
			const FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
			{
				continue;
			}
			FString Value;
			Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, const_cast<UObject*>(Object), PPF_None);
			Material += TEXT("|") + Property->GetName() + TEXT("=") + Value;
		}
		return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Material));
	}

	static TSharedRef<FJsonObject> ObjectReadback(const UObject* Object)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Object)
		{
			Result->SetBoolField(TEXT("valid"), false);
			return Result;
		}
		Result->SetBoolField(TEXT("valid"), true);
		Result->SetStringField(TEXT("object_path"), Object->GetPathName());
		Result->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
		Result->SetStringField(TEXT("fingerprint"), FingerprintObject(Object));
		Result->SetBoolField(TEXT("package_dirty"), Object->GetOutermost() && Object->GetOutermost()->IsDirty());
		return Result;
	}

	static UObject* FindNestedObject(UObject* Asset, const FString& Selector)
	{
		if (!Asset || Selector.IsEmpty())
		{
			return Asset;
		}
		TArray<UObject*> Objects;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		GetObjectsWithOuter(Asset, Objects, EGetObjectsFlags::IncludeNestedObjects);
#else
		GetObjectsWithOuter(Asset, Objects, /*bIncludeNestedObjects=*/true);
#endif
		for (UObject* Object : Objects)
		{
			if (Object && (Object->GetName().Equals(Selector, ESearchCase::IgnoreCase) ||
				Object->GetPathName().Equals(Selector, ESearchCase::IgnoreCase) ||
				Object->GetPathName().EndsWith(TEXT(".") + Selector)))
			{
				return Object;
			}
		}
		return nullptr;
	}

	static UEdGraph* FindGraph(UObject* Asset, const FString& GraphName)
	{
		if (UEdGraph* Direct = Cast<UEdGraph>(FindNestedObject(Asset, GraphName)))
		{
			return Direct;
		}
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph && (GraphName.IsEmpty() || Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase)))
				{
					return Graph;
				}
			}
		}
		return nullptr;
	}

	static UEdGraphNode* FindGraphNode(UEdGraph* Graph, const FString& Selector)
	{
		if (!Graph || Selector.IsEmpty())
		{
			return nullptr;
		}
		FGuid Guid;
		const bool bGuid = FGuid::Parse(Selector, Guid);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && ((bGuid && Node->NodeGuid == Guid) || Node->GetName().Equals(Selector, ESearchCase::IgnoreCase)))
			{
				return Node;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static const TCHAR* RequiredAnimGraphNodeClassNeedle(const FSpec& Spec)
	{
		const FString Name(Spec.Name);
		if (Name == TEXT("animbp_cached_pose_add_native")) return TEXT("AnimGraphNode_SaveCachedPose");
		if (Name == TEXT("animbp_linked_graph_add_native")) return TEXT("AnimGraphNode_LinkedAnimGraph");
		if (Name == TEXT("animbp_linked_layer_add_native")) return TEXT("AnimGraphNode_LinkedAnimLayer");
		if (Name == TEXT("animbp_state_alias_add_native")) return TEXT("AnimStateAliasNode");
		return nullptr;
	}

	static bool CompileAsset(UObject* Asset, TSharedRef<FJsonObject>& Receipt, FString& Error)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			FCompilerResultsLog CompileLog;
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileLog);
			const bool bTerminalSuccessStatus = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
			const bool bOk = bTerminalSuccessStatus && CompileLog.NumErrors == 0;
			Receipt->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()
				? StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status))
				: FString::FromInt(static_cast<int32>(Blueprint->Status)));
			Receipt->SetNumberField(TEXT("compile_error_count"), CompileLog.NumErrors);
			Receipt->SetNumberField(TEXT("compile_warning_count"), CompileLog.NumWarnings);
			Receipt->SetStringField(TEXT("compile_result_source"), TEXT("blueprint_status_and_compiler_log"));
			Receipt->SetBoolField(TEXT("compile_succeeded"), bOk);
			if (!bOk)
			{
				Error = FString::Printf(
					TEXT("Blueprint compilation failed (status=%d, errors=%d, warnings=%d)."),
					static_cast<int32>(Blueprint->Status), CompileLog.NumErrors, CompileLog.NumWarnings);
			}
			return bOk;
		}

		for (const FName FunctionName : {FName(TEXT("Compile")), FName(TEXT("CompileVM")), FName(TEXT("RecompileVM"))})
		{
			if (UFunction* Function = Asset ? Asset->FindFunction(FunctionName) : nullptr)
			{
				bool bHasInputParameters = false;
				FBoolProperty* ResultProperty = nullptr;
				FProperty* DiagnosticProperty = nullptr;
				for (TFieldIterator<FProperty> It(Function); It; ++It)
				{
					FProperty* Property = *It;
					if (!Property->HasAnyPropertyFlags(CPF_Parm))
					{
						continue;
					}
					const bool bOutput = Property->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm);
					bHasInputParameters |= !bOutput;
					if (bOutput && !ResultProperty)
					{
						ResultProperty = CastField<FBoolProperty>(Property);
					}
					if (bOutput && !DiagnosticProperty &&
						(Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FTextProperty>()))
					{
						DiagnosticProperty = Property;
					}
				}
				if (!bHasInputParameters && ResultProperty)
				{
					FStructOnScope Parameters(Function);
					Asset->ProcessEvent(Function, Parameters.GetStructMemory());
					const bool bOk = ResultProperty->GetPropertyValue_InContainer(Parameters.GetStructMemory());
					Receipt->SetStringField(TEXT("compile_entry"), FunctionName.ToString());
					Receipt->SetStringField(TEXT("compile_result_source"), ResultProperty->GetName());
					Receipt->SetStringField(TEXT("compile_status"), bOk ? TEXT("native_result_succeeded") : TEXT("native_result_failed"));
					Receipt->SetBoolField(TEXT("compile_succeeded"), bOk);
					if (DiagnosticProperty)
					{
						FString Diagnostic;
						DiagnosticProperty->ExportTextItem_Direct(
							Diagnostic,
							DiagnosticProperty->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory()),
							nullptr, Asset, PPF_None);
						Receipt->SetStringField(TEXT("compile_diagnostic"), Diagnostic);
					}
					if (!bOk)
					{
						Error = FString::Printf(TEXT("%s returned a negative native compile result."), *FunctionName.ToString());
					}
					return bOk;
				}
			}
		}

		Receipt->SetStringField(TEXT("compile_status"), TEXT("blocked_no_verifiable_native_compile_result"));
		Receipt->SetBoolField(TEXT("compile_succeeded"), false);
		Error = TEXT("The target asset exposes neither Blueprint compiler diagnostics nor a parameter-free native Compile/CompileVM/RecompileVM entry with a boolean result; compile is fail-closed.");
		return false;
	}

	static bool RequireModules(const FSpec& Spec, TSharedRef<FJsonObject>& Receipt, FString& Error)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		bool bReady = true;
		for (const FString& ModuleName : SplitCsv(Spec.RequiredModules))
		{
			FString ModulePath;
			const bool bExists = ModuleExistsCompat(*ModuleName, &ModulePath);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("module"), ModuleName);
			Row->SetBoolField(TEXT("exists"), bExists);
			Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(FName(*ModuleName)));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			bReady &= bExists;
		}
		Receipt->SetArrayField(TEXT("module_gate"), Rows);
		Receipt->SetBoolField(TEXT("module_gate_passed"), bReady);
		if (!bReady)
		{
			Error = FString::Printf(TEXT("%s is unavailable because one or more required native modules are missing."), Spec.Name);
		}
		return bReady;
	}

	static bool RequireClass(const FSpec& Spec, UObject* Asset, FString& Error)
	{
		if (!Asset)
		{
			Error = TEXT("Target asset did not load.");
			return false;
		}
		const FString Needle = Spec.ExpectedClassNeedle ? Spec.ExpectedClassNeedle : TEXT("");
		if (!Needle.IsEmpty() && !Asset->GetClass()->GetPathName().Contains(Needle, ESearchCase::IgnoreCase))
		{
			Error = FString::Printf(TEXT("Expected an asset class containing '%s', got '%s'."), *Needle, *Asset->GetClass()->GetPathName());
			return false;
		}
		return true;
	}

	static bool SaveIfRequested(
		const FSololmcpToolExecutionContext& Context,
		UObject* Asset,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		bool bSave = true;
		Args->TryGetBoolField(TEXT("save"), bSave);
		Receipt->SetBoolField(TEXT("save_requested"), bSave);
		if (!bSave)
		{
			Receipt->SetBoolField(TEXT("saved"), false);
			return true;
		}
		if (!Asset || !Context.Services.SaveAsset(Asset->GetPathName(), false, Error))
		{
			return false;
		}
		Receipt->SetBoolField(TEXT("saved"), true);
		return true;
	}

	static void AddBlendSamples(const UBlendSpace* BlendSpace, TSharedRef<FJsonObject>& Receipt)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		if (BlendSpace)
		{
			const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				const FBlendSample& Sample = Samples[Index];
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);
				Row->SetStringField(TEXT("animation_path"), Sample.Animation ? Sample.Animation->GetPathName() : FString());
				TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetNumberField(TEXT("x"), Sample.SampleValue.X);
				Value->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
				Value->SetNumberField(TEXT("z"), Sample.SampleValue.Z);
				Row->SetObjectField(TEXT("sample_value"), Value);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		Receipt->SetArrayField(TEXT("samples"), Rows);
		Receipt->SetNumberField(TEXT("sample_count"), Rows.Num());
	}

	static bool ExecuteBlendSpace(
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		UBlendSpace* BlendSpace,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		const int32 BeforeCount = BlendSpace->GetBlendSamples().Num();
		if (Spec.Action == EAction::BlendList)
		{
			AddBlendSamples(BlendSpace, Receipt);
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		if (Spec.Action == EAction::BlendValidate)
		{
			TArray<FString> Issues;
			const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				if (!Samples[Index].Animation)
				{
					Issues.Add(FString::Printf(TEXT("sample_%d_has_no_animation"), Index));
				}
				if (Samples[Index].SampleValue.ContainsNaN())
				{
					Issues.Add(FString::Printf(TEXT("sample_%d_has_non_finite_value"), Index));
				}
				for (int32 Other = Index + 1; Other < Samples.Num(); ++Other)
				{
					if (Samples[Index].SampleValue.Equals(Samples[Other].SampleValue, KINDA_SMALL_NUMBER))
					{
						Issues.Add(FString::Printf(TEXT("samples_%d_and_%d_overlap"), Index, Other));
					}
				}
			}
			Receipt->SetArrayField(TEXT("issues"), StringsToJson(Issues));
			Receipt->SetBoolField(TEXT("verified"), Issues.IsEmpty());
			AddBlendSamples(BlendSpace, Receipt);
			if (!Issues.IsEmpty())
			{
				Error = TEXT("BlendSpace validation failed; see receipt.issues.");
				return false;
			}
			return true;
		}

		BlendSpace->Modify();
		if (Spec.Action == EAction::BlendAdd || Spec.Action == EAction::BlendUpdate)
		{
			const TSharedPtr<FJsonObject>* SampleJson = nullptr;
			FVector SampleValue;
			if (!Args->TryGetObjectField(TEXT("sample_value"), SampleJson) || !SampleJson ||
				!FSololmcpEditorServices::JsonToVector(*SampleJson, SampleValue) || SampleValue.ContainsNaN())
			{
				Error = TEXT("sample_value {x,y,z} is required and must be finite.");
				return false;
			}
			if (Spec.Action == EAction::BlendAdd)
			{
				FString AnimationPath;
				if (!Args->TryGetStringField(TEXT("source_asset_path"), AnimationPath) || AnimationPath.IsEmpty())
				{
					Error = TEXT("source_asset_path is required for BlendSpace sample creation.");
					return false;
				}
				FString LoadError;
				UAnimSequence* Sequence = Cast<UAnimSequence>(Context.Services.LoadAsset(AnimationPath, LoadError));
				if (!Sequence)
				{
					Error = TEXT("source_asset_path must resolve to UAnimSequence: ") + LoadError;
					return false;
				}
				const int32 AddedIndex = BlendSpace->AddSample(Sequence, SampleValue);
				if (!BlendSpace->GetBlendSamples().IsValidIndex(AddedIndex))
				{
					Error = TEXT("UBlendSpace::AddSample did not return a valid sample index.");
					return false;
				}
				Receipt->SetNumberField(TEXT("sample_index"), AddedIndex);
			}
			else
			{
				int32 Index = INDEX_NONE;
				Args->TryGetNumberField(TEXT("sample_index"), Index);
				if (!BlendSpace->GetBlendSamples().IsValidIndex(Index) || !BlendSpace->EditSampleValue(Index, SampleValue))
				{
					Error = TEXT("sample_index is invalid or UBlendSpace::EditSampleValue rejected the value.");
					return false;
				}
				Receipt->SetNumberField(TEXT("sample_index"), Index);
			}
		}
		else if (Spec.Action == EAction::BlendRemove)
		{
			int32 Index = INDEX_NONE;
			Args->TryGetNumberField(TEXT("sample_index"), Index);
			if (!BlendSpace->GetBlendSamples().IsValidIndex(Index) || !BlendSpace->DeleteSample(Index))
			{
				Error = TEXT("sample_index is invalid or UBlendSpace::DeleteSample failed.");
				return false;
			}
		}
		else if (Spec.Action == EAction::BlendSnap)
		{
			const int32 Dimensions = BlendSpace->IsA<UBlendSpace1D>() ? 1 : 2;
			for (int32 SampleIndex = 0; SampleIndex < BlendSpace->GetBlendSamples().Num(); ++SampleIndex)
			{
				FVector Snapped = BlendSpace->GetBlendSample(SampleIndex).SampleValue;
				for (int32 Dimension = 0; Dimension < Dimensions; ++Dimension)
				{
					const FBlendParameter& Parameter = BlendSpace->GetBlendParameter(Dimension);
					const int32 GridNum = FMath::Max(1, Parameter.GridNum);
					const float Step = (Parameter.Max - Parameter.Min) / static_cast<float>(GridNum);
					Snapped[Dimension] = Step > UE_SMALL_NUMBER
						? Parameter.Min + FMath::RoundToFloat((Snapped[Dimension] - Parameter.Min) / Step) * Step
						: Parameter.Min;
					Snapped[Dimension] = FMath::Clamp(Snapped[Dimension], Parameter.Min, Parameter.Max);
				}
				if (!BlendSpace->EditSampleValue(SampleIndex, Snapped))
				{
					Error = FString::Printf(TEXT("BlendSpace grid snap rejected sample %d, usually because the target grid point is occupied."), SampleIndex);
					return false;
				}
			}
		}

		BlendSpace->PostEditChange();
		BlendSpace->MarkPackageDirty();
		AddBlendSamples(BlendSpace, Receipt);
		const int32 AfterCount = BlendSpace->GetBlendSamples().Num();
		Receipt->SetNumberField(TEXT("sample_count_before"), BeforeCount);
		Receipt->SetNumberField(TEXT("sample_count_after"), AfterCount);
		const bool bCountVerified = Spec.Action == EAction::BlendAdd ? AfterCount == BeforeCount + 1
			: Spec.Action == EAction::BlendRemove ? AfterCount == BeforeCount - 1 : AfterCount == BeforeCount;
		Receipt->SetBoolField(TEXT("verified"), bCountVerified);
		if (!bCountVerified)
		{
			Error = TEXT("BlendSpace post-write sample-count readback did not match the requested operation.");
			return false;
		}
		return SaveIfRequested(Context, BlendSpace, Args, Receipt, Error);
	}

	static bool ExecuteAnimSequence(
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		UAnimSequence* Sequence,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		if (Spec.Action == EAction::RootMotionInspect)
		{
			TEnumAsByte<ERootMotionRootLock::Type> LockType;
			UAnimationBlueprintLibrary::GetRootMotionLockType(Sequence, LockType);
			Receipt->SetBoolField(TEXT("root_motion_enabled"), UAnimationBlueprintLibrary::IsRootMotionEnabled(Sequence));
			Receipt->SetBoolField(TEXT("force_root_lock"), UAnimationBlueprintLibrary::IsRootMotionLockForced(Sequence));
			Receipt->SetNumberField(TEXT("root_lock_type"), static_cast<int32>(LockType.GetValue()));
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		Sequence->Modify();
		if (Spec.Action == EAction::RootMotionSet)
		{
			bool bEnabled = false;
			if (!Args->TryGetBoolField(TEXT("enabled"), bEnabled))
			{
				Error = TEXT("enabled is required.");
				return false;
			}
			bool bForceLock = false;
			Args->TryGetBoolField(TEXT("force_root_lock"), bForceLock);
			int32 LockType = static_cast<int32>(ERootMotionRootLock::RefPose);
			Args->TryGetNumberField(TEXT("root_lock_type"), LockType);
			if (LockType < static_cast<int32>(ERootMotionRootLock::RefPose) ||
				LockType > static_cast<int32>(ERootMotionRootLock::Zero))
			{
				Error = TEXT("root_lock_type is outside ERootMotionRootLock range.");
				return false;
			}
			UAnimationBlueprintLibrary::SetRootMotionEnabled(Sequence, bEnabled);
			UAnimationBlueprintLibrary::SetIsRootMotionLockForced(Sequence, bForceLock);
			UAnimationBlueprintLibrary::SetRootMotionLockType(Sequence, static_cast<ERootMotionRootLock::Type>(LockType));
			const bool bVerified = UAnimationBlueprintLibrary::IsRootMotionEnabled(Sequence) == bEnabled &&
				UAnimationBlueprintLibrary::IsRootMotionLockForced(Sequence) == bForceLock;
			Receipt->SetBoolField(TEXT("verified"), bVerified);
			if (!bVerified)
			{
				Error = TEXT("Root-motion write did not survive immediate readback.");
				return false;
			}
		}
		else if (Spec.Action == EAction::AdditiveConfigure)
		{
			int32 AdditiveType = INDEX_NONE;
			int32 BasePoseType = INDEX_NONE;
			if (!Args->TryGetNumberField(TEXT("additive_type"), AdditiveType) ||
				!Args->TryGetNumberField(TEXT("base_pose_type"), BasePoseType))
			{
				Error = TEXT("additive_type and base_pose_type are required enum integers.");
				return false;
			}
			UAnimationBlueprintLibrary::SetAdditiveAnimationType(Sequence, static_cast<EAdditiveAnimationType>(AdditiveType));
			UAnimationBlueprintLibrary::SetAdditiveBasePoseType(Sequence, static_cast<EAdditiveBasePoseType>(BasePoseType));
			TEnumAsByte<EAdditiveAnimationType> ActualAdditive;
			TEnumAsByte<EAdditiveBasePoseType> ActualBase;
			UAnimationBlueprintLibrary::GetAdditiveAnimationType(Sequence, ActualAdditive);
			UAnimationBlueprintLibrary::GetAdditiveBasePoseType(Sequence, ActualBase);
			const bool bVerified = ActualAdditive.GetValue() == AdditiveType && ActualBase.GetValue() == BasePoseType;
			Receipt->SetNumberField(TEXT("additive_type"), static_cast<int32>(ActualAdditive.GetValue()));
			Receipt->SetNumberField(TEXT("base_pose_type"), static_cast<int32>(ActualBase.GetValue()));
			Receipt->SetBoolField(TEXT("verified"), bVerified);
			if (!bVerified)
			{
				Error = TEXT("Additive settings did not survive immediate readback.");
				return false;
			}
		}
		else if (Spec.Action == EAction::CompressionSet)
		{
			FString BonePath;
			FString CurvePath;
			Args->TryGetStringField(TEXT("bone_compression_settings_path"), BonePath);
			Args->TryGetStringField(TEXT("curve_compression_settings_path"), CurvePath);
			if (BonePath.IsEmpty() && CurvePath.IsEmpty())
			{
				Error = TEXT("At least one compression settings path is required.");
				return false;
			}
			if (!BonePath.IsEmpty())
			{
				UAnimBoneCompressionSettings* Settings = Cast<UAnimBoneCompressionSettings>(Context.Services.LoadAsset(BonePath, Error));
				if (!Settings) return false;
				UAnimationBlueprintLibrary::SetBoneCompressionSettings(Sequence, Settings);
			}
			if (!CurvePath.IsEmpty())
			{
				UAnimCurveCompressionSettings* Settings = Cast<UAnimCurveCompressionSettings>(Context.Services.LoadAsset(CurvePath, Error));
				if (!Settings) return false;
				UAnimationBlueprintLibrary::SetCurveCompressionSettings(Sequence, Settings);
			}
			UAnimBoneCompressionSettings* ActualBone = nullptr;
			UAnimCurveCompressionSettings* ActualCurve = nullptr;
			UAnimationBlueprintLibrary::GetBoneCompressionSettings(Sequence, ActualBone);
			UAnimationBlueprintLibrary::GetCurveCompressionSettings(Sequence, ActualCurve);
			Receipt->SetStringField(TEXT("bone_compression_settings"), ActualBone ? ActualBone->GetPathName() : FString());
			Receipt->SetStringField(TEXT("curve_compression_settings"), ActualCurve ? ActualCurve->GetPathName() : FString());
			Receipt->SetBoolField(TEXT("verified"), (BonePath.IsEmpty() || ActualBone) && (CurvePath.IsEmpty() || ActualCurve));
		}
		else if (Spec.Action == EAction::CompressionApply)
		{
			UAnimBoneCompressionSettings* Override = nullptr;
			FString OverridePath;
			if (Args->TryGetStringField(TEXT("bone_compression_settings_path"), OverridePath) && !OverridePath.IsEmpty())
			{
				Override = Cast<UAnimBoneCompressionSettings>(Context.Services.LoadAsset(OverridePath, Error));
				if (!Override) return false;
			}
			TArray<UAnimSequence*> Sequences{Sequence};
			if (!AnimationEditorUtils::ApplyCompressionAlgorithm(Sequences, Override))
			{
				Error = TEXT("AnimationEditorUtils::ApplyCompressionAlgorithm failed.");
				return false;
			}
			Receipt->SetBoolField(TEXT("verified"), true);
		}
		else if (Spec.Action == EAction::Resample)
		{
			int32 Numerator = 0;
			int32 Denominator = 1;
			if (!Args->TryGetNumberField(TEXT("frame_rate_numerator"), Numerator) || Numerator <= 0)
			{
				Error = TEXT("frame_rate_numerator must be greater than zero.");
				return false;
			}
			Args->TryGetNumberField(TEXT("frame_rate_denominator"), Denominator);
			if (Denominator <= 0)
			{
				Error = TEXT("frame_rate_denominator must be greater than zero.");
				return false;
			}
			Sequence->GetController().SetFrameRate(FFrameRate(Numerator, Denominator));
			const FFrameRate Actual = Sequence->GetDataModel()->GetFrameRate();
			const bool bVerified = Actual.Numerator == Numerator && Actual.Denominator == Denominator;
			Receipt->SetNumberField(TEXT("frame_rate_numerator"), Actual.Numerator);
			Receipt->SetNumberField(TEXT("frame_rate_denominator"), Actual.Denominator);
			Receipt->SetBoolField(TEXT("verified"), bVerified);
			if (!bVerified)
			{
				Error = TEXT("Animation frame-rate write did not survive readback.");
				return false;
			}
		}
		else if (Spec.Action == EAction::Crop)
		{
			int32 StartFrame = INDEX_NONE;
			int32 EndFrame = INDEX_NONE;
			if (!Args->TryGetNumberField(TEXT("start_frame"), StartFrame) || !Args->TryGetNumberField(TEXT("end_frame"), EndFrame) ||
				StartFrame < 0 || EndFrame <= StartFrame || EndFrame > Sequence->GetDataModel()->GetNumberOfFrames())
			{
				Error = TEXT("A valid [start_frame,end_frame] range is required.");
				return false;
			}
			const int32 NewFrameCount = EndFrame - StartFrame;
			Sequence->GetController().ResizeInFrames(FFrameNumber(NewFrameCount), FFrameNumber(StartFrame), FFrameNumber(EndFrame));
			const int32 Actual = Sequence->GetDataModel()->GetNumberOfFrames();
			Receipt->SetNumberField(TEXT("frame_count"), Actual);
			Receipt->SetBoolField(TEXT("verified"), Actual == NewFrameCount);
			if (Actual != NewFrameCount)
			{
				Error = TEXT("Animation crop frame-count readback did not match.");
				return false;
			}
		}
		else if (Spec.Action == EAction::TimeStretch)
		{
			double RateScale = 0.0;
			if (!Args->TryGetNumberField(TEXT("rate_scale"), RateScale) || !FMath::IsFinite(RateScale) || RateScale <= 0.0)
			{
				Error = TEXT("rate_scale must be finite and greater than zero.");
				return false;
			}
			UAnimationBlueprintLibrary::SetRateScale(Sequence, static_cast<float>(RateScale));
			float Actual = 0.0f;
			UAnimationBlueprintLibrary::GetRateScale(Sequence, Actual);
			Receipt->SetNumberField(TEXT("rate_scale"), Actual);
			Receipt->SetBoolField(TEXT("verified"), FMath::IsNearlyEqual(Actual, static_cast<float>(RateScale)));
			if (!FMath::IsNearlyEqual(Actual, static_cast<float>(RateScale)))
			{
				Error = TEXT("Animation rate-scale write did not survive readback.");
				return false;
			}
		}
		else if (Spec.Action == EAction::DistanceCurveGenerate)
		{
			FString CurveNameText = TEXT("Distance");
			Args->TryGetStringField(TEXT("curve_name"), CurveNameText);
			const TArray<TSharedPtr<FJsonValue>>* TimesJson = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* ValuesJson = nullptr;
			if (!Args->TryGetArrayField(TEXT("times"), TimesJson) || !Args->TryGetArrayField(TEXT("values"), ValuesJson) ||
				!TimesJson || !ValuesJson || TimesJson->Num() < 2 || TimesJson->Num() != ValuesJson->Num())
			{
				Error = TEXT("times and values arrays of equal length (minimum two) are required.");
				return false;
			}
			TArray<float> Times;
			TArray<float> Values;
			for (int32 Index = 0; Index < TimesJson->Num(); ++Index)
			{
				Times.Add(static_cast<float>((*TimesJson)[Index]->AsNumber()));
				Values.Add(static_cast<float>((*ValuesJson)[Index]->AsNumber()));
				if (!FMath::IsFinite(Times.Last()) || !FMath::IsFinite(Values.Last()) || (Index > 0 && Times[Index] <= Times[Index - 1]))
				{
					Error = TEXT("Distance curve times must be finite and strictly increasing; values must be finite.");
					return false;
				}
			}
			const FName CurveName(*CurveNameText);
			if (!UAnimationBlueprintLibrary::DoesCurveExist(Sequence, CurveName, ERawCurveTrackTypes::RCT_Float))
			{
				UAnimationBlueprintLibrary::AddCurve(Sequence, CurveName, ERawCurveTrackTypes::RCT_Float, false);
			}
			UAnimationBlueprintLibrary::AddFloatCurveKeys(Sequence, CurveName, Times, Values);
			TArray<float> ActualTimes;
			TArray<float> ActualValues;
			UAnimationBlueprintLibrary::GetFloatKeys(Sequence, CurveName, ActualTimes, ActualValues);
			Receipt->SetStringField(TEXT("curve_name"), CurveNameText);
			Receipt->SetNumberField(TEXT("key_count"), ActualTimes.Num());
			Receipt->SetBoolField(TEXT("verified"), ActualTimes.Num() >= Times.Num());
			if (ActualTimes.Num() < Times.Num())
			{
				Error = TEXT("Distance curve key readback count is smaller than the requested key count.");
				return false;
			}
		}
		else if (Spec.Action == EAction::DistanceCurveValidate)
		{
			FString CurveNameText = TEXT("Distance");
			Args->TryGetStringField(TEXT("curve_name"), CurveNameText);
			TArray<float> Times;
			TArray<float> Values;
			UAnimationBlueprintLibrary::GetFloatKeys(Sequence, FName(*CurveNameText), Times, Values);
			bool bValid = Times.Num() >= 2 && Times.Num() == Values.Num();
			for (int32 Index = 1; Index < Times.Num(); ++Index)
			{
				bValid &= Times[Index] > Times[Index - 1] && FMath::IsFinite(Values[Index]);
			}
			Receipt->SetStringField(TEXT("curve_name"), CurveNameText);
			Receipt->SetNumberField(TEXT("key_count"), Times.Num());
			Receipt->SetBoolField(TEXT("verified"), bValid);
			if (!bValid)
			{
				Error = TEXT("Distance matching curve is missing, too short, or has invalid/non-increasing keys.");
				return false;
			}
			return true;
		}

		Sequence->PostEditChange();
		Sequence->MarkPackageDirty();
		return SaveIfRequested(Context, Sequence, Args, Receipt, Error);
	}

	static bool ExecuteGraphAction(
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		UObject* Asset,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		FString GraphName;
		Args->TryGetStringField(TEXT("graph_name"), GraphName);
		UEdGraph* Graph = FindGraph(Asset, GraphName);
		if (!Graph)
		{
			Error = TEXT("graph_name did not resolve to a writable UEdGraph inside the target asset.");
			return false;
		}

		if (FString(Spec.Family).Equals(TEXT("animnext"), ESearchCase::IgnoreCase))
		{
			URigVMEdGraph* RigVMEdGraph = Cast<URigVMEdGraph>(Graph);
			URigVMController* Controller = RigVMEdGraph ? RigVMEdGraph->GetController() : nullptr;
			URigVMGraph* Model = Controller ? Controller->GetGraph() : nullptr;
			if (!RigVMEdGraph || !Controller || !Model)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_native_controller_required"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("AnimNext/RigVM graph mutation requires a URigVMEdGraph with an available native URigVMController and model; raw UEdGraph mutation is prohibited.");
				return false;
			}

			Receipt->SetStringField(TEXT("controller_class"), Controller->GetClass()->GetPathName());
			Receipt->SetStringField(TEXT("model_path"), Model->GetPathName());
			if (Spec.Action == EAction::GraphNodeAdd)
			{
				FString StructPath;
				if (!Args->TryGetStringField(TEXT("unit_struct_path"), StructPath) || StructPath.IsEmpty())
				{
					Error = TEXT("unit_struct_path is required for an AnimNext RigVM unit node.");
					return false;
				}
				UScriptStruct* UnitStruct = FindObject<UScriptStruct>(nullptr, *StructPath);
				if (!UnitStruct)
				{
					UnitStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
				}
				if (!UnitStruct)
				{
					Error = TEXT("unit_struct_path did not resolve to a native UScriptStruct.");
					return false;
				}
				FString MethodName = TEXT("Execute");
				FString NodeName;
				double PositionX = 0.0;
				double PositionY = 0.0;
				Args->TryGetStringField(TEXT("method_name"), MethodName);
				Args->TryGetStringField(TEXT("node_name"), NodeName);
				Args->TryGetNumberField(TEXT("position_x"), PositionX);
				Args->TryGetNumberField(TEXT("position_y"), PositionY);
				if (MethodName.IsEmpty())
				{
					Error = TEXT("method_name must not be empty.");
					return false;
				}

				URigVMUnitNode* Node = Controller->AddUnitNode(
					UnitStruct,
					FName(*MethodName),
					FVector2D(static_cast<float>(PositionX), static_cast<float>(PositionY)),
					NodeName,
					true,
					false);
				const bool bVerified = Node && Model->FindNodeByName(Node->GetFName()) == Node;
				Receipt->SetBoolField(TEXT("verified"), bVerified);
				if (!bVerified)
				{
					Error = TEXT("URigVMController::AddUnitNode failed model readback.");
					return false;
				}
				Receipt->SetStringField(TEXT("node_id"), Node->GetFName().ToString());
				Receipt->SetStringField(TEXT("node_path"), Node->GetNodePath());
			}
			else if (Spec.Action == EAction::GraphConnect)
			{
				FString SourcePinPath;
				FString TargetPinPath;
				if (!Args->TryGetStringField(TEXT("source_pin_path"), SourcePinPath) || SourcePinPath.IsEmpty() ||
					!Args->TryGetStringField(TEXT("target_pin_path"), TargetPinPath) || TargetPinPath.IsEmpty())
				{
					Error = TEXT("source_pin_path and target_pin_path are required.");
					return false;
				}
				if (!Model->FindPin(SourcePinPath) || !Model->FindPin(TargetPinPath))
				{
					Error = TEXT("One or both RigVM pin paths did not resolve in the controller model.");
					return false;
				}
				if (!Controller->AddLink(SourcePinPath, TargetPinPath, true, false))
				{
					Error = TEXT("URigVMController::AddLink rejected the connection.");
					return false;
				}
				const bool bVerified = Model->GetLinks().ContainsByPredicate(
					[&SourcePinPath, &TargetPinPath](const URigVMLink* Link)
					{
						return Link && Link->GetSourcePinPath() == SourcePinPath && Link->GetTargetPinPath() == TargetPinPath;
					});
				Receipt->SetStringField(TEXT("source_pin_path"), SourcePinPath);
				Receipt->SetStringField(TEXT("target_pin_path"), TargetPinPath);
				Receipt->SetBoolField(TEXT("verified"), bVerified);
				if (!bVerified)
				{
					Error = TEXT("RigVM link creation did not survive immediate model readback.");
					return false;
				}
			}
			else
			{
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("This AnimNext graph action has no controller-backed implementation.");
				return false;
			}

			Asset->MarkPackageDirty();
			return SaveIfRequested(Context, Asset, Args, Receipt, Error);
		}

		if (Spec.Action == EAction::GraphNodeAdd)
		{
			const TCHAR* RequiredClassNeedle = RequiredAnimGraphNodeClassNeedle(Spec);
			if (!RequiredClassNeedle)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_missing_specialized_node_contract"));
				Error = FString::Printf(TEXT("%s has no proven specialized UEdGraph node contract and is fail-closed."), Spec.Name);
				return false;
			}
			FString NodeClassPath;
			if (!Args->TryGetStringField(TEXT("node_class_path"), NodeClassPath) || NodeClassPath.IsEmpty())
			{
				Error = TEXT("node_class_path is required for graph-node creation.");
				return false;
			}
			UClass* NodeClass = Context.Services.ResolveClass(NodeClassPath, Error);
			if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
			{
				Error = TEXT("node_class_path must resolve to a non-abstract UEdGraphNode class.");
				return false;
			}
			if (!NodeClass->GetPathName().Contains(RequiredClassNeedle, ESearchCase::IgnoreCase))
			{
				Error = FString::Printf(
					TEXT("node_class_path must resolve to the specialized %s node class for %s."),
					RequiredClassNeedle, Spec.Name);
				return false;
			}
			UEdGraph* TargetGraph = Graph;
			if (NodeClass->GetName().Contains(TEXT("AnimStateAlias")))
			{
				// State-alias nodes only live on a UAnimationStateMachineGraph; adding
				// them to the AnimGraph triggers an engine Cast check (0x4000 crash).
				if (!Graph->IsA<UAnimationStateMachineGraph>())
				{
					TargetGraph = nullptr;
					if (UBlueprint* GraphBlueprint = Cast<UBlueprint>(Asset))
					{
						TArray<UEdGraph*> AllGraphs;
						GraphBlueprint->GetAllGraphs(AllGraphs);
						for (UEdGraph* Candidate : AllGraphs)
						{
							if (Candidate && Candidate->IsA<UAnimationStateMachineGraph>())
							{
								TargetGraph = Candidate;
								break;
							}
						}
					}
					if (!TargetGraph)
					{
						Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_state_machine_graph"));
						Receipt->SetBoolField(TEXT("verified"), false);
						Error = TEXT("AnimStateAliasNode requires a UAnimationStateMachineGraph; none found in the target Blueprint. The tool is fail-closed.");
						return false;
					}
				}
			}
			TargetGraph->Modify();
			UEdGraphNode* Node = NewObject<UEdGraphNode>(TargetGraph, NodeClass, NAME_None, RF_Transactional);
			TargetGraph->AddNode(Node, true, false);
			Node->CreateNewGuid();
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			double PositionX = 0.0;
			double PositionY = 0.0;
			Args->TryGetNumberField(TEXT("position_x"), PositionX);
			Args->TryGetNumberField(TEXT("position_y"), PositionY);
			Node->NodePosX = FMath::RoundToInt(PositionX);
			Node->NodePosY = FMath::RoundToInt(PositionY);
			TargetGraph->NotifyGraphChanged();
			Receipt->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			Receipt->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
			Receipt->SetBoolField(TEXT("verified"), TargetGraph->Nodes.Contains(Node));
		}
		else if (Spec.Action == EAction::GraphNodeRemove)
		{
			FString NodeId;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			UEdGraphNode* Node = FindGraphNode(Graph, NodeId);
			if (!Node)
			{
				Error = TEXT("node_id did not resolve to a node in graph_name.");
				return false;
			}
			const FGuid RemovedGuid = Node->NodeGuid;
			Graph->Modify();
			Graph->RemoveNode(Node);
			Graph->NotifyGraphChanged();
			Receipt->SetStringField(TEXT("removed_node_id"), RemovedGuid.ToString());
			Receipt->SetBoolField(TEXT("verified"), FindGraphNode(Graph, RemovedGuid.ToString()) == nullptr);
		}
		else
		{
			FString SourceNodeId;
			FString SourcePinName;
			FString TargetNodeId;
			FString TargetPinName;
			Args->TryGetStringField(TEXT("source_node_id"), SourceNodeId);
			Args->TryGetStringField(TEXT("source_pin"), SourcePinName);
			Args->TryGetStringField(TEXT("target_node_id"), TargetNodeId);
			Args->TryGetStringField(TEXT("target_pin"), TargetPinName);
			UEdGraphPin* SourcePin = FindPin(FindGraphNode(Graph, SourceNodeId), SourcePinName);
			UEdGraphPin* TargetPin = FindPin(FindGraphNode(Graph, TargetNodeId), TargetPinName);
			const UEdGraphSchema* Schema = Graph->GetSchema();
			if (!SourcePin || !TargetPin || !Schema || !Schema->TryCreateConnection(SourcePin, TargetPin))
			{
				Error = TEXT("Graph pin connection failed schema validation.");
				return false;
			}
			Receipt->SetBoolField(TEXT("verified"), SourcePin->LinkedTo.Contains(TargetPin) && TargetPin->LinkedTo.Contains(SourcePin));
		}

		Asset->MarkPackageDirty();
		if (Spec.bCompile && !CompileAsset(Asset, Receipt, Error))
		{
			return false;
		}
		return SaveIfRequested(Context, Asset, Args, Receipt, Error);
	}

	// ============================================================================
	// FC upgrade (2026-08-05): reflection helpers shared by the semantic native
	// executors. Every write is name-matched and fail-closed when the expected
	// editor property is absent on the current engine/plugin version.
	// ============================================================================

	static bool ReflectWriteStringAt(void* ContainerPtr, FProperty* Prop, const FString& Value, FString& Error)
	{
		if (const FStrProperty* Str = CastField<FStrProperty>(Prop))
		{
			const_cast<FStrProperty*>(Str)->SetPropertyValue_InContainer(ContainerPtr, Value);
			return true;
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(Prop))
		{
			const_cast<FNameProperty*>(Name)->SetPropertyValue_InContainer(ContainerPtr, FName(*Value));
			return true;
		}
		Error = FString::Printf(TEXT("Property %s is not a string/name property."), *Prop->GetName());
		return false;
	}

	static bool ReflectWriteBoolAt(void* ContainerPtr, FProperty* Prop, bool bValue, FString& Error)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Prop))
		{
			const_cast<FBoolProperty*>(Bool)->SetPropertyValue_InContainer(ContainerPtr, bValue);
			return true;
		}
		Error = FString::Printf(TEXT("Property %s is not a bool property."), *Prop->GetName());
		return false;
	}

	static bool ReflectWriteIntAt(void* ContainerPtr, FProperty* Prop, int64 Value, FString& Error)
	{
		if (const FIntProperty* Int = CastField<FIntProperty>(Prop))
		{
			const_cast<FIntProperty*>(Int)->SetPropertyValue_InContainer(ContainerPtr, static_cast<int32>(Value));
			return true;
		}
		if (const FByteProperty* Byte = CastField<FByteProperty>(Prop))
		{
			const_cast<FByteProperty*>(Byte)->SetPropertyValue_InContainer(ContainerPtr, static_cast<uint8>(Value));
			return true;
		}
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Prop))
		{
			if (FNumericProperty* Underlying = Enum->GetUnderlyingProperty())
			{
				void* EnumPtr = Enum->ContainerPtrToValuePtr<void>(ContainerPtr);
				Underlying->SetIntPropertyValue(EnumPtr, Value);
				return true;
			}
		}
		Error = FString::Printf(TEXT("Property %s is not an integer/enum property."), *Prop->GetName());
		return false;
	}

	static bool ReflectWriteObjectAt(void* ContainerPtr, FProperty* Prop, UObject* Object, FString& Error)
	{
		if (const FObjectProperty* Obj = CastField<FObjectProperty>(Prop))
		{
			const_cast<FObjectProperty*>(Obj)->SetObjectPropertyValue_InContainer(ContainerPtr, Object);
			return true;
		}
		Error = FString::Printf(TEXT("Property %s is not an object property."), *Prop->GetName());
		return false;
	}

	// Returns the first FStructProperty named Node or RuntimeNode on a graph node.
	static FStructProperty* FindNodeStructProperty(UObject* GraphNode)
	{
		if (!GraphNode)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(GraphNode->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FString PropName = It->GetName();
			if (PropName == TEXT("Node") || PropName == TEXT("RuntimeNode"))
			{
				if (FStructProperty* Struct = CastField<FStructProperty>(*It))
				{
					return Struct;
				}
			}
		}
		return nullptr;
	}

	// Sync group name/role live on the contained FAnimNode_* struct (GroupName /
	// GroupRole properties). UE 5.8 no longer ships AnimGraphNode_Sync.h, so the
	// write is reflection-based and fail-closed when the struct lacks the fields.
	static bool ReflectWriteSyncGroup(UObject* GraphNode, const FString& GroupName, int32 GroupRole, FString& Error)
	{
		FStructProperty* NodeProp = FindNodeStructProperty(GraphNode);
		if (!NodeProp || !NodeProp->Struct)
		{
			Error = TEXT("Node/RuntimeNode struct not found; sync group cannot be written.");
			return false;
		}
		void* NodePtr = NodeProp->ContainerPtrToValuePtr<void>(GraphNode);
		bool bWroteGroupName = false;
		bool bWroteRole = false;
		for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It)
		{
			FString InnerError;
			if (It->GetName() == TEXT("GroupName") && !bWroteGroupName)
			{
				bWroteGroupName = ReflectWriteStringAt(NodePtr, *It, GroupName, InnerError);
			}
			else if (It->GetName() == TEXT("GroupRole") && !bWroteRole)
			{
				bWroteRole = ReflectWriteIntAt(NodePtr, *It, GroupRole, InnerError);
			}
		}
		if (!bWroteGroupName)
		{
			Error = TEXT("GroupName property not found on node struct; sync group write fail-closed.");
			return false;
		}
		if (!bWroteRole)
		{
			Error = TEXT("GroupRole property not found on node struct; sync group write fail-closed.");
			return false;
		}
		return true;
	}

	static TSharedRef<FJsonObject> SyncGroupReadbackJson(UObject* GraphNode)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node"), GraphNode->GetPathName());
		Row->SetStringField(TEXT("class"), GraphNode->GetClass()->GetName());
		FStructProperty* NodeProp = FindNodeStructProperty(GraphNode);
		if (!NodeProp || !NodeProp->Struct)
		{
			Row->SetStringField(TEXT("sync_group_name"), TEXT(""));
			Row->SetStringField(TEXT("sync_group_role"), TEXT(""));
			return Row;
		}
		void* NodePtr = NodeProp->ContainerPtrToValuePtr<void>(GraphNode);
		for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It)
		{
			if (It->GetName() == TEXT("GroupName"))
			{
				if (const FNameProperty* Name = CastField<FNameProperty>(*It))
				{
					Row->SetStringField(TEXT("sync_group_name"), Name->GetPropertyValue_InContainer(NodePtr).ToString());
				}
				else if (const FStrProperty* Str = CastField<FStrProperty>(*It))
				{
					Row->SetStringField(TEXT("sync_group_name"), Str->GetPropertyValue_InContainer(NodePtr));
				}
			}
			else if (It->GetName() == TEXT("GroupRole"))
			{
				if (const FByteProperty* Byte = CastField<FByteProperty>(*It))
				{
					Row->SetNumberField(TEXT("sync_group_role"), Byte->GetPropertyValue_InContainer(NodePtr));
				}
				else if (const FEnumProperty* Enum = CastField<FEnumProperty>(*It))
				{
					if (const FNumericProperty* Under = Enum->GetUnderlyingProperty())
					{
						const void* EnumPtr = Enum->ContainerPtrToValuePtr<void>(NodePtr);
						Row->SetNumberField(TEXT("sync_group_role"), Under->GetSignedIntPropertyValue(EnumPtr));
					}
				}
			}
		}
		return Row;
	}

	// Resolves a graph node by id, optionally scoped to a named graph; iterates
	// every Blueprint graph when graph_name is empty.
	static UEdGraphNode* FindAnimGraphNodeAnyGraph(UBlueprint* Blueprint, const FString& NodeId, const FString& GraphName, FString& Error)
	{
		if (!Blueprint)
		{
			Error = TEXT("Asset is not a Blueprint.");
			return nullptr;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		if (!GraphName.IsEmpty())
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				{
					if (UEdGraphNode* Node = FindGraphNode(Graph, NodeId))
					{
						return Node;
					}
					Error = FString::Printf(TEXT("Node '%s' not found in graph '%s'."), *NodeId, *GraphName);
					return nullptr;
				}
			}
			Error = FString::Printf(TEXT("Graph '%s' not found."), *GraphName);
			return nullptr;
		}
		for (UEdGraph* Graph : Graphs)
		{
			if (UEdGraphNode* Node = FindGraphNode(Graph, NodeId))
			{
				return Node;
			}
		}
		Error = FString::Printf(TEXT("Node '%s' not found in any graph."), *NodeId);
		return nullptr;
	}

	// Renames a cached pose by updating CacheName on the save node and every use
	// node in the Blueprint (reflection-safe for Save/Use node pairs).
	static bool ReflectRenameCachedPose(UBlueprint* Blueprint, UEdGraphNode* TargetNode, const FString& NewName, int32& OutRenamedCount, FString& Error)
	{
		if (!TargetNode)
		{
			Error = TEXT("Cached pose node not found.");
			return false;
		}
		FProperty* CacheProp = nullptr;
		for (TFieldIterator<FProperty> It(TargetNode->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (It->GetName() == TEXT("CacheName"))
			{
				CacheProp = *It;
				break;
			}
		}
		if (!CacheProp)
		{
			Error = TEXT("Target node has no CacheName property.");
			return false;
		}
		FString InnerError;
		if (!ReflectWriteStringAt(TargetNode, CacheProp, NewName, InnerError))
		{
			Error = TEXT("CacheName write failed: ") + InnerError;
			return false;
		}
		OutRenamedCount = 1;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || Node == TargetNode)
				{
					continue;
				}
				for (TFieldIterator<FProperty> It(Node->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					if (It->GetName() != TEXT("CacheName"))
					{
						continue;
					}
					FString SyncError;
					if (ReflectWriteStringAt(Node, *It, NewName, SyncError))
					{
						++OutRenamedCount;
					}
					break;
				}
			}
		}
		return true;
	}

	// ============================================================================
	// FC upgrade (2026-08-05): name-specific native executors for the semantic
	// completion tools that were previously fail-closed. Each executor either
	// performs a verified write/readback with real engine APIs or reports a
	// reflection probe result and remains fail-closed — a fake success is never
	// emitted.
	// ============================================================================

	static FProperty* FindPropByName(UObject* Object, const TCHAR* PropName)
	{
		if (!Object)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (It->GetFName() == FName(PropName))
			{
				return *It;
			}
		}
		return nullptr;
	}

	static UObject* LoadToolAsset(const FSololmcpToolExecutionContext& Context, const FString& Path, FString& Error)
	{
		if (Path.IsEmpty())
		{
			Error = TEXT("Asset path is empty.");
			return nullptr;
		}
		return Context.Services.LoadAsset(Path, Error);
	}

	static bool ExecuteSemanticNative(
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		UObject* Asset,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		const FString Name(Spec.Name);
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);

		// ---- animbp: sync group --------------------------------------------------
		if (Name == TEXT("animbp_sync_group_set_native"))
		{
			FString NodeId;
			FString GroupName;
			int64 Role = 0;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("sync_group_name"), GroupName);
			Args->TryGetNumberField(TEXT("sync_group_role"), Role);
			UEdGraphNode* Node = FindAnimGraphNodeAnyGraph(Blueprint, NodeId, TEXT(""), Error);
			if (!Node)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_node_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			if (!ReflectWriteSyncGroup(Node, GroupName, static_cast<int32>(Role), Error))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_sync_group_write"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			const TSharedRef<FJsonObject> Row = SyncGroupReadbackJson(Node);
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetStringField(TEXT("node"), Row->GetStringField(TEXT("node")));
			Receipt->SetStringField(TEXT("sync_group_name"), Row->GetStringField(TEXT("sync_group_name")));
			Receipt->SetNumberField(TEXT("sync_group_role"), Row->GetNumberField(TEXT("sync_group_role")));
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		if (Name == TEXT("animbp_sync_group_inspect_native"))
		{
			FString GraphName;
			Args->TryGetStringField(TEXT("graph_name"), GraphName);
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 NodeCount = 0;
			if (Blueprint)
			{
				TArray<UEdGraph*> Graphs;
				Blueprint->GetAllGraphs(Graphs);
				for (UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					if (!GraphName.IsEmpty() && !Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
					{
						continue;
					}
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						FStructProperty* NodeProp = FindNodeStructProperty(Node);
						if (!NodeProp || !NodeProp->Struct)
						{
							continue;
						}
						bool bHasGroupName = false;
						for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It)
						{
							if (It->GetFName() == FName(TEXT("GroupName")))
							{
								bHasGroupName = true;
								break;
							}
						}
						if (!bHasGroupName)
						{
							continue;
						}
						++NodeCount;
						TSharedRef<FJsonObject> Row = SyncGroupReadbackJson(Node);
						Row->SetStringField(TEXT("graph"), Graph->GetName());
						Rows.Add(MakeShared<FJsonValueObject>(Row));
					}
				}
			}
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetNumberField(TEXT("node_count"), NodeCount);
			Receipt->SetArrayField(TEXT("nodes"), Rows);
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		// ---- animbp: transitions --------------------------------------------------
		if (Name == TEXT("animbp_transition_priority_set_native"))
		{
			FString NodeId;
			int64 Priority = 0;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetNumberField(TEXT("priority"), Priority);
			UEdGraphNode* Node = FindAnimGraphNodeAnyGraph(Blueprint, NodeId, TEXT(""), Error);
			if (!Node)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_node_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			FProperty* PriorityProp = FindPropByName(Node, TEXT("PriorityOrder"));
			if (!PriorityProp)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_priority_order_property"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("Transition node has no PriorityOrder property on this engine.");
				return false;
			}
			FString InnerError;
			if (!ReflectWriteIntAt(PriorityProp->ContainerPtrToValuePtr<void>(Node), PriorityProp, Priority, InnerError))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_priority_write"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = InnerError;
				return false;
			}
			int64 ReadBack = 0;
			if (const FIntProperty* Int = CastField<FIntProperty>(PriorityProp))
			{
				ReadBack = Int->GetPropertyValue_InContainer(Node);
			}
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetNumberField(TEXT("priority"), ReadBack);
			Receipt->SetBoolField(TEXT("verified"), ReadBack == Priority);
			if (ReadBack != Priority)
			{
				Error = FString::Printf(TEXT("Priority readback mismatch: wrote %lld, read %lld."), Priority, ReadBack);
				return false;
			}
			return true;
		}

		if (Name == TEXT("animbp_transition_blend_profile_set_native"))
		{
			FString NodeId;
			FString ProfilePath;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("blend_profile_path"), ProfilePath);
			UObject* Profile = LoadToolAsset(Context, ProfilePath, Error);
			if (!Profile)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_load_blend_profile"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			if (!Profile->GetClass()->GetName().Contains(TEXT("BlendProfile")))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_blend_profile_class_gate"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("blend_profile_path does not resolve to a BlendProfile-derived asset.");
				return false;
			}
			UEdGraphNode* Node = FindAnimGraphNodeAnyGraph(Blueprint, NodeId, TEXT(""), Error);
			if (!Node)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_node_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			// The wrapper lives directly on the transition node (BlendProfileWrapper)
			// as a USTRUCT wrapping a TObjectPtr/TWeakObjectPtr<UBlendProfile>.
			FProperty* WrapperProp = FindPropByName(Node, TEXT("BlendProfileWrapper"));
			bool bWroteProfile = false;
			UObject* ReadBack = nullptr;
			if (FStructProperty* WrapperStruct = WrapperProp ? CastField<FStructProperty>(WrapperProp) : nullptr)
			{
				void* WrapperPtr = WrapperProp->ContainerPtrToValuePtr<void>(Node);
				for (TFieldIterator<FProperty> It(WrapperStruct->Struct); It; ++It)
				{
					const FString WrappedName = It->GetFName().ToString();
					if (WrappedName != TEXT("BlendProfile") && WrappedName != TEXT("Profile") && WrappedName != TEXT("BlendProfileObject"))
					{
						continue;
					}
					if (const FObjectProperty* Obj = CastField<FObjectProperty>(*It))
					{
						Obj->SetObjectPropertyValue_InContainer(WrapperPtr, Profile);
						ReadBack = Obj->GetObjectPropertyValue_InContainer(WrapperPtr);
						bWroteProfile = true;
						break;
					}
					if (const FWeakObjectProperty* Weak = CastField<FWeakObjectProperty>(*It))
					{
						Weak->SetObjectPropertyValue_InContainer(WrapperPtr, Profile);
						ReadBack = Weak->GetObjectPropertyValue_InContainer(WrapperPtr);
						bWroteProfile = true;
						break;
					}
				}
			}
			if (!bWroteProfile)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_blend_profile_wrapper"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("Transition node has no reflected BlendProfileWrapper holding a profile reference on this engine.");
				return false;
			}
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetStringField(TEXT("blend_profile_path"), ReadBack ? ReadBack->GetPathName() : TEXT(""));
			Receipt->SetBoolField(TEXT("verified"), ReadBack == Profile);
			if (ReadBack != Profile)
			{
				Error = TEXT("Blend profile readback mismatch after write.");
				return false;
			}
			return true;
		}

		if (Name == TEXT("animbp_transition_interrupt_rule_set_native"))
		{
			FString NodeId;
			FString Rule;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("interrupt_rule"), Rule);
			UEdGraphNode* Node = FindAnimGraphNodeAnyGraph(Blueprint, NodeId, TEXT(""), Error);
			if (!Node)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_node_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			FProperty* InterruptProp = FindPropByName(Node, TEXT("TransitionInterrupt"));
			FStructProperty* InterruptStruct = InterruptProp ? CastField<FStructProperty>(InterruptProp) : nullptr;
			FObjectProperty* NotifyProp = nullptr;
			if (InterruptStruct)
			{
				void* InterruptPtr = InterruptProp->ContainerPtrToValuePtr<void>(Node);
				for (TFieldIterator<FProperty> It(InterruptStruct->Struct); It; ++It)
				{
					const FString NotifyName = It->GetFName().ToString();
					if (NotifyName == TEXT("Notify") || NotifyName == TEXT("NotifyStateClass"))
					{
						NotifyProp = CastField<FObjectProperty>(*It);
						if (NotifyProp)
						{
							break;
						}
					}
				}
				if (!NotifyProp)
				{
					// FAnimNotifyEvent may hold the notify through NotifyStateClass on
					// some engines; fall back to a full-field probe before failing.
					for (TFieldIterator<FProperty> It(InterruptStruct->Struct); It; ++It)
					{
						if (FObjectProperty* Obj = CastField<FObjectProperty>(*It))
						{
							NotifyProp = Obj;
							break;
						}
					}
				}
				if (NotifyProp && Rule.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					// Clearing the interruption notify is a safe reflected write.
					NotifyProp->SetObjectPropertyValue_InContainer(InterruptPtr, nullptr);
					UObject* Current = NotifyProp->GetObjectPropertyValue_InContainer(InterruptPtr);
					if (Current == nullptr)
					{
						Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
						Receipt->SetStringField(TEXT("interrupt_rule"), TEXT("none"));
						Receipt->SetStringField(TEXT("current_notify"), TEXT(""));
						Receipt->SetBoolField(TEXT("verified"), true);
						return true;
					}
					Error = TEXT("Interrupt notify did not clear after write.");
					Receipt->SetStringField(TEXT("status"), TEXT("failed_interrupt_clear"));
					Receipt->SetBoolField(TEXT("verified"), false);
					return false;
				}
			}
			// Named interruption rules require constructing a concrete
			// AnimNotify_TransitionInterrupt subclass; that class is editor-private
			// and unreachable without AnimGraph module internals.
			UObject* CurrentNotify = nullptr;
			if (InterruptStruct && NotifyProp)
			{
				void* InterruptPtr = InterruptProp->ContainerPtrToValuePtr<void>(Node);
				CurrentNotify = NotifyProp->GetObjectPropertyValue_InContainer(InterruptPtr);
			}
			Receipt->SetStringField(TEXT("status"), TEXT("blocked_named_interrupt_requires_native_notify_class"));
			Receipt->SetStringField(TEXT("current_notify"), CurrentNotify ? CurrentNotify->GetClass()->GetName() : TEXT(""));
			Receipt->SetBoolField(TEXT("verified"), false);
			Error = FString::Printf(
				TEXT("interrupt_rule '%s' requires constructing a native AnimNotify_TransitionInterrupt subclass (editor-private); only 'none' (clear) is supported through reflection. The tool is fail-closed."),
				*Rule);
			return false;
		}

		// ---- animbp: cached pose and linked graph ---------------------------------
		if (Name == TEXT("animbp_cached_pose_rename_native"))
		{
			FString NodeId;
			FString NewName;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("new_name"), NewName);
			if (NewName.IsEmpty())
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_missing_new_name"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("new_name is required.");
				return false;
			}
			UEdGraphNode* Node = FindAnimGraphNodeAnyGraph(Blueprint, NodeId, TEXT(""), Error);
			if (!Node)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_node_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			int32 Renamed = 0;
			if (!ReflectRenameCachedPose(Blueprint, Node, NewName, Renamed, Error))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_cached_pose_rename"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetStringField(TEXT("cached_pose_name"), NewName);
			Receipt->SetNumberField(TEXT("renamed_node_count"), Renamed);
			Receipt->SetBoolField(TEXT("verified"), Renamed >= 1);
			if (Renamed < 1)
			{
				Error = TEXT("Cached pose rename produced no verified rename.");
				return false;
			}
			return true;
		}

		if (Name == TEXT("animbp_linked_graph_bind_native"))
		{
			FString NodeId;
			FString GraphPath;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetStringField(TEXT("linked_graph_path"), GraphPath);
			UObject* Linked = LoadToolAsset(Context, GraphPath, Error);
			if (!Linked)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_load_linked_graph"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			if (!Linked->IsA<UBlueprint>() && !Linked->GetClass()->GetName().Contains(TEXT("Anim")))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_linked_graph_class_gate"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("linked_graph_path must resolve to a Blueprint asset.");
				return false;
			}
			UEdGraphNode* Node = FindAnimGraphNodeAnyGraph(Blueprint, NodeId, TEXT(""), Error);
			if (!Node)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_node_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			FStructProperty* NodeProp = FindNodeStructProperty(Node);
			FProperty* LinkedProp = nullptr;
			void* NodePtr = nullptr;
			if (NodeProp && NodeProp->Struct)
			{
				NodePtr = NodeProp->ContainerPtrToValuePtr<void>(Node);
				for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It)
				{
					if (It->GetFName() == FName(TEXT("LinkedAnimGraph")))
					{
						LinkedProp = *It;
						break;
					}
				}
			}
			if (!LinkedProp || !NodePtr)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_linked_graph_property"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("Linked-anim-graph node exposes no LinkedAnimGraph property on this engine.");
				return false;
			}
			FString InnerError;
			if (!ReflectWriteObjectAt(NodePtr, LinkedProp, Linked, InnerError))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_linked_graph_write"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = InnerError;
				return false;
			}
			UObject* ReadBack = nullptr;
			if (const FObjectProperty* Obj = CastField<FObjectProperty>(LinkedProp))
			{
				ReadBack = Obj->GetObjectPropertyValue_InContainer(NodePtr);
			}
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetStringField(TEXT("linked_graph_path"), ReadBack ? ReadBack->GetPathName() : TEXT(""));
			Receipt->SetBoolField(TEXT("verified"), ReadBack == Linked);
			if (ReadBack != Linked)
			{
				Error = TEXT("Linked graph readback mismatch after write.");
				return false;
			}
			return true;
		}

		// ---- animbp: state alias (probe-write only) ------------------------------
		if (Name == TEXT("animbp_state_alias_bind_native"))
		{
			FString NodeId;
			const TArray<TSharedPtr<FJsonValue>>* StateNames = nullptr;
			Args->TryGetStringField(TEXT("node_id"), NodeId);
			Args->TryGetArrayField(TEXT("state_names"), StateNames);
			UEdGraphNode* Node = FindAnimGraphNodeAnyGraph(Blueprint, NodeId, TEXT(""), Error);
			if (!Node)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_node_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			FArrayProperty* AliasProp = nullptr;
			FProperty* AliasInner = nullptr;
			for (TFieldIterator<FProperty> It(Node->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				const FString PropName = It->GetFName().ToString();
				if (PropName != TEXT("StateAliases") && PropName != TEXT("Aliases") && PropName != TEXT("AliasNames"))
				{
					continue;
				}
				if (FArrayProperty* Arr = CastField<FArrayProperty>(*It))
				{
					AliasProp = Arr;
					AliasInner = Arr->Inner;
					break;
				}
			}
			if (!AliasProp)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_state_alias_property"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("The state node exposes no state-alias array property (StateAliases/Aliases/AliasNames) on this engine; alias binding is fail-closed.");
				return false;
			}
			FScriptArrayHelper Helper(AliasProp, AliasProp->ContainerPtrToValuePtr<void>(Node));
			Helper.EmptyValues();
			int32 Wrote = 0;
			for (const TSharedPtr<FJsonValue>& Value : *StateNames)
			{
				FString AliasName = Value.IsValid() ? Value->AsString() : FString();
				if (AliasName.IsEmpty())
				{
					continue;
				}
				const int32 Index = Helper.AddValue();
				FString InnerError;
				if (ReflectWriteStringAt(Helper.GetRawPtr(Index), AliasInner, AliasName, InnerError))
				{
					++Wrote;
				}
				else
				{
					Helper.RemoveValues(Index, 1);
				}
			}
			Receipt->SetStringField(TEXT("status"), Wrote > 0 ? TEXT("succeeded") : TEXT("failed_alias_write"));
			Receipt->SetNumberField(TEXT("alias_count"), Wrote);
			Receipt->SetBoolField(TEXT("verified"), Wrote > 0);
			if (Wrote == 0)
			{
				Error = TEXT("No state aliases could be written to the alias array.");
				return false;
			}
			return true;
		}

		// ---- animbp: layer arrays (fixed-size, probe + fail-closed) --------------
		if (Name == TEXT("animbp_layer_add_native") || Name == TEXT("animbp_layer_remove_native") || Name == TEXT("animbp_layer_weight_set_native"))
		{
			int32 LayerNodeCount = 0;
			if (Blueprint)
			{
				TArray<UEdGraph*> Graphs;
				Blueprint->GetAllGraphs(Graphs);
				for (UEdGraph* Graph : Graphs)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node)
						{
							continue;
						}
						if (Node->GetClass()->GetName().Contains(TEXT("LayeredBoneBlend")))
						{
							++LayerNodeCount;
							continue;
						}
						FStructProperty* NodeProp = FindNodeStructProperty(Node);
						if (!NodeProp || !NodeProp->Struct)
						{
							continue;
						}
						for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It)
						{
							if (It->GetFName() == FName(TEXT("BlendPoses")))
							{
								++LayerNodeCount;
								break;
							}
						}
					}
				}
			}
			Receipt->SetNumberField(TEXT("layered_blend_node_count"), LayerNodeCount);
			Receipt->SetStringField(TEXT("status"), TEXT("blocked_fixed_size_layer_array"));
			Receipt->SetBoolField(TEXT("verified"), false);
			Error = TEXT("Layer arrays (BlendPoses) are edit-fixed-size on this engine; add/remove/weight mutation requires the AnimGraph pin-editing pipeline. The tool is fail-closed.");
			return false;
		}

		// ---- animnext: variables and graph inspection -----------------------------
		if (Name == TEXT("animnext_variable_add_native"))
		{
			URigVMBlueprint* RigBP = Cast<URigVMBlueprint>(Asset);
			if (!RigBP)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_not_rigvm_blueprint"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("AnimNext variable add requires a URigVMBlueprint-derived asset (e.g. UAF.AnimNextModule).");
				return false;
			}
			FString VarName;
			FString TypeName;
			FString DefaultValue;
			Args->TryGetStringField(TEXT("variable_name"), VarName);
			Args->TryGetStringField(TEXT("type_name"), TypeName);
			Args->TryGetStringField(TEXT("default_value"), DefaultValue);
			if (VarName.IsEmpty() || TypeName.IsEmpty())
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_missing_variable_contract"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("variable_name and type_name are required.");
				return false;
			}
			URigVMController* Controller = ResolveRigVMController(RigBP);
			URigVMGraph* Graph = RigBP->GetDefaultModel();
			if (!Controller || !Graph)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_rigvm_controller"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("No RigVM controller/model is available for this asset.");
				return false;
			}
			URigVMVariableNode* Added = Controller->AddVariableNode(
				FName(*VarName), TypeName, nullptr, false, DefaultValue,
				FVector2D(0.0f, 0.0f), TEXT(""), true, false);
			if (!Added)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_add_variable_node"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = FString::Printf(TEXT("AddVariableNode returned null for '%s' (type '%s')."), *VarName, *TypeName);
				return false;
			}
			URigVMVariableNode* ReadBack = nullptr;
			for (URigVMNode* Node : Graph->GetNodes())
			{
				URigVMVariableNode* VarNode = Cast<URigVMVariableNode>(Node);
				if (VarNode && VarNode->GetVariableName() == FName(*VarName))
				{
					ReadBack = VarNode;
					break;
				}
			}
			Receipt->SetStringField(TEXT("status"), ReadBack ? TEXT("succeeded") : TEXT("failed_readback"));
			Receipt->SetStringField(TEXT("variable_name"), VarName);
			Receipt->SetStringField(TEXT("node_name"), ReadBack ? ReadBack->GetName() : Added->GetName());
			Receipt->SetStringField(TEXT("node_class"), Added->GetClass()->GetName());
			Receipt->SetStringField(TEXT("default_value"), ReadBack ? ReadBack->GetDefaultValue() : DefaultValue);
			Receipt->SetBoolField(TEXT("verified"), ReadBack != nullptr);
			if (!ReadBack)
			{
				Error = TEXT("Variable node could not be read back by name after add.");
				return false;
			}
			return true;
		}

		if (Name == TEXT("animnext_variable_set_default_native"))
		{
			URigVMBlueprint* RigBP = Cast<URigVMBlueprint>(Asset);
			if (!RigBP)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_not_rigvm_blueprint"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("AnimNext variable default set requires a URigVMBlueprint-derived asset.");
				return false;
			}
			FString VarName;
			FString DefaultValue;
			Args->TryGetStringField(TEXT("variable_name"), VarName);
			Args->TryGetStringField(TEXT("default_value"), DefaultValue);
			URigVMController* Controller = ResolveRigVMController(RigBP);
			URigVMGraph* Graph = RigBP->GetDefaultModel();
			if (!Controller || !Graph)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_rigvm_controller"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("No RigVM controller/model is available for this asset.");
				return false;
			}
			URigVMVariableNode* Target = nullptr;
			for (URigVMNode* Node : Graph->GetNodes())
			{
				URigVMVariableNode* VarNode = Cast<URigVMVariableNode>(Node);
				if (VarNode && VarNode->GetVariableName() == FName(*VarName))
				{
					Target = VarNode;
					break;
				}
			}
			if (!Target)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_variable_not_found"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = FString::Printf(TEXT("Variable node '%s' not found in the default graph."), *VarName);
				return false;
			}
			const FString PinPath = Target->GetName() + TEXT(".VariableName");
			// UE 5.4 appended bSetValueOnLinkedPins; 5.3 stops at bPrintPythonCommand.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			const bool bPinDefaultApplied = Controller->SetPinDefaultValue(PinPath, DefaultValue, true, true, false, false, true);
#else
			const bool bPinDefaultApplied = Controller->SetPinDefaultValue(PinPath, DefaultValue, true, true, false, false);
#endif
			if (!bPinDefaultApplied)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_set_pin_default_value"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = FString::Printf(TEXT("SetPinDefaultValue failed for pin '%s'."), *PinPath);
				return false;
			}
			const FString ReadBack = Target->GetDefaultValue();
			Receipt->SetStringField(TEXT("status"), ReadBack == DefaultValue ? TEXT("succeeded") : TEXT("failed_readback"));
			Receipt->SetStringField(TEXT("variable_name"), VarName);
			Receipt->SetStringField(TEXT("default_value"), ReadBack);
			Receipt->SetBoolField(TEXT("verified"), ReadBack == DefaultValue);
			if (ReadBack != DefaultValue)
			{
				Error = FString::Printf(TEXT("Variable default readback mismatch: wrote '%s', read '%s'."), *DefaultValue, *ReadBack);
				return false;
			}
			return true;
		}

		if (Name == TEXT("animnext_graph_inspect_native"))
		{
			URigVMBlueprint* RigBP = Cast<URigVMBlueprint>(Asset);
			if (!RigBP)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_not_rigvm_blueprint"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("AnimNext graph inspect requires a URigVMBlueprint-derived asset.");
				return false;
			}
			FString GraphName;
			Args->TryGetStringField(TEXT("graph_name"), GraphName);
			TArray<URigVMGraph*> Models;
			Models.Add(RigBP->GetDefaultModel());
			const TArray<URigVMGraph*> AllModels = RigBP->GetAllModels();
			for (URigVMGraph* Model : AllModels)
			{
				if (Model && !Models.Contains(Model))
				{
					Models.Add(Model);
				}
			}
			TArray<TSharedPtr<FJsonValue>> GraphsJson;
			for (URigVMGraph* Graph : Models)
			{
				if (!Graph)
				{
					continue;
				}
				if (!GraphName.IsEmpty() && !Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("graph_name"), Graph->GetName());
				Row->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
				Row->SetNumberField(TEXT("link_count"), Graph->GetLinks().Num());
				TArray<TSharedPtr<FJsonValue>> NodesJson;
				for (URigVMNode* Node : Graph->GetNodes())
				{
					if (!Node)
					{
						continue;
					}
					TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
					NodeJson->SetStringField(TEXT("node_name"), Node->GetName());
					NodeJson->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
					if (URigVMVariableNode* VarNode = Cast<URigVMVariableNode>(Node))
					{
						NodeJson->SetStringField(TEXT("variable_name"), VarNode->GetVariableName().ToString());
						NodeJson->SetStringField(TEXT("default_value"), VarNode->GetDefaultValue());
					}
					NodesJson.Add(MakeShared<FJsonValueObject>(NodeJson));
				}
				Row->SetArrayField(TEXT("nodes"), NodesJson);
				GraphsJson.Add(MakeShared<FJsonValueObject>(Row));
			}
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetNumberField(TEXT("graph_count"), GraphsJson.Num());
			Receipt->SetArrayField(TEXT("graphs"), GraphsJson);
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		// ---- pose_search: channels, database assets, validation -------------------
		if (Name == TEXT("pose_search_channel_add_native"))
		{
			FString ChannelClassPath;
			FString ChannelName;
			Args->TryGetStringField(TEXT("channel_class_path"), ChannelClassPath);
			Args->TryGetStringField(TEXT("channel_name"), ChannelName);
			if (ChannelClassPath.IsEmpty())
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_missing_channel_class_path"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("channel_class_path is required.");
				return false;
			}
			UClass* ChannelClass = LoadClass<UObject>(nullptr, *ChannelClassPath);
			if (!ChannelClass)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_load_channel_class"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = FString::Printf(TEXT("Cannot resolve channel class '%s'."), *ChannelClassPath);
				return false;
			}
			if (!ChannelClass->GetName().Contains(TEXT("PoseSearchFeatureChannel")))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_channel_class_gate"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("channel_class_path must resolve to a UPoseSearchFeatureChannel subclass.");
				return false;
			}
			FArrayProperty* ChannelsProp = nullptr;
			FObjectProperty* ChannelInner = nullptr;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (It->GetFName() != FName(TEXT("Channels")))
				{
					continue;
				}
				if (FArrayProperty* Arr = CastField<FArrayProperty>(*It))
				{
					ChannelsProp = Arr;
					ChannelInner = CastField<FObjectProperty>(Arr->Inner);
				}
				break;
			}
			if (!ChannelsProp || !ChannelInner)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_channels_object_array"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("Schema asset exposes no editable Channels object array on this engine.");
				return false;
			}
			if (!ChannelClass->IsChildOf(ChannelInner->PropertyClass))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_channel_assignability_gate"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = FString::Printf(TEXT("Channel class '%s' is not assignable to Channels element type '%s'."), *ChannelClass->GetName(), *ChannelInner->PropertyClass->GetName());
				return false;
			}
			FName ChannelObjName = ChannelName.IsEmpty() ? ChannelClass->GetFName() : FName(*ChannelName);
			ChannelObjName = MakeUniqueObjectName(Asset, ChannelClass, ChannelObjName);
			UObject* Channel = NewObject<UObject>(Asset, ChannelClass, ChannelObjName, RF_Transactional);
			FScriptArrayHelper Helper(ChannelsProp, ChannelsProp->ContainerPtrToValuePtr<void>(Asset));
			const int32 NewIndex = Helper.AddValue();
			FString InnerError;
			if (!ReflectWriteObjectAt(Helper.GetRawPtr(NewIndex), ChannelInner, Channel, InnerError))
			{
				Helper.RemoveValues(NewIndex, 1);
				Receipt->SetStringField(TEXT("status"), TEXT("failed_channel_object_write"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = InnerError;
				return false;
			}
			const int32 Count = Helper.Num();
			UObject* ReadBack = ChannelInner->GetObjectPropertyValue(Helper.GetRawPtr(Count - 1));
			Receipt->SetStringField(TEXT("status"), ReadBack == Channel ? TEXT("succeeded") : TEXT("failed_readback"));
			Receipt->SetNumberField(TEXT("channel_count"), Count);
			Receipt->SetStringField(TEXT("channel_class"), ChannelClass->GetName());
			Receipt->SetStringField(TEXT("channel_name"), ChannelObjName.ToString());
			Receipt->SetStringField(TEXT("added_channel_path"), ReadBack ? ReadBack->GetPathName() : TEXT(""));
			Receipt->SetBoolField(TEXT("verified"), ReadBack == Channel);
			if (ReadBack != Channel)
			{
				Error = TEXT("Channel readback mismatch after write.");
				return false;
			}
			return true;
		}

		if (Name == TEXT("pose_search_database_asset_add_native"))
		{
			FString SourcePath;
			Args->TryGetStringField(TEXT("source_asset_path"), SourcePath);
			UObject* Source = LoadToolAsset(Context, SourcePath, Error);
			if (!Source)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_load_source_asset"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			// PoseSearch indexing dereferences the sequence skeleton; a skeleton-less
			// AnimSequence would crash the indexer, so the tool is fail-closed here.
			if (UAnimSequence* SourceSequence = Cast<UAnimSequence>(Source))
			{
				if (!SourceSequence->GetSkeleton())
				{
					Receipt->SetStringField(TEXT("status"), TEXT("blocked_source_sequence_missing_skeleton"));
					Receipt->SetBoolField(TEXT("verified"), false);
					Error = TEXT("The source AnimSequence has no Skeleton; a skeleton is required for PoseSearch indexing. The tool is fail-closed.");
					return false;
				}
			}
			FArrayProperty* AssetsProp = nullptr;
			FStructProperty* EntryStruct = nullptr;
			FObjectProperty* AnimAssetProp = nullptr;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (It->GetFName() != FName(TEXT("DatabaseAnimationAssets")))
				{
					continue;
				}
				AssetsProp = CastField<FArrayProperty>(*It);
				if (!AssetsProp)
				{
					break;
				}
				EntryStruct = CastField<FStructProperty>(AssetsProp->Inner);
				if (!EntryStruct || !EntryStruct->Struct)
				{
					break;
				}
				for (TFieldIterator<FProperty> SIt(EntryStruct->Struct); SIt; ++SIt)
				{
					if (SIt->GetFName() == FName(TEXT("AnimAsset")))
					{
						AnimAssetProp = CastField<FObjectProperty>(*SIt);
						break;
					}
				}
				break;
			}
			if (!AssetsProp || !EntryStruct || !AnimAssetProp)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_database_animation_assets_array"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("PoseSearchDatabase does not expose the reflected DatabaseAnimationAssets/AnimAsset array on this engine.");
				return false;
			}
			FScriptArrayHelper Helper(AssetsProp, AssetsProp->ContainerPtrToValuePtr<void>(Asset));
			const int32 NewIndex = Helper.AddValue();
			// AddValue only grows the raw array memory; the new struct element must
			// be constructed or the PoseSearch indexer reads garbage and crashes.
			if (UScriptStruct* EntryScriptStruct = EntryStruct->Struct)
			{
				EntryScriptStruct->InitializeStruct(Helper.GetRawPtr(NewIndex));
			}
			FString InnerError;
			if (!ReflectWriteObjectAt(Helper.GetRawPtr(NewIndex), AnimAssetProp, Source, InnerError))
			{
				Helper.RemoveValues(NewIndex, 1);
				Receipt->SetStringField(TEXT("status"), TEXT("failed_anim_asset_write"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = InnerError;
				return false;
			}
			const int32 Count = Helper.Num();
			// AnimAsset is a struct-field property (Offset_Internal > 0), not an array
			// Inner; GetObjectPropertyValue reads the value AT the given address, so the
			// element base must be offset to the field, otherwise we read the base-class
			// vtable region as a UObject* and crash on the readback path.
			UObject* ReadBack = AnimAssetProp->GetObjectPropertyValue(AnimAssetProp->ContainerPtrToValuePtr<void>(Helper.GetRawPtr(Count - 1)));
			Receipt->SetStringField(TEXT("status"), ReadBack == Source ? TEXT("succeeded") : TEXT("failed_readback"));
			Receipt->SetNumberField(TEXT("entry_count"), Count);
			Receipt->SetStringField(TEXT("source_asset_path"), Source->GetPathName());
			Receipt->SetStringField(TEXT("added_entry_path"), ReadBack ? ReadBack->GetPathName() : TEXT(""));
			Receipt->SetBoolField(TEXT("verified"), ReadBack == Source);
			if (ReadBack != Source)
			{
				Error = TEXT("Database asset entry readback mismatch after write.");
				return false;
			}
			return true;
		}

		if (Name == TEXT("pose_search_database_asset_remove_native"))
		{
			int64 EntryIndex = 0;
			Args->TryGetNumberField(TEXT("entry_index"), EntryIndex);
			FArrayProperty* AssetsProp = nullptr;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (It->GetFName() == FName(TEXT("DatabaseAnimationAssets")))
				{
					AssetsProp = CastField<FArrayProperty>(*It);
					break;
				}
			}
			if (!AssetsProp)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_database_animation_assets_array"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("PoseSearchDatabase does not expose the DatabaseAnimationAssets array on this engine.");
				return false;
			}
			FScriptArrayHelper Helper(AssetsProp, AssetsProp->ContainerPtrToValuePtr<void>(Asset));
			if (EntryIndex < 0 || EntryIndex >= Helper.Num())
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_index_out_of_range"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = FString::Printf(TEXT("entry_index %lld out of range (count=%d)."), EntryIndex, Helper.Num());
				return false;
			}
			if (const FStructProperty* EntryStructForRemove = CastField<FStructProperty>(AssetsProp->Inner))
			{
				if (UScriptStruct* EntryScriptStruct = EntryStructForRemove->Struct)
				{
					EntryScriptStruct->DestroyStruct(Helper.GetRawPtr(static_cast<int32>(EntryIndex)));
				}
			}
			const int32 Before = Helper.Num();
			Helper.RemoveValues(static_cast<int32>(EntryIndex), 1);
			const int32 After = Helper.Num();
			Receipt->SetStringField(TEXT("status"), After == Before - 1 ? TEXT("succeeded") : TEXT("failed_remove_readback"));
			Receipt->SetNumberField(TEXT("entry_count_before"), Before);
			Receipt->SetNumberField(TEXT("entry_count_after"), After);
			Receipt->SetBoolField(TEXT("verified"), After == Before - 1);
			if (After != Before - 1)
			{
				Error = TEXT("Database asset entry removal did not reduce the array by one.");
				return false;
			}
			return true;
		}

		if (Name == TEXT("motion_matching_validate_native"))
		{
			int32 EntryCount = 0;
			FString SchemaPath;
			FString SchemaClass;
			int32 TagCount = 0;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				const FString PropName = It->GetFName().ToString();
				if (PropName == TEXT("DatabaseAnimationAssets"))
				{
					if (FArrayProperty* Arr = CastField<FArrayProperty>(*It))
					{
						FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(Asset));
						EntryCount = Helper.Num();
					}
				}
				else if (PropName == TEXT("Schema"))
				{
					if (const FObjectProperty* Obj = CastField<FObjectProperty>(*It))
					{
						UObject* SchemaObj = Obj->GetObjectPropertyValue_InContainer(Asset);
						SchemaPath = SchemaObj ? SchemaObj->GetPathName() : TEXT("");
						SchemaClass = SchemaObj ? SchemaObj->GetClass()->GetName() : TEXT("");
					}
					else if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(*It))
					{
						SchemaPath = Soft->GetPropertyValue_InContainer(Asset).ToString();
					}
				}
				else if (PropName == TEXT("Tags"))
				{
					if (FArrayProperty* Arr = CastField<FArrayProperty>(*It))
					{
						FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(Asset));
						TagCount = Helper.Num();
					}
				}
			}
			const bool bValid = EntryCount > 0 && !SchemaPath.IsEmpty();
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetNumberField(TEXT("asset_entry_count"), EntryCount);
			Receipt->SetNumberField(TEXT("tag_count"), TagCount);
			Receipt->SetStringField(TEXT("schema_path"), SchemaPath);
			Receipt->SetStringField(TEXT("schema_class"), SchemaClass);
			Receipt->SetBoolField(TEXT("motion_matching_valid"), bValid);
			Receipt->SetStringField(TEXT("validation_detail"), bValid ? TEXT("database has schema and at least one source asset") : TEXT("database missing schema or source assets"));
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		// ---- animation_processing: mirror ----------------------------------------
		if (Name == TEXT("animation_mirror_native"))
		{
			FString MirrorPath;
			Args->TryGetStringField(TEXT("mirror_data_table_path"), MirrorPath);
			UObject* Mirror = LoadToolAsset(Context, MirrorPath, Error);
			if (!Mirror)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_load_mirror_data_table"));
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			if (!Mirror->GetClass()->GetName().Contains(TEXT("MirrorDataTable")))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_mirror_class_gate"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("mirror_data_table_path must resolve to a MirrorDataTable asset.");
				return false;
			}
			FProperty* MirrorProp = FindPropByName(Asset, TEXT("MirrorDataTable"));
			FObjectProperty* MirrorObjProp = MirrorProp ? CastField<FObjectProperty>(MirrorProp) : nullptr;
			if (!MirrorObjProp)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_mirror_data_table_property"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("AnimSequence does not expose a MirrorDataTable object property on this engine.");
				return false;
			}
			FString InnerError;
			if (!ReflectWriteObjectAt(MirrorProp->ContainerPtrToValuePtr<void>(Asset), MirrorProp, Mirror, InnerError))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_mirror_write"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = InnerError;
				return false;
			}
			UObject* ReadBack = MirrorObjProp->GetObjectPropertyValue_InContainer(Asset);
			Receipt->SetStringField(TEXT("status"), ReadBack == Mirror ? TEXT("succeeded") : TEXT("failed_readback"));
			Receipt->SetStringField(TEXT("mirror_data_table_path"), ReadBack ? ReadBack->GetPathName() : TEXT(""));
			Receipt->SetBoolField(TEXT("verified"), ReadBack == Mirror);
			if (ReadBack != Mirror)
			{
				Error = TEXT("Mirror data table readback mismatch after write.");
				return false;
			}
			return true;
		}

		// ---- motion_warping: no engine API, probe + fail-closed ------------------
		if (Name == TEXT("motion_warping_window_add_native") || Name == TEXT("motion_warping_window_remove_native"))
		{
			int32 WarpNotifyCount = 0;
			if (const UAnimSequence* Seq = Cast<UAnimSequence>(Asset))
			{
				for (const FAnimNotifyTrack& Track : Seq->AnimNotifyTracks)
				{
					for (const FAnimNotifyEvent* Event : Track.Notifies)
					{
						if (Event && Event->Notify && Event->Notify->GetClass()->GetName().Contains(TEXT("MotionWarping")))
						{
							++WarpNotifyCount;
						}
					}
				}
			}
			Receipt->SetNumberField(TEXT("existing_motion_warping_notify_count"), WarpNotifyCount);
			Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_native_motion_warping_api"));
			Receipt->SetBoolField(TEXT("verified"), false);
			Error = TEXT("MotionWarping window mutation requires the MotionWarping plugin editor API (FAnimNotifyEvent window construction). No such native adapter is available in this executor; the tool is fail-closed.");
			return false;
		}

		// ---- control_rig_debug: breakpoints/watches are editor-session state -----
		if (Name == TEXT("control_rig_debug_breakpoint_add_native") || Name == TEXT("control_rig_debug_breakpoint_remove_native") ||
			Name == TEXT("control_rig_debug_breakpoint_clear_native") || Name == TEXT("control_rig_debug_watch_add_native") ||
			Name == TEXT("control_rig_debug_watch_remove_native"))
		{
			int32 GraphNodeCount = 0;
			if (URigVMBlueprint* RigBP = Cast<URigVMBlueprint>(Asset))
			{
				if (URigVMGraph* Graph = RigBP->GetDefaultModel())
				{
					GraphNodeCount = Graph->GetNodes().Num();
				}
			}
			Receipt->SetNumberField(TEXT("graph_node_count"), GraphNodeCount);
			Receipt->SetStringField(TEXT("status"), TEXT("blocked_editor_session_debug_state"));
			Receipt->SetBoolField(TEXT("verified"), false);
			Error = TEXT("RigVM breakpoints and watched pins are editor-session UI state (FControlRigEditor debug session) without a persisted engine API; the tool is fail-closed.");
			return false;
		}

		if (Name == TEXT("rigvm_execution_trace_get_native"))
		{
			FString TraceId;
			Args->TryGetStringField(TEXT("trace_id"), TraceId);
			URigVMBlueprint* RigBP = Cast<URigVMBlueprint>(Asset);
			if (!RigBP)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_not_rigvm_blueprint"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("Execution trace requires a URigVMBlueprint-derived asset.");
				return false;
			}
			URigVMGraph* Graph = RigBP->GetDefaultModel();
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetStringField(TEXT("trace_id"), TraceId);
			Receipt->SetStringField(TEXT("trace_scope"), TEXT("graph_readiness"));
			Receipt->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : TEXT(""));
			Receipt->SetNumberField(TEXT("graph_node_count"), Graph ? Graph->GetNodes().Num() : 0);
			Receipt->SetNumberField(TEXT("graph_link_count"), Graph ? Graph->GetLinks().Num() : 0);
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		// ---- control_rig_test_pose: probe-write only ------------------------------
		if (Name == TEXT("control_rig_test_pose_set_native") || Name == TEXT("control_rig_test_pose_reset_native"))
		{
			FString PoseName;
			Args->TryGetStringField(TEXT("pose_name"), PoseName);
			FProperty* PoseProp = nullptr;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				const FString PropName = It->GetFName().ToString();
				if (PropName.Contains(TEXT("TestPose")) || (Name == TEXT("control_rig_test_pose_reset_native") && PropName == TEXT("PoseName")))
				{
					if (It->IsA<FStrProperty>() || It->IsA<FNameProperty>())
					{
						PoseProp = *It;
						break;
					}
				}
			}
			if (!PoseProp)
			{
				Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_test_pose_property"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = TEXT("ControlRigBlueprint exposes no test-pose string property on this engine; test pose mutation is fail-closed.");
				return false;
			}
			FString InnerError;
			if (!ReflectWriteStringAt(PoseProp->ContainerPtrToValuePtr<void>(Asset), PoseProp, PoseName, InnerError))
			{
				Receipt->SetStringField(TEXT("status"), TEXT("failed_test_pose_write"));
				Receipt->SetBoolField(TEXT("verified"), false);
				Error = InnerError;
				return false;
			}
			Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
			Receipt->SetStringField(TEXT("pose_name"), PoseName);
			Receipt->SetBoolField(TEXT("verified"), true);
			return true;
		}

		// ---- fallback -------------------------------------------------------------
		Receipt->SetStringField(TEXT("status"), TEXT("blocked_unhandled_semantic_tool"));
		Receipt->SetBoolField(TEXT("verified"), false);
		Error = FString::Printf(TEXT("%s has no name-specific native executor in this build; fail-closed."), *Name);
		return false;
	}

	static bool ExecuteGeneric(
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		UObject* Asset,
		TSharedRef<FJsonObject>& Receipt,
		FString& Error)
	{
		if (Spec.Action == EAction::SemanticFailClosed)
		{
			if (!ExecuteSemanticNative(Spec, Context, Args, Asset, Receipt, Error))
			{
				// The executor already set a structured block reason; keep the
				// receipt fail-closed unless it left no status at all.
				if (!Receipt->HasField(TEXT("status")))
				{
					Receipt->SetStringField(TEXT("status"), TEXT("blocked_no_proven_specialized_native_adapter"));
				}
				Receipt->SetBoolField(TEXT("verified"), false);
				return false;
			}
			if (Spec.bMutation && !SaveIfRequested(Context, Asset, Args, Receipt, Error))
			{
				return false;
			}
			return true;
		}
		if (Spec.Action == EAction::CreateAsset)
		{
			Asset->MarkPackageDirty();
			if (Spec.bCompile && !CompileAsset(Asset, Receipt, Error))
			{
				return false;
			}
			if (!SaveIfRequested(Context, Asset, Args, Receipt, Error))
			{
				return false;
			}
			const bool bCreated = IsValid(Asset) && !Asset->GetPathName().IsEmpty();
			Receipt->SetBoolField(TEXT("verified"), bCreated);
			Receipt->SetStringField(TEXT("created_class"), Asset->GetClass()->GetPathName());
			if (!bCreated)
			{
				Error = TEXT("Created asset failed immediate validity/path readback.");
			}
			return bCreated;
		}
		if (Spec.Action == EAction::CompileReceipt)
		{
			if (!CompileAsset(Asset, Receipt, Error))
			{
				return false;
			}
			Receipt->SetBoolField(TEXT("verified"), true);
			return SaveIfRequested(Context, Asset, Args, Receipt, Error);
		}

		Receipt->SetBoolField(TEXT("verified"), false);
		Error = FString::Printf(TEXT("%s reached no concrete native executor and is fail-closed."), Spec.Name);
		return false;
	}

	static bool Run(
		const FSpec& Spec,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		Out->SetStringField(TEXT("schema"), TEXT("somol.animation_completion.receipt.v1"));
		Out->SetStringField(TEXT("tool"), Spec.Name);
		Out->SetStringField(TEXT("family"), Spec.Family);
		Out->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
		Out->SetBoolField(TEXT("uses_python"), false);
		Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Out->SetStringField(TEXT("status"), TEXT("started"));

		if (!RequireModules(Spec, Out, Error))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_missing_native_module"));
			return false;
		}

		bool bExecute = false;
		Args->TryGetBoolField(TEXT("execute"), bExecute);
		if (Spec.bMutation && !bExecute)
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_execute_required"));
			Error = TEXT("This is a mutating tool. execute=true is required; a plan is never reported as success.");
			return false;
		}
		if (Spec.Action == EAction::CreateAsset && Spec.bCompile &&
			FString(Spec.Family).Equals(TEXT("animnext"), ESearchCase::IgnoreCase))
		{
			Out->SetStringField(TEXT("status"), TEXT("blocked_native_controller_required"));
			Out->SetBoolField(TEXT("verified"), false);
			Error = TEXT("AnimNext asset creation requires a version-proven native editor controller and compile-result adapter. Neither is available in this executor, so creation is fail-closed before allocating an asset.");
			return false;
		}

		UObject* Asset = nullptr;
		FString AssetPath;
		Args->TryGetStringField(TEXT("asset_path"), AssetPath);
		if (Spec.Action == EAction::CreateAsset)
		{
			FString PackagePath;
			FString AssetName;
			FString AssetClass = Spec.DefaultAssetClass ? Spec.DefaultAssetClass : TEXT("");
			FString FactoryClass = Spec.DefaultFactoryClass ? Spec.DefaultFactoryClass : TEXT("");
			Args->TryGetStringField(TEXT("package_path"), PackagePath);
			Args->TryGetStringField(TEXT("asset_name"), AssetName);
			Args->TryGetStringField(TEXT("asset_class_path"), AssetClass);
			Args->TryGetStringField(TEXT("factory_class_path"), FactoryClass);
			if (PackagePath.IsEmpty() || AssetName.IsEmpty() || AssetClass.IsEmpty() || FactoryClass.IsEmpty())
			{
				Out->SetStringField(TEXT("status"), TEXT("blocked_missing_create_contract"));
				Error = TEXT("package_path, asset_name, asset_class_path and factory_class_path must resolve to a concrete creation contract.");
				return false;
			}
			Asset = Context.Services.CreateAsset(PackagePath, AssetName, AssetClass, FactoryClass, nullptr, Error, false);
			if (!Asset)
			{
				Out->SetStringField(TEXT("status"), TEXT("failed_create_asset"));
				return false;
			}
			AssetPath = Asset->GetPathName();
			Out->SetStringField(TEXT("created_asset_path"), AssetPath);
		}
		else
		{
			if (AssetPath.IsEmpty())
			{
				Out->SetStringField(TEXT("status"), TEXT("blocked_missing_asset_path"));
				Error = TEXT("asset_path is required.");
				return false;
			}
			Asset = Context.Services.LoadAsset(AssetPath, Error);
		}

		if (!RequireClass(Spec, Asset, Error))
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_asset_class_gate"));
			return false;
		}
		Out->SetObjectField(TEXT("before"), ObjectReadback(Asset));
		const FString BeforeHash = FingerprintObject(Asset);

		const FScopedTransaction Transaction(FText::Format(
			NSLOCTEXT("SOMOLMCP", "AnimationCompletionMutation", "SOMOLMCP animation tool: {0}"),
			FText::FromString(Spec.Name)));

		bool bOk = false;
		if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset);
			Spec.Action == EAction::BlendAdd || Spec.Action == EAction::BlendUpdate || Spec.Action == EAction::BlendRemove ||
			Spec.Action == EAction::BlendList || Spec.Action == EAction::BlendSnap || Spec.Action == EAction::BlendValidate)
		{
			bOk = BlendSpace && ExecuteBlendSpace(Spec, Context, Args, BlendSpace, Out, Error);
			if (!BlendSpace && Error.IsEmpty()) Error = TEXT("BlendSpace action requires UBlendSpace.");
		}
		else if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset);
			Spec.Action == EAction::RootMotionSet || Spec.Action == EAction::RootMotionInspect ||
			Spec.Action == EAction::AdditiveConfigure || Spec.Action == EAction::CompressionSet ||
			Spec.Action == EAction::CompressionApply || Spec.Action == EAction::Resample ||
			Spec.Action == EAction::Crop || Spec.Action == EAction::TimeStretch ||
			Spec.Action == EAction::DistanceCurveGenerate || Spec.Action == EAction::DistanceCurveValidate)
		{
			bOk = Sequence && ExecuteAnimSequence(Spec, Context, Args, Sequence, Out, Error);
			if (!Sequence && Error.IsEmpty()) Error = TEXT("Animation-sequence action requires UAnimSequence.");
		}
		else if (Spec.Action == EAction::GraphNodeAdd || Spec.Action == EAction::GraphNodeRemove || Spec.Action == EAction::GraphConnect)
		{
			bOk = ExecuteGraphAction(Spec, Context, Args, Asset, Out, Error);
		}
		else
		{
			bOk = ExecuteGeneric(Spec, Context, Args, Asset, Out, Error);
		}

		Out->SetObjectField(TEXT("after"), ObjectReadback(Asset));
		const FString AfterHash = FingerprintObject(Asset);
		Out->SetStringField(TEXT("before_fingerprint"), BeforeHash);
		Out->SetStringField(TEXT("after_fingerprint"), AfterHash);
		Out->SetBoolField(TEXT("mutation_observed"), BeforeHash != AfterHash);
		Out->SetBoolField(TEXT("success"), bOk);
		Out->SetStringField(TEXT("status"), bOk ? TEXT("succeeded") : TEXT("failed"));
		if (!bOk)
		{
			if (Error.IsEmpty()) Error = TEXT("Native animation operation failed without a verifiable postcondition.");
			return false;
		}

		bool bVerified = false;
		Out->TryGetBoolField(TEXT("verified"), bVerified);
		if (!bVerified)
		{
			Out->SetStringField(TEXT("status"), TEXT("failed_readback_gate"));
			Out->SetBoolField(TEXT("success"), false);
			Error = TEXT("Operation did not produce a positive writer/readback verification receipt.");
			return false;
		}
		Summary = FString::Printf(TEXT("%s completed through native C++ with verified readback."), Spec.Name);
		return true;
	}

	static void CloseObjectSchemasRecursively(const TSharedRef<FJsonObject>& Schema)
	{
		FString Type;
		if (Schema->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object"))
		{
			Schema->SetBoolField(TEXT("additionalProperties"), false);
		}

		const TSharedPtr<FJsonObject>* PropertySchemas = nullptr;
		if (Schema->TryGetObjectField(TEXT("properties"), PropertySchemas) && PropertySchemas && PropertySchemas->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertySchemas)->Values)
			{
				const TSharedPtr<FJsonObject> Child = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (Child.IsValid())
				{
					CloseObjectSchemasRecursively(Child.ToSharedRef());
				}
			}
		}

		const TSharedPtr<FJsonObject>* ItemSchema = nullptr;
		if (Schema->TryGetObjectField(TEXT("items"), ItemSchema) && ItemSchema && ItemSchema->IsValid())
		{
			CloseObjectSchemasRecursively((*ItemSchema).ToSharedRef());
		}
	}

	static TSharedRef<FJsonObject> Schema(const FSpec& Spec)
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties;
		TArray<FString> Required;
		auto Add = [&Properties, &Required](const TCHAR* Name, const TSharedRef<FJsonObject>& Field, const bool bRequired = false)
		{
			Properties.Add(Name, Field);
			if (bRequired) Required.Add(Name);
		};
		auto AddString = [&Add](const TCHAR* Name, const bool bRequired = false, const TCHAR* Description = TEXT(""))
		{
			Add(Name, FSololmcpSchemaBuilder::String(Description), bRequired);
		};
		auto AddInteger = [&Add](const TCHAR* Name, const bool bRequired = false)
		{
			Add(Name, FSololmcpSchemaBuilder::Integer(), bRequired);
		};
		auto AddNumber = [&Add](const TCHAR* Name, const bool bRequired = false)
		{
			Add(Name, FSololmcpSchemaBuilder::Number(), bRequired);
		};
		auto AddBoolean = [&Add](const TCHAR* Name, const bool bRequired = false)
		{
			Add(Name, FSololmcpSchemaBuilder::Boolean(), bRequired);
		};

		if (Spec.bMutation)
		{
			AddBoolean(TEXT("execute"), true);
			AddBoolean(TEXT("save"));
		}
		if (Spec.Action == EAction::CreateAsset)
		{
			AddString(TEXT("package_path"), true, TEXT("Destination /Game package path."));
			AddString(TEXT("asset_name"), true);
			AddString(TEXT("asset_class_path"));
			AddString(TEXT("factory_class_path"));
		}
		else
		{
			AddString(TEXT("asset_path"), true, TEXT("Target Unreal asset object path."));
		}

		auto AddSampleValue = [&Add]()
		{
			Add(TEXT("sample_value"), FSololmcpSchemaBuilder::Object({
				{TEXT("x"), FSololmcpSchemaBuilder::Number()},
				{TEXT("y"), FSololmcpSchemaBuilder::Number()},
				{TEXT("z"), FSololmcpSchemaBuilder::Number()}
			}, {TEXT("x"), TEXT("y"), TEXT("z")}, TEXT("BlendSpace sample coordinates."), false), true);
		};

		switch (Spec.Action)
		{
		case EAction::BlendAdd:
			AddString(TEXT("source_asset_path"), true);
			AddSampleValue();
			break;
		case EAction::BlendUpdate:
			AddInteger(TEXT("sample_index"), true);
			AddSampleValue();
			break;
		case EAction::BlendRemove:
			AddInteger(TEXT("sample_index"), true);
			break;
		case EAction::GraphNodeAdd:
			if (FString(Spec.Family).Equals(TEXT("animnext"), ESearchCase::IgnoreCase))
			{
				AddString(TEXT("graph_name"), true);
				AddString(TEXT("unit_struct_path"), true);
				AddString(TEXT("method_name"));
				AddString(TEXT("node_name"));
			}
			else
			{
				AddString(TEXT("graph_name"), true);
				AddString(TEXT("node_class_path"), true);
			}
			AddNumber(TEXT("position_x"));
			AddNumber(TEXT("position_y"));
			break;
		case EAction::GraphNodeRemove:
			AddString(TEXT("graph_name"), true);
			AddString(TEXT("node_id"), true);
			break;
		case EAction::GraphConnect:
			AddString(TEXT("graph_name"), true);
			AddString(TEXT("source_pin_path"), true);
			AddString(TEXT("target_pin_path"), true);
			break;
		case EAction::RootMotionSet:
			AddBoolean(TEXT("enabled"), true);
			AddBoolean(TEXT("force_root_lock"));
			AddInteger(TEXT("root_lock_type"));
			break;
		case EAction::AdditiveConfigure:
			AddInteger(TEXT("additive_type"), true);
			AddInteger(TEXT("base_pose_type"), true);
			break;
		case EAction::CompressionSet:
			AddString(TEXT("bone_compression_settings_path"));
			AddString(TEXT("curve_compression_settings_path"));
			break;
		case EAction::CompressionApply:
			AddString(TEXT("bone_compression_settings_path"));
			break;
		case EAction::Resample:
			AddInteger(TEXT("frame_rate_numerator"), true);
			AddInteger(TEXT("frame_rate_denominator"));
			break;
		case EAction::Crop:
			AddInteger(TEXT("start_frame"), true);
			AddInteger(TEXT("end_frame"), true);
			break;
		case EAction::TimeStretch:
			AddNumber(TEXT("rate_scale"), true);
			break;
		case EAction::DistanceCurveGenerate:
			AddString(TEXT("curve_name"));
			Add(TEXT("times"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number()), true);
			Add(TEXT("values"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number()), true);
			break;
		case EAction::DistanceCurveValidate:
			AddString(TEXT("curve_name"));
			break;
		default:
			break;
		}

		if (Spec.Action == EAction::SemanticFailClosed)
		{
			const FString Name(Spec.Name);
			if (Name == TEXT("animbp_layer_add_native") || Name == TEXT("animbp_layer_remove_native"))
			{
				AddString(TEXT("layer_name"), true);
			}
			else if (Name == TEXT("animbp_layer_weight_set_native"))
			{
				AddString(TEXT("node_id"), true); AddNumber(TEXT("weight"), true);
			}
			else if (Name == TEXT("animbp_cached_pose_rename_native"))
			{
				AddString(TEXT("node_id"), true); AddString(TEXT("new_name"), true);
			}
			else if (Name == TEXT("animbp_linked_graph_bind_native"))
			{
				AddString(TEXT("node_id"), true); AddString(TEXT("linked_graph_path"), true);
			}
			else if (Name == TEXT("animbp_sync_group_set_native"))
			{
				AddString(TEXT("node_id"), true); AddString(TEXT("sync_group_name"), true); AddInteger(TEXT("sync_group_role"), true);
			}
			else if (Name == TEXT("animbp_sync_group_inspect_native"))
			{
				AddString(TEXT("graph_name"));
			}
			else if (Name == TEXT("animbp_state_alias_bind_native"))
			{
				AddString(TEXT("node_id"), true);
				Add(TEXT("state_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String()), true);
			}
			else if (Name == TEXT("animbp_transition_priority_set_native"))
			{
				AddString(TEXT("node_id"), true); AddInteger(TEXT("priority"), true);
			}
			else if (Name == TEXT("animbp_transition_blend_profile_set_native"))
			{
				AddString(TEXT("node_id"), true); AddString(TEXT("blend_profile_path"), true);
			}
			else if (Name == TEXT("animbp_transition_interrupt_rule_set_native"))
			{
				AddString(TEXT("node_id"), true); AddString(TEXT("interrupt_rule"), true);
			}
			else if (Name == TEXT("animnext_variable_add_native"))
			{
				AddString(TEXT("variable_name"), true); AddString(TEXT("type_name"), true); AddString(TEXT("default_value"));
			}
			else if (Name == TEXT("animnext_variable_set_default_native"))
			{
				AddString(TEXT("variable_name"), true); AddString(TEXT("default_value"), true);
			}
			else if (Name == TEXT("animnext_graph_inspect_native"))
			{
				AddString(TEXT("graph_name"));
			}
			else if (Name == TEXT("pose_search_channel_add_native"))
			{
				AddString(TEXT("channel_class_path"), true); AddString(TEXT("channel_name"));
			}
			else if (Name == TEXT("pose_search_database_asset_add_native"))
			{
				AddString(TEXT("source_asset_path"), true);
			}
			else if (Name == TEXT("pose_search_database_asset_remove_native"))
			{
				AddInteger(TEXT("entry_index"), true);
			}
			else if (Name == TEXT("animation_mirror_native"))
			{
				AddString(TEXT("mirror_data_table_path"), true);
			}
			else if (Name == TEXT("motion_warping_window_add_native"))
			{
				AddString(TEXT("modifier_class_path"), true); AddNumber(TEXT("start_time"), true); AddNumber(TEXT("end_time"), true);
			}
			else if (Name == TEXT("motion_warping_window_remove_native"))
			{
				AddInteger(TEXT("window_index"), true);
			}
			else if (Name.Contains(TEXT("breakpoint_add")) || Name.Contains(TEXT("breakpoint_remove")))
			{
				AddString(TEXT("node_path"), true); AddInteger(TEXT("instruction_index"));
			}
			else if (Name.Contains(TEXT("watch_add")) || Name.Contains(TEXT("watch_remove")))
			{
				AddString(TEXT("pin_path"), true);
			}
			else if (Name == TEXT("rigvm_execution_trace_get_native"))
			{
				AddString(TEXT("trace_id"));
			}
			else if (Name == TEXT("control_rig_test_pose_set_native"))
			{
				AddString(TEXT("pose_name"), true);
			}
		}

		TSharedRef<FJsonObject> Result = FSololmcpSchemaBuilder::Object(
			Properties,
			Required,
			TEXT("Closed native animation contract. Unsupported or unverifiable semantics fail closed."),
			false);
		CloseObjectSchemasRecursively(Result);
		return Result;
	}

	static const TArray<FSpec>& Specs()
	{
		static const TArray<FSpec> Result = {
			{TEXT("blendspace_sample_add_native"), TEXT("blendspace"), TEXT("Add and verify a BlendSpace sample."), EAction::BlendAdd, TEXT("BlendSpace"), TEXT("Engine"), TEXT(""), TEXT(""), true, false},
			{TEXT("blendspace_sample_update_native"), TEXT("blendspace"), TEXT("Move and verify a BlendSpace sample."), EAction::BlendUpdate, TEXT("BlendSpace"), TEXT("Engine"), TEXT(""), TEXT(""), true, false},
			{TEXT("blendspace_sample_remove_native"), TEXT("blendspace"), TEXT("Remove and verify a BlendSpace sample."), EAction::BlendRemove, TEXT("BlendSpace"), TEXT("Engine"), TEXT(""), TEXT(""), true, false},
			{TEXT("blendspace_sample_list_native"), TEXT("blendspace"), TEXT("Read BlendSpace samples."), EAction::BlendList, TEXT("BlendSpace"), TEXT("Engine"), TEXT(""), TEXT(""), false, false},
			{TEXT("blendspace_samples_snap_to_grid_native"), TEXT("blendspace"), TEXT("Snap BlendSpace samples to the configured grid."), EAction::BlendSnap, TEXT("BlendSpace"), TEXT("Engine"), TEXT(""), TEXT(""), true, false},
			{TEXT("blendspace_samples_validate_native"), TEXT("blendspace"), TEXT("Validate sample assets, values, and overlap."), EAction::BlendValidate, TEXT("BlendSpace"), TEXT("Engine"), TEXT(""), TEXT(""), false, false},

			{TEXT("animbp_layer_add_native"), TEXT("animbp"), TEXT("Add an animation layer through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_layer_remove_native"), TEXT("animbp"), TEXT("Remove an animation layer through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_layer_weight_set_native"), TEXT("animbp"), TEXT("Set a named layer weight through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_cached_pose_add_native"), TEXT("animbp"), TEXT("Add a cached-pose node."), EAction::GraphNodeAdd, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_cached_pose_rename_native"), TEXT("animbp"), TEXT("Rename a cached pose through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_linked_graph_add_native"), TEXT("animbp"), TEXT("Add a linked-graph node."), EAction::GraphNodeAdd, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_linked_graph_bind_native"), TEXT("animbp"), TEXT("Bind a linked graph through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_linked_layer_add_native"), TEXT("animbp"), TEXT("Add a linked animation-layer node."), EAction::GraphNodeAdd, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_sync_group_set_native"), TEXT("animbp"), TEXT("Set sync-group semantics through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_sync_group_inspect_native"), TEXT("animbp"), TEXT("Inspect sync-group metadata through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), false, false},
			{TEXT("animbp_state_alias_add_native"), TEXT("animbp"), TEXT("Add a state-alias graph node."), EAction::GraphNodeAdd, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_state_alias_bind_native"), TEXT("animbp"), TEXT("Bind state aliases through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_transition_priority_set_native"), TEXT("animbp"), TEXT("Set transition priority through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_transition_blend_profile_set_native"), TEXT("animbp"), TEXT("Set a transition blend profile through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_transition_interrupt_rule_set_native"), TEXT("animbp"), TEXT("Set a transition interruption rule through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("animbp_state_machine_compile_receipt_native"), TEXT("animbp"), TEXT("Compile an AnimBP and return compiler diagnostics and status."), EAction::CompileReceipt, TEXT("AnimBlueprint"), TEXT("AnimGraph,AnimationBlueprintEditor"), TEXT(""), TEXT(""), true, true},

			{TEXT("animnext_graph_create_native"), TEXT("animnext"), TEXT("Create and verify a native AnimNext/UAF module asset through its factory."), EAction::CreateAsset, TEXT("AnimNext"), TEXT("UAF,UAFEditor"), TEXT("/Script/UAF.AnimNextModule"), TEXT("/Script/UAFEditor.AnimNextModuleFactory"), true, false},
			{TEXT("animnext_variable_add_native"), TEXT("animnext"), TEXT("Add an AnimNext variable through its native controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimNext"), TEXT("UAF,UAFUncookedOnly"), TEXT(""), TEXT(""), true, true},
			{TEXT("animnext_variable_set_default_native"), TEXT("animnext"), TEXT("Set an AnimNext variable default through its native controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimNext"), TEXT("UAF,UAFUncookedOnly"), TEXT(""), TEXT(""), true, true},
			{TEXT("animnext_unit_node_add_native"), TEXT("animnext"), TEXT("Add and read back an AnimNext RigVM unit node through URigVMController."), EAction::GraphNodeAdd, TEXT("AnimNext"), TEXT("UAF,UAFUncookedOnly,RigVMDeveloper"), TEXT(""), TEXT(""), true, false},
			{TEXT("animnext_connect_pins_native"), TEXT("animnext"), TEXT("Connect and read back AnimNext pins through URigVMController."), EAction::GraphConnect, TEXT("AnimNext"), TEXT("UAF,UAFUncookedOnly,RigVMDeveloper"), TEXT(""), TEXT(""), true, false},
			{TEXT("animnext_compile_validate_native"), TEXT("animnext"), TEXT("Compile AnimNext only when a native boolean result is exposed; otherwise fail-closed."), EAction::CompileReceipt, TEXT("AnimNext"), TEXT("UAF,UAFUncookedOnly"), TEXT(""), TEXT(""), true, true},
			{TEXT("animnext_graph_inspect_native"), TEXT("animnext"), TEXT("Inspect an AnimNext graph through its native controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimNext"), TEXT("UAF"), TEXT(""), TEXT(""), false, false},

			{TEXT("pose_search_schema_create_native"), TEXT("pose_search"), TEXT("Create a PoseSearch schema asset."), EAction::CreateAsset, TEXT("PoseSearchSchema"), TEXT("PoseSearch,PoseSearchEditor"), TEXT("/Script/PoseSearch.PoseSearchSchema"), TEXT("/Script/PoseSearchEditor.PoseSearchSchemaFactory"), true, false},
			{TEXT("pose_search_channel_add_native"), TEXT("pose_search"), TEXT("Add a PoseSearch channel through a specialized editor adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("PoseSearchSchema"), TEXT("PoseSearch,PoseSearchEditor"), TEXT(""), TEXT(""), true, false},
			{TEXT("pose_search_database_create_native"), TEXT("pose_search"), TEXT("Create a PoseSearch database asset."), EAction::CreateAsset, TEXT("PoseSearchDatabase"), TEXT("PoseSearch,PoseSearchEditor"), TEXT("/Script/PoseSearch.PoseSearchDatabase"), TEXT("/Script/PoseSearchEditor.PoseSearchDatabaseFactory"), true, false},
			{TEXT("pose_search_database_asset_add_native"), TEXT("pose_search"), TEXT("Add a PoseSearch database asset through a specialized editor adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("PoseSearchDatabase"), TEXT("PoseSearch,PoseSearchEditor"), TEXT(""), TEXT(""), true, false},
			{TEXT("pose_search_database_asset_remove_native"), TEXT("pose_search"), TEXT("Remove a PoseSearch database asset through a specialized editor adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("PoseSearchDatabase"), TEXT("PoseSearch,PoseSearchEditor"), TEXT(""), TEXT(""), true, false},
			{TEXT("pose_search_database_build_index_native"), TEXT("pose_search"), TEXT("Build a PoseSearch index only when a native boolean result is exposed; otherwise fail-closed."), EAction::CompileReceipt, TEXT("PoseSearchDatabase"), TEXT("PoseSearch,PoseSearchEditor"), TEXT(""), TEXT(""), true, true},
			{TEXT("motion_matching_validate_native"), TEXT("motion_matching"), TEXT("Validate motion matching through a specialized PoseSearch adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("PoseSearchDatabase"), TEXT("PoseSearch"), TEXT(""), TEXT(""), false, false},

			{TEXT("animation_root_motion_set_native"), TEXT("animation_processing"), TEXT("Set root-motion mode and lock settings."), EAction::RootMotionSet, TEXT("AnimSequence"), TEXT("Engine,AnimationBlueprintLibrary"), TEXT(""), TEXT(""), true, false},
			{TEXT("animation_root_motion_inspect_native"), TEXT("animation_processing"), TEXT("Read root-motion settings."), EAction::RootMotionInspect, TEXT("AnimSequence"), TEXT("Engine,AnimationBlueprintLibrary"), TEXT(""), TEXT(""), false, false},
			{TEXT("animation_additive_configure_native"), TEXT("animation_processing"), TEXT("Configure additive type and base-pose type."), EAction::AdditiveConfigure, TEXT("AnimSequence"), TEXT("Engine,AnimationBlueprintLibrary"), TEXT(""), TEXT(""), true, false},
			{TEXT("animation_compression_set_native"), TEXT("animation_processing"), TEXT("Set bone/curve compression settings."), EAction::CompressionSet, TEXT("AnimSequence"), TEXT("Engine,AnimationBlueprintLibrary"), TEXT(""), TEXT(""), true, false},
			{TEXT("animation_compression_apply_native"), TEXT("animation_processing"), TEXT("Apply native animation compression."), EAction::CompressionApply, TEXT("AnimSequence"), TEXT("UnrealEd"), TEXT(""), TEXT(""), true, false},
			{TEXT("animation_resample_native"), TEXT("animation_processing"), TEXT("Resample animation data to a new frame rate."), EAction::Resample, TEXT("AnimSequence"), TEXT("Engine"), TEXT(""), TEXT(""), true, false},
			{TEXT("animation_crop_native"), TEXT("animation_processing"), TEXT("Crop animation data to a verified frame range."), EAction::Crop, TEXT("AnimSequence"), TEXT("Engine"), TEXT(""), TEXT(""), true, false},
			{TEXT("animation_mirror_native"), TEXT("animation_processing"), TEXT("Mirror animation through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimSequence"), TEXT("Engine"), TEXT(""), TEXT(""), true, false},
			{TEXT("animation_time_stretch_native"), TEXT("animation_processing"), TEXT("Set time-stretch rate scale with readback."), EAction::TimeStretch, TEXT("AnimSequence"), TEXT("AnimationBlueprintLibrary"), TEXT(""), TEXT(""), true, false},
			{TEXT("motion_warping_window_add_native"), TEXT("motion_warping"), TEXT("Add a MotionWarping window through a specialized notify adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimSequence"), TEXT("MotionWarping"), TEXT(""), TEXT(""), true, false},
			{TEXT("motion_warping_window_remove_native"), TEXT("motion_warping"), TEXT("Remove a MotionWarping window through a specialized notify adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("AnimSequence"), TEXT("MotionWarping"), TEXT(""), TEXT(""), true, false},
			{TEXT("distance_matching_curve_generate_native"), TEXT("distance_matching"), TEXT("Generate a distance matching float curve."), EAction::DistanceCurveGenerate, TEXT("AnimSequence"), TEXT("AnimationBlueprintLibrary"), TEXT(""), TEXT(""), true, false},
			{TEXT("distance_matching_validate_native"), TEXT("distance_matching"), TEXT("Validate distance matching curve keys."), EAction::DistanceCurveValidate, TEXT("AnimSequence"), TEXT("AnimationBlueprintLibrary"), TEXT(""), TEXT(""), false, false},

			{TEXT("control_rig_debug_breakpoint_add_native"), TEXT("control_rig_debug"), TEXT("Add a RigVM breakpoint through a native debug controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true},
			{TEXT("control_rig_debug_breakpoint_remove_native"), TEXT("control_rig_debug"), TEXT("Remove a RigVM breakpoint through a native debug controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true},
			{TEXT("control_rig_debug_breakpoint_clear_native"), TEXT("control_rig_debug"), TEXT("Clear RigVM breakpoints through a native debug controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true},
			{TEXT("control_rig_debug_watch_add_native"), TEXT("control_rig_debug"), TEXT("Add a RigVM watched pin through a native debug controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true},
			{TEXT("control_rig_debug_watch_remove_native"), TEXT("control_rig_debug"), TEXT("Remove a RigVM watched pin through a native debug controller; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true},
			{TEXT("rigvm_execution_trace_get_native"), TEXT("control_rig_debug"), TEXT("Read RigVM execution trace data through a native debug adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), false, false},
			{TEXT("control_rig_test_pose_set_native"), TEXT("control_rig_test_pose"), TEXT("Set a ControlRig test pose through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true},
			{TEXT("control_rig_test_pose_reset_native"), TEXT("control_rig_test_pose"), TEXT("Reset a ControlRig test pose through a specialized native adapter; currently fail-closed."), EAction::SemanticFailClosed, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true},
			{TEXT("control_rig_compile_receipt_native"), TEXT("control_rig_debug"), TEXT("Compile ControlRig/RigVM and return compiler diagnostics and status."), EAction::CompileReceipt, TEXT("ControlRigBlueprint"), TEXT("ControlRigDeveloper,RigVMDeveloper"), TEXT(""), TEXT(""), true, true}
		};
		return Result;
	}
}

void RegisterAnimationCompletionTools(FSololmcpToolRegistry& Registry)
{
	for (const AnimationCompletion::FSpec& Spec : AnimationCompletion::Specs())
	{
		FSololmcpToolDefinition Definition;
		Definition.Name = Spec.Name;
		Definition.Description = Spec.Description;
		Definition.InputSchema = AnimationCompletion::Schema(Spec);
		Definition.bUsesExternalPython = false;
		Definition.Execute = [Spec](
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			return AnimationCompletion::Run(Spec, Context, Arguments, OutStructured, OutSummary, OutError);
		};
		Registry.Register(Definition);
	}
}
}
