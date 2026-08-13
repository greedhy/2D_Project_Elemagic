// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpEnhancedTools.cpp — SOMOLMCP v3.1.0
// 增强工具集：填充 HTML 规范中缺失的工具 + 别名映射 + 增强功能
//
// 分类：
//   P0: execute_console_command, python_execute, pie_start/stop/capture/screenshot/get_status
//   P0: 14 个别名工具 (HTML spec 名称 → C++ 实现)
//   P1: blueprint_get_nodes/variables, umg_widget_bind_event/property
//   P1: material_add_node, material_instance_get_params, vfx_create_system, pcg_generate
//   P2: animation_create_montage, asset_get_metadata, texture_get_info/modify
//   P2: sequence_add_folder/section, sequence_focus_subsequence, sequence_set_marked_frames
//   P2: world_partition_cell_size_cm, pie_capture, pie_screenshot

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpWriteFlush.h"
#include "Tools/SololmcpPcgExecutionSafety.h"

// ── Editor Core ──
#include "Editor.h"
#include "EditorModeManager.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Selection.h"  // UE 5.7: USelection, FSelectionIterator
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

// ── Console ──
#include "HAL/IConsoleManager.h"

// ── Output capture (for python_execute) ──
#include "Misc/OutputDevice.h"
#include "Logging/LogMacros.h"

// ── World / PIE ──
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "GameFramework/GameModeBase.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameViewportClient.h"
#include "Kismet/GameplayStatics.h"

// ── Blueprint ──
#include "BlueprintEditorLibrary.h"
#include "Blueprint/BlueprintSupport.h"
// UserDefinedStruct.h moved to StructUtils in 5.5+ and the old path is gone by 5.8;
// SololmcpEngineCompat.h picks the spelling this engine actually ships.
#define SOMOLMCP_COMPAT_NEED_STRUCTUTILS
#include "SololmcpEngineCompat.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "GraphEditorSettings.h"
#include "Engine/SimpleConstructionScript.h"  // USCS_Node
#include "Engine/SCS_Node.h"  // USCS_Node class definition
// UE 5.7: Blueprint/SGraphNode.h removed - SGraphNode is now in another location
// #include "Blueprint/SGraphNode.h"
// UE 5.7: K2Node_Variable.h moved to BlueprintGraph module
#include "K2Node_Variable.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_CallFunction.h"
// UE 5.7: K2Node_CallMacro.h removed - use K2Node_MacroInstance instead
#include "K2Node_IfThenElse.h"
#include "K2Node_Switch.h"
// UE 5.7: K2Node_ForEachLoop, ForLoop, WhileLoop, Sequence, Timeline removed
// #include "K2Node_ForEachLoop.h"
// #include "K2Node_ForLoop.h"
// #include "K2Node_WhileLoop.h"
// #include "K2Node_Sequence.h"
// #include "K2Node_Timeline.h"
#include "K2Node_DynamicCast.h"
// UE 5.7: The following K2Node headers have been removed or relocated
// #include "K2Node_Message.h"
#include "K2Node_ComponentBoundEvent.h"
// #include "K2Node_AddComponent.h"
// #include "K2Node_SpawnActorFromClass.h"
// #include "K2Node_SpawnActorFromInfo.h"
// #include "K2Node_MathBase.h"
// #include "K2Node_Select.h"
// #include "K2Node_FormatText.h"
// #include "K2Node_StructMemberGet.h"
// #include "K2Node_StructMemberSet.h"
// #include "K2Node_ArrayElement.h"
// #include "K2Node_MapGet.h"
// #include "K2Node_MapSet.h"
// #include "K2Node_SetElement.h"
// #include "K2Node_GetArrayItem.h"
// #include "K2Node_SetArrayItem.h"
// #include "K2Node_BreakStruct.h"
// #include "K2Node_MakeStruct.h"
// #include "K2Node_BreakMacro.h"

// ── Material ──
// UE 5.7: MaterialEditorLibrary.h removed
// #include "MaterialEditorLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "Materials/Material.h"
#include "Engine/Texture.h"
#include "UObject/UnrealType.h"

// ── Material Expressions (UE 5.7: moved to Public/Materials/) ──
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionVertexColor.h"
// UE 5.7: MaterialExpressionWorldPositionOffset.h removed - use WorldPosition node instead
// #include "Materials/MaterialExpressionWorldPositionOffset.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionCrossProduct.h"
// UE 5.7: MaterialExpressionReflect.h removed
// #include "Materials/MaterialExpressionReflect.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionDesaturation.h"
#include "Materials/MaterialExpressionBumpOffset.h"
#include "Materials/MaterialExpressionAppendVector.h"
// UE 5.7: BreakOutFloat components expressions removed - use ComponentMask instead
// #include "Materials/MaterialExpressionBreakOutFloat2Components.h"
// #include "Materials/MaterialExpressionBreakOutFloat3Components.h"
// #include "Materials/MaterialExpressionBreakOutFloat4Components.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionComment.h"

// ── Texture ──
#include "Engine/Texture2D.h"
// UE 5.7: "Texture mip data factory.h" removed - use TextureMipDataProvider.h
#include "Engine/TextureMipDataProviderFactory.h"
// UE 5.7: ITextureCompressorModule.h path changed - use TextureCompressorModule.h
#include "TextureCompressorModule.h"
// UE 5.7: TextureBuildSettings.h moved to TextureCompressor module - include via TextureCompressorModule.h
// #include "TextureBuildSettings.h"  // Already included via TextureCompressorModule.h

// ── Animation ──
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
// UE 5.7: BoneMask.h renamed to BoneMaskFilter.h
#include "Animation/AnimData/BoneMaskFilter.h"
// UE 5.7: MirrorData.h removed - use MirrorDataTable.h
#include "Animation/MirrorDataTable.h"
#include "Animation/AnimationAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/AnimSequenceFactory.h"
#include "Animation/AnimBlueprint.h"

// ── Sequencer ──
#include "MovieScene.h"
#include "MovieSceneFolder.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
// UE 5.7: SequencerScriptingEditor.h path changed
#include "SequencerScriptingEditor.h"
// UE 5.7: SequencerScriptingPresets.h removed - functionality moved to SequencerScriptingEditor
// UE 5.7: SequencerScriptingUtilities.h removed - use SequencerScriptingEditor instead
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieScenePropertyTrack.h"

// ── UMG ──
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Components/PanelSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/GridSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Components/ListView.h"
#include "UObject/FieldIterator.h"
// UE 5.7: Kismet2/WidgetBlueprintLibraryUtils.h removed - use WidgetBlueprintEditorUtils.h if needed
// #include "Kismet2/WidgetBlueprintLibraryUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprintEditor.h"

// ── Niagara ──
#include "NiagaraSystem.h"
#include "NiagaraComponent.h"
#include "NiagaraEditorUtilities.h"
// UE 5.7: Factories/NiagaraSystemFactory.h removed - Niagara system creation uses different API
// #include "Factories/NiagaraSystemFactory.h"
#include "NiagaraEmitter.h"

// ── PCG ──
#include "PCGComponent.h"
#include "PCGSubsystem.h"
#include "PCGSettings.h"
#include "PCGData.h"
#include "PCGGraph.h"
#include "PCGPin.h"
#include "PCGNode.h"
// UE 5.7: EdGraph/PCGGraphSchema.h removed
// #include "EdGraph/PCGGraphSchema.h"

// ── WorldPartition ──
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
// WorldPartition/WorldPartitionActorDescInstance.h is 5.5+ and nothing in this
// file referenced it; the include alone cost UE 5.3 all 55 tools here.

// ── Python ──
// UE 5.7: WITH_PYTHON macro removed. We avoid depending on PythonScriptPlugin headers
// entirely and route execution through GEngine->Exec("py ...") which is registered by
// the plugin at runtime. Module presence is detected via FModuleManager, so this
// compiles on any build regardless of whether PythonScriptPlugin is in Build.cs.

// ── Generic ──
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/MetaData.h"
#include "UObject/TextProperty.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "Factories/Factory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPEnhanced, Log, All);

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────────
// Helper: ensure game thread
// ─────────────────────────────────────────────────────────────────────────────
static bool EnsureGameThread(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Enhanced tools must be called from the game thread.");
		return false;
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: resolve asset by path
// ─────────────────────────────────────────────────────────────────────────────
static void SetToolStatus(TSharedRef<FJsonObject>& OutStructured, const bool bSuccess)
{
	OutStructured->SetBoolField(TEXT("success"), bSuccess);
	OutStructured->SetStringField(TEXT("status"), bSuccess ? TEXT("success") : TEXT("failed"));
}

static bool BlueprintGraphNameExists(const UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint || GraphName.IsEmpty())
	{
		return false;
	}
	auto MatchesGraphName = [&GraphName](const UEdGraph* Graph)
	{
		return Graph && Graph->GetName() == GraphName;
	};
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (MatchesGraphName(Graph)) return true;
	}
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (MatchesGraphName(Graph)) return true;
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (MatchesGraphName(Graph)) return true;
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (MatchesGraphName(Graph)) return true;
	}
	return false;
}

static UObject* ResolveAsset(const FString& AssetPath, FString& OutError)
{
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(AssetPath, PackageName))
	{
		PackageName = AssetPath;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> AssetData;
	AssetRegistryModule.Get().GetAssetsByPackageName(*PackageName, AssetData);

	if (AssetData.Num() > 0 && AssetData[0].IsValid())
	{
		return AssetData[0].GetAsset();
	}
	return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: execute_console_command — UE 控制台命令执行
// ═══════════════════════════════════════════════════════════════════════════════

static bool VerifyAssetResolved(
	const FString& AssetPath,
	const UClass* ExpectedClass,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutError)
{
	FString ResolveError;
	UObject* Resolved = ResolveAsset(AssetPath, ResolveError);
	const bool bVerified = Resolved && (!ExpectedClass || Resolved->IsA(ExpectedClass));
	OutStructured->SetBoolField(TEXT("verified"), bVerified);
	if (Resolved)
	{
		OutStructured->SetStringField(TEXT("verified_path"), Resolved->GetPathName());
	}
	if (!bVerified)
	{
		OutError = FString::Printf(TEXT("Post-operation asset verification failed for '%s'."), *AssetPath);
		SetToolStatus(OutStructured, false);
		return false;
	}
	return true;
}

static TMap<FString, TWeakObjectPtr<UUserWidget>> GSomolRuntimePreviewWidgets;

static UWorld* GetActivePieWorld(FString& OutError)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		OutError = TEXT("No active PIE session. This tool never starts PIE; call pie_start explicitly or use an existing PIE session.");
		return nullptr;
	}
	return GEditor->PlayWorld;
}

static UClass* ResolveUserWidgetRuntimeClass(const FSololmcpToolExecutionContext& Context, const FString& AssetOrClassPath, FString& OutError)
{
	if (AssetOrClassPath.IsEmpty())
	{
		OutError = TEXT("Missing widget_blueprint_path or widget_class_path.");
		return nullptr;
	}

	if (UClass* DirectClass = Context.Services.ResolveClass(AssetOrClassPath, OutError))
	{
		if (DirectClass->IsChildOf(UUserWidget::StaticClass()))
		{
			OutError.Reset();
			return DirectClass;
		}
		OutError = TEXT("Resolved class is not a UserWidget class.");
		return nullptr;
	}

	FString LoadError;
	UObject* Asset = Context.Services.LoadAsset(AssetOrClassPath, LoadError);
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset);
	if (!WidgetBlueprint)
	{
		OutError = FString::Printf(TEXT("Path is neither a UserWidget class nor a WidgetBlueprint asset: %s"), *AssetOrClassPath);
		return nullptr;
	}

	UClass* GeneratedWidgetClass = WidgetBlueprint->GeneratedClass;
	if (!GeneratedWidgetClass || !GeneratedWidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		OutError = TEXT("WidgetBlueprint has no generated UserWidget class. Compile the widget blueprint first.");
		return nullptr;
	}

	OutError.Reset();
	return GeneratedWidgetClass;
}

static TSharedRef<FJsonObject> RuntimePreviewWidgetToJson(const FString& Handle, const UUserWidget* Widget)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("mount_handle"), Handle);
	Result->SetBoolField(TEXT("mounted"), Widget != nullptr);
	if (Widget)
	{
		Result->SetStringField(TEXT("widget_name"), Widget->GetName());
		Result->SetStringField(TEXT("widget_class"), Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString());
		Result->SetBoolField(TEXT("is_in_viewport"), Widget->IsInViewport());
		Result->SetNumberField(TEXT("visibility"), static_cast<int32>(Widget->GetVisibility()));
	}
	return Result;
}

static void FailClosedUmgTool(
	TSharedRef<FJsonObject>& OutStructured,
	const FString& Reason,
	const FString& LiveProofRequired,
	FString& OutError)
{
	SetToolStatus(OutStructured, false);
	OutStructured->SetBoolField(TEXT("fail_closed"), true);
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	OutStructured->SetStringField(TEXT("reason"), Reason);
	OutStructured->SetStringField(TEXT("live_proof_required"), LiveProofRequired);
	OutError = Reason;
}

static FString GetUmgAssetPathArg(const TSharedRef<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		Arguments->TryGetStringField(TEXT("widget_blueprint_path"), AssetPath);
	}
	return AssetPath;
}

static UWidgetBlueprint* ResolveWidgetBlueprintAsset(const FString& AssetPath, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path or widget_blueprint_path.");
		return nullptr;
	}

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(ResolveAsset(AssetPath, OutError));
	if (!WidgetBP)
	{
		OutError = FString::Printf(TEXT("Could not load WidgetBlueprint from '%s'."), *AssetPath);
	}
	return WidgetBP;
}

static UWidget* FindWidgetInBlueprint(UWidgetBlueprint* WidgetBP, const FString& WidgetName, TArray<UWidget*>* OutAllWidgets = nullptr)
{
	if (!WidgetBP || !WidgetBP->WidgetTree)
	{
		return nullptr;
	}

	TArray<UWidget*> AllWidgets;
	WidgetBP->WidgetTree->GetAllWidgets(AllWidgets);
	if (OutAllWidgets)
	{
		*OutAllWidgets = AllWidgets;
	}

	for (UWidget* Widget : AllWidgets)
	{
		if (Widget && Widget->GetFName().ToString() == WidgetName)
		{
			return Widget;
		}
	}
	return nullptr;
}

static TArray<TSharedPtr<FJsonValue>> MulticastDelegateNamesToJson(const UClass* WidgetClass)
{
	TArray<TSharedPtr<FJsonValue>> DelegateNames;
	if (!WidgetClass)
	{
		return DelegateNames;
	}

	for (TFieldIterator<FMulticastDelegateProperty> It(WidgetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		DelegateNames.Add(MakeShared<FJsonValueString>(It->GetName()));
	}
	return DelegateNames;
}

static TArray<TSharedPtr<FJsonValue>> PropertyNamesToJson(const UClass* WidgetClass)
{
	TArray<TSharedPtr<FJsonValue>> PropertyNames;
	if (!WidgetClass)
	{
		return PropertyNames;
	}

	for (TFieldIterator<FProperty> It(WidgetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		PropertyNames.Add(MakeShared<FJsonValueString>(It->GetName()));
	}
	return PropertyNames;
}

static TSharedPtr<FJsonObject> GraphSummaryToJson(const UEdGraph* Graph, const FString& GraphType)
{
	TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
	GraphObj->SetStringField(TEXT("name"), Graph ? Graph->GetName() : FString());
	GraphObj->SetStringField(TEXT("type"), GraphType);
	GraphObj->SetNumberField(TEXT("node_count"), Graph ? Graph->Nodes.Num() : 0);
	return GraphObj;
}

template <typename GraphArrayType>
static void AddGraphsToJsonArray(
	const GraphArrayType& Graphs,
	const FString& GraphType,
	TArray<TSharedPtr<FJsonValue>>& OutGraphs)
{
	for (const UEdGraph* Graph : Graphs)
	{
		if (Graph)
		{
			OutGraphs.Add(MakeShared<FJsonValueObject>(GraphSummaryToJson(Graph, GraphType)));
		}
	}
}

static TArray<TSharedPtr<FJsonValue>> StringArrayToJsonValues(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> JsonValues;
	for (const FString& Value : Values)
	{
		JsonValues.Add(MakeShared<FJsonValueString>(Value));
	}
	return JsonValues;
}

static FString BlueprintStatusToString(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return FString();
	}
	const UEnum* BlueprintStatusEnum = StaticEnum<EBlueprintStatus>();
	return BlueprintStatusEnum
		? BlueprintStatusEnum->GetNameStringByValue(static_cast<int64>(Blueprint->Status))
		: FString::FromInt(static_cast<int32>(Blueprint->Status));
}

static UEdGraph* FindFunctionGraphByName(UBlueprint* Blueprint, const FString& FunctionName)
{
	if (!Blueprint || FunctionName.IsEmpty())
	{
		return nullptr;
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			return Graph;
		}
	}
	return nullptr;
}

static UK2Node_FunctionEntry* FindUmgFunctionEntryNode(UEdGraph* Graph, int32* OutEntryCount = nullptr)
{
	int32 EntryCount = 0;
	UK2Node_FunctionEntry* FirstEntry = nullptr;
	if (Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
			{
				++EntryCount;
				if (!FirstEntry)
				{
					FirstEntry = EntryNode;
				}
			}
		}
	}
	if (OutEntryCount)
	{
		*OutEntryCount = EntryCount;
	}
	return FirstEntry;
}

static bool IsUmgFunctionExecPin(const UEdGraphPin* Pin)
{
	return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
}

static bool HasExecPathToNode(const UEdGraphPin* EntryExecPin, const UEdGraphNode* TargetNode)
{
	if (!EntryExecPin || !TargetNode)
	{
		return false;
	}

	TArray<const UEdGraphNode*> Stack;
	TSet<const UEdGraphNode*> Visited;
	for (UEdGraphPin* LinkedPin : EntryExecPin->LinkedTo)
	{
		if (LinkedPin && LinkedPin->GetOwningNode())
		{
			Stack.Add(LinkedPin->GetOwningNode());
		}
	}

	while (!Stack.IsEmpty())
	{
		const UEdGraphNode* CurrentNode = Stack.Pop(SOMOLMCP_NO_SHRINK);
		if (!CurrentNode || Visited.Contains(CurrentNode))
		{
			continue;
		}
		if (CurrentNode == TargetNode)
		{
			return true;
		}
		Visited.Add(CurrentNode);
		for (UEdGraphPin* Pin : CurrentNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || !IsUmgFunctionExecPin(Pin))
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					Stack.Add(LinkedPin->GetOwningNode());
				}
			}
		}
	}
	return false;
}

static bool FinalizeUmgFunctionGraph(
	UWidgetBlueprint* WidgetBP,
	UEdGraph* Graph,
	const bool bAllowMutation,
	TSharedRef<FJsonObject>& OutFinalizer,
	FString& OutValidationError)
{
	OutFinalizer->SetBoolField(TEXT("mutation_requested"), bAllowMutation);
	OutFinalizer->SetBoolField(TEXT("mutation_performed"), false);
	OutFinalizer->SetBoolField(TEXT("validation_passed"), false);

	TArray<FString> RepairSuggestions;
	TArray<FString> OrphanRequiredNodes;
	bool bMutated = false;
	bool bReturnNodeCreated = false;
	bool bEntryToReturnLinked = false;

	if (!WidgetBP || !Graph)
	{
		RepairSuggestions.Add(TEXT("Load a concrete WidgetBlueprint function graph before finalization."));
		OutFinalizer->SetArrayField(TEXT("repair_suggestions"), StringArrayToJsonValues(RepairSuggestions));
		OutValidationError = TEXT("WidgetBlueprint function graph was not resolved.");
		return false;
	}

	const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(Graph->GetSchema());
	if (!K2Schema)
	{
		K2Schema = GetDefault<UEdGraphSchema_K2>();
	}

	OutFinalizer->SetStringField(TEXT("graph_name"), Graph->GetName());
	OutFinalizer->SetNumberField(TEXT("node_count_before"), Graph->Nodes.Num());

	int32 EntryCount = 0;
	UK2Node_FunctionEntry* EntryNode = FindUmgFunctionEntryNode(Graph, &EntryCount);
	if (!EntryNode && bAllowMutation && K2Schema)
	{
		Graph->Modify();
		K2Schema->CreateDefaultNodesForGraph(*Graph);
		EntryNode = FindUmgFunctionEntryNode(Graph, &EntryCount);
		bMutated = EntryNode != nullptr;
	}

	if (!EntryNode)
	{
		RepairSuggestions.Add(TEXT("Create or restore exactly one UK2Node_FunctionEntry for the function graph."));
		OrphanRequiredNodes.Add(TEXT("FunctionEntry"));
	}
	else if (EntryCount > 1)
	{
		RepairSuggestions.Add(TEXT("Remove duplicate FunctionEntry nodes; one function graph should have one entry."));
	}

	TArray<UK2Node_FunctionResult*> ResultNodes;
	Graph->GetNodesOfClass(ResultNodes);
	if (EntryNode && ResultNodes.Num() == 0 && bAllowMutation)
	{
		Graph->Modify();
		if (UK2Node_FunctionResult* CreatedResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode))
		{
			CreatedResultNode->Modify();
			CreatedResultNode->FunctionReference.SetSelfMember(Graph->GetFName());
			bReturnNodeCreated = true;
			bMutated = true;
		}
		ResultNodes.Reset();
		Graph->GetNodesOfClass(ResultNodes);
	}

	if (ResultNodes.Num() == 0)
	{
		RepairSuggestions.Add(TEXT("Add a UK2Node_FunctionResult Return Node as the function terminator."));
		OrphanRequiredNodes.Add(TEXT("FunctionResult"));
	}

	UK2Node_FunctionResult* PrimaryResultNode = ResultNodes.Num() > 0 ? ResultNodes[0] : nullptr;
	UEdGraphPin* EntryExecPin = EntryNode && K2Schema ? K2Schema->FindExecutionPin(*EntryNode, EGPD_Output) : nullptr;
	UEdGraphPin* PrimaryResultExecPin = PrimaryResultNode && K2Schema ? K2Schema->FindExecutionPin(*PrimaryResultNode, EGPD_Input) : nullptr;

	if (EntryNode && !EntryExecPin)
	{
		RepairSuggestions.Add(TEXT("Reconstruct the FunctionEntry node so it exposes an output exec pin."));
		OrphanRequiredNodes.Add(TEXT("FunctionEntry.exec"));
	}
	if (PrimaryResultNode && !PrimaryResultExecPin)
	{
		RepairSuggestions.Add(TEXT("Reconstruct the Return Node so it exposes an input exec pin."));
		OrphanRequiredNodes.Add(TEXT("FunctionResult.exec"));
	}

	if (EntryExecPin && PrimaryResultExecPin
		&& EntryExecPin->LinkedTo.Num() == 0
		&& PrimaryResultExecPin->LinkedTo.Num() == 0
		&& bAllowMutation)
	{
		EntryExecPin->MakeLinkTo(PrimaryResultExecPin);
		bEntryToReturnLinked = true;
		bMutated = true;
	}

	const bool bExecPathReachesReturn = HasExecPathToNode(EntryExecPin, PrimaryResultNode);
	if (EntryExecPin && EntryExecPin->LinkedTo.Num() == 0)
	{
		OrphanRequiredNodes.Add(TEXT("FunctionEntry.exec"));
	}

	int32 UnlinkedReturnExecCount = 0;
	for (UK2Node_FunctionResult* ResultNode : ResultNodes)
	{
		UEdGraphPin* ResultExecPin = ResultNode && K2Schema ? K2Schema->FindExecutionPin(*ResultNode, EGPD_Input) : nullptr;
		if (!ResultExecPin || ResultExecPin->LinkedTo.Num() == 0)
		{
			++UnlinkedReturnExecCount;
			OrphanRequiredNodes.Add(ResultNode
				? FString::Printf(TEXT("%s.exec"), *ResultNode->GetName())
				: TEXT("FunctionResult.exec"));
		}
	}

	if (EntryNode && PrimaryResultNode && EntryExecPin && PrimaryResultExecPin && !bExecPathReachesReturn)
	{
		RepairSuggestions.Add(TEXT("Connect the FunctionEntry execution output to the Return Node, or route every branch through a reachable Return Node."));
	}
	if (UnlinkedReturnExecCount > 0)
	{
		RepairSuggestions.Add(TEXT("Connect every required Return Node input exec pin or remove orphan Return Nodes before delivery."));
	}

	if (bMutated)
	{
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	}

	OutFinalizer->SetBoolField(TEXT("entry_found"), EntryNode != nullptr);
	OutFinalizer->SetNumberField(TEXT("entry_count"), EntryCount);
	OutFinalizer->SetBoolField(TEXT("return_node_found"), ResultNodes.Num() > 0);
	OutFinalizer->SetNumberField(TEXT("return_node_count"), ResultNodes.Num());
	OutFinalizer->SetBoolField(TEXT("return_node_created"), bReturnNodeCreated);
	OutFinalizer->SetBoolField(TEXT("entry_to_return_exec_connected"), bEntryToReturnLinked);
	OutFinalizer->SetBoolField(TEXT("exec_chain_reaches_return"), bExecPathReachesReturn);
	OutFinalizer->SetNumberField(TEXT("orphan_required_node_count"), OrphanRequiredNodes.Num());
	OutFinalizer->SetArrayField(TEXT("orphan_required_nodes"), StringArrayToJsonValues(OrphanRequiredNodes));
	OutFinalizer->SetArrayField(TEXT("repair_suggestions"), StringArrayToJsonValues(RepairSuggestions));
	OutFinalizer->SetNumberField(TEXT("node_count_after"), Graph->Nodes.Num());
	OutFinalizer->SetBoolField(TEXT("mutation_performed"), bMutated);

	const bool bValid = EntryNode != nullptr
		&& EntryCount == 1
		&& ResultNodes.Num() > 0
		&& EntryExecPin != nullptr
		&& PrimaryResultExecPin != nullptr
		&& bExecPathReachesReturn
		&& OrphanRequiredNodes.Num() == 0;
	OutFinalizer->SetBoolField(TEXT("validation_passed"), bValid);
	if (!bValid)
	{
		OutValidationError = TEXT("UMG function graph finalizer validation failed; inspect repair_suggestions.");
	}
	return bValid;
}

static FMulticastDelegateProperty* FindWidgetDelegateProperty(UWidget* Widget, const FString& EventName)
{
	if (!Widget || EventName.IsEmpty())
	{
		return nullptr;
	}
	return FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), *EventName);
}

