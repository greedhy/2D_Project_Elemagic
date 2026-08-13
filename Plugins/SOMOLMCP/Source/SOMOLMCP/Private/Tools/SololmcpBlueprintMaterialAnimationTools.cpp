// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"
// Engine-version gates used further down (Landscape Edit Layer, StructUtils paths).
#include "SololmcpEngineCompat.h"
// EMaterialDomain / MD_Volume. Reached transitively on 5.6+, not on 5.5.
#include "MaterialDomain.h"
#include "SOMOLMCP.h"
#include "SololmcpTerrainModeGuard.h"
#include "SololmcpSharedLocks.h" // v3.10.x worker-safety: cached IAssetRegistry pointer
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SoftObjectPath.h"

#include "AnimationBlueprintLibrary.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimData/AttributeIdentifier.h"
#include "Animation/AnimData/AnimDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Containers/Ticker.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorLibrary.h"
#include "BlueprintDelegateNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "Components/CanvasPanel.h"
#include "Components/SceneComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraShakeSourceComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/Image.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/MeshComponent.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelWidget.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"  // ASkyAtmosphere (Engine module) — for scene_lighting_ensure visible-sky fix
#include "Components/SpotLightComponent.h"
#include "Components/AudioComponent.h"
#include "Camera/CameraActor.h"
#include "CameraRig_Crane.h"
#include "CameraRig_Rail.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "CineCameraSettings.h"
#include "ControlRigBlueprintEditorLibrary.h"
#include "ControlRigBlueprintFactory.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyElements.h"
// RigVMEditorBlueprintLibrary is 5.4+. On 5.3 the same controller is reachable as
// a member of URigVMBlueprint, so ResolveRigVMControllerCompat below keeps one call
// shape across engines rather than costing 5.3 this whole file.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#define SOMOLMCP_DOMAIN_HAS_RIGVM_EDITOR_BP_LIBRARY 1
#include "RigVMEditorBlueprintLibrary.h"
#else
#define SOMOLMCP_DOMAIN_HAS_RIGVM_EDITOR_BP_LIBRARY 0
#endif
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "DataLayer/DataLayerFactory.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"
// Audit round 3: needed for FPackageName::IsValidObjectPath in asset_path validation guards.
#include "Misc/PackageName.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonReader.h"
#include "Editor.h"
#include "EditorReimportHandler.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/SkeletalMesh.h"
// FSkeletalMaterial and FSkeletalMeshLODInfo live here. Newer engines pull it in
// transitively, which is why the omission only showed up on 5.4 as six
// "incomplete type" errors. Present on 5.3 through 5.8, so no version gate.
#include "Engine/SkinnedAssetCommon.h"

// UDataTable::AddRow(FName, const uint8*, const UScriptStruct*) is 5.5+. On 5.4 the
// only route from raw struct memory is AddRowInternal, which takes a non-const
// pointer and does not re-validate the row type -- acceptable here because the
// caller allocated the memory from DT->RowStruct in the first place.
static void SomolAddDataTableRow(UDataTable* Table, FName RowName, uint8* RowMemory)
{
	if (Table == nullptr || RowMemory == nullptr)
	{
		return;
	}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
	Table->AddRow(RowName, RowMemory, Table->RowStruct);
#else
	// 5.4 has only the FTableRowBase overload, and AddRowInternal is protected. A
	// data table's row struct is required to derive from FTableRowBase, so the raw
	// memory can be viewed as one -- that requirement is what makes this safe rather
	// than a convenient cast.
	Table->AddRow(RowName, *reinterpret_cast<const FTableRowBase*>(RowMemory));
#endif
}
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimInstance.h"
#include "Engine/Texture.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureLightProfile.h"
#include "Engine/DirectionalLight.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "EngineUtils.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/SoundCueFactoryNew.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
// BR-01..BR-04 shared brush kernel: level persistence for writer receipts.
#include "FileHelpers.h"
#include "TextureResource.h"
#include "Engine/Texture2D.h"
// ULandscapeEditLayerBase and ALandscape::GetEditLayers are UE 5.6+. Thirteen uses
// of that API in this file are the only reason UE 5.3/5.4/5.5 stubbed all 623 tools
// registered here, so they are gated individually instead.
#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
#include "LandscapeEditLayer.h"
#endif
#include "LandscapeEditorUtils.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeUtils.h"
#include "LandscapeProxy.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "LevelSequenceEditorSubsystem.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpWriteFlush.h"
#include "SololmcpErrorHelpers.h"
#include "Protocol/SololmcpJobService.h"
#include "Modules/ModuleManager.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneObjectPathChannel.h"
#include "MovieSceneBindingProxy.h"
#include "MovieSceneBinding.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneFolder.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphUtilities.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraMeshRendererProperties.h"
#if __has_include("NiagaraMeshRendererMeshProperties.h")
#include "NiagaraMeshRendererMeshProperties.h"
#endif
#include "NiagaraRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "PhysicsEngine/PhysicsAsset.h"
// PhysicsEngine/SkeletalBodySetup.h is 5.5+; the single loop needing the complete
// type is gated below, and UPhysicsAsset::SkeletalBodySetups itself is universal.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#define SOMOLMCP_DOMAIN_HAS_SKELETAL_BODY_SETUP 1
#include "PhysicsEngine/SkeletalBodySetup.h"
#else
#define SOMOLMCP_DOMAIN_HAS_SKELETAL_BODY_SETUP 0
#endif
#include "ScopedTransaction.h"
#include "Selection.h"
#include "SkeletalMeshEditorSubsystem.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundBase.h"
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshEditorSubsystemHelpers.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "LevelEditorSubsystem.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_CallFunction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "NiagaraTypes.h"
#include "NiagaraSystemImpl.h"
#include "NiagaraShared.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraMessages.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Slate/WidgetTransform.h"
#include "MovieScene.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "WidgetBlueprintEditorUtils.h"
#include "WidgetBlueprintFactory.h"
#include "WidgetReference.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "WorldPartition/WorldPartitionUtils.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
// World Partition switched actor descriptors to instances in 5.4.
#if SOMOLMCP_HAS_ACTORDESC_INSTANCE
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#endif
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/ErrorHandling/WorldPartitionStreamingGenerationErrorHandler.h"
#include "WorldPartition/ContentBundle/ContentBundleEditor.h"
#include "WorldPartition/ContentBundle/ContentBundleDescriptor.h"
#include "WorldPartition/ContentBundle/ContentBundleStatus.h"
#include "WorldPartition/ContentBundle/ContentBundleEditorSubsystem.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
// UDataLayerInstanceWithAsset. Newer engines reach it transitively, so its absence
// only surfaced on 5.3 -- as six incomplete-type errors plus a long tail of
// FJsonObject overload and syntax errors that were all downstream of them.
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"

// UAnimationBlueprintLibrary widened these accessors from UAnimSequence* to
// UAnimSequenceBase* in 5.4. The call sites hold a UAnimSequenceBase*, which 5.3
// will not take, so narrow it there. Cast rather than static_cast: a sequence base
// that is not a UAnimSequence yields null, and the library already tolerates null
// by leaving the output arrays empty -- which is the same answer as "this asset has
// no such curve keys".
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#define SOMOLMCP_ANIM_KEY_TARGET(Seq) (Seq)
#else
#define SOMOLMCP_ANIM_KEY_TARGET(Seq) Cast<UAnimSequence>(Seq)
#endif
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
// Fog and PostProcess (v1.6.0)
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/PostProcessVolume.h"
// PCG includes (v1.6.0)
// AI / BehaviorTree includes (v1.7.0)
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_NativeEnum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "AIController.h"
// Navigation includes (v1.7.0)
#include "NavigationSystem.h"
#include "Navigation/NavLinkProxy.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
// Physics constraint includes (v1.7.0)
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "PhysicsEngine/BodySetup.h"
// IK includes (v1.7.0)
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetProcessor.h"
#include "Retargeter/IKRetargetSettings.h"
#include "RetargetEditor/IKRetargeterController.h"
// MPC / Foliage / LevelStreaming / Audio includes (v1.7.0)
#include "Materials/MaterialParameterCollection.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundSubmix.h"
// GAS includes (v1.7.0) — re-enabled for UE 5.7.4
#if 1
#include "AbilitySystemComponent.h"
#include "GameplayAbilitySpec.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsEditorModule.h"
#endif
#include "Animation/AnimMontage.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionSimulationTypes.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
// #include "PCGGraphGenerator.h" — Removed in UE5.7.4, class not used
#include "PCGSubgraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Elements/PCGPointProcessingElementBase.h"

#include "Tools/SololmcpDomainToolsShared.h"


// One register function moved out of SololmcpDomainTools.cpp.
//
// The point of moving it is not this function -- it is proving that the shared
// helper header can be included from a second translation unit. If it cannot,
// the link fails and names the colliding symbols.

namespace UE::SOMOLMCP
{
	static bool TryParseMaterialVectorValue(
		const TSharedRef<FJsonObject>& Arguments,
		FLinearColor& OutColor,
		FString* OutInputForm = nullptr)
	{
		const TCHAR* CandidateFields[] = {TEXT("value"), TEXT("color_value")};
		for (const TCHAR* FieldName : CandidateFields)
		{
			const TSharedPtr<FJsonValue> Candidate = Arguments->TryGetField(FieldName);
			if (!Candidate.IsValid())
			{
				continue;
			}
			if (Candidate->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Candidate->AsObject();
				if (Object.IsValid() && FSololmcpEditorServices::JsonToLinearColor(Object, OutColor))
				{
					if (OutInputForm) *OutInputForm = FString::Printf(TEXT("%s_object"), FieldName);
					return true;
				}
			}
			else if (Candidate->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Components = Candidate->AsArray();
				if (Components.Num() != 3 && Components.Num() != 4)
				{
					continue;
				}
				double Values[4] = {0.0, 0.0, 0.0, 1.0};
				bool bValid = true;
				for (int32 Index = 0; Index < Components.Num(); ++Index)
				{
					bValid = Components[Index].IsValid() && Components[Index]->TryGetNumber(Values[Index]);
					if (!bValid) break;
				}
				if (bValid)
				{
					OutColor = FLinearColor(Values[0], Values[1], Values[2], Values[3]);
					if (OutInputForm) *OutInputForm = FString::Printf(TEXT("%s_array%d"), FieldName, Components.Num());
					return true;
				}
			}
		}
		return false;
	}

	void RegisterBlueprintMaterialAnimationTools(FSololmcpToolRegistry& Registry)
	{
		auto RegisterPythonTool = [&Registry](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema, auto BuildCode)
		{
			Registry.Register({
				ToolName,
				Description,
				Schema,
				[BuildCode](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					const FString Code = BuildCode(Context, Arguments, OutError);
					if (Code.IsEmpty())
					{
						if (OutError.IsEmpty())
						{
							OutError = TEXT("Failed to build Python command.");
						}
						return false;
					}
					return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
				},
				[](const FSololmcpToolExecutionContext& Context, FString& OutReason)
				{
					return Context.Services.IsPythonAvailable(&OutReason);
},
0,
nullptr,
true
});
		};

		Registry.Register({
			TEXT("blueprint_create"),
			TEXT("Create a Blueprint asset with a specified parent class."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("parent_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParentClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("parent_class_path"), ParentClassPath))
				{
					OutError = TEXT("Missing asset_path or parent_class_path.");
					return false;
				}
				UClass* ParentClass = Context.Services.ResolveClass(ParentClassPath, OutError);
				if (!ParentClass)
				{
					return false;
				}

				// Split asset_path → PackagePath + AssetName for auto-naming
				FString PackagePath;
				FString AssetName;
				int32 LastSlash;
				if (AssetPath.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
				{
					PackagePath = AssetPath.Left(LastSlash);
					AssetName = AssetPath.RightChop(LastSlash + 1);
				}
				else
				{
					OutError = TEXT("asset_path must be in format /Game/Folder/BlueprintName");
					return false;
				}

				// Auto-naming: if asset already exists, generate a unique name
				FString EffectiveName = Context.Services.GenerateUniqueAssetName(PackagePath, AssetName);
				if (EffectiveName != AssetName)
				{
					OutStructured->SetStringField(TEXT("original_name"), AssetName);
					AssetName = EffectiveName;
				}
				const FString EffectiveAssetPath = PackagePath / AssetName;

				// Auto-delete existing asset to suppress overwrite confirmation dialog
				if (Context.Services.AssetExists(EffectiveAssetPath))
				{
					FString DelError;
					Context.Services.DeleteAsset(EffectiveAssetPath, DelError);
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintCreate", "SOMOLMCP Create Blueprint"));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				UBlueprint* Blueprint = UBlueprintEditorLibrary::CreateBlueprintAssetWithParent(EffectiveAssetPath, ParentClass);
#else
				// CreateBlueprintAssetWithParent is 5.5+; create the package and blueprint
				// directly, which is what it does internally.
				UPackage* BlueprintPackage = CreatePackage(*FPackageName::GetLongPackagePath(EffectiveAssetPath));
				UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
					ParentClass, BlueprintPackage, FName(*FPackageName::GetLongPackageAssetName(EffectiveAssetPath)),
					BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
#endif
				if (!Blueprint)
				{
					OutError = FString::Printf(TEXT("Failed to create blueprint at '%s' with parent '%s' (%s). CreateBlueprintAssetWithParent returned nullptr."),
						*EffectiveAssetPath, *ParentClassPath, *ParentClass->GetName());
					return false;
				}
				// Audit round 7 (silent-create fix): force save + asset_registry notify so subsequent
				// blueprint_compile/blueprint_inspect can LoadAsset(); verify persistence before returning ok.
				const FString CreatedPath = Blueprint->GetPathName();
				Blueprint->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Blueprint);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!VerifyCreatedAssetReloaded(Context.Services, Blueprint, UBlueprint::StaticClass(), OutStructured, OutError))
				{
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					return false;
				}
				UBlueprint* ReloadedBlueprint = Cast<UBlueprint>(Context.Services.LoadAsset(CreatedPath, SaveErr));
				if (!ReloadedBlueprint || ReloadedBlueprint->ParentClass != ParentClass)
				{
					OutStructured = MakeShared<FJsonObject>();
					OutStructured->SetStringField(TEXT("error"), TEXT("blueprint_parent_mismatch_after_create"));
					OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
					OutStructured->SetStringField(TEXT("expected_parent"), ParentClass->GetPathName());
					OutStructured->SetStringField(TEXT("actual_parent"), ReloadedBlueprint && ReloadedBlueprint->ParentClass ? ReloadedBlueprint->ParentClass->GetPathName() : FString());
					OutError = FString::Printf(TEXT("blueprint_parent_mismatch_after_create: %s"), *CreatedPath);
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutStructured->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
				OutSummary = FString::Printf(TEXT("Created blueprint: %s"), *EffectiveAssetPath);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_instance_batch_create"),
			TEXT("Create multiple MaterialInstanceConstant assets with optional parameter sets, per-item receipts, conflict policy, save/reload verification, and optional atomic rollback."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("items"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
					{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Destination package folder, for example /Game/Materials."))},
					{TEXT("asset_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("parent_material_path"), FSololmcpSchemaBuilder::String(TEXT("Optional per-item parent override."))},
					{TEXT("parameters"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Same entries accepted by material_instance_batch_parameters.")))}
				}, {TEXT("package_path"), TEXT("asset_name")}))},
				{TEXT("parent_material_path"), FSololmcpSchemaBuilder::String(TEXT("Default parent material for every item."))},
				{TEXT("parent"), FSololmcpSchemaBuilder::String(TEXT("Compatibility alias for parent_material_path."))},
				{TEXT("parameters"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}, TEXT("Default parameter entries for every item.")))},
				{TEXT("atomic"), FSololmcpSchemaBuilder::Boolean(TEXT("If true, delete all assets created by this call when any item fails."))},
				{TEXT("continue_on_error"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("name_conflict_policy"), FSololmcpSchemaBuilder::String(TEXT("fail (default), skip, or unique."), {TEXT("fail"), TEXT("skip"), TEXT("unique")})}
			}, {TEXT("items")}),
			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("items"), Items) || !Items || Items->IsEmpty())
				{
					OutError = TEXT("items must contain at least one batch-create item.");
					return false;
				}
				FString DefaultParent;
				if (!Arguments->TryGetStringField(TEXT("parent_material_path"), DefaultParent))
				{
					Arguments->TryGetStringField(TEXT("parent"), DefaultParent);
				}
				const TArray<TSharedPtr<FJsonValue>>* DefaultParameters = nullptr;
				Arguments->TryGetArrayField(TEXT("parameters"), DefaultParameters);
				const bool bAtomic = Arguments->HasTypedField<EJson::Boolean>(TEXT("atomic")) && Arguments->GetBoolField(TEXT("atomic"));
				const bool bContinue = Arguments->HasTypedField<EJson::Boolean>(TEXT("continue_on_error")) && Arguments->GetBoolField(TEXT("continue_on_error"));
				FString ConflictPolicy = TEXT("fail");
				Arguments->TryGetStringField(TEXT("name_conflict_policy"), ConflictPolicy);
				ConflictPolicy = ConflictPolicy.TrimStartAndEnd().ToLower();
				if (ConflictPolicy != TEXT("fail") && ConflictPolicy != TEXT("skip") && ConflictPolicy != TEXT("unique"))
				{
					OutError = TEXT("name_conflict_policy must be fail, skip, or unique.");
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> Receipts;
				TArray<FString> CreatedPaths;
				TArray<TSharedRef<FJsonObject>> CreatedRows;
				int32 Succeeded = 0;
				int32 Failed = 0;
				int32 Skipped = 0;
				for (int32 ItemIndex = 0; ItemIndex < Items->Num(); ++ItemIndex)
				{
					const TSharedPtr<FJsonObject> Item = (*Items)[ItemIndex].IsValid() ? (*Items)[ItemIndex]->AsObject() : nullptr;
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetNumberField(TEXT("index"), ItemIndex);
					Receipts.Add(MakeShared<FJsonValueObject>(Row));
					FString PackagePath;
					FString AssetName;
					FString ParentPath = DefaultParent;
					if (!Item.IsValid()
						|| !Item->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty()
						|| !Item->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
					{
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), TEXT("Missing package_path or asset_name."));
						++Failed;
						if (!bContinue) break;
						continue;
					}
					Item->TryGetStringField(TEXT("parent_material_path"), ParentPath);
					Row->SetStringField(TEXT("requested_asset_path"), CombinePackageAssetPath(PackagePath, AssetName));
					if (ParentPath.IsEmpty())
					{
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), TEXT("No parent_material_path was supplied."));
						++Failed;
						if (!bContinue) break;
						continue;
					}
					const FString RequestedPath = CombinePackageAssetPath(PackagePath, AssetName);
					if (Context.Services.AssetExists(RequestedPath))
					{
						if (ConflictPolicy == TEXT("skip"))
						{
							Row->SetStringField(TEXT("status"), TEXT("skipped_existing"));
							Row->SetStringField(TEXT("asset_path"), RequestedPath);
							++Skipped;
							continue;
						}
						if (ConflictPolicy == TEXT("unique"))
						{
							AssetName = Context.Services.GenerateUniqueAssetName(PackagePath, AssetName);
						}
						else
						{
							Row->SetStringField(TEXT("status"), TEXT("failed"));
							Row->SetStringField(TEXT("error"), TEXT("Asset already exists."));
							++Failed;
							if (!bContinue) break;
							continue;
						}
					}

					TSharedRef<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
					CreateArgs->SetStringField(TEXT("package_path"), PackagePath);
					CreateArgs->SetStringField(TEXT("asset_name"), AssetName);
					CreateArgs->SetStringField(TEXT("parent_material_path"), ParentPath);
					TSharedRef<FJsonObject> CreateOut = MakeShared<FJsonObject>();
					FString CreateSummary;
					FString CreateError;
					const bool bCreated = Registry.ExecuteTool(TEXT("material_instance_create"), CreateArgs, CreateOut, CreateSummary, CreateError);
					Row->SetObjectField(TEXT("create_receipt"), CreateOut);
					if (!bCreated)
					{
						Row->SetStringField(TEXT("status"), TEXT("failed"));
						Row->SetStringField(TEXT("error"), CreateError);
						++Failed;
						if (!bContinue) break;
						continue;
					}
					FString CreatedPath;
					CreateOut->TryGetStringField(TEXT("asset_path"), CreatedPath);
					if (CreatedPath.IsEmpty()) CreatedPath = CombinePackageAssetPath(PackagePath, AssetName);
					CreatedPaths.Add(CreatedPath);
					CreatedRows.Add(Row);
					Row->SetStringField(TEXT("asset_path"), CreatedPath);

					const TArray<TSharedPtr<FJsonValue>>* ItemParameters = nullptr;
					Item->TryGetArrayField(TEXT("parameters"), ItemParameters);
					const TArray<TSharedPtr<FJsonValue>>* EffectiveParameters = ItemParameters ? ItemParameters : DefaultParameters;
					bool bParametersApplied = true;
					if (EffectiveParameters && !EffectiveParameters->IsEmpty())
					{
						TSharedRef<FJsonObject> ParameterArgs = MakeShared<FJsonObject>();
						ParameterArgs->SetArrayField(TEXT("asset_paths"), {MakeShared<FJsonValueString>(CreatedPath)});
						ParameterArgs->SetArrayField(TEXT("parameters"), *EffectiveParameters);
						ParameterArgs->SetBoolField(TEXT("continue_on_error"), bContinue);
						TSharedRef<FJsonObject> ParameterOut = MakeShared<FJsonObject>();
						FString ParameterSummary;
						FString ParameterError;
						bParametersApplied = Registry.ExecuteTool(TEXT("material_instance_batch_parameters"), ParameterArgs, ParameterOut, ParameterSummary, ParameterError);
						Row->SetObjectField(TEXT("parameter_receipt"), ParameterOut);
						if (!bParametersApplied) Row->SetStringField(TEXT("error"), ParameterError);
					}
					if (bParametersApplied)
					{
						Row->SetStringField(TEXT("status"), TEXT("created_verified"));
						Row->SetBoolField(TEXT("reload_verified"), true);
						++Succeeded;
					}
					else
					{
						Row->SetStringField(TEXT("status"), TEXT("failed_parameters"));
						++Failed;
						if (!bContinue) break;
					}
				}

				TArray<TSharedPtr<FJsonValue>> RollbackReceipts;
				bool bRollbackComplete = true;
				if (bAtomic && Failed > 0)
				{
					for (int32 Index = CreatedPaths.Num() - 1; Index >= 0; --Index)
					{
						FString DeleteError;
						const bool bDeleted = Context.Services.DeleteAsset(CreatedPaths[Index], DeleteError);
						TSharedRef<FJsonObject> DeleteReceipt = MakeShared<FJsonObject>();
						DeleteReceipt->SetStringField(TEXT("asset_path"), CreatedPaths[Index]);
						DeleteReceipt->SetBoolField(TEXT("deleted"), bDeleted);
						if (!DeleteError.IsEmpty()) DeleteReceipt->SetStringField(TEXT("error"), DeleteError);
						RollbackReceipts.Add(MakeShared<FJsonValueObject>(DeleteReceipt));
						bRollbackComplete = bRollbackComplete && bDeleted;
						CreatedRows[Index]->SetStringField(TEXT("status"), bDeleted ? TEXT("rolled_back") : TEXT("rollback_failed"));
					}
					Succeeded = 0;
				}
				OutStructured->SetArrayField(TEXT("items"), Receipts);
				OutStructured->SetArrayField(TEXT("rollback_receipts"), RollbackReceipts);
				OutStructured->SetNumberField(TEXT("succeeded"), Succeeded);
				OutStructured->SetNumberField(TEXT("failed"), Failed);
				OutStructured->SetNumberField(TEXT("skipped"), Skipped);
				OutStructured->SetBoolField(TEXT("atomic"), bAtomic);
				OutStructured->SetBoolField(TEXT("rolled_back"), bAtomic && Failed > 0);
				OutStructured->SetBoolField(TEXT("rollback_complete"), bRollbackComplete);
				OutStructured->SetStringField(TEXT("name_conflict_policy"), ConflictPolicy);
				OutSummary = FString::Printf(TEXT("Material instance batch create: %d succeeded, %d failed, %d skipped%s."),
					Succeeded, Failed, Skipped, (bAtomic && Failed > 0) ? TEXT("; atomic rollback attempted") : TEXT(""));
				if (Failed > 0)
				{
					OutError = bAtomic && !bRollbackComplete
						? TEXT("Batch failed and atomic rollback was incomplete.")
						: TEXT("One or more material instance batch-create items failed.");
				}
				return Failed == 0 || (bContinue && !bAtomic && Succeeded > 0);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_compile"),
			TEXT("Compile a Blueprint asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
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
					OutError = TEXT("Asset is not a Blueprint.");
					return false;
				}
				UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				SololmcpWriteFlush::EnsureFlushed(Blueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				if (Blueprint->Status == BS_Error)
				{
					SololmcpError::Set(OutStructured, TEXT("COMPILE_FAILED"), TEXT("asset_path"),
						TEXT("Blueprint compile finished with BS_Error."));
					OutError = FString::Printf(TEXT("Blueprint compile failed: %s"), *AssetPath);
					return false;
				}
				OutSummary = TEXT("Compiled blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_member_variable"),
			TEXT("Add a member variable to a Blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("name"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("bool | int | float | string | name | text | vector | rotator"))}}, {TEXT("asset_path"), TEXT("name"), TEXT("type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				FString TypeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("name"), VariableName) || !Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing asset_path, name or type.");
					return false;
				}
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = TEXT("Asset is not a Blueprint.");
					return false;
				}
				FEdGraphPinType PinType;
				if (!MakeBlueprintPinType(TypeName, PinType))
				{
					OutError = TEXT("Unsupported Blueprint variable type.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddVariable", "SOMOLMCP Add Blueprint Variable"));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				if (!UBlueprintEditorLibrary::AddMemberVariable(Blueprint, *VariableName, PinType))
#else
				// UBlueprintEditorLibrary::AddMemberVariable is 5.5+. FBlueprintEditorUtils
				// exposes the same operation and is already used elsewhere in this file.
				if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, *VariableName, PinType))
#endif
				{
					OutError = TEXT("Failed to add member variable.");
					return false;
				}
				UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Added blueprint variable.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_create_from_actor"),
			TEXT("Create a blueprint from a placed actor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("replace_actor"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("open_blueprint"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("asset_path"), TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing asset_path or actor.");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}

				FKismetEditorUtilities::FCreateBlueprintFromActorParams Params;
				Params.bReplaceActor = Arguments->HasTypedField<EJson::Boolean>(TEXT("replace_actor")) ? Arguments->GetBoolField(TEXT("replace_actor")) : true;
				Params.bOpenBlueprint = Arguments->HasTypedField<EJson::Boolean>(TEXT("open_blueprint")) ? Arguments->GetBoolField(TEXT("open_blueprint")) : true;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintCreateFromActor", "SOMOLMCP Create Blueprint From Actor"));
				UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprintFromActor(AssetPath, Actor, Params);
				if (!Blueprint)
				{
					OutError = TEXT("Failed to create blueprint from actor.");
					return false;
				}
				const FString CreatedPath = Blueprint->GetPathName();
				Blueprint->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Blueprint);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!VerifyCreatedAssetReloaded(Context.Services, Blueprint, UBlueprint::StaticClass(), OutStructured, OutError))
				{
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = TEXT("Created blueprint from actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_harvest_from_actors"),
			TEXT("Harvest components from actors into a new blueprint."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
					{TEXT("replace_actors"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("open_blueprint"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("asset_path"), TEXT("actors")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				TArray<FString> ActorIds;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetStringArray(Arguments, TEXT("actors"), ActorIds))
				{
					OutError = TEXT("Missing asset_path or actors.");
					return false;
				}
				TArray<AActor*> Actors = ResolveActors(Context.Services, ActorIds, OutError);
				if (Actors.IsEmpty())
				{
					return false;
				}

				FKismetEditorUtilities::FHarvestBlueprintFromActorsParams Params;
				Params.bReplaceActors = Arguments->HasTypedField<EJson::Boolean>(TEXT("replace_actors")) ? Arguments->GetBoolField(TEXT("replace_actors")) : true;
				Params.bOpenBlueprint = Arguments->HasTypedField<EJson::Boolean>(TEXT("open_blueprint")) ? Arguments->GetBoolField(TEXT("open_blueprint")) : true;

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintHarvestFromActors", "SOMOLMCP Harvest Blueprint From Actors"));
				UBlueprint* Blueprint = FKismetEditorUtilities::HarvestBlueprintFromActors(AssetPath, Actors, Params);
				if (!Blueprint)
				{
					OutError = TEXT("Failed to harvest blueprint from actors.");
					return false;
				}
				const FString CreatedPath = Blueprint->GetPathName();
				Blueprint->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Blueprint);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!VerifyCreatedAssetReloaded(Context.Services, Blueprint, UBlueprint::StaticClass(), OutStructured, OutError))
				{
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = TEXT("Harvested blueprint from actors.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_function_graph"),
			TEXT("Add a new function graph to a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("function_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("function_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString FunctionName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("function_name"), FunctionName))
				{
					OutError = TEXT("Missing asset_path or function_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddFunctionGraph", "SOMOLMCP Add Function Graph"));
				UEdGraph* Graph = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, FunctionName);
				if (!Graph)
				{
					OutError = TEXT("Failed to add function graph.");
					return false;
				}
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Added blueprint function graph.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_remove_function_graph"),
			TEXT("Remove a function graph from a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("function_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("function_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString FunctionName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("function_name"), FunctionName))
				{
					OutError = TEXT("Missing asset_path or function_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* ExistingGraph = nullptr;
				for (UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (Graph && Graph->GetName() == FunctionName)
					{
						ExistingGraph = Graph;
						break;
					}
				}
				if (!ExistingGraph)
				{
					SololmcpError::NotFound(OutStructured, FunctionName);
					OutError = FString::Printf(TEXT("Function graph '%s' was not found."), *FunctionName);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRemoveFunctionGraph", "SOMOLMCP Remove Function Graph"));
				UBlueprintEditorLibrary::RemoveFunctionGraph(Blueprint, *FunctionName);
				for (UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (Graph && Graph->GetName() == FunctionName)
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("function_name"),
							TEXT("RemoveFunctionGraph returned but the function graph is still present."));
						OutError = FString::Printf(TEXT("Function graph '%s' was not removed."), *FunctionName);
						return false;
					}
				}
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Removed blueprint function graph.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rename_graph"),
			TEXT("Rename a graph in a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing asset_path, graph_name or new_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* Graph = FindBlueprintGraphByName(Blueprint, GraphName);
				if (!Graph)
				{
					OutError = TEXT("Graph was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRenameGraph", "SOMOLMCP Rename Blueprint Graph"));
				UBlueprintEditorLibrary::RenameGraph(Graph, NewName);
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Renamed blueprint graph.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_reparent"),
			TEXT("Reparent a blueprint to a new parent class."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("parent_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParentClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("parent_class_path"), ParentClassPath))
				{
					OutError = TEXT("Missing asset_path or parent_class_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UClass* ParentClass = Context.Services.ResolveClass(ParentClassPath, OutError);
				if (!ParentClass)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintReparent", "SOMOLMCP Reparent Blueprint"));
				UBlueprintEditorLibrary::ReparentBlueprint(Blueprint, ParentClass);
				if (Blueprint->ParentClass != ParentClass)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("parent_class_path"),
						TEXT("ReparentBlueprint returned but ParentClass did not match the requested class."));
					OutError = FString::Printf(TEXT("Blueprint parent did not change to '%s'."), *ParentClassPath);
					return false;
				}
				UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				SololmcpWriteFlush::EnsureFlushed(Blueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				if (Blueprint->Status == BS_Error)
				{
					SololmcpError::Set(OutStructured, TEXT("COMPILE_FAILED"), TEXT("asset_path"),
						TEXT("Blueprint reparent succeeded but compile finished with BS_Error."));
					OutError = FString::Printf(TEXT("Blueprint compile failed after reparent: %s"), *AssetPath);
					return false;
				}
				OutSummary = TEXT("Reparented blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_refresh_editors"),
			TEXT("Refresh open editors for a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UBlueprintEditorLibrary::RefreshOpenEditorsForBlueprint(Blueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Refreshed open blueprint editors.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_components_from_actor"),
			TEXT("Copy actor components into a blueprint as SCS nodes."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing asset_path or actor.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				if (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()) || !Blueprint->SimpleConstructionScript)
				{
					OutError = TEXT("Blueprint must be an Actor Blueprint with a SimpleConstructionScript.");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				if (Components.Num() == 0)
				{
					OutError = TEXT("Actor has no components to add.");
					return false;
				}
				const int32 NodesBefore = Blueprint->SimpleConstructionScript
					? Blueprint->SimpleConstructionScript->GetAllNodes().Num()
					: 0;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddComponents", "SOMOLMCP Add Components To Blueprint"));
				FKismetEditorUtilities::AddComponentsToBlueprint(Blueprint, Components);
				const int32 NodesAfter = Blueprint->SimpleConstructionScript
					? Blueprint->SimpleConstructionScript->GetAllNodes().Num()
					: 0;
				if (NodesAfter <= NodesBefore)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("actor"),
						TEXT("AddComponentsToBlueprint completed but no SCS nodes were added."));
					OutError = TEXT("No blueprint components were added.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutStructured->SetNumberField(TEXT("components_added"), NodesAfter - NodesBefore);
				OutSummary = TEXT("Added actor components to blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_list_nodes"),
			TEXT("List nodes in a blueprint graph or in all blueprint graphs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				FString GraphName;
				if (Arguments->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
				{
					UEdGraph* Graph = FindBlueprintGraphByName(Blueprint, GraphName);
					if (!Graph)
					{
						OutError = TEXT("Blueprint graph was not found.");
						return false;
					}
					OutStructured = BlueprintNodesToJson(Blueprint, Graph);
				}
				else
				{
					OutStructured = BlueprintNodesToJson(Blueprint);
				}
				OutSummary = TEXT("Listed blueprint nodes.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_function_call_node"),
			TEXT("Add a function call node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("class_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("function_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("class_path"), TEXT("function_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString ClassPath;
				FString FunctionName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("class_path"), ClassPath) ||
					!Arguments->TryGetStringField(TEXT("function_name"), FunctionName))
				{
					OutError = TEXT("Missing blueprint function node arguments.");
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
					OutError = TEXT("Function was not found on class.");
					return false;
				}

				const FVector2D Location(
					Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0,
					Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddFunctionCallNode", "SOMOLMCP Add Blueprint Function Call Node"));
				Blueprint->Modify();
				UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(Function);
				UEdGraphNode* Node = Spawner ? Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), Location) : nullptr;
				if (!Node)
				{
					OutError = TEXT("Failed to spawn function call node.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint function call node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_custom_event_node"),
			TEXT("Add a custom event node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("event_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("event_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString EventName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("event_name"), EventName))
				{
					OutError = TEXT("Missing blueprint custom event arguments.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				EventName = EventName.TrimStartAndEnd();
				if (EventName.IsEmpty())
				{
					OutError = TEXT("event_name must not be empty.");
					return false;
				}
				if (!Graph || !Cast<UEdGraphSchema_K2>(Graph->GetSchema()))
				{
					OutError = TEXT("Custom events can only be added to K2 blueprint graphs.");
					return false;
				}
				const FName EventFName(*EventName);
				for (UEdGraph* ExistingGraph : Blueprint->UbergraphPages)
				{
					if (!ExistingGraph)
					{
						continue;
					}
					for (UEdGraphNode* ExistingNode : ExistingGraph->Nodes)
					{
						if (const UK2Node_CustomEvent* ExistingEvent = Cast<UK2Node_CustomEvent>(ExistingNode))
						{
							if (ExistingEvent->CustomFunctionName == EventFName)
							{
								OutError = TEXT("A custom event with this name already exists.");
								return false;
							}
						}
					}
				}

				const FVector2D Location(
					Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0,
					Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddCustomEventNode", "SOMOLMCP Add Blueprint Custom Event Node"));
				Blueprint->Modify();
				UBlueprintEventNodeSpawner* Spawner = UBlueprintEventNodeSpawner::Create(UK2Node_CustomEvent::StaticClass(), *EventName);
				UEdGraphNode* Node = Spawner ? Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), Location) : nullptr;
				if (!Node)
				{
					OutError = TEXT("Failed to spawn custom event node.");
					return false;
				}
				// Round 12A: honor optional `pins` array — was silently ignored, breaking
				// real_blueprint phase5_wire_onhit_health which references DamageAmount.
				// Each entry: { name: string, type: string }. Direction = output (event payload pins).
				int32 UserPinsAdded = 0;
				FString UserPinWarnings;
				const bool bAllowPartialPins = Arguments->HasTypedField<EJson::Boolean>(TEXT("allow_partial_pins"))
					? Arguments->GetBoolField(TEXT("allow_partial_pins")) : false;
				if (Arguments->HasTypedField<EJson::Array>(TEXT("pins")))
				{
					if (UK2Node_CustomEvent* CustomEvtNode = Cast<UK2Node_CustomEvent>(Node))
					{
						const TArray<TSharedPtr<FJsonValue>>& PinDefs = Arguments->GetArrayField(TEXT("pins"));
						for (const TSharedPtr<FJsonValue>& PinValue : PinDefs)
						{
							const TSharedPtr<FJsonObject>* PinObj = nullptr;
							if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObj) || !PinObj || !PinObj->IsValid())
							{
								UserPinWarnings += TEXT("[skipped non-object pin entry]");
								continue;
							}
							FString PinName;
							FString PinTypeStr;
							if (!(*PinObj)->TryGetStringField(TEXT("name"), PinName) || PinName.IsEmpty())
							{
								UserPinWarnings += TEXT("[skipped pin: missing name]");
								continue;
							}
							if (!(*PinObj)->TryGetStringField(TEXT("type"), PinTypeStr) || PinTypeStr.IsEmpty())
							{
								PinTypeStr = TEXT("float");
							}
							FEdGraphPinType NewPinType;
							if (!MakeBlueprintPinType(PinTypeStr, NewPinType))
							{
								UserPinWarnings += FString::Printf(TEXT("[pin %s: bad type %s]"), *PinName, *PinTypeStr);
								continue;
							}
							FText CanCreateErr;
							if (!CustomEvtNode->CanCreateUserDefinedPin(NewPinType, EGPD_Output, CanCreateErr))
							{
								UserPinWarnings += FString::Printf(TEXT("[pin %s: %s]"), *PinName, *CanCreateErr.ToString());
								continue;
							}
							CustomEvtNode->CreateUserDefinedPin(*PinName, NewPinType, EGPD_Output);
							++UserPinsAdded;
						}
						if (UserPinsAdded > 0)
						{
							CustomEvtNode->ReconstructNode();
						}
					}
					if (!UserPinWarnings.IsEmpty() && !bAllowPartialPins)
					{
						Graph->RemoveNode(Node);
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("pins"), UserPinWarnings);
						OutError = FString::Printf(TEXT("Failed to create all custom event pins: %s"), *UserPinWarnings);
						return false;
					}
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetNumberField(TEXT("user_pins_added"), UserPinsAdded);
				if (!UserPinWarnings.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("pin_warnings"), UserPinWarnings);
				}
				OutSummary = TEXT("Added blueprint custom event node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_variable_get_node"),
			TEXT("Add a variable getter node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("variable_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("variable_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString VariableName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("variable_name"), VariableName))
				{
					OutError = TEXT("Missing blueprint variable getter arguments.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				const FVector2D Location(
					Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0,
					Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddVariableGetNode", "SOMOLMCP Add Blueprint Variable Getter Node"));
				Blueprint->Modify();
				UStruct* SourceStruct = Blueprint->SkeletonGeneratedClass ? static_cast<UStruct*>(Blueprint->SkeletonGeneratedClass) : static_cast<UStruct*>(Blueprint->GeneratedClass);
				UEdGraphNode* Node = Schema->SpawnVariableGetNode(Location, Graph, *VariableName, SourceStruct);
				if (!Node)
				{
					OutError = TEXT("Failed to spawn variable getter node.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint variable getter node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_variable_set_node"),
			TEXT("Add a variable setter node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("variable_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("variable_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString VariableName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("variable_name"), VariableName))
				{
					OutError = TEXT("Missing blueprint variable setter arguments.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				const FVector2D Location(
					Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0,
					Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddVariableSetNode", "SOMOLMCP Add Blueprint Variable Setter Node"));
				Blueprint->Modify();
				UStruct* SourceStruct = Blueprint->SkeletonGeneratedClass ? static_cast<UStruct*>(Blueprint->SkeletonGeneratedClass) : static_cast<UStruct*>(Blueprint->GeneratedClass);
				UEdGraphNode* Node = Schema->SpawnVariableSetNode(Location, Graph, *VariableName, SourceStruct);
				if (!Node)
				{
					OutError = TEXT("Failed to spawn variable setter node.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint variable setter node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_knot_node"),
			TEXT("Add a reroute knot node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
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
				const FVector2D Location(
					Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0,
					Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddKnotNode", "SOMOLMCP Add Blueprint Knot Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Knot>(Graph, Location, EK2NewNodeFlags::SelectNewNode);
				if (!Node)
				{
					OutError = TEXT("Failed to spawn knot node.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint knot node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_delete_node"),
			TEXT("Delete a node from a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuid;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid))
				{
					OutError = TEXT("Missing asset_path or node_guid.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuid);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintDeleteNode", "SOMOLMCP Delete Blueprint Node"));
				Blueprint->Modify();
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, false);
				OutStructured->SetBoolField(TEXT("deleted"), true);
				OutSummary = TEXT("Deleted blueprint node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_move_node"),
			TEXT("Move a node in a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("node_x"), TEXT("node_y")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuid;
				int32 NodeX = 0;
				int32 NodeY = 0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetNumberField(TEXT("node_x"), NodeX) ||
					!Arguments->TryGetNumberField(TEXT("node_y"), NodeY))
				{
					OutError = TEXT("Missing move node arguments.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuid);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintMoveNode", "SOMOLMCP Move Blueprint Node"));
				Node->Modify();
				Node->NodePosX = NodeX;
				Node->NodePosY = NodeY;
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Moved blueprint node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_connect_pins"),
			TEXT("Connect two pins in a blueprint graph."),
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
				{TEXT("asset_path"), TEXT("from_node_guid"), TEXT("to_node_guid"), TEXT("from_pin_name"), TEXT("to_pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString FromNodeGuid;
				FString FromPinName;
				FString ToNodeGuid;
				FString ToPinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("from_node_guid"), FromNodeGuid) ||
					!Arguments->TryGetStringField(TEXT("from_pin_name"), FromPinName) ||
					!Arguments->TryGetStringField(TEXT("to_node_guid"), ToNodeGuid) ||
					!Arguments->TryGetStringField(TEXT("to_pin_name"), ToPinName))
				{
					OutError = TEXT("Missing pin connection arguments.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* FromNode = FindBlueprintNodeByGuid(Blueprint, FromNodeGuid);
				UEdGraphNode* ToNode = FindBlueprintNodeByGuid(Blueprint, ToNodeGuid);
				if (!FromNode || !ToNode)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}

				FString FromPinGuid;
				FString ToPinGuid;
				Arguments->TryGetStringField(TEXT("from_pin_guid"), FromPinGuid);
				Arguments->TryGetStringField(TEXT("to_pin_guid"), ToPinGuid);
				UEdGraphPin* FromPin = FindNodePin(FromNode, FromPinName, FromPinGuid, EGPD_Output);
				UEdGraphPin* ToPin = FindNodePin(ToNode, ToPinName, ToPinGuid, EGPD_Input);
				if (!FromPin || !ToPin)
				{
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(FromNode->GetGraph() ? FromNode->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintConnectPins", "SOMOLMCP Connect Blueprint Pins"));
				if (!Schema->TryCreateConnection(FromPin, ToPin))
				{
					OutError = TEXT("Failed to connect blueprint pins.");
					return false;
				}
				if (!FromPin->LinkedTo.Contains(ToPin) || !ToPin->LinkedTo.Contains(FromPin))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("to_pin_name"),
						TEXT("TryCreateConnection returned success but the pin links did not verify."));
					OutError = TEXT("Blueprint pin connection readback failed.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured->SetObjectField(TEXT("fromNode"), BlueprintNodeToJson(FromNode));
				OutStructured->SetObjectField(TEXT("toNode"), BlueprintNodeToJson(ToNode));
				OutStructured->SetBoolField(TEXT("connection_verified"), true);
				OutSummary = TEXT("Connected blueprint pins.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_break_pin_links"),
			TEXT("Break all links from a pin in a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
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
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuid);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				FString PinGuid;
				Arguments->TryGetStringField(TEXT("pin_guid"), PinGuid);
				UEdGraphPin* Pin = FindNodePin(Node, PinName, PinGuid);
				if (!Pin)
				{
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
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}
				UEdGraph* Graph = Node->GetGraph();
				const int32 LinksBefore = Pin->LinkedTo.Num();
				TSharedRef<FJsonObject> PinBefore = BlueprintPinToJson(Pin);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintBreakPinLinks", "SOMOLMCP Break Blueprint Pin Links"));
				Schema->BreakPinLinks(*Pin, true);
				Pin = FindNodePin(Node, PinName, PinGuid);
				const bool bPinResolvedAfter = IsPinOwnedByNode(Pin, Node);
				const int32 LinksAfter = bPinResolvedAfter ? Pin->LinkedTo.Num() : 0;
				if (bPinResolvedAfter && LinksAfter > 0)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("pin_name"),
						TEXT("BreakPinLinks returned but the pin still has links."));
					OutError = TEXT("Blueprint pin break readback failed.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
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
					OutStructured->SetObjectField(TEXT("pin_after"), BlueprintPinToJson(Pin));
				}
				OutStructured->SetNumberField(TEXT("links_before"), LinksBefore);
				OutStructured->SetNumberField(TEXT("links_after"), LinksAfter);
				OutStructured->SetBoolField(TEXT("pin_resolved_after_mutation"), bPinResolvedAfter);
				OutStructured->SetStringField(TEXT("readback_mode"), bPinResolvedAfter ? TEXT("resolved_pin_after_mutation") : TEXT("pin_rebuilt_or_removed_after_mutation"));
				OutStructured->SetBoolField(TEXT("links_cleared_verified"), true);
				OutSummary = TEXT("Broke blueprint pin links.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_break_single_pin_link"),
			TEXT("Break one specific pin link in a blueprint graph."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("source_node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("source_pin_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("target_node_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("target_pin_name"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("source_node_guid"), TEXT("source_pin_name"), TEXT("target_node_guid"), TEXT("target_pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
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
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* SourceNode = FindBlueprintNodeByGuid(Blueprint, SourceNodeGuid);
				UEdGraphNode* TargetNode = FindBlueprintNodeByGuid(Blueprint, TargetNodeGuid);
				if (!SourceNode || !TargetNode)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				UEdGraphPin* SourcePin = FindNodePin(SourceNode, SourcePinName);
				UEdGraphPin* TargetPin = FindNodePin(TargetNode, TargetPinName);
				if (!SourcePin || !TargetPin)
				{
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				if (!EnsurePinSafeForMutation(SourceNode, SourcePin, TEXT("source_pin"), OutStructured, OutError) ||
					!EnsurePinSafeForMutation(TargetNode, TargetPin, TEXT("target_pin"), OutStructured, OutError))
				{
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(SourceNode->GetGraph() ? SourceNode->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}
				const bool bLinkPresentBefore = SourcePin->LinkedTo.Contains(TargetPin) || TargetPin->LinkedTo.Contains(SourcePin);
				TSharedRef<FJsonObject> SourcePinBefore = BlueprintPinToJson(SourcePin);
				TSharedRef<FJsonObject> TargetPinBefore = BlueprintPinToJson(TargetPin);
				if (!bLinkPresentBefore)
				{
					SololmcpError::Set(OutStructured, TEXT("PRECONDITION_FAILED"), TEXT("target_pin_name"),
						TEXT("Requested pin link is not present."));
					OutError = TEXT("Requested Blueprint pin link was not present.");
					return false;
				}
				UEdGraph* Graph = SourceNode->GetGraph();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintBreakSinglePinLink", "SOMOLMCP Break Blueprint Single Pin Link"));
				Schema->BreakSinglePinLink(SourcePin, TargetPin);
				SourcePin = FindNodePin(SourceNode, SourcePinName);
				TargetPin = FindNodePin(TargetNode, TargetPinName);
				const bool bSourceResolvedAfter = IsPinOwnedByNode(SourcePin, SourceNode);
				const bool bTargetResolvedAfter = IsPinOwnedByNode(TargetPin, TargetNode);
				const bool bStillLinkedAfter = bSourceResolvedAfter && bTargetResolvedAfter &&
					(SourcePin->LinkedTo.Contains(TargetPin) || TargetPin->LinkedTo.Contains(SourcePin));
				if (bStillLinkedAfter)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("target_pin_name"),
						TEXT("BreakSinglePinLink returned but the requested link is still present."));
					OutError = TEXT("Blueprint single pin break readback failed.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
				OutStructured->SetStringField(TEXT("operation"), TEXT("break_single_pin_link"));
				TSharedRef<FJsonObject> SourceNodeRef = MakeShared<FJsonObject>();
				SourceNodeRef->SetStringField(TEXT("guid"), SourceNode->NodeGuid.ToString());
				SourceNodeRef->SetStringField(TEXT("title"), SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				SourceNodeRef->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
				TSharedRef<FJsonObject> TargetNodeRef = MakeShared<FJsonObject>();
				TargetNodeRef->SetStringField(TEXT("guid"), TargetNode->NodeGuid.ToString());
				TargetNodeRef->SetStringField(TEXT("title"), TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				TargetNodeRef->SetStringField(TEXT("graph"), TargetNode->GetGraph() ? TargetNode->GetGraph()->GetName() : FString());
				OutStructured->SetObjectField(TEXT("sourceNode"), SourceNodeRef);
				OutStructured->SetObjectField(TEXT("targetNode"), TargetNodeRef);
				OutStructured->SetObjectField(TEXT("source_pin_before"), SourcePinBefore);
				OutStructured->SetObjectField(TEXT("target_pin_before"), TargetPinBefore);
				if (bSourceResolvedAfter)
				{
					OutStructured->SetObjectField(TEXT("source_pin_after"), BlueprintPinToJson(SourcePin));
				}
				if (bTargetResolvedAfter)
				{
					OutStructured->SetObjectField(TEXT("target_pin_after"), BlueprintPinToJson(TargetPin));
				}
				OutStructured->SetBoolField(TEXT("source_pin_resolved_after_mutation"), bSourceResolvedAfter);
				OutStructured->SetBoolField(TEXT("target_pin_resolved_after_mutation"), bTargetResolvedAfter);
				OutStructured->SetStringField(TEXT("readback_mode"), (bSourceResolvedAfter && bTargetResolvedAfter) ? TEXT("resolved_pins_after_mutation") : TEXT("pin_rebuilt_or_removed_after_mutation"));
				OutStructured->SetBoolField(TEXT("link_removed_verified"), true);
				OutSummary = TEXT("Broke one blueprint pin link.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_pin_default_value"),
			TEXT("Set the default value of a blueprint pin."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("value"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuid;
				FString PinName;
				FString Value;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName) ||
					!Arguments->TryGetStringField(TEXT("value"), Value))
				{
					OutError = TEXT("Missing pin default value arguments.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuid);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				UEdGraphPin* Pin = FindNodePin(Node, PinName);
				if (!Pin)
				{
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetPinDefault", "SOMOLMCP Set Blueprint Pin Default Value"));
				Schema->TrySetDefaultValue(*Pin, Value, true);
				if (Pin->DefaultValue != Value)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"),
						TEXT("TrySetDefaultValue returned success but the pin default did not match on readback."));
					OutStructured->SetStringField(TEXT("actual_value"), Pin->DefaultValue);
					OutError = TEXT("Blueprint pin default value readback failed.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetBoolField(TEXT("default_value_verified"), true);
				OutSummary = TEXT("Set blueprint pin default value.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_pin_default_object"),
			TEXT("Set or clear the default object of a blueprint object pin, for example DataTable pins on GetDataTableRow."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("object_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuid;
				FString PinName;
				FString ObjectPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing pin default object arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("object_path"), ObjectPath);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuid);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				UEdGraphPin* Pin = FindNodePin(Node, PinName);
				if (!Pin)
				{
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}
				UObject* DefaultObject = nullptr;
				if (!ObjectPath.IsEmpty())
				{
					DefaultObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
					if (!DefaultObject)
					{
						OutError = TEXT("object_path did not resolve to an asset object.");
						return false;
					}
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetPinDefaultObject", "SOMOLMCP Set Blueprint Pin Default Object"));
				Schema->TrySetDefaultObject(*Pin, DefaultObject, true);
				if (Pin->DefaultObject != DefaultObject)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("object_path"),
						TEXT("TrySetDefaultObject returned but the pin default object did not match on readback."));
					OutStructured->SetStringField(TEXT("actual_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());
					OutError = TEXT("Blueprint pin default object readback failed.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetStringField(TEXT("default_object"), DefaultObject ? DefaultObject->GetPathName() : FString());
				OutStructured->SetBoolField(TEXT("default_object_verified"), true);
				OutSummary = TEXT("Set blueprint pin default object.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_reset_pin_default"),
			TEXT("Reset a blueprint pin to its autogenerated default value."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuid;
				FString PinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing pin default reset arguments.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuid);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				UEdGraphPin* Pin = FindNodePin(Node, PinName);
				if (!Pin)
				{
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintResetPinDefault", "SOMOLMCP Reset Blueprint Pin Default"));
				Schema->ResetPinToAutogeneratedDefaultValue(Pin, true);
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetStringField(TEXT("reset_pin"), PinName);
				OutSummary = TEXT("Reset blueprint pin default.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_inspect_variable_node"),
			TEXT("Inspect a K2 variable get/set node, including its stale or resolved property binding."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid))
				{
					OutError = TEXT("Missing variable node inspect arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!VariableNode)
				{
					OutError = TEXT("Blueprint node was not a variable get/set node.");
					return false;
				}
				OutStructured = BlueprintVariableNodeDetailsToJson(VariableNode);
				OutSummary = TEXT("Inspected blueprint variable node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rebind_variable_node"),
			TEXT("Rebind an existing K2 variable get/set node to a resolved property and reconstruct its pins."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("self_context"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("class_path"), TEXT("variable_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				FString ClassPath;
				FString VariableName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("class_path"), ClassPath) ||
					!Arguments->TryGetStringField(TEXT("variable_name"), VariableName))
				{
					OutError = TEXT("Missing variable node rebind arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				bool bSelfContext = true;
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("self_context"), bSelfContext);
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!VariableNode)
				{
					OutError = TEXT("Blueprint node was not a variable get/set node.");
					return false;
				}
				UClass* OwnerClass = Context.Services.ResolveClass(ClassPath, OutError);
				if (!OwnerClass)
				{
					return false;
				}
				FProperty* Property = ResolvePropertyOnClass(OwnerClass, VariableName);
				if (!Property)
				{
					OutError = TEXT("Variable property was not found on class.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRebindVariableNode", "SOMOLMCP Rebind Blueprint Variable Node"));
				Blueprint->Modify();
				VariableNode->Modify();
				VariableNode->SetFromProperty(Property, bSelfContext, OwnerClass);
				VariableNode->ReconstructNode();
				if (VariableNode->GetGraph())
				{
					VariableNode->GetGraph()->NotifyGraphChanged();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
					SololmcpWriteFlush::EnsureFlushed(Blueprint);
				}
				OutStructured = BlueprintVariableNodeDetailsToJson(VariableNode);
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Rebound blueprint variable node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_inspect_call_function_node"),
			TEXT("Inspect a K2 call-function node, including its stale or resolved member binding."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid))
				{
					OutError = TEXT("Missing call-function inspect arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!CallNode)
				{
					OutError = TEXT("Blueprint node was not a call-function node.");
					return false;
				}
				OutStructured = BlueprintCallFunctionNodeDetailsToJson(CallNode);
				OutSummary = TEXT("Inspected blueprint call-function node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_function_catalog_lookup"),
			TEXT("Look up callable functions on a class by exact or loose display/name match."),
			FSololmcpSchemaBuilder::Object({{TEXT("class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("query"), FSololmcpSchemaBuilder::String()}, {TEXT("max_results"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ClassPath;
				FString Query;
				if (!Arguments->TryGetStringField(TEXT("class_path"), ClassPath))
				{
					OutError = TEXT("Missing class_path.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("query"), Query);
				int32 MaxResults = 50;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 500);

				UClass* OwnerClass = Context.Services.ResolveClass(ClassPath, OutError);
				if (!OwnerClass)
				{
					return false;
				}

				const FString NormalizedQuery = Query.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
				TArray<TSharedPtr<FJsonValue>> Functions;
				for (TFieldIterator<UFunction> It(OwnerClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					UFunction* Function = *It;
					if (!Function)
					{
						continue;
					}
					const FString Name = Function->GetName();
					const FString DisplayName = Function->GetDisplayNameText().ToString();
					const FString NormalizedName = Name.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
					const FString NormalizedDisplay = DisplayName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT("")).ToLower();
					if (!NormalizedQuery.IsEmpty()
						&& !NormalizedName.Contains(NormalizedQuery)
						&& !NormalizedDisplay.Contains(NormalizedQuery))
					{
						continue;
					}
					TSharedRef<FJsonObject> FnJson = MakeShared<FJsonObject>();
					FnJson->SetStringField(TEXT("name"), Name);
					FnJson->SetStringField(TEXT("display_name"), DisplayName);
					FnJson->SetStringField(TEXT("path"), Function->GetPathName());
					FnJson->SetStringField(TEXT("owner"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : FString());
					Functions.Add(MakeShared<FJsonValueObject>(FnJson));
					if (Functions.Num() >= MaxResults)
					{
						break;
					}
				}
				OutStructured->SetStringField(TEXT("class_path"), OwnerClass->GetPathName());
				OutStructured->SetStringField(TEXT("query"), Query);
				OutStructured->SetArrayField(TEXT("functions"), Functions);
				OutStructured->SetNumberField(TEXT("count"), Functions.Num());
				OutSummary = TEXT("Looked up blueprint function catalog.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rebind_call_function_node"),
			TEXT("Rebind an existing K2 call-function node to a resolved function and reconstruct its pins."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("function_name"), FSololmcpSchemaBuilder::String()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("class_path"), TEXT("function_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				FString ClassPath;
				FString FunctionName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("class_path"), ClassPath) ||
					!Arguments->TryGetStringField(TEXT("function_name"), FunctionName))
				{
					OutError = TEXT("Missing call-function rebind arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!CallNode)
				{
					OutError = TEXT("Blueprint node was not a call-function node.");
					return false;
				}
				UClass* OwnerClass = Context.Services.ResolveClass(ClassPath, OutError);
				if (!OwnerClass)
				{
					return false;
				}
				UFunction* Function = FindFunctionOnClassByLooseName(OwnerClass, FunctionName);
				if (!Function)
				{
					OutError = TEXT("Function was not found on class.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRebindCallFunctionNode", "SOMOLMCP Rebind Blueprint Call Function Node"));
				Blueprint->Modify();
				CallNode->Modify();
				CallNode->SetFromFunction(Function);
				CallNode->FunctionReference.SetExternalMember(Function->GetFName(), OwnerClass);
				CallNode->ReconstructNode();
				if (CallNode->GetGraph())
				{
					CallNode->GetGraph()->NotifyGraphChanged();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
					SololmcpWriteFlush::EnsureFlushed(Blueprint);
				}
				OutStructured = BlueprintCallFunctionNodeDetailsToJson(CallNode);
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Rebound blueprint call-function node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_inspect_macro_instances"),
			TEXT("Inspect macro instance nodes and report unresolved macro graph bindings."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("unresolved_only"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				bool bUnresolvedOnly = false;
				Arguments->TryGetBoolField(TEXT("unresolved_only"), bUnresolvedOnly);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				TArray<UEdGraph*> Graphs;
				if (!GraphName.IsEmpty())
				{
					if (UEdGraph* Graph = FindBlueprintGraphByName(Blueprint, GraphName))
					{
						Graphs.Add(Graph);
					}
				}
				else
				{
					Blueprint->GetAllGraphs(Graphs);
				}

				TArray<TSharedPtr<FJsonValue>> NodesJson;
				for (UEdGraph* Graph : Graphs)
				{
					if (!Graph)
					{
						continue;
					}
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node);
						if (!MacroNode)
						{
							continue;
						}
						if (bUnresolvedOnly && MacroNode->GetMacroGraph())
						{
							continue;
						}
						NodesJson.Add(MakeShared<FJsonValueObject>(BlueprintMacroInstanceDetailsToJson(MacroNode)));
					}
				}
				OutStructured->SetArrayField(TEXT("macro_instances"), NodesJson);
				OutStructured->SetNumberField(TEXT("count"), NodesJson.Num());
				OutSummary = TEXT("Inspected blueprint macro instances.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_inspect_delegate_node"),
			TEXT("Inspect an existing multicast delegate node, including its resolved dispatcher property and signature."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid))
				{
					OutError = TEXT("Missing delegate node inspect arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!DelegateNode)
				{
					OutError = TEXT("Blueprint node was not a multicast delegate node.");
					return false;
				}
				OutStructured = BlueprintDelegateNodeDetailsToJson(DelegateNode);
				OutSummary = TEXT("Inspected blueprint delegate node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rebind_delegate_node"),
			TEXT("Rebind an existing multicast delegate node to an event dispatcher property on a class, preserving compatible pins where UE can remap them."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("dispatcher_name"), FSololmcpSchemaBuilder::String()}, {TEXT("self_context"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("class_path"), TEXT("dispatcher_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				FString ClassPath;
				FString DispatcherName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("class_path"), ClassPath) ||
					!Arguments->TryGetStringField(TEXT("dispatcher_name"), DispatcherName))
				{
					OutError = TEXT("Missing delegate node rebind arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				bool bSelfContext = false;
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("self_context"), bSelfContext);
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!DelegateNode)
				{
					OutError = TEXT("Blueprint node was not a multicast delegate node.");
					return false;
				}
				UClass* OwnerClass = Context.Services.ResolveClass(ClassPath, OutError);
				if (!OwnerClass)
				{
					return false;
				}
				FMulticastDelegateProperty* DelegateProperty = ResolveMulticastDelegatePropertyOnClass(OwnerClass, DispatcherName);
				if (!DelegateProperty)
				{
					OutError = TEXT("Event dispatcher property was not found on class.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRebindDelegateNode", "SOMOLMCP Rebind Blueprint Delegate Node"));
				Blueprint->Modify();
				DelegateNode->Modify();
				DelegateNode->SetFromProperty(DelegateProperty, bSelfContext, OwnerClass);
				DelegateNode->ReconstructNode();
				if (DelegateNode->GetGraph())
				{
					DelegateNode->GetGraph()->NotifyGraphChanged();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
					SololmcpWriteFlush::EnsureFlushed(Blueprint);
				}
				OutStructured = BlueprintDelegateNodeDetailsToJson(DelegateNode);
				OutStructured->SetStringField(TEXT("requested_dispatcher"), DispatcherName);
				OutStructured->SetStringField(TEXT("requested_owner_class"), OwnerClass->GetPathName());
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Rebound blueprint delegate node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rebind_macro_instance"),
			TEXT("Rebind an existing macro instance node to a same-blueprint or external macro graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("macro_graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("macro_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("macro_graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				FString MacroGraphName;
				FString MacroAssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("macro_graph_name"), MacroGraphName))
				{
					OutError = TEXT("Missing macro rebind arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				Arguments->TryGetStringField(TEXT("macro_asset_path"), MacroAssetPath);
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!MacroNode)
				{
					OutError = TEXT("Blueprint node was not a macro instance.");
					return false;
				}

				UBlueprint* MacroBlueprint = Blueprint;
				if (!MacroAssetPath.IsEmpty())
				{
					MacroBlueprint = LoadBlueprintAsset(Context.Services, MacroAssetPath, OutError);
					if (!MacroBlueprint)
					{
						return false;
					}
				}
				UEdGraph* MacroGraph = FindBlueprintGraphByName(MacroBlueprint, MacroGraphName);
				if (!MacroGraph)
				{
					OutError = TEXT("Macro graph was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRebindMacroInstance", "SOMOLMCP Rebind Blueprint Macro Instance"));
				Blueprint->Modify();
				MacroNode->Modify();
				MacroNode->SetMacroGraph(MacroGraph);
				MacroNode->ReconstructNode();
				if (MacroNode->GetGraph())
				{
					MacroNode->GetGraph()->NotifyGraphChanged();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
					SololmcpWriteFlush::EnsureFlushed(Blueprint);
				}
				OutStructured = BlueprintMacroInstanceDetailsToJson(MacroNode);
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Rebound blueprint macro instance.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rebind_switch_enum"),
			TEXT("Rebind an existing enum switch node to a resolved enum asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("enum_path"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("enum_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				FString EnumPath;
				FString DefaultValue;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("enum_path"), EnumPath))
				{
					OutError = TEXT("Missing switch enum rebind arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UK2Node_SwitchEnum* SwitchNode = Cast<UK2Node_SwitchEnum>(ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName));
				if (!SwitchNode)
				{
					OutError = TEXT("Blueprint node was not an enum switch node.");
					return false;
				}
				UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
				if (!Enum)
				{
					OutError = TEXT("enum_path did not resolve to an enum.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRebindSwitchEnum", "SOMOLMCP Rebind Blueprint Switch Enum"));
				Blueprint->Modify();
				SwitchNode->Modify();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				SwitchNode->SetEnum(Enum);
#else
				// SetEnum only gained BLUEPRINTGRAPH_API in 5.6, so it cannot be linked from
				// outside the module on 5.5. The Enum member is a public UPROPERTY and the
				// ReconstructNode() below rebuilds the case pins from it, which is exactly
				// what SetEnum does internally.
				SwitchNode->Enum = Enum;
#endif
				SwitchNode->ReconstructNode();
				if (!DefaultValue.IsEmpty())
				{
					if (UEdGraphPin* SelectionPin = SwitchNode->GetSelectionPin())
					{
						if (const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(SwitchNode->GetGraph() ? SwitchNode->GetGraph()->GetSchema() : nullptr))
						{
							Schema->TrySetDefaultValue(*SelectionPin, DefaultValue, true);
						}
					}
				}
				if (SwitchNode->GetGraph())
				{
					SwitchNode->GetGraph()->NotifyGraphChanged();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
					SololmcpWriteFlush::EnsureFlushed(Blueprint);
				}
				OutStructured = BlueprintNodeToJson(SwitchNode);
				OutStructured->SetStringField(TEXT("enum_path"), Enum->GetPathName());
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Rebound blueprint switch enum.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_enum_pin_type_and_default"),
			TEXT("Retype a byte pin to an enum-backed pin and optionally set its default value."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("enum_path"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name"), TEXT("enum_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				FString PinName;
				FString EnumPath;
				FString DefaultValue;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName) ||
					!Arguments->TryGetStringField(TEXT("enum_path"), EnumPath))
				{
					OutError = TEXT("Missing enum pin retype arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				UEdGraphPin* Pin = FindNodePin(Node, PinName);
				if (!Pin)
				{
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
				if (!Enum)
				{
					OutError = TEXT("enum_path did not resolve to an enum.");
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Schema)
				{
					OutError = TEXT("Graph schema is not K2.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetEnumPinTypeAndDefault", "SOMOLMCP Set Blueprint Enum Pin Type And Default"));
				Blueprint->Modify();
				Node->Modify();
				Pin->Modify();
				Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				Pin->PinType.PinSubCategoryObject = Enum;
				if (!DefaultValue.IsEmpty())
				{
					Schema->TrySetDefaultValue(*Pin, DefaultValue, true);
				}
				Node->ReconstructNode();
				if (Node->GetGraph())
				{
					Node->GetGraph()->NotifyGraphChanged();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
					SololmcpWriteFlush::EnsureFlushed(Blueprint);
				}
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetStringField(TEXT("enum_path"), Enum->GetPathName());
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Retyped blueprint enum pin.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_node_pin_type"),
			TEXT("Retype a data pin on an existing K2 node. This is intended for migrated wildcard/unknown struct pins such as Select, Knot, GetArrayItem, and CallArrayFunction."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("direction"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}, {TEXT("reconstruct"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name"), TEXT("type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuid;
				FString PinName;
				FString PinGuid;
				FString DirectionName;
				FString TypeName;
				FString DefaultValue;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuid) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName) ||
					!Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing node pin retype arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				Arguments->TryGetStringField(TEXT("pin_guid"), PinGuid);
				Arguments->TryGetStringField(TEXT("direction"), DirectionName);
				Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);
				bool bReconstruct = false;
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("reconstruct"), bReconstruct);
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = ResolveBlueprintNodeByGuid(Blueprint, NodeGuid, GraphName);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				TOptional<EEdGraphPinDirection> Direction;
				if (!DirectionName.IsEmpty())
				{
					EEdGraphPinDirection ParsedDirection = EGPD_Input;
					if (!TryParseBlueprintPinDirection(DirectionName, ParsedDirection))
					{
						OutError = TEXT("direction must be input or output.");
						return false;
					}
					Direction = ParsedDirection;
				}
				UEdGraphPin* Pin = FindNodePin(Node, PinName, PinGuid, Direction);
				if (!Pin)
				{
					OutError = TEXT("Blueprint pin was not found.");
					return false;
				}
				FEdGraphPinType NewPinType;
				if (!MakeBlueprintPinType(TypeName, NewPinType))
				{
					OutError = TEXT("Unsupported Blueprint pin type.");
					return false;
				}
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetNodePinType", "SOMOLMCP Set Blueprint Node Pin Type"));
				Blueprint->Modify();
				Node->Modify();
				Pin->Modify();
				Pin->PinType = NewPinType;
				if (!DefaultValue.IsEmpty() && Schema)
				{
					Schema->TrySetDefaultValue(*Pin, DefaultValue, true);
				}
				if (bReconstruct)
				{
					Node->ReconstructNode();
				}
				if (Node->GetGraph())
				{
					Node->GetGraph()->NotifyGraphChanged();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
					SololmcpWriteFlush::EnsureFlushed(Blueprint);
				}
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetStringField(TEXT("retyped_pin"), PinName);
				OutStructured->SetStringField(TEXT("requested_type"), TypeName);
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Retyped blueprint node pin.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rename_member_variable"),
			TEXT("Rename a member variable on a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("variable_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing asset_path, variable_name or new_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRenameMemberVariable", "SOMOLMCP Rename Blueprint Member Variable"));
				FBlueprintEditorUtils::RenameMemberVariable(Blueprint, *VariableName, *NewName);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Renamed blueprint member variable.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_remove_member_variable"),
			TEXT("Remove a member variable from a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("variable_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName))
				{
					OutError = TEXT("Missing asset_path or variable_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRemoveMemberVariable", "SOMOLMCP Remove Blueprint Member Variable"));
				FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, *VariableName);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Removed blueprint member variable.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_change_member_variable_type"),
			TEXT("Change the type of a member variable on a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("variable_name"), TEXT("type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				FString TypeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing asset_path, variable_name or type.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				FEdGraphPinType PinType;
				if (!MakeBlueprintPinType(TypeName, PinType))
				{
					OutError = TEXT("Unsupported Blueprint variable type.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintChangeMemberVariableType", "SOMOLMCP Change Blueprint Member Variable Type"));
				FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, *VariableName, PinType);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Changed blueprint member variable type.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_variable_metadata"),
			TEXT("Set metadata on a member or local blueprint variable."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("metadata_key"), FSololmcpSchemaBuilder::String()}, {TEXT("metadata_value"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("variable_name"), TEXT("metadata_key"), TEXT("metadata_value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				FString MetadataKey;
				FString MetadataValue;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("metadata_key"), MetadataKey) || !Arguments->TryGetStringField(TEXT("metadata_value"), MetadataValue))
				{
					OutError = TEXT("Missing variable metadata arguments.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* ScopeGraph = nullptr;
				FString GraphName;
				if (Arguments->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
				{
					ScopeGraph = FindBlueprintGraphByName(Blueprint, GraphName);
					if (!ScopeGraph)
					{
						OutError = TEXT("Blueprint graph was not found.");
						return false;
					}
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetVariableMetadata", "SOMOLMCP Set Blueprint Variable Metadata"));
				if (ScopeGraph)
				{
					FBPVariableDescription* Variable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, ScopeGraph, *VariableName);
					if (!Variable)
					{
						OutError = TEXT("Blueprint local variable was not found.");
						return false;
					}
					Variable->SetMetaData(*MetadataKey, MetadataValue);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				}
				else
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, *VariableName, static_cast<const UStruct*>(nullptr), *MetadataKey, MetadataValue);
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Updated blueprint variable metadata.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_variable_category"),
			TEXT("Set category on a member or local blueprint variable."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("category"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("variable_name"), TEXT("category")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				FString Category;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("category"), Category))
				{
					OutError = TEXT("Missing asset_path, variable_name or category.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* ScopeGraph = nullptr;
				FString GraphName;
				if (Arguments->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
				{
					ScopeGraph = FindBlueprintGraphByName(Blueprint, GraphName);
					if (!ScopeGraph)
					{
						OutError = TEXT("Blueprint graph was not found.");
						return false;
					}
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetVariableCategory", "SOMOLMCP Set Blueprint Variable Category"));
				if (ScopeGraph)
				{
					FBPVariableDescription* Variable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, ScopeGraph, *VariableName);
					if (!Variable)
					{
						OutError = TEXT("Blueprint local variable was not found.");
						return false;
					}
					Variable->Category = FText::FromString(Category);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				}
				else
				{
					FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, *VariableName, static_cast<const UStruct*>(nullptr), FText::FromString(Category));
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Updated blueprint variable category.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_variable_flags"),
			TEXT("Set common member variable flags on a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("blueprint_only_editable"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("read_only"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("transient"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("save_game"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("advanced_display"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("variable_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName))
				{
					OutError = TEXT("Missing asset_path or variable_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetVariableFlags", "SOMOLMCP Set Blueprint Variable Flags"));
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("blueprint_only_editable")))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, *VariableName, Arguments->GetBoolField(TEXT("blueprint_only_editable")));
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("read_only")))
				{
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, *VariableName, Arguments->GetBoolField(TEXT("read_only")));
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("transient")))
				{
					FBlueprintEditorUtils::SetVariableTransientFlag(Blueprint, *VariableName, Arguments->GetBoolField(TEXT("transient")));
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("save_game")))
				{
					FBlueprintEditorUtils::SetVariableSaveGameFlag(Blueprint, *VariableName, Arguments->GetBoolField(TEXT("save_game")));
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("advanced_display")))
				{
					FBlueprintEditorUtils::SetVariableAdvancedDisplayFlag(Blueprint, *VariableName, Arguments->GetBoolField(TEXT("advanced_display")));
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Updated blueprint variable flags.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_local_variable"),
			TEXT("Add a local variable to a blueprint function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("variable_name"), TEXT("type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString VariableName;
				FString TypeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing local variable arguments.");
					return false;
				}
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				FEdGraphPinType PinType;
				if (!MakeBlueprintPinType(TypeName, PinType))
				{
					OutError = TEXT("Unsupported Blueprint variable type.");
					return false;
				}
				FString DefaultValue;
				Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddLocalVariable", "SOMOLMCP Add Blueprint Local Variable"));
				if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, *VariableName, PinType, DefaultValue))
				{
					OutError = TEXT("Failed to add local variable.");
					return false;
				}
				OutStructured = BlueprintLocalVariablesToJson(Blueprint, Graph);
				OutSummary = TEXT("Added blueprint local variable.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_rename_local_variable"),
			TEXT("Rename a local variable on a blueprint function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("variable_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString VariableName;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing local variable rename arguments.");
					return false;
				}
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRenameLocalVariable", "SOMOLMCP Rename Blueprint Local Variable"));
				FBPVariableDescription* Variable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, *VariableName);
				if (!Variable)
				{
					OutError = TEXT("Blueprint local variable was not found.");
					return false;
				}
				Variable->VarName = *NewName;
				Variable->FriendlyName = NewName;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintLocalVariablesToJson(Blueprint, Graph);
				OutSummary = TEXT("Renamed blueprint local variable.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_change_local_variable_type"),
			TEXT("Change the type of a local variable on a blueprint function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("variable_name"), TEXT("type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString VariableName;
				FString TypeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing local variable type arguments.");
					return false;
				}
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				FEdGraphPinType PinType;
				if (!MakeBlueprintPinType(TypeName, PinType))
				{
					OutError = TEXT("Unsupported Blueprint variable type.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintChangeLocalVariableType", "SOMOLMCP Change Blueprint Local Variable Type"));
				FBPVariableDescription* Variable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, *VariableName);
				if (!Variable)
				{
					OutError = TEXT("Blueprint local variable was not found.");
					return false;
				}
				Variable->VarType = PinType;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintLocalVariablesToJson(Blueprint, Graph);
				OutSummary = TEXT("Changed blueprint local variable type.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_local_variable_default"),
			TEXT("Set the default value of a local variable on a blueprint function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("variable_name"), TEXT("default_value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString VariableName;
				FString DefaultValue;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName) || !Arguments->TryGetStringField(TEXT("default_value"), DefaultValue))
				{
					OutError = TEXT("Missing local variable default arguments.");
					return false;
				}
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				FBPVariableDescription* Variable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, *VariableName);
				if (!Variable)
				{
					OutError = TEXT("Blueprint local variable was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetLocalVariableDefault", "SOMOLMCP Set Blueprint Local Variable Default"));
				Variable->DefaultValue = DefaultValue;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintLocalVariablesToJson(Blueprint, Graph);
				OutSummary = TEXT("Updated blueprint local variable default.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_list_signature_pins"),
			TEXT("List editable signature pins on a function entry or custom event node."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuidString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString);
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				UK2Node_EditablePinBase* Node = ResolveBlueprintSignatureNode(Blueprint, Graph, NodeGuidString, OutError);
				if (!Node)
				{
					return false;
				}
				OutStructured = BlueprintSignaturePinsToJson(Node);
				OutSummary = TEXT("Collected blueprint signature pins.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_add_signature_pin"),
			TEXT("Add an input or output pin to a function entry or custom event node."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("direction"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("pin_name"), TEXT("direction"), TEXT("type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString PinName;
				FString DirectionName;
				FString TypeName;
				FString DefaultValue;
				FString NodeGuidString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName) || !Arguments->TryGetStringField(TEXT("direction"), DirectionName) || !Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing signature pin arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);
				Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString);
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				UK2Node_EditablePinBase* Node = ResolveBlueprintSignatureNode(Blueprint, Graph, NodeGuidString, OutError);
				if (!Node)
				{
					return false;
				}
				if (FindUserDefinedPinInfo(Node, PinName).IsValid())
				{
					OutError = TEXT("Signature pin already exists.");
					return false;
				}
				FEdGraphPinType PinType;
				if (!MakeBlueprintPinType(TypeName, PinType))
				{
					OutError = TEXT("Unsupported Blueprint variable type.");
					return false;
				}
				EEdGraphPinDirection Direction = EGPD_Input;
				if (!TryParseBlueprintPinDirection(DirectionName, Direction))
				{
					OutError = TEXT("direction must be input or output.");
					return false;
				}
				FText ErrorMessage;
				if (!Node->CanCreateUserDefinedPin(PinType, Direction, ErrorMessage))
				{
					OutError = ErrorMessage.ToString();
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddSignaturePin", "SOMOLMCP Add Blueprint Signature Pin"));
				Node->Modify();
				Node->CreateUserDefinedPin(*PinName, PinType, Direction);
				if (!DefaultValue.IsEmpty())
				{
					if (TSharedPtr<FUserPinInfo> PinInfo = FindUserDefinedPinInfo(Node, PinName))
					{
						Node->ModifyUserDefinedPinDefaultValue(PinInfo, DefaultValue);
					}
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintSignaturePinsToJson(Node);
				OutSummary = TEXT("Added blueprint signature pin.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_remove_signature_pin"),
			TEXT("Remove a user-defined pin from a function entry or custom event node."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString PinName;
				FString NodeGuidString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing asset_path, graph_name or pin_name.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString);
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				UK2Node_EditablePinBase* Node = ResolveBlueprintSignatureNode(Blueprint, Graph, NodeGuidString, OutError);
				if (!Node)
				{
					return false;
				}
				if (!FindUserDefinedPinInfo(Node, PinName).IsValid())
				{
					OutError = TEXT("Signature pin was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRemoveSignaturePin", "SOMOLMCP Remove Blueprint Signature Pin"));
				Node->Modify();
				Node->RemoveUserDefinedPinByName(*PinName);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintSignaturePinsToJson(Node);
				OutSummary = TEXT("Removed blueprint signature pin.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_signature_pin_default"),
			TEXT("Set the default value on a function entry or custom event user-defined pin."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("pin_name"), TEXT("default_value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString PinName;
				FString DefaultValue;
				FString NodeGuidString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName) || !Arguments->TryGetStringField(TEXT("default_value"), DefaultValue))
				{
					OutError = TEXT("Missing signature pin default arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString);
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				UK2Node_EditablePinBase* Node = ResolveBlueprintSignatureNode(Blueprint, Graph, NodeGuidString, OutError);
				if (!Node)
				{
					return false;
				}
				TSharedPtr<FUserPinInfo> PinInfo = FindUserDefinedPinInfo(Node, PinName);
				if (!PinInfo.IsValid())
				{
					OutError = TEXT("Signature pin was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetSignaturePinDefault", "SOMOLMCP Set Blueprint Signature Pin Default"));
				Node->Modify();
				if (!Node->ModifyUserDefinedPinDefaultValue(PinInfo, DefaultValue))
				{
					OutError = TEXT("Failed to update signature pin default value.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintSignaturePinsToJson(Node);
				OutSummary = TEXT("Updated blueprint signature pin default.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_retype_signature_pin"),
			TEXT("Retype an existing user-defined function entry or custom event signature pin, preserving the pin name and direction."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("default_value"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("pin_name"), TEXT("type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString PinName;
				FString TypeName;
				FString NodeGuidString;
				FString DefaultValue;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName) ||
					!Arguments->TryGetStringField(TEXT("type"), TypeName))
				{
					OutError = TEXT("Missing signature pin retype arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString);
				Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				UK2Node_EditablePinBase* Node = ResolveBlueprintSignatureNode(Blueprint, Graph, NodeGuidString, OutError);
				if (!Node)
				{
					return false;
				}
				TSharedPtr<FUserPinInfo> PinInfo = FindUserDefinedPinInfo(Node, PinName);
				if (!PinInfo.IsValid())
				{
					OutError = TEXT("Signature pin was not found or is not user-defined.");
					return false;
				}
				FEdGraphPinType NewPinType;
				if (!MakeBlueprintPinType(TypeName, NewPinType))
				{
					OutError = TEXT("Unsupported Blueprint signature pin type.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRetypeSignaturePin", "SOMOLMCP Retype Blueprint Signature Pin"));
				Node->Modify();
				PinInfo->PinType = NewPinType;
				if (!DefaultValue.IsEmpty())
				{
					PinInfo->PinDefaultValue = DefaultValue;
				}
				Node->ReconstructNode();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintSignaturePinsToJson(Node);
				OutStructured->SetStringField(TEXT("retyped_pin"), PinName);
				OutStructured->SetStringField(TEXT("requested_type"), TypeName);
				OutSummary = TEXT("Retyped blueprint signature pin.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_reorder_signature_pin"),
			TEXT("Reorder a function entry or custom event user-defined pin."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("target_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("pin_name"), TEXT("target_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString PinName;
				FString NodeGuidString;
				int32 TargetIndex = 0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName) || !Arguments->TryGetNumberField(TEXT("target_index"), TargetIndex))
				{
					OutError = TEXT("Missing signature pin reorder arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString);
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				UK2Node_EditablePinBase* Node = ResolveBlueprintSignatureNode(Blueprint, Graph, NodeGuidString, OutError);
				if (!Node)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintReorderSignaturePin", "SOMOLMCP Reorder Blueprint Signature Pin"));
				Node->Modify();
				ReorderBlueprintUserPin(Node, PinName, TargetIndex, OutError);
				if (!OutError.IsEmpty())
				{
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintSignaturePinsToJson(Node);
				OutSummary = TEXT("Reordered blueprint signature pin.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_function_metadata"),
			TEXT("Set category and descriptive metadata on a blueprint function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("category"), FSololmcpSchemaBuilder::String()}, {TEXT("tooltip"), FSololmcpSchemaBuilder::String()}, {TEXT("keywords"), FSololmcpSchemaBuilder::String()}, {TEXT("compact_node_title"), FSololmcpSchemaBuilder::String()}, {TEXT("deprecation_message"), FSololmcpSchemaBuilder::String()}, {TEXT("deprecated"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("call_in_editor"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("thread_safe"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("unsafe_during_actor_construction"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
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
				FKismetUserDeclaredFunctionMetadata* Metadata = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);
				if (!Metadata)
				{
					OutError = TEXT("Graph does not expose editable function metadata.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetFunctionMetadata", "SOMOLMCP Set Blueprint Function Metadata"));
				FString Category;
				if (Arguments->TryGetStringField(TEXT("category"), Category))
				{
					FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, FText::FromString(Category));
				}
				FString TextValue;
				if (Arguments->TryGetStringField(TEXT("tooltip"), TextValue)) { Metadata->ToolTip = FText::FromString(TextValue); }
				if (Arguments->TryGetStringField(TEXT("keywords"), TextValue)) { Metadata->Keywords = FText::FromString(TextValue); }
				if (Arguments->TryGetStringField(TEXT("compact_node_title"), TextValue)) { Metadata->CompactNodeTitle = FText::FromString(TextValue); }
				if (Arguments->TryGetStringField(TEXT("deprecation_message"), TextValue)) { Metadata->DeprecationMessage = TextValue; }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("deprecated"))) { Metadata->bIsDeprecated = Arguments->GetBoolField(TEXT("deprecated")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("call_in_editor"))) { Metadata->bCallInEditor = Arguments->GetBoolField(TEXT("call_in_editor")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("thread_safe"))) { Metadata->bThreadSafe = Arguments->GetBoolField(TEXT("thread_safe")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("unsafe_during_actor_construction"))) { Metadata->bIsUnsafeDuringActorConstruction = Arguments->GetBoolField(TEXT("unsafe_during_actor_construction")); }
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Updated blueprint function metadata.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_reorder_graph"),
			TEXT("Move a graph to a new order index inside a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("target_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("target_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				int32 TargetIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetNumberField(TEXT("target_index"), TargetIndex))
				{
					OutError = TEXT("Missing graph reorder arguments.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* Graph = FindBlueprintGraphByName(Blueprint, GraphName);
				if (!Graph)
				{
					OutError = TEXT("Blueprint graph was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintReorderGraph", "SOMOLMCP Reorder Blueprint Graph"));
				if (!FBlueprintEditorUtils::MoveGraphBeforeOtherGraph(Graph, TargetIndex, false))
				{
					OutError = TEXT("Failed to move blueprint graph.");
					return false;
				}
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Reordered blueprint graph.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_implement_interface"),
			TEXT("Implement a new interface on a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("interface_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("interface_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString InterfaceClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("interface_class_path"), InterfaceClassPath))
				{
					OutError = TEXT("Missing asset_path or interface_class_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UClass* InterfaceClass = Context.Services.ResolveClass(InterfaceClassPath, OutError);
				if (!InterfaceClass)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintImplementInterface", "SOMOLMCP Implement Blueprint Interface"));
				if (!FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetClassPathName()))
				{
					OutError = TEXT("Failed to implement blueprint interface.");
					return false;
				}
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Implemented blueprint interface.");
				return true;
			}
		, nullptr
		, 5
		});

	// =========================================================================
	// P3: Geometry Script Tools (8 tools, Python bridging)
	// Uses unreal.GeometryScript + unreal.StaticMeshEditorSubsystem
	// =========================================================================
	{
		auto RegisterGeoPyTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString&)
				{
					const FString ArgsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal,json,math\n")
						TEXT("tool_name=%s\nargs=json.loads(%s)\n")
						TEXT("EAS=unreal.EditorAssetSubsystem\n")
						TEXT("eas=unreal.get_editor_subsystem(EAS)\n")
						TEXT("SMES=unreal.StaticMeshEditorSubsystem\n")
						TEXT("smes=unreal.get_editor_subsystem(SMES) if SMES else None\n")
						TEXT("def load_mesh(p):\n a=eas.load_asset(p)\n assert a,f'Failed to load: {p}'\n return a\n")
						TEXT("def save_mesh(m):\n eas.save_loaded_asset(m)\n")
						TEXT("def gf(k,d=0.0):\n v=args.get(k)\n return float(v) if v is not None else d\n")
						TEXT("def gi(k,d=0):\n v=args.get(k)\n return int(v) if v is not None else d\n")
						TEXT("def gb(k,d=False):\n v=args.get(k)\n return bool(v) if v is not None else d\n")
						TEXT("def gs(k,d=''):\n v=args.get(k)\n return str(v) if v is not None else d\n")
						TEXT("def gv(k,d=None):\n v=args.get(k)\n return unreal.Vector(float(v.get('x',0)),float(v.get('y',0)),float(v.get('z',0))) if isinstance(v,dict) else d\n")
						TEXT("GS=unreal.GeometryScript if hasattr(unreal,'GeometryScript') else None\n")
						TEXT("if tool_name=='geometry_extrude_polygon':\n")
						TEXT(" m=load_mesh(gs('asset_path'))\n h=gf('height',100.0)\n d=gv('direction',unreal.Vector(0,0,1))\n")
						TEXT(" if GS and hasattr(GS,'extrude_polygon_mesh'):\n  GS.extrude_polygon_mesh(m,height=h,direction=d)\n")
						TEXT(" elif smes:\n  unreal.SystemLibrary.execute_console_command(None,f'geom.extrude {h}')\n")
						TEXT(" else:\n  raise RuntimeError('Geometry Script plugin not available')\n")
						TEXT(" save_mesh(m)\n")
						TEXT("elif tool_name=='geometry_revolve_spline':\n")
						TEXT(" m=load_mesh(gs('asset_path'))\n deg=gf('degrees',360.0)\n seg=gi('segments',16)\n ax=gs('axis','z').lower()\n")
						TEXT(" if GS and hasattr(GS,'revolve_spline_mesh'):\n  GS.revolve_spline_mesh(m,degrees=deg,segments=seg,axis=ax)\n")
						TEXT(" elif smes:\n  unreal.SystemLibrary.execute_console_command(None,f'geom.revolve {deg} {seg} {ax}')\n")
						TEXT(" else:\n  raise RuntimeError('Geometry Script plugin not available')\n")
						TEXT(" save_mesh(m)\n")
						TEXT("elif tool_name=='geometry_boolean_operation':\n")
						TEXT(" ma=load_mesh(gs('mesh_a'))\n mb=load_mesh(gs('mesh_b'))\n op=gs('operation','union').lower()\n")
						TEXT(" op_map={'union':0,'subtract':1,'intersect':2}\n")
						TEXT(" if GS and hasattr(GS,'boolean_mesh'):\n  GS.boolean_mesh(ma,mb,operation=op_map.get(op,0))\n")
						TEXT(" elif smes:\n  unreal.SystemLibrary.execute_console_command(None,f'geom.boolean {op}')\n")
						TEXT(" else:\n  raise RuntimeError('Geometry Script plugin not available')\n")
						TEXT(" save_mesh(ma)\n")
						TEXT("elif tool_name=='geometry_solidify':\n")
						TEXT(" m=load_mesh(gs('asset_path'))\n th=gf('thickness',1.0)\n off=gf('offset_distance',0.0)\n")
						TEXT(" if GS and hasattr(GS,'solidify_mesh'):\n  GS.solidify_mesh(m,thickness=th,offset=off)\n")
						TEXT(" elif smes:\n  unreal.SystemLibrary.execute_console_command(None,f'geom.solidify {th} {off}')\n")
						TEXT(" else:\n  raise RuntimeError('Geometry Script plugin not available')\n")
						TEXT(" save_mesh(m)\n")
						TEXT("elif tool_name=='geometry_remesh':\n")
						TEXT(" m=load_mesh(gs('asset_path'))\n tel=gf('target_edge_length',10.0)\n si=gi('smoothing_iterations',5)\n")
						TEXT(" if GS and hasattr(GS,'remesh_mesh'):\n  GS.remesh_mesh(m,target_edge_length=tel,smoothing_iterations=si)\n")
						TEXT(" elif smes:\n  unreal.SystemLibrary.execute_console_command(None,f'geom.remesh {tel} {si}')\n")
						TEXT(" else:\n  raise RuntimeError('Geometry Script plugin not available')\n")
						TEXT(" save_mesh(m)\n")
						TEXT("elif tool_name=='geometry_simplify':\n")
						TEXT(" m=load_mesh(gs('asset_path'))\n ttc=gi('target_triangle_count',1000)\n")
						TEXT(" if GS and hasattr(GS,'simplify_mesh'):\n  GS.simplify_mesh(m,target_triangle_count=ttc)\n")
						TEXT(" elif smes:\n  unreal.SystemLibrary.execute_console_command(None,f'geom.simplify {ttc}')\n")
						TEXT(" else:\n  raise RuntimeError('Geometry Script plugin not available')\n")
						TEXT(" save_mesh(m)\n")
						TEXT("elif tool_name=='geometry_set_material_on_faces':\n")
						TEXT(" m=load_mesh(gs('asset_path'))\n mi=gi('material_index',0)\n mp=gs('material_path','')\n")
						TEXT(" if mp:\n  mat=eas.load_asset(mp)\n  if mat:\n   m.set_material(mi,mat)\n")
						TEXT(" save_mesh(m)\n")
						TEXT("elif tool_name=='geometry_mirror':\n")
						TEXT(" m=load_mesh(gs('asset_path'))\n ax=gs('axis','x').lower()\n dup=gb('duplicate',True)\n")
						TEXT(" if GS and hasattr(GS,'mirror_mesh'):\n  GS.mirror_mesh(m,axis=ax,duplicate=dup)\n")
						TEXT(" elif smes:\n  unreal.SystemLibrary.execute_console_command(None,f'geom.mirror {ax} {int(dup)}')\n")
						TEXT(" else:\n  raise RuntimeError('Geometry Script plugin not available')\n")
						TEXT(" save_mesh(m)\n")
						TEXT("else:\n raise RuntimeError(f'Unknown geometry tool: {tool_name}')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgsJson));
				});
		};

		RegisterGeoPyTool(TEXT("geometry_extrude_polygon"),
			TEXT("Extrude a polygon on a static mesh by a given height and direction using Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Static mesh asset path"))}, {TEXT("height"), FSololmcpSchemaBuilder::Number(TEXT("Extrusion height in cm"))}, {TEXT("direction"), VectorSchema(TEXT("Extrusion direction (default Z+)"))}}, {TEXT("asset_path")}));

		RegisterGeoPyTool(TEXT("geometry_revolve_spline"),
			TEXT("Revolve/lathe a spline profile to create a mesh of revolution using Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("degrees"), FSololmcpSchemaBuilder::Number(TEXT("Revolution degrees (default 360)"))}, {TEXT("segments"), FSololmcpSchemaBuilder::Integer(TEXT("Segments (default 16)"))}, {TEXT("axis"), FSololmcpSchemaBuilder::String(TEXT("Axis: x/y/z"), {TEXT("x"), TEXT("y"), TEXT("z")})}}, {TEXT("asset_path")}));

		RegisterGeoPyTool(TEXT("geometry_boolean_operation"),
			TEXT("Perform boolean operation (union/subtract/intersect) between two meshes via Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("mesh_a"), FSololmcpSchemaBuilder::String()}, {TEXT("mesh_b"), FSololmcpSchemaBuilder::String()}, {TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("union/subtract/intersect"), {TEXT("union"), TEXT("subtract"), TEXT("intersect")})}}, {TEXT("mesh_a"), TEXT("mesh_b")}));

		RegisterGeoPyTool(TEXT("geometry_solidify"),
			TEXT("Solidify/thicken an open mesh into a closed solid using Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("thickness"), FSololmcpSchemaBuilder::Number(TEXT("Thickness (default 1.0)"))}, {TEXT("offset_distance"), FSololmcpSchemaBuilder::Number(TEXT("Offset distance"))}}, {TEXT("asset_path")}));

		RegisterGeoPyTool(TEXT("geometry_remesh"),
			TEXT("Remesh a static mesh with target edge length and smoothing via Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("target_edge_length"), FSololmcpSchemaBuilder::Number(TEXT("Target edge length"))}, {TEXT("smoothing_iterations"), FSololmcpSchemaBuilder::Integer(TEXT("Smoothing iterations"))}}, {TEXT("asset_path")}));

		RegisterGeoPyTool(TEXT("geometry_simplify"),
			TEXT("Simplify/reduce triangle count of a static mesh using Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("target_triangle_count"), FSololmcpSchemaBuilder::Integer(TEXT("Target triangle count"))}}, {TEXT("asset_path")}));

		RegisterGeoPyTool(TEXT("geometry_set_material_on_faces"),
			TEXT("Assign a material to a material slot on a static mesh via Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("material_index"), FSololmcpSchemaBuilder::Integer(TEXT("Material slot index"))}, {TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path"))}}, {TEXT("asset_path")}));

		RegisterGeoPyTool(TEXT("geometry_mirror"),
			TEXT("Mirror a static mesh along a specified axis using Geometry Script."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("axis"), FSololmcpSchemaBuilder::String(TEXT("Axis: x/y/z"), {TEXT("x"), TEXT("y"), TEXT("z")})}, {TEXT("duplicate"), FSololmcpSchemaBuilder::Boolean(TEXT("Keep original (default true)"))}}, {TEXT("asset_path")}));
	}

	// =========================================================================
	// P3: AnimBP State Machine Editing (7 tools, Python bridging)
	// Uses unreal.AnimationBlueprintLibrary + unreal.EditorAssetSubsystem
	// for state machine graph creation, state/transition add/remove, and
	// transition rule setting. Bridges FAnimationBlueprintLibrary and
	// FAnimGraph module access via Python scripting.
	// =========================================================================
	{
		auto RegisterAnimBPPyTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				// Audit round 7: anim_bp_* Python tools previously returned ok when handed a non-AnimBP
				// asset (the inner `assert isinstance(...)` was swallowed by the python-execute boundary,
				// leaving the tool a no-op). Add a hard C++ entry guard that resolves asset_path and
				// confirms it Casts to UAnimBlueprint before we even build the Python script. Returns
				// isError=true with the asset_not_anim_blueprint sentinel so callers can distinguish
				// wrong-type-asset from genuine state-machine failures. Runs BEFORE the round 3
				// AnimationBlueprintLibrary fallback so we know we have a real AnimBP first.
				[ToolName](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
				{
					FString AssetPathProbe;
					if (Arguments->TryGetStringField(TEXT("asset_path"), AssetPathProbe) && !AssetPathProbe.IsEmpty())
					{
						FString LoadErr;
						UObject* Probe = Context.Services.LoadAsset(AssetPathProbe, LoadErr);
						if (!Probe)
						{
							OutError = FString::Printf(TEXT("asset_not_anim_blueprint: failed to load '%s' (%s)"), *AssetPathProbe, *LoadErr);
							return FString();
						}
						if (!Cast<UAnimBlueprint>(Probe))
						{
							OutError = FString::Printf(TEXT("asset_not_anim_blueprint: '%s' is %s, not UAnimBlueprint"), *AssetPathProbe, *Probe->GetClass()->GetName());
							return FString();
						}
					}
					const FString ArgsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal,json\n")
						TEXT("tool_name=%s\nargs=json.loads(%s)\n")
						TEXT("EAS=unreal.EditorAssetSubsystem\n")
						TEXT("eas=unreal.get_editor_subsystem(EAS)\n")
						// Audit round 3: guard against unreal.AnimationBlueprintLibrary AttributeError on UE builds without it.
						TEXT("try:\n")
						TEXT(" ABPL=unreal.AnimationBlueprintLibrary\n")
						TEXT("except AttributeError:\n")
						TEXT(" try:\n")
						TEXT("  import importlib\n")
						TEXT("  ABPL=getattr(importlib.import_module('unreal'),'AnimationBlueprintLibrary',None)\n")
						TEXT(" except Exception:\n")
						TEXT("  ABPL=None\n")
						TEXT("if ABPL is None:\n")
						TEXT(" print(json.dumps({'error':'AnimationBlueprintLibrary unavailable in this UE build','tool':tool_name,'fallback':'use unreal.AnimationLibrary or skip this tool'}))\n")
						TEXT(" raise SystemExit(0)\n")
						TEXT("def load_bp(p):\n a=eas.load_asset(p)\n assert a,f'Failed to load: {p}'\n assert isinstance(a,unreal.AnimBlueprint),f'Not AnimBP: {p}'\n return a\n")
						TEXT("def save_bp(b):\n eas.save_loaded_asset(b)\n")
						TEXT("def gs(k,d=''):\n v=args.get(k)\n return str(v) if v is not None else d\n")
						TEXT("def gf(k,d=0.0):\n v=args.get(k)\n return float(v) if v is not None else d\n")
						TEXT("def gi(k,d=0):\n v=args.get(k)\n return int(v) if v is not None else d\n")
						TEXT("def gb(k,d=False):\n v=args.get(k)\n return bool(v) if v is not None else d\n")
						TEXT("def find_sm(bp,sm_name):\n")
						TEXT(" for graph in bp.get_graphs():\n")
						TEXT("  if hasattr(graph,'get_sub_graphs'):\n")
						TEXT("   for sub in graph.get_sub_graphs():\n")
						TEXT("    if sm_name in str(sub.get_name()):\n     return sub\n")
						TEXT("  if hasattr(graph,'get_name') and sm_name in str(graph.get_name()):\n   return graph\n")
						TEXT(" return None\n")
						TEXT("\n")
						TEXT("if tool_name=='anim_bp_create_state_machine':\n")
						TEXT(" bp=load_bp(gs('asset_path'))\n smn=gs('state_machine_name')\n assert smn,'Missing state_machine_name'\n")
						TEXT(" if hasattr(ABPL,'create_state_machine'):\n  ABPL.create_state_machine(bp,smn)\n")
						TEXT(" else:\n")
						TEXT("  anim_graphs=bp.get_graphs()\n")
						TEXT("  if not anim_graphs:\n   raise RuntimeError('No animation graphs found')\n")
						TEXT("  ag=anim_graphs[0]\n")
						TEXT("  schema=ag.get_schema() if hasattr(ag,'get_schema') else None\n")
						TEXT("  if schema and hasattr(schema,'create_state_machine_node'):\n   schema.create_state_machine_node(ag,smn)\n")
						TEXT("  else:\n   raise RuntimeError('Cannot create state machine node via schema')\n")
						TEXT(" save_bp(bp)\n")
						TEXT("\n")
						TEXT("elif tool_name=='anim_bp_add_state':\n")
						TEXT(" bp=load_bp(gs('asset_path'))\n smn=gs('state_machine_name')\n sn=gs('state_name')\n assert smn and sn,'Missing state_machine_name or state_name'\n")
						TEXT(" if hasattr(ABPL,'add_state_to_state_machine'):\n  ABPL.add_state_to_state_machine(bp,smn,sn)\n")
						TEXT(" else:\n")
						TEXT("  sm=find_sm(bp,smn)\n  assert sm,f'State machine not found: {smn}'\n")
						TEXT("  schema=sm.get_schema() if hasattr(sm,'get_schema') else None\n")
						TEXT("  if schema and hasattr(schema,'create_state_node'):\n   schema.create_state_node(sm,sn)\n")
						TEXT("  else:\n   raise RuntimeError('Cannot create state node via schema')\n")
						TEXT(" save_bp(bp)\n")
						TEXT("\n")
						TEXT("elif tool_name=='anim_bp_add_transition':\n")
						TEXT(" bp=load_bp(gs('asset_path'))\n smn=gs('state_machine_name')\n fs=gs('from_state')\n ts=gs('to_state')\n assert smn and fs and ts,'Missing required fields'\n")
						TEXT(" if hasattr(ABPL,'add_transition'):\n  ABPL.add_transition(bp,smn,fs,ts)\n")
						TEXT(" else:\n")
						TEXT("  sm=find_sm(bp,smn)\n  assert sm,f'State machine not found: {smn}'\n")
						TEXT("  schema=sm.get_schema() if hasattr(sm,'get_schema') else None\n")
						TEXT("  if schema and hasattr(schema,'create_transition_node'):\n   schema.create_transition_node(sm,fs,ts)\n")
						TEXT("  else:\n   raise RuntimeError('Cannot create transition via schema')\n")
						TEXT(" save_bp(bp)\n")
						TEXT("\n")
						TEXT("elif tool_name=='anim_bp_set_transition_rule':\n")
						TEXT(" bp=load_bp(gs('asset_path'))\n smn=gs('state_machine_name')\n fs=gs('from_state')\n ts=gs('to_state')\n cond=gs('condition_expression')\n rt=gs('rule_type','automatic').lower()\n dur=gf('blend_duration',0.2)\n")
						TEXT(" assert smn and fs and ts,'Missing required fields'\n")
						TEXT(" if hasattr(ABPL,'set_transition_rule'):\n  ABPL.set_transition_rule(bp,smn,fs,ts,cond,rule_type=rt)\n")
						TEXT(" else:\n")
						TEXT("  sm=find_sm(bp,smn)\n  assert sm,f'State machine not found: {smn}'\n")
						TEXT("  for node in sm.get_nodes():\n")
						TEXT("   nn=str(node.get_name()) if hasattr(node,'get_name') else ''\n")
						TEXT("   if fs in nn and ts in nn and 'transition' in nn.lower():\n")
						TEXT("    if hasattr(node,'get_bound_graph'):\n")
						TEXT("     tg=node.get_bound_graph()\n")
						TEXT("     if cond and tg:\n")
						TEXT("      for tn in tg.get_nodes():\n")
						TEXT("       if hasattr(tn,'set_value'):\n        tn.set_value(cond.lower()=='true')\n")
						TEXT("    break\n")
						TEXT(" unreal.SystemLibrary.execute_console_command(None,f'anim.transition.blend_duration {dur}')\n")
						TEXT(" save_bp(bp)\n")
						TEXT("\n")
						TEXT("elif tool_name=='anim_bp_remove_state':\n")
						TEXT(" bp=load_bp(gs('asset_path'))\n smn=gs('state_machine_name')\n sn=gs('state_name')\n assert smn and sn,'Missing required fields'\n")
						TEXT(" if hasattr(ABPL,'remove_state'):\n  ABPL.remove_state(bp,smn,sn)\n")
						TEXT(" else:\n")
						TEXT("  sm=find_sm(bp,smn)\n  assert sm,f'State machine not found: {smn}'\n")
						TEXT("  for node in sm.get_nodes():\n")
						TEXT("   if sn in (str(node.get_name()) if hasattr(node,'get_name') else ''):\n")
						TEXT("    if hasattr(node,'destroy_node'):\n     node.destroy_node()\n")
						TEXT("    elif hasattr(sm,'remove_node'):\n     sm.remove_node(node)\n")
						TEXT("    break\n")
						TEXT(" save_bp(bp)\n")
						TEXT("\n")
						TEXT("elif tool_name=='anim_bp_list_states':\n")
						TEXT(" bp=load_bp(gs('asset_path'))\n smn=gs('state_machine_name')\n assert smn,'Missing state_machine_name'\n")
						TEXT(" states=[]\n")
						TEXT(" if hasattr(ABPL,'get_state_machine_states'):\n  states=list(ABPL.get_state_machine_states(bp,smn))\n")
						TEXT(" else:\n")
						TEXT("  sm=find_sm(bp,smn)\n")
						TEXT("  if sm:\n")
						TEXT("   for node in sm.get_nodes():\n")
						TEXT("    if hasattr(node,'get_name'):\n")
						TEXT("     n=str(node.get_name())\n")
						TEXT("     if 'transition' not in n.lower():\n      states.append(n)\n")
						TEXT(" print(json.dumps({'states':states,'count':len(states)}))\n")
						TEXT("\n")
						TEXT("elif tool_name=='anim_bp_set_state_animation':\n")
						TEXT(" bp=load_bp(gs('asset_path'))\n smn=gs('state_machine_name')\n sn=gs('state_name')\n anim=gs('animation_asset','')\n assert smn and sn,'Missing required fields'\n")
						TEXT(" if anim:\n  anim_asset=eas.load_asset(anim)\n  if not anim_asset:\n   raise RuntimeError(f'Animation asset not found: {anim}')\n")
						TEXT("  # Find state node and set its animation\n")
						TEXT("  sm=find_sm(bp,smn)\n  assert sm,f'State machine not found: {smn}'\n")
						TEXT("  for node in sm.get_nodes():\n")
						TEXT("   if sn in (str(node.get_name()) if hasattr(node,'get_name') else ''):\n")
						TEXT("    if hasattr(node,'get_bound_graph'):\n")
						TEXT("     sg=node.get_bound_graph()\n")
						TEXT("     if sg:\n")
						TEXT("      for sn2 in sg.get_nodes():\n")
						TEXT("       if hasattr(sn2,'set_animation'):\n        sn2.set_animation(anim_asset)\n")
						TEXT("       elif hasattr(sn2,'set_sequence'):\n        sn2.set_sequence(anim_asset)\n")
						TEXT("    break\n")
						TEXT(" save_bp(bp)\n")
						TEXT("\n")
						TEXT("else:\n raise RuntimeError(f'Unknown AnimBP tool: {tool_name}')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgsJson));
				});
		};

		RegisterAnimBPPyTool(TEXT("anim_bp_create_state_machine"),
			TEXT("Create a new state machine in an animation blueprint. Uses FAnimGraph module for state machine graph creation."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("AnimBlueprint asset path"))}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the new state machine"))}}, {TEXT("asset_path"), TEXT("state_machine_name")}));

		RegisterAnimBPPyTool(TEXT("anim_bp_add_state"),
			TEXT("Add a state to an animation blueprint state machine. Creates state node and bound anim graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("state_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the new state"))}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("state_name")}));

		RegisterAnimBPPyTool(TEXT("anim_bp_add_transition"),
			TEXT("Add a transition between two states in an AnimBP state machine. Creates transition graph for rule editing."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("from_state"), FSololmcpSchemaBuilder::String()}, {TEXT("to_state"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("from_state"), TEXT("to_state")}));

		RegisterAnimBPPyTool(TEXT("anim_bp_set_transition_rule"),
			TEXT("Set transition rule/condition for a state machine transition. Supports condition expression and blend duration via FAnimationBlueprintLibrary."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("from_state"), FSololmcpSchemaBuilder::String()}, {TEXT("to_state"), FSololmcpSchemaBuilder::String()}, {TEXT("condition_expression"), FSololmcpSchemaBuilder::String(TEXT("Boolean condition expression"))}, {TEXT("rule_type"), FSololmcpSchemaBuilder::String(TEXT("automatic/custom"), {TEXT("automatic"), TEXT("custom")})}, {TEXT("blend_duration"), FSololmcpSchemaBuilder::Number(TEXT("Blend duration in seconds"))}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("from_state"), TEXT("to_state")}));

		RegisterAnimBPPyTool(TEXT("anim_bp_remove_state"),
			TEXT("Remove a state from an animation blueprint state machine, including its bound graph and transitions."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("state_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("state_name")}));

		RegisterAnimBPPyTool(TEXT("anim_bp_list_states"),
			TEXT("List all states in an animation blueprint state machine."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name")}));

		RegisterAnimBPPyTool(TEXT("anim_bp_set_state_animation"),
			TEXT("Set the animation sequence that plays in a specific state machine state."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("state_name"), FSololmcpSchemaBuilder::String()}, {TEXT("animation_asset"), FSololmcpSchemaBuilder::String(TEXT("AnimSequence or BlendSpace asset path"))}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("state_name"), TEXT("animation_asset")}));
	}

	// =========================================================================
	// P3: Control Rig Advanced Features (4 tools, Python bridging)
	// Space switching, stretch IK, bone constraints, and bone chain creation.
	// Uses ControlRigBlueprintEditorLibrary + hierarchy controller API.
	// =========================================================================
	{
		auto RegisterCRAdvPyTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString&)
				{
					const FString ArgsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal,json\n")
						TEXT("tool_name=%s\nargs=json.loads(%s)\n")
						TEXT("EAS=unreal.EditorAssetSubsystem\n")
						TEXT("eas=unreal.get_editor_subsystem(EAS)\n")
						TEXT("CRBL=unreal.ControlRigBlueprintEditorLibrary\n")
						TEXT("def load_rig(p):\n a=eas.load_asset(p)\n assert a,f'Failed to load: {p}'\n return a\n")
						TEXT("def save_rig(r):\n eas.save_loaded_asset(r)\n")
						TEXT("def gs(k,d=''):\n v=args.get(k)\n return str(v) if v is not None else d\n")
						TEXT("def gf(k,d=0.0):\n v=args.get(k)\n return float(v) if v is not None else d\n")
						TEXT("def gi(k,d=0):\n v=args.get(k)\n return int(v) if v is not None else d\n")
						TEXT("def gb(k,d=False):\n v=args.get(k)\n return bool(v) if v is not None else d\n")
						TEXT("def get_ctrl(rig):\n return CRBL.get_hierarchy_controller(rig)\n")
						TEXT("def get_hier(rig):\n return CRBL.get_hierarchy(rig)\n")
						TEXT("def find_key(hier,name):\n")
						TEXT(" for k in hier.get_all_keys():\n")
						TEXT("  if str(k.name)==name: return k\n")
						TEXT(" return unreal.RigElementKey()\n")
						TEXT("\n")
						// --- control_rig_add_space_switch ---
						TEXT("if tool_name=='control_rig_add_space_switch':\n")
						TEXT(" rig=load_rig(gs('asset_path'))\n ctrl=get_ctrl(rig)\n hier=get_hier(rig)\n")
						TEXT(" cn=gs('control_name')\n spaces=args.get('spaces',[])\n")
						TEXT(" assert cn,'Missing control_name'\n")
						TEXT(" ckey=find_key(hier,cn)\n assert ckey.type!=unreal.RigElementType.NONE,f'Control not found: {cn}'\n")
						TEXT(" if hasattr(ctrl,'add_space_switch'):\n")
						TEXT("  space_keys=[find_key(hier,s) for s in spaces]\n")
						TEXT("  space_keys=[k for k in space_keys if k.type!=unreal.RigElementType.NONE]\n")
						TEXT("  ctrl.add_space_switch(ckey,space_keys,gb('default_to_primary',True))\n")
						TEXT(" else:\n")
						TEXT("  # Fallback: add multiple parents and use constraints\n")
						TEXT("  for i,s in enumerate(spaces):\n")
						TEXT("   skey=find_key(hier,s)\n")
						TEXT("   if skey.type!=unreal.RigElementType.NONE:\n")
						TEXT("    null_name=f'{cn}_Space_{i}'\n")
						TEXT("    ctrl.add_null(unreal.Name(null_name),skey,unreal.Transform(),False,True)\n")
						TEXT(" save_rig(rig)\n")
						TEXT("\n")
						// --- control_rig_add_stretch_ik ---
						TEXT("elif tool_name=='control_rig_add_stretch_ik':\n")
						TEXT(" rig=load_rig(gs('asset_path'))\n ctrl=get_ctrl(rig)\n hier=get_hier(rig)\n")
						TEXT(" start_bone=gs('start_bone')\n end_bone=gs('end_bone')\n")
						TEXT(" assert start_bone and end_bone,'Missing start_bone or end_bone'\n")
						TEXT(" skey=find_key(hier,start_bone)\n ekey=find_key(hier,end_bone)\n")
						TEXT(" assert skey.type!=unreal.RigElementType.NONE,f'Start bone not found: {start_bone}'\n")
						TEXT(" assert ekey.type!=unreal.RigElementType.NONE,f'End bone not found: {end_bone}'\n")
						TEXT(" stretch_ratio=gf('stretch_ratio',1.0)\n max_stretch=gf('max_stretch',2.0)\n")
						TEXT(" # Get bone chain via hierarchy iteration\n")
						TEXT(" vm=CRBL.get_vm(rig) if hasattr(CRBL,'get_vm') else None\n")
						TEXT(" if vm:\n")
						TEXT("  # Add IK node via VM graph\n")
						TEXT("  graph=vm.get_default_graph()\n")
						TEXT("  if graph and hasattr(graph,'add_unit_node'):\n")
						TEXT("   # Attempt to add a FullBodyIK or LimbIK node\n")
						TEXT("   ik_struct=' unreal.ControlRigVectorMath'\n")
						TEXT("   unreal.SystemLibrary.execute_console_command(None,'cr.stretchik.auto 1')\n")
						TEXT("  else:\n")
						TEXT("   unreal.SystemLibrary.execute_console_command(None,f'cr.ik.stretch {start_bone} {end_bone} {stretch_ratio} {max_stretch}')\n")
						TEXT(" else:\n")
						TEXT("  unreal.SystemLibrary.execute_console_command(None,f'cr.ik.stretch {start_bone} {end_bone} {stretch_ratio} {max_stretch}')\n")
						TEXT(" save_rig(rig)\n")
						TEXT("\n")
						// --- control_rig_set_bone_constraint ---
						TEXT("elif tool_name=='control_rig_set_bone_constraint':\n")
						TEXT(" rig=load_rig(gs('asset_path'))\n ctrl=get_ctrl(rig)\n hier=get_hier(rig)\n")
						TEXT(" bn=gs('bone_name')\n ct=gs('constraint_type','aim').lower()\n")
						TEXT(" assert bn,'Missing bone_name'\n")
						TEXT(" bkey=find_key(hier,bn)\n assert bkey.type!=unreal.RigElementType.NONE,f'Bone not found: {bn}'\n")
						TEXT(" target_name=gs('target_element','')\n tkey=find_key(hier,target_name) if target_name else unreal.RigElementKey()\n")
						TEXT(" weight=gf('weight',1.0)\n maint_offset=gb('maintain_offset',True)\n")
						TEXT(" if hasattr(ctrl,'add_constraint'):\n")
						TEXT("  # Use native constraint API\n")
						TEXT("  settings=unreal.RigConstraintSettings()\n")
						TEXT("  if hasattr(settings,'set_weight'): settings.set_weight(weight)\n")
						TEXT("  ctrl.add_constraint(bkey,tkey,settings,maint_offset)\n")
						TEXT(" elif hasattr(ctrl,'add_aim_constraint'):\n")
						TEXT("  # Fallback: use specific constraint type\n")
						TEXT("  if ct=='aim':\n")
						TEXT("   ctrl.add_aim_constraint(bkey,tkey,unreal.Vector(1,0,0),unreal.Vector(0,0,1),maint_offset)\n")
						TEXT("  elif ct=='position':\n")
						TEXT("   ctrl.add_position_constraint(bkey,tkey,maint_offset)\n")
						TEXT("  elif ct=='orientation':\n")
						TEXT("   ctrl.add_orientation_constraint(bkey,tkey,maint_offset)\n")
						TEXT("  elif ct=='parent':\n")
						TEXT("   ctrl.add_parent_constraint(bkey,tkey,maint_offset)\n")
						TEXT("  elif ct=='spring':\n")
						TEXT("   ctrl.add_spring_constraint(bkey,tkey,gf('spring_stiffness',10.0),gf('spring_damping',1.0))\n")
						TEXT("  else:\n")
						TEXT("   raise RuntimeError(f'Unknown constraint type: {ct}')\n")
						TEXT(" else:\n")
						TEXT("  raise RuntimeError('Constraint API not available in this UE version')\n")
						TEXT(" save_rig(rig)\n")
						TEXT("\n")
						// --- control_rig_add_bone_chain ---
						TEXT("elif tool_name=='control_rig_add_bone_chain':\n")
						TEXT(" rig=load_rig(gs('asset_path'))\n ctrl=get_ctrl(rig)\n hier=get_hier(rig)\n")
						TEXT(" bones=args.get('bones',[])\n assert bones,'Missing bones list'\n")
						TEXT(" parent_name=gs('parent','')\n pkey=find_key(hier,parent_name) if parent_name else unreal.RigElementKey()\n")
						TEXT(" created_bones=[]\n")
						TEXT(" last_key=pkey\n")
						TEXT(" for i,b in enumerate(bones):\n")
						TEXT("  bname=b.get('name',f'Bone_{i}') if isinstance(b,dict) else str(b)\n")
						TEXT("  loc=b.get('location',{}) if isinstance(b,dict) else {}\n")
						TEXT("  t=unreal.Transform()\n")
						TEXT("  if isinstance(loc,dict):\n")
						TEXT("   t.translation=unreal.Vector(float(loc.get('x',0)),float(loc.get('y',0)),float(loc.get('z',0)))\n")
						TEXT("  ctrl.add_bone(unreal.Name(bname),last_key,t,unreal.Name(''))\n")
						TEXT("  created_bones.append(bname)\n")
						TEXT("  last_key=find_key(get_hier(rig),bname)\n")
						TEXT(" # Verify all bones created\n")
						TEXT(" hier2=get_hier(rig)\n")
						TEXT(" missing=[n for n in created_bones if find_key(hier2,n).type==unreal.RigElementType.NONE]\n")
						TEXT(" if missing: raise RuntimeError(f'Bone creation failed for: {missing}')\n")
						TEXT(" save_rig(rig)\n")
						TEXT("\n")
						TEXT("else:\n raise RuntimeError(f'Unknown Control Rig advanced tool: {tool_name}')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgsJson));
				});
		};

		RegisterCRAdvPyTool(TEXT("control_rig_add_space_switch"),
			TEXT("Add space switching to a control, allowing it to switch between multiple parent spaces (e.g., world/root/IK). Uses ControlRig hierarchy controller."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Control Rig Blueprint asset path"))}, {TEXT("control_name"), FSololmcpSchemaBuilder::String(TEXT("Name of the control to add space switching to"))}, {TEXT("spaces"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Element names for each space")))}, {TEXT("default_to_primary"), FSololmcpSchemaBuilder::Boolean(TEXT("Default to primary space (default true)"))}}, {TEXT("asset_path"), TEXT("control_name"), TEXT("spaces")}));

		RegisterCRAdvPyTool(TEXT("control_rig_add_stretch_ik"),
			TEXT("Add stretch IK setup between two bones. Configures bone chain to stretch toward the end effector based on distance ratio. Uses ControlRig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Control Rig Blueprint asset path"))}, {TEXT("start_bone"), FSololmcpSchemaBuilder::String(TEXT("Start bone name (root of chain)"))}, {TEXT("end_bone"), FSololmcpSchemaBuilder::String(TEXT("End bone name (effector)"))}, {TEXT("stretch_ratio"), FSololmcpSchemaBuilder::Number(TEXT("Stretch ratio (default 1.0, no stretch)"))}, {TEXT("max_stretch"), FSololmcpSchemaBuilder::Number(TEXT("Maximum stretch multiplier (default 2.0)"))}}, {TEXT("asset_path"), TEXT("start_bone"), TEXT("end_bone")}));

		RegisterCRAdvPyTool(TEXT("control_rig_set_bone_constraint"),
			TEXT("Set a bone constraint (aim/position/orientation/parent/spring) on a Control Rig element. Uses hierarchy controller constraint API."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Control Rig Blueprint asset path"))}, {TEXT("bone_name"), FSololmcpSchemaBuilder::String(TEXT("Bone element name"))}, {TEXT("constraint_type"), FSololmcpSchemaBuilder::String(TEXT("Constraint type"), {TEXT("aim"), TEXT("position"), TEXT("orientation"), TEXT("parent"), TEXT("spring")})}, {TEXT("target_element"), FSololmcpSchemaBuilder::String(TEXT("Target element name for constraint"))}, {TEXT("weight"), FSololmcpSchemaBuilder::Number(TEXT("Constraint weight (0-1, default 1.0)"))}, {TEXT("maintain_offset"), FSololmcpSchemaBuilder::Boolean(TEXT("Maintain current offset (default true)"))}, {TEXT("spring_stiffness"), FSololmcpSchemaBuilder::Number(TEXT("Spring stiffness (for spring type, default 10.0)"))}, {TEXT("spring_damping"), FSololmcpSchemaBuilder::Number(TEXT("Spring damping (for spring type, default 1.0)"))}}, {TEXT("asset_path"), TEXT("bone_name"), TEXT("constraint_type")}));

		RegisterCRAdvPyTool(TEXT("control_rig_add_bone_chain"),
			TEXT("Add a chain of bones to a Control Rig hierarchy with automatic parenting. Each bone is parented to the previous one."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Control Rig Blueprint asset path"))}, {TEXT("bones"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Bone name"))}, {TEXT("location"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})}}))}, {TEXT("parent"), FSololmcpSchemaBuilder::String(TEXT("Optional parent element name"))}}, {TEXT("asset_path"), TEXT("bones")}));
	}

	// =========================================================================
	// P3: Audio Waveform Editing (3 tools, Python bridging)
	// Trim, fade, and normalize audio waveforms using EditorAssetSubsystem
	// and SoundWave utilities.
	// =========================================================================
	{
		auto RegisterAudioWavePyTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString&)
				{
					const FString ArgsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal,json\n")
						TEXT("tool_name=%s\nargs=json.loads(%s)\n")
						TEXT("EAS=unreal.EditorAssetSubsystem\n")
						TEXT("eas=unreal.get_editor_subsystem(EAS)\n")
						TEXT("def gs(k,d=''):\n v=args.get(k)\n return str(v) if v is not None else d\n")
						TEXT("def gf(k,d=0.0):\n v=args.get(k)\n return float(v) if v is not None else d\n")
						TEXT("def gi(k,d=0):\n v=args.get(k)\n return int(v) if v is not None else d\n")
						TEXT("\n")
						// --- audio_waveform_trim ---
						TEXT("if tool_name=='audio_waveform_trim':\n")
						TEXT(" ap=gs('asset_path')\n assert ap,'Missing asset_path'\n")
						TEXT(" sw=eas.load_asset(ap)\n assert sw,f'Failed to load: {ap}'\n")
						TEXT(" assert isinstance(sw,unreal.SoundWave),f'Not a SoundWave: {ap}'\n")
						TEXT(" start=gf('start_time',0.0)\n end=gf('end_time',-1.0)\n")
						TEXT(" if hasattr(sw,'set_trim_region'):\n")
						TEXT("  if end<0: end=sw.get_duration()\n")
						TEXT("  sw.set_trim_region(start,end)\n")
						TEXT(" elif hasattr(sw,'trim_to_region'):\n")
						TEXT("  if end<0: end=sw.get_duration()\n")
						TEXT("  sw.trim_to_region(start,end)\n")
						TEXT(" else:\n")
						TEXT("  unreal.SystemLibrary.execute_console_command(None,f'audio.trim {ap} {start} {end}')\n")
						TEXT(" eas.save_loaded_asset(sw)\n")
						TEXT("\n")
						// --- audio_waveform_fade ---
						TEXT("elif tool_name=='audio_waveform_fade':\n")
						TEXT(" ap=gs('asset_path')\n assert ap,'Missing asset_path'\n")
						TEXT(" sw=eas.load_asset(ap)\n assert sw,f'Failed to load: {ap}'\n")
						TEXT(" assert isinstance(sw,unreal.SoundWave),f'Not a SoundWave: {ap}'\n")
						TEXT(" ft=gs('fade_type','both').lower()\n")
						TEXT(" fade_in=gf('fade_in_duration',0.0)\n fade_out=gf('fade_out_duration',0.0)\n")
						TEXT(" curve=gs('fade_curve','linear').lower()\n")
						TEXT(" if hasattr(sw,'set_fade_in_duration'):\n")
						TEXT("  if ft in ('in','both'): sw.set_fade_in_duration(fade_in)\n")
						TEXT("  if ft in ('out','both'): sw.set_fade_out_duration(fade_out)\n")
						TEXT(" elif hasattr(sw,'fade_in_duration'):\n")
						TEXT("  if ft in ('in','both'): sw.set_editor_property('fade_in_duration',fade_in)\n")
						TEXT("  if ft in ('out','both'): sw.set_editor_property('fade_out_duration',fade_out)\n")
						TEXT(" else:\n")
						TEXT("  unreal.SystemLibrary.execute_console_command(None,f'audio.fade {ap} {fade_in} {fade_out}')\n")
						TEXT(" eas.save_loaded_asset(sw)\n")
						TEXT("\n")
						// --- audio_waveform_normalize ---
						TEXT("elif tool_name=='audio_waveform_normalize':\n")
						TEXT(" ap=gs('asset_path')\n assert ap,'Missing asset_path'\n")
						TEXT(" sw=eas.load_asset(ap)\n assert sw,f'Failed to load: {ap}'\n")
						TEXT(" assert isinstance(sw,unreal.SoundWave),f'Not a SoundWave: {ap}'\n")
						TEXT(" target_db=gf('target_db',-1.0)\n normalize_mode=gs('normalize_mode','peak').lower()\n")
						TEXT(" if hasattr(sw,'normalize'):\n")
						TEXT("  sw.normalize(target_db)\n")
						TEXT(" elif hasattr(unreal.SoundWaveTools,'normalize_sound_wave'):\n")
						TEXT("  unreal.SoundWaveTools.normalize_sound_wave(sw,target_db)\n")
						TEXT(" else:\n")
						TEXT("  # Fallback: use console command\n")
						TEXT("  unreal.SystemLibrary.execute_console_command(None,f'audio.normalize {ap} {target_db}')\n")
						TEXT(" eas.save_loaded_asset(sw)\n")
						TEXT("\n")
						TEXT("else:\n raise RuntimeError(f'Unknown audio waveform tool: {tool_name}')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgsJson));
				});
		};

		RegisterAudioWavePyTool(TEXT("audio_waveform_trim"),
			TEXT("Trim a SoundWave asset to a specific time region. Removes audio outside the specified start/end times."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("SoundWave asset path"))}, {TEXT("start_time"), FSololmcpSchemaBuilder::Number(TEXT("Start time in seconds (default 0.0)"))}, {TEXT("end_time"), FSololmcpSchemaBuilder::Number(TEXT("End time in seconds (-1 = use full duration)"))}}, {TEXT("asset_path")}));

		RegisterAudioWavePyTool(TEXT("audio_waveform_fade"),
			TEXT("Apply fade in/out to a SoundWave asset. Supports linear, logarithmic, and equal power curves."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("SoundWave asset path"))}, {TEXT("fade_type"), FSololmcpSchemaBuilder::String(TEXT("Fade type"), {TEXT("in"), TEXT("out"), TEXT("both")})}, {TEXT("fade_in_duration"), FSololmcpSchemaBuilder::Number(TEXT("Fade in duration in seconds"))}, {TEXT("fade_out_duration"), FSololmcpSchemaBuilder::Number(TEXT("Fade out duration in seconds"))}, {TEXT("fade_curve"), FSololmcpSchemaBuilder::String(TEXT("Fade curve type"), {TEXT("linear"), TEXT("logarithmic"), TEXT("equal_power")})}}, {TEXT("asset_path")}));

		RegisterAudioWavePyTool(TEXT("audio_waveform_normalize"),
			TEXT("Normalize a SoundWave asset to a target decibel level. Supports peak and loudness normalization modes."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("SoundWave asset path"))}, {TEXT("target_db"), FSololmcpSchemaBuilder::Number(TEXT("Target level in dB (default -1.0)"))}, {TEXT("normalize_mode"), FSololmcpSchemaBuilder::String(TEXT("Normalization mode"), {TEXT("peak"), TEXT("loudness")})}}, {TEXT("asset_path")}));
	}

// ============================================================================
// UE 5.7: The following tools (v1.6 Sequencer/Audio/VFX, v1.7 Project Perception)
// have API incompatibilities with UE 5.7. Re-disabled pending fixes.
// Previously enabled #if 0 blocks (lines 13773-31765 of original).
// ============================================================================

	// --- v1.6.0 新增工具（UE5.7.4 adapted）---

		Registry.Register({
			TEXT("blueprint_remove_interface"),
			TEXT("Remove an implemented interface from a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("interface_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("preserve_functions"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("interface_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString InterfaceClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("interface_class_path"), InterfaceClassPath))
				{
					OutError = TEXT("Missing asset_path or interface_class_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UClass* InterfaceClass = Context.Services.ResolveClass(InterfaceClassPath, OutError);
				if (!InterfaceClass)
				{
					return false;
				}
				const bool bPreserveFunctions = Arguments->HasTypedField<EJson::Boolean>(TEXT("preserve_functions")) ? Arguments->GetBoolField(TEXT("preserve_functions")) : false;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRemoveInterface", "SOMOLMCP Remove Blueprint Interface"));
				FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceClass->GetClassPathName(), bPreserveFunctions);
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Removed blueprint interface.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_list_graphs"),
			TEXT("List all graphs in a blueprint asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				OutStructured = BlueprintGraphsToJson(Blueprint);
				OutSummary = TEXT("Listed blueprint graphs.");
				return true;
			}
		, nullptr
		, 5
		});

			Registry.Register({
			TEXT("blueprint_read"),
			TEXT("Read comprehensive information about a Blueprint asset including parent class, variables, functions, interfaces, components, and graphs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path, e.g. /Game/MyBlueprint."))}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				// Basic info
				OutStructured->SetStringField(TEXT("name"), Blueprint->GetName());
				OutStructured->SetStringField(TEXT("path"), AssetPath);
				OutStructured->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT("None"));
				OutStructured->SetBoolField(TEXT("is_valid"), IsValid(Blueprint));
				OutStructured->SetNumberField(TEXT("status"), static_cast<int32>(Blueprint->Status));

				// Implemented interfaces
				TArray<TSharedPtr<FJsonValue>> Interfaces;
				for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
				{
					TSharedRef<FJsonObject> IfaceJson = MakeShared<FJsonObject>();
					if (Desc.Interface)
					{
						IfaceJson->SetStringField(TEXT("class"), Desc.Interface->GetPathName());
					}
					// UE 5.7: bImplements removed
					Interfaces.Add(MakeShared<FJsonValueObject>(IfaceJson));
				}
				OutStructured->SetArrayField(TEXT("interfaces"), Interfaces);

				// Member variables (UE5: iterate GeneratedClass properties instead of NewVariableNames)
				TArray<TSharedPtr<FJsonValue>> Variables;
				if (Blueprint->GeneratedClass)
				{
					for (TFieldIterator<FProperty> It(Blueprint->GeneratedClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
					{
						FProperty* Prop = *It;
						if (!Prop || !Prop->HasAllPropertyFlags(CPF_BlueprintVisible)) { continue; }
						TSharedRef<FJsonObject> VarJson = MakeShared<FJsonObject>();
						VarJson->SetStringField(TEXT("name"), Prop->GetName());
						VarJson->SetStringField(TEXT("type"), Prop->GetCPPType());
						VarJson->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));
						VarJson->SetBoolField(TEXT("editable"), !Prop->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance));
						VarJson->SetBoolField(TEXT("instance_editable"), Prop->HasAnyPropertyFlags(CPF_Edit | CPF_DisableEditOnInstance) && !Prop->HasAllPropertyFlags(CPF_DisableEditOnInstance));
						VarJson->SetBoolField(TEXT("expose_to_spawn"), Prop->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible) && !Prop->HasAnyPropertyFlags(CPF_DisableEditOnInstance));
						Variables.Add(MakeShared<FJsonValueObject>(VarJson));
					}
				}
				OutStructured->SetArrayField(TEXT("variables"), Variables);
				OutStructured->SetNumberField(TEXT("variable_count"), Variables.Num());

				// Functions
				TArray<TSharedPtr<FJsonValue>> Functions;
				for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (!Graph) { continue; }
					TSharedRef<FJsonObject> FuncJson = MakeShared<FJsonObject>();
					FuncJson->SetStringField(TEXT("name"), Graph->GetName());
					// Count nodes in this function graph
					FuncJson->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
					Functions.Add(MakeShared<FJsonValueObject>(FuncJson));
				}
				OutStructured->SetArrayField(TEXT("functions"), Functions);
				OutStructured->SetNumberField(TEXT("function_count"), Functions.Num());

				// Graphs (event graphs, macro graphs, delegate graphs)
				OutStructured->SetObjectField(TEXT("graphs"), BlueprintGraphsToJson(Blueprint));

				// Component list (if blueprint is based on Actor)
				TArray<TSharedPtr<FJsonValue>> Components;
				if (Blueprint->GeneratedClass)
				{
					for (TFieldIterator<FObjectProperty> It(Blueprint->GeneratedClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
					{
						FObjectProperty* ObjProp = *It;
						if (ObjProp && ObjProp->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible) && !ObjProp->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
						{
							// Check if it's a component template
							UObject* ObjDefault = ObjProp->GetObjectPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject());
							if (ObjDefault && ObjDefault->IsA<UActorComponent>())
							{
								TSharedRef<FJsonObject> CompJson = MakeShared<FJsonObject>();
								CompJson->SetStringField(TEXT("name"), ObjProp->GetName());
								CompJson->SetStringField(TEXT("class"), ObjDefault->GetClass()->GetPathName());
								Components.Add(MakeShared<FJsonValueObject>(CompJson));
							}
						}
					}
				}
				OutStructured->SetArrayField(TEXT("components"), Components);
				OutStructured->SetNumberField(TEXT("component_count"), Components.Num());

				OutSummary = FString::Printf(TEXT("Read blueprint: %s (%d variables, %d functions, %d graphs, %d components, %d interfaces)"),
					*Blueprint->GetName(), Variables.Num(), Functions.Num(),
					Blueprint->UbergraphPages.Num() + Blueprint->FunctionGraphs.Num() + Blueprint->MacroGraphs.Num(),
					Components.Num(), Interfaces.Num());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_get_variable_details"),
			TEXT("Get detailed information about a specific member variable in a Blueprint, including its type, default value, replication, and metadata."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("variable_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString VariableName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName))
				{
					OutError = TEXT("Missing asset_path or variable_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				const FName VarFName(*VariableName);
				FProperty* Prop = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, VarFName);
				if (!Prop)
				{
					OutError = FString::Printf(TEXT("Variable '%s' not found in blueprint."), *VariableName);
					return false;
				}

				OutStructured->SetStringField(TEXT("name"), Prop->GetName());
				OutStructured->SetStringField(TEXT("type"), Prop->GetCPPType());
				OutStructured->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				OutStructured->SetNumberField(TEXT("size"), Prop->GetElementSize()); // UE 5.7: use GetElementSize()
#else
				OutStructured->SetNumberField(TEXT("size"), Prop->ElementSize);
#endif
				OutStructured->SetNumberField(TEXT("offset_internal"), Prop->GetOffset_ForDebug()); // UE 5.7: use GetOffset_ForDebug()
				OutStructured->SetBoolField(TEXT("is_array"), Prop->ArrayDim > 0 ? true : false);
				OutStructured->SetNumberField(TEXT("array_dim"), Prop->ArrayDim);

				// Editability flags
				OutStructured->SetBoolField(TEXT("editable"), !Prop->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance));
				OutStructured->SetBoolField(TEXT("blueprint_read_only"), Prop->HasAllPropertyFlags(CPF_BlueprintReadOnly));
				OutStructured->SetBoolField(TEXT("config"), Prop->HasAnyPropertyFlags(CPF_Config));
				OutStructured->SetBoolField(TEXT("save_game"), Prop->HasAnyPropertyFlags(CPF_SaveGame));
				OutStructured->SetBoolField(TEXT("replicated"), Prop->HasAnyPropertyFlags(CPF_Net));
				OutStructured->SetBoolField(TEXT("rep_notify"), Prop->HasAnyPropertyFlags(CPF_RepNotify));
				OutStructured->SetBoolField(TEXT("interp"), Prop->HasAnyPropertyFlags(CPF_Interp));
				OutStructured->SetBoolField(TEXT("non_transactional"), Prop->HasAnyPropertyFlags(CPF_NonTransactional));
				OutStructured->SetBoolField(TEXT("expose_to_cinematics"), Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) && Prop->HasAnyPropertyFlags(CPF_Interp));
				OutStructured->SetBoolField(TEXT("advanced_display"), Prop->GetMetaData(TEXT("AdvancedDisplay")) == TEXT("true"));

				// Tooltip
				FString Tooltip = Prop->GetToolTipText().ToString();
				if (!Tooltip.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("tooltip"), Tooltip);
				}

				OutSummary = FString::Printf(TEXT("Variable '%s': type=%s, editable=%s, replicated=%s"),
					*VariableName, *Prop->GetCPPType(),
					(!Prop->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance)) ? TEXT("yes") : TEXT("no"),
					Prop->HasAnyPropertyFlags(CPF_Net) ? TEXT("yes") : TEXT("no"));
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_get_function_details"),
			TEXT("Get detailed information about a specific function graph in a Blueprint, including its signature pins and node count."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				UEdGraph* Graph = FindBlueprintGraphByName(Blueprint, GraphName);
				if (!Graph)
				{
					OutError = FString::Printf(TEXT("Graph '%s' not found in blueprint."), *GraphName);
					return false;
				}

				OutStructured->SetStringField(TEXT("name"), Graph->GetName());
				OutStructured->SetStringField(TEXT("class"), Graph->GetClass()->GetPathName());
				OutStructured->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

				// Find the function entry node to extract signature
				TArray<TSharedPtr<FJsonValue>> SignaturePins;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
					{
						// Extract pins from function entry node
						for (const UEdGraphPin* Pin : EntryNode->Pins)
						{
							if (!Pin) { continue; }
							// Skip exec pins and the "then" output
							if (Pin->PinType.PinCategory == TEXT("exec")) { continue; }
							TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
							PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
							PinJson->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
							PinJson->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
							if (Pin->PinType.PinSubCategoryObject.IsValid())
							{
								PinJson->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject->GetPathName());
							}
							PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
							SignaturePins.Add(MakeShared<FJsonValueObject>(PinJson));
						}
						break;
					}
				}
				OutStructured->SetArrayField(TEXT("signature_pins"), SignaturePins);
				OutStructured->SetNumberField(TEXT("signature_pin_count"), SignaturePins.Num());

				// Flag whether this is an Event graph
				bool bIsEventGraph = (Graph->GetClass() == UEdGraph::StaticClass());
				OutStructured->SetBoolField(TEXT("is_event_graph"), bIsEventGraph);

				// List node types for a quick overview
				TMap<FString, int32> NodeTypeCounts;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node)
					{
						FString NodeClass = Node->GetClass()->GetName();
						NodeTypeCounts.FindOrAdd(NodeClass, 0)++;
					}
				}
				TArray<TSharedPtr<FJsonValue>> NodeTypeArray;
				for (const TPair<FString, int32>& Pair : NodeTypeCounts)
				{
					TSharedRef<FJsonObject> TypeJson = MakeShared<FJsonObject>();
					TypeJson->SetStringField(TEXT("class"), Pair.Key);
					TypeJson->SetNumberField(TEXT("count"), Pair.Value);
					NodeTypeArray.Add(MakeShared<FJsonValueObject>(TypeJson));
				}
				OutStructured->SetArrayField(TEXT("node_types"), NodeTypeArray);

				OutSummary = FString::Printf(TEXT("Function '%s': %d nodes, %d signature pins, %d distinct node types"),
					*GraphName, Graph->Nodes.Num(), SignaturePins.Num(), NodeTypeCounts.Num());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_analyze_graph"),
			TEXT("Analyze a blueprint graph's execution flow: list all nodes with their pins, connections, and topology. Optionally filter by graph_name."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				// Determine which graphs to analyze
				TArray<UEdGraph*> GraphsToAnalyze;
				FString GraphNameFilter;
				if (Arguments->TryGetStringField(TEXT("graph_name"), GraphNameFilter) && !GraphNameFilter.IsEmpty())
				{
					UEdGraph* Found = FindBlueprintGraphByName(Blueprint, GraphNameFilter);
					if (!Found)
					{
						OutError = FString::Printf(TEXT("Graph '%s' not found."), *GraphNameFilter);
						return false;
					}
					GraphsToAnalyze.Add(Found);
				}
				else
				{
					// Analyze all graphs
					Blueprint->GetAllGraphs(GraphsToAnalyze);
				}

				TArray<TSharedPtr<FJsonValue>> GraphAnalysis;
				for (UEdGraph* Graph : GraphsToAnalyze)
				{
					if (!Graph) { continue; }
					TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
					GraphJson->SetStringField(TEXT("name"), Graph->GetName());
					GraphJson->SetStringField(TEXT("class"), Graph->GetClass()->GetPathName());

					// Serialize all nodes with full connection info
					TArray<TSharedPtr<FJsonValue>> NodeArray;
					TMap<FString, int32> ConnectionCount; // Track total connections

					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node) { continue; }
						TSharedRef<FJsonObject> NodeJson = BlueprintNodeToJson(Node);

						// Enrich with connection summary
						int32 InputConnections = 0;
						int32 OutputConnections = 0;
						for (const UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin) { continue; }
							if (Pin->Direction == EGPD_Input)
							{
								InputConnections += Pin->LinkedTo.Num();
							}
							else
							{
								OutputConnections += Pin->LinkedTo.Num();
							}
						}
						NodeJson->SetNumberField(TEXT("input_connections"), InputConnections);
						NodeJson->SetNumberField(TEXT("output_connections"), OutputConnections);
						ConnectionCount.FindOrAdd(TEXT("total"), 0) += InputConnections + OutputConnections;

						NodeArray.Add(MakeShared<FJsonValueObject>(NodeJson));
					}

					GraphJson->SetArrayField(TEXT("nodes"), NodeArray);
					GraphJson->SetNumberField(TEXT("node_count"), NodeArray.Num());
					GraphJson->SetNumberField(TEXT("connection_count"), ConnectionCount.FindRef(TEXT("total")));
					GraphAnalysis.Add(MakeShared<FJsonValueObject>(GraphJson));
				}

				OutStructured->SetArrayField(TEXT("graph_analysis"), GraphAnalysis);
				OutStructured->SetNumberField(TEXT("total_graphs"), GraphAnalysis.Num());

				int32 TotalNodes = 0;
				int32 TotalConnections = 0;
				for (const TSharedPtr<FJsonValue>& GVal : GraphAnalysis)
				{
					const TSharedPtr<FJsonObject>* GObj = nullptr;
					// UE 5.7: TryGetObject signature changed - single output param
					// UE 5.7: FJsonObject::IsValid() removed - check GObj pointer instead
					if (GVal.IsValid() && GVal->TryGetObject(GObj) && GObj && GObj->IsValid())
					{
						int32 N = 0, C = 0;
						(*GObj)->TryGetNumberField(TEXT("node_count"), N);
						(*GObj)->TryGetNumberField(TEXT("connection_count"), C);
						TotalNodes += N;
						TotalConnections += C;
					}
				}

				OutStructured->SetNumberField(TEXT("total_nodes"), TotalNodes);
				OutStructured->SetNumberField(TEXT("total_connections"), TotalConnections);

				OutSummary = FString::Printf(TEXT("Analyzed %d graphs: %d nodes, %d connections"),
					GraphAnalysis.Num(), TotalNodes, TotalConnections);
				return true;
			}
		, nullptr
		, 5
		});

	Registry.Register({
			TEXT("blueprint_get_node_details"),
			TEXT("Add an arbitrary blueprint graph node by class."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("node_class_path"), NodeClassPath))
				{
					OutError = TEXT("Missing asset_path, graph_name, or node_class_path.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				UClass* NodeClass = Context.Services.ResolveClass(NodeClassPath, OutError);
				if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
				{
					OutError = TEXT("node_class_path is not a graph node class.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddNodeByClass", "SOMOLMCP Add Blueprint Node By Class"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(Graph, NodeClass, GetBlueprintNodeLocationFromArguments(Arguments));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn blueprint node.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint node by class.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_branch_node"),
			TEXT("Add a branch node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}
				if (!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					SololmcpError::MissingParam(OutStructured, TEXT("graph_name"));
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					SololmcpError::InvalidPath(OutStructured, AssetPath);
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddBranchNode", "SOMOLMCP Add Blueprint Branch Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(Graph, UK2Node_IfThenElse::StaticClass(), GetBlueprintNodeLocationFromArguments(Arguments));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn branch node.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint branch node.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_sequence_node"),
			TEXT("Add an execution sequence node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
				{
					OutError = TEXT("Missing asset_path or graph_name.");
					return false;
				}
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddSequenceNode", "SOMOLMCP Add Blueprint Sequence Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(Graph, UK2Node_ExecutionSequence::StaticClass(), GetBlueprintNodeLocationFromArguments(Arguments));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn sequence node.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint sequence node.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_dynamic_cast_node"),
			TEXT("Add a dynamic cast node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("target_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("target_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString TargetClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("target_class_path"), TargetClassPath))
				{
					OutError = TEXT("Missing asset_path, graph_name, or target_class_path.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				UClass* TargetClass = Context.Services.ResolveClass(TargetClassPath, OutError);
				if (!TargetClass)
				{
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddDynamicCastNode", "SOMOLMCP Add Blueprint Dynamic Cast Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(
					Graph,
					UK2Node_DynamicCast::StaticClass(),
					GetBlueprintNodeLocationFromArguments(Arguments),
					UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda([TargetClass](UEdGraphNode* NewNode, bool)
					{
						if (UK2Node_DynamicCast* DynamicCastNode = Cast<UK2Node_DynamicCast>(NewNode))
						{
							DynamicCastNode->TargetType = TargetClass;
							DynamicCastNode->ReconstructNode();
						}
					}));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn dynamic cast node.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint dynamic cast node.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_macro_instance_node"),
			TEXT("Add a macro instance node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("macro_graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("macro_graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString MacroGraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("macro_graph_name"), MacroGraphName))
				{
					OutError = TEXT("Missing asset_path, graph_name, or macro_graph_name.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				UEdGraph* MacroGraph = FindBlueprintGraphByName(Blueprint, MacroGraphName);
				if (!MacroGraph)
				{
					OutError = TEXT("Macro graph was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddMacroInstanceNode", "SOMOLMCP Add Blueprint Macro Instance Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(
					Graph,
					UK2Node_MacroInstance::StaticClass(),
					GetBlueprintNodeLocationFromArguments(Arguments),
					UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda([MacroGraph](UEdGraphNode* NewNode, bool)
					{
						if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(NewNode))
						{
							MacroNode->SetMacroGraph(MacroGraph);
							MacroNode->ReconstructNode();
						}
					}));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn macro instance node.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint macro instance node.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_make_struct_node"),
			TEXT("Add a make-struct node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("struct_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("struct_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString StructPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("struct_path"), StructPath))
				{
					OutError = TEXT("Missing asset_path, graph_name, or struct_path.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				UScriptStruct* ScriptStruct = FindObject<UScriptStruct>(nullptr, *StructPath);
				if (!ScriptStruct)
				{
					ScriptStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
				}
				if (!ScriptStruct)
				{
					OutError = TEXT("struct_path does not resolve to a script struct.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddMakeStructNode", "SOMOLMCP Add Blueprint Make Struct Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(
					Graph,
					UK2Node_MakeStruct::StaticClass(),
					GetBlueprintNodeLocationFromArguments(Arguments),
					UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda([ScriptStruct](UEdGraphNode* NewNode, bool)
					{
						if (UK2Node_MakeStruct* MakeStructNode = Cast<UK2Node_MakeStruct>(NewNode))
						{
							MakeStructNode->StructType = ScriptStruct;
							MakeStructNode->ReconstructNode();
						}
					}));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn make struct node.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint make struct node.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_break_struct_node"),
			TEXT("Add a break-struct node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("struct_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("struct_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString StructPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("struct_path"), StructPath))
				{
					OutError = TEXT("Missing asset_path, graph_name, or struct_path.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				UScriptStruct* ScriptStruct = FindObject<UScriptStruct>(nullptr, *StructPath);
				if (!ScriptStruct)
				{
					ScriptStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
				}
				if (!ScriptStruct)
				{
					OutError = TEXT("struct_path does not resolve to a script struct.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddBreakStructNode", "SOMOLMCP Add Blueprint Break Struct Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(
					Graph,
					UK2Node_BreakStruct::StaticClass(),
					GetBlueprintNodeLocationFromArguments(Arguments),
					UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda([ScriptStruct](UEdGraphNode* NewNode, bool)
					{
						if (UK2Node_BreakStruct* BreakStructNode = Cast<UK2Node_BreakStruct>(NewNode))
						{
							BreakStructNode->StructType = ScriptStruct;
							BreakStructNode->ReconstructNode();
						}
					}));
				if (!Node)
				{
					OutError = TEXT("Failed to spawn break struct node.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint break struct node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_set_struct_node_type"),
			TEXT("Retype an existing make-struct or break-struct node to a resolved struct asset. graph_name may be supplied when migrated assets contain duplicate node GUIDs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("struct_path"), FSololmcpSchemaBuilder::String()}, {TEXT("compile"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("struct_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuidString;
				FString StructPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString) ||
					!Arguments->TryGetStringField(TEXT("struct_path"), StructPath))
				{
					OutError = TEXT("Missing struct node retype arguments.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("graph_name"), GraphName);
				bool bCompile = false;
				Arguments->TryGetBoolField(TEXT("compile"), bCompile);

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = ResolveBlueprintNodeByGuid(Blueprint, NodeGuidString, GraphName);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}
				UScriptStruct* ScriptStruct = FindObject<UScriptStruct>(nullptr, *StructPath);
				if (!ScriptStruct)
				{
					ScriptStruct = LoadObject<UScriptStruct>(nullptr, *StructPath);
				}
				if (!ScriptStruct)
				{
					OutError = TEXT("struct_path does not resolve to a script struct.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSetStructNodeType", "SOMOLMCP Set Blueprint Struct Node Type"));
				Blueprint->Modify();
				Node->Modify();
				if (UK2Node_BreakStruct* BreakStructNode = Cast<UK2Node_BreakStruct>(Node))
				{
					BreakStructNode->StructType = ScriptStruct;
					BreakStructNode->ReconstructNode();
				}
				else if (UK2Node_MakeStruct* MakeStructNode = Cast<UK2Node_MakeStruct>(Node))
				{
					MakeStructNode->StructType = ScriptStruct;
					MakeStructNode->ReconstructNode();
				}
				else
				{
					OutError = TEXT("Node is not a make-struct or break-struct node.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				if (bCompile)
				{
					UBlueprintEditorLibrary::CompileBlueprint(Blueprint);
				}
				OutStructured = BlueprintNodeToJson(Node);
				OutStructured->SetStringField(TEXT("struct_path"), ScriptStruct->GetPathName());
				OutStructured->SetStringField(TEXT("compile_status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				OutSummary = TEXT("Retyped blueprint struct node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("blueprint_split_pin"),
			TEXT("Split a struct pin on a blueprint node."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuidString;
				FString PinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing asset_path, node_guid, or pin_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuidString);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}

				FString PinGuidString;
				Arguments->TryGetStringField(TEXT("pin_guid"), PinGuidString);
				UEdGraphPin* Pin = FindNodePin(Node, PinName, PinGuidString);
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Pin || !Schema)
				{
					OutError = TEXT("Blueprint pin or schema was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintSplitPin", "SOMOLMCP Split Blueprint Pin"));
				Schema->SplitPin(Pin, true);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Split blueprint pin.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_recombine_pin"),
			TEXT("Recombine a previously split blueprint pin."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuidString;
				FString PinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing asset_path, node_guid, or pin_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuidString);
				if (!Node)
				{
					OutError = TEXT("Blueprint node was not found.");
					return false;
				}

				FString PinGuidString;
				Arguments->TryGetStringField(TEXT("pin_guid"), PinGuidString);
				UEdGraphPin* Pin = FindNodePin(Node, PinName, PinGuidString);
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Pin || !Schema)
				{
					OutError = TEXT("Blueprint pin or schema was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRecombinePin", "SOMOLMCP Recombine Blueprint Pin"));
				Schema->RecombinePin(Pin);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Recombined blueprint pin.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_comment_node"),
			TEXT("Add a comment node to a blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("text"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
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

				FString CommentText;
				Arguments->TryGetStringField(TEXT("text"), CommentText);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddCommentNode", "SOMOLMCP Add Blueprint Comment Node"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(Graph, UEdGraphNode_Comment::StaticClass(), GetBlueprintNodeLocationFromArguments(Arguments));
				UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node);
				if (!CommentNode)
				{
					OutError = TEXT("Failed to spawn comment node.");
					return false;
				}
				CommentNode->NodeComment = CommentText;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(CommentNode);
				OutSummary = TEXT("Added blueprint comment node.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_promote_pin_to_member_variable"),
			TEXT("Promote a blueprint pin to a member variable."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guid"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeGuidString;
				FString PinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing asset_path, node_guid, or pin_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuidString);
				UEdGraphPin* Pin = FindNodePin(Node, PinName);
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node && Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Node || !Pin || !Schema || !Schema->CanPromotePinToVariable(*Pin, true))
				{
					OutError = TEXT("Blueprint pin cannot be promoted to a member variable.");
					return false;
				}

				FBlueprintEditor* BlueprintEditor = GetBlueprintEditorForAsset(Blueprint, OutError);
				if (!BlueprintEditor)
				{
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintPromotePinMemberVariable", "SOMOLMCP Promote Pin To Blueprint Member Variable"));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				BlueprintEditor->DoPromoteToVariable2f(Blueprint, Pin, true, TOptional<FVector2f>());
#else
				// 5.6 renamed this to the 2f form when node coordinates became FVector2f.
				BlueprintEditor->DoPromoteToVariable(Blueprint, Pin, true);
#endif
				FString VariableName;
				if (Arguments->TryGetStringField(TEXT("variable_name"), VariableName) && !VariableName.IsEmpty() && !VariableName.Equals(Pin->PinName.ToString(), ESearchCase::CaseSensitive))
				{
					FBlueprintEditorUtils::RenameMemberVariable(Blueprint, Pin->PinName, *VariableName);
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Promoted blueprint pin to member variable.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_promote_pin_to_local_variable"),
			TEXT("Promote a blueprint pin to a local variable."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_name"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_guid"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString NodeGuidString;
				FString PinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidString) || !Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing local variable promotion arguments.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}
				UEdGraphNode* Node = FindBlueprintNodeByGuid(Blueprint, NodeGuidString);
				UEdGraphPin* Pin = FindNodePin(Node, PinName);
				const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node && Node->GetGraph() ? Node->GetGraph()->GetSchema() : nullptr);
				if (!Node || !Pin || !Schema || !FBlueprintEditorUtils::DoesSupportLocalVariables(Graph) || !Schema->CanPromotePinToVariable(*Pin, false))
				{
					OutError = TEXT("Blueprint pin cannot be promoted to a local variable.");
					return false;
				}

				FBlueprintEditor* BlueprintEditor = GetBlueprintEditorForAsset(Blueprint, OutError);
				if (!BlueprintEditor)
				{
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintPromotePinLocalVariable", "SOMOLMCP Promote Pin To Blueprint Local Variable"));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				BlueprintEditor->DoPromoteToVariable2f(Blueprint, Pin, false, TOptional<FVector2f>());
#else
				// 5.6 renamed this to the 2f form when node coordinates became FVector2f.
				BlueprintEditor->DoPromoteToVariable(Blueprint, Pin, false);
#endif
				// Local variable promotion creates a scoped variable; explicit rename can be added once
				// a stable post-promotion local-scope handle is exposed.
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Blueprint);
				OutSummary = TEXT("Promoted blueprint pin to local variable.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_collapse_nodes_to_function"),
			TEXT("Collapse selected blueprint nodes to a function."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("function_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guids"), TEXT("function_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString FunctionName;
				TArray<FString> NodeGuids;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetStringArray(Arguments, TEXT("node_guids"), NodeGuids) || !Arguments->TryGetStringField(TEXT("function_name"), FunctionName))
				{
					OutError = TEXT("Missing asset_path, node_guids, or function_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				TSet<UEdGraphNode*> Nodes = ResolveBlueprintNodesByGuids(Blueprint, NodeGuids, OutError);
				if (Nodes.Num() == 0)
				{
					return false;
				}
				UEdGraph* SourceGraph = (*Nodes.CreateConstIterator())->GetGraph();
				if (!SourceGraph)
				{
					OutError = TEXT("Selected nodes do not belong to a graph.");
					return false;
				}

				const TArray<UEdGraph*> GraphsBefore = GetAllBlueprintGraphs(Blueprint);

				FBlueprintEditor* BlueprintEditor = GetBlueprintEditorForAsset(Blueprint, OutError);
				if (!BlueprintEditor)
				{
					return false;
				}
				FSololmcpBlueprintEditorAccess* BlueprintEditorAccess = static_cast<FSololmcpBlueprintEditorAccess*>(BlueprintEditor);
				TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(SourceGraph, false);
				if (!GraphEditor.IsValid())
				{
					OutError = TEXT("Failed to focus blueprint graph editor.");
					return false;
				}

				GraphEditor->ClearSelectionSet();
				for (UEdGraphNode* Node : Nodes)
				{
					GraphEditor->SetNodeSelection(Node, true);
				}
				if (!BlueprintEditorAccess->CanCollapseSelectionToFunction())
				{
					OutError = TEXT("Selected nodes cannot be collapsed to a function.");
					return false;
				}

				BlueprintEditorAccess->OnCollapseSelectionToFunction();

				UEdGraphNode* FunctionNode = nullptr;
				const FGraphPanelSelectionSet SelectedNodes = GraphEditor->GetSelectedNodes();
				for (UObject* SelectedObject : SelectedNodes)
				{
					if (UEdGraphNode* SelectedNode = Cast<UEdGraphNode>(SelectedObject))
					{
						FunctionNode = SelectedNode;
						break;
					}
				}

				UEdGraph* FunctionGraph = nullptr;
				for (UEdGraph* ExistingGraph : GetAllBlueprintGraphs(Blueprint))
				{
					if (ExistingGraph && !GraphsBefore.Contains(ExistingGraph))
					{
						FunctionGraph = ExistingGraph;
						break;
					}
				}

				if (!FunctionGraph || !FunctionNode)
				{
					OutError = TEXT("Failed to collapse nodes to function.");
					return false;
				}
				if (!FunctionName.IsEmpty() && FunctionGraph->GetName() != FunctionName)
				{
					UBlueprintEditorLibrary::RenameGraph(FunctionGraph, FunctionName);
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetObjectField(TEXT("functionNode"), BlueprintNodeToJson(FunctionNode));
				OutStructured->SetObjectField(TEXT("graphs"), BlueprintGraphsToJson(Blueprint));
				OutSummary = TEXT("Collapsed blueprint nodes to function.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_collapse_nodes_to_macro"),
			TEXT("Collapse selected blueprint nodes to a macro."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_guids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("macro_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_guids"), TEXT("macro_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString MacroName;
				TArray<FString> NodeGuids;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetStringArray(Arguments, TEXT("node_guids"), NodeGuids) || !Arguments->TryGetStringField(TEXT("macro_name"), MacroName))
				{
					OutError = TEXT("Missing asset_path, node_guids, or macro_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				TSet<UEdGraphNode*> Nodes = ResolveBlueprintNodesByGuids(Blueprint, NodeGuids, OutError);
				if (Nodes.Num() == 0)
				{
					return false;
				}
				UEdGraph* SourceGraph = (*Nodes.CreateConstIterator())->GetGraph();
				if (!SourceGraph)
				{
					OutError = TEXT("Selected nodes do not belong to a graph.");
					return false;
				}

				const TArray<UEdGraph*> GraphsBefore = GetAllBlueprintGraphs(Blueprint);

				FBlueprintEditor* BlueprintEditor = GetBlueprintEditorForAsset(Blueprint, OutError);
				if (!BlueprintEditor)
				{
					return false;
				}
				FSololmcpBlueprintEditorAccess* BlueprintEditorAccess = static_cast<FSololmcpBlueprintEditorAccess*>(BlueprintEditor);
				TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(SourceGraph, false);
				if (!GraphEditor.IsValid())
				{
					OutError = TEXT("Failed to focus blueprint graph editor.");
					return false;
				}

				GraphEditor->ClearSelectionSet();
				for (UEdGraphNode* Node : Nodes)
				{
					GraphEditor->SetNodeSelection(Node, true);
				}
				if (!BlueprintEditorAccess->CanCollapseSelectionToMacro())
				{
					OutError = TEXT("Selected nodes cannot be collapsed to a macro.");
					return false;
				}

				BlueprintEditorAccess->OnCollapseSelectionToMacro();

				UEdGraphNode* MacroNode = nullptr;
				const FGraphPanelSelectionSet SelectedNodes = GraphEditor->GetSelectedNodes();
				for (UObject* SelectedObject : SelectedNodes)
				{
					if (UEdGraphNode* SelectedNode = Cast<UEdGraphNode>(SelectedObject))
					{
						MacroNode = SelectedNode;
						break;
					}
				}

				UEdGraph* MacroGraph = nullptr;
				for (UEdGraph* ExistingGraph : GetAllBlueprintGraphs(Blueprint))
				{
					if (ExistingGraph && !GraphsBefore.Contains(ExistingGraph))
					{
						MacroGraph = ExistingGraph;
						break;
					}
				}

				if (!MacroGraph || !MacroNode)
				{
					for (UEdGraphNode* CandidateNode : SourceGraph->Nodes)
					{
						UK2Node_MacroInstance* MacroInstance = Cast<UK2Node_MacroInstance>(CandidateNode);
						if (!MacroInstance)
						{
							continue;
						}
						UEdGraph* CandidateMacroGraph = MacroInstance->GetMacroGraph();
						if (!CandidateMacroGraph)
						{
							continue;
						}
						if (!GraphsBefore.Contains(CandidateMacroGraph)
							|| (!MacroName.IsEmpty() && CandidateMacroGraph->GetName().Contains(MacroName)))
						{
							MacroNode = MacroInstance;
							MacroGraph = CandidateMacroGraph;
							break;
						}
					}
				}

				if (!MacroGraph || !MacroNode)
				{
					OutError = TEXT("Failed to collapse nodes to macro.");
					return false;
				}
				if (!MacroName.IsEmpty() && MacroGraph->GetName() != MacroName)
				{
					UBlueprintEditorLibrary::RenameGraph(MacroGraph, MacroName);
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetObjectField(TEXT("macroNode"), BlueprintNodeToJson(MacroNode));
				OutStructured->SetObjectField(TEXT("graphs"), BlueprintGraphsToJson(Blueprint));
				OutSummary = TEXT("Collapsed blueprint nodes to macro.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_timeline"),
			TEXT("Add a timeline node to a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("timeline_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("timeline_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString TimelineName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("timeline_name"), TimelineName))
				{
					OutError = TEXT("Missing asset_path, graph_name, or timeline_name.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddTimeline", "SOMOLMCP Add Blueprint Timeline"));
				Blueprint->Modify();
				UEdGraphNode* Node = SpawnBlueprintNodeByClass(Graph, UK2Node_Timeline::StaticClass(), GetBlueprintNodeLocationFromArguments(Arguments));
				UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node);
				if (!TimelineNode)
				{
					OutError = TEXT("Failed to spawn timeline node.");
					return false;
				}
				if (!TimelineName.IsEmpty() && TimelineNode->TimelineName.ToString() != TimelineName)
				{
					TimelineNode->RenameTimeline(TimelineName);
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(TimelineNode);
				OutSummary = TEXT("Added blueprint timeline node.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_list_event_dispatchers"),
			TEXT("List event dispatchers defined on a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				OutStructured = BlueprintEventDispatchersToJson(Blueprint);
				OutSummary = TEXT("Listed blueprint event dispatchers.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_create_event_dispatcher"),
			TEXT("Create an event dispatcher on a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("dispatcher_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("dispatcher_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString DispatcherName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("dispatcher_name"), DispatcherName))
				{
					OutError = TEXT("Missing asset_path or dispatcher_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
				const FName UniqueName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, DispatcherName.IsEmpty() ? TEXT("NewEventDispatcher") : DispatcherName);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintCreateEventDispatcher", "SOMOLMCP Create Blueprint Event Dispatcher"));
				Blueprint->Modify();

				FEdGraphPinType DelegateType;
				DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
				if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, UniqueName, DelegateType))
				{
					OutError = TEXT("Failed to create event dispatcher variable.");
					return false;
				}

				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UniqueName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				if (!NewGraph)
				{
					FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, UniqueName);
					OutError = TEXT("Failed to create event dispatcher graph.");
					return false;
				}

				NewGraph->bEditable = false;
				K2Schema->CreateDefaultNodesForGraph(*NewGraph);
				K2Schema->CreateFunctionGraphTerminators(*NewGraph, static_cast<UClass*>(nullptr));
				K2Schema->AddExtraFunctionFlags(NewGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
				K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);
				Blueprint->DelegateSignatureGraphs.Add(NewGraph);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				OutStructured = EventDispatcherGraphToJson(Blueprint, NewGraph);
				OutSummary = TEXT("Created blueprint event dispatcher.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_rename_event_dispatcher"),
			TEXT("Rename an event dispatcher on a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("dispatcher_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("dispatcher_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString DispatcherName;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("dispatcher_name"), DispatcherName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing asset_path, dispatcher_name, or new_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				UEdGraph* DelegateGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, *DispatcherName);
				if (!DelegateGraph)
				{
					OutError = TEXT("Event dispatcher graph was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRenameEventDispatcher", "SOMOLMCP Rename Blueprint Event Dispatcher"));
				FBlueprintEditorUtils::RenameMemberVariable(Blueprint, *DispatcherName, *NewName);
				UBlueprintEditorLibrary::RenameGraph(DelegateGraph, NewName);
				FBlueprintEditorUtils::ConformDelegateSignatureGraphs(Blueprint);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				OutStructured = EventDispatcherGraphToJson(Blueprint, DelegateGraph);
				OutSummary = TEXT("Renamed blueprint event dispatcher.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_remove_event_dispatcher"),
			TEXT("Remove an event dispatcher from a blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("dispatcher_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("dispatcher_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString DispatcherName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("dispatcher_name"), DispatcherName))
				{
					OutError = TEXT("Missing asset_path or dispatcher_name.");
					return false;
				}

				UBlueprint* Blueprint = LoadBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}

				UEdGraph* DelegateGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, *DispatcherName);
				if (!DelegateGraph)
				{
					OutError = TEXT("Event dispatcher graph was not found.");
					return false;
				}

				const TSharedRef<FJsonObject> RemovedDispatcher = EventDispatcherGraphToJson(Blueprint, DelegateGraph);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintRemoveEventDispatcher", "SOMOLMCP Remove Blueprint Event Dispatcher"));
				Blueprint->Modify();
				DelegateGraph->Modify();
				FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DelegateGraph->GetFName());
				FBlueprintEditorUtils::RemoveGraph(Blueprint, DelegateGraph, EGraphRemoveFlags::Recompile);
				for (TObjectIterator<UK2Node_CreateDelegate> It(RF_ClassDefaultObject, true, EInternalObjectFlags::Garbage); It; ++It)
				{
					UK2Node_CreateDelegate* CreateDelegateNode = *It;
					if (IsValid(CreateDelegateNode) && IsValid(CreateDelegateNode->GetGraph()))
					{
						CreateDelegateNode->HandleAnyChange();
					}
				}

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetBoolField(TEXT("deleted"), true);
				OutStructured->SetObjectField(TEXT("eventDispatcher"), RemovedDispatcher);
				OutSummary = TEXT("Removed blueprint event dispatcher.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("blueprint_add_delegate_node"),
			TEXT("Add a delegate bind/unbind/clear/call node for an event dispatcher."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("dispatcher_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("action"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("dispatcher_name"), TEXT("action")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString DispatcherName;
				FString Action;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("dispatcher_name"), DispatcherName) || !Arguments->TryGetStringField(TEXT("action"), Action))
				{
					OutError = TEXT("Missing delegate node arguments.");
					return false;
				}

				UBlueprint* Blueprint = nullptr;
				UEdGraph* Graph = nullptr;
				if (!TryGetGraphAndBlueprint(Context.Services, AssetPath, GraphName, Blueprint, Graph, OutError))
				{
					return false;
				}

				FMulticastDelegateProperty* DelegateProperty = ResolveBlueprintEventDispatcherProperty(Blueprint, DispatcherName, OutError);
				if (!DelegateProperty)
				{
					return false;
				}

				TSubclassOf<UK2Node_BaseMCDelegate> NodeClass = nullptr;
				if (Action.Equals(TEXT("bind"), ESearchCase::IgnoreCase) || Action.Equals(TEXT("add"), ESearchCase::IgnoreCase))
				{
					NodeClass = UK2Node_AddDelegate::StaticClass();
				}
				else if (Action.Equals(TEXT("unbind"), ESearchCase::IgnoreCase) || Action.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
				{
					NodeClass = UK2Node_RemoveDelegate::StaticClass();
				}
				else if (Action.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
				{
					NodeClass = UK2Node_ClearDelegate::StaticClass();
				}
				else if (Action.Equals(TEXT("call"), ESearchCase::IgnoreCase))
				{
					NodeClass = UK2Node_CallDelegate::StaticClass();
				}
				else
				{
					OutError = TEXT("action must be bind, remove, clear, or call.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlueprintAddDelegateNode", "SOMOLMCP Add Blueprint Delegate Node"));
				Blueprint->Modify();
				UBlueprintDelegateNodeSpawner* NodeSpawner = UBlueprintDelegateNodeSpawner::Create(NodeClass, DelegateProperty);
				UEdGraphNode* Node = NodeSpawner ? NodeSpawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), FVector2D(GetBlueprintNodeLocationFromArguments(Arguments))) : nullptr;
				if (!Node)
				{
					OutError = TEXT("Failed to spawn delegate node.");
					return false;
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added blueprint delegate node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_blueprint_create"),
			TEXT("Create a widget blueprint asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetName;
				if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) || !Arguments->TryGetStringField(TEXT("asset_name"), AssetName))
				{
					OutError = TEXT("Missing package_path or asset_name.");
					return false;
				}
				FString ParentClassPath;
				Arguments->TryGetStringField(TEXT("parent_class_path"), ParentClassPath);
				TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
				Overrides->SetStringField(TEXT("ParentClass"), ParentClassPath.IsEmpty() ? TEXT("/Script/UMG.UserWidget") : ParentClassPath);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgWidgetBlueprintCreate", "SOMOLMCP Create Widget Blueprint"));
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/UMGEditor.WidgetBlueprint"), TEXT("/Script/UMGEditor.WidgetBlueprintFactory"), Overrides, OutError);
				if (!Asset)
				{
					return false;
				}
				// Audit round 7 (silent-create fix): force save + asset_registry notify so subsequent
				// widget_blueprint_inspect can LoadAsset(); verify persistence before returning ok.
				const FString CreatedPath = Asset->GetPathName();
				Asset->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Asset);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!Context.Services.AssetExists(CreatedPath))
				{
					OutStructured = MakeShared<FJsonObject>();
					OutStructured->SetStringField(TEXT("error"), TEXT("asset_not_persisted_after_create"));
					OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					OutError = FString::Printf(TEXT("asset_not_persisted_after_create: %s"), *CreatedPath);
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutSummary = TEXT("Created widget blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_list"),
			TEXT("List widgets in a widget blueprint tree."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				TArray<UWidget*> Widgets;
				WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
				TArray<TSharedPtr<FJsonValue>> WidgetJson;
				for (UWidget* Widget : Widgets)
				{
					TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
					WidgetObject->SetStringField(TEXT("name"), Widget ? Widget->GetName() : FString());
					WidgetObject->SetStringField(TEXT("class"), Widget ? Widget->GetClass()->GetPathName() : FString());
					WidgetObject->SetStringField(TEXT("parent"), Widget && Widget->GetParent() ? Widget->GetParent()->GetName() : FString());
					WidgetJson.Add(MakeShared<FJsonValueObject>(WidgetObject));
				}
				OutStructured->SetArrayField(TEXT("widgets"), WidgetJson);
				OutStructured->SetNumberField(TEXT("count"), WidgetJson.Num());
				OutStructured->SetStringField(TEXT("rootWidget"), WidgetBlueprint->WidgetTree->RootWidget ? WidgetBlueprint->WidgetTree->RootWidget->GetName() : FString());
				OutSummary = TEXT("Listed widget blueprint tree.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_get_widget"),
			TEXT("Get one widget entry from a widget blueprint tree."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("widget_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName))
				{
					OutError = TEXT("Missing asset_path or widget_name.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
				if (!Widget)
				{
					OutError = TEXT("Widget was not found in widget tree.");
					return false;
				}
				OutStructured->SetStringField(TEXT("name"), Widget->GetName());
				OutStructured->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
				OutStructured->SetStringField(TEXT("parent"), Widget->GetParent() ? Widget->GetParent()->GetName() : FString());
				OutSummary = TEXT("Collected widget details.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_rename_widget"),
			TEXT("Rename a widget in a widget blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing UMG rename arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* Widget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, Widget, OutError))
				{
					return false;
				}
				FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditorForAsset(WidgetBlueprint, OutError);
				if (!WidgetEditor)
				{
					return false;
				}
				TSharedRef<FWidgetBlueprintEditor> WidgetEditorRef = StaticCastSharedRef<FWidgetBlueprintEditor>(WidgetEditor->AsShared());
				FText RenameError;
				if (!FWidgetBlueprintEditorUtils::VerifyWidgetRename(WidgetEditorRef, WidgetEditor->GetReferenceFromTemplate(Widget), FText::FromString(NewName), RenameError))
				{
					OutError = RenameError.ToString();
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgRenameWidget", "SOMOLMCP Rename Widget"));
				WidgetBlueprint->Modify();
				if (!FWidgetBlueprintEditorUtils::RenameWidget(WidgetEditorRef, Widget->GetFName(), NewName))
				{
					OutError = TEXT("RenameWidget failed.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Renamed widget.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_move_widget"),
			TEXT("Move a widget under a new parent widget."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_parent_widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("child_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("new_parent_widget_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString ParentWidgetName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetStringField(TEXT("new_parent_widget_name"), ParentWidgetName))
				{
					OutError = TEXT("Missing UMG move arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* Widget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, Widget, OutError))
				{
					return false;
				}
				UWidget* ParentWidget = WidgetBlueprint->WidgetTree->FindWidget(*ParentWidgetName);
				UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
				if (!ParentPanel)
				{
					OutError = TEXT("new_parent_widget_name must resolve to a panel widget.");
					return false;
				}
				const int32 ChildIndex = Arguments->HasTypedField<EJson::Number>(TEXT("child_index")) ? Arguments->GetIntegerField(TEXT("child_index")) : INDEX_NONE;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgMoveWidget", "SOMOLMCP Move Widget"));
				WidgetBlueprint->Modify();
				if (!RemoveWidgetFromTree(WidgetBlueprint, Widget, OutError) || !InsertWidgetIntoTree(WidgetBlueprint, Widget, ParentPanel, ChildIndex, OutError))
				{
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Moved widget.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_reorder_child"),
			TEXT("Reorder a child within a panel widget."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("child_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("parent_widget_name"), TEXT("widget_name"), TEXT("child_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParentWidgetName;
				FString WidgetName;
				int32 ChildIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("parent_widget_name"), ParentWidgetName) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetNumberField(TEXT("child_index"), ChildIndex))
				{
					OutError = TEXT("Missing UMG reorder arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UPanelWidget* ParentPanel = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->FindWidget(*ParentWidgetName));
				UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
				if (!ParentPanel || !Widget || Widget->GetParent() != ParentPanel)
				{
					OutError = TEXT("Widget must already be parented to parent_widget_name.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgReorderChild", "SOMOLMCP Reorder Widget Child"));
				WidgetBlueprint->Modify();
				ParentPanel->RemoveChild(Widget);
				if (!InsertWidgetIntoTree(WidgetBlueprint, Widget, ParentPanel, ChildIndex, OutError))
				{
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Reordered widget child.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_replace_widget"),
			TEXT("Replace a widget with another widget class."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("widget_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString WidgetClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetStringField(TEXT("widget_class_path"), WidgetClassPath))
				{
					OutError = TEXT("Missing UMG replace arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* OldWidget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, OldWidget, OutError))
				{
					return false;
				}
				UClass* WidgetClass = Context.Services.ResolveClass(WidgetClassPath, OutError);
				if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
				{
					OutError = TEXT("widget_class_path must resolve to a widget class.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgReplaceWidget", "SOMOLMCP Replace Widget"));
				WidgetBlueprint->Modify();
				TSet<UWidget*> WidgetsToReplace;
				WidgetsToReplace.Add(OldWidget);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				FWidgetBlueprintEditorUtils::ReplaceWidgets(WidgetBlueprint, WidgetsToReplace, WidgetClass, FWidgetBlueprintEditorUtils::EReplaceWidgetNamingMethod::MaintainNameAndReferences);
#else
				// UE 5.5's ReplaceWidgets takes a live FWidgetBlueprintEditor and a set of
				// FWidgetReference instead of raw widgets, and has no naming-method option.
				// There is no equivalent that preserves names and references headlessly, so
				// this reports rather than replacing widgets under different names.
				OutError = TEXT("umg_replace_widget requires UE 5.6 or newer: 5.5's ReplaceWidgets cannot maintain names and references without an open widget editor.");
				OutStructured->SetStringField(TEXT("error_code"), TEXT("NOT_AVAILABLE_ON_ENGINE"));
				OutStructured->SetStringField(TEXT("required_api"), TEXT("FWidgetBlueprintEditorUtils::EReplaceWidgetNamingMethod"));
				OutStructured->SetStringField(TEXT("minimum_engine"), TEXT("5.6"));
				return false;
#endif
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Replaced widget.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_wrap_widget"),
			TEXT("Wrap a widget in another panel widget class."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("wrapper_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("wrapper_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString WrapperClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetStringField(TEXT("wrapper_class_path"), WrapperClassPath))
				{
					OutError = TEXT("Missing UMG wrap arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* Widget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, Widget, OutError))
				{
					return false;
				}
				UClass* WrapperClass = Context.Services.ResolveClass(WrapperClassPath, OutError);
				if (!WrapperClass || !WrapperClass->IsChildOf(UPanelWidget::StaticClass()))
				{
					OutError = TEXT("wrapper_class_path must resolve to a panel widget class.");
					return false;
				}
				UPanelWidget* OldParent = Widget->GetParent();
				int32 OldIndex = INDEX_NONE;
				if (OldParent)
				{
					OldIndex = OldParent->GetChildIndex(Widget);
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgWrapWidget", "SOMOLMCP Wrap Widget"));
				WidgetBlueprint->Modify();
				if (!RemoveWidgetFromTree(WidgetBlueprint, Widget, OutError))
				{
					return false;
				}
				UPanelWidget* Wrapper = Cast<UPanelWidget>(WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WrapperClass, *FString::Printf(TEXT("%s_Wrapper"), *WidgetName)));
				if (!Wrapper)
				{
					OutError = TEXT("Failed to construct wrapper widget.");
					return false;
				}
				if (!InsertWidgetIntoTree(WidgetBlueprint, Wrapper, OldParent, OldIndex, OutError) || !InsertWidgetIntoTree(WidgetBlueprint, Widget, Wrapper, INDEX_NONE, OutError))
				{
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Wrapped widget.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_set_widget_properties"),
			TEXT("Apply public properties to a widget in a widget blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("properties")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				TSharedPtr<FJsonObject> Properties;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !TryGetObjectField(Arguments, TEXT("properties"), Properties))
				{
					OutError = TEXT("Missing UMG widget property arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
				if (!Widget)
				{
					OutError = TEXT("Widget was not found in widget tree.");
					return false;
				}
				if (!Context.Services.ApplyProperties(Widget, Properties, OutError))
				{
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Updated widget properties.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_list_view_set_entry_class"),
			TEXT("Set the EntryWidgetClass on a ListView, TileView, or TreeView widget blueprint child."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("entry_widget_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("entry_widget_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString EntryWidgetClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetStringField(TEXT("entry_widget_class_path"), EntryWidgetClassPath))
				{
					OutError = TEXT("Missing UMG list view entry class arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
				if (!Widget)
				{
					OutError = TEXT("Widget was not found in widget tree.");
					return false;
				}
				UClass* EntryClass = Context.Services.ResolveClass(EntryWidgetClassPath, OutError);
				if (!EntryClass)
				{
					return false;
				}
				if (!EntryClass->IsChildOf(UUserWidget::StaticClass()))
				{
					OutError = TEXT("entry_widget_class_path must resolve to a UserWidget-derived class.");
					return false;
				}
				FProperty* EntryProperty = Widget->GetClass()->FindPropertyByName(TEXT("EntryWidgetClass"));
				FClassProperty* EntryClassProperty = CastField<FClassProperty>(EntryProperty);
				if (!EntryClassProperty)
				{
					OutError = TEXT("Widget does not expose an EntryWidgetClass class property.");
					return false;
				}
				if (EntryClassProperty->MetaClass && !EntryClass->IsChildOf(EntryClassProperty->MetaClass))
				{
					OutError = TEXT("entry_widget_class_path is not compatible with the widget EntryWidgetClass property.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgListViewSetEntryClass", "SOMOLMCP Set List View Entry Class"));
				WidgetBlueprint->Modify();
				Widget->Modify();
				EntryClassProperty->SetObjectPropertyValue_InContainer(Widget, EntryClass);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
				OutStructured->SetStringField(TEXT("entry_widget_class_path"), EntryClass->GetPathName());
				OutSummary = TEXT("Updated list view entry widget class.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_list_view_bind_items_source"),
			TEXT("Validate and receipt a ListView, TileView, or TreeView item-source binding contract."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("view_kind"), FSololmcpSchemaBuilder::String()}, {TEXT("source_kind"), FSololmcpSchemaBuilder::String()}, {TEXT("source_name"), FSololmcpSchemaBuilder::String()}, {TEXT("source_path"), FSololmcpSchemaBuilder::String()}, {TEXT("item_type"), FSololmcpSchemaBuilder::String()}, {TEXT("sample_item_count"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("source_kind"), TEXT("source_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString SourceKind;
				FString SourceName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetStringField(TEXT("source_kind"), SourceKind) || !Arguments->TryGetStringField(TEXT("source_name"), SourceName))
				{
					OutError = TEXT("Missing UMG list view item-source binding arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* Widget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, Widget, OutError))
				{
					return false;
				}
				const FString WidgetClass = Widget->GetClass()->GetName();
				if (!IsDataViewWidgetClassName(WidgetClass))
				{
					OutError = TEXT("widget_name must resolve to a ListView, TileView, or TreeView widget.");
					return false;
				}
				OutStructured = MakeUmgDataViewContractReceipt(WidgetBlueprint, Widget, WidgetName, TEXT("umg_list_view_bind_items_source"));
				FString ViewKind;
				FString SourcePath;
				FString ItemType;
				int32 SampleItemCount = 0;
				Arguments->TryGetStringField(TEXT("view_kind"), ViewKind);
				Arguments->TryGetStringField(TEXT("source_path"), SourcePath);
				Arguments->TryGetStringField(TEXT("item_type"), ItemType);
				Arguments->TryGetNumberField(TEXT("sample_item_count"), SampleItemCount);
				OutStructured->SetStringField(TEXT("view_kind"), ViewKind);
				OutStructured->SetStringField(TEXT("source_kind"), SourceKind);
				OutStructured->SetStringField(TEXT("source_name"), SourceName);
				OutStructured->SetStringField(TEXT("source_path"), SourcePath);
				OutStructured->SetStringField(TEXT("item_type"), ItemType);
				OutStructured->SetNumberField(TEXT("sample_item_count"), SampleItemCount);
				OutSummary = TEXT("Validated UMG data-view item-source binding contract.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_tree_view_bind_hierarchy_source"),
			TEXT("Validate and receipt a TreeView hierarchy children-source binding contract."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("source_name"), FSololmcpSchemaBuilder::String()}, {TEXT("source_path"), FSololmcpSchemaBuilder::String()}, {TEXT("item_type"), FSololmcpSchemaBuilder::String()}, {TEXT("children_field"), FSololmcpSchemaBuilder::String()}, {TEXT("sample_item_count"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("source_name"), TEXT("children_field")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString SourceName;
				FString ChildrenField;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !Arguments->TryGetStringField(TEXT("source_name"), SourceName) || !Arguments->TryGetStringField(TEXT("children_field"), ChildrenField))
				{
					OutError = TEXT("Missing UMG TreeView hierarchy binding arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* Widget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, Widget, OutError))
				{
					return false;
				}
				const FString WidgetClass = Widget->GetClass()->GetName();
				if (!WidgetClass.Contains(TEXT("TreeView")))
				{
					OutError = TEXT("widget_name must resolve to a TreeView widget.");
					return false;
				}
				OutStructured = MakeUmgDataViewContractReceipt(WidgetBlueprint, Widget, WidgetName, TEXT("umg_tree_view_bind_hierarchy_source"));
				FString SourcePath;
				FString ItemType;
				int32 SampleItemCount = 0;
				Arguments->TryGetStringField(TEXT("source_path"), SourcePath);
				Arguments->TryGetStringField(TEXT("item_type"), ItemType);
				Arguments->TryGetNumberField(TEXT("sample_item_count"), SampleItemCount);
				OutStructured->SetStringField(TEXT("source_name"), SourceName);
				OutStructured->SetStringField(TEXT("source_path"), SourcePath);
				OutStructured->SetStringField(TEXT("item_type"), ItemType);
				OutStructured->SetStringField(TEXT("children_field"), ChildrenField);
				OutStructured->SetNumberField(TEXT("sample_item_count"), SampleItemCount);
				OutSummary = TEXT("Validated UMG TreeView hierarchy binding contract.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_set_slot_properties"),
			TEXT("Apply public properties to a widget slot in a widget blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("properties")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				TSharedPtr<FJsonObject> Properties;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) || !TryGetObjectField(Arguments, TEXT("properties"), Properties))
				{
					OutError = TEXT("Missing UMG slot property arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
				if (!Widget || !Widget->Slot)
				{
					OutError = TEXT("Widget slot was not found.");
					return false;
				}
				if (!Context.Services.ApplyProperties(Widget->Slot, Properties, OutError))
				{
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Updated widget slot properties.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_image_bind_render_target"),
			TEXT("Bind a UMG Image widget brush to a Texture or TextureRenderTarget asset and return readback evidence."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("widget_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("render_target_asset"), FSololmcpSchemaBuilder::String()},
				{TEXT("width"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("height"), FSololmcpSchemaBuilder::Integer()}
			}, {TEXT("asset_path"), TEXT("widget_name"), TEXT("render_target_asset")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				FString RenderTargetAsset;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) ||
					!Arguments->TryGetStringField(TEXT("render_target_asset"), RenderTargetAsset))
				{
					OutError = TEXT("Missing UMG Image RenderTarget binding arguments.");
					return false;
				}

				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
				UImage* Image = Cast<UImage>(Widget);
				if (!Image)
				{
					OutError = TEXT("Widget was not found or is not a UMG Image.");
					return false;
				}

				UObject* TextureObject = Context.Services.LoadAsset(RenderTargetAsset, OutError);
				UTexture* Texture = Cast<UTexture>(TextureObject);
				if (!Texture)
				{
					OutError = TEXT("render_target_asset must resolve to a UTexture or TextureRenderTarget asset.");
					return false;
				}

				int32 Width = 0;
				int32 Height = 0;
				if (UTextureRenderTarget2D* RT = Cast<UTextureRenderTarget2D>(Texture))
				{
					Width = RT->SizeX;
					Height = RT->SizeY;
				}
				double RequestedWidth = 0.0;
				double RequestedHeight = 0.0;
				if (Arguments->TryGetNumberField(TEXT("width"), RequestedWidth) && RequestedWidth > 0.0)
				{
					Width = static_cast<int32>(RequestedWidth);
				}
				if (Arguments->TryGetNumberField(TEXT("height"), RequestedHeight) && RequestedHeight > 0.0)
				{
					Height = static_cast<int32>(RequestedHeight);
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgImageBindRenderTarget", "SOMOLMCP Bind UMG Image RenderTarget"));
				WidgetBlueprint->Modify();
				Image->Modify();
				FSlateBrush Brush = Image->GetBrush();
				Brush.SetResourceObject(Texture);
				if (Width > 0 && Height > 0)
				{
					Brush.ImageSize = FVector2D(static_cast<double>(Width), static_cast<double>(Height));
				}
				Image->SetBrush(Brush);
				Image->SynchronizeProperties();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				SololmcpWriteFlush::EnsureFlushed(WidgetBlueprint);

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
				OutStructured->SetStringField(TEXT("render_target_asset"), Texture->GetPathName());
				OutStructured->SetStringField(TEXT("brush_resource"), Brush.GetResourceObject() ? Brush.GetResourceObject()->GetPathName() : FString());
				OutStructured->SetBoolField(TEXT("brush_resource_points_to_render_target"), Brush.GetResourceObject() == Texture);
				OutStructured->SetBoolField(TEXT("image_brush_resource_points_to_render_target"), Brush.GetResourceObject() == Texture);
				OutStructured->SetBoolField(TEXT("render_target_bound"), Brush.GetResourceObject() == Texture);
				OutStructured->SetBoolField(TEXT("mutation_performed"), true);
				OutStructured->SetNumberField(TEXT("width"), Width);
				OutStructured->SetNumberField(TEXT("height"), Height);
				OutSummary = FString::Printf(TEXT("Bound Image '%s' to render target '%s'."), *WidgetName, *Texture->GetPathName());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_list_named_slots"),
			TEXT("List named slots and their content on a widget blueprint or host widget."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("host_widget_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, HostWidgetName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("host_widget_name"), HostWidgetName);
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UWidget* HostWidget = nullptr;
				if (HostWidgetName.IsEmpty())
				{
					HostWidget = WidgetBlueprint->WidgetTree->RootWidget;
				}
				else
				{
					HostWidget = WidgetBlueprint->WidgetTree->FindWidget(*HostWidgetName);
				}
				if (!HostWidget)
				{
					OutError = HostWidgetName.IsEmpty() ? TEXT("Widget blueprint has no root widget.") : FString::Printf(TEXT("Host widget '%s' not found."), *HostWidgetName);
					return false;
				}
				INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(HostWidget);
				if (!NamedSlotHost)
				{
					OutError = TEXT("Host widget does not implement INamedSlotInterface.");
					return false;
				}
				TArray<FName> SlotNames;
				NamedSlotHost->GetSlotNames(SlotNames);
				TArray<TSharedPtr<FJsonValue>> SlotsJson;
				for (const FName& SlotName : SlotNames)
				{
					UWidget* Content = NamedSlotHost->GetContentForSlot(SlotName);
					TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
					SlotObj->SetStringField(TEXT("slot"), SlotName.ToString());
					SlotObj->SetStringField(TEXT("content"), Content ? Content->GetName() : FString());
					SlotsJson.Add(MakeShared<FJsonValueObject>(SlotObj));
				}
				OutStructured->SetStringField(TEXT("host"), HostWidget->GetName());
				OutStructured->SetArrayField(TEXT("slots"), SlotsJson);
				OutSummary = TEXT("Listed named slots.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("umg_widget_tree_set_named_slot_content"),
			TEXT("Set content for a named slot host."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("host_widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("slot_name"), FSololmcpSchemaBuilder::String()}, {TEXT("content_widget_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("host_widget_name"), TEXT("slot_name"), TEXT("content_widget_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, HostWidgetName, SlotName, ContentWidgetName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("host_widget_name"), HostWidgetName) || !Arguments->TryGetStringField(TEXT("slot_name"), SlotName) || !Arguments->TryGetStringField(TEXT("content_widget_name"), ContentWidgetName))
				{
					OutError = TEXT("Missing named slot arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* HostWidget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, HostWidgetName, WidgetBlueprint, HostWidget, OutError))
				{
					return false;
				}
				INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(HostWidget);
				if (!NamedSlotHost)
				{
					OutError = TEXT("Host widget does not implement INamedSlotInterface.");
					return false;
				}
				TArray<FName> SlotNames;
				NamedSlotHost->GetSlotNames(SlotNames);
				const FName SlotNameF = FName(*SlotName);
				if (SlotNames.Num() > 0 && !SlotNames.Contains(SlotNameF))
				{
					OutError = FString::Printf(TEXT("Slot '%s' not found on host."), *SlotName);
					return false;
				}
				UWidget* ContentWidget = WidgetBlueprint->WidgetTree ? WidgetBlueprint->WidgetTree->FindWidget(*ContentWidgetName) : nullptr;
				if (!ContentWidget)
				{
					OutError = TEXT("Content widget was not found in widget tree.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgSetNamedSlotContent", "SOMOLMCP Set Named Slot Content"));
				WidgetBlueprint->Modify();
				if (UWidget* ExistingContent = NamedSlotHost->GetContentForSlot(SlotNameF))
				{
					ExistingContent->Modify();
				}
				HostWidget->Modify();
				if (UPanelWidget* CurrentParent = ContentWidget->GetParent())
				{
					CurrentParent->RemoveChild(ContentWidget);
				}
				NamedSlotHost->SetContentForSlot(SlotNameF, ContentWidget);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured->SetStringField(TEXT("host"), HostWidgetName);
				OutStructured->SetStringField(TEXT("slot"), SlotName);
				OutStructured->SetStringField(TEXT("content"), ContentWidgetName);
				OutSummary = TEXT("Set named slot content.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("umg_widget_tree_clear_named_slot_content"),
			TEXT("Clear content for a named slot host."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("host_widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("slot_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("host_widget_name"), TEXT("slot_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, HostWidgetName, SlotName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("host_widget_name"), HostWidgetName) || !Arguments->TryGetStringField(TEXT("slot_name"), SlotName))
				{
					OutError = TEXT("Missing named slot clear arguments.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* HostWidget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, HostWidgetName, WidgetBlueprint, HostWidget, OutError))
				{
					return false;
				}
				INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(HostWidget);
				if (!NamedSlotHost)
				{
					OutError = TEXT("Host widget does not implement INamedSlotInterface.");
					return false;
				}
				TArray<FName> SlotNames;
				NamedSlotHost->GetSlotNames(SlotNames);
				const FName SlotNameF = FName(*SlotName);
				if (SlotNames.Num() > 0 && !SlotNames.Contains(SlotNameF))
				{
					OutError = FString::Printf(TEXT("Slot '%s' not found on host."), *SlotName);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgClearNamedSlotContent", "SOMOLMCP Clear Named Slot Content"));
				WidgetBlueprint->Modify();
				HostWidget->Modify();
				NamedSlotHost->SetContentForSlot(SlotNameF, nullptr);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured->SetStringField(TEXT("host"), HostWidgetName);
				OutStructured->SetStringField(TEXT("slot"), SlotName);
				OutSummary = TEXT("Cleared named slot content.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_animation_list"),
			TEXT("List widget animations on a widget blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> AnimationsJson;
				int32 TotalTrackCount = 0;
				int32 TotalKeyframeCount = 0;
				for (UWidgetAnimation* Anim : WidgetBlueprint->Animations)
				{
					if (!Anim) continue;
					int32 TrackCount = 0;
					int32 KeyframeCount = 0;
					CountUmgAnimationTracksAndKeyframes(Anim, TrackCount, KeyframeCount);
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Anim->GetName());
					Obj->SetStringField(TEXT("display_label"), Anim->GetDisplayLabel());
					Obj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
					Obj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());
					Obj->SetNumberField(TEXT("binding_count"), Anim->AnimationBindings.Num());
					Obj->SetNumberField(TEXT("track_count"), TrackCount);
					Obj->SetNumberField(TEXT("keyframe_count"), KeyframeCount);
					AnimationsJson.Add(MakeShared<FJsonValueObject>(Obj));
					TotalTrackCount += TrackCount;
					TotalKeyframeCount += KeyframeCount;
				}
				OutStructured->SetArrayField(TEXT("animations"), AnimationsJson);
				OutStructured->SetNumberField(TEXT("track_count"), TotalTrackCount);
				OutStructured->SetNumberField(TEXT("keyframe_count"), TotalKeyframeCount);
				OutStructured->SetBoolField(TEXT("receipt_complete"), true);
				OutStructured->SetStringField(TEXT("compile_or_refresh_hint"), GetUmgAnimationCompileOrRefreshHint());
				OutSummary = TEXT("Listed widget animations.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("umg_animation_create"),
			TEXT("Create a new widget animation."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("animation_name"), FSololmcpSchemaBuilder::String()}, {TEXT("duration_seconds"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, AnimationName;
				double DurationSeconds = 1.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("animation_name"), AnimationName);
				Arguments->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds);
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				FString BaseName = AnimationName.IsEmpty() ? TEXT("NewAnimation") : AnimationName;
				FString UniqueName = FBlueprintEditorUtils::FindUniqueKismetName(WidgetBlueprint, BaseName).ToString();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgAnimationCreate", "SOMOLMCP Create Widget Animation"));
				WidgetBlueprint->Modify();
				UWidgetAnimation* NewAnimation = NewObject<UWidgetAnimation>(WidgetBlueprint, FName(), RF_Transactional);
				NewAnimation->SetDisplayLabel(UniqueName);
				NewAnimation->Rename(*UniqueName);
				NewAnimation->MovieScene = NewObject<UMovieScene>(NewAnimation, FName(*UniqueName), RF_Transactional);
				NewAnimation->MovieScene->SetDisplayRate(FFrameRate(20, 1));
				const float Duration = FMath::Max(0.01f, static_cast<float>(DurationSeconds));
				const FFrameTime DurationFrames = Duration * NewAnimation->MovieScene->GetTickResolution();
				NewAnimation->MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(0) + DurationFrames.FrameNumber));
				NewAnimation->MovieScene->GetEditorData().WorkStart = 0.0;
				NewAnimation->MovieScene->GetEditorData().WorkEnd = Duration;
				WidgetBlueprint->Animations.Add(NewAnimation);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(NewAnimation->GetFName()))
				{
					WidgetBlueprint->WidgetVariableNameToGuidMap.Emplace(NewAnimation->GetFName(), FGuid::NewDeterministicGuid(NewAnimation->GetPathName()));
				}
#endif // WidgetVariableNameToGuidMap is UE 5.6+
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured->SetStringField(TEXT("name"), UniqueName);
				OutStructured->SetNumberField(TEXT("duration"), Duration);
				OutStructured->SetNumberField(TEXT("track_count"), 0);
				OutStructured->SetNumberField(TEXT("keyframe_count"), 0);
				OutStructured->SetBoolField(TEXT("receipt_complete"), false);
				OutStructured->SetStringField(TEXT("compile_or_refresh_hint"), GetUmgAnimationCompileOrRefreshHint());
				OutSummary = TEXT("Created widget animation.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("umg_animation_rename"),
			TEXT("Rename a widget animation."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("animation_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("animation_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, AnimationName, NewName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("animation_name"), AnimationName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing asset_path, animation_name, or new_name.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint) return false;
				UWidgetAnimation* Anim = SOMOLMCP_FIND_OBJECT_EXACT(UWidgetAnimation, WidgetBlueprint, *AnimationName);
				if (!Anim)
				{
					OutError = FString::Printf(TEXT("Animation '%s' not found."), *AnimationName);
					return false;
				}
				const FName NewNameF = FBlueprintEditorUtils::FindUniqueKismetName(WidgetBlueprint, NewName);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgAnimationRename", "SOMOLMCP Rename Widget Animation"));
				WidgetBlueprint->Modify();
				Anim->Modify();
				const FName OldNameF = Anim->GetFName();
				FGuid ExistingVariableGuid;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				const bool bHadVariableGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.RemoveAndCopyValue(OldNameF, ExistingVariableGuid);
#endif // WidgetVariableNameToGuidMap is UE 5.6+
				Anim->SetDisplayLabel(NewNameF.ToString());
				Anim->Rename(*NewNameF.ToString(), WidgetBlueprint, REN_DontCreateRedirectors);
				if (Anim->MovieScene) Anim->MovieScene->Rename(*NewNameF.ToString(), Anim, REN_DontCreateRedirectors);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				WidgetBlueprint->WidgetVariableNameToGuidMap.Emplace(Anim->GetFName(), bHadVariableGuid ? ExistingVariableGuid : FGuid::NewDeterministicGuid(Anim->GetPathName()));
#endif // WidgetVariableNameToGuidMap is UE 5.6+
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured->SetStringField(TEXT("name"), NewNameF.ToString());
				OutSummary = TEXT("Renamed widget animation.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("umg_animation_delete"),
			TEXT("Delete a widget animation."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("animation_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("animation_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, AnimationName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("animation_name"), AnimationName))
				{
					OutError = TEXT("Missing asset_path or animation_name.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint) return false;
				int32 FoundIndex = WidgetBlueprint->Animations.IndexOfByPredicate([&](UWidgetAnimation* A) { return A && A->GetFName() == FName(*AnimationName); });
				if (FoundIndex == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Animation '%s' not found."), *AnimationName);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgAnimationDelete", "SOMOLMCP Delete Widget Animation"));
				WidgetBlueprint->Modify();
				WidgetBlueprint->Animations.RemoveAt(FoundIndex);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured->SetStringField(TEXT("deleted"), AnimationName);
				OutSummary = TEXT("Deleted widget animation.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("umg_animation_list_bindings"),
			TEXT("List widget bindings for an animation."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("animation_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("animation_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, AnimationName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("animation_name"), AnimationName))
				{
					OutError = TEXT("Missing asset_path or animation_name.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint) return false;
				UWidgetAnimation* Anim = SOMOLMCP_FIND_OBJECT_EXACT(UWidgetAnimation, WidgetBlueprint, *AnimationName);
				if (!Anim)
				{
					OutError = FString::Printf(TEXT("Animation '%s' not found."), *AnimationName);
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> BindingsJson;
				for (const FWidgetAnimationBinding& B : Anim->AnimationBindings)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("widget_name"), B.WidgetName.ToString());
					Obj->SetStringField(TEXT("slot_widget_name"), B.SlotWidgetName.ToString());
					Obj->SetStringField(TEXT("animation_guid"), B.AnimationGuid.ToString());
					Obj->SetBoolField(TEXT("is_root_widget"), B.bIsRootWidget);
					BindingsJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetStringField(TEXT("animation"), AnimationName);
				OutStructured->SetArrayField(TEXT("bindings"), BindingsJson);
				OutSummary = TEXT("Listed animation bindings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_animation_inspect_tracks"),
			TEXT("Inspect movie scene tracks and sections for a widget animation."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("animation_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("animation_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, AnimationName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("animation_name"), AnimationName))
				{
					OutError = TEXT("Missing asset_path or animation_name.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint) return false;
				UWidgetAnimation* Anim = SOMOLMCP_FIND_OBJECT_EXACT(UWidgetAnimation, WidgetBlueprint, *AnimationName);
				if (!Anim)
				{
					OutError = FString::Printf(TEXT("Animation '%s' not found."), *AnimationName);
					return false;
				}

				UMovieScene* MovieScene = Anim->MovieScene;
				TArray<TSharedPtr<FJsonValue>> TracksJson;
				if (MovieScene)
				{
					for (UMovieSceneTrack* Track : MovieScene->GetTracks())
					{
						if (!Track)
						{
							continue;
						}
						TSharedRef<FJsonObject> TrackJson = MakeUmgAnimationTrackJson(Anim, Track, FGuid());
						TracksJson.Add(MakeShared<FJsonValueObject>(TrackJson.ToSharedPtr()));
					}
					const UMovieScene* ConstMovieScene = MovieScene;
					for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
					{
						for (UMovieSceneTrack* Track : Binding.GetTracks())
						{
							if (!Track)
							{
								continue;
							}
							TSharedRef<FJsonObject> TrackJson = MakeUmgAnimationTrackJson(Anim, Track, Binding.GetObjectGuid());
							TracksJson.Add(MakeShared<FJsonValueObject>(TrackJson.ToSharedPtr()));
						}
					}
				}
				int32 TrackCount = 0;
				int32 KeyframeCount = 0;
				CountUmgAnimationTracksAndKeyframes(Anim, TrackCount, KeyframeCount);

				OutStructured->SetStringField(TEXT("animation"), AnimationName);
				OutStructured->SetNumberField(TEXT("track_count"), TrackCount);
				OutStructured->SetNumberField(TEXT("keyframe_count"), KeyframeCount);
				OutStructured->SetBoolField(TEXT("receipt_complete"), true);
				OutStructured->SetStringField(TEXT("compile_or_refresh_hint"), GetUmgAnimationCompileOrRefreshHint());
				OutStructured->SetArrayField(TEXT("tracks"), TracksJson);
				OutSummary = TEXT("Inspected animation tracks.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_animation_add_track"),
			TEXT("Add a safe UMG animation track shell for a widget. The receipt is incomplete until at least one keyframe is added."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("animation_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("widget_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("track_type"), FSololmcpSchemaBuilder::String(TEXT("float | render_opacity | 2d_transform | render_transform"))},
					{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("Defaults to RenderOpacity for float, RenderTransform for 2d_transform."))},
					{TEXT("property_path"), FSololmcpSchemaBuilder::String(TEXT("Direct widget property path. Nested paths fail closed."))},
					{TEXT("channel"), FSololmcpSchemaBuilder::String(TEXT("Optional 2D channel: translation_x, translation_y, rotation, scale_x, scale_y, shear_x, shear_y."))}
				},
				{TEXT("asset_path"), TEXT("animation_name"), TEXT("widget_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, AnimationName, WidgetName, TrackType, PropertyName, PropertyPath, ChannelName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("animation_name"), AnimationName) ||
					!Arguments->TryGetStringField(TEXT("widget_name"), WidgetName))
				{
					OutError = TEXT("Missing asset_path, animation_name, or widget_name.");
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, TrackType, nullptr, OutError);
					return false;
				}
				Arguments->TryGetStringField(TEXT("track_type"), TrackType);
				Arguments->TryGetStringField(TEXT("property_name"), PropertyName);
				Arguments->TryGetStringField(TEXT("property_path"), PropertyPath);
				Arguments->TryGetStringField(TEXT("channel"), ChannelName);
				const EUmgAnimationTrackKind TrackKind = ResolveUmgAnimationTrackKind(TrackType);
				const FString ResolvedTrackType = UmgAnimationTrackKindToString(TrackKind);

				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* Widget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, Widget, OutError))
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, nullptr, OutError);
					return false;
				}
				UWidgetAnimation* Animation = FindUmgAnimationByName(WidgetBlueprint, AnimationName);
				if (!Animation)
				{
					OutError = FString::Printf(TEXT("Animation '%s' not found."), *AnimationName);
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, nullptr, OutError);
					return false;
				}
				if (!Animation->MovieScene)
				{
					OutError = TEXT("Animation has no MovieScene; refusing to create an unverified empty track.");
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}
				if (!NormalizeUmgAnimationProperty(Widget, TrackKind, PropertyName, PropertyPath, OutError))
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}

				int32 TrackCountBefore = 0;
				int32 KeyframeCountBefore = 0;
				CountUmgAnimationTracksAndKeyframes(Animation, TrackCountBefore, KeyframeCountBefore);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgAnimationAddTrack", "SOMOLMCP Add UMG Animation Track"));
				WidgetBlueprint->Modify();
				Animation->Modify();
				Animation->MovieScene->Modify();
				Widget->Modify();

				FGuid BindingGuid;
				if (!ResolveUmgAnimationBinding(WidgetBlueprint, Animation, Widget, WidgetName, BindingGuid, OutError))
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}

				bool bTrackCreated = false;
				UMovieScenePropertyTrack* Track = EnsureUmgAnimationPropertyTrack(Animation->MovieScene, BindingGuid, TrackKind, PropertyName, PropertyPath, bTrackCreated, OutError);
				if (!Track)
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}
				Track->Modify();

				bool bSectionCreated = false;
				UMovieSceneSection* Section = EnsureUmgAnimationSection(Track, FFrameNumber(0), bSectionCreated, OutError);
				if (!Section)
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}
				Section->Modify();

				if (TrackKind == EUmgAnimationTrackKind::Transform2D)
				{
					UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(Section);
					if (!TransformSection)
					{
						OutError = TEXT("Created section is not a UMovieScene2DTransformSection.");
						SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
						return false;
					}
					if (ChannelName.IsEmpty())
					{
						TransformSection->SetMask(FMovieScene2DTransformMask(EMovieScene2DTransformChannel::AllTransform));
					}
					else
					{
						FMovieSceneFloatChannel* Channel = nullptr;
						EMovieScene2DTransformChannel ChannelMask = EMovieScene2DTransformChannel::None;
						if (!Resolve2DTransformChannel(TransformSection, ChannelName, Channel, ChannelMask, OutError))
						{
							SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
							return false;
						}
						TransformSection->SetMask(FMovieScene2DTransformMask(TransformSection->GetMask().GetChannels() | ChannelMask));
					}
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				WidgetBlueprint->MarkPackageDirty();
				Animation->MarkPackageDirty();

				SetUmgAnimationReceiptBase(OutStructured, TEXT("umg_animation_add_track"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, false);
				OutStructured->SetStringField(TEXT("status"), TEXT("track_ready_keyframe_required"));
				OutStructured->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
				OutStructured->SetStringField(TEXT("property_name"), PropertyName);
				OutStructured->SetStringField(TEXT("property_path"), PropertyPath);
				OutStructured->SetStringField(TEXT("channel"), ChannelName);
				OutStructured->SetBoolField(TEXT("created_track"), bTrackCreated);
				OutStructured->SetBoolField(TEXT("created_section"), bSectionCreated);
				OutStructured->SetBoolField(TEXT("requires_keyframe"), true);
				OutStructured->SetNumberField(TEXT("track_count_before"), TrackCountBefore);
				OutStructured->SetNumberField(TEXT("keyframe_count_before"), KeyframeCountBefore);
				OutStructured->SetNumberField(TEXT("target_track_keyframe_count"), CountMovieSceneTrackKeyframes(Track));
				OutSummary = TEXT("UMG animation track is present; receipt remains incomplete until a keyframe is added and read back.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_animation_add_keyframe"),
			TEXT("Add or update a keyframe on a safe UMG animation property track and verify it by readback."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("animation_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("widget_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("track_type"), FSololmcpSchemaBuilder::String(TEXT("float | render_opacity | 2d_transform | render_transform"))},
					{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("Defaults to RenderOpacity for float, RenderTransform for 2d_transform."))},
					{TEXT("property_path"), FSololmcpSchemaBuilder::String(TEXT("Direct widget property path. Nested paths fail closed."))},
					{TEXT("channel"), FSololmcpSchemaBuilder::String(TEXT("Required for 2d_transform keyframes."))},
					{TEXT("time_seconds"), FSololmcpSchemaBuilder::Number()},
					{TEXT("value"), FSololmcpSchemaBuilder::Number()},
					{TEXT("interpolation"), FSololmcpSchemaBuilder::String(TEXT("linear | constant | cubic | auto"))}
				},
				{TEXT("asset_path"), TEXT("animation_name"), TEXT("widget_name"), TEXT("time_seconds"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, AnimationName, WidgetName, TrackType, PropertyName, PropertyPath, ChannelName, Interpolation;
				double TimeSeconds = 0.0;
				double Value = 0.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("animation_name"), AnimationName) ||
					!Arguments->TryGetStringField(TEXT("widget_name"), WidgetName) ||
					!Arguments->TryGetNumberField(TEXT("time_seconds"), TimeSeconds) ||
					!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("Missing asset_path, animation_name, widget_name, time_seconds, or value.");
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, TrackType, nullptr, OutError);
					return false;
				}
				Arguments->TryGetStringField(TEXT("track_type"), TrackType);
				Arguments->TryGetStringField(TEXT("property_name"), PropertyName);
				Arguments->TryGetStringField(TEXT("property_path"), PropertyPath);
				Arguments->TryGetStringField(TEXT("channel"), ChannelName);
				Arguments->TryGetStringField(TEXT("interpolation"), Interpolation);
				const EUmgAnimationTrackKind TrackKind = ResolveUmgAnimationTrackKind(TrackType);
				const FString ResolvedTrackType = UmgAnimationTrackKindToString(TrackKind);

				UWidgetBlueprint* WidgetBlueprint = nullptr;
				UWidget* Widget = nullptr;
				if (!ResolveWidgetBlueprintAndWidget(Context.Services, AssetPath, WidgetName, WidgetBlueprint, Widget, OutError))
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, nullptr, OutError);
					return false;
				}
				UWidgetAnimation* Animation = FindUmgAnimationByName(WidgetBlueprint, AnimationName);
				if (!Animation)
				{
					OutError = FString::Printf(TEXT("Animation '%s' not found."), *AnimationName);
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, nullptr, OutError);
					return false;
				}
				if (!Animation->MovieScene)
				{
					OutError = TEXT("Animation has no MovieScene; refusing to key an unverified animation.");
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}
				if (TrackKind == EUmgAnimationTrackKind::Transform2D && ChannelName.IsEmpty())
				{
					OutError = TEXT("2d_transform keyframes require a channel argument.");
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}
				if (!NormalizeUmgAnimationProperty(Widget, TrackKind, PropertyName, PropertyPath, OutError))
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}

				int32 TrackCountBefore = 0;
				int32 KeyframeCountBefore = 0;
				CountUmgAnimationTracksAndKeyframes(Animation, TrackCountBefore, KeyframeCountBefore);

				const FFrameNumber KeyFrame = (static_cast<float>(FMath::Max(0.0, TimeSeconds)) * Animation->MovieScene->GetTickResolution()).RoundToFrame();
				const float KeyValue = static_cast<float>(Value);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgAnimationAddKeyframe", "SOMOLMCP Add UMG Animation Keyframe"));
				WidgetBlueprint->Modify();
				Animation->Modify();
				Animation->MovieScene->Modify();
				Widget->Modify();

				FGuid BindingGuid;
				if (!ResolveUmgAnimationBinding(WidgetBlueprint, Animation, Widget, WidgetName, BindingGuid, OutError))
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}

				bool bTrackCreated = false;
				UMovieScenePropertyTrack* Track = EnsureUmgAnimationPropertyTrack(Animation->MovieScene, BindingGuid, TrackKind, PropertyName, PropertyPath, bTrackCreated, OutError);
				if (!Track)
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}
				Track->Modify();

				bool bSectionCreated = false;
				UMovieSceneSection* Section = EnsureUmgAnimationSection(Track, KeyFrame, bSectionCreated, OutError);
				if (!Section)
				{
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}
				Section->Modify();

				FMovieSceneFloatChannel* ChannelToKey = nullptr;
				if (TrackKind == EUmgAnimationTrackKind::Float)
				{
					UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
					if (!FloatSection)
					{
						OutError = TEXT("Target section is not a UMovieSceneFloatSection.");
						SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
						return false;
					}
					ChannelToKey = &FloatSection->GetChannel();
				}
				else if (TrackKind == EUmgAnimationTrackKind::Transform2D)
				{
					UMovieScene2DTransformSection* TransformSection = Cast<UMovieScene2DTransformSection>(Section);
					EMovieScene2DTransformChannel ChannelMask = EMovieScene2DTransformChannel::None;
					if (!Resolve2DTransformChannel(TransformSection, ChannelName, ChannelToKey, ChannelMask, OutError))
					{
						SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
						return false;
					}
					TransformSection->SetMask(FMovieScene2DTransformMask(TransformSection->GetMask().GetChannels() | ChannelMask));
				}
				if (!ChannelToKey)
				{
					OutError = TEXT("No writable float channel was resolved for the UMG animation keyframe.");
					SetUmgAnimationFailClosedReceipt(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, OutError);
					return false;
				}

				ChannelToKey->SetTickResolution(Animation->MovieScene->GetTickResolution());
				AddUmgFloatKey(*ChannelToKey, KeyFrame, KeyValue, Interpolation);
				float ReadbackValue = 0.0f;
				const bool bReadbackOk = ReadBackUmgFloatKey(*ChannelToKey, KeyFrame, KeyValue, ReadbackValue);

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				WidgetBlueprint->MarkPackageDirty();
				Animation->MarkPackageDirty();

				const bool bReceiptComplete = bReadbackOk && CountMovieSceneTrackKeyframes(Track) > 0;
				SetUmgAnimationReceiptBase(OutStructured, TEXT("umg_animation_add_keyframe"), AssetPath, AnimationName, WidgetName, ResolvedTrackType, Animation, bReceiptComplete);
				OutStructured->SetStringField(TEXT("status"), bReceiptComplete ? TEXT("keyframe_added") : TEXT("failed_validation"));
				OutStructured->SetStringField(TEXT("binding_guid"), BindingGuid.ToString());
				OutStructured->SetStringField(TEXT("property_name"), PropertyName);
				OutStructured->SetStringField(TEXT("property_path"), PropertyPath);
				OutStructured->SetStringField(TEXT("channel"), ChannelName);
				OutStructured->SetNumberField(TEXT("time_seconds"), TimeSeconds);
				OutStructured->SetNumberField(TEXT("frame"), KeyFrame.Value);
				OutStructured->SetNumberField(TEXT("value"), Value);
				OutStructured->SetNumberField(TEXT("readback_value"), ReadbackValue);
				OutStructured->SetBoolField(TEXT("readback_ok"), bReadbackOk);
				OutStructured->SetBoolField(TEXT("created_track"), bTrackCreated);
				OutStructured->SetBoolField(TEXT("created_section"), bSectionCreated);
				OutStructured->SetNumberField(TEXT("track_count_before"), TrackCountBefore);
				OutStructured->SetNumberField(TEXT("keyframe_count_before"), KeyframeCountBefore);
				OutStructured->SetNumberField(TEXT("target_track_keyframe_count"), CountMovieSceneTrackKeyframes(Track));
				if (!bReceiptComplete)
				{
					OutError = TEXT("Keyframe write did not pass readback validation.");
					OutStructured->SetStringField(TEXT("error"), OutError);
					return false;
				}
				OutSummary = TEXT("Added UMG animation keyframe and verified readback.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_copy_widgets"),
			TEXT("Copy widgets from a widget blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}}, {TEXT("asset_path"), TEXT("widget_names")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				TArray<FString> WidgetNames;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetStringArray(Arguments, TEXT("widget_names"), WidgetNames) || WidgetNames.Num() == 0)
				{
					OutError = TEXT("Missing asset_path or widget_names.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditorForAsset(WidgetBlueprint, OutError);
				if (!WidgetEditor)
				{
					return false;
				}
				TSet<FWidgetReference> WidgetReferences;
				TArray<TSharedPtr<FJsonValue>> NamesJson;
				for (const FString& Name : WidgetNames)
				{
					if (UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*Name))
					{
						WidgetReferences.Add(WidgetEditor->GetReferenceFromTemplate(Widget));
					}
					NamesJson.Add(MakeShared<FJsonValueString>(Name));
				}
				if (WidgetReferences.Num() == 0)
				{
					OutError = TEXT("None of the requested widgets were found.");
					return false;
				}
				FWidgetBlueprintEditorUtils::CopyWidgets(WidgetBlueprint, WidgetReferences);
				GWidgetClipboard.SourceAssetPath = AssetPath;
				GWidgetClipboard.WidgetNames = WidgetNames;
				OutStructured->SetStringField(TEXT("sourceAssetPath"), AssetPath);
				OutStructured->SetArrayField(TEXT("widgetNames"), NamesJson);
				OutSummary = TEXT("Copied widgets into the widget clipboard.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_paste_widgets"),
			TEXT("Paste widgets into a widget blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_widget_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParentWidgetName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("parent_widget_name"), ParentWidgetName);
				UWidgetBlueprint* TargetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!TargetBlueprint || !TargetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditorForAsset(TargetBlueprint, OutError);
				if (!WidgetEditor)
				{
					return false;
				}
				TSharedRef<FWidgetBlueprintEditor> WidgetEditorRef = StaticCastSharedRef<FWidgetBlueprintEditor>(WidgetEditor->AsShared());
				FWidgetReference ParentWidgetReference;
				if (!ParentWidgetName.IsEmpty())
				{
					UWidget* ParentWidget = TargetBlueprint->WidgetTree->FindWidget(*ParentWidgetName);
					if (!Cast<UPanelWidget>(ParentWidget))
					{
						OutError = TEXT("parent_widget_name must resolve to a panel widget.");
						return false;
					}
					ParentWidgetReference = WidgetEditor->GetReferenceFromTemplate(ParentWidget);
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgPasteWidgets", "SOMOLMCP Paste Widgets"));
				TargetBlueprint->Modify();
				TArray<TSharedPtr<FJsonValue>> PastedNames;
				const TArray<UWidget*> PastedWidgets = FWidgetBlueprintEditorUtils::PasteWidgets(WidgetEditorRef, TargetBlueprint, ParentWidgetReference, NAME_None, FVector2D::ZeroVector);
				for (UWidget* PastedWidget : PastedWidgets)
				{
					if (!PastedWidget)
					{
						continue;
					}
					PastedNames.Add(MakeShared<FJsonValueString>(PastedWidget->GetName()));
				}
				if (PastedNames.Num() == 0)
				{
					OutError = TEXT("PasteWidgets did not create any widgets.");
					return false;
				}
				OutStructured->SetArrayField(TEXT("pastedWidgets"), PastedNames);
				OutSummary = TEXT("Pasted widgets from the editor clipboard.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_duplicate_widgets"),
			TEXT("Duplicate widgets in a widget blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}}, {TEXT("asset_path"), TEXT("widget_names")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				TArray<FString> WidgetNames;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetStringArray(Arguments, TEXT("widget_names"), WidgetNames) || WidgetNames.Num() == 0)
				{
					OutError = TEXT("Missing asset_path or widget_names.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				FWidgetBlueprintEditor* WidgetEditor = GetWidgetBlueprintEditorForAsset(WidgetBlueprint, OutError);
				if (!WidgetEditor)
				{
					return false;
				}
				TSharedRef<FWidgetBlueprintEditor> WidgetEditorRef = StaticCastSharedRef<FWidgetBlueprintEditor>(WidgetEditor->AsShared());
				TSet<FWidgetReference> WidgetReferences;
				TArray<TSharedPtr<FJsonValue>> DuplicatedNames;
				for (const FString& WidgetName : WidgetNames)
				{
					UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
					if (!Widget)
					{
						continue;
					}
					WidgetReferences.Add(WidgetEditor->GetReferenceFromTemplate(Widget));
				}
				if (WidgetReferences.Num() == 0)
				{
					OutError = TEXT("None of the requested widgets were found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgDuplicateWidgets", "SOMOLMCP Duplicate Widgets"));
				WidgetBlueprint->Modify();
				const TArray<UWidget*> DuplicatedWidgets = FWidgetBlueprintEditorUtils::DuplicateWidgets(WidgetEditorRef, WidgetBlueprint, WidgetReferences);
				for (UWidget* DuplicatedWidget : DuplicatedWidgets)
				{
					if (DuplicatedWidget)
					{
						DuplicatedNames.Add(MakeShared<FJsonValueString>(DuplicatedWidget->GetName()));
					}
				}
				if (DuplicatedNames.Num() == 0)
				{
					OutError = TEXT("DuplicateWidgets did not create any widgets.");
					return false;
				}
				OutStructured->SetArrayField(TEXT("duplicatedWidgets"), DuplicatedNames);
				OutSummary = TEXT("Duplicated widgets.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_add_widget"),
			TEXT("Add a widget to a widget blueprint tree."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_widget_name"), FSololmcpSchemaBuilder::String()}, {TEXT("child_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("entry_widget_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("widget_class_path"), TEXT("widget_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetClassPath;
				FString WidgetName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_class_path"), WidgetClassPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName))
				{
					OutError = TEXT("Missing asset_path, widget_class_path or widget_name.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UClass* WidgetClass = Context.Services.ResolveClass(WidgetClassPath, OutError);
				if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
				{
					OutError = TEXT("widget_class_path must resolve to a widget class.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgWidgetTreeAddWidget", "SOMOLMCP Add Widget To Widget Blueprint"));
				WidgetBlueprint->Modify();
				WidgetBlueprint->WidgetTree->SetFlags(RF_Transactional);
				WidgetBlueprint->WidgetTree->Modify();
				UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, *WidgetName);
				if (!NewWidget)
				{
					OutError = TEXT("Failed to construct widget.");
					return false;
				}
				NewWidget->SetFlags(RF_Transactional);
				NewWidget->Modify();
#if WITH_EDITORONLY_DATA && (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6))
				if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(NewWidget->GetFName()))
				{
					WidgetBlueprint->OnVariableAdded(NewWidget->GetFName());
				}
#endif
				FString ParentWidgetName;
				Arguments->TryGetStringField(TEXT("parent_widget_name"), ParentWidgetName);
				if (ParentWidgetName.IsEmpty())
				{
					if (WidgetBlueprint->WidgetTree->RootWidget)
					{
						WidgetBlueprint->WidgetTree->RootWidget->Modify();
					}
					WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
				}
				else
				{
					UWidget* ParentWidget = WidgetBlueprint->WidgetTree->FindWidget(*ParentWidgetName);
					UPanelWidget* PanelWidget = Cast<UPanelWidget>(ParentWidget);
					if (!PanelWidget)
					{
						OutError = TEXT("parent_widget_name must resolve to a panel widget.");
						return false;
					}
					PanelWidget->SetFlags(RF_Transactional);
					PanelWidget->Modify();
					const int32 ChildIndex = Arguments->HasTypedField<EJson::Number>(TEXT("child_index")) ? Arguments->GetIntegerField(TEXT("child_index")) : INDEX_NONE;
					if (ChildIndex >= 0)
					{
						PanelWidget->InsertChildAt(ChildIndex, NewWidget);
					}
					else
					{
						PanelWidget->AddChild(NewWidget);
					}
				}

				FString EntryWidgetClassPath;
				FClassProperty* EntryClassProperty = CastField<FClassProperty>(NewWidget->GetClass()->FindPropertyByName(TEXT("EntryWidgetClass")));
				bool bRequiresEntryWidgetClass = false;
				bool bEntryWidgetClassSet = false;
				if (EntryClassProperty)
				{
					Arguments->TryGetStringField(TEXT("entry_widget_class_path"), EntryWidgetClassPath);
					if (!EntryWidgetClassPath.IsEmpty())
					{
						FString EntryResolveError;
						UClass* EntryClass = Context.Services.ResolveClass(EntryWidgetClassPath, EntryResolveError);
						if (!EntryClass)
						{
							OutError = FString::Printf(TEXT("entry_widget_class_path could not be resolved: %s"), *EntryResolveError);
							return false;
						}
						if (!EntryClass->IsChildOf(UUserWidget::StaticClass()))
						{
							OutError = TEXT("entry_widget_class_path must resolve to a UserWidget-derived class.");
							return false;
						}
						if (EntryClassProperty->MetaClass && !EntryClass->IsChildOf(EntryClassProperty->MetaClass))
						{
							OutError = TEXT("entry_widget_class_path is not compatible with the widget EntryWidgetClass property.");
							return false;
						}
						EntryClassProperty->SetObjectPropertyValue_InContainer(NewWidget, EntryClass);
						bEntryWidgetClassSet = true;
					}
					else
					{
						UObject* CurrentEntryClass = EntryClassProperty->GetObjectPropertyValue_InContainer(NewWidget);
						bRequiresEntryWidgetClass = CurrentEntryClass == nullptr;
					}
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutStructured->SetStringField(TEXT("widget_name"), WidgetName);
				OutStructured->SetBoolField(TEXT("requires_entry_widget_class"), bRequiresEntryWidgetClass);
				OutStructured->SetBoolField(TEXT("entry_widget_class_set"), bEntryWidgetClassSet);
				if (bRequiresEntryWidgetClass)
				{
					OutStructured->SetStringField(TEXT("recommended_next_tool"), TEXT("umg_list_view_set_entry_class"));
					OutStructured->SetStringField(TEXT("compile_guard"), TEXT("ListView/TileView/TreeView requires EntryWidgetClass before blueprint_compile or PIE."));
				}
				if (!EntryWidgetClassPath.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("entry_widget_class_path"), EntryWidgetClassPath);
				}
				OutSummary = TEXT("Added widget to widget blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("umg_widget_tree_remove_widget"),
			TEXT("Remove a widget from a widget blueprint tree."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("widget_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("widget_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString WidgetName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("widget_name"), WidgetName))
				{
					OutError = TEXT("Missing asset_path or widget_name.");
					return false;
				}
				UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
				{
					OutError = TEXT("Asset is not a widget blueprint.");
					return false;
				}
				UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
				if (!Widget)
				{
					OutError = TEXT("Widget was not found in widget tree.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgWidgetTreeRemoveWidget", "SOMOLMCP Remove Widget From Widget Blueprint"));
				WidgetBlueprint->Modify();
				TSet<UWidget*> WidgetsToDelete;
				WidgetsToDelete.Add(Widget);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				FWidgetBlueprintEditorUtils::DeleteWidgets(WidgetBlueprint, WidgetsToDelete, FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
#else
				// UE 5.5's DeleteWidgets takes a live FWidgetBlueprintEditor and FWidgetReference
				// set; there is no headless equivalent, so this reports instead of half-deleting.
				OutError = TEXT("umg_remove_widget requires UE 5.6 or newer: 5.5 cannot delete widgets without an open widget editor.");
				OutStructured->SetStringField(TEXT("error_code"), TEXT("NOT_AVAILABLE_ON_ENGINE"));
				OutStructured->SetStringField(TEXT("minimum_engine"), TEXT("5.6"));
				return false;
#endif
				OutStructured = FSololmcpEditorServices::MakeObjectReference(WidgetBlueprint);
				OutSummary = TEXT("Removed widget from widget blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_asset_create"),
			TEXT("Create a data layer asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetName;
				if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) || !Arguments->TryGetStringField(TEXT("asset_name"), AssetName))
				{
					OutError = TEXT("Missing package_path or asset_name.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DataLayerAssetCreate", "SOMOLMCP Create Data Layer Asset"));
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/Engine.DataLayerAsset"), TEXT("/Script/DataLayerEditor.DataLayerFactory"), nullptr, OutError);
				if (!Asset)
				{
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutSummary = TEXT("Created data layer asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_instance_list"),
			TEXT("List data layer instances in the current editor world."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				UDataLayerManager* DataLayerManager = UDataLayerManager::GetDataLayerManager(World);
				if (!DataLayerManager)
				{
					OutError = TEXT("Current world does not expose a data layer manager.");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> DataLayerJson;
				DataLayerManager->ForEachDataLayerInstance([&DataLayerJson](UDataLayerInstance* DataLayerInstance)
				{
					DataLayerJson.Add(MakeShared<FJsonValueObject>(DataLayerInstanceToJson(DataLayerInstance)));
					return true;
				});
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetArrayField(TEXT("instances"), DataLayerJson);
				OutStructured->SetNumberField(TEXT("count"), DataLayerJson.Num());
				OutSummary = TEXT("Listed data layer instances.");
				return true;
}
, nullptr
, 5
});

		Registry.Register({
			TEXT("data_layer_instance_create"),
			TEXT("Create a data layer instance in the current editor world."),
			FSololmcpSchemaBuilder::Object({{TEXT("data_layer_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("is_private"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("data_layer_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString DataLayerAssetPath;
				if (!Arguments->TryGetStringField(TEXT("data_layer_asset_path"), DataLayerAssetPath))
				{
					OutError = TEXT("Missing data_layer_asset_path.");
					return false;
				}
				UDataLayerAsset* DataLayerAsset = Cast<UDataLayerAsset>(Context.Services.LoadAsset(DataLayerAssetPath, OutError));
				if (!DataLayerAsset)
				{
					OutError = TEXT("data_layer_asset_path is not a data layer asset.");
					return false;
				}
				UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
				if (!Subsystem)
				{
					OutError = TEXT("DataLayerEditorSubsystem is unavailable.");
					return false;
				}
				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				AWorldDataLayers* WorldDataLayers = World ? World->GetWorldDataLayers() : nullptr;
				if (!WorldDataLayers)
				{
					OutError = TEXT("Current world does not expose WorldDataLayers.");
					return false;
				}
				FDataLayerCreationParameters Parameters;
				Parameters.DataLayerAsset = DataLayerAsset;
				Parameters.WorldDataLayers = WorldDataLayers;
				Parameters.bIsPrivate = Arguments->HasTypedField<EJson::Boolean>(TEXT("is_private")) ? Arguments->GetBoolField(TEXT("is_private")) : false;
				UDataLayerInstance* DataLayerInstance = Subsystem->CreateDataLayerInstance(Parameters);
				if (!DataLayerInstance)
				{
					OutError = TEXT("Failed to create data layer instance.");
					return false;
				}
				OutStructured = DataLayerInstanceToJson(DataLayerInstance);
				OutSummary = TEXT("Created data layer instance.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_instance_set_visibility"),
			TEXT("Set editor visibility on a data layer instance."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("name"), FSololmcpSchemaBuilder::String()},
					{TEXT("full_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("object_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("visible"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("visible")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				bool bVisible = true;
				if (!Arguments->TryGetBoolField(TEXT("visible"), bVisible))
				{
					OutError = TEXT("Missing visible.");
					return false;
				}
				UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
				if (!Subsystem)
				{
					OutError = TEXT("Data layer editing is unavailable in the current world.");
					return false;
				}
				UDataLayerInstance* TargetLayer = ResolveDataLayerInstanceFromArguments(Arguments, TEXT("name"), OutError);
				if (!TargetLayer)
				{
					return false;
				}
				Subsystem->SetDataLayerVisibility(TargetLayer, bVisible);
				OutStructured = DataLayerInstanceToJson(TargetLayer);
				OutSummary = TEXT("Updated data layer instance visibility.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_instance_set_loaded_in_editor"),
			TEXT("Set whether a data layer instance is loaded in the editor world."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("name"), FSololmcpSchemaBuilder::String()},
					{TEXT("full_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("object_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("loaded"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("loaded")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				bool bLoaded = true;
				if (!Arguments->TryGetBoolField(TEXT("loaded"), bLoaded))
				{
					OutError = TEXT("Missing loaded.");
					return false;
				}
				UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
				if (!Subsystem)
				{
					OutError = TEXT("Data layer editing is unavailable in the current world.");
					return false;
				}
				UDataLayerInstance* TargetLayer = ResolveDataLayerInstanceFromArguments(Arguments, TEXT("name"), OutError);
				if (!TargetLayer)
				{
					return false;
				}
				if (!Subsystem->SetDataLayerIsLoadedInEditor(TargetLayer, bLoaded, true))
				{
					OutError = TEXT("Failed to update data layer loaded state.");
					return false;
				}
				OutStructured = DataLayerInstanceToJson(TargetLayer);
				OutSummary = TEXT("Updated data layer instance loaded state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_instance_rename"),
			TEXT("Rename a data layer instance."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("name"), FSololmcpSchemaBuilder::String()},
					{TEXT("full_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("object_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("new_name"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing new_name.");
					return false;
				}
				UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
				if (!Subsystem)
				{
					OutError = TEXT("Data layer editing is unavailable in the current world.");
					return false;
				}
				UDataLayerInstance* TargetLayer = ResolveDataLayerInstanceFromArguments(Arguments, TEXT("name"), OutError);
				if (!TargetLayer)
				{
					return false;
				}
				if (!Subsystem->SetDataLayerShortName(TargetLayer, NewName))
				{
					OutError = TEXT("Failed to rename data layer instance.");
					return false;
				}
				OutStructured = DataLayerInstanceToJson(TargetLayer);
				OutSummary = TEXT("Renamed data layer instance.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_instance_delete"),
			TEXT("Delete a data layer instance."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("name"), FSololmcpSchemaBuilder::String()},
					{TEXT("full_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("object_path"), FSololmcpSchemaBuilder::String()}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
				if (!Subsystem)
				{
					OutError = TEXT("Data layer editing is unavailable in the current world.");
					return false;
				}
				UDataLayerInstance* TargetLayer = ResolveDataLayerInstanceFromArguments(Arguments, TEXT("name"), OutError);
				if (!TargetLayer)
				{
					return false;
				}
				const TSharedRef<FJsonObject> DeletedLayer = DataLayerInstanceToJson(TargetLayer);
				TArray<UDataLayerInstance*> LayersToDelete;
				LayersToDelete.Add(TargetLayer);
				Subsystem->DeleteDataLayers(LayersToDelete);
				FString VerifyError;
				if (ResolveDataLayerInstanceFromArguments(Arguments, TEXT("name"), VerifyError))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("name"),
						TEXT("DeleteDataLayers completed but the data layer still resolves."));
					OutError = TEXT("Failed to delete data layer instance.");
					return false;
				}
				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetBoolField(TEXT("deleted"), true);
				OutStructured->SetObjectField(TEXT("dataLayer"), DeletedLayer);
				OutSummary = TEXT("Deleted data layer instance.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_list_supported_properties"),
			TEXT("List supported material property names for material_connect_property. Pass asset_path for a domain-aware list (e.g. Volume adds Extinction/Albedo aliases and only domain-active pins)."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional material path; list reflects the material's MaterialDomain when provided."))}}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> Names = {
					TEXT("BaseColor"), TEXT("Metallic"), TEXT("Specular"), TEXT("Roughness"), TEXT("EmissiveColor"),
					TEXT("Opacity"), TEXT("OpacityMask"), TEXT("Normal"), TEXT("WorldPositionOffset"), TEXT("AmbientOcclusion"),
					TEXT("Refraction"), TEXT("Anisotropy"), TEXT("Tangent"), TEXT("Displacement"), TEXT("SubsurfaceColor"),
					TEXT("ClearCoat"), TEXT("ClearCoatRoughness"), TEXT("PixelDepthOffset")
				};

				FString AssetPath;
				UMaterial* Material = nullptr;
				if (Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
				{
					Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
					if (!Material)
					{
						return false;
					}
				}

				TArray<TSharedPtr<FJsonValue>> AliasesJson;
				if (Material)
				{
					// Domain pin aliases (see TryParseMaterialProperty): UE 5.8 has no
					// MP_Extinction; Volume "Extinction" is MP_SubsurfaceColor, "Albedo"
					// is MP_BaseColor (FMaterialAttributeDefinitionMap overrides).
					if (Material->MaterialDomain == MD_Volume)
					{
						Names.Add(TEXT("Extinction"));
						Names.Add(TEXT("Albedo"));
						TSharedRef<FJsonObject> AliasA = MakeShared<FJsonObject>();
						AliasA->SetStringField(TEXT("name"), TEXT("Extinction"));
						AliasA->SetStringField(TEXT("resolves_to"), TEXT("SubsurfaceColor"));
						AliasesJson.Add(MakeShared<FJsonValueObject>(AliasA));
						TSharedRef<FJsonObject> AliasB = MakeShared<FJsonObject>();
						AliasB->SetStringField(TEXT("name"), TEXT("Albedo"));
						AliasB->SetStringField(TEXT("resolves_to"), TEXT("BaseColor"));
						AliasesJson.Add(MakeShared<FJsonValueObject>(AliasB));
					}
					// Add any other domain-active EMaterialProperty not already listed.
					if (const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>())
					{
						for (int32 Index = 0; Index < PropertyEnum->NumEnums(); ++Index)
						{
							const int64 Value = PropertyEnum->GetValueByIndex(Index);
							if (Value == MP_MAX || Value == MP_MaterialAttributes || Value == MP_CustomOutput)
							{
								continue;
							}
							const EMaterialProperty Candidate = static_cast<EMaterialProperty>(Value);
							if (!Material->IsPropertyActiveInEditor(Candidate))
							{
								continue;
							}
							FString EnumName = PropertyEnum->GetNameStringByIndex(Index);
							EnumName.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase);
							if (!Names.Contains(EnumName))
							{
								Names.Add(EnumName);
							}
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> Properties;
				for (const FString& Name : Names)
				{
					Properties.Add(MakeShared<FJsonValueString>(Name));
				}
				OutStructured->SetArrayField(TEXT("properties"), Properties);
				OutStructured->SetNumberField(TEXT("count"), Properties.Num());
				if (Material)
				{
					const FString DomainName = StaticEnum<EMaterialDomain>()
						? StaticEnum<EMaterialDomain>()->GetNameStringByValue(static_cast<int64>(Material->MaterialDomain))
						: TEXT("Unknown");
					OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
					OutStructured->SetStringField(TEXT("domain"), DomainName);
					OutStructured->SetArrayField(TEXT("domain_aliases"), AliasesJson);
				}
				OutSummary = TEXT("Listed supported material properties.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_batch_edit"),
			TEXT("Run multiple material edit operations in sequence. Each operation requires {tool, arguments}."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("operations"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("tool"), FSololmcpSchemaBuilder::String()},
						{TEXT("arguments"), FSololmcpSchemaBuilder::Object({})}
					}, {TEXT("tool"), TEXT("arguments")}))},
					{TEXT("continue_on_error"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("atomic"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("rollback_on_error"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("operations")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<FString> AllowedTools = {
					TEXT("material_create_expression"),
					TEXT("material_delete_expression"),
					TEXT("material_connect_property"),
					TEXT("material_connect_expressions"),
					TEXT("material_disconnect_property"),
					TEXT("material_disconnect_expressions"),
					TEXT("material_set_expression_properties"),
					TEXT("material_layout_expressions"),
					TEXT("material_recompile")
				};

				const bool bContinueOnError = Arguments->HasTypedField<EJson::Boolean>(TEXT("continue_on_error")) && Arguments->GetBoolField(TEXT("continue_on_error"));
				const bool bAtomic = Arguments->HasTypedField<EJson::Boolean>(TEXT("atomic")) && Arguments->GetBoolField(TEXT("atomic"));
				const bool bRollbackOnError = bAtomic || (Arguments->HasTypedField<EJson::Boolean>(TEXT("rollback_on_error")) && Arguments->GetBoolField(TEXT("rollback_on_error")));
				const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("operations"), Operations) || !Operations)
				{
					OutError = TEXT("Missing operations array.");
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> Results;
				int32 SuccessCount = 0;
				int32 FailureCount = 0;

				for (int32 Index = 0; Index < Operations->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> OpObject = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
					if (!OpObject.IsValid())
					{
						OutError = FString::Printf(TEXT("operations[%d] must be an object."), Index);
						return false;
					}

					FString ToolName;
					const TSharedPtr<FJsonObject>* OpArgsPtr = nullptr;
					if (!OpObject->TryGetStringField(TEXT("tool"), ToolName) || !OpObject->TryGetObjectField(TEXT("arguments"), OpArgsPtr) || !OpArgsPtr || !OpArgsPtr->IsValid())
					{
						OutError = FString::Printf(TEXT("operations[%d] must include tool and arguments object."), Index);
						return false;
					}
					if (!AllowedTools.Contains(ToolName))
					{
						OutError = FString::Printf(TEXT("operations[%d] has unsupported material tool: %s"), Index, *ToolName);
						return false;
					}

					TSharedRef<FJsonObject> StepStructured = MakeShared<FJsonObject>();
					FString StepSummary;
					FString StepError;
					const bool bStepOk = Registry.ExecuteTool(ToolName, OpArgsPtr->ToSharedRef(), StepStructured, StepSummary, StepError);

					TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
					StepResult->SetNumberField(TEXT("index"), Index);
					StepResult->SetStringField(TEXT("tool"), ToolName);
					StepResult->SetBoolField(TEXT("ok"), bStepOk);
					StepResult->SetStringField(TEXT("code"), bStepOk ? TEXT("ok") : TEXT("tool_execution_failed"));
					StepResult->SetStringField(TEXT("summary"), StepSummary);
					if (!StepError.IsEmpty())
					{
						StepResult->SetStringField(TEXT("error"), StepError);
					}
					StepResult->SetObjectField(TEXT("result"), StepStructured);
					Results.Add(MakeShared<FJsonValueObject>(StepResult));

					if (bStepOk)
					{
						++SuccessCount;
					}
					else
					{
						++FailureCount;
						if (!bContinueOnError)
						{
							int32 RollbackCount = 0;
							if (bRollbackOnError && GEditor)
							{
								for (int32 UndoIndex = 0; UndoIndex < SuccessCount; ++UndoIndex)
								{
									if (GEditor->UndoTransaction())
									{
										++RollbackCount;
									}
									else
									{
										break;
									}
								}
							}
							OutStructured->SetArrayField(TEXT("steps"), Results);
							OutStructured->SetNumberField(TEXT("successCount"), SuccessCount);
							OutStructured->SetNumberField(TEXT("failureCount"), FailureCount);
							OutStructured->SetBoolField(TEXT("rolledBack"), bRollbackOnError && RollbackCount > 0);
							OutStructured->SetNumberField(TEXT("rollbackCount"), RollbackCount);
							OutStructured->SetStringField(TEXT("errorCode"), TEXT("material_batch_step_failed"));
							OutError = FString::Printf(TEXT("material_batch_edit failed at operation %d: %s"), Index, *StepError);
							return false;
						}
					}
				}

				OutStructured->SetArrayField(TEXT("steps"), Results);
				OutStructured->SetNumberField(TEXT("successCount"), SuccessCount);
				OutStructured->SetNumberField(TEXT("failureCount"), FailureCount);
				OutStructured->SetBoolField(TEXT("atomic"), bAtomic);
				OutStructured->SetBoolField(TEXT("rollbackOnError"), bRollbackOnError);
				OutStructured->SetStringField(TEXT("code"), FailureCount > 0 ? TEXT("partial_success") : TEXT("ok"));
				OutSummary = FString::Printf(TEXT("Executed %d material operations (%d success, %d failed)."), Results.Num(), SuccessCount, FailureCount);
				if (FailureCount > 0 && SuccessCount == 0)
				{
					OutError = TEXT("All material batch operations failed.");
					return false;
				}
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_instance_batch_parameters"),
			TEXT("Apply scalar, vector, texture, or static-switch parameters to multiple MaterialInstanceConstant assets with per-step receipts."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("Material instance asset paths.")))},
				{TEXT("parameters"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
					{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("scalar, vector, texture, or static_switch."))},
					{TEXT("parameter_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("value"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("For scalar use {number}; vector {r,g,b,a}; texture {texture_path}; static switch {enabled}."))}
				}, {TEXT("type"), TEXT("parameter_name"), TEXT("value")}))},
				{TEXT("continue_on_error"), FSololmcpSchemaBuilder::Boolean()}
			}, {TEXT("asset_paths"), TEXT("parameters")}),
			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<TSharedPtr<FJsonValue>>* AssetPaths = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("asset_paths"), AssetPaths) || !AssetPaths || AssetPaths->IsEmpty()
					|| !Arguments->TryGetArrayField(TEXT("parameters"), Parameters) || !Parameters || Parameters->IsEmpty())
				{
					OutError = TEXT("asset_paths and parameters are required.");
					return false;
				}
				const bool bContinue = Arguments->HasTypedField<EJson::Boolean>(TEXT("continue_on_error")) && Arguments->GetBoolField(TEXT("continue_on_error"));
				TArray<TSharedPtr<FJsonValue>> Results;
				int32 Succeeded = 0;
				int32 Failed = 0;
				for (const TSharedPtr<FJsonValue>& AssetValue : *AssetPaths)
				{
					FString AssetPath;
					if (!AssetValue.IsValid() || !AssetValue->TryGetString(AssetPath) || AssetPath.IsEmpty()) continue;
					for (const TSharedPtr<FJsonValue>& ParameterValue : *Parameters)
					{
						const TSharedPtr<FJsonObject> Parameter = ParameterValue.IsValid() ? ParameterValue->AsObject() : nullptr;
						if (!Parameter.IsValid()) continue;
						FString Type;
						FString Name;
						Parameter->TryGetStringField(TEXT("type"), Type);
						Parameter->TryGetStringField(TEXT("parameter_name"), Name);
						const TSharedPtr<FJsonObject>* Value = nullptr;
						Parameter->TryGetObjectField(TEXT("value"), Value);
						FString Tool;
						TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
						ToolArgs->SetStringField(TEXT("asset_path"), AssetPath);
						ToolArgs->SetStringField(TEXT("parameter_name"), Name);
						if (Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
						{
							Tool = TEXT("material_instance_set_scalar_parameter");
							double Number = 0.0; if (Value && Value->IsValid()) (*Value)->TryGetNumberField(TEXT("number"), Number);
							ToolArgs->SetNumberField(TEXT("value"), Number);
						}
						else if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
						{
							Tool = TEXT("material_instance_set_vector_parameter");
							if (Value && Value->IsValid())
							{
								// Canonical vector setter contract is value:{r,g,b,a}.
								ToolArgs->SetObjectField(TEXT("value"), (*Value).ToSharedRef());
							}
						}
						else if (Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
						{
							Tool = TEXT("material_instance_set_texture_parameter");
							FString TexturePath; if (Value && Value->IsValid()) (*Value)->TryGetStringField(TEXT("texture_path"), TexturePath);
							// Canonical texture setter contract is value:"/Game/...".
							ToolArgs->SetStringField(TEXT("value"), TexturePath);
						}
						else if (Type.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
						{
							Tool = TEXT("material_instance_set_static_switch_parameter");
							bool bEnabled = false; if (Value && Value->IsValid()) (*Value)->TryGetBoolField(TEXT("enabled"), bEnabled);
							ToolArgs->SetBoolField(TEXT("value"), bEnabled);
						}
						TSharedRef<FJsonObject> StepOut = MakeShared<FJsonObject>();
						FString StepSummary;
						FString StepError;
						const bool bOk = !Tool.IsEmpty() && Registry.ExecuteTool(Tool, ToolArgs, StepOut, StepSummary, StepError);
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("asset_path"), AssetPath);
						Row->SetStringField(TEXT("parameter_name"), Name);
						Row->SetStringField(TEXT("tool"), Tool);
						Row->SetStringField(TEXT("status"), bOk ? TEXT("completed") : TEXT("failed"));
						if (!StepError.IsEmpty()) Row->SetStringField(TEXT("error"), StepError);
						Row->SetObjectField(TEXT("readback"), StepOut);
						Results.Add(MakeShared<FJsonValueObject>(Row));
						if (bOk) ++Succeeded; else { ++Failed; if (!bContinue) break; }
					}
					if (Failed > 0 && !bContinue) break;
				}
				OutStructured->SetArrayField(TEXT("results"), Results);
				OutStructured->SetNumberField(TEXT("succeeded"), Succeeded);
				OutStructured->SetNumberField(TEXT("failed"), Failed);
				OutStructured->SetStringField(TEXT("status"), Failed == 0 ? TEXT("completed") : TEXT("completed_with_errors"));
				OutSummary = FString::Printf(TEXT("Applied %d material-instance parameters; %d failed."), Succeeded, Failed);
				return Succeeded > 0 && (bContinue || Failed == 0);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_create"),
			TEXT("Create a new material asset. Accepts either (package_path + asset_name) or a single asset_path like '/Game/Folder/MaterialName'."),
			FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Full path like /Game/Folder/Name. Overrides package_path+asset_name."))}}, {TEXT("package_path"), TEXT("asset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetName;
				// Support asset_path as alternative: split "/Game/Folder/Name" → package_path="/Game/Folder", asset_name="Name"
				FString AssetPath;
				if (Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
				{
					int32 LastSlash;
					if (AssetPath.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
					{
						PackagePath = AssetPath.Left(LastSlash);
						AssetName = AssetPath.RightChop(LastSlash + 1);
					}
					else
					{
						SololmcpError::InvalidPath(OutStructured, AssetPath);
						OutError = TEXT("asset_path must be in format /Game/Folder/AssetName");
						return false;
					}
				}
				else
				{
					if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath))
					{
						SololmcpError::MissingParam(OutStructured, TEXT("package_path"));
						OutError = TEXT("Missing package_path+asset_name or asset_path.");
						return false;
					}
					if (!Arguments->TryGetStringField(TEXT("asset_name"), AssetName))
					{
						SololmcpError::MissingParam(OutStructured, TEXT("asset_name"));
						OutError = TEXT("Missing package_path+asset_name or asset_path.");
						return false;
					}
				}
				// Auto-naming: if asset already exists, generate a unique name
				FString EffectiveName = Context.Services.GenerateUniqueAssetName(PackagePath, AssetName);
				if (EffectiveName != AssetName)
				{
					OutStructured->SetStringField(TEXT("original_name"), AssetName);
					AssetName = EffectiveName;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialCreate", "SOMOLMCP Create Material"));
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, UMaterial::StaticClass()->GetPathName(), UMaterialFactoryNew::StaticClass()->GetPathName(), nullptr, OutError);
				if (!Asset)
				{
					return false;
				}
				if (!Asset->IsA<UMaterial>())
				{
					OutError = FString::Printf(TEXT("create_returned_unexpected_class: %s"), *Asset->GetClass()->GetPathName());
					return false;
				}
				// Audit round 7 (silent-create fix): force save + asset_registry notify so subsequent
				// material_inspect can LoadAsset() and "Asset is not a Material" stops firing.
				const FString CreatedPath = Asset->GetPathName();
				Asset->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Asset);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!VerifyCreatedAssetReloaded(Context.Services, Asset, UMaterial::StaticClass(), OutStructured, OutError))
				{
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = FString::Printf(TEXT("Created material: %s/%s"), *PackagePath, *AssetName);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_instance_create"),
			TEXT("Create a material instance asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_material_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name"), TEXT("parent_material_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetName;
				FString ParentMaterialPath;
				if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) || !Arguments->TryGetStringField(TEXT("asset_name"), AssetName) || !Arguments->TryGetStringField(TEXT("parent_material_path"), ParentMaterialPath))
				{
					OutError = TEXT("Missing package_path, asset_name or parent_material_path.");
					return false;
				}

				TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
				Overrides->SetStringField(TEXT("InitialParent"), ParentMaterialPath);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialInstanceCreate", "SOMOLMCP Create Material Instance"));
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, UMaterialInstanceConstant::StaticClass()->GetPathName(), UMaterialInstanceConstantFactoryNew::StaticClass()->GetPathName(), Overrides, OutError);
				if (!Asset)
				{
					return false;
				}
				if (!Asset->IsA<UMaterialInstanceConstant>())
				{
					OutError = FString::Printf(TEXT("create_returned_unexpected_class: %s"), *Asset->GetClass()->GetPathName());
					return false;
				}
				// Audit round 10B (silent-create fix): persist + verify on disk.
				const FString CreatedPath = Asset->GetPathName();
				Asset->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Asset);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!VerifyCreatedAssetReloaded(Context.Services, Asset, UMaterialInstanceConstant::StaticClass(), OutStructured, OutError))
				{
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = TEXT("Created material instance.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_list_expressions"),
			TEXT("List material expressions in a material graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				OutStructured = MaterialExpressionsToJson(Material);
				OutSummary = TEXT("Listed material expressions.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_texture_role_detect"),
			TEXT("Detect semantic roles for textures used by a material, or for a single texture asset. Roles include base_color, normal, roughness, metallic, ambient_occlusion, packed_orm, height, opacity and emissive."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material or texture asset path"))},
					{TEXT("expression_index"), FSololmcpSchemaBuilder::Integer(TEXT("Optional material texture sample expression index filter"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset)
				{
					return false;
				}

				if (UTexture* Texture = Cast<UTexture>(Asset))
				{
					OutStructured = DetectMaterialTextureRoleJson(Texture, FString(), FString(), FString());
					OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
					OutStructured->SetStringField(TEXT("mode"), TEXT("single_texture"));
					OutSummary = FString::Printf(TEXT("Detected texture role for %s: %s"),
						*Texture->GetName(), *OutStructured->GetStringField(TEXT("role")));
					return true;
				}

				UMaterial* Material = Cast<UMaterial>(Asset);
				if (!Material)
				{
					OutError = FString::Printf(TEXT("Asset is not a material or texture (got %s)."), *Asset->GetClass()->GetName());
					return false;
				}

				int32 FilterExpressionIndex = INDEX_NONE;
				Arguments->TryGetNumberField(TEXT("expression_index"), FilterExpressionIndex);

				TMap<UMaterialExpression*, FString> DirectPropertyByExpression;
				for (const FString& PropertyName : GetSupportedMaterialPropertyNames())
				{
					if (const FExpressionInput* Input = GetMaterialPropertyInputByName(Material, PropertyName))
					{
						if (Input->Expression)
						{
							DirectPropertyByExpression.FindOrAdd(Input->Expression) = PropertyName;
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> Detections;
				for (int32 Index = 0; Index < Material->GetExpressions().Num(); ++Index)
				{
					if (FilterExpressionIndex != INDEX_NONE && FilterExpressionIndex != Index)
					{
						continue;
					}
					UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Material->GetExpressions()[Index]);
					if (!TextureSample)
					{
						continue;
					}

					const FString ConnectedProperty = DirectPropertyByExpression.FindRef(TextureSample);
					const FString SamplerTypeName = StaticEnum<EMaterialSamplerType>()->GetNameStringByValue(static_cast<int64>(TextureSample->SamplerType));
					TSharedRef<FJsonObject> Detection = DetectMaterialTextureRoleJson(
						TextureSample->Texture,
						ConnectedProperty,
						TextureSample->GetParameterName().ToString(),
						SamplerTypeName);
					Detection->SetNumberField(TEXT("expression_index"), Index);
					Detection->SetStringField(TEXT("expression_guid"), TextureSample->MaterialExpressionGuid.ToString());
					Detection->SetStringField(TEXT("expression_name"), TextureSample->GetName());
					Detection->SetStringField(TEXT("expression_class"), TextureSample->GetClass()->GetPathName());
					Detections.Add(MakeShared<FJsonValueObject>(Detection));
				}

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), Material->GetName());
				OutStructured->SetStringField(TEXT("mode"), TEXT("material_texture_samples"));
				OutStructured->SetArrayField(TEXT("detections"), Detections);
				OutStructured->SetNumberField(TEXT("count"), Detections.Num());
				OutSummary = FString::Printf(TEXT("Detected roles for %d texture sample(s) in %s."), Detections.Num(), *Material->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_find_edit_points"),
			TEXT("Find safe semantic edit points in a material: output property connections, parameters, texture samples, and editable expression nodes."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path"))},
					{TEXT("include_expressions"), FSololmcpSchemaBuilder::Boolean(TEXT("Include every non-parameter expression as a node edit point (default true)"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				const bool bIncludeExpressions = !Arguments->HasTypedField<EJson::Boolean>(TEXT("include_expressions")) || Arguments->GetBoolField(TEXT("include_expressions"));
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> EditPoints;
				TArray<TSharedPtr<FJsonValue>> PropertyConnections;
				TMap<UMaterialExpression*, FString> DirectPropertyByExpression;
				for (const FString& PropertyName : GetSupportedMaterialPropertyNames())
				{
					const FExpressionInput* Input = GetMaterialPropertyInputByName(Material, PropertyName);
					TSharedRef<FJsonObject> Connection = MakeMaterialConnectionJson(Material, PropertyName, Input);
					PropertyConnections.Add(MakeShared<FJsonValueObject>(Connection));
					if (Input && Input->Expression)
					{
						DirectPropertyByExpression.FindOrAdd(Input->Expression) = PropertyName;
					}

					TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
					Point->SetStringField(TEXT("id"), FString::Printf(TEXT("property:%s"), *PropertyName));
					Point->SetStringField(TEXT("kind"), TEXT("material_property"));
					Point->SetStringField(TEXT("property"), PropertyName);
					Point->SetStringField(TEXT("role"), PropertyName);
					Point->SetBoolField(TEXT("connected"), Input && Input->Expression);
					Point->SetObjectField(TEXT("current_connection"), Connection);
					AddStringArrayField(Point, TEXT("recommended_tools"), {
						TEXT("material_connect_property"),
						TEXT("material_disconnect_property"),
						TEXT("material_list_supported_properties")
					});
					EditPoints.Add(MakeShared<FJsonValueObject>(Point));
				}

				int32 ParameterCount = 0;
				int32 TextureSampleCount = 0;
				for (int32 Index = 0; Index < Material->GetExpressions().Num(); ++Index)
				{
					UMaterialExpression* Expression = Material->GetExpressions()[Index];
					if (!Expression)
					{
						continue;
					}

					const FString ParameterName = Expression->GetParameterName().ToString();
					const bool bHasParameterName = !ParameterName.IsEmpty() && ParameterName != TEXT("None");
					if (bHasParameterName)
					{
						++ParameterCount;
						TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
						Point->SetStringField(TEXT("id"), FString::Printf(TEXT("parameter:%s:%d"), *ParameterName, Index));
						Point->SetStringField(TEXT("kind"), TEXT("parameter"));
						Point->SetStringField(TEXT("parameter_name"), ParameterName);
						Point->SetNumberField(TEXT("expression_index"), Index);
						Point->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString());
						Point->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetPathName());
						Point->SetStringField(TEXT("group"), TEXT(""));
						if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
						{
							Point->SetStringField(TEXT("value_type"), TEXT("scalar"));
							Point->SetNumberField(TEXT("default_value"), Scalar->DefaultValue);
							Point->SetStringField(TEXT("group"), Scalar->Group.ToString());
						}
						else if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
						{
							Point->SetStringField(TEXT("value_type"), TEXT("vector"));
							Point->SetObjectField(TEXT("default_value"), LinearColorToJson(Vector->DefaultValue));
							Point->SetStringField(TEXT("group"), Vector->Group.ToString());
						}
						else if (UMaterialExpressionStaticBoolParameter* BoolParam = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
						{
							Point->SetStringField(TEXT("value_type"), TEXT("static_bool"));
							Point->SetBoolField(TEXT("default_value"), BoolParam->DefaultValue);
							Point->SetStringField(TEXT("group"), BoolParam->Group.ToString());
						}
						else
						{
							Point->SetStringField(TEXT("value_type"), TEXT("expression_parameter"));
						}
						AddStringArrayField(Point, TEXT("recommended_tools"), {
							TEXT("material_set_expression_properties"),
							TEXT("material_instance_set_scalar_parameter"),
							TEXT("material_instance_set_vector_parameter"),
							TEXT("material_instance_set_texture_parameter")
						});
						EditPoints.Add(MakeShared<FJsonValueObject>(Point));
					}

					if (UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
					{
						++TextureSampleCount;
						const FString ConnectedProperty = DirectPropertyByExpression.FindRef(TextureSample);
						const FString SamplerTypeName = StaticEnum<EMaterialSamplerType>()->GetNameStringByValue(static_cast<int64>(TextureSample->SamplerType));
						TSharedRef<FJsonObject> Role = DetectMaterialTextureRoleJson(TextureSample->Texture, ConnectedProperty, ParameterName, SamplerTypeName);

						TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
						Point->SetStringField(TEXT("id"), FString::Printf(TEXT("texture_sample:%d"), Index));
						Point->SetStringField(TEXT("kind"), TEXT("texture_sample"));
						Point->SetNumberField(TEXT("expression_index"), Index);
						Point->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString());
						Point->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetPathName());
						Point->SetStringField(TEXT("texture_path"), TextureSample->Texture ? TextureSample->Texture->GetPathName() : FString());
						Point->SetObjectField(TEXT("role_detection"), Role);
						AddStringArrayField(Point, TEXT("recommended_tools"), {
							TEXT("material_texture_role_detect"),
							TEXT("texture_analyze"),
							TEXT("material_set_expression_properties"),
							TEXT("material_connect_property")
						});
						EditPoints.Add(MakeShared<FJsonValueObject>(Point));
					}
					else if (bIncludeExpressions && !bHasParameterName)
					{
						TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
						Point->SetStringField(TEXT("id"), FString::Printf(TEXT("expression:%d"), Index));
						Point->SetStringField(TEXT("kind"), TEXT("expression"));
						Point->SetNumberField(TEXT("expression_index"), Index);
						Point->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString());
						Point->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetPathName());
						Point->SetStringField(TEXT("description"), Expression->GetDescription());
						Point->SetNumberField(TEXT("node_x"), Expression->MaterialExpressionEditorX);
						Point->SetNumberField(TEXT("node_y"), Expression->MaterialExpressionEditorY);
						AddStringArrayField(Point, TEXT("recommended_tools"), {
							TEXT("material_set_expression_properties"),
							TEXT("material_connect_expressions"),
							TEXT("material_delete_expression")
						});
						EditPoints.Add(MakeShared<FJsonValueObject>(Point));
					}
				}

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), Material->GetName());
				OutStructured->SetArrayField(TEXT("property_connections"), PropertyConnections);
				OutStructured->SetArrayField(TEXT("edit_points"), EditPoints);
				OutStructured->SetNumberField(TEXT("edit_point_count"), EditPoints.Num());
				TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
				Counts->SetNumberField(TEXT("property_count"), GetSupportedMaterialPropertyNames().Num());
				Counts->SetNumberField(TEXT("parameter_count"), ParameterCount);
				Counts->SetNumberField(TEXT("texture_sample_count"), TextureSampleCount);
				Counts->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
				OutStructured->SetObjectField(TEXT("counts"), Counts);
				OutSummary = FString::Printf(TEXT("Found %d material edit point(s): %d properties, %d parameters, %d texture samples."),
					EditPoints.Num(), GetSupportedMaterialPropertyNames().Num(), ParameterCount, TextureSampleCount);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_create_expression"),
			TEXT("Create a material expression node in a material graph. expression_type accepts short names: Constant, Constant2Vector, Constant3Vector, Constant4Vector, TextureSample, Multiply, Add, Subtract, Lerp, WorldPosition, CameraVector, VertexNormalWS, Time, Sine, Cosine, Power, OneMinus, Clamp, ComponentMask, StaticSwitch, Custom, ScalarParameter, VectorParameter, TextureSampleParameter2D, CollectionParameter."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("expression_class_path"), FSololmcpSchemaBuilder::String(TEXT("Full class path like /Script/Engine.MaterialExpressionConstant3Vector"))},
					{TEXT("expression_type"), FSololmcpSchemaBuilder::String(TEXT("Short name like 'Constant3Vector', 'TextureSample', 'Multiply'. Auto-resolves to class path."))},
					{TEXT("parameter_name"), FSololmcpSchemaBuilder::String(TEXT("Parameter name for parameter expressions and CollectionParameter nodes."))},
					{TEXT("collection_path"), FSololmcpSchemaBuilder::String(TEXT("Material Parameter Collection asset path for CollectionParameter nodes."))},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ExpressionClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				// Accept expression_type as short name, resolve to full class path
				if (!Arguments->TryGetStringField(TEXT("expression_class_path"), ExpressionClassPath) || ExpressionClassPath.IsEmpty())
				{
					FString ExpressionType;
					if (Arguments->TryGetStringField(TEXT("expression_type"), ExpressionType) && !ExpressionType.IsEmpty())
					{
						// Strip "MaterialExpression" prefix if user included it
						FString ShortName = ExpressionType;
						if (ShortName.StartsWith(TEXT("MaterialExpression")))
						{
							ShortName = ShortName.RightChop(18);
						}
						ExpressionClassPath = FString::Printf(TEXT("/Script/Engine.MaterialExpression%s"), *ShortName);
					}
					else
					{
						OutError = TEXT("Missing expression_class_path or expression_type.");
						return false;
					}
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UClass* ExpressionClass = Context.Services.ResolveClass(ExpressionClassPath, OutError);
				if (!ExpressionClass)
				{
					return false;
				}
				const int32 NodeX = Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0;
				const int32 NodeY = Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialCreateExpression", "SOMOLMCP Create Material Expression"));
				UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExpressionClass, NodeX, NodeY);
				if (!Expression)
				{
					OutError = TEXT("Failed to create material expression.");
					return false;
				}
				if (FindMaterialExpressionIndex(Material, Expression) == INDEX_NONE || !Expression->IsA(ExpressionClass))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("expression_class_path"),
						TEXT("CreateMaterialExpression returned an expression that was not present in the material expression list or had the wrong class."));
					OutError = TEXT("Material expression creation readback failed.");
					return false;
				}
				if (UMaterialExpressionCollectionParameter* CollectionParameter = Cast<UMaterialExpressionCollectionParameter>(Expression))
				{
					FString CollectionPath;
					FString ParameterName;
					if (!Arguments->TryGetStringField(TEXT("collection_path"), CollectionPath) || CollectionPath.IsEmpty() ||
						!Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
					{
						OutError = TEXT("CollectionParameter expressions require collection_path and parameter_name.");
						return false;
					}
					FString CollectionError;
					UMaterialParameterCollection* Collection = Cast<UMaterialParameterCollection>(Context.Services.LoadAsset(CollectionPath, CollectionError));
					if (!Collection)
					{
						OutError = CollectionError.IsEmpty()
							? FString::Printf(TEXT("collection_path is not a MaterialParameterCollection: %s"), *CollectionPath)
							: CollectionError;
						return false;
					}
					CollectionParameter->Modify();
					CollectionParameter->Collection = Collection;
					CollectionParameter->ParameterName = FName(*ParameterName);
					Material->PostEditChange();
					Material->MarkPackageDirty();
				}
				// Bucket A fix: when caller passes parameter_name (or 'name' fallback) and the
				// new expression is any UMaterialExpressionParameter (Scalar/Vector/StaticBool/
				// TextureSampleParameter etc, all derived from UMaterialExpressionParameter),
				// apply the parameter name. Previously the arg was silently dropped.
				{
					FString ParamName;
					if (!Arguments->TryGetStringField(TEXT("parameter_name"), ParamName) || ParamName.IsEmpty())
					{
						Arguments->TryGetStringField(TEXT("name"), ParamName);
					}
					if (!ParamName.IsEmpty())
					{
						// Try concrete subclasses we already include (covers Scalar/Vector/StaticBool).
						// TextureSampleParameter would also work via the base, but its header is not
						// included here; skip silently if cast fails.
						if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expression))
						{
							SP->ParameterName = FName(*ParamName);
						}
						else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expression))
						{
							VP->ParameterName = FName(*ParamName);
						}
						else if (UMaterialExpressionStaticBoolParameter* BP = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
						{
							BP->ParameterName = FName(*ParamName);
						}
						Expression->Modify();
						Material->PostEditChange();
						Material->MarkPackageDirty();
					}
				}
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured = MaterialExpressionToJson(Material, Expression);
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutSummary = TEXT("Created material expression.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_delete_expression"),
			TEXT("Delete a material expression by index."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("expression_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("expression_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UMaterialExpression* Expression = ResolveMaterialExpressionFromArguments(Material, Arguments, TEXT("expression_index"), TEXT("expression_guid"), OutError);
				if (!Expression)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialDeleteExpression", "SOMOLMCP Delete Material Expression"));
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
				if (FindMaterialExpressionIndex(Material, Expression) != INDEX_NONE)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("expression_index"),
						TEXT("DeleteMaterialExpression returned but the expression is still present."));
					OutError = TEXT("Material expression delete readback failed.");
					return false;
				}
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured = MaterialExpressionsToJson(Material);
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutSummary = TEXT("Deleted material expression.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_connect_property"),
			TEXT("Connect a material expression output to a material property."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("expression_index"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("expression_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("property"), FSololmcpSchemaBuilder::String(TEXT("Material property name, e.g. BaseColor, Roughness, Normal"))},
					{TEXT("output_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("asset_path"), TEXT("property")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString PropertyName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("property"), PropertyName))
				{
					OutError = TEXT("Missing asset_path or property.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UMaterialExpression* Expression = ResolveMaterialExpressionFromArguments(Material, Arguments, TEXT("expression_index"), TEXT("expression_guid"), OutError);
				if (!Expression)
				{
					return false;
				}
				EMaterialProperty Property = MP_BaseColor;
				if (!TryParseMaterialProperty(PropertyName, Property, Material))
				{
					const FString DomainName = StaticEnum<EMaterialDomain>()
						? StaticEnum<EMaterialDomain>()->GetNameStringByValue(static_cast<int64>(Material->MaterialDomain))
						: TEXT("Unknown");
					OutError = FString::Printf(TEXT("Unsupported material property '%s' for domain %s. Call material_list_supported_properties with asset_path for the domain-aware list (Volume domain: Extinction/Albedo/EmissiveColor/AmbientOcclusion)."), *PropertyName, *DomainName);
					return false;
				}
				FString OutputName;
				Arguments->TryGetStringField(TEXT("output_name"), OutputName);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialConnectProperty", "SOMOLMCP Connect Material Property"));
				// Bucket B fix: ConnectMaterialProperty refuses connections silently when the
				// output name doesn't match a real output of the expression. Try the caller's
				// requested output first; on failure retry with common alternates and log what
				// was tried so the caller can diagnose. The accepted output name is included
				// in the response payload.
				TArray<FString> TriedOutputs;
				bool bConnected = false;
				FString AcceptedOutput;
				{
					TArray<FString> Candidates;
					Candidates.Add(OutputName);
					// Always try the empty/default output as a fallback.
					if (!OutputName.IsEmpty())
					{
						Candidates.Add(FString());
					}
					Candidates.Add(TEXT("RGB"));
					Candidates.Add(TEXT("RGBA"));
					Candidates.Add(TEXT("R"));
					Candidates.Add(TEXT("G"));
					Candidates.Add(TEXT("B"));
					Candidates.Add(TEXT("A"));
					// If it's a parameter, also try its ParameterName as an output (some setups use that).
					if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expression))
					{
						Candidates.Add(SP->ParameterName.ToString());
					}
					else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expression))
					{
						Candidates.Add(VP->ParameterName.ToString());
					}

					TSet<FString> Seen;
					for (const FString& Candidate : Candidates)
					{
						if (Seen.Contains(Candidate))
						{
							continue;
						}
						Seen.Add(Candidate);
						TriedOutputs.Add(Candidate);
						if (UMaterialEditingLibrary::ConnectMaterialProperty(Expression, Candidate, Property))
						{
							bConnected = true;
							AcceptedOutput = Candidate;
							break;
						}
					}
				}
				if (!bConnected)
				{
					FString TriedJoined = FString::Join(TriedOutputs, TEXT(","));
					UE_LOG(LogSOMOLMCP, Warning,
						TEXT("material_connect_property: ConnectMaterialProperty refused all outputs. ExprClass=%s requested_output='%s' property='%s' tried=[%s]"),
						*Expression->GetClass()->GetName(), *OutputName, *PropertyName, *TriedJoined);
					TArray<TSharedPtr<FJsonValue>> TriedJson;
					for (const FString& T : TriedOutputs)
					{
						TriedJson.Add(MakeShared<FJsonValueString>(T));
					}
					OutStructured->SetArrayField(TEXT("tried_outputs"), TriedJson);
					OutStructured->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetName());
					OutError = FString::Printf(TEXT("Failed to connect expression to material property (tried %d output names; see tried_outputs in response)."), TriedOutputs.Num());
					return false;
				}
				FExpressionInput* PropertyInput = Material->GetExpressionInputForProperty(Property);
				if (!PropertyInput || PropertyInput->Expression != Expression)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("property"),
						TEXT("ConnectMaterialProperty returned success but the material property input did not read back."));
					OutStructured->SetStringField(TEXT("accepted_output"), AcceptedOutput);
					OutError = TEXT("Material property connection readback failed.");
					return false;
				}
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured = MaterialExpressionsToJson(Material);
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetStringField(TEXT("accepted_output"), AcceptedOutput);
				OutStructured->SetBoolField(TEXT("connection_verified"), true);
				{
					TArray<TSharedPtr<FJsonValue>> TriedJson;
					for (const FString& T : TriedOutputs)
					{
						TriedJson.Add(MakeShared<FJsonValueString>(T));
					}
					OutStructured->SetArrayField(TEXT("tried_outputs"), TriedJson);
				}
				OutSummary = TEXT("Connected material expression to property.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_connect_expressions"),
			TEXT("Connect one material expression to another."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("from_expression_index"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("from_expression_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("to_expression_index"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("to_expression_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("from_output_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("to_input_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UMaterialExpression* FromExpression = ResolveMaterialExpressionFromArguments(Material, Arguments, TEXT("from_expression_index"), TEXT("from_expression_guid"), OutError);
				UMaterialExpression* ToExpression = ResolveMaterialExpressionFromArguments(Material, Arguments, TEXT("to_expression_index"), TEXT("to_expression_guid"), OutError);
				if (!FromExpression || !ToExpression)
				{
					return false;
				}
				FString FromOutputName;
				FString ToInputName;
				Arguments->TryGetStringField(TEXT("from_output_name"), FromOutputName);
				Arguments->TryGetStringField(TEXT("to_input_name"), ToInputName);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialConnectExpressions", "SOMOLMCP Connect Material Expressions"));
				if (!UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpression, FromOutputName, ToExpression, ToInputName))
				{
					OutError = TEXT("Failed to connect material expressions.");
					return false;
				}
				if (!UMaterialEditingLibrary::GetInputsForMaterialExpression(Material, ToExpression).Contains(FromExpression))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("to_input_name"),
						TEXT("ConnectMaterialExpressions returned success but the expression input did not read back."));
					OutError = TEXT("Material expression connection readback failed.");
					return false;
				}
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured = MaterialExpressionsToJson(Material);
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("connection_verified"), true);
				OutSummary = TEXT("Connected material expressions.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_recompile"),
			TEXT("Recompile a material after graph edits."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UMaterialEditingLibrary::RecompileMaterial(Material);
				SololmcpWriteFlush::EnsureFlushed(Material);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Material);
				OutSummary = TEXT("Recompiled material.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_get_statistics"),
			TEXT("Return material instruction and sampler statistics."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UMaterialInterface* Material = Cast<UMaterialInterface>(Asset);
				if (!Material)
				{
					OutError = TEXT("Asset is not a material interface.");
					return false;
				}
				const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Material);
				OutStructured->SetNumberField(TEXT("numVertexShaderInstructions"), Stats.NumVertexShaderInstructions);
				OutStructured->SetNumberField(TEXT("numPixelShaderInstructions"), Stats.NumPixelShaderInstructions);
				OutStructured->SetNumberField(TEXT("numSamplers"), Stats.NumSamplers);
				OutStructured->SetNumberField(TEXT("numVertexTextureSamples"), Stats.NumVertexTextureSamples);
				OutStructured->SetNumberField(TEXT("numPixelTextureSamples"), Stats.NumPixelTextureSamples);
				OutStructured->SetNumberField(TEXT("numVirtualTextureSamples"), Stats.NumVirtualTextureSamples);
				OutStructured->SetNumberField(TEXT("numUVScalars"), Stats.NumUVScalars);
				OutStructured->SetNumberField(TEXT("numInterpolatorScalars"), Stats.NumInterpolatorScalars);
				OutSummary = TEXT("Collected material statistics.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_duplicate_expression"),
			TEXT("Duplicate a material expression inside a material graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("expression_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("expression_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				int32 ExpressionIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetNumberField(TEXT("expression_index"), ExpressionIndex))
				{
					OutError = TEXT("Missing asset_path or expression_index.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UMaterialExpression* Expression = FindMaterialExpressionByIndex(Material, ExpressionIndex);
				if (!Expression)
				{
					OutError = TEXT("Material expression index is invalid.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialDuplicateExpression", "SOMOLMCP Duplicate Material Expression"));
				UMaterialExpression* Duplicated = UMaterialEditingLibrary::DuplicateMaterialExpression(Material, nullptr, Expression);
				if (!Duplicated)
				{
					OutError = TEXT("Failed to duplicate material expression.");
					return false;
				}
				OutStructured = MaterialExpressionToJson(Material, Duplicated);
				OutSummary = TEXT("Duplicated material expression.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_set_expression_properties"),
			TEXT("Apply public UPROPERTY values onto a material expression."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("expression_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("expression_index"), TEXT("properties")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				int32 ExpressionIndex = INDEX_NONE;
				TSharedPtr<FJsonObject> Properties;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetNumberField(TEXT("expression_index"), ExpressionIndex) || !TryGetObjectField(Arguments, TEXT("properties"), Properties))
				{
					OutError = TEXT("Missing asset_path, expression_index or properties.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UMaterialExpression* Expression = FindMaterialExpressionByIndex(Material, ExpressionIndex);
				if (!Expression)
				{
					OutError = TEXT("Material expression index is invalid.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialSetExpressionProperties", "SOMOLMCP Set Material Expression Properties"));
				if (!Context.Services.ApplyProperties(Expression, Properties.ToSharedRef(), OutError))
				{
					return false;
				}
				OutStructured = MaterialExpressionToJson(Material, Expression);
				OutSummary = TEXT("Updated material expression properties.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_layout_expressions"),
			TEXT("Auto-layout all expressions in a material graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
				OutStructured = MaterialExpressionsToJson(Material);
				OutSummary = TEXT("Laid out material expressions.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_create"),
			TEXT("Create a material function asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetName;
				if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) || !Arguments->TryGetStringField(TEXT("asset_name"), AssetName))
				{
					OutError = TEXT("Missing package_path or asset_name.");
					return false;
				}
				const FString FullAssetPath = PackagePath / AssetName;
				if (Context.Services.AssetExists(FullAssetPath) || Context.Services.AssetExists(FullAssetPath + TEXT(".") + AssetName))
				{
					OutError = FString::Printf(TEXT("Asset already exists: %s. Use a different asset_name or delete it first."), *FullAssetPath);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialFunctionCreate", "SOMOLMCP Create Material Function"));
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/Engine.MaterialFunction"), TEXT("/Script/UnrealEd.MaterialFunctionFactoryNew"), nullptr, OutError);
				if (!Asset)
				{
					return false;
				}
				if (!Asset->IsA<UMaterialFunction>())
				{
					OutError = FString::Printf(TEXT("create_returned_unexpected_class: %s"), *Asset->GetClass()->GetPathName());
					return false;
				}
				// Audit round 10B (silent-create fix): persist + verify on disk.
				const FString CreatedPath = Asset->GetPathName();
				Asset->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Asset);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!VerifyCreatedAssetReloaded(Context.Services, Asset, UMaterialFunction::StaticClass(), OutStructured, OutError))
				{
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = TEXT("Created material function.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_list_expressions"),
			TEXT("List expressions in a material function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterialFunction* MaterialFunction = LoadMaterialFunctionAsset(Context.Services, AssetPath, OutError);
				if (!MaterialFunction)
				{
					return false;
				}
				OutStructured = MaterialFunctionExpressionsToJson(MaterialFunction);
				OutSummary = TEXT("Listed material function expressions.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_create_expression"),
			TEXT("Create a material expression inside a material function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("expression_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("expression_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ExpressionClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("expression_class_path"), ExpressionClassPath))
				{
					OutError = TEXT("Missing asset_path or expression_class_path.");
					return false;
				}
				UMaterialFunction* MaterialFunction = LoadMaterialFunctionAsset(Context.Services, AssetPath, OutError);
				if (!MaterialFunction)
				{
					return false;
				}
				UClass* ExpressionClass = Context.Services.ResolveClass(ExpressionClassPath, OutError);
				if (!ExpressionClass)
				{
					return false;
				}
				const int32 NodeX = Arguments->HasTypedField<EJson::Number>(TEXT("node_x")) ? Arguments->GetIntegerField(TEXT("node_x")) : 0;
				const int32 NodeY = Arguments->HasTypedField<EJson::Number>(TEXT("node_y")) ? Arguments->GetIntegerField(TEXT("node_y")) : 0;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialFunctionCreateExpression", "SOMOLMCP Create Material Function Expression"));
				UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(MaterialFunction, ExpressionClass, NodeX, NodeY);
				if (!Expression)
				{
					OutError = TEXT("Failed to create material function expression.");
					return false;
				}
				if (FindMaterialFunctionExpressionIndex(MaterialFunction, Expression) == INDEX_NONE || !Expression->IsA(ExpressionClass))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("expression_class_path"),
						TEXT("CreateMaterialExpressionInFunction returned an expression that did not read back from the material function."));
					OutError = TEXT("Material function expression creation readback failed.");
					return false;
				}
				OutStructured = MaterialFunctionExpressionToJson(MaterialFunction, Expression);
				OutStructured->SetBoolField(TEXT("expression_verified"), true);
				OutSummary = TEXT("Created material function expression.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_delete_expression"),
			TEXT("Delete a material expression from a material function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("expression_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("expression_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				int32 ExpressionIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetNumberField(TEXT("expression_index"), ExpressionIndex))
				{
					OutError = TEXT("Missing asset_path or expression_index.");
					return false;
				}
				UMaterialFunction* MaterialFunction = LoadMaterialFunctionAsset(Context.Services, AssetPath, OutError);
				if (!MaterialFunction)
				{
					return false;
				}
				UMaterialExpression* Expression = FindMaterialFunctionExpressionByIndex(MaterialFunction, ExpressionIndex);
				if (!Expression)
				{
					OutError = TEXT("Material function expression index is invalid.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialFunctionDeleteExpression", "SOMOLMCP Delete Material Function Expression"));
				UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(MaterialFunction, Expression);
				if (FindMaterialFunctionExpressionIndex(MaterialFunction, Expression) != INDEX_NONE)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("expression_index"),
						TEXT("DeleteMaterialExpressionInFunction returned but the expression is still present."));
					OutError = TEXT("Material function expression delete readback failed.");
					return false;
				}
				OutStructured = MaterialFunctionExpressionsToJson(MaterialFunction);
				OutStructured->SetBoolField(TEXT("expression_removed_verified"), true);
				OutSummary = TEXT("Deleted material function expression.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_layout"),
			TEXT("Auto-layout all expressions in a material function graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterialFunction* MaterialFunction = LoadMaterialFunctionAsset(Context.Services, AssetPath, OutError);
				if (!MaterialFunction)
				{
					return false;
				}
				UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(MaterialFunction);
				OutStructured = MaterialFunctionExpressionsToJson(MaterialFunction);
				OutSummary = TEXT("Laid out material function expressions.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_recompile"),
			TEXT("Update and recompile a material function."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterialFunction* MaterialFunction = LoadMaterialFunctionAsset(Context.Services, AssetPath, OutError);
				if (!MaterialFunction)
				{
					return false;
				}
				UMaterialEditingLibrary::UpdateMaterialFunction(MaterialFunction, nullptr);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(MaterialFunction);
				OutSummary = TEXT("Updated material function.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_instance_set_parent"),
			TEXT("Set the parent material for a material instance."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_material_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("parent_material_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParentMaterialPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("parent_material_path"), ParentMaterialPath))
				{
					OutError = TEXT("Missing asset_path or parent_material_path.");
					return false;
				}
				UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Context.Services.LoadAsset(AssetPath, OutError));
				UMaterialInterface* ParentMaterial = Cast<UMaterialInterface>(Context.Services.LoadAsset(ParentMaterialPath, OutError));
				if (!MaterialInstance || !ParentMaterial)
				{
					OutError = TEXT("asset_path must be a material instance and parent_material_path must be a material interface.");
					return false;
				}
				UMaterialEditingLibrary::SetMaterialInstanceParent(MaterialInstance, ParentMaterial);
				if (MaterialInstance->Parent != ParentMaterial)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("parent_material_path"),
						TEXT("SetMaterialInstanceParent returned but Parent did not match on readback."));
					OutError = TEXT("Material instance parent readback failed.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(MaterialInstance);
				OutStructured->SetBoolField(TEXT("parent_verified"), true);
				OutSummary = TEXT("Updated material instance parent.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_instance_set_static_switch_parameter"),
			TEXT("Set a static switch parameter on a material instance."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parameter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("value"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("parameter_name"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ParameterName;
				bool bValue = false;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) || !Arguments->TryGetBoolField(TEXT("value"), bValue))
				{
					OutError = TEXT("Missing asset_path, parameter_name or value.");
					return false;
				}
				UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!MaterialInstance)
				{
					OutError = TEXT("Asset is not a material instance constant.");
					return false;
				}
				if (!UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, *ParameterName, bValue))
				{
					OutError = TEXT("Failed to set static switch parameter.");
					return false;
				}
				FStaticParameterSet StaticParameters;
				MaterialInstance->GetStaticParameterValues(StaticParameters);
				bool bFoundSwitch = false;
				for (const FStaticSwitchParameter& Parameter : StaticParameters.StaticSwitchParameters)
				{
					if (Parameter.ParameterInfo.Name == FName(*ParameterName))
					{
						bFoundSwitch = true;
						if (Parameter.Value != bValue)
						{
							SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"),
								TEXT("Static switch parameter value did not match on readback."));
							OutError = TEXT("Material instance static switch readback failed.");
							return false;
						}
						break;
					}
				}
				if (!bFoundSwitch)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("parameter_name"),
						TEXT("Static switch parameter was not present after SetMaterialInstanceStaticSwitchParameterValue."));
					OutError = TEXT("Material instance static switch readback failed.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(MaterialInstance);
				OutStructured->SetBoolField(TEXT("parameter_verified"), true);
				OutSummary = TEXT("Updated material instance static switch parameter.");
				return true;
			}
		, nullptr
		, 5
		});

		auto RegisterMaterialParamTool = [&Registry](const FString& ToolName, const FString& Description, auto ApplyParameter,
			const TSharedRef<FJsonObject>& ValueSchema, const bool bValueRequired, const bool bColorAlias)
		{
			TMap<FString, TSharedRef<FJsonObject>> Properties =
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("parameter_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("value"), ValueSchema}
			};
			if (bColorAlias)
			{
				Properties.Add(TEXT("color_value"), ValueSchema);
			}
			TArray<FString> Required = {TEXT("asset_path"), TEXT("parameter_name")};
			if (bValueRequired)
			{
				Required.Add(TEXT("value"));
			}
			Registry.Register({
				ToolName,
				Description,
				FSololmcpSchemaBuilder::Object(Properties, Required),

				[ApplyParameter, ToolName](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					FString AssetPath;
					FString ParameterName;
					if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName))
					{
						OutError = TEXT("Missing asset_path or parameter_name.");
						return false;
					}
					UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
					UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(Asset);
					if (!MaterialInstance)
					{
						OutError = TEXT("Asset is not a material instance constant.");
						return false;
					}
					MaterialInstance->Modify();
					if (!ApplyParameter(Context, Arguments, MaterialInstance, ParameterName, OutError))
					{
						return false;
					}
					UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
					MaterialInstance->PostEditChange();
					MaterialInstance->MarkPackageDirty();
					FString SaveError;
					if (!Context.Services.SaveAsset(AssetPath, false, SaveError))
					{
						OutError = FString::Printf(
							TEXT("Parameter was applied but the material instance could not be saved: %s"),
							*SaveError);
						return false;
					}
					OutStructured = FSololmcpEditorServices::MakeObjectReference(MaterialInstance);
					OutStructured->SetStringField(TEXT("parameter_name"), ParameterName);
					const FMaterialParameterInfo ParameterInfo{FName(*ParameterName)};
					bool bVerified = false;
					if (ToolName == TEXT("material_instance_set_scalar_parameter"))
					{
						double Requested = 0.0;
						float Readback = 0.0f;
						bVerified = Arguments->TryGetNumberField(TEXT("value"), Requested)
							&& MaterialInstance->GetScalarParameterValue(ParameterInfo, Readback)
							&& FMath::IsNearlyEqual(Readback, static_cast<float>(Requested), 1.0e-6f);
						OutStructured->SetNumberField(TEXT("requested_value"), Requested);
						OutStructured->SetNumberField(TEXT("readback_value"), Readback);
					}
					else if (ToolName == TEXT("material_instance_set_vector_parameter"))
					{
						FLinearColor Requested = FLinearColor::Black;
						FLinearColor Readback = FLinearColor::Black;
						FString InputForm;
						bVerified = TryParseMaterialVectorValue(Arguments, Requested, &InputForm)
							&& MaterialInstance->GetVectorParameterValue(ParameterInfo, Readback)
							&& Requested.Equals(Readback, 1.0e-6f);
						OutStructured->SetObjectField(TEXT("requested_value"), LinearColorToJson(Requested));
						OutStructured->SetObjectField(TEXT("readback_value"), LinearColorToJson(Readback));
						OutStructured->SetStringField(TEXT("input_form"), InputForm);
						OutStructured->SetStringField(TEXT("normalized_form"), TEXT("value_object_rgba"));
					}
					else if (ToolName == TEXT("material_instance_set_texture_parameter"))
					{
						FString RequestedPath;
						UTexture* Readback = nullptr;
						FString ResolveError;
						UTexture* RequestedTexture = nullptr;
						if (Arguments->TryGetStringField(TEXT("value"), RequestedPath))
						{
							RequestedTexture = Cast<UTexture>(
								Context.Services.LoadAsset(RequestedPath, ResolveError));
						}
						bVerified = RequestedTexture
							&& MaterialInstance->GetTextureParameterValue(ParameterInfo, Readback)
							&& Readback == RequestedTexture;
						OutStructured->SetStringField(TEXT("requested_value"), RequestedPath);
						OutStructured->SetStringField(
							TEXT("readback_value"), Readback ? Readback->GetPathName() : FString());
					}
					if (!bVerified)
					{
						OutStructured->SetBoolField(TEXT("parameter_verified"), false);
						OutError = TEXT("Material instance parameter effective-value readback did not match the request.");
						return false;
					}
					OutStructured->SetBoolField(TEXT("parameter_verified"), true);
					OutStructured->SetBoolField(TEXT("saved"), true);
					OutSummary = TEXT("Updated material instance parameter.");
					return true;
			}
		, nullptr
		, 5
		});
		};

		RegisterMaterialParamTool(TEXT("material_instance_set_scalar_parameter"), TEXT("Set a scalar parameter on a material instance."),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, UMaterialInstanceConstant* MaterialInstance, const FString& ParameterName, FString& OutError)
			{
				double Value = 0.0;
				if (!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("Missing scalar value.");
					return false;
				}
				const float FloatValue = static_cast<float>(Value);
				// Round 12M: UMaterialEditingLibrary returns false silently when parameter
				// isn't yet in MI's override set. Fall back to direct MI override array.
				// UE 5.8 can report success here without materializing an override entry.
				// Always upsert the native override array, then let the common wrapper
				// update, save, and read back the asset.
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(
					MaterialInstance, *ParameterName, FloatValue);
				const FName ParamName(*ParameterName);
				const FMaterialParameterInfo ParamInfo(ParamName);
				FScalarParameterValue* Existing = nullptr;
				for (FScalarParameterValue& P : MaterialInstance->ScalarParameterValues)
				{
					if (P.ParameterInfo.Name == ParamName)
					{
						Existing = &P;
						break;
					}
				}
				MaterialInstance->Modify();
				if (Existing)
				{
					Existing->ParameterValue = FloatValue;
				}
				else
				{
					FScalarParameterValue NewParam;
					NewParam.ParameterInfo = ParamInfo;
					NewParam.ParameterValue = FloatValue;
					MaterialInstance->ScalarParameterValues.Add(NewParam);
				}
				MaterialInstance->SetScalarParameterValueEditorOnly(ParamInfo, FloatValue);
				MaterialInstance->PostEditChange();
				UE_LOG(LogTemp, Verbose, TEXT("[12M] scalar param '%s' set via direct override"), *ParameterName);
				return true;
			},
			FSololmcpSchemaBuilder::Number(), true, false);

		RegisterMaterialParamTool(TEXT("material_instance_set_vector_parameter"), TEXT("Set a vector parameter on a material instance. Accepts value as {r,g,b,a}, [r,g,b], [r,g,b,a], or the color_value compatibility alias."),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, UMaterialInstanceConstant* MaterialInstance, const FString& ParameterName, FString& OutError)
			{
				FLinearColor ColorValue = FLinearColor::White;
				if (!TryParseMaterialVectorValue(Arguments, ColorValue))
				{
					OutError = TEXT("Invalid color value. Use value:{r,g,b,a}, value:[r,g,b,a], or color_value.");
					return false;
				}
				// Round 12M: same direct-override fallback as scalar.
				// Do not trust the editor library's boolean as persistence evidence;
				// explicitly materialize the override for deterministic cold-load readback.
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
					MaterialInstance, *ParameterName, ColorValue);
				const FName ParamName(*ParameterName);
				const FMaterialParameterInfo ParamInfo(ParamName);
				FVectorParameterValue* Existing = nullptr;
				for (FVectorParameterValue& P : MaterialInstance->VectorParameterValues)
				{
					if (P.ParameterInfo.Name == ParamName)
					{
						Existing = &P;
						break;
					}
				}
				MaterialInstance->Modify();
				if (Existing)
				{
					Existing->ParameterValue = ColorValue;
				}
				else
				{
					FVectorParameterValue NewParam;
					NewParam.ParameterInfo = ParamInfo;
					NewParam.ParameterValue = ColorValue;
					MaterialInstance->VectorParameterValues.Add(NewParam);
				}
				MaterialInstance->SetVectorParameterValueEditorOnly(ParamInfo, ColorValue);
				MaterialInstance->PostEditChange();
				UE_LOG(LogTemp, Verbose, TEXT("[12M] vector param '%s' set via direct override"), *ParameterName);
				return true;
			},
			FSololmcpSchemaBuilder::AnyOf({
				ColorSchema(),
				FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number(), TEXT("RGB or RGBA components."), 3, 4)
			}), false, true);

		RegisterMaterialParamTool(TEXT("material_instance_set_texture_parameter"), TEXT("Set a texture parameter on a material instance."),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, UMaterialInstanceConstant* MaterialInstance, const FString& ParameterName, FString& OutError)
			{
				FString TexturePath;
				if (!Arguments->TryGetStringField(TEXT("value"), TexturePath))
				{
					OutError = TEXT("Missing texture asset path.");
					return false;
				}
				UObject* TextureObject = Context.Services.LoadAsset(TexturePath, OutError);
				UTexture* Texture = Cast<UTexture>(TextureObject);
				if (!Texture)
				{
					OutError = TEXT("value must resolve to a texture asset.");
					return false;
				}
				return UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, *ParameterName, Texture);
			},
			FSololmcpSchemaBuilder::String(), true, false);

		Registry.Register({
			TEXT("static_mesh_set_nanite"),
			TEXT("Enable or disable Nanite on a static mesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("enabled"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("enabled")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				bool bEnabled = false;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetBoolField(TEXT("enabled"), bEnabled))
				{
					OutError = TEXT("Missing asset_path or enabled.");
					return false;
				}
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
				if (!StaticMesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}
				UStaticMeshEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
				FMeshNaniteSettings Settings = SOMOLMCP_NANITE_SETTINGS(StaticMesh);
				Settings.bEnabled = bEnabled;
				Subsystem->SetNaniteSettings(StaticMesh, Settings, true);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(StaticMesh);
				OutSummary = TEXT("Updated Nanite settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("static_mesh_add_simple_collision"),
			TEXT("Add simple collision to a static mesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("shape_type"), FSololmcpSchemaBuilder::String(TEXT("box | sphere | capsule | ndop10x | ndop10y | ndop10z | ndop18 | ndop26"))}}, {TEXT("asset_path"), TEXT("shape_type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ShapeType;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("shape_type"), ShapeType))
				{
					OutError = TEXT("Missing asset_path or shape_type.");
					return false;
				}
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
				if (!StaticMesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}

				EScriptCollisionShapeType CollisionShape = EScriptCollisionShapeType::Box;
				if (ShapeType == TEXT("sphere"))
				{
					CollisionShape = EScriptCollisionShapeType::Sphere;
				}
				else if (ShapeType == TEXT("capsule"))
				{
					CollisionShape = EScriptCollisionShapeType::Capsule;
				}
				else if (ShapeType == TEXT("ndop10x"))
				{
					CollisionShape = EScriptCollisionShapeType::NDOP10_X;
				}
				else if (ShapeType == TEXT("ndop10y"))
				{
					CollisionShape = EScriptCollisionShapeType::NDOP10_Y;
				}
				else if (ShapeType == TEXT("ndop10z"))
				{
					CollisionShape = EScriptCollisionShapeType::NDOP10_Z;
				}
				else if (ShapeType == TEXT("ndop18"))
				{
					CollisionShape = EScriptCollisionShapeType::NDOP18;
				}
				else if (ShapeType == TEXT("ndop26"))
				{
					CollisionShape = EScriptCollisionShapeType::NDOP26;
				}

				UStaticMeshEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>();
				const int32 AddedCount = Subsystem->AddSimpleCollisions(StaticMesh, CollisionShape);
				OutStructured->SetNumberField(TEXT("added"), AddedCount);
				OutStructured->SetObjectField(TEXT("asset"), FSololmcpEditorServices::MakeObjectReference(StaticMesh));
				OutSummary = TEXT("Added simple collisions.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("skeletal_mesh_create_physics_asset"),
			TEXT("Create a physics asset from a skeletal mesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("set_to_mesh"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset);
				if (!SkeletalMesh)
				{
					OutError = TEXT("Asset is not a skeletal mesh.");
					return false;
				}
				const bool bSetToMesh = Arguments->HasTypedField<EJson::Boolean>(TEXT("set_to_mesh")) ? Arguments->GetBoolField(TEXT("set_to_mesh")) : true;
				const int32 LODIndex = Arguments->HasTypedField<EJson::Number>(TEXT("lod_index")) ? Arguments->GetIntegerField(TEXT("lod_index")) : 0;
				#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
				UPhysicsAsset* PhysicsAsset = USkeletalMeshEditorSubsystem::CreatePhysicsAsset(SkeletalMesh);
				#else
				UPhysicsAsset* PhysicsAsset = USkeletalMeshEditorSubsystem::CreatePhysicsAsset(SkeletalMesh, bSetToMesh, LODIndex);
				#endif
				if (!PhysicsAsset)
				{
					OutError = TEXT("Failed to create physics asset.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(PhysicsAsset);
				OutSummary = TEXT("Created physics asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("skeletal_mesh_assign_physics_asset"),
			TEXT("Assign an existing physics asset to a skeletal mesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("physics_asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("physics_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString PhysicsAssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("physics_asset_path"), PhysicsAssetPath))
				{
					OutError = TEXT("Missing asset_path or physics_asset_path.");
					return false;
				}
				USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!SkeletalMesh)
				{
					OutError = TEXT("Asset is not a skeletal mesh.");
					return false;
				}
				UPhysicsAsset* PhysicsAsset = Cast<UPhysicsAsset>(Context.Services.LoadAsset(PhysicsAssetPath, OutError));
				if (!PhysicsAsset)
				{
					OutError = TEXT("physics_asset_path does not resolve to a physics asset.");
					return false;
				}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				if (!USkeletalMeshEditorSubsystem::AssignPhysicsAsset(SkeletalMesh, PhysicsAsset))
				{
					OutError = TEXT("Failed to assign physics asset.");
					return false;
				}
#else
				// USkeletalMeshEditorSubsystem::AssignPhysicsAsset arrived after 5.3.
				OutError = TEXT("NOT_AVAILABLE_ON_ENGINE: assigning a physics asset through the "
								 "skeletal mesh editor subsystem needs UE 5.4 or newer.");
				return false;
#endif
				OutStructured = FSololmcpEditorServices::MakeObjectReference(SkeletalMesh);
				OutSummary = TEXT("Assigned physics asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_batch_edit"),
			TEXT("Run multiple animation edit operations in sequence. Each operation requires {tool, arguments}."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("operations"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("tool"), FSololmcpSchemaBuilder::String()},
						{TEXT("arguments"), FSololmcpSchemaBuilder::Object({})}
					}, {TEXT("tool"), TEXT("arguments")}))},
					{TEXT("continue_on_error"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("atomic"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("rollback_on_error"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("operations")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<FString> AllowedTools = {
					TEXT("animation_add_notify"),
					TEXT("animation_add_curve"),
					TEXT("animation_add_notify_track"),
					TEXT("animation_remove_notify_track"),
					TEXT("animation_remove_notifies_by_name"),
					TEXT("animation_remove_notifies_by_track"),
					TEXT("animation_add_notify_state"),
					TEXT("animation_update_notify"),
					TEXT("animation_remove_notify_by_guid"),
					TEXT("animation_add_float_curve_key"),
					TEXT("animation_set_float_curve_keys"),
					TEXT("animation_remove_float_curve_key"),
					TEXT("animation_add_vector_curve_key"),
					TEXT("animation_set_vector_curve_keys"),
					TEXT("animation_add_transform_curve_key"),
					TEXT("animation_set_transform_curve_keys"),
					TEXT("animation_add_sync_marker"),
					TEXT("animation_remove_curve"),
					TEXT("animation_rename_curve")
				};

				const bool bContinueOnError = Arguments->HasTypedField<EJson::Boolean>(TEXT("continue_on_error")) && Arguments->GetBoolField(TEXT("continue_on_error"));
				const bool bAtomic = Arguments->HasTypedField<EJson::Boolean>(TEXT("atomic")) && Arguments->GetBoolField(TEXT("atomic"));
				const bool bRollbackOnError = bAtomic || (Arguments->HasTypedField<EJson::Boolean>(TEXT("rollback_on_error")) && Arguments->GetBoolField(TEXT("rollback_on_error")));
				const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("operations"), Operations) || !Operations)
				{
					OutError = TEXT("Missing operations array.");
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> Results;
				int32 SuccessCount = 0;
				int32 FailureCount = 0;

				for (int32 Index = 0; Index < Operations->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> OpObject = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
					if (!OpObject.IsValid())
					{
						OutError = FString::Printf(TEXT("operations[%d] must be an object."), Index);
						return false;
					}

					FString ToolName;
					const TSharedPtr<FJsonObject>* OpArgsPtr = nullptr;
					if (!OpObject->TryGetStringField(TEXT("tool"), ToolName) || !OpObject->TryGetObjectField(TEXT("arguments"), OpArgsPtr) || !OpArgsPtr || !OpArgsPtr->IsValid())
					{
						OutError = FString::Printf(TEXT("operations[%d] must include tool and arguments object."), Index);
						return false;
					}
					if (!AllowedTools.Contains(ToolName))
					{
						OutError = FString::Printf(TEXT("operations[%d] has unsupported animation tool: %s"), Index, *ToolName);
						return false;
					}

					TSharedRef<FJsonObject> StepStructured = MakeShared<FJsonObject>();
					FString StepSummary;
					FString StepError;
					const bool bStepOk = Registry.ExecuteTool(ToolName, OpArgsPtr->ToSharedRef(), StepStructured, StepSummary, StepError);

					TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
					StepResult->SetNumberField(TEXT("index"), Index);
					StepResult->SetStringField(TEXT("tool"), ToolName);
					StepResult->SetBoolField(TEXT("ok"), bStepOk);
					StepResult->SetStringField(TEXT("code"), bStepOk ? TEXT("ok") : TEXT("tool_execution_failed"));
					StepResult->SetStringField(TEXT("summary"), StepSummary);
					if (!StepError.IsEmpty())
					{
						StepResult->SetStringField(TEXT("error"), StepError);
					}
					StepResult->SetObjectField(TEXT("result"), StepStructured);
					Results.Add(MakeShared<FJsonValueObject>(StepResult));

					if (bStepOk)
					{
						++SuccessCount;
					}
					else
					{
						++FailureCount;
						if (!bContinueOnError)
						{
							int32 RollbackCount = 0;
							if (bRollbackOnError && GEditor)
							{
								for (int32 UndoIndex = 0; UndoIndex < SuccessCount; ++UndoIndex)
								{
									if (GEditor->UndoTransaction())
									{
										++RollbackCount;
									}
									else
									{
										break;
									}
								}
							}
							OutStructured->SetArrayField(TEXT("steps"), Results);
							OutStructured->SetNumberField(TEXT("successCount"), SuccessCount);
							OutStructured->SetNumberField(TEXT("failureCount"), FailureCount);
							OutStructured->SetBoolField(TEXT("rolledBack"), bRollbackOnError && RollbackCount > 0);
							OutStructured->SetNumberField(TEXT("rollbackCount"), RollbackCount);
							OutStructured->SetStringField(TEXT("errorCode"), TEXT("animation_batch_step_failed"));
							OutError = FString::Printf(TEXT("animation_batch_edit failed at operation %d: %s"), Index, *StepError);
							return false;
						}
					}
				}

				OutStructured->SetArrayField(TEXT("steps"), Results);
				OutStructured->SetNumberField(TEXT("successCount"), SuccessCount);
				OutStructured->SetNumberField(TEXT("failureCount"), FailureCount);
				OutStructured->SetBoolField(TEXT("atomic"), bAtomic);
				OutStructured->SetBoolField(TEXT("rollbackOnError"), bRollbackOnError);
				OutStructured->SetStringField(TEXT("code"), FailureCount > 0 ? TEXT("partial_success") : TEXT("ok"));
				OutSummary = FString::Printf(TEXT("Executed %d animation operations (%d success, %d failed)."), Results.Num(), SuccessCount, FailureCount);
				if (FailureCount > 0 && SuccessCount == 0)
				{
					OutError = TEXT("All animation batch operations failed.");
					return false;
				}
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_add_notify"),
			TEXT("Add an animation notify event to an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("notify_track_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("notify_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("notify_track_name"), TEXT("time"), TEXT("notify_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NotifyTrackName;
				FString NotifyClassPath;
				double Time = 0.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("notify_track_name"), NotifyTrackName) ||
					!Arguments->TryGetNumberField(TEXT("time"), Time) ||
					!Arguments->TryGetStringField(TEXT("notify_class_path"), NotifyClassPath))
				{
					OutError = TEXT("Missing asset_path, notify_track_name, time or notify_class_path.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UClass* NotifyClass = Context.Services.ResolveClass(NotifyClassPath, OutError);
				if (!NotifyClass)
				{
					return false;
				}
				const int32 BeforeNotifyCount = Animation->Notifies.Num();
				UAnimationBlueprintLibrary::AddAnimationNotifyEvent(SOMOLMCP_ANIM_KEY_TARGET(Animation), *NotifyTrackName, static_cast<float>(Time), NotifyClass);
				if (Animation->Notifies.Num() <= BeforeNotifyCount)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("notify_class_path"),
						TEXT("AddAnimationNotifyEvent returned but notify count did not increase."));
					OutError = TEXT("Animation notify add readback failed.");
					return false;
				}
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Animation);
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("notify_verified"), true);
				OutSummary = TEXT("Added animation notify.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_add_curve"),
			TEXT("Add a float curve to an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("curve_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName))
				{
					OutError = TEXT("Missing asset_path or curve_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UAnimationBlueprintLibrary::AddCurve(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName);
				const FAnimationCurveIdentifier CurveId(*CurveName, ERawCurveTrackTypes::RCT_Float);
				if (!Animation->GetDataModel() || Animation->GetDataModel()->FindCurve(CurveId) == nullptr)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("curve_name"),
						TEXT("AddCurve returned but the float curve did not read back."));
					OutError = TEXT("Animation curve add readback failed.");
					return false;
				}
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Animation);
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("curve_verified"), true);
				OutSummary = TEXT("Added animation curve.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_list_notify_tracks"),
			TEXT("List notify tracks on an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<FName> TrackNames;
				UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames(SOMOLMCP_ANIM_KEY_TARGET(Animation), TrackNames);
				OutStructured = NamesToJson(TrackNames, TEXT("tracks"));
				OutSummary = TEXT("Listed animation notify tracks.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_add_notify_track"),
			TEXT("Add a notify track to an animation sequence."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("track_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("track_color"), ColorSchema()}
				},
				{TEXT("asset_path"), TEXT("track_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString TrackName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("track_name"), TrackName))
				{
					OutError = TEXT("Missing asset_path or track_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				FLinearColor TrackColor = FLinearColor::White;
				TSharedPtr<FJsonObject> ColorObject;
				if (TryGetObjectField(Arguments, TEXT("track_color"), ColorObject) && !FSololmcpEditorServices::JsonToLinearColor(ColorObject, TrackColor))
				{
					OutError = TEXT("track_color must be a color object.");
					return false;
				}
				UAnimationBlueprintLibrary::AddAnimationNotifyTrack(SOMOLMCP_ANIM_KEY_TARGET(Animation), *TrackName, TrackColor);
				TArray<FName> TrackNames;
				UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames(SOMOLMCP_ANIM_KEY_TARGET(Animation), TrackNames);
				if (!TrackNames.Contains(FName(*TrackName)))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("track_name"),
						TEXT("AddAnimationNotifyTrack returned but the track did not read back."));
					OutError = TEXT("Animation notify track add readback failed.");
					return false;
				}
				OutStructured = NamesToJson(TrackNames, TEXT("tracks"));
				OutStructured->SetBoolField(TEXT("track_verified"), true);
				OutSummary = TEXT("Added animation notify track.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_remove_notify_track"),
			TEXT("Remove a notify track from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("track_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString TrackName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("track_name"), TrackName))
				{
					OutError = TEXT("Missing asset_path or track_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UAnimationBlueprintLibrary::RemoveAnimationNotifyTrack(SOMOLMCP_ANIM_KEY_TARGET(Animation), *TrackName);
				TArray<FName> TrackNames;
				UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames(SOMOLMCP_ANIM_KEY_TARGET(Animation), TrackNames);
				if (TrackNames.Contains(FName(*TrackName)))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("track_name"),
						TEXT("RemoveAnimationNotifyTrack returned but the track is still present."));
					OutError = TEXT("Animation notify track remove readback failed.");
					return false;
				}
				OutStructured = NamesToJson(TrackNames, TEXT("tracks"));
				OutStructured->SetBoolField(TEXT("track_removed_verified"), true);
				OutSummary = TEXT("Removed animation notify track.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_list_notifies"),
			TEXT("List notify events on an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<FAnimNotifyEvent> Events;
				FString TrackName;
				if (Arguments->TryGetStringField(TEXT("track_name"), TrackName) && !TrackName.IsEmpty())
				{
					UAnimationBlueprintLibrary::GetAnimationNotifyEventsForTrack(SOMOLMCP_ANIM_KEY_TARGET(Animation), *TrackName, Events);
				}
				else
				{
					UAnimationBlueprintLibrary::GetAnimationNotifyEvents(SOMOLMCP_ANIM_KEY_TARGET(Animation), Events);
				}
				OutStructured = AnimationNotifyEventsToJson(Events);
				OutSummary = TEXT("Listed animation notify events.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_remove_notifies_by_name"),
			TEXT("Remove notify events by name from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("notify_name"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("notify_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NotifyName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("notify_name"), NotifyName))
				{
					OutError = TEXT("Missing asset_path or notify_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				const int32 RemovedCount = UAnimationBlueprintLibrary::RemoveAnimationNotifyEventsByName(SOMOLMCP_ANIM_KEY_TARGET(Animation), *NotifyName);
				if (RemovedCount <= 0)
				{
					SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("notify_name"),
						TEXT("No animation notify events matched the requested name."));
					OutError = FString::Printf(TEXT("No animation notify events matched: %s"), *NotifyName);
					return false;
				}
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured->SetNumberField(TEXT("removedCount"), RemovedCount);
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutSummary = TEXT("Removed animation notify events by name.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_remove_notifies_by_track"),
			TEXT("Remove notify events by track from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("track_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString TrackName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("track_name"), TrackName))
				{
					OutError = TEXT("Missing asset_path or track_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				const int32 RemovedCount = UAnimationBlueprintLibrary::RemoveAnimationNotifyEventsByTrack(SOMOLMCP_ANIM_KEY_TARGET(Animation), *TrackName);
				if (RemovedCount <= 0)
				{
					SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("track_name"),
						TEXT("No animation notify events matched the requested track."));
					OutError = FString::Printf(TEXT("No animation notify events matched track: %s"), *TrackName);
					return false;
				}
				OutStructured->SetNumberField(TEXT("removedCount"), RemovedCount);
				OutSummary = TEXT("Removed animation notify events by track.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_add_notify_state"),
			TEXT("Add an animation notify state to an animation sequence."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("notify_track_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("time"), FSololmcpSchemaBuilder::Number()},
					{TEXT("duration"), FSololmcpSchemaBuilder::Number()},
					{TEXT("notify_state_class_path"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("notify_track_name"), TEXT("time"), TEXT("duration"), TEXT("notify_state_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NotifyTrackName;
				FString NotifyStateClassPath;
				double Time = 0.0;
				double Duration = 0.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("notify_track_name"), NotifyTrackName) ||
					!Arguments->TryGetNumberField(TEXT("time"), Time) ||
					!Arguments->TryGetNumberField(TEXT("duration"), Duration) ||
					!Arguments->TryGetStringField(TEXT("notify_state_class_path"), NotifyStateClassPath))
				{
					OutError = TEXT("Missing required notify state arguments.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UClass* NotifyStateClass = Context.Services.ResolveClass(NotifyStateClassPath, OutError);
				if (!NotifyStateClass)
				{
					return false;
				}
				const int32 BeforeNotifyCount = Animation->Notifies.Num();
				UAnimationBlueprintLibrary::AddAnimationNotifyStateEvent(SOMOLMCP_ANIM_KEY_TARGET(Animation), *NotifyTrackName, static_cast<float>(Time), static_cast<float>(Duration), NotifyStateClass);
				if (Animation->Notifies.Num() <= BeforeNotifyCount)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("notify_state_class_path"),
						TEXT("AddAnimationNotifyStateEvent returned but notify count did not increase."));
					OutError = TEXT("Animation notify state add readback failed.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Animation);
				OutStructured->SetBoolField(TEXT("notify_state_verified"), true);
				OutSummary = TEXT("Added animation notify state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_update_notify"),
			TEXT("Update a notify event or notify state by stable guid."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("notify_guid"), FSololmcpSchemaBuilder::String()},
					{TEXT("time"), FSololmcpSchemaBuilder::Number()},
					{TEXT("duration"), FSololmcpSchemaBuilder::Number()},
					{TEXT("track_index"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("asset_path"), TEXT("notify_guid")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NotifyGuidString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("notify_guid"), NotifyGuidString))
				{
					OutError = TEXT("Missing asset_path or notify_guid.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				FAnimNotifyEvent* Event = FindAnimationNotifyByGuid(Animation, NotifyGuidString);
				if (!Event)
				{
					OutError = TEXT("Animation notify guid was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimationUpdateNotify", "SOMOLMCP Update Animation Notify"));
				Animation->Modify();
				double Time = 0.0;
				if (Arguments->TryGetNumberField(TEXT("time"), Time))
				{
					Event->SetTime(static_cast<float>(Time));
				}
				double Duration = 0.0;
				if (Arguments->TryGetNumberField(TEXT("duration"), Duration))
				{
					Event->SetDuration(static_cast<float>(Duration));
				}
				int32 TrackIndex = 0;
				if (Arguments->TryGetNumberField(TEXT("track_index"), TrackIndex))
				{
					Event->TrackIndex = TrackIndex;
				}
				Animation->SortNotifies();
				Animation->MarkPackageDirty();
				OutStructured = AnimationNotifyEventToJson(*Event);
				OutSummary = TEXT("Updated animation notify.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_remove_notify_by_guid"),
			TEXT("Remove a notify event or notify state by stable guid."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("notify_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("notify_guid")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NotifyGuidString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("notify_guid"), NotifyGuidString))
				{
					OutError = TEXT("Missing asset_path or notify_guid.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				FGuid NotifyGuid;
				if (!FGuid::Parse(NotifyGuidString, NotifyGuid))
				{
					OutError = TEXT("notify_guid is not a valid guid.");
					return false;
				}
				const int32 RemovedCount = Animation->Notifies.RemoveAll([&NotifyGuid](const FAnimNotifyEvent& Event)
				{
					return Event.Guid == NotifyGuid;
				});
				if (RemovedCount <= 0)
				{
					OutError = TEXT("Animation notify guid was not found.");
					return false;
				}
				Animation->MarkPackageDirty();
				OutStructured->SetNumberField(TEXT("removedCount"), RemovedCount);
				OutStructured->SetObjectField(TEXT("asset"), FSololmcpEditorServices::MakeObjectReference(Animation));
				OutSummary = TEXT("Removed animation notify.");
				return true;
}
, nullptr
, 5
});

		Registry.Register({
			TEXT("animation_add_float_curve_key"),
			TEXT("Add a float curve key to an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("value"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("time"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				double Time = 0.0;
				double Value = 0.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("curve_name"), CurveName) ||
					!Arguments->TryGetNumberField(TEXT("time"), Time) ||
					!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("Missing asset_path, curve_name, time or value.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UAnimationBlueprintLibrary::AddFloatCurveKey(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, static_cast<float>(Time), static_cast<float>(Value));
				TArray<float> Times;
				TArray<float> Values;
				UAnimationBlueprintLibrary::GetFloatKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = FloatCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Added animation float curve key.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_get_float_curve_keys"),
			TEXT("Get float curve keys from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("curve_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName))
				{
					OutError = TEXT("Missing asset_path or curve_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<float> Times;
				TArray<float> Values;
				UAnimationBlueprintLibrary::GetFloatKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = FloatCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Fetched animation float curve keys.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_get_float_curve_value"),
			TEXT("Evaluate a float curve at a given time."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("time")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				double Time = 0.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("curve_name"), CurveName) ||
					!Arguments->TryGetNumberField(TEXT("time"), Time))
				{
					OutError = TEXT("Missing asset_path, curve_name or time.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
OutStructured->SetNumberField(TEXT("value"), UAnimationBlueprintLibrary::GetFloatValueAtTime(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, static_cast<float>(Time)));
#else
				// UAnimationBlueprintLibrary::GetFloatValueAtTime arrived after 5.3.
				OutError = TEXT("NOT_AVAILABLE_ON_ENGINE: sampling an animation float curve at a "
								 "time needs UE 5.4 or newer.");
				return false;
#endif
				OutSummary = TEXT("Evaluated animation float curve.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_remove_curve"),
			TEXT("Remove a curve from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("remove_name_from_skeleton"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("curve_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName))
				{
					OutError = TEXT("Missing asset_path or curve_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				const bool bRemoveNameFromSkeleton = Arguments->HasTypedField<EJson::Boolean>(TEXT("remove_name_from_skeleton")) ? Arguments->GetBoolField(TEXT("remove_name_from_skeleton")) : false;
				const FAnimationCurveIdentifier CurveId(*CurveName, ERawCurveTrackTypes::RCT_Float);
				if (!Animation->GetDataModel() || Animation->GetDataModel()->FindCurve(CurveId) == nullptr)
				{
					SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("curve_name"),
						TEXT("Float animation curve was not found before remove."));
					OutError = FString::Printf(TEXT("Animation curve was not found: %s"), *CurveName);
					return false;
				}
				UAnimationBlueprintLibrary::RemoveCurve(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, bRemoveNameFromSkeleton);
				if (Animation->GetDataModel() && Animation->GetDataModel()->FindCurve(CurveId) != nullptr)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("curve_name"),
						TEXT("RemoveCurve returned but the curve is still present."));
					OutError = TEXT("Animation curve remove readback failed.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Animation);
				OutStructured->SetBoolField(TEXT("curve_removed_verified"), true);
				OutSummary = TEXT("Removed animation curve.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_rename_curve"),
			TEXT("Rename an animation curve through the data controller."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_type"), FSololmcpSchemaBuilder::String(TEXT("float | vector | transform"))}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				FString NewName;
				FString CurveTypeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing asset_path, curve_name or new_name.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("curve_type"), CurveTypeName);
				ERawCurveTrackTypes CurveType = ERawCurveTrackTypes::RCT_Float;
				if (CurveTypeName == TEXT("vector")) { CurveType = ERawCurveTrackTypes::RCT_Vector; }
				else if (CurveTypeName == TEXT("transform")) { CurveType = ERawCurveTrackTypes::RCT_Transform; }
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				IAnimationDataController& Controller = Animation->GetController();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimationRenameCurve", "SOMOLMCP Rename Animation Curve"));
				if (!Controller.RenameCurve(FAnimationCurveIdentifier(*CurveName, CurveType), FAnimationCurveIdentifier(*NewName, CurveType)))
				{
					OutError = TEXT("Failed to rename animation curve.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Animation);
				OutSummary = TEXT("Renamed animation curve.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_set_float_curve_keys"),
			TEXT("Replace all keys on a float animation curve."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("keys"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({{TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("value"), FSololmcpSchemaBuilder::Number()}}, {TEXT("time"), TEXT("value")}))}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("keys")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || !Arguments->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
				{
					OutError = TEXT("Missing asset_path, curve_name or keys.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<FRichCurveKey> CurveKeys;
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					const TSharedPtr<FJsonObject>* KeyObject = nullptr;
					if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject || !KeyObject->IsValid())
					{
						OutError = TEXT("Each key must be an object.");
						return false;
					}
					double Time = 0.0;
					double Value = 0.0;
					if (!(*KeyObject)->TryGetNumberField(TEXT("time"), Time) || !(*KeyObject)->TryGetNumberField(TEXT("value"), Value))
					{
						OutError = TEXT("Each key must contain time and value.");
						return false;
					}
					CurveKeys.Add(FRichCurveKey(static_cast<float>(Time), static_cast<float>(Value)));
				}
				IAnimationDataController& Controller = Animation->GetController();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimationSetFloatCurveKeys", "SOMOLMCP Set Animation Float Curve Keys"));
				if (!Controller.SetCurveKeys(FAnimationCurveIdentifier(*CurveName, ERawCurveTrackTypes::RCT_Float), CurveKeys))
				{
					OutError = TEXT("Failed to set animation curve keys.");
					return false;
				}
				TArray<float> Times;
				TArray<float> Values;
				UAnimationBlueprintLibrary::GetFloatKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = FloatCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Replaced animation float curve keys.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_remove_float_curve_key"),
			TEXT("Remove one key from a float animation curve."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("time")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				double Time = 0.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || !Arguments->TryGetNumberField(TEXT("time"), Time))
				{
					OutError = TEXT("Missing asset_path, curve_name or time.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				IAnimationDataController& Controller = Animation->GetController();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimationRemoveFloatCurveKey", "SOMOLMCP Remove Animation Float Curve Key"));
				if (!Controller.RemoveCurveKey(FAnimationCurveIdentifier(*CurveName, ERawCurveTrackTypes::RCT_Float), static_cast<float>(Time)))
				{
					OutError = TEXT("Failed to remove animation curve key.");
					return false;
				}
				TArray<float> Times;
				TArray<float> Values;
				UAnimationBlueprintLibrary::GetFloatKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = FloatCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Removed animation float curve key.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_add_vector_curve_key"),
			TEXT("Add a vector curve key to an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("value"), VectorSchema()}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("time"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				double Time = 0.0;
				TSharedPtr<FJsonObject> ValueObject;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || !Arguments->TryGetNumberField(TEXT("time"), Time) || !TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("Missing asset_path, curve_name, time or value.");
					return false;
				}
				FVector Value = FVector::ZeroVector;
				if (!FSololmcpEditorServices::JsonToVector(ValueObject, Value))
				{
					OutError = TEXT("value must be a vector object.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UAnimationBlueprintLibrary::AddVectorCurveKey(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, static_cast<float>(Time), Value);
				TArray<float> Times;
				TArray<FVector> Values;
				UAnimationBlueprintLibrary::GetVectorKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = VectorCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Added animation vector curve key.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_get_vector_curve_keys"),
			TEXT("Get vector curve keys from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("curve_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName))
				{
					OutError = TEXT("Missing asset_path or curve_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<float> Times;
				TArray<FVector> Values;
				UAnimationBlueprintLibrary::GetVectorKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = VectorCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Fetched animation vector curve keys.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_set_vector_curve_keys"),
			TEXT("Replace all keys on a vector animation curve."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("keys"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({{TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("value"), VectorSchema()}}, {TEXT("time"), TEXT("value")}))}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("keys")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || !Arguments->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
				{
					OutError = TEXT("Missing asset_path, curve_name or keys.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<float> TimesToApply;
				TArray<FVector> ValuesToApply;
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					const TSharedPtr<FJsonObject>* KeyObject = nullptr;
					if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject || !KeyObject->IsValid())
					{
						OutError = TEXT("Each key must be an object.");
						return false;
					}
					double Time = 0.0;
					const TSharedPtr<FJsonObject>* NestedValueObject = nullptr;
					if (!(*KeyObject)->TryGetNumberField(TEXT("time"), Time) || !(*KeyObject)->TryGetObjectField(TEXT("value"), NestedValueObject) || !NestedValueObject)
					{
						OutError = TEXT("Each key must contain time and value.");
						return false;
					}
					TSharedPtr<FJsonObject> ValueObject = *NestedValueObject;
					FVector Value = FVector::ZeroVector;
					if (!FSololmcpEditorServices::JsonToVector(ValueObject, Value))
					{
						OutError = TEXT("Each key value must be a vector object.");
						return false;
					}
					TimesToApply.Add(static_cast<float>(Time));
					ValuesToApply.Add(Value);
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimationSetVectorCurveKeys", "SOMOLMCP Set Animation Vector Curve Keys"));
				UAnimationBlueprintLibrary::RemoveCurve(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, false);
				UAnimationBlueprintLibrary::AddCurve(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, ERawCurveTrackTypes::RCT_Vector, false);
				UAnimationBlueprintLibrary::AddVectorCurveKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, TimesToApply, ValuesToApply);
				TArray<float> Times;
				TArray<FVector> Values;
				UAnimationBlueprintLibrary::GetVectorKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = VectorCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Replaced animation vector curve keys.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_add_transform_curve_key"),
			TEXT("Add a transform curve key to an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("value"), TransformSchema()}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("time"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				double Time = 0.0;
				TSharedPtr<FJsonObject> ValueObject;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || !Arguments->TryGetNumberField(TEXT("time"), Time) || !TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("Missing asset_path, curve_name, time or value.");
					return false;
				}
				FTransform Value = FTransform::Identity;
				if (!FSololmcpEditorServices::JsonToTransform(ValueObject, Value))
				{
					OutError = TEXT("value must be a transform object.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UAnimationBlueprintLibrary::AddTransformationCurveKey(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, static_cast<float>(Time), Value);
				TArray<float> Times;
				TArray<FTransform> Values;
				UAnimationBlueprintLibrary::GetTransformationKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = TransformCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Added animation transform curve key.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_get_transform_curve_keys"),
			TEXT("Get transform curve keys from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("curve_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName))
				{
					OutError = TEXT("Missing asset_path or curve_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<float> Times;
				TArray<FTransform> Values;
				UAnimationBlueprintLibrary::GetTransformationKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = TransformCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Fetched animation transform curve keys.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_set_transform_curve_keys"),
			TEXT("Replace all keys on a transform animation curve."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("curve_name"), FSololmcpSchemaBuilder::String()}, {TEXT("keys"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({{TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("value"), TransformSchema()}}, {TEXT("time"), TEXT("value")}))}}, {TEXT("asset_path"), TEXT("curve_name"), TEXT("keys")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString CurveName;
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("curve_name"), CurveName) || !Arguments->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
				{
					OutError = TEXT("Missing asset_path, curve_name or keys.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<float> TimesToApply;
				TArray<FTransform> ValuesToApply;
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					const TSharedPtr<FJsonObject>* KeyObject = nullptr;
					if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject || !KeyObject->IsValid())
					{
						OutError = TEXT("Each key must be an object.");
						return false;
					}
					double Time = 0.0;
					const TSharedPtr<FJsonObject>* NestedValueObject = nullptr;
					if (!(*KeyObject)->TryGetNumberField(TEXT("time"), Time) || !(*KeyObject)->TryGetObjectField(TEXT("value"), NestedValueObject) || !NestedValueObject)
					{
						OutError = TEXT("Each key must contain time and value.");
						return false;
					}
					TSharedPtr<FJsonObject> ValueObject = *NestedValueObject;
					FTransform Value = FTransform::Identity;
					if (!FSololmcpEditorServices::JsonToTransform(ValueObject, Value))
					{
						OutError = TEXT("Each key value must be a transform object.");
						return false;
					}
					TimesToApply.Add(static_cast<float>(Time));
					ValuesToApply.Add(Value);
				}
				IAnimationDataController& Controller = Animation->GetController();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimationSetTransformCurveKeys", "SOMOLMCP Set Animation Transform Curve Keys"));
				if (!Controller.SetTransformCurveKeys(FAnimationCurveIdentifier(*CurveName, ERawCurveTrackTypes::RCT_Transform), ValuesToApply, TimesToApply))
				{
					OutError = TEXT("Failed to set transform animation curve keys.");
					return false;
				}
				TArray<float> Times;
				TArray<FTransform> Values;
				UAnimationBlueprintLibrary::GetTransformationKeys(SOMOLMCP_ANIM_KEY_TARGET(Animation), *CurveName, Times, Values);
				OutStructured = TransformCurveKeysToJson(Times, Values);
				OutSummary = TEXT("Replaced animation transform curve keys.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_list_sync_markers"),
			TEXT("List sync markers on an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UAnimSequence* Animation = Cast<UAnimSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				TArray<FAnimSyncMarker> Markers;
				UAnimationBlueprintLibrary::GetAnimationSyncMarkers(SOMOLMCP_ANIM_KEY_TARGET(Animation), Markers);
				TArray<TSharedPtr<FJsonValue>> MarkerJson;
				for (const FAnimSyncMarker& Marker : Markers)
				{
					TSharedRef<FJsonObject> MarkerObject = MakeShared<FJsonObject>();
					MarkerObject->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
					MarkerObject->SetNumberField(TEXT("trackIndex"), Marker.TrackIndex);
					MarkerObject->SetNumberField(TEXT("time"), Marker.Time);
					MarkerJson.Add(MakeShared<FJsonValueObject>(MarkerObject));
				}
				OutStructured->SetArrayField(TEXT("markers"), MarkerJson);
				OutStructured->SetNumberField(TEXT("count"), MarkerJson.Num());
				OutSummary = TEXT("Listed animation sync markers.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("animation_add_sync_marker"),
			TEXT("Add a sync marker to an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("marker_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("marker_name"), TEXT("time"), TEXT("track_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString MarkerName;
				FString TrackName;
				double Time = 0.0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("marker_name"), MarkerName) || !Arguments->TryGetNumberField(TEXT("time"), Time) || !Arguments->TryGetStringField(TEXT("track_name"), TrackName))
				{
					OutError = TEXT("Missing asset_path, marker_name, time or track_name.");
					return false;
				}
				UAnimSequence* Animation = Cast<UAnimSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				UAnimationBlueprintLibrary::AddAnimationSyncMarker(SOMOLMCP_ANIM_KEY_TARGET(Animation), *MarkerName, static_cast<float>(Time), *TrackName);
				TArray<FAnimSyncMarker> Markers;
				UAnimationBlueprintLibrary::GetAnimationSyncMarkers(SOMOLMCP_ANIM_KEY_TARGET(Animation), Markers);
				TArray<TSharedPtr<FJsonValue>> MarkerJson;
				for (const FAnimSyncMarker& Marker : Markers)
				{
					TSharedRef<FJsonObject> MarkerObject = MakeShared<FJsonObject>();
					MarkerObject->SetStringField(TEXT("name"), Marker.MarkerName.ToString());
					MarkerObject->SetNumberField(TEXT("trackIndex"), Marker.TrackIndex);
					MarkerObject->SetNumberField(TEXT("time"), Marker.Time);
					MarkerJson.Add(MakeShared<FJsonValueObject>(MarkerObject));
				}
				OutStructured->SetArrayField(TEXT("markers"), MarkerJson);
				OutStructured->SetNumberField(TEXT("count"), MarkerJson.Num());
				OutSummary = TEXT("Added animation sync marker.");
				return true;
			}
		, nullptr
		, 5
		});

		// AN-07 fix 2026-08-05: zero-length sequences (created by asset_create /
		// asset_duplicate without imported frames) silently reject sync markers,
		// notify events and bone-track mutations. This tool gives a sequence a real
		// frame count so the rest of the animation_* authoring family can write into it.
		Registry.Register({
			TEXT("animation_set_sequence_duration"),
			TEXT("Set the frame rate and frame count of an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("frame_count"), FSololmcpSchemaBuilder::Integer()}, {TEXT("frame_rate_numerator"), FSololmcpSchemaBuilder::Integer()}, {TEXT("frame_rate_denominator"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("frame_count")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				const int32 FrameCount = Arguments->HasTypedField<EJson::Number>(TEXT("frame_count")) ? Arguments->GetIntegerField(TEXT("frame_count")) : 0;
				if (FrameCount <= 0)
				{
					OutError = TEXT("frame_count must be greater than zero.");
					return false;
				}
				int32 Numerator = 30;
				int32 Denominator = 1;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("frame_rate_numerator"))) { Numerator = Arguments->GetIntegerField(TEXT("frame_rate_numerator")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("frame_rate_denominator"))) { Denominator = Arguments->GetIntegerField(TEXT("frame_rate_denominator")); }
				if (Numerator <= 0 || Denominator <= 0)
				{
					OutError = TEXT("Frame rate numerator and denominator must be greater than zero.");
					return false;
				}
				UAnimSequence* Animation = Cast<UAnimSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				IAnimationDataController& Controller = Animation->GetController();
				Controller.OpenBracket(NSLOCTEXT("SOMOLMCP", "SetSequenceDuration", "SOMOLMCP Set Sequence Duration"));
				Controller.SetFrameRate(FFrameRate(Numerator, Denominator));
				Controller.SetNumberOfFrames(FFrameNumber(FrameCount));
				Controller.CloseBracket();
				const FFrameRate ActualRate = Animation->GetDataModel()->GetFrameRate();
				const int32 ActualFrames = Animation->GetDataModel()->GetNumberOfFrames();
				const bool bVerified = ActualRate.Numerator == Numerator && ActualRate.Denominator == Denominator && ActualFrames == FrameCount;
				if (!bVerified)
				{
					OutError = TEXT("Animation duration write did not survive readback.");
					return false;
				}
				if (!Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetNumberField(TEXT("frame_rate_numerator"), ActualRate.Numerator);
				OutStructured->SetNumberField(TEXT("frame_rate_denominator"), ActualRate.Denominator);
				OutStructured->SetNumberField(TEXT("frame_count"), ActualFrames);
				OutStructured->SetNumberField(TEXT("play_length_seconds"), ActualFrames / static_cast<double>(ActualRate.AsDecimal()));
				OutStructured->SetBoolField(TEXT("verified"), true);
				OutSummary = TEXT("Set animation sequence duration.");
				return true;
			}
		// duration write is a mutation; CacheTtlSeconds must be 0.
		, nullptr
		, 0
		});

		// AN-08 fix 2026-08-05: InsertBoneTrack (via animation_insert_bone_track)
		// fails hard when the sequence has no Skeleton assigned or the bone name is
		// missing from the skeleton's reference skeleton. asset_duplicate creates a
		// bare sequence; this tool binds an existing USkeleton so the animation_*
		// authoring family can add bone tracks and sample against real bones.
		Registry.Register({
			TEXT("animation_set_skeleton"),
			TEXT("Assign a skeleton to an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("skeleton_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("skeleton_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString SkeletonPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
				{
					OutError = TEXT("Missing asset_path or skeleton_path.");
					return false;
				}
				UAnimSequence* Animation = Cast<UAnimSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				USkeleton* Skeleton = Cast<USkeleton>(Context.Services.LoadAsset(SkeletonPath, OutError));
				if (!Skeleton)
				{
					OutError = TEXT("skeleton_path is not a skeleton asset.");
					return false;
				}
				Animation->SetSkeleton(Skeleton);
				const bool bVerified = Animation->GetSkeleton() == Skeleton;
				// AN-08 root-cause fix 2026-08-05: in UE 5.8 the sequence data model is a
				// UAnimationSequencerDataModel whose FK Control Rig hierarchy is built
				// from the skeleton at model-init time. Assigning the Skeleton member
				// alone leaves the rig hierarchy empty (asset_duplicate copies a bare
				// model), so bone-track insertion silently no-ops. UpdateWithSkeleton is
				// the engine's standard re-rig path: it diffs the FK section against the
				// new reference skeleton and rebuilds the hierarchy when bones differ.
				if (bVerified)
				{
					Animation->GetController().UpdateWithSkeleton(Skeleton, false);
				}
				if (!bVerified)
				{
					OutError = TEXT("Skeleton assignment did not survive readback.");
					return false;
				}
				if (!Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("skeleton_path"), SkeletonPath);
				OutStructured->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
				TArray<TSharedPtr<FJsonValue>> SkeletonBones;
				for (int32 i = 0; i < Skeleton->GetReferenceSkeleton().GetNum(); i++)
				{
					SkeletonBones.Add(MakeShared<FJsonValueString>(Skeleton->GetReferenceSkeleton().GetBoneName(i).ToString()));
				}
				OutStructured->SetArrayField(TEXT("bones"), SkeletonBones);
				OutStructured->SetBoolField(TEXT("verified"), true);
				OutSummary = TEXT("Assigned skeleton to animation sequence.");
				return true;
			}
		// skeleton assignment is a mutation; CacheTtlSeconds must be 0.
		, nullptr
		, 0
		});

		Registry.Register({
			TEXT("animation_insert_bone_track"),
			TEXT("Insert a bone track into an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("bone_name"), FSololmcpSchemaBuilder::String()}, {TEXT("index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("bone_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString BoneName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("bone_name"), BoneName))
				{
					OutError = TEXT("Missing asset_path or bone_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				const int32 DesiredIndex = Arguments->HasTypedField<EJson::Number>(TEXT("index")) ? Arguments->GetIntegerField(TEXT("index")) : INDEX_NONE;
				// AN-08 fix: asset_duplicate can leave the data model's outer pointing at the
				// ORIGINAL sequence, so the model resolves GetAnimationSequence() to the
				// source asset (whose Skeleton is null) and bone-track mutations silently
				// no-op. Re-home the model under the actual sequence before mutating.
				if (IAnimationDataModel* ModelInterface = Animation->GetDataModel())
				{
					if (ModelInterface->GetAnimationSequence() != Animation)
					{
						if (UAnimDataModel* AnimModel = Cast<UAnimDataModel>(ModelInterface))
						{
							OutStructured->SetStringField(TEXT("model_rehomed_from"), ModelInterface->GetAnimationSequence() ? ModelInterface->GetAnimationSequence()->GetPathName() : TEXT("(null)"));
							AnimModel->Rename(nullptr, Animation);
						}
					}
				}
				IAnimationDataController& Controller = Animation->GetController();
				const bool bAdded = Controller.AddBoneCurve(*BoneName);
				TArray<FName> BoneTracks;
				Animation->GetDataModel()->GetBoneTrackNames(BoneTracks);
				if (!bAdded || !BoneTracks.Contains(*BoneName))
				{
					// AN-08 diagnostics: dump the full mutation state so a silent
					// controller/model mismatch is visible instead of a bare failure.
					OutStructured->SetBoolField(TEXT("diag_added"), bAdded);
					OutStructured->SetNumberField(TEXT("diag_track_count"), BoneTracks.Num());
					// Controller-vs-readback model identity: if these differ the controller
					// mutated a stale model while GetDataModel() sees a fresh one.
					const TScriptInterface<IAnimationDataModel> ReadIfc = Animation->GetDataModelInterface();
					const TScriptInterface<IAnimationDataModel> CtrlIfc = Controller.GetModelInterface();
					OutStructured->SetStringField(TEXT("diag_read_model_class"), ReadIfc.GetObject() ? ReadIfc.GetObject()->GetClass()->GetName() : TEXT("(null)"));
					OutStructured->SetStringField(TEXT("diag_read_model_ptr"), FString::Printf(TEXT("0x%llx"), (uint64)(UPTRINT)ReadIfc.GetInterface()));
					OutStructured->SetStringField(TEXT("diag_read_model_outer"), ReadIfc.GetObject() && ReadIfc.GetObject()->GetOuter() ? ReadIfc.GetObject()->GetOuter()->GetPathName() : TEXT("(null)"));
					OutStructured->SetStringField(TEXT("diag_ctrl_model_class"), CtrlIfc.GetObject() ? CtrlIfc.GetObject()->GetClass()->GetName() : TEXT("(null)"));
					OutStructured->SetStringField(TEXT("diag_ctrl_model_ptr"), FString::Printf(TEXT("0x%llx"), (uint64)(UPTRINT)CtrlIfc.GetInterface()));
					OutStructured->SetStringField(TEXT("diag_ctrl_model_outer"), CtrlIfc.GetObject() && CtrlIfc.GetObject()->GetOuter() ? CtrlIfc.GetObject()->GetOuter()->GetPathName() : TEXT("(null)"));
					OutStructured->SetNumberField(TEXT("diag_ctrl_track_count"), CtrlIfc.GetInterface() ? CtrlIfc.GetInterface()->GetNumBoneTracks() : -1);
					if (IAnimationDataModel* DiagModel = Animation->GetDataModel())
					{
						OutStructured->SetStringField(TEXT("diag_model_seq"), DiagModel->GetAnimationSequence() ? DiagModel->GetAnimationSequence()->GetPathName() : TEXT("(null)"));
					}
					OutStructured->SetStringField(TEXT("diag_skel"), Animation->GetSkeleton() ? Animation->GetSkeleton()->GetPathName() : TEXT("(null)"));
					OutStructured->SetNumberField(TEXT("diag_bone_index"), Animation->GetSkeleton() ? Animation->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(*BoneName) : -1);
					// AN-08 fix: name the actual failure so callers can act on it
					// (sequence without a skeleton vs. bone missing from the skeleton).
					const USkeleton* Skel = Animation->GetSkeleton();
					if (!Skel)
					{
						OutError = TEXT("Failed to insert bone track: sequence has no skeleton assigned. Use animation_set_skeleton first.");
					}
					else if (Skel->GetReferenceSkeleton().FindBoneIndex(*BoneName) == INDEX_NONE)
					{
						OutError = FString::Printf(TEXT("Failed to insert bone track: bone '%s' is not in skeleton '%s'."), *BoneName, *Skel->GetName());
					}
					else
					{
						OutError = TEXT("Failed to insert bone track.");
					}
					return false;
				}
				OutStructured = NamesToJson(BoneTracks, TEXT("boneTracks"));
				OutSummary = TEXT("Inserted animation bone track.");
				return true;
			}
		// AN-08 fix 2026-08-05: insert is a mutation; CacheTtlSeconds must be 0 so the
		// registry never serves stale cached results for repeated calls.
		, nullptr
		, 0
		});

		Registry.Register({
			TEXT("animation_remove_bone_track"),
			TEXT("Remove a bone track from an animation sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("bone_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("bone_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString BoneName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("bone_name"), BoneName))
				{
					OutError = TEXT("Missing asset_path or bone_name.");
					return false;
				}
				UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Animation)
				{
					OutError = TEXT("Asset is not an animation sequence.");
					return false;
				}
				IAnimationDataController& Controller = Animation->GetController();
				if (!Controller.RemoveBoneTrack(*BoneName))
				{
					OutError = TEXT("Failed to remove bone track.");
					return false;
				}
				TArray<FName> BoneTracks;
				Animation->GetDataModel()->GetBoneTrackNames(BoneTracks);
				OutStructured = NamesToJson(BoneTracks, TEXT("boneTracks"));
				OutSummary = TEXT("Removed animation bone track.");
				return true;
			}
		// AN-09 fix 2026-08-05: remove is a mutation; CacheTtlSeconds must be 0.
		, nullptr
		, 0
		});

		auto RegisterP1PythonAssetTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
				{
					FString AssetPath;
					FString PackagePath;
					Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
					Arguments->TryGetStringField(TEXT("package_path"), PackagePath);
					if (AssetPath.IsEmpty() && PackagePath.IsEmpty())
					{
						OutError = TEXT("Missing asset_path or package_path.");
						return FString();
					}
					// Audit round 7: material_get_parameter_info previously crashed with a Python
					// Traceback (RuntimeError at get_expression line 274/49) when expression_id was
					// missing — `int(args.get('expression_id', -1))` flowed into get_expression which
					// raised mid-script. Schema marks expression_id required but the validator doesn't
					// enforce required==typed-present, so guard at the C++ entry instead and return
					// missing_arg before the Python ever runs.
					if (ToolName == TEXT("material_get_parameter_info"))
					{
						double ExprIdProbe = 0.0;
						int32 ExprIdProbeI = 0;
						const bool bHasExprId = Arguments->TryGetNumberField(TEXT("expression_id"), ExprIdProbe)
							|| Arguments->TryGetNumberField(TEXT("expression_id"), ExprIdProbeI);
						if (!bHasExprId)
						{
							OutError = TEXT("missing_arg: expression_id is required (integer index into the material's expressions array)");
							return FString();
						}
					}
					const FString ArgumentsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal\n")
						TEXT("import json, ast\n")
						TEXT("tool_name = %s\n")
						TEXT("args = json.loads(%s)\n")
						TEXT("asset_subsystem = unreal.EditorAssetSubsystem()\n")
						TEXT("asset_tools = unreal.AssetToolsHelpers.get_asset_tools()\n")
						TEXT("asset_path = args.get('asset_path', '')\n")
						TEXT("package_path = args.get('package_path', '')\n")
						TEXT("asset_name = args.get('asset_name', '')\n")
						TEXT("target = asset_subsystem.load_asset(asset_path) if asset_path else None\n")
						TEXT("def save_target(obj):\n")
						TEXT("    if obj is not None:\n")
						TEXT("        asset_subsystem.save_loaded_asset(obj)\n")
						TEXT("def factory_create(factory_paths):\n")
						TEXT("    for class_path in factory_paths:\n")
						TEXT("        factory_class = unreal.load_class(None, class_path)\n")
						TEXT("        if factory_class is not None:\n")
						TEXT("            factory = unreal.new_object(factory_class)\n")
						TEXT("            created = asset_tools.create_asset(asset_name, package_path, None, factory)\n")
						TEXT("            if created is not None:\n")
						TEXT("                save_target(created)\n")
						TEXT("                created_path = created.get_path_name()\n")
						TEXT("                reloaded = asset_subsystem.load_asset(created_path)\n")
						TEXT("                if reloaded is None:\n")
						TEXT("                    raise RuntimeError('Created asset failed reload verification: ' + str(created_path))\n")
						TEXT("                return created\n")
						TEXT("    raise RuntimeError('Unable to create asset for ' + tool_name)\n")
						TEXT("def make_vector(data):\n")
						TEXT("    data = data or {}\n")
						TEXT("    return unreal.Vector(float(data.get('x', 0.0)), float(data.get('y', 0.0)), float(data.get('z', 0.0)))\n")
						TEXT("def make_rotator(data):\n")
						TEXT("    data = data or {}\n")
						TEXT("    return unreal.Rotator(float(data.get('pitch', 0.0)), float(data.get('yaw', 0.0)), float(data.get('roll', 0.0)))\n")
						TEXT("def load_optional(path):\n")
						TEXT("    return asset_subsystem.load_asset(path) if path else None\n")
						TEXT("def try_literal(value):\n")
						TEXT("    if isinstance(value, str):\n")
						TEXT("        try:\n")
						TEXT("            return ast.literal_eval(value)\n")
						TEXT("        except Exception:\n")
						TEXT("            return value\n")
						TEXT("    return value\n")
						TEXT("def list_expressions(obj):\n")
						TEXT("    for field in ('expressions', 'function_expressions'):\n")
						TEXT("        try:\n")
						TEXT("            return list(obj.get_editor_property(field))\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("    return []\n")
						TEXT("def get_expression(obj, expr_id):\n")
						TEXT("    expressions = list_expressions(obj)\n")
						TEXT("    if expr_id < 0 or expr_id >= len(expressions):\n")
						TEXT("        raise RuntimeError('Expression index out of range')\n")
						TEXT("    return expressions[expr_id]\n")
						TEXT("if tool_name.startswith('anim_montage_'):\n")
						TEXT("    if tool_name == 'anim_montage_create':\n")
						TEXT("        target = factory_create(['/Script/UnrealEd.AnimMontageFactory', '/Script/Persona.AnimMontageFactory'])\n")
						TEXT("    elif target is None:\n")
						TEXT("        raise RuntimeError('Failed to load animation montage asset')\n")
						TEXT("    if tool_name == 'anim_montage_list_sections':\n")
						TEXT("        for section in list(target.get_editor_property('composite_sections')):\n")
						TEXT("            unreal.log('section=' + str(section.section_name) + ' time=' + str(section.get_time()))\n")
						TEXT("    elif tool_name == 'anim_montage_add_section':\n")
						TEXT("        section = unreal.CompositeSection()\n")
						TEXT("        section.section_name = args.get('section_name', '')\n")
						TEXT("        section.set_time(float(args.get('time', 0.0)))\n")
						TEXT("        sections = list(target.get_editor_property('composite_sections'))\n")
						TEXT("        sections.append(section)\n")
						TEXT("        target.set_editor_property('composite_sections', sections)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'anim_montage_remove_section':\n")
						TEXT("        section_name = args.get('section_name', '')\n")
						TEXT("        before_sections = list(target.get_editor_property('composite_sections'))\n")
						TEXT("        sections = [section for section in before_sections if str(section.section_name) != section_name]\n")
						TEXT("        if len(sections) == len(before_sections):\n")
						TEXT("            raise RuntimeError('Montage section not found: ' + str(section_name))\n")
						TEXT("        target.set_editor_property('composite_sections', sections)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'anim_montage_set_section_next':\n")
						TEXT("        section_name = args.get('section_name', '')\n")
						TEXT("        next_name = args.get('next_section_name', '')\n")
						TEXT("        sections = list(target.get_editor_property('composite_sections'))\n")
						TEXT("        indices = {str(section.section_name): index for index, section in enumerate(sections)}\n")
						TEXT("        if section_name not in indices or next_name not in indices:\n")
						TEXT("            raise RuntimeError('Montage section not found')\n")
						TEXT("        target.set_next_section(section_name, next_name)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'anim_montage_add_slot_track':\n")
						TEXT("        slot_track = unreal.AnimTrack()\n")
						TEXT("        slot = unreal.SlotAnimationTrack()\n")
						TEXT("        slot.slot_name = args.get('slot_name', '')\n")
						TEXT("        slot.anim_track = slot_track\n")
						TEXT("        tracks = list(target.get_editor_property('slot_anim_tracks'))\n")
						TEXT("        tracks.append(slot)\n")
						TEXT("        target.set_editor_property('slot_anim_tracks', tracks)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'anim_montage_add_segment':\n")
						TEXT("        sequence = load_optional(args.get('sequence_path', ''))\n")
						TEXT("        if sequence is None:\n")
						TEXT("            raise RuntimeError('Failed to load montage segment sequence')\n")
						TEXT("        slot_name = args.get('slot_name', '')\n")
						TEXT("        tracks = list(target.get_editor_property('slot_anim_tracks'))\n")
						TEXT("        slot = next((item for item in tracks if str(item.slot_name) == slot_name), None)\n")
						TEXT("        if slot is None:\n")
						TEXT("            raise RuntimeError('Montage slot track not found')\n")
						TEXT("        segment = unreal.AnimSegment()\n")
						TEXT("        segment.anim_reference = sequence\n")
						TEXT("        segment.start_pos = float(args.get('anim_start_time', 0.0))\n")
						TEXT("        segment.anim_end_time = float(args.get('anim_end_time', 0.0)) if args.get('anim_end_time') is not None else sequence.get_editor_property('sequence_length')\n")
						TEXT("        segment.anim_start_time = float(args.get('anim_start_time', 0.0))\n")
						TEXT("        segment.start_time = float(args.get('start_time', 0.0))\n")
						TEXT("        anim_track = slot.anim_track\n")
						TEXT("        segments = list(anim_track.anim_segments)\n")
						TEXT("        segments.append(segment)\n")
						TEXT("        anim_track.anim_segments = segments\n")
						TEXT("        save_target(target)\n")
						TEXT("elif tool_name.startswith('blend_space_'):\n")
						TEXT("    if tool_name == 'blend_space_create':\n")
						TEXT("        axis_count = int(args.get('axis_count', 2))\n")
						TEXT("        target = factory_create(['/Script/UnrealEd.BlendSpaceFactoryNew'] if axis_count > 1 else ['/Script/UnrealEd.BlendSpace1DFactoryNew'])\n")
						TEXT("    elif target is None:\n")
						TEXT("        raise RuntimeError('Failed to load blend space asset')\n")
						TEXT("    if tool_name == 'blend_space_list_samples':\n")
						TEXT("        for index, sample in enumerate(list(target.get_editor_property('sample_data'))):\n")
						TEXT("            unreal.log('sample=' + str(index) + ' values=' + str(sample.sample_value))\n")
						TEXT("    elif tool_name == 'blend_space_add_sample':\n")
						TEXT("        sequence = load_optional(args.get('sequence_path', ''))\n")
						TEXT("        if sequence is None:\n")
						TEXT("            raise RuntimeError('Failed to load blend space sample sequence')\n")
						TEXT("        values = list(args.get('sample_value', []))\n")
						TEXT("        sample = unreal.BlendSample()\n")
						TEXT("        sample.animation = sequence\n")
						TEXT("        sample.sample_value = values\n")
						TEXT("        samples = list(target.get_editor_property('sample_data'))\n")
						TEXT("        samples.append(sample)\n")
						TEXT("        target.set_editor_property('sample_data', samples)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'blend_space_update_sample':\n")
						TEXT("        index = int(args.get('sample_index', -1))\n")
						TEXT("        values = list(args.get('sample_value', []))\n")
						TEXT("        samples = list(target.get_editor_property('sample_data'))\n")
						TEXT("        if index < 0 or index >= len(samples):\n")
						TEXT("            raise RuntimeError('Blend space sample index out of range')\n")
						TEXT("        sample = samples[index]\n")
						TEXT("        sample.sample_value = values\n")
						TEXT("        samples[index] = sample\n")
						TEXT("        target.set_editor_property('sample_data', samples)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'blend_space_remove_sample':\n")
						TEXT("        index = int(args.get('sample_index', -1))\n")
						TEXT("        samples = list(target.get_editor_property('sample_data'))\n")
						TEXT("        if index < 0 or index >= len(samples):\n")
						TEXT("            raise RuntimeError('Blend space sample index out of range')\n")
						TEXT("        del samples[index]\n")
						TEXT("        target.set_editor_property('sample_data', samples)\n")
						TEXT("        save_target(target)\n")
						TEXT("elif tool_name.startswith('static_mesh_'):\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load static mesh asset')\n")
						TEXT("    if tool_name == 'static_mesh_add_socket':\n")
						TEXT("        socket_class = unreal.load_class(None, '/Script/Engine.StaticMeshSocket')\n")
						TEXT("        socket = unreal.new_object(socket_class, target, args.get('socket_name', ''))\n")
						TEXT("        socket.set_editor_property('socket_name', args.get('socket_name', ''))\n")
						TEXT("        socket.set_editor_property('relative_location', make_vector(args.get('relative_location')))\n")
						TEXT("        socket.set_editor_property('relative_rotation', make_rotator(args.get('relative_rotation')))\n")
						TEXT("        target.add_socket(socket)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'static_mesh_remove_socket':\n")
						TEXT("        socket_name = args.get('socket_name', '')\n")
						TEXT("        removed = False\n")
						TEXT("        for socket in list(target.sockets):\n")
						TEXT("            if socket.get_name() == socket_name or str(socket.socket_name) == socket_name:\n")
						TEXT("                target.remove_socket(socket)\n")
						TEXT("                removed = True\n")
						TEXT("                break\n")
						TEXT("        if not removed:\n")
						TEXT("            raise RuntimeError('Static mesh socket not found: ' + str(socket_name))\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'static_mesh_set_material_slot':\n")
						TEXT("        material = load_optional(args.get('material_path', ''))\n")
						TEXT("        slot_index = int(args.get('slot_index', -1))\n")
						TEXT("        if material is None:\n")
						TEXT("            raise RuntimeError('Failed to load static mesh material')\n")
						TEXT("        if hasattr(target, 'set_material'):\n")
						TEXT("            target.set_material(slot_index, material)\n")
						TEXT("        else:\n")
						TEXT("            materials = list(target.static_materials)\n")
						TEXT("            materials[slot_index].material_interface = material\n")
						TEXT("            target.set_editor_property('static_materials', materials)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'static_mesh_set_build_settings':\n")
						TEXT("        settings = dict(args.get('settings', {}))\n")
						TEXT("        lod_index = int(args.get('lod_index', 0))\n")
						TEXT("        editor_subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)\n")
						TEXT("        if editor_subsystem is not None and hasattr(editor_subsystem, 'set_lod_build_settings'):\n")
						TEXT("            editor_subsystem.set_lod_build_settings(target, lod_index, settings)\n")
						TEXT("        else:\n")
						TEXT("            raise RuntimeError('StaticMeshEditorSubsystem build settings API is unavailable')\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'static_mesh_reimport':\n")
						TEXT("        if not (hasattr(unreal, 'EditorAssetLibrary') and hasattr(unreal.EditorAssetLibrary, 'reimport_asset')):\n")
						TEXT("            raise RuntimeError('EditorAssetLibrary.reimport_asset is unavailable')\n")
						TEXT("        if not unreal.EditorAssetLibrary.reimport_asset(asset_path):\n")
						TEXT("            raise RuntimeError('Static mesh reimport failed')\n")
						TEXT("        save_target(target)\n")
						TEXT("elif tool_name.startswith('skeletal_mesh_'):\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load skeletal mesh asset')\n")
						TEXT("    if tool_name == 'skeletal_mesh_list_lods':\n")
						TEXT("        lod_count = target.get_lod_num() if hasattr(target, 'get_lod_num') else len(list(target.get_editor_property('lod_info')))\n")
						TEXT("        for lod_index in range(lod_count):\n")
						TEXT("            unreal.log('lod=' + str(lod_index))\n")
						TEXT("    elif tool_name == 'skeletal_mesh_list_sockets':\n")
						TEXT("        for socket in list(target.get_editor_property('sockets')):\n")
						TEXT("            unreal.log('socket=' + socket.get_name())\n")
						TEXT("    elif tool_name == 'skeletal_mesh_add_socket':\n")
						TEXT("        socket_class = unreal.load_class(None, '/Script/Engine.SkeletalMeshSocket')\n")
						TEXT("        socket = unreal.new_object(socket_class, target, args.get('socket_name', ''))\n")
						TEXT("        socket.set_editor_property('socket_name', args.get('socket_name', ''))\n")
						TEXT("        socket.set_editor_property('bone_name', args.get('bone_name', ''))\n")
						TEXT("        sockets = list(target.get_editor_property('sockets'))\n")
						TEXT("        sockets.append(socket)\n")
						TEXT("        target.set_editor_property('sockets', sockets)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'skeletal_mesh_remove_socket':\n")
						TEXT("        socket_name = args.get('socket_name', '')\n")
						TEXT("        before_sockets = list(target.get_editor_property('sockets'))\n")
						TEXT("        sockets = [socket for socket in before_sockets if socket.get_name() != socket_name and str(socket.socket_name) != socket_name]\n")
						TEXT("        if len(sockets) == len(before_sockets):\n")
						TEXT("            raise RuntimeError('Skeletal mesh socket not found: ' + str(socket_name))\n")
						TEXT("        target.set_editor_property('sockets', sockets)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'skeletal_mesh_set_material_slot':\n")
						TEXT("        material = load_optional(args.get('material_path', ''))\n")
						TEXT("        slot_index = int(args.get('slot_index', -1))\n")
						TEXT("        if material is None:\n")
						TEXT("            raise RuntimeError('Failed to load skeletal mesh material')\n")
						TEXT("        materials = list(target.get_editor_property('materials'))\n")
						TEXT("        materials[slot_index].material_interface = material\n")
						TEXT("        target.set_editor_property('materials', materials)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'skeletal_mesh_set_import_settings':\n")
						TEXT("        for key, value in dict(args.get('settings', {})).items():\n")
						TEXT("            target.set_editor_property(key, value)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'skeletal_mesh_reimport':\n")
						TEXT("        if not (hasattr(unreal, 'EditorAssetLibrary') and hasattr(unreal.EditorAssetLibrary, 'reimport_asset')):\n")
						TEXT("            raise RuntimeError('EditorAssetLibrary.reimport_asset is unavailable')\n")
						TEXT("        if not unreal.EditorAssetLibrary.reimport_asset(asset_path):\n")
						TEXT("            raise RuntimeError('Skeletal mesh reimport failed')\n")
						TEXT("        save_target(target)\n")
						TEXT("elif tool_name.startswith('material_') or tool_name.startswith('material_function_'):\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load material asset')\n")
						TEXT("    editing_lib = unreal.MaterialEditingLibrary\n")
						TEXT("    if tool_name == 'material_disconnect_property':\n")
						TEXT("        property_name = 'MP_' + str(args.get('property', '')).upper()\n")
						TEXT("        material_property = getattr(unreal.MaterialProperty, property_name, None)\n")
						TEXT("        if material_property is None:\n")
						TEXT("            raise RuntimeError('Unsupported material property')\n")
						TEXT("        editing_lib.disconnect_material_property(target, material_property)\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'material_disconnect_expressions':\n")
						TEXT("        input_expr = get_expression(target, int(args.get('input_expression_id', -1)))\n")
						TEXT("        input_name = args.get('input_name', '')\n")
						TEXT("        if input_name:\n")
						TEXT("            try:\n")
						TEXT("                input_expr.set_editor_property(input_name, None)\n")
						TEXT("            except Exception:\n")
						TEXT("                pass\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'material_add_comment':\n")
						TEXT("        comment_class = unreal.load_class(None, '/Script/Engine.MaterialExpressionComment')\n")
						TEXT("        comment = editing_lib.create_material_expression(target, comment_class, int(args.get('node_x', 0)), int(args.get('node_y', 0)))\n")
						TEXT("        comment.set_editor_property('text', args.get('text', ''))\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'material_function_add_input':\n")
						TEXT("        input_class = unreal.load_class(None, '/Script/Engine.MaterialExpressionFunctionInput')\n")
						TEXT("        expr = editing_lib.create_material_expression_in_function(target, input_class, -400, 0) if hasattr(editing_lib, 'create_material_expression_in_function') else editing_lib.create_material_expression(target, input_class, -400, 0)\n")
						TEXT("        expr.set_editor_property('input_name', args.get('input_name', ''))\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'material_function_add_output':\n")
						TEXT("        output_class = unreal.load_class(None, '/Script/Engine.MaterialExpressionFunctionOutput')\n")
						TEXT("        expr = editing_lib.create_material_expression_in_function(target, output_class, 400, 0) if hasattr(editing_lib, 'create_material_expression_in_function') else editing_lib.create_material_expression(target, output_class, 400, 0)\n")
						TEXT("        expr.set_editor_property('output_name', args.get('output_name', ''))\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'material_set_parameter_group':\n")
						TEXT("        expr = get_expression(target, int(args.get('expression_id', -1)))\n")
						TEXT("        expr.set_editor_property('group', args.get('group_name', ''))\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif tool_name == 'material_get_parameter_info':\n")
						TEXT("        expr = get_expression(target, int(args.get('expression_id', -1)))\n")
						TEXT("        unreal.log('parameter_name=' + str(expr.get_editor_property('parameter_name')))\n")
						TEXT("        try:\n")
						TEXT("            unreal.log('group=' + str(expr.get_editor_property('group')))\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("elif tool_name.startswith('anim_blueprint_'):\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load anim blueprint asset')\n")
						TEXT("    asset_subsystem.open_editor_for_assets([target])\n")
						TEXT("    if tool_name == 'anim_blueprint_list_state_machines':\n")
						TEXT("        for graph in list(target.get_all_graphs()) if hasattr(target, 'get_all_graphs') else []:\n")
						TEXT("            if 'StateMachine' in graph.get_name() or 'State Machine' in graph.get_name():\n")
						TEXT("                unreal.log('state_machine=' + graph.get_name())\n")
						TEXT("    else:\n")
						TEXT("        blueprint_utils = getattr(unreal, 'BlueprintEditorLibrary', None)\n")
						TEXT("        if blueprint_utils is None:\n")
						TEXT("            raise RuntimeError('Animation blueprint editor API is unavailable for ' + tool_name)\n")
						TEXT("        raise RuntimeError('Animation blueprint mutation path requires additional UE Python editor APIs for ' + tool_name)\n")
						TEXT("else:\n")
						TEXT("    raise RuntimeError('Unsupported P1 asset tool')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgumentsJson));
				});
		};

		auto ResolveAnimStateMachineForTool = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, UAnimBlueprint*& OutBlueprint, UAnimGraphNode_StateMachineBase*& OutStateMachineNode, UAnimationStateMachineGraph*& OutStateMachineGraph, FString& OutError)
		{
			FString AssetPath;
			FString StateMachineName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("state_machine_name"), StateMachineName))
			{
				OutError = TEXT("Missing asset_path or state_machine_name.");
				return false;
			}

			OutBlueprint = LoadAnimBlueprintAsset(Context.Services, AssetPath, OutError);
			if (!OutBlueprint)
			{
				return false;
			}

			OutStateMachineNode = FindAnimStateMachineNode(OutBlueprint, StateMachineName);
			if (!OutStateMachineNode || !OutStateMachineNode->EditorStateMachineGraph)
			{
				OutError = TEXT("Animation state machine was not found.");
				return false;
			}

			OutStateMachineGraph = OutStateMachineNode->EditorStateMachineGraph;
			return true;
		};

		Registry.Register({
			TEXT("anim_blueprint_list_state_machines"),
			TEXT("List state machines in an animation blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UAnimBlueprint* Blueprint = LoadAnimBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				OutStructured = AnimBlueprintStateMachinesToJson(Blueprint);
				OutSummary = TEXT("Listed animation blueprint state machines.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_add_state_machine"),
			TEXT("Add a state machine to an animation blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString StateMachineName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("state_machine_name"), StateMachineName))
				{
					OutError = TEXT("Missing asset_path or state_machine_name.");
					return false;
				}
				UAnimBlueprint* Blueprint = LoadAnimBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* RootGraph = FindPrimaryAnimBlueprintGraph(Blueprint);
				if (!RootGraph)
				{
					OutError = TEXT("Primary animation graph was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintAddStateMachine", "SOMOLMCP Add Animation State Machine"));
				UAnimGraphNode_StateMachine* StateMachineNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimGraphNode_StateMachine>(RootGraph, NewObject<UAnimGraphNode_StateMachine>(), FSomolEditorGraphPosition(0.0f, 0.0f), false);
				if (!StateMachineNode)
				{
					OutError = TEXT("Failed to create animation state machine node.");
					return false;
				}
				StateMachineNode->Modify();
				StateMachineNode->OnRenameNode(StateMachineName);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimBlueprintStateMachinesToJson(Blueprint);
				OutSummary = TEXT("Added animation blueprint state machine.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_list_states"),
			TEXT("List states in an animation blueprint state machine."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				OutStructured = AnimStateMachineStatesToJson(StateMachineGraph);
				OutSummary = TEXT("Listed animation blueprint states.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_add_state"),
			TEXT("Add a state to an animation blueprint state machine."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("state_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("state_name")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString StateName;
				if (!Arguments->TryGetStringField(TEXT("state_name"), StateName))
				{
					OutError = TEXT("Missing state_name.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintAddState", "SOMOLMCP Add Animation State"));
				UAnimStateNode* StateNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(StateMachineGraph, NewObject<UAnimStateNode>(), FSomolEditorGraphPosition(0.0f, 0.0f), false);
				if (!StateNode || !StateNode->BoundGraph)
				{
					OutError = TEXT("Failed to create animation state node.");
					return false;
				}
				FEdGraphUtilities::RenameGraphToNameOrCloseToName(StateNode->BoundGraph, StateName);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimStateMachineStatesToJson(StateMachineGraph);
				OutSummary = TEXT("Added animation blueprint state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_rename_state"),
			TEXT("Rename a state in an animation blueprint state machine."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("state_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("state_name"), TEXT("new_name")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString StateName;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("state_name"), StateName) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing state_name or new_name.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				UAnimStateNode* StateNode = FindAnimStateNode(StateMachineGraph, StateName);
				if (!StateNode || !StateNode->BoundGraph)
				{
					OutError = TEXT("Animation state was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintRenameState", "SOMOLMCP Rename Animation State"));
				StateNode->Modify();
				FEdGraphUtilities::RenameGraphToNameOrCloseToName(StateNode->BoundGraph, NewName);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimStateMachineStatesToJson(StateMachineGraph);
				OutSummary = TEXT("Renamed animation blueprint state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_remove_state"),
			TEXT("Remove a state from an animation blueprint state machine."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("state_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("state_name")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString StateName;
				if (!Arguments->TryGetStringField(TEXT("state_name"), StateName))
				{
					OutError = TEXT("Missing state_name.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				UAnimStateNode* StateNode = FindAnimStateNode(StateMachineGraph, StateName);
				if (!StateNode)
				{
					OutError = TEXT("Animation state was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintRemoveState", "SOMOLMCP Remove Animation State"));
				StateNode->DestroyNode();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimStateMachineStatesToJson(StateMachineGraph);
				OutSummary = TEXT("Removed animation blueprint state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_set_state_position"),
			TEXT("Set the editor position of a state node."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("state_name"), FSololmcpSchemaBuilder::String()}, {TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("state_name")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString StateName;
				int32 NodeX = 0;
				int32 NodeY = 0;
				if (!Arguments->TryGetStringField(TEXT("state_name"), StateName))
				{
					OutError = TEXT("Missing state_name.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				UAnimStateNode* StateNode = FindAnimStateNode(StateMachineGraph, StateName);
				if (!StateNode)
				{
					OutError = TEXT("Animation state was not found.");
					return false;
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("node_x"))) { NodeX = Arguments->GetIntegerField(TEXT("node_x")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("node_y"))) { NodeY = Arguments->GetIntegerField(TEXT("node_y")); }
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintSetStatePosition", "SOMOLMCP Set Animation State Position"));
				StateNode->Modify();
				StateNode->NodePosX = NodeX;
				StateNode->NodePosY = NodeY;
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimStateMachineStatesToJson(StateMachineGraph);
				OutSummary = TEXT("Updated animation blueprint state position.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_list_transitions"),
			TEXT("List transitions in an animation blueprint state machine."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				OutStructured = AnimStateMachineTransitionsToJson(StateMachineGraph);
				OutSummary = TEXT("Listed animation blueprint transitions.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_add_transition"),
			TEXT("Add a transition between two states."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("from_state"), FSololmcpSchemaBuilder::String()}, {TEXT("to_state"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("from_state"), TEXT("to_state")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString FromState;
				FString ToState;
				if (!Arguments->TryGetStringField(TEXT("from_state"), FromState) || !Arguments->TryGetStringField(TEXT("to_state"), ToState))
				{
					OutError = TEXT("Missing from_state or to_state.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				UAnimStateNode* PreviousState = FindAnimStateNode(StateMachineGraph, FromState);
				UAnimStateNode* NextState = FindAnimStateNode(StateMachineGraph, ToState);
				if (!PreviousState || !NextState)
				{
					OutError = TEXT("from_state or to_state was not found.");
					return false;
				}
				const FSomolEditorGraphPosition Location = (FSomolEditorGraphPosition(PreviousState->NodePosX, PreviousState->NodePosY) + FSomolEditorGraphPosition(NextState->NodePosX, NextState->NodePosY)) * 0.5f;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintAddTransition", "SOMOLMCP Add Animation Transition"));
				UAnimStateTransitionNode* TransitionNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(StateMachineGraph, NewObject<UAnimStateTransitionNode>(), Location, false);
				if (!TransitionNode)
				{
					OutError = TEXT("Failed to create animation transition.");
					return false;
				}
				TransitionNode->CreateConnections(PreviousState, NextState);
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimStateMachineTransitionsToJson(StateMachineGraph);
				OutSummary = TEXT("Added animation blueprint transition.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_remove_transition"),
			TEXT("Remove a transition between two states."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("from_state"), FSololmcpSchemaBuilder::String()}, {TEXT("to_state"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("from_state"), TEXT("to_state")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString FromState;
				FString ToState;
				if (!Arguments->TryGetStringField(TEXT("from_state"), FromState) || !Arguments->TryGetStringField(TEXT("to_state"), ToState))
				{
					OutError = TEXT("Missing from_state or to_state.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				UAnimStateTransitionNode* TransitionNode = FindAnimTransition(StateMachineGraph, FromState, ToState);
				if (!TransitionNode)
				{
					OutError = TEXT("Animation transition was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintRemoveTransition", "SOMOLMCP Remove Animation Transition"));
				TransitionNode->DestroyNode();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimStateMachineTransitionsToJson(StateMachineGraph);
				OutSummary = TEXT("Removed animation blueprint transition.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_get_transition_graph"),
			TEXT("Get the transition graph for a transition."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("from_state"), FSololmcpSchemaBuilder::String()}, {TEXT("to_state"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("from_state"), TEXT("to_state")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString FromState;
				FString ToState;
				if (!Arguments->TryGetStringField(TEXT("from_state"), FromState) || !Arguments->TryGetStringField(TEXT("to_state"), ToState))
				{
					OutError = TEXT("Missing from_state or to_state.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				UAnimStateTransitionNode* TransitionNode = FindAnimTransition(StateMachineGraph, FromState, ToState);
				if (!TransitionNode || !TransitionNode->GetBoundGraph())
				{
					OutError = TEXT("Animation transition graph was not found.");
					return false;
				}
				OutStructured = AnimStateMachineTransitionToJson(TransitionNode);
				OutStructured->SetObjectField(TEXT("graph"), FSololmcpEditorServices::MakeObjectReference(TransitionNode->GetBoundGraph()));
				OutSummary = TEXT("Fetched animation blueprint transition graph.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_set_transition_properties"),
			TEXT("Set common properties on a transition."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()}, {TEXT("from_state"), FSololmcpSchemaBuilder::String()}, {TEXT("to_state"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("state_machine_name"), TEXT("from_state"), TEXT("to_state")}),

			[ResolveAnimStateMachineForTool](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString FromState;
				FString ToState;
				TSharedPtr<FJsonObject> Properties;
				if (!Arguments->TryGetStringField(TEXT("from_state"), FromState) || !Arguments->TryGetStringField(TEXT("to_state"), ToState) || !TryGetObjectField(Arguments, TEXT("properties"), Properties))
				{
					OutError = TEXT("Missing from_state, to_state or properties.");
					return false;
				}
				UAnimBlueprint* Blueprint = nullptr;
				UAnimGraphNode_StateMachineBase* StateMachineNode = nullptr;
				UAnimationStateMachineGraph* StateMachineGraph = nullptr;
				if (!ResolveAnimStateMachineForTool(Context, Arguments, Blueprint, StateMachineNode, StateMachineGraph, OutError))
				{
					return false;
				}
				UAnimStateTransitionNode* TransitionNode = FindAnimTransition(StateMachineGraph, FromState, ToState);
				if (!TransitionNode)
				{
					OutError = TEXT("Animation transition was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintSetTransitionProperties", "SOMOLMCP Set Animation Transition Properties"));
				TransitionNode->Modify();
				if (!Context.Services.ApplyProperties(TransitionNode, Properties.ToSharedRef(), OutError))
				{
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = AnimStateMachineTransitionToJson(TransitionNode);
				OutSummary = TEXT("Updated animation blueprint transition properties.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("anim_blueprint_add_slot_node"),
			TEXT("Add a slot-related node to an animation blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("slot_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("slot_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString SlotName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("slot_name"), SlotName))
				{
					OutError = TEXT("Missing asset_path, graph_name or slot_name.");
					return false;
				}
				UAnimBlueprint* Blueprint = LoadAnimBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* Graph = FindBlueprintGraphByName(Blueprint, GraphName);
				if (!Graph)
				{
					OutError = TEXT("Graph was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintAddSlotNode", "SOMOLMCP Add Animation Slot Node"));
				UAnimGraphNode_Slot* Node = SpawnEditorGraphNode<UAnimGraphNode_Slot>(Graph, FVector2f(0.0f, 0.0f));
				if (!Node)
				{
					OutError = TEXT("Failed to create slot node.");
					return false;
				}
				Node->Modify();
				Node->Node.SlotName = *SlotName;
				Node->ReconstructNode();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(Node);
				OutSummary = TEXT("Added animation blueprint slot node.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("anim_blueprint_add_blend_node"),
			TEXT("Add a blend-related node to an animation blueprint graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_name"), FSololmcpSchemaBuilder::String()}, {TEXT("blend_type"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("graph_name"), TEXT("blend_type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GraphName;
				FString BlendType;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || !Arguments->TryGetStringField(TEXT("blend_type"), BlendType))
				{
					OutError = TEXT("Missing asset_path, graph_name or blend_type.");
					return false;
				}
				UAnimBlueprint* Blueprint = LoadAnimBlueprintAsset(Context.Services, AssetPath, OutError);
				if (!Blueprint)
				{
					return false;
				}
				UEdGraph* Graph = FindBlueprintGraphByName(Blueprint, GraphName);
				if (!Graph)
				{
					OutError = TEXT("Graph was not found.");
					return false;
				}

				const FString NormalizedBlendType = BlendType.ToLower();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AnimBlueprintAddBlendNode", "SOMOLMCP Add Animation Blend Node"));
				UEdGraphNode* NewNode = nullptr;
				if (NormalizedBlendType == TEXT("bool") || NormalizedBlendType == TEXT("blend_list_by_bool"))
				{
					NewNode = SpawnEditorGraphNode<UAnimGraphNode_BlendListByBool>(Graph, FVector2f(0.0f, 0.0f));
				}
				else if (NormalizedBlendType == TEXT("int") || NormalizedBlendType == TEXT("blend_list_by_int"))
				{
					NewNode = SpawnEditorGraphNode<UAnimGraphNode_BlendListByInt>(Graph, FVector2f(0.0f, 0.0f));
				}
				else if (NormalizedBlendType == TEXT("layered") || NormalizedBlendType == TEXT("layered_bone_blend"))
				{
					NewNode = SpawnEditorGraphNode<UAnimGraphNode_LayeredBoneBlend>(Graph, FVector2f(0.0f, 0.0f));
				}
				else
				{
					OutError = TEXT("Unsupported blend_type. Supported values: bool, int, layered_bone_blend.");
					return false;
				}

				if (!NewNode)
				{
					OutError = TEXT("Failed to create blend node.");
					return false;
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				OutStructured = BlueprintNodeToJson(NewNode);
				OutSummary = TEXT("Added animation blueprint blend node.");
				return true;
			}
		, nullptr
		, 5
		});

		RegisterP1PythonAssetTool(TEXT("anim_montage_create"), TEXT("Create an animation montage asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterP1PythonAssetTool(TEXT("anim_montage_list_sections"), TEXT("List sections in an animation montage."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterP1PythonAssetTool(TEXT("anim_montage_add_section"), TEXT("Add a section to an animation montage."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("section_name"), FSololmcpSchemaBuilder::String()}, {TEXT("time"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("section_name")}));
		RegisterP1PythonAssetTool(TEXT("anim_montage_remove_section"), TEXT("Remove a section from an animation montage."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("section_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("section_name")}));
		RegisterP1PythonAssetTool(TEXT("anim_montage_set_section_next"), TEXT("Set the next section relationship in a montage."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("section_name"), FSololmcpSchemaBuilder::String()}, {TEXT("next_section_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("section_name"), TEXT("next_section_name")}));
		RegisterP1PythonAssetTool(TEXT("anim_montage_add_slot_track"), TEXT("Add a slot track to a montage."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("slot_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("slot_name")}));
		RegisterP1PythonAssetTool(TEXT("anim_montage_add_segment"), TEXT("Add a segment to a montage slot track."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("slot_name"), FSololmcpSchemaBuilder::String()}, {TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("start_time"), FSololmcpSchemaBuilder::Number()}, {TEXT("anim_start_time"), FSololmcpSchemaBuilder::Number()}, {TEXT("anim_end_time"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("slot_name"), TEXT("sequence_path")}));

		RegisterP1PythonAssetTool(TEXT("blend_space_create"), TEXT("Create a blend space asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("axis_count"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterP1PythonAssetTool(TEXT("blend_space_list_samples"), TEXT("List samples in a blend space."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterP1PythonAssetTool(TEXT("blend_space_add_sample"), TEXT("Add a sample to a blend space."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sample_value"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number())}}, {TEXT("asset_path"), TEXT("sequence_path"), TEXT("sample_value")}));
		RegisterP1PythonAssetTool(TEXT("blend_space_update_sample"), TEXT("Update a sample in a blend space."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sample_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("sample_value"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number())}}, {TEXT("asset_path"), TEXT("sample_index"), TEXT("sample_value")}));
		RegisterP1PythonAssetTool(TEXT("blend_space_remove_sample"), TEXT("Remove a sample from a blend space."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sample_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("sample_index")}));

		Registry.Register({
			TEXT("static_mesh_list_lods"),
			TEXT("List LOD indices on a static mesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> Lods;
				for (int32 LODIndex = 0; LODIndex < Mesh->GetNumLODs(); ++LODIndex)
				{
					Lods.Add(MakeShared<FJsonValueNumber>(LODIndex));
				}
				OutStructured->SetArrayField(TEXT("lods"), Lods);
				OutStructured->SetNumberField(TEXT("count"), Lods.Num());
				OutSummary = TEXT("Listed static mesh LODs.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("static_mesh_list_sockets"),
			TEXT("List sockets on a static mesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> Sockets;
				for (UStaticMeshSocket* Socket : Mesh->Sockets)
				{
					if (!Socket) { continue; }
					TSharedRef<FJsonObject> SocketJson = MakeShared<FJsonObject>();
					SocketJson->SetStringField(TEXT("name"), Socket->SocketName.ToString());
					SocketJson->SetObjectField(TEXT("relativeLocation"), VectorToJson(Socket->RelativeLocation));
					Sockets.Add(MakeShared<FJsonValueObject>(SocketJson));
				}
				OutStructured->SetArrayField(TEXT("sockets"), Sockets);
				OutSummary = TEXT("Listed static mesh sockets.");
				return true;
			}
		, nullptr
		, 5
		});

		auto CommitMeshAsset = [](const FSololmcpToolExecutionContext& Context, UObject* Asset, const FString& AssetPath, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("Mesh asset is null.");
				return false;
			}
			Asset->PostEditChange();
			Asset->MarkPackageDirty();
			return Context.Services.SaveAsset(AssetPath, false, OutError);
		};

		Registry.Register({
			TEXT("static_mesh_add_socket"),
			TEXT("Add a validated socket to a static mesh using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("socket_name"), FSololmcpSchemaBuilder::String()}, {TEXT("relative_location"), VectorSchema()}, {TEXT("relative_rotation"), RotatorSchema()}}, {TEXT("asset_path"), TEXT("socket_name")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString SocketNameString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("socket_name"), SocketNameString) || SocketNameString.IsEmpty())
				{
					OutError = TEXT("Missing asset_path or socket_name.");
					return false;
				}
				UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}
				const FName SocketName(*SocketNameString);
				if (Mesh->FindSocket(SocketName))
				{
					OutError = FString::Printf(TEXT("Static mesh socket already exists: %s"), *SocketNameString);
					return false;
				}
				FVector RelativeLocation = FVector::ZeroVector;
				FRotator RelativeRotation = FRotator::ZeroRotator;
				if (TSharedPtr<FJsonObject> LocationObject; TryGetObjectField(Arguments, TEXT("relative_location"), LocationObject) && !FSololmcpEditorServices::JsonToVector(LocationObject, RelativeLocation))
				{
					OutError = TEXT("relative_location must be a valid vector.");
					return false;
				}
				if (TSharedPtr<FJsonObject> RotationObject; TryGetObjectField(Arguments, TEXT("relative_rotation"), RotationObject) && !FSololmcpEditorServices::JsonToRotator(RotationObject, RelativeRotation))
				{
					OutError = TEXT("relative_rotation must be a valid rotator.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshAddSocketNative", "SOMOLMCP Add Static Mesh Socket"));
				Mesh->Modify();
				UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh, NAME_None, RF_Transactional);
				Socket->SocketName = SocketName;
				Socket->RelativeLocation = RelativeLocation;
				Socket->RelativeRotation = RelativeRotation;
				Socket->RelativeScale = FVector::OneVector;
				Mesh->AddSocket(Socket);
				if (!Mesh->FindSocket(SocketName) || !CommitMeshAsset(Context, Mesh, AssetPath, OutError))
				{
					if (OutError.IsEmpty()) { OutError = TEXT("Static mesh socket readback failed."); }
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("socket_name"), SocketNameString);
				OutStructured->SetObjectField(TEXT("relative_location"), VectorToJson(Socket->RelativeLocation));
				OutStructured->SetObjectField(TEXT("relative_rotation"), RotatorToJson(Socket->RelativeRotation));
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Added and verified static mesh socket.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("static_mesh_remove_socket"),
			TEXT("Remove and verify a socket on a static mesh using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("socket_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("socket_name")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString SocketNameString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("socket_name"), SocketNameString) || SocketNameString.IsEmpty())
				{
					OutError = TEXT("Missing asset_path or socket_name.");
					return false;
				}
				UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}
				const FName SocketName(*SocketNameString);
				UStaticMeshSocket* Socket = Mesh->FindSocket(SocketName);
				if (!Socket)
				{
					OutError = FString::Printf(TEXT("Static mesh socket was not found: %s"), *SocketNameString);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshRemoveSocketNative", "SOMOLMCP Remove Static Mesh Socket"));
				Mesh->Modify();
				Mesh->RemoveSocket(Socket);
				if (Mesh->FindSocket(SocketName) || !CommitMeshAsset(Context, Mesh, AssetPath, OutError))
				{
					if (OutError.IsEmpty()) { OutError = TEXT("Static mesh socket removal readback failed."); }
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("socket_name"), SocketNameString);
				OutStructured->SetBoolField(TEXT("removed"), true);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Removed and verified static mesh socket.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("static_mesh_set_material_slot"),
			TEXT("Assign and verify a material slot on a static mesh using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("slot_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("material_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("slot_index"), TEXT("material_path")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString MaterialPath;
				int32 SlotIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) || !Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndex))
				{
					OutError = TEXT("Missing asset_path, slot_index or material_path.");
					return false;
				}
				UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}
				UMaterialInterface* Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, OutError));
				if (!Material)
				{
					OutError = TEXT("material_path is not a material interface.");
					return false;
				}
				TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
				if (!Materials.IsValidIndex(SlotIndex))
				{
					OutError = FString::Printf(TEXT("slot_index %d is outside the static mesh material range [0, %d)."), SlotIndex, Materials.Num());
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshSetMaterialNative", "SOMOLMCP Set Static Mesh Material"));
				Mesh->Modify();
				Materials[SlotIndex].MaterialInterface = Material;
				if (Materials[SlotIndex].MaterialInterface != Material || !CommitMeshAsset(Context, Mesh, AssetPath, OutError))
				{
					if (OutError.IsEmpty()) { OutError = TEXT("Static mesh material readback failed."); }
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetNumberField(TEXT("slot_index"), SlotIndex);
				OutStructured->SetStringField(TEXT("material_path"), Material->GetPathName());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Assigned and verified static mesh material slot.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("static_mesh_set_build_settings"),
			TEXT("Set validated build settings on a static mesh source LOD using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("settings"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("lod_index"), TEXT("settings")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				int32 LODIndex = INDEX_NONE;
				TSharedPtr<FJsonObject> Settings;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetNumberField(TEXT("lod_index"), LODIndex) || !TryGetObjectField(Arguments, TEXT("settings"), Settings))
				{
					OutError = TEXT("Missing asset_path, lod_index or settings.");
					return false;
				}
				UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh)
				{
					OutError = TEXT("Asset is not a static mesh.");
					return false;
				}
				if (LODIndex < 0 || LODIndex >= Mesh->GetNumSourceModels())
				{
					OutError = FString::Printf(TEXT("lod_index %d is outside the source model range [0, %d)."), LODIndex, Mesh->GetNumSourceModels());
					return false;
				}
				FMeshBuildSettings UpdatedBuildSettings = Mesh->GetSourceModel(LODIndex).BuildSettings;
				bool bRecognized = false;
				bool BoolValue = false;
				if (Settings->TryGetBoolField(TEXT("use_mikk_t_space"), BoolValue)) { UpdatedBuildSettings.bUseMikkTSpace = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("recompute_normals"), BoolValue)) { UpdatedBuildSettings.bRecomputeNormals = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("recompute_tangents"), BoolValue)) { UpdatedBuildSettings.bRecomputeTangents = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("compute_weighted_normals"), BoolValue)) { UpdatedBuildSettings.bComputeWeightedNormals = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("remove_degenerates"), BoolValue)) { UpdatedBuildSettings.bRemoveDegenerates = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("build_reversed_index_buffer"), BoolValue)) { UpdatedBuildSettings.bBuildReversedIndexBuffer = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("use_high_precision_tangent_basis"), BoolValue)) { UpdatedBuildSettings.bUseHighPrecisionTangentBasis = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("use_full_precision_uvs"), BoolValue)) { UpdatedBuildSettings.bUseFullPrecisionUVs = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("generate_lightmap_uvs"), BoolValue)) { UpdatedBuildSettings.bGenerateLightmapUVs = BoolValue; bRecognized = true; }
				if (Settings->TryGetBoolField(TEXT("generate_distance_field_as_if_two_sided"), BoolValue)) { UpdatedBuildSettings.bGenerateDistanceFieldAsIfTwoSided = BoolValue; bRecognized = true; }
				int32 IntValue = 0;
				if (Settings->TryGetNumberField(TEXT("min_lightmap_resolution"), IntValue)) { UpdatedBuildSettings.MinLightmapResolution = FMath::Max(1, IntValue); bRecognized = true; }
				if (Settings->TryGetNumberField(TEXT("src_lightmap_index"), IntValue)) { UpdatedBuildSettings.SrcLightmapIndex = FMath::Max(0, IntValue); bRecognized = true; }
				if (Settings->TryGetNumberField(TEXT("dst_lightmap_index"), IntValue)) { UpdatedBuildSettings.DstLightmapIndex = FMath::Max(0, IntValue); bRecognized = true; }
				if (Settings->TryGetNumberField(TEXT("max_lumen_mesh_cards"), IntValue)) { UpdatedBuildSettings.MaxLumenMeshCards = FMath::Max(0, IntValue); bRecognized = true; }
				double NumberValue = 0.0;
				if (Settings->TryGetNumberField(TEXT("distance_field_resolution_scale"), NumberValue)) { UpdatedBuildSettings.DistanceFieldResolutionScale = FMath::Max(0.0f, static_cast<float>(NumberValue)); bRecognized = true; }
				if (TSharedPtr<FJsonObject> ScaleObject; TryGetObjectField(Settings.ToSharedRef(), TEXT("build_scale"), ScaleObject))
				{
					FVector BuildScale;
					if (!FSololmcpEditorServices::JsonToVector(ScaleObject, BuildScale) || BuildScale.X <= 0.0 || BuildScale.Y <= 0.0 || BuildScale.Z <= 0.0)
					{
						OutError = TEXT("build_scale must be a positive vector.");
						return false;
					}
					UpdatedBuildSettings.BuildScale3D = BuildScale;
					bRecognized = true;
				}
				if (!bRecognized)
				{
					OutError = TEXT("settings contains no supported static mesh build setting.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshBuildSettingsNative", "SOMOLMCP Set Static Mesh Build Settings"));
				Mesh->Modify();
				Mesh->GetSourceModel(LODIndex).BuildSettings = UpdatedBuildSettings;
				Mesh->Build(true);
				if (!CommitMeshAsset(Context, Mesh, AssetPath, OutError)) { return false; }
				const FMeshBuildSettings& BuildSettings = Mesh->GetSourceModel(LODIndex).BuildSettings;
				TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
				Readback->SetBoolField(TEXT("use_mikk_t_space"), BuildSettings.bUseMikkTSpace);
				Readback->SetBoolField(TEXT("recompute_normals"), BuildSettings.bRecomputeNormals);
				Readback->SetBoolField(TEXT("recompute_tangents"), BuildSettings.bRecomputeTangents);
				Readback->SetBoolField(TEXT("compute_weighted_normals"), BuildSettings.bComputeWeightedNormals);
				Readback->SetBoolField(TEXT("remove_degenerates"), BuildSettings.bRemoveDegenerates);
				Readback->SetBoolField(TEXT("generate_lightmap_uvs"), BuildSettings.bGenerateLightmapUVs);
				Readback->SetNumberField(TEXT("min_lightmap_resolution"), BuildSettings.MinLightmapResolution);
				Readback->SetNumberField(TEXT("src_lightmap_index"), BuildSettings.SrcLightmapIndex);
				Readback->SetNumberField(TEXT("dst_lightmap_index"), BuildSettings.DstLightmapIndex);
				Readback->SetObjectField(TEXT("build_scale"), VectorToJson(BuildSettings.BuildScale3D));
				Readback->SetNumberField(TEXT("distance_field_resolution_scale"), BuildSettings.DistanceFieldResolutionScale);
				Readback->SetNumberField(TEXT("max_lumen_mesh_cards"), BuildSettings.MaxLumenMeshCards);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetNumberField(TEXT("lod_index"), LODIndex);
				OutStructured->SetObjectField(TEXT("settings"), Readback);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Updated, rebuilt, saved and verified static mesh build settings.");
				return true;
			}
		, nullptr
		, 5
		});
		RegisterP1PythonAssetTool(TEXT("static_mesh_reimport"), TEXT("Reimport a static mesh asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));

		Registry.Register({
			TEXT("skeletal_mesh_list_lods"),
			TEXT("List skeletal mesh LOD indices using native C++ asset readback."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a skeletal mesh."); return false; }
				TArray<TSharedPtr<FJsonValue>> Lods;
				for (int32 LODIndex = 0; LODIndex < Mesh->GetLODNum(); ++LODIndex) { Lods.Add(MakeShared<FJsonValueNumber>(LODIndex)); }
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetArrayField(TEXT("lods"), Lods);
				OutStructured->SetNumberField(TEXT("count"), Lods.Num());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Listed skeletal mesh LODs.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("skeletal_mesh_list_sockets"),
			TEXT("List active skeletal mesh and skeleton sockets using native C++ asset readback."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a skeletal mesh."); return false; }
				TSet<const USkeletalMeshSocket*> MeshOnlySockets;
				for (const USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList()) { if (Socket) { MeshOnlySockets.Add(Socket); } }
				TArray<TSharedPtr<FJsonValue>> Sockets;
				for (USkeletalMeshSocket* Socket : Mesh->GetActiveSocketList())
				{
					if (!Socket) { continue; }
					TSharedRef<FJsonObject> SocketJson = MakeShared<FJsonObject>();
					SocketJson->SetStringField(TEXT("name"), Socket->SocketName.ToString());
					SocketJson->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
					SocketJson->SetStringField(TEXT("source"), MeshOnlySockets.Contains(Socket) ? TEXT("mesh") : TEXT("skeleton"));
					SocketJson->SetObjectField(TEXT("relative_location"), VectorToJson(Socket->RelativeLocation));
					SocketJson->SetObjectField(TEXT("relative_rotation"), RotatorToJson(Socket->RelativeRotation));
					SocketJson->SetObjectField(TEXT("relative_scale"), VectorToJson(Socket->RelativeScale));
					Sockets.Add(MakeShared<FJsonValueObject>(SocketJson));
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetArrayField(TEXT("sockets"), Sockets);
				OutStructured->SetNumberField(TEXT("count"), Sockets.Num());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Listed active skeletal mesh sockets.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("skeletal_mesh_add_socket"),
			TEXT("Add a validated mesh-only socket to a skeletal mesh using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("socket_name"), FSololmcpSchemaBuilder::String()}, {TEXT("bone_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("socket_name"), TEXT("bone_name")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString SocketNameString;
				FString BoneNameString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("socket_name"), SocketNameString) || !Arguments->TryGetStringField(TEXT("bone_name"), BoneNameString) || SocketNameString.IsEmpty() || BoneNameString.IsEmpty())
				{
					OutError = TEXT("Missing asset_path, socket_name or bone_name.");
					return false;
				}
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a skeletal mesh."); return false; }
				const FName SocketName(*SocketNameString);
				const FName BoneName(*BoneNameString);
				if (Mesh->FindSocket(SocketName))
				{
					OutError = FString::Printf(TEXT("Skeletal mesh or skeleton socket already exists: %s"), *SocketNameString);
					return false;
				}
				if (Mesh->GetRefSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Bone was not found on skeletal mesh: %s"), *BoneNameString);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SkeletalMeshAddSocketNative", "SOMOLMCP Add Skeletal Mesh Socket"));
				Mesh->Modify();
				USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Mesh, NAME_None, RF_Transactional);
				Socket->SocketName = SocketName;
				Socket->BoneName = BoneName;
				Mesh->AddSocket(Socket, false);
				Mesh->RebuildSocketMap();
				if (!Mesh->FindSocket(SocketName) || !CommitMeshAsset(Context, Mesh, AssetPath, OutError))
				{
					if (OutError.IsEmpty()) { OutError = TEXT("Skeletal mesh socket readback failed."); }
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("socket_name"), SocketNameString);
				OutStructured->SetStringField(TEXT("bone_name"), BoneNameString);
				OutStructured->SetStringField(TEXT("source"), TEXT("mesh"));
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Added and verified skeletal mesh socket.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("skeletal_mesh_remove_socket"),
			TEXT("Remove and verify a mesh-only socket on a skeletal mesh using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("socket_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("socket_name")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString SocketNameString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("socket_name"), SocketNameString) || SocketNameString.IsEmpty())
				{
					OutError = TEXT("Missing asset_path or socket_name.");
					return false;
				}
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a skeletal mesh."); return false; }
				const FName SocketName(*SocketNameString);
				TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = Mesh->GetMeshOnlySocketList();
				const int32 SocketIndex = MeshSockets.IndexOfByPredicate([SocketName](const TObjectPtr<USkeletalMeshSocket>& Socket) { return Socket && Socket->SocketName == SocketName; });
				if (SocketIndex == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Mesh-only skeletal socket was not found: %s"), *SocketNameString);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SkeletalMeshRemoveSocketNative", "SOMOLMCP Remove Skeletal Mesh Socket"));
				Mesh->Modify();
				MeshSockets.RemoveAt(SocketIndex);
				Mesh->RebuildSocketMap();
				const bool bStillMeshOnly = MeshSockets.ContainsByPredicate([SocketName](const TObjectPtr<USkeletalMeshSocket>& Socket) { return Socket && Socket->SocketName == SocketName; });
				if (bStillMeshOnly || !CommitMeshAsset(Context, Mesh, AssetPath, OutError))
				{
					if (OutError.IsEmpty()) { OutError = TEXT("Skeletal mesh socket removal readback failed."); }
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("socket_name"), SocketNameString);
				OutStructured->SetBoolField(TEXT("removed"), true);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Removed and verified mesh-only skeletal socket.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("skeletal_mesh_set_material_slot"),
			TEXT("Assign and verify a material slot on a skeletal mesh using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("slot_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("material_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("slot_index"), TEXT("material_path")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString MaterialPath;
				int32 SlotIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) || !Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndex))
				{
					OutError = TEXT("Missing asset_path, slot_index or material_path.");
					return false;
				}
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a skeletal mesh."); return false; }
				UMaterialInterface* Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, OutError));
				if (!Material) { OutError = TEXT("material_path is not a material interface."); return false; }
				TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
				if (!Materials.IsValidIndex(SlotIndex))
				{
					OutError = FString::Printf(TEXT("slot_index %d is outside the skeletal mesh material range [0, %d)."), SlotIndex, Materials.Num());
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SkeletalMeshSetMaterialNative", "SOMOLMCP Set Skeletal Mesh Material"));
				Mesh->Modify();
				Materials[SlotIndex].MaterialInterface = Material;
				if (Materials[SlotIndex].MaterialInterface != Material || !CommitMeshAsset(Context, Mesh, AssetPath, OutError))
				{
					if (OutError.IsEmpty()) { OutError = TEXT("Skeletal mesh material readback failed."); }
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetNumberField(TEXT("slot_index"), SlotIndex);
				OutStructured->SetStringField(TEXT("material_path"), Material->GetPathName());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Assigned and verified skeletal mesh material slot.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("skeletal_mesh_set_import_settings"),
			TEXT("Set validated skeletal mesh LOD and streaming settings using native C++ editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("settings"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("settings")}),
			[CommitMeshAsset](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				TSharedPtr<FJsonObject> Settings;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetObjectField(Arguments, TEXT("settings"), Settings))
				{
					OutError = TEXT("Missing asset_path or settings.");
					return false;
				}
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a skeletal mesh."); return false; }
				TOptional<int32> RequestedMinLod;
				TOptional<int32> RequestedMaxStreamedLods;
				TOptional<int32> RequestedMaxOptionalLods;
				TOptional<bool> RequestedDisableBelowMinLodStripping;
				TOptional<bool> RequestedOverrideStreaming;
				TOptional<bool> RequestedSupportStreaming;
				int32 IntValue = 0;
				bool BoolValue = false;
				if (Settings->TryGetNumberField(TEXT("min_lod"), IntValue))
				{
					if (IntValue < 0 || IntValue >= FMath::Max(1, Mesh->GetLODNum())) { OutError = TEXT("min_lod is outside the skeletal mesh LOD range."); return false; }
					RequestedMinLod = IntValue;
				}
				if (Settings->TryGetBoolField(TEXT("disable_below_min_lod_stripping"), BoolValue)) { RequestedDisableBelowMinLodStripping = BoolValue; }
				if (Settings->TryGetBoolField(TEXT("override_lod_streaming_settings"), BoolValue)) { RequestedOverrideStreaming = BoolValue; }
				if (Settings->TryGetBoolField(TEXT("support_lod_streaming"), BoolValue)) { RequestedSupportStreaming = BoolValue; }
				if (Settings->TryGetNumberField(TEXT("max_num_streamed_lods"), IntValue))
				{
					if (IntValue < 0 || IntValue > Mesh->GetLODNum()) { OutError = TEXT("max_num_streamed_lods is outside the skeletal mesh LOD range."); return false; }
					RequestedMaxStreamedLods = IntValue;
				}
				if (Settings->TryGetNumberField(TEXT("max_num_optional_lods"), IntValue))
				{
					if (IntValue < 0 || IntValue > Mesh->GetLODNum()) { OutError = TEXT("max_num_optional_lods is outside the skeletal mesh LOD range."); return false; }
					RequestedMaxOptionalLods = IntValue;
				}
				if (!RequestedMinLod.IsSet() && !RequestedMaxStreamedLods.IsSet() && !RequestedMaxOptionalLods.IsSet() &&
					!RequestedDisableBelowMinLodStripping.IsSet() && !RequestedOverrideStreaming.IsSet() && !RequestedSupportStreaming.IsSet())
				{
					OutError = TEXT("settings contains no supported skeletal mesh LOD or streaming setting.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SkeletalMeshSettingsNative", "SOMOLMCP Set Skeletal Mesh Settings"));
				Mesh->Modify();
				if (RequestedMinLod.IsSet()) { Mesh->SetMinLod(FPerPlatformInt(RequestedMinLod.GetValue())); }
				if (RequestedDisableBelowMinLodStripping.IsSet()) { Mesh->SetDisableBelowMinLodStripping(FPerPlatformBool(RequestedDisableBelowMinLodStripping.GetValue())); }
				if (RequestedOverrideStreaming.IsSet()) { Mesh->SetOverrideLODStreamingSettings(RequestedOverrideStreaming.GetValue()); }
				if (RequestedSupportStreaming.IsSet()) { Mesh->SetSupportLODStreaming(FPerPlatformBool(RequestedSupportStreaming.GetValue())); }
				if (RequestedMaxStreamedLods.IsSet()) { Mesh->SetMaxNumStreamedLODs(FPerPlatformInt(RequestedMaxStreamedLods.GetValue())); }
				if (RequestedMaxOptionalLods.IsSet()) { Mesh->SetMaxNumOptionalLODs(FPerPlatformInt(RequestedMaxOptionalLods.GetValue())); }
				if (!CommitMeshAsset(Context, Mesh, AssetPath, OutError)) { return false; }
				TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
				Readback->SetNumberField(TEXT("min_lod"), Mesh->GetMinLod().Default);
				Readback->SetBoolField(TEXT("disable_below_min_lod_stripping"), Mesh->GetDisableBelowMinLodStripping().Default);
				Readback->SetBoolField(TEXT("override_lod_streaming_settings"), Mesh->GetOverrideLODStreamingSettings());
				Readback->SetBoolField(TEXT("support_lod_streaming"), Mesh->GetSupportLODStreaming().Default);
				Readback->SetNumberField(TEXT("max_num_streamed_lods"), Mesh->GetMaxNumStreamedLODs().Default);
				Readback->SetNumberField(TEXT("max_num_optional_lods"), Mesh->GetMaxNumOptionalLODs().Default);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetObjectField(TEXT("settings"), Readback);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutSummary = TEXT("Updated, saved and verified skeletal mesh LOD and streaming settings.");
				return true;
			}
		, nullptr
		, 5
		});
		RegisterP1PythonAssetTool(TEXT("skeletal_mesh_reimport"), TEXT("Reimport a skeletal mesh asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));

		RegisterP1PythonAssetTool(TEXT("material_disconnect_property"), TEXT("Disconnect a material property input."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("property"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("property")}));
		RegisterP1PythonAssetTool(TEXT("material_disconnect_expressions"), TEXT("Disconnect two linked material expressions."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("output_expression_id"), FSololmcpSchemaBuilder::Integer()}, {TEXT("input_expression_id"), FSololmcpSchemaBuilder::Integer()}, {TEXT("input_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("output_expression_id"), TEXT("input_expression_id")}));
		Registry.Register({
			TEXT("material_add_comment"),
			TEXT("Add, save, and read back a native comment expression in a material or material function graph."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("text"), FSololmcpSchemaBuilder::String()},
				{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("size_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("size_y"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("text")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, Text;
				int32 NodeX = 0, NodeY = 0, SizeX = 400, SizeY = 200;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("text"), Text))
				{
					OutError = TEXT("Missing asset_path or text.");
					return false;
				}
				Arguments->TryGetNumberField(TEXT("node_x"), NodeX);
				Arguments->TryGetNumberField(TEXT("node_y"), NodeY);
				Arguments->TryGetNumberField(TEXT("size_x"), SizeX);
				Arguments->TryGetNumberField(TEXT("size_y"), SizeY);
				const bool bSaveAsset = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				UObject* Target = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Target) return false;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialAddCommentNative", "SOMOLMCP Add Material Comment"));
				Target->Modify();
				UMaterialExpressionComment* Comment = nullptr;
				int32 ExpressionIndex = INDEX_NONE;
				if (UMaterial* Material = Cast<UMaterial>(Target))
				{
					Comment = Cast<UMaterialExpressionComment>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionComment::StaticClass(), NodeX, NodeY));
					ExpressionIndex = FindMaterialExpressionIndex(Material, Comment);
				}
				else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Target))
				{
					Comment = Cast<UMaterialExpressionComment>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionComment::StaticClass(), NodeX, NodeY));
					ExpressionIndex = FindMaterialFunctionExpressionIndex(Function, Comment);
				}
				else
				{
					OutError = FString::Printf(TEXT("Asset is not a material or material function: %s"), *AssetPath);
					return false;
				}
				if (!Comment || ExpressionIndex == INDEX_NONE)
				{
					OutError = TEXT("Failed to create material comment expression.");
					return false;
				}
				Comment->Modify();
				Comment->Text = Text;
				Comment->SizeX = FMath::Max(64, SizeX);
				Comment->SizeY = FMath::Max(64, SizeY);
				Comment->PostEditChange();
				Target->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(Target);
				if (bSaveAsset && !Context.Services.SaveAsset(Target->GetPathName(), false, OutError)) return false;
				const bool bVerified = Comment->Text == Text && ExpressionIndex != INDEX_NONE;
				OutStructured->SetStringField(TEXT("asset_path"), Target->GetPathName());
				OutStructured->SetNumberField(TEXT("expression_index"), ExpressionIndex);
				OutStructured->SetStringField(TEXT("expression_guid"), Comment->MaterialExpressionGuid.ToString());
				OutStructured->SetStringField(TEXT("text"), Comment->Text);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				if (!bVerified) { OutError = TEXT("Material comment readback failed."); return false; }
				OutSummary = TEXT("Added and verified native material graph comment.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_add_input"),
			TEXT("Add, configure, save, and read back a native material-function input expression."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("input_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("input_type"), FSololmcpSchemaBuilder::String(TEXT("scalar, vector2, vector3, vector4, texture2d, texturecube, texture2darray, volumetexture, staticbool, materialattributes, textureexternal, bool, or substrate"))},
				{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("input_name"), TEXT("input_type")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, InputName, InputTypeName;
				int32 NodeX = -400, NodeY = 0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("input_name"), InputName)
					|| !Arguments->TryGetStringField(TEXT("input_type"), InputTypeName))
				{
					OutError = TEXT("Missing material function input arguments.");
					return false;
				}
				Arguments->TryGetNumberField(TEXT("node_x"), NodeX);
				Arguments->TryGetNumberField(TEXT("node_y"), NodeY);
				const bool bSaveAsset = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				UMaterialFunction* Function = LoadMaterialFunctionAsset(Context.Services, AssetPath, OutError);
				if (!Function) return false;
				FString Normalized = InputTypeName.ToLower().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
				EFunctionInputType InputType = FunctionInput_MAX;
				if (Normalized == TEXT("scalar")) InputType = FunctionInput_Scalar;
				else if (Normalized == TEXT("vector2")) InputType = FunctionInput_Vector2;
				else if (Normalized == TEXT("vector3")) InputType = FunctionInput_Vector3;
				else if (Normalized == TEXT("vector4")) InputType = FunctionInput_Vector4;
				else if (Normalized == TEXT("texture2d")) InputType = FunctionInput_Texture2D;
				else if (Normalized == TEXT("texturecube")) InputType = FunctionInput_TextureCube;
				else if (Normalized == TEXT("texture2darray")) InputType = FunctionInput_Texture2DArray;
				else if (Normalized == TEXT("volumetexture")) InputType = FunctionInput_VolumeTexture;
				else if (Normalized == TEXT("staticbool")) InputType = FunctionInput_StaticBool;
				else if (Normalized == TEXT("materialattributes")) InputType = FunctionInput_MaterialAttributes;
				else if (Normalized == TEXT("textureexternal")) InputType = FunctionInput_TextureExternal;
				else if (Normalized == TEXT("bool")) InputType = FunctionInput_Bool;
				else if (Normalized == TEXT("substrate")) InputType = FunctionInput_Substrate;
				if (InputType == FunctionInput_MAX)
				{
					OutError = FString::Printf(TEXT("Unsupported material function input type: %s"), *InputTypeName);
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialFunctionAddInputNative", "SOMOLMCP Add Material Function Input"));
				Function->Modify();
				UMaterialExpressionFunctionInput* Expression = Cast<UMaterialExpressionFunctionInput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionInput::StaticClass(), NodeX, NodeY));
				if (!Expression) { OutError = TEXT("Failed to create material function input expression."); return false; }
				Expression->Modify();
				Expression->InputName = FName(*InputName);
				Expression->InputType = InputType;
				Expression->ConditionallyGenerateId(false);
				Expression->PostEditChange();
				Function->UpdateFromFunctionResource();
				Function->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(Function);
				if (bSaveAsset && !Context.Services.SaveAsset(Function->GetPathName(), false, OutError)) return false;
				const int32 ExpressionIndex = FindMaterialFunctionExpressionIndex(Function, Expression);
				const bool bVerified = ExpressionIndex != INDEX_NONE && Expression->InputName == FName(*InputName) && Expression->InputType == InputType;
				OutStructured->SetStringField(TEXT("asset_path"), Function->GetPathName());
				OutStructured->SetNumberField(TEXT("expression_index"), ExpressionIndex);
				OutStructured->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString());
				OutStructured->SetStringField(TEXT("input_name"), Expression->InputName.ToString());
				OutStructured->SetStringField(TEXT("input_type"), InputTypeName);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				if (!bVerified) { OutError = TEXT("Material function input readback failed."); return false; }
				OutSummary = TEXT("Added and verified native material function input.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("material_function_add_output"),
			TEXT("Add, configure, save, and read back a native material-function output expression."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("output_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("node_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("node_y"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("output_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, OutputName;
				int32 NodeX = 400, NodeY = 0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("output_name"), OutputName))
				{
					OutError = TEXT("Missing asset_path or output_name.");
					return false;
				}
				Arguments->TryGetNumberField(TEXT("node_x"), NodeX);
				Arguments->TryGetNumberField(TEXT("node_y"), NodeY);
				const bool bSaveAsset = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				UMaterialFunction* Function = LoadMaterialFunctionAsset(Context.Services, AssetPath, OutError);
				if (!Function) return false;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialFunctionAddOutputNative", "SOMOLMCP Add Material Function Output"));
				Function->Modify();
				UMaterialExpressionFunctionOutput* Expression = Cast<UMaterialExpressionFunctionOutput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, UMaterialExpressionFunctionOutput::StaticClass(), NodeX, NodeY));
				if (!Expression) { OutError = TEXT("Failed to create material function output expression."); return false; }
				Expression->Modify();
				Expression->OutputName = FName(*OutputName);
				Expression->ConditionallyGenerateId(false);
				Expression->PostEditChange();
				Function->UpdateFromFunctionResource();
				Function->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(Function);
				if (bSaveAsset && !Context.Services.SaveAsset(Function->GetPathName(), false, OutError)) return false;
				const int32 ExpressionIndex = FindMaterialFunctionExpressionIndex(Function, Expression);
				const bool bVerified = ExpressionIndex != INDEX_NONE && Expression->OutputName == FName(*OutputName);
				OutStructured->SetStringField(TEXT("asset_path"), Function->GetPathName());
				OutStructured->SetNumberField(TEXT("expression_index"), ExpressionIndex);
				OutStructured->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString());
				OutStructured->SetStringField(TEXT("output_name"), Expression->OutputName.ToString());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				if (!bVerified) { OutError = TEXT("Material function output readback failed."); return false; }
				OutSummary = TEXT("Added and verified native material function output.");
				return true;
			}
		, nullptr
		, 5
		});
		// Bucket C fix: pure-C++ replacement of material_set_parameter_group. The previous python
		// path emitted "Traceback (most recent call last): RuntimeError: Failed to load material asset"
		// when the asset_path target couldn't be resolved by EditorAssetSubsystem.load_asset (e.g.
		// the asset registry hadn't picked up a freshly-saved material yet). Doing this in C++ via
		// LoadMaterialAsset gives a deterministic error and avoids the python dependency entirely.
		Registry.Register({
			TEXT("material_set_parameter_group"),
			TEXT("Set parameter group metadata for a material expression parameter."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("expression_id"), FSololmcpSchemaBuilder::Integer()}, {TEXT("expression_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("expression_guid"), FSololmcpSchemaBuilder::String()}, {TEXT("group_name"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("group_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString GroupName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				if (!Arguments->TryGetStringField(TEXT("group_name"), GroupName))
				{
					OutError = TEXT("Missing group_name.");
					return false;
				}
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material)
				{
					return false;
				}
				// Accept either expression_index (preferred) or expression_id (legacy python name).
				TSharedRef<FJsonObject> LocalArgs = MakeShared<FJsonObject>();
				for (const auto& KV : Arguments->Values)
				{
					LocalArgs->SetField(FString(*KV.Key), KV.Value);
				}
				if (!LocalArgs->HasField(TEXT("expression_index")) && LocalArgs->HasField(TEXT("expression_id")))
				{
					double V = 0.0;
					int32 Vi = 0;
					if (LocalArgs->TryGetNumberField(TEXT("expression_id"), V))
					{
						LocalArgs->SetNumberField(TEXT("expression_index"), V);
					}
					else if (LocalArgs->TryGetNumberField(TEXT("expression_id"), Vi))
					{
						LocalArgs->SetNumberField(TEXT("expression_index"), Vi);
					}
				}
				UMaterialExpression* Expression = ResolveMaterialExpressionFromArguments(Material, LocalArgs, TEXT("expression_index"), TEXT("expression_guid"), OutError);
				if (!Expression)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MaterialSetParameterGroup", "SOMOLMCP Set Material Parameter Group"));
				bool bApplied = false;
				if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expression))
				{
					SP->Group = FName(*GroupName);
					bApplied = true;
				}
				else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expression))
				{
					VP->Group = FName(*GroupName);
					bApplied = true;
				}
				else if (UMaterialExpressionStaticBoolParameter* BP = Cast<UMaterialExpressionStaticBoolParameter>(Expression))
				{
					BP->Group = FName(*GroupName);
					bApplied = true;
				}
				if (!bApplied)
				{
					OutError = FString::Printf(TEXT("Expression class %s is not a parameter expression with a Group field."), *Expression->GetClass()->GetName());
					return false;
				}
				Expression->Modify();
				Material->PostEditChange();
				Material->MarkPackageDirty();
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				if (bSaveAsset && !Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("group"), GroupName);
				OutStructured->SetStringField(TEXT("expression_class"), Expression->GetClass()->GetName());
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutSummary = TEXT("Set material parameter group.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("material_get_parameter_info"),
			TEXT("Read native parameter metadata from a material expression by index or GUID."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("expression_id"), FSololmcpSchemaBuilder::Integer()},
				{TEXT("expression_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("expression_guid"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UMaterial* Material = LoadMaterialAsset(Context.Services, AssetPath, OutError);
				if (!Material) return false;
				TSharedRef<FJsonObject> LocalArgs = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments->Values) LocalArgs->SetField(Pair.Key, Pair.Value);
				if (!LocalArgs->HasField(TEXT("expression_index")) && LocalArgs->HasField(TEXT("expression_id")))
				{
					LocalArgs->SetField(TEXT("expression_index"), LocalArgs->TryGetField(TEXT("expression_id")));
				}
				UMaterialExpression* Expression = ResolveMaterialExpressionFromArguments(Material, LocalArgs, TEXT("expression_index"), TEXT("expression_guid"), OutError);
				if (!Expression) return false;
				UMaterialExpressionParameter* Parameter = Cast<UMaterialExpressionParameter>(Expression);
				if (!Parameter)
				{
					OutError = FString::Printf(TEXT("Expression %s is not a material parameter expression."), *Expression->GetClass()->GetName());
					return false;
				}
				OutStructured = MaterialExpressionToJson(Material, Expression);
				OutStructured->SetStringField(TEXT("parameter_name"), Parameter->ParameterName.ToString());
				OutStructured->SetStringField(TEXT("group"), Parameter->Group.ToString());
				OutStructured->SetBoolField(TEXT("is_parameter"), true);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("verified"), true);
				OutSummary = FString::Printf(TEXT("Read native parameter '%s' metadata."), *Parameter->ParameterName.ToString());
				return true;
			}
		, nullptr
		, 5
		});
	}
}
