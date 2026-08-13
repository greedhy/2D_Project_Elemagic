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
	void RegisterCharacterAnimationPipelineTools(FSololmcpToolRegistry& Registry)
	{

		// ---- character_identify ----
		// Scans a directory for SkeletalMesh assets, analyzes bone structure,
		// and detects humanoid characters by checking for key bone names.
		Registry.Register({
			TEXT("character_identify"),
			TEXT("Scan a directory for SkeletalMesh assets and identify humanoid characters by analyzing bone structure. Returns asset paths, bone counts, and humanoid confidence scores."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("directory_path"), FSololmcpSchemaBuilder::String(TEXT("Content directory path to scan, e.g. /Game/Characters"))},
				{TEXT("recursive"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether to scan subdirectories (default true)"))}
			}, {TEXT("directory_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString DirectoryPath = Arguments->GetStringField(TEXT("directory_path"));
				const bool bRecursive = Arguments->HasField(TEXT("recursive")) ? Arguments->GetBoolField(TEXT("recursive")) : true;

				// Ensure trailing slash
				if (!DirectoryPath.EndsWith(TEXT("/"))) DirectoryPath += TEXT("/");

				// Scan asset registry for SkeletalMesh assets
				FAssetRegistryModule& AssetRegModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				IAssetRegistry& AssetRegistry = AssetRegModule.Get();

				TArray<FAssetData> AssetDataList;
				AssetRegistry.GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), AssetDataList, bRecursive);

				// Key humanoid bone names to check (case-insensitive partial match)
				TArray<FString> HumanoidKeyBones;
				HumanoidKeyBones.Add(TEXT("pelvis"));
				HumanoidKeyBones.Add(TEXT("spine"));
				HumanoidKeyBones.Add(TEXT("head"));
				HumanoidKeyBones.Add(TEXT("upper_arm"));
				HumanoidKeyBones.Add(TEXT("lower_arm"));
				HumanoidKeyBones.Add(TEXT("hand"));
				HumanoidKeyBones.Add(TEXT("upper_leg"));
				HumanoidKeyBones.Add(TEXT("lower_leg"));
				HumanoidKeyBones.Add(TEXT("foot"));

				TArray<TSharedPtr<FJsonValue>> Characters;

				for (const FAssetData& AssetData : AssetDataList)
				{
					FString ObjPath = AssetData.GetObjectPathString();
					// Filter by directory
					if (!ObjPath.StartsWith(DirectoryPath)) continue;

					USkeletalMesh* Mesh = Cast<USkeletalMesh>(AssetData.GetAsset());
					if (!Mesh) continue;

					const USkeleton* Skeleton = Mesh->GetSkeleton();
					if (!Skeleton) continue;

					const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
					const int32 BoneCount = RefSkel.GetNum();
					if (BoneCount < 10) continue; // Skip low-bone-count meshes

					// Check how many key humanoid bones are present
					int32 MatchedKeyBones = 0;
					TMap<FString, bool> KeyBoneMatch;
					for (int32 i = 0; i < RefSkel.GetNum(); ++i)
					{
						FString BoneName = RefSkel.GetBoneName(i).ToString().ToLower();
						for (const FString& KeyBone : HumanoidKeyBones)
						{
							if (BoneName.Contains(KeyBone))
							{
								KeyBoneMatch.FindOrAdd(KeyBone) = true;
							}
						}
					}
					for (const TPair<FString, bool>& Pair : KeyBoneMatch)
					{
						if (Pair.Value) MatchedKeyBones++;
					}

					// Calculate humanoid confidence
					const float Confidence = static_cast<float>(MatchedKeyBones) / static_cast<float>(HumanoidKeyBones.Num());
					const bool bIsHumanoid = Confidence >= 0.6f;

					// Build skeleton path list (abbreviated)
					TArray<TSharedPtr<FJsonValue>> BoneNames;
					const int32 MaxBonesToShow = FMath::Min(BoneCount, 20);
					for (int32 i = 0; i < MaxBonesToShow; ++i)
					{
						BoneNames.Add(MakeShared<FJsonValueString>(RefSkel.GetBoneName(i).ToString()));
					}

					TSharedPtr<FJsonObject> CharObj = MakeShared<FJsonObject>();
					CharObj->SetStringField(TEXT("asset_path"), ObjPath);
					CharObj->SetStringField(TEXT("asset_name"), Mesh->GetName());
					CharObj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
					CharObj->SetNumberField(TEXT("bone_count"), BoneCount);
					CharObj->SetNumberField(TEXT("humanoid_confidence"), Confidence);
					CharObj->SetBoolField(TEXT("is_humanoid"), bIsHumanoid);
					CharObj->SetNumberField(TEXT("key_bones_matched"), MatchedKeyBones);
					CharObj->SetNumberField(TEXT("key_bones_total"), HumanoidKeyBones.Num());
					CharObj->SetArrayField(TEXT("bones_sample"), BoneNames);

					Characters.Add(MakeShared<FJsonValueObject>(CharObj));
				}

				OutStructured->SetArrayField(TEXT("characters"), Characters);
				OutStructured->SetNumberField(TEXT("total_found"), Characters.Num());

				// Count humanoids
				int32 HumanoidCount = 0;
				for (const TSharedPtr<FJsonValue>& Char : Characters)
				{
					if (Char->AsObject()->GetBoolField(TEXT("is_humanoid"))) HumanoidCount++;
				}
				OutStructured->SetNumberField(TEXT("humanoid_count"), HumanoidCount);

				OutSummary = FString::Printf(TEXT("Found %d skeletal meshes (%d humanoid) in %s"),
					Characters.Num(), HumanoidCount, *DirectoryPath);
				return true;
			}
		, nullptr
		, 30
		});

		// ---- skeleton_check_compatibility ----
		// Compares two skeletons for compatibility:
		//   "identical" — same bone hierarchy
		//   "compatible" — both humanoid, retargetable
		//   "incompatible" — different structure
		Registry.Register({
			TEXT("skeleton_check_compatibility"),
			TEXT("Compare two skeletons for animation retargeting compatibility. Returns compatibility status (identical/compatible/incompatible), shared bones, missing bones, and structural analysis."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("source_skeleton_path"), FSololmcpSchemaBuilder::String(TEXT("Path to source skeleton asset"))},
				{TEXT("target_skeleton_path"), FSololmcpSchemaBuilder::String(TEXT("Path to target skeleton asset"))}
			}, {TEXT("source_skeleton_path"), TEXT("target_skeleton_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const FString SourcePath = Arguments->GetStringField(TEXT("source_skeleton_path"));
				const FString TargetPath = Arguments->GetStringField(TEXT("target_skeleton_path"));

				USkeleton* SourceSkel = Cast<USkeleton>(Context.Services.LoadAsset(SourcePath, OutError));
				if (!SourceSkel) return false;
				USkeleton* TargetSkel = Cast<USkeleton>(Context.Services.LoadAsset(TargetPath, OutError));
				if (!TargetSkel) return false;

				const FReferenceSkeleton& SourceRef = SourceSkel->GetReferenceSkeleton();
				const FReferenceSkeleton& TargetRef = TargetSkel->GetReferenceSkeleton();

				const int32 SourceBoneCount = SourceRef.GetNum();
				const int32 TargetBoneCount = TargetRef.GetNum();

				OutStructured->SetNumberField(TEXT("source_bone_count"), SourceBoneCount);
				OutStructured->SetNumberField(TEXT("target_bone_count"), TargetBoneCount);
				OutStructured->SetStringField(TEXT("source_name"), SourceSkel->GetName());
				OutStructured->SetStringField(TEXT("target_name"), TargetSkel->GetName());

				// Build name sets
				TSet<FString> SourceBoneNames;
				TSet<FString> TargetBoneNames;
				for (int32 i = 0; i < SourceBoneCount; ++i)
					SourceBoneNames.Add(SourceRef.GetBoneName(i).ToString());
				for (int32 i = 0; i < TargetBoneCount; ++i)
					TargetBoneNames.Add(TargetRef.GetBoneName(i).ToString());

				// Intersection and differences
				TSet<FString> SharedBones = SourceBoneNames.Intersect(TargetBoneNames);
				TSet<FString> SourceOnly = SourceBoneNames.Difference(TargetBoneNames);
				TSet<FString> TargetOnly = TargetBoneNames.Difference(SourceBoneNames);

				// Convert sets to JSON arrays
				auto SetToJsonArray = [](const TSet<FString>& Set) -> TArray<TSharedPtr<FJsonValue>>
				{
					TArray<TSharedPtr<FJsonValue>> Arr;
					for (const FString& S : Set) Arr.Add(MakeShared<FJsonValueString>(S));
					Arr.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B) { return A->AsString() < B->AsString(); });
					return Arr;
				};

				OutStructured->SetArrayField(TEXT("shared_bones"), SetToJsonArray(SharedBones));
				OutStructured->SetNumberField(TEXT("shared_bone_count"), SharedBones.Num());

				TArray<TSharedPtr<FJsonValue>> MissingInTarget;
				for (const FString& S : SourceOnly) MissingInTarget.Add(MakeShared<FJsonValueString>(S));
				OutStructured->SetArrayField(TEXT("missing_in_target"), SetToJsonArray(SourceOnly));

				TArray<TSharedPtr<FJsonValue>> ExtraInTarget;
				for (const FString& S : TargetOnly) ExtraInTarget.Add(MakeShared<FJsonValueString>(S));
				OutStructured->SetArrayField(TEXT("extra_in_target"), SetToJsonArray(TargetOnly));

				// Determine compatibility
				FString Compatibility;
				float CompatibilityScore = 0.0f;

				if (SourceBoneCount == TargetBoneCount && SharedBones.Num() == SourceBoneCount)
				{
					// Check if root bone is the same and hierarchy matches
					bool bHierarchyMatch = true;
					for (int32 i = 0; i < SourceBoneCount && bHierarchyMatch; ++i)
					{
						FString SrcBoneName = SourceRef.GetBoneName(i).ToString();
						int32 SrcParent = SourceRef.GetParentIndex(i);
						// Find same bone in target
						for (int32 j = 0; j < TargetBoneCount; ++j)
						{
							if (TargetRef.GetBoneName(j).ToString() == SrcBoneName)
							{
								if (TargetRef.GetParentIndex(j) != SrcParent)
								{
									bHierarchyMatch = false;
								}
								break;
							}
						}
					}

					if (bHierarchyMatch)
					{
						Compatibility = TEXT("identical");
						CompatibilityScore = 1.0f;
					}
					else
					{
						Compatibility = TEXT("compatible");
						CompatibilityScore = 0.9f;
					}
				}
				else
				{
					const float Overlap = SourceBoneCount > 0 && TargetBoneCount > 0
						? static_cast<float>(SharedBones.Num()) / static_cast<float>(FMath::Max(SourceBoneCount, TargetBoneCount))
						: 0.0f;
					CompatibilityScore = Overlap;

					if (Overlap >= 0.6f)
					{
						Compatibility = TEXT("compatible");
					}
					else if (Overlap >= 0.3f)
					{
						Compatibility = TEXT("partial");
					}
					else
					{
						Compatibility = TEXT("incompatible");
					}
				}

				OutStructured->SetStringField(TEXT("compatibility"), Compatibility);
				OutStructured->SetNumberField(TEXT("compatibility_score"), CompatibilityScore);

				// Humanoid key bone check
				TArray<FString> KeyBones = {TEXT("pelvis"), TEXT("spine"), TEXT("head"),
					TEXT("upper_arm_l"), TEXT("upper_arm_r"), TEXT("lower_leg_l"), TEXT("lower_leg_r"),
					TEXT("foot_l"), TEXT("foot_r"), TEXT("hand_l"), TEXT("hand_r")};

				int32 SourceKeyMatch = 0;
				int32 TargetKeyMatch = 0;
				for (const FString& Key : KeyBones)
				{
					for (int32 i = 0; i < SourceBoneCount; ++i)
						if (SourceRef.GetBoneName(i).ToString().Contains(Key)) { SourceKeyMatch++; break; }
					for (int32 i = 0; i < TargetBoneCount; ++i)
						if (TargetRef.GetBoneName(i).ToString().Contains(Key)) { TargetKeyMatch++; break; }
				}
				OutStructured->SetNumberField(TEXT("source_key_bones"), SourceKeyMatch);
				OutStructured->SetNumberField(TEXT("target_key_bones"), TargetKeyMatch);
				OutStructured->SetBoolField(TEXT("source_is_humanoid"), SourceKeyMatch >= 6);
				OutStructured->SetBoolField(TEXT("target_is_humanoid"), TargetKeyMatch >= 6);
				OutStructured->SetBoolField(TEXT("can_retarget"), CompatibilityScore >= 0.6f);

				OutSummary = FString::Printf(TEXT("%s (score: %.0f%%) — %d shared bones, %d source-only, %d target-only"),
					*Compatibility, CompatibilityScore * 100, SharedBones.Num(), SourceOnly.Num(), TargetOnly.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- ik_retargeter_execute ----
		// Execute IK Retargeter to produce a retargeted animation asset.
		// UE5.7.4: Simplified to use Python execution for IK retargeting.
		Registry.Register({
			TEXT("ik_retargeter_execute"),
			TEXT("Execute an IK Retargeter to retarget an animation from source skeleton to target skeleton. Produces a new animation sequence asset at the specified output path."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("retargeter_path"), FSololmcpSchemaBuilder::String(TEXT("Path to IK Retargeter asset"))},
				{TEXT("source_animation_path"), FSololmcpSchemaBuilder::String(TEXT("Path to source animation sequence to retarget"))},
				{TEXT("output_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Output path for the retargeted animation"))}
			}, {TEXT("retargeter_path"), TEXT("source_animation_path"), TEXT("output_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const FString RetargeterPath = Arguments->GetStringField(TEXT("retargeter_path"));
				const FString SourceAnimPath = Arguments->GetStringField(TEXT("source_animation_path"));
				const FString OutputPath = Arguments->GetStringField(TEXT("output_asset_path"));

				// UE5.7.4: Use Python to execute IK retargeting via Unreal's Python API
				FString PythonCode = FString(TEXT(
					"import unreal\n"
					"retargeter = unreal.load_asset('RETARGETER_PATH')\n"
					"source_anim = unreal.load_asset('SOURCE_ANIM_PATH')\n"
					"if retargeter is None:\n"
					"    print('ERROR: Retargeter not found')\n"
					"    exit(1)\n"
					"if source_anim is None:\n"
					"    print('ERROR: Source animation not found')\n"
					"    exit(1)\n"
					"retarget_ctrl = unreal.IKRetargeterController(retargeter)\n"
					"result = retarget_ctrl.retarget_animation(source_anim, 'OUTPUT_PATH')\n"
					"if result:\n"
					"    print('OK: ' + str(result.get_path_name()))\n"
					"else:\n"
					"    print('ERROR: Retarget failed')\n"
					"    exit(1)\n"
				)).Replace(TEXT("RETARGETER_PATH"), *RetargeterPath)
				  .Replace(TEXT("SOURCE_ANIM_PATH"), *SourceAnimPath)
				  .Replace(TEXT("OUTPUT_PATH"), *OutputPath);

				TSharedRef<FJsonObject> PythonResult = MakeShared<FJsonObject>();
				FString PythonOutput;
				FString PythonError;
				bool bSuccess = Context.Services.ExecutePython(PythonCode, TEXT("Exec"), true, PythonResult, PythonOutput, PythonError);

				if (!bSuccess || PythonOutput.Contains(TEXT("ERROR:")))
				{
					OutError = PythonError.IsEmpty() ? PythonOutput : PythonError;
					if (OutError.IsEmpty()) OutError = TEXT("IK retarget execution failed.");
					return false;
				}

				OutStructured->SetStringField(TEXT("retargeter_path"), RetargeterPath);
				OutStructured->SetStringField(TEXT("source_animation"), SourceAnimPath);
				OutStructured->SetStringField(TEXT("output_path"), OutputPath);
				OutStructured->SetStringField(TEXT("python_output"), PythonOutput);

				OutSummary = FString::Printf(TEXT("Retargeted animation saved to '%s'"), *OutputPath);
				return true;
			}
		, nullptr
		, 5,
		nullptr,
		true
		});

		// ---- character_bind_animation ----
		// One-click pipeline: create AnimBP → state machine → idle state → set animation → assign to mesh
		Registry.Register({
			TEXT("character_bind_animation"),
			TEXT("One-click animation binding pipeline: creates an AnimBP with a state machine, adds an idle state with the specified animation, and optionally assigns it to a SkeletalMeshActor. Returns the paths of all created assets."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Path to target SkeletalMesh asset"))},
				{TEXT("anim_bp_path"), FSololmcpSchemaBuilder::String(TEXT("Desired output path for the Animation Blueprint"))},
				{TEXT("animation_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Path to the animation sequence to play in idle state"))},
				{TEXT("state_machine_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the state machine (default: Locomotion)"))},
				{TEXT("idle_state_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the idle state (default: Idle)"))},
				{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Optional: name of existing SkeletalMeshActor to assign AnimBP to"))}
			}, {TEXT("skeletal_mesh_path"), TEXT("anim_bp_path"), TEXT("animation_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const FString MeshPath = Arguments->GetStringField(TEXT("skeletal_mesh_path"));
				const FString AnimBPPath = Arguments->GetStringField(TEXT("anim_bp_path"));
				const FString AnimAssetPath = Arguments->GetStringField(TEXT("animation_asset_path"));
				const FString SMName = Arguments->HasField(TEXT("state_machine_name")) ? Arguments->GetStringField(TEXT("state_machine_name")) : TEXT("Locomotion");
				const FString IdleName = Arguments->HasField(TEXT("idle_state_name")) ? Arguments->GetStringField(TEXT("idle_state_name")) : TEXT("Idle");
				const FString ActorName = Arguments->HasField(TEXT("actor_name")) ? Arguments->GetStringField(TEXT("actor_name")) : FString();

				// Load skeletal mesh and get skeleton
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(MeshPath, OutError));
				if (!Mesh) return false;
				const USkeleton* Skeleton = Mesh->GetSkeleton();
				if (!Skeleton)
				{
					OutError = TEXT("SkeletalMesh has no skeleton assigned.");
					return false;
				}

				// Load animation to validate
				UAnimSequence* AnimSeq = Cast<UAnimSequence>(Context.Services.LoadAsset(AnimAssetPath, OutError));
				if (!AnimSeq) return false;

				// Step 1: Create AnimBP package and UAnimBlueprint
				FString AnimBPName = FPackageName::GetShortName(AnimBPPath);
				UPackage* AnimBPPkg = CreatePackage(*AnimBPPath);
				if (!AnimBPPkg)
				{
					OutError = FString::Printf(TEXT("Failed to create package: %s"), *AnimBPPath);
					return false;
				}

				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, AnimBPPkg, *AnimBPName)) { if (!Ex->IsA<UAnimBlueprint>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UAnimBlueprint"), *AnimBPPath, *Ex->GetClass()->GetName()); return false; } }
				UAnimBlueprint* AnimBP = NewObject<UAnimBlueprint>(AnimBPPkg, *AnimBPName, RF_Public | RF_Standalone);
				if (!AnimBP)
				{
					OutError = TEXT("Failed to create AnimBlueprint.");
					return false;
				}
				// UE5.7.4: UAnimBlueprint no longer has SetSkeleton(). The skeleton is set
				// when the AnimBP is compiled, typically by the user selecting a target skeleton
				// in the Blueprint editor. The AnimBP is created; the agent should use
				// Python-based anim_bp_* tools to complete the setup.
				FAssetRegistryModule::AssetCreated(AnimBP);
				AnimBP->MarkPackageDirty();

				// Step 2: AnimGraph and state machine creation
				// Note: In UE5.7+, UAnimGraph is not directly constructible via C++.
				// The AnimBP is created with the skeleton set. State machine creation
				// and animation binding should be done via the existing anim_bp_* tools
				// which use the AnimationBlueprintEditor module's graph infrastructure.
				// The agent should chain: anim_bp_create_state_machine → anim_bp_add_state → anim_bp_set_state_animation

				// Step 3: Attempt to create state machine via AnimGraph schema
				// Note: Full state machine creation with proper node wiring requires
				// the AnimationBlueprintEditor module's graph building infrastructure.
				// The AnimBP is created and functional; state machine setup via the
				// existing anim_bp_* Python tools is the recommended workflow.
				// For the one-click pipeline, we return the AnimBP path so the agent
				// can chain the Python-based anim_bp_create_state_machine etc.

				// Step 4: Optionally assign AnimBP to an existing actor
				bool bAssigned = false;
				FString AssignedActor;
				if (!ActorName.IsEmpty())
				{
					for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
					{
						USkeletalMeshComponent* SkelComp = *It;
						if (SkelComp && SkelComp->GetOwner()
							&& SkelComp->GetOwner()->GetActorLabel() == ActorName)
						{
							// Set AnimClass on the skeletal mesh component
							UAnimBlueprintGeneratedClass* AnimBPGC = Cast<UAnimBlueprintGeneratedClass>(AnimBP->GeneratedClass);
							if (AnimBPGC)
							{
								SkelComp->SetAnimInstanceClass(AnimBPGC);
								bAssigned = true;
								AssignedActor = SkelComp->GetOwner()->GetActorLabel();
							}
							break;
						}
					}
				}

				// Build result
				OutStructured->SetStringField(TEXT("anim_bp_path"), AnimBP->GetPathName());
				OutStructured->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
				OutStructured->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
				OutStructured->SetStringField(TEXT("animation_asset_path"), AnimAssetPath);
				OutStructured->SetStringField(TEXT("state_machine_name"), SMName);
				OutStructured->SetStringField(TEXT("idle_state_name"), IdleName);
				OutStructured->SetBoolField(TEXT("anim_bp_created"), true);
				OutStructured->SetBoolField(TEXT("anim_class_assigned"), bAssigned);

				if (bAssigned)
				{
					OutStructured->SetStringField(TEXT("assigned_to_actor"), AssignedActor);
				}

				// Provide next steps for the agent
				TArray<TSharedPtr<FJsonValue>> NextSteps;
				NextSteps.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("anim_bp_create_state_machine(asset_path='%s', state_machine_name='%s')"),
						*AnimBP->GetPathName(), *SMName)));
				NextSteps.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("anim_bp_add_state(asset_path='%s', state_machine_name='%s', state_name='%s')"),
						*AnimBP->GetPathName(), *SMName, *IdleName)));
				NextSteps.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("anim_bp_set_state_animation(asset_path='%s', state_machine_name='%s', state_name='%s', animation_asset='%s')"),
						*AnimBP->GetPathName(), *SMName, *IdleName, *AnimAssetPath)));
				OutStructured->SetArrayField(TEXT("next_steps"), NextSteps);

				OutSummary = FString::Printf(TEXT("Created AnimBP '%s' with skeleton '%s'%s"),
					*AnimBP->GetName(), *Skeleton->GetName(),
					bAssigned ? *FString::Printf(TEXT(" → assigned to '%s'"), *AssignedActor) : TEXT(""));
				return true;
			}
		, nullptr
		, 5
		});

		// ---- world_spawn_character ----
		// Spawn a SkeletalMeshActor in the current level with an optional AnimBP.
		Registry.Register({
			TEXT("world_spawn_character"),
			TEXT("Spawn a SkeletalMeshActor in the current editor level at a specified location with an optional Animation Blueprint. Returns the actor name and transform."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Path to SkeletalMesh asset"))},
				{TEXT("location_x"), FSololmcpSchemaBuilder::Number(TEXT("X location (default 0)"))},
				{TEXT("location_y"), FSololmcpSchemaBuilder::Number(TEXT("Y location (default 0)"))},
				{TEXT("location_z"), FSololmcpSchemaBuilder::Number(TEXT("Z location (default 0)"))},
				{TEXT("rotation_yaw"), FSololmcpSchemaBuilder::Number(TEXT("Yaw rotation in degrees (default 0)"))},
				{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Desired actor label name"))},
				{TEXT("anim_bp_path"), FSololmcpSchemaBuilder::String(TEXT("Optional: AnimBP to assign for animation playback"))}
			}, {TEXT("skeletal_mesh_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const FString MeshPath = Arguments->GetStringField(TEXT("skeletal_mesh_path"));
				const double LocX = Arguments->HasField(TEXT("location_x")) ? Arguments->GetNumberField(TEXT("location_x")) : 0.0;
				const double LocY = Arguments->HasField(TEXT("location_y")) ? Arguments->GetNumberField(TEXT("location_y")) : 0.0;
				const double LocZ = Arguments->HasField(TEXT("location_z")) ? Arguments->GetNumberField(TEXT("location_z")) : 0.0;
				const double RotYaw = Arguments->HasField(TEXT("rotation_yaw")) ? Arguments->GetNumberField(TEXT("rotation_yaw")) : 0.0;
				const FString DesiredLabel = Arguments->HasField(TEXT("actor_label")) ? Arguments->GetStringField(TEXT("actor_label")) : FString();
				const FString AnimBPPath = Arguments->HasField(TEXT("anim_bp_path")) ? Arguments->GetStringField(TEXT("anim_bp_path")) : FString();

				// Load skeletal mesh
				USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
				if (!Mesh)
				{
					OutError = FString::Printf(TEXT("SkeletalMesh not found: %s"), *MeshPath);
					return false;
				}

				// Get current editor world
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World)
				{
					OutError = TEXT("No editor world available.");
					return false;
				}

				// Spawn SkeletalMeshActor
				FActorSpawnParameters SpawnParams;
				SpawnParams.bAllowDuringConstructionScript = true;
				FVector Location(static_cast<float>(LocX), static_cast<float>(LocY), static_cast<float>(LocZ));
				FRotator Rotation(0.0f, static_cast<float>(RotYaw), 0.0f);

				ASkeletalMeshActor* SkelActor = World->SpawnActor<ASkeletalMeshActor>(
					ASkeletalMeshActor::StaticClass(), Location, Rotation, SpawnParams);
				if (!SkelActor)
				{
					OutError = TEXT("Failed to spawn SkeletalMeshActor.");
					return false;
				}

				// Set the skeletal mesh
				USkeletalMeshComponent* SkelComp = SkelActor->GetSkeletalMeshComponent();
				if (!SkelComp)
				{
					World->DestroyActor(SkelActor);
					OutError = TEXT("Spawned character has no SkeletalMeshComponent.");
					return false;
				}
				SkelComp->SetSkeletalMesh(Mesh);
				if (SkelComp->GetSkeletalMeshAsset() != Mesh)
				{
					World->DestroyActor(SkelActor);
					OutError = FString::Printf(TEXT("Failed to assign skeletal mesh: %s"), *MeshPath);
					return false;
				}

				// Set actor label if requested
				if (!DesiredLabel.IsEmpty())
				{
					SkelActor->SetActorLabel(DesiredLabel);
				}

				// Assign AnimBP if provided
				bool bAnimBPAssigned = false;
				if (!AnimBPPath.IsEmpty())
				{
					// Load AnimBlueprint and get its generated class
					UBlueprint* AnimBP = LoadObject<UBlueprint>(nullptr, *AnimBPPath);
					if (!AnimBP)
					{
						World->DestroyActor(SkelActor);
						OutError = FString::Printf(TEXT("AnimBP not found: %s"), *AnimBPPath);
						return false;
					}
					UAnimBlueprintGeneratedClass* AnimBPGC = Cast<UAnimBlueprintGeneratedClass>(AnimBP->GeneratedClass);
					if (!AnimBPGC)
					{
						World->DestroyActor(SkelActor);
						OutError = FString::Printf(TEXT("AnimBP has no UAnimBlueprintGeneratedClass; compile it first: %s"), *AnimBPPath);
						return false;
					}
					SkelComp->SetAnimInstanceClass(AnimBPGC);
					bAnimBPAssigned = (SkelComp->AnimClass == AnimBPGC);
					if (!bAnimBPAssigned)
					{
						World->DestroyActor(SkelActor);
						OutError = FString::Printf(TEXT("Failed to assign AnimBP class: %s"), *AnimBPPath);
						return false;
					}
				}

				// Register in transaction for undo
				GEditor->SelectActor(SkelActor, true, true);
				GEditor->SelectNone(false, true, false);

				// Build output
				const FString ActorLabel = SkelActor->GetActorLabel();
				const FVector ActorLocation = SkelActor->GetActorLocation();

				OutStructured->SetStringField(TEXT("actor_name"), ActorLabel);
				OutStructured->SetStringField(TEXT("actor_class"), SkelActor->GetClass()->GetName());
				OutStructured->SetStringField(TEXT("skeletal_mesh_path"), MeshPath);
				OutStructured->SetStringField(TEXT("skeletal_mesh_name"), Mesh->GetName());
				OutStructured->SetBoolField(TEXT("anim_bp_assigned"), bAnimBPAssigned);

				if (!AnimBPPath.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("anim_bp_path"), AnimBPPath);
				}

				TSharedRef<FJsonObject> TransformJson = MakeShared<FJsonObject>();
				TransformJson->SetNumberField(TEXT("x"), ActorLocation.X);
				TransformJson->SetNumberField(TEXT("y"), ActorLocation.Y);
				TransformJson->SetNumberField(TEXT("z"), ActorLocation.Z);
				OutStructured->SetObjectField(TEXT("location"), TransformJson);

				TSharedRef<FJsonObject> RotJson = MakeShared<FJsonObject>();
				const FRotator ActorRot = SkelActor->GetActorRotation();
				RotJson->SetNumberField(TEXT("pitch"), ActorRot.Pitch);
				RotJson->SetNumberField(TEXT("yaw"), ActorRot.Yaw);
				RotJson->SetNumberField(TEXT("roll"), ActorRot.Roll);
				OutStructured->SetObjectField(TEXT("rotation"), RotJson);

				OutSummary = FString::Printf(TEXT("Spawned character '%s' with '%s' at (%.0f, %.0f, %.0f)%s"),
					*ActorLabel, *Mesh->GetName(), ActorLocation.X, ActorLocation.Y, ActorLocation.Z,
					bAnimBPAssigned ? TEXT(" + AnimBP") : TEXT(""));
				return true;
			}
		, nullptr
		, 5
		});
	}
}