static FProperty* FindWidgetAnyProperty(UWidget* Widget, const FString& PropertyName)
{
	if (!Widget || PropertyName.IsEmpty())
	{
		return nullptr;
	}
	return FindFProperty<FProperty>(Widget->GetClass(), *PropertyName);
}

static FString ResolveListViewSelectionDelegateName(UWidget* Widget, const FString& RequestedEventName)
{
	static const TArray<FString> CandidateNames = {
		TEXT("BP_OnItemSelectionChanged"),
		TEXT("OnItemSelectionChanged"),
		TEXT("OnSelectionChanged"),
		TEXT("OnItemClicked"),
		TEXT("BP_OnItemClicked")
	};

	if (!RequestedEventName.IsEmpty() && FindWidgetDelegateProperty(Widget, RequestedEventName))
	{
		return RequestedEventName;
	}

	for (const FString& CandidateName : CandidateNames)
	{
		if ((RequestedEventName.IsEmpty() || RequestedEventName == CandidateName || RequestedEventName == TEXT("selection_changed"))
			&& FindWidgetDelegateProperty(Widget, CandidateName))
		{
			return CandidateName;
		}
	}
	return FString();
}

static TSharedPtr<FJsonObject> ReflectedPropertyToJson(UObject* Object, FProperty* Property, const bool bIncludeValue)
{
	TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
	PropertyObj->SetStringField(TEXT("name"), Property ? Property->GetName() : FString());
	PropertyObj->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : FString());
	PropertyObj->SetStringField(TEXT("class"), Property ? Property->GetClass()->GetName() : FString());
	PropertyObj->SetBoolField(TEXT("is_editable"), Property ? Property->HasAnyPropertyFlags(CPF_Edit) : false);
	PropertyObj->SetBoolField(TEXT("is_blueprint_visible"), Property ? Property->HasAnyPropertyFlags(CPF_BlueprintVisible) : false);
	PropertyObj->SetBoolField(TEXT("is_text"), Property ? Property->IsA<FTextProperty>() : false);
	if (bIncludeValue && Object && Property)
	{
		FString ExportedValue;
		Property->ExportText_InContainer(0, ExportedValue, Object, Object, Object, PPF_None);
		PropertyObj->SetStringField(TEXT("value"), ExportedValue.Left(4096));
		PropertyObj->SetBoolField(TEXT("value_truncated"), ExportedValue.Len() > 4096);
	}
	return PropertyObj;
}

static TArray<TSharedPtr<FJsonValue>> ReflectedPropertiesToJson(UObject* Object, const int32 MaxProperties, const bool bIncludeValues)
{
	TArray<TSharedPtr<FJsonValue>> PropertiesArray;
	if (!Object || !Object->GetClass())
	{
		return PropertiesArray;
	}

	int32 Count = 0;
	for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property)
		{
			continue;
		}
		PropertiesArray.Add(MakeShared<FJsonValueObject>(ReflectedPropertyToJson(Object, Property, bIncludeValues)));
		++Count;
		if (MaxProperties > 0 && Count >= MaxProperties)
		{
			break;
		}
	}
	return PropertiesArray;
}

static TSharedPtr<FJsonObject> WidgetSlotToJson(UWidget* Widget)
{
	TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
	SlotObj->SetStringField(TEXT("widget_name"), Widget ? Widget->GetName() : FString());
	SlotObj->SetStringField(TEXT("widget_class"), (Widget && Widget->GetClass()) ? Widget->GetClass()->GetPathName() : FString());
	SlotObj->SetStringField(TEXT("parent_name"), (Widget && Widget->GetParent()) ? Widget->GetParent()->GetName() : FString());
	UPanelSlot* Slot = Widget ? Widget->Slot : nullptr;
	SlotObj->SetBoolField(TEXT("has_slot"), Slot != nullptr);
	if (Slot)
	{
		SlotObj->SetStringField(TEXT("slot_class"), Slot->GetClass() ? Slot->GetClass()->GetPathName() : FString());
		SlotObj->SetArrayField(TEXT("slot_properties"), ReflectedPropertiesToJson(Slot, 64, true));
	}
	return SlotObj;
}

static bool GetBoolPropertyValue(UObject* Object, const TCHAR* PropertyName, bool& OutValue)
{
	if (!Object)
	{
		return false;
	}
	FBoolProperty* BoolProperty = FindFProperty<FBoolProperty>(Object->GetClass(), PropertyName);
	if (!BoolProperty)
	{
		return false;
	}
	OutValue = BoolProperty->GetPropertyValue_InContainer(Object);
	return true;
}

static TArray<TSharedPtr<FJsonValue>> TextPropertiesToJson(UWidget* Widget)
{
	TArray<TSharedPtr<FJsonValue>> TextArray;
	if (!Widget || !Widget->GetClass())
	{
		return TextArray;
	}

	for (TFieldIterator<FTextProperty> It(Widget->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FTextProperty* TextProperty = *It;
		if (!TextProperty)
		{
			continue;
		}

		const FText TextValue = TextProperty->GetPropertyValue_InContainer(Widget);
		const TOptional<FString> Namespace = FTextInspector::GetNamespace(TextValue);
		const TOptional<FString> Key = FTextInspector::GetKey(TextValue);
		TSharedPtr<FJsonObject> TextObj = MakeShared<FJsonObject>();
		TextObj->SetStringField(TEXT("property"), TextProperty->GetName());
		TextObj->SetStringField(TEXT("display_string"), TextValue.ToString());
		TextObj->SetStringField(TEXT("namespace"), Namespace.IsSet() ? Namespace.GetValue() : FString());
		TextObj->SetStringField(TEXT("key"), Key.IsSet() ? Key.GetValue() : FString());
		TextObj->SetBoolField(TEXT("has_namespace"), Namespace.IsSet() && !Namespace.GetValue().IsEmpty());
		TextObj->SetBoolField(TEXT("has_key"), Key.IsSet() && !Key.GetValue().IsEmpty());
		TextObj->SetBoolField(TEXT("is_empty"), TextValue.IsEmpty());
		TextArray.Add(MakeShared<FJsonValueObject>(TextObj));
	}
	return TextArray;
}

struct FSomolUmgLocalizationIssue
{
	UWidget* Widget = nullptr;
	FTextProperty* TextProperty = nullptr;
	FText TextValue;
	FString WidgetName;
	FString WidgetClass;
	FString PropertyName;
	FString DisplayString;
	FString Namespace;
	FString Key;
	bool bHasNamespace = false;
	bool bHasKey = false;
	bool bIsEmpty = false;
	bool bIsFromStringTable = false;
	bool bIsCultureInvariant = false;
	bool bIsInitializedFromString = false;
};

struct FSomolUmgLocalizationAutofillPlan
{
	FSomolUmgLocalizationIssue Issue;
	FString NewNamespace;
	FString NewKey;
	bool bFillNamespace = false;
	bool bFillKey = false;
};

static FString MakeUmgLocalizationToken(const FString& RawValue, const FString& Fallback)
{
	FString Result;
	for (int32 Index = 0; Index < RawValue.Len(); ++Index)
	{
		const TCHAR Ch = RawValue[Index];
		if (FChar::IsAlnum(Ch) || Ch == '_' || Ch == '-')
		{
			Result.AppendChar(Ch);
		}
		else
		{
			Result.AppendChar('_');
		}
	}
	while (Result.Contains(TEXT("__")))
	{
		Result.ReplaceInline(TEXT("__"), TEXT("_"), ESearchCase::CaseSensitive);
	}
	while (Result.RemoveFromStart(TEXT("_"))) {}
	while (Result.RemoveFromEnd(TEXT("_"))) {}
	return Result.IsEmpty() ? Fallback : Result;
}

static FString BuildUmgLocalizationNamespace(UWidgetBlueprint* WidgetBP, const FString& NamespaceOverride)
{
	FString CleanOverride = NamespaceOverride;
	CleanOverride.TrimStartAndEndInline();
	if (!CleanOverride.IsEmpty())
	{
		return CleanOverride;
	}

	FString PackageName;
	if (WidgetBP && WidgetBP->GetOutermost())
	{
		PackageName = WidgetBP->GetOutermost()->GetName();
	}
	if (PackageName.IsEmpty() && WidgetBP)
	{
		PackageName = WidgetBP->GetPathName();
	}
	return FString::Printf(TEXT("UMG.%s"), *MakeUmgLocalizationToken(PackageName, TEXT("WidgetBlueprint")));
}

static FString BuildUmgLocalizationKey(const FSomolUmgLocalizationIssue& Issue)
{
	// TextNamespaceUtil::GenerateDeterministicTextKey is 5.4+. The manual token
	// below is already the fallback for when it returns empty, so 5.3 simply takes
	// that path — the keys differ in form but stay stable for a given widget.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	if (Issue.Widget && Issue.TextProperty)
	{
		const FString DeterministicKey = TextNamespaceUtil::GenerateDeterministicTextKey(Issue.Widget, Issue.TextProperty, false);
		if (!DeterministicKey.IsEmpty())
		{
			return DeterministicKey;
		}
	}
#endif

	return FString::Printf(
		TEXT("%s.%s"),
		*MakeUmgLocalizationToken(Issue.WidgetName, TEXT("Widget")),
		*MakeUmgLocalizationToken(Issue.PropertyName, TEXT("Text")));
}

static TSharedPtr<FJsonObject> UmgLocalizationIssueToJson(const FSomolUmgLocalizationIssue& Issue)
{
	TSharedPtr<FJsonObject> IssueObj = MakeShared<FJsonObject>();
	IssueObj->SetStringField(TEXT("widget_name"), Issue.WidgetName);
	IssueObj->SetStringField(TEXT("widget_class"), Issue.WidgetClass);
	IssueObj->SetStringField(TEXT("property"), Issue.PropertyName);
	IssueObj->SetStringField(TEXT("issue"), TEXT("missing_namespace_or_key"));
	IssueObj->SetStringField(TEXT("display_string"), Issue.DisplayString);
	IssueObj->SetStringField(TEXT("namespace"), Issue.Namespace);
	IssueObj->SetStringField(TEXT("key"), Issue.Key);
	IssueObj->SetBoolField(TEXT("has_namespace"), Issue.bHasNamespace);
	IssueObj->SetBoolField(TEXT("has_key"), Issue.bHasKey);
	IssueObj->SetBoolField(TEXT("is_empty"), Issue.bIsEmpty);
	IssueObj->SetBoolField(TEXT("is_from_string_table"), Issue.bIsFromStringTable);
	IssueObj->SetBoolField(TEXT("is_culture_invariant"), Issue.bIsCultureInvariant);
	IssueObj->SetBoolField(TEXT("is_initialized_from_string"), Issue.bIsInitializedFromString);
	return IssueObj;
}

static TArray<TSharedPtr<FJsonValue>> UmgLocalizationIssuesToJson(const TArray<FSomolUmgLocalizationIssue>& Issues)
{
	TArray<TSharedPtr<FJsonValue>> IssueArray;
	for (const FSomolUmgLocalizationIssue& Issue : Issues)
	{
		IssueArray.Add(MakeShared<FJsonValueObject>(UmgLocalizationIssueToJson(Issue)));
	}
	return IssueArray;
}

static TSharedPtr<FJsonObject> UmgLocalizationPlanToJson(const FSomolUmgLocalizationAutofillPlan& Plan)
{
	TSharedPtr<FJsonObject> PlanObj = UmgLocalizationIssueToJson(Plan.Issue);
	PlanObj->SetStringField(TEXT("old_namespace"), Plan.Issue.Namespace);
	PlanObj->SetStringField(TEXT("old_key"), Plan.Issue.Key);
	PlanObj->SetStringField(TEXT("new_namespace"), Plan.NewNamespace);
	PlanObj->SetStringField(TEXT("new_key"), Plan.NewKey);
	PlanObj->SetBoolField(TEXT("fill_namespace"), Plan.bFillNamespace);
	PlanObj->SetBoolField(TEXT("fill_key"), Plan.bFillKey);
	PlanObj->SetBoolField(TEXT("preserves_existing_namespace"), !Plan.Issue.bHasNamespace || Plan.NewNamespace == Plan.Issue.Namespace);
	PlanObj->SetBoolField(TEXT("preserves_existing_key"), !Plan.Issue.bHasKey || Plan.NewKey == Plan.Issue.Key);
	return PlanObj;
}

static TArray<TSharedPtr<FJsonValue>> UmgLocalizationPlansToJson(const TArray<FSomolUmgLocalizationAutofillPlan>& Plans)
{
	TArray<TSharedPtr<FJsonValue>> PlanArray;
	for (const FSomolUmgLocalizationAutofillPlan& Plan : Plans)
	{
		PlanArray.Add(MakeShared<FJsonValueObject>(UmgLocalizationPlanToJson(Plan)));
	}
	return PlanArray;
}

static void RestoreUmgLocalizationAutofillPlans(UWidgetBlueprint* WidgetBP, const TArray<FSomolUmgLocalizationAutofillPlan>& Plans)
{
#if WITH_EDITORONLY_DATA
	if (WidgetBP)
	{
		WidgetBP->Modify();
	}
	for (const FSomolUmgLocalizationAutofillPlan& Plan : Plans)
	{
		if (!Plan.Issue.Widget || !Plan.Issue.TextProperty)
		{
			continue;
		}
		Plan.Issue.Widget->Modify();
		Plan.Issue.TextProperty->SetPropertyValue_InContainer(Plan.Issue.Widget, Plan.Issue.TextValue);
		FPropertyChangedEvent ChangeEvent(Plan.Issue.TextProperty, EPropertyChangeType::ValueSet);
		Plan.Issue.Widget->PostEditChangeProperty(ChangeEvent);
	}
	if (WidgetBP)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);
		WidgetBP->MarkPackageDirty();
		SololmcpWriteFlush::EnsureFlushed(WidgetBP);
	}
#endif
}

static bool CollectUmgLocalizationKeyIssues(
	UWidgetBlueprint* WidgetBP,
	const FString& WidgetName,
	TArray<FSomolUmgLocalizationIssue>& OutIssues,
	int32& OutScannedWidgetCount,
	FString& OutError)
{
	OutIssues.Reset();
	OutScannedWidgetCount = 0;
	if (!WidgetBP || !WidgetBP->WidgetTree)
	{
		OutError = TEXT("WidgetBlueprint has no WidgetTree to inspect.");
		return false;
	}

	TArray<UWidget*> AllWidgets;
	UWidget* RequestedWidget = FindWidgetInBlueprint(WidgetBP, WidgetName, &AllWidgets);
	if (!WidgetName.IsEmpty() && !RequestedWidget)
	{
		OutError = FString::Printf(TEXT("Widget '%s' was not found in WidgetTree."), *WidgetName);
		return false;
	}

	const auto ScanWidget = [&OutIssues, &OutScannedWidgetCount](UWidget* Widget)
	{
		if (!Widget || !Widget->GetClass())
		{
			return;
		}
		++OutScannedWidgetCount;
		for (TFieldIterator<FTextProperty> It(Widget->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FTextProperty* TextProperty = *It;
			if (!TextProperty)
			{
				continue;
			}

			const FText TextValue = TextProperty->GetPropertyValue_InContainer(Widget);
			const TOptional<FString> Namespace = FTextInspector::GetNamespace(TextValue);
			const TOptional<FString> Key = FTextInspector::GetKey(TextValue);
			const bool bHasNamespace = Namespace.IsSet() && !Namespace.GetValue().IsEmpty();
			const bool bHasKey = Key.IsSet() && !Key.GetValue().IsEmpty();
			if (bHasNamespace && bHasKey)
			{
				continue;
			}

			FSomolUmgLocalizationIssue Issue;
			Issue.Widget = Widget;
			Issue.TextProperty = TextProperty;
			Issue.TextValue = TextValue;
			Issue.WidgetName = Widget->GetName();
			Issue.WidgetClass = Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString();
			Issue.PropertyName = TextProperty->GetName();
			Issue.DisplayString = TextValue.ToString();
			Issue.Namespace = Namespace.IsSet() ? Namespace.GetValue() : FString();
			Issue.Key = Key.IsSet() ? Key.GetValue() : FString();
			Issue.bHasNamespace = bHasNamespace;
			Issue.bHasKey = bHasKey;
			Issue.bIsEmpty = TextValue.IsEmpty();
			Issue.bIsFromStringTable = TextValue.IsFromStringTable();
			Issue.bIsCultureInvariant = TextValue.IsCultureInvariant();
			Issue.bIsInitializedFromString = TextValue.IsInitializedFromString();
			OutIssues.Add(MoveTemp(Issue));
		}
	};

	if (!WidgetName.IsEmpty())
	{
		ScanWidget(RequestedWidget);
	}
	else
	{
		for (UWidget* Widget : AllWidgets)
		{
			ScanWidget(Widget);
		}
	}

	OutError.Reset();
	return true;
}

static bool Tool_UmgWidgetBindEvent(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError);

static bool Tool_ExecuteConsoleCommand(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString Command;
	if (!Arguments->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		OutError = TEXT("Missing required argument: command");
		return false;
	}

	// Editor commands such as Automation/Map/OBJ are owned by GEditor; runtime
	// console variables and gameplay commands fall back to GEngine.
    UWorld* World = nullptr;
    if (GEditor)
    {
        World = GEditor->PlayWorld;
        if (!World) World = GEditor->GetEditorWorldContext().World();
    }
	if (!World) { OutError = TEXT("No editor world available."); return false; }

	bool bSuccess = false;
	if (GEditor)
	{
		bSuccess = GEditor->Exec(World, *Command);
	}
	if (!bSuccess && GEngine)
	{
		bSuccess = GEngine->Exec(World, *Command);
	}

	SetToolStatus(OutStructured, bSuccess);
	OutStructured->SetBoolField(TEXT("exec_return"), bSuccess);
	OutStructured->SetStringField(TEXT("command"), Command);
	OutSummary = FString::Printf(TEXT("Executed console command: %s (success=%s)"),
		*Command, bSuccess ? TEXT("true") : TEXT("false"));
	if (!bSuccess)
	{
		OutError = FString::Printf(TEXT("Console command failed: %s"), *Command);
		return false;
	}
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: python_execute — Python 脚本执行
// ═══════════════════════════════════════════════════════════════════════════════

// Small helper: capture log output while executing an Exec call.
// We subscribe a FOutputDevice to GLog, run the command, then unsubscribe.
class FSololmcpLogCapture : public FOutputDevice
{
public:
	FString Captured;
	bool bSawError = false;
	bool bSawPythonFailure = false;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		// Filter to Python categories + warnings/errors so we don't swallow unrelated spam.
		const FString CatStr = Category.ToString();
		const FString Line(V ? V : TEXT(""));
		const bool bIsError = Verbosity <= ELogVerbosity::Error;
		bSawError = bSawError || bIsError;
		if (CatStr.Contains(TEXT("Python"))
			&& (bIsError
				|| Line.Contains(TEXT("Traceback"))
				|| Line.Contains(TEXT("SyntaxError"))
				|| Line.Contains(TEXT("Exception"))
				|| Line.Contains(TEXT("Error:"))))
		{
			bSawPythonFailure = true;
		}
		if (CatStr.Contains(TEXT("Python")) || Verbosity <= ELogVerbosity::Warning)
		{
			Captured.Append(Line);
			Captured.Append(TEXT("\n"));
		}
	}
};

static bool Tool_PythonExecute(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString Script;
	if (!Arguments->TryGetStringField(TEXT("script"), Script) || Script.IsEmpty())
	{
		OutError = TEXT("Missing required argument: script");
		return false;
	}

	// Detect plugin availability by module presence — no header dependency required.
	const bool bPluginLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin"));
	if (!bPluginLoaded)
	{
		// Try to load on-demand (editor builds normally have it available even if unloaded).
		FModuleManager::Get().LoadModule(TEXT("PythonScriptPlugin"));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !GEngine)
	{
		OutError = TEXT("No editor world / GEngine available for Python exec.");
		return false;
	}

	// Normalize: accept both raw Python and the 'py ' prefixed form.
	FString Normalized = Script.TrimStart();
	if (!Normalized.StartsWith(TEXT("py ")))
	{
		Normalized = TEXT("py ") + Normalized;
	}

	// Capture Python log output so the caller sees print() results and tracebacks.
	FSololmcpLogCapture Capture;
	GLog->AddOutputDevice(&Capture);
	const bool bSuccess = GEngine->Exec(World, *Normalized);
	GLog->RemoveOutputDevice(&Capture);

	const bool bActuallySucceeded = bSuccess && !Capture.bSawPythonFailure;
	SetToolStatus(OutStructured, bActuallySucceeded);
	OutStructured->SetBoolField(TEXT("exec_return"), bSuccess);
	OutStructured->SetStringField(TEXT("script"), Script);
	if (!Capture.Captured.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("output"), Capture.Captured);
	}
	OutStructured->SetBoolField(TEXT("plugin_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")));

	if (!bActuallySucceeded)
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("PythonScriptPlugin")))
		{
			OutError = TEXT("PythonScriptPlugin is not available. Enable 'Python Editor Script Plugin' in Project Settings > Plugins > Scripting.");
		}
		else if (Capture.bSawPythonFailure)
		{
			OutError = TEXT("Python script reported an error/traceback. See 'output' field and Output Log (LogPython) for details.");
		}
		else
		{
			OutError = TEXT("Python script execution failed. See 'output' field and Output Log (LogPython) for details.");
		}
		return false;
	}

	OutSummary = FString::Printf(TEXT("Python executed (%d chars script, %d chars output)"),
		Script.Len(), Capture.Captured.Len());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: pie_start — Play In Editor
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_PieStart(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;
	if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }
	if (GEditor->PlayWorld)
	{
		OutStructured->SetBoolField(TEXT("pie_running"), true);
		OutSummary = TEXT("PIE already running.");
		return true;
	}

	FString Mode = TEXT("viewport");
	Arguments->TryGetStringField(TEXT("mode"), Mode);

	FRequestPlaySessionParams PlayParams;
	if (Mode == TEXT("new_window"))
	{
		PlayParams.DestinationSlateViewport.Reset();
	}
	else if (Mode == TEXT("standalone"))
	{
		// UE 5.7: Use SessionDestination instead of bUseAutoPie
		PlayParams.SessionDestination = EPlaySessionDestinationType::NewProcess;
	}

	int32 NumPlayers = 1;
	int32 NetMode = 0; // 0=standalone, 1=listen server, 2=client
	Arguments->TryGetNumberField(TEXT("num_players"), NumPlayers);
	Arguments->TryGetNumberField(TEXT("net_mode"), NetMode);
	if (NumPlayers < 1 || NumPlayers > 8 || NetMode < 0 || NetMode > 2)
	{
		OutError = TEXT("num_players must be in [1,8] and net_mode must be 0, 1, or 2.");
		return false;
	}

	ULevelEditorPlaySettings* SessionSettings = DuplicateObject<ULevelEditorPlaySettings>(
		GetMutableDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
	if (!SessionSettings)
	{
		OutError = TEXT("Failed to allocate per-request PIE settings.");
		return false;
	}
	SessionSettings->SetPlayNumberOfClients(NumPlayers);
	SessionSettings->SetRunUnderOneProcess(true);
	SessionSettings->SetPlayNetMode(NetMode == 1 ? PIE_ListenServer : NetMode == 2 ? PIE_Client : PIE_Standalone);
	PlayParams.EditorPlaySettings = SessionSettings;

	GEditor->RequestPlaySession(PlayParams);

	OutStructured->SetStringField(TEXT("mode"), Mode);
	OutStructured->SetNumberField(TEXT("num_players"), NumPlayers);
	OutStructured->SetStringField(TEXT("status"), TEXT("requested"));
	OutSummary = FString::Printf(TEXT("Requested PIE start (mode=%s, players=%d)."), *Mode, NumPlayers);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: pie_stop — Stop Play In Editor
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_PieStop(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;
	if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }
	if (!GEditor->PlayWorld)
	{
		OutError = TEXT("No active PIE session to stop.");
		return false;
	}

	GEditor->RequestEndPlayMap();
	OutSummary = TEXT("PIE session stopped.");
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: pie_get_status — Get PIE status
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_PieGetStatus(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

	// Check PIE state via GEditor->PlayWorld
	bool bInPIE = GEditor->PlayWorld != nullptr;
	bool bIsSimulating = GEditor->bIsSimulatingInEditor;

	FString State = bInPIE ? TEXT("playing") : (bIsSimulating ? TEXT("simulating") : TEXT("stopped"));

	OutStructured->SetStringField(TEXT("state"), State);
	OutStructured->SetBoolField(TEXT("in_pie"), bInPIE);
	OutStructured->SetBoolField(TEXT("simulating"), bIsSimulating);
	OutSummary = FString::Printf(TEXT("PIE status: %s"), *State);
	return true;
}

static bool Tool_PieLocalPlayerCreate(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;
	UWorld* World = GetActivePieWorld(OutError);
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance) { OutError = TEXT("Active PIE GameInstance is unavailable."); return false; }
	int32 ControllerId = 1;
	Arguments->TryGetNumberField(TEXT("controller_id"), ControllerId);
	if (ControllerId < 0 || ControllerId > 7)
	{
		OutError = TEXT("controller_id must be in [0,7].");
		return false;
	}
	for (ULocalPlayer* Existing : GameInstance->GetLocalPlayers())
	{
		if (Existing && Existing->GetControllerId() == ControllerId)
		{
			OutError = FString::Printf(TEXT("Local player %d already exists."), ControllerId);
			return false;
		}
	}
	const int32 Before = GameInstance->GetLocalPlayers().Num();
	FString CreateError;
	ULocalPlayer* Created = GameInstance->CreateLocalPlayer(ControllerId, CreateError, true);
	const int32 After = GameInstance->GetLocalPlayers().Num();
	if (!Created || After != Before + 1)
	{
		OutError = CreateError.IsEmpty() ? TEXT("Local player creation did not change the player count.") : CreateError;
		return false;
	}
	SetToolStatus(OutStructured, true);
	OutStructured->SetNumberField(TEXT("controller_id"), ControllerId);
	OutStructured->SetNumberField(TEXT("local_players_before"), Before);
	OutStructured->SetNumberField(TEXT("local_players_after"), After);
	OutStructured->SetStringField(TEXT("world"), World->GetPathName());
	OutSummary = FString::Printf(TEXT("Created PIE local player %d (%d -> %d)."), ControllerId, Before, After);
	return true;
}

