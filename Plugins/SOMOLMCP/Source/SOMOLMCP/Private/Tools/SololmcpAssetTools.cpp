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
	void RegisterAssetTools(FSololmcpToolRegistry& Registry)
	{
		Registry.Register({
			TEXT("asset_create"),
			TEXT("Create an asset with an explicit asset class and factory class."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("package_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("asset_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("asset_class_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("factory_class_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("factory_overrides"), FSololmcpSchemaBuilder::Object({})},
					{TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicitly replace an existing non-PCG asset at the same path."))},
					{TEXT("overwrite"), FSololmcpSchemaBuilder::Boolean(TEXT("Alias for replace_existing."))},
					{TEXT("force"), FSololmcpSchemaBuilder::Boolean(TEXT("Alias for replace_existing."))}
				},
				{TEXT("package_path"), TEXT("asset_name"), TEXT("asset_class_path"), TEXT("factory_class_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetName;
				FString AssetClassPath;
				FString FactoryClassPath;
				if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) ||
					!Arguments->TryGetStringField(TEXT("asset_name"), AssetName) ||
					!Arguments->TryGetStringField(TEXT("asset_class_path"), AssetClassPath) ||
					!Arguments->TryGetStringField(TEXT("factory_class_path"), FactoryClassPath))
				{
					OutError = TEXT("Missing package_path, asset_name, asset_class_path or factory_class_path.");
					return false;
				}

				TSharedPtr<FJsonObject> FactoryOverrides;
				TryGetObjectField(Arguments, TEXT("factory_overrides"), FactoryOverrides);
				const bool bReplaceExisting =
					(Arguments->HasTypedField<EJson::Boolean>(TEXT("replace_existing")) && Arguments->GetBoolField(TEXT("replace_existing"))) ||
					(Arguments->HasTypedField<EJson::Boolean>(TEXT("overwrite")) && Arguments->GetBoolField(TEXT("overwrite"))) ||
					(Arguments->HasTypedField<EJson::Boolean>(TEXT("force")) && Arguments->GetBoolField(TEXT("force")));
				FString ClassResolveError;
				UClass* ExpectedAssetClass = Context.Services.ResolveClass(AssetClassPath, ClassResolveError);
				if (!ExpectedAssetClass)
				{
					OutError = ClassResolveError.IsEmpty()
						? FString::Printf(TEXT("Failed to resolve asset_class_path: %s"), *AssetClassPath)
						: ClassResolveError;
					return false;
				}
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AssetCreate", "SOMOLMCP Create Asset"));
				UObject* Asset = Context.Services.CreateAsset(PackagePath, AssetName, AssetClassPath, FactoryClassPath, FactoryOverrides, OutError, bReplaceExisting);
				if (!Asset)
				{
					return false;
				}
				// Audit round 7 (silent-create fix): persist + notify registry, then verify the asset
				// is actually visible to the registry/disk so callers don't get a fake "ok" reference.
				const FString CreatedPath = Asset->GetPathName();
				Asset->MarkPackageDirty();
				FAssetRegistryModule::AssetCreated(Asset);
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(CreatedPath, false, SaveErr);
				if (!VerifyCreatedAssetReloaded(Context.Services, Asset, ExpectedAssetClass, OutStructured, OutError))
				{
					if (!bSaved) { OutStructured->SetStringField(TEXT("save_error"), SaveErr); }
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = TEXT("Created asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_query"),
			TEXT("Query assets by package path and class path. For exact readback, pass asset_path; an empty exact result is a business failure."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("package_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("class_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("recursive"), FSololmcpSchemaBuilder::Boolean()}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PackagePath;
				FString AssetPath;
				FString ClassPath;
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
				Arguments->TryGetStringField(TEXT("package_path"), PackagePath);
				Arguments->TryGetStringField(TEXT("class_path"), ClassPath);
				const bool bRecursive = Arguments->HasTypedField<EJson::Boolean>(TEXT("recursive")) ? Arguments->GetBoolField(TEXT("recursive")) : true;
				const bool bExactAssetQuery = !AssetPath.IsEmpty();
				TArray<FAssetData> Assets;
				if (bExactAssetQuery)
				{
					FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
					IAssetRegistry& Registry = RegistryModule.Get();
					Registry.WaitForCompletion();

					FString ObjectPath = AssetPath;
					const int32 LastSlash = ObjectPath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					const int32 LastDot = ObjectPath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					if (LastDot <= LastSlash)
					{
						FString AssetName = ObjectPath;
						int32 SlashIndex = INDEX_NONE;
						if (AssetName.FindLastChar(TEXT('/'), SlashIndex))
						{
							AssetName = AssetName.Mid(SlashIndex + 1);
						}
						ObjectPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
					}

					const FAssetData ExactAsset = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
					if (ExactAsset.IsValid())
					{
						Assets.Add(ExactAsset);
					}
					else
					{
						Registry.GetAssetsByPackageName(FName(*AssetPath), Assets);
					}
				}
				else
				{
					Assets = Context.Services.QueryAssets(PackagePath, ClassPath, bRecursive, OutError);
				}

				if (!OutError.IsEmpty() && Assets.Num() == 0)
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> AssetJson;
				for (const FAssetData& Asset : Assets)
				{
					AssetJson.Add(MakeShared<FJsonValueObject>(AssetDataToJson(Asset)));
				}
				OutStructured->SetArrayField(TEXT("assets"), AssetJson);
				OutStructured->SetNumberField(TEXT("count"), AssetJson.Num());
				OutStructured->SetBoolField(TEXT("found"), AssetJson.Num() > 0);
				if (bExactAssetQuery)
				{
					OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
					if (AssetJson.Num() == 0)
					{
						OutStructured->SetStringField(TEXT("status"), TEXT("not_found"));
						OutError = FString::Printf(TEXT("Asset not found: %s"), *AssetPath);
						OutSummary = OutError;
						return false;
					}
				}
				OutSummary = TEXT("Queried assets.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_make_directory"),
			TEXT("Create a content directory."),
			FSololmcpSchemaBuilder::Object({{TEXT("directory_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("directory_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString DirectoryPath;
				if (!Arguments->TryGetStringField(TEXT("directory_path"), DirectoryPath))
				{
					OutError = TEXT("Missing argument: directory_path");
					return false;
				}
				if (!Context.Services.MakeDirectory(DirectoryPath, OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("directoryPath"), DirectoryPath);
				OutSummary = TEXT("Created directory.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_list"),
			TEXT("List assets under a content directory."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("directory_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("recursive"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("include_folders"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("directory_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString DirectoryPath;
				if (!Arguments->TryGetStringField(TEXT("directory_path"), DirectoryPath))
				{
					OutError = TEXT("Missing argument: directory_path");
					return false;
				}
				const bool bRecursive = Arguments->HasTypedField<EJson::Boolean>(TEXT("recursive")) ? Arguments->GetBoolField(TEXT("recursive")) : true;
				const bool bIncludeFolders = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_folders")) ? Arguments->GetBoolField(TEXT("include_folders")) : false;
				const TArray<FString> Assets = Context.Services.ListAssets(DirectoryPath, bRecursive, bIncludeFolders, OutError);
				if (!OutError.IsEmpty())
				{
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> AssetJson;
				for (const FString& Asset : Assets)
				{
					AssetJson.Add(MakeShared<FJsonValueString>(Asset));
				}
				OutStructured->SetArrayField(TEXT("assets"), AssetJson);
				OutStructured->SetNumberField(TEXT("count"), AssetJson.Num());
				OutSummary = TEXT("Listed assets.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_save"),
			TEXT("Save an asset package."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("only_if_dirty"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}
				const bool bOnlyIfDirty = Arguments->HasTypedField<EJson::Boolean>(TEXT("only_if_dirty")) ? Arguments->GetBoolField(TEXT("only_if_dirty")) : true;
				if (!Context.Services.SaveAsset(AssetPath, bOnlyIfDirty, OutError))
				{
					return false;
				}
				FString ReloadError;
				UObject* ReloadedAsset = Context.Services.LoadAsset(AssetPath, ReloadError);
				if (!ReloadedAsset)
				{
					OutStructured->SetStringField(TEXT("error"), TEXT("asset_reload_failed_after_save"));
					if (!ReloadError.IsEmpty())
					{
						OutStructured->SetStringField(TEXT("reload_error"), ReloadError);
					}
					OutError = FString::Printf(TEXT("asset_reload_failed_after_save: %s"), *AssetPath);
					return false;
				}
				OutStructured->SetStringField(TEXT("assetPath"), AssetPath);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = TEXT("Saved asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_delete"),
			TEXT("Delete an asset package."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}
				FString CanonicalAssetPath = AssetPath;
				if (!CanonicalAssetPath.Contains(TEXT(".")))
				{
					FString PackageRoot;
					FString AssetLeaf;
					if (!CanonicalAssetPath.Split(TEXT("/"), &PackageRoot, &AssetLeaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
						|| AssetLeaf.IsEmpty())
					{
						OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
						return false;
					}
					CanonicalAssetPath += TEXT(".") + AssetLeaf;
				}
				const FString PackagePath = FPackageName::ObjectPathToPackageName(CanonicalAssetPath);
				const FString AssetFilename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
				const FString MapFilename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetMapPackageExtension());

				if (!Context.Services.DeleteAsset(CanonicalAssetPath, OutError))
				{
					const bool bRegistryMissing = !Context.Services.AssetExists(CanonicalAssetPath)
						&& !Context.Services.AssetExists(PackagePath);
					const bool bDiskMissing = !FPaths::FileExists(AssetFilename) && !FPaths::FileExists(MapFilename);
					if (PackagePath.StartsWith(TEXT("/Game/SOMOLMCP/Disposable")) && bRegistryMissing && bDiskMissing)
					{
						OutStructured->SetStringField(TEXT("assetPath"), CanonicalAssetPath);
						OutStructured->SetStringField(TEXT("packagePath"), PackagePath);
						OutStructured->SetBoolField(TEXT("deleted_verified"), true);
						OutStructured->SetBoolField(TEXT("already_missing"), true);
						OutSummary = TEXT("Disposable asset was already missing.");
						return true;
					}
					return false;
				}
				bool bRegistryExists = Context.Services.AssetExists(CanonicalAssetPath)
					|| Context.Services.AssetExists(PackagePath);
				bool bAssetFileExists = FPaths::FileExists(AssetFilename);
				bool bMapFileExists = FPaths::FileExists(MapFilename);
				bool bDiskFallbackAttempted = false;
				bool bDiskFallbackSucceeded = false;
				if (!bRegistryExists && (bAssetFileExists || bMapFileExists) && PackagePath.StartsWith(TEXT("/Game/")))
				{
					const FString ContentRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
					auto DeleteOrphanedPackageFile = [&ContentRoot](const FString& Candidate) -> bool
					{
						const FString FullCandidate = FPaths::ConvertRelativePathToFull(Candidate);
						if (!FullCandidate.StartsWith(ContentRoot, ESearchCase::IgnoreCase))
						{
							return false;
						}
						return !FPaths::FileExists(FullCandidate)
							|| IFileManager::Get().Delete(*FullCandidate, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
					};

					bDiskFallbackAttempted = true;
					const bool bAssetFileRemoved = !bAssetFileExists || DeleteOrphanedPackageFile(AssetFilename);
					const bool bMapFileRemoved = !bMapFileExists || DeleteOrphanedPackageFile(MapFilename);
					bAssetFileExists = FPaths::FileExists(AssetFilename);
					bMapFileExists = FPaths::FileExists(MapFilename);
					bDiskFallbackSucceeded = bAssetFileRemoved && bMapFileRemoved && !bAssetFileExists && !bMapFileExists;
				}
				if (bRegistryExists || bAssetFileExists || bMapFileExists)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
						TEXT("DeleteAsset returned success but the registry entry or package file still exists."));
					OutStructured->SetBoolField(TEXT("registry_exists"), bRegistryExists);
					OutStructured->SetBoolField(TEXT("asset_file_exists"), bAssetFileExists);
					OutStructured->SetBoolField(TEXT("map_file_exists"), bMapFileExists);
					OutStructured->SetBoolField(TEXT("disk_fallback_attempted"), bDiskFallbackAttempted);
					OutStructured->SetBoolField(TEXT("disk_fallback_succeeded"), bDiskFallbackSucceeded);
					OutStructured->SetStringField(TEXT("canonical_asset_path"), CanonicalAssetPath);
					OutStructured->SetStringField(TEXT("package_path"), PackagePath);
					OutError = FString::Printf(TEXT("Asset still exists after delete: %s"), *CanonicalAssetPath);
					return false;
				}
				OutStructured->SetStringField(TEXT("assetPath"), CanonicalAssetPath);
				OutStructured->SetStringField(TEXT("packagePath"), PackagePath);
				OutStructured->SetBoolField(TEXT("deleted_verified"), true);
				OutStructured->SetBoolField(TEXT("registry_missing"), true);
				OutStructured->SetBoolField(TEXT("package_files_missing"), true);
				OutStructured->SetBoolField(TEXT("disk_fallback_attempted"), bDiskFallbackAttempted);
				OutStructured->SetBoolField(TEXT("disk_fallback_succeeded"), bDiskFallbackSucceeded);
				OutSummary = TEXT("Deleted asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("tools_capabilities_describe"),
			TEXT("Return machine-readable capability metadata and recommended batch templates."),
			FSololmcpSchemaBuilder::Object({}),

			[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<TSharedPtr<FJsonValue>> BatchTemplates;
				{
					TSharedRef<FJsonObject> Template = MakeShared<FJsonObject>();
					Template->SetStringField(TEXT("name"), TEXT("material_pipeline"));
					Template->SetArrayField(TEXT("steps"), {
						MakeShared<FJsonValueString>(TEXT("material_create_expression")),
						MakeShared<FJsonValueString>(TEXT("material_connect_expressions")),
						MakeShared<FJsonValueString>(TEXT("material_connect_property")),
						MakeShared<FJsonValueString>(TEXT("material_recompile"))
		});
					BatchTemplates.Add(MakeShared<FJsonValueObject>(Template));
				}
				{
					TSharedRef<FJsonObject> Template = MakeShared<FJsonObject>();
					Template->SetStringField(TEXT("name"), TEXT("animation_notify_pipeline"));
					Template->SetArrayField(TEXT("steps"), {
						MakeShared<FJsonValueString>(TEXT("animation_add_notify_track")),
						MakeShared<FJsonValueString>(TEXT("animation_add_notify")),
						MakeShared<FJsonValueString>(TEXT("animation_list_notifies"))
		});
					BatchTemplates.Add(MakeShared<FJsonValueObject>(Template));
				}
				{
					TSharedRef<FJsonObject> Template = MakeShared<FJsonObject>();
					Template->SetStringField(TEXT("name"), TEXT("niagara_explosion_pipeline"));
					Template->SetArrayField(TEXT("steps"), {
						MakeShared<FJsonValueString>(TEXT("niagara_create_system")),
						MakeShared<FJsonValueString>(TEXT("niagara_create_emitter")),
						MakeShared<FJsonValueString>(TEXT("niagara_add_emitter_to_system")),
						MakeShared<FJsonValueString>(TEXT("niagara_compile_diagnostics"))
		});
					BatchTemplates.Add(MakeShared<FJsonValueObject>(Template));
				}
				{
					TSharedRef<FJsonObject> Template = MakeShared<FJsonObject>();
					Template->SetStringField(TEXT("name"), TEXT("niagara_regression_baseline"));
					Template->SetArrayField(TEXT("steps"), {
						MakeShared<FJsonValueString>(TEXT("niagara_regression_smoke_suite")),
						MakeShared<FJsonValueString>(TEXT("niagara_regression_baseline_save")),
						MakeShared<FJsonValueString>(TEXT("niagara_regression_baseline_compare")),
						MakeShared<FJsonValueString>(TEXT("niagara_regression_report_write"))
		});
					BatchTemplates.Add(MakeShared<FJsonValueObject>(Template));
				}

				TArray<TSharedPtr<FJsonValue>> SaveAwareTools;
				const TArray<FString> SaveAwareToolNames = {
					TEXT("material_create_expression"), TEXT("material_delete_expression"),
					TEXT("material_connect_property"), TEXT("material_connect_expressions"),
					TEXT("animation_add_notify"), TEXT("animation_add_curve"),
					TEXT("animation_remove_notifies_by_name"),
					TEXT("sequence_add_track"), TEXT("sequence_add_property_track"),
					TEXT("sequence_add_section"), TEXT("sequence_set_section_range"),
					TEXT("sequence_add_folder"), TEXT("sequence_set_marked_frames"),
					TEXT("blueprint_compile_diagnostics"),
					TEXT("character_anim_bind"),
					TEXT("animation_pipeline_smoke_test"),
					TEXT("blend_space_set_axis_settings"),
					TEXT("niagara_pipeline_template_run"),
					TEXT("niagara_regression_baseline_save"),
					TEXT("niagara_regression_report_write")
				};
				for (const FString& ToolName : SaveAwareToolNames)
				{
					SaveAwareTools.Add(MakeShared<FJsonValueString>(ToolName));
				}

				TArray<TSharedPtr<FJsonValue>> WriteVerifiedTools;
				const TArray<FString> WriteVerifiedToolNames = {
					TEXT("control_rig_add_controls"),
					TEXT("control_rig_add_nulls")
				};
				for (const FString& ToolName : WriteVerifiedToolNames)
				{
					WriteVerifiedTools.Add(MakeShared<FJsonValueString>(ToolName));
				}

				TSharedRef<FJsonObject> ToolImplementationStatus = MakeShared<FJsonObject>();
				ToolImplementationStatus->SetBoolField(TEXT("control_rig_add_controls"), true);
				ToolImplementationStatus->SetBoolField(TEXT("control_rig_add_nulls"), true);
				ToolImplementationStatus->SetBoolField(TEXT("control_rig_reparent_elements"), true);
				ToolImplementationStatus->SetBoolField(TEXT("control_rig_duplicate_elements"), true);
				ToolImplementationStatus->SetBoolField(TEXT("control_rig_mirror_elements"), true);

				TArray<TSharedPtr<FJsonValue>> UnimplementedTools;

				TArray<FString> RegisteredToolNames;
				Registry.GetRegisteredToolNamesSorted(RegisteredToolNames);
				TArray<TSharedPtr<FJsonValue>> RegisteredToolJson;
				for (const FString& N : RegisteredToolNames)
				{
					RegisteredToolJson.Add(MakeShared<FJsonValueString>(N));
				}

				OutStructured->SetStringField(TEXT("schemaVersion"), TEXT("1.2"));
				OutStructured->SetArrayField(TEXT("batchTemplates"), BatchTemplates);
				OutStructured->SetArrayField(TEXT("saveAwareTools"), SaveAwareTools);
				OutStructured->SetArrayField(TEXT("writeVerifiedTools"), WriteVerifiedTools);
				OutStructured->SetObjectField(TEXT("toolImplementationStatus"), ToolImplementationStatus);
				OutStructured->SetArrayField(TEXT("unimplementedTools"), UnimplementedTools);
				OutStructured->SetArrayField(TEXT("registeredToolNames"), RegisteredToolJson);
				OutStructured->SetNumberField(TEXT("registeredToolCount"), RegisteredToolNames.Num());
				OutStructured->SetStringField(TEXT("notes"), TEXT("Use *_batch_edit for single-domain pipelines, or tools_batch_execute for cross-domain orchestration with optional rollback."));
				OutSummary = TEXT("Described tool capabilities.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("tools_batch_execute"),
			TEXT("Cross-domain batch executor with structured step envelopes and optional rollback-on-error."),
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
					if (ToolName == TEXT("tools_batch_execute"))
					{
						OutError = TEXT("tools_batch_execute cannot call itself.");
						return false;
					}

					TSharedRef<FJsonObject> StepStructured = MakeShared<FJsonObject>();
					FString StepSummary;
					FString StepError;
					const bool bStepOk = Registry.ExecuteTool(ToolName, OpArgsPtr->ToSharedRef(), StepStructured, StepSummary, StepError);

					TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
					StepResult->SetNumberField(TEXT("index"), Index);
					StepResult->SetStringField(TEXT("tool"), ToolName);
					StepResult->SetStringField(TEXT("domain"), ToolName.Contains(TEXT("_")) ? ToolName.Left(ToolName.Find(TEXT("_"))) : TEXT("generic"));
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
							OutStructured->SetStringField(TEXT("errorCode"), TEXT("batch_step_failed"));
							OutError = FString::Printf(TEXT("tools_batch_execute failed at operation %d: %s"), Index, *StepError);
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
				OutSummary = FString::Printf(TEXT("Executed %d operations (%d success, %d failed)."), Results.Num(), SuccessCount, FailureCount);
				if (FailureCount > 0 && SuccessCount == 0)
				{
					OutError = TEXT("All batch operations failed.");
					return false;
				}
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_duplicate"),
			TEXT("Duplicate an asset package."),
			FSololmcpSchemaBuilder::Object({{TEXT("source_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("destination_asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("source_asset_path"), TEXT("destination_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SourceAssetPath;
				FString DestinationAssetPath;
				if (!Arguments->TryGetStringField(TEXT("source_asset_path"), SourceAssetPath) || !Arguments->TryGetStringField(TEXT("destination_asset_path"), DestinationAssetPath))
				{
					OutError = TEXT("Missing source_asset_path or destination_asset_path.");
					return false;
				}
				UObject* Asset = Context.Services.DuplicateAsset(SourceAssetPath, DestinationAssetPath, OutError);
				if (!Asset)
				{
					return false;
				}
				if (!Context.Services.AssetExists(Asset->GetPathName()))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("destination_asset_path"),
						TEXT("DuplicateAsset returned an object but the duplicated asset is not visible in the asset registry."));
					OutError = FString::Printf(TEXT("Duplicated asset was not persisted: %s"), *Asset->GetPathName());
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutStructured->SetBoolField(TEXT("duplicate_verified"), true);
				OutSummary = TEXT("Duplicated asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_rename"),
			TEXT("Rename or move an asset package."),
			FSololmcpSchemaBuilder::Object({{TEXT("source_asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("destination_asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("source_asset_path"), TEXT("destination_asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SourceAssetPath;
				FString DestinationAssetPath;
				if (!Arguments->TryGetStringField(TEXT("source_asset_path"), SourceAssetPath) || !Arguments->TryGetStringField(TEXT("destination_asset_path"), DestinationAssetPath))
				{
					OutError = TEXT("Missing source_asset_path or destination_asset_path.");
					return false;
				}
				if (!Context.Services.RenameAsset(SourceAssetPath, DestinationAssetPath, OutError))
				{
					return false;
				}
				if (!Context.Services.AssetExists(DestinationAssetPath) || Context.Services.AssetExists(SourceAssetPath))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("destination_asset_path"),
						TEXT("RenameAsset returned success but the source/destination asset registry state did not verify."));
					OutStructured->SetBoolField(TEXT("source_still_exists"), Context.Services.AssetExists(SourceAssetPath));
					OutStructured->SetBoolField(TEXT("destination_exists"), Context.Services.AssetExists(DestinationAssetPath));
					OutError = FString::Printf(TEXT("Asset rename verification failed: %s -> %s"), *SourceAssetPath, *DestinationAssetPath);
					return false;
				}
				OutStructured->SetStringField(TEXT("destination"), DestinationAssetPath);
				OutStructured->SetBoolField(TEXT("rename_verified"), true);
				OutSummary = TEXT("Renamed asset.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_open_editor"),
			TEXT("Open an asset in the relevant editor."),
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
				if (!Asset)
				{
					return false;
				}
				FString Error;
				UAssetEditorSubsystem* AssetEditorSubsystem = Context.Services.GetAssetEditorSubsystem(Error);
				if (!AssetEditorSubsystem)
				{
					OutError = Error;
					return false;
				}
				TWeakObjectPtr<UObject> AssetPtr(Asset);
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([AssetPtr](float)
				{
					if (UObject* DeferredAsset = AssetPtr.Get())
					{
						if (GEditor)
						{
							if (UAssetEditorSubsystem* DeferredSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
							{
								DeferredSubsystem->OpenEditorForAsset(DeferredAsset);
							}
						}
					}
					return false;
				}), 0.01f);
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutStructured->SetBoolField(TEXT("open_scheduled"), true);
				OutSummary = TEXT("Scheduled asset editor open.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("asset_dependencies"),
			TEXT("Collect direct dependencies for an asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}
				const TArray<FString> Dependencies = Context.Services.GetAssetDependencies(AssetPath, OutError);
				if (!OutError.IsEmpty() && Dependencies.Num() == 0)
				{
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> DependencyJson;
				for (const FString& Dependency : Dependencies)
				{
					DependencyJson.Add(MakeShared<FJsonValueString>(Dependency));
				}
				OutStructured->SetArrayField(TEXT("dependencies"), DependencyJson);
				OutStructured->SetNumberField(TEXT("count"), DependencyJson.Num());
				OutSummary = TEXT("Collected asset dependencies.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("import_asset"),
			TEXT("Import one or more source files into a destination content path."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("source_files"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())},
					{TEXT("destination_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("save"), FSololmcpSchemaBuilder::Boolean()},
					{TEXT("automated"), FSololmcpSchemaBuilder::Boolean()}
				},
				{TEXT("source_files"), TEXT("destination_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<FString> SourceFiles;
				FString DestinationPath;
				if (!TryGetStringArray(Arguments, TEXT("source_files"), SourceFiles) || !Arguments->TryGetStringField(TEXT("destination_path"), DestinationPath))
				{
					OutError = TEXT("Missing source_files or destination_path.");
					return false;
				}

				const bool bReplaceExisting = Arguments->HasTypedField<EJson::Boolean>(TEXT("replace_existing")) ? Arguments->GetBoolField(TEXT("replace_existing")) : true;
				const bool bSave = Arguments->HasTypedField<EJson::Boolean>(TEXT("save")) ? Arguments->GetBoolField(TEXT("save")) : true;
				const bool bAutomated = Arguments->HasTypedField<EJson::Boolean>(TEXT("automated")) ? Arguments->GetBoolField(TEXT("automated")) : true;

				TArray<UAssetImportTask*> Tasks;
				for (const FString& SourceFile : SourceFiles)
				{
					if (!FPaths::FileExists(SourceFile))
					{
						SololmcpError::InvalidPath(OutStructured, SourceFile);
						OutError = FString::Printf(TEXT("Source file does not exist: %s"), *SourceFile);
						return false;
					}
					UAssetImportTask* Task = NewObject<UAssetImportTask>(GetTransientPackage());
					Task->Filename = SourceFile;
					Task->DestinationPath = DestinationPath;
					Task->bAutomated = bAutomated;
					Task->bReplaceExisting = bReplaceExisting;
					Task->bSave = bSave;
					Tasks.Add(Task);
				}

				FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
				AssetToolsModule.Get().ImportAssetTasks(Tasks);

				TArray<TSharedPtr<FJsonValue>> ImportedAssets;
				TArray<TSharedPtr<FJsonValue>> FailedFiles;
				for (UAssetImportTask* Task : Tasks)
				{
					const int32 BeforeCount = ImportedAssets.Num();
					for (UObject* Object : Task->GetObjects())
					{
						ImportedAssets.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeObjectReference(Object)));
					}
					if (ImportedAssets.Num() == BeforeCount)
					{
						FailedFiles.Add(MakeShared<FJsonValueString>(Task ? Task->Filename : TEXT("(null task)")));
					}
				}
				OutStructured->SetArrayField(TEXT("assets"), ImportedAssets);
				OutStructured->SetArrayField(TEXT("failed_files"), FailedFiles);
				OutStructured->SetNumberField(TEXT("count"), ImportedAssets.Num());
				OutStructured->SetNumberField(TEXT("failed_count"), FailedFiles.Num());
				if (ImportedAssets.Num() == 0)
				{
					SololmcpError::Set(OutStructured, TEXT("IMPORT_FAILED"), TEXT("source_files"),
						TEXT("No source files produced imported assets."));
					OutError = FString::Printf(TEXT("Imported %d assets; %d source files failed."), ImportedAssets.Num(), FailedFiles.Num());
					return false;
				}
				OutStructured->SetStringField(TEXT("code"), FailedFiles.Num() > 0 ? TEXT("partial_success") : TEXT("ok"));
				OutSummary = FailedFiles.Num() > 0 ? TEXT("Imported assets with some source file failures.") : TEXT("Imported assets.");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({
			TEXT("reimport_asset"),
			TEXT("Reimport an existing asset from its source file."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("automated"), FSololmcpSchemaBuilder::Boolean()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset)
				{
					return false;
				}
				TArray<FString> ReimportSources;
				if (!FReimportManager::Instance()->CanReimport(Asset, &ReimportSources))
				{
					OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
					OutStructured->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
					OutStructured->SetArrayField(TEXT("reimport_sources"), {});
					OutError = FString::Printf(TEXT("Asset is not reimportable: %s"), *AssetPath);
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> SourceValues;
				bool bHasExistingSource = false;
				for (const FString& Source : ReimportSources)
				{
					SourceValues.Add(MakeShared<FJsonValueString>(Source));
					if (!Source.IsEmpty() && FPaths::FileExists(Source))
					{
						bHasExistingSource = true;
					}
				}
				if (!bHasExistingSource)
				{
					OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
					OutStructured->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
					OutStructured->SetArrayField(TEXT("reimport_sources"), SourceValues);
					OutError = FString::Printf(TEXT("Asset has no existing source file for reimport: %s"), *AssetPath);
					return false;
				}
				UClass* OriginalClass = Asset->GetClass();
				const bool bAutomated = Arguments->HasTypedField<EJson::Boolean>(TEXT("automated")) ? Arguments->GetBoolField(TEXT("automated")) : true;
				#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				// 5.5 appended bInForceShowDialog; passing false matches 5.4's behaviour,
				// so the two branches are equivalent rather than merely similar.
				if (!FReimportManager::Instance()->Reimport(Asset, false, true, FString(), nullptr, INDEX_NONE, false, bAutomated, false))
#else
				if (!FReimportManager::Instance()->Reimport(Asset, false, true, FString(), nullptr, INDEX_NONE, false, bAutomated))
#endif
				{
					OutError = TEXT("Failed to reimport asset.");
					return false;
				}
				FString ReloadError;
				UObject* ReloadedAsset = Context.Services.LoadAsset(AssetPath, ReloadError);
				if (!ReloadedAsset || !ReloadedAsset->IsA(OriginalClass))
				{
					OutStructured->SetStringField(TEXT("error"), TEXT("asset_reload_failed_after_reimport"));
					if (!ReloadError.IsEmpty())
					{
						OutStructured->SetStringField(TEXT("reload_error"), ReloadError);
					}
					OutStructured->SetStringField(TEXT("expected_class"), OriginalClass ? OriginalClass->GetPathName() : FString());
					OutStructured->SetStringField(TEXT("actual_class"), ReloadedAsset ? ReloadedAsset->GetClass()->GetPathName() : FString());
					OutError = FString::Printf(TEXT("asset_reload_failed_after_reimport: %s"), *AssetPath);
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeObjectReference(Asset);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = TEXT("Reimported asset.");
				return true;
			}
		, nullptr
		, 5
		});
	}
}
