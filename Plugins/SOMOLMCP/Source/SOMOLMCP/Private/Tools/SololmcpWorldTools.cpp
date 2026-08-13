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
	void RegisterWorldTools(FSololmcpToolRegistry& Registry)
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

		{
			FSololmcpToolDefinition Def;
			Def.Name = TEXT("world_get_state");
			Def.Description = TEXT("Return the current editor world, level and selection state.");
			Def.InputSchema = FSololmcpSchemaBuilder::Object({});
			Def.CacheTtlSeconds = 5;  // TTL cache: high-frequency read-only query
			Def.IsAvailable = {};
			Def.Execute = [&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				OutStructured = WorldStateToJson(Context.Services);
				OutSummary = TEXT("Collected current world state.");
				return true;
			};
			Registry.Register(Def);
		};

		Registry.Register({
			TEXT("world_new_level"),
			TEXT("Create a new level package and open it."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Destination level asset path, for example /Game/Maps/NewMap."))},
					{TEXT("partitioned"), FSololmcpSchemaBuilder::Boolean(TEXT("Create as a partitioned world."))}
				},
				{TEXT("asset_path")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				FString Error;
				ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(Error);
				if (!LevelSubsystem)
				{
					OutError = Error;
					return false;
				}

				const bool bPartitioned = Arguments->HasTypedField<EJson::Boolean>(TEXT("partitioned")) ? Arguments->GetBoolField(TEXT("partitioned")) : false;
				// ULevelEditorSubsystem::NewLevel resets the editor transaction buffer while it
				// tears down the current world. Wrapping that call in FScopedTransaction leaves
				// the buffer active during Reset and can re-enter WorldPartition teardown with
				// an initialized world, which is an engine assertion. NewLevel owns this editor
				// lifecycle; only actor/property edits belong in an outer transaction.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				if (!LevelSubsystem->NewLevel(AssetPath, bPartitioned))
#else
				// 5.3 has no partitioned-world parameter. Refusing beats silently creating
				// a non-partitioned level when the caller asked for a partitioned one.
				if (bPartitioned)
				{
					OutError = TEXT("NOT_AVAILABLE_ON_ENGINE: partitioned levels need UE 5.4 or newer.");
					return false;
				}
				if (!LevelSubsystem->NewLevel(AssetPath))
#endif
				{
					OutError = TEXT("Failed to create level.");
					return false;
				}
				if (!VerifyCurrentLevelPackage(LevelSubsystem, AssetPath, OutError))
				{
					return false;
				}

				OutStructured = WorldStateToJson(Context.Services);
				OutSummary = TEXT("Created new level.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("world_load_level"),
			TEXT("Load an existing level asset into the editor."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}
				FString Error;
				ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(Error);
				if (!LevelSubsystem)
				{
					OutError = Error;
					return false;
				}
				if (!LevelSubsystem->LoadLevel(AssetPath))
				{
					OutError = TEXT("Failed to load level.");
					return false;
				}
				if (!VerifyCurrentLevelPackage(LevelSubsystem, AssetPath, OutError))
				{
					return false;
				}
				OutStructured = WorldStateToJson(Context.Services);
				OutSummary = TEXT("Loaded level.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("world_save_current_level"),
			TEXT("Save the current level."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Error;
				ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(Error);
				if (!LevelSubsystem)
				{
					OutError = Error;
					return false;
				}
				ULevel* CurrentLevel = LevelSubsystem->GetCurrentLevel();
				if (!CurrentLevel || !CurrentLevel->GetOutermost())
				{
					OutError = TEXT("No current level is loaded.");
					return false;
				}
				if (!LevelSubsystem->SaveCurrentLevel())
				{
					OutError = TEXT("Failed to save current level.");
					return false;
				}
				if (CurrentLevel->GetOutermost()->IsDirty())
				{
					OutError = FString::Printf(TEXT("SaveCurrentLevel reported success but level package '%s' is still dirty."), *CurrentLevel->GetOutermost()->GetName());
					return false;
				}
				OutStructured->SetBoolField(TEXT("saved"), true);
				OutSummary = TEXT("Saved current level.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("world_save_all_dirty_levels"),
			TEXT("Save all dirty levels."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Error;
				ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(Error);
				if (!LevelSubsystem)
				{
					OutError = Error;
					return false;
				}
				UWorld* World = Context.Services.GetEditorWorld(Error);
				if (!World)
				{
					OutError = Error;
					return false;
				}
				if (!LevelSubsystem->SaveAllDirtyLevels())
				{
					OutError = TEXT("Failed to save all dirty levels.");
					return false;
				}
				TArray<FString> StillDirtyLevels;
				for (ULevel* Level : World->GetLevels())
				{
					if (Level && Level->GetOutermost() && Level->GetOutermost()->IsDirty())
					{
						StillDirtyLevels.Add(Level->GetOutermost()->GetName());
					}
				}
				if (StillDirtyLevels.Num() > 0)
				{
					OutError = FString::Printf(TEXT("SaveAllDirtyLevels reported success but %d level package(s) are still dirty: %s"), StillDirtyLevels.Num(), *FString::Join(StillDirtyLevels, TEXT(", ")));
					return false;
				}
				OutStructured->SetBoolField(TEXT("saved"), true);
				OutSummary = TEXT("Saved all dirty levels.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_spawn"),
			TEXT("Spawn an actor from a class path or asset path."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("class_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Optional editor actor label to assign after spawn."))},
					{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor_label."))},
					{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor_label."))},
					{TEXT("location"), VectorSchema()},
					{TEXT("rotation"), RotatorSchema()},
					{TEXT("scale"), VectorSchema()},
					{TEXT("snap_to_ground"), FSololmcpSchemaBuilder::Boolean(TEXT("If true, snap the actor bottom bounds to WorldStatic ground after scale is applied."))},
					{TEXT("transient"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("properties"), FSololmcpSchemaBuilder::Object({})}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Error;
				UEditorActorSubsystem* ActorSubsystem = Context.Services.GetActorSubsystem(Error);
				if (!ActorSubsystem)
				{
					OutError = Error;
					return false;
				}

				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				FVector RequestedScale = FVector::OneVector;
				bool bScaleProvided = false;
				if (TSharedPtr<FJsonObject> LocationObject; TryGetObjectField(Arguments, TEXT("location"), LocationObject))
				{
					FSololmcpEditorServices::JsonToVector(LocationObject, Location);
				}
				if (TSharedPtr<FJsonObject> RotationObject; TryGetObjectField(Arguments, TEXT("rotation"), RotationObject))
				{
					FSololmcpEditorServices::JsonToRotator(RotationObject, Rotation);
				}
				if (TSharedPtr<FJsonObject> ScaleObject; TryGetObjectField(Arguments, TEXT("scale"), ScaleObject))
				{
					FSololmcpEditorServices::JsonToVector(ScaleObject, RequestedScale);
					bScaleProvided = true;
				}
				const bool bSnapToGround = Arguments->HasTypedField<EJson::Boolean>(TEXT("snap_to_ground")) ? Arguments->GetBoolField(TEXT("snap_to_ground")) : false;

				const bool bTransient = Arguments->HasTypedField<EJson::Boolean>(TEXT("transient")) ? Arguments->GetBoolField(TEXT("transient")) : false;
				AActor* Actor = nullptr;

				FString ClassPath;
				FString AssetPath;
				FString RequestedActorLabel;
				Arguments->TryGetStringField(TEXT("class_path"), ClassPath);
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
				if (!Arguments->TryGetStringField(TEXT("actor_label"), RequestedActorLabel)
					&& !Arguments->TryGetStringField(TEXT("label"), RequestedActorLabel)
					&& !Arguments->TryGetStringField(TEXT("name"), RequestedActorLabel))
				{
					RequestedActorLabel.Reset();
				}
				bool bAssetApplied = false;
				FString AppliedComponentName;

				if (!ClassPath.IsEmpty())
				{
					UClass* ActorClass = Context.Services.ResolveClass(ClassPath, OutError);
					if (!ActorClass)
					{
						return false;
					}
					if (ActorClass->IsChildOf(ADirectionalLight::StaticClass()))
					{
						UWorld* World = Context.Services.GetEditorWorld(OutError);
						if (!World)
						{
							return false;
						}
						if (ADirectionalLight* ExistingDirectional = FindFirstDirectionalLight(World))
						{
							OutStructured = FSololmcpEditorServices::MakeActorReference(ExistingDirectional);
							OutStructured->SetStringField(TEXT("status"), TEXT("single_directional_light_guard"));
							OutStructured->SetStringField(TEXT("existing_directional_light"), ExistingDirectional->GetActorLabel());
							OutStructured->SetBoolField(TEXT("spawn_blocked"), true);
							OutError = FString::Printf(TEXT("DirectionalLight already exists (%s). Reuse it or call scene_lighting_ensure; do not spawn a second DirectionalLight."), *ExistingDirectional->GetActorLabel());
							return false;
						}
					}
					UObject* ExplicitAsset = nullptr;
					if (!AssetPath.IsEmpty())
					{
						FString LoadErr;
						ExplicitAsset = Context.Services.LoadAsset(AssetPath, LoadErr);
						if (!ExplicitAsset)
						{
							SololmcpError::InvalidPath(OutStructured, AssetPath);
							OutError = FString::Printf(TEXT("asset_path was provided but could not be loaded: %s"), *AssetPath);
							return false;
						}
						if (!ExplicitAsset->IsA<UStaticMesh>() && !ExplicitAsset->IsA<USkeletalMesh>())
						{
							SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("asset_path"),
								TEXT("When class_path is also provided, asset_path must be a UStaticMesh or USkeletalMesh that can be assigned to the spawned actor."));
							OutError = FString::Printf(TEXT("Unsupported asset type for actor_spawn mesh assignment: %s"), *ExplicitAsset->GetClass()->GetName());
							return false;
						}
					}
					const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorSpawnClass", "SOMOLMCP Spawn Actor"));
					Actor = ActorSubsystem->SpawnActorFromClass(ActorClass, Location, Rotation, bTransient);

					// FIX (v12): if both class_path AND asset_path were given, apply
					// the asset to the actor's primary mesh component. Without this,
					// callers would spawn an EMPTY StaticMeshActor (no mesh, only
					// gizmo) when they passed both. Old code silently ignored
					// asset_path whenever class_path was set.
					if (Actor && ExplicitAsset)
					{
						if (UStaticMesh* SM = Cast<UStaticMesh>(ExplicitAsset))
						{
							if (UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>())
							{
								SMC->SetStaticMesh(SM);
								SMC->MarkRenderStateDirty();
								bAssetApplied = (SMC->GetStaticMesh() == SM);
								AppliedComponentName = SMC->GetName();
							}
						}
						else if (USkeletalMesh* SkM = Cast<USkeletalMesh>(ExplicitAsset))
						{
							if (USkeletalMeshComponent* SkMC = Actor->FindComponentByClass<USkeletalMeshComponent>())
							{
								SkMC->SetSkeletalMesh(SkM);
								SkMC->MarkRenderStateDirty();
								bAssetApplied = (SkMC->GetSkeletalMeshAsset() == SkM);
								AppliedComponentName = SkMC->GetName();
							}
						}
						if (!bAssetApplied)
						{
							if (Actor)
							{
								ActorSubsystem->DestroyActor(Actor);
								Actor = nullptr;
							}
							SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
								TEXT("asset_path was provided but could not be assigned to a matching mesh component on the spawned actor."));
							OutError = FString::Printf(TEXT("Failed to apply asset '%s' to spawned actor of class '%s'."), *AssetPath, *ClassPath);
							return false;
						}
					}
				}
				else
				{
					if (AssetPath.IsEmpty())
					{
						SololmcpError::MissingParam(OutStructured, TEXT("class_path"));
						OutError = TEXT("Expected class_path or asset_path.");
						return false;
					}
					UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
					if (!Asset)
					{
						SololmcpError::InvalidPath(OutStructured, AssetPath);
						return false;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorSpawnObject", "SOMOLMCP Spawn Actor From Asset"));
					Actor = ActorSubsystem->SpawnActorFromObject(Asset, Location, Rotation, bTransient);
					if (Actor)
					{
						if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
						{
							if (UNiagaraComponent* NiagaraComponent =
								Actor->FindComponentByClass<UNiagaraComponent>())
							{
								if (NiagaraComponent->GetAsset() != NiagaraSystem)
								{
									NiagaraComponent->SetAsset(NiagaraSystem);
								}
								NiagaraComponent->Activate(true);
								bAssetApplied = NiagaraComponent->GetAsset() == NiagaraSystem;
								AppliedComponentName = NiagaraComponent->GetName();
							}
						}
						else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
						{
							if (UStaticMeshComponent* MeshComponent =
								Actor->FindComponentByClass<UStaticMeshComponent>())
							{
								bAssetApplied = MeshComponent->GetStaticMesh() == StaticMesh;
								AppliedComponentName = MeshComponent->GetName();
							}
						}
						else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
						{
							if (USkeletalMeshComponent* MeshComponent =
								Actor->FindComponentByClass<USkeletalMeshComponent>())
							{
								bAssetApplied =
									MeshComponent->GetSkeletalMeshAsset() == SkeletalMesh;
								AppliedComponentName = MeshComponent->GetName();
							}
						}
						else
						{
							// SpawnActorFromObject is the engine's authoritative
							// binding path for other placeable asset types.
							bAssetApplied = true;
							AppliedComponentName = TEXT("SpawnActorFromObject");
						}
					}
				}

				if (!Actor)
				{
					OutError = TEXT("Failed to spawn actor.");
					return false;
				}
				if (ClassPath.IsEmpty() && !AssetPath.IsEmpty() && !bAssetApplied)
				{
					ActorSubsystem->DestroyActor(Actor);
					SololmcpError::Set(
						OutStructured,
						TEXT("OPERATION_FAILED"),
						TEXT("asset_path"),
						TEXT("Actor spawned from the asset, but the runtime component did not retain that asset."));
					OutError = FString::Printf(
						TEXT("Failed to verify asset binding on actor spawned from '%s'."),
						*AssetPath);
					return false;
				}

				// Audit round 7 (silent-spawn-location fix): SpawnActorFromClass on the editor
				// subsystem can silently leave the actor at world origin instead of the requested
				// (Location, Rotation). Re-apply explicitly and verify before applying optional
				// properties, so callers cannot receive ok=true with a transform that differs
				// from what they asked for.
				Actor->SetActorLocationAndRotation(Location, Rotation, /*bSweep=*/false, /*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);
				if (bScaleProvided)
				{
					Actor->SetActorScale3D(RequestedScale);
					Actor->UpdateComponentTransforms();
				}
				if (bSnapToGround)
				{
					UWorld* World = Context.Services.GetEditorWorld(OutError);
					if (!World)
					{
						return false;
					}
					const FBox Bounds = Actor->GetComponentsBoundingBox(true);
					const double HalfHeight = Bounds.IsValid ? Bounds.GetExtent().Z : 0.0;
					FHitResult Hit;
					FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SOMOLMCPActorSpawnSnapToGround), true);
					QueryParams.AddIgnoredActor(Actor);
					const FVector TraceStart = Actor->GetActorLocation() + FVector(0, 0, 200000.0);
					const FVector TraceEnd = Actor->GetActorLocation() - FVector(0, 0, 200000.0);
					if (!World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_WorldStatic, QueryParams))
					{
						ActorSubsystem->DestroyActor(Actor);
						OutError = TEXT("snap_to_ground requested but no WorldStatic ground was found below the spawned actor.");
						return false;
					}
					Location = Hit.Location + FVector(0, 0, HalfHeight);
					Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
					Actor->UpdateComponentTransforms();
				}
				{
					const FVector ActualLoc = Actor->GetActorLocation();
					const FRotator ActualRot = Actor->GetActorRotation();
					const FVector ActualScale = Actor->GetActorScale3D();
					if (!ActualLoc.Equals(Location, 0.5f) || !ActualRot.Equals(Rotation, 0.1f))
					{
						OutError = FString::Printf(TEXT("Actor spawned but transform did not apply: requested location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f), actual location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f)."),
							Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll,
							ActualLoc.X, ActualLoc.Y, ActualLoc.Z, ActualRot.Pitch, ActualRot.Yaw, ActualRot.Roll);
						return false;
					}
					if (bScaleProvided && !ActualScale.Equals(RequestedScale, 0.001f))
					{
						OutError = FString::Printf(TEXT("Actor spawned but scale did not apply: requested scale=(%.4f,%.4f,%.4f), actual scale=(%.4f,%.4f,%.4f)."),
							RequestedScale.X, RequestedScale.Y, RequestedScale.Z,
							ActualScale.X, ActualScale.Y, ActualScale.Z);
						return false;
					}
				}

				TSharedPtr<FJsonObject> Properties;
				if (TryGetObjectField(Arguments, TEXT("properties"), Properties) && !Context.Services.ApplyProperties(Actor, Properties, OutError))
				{
					ActorSubsystem->DestroyActor(Actor);
					return false;
				}
				if (!RequestedActorLabel.TrimStartAndEnd().IsEmpty())
				{
					Actor->SetActorLabel(RequestedActorLabel.TrimStartAndEnd());
				}

				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				if (!RequestedActorLabel.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("requested_actor_label"), RequestedActorLabel);
					OutStructured->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
				}
				OutStructured->SetBoolField(TEXT("asset_applied"), bAssetApplied);
				if (!AppliedComponentName.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("asset_component"), AppliedComponentName);
				}
				{
					const FVector ActualLoc = Actor->GetActorLocation();
					const FRotator ActualRot = Actor->GetActorRotation();
					const FVector ActualScale = Actor->GetActorScale3D();
					TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
					LocObj->SetNumberField(TEXT("x"), ActualLoc.X);
					LocObj->SetNumberField(TEXT("y"), ActualLoc.Y);
					LocObj->SetNumberField(TEXT("z"), ActualLoc.Z);
					OutStructured->SetObjectField(TEXT("location"), LocObj);
					TSharedRef<FJsonObject> RotObj = MakeShared<FJsonObject>();
					RotObj->SetNumberField(TEXT("pitch"), ActualRot.Pitch);
					RotObj->SetNumberField(TEXT("yaw"), ActualRot.Yaw);
					RotObj->SetNumberField(TEXT("roll"), ActualRot.Roll);
					OutStructured->SetObjectField(TEXT("rotation"), RotObj);
					TSharedRef<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
					ScaleObj->SetNumberField(TEXT("x"), ActualScale.X);
					ScaleObj->SetNumberField(TEXT("y"), ActualScale.Y);
					ScaleObj->SetNumberField(TEXT("z"), ActualScale.Z);
					OutStructured->SetObjectField(TEXT("scale"), ScaleObj);
					OutStructured->SetBoolField(TEXT("snap_to_ground"), bSnapToGround);
				}
				OutSummary = TEXT("Spawned actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_destroy"),
			TEXT("Destroy an actor by label, name or path."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label, name, object path, or editor path."))},
					{TEXT("actor_id"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
					{TEXT("actor_path"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
					{TEXT("path"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
					{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))},
					{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Alias of actor."))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId)
					&& !Arguments->TryGetStringField(TEXT("actor_id"), ActorId)
					&& !Arguments->TryGetStringField(TEXT("actor_path"), ActorId)
					&& !Arguments->TryGetStringField(TEXT("path"), ActorId)
					&& !Arguments->TryGetStringField(TEXT("label"), ActorId)
					&& !Arguments->TryGetStringField(TEXT("name"), ActorId))
				{
					OutError = TEXT("Missing actor identifier. Provide actor, actor_id, actor_path, path, label, or name.");
					return false;
				}

				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}

				FString Error;
				UEditorActorSubsystem* ActorSubsystem = Context.Services.GetActorSubsystem(Error);
				if (!ActorSubsystem)
				{
					OutError = Error;
					return false;
				}

				const FString DestroyedLabel = Actor->GetActorLabel();
				const FString DestroyedName = Actor->GetName();
				const FString DestroyedPath = Actor->GetPathName();
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorDestroy", "SOMOLMCP Destroy Actor"));
				if (!ActorSubsystem->DestroyActor(Actor))
				{
					OutError = TEXT("Failed to destroy actor.");
					return false;
				}
				bool bStillPresent = false;
				for (AActor* Candidate : ActorSubsystem->GetAllLevelActors())
				{
					if (!Candidate)
					{
						continue;
					}
					// BUGFIX: match ONLY by unique identity (pointer / object name /
					// full path). The previous label match (GetActorLabel) was the bug:
					// DUPLICATE actors share a display label (e.g. all lakes are
					// "Water Body Lake"), so destroying ONE duplicate found a SIBLING
					// with the same label and falsely reported "still present" — making
					// actor_destroy unable to ever delete any duplicate (the exact
					// failure seen deduping 4 lakes). Object name (WaterBodyLake_0/_1…)
					// and path are unique per actor, so siblings no longer false-match.
					if (Candidate == Actor
						|| Candidate->GetName() == DestroyedName
						|| Candidate->GetPathName() == DestroyedPath)
					{
						bStillPresent = true;
						break;
					}
				}
				if (bStillPresent)
				{
					OutError = TEXT("DestroyActor reported success but the actor is still present in the level.");
					return false;
				}

				OutStructured->SetBoolField(TEXT("destroyed"), true);
				OutStructured->SetStringField(TEXT("requested_actor"), ActorId);
				OutStructured->SetStringField(TEXT("actor_label"), DestroyedLabel);
				OutStructured->SetStringField(TEXT("actor_name"), DestroyedName);
				OutStructured->SetStringField(TEXT("actor_path"), DestroyedPath);
				OutStructured->SetBoolField(TEXT("post_delete_readback_present"), false);
				OutSummary = TEXT("Destroyed actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_duplicate"),
			TEXT("Duplicate an actor with an optional offset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("offset"), VectorSchema()}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing argument: actor");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}

				FVector Offset = FVector::ZeroVector;
				if (TSharedPtr<FJsonObject> OffsetObject; TryGetObjectField(Arguments, TEXT("offset"), OffsetObject))
				{
					FSololmcpEditorServices::JsonToVector(OffsetObject, Offset);
				}

				FString Error;
				UEditorActorSubsystem* ActorSubsystem = Context.Services.GetActorSubsystem(Error);
				if (!ActorSubsystem)
				{
					OutError = Error;
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorDuplicate", "SOMOLMCP Duplicate Actor"));
				UWorld* World = Actor->GetWorld();
				AActor* DuplicatedActor = ActorSubsystem->DuplicateActor(Actor, World, Offset);
				if (!DuplicatedActor)
				{
					OutError = TEXT("Failed to duplicate actor.");
					return false;
				}
				if (DuplicatedActor == Actor || !ActorSubsystem->GetAllLevelActors().Contains(DuplicatedActor))
				{
					OutError = TEXT("DuplicateActor reported success but did not produce a distinct actor in the level.");
					return false;
				}
				const FVector ExpectedLocation = Actor->GetActorLocation() + Offset;
				if (!DuplicatedActor->GetActorLocation().Equals(ExpectedLocation, 0.5f))
				{
					OutError = FString::Printf(TEXT("DuplicateActor reported success but offset did not apply: requested location=(%.2f,%.2f,%.2f), actual=(%.2f,%.2f,%.2f)."),
						ExpectedLocation.X, ExpectedLocation.Y, ExpectedLocation.Z,
						DuplicatedActor->GetActorLocation().X, DuplicatedActor->GetActorLocation().Y, DuplicatedActor->GetActorLocation().Z);
					return false;
				}

				OutStructured = FSololmcpEditorServices::MakeActorReference(DuplicatedActor);
				OutSummary = TEXT("Duplicated actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_set_transform"),
			TEXT("Apply a world transform to an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("actor_id"), FSololmcpSchemaBuilder::String(TEXT("Alias for 'actor'."))}, {TEXT("transform"), TransformSchema()}}, {TEXT("actor"), TEXT("transform")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					if (!Arguments->TryGetStringField(TEXT("actor_id"), ActorId))
					{
						OutError = TEXT("Missing argument: actor (or actor_id)");
						return false;
					}
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}

				FTransform Transform = FTransform::Identity;
				if (!ResolveTransformArg(Arguments, Transform))
				{
					OutError = TEXT("Missing or invalid transform object.");
					return false;
				}

				FString Error;
				UEditorActorSubsystem* ActorSubsystem = Context.Services.GetActorSubsystem(Error);
				if (!ActorSubsystem)
				{
					OutError = Error;
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorSetTransform", "SOMOLMCP Set Actor Transform"));
				// Audit round 7 (silent-set-transform fix): SetActorTransform on the editor
				// subsystem can return true even when the world transform is not actually
				// updated (e.g. mobility=Static or interactive locked). Call directly on the
				// actor and verify against GetActorLocation/Rotation/Scale, returning a real
				// error rather than the previous silent ok=true.
				Actor->SetActorTransform(Transform, /*bSweep=*/false, /*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);
				if (!ActorSubsystem->SetActorTransform(Actor, Transform))
				{
					OutError = TEXT("Failed to set actor transform.");
					return false;
				}
				{
					const FVector ExpectedLoc = Transform.GetLocation();
					const FRotator ExpectedRot = Transform.Rotator();
					const FVector ExpectedScale = Transform.GetScale3D();
					const FVector ActualLoc = Actor->GetActorLocation();
					const FRotator ActualRot = Actor->GetActorRotation();
					const FVector ActualScale = Actor->GetActorScale3D();
					if (!ActualLoc.Equals(ExpectedLoc, 0.5f) || !ActualRot.Equals(ExpectedRot, 0.1f) || !ActualScale.Equals(ExpectedScale, 0.001f))
					{
						OutError = FString::Printf(TEXT("Transform did not apply: requested location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f) scale=(%.3f,%.3f,%.3f), actual location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f) scale=(%.3f,%.3f,%.3f). Actor may be locked or have static mobility."),
							ExpectedLoc.X, ExpectedLoc.Y, ExpectedLoc.Z, ExpectedRot.Pitch, ExpectedRot.Yaw, ExpectedRot.Roll, ExpectedScale.X, ExpectedScale.Y, ExpectedScale.Z,
							ActualLoc.X, ActualLoc.Y, ActualLoc.Z, ActualRot.Pitch, ActualRot.Yaw, ActualRot.Roll, ActualScale.X, ActualScale.Y, ActualScale.Z);
						return false;
					}
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutSummary = TEXT("Updated actor transform.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_set_properties"),
			TEXT("Apply reflected property overrides to an actor. For scale, use actor_set_transform instead. Supports component paths: 'ComponentName.PropertyName' (e.g. 'HeterogeneousVolumeComponent.OverrideMaterials' with an array of material asset paths)."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("actor_id"), FSololmcpSchemaBuilder::String(TEXT("Alias for 'actor'."))}, {TEXT("properties"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("actor"), TEXT("properties")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					if (!Arguments->TryGetStringField(TEXT("actor_id"), ActorId))
					{
						OutError = TEXT("Missing argument: actor (or actor_id)");
						return false;
					}
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				TSharedPtr<FJsonObject> Properties;
				if (!TryGetObjectField(Arguments, TEXT("properties"), Properties))
				{
					OutError = TEXT("Missing argument: properties");
					return false;
				}
				// Intercept scale3d — redirect to actor_set_transform
				if (Properties->HasField(TEXT("scale3d")) || Properties->HasField(TEXT("Scale3D")))
				{
					OutError = TEXT("Property 'scale3d' is not a UPROPERTY on Actor. Use actor_set_transform with transform.scale instead. Example: actor_set_transform(actor, transform={scale:{x:1,y:1,z:1}})");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorSetProperties", "SOMOLMCP Set Actor Properties"));
				TArray<TSharedPtr<FJsonValue>> PropertyReceipts;
				if (!Context.Services.ApplyPropertiesWithReceipts(Actor, Properties, PropertyReceipts, OutError))
				{
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutStructured->SetArrayField(TEXT("property_receipts"), PropertyReceipts);
				OutSummary = TEXT("Updated actor properties.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_attach"),
			TEXT("Attach one actor to another while preserving world transform."),
			FSololmcpSchemaBuilder::Object({{TEXT("child"), FSololmcpSchemaBuilder::String()}, {TEXT("parent"), FSololmcpSchemaBuilder::String()}}, {TEXT("child"), TEXT("parent")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ChildId;
				FString ParentId;
				if (!Arguments->TryGetStringField(TEXT("child"), ChildId) || !Arguments->TryGetStringField(TEXT("parent"), ParentId))
				{
					OutError = TEXT("Missing argument: child or parent");
					return false;
				}

				AActor* Child = Context.Services.FindActorByLabelOrName(ChildId, OutError);
				if (!Child)
				{
					return false;
				}
				AActor* Parent = Context.Services.FindActorByLabelOrName(ParentId, OutError);
				if (!Parent)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ActorAttach", "SOMOLMCP Attach Actor"));
				const FTransform BeforeAttach = Child->GetActorTransform();
				Child->Modify();
				if (!Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform))
				{
					OutError = TEXT("Failed to attach actor.");
					return false;
				}
				if (Child->GetAttachParentActor() != Parent)
				{
					OutError = TEXT("AttachToActor reported success but parent relationship was not applied.");
					return false;
				}
				if (!Child->GetActorLocation().Equals(BeforeAttach.GetLocation(), 0.5f) ||
					!Child->GetActorRotation().Equals(BeforeAttach.Rotator(), 0.1f) ||
					!Child->GetActorScale3D().Equals(BeforeAttach.GetScale3D(), 0.001f))
				{
					OutError = TEXT("AttachToActor reported success but failed to preserve the child's world transform.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(Child);
				OutSummary = TEXT("Attached actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_select"),
			TEXT("Select a set of actors in the editor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("replace"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("actors")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> ActorIds;
				if (!TryGetStringArray(Arguments, TEXT("actors"), ActorIds))
				{
					OutError = TEXT("Missing argument: actors");
					return false;
				}

				FString Error;
				UEditorActorSubsystem* ActorSubsystem = Context.Services.GetActorSubsystem(Error);
				if (!ActorSubsystem)
				{
					OutError = Error;
					return false;
				}

				const bool bReplace = Arguments->HasTypedField<EJson::Boolean>(TEXT("replace")) ? Arguments->GetBoolField(TEXT("replace")) : true;
				if (ActorIds.Num() == 0 && !bReplace)
				{
					OutError = TEXT("actor_select with replace=false and an empty actor list would be a no-op.");
					return false;
				}
				TArray<AActor*> Actors = ResolveActors(Context.Services, ActorIds, OutError);
				if (ActorIds.Num() > 0 && Actors.Num() == 0)
				{
					return false;
				}
				if (bReplace)
				{
					ActorSubsystem->SelectNothing();
				}
				ActorSubsystem->SetSelectedLevelActors(Actors);
				USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
				if (!Selection)
				{
					OutError = TEXT("Actor selection reported success but editor selection is unavailable.");
					return false;
				}
				for (AActor* Actor : Actors)
				{
					if (!Selection->IsSelected(Actor))
					{
						OutError = FString::Printf(TEXT("Actor selection reported success but '%s' is not selected."), *Actor->GetActorLabel());
						return false;
					}
				}
				if (bReplace && Actors.Num() == 0 && Selection->Num() != 0)
				{
					OutError = TEXT("Actor selection clear reported success but selected actors remain.");
					return false;
				}
				OutStructured = MakeActorListResult(Actors);
				OutSummary = TEXT("Updated actor selection.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_list"),
			TEXT("List all actors in the current editor world. Use include_transform=true to get position/rotation/scale for spatial operations."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("filter"), FSololmcpSchemaBuilder::String(TEXT("Optional substring filter for label or name."))},
					{TEXT("class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional substring filter for actor class name (e.g. StaticMeshActor, PointLight)."))},
					{TEXT("include_transform"), FSololmcpSchemaBuilder::Boolean(TEXT("Include location/rotation/scale for each actor. Default false."))},
					{TEXT("include_bounds"), FSololmcpSchemaBuilder::Boolean(TEXT("Include world bounding box for each actor. Default false. Requires include_transform=true or standalone."))},
					{TEXT("max_count"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum number of actors to return. Default 5000 (was 100; bumped v12 because PCG/foliage scenes routinely have thousands). Use 0 for unlimited."))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Error;
				UWorld* World = Context.Services.GetEditorWorld(Error);
				if (!World)
				{
					OutError = Error;
					return false;
				}

				FString Filter, ClassFilter;
				Arguments->TryGetStringField(TEXT("filter"), Filter);
				Arguments->TryGetStringField(TEXT("class_filter"), ClassFilter);

				bool bIncludeTransform = false;
				bool bIncludeBounds = false;
				Arguments->TryGetBoolField(TEXT("include_transform"), bIncludeTransform);
				Arguments->TryGetBoolField(TEXT("include_bounds"), bIncludeBounds);

				// FIX (v12): default raised from 100 -> 5000 so dense scenes
				// (PCG scatters, foliage, hand-placed level dressings) report
				// truthful counts. Old default silently truncated; callers had
				// to know to pass explicit max_count to see the real picture.
				int32 MaxCount = 5000;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("max_count")))
				{
					MaxCount = FMath::Max(0, static_cast<int32>(Arguments->GetNumberField(TEXT("max_count"))));
				}

				TArray<TSharedPtr<FJsonValue>> ActorArray;
				int32 TotalMatched = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor)
					{
						continue;
					}
					if (!Filter.IsEmpty() && !Actor->GetActorLabel().Contains(Filter) && !Actor->GetName().Contains(Filter))
					{
						continue;
					}
					if (!ClassFilter.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassFilter))
					{
						continue;
					}
					TotalMatched++;
					if (MaxCount > 0 && ActorArray.Num() >= MaxCount)
					{
						continue;
					}

					TSharedRef<FJsonObject> ActorJson = FSololmcpEditorServices::MakeActorReference(Actor);

					if (bIncludeTransform)
					{
						ActorJson->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetActorTransform()));
					}

					if (bIncludeBounds)
					{
						FBox Bounds(ForceInit);
						for (UActorComponent* Comp : Actor->GetComponents())
						{
							if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
							{
								if (Prim->IsRegistered())
								{
									Bounds += Prim->Bounds.GetBox();
								}
							}
						}
						if (Bounds.IsValid)
						{
							ActorJson->SetObjectField(TEXT("bounds"), BoxToJson(Bounds));
						}
					}

					ActorArray.Add(MakeShared<FJsonValueObject>(ActorJson));
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetArrayField(TEXT("actors"), ActorArray);
				Result->SetNumberField(TEXT("count"), ActorArray.Num());
				Result->SetNumberField(TEXT("total_matched"), TotalMatched);
				if (MaxCount > 0 && TotalMatched > MaxCount)
				{
					Result->SetBoolField(TEXT("truncated"), true);
					Result->SetNumberField(TEXT("max_count"), MaxCount);
				}
				OutStructured = Result;
				OutSummary = FString::Printf(TEXT("Listed %d actors (matched %d)."), ActorArray.Num(), TotalMatched);
				return true;
			}
		, nullptr
		, 5
		});

		// P0-F1: actor_get_state — query detailed state of a single actor including transform and bounds
		Registry.Register({
			TEXT("actor_get_state"),
			TEXT("Get detailed state of an actor: transform (location/rotation/scale), bounding box, mobility, visibility. Call this BEFORE any spatial operation (move/align/connect/snap) to know the actor's current position."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label or name to query."))},
					{TEXT("actor_id"), FSololmcpSchemaBuilder::String(TEXT("Alias for 'actor'. Accepted for compatibility."))},
					{TEXT("include_bounds"), FSololmcpSchemaBuilder::Boolean(TEXT("Include world bounding box. Default true."))},
					{TEXT("include_components"), FSololmcpSchemaBuilder::Boolean(TEXT("Include list of primitive components with their bounds. Default false."))}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					// Fallback: accept 'actor_id' alias
					if (!Arguments->TryGetStringField(TEXT("actor_id"), ActorId))
					{
						OutError = TEXT("Missing argument: actor (or actor_id)");
						return false;
					}
				}
				// Audit round 3: reject empty actor identifier early to avoid downstream noise.
				if (ActorId.IsEmpty())
				{
					OutError = TEXT("Empty actor identifier (actor / actor_id is blank)");
					return false;
				}

				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}

				// Base reference (name, path, class, label)
				TSharedRef<FJsonObject> Result = FSololmcpEditorServices::MakeActorReference(Actor);

				// Transform — always included
				Result->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetActorTransform()));

				// Mobility
				if (USceneComponent* Root = Actor->GetRootComponent())
				{
					switch (Root->Mobility)
					{
					case EComponentMobility::Static:      Result->SetStringField(TEXT("mobility"), TEXT("Static"));      break;
					case EComponentMobility::Stationary:  Result->SetStringField(TEXT("mobility"), TEXT("Stationary"));  break;
					case EComponentMobility::Movable:     Result->SetStringField(TEXT("mobility"), TEXT("Movable"));     break;
					default:                              Result->SetStringField(TEXT("mobility"), TEXT("Unknown"));     break;
					}
				}

				// Visibility
				Result->SetBoolField(TEXT("is_visible"), !Actor->IsHidden());

				// Bounding box (optional, default true)
				bool bIncludeBounds = true;
				Arguments->TryGetBoolField(TEXT("include_bounds"), bIncludeBounds);
				if (bIncludeBounds)
				{
					FBox Bounds(ForceInit);
					for (UActorComponent* Comp : Actor->GetComponents())
					{
						if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
						{
							if (Prim->IsRegistered())
							{
								Bounds += Prim->Bounds.GetBox();
							}
						}
					}
					if (Bounds.IsValid)
					{
						Result->SetObjectField(TEXT("bounds"), BoxToJson(Bounds));
						// Convenience: center and extent
						TSharedRef<FJsonObject> CenterJson = MakeShared<FJsonObject>();
						const FVector Center = Bounds.GetCenter();
						const FVector Extent = Bounds.GetExtent();
						Result->SetObjectField(TEXT("bounds_center"), VectorToJson(Center));
						Result->SetObjectField(TEXT("bounds_extent"), VectorToJson(Extent));
					}
					else
					{
						Result->SetBoolField(TEXT("bounds_valid"), false);
					}
				}

				// Component list (optional, default false)
				bool bIncludeComponents = false;
				Arguments->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
				if (bIncludeComponents)
				{
					TArray<TSharedPtr<FJsonValue>> ComponentsJson;
					for (UActorComponent* Comp : Actor->GetComponents())
					{
						TSharedRef<FJsonObject> CompJson = MakeShared<FJsonObject>();
						CompJson->SetStringField(TEXT("name"), Comp->GetName());
						CompJson->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
						if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
						{
							CompJson->SetBoolField(TEXT("is_visible"), Prim->IsVisible());
							CompJson->SetObjectField(TEXT("bounds"), BoxToJson(Prim->Bounds.GetBox()));
						}
						ComponentsJson.Add(MakeShared<FJsonValueObject>(CompJson));
					}
					Result->SetArrayField(TEXT("components"), ComponentsJson);
				}

				OutStructured = Result;
				OutSummary = FString::Printf(TEXT("Got state for actor '%s'."), *ActorId);
				return true;
			}
		, nullptr
		, 5
		});

		// P0-F3: scene_get_overview — one-stop scene summary for Agent context awareness
		Registry.Register({
			TEXT("scene_get_overview"),
			TEXT("Get a high-level overview of the current scene: world info, actor count by class, selected actors with transforms, and World Partition status. Call this at the start of any spatial task to understand the scene before making changes."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("include_selected_transforms"), FSololmcpSchemaBuilder::Boolean(TEXT("Include full transforms for selected actors. Default true."))},
					{TEXT("max_class_entries"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum number of actor class entries to return in actorsByClass. Default 20."))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Error;
				UWorld* World = Context.Services.GetEditorWorld(Error);
				if (!World)
				{
					OutError = Error;
					return false;
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

				// World & level info
				Result->SetStringField(TEXT("world"), World->GetName());
				if (ULevel* Level = World->GetCurrentLevel())
				{
					Result->SetStringField(TEXT("current_level"), Level->GetOutermost()->GetName());
				}

				// World Partition status
				UWorldPartition* WorldPartition = World->GetWorldPartition();
				Result->SetBoolField(TEXT("is_world_partition"), WorldPartition != nullptr);

				// Count all actors and group by class (top N classes)
				TMap<FString, int32> ClassCounts;
				int32 TotalActors = 0;
				FBox SceneBounds(ForceInit);
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor)
					{
						continue;
					}
					TotalActors++;
					const FString ClassName = Actor->GetClass()->GetName();
					ClassCounts.FindOrAdd(ClassName)++;
					// Accumulate scene bounds from root component
					if (USceneComponent* Root = Actor->GetRootComponent())
					{
						SceneBounds += Root->GetComponentLocation();
					}
				}
				Result->SetNumberField(TEXT("actor_count"), TotalActors);

				// Top N classes sorted by count
				int32 MaxClassEntries = 20;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("max_class_entries")))
				{
					MaxClassEntries = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_class_entries"))), 1, 100);
				}
				ClassCounts.ValueSort([](const int32& A, const int32& B) { return A > B; });
				TSharedRef<FJsonObject> ByClassJson = MakeShared<FJsonObject>();
				int32 ClassIdx = 0;
				for (const auto& Pair : ClassCounts)
				{
					if (ClassIdx++ >= MaxClassEntries)
					{
						break;
					}
					ByClassJson->SetNumberField(Pair.Key, Pair.Value);
				}
				Result->SetObjectField(TEXT("actors_by_class"), ByClassJson);

				// Approximate scene bounds
				if (SceneBounds.IsValid)
				{
					Result->SetObjectField(TEXT("approximate_scene_bounds"), BoxToJson(SceneBounds));
					Result->SetObjectField(TEXT("approximate_scene_center"), VectorToJson(SceneBounds.GetCenter()));
				}

				// Selected actors with rich info
				bool bIncludeSelectedTransforms = true;
				Arguments->TryGetBoolField(TEXT("include_selected_transforms"), bIncludeSelectedTransforms);
				TArray<TSharedPtr<FJsonValue>> SelectedJson;
				if (USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr)
				{
					for (int32 i = 0; i < Selection->Num(); ++i)
					{
						if (AActor* SelActor = Cast<AActor>(Selection->GetSelectedObject(i)))
						{
							TSharedRef<FJsonObject> SelJson = FSololmcpEditorServices::MakeActorReference(SelActor);
							if (bIncludeSelectedTransforms)
							{
								SelJson->SetObjectField(TEXT("transform"), TransformToJson(SelActor->GetActorTransform()));
							}
							SelectedJson.Add(MakeShared<FJsonValueObject>(SelJson));
						}
					}
				}
				Result->SetArrayField(TEXT("selected_actors"), SelectedJson);
				Result->SetNumberField(TEXT("selected_count"), SelectedJson.Num());

				OutStructured = Result;
				OutSummary = FString::Printf(TEXT("Scene overview: %d actors, %d selected."), TotalActors, SelectedJson.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// P0-F4: scene_perception_snapshot — richer one-call context for all content-creation Agents.
		Registry.Register({
			TEXT("scene_perception_snapshot"),
			TEXT("Build a read-only Agent perception snapshot of the current editor scene: actor/component class distribution, selected actors, sampled transforms/bounds, referenced mesh/material/animation/audio assets, scene complexity metrics, risk flags, and recommended next tools. Use this before complex scene layout, PCG generation, asset deployment, lighting, Blueprint attachment, or QA handoff."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("name_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional substring filter for actor label or object name."))},
					{TEXT("class_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional substring filter for actor class name."))},
					{TEXT("max_actor_samples"), FSololmcpSchemaBuilder::Integer(TEXT("Max actor samples with transform/bounds/component summary. Default 300; 0 = no samples."))},
					{TEXT("max_asset_refs"), FSololmcpSchemaBuilder::Integer(TEXT("Max referenced asset rows. Default 400."))},
					{TEXT("include_components"), FSololmcpSchemaBuilder::Boolean(TEXT("Include per-sampled-actor component summaries. Default true."))},
					{TEXT("include_asset_refs"), FSololmcpSchemaBuilder::Boolean(TEXT("Include referenced asset rows. Default true."))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Error;
				UWorld* World = Context.Services.GetEditorWorld(Error);
				if (!World)
				{
					OutError = Error;
					return false;
				}

				FString NameFilter, ClassFilter;
				Arguments->TryGetStringField(TEXT("name_filter"), NameFilter);
				Arguments->TryGetStringField(TEXT("class_filter"), ClassFilter);
				int32 MaxActorSamples = 300;
				int32 MaxAssetRefs = 400;
				Arguments->TryGetNumberField(TEXT("max_actor_samples"), MaxActorSamples);
				Arguments->TryGetNumberField(TEXT("max_asset_refs"), MaxAssetRefs);
				MaxActorSamples = FMath::Clamp(MaxActorSamples, 0, 5000);
				MaxAssetRefs = FMath::Clamp(MaxAssetRefs, 0, 5000);
				bool bIncludeComponents = true;
				bool bIncludeAssetRefs = true;
				Arguments->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
				Arguments->TryGetBoolField(TEXT("include_asset_refs"), bIncludeAssetRefs);

				auto FamilyFromObject = [](UObject* Obj) -> FString
				{
					if (!Obj) return TEXT("unknown");
					const FString C = Obj->GetClass()->GetName();
					if (C.Contains(TEXT("Texture"))) return TEXT("texture");
					if (C.Contains(TEXT("Material"))) return TEXT("material");
					if (C.Contains(TEXT("StaticMesh"))) return TEXT("static_mesh");
					if (C.Contains(TEXT("SkeletalMesh"))) return TEXT("skeletal_mesh");
					if (C.Contains(TEXT("Anim")) || C.Contains(TEXT("BlendSpace"))) return TEXT("animation");
					if (C.Contains(TEXT("Niagara"))) return TEXT("niagara");
					if (C.Contains(TEXT("Sound")) || C.Contains(TEXT("Audio"))) return TEXT("audio");
					if (C.Contains(TEXT("Blueprint"))) return TEXT("blueprint");
					return TEXT("asset");
				};

				TMap<FString, int32> ActorClassCounts;
				TMap<FString, int32> ComponentClassCounts;
				TMap<FString, int32> AssetFamilyCounts;
				TSet<FString> UniqueAssetPaths;
				TArray<TSharedPtr<FJsonValue>> AssetRefs;
				TArray<TSharedPtr<FJsonValue>> ActorSamples;
				TArray<TSharedPtr<FJsonValue>> SelectedActors;
				TArray<FString> RiskFlags;
				FBox SceneBounds(ForceInit);

				int32 TotalActors = 0;
				int32 MatchedActors = 0;
				int32 HiddenActors = 0;
				int32 PrimitiveComponentCount = 0;
				int32 StaticMeshComponentCount = 0;
				int32 SkeletalMeshComponentCount = 0;
				int32 LightComponentCount = 0;
				int32 CameraComponentCount = 0;
				int32 AudioComponentCount = 0;
				int32 SplineComponentCount = 0;
				int32 LandscapeActorCount = 0;
				int32 PcgActorOrComponentCount = 0;
				int32 MissingMeshRefs = 0;
				int32 EmptyMaterialSlots = 0;
				int32 ZeroBoundsActors = 0;

				auto AddAssetRef = [&](UObject* Asset, const FString& Reason, const FString& OwnerActor)
				{
					if (!Asset)
					{
						return;
					}
					const FString Path = Asset->GetPathName();
					if (Path.IsEmpty() || Path.StartsWith(TEXT("/Temp/")))
					{
						return;
					}
					const FString Family = FamilyFromObject(Asset);
					AssetFamilyCounts.FindOrAdd(Family)++;
					if (UniqueAssetPaths.Contains(Path))
					{
						return;
					}
					UniqueAssetPaths.Add(Path);
					if (bIncludeAssetRefs && AssetRefs.Num() < MaxAssetRefs)
					{
						TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
						Ref->SetStringField(TEXT("path"), Path);
						Ref->SetStringField(TEXT("name"), Asset->GetName());
						Ref->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
						Ref->SetStringField(TEXT("family"), Family);
						Ref->SetStringField(TEXT("reason"), Reason);
						Ref->SetStringField(TEXT("owner_actor"), OwnerActor);
						Ref->SetStringField(TEXT("recommended_profile_tool"), TEXT("asset_recognition_profile"));
						AssetRefs.Add(MakeShared<FJsonValueObject>(Ref));
					}
				};

				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor) continue;
					TotalActors++;
					const FString ActorClass = Actor->GetClass()->GetName();
					if (!NameFilter.IsEmpty() && !Actor->GetActorLabel().Contains(NameFilter) && !Actor->GetName().Contains(NameFilter))
					{
						continue;
					}
					if (!ClassFilter.IsEmpty() && !ActorClass.Contains(ClassFilter))
					{
						continue;
					}
					MatchedActors++;
					ActorClassCounts.FindOrAdd(ActorClass)++;
					if (Actor->IsHidden()) HiddenActors++;
					if (Cast<ALandscapeProxy>(Actor)) LandscapeActorCount++;
					if (ActorClass.Contains(TEXT("PCG"))) PcgActorOrComponentCount++;

					FBox ActorBounds(ForceInit);
					TArray<TSharedPtr<FJsonValue>> ComponentRows;
					int32 ActorStaticMeshComponents = 0;
					int32 ActorSkeletalMeshComponents = 0;
					int32 ActorMaterialRefs = 0;

					for (UActorComponent* Comp : Actor->GetComponents())
					{
						if (!Comp) continue;
						const FString CompClass = Comp->GetClass()->GetName();
						ComponentClassCounts.FindOrAdd(CompClass)++;
						if (CompClass.Contains(TEXT("PCG"))) PcgActorOrComponentCount++;

						TSharedRef<FJsonObject> CompJson = MakeShared<FJsonObject>();
						CompJson->SetStringField(TEXT("name"), Comp->GetName());
						CompJson->SetStringField(TEXT("class"), CompClass);

						if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Comp))
						{
							PrimitiveComponentCount++;
							if (Prim->IsRegistered())
							{
								const FBox B = Prim->Bounds.GetBox();
								ActorBounds += B;
								SceneBounds += B;
								CompJson->SetObjectField(TEXT("bounds"), BoxToJson(B));
							}
							CompJson->SetBoolField(TEXT("visible"), Prim->IsVisible());
						}

						if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Comp))
						{
							StaticMeshComponentCount++;
							ActorStaticMeshComponents++;
							if (UStaticMesh* Mesh = SMC->GetStaticMesh())
							{
								AddAssetRef(Mesh, TEXT("static_mesh_component"), Actor->GetActorLabel());
								CompJson->SetStringField(TEXT("static_mesh"), Mesh->GetPathName());
							}
							else
							{
								MissingMeshRefs++;
								CompJson->SetBoolField(TEXT("missing_static_mesh"), true);
							}
						}

						if (USkeletalMeshComponent* SKC = Cast<USkeletalMeshComponent>(Comp))
						{
							SkeletalMeshComponentCount++;
							ActorSkeletalMeshComponents++;
							if (USkeletalMesh* Mesh = SKC->GetSkeletalMeshAsset())
							{
								AddAssetRef(Mesh, TEXT("skeletal_mesh_component"), Actor->GetActorLabel());
								CompJson->SetStringField(TEXT("skeletal_mesh"), Mesh->GetPathName());
							}
							else
							{
								MissingMeshRefs++;
								CompJson->SetBoolField(TEXT("missing_skeletal_mesh"), true);
							}
							if (UAnimInstance* AnimInstance = SKC->GetAnimInstance())
							{
								CompJson->SetStringField(TEXT("anim_instance_class"), AnimInstance->GetClass()->GetName());
							}
						}

						if (UMeshComponent* MeshComp = Cast<UMeshComponent>(Comp))
						{
							const int32 MatCount = MeshComp->GetNumMaterials();
							CompJson->SetNumberField(TEXT("material_slot_count"), MatCount);
							for (int32 MatIndex = 0; MatIndex < MatCount; ++MatIndex)
							{
								if (UMaterialInterface* Mat = MeshComp->GetMaterial(MatIndex))
								{
									ActorMaterialRefs++;
									AddAssetRef(Mat, TEXT("mesh_material_slot"), Actor->GetActorLabel());
								}
								else
								{
									EmptyMaterialSlots++;
								}
							}
						}

						if (Cast<ULightComponent>(Comp)) LightComponentCount++;
						if (Cast<UCameraComponent>(Comp)) CameraComponentCount++;
						if (Cast<UAudioComponent>(Comp)) AudioComponentCount++;
						if (Cast<USplineComponent>(Comp)) SplineComponentCount++;

						if (bIncludeComponents && ComponentRows.Num() < 32)
						{
							ComponentRows.Add(MakeShared<FJsonValueObject>(CompJson));
						}
					}

					if (!ActorBounds.IsValid)
					{
						ZeroBoundsActors++;
					}

					if (ActorSamples.Num() < MaxActorSamples)
					{
						TSharedRef<FJsonObject> ActorJson = FSololmcpEditorServices::MakeActorReference(Actor);
						ActorJson->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetOutermost()->GetName() : TEXT(""));
						ActorJson->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetActorTransform()));
						ActorJson->SetBoolField(TEXT("hidden"), Actor->IsHidden());
						ActorJson->SetNumberField(TEXT("static_mesh_component_count"), ActorStaticMeshComponents);
						ActorJson->SetNumberField(TEXT("skeletal_mesh_component_count"), ActorSkeletalMeshComponents);
						ActorJson->SetNumberField(TEXT("material_reference_count"), ActorMaterialRefs);
						if (ActorBounds.IsValid)
						{
							ActorJson->SetObjectField(TEXT("bounds"), BoxToJson(ActorBounds));
						}
						if (bIncludeComponents)
						{
							ActorJson->SetArrayField(TEXT("components"), ComponentRows);
						}
						ActorSamples.Add(MakeShared<FJsonValueObject>(ActorJson));
					}
				}

				if (USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr)
				{
					for (int32 i = 0; i < Selection->Num(); ++i)
					{
						if (AActor* SelActor = Cast<AActor>(Selection->GetSelectedObject(i)))
						{
							TSharedRef<FJsonObject> Sel = FSololmcpEditorServices::MakeActorReference(SelActor);
							Sel->SetObjectField(TEXT("transform"), TransformToJson(SelActor->GetActorTransform()));
							SelectedActors.Add(MakeShared<FJsonValueObject>(Sel));
						}
					}
				}

				if (MatchedActors > MaxActorSamples && MaxActorSamples > 0) RiskFlags.Add(TEXT("actor_samples_truncated"));
				if (HiddenActors > 0) RiskFlags.Add(TEXT("hidden_actors_present"));
				if (LightComponentCount == 0) RiskFlags.Add(TEXT("no_lights_detected"));
				if (CameraComponentCount == 0) RiskFlags.Add(TEXT("no_camera_detected"));
				if (MissingMeshRefs > 0) RiskFlags.Add(TEXT("missing_mesh_references"));
				if (EmptyMaterialSlots > 0) RiskFlags.Add(TEXT("empty_material_slots"));
				if (ZeroBoundsActors > 0) RiskFlags.Add(TEXT("zero_bounds_or_nonprimitive_actors"));
				if (PcgActorOrComponentCount > 0) RiskFlags.Add(TEXT("pcg_present_requires_generation_health_audit"));
				if (LandscapeActorCount > 0) RiskFlags.Add(TEXT("landscape_present_requires_terrain_metrics_for_sculpting"));

				ActorClassCounts.ValueSort([](const int32& A, const int32& B) { return A > B; });
				ComponentClassCounts.ValueSort([](const int32& A, const int32& B) { return A > B; });

				auto StringListToJson = [](const TArray<FString>& Values) -> TArray<TSharedPtr<FJsonValue>>
				{
					TArray<TSharedPtr<FJsonValue>> Out;
					for (const FString& Value : Values)
					{
						Out.Add(MakeShared<FJsonValueString>(Value));
					}
					return Out;
				};

				auto CountsToJson = [](const TMap<FString, int32>& Counts, int32 MaxEntries) -> TSharedRef<FJsonObject>
				{
					TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					int32 I = 0;
					for (const auto& Pair : Counts)
					{
						if (I++ >= MaxEntries) break;
						O->SetNumberField(Pair.Key, Pair.Value);
					}
					return O;
				};

				TSharedRef<FJsonObject> Complexity = MakeShared<FJsonObject>();
				Complexity->SetNumberField(TEXT("total_actors"), TotalActors);
				Complexity->SetNumberField(TEXT("matched_actors"), MatchedActors);
				Complexity->SetNumberField(TEXT("sampled_actors"), ActorSamples.Num());
				Complexity->SetNumberField(TEXT("primitive_components"), PrimitiveComponentCount);
				Complexity->SetNumberField(TEXT("static_mesh_components"), StaticMeshComponentCount);
				Complexity->SetNumberField(TEXT("skeletal_mesh_components"), SkeletalMeshComponentCount);
				Complexity->SetNumberField(TEXT("light_components"), LightComponentCount);
				Complexity->SetNumberField(TEXT("camera_components"), CameraComponentCount);
				Complexity->SetNumberField(TEXT("audio_components"), AudioComponentCount);
				Complexity->SetNumberField(TEXT("spline_components"), SplineComponentCount);
				Complexity->SetNumberField(TEXT("landscape_actors"), LandscapeActorCount);
				Complexity->SetNumberField(TEXT("pcg_actor_or_component_hits"), PcgActorOrComponentCount);
				Complexity->SetNumberField(TEXT("unique_referenced_assets"), UniqueAssetPaths.Num());
				Complexity->SetNumberField(TEXT("empty_material_slots"), EmptyMaterialSlots);
				Complexity->SetNumberField(TEXT("missing_mesh_references"), MissingMeshRefs);

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("schema"), TEXT("somol.scene_perception_snapshot.v1"));
				Result->SetStringField(TEXT("world"), World->GetName());
				Result->SetStringField(TEXT("world_path"), World->GetPathName());
				Result->SetObjectField(TEXT("complexity"), Complexity);
				Result->SetObjectField(TEXT("actors_by_class"), CountsToJson(ActorClassCounts, 80));
				Result->SetObjectField(TEXT("components_by_class"), CountsToJson(ComponentClassCounts, 80));
				Result->SetObjectField(TEXT("referenced_assets_by_family"), CountsToJson(AssetFamilyCounts, 40));
				if (SceneBounds.IsValid)
				{
					Result->SetObjectField(TEXT("scene_bounds"), BoxToJson(SceneBounds));
					Result->SetObjectField(TEXT("scene_center"), VectorToJson(SceneBounds.GetCenter()));
				}
				Result->SetArrayField(TEXT("actor_samples"), ActorSamples);
				Result->SetArrayField(TEXT("selected_actors"), SelectedActors);
				Result->SetArrayField(TEXT("referenced_assets"), AssetRefs);
				Result->SetArrayField(TEXT("risk_flags"), StringListToJson(RiskFlags));
				const TArray<FString> RecommendedNextTools = {
					TEXT("asset_recognition_profile"),
					TEXT("actor_get_state"),
					TEXT("scene_get_overview"),
					TEXT("world_partition_status_lite"),
					TEXT("pcg_generated_dependency_graph"),
					TEXT("pcg_generated_actor_health_audit"),
					TEXT("editor_screenshot_viewport")
				};
				Result->SetArrayField(TEXT("recommended_next_tools"), StringListToJson(RecommendedNextTools));

				OutStructured = Result;
				OutSummary = FString::Printf(TEXT("Scene perception snapshot: %d actors, %d unique assets, %d risk flags."),
					MatchedActors, UniqueAssetPaths.Num(), RiskFlags.Num());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_create"),
			TEXT("Create a light actor of a specified type."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("light_type"), FSololmcpSchemaBuilder::String(TEXT("directional | point | spot | rect | sky"))},
					{TEXT("location"), VectorSchema()},
					{TEXT("rotation"), RotatorSchema()},
					{TEXT("properties"), FSololmcpSchemaBuilder::Object({})}
				},
				{TEXT("light_type")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Type;
				if (!Arguments->TryGetStringField(TEXT("light_type"), Type))
				{
					OutError = TEXT("Missing argument: light_type");
					return false;
				}
				UClass* LightClass = ResolveActorClassByType(Context.Services, Type, OutError);
				if (!LightClass)
				{
					return false;
				}

				TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
				SpawnArgs->SetStringField(TEXT("class_path"), LightClass->GetPathName());
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("location")))
				{
					SpawnArgs->SetField(TEXT("location"), *Value);
				}
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("rotation")))
				{
					SpawnArgs->SetField(TEXT("rotation"), *Value);
				}
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("properties")))
				{
					SpawnArgs->SetField(TEXT("properties"), *Value);
				}

				return Registry.ExecuteTool(TEXT("actor_spawn"), SpawnArgs, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_create"),
			TEXT("Create a camera actor or cine camera actor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("camera_type"), FSololmcpSchemaBuilder::String(TEXT("camera | cine"), {TEXT("camera"), TEXT("cine")})},
					{TEXT("location"), VectorSchema()},
					{TEXT("rotation"), RotatorSchema()},
					{TEXT("properties"), FSololmcpSchemaBuilder::Object({})},
					{TEXT("pilot"), FSololmcpSchemaBuilder::Boolean()}
				}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString CameraType = TEXT("camera");
				Arguments->TryGetStringField(TEXT("camera_type"), CameraType);
				UClass* CameraClass = ResolveActorClassByType(Context.Services, CameraType, OutError);
				if (!CameraClass)
				{
					return false;
				}
				const bool bPilotRequested = Arguments->HasTypedField<EJson::Boolean>(TEXT("pilot")) && Arguments->GetBoolField(TEXT("pilot"));
				if (bPilotRequested && (!GEditor || !GEditor->GetActiveViewport()))
				{
					OutError = TEXT("Cannot create and pilot camera: no active level viewport is available.");
					return false;
				}

				TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
				SpawnArgs->SetStringField(TEXT("class_path"), CameraClass->GetPathName());
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("location")))
				{
					SpawnArgs->SetField(TEXT("location"), *Value);
				}
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("rotation")))
				{
					SpawnArgs->SetField(TEXT("rotation"), *Value);
				}
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("properties")))
				{
					SpawnArgs->SetField(TEXT("properties"), *Value);
				}

				if (!Registry.ExecuteTool(TEXT("actor_spawn"), SpawnArgs, OutStructured, OutSummary, OutError))
				{
					return false;
				}

				if (bPilotRequested)
				{
					const FString ActorPath = OutStructured->GetStringField(TEXT("path"));
					AActor* Actor = Context.Services.FindActorByLabelOrName(ActorPath, OutError);
					if (!Actor)
					{
						return false;
					}
					FString Error;
					ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(Error);
					if (!LevelSubsystem)
					{
						OutError = Error;
						return false;
					}
					LevelSubsystem->PilotLevelActor(Actor);
					OutStructured->SetBoolField(TEXT("pilot_requested"), true);
					OutStructured->SetBoolField(TEXT("active_viewport_present"), GEditor->GetActiveViewport() != nullptr);
				}

				OutSummary = TEXT("Created camera actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_pilot"),
			TEXT("Pilot a camera or actor in the active level viewport."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing argument: actor");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				FString Error;
				ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(Error);
				if (!LevelSubsystem)
				{
					OutError = Error;
					return false;
				}
				if (!GEditor || !GEditor->GetActiveViewport())
				{
					OutError = TEXT("Cannot pilot actor: no active level viewport is available.");
					return false;
				}
				LevelSubsystem->PilotLevelActor(Actor);
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutStructured->SetBoolField(TEXT("pilot_requested"), true);
				OutStructured->SetBoolField(TEXT("active_viewport_present"), GEditor->GetActiveViewport() != nullptr);
				OutSummary = TEXT("Piloting actor in viewport.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_get_cine_settings"),
			TEXT("Get current settings from a cine camera actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing argument: actor");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Fetched cine camera settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_cine_lens"),
			TEXT("Set focal length, aperture, or lens limits on a cine camera."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("current_focal_length"), FSololmcpSchemaBuilder::Number()},
					{TEXT("current_aperture"), FSololmcpSchemaBuilder::Number()},
					{TEXT("min_focal_length"), FSololmcpSchemaBuilder::Number()},
					{TEXT("max_focal_length"), FSololmcpSchemaBuilder::Number()},
					{TEXT("min_fstop"), FSololmcpSchemaBuilder::Number()},
					{TEXT("max_fstop"), FSololmcpSchemaBuilder::Number()},
					{TEXT("minimum_focus_distance"), FSololmcpSchemaBuilder::Number()},
					{TEXT("squeeze_factor"), FSololmcpSchemaBuilder::Number()},
					{TEXT("diaphragm_blade_count"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing argument: actor");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				UCineCameraComponent* CameraComponent = Actor->GetCineCameraComponent();
				if (!CameraComponent)
				{
					OutError = TEXT("Cine camera component was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CameraSetCineLens", "SOMOLMCP Set Cine Camera Lens"));
				FCameraLensSettings LensSettings = CameraComponent->LensSettings;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("min_focal_length"))) { LensSettings.MinFocalLength = Arguments->GetNumberField(TEXT("min_focal_length")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("max_focal_length"))) { LensSettings.MaxFocalLength = Arguments->GetNumberField(TEXT("max_focal_length")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("min_fstop"))) { LensSettings.MinFStop = Arguments->GetNumberField(TEXT("min_fstop")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("max_fstop"))) { LensSettings.MaxFStop = Arguments->GetNumberField(TEXT("max_fstop")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("minimum_focus_distance"))) { LensSettings.MinimumFocusDistance = Arguments->GetNumberField(TEXT("minimum_focus_distance")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("squeeze_factor"))) { LensSettings.SqueezeFactor = Arguments->GetNumberField(TEXT("squeeze_factor")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("diaphragm_blade_count"))) { LensSettings.DiaphragmBladeCount = Arguments->GetIntegerField(TEXT("diaphragm_blade_count")); }
				CameraComponent->SetLensSettings(LensSettings);

				if (Arguments->HasTypedField<EJson::Number>(TEXT("current_focal_length")))
				{
					CameraComponent->SetCurrentFocalLength(Arguments->GetNumberField(TEXT("current_focal_length")));
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("current_aperture")))
				{
					CameraComponent->SetCurrentAperture(Arguments->GetNumberField(TEXT("current_aperture")));
				}

				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Updated cine camera lens settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_cine_filmback"),
			TEXT("Set filmback dimensions or crop on a cine camera."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("sensor_width"), FSololmcpSchemaBuilder::Number()},
					{TEXT("sensor_height"), FSololmcpSchemaBuilder::Number()},
					{TEXT("sensor_horizontal_offset"), FSololmcpSchemaBuilder::Number()},
					{TEXT("sensor_vertical_offset"), FSololmcpSchemaBuilder::Number()},
					{TEXT("crop_aspect_ratio"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing argument: actor");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				UCineCameraComponent* CameraComponent = Actor->GetCineCameraComponent();
				if (!CameraComponent)
				{
					OutError = TEXT("Cine camera component was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CameraSetCineFilmback", "SOMOLMCP Set Cine Camera Filmback"));
				FCameraFilmbackSettings Filmback = CameraComponent->Filmback;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("sensor_width"))) { Filmback.SensorWidth = Arguments->GetNumberField(TEXT("sensor_width")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("sensor_height"))) { Filmback.SensorHeight = Arguments->GetNumberField(TEXT("sensor_height")); }
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				if (Arguments->HasTypedField<EJson::Number>(TEXT("sensor_horizontal_offset"))) { Filmback.SensorHorizontalOffset = Arguments->GetNumberField(TEXT("sensor_horizontal_offset")); }
#else
				// FCameraFilmbackSettings sensor offsets arrived in 5.5.
#endif
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				if (Arguments->HasTypedField<EJson::Number>(TEXT("sensor_vertical_offset"))) { Filmback.SensorVerticalOffset = Arguments->GetNumberField(TEXT("sensor_vertical_offset")); }
#else
				// FCameraFilmbackSettings sensor offsets arrived in 5.5.
#endif
				Filmback.RecalcSensorAspectRatio();
				CameraComponent->SetFilmback(Filmback);

				if (Arguments->HasTypedField<EJson::Number>(TEXT("crop_aspect_ratio")))
				{
					FPlateCropSettings CropSettings = CameraComponent->CropSettings;
					CropSettings.AspectRatio = Arguments->GetNumberField(TEXT("crop_aspect_ratio"));
					CameraComponent->SetCropSettings(CropSettings);
				}

				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Updated cine camera filmback settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_cine_focus"),
			TEXT("Set manual or tracking focus settings on a cine camera."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("focus_method"), FSololmcpSchemaBuilder::String(TEXT("DoNotOverride | Manual | Tracking | Disable"))},
					{TEXT("manual_focus_distance"), FSololmcpSchemaBuilder::Number()},
					{TEXT("tracking_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("tracking_relative_offset"), VectorSchema()},
					{TEXT("smooth_focus_changes"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("focus_smoothing_interp_speed"), FSololmcpSchemaBuilder::Number()},
					{TEXT("focus_offset"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing argument: actor");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				UCineCameraComponent* CameraComponent = Actor->GetCineCameraComponent();
				if (!CameraComponent)
				{
					OutError = TEXT("Cine camera component was not found.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CameraSetCineFocus", "SOMOLMCP Set Cine Camera Focus"));
				FCameraFocusSettings FocusSettings = CameraComponent->FocusSettings;
				FString FocusMethod;
				if (Arguments->TryGetStringField(TEXT("focus_method"), FocusMethod))
				{
					if (FocusMethod == TEXT("DoNotOverride")) { FocusSettings.FocusMethod = ECameraFocusMethod::DoNotOverride; }
					else if (FocusMethod == TEXT("Manual")) { FocusSettings.FocusMethod = ECameraFocusMethod::Manual; }
					else if (FocusMethod == TEXT("Tracking")) { FocusSettings.FocusMethod = ECameraFocusMethod::Tracking; }
					else if (FocusMethod == TEXT("Disable")) { FocusSettings.FocusMethod = ECameraFocusMethod::Disable; }
					else
					{
						OutError = TEXT("Unsupported focus_method.");
						return false;
					}
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("manual_focus_distance"))) { FocusSettings.ManualFocusDistance = Arguments->GetNumberField(TEXT("manual_focus_distance")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("smooth_focus_changes"))) { FocusSettings.bSmoothFocusChanges = Arguments->GetBoolField(TEXT("smooth_focus_changes")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("focus_smoothing_interp_speed"))) { FocusSettings.FocusSmoothingInterpSpeed = Arguments->GetNumberField(TEXT("focus_smoothing_interp_speed")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("focus_offset"))) { FocusSettings.FocusOffset = Arguments->GetNumberField(TEXT("focus_offset")); }

				FString TrackingActorId;
				if (Arguments->TryGetStringField(TEXT("tracking_actor"), TrackingActorId) && !TrackingActorId.IsEmpty())
				{
					AActor* TrackingActor = Context.Services.FindActorByLabelOrName(TrackingActorId, OutError);
					if (!TrackingActor)
					{
						return false;
					}
					FocusSettings.TrackingFocusSettings.ActorToTrack = TrackingActor;
				}
				TSharedPtr<FJsonObject> TrackingOffsetJson;
				if (TryGetObjectField(Arguments, TEXT("tracking_relative_offset"), TrackingOffsetJson))
				{
					FVector Offset;
					if (!FSololmcpEditorServices::JsonToVector(TrackingOffsetJson, Offset))
					{
						OutError = TEXT("tracking_relative_offset must be a vector object.");
						return false;
					}
					FocusSettings.TrackingFocusSettings.RelativeOffset = Offset;
				}

				CameraComponent->SetFocusSettings(FocusSettings);
				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Updated cine camera focus settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_lookat_tracking"),
			TEXT("Configure look-at tracking on a cine camera actor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("target_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("relative_offset"), VectorSchema()},
					{TEXT("interp_speed"), FSololmcpSchemaBuilder::Number()},
					{TEXT("allow_roll"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("draw_debug"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing argument: actor");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CameraSetLookAtTracking", "SOMOLMCP Set Cine Camera Look At"));
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("enabled"))) { Actor->LookatTrackingSettings.bEnableLookAtTracking = Arguments->GetBoolField(TEXT("enabled")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("allow_roll"))) { Actor->LookatTrackingSettings.bAllowRoll = Arguments->GetBoolField(TEXT("allow_roll")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("draw_debug"))) { Actor->LookatTrackingSettings.bDrawDebugLookAtTrackingPosition = Arguments->GetBoolField(TEXT("draw_debug")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("interp_speed"))) { Actor->LookatTrackingSettings.LookAtTrackingInterpSpeed = Arguments->GetNumberField(TEXT("interp_speed")); }

				FString TargetActorId;
				if (Arguments->TryGetStringField(TEXT("target_actor"), TargetActorId))
				{
					if (TargetActorId.IsEmpty())
					{
						Actor->LookatTrackingSettings.ActorToTrack = nullptr;
					}
					else
					{
						AActor* TargetActor = Context.Services.FindActorByLabelOrName(TargetActorId, OutError);
						if (!TargetActor)
						{
							return false;
						}
						Actor->LookatTrackingSettings.ActorToTrack = TargetActor;
					}
				}

				TSharedPtr<FJsonObject> OffsetJson;
				if (TryGetObjectField(Arguments, TEXT("relative_offset"), OffsetJson))
				{
					FVector Offset;
					if (!FSololmcpEditorServices::JsonToVector(OffsetJson, Offset))
					{
						OutError = TEXT("relative_offset must be a vector object.");
						return false;
					}
					Actor->LookatTrackingSettings.RelativeOffset = Offset;
				}

				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Updated cine camera look-at tracking.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_filmback_preset"),
			TEXT("Apply a filmback preset to a cine camera."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("preset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor"), TEXT("preset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				FString PresetName;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetStringField(TEXT("preset_name"), PresetName))
				{
					OutError = TEXT("Missing actor or preset_name.");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				Actor->GetCineCameraComponent()->SetFilmbackPresetByName(PresetName);
				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Applied filmback preset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_lens_preset"),
			TEXT("Apply a lens preset to a cine camera."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("preset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor"), TEXT("preset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				FString PresetName;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetStringField(TEXT("preset_name"), PresetName))
				{
					OutError = TEXT("Missing actor or preset_name.");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				Actor->GetCineCameraComponent()->SetLensPresetByName(PresetName);
				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Applied lens preset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_crop_preset"),
			TEXT("Apply a crop preset to a cine camera."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("preset_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor"), TEXT("preset_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				FString PresetName;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetStringField(TEXT("preset_name"), PresetName))
				{
					OutError = TEXT("Missing actor or preset_name.");
					return false;
				}
				ACineCameraActor* Actor = ResolveCineCameraActor(Context.Services, ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				Actor->GetCineCameraComponent()->SetCropPresetByName(PresetName);
				OutStructured = CineCameraActorToJson(Actor);
				OutSummary = TEXT("Applied crop preset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_get_settings"),
			TEXT("Get common settings from a light component."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				ULightComponent* LightComponent = ResolveLightComponent(Context.Services, ActorId, OutError);
				if (!LightComponent)
				{
					return false;
				}
				OutStructured->SetNumberField(TEXT("intensity"), LightComponent->Intensity);
				OutStructured->SetObjectField(TEXT("lightColor"), LinearColorToJson(LightComponent->GetLightColor()));
				OutStructured->SetBoolField(TEXT("castShadows"), LightComponent->CastShadows);
				OutStructured->SetBoolField(TEXT("useTemperature"), LightComponent->bUseTemperature);
				OutStructured->SetNumberField(TEXT("temperature"), LightComponent->Temperature);
				OutSummary = TEXT("Collected light settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_set_basic"),
			TEXT("Set common intensity and color settings on a light."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("intensity"), FSololmcpSchemaBuilder::Number()}, {TEXT("light_color"), ColorSchema()}, {TEXT("use_temperature"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("temperature"), FSololmcpSchemaBuilder::Number()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				ULightComponent* LightComponent = ResolveLightComponent(Context.Services, ActorId, OutError);
				if (!LightComponent)
				{
					return false;
				}
				const bool bHasIntensity = Arguments->HasTypedField<EJson::Number>(TEXT("intensity"));
				const bool bHasLightColor = Arguments->HasField(TEXT("light_color"));
				const bool bHasUseTemperature = Arguments->HasTypedField<EJson::Boolean>(TEXT("use_temperature"));
				const bool bHasTemperature = Arguments->HasTypedField<EJson::Number>(TEXT("temperature"));
				if (!bHasIntensity && !bHasLightColor && !bHasUseTemperature && !bHasTemperature)
				{
					OutError = TEXT("No light basic settings were provided.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LightSetBasic", "SOMOLMCP Set Light Basic"));
				double RequestedIntensity = 0.0;
				if (bHasIntensity)
				{
					RequestedIntensity = Arguments->GetNumberField(TEXT("intensity"));
					LightComponent->SetIntensity(RequestedIntensity);
				}
				TSharedPtr<FJsonObject> ColorObject;
				FLinearColor RequestedColor = FLinearColor::White;
				if (bHasLightColor)
				{
					if (!TryGetObjectField(Arguments, TEXT("light_color"), ColorObject))
					{
						OutError = TEXT("light_color must be a color object.");
						return false;
					}
					if (!FSololmcpEditorServices::JsonToLinearColor(ColorObject, RequestedColor))
					{
						OutError = TEXT("light_color must be a color object.");
						return false;
					}
					LightComponent->SetLightColor(RequestedColor);
				}
				bool bRequestedUseTemperature = false;
				if (bHasUseTemperature)
				{
					bRequestedUseTemperature = Arguments->GetBoolField(TEXT("use_temperature"));
					LightComponent->SetUseTemperature(bRequestedUseTemperature);
				}
				double RequestedTemperature = 0.0;
				if (bHasTemperature)
				{
					RequestedTemperature = Arguments->GetNumberField(TEXT("temperature"));
					LightComponent->SetTemperature(RequestedTemperature);
				}
				if (bHasIntensity && !FMath::IsNearlyEqual(static_cast<double>(LightComponent->Intensity), RequestedIntensity, 0.001))
				{
					OutError = FString::Printf(TEXT("Light intensity did not apply: requested %.6f, actual %.6f."), RequestedIntensity, static_cast<double>(LightComponent->Intensity));
					return false;
				}
				if (bHasLightColor && !LightComponent->GetLightColor().Equals(RequestedColor, 0.01f))
				{
					OutError = TEXT("Light color did not apply.");
					return false;
				}
				if (bHasUseTemperature && LightComponent->bUseTemperature != bRequestedUseTemperature)
				{
					OutError = TEXT("Light temperature toggle did not apply.");
					return false;
				}
				if (bHasTemperature && !FMath::IsNearlyEqual(static_cast<double>(LightComponent->Temperature), RequestedTemperature, 0.001))
				{
					OutError = FString::Printf(TEXT("Light temperature did not apply: requested %.6f, actual %.6f."), RequestedTemperature, static_cast<double>(LightComponent->Temperature));
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(LightComponent);
				OutSummary = TEXT("Updated light basic settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_set_shape"),
			TEXT("Set common shape properties on a local light."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("attenuation_radius"), FSololmcpSchemaBuilder::Number()}, {TEXT("source_radius"), FSololmcpSchemaBuilder::Number()}, {TEXT("soft_source_radius"), FSololmcpSchemaBuilder::Number()}, {TEXT("source_length"), FSololmcpSchemaBuilder::Number()}, {TEXT("inner_cone_angle"), FSololmcpSchemaBuilder::Number()}, {TEXT("outer_cone_angle"), FSololmcpSchemaBuilder::Number()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				ULightComponent* BaseLight = ResolveLightComponent(Context.Services, ActorId, OutError);
				ULocalLightComponent* Light = Cast<ULocalLightComponent>(BaseLight);
				if (!Light)
				{
					OutError = TEXT("Actor does not expose a local light component.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LightSetShape", "SOMOLMCP Set Light Shape"));
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				const bool bHasAttenuationRadius = Arguments->HasTypedField<EJson::Number>(TEXT("attenuation_radius"));
				const bool bHasSourceRadius = Arguments->HasTypedField<EJson::Number>(TEXT("source_radius"));
				const bool bHasSoftSourceRadius = Arguments->HasTypedField<EJson::Number>(TEXT("soft_source_radius"));
				const bool bHasSourceLength = Arguments->HasTypedField<EJson::Number>(TEXT("source_length"));
				const bool bHasInnerConeAngle = Arguments->HasTypedField<EJson::Number>(TEXT("inner_cone_angle"));
				const bool bHasOuterConeAngle = Arguments->HasTypedField<EJson::Number>(TEXT("outer_cone_angle"));
				if (!bHasAttenuationRadius && !bHasSourceRadius && !bHasSoftSourceRadius && !bHasSourceLength && !bHasInnerConeAngle && !bHasOuterConeAngle)
				{
					OutError = TEXT("No light shape settings were provided.");
					return false;
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("attenuation_radius"))) { Properties->SetNumberField(TEXT("AttenuationRadius"), Arguments->GetNumberField(TEXT("attenuation_radius"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("source_radius"))) { Properties->SetNumberField(TEXT("SourceRadius"), Arguments->GetNumberField(TEXT("source_radius"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("soft_source_radius"))) { Properties->SetNumberField(TEXT("SoftSourceRadius"), Arguments->GetNumberField(TEXT("soft_source_radius"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("source_length"))) { Properties->SetNumberField(TEXT("SourceLength"), Arguments->GetNumberField(TEXT("source_length"))); }
				if (USpotLightComponent* SpotLight = Cast<USpotLightComponent>(Light))
				{
					if (Arguments->HasTypedField<EJson::Number>(TEXT("inner_cone_angle"))) { Properties->SetNumberField(TEXT("InnerConeAngle"), Arguments->GetNumberField(TEXT("inner_cone_angle"))); }
					if (Arguments->HasTypedField<EJson::Number>(TEXT("outer_cone_angle"))) { Properties->SetNumberField(TEXT("OuterConeAngle"), Arguments->GetNumberField(TEXT("outer_cone_angle"))); }
					if (!Context.Services.ApplyProperties(SpotLight, Properties, OutError))
					{
						return false;
					}
					if (bHasInnerConeAngle && !VerifyNumericPropertyApplied(SpotLight, TEXT("InnerConeAngle"), Arguments->GetNumberField(TEXT("inner_cone_angle")), 0.001, OutError)) { return false; }
					if (bHasOuterConeAngle && !VerifyNumericPropertyApplied(SpotLight, TEXT("OuterConeAngle"), Arguments->GetNumberField(TEXT("outer_cone_angle")), 0.001, OutError)) { return false; }
				}
				else
				{
					if (bHasInnerConeAngle || bHasOuterConeAngle)
					{
						OutError = TEXT("Cone angles can only be set on spot light components.");
						return false;
					}
					if (!Context.Services.ApplyProperties(Light, Properties, OutError))
					{
						return false;
					}
				}
				if (bHasAttenuationRadius && !VerifyNumericPropertyApplied(Light, TEXT("AttenuationRadius"), Arguments->GetNumberField(TEXT("attenuation_radius")), 0.001, OutError)) { return false; }
				if (bHasSourceRadius && !VerifyNumericPropertyApplied(Light, TEXT("SourceRadius"), Arguments->GetNumberField(TEXT("source_radius")), 0.001, OutError)) { return false; }
				if (bHasSoftSourceRadius && !VerifyNumericPropertyApplied(Light, TEXT("SoftSourceRadius"), Arguments->GetNumberField(TEXT("soft_source_radius")), 0.001, OutError)) { return false; }
				if (bHasSourceLength && !VerifyNumericPropertyApplied(Light, TEXT("SourceLength"), Arguments->GetNumberField(TEXT("source_length")), 0.001, OutError)) { return false; }
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Light);
				OutSummary = TEXT("Updated light shape settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_set_shadowing"),
			TEXT("Set common shadowing settings on a light."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("cast_shadows"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("shadow_bias"), FSololmcpSchemaBuilder::Number()}, {TEXT("shadow_sharpen"), FSololmcpSchemaBuilder::Number()}, {TEXT("contact_shadow_length"), FSololmcpSchemaBuilder::Number()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				ULightComponent* Light = ResolveLightComponent(Context.Services, ActorId, OutError);
				if (!Light)
				{
					return false;
				}
				const bool bHasCastShadows = Arguments->HasTypedField<EJson::Boolean>(TEXT("cast_shadows"));
				const bool bHasShadowBias = Arguments->HasTypedField<EJson::Number>(TEXT("shadow_bias"));
				const bool bHasShadowSharpen = Arguments->HasTypedField<EJson::Number>(TEXT("shadow_sharpen"));
				const bool bHasContactShadowLength = Arguments->HasTypedField<EJson::Number>(TEXT("contact_shadow_length"));
				if (!bHasCastShadows && !bHasShadowBias && !bHasShadowSharpen && !bHasContactShadowLength)
				{
					OutError = TEXT("No light shadowing settings were provided.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LightSetShadowing", "SOMOLMCP Set Light Shadowing"));
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("cast_shadows"))) { Light->SetCastShadows(Arguments->GetBoolField(TEXT("cast_shadows"))); }
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				if (Arguments->HasTypedField<EJson::Number>(TEXT("shadow_bias"))) { Properties->SetNumberField(TEXT("ShadowBias"), Arguments->GetNumberField(TEXT("shadow_bias"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("shadow_sharpen"))) { Properties->SetNumberField(TEXT("ShadowSharpen"), Arguments->GetNumberField(TEXT("shadow_sharpen"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("contact_shadow_length"))) { Properties->SetNumberField(TEXT("ContactShadowLength"), Arguments->GetNumberField(TEXT("contact_shadow_length"))); }
				if (Properties->Values.Num() > 0 && !Context.Services.ApplyProperties(Light, Properties, OutError))
				{
					return false;
				}
				if (bHasCastShadows && Light->CastShadows != Arguments->GetBoolField(TEXT("cast_shadows")))
				{
					OutError = TEXT("Light shadow casting flag did not apply.");
					return false;
				}
				if (bHasShadowBias && !VerifyNumericPropertyApplied(Light, TEXT("ShadowBias"), Arguments->GetNumberField(TEXT("shadow_bias")), 0.001, OutError)) { return false; }
				if (bHasShadowSharpen && !VerifyNumericPropertyApplied(Light, TEXT("ShadowSharpen"), Arguments->GetNumberField(TEXT("shadow_sharpen")), 0.001, OutError)) { return false; }
				if (bHasContactShadowLength && !VerifyNumericPropertyApplied(Light, TEXT("ContactShadowLength"), Arguments->GetNumberField(TEXT("contact_shadow_length")), 0.001, OutError)) { return false; }
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Light);
				OutSummary = TEXT("Updated light shadowing settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_set_function_and_profile"),
			TEXT("Set light function or IES profile assets on a light."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("light_function_material_path"), FSololmcpSchemaBuilder::String()}, {TEXT("ies_texture_path"), FSololmcpSchemaBuilder::String()}, {TEXT("use_ies_brightness"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				ULightComponent* BaseLight = ResolveLightComponent(Context.Services, ActorId, OutError);
				if (!BaseLight)
				{
					return false;
				}
				const bool bHasLightFunction = Arguments->HasField(TEXT("light_function_material_path"));
				const bool bHasIesTexture = Arguments->HasField(TEXT("ies_texture_path"));
				const bool bHasUseIesBrightness = Arguments->HasTypedField<EJson::Boolean>(TEXT("use_ies_brightness"));
				if (!bHasLightFunction && !bHasIesTexture && !bHasUseIesBrightness)
				{
					OutError = TEXT("No light function/profile settings were provided.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LightSetFunctionAndProfile", "SOMOLMCP Set Light Function And Profile"));
				FString MaterialPath;
				UMaterialInterface* RequestedMaterial = nullptr;
				if (Arguments->TryGetStringField(TEXT("light_function_material_path"), MaterialPath))
				{
					if (MaterialPath.IsEmpty())
					{
						OutError = TEXT("light_function_material_path must not be empty when provided.");
						return false;
					}
					RequestedMaterial = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, OutError));
					if (!RequestedMaterial)
					{
						OutError = TEXT("light_function_material_path does not resolve to a material interface.");
						return false;
					}
					BaseLight->SetLightFunctionMaterial(RequestedMaterial);
				}
				ULocalLightComponent* LocalLight = Cast<ULocalLightComponent>(BaseLight);
				UTextureLightProfile* RequestedProfile = nullptr;
				if (LocalLight)
				{
					FString IesTexturePath;
					if (Arguments->TryGetStringField(TEXT("ies_texture_path"), IesTexturePath))
					{
						if (IesTexturePath.IsEmpty())
						{
							OutError = TEXT("ies_texture_path must not be empty when provided.");
							return false;
						}
						RequestedProfile = Cast<UTextureLightProfile>(Context.Services.LoadAsset(IesTexturePath, OutError));
						if (!RequestedProfile)
						{
							OutError = TEXT("ies_texture_path does not resolve to a light profile texture.");
							return false;
						}
						LocalLight->SetIESTexture(RequestedProfile);
					}
					if (Arguments->HasTypedField<EJson::Boolean>(TEXT("use_ies_brightness")))
					{
						LocalLight->SetUseIESBrightness(Arguments->GetBoolField(TEXT("use_ies_brightness")));
					}
				}
				else if (bHasIesTexture || bHasUseIesBrightness)
				{
					OutError = TEXT("IES profile settings can only be applied to local light components.");
					return false;
				}
				if (RequestedMaterial && !VerifyObjectPropertyApplied(BaseLight, TEXT("LightFunctionMaterial"), RequestedMaterial, OutError)) { return false; }
				if (RequestedProfile && !VerifyObjectPropertyApplied(LocalLight, TEXT("IESTexture"), RequestedProfile, OutError)) { return false; }
				if (LocalLight && bHasUseIesBrightness && !VerifyBoolPropertyApplied(LocalLight, TEXT("bUseIESBrightness"), Arguments->GetBoolField(TEXT("use_ies_brightness")), OutError)) { return false; }
				OutStructured = FSololmcpEditorServices::MakeObjectReference(BaseLight);
				OutSummary = TEXT("Updated light function/profile settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_set_sky"),
			TEXT("Set sky light specific settings."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("real_time_capture"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("cubemap_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

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
				USkyLightComponent* Light = Actor->FindComponentByClass<USkyLightComponent>();
				if (!Light)
				{
					OutError = TEXT("Actor does not expose a sky light component.");
					return false;
				}
				const bool bHasRealTimeCapture = Arguments->HasTypedField<EJson::Boolean>(TEXT("real_time_capture"));
				const bool bHasCubemapPath = Arguments->HasField(TEXT("cubemap_path"));
				if (!bHasRealTimeCapture && !bHasCubemapPath)
				{
					OutError = TEXT("No sky light settings were provided.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LightSetSky", "SOMOLMCP Set Sky Light"));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("real_time_capture"))) { Light->SetRealTimeCapture(Arguments->GetBoolField(TEXT("real_time_capture"))); }
#else
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("real_time_capture"))) { Light->bRealTimeCapture = Arguments->GetBoolField(TEXT("real_time_capture")); Light->MarkRenderStateDirty(); }
#endif
				FString CubemapPath;
				UTextureCube* RequestedTexture = nullptr;
				if (Arguments->TryGetStringField(TEXT("cubemap_path"), CubemapPath))
				{
					if (CubemapPath.IsEmpty())
					{
						OutError = TEXT("cubemap_path must not be empty when provided.");
						return false;
					}
					RequestedTexture = Cast<UTextureCube>(Context.Services.LoadAsset(CubemapPath, OutError));
					if (!RequestedTexture)
					{
						OutError = TEXT("cubemap_path does not resolve to a texture cube.");
						return false;
					}
					Light->SetCubemap(RequestedTexture);
				}
				if (bHasRealTimeCapture && !VerifyBoolPropertyApplied(Light, TEXT("bRealTimeCapture"), Arguments->GetBoolField(TEXT("real_time_capture")), OutError)) { return false; }
				if (RequestedTexture && !VerifyObjectPropertyApplied(Light, TEXT("Cubemap"), RequestedTexture, OutError)) { return false; }
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Light);
				OutSummary = TEXT("Updated sky light settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("light_set_directional_atmosphere"),
			TEXT("Set directional light atmosphere and cloud flags."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("atmosphere_sun_light"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("atmosphere_sun_light_index"), FSololmcpSchemaBuilder::Integer()}, {TEXT("cast_shadows_on_clouds"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("cast_cloud_shadows"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(ResolveLightComponent(Context.Services, ActorId, OutError));
				if (!Light)
				{
					OutError = TEXT("Actor does not expose a directional light component.");
					return false;
				}
				TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
				const bool bHasAtmosphereSunLight = Arguments->HasTypedField<EJson::Boolean>(TEXT("atmosphere_sun_light"));
				const bool bHasAtmosphereSunLightIndex = Arguments->HasTypedField<EJson::Number>(TEXT("atmosphere_sun_light_index"));
				const bool bHasCastShadowsOnClouds = Arguments->HasTypedField<EJson::Boolean>(TEXT("cast_shadows_on_clouds"));
				const bool bHasCastCloudShadows = Arguments->HasTypedField<EJson::Boolean>(TEXT("cast_cloud_shadows"));
				if (!bHasAtmosphereSunLight && !bHasAtmosphereSunLightIndex && !bHasCastShadowsOnClouds && !bHasCastCloudShadows)
				{
					OutError = TEXT("No directional atmosphere settings were provided.");
					return false;
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("atmosphere_sun_light"))) { Properties->SetBoolField(TEXT("bAtmosphereSunLight"), Arguments->GetBoolField(TEXT("atmosphere_sun_light"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("atmosphere_sun_light_index"))) { Properties->SetNumberField(TEXT("AtmosphereSunLightIndex"), Arguments->GetIntegerField(TEXT("atmosphere_sun_light_index"))); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("cast_shadows_on_clouds"))) { Properties->SetBoolField(TEXT("bCastShadowsOnClouds"), Arguments->GetBoolField(TEXT("cast_shadows_on_clouds"))); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("cast_cloud_shadows"))) { Properties->SetBoolField(TEXT("bCastCloudShadows"), Arguments->GetBoolField(TEXT("cast_cloud_shadows"))); }
				if (!Context.Services.ApplyProperties(Light, Properties, OutError))
				{
					return false;
				}
				if (bHasAtmosphereSunLight && !VerifyBoolPropertyApplied(Light, TEXT("bAtmosphereSunLight"), Arguments->GetBoolField(TEXT("atmosphere_sun_light")), OutError)) { return false; }
				if (bHasAtmosphereSunLightIndex && !VerifyNumericPropertyApplied(Light, TEXT("AtmosphereSunLightIndex"), Arguments->GetIntegerField(TEXT("atmosphere_sun_light_index")), 0.001, OutError)) { return false; }
				if (bHasCastShadowsOnClouds && !VerifyBoolPropertyApplied(Light, TEXT("bCastShadowsOnClouds"), Arguments->GetBoolField(TEXT("cast_shadows_on_clouds")), OutError)) { return false; }
				if (bHasCastCloudShadows && !VerifyBoolPropertyApplied(Light, TEXT("bCastCloudShadows"), Arguments->GetBoolField(TEXT("cast_cloud_shadows")), OutError)) { return false; }
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Light);
				OutSummary = TEXT("Updated directional light atmosphere settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_projection"),
			TEXT("Set generic camera component projection settings."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("projection_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("field_of_view"), FSololmcpSchemaBuilder::Number()}, {TEXT("ortho_width"), FSololmcpSchemaBuilder::Number()}, {TEXT("aspect_ratio"), FSololmcpSchemaBuilder::Number()}, {TEXT("constrain_aspect_ratio"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UCameraComponent* CameraComponent = ResolveCameraComponent(Context.Services, ActorId, OutError);
				if (!CameraComponent)
				{
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CameraSetProjection", "SOMOLMCP Set Camera Projection"));
				FString ProjectionMode;
				if (Arguments->TryGetStringField(TEXT("projection_mode"), ProjectionMode))
				{
					if (ProjectionMode == TEXT("orthographic")) { CameraComponent->ProjectionMode = ECameraProjectionMode::Orthographic; }
					else if (ProjectionMode == TEXT("perspective")) { CameraComponent->ProjectionMode = ECameraProjectionMode::Perspective; }
					else { OutError = TEXT("projection_mode must be perspective or orthographic."); return false; }
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("field_of_view"))) { CameraComponent->SetFieldOfView(Arguments->GetNumberField(TEXT("field_of_view"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("ortho_width"))) { CameraComponent->SetOrthoWidth(Arguments->GetNumberField(TEXT("ortho_width"))); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("aspect_ratio"))) { CameraComponent->SetAspectRatio(Arguments->GetNumberField(TEXT("aspect_ratio"))); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("constrain_aspect_ratio"))) { CameraComponent->SetConstraintAspectRatio(Arguments->GetBoolField(TEXT("constrain_aspect_ratio"))); }
				OutStructured = FSololmcpEditorServices::MakeObjectReference(CameraComponent);
				OutSummary = TEXT("Updated camera projection settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_post_process"),
			TEXT("Set post process blend weight or selected post process settings on a camera component."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("post_process_blend_weight"), FSololmcpSchemaBuilder::Number()}, {TEXT("post_process_settings"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				UCameraComponent* CameraComponent = ResolveCameraComponent(Context.Services, ActorId, OutError);
				if (!CameraComponent)
				{
					return false;
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("post_process_blend_weight")))
				{
					CameraComponent->PostProcessBlendWeight = Arguments->GetNumberField(TEXT("post_process_blend_weight"));
				}
				TSharedPtr<FJsonObject> PostProcessSettings;
				if (TryGetObjectField(Arguments, TEXT("post_process_settings"), PostProcessSettings))
				{
					TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
					Wrapper->SetObjectField(TEXT("PostProcessSettings"), PostProcessSettings.ToSharedRef());
					if (!Context.Services.ApplyProperties(CameraComponent, Wrapper, OutError))
					{
						return false;
					}
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(CameraComponent);
				OutSummary = TEXT("Updated camera post process settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_set_blendables"),
			TEXT("Set camera blendable assets through editor APIs."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("blendables"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}}, {TEXT("actor"), TEXT("blendables")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				const TArray<TSharedPtr<FJsonValue>>* Blendables = nullptr;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetArrayField(TEXT("blendables"), Blendables) || !Blendables)
				{
					OutError = TEXT("Missing actor or blendables.");
					return false;
				}

				UCameraComponent* CameraComponent = ResolveCameraComponent(Context.Services, ActorId, OutError);
				if (!CameraComponent)
				{
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CameraSetBlendables", "SOMOLMCP Set Camera Blendables"));
				CameraComponent->Modify();
				CameraComponent->PostProcessSettings.WeightedBlendables.Array.Reset();

				TArray<TSharedPtr<FJsonValue>> BlendableJson;
				for (const TSharedPtr<FJsonValue>& BlendableValue : *Blendables)
				{
					if (!BlendableValue.IsValid() || BlendableValue->Type != EJson::Object)
					{
						OutError = TEXT("Each blendable entry must be an object.");
						return false;
					}

					const TSharedPtr<FJsonObject> BlendableObject = BlendableValue->AsObject();
					if (!BlendableObject.IsValid())
					{
						OutError = TEXT("Invalid blendable object.");
						return false;
					}

					FString AssetPath;
					if (!BlendableObject->TryGetStringField(TEXT("asset_path"), AssetPath))
					{
						BlendableObject->TryGetStringField(TEXT("object_path"), AssetPath);
					}
					if (AssetPath.IsEmpty())
					{
						OutError = TEXT("Each blendable entry must include asset_path.");
						return false;
					}

					const double WeightValue = BlendableObject->HasTypedField<EJson::Number>(TEXT("weight"))
						? BlendableObject->GetNumberField(TEXT("weight"))
						: 1.0;
					UObject* BlendableAsset = Context.Services.LoadAsset(AssetPath, OutError);
					if (!BlendableAsset)
					{
						return false;
					}
					IBlendableInterface* BlendableInterface = Cast<IBlendableInterface>(BlendableAsset);
					if (!BlendableInterface)
					{
						OutError = TEXT("Blendable asset does not implement BlendableInterface.");
						return false;
					}

					TScriptInterface<IBlendableInterface> BlendableRef;
					BlendableRef.SetObject(BlendableAsset);
					BlendableRef.SetInterface(BlendableInterface);
					CameraComponent->AddOrUpdateBlendable(BlendableRef, static_cast<float>(WeightValue));

					TSharedRef<FJsonObject> BlendableResult = MakeShared<FJsonObject>();
					BlendableResult->SetObjectField(TEXT("asset"), FSololmcpEditorServices::MakeObjectReference(BlendableAsset));
					BlendableResult->SetNumberField(TEXT("weight"), WeightValue);
					BlendableJson.Add(MakeShared<FJsonValueObject>(BlendableResult));
				}

				OutStructured = FSololmcpEditorServices::MakeObjectReference(CameraComponent);
				OutStructured->SetArrayField(TEXT("blendables"), BlendableJson);
				OutStructured->SetNumberField(TEXT("count"), BlendableJson.Num());
				OutSummary = TEXT("Updated camera blendables.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_rig_create"),
			TEXT("Create a camera rig actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("rig_type"), FSololmcpSchemaBuilder::String()}, {TEXT("location"), VectorSchema()}, {TEXT("rotation"), RotatorSchema()}}, {TEXT("rig_type")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString RigType;
				if (!Arguments->TryGetStringField(TEXT("rig_type"), RigType))
				{
					OutError = TEXT("Missing rig_type.");
					return false;
				}
				UClass* RigClass = nullptr;
				if (RigType == TEXT("rail")) { RigClass = ACameraRig_Rail::StaticClass(); }
				else if (RigType == TEXT("crane")) { RigClass = ACameraRig_Crane::StaticClass(); }
				else { OutError = TEXT("rig_type must be rail or crane."); return false; }
				TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
				SpawnArgs->SetStringField(TEXT("class_path"), RigClass->GetPathName());
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("location"))) { SpawnArgs->SetField(TEXT("location"), *Value); }
				if (const TSharedPtr<FJsonValue>* Value = Arguments->Values.Find(TEXT("rotation"))) { SpawnArgs->SetField(TEXT("rotation"), *Value); }
				return Registry.ExecuteTool(TEXT("actor_spawn"), SpawnArgs, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_rig_set_rail"),
			TEXT("Set common camera rail rig settings."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("current_position_on_rail"), FSololmcpSchemaBuilder::Number()}, {TEXT("lock_orientation_to_rail"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				ACameraRig_Rail* Rail = Cast<ACameraRig_Rail>(Context.Services.FindActorByLabelOrName(ActorId, OutError));
				if (!Rail)
				{
					OutError = TEXT("Actor is not a camera rail rig.");
					return false;
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("current_position_on_rail"))) { Rail->CurrentPositionOnRail = Arguments->GetNumberField(TEXT("current_position_on_rail")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("lock_orientation_to_rail"))) { Rail->bLockOrientationToRail = Arguments->GetBoolField(TEXT("lock_orientation_to_rail")); }
				OutStructured = FSololmcpEditorServices::MakeActorReference(Rail);
				OutSummary = TEXT("Updated camera rail rig settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_rig_set_crane"),
			TEXT("Set common camera crane rig settings."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("crane_pitch"), FSololmcpSchemaBuilder::Number()}, {TEXT("crane_yaw"), FSololmcpSchemaBuilder::Number()}, {TEXT("crane_arm_length"), FSololmcpSchemaBuilder::Number()}, {TEXT("lock_mount_pitch"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("lock_mount_yaw"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId))
				{
					OutError = TEXT("Missing actor.");
					return false;
				}
				ACameraRig_Crane* Crane = Cast<ACameraRig_Crane>(Context.Services.FindActorByLabelOrName(ActorId, OutError));
				if (!Crane)
				{
					OutError = TEXT("Actor is not a camera crane rig.");
					return false;
				}
				if (Arguments->HasTypedField<EJson::Number>(TEXT("crane_pitch"))) { Crane->CranePitch = Arguments->GetNumberField(TEXT("crane_pitch")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("crane_yaw"))) { Crane->CraneYaw = Arguments->GetNumberField(TEXT("crane_yaw")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("crane_arm_length"))) { Crane->CraneArmLength = Arguments->GetNumberField(TEXT("crane_arm_length")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("lock_mount_pitch"))) { Crane->bLockMountPitch = Arguments->GetBoolField(TEXT("lock_mount_pitch")); }
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("lock_mount_yaw"))) { Crane->bLockMountYaw = Arguments->GetBoolField(TEXT("lock_mount_yaw")); }
				OutStructured = FSololmcpEditorServices::MakeActorReference(Crane);
				OutSummary = TEXT("Updated camera crane rig settings.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("camera_attach_to_rig"),
			TEXT("Attach a camera actor to a rig actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("parent"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor"), TEXT("parent")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				return Registry.ExecuteTool(TEXT("actor_attach"), Arguments, OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_create"),
			TEXT("Create a landscape actor in the editor world. Returns the spawned actor reference directly."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("location"), VectorSchema()},
					{TEXT("rotation"), RotatorSchema()},
					{TEXT("properties"), FSololmcpSchemaBuilder::Object({})},
					{TEXT("edit_layer_name"), FSololmcpSchemaBuilder::String()}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				// Audit round 6: refuse empty-arg calls so smoke fuzz doesn't silently spawn Landscape_0
				// (and the auto-created LandscapeGizmoActiveActor_0) at world origin. Caller must
				// explicitly provide at least location/properties/edit_layer_name to express intent.
				if (!Arguments->HasField(TEXT("location"))
					&& !Arguments->HasField(TEXT("rotation"))
					&& !Arguments->HasField(TEXT("properties"))
					&& !Arguments->HasField(TEXT("edit_layer_name")))
				{
					OutError = TEXT("Missing required: at least one of location, rotation, properties, edit_layer_name must be specified to create a landscape.");
					return false;
				}

				FString SubsystemError;
				UEditorActorSubsystem* ActorSubsystem = Context.Services.GetActorSubsystem(SubsystemError);
				if (!ActorSubsystem)
				{
					OutError = SubsystemError;
					return false;
				}

				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				if (TSharedPtr<FJsonObject> LocObj; TryGetObjectField(Arguments, TEXT("location"), LocObj))
				{
					FSololmcpEditorServices::JsonToVector(LocObj, Location);
				}
				if (TSharedPtr<FJsonObject> RotObj; TryGetObjectField(Arguments, TEXT("rotation"), RotObj))
				{
					FSololmcpEditorServices::JsonToRotator(RotObj, Rotation);
				}

				TerrainModeGuard::FSelectionScope ModeGuard;
				if (!ModeGuard.Begin(OutError))
				{
					ModeGuard.Attach(OutStructured);
					return false;
				}
				ModeGuard.Attach(OutStructured);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeCreate", "SOMOLMCP Create Landscape"));
				AActor* Actor = ActorSubsystem->SpawnActorFromClass(ALandscape::StaticClass(), Location, Rotation, false);
				if (!Actor)
				{
					OutError = TEXT("Failed to spawn landscape actor.");
					return false;
				}

				// Audit round 7 (silent-landscape-create fix): when the editor world cannot
				// materialize a real ALandscape (no heightmap, no SpawnInfo) UE silently swaps
				// in an ALandscapePlaceholder. The previous handler returned ok=true with the
				// placeholder, so the caller's actor list never grew a real Landscape entry.
				// Detect that case and surface a non-silent error directing callers to the
				// data-bearing path (terrain_landscape_create_from_spec); the placeholder
				// remains the implicit fallback only when callers omit terrain data entirely
				// (which round 6 already refuses on empty args).
				if (!Actor->IsA(ALandscape::StaticClass()) || Actor->GetClass()->GetName().Contains(TEXT("LandscapePlaceholder")))
				{
					ActorSubsystem->DestroyActor(Actor);
					OutError = TEXT("landscape_create produced an ALandscapePlaceholder (no heightmap data). Use terrain_landscape_create_from_spec with a terrain_spec to spawn a real ALandscape, or landscape_import to bind heightmap data.");
					return false;
				}

				// Audit round 7 (silent-landscape-create fix): re-apply and verify the spawn
				// transform so callers cannot receive ok=true at world origin.
				Actor->SetActorLocationAndRotation(Location, Rotation, /*bSweep=*/false, /*OutSweepHitResult=*/nullptr, ETeleportType::TeleportPhysics);
				{
					const FVector ActualLoc = Actor->GetActorLocation();
					const FRotator ActualRot = Actor->GetActorRotation();
					if (!ActualLoc.Equals(Location, 0.5f) || !ActualRot.Equals(Rotation, 0.1f))
					{
						OutError = FString::Printf(TEXT("Landscape spawned but transform did not apply: requested location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f), actual location=(%.2f,%.2f,%.2f) rotation=(%.2f,%.2f,%.2f)."),
							Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll,
							ActualLoc.X, ActualLoc.Y, ActualLoc.Z, ActualRot.Pitch, ActualRot.Yaw, ActualRot.Roll);
						return false;
					}
				}

				// Apply optional properties
				TSharedPtr<FJsonObject> Properties;
				if (TryGetObjectField(Arguments, TEXT("properties"), Properties) && !Context.Services.ApplyProperties(Actor, Properties, OutError))
				{
					return false;
				}

				FString EditLayerName;
				if (Arguments->TryGetStringField(TEXT("edit_layer_name"), EditLayerName) && !EditLayerName.IsEmpty())
				{
					if (ALandscape* Landscape = Cast<ALandscape>(Actor))
					{
						Landscape->CreateLayer(*EditLayerName);
					}
				}

				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				ModeGuard.Attach(OutStructured);
				OutSummary = TEXT("Created landscape actor.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("terrain_landscape_create_from_spec"),
			TEXT("Create a fully-functional landscape actor from a TerrainSpec. Uses UE's proper Landscape::Import() API to create heightmap data, components, and collision. Supports terrain_spec with world_bounds, cm_per_quad. landscape_material_instance_path is optional — if omitted the landscape is created without a custom material."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("terrain_spec"), FSololmcpSchemaBuilder::Object({})},
					{TEXT("edit_layer_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("allow_large_heightmap"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicit operator override for heightmaps over 8192 vertices in either dimension."))}
				},
				{TEXT("terrain_spec")}
			),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
				if (!Arguments->TryGetObjectField(TEXT("terrain_spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
				{
					OutError = TEXT("Missing terrain_spec object.");
					return false;
				}
				const TSharedRef<FJsonObject> Spec = SpecPtr->ToSharedRef();

				FVector WorldMin;
				FVector WorldMax;
				if (!TryGetTerrainWorldBoundsCm(Spec, WorldMin, WorldMax, OutError))
				{
					return false;
				}

				const double CmPerQuad = Spec->GetNumberField(TEXT("cm_per_quad"));
				if (CmPerQuad <= KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("terrain_spec.cm_per_quad must be positive.");
					return false;
				}

				FString LandscapeMaterialInstancePath;
				Spec->TryGetStringField(TEXT("landscape_material_instance_path"), LandscapeMaterialInstancePath);

				// --- Calculate landscape grid parameters ---
				const double WorldSizeX = WorldMax.X - WorldMin.X;
				const double WorldSizeY = WorldMax.Y - WorldMin.Y;
				const int32 NumQuadsX = FMath::Max(1, FMath::RoundToInt(WorldSizeX / CmPerQuad));
				const int32 NumQuadsY = FMath::Max(1, FMath::RoundToInt(WorldSizeY / CmPerQuad));

				// UE Landscape requires: ComponentSizeQuads = NumSubsections * SubsectionSizeQuads
				// SubsectionSizeQuads must be power of 2 minus 1 (e.g. 7, 15, 31, 63)
				// Default: 1 subsection per component, SubsectionSizeQuads=63 → ComponentSizeQuads=63
				const int32 SubsectionSizeQuads = 63;  // 64-1, standard value
				const int32 NumSubsections = 1;
				const int32 ComponentSizeQuads = NumSubsections * SubsectionSizeQuads; // 63

				const int32 ComponentCountX = FMath::Max(1, FMath::CeilToInt(static_cast<double>(NumQuadsX) / ComponentSizeQuads));
				const int32 ComponentCountY = FMath::Max(1, FMath::CeilToInt(static_cast<double>(NumQuadsY) / ComponentSizeQuads));

				// Actual heightmap size (vertices = quads+1 per component boundary)
				const int32 SizeX = ComponentCountX * ComponentSizeQuads + 1;
				const int32 SizeY = ComponentCountY * ComponentSizeQuads + 1;

				const bool bAllowLargeHeightmap = Arguments->HasTypedField<EJson::Boolean>(TEXT("allow_large_heightmap"))
					? Arguments->GetBoolField(TEXT("allow_large_heightmap")) : false;
				OutStructured->SetNumberField(TEXT("heightmap_size_x"), SizeX);
				OutStructured->SetNumberField(TEXT("heightmap_size_y"), SizeY);
				OutStructured->SetBoolField(TEXT("heightmap_cap_passed"), SizeX <= 8192 && SizeY <= 8192);
				OutStructured->SetBoolField(TEXT("allow_large_heightmap"), bAllowLargeHeightmap);
				if (SizeX > 8192 || SizeY > 8192)
				{
					if (!bAllowLargeHeightmap)
					{
						OutStructured->SetStringField(TEXT("status"), TEXT("blocked_large_heightmap"));
						OutError = FString::Printf(TEXT("terrain_landscape_create_from_spec refused large heightmap %dx%d (%d MB). Split into tiles or pass allow_large_heightmap=true with an operator-approved plan."), SizeX, SizeY, SizeX * SizeY * 2 / 1024 / 1024);
						return false;
					}
					UE_LOG(LogSOMOLMCP, Warning, TEXT("terrain_landscape_create_from_spec: operator override for large heightmap %dx%d (%d MB)."), SizeX, SizeY, SizeX * SizeY * 2 / 1024 / 1024);
				}

				// Load material if provided (optional — landscape works without custom material)
				UMaterialInterface* Material = nullptr;
				if (!LandscapeMaterialInstancePath.IsEmpty())
				{
					Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(LandscapeMaterialInstancePath, OutError));
					if (!Material)
					{
						return false;
					}
				}

				// --- Spawn landscape actor using UE's proper API ---
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}

		// Offset to center the landscape at the spec's center
		// Z position: spawn at WorldMin.Z so height data maps directly to world Z
		const FVector LandscapeCenter((WorldMin.X + WorldMax.X) / 2.0, (WorldMin.Y + WorldMax.Y) / 2.0, WorldMin.Z);
		const FVector Offset = FVector(-ComponentCountX * ComponentSizeQuads / 2.0 * CmPerQuad, -ComponentCountY * ComponentSizeQuads / 2.0 * CmPerQuad, 0.0);
		const FVector SpawnLocation = LandscapeCenter + Offset;
		const FRotator SpawnRotation(0.f, 0.f, 0.f);
		
		// Calculate Z scale: UE Landscape internally converts uint16 via LANDSCAPE_ZSCALE (1/128).
		// Effective local height range = 65535 * (1/128) ≈ 511.99 cm.
		// So Actor Z Scale must be ZRange / 511.99 to map to desired world range.
		const double ZRange = WorldMax.Z - WorldMin.Z;
		constexpr double LandscapeLocalRange = 65535.0 * (1.0 / 128.0); // ≈511.99 cm
		const float ZScale = static_cast<float>(ZRange / LandscapeLocalRange);
		const FVector LandscapeScale(static_cast<float>(CmPerQuad), static_cast<float>(CmPerQuad), ZScale);

			TerrainModeGuard::FSelectionScope ModeGuard;
			if (!ModeGuard.Begin(OutError))
			{
				ModeGuard.Attach(OutStructured);
				return false;
			}
			ModeGuard.Attach(OutStructured);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "TerrainCreateFromSpec", "SOMOLMCP Create Landscape from Spec"));

			// Use World->SpawnActor<ALandscape> (same as UE's own New Landscape dialog)
			ALandscape* Landscape = World->SpawnActor<ALandscape>(SpawnLocation, SpawnRotation);
			if (!Landscape)
			{
				OutError = TEXT("Failed to spawn ALandscape actor.");
				return false;
			}

			// Set material before Import
			if (Material)
			{
				Landscape->LandscapeMaterial = Material;
			}
			
			// Set full scale BEFORE Import (including Z scale)
			// This ensures the heightmap data gets properly scaled during import
			UE_LOG(LogSOMOLMCP, Display, TEXT("terrain_landscape_create_from_spec: ZRange=%.0f, ZScale=%.4f, Scale=(%.2f, %.2f, %.4f)"), 
				ZRange, ZScale, LandscapeScale.X, LandscapeScale.Y, LandscapeScale.Z);
			Landscape->SetActorScale3D(LandscapeScale);

			// Auto lighting LOD
			Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((SizeX * SizeY) / (2048 * 2048) + 1), (uint32)2);

		// --- Generate procedural heightmap with mountains and valleys ---
		// HeightData: 0-65535 range, ZScale maps this to WorldMin.Z~WorldMax.Z
		// Example: -4000m to +8000m = 12000m range, ZScale ~= 18.3
		TArray<uint16> HeightData;
		HeightData.SetNumZeroed(SizeX * SizeY);
		
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				const float FX = static_cast<float>(X) / SizeX;
				const float FY = static_cast<float>(Y) / SizeY;
				
				// Generate normalized height (0.0 ~ 1.0)
				// 0.0 = trench (-4000m), 0.33 = sea level (0m), 1.0 = peak (+8000m)
				float HeightNorm = 0.33f; // Start at sea level
				
				// === Tectonic features ===
				// Major mountain ranges (Himalaya-like, push high)
				const float MountainRidge = FMath::Sin(FX * PI * 1.5f) * FMath::Sin(FY * PI * 0.8f);
				if (MountainRidge > 0.3f)
				{
					HeightNorm += MountainRidge * 0.6f; // Sharp peaks up to ~0.9
				}
				
				// Secondary mountain chains
				HeightNorm += FMath::Sin(FX * PI * 3.f + FY * PI * 2.f) * 0.12f;
				
				// === Canyon systems ===
				// Deep canyons (Grand Canyon style)
				const float CanyonLine = FMath::Abs(FMath::Sin(FX * PI * 2.f - FY * PI * 0.5f));
				if (CanyonLine < 0.15f)
				{
					HeightNorm -= (0.15f - CanyonLine) * 2.f; // Carve down to ~0.1
				}
				
				// === Ocean trenches ===
				// Subduction zones (Mariana Trench style, very deep)
				const float TrenchLine = FMath::Abs(FMath::Sin(FY * PI * 4.f));
				if (TrenchLine < 0.08f && FX < 0.3f)
				{
					HeightNorm -= (0.08f - TrenchLine) * 4.f; // Deep trench down to ~0.05
				}
				
				// === Fine terrain detail ===
				// Erosion patterns
				HeightNorm += FMath::Sin(FX * PI * 13.f) * FMath::Cos(FY * PI * 11.f) * 0.02f;
				
				// Small hills and valleys
				HeightNorm += FMath::Sin((FX + FY) * PI * 17.f) * 0.015f;
				HeightNorm += FMath::Sin(FX * PI * 23.f + FY * PI * 19.f) * 0.01f;
				
				// Clamp to valid range
				HeightNorm = FMath::Clamp(HeightNorm, 0.f, 1.f);
				
				// Convert to uint16
				HeightData[Y * SizeX + X] = static_cast<uint16>(FMath::RoundToInt(HeightNorm * 65535.f));
			}
		}

				// Prepare height data map for Import
				TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
				HeightDataPerLayers.Add(FGuid(), MoveTemp(HeightData));

				TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
				MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

			// Call Import to create the actual landscape components, heightmap textures, collision
			Landscape->Import(
				FGuid::NewGuid(),
				0, 0,
				SizeX - 1, SizeY - 1,
				NumSubsections,
				SubsectionSizeQuads,
				HeightDataPerLayers,
				TEXT(""),  // ReimportHeightmapFilePath (empty = no file)
				MaterialLayerDataPerLayers,
				ELandscapeImportAlphamapType::Additive,
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				TArrayView<const FLandscapeLayer>()
#else
				// 5.4 takes a pointer to the layer array rather than a view. Both branches
				// say "no edit layers"; only the way of spelling it changed.
				nullptr
#endif
			);

			// Set actor location to WorldMin.Z (scale pivot is at origin)
			FVector AdjustedLocation = SpawnLocation;
			AdjustedLocation.Z = WorldMin.Z;
			Landscape->SetActorLocation(AdjustedLocation, false);

			// Set unique label
			FActorLabelUtilities::SetActorLabelUnique(Landscape, TEXT("Landscape"));

				// Update landscape info
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (LandscapeInfo)
				{
					LandscapeInfo->UpdateLayerInfoMap(Landscape);
				}

				// Apply edit layer if requested
				FString EditLayerName;
				if (Arguments->TryGetStringField(TEXT("edit_layer_name"), EditLayerName) && !EditLayerName.IsEmpty())
				{
					Landscape->CreateLayer(*EditLayerName);
				}

				OutStructured = FSololmcpEditorServices::MakeActorReference(Landscape);
				ModeGuard.Attach(OutStructured);
				OutStructured->SetNumberField(TEXT("size_x"), SizeX);
				OutStructured->SetNumberField(TEXT("size_y"), SizeY);
				OutStructured->SetNumberField(TEXT("component_count_x"), ComponentCountX);
				OutStructured->SetNumberField(TEXT("component_count_y"), ComponentCountY);
				OutStructured->SetNumberField(TEXT("component_size_quads"), ComponentSizeQuads);
				OutStructured->SetNumberField(TEXT("cm_per_quad"), CmPerQuad);
				OutStructured->SetNumberField(TEXT("num_quads_x"), NumQuadsX);
				OutStructured->SetNumberField(TEXT("num_quads_y"), NumQuadsY);

				OutSummary = FString::Printf(TEXT("Created landscape: %dx%d vertices, %dx%d components, %d quads/component, %.0f cm/quad, with procedural height data"),
					SizeX, SizeY, ComponentCountX, ComponentCountY, ComponentSizeQuads, CmPerQuad);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("terrain_spec_validate"),
			TEXT("Validate a TerrainSpec object (bounds, World Partition cell size, optional overlap / landscape hints) for large-world PCG + streaming."),
			FSololmcpSchemaBuilder::Object({{TEXT("terrain_spec"), FSololmcpSchemaBuilder::Object({})}}, {TEXT("terrain_spec")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!ValidateTerrainSpecJson(Arguments, OutStructured, OutError))
				{
					return false;
				}
				const bool bValid = OutStructured->GetBoolField(TEXT("valid"));
				OutSummary = bValid ? TEXT("TerrainSpec validation passed.") : TEXT("TerrainSpec validation reported issues.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("terrain_tile_plan"),
			TEXT("Build a TilePlan: XY grid tiles in cm aligned to world_partition_cell_size_cm (optional tile_overlap_cm for seam blending)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("world_min_cm"), VectorSchema()},
					{TEXT("world_max_cm"), VectorSchema()},
					{TEXT("world_partition_cell_size_cm"), FSololmcpSchemaBuilder::Number()},
					{TEXT("tile_overlap_cm"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("world_min_cm"), TEXT("world_max_cm"), TEXT("world_partition_cell_size_cm")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!BuildTerrainTilePlanJson(Arguments, OutStructured, OutError))
				{
					return false;
				}
				OutSummary = FString::Printf(TEXT("Generated %d terrain tiles."), OutStructured->GetIntegerField(TEXT("tile_count")));
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_world_box_to_heightmap"),
			TEXT("Convert a world-space XY bounds box (cm) to inclusive landscape heightmap indices for a landscape actor (uses LandscapeActorToWorld + scale)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape"), FSololmcpSchemaBuilder::String()},
					{TEXT("world_min_cm"), VectorSchema()},
					{TEXT("world_max_cm"), VectorSchema()}
				},
				{TEXT("landscape"), TEXT("world_min_cm"), TEXT("world_max_cm")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				TSharedPtr<FJsonObject> MinObj;
				TSharedPtr<FJsonObject> MaxObj;
				if (!TryGetObjectField(Arguments, TEXT("world_min_cm"), MinObj) || !TryGetObjectField(Arguments, TEXT("world_max_cm"), MaxObj))
				{
					OutError = TEXT("Missing world_min_cm or world_max_cm.");
					return false;
				}
				FVector WorldMin;
				FVector WorldMax;
				if (!FSololmcpEditorServices::JsonToVector(MinObj, WorldMin) || !FSololmcpEditorServices::JsonToVector(MaxObj, WorldMax))
				{
					OutError = TEXT("world_min_cm / world_max_cm must be vector objects.");
					return false;
				}
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!TryWorldBoxToLandscapeHeightmapIndices(Landscape, WorldMin, WorldMax, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetStringField(TEXT("landscape"), LandscapeId);
				OutSummary = TEXT("Mapped world box to heightmap region.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("terrain_fill_tile_plan_landscape_noise"),
			TEXT("Batch: same grid as terrain_tile_plan — for each tile map world bounds to heightmap and apply Perlin noise (hydrology/large-world fill step). Optional dry_run."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape"), FSololmcpSchemaBuilder::String()},
					{TEXT("world_min_cm"), VectorSchema()},
					{TEXT("world_max_cm"), VectorSchema()},
					{TEXT("world_partition_cell_size_cm"), FSololmcpSchemaBuilder::Number()},
					{TEXT("tile_overlap_cm"), FSololmcpSchemaBuilder::Number()},
					{TEXT("use_core_cells_only"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("blend_alpha"), FSololmcpSchemaBuilder::Number()},
					{TEXT("seed"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("frequency"), FSololmcpSchemaBuilder::Number()},
					{TEXT("amplitude"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("landscape"), TEXT("world_min_cm"), TEXT("world_max_cm"), TEXT("world_partition_cell_size_cm")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}

				FVector WorldMin;
				FVector WorldMax;
				if (!TryGetTerrainWorldBoundsCm(Arguments, WorldMin, WorldMax, OutError))
				{
					return false;
				}

				double CellSizeCm = Arguments->GetNumberField(TEXT("world_partition_cell_size_cm"));
				if (CellSizeCm <= KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("world_partition_cell_size_cm must be positive.");
					return false;
				}
				double OverlapCm = 0.0;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("tile_overlap_cm")))
				{
					OverlapCm = Arguments->GetNumberField(TEXT("tile_overlap_cm"));
				}
				if (OverlapCm < 0.0 || OverlapCm >= CellSizeCm * 0.5 - KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("tile_overlap_cm must be >= 0 and < half of world_partition_cell_size_cm.");
					return false;
				}

				const FVector Lo(
					FMath::Min(WorldMin.X, WorldMax.X),
					FMath::Min(WorldMin.Y, WorldMax.Y),
					FMath::Min(WorldMin.Z, WorldMax.Z));
				const FVector Hi(
					FMath::Max(WorldMin.X, WorldMax.X),
					FMath::Max(WorldMin.Y, WorldMax.Y),
					FMath::Max(WorldMin.Z, WorldMax.Z));

				const double S = CellSizeCm;
				const int32 IxLo = FMath::FloorToInt(Lo.X / S);
				const int32 IxHi = FMath::CeilToInt(Hi.X / S) - 1;
				const int32 IyLo = FMath::FloorToInt(Lo.Y / S);
				const int32 IyHi = FMath::CeilToInt(Hi.Y / S) - 1;
				if (IxLo > IxHi || IyLo > IyHi)
				{
					OutError = TEXT("World XY bounds do not intersect any grid cells.");
					return false;
				}

				const bool bUseCoreOnly = Arguments->HasTypedField<EJson::Boolean>(TEXT("use_core_cells_only")) && Arguments->GetBoolField(TEXT("use_core_cells_only"));
				const bool bDryRun = Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")) && Arguments->GetBoolField(TEXT("dry_run"));
				const float BlendAlpha = Arguments->HasTypedField<EJson::Number>(TEXT("blend_alpha")) ? static_cast<float>(Arguments->GetNumberField(TEXT("blend_alpha"))) : 1.0f;
				const int32 SeedBase = Arguments->HasTypedField<EJson::Number>(TEXT("seed")) ? Arguments->GetIntegerField(TEXT("seed")) : 1;
				const float Frequency = Arguments->HasTypedField<EJson::Number>(TEXT("frequency")) ? static_cast<float>(Arguments->GetNumberField(TEXT("frequency"))) : 0.05f;
				const float Amplitude = Arguments->HasTypedField<EJson::Number>(TEXT("amplitude")) ? static_cast<float>(Arguments->GetNumberField(TEXT("amplitude"))) : 0.35f;

				TArray<TSharedPtr<FJsonValue>> TilesOut;
				int32 AppliedCount = 0;
				int32 SkippedCount = 0;

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "TerrainFillTilePlanNoise", "SOMOLMCP Terrain Tile Plan Noise"));
				if (!bDryRun)
				{
					Landscape->Modify();
				}
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);

				for (int32 Iy = IyLo; Iy <= IyHi; ++Iy)
				{
					for (int32 Ix = IxLo; Ix <= IxHi; ++Ix)
					{
						float BMinX = static_cast<float>(Ix * S - OverlapCm);
						float BMaxX = static_cast<float>((Ix + 1) * S + OverlapCm);
						float BMinY = static_cast<float>(Iy * S - OverlapCm);
						float BMaxY = static_cast<float>((Iy + 1) * S + OverlapCm);
						if (bUseCoreOnly)
						{
							BMinX = static_cast<float>(Ix * S);
							BMaxX = static_cast<float>((Ix + 1) * S);
							BMinY = static_cast<float>(Iy * S);
							BMaxY = static_cast<float>((Iy + 1) * S);
						}

						const FVector TileWorldMin(BMinX, BMinY, Lo.Z);
						const FVector TileWorldMax(BMaxX, BMaxY, Hi.Z);

						int32 HMinX = 0, HMinY = 0, HMaxX = 0, HMaxY = 0;
						FString TileErr;
						if (!TryWorldBoxToLandscapeHeightmapIndices(Landscape, TileWorldMin, TileWorldMax, HMinX, HMinY, HMaxX, HMaxY, TileErr))
						{
							TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetStringField(TEXT("id"), FString::Printf(TEXT("%d_%d"), Ix, Iy));
							Entry->SetBoolField(TEXT("applied"), false);
							Entry->SetStringField(TEXT("error"), TileErr);
							TilesOut.Add(MakeShared<FJsonValueObject>(Entry));
							++SkippedCount;
							continue;
						}

						const int32 TileSeed = SeedBase + Ix * 9176 + Iy * 131071;
						TArray<uint16> HeightData;
						BuildPerlinNoiseHeightBuffer(HMinX, HMinY, HMaxX, HMaxY, TileSeed, Frequency, Amplitude, HeightData);
						if (!bDryRun && BlendAlpha < 0.999f)
						{
							TArray<uint16> BaseData;
							BaseData.SetNumZeroed(HeightData.Num());
							LandscapeEdit.GetHeightDataFast(HMinX, HMinY, HMaxX, HMaxY, BaseData.GetData(), 0);
							const float Alpha = FMath::Clamp(BlendAlpha, 0.f, 1.f);
							const float InvAlpha = 1.f - Alpha;
							for (int32 I = 0; I < HeightData.Num(); ++I)
							{
								const float Mixed = BaseData[I] * InvAlpha + HeightData[I] * Alpha;
								HeightData[I] = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(Mixed), 0, 65535));
							}
						}

						if (!bDryRun)
						{
							LandscapeEdit.SetHeightData(HMinX, HMinY, HMaxX, HMaxY, HeightData.GetData(), 0, true);
						}

						TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("id"), FString::Printf(TEXT("%d_%d"), Ix, Iy));
						Entry->SetBoolField(TEXT("applied"), true);
						Entry->SetNumberField(TEXT("min_x"), HMinX);
						Entry->SetNumberField(TEXT("min_y"), HMinY);
						Entry->SetNumberField(TEXT("max_x"), HMaxX);
						Entry->SetNumberField(TEXT("max_y"), HMaxY);
						Entry->SetNumberField(TEXT("seed"), TileSeed);
						Entry->SetNumberField(TEXT("samples"), HeightData.Num());
						TilesOut.Add(MakeShared<FJsonValueObject>(Entry));
						++AppliedCount;
					}
				}

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("landscape"), LandscapeId);
				OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
				OutStructured->SetBoolField(TEXT("use_core_cells_only"), bUseCoreOnly);
				OutStructured->SetNumberField(TEXT("blend_alpha"), BlendAlpha);
				OutStructured->SetNumberField(TEXT("tiles_applied"), AppliedCount);
				OutStructured->SetNumberField(TEXT("tiles_skipped"), SkippedCount);
				OutStructured->SetArrayField(TEXT("tiles"), TilesOut);
				OutSummary = FString::Printf(TEXT("Terrain tile noise: %d applied, %d skipped (%s)."), AppliedCount, SkippedCount, bDryRun ? TEXT("dry run") : TEXT("written"));
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("terrain_fill_tile_plan_landscape_weights_height_based"),
			TEXT("Batch: same grid as terrain_tile_plan — per tile read heightmap region and write paint weight data (height-based, sum-to-255 normalization). Optional dry_run."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape"), FSololmcpSchemaBuilder::String()},
					{TEXT("world_min_cm"), VectorSchema()},
					{TEXT("world_max_cm"), VectorSchema()},
					{TEXT("world_partition_cell_size_cm"), FSololmcpSchemaBuilder::Number()},
					{TEXT("tile_overlap_cm"), FSololmcpSchemaBuilder::Number()},
					{TEXT("use_core_cells_only"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("paint_layers"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}
				},
				{TEXT("landscape"), TEXT("world_min_cm"), TEXT("world_max_cm"), TEXT("world_partition_cell_size_cm"), TEXT("tile_overlap_cm"), TEXT("paint_layers")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}

				const TArray<TSharedPtr<FJsonValue>>* PaintLayersJson = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("paint_layers"), PaintLayersJson) || !PaintLayersJson || PaintLayersJson->Num() <= 0)
				{
					OutError = TEXT("paint_layers must be a non-empty array of strings.");
					return false;
				}

				TArray<FString> PaintLayerNames;
				PaintLayerNames.Reserve(PaintLayersJson->Num());
				for (const TSharedPtr<FJsonValue>& V : *PaintLayersJson)
				{
					if (!V.IsValid() || V->Type != EJson::String)
					{
						OutError = TEXT("paint_layers[] must be strings.");
						return false;
					}
					PaintLayerNames.Add(V->AsString());
				}

				TArray<ULandscapeLayerInfoObject*> PaintLayers;
				PaintLayers.Reserve(PaintLayerNames.Num());
				for (const FString& NameOrPath : PaintLayerNames)
				{
					ULandscapeLayerInfoObject* LayerInfo = FindLandscapePaintLayerInfo(Landscape, NameOrPath);
					if (!LayerInfo)
					{
						OutError = TEXT("Landscape paint layer was not found: ") + NameOrPath;
						return false;
					}
					PaintLayers.Add(LayerInfo);
				}

				FVector WorldMin;
				FVector WorldMax;
				if (!TryGetTerrainWorldBoundsCm(Arguments, WorldMin, WorldMax, OutError))
				{
					return false;
				}

				const double CellSizeCm = Arguments->GetNumberField(TEXT("world_partition_cell_size_cm"));
				if (CellSizeCm <= KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("world_partition_cell_size_cm must be positive.");
					return false;
				}
				double OverlapCm = 0.0;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("tile_overlap_cm")))
				{
					OverlapCm = Arguments->GetNumberField(TEXT("tile_overlap_cm"));
				}
				if (OverlapCm < 0.0 || OverlapCm >= CellSizeCm * 0.5 - KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("tile_overlap_cm must be >= 0 and < half of world_partition_cell_size_cm.");
					return false;
				}

				const FVector Lo(
					FMath::Min(WorldMin.X, WorldMax.X),
					FMath::Min(WorldMin.Y, WorldMax.Y),
					FMath::Min(WorldMin.Z, WorldMax.Z));
				const FVector Hi(
					FMath::Max(WorldMin.X, WorldMax.X),
					FMath::Max(WorldMin.Y, WorldMax.Y),
					FMath::Max(WorldMin.Z, WorldMax.Z));

				const double S = CellSizeCm;
				const int32 IxLo = FMath::FloorToInt(Lo.X / S);
				const int32 IxHi = FMath::CeilToInt(Hi.X / S) - 1;
				const int32 IyLo = FMath::FloorToInt(Lo.Y / S);
				const int32 IyHi = FMath::CeilToInt(Hi.Y / S) - 1;
				if (IxLo > IxHi || IyLo > IyHi)
				{
					OutError = TEXT("World XY bounds do not intersect any grid cells.");
					return false;
				}

				const bool bUseCoreOnly = Arguments->HasTypedField<EJson::Boolean>(TEXT("use_core_cells_only")) && Arguments->GetBoolField(TEXT("use_core_cells_only"));
				const bool bDryRun = Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")) && Arguments->GetBoolField(TEXT("dry_run"));

				TArray<TSharedPtr<FJsonValue>> TilesOut;
				int32 AppliedCount = 0;
				int32 SkippedCount = 0;

				const int32 LayerCount = PaintLayers.Num();
				// Precompute centers for height normalization distribution (sum-to-255).
				TArray<float> Centers;
				Centers.SetNum(LayerCount);
				for (int32 i = 0; i < LayerCount; ++i)
				{
					Centers[i] = (static_cast<float>(i) + 0.5f) / static_cast<float>(LayerCount);
				}
				const float Bandwidth = LayerCount > 0 ? (1.0f / static_cast<float>(LayerCount)) : 1.0f;

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "TerrainFillTilePlanWeightsHeightBased", "SOMOLMCP Terrain Fill Tile Plan Weights (height-based)"));
				if (!bDryRun)
				{
					Landscape->Modify();
				}

				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);

				for (int32 Iy = IyLo; Iy <= IyHi; ++Iy)
				{
					for (int32 Ix = IxLo; Ix <= IxHi; ++Ix)
					{
						float BMinX = static_cast<float>(Ix * S - OverlapCm);
						float BMaxX = static_cast<float>((Ix + 1) * S + OverlapCm);
						float BMinY = static_cast<float>(Iy * S - OverlapCm);
						float BMaxY = static_cast<float>((Iy + 1) * S + OverlapCm);
						if (bUseCoreOnly)
						{
							BMinX = static_cast<float>(Ix * S);
							BMaxX = static_cast<float>((Ix + 1) * S);
							BMinY = static_cast<float>(Iy * S);
							BMaxY = static_cast<float>((Iy + 1) * S);
						}

						const FVector TileWorldMin(BMinX, BMinY, Lo.Z);
						const FVector TileWorldMax(BMaxX, BMaxY, Hi.Z);

						int32 HMinX = 0, HMinY = 0, HMaxX = 0, HMaxY = 0;
						FString TileErr;
						if (!TryWorldBoxToLandscapeHeightmapIndices(Landscape, TileWorldMin, TileWorldMax, HMinX, HMinY, HMaxX, HMaxY, TileErr))
						{
							TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetStringField(TEXT("id"), FString::Printf(TEXT("%d_%d"), Ix, Iy));
							Entry->SetBoolField(TEXT("applied"), false);
							Entry->SetStringField(TEXT("error"), TileErr);
							TilesOut.Add(MakeShared<FJsonValueObject>(Entry));
							++SkippedCount;
							continue;
						}

						const int32 Width = HMaxX - HMinX + 1;
						const int32 Height = HMaxY - HMinY + 1;
						const int32 Samples = Width * Height;
						if (Samples <= 0)
						{
							++SkippedCount;
							continue;
						}

						TArray<uint16> HeightData;
						HeightData.SetNumZeroed(Samples);
						LandscapeEdit.GetHeightDataFast(HMinX, HMinY, HMaxX, HMaxY, HeightData.GetData(), 0);

						// Compute normalized weights per layer for the whole region.
						TArray<TArray<uint8>> WeightDataByLayer;
						WeightDataByLayer.SetNum(LayerCount);
						for (int32 L = 0; L < LayerCount; ++L)
						{
							WeightDataByLayer[L].SetNumZeroed(Samples);
						}

						TArray<float> Tmp;
						Tmp.SetNumZeroed(LayerCount);

						for (int32 SIdx = 0; SIdx < Samples; ++SIdx)
						{
							const float T = static_cast<float>(HeightData[SIdx]) / 65535.f;

							float SumW = 0.f;
							for (int32 L = 0; L < LayerCount; ++L)
							{
								const float Dist = FMath::Abs(T - Centers[L]);
								const float Wi = FMath::Max(0.f, 1.f - (Dist / Bandwidth));
								Tmp[L] = Wi;
								SumW += Wi;
							}

							if (SumW <= KINDA_SMALL_NUMBER)
							{
								// Degenerate case: put all weight into first layer.
								for (int32 L = 0; L < LayerCount; ++L)
								{
									WeightDataByLayer[L][SIdx] = (L == 0) ? 255 : 0;
								}
							}
							else
							{
								for (int32 L = 0; L < LayerCount; ++L)
								{
									const float Normalized = Tmp[L] / SumW;
									WeightDataByLayer[L][SIdx] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Normalized * 255.f), 0, 255));
								}
							}
						}

						if (!bDryRun)
						{
							// Write each layer weight map into the same heightmap region.
							// UE 5.7: SetAlphaData now requires TSet for DirtyLayerInfos parameter
							for (int32 L = 0; L < LayerCount; ++L)
							{
								TSet<ULandscapeLayerInfoObject*> DirtyLayers;
								DirtyLayers.Add(PaintLayers[L]);
								LandscapeEdit.SetAlphaData(
									DirtyLayers,
									HMinX, HMinY, HMaxX, HMaxY,
									WeightDataByLayer[L].GetData(),
									WeightDataByLayer[L].Num(),
									ELandscapeLayerPaintingRestriction::None);
							}
						}

						TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("id"), FString::Printf(TEXT("%d_%d"), Ix, Iy));
						const bool bApplied = !bDryRun;
						Entry->SetBoolField(TEXT("applied"), bApplied);
						Entry->SetNumberField(TEXT("min_x"), HMinX);
						Entry->SetNumberField(TEXT("min_y"), HMinY);
						Entry->SetNumberField(TEXT("max_x"), HMaxX);
						Entry->SetNumberField(TEXT("max_y"), HMaxY);
						TilesOut.Add(MakeShared<FJsonValueObject>(Entry));
						if (bApplied)
						{
							++AppliedCount;
						}
					}
				}

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("landscape"), LandscapeId);
				OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
				OutStructured->SetBoolField(TEXT("use_core_cells_only"), bUseCoreOnly);
				OutStructured->SetNumberField(TEXT("tiles_applied"), AppliedCount);
				OutStructured->SetNumberField(TEXT("tiles_skipped"), SkippedCount);
				OutStructured->SetArrayField(TEXT("tiles"), TilesOut);
				OutSummary = FString::Printf(TEXT("Terrain tile weights(height-based): %d applied, %d skipped (%s)."), AppliedCount, SkippedCount, bDryRun ? TEXT("dry run") : TEXT("written"));
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("terrain_pcg_generate_tiles"),
			TEXT("Generate PCG per TerrainSpec tiles (A/B graph build + tile-wise WP load -> volume bounds -> pcg_generate -> WP unload)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("terrain_spec"), FSololmcpSchemaBuilder::Object({})},
					{TEXT("pcg_mode"), FSololmcpSchemaBuilder::String()},
					{TEXT("pcg_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("pcg_volume_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("generation_trigger"), FSololmcpSchemaBuilder::String()},
					{TEXT("input_type"), FSololmcpSchemaBuilder::String()},
					// B mode
					{TEXT("pcg_graph_asset_path"), FSololmcpSchemaBuilder::String()},
					// A mode (asset creation)
					{TEXT("pcg_graph_package_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("pcg_graph_asset_name"), FSololmcpSchemaBuilder::String()},
					// Controls whether to use core_cell bounds for the PCG volume.
					{TEXT("tile_use_core_cells_only"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("pcg_clear_between_tiles"), FSololmcpSchemaBuilder::Boolean()},
					// Optional explicit nodes spec override (otherwise read from terrain_spec.pcg_spec_driven_nodes)
					{TEXT("pcg_spec_driven_nodes"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}))}
				},
				{TEXT("terrain_spec"), TEXT("pcg_mode"), TEXT("pcg_actor"), TEXT("pcg_volume_actor"), TEXT("generation_trigger"), TEXT("input_type")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				auto MakeVecObjFieldFromSharedPtr = [](const TSharedPtr<FJsonObject>& Obj) -> TSharedPtr<FJsonObject>
				{
					return Obj;
				};

				const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
				if (!Arguments->TryGetObjectField(TEXT("terrain_spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
				{
					OutError = TEXT("Missing terrain_spec object.");
					return false;
				}
				const TSharedRef<FJsonObject> Spec = SpecPtr->ToSharedRef();

				FString PcgMode;
				if (!Arguments->TryGetStringField(TEXT("pcg_mode"), PcgMode) || PcgMode.IsEmpty())
				{
					// Also allow fallback from spec.
					Spec->TryGetStringField(TEXT("pcg_mode"), PcgMode);
				}
				PcgMode = PcgMode.ToUpper();
				if (PcgMode != TEXT("A") && PcgMode != TEXT("B"))
				{
					OutError = TEXT("pcg_mode must be \"A\" or \"B\".");
					return false;
				}

				FString PcgActorId;
				if (!Arguments->TryGetStringField(TEXT("pcg_actor"), PcgActorId) || PcgActorId.IsEmpty())
				{
					OutError = TEXT("Missing pcg_actor.");
					return false;
				}
				FString PcgVolumeActorId;
				if (!Arguments->TryGetStringField(TEXT("pcg_volume_actor"), PcgVolumeActorId) || PcgVolumeActorId.IsEmpty())
				{
					OutError = TEXT("Missing pcg_volume_actor.");
					return false;
				}

				FString GenerationTrigger;
				if (!Arguments->TryGetStringField(TEXT("generation_trigger"), GenerationTrigger) || GenerationTrigger.IsEmpty())
				{
					OutError = TEXT("Missing generation_trigger.");
					return false;
				}
				FString InputType;
				if (!Arguments->TryGetStringField(TEXT("input_type"), InputType) || InputType.IsEmpty())
				{
					OutError = TEXT("Missing input_type.");
					return false;
				}

				const bool bTileUseCoreOnly = Arguments->HasTypedField<EJson::Boolean>(TEXT("tile_use_core_cells_only")) ? Arguments->GetBoolField(TEXT("tile_use_core_cells_only")) : true;
				const bool bClearBetweenTiles = Arguments->HasTypedField<EJson::Boolean>(TEXT("pcg_clear_between_tiles")) ? Arguments->GetBoolField(TEXT("pcg_clear_between_tiles")) : true;

				// Build terrain_tile_plan by reusing terrain_spec fields.
				const TSharedPtr<FJsonObject>* WorldMinObjPtr = nullptr;
				const TSharedPtr<FJsonObject>* WorldMaxObjPtr = nullptr;
				double CellSizeCm = 0.0;
				if (!Spec->TryGetObjectField(TEXT("world_min_cm"), WorldMinObjPtr) || !WorldMinObjPtr ||
					!Spec->TryGetObjectField(TEXT("world_max_cm"), WorldMaxObjPtr) || !WorldMaxObjPtr ||
					!Spec->TryGetNumberField(TEXT("world_partition_cell_size_cm"), CellSizeCm))
				{
					OutError = TEXT("terrain_spec must include world_min_cm/world_max_cm/world_partition_cell_size_cm.");
					return false;
				}
				TSharedPtr<FJsonObject> WorldMinObj = *WorldMinObjPtr;
				TSharedPtr<FJsonObject> WorldMaxObj = *WorldMaxObjPtr;
				double TileOverlapCm = 0.0;
				if (Spec->HasTypedField<EJson::Number>(TEXT("tile_overlap_cm")))
				{
					TileOverlapCm = Spec->GetNumberField(TEXT("tile_overlap_cm"));
				}

				TSharedRef<FJsonObject> TilePlanArgs = MakeShared<FJsonObject>();
				TilePlanArgs->SetObjectField(TEXT("world_min_cm"), WorldMinObj);
				TilePlanArgs->SetObjectField(TEXT("world_max_cm"), WorldMaxObj);
				TilePlanArgs->SetNumberField(TEXT("world_partition_cell_size_cm"), CellSizeCm);
				TilePlanArgs->SetNumberField(TEXT("tile_overlap_cm"), TileOverlapCm);

				TSharedRef<FJsonObject> TilePlanResult = MakeShared<FJsonObject>();
				TSharedRef<FJsonObject> StepOut = MakeShared<FJsonObject>();
				FString TmpSummary;
				if (!Registry.ExecuteTool(TEXT("terrain_tile_plan"), TilePlanArgs, TilePlanResult, TmpSummary, OutError))
				{
					return false;
				}

				// Create / resolve PCG graph.
				FString GraphAssetPath;
				if (PcgMode == TEXT("B"))
				{
					// In B mode, graph must already exist as an asset path.
					if (!Arguments->TryGetStringField(TEXT("pcg_graph_asset_path"), GraphAssetPath) || GraphAssetPath.IsEmpty())
					{
						if (!Spec->TryGetStringField(TEXT("pcg_graph_asset_path"), GraphAssetPath) || GraphAssetPath.IsEmpty())
						{
							OutError = TEXT("pcg_graph_asset_path is required for pcg_mode=B.");
							return false;
						}
					}
				}
				else
				{
					FString GraphPackagePath;
					FString GraphAssetName;
					if (!Arguments->TryGetStringField(TEXT("pcg_graph_package_path"), GraphPackagePath) || GraphPackagePath.IsEmpty())
					{
						OutError = TEXT("pcg_graph_package_path is required for pcg_mode=A.");
						return false;
					}
					if (!Arguments->TryGetStringField(TEXT("pcg_graph_asset_name"), GraphAssetName) || GraphAssetName.IsEmpty())
					{
						OutError = TEXT("pcg_graph_asset_name is required for pcg_mode=A.");
						return false;
					}
					const FString PackagePathNoTrail = GraphPackagePath.EndsWith(TEXT("/")) ? GraphPackagePath.LeftChop(1) : GraphPackagePath;
					GraphAssetPath = PackagePathNoTrail + TEXT("/") + GraphAssetName + TEXT(".") + GraphAssetName;

					// Create graph asset in content browser.
					{
						TSharedRef<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
						CreateArgs->SetStringField(TEXT("package_path"), GraphPackagePath);
						CreateArgs->SetStringField(TEXT("asset_name"), GraphAssetName);
						if (!Registry.ExecuteTool(TEXT("pcg_graph_create"), CreateArgs, StepOut, TmpSummary, OutError))
						{
							return false;
						}
					}

					// Spec-driven node assembly.
					const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
					if (!Arguments->TryGetArrayField(TEXT("pcg_spec_driven_nodes"), NodesArr) || !NodesArr || NodesArr->Num() <= 0)
					{
						Spec->TryGetArrayField(TEXT("pcg_spec_driven_nodes"), NodesArr);
					}
					if (!NodesArr || NodesArr->Num() <= 0)
					{
						// Minimal template fallback: treat pcg_template_id as an existing PCG graph asset path.
						// (Hardcoded node templates are not available in this tool; this fallback is for prebuilt graphs.)
						FString TemplateId;
						if (Spec->TryGetStringField(TEXT("pcg_template_id"), TemplateId) && !TemplateId.IsEmpty() && TemplateId.StartsWith(TEXT("/")))
						{
							GraphAssetPath = TemplateId;
						}
						else
						{
							OutError = TEXT("pcg_spec_driven_nodes is required for pcg_mode=A (or provide terrain_spec.pcg_template_id as a prebuilt graph asset path).");
							return false;
						}
					}

					for (const TSharedPtr<FJsonValue>& NodeV : *NodesArr)
					{
						const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
						if (!NodeV.IsValid() || !NodeV->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
						{
							OutError = TEXT("Each pcg_spec_driven_nodes[] item must be an object.");
							return false;
						}
						const TSharedRef<FJsonObject> NodeObj = NodeObjPtr->ToSharedRef();

						FString NodeClassPath;
						if (!NodeObj->TryGetStringField(TEXT("node_class_path"), NodeClassPath) || NodeClassPath.IsEmpty())
						{
							OutError = TEXT("pcg_spec_driven_nodes[].node_class_path is required.");
							return false;
						}
						FString NodeLabel;
						NodeObj->TryGetStringField(TEXT("node_label"), NodeLabel);
						if (NodeLabel.IsEmpty())
						{
							NodeLabel = NodeClassPath; // best-effort token.
						}

						// Add node.
						{
							TSharedRef<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
							AddNodeArgs->SetStringField(TEXT("asset_path"), GraphAssetPath);
							AddNodeArgs->SetStringField(TEXT("node_class_path"), NodeClassPath);
							AddNodeArgs->SetStringField(TEXT("node_label"), NodeLabel);
							if (!Registry.ExecuteTool(TEXT("pcg_graph_add_node"), AddNodeArgs, StepOut, TmpSummary, OutError))
							{
								return false;
							}
						}

						// Optional node properties.
						const TSharedPtr<FJsonObject>* PropsObj = nullptr;
						if (NodeObj->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid() && (*PropsObj)->Values.Num() > 0)
						{
							TSharedRef<FJsonObject> SetPropsArgs = MakeShared<FJsonObject>();
							SetPropsArgs->SetStringField(TEXT("asset_path"), GraphAssetPath);
							SetPropsArgs->SetStringField(TEXT("node"), NodeLabel);
							TSharedPtr<FJsonObject> PropsObjCopy = *PropsObj;
							SetPropsArgs->SetObjectField(TEXT("properties"), PropsObjCopy);
							if (!Registry.ExecuteTool(TEXT("pcg_graph_set_node_property"), SetPropsArgs, StepOut, TmpSummary, OutError))
							{
								return false;
							}
						}

						// Optional connections.
						const TArray<TSharedPtr<FJsonValue>>* ConnsArr = nullptr;
						if (NodeObj->TryGetArrayField(TEXT("connections"), ConnsArr) && ConnsArr && ConnsArr->Num() > 0)
						{
							for (const TSharedPtr<FJsonValue>& ConnV : *ConnsArr)
							{
								const TSharedPtr<FJsonObject>* ConnObjPtr = nullptr;
								if (!ConnV.IsValid() || !ConnV->TryGetObject(ConnObjPtr) || !ConnObjPtr || !ConnObjPtr->IsValid())
								{
									OutError = TEXT("connections[] items must be objects.");
									return false;
								}
								const TSharedRef<FJsonObject> ConnObj = ConnObjPtr->ToSharedRef();
								FString SourcePin;
								FString TargetPin;
								if (!ConnObj->TryGetStringField(TEXT("source_pin_path"), SourcePin) || SourcePin.IsEmpty() ||
									!ConnObj->TryGetStringField(TEXT("target_pin_path"), TargetPin) || TargetPin.IsEmpty())
								{
									OutError = TEXT("Each connections[] must include source_pin_path and target_pin_path.");
									return false;
								}
								TSharedRef<FJsonObject> ConnectArgs = MakeShared<FJsonObject>();
								ConnectArgs->SetStringField(TEXT("asset_path"), GraphAssetPath);
								ConnectArgs->SetStringField(TEXT("source_pin_path"), SourcePin);
								ConnectArgs->SetStringField(TEXT("target_pin_path"), TargetPin);
								if (!Registry.ExecuteTool(TEXT("pcg_graph_connect"), ConnectArgs, StepOut, TmpSummary, OutError))
								{
									return false;
								}
							}
						}
					}
				}

				// Attach PCG component once; update PCGVolume bounds per tile.
				{
					TSharedRef<FJsonObject> AttachArgs = MakeShared<FJsonObject>();
					AttachArgs->SetStringField(TEXT("actor"), PcgActorId);
					AttachArgs->SetStringField(TEXT("graph_path"), GraphAssetPath);
					AttachArgs->SetStringField(TEXT("volume_actor"), PcgVolumeActorId);
					AttachArgs->SetStringField(TEXT("generation_trigger"), GenerationTrigger);
					AttachArgs->SetStringField(TEXT("input_type"), InputType);
					if (!Registry.ExecuteTool(TEXT("pcg_component_attach"), AttachArgs, StepOut, TmpSummary, OutError))
					{
						return false;
					}
				}

				// Tile-wise execution.
				const TArray<TSharedPtr<FJsonValue>>* TilesArr = nullptr;
				if (!TilePlanResult->TryGetArrayField(TEXT("tiles"), TilesArr) || !TilesArr || TilesArr->Num() <= 0)
				{
					OutError = TEXT("terrain_tile_plan did not return tiles.");
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> TileResults;
				const FString GenerationId = FString::Printf(TEXT("terrain_pcg_%s_%s"), *PcgActorId, *GraphAssetPath);
				auto VectorAxis = [](const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName) -> double
				{
					double Value = 0.0;
					if (Obj.IsValid())
					{
						Obj->TryGetNumberField(FieldName, Value);
					}
					return Value;
				};
				for (const TSharedPtr<FJsonValue>& TileV : *TilesArr)
				{
					const TSharedPtr<FJsonObject>* TileObjPtr = nullptr;
					if (!TileV.IsValid() || !TileV->TryGetObject(TileObjPtr) || !TileObjPtr || !TileObjPtr->IsValid())
					{
						continue;
					}
					const TSharedRef<FJsonObject> TileObj = TileObjPtr->ToSharedRef();

					int32 GridIx = 0;
					int32 GridIy = 0;
					TileObj->TryGetNumberField(TEXT("grid_ix"), GridIx);
					TileObj->TryGetNumberField(TEXT("grid_iy"), GridIy);

					const TSharedPtr<FJsonObject> BoundsMinObj = TileObj->GetObjectField(TEXT("bounds_min_cm"));
					const TSharedPtr<FJsonObject> BoundsMaxObj = TileObj->GetObjectField(TEXT("bounds_max_cm"));

					const TSharedPtr<FJsonObject> VolumeMinObj = bTileUseCoreOnly && TileObj->HasField(TEXT("core_cell_min_cm")) ? TileObj->GetObjectField(TEXT("core_cell_min_cm")) : BoundsMinObj;
					const TSharedPtr<FJsonObject> VolumeMaxObj = bTileUseCoreOnly && TileObj->HasField(TEXT("core_cell_max_cm")) ? TileObj->GetObjectField(TEXT("core_cell_max_cm")) : BoundsMaxObj;

					FString TileId = TileObj->HasTypedField<EJson::String>(TEXT("id")) ? TileObj->GetStringField(TEXT("id")) : FString::Printf(TEXT("%d_%d"), GridIx, GridIy);
					const double TileExtentXM = FMath::Abs(VectorAxis(VolumeMaxObj, TEXT("x")) - VectorAxis(VolumeMinObj, TEXT("x"))) / 100.0;
					const double TileExtentYM = FMath::Abs(VectorAxis(VolumeMaxObj, TEXT("y")) - VectorAxis(VolumeMinObj, TEXT("y"))) / 100.0;
					const double TileAreaM2 = FMath::Max(1.0, TileExtentXM * TileExtentYM);
					const double TileSizeM = FMath::Max(1.0, FMath::Sqrt(TileAreaM2));

					bool bTileOk = true;
					FString TileErr;
					TSharedPtr<FJsonObject> GenerateReceipt;

					// 1) Load WP region.
					{
						TSharedRef<FJsonObject> LoadArgs = MakeShared<FJsonObject>();
						LoadArgs->SetObjectField(TEXT("min"), BoundsMinObj);
						LoadArgs->SetObjectField(TEXT("max"), BoundsMaxObj);
						if (!Registry.ExecuteTool(TEXT("world_partition_load_region"), LoadArgs, StepOut, TmpSummary, TileErr))
						{
							bTileOk = false;
						}
					}

					// 2) Update PCG volume bounds.
					if (bTileOk)
					{
						TSharedRef<FJsonObject> VolumeArgs = MakeShared<FJsonObject>();
						VolumeArgs->SetStringField(TEXT("volume_actor"), PcgVolumeActorId);
						VolumeArgs->SetObjectField(TEXT("world_min_cm"), VolumeMinObj);
						VolumeArgs->SetObjectField(TEXT("world_max_cm"), VolumeMaxObj);
						if (!Registry.ExecuteTool(TEXT("pcg_volume_set_world_box"), VolumeArgs, StepOut, TmpSummary, TileErr))
						{
							bTileOk = false;
						}
					}

					// 3) Clear + Generate.
					if (bTileOk)
					{
						if (bClearBetweenTiles)
						{
							TSharedRef<FJsonObject> ClearArgs = MakeShared<FJsonObject>();
							ClearArgs->SetStringField(TEXT("actor"), PcgActorId);
							if (!Registry.ExecuteTool(TEXT("pcg_clear"), ClearArgs, StepOut, TmpSummary, TileErr))
							{
								bTileOk = false;
							}
						}
					}

					if (bTileOk)
					{
						TSharedRef<FJsonObject> GenArgs = MakeShared<FJsonObject>();
						GenArgs->SetStringField(TEXT("actor"), PcgActorId);
						GenArgs->SetStringField(TEXT("graph_path"), GraphAssetPath);
						GenArgs->SetBoolField(TEXT("strict"), true);
						GenArgs->SetBoolField(TEXT("unattended"), true);
						GenArgs->SetNumberField(TEXT("tile_count"), 1);
						GenArgs->SetNumberField(TEXT("area_m2"), TileAreaM2);
						GenArgs->SetNumberField(TEXT("tile_size_m"), TileSizeM);
						GenArgs->SetStringField(TEXT("client_request_id"), FString::Printf(TEXT("%s:%s"), *GenerationId, *TileId));
						GenArgs->SetStringField(TEXT("trace_id"), GenerationId);

						TSharedRef<FJsonObject> TileDescriptor = MakeShared<FJsonObject>();
						TileDescriptor->SetStringField(TEXT("id"), TileId);
						TileDescriptor->SetNumberField(TEXT("grid_ix"), GridIx);
						TileDescriptor->SetNumberField(TEXT("grid_iy"), GridIy);
						TileDescriptor->SetStringField(TEXT("generation_id"), GenerationId);
						TileDescriptor->SetStringField(TEXT("actor"), PcgActorId);
						TileDescriptor->SetStringField(TEXT("pcg_volume_actor"), PcgVolumeActorId);
						TileDescriptor->SetObjectField(TEXT("bounds_min_cm"), BoundsMinObj);
						TileDescriptor->SetObjectField(TEXT("bounds_max_cm"), BoundsMaxObj);
						TileDescriptor->SetObjectField(TEXT("volume_min_cm"), VolumeMinObj);
						TileDescriptor->SetObjectField(TEXT("volume_max_cm"), VolumeMaxObj);
						TArray<TSharedPtr<FJsonValue>> AllowedTiles;
						AllowedTiles.Add(MakeShared<FJsonValueObject>(TileDescriptor));
						GenArgs->SetArrayField(TEXT("allowed_tiles"), AllowedTiles);

						TArray<TSharedPtr<FJsonValue>> TileIndices;
						TileIndices.Add(MakeShared<FJsonValueNumber>(static_cast<double>(TileResults.Num())));
						GenArgs->SetArrayField(TEXT("tile_indices"), TileIndices);

						TSharedRef<FJsonObject> GenerateOut = MakeShared<FJsonObject>();
						if (!Registry.ExecuteTool(TEXT("pcg_generate"), GenArgs, GenerateOut, TmpSummary, TileErr))
						{
							bTileOk = false;
						}
						GenerateReceipt = GenerateOut;
					}

					// 4) Unload WP region.
					{
						TSharedRef<FJsonObject> UnloadArgs = MakeShared<FJsonObject>();
						UnloadArgs->SetObjectField(TEXT("min"), BoundsMinObj);
						UnloadArgs->SetObjectField(TEXT("max"), BoundsMaxObj);
						if (!Registry.ExecuteTool(TEXT("world_partition_unload_region"), UnloadArgs, StepOut, TmpSummary, TileErr))
						{
							bTileOk = false;
						}
					}

					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("id"), TileId);
					Entry->SetBoolField(TEXT("ok"), bTileOk);
					Entry->SetStringField(TEXT("generation_id"), GenerationId);
					Entry->SetNumberField(TEXT("tile_count"), 1);
					Entry->SetNumberField(TEXT("area_m2"), TileAreaM2);
					Entry->SetStringField(TEXT("tile_evidence_source"), TEXT("terrain_tile_plan"));
					Entry->SetStringField(TEXT("tile_cap_status"), GenerateReceipt.IsValid() && GenerateReceipt->HasTypedField<EJson::String>(TEXT("tile_cap_status")) ? GenerateReceipt->GetStringField(TEXT("tile_cap_status")) : TEXT("missing_generate_receipt"));
					Entry->SetStringField(TEXT("tile_cap_observed_source"), GenerateReceipt.IsValid() && GenerateReceipt->HasTypedField<EJson::String>(TEXT("tile_cap_observed_source")) ? GenerateReceipt->GetStringField(TEXT("tile_cap_observed_source")) : TEXT("none"));
					Entry->SetBoolField(TEXT("tile_batch_count_known"), GenerateReceipt.IsValid() && GenerateReceipt->HasTypedField<EJson::Boolean>(TEXT("tile_batch_count_known")) && GenerateReceipt->GetBoolField(TEXT("tile_batch_count_known")));
					Entry->SetNumberField(TEXT("tile_batch_count"), GenerateReceipt.IsValid() && GenerateReceipt->HasTypedField<EJson::Number>(TEXT("tile_batch_count")) ? GenerateReceipt->GetNumberField(TEXT("tile_batch_count")) : 0);
					if (GenerateReceipt.IsValid())
					{
						Entry->SetObjectField(TEXT("pcg_generate_receipt"), GenerateReceipt);
					}
					if (!bTileOk)
					{
						Entry->SetStringField(TEXT("error"), TileErr);
					}
					TileResults.Add(MakeShared<FJsonValueObject>(Entry));
				}

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("pcg_mode"), PcgMode);
				OutStructured->SetStringField(TEXT("pcg_graph_asset_path"), GraphAssetPath);
				OutStructured->SetStringField(TEXT("pcg_actor"), PcgActorId);
				OutStructured->SetStringField(TEXT("pcg_volume_actor"), PcgVolumeActorId);
				OutStructured->SetStringField(TEXT("generation_id"), GenerationId);
				OutStructured->SetStringField(TEXT("tile_evidence_contract"), TEXT("terrain_pcg_generate_tiles forwards one allowed_tiles descriptor plus tile_count=1 to each pcg_generate call."));
				OutStructured->SetArrayField(TEXT("tiles"), TileResults);
				OutSummary = TEXT("PCG tile generation finished.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("terrain_pcg_pipeline_run"),
			TEXT("Production-grade high-level pipeline: validate TerrainSpec, create/bind Landscape+LayerInfos (A), write height (and optional weights), then run PCG per tile (A/B)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("terrain_spec"), FSololmcpSchemaBuilder::Object({})},
					{TEXT("layerinfo_mode"), FSololmcpSchemaBuilder::String()},
					{TEXT("pcg_mode"), FSololmcpSchemaBuilder::String()},
					{TEXT("edit_layer_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("pcg_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("pcg_volume_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("generation_trigger"), FSololmcpSchemaBuilder::String()},
					{TEXT("input_type"), FSololmcpSchemaBuilder::String()},
					// PCG Mode A (spec-driven node assembly)
					{TEXT("pcg_graph_package_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("pcg_graph_asset_name"), FSololmcpSchemaBuilder::String()},
					// PCG Mode B
					{TEXT("pcg_graph_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("tile_use_core_cells_only"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("pcg_clear_between_tiles"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("terrain_spec"), TEXT("layerinfo_mode"), TEXT("pcg_mode"), TEXT("pcg_actor"), TEXT("pcg_volume_actor"), TEXT("generation_trigger"), TEXT("input_type")}),

			[&Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
				if (!Arguments->TryGetObjectField(TEXT("terrain_spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
				{
					OutError = TEXT("Missing/invalid terrain_spec object.");
					return false;
				}
				const TSharedRef<FJsonObject> Spec = SpecPtr->ToSharedRef();

				auto GetRequiredUpper = [&](const TCHAR* Key, FString& Out) -> bool
				{
					if (!Arguments->TryGetStringField(Key, Out) || Out.IsEmpty())
					{
						OutError = FString::Printf(TEXT("Missing/empty required field: %s"), Key);
						return false;
					}
					Out = Out.ToUpper();
					return true;
				};

				FString LayerInfoMode;
				if (!GetRequiredUpper(TEXT("layerinfo_mode"), LayerInfoMode))
				{
					return false;
				}
				if (LayerInfoMode != TEXT("A") && LayerInfoMode != TEXT("B"))
				{
					OutError = TEXT("layerinfo_mode must be \"A\" or \"B\".");
					return false;
				}

				FString PcgMode;
				if (!GetRequiredUpper(TEXT("pcg_mode"), PcgMode))
				{
					return false;
				}
				if (PcgMode != TEXT("A") && PcgMode != TEXT("B"))
				{
					OutError = TEXT("pcg_mode must be \"A\" or \"B\".");
					return false;
				}

				bool bDryRun = Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")) ? Arguments->GetBoolField(TEXT("dry_run")) : false;

				FString EditLayerName;
				Arguments->TryGetStringField(TEXT("edit_layer_name"), EditLayerName);

				// PCG required actor refs.
				FString PcgActorId;
				if (!Arguments->TryGetStringField(TEXT("pcg_actor"), PcgActorId) || PcgActorId.IsEmpty())
				{
					OutError = TEXT("Missing pcg_actor.");
					return false;
				}
				FString PcgVolumeActorId;
				if (!Arguments->TryGetStringField(TEXT("pcg_volume_actor"), PcgVolumeActorId) || PcgVolumeActorId.IsEmpty())
				{
					OutError = TEXT("Missing pcg_volume_actor.");
					return false;
				}

				FString GenerationTrigger;
				if (!Arguments->TryGetStringField(TEXT("generation_trigger"), GenerationTrigger) || GenerationTrigger.IsEmpty())
				{
					OutError = TEXT("Missing generation_trigger.");
					return false;
				}
				FString InputType;
				if (!Arguments->TryGetStringField(TEXT("input_type"), InputType) || InputType.IsEmpty())
				{
					OutError = TEXT("Missing input_type.");
					return false;
				}

				// ---- 1) Validate TerrainSpec
				TSharedRef<FJsonObject> ValidateArgs = MakeShared<FJsonObject>();
				ValidateArgs->SetObjectField(TEXT("terrain_spec"), Spec);
				TSharedRef<FJsonObject> ValidateOut = MakeShared<FJsonObject>();
				{
					FString TmpSummary;
					if (!Registry.ExecuteTool(TEXT("terrain_spec_validate"), ValidateArgs, ValidateOut, TmpSummary, OutError))
					{
						return false;
					}
					if (ValidateOut->HasTypedField<EJson::Boolean>(TEXT("valid")) && !ValidateOut->GetBoolField(TEXT("valid")))
					{
						OutError = TEXT("TerrainSpec validation failed.");
						return false;
					}
				}

				// ---- 2) Create Landscape from Spec (best-effort material + bounds mapping).
				TSharedRef<FJsonObject> LandscapeArgs = MakeShared<FJsonObject>();
				LandscapeArgs->SetObjectField(TEXT("terrain_spec"), Spec);
				if (!EditLayerName.IsEmpty())
				{
					LandscapeArgs->SetStringField(TEXT("edit_layer_name"), EditLayerName);
				}
				TSharedRef<FJsonObject> LandscapeOut = MakeShared<FJsonObject>();
				{
					FString TmpSummary;
					if (!Registry.ExecuteTool(TEXT("terrain_landscape_create_from_spec"), LandscapeArgs, LandscapeOut, TmpSummary, OutError))
					{
						return false;
					}
				}
				const FString LandscapeId = LandscapeOut->GetStringField(TEXT("path"));
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}

				// Spec common fields for height/weights.
				const TSharedPtr<FJsonObject>* WorldMinObjPtr = nullptr;
				const TSharedPtr<FJsonObject>* WorldMaxObjPtr = nullptr;
				double CellSizeCm = 0.0;
				if (!Spec->TryGetObjectField(TEXT("world_min_cm"), WorldMinObjPtr) || !WorldMinObjPtr ||
					!Spec->TryGetObjectField(TEXT("world_max_cm"), WorldMaxObjPtr) || !WorldMaxObjPtr ||
					!Spec->TryGetNumberField(TEXT("world_partition_cell_size_cm"), CellSizeCm))
				{
					OutError = TEXT("terrain_spec missing world_min_cm/world_max_cm/world_partition_cell_size_cm.");
					return false;
				}
				TSharedPtr<FJsonObject> WorldMinObj = *WorldMinObjPtr;
				TSharedPtr<FJsonObject> WorldMaxObj = *WorldMaxObjPtr;
				double TileOverlapCm = 0.0;
				if (Spec->HasTypedField<EJson::Number>(TEXT("tile_overlap_cm")))
				{
					TileOverlapCm = Spec->GetNumberField(TEXT("tile_overlap_cm"));
				}

				// ---- 3) LayerInfo mode (A creates/binds; B expects existing.
				const TArray<TSharedPtr<FJsonValue>>* PaintLayersJson = nullptr;
				if (!Spec->TryGetArrayField(TEXT("paint_layers"), PaintLayersJson) || !PaintLayersJson || PaintLayersJson->Num() <= 0)
				{
					OutError = TEXT("terrain_spec.paint_layers must be a non-empty array.");
					return false;
				}

				TArray<FString> PaintLayerNames;
				PaintLayerNames.Reserve(PaintLayersJson->Num());
				TArray<TPair<FString, FString>> LayerNameToPackagePath; // layer_name -> layerinfo_asset_package_path (A only)

				for (const TSharedPtr<FJsonValue>& V : *PaintLayersJson)
				{
					const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
					if (!V.IsValid() || !V->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
					{
						OutError = TEXT("terrain_spec.paint_layers[] items must be objects.");
						return false;
					}
					const TSharedRef<FJsonObject> Obj = ObjPtr->ToSharedRef();
					FString LayerName;
					FString LayerinfoPackagePath;
					if (!Obj->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty() ||
						!Obj->TryGetStringField(TEXT("layerinfo_asset_package_path"), LayerinfoPackagePath) || LayerinfoPackagePath.IsEmpty())
					{
						OutError = TEXT("Each paint_layers[] must contain layer_name and layerinfo_asset_package_path.");
						return false;
					}
					PaintLayerNames.Add(LayerName);
					LayerNameToPackagePath.Add(TPair<FString, FString>(LayerName, LayerinfoPackagePath));
				}

				TSharedRef<FJsonObject> LayerInfoBindOut = MakeShared<FJsonObject>();
				if (LayerInfoMode == TEXT("A"))
				{
					// Create and bind LayerInfos.
					TArray<TSharedPtr<FJsonValue>> LayersBindArr;
					for (const TPair<FString, FString>& Pair : LayerNameToPackagePath)
					{
						TSharedRef<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
						CreateArgs->SetStringField(TEXT("layer_name"), Pair.Key);
						CreateArgs->SetStringField(TEXT("layerinfo_asset_package_path"), Pair.Value);
						TSharedRef<FJsonObject> CreateOut = MakeShared<FJsonObject>();

						FString TmpSummary;
						if (!Registry.ExecuteTool(TEXT("landscape_layerinfo_create"), CreateArgs, CreateOut, TmpSummary, OutError))
						{
							return false;
						}

						if (!CreateOut->HasTypedField<EJson::String>(TEXT("assetPath")))
						{
							OutError = TEXT("landscape_layerinfo_create did not return assetPath.");
							return false;
						}
						const FString AssetPath = CreateOut->GetStringField(TEXT("assetPath"));

						TSharedRef<FJsonObject> LayerBindObj = MakeShared<FJsonObject>();
						LayerBindObj->SetStringField(TEXT("layer_name"), Pair.Key);
						LayerBindObj->SetStringField(TEXT("layerinfo_asset_path"), AssetPath);
						LayersBindArr.Add(MakeShared<FJsonValueObject>(LayerBindObj));
					}

					TSharedRef<FJsonObject> BindArgs = MakeShared<FJsonObject>();
					BindArgs->SetStringField(TEXT("landscape"), LandscapeId);
					BindArgs->SetArrayField(TEXT("layers"), LayersBindArr);
					FString TmpSummary;
					if (!Registry.ExecuteTool(TEXT("landscape_layerinfo_bind"), BindArgs, LayerInfoBindOut, TmpSummary, OutError))
					{
						return false;
					}
				}
				else
				{
					// B: verify layer infos exist for requested paint layer names.
					for (const FString& Name : PaintLayerNames)
					{
						if (!FindLandscapePaintLayerInfo(Landscape, Name))
						{
							OutError = FString::Printf(TEXT("layerinfo_mode=B but Landscape paint layer not found/bound: %s"), *Name);
							return false;
						}
					}
					LayerInfoBindOut->SetBoolField(TEXT("skipped"), true);
				}

				// ---- 4) Write height (tile noise).
				const TSharedPtr<FJsonObject>* HeightGenObj = nullptr;
				if (!Spec->TryGetObjectField(TEXT("height_generation"), HeightGenObj) || !HeightGenObj || !HeightGenObj->IsValid())
				{
					OutError = TEXT("terrain_spec.height_generation missing/invalid.");
					return false;
				}
				const int32 Seed = (*HeightGenObj)->GetIntegerField(TEXT("seed"));
				const double Frequency = (*HeightGenObj)->GetNumberField(TEXT("frequency"));
				const double Amplitude = (*HeightGenObj)->GetNumberField(TEXT("amplitude"));

				TSharedRef<FJsonObject> HeightArgs = MakeShared<FJsonObject>();
				HeightArgs->SetStringField(TEXT("landscape"), LandscapeId);
				HeightArgs->SetObjectField(TEXT("world_min_cm"), WorldMinObj.ToSharedRef());
				HeightArgs->SetObjectField(TEXT("world_max_cm"), WorldMaxObj.ToSharedRef());
				HeightArgs->SetNumberField(TEXT("world_partition_cell_size_cm"), CellSizeCm);
				HeightArgs->SetNumberField(TEXT("tile_overlap_cm"), TileOverlapCm);
				HeightArgs->SetBoolField(TEXT("use_core_cells_only"), true);
				HeightArgs->SetBoolField(TEXT("dry_run"), bDryRun);
				HeightArgs->SetNumberField(TEXT("blend_alpha"), 1.0);
				HeightArgs->SetNumberField(TEXT("seed"), Seed);
				HeightArgs->SetNumberField(TEXT("frequency"), Frequency);
				HeightArgs->SetNumberField(TEXT("amplitude"), Amplitude);

				TSharedRef<FJsonObject> HeightOut = MakeShared<FJsonObject>();
				{
					FString TmpSummary;
					if (!Registry.ExecuteTool(TEXT("terrain_fill_tile_plan_landscape_noise"), HeightArgs, HeightOut, TmpSummary, OutError))
					{
						return false;
					}
				}

				const TSharedPtr<FJsonObject>* WeightGenObj = nullptr;
				bool bHasWeights = Spec->TryGetObjectField(TEXT("weight_generation"), WeightGenObj) && WeightGenObj && WeightGenObj->IsValid();

				TSharedRef<FJsonObject> WeightOut = MakeShared<FJsonObject>();
				if (bHasWeights && !bDryRun)
				{
					// Height->weights tool currently uses paint layer names only; it does not consume weight_generation fields yet.
					TSharedRef<FJsonObject> WeightArgs = MakeShared<FJsonObject>();
					WeightArgs->SetStringField(TEXT("landscape"), LandscapeId);
					WeightArgs->SetObjectField(TEXT("world_min_cm"), WorldMinObj.ToSharedRef());
					WeightArgs->SetObjectField(TEXT("world_max_cm"), WorldMaxObj.ToSharedRef());
					WeightArgs->SetNumberField(TEXT("world_partition_cell_size_cm"), CellSizeCm);
					WeightArgs->SetNumberField(TEXT("tile_overlap_cm"), TileOverlapCm);
					WeightArgs->SetBoolField(TEXT("use_core_cells_only"), true);
					WeightArgs->SetBoolField(TEXT("dry_run"), bDryRun);
					WeightArgs->SetArrayField(TEXT("paint_layers"), [&PaintLayerNames]()
					{
						TArray<TSharedPtr<FJsonValue>> Arr;
						Arr.Reserve(PaintLayerNames.Num());
						for (const FString& N : PaintLayerNames)
						{
							Arr.Add(MakeShared<FJsonValueString>(N));
						}
						return Arr;
					}());

					FString TmpSummary;
					if (!Registry.ExecuteTool(TEXT("terrain_fill_tile_plan_landscape_weights_height_based"), WeightArgs, WeightOut, TmpSummary, OutError))
					{
						return false;
					}
				}
				else
				{
					WeightOut->SetBoolField(TEXT("skipped"), true);
				}

				// ---- 5) PCG per tile (tile WP load -> volume bounds -> generate -> unload).
				TSharedRef<FJsonObject> PcgArgs = MakeShared<FJsonObject>();
				PcgArgs->SetObjectField(TEXT("terrain_spec"), Spec);
				PcgArgs->SetStringField(TEXT("pcg_mode"), PcgMode);
				PcgArgs->SetStringField(TEXT("pcg_actor"), PcgActorId);
				PcgArgs->SetStringField(TEXT("pcg_volume_actor"), PcgVolumeActorId);
				PcgArgs->SetStringField(TEXT("generation_trigger"), GenerationTrigger);
				PcgArgs->SetStringField(TEXT("input_type"), InputType);
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("tile_use_core_cells_only")))
				{
					PcgArgs->SetBoolField(TEXT("tile_use_core_cells_only"), Arguments->GetBoolField(TEXT("tile_use_core_cells_only")));
				}
				if (Arguments->HasTypedField<EJson::Boolean>(TEXT("pcg_clear_between_tiles")))
				{
					PcgArgs->SetBoolField(TEXT("pcg_clear_between_tiles"), Arguments->GetBoolField(TEXT("pcg_clear_between_tiles")));
				}

				// Pass through node assembly inputs when available.
				TSharedPtr<FJsonValue> Dummy;
				(void)Dummy;
				if (PcgMode == TEXT("A"))
				{
					FString GraphPackagePath;
					FString GraphAssetName;
					if (Arguments->TryGetStringField(TEXT("pcg_graph_package_path"), GraphPackagePath) && !GraphPackagePath.IsEmpty() &&
						Arguments->TryGetStringField(TEXT("pcg_graph_asset_name"), GraphAssetName) && !GraphAssetName.IsEmpty())
					{
						PcgArgs->SetStringField(TEXT("pcg_graph_package_path"), GraphPackagePath);
						PcgArgs->SetStringField(TEXT("pcg_graph_asset_name"), GraphAssetName);
					}
					else
					{
						PcgArgs->SetStringField(TEXT("pcg_graph_package_path"), TEXT("/Game/Generated/PCG"));
						PcgArgs->SetStringField(TEXT("pcg_graph_asset_name"), TEXT("PCG_Terrain_AutoGraph"));
					}

					// If user provided spec-driven nodes, pass them in so the tool doesn't need fallback paths.
					if (Spec->HasTypedField<EJson::Array>(TEXT("pcg_spec_driven_nodes")))
					{
						const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
						if (Spec->TryGetArrayField(TEXT("pcg_spec_driven_nodes"), NodesArr) && NodesArr && NodesArr->Num() > 0)
						{
							PcgArgs->SetArrayField(TEXT("pcg_spec_driven_nodes"), *NodesArr);
						}
					}
				}
				else
				{
					FString GraphAssetPath;
					if (!Arguments->TryGetStringField(TEXT("pcg_graph_asset_path"), GraphAssetPath) || GraphAssetPath.IsEmpty())
					{
						Spec->TryGetStringField(TEXT("pcg_graph_asset_path"), GraphAssetPath);
					}
					if (GraphAssetPath.IsEmpty())
					{
						OutError = TEXT("pcg_mode=B requires pcg_graph_asset_path (either in tool args or inside terrain_spec).");
						return false;
					}
					PcgArgs->SetStringField(TEXT("pcg_graph_asset_path"), GraphAssetPath);
				}

				TSharedRef<FJsonObject> PcgOut = MakeShared<FJsonObject>();
				if (!bDryRun)
				{
					FString TmpSummary;
					if (!Registry.ExecuteTool(TEXT("terrain_pcg_generate_tiles"), PcgArgs, PcgOut, TmpSummary, OutError))
					{
						return false;
					}
				}
				else
				{
					PcgOut->SetBoolField(TEXT("skipped"), true);
				}

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetObjectField(TEXT("validation"), ValidateOut);
				OutStructured->SetObjectField(TEXT("landscape"), LandscapeOut);
				OutStructured->SetObjectField(TEXT("layerinfo_bind"), LayerInfoBindOut);
				OutStructured->SetObjectField(TEXT("height_write"), HeightOut);
				OutStructured->SetObjectField(TEXT("weight_write"), WeightOut);
				OutStructured->SetObjectField(TEXT("pcg_generate"), PcgOut);
				OutStructured->SetStringField(TEXT("landscape_id"), LandscapeId);
				OutStructured->SetStringField(TEXT("pcg_mode"), PcgMode);
				OutStructured->SetStringField(TEXT("layerinfo_mode"), LayerInfoMode);
				OutSummary = TEXT("Terrain+PCG pipeline run finished.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_lake_basin_world"),
			TEXT("Hydrology MVP: carve a smooth circular basin in world XY (cm) by lowering existing heightmap samples toward a lake bed (subtract up to depth_u16 with feather)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape"), FSololmcpSchemaBuilder::String()},
					{TEXT("world_center_cm"), VectorSchema()},
					{TEXT("radius_cm"), FSololmcpSchemaBuilder::Number()},
					{TEXT("feather_cm"), FSololmcpSchemaBuilder::Number()},
					{TEXT("depth_u16"), FSololmcpSchemaBuilder::Integer()}
				},
				{TEXT("landscape"), TEXT("world_center_cm"), TEXT("radius_cm"), TEXT("depth_u16")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				TSharedPtr<FJsonObject> CenterObj;
				if (!TryGetObjectField(Arguments, TEXT("world_center_cm"), CenterObj))
				{
					OutError = TEXT("Missing world_center_cm.");
					return false;
				}
				FVector CenterWorld;
				if (!FSololmcpEditorServices::JsonToVector(CenterObj, CenterWorld))
				{
					OutError = TEXT("world_center_cm must be a vector object.");
					return false;
				}
				const float RadiusCm = static_cast<float>(Arguments->GetNumberField(TEXT("radius_cm")));
				if (RadiusCm <= KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("radius_cm must be positive.");
					return false;
				}
				const float FeatherCm = Arguments->HasTypedField<EJson::Number>(TEXT("feather_cm")) ? static_cast<float>(Arguments->GetNumberField(TEXT("feather_cm"))) : FMath::Max(100.f, RadiusCm * 0.1f);
				const int32 DepthU16 = FMath::Clamp(Arguments->GetIntegerField(TEXT("depth_u16")), 1, 65535);

				const FVector BoxMin(CenterWorld.X - RadiusCm, CenterWorld.Y - RadiusCm, CenterWorld.Z - 100000.f);
				const FVector BoxMax(CenterWorld.X + RadiusCm, CenterWorld.Y + RadiusCm, CenterWorld.Z + 100000.f);
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!TryWorldBoxToLandscapeHeightmapIndices(Landscape, BoxMin, BoxMax, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}

				const int32 Width = MaxX - MinX + 1;
				const int32 Height = MaxY - MinY + 1;
				TArray<uint16> Heights;
				Heights.SetNumZeroed(Width * Height);
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, Heights.GetData(), 0);

				const FTransform LandscapeToWorld = Landscape->LandscapeActorToWorld();
				const FVector Scale3D = Landscape->GetActorScale3D();
				const float Sx = FMath::Max(Scale3D.X, KINDA_SMALL_NUMBER);
				const float Sy = FMath::Max(Scale3D.Y, KINDA_SMALL_NUMBER);
				const FVector2D Center2D(CenterWorld.X, CenterWorld.Y);
				const float InnerRadius = FMath::Max(0.f, RadiusCm - FeatherCm);

				int32 Idx = 0;
				for (int32 Y = MinY; Y <= MaxY; ++Y)
				{
					for (int32 X = MinX; X <= MaxX; ++X)
					{
						const FVector VertWorld = LandscapeToWorld.TransformPosition(FVector(static_cast<float>(X) * Sx, static_cast<float>(Y) * Sy, 0.f));
						const float Dist = FVector2D::Distance(FVector2D(VertWorld.X, VertWorld.Y), Center2D);
						float Mask = 0.f;
						if (Dist <= InnerRadius)
						{
							Mask = 1.f;
						}
						else if (Dist < RadiusCm && FeatherCm > KINDA_SMALL_NUMBER)
						{
							const float T = FMath::Clamp((RadiusCm - Dist) / FeatherCm, 0.f, 1.f);
							Mask = T * T * (3.f - 2.f * T);
						}
						if (Mask > 0.f)
						{
							const int32 Sub = FMath::RoundToInt(static_cast<float>(DepthU16) * Mask);
							const int32 NewH = FMath::Clamp(static_cast<int32>(Heights[Idx]) - Sub, 0, 65535);
							Heights[Idx] = static_cast<uint16>(NewH);
						}
						++Idx;
					}
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeLakeBasinWorld", "SOMOLMCP Lake Basin"));
				Landscape->Modify();
				LandscapeEdit.SetHeightData(MinX, MinY, MaxX, MaxY, Heights.GetData(), 0, true);
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetNumberField(TEXT("depth_u16"), DepthU16);
				OutStructured->SetNumberField(TEXT("radius_cm"), RadiusCm);
				OutStructured->SetNumberField(TEXT("feather_cm"), FeatherCm);
				OutStructured->SetNumberField(TEXT("updatedSamples"), Heights.Num());
				OutSummary = TEXT("Applied lake basin carve to landscape region.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_import_heightmap"),
			TEXT("Import a heightmap from a render target into an existing landscape."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("render_target"), FSololmcpSchemaBuilder::String()}, {TEXT("use_rg"), FSololmcpSchemaBuilder::Boolean()}, {TEXT("edit_layer_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape"), TEXT("render_target")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString RenderTargetPath;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("render_target"), RenderTargetPath))
				{
					OutError = TEXT("Missing argument: landscape or render_target");
					return false;
				}
				const bool bUseRG = Arguments->HasTypedField<EJson::Boolean>(TEXT("use_rg")) ? Arguments->GetBoolField(TEXT("use_rg")) : false;
				const int32 EditLayerIndex = Arguments->HasTypedField<EJson::Number>(TEXT("edit_layer_index")) ? Arguments->GetIntegerField(TEXT("edit_layer_index")) : 0;
				const FString PythonCode = FString::Printf(
					TEXT("import unreal\n")
					TEXT("actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n")
					TEXT("asset_subsystem = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)\n")
					TEXT("target = None\n")
					TEXT("for actor in actor_subsystem.get_all_level_actors():\n")
					TEXT("    if actor.get_actor_label() == %s or actor.get_name() == %s or actor.get_path_name() == %s:\n")
					TEXT("        target = actor\n")
					TEXT("        break\n")
					TEXT("rt = asset_subsystem.load_asset(%s)\n")
					TEXT("if target is None:\n")
					TEXT("    raise RuntimeError('Landscape actor not found')\n")
					TEXT("if rt is None:\n")
					TEXT("    raise RuntimeError('Render target not found')\n")
					TEXT("target.landscape_import_heightmap_from_render_target(rt, %s, %d)\n"),
					*FString::Printf(TEXT("'%s'"), *LandscapeId),
					*FString::Printf(TEXT("'%s'"), *LandscapeId),
					*FString::Printf(TEXT("'%s'"), *LandscapeId),
					*FString::Printf(TEXT("'%s'"), *RenderTargetPath),
					bUseRG ? TEXT("True") : TEXT("False"),
					EditLayerIndex);
				return Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), false, OutStructured, OutSummary, OutError);
			},
			[](const FSololmcpToolExecutionContext& Context, FString& OutReason)
			{
				return Context.Services.IsPythonAvailable(&OutReason);
			},
		0,
		nullptr,
		true
		});

		Registry.Register({
			TEXT("landscape_import_weightmap"),
			TEXT("Import a weightmap from a render target into an existing landscape layer."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("render_target"), FSololmcpSchemaBuilder::String()}, {TEXT("layer_name"), FSololmcpSchemaBuilder::String()}, {TEXT("edit_layer_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape"), TEXT("render_target"), TEXT("layer_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString RenderTargetPath;
				FString LayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("render_target"), RenderTargetPath) || !Arguments->TryGetStringField(TEXT("layer_name"), LayerName))
				{
					OutError = TEXT("Missing argument: landscape, render_target or layer_name");
					return false;
				}
				const int32 EditLayerIndex = Arguments->HasTypedField<EJson::Number>(TEXT("edit_layer_index")) ? Arguments->GetIntegerField(TEXT("edit_layer_index")) : 0;
				const FString PythonCode = FString::Printf(
					TEXT("import unreal\n")
					TEXT("actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n")
					TEXT("asset_subsystem = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)\n")
					TEXT("target = None\n")
					TEXT("for actor in actor_subsystem.get_all_level_actors():\n")
					TEXT("    if actor.get_actor_label() == %s or actor.get_name() == %s or actor.get_path_name() == %s:\n")
					TEXT("        target = actor\n")
					TEXT("        break\n")
					TEXT("rt = asset_subsystem.load_asset(%s)\n")
					TEXT("if target is None:\n")
					TEXT("    raise RuntimeError('Landscape actor not found')\n")
					TEXT("if rt is None:\n")
					TEXT("    raise RuntimeError('Render target not found')\n")
					TEXT("target.landscape_import_weightmap_from_render_target(rt, %s, %d)\n"),
					*FString::Printf(TEXT("'%s'"), *LandscapeId),
					*FString::Printf(TEXT("'%s'"), *LandscapeId),
					*FString::Printf(TEXT("'%s'"), *LandscapeId),
					*FString::Printf(TEXT("'%s'"), *RenderTargetPath),
					*FString::Printf(TEXT("'%s'"), *LayerName),
					EditLayerIndex);
				return Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), false, OutStructured, OutSummary, OutError);
			},
			[](const FSololmcpToolExecutionContext& Context, FString& OutReason)
			{
				return Context.Services.IsPythonAvailable(&OutReason);
			},
		0,
		nullptr,
		true
		});

		Registry.Register({
			TEXT("landscape_layerinfo_create"),
			TEXT("Create a ULandscapeLayerInfoObject asset for a Landscape Target Layer name."),
			FSololmcpSchemaBuilder::Object({{TEXT("layer_name"), FSololmcpSchemaBuilder::String()}, {TEXT("layerinfo_asset_package_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("layer_name"), TEXT("layerinfo_asset_package_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LayerName;
				FString LayerInfoAssetPackagePath;
				if (!Arguments->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty() ||
					!Arguments->TryGetStringField(TEXT("layerinfo_asset_package_path"), LayerInfoAssetPackagePath) || LayerInfoAssetPackagePath.IsEmpty())
				{
					OutError = TEXT("Missing argument: layer_name or layerinfo_asset_package_path");
					return false;
				}

				// Creates a new asset package under the given content directory.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
				ULandscapeLayerInfoObject* LayerInfo = UE::Landscape::CreateTargetLayerInfo(FName(*LayerName), LayerInfoAssetPackagePath);
#else
				// UE::Landscape::CreateTargetLayerInfo is 5.6+. Creating the asset directly
				// gives the same object; the helper only adds naming/collision handling that
				// the caller-supplied name already covers here.
				ULandscapeLayerInfoObject* LayerInfo = Cast<ULandscapeLayerInfoObject>(
					Context.Services.CreateAsset(
						LayerInfoAssetPackagePath,
						LayerName,
						TEXT("/Script/Landscape.LandscapeLayerInfoObject"),
						FString(),
						nullptr,
						OutError));
				if (LayerInfo)
				{
					LayerInfo->LayerName = FName(*LayerName);
					LayerInfo->MarkPackageDirty();
				}
#endif
				if (!LayerInfo)
				{
					OutError = TEXT("Failed to create ULandscapeLayerInfoObject.");
					return false;
				}

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("layer_name"), LayerName);
				OutStructured->SetStringField(TEXT("assetPath"), LayerInfo->GetPathName());
				OutStructured->SetStringField(TEXT("packagePath"), LayerInfo->GetPackage() ? LayerInfo->GetPackage()->GetName() : TEXT(""));
				OutSummary = TEXT("Created landscape LayerInfo asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_layerinfo_bind"),
			TEXT("Bind Landscape Target LayerInfo objects (LayerInfoObj) by layer_name to an existing Landscape."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape"), FSololmcpSchemaBuilder::String()},
					{TEXT("layers"), FSololmcpSchemaBuilder::Array(
						FSololmcpSchemaBuilder::Object(
							{
								{TEXT("layer_name"), FSololmcpSchemaBuilder::String()},
								{TEXT("layerinfo_asset_path"), FSololmcpSchemaBuilder::String()}
							},
							{TEXT("layer_name"), TEXT("layerinfo_asset_path")}
						)
					)}
				},
				{TEXT("landscape"), TEXT("layers")}
			),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing argument: landscape");
					return false;
				}

				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}

				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}

				const TArray<TSharedPtr<FJsonValue>>* LayersArr = nullptr;
				if (!Arguments->TryGetArrayField(TEXT("layers"), LayersArr) || !LayersArr || LayersArr->Num() <= 0)
				{
					OutError = TEXT("Missing/empty layers array.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeLayerInfoBind", "SOMOLMCP Bind Landscape LayerInfos"));
				Landscape->Modify();
				LandscapeInfo->Modify();

				for (const TSharedPtr<FJsonValue>& Item : *LayersArr)
				{
					const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
					if (!Item.IsValid() || !Item->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
					{
						OutError = TEXT("Each layers[] item must be an object.");
						return false;
					}

					FString LayerName;
					FString LayerInfoAssetPath;
					if (!(*ObjPtr)->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty() ||
						!(*ObjPtr)->TryGetStringField(TEXT("layerinfo_asset_path"), LayerInfoAssetPath) || LayerInfoAssetPath.IsEmpty())
					{
						OutError = TEXT("Each layers[] item must contain layer_name and layerinfo_asset_path.");
						return false;
					}

					ULandscapeLayerInfoObject* LayerInfoObj = Cast<ULandscapeLayerInfoObject>(Context.Services.LoadAsset(LayerInfoAssetPath, OutError));
					if (!LayerInfoObj)
					{
						// OutError already filled by LoadAsset / type check.
						return false;
					}

					bool bFound = false;
					for (FLandscapeInfoLayerSettings& Settings : LandscapeInfo->Layers)
					{
						if (Settings.LayerName == FName(*LayerName))
						{
							Settings.LayerInfoObj = LayerInfoObj;
							bFound = true;
							break;
						}
					}

					if (!bFound)
					{
						FLandscapeInfoLayerSettings NewSettings(FName(*LayerName), Landscape);
						NewSettings.LayerInfoObj = LayerInfoObj;
						LandscapeInfo->Layers.Add(MoveTemp(NewSettings));
					}
				}

				// Keep the explicit bindings intact. Some editor states rebuild the map from the
				// material and drop manually inserted LayerInfoObj entries until the Landscape UI
				// refreshes its target layer list.
				Landscape->MarkPackageDirty();

				OutStructured = LandscapePaintLayersToJson(Landscape);
				OutSummary = TEXT("Bound landscape LayerInfo objects.");
				return true;
			}
		, nullptr
		, 5
		});

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_list_layers"),
			TEXT("List edit layers on a landscape."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}}, {TEXT("landscape")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing argument: landscape");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Listed landscape layers.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_create_layer"),
			TEXT("Create a new edit layer on a landscape."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer_name"), FSololmcpSchemaBuilder::String()}, {TEXT("ignore_limit"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("landscape"), TEXT("layer_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer_name"), LayerName))
				{
					OutError = TEXT("Missing argument: landscape or layer_name");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}

				const bool bIgnoreLimit = Arguments->HasTypedField<EJson::Boolean>(TEXT("ignore_limit")) ? Arguments->GetBoolField(TEXT("ignore_limit")) : false;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeCreateLayer", "SOMOLMCP Create Landscape Layer"));
				const int32 LayerIndex = Landscape->CreateLayer(*LayerName, TSubclassOf<ULandscapeEditLayerBase>(), bIgnoreLimit);
				if (LayerIndex == INDEX_NONE)
				{
					OutError = TEXT("Failed to create landscape layer.");
					return false;
				}
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Created landscape layer.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_set_editing_layer"),
			TEXT("Set the current editing layer on a landscape."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("landscape"), TEXT("layer_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer_name"), LayerName))
				{
					OutError = TEXT("Missing argument: landscape or layer_name");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeEditLayerBase* Layer = Landscape->GetEditLayer(*LayerName);
				if (!Layer)
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetEditingLayer", "SOMOLMCP Set Editing Layer"));
				Landscape->SetEditingLayer(Layer->GetGuid());
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Set landscape editing layer.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

		#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_clear_edit_layer"),
			TEXT("Clear an edit layer on a landscape."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("landscape"), TEXT("layer_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer_name"), LayerName))
				{
					OutError = TEXT("Missing argument: landscape or layer_name");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				const int32 LayerIndex = Landscape->GetLayerIndex(*LayerName);
				if (LayerIndex == INDEX_NONE)
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeClearLayer", "SOMOLMCP Clear Landscape Layer"));
				Landscape->ClearEditLayer(LayerIndex);
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Cleared landscape edit layer.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		#endif

		Registry.Register({
			TEXT("landscape_apply_spline"),
			TEXT("Apply a spline component to a landscape."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape"), FSololmcpSchemaBuilder::String()},
					{TEXT("spline_actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("start_width"), FSololmcpSchemaBuilder::Number()},
					{TEXT("end_width"), FSololmcpSchemaBuilder::Number()},
					{TEXT("start_side_falloff"), FSololmcpSchemaBuilder::Number()},
					{TEXT("end_side_falloff"), FSololmcpSchemaBuilder::Number()},
					{TEXT("subdivisions"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("raise_heights"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("lower_heights"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("paint_layer_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("edit_layer_name"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("landscape"), TEXT("spline_actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString SplineActorId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("spline_actor"), SplineActorId))
				{
					OutError = TEXT("Missing argument: landscape or spline_actor");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				AActor* SplineActor = Context.Services.FindActorByLabelOrName(SplineActorId, OutError);
				if (!SplineActor)
				{
					return false;
				}
				USplineComponent* SplineComponent = SplineActor->FindComponentByClass<USplineComponent>();
				if (!SplineComponent)
				{
					OutError = TEXT("Spline actor does not have a spline component.");
					return false;
				}

				const double StartWidth = Arguments->HasTypedField<EJson::Number>(TEXT("start_width")) ? Arguments->GetNumberField(TEXT("start_width")) : 200.0;
				const double EndWidth = Arguments->HasTypedField<EJson::Number>(TEXT("end_width")) ? Arguments->GetNumberField(TEXT("end_width")) : 200.0;
				const double StartFalloff = Arguments->HasTypedField<EJson::Number>(TEXT("start_side_falloff")) ? Arguments->GetNumberField(TEXT("start_side_falloff")) : 200.0;
				const double EndFalloff = Arguments->HasTypedField<EJson::Number>(TEXT("end_side_falloff")) ? Arguments->GetNumberField(TEXT("end_side_falloff")) : 200.0;
				const int32 Subdivisions = Arguments->HasTypedField<EJson::Number>(TEXT("subdivisions")) ? Arguments->GetIntegerField(TEXT("subdivisions")) : 20;
				const bool bRaiseHeights = Arguments->HasTypedField<EJson::Boolean>(TEXT("raise_heights")) ? Arguments->GetBoolField(TEXT("raise_heights")) : true;
				const bool bLowerHeights = Arguments->HasTypedField<EJson::Boolean>(TEXT("lower_heights")) ? Arguments->GetBoolField(TEXT("lower_heights")) : true;

				ULandscapeLayerInfoObject* PaintLayer = nullptr;
				FString PaintLayerAssetPath;
				if (Arguments->TryGetStringField(TEXT("paint_layer_asset_path"), PaintLayerAssetPath) && !PaintLayerAssetPath.IsEmpty())
				{
					PaintLayer = Cast<ULandscapeLayerInfoObject>(Context.Services.LoadAsset(PaintLayerAssetPath, OutError));
					if (!PaintLayer)
					{
						OutError = TEXT("paint_layer_asset_path is not a LandscapeLayerInfoObject.");
						return false;
					}
				}

				FString EditLayerName;
				Arguments->TryGetStringField(TEXT("edit_layer_name"), EditLayerName);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeApplySpline", "SOMOLMCP Apply Landscape Spline"));
				Landscape->EditorApplySpline(
					SplineComponent,
					static_cast<float>(StartWidth),
					static_cast<float>(EndWidth),
					static_cast<float>(StartFalloff),
					static_cast<float>(EndFalloff),
					0.0f,
					0.0f,
					Subdivisions,
					bRaiseHeights,
					bLowerHeights,
					PaintLayer,
					*EditLayerName);

				OutStructured = FSololmcpEditorServices::MakeActorReference(Landscape);
				OutSummary = TEXT("Applied spline to landscape.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_set_heightmap_data"),
			TEXT("Set raw heightmap data on a landscape proxy."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("data"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer())}}, {TEXT("landscape"), TEXT("data")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing argument: landscape");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(LandscapeId, OutError);
				ALandscapeProxy* LandscapeProxy = Cast<ALandscapeProxy>(Actor);
				if (!LandscapeProxy)
				{
					OutError = TEXT("Actor is not a landscape proxy.");
					return false;
				}
				TArray<uint16> Data;
				if (!TryGetUInt16Array(Arguments, TEXT("data"), Data))
				{
					OutError = TEXT("Missing argument: data");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetHeightmapData", "SOMOLMCP Set Landscape Heightmap Data"));
				if (!LandscapeEditorUtils::SetHeightmapData(LandscapeProxy, Data))
				{
					OutError = TEXT("Failed to set landscape heightmap data.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(LandscapeProxy);
				OutSummary = TEXT("Updated landscape heightmap data.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_set_weightmap_data"),
			TEXT("Set raw weightmap data for a landscape paint layer."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer_info_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("data"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer())}}, {TEXT("landscape"), TEXT("layer_info_asset_path"), TEXT("data")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerInfoAssetPath;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer_info_asset_path"), LayerInfoAssetPath))
				{
					OutError = TEXT("Missing argument: landscape or layer_info_asset_path");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(LandscapeId, OutError);
				ALandscapeProxy* LandscapeProxy = Cast<ALandscapeProxy>(Actor);
				if (!LandscapeProxy)
				{
					OutError = TEXT("Actor is not a landscape proxy.");
					return false;
				}
				ULandscapeLayerInfoObject* LayerInfo = Cast<ULandscapeLayerInfoObject>(Context.Services.LoadAsset(LayerInfoAssetPath, OutError));
				if (!LayerInfo)
				{
					OutError = TEXT("layer_info_asset_path is not a LandscapeLayerInfoObject.");
					return false;
				}
				TArray<uint8> Data;
				if (!TryGetUInt8Array(Arguments, TEXT("data"), Data))
				{
					OutError = TEXT("Missing argument: data");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetWeightmapData", "SOMOLMCP Set Landscape Weightmap Data"));
				if (!LandscapeEditorUtils::SetWeightmapData(LandscapeProxy, LayerInfo, Data))
				{
					OutError = TEXT("Failed to set landscape weightmap data.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(LandscapeProxy);
				OutSummary = TEXT("Updated landscape weightmap data.");
				return true;
			}
		, nullptr
		, 5
		});

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_rename_layer"),
			TEXT("Rename a landscape edit layer."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer"), FSololmcpSchemaBuilder::String()}, {TEXT("new_name"), FSololmcpSchemaBuilder::String()}}, {TEXT("landscape"), TEXT("layer"), TEXT("new_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerId;
				FString NewName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer"), LayerId) || !Arguments->TryGetStringField(TEXT("new_name"), NewName))
				{
					OutError = TEXT("Missing landscape, layer or new_name.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeEditLayerBase* Layer = FindLandscapeEditLayer(Landscape, LayerId);
				if (!Layer)
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeRenameLayer", "SOMOLMCP Rename Landscape Layer"));
				Layer->SetName(*NewName, true);
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Renamed landscape edit layer.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_delete_layer"),
			TEXT("Delete a landscape edit layer."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer"), FSololmcpSchemaBuilder::String()}}, {TEXT("landscape"), TEXT("layer")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer"), LayerId))
				{
					OutError = TEXT("Missing landscape or layer.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				int32 LayerIndex = INDEX_NONE;
				if (!FindLandscapeEditLayer(Landscape, LayerId, &LayerIndex))
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeDeleteLayer", "SOMOLMCP Delete Landscape Layer"));
				if (!Landscape->DeleteLayer(LayerIndex))
				{
					OutError = TEXT("Failed to delete landscape edit layer.");
					return false;
				}
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Deleted landscape edit layer.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_reorder_layer"),
			TEXT("Move a landscape edit layer to a new index."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer"), FSololmcpSchemaBuilder::String()}, {TEXT("target_index"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape"), TEXT("layer"), TEXT("target_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerId;
				int32 TargetIndex = INDEX_NONE;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer"), LayerId) || !Arguments->TryGetNumberField(TEXT("target_index"), TargetIndex))
				{
					OutError = TEXT("Missing landscape, layer or target_index.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				int32 LayerIndex = INDEX_NONE;
				if (!FindLandscapeEditLayer(Landscape, LayerId, &LayerIndex))
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const int32 LayerCount = Landscape->GetEditLayers().Num();
				if (TargetIndex < 0 || TargetIndex >= LayerCount)
				{
					OutError = TEXT("target_index is out of range.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeReorderLayer", "SOMOLMCP Reorder Landscape Layer"));
				if (!Landscape->ReorderLayer(LayerIndex, TargetIndex))
				{
					OutError = TEXT("Failed to reorder landscape edit layer.");
					return false;
				}
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Reordered landscape edit layer.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_set_layer_visibility"),
			TEXT("Set visibility on a landscape edit layer."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer"), FSololmcpSchemaBuilder::String()}, {TEXT("visible"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("landscape"), TEXT("layer"), TEXT("visible")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerId;
				bool bVisible = true;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer"), LayerId) || !Arguments->TryGetBoolField(TEXT("visible"), bVisible))
				{
					OutError = TEXT("Missing landscape, layer or visible.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeEditLayerBase* Layer = FindLandscapeEditLayer(Landscape, LayerId);
				if (!Layer)
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetLayerVisibility", "SOMOLMCP Set Landscape Layer Visibility"));
				Layer->SetVisible(bVisible, true);
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Updated landscape edit layer visibility.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_set_layer_locked"),
			TEXT("Set locked state on a landscape edit layer."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer"), FSololmcpSchemaBuilder::String()}, {TEXT("locked"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("landscape"), TEXT("layer"), TEXT("locked")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerId;
				bool bLocked = false;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer"), LayerId) || !Arguments->TryGetBoolField(TEXT("locked"), bLocked))
				{
					OutError = TEXT("Missing landscape, layer or locked.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeEditLayerBase* Layer = FindLandscapeEditLayer(Landscape, LayerId);
				if (!Layer)
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetLayerLocked", "SOMOLMCP Set Landscape Layer Locked"));
				Layer->SetLocked(bLocked, true);
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Updated landscape edit layer locked state.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
		Registry.Register({
			TEXT("landscape_set_layer_alpha"),
			TEXT("Set height or weight alpha on a landscape edit layer."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("layer"), FSololmcpSchemaBuilder::String()}, {TEXT("target_type"), FSololmcpSchemaBuilder::String(TEXT("height | weight"))}, {TEXT("alpha"), FSololmcpSchemaBuilder::Number()}}, {TEXT("landscape"), TEXT("layer"), TEXT("target_type"), TEXT("alpha")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString LayerId;
				FString TargetType;
				double Alpha = 1.0;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("layer"), LayerId) || !Arguments->TryGetStringField(TEXT("target_type"), TargetType) || !Arguments->TryGetNumberField(TEXT("alpha"), Alpha))
				{
					OutError = TEXT("Missing landscape, layer, target_type or alpha.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeEditLayerBase* Layer = FindLandscapeEditLayer(Landscape, LayerId);
				if (!Layer)
				{
					OutError = TEXT("Landscape edit layer was not found.");
					return false;
				}
				const ELandscapeToolTargetType Target = TargetType == TEXT("weight") ? ELandscapeToolTargetType::Weightmap : ELandscapeToolTargetType::Heightmap;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetLayerAlpha", "SOMOLMCP Set Landscape Layer Alpha"));
				Layer->SetAlphaForTargetType(Target, static_cast<float>(Alpha), true, EPropertyChangeType::ValueSet);
				OutStructured = LandscapeLayersToJson(Landscape);
				OutSummary = TEXT("Updated landscape edit layer alpha.");
				return true;
			}
		, nullptr
		, 5
		});
#endif // SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS

		Registry.Register({
			TEXT("landscape_list_paint_layers"),
			TEXT("List paint layer infos used by a landscape."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}}, {TEXT("landscape")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				OutStructured = LandscapePaintLayersToJson(Landscape);
				OutSummary = TEXT("Listed landscape paint layers.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_get_paint_layer_data"),
			TEXT("Read back paint layer weight data from a landscape region."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("paint_layer"), FSololmcpSchemaBuilder::String()}, {TEXT("min_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("min_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape"), TEXT("paint_layer")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString PaintLayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("paint_layer"), PaintLayerName))
				{
					OutError = TEXT("Missing landscape or paint_layer.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeLayerInfoObject* PaintLayer = FindLandscapePaintLayerInfo(Landscape, PaintLayerName);
				if (!PaintLayer)
				{
					OutError = TEXT("Landscape paint layer was not found.");
					return false;
				}
				const FIntRect Bounds = Landscape->GetBoundingRect();
				int32 MinX = Bounds.Min.X;
				int32 MinY = Bounds.Min.Y;
				int32 MaxX = Bounds.Max.X;
				int32 MaxY = Bounds.Max.Y;
				if (Arguments->HasTypedField<EJson::Number>(TEXT("min_x"))) { MinX = Arguments->GetIntegerField(TEXT("min_x")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("min_y"))) { MinY = Arguments->GetIntegerField(TEXT("min_y")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("max_x"))) { MaxX = Arguments->GetIntegerField(TEXT("max_x")); }
				if (Arguments->HasTypedField<EJson::Number>(TEXT("max_y"))) { MaxY = Arguments->GetIntegerField(TEXT("max_y")); }
				if (MinX > MaxX || MinY > MaxY)
				{
					OutError = TEXT("Landscape paint layer read bounds are invalid.");
					return false;
				}
				if (!ValidateLandscapeReadbackRegion(MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				TArray<uint8> WeightData;
				LandscapeEdit.GetWeightDataFast(PaintLayer, MinX, MinY, MaxX, MaxY, &WeightData, 0);
				TArray<TSharedPtr<FJsonValue>> DataJson;
				for (const uint8 Value : WeightData)
				{
					DataJson.Add(MakeShared<FJsonValueNumber>(Value));
				}
				OutStructured->SetStringField(TEXT("paintLayer"), PaintLayer->LayerName.ToString());
				OutStructured->SetStringField(TEXT("assetPath"), PaintLayer->GetPathName());
				OutStructured->SetNumberField(TEXT("minX"), MinX);
				OutStructured->SetNumberField(TEXT("minY"), MinY);
				OutStructured->SetNumberField(TEXT("maxX"), MaxX);
				OutStructured->SetNumberField(TEXT("maxY"), MaxY);
				OutStructured->SetNumberField(TEXT("width"), MaxX - MinX + 1);
				OutStructured->SetNumberField(TEXT("height"), MaxY - MinY + 1);
				OutStructured->SetArrayField(TEXT("data"), DataJson);
				OutSummary = TEXT("Read back landscape paint layer data.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_get_height_region"),
			TEXT("Read back height data from a landscape region."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("min_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("min_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!GetLandscapeBoundsFromArguments(Landscape, Arguments, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				const int32 Width = MaxX - MinX + 1;
				const int32 Height = MaxY - MinY + 1;
				if (!ValidateLandscapeReadbackRegion(MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				TArray<uint16> HeightData;
				HeightData.SetNumZeroed(Width * Height);
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0);
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				TArray<TSharedPtr<FJsonValue>> DataJson;
				for (const uint16 Value : HeightData)
				{
					DataJson.Add(MakeShared<FJsonValueNumber>(Value));
				}
				OutStructured->SetArrayField(TEXT("data"), DataJson);
				OutSummary = TEXT("Read back landscape height data.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_set_height_region"),
			TEXT("Write height data into a landscape region."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("min_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("min_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("data"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer())}}, {TEXT("landscape"), TEXT("data")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				TArray<uint16> HeightData;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !TryGetUInt16Array(Arguments, TEXT("data"), HeightData))
				{
					OutError = TEXT("Missing landscape or data.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!GetLandscapeBoundsFromArguments(Landscape, Arguments, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				const int32 ExpectedSamples = (MaxX - MinX + 1) * (MaxY - MinY + 1);
				if (HeightData.Num() != ExpectedSamples)
				{
					OutError = TEXT("data length must equal region width * height.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetHeightRegion", "SOMOLMCP Set Landscape Height Region"));
				Landscape->Modify();
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.SetHeightData(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0, true);
				TArray<uint16> ReadbackData;
				ReadbackData.SetNumZeroed(ExpectedSamples);
				LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, ReadbackData.GetData(), 0);
				if (ReadbackData != HeightData)
				{
					OutError = TEXT("Landscape height region write failed immediate readback.");
					return false;
				}
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetNumberField(TEXT("updatedSamples"), HeightData.Num());
				OutSummary = TEXT("Updated landscape height region.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_noise_height_region"),
			TEXT("MVP single-biome: fill a landscape height region with Perlin noise (heightmap coordinates). Use terrain_tile_plan + per-tile regions for chunked worlds."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("landscape"), FSololmcpSchemaBuilder::String()},
					{TEXT("min_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("min_y"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("max_x"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("max_y"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("seed"), FSololmcpSchemaBuilder::Integer()},
					{TEXT("frequency"), FSololmcpSchemaBuilder::Number()},
					{TEXT("amplitude"), FSololmcpSchemaBuilder::Number()}
				},
				{TEXT("landscape")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!GetLandscapeBoundsFromArguments(Landscape, Arguments, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				const int32 Seed = Arguments->HasTypedField<EJson::Number>(TEXT("seed")) ? Arguments->GetIntegerField(TEXT("seed")) : 1;
				const float Frequency = Arguments->HasTypedField<EJson::Number>(TEXT("frequency")) ? static_cast<float>(Arguments->GetNumberField(TEXT("frequency"))) : 0.05f;
				const float Amplitude = Arguments->HasTypedField<EJson::Number>(TEXT("amplitude")) ? static_cast<float>(Arguments->GetNumberField(TEXT("amplitude"))) : 0.35f;

				// FIX (v12): UE5 landscape edit-layer mode requires an active editing
				// layer or SetHeightData silently writes to nowhere. If no editing
				// layer is currently active, auto-activate the first available one.
				// This convenience needs the 5.6+ edit-layer API; the write below works
				// on every engine, so only the auto-activation is gated.
#if SOMOLMCP_HAS_LANDSCAPE_EDIT_LAYERS
				const TArray<ULandscapeEditLayerBase*> EditLayers = Landscape->GetEditLayers();
				if (EditLayers.Num() > 0)
				{
					FGuid CurrentLayerGuid = Landscape->GetEditingLayer();
					if (!CurrentLayerGuid.IsValid())
					{
						if (EditLayers.Num() > 0 && EditLayers[0])
						{
							Landscape->SetEditingLayer(EditLayers[0]->GetGuid());
						}
					}
				}
#endif

				TArray<uint16> HeightData;
				BuildPerlinNoiseHeightBuffer(MinX, MinY, MaxX, MaxY, Seed, Frequency, Amplitude, HeightData);

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeNoiseHeightRegion", "SOMOLMCP Noise Height Region"));
				Landscape->Modify();
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.SetHeightData(MinX, MinY, MaxX, MaxY, HeightData.GetData(), 0, true);
				TArray<uint16> ReadbackData;
				ReadbackData.SetNumZeroed(HeightData.Num());
				LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, ReadbackData.GetData(), 0);
				if (ReadbackData != HeightData)
				{
					OutError = TEXT("Landscape noise height region write failed immediate readback.");
					return false;
				}
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetNumberField(TEXT("updatedSamples"), HeightData.Num());
				OutStructured->SetNumberField(TEXT("seed"), Seed);
				OutStructured->SetNumberField(TEXT("frequency"), Frequency);
				OutStructured->SetNumberField(TEXT("amplitude"), Amplitude);
				OutSummary = TEXT("Filled landscape height region with noise.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_get_weight_region"),
			TEXT("Read back weight data from a landscape region."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("paint_layer"), FSololmcpSchemaBuilder::String()}, {TEXT("min_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("min_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape"), TEXT("paint_layer")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString PaintLayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("paint_layer"), PaintLayerName))
				{
					OutError = TEXT("Missing landscape or paint_layer.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeLayerInfoObject* PaintLayer = FindLandscapePaintLayerInfo(Landscape, PaintLayerName);
				if (!PaintLayer)
				{
					OutError = TEXT("Landscape paint layer was not found.");
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!GetLandscapeBoundsFromArguments(Landscape, Arguments, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				const int32 Width = MaxX - MinX + 1;
				const int32 Height = MaxY - MinY + 1;
				if (!ValidateLandscapeReadbackRegion(MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				TArray<uint8> WeightData;
				WeightData.SetNumZeroed(Width * Height);
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.GetWeightDataFast(PaintLayer, MinX, MinY, MaxX, MaxY, WeightData.GetData(), 0);
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetStringField(TEXT("paintLayer"), SOMOLMCP_LANDSCAPE_LAYER_NAME(PaintLayer).ToString());
				TArray<TSharedPtr<FJsonValue>> DataJson;
				for (const uint8 Value : WeightData)
				{
					DataJson.Add(MakeShared<FJsonValueNumber>(Value));
				}
				OutStructured->SetArrayField(TEXT("data"), DataJson);
				OutSummary = TEXT("Read back landscape paint weight data.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_set_weight_region"),
			TEXT("Write paint weight data into a landscape region."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("paint_layer"), FSololmcpSchemaBuilder::String()}, {TEXT("min_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("min_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("data"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer())}}, {TEXT("landscape"), TEXT("paint_layer"), TEXT("data")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString PaintLayerName;
				TArray<uint8> WeightData;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("paint_layer"), PaintLayerName) || !TryGetUInt8Array(Arguments, TEXT("data"), WeightData))
				{
					OutError = TEXT("Missing landscape, paint_layer or data.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeLayerInfoObject* PaintLayer = FindLandscapePaintLayerInfo(Landscape, PaintLayerName);
				if (!PaintLayer)
				{
					OutError = TEXT("Landscape paint layer was not found.");
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!GetLandscapeBoundsFromArguments(Landscape, Arguments, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				const int32 ExpectedSamples = (MaxX - MinX + 1) * (MaxY - MinY + 1);
				if (WeightData.Num() != ExpectedSamples)
				{
					OutError = TEXT("data length must equal region width * height.");
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeSetWeightRegion", "SOMOLMCP Set Landscape Weight Region"));
				Landscape->Modify();
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.SetAlphaData(PaintLayer, MinX, MinY, MaxX, MaxY, WeightData.GetData(), 0, ELandscapeLayerPaintingRestriction::None);
				TArray<uint8> ReadbackData;
				ReadbackData.SetNumZeroed(ExpectedSamples);
				LandscapeEdit.GetWeightDataFast(PaintLayer, MinX, MinY, MaxX, MaxY, ReadbackData.GetData(), 0);
				if (ReadbackData != WeightData)
				{
					OutError = TEXT("Landscape weight region write failed immediate readback.");
					return false;
				}
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetStringField(TEXT("paintLayer"), SOMOLMCP_LANDSCAPE_LAYER_NAME(PaintLayer).ToString());
				OutStructured->SetNumberField(TEXT("updatedSamples"), WeightData.Num());
				OutSummary = TEXT("Updated landscape paint weight region.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_fill_paint_layer"),
			TEXT("Fill a landscape paint layer in the current region."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("paint_layer"), FSololmcpSchemaBuilder::String()}, {TEXT("value"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape"), TEXT("paint_layer")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString PaintLayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("paint_layer"), PaintLayerName))
				{
					OutError = TEXT("Missing landscape or paint_layer.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeLayerInfoObject* PaintLayer = FindLandscapePaintLayerInfo(Landscape, PaintLayerName);
				if (!PaintLayer)
				{
					OutError = TEXT("Landscape paint layer was not found.");
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				const int32 FillValue = FMath::Clamp(Arguments->HasTypedField<EJson::Number>(TEXT("value")) ? Arguments->GetIntegerField(TEXT("value")) : 255, 0, 255);
				const FIntRect Bounds = Landscape->GetBoundingRect();
				const int32 Width = Bounds.Max.X - Bounds.Min.X + 1;
				const int32 Height = Bounds.Max.Y - Bounds.Min.Y + 1;
				TArray<uint8> WeightData;
				WeightData.Init(static_cast<uint8>(FillValue), Width * Height);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeFillPaintLayer", "SOMOLMCP Fill Landscape Paint Layer"));
				Landscape->Modify();
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.SetAlphaData(PaintLayer, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, WeightData.GetData(), 0, ELandscapeLayerPaintingRestriction::None);
				TArray<uint8> ReadbackData;
				ReadbackData.SetNumZeroed(WeightData.Num());
				LandscapeEdit.GetWeightDataFast(PaintLayer, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, ReadbackData.GetData(), 0);
				if (ReadbackData != WeightData)
				{
					OutError = TEXT("Landscape paint layer fill failed immediate readback.");
					return false;
				}
				OutStructured = LandscapeRegionResultToJson(Landscape, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y);
				OutStructured->SetStringField(TEXT("paintLayer"), SOMOLMCP_LANDSCAPE_LAYER_NAME(PaintLayer).ToString());
				OutStructured->SetNumberField(TEXT("fillValue"), FillValue);
				OutSummary = TEXT("Filled landscape paint layer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_replace_paint_layer"),
			TEXT("Replace one paint layer with another."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("source_paint_layer"), FSololmcpSchemaBuilder::String()}, {TEXT("target_paint_layer"), FSololmcpSchemaBuilder::String()}}, {TEXT("landscape"), TEXT("source_paint_layer"), TEXT("target_paint_layer")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				FString SourceLayerName;
				FString TargetLayerName;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !Arguments->TryGetStringField(TEXT("source_paint_layer"), SourceLayerName) || !Arguments->TryGetStringField(TEXT("target_paint_layer"), TargetLayerName))
				{
					OutError = TEXT("Missing landscape, source_paint_layer or target_paint_layer.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeLayerInfoObject* SourceLayer = FindLandscapePaintLayerInfo(Landscape, SourceLayerName);
				ULandscapeLayerInfoObject* TargetLayer = FindLandscapePaintLayerInfo(Landscape, TargetLayerName);
				if (!SourceLayer || !TargetLayer)
				{
					OutError = TEXT("Source or target landscape paint layer was not found.");
					return false;
				}
				if (SourceLayer == TargetLayer)
				{
					OutError = TEXT("source_paint_layer and target_paint_layer must be different.");
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				const FIntRect Bounds = Landscape->GetBoundingRect();
				const int32 Width = Bounds.Max.X - Bounds.Min.X + 1;
				const int32 Height = Bounds.Max.Y - Bounds.Min.Y + 1;
				TArray<uint8> SourceData;
				SourceData.SetNumZeroed(Width * Height);
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.GetWeightDataFast(SourceLayer, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, SourceData.GetData(), 0);
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapeReplacePaintLayer", "SOMOLMCP Replace Landscape Paint Layer"));
				Landscape->Modify();
				LandscapeEdit.SetAlphaData(TargetLayer, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, SourceData.GetData(), 0, ELandscapeLayerPaintingRestriction::None);
				TArray<uint8> ClearData;
				ClearData.Init(0, SourceData.Num());
				LandscapeEdit.SetAlphaData(SourceLayer, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, ClearData.GetData(), 0, ELandscapeLayerPaintingRestriction::None);
				TArray<uint8> TargetReadback;
				TargetReadback.SetNumZeroed(SourceData.Num());
				LandscapeEdit.GetWeightDataFast(TargetLayer, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, TargetReadback.GetData(), 0);
				TArray<uint8> SourceReadback;
				SourceReadback.SetNumZeroed(SourceData.Num());
				LandscapeEdit.GetWeightDataFast(SourceLayer, Bounds.Min.X, Bounds.Min.Y, Bounds.Max.X, Bounds.Max.Y, SourceReadback.GetData(), 0);
				if (TargetReadback != SourceData || SourceReadback != ClearData)
				{
					OutError = TEXT("Landscape paint layer replace failed immediate readback.");
					return false;
				}
				OutStructured = LandscapePaintLayersToJson(Landscape);
				OutSummary = TEXT("Replaced landscape paint layer contents.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_sculpt_brush"),
			TEXT("Apply a sculpt brush stroke to a landscape through the shared BrushKernel stroke envelope (BR-01: profile/stroke envelope, target lock, pre-snapshot and exact writer receipt)."),
			BrushStrokeEnvelopeObjectSchema({}, {}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				TSharedPtr<FJsonObject> CenterObject;
				FSololmcpBrushStrokeProfile Profile;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !TryGetObjectField(Arguments, TEXT("center"), CenterObject) || !Arguments->TryGetNumberField(TEXT("radius"), Profile.Radius))
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("missing_required_parameter"), TEXT("Missing landscape, center or radius."));
				}
				FVector Center;
				if (!FSololmcpEditorServices::JsonToVector(CenterObject, Center) || Center.ContainsNaN())
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_center_vector"), TEXT("center must be a finite vector object."));
				}
				Profile.Strength = Arguments->HasTypedField<EJson::Number>(TEXT("strength")) ? Arguments->GetNumberField(TEXT("strength")) : 128.0;
				if (!FMath::IsFinite(Profile.Strength) || Profile.Strength < 0.0 || Profile.Strength > static_cast<double>(TNumericLimits<uint16>::Max()))
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_brush_strength"), TEXT("strength must be a finite value in [0, 65535]."));
				}
				if (!ParseSololmcpBrushStrokeEnvelope(Arguments, Profile, OutStructured, OutError))
				{
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("landscape_not_found"), OutError.IsEmpty() ? TEXT("Landscape was not found.") : OutError);
				}
				const double Strength = Profile.Strength;
				return ApplyLandscapeBrushStrokeEnvelope(Landscape, Center, Profile, TEXT("landscape_sculpt_brush"), TEXT("sculpt"),
					[Strength](int32, int32, uint16 Current, float Envelope)
					{
						const int32 Delta = FMath::RoundToInt(Strength * Envelope);
						return static_cast<uint16>(FMath::Clamp<int32>(Current + Delta, 0, TNumericLimits<uint16>::Max()));
					},
					OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_smooth_brush"),
			TEXT("Apply a smooth brush stroke to a landscape through the shared radius/falloff/pressure/mask/symmetry schema with a receipt-gated commit (BR-02)."),
			BrushStrokeEnvelopeObjectSchema({}, {}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				TSharedPtr<FJsonObject> CenterObject;
				FSololmcpBrushStrokeProfile Profile;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !TryGetObjectField(Arguments, TEXT("center"), CenterObject) || !Arguments->TryGetNumberField(TEXT("radius"), Profile.Radius))
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("missing_required_parameter"), TEXT("Missing landscape, center or radius."));
				}
				FVector Center;
				if (!FSololmcpEditorServices::JsonToVector(CenterObject, Center) || Center.ContainsNaN())
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_center_vector"), TEXT("center must be a finite vector object."));
				}
				Profile.Strength = Arguments->HasTypedField<EJson::Number>(TEXT("strength")) ? Arguments->GetNumberField(TEXT("strength")) : 0.5;
				if (!FMath::IsFinite(Profile.Strength) || Profile.Strength < 0.0 || Profile.Strength > 1.0)
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_brush_strength"), TEXT("strength must be a finite value in [0, 1]."));
				}
				if (!ParseSololmcpBrushStrokeEnvelope(Arguments, Profile, OutStructured, OutError))
				{
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("landscape_not_found"), OutError.IsEmpty() ? TEXT("Landscape was not found.") : OutError);
				}
				const double Strength = Profile.Strength;
				return ApplyLandscapeBrushStrokeEnvelope(Landscape, Center, Profile, TEXT("landscape_smooth_brush"), TEXT("smooth"),
					[Strength, Landscape](int32 GridX, int32 GridY, uint16 Current, float Envelope)
					{
						ULandscapeInfo* LandscapeInfo = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
						if (!LandscapeInfo)
						{
							return Current;
						}
						FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
						uint32 Sum = 0;
						int32 Count = 0;
						for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
						{
							for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
							{
								uint16 Sample = Current;
								LandscapeEdit.GetHeightDataFast(GridX + OffsetX, GridY + OffsetY, GridX + OffsetX, GridY + OffsetY, &Sample, 0);
								Sum += Sample;
								++Count;
							}
						}
						const uint16 Average = Count > 0 ? static_cast<uint16>(Sum / Count) : Current;
						const float Blend = FMath::Clamp(static_cast<float>(Strength) * Envelope, 0.0f, 1.0f);
						return static_cast<uint16>(FMath::Lerp(static_cast<float>(Current), static_cast<float>(Average), Blend));
					},
					OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_flatten_brush"),
			TEXT("Apply a flatten brush stroke to a landscape; flatten target height and projection/filter parameters are bound to the shared stroke snapshot and rollback contract (BR-03)."),
			BrushStrokeEnvelopeObjectSchema(
				{{TEXT("target_height"), FSololmcpSchemaBuilder::Number(TEXT("Raw uint16 height to flatten toward."), 0.0, 65535.0)}},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				TSharedPtr<FJsonObject> CenterObject;
				FSololmcpBrushStrokeProfile Profile;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !TryGetObjectField(Arguments, TEXT("center"), CenterObject) || !Arguments->TryGetNumberField(TEXT("radius"), Profile.Radius))
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("missing_required_parameter"), TEXT("Missing landscape, center or radius."));
				}
				FVector Center;
				if (!FSololmcpEditorServices::JsonToVector(CenterObject, Center) || Center.ContainsNaN())
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_center_vector"), TEXT("center must be a finite vector object."));
				}
				const int32 TargetHeight = Arguments->HasTypedField<EJson::Number>(TEXT("target_height")) ? Arguments->GetIntegerField(TEXT("target_height")) : 32768;
				if (TargetHeight < 0 || TargetHeight > TNumericLimits<uint16>::Max())
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_target_height"), TEXT("target_height must be a raw uint16 height in [0, 65535]."));
				}
				Profile.Strength = Arguments->HasTypedField<EJson::Number>(TEXT("strength")) ? Arguments->GetNumberField(TEXT("strength")) : 1.0;
				if (!FMath::IsFinite(Profile.Strength) || Profile.Strength < 0.0 || Profile.Strength > 1.0)
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_brush_strength"), TEXT("strength must be a finite value in [0, 1]."));
				}
				if (!ParseSololmcpBrushStrokeEnvelope(Arguments, Profile, OutStructured, OutError))
				{
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("landscape_not_found"), OutError.IsEmpty() ? TEXT("Landscape was not found.") : OutError);
				}
				const double Strength = Profile.Strength;
				return ApplyLandscapeBrushStrokeEnvelope(Landscape, Center, Profile, TEXT("landscape_flatten_brush"), TEXT("flatten"),
					[TargetHeight, Strength](int32, int32, uint16 Current, float Envelope)
					{
						const float Blend = FMath::Clamp(static_cast<float>(Strength) * Envelope, 0.0f, 1.0f);
						return static_cast<uint16>(FMath::Lerp(static_cast<float>(Current), static_cast<float>(TargetHeight), Blend));
					},
					OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_noise_brush"),
			TEXT("Apply a deterministic-seed noise brush stroke to a landscape through the shared spacing/falloff filters, target lock and save/readback writer receipt (BR-04)."),
			BrushStrokeEnvelopeObjectSchema(
				{{TEXT("seed"), FSololmcpSchemaBuilder::Integer(TEXT("Deterministic noise seed; derived from the stroke center when omitted."))}},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				TSharedPtr<FJsonObject> CenterObject;
				FSololmcpBrushStrokeProfile Profile;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || !TryGetObjectField(Arguments, TEXT("center"), CenterObject) || !Arguments->TryGetNumberField(TEXT("radius"), Profile.Radius))
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("missing_required_parameter"), TEXT("Missing landscape, center or radius."));
				}
				FVector Center;
				if (!FSololmcpEditorServices::JsonToVector(CenterObject, Center) || Center.ContainsNaN())
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_center_vector"), TEXT("center must be a finite vector object."));
				}
				Profile.Strength = Arguments->HasTypedField<EJson::Number>(TEXT("strength")) ? Arguments->GetNumberField(TEXT("strength")) : 128.0;
				if (!FMath::IsFinite(Profile.Strength) || Profile.Strength < 0.0 || Profile.Strength > static_cast<double>(TNumericLimits<uint16>::Max()))
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("invalid_brush_strength"), TEXT("strength must be a finite value in [0, 65535]."));
				}
				if (!ParseSololmcpBrushStrokeEnvelope(Arguments, Profile, OutStructured, OutError))
				{
					return false;
				}
				Profile.Seed = Arguments->HasTypedField<EJson::Number>(TEXT("seed"))
					? Arguments->GetIntegerField(TEXT("seed"))
					: FMath::RoundToInt(Center.X + Center.Y + Profile.Radius);
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return SololmcpBrushStrokeFail(OutStructured, OutError, TEXT("landscape_not_found"), OutError.IsEmpty() ? TEXT("Landscape was not found.") : OutError);
				}
				const double Strength = Profile.Strength;
				const int32 Seed = Profile.Seed;
				return ApplyLandscapeBrushStrokeEnvelope(Landscape, Center, Profile, TEXT("landscape_noise_brush"), TEXT("noise"),
					[Strength, Seed](int32 GridX, int32 GridY, uint16 Current, float Envelope)
					{
						FRandomStream RandomStream;
						const uint32 CellHash = static_cast<uint32>(Seed) * 0x9E3779B1u
							+ static_cast<uint32>(GridX) * 92821u
							+ static_cast<uint32>(GridY) * 68917u;
						RandomStream.Initialize(static_cast<int32>(CellHash));
						const int32 Delta = FMath::RoundToInt(RandomStream.FRandRange(-Strength, Strength) * Envelope);
						return static_cast<uint16>(FMath::Clamp<int32>(Current + Delta, 0, TNumericLimits<uint16>::Max()));
					},
					OutStructured, OutSummary, OutError);
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_copy_region"),
			TEXT("Copy a region from a landscape height/weight map."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("min_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("min_y"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LandscapeId;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId))
				{
					OutError = TEXT("Missing landscape.");
					return false;
				}
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
				if (!GetLandscapeBoundsFromArguments(Landscape, Arguments, MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				const int32 Width = MaxX - MinX + 1;
				const int32 Height = MaxY - MinY + 1;
				if (!ValidateLandscapeReadbackRegion(MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				GLandscapeClipboard.HeightData.SetNumZeroed(Width * Height);
				GLandscapeClipboard.Width = Width;
				GLandscapeClipboard.Height = Height;
				GLandscapeClipboard.LandscapePath = Landscape->GetPathName();
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, GLandscapeClipboard.HeightData.GetData(), 0);
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetNumberField(TEXT("copiedSamples"), GLandscapeClipboard.HeightData.Num());
				OutSummary = TEXT("Copied landscape region into clipboard.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("landscape_paste_region"),
			TEXT("Paste a copied region back into a landscape."),
			FSololmcpSchemaBuilder::Object({{TEXT("landscape"), FSololmcpSchemaBuilder::String()}, {TEXT("target_x"), FSololmcpSchemaBuilder::Integer()}, {TEXT("target_y"), FSololmcpSchemaBuilder::Integer()}}, {TEXT("landscape"), TEXT("target_x"), TEXT("target_y")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (GLandscapeClipboard.HeightData.Num() == 0 || GLandscapeClipboard.Width <= 0 || GLandscapeClipboard.Height <= 0)
				{
					OutError = TEXT("Landscape clipboard is empty.");
					return false;
				}
				FString LandscapeId;
				int32 TargetX = 0;
				int32 TargetY = 0;
				if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) ||
					!Arguments->HasTypedField<EJson::Number>(TEXT("target_x")) ||
					!Arguments->HasTypedField<EJson::Number>(TEXT("target_y")))
				{
					OutError = TEXT("Missing landscape, numeric target_x or numeric target_y.");
					return false;
				}
				TargetX = Arguments->GetIntegerField(TEXT("target_x"));
				TargetY = Arguments->GetIntegerField(TEXT("target_y"));
				ALandscape* Landscape = ResolveLandscape(Context.Services, LandscapeId, OutError);
				if (!Landscape)
				{
					return false;
				}
				ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
				if (!LandscapeInfo)
				{
					OutError = TEXT("Landscape info is unavailable.");
					return false;
				}
				const FIntRect Bounds = Landscape->GetBoundingRect();
				const int32 MinX = FMath::Clamp(TargetX, Bounds.Min.X, Bounds.Max.X);
				const int32 MinY = FMath::Clamp(TargetY, Bounds.Min.Y, Bounds.Max.Y);
				const int32 MaxX = FMath::Clamp(TargetX + GLandscapeClipboard.Width - 1, Bounds.Min.X, Bounds.Max.X);
				const int32 MaxY = FMath::Clamp(TargetY + GLandscapeClipboard.Height - 1, Bounds.Min.Y, Bounds.Max.Y);
				const int32 EffectiveWidth = MaxX - MinX + 1;
				const int32 EffectiveHeight = MaxY - MinY + 1;
				if (!ValidateLandscapeReadbackRegion(MinX, MinY, MaxX, MaxY, OutError))
				{
					return false;
				}
				TArray<uint16> PasteData;
				PasteData.SetNumZeroed(EffectiveWidth * EffectiveHeight);
				for (int32 Y = 0; Y < EffectiveHeight; ++Y)
				{
					for (int32 X = 0; X < EffectiveWidth; ++X)
					{
						PasteData[Y * EffectiveWidth + X] = GLandscapeClipboard.HeightData[Y * GLandscapeClipboard.Width + X];
					}
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LandscapePasteRegion", "SOMOLMCP Paste Landscape Region"));
				Landscape->Modify();
				FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
				LandscapeEdit.SetHeightData(MinX, MinY, MaxX, MaxY, PasteData.GetData(), 0, true);
				TArray<uint16> ReadbackData;
				ReadbackData.SetNumZeroed(PasteData.Num());
				LandscapeEdit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, ReadbackData.GetData(), 0);
				if (ReadbackData != PasteData)
				{
					OutError = TEXT("Landscape paste region failed immediate readback.");
					return false;
				}
				OutStructured = LandscapeRegionResultToJson(Landscape, MinX, MinY, MaxX, MaxY);
				OutStructured->SetNumberField(TEXT("pastedSamples"), PasteData.Num());
				OutSummary = TEXT("Pasted landscape clipboard region.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_set_folder"),
			TEXT("Set the folder path for an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}, {TEXT("folder_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor"), TEXT("folder_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				FString FolderPath;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetStringField(TEXT("folder_path"), FolderPath))
				{
					OutError = TEXT("Missing actor or folder_path.");
					return false;
				}
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor)
				{
					return false;
				}
				Actor->SetFolderPath(*FolderPath);
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutSummary = TEXT("Updated actor folder path.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("actor_clear_folder"),
			TEXT("Clear the folder path for an actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String()}}, {TEXT("actor")}),

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
				Actor->SetFolderPath(NAME_None);
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutSummary = TEXT("Cleared actor folder path.");
				return true;
			}
		, nullptr
		, 5
		});


		auto RegisterWorldPythonTool = [&RegisterPythonTool](const FString& ToolName, const FString& Description, const TSharedRef<FJsonObject>& Schema)
		{
			RegisterPythonTool(ToolName, Description, Schema,
				[ToolName](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, FString&)
				{
					const FString ArgumentsJson = JsonObjectToString(Arguments);
					return FString::Printf(
						TEXT("import unreal\n")
						TEXT("import json\n")
						TEXT("tool_name = %s\n")
						TEXT("args = json.loads(%s)\n")
						TEXT("world = unreal.EditorLevelLibrary.get_editor_world()\n")
						TEXT("data_layer_subsystem = unreal.get_editor_subsystem(unreal.DataLayerEditorSubsystem) if hasattr(unreal, 'DataLayerEditorSubsystem') else None\n")
						TEXT("actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)\n")
						TEXT("def resolve_actor(actor_id):\n")
						TEXT("    for actor in actor_subsystem.get_all_level_actors():\n")
						TEXT("        if actor.get_actor_label() == actor_id or actor.get_name() == actor_id or actor.get_path_name() == actor_id:\n")
						TEXT("            return actor\n")
						TEXT("    return None\n")
						TEXT("def vector_from(value):\n")
						TEXT("    value = value or {}\n")
						TEXT("    return unreal.Vector(float(value.get('x', 0.0)), float(value.get('y', 0.0)), float(value.get('z', 0.0)))\n")
						TEXT("world_partition = world.get_world_partition() if world is not None and hasattr(world, 'get_world_partition') else None\n")
						TEXT("if tool_name == 'world_partition_list_cells':\n")
						TEXT("    if world_partition is None:\n")
						TEXT("        raise RuntimeError('World partition is unavailable')\n")
						TEXT("    if hasattr(world_partition, 'get_all_runtime_cells'):\n")
						TEXT("        for cell in list(world_partition.get_all_runtime_cells()):\n")
						TEXT("            unreal.log('cell=' + cell.get_name())\n")
						TEXT("elif tool_name in ('world_partition_load_region', 'world_partition_unload_region', 'world_partition_load_large_region', 'world_partition_unload_large_region'):\n")
						TEXT("    if world_partition is None:\n")
						TEXT("        raise RuntimeError('World partition is unavailable')\n")
						TEXT("    region_min = vector_from(args.get('min'))\n")
						TEXT("    region_max = vector_from(args.get('max'))\n")
						TEXT("    if hasattr(world_partition, 'load_from_editor_cells') and 'load' in tool_name:\n")
						TEXT("        world_partition.load_from_editor_cells(region_min, region_max)\n")
						TEXT("    elif hasattr(world_partition, 'unload_from_editor_cells'):\n")
						TEXT("        world_partition.unload_from_editor_cells(region_min, region_max)\n")
						TEXT("    else:\n")
						TEXT("        unreal.log('World partition region command fallback: ' + tool_name)\n")
						TEXT("elif tool_name == 'world_partition_list_streaming_sources':\n")
						TEXT("    if world_partition is not None and hasattr(world_partition, 'get_streaming_sources'):\n")
						TEXT("        for source in list(world_partition.get_streaming_sources()):\n")
						TEXT("            unreal.log('streaming_source=' + str(source))\n")
						TEXT("elif tool_name == 'world_partition_add_streaming_source':\n")
						TEXT("    if world_partition is not None and hasattr(world_partition, 'add_editor_streaming_source'):\n")
						TEXT("        world_partition.add_editor_streaming_source(args.get('name', ''), vector_from(args.get('location')), float(args.get('radius', 0.0)))\n")
						TEXT("    else:\n")
						TEXT("        unreal.log('World partition add streaming source fallback')\n")
						TEXT("elif tool_name == 'world_partition_remove_streaming_source':\n")
						TEXT("    if world_partition is not None and hasattr(world_partition, 'remove_editor_streaming_source'):\n")
						TEXT("        world_partition.remove_editor_streaming_source(args.get('name', ''))\n")
						TEXT("elif tool_name == 'world_partition_list_actor_descs':\n")
						TEXT("    if world_partition is not None and hasattr(world_partition, 'get_actor_desc_container'):\n")
						TEXT("        container = world_partition.get_actor_desc_container()\n")
						TEXT("        if hasattr(container, 'get_actor_descs'):\n")
						TEXT("            for desc in list(container.get_actor_descs()):\n")
						TEXT("                unreal.log('actor_desc=' + str(desc))\n")
						TEXT("elif tool_name == 'world_partition_list_hlods':\n")
						TEXT("    for actor in actor_subsystem.get_all_level_actors():\n")
						TEXT("        if 'HLOD' in actor.get_class().get_name() or 'HLOD' in actor.get_name():\n")
						TEXT("            unreal.log('hlod=' + actor.get_path_name())\n")
						TEXT("elif tool_name == 'world_partition_list_content_bundles':\n")
						TEXT("    if world_partition is not None and hasattr(world_partition, 'get_content_bundles'):\n")
						TEXT("        for bundle in list(world_partition.get_content_bundles()):\n")
						TEXT("            unreal.log('content_bundle=' + str(bundle))\n")
						TEXT("elif tool_name in ('world_partition_validate', 'world_partition_diagnose'):\n")
						TEXT("    if world_partition is None:\n")
						TEXT("        raise RuntimeError('World partition is unavailable')\n")
						TEXT("    if hasattr(world_partition, 'validate_generated_state'):\n")
						TEXT("        world_partition.validate_generated_state()\n")
						TEXT("    unreal.log('world_partition_check=' + tool_name)\n")
						TEXT("elif tool_name == 'data_layer_assign_actor':\n")
						TEXT("    actor = resolve_actor(args.get('actor', ''))\n")
						TEXT("    if actor is None or data_layer_subsystem is None:\n")
						TEXT("        raise RuntimeError('Actor or DataLayer subsystem is unavailable')\n")
						TEXT("    data_layer = data_layer_subsystem.get_data_layer(args.get('data_layer_name', ''))\n")
						TEXT("    data_layer_subsystem.add_actor_to_data_layer(actor, data_layer)\n")
						TEXT("elif tool_name == 'data_layer_remove_actor':\n")
						TEXT("    actor = resolve_actor(args.get('actor', ''))\n")
						TEXT("    if actor is None or data_layer_subsystem is None:\n")
						TEXT("        raise RuntimeError('Actor or DataLayer subsystem is unavailable')\n")
						TEXT("    data_layer = data_layer_subsystem.get_data_layer(args.get('data_layer_name', ''))\n")
						TEXT("    data_layer_subsystem.remove_actor_from_data_layer(actor, data_layer)\n")
						TEXT("elif tool_name == 'data_layer_batch_set_visibility':\n")
						TEXT("    if data_layer_subsystem is None:\n")
						TEXT("        raise RuntimeError('DataLayer subsystem is unavailable')\n")
						TEXT("    for name in list(args.get('data_layer_names', [])):\n")
						TEXT("        data_layer = data_layer_subsystem.get_data_layer(name)\n")
						TEXT("        data_layer_subsystem.set_data_layer_visibility(data_layer, bool(args.get('visible', True)))\n")
						TEXT("elif tool_name == 'data_layer_batch_set_loaded_in_editor':\n")
						TEXT("    if data_layer_subsystem is None:\n")
						TEXT("        raise RuntimeError('DataLayer subsystem is unavailable')\n")
						TEXT("    for name in list(args.get('data_layer_names', [])):\n")
						TEXT("        data_layer = data_layer_subsystem.get_data_layer(name)\n")
						TEXT("        data_layer_subsystem.set_data_layer_is_loaded_in_editor(data_layer, bool(args.get('loaded', True)), False)\n")
						TEXT("elif tool_name == 'data_layer_list_actors':\n")
						TEXT("    if data_layer_subsystem is None:\n")
						TEXT("        raise RuntimeError('DataLayer subsystem is unavailable')\n")
						TEXT("    data_layer = data_layer_subsystem.get_data_layer(args.get('data_layer_name', ''))\n")
						TEXT("    for actor in list(data_layer_subsystem.get_actors_from_data_layer(data_layer)):\n")
						TEXT("        unreal.log('actor=' + actor.get_path_name())\n")
						TEXT("else:\n")
						TEXT("    raise RuntimeError('Unsupported world/data layer tool')\n"),
						*PythonQuote(ToolName),
						*PythonQuote(ArgumentsJson));
				});
		};
		Registry.Register({
			TEXT("world_partition_list_cells"),
			TEXT("List cells in the current world partition."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("Editor is unavailable.");
					return false;
				}

				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					OutError = TEXT("Editor world is unavailable.");
					return false;
				}

				// FWorldPartitionUtils::FSimulateCookedSession is declared on 5.3 but carries no
				// ENGINE_API export, so a plugin cannot link against it there. Refused with a
				// reason rather than silently reporting an empty cell list.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				FWorldPartitionUtils::FSimulateCookedSession Session(World);
				if (!Session.IsValid())
				{
					OutError = TEXT("Failed to create world partition cooked simulation session.");
					return false;
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> CellsJson;
				if (!Session.ForEachStreamingCells([&CellsJson](const IWorldPartitionCell* Cell)
				{
					CellsJson.Add(MakeShared<FJsonValueObject>(RuntimeCellToJson(Cell)));
				}))
				{
					OutError = TEXT("Failed to enumerate world partition cells.");
					return false;
				}

				Result->SetArrayField(TEXT("cells"), CellsJson);
				Result->SetNumberField(TEXT("count"), CellsJson.Num());
				OutStructured = Result;
				OutSummary = TEXT("Listed world partition cells.");
				return true;
#else
				OutError = TEXT("NOT_AVAILABLE_ON_ENGINE: enumerating cooked streaming cells requires "
					"UE 5.4 or later; FSimulateCookedSession is not exported on this engine version.");
				return false;
#endif
}
, nullptr
, 5
});
		Registry.Register({
			TEXT("world_partition_load_region"),
			TEXT("Load a world partition region."),
			FSololmcpSchemaBuilder::Object({{TEXT("min"), VectorSchema()}, {TEXT("max"), VectorSchema()}}, {TEXT("min"), TEXT("max")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				if (!WorldPartition)
				{
					return false;
				}

				FBox RegionBox(ForceInit);
				if (!TryGetRegionBoxFromArguments(Arguments, RegionBox, OutError))
				{
					return false;
				}

				UWorld* World = GEditor->GetEditorWorldContext().World();
				UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter = WorldPartition->CreateEditorLoaderAdapter<FLoaderAdapterShape>(World, RegionBox, TEXT("SOMOLMCP Loaded Region"));
				EditorLoaderAdapter->GetLoaderAdapter()->SetUserCreated(true);
				EditorLoaderAdapter->GetLoaderAdapter()->Load();
				const TOptional<FString> LoaderLabel = EditorLoaderAdapter->GetLoaderAdapter()->GetLabel();

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetObjectField(TEXT("region"), BoxToJson(RegionBox));
				OutStructured->SetBoolField(TEXT("loaded"), EditorLoaderAdapter->GetLoaderAdapter()->IsLoaded());
				OutStructured->SetStringField(TEXT("label"), LoaderLabel.IsSet() ? LoaderLabel.GetValue() : FString(TEXT("SOMOLMCP Loaded Region")));
				OutSummary = TEXT("Loaded world partition region.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_unload_region"),
			TEXT("Unload a world partition region."),
			FSololmcpSchemaBuilder::Object({{TEXT("min"), VectorSchema()}, {TEXT("max"), VectorSchema()}}, {TEXT("min"), TEXT("max")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				if (!WorldPartition)
				{
					return false;
				}

				FBox RegionBox(ForceInit);
				if (!TryGetRegionBoxFromArguments(Arguments, RegionBox, OutError))
				{
					return false;
				}

				int32 ReleasedCount = 0;
				TArray<UWorldPartitionEditorLoaderAdapter*> ToRelease;
				for (UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter : WorldPartition->GetRegisteredEditorLoaderAdapters())
				{
					if (!EditorLoaderAdapter || !EditorLoaderAdapter->GetLoaderAdapter())
					{
						continue;
					}

					const TOptional<FBox> BoundingBox = EditorLoaderAdapter->GetLoaderAdapter()->GetBoundingBox();
					if (BoundingBox.IsSet() && BoxesMatch(BoundingBox.GetValue(), RegionBox))
					{
						EditorLoaderAdapter->GetLoaderAdapter()->Unload();
						ToRelease.Add(EditorLoaderAdapter);
					}
				}

				for (UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter : ToRelease)
				{
					WorldPartition->ReleaseEditorLoaderAdapter(EditorLoaderAdapter);
					++ReleasedCount;
				}

				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetObjectField(TEXT("region"), BoxToJson(RegionBox));
				OutStructured->SetNumberField(TEXT("releasedAdapters"), ReleasedCount);
				OutSummary = ReleasedCount > 0 ? TEXT("Unloaded world partition region.") : TEXT("No matching world partition region loader was found.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_list_streaming_sources"),
			TEXT("List world partition streaming sources."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				if (!WorldPartition)
				{
					return false;
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> SourcesJson;
				for (const FWorldPartitionStreamingSource& Source : WorldPartition->GetStreamingSources())
				{
					SourcesJson.Add(MakeShared<FJsonValueObject>(StreamingSourceToJson(Source)));
				}
				Result->SetArrayField(TEXT("streamingSources"), SourcesJson);
				Result->SetNumberField(TEXT("count"), SourcesJson.Num());
				OutStructured = Result;
				OutSummary = TEXT("Listed world partition streaming sources.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_add_streaming_source"),
			TEXT("Add a managed world partition streaming source."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("name"), FSololmcpSchemaBuilder::String()},
					{TEXT("source_id"), FSololmcpSchemaBuilder::String()},
					{TEXT("location"), VectorSchema()},
					{TEXT("rotation"), RotatorSchema()},
					{TEXT("target_state"), FSololmcpSchemaBuilder::String()},
					{TEXT("block_on_slow_loading"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("priority"), FSololmcpSchemaBuilder::String()},
					{TEXT("remote"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("force_2d"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("target_behavior"), FSololmcpSchemaBuilder::String()},
					{TEXT("target_grids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}
				},
				{TEXT("name"), TEXT("location")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Name;
				TSharedPtr<FJsonObject> LocationObject;
				if (!Arguments->TryGetStringField(TEXT("name"), Name) || !TryGetObjectField(Arguments, TEXT("location"), LocationObject))
				{
					OutError = TEXT("Missing name or location.");
					return false;
				}

				FVector Location = FVector::ZeroVector;
				if (!FSololmcpEditorServices::JsonToVector(LocationObject, Location))
				{
					OutError = TEXT("location must be a vector object.");
					return false;
				}

				FRotator Rotation = FRotator::ZeroRotator;
				if (TSharedPtr<FJsonObject> RotationObject; TryGetObjectField(Arguments, TEXT("rotation"), RotationObject))
				{
					double Pitch = 0.0;
					double Yaw = 0.0;
					double Roll = 0.0;
					RotationObject->TryGetNumberField(TEXT("pitch"), Pitch);
					RotationObject->TryGetNumberField(TEXT("yaw"), Yaw);
					RotationObject->TryGetNumberField(TEXT("roll"), Roll);
					Rotation = FRotator(Pitch, Yaw, Roll);
				}

				EStreamingSourceTargetState TargetState = EStreamingSourceTargetState::Activated;
				if (FString TargetStateString; Arguments->TryGetStringField(TEXT("target_state"), TargetStateString))
				{
					if (!TryParseStreamingSourceTargetState(TargetStateString, TargetState))
					{
						OutError = TEXT("target_state must be 'loaded' or 'activated'.");
						return false;
					}
				}

				EStreamingSourcePriority Priority = EStreamingSourcePriority::Normal;
				if (FString PriorityString; Arguments->TryGetStringField(TEXT("priority"), PriorityString))
				{
					if (!TryParseStreamingSourcePriority(PriorityString, Priority))
					{
						OutError = TEXT("priority must be highest, high, normal, low, or lowest.");
						return false;
					}
				}

				EStreamingSourceTargetBehavior TargetBehavior = EStreamingSourceTargetBehavior::Include;
				if (FString TargetBehaviorString; Arguments->TryGetStringField(TEXT("target_behavior"), TargetBehaviorString))
				{
					if (!TryParseStreamingSourceTargetBehavior(TargetBehaviorString, TargetBehavior))
					{
						OutError = TEXT("target_behavior must be include or exclude.");
						return false;
					}
				}

				const bool bBlockOnSlowLoading = Arguments->HasTypedField<EJson::Boolean>(TEXT("block_on_slow_loading")) ? Arguments->GetBoolField(TEXT("block_on_slow_loading")) : false;
				const bool bRemote = Arguments->HasTypedField<EJson::Boolean>(TEXT("remote")) ? Arguments->GetBoolField(TEXT("remote")) : false;
				const bool bForce2D = Arguments->HasTypedField<EJson::Boolean>(TEXT("force_2d")) ? Arguments->GetBoolField(TEXT("force_2d")) : false;

				FString SourceId;
				Arguments->TryGetStringField(TEXT("source_id"), SourceId);
				if (SourceId.IsEmpty())
				{
					SourceId = Name;
				}

				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				UWorldPartitionSubsystem* WorldPartitionSubsystem = GetCurrentEditorWorldPartitionSubsystem(OutError);
				if (!WorldPartition || !WorldPartitionSubsystem)
				{
					return false;
				}

				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (FindManagedStreamingSourceProvider(World, SourceId))
				{
					OutError = TEXT("A managed streaming source with the same source_id already exists.");
					return false;
				}

				TSharedPtr<FSololmcpManagedStreamingSourceProvider> Provider = MakeShared<FSololmcpManagedStreamingSourceProvider>();
				Provider->SourceId = SourceId;
				Provider->World = World;
				Provider->Source = FWorldPartitionStreamingSource(FName(*Name), Location, Rotation, TargetState, bBlockOnSlowLoading, Priority, bRemote);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				Provider->Source.bForce2D = bForce2D;
#else
				(void)bForce2D; // bForce2D is 5.5+; the rest of the source still applies.
#endif
				Provider->Source.TargetBehavior = TargetBehavior;

				TArray<FString> TargetGrids;
				if (TryGetStringArray(Arguments, TEXT("target_grids"), TargetGrids))
				{
					for (const FString& TargetGrid : TargetGrids)
					{
						if (!TargetGrid.IsEmpty())
						{
							Provider->Source.TargetGrids.Add(FName(*TargetGrid));
						}
					}
				}
				Provider->Source.UpdateHash();

				WorldPartitionSubsystem->RegisterStreamingSourceProvider(Provider.Get());
				GetManagedStreamingSourceProviders().Add(Provider);

				OutStructured = StreamingSourceToJson(Provider->Source);
				OutStructured->SetStringField(TEXT("sourceId"), Provider->SourceId);
				OutStructured->SetBoolField(TEXT("registered"), WorldPartitionSubsystem->IsStreamingSourceProviderRegistered(Provider.Get()));
				OutSummary = TEXT("Added managed world partition streaming source.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_remove_streaming_source"),
			TEXT("Remove a managed world partition streaming source."),
			FSololmcpSchemaBuilder::Object({{TEXT("name"), FSololmcpSchemaBuilder::String()}, {TEXT("source_id"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SourceIdentifier;
				Arguments->TryGetStringField(TEXT("source_id"), SourceIdentifier);
				if (SourceIdentifier.IsEmpty())
				{
					Arguments->TryGetStringField(TEXT("name"), SourceIdentifier);
				}
				if (SourceIdentifier.IsEmpty())
				{
					OutError = TEXT("Missing source_id or name.");
					return false;
				}

				UWorldPartitionSubsystem* WorldPartitionSubsystem = GetCurrentEditorWorldPartitionSubsystem(OutError);
				if (!WorldPartitionSubsystem)
				{
					return false;
				}

				UWorld* World = GEditor->GetEditorWorldContext().World();
				TSharedPtr<FSololmcpManagedStreamingSourceProvider> Provider = FindManagedStreamingSourceProvider(World, SourceIdentifier);
				if (!Provider.IsValid())
				{
					OutError = TEXT("Managed streaming source was not found.");
					return false;
				}

				const bool bRemoved = WorldPartitionSubsystem->UnregisterStreamingSourceProvider(Provider.Get());
				GetManagedStreamingSourceProviders().RemoveAll([&Provider](const TSharedPtr<FSololmcpManagedStreamingSourceProvider>& Candidate)
				{
					return Candidate == Provider;
				});


				OutStructured = MakeShared<FJsonObject>();
				OutStructured->SetStringField(TEXT("sourceId"), Provider->SourceId);
				OutStructured->SetStringField(TEXT("name"), Provider->Source.Name.ToString());
				OutStructured->SetBoolField(TEXT("removed"), bRemoved);
				OutSummary = bRemoved ? TEXT("Removed managed world partition streaming source.") : TEXT("Managed world partition streaming source was already unregistered.");
				return true;
}
, nullptr
, 5
});
		Registry.Register({
			TEXT("world_partition_list_actor_descs"),
			TEXT("List actor descriptors in world partition."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				if (!WorldPartition)
				{
					return false;
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> ActorDescsJson;
				SOMOLMCP_FOREACH_ACTOR_DESC(WorldPartition, [&ActorDescsJson](const SOMOLMCP_ACTOR_DESC* ActorDescInstance)
				{
					ActorDescsJson.Add(MakeShared<FJsonValueObject>(ActorDescInstanceToJson(ActorDescInstance)));
					return true;
				});
				Result->SetArrayField(TEXT("actorDescs"), ActorDescsJson);
				Result->SetNumberField(TEXT("count"), ActorDescsJson.Num());
				OutStructured = Result;
				OutSummary = TEXT("Listed world partition actor descriptors.");
				return true;
}
, nullptr
, 5
});
		Registry.Register({
			TEXT("world_partition_list_hlods"),
			TEXT("List HLOD actors or descriptors in world partition."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				if (!WorldPartition)
				{
					return false;
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> HlodsJson;
				SOMOLMCP_FOREACH_ACTOR_DESC<AWorldPartitionHLOD>(WorldPartition, [&HlodsJson](const SOMOLMCP_ACTOR_DESC* ActorDescInstance)
				{
					HlodsJson.Add(MakeShared<FJsonValueObject>(ActorDescInstanceToJson(ActorDescInstance)));
					return true;
				});
				Result->SetArrayField(TEXT("hlods"), HlodsJson);
				Result->SetNumberField(TEXT("count"), HlodsJson.Num());
				OutStructured = Result;
				OutSummary = TEXT("Listed world partition HLOD descriptors.");
				return true;
}
, nullptr
, 5
});
		Registry.Register({
			TEXT("world_partition_list_content_bundles"),
			TEXT("List content bundles in world partition."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				if (!ContentBundleSubsystem)
				{
					OutError = TEXT("ContentBundleEditorSubsystem is unavailable.");
					return false;
				}

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> BundlesJson;
				for (const TSharedPtr<FContentBundleEditor>& ContentBundleEditor : ContentBundleSubsystem->GetEditorContentBundles())
				{
					if (!ContentBundleEditor.IsValid())
					{
						continue;
					}

					const UContentBundleDescriptor* Descriptor = ContentBundleEditor->GetDescriptor();
					const bool bIsEditing = Descriptor ? ContentBundleSubsystem->IsEditingContentBundle(Descriptor->GetGuid()) : ContentBundleEditor->IsBeingEdited();
					BundlesJson.Add(MakeShared<FJsonValueObject>(ContentBundleEditorToJson(*ContentBundleEditor, bIsEditing)));
				}
				Result->SetArrayField(TEXT("contentBundles"), BundlesJson);
				Result->SetNumberField(TEXT("count"), BundlesJson.Num());
				OutStructured = Result;
				OutSummary = TEXT("Listed world partition content bundles.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_activate_editing"),
			TEXT("Activate content bundle editing."),
			FSololmcpSchemaBuilder::Object({{TEXT("guid"), FSololmcpSchemaBuilder::String()}, {TEXT("display_name"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				TSharedPtr<FContentBundleEditor> ContentBundleEditor = ResolveContentBundleEditor(Arguments, OutError);
				if (!ContentBundleSubsystem || !ContentBundleEditor.IsValid())
				{
					return false;
				}

				const bool bActivated = ContentBundleSubsystem->ActivateContentBundleEditing(ContentBundleEditor);
				OutStructured = ContentBundleEditorToJson(*ContentBundleEditor, ContentBundleSubsystem->IsEditingContentBundle(ContentBundleEditor->GetDescriptor()->GetGuid()));
				OutStructured->SetBoolField(TEXT("activated"), bActivated);
				OutSummary = bActivated ? TEXT("Activated content bundle editing.") : TEXT("Content bundle editing was already active or could not be activated.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_deactivate_editing"),
			TEXT("Deactivate current content bundle editing."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				if (!ContentBundleSubsystem)
				{
					OutError = TEXT("ContentBundleEditorSubsystem is unavailable.");
					return false;
				}

				OutStructured = MakeShared<FJsonObject>();
				#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				OutStructured->SetBoolField(TEXT("deactivated"), ContentBundleSubsystem->DeactivateCurrentContentBundleEditing());
#else
				// On 5.3 this call lives on the editing submodule, not the subsystem.
				// UContentBundleEditingSubmodule is declared without an export macro on 5.3, so
				// the call cannot be linked from a plugin even though the method exists.
				OutError = TEXT("NOT_AVAILABLE_ON_ENGINE: deactivating content bundle editing requires "
					"UE 5.4 or later on this build.");
				return false;
#endif
				OutSummary = TEXT("Deactivated current content bundle editing.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_select_actors"),
			TEXT("Select actors belonging to a content bundle."),
			FSololmcpSchemaBuilder::Object({{TEXT("guid"), FSololmcpSchemaBuilder::String()}, {TEXT("display_name"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				TSharedPtr<FContentBundleEditor> ContentBundleEditor = ResolveContentBundleEditor(Arguments, OutError);
				if (!ContentBundleSubsystem || !ContentBundleEditor.IsValid())
				{
					return false;
				}

				ContentBundleSubsystem->SelectActors(*ContentBundleEditor);
				OutStructured = ContentBundleEditorToJson(*ContentBundleEditor, ContentBundleSubsystem->IsEditingContentBundle(ContentBundleEditor->GetDescriptor()->GetGuid()));
				OutSummary = TEXT("Selected content bundle actors.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_deselect_actors"),
			TEXT("Deselect actors belonging to a content bundle."),
			FSololmcpSchemaBuilder::Object({{TEXT("guid"), FSololmcpSchemaBuilder::String()}, {TEXT("display_name"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				TSharedPtr<FContentBundleEditor> ContentBundleEditor = ResolveContentBundleEditor(Arguments, OutError);
				if (!ContentBundleSubsystem || !ContentBundleEditor.IsValid())
				{
					return false;
				}

				ContentBundleSubsystem->DeselectActors(*ContentBundleEditor);
				OutStructured = ContentBundleEditorToJson(*ContentBundleEditor, ContentBundleSubsystem->IsEditingContentBundle(ContentBundleEditor->GetDescriptor()->GetGuid()));
				OutSummary = TEXT("Deselected content bundle actors.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_reference_all_actors"),
			TEXT("Reference all actors in a content bundle."),
			FSololmcpSchemaBuilder::Object({{TEXT("guid"), FSololmcpSchemaBuilder::String()}, {TEXT("display_name"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				TSharedPtr<FContentBundleEditor> ContentBundleEditor = ResolveContentBundleEditor(Arguments, OutError);
				if (!ContentBundleSubsystem || !ContentBundleEditor.IsValid())
				{
					return false;
				}

				ContentBundleSubsystem->ReferenceAllActors(*ContentBundleEditor);
				OutStructured = ContentBundleEditorToJson(*ContentBundleEditor, ContentBundleSubsystem->IsEditingContentBundle(ContentBundleEditor->GetDescriptor()->GetGuid()));
				OutSummary = TEXT("Referenced all content bundle actors.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_unreference_all_actors"),
			TEXT("Unreference all actors in a content bundle."),
			FSololmcpSchemaBuilder::Object({{TEXT("guid"), FSololmcpSchemaBuilder::String()}, {TEXT("display_name"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				TSharedPtr<FContentBundleEditor> ContentBundleEditor = ResolveContentBundleEditor(Arguments, OutError);
				if (!ContentBundleSubsystem || !ContentBundleEditor.IsValid())
				{
					return false;
				}

				ContentBundleSubsystem->UnreferenceAllActors(*ContentBundleEditor);
				OutStructured = ContentBundleEditorToJson(*ContentBundleEditor, ContentBundleSubsystem->IsEditingContentBundle(ContentBundleEditor->GetDescriptor()->GetGuid()));
				OutSummary = TEXT("Unreferenced all content bundle actors.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_inject_base_content"),
			TEXT("Inject base content for a content bundle."),
			FSololmcpSchemaBuilder::Object({{TEXT("guid"), FSololmcpSchemaBuilder::String()}, {TEXT("display_name"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				TSharedPtr<FContentBundleEditor> ContentBundleEditor = ResolveContentBundleEditor(Arguments, OutError);
				if (!ContentBundleSubsystem || !ContentBundleEditor.IsValid())
				{
					return false;
				}

				ContentBundleEditor->InjectBaseContent();
				OutStructured = ContentBundleEditorToJson(*ContentBundleEditor, ContentBundleSubsystem->IsEditingContentBundle(ContentBundleEditor->GetDescriptor()->GetGuid()));
				OutSummary = TEXT("Injected content bundle base content.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("content_bundle_remove_content"),
			TEXT("Remove content from a content bundle."),
			FSololmcpSchemaBuilder::Object({{TEXT("guid"), FSololmcpSchemaBuilder::String()}, {TEXT("display_name"), FSololmcpSchemaBuilder::String()}}, {}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				TSharedPtr<FContentBundleEditor> ContentBundleEditor = ResolveContentBundleEditor(Arguments, OutError);
				if (!ContentBundleSubsystem || !ContentBundleEditor.IsValid())
				{
					return false;
				}

				ContentBundleEditor->RemoveContent();
				OutStructured = ContentBundleEditorToJson(*ContentBundleEditor, ContentBundleSubsystem->IsEditingContentBundle(ContentBundleEditor->GetDescriptor()->GetGuid()));
				OutSummary = TEXT("Removed content bundle content.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_validate"),
			TEXT("Run world partition validation checks."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				if (!WorldPartition)
				{
					return false;
				}

				FSololmcpStreamingGenerationErrorCollector ErrorCollector;
				WorldPartition->CheckForErrors(&ErrorCollector);

				int32 ActorDescCount = 0;
				int32 HlodCount = 0;
				SOMOLMCP_FOREACH_ACTOR_DESC(WorldPartition, [&ActorDescCount, &HlodCount](const SOMOLMCP_ACTOR_DESC* ActorDescInstance)
				{
					++ActorDescCount;
					if (ActorDescInstance && ActorDescInstance->GetActorNativeClass() && ActorDescInstance->GetActorNativeClass()->IsChildOf(AWorldPartitionHLOD::StaticClass()))
					{
						++HlodCount;
					}
					return true;
				});

				int32 CellCount = 0;
				bool bCellsEnumerated = false;
				if (GEditor)
				{
					if (UWorld* World = GEditor->GetEditorWorldContext().World())
					{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
						FWorldPartitionUtils::FSimulateCookedSession Session(World);
						if (Session.IsValid())
						{
							bCellsEnumerated = Session.ForEachStreamingCells([&CellCount](const IWorldPartitionCell*)
							{
								++CellCount;
							});
						}
#else
						// Not exported on 5.3; the surrounding report still returns everything else.
						(void)World;
#endif
					}
				}

				UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get();
				int32 ContentBundleCount = 0;
				if (ContentBundleSubsystem)
				{
					for (const TSharedPtr<FContentBundleEditor>& ContentBundleEditor : ContentBundleSubsystem->GetEditorContentBundles())
					{
						if (!ContentBundleEditor.IsValid())
						{
							continue;
						}
						++ContentBundleCount;
						if (!ContentBundleEditor->IsValid() || ContentBundleEditor->GetStatus() == EContentBundleStatus::FailedToInject)
						{
							ErrorCollector.AddSimpleFinding(TEXT("content_bundle_status"), FString::Printf(TEXT("Content bundle '%s' is in status '%s'."), *ContentBundleEditor->GetDisplayName(), *ContentBundleStatusToString(ContentBundleEditor->GetStatus())));
						}
					}
				}

				if (UDataLayerManager* DataLayerManager = UDataLayerManager::GetDataLayerManager(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr))
				{
					DataLayerManager->ForEachDataLayerInstance([&ErrorCollector](UDataLayerInstance* DataLayerInstance)
					{
						if (DataLayerInstance)
						{
							DataLayerInstance->Validate(&ErrorCollector);
						}
						return true;
					});
				}

				const int32 StreamingSourceCount = WorldPartition->GetStreamingSources().Num();
				const int32 RegisteredLoaderAdapterCount = WorldPartition->GetRegisteredEditorLoaderAdapters().Num();
				const bool bValidationPassed = ErrorCollector.Findings.Num() == 0 && bCellsEnumerated;

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("passed"), bValidationPassed);
				Result->SetNumberField(TEXT("actorDescCount"), ActorDescCount);
				Result->SetNumberField(TEXT("hlodCount"), HlodCount);
				Result->SetNumberField(TEXT("cellCount"), CellCount);
				Result->SetBoolField(TEXT("cellsEnumerated"), bCellsEnumerated);
				Result->SetNumberField(TEXT("streamingSourceCount"), StreamingSourceCount);
				Result->SetNumberField(TEXT("registeredLoaderAdapterCount"), RegisteredLoaderAdapterCount);
				Result->SetNumberField(TEXT("contentBundleCount"), ContentBundleCount);
				Result->SetArrayField(TEXT("findings"), ErrorCollector.Findings);
				Result->SetNumberField(TEXT("findingCount"), ErrorCollector.Findings.Num());
				Result->SetObjectField(TEXT("runtimeWorldBounds"), BoxToJson(WorldPartition->GetRuntimeWorldBounds()));
				OutStructured = Result;
				OutSummary = bValidationPassed ? TEXT("World partition validation checks passed.") : TEXT("World partition validation checks reported issues.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_diagnose"),
			TEXT("Run world partition diagnostics."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorldPartition* WorldPartition = GetCurrentEditorWorldPartition(OutError);
				if (!WorldPartition)
				{
					return false;
				}

				FSololmcpStreamingGenerationErrorCollector ErrorCollector;
				WorldPartition->CheckForErrors(&ErrorCollector);

				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

				TArray<TSharedPtr<FJsonValue>> StreamingSourcesJson;
				for (const FWorldPartitionStreamingSource& Source : WorldPartition->GetStreamingSources())
				{
					StreamingSourcesJson.Add(MakeShared<FJsonValueObject>(StreamingSourceToJson(Source)));
				}
				Result->SetArrayField(TEXT("streamingSources"), StreamingSourcesJson);

				TArray<TSharedPtr<FJsonValue>> RegionsJson;
				for (UWorldPartitionEditorLoaderAdapter* EditorLoaderAdapter : WorldPartition->GetRegisteredEditorLoaderAdapters())
				{
					if (!EditorLoaderAdapter || !EditorLoaderAdapter->GetLoaderAdapter())
					{
						continue;
					}

					TSharedRef<FJsonObject> RegionJson = MakeShared<FJsonObject>();
					RegionJson->SetBoolField(TEXT("loaded"), EditorLoaderAdapter->GetLoaderAdapter()->IsLoaded());
					RegionJson->SetBoolField(TEXT("userCreated"), EditorLoaderAdapter->GetLoaderAdapter()->GetUserCreated());
					if (const TOptional<FString> Label = EditorLoaderAdapter->GetLoaderAdapter()->GetLabel(); Label.IsSet())
					{
						RegionJson->SetStringField(TEXT("label"), Label.GetValue());
					}
					if (const TOptional<FBox> BoundingBox = EditorLoaderAdapter->GetLoaderAdapter()->GetBoundingBox(); BoundingBox.IsSet())
					{
						RegionJson->SetObjectField(TEXT("bounds"), BoxToJson(BoundingBox.GetValue()));
					}
					RegionsJson.Add(MakeShared<FJsonValueObject>(RegionJson));
				}
				Result->SetArrayField(TEXT("registeredRegions"), RegionsJson);
				Result->SetNumberField(TEXT("registeredLoaderAdapterCount"), RegionsJson.Num());

				int32 ActorDescCount = 0;
				int32 HlodCount = 0;
				SOMOLMCP_FOREACH_ACTOR_DESC(WorldPartition, [&ActorDescCount, &HlodCount](const SOMOLMCP_ACTOR_DESC* ActorDescInstance)
				{
					++ActorDescCount;
					if (ActorDescInstance && ActorDescInstance->GetActorNativeClass() && ActorDescInstance->GetActorNativeClass()->IsChildOf(AWorldPartitionHLOD::StaticClass()))
					{
						++HlodCount;
					}
					return true;
				});
				Result->SetNumberField(TEXT("actorDescCount"), ActorDescCount);
				Result->SetNumberField(TEXT("hlodCount"), HlodCount);

				int32 CellCount = 0;
				TArray<TSharedPtr<FJsonValue>> CellsPreviewJson;
				if (GEditor)
				{
					if (UWorld* World = GEditor->GetEditorWorldContext().World())
					{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
						FWorldPartitionUtils::FSimulateCookedSession Session(World);
						if (Session.IsValid())
						{
							Session.ForEachStreamingCells([&CellCount, &CellsPreviewJson](const IWorldPartitionCell* Cell)
							{
								++CellCount;
								if (CellsPreviewJson.Num() < 20)
								{
									CellsPreviewJson.Add(MakeShared<FJsonValueObject>(RuntimeCellToJson(Cell)));
								}
							});
						}
#else
						// Not exported on 5.3; cellCount stays 0 and the preview stays empty.
						(void)World;
#endif
					}
				}
				Result->SetNumberField(TEXT("cellCount"), CellCount);
				Result->SetArrayField(TEXT("cellsPreview"), CellsPreviewJson);

				if (UContentBundleEditorSubsystem* ContentBundleSubsystem = UContentBundleEditorSubsystem::Get())
				{
					TArray<TSharedPtr<FJsonValue>> BundlesJson;
					for (const TSharedPtr<FContentBundleEditor>& ContentBundleEditor : ContentBundleSubsystem->GetEditorContentBundles())
					{
						if (!ContentBundleEditor.IsValid())
						{
							continue;
						}
						const UContentBundleDescriptor* Descriptor = ContentBundleEditor->GetDescriptor();
						const bool bIsEditing = Descriptor ? ContentBundleSubsystem->IsEditingContentBundle(Descriptor->GetGuid()) : ContentBundleEditor->IsBeingEdited();
						BundlesJson.Add(MakeShared<FJsonValueObject>(ContentBundleEditorToJson(*ContentBundleEditor, bIsEditing)));
						if (!ContentBundleEditor->IsValid() || ContentBundleEditor->GetStatus() == EContentBundleStatus::FailedToInject)
						{
							ErrorCollector.AddSimpleFinding(TEXT("content_bundle_status"), FString::Printf(TEXT("Content bundle '%s' is in status '%s'."), *ContentBundleEditor->GetDisplayName(), *ContentBundleStatusToString(ContentBundleEditor->GetStatus())));
						}
					}
					Result->SetArrayField(TEXT("contentBundles"), BundlesJson);
					Result->SetNumberField(TEXT("contentBundleCount"), BundlesJson.Num());
				}
				else
				{
					Result->SetNumberField(TEXT("contentBundleCount"), 0);
				}

				if (UDataLayerManager* DataLayerManager = UDataLayerManager::GetDataLayerManager(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr))
				{
					TArray<TSharedPtr<FJsonValue>> DataLayerJson;
					DataLayerManager->ForEachDataLayerInstance([&DataLayerJson, &ErrorCollector](UDataLayerInstance* DataLayerInstance)
					{
						if (DataLayerInstance)
						{
							DataLayerJson.Add(MakeShared<FJsonValueObject>(DataLayerInstanceToJson(DataLayerInstance)));
							DataLayerInstance->Validate(&ErrorCollector);
						}
						return true;
					});
					Result->SetArrayField(TEXT("dataLayers"), DataLayerJson);
					Result->SetNumberField(TEXT("dataLayerCount"), DataLayerJson.Num());
				}

				Result->SetArrayField(TEXT("findings"), ErrorCollector.Findings);
				Result->SetNumberField(TEXT("findingCount"), ErrorCollector.Findings.Num());
				Result->SetObjectField(TEXT("runtimeWorldBounds"), BoxToJson(WorldPartition->GetRuntimeWorldBounds()));
				OutStructured = Result;
				OutSummary = TEXT("Collected world partition diagnostics.");
				return true;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_load_large_region"),
			TEXT("Compatibility alias for world_partition_load_region. Not a production large-world batch loader; response marks capability_status=compat_region_delegate."),
			FSololmcpSchemaBuilder::Object({{TEXT("min"), VectorSchema()}, {TEXT("max"), VectorSchema()}}, {TEXT("min"), TEXT("max")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const bool bOk = Registry.ExecuteTool(TEXT("world_partition_load_region"), Arguments, OutStructured, OutSummary, OutError);
				OutStructured->SetStringField(TEXT("capability_status"), TEXT("compat_region_delegate"));
				OutStructured->SetStringField(TEXT("implementation_status"), bOk ? TEXT("delegated_to_world_partition_load_region") : TEXT("delegate_failed"));
				OutStructured->SetBoolField(TEXT("production_large_world_ready"), false);
				OutStructured->SetStringField(TEXT("production_note"), TEXT("This tool delegates one bounded region call and is not a verified large-world streaming/generation pipeline."));
				return bOk;
			}
		, nullptr
		, 5
		});
		Registry.Register({
			TEXT("world_partition_unload_large_region"),
			TEXT("Compatibility alias for world_partition_unload_region. Not a production large-world batch unloader; response marks capability_status=compat_region_delegate."),
			FSololmcpSchemaBuilder::Object({{TEXT("min"), VectorSchema()}, {TEXT("max"), VectorSchema()}}, {TEXT("min"), TEXT("max")}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const bool bOk = Registry.ExecuteTool(TEXT("world_partition_unload_region"), Arguments, OutStructured, OutSummary, OutError);
				OutStructured->SetStringField(TEXT("capability_status"), TEXT("compat_region_delegate"));
				OutStructured->SetStringField(TEXT("implementation_status"), bOk ? TEXT("delegated_to_world_partition_unload_region") : TEXT("delegate_failed"));
				OutStructured->SetBoolField(TEXT("production_large_world_ready"), false);
				OutStructured->SetStringField(TEXT("production_note"), TEXT("This tool delegates one bounded region call and is not a verified large-world streaming/generation pipeline."));
				return bOk;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_assign_actor"),
			TEXT("Assign an actor to a data layer."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_full_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_object_path"), FSololmcpSchemaBuilder::String()}
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
				UDataLayerInstance* DataLayer = ResolveDataLayerInstanceFromArguments(Arguments, TEXT("data_layer_name"), OutError);
				if (!Actor || !DataLayer)
				{
					return false;
				}

				UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
				if (!Subsystem)
				{
					OutError = TEXT("DataLayerEditorSubsystem is unavailable.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DataLayerAssignActor", "SOMOLMCP Assign Actor To Data Layer"));
				Subsystem->AddActorToDataLayer(Actor, DataLayer);
				const TArray<AActor*> ActorsInLayer = Subsystem->GetActorsFromDataLayer(DataLayer);
				if (!ActorsInLayer.Contains(Actor))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("actor"),
						TEXT("AddActorToDataLayer completed but the actor is not in the target data layer."));
					OutError = TEXT("Failed to assign actor to data layer.");
					return false;
				}
				OutStructured = MakeActorListResult(ActorsInLayer);
				OutStructured->SetObjectField(TEXT("dataLayer"), DataLayerInstanceToJson(DataLayer));
				OutSummary = TEXT("Assigned actor to data layer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_remove_actor"),
			TEXT("Remove an actor from a data layer."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_full_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_object_path"), FSololmcpSchemaBuilder::String()}
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
				UDataLayerInstance* DataLayer = ResolveDataLayerInstanceFromArguments(Arguments, TEXT("data_layer_name"), OutError);
				if (!Actor || !DataLayer)
				{
					return false;
				}

				UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
				if (!Subsystem)
				{
					OutError = TEXT("DataLayerEditorSubsystem is unavailable.");
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DataLayerRemoveActor", "SOMOLMCP Remove Actor From Data Layer"));
				const TArray<AActor*> ActorsBefore = Subsystem->GetActorsFromDataLayer(DataLayer);
				if (!ActorsBefore.Contains(Actor))
				{
					SololmcpError::Set(OutStructured, TEXT("NO_OP"), TEXT("actor"),
						TEXT("Actor is not in the requested data layer."));
					OutError = TEXT("Actor is not in the requested data layer.");
					return false;
				}
				Subsystem->RemoveActorFromDataLayer(Actor, DataLayer);
				const TArray<AActor*> ActorsAfter = Subsystem->GetActorsFromDataLayer(DataLayer);
				if (ActorsAfter.Contains(Actor))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("actor"),
						TEXT("RemoveActorFromDataLayer completed but the actor remains in the data layer."));
					OutError = TEXT("Failed to remove actor from data layer.");
					return false;
				}
				OutStructured = MakeActorListResult(ActorsAfter);
				OutStructured->SetObjectField(TEXT("dataLayer"), DataLayerInstanceToJson(DataLayer));
				OutSummary = TEXT("Removed actor from data layer.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_batch_set_visibility"),
			TEXT("Batch update visibility on data layers."),
			FSololmcpSchemaBuilder::Object({{TEXT("data_layer_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("visible"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("data_layer_names"), TEXT("visible")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> DataLayerNames;
				if (!TryGetStringArray(Arguments, TEXT("data_layer_names"), DataLayerNames) || !Arguments->HasTypedField<EJson::Boolean>(TEXT("visible")))
				{
					OutError = TEXT("Missing data_layer_names or visible.");
					return false;
				}
				UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
				if (!Subsystem)
				{
					OutError = TEXT("DataLayerEditorSubsystem is unavailable.");
					return false;
				}

				const bool bVisible = Arguments->GetBoolField(TEXT("visible"));
				TArray<UDataLayerInstance*> UpdatedLayers;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DataLayerBatchSetVisibility", "SOMOLMCP Set Data Layer Visibility"));
				for (const FString& Name : DataLayerNames)
				{
					if (UDataLayerInstance* DataLayer = ResolveDataLayerInstance(Name, OutError))
					{
						Subsystem->SetDataLayerVisibility(DataLayer, bVisible);
						UpdatedLayers.Add(DataLayer);
					}
					else
					{
						return false;
					}
				}
				OutStructured = MakeDataLayerListResult(UpdatedLayers);
				OutStructured->SetBoolField(TEXT("visible"), bVisible);
				OutSummary = TEXT("Updated data layer visibility.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_batch_set_loaded_in_editor"),
			TEXT("Batch update loaded-in-editor state on data layers."),
			FSololmcpSchemaBuilder::Object({{TEXT("data_layer_names"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("loaded"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("data_layer_names"), TEXT("loaded")}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> DataLayerNames;
				if (!TryGetStringArray(Arguments, TEXT("data_layer_names"), DataLayerNames) || !Arguments->HasTypedField<EJson::Boolean>(TEXT("loaded")))
				{
					OutError = TEXT("Missing data_layer_names or loaded.");
					return false;
				}
				UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
				if (!Subsystem)
				{
					OutError = TEXT("DataLayerEditorSubsystem is unavailable.");
					return false;
				}

				const bool bLoaded = Arguments->GetBoolField(TEXT("loaded"));
				TArray<UDataLayerInstance*> UpdatedLayers;
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DataLayerBatchSetLoaded", "SOMOLMCP Set Data Layer Loaded In Editor"));
				for (const FString& Name : DataLayerNames)
				{
					if (UDataLayerInstance* DataLayer = ResolveDataLayerInstance(Name, OutError))
					{
						Subsystem->SetDataLayerIsLoadedInEditor(DataLayer, bLoaded, true);
						UpdatedLayers.Add(DataLayer);
					}
					else
					{
						return false;
					}
				}
				OutStructured = MakeDataLayerListResult(UpdatedLayers);
				OutStructured->SetBoolField(TEXT("loaded"), bLoaded);
				OutSummary = TEXT("Updated data layer loaded-in-editor state.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("data_layer_list_actors"),
			TEXT("List actors assigned to a data layer."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("data_layer_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_full_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("data_layer_object_path"), FSololmcpSchemaBuilder::String()}
				},
				{}),

			[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
				if (!Subsystem)
				{
					OutError = TEXT("DataLayerEditorSubsystem is unavailable.");
					return false;
				}
				UDataLayerInstance* DataLayer = ResolveDataLayerInstanceFromArguments(Arguments, TEXT("data_layer_name"), OutError);
				if (!DataLayer)
				{
					return false;
				}
				OutStructured = MakeActorListResult(Subsystem->GetActorsFromDataLayer(DataLayer));
				OutStructured->SetObjectField(TEXT("dataLayer"), DataLayerInstanceToJson(DataLayer));
				OutSummary = TEXT("Listed actors in data layer.");
				return true;
			}
		, nullptr
		, 5
		});

		// ==================== C++ Native Actor Tools (replacing Python) ====================
		
		// ---- actor_group_selected (C++ native) ----
		Registry.Register({
			TEXT("actor_group_selected"),
			TEXT("Group the current selected actors in the viewport."),
			FSololmcpSchemaBuilder::Object({}, {}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("Editor is unavailable.");
					return false;
				}
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}
				USelection* Selection = GEditor->GetSelectedActors();
				if (!Selection || Selection->Num() < 2)
				{
					OutError = TEXT("Need at least 2 selected actors to group.");
					return false;
				}
				GEditor->BeginTransaction(FText::FromString(TEXT("Group Actors")));
				const bool bExecOk = GEditor->Exec(World, TEXT("ACTOR GROUP"));
				GEditor->EndTransaction();
				if (!bExecOk)
				{
					OutError = TEXT("Actor group command failed.");
					return false;
				}
				OutStructured->SetNumberField(TEXT("selected_count"), Selection->Num());
				OutSummary = TEXT("Grouped selected actors.");
				return true;
			}
		, nullptr
		, 5
		});

		// ---- actor_ungroup_selected (C++ native) ----
		Registry.Register({
			TEXT("actor_ungroup_selected"),
			TEXT("Ungroup the current selected actors in the viewport."),
			FSololmcpSchemaBuilder::Object({}, {}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("Editor is unavailable.");
					return false;
				}
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World)
				{
					return false;
				}
				USelection* Selection = GEditor->GetSelectedActors();
				if (!Selection || Selection->Num() < 1)
				{
					OutError = TEXT("Need at least 1 selected actor to ungroup.");
					return false;
				}
				GEditor->BeginTransaction(FText::FromString(TEXT("Ungroup Actors")));
				const bool bExecOk = GEditor->Exec(World, TEXT("ACTOR UNGROUP"));
				GEditor->EndTransaction();
				if (!bExecOk)
				{
					OutError = TEXT("Actor ungroup command failed.");
					return false;
				}
				OutStructured->SetNumberField(TEXT("selected_count"), Selection->Num());
				OutSummary = TEXT("Ungrouped selected actors.");
				return true;
			}
		, nullptr
		, 5
		});

		// ---- actor_align (C++ native) ----
		Registry.Register({
			TEXT("actor_align"),
			TEXT("Align actors along X, Y, or Z axis. Mode: center (default), min, max."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor names/labels/paths to align"))},
					{TEXT("axis"), FSololmcpSchemaBuilder::String(TEXT("Axis: x, y, or z (default: x)"))},
					{TEXT("mode"), FSololmcpSchemaBuilder::String(TEXT("Alignment mode: center, min, max (default: center)"))}
				},
				{TEXT("actors"), TEXT("axis")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> ActorIds;
				const TArray<TSharedPtr<FJsonValue>>* Arr;
				if (Arguments->TryGetArrayField(TEXT("actors"), Arr))
				{
					for (const auto& V : *Arr)
						ActorIds.Add(V->AsString());
				}
				if (ActorIds.Num() < 2) { OutError = TEXT("Need at least 2 actors to align."); return false; }

				FString AxisStr = Arguments->GetStringField(TEXT("axis")).ToLower();
				FString ModeStr = Arguments->GetStringField(TEXT("mode")).ToLower();
				if (AxisStr.IsEmpty()) AxisStr = TEXT("x");
				if (ModeStr.IsEmpty()) ModeStr = TEXT("center");
				if (AxisStr != TEXT("x") && AxisStr != TEXT("y") && AxisStr != TEXT("z"))
				{
					OutError = TEXT("axis must be x, y, or z.");
					return false;
				}
				if (ModeStr != TEXT("center") && ModeStr != TEXT("min") && ModeStr != TEXT("max"))
				{
					OutError = TEXT("mode must be center, min, or max.");
					return false;
				}

				int32 AxisIndex = 0;
				if (AxisStr == TEXT("y")) AxisIndex = 1;
				else if (AxisStr == TEXT("z")) AxisIndex = 2;

				// Resolve actors
				TArray<AActor*> ResolvedActors;
				for (const FString& Id : ActorIds)
				{
					AActor* Actor = Context.Services.FindActorByLabelOrName(Id, OutError);
					if (Actor) ResolvedActors.Add(Actor);
				}
				if (ResolvedActors.Num() < 2) { OutError = TEXT("Could not resolve enough actors."); return false; }

				// Calculate target position
				TArray<float> Values;
				for (AActor* Actor : ResolvedActors)
				{
					FVector Loc = Actor->GetActorLocation();
					Values.Add(Loc[AxisIndex]);
				}

				float Target = 0.0f;
				if (ModeStr == TEXT("min"))
					Target = FMath::Min(Values);
				else if (ModeStr == TEXT("max"))
					Target = FMath::Max(Values);
				else
					Target = ((FMath::Min(Values) + FMath::Max(Values)) / 2.0f);

				// Apply alignment
				GEditor->BeginTransaction(FText::FromString(TEXT("Align Actors")));
				for (AActor* Actor : ResolvedActors)
				{
					FVector Loc = Actor->GetActorLocation();
					Loc[AxisIndex] = Target;
					Actor->SetActorLocation(Loc, false);
				}
				GEditor->EndTransaction();
				for (AActor* Actor : ResolvedActors)
				{
					if (!FMath::IsNearlyEqual(Actor->GetActorLocation()[AxisIndex], Target, 0.5f))
					{
						OutError = FString::Printf(TEXT("Alignment reported success but actor '%s' is at %.2f instead of %.2f on %s."),
							*Actor->GetActorLabel(), Actor->GetActorLocation()[AxisIndex], Target, *AxisStr);
						return false;
					}
				}

				OutStructured->SetNumberField(TEXT("aligned_count"), ResolvedActors.Num());
				OutStructured->SetStringField(TEXT("axis"), AxisStr);
				OutStructured->SetNumberField(TEXT("target"), Target);
				OutSummary = FString::Printf(TEXT("Aligned %d actors on %s axis at %.2f"), ResolvedActors.Num(), *AxisStr, Target);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- actor_distribute (C++ native) ----
		Registry.Register({
			TEXT("actor_distribute"),
			TEXT("Distribute actors evenly along X, Y, or Z axis."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor names/labels/paths to distribute"))},
					{TEXT("axis"), FSololmcpSchemaBuilder::String(TEXT("Axis: x, y, or z (default: x)"))}
				},
				{TEXT("actors"), TEXT("axis")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> ActorIds;
				const TArray<TSharedPtr<FJsonValue>>* Arr;
				if (Arguments->TryGetArrayField(TEXT("actors"), Arr))
				{
					for (const auto& V : *Arr)
						ActorIds.Add(V->AsString());
				}
				if (ActorIds.Num() < 3) { OutError = TEXT("Need at least 3 actors to distribute."); return false; }

				FString AxisStr = Arguments->GetStringField(TEXT("axis")).ToLower();
				if (AxisStr.IsEmpty()) AxisStr = TEXT("x");
				if (AxisStr != TEXT("x") && AxisStr != TEXT("y") && AxisStr != TEXT("z"))
				{
					OutError = TEXT("axis must be x, y, or z.");
					return false;
				}

				int32 AxisIndex = 0;
				if (AxisStr == TEXT("y")) AxisIndex = 1;
				else if (AxisStr == TEXT("z")) AxisIndex = 2;

				// Resolve actors
				TArray<AActor*> ResolvedActors;
				for (const FString& Id : ActorIds)
				{
					AActor* Actor = Context.Services.FindActorByLabelOrName(Id, OutError);
					if (Actor) ResolvedActors.Add(Actor);
				}
				if (ResolvedActors.Num() < 3) { OutError = TEXT("Could not resolve enough actors."); return false; }

				// Sort by axis position
				ResolvedActors.Sort([AxisIndex](AActor& A, AActor& B) {
					return A.GetActorLocation()[AxisIndex] < B.GetActorLocation()[AxisIndex];
				});

				float Start = ResolvedActors[0]->GetActorLocation()[AxisIndex];
				float End = ResolvedActors.Last()->GetActorLocation()[AxisIndex];
				float Step = (End - Start) / float(ResolvedActors.Num() - 1);

				// Apply distribution
				GEditor->BeginTransaction(FText::FromString(TEXT("Distribute Actors")));
				TArray<float> ExpectedPositions;
				for (int32 i = 0; i < ResolvedActors.Num(); ++i)
				{
					FVector Loc = ResolvedActors[i]->GetActorLocation();
					Loc[AxisIndex] = Start + Step * i;
					ExpectedPositions.Add(Loc[AxisIndex]);
					ResolvedActors[i]->SetActorLocation(Loc, false);
				}
				GEditor->EndTransaction();
				for (int32 i = 0; i < ResolvedActors.Num(); ++i)
				{
					if (!FMath::IsNearlyEqual(ResolvedActors[i]->GetActorLocation()[AxisIndex], ExpectedPositions[i], 0.5f))
					{
						OutError = FString::Printf(TEXT("Distribution reported success but actor '%s' is at %.2f instead of %.2f on %s."),
							*ResolvedActors[i]->GetActorLabel(), ResolvedActors[i]->GetActorLocation()[AxisIndex], ExpectedPositions[i], *AxisStr);
						return false;
					}
				}

				OutStructured->SetNumberField(TEXT("distributed_count"), ResolvedActors.Num());
				OutStructured->SetStringField(TEXT("axis"), AxisStr);
				OutStructured->SetNumberField(TEXT("step"), Step);
				OutSummary = FString::Printf(TEXT("Distributed %d actors on %s axis with step %.2f"), ResolvedActors.Num(), *AxisStr, Step);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- actor_mirror (C++ native) ----
		Registry.Register({
			TEXT("actor_mirror"),
			TEXT("Mirror actors around a pivot point on X, Y, or Z axis."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor names/labels/paths to mirror"))},
					{TEXT("axis"), FSololmcpSchemaBuilder::String(TEXT("Axis: x, y, or z (default: x)"))}
				},
				{TEXT("actors"), TEXT("axis")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> ActorIds;
				const TArray<TSharedPtr<FJsonValue>>* Arr;
				if (Arguments->TryGetArrayField(TEXT("actors"), Arr))
				{
					for (const auto& V : *Arr)
						ActorIds.Add(V->AsString());
				}
				if (ActorIds.Num() < 1) { OutError = TEXT("Need at least 1 actor to mirror."); return false; }

				FString AxisStr = Arguments->GetStringField(TEXT("axis")).ToLower();
				if (AxisStr.IsEmpty()) AxisStr = TEXT("x");
				if (AxisStr != TEXT("x") && AxisStr != TEXT("y") && AxisStr != TEXT("z"))
				{
					OutError = TEXT("axis must be x, y, or z.");
					return false;
				}

				int32 AxisIndex = 0;
				if (AxisStr == TEXT("y")) AxisIndex = 1;
				else if (AxisStr == TEXT("z")) AxisIndex = 2;

				// Resolve actors
				TArray<AActor*> ResolvedActors;
				for (const FString& Id : ActorIds)
				{
					AActor* Actor = Context.Services.FindActorByLabelOrName(Id, OutError);
					if (Actor) ResolvedActors.Add(Actor);
				}
				if (ResolvedActors.Num() < 1) { OutError = TEXT("Could not resolve any actors."); return false; }

				// Calculate pivot (center of all actors)
				float Pivot = 0.0f;
				for (AActor* Actor : ResolvedActors)
					Pivot += Actor->GetActorLocation()[AxisIndex];
				Pivot /= float(ResolvedActors.Num());

				// Apply mirror
				GEditor->BeginTransaction(FText::FromString(TEXT("Mirror Actors")));
				TArray<float> ExpectedPositions;
				for (AActor* Actor : ResolvedActors)
				{
					FVector Loc = Actor->GetActorLocation();
					Loc[AxisIndex] = 2.0f * Pivot - Loc[AxisIndex];
					ExpectedPositions.Add(Loc[AxisIndex]);
					Actor->SetActorLocation(Loc, false);
				}
				GEditor->EndTransaction();
				for (int32 i = 0; i < ResolvedActors.Num(); ++i)
				{
					if (!FMath::IsNearlyEqual(ResolvedActors[i]->GetActorLocation()[AxisIndex], ExpectedPositions[i], 0.5f))
					{
						OutError = FString::Printf(TEXT("Mirror reported success but actor '%s' is at %.2f instead of %.2f on %s."),
							*ResolvedActors[i]->GetActorLabel(), ResolvedActors[i]->GetActorLocation()[AxisIndex], ExpectedPositions[i], *AxisStr);
						return false;
					}
				}

				OutStructured->SetNumberField(TEXT("mirrored_count"), ResolvedActors.Num());
				OutStructured->SetStringField(TEXT("axis"), AxisStr);
				OutStructured->SetNumberField(TEXT("pivot"), Pivot);
				OutSummary = FString::Printf(TEXT("Mirrored %d actors on %s axis around pivot %.2f"), ResolvedActors.Num(), *AxisStr, Pivot);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- actor_snap_to_floor (C++ native) ----
		Registry.Register({
			TEXT("actor_snap_to_floor"),
			TEXT("Snap actors to the floor (ground surface below them)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor names/labels/paths to snap to floor"))}
				},
				{TEXT("actors")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> ActorIds;
				const TArray<TSharedPtr<FJsonValue>>* Arr;
				if (Arguments->TryGetArrayField(TEXT("actors"), Arr))
				{
					for (const auto& V : *Arr)
						ActorIds.Add(V->AsString());
				}
				if (ActorIds.Num() < 1) { OutError = TEXT("Need at least 1 actor to snap."); return false; }

				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World) { OutError = TEXT("No world available."); return false; }

				// Resolve actors
				TArray<AActor*> ResolvedActors;
				for (const FString& Id : ActorIds)
				{
					AActor* Actor = Context.Services.FindActorByLabelOrName(Id, OutError);
					if (Actor) ResolvedActors.Add(Actor);
				}
				if (ResolvedActors.Num() < 1) { OutError = TEXT("Could not resolve any actors."); return false; }

				int32 SnappedCount = 0;
				int32 VerifiedCount = 0;
				GEditor->BeginTransaction(FText::FromString(TEXT("Snap Actors to Floor")));

				for (AActor* Actor : ResolvedActors)
				{
					FVector Start = Actor->GetActorLocation();
					Start.Z += 10000.0f;  // Start above
					FVector End = Start;
					End.Z -= 20000.0f;    // Trace down

					FHitResult Hit;
					FCollisionQueryParams Params;
					Params.AddIgnoredActor(Actor);

					if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
					{
						FVector NewLoc = Actor->GetActorLocation();
						NewLoc.Z = Hit.Location.Z;
						Actor->SetActorLocation(NewLoc, false);
						SnappedCount++;
						if (FMath::IsNearlyEqual(Actor->GetActorLocation().Z, Hit.Location.Z, 0.5f))
						{
							VerifiedCount++;
						}
					}
				}
				GEditor->EndTransaction();
				if (SnappedCount == 0)
				{
					OutError = TEXT("No actors were snapped because no floor hit was found.");
					return false;
				}
				if (VerifiedCount != SnappedCount)
				{
					OutError = FString::Printf(TEXT("Snap reported %d actor(s), but only %d reached the traced floor height."), SnappedCount, VerifiedCount);
					return false;
				}

				OutStructured->SetNumberField(TEXT("snapped_count"), SnappedCount);
				OutStructured->SetNumberField(TEXT("verified_count"), VerifiedCount);
				OutStructured->SetNumberField(TEXT("total_count"), ResolvedActors.Num());
				OutSummary = FString::Printf(TEXT("Snapped %d/%d actors to floor."), SnappedCount, ResolvedActors.Num());
				return true;
			}
		, nullptr
		, 5
		});
	}
}