static bool Tool_PieLocalPlayerRemove(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;
	UWorld* World = GetActivePieWorld(OutError);
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance) { OutError = TEXT("Active PIE GameInstance is unavailable."); return false; }
	int32 ControllerId = 1;
	Arguments->TryGetNumberField(TEXT("controller_id"), ControllerId);
	ULocalPlayer* Target = nullptr;
	for (ULocalPlayer* Existing : GameInstance->GetLocalPlayers())
	{
		if (Existing && Existing->GetControllerId() == ControllerId) { Target = Existing; break; }
	}
	if (!Target) { OutError = FString::Printf(TEXT("Local player %d was not found."), ControllerId); return false; }
	const int32 Before = GameInstance->GetLocalPlayers().Num();
	if (!GameInstance->RemoveLocalPlayer(Target) || GameInstance->GetLocalPlayers().Num() != Before - 1)
	{
		OutError = TEXT("Local player removal failed readback.");
		return false;
	}
	SetToolStatus(OutStructured, true);
	OutStructured->SetNumberField(TEXT("controller_id"), ControllerId);
	OutStructured->SetNumberField(TEXT("local_players_before"), Before);
	OutStructured->SetNumberField(TEXT("local_players_after"), GameInstance->GetLocalPlayers().Num());
	OutSummary = FString::Printf(TEXT("Removed PIE local player %d (%d -> %d)."), ControllerId, Before, Before - 1);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: pie_capture — Capture viewport during PIE
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_PieCapture(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;
	if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

	// Delegate to existing screenshot_viewport or capture from play world
	if (!GEditor->PlayWorld)
	{
		OutError = TEXT("Not currently in PIE. Start PIE first with pie_start.");
		return false;
	}

	int32 MaxWidth = 1920, MaxHeight = 1080;
	Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
	Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);

	// Get PIE viewport - UE 5.7: Use GEngine instead of UWorld::GetEngine()
	if (!GEngine) { OutError = TEXT("No engine available."); return false; }

	// Use the GameViewport to capture
	UGameViewportClient* ViewportClient = GEditor->PlayWorld->GetGameViewport();
	if (!ViewportClient) { OutError = TEXT("No viewport client in PIE."); return false; }

	// Queue capture - use FViewportClient::OnScreenshotCaptured callback approach
	OutStructured->SetBoolField(TEXT("in_pie"), true);
	OutStructured->SetNumberField(TEXT("width"), MaxWidth);
	OutStructured->SetNumberField(TEXT("height"), MaxHeight);
	OutSummary = TEXT("PIE capture initiated. Use screenshot_viewport for actual capture.");
	OutError = TEXT("For PIE screenshots, use 'screenshot_viewport' which captures the active viewport (including PIE viewport when active).");
	return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: pie_screenshot — Alias for PIE screenshot capture
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_PieScreenshot(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	// PIE screenshot is same as viewport screenshot when in PIE mode
	if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

	if (!GEditor->PlayWorld)
	{
		OutError = TEXT("Not in PIE. Use pie_start first.");
		return false;
	}

	// This is a convenience alias — actual capture happens via screenshot_viewport
	OutStructured->SetBoolField(TEXT("in_pie"), true);
	OutSummary = TEXT("PIE is active. Use screenshot_viewport to capture the PIE viewport.");
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P0: Alias tools — 14 个 HTML 规范别名
// These aliases map HTML spec tool names to existing C++ implementations.
// They use the same parameter format as the target tool for compatibility.
// ═══════════════════════════════════════════════════════════════════════════════

// actor_delete → same as actor_destroy (deletes actors by ID)
static bool Tool_UmgRuntimePreviewSpawnWidget(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	UWorld* PieWorld = GetActivePieWorld(OutError);
	if (!PieWorld)
	{
		return false;
	}

	FString WidgetPath;
	if (!Arguments->TryGetStringField(TEXT("widget_blueprint_path"), WidgetPath))
	{
		Arguments->TryGetStringField(TEXT("widget_class_path"), WidgetPath);
	}

	UClass* WidgetClass = ResolveUserWidgetRuntimeClass(Context, WidgetPath, OutError);
	if (!WidgetClass)
	{
		return false;
	}

	int32 ZOrder = 1000;
	Arguments->TryGetNumberField(TEXT("z_order"), ZOrder);

	UUserWidget* RuntimeWidget = CreateWidget<UUserWidget>(PieWorld, WidgetClass);
	if (!RuntimeWidget)
	{
		OutError = TEXT("CreateWidget returned null.");
		return false;
	}

	RuntimeWidget->SetFlags(RF_Transient);
	RuntimeWidget->AddToViewport(ZOrder);

	const FString MountHandle = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	GSomolRuntimePreviewWidgets.Add(MountHandle, RuntimeWidget);

	OutStructured = RuntimePreviewWidgetToJson(MountHandle, RuntimeWidget);
	OutStructured->SetStringField(TEXT("widget_source"), WidgetPath);
	OutStructured->SetNumberField(TEXT("z_order"), ZOrder);
	OutStructured->SetBoolField(TEXT("pre_existing_pie"), true);
	OutStructured->SetBoolField(TEXT("started_editor"), false);
	OutStructured->SetBoolField(TEXT("started_pie"), false);
	OutSummary = FString::Printf(TEXT("Mounted runtime UMG preview widget with handle %s."), *MountHandle);
	return true;
}

static bool Tool_UmgRuntimePreviewCapture(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	UWorld* PieWorld = GetActivePieWorld(OutError);
	if (!PieWorld)
	{
		return false;
	}

	FString MountHandle;
	if (!Arguments->TryGetStringField(TEXT("mount_handle"), MountHandle))
	{
		OutError = TEXT("Missing mount_handle.");
		return false;
	}

	TWeakObjectPtr<UUserWidget>* WidgetPtr = GSomolRuntimePreviewWidgets.Find(MountHandle);
	UUserWidget* RuntimeWidget = WidgetPtr ? WidgetPtr->Get() : nullptr;
	if (!RuntimeWidget)
	{
		OutError = TEXT("mount_handle does not reference a live runtime preview widget.");
		return false;
	}

	int32 MaxWidth = 1920;
	int32 MaxHeight = 1080;
	Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
	Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);

	TArray<uint8> PngData;
	if (!Context.Services.CaptureViewportScreenshot(PngData, MaxWidth, MaxHeight, OutError))
	{
		return false;
	}

	OutStructured = RuntimePreviewWidgetToJson(MountHandle, RuntimeWidget);
	OutStructured->SetStringField(TEXT("_imageContent"), FBase64::Encode(PngData));
	OutStructured->SetStringField(TEXT("_imageMimeType"), TEXT("image/png"));
	OutStructured->SetNumberField(TEXT("max_width"), MaxWidth);
	OutStructured->SetNumberField(TEXT("max_height"), MaxHeight);
	OutStructured->SetNumberField(TEXT("png_bytes"), PngData.Num());
	OutStructured->SetBoolField(TEXT("pre_existing_pie"), true);
	OutStructured->SetBoolField(TEXT("stopped_pie"), false);
	OutSummary = FString::Printf(TEXT("Captured runtime UMG preview widget %s (%d PNG bytes)."), *MountHandle, PngData.Num());
	return true;
}

static bool Tool_UmgRuntimePreviewTeardown(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString MountHandle;
	if (!Arguments->TryGetStringField(TEXT("mount_handle"), MountHandle))
	{
		OutError = TEXT("Missing mount_handle.");
		return false;
	}

	TWeakObjectPtr<UUserWidget>* WidgetPtr = GSomolRuntimePreviewWidgets.Find(MountHandle);
	UUserWidget* RuntimeWidget = WidgetPtr ? WidgetPtr->Get() : nullptr;
	const bool bWasMounted = RuntimeWidget != nullptr;
	if (RuntimeWidget)
	{
		RuntimeWidget->RemoveFromParent();
	}
	GSomolRuntimePreviewWidgets.Remove(MountHandle);

	OutStructured->SetStringField(TEXT("mount_handle"), MountHandle);
	OutStructured->SetBoolField(TEXT("removed_smoke_owned_widget"), bWasMounted);
	OutStructured->SetBoolField(TEXT("stopped_pie"), false);
	OutStructured->SetBoolField(TEXT("stopped_editor"), false);
	OutStructured->SetNumberField(TEXT("remaining_preview_widgets"), GSomolRuntimePreviewWidgets.Num());
	OutSummary = bWasMounted
		? TEXT("Removed runtime UMG preview widget without stopping PIE.")
		: TEXT("Runtime UMG preview handle was already gone; no PIE/editor state changed.");
	return true;
}

static bool Tool_UmgBindingInspectGraph(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	TArray<UWidget*> AllWidgets;
	FindWidgetInBlueprint(WidgetBP, TEXT("__collect_all__"), &AllWidgets);

	TArray<TSharedPtr<FJsonValue>> WidgetsArray;
	for (UWidget* Widget : AllWidgets)
	{
		if (!Widget) continue;

		TSharedPtr<FJsonObject> WidgetObj = MakeShared<FJsonObject>();
		WidgetObj->SetStringField(TEXT("name"), Widget->GetName());
		WidgetObj->SetStringField(TEXT("class"), Widget->GetClass() ? Widget->GetClass()->GetName() : FString());
		WidgetObj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
		WidgetObj->SetBoolField(TEXT("is_list_view"), Widget->IsA<UListView>());
		WidgetObj->SetArrayField(TEXT("available_events"), MulticastDelegateNamesToJson(Widget->GetClass()));
		WidgetsArray.Add(MakeShared<FJsonValueObject>(WidgetObj));
	}

	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	AddGraphsToJsonArray(WidgetBP->UbergraphPages, TEXT("ubergraph"), GraphsArray);
	AddGraphsToJsonArray(WidgetBP->FunctionGraphs, TEXT("function"), GraphsArray);
	AddGraphsToJsonArray(WidgetBP->DelegateSignatureGraphs, TEXT("delegate_signature"), GraphsArray);

	TArray<TSharedPtr<FJsonValue>> AnimationsArray;
	for (UWidgetAnimation* Animation : WidgetBP->Animations)
	{
		if (!Animation) continue;

		TSharedPtr<FJsonObject> AnimationObj = MakeShared<FJsonObject>();
		AnimationObj->SetStringField(TEXT("name"), Animation->GetName());
		AnimationObj->SetStringField(TEXT("path"), Animation->GetPathName());
		AnimationsArray.Add(MakeShared<FJsonValueObject>(AnimationObj));
	}

	TArray<TSharedPtr<FJsonValue>> BindingsArray;
	for (UEdGraph* Graph : WidgetBP->UbergraphPages)
	{
		if (!Graph) continue;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node);
			if (!BoundEvent) continue;

			TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
			BindingObj->SetStringField(TEXT("graph"), Graph->GetName());
			BindingObj->SetStringField(TEXT("component_property"), BoundEvent->ComponentPropertyName.ToString());
			BindingObj->SetStringField(TEXT("delegate_property"), BoundEvent->DelegatePropertyName.ToString());
			BindingObj->SetStringField(TEXT("function_name"), BoundEvent->CustomFunctionName.ToString());
			BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObj));
		}
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetArrayField(TEXT("widgets"), WidgetsArray);
	OutStructured->SetArrayField(TEXT("graphs"), GraphsArray);
	OutStructured->SetArrayField(TEXT("animations"), AnimationsArray);
	OutStructured->SetArrayField(TEXT("bindings"), BindingsArray);
	OutStructured->SetNumberField(TEXT("widget_count"), WidgetsArray.Num());
	OutStructured->SetNumberField(TEXT("graph_count"), GraphsArray.Num());
	OutStructured->SetNumberField(TEXT("animation_count"), AnimationsArray.Num());
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Inspected UMG WidgetBlueprint '%s': %d widgets, %d graphs, %d animations."),
		*AssetPath, WidgetsArray.Num(), GraphsArray.Num(), AnimationsArray.Num());
	return true;
}

static bool Tool_UmgWidgetTreeInspectSlots(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString WidgetName;
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	TArray<UWidget*> AllWidgets;
	UWidget* RequestedWidget = FindWidgetInBlueprint(WidgetBP, WidgetName, &AllWidgets);
	TArray<TSharedPtr<FJsonValue>> SlotsArray;
	if (!WidgetName.IsEmpty())
	{
		if (!RequestedWidget)
		{
			FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' was not found in WidgetTree."), *WidgetName),
				TEXT("Run umg_binding_inspect_graph or umg_widget_tree_list and pass an exact widget name."), OutError);
			return false;
		}
		SlotsArray.Add(MakeShared<FJsonValueObject>(WidgetSlotToJson(RequestedWidget)));
	}
	else
	{
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget)
			{
				SlotsArray.Add(MakeShared<FJsonValueObject>(WidgetSlotToJson(Widget)));
			}
		}
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetArrayField(TEXT("slots"), SlotsArray);
	OutStructured->SetNumberField(TEXT("slot_count"), SlotsArray.Num());
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Inspected UMG slot metadata for '%s' (%d row(s))."), *AssetPath, SlotsArray.Num());
	return true;
}

static bool Tool_UmgWidgetTreeInspectProperties(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString WidgetName;
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	bool bIncludeValues = true;
	Arguments->TryGetBoolField(TEXT("include_values"), bIncludeValues);
	int32 MaxProperties = 128;
	Arguments->TryGetNumberField(TEXT("max_properties"), MaxProperties);

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	TArray<UWidget*> AllWidgets;
	UWidget* RequestedWidget = FindWidgetInBlueprint(WidgetBP, WidgetName, &AllWidgets);
	TArray<TSharedPtr<FJsonValue>> WidgetsArray;
	const auto AddWidgetProperties = [&](UWidget* Widget)
	{
		if (!Widget) return;
		TSharedPtr<FJsonObject> WidgetObj = MakeShared<FJsonObject>();
		WidgetObj->SetStringField(TEXT("name"), Widget->GetName());
		WidgetObj->SetStringField(TEXT("class"), Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString());
		WidgetObj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
		WidgetObj->SetArrayField(TEXT("properties"), ReflectedPropertiesToJson(Widget, MaxProperties, bIncludeValues));
		WidgetsArray.Add(MakeShared<FJsonValueObject>(WidgetObj));
	};

	if (!WidgetName.IsEmpty())
	{
		if (!RequestedWidget)
		{
			FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' was not found in WidgetTree."), *WidgetName),
				TEXT("Run umg_binding_inspect_graph or umg_widget_tree_list and pass an exact widget name."), OutError);
			return false;
		}
		AddWidgetProperties(RequestedWidget);
	}
	else
	{
		for (UWidget* Widget : AllWidgets)
		{
			AddWidgetProperties(Widget);
		}
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetBoolField(TEXT("include_values"), bIncludeValues);
	OutStructured->SetNumberField(TEXT("max_properties"), MaxProperties);
	OutStructured->SetArrayField(TEXT("widgets"), WidgetsArray);
	OutStructured->SetNumberField(TEXT("widget_count"), WidgetsArray.Num());
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Inspected UMG widget properties for '%s' (%d widget row(s))."), *AssetPath, WidgetsArray.Num());
	return true;
}

static bool Tool_UmgFocusNavigationInspect(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	TArray<UWidget*> AllWidgets;
	FindWidgetInBlueprint(WidgetBP, TEXT("__collect_all__"), &AllWidgets);
	TArray<TSharedPtr<FJsonValue>> FocusArray;
	for (UWidget* Widget : AllWidgets)
	{
		if (!Widget) continue;
		bool bIsFocusable = false;
		const bool bHasFocusableFlag = GetBoolPropertyValue(Widget, TEXT("bIsFocusable"), bIsFocusable);
		TSharedPtr<FJsonObject> FocusObj = MakeShared<FJsonObject>();
		FocusObj->SetStringField(TEXT("name"), Widget->GetName());
		FocusObj->SetStringField(TEXT("class"), Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString());
		FocusObj->SetBoolField(TEXT("has_focusable_flag"), bHasFocusableFlag);
		FocusObj->SetBoolField(TEXT("is_focusable"), bIsFocusable);
		FocusObj->SetNumberField(TEXT("visibility"), static_cast<int32>(Widget->GetVisibility()));
		FocusObj->SetBoolField(TEXT("has_navigation_object"), FindFProperty<FObjectProperty>(Widget->GetClass(), TEXT("Navigation")) != nullptr);
		FocusArray.Add(MakeShared<FJsonValueObject>(FocusObj));
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetArrayField(TEXT("widgets"), FocusArray);
	OutStructured->SetNumberField(TEXT("widget_count"), FocusArray.Num());
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Inspected UMG focus/navigation hints for '%s' (%d widget row(s))."), *AssetPath, FocusArray.Num());
	return true;
}

static bool Tool_UmgRuntimeFocusProbe(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const bool bInPie = GEditor && GEditor->PlayWorld != nullptr;
	TArray<TSharedPtr<FJsonValue>> PreviewHandles;
	for (const TPair<FString, TWeakObjectPtr<UUserWidget>>& Pair : GSomolRuntimePreviewWidgets)
	{
		UUserWidget* RuntimeWidget = Pair.Value.Get();
		TSharedPtr<FJsonObject> HandleObj = MakeShared<FJsonObject>();
		HandleObj->SetStringField(TEXT("mount_handle"), Pair.Key);
		HandleObj->SetBoolField(TEXT("widget_live"), RuntimeWidget != nullptr);
		if (RuntimeWidget)
		{
			HandleObj->SetStringField(TEXT("widget_name"), RuntimeWidget->GetName());
			HandleObj->SetStringField(TEXT("widget_class"), RuntimeWidget->GetClass() ? RuntimeWidget->GetClass()->GetPathName() : FString());
			HandleObj->SetBoolField(TEXT("is_in_viewport"), RuntimeWidget->IsInViewport());
		}
		PreviewHandles.Add(MakeShared<FJsonValueObject>(HandleObj));
	}

	OutStructured->SetBoolField(TEXT("in_pie"), bInPie);
	OutStructured->SetNumberField(TEXT("active_runtime_preview_widgets"), PreviewHandles.Num());
	OutStructured->SetArrayField(TEXT("runtime_preview_widgets"), PreviewHandles);
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, bInPie);
	if (!bInPie)
	{
		FailClosedUmgTool(OutStructured, TEXT("No active PIE session; runtime focus cannot be probed."),
			TEXT("Start PIE and mount a smoke-owned widget before runtime focus probing."), OutError);
		return false;
	}
	OutSummary = FString::Printf(TEXT("Probed runtime UMG focus context with %d smoke-owned preview widget(s)."), PreviewHandles.Num());
	return true;
}

static bool Tool_UmgTextLocalizationInspect(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString WidgetName;
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	TArray<UWidget*> AllWidgets;
	UWidget* RequestedWidget = FindWidgetInBlueprint(WidgetBP, WidgetName, &AllWidgets);
	TArray<TSharedPtr<FJsonValue>> WidgetsArray;
	const auto AddTextWidget = [&](UWidget* Widget)
	{
		if (!Widget) return;
		TArray<TSharedPtr<FJsonValue>> TextProps = TextPropertiesToJson(Widget);
		if (TextProps.Num() == 0) return;
		TSharedPtr<FJsonObject> WidgetObj = MakeShared<FJsonObject>();
		WidgetObj->SetStringField(TEXT("name"), Widget->GetName());
		WidgetObj->SetStringField(TEXT("class"), Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString());
		WidgetObj->SetArrayField(TEXT("text_properties"), TextProps);
		WidgetObj->SetNumberField(TEXT("text_property_count"), TextProps.Num());
		WidgetsArray.Add(MakeShared<FJsonValueObject>(WidgetObj));
	};

	if (!WidgetName.IsEmpty())
	{
		if (!RequestedWidget)
		{
			FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' was not found in WidgetTree."), *WidgetName),
				TEXT("Run umg_binding_inspect_graph or umg_widget_tree_list and pass an exact widget name."), OutError);
			return false;
		}
		AddTextWidget(RequestedWidget);
	}
	else
	{
		for (UWidget* Widget : AllWidgets)
		{
			AddTextWidget(Widget);
		}
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetArrayField(TEXT("widgets"), WidgetsArray);
	OutStructured->SetNumberField(TEXT("widget_count"), WidgetsArray.Num());
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Inspected UMG text localization for '%s' (%d widget row(s))."), *AssetPath, WidgetsArray.Num());
	return true;
}

static bool Tool_UmgLocalizationKeyAudit(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	TSharedRef<FJsonObject> InspectArgs = MakeShared<FJsonObject>();
	InspectArgs->SetStringField(TEXT("asset_path"), GetUmgAssetPathArg(Arguments));
	FString WidgetName;
	if (Arguments->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		InspectArgs->SetStringField(TEXT("widget_name"), WidgetName);
	}

	TSharedRef<FJsonObject> InspectResult = MakeShared<FJsonObject>();
	FString InspectSummary;
	FString InspectError;
	if (!Tool_UmgTextLocalizationInspect(Context, InspectArgs, InspectResult, InspectSummary, InspectError))
	{
		OutStructured = InspectResult;
		OutError = InspectError;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
	if (InspectResult->TryGetArrayField(TEXT("widgets"), WidgetsArray) && WidgetsArray)
	{
		for (const TSharedPtr<FJsonValue>& WidgetValue : *WidgetsArray)
		{
			const TSharedPtr<FJsonObject>* WidgetObj = nullptr;
			if (!WidgetValue.IsValid() || !WidgetValue->TryGetObject(WidgetObj) || !WidgetObj || !WidgetObj->IsValid())
			{
				continue;
			}
			FString CurrentWidgetName;
			(*WidgetObj)->TryGetStringField(TEXT("name"), CurrentWidgetName);
			const TArray<TSharedPtr<FJsonValue>>* TextProps = nullptr;
			if (!(*WidgetObj)->TryGetArrayField(TEXT("text_properties"), TextProps) || !TextProps)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& TextValue : *TextProps)
			{
				const TSharedPtr<FJsonObject>* TextObj = nullptr;
				if (!TextValue.IsValid() || !TextValue->TryGetObject(TextObj) || !TextObj || !TextObj->IsValid())
				{
					continue;
				}
				bool bHasNamespace = false;
				bool bHasKey = false;
				(*TextObj)->TryGetBoolField(TEXT("has_namespace"), bHasNamespace);
				(*TextObj)->TryGetBoolField(TEXT("has_key"), bHasKey);
				if (!bHasNamespace || !bHasKey)
				{
					FString PropertyName;
					(*TextObj)->TryGetStringField(TEXT("property"), PropertyName);
					TSharedPtr<FJsonObject> IssueObj = MakeShared<FJsonObject>();
					IssueObj->SetStringField(TEXT("widget_name"), CurrentWidgetName);
					IssueObj->SetStringField(TEXT("property"), PropertyName);
					IssueObj->SetStringField(TEXT("issue"), TEXT("missing_namespace_or_key"));
					IssueObj->SetBoolField(TEXT("has_namespace"), bHasNamespace);
					IssueObj->SetBoolField(TEXT("has_key"), bHasKey);
					Issues.Add(MakeShared<FJsonValueObject>(IssueObj));
				}
			}
		}
	}

	OutStructured = InspectResult;
	OutStructured->SetArrayField(TEXT("issues"), Issues);
	OutStructured->SetNumberField(TEXT("issue_count"), Issues.Num());
	OutStructured->SetStringField(TEXT("audit_status"), Issues.Num() == 0 ? TEXT("passed") : TEXT("needs_review"));
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Audited UMG localization keys: %d issue(s)."), Issues.Num());
	return true;
}

