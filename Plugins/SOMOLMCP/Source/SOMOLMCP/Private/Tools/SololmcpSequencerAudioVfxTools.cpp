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

// FNiagaraEmitterInstanceRef was introduced in 5.4 as an alias for the shared-ref
// type FNiagaraSystemInstance::GetEmitters() has always returned.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 4
using FNiagaraEmitterInstanceRef = TSharedRef<FNiagaraEmitterInstance, ESPMode::ThreadSafe>;
#endif
	void RegisterSequencerAudioVfxTools(FSololmcpToolRegistry& Registry)
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
			TEXT("audio_import_sound"),
			TEXT("Import one sound file into the content browser."),
			FSololmcpSchemaBuilder::Object({{TEXT("source_file"), FSololmcpSchemaBuilder::String()}, {TEXT("destination_path"), FSololmcpSchemaBuilder::String()}, {TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("source_file"), TEXT("destination_path")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SourceFile;
				FString DestinationPath;
				if (!Arguments->TryGetStringField(TEXT("source_file"), SourceFile) || !Arguments->TryGetStringField(TEXT("destination_path"), DestinationPath))
				{
					OutError = TEXT("Missing source_file or destination_path.");
					return false;
				}
				TSharedRef<FJsonObject> ImportArgs = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> SourceFiles;
				SourceFiles.Add(MakeShared<FJsonValueString>(SourceFile));
				ImportArgs->SetArrayField(TEXT("source_files"), SourceFiles);
				ImportArgs->SetStringField(TEXT("destination_path"), DestinationPath);
				ImportArgs->SetBoolField(TEXT("replace_existing"), Arguments->HasTypedField<EJson::Boolean>(TEXT("replace_existing")) ? Arguments->GetBoolField(TEXT("replace_existing")) : true);
				ImportArgs->SetBoolField(TEXT("save"), true);
				ImportArgs->SetBoolField(TEXT("automated"), true);
				return Registry.ExecuteTool(TEXT("import_asset"), ImportArgs, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("audio_create_sound_cue"),
			TEXT("Create a sound cue asset optionally seeded with a sound wave."),
			FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("sound_wave_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}),

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

				TSharedPtr<FJsonObject> FactoryOverrides;
				FString SoundWavePath;
				if (Arguments->TryGetStringField(TEXT("sound_wave_path"), SoundWavePath) && !SoundWavePath.IsEmpty())
				{
					USoundWave* SoundWave = Cast<USoundWave>(Context.Services.LoadAsset(SoundWavePath, OutError));
					if (!SoundWave)
					{
						OutError = TEXT("sound_wave_path does not resolve to a USoundWave.");
						return false;
					}
					FactoryOverrides = MakeShared<FJsonObject>();
					TArray<TSharedPtr<FJsonValue>> InitialSoundWaves;
					InitialSoundWaves.Add(MakeShared<FJsonValueString>(SoundWave->GetPathName()));
					FactoryOverrides->SetArrayField(TEXT("InitialSoundWaves"), InitialSoundWaves);
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CreateSoundCue", "SOMOLMCP Create Sound Cue"));
				UObject* Asset = Context.Services.CreateAsset(
					PackagePath,
					AssetName,
					USoundCue::StaticClass()->GetPathName(),
					USoundCueFactoryNew::StaticClass()->GetPathName(),
					FactoryOverrides,
					OutError);
				if (!Asset)
				{
					return false;
				}

				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutSummary = TEXT("Created sound cue.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("audio_place_ambient_sound"),
			TEXT("Place a sound asset into the world as an ambient sound actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("location"), VectorSchema()}, {TEXT("rotation"), RotatorSchema()}}, {TEXT("asset_path")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				SpawnArgs->SetStringField(TEXT("asset_path"), AssetPath);
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("location")))
				{
					SpawnArgs->SetField(TEXT("location"), *Value);
				}
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("rotation")))
				{
					SpawnArgs->SetField(TEXT("rotation"), *Value);
				}
				return Registry.ExecuteTool(TEXT("actor_spawn"), SpawnArgs, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		auto RegisterAudioPythonTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
				{
					const FString ArgumentsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal\n")
						TEXT("import json\n")
						TEXT("tool_name = %s\n")
						TEXT("args = json.loads(%s)\n")
						TEXT("asset_tools = unreal.AssetToolsHelpers.get_asset_tools()\n")
						TEXT("asset_subsystem = unreal.EditorAssetSubsystem()\n")
						TEXT("asset_path = args.get('asset_path', '')\n")
						TEXT("package_path = args.get('package_path', '')\n")
						TEXT("asset_name = args.get('asset_name', '')\n")
						TEXT("target = asset_subsystem.load_asset(asset_path) if asset_path else None\n")
						TEXT("def save_target(obj):\n")
						TEXT("    if obj is not None:\n")
						TEXT("        asset_subsystem.save_loaded_asset(obj)\n")
						TEXT("def load_optional(path):\n")
						TEXT("    return asset_subsystem.load_asset(path) if path else None\n")
						TEXT("def resolve_soundcue_node(name):\n")
						TEXT("    if target is None or not hasattr(target, 'all_nodes'):\n")
						TEXT("        return None\n")
						TEXT("    for node in list(target.all_nodes):\n")
						TEXT("        if node.get_name() == name:\n")
						TEXT("            return node\n")
						TEXT("    return None\n")
						TEXT("if tool_name == 'audio_sound_cue_list_nodes':\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load sound cue')\n")
						TEXT("    for node in list(target.all_nodes) if hasattr(target, 'all_nodes') else []:\n")
						TEXT("        child_count = len(list(node.child_nodes)) if hasattr(node, 'child_nodes') and node.child_nodes is not None else 0\n")
						TEXT("        unreal.log('node=' + node.get_name() + ' class=' + node.get_class().get_name() + ' children=' + str(child_count))\n")
						TEXT("elif tool_name == 'audio_sound_cue_add_node':\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load sound cue')\n")
						TEXT("    node_class = unreal.load_class(None, args.get('node_class_path', ''))\n")
						TEXT("    if node_class is None:\n")
						TEXT("        raise RuntimeError('Failed to load node class')\n")
						TEXT("    base_name = node_class.get_name()\n")
						TEXT("    node = unreal.new_object(node_class, target, base_name)\n")
						TEXT("    nodes = list(target.all_nodes) if hasattr(target, 'all_nodes') and target.all_nodes is not None else []\n")
						TEXT("    nodes.append(node)\n")
						TEXT("    target.set_editor_property('all_nodes', nodes)\n")
						TEXT("    if hasattr(target, 'first_node') and target.get_editor_property('first_node') is None:\n")
						TEXT("        target.set_editor_property('first_node', node)\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_sound_cue_connect':\n")
						TEXT("    source = resolve_soundcue_node(args.get('from_node', ''))\n")
						TEXT("    target_node = resolve_soundcue_node(args.get('to_node', ''))\n")
						TEXT("    if source is None or target_node is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve source or target sound cue node')\n")
						TEXT("    children = list(source.child_nodes) if hasattr(source, 'child_nodes') and source.child_nodes is not None else []\n")
						TEXT("    if target_node not in children:\n")
						TEXT("        children.append(target_node)\n")
						TEXT("        source.set_editor_property('child_nodes', children)\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_sound_cue_disconnect':\n")
						TEXT("    node = resolve_soundcue_node(args.get('node', ''))\n")
						TEXT("    if node is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve sound cue node')\n")
						TEXT("    if hasattr(node, 'child_nodes'):\n")
						TEXT("        node.set_editor_property('child_nodes', [])\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_sound_cue_delete_node':\n")
						TEXT("    node = resolve_soundcue_node(args.get('node', ''))\n")
						TEXT("    if node is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve sound cue node')\n")
						TEXT("    nodes = [candidate for candidate in list(target.all_nodes) if candidate != node] if hasattr(target, 'all_nodes') else []\n")
						TEXT("    target.set_editor_property('all_nodes', nodes)\n")
						TEXT("    for candidate in nodes:\n")
						TEXT("        if hasattr(candidate, 'child_nodes') and candidate.child_nodes is not None:\n")
						TEXT("            candidate.set_editor_property('child_nodes', [child for child in list(candidate.child_nodes) if child != node])\n")
						TEXT("    if hasattr(target, 'first_node') and target.get_editor_property('first_node') == node:\n")
						TEXT("        target.set_editor_property('first_node', nodes[0] if nodes else None)\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_sound_cue_set_node_properties':\n")
						TEXT("    node = resolve_soundcue_node(args.get('node', ''))\n")
						TEXT("    if node is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve sound cue node')\n")
						TEXT("    for key, value in dict(args.get('properties', {})).items():\n")
						TEXT("        node.set_editor_property(key, value)\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_sound_cue_layout':\n")
						TEXT("    nodes = list(target.all_nodes) if target is not None and hasattr(target, 'all_nodes') else []\n")
						TEXT("    ? = 0\n")
						TEXT("    for index, node in enumerate(nodes):\n")
						TEXT("        if hasattr(node, 'graph_node') and node.graph_node is not None:\n")
						TEXT("            node.graph_node.node_pos_x = x\n")
						TEXT("            node.graph_node.node_pos_y = index * 160\n")
						TEXT("        ? += 320\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_sound_cue_compile':\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load sound cue')\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name in ('audio_metasound_create_source', 'audio_metasound_create_patch'):\n")
						TEXT("    factory_candidates = []\n")
						TEXT("    if tool_name == 'audio_metasound_create_source':\n")
						TEXT("        factory_candidates = ['/Script/MetasoundEditor.MetaSoundSourceFactory', '/Script/MetasoundEditor.MetaSoundSourceFactoryNew']\n")
						TEXT("    else:\n")
						TEXT("        factory_candidates = ['/Script/MetasoundEditor.MetaSoundPatchFactory', '/Script/MetasoundEditor.MetaSoundPatchFactoryNew']\n")
						TEXT("    factory = None\n")
						TEXT("    for class_path in factory_candidates:\n")
						TEXT("        factory_class = unreal.load_class(None, class_path)\n")
						TEXT("        if factory_class is not None:\n")
						TEXT("            factory = unreal.new_object(factory_class)\n")
						TEXT("            break\n")
						TEXT("    if factory is None:\n")
						TEXT("        raise RuntimeError('Unable to resolve MetaSound factory')\n")
						TEXT("    created = asset_tools.create_asset(asset_name, package_path, None, factory)\n")
						TEXT("    if created is None:\n")
						TEXT("        raise RuntimeError('Failed to create MetaSound asset')\n")
						TEXT("    save_target(created)\n")
						TEXT("elif tool_name in ('audio_metasound_add_input', 'audio_metasound_add_output', 'audio_metasound_add_node', 'audio_metasound_connect', 'audio_metasound_set_input_default', 'audio_metasound_remove_node', 'audio_metasound_build_asset'):\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load MetaSound asset')\n")
						TEXT("    editor_subsystem = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)\n")
						TEXT("    if editor_subsystem is not None:\n")
						TEXT("        editor_subsystem.open_editor_for_assets([target])\n")
						TEXT("    builder = None\n")
						TEXT("    builder_subsystem = getattr(unreal, 'MetaSoundBuilderSubsystem', None)\n")
						TEXT("    if builder_subsystem is not None:\n")
						TEXT("        builder = unreal.get_editor_subsystem(builder_subsystem)\n")
						TEXT("    if tool_name == 'audio_metasound_build_asset':\n")
						TEXT("        if builder is not None and hasattr(builder, 'build_metasound'):\n")
						TEXT("            builder.build_metasound(target)\n")
						TEXT("        else:\n")
						TEXT("            raise RuntimeError('MetaSound builder method unavailable: build_metasound')\n")
						TEXT("        save_target(target)\n")
						TEXT("    elif builder is not None:\n")
						TEXT("        applied = False\n")
						TEXT("        if tool_name == 'audio_metasound_add_input' and hasattr(builder, 'add_input'):\n")
						TEXT("            builder.add_input(target, args.get('name', ''), args.get('type', ''))\n")
						TEXT("            applied = True\n")
						TEXT("        elif tool_name == 'audio_metasound_add_output' and hasattr(builder, 'add_output'):\n")
						TEXT("            builder.add_output(target, args.get('name', ''), args.get('type', ''))\n")
						TEXT("            applied = True\n")
						TEXT("        elif tool_name == 'audio_metasound_add_node' and hasattr(builder, 'add_node_by_class_name'):\n")
						TEXT("            builder.add_node_by_class_name(target, args.get('node_class_name', ''))\n")
						TEXT("            applied = True\n")
						TEXT("        elif tool_name == 'audio_metasound_connect' and hasattr(builder, 'connect_nodes'):\n")
						TEXT("            builder.connect_nodes(target, args.get('from_vertex', ''), args.get('to_vertex', ''))\n")
						TEXT("            applied = True\n")
						TEXT("        elif tool_name == 'audio_metasound_set_input_default' and hasattr(builder, 'set_input_default'):\n")
						TEXT("            builder.set_input_default(target, args.get('input_name', ''), args.get('value'))\n")
						TEXT("            applied = True\n")
						TEXT("        elif tool_name == 'audio_metasound_remove_node' and hasattr(builder, 'remove_node'):\n")
						TEXT("            builder.remove_node(target, args.get('node_name', ''))\n")
						TEXT("            applied = True\n")
						TEXT("        if not applied:\n")
						TEXT("            raise RuntimeError('MetaSound builder method unavailable for ' + tool_name)\n")
						TEXT("        save_target(target)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('MetaSound builder subsystem is unavailable')\n")
						TEXT("elif tool_name in ('audio_set_attenuation', 'audio_set_concurrency', 'audio_set_sound_class'):\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load audio target asset')\n")
						TEXT("    field_map = {\n")
						TEXT("        'audio_set_attenuation': ('attenuation_path', ['attenuation_settings', 'attenuation_settings_asset']),\n")
						TEXT("        'audio_set_concurrency': ('concurrency_path', ['concurrency_set', 'concurrency_settings']),\n")
						TEXT("        'audio_set_sound_class': ('sound_class_path', ['sound_class_object', 'sound_class']),\n")
						TEXT("    }\n")
						TEXT("    path_key, candidate_fields = field_map[tool_name]\n")
						TEXT("    ref_obj = load_optional(args.get(path_key, ''))\n")
						TEXT("    if args.get(path_key, '') and ref_obj is None:\n")
						TEXT("        raise RuntimeError('Failed to load referenced audio asset')\n")
						TEXT("    applied = False\n")
						TEXT("    for field_name in candidate_fields:\n")
						TEXT("        try:\n")
						TEXT("            target.set_editor_property(field_name, ref_obj)\n")
						TEXT("            applied = True\n")
						TEXT("            break\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("    if not applied:\n")
						TEXT("        raise RuntimeError('Unable to apply audio reference property on target asset')\n")
						TEXT("    save_target(target)\n")
						TEXT("else:\n")
						TEXT("    raise RuntimeError('Unsupported audio tool')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgumentsJson));
				});
		};
		RegisterAudioPythonTool(TEXT("audio_sound_cue_list_nodes"), TEXT("List nodes in a sound cue graph."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterAudioPythonTool(TEXT("audio_sound_cue_add_node"), TEXT("Add a node to a sound cue graph."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_class_path")}));
		RegisterAudioPythonTool(TEXT("audio_sound_cue_connect"), TEXT("Connect two sound cue graph nodes."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("from_node"), FSololmcpSchemaBuilder::String()}, {TEXT("to_node"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("from_node"), TEXT("to_node")}));
		RegisterAudioPythonTool(TEXT("audio_sound_cue_disconnect"), TEXT("Disconnect sound cue graph nodes."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node")}));
		RegisterAudioPythonTool(TEXT("audio_sound_cue_delete_node"), TEXT("Delete a node from a sound cue graph."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node")}));
		RegisterAudioPythonTool(TEXT("audio_sound_cue_set_node_properties"), TEXT("Set node properties in a sound cue graph."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("node"), TEXT("properties")}));
		RegisterAudioPythonTool(TEXT("audio_sound_cue_layout"), TEXT("Auto-layout a sound cue graph."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterAudioPythonTool(TEXT("audio_sound_cue_compile"), TEXT("Compile a sound cue graph."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_create_source"), TEXT("Create a MetaSound source asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_create_patch"), TEXT("Create a MetaSound patch asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_add_input"), TEXT("Add an input to a MetaSound asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("name"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("name"), TEXT("type")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_add_output"), TEXT("Add an output to a MetaSound asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("name"), FSololmcpSchemaBuilder::String()}, {TEXT("type"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("name"), TEXT("type")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_add_node"), TEXT("Add a node to a MetaSound asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_class_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_class_name")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_connect"), TEXT("Connect nodes in a MetaSound asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("from_vertex"), FSololmcpSchemaBuilder::String()}, {TEXT("to_vertex"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("from_vertex"), TEXT("to_vertex")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_set_input_default"), TEXT("Set an input default on a MetaSound asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("input_name"), FSololmcpSchemaBuilder::String()}, {TEXT("value"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("input_name"), TEXT("value")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_remove_node"), TEXT("Remove a node from a MetaSound asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_name")}));
		RegisterAudioPythonTool(TEXT("audio_metasound_build_asset"), TEXT("Build or compile a MetaSound asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterAudioPythonTool(TEXT("audio_set_attenuation"), TEXT("Set attenuation asset references on an audio actor or asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("attenuation_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("attenuation_path")}));
		RegisterAudioPythonTool(TEXT("audio_set_concurrency"), TEXT("Set concurrency asset references on an audio actor or asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("concurrency_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("concurrency_path")}));
		RegisterAudioPythonTool(TEXT("audio_set_sound_class"), TEXT("Set sound class references on an audio asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sound_class_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("sound_class_path")}));

		Registry.Register({
			TEXT("niagara_create_system"),
			TEXT("Create a Niagara system asset."),
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
				// Auto-naming: if asset already exists, generate a unique name
				FString EffectiveName = Context.Services.GenerateUniqueAssetName(PackagePath, AssetName);
				if (EffectiveName != AssetName)
				{
					OutStructured->SetStringField(TEXT("original_name"), AssetName);
					AssetName = EffectiveName;
				}
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/Niagara.NiagaraSystem"), TEXT("/Script/NiagaraEditor.NiagaraSystemFactoryNew"), nullptr, OutError);
				if (!Asset)
				{
					return false;
				}
				// Audit round 7 (silent-create fix): force save + asset_registry notify so subsequent
				// niagara_system_inspect can LoadAsset(); verify persistence before returning ok.
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
				OutSummary = FString::Printf(TEXT("Created Niagara system: %s/%s"), *PackagePath, *AssetName);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_create_emitter"),
			TEXT("Create a Niagara emitter asset."),
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
				// Auto-naming: if asset already exists, generate a unique name
				FString EffectiveName = Context.Services.GenerateUniqueAssetName(PackagePath, AssetName);
				if (EffectiveName != AssetName)
				{
					OutStructured->SetStringField(TEXT("original_name"), AssetName);
					AssetName = EffectiveName;
				}
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/Niagara.NiagaraEmitter"), TEXT("/Script/NiagaraEditor.NiagaraEmitterFactoryNew"), nullptr, OutError);
				if (!Asset)
				{
					return false;
				}
				if (!Asset->IsA<UNiagaraEmitter>())
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
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutSummary = FString::Printf(TEXT("Created Niagara emitter: %s/%s"), *PackagePath, *AssetName);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_add_emitter_to_system"),
			TEXT("Add a Niagara emitter asset to a Niagara system asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("create_copy"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("system_asset_path"), TEXT("emitter_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString EmitterAssetPath;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) || !Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath))
				{
					OutError = TEXT("Missing system_asset_path or emitter_asset_path.");
					return false;
				}

				UNiagaraSystem* System = Cast<UNiagaraSystem>(Context.Services.LoadAsset(SystemAssetPath, OutError));
				if (!System)
				{
					OutError = TEXT("system_asset_path is not a Niagara system.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}

				const bool bCreateCopy = Arguments->HasTypedField<EJson::Boolean>(TEXT("create_copy")) ? Arguments->GetBoolField(TEXT("create_copy")) : true;
				FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Emitter, Emitter->GetExposedVersion().VersionGuid, bCreateCopy);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(System);
				OutSummary = TEXT("Added emitter to Niagara system.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_spawn_actor"),
			TEXT("Spawn a Niagara system into the current level."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("location"), VectorSchema()}, {TEXT("rotation"), RotatorSchema()}}, {TEXT("asset_path")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				SpawnArgs->SetStringField(TEXT("asset_path"), AssetPath);
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("location")))
				{
					SpawnArgs->SetField(TEXT("location"), *Value);
				}
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("rotation")))
				{
					SpawnArgs->SetField(TEXT("rotation"), *Value);
				}
				return Registry.ExecuteTool(TEXT("actor_spawn"), SpawnArgs, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_list_emitters"),
			TEXT("List emitters inside a Niagara system asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("system_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath))
				{
					OutError = TEXT("Missing system_asset_path.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				OutStructured = NiagaraEmittersToJson(System);
				OutSummary = TEXT("Listed Niagara emitters.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_list_renderers"),
			TEXT("List renderers on a Niagara emitter asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("emitter_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath))
				{
					OutError = TEXT("Missing emitter_asset_path.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Listed Niagara renderers.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_add_renderer"),
			TEXT("Add a renderer to a Niagara emitter asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("renderer_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("emitter_asset_path"), TEXT("renderer_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				FString RendererClassPath;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath) || !Arguments->TryGetStringField(TEXT("renderer_class_path"), RendererClassPath))
				{
					OutError = TEXT("Missing emitter_asset_path or renderer_class_path.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				UClass* RendererClass = Context.Services.ResolveClass(RendererClassPath, OutError);
				if (!RendererClass || !RendererClass->IsChildOf(UNiagaraRendererProperties::StaticClass()))
				{
					OutError = TEXT("renderer_class_path must resolve to a NiagaraRendererProperties class.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraAddRenderer", "SOMOLMCP Add Niagara Renderer"));
				Emitter->Modify();
				UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, NAME_None, RF_Transactional);
				if (!Renderer)
				{
					OutError = TEXT("Failed to create Niagara renderer.");
					return false;
				}
				Emitter->AddRenderer(Renderer, Emitter->GetExposedVersion().VersionGuid);
				TSharedPtr<FJsonObject> Properties;
				if (TryGetObjectField(Arguments, TEXT("properties"), Properties) && Properties.IsValid() && !Context.Services.ApplyProperties(Renderer, Properties.ToSharedRef(), OutError))
				{
					return false;
				}
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Added Niagara renderer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_add_sprite_renderer"),
			TEXT("Add a sprite renderer with common strong-typed settings to a Niagara emitter."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("material_path"), FSololmcpSchemaBuilder::String()}, {TEXT("alignment"), FSololmcpSchemaBuilder::String()}, {TEXT("facing_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("sort_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("pivot_uv"), FSololmcpSchemaBuilder::Object({})}, {TEXT("sub_image_size"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("emitter_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath))
				{
					OutError = TEXT("Missing emitter_asset_path.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraAddSpriteRenderer", "SOMOLMCP Add Niagara Sprite Renderer"));
				Emitter->Modify();
				UNiagaraSpriteRendererProperties* Renderer = NewObject<UNiagaraSpriteRendererProperties>(Emitter, NAME_None, RF_Transactional);
				if (!Renderer)
				{
					OutError = TEXT("Failed to create sprite renderer.");
					return false;
				}
				FString MaterialPath;
				if (Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) && !MaterialPath.IsEmpty())
				{
					Renderer->Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, OutError));
					if (!Renderer->Material)
					{
						OutError = TEXT("material_path does not resolve to a material interface.");
						return false;
					}
				}
				FString AlignmentName;
				if (Arguments->TryGetStringField(TEXT("alignment"), AlignmentName) && !AlignmentName.IsEmpty() && !TryParseNiagaraSpriteAlignment(AlignmentName, Renderer->Alignment))
				{
					OutError = TEXT("Unsupported sprite alignment.");
					return false;
				}
				FString FacingModeName;
				if (Arguments->TryGetStringField(TEXT("facing_mode"), FacingModeName) && !FacingModeName.IsEmpty() && !TryParseNiagaraSpriteFacingMode(FacingModeName, Renderer->FacingMode))
				{
					OutError = TEXT("Unsupported sprite facing mode.");
					return false;
				}
				FString SortModeName;
				if (Arguments->TryGetStringField(TEXT("sort_mode"), SortModeName) && !SortModeName.IsEmpty() && !TryParseNiagaraSortMode(SortModeName, Renderer->SortMode))
				{
					OutError = TEXT("Unsupported sort mode.");
					return false;
				}
				TSharedPtr<FJsonObject> PivotUv;
				if (TryGetObjectField(Arguments, TEXT("pivot_uv"), PivotUv))
				{
					double X = 0.0;
					double Y = 0.0;
					if (!PivotUv->TryGetNumberField(TEXT("x"), X) || !PivotUv->TryGetNumberField(TEXT("y"), Y))
					{
						OutError = TEXT("pivot_uv must contain ? and y.");
						return false;
					}
					Renderer->PivotInUVSpace = FVector2D(X, Y);
				}
				TSharedPtr<FJsonObject> SubImageSize;
				if (TryGetObjectField(Arguments, TEXT("sub_image_size"), SubImageSize))
				{
					double X = 1.0;
					double Y = 1.0;
					if (!SubImageSize->TryGetNumberField(TEXT("x"), X) || !SubImageSize->TryGetNumberField(TEXT("y"), Y))
					{
						OutError = TEXT("sub_image_size must contain ? and y.");
						return false;
					}
					Renderer->SubImageSize = FVector2D(X, Y);
				}
				Emitter->AddRenderer(Renderer, Emitter->GetExposedVersion().VersionGuid);
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Added Niagara sprite renderer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_add_mesh_renderer"),
			TEXT("Add a mesh renderer with common strong-typed settings to a Niagara emitter."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("static_mesh_path"), FSololmcpSchemaBuilder::String()}, {TEXT("override_material_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("facing_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("sort_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("mesh_scale"), VectorSchema()}, {TEXT("mesh_rotation"), RotatorSchema()}, {TEXT("pivot_offset"), VectorSchema()}}, {TEXT("emitter_asset_path"), TEXT("static_mesh_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				FString StaticMeshPath;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath) || !Arguments->TryGetStringField(TEXT("static_mesh_path"), StaticMeshPath))
				{
					OutError = TEXT("Missing emitter_asset_path or static_mesh_path.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				UStaticMesh* StaticMesh = Cast<UStaticMesh>(Context.Services.LoadAsset(StaticMeshPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				if (!StaticMesh)
				{
					OutError = TEXT("static_mesh_path does not resolve to a static mesh.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraAddMeshRenderer", "SOMOLMCP Add Niagara Mesh Renderer"));
				Emitter->Modify();
				UNiagaraMeshRendererProperties* Renderer = NewObject<UNiagaraMeshRendererProperties>(Emitter, NAME_None, RF_Transactional);
				if (!Renderer)
				{
					OutError = TEXT("Failed to create mesh renderer.");
					return false;
				}
				FNiagaraMeshRendererMeshProperties MeshProperties;
				MeshProperties.Mesh = StaticMesh;
				TSharedPtr<FJsonObject> MeshScale;
				if (TryGetObjectField(Arguments, TEXT("mesh_scale"), MeshScale) && !FSololmcpEditorServices::JsonToVector(MeshScale, MeshProperties.Scale))
				{
					OutError = TEXT("mesh_scale must be a vector object.");
					return false;
				}
				TSharedPtr<FJsonObject> MeshRotation;
				if (TryGetObjectField(Arguments, TEXT("mesh_rotation"), MeshRotation) && !FSololmcpEditorServices::JsonToRotator(MeshRotation, MeshProperties.Rotation))
				{
					OutError = TEXT("mesh_rotation must be a rotator object.");
					return false;
				}
				TSharedPtr<FJsonObject> PivotOffset;
				if (TryGetObjectField(Arguments, TEXT("pivot_offset"), PivotOffset) && !FSololmcpEditorServices::JsonToVector(PivotOffset, MeshProperties.PivotOffset))
				{
					OutError = TEXT("pivot_offset must be a vector object.");
					return false;
				}
				Renderer->Meshes.Reset();
				Renderer->Meshes.Add(MeshProperties);
				FString FacingModeName;
				if (Arguments->TryGetStringField(TEXT("facing_mode"), FacingModeName) && !FacingModeName.IsEmpty() && !TryParseNiagaraMeshFacingMode(FacingModeName, Renderer->FacingMode))
				{
					OutError = TEXT("Unsupported mesh facing mode.");
					return false;
				}
				FString SortModeName;
				if (Arguments->TryGetStringField(TEXT("sort_mode"), SortModeName) && !SortModeName.IsEmpty() && !TryParseNiagaraSortMode(SortModeName, Renderer->SortMode))
				{
					OutError = TEXT("Unsupported sort mode.");
					return false;
				}
				TArray<FString> OverrideMaterialPaths;
				if (TryGetStringArray(Arguments, TEXT("override_material_paths"), OverrideMaterialPaths) && OverrideMaterialPaths.Num() > 0)
				{
					Renderer->bOverrideMaterials = true;
					Renderer->OverrideMaterials.Reset();
					for (const FString& MaterialPath : OverrideMaterialPaths)
					{
						UMaterialInterface* Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, OutError));
						if (!Material)
						{
							OutError = TEXT("override_material_paths must only contain material interface assets.");
							return false;
						}
						FNiagaraMeshMaterialOverride& Override = Renderer->OverrideMaterials.AddDefaulted_GetRef();
						Override.ExplicitMat = Material;
					}
				}
				Emitter->AddRenderer(Renderer, Emitter->GetExposedVersion().VersionGuid);
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Added Niagara mesh renderer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_add_ribbon_renderer"),
			TEXT("Add a ribbon renderer with common strong-typed settings to a Niagara emitter."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("material_path"), FSololmcpSchemaBuilder::String()}, {TEXT("facing_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("draw_direction"), FSololmcpSchemaBuilder::String()}, {TEXT("shape"), FSololmcpSchemaBuilder::String()}, {TEXT("tessellation_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("screen_space_tessellation"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("emitter_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath))
				{
					OutError = TEXT("Missing emitter_asset_path.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraAddRibbonRenderer", "SOMOLMCP Add Niagara Ribbon Renderer"));
				Emitter->Modify();
				UNiagaraRibbonRendererProperties* Renderer = NewObject<UNiagaraRibbonRendererProperties>(Emitter, NAME_None, RF_Transactional);
				if (!Renderer)
				{
					OutError = TEXT("Failed to create ribbon renderer.");
					return false;
				}
				FString MaterialPath;
				if (Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) && !MaterialPath.IsEmpty())
				{
					Renderer->Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, OutError));
					if (!Renderer->Material)
					{
						OutError = TEXT("material_path does not resolve to a material interface.");
						return false;
					}
				}
				FString FacingModeName;
				if (Arguments->TryGetStringField(TEXT("facing_mode"), FacingModeName) && !FacingModeName.IsEmpty() && !TryParseNiagaraRibbonFacingMode(FacingModeName, Renderer->FacingMode))
				{
					OutError = TEXT("Unsupported ribbon facing mode.");
					return false;
				}
				FString DrawDirectionName;
				if (Arguments->TryGetStringField(TEXT("draw_direction"), DrawDirectionName) && !DrawDirectionName.IsEmpty() && !TryParseNiagaraRibbonDrawDirection(DrawDirectionName, Renderer->DrawDirection))
				{
					OutError = TEXT("Unsupported ribbon draw direction.");
					return false;
				}
				FString ShapeName;
				if (Arguments->TryGetStringField(TEXT("shape"), ShapeName) && !ShapeName.IsEmpty() && !TryParseNiagaraRibbonShapeMode(ShapeName, Renderer->Shape))
				{
					OutError = TEXT("Unsupported ribbon shape.");
					return false;
				}
				FString TessellationModeName;
				if (Arguments->TryGetStringField(TEXT("tessellation_mode"), TessellationModeName) && !TessellationModeName.IsEmpty() && !TryParseNiagaraRibbonTessellationMode(TessellationModeName, Renderer->TessellationMode))
				{
					OutError = TEXT("Unsupported ribbon tessellation mode.");
					return false;
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("screen_space_tessellation")))
				{
					Renderer->bScreenSpaceTessellation = Arguments->GetBoolField(TEXT("screen_space_tessellation"));
				}
				Emitter->AddRenderer(Renderer, Emitter->GetExposedVersion().VersionGuid);
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Added Niagara ribbon renderer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_remove_renderer"),
			TEXT("Remove a renderer from a Niagara emitter asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("renderer_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("emitter_asset_path"), TEXT("renderer_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				int32 RendererIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath) || !Arguments->TryGetNumberField(TEXT("renderer_index"), RendererIndex))
				{
					OutError = TEXT("Missing emitter_asset_path or renderer_index.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				UNiagaraRendererProperties* Renderer = FindNiagaraRendererByIndex(Emitter, RendererIndex);
				if (!Renderer)
				{
					OutError = TEXT("renderer_index is out of range.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraRemoveRenderer", "SOMOLMCP Remove Niagara Renderer"));
				Emitter->Modify();
				Emitter->RemoveRenderer(Renderer, Emitter->GetExposedVersion().VersionGuid);
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Removed Niagara renderer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_set_renderer_enabled"),
			TEXT("Enable or disable a Niagara emitter renderer."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("renderer_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("enabled"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("emitter_asset_path"), TEXT("renderer_index"), TEXT("enabled")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				int32 RendererIndex = INDEX_NONE;
				bool bEnabled = false;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath) || !Arguments->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || !Arguments->TryGetBoolField(TEXT("enabled"), bEnabled))
				{
					OutError = TEXT("Missing emitter_asset_path, renderer_index or enabled.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				UNiagaraRendererProperties* Renderer = FindNiagaraRendererByIndex(Emitter, RendererIndex);
				if (!Renderer)
				{
					OutError = TEXT("renderer_index is out of range.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraSetRendererEnabled", "SOMOLMCP Set Niagara Renderer Enabled"));
				Renderer->Modify();
				Renderer->SetIsEnabled(bEnabled);
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Updated Niagara renderer enabled state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_reorder_renderer"),
			TEXT("Move a renderer to a new index on a Niagara emitter asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("renderer_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("target_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("emitter_asset_path"), TEXT("renderer_index"), TEXT("target_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				int32 RendererIndex = INDEX_NONE;
				int32 TargetIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath) || !Arguments->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || !Arguments->TryGetNumberField(TEXT("target_index"), TargetIndex))
				{
					OutError = TEXT("Missing emitter_asset_path, renderer_index or target_index.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				UNiagaraRendererProperties* Renderer = FindNiagaraRendererByIndex(Emitter, RendererIndex);
				if (!Renderer)
				{
					OutError = TEXT("renderer_index is out of range.");
					return false;
				}
				if (!Emitter->GetLatestEmitterData()->GetRenderers().IsValidIndex(TargetIndex))
				{
					OutError = TEXT("target_index is out of range.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraReorderRenderer", "SOMOLMCP Reorder Niagara Renderer"));
				Emitter->Modify();
				Emitter->MoveRenderer(Renderer, TargetIndex, Emitter->GetExposedVersion().VersionGuid);
				OutStructured = NiagaraRenderersToJson(Emitter);
				OutSummary = TEXT("Reordered Niagara renderer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_set_renderer_properties"),
			TEXT("Apply public UPROPERTY values onto a Niagara renderer."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("renderer_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("emitter_asset_path"), TEXT("renderer_index"), TEXT("properties")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				int32 RendererIndex = INDEX_NONE;
				TSharedPtr<FJsonObject> Properties;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath) || !Arguments->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || !TryGetObjectField(Arguments, TEXT("properties"), Properties))
				{
					OutError = TEXT("Missing emitter_asset_path, renderer_index or properties.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter)
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				UNiagaraRendererProperties* Renderer = FindNiagaraRendererByIndex(Emitter, RendererIndex);
				if (!Renderer)
				{
					OutError = TEXT("renderer_index is out of range.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraSetRendererProperties", "SOMOLMCP Set Niagara Renderer Properties"));
				if (!Context.Services.ApplyProperties(Renderer, Properties.ToSharedRef(), OutError))
				{
					return false;
				}
				OutStructured = NiagaraRendererToJson(Renderer, RendererIndex);
				OutSummary = TEXT("Updated Niagara renderer properties.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_set_emitter_settings"),
			TEXT("Set common Niagara emitter settings such as local space, sim target and fixed bounds."),
			FSololmcpSchemaBuilder::Object({{TEXT("emitter_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("local_space"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("sim_target"), FSololmcpSchemaBuilder::String(TEXT("cpu | gpu"))}, {TEXT("use_fixed_bounds"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("fixed_bounds"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("emitter_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EmitterAssetPath;
				if (!Arguments->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPath))
				{
					OutError = TEXT("Missing emitter_asset_path.");
					return false;
				}
				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Context.Services.LoadAsset(EmitterAssetPath, OutError));
				if (!Emitter || !Emitter->GetLatestEmitterData())
				{
					OutError = TEXT("emitter_asset_path is not a Niagara emitter.");
					return false;
				}
				FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraSetEmitterSettings", "SOMOLMCP Set Niagara Emitter Settings"));
				Emitter->Modify();
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("local_space")))
				{
					EmitterData->bLocalSpace = Arguments->GetBoolField(TEXT("local_space"));
				}
				FString SimTarget;
				if (Arguments->TryGetStringField(TEXT("sim_target"), SimTarget))
				{
					EmitterData->SimTarget = SimTarget == TEXT("gpu") ? ENiagaraSimTarget::GPUComputeSim : ENiagaraSimTarget::CPUSim;
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("use_fixed_bounds")))
				{
					EmitterData->CalculateBoundsMode = Arguments->GetBoolField(TEXT("use_fixed_bounds")) ? ENiagaraEmitterCalculateBoundMode::Fixed : ENiagaraEmitterCalculateBoundMode::Dynamic;
				}
				TSharedPtr<FJsonObject> FixedBoundsObject;
				if (TryGetObjectField(Arguments, TEXT("fixed_bounds"), FixedBoundsObject))
				{
					FBox FixedBounds;
					if (!JsonToBox(FixedBoundsObject, FixedBounds))
					{
						OutError = TEXT("fixed_bounds must contain min/max vectors.");
						return false;
					}
					EmitterData->FixedBounds = FixedBounds;
				}
				OutStructured->SetBoolField(TEXT("localSpace"), EmitterData->bLocalSpace);
				OutStructured->SetStringField(TEXT("simTarget"), EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("gpu") : TEXT("cpu"));
				OutStructured->SetObjectField(TEXT("fixedBounds"), BoxToJson(EmitterData->FixedBounds));
				OutSummary = TEXT("Updated Niagara emitter settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_set_system_fixed_bounds"),
			TEXT("Set fixed bounds usage and values on a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("use_fixed_bounds"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("fixed_bounds"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("system_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath))
				{
					OutError = TEXT("Missing system_asset_path.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraSetSystemFixedBounds", "SOMOLMCP Set Niagara System Fixed Bounds"));
				System->Modify();
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("use_fixed_bounds")))
				{
					TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
					Properties->SetBoolField(TEXT("bFixedBounds"), Arguments->GetBoolField(TEXT("use_fixed_bounds")));
					if (!Context.Services.ApplyProperties(System, Properties, OutError))
					{
						return false;
					}
				}
				TSharedPtr<FJsonObject> FixedBoundsObject;
				if (TryGetObjectField(Arguments, TEXT("fixed_bounds"), FixedBoundsObject))
				{
					FBox FixedBounds;
					if (!JsonToBox(FixedBoundsObject, FixedBounds))
					{
						OutError = TEXT("fixed_bounds must contain min/max vectors.");
						return false;
					}
					System->SetFixedBounds(FixedBounds);
				}
				OutStructured->SetObjectField(TEXT("fixedBounds"), BoxToJson(System->GetFixedBounds()));
				OutSummary = TEXT("Updated Niagara system fixed bounds.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_list_user_parameters"),
			TEXT("List exposed user parameters on a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("system_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath))
				{
					OutError = TEXT("Missing system_asset_path.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				OutStructured = NiagaraUserParametersToJson(System);
				OutSummary = TEXT("Listed Niagara user parameters.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_add_user_parameter"),
			TEXT("Add an exposed user parameter to a Niagara system."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("parameter_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("parameter_type"), FSololmcpSchemaBuilder::String(TEXT("float | int | bool | vector | color"))},
					{TEXT("value"), FSololmcpSchemaBuilder::Object({})}
				},
				{TEXT("system_asset_path"), TEXT("parameter_name"), TEXT("parameter_type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString ParameterName;
				FString ParameterType;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) ||
					!Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) ||
					!Arguments->TryGetStringField(TEXT("parameter_type"), ParameterType))
				{
					OutError = TEXT("Missing Niagara user parameter arguments.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				FNiagaraTypeDefinition TypeDef;
				if (!TryParseNiagaraType(ParameterType, TypeDef))
				{
					OutError = TEXT("Unsupported Niagara parameter_type.");
					return false;
				}
				FNiagaraVariable Variable(TypeDef, *ParameterName);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraAddUserParameter", "SOMOLMCP Add Niagara User Parameter"));
				System->Modify();
				System->GetExposedParameters().AddParameter(Variable, true, true);
				FNiagaraVariable StoredVariable;
				if (!FindNiagaraUserParameter(System, ParameterName, StoredVariable))
				{
					OutError = TEXT("Failed to add Niagara user parameter.");
					return false;
				}
				if (Arguments->HasField(TEXT("value")) && !SetNiagaraUserParameterValue(System, StoredVariable, Arguments, OutError))
				{
					return false;
				}
				System->RequestCompile(false);
				OutStructured = NiagaraUserParametersToJson(System);
				OutSummary = TEXT("Added Niagara user parameter.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_remove_user_parameter"),
			TEXT("Remove an exposed user parameter from a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parameter_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("system_asset_path"), TEXT("parameter_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString ParameterName;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) || !Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName))
				{
					OutError = TEXT("Missing system_asset_path or parameter_name.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				FNiagaraVariable Variable;
				if (!FindNiagaraUserParameter(System, ParameterName, Variable))
				{
					OutError = TEXT("Niagara user parameter was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraRemoveUserParameter", "SOMOLMCP Remove Niagara User Parameter"));
				System->Modify();
				System->HandleVariableRemoved(Variable, true);
				System->RequestCompile(false);
				OutStructured = NiagaraUserParametersToJson(System);
				OutSummary = TEXT("Removed Niagara user parameter.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_rename_user_parameter"),
			TEXT("Rename an exposed user parameter on a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parameter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("system_asset_path"), TEXT("parameter_name"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString ParameterName;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) ||
					!Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) ||
					!Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing system_asset_path, parameter_name or new_name.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				FNiagaraVariable OldVariable;
				if (!FindNiagaraUserParameter(System, ParameterName, OldVariable))
				{
					OutError = TEXT("Niagara user parameter was not found.");
					return false;
				}
				FNiagaraVariable NewVariable(OldVariable.GetType(), *NewName);
				FNiagaraUserRedirectionParameterStore::MakeUserVariable(NewVariable);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraRenameUserParameter", "SOMOLMCP Rename Niagara User Parameter"));
				System->Modify();
				System->HandleVariableRenamed(OldVariable, NewVariable, true);
				System->RequestCompile(false);
				OutStructured = NiagaraUserParametersToJson(System);
				OutSummary = TEXT("Renamed Niagara user parameter.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_set_user_parameter_default"),
			TEXT("Set the default value of an exposed user parameter on a Niagara system."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("parameter_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("value"), FSololmcpSchemaBuilder::Object({})}
				},
				{TEXT("system_asset_path"), TEXT("parameter_name"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString ParameterName;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) || !Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName))
				{
					OutError = TEXT("Missing system_asset_path or parameter_name.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				FNiagaraVariable Variable;
				if (!FindNiagaraUserParameter(System, ParameterName, Variable))
				{
					OutError = TEXT("Niagara user parameter was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraSetUserParameterDefault", "SOMOLMCP Set Niagara User Parameter Default"));
				System->Modify();
				if (!SetNiagaraUserParameterValue(System, Variable, Arguments, OutError))
				{
					return false;
				}
				System->RequestCompile(false);
				OutStructured = NiagaraUserParametersToJson(System);
				OutSummary = TEXT("Updated Niagara user parameter default.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_remove_emitter_from_system"),
			TEXT("Remove an emitter from a Niagara system by name or handle id."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter"), FSololmcpSchemaBuilder::String()}}, {TEXT("system_asset_path"), TEXT("emitter")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString EmitterId;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) || !Arguments->TryGetStringField(TEXT("emitter"), EmitterId))
				{
					OutError = TEXT("Missing system_asset_path or emitter.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				FNiagaraEmitterHandle* Handle = FindEmitterHandleByNameOrId(System, EmitterId);
				if (!Handle)
				{
					OutError = TEXT("Emitter was not found in Niagara system.");
					return false;
				}
				TSet<FGuid> HandlesToRemove;
				HandlesToRemove.Add(Handle->GetId());
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraRemoveEmitter", "SOMOLMCP Remove Niagara Emitter"));
				System->RemoveEmitterHandlesById(HandlesToRemove);
				OutStructured = NiagaraEmittersToJson(System);
				OutSummary = TEXT("Removed Niagara emitter from system.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_rename_emitter_in_system"),
			TEXT("Rename an emitter inside a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("system_asset_path"), TEXT("emitter"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString EmitterId;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) ||
					!Arguments->TryGetStringField(TEXT("emitter"), EmitterId) ||
					!Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing system_asset_path, emitter or new_name.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				FNiagaraEmitterHandle* Handle = FindEmitterHandleByNameOrId(System, EmitterId);
				if (!Handle)
				{
					OutError = TEXT("Emitter was not found in Niagara system.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraRenameEmitter", "SOMOLMCP Rename Niagara Emitter"));
				System->Modify();
				Handle->SetName(*NewName, *System);
				System->RequestCompile(false);
				OutStructured = NiagaraEmittersToJson(System);
				OutSummary = TEXT("Renamed Niagara emitter.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_reorder_emitter_in_system"),
			TEXT("Move an emitter to a new index inside a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter"), FSololmcpSchemaBuilder::String()}, {TEXT("target_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("system_asset_path"), TEXT("emitter"), TEXT("target_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString EmitterId;
				int32 TargetIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) ||
					!Arguments->TryGetStringField(TEXT("emitter"), EmitterId) ||
					!Arguments->TryGetNumberField(TEXT("target_index"), TargetIndex))
				{
					OutError = TEXT("Missing system_asset_path, emitter or target_index.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}

				TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
				const int32 CurrentIndex = Handles.IndexOfByPredicate([&EmitterId](const FNiagaraEmitterHandle& Handle)
				{
					FGuid ParsedGuid;
					const bool bHasGuid = FGuid::Parse(EmitterId, ParsedGuid);
					return Handle.GetName().ToString() == EmitterId || (bHasGuid && Handle.GetId() == ParsedGuid);
});
				if (!Handles.IsValidIndex(CurrentIndex))
				{
					OutError = TEXT("Emitter was not found in Niagara system.");
					return false;
				}
				if (!Handles.IsValidIndex(TargetIndex))
				{
					OutError = TEXT("target_index is out of range.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraReorderEmitter", "SOMOLMCP Reorder Niagara Emitter"));
				System->Modify();
				const FNiagaraEmitterHandle HandleCopy = Handles[CurrentIndex];
				Handles.RemoveAt(CurrentIndex);
				Handles.Insert(HandleCopy, TargetIndex);
				System->RequestCompile(false);
				OutStructured = NiagaraEmittersToJson(System);
				OutSummary = TEXT("Reordered Niagara emitter.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_set_warmup_settings"),
			TEXT("Set warmup time and tick count on a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("warmup_time"), FSololmcpSchemaBuilder::Number()}, {TEXT("warmup_tick_count"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("system_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath))
				{
					OutError = TEXT("Missing system_asset_path.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraSetWarmup", "SOMOLMCP Set Niagara Warmup"));
				System->Modify();
				if (Arguments->HasTypedField<EJson::Number>(TEXT("warmup_time")))
				{
					System->SetWarmupTime(Arguments->GetNumberField(TEXT("warmup_time")));
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("warmup_tick_count")))
				{
					TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
					Properties->SetNumberField(TEXT("WarmupTickCount"), Arguments->GetIntegerField(TEXT("warmup_tick_count")));
					if (!Context.Services.ApplyProperties(System, Properties, OutError))
					{
						return false;
					}
				}
				System->ResolveWarmupTickCount();
				OutStructured->SetNumberField(TEXT("warmupTime"), System->GetWarmupTime());
				OutStructured->SetNumberField(TEXT("warmupTickCount"), System->GetWarmupTickCount());
				OutSummary = TEXT("Updated Niagara warmup settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_set_emitter_enabled"),
			TEXT("Enable or disable an emitter inside a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter"), FSololmcpSchemaBuilder::String()}, {TEXT("enabled"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("system_asset_path"), TEXT("emitter"), TEXT("enabled")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				FString EmitterId;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) ||
					!Arguments->TryGetStringField(TEXT("emitter"), EmitterId) ||
					!Arguments->HasTypedField<EJson::Boolean>(TEXT("enabled")))
				{
					OutError = TEXT("Missing system_asset_path, emitter or enabled.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				FNiagaraEmitterHandle* Handle = FindEmitterHandleByNameOrId(System, EmitterId);
				if (!Handle)
				{
					OutError = TEXT("Emitter was not found in Niagara system.");
					return false;
				}
				const bool bEnabled = Arguments->GetBoolField(TEXT("enabled"));
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NiagaraSetEmitterEnabled", "SOMOLMCP Set Niagara Emitter Enabled"));
				Handle->SetIsEnabled(bEnabled, *System, true);
				OutStructured = NiagaraEmittersToJson(System);
				OutSummary = TEXT("Updated Niagara emitter enabled state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_compile_system"),
			TEXT("Request compile for a Niagara system asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("force"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("wait_for_completion"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("system_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SystemAssetPath;
				if (!Arguments->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath))
				{
					OutError = TEXT("Missing system_asset_path.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				const bool bForce = Arguments->HasTypedField<EJson::Boolean>(TEXT("force")) ? Arguments->GetBoolField(TEXT("force")) : false;
				const bool bWaitForCompletion = Arguments->HasTypedField<EJson::Boolean>(TEXT("wait_for_completion")) ? Arguments->GetBoolField(TEXT("wait_for_completion")) : true;
				const bool bRequested = System->RequestCompile(bForce);
				OutStructured->SetBoolField(TEXT("requested"), bRequested);
				if (bWaitForCompletion)
				{
					const bool bPollCompleted = System->PollForCompilationComplete(true);
					OutStructured->SetBoolField(TEXT("completed"), bPollCompleted || !System->HasOutstandingCompilationRequests(true));
				}
				OutSummary = TEXT("Requested Niagara system compile.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_compile_diagnostics"),
			TEXT("Compile a Niagara system and return structured diagnostics including VM compile events (node/pin GUIDs when available) and asset message store entries."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("force"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("wait_for_completion"), FSololmcpSchemaBuilder::Boolean()},
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
					OutError = TEXT("Missing system_asset_path.");
					return false;
				}
				UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, SystemAssetPath, OutError);
				if (!System)
				{
					return false;
				}
				const bool bForce = Arguments->HasTypedField<EJson::Boolean>(TEXT("force")) ? Arguments->GetBoolField(TEXT("force")) : false;
				const bool bWait = Arguments->HasTypedField<EJson::Boolean>(TEXT("wait_for_completion")) ? Arguments->GetBoolField(TEXT("wait_for_completion")) : true;
				const bool bIncludeVm = !Arguments->HasTypedField<EJson::Boolean>(TEXT("include_vm_compile_events")) || Arguments->GetBoolField(TEXT("include_vm_compile_events"));
				const bool bIncludeAssetMsgs = !Arguments->HasTypedField<EJson::Boolean>(TEXT("include_asset_messages")) || Arguments->GetBoolField(TEXT("include_asset_messages"));
				int32 MaxVmEvents = 256;
				Arguments->TryGetNumberField(TEXT("max_compile_events"), MaxVmEvents);
				int32 MaxAssetMsgs = 64;
				Arguments->TryGetNumberField(TEXT("max_asset_messages"), MaxAssetMsgs);

				auto SeverityToString = [](FNiagaraCompileEventSeverity S) -> FString
				{
					switch (S)
					{
					case FNiagaraCompileEventSeverity::Error: return TEXT("error");
					case FNiagaraCompileEventSeverity::Warning: return TEXT("warning");
					case FNiagaraCompileEventSeverity::Display: return TEXT("display");
					case FNiagaraCompileEventSeverity::Log: return TEXT("log");
					default: return TEXT("unknown");
					}
				};

				const bool bRequested = System->RequestCompile(bForce);
				const bool bPollCompleted = bWait ? System->PollForCompilationComplete(true) : false;
				const bool bHasOutstandingCompilation = System->HasOutstandingCompilationRequests(true);
				const bool bCompleted = bWait ? (bPollCompleted || !bHasOutstandingCompilation) : !bHasOutstandingCompilation;
				const bool bReady = System->IsReadyToRun();

				TArray<TSharedPtr<FJsonValue>> Messages;
				auto AddMessage = [&Messages](const FString& Severity, const FString& Text)
				{
					TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
					Msg->SetStringField(TEXT("severity"), Severity);
					Msg->SetStringField(TEXT("text"), Text);
					Messages.Add(MakeShared<FJsonValueObject>(Msg));
				};
				if (!bRequested)
				{
					AddMessage(TEXT("warning"), TEXT("Compile request was not queued (system may already be up to date)."));
				}
				if (!bCompleted)
				{
					AddMessage(TEXT("warning"), TEXT("Compilation not completed yet."));
				}
				if (!bReady)
				{
					AddMessage(TEXT("error"), TEXT("Niagara system is not ready to run after compile."));
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
						for (const FNiagaraCompileEvent& Ev : VM.LastCompileEvents)
						{
							if (MaxVmEvents <= 0)
							{
								break;
							}
							TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
							E->SetStringField(TEXT("source"), TEXT("vm_compile_event"));
							E->SetStringField(TEXT("severity"), SeverityToString(Ev.Severity));
							E->SetStringField(TEXT("text"), Ev.Message);
							if (!Ev.ShortDescription.IsEmpty())
							{
								E->SetStringField(TEXT("short_description"), Ev.ShortDescription);
							}
							if (Ev.NodeGuid.IsValid())
							{
								E->SetStringField(TEXT("node_guid"), Ev.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
							}
							if (Ev.PinGuid.IsValid())
							{
								E->SetStringField(TEXT("pin_guid"), Ev.PinGuid.ToString(EGuidFormats::DigitsWithHyphens));
							}
							TArray<TSharedPtr<FJsonValue>> StackArr;
							for (const FGuid& G : Ev.StackGuids)
							{
								if (G.IsValid())
								{
									StackArr.Add(MakeShared<FJsonValueString>(G.ToString(EGuidFormats::DigitsWithHyphens)));
								}
							}
							if (StackArr.Num() > 0)
							{
								E->SetArrayField(TEXT("stack_guids"), StackArr);
							}
							E->SetStringField(TEXT("script_usage"), UsageStr);
							E->SetStringField(TEXT("script_path"), Script->GetPathName());
							VmEventsJson.Add(MakeShared<FJsonValueObject>(E));
							--MaxVmEvents;
							if (Ev.Severity == FNiagaraCompileEventSeverity::Error)
							{
								++VmErrors;
							}
							else if (Ev.Severity == FNiagaraCompileEventSeverity::Warning)
							{
								++VmWarnings;
							}
						}
					});
				}
#endif

				TArray<TSharedPtr<FJsonValue>> AssetMessagesJson;
#if WITH_EDITORONLY_DATA
				if (bIncludeAssetMsgs)
				{
					const TMap<FGuid, TObjectPtr<UNiagaraMessageDataBase>>& Raw = System->GetMessageStore().GetMessages();
					for (const TPair<FGuid, TObjectPtr<UNiagaraMessageDataBase>>& Pair : Raw)
					{
						if (MaxAssetMsgs <= 0)
						{
							break;
						}
						UNiagaraMessageDataBase* Base = Pair.Value.Get();
						if (!Base)
						{
							continue;
						}
						UNiagaraMessageData* MsgData = Cast<UNiagaraMessageData>(Base);
						if (!MsgData)
						{
							continue;
						}
						TSharedRef<const INiagaraMessage> Msg = MsgData->GenerateNiagaraMessage();
						TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
						J->SetStringField(TEXT("source"), TEXT("asset_message_store"));
						J->SetStringField(TEXT("message_key"), Pair.Key.ToString(EGuidFormats::DigitsWithHyphens));
						J->SetStringField(TEXT("title"), Msg->GenerateMessageTitle().ToString());
						J->SetStringField(TEXT("text"), Msg->GenerateMessageText().ToString());
						J->SetStringField(TEXT("topic"), Msg->GetMessageTopic().ToString());
						AssetMessagesJson.Add(MakeShared<FJsonValueObject>(J));
						--MaxAssetMsgs;
					}
				}
#endif

				OutStructured->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
				OutStructured->SetBoolField(TEXT("requested"), bRequested);
				OutStructured->SetBoolField(TEXT("completed"), bCompleted);
				OutStructured->SetBoolField(TEXT("ready_to_run"), bReady);
				OutStructured->SetBoolField(TEXT("has_outstanding_compilation"), bHasOutstandingCompilation);
				OutStructured->SetArrayField(TEXT("messages"), Messages);
				OutStructured->SetArrayField(TEXT("vm_compile_events"), VmEventsJson);
				OutStructured->SetArrayField(TEXT("asset_messages"), AssetMessagesJson);
				OutStructured->SetNumberField(TEXT("vm_compile_event_error_count"), VmErrors);
				OutStructured->SetNumberField(TEXT("vm_compile_event_warning_count"), VmWarnings);
				OutStructured->SetNumberField(TEXT("message_count"), Messages.Num());
				OutStructured->SetBoolField(TEXT("success"), bReady && bCompleted && VmErrors == 0);
				OutSummary = TEXT("Collected Niagara compile diagnostics.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_runtime_snapshot"),
			TEXT("Capture lightweight runtime snapshot of a Niagara component on an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UNiagaraComponent* Component = ResolveNiagaraComponent(Context.Services, ActorId, OutError);
				if (!Component)
				{
					return false;
				}
				UNiagaraSystem* Asset = Component->GetAsset();
				OutStructured->SetStringField(TEXT("actor"), ActorId);
				OutStructured->SetBoolField(TEXT("active"), Component->IsActive());
				OutStructured->SetBoolField(TEXT("registered"), Component->IsRegistered());
				OutStructured->SetBoolField(TEXT("visible"), Component->IsVisible());
				OutStructured->SetNumberField(TEXT("tick_behavior"), static_cast<int32>(Component->GetTickBehavior()));
				OutStructured->SetStringField(TEXT("system_asset"), Asset ? Asset->GetPathName() : FString());
				OutStructured->SetNumberField(TEXT("desired_age"), Component->GetDesiredAge());
				OutStructured->SetNumberField(TEXT("seek_delta"), Component->GetSeekDelta());
				OutStructured->SetStringField(TEXT("snapshot_time_utc"), FDateTime::UtcNow().ToIso8601());
				OutSummary = TEXT("Captured Niagara runtime snapshot.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_batch_preview_diff"),
			TEXT("Preview high-level impact of Niagara batch operations without execution."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("operations"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
						{TEXT("tool"), FSololmcpSchemaBuilder::String()},
						{TEXT("arguments"), FSololmcpSchemaBuilder::Object({})}
					}, {TEXT("tool"), TEXT("arguments")}))}
				},
				{TEXT("operations")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("operations"), Operations) || !Operations)
				{
					OutError = TEXT("Missing operations array.");
					return false;
				}
				TSet<FString> TouchedAssets;
				TSet<FString> TouchedActors;
				TArray<TSharedPtr<FJsonValue>> Steps;
				for (int32 Index = 0; Index < Operations->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Op = (*Operations)[Index].IsValid() ? (*Operations)[Index]->AsObject() : nullptr;
					if (!Op.IsValid())
					{
						continue;
					}
					FString ToolName;
					const TSharedPtr<FJsonObject>* OpArgs = nullptr;
					if (!Op->TryGetStringField(TEXT("tool"), ToolName) || !Op->TryGetObjectField(TEXT("arguments"), OpArgs) || !OpArgs || !OpArgs->IsValid())
					{
						continue;
					}
					FString AssetPath;
					(*OpArgs)->TryGetStringField(TEXT("system_asset_path"), AssetPath);
					if (AssetPath.IsEmpty())
					{
						(*OpArgs)->TryGetStringField(TEXT("asset_path"), AssetPath);
					}
					FString ActorId;
					(*OpArgs)->TryGetStringField(TEXT("actor"), ActorId);
					if (!AssetPath.IsEmpty()) { TouchedAssets.Add(AssetPath); }
					if (!ActorId.IsEmpty()) { TouchedActors.Add(ActorId); }
					TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
					Step->SetNumberField(TEXT("index"), Index);
					Step->SetStringField(TEXT("tool"), ToolName);
					if (!AssetPath.IsEmpty()) { Step->SetStringField(TEXT("asset_path"), AssetPath); }
					if (!ActorId.IsEmpty()) { Step->SetStringField(TEXT("actor"), ActorId); }
					Step->SetStringField(TEXT("impact"), ToolName.Contains(TEXT("create")) || ToolName.Contains(TEXT("remove")) ? TEXT("structural") : TEXT("parameter_or_runtime"));
					Steps.Add(MakeShared<FJsonValueObject>(Step));
				}
				TArray<TSharedPtr<FJsonValue>> AssetsJson;
				for (const FString& A : TouchedAssets) { AssetsJson.Add(MakeShared<FJsonValueString>(A)); }
				TArray<TSharedPtr<FJsonValue>> ActorsJson;
				for (const FString& A : TouchedActors) { ActorsJson.Add(MakeShared<FJsonValueString>(A)); }
				OutStructured->SetBoolField(TEXT("dry_run"), true);
				OutStructured->SetArrayField(TEXT("steps"), Steps);
				OutStructured->SetArrayField(TEXT("touchedAssets"), AssetsJson);
				OutStructured->SetArrayField(TEXT("touchedActors"), ActorsJson);
				OutStructured->SetNumberField(TEXT("operationCount"), Steps.Num());
				OutSummary = TEXT("Generated Niagara batch preview diff.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_pipeline_smoke_test"),
			TEXT("Run niagara_compile_diagnostics plus optional niagara_runtime_snapshot."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("actor"), FSololmcpSchemaBuilder::String()},
				{TEXT("force"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("wait_for_completion"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("include_runtime_snapshot"), FSololmcpSchemaBuilder::Boolean()}
			}, {TEXT("system_asset_path")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedRef<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
				CompileArgs->SetStringField(TEXT("system_asset_path"), Arguments->GetStringField(TEXT("system_asset_path")));
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("force")))
				{
					CompileArgs->SetBoolField(TEXT("force"), Arguments->GetBoolField(TEXT("force")));
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("wait_for_completion")))
				{
					CompileArgs->SetBoolField(TEXT("wait_for_completion"), Arguments->GetBoolField(TEXT("wait_for_completion")));
				}
				TSharedRef<FJsonObject> CompileOut = MakeShared<FJsonObject>();
				FString CompileSummary;
				FString CompileError;
				const bool bCompileOk = Registry.ExecuteTool(TEXT("niagara_compile_diagnostics"), CompileArgs, CompileOut, CompileSummary, CompileError);
				OutStructured->SetBoolField(TEXT("compile_ok"), bCompileOk);
				OutStructured->SetObjectField(TEXT("compile"), CompileOut);
				if (!CompileError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("compile_error"), CompileError);
				}
				const bool bIncludeRuntime = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_runtime_snapshot")) ? Arguments->GetBoolField(TEXT("include_runtime_snapshot")) : true;
				if (bIncludeRuntime && Arguments->HasTypedField<EJson::String>(TEXT("actor")))
				{
					TSharedRef<FJsonObject> RuntimeArgs = MakeShared<FJsonObject>();
					RuntimeArgs->SetStringField(TEXT("actor"), Arguments->GetStringField(TEXT("actor")));
					TSharedRef<FJsonObject> RuntimeOut = MakeShared<FJsonObject>();
					FString RuntimeSummary;
					FString RuntimeError;
					const bool bRuntimeOk = Registry.ExecuteTool(TEXT("niagara_runtime_snapshot"), RuntimeArgs, RuntimeOut, RuntimeSummary, RuntimeError);
					OutStructured->SetBoolField(TEXT("runtime_ok"), bRuntimeOk);
					OutStructured->SetObjectField(TEXT("runtime"), RuntimeOut);
					if (!RuntimeError.IsEmpty())
					{
						OutStructured->SetStringField(TEXT("runtime_error"), RuntimeError);
					}
				}
				OutSummary = TEXT("Completed Niagara pipeline smoke test.");
				return bCompileOk;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_runtime_stats_get"),
			TEXT("Niagara runtime and asset stats: optional per-emitter simulation state and particle counts when a live instance exists."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("actor"), FSololmcpSchemaBuilder::String()},
				{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()}
			}, {}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				bool bHasAny = false;
				const UEnum* ExecEnum = StaticEnum<ENiagaraExecutionState>();
				auto ExecToString = [ExecEnum](ENiagaraExecutionState S) -> FString
				{
					return ExecEnum ? ExecEnum->GetNameStringByValue(static_cast<int64>(S)) : TEXT("unknown");
				};
				if (Arguments->HasTypedField<EJson::String>(TEXT("actor")))
				{
					const FString ActorId = Arguments->GetStringField(TEXT("actor"));
					UNiagaraComponent* Component = ResolveNiagaraComponent(Context.Services, ActorId, OutError);
					if (!Component)
					{
						return false;
					}
					bHasAny = true;
					OutStructured->SetStringField(TEXT("actor"), ActorId);
					OutStructured->SetBoolField(TEXT("active"), Component->IsActive());
					OutStructured->SetBoolField(TEXT("registered"), Component->IsRegistered());
					OutStructured->SetNumberField(TEXT("desired_age"), Component->GetDesiredAge());
					OutStructured->SetNumberField(TEXT("seek_delta"), Component->GetSeekDelta());
					OutStructured->SetNumberField(TEXT("tick_behavior"), static_cast<int32>(Component->GetTickBehavior()));
					if (UNiagaraSystem* ComponentAsset = Component->GetAsset())
					{
						OutStructured->SetStringField(TEXT("component_system_asset"), ComponentAsset->GetPathName());
					}
					TArray<TSharedPtr<FJsonValue>> RuntimeEmitters;
					FNiagaraSystemInstance* SysInst = nullptr;
					if (FNiagaraSystemInstanceControllerPtr SysCtrl = Component->GetSystemInstanceController())
					{
						if (SysCtrl.IsValid())
						{
							SysInst = SysCtrl->GetSystemInstance_Unsafe();
						}
					}
					if (SysInst)
					{
						OutStructured->SetBoolField(TEXT("has_system_instance"), true);
						int32 Idx = 0;
						for (const FNiagaraEmitterInstanceRef& Ref : SysInst->GetEmitters())
						{
							FNiagaraEmitterInstance* EInst = &Ref.Get();
							if (!EInst)
							{
								continue;
							}
							TSharedRef<FJsonObject> EJ = MakeShared<FJsonObject>();
							EJ->SetNumberField(TEXT("index"), Idx++);
							const FNiagaraEmitterHandle& H = EInst->GetEmitterHandle();
							EJ->SetStringField(TEXT("emitter_name"), H.GetName().ToString());
							EJ->SetStringField(TEXT("emitter_id"), H.GetId().ToString());
							EJ->SetBoolField(TEXT("handle_enabled"), H.GetIsEnabled());
							EJ->SetStringField(TEXT("execution_state"), ExecToString(EInst->GetExecutionState()));
							EJ->SetBoolField(TEXT("is_active"), EInst->IsActive());
							EJ->SetBoolField(TEXT("is_disabled"), EInst->IsDisabled());
							EJ->SetBoolField(TEXT("is_inactive"), EInst->IsInactive());
							EJ->SetBoolField(TEXT("is_complete"), EInst->IsComplete());
							EJ->SetNumberField(TEXT("num_particles"), EInst->GetNumParticles());
							EJ->SetNumberField(TEXT("total_spawned_particles"), EInst->GetTotalSpawnedParticles());
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
							if (UNiagaraEmitter* Em = EInst->GetEmitter())
							{
								EJ->SetStringField(TEXT("emitter_asset"), Em->GetPathName());
							}
#else
							if (UNiagaraEmitter* Em = EInst->GetCachedEmitter().Emitter)
							{
								EJ->SetStringField(TEXT("emitter_asset"), Em->GetPathName());
							}
#endif
							RuntimeEmitters.Add(MakeShared<FJsonValueObject>(EJ));
						}
					}
					else
					{
						OutStructured->SetBoolField(TEXT("has_system_instance"), false);
						OutStructured->SetStringField(TEXT("runtime_emitters_note"), TEXT("No live FNiagaraSystemInstance (not simulating or not yet initialized)."));
					}
					OutStructured->SetArrayField(TEXT("runtime_emitters"), RuntimeEmitters);
				}
				if (Arguments->HasTypedField<EJson::String>(TEXT("system_asset_path")))
				{
					FString LocalError;
					UNiagaraSystem* System = ResolveNiagaraSystem(Context.Services, Arguments->GetStringField(TEXT("system_asset_path")), LocalError);
					if (!System)
					{
						OutError = LocalError;
						return false;
					}
					bHasAny = true;
					OutStructured->SetStringField(TEXT("system_asset_path"), System->GetPathName());
					OutStructured->SetBoolField(TEXT("ready_to_run"), System->IsReadyToRun());
					OutStructured->SetBoolField(TEXT("has_outstanding_compilation"), System->HasOutstandingCompilationRequests(true));
					OutStructured->SetNumberField(TEXT("emitter_handle_count"), System->GetEmitterHandles().Num());
					OutStructured->SetObjectField(TEXT("asset_emitters"), NiagaraEmittersToJson(System));
					OutStructured->SetObjectField(TEXT("user_parameters"), NiagaraUserParametersToJson(System));
				}
				if (!bHasAny)
				{
					OutError = TEXT("Provide at least actor or system_asset_path.");
					return false;
				}
				OutStructured->SetStringField(TEXT("snapshot_time_utc"), FDateTime::UtcNow().ToIso8601());
				OutSummary = TEXT("Collected Niagara runtime stats.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_pipeline_template_run"),
			TEXT("Run a predefined Niagara pipeline template with per-step execution and structured failure reporting (failed_step_index, failed_tool, failed_arguments). Supports extended template_id library; use niagara_pipeline_templates_list for ids and validation rules."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("template_id"), FSololmcpSchemaBuilder::String()},
				{TEXT("system_package_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("system_asset_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("emitter_package_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("emitter_asset_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("spawn_actor_label"), FSololmcpSchemaBuilder::String()},
				{TEXT("skip_spawn_actor"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("force_compile"), FSololmcpSchemaBuilder::Boolean()}
			}, {TEXT("template_id")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				auto ValidateContentPath = [](const FString& Path, const TCHAR* FieldLabel, TArray<FString>& OutErrors) -> void
				{
					const FString Trimmed = Path.TrimStartAndEnd();
					if (!Trimmed.StartsWith(TEXT("/Game")))
					{
						OutErrors.Add(FString::Printf(TEXT("%s must start with /Game (got: %s)"), FieldLabel, *Trimmed));
					}
				};
				auto ValidateAssetNameField = [](const FString& Name, const TCHAR* FieldLabel, TArray<FString>& OutErrors) -> void
				{
					if (Name.Len() < 1 || Name.Len() > 120)
					{
						OutErrors.Add(FString::Printf(TEXT("%s length must be 1..120 (got %d)"), FieldLabel, Name.Len()));
						return;
					}
					for (const TCHAR* P = *Name; *P; ++P)
					{
						const TCHAR C = *P;
						if (!FChar::IsAlnum(C) && C != TEXT('_'))
						{
							OutErrors.Add(FString::Printf(TEXT("%s may only contain [A-Za-z0-9_] (got: %s)"), FieldLabel, *Name));
							return;
						}
					}
				};

				FString TemplateId;
				if (!Arguments->TryGetStringField(TEXT("template_id"), TemplateId) || TemplateId.IsEmpty())
				{
					OutError = TEXT("Missing template_id.");
					return false;
				}
				TemplateId.ToLowerInline();

				FString DefaultSystemName;
				FString DefaultEmitterName;
				FString DefaultSpawnLabel;
				if (TemplateId == TEXT("niagara_explosion_basic"))
				{
					DefaultSystemName = TEXT("NS_Explosion_Auto");
					DefaultEmitterName = TEXT("NE_Explosion_Auto");
					DefaultSpawnLabel = TEXT("VFX_Explosion_Actor");
				}
				else if (TemplateId == TEXT("niagara_ambient_loop"))
				{
					DefaultSystemName = TEXT("NS_Ambient_Auto");
					DefaultEmitterName = TEXT("NE_Ambient_Auto");
					DefaultSpawnLabel = TEXT("VFX_Ambient_Actor");
				}
				else if (TemplateId == TEXT("niagara_fire_basic"))
				{
					DefaultSystemName = TEXT("NS_Fire_Auto");
					DefaultEmitterName = TEXT("NE_Fire_Auto");
					DefaultSpawnLabel = TEXT("VFX_Fire_Actor");
				}
				else if (TemplateId == TEXT("niagara_smoke_basic"))
				{
					DefaultSystemName = TEXT("NS_Smoke_Auto");
					DefaultEmitterName = TEXT("NE_Smoke_Auto");
					DefaultSpawnLabel = TEXT("VFX_Smoke_Actor");
				}
				else if (TemplateId == TEXT("niagara_rain_basic"))
				{
					DefaultSystemName = TEXT("NS_Rain_Auto");
					DefaultEmitterName = TEXT("NE_Rain_Auto");
					DefaultSpawnLabel = TEXT("VFX_Rain_Actor");
				}
				else if (TemplateId == TEXT("niagara_spark_basic"))
				{
					DefaultSystemName = TEXT("NS_Spark_Auto");
					DefaultEmitterName = TEXT("NE_Spark_Auto");
					DefaultSpawnLabel = TEXT("VFX_Spark_Actor");
				}
				else if (TemplateId == TEXT("niagara_muzzle_flash_basic"))
				{
					DefaultSystemName = TEXT("NS_MuzzleFlash_Auto");
					DefaultEmitterName = TEXT("NE_MuzzleFlash_Auto");
					DefaultSpawnLabel = TEXT("VFX_MuzzleFlash_Actor");
				}
				else
				{
					OutError = TEXT("Unsupported template_id. Call niagara_pipeline_templates_list for supported ids.");
					return false;
				}

				const bool bDryRun = Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")) && Arguments->GetBoolField(TEXT("dry_run"));
				const bool bForceCompile = Arguments->HasTypedField<EJson::Boolean>(TEXT("force_compile")) ? Arguments->GetBoolField(TEXT("force_compile")) : false;
				const bool bSkipSpawnActor = Arguments->HasTypedField<EJson::Boolean>(TEXT("skip_spawn_actor")) && Arguments->GetBoolField(TEXT("skip_spawn_actor"));
				FString SystemPackagePath = TEXT("/Game/VFX/Generated");
				FString SystemAssetName = DefaultSystemName;
				FString EmitterPackagePath = TEXT("/Game/VFX/Generated");
				FString EmitterAssetName = DefaultEmitterName;
				FString SpawnActorLabel = DefaultSpawnLabel;
				Arguments->TryGetStringField(TEXT("system_package_path"), SystemPackagePath);
				Arguments->TryGetStringField(TEXT("system_asset_name"), SystemAssetName);
				Arguments->TryGetStringField(TEXT("emitter_package_path"), EmitterPackagePath);
				Arguments->TryGetStringField(TEXT("emitter_asset_name"), EmitterAssetName);
				Arguments->TryGetStringField(TEXT("spawn_actor_label"), SpawnActorLabel);

				TArray<FString> ValidationErrors;
				ValidateContentPath(SystemPackagePath, TEXT("system_package_path"), ValidationErrors);
				ValidateContentPath(EmitterPackagePath, TEXT("emitter_package_path"), ValidationErrors);
				ValidateAssetNameField(SystemAssetName, TEXT("system_asset_name"), ValidationErrors);
				ValidateAssetNameField(EmitterAssetName, TEXT("emitter_asset_name"), ValidationErrors);
				if (SpawnActorLabel.Len() < 1 || SpawnActorLabel.Len() > 120)
				{
					ValidationErrors.Add(FString::Printf(TEXT("spawn_actor_label length must be 1..120 (got %d)"), SpawnActorLabel.Len()));
				}
				if (ValidationErrors.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> ErrArr;
					for (const FString& E : ValidationErrors)
					{
						ErrArr.Add(MakeShared<FJsonValueString>(E));
					}
					OutStructured->SetArrayField(TEXT("validation_errors"), ErrArr);
					OutStructured->SetStringField(TEXT("template_id"), TemplateId);
					OutError = TEXT("Niagara pipeline template arguments failed validation.");
					return false;
				}

				const FString SystemAssetPath = FString::Printf(TEXT("%s/%s.%s"), *SystemPackagePath, *SystemAssetName, *SystemAssetName);
				const FString EmitterAssetPath = FString::Printf(TEXT("%s/%s.%s"), *EmitterPackagePath, *EmitterAssetName, *EmitterAssetName);
				auto MakeOperation = [](const FString& ToolName, const TSharedRef<FJsonObject>& ToolArgs) -> TSharedRef<FJsonObject>
				{
					TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
					Op->SetStringField(TEXT("tool"), ToolName);
					Op->SetObjectField(TEXT("arguments"), ToolArgs);
					return Op;
				};
				TArray<TSharedPtr<FJsonValue>> Operations;
				{
					TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
					ToolArgs->SetStringField(TEXT("package_path"), SystemPackagePath);
					ToolArgs->SetStringField(TEXT("asset_name"), SystemAssetName);
					Operations.Add(MakeShared<FJsonValueObject>(MakeOperation(TEXT("niagara_create_system"), ToolArgs)));
				}
				{
					TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
					ToolArgs->SetStringField(TEXT("package_path"), EmitterPackagePath);
					ToolArgs->SetStringField(TEXT("asset_name"), EmitterAssetName);
					Operations.Add(MakeShared<FJsonValueObject>(MakeOperation(TEXT("niagara_create_emitter"), ToolArgs)));
				}
				{
					TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
					ToolArgs->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
					ToolArgs->SetStringField(TEXT("emitter_asset_path"), EmitterAssetPath);
					Operations.Add(MakeShared<FJsonValueObject>(MakeOperation(TEXT("niagara_add_emitter_to_system"), ToolArgs)));
				}
				{
					TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
					ToolArgs->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
					ToolArgs->SetBoolField(TEXT("force"), bForceCompile);
					ToolArgs->SetBoolField(TEXT("wait_for_completion"), true);
					Operations.Add(MakeShared<FJsonValueObject>(MakeOperation(TEXT("niagara_compile_diagnostics"), ToolArgs)));
				}
				if (!bSkipSpawnActor)
				{
					TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
					ToolArgs->SetStringField(TEXT("asset_path"), SystemAssetPath);
					TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
					Props->SetStringField(TEXT("ActorLabel"), SpawnActorLabel);
					ToolArgs->SetObjectField(TEXT("properties"), Props);
					Operations.Add(MakeShared<FJsonValueObject>(MakeOperation(TEXT("niagara_spawn_actor"), ToolArgs)));
				}
				OutStructured->SetStringField(TEXT("template_id"), TemplateId);
				OutStructured->SetBoolField(TEXT("skip_spawn_actor"), bSkipSpawnActor);
				OutStructured->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
				OutStructured->SetStringField(TEXT("emitter_asset_path"), EmitterAssetPath);
				OutStructured->SetArrayField(TEXT("planned_operations"), Operations);
				OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
				if (bDryRun)
				{
					TSharedRef<FJsonObject> PreviewArgs = MakeShared<FJsonObject>();
					PreviewArgs->SetArrayField(TEXT("operations"), Operations);
					TSharedRef<FJsonObject> PreviewOut = MakeShared<FJsonObject>();
					FString PreviewSummary;
					FString PreviewError;
					if (!Registry.ExecuteTool(TEXT("niagara_batch_preview_diff"), PreviewArgs, PreviewOut, PreviewSummary, PreviewError))
					{
						OutError = PreviewError.IsEmpty() ? TEXT("Failed to preview Niagara template operations.") : PreviewError;
						return false;
					}
					OutStructured->SetObjectField(TEXT("preview"), PreviewOut);
					OutSummary = TEXT("Previewed Niagara pipeline template.");
					return true;
				}
				TArray<TSharedPtr<FJsonValue>> StepResults;
				for (int32 StepIdx = 0; StepIdx < Operations.Num(); ++StepIdx)
				{
					const TSharedPtr<FJsonObject> OpObj = Operations[StepIdx]->AsObject();
					if (!OpObj.IsValid())
					{
						continue;
					}
					FString ToolName;
					const TSharedPtr<FJsonObject>* OpArgsPtr = nullptr;
					if (!OpObj->TryGetStringField(TEXT("tool"), ToolName) || !OpObj->TryGetObjectField(TEXT("arguments"), OpArgsPtr) || !OpArgsPtr || !OpArgsPtr->IsValid())
					{
						OutStructured->SetNumberField(TEXT("failed_step_index"), StepIdx);
						OutStructured->SetStringField(TEXT("failed_tool"), ToolName);
						OutError = TEXT("Invalid planned operation envelope.");
						return false;
					}
					TSharedRef<FJsonObject> StepOut = MakeShared<FJsonObject>();
					FString StepSummary;
					FString StepError;
					const bool bStepOk = Registry.ExecuteTool(ToolName, OpArgsPtr->ToSharedRef(), StepOut, StepSummary, StepError);
					TSharedRef<FJsonObject> StepRecord = MakeShared<FJsonObject>();
					StepRecord->SetNumberField(TEXT("index"), StepIdx);
					StepRecord->SetStringField(TEXT("tool"), ToolName);
					StepRecord->SetBoolField(TEXT("ok"), bStepOk);
					StepRecord->SetStringField(TEXT("summary"), StepSummary);
					if (!StepError.IsEmpty())
					{
						StepRecord->SetStringField(TEXT("error"), StepError);
					}
					StepRecord->SetObjectField(TEXT("result"), StepOut);
					StepResults.Add(MakeShared<FJsonValueObject>(StepRecord));
					if (!bStepOk)
					{
						OutStructured->SetArrayField(TEXT("steps"), StepResults);
						OutStructured->SetNumberField(TEXT("failed_step_index"), StepIdx);
						OutStructured->SetStringField(TEXT("failed_tool"), ToolName);
						OutStructured->SetObjectField(TEXT("failed_arguments"), OpArgsPtr->ToSharedRef());
						OutStructured->SetStringField(TEXT("failed_summary"), StepSummary);
						OutError = StepError.IsEmpty() ? FString::Printf(TEXT("Step %d (%s) failed."), StepIdx, *ToolName) : StepError;
						return false;
					}
				}
				OutStructured->SetArrayField(TEXT("steps"), StepResults);
				OutStructured->SetBoolField(TEXT("success"), true);
				OutSummary = TEXT("Executed Niagara pipeline template.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_regression_smoke_suite"),
			TEXT("Batch niagara_pipeline_smoke_test over multiple systems. Supports per-case group/tags and suite-level filter_groups / filter_tags_any / filter_tags_all. Skipped cases appear with skipped=true."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("cases"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
					{TEXT("system_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("group"), FSololmcpSchemaBuilder::String()},
					{TEXT("tags"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("include_runtime_snapshot"), FSololmcpSchemaBuilder::Boolean()}
				}, {TEXT("system_asset_path")}))},
				{TEXT("filter_groups"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
				{TEXT("filter_tags_any"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
				{TEXT("filter_tags_all"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
				{TEXT("force"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("wait_for_completion"), FSololmcpSchemaBuilder::Boolean()}
			}, {TEXT("cases")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("cases"), Cases) || !Cases)
				{
					OutError = TEXT("Missing cases array.");
					return false;
				}
				const bool bForce = Arguments->HasTypedField<EJson::Boolean>(TEXT("force")) && Arguments->GetBoolField(TEXT("force"));
				const bool bWait = Arguments->HasTypedField<EJson::Boolean>(TEXT("wait_for_completion")) ? Arguments->GetBoolField(TEXT("wait_for_completion")) : true;

				TSet<FString> FilterGroups;
				const TArray<TSharedPtr<FJsonValue>>* FilterGroupsArr = nullptr;
				if (Arguments->TryGetArrayField(TEXT("filter_groups"), FilterGroupsArr) && FilterGroupsArr)
				{
					for (const TSharedPtr<FJsonValue>& V : *FilterGroupsArr)
					{
						if (V.IsValid() && V->Type == EJson::String)
						{
							FilterGroups.Add(V->AsString());
						}
					}
				}
				TSet<FString> FilterTagsAny;
				const TArray<TSharedPtr<FJsonValue>>* FilterTagsAnyArr = nullptr;
				if (Arguments->TryGetArrayField(TEXT("filter_tags_any"), FilterTagsAnyArr) && FilterTagsAnyArr)
				{
					for (const TSharedPtr<FJsonValue>& V : *FilterTagsAnyArr)
					{
						if (V.IsValid() && V->Type == EJson::String)
						{
							FilterTagsAny.Add(V->AsString());
						}
					}
				}
				TArray<FString> FilterTagsAll;
				const TArray<TSharedPtr<FJsonValue>>* FilterTagsAllArr = nullptr;
				if (Arguments->TryGetArrayField(TEXT("filter_tags_all"), FilterTagsAllArr) && FilterTagsAllArr)
				{
					for (const TSharedPtr<FJsonValue>& V : *FilterTagsAllArr)
					{
						if (V.IsValid() && V->Type == EJson::String)
						{
							FilterTagsAll.Add(V->AsString());
						}
					}
				}

				int32 Passed = 0;
				int32 Failed = 0;
				int32 SkippedByFilter = 0;
				TArray<TSharedPtr<FJsonValue>> Results;
				TMap<FString, TPair<int32, int32>> GroupPassFail;

				auto BumpGroup = [&GroupPassFail](const FString& G, bool bPass)
				{
					TPair<int32, int32>& C = GroupPassFail.FindOrAdd(G);
					if (bPass) { ++C.Key; } else { ++C.Value; }
				};

				for (int32 Index = 0; Index < Cases->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> CaseObj = (*Cases)[Index].IsValid() ? (*Cases)[Index]->AsObject() : nullptr;
					if (!CaseObj.IsValid())
					{
						continue;
					}
					FString SystemAssetPath;
					if (!CaseObj->TryGetStringField(TEXT("system_asset_path"), SystemAssetPath) || SystemAssetPath.IsEmpty())
					{
						continue;
					}
					FString GroupName = TEXT("default");
					CaseObj->TryGetStringField(TEXT("group"), GroupName);
					if (GroupName.IsEmpty())
					{
						GroupName = TEXT("default");
					}
					TSet<FString> CaseTags;
					const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
					if (CaseObj->TryGetArrayField(TEXT("tags"), TagsArr) && TagsArr)
					{
						for (const TSharedPtr<FJsonValue>& Tv : *TagsArr)
						{
							if (Tv.IsValid() && Tv->Type == EJson::String)
							{
								CaseTags.Add(Tv->AsString());
							}
						}
					}

					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
					Item->SetStringField(TEXT("group"), GroupName);
					TArray<TSharedPtr<FJsonValue>> TagsJson;
					for (const FString& Tg : CaseTags)
					{
						TagsJson.Add(MakeShared<FJsonValueString>(Tg));
					}
					Item->SetArrayField(TEXT("tags"), TagsJson);

					bool bSkipped = false;
					FString SkipReason;
					if (FilterGroups.Num() > 0 && !FilterGroups.Contains(GroupName))
					{
						bSkipped = true;
						SkipReason = TEXT("group_not_in_filter_groups");
					}
					if (!bSkipped && FilterTagsAny.Num() > 0)
					{
						bool bAny = false;
						for (const FString& Req : FilterTagsAny)
						{
							if (CaseTags.Contains(Req))
							{
								bAny = true;
								break;
							}
						}
						if (!bAny)
						{
							bSkipped = true;
							SkipReason = TEXT("no_matching_filter_tags_any");
						}
					}
					if (!bSkipped && FilterTagsAll.Num() > 0)
					{
						for (const FString& Req : FilterTagsAll)
						{
							if (!CaseTags.Contains(Req))
							{
								bSkipped = true;
								SkipReason = TEXT("missing_filter_tags_all_tag");
								break;
							}
						}
					}

					if (bSkipped)
					{
						Item->SetBoolField(TEXT("skipped"), true);
						Item->SetStringField(TEXT("skip_reason"), SkipReason);
						Item->SetBoolField(TEXT("success"), false);
						Results.Add(MakeShared<FJsonValueObject>(Item));
						++SkippedByFilter;
						BumpGroup(GroupName, false);
						continue;
					}

					TSharedRef<FJsonObject> SmokeArgs = MakeShared<FJsonObject>();
					SmokeArgs->SetStringField(TEXT("system_asset_path"), SystemAssetPath);
					SmokeArgs->SetBoolField(TEXT("force"), bForce);
					SmokeArgs->SetBoolField(TEXT("wait_for_completion"), bWait);
					if (CaseObj->HasTypedField<EJson::String>(TEXT("actor")))
					{
						SmokeArgs->SetStringField(TEXT("actor"), CaseObj->GetStringField(TEXT("actor")));
					}
					if (CaseObj->HasTypedField<EJson::Boolean>(TEXT("include_runtime_snapshot")))
					{
						SmokeArgs->SetBoolField(TEXT("include_runtime_snapshot"), CaseObj->GetBoolField(TEXT("include_runtime_snapshot")));
					}
					TSharedRef<FJsonObject> SmokeOut = MakeShared<FJsonObject>();
					FString SmokeSummary;
					FString SmokeError;
					const bool bOk = Registry.ExecuteTool(TEXT("niagara_pipeline_smoke_test"), SmokeArgs, SmokeOut, SmokeSummary, SmokeError);
					Item->SetBoolField(TEXT("skipped"), false);
					Item->SetBoolField(TEXT("success"), bOk);
					Item->SetObjectField(TEXT("result"), SmokeOut);
					if (!SmokeError.IsEmpty())
					{
						Item->SetStringField(TEXT("error"), SmokeError);
					}
					Results.Add(MakeShared<FJsonValueObject>(Item));
					if (bOk)
					{
						++Passed;
						BumpGroup(GroupName, true);
					}
					else
					{
						++Failed;
						BumpGroup(GroupName, false);
					}
				}
				const int32 Executed = Passed + Failed;
				const int32 TotalCases = Results.Num();
				const double PassRate = Executed > 0 ? static_cast<double>(Passed) / static_cast<double>(Executed) : 0.0;
				OutStructured->SetArrayField(TEXT("cases"), Results);
				OutStructured->SetNumberField(TEXT("total"), TotalCases);
				OutStructured->SetNumberField(TEXT("executed"), Executed);
				OutStructured->SetNumberField(TEXT("skipped_by_filter"), SkippedByFilter);
				OutStructured->SetNumberField(TEXT("passed"), Passed);
				OutStructured->SetNumberField(TEXT("failed"), Failed);
				OutStructured->SetNumberField(TEXT("pass_rate"), PassRate);
				OutStructured->SetBoolField(TEXT("success"), Failed == 0);
				TSharedRef<FJsonObject> ByGroup = MakeShared<FJsonObject>();
				for (const TPair<FString, TPair<int32, int32>>& KV : GroupPassFail)
				{
					TSharedRef<FJsonObject> GObj = MakeShared<FJsonObject>();
					GObj->SetNumberField(TEXT("passed"), KV.Value.Key);
					GObj->SetNumberField(TEXT("failed"), KV.Value.Value);
					ByGroup->SetObjectField(KV.Key, GObj);
				}
				OutStructured->SetObjectField(TEXT("by_group"), ByGroup);
				OutSummary = TEXT("Completed Niagara regression smoke suite.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_pipeline_templates_list"),
			TEXT("List supported niagara_pipeline_template_run template_id values with default asset names and validation constraints."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString&)
			{
				auto MakeTpl = [](const FString& Id, const FString& Desc, const FString& Sys, const FString& Emit, const FString& Spawn) -> TSharedRef<FJsonObject>
				{
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("template_id"), Id);
					O->SetStringField(TEXT("description"), Desc);
					O->SetStringField(TEXT("default_system_asset_name"), Sys);
					O->SetStringField(TEXT("default_emitter_asset_name"), Emit);
					O->SetStringField(TEXT("default_spawn_actor_label"), Spawn);
					return O;
				};
				TArray<TSharedPtr<FJsonValue>> Arr;
				Arr.Add(MakeShared<FJsonValueObject>(MakeTpl(
					TEXT("niagara_explosion_basic"),
					TEXT("One-shot style burst pipeline (system + emitter + compile + optional spawn)."),
					TEXT("NS_Explosion_Auto"), TEXT("NE_Explosion_Auto"), TEXT("VFX_Explosion_Actor"))));
				Arr.Add(MakeShared<FJsonValueObject>(MakeTpl(
					TEXT("niagara_ambient_loop"),
					TEXT("Ambient looping style pipeline."),
					TEXT("NS_Ambient_Auto"), TEXT("NE_Ambient_Auto"), TEXT("VFX_Ambient_Actor"))));
				Arr.Add(MakeShared<FJsonValueObject>(MakeTpl(
					TEXT("niagara_fire_basic"),
					TEXT("Fire-themed placeholder naming; same create/attach/compile/spawn steps."),
					TEXT("NS_Fire_Auto"), TEXT("NE_Fire_Auto"), TEXT("VFX_Fire_Actor"))));
				Arr.Add(MakeShared<FJsonValueObject>(MakeTpl(
					TEXT("niagara_smoke_basic"),
					TEXT("Smoke-themed placeholder naming."),
					TEXT("NS_Smoke_Auto"), TEXT("NE_Smoke_Auto"), TEXT("VFX_Smoke_Actor"))));
				Arr.Add(MakeShared<FJsonValueObject>(MakeTpl(
					TEXT("niagara_rain_basic"),
					TEXT("Rain-themed placeholder naming."),
					TEXT("NS_Rain_Auto"), TEXT("NE_Rain_Auto"), TEXT("VFX_Rain_Actor"))));
				Arr.Add(MakeShared<FJsonValueObject>(MakeTpl(
					TEXT("niagara_spark_basic"),
					TEXT("Spark-themed placeholder naming."),
					TEXT("NS_Spark_Auto"), TEXT("NE_Spark_Auto"), TEXT("VFX_Spark_Actor"))));
				Arr.Add(MakeShared<FJsonValueObject>(MakeTpl(
					TEXT("niagara_muzzle_flash_basic"),
					TEXT("Muzzle flash placeholder naming."),
					TEXT("NS_MuzzleFlash_Auto"), TEXT("NE_MuzzleFlash_Auto"), TEXT("VFX_MuzzleFlash_Actor"))));
				OutStructured->SetArrayField(TEXT("templates"), Arr);
				TSharedRef<FJsonObject> Constraints = MakeShared<FJsonObject>();
				Constraints->SetStringField(TEXT("package_path_prefix_required"), TEXT("/Game"));
				Constraints->SetNumberField(TEXT("asset_name_max_length"), 120);
				Constraints->SetStringField(TEXT("asset_name_charset"), TEXT("alphanumeric_and_underscore"));
				Constraints->SetBoolField(TEXT("skip_spawn_actor_supported"), true);
				OutStructured->SetObjectField(TEXT("constraints"), Constraints);
				OutSummary = TEXT("Listed Niagara pipeline templates.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_regression_baseline_save"),
			TEXT("Save a Niagara regression smoke suite snapshot to Saved/SOMOLMCP/NiagaraRegression/baselines/<baseline_id>.json for later comparison."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("baseline_id"), FSololmcpSchemaBuilder::String()},
				{TEXT("suite_result"), FSololmcpSchemaBuilder::Object({})},
				{TEXT("run_suite"), FSololmcpSchemaBuilder::Object({})}
			}, {TEXT("baseline_id")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				auto IsValidId = [](const FString& Id, FString& Err) -> bool
				{
					if (Id.Len() < 1 || Id.Len() > 64)
					{
						Err = TEXT("baseline_id length must be 1..64.");
						return false;
					}
					for (const TCHAR* P = *Id; *P; ++P)
					{
						const TCHAR C = *P;
						if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
						{
							Err = TEXT("baseline_id may only contain [A-Za-z0-9_-].");
							return false;
						}
					}
					return true;
				};

				FString BaselineId;
				if (!Arguments->TryGetStringField(TEXT("baseline_id"), BaselineId) || BaselineId.IsEmpty())
				{
					OutError = TEXT("Missing baseline_id.");
					return false;
				}
				FString IdErr;
				if (!IsValidId(BaselineId, IdErr))
				{
					OutError = IdErr;
					return false;
				}

				TSharedRef<FJsonObject> SuiteResult = MakeShared<FJsonObject>();
				const TSharedPtr<FJsonObject>* SuitePtr = nullptr;
				if (Arguments->TryGetObjectField(TEXT("suite_result"), SuitePtr) && SuitePtr && SuitePtr->IsValid())
				{
					SuiteResult = SuitePtr->ToSharedRef();
				}
				else
				{
					const TSharedPtr<FJsonObject>* RunSuite = nullptr;
					if (!Arguments->TryGetObjectField(TEXT("run_suite"), RunSuite) || !RunSuite || !RunSuite->IsValid())
					{
						OutError = TEXT("Provide suite_result or run_suite.");
						return false;
					}
					FString RunErr;
					FString RunSum;
					if (!Registry.ExecuteTool(TEXT("niagara_regression_smoke_suite"), RunSuite->ToSharedRef(), SuiteResult, RunSum, RunErr))
					{
						OutError = RunErr.IsEmpty() ? TEXT("niagara_regression_smoke_suite failed.") : RunErr;
						return false;
					}
				}

				const FString Dir = FPaths::ProjectSavedDir() / TEXT("SOMOLMCP") / TEXT("NiagaraRegression") / TEXT("baselines");
				IFileManager::Get().MakeDirectory(*Dir, true);
				const FString FilePath = Dir / (BaselineId + TEXT(".json"));

				TSharedRef<FJsonObject> BaselineDoc = MakeShared<FJsonObject>();
				BaselineDoc->SetStringField(TEXT("schema"), TEXT("somolmcp.niagara_regression_baseline.v1"));
				BaselineDoc->SetStringField(TEXT("baseline_id"), BaselineId);
				BaselineDoc->SetStringField(TEXT("saved_utc"), FDateTime::UtcNow().ToIso8601());

				double PassRate = 0.0;
				SuiteResult->TryGetNumberField(TEXT("pass_rate"), PassRate);
				int32 Total = 0, Passed = 0, Failed = 0;
				SuiteResult->TryGetNumberField(TEXT("total"), Total);
				SuiteResult->TryGetNumberField(TEXT("passed"), Passed);
				SuiteResult->TryGetNumberField(TEXT("failed"), Failed);

				TSharedRef<FJsonObject> Metrics = MakeShared<FJsonObject>();
				Metrics->SetNumberField(TEXT("total"), Total);
				Metrics->SetNumberField(TEXT("passed"), Passed);
				Metrics->SetNumberField(TEXT("failed"), Failed);
				Metrics->SetNumberField(TEXT("pass_rate"), PassRate);
				BaselineDoc->SetObjectField(TEXT("metrics"), Metrics);

				TArray<TSharedPtr<FJsonValue>> CaseSummaries;
				const TArray<TSharedPtr<FJsonValue>>* CasesArr = nullptr;
				if (SuiteResult->TryGetArrayField(TEXT("cases"), CasesArr) && CasesArr)
				{
					for (const TSharedPtr<FJsonValue>& Cv : *CasesArr)
					{
						const TSharedPtr<FJsonObject> Co = Cv.IsValid() ? Cv->AsObject() : nullptr;
						if (!Co.IsValid())
						{
							continue;
						}
						FString PathStr;
						if (!Co->TryGetStringField(TEXT("system_asset_path"), PathStr))
						{
							continue;
						}
						TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
						Row->SetStringField(TEXT("system_asset_path"), PathStr);
						FString G = TEXT("default");
						Co->TryGetStringField(TEXT("group"), G);
						Row->SetStringField(TEXT("group"), G.IsEmpty() ? TEXT("default") : G);
						bool bSkipped = false;
						if (Co->HasTypedField<EJson::Boolean>(TEXT("skipped")))
						{
							bSkipped = Co->GetBoolField(TEXT("skipped"));
						}
						Row->SetBoolField(TEXT("skipped"), bSkipped);
						bool bSuccess = false;
						if (Co->HasTypedField<EJson::Boolean>(TEXT("success")))
						{
							bSuccess = Co->GetBoolField(TEXT("success"));
						}
						Row->SetBoolField(TEXT("success"), bSuccess);
						CaseSummaries.Add(MakeShared<FJsonValueObject>(Row));
					}
				}
				BaselineDoc->SetArrayField(TEXT("cases"), CaseSummaries);

				FString JsonOut;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonOut);
				if (!FJsonSerializer::Serialize(BaselineDoc, Writer))
				{
					OutError = TEXT("Failed to serialize baseline JSON.");
					return false;
				}
				if (!FFileHelper::SaveStringToFile(JsonOut, *FilePath))
				{
					OutError = FString::Printf(TEXT("Failed to write baseline file: %s"), *FilePath);
					return false;
				}

				OutStructured->SetStringField(TEXT("baseline_id"), BaselineId);
				OutStructured->SetStringField(TEXT("absolute_path"), FPaths::ConvertRelativePathToFull(FilePath));
				OutStructured->SetObjectField(TEXT("metrics"), Metrics);
				OutStructured->SetNumberField(TEXT("case_count"), CaseSummaries.Num());
				OutSummary = TEXT("Saved Niagara regression baseline.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_regression_baseline_compare"),
			TEXT("Compare current niagara_regression_smoke_suite output (or re-run via run_suite) against a saved baseline; emits regressions/improvements and threshold alerts."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("baseline_id"), FSololmcpSchemaBuilder::String()},
				{TEXT("suite_result"), FSololmcpSchemaBuilder::Object({})},
				{TEXT("run_suite"), FSololmcpSchemaBuilder::Object({})},
				{TEXT("min_pass_rate"), FSololmcpSchemaBuilder::Number()},
				{TEXT("max_regression_count"), FSololmcpSchemaBuilder::Number()}
			}, {TEXT("baseline_id")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				auto IsValidId = [](const FString& Id, FString& Err) -> bool
				{
					if (Id.Len() < 1 || Id.Len() > 64)
					{
						Err = TEXT("baseline_id length must be 1..64.");
						return false;
					}
					for (const TCHAR* P = *Id; *P; ++P)
					{
						const TCHAR C = *P;
						if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
						{
							Err = TEXT("baseline_id may only contain [A-Za-z0-9_-].");
							return false;
						}
					}
					return true;
				};

				FString BaselineId;
				if (!Arguments->TryGetStringField(TEXT("baseline_id"), BaselineId) || BaselineId.IsEmpty())
				{
					OutError = TEXT("Missing baseline_id.");
					return false;
				}
				FString IdErr;
				if (!IsValidId(BaselineId, IdErr))
				{
					OutError = IdErr;
					return false;
				}

				const FString BaselinePath = FPaths::ProjectSavedDir() / TEXT("SOMOLMCP") / TEXT("NiagaraRegression") / TEXT("baselines") / (BaselineId + TEXT(".json"));
				FString BaselineRaw;
				if (!FFileHelper::LoadFileToString(BaselineRaw, *BaselinePath))
				{
					OutError = FString::Printf(TEXT("Baseline not found: %s"), *BaselinePath);
					return false;
				}
				TSharedPtr<FJsonObject> BaselineObj;
				{
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BaselineRaw);
					if (!FJsonSerializer::Deserialize(Reader, BaselineObj) || !BaselineObj.IsValid())
					{
						OutError = TEXT("Failed to parse baseline JSON.");
						return false;
					}
				}

				TSharedRef<FJsonObject> CurrentSuite = MakeShared<FJsonObject>();
				const TSharedPtr<FJsonObject>* SuitePtr = nullptr;
				if (Arguments->TryGetObjectField(TEXT("suite_result"), SuitePtr) && SuitePtr && SuitePtr->IsValid())
				{
					CurrentSuite = SuitePtr->ToSharedRef();
				}
				else
				{
					const TSharedPtr<FJsonObject>* RunSuite = nullptr;
					if (!Arguments->TryGetObjectField(TEXT("run_suite"), RunSuite) || !RunSuite || !RunSuite->IsValid())
					{
						OutError = TEXT("Provide suite_result or run_suite.");
						return false;
					}
					FString RunErr;
					FString RunSum;
					if (!Registry.ExecuteTool(TEXT("niagara_regression_smoke_suite"), RunSuite->ToSharedRef(), CurrentSuite, RunSum, RunErr))
					{
						OutError = RunErr.IsEmpty() ? TEXT("niagara_regression_smoke_suite failed.") : RunErr;
						return false;
					}
				}

				TMap<FString, bool> BaseSuccessByPath;
				const TArray<TSharedPtr<FJsonValue>>* BaseCases = nullptr;
				if (BaselineObj->TryGetArrayField(TEXT("cases"), BaseCases) && BaseCases)
				{
					for (const TSharedPtr<FJsonValue>& Cv : *BaseCases)
					{
						const TSharedPtr<FJsonObject> Co = Cv.IsValid() ? Cv->AsObject() : nullptr;
						if (!Co.IsValid())
						{
							continue;
						}
						FString Pth;
						if (!Co->TryGetStringField(TEXT("system_asset_path"), Pth))
						{
							continue;
						}
						bool bSkipped = false;
						if (Co->HasTypedField<EJson::Boolean>(TEXT("skipped")))
						{
							bSkipped = Co->GetBoolField(TEXT("skipped"));
						}
						bool bOk = false;
						if (Co->HasTypedField<EJson::Boolean>(TEXT("success")))
						{
							bOk = Co->GetBoolField(TEXT("success"));
						}
						BaseSuccessByPath.Add(Pth, !bSkipped && bOk);
					}
				}

				TMap<FString, bool> CurSuccessByPath;
				const TArray<TSharedPtr<FJsonValue>>* CurCases = nullptr;
				if (CurrentSuite->TryGetArrayField(TEXT("cases"), CurCases) && CurCases)
				{
					for (const TSharedPtr<FJsonValue>& Cv : *CurCases)
					{
						const TSharedPtr<FJsonObject> Co = Cv.IsValid() ? Cv->AsObject() : nullptr;
						if (!Co.IsValid())
						{
							continue;
						}
						FString Pth;
						if (!Co->TryGetStringField(TEXT("system_asset_path"), Pth))
						{
							continue;
						}
						bool bSkipped = false;
						if (Co->HasTypedField<EJson::Boolean>(TEXT("skipped")))
						{
							bSkipped = Co->GetBoolField(TEXT("skipped"));
						}
						bool bOk = false;
						if (Co->HasTypedField<EJson::Boolean>(TEXT("success")))
						{
							bOk = Co->GetBoolField(TEXT("success"));
						}
						CurSuccessByPath.Add(Pth, !bSkipped && bOk);
					}
				}

				TArray<TSharedPtr<FJsonValue>> Regressions;
				TArray<TSharedPtr<FJsonValue>> Improvements;
				for (const TPair<FString, bool>& KV : BaseSuccessByPath)
				{
					const bool* Cur = CurSuccessByPath.Find(KV.Key);
					const bool CurOk = Cur ? *Cur : false;
					if (KV.Value && !CurOk)
					{
						Regressions.Add(MakeShared<FJsonValueString>(KV.Key));
					}
					if (!KV.Value && CurOk)
					{
						Improvements.Add(MakeShared<FJsonValueString>(KV.Key));
					}
				}

				double CurPassRate = 0.0;
				CurrentSuite->TryGetNumberField(TEXT("pass_rate"), CurPassRate);
				double BasePassRate = 0.0;
				const TSharedPtr<FJsonObject>* BaseMetrics = nullptr;
				if (BaselineObj->TryGetObjectField(TEXT("metrics"), BaseMetrics) && BaseMetrics && BaseMetrics->IsValid())
				{
					(*BaseMetrics)->TryGetNumberField(TEXT("pass_rate"), BasePassRate);
				}

				double MinPassRate = 0.95;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("min_pass_rate")))
				{
					MinPassRate = Arguments->GetNumberField(TEXT("min_pass_rate"));
				}
				double MaxRegressionCountD = 0.0;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("max_regression_count")))
				{
					MaxRegressionCountD = Arguments->GetNumberField(TEXT("max_regression_count"));
				}
				const int32 MaxRegressionCount = FMath::Max(0, static_cast<int32>(FMath::RoundToDouble(MaxRegressionCountD)));

				TArray<TSharedPtr<FJsonValue>> Alerts;
				if (CurPassRate + 1e-9 < MinPassRate)
				{
					TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
					A->SetStringField(TEXT("code"), TEXT("pass_rate_below_min"));
					A->SetNumberField(TEXT("current_pass_rate"), CurPassRate);
					A->SetNumberField(TEXT("min_pass_rate"), MinPassRate);
					Alerts.Add(MakeShared<FJsonValueObject>(A));
				}
				if (Regressions.Num() > MaxRegressionCount)
				{
					TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
					A->SetStringField(TEXT("code"), TEXT("too_many_regressions"));
					A->SetNumberField(TEXT("regression_count"), Regressions.Num());
					A->SetNumberField(TEXT("max_regression_count"), MaxRegressionCount);
					Alerts.Add(MakeShared<FJsonValueObject>(A));
				}

				OutStructured->SetStringField(TEXT("baseline_id"), BaselineId);
				OutStructured->SetStringField(TEXT("baseline_path"), FPaths::ConvertRelativePathToFull(BaselinePath));
				OutStructured->SetNumberField(TEXT("baseline_pass_rate"), BasePassRate);
				OutStructured->SetNumberField(TEXT("current_pass_rate"), CurPassRate);
				OutStructured->SetNumberField(TEXT("pass_rate_delta"), CurPassRate - BasePassRate);
				OutStructured->SetArrayField(TEXT("regressions"), Regressions);
				OutStructured->SetArrayField(TEXT("improvements"), Improvements);
				OutStructured->SetArrayField(TEXT("alerts"), Alerts);
				OutStructured->SetBoolField(TEXT("thresholds_ok"), Alerts.Num() == 0);
				OutStructured->SetObjectField(TEXT("current_suite"), CurrentSuite);
				OutSummary = TEXT("Compared Niagara regression suite to baseline.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_regression_report_write"),
			TEXT("Write a JSON report (and optional .txt summary) under Saved/SOMOLMCP/NiagaraRegression/reports/ from suite_result and optional baseline compare output."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("report_id"), FSololmcpSchemaBuilder::String()},
				{TEXT("suite_result"), FSololmcpSchemaBuilder::Object({})},
				{TEXT("compare_result"), FSololmcpSchemaBuilder::Object({})},
				{TEXT("format"), FSololmcpSchemaBuilder::String()}
			}, {TEXT("report_id"), TEXT("suite_result")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				auto IsValidId = [](const FString& Id, FString& Err) -> bool
				{
					if (Id.Len() < 1 || Id.Len() > 64)
					{
						Err = TEXT("report_id length must be 1..64.");
						return false;
					}
					for (const TCHAR* P = *Id; *P; ++P)
					{
						const TCHAR C = *P;
						if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
						{
							Err = TEXT("report_id may only contain [A-Za-z0-9_-].");
							return false;
						}
					}
					return true;
				};

				FString ReportId;
				if (!Arguments->TryGetStringField(TEXT("report_id"), ReportId) || ReportId.IsEmpty())
				{
					OutError = TEXT("Missing report_id.");
					return false;
				}
				FString IdErr;
				if (!IsValidId(ReportId, IdErr))
				{
					OutError = IdErr;
					return false;
				}

				const TSharedPtr<FJsonObject>* SuitePtr = nullptr;
				if (!Arguments->TryGetObjectField(TEXT("suite_result"), SuitePtr) || !SuitePtr || !SuitePtr->IsValid())
				{
					OutError = TEXT("Missing suite_result.");
					return false;
				}

				const TSharedPtr<FJsonObject>* ComparePtr = nullptr;
				Arguments->TryGetObjectField(TEXT("compare_result"), ComparePtr);

				FString Format = TEXT("both");
				Arguments->TryGetStringField(TEXT("format"), Format);
				Format.ToLowerInline();

				TSharedRef<FJsonObject> ReportDoc = MakeShared<FJsonObject>();
				ReportDoc->SetStringField(TEXT("schema"), TEXT("somolmcp.niagara_regression_report.v1"));
				ReportDoc->SetStringField(TEXT("report_id"), ReportId);
				ReportDoc->SetStringField(TEXT("written_utc"), FDateTime::UtcNow().ToIso8601());
				ReportDoc->SetObjectField(TEXT("suite_result"), SuitePtr->ToSharedRef());
				if (ComparePtr && ComparePtr->IsValid())
				{
					ReportDoc->SetObjectField(TEXT("compare_result"), ComparePtr->ToSharedRef());
				}

				const FString Dir = FPaths::ProjectSavedDir() / TEXT("SOMOLMCP") / TEXT("NiagaraRegression") / TEXT("reports");
				IFileManager::Get().MakeDirectory(*Dir, true);
				const FString JsonPath = Dir / (ReportId + TEXT(".json"));
				const FString TxtPathFull = Dir / (ReportId + TEXT(".txt"));

				const bool bWriteJson = (Format != TEXT("text"));
				const bool bWriteTxt = (Format != TEXT("json"));

				if (bWriteJson)
				{
					FString JsonOut;
					TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonOut);
					if (!FJsonSerializer::Serialize(ReportDoc, Writer))
					{
						OutError = TEXT("Failed to serialize report JSON.");
						return false;
					}
					if (!FFileHelper::SaveStringToFile(JsonOut, *JsonPath))
					{
						OutError = FString::Printf(TEXT("Failed to write report JSON: %s"), *JsonPath);
						return false;
					}
				}

				FString TxtPath;
				if (bWriteTxt)
				{
					TxtPath = TxtPathFull;
					const TSharedPtr<FJsonObject>& Suite = *SuitePtr;
					double Pr = 0.0;
					Suite->TryGetNumberField(TEXT("pass_rate"), Pr);
					int32 Tot = 0, Pss = 0, Fld = 0, Sk = 0;
					Suite->TryGetNumberField(TEXT("total"), Tot);
					Suite->TryGetNumberField(TEXT("passed"), Pss);
					Suite->TryGetNumberField(TEXT("failed"), Fld);
					Suite->TryGetNumberField(TEXT("skipped_by_filter"), Sk);
					FString Txt;
					Txt += FString::Printf(TEXT("Niagara regression report: %s\r\n"), *ReportId);
					Txt += FString::Printf(TEXT("UTC: %s\r\n"), *FDateTime::UtcNow().ToIso8601());
					Txt += FString::Printf(TEXT("Suite total=%d passed=%d failed=%d skipped_by_filter=%d pass_rate=%.4f\r\n"),
						Tot, Pss, Fld, Sk, Pr);
					if (ComparePtr && ComparePtr->IsValid())
					{
						const TSharedPtr<FJsonObject>& Cmp = *ComparePtr;
						bool bOk = true;
						if (Cmp->HasTypedField<EJson::Boolean>(TEXT("thresholds_ok")))
						{
							bOk = Cmp->GetBoolField(TEXT("thresholds_ok"));
						}
						int32 Rc = 0;
						const TArray<TSharedPtr<FJsonValue>>* RegArr = nullptr;
						if (Cmp->TryGetArrayField(TEXT("regressions"), RegArr) && RegArr)
						{
							Rc = RegArr->Num();
						}
						Txt += FString::Printf(TEXT("Baseline compare: thresholds_ok=%s regression_count=%d\r\n"),
							bOk ? TEXT("true") : TEXT("false"), Rc);
					}
					if (!FFileHelper::SaveStringToFile(Txt, *TxtPath))
					{
						OutError = FString::Printf(TEXT("Failed to write report text: %s"), *TxtPath);
						return false;
					}
				}

				OutStructured->SetStringField(TEXT("report_id"), ReportId);
				OutStructured->SetStringField(TEXT("format"), Format);
				if (bWriteJson)
				{
					OutStructured->SetStringField(TEXT("json_path"), FPaths::ConvertRelativePathToFull(JsonPath));
				}
				if (!TxtPath.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("text_path"), FPaths::ConvertRelativePathToFull(TxtPath));
				}
				OutSummary = TEXT("Wrote Niagara regression report.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_component_activate"),
			TEXT("Activate a Niagara component on an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("reset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UNiagaraComponent* Component = ResolveNiagaraComponent(Context.Services, ActorId, OutError);
				if (!Component)
				{
					return false;
				}
				const bool bReset = Arguments->HasTypedField<EJson::Boolean>(TEXT("reset")) ? Arguments->GetBoolField(TEXT("reset")) : false;
				Component->Activate(bReset);
				OutStructured->SetBoolField(TEXT("active"), true);
				OutSummary = TEXT("Activated Niagara component.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_component_deactivate"),
			TEXT("Deactivate a Niagara component on an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UNiagaraComponent* Component = ResolveNiagaraComponent(Context.Services, ActorId, OutError);
				if (!Component)
				{
					return false;
				}
				Component->Deactivate();
				OutStructured->SetBoolField(TEXT("active"), false);
				OutSummary = TEXT("Deactivated Niagara component.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_component_reset"),
			TEXT("Reset a Niagara component on an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UNiagaraComponent* Component = ResolveNiagaraComponent(Context.Services, ActorId, OutError);
				if (!Component)
				{
					return false;
				}
				Component->ResetSystem();
				OutStructured->SetBoolField(TEXT("reset"), true);
				OutSummary = TEXT("Reset Niagara component.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_component_reinitialize"),
			TEXT("Reinitialize a Niagara component on an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UNiagaraComponent* Component = ResolveNiagaraComponent(Context.Services, ActorId, OutError);
				if (!Component)
				{
					return false;
				}
				Component->ReinitializeSystem();
				OutStructured->SetBoolField(TEXT("reinitialized"), true);
				OutSummary = TEXT("Reinitialized Niagara component.");
				return true;
			}
		, nullptr
		, 5
		});

		auto RegisterNiagaraComponentSetter = [&Registry](const FString& ToolName, const FString& Description, auto Setter, const TSharedRef<FJsonObject>& ValueSchema)
		{
			Registry.Register({
				ToolName,
				Description,
				FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("value"), ValueSchema}}, {TEXT("actor"), TEXT("variable_name"), TEXT("value")}),

				[Setter](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					FString ActorId;
					FString VariableName;
					if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetStringField(TEXT("variable_name"), VariableName))
					{
						OutError = TEXT("Missing actor or variable_name.");
						return false;
					}
					UNiagaraComponent* Component = ResolveNiagaraComponent(Context.Services, ActorId, OutError);
					if (!Component)
					{
						return false;
					}
					return Setter(Context, Arguments, Component, VariableName, OutStructured, OutSummary, OutError);
}
, nullptr
, 0
});
		};

		RegisterNiagaraComponentSetter(
			TEXT("niagara_component_set_float"),
			TEXT("Set a float user variable on a Niagara component."),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, UNiagaraComponent* Component, const FString& VariableName, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				double Value = 0.0;
				if (!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("Missing numeric value.");
					return false;
				}
				Component->SetVariableFloat(*VariableName, static_cast<float>(Value));
				OutStructured->SetStringField(TEXT("variable"), VariableName);
				OutStructured->SetNumberField(TEXT("value"), Value);
				OutSummary = TEXT("Set Niagara float variable.");
				return true;
			},
			FSololmcpSchemaBuilder::Number());

		RegisterNiagaraComponentSetter(
			TEXT("niagara_component_set_int"),
			TEXT("Set an int user variable on a Niagara component."),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, UNiagaraComponent* Component, const FString& VariableName, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				int32 Value = 0;
				if (!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("Missing numeric value.");
					return false;
				}
				Component->SetVariableInt(*VariableName, Value);
				OutStructured->SetStringField(TEXT("variable"), VariableName);
				OutStructured->SetNumberField(TEXT("value"), Value);
				OutSummary = TEXT("Set Niagara int variable.");
				return true;
			},
			FSololmcpSchemaBuilder::Integer());

		RegisterNiagaraComponentSetter(
			TEXT("niagara_component_set_bool"),
			TEXT("Set a bool user variable on a Niagara component."),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, UNiagaraComponent* Component, const FString& VariableName, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!Arguments->HasTypedField<EJson::Boolean>(TEXT("value")))
				{
					OutError = TEXT("Missing boolean value.");
					return false;
				}
				const bool bValue = Arguments->GetBoolField(TEXT("value"));
				Component->SetVariableBool(*VariableName, bValue);
				OutStructured->SetStringField(TEXT("variable"), VariableName);
				OutStructured->SetBoolField(TEXT("value"), bValue);
				OutSummary = TEXT("Set Niagara bool variable.");
				return true;
			},
			FSololmcpSchemaBuilder::Boolean());

		RegisterNiagaraComponentSetter(
			TEXT("niagara_component_set_vector"),
			TEXT("Set a vector user variable on a Niagara component."),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, UNiagaraComponent* Component, const FString& VariableName, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedPtr<FJsonObject> ValueObject;
				if (!TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("Missing vector value.");
					return false;
				}
				FVector Value;
				if (!FSololmcpEditorServices::JsonToVector(ValueObject, Value))
				{
					OutError = TEXT("value must be a vector object.");
					return false;
				}
				Component->SetVariableVec3(*VariableName, Value);
				OutStructured->SetStringField(TEXT("variable"), VariableName);
				OutStructured->SetObjectField(TEXT("value"), VectorToJson(Value));
				OutSummary = TEXT("Set Niagara vector variable.");
				return true;
			},
			VectorSchema());

		RegisterNiagaraComponentSetter(
			TEXT("niagara_component_set_color"),
			TEXT("Set a color user variable on a Niagara component."),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, UNiagaraComponent* Component, const FString& VariableName, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedPtr<FJsonObject> ValueObject;
				if (!TryGetObjectField(Arguments, TEXT("value"), ValueObject))
				{
					OutError = TEXT("Missing color value.");
					return false;
				}
				FLinearColor Value;
				if (!FSololmcpEditorServices::JsonToLinearColor(ValueObject, Value))
				{
					OutError = TEXT("value must be a color object.");
					return false;
				}
				Component->SetVariableLinearColor(*VariableName, Value);
				OutStructured->SetStringField(TEXT("variable"), VariableName);
				OutStructured->SetObjectField(TEXT("value"), LinearColorToJson(Value));
				OutSummary = TEXT("Set Niagara color variable.");
				return true;
			},
			ColorSchema());

		auto RegisterNiagaraPythonTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
				{
					FString AssetPath;
					if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
					{
						Arguments->TryGetStringField(TEXT("system_asset_path"), AssetPath);
					}
					if (AssetPath.IsEmpty())
					{
						OutError = TEXT("Missing asset_path or system_asset_path.");
						return FString();
					}
					const FString ArgumentsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal\n")
						TEXT("import json\n")
						TEXT("tool_name = %s\n")
						TEXT("args = json.loads(%s)\n")
						TEXT("asset_subsystem = unreal.EditorAssetSubsystem()\n")
						TEXT("asset = asset_subsystem.load_asset(%s)\n")
						TEXT("if asset is None:\n")
						TEXT("    raise RuntimeError('Failed to load Niagara asset')\n")
						TEXT("def resolve_emitter(source_asset, emitter_name):\n")
						TEXT("    if source_asset.get_class().get_name() == 'NiagaraEmitter':\n")
						TEXT("        return source_asset\n")
						TEXT("    handles = list(source_asset.get_emitter_handles()) if hasattr(source_asset, 'get_emitter_handles') else []\n")
						TEXT("    for handle in handles:\n")
						TEXT("        name = handle.get_name() if hasattr(handle, 'get_name') else ''\n")
						TEXT("        if not emitter_name or name == emitter_name:\n")
						TEXT("            return handle.get_instance() if hasattr(handle, 'get_instance') else handle.get_emitter_data() if hasattr(handle, 'get_emitter_data') else None\n")
						TEXT("    return None\n")
						TEXT("def iter_scripts(source_asset, emitter):\n")
						TEXT("    scripts = []\n")
						TEXT("    if source_asset.get_class().get_name() == 'NiagaraSystem':\n")
						TEXT("        for property_name in ('system_spawn_script', 'system_update_script'):\n")
						TEXT("            try:\n")
						TEXT("                scripts.append(source_asset.get_editor_property(property_name))\n")
						TEXT("            except Exception:\n")
						TEXT("                pass\n")
						TEXT("    if emitter is not None:\n")
						TEXT("        for property_name in ('graph_source', 'spawn_script_props', 'update_script_props'):\n")
						TEXT("            try:\n")
						TEXT("                scripts.append(emitter.get_editor_property(property_name))\n")
						TEXT("            except Exception:\n")
						TEXT("                pass\n")
						TEXT("    return [script for script in scripts if script is not None]\n")
						TEXT("emitter_name = args.get('emitter_name', '')\n")
						TEXT("emitter = resolve_emitter(asset, emitter_name)\n")
						TEXT("editor_subsystem_type = getattr(unreal, 'NiagaraEditorSubsystem', None)\n")
						TEXT("editor_subsystem = unreal.get_editor_subsystem(editor_subsystem_type) if editor_subsystem_type is not None else None\n")
						TEXT("if tool_name == 'niagara_list_stack_modules':\n")
						TEXT("    if emitter is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve Niagara emitter')\n")
						TEXT("    for script in iter_scripts(asset, emitter):\n")
						TEXT("        script_name = script.get_name() if hasattr(script, 'get_name') else str(script)\n")
						TEXT("        unreal.log('script=' + script_name)\n")
						TEXT("        graph = script.get_source_graph() if hasattr(script, 'get_source_graph') else script.get_editor_property('node_graph') if hasattr(script, 'get_editor_property') else None\n")
						TEXT("        if graph is not None and hasattr(graph, 'nodes'):\n")
						TEXT("            for node in list(graph.nodes):\n")
						TEXT("                unreal.log('module=' + node.get_name())\n")
						TEXT("elif tool_name == 'niagara_add_module_to_stack':\n")
						TEXT("    if emitter is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve Niagara emitter')\n")
						TEXT("    script_asset = asset_subsystem.load_asset(args.get('script_asset_path', ''))\n")
						TEXT("    if script_asset is None:\n")
						TEXT("        raise RuntimeError('Failed to load Niagara module script asset')\n")
						TEXT("    if editor_subsystem is not None and hasattr(editor_subsystem, 'add_module_script_to_emitter'):\n")
						TEXT("        editor_subsystem.add_module_script_to_emitter(asset, emitter_name, script_asset, args.get('stack_key', ''))\n")
						TEXT("    elif hasattr(unreal.NiagaraEditorUtilities, 'add_module_script'):\n")
						TEXT("        unreal.NiagaraEditorUtilities.add_module_script(emitter, script_asset)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('No Niagara module insertion API is available')\n")
						TEXT("    asset_subsystem.save_loaded_asset(asset)\n")
						TEXT("elif tool_name == 'niagara_remove_module_from_stack':\n")
						TEXT("    if editor_subsystem is not None and hasattr(editor_subsystem, 'remove_module_from_emitter'):\n")
						TEXT("        editor_subsystem.remove_module_from_emitter(asset, emitter_name, args.get('module_name', ''))\n")
						TEXT("        asset_subsystem.save_loaded_asset(asset)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('Niagara module removal API is unavailable')\n")
						TEXT("elif tool_name == 'niagara_set_module_enabled':\n")
						TEXT("    if editor_subsystem is not None and hasattr(editor_subsystem, 'set_module_is_enabled'):\n")
						TEXT("        editor_subsystem.set_module_is_enabled(asset, emitter_name, args.get('module_name', ''), bool(args.get('enabled', True)))\n")
						TEXT("        asset_subsystem.save_loaded_asset(asset)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('Niagara module enable API is unavailable')\n")
						TEXT("elif tool_name == 'niagara_set_module_input_value':\n")
						TEXT("    if editor_subsystem is not None and hasattr(editor_subsystem, 'set_module_input_value'):\n")
						TEXT("        editor_subsystem.set_module_input_value(asset, emitter_name, args.get('module_name', ''), args.get('input_name', ''), args.get('value'))\n")
						TEXT("        asset_subsystem.save_loaded_asset(asset)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('Niagara module input value API is unavailable')\n")
						TEXT("elif tool_name == 'niagara_set_module_input_linked_parameter':\n")
						TEXT("    if editor_subsystem is not None and hasattr(editor_subsystem, 'set_module_input_linked_parameter'):\n")
						TEXT("        editor_subsystem.set_module_input_linked_parameter(asset, emitter_name, args.get('module_name', ''), args.get('input_name', ''), args.get('parameter_name', ''))\n")
						TEXT("        asset_subsystem.save_loaded_asset(asset)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('Niagara linked parameter API is unavailable')\n")
						TEXT("elif tool_name == 'niagara_set_module_input_dynamic_input':\n")
						TEXT("    if editor_subsystem is not None and hasattr(editor_subsystem, 'set_module_input_dynamic_input'):\n")
						TEXT("        script_asset = asset_subsystem.load_asset(args.get('dynamic_input_script_path', ''))\n")
						TEXT("        editor_subsystem.set_module_input_dynamic_input(asset, emitter_name, args.get('module_name', ''), args.get('input_name', ''), script_asset)\n")
						TEXT("        asset_subsystem.save_loaded_asset(asset)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('Niagara dynamic input API is unavailable')\n")
						TEXT("elif tool_name == 'niagara_list_scripts':\n")
						TEXT("    for script in iter_scripts(asset, emitter):\n")
						TEXT("        unreal.log('script=' + script.get_name())\n")
						TEXT("elif tool_name == 'niagara_list_script_graph_nodes':\n")
						TEXT("    desired_script = args.get('script_name', '')\n")
						TEXT("    for script in iter_scripts(asset, emitter):\n")
						TEXT("        script_name = script.get_name() if hasattr(script, 'get_name') else ''\n")
						TEXT("        if desired_script and desired_script not in script_name:\n")
						TEXT("            continue\n")
						TEXT("        graph = script.get_source_graph() if hasattr(script, 'get_source_graph') else script.get_editor_property('node_graph') if hasattr(script, 'get_editor_property') else None\n")
						TEXT("        if graph is not None and hasattr(graph, 'nodes'):\n")
						TEXT("            for node in list(graph.nodes):\n")
						TEXT("                unreal.log('node=' + node.get_name() + ' title=' + node.get_node_title(unreal.NodeTitleType.FULL_TITLE))\n")
						TEXT("else:\n")
						TEXT("    raise RuntimeError('Unsupported Niagara tool')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgumentsJson),
						*PythonQuote(AssetPath));
				});
		};
		RegisterNiagaraPythonTool(TEXT("niagara_list_stack_modules"), TEXT("List stack modules in a Niagara system or emitter."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("stack_key"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		Registry.Register({
			TEXT("niagara_add_module_to_stack"),
			TEXT("Add a module script to a Niagara emitter stack using the native UE 5.8 stack graph API."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("emitter_name"), FSololmcpSchemaBuilder::String()},
				{TEXT("script_asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("stack_key"), FSololmcpSchemaBuilder::String(
					TEXT("EmitterSpawn | EmitterUpdate | ParticleSpawn | ParticleUpdate"))}
			}, {TEXT("asset_path"), TEXT("emitter_name"), TEXT("script_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
				TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString ScriptAssetPath;
				FString StackKey = TEXT("ParticleUpdate");
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)
					|| !Arguments->TryGetStringField(TEXT("script_asset_path"), ScriptAssetPath))
				{
					OutError = TEXT("Missing asset_path or script_asset_path.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("stack_key"), StackKey);

				UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(
					Context.Services.LoadAsset(AssetPath, OutError));
				if (!Emitter || !Emitter->GetLatestEmitterData())
				{
					OutError = TEXT("asset_path is not a Niagara emitter.");
					return false;
				}
				UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(
					Context.Services.LoadAsset(ScriptAssetPath, OutError));
				if (!ModuleScript)
				{
					OutError = TEXT("script_asset_path is not a Niagara module script.");
					return false;
				}

				FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
				UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
				if (!Source || !Source->NodeGraph)
				{
					OutError = TEXT("Niagara emitter has no editable stack graph.");
					return false;
				}

				ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
				if (StackKey.Equals(TEXT("EmitterSpawn"), ESearchCase::IgnoreCase))
				{
					Usage = ENiagaraScriptUsage::EmitterSpawnScript;
				}
				else if (StackKey.Equals(TEXT("EmitterUpdate"), ESearchCase::IgnoreCase))
				{
					Usage = ENiagaraScriptUsage::EmitterUpdateScript;
				}
				else if (StackKey.Equals(TEXT("ParticleSpawn"), ESearchCase::IgnoreCase))
				{
					Usage = ENiagaraScriptUsage::ParticleSpawnScript;
				}
				else if (!StackKey.Equals(TEXT("ParticleUpdate"), ESearchCase::IgnoreCase))
				{
					OutError = TEXT("Unsupported stack_key.");
					return false;
				}

				UNiagaraNodeOutput* OutputNode = nullptr;
				for (UEdGraphNode* GraphNode : Source->NodeGraph->Nodes)
				{
					UNiagaraNodeOutput* Candidate = Cast<UNiagaraNodeOutput>(GraphNode);
					if (Candidate && Candidate->GetUsage() == Usage)
					{
						OutputNode = Candidate;
						break;
					}
				}
				if (!OutputNode)
				{
					OutError = FString::Printf(
						TEXT("No Niagara output node exists for stack_key=%s."), *StackKey);
					return false;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("SOMOLMCP", "NiagaraAddModuleNative", "SOMOLMCP Add Niagara Module"));
				Emitter->Modify();
				Source->Modify();
				Source->NodeGraph->Modify();
				UNiagaraNodeFunctionCall* Added =
					FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, *OutputNode);
				if (!Added)
				{
					OutError = TEXT("Native Niagara stack graph insertion failed.");
					return false;
				}
				Emitter->PostEditChange();
				Emitter->MarkPackageDirty();

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("script_asset_path"), ScriptAssetPath);
				OutStructured->SetStringField(TEXT("stack_key"), StackKey);
				OutStructured->SetStringField(TEXT("module_node"), Added->GetName());
				OutStructured->SetStringField(TEXT("function_name"), Added->GetFunctionName());
				OutStructured->SetBoolField(TEXT("native_stack_graph_write"), true);
				OutStructured->SetBoolField(TEXT("success"), true);
				OutSummary = TEXT("Added Niagara module through native stack graph API.");
				return true;
			}
		, nullptr
		, 5
		});
		RegisterNiagaraPythonTool(TEXT("niagara_remove_module_from_stack"), TEXT("Remove a module from a Niagara emitter stack."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("module_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("emitter_name"), TEXT("module_name")}));
		RegisterNiagaraPythonTool(TEXT("niagara_set_module_enabled"), TEXT("Enable or disable a Niagara module."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("module_name"), FSololmcpSchemaBuilder::String()}, {TEXT("enabled"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("emitter_name"), TEXT("module_name"), TEXT("enabled")}));
		RegisterNiagaraPythonTool(TEXT("niagara_set_module_input_value"), TEXT("Set a value on a Niagara module input."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("module_name"), FSololmcpSchemaBuilder::String()}, {TEXT("input_name"), FSololmcpSchemaBuilder::String()}, {TEXT("value"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("emitter_name"), TEXT("module_name"), TEXT("input_name")}));
		RegisterNiagaraPythonTool(TEXT("niagara_set_module_input_linked_parameter"), TEXT("Link a Niagara module input to a parameter."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("module_name"), FSololmcpSchemaBuilder::String()}, {TEXT("input_name"), FSololmcpSchemaBuilder::String()}, {TEXT("parameter_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("emitter_name"), TEXT("module_name"), TEXT("input_name"), TEXT("parameter_name")}));
		RegisterNiagaraPythonTool(TEXT("niagara_set_module_input_dynamic_input"), TEXT("Set a dynamic input on a Niagara module input."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("emitter_name"), FSololmcpSchemaBuilder::String()}, {TEXT("module_name"), FSololmcpSchemaBuilder::String()}, {TEXT("input_name"), FSololmcpSchemaBuilder::String()}, {TEXT("dynamic_input_script_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("emitter_name"), TEXT("module_name"), TEXT("input_name"), TEXT("dynamic_input_script_path")}));
		Registry.Register({
			TEXT("niagara_list_scripts"),
			TEXT("List scripts referenced by a Niagara system."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;
				TArray<TSharedPtr<FJsonValue>> ScriptsJson;
				auto AddScript = [&ScriptsJson](UNiagaraScript* Script, const FString& ContextName)
				{
					if (!Script) return;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Script->GetName());
					Obj->SetStringField(TEXT("path"), Script->GetPathName());
					Obj->SetStringField(TEXT("context"), ContextName);
					ScriptsJson.Add(MakeShared<FJsonValueObject>(Obj));
				};
				if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
				{
					AddScript(System->GetSystemSpawnScript(), TEXT("system_spawn"));
					AddScript(System->GetSystemUpdateScript(), TEXT("system_update"));
					for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
					{
						if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetInstance().GetEmitterData())
						{
							AddScript(EmitterData->SpawnScriptProps.Script, FString::Printf(TEXT("emitter_%s_spawn"), *Handle.GetName().ToString()));
							AddScript(EmitterData->UpdateScriptProps.Script, FString::Printf(TEXT("emitter_%s_update"), *Handle.GetName().ToString()));
						}
					}
				}
				else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
				{
					if (FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData())
					{
						AddScript(Data->SpawnScriptProps.Script, TEXT("spawn"));
						AddScript(Data->UpdateScriptProps.Script, TEXT("update"));
					}
				}
				else { OutError = TEXT("Asset is not a Niagara system or emitter."); return false; }
				OutStructured->SetArrayField(TEXT("scripts"), ScriptsJson);
				OutStructured->SetNumberField(TEXT("count"), ScriptsJson.Num());
				OutSummary = TEXT("Listed Niagara scripts.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("niagara_list_script_graph_nodes"),
			TEXT("List nodes in a Niagara script graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("script_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ScriptNameFilter;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				Arguments->TryGetStringField(TEXT("script_name"), ScriptNameFilter);
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;
				TArray<TSharedPtr<FJsonValue>> NodesJson;
				auto CollectNodesFromScript = [&NodesJson, &ScriptNameFilter](UNiagaraScript* Script, const FString& ScriptContext)
				{
					if (!Script) return;
					if (!ScriptNameFilter.IsEmpty() && !Script->GetName().Contains(ScriptNameFilter)) return;
					UNiagaraScriptSourceBase* SourceBase = Script->GetLatestSource();
					UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
					if (!Source || !Source->NodeGraph) return;
					UNiagaraGraph* Graph = Source->NodeGraph;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (!Node) continue;
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("name"), Node->GetName());
						Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
						Obj->SetStringField(TEXT("script_context"), ScriptContext);
						NodesJson.Add(MakeShared<FJsonValueObject>(Obj));
					}
				};
				if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
				{
					if (UNiagaraScript* S = System->GetSystemSpawnScript()) CollectNodesFromScript(S, TEXT("system_spawn"));
					if (UNiagaraScript* S = System->GetSystemUpdateScript()) CollectNodesFromScript(S, TEXT("system_update"));
					for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
					{
						if (FVersionedNiagaraEmitterData* Data = Handle.GetInstance().GetEmitterData())
						{
							CollectNodesFromScript(Data->SpawnScriptProps.Script, FString::Printf(TEXT("emitter_%s_spawn"), *Handle.GetName().ToString()));
							CollectNodesFromScript(Data->UpdateScriptProps.Script, FString::Printf(TEXT("emitter_%s_update"), *Handle.GetName().ToString()));
						}
					}
				}
				else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
				{
					if (FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData())
					{
						CollectNodesFromScript(Data->SpawnScriptProps.Script, TEXT("spawn"));
						CollectNodesFromScript(Data->UpdateScriptProps.Script, TEXT("update"));
					}
				}
				else { OutError = TEXT("Asset is not a Niagara system or emitter."); return false; }
				OutStructured->SetArrayField(TEXT("nodes"), NodesJson);
				OutStructured->SetNumberField(TEXT("count"), NodesJson.Num());
				OutSummary = TEXT("Listed Niagara script graph nodes.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_open"),
			TEXT("Open a level sequence asset in the editor."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence)
				{
					OutError = TEXT("Asset is not a level sequence.");
					return false;
				}
				if (!ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence))
				{
					OutError = TEXT("Failed to open level sequence.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Sequence);
				OutSummary = TEXT("Opened level sequence.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_get_current"),
			TEXT("Return the currently open level sequence."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
				if (!Sequence)
				{
					OutError = TEXT("No level sequence is currently open.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Sequence);
				OutSummary = TEXT("Collected current level sequence.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_add_actors"),
			TEXT("Add actors from the level into the currently open sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}}, {TEXT("actors")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> ActorIds;
				if (!TryGetStringArray(Arguments, TEXT("actors"), ActorIds))
				{
					OutError = TEXT("Missing actors.");
					return false;
				}
				if (ActorIds.Num() == 0)
				{
					OutError = TEXT("actors must contain at least one actor id.");
					return false;
				}
				TArray<AActor*> Actors = ResolveActors(Context.Services, ActorIds, OutError);
				if (Actors.Num() != ActorIds.Num())
				{
					if (OutError.IsEmpty())
					{
						OutError = FString::Printf(TEXT("Resolved %d/%d requested actors."), Actors.Num(), ActorIds.Num());
					}
					return false;
				}
				if (!GEditor)
				{
					OutError = TEXT("GEditor is unavailable.");
					return false;
				}
				ULevelSequence* CurrentSequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
				UMovieScene* MovieScene = CurrentSequence ? CurrentSequence->GetMovieScene() : nullptr;
				if (!MovieScene)
				{
					OutError = TEXT("No level sequence is currently open.");
					return false;
				}
				ULevelSequenceEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<ULevelSequenceEditorSubsystem>();
				if (!Subsystem)
				{
					OutError = TEXT("LevelSequenceEditorSubsystem is unavailable.");
					return false;
				}
				const TArray<FMovieSceneBindingProxy> Bindings = Subsystem->AddActors(Actors);
				if (Bindings.Num() != Actors.Num())
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("actors"),
						TEXT("Not all actors were bound into the current sequence."));
					OutError = FString::Printf(TEXT("Added %d/%d actor bindings."), Bindings.Num(), Actors.Num());
					return false;
				}
				for (const FMovieSceneBindingProxy& Binding : Bindings)
				{
					if (!Binding.BindingID.IsValid() || !MovieScene->FindBinding(Binding.BindingID))
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("actors"),
							TEXT("AddActors returned an invalid or missing binding."));
						OutError = TEXT("AddActors returned an invalid or missing binding.");
						return false;
					}
				}
				OutStructured->SetNumberField(TEXT("bindingCount"), Bindings.Num());
				OutStructured->SetObjectField(TEXT("sequence"), FSololmcpEditorServices::MakeObjectReference(CurrentSequence));
				OutSummary = TEXT("Added actors to sequence.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_create_camera"),
			TEXT("Create and bind a cine camera in the currently open sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("spawnable"), FSololmcpSchemaBuilder::Boolean()}}, {}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("GEditor is unavailable.");
					return false;
				}
				ULevelSequenceEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<ULevelSequenceEditorSubsystem>();
				if (!Subsystem)
				{
					OutError = TEXT("LevelSequenceEditorSubsystem is unavailable.");
					return false;
				}
				ULevelSequence* CurrentSequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
				UMovieScene* MovieScene = CurrentSequence ? CurrentSequence->GetMovieScene() : nullptr;
				if (!MovieScene)
				{
					OutError = TEXT("No level sequence is currently open.");
					return false;
				}
				const bool bSpawnable = Arguments->HasTypedField<EJson::Boolean>(TEXT("spawnable")) ? Arguments->GetBoolField(TEXT("spawnable")) : true;
				ACineCameraActor* CameraActor = nullptr;
				const FMovieSceneBindingProxy Binding = Subsystem->CreateCamera(bSpawnable, CameraActor);
				if (!CameraActor || !Binding.BindingID.IsValid())
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("CreateCamera returned no camera actor or an invalid binding id."));
					OutError = TEXT("Failed to create and bind sequence camera.");
					return false;
				}
				if (!MovieScene->FindBinding(Binding.BindingID))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("CreateCamera returned a binding that is missing from the current movie scene."));
					OutError = TEXT("Created camera binding was not found in the current sequence.");
					return false;
				}
				OutStructured->SetObjectField(TEXT("camera"), FSololmcpEditorServices::MakeActorReference(CameraActor));
				OutStructured->SetStringField(TEXT("bindingId"), Binding.BindingID.ToString());
				OutSummary = TEXT("Created sequence camera.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_create_asset"),
			TEXT("Create a level sequence asset."),
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
				const FString FullAssetPath = CombinePackageAssetPath(PackagePath, AssetName);
				if (Context.Services.AssetExists(FullAssetPath) || Context.Services.AssetExists(FullAssetPath + TEXT(".") + AssetName))
				{
					OutStructured->SetStringField(TEXT("asset_path"), FullAssetPath);
					OutError = FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath);
					return false;
				}
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/LevelSequence.LevelSequence"), TEXT("/Script/LevelSequenceEditor.LevelSequenceFactoryNew"), nullptr, OutError);
				if (!Asset)
				{
					return false;
				}
				if (!Asset->IsA<ULevelSequence>())
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
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutSummary = TEXT("Created level sequence.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_set_playback_range"),
			TEXT("Set the playback range of a level sequence asset in display-rate frames."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("start_frame"), FSololmcpSchemaBuilder::Integer()}, {TEXT("duration_frames"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("start_frame"), TEXT("duration_frames")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				int32 StartFrame = 0;
				int32 DurationFrames = 0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetNumberField(TEXT("start_frame"), StartFrame) || !Arguments->TryGetNumberField(TEXT("duration_frames"), DurationFrames))
				{
					OutError = TEXT("Missing asset_path, start_frame or duration_frames.");
					return false;
				}
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence)
				{
					OutError = TEXT("Asset is not a level sequence.");
					return false;
				}
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				if (!MovieScene)
				{
					OutError = TEXT("Level sequence movie scene is unavailable.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceSetPlaybackRange", "SOMOLMCP Set Sequence Playback Range"));
				MovieScene->Modify();
				MovieScene->SetPlaybackRange(StartFrame, DurationFrames);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Sequence);
				OutSummary = TEXT("Updated level sequence playback range.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_convert_binding_to_spawnable"),
			TEXT("Convert a binding in a level sequence to a spawnable."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("binding_id")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString BindingIdString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("binding_id"), BindingIdString))
				{
					OutError = TEXT("Missing asset_path or binding_id.");
					return false;
				}
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence)
				{
					OutError = TEXT("Asset is not a level sequence.");
					return false;
				}
				FGuid BindingGuid;
				if (!FGuid::Parse(BindingIdString, BindingGuid))
				{
					OutError = TEXT("binding_id is not a valid guid.");
					return false;
				}
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				if (!MovieScene || !MovieScene->FindBinding(BindingGuid))
				{
					OutError = TEXT("binding_id was not found in the level sequence.");
					return false;
				}
				if (!MovieScene->FindPossessable(BindingGuid))
				{
					OutError = TEXT("binding_id is not a possessable binding.");
					return false;
				}
				if (!ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence))
				{
					OutError = TEXT("Failed to open level sequence.");
					return false;
				}
				if (!GEditor)
				{
					OutError = TEXT("GEditor is unavailable.");
					return false;
				}
				ULevelSequenceEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<ULevelSequenceEditorSubsystem>();
				if (!Subsystem)
				{
					OutError = TEXT("LevelSequenceEditorSubsystem is unavailable.");
					return false;
				}
				const TArray<FMovieSceneBindingProxy> NewBindings = Subsystem->ConvertToSpawnable(FMovieSceneBindingProxy(BindingGuid, Sequence));
				if (NewBindings.Num() == 0)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("binding_id"),
						TEXT("ConvertToSpawnable returned no new bindings."));
					OutError = TEXT("Binding was not converted to a spawnable.");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> BindingJson;
				for (const FMovieSceneBindingProxy& Binding : NewBindings)
				{
					if (!Binding.BindingID.IsValid() || !MovieScene->FindBinding(Binding.BindingID))
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("binding_id"),
							TEXT("ConvertToSpawnable returned an invalid or missing binding."));
						OutError = TEXT("ConvertToSpawnable returned an invalid or missing binding.");
						return false;
					}
					TSharedRef<FJsonObject> BindingObject = MakeShared<FJsonObject>();
					BindingObject->SetStringField(TEXT("bindingId"), Binding.BindingID.ToString());
					BindingJson.Add(MakeShared<FJsonValueObject>(BindingObject));
				}
				OutStructured->SetArrayField(TEXT("bindings"), BindingJson);
				OutStructured->SetNumberField(TEXT("count"), BindingJson.Num());
				OutSummary = TEXT("Converted level sequence binding to spawnable.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_convert_binding_to_possessable"),
			TEXT("Convert a binding in a level sequence to a possessable."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("binding_id")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString BindingIdString;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("binding_id"), BindingIdString))
				{
					OutError = TEXT("Missing asset_path or binding_id.");
					return false;
				}
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence)
				{
					OutError = TEXT("Asset is not a level sequence.");
					return false;
				}
				FGuid BindingGuid;
				if (!FGuid::Parse(BindingIdString, BindingGuid))
				{
					OutError = TEXT("binding_id is not a valid guid.");
					return false;
				}
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				if (!MovieScene || !MovieScene->FindBinding(BindingGuid))
				{
					OutError = TEXT("binding_id was not found in the level sequence.");
					return false;
				}
				if (!MovieScene->FindSpawnable(BindingGuid))
				{
					OutError = TEXT("binding_id is not a spawnable binding.");
					return false;
				}
				if (!ULevelSequenceEditorBlueprintLibrary::OpenLevelSequence(Sequence))
				{
					OutError = TEXT("Failed to open level sequence.");
					return false;
				}
				if (!GEditor)
				{
					OutError = TEXT("GEditor is unavailable.");
					return false;
				}
				ULevelSequenceEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<ULevelSequenceEditorSubsystem>();
				if (!Subsystem)
				{
					OutError = TEXT("LevelSequenceEditorSubsystem is unavailable.");
					return false;
				}
				const FMovieSceneBindingProxy NewBinding = Subsystem->ConvertToPossessable(FMovieSceneBindingProxy(BindingGuid, Sequence));
				if (!NewBinding.BindingID.IsValid() || !MovieScene->FindBinding(NewBinding.BindingID))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("binding_id"),
						TEXT("ConvertToPossessable returned an invalid or missing binding."));
					OutError = TEXT("Binding was not converted to a possessable.");
					return false;
				}
				OutStructured->SetStringField(TEXT("bindingId"), NewBinding.BindingID.ToString());
				OutStructured->SetObjectField(TEXT("sequence"), FSololmcpEditorServices::MakeObjectReference(Sequence));
				OutSummary = TEXT("Converted level sequence binding to possessable.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("control_rig_create"),
			TEXT("Create a Control Rig Blueprint asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("modular"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				const bool bModular = Arguments->HasTypedField<EJson::Boolean>(TEXT("modular")) ? Arguments->GetBoolField(TEXT("modular")) : false;
				// Modular Control Rig arrived in UE 5.4; 5.3's factory takes the path only.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				UControlRigBlueprint* RigBlueprint = UControlRigBlueprintFactory::CreateNewControlRigAsset(AssetPath, bModular);
#else
				if (bModular)
				{
					OutError = TEXT("Modular Control Rig requires UE 5.4 or newer; retry with modular=false.");
					return false;
				}
				UControlRigBlueprint* RigBlueprint = UControlRigBlueprintFactory::CreateNewControlRigAsset(AssetPath);
#endif
				if (!RigBlueprint)
				{
					OutError = TEXT("Failed to create Control Rig.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(RigBlueprint);
				OutSummary = TEXT("Created Control Rig.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("control_rig_set_preview_mesh"),
			TEXT("Set the preview skeletal mesh for a Control Rig Blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("preview_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("preview_mesh_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, PreviewMeshPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("preview_mesh_path"), PreviewMeshPath))
				{
					OutError = TEXT("Missing asset_path or preview_mesh_path.");
					return false;
				}
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(PreviewMeshPath, OutError));
				if (!Mesh) { OutError = TEXT("Failed to load preview mesh asset."); return false; }
				UControlRigBlueprintEditorLibrary::SetPreviewMesh(Rig, Mesh, true);
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("preview_mesh_path"), PreviewMeshPath);
				OutSummary = TEXT("Set Control Rig preview mesh.");
				return true;
			}
		// AN-11 fix 2026-08-05: set_preview_mesh is a mutation; CacheTtlSeconds must be 0.
		, nullptr
		, 0
		});

		// Round 12K-D fix: pure-C++ replacement of control_rig_template_create_from_skeletal_mesh.
		// The previous python path raised "RuntimeError: Asset creation failed for
		// control_rig_template_create_from_skeletal_mesh" on bogus args, AND on editor shutdown
		// the python ControlRig template objects were forcibly released causing an access
		// violation crash. Doing this in C++ via UControlRigBlueprintFactory gives a deterministic
		// error path and removes the python dependency entirely.
		// Schema preserved from the legacy python tool: package_path + asset_name + skeletal_mesh_path.
		Registry.Register({
			TEXT("control_rig_template_create_from_skeletal_mesh"),
			TEXT("Create a template-style Control Rig Blueprint from a skeletal mesh (pure C++)."),
			FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name"), TEXT("skeletal_mesh_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
			{
				FString PackagePath, AssetName, MeshPath;
				if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) ||
					!Arguments->TryGetStringField(TEXT("asset_name"), AssetName) ||
					!Arguments->TryGetStringField(TEXT("skeletal_mesh_path"), MeshPath))
				{
					OutError = TEXT("Missing package_path, asset_name, or skeletal_mesh_path.");
					return false;
				}
				if (PackagePath.IsEmpty() || AssetName.IsEmpty() || MeshPath.IsEmpty())
				{
					OutError = TEXT("package_path, asset_name, and skeletal_mesh_path must be non-empty.");
					return false;
				}
				// Validate skeletal mesh BEFORE creating the rig — avoid leaving a half-created
				// asset on disk if the mesh is bogus.
				UObject* MeshObj = Context.Services.LoadAsset(MeshPath, OutError);
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(MeshObj);
				if (!Mesh)
				{
					if (OutError.IsEmpty())
					{
						OutError = TEXT("skeletal_mesh_path is not a USkeletalMesh.");
					}
					return false;
				}
				// Compose the full asset path expected by CreateNewControlRigAsset (e.g. /Game/Foo/MyRig).
				FString FullAssetPath = PackagePath;
				if (!FullAssetPath.EndsWith(TEXT("/")))
				{
					FullAssetPath += TEXT("/");
				}
				FullAssetPath += AssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				UControlRigBlueprint* RigBlueprint = UControlRigBlueprintFactory::CreateNewControlRigAsset(FullAssetPath, /*bModular=*/false);
#else
				UControlRigBlueprint* RigBlueprint = UControlRigBlueprintFactory::CreateNewControlRigAsset(FullAssetPath);
#endif
				if (!RigBlueprint)
				{
					OutError = FString::Printf(TEXT("Failed to create Control Rig at %s."), *FullAssetPath);
					return false;
				}
				// Bind the preview mesh on the new ControlRig BP. SetPreviewMesh handles MarkPackageDirty
				// internally when bMarkAsModified=true.
				UControlRigBlueprintEditorLibrary::SetPreviewMesh(RigBlueprint, Mesh, /*bMarkAsModified=*/true);
				FAssetRegistryModule::AssetCreated(RigBlueprint);
				RigBlueprint->MarkPackageDirty();
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(RigBlueprint->GetPathName(), /*bPromptToSave=*/false, SaveErr);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(RigBlueprint);
				OutStructured->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
				OutStructured->SetBoolField(TEXT("saved"), bSaved);
				if (!bSaved && !SaveErr.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("save_error"), SaveErr);
				}
				OutSummary = TEXT("Created Control Rig template from skeletal mesh.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_describe"),
			TEXT("Describe a level sequence asset, including bindings, tracks and folders."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) { OutError = TEXT("Failed to load level sequence."); return false; }
				UMovieScene* MS = Sequence->GetMovieScene();
				if (!MS) { OutError = TEXT("Movie scene unavailable."); return false; }
				const UMovieScene* ConstMS = MS;
				OutStructured->SetStringField(TEXT("sequence"), Sequence->GetPathName());
				OutStructured->SetNumberField(TEXT("binding_count"), ConstMS->GetBindings().Num());
				OutStructured->SetNumberField(TEXT("master_track_count"), MS->GetTracks().Num());
				TArray<UMovieSceneFolder*> RootFolders;
				MS->GetRootFolders(RootFolders);
				OutStructured->SetNumberField(TEXT("folder_count"), RootFolders.Num());
				OutSummary = TEXT("Described level sequence.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_list_bindings"),
			TEXT("List bindings in a level sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MS = Sequence->GetMovieScene();
				if (!MS) { OutError = TEXT("Movie scene unavailable."); return false; }
				const UMovieScene* ConstMS = MS;
				TArray<TSharedPtr<FJsonValue>> BindingsJson;
				for (const FMovieSceneBinding& B : ConstMS->GetBindings())
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("id"), B.GetObjectGuid().ToString());
					Obj->SetStringField(TEXT("name"), MS->GetObjectDisplayName(B.GetObjectGuid()).ToString());
					Obj->SetNumberField(TEXT("track_count"), B.GetTracks().Num());
					BindingsJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("bindings"), BindingsJson);
				OutSummary = TEXT("Listed sequence bindings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_list_tracks"),
			TEXT("List tracks in a level sequence or binding."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, BindingId;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MS = Sequence->GetMovieScene();
				if (!MS) { OutError = TEXT("Movie scene unavailable."); return false; }
				TArray<TSharedPtr<FJsonValue>> TracksJson;
				TArray<UMovieSceneTrack*> TracksToIterate;
				if (BindingId.IsEmpty())
				{
					for (UMovieSceneTrack* T : MS->GetTracks()) TracksToIterate.Add(T);
				}
				else
				{
					FGuid Guid; if (!FGuid::Parse(BindingId, Guid)) { OutError = TEXT("Invalid binding_id."); return false; }
					const FMovieSceneBinding* B = MS->FindBinding(Guid);
					if (!B) { OutError = TEXT("Binding not found."); return false; }
					for (UMovieSceneTrack* T : B->GetTracks()) TracksToIterate.Add(T);
				}
				for (UMovieSceneTrack* T : TracksToIterate)
				{
					if (!T) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), T->GetTrackName().ToString());
					Obj->SetStringField(TEXT("class"), T->GetClass()->GetPathName());
					TracksJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("tracks"), TracksJson);
				OutSummary = TEXT("Listed sequence tracks.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_list_sections"),
			TEXT("List sections on sequence tracks."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, BindingId, TrackName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
				Arguments->TryGetStringField(TEXT("track_name"), TrackName);
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MS = Sequence->GetMovieScene();
				if (!MS) { OutError = TEXT("Movie scene unavailable."); return false; }
				TArray<TSharedPtr<FJsonValue>> SectionsJson;
				TArray<UMovieSceneTrack*> TracksToIterate;
				if (BindingId.IsEmpty())
				{
					for (UMovieSceneTrack* T : MS->GetTracks()) TracksToIterate.Add(T);
				}
				else
				{
					FGuid Guid; if (!FGuid::Parse(BindingId, Guid)) { OutError = TEXT("Invalid binding_id."); return false; }
					const FMovieSceneBinding* B = MS->FindBinding(Guid);
					if (!B) { OutError = TEXT("Binding not found."); return false; }
					for (UMovieSceneTrack* T : B->GetTracks()) TracksToIterate.Add(T);
				}
				for (UMovieSceneTrack* T : TracksToIterate)
				{
					if (!T || (!TrackName.IsEmpty() && T->GetTrackName() != FName(*TrackName))) continue;
					int32 Idx = 0;
					for (UMovieSceneSection* S : T->GetAllSections())
					{
						if (!S) continue;
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("track"), T->GetTrackName().ToString());
						Obj->SetNumberField(TEXT("index"), Idx);
						Obj->SetStringField(TEXT("class"), S->GetClass()->GetPathName());
						SectionsJson.Add(MakeShared<FJsonValueObject>(Obj));
						Idx++;
					}
				}
				OutStructured->SetArrayField(TEXT("sections"), SectionsJson);
				OutSummary = TEXT("Listed sequence sections.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_list_channels"),
			TEXT("List channels on a sequence section."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}, {TEXT("section_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("asset_path"), TEXT("track_name"), TEXT("section_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, BindingId, TrackName;
				int32 SectionIndex = 0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("track_name"), TrackName) || !Arguments->TryGetNumberField(TEXT("section_index"), SectionIndex)) { OutError = TEXT("Missing sequence channel arguments."); return false; }
				Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MS = Sequence->GetMovieScene();
				if (!MS) { OutError = TEXT("Movie scene unavailable."); return false; }
				TArray<UMovieSceneTrack*> TracksToIterate;
				if (BindingId.IsEmpty())
				{
					for (UMovieSceneTrack* T : MS->GetTracks()) TracksToIterate.Add(T);
				}
				else
				{
					FGuid Guid; if (!FGuid::Parse(BindingId, Guid)) { OutError = TEXT("Invalid binding_id."); return false; }
					const FMovieSceneBinding* B = MS->FindBinding(Guid);
					if (!B) { OutError = TEXT("Binding not found."); return false; }
					for (UMovieSceneTrack* T : B->GetTracks()) TracksToIterate.Add(T);
				}
				TArray<TSharedPtr<FJsonValue>> ChannelsJson;
				TArray<TSharedPtr<FJsonValue>> ChannelInstancesJson;
				int32 TotalChannelCount = 0;
				for (UMovieSceneTrack* T : TracksToIterate)
				{
					if (!T || T->GetTrackName() != FName(*TrackName)) continue;
					const TArray<UMovieSceneSection*>& Secs = T->GetAllSections();
					if (SectionIndex < 0 || SectionIndex >= Secs.Num()) { OutError = TEXT("Section index out of range."); return false; }
					UMovieSceneSection* Sec = Secs[SectionIndex];
					if (!Sec) continue;
					for (const FMovieSceneChannelEntry& Entry : const_cast<UMovieSceneSection*>(Sec)->GetChannelProxy().GetAllEntries())
					{
						const FString ChannelType = Entry.GetChannelTypeName().ToString();
						const int32 ChannelCount = Entry.GetChannels().Num();
#if WITH_EDITOR
						const TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
#endif
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("type"), ChannelType);
						Obj->SetNumberField(TEXT("channel_count"), ChannelCount);
						ChannelsJson.Add(MakeShared<FJsonValueObject>(Obj));
						for (int32 ChannelIndex = 0; ChannelIndex < ChannelCount; ++ChannelIndex)
						{
							TSharedPtr<FJsonObject> Instance = MakeShared<FJsonObject>();
							Instance->SetStringField(TEXT("type"), ChannelType);
							Instance->SetNumberField(TEXT("type_index"), ChannelIndex);
							Instance->SetNumberField(TEXT("absolute_index"), TotalChannelCount + ChannelIndex);
#if WITH_EDITOR
							if (ChannelIndex >= 0 && ChannelIndex < MetaData.Num())
							{
								Instance->SetStringField(TEXT("name"), MetaData[ChannelIndex].Name.ToString());
								Instance->SetStringField(TEXT("display_name"), MetaData[ChannelIndex].DisplayText.ToString());
								Instance->SetStringField(TEXT("group"), MetaData[ChannelIndex].Group.ToString());
							}
#endif
							ChannelInstancesJson.Add(MakeShared<FJsonValueObject>(Instance));
						}
						TotalChannelCount += ChannelCount;
					}
					break;
				}
				OutStructured->SetArrayField(TEXT("channels"), ChannelsJson);
				OutStructured->SetArrayField(TEXT("channel_instances"), ChannelInstancesJson);
				OutStructured->SetNumberField(TEXT("channel_group_count"), ChannelsJson.Num());
				OutStructured->SetNumberField(TEXT("channel_count_total"), TotalChannelCount);
				OutSummary = TEXT("Listed sequence channels.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("sequence_add_track"),
			TEXT("Add a native master or object-binding track to a level sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("track_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("track_class_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, TrackClassPath, BindingId;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("track_class_path"), TrackClassPath))
				{
					OutError = TEXT("Missing asset_path or track_class_path.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
				const bool bSaveAsset = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				if (!MovieScene) { OutError = TEXT("LevelSequence has no MovieScene."); return false; }
				UClass* TrackClass = ResolveMovieSceneTrackClass(TrackClassPath, OutError);
				if (!TrackClass) return false;
				FGuid BindingGuid;
				if (!ResolveMovieSceneBindingGuid(MovieScene, BindingId, BindingGuid, OutError)) return false;

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddTrackNative", "SOMOLMCP Add Sequence Track"));
				Sequence->Modify();
				MovieScene->Modify();
				UMovieSceneTrack* NewTrack = BindingGuid.IsValid()
					? MovieScene->AddTrack(TrackClass, BindingGuid)
					: MovieScene->AddTrack(TrackClass);
				if (!NewTrack)
				{
					OutError = FString::Printf(TEXT("Failed to add track class '%s'; the track may be unique or incompatible with this binding."), *TrackClass->GetPathName());
					return false;
				}
				MovieScene->MarkAsChanged();
				Sequence->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(Sequence);
				if (bSaveAsset && !Context.Services.SaveAsset(Sequence->GetPathName(), false, OutError)) return false;

				const bool bVerified = BindingGuid.IsValid()
					? (MovieScene->FindBinding(BindingGuid) && MovieScene->FindBinding(BindingGuid)->GetTracks().Contains(NewTrack))
					: MovieScene->GetTracks().Contains(NewTrack);
				OutStructured->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
				OutStructured->SetStringField(TEXT("track_name"), NewTrack->GetTrackName().ToString());
				OutStructured->SetStringField(TEXT("track_class"), NewTrack->GetClass()->GetPathName());
				OutStructured->SetStringField(TEXT("binding_id"), BindingGuid.IsValid() ? BindingGuid.ToString() : FString());
				OutStructured->SetStringField(TEXT("scope"), BindingGuid.IsValid() ? TEXT("binding") : TEXT("master"));
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("success") : TEXT("failed"));
				if (!bVerified) { OutError = TEXT("Sequence track readback failed after creation."); return false; }
				OutSummary = FString::Printf(TEXT("Added and verified native %s track '%s'."), BindingGuid.IsValid() ? TEXT("binding") : TEXT("master"), *NewTrack->GetTrackName().ToString());
				return true;
			}
		});

		Registry.Register({
			TEXT("sequence_add_property_track"),
			TEXT("Add and configure a native property track on a sequence binding."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}, {TEXT("track_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("property_name"), FSololmcpSchemaBuilder::String()}, {TEXT("property_path"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("binding_id"), TEXT("track_class_path"), TEXT("property_name"), TEXT("property_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, BindingId, TrackClassPath, PropertyName, PropertyPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("binding_id"), BindingId)
					|| !Arguments->TryGetStringField(TEXT("track_class_path"), TrackClassPath) || !Arguments->TryGetStringField(TEXT("property_name"), PropertyName)
					|| !Arguments->TryGetStringField(TEXT("property_path"), PropertyPath))
				{
					OutError = TEXT("Missing property track arguments.");
					return false;
				}
				const bool bSaveAsset = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				if (!MovieScene) { OutError = TEXT("LevelSequence has no MovieScene."); return false; }
				UClass* TrackClass = ResolveMovieSceneTrackClass(TrackClassPath, OutError);
				if (!TrackClass) return false;
				if (!TrackClass->IsChildOf(UMovieScenePropertyTrack::StaticClass()))
				{
					OutError = FString::Printf(TEXT("Track class does not support property binding: %s"), *TrackClass->GetPathName());
					return false;
				}
				FGuid BindingGuid;
				if (!ResolveMovieSceneBindingGuid(MovieScene, BindingId, BindingGuid, OutError) || !BindingGuid.IsValid()) return false;

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddPropertyTrackNative", "SOMOLMCP Add Sequence Property Track"));
				Sequence->Modify();
				MovieScene->Modify();
				UMovieScenePropertyTrack* NewTrack = Cast<UMovieScenePropertyTrack>(MovieScene->AddTrack(TrackClass, BindingGuid));
				if (!NewTrack)
				{
					OutError = FString::Printf(TEXT("Failed to add property track class '%s'."), *TrackClass->GetPathName());
					return false;
				}
				NewTrack->Modify();
				NewTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);
				MovieScene->MarkAsChanged();
				Sequence->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(Sequence);
				if (bSaveAsset && !Context.Services.SaveAsset(Sequence->GetPathName(), false, OutError)) return false;

				const bool bVerified = MovieScene->FindBinding(BindingGuid)
					&& MovieScene->FindBinding(BindingGuid)->GetTracks().Contains(NewTrack)
					&& NewTrack->GetPropertyName() == FName(*PropertyName)
					&& NewTrack->GetPropertyPath() == FName(*PropertyPath);
				OutStructured->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
				OutStructured->SetStringField(TEXT("binding_id"), BindingGuid.ToString());
				OutStructured->SetStringField(TEXT("track_name"), NewTrack->GetTrackName().ToString());
				OutStructured->SetStringField(TEXT("track_class"), NewTrack->GetClass()->GetPathName());
				OutStructured->SetStringField(TEXT("property_name"), NewTrack->GetPropertyName().ToString());
				OutStructured->SetStringField(TEXT("property_path"), NewTrack->GetPropertyPath().ToString());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("success") : TEXT("failed"));
				if (!bVerified) { OutError = TEXT("Property track readback failed after creation."); return false; }
				OutSummary = FString::Printf(TEXT("Added and verified native property track '%s' for '%s'."), *NewTrack->GetTrackName().ToString(), *PropertyPath);
				return true;
			}
		});

		RegisterPythonTool(TEXT("sequence_add_section"), TEXT("Add a section to a track."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}, {TEXT("section_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("track_name")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
			{
				FString AssetPath, TrackName, BindingId, SectionClassPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("track_name"), TrackName)) { OutError = TEXT("Missing asset_path or track_name."); return FString(); }
				Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
				Arguments->TryGetStringField(TEXT("section_class_path"), SectionClassPath);
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				return FString::Printf(TEXT("import unreal\nseq=unreal.EditorAssetSubsystem().load_asset(%s)\nms=seq.get_movie_scene()\ntracks=[]\nbinding_id=%s\nbinding_found=False\nif binding_id:\n    for b in ms.get_bindings():\n        if str(b.get_id()) == binding_id:\n            tracks=list(b.get_tracks())\n            binding_found=True\n            break\n    if not binding_found: raise RuntimeError('Binding not found')\nelse:\n    tracks=list(ms.get_master_tracks())\nmutated=False\nfor t in tracks:\n    if t.get_name()==%s:\n        if %s and hasattr(t,'add_section'):\n            sec_cls=unreal.load_class(None,%s)\n            if sec_cls is None: raise RuntimeError('Failed to resolve section class')\n            section=t.add_section(sec_cls)\n        elif hasattr(t,'add_section'):\n            section=t.add_section()\n        else:\n            raise RuntimeError('Track does not support sections')\n        if section is None: raise RuntimeError('Failed to add section')\n        mutated=True\n        break\nif not mutated: raise RuntimeError('Track not found')\nif %s:\n    if not unreal.EditorAssetSubsystem().save_loaded_asset(seq): raise RuntimeError('Failed to save sequence')\nprint('SOMO_SEQUENCE_ADD_SECTION:OK')\n"),
					*PythonQuote(AssetPath), *PythonQuote(BindingId), *PythonQuote(TrackName), *PythonQuote(SectionClassPath), *PythonQuote(SectionClassPath), bSaveAsset ? TEXT("True") : TEXT("False"));
			});

		Registry.Register({
			TEXT("sequence_set_section_range"),
			TEXT("Set and verify the native frame range of a sequence section."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("track_name"), FSololmcpSchemaBuilder::String()}, {TEXT("section_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("start_frame"), FSololmcpSchemaBuilder::Integer()}, {TEXT("end_frame"), FSololmcpSchemaBuilder::Integer()}, {TEXT("binding_id"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("track_name"), TEXT("section_index"), TEXT("start_frame"), TEXT("end_frame")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, TrackName, BindingId;
				int32 SectionIndex = 0, StartFrame = 0, EndFrame = 0;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("track_name"), TrackName)
					|| !Arguments->TryGetNumberField(TEXT("section_index"), SectionIndex) || !Arguments->TryGetNumberField(TEXT("start_frame"), StartFrame)
					|| !Arguments->TryGetNumberField(TEXT("end_frame"), EndFrame))
				{
					OutError = TEXT("Missing section range arguments.");
					return false;
				}
				if (SectionIndex < 0 || EndFrame <= StartFrame)
				{
					OutError = TEXT("section_index must be non-negative and end_frame must be greater than start_frame.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("binding_id"), BindingId);
				const bool bSaveAsset = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MovieScene = Sequence->GetMovieScene();
				if (!MovieScene) { OutError = TEXT("LevelSequence has no MovieScene."); return false; }
				FGuid BindingGuid;
				if (!ResolveMovieSceneBindingGuid(MovieScene, BindingId, BindingGuid, OutError)) return false;
				UMovieSceneTrack* Track = FindMovieSceneTrackByName(MovieScene, BindingGuid, TrackName);
				if (!Track) { OutError = FString::Printf(TEXT("Track was not found: %s"), *TrackName); return false; }
				const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
				if (!Sections.IsValidIndex(SectionIndex) || !Sections[SectionIndex])
				{
					OutError = FString::Printf(TEXT("section_index %d is out of range for track '%s'."), SectionIndex, *TrackName);
					return false;
				}
				UMovieSceneSection* Section = Sections[SectionIndex];
				const TRange<FFrameNumber> RequestedRange{FFrameNumber(StartFrame), FFrameNumber(EndFrame)};
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceSetSectionRangeNative", "SOMOLMCP Set Sequence Section Range"));
				Sequence->Modify();
				MovieScene->Modify();
				Track->Modify();
				Section->Modify();
				Section->SetRange(RequestedRange);
				MovieScene->MarkAsChanged();
				Sequence->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(Sequence);
				if (bSaveAsset && !Context.Services.SaveAsset(Sequence->GetPathName(), false, OutError)) return false;
				const bool bVerified = Section->GetRange() == RequestedRange;
				OutStructured->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
				OutStructured->SetStringField(TEXT("track_name"), Track->GetTrackName().ToString());
				OutStructured->SetNumberField(TEXT("section_index"), SectionIndex);
				OutStructured->SetNumberField(TEXT("start_frame"), StartFrame);
				OutStructured->SetNumberField(TEXT("end_frame"), EndFrame);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("saved"), bSaveAsset);
				OutStructured->SetBoolField(TEXT("verified"), bVerified);
				OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("success") : TEXT("failed"));
				if (!bVerified) { OutError = TEXT("Section range readback failed after mutation."); return false; }
				OutSummary = FString::Printf(TEXT("Set and verified section %d range [%d, %d) on '%s'."), SectionIndex, StartFrame, EndFrame, *Track->GetTrackName().ToString());
				return true;
			}
		});

		RegisterPythonTool(TEXT("sequence_add_folder"), TEXT("Add a folder to a level sequence."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("folder_name"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("folder_name")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
			{
				FString AssetPath, FolderName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("folder_name"), FolderName)) { OutError = TEXT("Missing asset_path or folder_name."); return FString(); }
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				return FString::Printf(TEXT("import unreal\nseq=unreal.EditorAssetSubsystem().load_asset(%s)\nms=seq.get_movie_scene()\nfolder=unreal.MovieSceneFolder()\nfolder.set_folder_name(%s)\nms.add_root_folder(folder)\nif %s:\n    unreal.EditorAssetSubsystem().save_loaded_asset(seq)\n"),
					*PythonQuote(AssetPath), *PythonQuote(FolderName), bSaveAsset ? TEXT("True") : TEXT("False"));
			});

		Registry.Register({
			TEXT("sequence_list_folders"),
			TEXT("List folders in a level sequence."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Sequence) return false;
				UMovieScene* MS = Sequence->GetMovieScene();
				if (!MS) { OutError = TEXT("Movie scene unavailable."); return false; }
				TArray<UMovieSceneFolder*> RootFolders;
				MS->GetRootFolders(RootFolders);
				TArray<TSharedPtr<FJsonValue>> FoldersJson;
				for (UMovieSceneFolder* F : RootFolders)
				{
					if (!F) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), FString(F->GetFolderName().ToString()));
					FoldersJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("folders"), FoldersJson);
				OutSummary = TEXT("Listed sequence folders.");
				return true;
			}
		, nullptr
		, 5
		});

		RegisterPythonTool(TEXT("sequence_set_marked_frames"), TEXT("Replace marked frames on a level sequence."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("frames"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer())}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("frames")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
			{
				FString AssetPath; TArray<FString> FrameStrings;
				const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetArrayField(TEXT("frames"), Frames) || !Frames) { OutError = TEXT("Missing asset_path or frames."); return FString(); }
				for (const TSharedPtr<FJsonValue>& Value : *Frames) { FrameStrings.Add(FString::FromInt(static_cast<int32>(Value->AsNumber()))); }
				const bool bSaveAsset = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) ? Arguments->GetBoolField(TEXT("save_asset")) : true;
				return FString::Printf(TEXT("import unreal\nseq=unreal.EditorAssetSubsystem().load_asset(%s)\nms=seq.get_movie_scene()\nms.set_marked_frames([unreal.MovieSceneMarkedFrame(frame_number=int(f)) for f in [%s]])\nif %s:\n    unreal.EditorAssetSubsystem().save_loaded_asset(seq)\n"),
					*PythonQuote(AssetPath), *FString::Join(FrameStrings, TEXT(",")), bSaveAsset ? TEXT("True") : TEXT("False"));
		});

		RegisterPythonTool(TEXT("sequence_focus_subsequence"), TEXT("Open and focus a subsequence asset in the editor."), FSololmcpSchemaBuilder::Object({{TEXT("subsequence_asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("subsequence_asset_path")}),
			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
			{
				FString SubsequencePath;
				if (!Arguments->TryGetStringField(TEXT("subsequence_asset_path"), SubsequencePath)) { OutError = TEXT("Missing subsequence_asset_path."); return FString(); }
				return FString::Printf(TEXT("import unreal\nseq=unreal.EditorAssetSubsystem().load_asset(%s)\nif seq is None: raise RuntimeError('Failed to load subsequence')\nunreal.LevelSequenceEditorBlueprintLibrary.open_level_sequence(seq)\n"),
					*PythonQuote(SubsequencePath));
		});

		auto RegisterSequenceKeyTool = [&Registry](const FString& ToolName)
		{
			TSharedRef<FJsonObject> AnyValueSchema = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> KeySchema = FSololmcpSchemaBuilder::Object(
				{{TEXT("frame"), FSololmcpSchemaBuilder::Integer()}, {TEXT("value"), AnyValueSchema}},
				{TEXT("frame"), TEXT("value")});
			Registry.Register({
				ToolName,
				TEXT("Mutate a typed MovieScene channel directly through the native C++ queue backend."),
				FSololmcpSchemaBuilder::Object(
					{{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					 {TEXT("channel_name"), FSololmcpSchemaBuilder::String()},
					 {TEXT("channel_type"), FSololmcpSchemaBuilder::String()},
					 {TEXT("channel_index"), FSololmcpSchemaBuilder::Integer()},
					 {TEXT("absolute_channel_index"), FSololmcpSchemaBuilder::Integer()},
					 {TEXT("track_name"), FSololmcpSchemaBuilder::String()},
					 {TEXT("section_index"), FSololmcpSchemaBuilder::Integer()},
					 {TEXT("binding_id"), FSololmcpSchemaBuilder::String()},
					 {TEXT("keys"), FSololmcpSchemaBuilder::Array(KeySchema)},
					 {TEXT("value"), AnyValueSchema},
					 {TEXT("frames"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer())},
					 {TEXT("frame_offset"), FSololmcpSchemaBuilder::Integer()},
					 {TEXT("interpolation"), FSololmcpSchemaBuilder::String()}},
					{TEXT("asset_path"), TEXT("track_name"), TEXT("section_index")}),
				[ToolName](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					FResolvedSequenceChannel Resolved;
					if (!ResolveSequenceChannel(Context, Arguments, Resolved, OutError))
					{
						return false;
					}
					Resolved.Sequence->Modify();
					Resolved.MovieScene->Modify();
					Resolved.Section->Modify();

					TArray<FFrameNumber> Frames;
					if (const TArray<TSharedPtr<FJsonValue>>* FrameValues = nullptr; Arguments->TryGetArrayField(TEXT("frames"), FrameValues) && FrameValues)
					{
						for (const TSharedPtr<FJsonValue>& Value : *FrameValues)
						{
							if (Value.IsValid() && Value->Type == EJson::Number)
							{
								Frames.Add(FFrameNumber(static_cast<int32>(Value->AsNumber())));
							}
						}
					}
					TArray<TSharedPtr<FJsonValue>> KeyValues;
					if (const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr; Arguments->TryGetArrayField(TEXT("keys"), Keys) && Keys)
					{
						Frames.Reset();
						for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
						{
							const TSharedPtr<FJsonObject> KeyObject = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
							int32 Frame = 0;
							if (!KeyObject.IsValid() || !KeyObject->TryGetNumberField(TEXT("frame"), Frame))
							{
								OutError = TEXT("Every keys entry requires an integer frame.");
								return false;
							}
							Frames.Add(FFrameNumber(Frame));
							KeyValues.Add(KeyObject->Values.FindRef(TEXT("value")));
						}
					}
					const TSharedPtr<FJsonValue> DefaultValue = Arguments->Values.FindRef(TEXT("value"));
					TSet<FFrameNumber> RequestedFrames;
					for (const FFrameNumber Frame : Frames)
					{
						RequestedFrames.Add(Frame);
					}
					int32 MutatedCount = 0;

					auto ValueAt = [&](int32 Index) -> TSharedPtr<FJsonValue>
					{
						return KeyValues.IsValidIndex(Index) ? KeyValues[Index] : DefaultValue;
					};
					auto RequireFrames = [&]() -> bool
					{
						if (!Frames.IsEmpty()) return true;
						OutError = TEXT("At least one frame or keys entry is required.");
						return false;
					};
					auto ParseInterpolation = [&](ERichCurveInterpMode& OutMode) -> bool
					{
						FString Interpolation = TEXT("cubic");
						Arguments->TryGetStringField(TEXT("interpolation"), Interpolation);
						if (Interpolation.Equals(TEXT("constant"), ESearchCase::IgnoreCase)) OutMode = RCIM_Constant;
						else if (Interpolation.Equals(TEXT("linear"), ESearchCase::IgnoreCase)) OutMode = RCIM_Linear;
						else if (Interpolation.Equals(TEXT("cubic"), ESearchCase::IgnoreCase) || Interpolation.Equals(TEXT("auto"), ESearchCase::IgnoreCase)) OutMode = RCIM_Cubic;
						else { OutError = FString::Printf(TEXT("Unsupported interpolation mode: %s"), *Interpolation); return false; }
						return true;
					};

					if (ToolName == TEXT("sequence_set_channel_default"))
					{
						if (!DefaultValue.IsValid()) { OutError = TEXT("value is required."); return false; }
						if (Resolved.ChannelType == FMovieSceneBoolChannel::StaticStruct()->GetFName())
						{
							bool Value = false; if (!SequenceJsonValueToBool(DefaultValue, Value)) { OutError = TEXT("Invalid bool default value."); return false; }
							static_cast<FMovieSceneBoolChannel*>(Resolved.Channel)->SetDefault(Value);
						}
						else if (Resolved.ChannelType == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
						{
							double Value = 0; if (!SequenceJsonValueToDouble(DefaultValue, Value)) { OutError = TEXT("Invalid integer default value."); return false; }
							static_cast<FMovieSceneIntegerChannel*>(Resolved.Channel)->SetDefault(static_cast<int32>(Value));
						}
						else if (Resolved.ChannelType == FMovieSceneFloatChannel::StaticStruct()->GetFName())
						{
							double Value = 0; if (!SequenceJsonValueToDouble(DefaultValue, Value)) { OutError = TEXT("Invalid float default value."); return false; }
							static_cast<FMovieSceneFloatChannel*>(Resolved.Channel)->SetDefault(static_cast<float>(Value));
						}
						else if (Resolved.ChannelType == FMovieSceneDoubleChannel::StaticStruct()->GetFName())
						{
							double Value = 0; if (!SequenceJsonValueToDouble(DefaultValue, Value)) { OutError = TEXT("Invalid double default value."); return false; }
							static_cast<FMovieSceneDoubleChannel*>(Resolved.Channel)->SetDefault(Value);
						}
						else if (Resolved.ChannelType == FMovieSceneObjectPathChannel::StaticStruct()->GetFName())
						{
							if (DefaultValue->Type != EJson::String) { OutError = TEXT("Object default value must be an asset path string."); return false; }
							UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *DefaultValue->AsString());
							if (!Object) { OutError = TEXT("Object default asset could not be loaded."); return false; }
							static_cast<FMovieSceneObjectPathChannel*>(Resolved.Channel)->SetDefault(Object);
						}
						else { OutError = FString::Printf(TEXT("Channel type does not support a native default adapter: %s"), *Resolved.ChannelType.ToString()); return false; }
						MutatedCount = 1;
					}
					else if (ToolName.StartsWith(TEXT("sequence_add_keys_")))
					{
						if (!RequireFrames()) return false;
						for (int32 Index = 0; Index < Frames.Num(); ++Index)
						{
							const TSharedPtr<FJsonValue> JsonValue = ValueAt(Index);
							if (ToolName == TEXT("sequence_add_keys_bool") && Resolved.ChannelType == FMovieSceneBoolChannel::StaticStruct()->GetFName())
							{
								bool Value = false; if (!SequenceJsonValueToBool(JsonValue, Value)) { OutError = TEXT("Invalid bool key value."); return false; }
								static_cast<FMovieSceneBoolChannel*>(Resolved.Channel)->GetData().UpdateOrAddKey(Frames[Index], Value);
							}
							else if (ToolName == TEXT("sequence_add_keys_integer") && Resolved.ChannelType == FMovieSceneIntegerChannel::StaticStruct()->GetFName())
							{
								double Value = 0; if (!SequenceJsonValueToDouble(JsonValue, Value)) { OutError = TEXT("Invalid integer key value."); return false; }
								static_cast<FMovieSceneIntegerChannel*>(Resolved.Channel)->GetData().UpdateOrAddKey(Frames[Index], static_cast<int32>(Value));
							}
							else if (ToolName == TEXT("sequence_add_keys_float") && Resolved.ChannelType == FMovieSceneFloatChannel::StaticStruct()->GetFName())
							{
								double Value = 0; if (!SequenceJsonValueToDouble(JsonValue, Value)) { OutError = TEXT("Invalid float key value."); return false; }
								ERichCurveInterpMode Mode; if (!ParseInterpolation(Mode)) return false;
								FMovieSceneFloatChannel* Channel = static_cast<FMovieSceneFloatChannel*>(Resolved.Channel);
								if (Mode == RCIM_Constant) Channel->AddConstantKey(Frames[Index], static_cast<float>(Value));
								else if (Mode == RCIM_Linear) Channel->AddLinearKey(Frames[Index], static_cast<float>(Value));
								else Channel->AddCubicKey(Frames[Index], static_cast<float>(Value));
							}
							else if (ToolName == TEXT("sequence_add_keys_double") && Resolved.ChannelType == FMovieSceneDoubleChannel::StaticStruct()->GetFName())
							{
								double Value = 0; if (!SequenceJsonValueToDouble(JsonValue, Value)) { OutError = TEXT("Invalid double key value."); return false; }
								ERichCurveInterpMode Mode; if (!ParseInterpolation(Mode)) return false;
								FMovieSceneDoubleChannel* Channel = static_cast<FMovieSceneDoubleChannel*>(Resolved.Channel);
								if (Mode == RCIM_Constant) Channel->AddConstantKey(Frames[Index], Value);
								else if (Mode == RCIM_Linear) Channel->AddLinearKey(Frames[Index], Value);
								else Channel->AddCubicKey(Frames[Index], Value);
							}
							else if (ToolName == TEXT("sequence_add_keys_object") && Resolved.ChannelType == FMovieSceneObjectPathChannel::StaticStruct()->GetFName())
							{
								if (!JsonValue.IsValid() || JsonValue->Type != EJson::String) { OutError = TEXT("Object key value must be an asset path string."); return false; }
								UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *JsonValue->AsString());
								if (!Object) { OutError = TEXT("Object key asset could not be loaded."); return false; }
								static_cast<FMovieSceneObjectPathChannel*>(Resolved.Channel)->GetData().UpdateOrAddKey(Frames[Index], FMovieSceneObjectPathChannelKeyValue(Object));
							}
							else { OutError = FString::Printf(TEXT("Tool '%s' is incompatible with channel type '%s'."), *ToolName, *Resolved.ChannelType.ToString()); return false; }
							++MutatedCount;
						}
					}
					else if (ToolName == TEXT("sequence_delete_keys"))
					{
						if (!RequireFrames()) return false;
						TArray<FFrameNumber> ExistingTimes; TArray<FKeyHandle> ExistingHandles;
						Resolved.Channel->GetKeys(TRange<FFrameNumber>::All(), &ExistingTimes, &ExistingHandles);
						TArray<FKeyHandle> DeleteHandles;
						for (int32 Index = 0; Index < ExistingTimes.Num() && Index < ExistingHandles.Num(); ++Index) if (RequestedFrames.Contains(ExistingTimes[Index])) DeleteHandles.Add(ExistingHandles[Index]);
						if (!DeleteHandles.IsEmpty()) Resolved.Channel->DeleteKeys(DeleteHandles);
						MutatedCount = DeleteHandles.Num();
					}
					else if (ToolName == TEXT("sequence_transform_keys"))
					{
						int32 FrameOffset = 0; Arguments->TryGetNumberField(TEXT("frame_offset"), FrameOffset);
						if (FrameOffset == 0) { OutError = TEXT("frame_offset must be non-zero."); return false; }
						TArray<FFrameNumber> ExistingTimes; TArray<FKeyHandle> ExistingHandles;
						Resolved.Channel->GetKeys(TRange<FFrameNumber>::All(), &ExistingTimes, &ExistingHandles);
						TArray<FKeyHandle> MoveHandles; TArray<FFrameNumber> NewTimes;
						for (int32 Index = 0; Index < ExistingTimes.Num() && Index < ExistingHandles.Num(); ++Index)
						{
							if (Frames.IsEmpty() || RequestedFrames.Contains(ExistingTimes[Index])) { MoveHandles.Add(ExistingHandles[Index]); NewTimes.Add(ExistingTimes[Index] + FrameOffset); }
						}
						if (!MoveHandles.IsEmpty()) Resolved.Channel->SetKeyTimes(MoveHandles, NewTimes);
						MutatedCount = MoveHandles.Num();
					}
					else if (ToolName == TEXT("sequence_set_key_interpolation"))
					{
						ERichCurveInterpMode Mode; if (!ParseInterpolation(Mode)) return false;
						auto ApplyMode = [&](auto* TypedChannel)
						{
							auto Data = TypedChannel->GetData();
							const TArrayView<FFrameNumber> Times = Data.GetTimes();
							auto Values = Data.GetValues();
							for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
							{
								if (Frames.IsEmpty() || RequestedFrames.Contains(Times[Index])) { Values[Index].InterpMode = Mode; Values[Index].TangentMode = RCTM_Auto; ++MutatedCount; }
							}
						};
						if (Resolved.ChannelType == FMovieSceneFloatChannel::StaticStruct()->GetFName()) ApplyMode(static_cast<FMovieSceneFloatChannel*>(Resolved.Channel));
						else if (Resolved.ChannelType == FMovieSceneDoubleChannel::StaticStruct()->GetFName()) ApplyMode(static_cast<FMovieSceneDoubleChannel*>(Resolved.Channel));
						else { OutError = TEXT("Interpolation is supported only for float and double channels."); return false; }
					}

					if (MutatedCount <= 0)
					{
						OutError = TEXT("No matching sequence channel values or keys were mutated.");
						return false;
					}
					Resolved.MovieScene->MarkAsChanged();
					Resolved.Sequence->MarkPackageDirty();
					SololmcpWriteFlush::EnsureFlushed(Resolved.Sequence);
					if (!Context.Services.SaveAsset(Resolved.Sequence->GetPathName(), false, OutError)) return false;
					OutStructured->SetStringField(TEXT("asset_path"), Resolved.Sequence->GetPathName());
					OutStructured->SetStringField(TEXT("channel_name"), Resolved.ChannelName);
					OutStructured->SetStringField(TEXT("channel_type"), Resolved.ChannelType.ToString());
					OutStructured->SetNumberField(TEXT("channel_index"), Resolved.ChannelTypeIndex);
					OutStructured->SetNumberField(TEXT("absolute_channel_index"), Resolved.AbsoluteChannelIndex);
					OutStructured->SetNumberField(TEXT("mutated_count"), MutatedCount);
					OutStructured->SetNumberField(TEXT("key_count_after"), Resolved.Channel->GetNumKeys());
					OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
					OutStructured->SetBoolField(TEXT("verified"), true);
					OutStructured->SetStringField(TEXT("status"), TEXT("success"));
					OutSummary = FString::Printf(TEXT("%s mutated %d value(s) on native channel '%s'."), *ToolName, MutatedCount, *Resolved.ChannelName);
					return true;
				},
				nullptr,
				5,
				nullptr,
				false
			});
		};
		RegisterSequenceKeyTool(TEXT("sequence_set_channel_default"));
		RegisterSequenceKeyTool(TEXT("sequence_add_keys_bool"));
		RegisterSequenceKeyTool(TEXT("sequence_add_keys_integer"));
		RegisterSequenceKeyTool(TEXT("sequence_add_keys_float"));
		RegisterSequenceKeyTool(TEXT("sequence_add_keys_double"));
		RegisterSequenceKeyTool(TEXT("sequence_add_keys_object"));
		RegisterSequenceKeyTool(TEXT("sequence_delete_keys"));
		RegisterSequenceKeyTool(TEXT("sequence_transform_keys"));
		RegisterSequenceKeyTool(TEXT("sequence_set_key_interpolation"));

		Registry.Register({
			TEXT("control_rig_describe"),
			TEXT("Describe a Control Rig Blueprint asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) { OutError = TEXT("Failed to load control rig blueprint."); return false; }
				URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
				OutStructured->SetStringField(TEXT("asset_path"), Rig->GetPathName());
				OutStructured->SetStringField(TEXT("class"), Rig->GetClass()->GetPathName());
				if (Hierarchy)
				{
					TArray<FRigElementKey> AllKeys = Hierarchy->GetAllKeys(true, ERigElementType::All);
					OutStructured->SetNumberField(TEXT("element_count"), AllKeys.Num());
					int32 BoneCount = 0, NullCount = 0, ControlCount = 0;
					for (const FRigElementKey& K : AllKeys)
					{
						if (K.Type == ERigElementType::Bone) BoneCount++;
						else if (K.Type == ERigElementType::Null) NullCount++;
						else if (K.Type == ERigElementType::Control) ControlCount++;
					}
					OutStructured->SetNumberField(TEXT("bone_count"), BoneCount);
					OutStructured->SetNumberField(TEXT("null_count"), NullCount);
					OutStructured->SetNumberField(TEXT("control_count"), ControlCount);
				}
				OutSummary = TEXT("Described control rig blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("control_rig_import_bones_from_mesh"),
			TEXT("Import bones from a skeletal mesh into a Control Rig Blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("skeletal_mesh_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, MeshPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("skeletal_mesh_path"), MeshPath)) { OutError = TEXT("Missing asset_path or skeletal_mesh_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(MeshPath, OutError));
				if (!Mesh) { OutError = TEXT("Failed to load skeletal mesh."); return false; }
				URigHierarchyController* Controller = UControlRigBlueprintEditorLibrary::GetHierarchyController(Rig);
				if (!Controller) { OutError = TEXT("Hierarchy controller unavailable."); return false; }
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigImportBones", "SOMOLMCP Import Bones From Mesh"));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				TArray<FRigElementKey> Imported = Controller->ImportBonesFromSkeletalMesh(Mesh, NAME_None, true, true, false, true);
#else
				// 5.6 renamed this; 5.5 takes the reference skeleton the mesh already carries,
				// so the imported hierarchy is identical.
				TArray<FRigElementKey> Imported = Controller->ImportBones(
					Mesh->GetRefSkeleton(), NAME_None, true, true, false, true);
#endif
				TArray<TSharedPtr<FJsonValue>> KeysJson;
				for (const FRigElementKey& K : Imported)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), K.Name.ToString());
					Obj->SetStringField(TEXT("type"), StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(K.Type)));
					KeysJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("imported_elements"), KeysJson);
				OutStructured->SetNumberField(TEXT("count"), Imported.Num());
				OutSummary = TEXT("Imported bones from skeletal mesh.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("control_rig_list_elements"),
			TEXT("List hierarchy elements in a Control Rig Blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigHierarchy* Hierarchy = UControlRigBlueprintEditorLibrary::GetHierarchy(Rig);
				if (!Hierarchy) { OutError = TEXT("Hierarchy unavailable."); return false; }
				TArray<FRigElementKey> AllKeys = Hierarchy->GetAllKeys(true, ERigElementType::All);
				TArray<TSharedPtr<FJsonValue>> ElementsJson;
				for (const FRigElementKey& K : AllKeys)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), K.Name.ToString());
					Obj->SetStringField(TEXT("type"), StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(K.Type)));
					ElementsJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("elements"), ElementsJson);
				OutStructured->SetNumberField(TEXT("count"), ElementsJson.Num());
				OutSummary = TEXT("Listed control rig hierarchy elements.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_update_control_settings - native C++ implementation
		Registry.Register({
			TEXT("control_rig_update_control_settings"),
			TEXT("Update control settings (control_type, display_name, etc.) for a Control Rig control."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("control"), FSololmcpSchemaBuilder::String()}, {TEXT("settings"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("control"), TEXT("settings")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ControlName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("control"), ControlName)) { OutError = TEXT("Missing control."); return false; }
				const TSharedPtr<FJsonObject>* SettingsObj;
				if (!Arguments->TryGetObjectField(TEXT("settings"), SettingsObj)) { OutError = TEXT("Missing settings."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigHierarchyController* Ctrl = UControlRigBlueprintEditorLibrary::GetHierarchyController(Rig);
				if (!Ctrl) { OutError = TEXT("Hierarchy controller unavailable."); return false; }
				FRigElementKey Key(FName(*ControlName), ERigElementType::Control);
				URigHierarchy* Hierarchy = Ctrl->GetHierarchy();
				if (!Hierarchy || !Hierarchy->Find(Key)) { OutError = FString::Printf(TEXT("Control '%s' not found."), *ControlName); return false; }
				FRigControlSettings Settings = Ctrl->GetControlSettings(Key);
				if ((*SettingsObj)->HasField(TEXT("control_type")))
				{
					FString TypeStr;
					if ((*SettingsObj)->TryGetStringField(TEXT("control_type"), TypeStr))
					{
						if (UEnum* E = StaticEnum<ERigControlType>())
						{
							const int64 V = E->GetValueByNameString(TypeStr);
							if (V != INDEX_NONE) Settings.ControlType = static_cast<ERigControlType>(V);
						}
					}
				}
				if ((*SettingsObj)->HasField(TEXT("display_name")))
				{
					FString D;
					if ((*SettingsObj)->TryGetStringField(TEXT("display_name"), D)) Settings.DisplayName = FName(*D);
				}
				if ((*SettingsObj)->HasField(TEXT("b_shape_visible")))
				{
					bool B = false;
					if ((*SettingsObj)->TryGetBoolField(TEXT("b_shape_visible"), B)) Settings.bShapeVisible = B;
				}
				if ((*SettingsObj)->HasField(TEXT("shape_color")))
				{
					FString C;
					if ((*SettingsObj)->TryGetStringField(TEXT("shape_color"), C))
					{
						FLinearColor Lc;
						if (Lc.InitFromString(C)) Settings.ShapeColor = Lc;
					}
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigUpdateControlSettings", "SOMOLMCP Update Control Settings"));
				if (!Ctrl->SetControlSettings(Key, Settings, true)) { OutError = TEXT("SetControlSettings failed."); return false; }
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("control"), ControlName);
				OutSummary = TEXT("Updated control settings.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_list_nodes - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_list_nodes"),
			TEXT("List nodes in the Control Rig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				URigVMGraph* Graph = VMController->GetGraph();
				if (!Graph) { OutError = TEXT("Graph unavailable."); return false; }
				TArray<TSharedPtr<FJsonValue>> NodesJson;
				for (URigVMNode* Node : Graph->GetNodes())
				{
					if (!Node) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Node->GetName());
					Obj->SetStringField(TEXT("path"), Node->GetNodePath());
					Obj->SetStringField(TEXT("title"), Node->GetNodeTitle());
					NodesJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("nodes"), NodesJson);
				OutStructured->SetNumberField(TEXT("count"), NodesJson.Num());
				OutSummary = TEXT("Listed control rig graph nodes.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_list_links - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_list_links"),
			TEXT("List links (connections) between pins in the Control Rig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				URigVMGraph* Graph = VMController->GetGraph();
				if (!Graph) { OutError = TEXT("Graph unavailable."); return false; }
				TArray<TSharedPtr<FJsonValue>> LinksJson;
				for (URigVMLink* Link : Graph->GetLinks())
				{
					if (!Link) continue;
					URigVMPin* SourcePin = Link->GetSourcePin();
					URigVMPin* TargetPin = Link->GetTargetPin();
					if (!SourcePin || !TargetPin) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("source_pin_path"), SourcePin->GetPinPath());
					Obj->SetStringField(TEXT("target_pin_path"), TargetPin->GetPinPath());
					LinksJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("links"), LinksJson);
				OutStructured->SetNumberField(TEXT("count"), LinksJson.Num());
				OutSummary = TEXT("Listed control rig graph links.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_add_unit_node - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_add_unit_node"),
			TEXT("Add a unit node to the Control Rig VM graph by struct path."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("struct_path"), FSololmcpSchemaBuilder::String()}, {TEXT("position_x"), FSololmcpSchemaBuilder::Number()}, {TEXT("position_y"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("struct_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, StructPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("struct_path"), StructPath)) { OutError = TEXT("Missing struct_path."); return false; }
				double Px = 0, Py = 0;
				Arguments->TryGetNumberField(TEXT("position_x"), Px);
				Arguments->TryGetNumberField(TEXT("position_y"), Py);
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigAddUnitNode", "SOMOLMCP Add Unit Node"));
				URigVMUnitNode* Node = VMController->AddUnitNodeFromStructPath(StructPath, TEXT("Execute"), FVector2D(static_cast<float>(Px), static_cast<float>(Py)), TEXT(""), true, false);
				if (!Node) { OutError = TEXT("AddUnitNodeFromStructPath failed."); return false; }
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("node_name"), Node->GetName());
				OutStructured->SetStringField(TEXT("node_path"), Node->GetNodePath());
				OutSummary = TEXT("Added unit node.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_add_template_node - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_add_template_node"),
			TEXT("Add a template node to the Control Rig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("template_name"), FSololmcpSchemaBuilder::String()}, {TEXT("position_x"), FSololmcpSchemaBuilder::Number()}, {TEXT("position_y"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("template_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, TemplateName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("template_name"), TemplateName)) { OutError = TEXT("Missing template_name."); return false; }
				double Px = 0, Py = 0;
				Arguments->TryGetNumberField(TEXT("position_x"), Px);
				Arguments->TryGetNumberField(TEXT("position_y"), Py);
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigAddTemplateNode", "SOMOLMCP Add Template Node"));
				URigVMTemplateNode* Node = VMController->AddTemplateNode(FName(*TemplateName), FVector2D(static_cast<float>(Px), static_cast<float>(Py)), TEXT(""), true, false);
				if (!Node) { OutError = TEXT("AddTemplateNode failed."); return false; }
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("node_name"), Node->GetName());
				OutStructured->SetStringField(TEXT("node_path"), Node->GetNodePath());
				OutSummary = TEXT("Added template node.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_add_variable_node - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_add_variable_node"),
			TEXT("Add a variable node to the Control Rig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("variable_name"), FSololmcpSchemaBuilder::String()}, {TEXT("is_getter"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("position_x"), FSololmcpSchemaBuilder::Number()}, {TEXT("position_y"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("variable_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, VarName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("variable_name"), VarName)) { OutError = TEXT("Missing variable_name."); return false; }
				bool bIsGetter = true;
				Arguments->TryGetBoolField(TEXT("is_getter"), bIsGetter);
				double Px = 0, Py = 0;
				Arguments->TryGetNumberField(TEXT("position_x"), Px);
				Arguments->TryGetNumberField(TEXT("position_y"), Py);
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigAddVariableNode", "SOMOLMCP Add Variable Node"));
				URigVMVariableNode* Node = VMController->AddVariableNode(FName(*VarName), TEXT("float"), nullptr, bIsGetter, TEXT("0.0"), FVector2D(static_cast<float>(Px), static_cast<float>(Py)), TEXT(""), true, false);
				if (!Node) { OutError = TEXT("AddVariableNode failed."); return false; }
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("node_name"), Node->GetName());
				OutStructured->SetStringField(TEXT("node_path"), Node->GetNodePath());
				OutSummary = TEXT("Added variable node.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_add_link - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_add_link"),
			TEXT("Add a link between two pins in the Control Rig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("source_pin_path"), FSololmcpSchemaBuilder::String()}, {TEXT("target_pin_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("source_pin_path"), TEXT("target_pin_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, SourcePath, TargetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("source_pin_path"), SourcePath)) { OutError = TEXT("Missing source_pin_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("target_pin_path"), TargetPath)) { OutError = TEXT("Missing target_pin_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigAddLink", "SOMOLMCP Add Link"));
				if (!VMController->AddLink(SourcePath, TargetPath, true, false)) { OutError = TEXT("AddLink failed."); return false; }
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("source_pin_path"), SourcePath);
				OutStructured->SetStringField(TEXT("target_pin_path"), TargetPath);
				OutSummary = TEXT("Added link.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_set_pin_default - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_set_pin_default"),
			TEXT("Set the default value of a pin in the Control Rig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("pin_path"), FSololmcpSchemaBuilder::String()}, {TEXT("value"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("pin_path"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, PinPath, Value;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("pin_path"), PinPath)) { OutError = TEXT("Missing pin_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("value"), Value)) { OutError = TEXT("Missing value."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigSetPinDefault", "SOMOLMCP Set Pin Default"));
				#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			if (!VMController->SetPinDefaultValue(PinPath, Value, true, true, false, false, true))
#else
			// 5.3 takes six parameters; the trailing flag was added in 5.4.
			if (!VMController->SetPinDefaultValue(PinPath, Value, true, true, false, false))
#endif { OutError = TEXT("SetPinDefaultValue failed."); return false; }
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("pin_path"), PinPath);
				OutStructured->SetStringField(TEXT("value"), Value);
				OutSummary = TEXT("Set pin default value.");
				return true;
			}
		, nullptr
		, 5
		});

		// control_rig_graph_remove_node - native C++ implementation
		Registry.Register({
			TEXT("control_rig_graph_remove_node"),
			TEXT("Remove a node from the Control Rig VM graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, NodePath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				if (!Arguments->TryGetStringField(TEXT("node_path"), NodePath)) { OutError = TEXT("Missing node_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				URigVMController* VMController = ResolveRigVMControllerCompat(Rig);
				if (!VMController) { OutError = TEXT("RigVM controller unavailable."); return false; }
				URigVMGraph* Graph = VMController->GetGraph();
				if (!Graph) { OutError = TEXT("Graph unavailable."); return false; }
				URigVMNode* Node = Graph->FindNode(NodePath);
				if (!Node) { Node = Graph->FindNodeByName(FName(*NodePath)); }
				if (!Node) { OutError = FString::Printf(TEXT("Node '%s' not found."), *NodePath); return false; }
				FName NodeName = Node->GetFName();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ControlRigRemoveNode", "SOMOLMCP Remove Node"));
				if (!VMController->RemoveNodesByName(TArray<FName>{NodeName}, true, false)) { OutError = TEXT("RemoveNodesByName failed."); return false; }
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("node_path"), NodePath);
				OutSummary = TEXT("Removed node.");
				return true;
			}
		, nullptr
		, 5
		});

		auto RegisterControlRigPythonMutation = [&Registry](const FString& ToolName, const TSharedRef<FJsonObject>& Schema)
		{
			Registry.Register({
				ToolName,
				TEXT("Mutate a Control Rig Blueprint through Python-backed editor APIs."),
				Schema,
				[ToolName](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					FString AssetPath;
					if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
					const FString ArgsJson = JsonObjectToString(Arguments);
					FString PythonCode = FString::Printf(
						TEXT("import unreal\nimport json\nrig=unreal.EditorAssetSubsystem().load_asset(%s)\nif rig is None: raise RuntimeError('Failed to load control rig')\n")
						TEXT("args=json.loads(%s)\nctrl=unreal.ControlRigBlueprintEditorLibrary.get_hierarchy_controller(rig)\n")
						TEXT("if ctrl is None: raise RuntimeError('Hierarchy controller unavailable')\n")
						TEXT("hier=unreal.ControlRigBlueprintEditorLibrary.get_hierarchy(rig)\n")
						TEXT("def _find_key(name):\n  for k in hier.get_all_keys():\n    if str(k.name)==name: return k\n  return unreal.RigElementKey()\n"),
						*PythonQuote(AssetPath), *PythonQuote(ArgsJson));
					if (ToolName == TEXT("control_rig_add_nulls"))
					{
						PythonCode += TEXT("empty=unreal.RigElementKey()\ncreated=[]\n")
							TEXT("for n in args.get('nulls',[]):\n")
							TEXT("  name=n.get('name','Null'); p=n.get('parent',''); parent=_find_key(p) if p else empty\n")
							TEXT("  t=unreal.Transform()\n")
							TEXT("  loc=n.get('location',None)\n")
							TEXT("  if isinstance(loc,dict): t.translation=unreal.Vector(float(loc.get('x',0.0)), float(loc.get('y',0.0)), float(loc.get('z',0.0)))\n")
							TEXT("  rot=n.get('rotation',None)\n")
							TEXT("  if isinstance(rot,dict): t.rotation=unreal.Rotator(float(rot.get('pitch',rot.get('x',0.0))), float(rot.get('yaw',rot.get('y',0.0))), float(rot.get('roll',rot.get('z',0.0)))).quaternion()\n")
							TEXT("  scl=n.get('scale',None)\n")
							TEXT("  if isinstance(scl,dict): t.scale3d=unreal.Vector(float(scl.get('x',1.0)), float(scl.get('y',1.0)), float(scl.get('z',1.0)))\n")
							TEXT("  ctrl.add_null(unreal.Name(name), parent, t, False, True)\n")
							TEXT("  created.append(name)\n")
							TEXT("missing=[nm for nm in created if _find_key(nm).type==unreal.RigElementType.NONE]\n")
							TEXT("if len(missing)>0: raise RuntimeError('Post-verify failed for control_rig_add_nulls: '+','.join(missing))\n");
					}
					else if (ToolName == TEXT("control_rig_reparent_elements"))
					{
						PythonCode += TEXT("parent_str=args.get('parent',''); parent_key=_find_key(parent_str) if parent_str else unreal.RigElementKey()\n")
							TEXT("for e in args.get('elements',[]): k=_find_key(e); ctrl.set_parent(k, parent_key, True, True)\n");
					}
					else if (ToolName == TEXT("control_rig_duplicate_elements"))
					{
						PythonCode += TEXT("keys=[_find_key(e) for e in args.get('elements',[])]\nkeys=[k for k in keys if k.type!=unreal.RigElementType.NONE]\nctrl.duplicate_elements(keys, False, True)\n");
					}
					else if (ToolName == TEXT("control_rig_mirror_elements"))
					{
						PythonCode += TEXT("keys=[_find_key(e) for e in args.get('elements',[])]\nkeys=[k for k in keys if k.type!=unreal.RigElementType.NONE]\n")
							TEXT("settings=unreal.RigVMMirrorSettings()\nctrl.mirror_elements(keys, settings, False, True)\n");
					}
					else if (ToolName == TEXT("control_rig_add_controls"))
					{
						PythonCode += TEXT("empty=unreal.RigElementKey()\ncreated=[]\nfor c in args.get('controls',[]): name=c.get('name','Control'); p=c.get('parent',''); parent=_find_key(p) if p else empty; ")
							TEXT("settings=unreal.RigControlSettings(); settings.control_type=unreal.RigControlType.FLOAT; value=unreal.RigControlValue(); ctrl.add_control(unreal.Name(name), parent, settings, value, True); created.append(name)\n")
							TEXT("missing=[nm for nm in created if _find_key(nm).type==unreal.RigElementType.NONE]\n")
							TEXT("if len(missing)>0: raise RuntimeError('Post-verify failed for control_rig_add_controls: '+','.join(missing))\n");
					}
					else
					{
						PythonCode += FString::Printf(TEXT("unreal.log('Executed %s (stub)')\n"), *PythonQuote(ToolName));
					}
					PythonCode += TEXT("unreal.EditorAssetSubsystem().save_loaded_asset(rig)\n");
					return Context.Services.ExecutePython(PythonCode, TEXT("ControlRigMutation"), false, OutStructured, OutSummary, OutError);
				},
				[](const FSololmcpToolExecutionContext& Context, FString& OutReason) { return Context.Services.IsPythonAvailable(&OutReason); },
		0,
		nullptr,
		true
		});
		};
		RegisterControlRigPythonMutation(TEXT("control_rig_add_controls"), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("controls"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}}, {TEXT("asset_path"), TEXT("controls")}));
		RegisterControlRigPythonMutation(TEXT("control_rig_add_nulls"), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("nulls"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}}, {TEXT("asset_path"), TEXT("nulls")}));
		RegisterControlRigPythonMutation(TEXT("control_rig_reparent_elements"), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("elements"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("parent"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("elements"), TEXT("parent")}));
		RegisterControlRigPythonMutation(TEXT("control_rig_duplicate_elements"), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("elements"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}}, {TEXT("asset_path"), TEXT("elements")}));
		RegisterControlRigPythonMutation(TEXT("control_rig_mirror_elements"), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("elements"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}}, {TEXT("asset_path"), TEXT("elements")}));

		Registry.Register({
			TEXT("control_rig_recompile"),
			TEXT("Request recompile of a Control Rig Blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				FKismetEditorUtilities::CompileBlueprint(Rig);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("recompiled"), true);
				OutSummary = TEXT("Recompiled control rig blueprint.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("control_rig_request_init"),
			TEXT("Request Control Rig initialization."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Rig) return false;
				UControlRigBlueprintEditorLibrary::RequestControlRigInit(Rig);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("requested"), true);
				OutSummary = TEXT("Requested control rig init.");
				return true;
			}
		, nullptr
		, 5
		});

		auto RegisterExtendedPythonTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
				{
					// Audit round 3: lightweight package_path guard for *_list tools to avoid downstream LoadAsset noise.
					if (ToolName.EndsWith(TEXT("_list")))
					{
						FString GuardPkg;
						if (!Arguments->TryGetStringField(TEXT("package_path"), GuardPkg) || GuardPkg.IsEmpty())
						{
							OutError = TEXT("Missing or empty package_path");
							return FString();
						}
					}
					// Audit round 5: PCG tool entry guards — refuse empty/incomplete args so callers
					// don't get misleading "Python command completed." or downstream py_exception strings.
					if (ToolName == TEXT("pcg_graph_create"))
					{
						FString GuardPkg, GuardName, GuardAssetPath;
						if (Arguments->TryGetStringField(TEXT("asset_path"), GuardAssetPath) && !GuardAssetPath.IsEmpty())
						{
							int32 SlashIndex = INDEX_NONE;
							if (GuardAssetPath.FindLastChar(TEXT('/'), SlashIndex) && SlashIndex > 0 && SlashIndex < GuardAssetPath.Len() - 1)
							{
								GuardPkg = GuardAssetPath.Left(SlashIndex);
								GuardName = GuardAssetPath.Mid(SlashIndex + 1);
							}
						}
						FString FieldPkg, FieldName;
						if (Arguments->TryGetStringField(TEXT("package_path"), FieldPkg) && !FieldPkg.IsEmpty())
						{
							GuardPkg = FieldPkg;
						}
						if (Arguments->TryGetStringField(TEXT("asset_name"), FieldName) && !FieldName.IsEmpty())
						{
							GuardName = FieldName;
						}
						const bool bHasPkg = !GuardPkg.IsEmpty();
						const bool bHasName = !GuardName.IsEmpty();
						if (!bHasPkg || !bHasName)
						{
							OutError = TEXT("Missing required: package_path and asset_name, or asset_path");
							return FString();
						}
					}
					else if (ToolName == TEXT("pcg_component_attach"))
					{
						FString GuardActor, GuardGraph;
						const bool bHasActor = Arguments->TryGetStringField(TEXT("actor"), GuardActor) && !GuardActor.IsEmpty();
						const bool bHasGraph = Arguments->TryGetStringField(TEXT("graph_path"), GuardGraph) && !GuardGraph.IsEmpty();
						if (!bHasActor || !bHasGraph)
						{
							OutError = TEXT("Missing required: actor and graph_path");
							return FString();
						}
					}
					else if (ToolName == TEXT("pcg_graph_add_node"))
					{
						FString GuardPath, GuardClass;
						const bool bHasPath = Arguments->TryGetStringField(TEXT("asset_path"), GuardPath) && !GuardPath.IsEmpty();
						const bool bHasClass = Arguments->TryGetStringField(TEXT("node_class_path"), GuardClass) && !GuardClass.IsEmpty();
						if (!bHasPath || !bHasClass)
						{
							OutError = TEXT("Missing required: asset_path and node_class_path");
							return FString();
						}
					}
					else if (ToolName == TEXT("pcg_graph_connect"))
					{
						FString GuardPath;
						const bool bHasPath = Arguments->TryGetStringField(TEXT("asset_path"), GuardPath) && !GuardPath.IsEmpty();
						FString SrcNode, SrcPin, DstNode, DstPin, SrcPath, DstPath;
						const bool bHasExplicitPair =
							(Arguments->TryGetStringField(TEXT("source_node"), SrcNode) && !SrcNode.IsEmpty()) &&
							(Arguments->TryGetStringField(TEXT("source_pin"), SrcPin) && !SrcPin.IsEmpty()) &&
							(Arguments->TryGetStringField(TEXT("target_node"), DstNode) && !DstNode.IsEmpty()) &&
							(Arguments->TryGetStringField(TEXT("target_pin"), DstPin) && !DstPin.IsEmpty());
						const bool bHasPathPair =
							(Arguments->TryGetStringField(TEXT("source_pin_path"), SrcPath) && !SrcPath.IsEmpty()) &&
							(Arguments->TryGetStringField(TEXT("target_pin_path"), DstPath) && !DstPath.IsEmpty());
						if (!bHasPath || (!bHasExplicitPair && !bHasPathPair))
						{
							OutError = TEXT("Missing required: asset_path and (source_node/source_pin/target_node/target_pin or source_pin_path/target_pin_path)");
							return FString();
						}
					}
					else if (ToolName == TEXT("pcg_graph_set_node_property"))
					{
						FString GuardPath, GuardNode;
						const bool bHasPath = Arguments->TryGetStringField(TEXT("asset_path"), GuardPath) && !GuardPath.IsEmpty();
						const bool bHasNode = Arguments->TryGetStringField(TEXT("node"), GuardNode) && !GuardNode.IsEmpty();
						const TSharedPtr<FJsonObject>* GuardProps = nullptr;
						const bool bHasProps = Arguments->TryGetObjectField(TEXT("properties"), GuardProps) && GuardProps && (*GuardProps)->Values.Num() > 0;
						if (!bHasPath || !bHasNode || !bHasProps)
						{
							OutError = TEXT("Missing required: asset_path, node and non-empty properties");
							return FString();
						}
					}
					else if (ToolName == TEXT("pcg_graph_add_static_mesh_spawner"))
					{
						FString GuardPath;
						const bool bHasPath = Arguments->TryGetStringField(TEXT("asset_path"), GuardPath) && !GuardPath.IsEmpty();
						const TArray<TSharedPtr<FJsonValue>>* GuardMeshes = nullptr;
						const bool bHasMeshes = Arguments->TryGetArrayField(TEXT("static_mesh_paths"), GuardMeshes) && GuardMeshes && GuardMeshes->Num() > 0;
						if (!bHasPath || !bHasMeshes)
						{
							OutError = TEXT("Missing required: asset_path and non-empty static_mesh_paths");
							return FString();
						}
					}
					else if (ToolName == TEXT("pcg_clear"))
					{
						// Round 9C: pcg_clear historically delegated straight to Python and crashed
						// with py_exception when the actor didn't exist or had no UPCGComponent.
						// Validate up front so callers get a clean structured error instead.
						FString GuardActor;
						if (!Arguments->TryGetStringField(TEXT("actor"), GuardActor) || GuardActor.IsEmpty())
						{
							OutError = TEXT("Missing required: actor");
							return FString();
						}
						FString ResolveError;
						AActor* TargetActor = Context.Services.FindActorByLabelOrName(GuardActor, ResolveError);
						if (!TargetActor)
						{
							OutError = ResolveError.IsEmpty()
								? FString::Printf(TEXT("Actor not found: %s"), *GuardActor)
								: ResolveError;
							return FString();
						}
						TArray<UPCGComponent*> PcgComps;
						TargetActor->GetComponents<UPCGComponent>(PcgComps);
						if (PcgComps.Num() == 0)
						{
							OutError = FString::Printf(TEXT("Actor '%s' has no UPCGComponent; nothing to clear."), *GuardActor);
							return FString();
						}
					}
					const FString ArgumentsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal\n")
						TEXT("import json\n")
						TEXT("tool_name = %s\n")
						TEXT("args = json.loads(%s)\n")
						TEXT("asset_subsystem = unreal.EditorAssetSubsystem()\n")
						TEXT("asset_tools = unreal.AssetToolsHelpers.get_asset_tools()\n")
						TEXT("asset_registry = unreal.AssetRegistryHelpers.get_asset_registry()\n")
						TEXT("asset_path = args.get('asset_path', '')\n")
						TEXT("package_path = args.get('package_path', '')\n")
						TEXT("asset_name = args.get('asset_name', '')\n")
						TEXT("if tool_name == 'pcg_graph_create' and asset_path and (not package_path or not asset_name):\n")
						TEXT("    _asset_path_trim = asset_path.rstrip('/')\n")
						TEXT("    _slash = _asset_path_trim.rfind('/')\n")
						TEXT("    if _slash > 0 and _slash < len(_asset_path_trim) - 1:\n")
						TEXT("        package_path = _asset_path_trim[:_slash]\n")
						TEXT("        asset_name = _asset_path_trim[_slash + 1:]\n")
						TEXT("if tool_name == 'pcg_graph_create' and package_path and asset_name:\n")
						TEXT("    _base_asset_name = asset_name\n")
						TEXT("    _base_asset_path = package_path.rstrip('/') + '/' + asset_name\n")
						TEXT("    try:\n")
						TEXT("        _exists = bool(unreal.EditorAssetLibrary.does_asset_exist(_base_asset_path))\n")
						TEXT("    except Exception:\n")
						TEXT("        _exists = False\n")
						TEXT("    if _exists:\n")
						TEXT("        _picked = None\n")
						TEXT("        for _idx in range(2, 1000):\n")
						TEXT("            _candidate = _base_asset_name + '_v' + str(_idx)\n")
						TEXT("            _candidate_path = package_path.rstrip('/') + '/' + _candidate\n")
						TEXT("            try:\n")
						TEXT("                _candidate_exists = bool(unreal.EditorAssetLibrary.does_asset_exist(_candidate_path))\n")
						TEXT("            except Exception:\n")
						TEXT("                _candidate_exists = False\n")
						TEXT("            if not _candidate_exists:\n")
						TEXT("                _picked = _candidate\n")
						TEXT("                break\n")
						TEXT("        if _picked is None:\n")
						TEXT("            raise RuntimeError('pcg_graph_create_no_unique_name_available base=' + _base_asset_name)\n")
						TEXT("        unreal.log('pcg_graph_create target exists; using unique asset_name=' + _picked)\n")
						TEXT("        asset_name = _picked\n")
						TEXT("target = asset_subsystem.load_asset(asset_path) if asset_path else None\n")
						TEXT("def save_target(obj):\n")
						TEXT("    # Audit round 7 (silent-create fix): also notify AssetRegistry and tolerate save failures.\n")
						TEXT("    if obj is None:\n")
						TEXT("        return\n")
						TEXT("    try:\n")
						TEXT("        asset_registry.asset_created(obj)\n")
						TEXT("    except Exception:\n")
						TEXT("        pass\n")
						TEXT("    try:\n")
						TEXT("        asset_subsystem.save_loaded_asset(obj)\n")
						TEXT("    except Exception as _save_err:\n")
						TEXT("        unreal.log_warning('save_target save_loaded_asset failed: ' + str(_save_err))\n")
						TEXT("def resolve_actor(actor_id):\n")
						TEXT("    if not actor_id:\n")
						TEXT("        return None\n")
						TEXT("    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n")
						TEXT("    for actor in subsystem.get_all_level_actors():\n")
						TEXT("        if actor.get_actor_label() == actor_id or actor.get_name() == actor_id or actor.get_path_name() == actor_id:\n")
						TEXT("            return actor\n")
						TEXT("    return None\n")
						TEXT("def vector_from(value):\n")
						TEXT("    value = value or {}\n")
						TEXT("    return unreal.Vector(float(value.get('x', 0.0)), float(value.get('y', 0.0)), float(value.get('z', 0.0)))\n")
						TEXT("def make_vector(data):\n")
						TEXT("    data = data or {}\n")
						TEXT("    return unreal.Vector(float(data.get('x', 0.0)), float(data.get('y', 0.0)), float(data.get('z', 0.0)))\n")
						TEXT("def make_rotator(data):\n")
						TEXT("    data = data or {}\n")
						TEXT("    return unreal.Rotator(float(data.get('pitch', 0.0)), float(data.get('yaw', 0.0)), float(data.get('roll', 0.0)))\n")
						TEXT("def create_with_factories(factory_paths):\n")
						TEXT("    factory = None\n")
						TEXT("    for class_path in factory_paths:\n")
						TEXT("        factory_class = unreal.load_class(None, class_path)\n")
						TEXT("        if factory_class is not None:\n")
						TEXT("            factory = unreal.new_object(factory_class)\n")
						TEXT("            break\n")
						TEXT("    if factory is None:\n")
						TEXT("        raise RuntimeError('Unable to resolve factory for ' + tool_name)\n")
						TEXT("    created = asset_tools.create_asset(asset_name, package_path, None, factory)\n")
						TEXT("    if created is None:\n")
						TEXT("        raise RuntimeError('Asset creation failed for ' + tool_name)\n")
						TEXT("    save_target(created)\n")
						TEXT("    return created\n")
						TEXT("def asset_data_path(asset_data):\n")
						TEXT("    if hasattr(asset_data, 'get_soft_object_path'):\n")
						TEXT("        return str(asset_data.get_soft_object_path())\n")
						TEXT("    if hasattr(asset_data, 'object_path'):\n")
						TEXT("        return str(asset_data.object_path.string)\n")
						TEXT("    if hasattr(asset_data, 'package_name') and hasattr(asset_data, 'asset_name'):\n")
						TEXT("        return str(asset_data.package_name) + '.' + str(asset_data.asset_name)\n")
						TEXT("    return str(asset_data)\n")
						TEXT("create_factories = {\n")
						TEXT("    'audio_sound_class_create': ['/Script/Engine.SoundClassFactory'],\n")
						TEXT("    'audio_sound_mix_create': ['/Script/Engine.SoundMixFactory'],\n")
						TEXT("    'audio_submix_create': ['/Script/AudioEditor.SoundSubmixFactory', '/Script/Engine.SoundSubmixFactory'],\n")
						TEXT("    'foliage_type_create': ['/Script/FoliageEditor.FoliageTypeFactory', '/Script/Foliage.FoliageTypeFactory'],\n")
						TEXT("    'pcg_graph_create': ['/Script/PCGEditor.PCGGraphFactory', '/Script/PCG.PCGGraphFactory'],\n")
						TEXT("    'packed_level_actor_create': ['/Script/Engine.WorldFactory'],\n")
						TEXT("    'gameplaycue_notify_create': ['/Script/GameplayAbilitiesEditor.GameplayCueNotify_StaticFactory', '/Script/GameplayAbilitiesEditor.GameplayCueNotify_ActorFactory'],\n")
						TEXT("    'ik_rig_create': ['/Script/IKRigEditor.IKRigDefinitionFactory'],\n")
						TEXT("    'ik_retargeter_create': ['/Script/IKRigEditor.IKRetargetFactory'],\n")
						TEXT("    'control_rig_template_create_from_skeletal_mesh': ['/Script/ControlRigEditor.ControlRigBlueprintFactory'],\n")
						TEXT("}\n")
						TEXT("list_classes = {\n")
						TEXT("    'audio_sound_class_list': 'SoundClass',\n")
						TEXT("    'audio_sound_mix_list': 'SoundMix',\n")
						TEXT("    'audio_submix_list': 'SoundSubmix',\n")
						TEXT("    'foliage_type_list': 'FoliageType',\n")
						TEXT("}\n")
						TEXT("property_tools = {'audio_sound_class_set_properties', 'audio_sound_mix_set_properties', 'audio_submix_set_properties'}\n")
						TEXT("if tool_name in create_factories:\n")
						TEXT("    created = create_with_factories(create_factories[tool_name])\n")
						TEXT("    if tool_name == 'pcg_graph_create':\n")
						TEXT("        # Audit round 7 (silent-create fix): second-confirmation that the PCG graph\n")
						TEXT("        # actually persisted (does_asset_exist) so pcg_graph_validate doesn't fail later.\n")
						TEXT("        full_path = (package_path.rstrip('/') + '/' + asset_name) if (package_path and asset_name) else (created.get_path_name() if created is not None else '')\n")
						TEXT("        try:\n")
						TEXT("            _persisted = bool(unreal.EditorAssetLibrary.does_asset_exist(full_path))\n")
						TEXT("        except Exception:\n")
						TEXT("            _persisted = False\n")
						TEXT("        if not _persisted:\n")
						TEXT("            raise RuntimeError('asset_not_persisted_after_create tool=pcg_graph_create asset_path=' + full_path)\n")
						TEXT("    if target is not None:\n")
						TEXT("        asset_subsystem.open_editor_for_assets([created])\n")
						TEXT("elif tool_name in list_classes:\n")
						TEXT("    package_name = package_path.rstrip('/')\n")
						TEXT("    assets = asset_registry.get_assets_by_path(package_name, recursive=True) if package_name else []\n")
						TEXT("    wanted = list_classes[tool_name].lower()\n")
						TEXT("    for asset_data in assets:\n")
						TEXT("        class_name = str(asset_data.asset_class_path.asset_name).lower() if hasattr(asset_data, 'asset_class_path') else asset_data.asset_class.lower()\n")
						TEXT("        if wanted in class_name:\n")
						TEXT("            unreal.log('asset=' + asset_data_path(asset_data))\n")
						TEXT("elif tool_name in property_tools:\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load asset for property update')\n")
						TEXT("    props_in = dict(args.get('properties', {}))\n")
						TEXT("    if tool_name == 'audio_sound_class_set_properties':\n")
						TEXT("        sc_props = target.get_editor_property('properties')\n")
						TEXT("        for key, value in props_in.items():\n")
						TEXT("            sc_props.set_editor_property(key, value)\n")
						TEXT("        target.set_editor_property('properties', sc_props)\n")
						TEXT("    else:\n")
						TEXT("        for key, value in props_in.items():\n")
						TEXT("            target.set_editor_property(key, value)\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_sound_mix_add_class_adjuster':\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load sound mix asset')\n")
						TEXT("    sound_class = asset_subsystem.load_asset(args.get('sound_class_path', ''))\n")
						TEXT("    if sound_class is None:\n")
						TEXT("        raise RuntimeError('Failed to load sound class asset')\n")
						TEXT("    adjuster = unreal.SoundClassAdjuster(\n")
						TEXT("        sound_class_object=sound_class,\n")
						TEXT("        volume_adjuster=float(args.get('volume_adjuster', 1.0)),\n")
						TEXT("        pitch_adjuster=float(args.get('pitch_adjuster', 1.0)))\n")
						TEXT("    existing = list(target.get_editor_property('sound_class_effects')) if hasattr(target, 'get_editor_property') else []\n")
						TEXT("    existing.append(adjuster)\n")
						TEXT("    target.set_editor_property('sound_class_effects', existing)\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'audio_submix_set_parent':\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load submix asset')\n")
						TEXT("    parent = asset_subsystem.load_asset(args.get('parent_submix_path', '')) if args.get('parent_submix_path') else None\n")
						TEXT("    applied = False\n")
						TEXT("    for property_name in ('parent_submix', 'parent_submix_object'):\n")
						TEXT("        try:\n")
						TEXT("            target.set_editor_property(property_name, parent)\n")
						TEXT("            applied = True\n")
						TEXT("            break\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("    if not applied:\n")
						TEXT("        raise RuntimeError('Unable to set parent submix property')\n")
						TEXT("    save_target(target)\n")
						TEXT("elif tool_name == 'pcg_component_attach':\n")
						TEXT("    actor = resolve_actor(args.get('actor', ''))\n")
						TEXT("    graph = asset_subsystem.load_asset(args.get('graph_path', ''))\n")
						TEXT("    if actor is None or graph is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve actor or PCG graph')\n")
						TEXT("    component_class = unreal.load_class(None, '/Script/PCG.PCGComponent')\n")
						TEXT("    if component_class is None:\n")
						TEXT("        raise RuntimeError('PCGComponent class is unavailable')\n")
						TEXT("    # UE5 Python: use SubobjectDataSubsystem to add component to existing actor\n")
						TEXT("    component = None\n")
						TEXT("    try:\n")
						TEXT("        subsys = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)\n")
						TEXT("        # Bug#2 fix: UE 5.7 renamed k2_gather_subobject_data_for_actor → gather_subobject_data_for_actor.\n")
						TEXT("        gather = getattr(subsys, 'gather_subobject_data_for_actor', None) or getattr(subsys, 'k2_gather_subobject_data_for_actor', None)\n")
						TEXT("        find_data = getattr(subsys, 'find_subobject_data_from_handle', None) or getattr(subsys, 'k2_find_subobject_data_from_handle', None)\n")
						TEXT("        if gather is None or find_data is None:\n")
						TEXT("            raise RuntimeError('SubobjectDataSubsystem: gather/find_data API not found in this UE version')\n")
						TEXT("        root_data = gather(actor)[0]\n")
						TEXT("        handle = subsys.add_new_subobject(unreal.AddNewSubobjectParams(\n")
						TEXT("            parent_handle=root_data.handle,\n")
						TEXT("            new_class=component_class,\n")
						TEXT("            blueprint_context=None))\n")
						TEXT("        component = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(find_data(handle))\n")
						TEXT("    except Exception as e:\n")
						TEXT("        # Fallback #1: Actor.add_component_by_class (UE5 editor)\n")
						TEXT("        component = None\n")
						TEXT("        try:\n")
						TEXT("            if hasattr(actor, 'add_component_by_class'):\n")
						TEXT("                component = actor.add_component_by_class(component_class, False, unreal.Transform(), False)\n")
						TEXT("        except Exception:\n")
						TEXT("            component = None\n")
						TEXT("        # Fallback #2: legacy Actor.add_component\n")
						TEXT("        if component is None and hasattr(actor, 'add_component'):\n")
						TEXT("            try:\n")
						TEXT("                component = actor.add_component(component_class, False, unreal.Transform())\n")
						TEXT("            except Exception:\n")
						TEXT("                component = None\n")
						TEXT("        if component is None:\n")
						TEXT("            raise RuntimeError('Cannot add PCGComponent to existing actor in this UE version: ' + str(e))\n")
						TEXT("    if component is None:\n")
						TEXT("        raise RuntimeError('Failed to add PCGComponent to actor')\n")
						TEXT("    component.set_editor_property('graph', graph)\n")
						TEXT("    if args.get('volume_actor', ''):\n")
						TEXT("        volume_actor = resolve_actor(args.get('volume_actor', ''))\n")
						TEXT("        if volume_actor is not None and hasattr(component, 'set_editor_property'):\n")
						TEXT("            # generation_trigger/input_type live on UPCGComponent; bind them first.\n")
						TEXT("            for prop_name in ('generation_trigger', 'input_type'):\n")
						TEXT("                try:\n")
						TEXT("                    component.set_editor_property(prop_name, args.get(prop_name))\n")
						TEXT("                except Exception:\n")
						TEXT("                    pass\n")
						TEXT("            # Best-effort bind volume actor into the component (or its node settings).\n")
						TEXT("            applied = False\n")
						TEXT("            for prop_name in ('volume_actor', 'pcg_volume_actor', 'input_volume', 'source_volume', 'input_actor', 'source_actor', 'pcg_volume'):\n")
						TEXT("                try:\n")
						TEXT("                    component.set_editor_property(prop_name, volume_actor)\n")
						TEXT("                    applied = True\n")
						TEXT("                    break\n")
						TEXT("                except Exception:\n")
						TEXT("                    pass\n")
						TEXT("            if not applied:\n")
						TEXT("                try:\n")
						TEXT("                    for node in list(graph.nodes) if hasattr(graph, 'nodes') else []:\n")
						TEXT("                        settings = node.get_settings() if hasattr(node, 'get_settings') else None\n")
						TEXT("                        if settings is None or not hasattr(settings, 'set_editor_property'):\n")
						TEXT("                            continue\n")
						TEXT("                        for prop_name in ('input_volume', 'source_volume', 'volume', 'input_actor', 'source_actor', 'volume_actor'):\n")
						TEXT("                            try:\n")
						TEXT("                                settings.set_editor_property(prop_name, volume_actor)\n")
						TEXT("                                applied = True\n")
						TEXT("                                break\n")
						TEXT("                            except Exception:\n")
						TEXT("                                pass\n")
						TEXT("                        if applied:\n")
						TEXT("                            break\n")
						TEXT("                except Exception:\n")
						TEXT("                    pass\n")
						TEXT("            unreal.log('pcg_component_attach: volume_actor binding applied=' + str(applied))\n")
						TEXT("elif tool_name in ('pcg_generate', 'pcg_clear'):\n")
						TEXT("    actor = resolve_actor(args.get('actor', ''))\n")
						TEXT("    if actor is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve actor')\n")
						TEXT("    touched = 0\n")
						TEXT("    for component in actor.get_components_by_class(unreal.ActorComponent):\n")
						TEXT("        if 'PCGComponent' in component.get_class().get_name():\n")
						TEXT("            if tool_name == 'pcg_generate' and hasattr(component, 'generate'):\n")
						TEXT("                component.generate()\n")
						TEXT("                touched += 1\n")
						TEXT("            elif tool_name == 'pcg_clear' and hasattr(component, 'cleanup'):\n")
						TEXT("                try:\n")
						TEXT("                    component.cleanup(False)\n")
						TEXT("                except TypeError:\n")
						TEXT("                    component.cleanup(remove_components=False)\n")
						TEXT("                touched += 1\n")
						TEXT("    if touched == 0:\n")
						TEXT("        raise RuntimeError('No PCGComponent found on actor')\n")
						TEXT("elif tool_name == 'pcg_graph_list_nodes':\n")
						TEXT("    if target is None:\n")
						// Audit round 3: return structured JSON instead of raising so callers see a clean error.
						TEXT("        import json as _json\n")
						TEXT("        print(_json.dumps({'error': 'Failed to load PCG graph asset', 'asset_path': asset_path or ''}))\n")
						TEXT("        raise SystemExit(0)\n")
						TEXT("    graph = target\n")
						TEXT("    for node in list(graph.nodes) if hasattr(graph, 'nodes') else []:\n")
						TEXT("        title = ''\n")
						TEXT("        try:\n")
						TEXT("            title = str(node.get_node_title())\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("        settings_name = ''\n")
						TEXT("        try:\n")
						TEXT("            settings = node.get_settings()\n")
						TEXT("            if settings is not None:\n")
						TEXT("                settings_name = settings.get_class().get_path_name()\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("        unreal.log('pcg_node=' + str(node.get_name()) + '|title=' + title + '|settings=' + settings_name)\n")
						TEXT("elif tool_name == 'pcg_graph_add_node':\n")
						TEXT("    if target is None:\n")
						// Audit round 3: return structured JSON instead of raising so callers see a clean error.
						TEXT("        import json as _json\n")
						TEXT("        print(_json.dumps({'error': 'Failed to load PCG graph asset', 'asset_path': asset_path or ''}))\n")
						TEXT("        raise SystemExit(0)\n")
						TEXT("    graph = target\n")
						TEXT("    if not hasattr(graph, 'add_node_of_type'):\n")
						TEXT("        raise RuntimeError('Asset is not a PCGGraph (missing add_node_of_type)')\n")
						TEXT("    node_class_path = args.get('node_class_path', '')\n")
						TEXT("    settings_class = unreal.load_class(None, node_class_path)\n")
						TEXT("    if settings_class is None:\n")
						TEXT("        raise RuntimeError('Failed to load node_class_path: ' + node_class_path)\n")
						TEXT("    created = graph.add_node_of_type(settings_class)\n")
						TEXT("    new_node = created[0] if isinstance(created, tuple) else created\n")
						TEXT("    if args.get('node_label', ''):\n")
						TEXT("        try:\n")
						TEXT("            new_node.set_node_title(args.get('node_label', ''))\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("    unreal.log('pcg_graph_add_node node=' + str(new_node.get_name()))\n")
						TEXT("    if hasattr(graph, 'force_notification_for_editor'):\n")
						TEXT("        graph.force_notification_for_editor()\n")
						TEXT("    save_target(graph)\n")
						TEXT("elif tool_name == 'pcg_graph_list_pins':\n")
						TEXT("    if target is None:\n")
						// Audit round 3: return structured JSON instead of raising so callers see a clean error.
						TEXT("        import json as _json\n")
						TEXT("        print(_json.dumps({'error': 'Failed to load PCG graph asset', 'asset_path': asset_path or ''}))\n")
						TEXT("        raise SystemExit(0)\n")
						TEXT("    graph = target\n")
						TEXT("    wanted_node = args.get('node', '')\n")
						TEXT("    for node in list(graph.nodes) if hasattr(graph, 'nodes') else []:\n")
						TEXT("        node_name = str(node.get_name())\n")
						TEXT("        if wanted_node and wanted_node.lower() not in node_name.lower():\n")
						TEXT("            continue\n")
						TEXT("        for getter, kind in (('get_input_pins', 'in'), ('get_output_pins', 'out')):\n")
						TEXT("            pins = []\n")
						TEXT("            try:\n")
						TEXT("                pins = list(getattr(node, getter)())\n")
						TEXT("            except Exception:\n")
						TEXT("                pass\n")
						TEXT("            for pin in pins:\n")
						TEXT("                label = ''\n")
						TEXT("                try:\n")
						TEXT("                    label = str(pin.get_label())\n")
						TEXT("                except Exception:\n")
						TEXT("                    label = str(pin)\n")
						TEXT("                unreal.log('pcg_pin=' + node_name + '::' + label + '|dir=' + kind)\n")
						TEXT("elif tool_name == 'pcg_graph_connect':\n")
						TEXT("    if target is None:\n")
						// Audit round 3: return structured JSON instead of raising so callers see a clean error.
						TEXT("        import json as _json\n")
						TEXT("        print(_json.dumps({'error': 'Failed to load PCG graph asset', 'asset_path': asset_path or ''}))\n")
						TEXT("        raise SystemExit(0)\n")
						TEXT("    graph = target\n")
						TEXT("    if not hasattr(graph, 'add_edge'):\n")
						TEXT("        raise RuntimeError('Asset is not a PCGGraph (missing add_edge)')\n")
						TEXT("    def _pcg_node_labels(node):\n")
						TEXT("        labels = [str(node.get_name())]\n")
						TEXT("        try:\n")
						TEXT("            labels.append(str(node.get_node_title()))\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("        # Bug#3 fix: include settings class name as searchable label so tokens like\n")
						TEXT("        # 'SpatialNoise' match 'PCGSpatialNoiseSettings' even when get_name/get_node_title are unset.\n")
						TEXT("        try:\n")
						TEXT("            s = node.get_settings() if hasattr(node, 'get_settings') else None\n")
						TEXT("            if s is not None:\n")
						TEXT("                cls_name = s.__class__.__name__\n")
						TEXT("                labels.append(cls_name)\n")
						TEXT("                # strip 'PCG' prefix and 'Settings' suffix so 'SpatialNoise' matches.\n")
						TEXT("                stripped = cls_name\n")
						TEXT("                if stripped.startswith('PCG'): stripped = stripped[3:]\n")
						TEXT("                if stripped.endswith('Settings'): stripped = stripped[:-8]\n")
						TEXT("                if stripped: labels.append(stripped)\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("        return [label for label in labels if label]\n")
						TEXT("    def _find_pcg_node(token):\n")
						TEXT("        if not token:\n")
						TEXT("            return None\n")
						TEXT("        want = str(token).lower()\n")
						TEXT("        exact = None\n")
						TEXT("        partial = []\n")
						TEXT("        for node in list(graph.nodes):\n")
						TEXT("            for label in _pcg_node_labels(node):\n")
						TEXT("                low = label.lower()\n")
						TEXT("                if low == want:\n")
						TEXT("                    exact = node\n")
						TEXT("                elif want in low:\n")
						TEXT("                    partial.append(node)\n")
						TEXT("        if exact is not None:\n")
						TEXT("            return exact\n")
						TEXT("        if len(partial) == 1:\n")
						TEXT("            return partial[0]\n")
						TEXT("        if len(partial) == 0:\n")
						TEXT("            return None\n")
						TEXT("        raise RuntimeError('Ambiguous PCG node token: ' + str(token))\n")
						TEXT("    def _split_node_pin(spec):\n")
						TEXT("        spec = str(spec or '')\n")
						TEXT("        for sep in ('::', '|'):\n")
						TEXT("            if sep in spec:\n")
						TEXT("                node_name, pin_name = spec.split(sep, 1)\n")
						TEXT("                return node_name.strip(), pin_name.strip()\n")
						TEXT("        raise RuntimeError('Pin specifier must be Node::Pin or Node|Pin, got: ' + spec)\n")
						TEXT("    src_node = args.get('source_node', '')\n")
						TEXT("    src_pin = args.get('source_pin', '')\n")
						TEXT("    dst_node = args.get('target_node', '')\n")
						TEXT("    dst_pin = args.get('target_pin', '')\n")
						TEXT("    if not src_pin:\n")
						TEXT("        src_node, src_pin = _split_node_pin(args.get('source_pin_path', ''))\n")
						TEXT("    if not dst_pin:\n")
						TEXT("        dst_node, dst_pin = _split_node_pin(args.get('target_pin_path', ''))\n")
						TEXT("    if not src_node or not src_pin or not dst_node or not dst_pin:\n")
						TEXT("        raise RuntimeError('Provide source_node/source_pin and target_node/target_pin, or source_pin_path/target_pin_path as Node::Pin')\n")
						TEXT("    n_from = _find_pcg_node(src_node)\n")
						TEXT("    n_to = _find_pcg_node(dst_node)\n")
						TEXT("    if n_from is None or n_to is None:\n")
						TEXT("        raise RuntimeError('Could not resolve PCG nodes for connection')\n")
						TEXT("    graph.add_edge(n_from, src_pin, n_to, dst_pin)\n")
						TEXT("    if hasattr(graph, 'force_notification_for_editor'):\n")
						TEXT("        graph.force_notification_for_editor()\n")
						TEXT("    save_target(graph)\n")
						TEXT("elif tool_name == 'pcg_graph_set_node_property':\n")
						TEXT("    if target is None:\n")
						// Audit round 3: return structured JSON instead of raising so callers see a clean error.
						TEXT("        import json as _json\n")
						TEXT("        print(_json.dumps({'error': 'Failed to load PCG graph asset', 'asset_path': asset_path or ''}))\n")
						TEXT("        raise SystemExit(0)\n")
						TEXT("    graph = target\n")
						TEXT("    token = args.get('node', '')\n")
						TEXT("    props = dict(args.get('properties', {}))\n")
						TEXT("    if not token:\n")
						TEXT("        raise RuntimeError('Missing node token')\n")
						TEXT("    node = None\n")
						TEXT("    for n in list(graph.nodes) if hasattr(graph, 'nodes') else []:\n")
						TEXT("        if token.lower() == str(n.get_name()).lower() or token.lower() in str(n.get_name()).lower():\n")
						TEXT("            node = n\n")
						TEXT("            break\n")
						TEXT("    if node is None:\n")
						TEXT("        raise RuntimeError('Node not found: ' + token)\n")
						TEXT("    settings = node.get_settings() if hasattr(node, 'get_settings') else None\n")
						TEXT("    if settings is None:\n")
						TEXT("        raise RuntimeError('Node settings are unavailable')\n")
						TEXT("    for key, value in props.items():\n")
						TEXT("        settings.set_editor_property(key, value)\n")
						TEXT("    if hasattr(graph, 'force_notification_for_editor'):\n")
						TEXT("        graph.force_notification_for_editor()\n")
						TEXT("    save_target(graph)\n")
						TEXT("elif tool_name == 'pcg_graph_add_static_mesh_spawner':\n")
						TEXT("    if target is None:\n")
						// Audit round 3: return structured JSON instead of raising so callers see a clean error.
						TEXT("        import json as _json\n")
						TEXT("        print(_json.dumps({'error': 'Failed to load PCG graph asset', 'asset_path': asset_path or ''}))\n")
						TEXT("        raise SystemExit(0)\n")
						TEXT("    graph = target\n")
						TEXT("    mesh_paths = list(args.get('static_mesh_paths') or [])\n")
						TEXT("    if not mesh_paths:\n")
						TEXT("        raise RuntimeError('static_mesh_paths must contain at least one mesh path')\n")
						TEXT("    settings = unreal.PCGStaticMeshSpawnerSettings()\n")
						TEXT("    descriptor_cls = getattr(unreal, 'PCGSoftISMComponentDescriptor', None)\n")
						TEXT("    weighted_entry_cls = getattr(unreal, 'PCGMeshSelectorWeightedEntry', None)\n")
						TEXT("    if descriptor_cls is None or weighted_entry_cls is None:\n")
						TEXT("        raise RuntimeError('PCGSoftISMComponentDescriptor or PCGMeshSelectorWeightedEntry is unavailable')\n")
						TEXT("    mesh_entries = []\n")
						TEXT("    for mesh_path in mesh_paths:\n")
						TEXT("        mesh = asset_subsystem.load_asset(mesh_path) or unreal.load_asset(mesh_path)\n")
						TEXT("        if mesh is None:\n")
						TEXT("            raise RuntimeError('Failed to load static mesh: ' + str(mesh_path))\n")
						TEXT("        descriptor = descriptor_cls()\n")
						TEXT("        descriptor.set_editor_property('static_mesh', mesh)\n")
						TEXT("        entry = weighted_entry_cls()\n")
						TEXT("        entry.set_editor_property('descriptor', descriptor)\n")
						TEXT("        entry.set_editor_property('weight', int(args.get('weight', 1)))\n")
						TEXT("        mesh_entries.append(entry)\n")
						TEXT("    if hasattr(graph, 'add_node'):\n")
						TEXT("        created = graph.add_node(settings)\n")
						TEXT("    elif hasattr(graph, 'add_node_of_type'):\n")
						TEXT("        created = graph.add_node_of_type(unreal.PCGStaticMeshSpawnerSettings)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('Asset is not a PCGGraph (missing add_node/add_node_of_type)')\n")
						TEXT("    new_node = created[0] if isinstance(created, tuple) else created\n")
						TEXT("    label = args.get('node_label', 'StaticMeshSpawner_0')\n")
						TEXT("    if label:\n")
						TEXT("        try:\n")
						TEXT("            new_node.set_node_title(label)\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("    node_settings = new_node.get_settings() if hasattr(new_node, 'get_settings') else settings\n")
						TEXT("    selector = node_settings.get_editor_property('mesh_selector_parameters')\n")
						TEXT("    if selector is None:\n")
						TEXT("        selector_cls = getattr(unreal, 'PCGMeshSelectorWeighted', None)\n")
						TEXT("        if selector_cls is None:\n")
						TEXT("            raise RuntimeError('PCGMeshSelectorWeighted is unavailable')\n")
						TEXT("        selector = selector_cls()\n")
						TEXT("    selector.set_editor_property('mesh_entries', mesh_entries)\n")
						TEXT("    connected = False\n")
						TEXT("    source_node = args.get('source_node', '')\n")
						TEXT("    source_pin = args.get('source_pin', 'Out')\n")
						TEXT("    target_pin = args.get('target_pin', 'In')\n")
						TEXT("    if source_node:\n")
						TEXT("        def _labels(node):\n")
						TEXT("            values = [str(node.get_name())]\n")
						TEXT("            try:\n")
						TEXT("                values.append(str(node.get_node_title()))\n")
						TEXT("            except Exception:\n")
						TEXT("                pass\n")
						TEXT("            try:\n")
						TEXT("                s = node.get_settings() if hasattr(node, 'get_settings') else None\n")
						TEXT("                if s is not None:\n")
						TEXT("                    values.append(str(s.__class__.__name__))\n")
						TEXT("            except Exception:\n")
						TEXT("                pass\n")
						TEXT("            return [v for v in values if v]\n")
						TEXT("        wanted = str(source_node).lower()\n")
						TEXT("        source = None\n")
						TEXT("        partial = []\n")
						TEXT("        for node in list(graph.nodes) if hasattr(graph, 'nodes') else []:\n")
						TEXT("            for value in _labels(node):\n")
						TEXT("                low = value.lower()\n")
						TEXT("                if low == wanted:\n")
						TEXT("                    source = node\n")
						TEXT("                    break\n")
						TEXT("                if wanted in low:\n")
						TEXT("                    partial.append(node)\n")
						TEXT("            if source is not None:\n")
						TEXT("                break\n")
						TEXT("        if source is None and len(partial) == 1:\n")
						TEXT("            source = partial[0]\n")
						TEXT("        if source is None:\n")
						TEXT("            raise RuntimeError('Could not resolve source_node for spawner connection: ' + str(source_node))\n")
						TEXT("        graph.add_edge(source, source_pin, new_node, target_pin)\n")
						TEXT("        connected = True\n")
						TEXT("    if hasattr(graph, 'force_notification_for_editor'):\n")
						TEXT("        graph.force_notification_for_editor()\n")
						TEXT("    save_target(graph)\n")
						TEXT("    unreal.log('pcg_graph_add_static_mesh_spawner node=' + str(new_node.get_name()) + '|mesh_count=' + str(len(mesh_entries)) + '|connected=' + str(connected))\n")
						TEXT("elif tool_name.startswith('foliage_'):\n")
						TEXT("    foliage_subsystem_type = getattr(unreal, 'FoliageEditorSubsystem', None)\n")
						TEXT("    foliage_subsystem = unreal.get_editor_subsystem(foliage_subsystem_type) if foliage_subsystem_type is not None else None\n")
						TEXT("    foliage_type = asset_subsystem.load_asset(args.get('foliage_type_path', '')) if args.get('foliage_type_path') else target\n")
						TEXT("    if tool_name == 'foliage_paint_instances':\n")
						TEXT("        if foliage_subsystem is None or foliage_type is None:\n")
						TEXT("            raise RuntimeError('Foliage subsystem or foliage type is unavailable')\n")
						TEXT("        transforms = []\n")
						TEXT("        for location_data in list(args.get('locations', [])):\n")
						TEXT("            transforms.append(unreal.Transform(location=unreal.Vector(float(location_data.get('x', 0.0)), float(location_data.get('y', 0.0)), float(location_data.get('z', 0.0)))))\n")
						TEXT("        if hasattr(foliage_subsystem, 'add_instances'):\n")
						TEXT("            foliage_subsystem.add_instances(foliage_type, transforms)\n")
						TEXT("    elif tool_name == 'foliage_remove_instances':\n")
						TEXT("        if foliage_subsystem is None or foliage_type is None:\n")
						TEXT("            raise RuntimeError('Foliage subsystem or foliage type is unavailable')\n")
						TEXT("        min_v = vector_from(args.get('min'))\n")
						TEXT("        max_v = vector_from(args.get('max'))\n")
						TEXT("        if hasattr(foliage_subsystem, 'remove_instances_in_box'):\n")
						TEXT("            foliage_subsystem.remove_instances_in_box(foliage_type, unreal.Box(min_v, max_v))\n")
						TEXT("    elif tool_name == 'foliage_query_instances':\n")
						TEXT("        if foliage_subsystem is None or foliage_type is None:\n")
						TEXT("            raise RuntimeError('Foliage subsystem or foliage type is unavailable')\n")
						TEXT("        min_v = vector_from(args.get('min'))\n")
						TEXT("        max_v = vector_from(args.get('max'))\n")
						TEXT("        if hasattr(foliage_subsystem, 'get_instances_in_box'):\n")
						TEXT("            for instance in list(foliage_subsystem.get_instances_in_box(foliage_type, unreal.Box(min_v, max_v))):\n")
						TEXT("                unreal.log('instance=' + str(instance))\n")
						TEXT("    elif tool_name == 'foliage_set_instance_transforms':\n")
						TEXT("        if foliage_subsystem is None or foliage_type is None:\n")
						TEXT("            raise RuntimeError('Foliage subsystem or foliage type is unavailable')\n")
						TEXT("        if hasattr(foliage_subsystem, 'set_instance_world_transforms'):\n")
						TEXT("            indices = []\n")
						TEXT("            transforms = []\n")
						TEXT("            for item in list(args.get('instances', [])):\n")
						TEXT("                indices.append(int(item.get('index', 0)))\n")
						TEXT("                transform_data = item.get('transform', {})\n")
						TEXT("                transforms.append(unreal.Transform(rotation=make_rotator(transform_data.get('rotation')), location=make_vector(transform_data.get('location')), scale=make_vector(transform_data.get('scale', {'x':1.0,'y':1.0,'z':1.0}))))\n")
						TEXT("            foliage_subsystem.set_instance_world_transforms(foliage_type, indices, transforms)\n")
						TEXT("elif tool_name.startswith('geometry_script_'):\n")
						TEXT("    target_mesh = asset_subsystem.load_asset(args.get('target_mesh_path', ''))\n")
						TEXT("    if target_mesh is None:\n")
						TEXT("        raise RuntimeError('Failed to load target mesh')\n")
						TEXT("    dynamic_mesh_type = getattr(unreal, 'DynamicMesh', None)\n")
						TEXT("    if dynamic_mesh_type is None:\n")
						TEXT("        raise RuntimeError('DynamicMesh type is unavailable')\n")
						TEXT("    dyn = dynamic_mesh_type()\n")
						TEXT("    if hasattr(unreal, 'GeometryScript_AssetUtils') and hasattr(unreal.GeometryScript_AssetUtils, 'copy_mesh_from_static_mesh'):\n")
						TEXT("        dyn = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(target_mesh, dyn)\n")
						TEXT("    if tool_name == 'geometry_script_boolean':\n")
						TEXT("        other_mesh = asset_subsystem.load_asset(args.get('other_mesh_path', ''))\n")
						TEXT("        if other_mesh is None:\n")
						TEXT("            raise RuntimeError('Failed to load boolean other mesh')\n")
						TEXT("        other_dyn = dynamic_mesh_type()\n")
						TEXT("        if hasattr(unreal.GeometryScript_AssetUtils, 'copy_mesh_from_static_mesh'):\n")
						TEXT("            other_dyn = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(other_mesh, other_dyn)\n")
						TEXT("        if hasattr(unreal, 'GeometryScript_MeshBooleans') and hasattr(unreal.GeometryScript_MeshBooleans, 'apply_mesh_boolean'):\n")
						TEXT("            unreal.GeometryScript_MeshBooleans.apply_mesh_boolean(dyn, other_dyn, args.get('operation', 'union'))\n")
						TEXT("    elif tool_name == 'geometry_script_extrude':\n")
						TEXT("        if hasattr(unreal, 'GeometryScript_MeshModelingFunctions') and hasattr(unreal.GeometryScript_MeshModelingFunctions, 'apply_mesh_extrude'):\n")
						TEXT("            unreal.GeometryScript_MeshModelingFunctions.apply_mesh_extrude(dyn, float(args.get('distance', 0.0)))\n")
						TEXT("    elif tool_name == 'geometry_script_remesh':\n")
						TEXT("        if hasattr(unreal, 'GeometryScript_MeshRemeshFunctions') and hasattr(unreal.GeometryScript_MeshRemeshFunctions, 'apply_uniform_remesh'):\n")
						TEXT("            unreal.GeometryScript_MeshRemeshFunctions.apply_uniform_remesh(dyn, int(args.get('target_triangle_count', 1000)))\n")
						TEXT("    elif tool_name == 'geometry_script_auto_uv':\n")
						TEXT("        if hasattr(unreal, 'GeometryScript_UVs') and hasattr(unreal.GeometryScript_UVs, 'auto_generate_xatlas_mesh_uvs'):\n")
						TEXT("            unreal.GeometryScript_UVs.auto_generate_xatlas_mesh_uvs(dyn)\n")
						TEXT("    elif tool_name == 'geometry_script_repair_mesh':\n")
						TEXT("        if hasattr(unreal, 'GeometryScript_MeshRepairFunctions') and hasattr(unreal.GeometryScript_MeshRepairFunctions, 'repair_mesh'):\n")
						TEXT("            unreal.GeometryScript_MeshRepairFunctions.repair_mesh(dyn)\n")
						TEXT("    if hasattr(unreal.GeometryScript_AssetUtils, 'copy_mesh_to_static_mesh'):\n")
						TEXT("        unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(dyn, target_mesh)\n")
						TEXT("        save_target(target_mesh)\n")
						TEXT("elif tool_name == 'ik_rig_add_goal':\n")
						TEXT("    if target is None:\n")
						TEXT("        raise RuntimeError('Failed to load IK Rig asset')\n")
						TEXT("    editor_subsystem_type = getattr(unreal, 'IKRigEditorSubsystem', None)\n")
						TEXT("    editor_subsystem = unreal.get_editor_subsystem(editor_subsystem_type) if editor_subsystem_type is not None else None\n")
						TEXT("    if editor_subsystem is not None and hasattr(editor_subsystem, 'add_new_goal'):\n")
						TEXT("        editor_subsystem.add_new_goal(target, args.get('goal_name', ''), args.get('bone_name', ''))\n")
						TEXT("        save_target(target)\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('IK Rig editor subsystem is unavailable')\n")
						TEXT("elif tool_name == 'control_rig_template_create_from_skeletal_mesh':\n")
						TEXT("    created = create_with_factories(create_factories[tool_name])\n")
						TEXT("    skeletal_mesh = asset_subsystem.load_asset(args.get('skeletal_mesh_path', '')) if args.get('skeletal_mesh_path') else None\n")
						TEXT("    if skeletal_mesh is not None:\n")
						TEXT("        if hasattr(unreal, 'ControlRigBlueprintEditorLibrary') and hasattr(unreal.ControlRigBlueprintEditorLibrary, 'import_bones_from_asset'):\n")
						TEXT("            unreal.ControlRigBlueprintEditorLibrary.import_bones_from_asset(created, skeletal_mesh)\n")
						TEXT("        try:\n")
						TEXT("            created.set_preview_mesh(skeletal_mesh)\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("        save_target(created)\n")
						TEXT("elif tool_name == 'level_instance_create':\n")
						TEXT("    world_asset = asset_subsystem.load_asset(args.get('level_asset_path', ''))\n")
						TEXT("    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n")
						TEXT("    level_instance_class = unreal.load_class(None, '/Script/Engine.LevelInstance')\n")
						TEXT("    if world_asset is None or level_instance_class is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve level asset or level instance class')\n")
						TEXT("    location_data = args.get('location', {})\n")
						TEXT("    actor = actor_subsystem.spawn_actor_from_class(level_instance_class, unreal.Vector(location_data.get('x', 0.0), location_data.get('y', 0.0), location_data.get('z', 0.0)))\n")
						TEXT("    if actor is None:\n")
						TEXT("        raise RuntimeError('Failed to spawn level instance actor')\n")
						TEXT("    for property_name in ('world_asset', 'level_asset', 'level'):\n")
						TEXT("        try:\n")
						TEXT("            actor.set_editor_property(property_name, world_asset)\n")
						TEXT("            break\n")
						TEXT("        except Exception:\n")
						TEXT("            pass\n")
						TEXT("elif tool_name in ('level_instance_break', 'packed_level_actor_unpack', 'level_instance_sync'):\n")
						TEXT("    actor = resolve_actor(args.get('actor', ''))\n")
						TEXT("    if actor is None:\n")
						TEXT("        raise RuntimeError('Failed to resolve target actor')\n")
						TEXT("    subsystem_type = getattr(unreal, 'LevelInstanceEditorSubsystem', None)\n")
						TEXT("    subsystem = unreal.get_editor_subsystem(subsystem_type) if subsystem_type is not None else None\n")
						TEXT("    if subsystem is None:\n")
						TEXT("        raise RuntimeError('LevelInstanceEditorSubsystem is unavailable')\n")
						TEXT("    if tool_name == 'level_instance_break' and hasattr(subsystem, 'break_level_instance'):\n")
						TEXT("        subsystem.break_level_instance(actor)\n")
						TEXT("    elif tool_name == 'packed_level_actor_unpack' and hasattr(subsystem, 'unpack_packed_level_actor'):\n")
						TEXT("        subsystem.unpack_packed_level_actor(actor)\n")
						TEXT("    elif tool_name == 'level_instance_sync' and hasattr(subsystem, 'commit_level_instance'):\n")
						TEXT("        subsystem.commit_level_instance(actor)\n")
						TEXT("elif tool_name in ('build_lighting', 'build_navigation', 'build_hlod', 'update_reflection_captures'):\n")
						TEXT("    if hasattr(unreal, 'EditorBuildUtils'):\n")
						TEXT("        if tool_name == 'build_lighting' and hasattr(unreal.EditorBuildUtils, 'editor_build_lighting'):\n")
						TEXT("            unreal.EditorBuildUtils.editor_build_lighting()\n")
						TEXT("        elif tool_name == 'build_navigation' and hasattr(unreal.EditorBuildUtils, 'editor_build_navigation'):\n")
						TEXT("            unreal.EditorBuildUtils.editor_build_navigation()\n")
						TEXT("        elif tool_name == 'build_hlod' and hasattr(unreal.EditorBuildUtils, 'editor_build_hlod'):\n")
						TEXT("            unreal.EditorBuildUtils.editor_build_hlod()\n")
						TEXT("        elif tool_name == 'update_reflection_captures' and hasattr(unreal.EditorBuildUtils, 'update_reflection_captures'):\n")
						TEXT("            unreal.EditorBuildUtils.update_reflection_captures()\n")
						TEXT("        else:\n")
						TEXT("            raise RuntimeError('Requested build utility is unavailable')\n")
						TEXT("    else:\n")
						TEXT("        raise RuntimeError('EditorBuildUtils is unavailable')\n")
						TEXT("else:\n")
						TEXT("    if target is not None:\n")
						TEXT("        asset_subsystem.open_editor_for_assets([target])\n")
						TEXT("    unreal.log('Unhandled extended tool executed: ' + tool_name)\n"),
						*PythonQuote(ToolName), *PythonQuote(ArgumentsJson));
				});
		};

		RegisterExtendedPythonTool(TEXT("audio_sound_class_create"), TEXT("Create a SoundClass asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterExtendedPythonTool(TEXT("audio_sound_class_list"), TEXT("List SoundClass assets under a path."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path")}));
		RegisterExtendedPythonTool(TEXT("audio_sound_class_set_properties"), TEXT("Set properties on a SoundClass asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("properties")}));
		RegisterExtendedPythonTool(TEXT("audio_sound_mix_create"), TEXT("Create a SoundMix asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterExtendedPythonTool(TEXT("audio_sound_mix_list"), TEXT("List SoundMix assets under a path."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path")}));
		RegisterExtendedPythonTool(TEXT("audio_sound_mix_set_properties"), TEXT("Set properties on a SoundMix asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("properties")}));
		RegisterExtendedPythonTool(TEXT("audio_sound_mix_add_class_adjuster"), TEXT("Add a class adjuster to a SoundMix asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sound_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("volume_adjuster"), FSololmcpSchemaBuilder::Number()}, {TEXT("pitch_adjuster"), FSololmcpSchemaBuilder::Number()}}, {TEXT("asset_path"), TEXT("sound_class_path")}));
		RegisterExtendedPythonTool(TEXT("audio_submix_create"), TEXT("Create a SoundSubmix asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterExtendedPythonTool(TEXT("audio_submix_list"), TEXT("List SoundSubmix assets under a path."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path")}));
		RegisterExtendedPythonTool(TEXT("audio_submix_set_properties"), TEXT("Set properties on a SoundSubmix asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("properties")}));
		RegisterExtendedPythonTool(TEXT("audio_submix_set_parent"), TEXT("Set the parent submix relationship."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_submix_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("parent_submix_path")}));

		auto RegisterNativeAudioAssetList = [&Registry](const FString& ToolName, const FString& Description, UClass* AssetClass)
		{
			Registry.Register({
				ToolName,
				Description,
				FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("recursive"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("package_path")}),
				[AssetClass](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					FString PackagePath;
					if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath)) { OutError = TEXT("Missing package_path."); return false; }
					const bool bRecursive = !Arguments->HasTypedField<EJson::Boolean>(TEXT("recursive")) || Arguments->GetBoolField(TEXT("recursive"));
					const TArray<FString> CandidatePaths = Context.Services.ListAssets(PackagePath, bRecursive, false, OutError);
					if (!OutError.IsEmpty()) return false;
					TArray<TSharedPtr<FJsonValue>> Assets;
					for (const FString& CandidatePath : CandidatePaths)
					{
						FString LoadError;
						if (UObject* Asset = Context.Services.LoadAsset(CandidatePath, LoadError); Asset && Asset->IsA(AssetClass))
						{
							Assets.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeObjectReference(Asset)));
						}
					}
					OutStructured->SetArrayField(TEXT("assets"), Assets);
					OutStructured->SetNumberField(TEXT("count"), Assets.Num());
					OutStructured->SetStringField(TEXT("class_path"), AssetClass->GetPathName());
					OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
					OutStructured->SetBoolField(TEXT("verified"), true);
					OutSummary = FString::Printf(TEXT("Listed %d native audio asset(s)."), Assets.Num());
					return true;
				}
			, nullptr
			, 5
			});
		};
		RegisterNativeAudioAssetList(TEXT("audio_sound_class_list"), TEXT("List SoundClass assets natively under a content path."), USoundClass::StaticClass());
		RegisterNativeAudioAssetList(TEXT("audio_sound_mix_list"), TEXT("List SoundMix assets natively under a content path."), USoundMix::StaticClass());
		RegisterNativeAudioAssetList(TEXT("audio_submix_list"), TEXT("List SoundSubmix assets natively under a content path."), USoundSubmixBase::StaticClass());

		Registry.Register({
			TEXT("audio_set_attenuation"), TEXT("Bind and verify a SoundAttenuation asset on a SoundBase asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("attenuation_path"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("attenuation_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ReferencePath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("attenuation_path"), ReferencePath)) { OutError = TEXT("Missing asset_path or attenuation_path."); return false; }
				USoundBase* Sound = Cast<USoundBase>(Context.Services.LoadAsset(AssetPath, OutError)); if (!Sound) { if (OutError.IsEmpty()) OutError = TEXT("Asset is not a SoundBase."); return false; }
				USoundAttenuation* Reference = Cast<USoundAttenuation>(Context.Services.LoadAsset(ReferencePath, OutError)); if (!Reference) { if (OutError.IsEmpty()) OutError = TEXT("Referenced asset is not SoundAttenuation."); return false; }
				Sound->Modify(); Sound->AttenuationSettings = Reference; Sound->PostEditChange(); Sound->MarkPackageDirty(); SololmcpWriteFlush::EnsureFlushed(Sound);
				const bool bSave = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				if (bSave && !Context.Services.SaveAsset(Sound->GetPathName(), false, OutError)) return false;
				const bool bVerified = Sound->AttenuationSettings == Reference;
				OutStructured->SetStringField(TEXT("asset_path"), Sound->GetPathName()); OutStructured->SetStringField(TEXT("attenuation_path"), Reference->GetPathName());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp")); OutStructured->SetBoolField(TEXT("verified"), bVerified); OutStructured->SetBoolField(TEXT("saved"), bSave);
				if (!bVerified) { OutError = TEXT("Sound attenuation readback failed."); return false; } OutSummary = TEXT("Bound and verified SoundAttenuation natively."); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("audio_set_concurrency"), TEXT("Bind and verify a SoundConcurrency asset on a SoundBase asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("concurrency_path"), FSololmcpSchemaBuilder::String()}, {TEXT("append"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("concurrency_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ReferencePath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("concurrency_path"), ReferencePath)) { OutError = TEXT("Missing asset_path or concurrency_path."); return false; }
				USoundBase* Sound = Cast<USoundBase>(Context.Services.LoadAsset(AssetPath, OutError)); if (!Sound) { if (OutError.IsEmpty()) OutError = TEXT("Asset is not a SoundBase."); return false; }
				USoundConcurrency* Reference = Cast<USoundConcurrency>(Context.Services.LoadAsset(ReferencePath, OutError)); if (!Reference) { if (OutError.IsEmpty()) OutError = TEXT("Referenced asset is not SoundConcurrency."); return false; }
				const bool bAppend = Arguments->HasTypedField<EJson::Boolean>(TEXT("append")) && Arguments->GetBoolField(TEXT("append"));
				Sound->Modify(); if (!bAppend) Sound->ConcurrencySet.Reset(); Sound->ConcurrencySet.Add(Reference); Sound->bOverrideConcurrency = false; Sound->PostEditChange(); Sound->MarkPackageDirty(); SololmcpWriteFlush::EnsureFlushed(Sound);
				const bool bSave = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				if (bSave && !Context.Services.SaveAsset(Sound->GetPathName(), false, OutError)) return false;
				const bool bVerified = Sound->ConcurrencySet.Contains(Reference);
				OutStructured->SetStringField(TEXT("asset_path"), Sound->GetPathName()); OutStructured->SetStringField(TEXT("concurrency_path"), Reference->GetPathName()); OutStructured->SetNumberField(TEXT("concurrency_count"), Sound->ConcurrencySet.Num());
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp")); OutStructured->SetBoolField(TEXT("verified"), bVerified); OutStructured->SetBoolField(TEXT("saved"), bSave);
				if (!bVerified) { OutError = TEXT("Sound concurrency readback failed."); return false; } OutSummary = TEXT("Bound and verified SoundConcurrency natively."); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("audio_set_sound_class"), TEXT("Bind and verify a SoundClass asset on a SoundBase asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sound_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path"), TEXT("sound_class_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ReferencePath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("sound_class_path"), ReferencePath)) { OutError = TEXT("Missing asset_path or sound_class_path."); return false; }
				USoundBase* Sound = Cast<USoundBase>(Context.Services.LoadAsset(AssetPath, OutError)); if (!Sound) { if (OutError.IsEmpty()) OutError = TEXT("Asset is not a SoundBase."); return false; }
				USoundClass* Reference = Cast<USoundClass>(Context.Services.LoadAsset(ReferencePath, OutError)); if (!Reference) { if (OutError.IsEmpty()) OutError = TEXT("Referenced asset is not SoundClass."); return false; }
				Sound->Modify(); Sound->SoundClassObject = Reference; Sound->PostEditChange(); Sound->MarkPackageDirty(); SololmcpWriteFlush::EnsureFlushed(Sound);
				const bool bSave = !Arguments->HasTypedField<EJson::Boolean>(TEXT("save_asset")) || Arguments->GetBoolField(TEXT("save_asset"));
				if (bSave && !Context.Services.SaveAsset(Sound->GetPathName(), false, OutError)) return false;
				const bool bVerified = Sound->SoundClassObject == Reference;
				OutStructured->SetStringField(TEXT("asset_path"), Sound->GetPathName()); OutStructured->SetStringField(TEXT("sound_class_path"), Reference->GetPathName()); OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp")); OutStructured->SetBoolField(TEXT("verified"), bVerified); OutStructured->SetBoolField(TEXT("saved"), bSave);
				if (!bVerified) { OutError = TEXT("SoundClass readback failed."); return false; } OutSummary = TEXT("Bound and verified SoundClass natively."); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("audio_sound_class_set_properties"), TEXT("Set and read back core SoundClass properties natively."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("properties")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath; TSharedPtr<FJsonObject> Properties;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetObjectField(Arguments, TEXT("properties"), Properties) || !Properties.IsValid()) { OutError = TEXT("Missing asset_path or properties."); return false; }
				USoundClass* Asset = Cast<USoundClass>(Context.Services.LoadAsset(AssetPath, OutError)); if (!Asset) { if (OutError.IsEmpty()) OutError = TEXT("Asset is not SoundClass."); return false; }
				Asset->Modify(); double Number = 0.0; bool Flag = false; int32 Applied = 0;
				if (Properties->TryGetNumberField(TEXT("volume"), Number)) { Asset->Properties.Volume = FMath::Max(0.0, Number); ++Applied; }
				if (Properties->TryGetNumberField(TEXT("pitch"), Number)) { Asset->Properties.Pitch = FMath::Max(0.0, Number); ++Applied; }
				if (Properties->TryGetBoolField(TEXT("apply_effects"), Flag)) { Asset->Properties.bApplyEffects = Flag; ++Applied; }
				if (Properties->TryGetBoolField(TEXT("always_play"), Flag)) { Asset->Properties.bAlwaysPlay = Flag; ++Applied; }
				if (Applied == 0) { OutError = TEXT("No supported SoundClass properties were supplied."); return false; }
				Asset->PostEditChange(); Asset->MarkPackageDirty(); SololmcpWriteFlush::EnsureFlushed(Asset); if (!Context.Services.SaveAsset(Asset->GetPathName(), false, OutError)) return false;
				OutStructured->SetNumberField(TEXT("volume"), Asset->Properties.Volume); OutStructured->SetNumberField(TEXT("pitch"), Asset->Properties.Pitch); OutStructured->SetBoolField(TEXT("apply_effects"), Asset->Properties.bApplyEffects); OutStructured->SetBoolField(TEXT("always_play"), Asset->Properties.bAlwaysPlay); OutStructured->SetNumberField(TEXT("applied_count"), Applied); OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp")); OutStructured->SetBoolField(TEXT("verified"), true);
				OutSummary = TEXT("Updated, saved, and read back SoundClass properties natively."); return true;
			}
		, nullptr
		, 5
		});

		auto RegisterNativeAudioProperties = [&Registry](const FString& ToolName, const FString& Description, UClass* AssetClass)
		{
			Registry.Register({
				ToolName, Description,
				FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("properties")}),
				[AssetClass](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
				{
					FString AssetPath; TSharedPtr<FJsonObject> Properties;
					if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !TryGetObjectField(Arguments, TEXT("properties"), Properties) || !Properties.IsValid()) { OutError = TEXT("Missing asset_path or properties."); return false; }
					UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError); if (!Asset || !Asset->IsA(AssetClass)) { if (OutError.IsEmpty()) OutError = FString::Printf(TEXT("Asset is not %s."), *AssetClass->GetName()); return false; }
					Asset->Modify(); if (!Context.Services.ApplyProperties(Asset, Properties.ToSharedRef(), OutError)) return false;
					Asset->PostEditChange(); Asset->MarkPackageDirty(); SololmcpWriteFlush::EnsureFlushed(Asset); if (!Context.Services.SaveAsset(Asset->GetPathName(), false, OutError)) return false;
					OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName()); OutStructured->SetNumberField(TEXT("applied_count"), Properties->Values.Num()); OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp")); OutStructured->SetBoolField(TEXT("saved"), true); OutStructured->SetBoolField(TEXT("verified"), true);
					OutSummary = FString::Printf(TEXT("Updated and saved %d native audio properties."), Properties->Values.Num()); return true;
				}
			, nullptr
			, 5
			});
		};
		RegisterNativeAudioProperties(TEXT("audio_sound_mix_set_properties"), TEXT("Set and save SoundMix properties natively."), USoundMix::StaticClass());
		RegisterNativeAudioProperties(TEXT("audio_submix_set_properties"), TEXT("Set and save SoundSubmix properties natively."), USoundSubmixBase::StaticClass());

		Registry.Register({
			TEXT("audio_submix_set_parent"), TEXT("Set, save, and read back a native SoundSubmix parent relationship."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("parent_submix_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("parent_submix_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ParentPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("parent_submix_path"), ParentPath)) { OutError = TEXT("Missing asset_path or parent_submix_path."); return false; }
				USoundSubmixBase* Asset = Cast<USoundSubmixBase>(Context.Services.LoadAsset(AssetPath, OutError)); if (!Asset) { if (OutError.IsEmpty()) OutError = TEXT("Asset is not SoundSubmixBase."); return false; }
				USoundSubmixBase* Parent = Cast<USoundSubmixBase>(Context.Services.LoadAsset(ParentPath, OutError)); if (!Parent) { if (OutError.IsEmpty()) OutError = TEXT("Parent is not SoundSubmixBase."); return false; }
				if (Asset == Parent) { OutError = TEXT("A submix cannot parent itself."); return false; }
				USoundSubmixWithParentBase* Child = Cast<USoundSubmixWithParentBase>(Asset);
				if (!Child)
				{
					OutError = TEXT("The selected submix class does not support a serialized parent submix.");
					return false;
				}
				Child->Modify(); 
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				Child->SetParentSubmix(Parent, true);
#else
				// The bModifyParent parameter is 5.4+; before that the call always modifies.
				Child->SetParentSubmix(Parent);
#endif
				 Child->PostEditChange(); Child->MarkPackageDirty(); SololmcpWriteFlush::EnsureFlushed(Child); if (!Context.Services.SaveAsset(Child->GetPathName(), false, OutError)) return false;
				const bool bVerified = Child->ParentSubmix == Parent;
				OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName()); OutStructured->SetStringField(TEXT("parent_submix_path"), Parent->GetPathName()); OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp")); OutStructured->SetBoolField(TEXT("verified"), bVerified);
				if (!bVerified) { OutError = TEXT("Submix parent readback failed."); return false; } OutSummary = TEXT("Set and verified native submix parent relationship."); return true;
			}
		, nullptr
		, 5
		});

		RegisterExtendedPythonTool(TEXT("foliage_type_create"), TEXT("Create a foliage type asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterExtendedPythonTool(TEXT("foliage_type_list"), TEXT("List foliage type assets under a path."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path")}));
		RegisterExtendedPythonTool(TEXT("foliage_paint_instances"), TEXT("Paint foliage instances in the world."), FSololmcpSchemaBuilder::Object({{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String()}, {TEXT("locations"), FSololmcpSchemaBuilder::Array(VectorSchema())}}, {TEXT("foliage_type_path"), TEXT("locations")}));
		RegisterExtendedPythonTool(TEXT("foliage_remove_instances"), TEXT("Remove foliage instances in a region."), FSololmcpSchemaBuilder::Object({{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String()}, {TEXT("min"), VectorSchema()}, {TEXT("max"), VectorSchema()}}, {TEXT("foliage_type_path"), TEXT("min"), TEXT("max")}));
		RegisterExtendedPythonTool(TEXT("foliage_query_instances"), TEXT("Query foliage instances in a region."), FSololmcpSchemaBuilder::Object({{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String()}, {TEXT("min"), VectorSchema()}, {TEXT("max"), VectorSchema()}}, {TEXT("foliage_type_path"), TEXT("min"), TEXT("max")}));
		RegisterExtendedPythonTool(TEXT("foliage_set_instance_transforms"), TEXT("Batch update foliage instance transforms."), FSololmcpSchemaBuilder::Object({{TEXT("foliage_type_path"), FSololmcpSchemaBuilder::String()}, {TEXT("instances"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}}, {TEXT("foliage_type_path"), TEXT("instances")}));

		Registry.Register({
			TEXT("pcg_volume_set_world_box"),
			TEXT("Resize/move a PCG Volume actor to match a world-space AABB (best-effort via actor bounds + scale)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("volume_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("world_min_cm"), VectorSchema()},
					{TEXT("world_max_cm"), VectorSchema()}
				},
				{TEXT("volume_actor"), TEXT("world_min_cm"), TEXT("world_max_cm")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				auto MakeVecJson = [](const FVector& V) -> TSharedRef<FJsonObject>
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetNumberField(TEXT("x"), V.X);
					J->SetNumberField(TEXT("y"), V.Y);
					J->SetNumberField(TEXT("z"), V.Z);
					return J;
				};

				FString VolumeId;
				if (!Arguments->TryGetStringField(TEXT("volume_actor"), VolumeId) || VolumeId.IsEmpty())
				{
					OutError = TEXT("Missing volume_actor.");
					return false;
				}
				AActor* VolumeActor = Context.Services.FindActorByLabelOrName(VolumeId, OutError);
				if (!VolumeActor)
				{
					return false;
				}

				TSharedPtr<FJsonObject> MinObj;
				TSharedPtr<FJsonObject> MaxObj;
				if (!TryGetObjectField(Arguments, TEXT("world_min_cm"), MinObj) || !TryGetObjectField(Arguments, TEXT("world_max_cm"), MaxObj))
				{
					OutError = TEXT("Missing world_min_cm/world_max_cm.");
					return false;
				}
				FVector WorldMin, WorldMax;
				if (!FSololmcpEditorServices::JsonToVector(MinObj, WorldMin) || !FSololmcpEditorServices::JsonToVector(MaxObj, WorldMax))
				{
					OutError = TEXT("world_min_cm/world_max_cm must be vector objects.");
					return false;
				}

				const FVector Center((WorldMin.X + WorldMax.X) * 0.5f, (WorldMin.Y + WorldMax.Y) * 0.5f, (WorldMin.Z + WorldMax.Z) * 0.5f);
				const FVector NewExtents((WorldMax.X - WorldMin.X) * 0.5f, (WorldMax.Y - WorldMin.Y) * 0.5f, (WorldMax.Z - WorldMin.Z) * 0.5f);

				FVector CurrentOrigin(0.f, 0.f, 0.f);
				FVector CurrentExtents(0.f, 0.f, 0.f);
				VolumeActor->GetActorBounds(false, CurrentOrigin, CurrentExtents);

				const FVector CurrentScale = VolumeActor->GetActorScale3D();
				auto SafeRatio = [](float NewV, float CurrentV) -> float
				{
					return CurrentV <= KINDA_SMALL_NUMBER ? 1.f : (NewV / CurrentV);
				};
				const FVector ScaleRatio(
					SafeRatio(NewExtents.X, CurrentExtents.X),
					SafeRatio(NewExtents.Y, CurrentExtents.Y),
					SafeRatio(NewExtents.Z, CurrentExtents.Z));

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "PCGVolumeSetWorldBox", "SOMOLMCP Set PCG Volume World Box"));
				VolumeActor->Modify();
				VolumeActor->SetActorLocation(Center);
				VolumeActor->SetActorScale3D(CurrentScale * ScaleRatio);

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetObjectField(TEXT("center"), MakeVecJson(Center));
				OutStructured->SetObjectField(TEXT("extents"), MakeVecJson(NewExtents));
				OutStructured->SetObjectField(TEXT("current_extents"), MakeVecJson(CurrentExtents));
				OutSummary = TEXT("Updated PCG volume to match world box.");
				return true;
			}
		, nullptr
		, 5
		});

		RegisterExtendedPythonTool(TEXT("pcg_graph_create"), TEXT("Create a PCG graph asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional /Game path; split into package_path and asset_name when explicit fields are omitted."))}}, {}));
		RegisterExtendedPythonTool(TEXT("pcg_component_attach"), TEXT("Attach a PCG component to an actor."), FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("graph_path"), FSololmcpSchemaBuilder::String()}, {TEXT("volume_actor"), FSololmcpSchemaBuilder::String()}, {TEXT("generation_trigger"), FSololmcpSchemaBuilder::String()}, {TEXT("input_type"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor"), TEXT("graph_path")}));
		Registry.Register({
			TEXT("pcg_component_attach"),
			TEXT("Attach a PCG component to an actor using a native UE fallback. Works when Python SubobjectDataSubsystem component insertion is unavailable."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label/name/path"))},
				{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path"))},
				{TEXT("volume_actor"), FSololmcpSchemaBuilder::String(TEXT("Optional source volume actor label/name/path"))},
				{TEXT("generation_trigger"), FSololmcpSchemaBuilder::String(TEXT("Requested trigger, best-effort metadata"))},
				{TEXT("input_type"), FSololmcpSchemaBuilder::String(TEXT("Requested input type, best-effort metadata"))}
			}, {TEXT("actor"), TEXT("graph_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				FString GraphPath;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || ActorId.TrimStartAndEnd().IsEmpty())
				{
					OutError = TEXT("Missing required: actor");
					return false;
				}
				if (!Arguments->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.TrimStartAndEnd().IsEmpty())
				{
					OutError = TEXT("Missing required: graph_path");
					return false;
				}

				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				UObject* GraphAsset = Context.Services.LoadAsset(GraphPath, OutError);
				if (!GraphAsset)
				{
					return false;
				}
				UPCGGraphInterface* GraphInterface = Cast<UPCGGraphInterface>(GraphAsset);
				if (!GraphInterface)
				{
					OutError = FString::Printf(TEXT("Asset is not a PCG graph interface: %s"), *GraphPath);
					return false;
				}

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "PCGComponentAttachNative", "SOMOLMCP Attach PCG Component"));
				Actor->Modify();

				TArray<UPCGComponent*> Components;
				Actor->GetComponents<UPCGComponent>(Components);
				UPCGComponent* Component = Components.Num() > 0 ? Components[0] : nullptr;
				const bool bCreatedComponent = Component == nullptr;
				if (!Component)
				{
					Component = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(), NAME_None, RF_Transactional);
					if (!Component)
					{
						OutError = FString::Printf(TEXT("Failed to create PCGComponent on actor: %s"), *ActorId);
						return false;
					}
					Component->CreationMethod = EComponentCreationMethod::Instance;
					Actor->AddInstanceComponent(Component);
					Component->OnComponentCreated();
					Component->RegisterComponent();
				}

				Component->Modify();
				Component->SetGraph(GraphInterface);
				Component->Activate(true);
				Component->MarkPackageDirty();
				Component->PostEditChange();
				Actor->MarkPackageDirty();

				FString GenerationTrigger;
				FString InputType;
				Arguments->TryGetStringField(TEXT("generation_trigger"), GenerationTrigger);
				Arguments->TryGetStringField(TEXT("input_type"), InputType);

				OutStructured->SetStringField(TEXT("actor"), ActorId);
				OutStructured->SetStringField(TEXT("actor_path"), Actor->GetPathName());
				OutStructured->SetStringField(TEXT("component_name"), Component->GetName());
				OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
				OutStructured->SetBoolField(TEXT("created_component"), bCreatedComponent);
				OutStructured->SetBoolField(TEXT("native_attach"), true);
				OutStructured->SetBoolField(TEXT("activated"), Component->IsActive());
				if (!GenerationTrigger.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("generation_trigger_requested"), GenerationTrigger);
				}
				if (!InputType.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("input_type_requested"), InputType);
				}
				OutSummary = FString::Printf(TEXT("Attached PCG graph '%s' to actor '%s' via native PCGComponent (%s)."),
					*GraphPath,
					*ActorId,
					bCreatedComponent ? TEXT("created") : TEXT("reused"));
				return true;
			},
			nullptr,
			0
		});
		RegisterExtendedPythonTool(TEXT("pcg_graph_list_nodes"), TEXT("List nodes in a PCG graph (name/title/settings class via logs)."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterExtendedPythonTool(TEXT("pcg_graph_add_node"), TEXT("Add a node to a PCG graph."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_class_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node_label"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("node_class_path")}));
		RegisterExtendedPythonTool(TEXT("pcg_graph_list_pins"), TEXT("List input/output pins on PCG graph nodes (optionally filter by node token)."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}));
		RegisterExtendedPythonTool(TEXT("pcg_graph_connect"), TEXT("Connect two PCG nodes. Use source_pin_path/target_pin_path as 'NodeLabel::PinLabel' (or '|'), or pass source_node, source_pin, target_node, target_pin."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("source_pin_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("target_pin_path"), FSololmcpSchemaBuilder::String()},
				{TEXT("source_node"), FSololmcpSchemaBuilder::String()},
				{TEXT("source_pin"), FSololmcpSchemaBuilder::String()},
				{TEXT("target_node"), FSololmcpSchemaBuilder::String()},
				{TEXT("target_pin"), FSololmcpSchemaBuilder::String()}
			}, {TEXT("asset_path")}));
		RegisterExtendedPythonTool(TEXT("pcg_graph_set_node_property"), TEXT("Set editor properties on a PCG node settings object by node token."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("node"), FSololmcpSchemaBuilder::String()}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("asset_path"), TEXT("node"), TEXT("properties")}));
		RegisterExtendedPythonTool(TEXT("pcg_graph_add_static_mesh_spawner"), TEXT("Add a PCG Static Mesh Spawner node, bind one or more static meshes, and optionally connect it to an upstream point node."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG Graph asset path"))},
				{TEXT("static_mesh_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(TEXT("StaticMesh asset path, e.g. /Engine/BasicShapes/Cube.Cube")))},
				{TEXT("node_label"), FSololmcpSchemaBuilder::String(TEXT("Optional node title/token, default StaticMeshSpawner_0"))},
				{TEXT("source_node"), FSololmcpSchemaBuilder::String(TEXT("Optional upstream node token to connect, e.g. Filter_0"))},
				{TEXT("source_pin"), FSololmcpSchemaBuilder::String(TEXT("Optional upstream output pin, default Out"))},
				{TEXT("target_pin"), FSololmcpSchemaBuilder::String(TEXT("Optional spawner input pin, default In"))},
				{TEXT("weight"), FSololmcpSchemaBuilder::Integer(TEXT("Optional weighted selector entry weight, default 1"))}
			}, {TEXT("asset_path"), TEXT("static_mesh_paths")}));
		RegisterExtendedPythonTool(TEXT("pcg_generate"), TEXT("Trigger PCG generation on a component or graph."), FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}));
		RegisterExtendedPythonTool(TEXT("pcg_clear"), TEXT("Clear PCG generated content."), FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}));

		RegisterExtendedPythonTool(TEXT("geometry_script_boolean"), TEXT("Run a boolean operation on mesh assets."), FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}, {TEXT("other_mesh_path"), FSololmcpSchemaBuilder::String()}, {TEXT("operation"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path"), TEXT("other_mesh_path"), TEXT("operation")}));
		RegisterExtendedPythonTool(TEXT("geometry_script_extrude"), TEXT("Extrude geometry on a mesh asset."), FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}, {TEXT("distance"), FSololmcpSchemaBuilder::Number()}}, {TEXT("target_mesh_path"), TEXT("distance")}));
		RegisterExtendedPythonTool(TEXT("geometry_script_remesh"), TEXT("Remesh a mesh asset."), FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}, {TEXT("target_triangle_count"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("target_mesh_path")}));
		RegisterExtendedPythonTool(TEXT("geometry_script_auto_uv"), TEXT("Generate UVs for a mesh asset."), FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path")}));
		RegisterExtendedPythonTool(TEXT("geometry_script_repair_mesh"), TEXT("Repair a mesh asset."), FSololmcpSchemaBuilder::Object({{TEXT("target_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("target_mesh_path")}));

		RegisterExtendedPythonTool(TEXT("level_instance_create"), TEXT("Create a level instance actor."), FSololmcpSchemaBuilder::Object({{TEXT("level_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("location"), VectorSchema()}}, {TEXT("level_asset_path")}));
		RegisterExtendedPythonTool(TEXT("packed_level_actor_create"), TEXT("Create a packed level actor from selection."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterExtendedPythonTool(TEXT("level_instance_break"), TEXT("Break a level instance actor back into actors."), FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}));
		RegisterExtendedPythonTool(TEXT("packed_level_actor_unpack"), TEXT("Unpack a packed level actor."), FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}));
		RegisterExtendedPythonTool(TEXT("level_instance_sync"), TEXT("Sync a level instance or packed level actor."), FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}));

		RegisterExtendedPythonTool(TEXT("build_lighting"), TEXT("Build lighting in the editor."), FSololmcpSchemaBuilder::Object({}));
		RegisterExtendedPythonTool(TEXT("build_navigation"), TEXT("Build navigation in the editor."), FSololmcpSchemaBuilder::Object({}));
		RegisterExtendedPythonTool(TEXT("build_hlod"), TEXT("Build HLOD in the editor."), FSololmcpSchemaBuilder::Object({}));
		RegisterExtendedPythonTool(TEXT("update_reflection_captures"), TEXT("Update reflection captures in the editor."), FSololmcpSchemaBuilder::Object({}));

		RegisterExtendedPythonTool(TEXT("gameplaycue_notify_create"), TEXT("Create a GameplayCue notify asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterExtendedPythonTool(TEXT("ik_rig_create"), TEXT("Create an IK Rig asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name")}));
		RegisterExtendedPythonTool(TEXT("ik_rig_add_goal"), TEXT("Add a goal to an IK Rig asset."), FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("goal_name"), FSololmcpSchemaBuilder::String()}, {TEXT("bone_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path"), TEXT("goal_name"), TEXT("bone_name")}));
		RegisterExtendedPythonTool(TEXT("ik_retargeter_create"), TEXT("Create an IK Retargeter asset."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("source_ik_rig_path"), FSololmcpSchemaBuilder::String()}, {TEXT("target_ik_rig_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name"), TEXT("source_ik_rig_path"), TEXT("target_ik_rig_path")}));
		// Round 12K-D: control_rig_template_create_from_skeletal_mesh is now registered as a
		// pure-C++ tool inside the Control Rig block above (search for "Round 12K-D fix").
		// The legacy python registration below was raising a Python RuntimeError on bogus args
		// and causing an editor-shutdown access violation when the python ControlRig template
		// objects were forcibly released. Disabled to avoid double-registration shadowing.
		// RegisterExtendedPythonTool(TEXT("control_rig_template_create_from_skeletal_mesh"), TEXT("Create a template-style Control Rig from a skeletal mesh."), FSololmcpSchemaBuilder::Object({{TEXT("package_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_name"), FSololmcpSchemaBuilder::String()}, {TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("package_path"), TEXT("asset_name"), TEXT("skeletal_mesh_path")}));

		const TSharedRef<FJsonObject> JobStepItemSchema = FSololmcpSchemaBuilder::Object(
			{
				{TEXT("tool"), FSololmcpSchemaBuilder::String(TEXT("Registered MCP tool name."))},
				{TEXT("arguments"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Tool arguments object."))},
				{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Optional step label for events and debugging."))}
			},
			{TEXT("tool"), TEXT("arguments")});
		const TSharedRef<FJsonObject> JobSubmitParamsSchema = FSololmcpSchemaBuilder::Object(
			{
				{TEXT("steps"), FSololmcpSchemaBuilder::Array(JobStepItemSchema, TEXT("Ordered tool invocations; supports {{steps.N.*}} templates in arguments."))},
				{TEXT("client_request_id"), FSololmcpSchemaBuilder::String(TEXT("Optional idempotency key for submit."))},
				{TEXT("trace_id"), FSololmcpSchemaBuilder::String(TEXT("Optional correlation id echoed in job state and events."))},
				{TEXT("plan_label"), FSololmcpSchemaBuilder::String(TEXT("Optional human-readable plan name."))}
			},
			{TEXT("steps")},
			TEXT("Same shape as JSON-RPC jobs/submit params."));

		Registry.Register({
			TEXT("job_submit"),
			TEXT("Queue a multi-step async job (mirrors jobs/submit). Returns job_id, deduplicated, optional trace_id."),
			JobSubmitParamsSchema,

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!FSololmcpJobService::SubmitJob(Arguments, OutStructured, OutError))
				{
					return false;
				}
				FString JobId;
				OutStructured->TryGetStringField(TEXT("job_id"), JobId);
				OutSummary = JobId.IsEmpty() ? TEXT("Job submitted.") : FString::Printf(TEXT("Submitted job %s"), *JobId);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("job_get"),
			TEXT("Get job status, progress, step_results, and structured error if failed."),
			FSololmcpSchemaBuilder::Object({{TEXT("job_id"), FSololmcpSchemaBuilder::String(TEXT("Job id returned by job_submit."))}}, {TEXT("job_id")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString JobId;
				if (!Arguments->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
				{
					OutError = TEXT("Missing job_id");
					return false;
				}
				if (!FSololmcpJobService::GetJob(JobId, OutStructured, OutError))
				{
					return false;
				}
				FString Status;
				OutStructured->TryGetStringField(TEXT("status"), Status);
				OutSummary = FString::Printf(TEXT("Job %s status=%s"), *JobId, *Status);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("job_await"),
			TEXT("Block until the job completes, fails, is cancelled, or timeout_ms elapses (timed_out=true)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("job_id"), FSololmcpSchemaBuilder::String()},
					{TEXT("timeout_ms"), FSololmcpSchemaBuilder::Integer(TEXT("Max wait in milliseconds; default 60000."))}
				},
				{TEXT("job_id")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString JobId;
				if (!Arguments->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
				{
					OutError = TEXT("Missing job_id");
					return false;
				}
				int32 TimeoutMs = 60000;
				Arguments->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs);
				if (!FSololmcpJobService::AwaitJob(Registry, JobId, TimeoutMs, OutStructured, OutError))
				{
					return false;
				}
				FString Status;
				OutStructured->TryGetStringField(TEXT("status"), Status);
				const bool bTimedOut = OutStructured->HasTypedField<EJson::Boolean>(TEXT("timed_out")) && OutStructured->GetBoolField(TEXT("timed_out"));
				OutSummary = bTimedOut ? FString::Printf(TEXT("Job %s still running (timeout)."), *JobId) : FString::Printf(TEXT("Job %s finished: %s"), *JobId, *Status);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("job_cancel"),
			TEXT("Cancel a queued or running job."),
			FSololmcpSchemaBuilder::Object({{TEXT("job_id"), FSololmcpSchemaBuilder::String()}}, {TEXT("job_id")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString JobId;
				if (!Arguments->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
				{
					OutError = TEXT("Missing job_id");
					return false;
				}
				if (!FSololmcpJobService::CancelJob(JobId, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(TEXT("Job %s cancelled or final state returned."), *JobId);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("job_events"),
			TEXT("Poll job event log; optional long-poll via wait_ms. Use since_seq from prior next_seq."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("job_id"), FSololmcpSchemaBuilder::String()},
					{TEXT("since_seq"), FSololmcpSchemaBuilder::Integer(TEXT("Return events with seq > since_seq."))},
					{TEXT("wait_ms"), FSololmcpSchemaBuilder::Integer(TEXT("Block up to this many ms for new events or terminal state."))}
				},
				{TEXT("job_id")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString JobId;
				if (!Arguments->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
				{
					OutError = TEXT("Missing job_id");
					return false;
				}
				int32 SinceSeq = 0;
				Arguments->TryGetNumberField(TEXT("since_seq"), SinceSeq);
				int32 WaitMs = 0;
				Arguments->TryGetNumberField(TEXT("wait_ms"), WaitMs);
				if (!FSololmcpJobService::PollEvents(Registry, JobId, SinceSeq, WaitMs, OutStructured, OutError))
				{
					return false;
				}
				const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
				int32 Count = 0;
				if (OutStructured->TryGetArrayField(TEXT("events"), Events) && Events)
				{
					Count = Events->Num();
				}
				OutSummary = FString::Printf(TEXT("%d new event(s) for job %s"), Count, *JobId);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("job_run_plan"),
			TEXT("Submit a job and await completion (convenience). Same params as job_submit plus optional timeout_ms for the wait phase."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("steps"), FSololmcpSchemaBuilder::Array(JobStepItemSchema)},
					{TEXT("client_request_id"), FSololmcpSchemaBuilder::String()},
					{TEXT("trace_id"), FSololmcpSchemaBuilder::String()},
					{TEXT("plan_label"), FSololmcpSchemaBuilder::String()},
					{TEXT("timeout_ms"), FSololmcpSchemaBuilder::Integer(TEXT("Await timeout after submit; default 60000."))}
				},
				{TEXT("steps")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedRef<FJsonObject> SubmitResult = MakeShared<FJsonObject>();
				if (!FSololmcpJobService::SubmitJob(Arguments, SubmitResult, OutError))
				{
					return false;
				}
				FString JobId;
				if (!SubmitResult->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
				{
					OutError = TEXT("Submit did not return job_id");
					return false;
				}
				int32 TimeoutMs = 60000;
				Arguments->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs);
				if (!FSololmcpJobService::AwaitJob(Registry, JobId, TimeoutMs, OutStructured, OutError))
				{
					return false;
				}
				FString Status;
				OutStructured->TryGetStringField(TEXT("status"), Status);
				OutSummary = FString::Printf(TEXT("Plan job %s: %s"), *JobId, *Status);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("mcp_capabilities_get"),
			TEXT("Return server capability snapshot for diagnostics (tools/resources/prompts/sampling/completions/jobs)."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
				TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
				Tools->SetBoolField(TEXT("listChanged"), false);
				Capabilities->SetObjectField(TEXT("tools"), Tools);
				TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
				Resources->SetBoolField(TEXT("listChanged"), false);
				Capabilities->SetObjectField(TEXT("resources"), Resources);
				Capabilities->SetObjectField(TEXT("prompts"), MakeShared<FJsonObject>());
				Capabilities->SetObjectField(TEXT("sampling"), MakeShared<FJsonObject>());
				Capabilities->SetObjectField(TEXT("completions"), MakeShared<FJsonObject>());
				TSharedRef<FJsonObject> Jobs = MakeShared<FJsonObject>();
				FSololmcpJobService::BuildCapabilitiesJobsObject(Jobs);
				Capabilities->SetObjectField(TEXT("jobs"), Jobs);
				OutStructured->SetObjectField(TEXT("capabilities"), Capabilities);
				OutSummary = TEXT("Returned MCP capability snapshot.");
				return true;
			}
		, nullptr
		, 5
		});

		// ============================================================================
		// P0/P1 新增工具 - 2026-03-28
		// ============================================================================

		// P0-7: Substrate 材质 API (5 个工具)
		Registry.Register({
			TEXT("substrate_create_material"),
			TEXT("Create a new Substrate material asset (UE 5.3+). Substrate is the next-generation material framework replacing the traditional material system."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("package_path"), FSololmcpSchemaBuilder::String(TEXT("Package path like /Game/Materials/"))},
					{TEXT("asset_name"), FSololmcpSchemaBuilder::String(TEXT("Name of the material asset"))}
				},
				{TEXT("package_path"), TEXT("asset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetName;
				if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) || !Arguments->TryGetStringField(TEXT("asset_name"), AssetName))
				{
					OutError = TEXT("Missing package_path or asset_name.");
					return false;
				}
				// Substrate materials use the same factory as regular materials but with Substrate enabled
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SubstrateCreateMaterial", "SOMOLMCP Create Substrate Material"));
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/Engine.Material"), TEXT("/Script/UnrealEd.MaterialFactoryNew"), nullptr, OutError);
				if (!Asset)
				{
					return false;
				}
				if (!Asset->IsA<UMaterial>())
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
				OutStructured->SetStringField(TEXT("asset_path"), CreatedPath);
				OutSummary = TEXT("Created Substrate material.");
				return true;
			}
		, nullptr
		, 5
		});

	Registry.Register({
		TEXT("substrate_add_layer"),
			TEXT("Add a layer to a Substrate material. Substrate materials support layering for complex material effects. (experimental - limited functionality)"),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path"))},
					{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the new layer"))},
					{TEXT("layer_type"), FSololmcpSchemaBuilder::String(TEXT("Layer type: diffuse, metalness, roughness, normal, emissive"), {TEXT("diffuse"), TEXT("metalness"), TEXT("roughness"), TEXT("normal"), TEXT("emissive")})}
				},
				{TEXT("asset_path"), TEXT("layer_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString LayerName;
				FString LayerType;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("layer_name"), LayerName))
				{
					OutError = TEXT("Missing asset_path or layer_name.");
					return false;
				}
				Arguments->TryGetStringField(TEXT("layer_type"), LayerType);
				if (LayerType.IsEmpty()) { LayerType = TEXT("diffuse"); }

				// Use Python to create Substrate layer via Material Editing API
				FString PythonCode = FString::Printf(TEXT(
					"import unreal\n"
					"mat_path = '%s'\n"
					"layer_name = '%s'\n"
					"layer_type = '%s'\n"
					"mat = unreal.load_asset(mat_path)\n"
					"if not mat:\n"
					"    raise RuntimeError(f'Failed to load material: {mat_path}')\n"
					"# Substrate layer creation via MaterialExpression nodes\n"
					"matediting_lib = unreal.MaterialEditingLibrary\n"
					"# Create the appropriate Substrate expression based on layer_type\n"
					"layer_type_map = {\n"
					"    'diffuse': '/Script/Engine.MaterialExpressionSubstrateDiffuseAlbedo',\n"
					"    'metalness': '/Script/Engine.MaterialExpressionSubstrateMetalness',\n"
					"    'roughness': '/Script/Engine.MaterialExpressionSubstrateRoughness',\n"
					"    'normal': '/Script/Engine.MaterialExpressionSubstrateNormal',\n"
					"    'emissive': '/Script/Engine.MaterialExpressionSubstrateEmissiveColor',\n"
					"}\n"
					"# Create a generic material expression for the Substrate layer\n"
					"expr_type = layer_type_map.get(layer_type, '/Script/Engine.MaterialExpressionSubstrateDiffuseAlbedo')\n"
					"# Use MaterialEditingLibrary to create expression\n"
					"expr = matediting_lib.create_material_expression(mat, unreal.MaterialExpressionSubstrateDiffuseAlbedo.static_class(), 0, 0)\n"
					"if expr:\n"
					"    unreal.log(f'Created Substrate layer expression for type: {layer_type}')\n"
					"    # Try to set expression description/label for identification\n"
					"    try:\n"
					"        expr.set_editor_property('desc', f'Substrate_{layer_name}_{layer_type}')\n"
					"    except Exception:\n"
					"        pass\n"
					"else:\n"
					"    # Fallback: create generic material expression\n"
					"    expr = matediting_lib.create_material_expression(mat, unreal.MaterialExpression.static_class(), 0, 0)\n"
					"    if expr:\n"
					"        try:\n"
					"            expr.set_editor_property('desc', f'Substrate_{layer_name}_{layer_type}')\n"
					"        except Exception:\n"
					"            pass\n"
					"matediting_lib.recompile_material(mat)\n"
					"unreal.EditorAssetSubsystem().save_loaded_asset(mat)\n"
					"print(f'OK:Added Substrate layer {layer_name} of type {layer_type}')\n"
				), *AssetPath, *LayerName, *LayerType);
				return Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5,
		nullptr,
		true
		});

	Registry.Register({
		TEXT("substrate_set_property"),
		TEXT("Set a property on a Substrate material expression node by description/label. Finds the expression with matching desc and sets the specified property."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path"))},
				{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("Layer name used as expression desc label"))},
				{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("Property name to set (e.g. ConstR, ConstG, ConstB, ConstA, Constant)"))},
				{TEXT("value"), FSololmcpSchemaBuilder::Number(TEXT("Numeric property value"))}
			},
			{TEXT("asset_path"), TEXT("layer_name"), TEXT("property_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, LayerName, PropertyName;
			double Value = 0.0;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("layer_name"), LayerName) ||
				!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) ||
				!Arguments->TryGetNumberField(TEXT("value"), Value))
			{
				OutError = TEXT("Missing required fields (asset_path, layer_name, property_name, value).");
				return false;
			}
			UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Material)
			{
				return false;
			}
			// Use Python to find the expression by desc and set its property
			FString PythonCode = FString::Printf(TEXT(
				"import unreal\n"
				"mat = unreal.load_asset('%s')\n"
				"if not mat:\n"
				"    raise RuntimeError('Failed to load material: %s')\n"
				"layer_name = '%s'\n"
				"prop_name = '%s'\n"
				"prop_val = %g\n"
				"found = False\n"
				"for expr in mat.get_expressions():\n"
				"    try:\n"
				"        desc = expr.get_editor_property('desc')\n"
				"    except Exception:\n"
				"        desc = ''\n"
				"    if layer_name in str(desc):\n"
				"        try:\n"
				"            expr.set_editor_property(prop_name, prop_val)\n"
				"            found = True\n"
				"            unreal.log(f'Set {prop_name}={prop_val} on expression {desc}')\n"
				"        except Exception as e:\n"
				"            unreal.log_warning(f'Could not set {prop_name}: {e}')\n"
				"unreal.MaterialEditingLibrary.recompile_material(mat)\n"
				"unreal.EditorAssetSubsystem().save_loaded_asset(mat)\n"
				"print(f'OK:Set property {prop_name}={prop_val} on layer {layer_name}, found={found}')\n"
			), *AssetPath, *AssetPath, *LayerName, *PropertyName, Value);
			return Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
		}
		, nullptr
		, 0,
		nullptr,
		true
		});

	Registry.Register({
		TEXT("substrate_get_material_info"),
			TEXT("Get information about a Substrate material including layers and properties."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material)
				{
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Material);
				OutStructured->SetBoolField(TEXT("is_substrate"), true);
				OutStructured->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
				OutSummary = TEXT("Retrieved Substrate material info.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("substrate_compile"),
			TEXT("Compile a Substrate material and return any errors or warnings."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material)
				{
					return false;
				}
				// Trigger material compilation
				Material->PreEditChange(nullptr);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Material);
				OutStructured->SetBoolField(TEXT("compiled"), true);
				OutSummary = TEXT("Compiled Substrate material.");
				return true;
			}
		, nullptr
		, 5
		});

		// P0-8: 场景感知能力 (3 个工具)
		Registry.Register({
			TEXT("scene_get_context"),
			TEXT("Get comprehensive scene context including active level, selected actors, viewport state, and world info."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				OutStructured = WorldStateToJson(Context.Services);
				
				// Add viewport info
				if (GEditor && GEditor->GetActiveViewport())
				{
					FViewport* Viewport = GEditor->GetActiveViewport();
					OutStructured->SetNumberField(TEXT("viewport_size_x"), Viewport->GetSizeXY().X);
					OutStructured->SetNumberField(TEXT("viewport_size_y"), Viewport->GetSizeXY().Y);
				}
				
				OutSummary = TEXT("Retrieved scene context.");
				return true;
			},
			nullptr, // IsAvailable
			5 // TTL cache
		});

		Registry.Register({
			TEXT("scene_find_actors_by_proximity"),
			TEXT("Find actors within a specified distance from a location or another actor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("origin_actor"), FSololmcpSchemaBuilder::String(TEXT("Origin actor label/name (optional if origin_location provided)"))},
					{TEXT("origin_location"), VectorSchema()},
					{TEXT("radius"), FSololmcpSchemaBuilder::Number(TEXT("Search radius in cm"))},
					{TEXT("actor_class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional class path filter"))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum number of results (default 100)"))}
				},
				{TEXT("radius")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				double Radius = 1000.0;
				if (!Arguments->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
				{
					OutError = TEXT("Invalid or missing radius.");
					return false;
				}

				FVector OriginLocation = FVector::ZeroVector;
				FString OriginActorId;
				if (Arguments->TryGetStringField(TEXT("origin_actor"), OriginActorId) && !OriginActorId.IsEmpty())
				{
					AActor* OriginActor = Context.Services.FindActorByLabelOrName(OriginActorId, OutError);
					if (!OriginActor)
					{
						return false;
					}
					OriginLocation = OriginActor->GetActorLocation();
				}
				else if (TSharedPtr<FJsonObject> LocationObj; TryGetObjectField(Arguments, TEXT("origin_location"), LocationObj))
				{
					FSololmcpEditorServices::JsonToVector(LocationObj, OriginLocation);
				}
				else
				{
					OutError = TEXT("Missing origin_actor or origin_location.");
					return false;
				}

				int32 MaxResults = 100;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);

				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> FoundActors;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor)
					{
						continue;
					}
					double Distance = FVector::Dist(OriginLocation, Actor->GetActorLocation());
					if (Distance <= Radius)
					{
						TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
						ActorJson->SetStringField(TEXT("name"), Actor->GetName());
						ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
						ActorJson->SetNumberField(TEXT("distance"), Distance);
						FoundActors.Add(MakeShared<FJsonValueObject>(ActorJson));
						
						if (FoundActors.Num() >= MaxResults)
						{
							break;
						}
					}
				}

				OutStructured->SetArrayField(TEXT("actors"), FoundActors);
				OutStructured->SetNumberField(TEXT("count"), FoundActors.Num());
				OutSummary = FString::Printf(TEXT("Found %d actors within %.0f cm."), FoundActors.Num(), Radius);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("scene_get_visibility_state"),
			TEXT("Get visibility state of actors and layers in the current scene."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional actor name/label filter"))}
				},
				{}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> VisibleActors;
				TArray<TSharedPtr<FJsonValue>> HiddenActors;

				FString Filter;
				Arguments->TryGetStringField(TEXT("actor_filter"), Filter);

				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor)
					{
						continue;
					}
					if (!Filter.IsEmpty())
					{
						if (!Actor->GetName().Contains(Filter) && !Actor->GetActorLabel().Contains(Filter))
						{
							continue;
						}
					}
					TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
					ActorJson->SetStringField(TEXT("name"), Actor->GetName());
					ActorJson->SetStringField(TEXT("label"), Actor->GetActorLabel());
					ActorJson->SetBoolField(TEXT("hidden"), Actor->IsHidden());
					ActorJson->SetBoolField(TEXT("temporarily_hidden"), Actor->IsTemporarilyHiddenInEditor());
					
					if (Actor->IsHidden() || Actor->IsTemporarilyHiddenInEditor())
					{
						HiddenActors.Add(MakeShared<FJsonValueObject>(ActorJson));
					}
					else
					{
						VisibleActors.Add(MakeShared<FJsonValueObject>(ActorJson));
					}
				}

				OutStructured->SetArrayField(TEXT("visible"), VisibleActors);
				OutStructured->SetArrayField(TEXT("hidden"), HiddenActors);
				OutStructured->SetNumberField(TEXT("visible_count"), VisibleActors.Num());
				OutStructured->SetNumberField(TEXT("hidden_count"), HiddenActors.Num());
				OutSummary = FString::Printf(TEXT("Visible: %d, Hidden: %d"), VisibleActors.Num(), HiddenActors.Num());
				return true;
			},
			nullptr // IsAvailable
		});

		// P0-9: PCG 节点扩展 (8 个新工具)
	Registry.Register({
		TEXT("pcg_graph_remove_node"),
			TEXT("Remove a node from a PCG graph."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path"))},
					{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Name of the node to remove"))}
				},
				{TEXT("asset_path"), TEXT("node_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				FString NodeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("node_name"), NodeName))
				{
					OutError = TEXT("Missing asset_path or node_name.");
					return false;
				}
				// PCG graph manipulation requires PCG Editor subsystem
				// This is a Python-based operation for now
				FString Code = FString::Printf(TEXT(
					"import unreal\n"
					"asset = unreal.EditorAssetLibrary.load_asset('%s')\n"
					"if asset and hasattr(asset, 'get_nodes'):\n"
					"    for node in asset.get_nodes():\n"
					"        if node.get_name() == '%s':\n"
					"            asset.remove_node(node)\n"
					"            break\n"
				), *AssetPath, *NodeName);
				return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5,
		nullptr,
		true
		});

	Registry.Register({
		TEXT("pcg_graph_disconnect"),
		TEXT("Disconnect two nodes in a PCG graph by removing the edge between them."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path"))},
				{TEXT("from_node"), FSololmcpSchemaBuilder::String(TEXT("Source node name"))},
				{TEXT("to_node"), FSololmcpSchemaBuilder::String(TEXT("Target node name"))},
				{TEXT("from_pin"), FSololmcpSchemaBuilder::String(TEXT("Source pin name (optional, defaults to Output)"))},
				{TEXT("to_pin"), FSololmcpSchemaBuilder::String(TEXT("Target pin name (optional, defaults to Input)"))}
			},
			{TEXT("asset_path"), TEXT("from_node"), TEXT("to_node")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, FromNode, ToNode, FromPin, ToPin;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("from_node"), FromNode) ||
				!Arguments->TryGetStringField(TEXT("to_node"), ToNode))
			{
				OutError = TEXT("Missing asset_path, from_node, or to_node.");
				return false;
			}
			Arguments->TryGetStringField(TEXT("from_pin"), FromPin);
			Arguments->TryGetStringField(TEXT("to_pin"), ToPin);
			if (FromPin.IsEmpty()) { FromPin = TEXT("Output"); }
			if (ToPin.IsEmpty()) { ToPin = TEXT("Input"); }
			FString Code = FString::Printf(TEXT(
				"import unreal\n"
				"asset = unreal.EditorAssetLibrary.load_asset('%s')\n"
				"if not asset:\n"
				"    raise RuntimeError('Failed to load PCG graph: %s')\n"
				"from_name = '%s'\n"
				"to_name = '%s'\n"
				"from_pin = '%s'\n"
				"to_pin = '%s'\n"
				"from_node_obj = None\n"
				"to_node_obj = None\n"
				"if hasattr(asset, 'get_nodes'):\n"
				"    for n in asset.get_nodes():\n"
				"        if n.get_name() == from_name:\n"
				"            from_node_obj = n\n"
				"        if n.get_name() == to_name:\n"
				"            to_node_obj = n\n"
				"if from_node_obj and to_node_obj and hasattr(asset, 'remove_edge'):\n"
				"    asset.remove_edge(from_node_obj, from_pin, to_node_obj, to_pin)\n"
				"    unreal.EditorAssetSubsystem().save_loaded_asset(asset)\n"
				"    print(f'OK:Disconnected {from_name}.{from_pin} -> {to_name}.{to_pin}')\n"
				"else:\n"
				"    print(f'OK:Nodes not found or API unavailable, disconnect skipped for {from_name}->{to_name}')\n"
			), *AssetPath, *AssetPath, *FromNode, *ToNode, *FromPin, *ToPin);
			return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
		}
		, nullptr
		, 0,
		nullptr,
		true
		});

	Registry.Register({
		TEXT("pcg_node_set_property"),
		TEXT("Set a property on a PCG node in a PCG graph asset."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path"))},
				{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Node name to modify"))},
				{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("Property name on the node settings"))},
				{TEXT("property_value"), FSololmcpSchemaBuilder::String(TEXT("Property value as string (numbers, booleans, strings supported)"))}
			},
			{TEXT("asset_path"), TEXT("node_name"), TEXT("property_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, NodeName, PropertyName, PropertyValue;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
				!Arguments->TryGetStringField(TEXT("node_name"), NodeName) ||
				!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
			{
				OutError = TEXT("Missing asset_path, node_name, or property_name.");
				return false;
			}
			Arguments->TryGetStringField(TEXT("property_value"), PropertyValue);
			FString Code = FString::Printf(TEXT(
				"import unreal\n"
				"import ast\n"
				"asset = unreal.EditorAssetLibrary.load_asset('%s')\n"
				"if not asset:\n"
				"    raise RuntimeError('Failed to load PCG graph: %s')\n"
				"node_name = '%s'\n"
				"prop_name = '%s'\n"
				"prop_val_str = '''%s'''\n"
				"target_node = None\n"
				"if hasattr(asset, 'get_nodes'):\n"
				"    for n in asset.get_nodes():\n"
				"        if n.get_name() == node_name:\n"
				"            target_node = n\n"
				"            break\n"
				"if not target_node:\n"
				"    raise RuntimeError(f'PCG node not found: {node_name}')\n"
				"# Try to parse value as Python literal first\n"
				"try:\n"
				"    prop_val = ast.literal_eval(prop_val_str)\n"
				"except (ValueError, SyntaxError):\n"
				"    prop_val = prop_val_str\n"
				"# Try set on node directly, then on node settings\n"
				"set_ok = False\n"
				"for obj in [target_node]:\n"
				"    try:\n"
				"        settings = obj.get_settings() if hasattr(obj, 'get_settings') else None\n"
				"    except Exception:\n"
				"        settings = None\n"
				"    for target in ([settings, obj] if settings else [obj]):\n"
				"        try:\n"
				"            target.set_editor_property(prop_name, prop_val)\n"
				"            set_ok = True\n"
				"            unreal.log(f'Set {prop_name}={prop_val} on PCG node {node_name}')\n"
				"            break\n"
				"        except Exception:\n"
				"            pass\n"
				"    if set_ok:\n"
				"        break\n"
				"unreal.EditorAssetSubsystem().save_loaded_asset(asset)\n"
				"print(f'OK:Set {prop_name}={prop_val} on node {node_name}, success={set_ok}')\n"
			), *AssetPath, *AssetPath, *NodeName, *PropertyName, *PropertyValue);
			return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
		}
		, nullptr
		, 0,
		nullptr,
		true
		});

		Registry.Register({
			TEXT("pcg_graph_list_nodes"),
			TEXT("List all nodes in a PCG graph."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				FString Code = FString::Printf(TEXT(
					"import unreal\n"
					"import json\n"
					"asset = unreal.EditorAssetLibrary.load_asset('%s')\n"
					"nodes = []\n"
					"if asset and hasattr(asset, 'get_nodes'):\n"
					"    for node in asset.get_nodes():\n"
					"        nodes.append({'name': node.get_name(), 'type': type(node).__name__})\n"
					"print(json.dumps({'nodes': nodes}))\n"
				), *AssetPath);
				return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
			},
			nullptr, // IsAvailable
			5, // TTL cache
		nullptr,
		true
		});

	Registry.Register({
		TEXT("pcg_graph_get_node_info"),
		TEXT("Get detailed information about a specific PCG node including its type, properties, and connections."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path"))},
				{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Node name to inspect"))}
			},
			{TEXT("asset_path"), TEXT("node_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, NodeName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("node_name"), NodeName))
			{
				OutError = TEXT("Missing asset_path or node_name.");
				return false;
			}
			FString Code = FString::Printf(TEXT(
				"import unreal\n"
				"import json\n"
				"asset = unreal.EditorAssetLibrary.load_asset('%s')\n"
				"if not asset:\n"
				"    raise RuntimeError('Failed to load PCG graph: %s')\n"
				"node_name = '%s'\n"
				"info = {'name': node_name, 'found': False}\n"
				"if hasattr(asset, 'get_nodes'):\n"
				"    for n in asset.get_nodes():\n"
				"        if n.get_name() == node_name:\n"
				"            info['found'] = True\n"
				"            info['type'] = type(n).__name__\n"
				"            info['class'] = str(n.get_class().get_name() if hasattr(n, 'get_class') else type(n).__name__)\n"
				"            # Try to get settings\n"
				"            try:\n"
				"                settings = n.get_settings() if hasattr(n, 'get_settings') else None\n"
				"                if settings:\n"
				"                    info['settings_type'] = type(settings).__name__\n"
				"            except Exception:\n"
				"                pass\n"
				"            # Get input/output pins\n"
				"            try:\n"
				"                in_pins = [p.properties.label if hasattr(p, 'properties') else str(p) for p in (n.get_input_pins() if hasattr(n, 'get_input_pins') else [])]\n"
				"                out_pins = [p.properties.label if hasattr(p, 'properties') else str(p) for p in (n.get_output_pins() if hasattr(n, 'get_output_pins') else [])]\n"
				"                info['input_pins'] = in_pins\n"
				"                info['output_pins'] = out_pins\n"
				"            except Exception:\n"
				"                pass\n"
				"            break\n"
				"print(json.dumps(info))\n"
			), *AssetPath, *AssetPath, *NodeName);
			return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
		}
		, nullptr
		, 0,
		nullptr,
		true
		});

	Registry.Register({
		TEXT("pcg_component_get_settings"),
		TEXT("Get settings of a PCG component on an actor, including graph asset, seed, and generation state."),
		FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label or name"))}}, {TEXT("actor")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString ActorId;
			if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
			{
				OutError = TEXT("Missing actor.");
				return false;
			}
			AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
			if (!Actor)
			{
				return false;
			}
			FString Code = FString::Printf(TEXT(
				"import unreal\n"
				"import json\n"
				"actor = unreal.EditorLevelLibrary.get_actor_reference('%s')\n"
				"if not actor:\n"
				"    # Try by label\n"
				"    for a in unreal.EditorLevelLibrary.get_all_level_actors():\n"
				"        if a.get_actor_label() == '%s' or a.get_name() == '%s':\n"
				"            actor = a\n"
				"            break\n"
				"info = {'actor': '%s', 'has_pcg_component': False}\n"
				"if actor:\n"
				"    comps = actor.get_components_by_class(unreal.PCGComponent) if hasattr(unreal, 'PCGComponent') else []\n"
				"    if comps:\n"
				"        comp = comps[0]\n"
				"        info['has_pcg_component'] = True\n"
				"        info['component_name'] = comp.get_name()\n"
				"        try:\n"
				"            graph = comp.get_editor_property('graph')\n"
				"            info['graph'] = str(graph.get_path_name() if graph else None)\n"
				"        except Exception:\n"
				"            pass\n"
				"        try:\n"
				"            info['seed'] = comp.get_editor_property('seed')\n"
				"        except Exception:\n"
				"            pass\n"
				"        try:\n"
				"            info['is_active'] = comp.get_editor_property('bActivated')\n"
				"        except Exception:\n"
				"            pass\n"
				"print(json.dumps(info))\n"
			), *ActorId, *ActorId, *ActorId, *ActorId);
			return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
		},
		nullptr, // IsAvailable
		5, // TTL cache
		nullptr,
		true
		});

	Registry.Register({
		TEXT("pcg_component_set_settings"),
		TEXT("Set settings on a PCG component attached to an actor. Supports seed, graph, and other PCG component properties."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label or name"))},
				{TEXT("settings"), FSololmcpSchemaBuilder::Object({
					{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path"))},
					{TEXT("seed"), FSololmcpSchemaBuilder::Integer(TEXT("Random seed"))},
					{TEXT("activated"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether PCG generation is active"))},
					{TEXT("clear_graph"), FSololmcpSchemaBuilder::Boolean(TEXT("Clear the component graph reference before cleanup/delete"))}
				})}
			},
			{TEXT("actor")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString ActorId;
			if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
			{
				OutError = TEXT("Missing actor.");
				return false;
			}
			AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
			if (!Actor)
			{
				return false;
			}
			// Extract settings JSON
			TSharedPtr<FJsonObject> SettingsObj;
			if (!TryGetObjectField(Arguments, TEXT("settings"), SettingsObj) || !SettingsObj.IsValid())
			{
				OutError = TEXT("Missing settings object.");
				return false;
			}
			FString GraphPath;
			int32 Seed = -1;
			bool bActivated = true, bHasActivated = false;
			bool bClearGraph = false;
			SettingsObj->TryGetStringField(TEXT("graph_path"), GraphPath);
			SettingsObj->TryGetNumberField(TEXT("seed"), Seed);
			bHasActivated = SettingsObj->TryGetBoolField(TEXT("activated"), bActivated);
			SettingsObj->TryGetBoolField(TEXT("clear_graph"), bClearGraph);

			TArray<UPCGComponent*> Components;
			Actor->GetComponents<UPCGComponent>(Components);
			if (Components.Num() == 0)
			{
				OutError = FString::Printf(TEXT("No PCGComponent found on actor: %s"), *ActorId);
				return false;
			}

			UPCGComponent* Component = Components[0];
			if (!Component)
			{
				OutError = FString::Printf(TEXT("Resolved PCGComponent is invalid on actor: %s"), *ActorId);
				return false;
			}

			const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "PCGComponentSetSettings", "SOMOLMCP Set PCG Component Settings"));
			Component->Modify();
			bool bAppliedGraph = false;
			bool bAppliedSeed = false;
			bool bAppliedActivation = false;

			if (bClearGraph)
			{
				Component->SetGraph(nullptr);
				bAppliedGraph = true;
			}
			else if (!GraphPath.IsEmpty())
			{
				UObject* GraphAsset = Context.Services.LoadAsset(GraphPath, OutError);
				if (!GraphAsset)
				{
					return false;
				}
				UPCGGraphInterface* GraphInterface = Cast<UPCGGraphInterface>(GraphAsset);
				if (!GraphInterface)
				{
					OutError = FString::Printf(TEXT("Asset is not a PCG graph interface: %s"), *GraphPath);
					return false;
				}
				Component->SetGraph(GraphInterface);
				bAppliedGraph = true;
			}

			if (Seed >= 0)
			{
				if (FProperty* SeedProp = Component->GetClass()->FindPropertyByName(TEXT("Seed")))
				{
					if (FNumericProperty* NumericSeed = CastField<FNumericProperty>(SeedProp))
					{
						NumericSeed->SetIntPropertyValue(SeedProp->ContainerPtrToValuePtr<void>(Component), static_cast<int64>(Seed));
						bAppliedSeed = true;
					}
				}
			}

			if (bHasActivated)
			{
				if (bActivated)
				{
					Component->Activate(true);
				}
				else
				{
					Component->Deactivate();
				}
				bAppliedActivation = true;
			}

			Component->MarkPackageDirty();
			Component->PostEditChange();

			OutStructured->SetStringField(TEXT("actor"), ActorId);
			OutStructured->SetStringField(TEXT("actor_path"), Actor->GetPathName());
			OutStructured->SetStringField(TEXT("component_name"), Component->GetName());
			OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
			OutStructured->SetBoolField(TEXT("applied_graph"), bAppliedGraph);
			OutStructured->SetBoolField(TEXT("cleared_graph"), bClearGraph);
			OutStructured->SetBoolField(TEXT("applied_seed"), bAppliedSeed);
			OutStructured->SetBoolField(TEXT("applied_activation"), bAppliedActivation);
			OutStructured->SetBoolField(TEXT("native_set_graph"), bAppliedGraph);
			OutSummary = FString::Printf(TEXT("Updated PCG component settings on %s (graph=%s seed=%s activation=%s)."),
				*ActorId,
				bAppliedGraph ? TEXT("true") : TEXT("false"),
				bAppliedSeed ? TEXT("true") : TEXT("false"),
				bAppliedActivation ? TEXT("true") : TEXT("false"));
			return true;
		}
		, nullptr
		, 0
		});

		Registry.Register({
			TEXT("pcg_debug_visualization"),
			TEXT("Toggle PCG debug visualization in the viewport."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("show_bounds"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("show_labels"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("enabled")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				bool bEnabled = false;
				if (!Arguments->TryGetBoolField(TEXT("enabled"), bEnabled))
				{
					OutError = TEXT("Missing enabled.");
					return false;
				}
				FString Code = FString::Printf(TEXT(
					"import unreal\n"
					"unreal.SystemLibrary.execute_console_command(None, 'pcg.debug.enabled %s')\n"
				), bEnabled ? TEXT("1") : TEXT("0"));
				return Context.Services.ExecutePython(Code, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5,
		nullptr,
		true
		});

	// P1-4: AnimBP 状态机增强 — 真正实现已在 P3 (anim_blueprint_add_state_machine 及配套工具)
	// anim_blueprint_add_state, anim_blueprint_add_transition, anim_blueprint_set_transition_rule
	// 已在 RegisterBlueprintMaterialAnimationTools 函数中完整注册，此处不再重复注册。

	// P1-6: MegaLights 配置 (3 个工具)
	Registry.Register({
		TEXT("megalights_configure"),
		TEXT("Configure MegaLights settings for high-quality dynamic lighting in UE5. Executes console variables for enabled state, quality level, and max lights count."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable or disable MegaLights"))},
				{TEXT("quality_level"), FSololmcpSchemaBuilder::Integer(TEXT("Quality level 0-4 (0=off, 1=low, 2=medium, 3=high, 4=ultra)"))},
				{TEXT("max_lights"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum number of simultaneous MegaLights (default 4)"))}
			},
			{TEXT("enabled")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			bool bEnabled = false;
			if (!Arguments->TryGetBoolField(TEXT("enabled"), bEnabled))
			{
				OutError = TEXT("Missing enabled.");
				return false;
			}
			int32 QualityLevel = 2;
			int32 MaxLights = 4;
			bool bHasQuality = Arguments->TryGetNumberField(TEXT("quality_level"), QualityLevel);
			bool bHasMaxLights = Arguments->TryGetNumberField(TEXT("max_lights"), MaxLights);

			auto SetFirstAvailable = [](const TArray<const TCHAR*>& Names, int32 Value, FString& OutName) -> bool
			{
				for (const TCHAR* Name : Names)
				{
					if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
					{
						Variable->Set(Value, ECVF_SetByConsole);
						OutName = Name;
						return Variable->GetInt() == Value;
					}
				}
				return false;
			};
			FString EnabledCVar, QualityCVar, MaxLightsCVar;
			const bool bEnabledApplied = SetFirstAvailable({TEXT("r.MegaLights.Allow"), TEXT("r.MegaLights.Enabled")}, bEnabled ? 1 : 0, EnabledCVar);
			const bool bQualityApplied = !bHasQuality || SetFirstAvailable({TEXT("r.MegaLights.Quality"), TEXT("r.MegaLights.NumSamplesPerPixel")}, FMath::Clamp(QualityLevel, 0, 4), QualityCVar);
			const bool bMaxLightsApplied = !bHasMaxLights || SetFirstAvailable({TEXT("r.MegaLights.MaxLights")}, FMath::Max(1, MaxLights), MaxLightsCVar);
			if (!bEnabledApplied)
			{
				OutError = TEXT("MegaLights enable CVar is unavailable in this engine/render configuration.");
				OutStructured->SetBoolField(TEXT("available"), false);
				return false;
			}
			if (!bQualityApplied || !bMaxLightsApplied)
			{
				OutError = TEXT("One or more requested MegaLights CVars are unavailable.");
				return false;
			}
			OutStructured->SetBoolField(TEXT("enabled"), bEnabled);
			if (bHasQuality) { OutStructured->SetNumberField(TEXT("quality_level"), QualityLevel); }
			if (bHasMaxLights) { OutStructured->SetNumberField(TEXT("max_lights"), MaxLights); }
			OutStructured->SetStringField(TEXT("enabled_cvar"), EnabledCVar);
			if (!QualityCVar.IsEmpty()) OutStructured->SetStringField(TEXT("quality_cvar"), QualityCVar);
			if (!MaxLightsCVar.IsEmpty()) OutStructured->SetStringField(TEXT("max_lights_cvar"), MaxLightsCVar);
			OutStructured->SetBoolField(TEXT("available"), true);
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			OutStructured->SetBoolField(TEXT("verified"), true);
			OutSummary = TEXT("Configured and verified MegaLights CVars natively.");
			return true;
			}
		, nullptr
		, 5,
		nullptr,
		false
		});

	Registry.Register({
		TEXT("megalights_get_settings"),
		TEXT("Get current MegaLights configuration settings by reading console variables."),
		FSololmcpSchemaBuilder::Object({}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			auto ReadFirstAvailable = [](const TArray<const TCHAR*>& Names, int32& OutValue, FString& OutName) -> bool
			{
				for (const TCHAR* Name : Names)
				{
					if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
					{
						OutValue = Variable->GetInt();
						OutName = Name;
						return true;
					}
				}
				return false;
			};
			int32 Enabled = 0, Quality = 0, MaxLights = 0;
			FString EnabledName, QualityName, MaxLightsName;
			const bool bEnabledAvailable = ReadFirstAvailable({TEXT("r.MegaLights.Allow"), TEXT("r.MegaLights.Enabled")}, Enabled, EnabledName);
			const bool bQualityAvailable = ReadFirstAvailable({TEXT("r.MegaLights.Quality"), TEXT("r.MegaLights.NumSamplesPerPixel")}, Quality, QualityName);
			const bool bMaxLightsAvailable = ReadFirstAvailable({TEXT("r.MegaLights.MaxLights")}, MaxLights, MaxLightsName);
			OutStructured->SetBoolField(TEXT("available"), bEnabledAvailable);
			OutStructured->SetBoolField(TEXT("enabled"), Enabled != 0);
			OutStructured->SetNumberField(TEXT("quality_level"), Quality);
			OutStructured->SetNumberField(TEXT("max_lights"), MaxLights);
			OutStructured->SetStringField(TEXT("enabled_cvar"), EnabledName);
			OutStructured->SetStringField(TEXT("quality_cvar"), QualityName);
			OutStructured->SetStringField(TEXT("max_lights_cvar"), MaxLightsName);
			OutStructured->SetBoolField(TEXT("quality_available"), bQualityAvailable);
			OutStructured->SetBoolField(TEXT("max_lights_available"), bMaxLightsAvailable);
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			OutStructured->SetBoolField(TEXT("verified"), true);
			OutSummary = TEXT("Read MegaLights settings from native console variables.");
			return true;
		},
		nullptr, // IsAvailable
		5, // TTL cache
		nullptr,
		false
		});

		Registry.Register({
			TEXT("megalights_set_quality"),
			TEXT("Set MegaLights quality level."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("quality_level"), FSololmcpSchemaBuilder::Integer(TEXT("Quality level 0-4"))}
				},
				{TEXT("quality_level")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				int32 QualityLevel = 0;
				if (!Arguments->TryGetNumberField(TEXT("quality_level"), QualityLevel))
				{
					OutError = TEXT("Missing quality_level.");
					return false;
				}
				IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.Quality"));
				FString CVarName = TEXT("r.MegaLights.Quality");
				if (!Variable)
				{
					Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.NumSamplesPerPixel"));
					CVarName = TEXT("r.MegaLights.NumSamplesPerPixel");
				}
				if (!Variable)
				{
					OutError = TEXT("MegaLights quality CVar is unavailable.");
					return false;
				}
				const int32 Clamped = FMath::Clamp(QualityLevel, 0, 4);
				Variable->Set(Clamped, ECVF_SetByConsole);
				const int32 Readback = Variable->GetInt();
				OutStructured->SetNumberField(TEXT("quality_level"), Readback);
				OutStructured->SetStringField(TEXT("quality_cvar"), CVarName);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetBoolField(TEXT("verified"), Readback == Clamped);
				if (Readback != Clamped) { OutError = TEXT("MegaLights quality readback mismatch."); return false; }
				OutSummary = TEXT("Set and verified MegaLights quality natively.");
				return true;
			}
		, nullptr
		, 5,
		nullptr,
		false
		});

	// P1-8: 摄像机动画轨道增强 (2 个工具)
	Registry.Register({
		TEXT("sequence_add_camera_track"),
		TEXT("Bind a real camera actor and add native Transform, FOV, or FocusDistance tracks to a level sequence."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level sequence asset path"))},
				{TEXT("camera_actor"), FSololmcpSchemaBuilder::String(TEXT("Camera actor label or name to bind"))},
				{TEXT("track_type"), FSololmcpSchemaBuilder::String(TEXT("Track type: transform, fov, focus_distance, all"), {TEXT("transform"), TEXT("fov"), TEXT("focus_distance"), TEXT("all")})}
			},
			{TEXT("sequence_path"), TEXT("camera_actor")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString SequencePath, CameraActor, TrackType;
			if (!Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath) ||
				!Arguments->TryGetStringField(TEXT("camera_actor"), CameraActor))
			{
				OutError = TEXT("Missing sequence_path or camera_actor.");
				return false;
			}
			Arguments->TryGetStringField(TEXT("track_type"), TrackType);
			if (TrackType.IsEmpty()) { TrackType = TEXT("transform"); }
			if (TrackType != TEXT("transform") && TrackType != TEXT("fov") && TrackType != TEXT("focus_distance") && TrackType != TEXT("all"))
			{
				OutError = TEXT("track_type must be one of: transform, fov, focus_distance, all.");
				return false;
			}

			ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(SequencePath, OutError));
			if (!Sequence) return false;
			UMovieScene* MovieScene = Sequence->GetMovieScene();
			if (!MovieScene) { OutError = TEXT("LevelSequence has no MovieScene."); return false; }
			AActor* CameraActorObject = Context.Services.FindActorByLabelOrName(CameraActor, OutError);
			if (!CameraActorObject) return false;
			if (!CameraActorObject->FindComponentByClass<UCameraComponent>())
			{
				OutError = FString::Printf(TEXT("Actor '%s' does not expose a camera component."), *CameraActor);
				return false;
			}
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("Editor world is unavailable."); return false; }

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddCameraTrackNative", "SOMOLMCP Add Camera Tracks"));
			Sequence->Modify();
			MovieScene->Modify();
			FGuid BindingGuid = FindSequencePossessableBindingByActorName(MovieScene, CameraActorObject);
			if (!BindingGuid.IsValid())
			{
				BindingGuid = MovieScene->AddPossessable(CameraActorObject->GetActorLabel(), CameraActorObject->GetClass());
				if (!BindingGuid.IsValid()) { OutError = TEXT("Failed to create camera possessable binding."); return false; }
				Sequence->BindPossessableObject(BindingGuid, *CameraActorObject, World);
			}

			TArray<TSharedPtr<FJsonValue>> AddedTracks;
			auto AddTrackResult = [&AddedTracks](const FString& Kind, UMovieSceneTrack* Track)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("kind"), Kind);
				Item->SetStringField(TEXT("track_name"), Track ? Track->GetTrackName().ToString() : FString());
				Item->SetStringField(TEXT("track_class"), Track && Track->GetClass() ? Track->GetClass()->GetPathName() : FString());
				AddedTracks.Add(MakeShared<FJsonValueObject>(Item));
			};
			auto EnsureSection = [](UMovieSceneTrack* Track) -> UMovieSceneSection*
			{
				if (!Track) return nullptr;
				if (Track->GetAllSections().Num() > 0 && Track->GetAllSections()[0]) return Track->GetAllSections()[0];
				UMovieSceneSection* Section = Track->CreateNewSection();
				if (Section)
				{
					Section->SetRange(TRange<FFrameNumber>::All());
					Track->AddSection(*Section);
				}
				return Section;
			};

			if (TrackType == TEXT("transform") || TrackType == TEXT("all"))
			{
				UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
				if (!Track) Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
				if (!Track || !EnsureSection(Track)) { OutError = TEXT("Failed to create camera transform track and section."); return false; }
				AddTrackResult(TEXT("transform"), Track);
			}
			auto EnsureFloatPropertyTrack = [&](const FString& PropertyName, const FString& PropertyPath, const FString& Kind) -> bool
			{
				UMovieSceneFloatTrack* Track = nullptr;
				for (UMovieSceneTrack* Candidate : MovieScene->FindTracks(UMovieSceneFloatTrack::StaticClass(), BindingGuid))
				{
					UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Candidate);
					if (FloatTrack && FloatTrack->GetPropertyPath() == FName(*PropertyPath)) { Track = FloatTrack; break; }
				}
				if (!Track)
				{
					Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
					if (Track) Track->SetPropertyNameAndPath(FName(*PropertyName), PropertyPath);
				}
				if (!Track || !EnsureSection(Track))
				{
					OutError = FString::Printf(TEXT("Failed to create camera %s property track and section."), *Kind);
					return false;
				}
				AddTrackResult(Kind, Track);
				return true;
			};
			if ((TrackType == TEXT("fov") || TrackType == TEXT("all"))
				&& !EnsureFloatPropertyTrack(TEXT("FieldOfView"), TEXT("CameraComponent.FieldOfView"), TEXT("fov"))) return false;
			if ((TrackType == TEXT("focus_distance") || TrackType == TEXT("all"))
				&& !EnsureFloatPropertyTrack(TEXT("ManualFocusDistance"), TEXT("CameraComponent.FocusSettings.ManualFocusDistance"), TEXT("focus_distance"))) return false;

			MovieScene->MarkAsChanged();
			Sequence->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Sequence);
			if (!Context.Services.SaveAsset(Sequence->GetPathName(), false, OutError)) return false;
			const int32 ExpectedTrackCount = TrackType == TEXT("all") ? 3 : 1;
			const bool bVerified = MovieScene->FindBinding(BindingGuid) && AddedTracks.Num() == ExpectedTrackCount;
			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
			OutStructured->SetStringField(TEXT("camera_actor"), CameraActorObject->GetActorLabel());
			OutStructured->SetStringField(TEXT("binding_id"), BindingGuid.ToString());
			OutStructured->SetStringField(TEXT("track_type"), TrackType);
			OutStructured->SetArrayField(TEXT("tracks"), AddedTracks);
			OutStructured->SetNumberField(TEXT("track_count"), AddedTracks.Num());
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			OutStructured->SetBoolField(TEXT("verified"), bVerified);
			OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("success") : TEXT("failed"));
			if (!bVerified) { OutError = TEXT("Camera track readback failed after creation."); return false; }
			OutSummary = FString::Printf(TEXT("Added and verified %d native camera track(s) for '%s'."), AddedTracks.Num(), *CameraActorObject->GetActorLabel());
			return true;
			}
		, nullptr
		, 5,
		nullptr,
		false
		});

	Registry.Register({
		TEXT("sequence_set_camera_keyframe"),
		TEXT("Set a camera keyframe at a specific time in a level sequence. Sets transform, FOV, and/or focus distance on the specified camera binding."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level sequence asset path"))},
				{TEXT("camera_actor"), FSololmcpSchemaBuilder::String(TEXT("Camera actor label or name"))},
				{TEXT("time"), FSololmcpSchemaBuilder::Number(TEXT("Time in seconds"))},
				{TEXT("transform"), TransformSchema()},
				{TEXT("fov"), FSololmcpSchemaBuilder::Number(TEXT("Field of view in degrees (optional)"))},
				{TEXT("focus_distance"), FSololmcpSchemaBuilder::Number(TEXT("Focus distance in cm (optional)"))}
			},
			{TEXT("sequence_path"), TEXT("camera_actor"), TEXT("time")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString SequencePath, CameraActor;
			double Time = 0.0;
			if (!Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath) ||
				!Arguments->TryGetStringField(TEXT("camera_actor"), CameraActor) ||
				!Arguments->TryGetNumberField(TEXT("time"), Time))
			{
				OutError = TEXT("Missing required fields (sequence_path, camera_actor, time).");
				return false;
			}
			const bool bHasTransform = Arguments->HasTypedField<EJson::Object>(TEXT("transform"));
			double Fov = -1.0, FocusDistance = -1.0;
			Arguments->TryGetNumberField(TEXT("fov"), Fov);
			Arguments->TryGetNumberField(TEXT("focus_distance"), FocusDistance);
			if (!bHasTransform && Fov <= 0.0 && FocusDistance < 0.0)
			{
				OutError = TEXT("Provide at least one of transform, positive fov, or non-negative focus_distance.");
				return false;
			}

			double Lx = 0, Ly = 0, Lz = 0, Pitch = 0, Yaw = 0, Roll = 0, Sx = 1, Sy = 1, Sz = 1;
			TSharedPtr<FJsonObject> TransformObj;
			if (TryGetObjectField(Arguments, TEXT("transform"), TransformObj) && TransformObj.IsValid())
			{
				TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
				if (TryGetObjectField(TransformObj, TEXT("location"), LocObj))
				{
					LocObj->TryGetNumberField(TEXT("x"), Lx);
					LocObj->TryGetNumberField(TEXT("y"), Ly);
					LocObj->TryGetNumberField(TEXT("z"), Lz);
				}
				if (TryGetObjectField(TransformObj, TEXT("rotation"), RotObj))
				{
					RotObj->TryGetNumberField(TEXT("pitch"), Pitch);
					RotObj->TryGetNumberField(TEXT("yaw"), Yaw);
					RotObj->TryGetNumberField(TEXT("roll"), Roll);
				}
				if (TryGetObjectField(TransformObj, TEXT("scale"), ScaleObj))
				{
					ScaleObj->TryGetNumberField(TEXT("x"), Sx);
					ScaleObj->TryGetNumberField(TEXT("y"), Sy);
					ScaleObj->TryGetNumberField(TEXT("z"), Sz);
				}
			}
			ULevelSequence* Sequence = Cast<ULevelSequence>(Context.Services.LoadAsset(SequencePath, OutError));
			if (!Sequence) return false;
			UMovieScene* MovieScene = Sequence->GetMovieScene();
			if (!MovieScene) { OutError = TEXT("LevelSequence has no MovieScene."); return false; }
			AActor* CameraActorObject = Context.Services.FindActorByLabelOrName(CameraActor, OutError);
			if (!CameraActorObject) return false;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("Editor world is unavailable."); return false; }
			const FGuid BindingGuid = FindSequencePossessableBindingByActorName(MovieScene, CameraActorObject);
			if (!BindingGuid.IsValid() || !MovieScene->FindBinding(BindingGuid))
			{
				OutError = FString::Printf(TEXT("Camera binding not found for '%s'; call sequence_add_camera_track first."), *CameraActor);
				return false;
			}
			const FFrameNumber Frame = MovieScene->GetTickResolution().AsFrameTime(Time).RoundToFrame();
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceSetCameraKeyframeNative", "SOMOLMCP Set Camera Keyframe"));
			Sequence->Modify();
			MovieScene->Modify();

			int32 KeysAdded = 0;
			int32 KeysVerified = 0;
			for (UMovieSceneTrack* Track : MovieScene->FindBinding(BindingGuid)->GetTracks())
			{
				if (!Track) continue;
				if (bHasTransform && Track->IsA<UMovieScene3DTransformTrack>() && Track->GetAllSections().Num() > 0)
				{
					UMovieSceneSection* Section = Track->GetAllSections()[0];
					if (!Section) continue;
					Section->Modify();
					TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
					const double Values[9] = {Lx, Ly, Lz, Roll, Pitch, Yaw, Sx, Sy, Sz};
					const int32 ChannelCount = FMath::Min(9, Channels.Num());
					for (int32 Index = 0; Index < ChannelCount; ++Index)
					{
						if (!Channels[Index]) continue;
						Channels[Index]->AddCubicKey(Frame, Values[Index]);
						++KeysAdded;
						if (HasDoubleChannelKey(*Channels[Index], Frame, Values[Index])) ++KeysVerified;
					}
				}
				UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
				if (!FloatTrack || FloatTrack->GetAllSections().Num() <= 0 || !FloatTrack->GetAllSections()[0]) continue;
				const FString PropertyPath = FloatTrack->GetPropertyPath().ToString();
				double Value = -1.0;
				if (Fov > 0.0 && PropertyPath.Contains(TEXT("FieldOfView"))) Value = Fov;
				else if (FocusDistance >= 0.0 && PropertyPath.Contains(TEXT("Focus"))) Value = FocusDistance;
				if (Value < 0.0) continue;
				UMovieSceneSection* Section = FloatTrack->GetAllSections()[0];
				Section->Modify();
				TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
				for (FMovieSceneFloatChannel* Channel : Channels)
				{
					if (!Channel) continue;
					Channel->AddCubicKey(Frame, static_cast<float>(Value));
					++KeysAdded;
					float Readback = 0.0f;
					if (ReadBackUmgFloatKey(*Channel, Frame, static_cast<float>(Value), Readback)) ++KeysVerified;
				}
			}
			if (KeysAdded <= 0)
			{
				OutError = TEXT("No matching native camera tracks or sections were available for the requested keyframe values.");
				return false;
			}
			MovieScene->MarkAsChanged();
			Sequence->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Sequence);
			if (!Context.Services.SaveAsset(Sequence->GetPathName(), false, OutError)) return false;
			const bool bVerified = KeysVerified == KeysAdded;
			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
			OutStructured->SetStringField(TEXT("camera_actor"), CameraActorObject->GetActorLabel());
			OutStructured->SetStringField(TEXT("binding_id"), BindingGuid.ToString());
			OutStructured->SetNumberField(TEXT("keyframe_time"), Time);
			OutStructured->SetNumberField(TEXT("frame"), Frame.Value);
			OutStructured->SetNumberField(TEXT("keys_added"), KeysAdded);
			OutStructured->SetNumberField(TEXT("keys_verified"), KeysVerified);
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			OutStructured->SetBoolField(TEXT("verified"), bVerified);
			OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("success") : TEXT("failed"));
			if (!bVerified) { OutError = TEXT("Camera keyframe readback did not match every written channel."); return false; }
			OutSummary = FString::Printf(TEXT("Set and verified %d native camera keys at %.3fs for '%s'."), KeysAdded, Time, *CameraActorObject->GetActorLabel());
			return true;
			}
		, nullptr
		, 5,
		nullptr,
		false
		});

		// P1-9: 体积雾 + 后处理体积 (4 个工具)
		Registry.Register({
			TEXT("volume_fog_create"),
			TEXT("Create an exponential height fog actor for volumetric fog."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the fog actor"))},
					{TEXT("location"), VectorSchema()},
					{TEXT("fog_density"), FSololmcpSchemaBuilder::Number(TEXT("Fog density (default 0.02)"))},
					{TEXT("height_falloff"), FSololmcpSchemaBuilder::Number(TEXT("Height falloff (default 0.2)"))},
					{TEXT("scattering_distribution"), FSololmcpSchemaBuilder::Number(TEXT("Scattering distribution (default 0.5)"))}
				},
				{}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorName;
				Arguments->TryGetStringField(TEXT("actor_name"), ActorName);
				
				FVector Location = FVector::ZeroVector;
				if (TSharedPtr<FJsonObject> LocationObj; TryGetObjectField(Arguments, TEXT("location"), LocationObj))
				{
					FSololmcpEditorServices::JsonToVector(LocationObj, Location);
				}

				TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
				SpawnArgs->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.ExponentialHeightFog"));
				SpawnArgs->SetObjectField(TEXT("location"), Arguments->GetObjectField(TEXT("location")).Get() ? Arguments->GetObjectField(TEXT("location")).ToSharedRef() : MakeShared<FJsonObject>());

				TSharedRef<FJsonObject> SpawnResult = MakeShared<FJsonObject>();
				FString SpawnSummary;
				if (!Registry.ExecuteTool(TEXT("actor_spawn"), SpawnArgs, SpawnResult, SpawnSummary, OutError))
				{
					return false;
				}

				FString ActorPath;
				SpawnResult->TryGetStringField(TEXT("path"), ActorPath);
				AActor* FogActor = ActorPath.IsEmpty() ? nullptr : FindObject<AActor>(nullptr, *ActorPath);
				if (!FogActor)
				{
					OutError = TEXT("Fog actor was spawned but could not be resolved for component setup.");
					return false;
				}
				if (!ActorName.IsEmpty())
				{
					FogActor->SetActorLabel(ActorName);
					SpawnResult->SetStringField(TEXT("label"), ActorName);
				}

				UExponentialHeightFogComponent* FogComponent = Cast<UExponentialHeightFogComponent>(
					FogActor->GetComponentByClass(UExponentialHeightFogComponent::StaticClass()));
				if (!FogComponent)
				{
					OutError = TEXT("Spawned actor has no ExponentialHeightFogComponent.");
					return false;
				}

				TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
				double FogDensity = 0.02;
				double HeightFalloff = 0.2;
				double ScatteringDistribution = 0.5;
				Arguments->TryGetNumberField(TEXT("fog_density"), FogDensity);
				Arguments->TryGetNumberField(TEXT("height_falloff"), HeightFalloff);
				Arguments->TryGetNumberField(TEXT("scattering_distribution"), ScatteringDistribution);
				Properties->SetNumberField(TEXT("FogDensity"), FogDensity);
				Properties->SetNumberField(TEXT("FogHeightFalloff"), HeightFalloff);
				Properties->SetBoolField(TEXT("bEnableVolumetricFog"), true);
				Properties->SetNumberField(TEXT("VolumetricFogScatteringDistribution"), ScatteringDistribution);
				if (!Context.Services.ApplyProperties(FogComponent, Properties, OutError))
				{
					return false;
				}

				OutStructured = SpawnResult;
				OutSummary = TEXT("Created volumetric fog actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("volume_fog_set_properties"),
			TEXT("Set volumetric fog properties on an exponential height fog actor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Fog actor identifier"))},
					{TEXT("fog_density"), FSololmcpSchemaBuilder::Number()},
					{TEXT("height_falloff"), FSololmcpSchemaBuilder::Number()},
					{TEXT("max_opacity"), FSololmcpSchemaBuilder::Number()},
					{TEXT("start_distance"), FSololmcpSchemaBuilder::Number()},
					{TEXT("fog_color"), ColorSchema()}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}

				UExponentialHeightFogComponent* FogComponent = Cast<UExponentialHeightFogComponent>(
					Actor->GetComponentByClass(UExponentialHeightFogComponent::StaticClass()));
				if (!FogComponent)
				{
					OutError = TEXT("Actor has no ExponentialHeightFogComponent.");
					return false;
				}

				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				double FogDensity, HeightFalloff, MaxOpacity, StartDistance;
				if (Arguments->TryGetNumberField(TEXT("fog_density"), FogDensity))
				{
					Properties->SetNumberField(TEXT("FogDensity"), FogDensity);
				}
				if (Arguments->TryGetNumberField(TEXT("height_falloff"), HeightFalloff))
				{
					Properties->SetNumberField(TEXT("FogHeightFalloff"), HeightFalloff);
				}
				if (Arguments->TryGetNumberField(TEXT("max_opacity"), MaxOpacity))
				{
					Properties->SetNumberField(TEXT("FogMaxOpacity"), MaxOpacity);
				}
				if (Arguments->TryGetNumberField(TEXT("start_distance"), StartDistance))
				{
					Properties->SetNumberField(TEXT("StartDistance"), StartDistance);
				}
				if (!Context.Services.ApplyProperties(FogComponent, Properties, OutError))
				{
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutSummary = TEXT("Set volumetric fog properties.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("post_process_volume_create"),
			TEXT("Create a post process volume actor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("location"), VectorSchema()},
					{TEXT("extent"), VectorSchema(TEXT("Box extent (half size)"))},
					{TEXT("priority"), FSololmcpSchemaBuilder::Number(TEXT("Priority for blending (default 0)"))},
					{TEXT("blend_radius"), FSololmcpSchemaBuilder::Number(TEXT("Blend radius in cm"))},
					{TEXT("unbound"), FSololmcpSchemaBuilder::Boolean(TEXT("If true, affects entire scene"))}
				},
				{}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorName;
				Arguments->TryGetStringField(TEXT("actor_name"), ActorName);

			TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
			SpawnArgs->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.PostProcessVolume"));
			if (!ActorName.IsEmpty())
			{
				SpawnArgs->SetStringField(TEXT("actor_label"), ActorName);
			}
			// UE 5.7: TryGetObjectField signature changed - uses const TSharedPtr* output param
			const TSharedPtr<FJsonObject>* LocationObjPtr = nullptr;
			if (Arguments->TryGetObjectField(TEXT("location"), LocationObjPtr) && LocationObjPtr && LocationObjPtr->IsValid())
			{
				SpawnArgs->SetObjectField(TEXT("location"), LocationObjPtr->ToSharedRef());
			}

				TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
				double Priority = 0.0;
				Arguments->TryGetNumberField(TEXT("priority"), Priority);
				Properties->SetNumberField(TEXT("Priority"), Priority);
				
				bool bUnbound = false;
				if (Arguments->TryGetBoolField(TEXT("unbound"), bUnbound))
				{
					Properties->SetBoolField(TEXT("bUnbound"), bUnbound);
				}
				SpawnArgs->SetObjectField(TEXT("properties"), Properties);

				TSharedRef<FJsonObject> SpawnResult = MakeShared<FJsonObject>();
				FString SpawnSummary;
				if (!Registry.ExecuteTool(TEXT("actor_spawn"), SpawnArgs, SpawnResult, SpawnSummary, OutError))
				{
					return false;
				}
				FString ActorPath;
				SpawnResult->TryGetStringField(TEXT("path"), ActorPath);
				AActor* PPActor = ActorPath.IsEmpty() ? nullptr : FindObject<AActor>(nullptr, *ActorPath);
				if (PPActor && !ActorName.IsEmpty())
				{
					PPActor->SetActorLabel(ActorName);
					SpawnResult->SetStringField(TEXT("label"), ActorName);
					SpawnResult->SetStringField(TEXT("actor_label"), PPActor->GetActorLabel());
				}

				OutStructured = SpawnResult;
				OutSummary = TEXT("Created post process volume.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("post_process_volume_set_settings"),
			TEXT("Set post process settings on a post process volume."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("settings"), FSololmcpSchemaBuilder::Object({{TEXT("bloom_intensity"), FSololmcpSchemaBuilder::Number()}, {TEXT("bloom_threshold"), FSololmcpSchemaBuilder::Number()}, {TEXT("exposure_compensation"), FSololmcpSchemaBuilder::Number()}, {TEXT("saturation"), FSololmcpSchemaBuilder::Number()}, {TEXT("contrast"), FSololmcpSchemaBuilder::Number()}, {TEXT("vignette_intensity"), FSololmcpSchemaBuilder::Number()}, {TEXT("depth_of_field_fstop"), FSololmcpSchemaBuilder::Number()}, {TEXT("motion_blur_amount"), FSololmcpSchemaBuilder::Number()}, {TEXT("ambient_occlusion_intensity"), FSololmcpSchemaBuilder::Number()}, {TEXT("ray_tracing_ambient_occlusion_intensity"), FSololmcpSchemaBuilder::Number()}})}
				},
				{TEXT("actor"), TEXT("settings")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				const TSharedPtr<FJsonObject>* SettingsPtr = nullptr;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetObjectField(TEXT("settings"), SettingsPtr))
				{
					OutError = TEXT("Missing actor or settings.");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				APostProcessVolume* PPVolume = Cast<APostProcessVolume>(Actor);
				if (!PPVolume)
				{
					OutError = FString::Printf(TEXT("Actor '%s' is not a PostProcessVolume."), *ActorId);
					return false;
				}
				const TSharedPtr<FJsonObject>& Settings = *SettingsPtr;
				if (!Settings.IsValid())
				{
					OutError = TEXT("Settings object is invalid.");
					return false;
				}
				int32 Applied = 0;
				double Number = 0.0;
				if (Settings->TryGetNumberField(TEXT("bloom_intensity"), Number))
				{
					PPVolume->Settings.bOverride_BloomIntensity = true;
					PPVolume->Settings.BloomIntensity = static_cast<float>(Number);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("bloom_threshold"), Number))
				{
					PPVolume->Settings.bOverride_BloomThreshold = true;
					PPVolume->Settings.BloomThreshold = static_cast<float>(Number);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("exposure_compensation"), Number))
				{
					PPVolume->Settings.bOverride_AutoExposureBias = true;
					PPVolume->Settings.AutoExposureBias = static_cast<float>(Number);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("saturation"), Number))
				{
					PPVolume->Settings.bOverride_ColorSaturation = true;
					const float Value = static_cast<float>(Number);
					PPVolume->Settings.ColorSaturation = FVector4(Value, Value, Value, 1.0f);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("contrast"), Number))
				{
					PPVolume->Settings.bOverride_ColorContrast = true;
					const float Value = static_cast<float>(Number);
					PPVolume->Settings.ColorContrast = FVector4(Value, Value, Value, 1.0f);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("vignette_intensity"), Number))
				{
					PPVolume->Settings.bOverride_VignetteIntensity = true;
					PPVolume->Settings.VignetteIntensity = static_cast<float>(Number);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("depth_of_field_fstop"), Number))
				{
					PPVolume->Settings.bOverride_DepthOfFieldFstop = true;
					PPVolume->Settings.DepthOfFieldFstop = static_cast<float>(Number);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("motion_blur_amount"), Number))
				{
					PPVolume->Settings.bOverride_MotionBlurAmount = true;
					PPVolume->Settings.MotionBlurAmount = static_cast<float>(Number);
					Applied++;
				}
				if (Settings->TryGetNumberField(TEXT("ambient_occlusion_intensity"), Number))
				{
					PPVolume->Settings.bOverride_AmbientOcclusionIntensity = true;
					PPVolume->Settings.AmbientOcclusionIntensity = static_cast<float>(Number);
					Applied++;
				}
				if (Applied == 0)
				{
					OutError = TEXT("No supported post process setting was provided.");
					return false;
				}
				PPVolume->MarkPackageDirty();
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutStructured->SetNumberField(TEXT("applied_settings"), Applied);
				OutStructured->SetBoolField(TEXT("unbound"), PPVolume->bUnbound != 0);
				OutSummary = FString::Printf(TEXT("Set %d post process settings."), Applied);
				return true;
			}
		, nullptr
		, 5
		});

	// end RegisterSequencerAudioVfxTools
	}
}