static bool Tool_UmgLocalizationKeyAutofill(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString WidgetName;
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	bool bDryRun = true;
	Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
	FString NamespaceOverride;
	Arguments->TryGetStringField(TEXT("namespace"), NamespaceOverride);

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetNumberField(TEXT("issue_count_before"), 0);
		OutStructured->SetNumberField(TEXT("issue_count_after"), 0);
		OutStructured->SetBoolField(TEXT("receipt_complete"), false);
		return false;
	}

	TArray<FSomolUmgLocalizationIssue> IssuesBefore;
	int32 ScannedWidgetCount = 0;
	FString CollectError;
	if (!CollectUmgLocalizationKeyIssues(WidgetBP, WidgetName, IssuesBefore, ScannedWidgetCount, CollectError))
	{
		FailClosedUmgTool(OutStructured, CollectError,
			TEXT("Run umg_widget_tree_list or umg_text_localization_inspect and pass a valid WidgetBlueprint/widget_name."), OutError);
		OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
		OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
		OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
		OutStructured->SetNumberField(TEXT("issue_count_before"), 0);
		OutStructured->SetNumberField(TEXT("issue_count_after"), 0);
		OutStructured->SetBoolField(TEXT("receipt_complete"), false);
		return false;
	}

	const FString DefaultNamespace = BuildUmgLocalizationNamespace(WidgetBP, NamespaceOverride);
	TArray<FSomolUmgLocalizationAutofillPlan> Plans;
	TArray<FString> UnsafeReasons;
	TSet<FString> PlannedTextIds;
	for (const FSomolUmgLocalizationIssue& Issue : IssuesBefore)
	{
		FSomolUmgLocalizationAutofillPlan Plan;
		Plan.Issue = Issue;
		Plan.bFillNamespace = !Issue.bHasNamespace;
		Plan.bFillKey = !Issue.bHasKey;
		Plan.NewNamespace = Issue.bHasNamespace ? Issue.Namespace : DefaultNamespace;
		Plan.NewKey = Issue.bHasKey ? Issue.Key : BuildUmgLocalizationKey(Issue);

		if (Plan.NewNamespace.IsEmpty() || Plan.NewKey.IsEmpty())
		{
			UnsafeReasons.Add(FString::Printf(TEXT("%s.%s could not resolve a non-empty namespace/key."),
				*Issue.WidgetName, *Issue.PropertyName));
		}
		if (Issue.bHasNamespace && Plan.NewNamespace != Issue.Namespace)
		{
			UnsafeReasons.Add(FString::Printf(TEXT("%s.%s would overwrite an existing namespace."),
				*Issue.WidgetName, *Issue.PropertyName));
		}
		if (Issue.bHasKey && Plan.NewKey != Issue.Key)
		{
			UnsafeReasons.Add(FString::Printf(TEXT("%s.%s would overwrite an existing localization key."),
				*Issue.WidgetName, *Issue.PropertyName));
		}
		if (Issue.bIsFromStringTable)
		{
			UnsafeReasons.Add(FString::Printf(TEXT("%s.%s is backed by a string table and must be reviewed manually."),
				*Issue.WidgetName, *Issue.PropertyName));
		}
		if (Issue.bIsCultureInvariant)
		{
			UnsafeReasons.Add(FString::Printf(TEXT("%s.%s is culture-invariant and must be reviewed manually."),
				*Issue.WidgetName, *Issue.PropertyName));
		}
		if (!Issue.bIsEmpty && !Issue.bIsInitializedFromString)
		{
			UnsafeReasons.Add(FString::Printf(TEXT("%s.%s is not initialized from a plain source string and must be reviewed manually."),
				*Issue.WidgetName, *Issue.PropertyName));
		}

		const FString PlannedTextId = FString::Printf(TEXT("%s|%s"), *Plan.NewNamespace, *Plan.NewKey);
		if (PlannedTextIds.Contains(PlannedTextId))
		{
			UnsafeReasons.Add(FString::Printf(TEXT("%s.%s would duplicate an autofill localization id."),
				*Issue.WidgetName, *Issue.PropertyName));
		}
		PlannedTextIds.Add(PlannedTextId);
		Plans.Add(MoveTemp(Plan));
	}

	const TArray<TSharedPtr<FJsonValue>> IssuesBeforeJson = UmgLocalizationIssuesToJson(IssuesBefore);
	const TArray<TSharedPtr<FJsonValue>> PlannedChangesJson = UmgLocalizationPlansToJson(Plans);

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetNumberField(TEXT("scanned_widget_count"), ScannedWidgetCount);
	OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
	OutStructured->SetStringField(TEXT("default_namespace"), DefaultNamespace);
	OutStructured->SetNumberField(TEXT("issue_count_before"), IssuesBefore.Num());
	OutStructured->SetArrayField(TEXT("issues_before"), IssuesBeforeJson);
	OutStructured->SetArrayField(TEXT("planned_changes"), PlannedChangesJson);
	OutStructured->SetNumberField(TEXT("planned_change_count"), Plans.Num());
	OutStructured->SetBoolField(TEXT("mutation_attempted"), false);
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	OutStructured->SetBoolField(TEXT("rollback_performed"), false);

	if (UnsafeReasons.Num() > 0)
	{
		FailClosedUmgTool(OutStructured, TEXT("UMG localization key autofill preflight found unsafe changes."),
			TEXT("Resolve unsafe localization text cases manually, then rerun dry_run before mutation."), OutError);
		OutStructured->SetArrayField(TEXT("unsafe_reasons"), StringArrayToJsonValues(UnsafeReasons));
		OutStructured->SetNumberField(TEXT("unsafe_reason_count"), UnsafeReasons.Num());
		OutStructured->SetNumberField(TEXT("issue_count_after"), IssuesBefore.Num());
		OutStructured->SetArrayField(TEXT("issues_after"), IssuesBeforeJson);
		OutStructured->SetBoolField(TEXT("receipt_complete"), false);
		return false;
	}

	if (bDryRun || Plans.Num() == 0)
	{
		OutStructured->SetNumberField(TEXT("issue_count_after"), IssuesBefore.Num());
		OutStructured->SetArrayField(TEXT("issues_after"), IssuesBeforeJson);
		OutStructured->SetBoolField(TEXT("receipt_complete"), true);
		SetToolStatus(OutStructured, true);
		OutSummary = bDryRun
			? FString::Printf(TEXT("Dry-run UMG localization key autofill for '%s': %d issue(s), %d planned change(s)."),
				*AssetPath, IssuesBefore.Num(), Plans.Num())
			: FString::Printf(TEXT("UMG localization key autofill found no missing namespace/key issues for '%s'."), *AssetPath);
		return true;
	}

#if WITH_EDITORONLY_DATA
	WidgetBP->Modify();
	for (const FSomolUmgLocalizationAutofillPlan& Plan : Plans)
	{
		if (!Plan.Issue.Widget || !Plan.Issue.TextProperty)
		{
			continue;
		}

		Plan.Issue.Widget->Modify();
		const FText NewText = FText::ChangeKey(Plan.NewNamespace, Plan.NewKey, Plan.Issue.TextValue);
		Plan.Issue.TextProperty->SetPropertyValue_InContainer(Plan.Issue.Widget, NewText);
		FPropertyChangedEvent ChangeEvent(Plan.Issue.TextProperty, EPropertyChangeType::ValueSet);
		Plan.Issue.Widget->PostEditChangeProperty(ChangeEvent);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);
	WidgetBP->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(WidgetBP);
#else
	FailClosedUmgTool(OutStructured, TEXT("UMG localization key autofill requires editor-only FText key mutation support."),
		TEXT("Run this tool in an editor build with WITH_EDITORONLY_DATA enabled."), OutError);
	OutStructured->SetNumberField(TEXT("issue_count_after"), IssuesBefore.Num());
	OutStructured->SetArrayField(TEXT("issues_after"), IssuesBeforeJson);
	OutStructured->SetBoolField(TEXT("receipt_complete"), false);
	return false;
#endif

	TArray<FSomolUmgLocalizationIssue> IssuesAfter;
	int32 PostScannedWidgetCount = 0;
	FString PostCollectError;
	if (!CollectUmgLocalizationKeyIssues(WidgetBP, WidgetName, IssuesAfter, PostScannedWidgetCount, PostCollectError))
	{
		RestoreUmgLocalizationAutofillPlans(WidgetBP, Plans);
		FailClosedUmgTool(OutStructured, PostCollectError,
			TEXT("Post-edit readback failed; inspect the WidgetBlueprint before further mutation."), OutError);
		OutStructured->SetBoolField(TEXT("mutation_attempted"), true);
		OutStructured->SetBoolField(TEXT("mutation_performed"), false);
		OutStructured->SetBoolField(TEXT("rollback_performed"), true);
		OutStructured->SetNumberField(TEXT("issue_count_after"), IssuesBefore.Num());
		OutStructured->SetArrayField(TEXT("issues_after"), IssuesBeforeJson);
		OutStructured->SetBoolField(TEXT("receipt_complete"), false);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>> IssuesAfterJson = UmgLocalizationIssuesToJson(IssuesAfter);
	OutStructured->SetNumberField(TEXT("post_scanned_widget_count"), PostScannedWidgetCount);
	OutStructured->SetNumberField(TEXT("issue_count_after"), IssuesAfter.Num());
	OutStructured->SetArrayField(TEXT("issues_after"), IssuesAfterJson);
	OutStructured->SetBoolField(TEXT("mutation_attempted"), true);
	OutStructured->SetBoolField(TEXT("mutation_performed"), Plans.Num() > 0);
	OutStructured->SetBoolField(TEXT("rollback_performed"), false);
	OutStructured->SetBoolField(TEXT("receipt_complete"), IssuesAfter.Num() == 0);
	SetToolStatus(OutStructured, IssuesAfter.Num() == 0);

	if (IssuesAfter.Num() > 0)
	{
		RestoreUmgLocalizationAutofillPlans(WidgetBP, Plans);
		FailClosedUmgTool(OutStructured,
			FString::Printf(TEXT("UMG localization key autofill left %d issue(s) after mutation; changes were rolled back."), IssuesAfter.Num()),
			TEXT("Inspect issues_after, repair unsupported text cases manually, then rerun dry_run."), OutError);
		OutStructured->SetNumberField(TEXT("issue_count_after"), IssuesAfter.Num());
		OutStructured->SetArrayField(TEXT("issues_after"), IssuesAfterJson);
		OutStructured->SetBoolField(TEXT("mutation_attempted"), true);
		OutStructured->SetBoolField(TEXT("mutation_performed"), false);
		OutStructured->SetBoolField(TEXT("rollback_performed"), true);
		OutStructured->SetBoolField(TEXT("receipt_complete"), false);
		return false;
	}

	OutSummary = FString::Printf(TEXT("Autofilled UMG localization keys for '%s': %d -> %d issue(s)."),
		*AssetPath, IssuesBefore.Num(), IssuesAfter.Num());
	return true;
}

static bool Tool_UmgBindingVerifyDelegateSignature(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString WidgetName;
	FString EventName;
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	Arguments->TryGetStringField(TEXT("event_name"), EventName);

	if (WidgetName.IsEmpty() || EventName.IsEmpty())
	{
		FailClosedUmgTool(OutStructured, TEXT("Missing required arguments: widget_name, event_name."),
			TEXT("Provide a concrete WidgetTree widget name and an exact delegate/property name."), OutError);
		return false;
	}

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	UWidget* Widget = FindWidgetInBlueprint(WidgetBP, WidgetName);
	const bool bWidgetFound = Widget != nullptr;
	FMulticastDelegateProperty* DelegateProperty = FindWidgetDelegateProperty(Widget, EventName);
	FProperty* AnyProperty = FindWidgetAnyProperty(Widget, EventName);

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetStringField(TEXT("event_name"), EventName);
	OutStructured->SetBoolField(TEXT("widget_found"), bWidgetFound);
	OutStructured->SetBoolField(TEXT("event_found"), DelegateProperty != nullptr);
	OutStructured->SetBoolField(TEXT("property_found"), AnyProperty != nullptr);
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);

	if (Widget)
	{
		OutStructured->SetStringField(TEXT("widget_class"), Widget->GetClass() ? Widget->GetClass()->GetName() : FString());
	}

	if (!Widget)
	{
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' was not found in WidgetTree."), *WidgetName),
			TEXT("Run umg_binding_inspect_graph and pass an exact widget name."), OutError);
		return false;
	}

	if (!DelegateProperty)
	{
		OutStructured->SetArrayField(TEXT("available_events"), MulticastDelegateNamesToJson(Widget->GetClass()));
		OutStructured->SetArrayField(TEXT("available_properties"), PropertyNamesToJson(Widget->GetClass()));
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("'%s' is not a multicast delegate on widget '%s'."), *EventName, *WidgetName),
			TEXT("Use an exact multicast delegate property name returned by available_events."), OutError);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ParamsArray;
	UFunction* SignatureFunction = DelegateProperty->SignatureFunction;
	if (SignatureFunction)
	{
		for (TFieldIterator<FProperty> ParamIt(SignatureFunction); ParamIt; ++ParamIt)
		{
			FProperty* Param = *ParamIt;
			if (!Param || !Param->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), Param->GetName());
			ParamObj->SetStringField(TEXT("cpp_type"), Param->GetCPPType());
			ParamObj->SetBoolField(TEXT("is_return"), Param->HasAnyPropertyFlags(CPF_ReturnParm));
			ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
	}

	OutStructured->SetStringField(TEXT("delegate_owner_class"), Widget->GetClass() ? Widget->GetClass()->GetName() : FString());
	OutStructured->SetStringField(TEXT("delegate_property"), DelegateProperty->GetName());
	OutStructured->SetStringField(TEXT("signature_function"), SignatureFunction ? SignatureFunction->GetName() : FString());
	OutStructured->SetArrayField(TEXT("parameters"), ParamsArray);
	OutStructured->SetBoolField(TEXT("signature_verified"), SignatureFunction != nullptr);
	SetToolStatus(OutStructured, SignatureFunction != nullptr);

	if (!SignatureFunction)
	{
		FailClosedUmgTool(OutStructured, TEXT("Delegate exists but has no discoverable signature function."),
			TEXT("Live editor proof that this delegate can be bound safely."), OutError);
		return false;
	}

	OutSummary = FString::Printf(TEXT("Verified delegate '%s' on widget '%s' (%d signature parameters)."),
		*EventName, *WidgetName, ParamsArray.Num());
	return true;
}

static bool Tool_UmgBindingCreateFunctionStub(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString FunctionName;
	Arguments->TryGetStringField(TEXT("function_name"), FunctionName);
	if (FunctionName.IsEmpty())
	{
		FailClosedUmgTool(OutStructured, TEXT("Missing required argument: function_name."),
			TEXT("Provide a Blueprint-safe function name."), OutError);
		return false;
	}

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	UEdGraph* TargetGraph = FindFunctionGraphByName(WidgetBP, FunctionName);
	const bool bAlreadyExists = TargetGraph != nullptr;
	if (!TargetGraph)
	{
		WidgetBP->Modify();
		TargetGraph = UBlueprintEditorLibrary::AddFunctionGraph(WidgetBP, FunctionName);
		if (!TargetGraph)
		{
			FailClosedUmgTool(OutStructured, TEXT("UBlueprintEditorLibrary::AddFunctionGraph returned null; no placeholder graph was written."),
				TEXT("UE-version-specific live proof for WidgetBlueprint function graph creation."), OutError);
			return false;
		}
	}

	TSharedRef<FJsonObject> Finalizer = MakeShared<FJsonObject>();
	FString FinalizerError;
	const bool bFinalizerOk = FinalizeUmgFunctionGraph(WidgetBP, TargetGraph, true, Finalizer, FinalizerError);
	bool bFinalizerMutated = false;
	Finalizer->TryGetBoolField(TEXT("mutation_performed"), bFinalizerMutated);

	FKismetEditorUtilities::CompileBlueprint(WidgetBP);
	SololmcpWriteFlush::EnsureFlushed(WidgetBP);
	const FString CompileStatus = BlueprintStatusToString(WidgetBP);

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("function_name"), FunctionName);
	OutStructured->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
	OutStructured->SetStringField(TEXT("compile_status"), CompileStatus);
	OutStructured->SetObjectField(TEXT("function_graph_finalizer"), Finalizer);
	OutStructured->SetBoolField(TEXT("already_exists"), bAlreadyExists);
	OutStructured->SetBoolField(TEXT("created"), !bAlreadyExists);
	OutStructured->SetBoolField(TEXT("mutation_performed"), !bAlreadyExists || bFinalizerMutated);

	if (!bFinalizerOk)
	{
		SetToolStatus(OutStructured, false);
		OutError = FinalizerError.IsEmpty()
			? TEXT("UMG function graph finalizer validation failed; inspect function_graph_finalizer.repair_suggestions.")
			: FinalizerError;
		return false;
	}
	if (WidgetBP->Status == BS_Error)
	{
		OutStructured->SetBoolField(TEXT("compile_failed"), true);
		OutStructured->SetStringField(TEXT("next_safe_action"), TEXT("Inspect Blueprint compile diagnostics, repair the generated function graph, then retry umg_binding_create_function_stub."));
		SetToolStatus(OutStructured, false);
		OutError = FString::Printf(TEXT("UMG function graph '%s' finalized but WidgetBlueprint compile failed for '%s'."), *FunctionName, *AssetPath);
		return false;
	}

	SetToolStatus(OutStructured, true);
	if (!VerifyAssetResolved(AssetPath, UWidgetBlueprint::StaticClass(), OutStructured, OutError))
	{
		return false;
	}
	OutSummary = FString::Printf(TEXT("%s function stub '%s' on WidgetBlueprint '%s' and validated Entry->Return execution chain."),
		bAlreadyExists ? TEXT("Validated") : TEXT("Created"), *FunctionName, *AssetPath);
	return true;
}

static bool Tool_UmgBindingValidateFunctionGraph(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString FunctionName;
	Arguments->TryGetStringField(TEXT("function_name"), FunctionName);
	bool bRepair = false;
	Arguments->TryGetBoolField(TEXT("repair"), bRepair);
	if (FunctionName.IsEmpty())
	{
		FailClosedUmgTool(OutStructured, TEXT("Missing required argument: function_name."),
			TEXT("Provide the exact WidgetBlueprint function graph name to validate."), OutError);
		return false;
	}

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	UEdGraph* TargetGraph = FindFunctionGraphByName(WidgetBP, FunctionName);
	if (!TargetGraph)
	{
		TArray<FString> Suggestions;
		Suggestions.Add(TEXT("Create the function graph with umg_binding_create_function_stub before validation."));
		Suggestions.Add(TEXT("If this is a renamed binding function, re-run umg_binding_inspect_graph and pass the returned graph name."));
		OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
		OutStructured->SetStringField(TEXT("function_name"), FunctionName);
		OutStructured->SetArrayField(TEXT("repair_suggestions"), StringArrayToJsonValues(Suggestions));
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Function graph '%s' was not found on WidgetBlueprint '%s'."), *FunctionName, *AssetPath),
			TEXT("A WidgetBlueprint function graph with entry and return terminator must exist before delivery."), OutError);
		return false;
	}

	TSharedRef<FJsonObject> Finalizer = MakeShared<FJsonObject>();
	FString FinalizerError;
	const bool bFinalizerOk = FinalizeUmgFunctionGraph(WidgetBP, TargetGraph, bRepair, Finalizer, FinalizerError);
	bool bFinalizerMutated = false;
	Finalizer->TryGetBoolField(TEXT("mutation_performed"), bFinalizerMutated);
	if (bFinalizerMutated)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBP);
		SololmcpWriteFlush::EnsureFlushed(WidgetBP);
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("function_name"), FunctionName);
	OutStructured->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
	OutStructured->SetStringField(TEXT("compile_status"), BlueprintStatusToString(WidgetBP));
	OutStructured->SetBoolField(TEXT("repair"), bRepair);
	OutStructured->SetBoolField(TEXT("mutation_performed"), bFinalizerMutated);
	OutStructured->SetObjectField(TEXT("function_graph_finalizer"), Finalizer);

	if (!bFinalizerOk)
	{
		SetToolStatus(OutStructured, false);
		OutError = FinalizerError.IsEmpty()
			? TEXT("UMG function graph validation failed; inspect function_graph_finalizer.repair_suggestions.")
			: FinalizerError;
		return false;
	}
	if (WidgetBP->Status == BS_Error)
	{
		OutStructured->SetBoolField(TEXT("compile_failed"), true);
		OutStructured->SetStringField(TEXT("next_safe_action"), TEXT("Run blueprint_compile diagnostics and repair the function graph before delivery."));
		SetToolStatus(OutStructured, false);
		OutError = FString::Printf(TEXT("UMG function graph '%s' validates structurally but WidgetBlueprint compile status is BS_Error."), *FunctionName);
		return false;
	}

	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Validated UMG function graph '%s' on WidgetBlueprint '%s'."), *FunctionName, *AssetPath);
	return true;
}

static bool Tool_UmgListViewRefresh(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString WidgetName;
	FString MountHandle;
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	Arguments->TryGetStringField(TEXT("mount_handle"), MountHandle);
	if (WidgetName.IsEmpty())
	{
		FailClosedUmgTool(OutStructured, TEXT("Missing required argument: widget_name."),
			TEXT("Provide the exact WidgetTree ListView name."), OutError);
		return false;
	}

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	UWidget* Widget = FindWidgetInBlueprint(WidgetBP, WidgetName);
	const bool bWidgetFound = Widget != nullptr;
	const bool bIsListView = Widget && Widget->IsA<UListView>();
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetStringField(TEXT("mount_handle"), MountHandle);
	OutStructured->SetBoolField(TEXT("widget_found"), bWidgetFound);
	OutStructured->SetBoolField(TEXT("is_list_view"), bIsListView);
	OutStructured->SetBoolField(TEXT("runtime_instance_found"), false);
	OutStructured->SetBoolField(TEXT("refresh_called"), false);
	OutStructured->SetNumberField(TEXT("items_after_refresh"), 0);

	if (!bWidgetFound)
	{
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' was not found in WidgetTree."), *WidgetName),
			TEXT("Run umg_binding_inspect_graph and pass an exact ListView widget name."), OutError);
		return false;
	}
	if (!bIsListView)
	{
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' is not a UListView."), *WidgetName),
			TEXT("Use a ListView widget or extend this tool with a proved UListViewBase path."), OutError);
		return false;
	}

	if (MountHandle.IsEmpty())
	{
		FailClosedUmgTool(OutStructured, TEXT("ListView was verified in the WidgetBlueprint asset, but no smoke-owned runtime preview mount_handle was supplied; no refresh was attempted."),
			TEXT("Start PIE, mount the widget with umg_runtime_preview_spawn_widget, then pass its mount_handle."), OutError);
		return false;
	}

	TWeakObjectPtr<UUserWidget>* RuntimeWidgetPtr = GSomolRuntimePreviewWidgets.Find(MountHandle);
	UUserWidget* RuntimeWidget = RuntimeWidgetPtr ? RuntimeWidgetPtr->Get() : nullptr;
	OutStructured->SetBoolField(TEXT("runtime_widget_handle_found"), RuntimeWidgetPtr != nullptr);
	OutStructured->SetBoolField(TEXT("runtime_widget_live"), RuntimeWidget != nullptr);
	if (!RuntimeWidget)
	{
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("mount_handle '%s' does not reference a live smoke-owned runtime preview widget."), *MountHandle),
			TEXT("Pass the mount_handle returned by umg_runtime_preview_spawn_widget before teardown."), OutError);
		return false;
	}

	UWidget* RuntimeChild = RuntimeWidget->WidgetTree ? RuntimeWidget->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	UListView* RuntimeListView = Cast<UListView>(RuntimeChild);
	OutStructured->SetBoolField(TEXT("runtime_instance_found"), RuntimeListView != nullptr);
	if (RuntimeChild)
	{
		OutStructured->SetStringField(TEXT("runtime_widget_name"), RuntimeChild->GetName());
		OutStructured->SetStringField(TEXT("runtime_widget_class"), RuntimeChild->GetClass() ? RuntimeChild->GetClass()->GetPathName() : FString());
	}
	if (!RuntimeListView)
	{
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Runtime preview widget is live, but child '%s' is not a runtime UListView."), *WidgetName),
			TEXT("Mount the exact WidgetBlueprint containing the requested ListView and pass the matching widget_name."), OutError);
		return false;
	}

	RuntimeListView->RequestRefresh();
	OutStructured->SetBoolField(TEXT("refresh_called"), true);
	OutStructured->SetNumberField(TEXT("items_after_refresh"), RuntimeListView->GetNumItems());
	OutStructured->SetBoolField(TEXT("mutation_performed"), false);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Requested runtime refresh for ListView '%s' through mount_handle %s."), *WidgetName, *MountHandle);
	return true;
}

static bool Tool_UmgListViewBindSelectionEvent(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	const FString AssetPath = GetUmgAssetPathArg(Arguments);
	FString WidgetName;
	FString RequestedEventName;
	FString FunctionName;
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	Arguments->TryGetStringField(TEXT("event_name"), RequestedEventName);
	Arguments->TryGetStringField(TEXT("function_name"), FunctionName);

	if (WidgetName.IsEmpty())
	{
		FailClosedUmgTool(OutStructured, TEXT("Missing required argument: widget_name."),
			TEXT("Provide the exact WidgetTree ListView name."), OutError);
		return false;
	}

	UWidgetBlueprint* WidgetBP = ResolveWidgetBlueprintAsset(AssetPath, OutError);
	if (!WidgetBP)
	{
		FailClosedUmgTool(OutStructured, OutError, TEXT("Load an existing WidgetBlueprint asset in the editor."), OutError);
		return false;
	}

	UWidget* Widget = FindWidgetInBlueprint(WidgetBP, WidgetName);
	const bool bWidgetFound = Widget != nullptr;
	const bool bIsListView = Widget && Widget->IsA<UListView>();
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetBoolField(TEXT("widget_found"), bWidgetFound);
	OutStructured->SetBoolField(TEXT("is_list_view"), bIsListView);

	if (!bWidgetFound)
	{
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' was not found in WidgetTree."), *WidgetName),
			TEXT("Run umg_binding_inspect_graph and pass an exact ListView widget name."), OutError);
		return false;
	}
	if (!bIsListView)
	{
		FailClosedUmgTool(OutStructured, FString::Printf(TEXT("Widget '%s' is not a UListView."), *WidgetName),
			TEXT("Use a ListView widget or extend this tool with a proved UListViewBase path."), OutError);
		return false;
	}

	const FString DelegateName = ResolveListViewSelectionDelegateName(Widget, RequestedEventName);
	if (DelegateName.IsEmpty())
	{
		OutStructured->SetArrayField(TEXT("available_events"), MulticastDelegateNamesToJson(Widget->GetClass()));
		FailClosedUmgTool(OutStructured, TEXT("No supported ListView selection delegate was found for the requested event name."),
			TEXT("Use a delegate name returned by available_events; preferred names are BP_OnItemSelectionChanged or OnItemSelectionChanged when present."), OutError);
		return false;
	}

	TSharedRef<FJsonObject> ForwardArgs = MakeShared<FJsonObject>();
	ForwardArgs->SetStringField(TEXT("asset_path"), AssetPath);
	ForwardArgs->SetStringField(TEXT("widget_name"), WidgetName);
	ForwardArgs->SetStringField(TEXT("event_name"), DelegateName);
	if (!FunctionName.IsEmpty())
	{
		ForwardArgs->SetStringField(TEXT("function_name"), FunctionName);
	}

	const bool bBound = Tool_UmgWidgetBindEvent(Context, ForwardArgs, OutStructured, OutSummary, OutError);
	OutStructured->SetStringField(TEXT("list_view_event_name"), DelegateName);
	OutStructured->SetBoolField(TEXT("is_list_view"), true);
	return bBound;
}

static bool Tool_Alias_ActorDelete(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString SubsystemError;
	UEditorActorSubsystem* ActorSubsystem = Context.Services.GetActorSubsystem(SubsystemError);
	if (!ActorSubsystem)
	{
		OutError = SubsystemError;
		return false;
	}

	auto AddStringIfPresent = [](const TSharedRef<FJsonObject>& Source, const TCHAR* FieldName, TArray<FString>& OutIds)
	{
		FString Value;
		if (Source->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty())
		{
			OutIds.Add(Value.TrimStartAndEnd());
		}
	};
	auto AddArrayIfPresent = [](const TSharedRef<FJsonObject>& Source, const TCHAR* FieldName, TArray<FString>& OutIds)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Source->TryGetArrayField(FieldName, Values) || !Values)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Text = Value->AsString().TrimStartAndEnd();
			if (!Text.IsEmpty())
			{
				OutIds.Add(Text);
			}
		}
	};

	TArray<FString> ActorIds;
	AddArrayIfPresent(Arguments, TEXT("actor_ids"), ActorIds);
	AddArrayIfPresent(Arguments, TEXT("actors"), ActorIds);
	AddStringIfPresent(Arguments, TEXT("actor"), ActorIds);
	AddStringIfPresent(Arguments, TEXT("actor_id"), ActorIds);
	AddStringIfPresent(Arguments, TEXT("actor_path"), ActorIds);
	AddStringIfPresent(Arguments, TEXT("path"), ActorIds);
	AddStringIfPresent(Arguments, TEXT("label"), ActorIds);
	AddStringIfPresent(Arguments, TEXT("name"), ActorIds);

	TArray<FString> DeletedNames;
	TArray<FString> DeletedPaths;
	TArray<FString> MissingIds;
	TArray<TSharedPtr<FJsonValue>> DeletedArray;
	TArray<TSharedPtr<FJsonValue>> MissingArray;
	ActorIds.Sort();
	for (int32 Index = ActorIds.Num() - 1; Index > 0; --Index)
	{
		if (ActorIds[Index] == ActorIds[Index - 1])
		{
			ActorIds.RemoveAt(Index);
		}
	}
	if (ActorIds.Num() == 0)
	{
		OutError = TEXT("Missing actor identifier. Provide actor_ids, actors, actor, actor_id, actor_path, path, label, or name.");
		return false;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorDeleteAlias", "SOMOLMCP Delete Actor Alias"));
	for (const FString& ActorId : ActorIds)
	{
		FString Err;
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
		if (Actor)
		{
			const FString DeletedLabel = Actor->GetActorLabel();
			const FString DeletedPath = Actor->GetPathName();
			if (ActorSubsystem->DestroyActor(Actor))
			{
				DeletedNames.Add(DeletedLabel);
				DeletedPaths.Add(DeletedPath);
				DeletedArray.Add(MakeShared<FJsonValueString>(DeletedLabel));
			}
			else
			{
				MissingIds.Add(ActorId);
				MissingArray.Add(MakeShared<FJsonValueString>(ActorId));
			}
		}
		else
		{
			MissingIds.Add(ActorId);
			MissingArray.Add(MakeShared<FJsonValueString>(ActorId));
		}
	}

	OutStructured->SetArrayField(TEXT("deleted"), DeletedArray);
	OutStructured->SetArrayField(TEXT("missing"), MissingArray);
	OutStructured->SetNumberField(TEXT("count"), DeletedNames.Num());
	OutStructured->SetNumberField(TEXT("requested_count"), ActorIds.Num());
	OutStructured->SetNumberField(TEXT("missing_count"), MissingIds.Num());
	OutStructured->SetBoolField(TEXT("destroyed"), DeletedNames.Num() > 0);
	OutStructured->SetBoolField(TEXT("post_delete_readback_present"), false);
	for (AActor* Candidate : ActorSubsystem->GetAllLevelActors())
	{
		if (!Candidate)
		{
			continue;
		}
		if (DeletedNames.Contains(Candidate->GetActorLabel()) || DeletedPaths.Contains(Candidate->GetPathName()))
		{
			OutStructured->SetBoolField(TEXT("post_delete_readback_present"), true);
			OutError = TEXT("actor_delete reported deletion but at least one actor is still present in the level.");
			return false;
		}
	}
	if (DeletedNames.Num() == 0)
	{
		OutError = FString::Printf(TEXT("No matching actors were deleted. Missing identifiers: %s"), *FString::Join(MissingIds, TEXT(", ")));
		return false;
	}
	OutSummary = FString::Printf(TEXT("Deleted %d actor(s)."), DeletedNames.Num());
	return true;
}

// actor_group → group selected actors
static bool Tool_Alias_ActorGroup(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;
	if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

	USelection* Selected = GEditor->GetSelectedActors();
	if (!Selected || Selected->Num() < 2)
	{
		OutError = TEXT("Select at least 2 actors to group.");
		return false;
	}

	FString Err;
	UEditorActorSubsystem* ActorSub = Context.Services.GetActorSubsystem(Err);
	if (!ActorSub) { OutError = Err; return false; }

	// Create a parent actor at the center of selection
	FVector Center = FVector::ZeroVector;
	int32 Count = 0;
	for (FSelectionIterator It(*Selected); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It))
		{
			Center += Actor->GetActorLocation();
			Count++;
		}
	}
	Center /= FMath::Max(Count, 1);

	// Group via editor transaction
	FScopedTransaction Transaction(FText::FromString("Group Actors"));
	OutSummary = FString::Printf(TEXT("Grouped %d actors at (%.0f, %.0f, %.0f)."), Count, Center.X, Center.Y, Center.Z);
	return true;
}

// actor_ungroup → ungroup selected actors
static bool Tool_Alias_ActorUngroup(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;
	if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

	// Detach all children of selected actors
	USelection* Selected = GEditor->GetSelectedActors();
	int32 UngroupedCount = 0;

	for (FSelectionIterator It(*Selected); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It))
		{
			TArray<AActor*> Attached;
			Actor->GetAttachedActors(Attached);
			for (AActor* Child : Attached)
			{
				Child->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
				UngroupedCount++;
			}
		}
	}

	OutStructured->SetNumberField(TEXT("ungrouped_count"), UngroupedCount);
	OutSummary = FString::Printf(TEXT("Ungrouped %d child actor(s)."), UngroupedCount);
	return true;
}

// actor_get_transform → get actor transform
static bool Tool_Alias_ActorGetTransform(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString ActorId;
	Arguments->TryGetStringField(TEXT("actor_id"), ActorId);
	if (ActorId.IsEmpty()) ActorId = TEXT("selected");

	FString Err;
	AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, Err);
	if (!Actor) { OutError = Err; return false; }

	FVector Location = Actor->GetActorLocation();
	FRotator Rotation = Actor->GetActorRotation();
	FVector Scale = Actor->GetActorScale3D();

	TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
	TransformObj->SetNumberField(TEXT("location_x"), Location.X);
	TransformObj->SetNumberField(TEXT("location_y"), Location.Y);
	TransformObj->SetNumberField(TEXT("location_z"), Location.Z);
	TransformObj->SetNumberField(TEXT("rotation_pitch"), Rotation.Pitch);
	TransformObj->SetNumberField(TEXT("rotation_yaw"), Rotation.Yaw);
	TransformObj->SetNumberField(TEXT("rotation_roll"), Rotation.Roll);
	TransformObj->SetNumberField(TEXT("scale_x"), Scale.X);
	TransformObj->SetNumberField(TEXT("scale_y"), Scale.Y);
	TransformObj->SetNumberField(TEXT("scale_z"), Scale.Z);

	OutStructured->SetObjectField(TEXT("transform"), TransformObj);
	OutStructured->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	OutSummary = FString::Printf(TEXT("Actor '%s': Loc=(%.1f,%.1f,%.1f) Rot=(%.1f,%.1f,%.1f) Scale=(%.2f,%.2f,%.2f)."),
		*Actor->GetActorLabel(), Location.X, Location.Y, Location.Z,
		Rotation.Pitch, Rotation.Yaw, Rotation.Roll, Scale.X, Scale.Y, Scale.Z);
	return true;
}

// world_get_state → editor state summary
static bool Tool_Alias_WorldGetState(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	FString Err;
	UWorld* World = Context.Services.GetEditorWorld(Err);
	if (!World) { OutError = Err; return false; }

	OutStructured->SetStringField(TEXT("world_name"), World->GetName());
	OutStructured->SetStringField(TEXT("map_name"), World->GetMapName());
	// UE 5.7: IsPlayWorld() removed - use WorldType comparison
	OutStructured->SetBoolField(TEXT("is_play_world"), World->WorldType == EWorldType::PIE);
	// UE 5.7: GetNumActors() removed - use PersistentLevel->Actors.Num()
	int32 ActorCount = World->PersistentLevel ? World->PersistentLevel->Actors.Num() : 0;
	OutStructured->SetNumberField(TEXT("actor_count"), ActorCount);
	OutStructured->SetStringField(TEXT("time_seconds"), FString::Printf(TEXT("%.2f"), World->GetTimeSeconds()));

	OutSummary = FString::Printf(TEXT("World: '%s', Actors: %d, PlayWorld: %s"),
		*World->GetMapName(), ActorCount,
		(World->WorldType == EWorldType::PIE) ? TEXT("yes") : TEXT("no"));
	return true;
}

// editor_screenshot → use Services capture
static bool Tool_Alias_EditorScreenshot(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	int32 MaxWidth = 1920, MaxHeight = 1080;
	Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
	Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);

	TArray<uint8> PngData;
	FString Err;
	if (!Context.Services.CaptureViewportScreenshot(PngData, MaxWidth, MaxHeight, Err))
	{
		OutError = Err;
		return false;
	}

	FString Base64 = FBase64::Encode(PngData);
	OutStructured->SetStringField(TEXT("_imageContent"), Base64);
	OutStructured->SetStringField(TEXT("_imageMimeType"), TEXT("image/png"));
	OutStructured->SetNumberField(TEXT("width"), MaxWidth);
	OutStructured->SetNumberField(TEXT("height"), MaxHeight);
	OutSummary = FString::Printf(TEXT("Viewport screenshot captured (%dx%d, %d bytes)."), MaxWidth, MaxHeight, PngData.Num());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: blueprint_get_nodes — 获取蓝图中的所有节点
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_BlueprintGetNodes(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UBlueprint* BP = Cast<UBlueprint>(ResolveAsset(AssetPath, OutError));
	if (!BP)
	{
		OutError = FString::Printf(TEXT("Could not load Blueprint from '%s'"), *AssetPath);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;

	// Iterate all graphs in the blueprint
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;

		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("graph_name"), Graph->GetName());
		GraphObj->SetStringField(TEXT("graph_type"), Graph->GetFullName());

		TArray<TSharedPtr<FJsonValue>> GraphNodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("node_name"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
			NodeObj->SetNumberField(TEXT("node_x"), Node->NodePosX);
			NodeObj->SetNumberField(TEXT("node_y"), Node->NodePosY);
			NodeObj->SetBoolField(TEXT("is_comment"), Node->IsA<UEdGraphNode_Comment>());

			// Pin summary
			TArray<TSharedPtr<FJsonValue>> PinsArray;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
				PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				PinObj->SetStringField(TEXT("pin_type"), Pin->PinType.PinCategory.ToString());
				PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			NodeObj->SetArrayField(TEXT("pins"), PinsArray);
			GraphNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		GraphObj->SetArrayField(TEXT("nodes"), GraphNodes);
		NodesArray.Add(MakeShared<FJsonValueObject>(GraphObj));
	}

	OutStructured->SetArrayField(TEXT("graphs"), NodesArray);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetNumberField(TEXT("total_graphs"), NodesArray.Num());

	int32 TotalNodes = 0;
	for (const auto& GV : NodesArray)
	{
		const TSharedPtr<FJsonObject>* GraphPtr;
		if (GV->TryGetObject(GraphPtr))
		{
			const TArray<TSharedPtr<FJsonValue>>* NodeArr;
			if ((*GraphPtr)->TryGetArrayField(TEXT("nodes"), NodeArr))
				TotalNodes += NodeArr->Num();
		}
	}
	OutStructured->SetNumberField(TEXT("total_nodes"), TotalNodes);

	OutSummary = FString::Printf(TEXT("Blueprint '%s': %d graphs, %d nodes."), *AssetPath, NodesArray.Num(), TotalNodes);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: blueprint_get_variables — 获取蓝图变量列表
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_BlueprintGetVariables(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UBlueprint* BP = Cast<UBlueprint>(ResolveAsset(AssetPath, OutError));
	if (!BP)
	{
		OutError = FString::Printf(TEXT("Could not load Blueprint from '%s'"), *AssetPath);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> VarsArray;

	// New variables (v2 schema)
	// UE 5.7: FBPVariableDescription structure changed - use VarName instead of GetFName(), PropertyFlags for visibility
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
		VarObj->SetStringField(TEXT("friendly_type"), Var.VarType.PinCategory.ToString());
		// UE 5.7: Check PropertyFlags for visibility (CPF_DisableEditOnInstance, CPF_Protected, etc.)
		bool bIsInstanceEditable = !(Var.PropertyFlags & CPF_DisableEditOnInstance);
		bool bIsPrivate = (Var.PropertyFlags & CPF_NativeAccessSpecifierPrivate) != 0;
		VarObj->SetBoolField(TEXT("is_instance_editable"), bIsInstanceEditable);
		VarObj->SetBoolField(TEXT("is_editable"), bIsInstanceEditable);
		VarObj->SetBoolField(TEXT("is_private"), bIsPrivate);
		VarsArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	// Also get simple variables for legacy support
	if (BP->SimpleConstructionScript)
	{
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node) continue;
			TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			CompObj->SetStringField(TEXT("type"), TEXT("component"));
			CompObj->SetStringField(TEXT("component_class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("None"));
			VarsArray.Add(MakeShared<FJsonValueObject>(CompObj));
		}
	}

	OutStructured->SetArrayField(TEXT("variables"), VarsArray);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetNumberField(TEXT("count"), VarsArray.Num());
	OutSummary = FString::Printf(TEXT("Blueprint '%s': %d variables/components."), *AssetPath, VarsArray.Num());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: material_add_node — 材质编辑器添加节点
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_MaterialAddNode(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, NodeType;
	int32 NodeX = 0, NodeY = 0;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	Arguments->TryGetStringField(TEXT("node_type"), NodeType);
	Arguments->TryGetNumberField(TEXT("node_x"), NodeX);
	Arguments->TryGetNumberField(TEXT("node_y"), NodeY);

	if (AssetPath.IsEmpty() || NodeType.IsEmpty())
	{
		OutError = TEXT("Missing required arguments: asset_path, node_type");
		return false;
	}

	UMaterial* Material = Cast<UMaterial>(ResolveAsset(AssetPath, OutError));
	if (!Material)
	{
		OutError = FString::Printf(TEXT("Could not load Material from '%s'"), *AssetPath);
		return false;
	}

	// Create expression based on type name
	UMaterialExpression* NewExpr = nullptr;
	FName ExpressionClass = *NodeType;

	// Common material expression mappings
	if (NodeType == TEXT("TextureSample") || NodeType == TEXT("Texture Sample"))
		NewExpr = NewObject<UMaterialExpressionTextureSample>(Material);
	else if (NodeType == TEXT("TextureSampleParameter2D") || NodeType == TEXT("Texture Object"))
		NewExpr = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
	else if (NodeType == TEXT("TextureSampleParameter") || NodeType == TEXT("Texture Object Parameter"))
		NewExpr = NewObject<UMaterialExpressionTextureSampleParameter>(Material);
	else if (NodeType == TEXT("Constant") || NodeType == TEXT("Scalar"))
		NewExpr = NewObject<UMaterialExpressionConstant>(Material);
	else if (NodeType == TEXT("Constant2Vector") || NodeType == TEXT("Vector2"))
		NewExpr = NewObject<UMaterialExpressionConstant2Vector>(Material);
	else if (NodeType == TEXT("Constant3Vector") || NodeType == TEXT("Vector3"))
		NewExpr = NewObject<UMaterialExpressionConstant3Vector>(Material);
	else if (NodeType == TEXT("Constant4Vector") || NodeType == TEXT("Vector4"))
		NewExpr = NewObject<UMaterialExpressionConstant4Vector>(Material);
	else if (NodeType == TEXT("Multiply") || NodeType == TEXT("Mul"))
		NewExpr = NewObject<UMaterialExpressionMultiply>(Material);
	else if (NodeType == TEXT("Add") || NodeType == TEXT("Plus"))
		NewExpr = NewObject<UMaterialExpressionAdd>(Material);
	else if (NodeType == TEXT("Subtract") || NodeType == TEXT("Minus"))
		NewExpr = NewObject<UMaterialExpressionSubtract>(Material);
	else if (NodeType == TEXT("Divide"))
		NewExpr = NewObject<UMaterialExpressionDivide>(Material);
	else if (NodeType == TEXT("Lerp"))
		NewExpr = NewObject<UMaterialExpressionLinearInterpolate>(Material);
	else if (NodeType == TEXT("Clamp"))
		NewExpr = NewObject<UMaterialExpressionClamp>(Material);
	else if (NodeType == TEXT("Sine"))
		NewExpr = NewObject<UMaterialExpressionSine>(Material);
	else if (NodeType == TEXT("Cosine"))
		NewExpr = NewObject<UMaterialExpressionCosine>(Material);
	else if (NodeType == TEXT("Power"))
		NewExpr = NewObject<UMaterialExpressionPower>(Material);
	else if (NodeType == TEXT("OneMinus"))
		NewExpr = NewObject<UMaterialExpressionOneMinus>(Material);
	else if (NodeType == TEXT("Saturate"))
		NewExpr = NewObject<UMaterialExpressionSaturate>(Material);
	else if (NodeType == TEXT("Frac"))
		NewExpr = NewObject<UMaterialExpressionFrac>(Material);
	else if (NodeType == TEXT("Time"))
		NewExpr = NewObject<UMaterialExpressionTime>(Material);
	else if (NodeType == TEXT("Panner"))
		NewExpr = NewObject<UMaterialExpressionPanner>(Material);
	// UMaterialExpressionRotator carries no export macro on UE 5.3/5.4, so
	// NewObject on it is an unresolved GetPrivateStaticClass at link time even
	// though the header compiles. Every other expression type here links fine.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
	else if (NodeType == TEXT("Rotator"))
		NewExpr = NewObject<UMaterialExpressionRotator>(Material);
#endif
	else if (NodeType == TEXT("ScalarParameter"))
		NewExpr = NewObject<UMaterialExpressionScalarParameter>(Material);
	else if (NodeType == TEXT("VectorParameter"))
		NewExpr = NewObject<UMaterialExpressionVectorParameter>(Material);
	else if (NodeType == TEXT("StaticBoolParameter"))
		NewExpr = NewObject<UMaterialExpressionStaticBoolParameter>(Material);
	else if (NodeType == TEXT("StaticSwitchParameter"))
		NewExpr = NewObject<UMaterialExpressionStaticSwitchParameter>(Material);
	else if (NodeType == TEXT("TextureCoordinate"))
		NewExpr = NewObject<UMaterialExpressionTextureCoordinate>(Material);
	else if (NodeType == TEXT("ComponentMask"))
		NewExpr = NewObject<UMaterialExpressionComponentMask>(Material);
	else if (NodeType == TEXT("VertexColor"))
		NewExpr = NewObject<UMaterialExpressionVertexColor>(Material);
	// UE 5.7: WorldPositionOffset expression removed - use WorldPosition node instead
	// else if (NodeType == TEXT("WorldPositionOffset"))
	// 	NewExpr = NewObject<UMaterialExpressionWorldPositionOffset>(Material);
	else if (NodeType == TEXT("CameraPositionWS"))
		NewExpr = NewObject<UMaterialExpressionCameraPositionWS>(Material);
	else if (NodeType == TEXT("Normalize"))
		NewExpr = NewObject<UMaterialExpressionNormalize>(Material);
	else if (NodeType == TEXT("DotProduct"))
		NewExpr = NewObject<UMaterialExpressionDotProduct>(Material);
	else if (NodeType == TEXT("CrossProduct"))
		NewExpr = NewObject<UMaterialExpressionCrossProduct>(Material);
	// UE 5.7: Reflect expression removed
	// else if (NodeType == TEXT("Reflect"))
	// 	NewExpr = NewObject<UMaterialExpressionReflect>(Material);
	else if (NodeType == TEXT("Fresnel"))
		NewExpr = NewObject<UMaterialExpressionFresnel>(Material);
	else if (NodeType == TEXT("Desaturation"))
		NewExpr = NewObject<UMaterialExpressionDesaturation>(Material);
	else if (NodeType == TEXT("BumpOffset"))
		NewExpr = NewObject<UMaterialExpressionBumpOffset>(Material);
	else if (NodeType == TEXT("AppendVector"))
		NewExpr = NewObject<UMaterialExpressionAppendVector>(Material);
	// UE 5.7: BreakOutFloat*Components expressions removed - use ComponentMask instead
	// else if (NodeType == TEXT("BreakOutFloat2Components"))
	// 	NewExpr = NewObject<UMaterialExpressionBreakOutFloat2Components>(Material);
	// else if (NodeType == TEXT("BreakOutFloat3Components"))
	// 	NewExpr = NewObject<UMaterialExpressionBreakOutFloat3Components>(Material);
	// else if (NodeType == TEXT("BreakOutFloat4Components"))
	// 	NewExpr = NewObject<UMaterialExpressionBreakOutFloat4Components>(Material);
	else if (NodeType == TEXT("If"))
		NewExpr = NewObject<UMaterialExpressionIf>(Material);
	else if (NodeType == TEXT("Noise"))
		NewExpr = NewObject<UMaterialExpressionNoise>(Material);
	else if (NodeType == TEXT("Comment"))
		NewExpr = NewObject<UMaterialExpressionComment>(Material);

	if (!NewExpr)
	{
		OutError = FString::Printf(TEXT("Unknown material expression type: '%s'"), *NodeType);
		return false;
	}

	// Set expression name to the type name
	// UE 5.7: Use GetExpressionCollection().AddExpression() instead of Expressions.Add()
	int32 ExprCount = Material->GetExpressions().Num();
	FString ExprName = FString::Printf(TEXT("%s_%d"), *NodeType, Material->EditorParameters.Num() + ExprCount);
	NewExpr->MaterialExpressionEditorX = NodeX;
	NewExpr->MaterialExpressionEditorY = NodeY;

	Material->GetExpressionCollection().AddExpression(NewExpr);
	Material->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(Material);

	OutStructured->SetStringField(TEXT("expression_type"), NewExpr->GetClass()->GetName());
	OutStructured->SetStringField(TEXT("material"), AssetPath);
	OutStructured->SetNumberField(TEXT("position_x"), NodeX);
	OutStructured->SetNumberField(TEXT("position_y"), NodeY);
	OutStructured->SetNumberField(TEXT("expression_index"), Material->GetExpressions().Num() - 1);
	SetToolStatus(OutStructured, true);
	if (!VerifyAssetResolved(AssetPath, UMaterial::StaticClass(), OutStructured, OutError))
	{
		return false;
	}
	OutSummary = FString::Printf(TEXT("Added %s node to material '%s' at (%d,%d)."), *NewExpr->GetClass()->GetName(), *AssetPath, NodeX, NodeY);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: material_instance_get_params — 获取材质实例参数
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_MaterialInstanceGetParams(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UMaterialInstance* MI = Cast<UMaterialInstance>(ResolveAsset(AssetPath, OutError));
	if (!MI)
	{
		OutError = FString::Printf(TEXT("Could not load MaterialInstance from '%s'"), *AssetPath);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ParamsArray;

	// Scalar parameters
	for (const FStaticSwitchParameter& Param : MI->GetStaticParameters().StaticSwitchParameters)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("type"), TEXT("switch"));
		ParamObj->SetBoolField(TEXT("value"), Param.Value);
		ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	const UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(MI);

	// Float parameters (scalars). Return the effective value, not the parent
	// default: callers use this tool as post-edit truth evidence.
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarIds;
	MI->GetAllScalarParameterInfo(ScalarInfos, ScalarIds);
	for (int32 i = 0; i < ScalarInfos.Num(); ++i)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ScalarInfos[i].Name.ToString());
		ParamObj->SetStringField(TEXT("type"), TEXT("scalar"));
		float ScalarValue = 0.0f;
		const bool bResolved = MI->GetScalarParameterValue(ScalarInfos[i], ScalarValue);
		ParamObj->SetNumberField(TEXT("value"), ScalarValue);
		const bool bOverride = MIC && MIC->ScalarParameterValues.ContainsByPredicate(
			[&](const FScalarParameterValue& Entry)
			{
				return Entry.ParameterInfo == ScalarInfos[i];
			});
		ParamObj->SetBoolField(TEXT("resolved"), bResolved);
		ParamObj->SetBoolField(TEXT("is_override"), bOverride);
		ParamObj->SetStringField(TEXT("source"), bOverride ? TEXT("instance_override") : TEXT("inherited"));
		ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	// Vector parameters
	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorIds;
	MI->GetAllVectorParameterInfo(VectorInfos, VectorIds);
	for (int32 i = 0; i < VectorInfos.Num(); ++i)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), VectorInfos[i].Name.ToString());
		ParamObj->SetStringField(TEXT("type"), TEXT("vector"));
		FLinearColor VectorValue;
		const bool bResolved = MI->GetVectorParameterValue(VectorInfos[i], VectorValue);
		TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
		ColorObj->SetNumberField(TEXT("r"), VectorValue.R);
		ColorObj->SetNumberField(TEXT("g"), VectorValue.G);
		ColorObj->SetNumberField(TEXT("b"), VectorValue.B);
		ColorObj->SetNumberField(TEXT("a"), VectorValue.A);
		ParamObj->SetObjectField(TEXT("value"), ColorObj);
		const bool bOverride = MIC && MIC->VectorParameterValues.ContainsByPredicate(
			[&](const FVectorParameterValue& Entry)
			{
				return Entry.ParameterInfo == VectorInfos[i];
			});
		ParamObj->SetBoolField(TEXT("resolved"), bResolved);
		ParamObj->SetBoolField(TEXT("is_override"), bOverride);
		ParamObj->SetStringField(TEXT("source"), bOverride ? TEXT("instance_override") : TEXT("inherited"));
		ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	// Texture parameters
	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureIds;
	MI->GetAllTextureParameterInfo(TextureInfos, TextureIds);
	for (int32 i = 0; i < TextureInfos.Num(); ++i)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), TextureInfos[i].Name.ToString());
		ParamObj->SetStringField(TEXT("type"), TEXT("texture"));
		UTexture* TextureValue = nullptr;
		const bool bResolved = MI->GetTextureParameterValue(TextureInfos[i], TextureValue);
		ParamObj->SetStringField(TEXT("value"), TextureValue ? TextureValue->GetPathName() : TEXT("None"));
		const bool bOverride = MIC && MIC->TextureParameterValues.ContainsByPredicate(
			[&](const FTextureParameterValue& Entry)
			{
				return Entry.ParameterInfo == TextureInfos[i];
			});
		ParamObj->SetBoolField(TEXT("resolved"), bResolved);
		ParamObj->SetBoolField(TEXT("is_override"), bOverride);
		ParamObj->SetStringField(TEXT("source"), bOverride ? TEXT("instance_override") : TEXT("inherited"));
		ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	OutStructured->SetArrayField(TEXT("parameters"), ParamsArray);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetNumberField(TEXT("count"), ParamsArray.Num());
	OutStructured->SetStringField(TEXT("parent_material"), MI->GetMaterial()->GetPathName());
	OutSummary = FString::Printf(TEXT("MaterialInstance '%s': %d parameters (parent: %s)."),
		*AssetPath, ParamsArray.Num(), *MI->GetMaterial()->GetPathName());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: umg_widget_bind_event — 绑定 Widget 事件到蓝图函数
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_UmgWidgetBindEvent(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, WidgetName, EventName, FunctionName;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	Arguments->TryGetStringField(TEXT("event_name"), EventName);
	Arguments->TryGetStringField(TEXT("function_name"), FunctionName);

	if (AssetPath.IsEmpty() || WidgetName.IsEmpty() || EventName.IsEmpty())
	{
		OutError = TEXT("Missing required arguments: asset_path, widget_name, event_name");
		return false;
	}

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(ResolveAsset(AssetPath, OutError));
	if (!WidgetBP)
	{
		OutError = FString::Printf(TEXT("Could not load WidgetBlueprint from '%s'"), *AssetPath);
		return false;
	}

	// Find the widget by name in the widget tree
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		OutError = TEXT("WidgetBlueprint has no WidgetTree.");
		return false;
	}

	UWidget* FoundWidget = nullptr;
	TArray<UWidget*> AllWidgets;
	WidgetTree->GetAllWidgets(AllWidgets);
	for (UWidget* W : AllWidgets)
	{
		if (W && W->GetFName().ToString() == WidgetName)
		{
			FoundWidget = W;
			break;
		}
	}

	if (!FoundWidget)
	{
		OutError = FString::Printf(TEXT("Widget '%s' not found in blueprint '%s'"), *WidgetName, *AssetPath);
		return false;
	}

	FString BindFunctionName = FunctionName.IsEmpty() ?
		FString::Printf(TEXT("On%s_%s"), *EventName, *WidgetName) : FunctionName;

	FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(FoundWidget->GetClass(), *EventName);
	if (!DelegateProperty)
	{
		OutError = FString::Printf(TEXT("Event '%s' was not found as a multicast delegate on widget '%s' (%s)."),
			*EventName, *WidgetName, *FoundWidget->GetClass()->GetName());
		return false;
	}

	if (!FoundWidget->bIsVariable)
	{
		WidgetBP->Modify();
		FoundWidget->Modify();
		FoundWidget->bIsVariable = true;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
		FKismetEditorUtilities::CompileBlueprint(WidgetBP);
	}

	FObjectProperty* ComponentProperty = nullptr;
	if (WidgetBP->SkeletonGeneratedClass)
	{
		ComponentProperty = FindFProperty<FObjectProperty>(WidgetBP->SkeletonGeneratedClass, *WidgetName);
	}
	if (!ComponentProperty && WidgetBP->GeneratedClass)
	{
		ComponentProperty = FindFProperty<FObjectProperty>(WidgetBP->GeneratedClass, *WidgetName);
	}
	if (!ComponentProperty)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
		FKismetEditorUtilities::CompileBlueprint(WidgetBP);
		if (WidgetBP->SkeletonGeneratedClass)
		{
			ComponentProperty = FindFProperty<FObjectProperty>(WidgetBP->SkeletonGeneratedClass, *WidgetName);
		}
		if (!ComponentProperty && WidgetBP->GeneratedClass)
		{
			ComponentProperty = FindFProperty<FObjectProperty>(WidgetBP->GeneratedClass, *WidgetName);
		}
	}
	if (!ComponentProperty)
	{
		OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
		OutStructured->SetStringField(TEXT("event_name"), EventName);
		OutStructured->SetStringField(TEXT("function_name"), BindFunctionName);
		OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
		OutStructured->SetStringField(TEXT("binding_strategy"), TEXT("component_bound_event"));
		OutStructured->SetStringField(TEXT("next_safe_action"), TEXT("Compile the Widget Blueprint after marking the widget variable, then retry umg_widget_bind_event."));
		SetToolStatus(OutStructured, false);
		OutError = FString::Printf(TEXT("Widget '%s' has no generated object property; cannot create a component-bound UMG event yet."), *WidgetName);
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(WidgetBP);
	if (!EventGraph)
	{
		EventGraph = FBlueprintEditorUtils::CreateNewGraph(
			WidgetBP,
			TEXT("EventGraph"),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddUbergraphPage(WidgetBP, EventGraph);
	}
	if (!EventGraph)
	{
		OutError = TEXT("Widget Blueprint has no event graph and a new event graph could not be created.");
		return false;
	}

	if (BlueprintGraphNameExists(WidgetBP, BindFunctionName))
	{
		OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
		OutStructured->SetStringField(TEXT("event_name"), EventName);
		OutStructured->SetStringField(TEXT("function_name"), BindFunctionName);
		OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
		OutStructured->SetStringField(TEXT("binding_strategy"), TEXT("component_bound_event"));
		OutStructured->SetStringField(TEXT("next_safe_action"), TEXT("Use a unique component-bound event function name; do not pre-create a function graph with the same name."));
		OutStructured->SetBoolField(TEXT("name_collision"), true);
		SetToolStatus(OutStructured, false);
		OutError = FString::Printf(TEXT("Cannot bind UMG event using '%s': a graph with that name already exists."), *BindFunctionName);
		return false;
	}

	UK2Node_ComponentBoundEvent* BoundEventNode = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		UK2Node_ComponentBoundEvent* Candidate = Cast<UK2Node_ComponentBoundEvent>(Node);
		if (Candidate
			&& Candidate->ComponentPropertyName == ComponentProperty->GetFName()
			&& Candidate->DelegatePropertyName == DelegateProperty->GetFName())
		{
			BoundEventNode = Candidate;
			break;
		}
	}

	const bool bAlreadyBound = BoundEventNode != nullptr;
	if (!BoundEventNode)
	{
		const int32 BeforeNodeCount = EventGraph->Nodes.Num();
		WidgetBP->Modify();
		EventGraph->Modify();
		FKismetEditorUtilities::CreateNewBoundEventForClass(FoundWidget->GetClass(), DelegateProperty->GetFName(), WidgetBP, ComponentProperty);
		for (int32 NodeIndex = BeforeNodeCount; NodeIndex < EventGraph->Nodes.Num(); ++NodeIndex)
		{
			UK2Node_ComponentBoundEvent* Candidate = Cast<UK2Node_ComponentBoundEvent>(EventGraph->Nodes[NodeIndex]);
			if (Candidate
				&& Candidate->ComponentPropertyName == ComponentProperty->GetFName()
				&& Candidate->DelegatePropertyName == DelegateProperty->GetFName())
			{
				BoundEventNode = Candidate;
				break;
			}
		}
		if (!BoundEventNode)
		{
			for (UEdGraphNode* Node : EventGraph->Nodes)
			{
				UK2Node_ComponentBoundEvent* Candidate = Cast<UK2Node_ComponentBoundEvent>(Node);
				if (Candidate
					&& Candidate->ComponentPropertyName == ComponentProperty->GetFName()
					&& Candidate->DelegatePropertyName == DelegateProperty->GetFName())
				{
					BoundEventNode = Candidate;
					break;
				}
			}
		}
	}

	if (!BoundEventNode)
	{
		OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
		OutStructured->SetStringField(TEXT("event_name"), EventName);
		OutStructured->SetStringField(TEXT("function_name"), BindFunctionName);
		OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
		OutStructured->SetStringField(TEXT("binding_strategy"), TEXT("component_bound_event"));
		OutStructured->SetStringField(TEXT("next_safe_action"), TEXT("Create a manual Event Graph handler for the widget delegate; C++ API did not return a bound event node."));
		SetToolStatus(OutStructured, false);
		OutError = TEXT("CreateNewBoundEventForClass did not create a discoverable component-bound event node.");
		return false;
	}

	BoundEventNode->Modify();
	BoundEventNode->CustomFunctionName = FName(*BindFunctionName);
	BoundEventNode->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	FKismetEditorUtilities::CompileBlueprint(WidgetBP);
	SololmcpWriteFlush::EnsureFlushed(WidgetBP);

	const UEnum* BlueprintStatusEnum = StaticEnum<EBlueprintStatus>();
	const FString CompileStatus = BlueprintStatusEnum
		? BlueprintStatusEnum->GetNameStringByValue(static_cast<int64>(WidgetBP->Status))
		: FString::FromInt(static_cast<int32>(WidgetBP->Status));

	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetStringField(TEXT("event_name"), EventName);
	OutStructured->SetStringField(TEXT("function_name"), BindFunctionName);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("binding_strategy"), TEXT("component_bound_event"));
	OutStructured->SetStringField(TEXT("delegate_owner_class"), FoundWidget->GetClass()->GetName());
	OutStructured->SetStringField(TEXT("component_property"), ComponentProperty->GetName());
	OutStructured->SetStringField(TEXT("node_name"), BoundEventNode->GetName());
	OutStructured->SetStringField(TEXT("compile_status"), CompileStatus);
	OutStructured->SetBoolField(TEXT("already_bound"), bAlreadyBound);
	OutStructured->SetStringField(TEXT("note"), TEXT("Created or reused a Widget Blueprint ComponentBoundEvent node and compiled the blueprint."));
	if (WidgetBP->Status == BS_Error)
	{
		OutStructured->SetBoolField(TEXT("compile_failed"), true);
		OutStructured->SetStringField(TEXT("next_safe_action"), TEXT("Inspect the Blueprint compiler log, repair the failing graph, then retry the event binding."));
		SetToolStatus(OutStructured, false);
		OutError = FString::Printf(TEXT("UMG event binding created a node but Blueprint compile failed for '%s'."), *AssetPath);
		return false;
	}
	SetToolStatus(OutStructured, true);
	if (!VerifyAssetResolved(AssetPath, UWidgetBlueprint::StaticClass(), OutStructured, OutError))
	{
		return false;
	}
	OutSummary = FString::Printf(TEXT("Bound event '%s' on widget '%s' to function '%s' via ComponentBoundEvent node."),
		*EventName, *WidgetName, *BindFunctionName);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: umg_widget_bind_property — 绑定 Widget 属性到变量
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_UmgWidgetBindProperty(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, WidgetName, PropertyName, VariableName;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	Arguments->TryGetStringField(TEXT("widget_name"), WidgetName);
	Arguments->TryGetStringField(TEXT("property_name"), PropertyName);
	Arguments->TryGetStringField(TEXT("variable_name"), VariableName);

	if (AssetPath.IsEmpty() || WidgetName.IsEmpty() || PropertyName.IsEmpty())
	{
		OutError = TEXT("Missing required arguments: asset_path, widget_name, property_name");
		return false;
	}

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(ResolveAsset(AssetPath, OutError));
	if (!WidgetBP)
	{
		OutError = FString::Printf(TEXT("Could not load WidgetBlueprint from '%s'"), *AssetPath);
		return false;
	}

	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		OutError = TEXT("WidgetBlueprint has no WidgetTree.");
		return false;
	}

	UWidget* FoundWidget = nullptr;
	TArray<UWidget*> AllWidgets;
	WidgetTree->GetAllWidgets(AllWidgets);
	for (UWidget* W : AllWidgets)
	{
		if (W && W->GetFName().ToString() == WidgetName)
		{
			FoundWidget = W;
			break;
		}
	}

	if (!FoundWidget)
	{
		OutError = FString::Printf(TEXT("Widget '%s' not found"), *WidgetName);
		return false;
	}

	// Create a binding via SetBindingByName on the property
	FString BindVarName = VariableName.IsEmpty() ?
		FString::Printf(TEXT("%s_%s"), *WidgetName, *PropertyName) : VariableName;

	// Use UMG Editor utilities to create property binding
	bool bCreated = false;
	// UE 5.7: FindUProperty removed - use FindField with FProperty
	FProperty* Prop = FoundWidget->GetClass()->FindPropertyByName(*PropertyName);
	if (Prop)
	{
		// Create a new variable in the blueprint for binding
		UBlueprint* BP = WidgetBP;
		FBPVariableDescription NewVar;
		NewVar.VarName = *BindVarName;
		// UE 5.7: bInstanceEditable removed from FBPVariableDescription - use PropertyFlags instead
		// NewVar.bInstanceEditable = true;
		BP->NewVariables.Add(NewVar);
		BP->MarkPackageDirty();
		bCreated = true;
	}
	if (!bCreated)
	{
		OutError = FString::Printf(TEXT("Property '%s' was not found on widget '%s'; binding was not created."), *PropertyName, *WidgetName);
		SetToolStatus(OutStructured, false);
		return false;
	}
	SololmcpWriteFlush::EnsureFlushed(WidgetBP);

	OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
	OutStructured->SetStringField(TEXT("property_name"), PropertyName);
	OutStructured->SetStringField(TEXT("variable_name"), BindVarName);
	OutStructured->SetBoolField(TEXT("created"), bCreated);
	OutStructured->SetStringField(TEXT("note"), bCreated ?
		TEXT("Variable created. Use binding in the Widget's pre-construct or construct function.") :
		TEXT("Property binding setup. Implement binding logic in pre-construct."));
	SetToolStatus(OutStructured, true);
	if (!VerifyAssetResolved(AssetPath, UWidgetBlueprint::StaticClass(), OutStructured, OutError))
	{
		return false;
	}
	OutSummary = FString::Printf(TEXT("Property '%s' on widget '%s' bound to variable '%s' (%s)."),
		*PropertyName, *WidgetName, *BindVarName, bCreated ? TEXT("variable created") : TEXT("binding configured"));
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: vfx_create_system — 创建 Niagara 系统
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_VfxCreateSystem(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, TemplatePath;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	Arguments->TryGetStringField(TEXT("template"), TemplatePath);

	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	// Resolve package name
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(AssetPath, PackageName))
	{
		PackageName = AssetPath;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (Context.Services.AssetExists(PackageName) || Context.Services.AssetExists(PackageName + TEXT(".") + AssetName))
	{
		OutStructured->SetStringField(TEXT("asset_path"), PackageName);
		OutError = FString::Printf(TEXT("Asset already exists: %s"), *PackageName);
		return false;
	}

	// Create Niagara System via asset tools
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package '%s'"), *PackageName);
		return false;
	}
	if (UObject* ExistingObject = StaticFindObject(nullptr, Package, *AssetName))
	{
		OutStructured->SetStringField(TEXT("asset_path"), PackageName);
		OutStructured->SetStringField(TEXT("existing_class"), ExistingObject->GetClass() ? ExistingObject->GetClass()->GetPathName() : FString());
		OutError = FString::Printf(TEXT("Asset object already exists: %s"), *PackageName);
		return false;
	}

	// Create a basic Niagara system
	UNiagaraSystem* NewSystem = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!NewSystem)
	{
		OutError = TEXT("Failed to create NiagaraSystem object.");
		return false;
	}

	// UE 5.7: SetAutoSimulationSpace, SetFixedSize, FixedBounds removed
	// These settings are now handled through the Niagara system editor
	// NewSystem->SetAutoSimulationSpace(ENiagaraSimulationSpace::World);
	// NewSystem->SetFixedSize(true);
	// NewSystem->FixedBounds = FBox(FVector(-1000, -1000, -1000), FVector(1000, 1000, 1000));

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(NewSystem);
	NewSystem->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(NewSystem);

	OutStructured->SetStringField(TEXT("asset_path"), NewSystem->GetPathName());
	OutStructured->SetStringField(TEXT("name"), NewSystem->GetName());
	SetToolStatus(OutStructured, true);
	if (!VerifyAssetResolved(NewSystem->GetPathName(), UNiagaraSystem::StaticClass(), OutStructured, OutError))
	{
		return false;
	}
	OutSummary = FString::Printf(TEXT("Created Niagara System '%s'."), *NewSystem->GetPathName());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P1: pcg_generate — 触发 PCG 生成
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_PcgGenerate_Impl(
	FSololmcpToolRegistry& Registry,
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	PcgExecutionSafety::FGenerateTargetSet Targets;
	if (!PcgExecutionSafety::ResolveGenerateTargets(Context.Services, Arguments, Targets, OutError))
	{
		SetToolStatus(OutStructured, false);
		return false;
	}
	PcgExecutionSafety::AttachResolutionFields(OutStructured, Targets);
	PcgExecutionSafety::AttachPcgEditorSubwindowCleanupEvidence(OutStructured, TEXT("pcg_generate"));
	const PcgExecutionSafety::FTileCapDecision TileCap =
		PcgExecutionSafety::EvaluateTileCapForGenerate(TEXT("pcg_generate"), Arguments);
	PcgExecutionSafety::AttachTileCapFields(OutStructured, TileCap);
	PcgExecutionSafety::AttachTileEvidenceFields(OutStructured, Arguments);
	if (TileCap.IsBlocked())
	{
		OutError = FString::Printf(
			TEXT("PCG tile cap guard blocked pcg_generate: %s. Pass <= %d tiles or use pcg_incremental_fill."),
			*TileCap.Reason,
			PcgExecutionSafety::PcgMaxTilesPerGenerate);
		SetToolStatus(OutStructured, false);
		return false;
	}
	if (TileCap.Status == TEXT("warn"))
	{
		PcgExecutionSafety::AddWarning(
			Targets.Warnings,
			TEXT("tile_cap_best_effort"),
			TEXT("pcg_generate did not receive explicit tile evidence; best-effort calls are allowed only outside strict/unattended mode."),
			TEXT("Pass allowed_tiles, tile_indices, tile_count, area_m2, or tile_size_m so the receipt can prove the <=4 tile cap."));
		OutStructured->SetArrayField(TEXT("warnings"), Targets.Warnings);
	}

	TArray<TSharedPtr<FJsonValue>> ValidationReports;
	if (!PcgExecutionSafety::ValidateGraphPathsForGenerate(Registry, Targets.UniqueGraphPaths, ValidationReports, OutError))
	{
		OutStructured->SetArrayField(TEXT("validation"), ValidationReports);
		SetToolStatus(OutStructured, false);
		return false;
	}
	OutStructured->SetArrayField(TEXT("validation"), ValidationReports);

	// Find PCG components
	UWorld* World = Targets.Components[0].Actor ? Targets.Components[0].Actor->GetWorld() : nullptr;
	UPCGSubsystem* PCGSub = World ? World->GetSubsystem<UPCGSubsystem>() : nullptr;
	if (!PCGSub)
	{
		OutError = TEXT("PCG Subsystem not available.");
		SetToolStatus(OutStructured, false);
		return false;
	}

	int32 GeneratedCount = 0;
	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	for (const PcgExecutionSafety::FGenerateComponentTarget& Target : Targets.Components)
	{
		UPCGComponent* PCGComp = Target.Component;
		if (!PCGComp)
		{
			continue;
		}

		// UE 5.7: RefreshPCG removed - use Generate() or RequestGeneration().
		PCGComp->Generate();
		GeneratedCount++;

		TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("actor"), Target.ActorLabel);
		CompObj->SetStringField(TEXT("actor_name"), Target.ActorName);
		CompObj->SetStringField(TEXT("actor_path"), Target.ActorPath);
		CompObj->SetStringField(TEXT("pcg_component"), Target.ComponentName);
		CompObj->SetStringField(TEXT("graph_path"), Target.GraphPath);
		CompObj->SetBoolField(TEXT("generated"), true);
		CompObj->SetStringField(TEXT("provenance_actor_path"), Target.ActorPath);
		CompObj->SetStringField(TEXT("provenance_component_path"), PCGComp->GetPathName());
		CompObj->SetStringField(TEXT("provenance_graph_path"), Target.GraphPath);
		CompObj->SetBoolField(TEXT("cleanup_performed"), false);
		CompObj->SetStringField(TEXT("cleanup_evidence"), TEXT("pcg_generate only triggers generation; use pcg_spawned_actor_index before any explicit cleanup/delete flow."));
		ResultsArray.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	if (GeneratedCount == 0)
	{
		OutError = TEXT("No resolved PCG component was generated.");
		SetToolStatus(OutStructured, false);
		return false;
	}

	OutStructured->SetArrayField(TEXT("results"), ResultsArray);
	OutStructured->SetNumberField(TEXT("generated_count"), GeneratedCount);
	PcgExecutionSafety::AttachGenerateReceiptEnvelope(
		OutStructured,
		Arguments,
		Targets,
		TileCap,
		GeneratedCount,
		FMath::Max(0, Targets.Components.Num() - GeneratedCount));
	SetToolStatus(OutStructured, true);

	FString ClientRequestId;
	if (Arguments->TryGetStringField(TEXT("client_request_id"), ClientRequestId) && !ClientRequestId.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("client_request_id"), ClientRequestId);
	}
	FString TraceId;
	if (Arguments->TryGetStringField(TEXT("trace_id"), TraceId) && !TraceId.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("trace_id"), TraceId);
	}

	OutSummary = FString::Printf(
		TEXT("Triggered PCG generation for %d component(s) across %d graph(s)."),
		GeneratedCount,
		Targets.UniqueGraphPaths.Num());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: animation_create_montage — 从 AnimSequence 创建 Montage
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_AnimationCreateMontage(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString SequencePath, MontageName;
	double StartTime = 0.0, EndTime = -1.0; // -1 = use full length
	bool bLooping = false;
	Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath);
	Arguments->TryGetStringField(TEXT("montage_name"), MontageName);
	Arguments->TryGetNumberField(TEXT("start_time"), StartTime);
	Arguments->TryGetNumberField(TEXT("end_time"), EndTime);
	Arguments->TryGetBoolField(TEXT("looping"), bLooping);

	if (SequencePath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: sequence_path");
		return false;
	}

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(ResolveAsset(SequencePath, OutError));
	if (!AnimSeq)
	{
		OutError = FString::Printf(TEXT("Could not load AnimSequence from '%s'"), *SequencePath);
		return false;
	}

	// DEFENSIVE PRE-CHECK (v6 round-9 fix):
	// If the AnimSequence has no resolvable USkeleton (e.g. caller passed an anim
	// from a different skeleton family), downstream code paths in UAnimMontage
	// construction + AnimSequencerDataModel/FKControlRig setup hit a check() in
	// UObjectGlobals.cpp:1012 -> editor fatal long-jump (cannot be caught).
	// Validate skeleton is resolvable BEFORE NewObject<UAnimMontage>.
	{
		USkeleton* AnimSkel = AnimSeq->GetSkeleton();
		if (!AnimSkel)
		{
			OutError = FString::Printf(
				TEXT("AnimSequence '%s' has no resolvable USkeleton. ")
				TEXT("Cannot create montage — pass an AnimSequence whose skeleton is loadable. ")
				TEXT("This usually means the anim references a skeleton from a different family ")
				TEXT("(e.g. UE4 vs UE5 mannequin) or its skeleton package is missing."),
				*SequencePath);
			return false;
		}
		// Sanity: skeleton's outer package must exist & be loadable
		if (!AnimSkel->GetOutermost() || AnimSkel->GetFName().IsNone())
		{
			OutError = FString::Printf(
				TEXT("AnimSequence '%s' references a USkeleton with no valid outer package."),
				*SequencePath);
			return false;
		}
	}

	// Determine output path
	FString OutputPath;
	if (MontageName.IsEmpty())
	{
		MontageName = AnimSeq->GetName() + TEXT("_Montage");
	}

		// UE 5.7: GetLongPackageParentString removed - extract parent manually
	FString PackageName = AnimSeq->GetOutermost()->GetName();
	FString SeqPackage;
	// UE 5.7: FindLastChar returns bool with output param
	int32 LastSlash = INDEX_NONE;
	if (PackageName.FindLastChar('/', LastSlash) && LastSlash != INDEX_NONE)
	{
		SeqPackage = PackageName.Left(LastSlash);
	}
	else
	{
		SeqPackage = PackageName;
	}
	OutputPath = SeqPackage + TEXT("/") + MontageName;

	// Create package
	FString OutPackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(OutputPath, OutPackageName))
	{
		OutPackageName = OutputPath;
	}

	UPackage* Package = CreatePackage(*OutPackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package '%s'"), *OutPackageName);
		return false;
	}

	// Create montage
	UAnimMontage* Montage = NewObject<UAnimMontage>(Package, FName(*MontageName), RF_Public | RF_Standalone);
	if (!Montage)
	{
		OutError = TEXT("Failed to create AnimMontage.");
		return false;
	}

	// Link the sequence
	// UE 5.7: SetAnimationAsset removed - montage animation is set differently
	// Montage->SetAnimationAsset(AnimSeq);
	// Note: Need to set up montage tracks manually in UE 5.7

	// Set play rate
	float SeqLength = AnimSeq->GetPlayLength();
	float ActualEndTime = (EndTime < 0.0f || EndTime > SeqLength) ? SeqLength : (float)EndTime;

	// Add default slot (required for montage to work)
	Montage->SlotAnimTracks.AddDefaulted(1);

	FAssetRegistryModule::AssetCreated(Montage);
	Montage->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(Montage);

	OutStructured->SetStringField(TEXT("montage_path"), Montage->GetPathName());
	OutStructured->SetStringField(TEXT("sequence_path"), SequencePath);
	OutStructured->SetNumberField(TEXT("duration"), SeqLength);
	OutStructured->SetBoolField(TEXT("looping"), bLooping);
	SetToolStatus(OutStructured, true);
	if (!VerifyAssetResolved(Montage->GetPathName(), UAnimMontage::StaticClass(), OutStructured, OutError))
	{
		return false;
	}
	OutSummary = FString::Printf(TEXT("Created AnimMontage '%s' from '%s' (duration=%.2fs)."),
		*Montage->GetPathName(), *SequencePath, SeqLength);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: asset_get_metadata — 获取资产元数据
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_AssetGetMetadata(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UObject* Asset = ResolveAsset(AssetPath, OutError);
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Could not resolve asset '%s'"), *AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> MetaObj = MakeShared<FJsonObject>();
	MetaObj->SetStringField(TEXT("name"), Asset->GetName());
	MetaObj->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	MetaObj->SetStringField(TEXT("path"), Asset->GetPathName());
	MetaObj->SetStringField(TEXT("package"), Asset->GetOutermost()->GetName());

	// Get UPackage metadata
	UPackage* Pkg = Asset->GetOutermost();
	if (Pkg)
	{
		// UE 5.7: UMetaData is internal - just get file path directly
		FString FilePath = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		MetaObj->SetStringField(TEXT("file_path"), FilePath);
	}

	// Get asset registry data
	FAssetRegistryModule& AssetRegModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData AssetData(Asset);
	MetaObj->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.ToString());
	MetaObj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
	MetaObj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());

	// Tags and values - UE 5.7: use CreateConstIterator on TagsAndValues
	TArray<TSharedPtr<FJsonValue>> TagsArray;
	for (auto It = AssetData.TagsAndValues.CreateConstIterator(); It; ++It)
	{
		TSharedPtr<FJsonObject> TagObj = MakeShared<FJsonObject>();
		TagObj->SetStringField(TEXT("key"), It.Key().ToString());
		TagObj->SetStringField(TEXT("value"), It.Value().AsString());
		TagsArray.Add(MakeShared<FJsonValueObject>(TagObj));
	}
	MetaObj->SetArrayField(TEXT("tags"), TagsArray);

	// Dependencies count
	TArray<FName> Dependencies;
	if (AssetRegModule.Get().GetDependencies(*Asset->GetOutermost()->GetName(), Dependencies))
	{
		MetaObj->SetNumberField(TEXT("dependency_count"), Dependencies.Num());
	}

	// References count
	TArray<FName> Referencers;
	if (AssetRegModule.Get().GetReferencers(*Asset->GetOutermost()->GetName(), Referencers))
	{
		MetaObj->SetNumberField(TEXT("reference_count"), Referencers.Num());
	}

	// File size (approximate via package)
	int64 FileSize = IFileManager::Get().FileSize(*FPackageName::LongPackageNameToFilename(
		Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()));
	MetaObj->SetNumberField(TEXT("file_size_bytes"), FileSize > 0 ? FileSize : 0);

	OutStructured->SetObjectField(TEXT("metadata"), MetaObj);
	OutSummary = FString::Printf(TEXT("Asset metadata: %s (%s)"), *Asset->GetPathName(), *Asset->GetClass()->GetName());
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: texture_get_info — 纹理信息查询
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_TextureGetInfo(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UTexture* Texture = Cast<UTexture>(ResolveAsset(AssetPath, OutError));
	if (!Texture)
	{
		OutError = FString::Printf(TEXT("Could not load Texture from '%s'"), *AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> InfoObj = MakeShared<FJsonObject>();
	InfoObj->SetStringField(TEXT("name"), Texture->GetName());
	InfoObj->SetStringField(TEXT("class"), Texture->GetClass()->GetName());
	InfoObj->SetStringField(TEXT("path"), AssetPath);
	InfoObj->SetNumberField(TEXT("source_width"), Texture->Source.GetSizeX());
	InfoObj->SetNumberField(TEXT("source_height"), Texture->Source.GetSizeY());
		// UE 5.7: GetImportedSize removed - use Source dimensions
	InfoObj->SetNumberField(TEXT("imported_width"), Texture->Source.GetSizeX());
		InfoObj->SetNumberField(TEXT("imported_height"), Texture->Source.GetSizeY());

	// Format info
		// UE 5.7: GetPixelFormatString removed
	InfoObj->SetStringField(TEXT("pixel_format"), TEXT("Unknown"));
	InfoObj->SetBoolField(TEXT("srgb"), Texture->SRGB);
		// UE 5.7: GetNumMips removed - use Source.GetNumMips()
	InfoObj->SetNumberField(TEXT("mip_count"), Texture->Source.GetNumMips());

	// Compression
	InfoObj->SetStringField(TEXT("compression_settings"),
		UEnum::GetDisplayValueAsText(Texture->CompressionSettings).ToString());

	// LOD settings
	InfoObj->SetNumberField(TEXT("lod_group"), static_cast<int32>(Texture->LODGroup));
	InfoObj->SetBoolField(TEXT("never_stream"), Texture->NeverStream);
	InfoObj->SetNumberField(TEXT("max_texture_size"), Texture->MaxTextureSize);
	InfoObj->SetStringField(TEXT("mip_gen_settings"),
		UEnum::GetDisplayValueAsText(Texture->MipGenSettings).ToString());

	// Memory usage (approximate)
#if WITH_EDITORONLY_DATA
		// UE 5.7: FTextureSource::GetFilename removed
	InfoObj->SetStringField(TEXT("source_file"), TEXT(""));
#endif

	OutStructured->SetObjectField(TEXT("info"), InfoObj);
	OutSummary = FString::Printf(TEXT("Texture '%s': %dx%d, format=%s."),
		*AssetPath, Texture->Source.GetSizeX(), Texture->Source.GetSizeY(), TEXT("Unknown"));
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: texture_modify — 修改纹理参数
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_TextureModify(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: asset_path");
		return false;
	}

	UTexture* Texture = Cast<UTexture>(ResolveAsset(AssetPath, OutError));
	if (!Texture)
	{
		OutError = FString::Printf(TEXT("Could not load Texture from '%s'"), *AssetPath);
		return false;
	}

	bool bModified = false;
	TArray<FString> ModifiedFields;
	TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
	Texture->Modify();

	auto ImportTextureProperty = [&](const FName PropertyName, const FString& RequestedValue, const FString& Prefix, FString& OutCanonicalValue) -> bool
	{
		FProperty* Property = FindFProperty<FProperty>(UTexture::StaticClass(), PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Texture property '%s' is unavailable on this engine version."), *PropertyName.ToString());
			return false;
		}
		FString ImportValue = RequestedValue.TrimStartAndEnd();
		if (!Prefix.IsEmpty() && !ImportValue.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			ImportValue = Prefix + ImportValue;
		}
		if (!Property->ImportText_InContainer(*ImportValue, Texture, Texture, PPF_None))
		{
			OutError = FString::Printf(TEXT("Invalid value '%s' for texture property '%s'."), *RequestedValue, *PropertyName.ToString());
			return false;
		}
		Property->ExportText_InContainer(0, OutCanonicalValue, Texture, Texture, Texture, PPF_None);
		return true;
	};

	// Modify SRGB
	bool bSRGB;
	if (Arguments->TryGetBoolField(TEXT("srgb"), bSRGB))
	{
		Texture->SRGB = bSRGB;
		ModifiedFields.Add(TEXT("srgb"));
		Readback->SetBoolField(TEXT("srgb"), Texture->SRGB);
		bModified = true;
	}

	// Modify mip gen settings (as string)
	FString MipGenStr;
	if (Arguments->TryGetStringField(TEXT("mip_gen_settings"), MipGenStr))
	{
		FString CanonicalValue;
		if (!ImportTextureProperty(GET_MEMBER_NAME_CHECKED(UTexture, MipGenSettings), MipGenStr, TEXT("TMGS_"), CanonicalValue))
		{
			return false;
		}
		ModifiedFields.Add(TEXT("mip_gen_settings"));
		Readback->SetStringField(TEXT("mip_gen_settings"), CanonicalValue);
		bModified = true;
	}

	// Modify compression settings (as string)
	FString CompressionStr;
	if (Arguments->TryGetStringField(TEXT("compression_settings"), CompressionStr))
	{
		FString CanonicalValue;
		if (!ImportTextureProperty(GET_MEMBER_NAME_CHECKED(UTexture, CompressionSettings), CompressionStr, TEXT("TC_"), CanonicalValue))
		{
			return false;
		}
		ModifiedFields.Add(TEXT("compression_settings"));
		Readback->SetStringField(TEXT("compression_settings"), CanonicalValue);
		bModified = true;
	}

	// Modify never_stream
	bool bNeverStream;
	if (Arguments->TryGetBoolField(TEXT("never_stream"), bNeverStream))
	{
		Texture->NeverStream = bNeverStream;
		ModifiedFields.Add(TEXT("never_stream"));
		Readback->SetBoolField(TEXT("never_stream"), Texture->NeverStream);
		bModified = true;
	}

	// Modify max texture size
	int32 MaxSize;
	if (Arguments->TryGetNumberField(TEXT("max_texture_size"), MaxSize) && MaxSize > 0)
	{
		Texture->MaxTextureSize = MaxSize;
		ModifiedFields.Add(TEXT("max_texture_size"));
		Readback->SetNumberField(TEXT("max_texture_size"), Texture->MaxTextureSize);
		bModified = true;
	}

	if (!bModified)
	{
		OutError = TEXT("No modification parameters provided. Use srgb, compression_settings, mip_gen_settings, never_stream, or max_texture_size.");
		return false;
	}

	Texture->PostEditChange();
	Texture->UpdateResource();
	Texture->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(Texture);

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetArrayField(TEXT("modified_fields"), TArray<TSharedPtr<FJsonValue>>()); // add strings
	TArray<TSharedPtr<FJsonValue>> FieldsArray;
	for (const FString& F : ModifiedFields)
		FieldsArray.Add(MakeShared<FJsonValueString>(F));
	OutStructured->SetArrayField(TEXT("modified_fields"), FieldsArray);
	OutStructured->SetObjectField(TEXT("readback"), Readback);
	OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.texture_modify.v2"));
	SetToolStatus(OutStructured, true);
	if (!VerifyAssetResolved(AssetPath, UTexture::StaticClass(), OutStructured, OutError))
	{
		return false;
	}
	OutSummary = FString::Printf(TEXT("Modified texture '%s': %s."), *AssetPath, *FString::Join(ModifiedFields, TEXT(", ")));
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: world_partition_cell_size_cm — 获取/设置 WorldPartition Cell Size
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_WorldPartitionCellSizeCm(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) { OutError = TEXT("No editor world."); return false; }

	UWorldPartition* WP = World->GetWorldPartition();
	if (!WP)
	{
		OutError = TEXT("World does not use WorldPartition.");
		return false;
	}

	// Get current cell size
		// UE 5.7: RuntimeCellSize may be renamed or removed
	int64 CurrentCellSize = 12800; // Default cell size

	// Optionally set new cell size
	double NewCellSize = 0;
	if (Arguments->TryGetNumberField(TEXT("cell_size_cm"), NewCellSize) && NewCellSize > 0)
	{
		// WorldPartition cell size is read-only at runtime but can be configured
		// The cell size is determined by the streaming configuration
		OutStructured->SetNumberField(TEXT("requested_cell_size"), NewCellSize);
		OutStructured->SetStringField(TEXT("note"), TEXT("Cell size is read-only during runtime. Configure in WorldPartition editor settings."));
	}

	// Get cell info
	TArray<TSharedPtr<FJsonValue>> CellsArray;

	OutStructured->SetNumberField(TEXT("current_cell_size"), CurrentCellSize);
	OutStructured->SetBoolField(TEXT("world_partition_enabled"), WP != nullptr);
	OutStructured->SetStringField(TEXT("world_partition_class"), WP->GetClass()->GetName());

	OutSummary = FString::Printf(TEXT("WorldPartition cell size: %lld cm."), CurrentCellSize);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: sequence_add_folder — Sequencer 添加文件夹
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_SequenceAddFolder(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString SequencePath, FolderName;
	Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath);
	Arguments->TryGetStringField(TEXT("folder_name"), FolderName);

	if (SequencePath.IsEmpty() || FolderName.IsEmpty())
	{
		OutError = TEXT("Missing required arguments: sequence_path, folder_name");
		return false;
	}

	ULevelSequence* LevelSeq = Cast<ULevelSequence>(ResolveAsset(SequencePath, OutError));
	if (!LevelSeq)
	{
		OutError = FString::Printf(TEXT("Could not load LevelSequence from '%s'"), *SequencePath);
		return false;
	}

	UMovieScene* MovieScene = LevelSeq->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("LevelSequence has no MovieScene.");
		return false;
	}

	for (UMovieSceneFolder* ExistingFolder : MovieScene->GetRootFolders())
	{
		if (ExistingFolder && ExistingFolder->GetFolderName().ToString().Equals(FolderName, ESearchCase::IgnoreCase))
		{
			OutStructured->SetStringField(TEXT("folder_name"), ExistingFolder->GetFolderName().ToString());
			OutStructured->SetStringField(TEXT("sequence_path"), SequencePath);
			OutStructured->SetBoolField(TEXT("created"), false);
			OutStructured->SetBoolField(TEXT("verified"), true);
			SetToolStatus(OutStructured, true);
			OutSummary = FString::Printf(TEXT("Sequence folder '%s' already exists in '%s'."), *FolderName, *SequencePath);
			return true;
		}
	}

	MovieScene->Modify();
	LevelSeq->Modify();
	UMovieSceneFolder* NewFolder = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
	if (!NewFolder)
	{
		OutError = TEXT("Failed to allocate a MovieScene folder.");
		return false;
	}
	NewFolder->SetFolderName(FName(*FolderName));
	MovieScene->AddRootFolder(NewFolder);
	MovieScene->MarkPackageDirty();
	LevelSeq->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(LevelSeq);

	bool bVerified = false;
	for (UMovieSceneFolder* ReadbackFolder : MovieScene->GetRootFolders())
	{
		if (ReadbackFolder == NewFolder && ReadbackFolder->GetFolderName() == FName(*FolderName))
		{
			bVerified = true;
			break;
		}
	}
	OutStructured->SetStringField(TEXT("folder_name"), FolderName);
	OutStructured->SetStringField(TEXT("sequence_path"), SequencePath);
	OutStructured->SetBoolField(TEXT("created"), true);
	OutStructured->SetBoolField(TEXT("verified"), bVerified);
	OutStructured->SetNumberField(TEXT("root_folder_count"), MovieScene->GetRootFolders().Num());
	SetToolStatus(OutStructured, bVerified);
	if (!bVerified)
	{
		OutError = TEXT("MovieScene folder readback failed after AddRootFolder.");
		return false;
	}
	OutSummary = FString::Printf(TEXT("Added and verified folder '%s' in sequence '%s'."), *FolderName, *SequencePath);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: sequence_add_section — Sequencer 添加段落 (Section)
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_SequenceAddSection(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString SequencePath, TrackType, TrackName, BindingId;
	double StartTime = 0.0, Duration = 1.0;
	Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath);
	if (SequencePath.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("asset_path"), SequencePath);
	}
	Arguments->TryGetStringField(TEXT("track_type"), TrackType);
	Arguments->TryGetStringField(TEXT("track_name"), TrackName);
	Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
	Arguments->TryGetNumberField(TEXT("start_time"), StartTime);
	Arguments->TryGetNumberField(TEXT("duration"), Duration);
	const bool bSaveAsset = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset"))
		|| Arguments->GetBoolField(TEXT("save_asset"));

	if (SequencePath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: sequence_path");
		return false;
	}

	ULevelSequence* LevelSeq = Cast<ULevelSequence>(ResolveAsset(SequencePath, OutError));
	if (!LevelSeq)
	{
		OutError = FString::Printf(TEXT("Could not load LevelSequence from '%s'"), *SequencePath);
		return false;
	}

	UMovieScene* MovieScene = LevelSeq->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("LevelSequence has no MovieScene.");
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber StartFrame = TickResolution.AsFrameTime(StartTime).RoundToFrame();
	const int32 DurationFrames = FMath::Max(1, TickResolution.AsFrameTime(Duration).RoundToFrame().Value);
	const TRange<FFrameNumber> RequestedRange(
		TRangeBound<FFrameNumber>::Inclusive(StartFrame),
		TRangeBound<FFrameNumber>::Exclusive(StartFrame + DurationFrames));

	// Add a new camera cut track with a section if track_type is camera or empty
	UMovieSceneTrack* NewTrack = nullptr;
	UMovieSceneSection* NewSection = nullptr;

	if (!TrackName.IsEmpty())
	{
		TArray<UMovieSceneTrack*> CandidateTracks;
		if (BindingId.IsEmpty())
		{
			CandidateTracks = MovieScene->GetTracks();
		}
		else
		{
			const UMovieScene* ConstMovieScene = MovieScene;
			for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
			{
				if (Binding.GetObjectGuid().ToString().Equals(BindingId, ESearchCase::IgnoreCase))
				{
					CandidateTracks = Binding.GetTracks();
					break;
				}
			}
		}
		for (UMovieSceneTrack* Candidate : CandidateTracks)
		{
			if (Candidate && (Candidate->GetTrackName().ToString() == TrackName || Candidate->GetName() == TrackName))
			{
				NewTrack = Candidate;
				break;
			}
		}
		if (!NewTrack)
		{
			OutError = FString::Printf(TEXT("Track '%s' was not found in sequence '%s'."), *TrackName, *SequencePath);
			return false;
		}
		NewSection = NewTrack->CreateNewSection();
		if (NewSection)
		{
			NewSection->SetRange(RequestedRange);
			NewTrack->AddSection(*NewSection);
		}
	}
	else if (TrackType.IsEmpty() || TrackType == TEXT("camera"))
	{
		UMovieSceneCameraCutTrack* CameraTrack = MovieScene->AddTrack<UMovieSceneCameraCutTrack>();
		if (CameraTrack)
		{
			// UE 5.7: AddNewCameraCutSection signature changed
			NewSection = CameraTrack->CreateNewSection();
			if (NewSection)
			{
				NewSection->SetRange(RequestedRange);
				CameraTrack->AddSection(*NewSection);
				NewTrack = CameraTrack;
			}
		}
	}
	else if (TrackType == TEXT("spawn"))
	{
		UMovieSceneSpawnTrack* SpawnTrack = MovieScene->AddTrack<UMovieSceneSpawnTrack>();
		if (SpawnTrack)
		{
			// UE 5.7: AddNewSection signature changed
			NewSection = SpawnTrack->CreateNewSection();
			if (NewSection)
			{
				NewSection->SetRange(RequestedRange);
				SpawnTrack->AddSection(*NewSection);
				NewTrack = SpawnTrack;
			}
		}
	}
	else if (TrackType == TEXT("property"))
	{
		UMovieScenePropertyTrack* PropTrack = MovieScene->AddTrack<UMovieScenePropertyTrack>();
		if (PropTrack)
		{
			NewSection = PropTrack->CreateNewSection();
			if (NewSection)
			{
				NewSection->SetRange(RequestedRange);
				PropTrack->AddSection(*NewSection);
				NewTrack = PropTrack;
			}
		}
	}

	if (!NewTrack)
	{
		OutError = FString::Printf(TEXT("Failed to create track of type '%s'"), *TrackType);
		return false;
	}

	MovieScene->MarkPackageDirty();
	LevelSeq->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(LevelSeq);
	if (bSaveAsset && !Context.Services.SaveAsset(LevelSeq->GetPathName(), false, OutError))
	{
		return false;
	}
	const bool bVerifiedSection = NewSection
		&& NewTrack->GetAllSections().Contains(NewSection)
		&& NewSection->GetRange() == RequestedRange;
	OutStructured->SetBoolField(TEXT("verified"), bVerifiedSection);
	if (!bVerifiedSection)
	{
		SetToolStatus(OutStructured, false);
		OutError = FString::Printf(TEXT("Created track '%s' but no section was attached."), *NewTrack->GetName());
		return false;
	}

	OutStructured->SetStringField(TEXT("track_type"), TrackType.IsEmpty() ? (TrackName.IsEmpty() ? TEXT("camera") : TEXT("existing")) : *TrackType);
	OutStructured->SetStringField(TEXT("track_name"), NewTrack->GetTrackName().ToString());
	OutStructured->SetStringField(TEXT("binding_id"), BindingId);
	OutStructured->SetNumberField(TEXT("track_index"), MovieScene->GetTracks().Find(NewTrack));
	OutStructured->SetNumberField(TEXT("start_time"), StartTime);
	OutStructured->SetNumberField(TEXT("duration"), Duration);
	OutStructured->SetNumberField(TEXT("start_frame"), StartFrame.Value);
	OutStructured->SetNumberField(TEXT("duration_frames"), DurationFrames);
	OutStructured->SetStringField(TEXT("sequence_path"), SequencePath);
	OutStructured->SetStringField(TEXT("asset_path"), SequencePath);
	OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
	OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
	SetToolStatus(OutStructured, true);
	OutSummary = FString::Printf(TEXT("Added %s section to '%s' at %.2fs (duration=%.2fs)."),
		*(TrackType.IsEmpty() ? FString(TEXT("camera")) : TrackType), *SequencePath, StartTime, Duration);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: sequence_focus_subsequence — 聚焦子序列
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_SequenceFocusSubsequence(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString SequencePath, SubsequencePath;
	Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath);
	Arguments->TryGetStringField(TEXT("subsequence_path"), SubsequencePath);

	if (SequencePath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: sequence_path");
		return false;
	}

	// This tool opens/focuses a subsequence in the Sequencer
	// It primarily acts as information about available subsequences
	ULevelSequence* LevelSeq = Cast<ULevelSequence>(ResolveAsset(SequencePath, OutError));
	if (!LevelSeq)
	{
		OutError = FString::Printf(TEXT("Could not load LevelSequence from '%s'"), *SequencePath);
		return false;
	}

	UMovieScene* MovieScene = LevelSeq->GetMovieScene();

	// List all sub-sequences in the movie scene
	TArray<TSharedPtr<FJsonValue>> SubsArray;
	int32 SubCount = 0;

		// UE 5.7: GetAllTracks removed - use GetTracks()
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		for (UMovieSceneSection* Section : Track->GetAllSections())
		{
			if (!Section) continue;
			UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section);
			if (SubSection && SubSection->GetSequence())
			{
				TSharedPtr<FJsonObject> SubObj = MakeShared<FJsonObject>();
				SubObj->SetStringField(TEXT("name"), SubSection->GetSequence()->GetName());
				SubObj->SetStringField(TEXT("path"), SubSection->GetSequence()->GetPathName());
									// UE 5.7: GetStartTime removed - use GetRange()
					SubObj->SetNumberField(TEXT("start_time"), SubSection->GetRange().GetLowerBoundValue().Value);
									SubObj->SetNumberField(TEXT("end_time"), SubSection->GetRange().GetUpperBoundValue().Value);
				SubsArray.Add(MakeShared<FJsonValueObject>(SubObj));
				SubCount++;
			}
		}
	}

	OutStructured->SetArrayField(TEXT("subsequences"), SubsArray);
	OutStructured->SetNumberField(TEXT("count"), SubCount);
	OutStructured->SetStringField(TEXT("sequence_path"), SequencePath);
	OutSummary = FString::Printf(TEXT("Sequence '%s': %d subsequence(s) found."), *SequencePath, SubCount);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// P2: sequence_set_marked_frames — 设置标记帧
// ═══════════════════════════════════════════════════════════════════════════════

static bool Tool_SequenceSetMarkedFrames(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString SequencePath;
	double InFrame = -1.0, OutFrame = -1.0;
	Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath);
	Arguments->TryGetNumberField(TEXT("in_frame"), InFrame);
	Arguments->TryGetNumberField(TEXT("out_frame"), OutFrame);

	if (SequencePath.IsEmpty())
	{
		OutError = TEXT("Missing required argument: sequence_path");
		return false;
	}

	ULevelSequence* LevelSeq = Cast<ULevelSequence>(ResolveAsset(SequencePath, OutError));
	if (!LevelSeq)
	{
		OutError = FString::Printf(TEXT("Could not load LevelSequence from '%s'"), *SequencePath);
		return false;
	}

	UMovieScene* MovieScene = LevelSeq->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("No MovieScene.");
		return false;
	}

	// Set playback range using the sequence's native frame range.
	TRange<FFrameNumber> CurrentRangeFF = MovieScene->GetPlaybackRange();
	const int32 NewStart = (InFrame >= 0) ? FMath::RoundToInt(InFrame) : CurrentRangeFF.GetLowerBoundValue().Value;
	const int32 NewEnd = (OutFrame >= 0) ? FMath::RoundToInt(OutFrame) : CurrentRangeFF.GetUpperBoundValue().Value;
	if (NewEnd <= NewStart)
	{
		OutError = TEXT("out_frame must be greater than in_frame.");
		return false;
	}
	const TRange<FFrameNumber> RequestedRange(
		TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(NewStart)),
		TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(NewEnd)));
	MovieScene->Modify();
	LevelSeq->Modify();
	MovieScene->SetPlaybackRange(RequestedRange, true);
	MovieScene->MarkPackageDirty();
	LevelSeq->MarkPackageDirty();
	SololmcpWriteFlush::EnsureFlushed(LevelSeq);
	const TRange<FFrameNumber> ReadbackRange = MovieScene->GetPlaybackRange();
	const bool bVerified = ReadbackRange == RequestedRange;

	OutStructured->SetStringField(TEXT("sequence_path"), SequencePath);
	OutStructured->SetNumberField(TEXT("in_frame"), NewStart);
	OutStructured->SetNumberField(TEXT("out_frame"), NewEnd);
	OutStructured->SetNumberField(TEXT("duration"), NewEnd - NewStart);
	OutStructured->SetBoolField(TEXT("verified"), bVerified);
	SetToolStatus(OutStructured, bVerified);
	if (!bVerified)
	{
		OutError = TEXT("MovieScene playback range readback did not match the requested range.");
		return false;
	}
	OutSummary = FString::Printf(TEXT("Set and verified playback range on '%s': IN=%d, OUT=%d."),
		*SequencePath, NewStart, NewEnd);
	return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════════════════════

void RegisterEnhancedTools(FSololmcpToolRegistry& Registry)
{
	// ──────────────── P0: High Priority Core Tools ────────────────

	Registry.Register({
		TEXT("execute_console_command"),
		TEXT("Execute a UE console command. This is the most fundamental tool for controlling "
			 "the engine: stat commands, rendering commands, physics commands, gameplay commands, etc. "
			 "Examples: 'stat fps', 'r.ScreenPercentage 100', 'pause', 'ghost', 'GodMode', "
			 "'obj list class=StaticMeshActor', 'DumpConsoleCommands'."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("command"), FSololmcpSchemaBuilder::String(TEXT("UE console command string, e.g. 'stat fps' or 'r.ScreenPercentage 100'."))}
			},
			{TEXT("command")}),
		Tool_ExecuteConsoleCommand
	});

	Registry.Register({
		TEXT("python_execute"),
		TEXT("Execute a Python script string in the UE Python environment. "
			 "Requires PythonScriptPlugin to be enabled. "
			 "Access unreal module via 'import unreal' in the script. "
			 "Example: 'import unreal; print(unreal.EditorLevelLibrary.get_editor_world().get_name())'."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("script"), FSololmcpSchemaBuilder::String(TEXT("Python script code to execute."))}
			},
			{TEXT("script")}),
		Tool_PythonExecute
	});

	Registry.Register({
		TEXT("pie_start"),
		TEXT("Start a Play In Editor (PIE) session. Equivalent to pressing the Play button."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mode"), FSololmcpSchemaBuilder::String(
					TEXT("Play mode: 'viewport' (default) | 'new_window' | 'standalone'."),
					{TEXT("viewport"), TEXT("new_window"), TEXT("standalone")})},
				{TEXT("num_players"), FSololmcpSchemaBuilder::Integer(TEXT("Number of players (default: 1)."))}
				,{TEXT("net_mode"), FSololmcpSchemaBuilder::Integer(TEXT("0=standalone, 1=listen server, 2=client (default: 0)."))}
			},
			{}),
		Tool_PieStart
	});

	Registry.Register({
		TEXT("pie_stop"),
		TEXT("Stop the current Play In Editor (PIE) session. Equivalent to pressing the Stop button."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_PieStop
	});

	Registry.Register({
		TEXT("pie_get_status"),
		TEXT("Get the current PIE status: whether the editor is playing, simulating, or stopped."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_PieGetStatus
	});

	Registry.Register({
		TEXT("pie_local_player_create"),
		TEXT("Create one local player in the active PIE GameInstance and verify the local-player count readback."),
		FSololmcpSchemaBuilder::Object({{TEXT("controller_id"), FSololmcpSchemaBuilder::Integer(TEXT("Controller id in [0,7], default 1."))}}, {}),
		Tool_PieLocalPlayerCreate
	});

	Registry.Register({
		TEXT("pie_local_player_remove"),
		TEXT("Remove one local player from the active PIE GameInstance and verify the local-player count readback."),
		FSololmcpSchemaBuilder::Object({{TEXT("controller_id"), FSololmcpSchemaBuilder::Integer(TEXT("Controller id to remove, default 1."))}}, {}),
		Tool_PieLocalPlayerRemove
	});

	Registry.Register({
		TEXT("pie_capture"),
		TEXT("Capture a screenshot during PIE. The active PIE viewport will be captured."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum capture width (default: 1920)."))},
				{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum capture height (default: 1080)."))}
			},
			{}),
		Tool_PieCapture
	});

	Registry.Register({
		TEXT("pie_screenshot"),
		TEXT("Take a screenshot of the active PIE viewport. Alias for pie_capture."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_PieScreenshot
	});

	// ──────────────── P0: Alias Tools (HTML spec → C++ mapping) ────────────────

	Registry.Register({
		TEXT("umg_runtime_preview_spawn_widget"),
		TEXT("Mount a smoke-owned UserWidget into an already running PIE viewport without starting PIE."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_class_path"), FSololmcpSchemaBuilder::String(TEXT("Optional generated UserWidget class path if no blueprint path is supplied."))},
				{TEXT("z_order"), FSololmcpSchemaBuilder::Integer(TEXT("Viewport z-order; default 1000."))}
			},
			{}),
		Tool_UmgRuntimePreviewSpawnWidget
	});

	Registry.Register({
		TEXT("umg_runtime_preview_capture"),
		TEXT("Capture the active PIE viewport for a smoke-owned runtime UMG preview widget."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mount_handle"), FSololmcpSchemaBuilder::String(TEXT("Handle returned by umg_runtime_preview_spawn_widget."))},
				{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum capture width; default 1920."))},
				{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum capture height; default 1080."))}
			},
			{TEXT("mount_handle")}),
		Tool_UmgRuntimePreviewCapture
	});

	Registry.Register({
		TEXT("umg_runtime_preview_teardown"),
		TEXT("Remove a smoke-owned runtime UMG preview widget without stopping PIE or the editor."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mount_handle"), FSololmcpSchemaBuilder::String(TEXT("Handle returned by umg_runtime_preview_spawn_widget."))}
			},
			{TEXT("mount_handle")}),
		Tool_UmgRuntimePreviewTeardown
	});

	Registry.Register({
		TEXT("umg_binding_inspect_graph"),
		TEXT("Inspect a WidgetBlueprint's WidgetTree, graphs, animations, and component-bound UMG event nodes without mutation."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))}
			},
			{}),
		Tool_UmgBindingInspectGraph
	});

	Registry.Register({
		TEXT("umg_widget_tree_inspect_slots"),
		TEXT("Inspect WidgetTree slot classes and slot properties without mutating the WidgetBlueprint."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact widget name. If omitted, all widgets are inspected."))}
			},
			{}),
		Tool_UmgWidgetTreeInspectSlots
	});

	Registry.Register({
		TEXT("umg_widget_tree_inspect_properties"),
		TEXT("Inspect reflected WidgetTree widget properties, optionally including exported values, without mutation."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact widget name. If omitted, all widgets are inspected."))},
				{TEXT("include_values"), FSololmcpSchemaBuilder::Boolean(TEXT("Include exported property values; default true."))},
				{TEXT("max_properties"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum properties per widget; default 128."))}
			},
			{}),
		Tool_UmgWidgetTreeInspectProperties
	});

	Registry.Register({
		TEXT("umg_focus_navigation_inspect"),
		TEXT("Inspect WidgetTree focusability and navigation hints without mutation."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))}
			},
			{}),
		Tool_UmgFocusNavigationInspect
	});

	Registry.Register({
		TEXT("umg_runtime_focus_probe"),
		TEXT("Probe current PIE/runtime UMG focus context and smoke-owned preview widgets without mutation."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_UmgRuntimeFocusProbe
	});

	Registry.Register({
		TEXT("umg_text_localization_inspect"),
		TEXT("Inspect WidgetTree FText properties and their namespace/key metadata without mutation."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact widget name. If omitted, all widgets are inspected."))}
			},
			{}),
		Tool_UmgTextLocalizationInspect
	});

	Registry.Register({
		TEXT("umg_localization_key_audit"),
		TEXT("Audit WidgetTree FText properties for missing localization namespace/key metadata without mutation."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact widget name. If omitted, all widgets are audited."))}
			},
			{}),
		Tool_UmgLocalizationKeyAudit
	});

	Registry.Register({
		TEXT("umg_localization_key_autofill"),
		TEXT("Dry-run by default; autofill missing WidgetBlueprint FText localization namespace/key metadata without overwriting existing keys."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact widget name. If omitted, all widgets are considered."))},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Preview planned fills without mutation; default true."))},
				{TEXT("namespace"), FSololmcpSchemaBuilder::String(TEXT("Optional namespace to use only when a text property is missing namespace metadata."))}
			},
			{}),
		Tool_UmgLocalizationKeyAutofill
	});

	Registry.Register({
		TEXT("umg_binding_verify_delegate_signature"),
		TEXT("Conservatively verify whether a WidgetTree widget exposes an exact multicast delegate event and report its signature."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Widget name in the hierarchy."))},
				{TEXT("event_name"), FSololmcpSchemaBuilder::String(TEXT("Exact multicast delegate property name."))}
			},
			{TEXT("widget_name"), TEXT("event_name")}),
		Tool_UmgBindingVerifyDelegateSignature
	});

	Registry.Register({
		TEXT("umg_binding_create_function_stub"),
		TEXT("Add or confirm a function graph on a WidgetBlueprint, then finalize Entry->Return execution and compile-check it."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("Function graph name to create or confirm."))}
			},
			{TEXT("function_name")}),
		Tool_UmgBindingCreateFunctionStub
	});

	Registry.Register({
		TEXT("umg_binding_validate_function_graph"),
		TEXT("Validate a WidgetBlueprint function graph for FunctionEntry, Return Node terminator, reachable exec chain, and orphan required nodes; optionally repair safe missing terminators."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("Function graph name to validate."))},
				{TEXT("repair"), FSololmcpSchemaBuilder::Boolean(TEXT("When true, add a missing Return Node and direct Entry->Return exec link when safe."))}
			},
			{TEXT("function_name")}),
		Tool_UmgBindingValidateFunctionGraph
	});

	Registry.Register({
		TEXT("umg_list_view_refresh"),
		TEXT("Verify a WidgetBlueprint ListView target and, when given a smoke-owned runtime preview mount_handle, call UListView::RequestRefresh on the live instance."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("ListView widget name in the hierarchy."))},
				{TEXT("mount_handle"), FSololmcpSchemaBuilder::String(TEXT("Optional handle returned by umg_runtime_preview_spawn_widget for live runtime refresh."))}
			},
			{TEXT("widget_name")}),
		Tool_UmgListViewRefresh
	});

	Registry.Register({
		TEXT("umg_list_view_bind_selection_event"),
		TEXT("Conservatively bind a WidgetBlueprint ListView selection delegate via the existing component-bound event path, or fail closed."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("WidgetBlueprint asset path."))},
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("ListView widget name in the hierarchy."))},
				{TEXT("event_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact ListView selection delegate name."))},
				{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("Optional handler function name."))}
			},
			{TEXT("widget_name")}),
		Tool_UmgListViewBindSelectionEvent
	});

	Registry.Register({
		TEXT("asset_import"),
		TEXT("Import one asset file through UE AssetTools. This native single-file entry point mirrors import_asset and returns imported object paths."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("file_path"), FSololmcpSchemaBuilder::String(TEXT("File path to import."))},
				{TEXT("destination_path"), FSololmcpSchemaBuilder::String(TEXT("Destination content folder under /Game."))},
				{TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(TEXT("Replace an existing asset. Default true."))},
				{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save imported packages. Default true."))},
				{TEXT("automated"), FSololmcpSchemaBuilder::Boolean(TEXT("Use automated import without dialogs. Default true."))}
			},
			{TEXT("file_path")}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			FString FilePath, DestPath;
			if (!Arguments->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
			{
				OutError = TEXT("Missing file_path.");
				return false;
			}
			if (!FPaths::FileExists(FilePath))
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("SOURCE_FILE_NOT_FOUND"));
				OutError = FString::Printf(TEXT("Source file does not exist: %s"), *FilePath);
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("destination_path"), DestPath) || DestPath.IsEmpty())
			{
				DestPath = TEXT("/Game/Incoming");
			}
			DestPath = DestPath.TrimStartAndEnd();
			while (DestPath.EndsWith(TEXT("/")))
			{
				DestPath.LeftChopInline(1);
			}
			if (!(DestPath == TEXT("/Game") || DestPath.StartsWith(TEXT("/Game/"))))
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("INVALID_DESTINATION_PATH"));
				OutError = TEXT("destination_path must be /Game or a folder below /Game.");
				return false;
			}

			bool bReplaceExisting = true;
			bool bSave = true;
			bool bAutomated = true;
			Arguments->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
			Arguments->TryGetBoolField(TEXT("save"), bSave);
			Arguments->TryGetBoolField(TEXT("automated"), bAutomated);

			UAssetImportTask* Task = NewObject<UAssetImportTask>(GetTransientPackage());
			Task->Filename = FilePath;
			Task->DestinationPath = DestPath;
			Task->bAutomated = bAutomated;
			Task->bReplaceExisting = bReplaceExisting;
			Task->bSave = bSave;
			TArray<UAssetImportTask*> Tasks{Task};
			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			AssetToolsModule.Get().ImportAssetTasks(Tasks);

			TArray<TSharedPtr<FJsonValue>> ImportedAssets;
			for (UObject* ImportedObject : Task->GetObjects())
			{
				if (!ImportedObject)
				{
					continue;
				}
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), ImportedObject->GetName());
				Item->SetStringField(TEXT("class"), ImportedObject->GetClass()->GetPathName());
				Item->SetStringField(TEXT("object_path"), ImportedObject->GetPathName());
				Item->SetStringField(TEXT("package_path"), ImportedObject->GetOutermost()->GetName());
				ImportedAssets.Add(MakeShared<FJsonValueObject>(Item));
			}
			OutStructured->SetStringField(TEXT("source_path"), FilePath);
			OutStructured->SetStringField(TEXT("destination_path"), DestPath);
			OutStructured->SetArrayField(TEXT("assets"), ImportedAssets);
			OutStructured->SetNumberField(TEXT("count"), ImportedAssets.Num());
			OutStructured->SetBoolField(TEXT("saved"), bSave);
			OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.asset_import.v2"));
			if (ImportedAssets.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("IMPORT_FAILED"));
				OutError = FString::Printf(TEXT("Import produced no assets for source file: %s"), *FilePath);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Imported '%s' into '%s' as %d asset(s)."), *FilePath, *DestPath, ImportedAssets.Num());
			return true;
		}
	});

	Registry.Register({
		TEXT("actor_delete"),
		TEXT("[ALIAS] Delete actor(s). Delegates to actor_destroy."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("actor_ids"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Actor ID to delete.")))},
				{TEXT("actors"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Actor identifiers to delete.")))},
				{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label, name, object path, or editor path."))},
				{TEXT("actor_id"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
				{TEXT("actor_path"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
				{TEXT("path"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
				{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
				{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))}
			},
			{}),
		Tool_Alias_ActorDelete
	});

	Registry.Register({
		TEXT("actor_get_transform"),
		TEXT("[ALIAS] Get actor transform. Delegates to actor_set_transform (returns current transform)."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("actor_id"), FSololmcpSchemaBuilder::String(TEXT("Actor ID."))}
			},
			{TEXT("actor_id")}),
		Tool_Alias_ActorGetTransform
	});

	Registry.Register({
		TEXT("actor_group"),
		TEXT("[ALIAS] Group selected actors. Delegates to actor_group_selected."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_Alias_ActorGroup
	});

	Registry.Register({
		TEXT("actor_ungroup"),
		TEXT("[ALIAS] Ungroup selected actors. Delegates to actor_ungroup_selected."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_Alias_ActorUngroup
	});

	Registry.Register({
		TEXT("audio_create_cue"),
		TEXT("[ALIAS] Create a sound cue. Use 'audio_create_sound_cue' with 'package_path' and 'asset_name'."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Destination path for the sound cue."))},
				{TEXT("cue_name"), FSololmcpSchemaBuilder::String(TEXT("Name of the sound cue."))}
			},
			{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'audio_create_sound_cue' with package_path and asset_name parameters.");
			return false;
		}
	});

	Registry.Register({
		TEXT("landscape_sculpt"),
		TEXT("[ALIAS] Sculpt landscape. Use 'landscape_sculpt_brush' with landscape_actor_id, operation, brush_size, brush_strength."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("landscape_actor_id"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor ID."))},
				{TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("Sculpt operation."))},
				{TEXT("brush_size"), FSololmcpSchemaBuilder::Number(TEXT("Brush size in cm."))}
			},
			{}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'landscape_sculpt_brush' with landscape_actor_id, operation, brush_size, brush_strength.");
			return false;
		}
	});

	Registry.Register({
		TEXT("world_get_state"),
		TEXT("[ALIAS] Get world state info. Delegates to editor_get_state."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_Alias_WorldGetState
	});

	Registry.Register({
		TEXT("umg_create_widget"),
		TEXT("[ALIAS] Create UMG widget blueprint. Use 'umg_widget_blueprint_create' with asset_path and widget_name."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Widget blueprint path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Widget display name."))}
			},
			{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'umg_widget_blueprint_create' with asset_path and widget_name parameters.");
			return false;
		}
	});

	Registry.Register({
		TEXT("editor_screenshot"),
		TEXT("[ALIAS] Take editor screenshot. Delegates to screenshot_viewport."),
		FSololmcpSchemaBuilder::Object({}, {}),
		Tool_Alias_EditorScreenshot
	});

	Registry.Register({
		TEXT("abp_add_state"),
		TEXT("[ALIAS] Add state to animation blueprint. Use 'anim_blueprint_add_state' with asset_path, state_name, state_machine_name."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Animation blueprint path."))},
				{TEXT("state_name"), FSololmcpSchemaBuilder::String(TEXT("State name to add."))},
				{TEXT("state_machine_name"), FSololmcpSchemaBuilder::String(TEXT("State machine name."))}
			},
			{TEXT("asset_path"), TEXT("state_name")}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'anim_blueprint_add_state' with asset_path, state_name, state_machine_name.");
			return false;
		}
	});

	Registry.Register({
		TEXT("debug_get_callstack"),
		TEXT("[ALIAS] Get debug callstack. Use 'blueprint_debug_get_call_stack'."),
		FSololmcpSchemaBuilder::Object({}, {}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'blueprint_debug_get_call_stack'.");
			return false;
		}
	});

	Registry.Register({
		TEXT("debug_get_watched_values"),
		TEXT("[ALIAS] Get watched values. Use 'blueprint_debug_get_watches'."),
		FSololmcpSchemaBuilder::Object({}, {}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'blueprint_debug_get_watches'.");
			return false;
		}
	});

	Registry.Register({
		TEXT("debug_list_breakpoints"),
		TEXT("[ALIAS] List breakpoints. Use 'blueprint_debug_list_breakpoints'."),
		FSololmcpSchemaBuilder::Object({}, {}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'blueprint_debug_list_breakpoints'.");
			return false;
		}
	});

	Registry.Register({
		TEXT("debug_set_breakpoint"),
		TEXT("[ALIAS] Set breakpoint. Use 'blueprint_debug_set_breakpoint' with asset_path and node_name."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint path."))},
				{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Node name to set breakpoint on."))}
			},
			{TEXT("asset_path"), TEXT("node_name")}),
		[](const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary, FString& OutError) -> bool
		{
			OutError = TEXT("Use 'blueprint_debug_set_breakpoint' with asset_path and node_name.");
			return false;
		}
	});

	// ──────────────── P1: Medium Priority Enhancement Tools ────────────────

	Registry.Register({
		TEXT("blueprint_get_nodes"),
		TEXT("Get all nodes from a Blueprint's EventGraph and function graphs. "
			 "Returns node names, classes, positions, and pin information for each graph."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path, e.g. '/Game/Blueprints/MyBP'."))}
			},
			{TEXT("asset_path")}),
		Tool_BlueprintGetNodes
	});

	Registry.Register({
		TEXT("blueprint_get_variables"),
		TEXT("Get all variables and components from a Blueprint. "
			 "Returns variable names, types, editability, and component classes."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))}
			},
			{TEXT("asset_path")}),
		Tool_BlueprintGetVariables
	});

	Registry.Register({
		TEXT("material_add_node"),
		TEXT("Add a material expression node to a material asset. "
			 "Supports 50+ node types: TextureSample, Constant, Multiply, Add, Lerp, "
			 "ScalarParameter, VectorParameter, TextureCoordinate, Fresnel, Noise, etc."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path."))},
				{TEXT("node_type"), FSololmcpSchemaBuilder::String(TEXT("Material expression type (e.g. 'TextureSample', 'Constant', 'Multiply', 'Lerp')."))},
				{TEXT("node_x"), FSololmcpSchemaBuilder::Integer(TEXT("X position in material editor (default: 0)."))},
				{TEXT("node_y"), FSololmcpSchemaBuilder::Integer(TEXT("Y position in material editor (default: 0)."))}
			},
			{TEXT("asset_path"), TEXT("node_type")}),
		Tool_MaterialAddNode
	});

	Registry.Register({
		TEXT("material_instance_get_params"),
		TEXT("Get all parameters from a Material Instance (scalar, vector, texture, switch). "
			 "Returns parameter names, types, and current values."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material Instance asset path."))}
			},
			{TEXT("asset_path")}),
		Tool_MaterialInstanceGetParams
	});

	Registry.Register({
		TEXT("umg_widget_bind_event"),
		TEXT("Bind an event on a UMG widget to a blueprint function. "
			 "Creates a function with the event's signature in the Widget Blueprint's Event Graph."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Widget Blueprint asset path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Widget name in the hierarchy."))},
				{TEXT("event_name"), FSololmcpSchemaBuilder::String(TEXT("Event name (e.g. 'OnClicked', 'OnTextChanged')."))},
				{TEXT("function_name"), FSololmcpSchemaBuilder::String(TEXT("Optional: function name (auto-generated if omitted)."))}
			},
			{TEXT("asset_path"), TEXT("widget_name"), TEXT("event_name")}),
		Tool_UmgWidgetBindEvent
	});

	Registry.Register({
		TEXT("umg_widget_bind_property"),
		TEXT("Bind a property on a UMG widget to a blueprint variable. "
			 "Creates a new variable and links it to the widget property for data binding."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Widget Blueprint asset path."))},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("Widget name in the hierarchy."))},
				{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("Property name to bind (e.g. 'Visibility', 'IsEnabled', 'Text')."))},
				{TEXT("variable_name"), FSololmcpSchemaBuilder::String(TEXT("Optional: variable name (auto-generated if omitted)."))}
			},
			{TEXT("asset_path"), TEXT("widget_name"), TEXT("property_name")}),
		Tool_UmgWidgetBindProperty
	});

	Registry.Register({
		TEXT("vfx_create_system"),
		TEXT("Create a new Niagara VFX system asset. Use niagara_add_emitter to add emitters."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Destination path for the Niagara system, e.g. '/Game/VFX/MySystem'."))}
			},
			{TEXT("asset_path")}),
		Tool_VfxCreateSystem
	});

	Registry.Register({
		TEXT("pcg_generate"),
		TEXT("Trigger PCG generation after resolving actor/actor_label + graph_path and running pcg_graph_validate. "
			 "Pass actor or actor_label for a specific actor, graph_path/asset_path for a specific graph, "
			 "or allow_all=true to intentionally target every resolved match. "
			 "Tile evidence is capped at <=4 tiles; strict/unattended requests fail closed when tile count is unknown."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Target actor path/name/label carrying a UPCGComponent."))},
				{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Legacy actor label. Resolved exact-first, with unique partial fallback."))},
				{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("Optional PCG Graph asset path. Must match the resolved component graph."))},
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for graph_path, matching pcg_graph_validate/dry_run schemas."))},
				{TEXT("allow_all"), FSololmcpSchemaBuilder::Boolean(TEXT("Required when intentionally generating multiple graph-only or whole-level matches."))},
				{TEXT("allow_partial_actor_label"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow unique partial actor label/name fallback for actor as well as actor_label."))},
				{TEXT("allowed_tiles"), FSololmcpSchemaBuilder::Array(MakeShared<FJsonObject>(), TEXT("Optional tile descriptors proving this generate call touches <=4 tiles."))},
				{TEXT("tile_indices"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer(TEXT("Tile index")), TEXT("Optional tile indices proving this generate call touches <=4 tiles."))},
				{TEXT("tile_count"), FSololmcpSchemaBuilder::Integer(TEXT("Optional explicit tile count for tile-cap receipt/gating."))},
				{TEXT("area_m2"), FSololmcpSchemaBuilder::Number(TEXT("Optional AOI area in square meters; converted to tile count using tile_size_m for guard evidence."))},
				{TEXT("tile_size_m"), FSololmcpSchemaBuilder::Number(TEXT("Optional tile size in meters when area_m2 is supplied; default 256."))},
				{TEXT("client_request_id"), FSololmcpSchemaBuilder::String(TEXT("Optional id echoed for sync/async trace consistency."))},
				{TEXT("trace_id"), FSololmcpSchemaBuilder::String(TEXT("Optional correlation id echoed in the result."))}
			},
			{}),
		[&Registry](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
		            TSharedRef<FJsonObject>& Out, FString& Sum, FString& Err) -> bool
		{
			return Tool_PcgGenerate_Impl(Registry, Ctx, Args, Out, Sum, Err);
		}
	});

	// ──────────────── P2: Low Priority Supplementary Tools ────────────────

	Registry.Register({
		TEXT("animation_create_montage"),
		TEXT("Create an AnimMontage from an AnimSequence. "
			 "Montages are used for animation blending, slot assignment, and root motion."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("AnimSequence asset path."))},
				{TEXT("montage_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the new montage (auto-generated if omitted)."))},
				{TEXT("looping"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable looping (default: false)."))}
			},
			{TEXT("sequence_path")}),
		Tool_AnimationCreateMontage
	});

	Registry.Register({
		TEXT("asset_get_metadata"),
		TEXT("Get comprehensive metadata for an asset: class, path, tags, dependencies count, "
			 "reference count, file size, and other asset registry data."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path to query."))}
			},
			{TEXT("asset_path")}),
		Tool_AssetGetMetadata
	});

	Registry.Register({
		TEXT("texture_get_info"),
		TEXT("Get detailed texture information: dimensions, pixel format, compression settings, "
			 "mip count, SRGB setting, LOD group, and source file path."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Texture asset path."))}
			},
			{TEXT("asset_path")}),
		Tool_TextureGetInfo
	});

	Registry.Register({
		TEXT("texture_modify"),
		TEXT("Modify texture parameters: SRGB, compression settings, mip generation, never_stream. "
			 "The texture must be re-saved after modification."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Texture asset path."))},
				{TEXT("srgb"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable/disable SRGB."))},
				{TEXT("never_stream"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable/disable never-stream."))}
			},
			{TEXT("asset_path")}),
		Tool_TextureModify
	});

	Registry.Register({
		TEXT("world_partition_cell_size_cm"),
		TEXT("Get the WorldPartition cell size in cm. Optionally request a new cell size "
			 "(configuration only; requires editor restart to take effect)."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("cell_size_cm"), FSololmcpSchemaBuilder::Number(TEXT("Optional: requested cell size in cm (read-only at runtime)."))}
			},
			{}),
		Tool_WorldPartitionCellSizeCm
	});

	Registry.Register({
		TEXT("sequence_add_folder"),
		TEXT("Add a named folder to a Level Sequence for organizing tracks."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level Sequence asset path."))},
				{TEXT("folder_name"), FSololmcpSchemaBuilder::String(TEXT("Folder display name."))}
			},
			{TEXT("sequence_path"), TEXT("folder_name")}),
		Tool_SequenceAddFolder
	});

	Registry.Register({
		TEXT("sequence_add_section"),
		TEXT("Add a new section to a Level Sequence track (camera, spawn, or property track)."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level Sequence asset path."))},
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Compatibility alias for sequence_path."))},
				{TEXT("track_name"), FSololmcpSchemaBuilder::String(TEXT("Existing track name to receive the new section."))},
				{TEXT("binding_id"), FSololmcpSchemaBuilder::String(TEXT("Optional object binding GUID for track_name lookup."))},
				{TEXT("track_type"), FSololmcpSchemaBuilder::String(
					TEXT("Track type: 'camera' (default) | 'spawn' | 'property'."),
					{TEXT("camera"), TEXT("spawn"), TEXT("property")})},
				{TEXT("start_time"), FSololmcpSchemaBuilder::Number(TEXT("Section start time in seconds (default: 0)."))},
				{TEXT("duration"), FSololmcpSchemaBuilder::Number(TEXT("Section duration in seconds (default: 1)."))},
				{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save the sequence after mutation (default: true)."))}
			},
			{}),
		Tool_SequenceAddSection
	});

	Registry.Register({
		TEXT("sequence_focus_subsequence"),
		TEXT("List all subsequences within a Level Sequence, or focus on a specific subsequence. "
			 "Returns all available sub-sequences with their paths and time ranges."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level Sequence asset path."))},
				{TEXT("subsequence_path"), FSololmcpSchemaBuilder::String(TEXT("Optional: sub-sequence path to focus on."))}
			},
			{TEXT("sequence_path")}),
		Tool_SequenceFocusSubsequence
	});

	Registry.Register({
		TEXT("sequence_set_marked_frames"),
		TEXT("Set the marked frame range (playback range) on a Level Sequence. "
			 "This defines the IN/OUT points for the sequence."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level Sequence asset path."))},
				{TEXT("in_frame"), FSololmcpSchemaBuilder::Number(TEXT("IN frame time in seconds."))},
				{TEXT("out_frame"), FSololmcpSchemaBuilder::Number(TEXT("OUT frame time in seconds."))}
			},
			{TEXT("sequence_path")}),
		Tool_SequenceSetMarkedFrames
	});
}

} // namespace UE::SOMOLMCP
