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
// FGeometryCollection and TManagedArray reach this file transitively on 5.4+ but not
// on 5.3, where the declaration in an if-condition then parses as an expression and
// reports a syntax error rather than a missing type. Included explicitly so the TU
// does not depend on another header's include graph.
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/ManagedArray.h"
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

// UPCGNode::GetNodeTitle took no argument until 5.4 added EPCGNodeTitleType, an enum
// 5.3 does not define at all. FullTitle is the pre-5.4 behaviour, so the two agree.
namespace
{
	inline FString SomolPcgNodeTitle(const UPCGNode* N)
	{
		if (!N) { return FString(); }
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		return N->GetNodeTitle(EPCGNodeTitleType::FullTitle).ToString();
#else
		return N->GetNodeTitle().ToString();
#endif
	}
}
	void RegisterProjectPerceptionTools(FSololmcpToolRegistry& Registry)
	{
		// ---- texture_analyze ----
		Registry.Register({
			TEXT("texture_analyze"),
			TEXT("Analyze a texture asset returning type, format, resolution, channels, compression, sRGB, memory estimate and source info."),
			FSololmcpSchemaBuilder::Object(
				{{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the texture asset"))}},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}
				UTexture* Texture = Cast<UTexture>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Texture)
				{
					OutError = TEXT("Asset is not a texture.");
					return false;
				}

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), Texture->GetName());
				OutStructured->SetStringField(TEXT("texture_type"), Texture->GetClass()->GetName());

				// UE5.7.4: GetPixelFormat removed from UTexture. Use resource format if available.
#if WITH_ENGINE
				EPixelFormat PixelFormat = PF_Unknown;
				if (Texture->GetResource() && Texture->GetResource()->TextureRHI.IsValid())
				{
					PixelFormat = Texture->GetResource()->TextureRHI->GetFormat();
				}
				OutStructured->SetStringField(TEXT("format"), GPixelFormats[PixelFormat].Name);
#else
				OutStructured->SetStringField(TEXT("format"), TEXT("unknown"));
#endif

				int32 Width = 0, Height = 0;
				if (UTexture2D* Tex2D = Cast<UTexture2D>(Texture))
				{
					Width = Tex2D->GetSizeX();
					Height = Tex2D->GetSizeY();
				}
				else if (UTextureCube* TexCube = Cast<UTextureCube>(Texture))
				{
					Width = TexCube->GetSizeX();
					Height = TexCube->GetSizeX();
				}
				else if (UTextureRenderTarget2D* RT = Cast<UTextureRenderTarget2D>(Texture))
				{
					Width = RT->SizeX;
					Height = RT->SizeY;
				}
				TSharedRef<FJsonObject> SizeJson = MakeShared<FJsonObject>();
				SizeJson->SetNumberField(TEXT("width"), Width);
				SizeJson->SetNumberField(TEXT("height"), Height);
				OutStructured->SetObjectField(TEXT("size"), SizeJson);

				int32 ChannelCount = 4;
				bool bHasAlpha = true;
				switch (PixelFormat)
				{
				case EPixelFormat::PF_G8: case EPixelFormat::PF_R8: case EPixelFormat::PF_R16F: case EPixelFormat::PF_R32_FLOAT:
					ChannelCount = 1; bHasAlpha = false; break;
				case EPixelFormat::PF_G16R16: case EPixelFormat::PF_G16R16F: case EPixelFormat::PF_R8G8:
					ChannelCount = 2; bHasAlpha = false; break;
				// UE 5.7: PF_R11G11B10_F renamed to PF_FloatR11G11B10
				case EPixelFormat::PF_FloatRGB: case EPixelFormat::PF_FloatR11G11B10:
					ChannelCount = 3; bHasAlpha = false; break;
				default: break;
				}
				OutStructured->SetNumberField(TEXT("channel_count"), ChannelCount);
				OutStructured->SetBoolField(TEXT("has_alpha"), bHasAlpha);
				OutStructured->SetStringField(TEXT("compression_settings"),
					StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(static_cast<int64>(Texture->CompressionSettings)));
				OutStructured->SetBoolField(TEXT("is_srgb"), Texture->SRGB);
				OutStructured->SetBoolField(TEXT("is_normal_map"), Texture->CompressionSettings == TC_Normalmap);
				OutStructured->SetNumberField(TEXT("lod_bias"), Texture->LODBias);
				OutStructured->SetNumberField(TEXT("max_texture_size"), Texture->MaxTextureSize);
				OutStructured->SetStringField(TEXT("filter"),
					StaticEnum<TextureFilter>()->GetNameStringByValue(static_cast<int64>(Texture->Filter)));

				const double MemoryMB = (static_cast<double>(Width) * Height * GPixelFormats[PixelFormat].BlockBytes) / (1024.0 * 1024.0);
				OutStructured->SetNumberField(TEXT("memory_estimate_mb"), FMath::RoundToDouble(MemoryMB * 100.0) / 100.0);

				if (UTexture2D* Tex2D = Cast<UTexture2D>(Texture))
				{
					TSharedRef<FJsonObject> SourceInfo = MakeShared<FJsonObject>();
					SourceInfo->SetNumberField(TEXT("source_width"), Tex2D->Source.GetSizeX());
					SourceInfo->SetNumberField(TEXT("source_height"), Tex2D->Source.GetSizeY());
					SourceInfo->SetNumberField(TEXT("num_mips"), Tex2D->GetNumMips());
					OutStructured->SetObjectField(TEXT("source_info"), SourceInfo);
				}

				OutSummary = FString::Printf(TEXT("Analyzed texture: %s (%dx%d %s)"), *Texture->GetName(), Width, Height, GPixelFormats[PixelFormat].Name);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- texture_batch_report ----
		Registry.Register({
			TEXT("texture_batch_report"),
			TEXT("Batch-analyze all textures under a directory. Returns per-format/size statistics and potential issues."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("directory_path"), FSololmcpSchemaBuilder::String(TEXT("Content directory path"))},
					{TEXT("recursive"), FSololmcpSchemaBuilder::Boolean(TEXT("Search subdirectories (default true)"))},
					{TEXT("max_count"), FSololmcpSchemaBuilder::Integer(TEXT("Max textures to analyze (default 200)"))}
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
				const int32 MaxCount = Arguments->HasTypedField<EJson::Number>(TEXT("max_count")) ? static_cast<int32>(Arguments->GetNumberField(TEXT("max_count"))) : 200;

				TArray<FAssetData> TextureAssets = Context.Services.QueryAssets(DirectoryPath, TEXT("/Script/Engine.Texture2D"), bRecursive, OutError);
				if (!OutError.IsEmpty() && TextureAssets.Num() == 0) return false;

				int32 Count = 0;
				double TotalMemoryMB = 0.0;
				TMap<FString, int32> ByFormat;
				TMap<FString, int32> BySizeRange;
				TArray<TSharedPtr<FJsonValue>> Issues;
				TArray<TSharedPtr<FJsonValue>> TextureList;

				for (const FAssetData& AssetData : TextureAssets)
				{
					if (Count >= MaxCount) break;
					UTexture2D* Tex = Cast<UTexture2D>(AssetData.GetAsset());
					if (!Tex) continue;
					Count++;

					const int32 W = Tex->GetSizeX(), H = Tex->GetSizeY();
					const EPixelFormat PF = Tex->GetPixelFormat();
					const FString FormatName = GPixelFormats[PF].Name;
					ByFormat.FindOrAdd(FormatName, 0)++;

					const int32 MaxDim = FMath::Max(W, H);
					FString SizeRange = MaxDim >= 4096 ? TEXT("4K+") : MaxDim >= 2048 ? TEXT("2K-4K") : MaxDim >= 1024 ? TEXT("1K-2K") : TEXT("<1K");
					BySizeRange.FindOrAdd(SizeRange, 0)++;

					const double MemMB = (static_cast<double>(W) * H * GPixelFormats[PF].BlockBytes) / (1024.0 * 1024.0);
					TotalMemoryMB += MemMB;

					if (!FMath::IsPowerOfTwo(W) || !FMath::IsPowerOfTwo(H))
					{
						TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("asset"), AssetData.GetObjectPathString());
						Issue->SetStringField(TEXT("issue"), TEXT("non_power_of_two"));
						Issue->SetStringField(TEXT("severity"), TEXT("warning"));
						Issues.Add(MakeShared<FJsonValueObject>(Issue));
					}

					TSharedRef<FJsonObject> TexEntry = MakeShared<FJsonObject>();
					TexEntry->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
					TexEntry->SetStringField(TEXT("name"), Tex->GetName());
					TexEntry->SetNumberField(TEXT("width"), W);
					TexEntry->SetNumberField(TEXT("height"), H);
					TexEntry->SetStringField(TEXT("format"), FormatName);
					TexEntry->SetNumberField(TEXT("memory_mb"), FMath::RoundToDouble(MemMB * 100.0) / 100.0);
					TextureList.Add(MakeShared<FJsonValueObject>(TexEntry));
				}

				OutStructured->SetStringField(TEXT("directory"), DirectoryPath);
				OutStructured->SetNumberField(TEXT("total_count"), Count);
				OutStructured->SetNumberField(TEXT("total_memory_estimate_mb"), FMath::RoundToDouble(TotalMemoryMB * 100.0) / 100.0);
				TSharedRef<FJsonObject> ByFormatJson = MakeShared<FJsonObject>();
				for (const auto& P : ByFormat) ByFormatJson->SetNumberField(P.Key, P.Value);
				OutStructured->SetObjectField(TEXT("by_format"), ByFormatJson);
				TSharedRef<FJsonObject> BySizeJson = MakeShared<FJsonObject>();
				for (const auto& P : BySizeRange) BySizeJson->SetNumberField(P.Key, P.Value);
				OutStructured->SetObjectField(TEXT("by_size_range"), BySizeJson);
				OutStructured->SetArrayField(TEXT("issues"), Issues);
				OutStructured->SetArrayField(TEXT("textures"), TextureList);
				OutSummary = FString::Printf(TEXT("Analyzed %d textures, total ~%.1f MB"), Count, TotalMemoryMB);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- static_mesh_analyze ----
		Registry.Register({
			TEXT("static_mesh_analyze"),
			TEXT("Comprehensive static mesh analysis: LODs (vertex/triangle counts), sockets, collision primitives, material slots, Nanite, bounds, UV channels."),
			FSololmcpSchemaBuilder::Object(
				{{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the static mesh asset"))}},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a static mesh."); return false; }

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), Mesh->GetName());

				// LODs
				TArray<TSharedPtr<FJsonValue>> LodArray;
				for (int32 i = 0; i < Mesh->GetNumLODs(); ++i)
				{
					TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
					L->SetNumberField(TEXT("lod_index"), i);
					const FStaticMeshLODResources& LODRes = Mesh->GetLODForExport(i);
					L->SetNumberField(TEXT("vertex_count"), LODRes.GetNumVertices());
					L->SetNumberField(TEXT("triangle_count"), LODRes.GetNumTriangles());
					L->SetNumberField(TEXT("section_count"), LODRes.Sections.Num());
					const FStaticMeshSourceModel* SM = Mesh->IsSourceModelValid(i) ? &Mesh->GetSourceModel(i) : nullptr;
					if (SM) L->SetNumberField(TEXT("screen_size"), SM->ScreenSize.Default);
					LodArray.Add(MakeShared<FJsonValueObject>(L));
				}
				OutStructured->SetArrayField(TEXT("lod_info"), LodArray);

				// Sockets
				TArray<TSharedPtr<FJsonValue>> SocketArr;
				for (UStaticMeshSocket* Socket : Mesh->Sockets)
				{
					if (!Socket) continue;
					TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
					S->SetStringField(TEXT("name"), Socket->SocketName.ToString());
					S->SetObjectField(TEXT("relative_location"), VectorToJson(Socket->RelativeLocation));
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetNumberField(TEXT("pitch"), Socket->RelativeRotation.Pitch);
					R->SetNumberField(TEXT("yaw"), Socket->RelativeRotation.Yaw);
					R->SetNumberField(TEXT("roll"), Socket->RelativeRotation.Roll);
					S->SetObjectField(TEXT("relative_rotation"), R);
					S->SetObjectField(TEXT("relative_scale"), VectorToJson(Socket->RelativeScale));
					SocketArr.Add(MakeShared<FJsonValueObject>(S));
				}
				OutStructured->SetArrayField(TEXT("sockets"), SocketArr);

				// Collision
				TSharedRef<FJsonObject> CollJson = MakeShared<FJsonObject>();
				const UBodySetup* BS = Mesh->GetBodySetup();
				if (BS)
				{
					CollJson->SetBoolField(TEXT("has_collision"), true);
					CollJson->SetStringField(TEXT("collision_complexity"),
						StaticEnum<ECollisionTraceFlag>()->GetNameStringByValue(static_cast<int64>(BS->CollisionTraceFlag)));
					TArray<TSharedPtr<FJsonValue>> Prims;
					for (const FKBoxElem& B : BS->AggGeom.BoxElems) { TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("box")); P->SetStringField(TEXT("name"), B.GetName().ToString()); P->SetNumberField(TEXT("x"), B.X); P->SetNumberField(TEXT("y"), B.Y); P->SetNumberField(TEXT("z"), B.Z); P->SetObjectField(TEXT("center"), VectorToJson(B.Center)); Prims.Add(MakeShared<FJsonValueObject>(P)); }
					for (const FKSphereElem& Sp : BS->AggGeom.SphereElems) { TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("sphere")); P->SetStringField(TEXT("name"), Sp.GetName().ToString()); P->SetNumberField(TEXT("radius"), Sp.Radius); P->SetObjectField(TEXT("center"), VectorToJson(Sp.Center)); Prims.Add(MakeShared<FJsonValueObject>(P)); }
					for (const FKSphylElem& Cap : BS->AggGeom.SphylElems) { TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("capsule")); P->SetStringField(TEXT("name"), Cap.GetName().ToString()); P->SetNumberField(TEXT("radius"), Cap.Radius); P->SetNumberField(TEXT("length"), Cap.Length); P->SetObjectField(TEXT("center"), VectorToJson(Cap.Center)); Prims.Add(MakeShared<FJsonValueObject>(P)); }
					CollJson->SetNumberField(TEXT("convex_count"), BS->AggGeom.ConvexElems.Num());
					CollJson->SetArrayField(TEXT("primitives"), Prims);
				}
				else { CollJson->SetBoolField(TEXT("has_collision"), false); }
				OutStructured->SetObjectField(TEXT("collision"), CollJson);

				// Materials
				TArray<TSharedPtr<FJsonValue>> MatArr;
				const TArray<FStaticMaterial>& Mats = Mesh->GetStaticMaterials();
				for (int32 i = 0; i < Mats.Num(); ++i)
				{
					TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
					M->SetNumberField(TEXT("slot_index"), i);
					M->SetStringField(TEXT("slot_name"), Mats[i].MaterialSlotName.ToString());
					M->SetStringField(TEXT("material_path"), Mats[i].MaterialInterface ? Mats[i].MaterialInterface->GetPathName() : FString());
					MatArr.Add(MakeShared<FJsonValueObject>(M));
				}
				OutStructured->SetArrayField(TEXT("materials"), MatArr);

				// Nanite
				TSharedRef<FJsonObject> NanJson = MakeShared<FJsonObject>();
				NanJson->SetBoolField(TEXT("enabled"), Mesh->NaniteSettings.bEnabled);
				OutStructured->SetObjectField(TEXT("nanite"), NanJson);

				// Bounds
				const FBoxSphereBounds Bounds = Mesh->GetBounds();
				TSharedRef<FJsonObject> BJson = MakeShared<FJsonObject>();
				BJson->SetObjectField(TEXT("origin"), VectorToJson(Bounds.Origin));
				BJson->SetObjectField(TEXT("box_extent"), VectorToJson(Bounds.BoxExtent));
				BJson->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
				OutStructured->SetObjectField(TEXT("bounds"), BJson);

				// UV & vertex colors
				if (Mesh->GetNumLODs() > 0)
				{
					const FStaticMeshLODResources& LOD0 = Mesh->GetLODForExport(0);
					OutStructured->SetNumberField(TEXT("uv_channels"), LOD0.GetNumTexCoords());
					OutStructured->SetBoolField(TEXT("has_vertex_colors"), LOD0.bHasColorVertexData);
				}

				OutSummary = FString::Printf(TEXT("Analyzed static mesh: %s (%d LODs, %d sockets, %d materials)"), *Mesh->GetName(), Mesh->GetNumLODs(), Mesh->Sockets.Num(), Mats.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- skeletal_mesh_analyze ----
		Registry.Register({
			TEXT("skeletal_mesh_analyze"),
			TEXT("Comprehensive skeletal mesh analysis: skeleton/bones, LODs, sockets, materials, physics asset, morph targets, bounds."),
			FSololmcpSchemaBuilder::Object(
				{{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the skeletal mesh asset"))}},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Mesh) { OutError = TEXT("Asset is not a skeletal mesh."); return false; }

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), Mesh->GetName());

				// Skeleton
				TSharedRef<FJsonObject> SkelJson = MakeShared<FJsonObject>();
				const USkeleton* Skeleton = Mesh->GetSkeleton();
				if (Skeleton)
				{
					SkelJson->SetStringField(TEXT("path"), Skeleton->GetPathName());
					const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
					SkelJson->SetNumberField(TEXT("bone_count"), RefSkel.GetNum());
					if (RefSkel.GetNum() > 0) SkelJson->SetStringField(TEXT("root_bone"), RefSkel.GetBoneName(0).ToString());
					TArray<TSharedPtr<FJsonValue>> BoneArr;
					const int32 MaxBones = FMath::Min(RefSkel.GetNum(), 100);
					for (int32 i = 0; i < MaxBones; ++i)
					{
						TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
						BJ->SetStringField(TEXT("name"), RefSkel.GetBoneName(i).ToString());
						BJ->SetNumberField(TEXT("index"), i);
						const int32 ParentIdx = RefSkel.GetParentIndex(i);
						BJ->SetNumberField(TEXT("parent_index"), ParentIdx);
						if (ParentIdx >= 0) BJ->SetStringField(TEXT("parent_name"), RefSkel.GetBoneName(ParentIdx).ToString());
						BoneArr.Add(MakeShared<FJsonValueObject>(BJ));
					}
					SkelJson->SetArrayField(TEXT("bones"), BoneArr);
				}
				else { SkelJson->SetNumberField(TEXT("bone_count"), 0); }
				OutStructured->SetObjectField(TEXT("skeleton"), SkelJson);

				// LODs
				TArray<TSharedPtr<FJsonValue>> LodArr;
				for (int32 i = 0; i < Mesh->GetLODNum(); ++i)
				{
					TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
					L->SetNumberField(TEXT("lod_index"), i);
					const FSkeletalMeshLODInfo* LI = Mesh->GetLODInfo(i);
					if (LI) L->SetNumberField(TEXT("screen_size"), LI->ScreenSize.Default);
					LodArr.Add(MakeShared<FJsonValueObject>(L));
				}
				OutStructured->SetArrayField(TEXT("lod_info"), LodArr);

				// Sockets
				TArray<TSharedPtr<FJsonValue>> SockArr;
				if (Skeleton)
				{
					TArray<USkeletalMeshSocket*> AllSockets = Mesh->GetActiveSocketList();
					for (const USkeletalMeshSocket* Sock : AllSockets)
					{
						if (!Sock) continue;
						TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
						SJ->SetStringField(TEXT("name"), Sock->SocketName.ToString());
						SJ->SetStringField(TEXT("bone_name"), Sock->BoneName.ToString());
						SJ->SetObjectField(TEXT("relative_location"), VectorToJson(Sock->RelativeLocation));
						TSharedRef<FJsonObject> RJ = MakeShared<FJsonObject>();
						RJ->SetNumberField(TEXT("pitch"), Sock->RelativeRotation.Pitch);
						RJ->SetNumberField(TEXT("yaw"), Sock->RelativeRotation.Yaw);
						RJ->SetNumberField(TEXT("roll"), Sock->RelativeRotation.Roll);
						SJ->SetObjectField(TEXT("relative_rotation"), RJ);
						SJ->SetObjectField(TEXT("relative_scale"), VectorToJson(Sock->RelativeScale));
						SockArr.Add(MakeShared<FJsonValueObject>(SJ));
					}
				}
				OutStructured->SetArrayField(TEXT("sockets"), SockArr);

				// Materials
				TArray<TSharedPtr<FJsonValue>> MatArr;
				const TArray<FSkeletalMaterial>& Mats = Mesh->GetMaterials();
				for (int32 i = 0; i < Mats.Num(); ++i)
				{
					TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
					M->SetNumberField(TEXT("slot_index"), i);
					M->SetStringField(TEXT("slot_name"), Mats[i].MaterialSlotName.ToString());
					M->SetStringField(TEXT("material_path"), Mats[i].MaterialInterface ? Mats[i].MaterialInterface->GetPathName() : FString());
					MatArr.Add(MakeShared<FJsonValueObject>(M));
				}
				OutStructured->SetArrayField(TEXT("materials"), MatArr);

				// Physics asset
				TSharedRef<FJsonObject> PhysJson = MakeShared<FJsonObject>();
				UPhysicsAsset* PhysAsset = Mesh->GetPhysicsAsset();
				if (PhysAsset)
				{
					PhysJson->SetBoolField(TEXT("has_physics"), true);
					PhysJson->SetStringField(TEXT("path"), PhysAsset->GetPathName());
					PhysJson->SetNumberField(TEXT("bodies_count"), PhysAsset->SkeletalBodySetups.Num());
					PhysJson->SetNumberField(TEXT("constraints_count"), PhysAsset->ConstraintSetup.Num());
				}
				else { PhysJson->SetBoolField(TEXT("has_physics"), false); }
				OutStructured->SetObjectField(TEXT("physics_asset"), PhysJson);

				// Morph targets
				TArray<TSharedPtr<FJsonValue>> MorphArr;
				const TArray<UMorphTarget*>& Morphs = Mesh->GetMorphTargets();
				for (const UMorphTarget* MT : Morphs)
				{
					if (!MT) continue;
					TSharedRef<FJsonObject> MJ = MakeShared<FJsonObject>();
					MJ->SetStringField(TEXT("name"), MT->GetName());
					MorphArr.Add(MakeShared<FJsonValueObject>(MJ));
				}
				OutStructured->SetArrayField(TEXT("morph_targets"), MorphArr);

				// Bounds
				const FBoxSphereBounds Bounds = Mesh->GetBounds();
				TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
				BJ->SetObjectField(TEXT("origin"), VectorToJson(Bounds.Origin));
				BJ->SetObjectField(TEXT("box_extent"), VectorToJson(Bounds.BoxExtent));
				BJ->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
				OutStructured->SetObjectField(TEXT("bounds"), BJ);

				OutSummary = FString::Printf(TEXT("Analyzed skeletal mesh: %s (%d LODs, %d sockets, %d materials, %d morphs)"),
					*Mesh->GetName(), Mesh->GetLODNum(), SockArr.Num(), Mats.Num(), Morphs.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- animation_analyze ----
		Registry.Register({
			TEXT("animation_analyze"),
			TEXT("Analyze an animation sequence: duration, frame count/rate, notifies, curves, skeleton binding."),
			FSololmcpSchemaBuilder::Object(
				{{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the animation asset"))}},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UAnimSequenceBase* AnimSeq = Cast<UAnimSequenceBase>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!AnimSeq) { OutError = TEXT("Asset is not an animation sequence."); return false; }

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), AnimSeq->GetName());
				OutStructured->SetStringField(TEXT("asset_type"), AnimSeq->GetClass()->GetName());

				TSharedRef<FJsonObject> AnimInfo = MakeShared<FJsonObject>();
				const double Duration = AnimSeq->GetPlayLength();
				AnimInfo->SetNumberField(TEXT("duration_seconds"), Duration);
				if (const IAnimationDataModel* DataModel = AnimSeq->GetDataModel())
				{
					AnimInfo->SetNumberField(TEXT("frame_count"), DataModel->GetNumberOfFrames());
					AnimInfo->SetNumberField(TEXT("frame_rate"), DataModel->GetFrameRate().AsDecimal());
					AnimInfo->SetNumberField(TEXT("total_keys"), DataModel->GetNumberOfKeys());
				}
				if (USkeleton* Skel = AnimSeq->GetSkeleton())
				{
					AnimInfo->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
					AnimInfo->SetNumberField(TEXT("total_bones"), Skel->GetReferenceSkeleton().GetNum());
				}
				OutStructured->SetObjectField(TEXT("animation_info"), AnimInfo);

				TArray<TSharedPtr<FJsonValue>> NotifyArr;
				for (const FAnimNotifyEvent& Event : AnimSeq->Notifies)
				{
					NotifyArr.Add(MakeShared<FJsonValueObject>(AnimationNotifyEventToJson(Event)));
				}
				OutStructured->SetArrayField(TEXT("notifies"), NotifyArr);

				TArray<TSharedPtr<FJsonValue>> CurveArr;
				if (const IAnimationDataModel* DataModel = AnimSeq->GetDataModel())
				{
					for (const FFloatCurve& Curve : DataModel->GetFloatCurves())
					{
						TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
						CJ->SetStringField(TEXT("name"), Curve.GetName().ToString());
						CJ->SetStringField(TEXT("type"), TEXT("float"));
						CJ->SetNumberField(TEXT("key_count"), Curve.FloatCurve.GetNumKeys());
						CurveArr.Add(MakeShared<FJsonValueObject>(CJ));
					}
				}
				OutStructured->SetArrayField(TEXT("curves"), CurveArr);

				OutSummary = FString::Printf(TEXT("Analyzed animation: %s (%.2fs, %d notifies, %d curves)"),
					*AnimSeq->GetName(), Duration, NotifyArr.Num(), CurveArr.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- material_analyze_structure ----
		Registry.Register({
			TEXT("material_analyze_structure"),
			TEXT("Deep-analyze a material's node graph: expressions, parameters, texture samples, and complexity estimate."),
			FSololmcpSchemaBuilder::Object(
				{{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the material asset"))}},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material) { OutError = TEXT("Asset is not a material."); return false; }

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), Material->GetName());
				OutStructured->SetStringField(TEXT("shading_model"),
					StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(static_cast<int64>(Material->GetShadingModels().GetFirstShadingModel())));
				OutStructured->SetStringField(TEXT("blend_mode"),
					StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Material->BlendMode)));
				OutStructured->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());

				TArray<TSharedPtr<FJsonValue>> ExprArr, ParamArr, TexSampleArr;
				int32 TexSampleCount = 0;

				for (int32 i = 0; i < Material->GetExpressions().Num(); ++i)
				{
					UMaterialExpression* Expr = Material->GetExpressions()[i];
					if (!Expr) continue;
					TSharedRef<FJsonObject> EJ = MakeShared<FJsonObject>();
					EJ->SetNumberField(TEXT("index"), i);
					EJ->SetStringField(TEXT("type"), Expr->GetClass()->GetName());
					EJ->SetStringField(TEXT("description"), Expr->GetDescription());
					EJ->SetNumberField(TEXT("node_x"), Expr->MaterialExpressionEditorX);
					EJ->SetNumberField(TEXT("node_y"), Expr->MaterialExpressionEditorY);
					ExprArr.Add(MakeShared<FJsonValueObject>(EJ));

					if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
					{
						TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
						PJ->SetStringField(TEXT("name"), SP->ParameterName.ToString());
						PJ->SetStringField(TEXT("type"), TEXT("scalar"));
						PJ->SetNumberField(TEXT("default_value"), SP->DefaultValue);
						PJ->SetNumberField(TEXT("expression_index"), i);
						PJ->SetStringField(TEXT("group"), SP->Group.ToString());
						ParamArr.Add(MakeShared<FJsonValueObject>(PJ));
					}
					else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
					{
						TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
						PJ->SetStringField(TEXT("name"), VP->ParameterName.ToString());
						PJ->SetStringField(TEXT("type"), TEXT("vector"));
						PJ->SetObjectField(TEXT("default_value"), LinearColorToJson(VP->DefaultValue));
						PJ->SetNumberField(TEXT("expression_index"), i);
						PJ->SetStringField(TEXT("group"), VP->Group.ToString());
						ParamArr.Add(MakeShared<FJsonValueObject>(PJ));
					}
					else if (UMaterialExpressionStaticBoolParameter* BP = Cast<UMaterialExpressionStaticBoolParameter>(Expr))
					{
						TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
						PJ->SetStringField(TEXT("name"), BP->ParameterName.ToString());
						PJ->SetStringField(TEXT("type"), TEXT("static_bool"));
						PJ->SetBoolField(TEXT("default_value"), BP->DefaultValue);
						PJ->SetNumberField(TEXT("expression_index"), i);
						PJ->SetStringField(TEXT("group"), BP->Group.ToString());
						ParamArr.Add(MakeShared<FJsonValueObject>(PJ));
					}

					if (UMaterialExpressionTextureSample* TS = Cast<UMaterialExpressionTextureSample>(Expr))
					{
						TexSampleCount++;
						TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
						TJ->SetNumberField(TEXT("expression_index"), i);
						TJ->SetStringField(TEXT("sampler_type"),
							StaticEnum<EMaterialSamplerType>()->GetNameStringByValue(static_cast<int64>(TS->SamplerType)));
						if (TS->Texture) { TJ->SetStringField(TEXT("texture_path"), TS->Texture->GetPathName()); TJ->SetStringField(TEXT("texture_name"), TS->Texture->GetName()); }
						TexSampleArr.Add(MakeShared<FJsonValueObject>(TJ));
					}
				}

				OutStructured->SetArrayField(TEXT("expressions"), ExprArr);
				OutStructured->SetArrayField(TEXT("parameters"), ParamArr);
				OutStructured->SetArrayField(TEXT("texture_samples"), TexSampleArr);
				TSharedRef<FJsonObject> CplxJson = MakeShared<FJsonObject>();
				CplxJson->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
				CplxJson->SetNumberField(TEXT("texture_sample_count"), TexSampleCount);
				CplxJson->SetNumberField(TEXT("parameter_count"), ParamArr.Num());
				OutStructured->SetObjectField(TEXT("complexity"), CplxJson);

				OutSummary = FString::Printf(TEXT("Analyzed material: %s (%d expressions, %d parameters, %d texture samples)"),
					*Material->GetName(), Material->GetExpressions().Num(), ParamArr.Num(), TexSampleCount);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- asset_analyze_related ----
		Registry.Register({
			TEXT("asset_analyze_related"),
			TEXT("Analyze an asset's dependency graph: direct dependencies grouped by type, referencers, and relation graph for smart copy/move."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the asset"))},
					{TEXT("include_referencers"), FSololmcpSchemaBuilder::Boolean(TEXT("Include assets that reference this asset (default false)"))},
					{TEXT("depth"), FSololmcpSchemaBuilder::Integer(TEXT("Dependency traversal depth (1-3, default 1)"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				const bool bIncludeRef = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_referencers")) ? Arguments->GetBoolField(TEXT("include_referencers")) : false;
				int32 Depth = Arguments->HasTypedField<EJson::Number>(TEXT("depth")) ? static_cast<int32>(Arguments->GetNumberField(TEXT("depth"))) : 1;
				Depth = FMath::Clamp(Depth, 1, 3);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_name"), Asset->GetName());
				OutStructured->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

				IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				TSet<FName> Visited;
				TArray<FName> CurLevel;
				CurLevel.Add(FName(*AssetPath));
				Visited.Add(FName(*AssetPath));

				TArray<TSharedPtr<FJsonValue>> DirectDeps, AllDeps, GraphNodes, GraphEdges;
				TMap<FString, TArray<FString>> ByType;

				{ TSharedRef<FJsonObject> RN = MakeShared<FJsonObject>(); RN->SetStringField(TEXT("id"), AssetPath); RN->SetStringField(TEXT("class"), Asset->GetClass()->GetName()); RN->SetBoolField(TEXT("is_root"), true); GraphNodes.Add(MakeShared<FJsonValueObject>(RN)); }

				for (int32 d = 0; d < Depth; ++d)
				{
					TArray<FName> NextLevel;
					for (const FName& Pkg : CurLevel)
					{
						TArray<FName> Deps;
						AR.GetDependencies(Pkg, Deps);
						for (const FName& Dep : Deps)
						{
							const FString DS = Dep.ToString();
							if (DS.StartsWith(TEXT("/Script/")) || DS.StartsWith(TEXT("/Engine/"))) continue;

							{ TSharedRef<FJsonObject> E = MakeShared<FJsonObject>(); E->SetStringField(TEXT("from"), Pkg.ToString()); E->SetStringField(TEXT("to"), DS); E->SetNumberField(TEXT("depth"), d + 1); GraphEdges.Add(MakeShared<FJsonValueObject>(E)); }

							if (!Visited.Contains(Dep))
							{
								Visited.Add(Dep);
								NextLevel.Add(Dep);
								TArray<FAssetData> DA; AR.GetAssetsByPackageName(Dep, DA);
								FString DC = DA.Num() > 0 ? DA[0].AssetClassPath.GetAssetName().ToString() : TEXT("Unknown");

								TSharedRef<FJsonObject> DJ = MakeShared<FJsonObject>();
								DJ->SetStringField(TEXT("path"), DS); DJ->SetStringField(TEXT("class"), DC); DJ->SetNumberField(TEXT("depth"), d + 1);
								AllDeps.Add(MakeShared<FJsonValueObject>(DJ));
								if (d == 0) DirectDeps.Add(MakeShared<FJsonValueObject>(DJ));
								ByType.FindOrAdd(DC).Add(DS);

								{ TSharedRef<FJsonObject> N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("id"), DS); N->SetStringField(TEXT("class"), DC); N->SetBoolField(TEXT("is_root"), false); GraphNodes.Add(MakeShared<FJsonValueObject>(N)); }
							}
						}
					}
					CurLevel = NextLevel;
				}

				TSharedRef<FJsonObject> DepsJson = MakeShared<FJsonObject>();
				DepsJson->SetArrayField(TEXT("direct"), DirectDeps);
				DepsJson->SetArrayField(TEXT("all"), AllDeps);
				TSharedRef<FJsonObject> BTJson = MakeShared<FJsonObject>();
				for (const auto& P : ByType) { TArray<TSharedPtr<FJsonValue>> TP; for (const FString& V : P.Value) TP.Add(MakeShared<FJsonValueString>(V)); BTJson->SetArrayField(P.Key, TP); }
				DepsJson->SetObjectField(TEXT("by_type"), BTJson);
				OutStructured->SetObjectField(TEXT("dependencies"), DepsJson);

				if (bIncludeRef)
				{
					TArray<FName> Refs; AR.GetReferencers(FName(*AssetPath), Refs);
					TArray<TSharedPtr<FJsonValue>> RA;
					for (const FName& R : Refs) { const FString RS = R.ToString(); if (!RS.StartsWith(TEXT("/Script/")) && !RS.StartsWith(TEXT("/Engine/"))) RA.Add(MakeShared<FJsonValueString>(RS)); }
					OutStructured->SetArrayField(TEXT("referencers"), RA);
				}

				TSharedRef<FJsonObject> GJ = MakeShared<FJsonObject>();
				GJ->SetArrayField(TEXT("nodes"), GraphNodes);
				GJ->SetArrayField(TEXT("edges"), GraphEdges);
				OutStructured->SetObjectField(TEXT("relation_graph"), GJ);

				OutSummary = FString::Printf(TEXT("Analyzed relations for %s: %d direct deps, %d total deps"), *Asset->GetName(), DirectDeps.Num(), AllDeps.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- asset_batch_operation ----
		Registry.Register({
			TEXT("asset_batch_operation"),
			TEXT("Batch copy, move, or delete assets. Optionally include related (dependency) assets. Supports dry_run preview."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("Operation type"), {TEXT("copy"), TEXT("move"), TEXT("delete")})},
					{TEXT("asset_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Array of asset paths"))},
					{TEXT("target_path"), FSololmcpSchemaBuilder::String(TEXT("Destination directory (for copy/move)"))},
					{TEXT("include_related"), FSololmcpSchemaBuilder::Boolean(TEXT("Also process dependency assets (default false)"))},
					{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Preview only, do not execute (default false)"))}
				},
				{TEXT("operation"), TEXT("asset_paths")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Operation;
				if (!Arguments->TryGetStringField(TEXT("operation"), Operation)) { OutError = TEXT("Missing operation."); return false; }
				TArray<FString> AssetPaths;
				if (!TryGetStringArray(Arguments, TEXT("asset_paths"), AssetPaths) || AssetPaths.IsEmpty()) { OutError = TEXT("Missing asset_paths."); return false; }
				FString TargetPath; Arguments->TryGetStringField(TEXT("target_path"), TargetPath);
				const bool bIncludeRelated = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_related")) ? Arguments->GetBoolField(TEXT("include_related")) : false;
				const bool bDryRun = Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")) ? Arguments->GetBoolField(TEXT("dry_run")) : false;

				if ((Operation == TEXT("copy") || Operation == TEXT("move")) && TargetPath.IsEmpty()) { OutError = TEXT("target_path required for copy/move."); return false; }

				TArray<FString> AllPaths = AssetPaths;
				TArray<FString> RelatedPaths;
				if (bIncludeRelated)
				{
					IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
					TSet<FString> Seen; for (const FString& P : AssetPaths) Seen.Add(P);
					for (const FString& P : AssetPaths)
					{
						TArray<FName> Deps; AR.GetDependencies(FName(*P), Deps);
						for (const FName& D : Deps) { FString DS = D.ToString(); if (!DS.StartsWith(TEXT("/Script/")) && !DS.StartsWith(TEXT("/Engine/")) && !Seen.Contains(DS)) { Seen.Add(DS); AllPaths.Add(DS); RelatedPaths.Add(DS); } }
					}
				}

				OutStructured->SetStringField(TEXT("operation"), Operation);
				OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
				TArray<TSharedPtr<FJsonValue>> Results;
				int32 Succeeded = 0, Failed = 0;
				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "BatchOp", "SOMOLMCP Batch Asset Op"));

				for (const FString& Path : AllPaths)
				{
					TSharedRef<FJsonObject> RE = MakeShared<FJsonObject>();
					RE->SetStringField(TEXT("source"), Path);
					RE->SetBoolField(TEXT("is_related"), RelatedPaths.Contains(Path));

					if (bDryRun) { RE->SetStringField(TEXT("status"), TEXT("preview")); if (!TargetPath.IsEmpty()) RE->SetStringField(TEXT("target"), TargetPath / FPaths::GetBaseFilename(Path)); Succeeded++; }
					else if (Operation == TEXT("delete")) { FString E; if (Context.Services.DeleteAsset(Path, E) && !Context.Services.AssetExists(Path)) { RE->SetStringField(TEXT("status"), TEXT("success")); RE->SetBoolField(TEXT("verified"), true); Succeeded++; } else { RE->SetStringField(TEXT("status"), TEXT("error")); RE->SetStringField(TEXT("error"), E.IsEmpty() ? TEXT("Delete did not verify.") : E); Failed++; } }
					else if (Operation == TEXT("copy")) { FString DP = TargetPath / FPaths::GetBaseFilename(Path); RE->SetStringField(TEXT("target"), DP); FString E; UObject* Duplicated = Context.Services.DuplicateAsset(Path, DP, E); if (Duplicated && Context.Services.AssetExists(Duplicated->GetPathName())) { RE->SetStringField(TEXT("status"), TEXT("success")); RE->SetBoolField(TEXT("verified"), true); Succeeded++; } else { RE->SetStringField(TEXT("status"), TEXT("error")); RE->SetStringField(TEXT("error"), E.IsEmpty() ? TEXT("Duplicate did not verify.") : E); Failed++; } }
					else if (Operation == TEXT("move")) { FString DP = TargetPath / FPaths::GetBaseFilename(Path); RE->SetStringField(TEXT("target"), DP); FString E; if (Context.Services.RenameAsset(Path, DP, E) && Context.Services.AssetExists(DP) && !Context.Services.AssetExists(Path)) { RE->SetStringField(TEXT("status"), TEXT("success")); RE->SetBoolField(TEXT("verified"), true); Succeeded++; } else { RE->SetStringField(TEXT("status"), TEXT("error")); RE->SetStringField(TEXT("error"), E.IsEmpty() ? TEXT("Move did not verify.") : E); Failed++; } }
					else { RE->SetStringField(TEXT("status"), TEXT("error")); RE->SetStringField(TEXT("error"), TEXT("Unknown operation.")); Failed++; }
					Results.Add(MakeShared<FJsonValueObject>(RE));
				}

				OutStructured->SetArrayField(TEXT("results"), Results);
				TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
				SJ->SetNumberField(TEXT("total"), AllPaths.Num()); SJ->SetNumberField(TEXT("succeeded"), Succeeded); SJ->SetNumberField(TEXT("failed"), Failed); SJ->SetNumberField(TEXT("related_included"), RelatedPaths.Num());
				OutStructured->SetObjectField(TEXT("summary"), SJ);
				OutStructured->SetStringField(TEXT("code"), Failed > 0 ? TEXT("partial_success") : TEXT("ok"));

				OutSummary = FString::Printf(TEXT("Batch %s: %d/%d succeeded%s"), *Operation, Succeeded, AllPaths.Num(), bDryRun ? TEXT(" (dry run)") : TEXT(""));
				if (!bDryRun && Failed > 0 && Succeeded == 0)
				{
					OutError = FString::Printf(TEXT("All asset batch %s operations failed."), *Operation);
					return false;
				}
				return true;
			}
		, nullptr
		, 5
		});

		// ---- asset_dependency_graph ----
		Registry.Register({
			TEXT("asset_dependency_graph"),
			TEXT("Generate an asset dependency graph in Mermaid or DOT format for visualization."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("root_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Root asset"))},
					{TEXT("depth"), FSololmcpSchemaBuilder::Integer(TEXT("Traversal depth (1-4, default 2)"))},
					{TEXT("format"), FSololmcpSchemaBuilder::String(TEXT("Output format"), {TEXT("mermaid"), TEXT("dot")})}
				},
				{TEXT("root_asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString RootPath;
				if (!Arguments->TryGetStringField(TEXT("root_asset_path"), RootPath)) { OutError = TEXT("Missing root_asset_path."); return false; }
				int32 Depth = Arguments->HasTypedField<EJson::Number>(TEXT("depth")) ? static_cast<int32>(Arguments->GetNumberField(TEXT("depth"))) : 2;
				Depth = FMath::Clamp(Depth, 1, 4);
				FString Format; if (!Arguments->TryGetStringField(TEXT("format"), Format)) Format = TEXT("mermaid");

				IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				AR.WaitForCompletion();
				FString RootObjectPath = RootPath;
				const int32 RootLastSlash = RootObjectPath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				const int32 RootLastDot = RootObjectPath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				if (RootLastDot <= RootLastSlash)
				{
					FString AssetName = RootObjectPath;
					int32 SlashIndex = INDEX_NONE;
					if (AssetName.FindLastChar(TEXT('/'), SlashIndex))
					{
						AssetName = AssetName.Mid(SlashIndex + 1);
					}
					RootObjectPath = FString::Printf(TEXT("%s.%s"), *RootObjectPath, *AssetName);
				}
				TArray<FAssetData> RootPackageAssets;
				const bool bRootFound =
					AR.GetAssetByObjectPath(FSoftObjectPath(RootObjectPath)).IsValid() ||
					(AR.GetAssetsByPackageName(FName(*RootPath), RootPackageAssets), RootPackageAssets.Num() > 0);
				OutStructured->SetStringField(TEXT("root_asset_path"), RootPath);
				OutStructured->SetBoolField(TEXT("root_found"), bRootFound);
				if (!bRootFound)
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("root_not_found"));
					OutError = FString::Printf(TEXT("Root asset not found: %s"), *RootPath);
					return false;
				}
				TSet<FName> Visited; TArray<FName> CurLevel;
				CurLevel.Add(FName(*RootPath)); Visited.Add(FName(*RootPath));

				struct FEdge { FString From, To; };
				TArray<FEdge> Edges;
				TSet<FString> NodeSet; NodeSet.Add(RootPath);

				for (int32 d = 0; d < Depth; ++d)
				{
					TArray<FName> Next;
					for (const FName& Pkg : CurLevel)
					{
						TArray<FName> Deps; AR.GetDependencies(Pkg, Deps);
						for (const FName& D : Deps) { FString DS = D.ToString(); if (DS.StartsWith(TEXT("/Script/")) || DS.StartsWith(TEXT("/Engine/"))) continue; NodeSet.Add(DS); Edges.Add({Pkg.ToString(), DS}); if (!Visited.Contains(D)) { Visited.Add(D); Next.Add(D); } }
					}
					CurLevel = Next;
				}

				FString GraphOut;
				if (Format == TEXT("mermaid"))
				{
					GraphOut = TEXT("graph TD\n");
					TMap<FString, FString> Ids; int32 Idx = 0;
					for (const FString& N : NodeSet) { FString Id = FString::Printf(TEXT("N%d"), Idx++); Ids.Add(N, Id); GraphOut += FString::Printf(TEXT("    %s[\"%s\"]\n"), *Id, *FPaths::GetBaseFilename(N)); }
					for (const FEdge& E : Edges) { FString* FI = Ids.Find(E.From); FString* TI = Ids.Find(E.To); if (FI && TI) GraphOut += FString::Printf(TEXT("    %s --> %s\n"), **FI, **TI); }
				}
				else
				{
					GraphOut = TEXT("digraph Dependencies {\n    rankdir=LR;\n");
					for (const FString& N : NodeSet) GraphOut += FString::Printf(TEXT("    \"%s\" [label=\"%s\"];\n"), *N, *FPaths::GetBaseFilename(N));
					for (const FEdge& E : Edges) GraphOut += FString::Printf(TEXT("    \"%s\" -> \"%s\";\n"), *E.From, *E.To);
					GraphOut += TEXT("}\n");
				}

				OutStructured->SetStringField(TEXT("format"), Format);
				OutStructured->SetStringField(TEXT("graph"), GraphOut);
				OutStructured->SetStringField(TEXT("status"), Edges.Num() > 0 ? TEXT("ok") : TEXT("no_dependencies"));
				TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
				Stats->SetNumberField(TEXT("node_count"), NodeSet.Num()); Stats->SetNumberField(TEXT("edge_count"), Edges.Num()); Stats->SetNumberField(TEXT("depth"), Depth);
				OutStructured->SetObjectField(TEXT("statistics"), Stats);
				OutSummary = FString::Printf(TEXT("Generated %s graph: %d nodes, %d edges"), *Format, NodeSet.Num(), Edges.Num());
				return true;
			},
			nullptr,  // IsAvailable
			30        // CacheTtlSeconds
		});

		// ---- actor_get_detailed_bounds ----
		Registry.Register({
			TEXT("actor_get_detailed_bounds"),
			TEXT("Get detailed spatial info for a scene actor: transform, world/local bounds, collision primitives (boxes, spheres, capsules), and sockets."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label or name"))},
					{TEXT("include_collision"), FSololmcpSchemaBuilder::Boolean(TEXT("Include collision details (default true)"))},
					{TEXT("include_sockets"), FSololmcpSchemaBuilder::Boolean(TEXT("Include socket info (default true)"))}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId)) { OutError = TEXT("Missing actor."); return false; }
				const bool bCollision = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_collision")) ? Arguments->GetBoolField(TEXT("include_collision")) : true;
				const bool bSockets = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_sockets")) ? Arguments->GetBoolField(TEXT("include_sockets")) : true;

				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor) return false;

				OutStructured->SetObjectField(TEXT("actor"), FSololmcpEditorServices::MakeActorReference(Actor));
				OutStructured->SetObjectField(TEXT("transform"), TransformToJson(Actor->GetActorTransform()));

				FVector Origin, BoxExtent;
				Actor->GetActorBounds(false, Origin, BoxExtent);
				TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
				BJ->SetObjectField(TEXT("center"), VectorToJson(Origin));
				BJ->SetObjectField(TEXT("extent"), VectorToJson(BoxExtent));
				BJ->SetNumberField(TEXT("sphere_radius"), BoxExtent.Size());
				TSharedRef<FJsonObject> WB = MakeShared<FJsonObject>();
				WB->SetObjectField(TEXT("min"), VectorToJson(Origin - BoxExtent));
				WB->SetObjectField(TEXT("max"), VectorToJson(Origin + BoxExtent));
				BJ->SetObjectField(TEXT("world_aabb"), WB);
				OutStructured->SetObjectField(TEXT("bounds"), BJ);

				if (bCollision)
				{
					TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
					TArray<TSharedPtr<FJsonValue>> Prims;
					bool bHasColl = false;
					TArray<UActorComponent*> Comps; Actor->GetComponents(Comps);
					for (UActorComponent* C : Comps)
					{
						UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(C);
						if (!PC) continue;
						const UBodySetup* BS = PC->GetBodySetup();
						if (!BS) continue;
						bHasColl = true;
						const FString CN = PC->GetName();
						for (const FKBoxElem& B : BS->AggGeom.BoxElems) { TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("box")); P->SetStringField(TEXT("component"), CN); P->SetNumberField(TEXT("x"), B.X); P->SetNumberField(TEXT("y"), B.Y); P->SetNumberField(TEXT("z"), B.Z); P->SetObjectField(TEXT("center"), VectorToJson(B.Center)); Prims.Add(MakeShared<FJsonValueObject>(P)); }
						for (const FKSphereElem& S : BS->AggGeom.SphereElems) { TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("sphere")); P->SetStringField(TEXT("component"), CN); P->SetNumberField(TEXT("radius"), S.Radius); P->SetObjectField(TEXT("center"), VectorToJson(S.Center)); Prims.Add(MakeShared<FJsonValueObject>(P)); }
						for (const FKSphylElem& Cap : BS->AggGeom.SphylElems) { TSharedRef<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("type"), TEXT("capsule")); P->SetStringField(TEXT("component"), CN); P->SetNumberField(TEXT("radius"), Cap.Radius); P->SetNumberField(TEXT("length"), Cap.Length); P->SetObjectField(TEXT("center"), VectorToJson(Cap.Center)); Prims.Add(MakeShared<FJsonValueObject>(P)); }
					}
					CJ->SetBoolField(TEXT("has_collision"), bHasColl);
					CJ->SetArrayField(TEXT("primitives"), Prims);
					OutStructured->SetObjectField(TEXT("collision"), CJ);
				}

				if (bSockets)
				{
					TArray<TSharedPtr<FJsonValue>> SockArr;
					TArray<UActorComponent*> AllComps; Actor->GetComponents(AllComps);
					for (UActorComponent* C : AllComps)
					{
						USceneComponent* SC = Cast<USceneComponent>(C);
						if (!SC) continue;
						TArray<FName> SNames = SC->GetAllSocketNames();
						for (const FName& SN : SNames)
						{
							TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
							SJ->SetStringField(TEXT("name"), SN.ToString());
							SJ->SetStringField(TEXT("component"), SC->GetName());
							SJ->SetObjectField(TEXT("world_transform"), TransformToJson(SC->GetSocketTransform(SN, RTS_World)));
							SJ->SetObjectField(TEXT("local_transform"), TransformToJson(SC->GetSocketTransform(SN, RTS_Component)));
							SockArr.Add(MakeShared<FJsonValueObject>(SJ));
						}
					}
					OutStructured->SetArrayField(TEXT("sockets"), SockArr);
				}

				OutSummary = FString::Printf(TEXT("Detailed bounds for actor: %s"), *Actor->GetActorLabel());
				return true;
			}
		, nullptr
		, 5
		});

		// ============================================================================
		// v1.8.0 Substrate Material Enhancement (5 tools)
		// ============================================================================

		// ---- substrate_set_blend_mode ----
		Registry.Register({
			TEXT("substrate_set_blend_mode"),
			TEXT("Set blend mode on a Substrate/standard material (Opaque, Translucent, Masked, Additive, AlphaComposite, AlphaHoldout)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path"))},
					{TEXT("blend_mode"), FSololmcpSchemaBuilder::String(TEXT("Blend mode"), {TEXT("Opaque"), TEXT("Translucent"), TEXT("Masked"), TEXT("Additive"), TEXT("AlphaComposite"), TEXT("AlphaHoldout")})}
				},
				{TEXT("asset_path"), TEXT("blend_mode")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, BlendModeStr;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
				{ OutError = TEXT("Missing asset_path or blend_mode."); return false; }
				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material) return false;

				EBlendMode NewMode = BLEND_Opaque;
				if (BlendModeStr == TEXT("Translucent")) NewMode = BLEND_Translucent;
				else if (BlendModeStr == TEXT("Masked")) NewMode = BLEND_Masked;
				else if (BlendModeStr == TEXT("Additive")) NewMode = BLEND_Additive;
				else if (BlendModeStr == TEXT("AlphaComposite")) NewMode = BLEND_AlphaComposite;
				else if (BlendModeStr == TEXT("AlphaHoldout")) NewMode = BLEND_AlphaHoldout;

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "SubstrateSetBlendMode", "SOMOLMCP Set Blend Mode"));
				Material->PreEditChange(nullptr);
				Material->BlendMode = NewMode;
				Material->PostEditChange();
				Context.Services.SaveAsset(AssetPath, false, OutError);

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("blend_mode"), BlendModeStr);
				OutSummary = FString::Printf(TEXT("Set blend mode to %s on %s"), *BlendModeStr, *Material->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- substrate_add_expression ----
		Registry.Register({
			TEXT("substrate_add_expression"),
			TEXT("Add a material expression node by class name. Supports Substrate nodes (SubstrateSlab, SubstrateHorizontalMixing, SubstrateVerticalLayering, etc.) and standard expression types."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path"))},
					{TEXT("expression_class"), FSololmcpSchemaBuilder::String(TEXT("Short class name, e.g. SubstrateSlab, TextureSample, ScalarParameter"))},
					{TEXT("node_x"), FSololmcpSchemaBuilder::Integer(TEXT("Editor X position (default 0)"))},
					{TEXT("node_y"), FSololmcpSchemaBuilder::Integer(TEXT("Editor Y position (default 0)"))},
					{TEXT("description"), FSololmcpSchemaBuilder::String(TEXT("Optional description/label for the node"))}
				},
				{TEXT("asset_path"), TEXT("expression_class")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, ExprClassName, Description;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("expression_class"), ExprClassName))
				{ OutError = TEXT("Missing asset_path or expression_class."); return false; }
				Arguments->TryGetStringField(TEXT("description"), Description);
				int32 NodeX = 0, NodeY = 0;
				Arguments->TryGetNumberField(TEXT("node_x"), NodeX);
				Arguments->TryGetNumberField(TEXT("node_y"), NodeY);

				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material) return false;

				// Resolve full class path
				FString FullPath = FString::Printf(TEXT("/Script/Engine.MaterialExpression%s"), *ExprClassName);
				UClass* ExprClass = FindObject<UClass>(nullptr, *FullPath);
				if (!ExprClass)
				{
					FullPath = FString::Printf(TEXT("/Script/Engine.%s"), *ExprClassName);
					ExprClass = FindObject<UClass>(nullptr, *FullPath);
				}
				if (!ExprClass)
				{
					OutError = FString::Printf(TEXT("Expression class not found: %s"), *ExprClassName);
					return false;
				}

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "SubstrateAddExpr", "SOMOLMCP Add Expression"));
				UMaterialExpression* NewExpr = NewObject<UMaterialExpression>(Material, ExprClass);
				if (!NewExpr) { OutError = TEXT("Failed to create expression."); return false; }
				NewExpr->MaterialExpressionEditorX = NodeX;
				NewExpr->MaterialExpressionEditorY = NodeY;
				if (!Description.IsEmpty()) { NewExpr->Desc = Description; }
				Material->GetExpressionCollection().AddExpression(NewExpr);
				Material->PreEditChange(nullptr);
				Material->PostEditChange();
				Context.Services.SaveAsset(AssetPath, false, OutError);

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("expression_class"), ExprClassName);
				OutStructured->SetNumberField(TEXT("expression_index"), Material->GetExpressions().Num() - 1);
				OutStructured->SetStringField(TEXT("expression_name"), NewExpr->GetName());
				OutSummary = FString::Printf(TEXT("Added %s expression to %s"), *ExprClassName, *Material->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- substrate_connect_nodes ----
		Registry.Register({
			TEXT("substrate_connect_nodes"),
			TEXT("Connect two material expression nodes. Links an output of one expression to an input of another (or to a material property)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path"))},
					{TEXT("source_index"), FSololmcpSchemaBuilder::Integer(TEXT("Source expression index in the expressions array"))},
					{TEXT("source_output"), FSololmcpSchemaBuilder::Integer(TEXT("Source output index (default 0)"))},
					{TEXT("target_index"), FSololmcpSchemaBuilder::Integer(TEXT("Target expression index (-1 for material output)"))},
					{TEXT("target_input"), FSololmcpSchemaBuilder::Integer(TEXT("Target input index (default 0)"))},
					{TEXT("material_property"), FSololmcpSchemaBuilder::String(TEXT("If target_index=-1, material property name (BaseColor, Metallic, Roughness, Normal, EmissiveColor, Opacity, SubsurfaceColor, FrontMaterial)"))}
				},
				{TEXT("asset_path"), TEXT("source_index")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				int32 SrcIdx = 0, SrcOut = 0, DstIdx = -1, DstIn = 0;
				Arguments->TryGetNumberField(TEXT("source_index"), SrcIdx);
				Arguments->TryGetNumberField(TEXT("source_output"), SrcOut);
				Arguments->TryGetNumberField(TEXT("target_index"), DstIdx);
				Arguments->TryGetNumberField(TEXT("target_input"), DstIn);

				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material) return false;
				const auto& Exprs = Material->GetExpressions();
				if (SrcIdx < 0 || SrcIdx >= Exprs.Num()) { OutError = TEXT("source_index out of range."); return false; }

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "SubstrateConnect", "SOMOLMCP Connect Nodes"));
				Material->PreEditChange(nullptr);

				UMaterialExpression* SrcExpr = Exprs[SrcIdx];
				if (DstIdx >= 0)
				{
					if (DstIdx >= Exprs.Num()) { OutError = TEXT("target_index out of range."); return false; }
					UMaterialExpression* DstExpr = Exprs[DstIdx];
					FExpressionInput* Input = DstExpr->GetInput(DstIn);
					if (!Input) { OutError = TEXT("target_input index invalid."); return false; }
					Input->Connect(SrcOut, SrcExpr);
				}
				else
				{
					// Connect to material property
					FString PropName;
					Arguments->TryGetStringField(TEXT("material_property"), PropName);
					if (PropName.IsEmpty()) PropName = TEXT("FrontMaterial");
					UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
					FExpressionInput* MatInput = nullptr;
					if (PropName == TEXT("BaseColor")) MatInput = &EditorData->BaseColor;
					else if (PropName == TEXT("Metallic")) MatInput = &EditorData->Metallic;
					else if (PropName == TEXT("Roughness")) MatInput = &EditorData->Roughness;
					else if (PropName == TEXT("Normal")) MatInput = &EditorData->Normal;
					else if (PropName == TEXT("EmissiveColor")) MatInput = &EditorData->EmissiveColor;
					else if (PropName == TEXT("Opacity")) MatInput = &EditorData->Opacity;
					else if (PropName == TEXT("SubsurfaceColor")) MatInput = &EditorData->SubsurfaceColor;
					else if (PropName == TEXT("FrontMaterial")) MatInput = &EditorData->FrontMaterial;
					if (!MatInput) { OutError = FString::Printf(TEXT("Unknown material_property: %s"), *PropName); return false; }
					MatInput->Connect(SrcOut, SrcExpr);
				}

				Material->PostEditChange();
				Context.Services.SaveAsset(AssetPath, false, OutError);

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetNumberField(TEXT("source_index"), SrcIdx);
				OutStructured->SetNumberField(TEXT("target_index"), DstIdx);
				OutSummary = TEXT("Connected material expression nodes.");
				return true;
			}
		, nullptr
		, 5
		});

		// ---- substrate_get_layer_info ----
		Registry.Register({
			TEXT("substrate_get_layer_info"),
			TEXT("Get detailed information about all Substrate-related expression nodes in a material, including node types, connections, and parameters."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material) return false;

				TArray<TSharedPtr<FJsonValue>> SubstrateNodes, AllNodes;
				const auto& Exprs = Material->GetExpressions();
				for (int32 i = 0; i < Exprs.Num(); ++i)
				{
					UMaterialExpression* Expr = Exprs[i];
					if (!Expr) continue;
					FString ClassName = Expr->GetClass()->GetName();
					TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
					NJ->SetNumberField(TEXT("index"), i);
					NJ->SetStringField(TEXT("class"), ClassName);
					NJ->SetStringField(TEXT("desc"), Expr->Desc);
					NJ->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
					NJ->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);
					// UE 5.7: GetInputsView() deprecated - count inputs by iterating until GetInput returns null
					int32 InputCount = 0;
					while (Expr->GetInput(InputCount) != nullptr) { ++InputCount; }
					NJ->SetNumberField(TEXT("input_count"), InputCount);
					NJ->SetNumberField(TEXT("output_count"), Expr->GetOutputs().Num());

					// Trace input connections
					TArray<TSharedPtr<FJsonValue>> Inputs;
					for (int32 j = 0; j < InputCount; ++j)
					{
						FExpressionInput* Inp = Expr->GetInput(j);
						if (Inp && Inp->Expression)
						{
							TSharedRef<FJsonObject> IJ = MakeShared<FJsonObject>();
							IJ->SetNumberField(TEXT("input_index"), j);
							IJ->SetStringField(TEXT("connected_to"), Inp->Expression->GetName());
							IJ->SetNumberField(TEXT("output_index"), Inp->OutputIndex);
							Inputs.Add(MakeShared<FJsonValueObject>(IJ));
						}
					}
					NJ->SetArrayField(TEXT("inputs"), Inputs);
					AllNodes.Add(MakeShared<FJsonValueObject>(NJ));
					if (ClassName.Contains(TEXT("Substrate"))) SubstrateNodes.Add(MakeShared<FJsonValueObject>(NJ));
				}

				// Check material output connection
				UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
				TSharedRef<FJsonObject> MatOutput = MakeShared<FJsonObject>();
				MatOutput->SetBoolField(TEXT("has_front_material"), EditorData->FrontMaterial.Expression != nullptr);
				if (EditorData->FrontMaterial.Expression) MatOutput->SetStringField(TEXT("front_material_expr"), EditorData->FrontMaterial.Expression->GetName());
				MatOutput->SetBoolField(TEXT("has_base_color"), EditorData->BaseColor.Expression != nullptr);
				MatOutput->SetBoolField(TEXT("has_normal"), EditorData->Normal.Expression != nullptr);
				OutStructured->SetObjectField(TEXT("material_outputs"), MatOutput);

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetArrayField(TEXT("substrate_nodes"), SubstrateNodes);
				OutStructured->SetArrayField(TEXT("all_nodes"), AllNodes);
				OutStructured->SetNumberField(TEXT("total_expressions"), Exprs.Num());
				OutStructured->SetNumberField(TEXT("substrate_expression_count"), SubstrateNodes.Num());
				OutStructured->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(Material->BlendMode)));
				OutStructured->SetStringField(TEXT("shading_model"), StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(static_cast<int64>(Material->GetShadingModels().GetFirstShadingModel())));
				OutSummary = FString::Printf(TEXT("Material %s: %d expressions (%d Substrate)"), *Material->GetName(), Exprs.Num(), SubstrateNodes.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- substrate_convert_legacy ----
		Registry.Register({
			TEXT("substrate_convert_legacy"),
			TEXT("Convert a traditional material to Substrate by inserting a SubstrateSlab node and remapping existing BaseColor/Metallic/Roughness/Normal inputs. The original inputs are preserved."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!Material) return false;
				UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
				if (EditorData->FrontMaterial.Expression) { OutError = TEXT("Material already has a FrontMaterial (Substrate) connection."); return false; }

				// Look for SubstrateSlab class
				UClass* SlabClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionSubstrateSlab"));
				if (!SlabClass) SlabClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.MaterialExpressionSubstrateSlabBSDF"));
				if (!SlabClass) { OutError = TEXT("SubstrateSlab expression class not found. Substrate may not be available in this UE version."); return false; }

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "SubstrateConvert", "SOMOLMCP Convert to Substrate"));
				Material->PreEditChange(nullptr);

				// Save existing connections
				UMaterialExpression* OldBaseColor = EditorData->BaseColor.Expression;
				int32 OldBaseColorIdx = EditorData->BaseColor.OutputIndex;
				UMaterialExpression* OldMetallic = EditorData->Metallic.Expression;
				int32 OldMetallicIdx = EditorData->Metallic.OutputIndex;
				UMaterialExpression* OldRoughness = EditorData->Roughness.Expression;
				int32 OldRoughnessIdx = EditorData->Roughness.OutputIndex;
				UMaterialExpression* OldNormal = EditorData->Normal.Expression;
				int32 OldNormalIdx = EditorData->Normal.OutputIndex;

				// Create SubstrateSlab
				UMaterialExpression* Slab = NewObject<UMaterialExpression>(Material, SlabClass);
				if (!Slab) { OutError = TEXT("Failed to create SubstrateSlab."); return false; }
				Slab->MaterialExpressionEditorX = 0;
				Slab->MaterialExpressionEditorY = 0;
				Slab->Desc = TEXT("Auto-converted Substrate Slab");
				Material->GetExpressionCollection().AddExpression(Slab);

				// Connect SubstrateSlab to FrontMaterial
				EditorData->FrontMaterial.Connect(0, Slab);

				// Remap: connect old inputs to SubstrateSlab inputs
				// SubstrateSlab inputs typically: 0=BaseColor/DiffuseAlbedo, 1=F0, 2=Roughness, 3=Normal
				if (OldBaseColor) { FExpressionInput* I = Slab->GetInput(0); if (I) I->Connect(OldBaseColorIdx, OldBaseColor); }
				if (OldRoughness) { FExpressionInput* I = Slab->GetInput(2); if (I) I->Connect(OldRoughnessIdx, OldRoughness); }
				if (OldNormal) { FExpressionInput* I = Slab->GetInput(3); if (I) I->Connect(OldNormalIdx, OldNormal); }

				// Disconnect old material property connections
				EditorData->BaseColor.Expression = nullptr;
				EditorData->Metallic.Expression = nullptr;
				EditorData->Roughness.Expression = nullptr;
				EditorData->Normal.Expression = nullptr;

				Material->PostEditChange();
				Context.Services.SaveAsset(AssetPath, false, OutError);

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("converted"), true);
				OutStructured->SetStringField(TEXT("slab_expression"), Slab->GetName());
				OutSummary = FString::Printf(TEXT("Converted %s to Substrate with SubstrateSlab"), *Material->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ============================================================================
		// v1.8.0 PCG Enhancement (5 tools)
		// ============================================================================

		// ---- pcg_volume_create ----
		Registry.Register({
			TEXT("pcg_volume_create"),
			TEXT("Create a PCG volume actor in the current scene at the specified location and size."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("location"), VectorSchema()},
					{TEXT("extent"), VectorSchema()},
					{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Optional label for the volume actor"))}
				},
				{}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				// Audit round 5: reject empty args so we don't silently spawn a default PCGVolume_0 actor.
				FString GuardLabel;
				TSharedPtr<FJsonObject> GuardLoc;
				const bool bHasLabel = Arguments->TryGetStringField(TEXT("actor_label"), GuardLabel) && !GuardLabel.IsEmpty();
				const bool bHasLocation = TryGetObjectField(Arguments, TEXT("location"), GuardLoc);
				if (!bHasLabel || !bHasLocation)
				{
					OutError = TEXT("Missing required: actor_label and location");
					return false;
				}

				FVector Location = FVector::ZeroVector, Extent = FVector(500, 500, 500);
				TSharedPtr<FJsonObject> LocObj, ExtObj;
				if (TryGetObjectField(Arguments, TEXT("location"), LocObj)) FSololmcpEditorServices::JsonToVector(LocObj, Location);
				if (TryGetObjectField(Arguments, TEXT("extent"), ExtObj)) FSololmcpEditorServices::JsonToVector(ExtObj, Extent);

				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World) { OutError = TEXT("No editor world."); return false; }

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "PCGVolumeCreate", "SOMOLMCP Create PCG Volume"));
				UClass* VolumeClass = FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGVolume"));
				if (!VolumeClass) VolumeClass = LoadClass<AActor>(nullptr, TEXT("/Script/PCG.PCGVolume"));
				if (!VolumeClass) { OutError = TEXT("PCGVolume class not found."); return false; }

				AActor* Volume = World->SpawnActor(VolumeClass, &Location);
				if (!Volume) { OutError = TEXT("Failed to spawn PCG volume."); return false; }
				Volume->SetActorScale3D(Extent / 100.0);

				FString Label;
				if (Arguments->TryGetStringField(TEXT("actor_label"), Label) && !Label.IsEmpty()) Volume->SetActorLabel(Label);

				OutStructured = FSololmcpEditorServices::MakeActorReference(Volume);
				OutSummary = FString::Printf(TEXT("Created PCG volume: %s"), *Volume->GetActorLabel());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- pcg_graph_export ----
		Registry.Register({
			TEXT("pcg_graph_export"),
			TEXT("Export a PCG graph structure as JSON, including all nodes, their types, and edge connections."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset path"))}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;

				// Use reflection to access PCG graph nodes
				UClass* PCGGraphClass = Asset->GetClass();
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("asset_class"), PCGGraphClass->GetName());

				TArray<TSharedPtr<FJsonValue>> NodeList;
				// Access nodes via UObject property reflection
				FArrayProperty* NodesProperty = CastField<FArrayProperty>(PCGGraphClass->FindPropertyByName(TEXT("Nodes")));
				if (NodesProperty)
				{
					FScriptArrayHelper ArrayHelper(NodesProperty, NodesProperty->ContainerPtrToValuePtr<void>(Asset));
					FObjectProperty* ElemProp = CastField<FObjectProperty>(NodesProperty->Inner);
					for (int32 i = 0; i < ArrayHelper.Num() && ElemProp; ++i)
					{
						UObject* NodeObj = ElemProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
						if (!NodeObj) continue;
						TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
						NJ->SetNumberField(TEXT("index"), i);
						NJ->SetStringField(TEXT("name"), NodeObj->GetName());
						NJ->SetStringField(TEXT("class"), NodeObj->GetClass()->GetName());
						// Try to get node title
						UFunction* TitleFunc = NodeObj->GetClass()->FindFunctionByName(TEXT("GetNodeTitle"));
						if (TitleFunc)
						{
							FString Title;
							NodeObj->ProcessEvent(TitleFunc, &Title);
							NJ->SetStringField(TEXT("title"), Title);
						}
						NodeList.Add(MakeShared<FJsonValueObject>(NJ));
					}
				}
				OutStructured->SetArrayField(TEXT("nodes"), NodeList);
				OutStructured->SetNumberField(TEXT("node_count"), NodeList.Num());
				OutSummary = FString::Printf(TEXT("Exported PCG graph: %d nodes"), NodeList.Num());
				return true;
			},
			nullptr,  // IsAvailable
			15        // CacheTtlSeconds
		});

		// ---- pcg_get_generation_results ----
		Registry.Register({
			TEXT("pcg_get_generation_results"),
			TEXT("Get information about the PCG generation state of an actor's PCG component, including generated resource count and state."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor with PCG component"))}}, {TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId)) { OutError = TEXT("Missing actor."); return false; }
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor) return false;

				TArray<UActorComponent*> Comps;
				Actor->GetComponents(Comps);
				TArray<TSharedPtr<FJsonValue>> PCGInfos;
				for (UActorComponent* C : Comps)
				{
					if (!C->GetClass()->GetName().Contains(TEXT("PCGComponent"))) continue;
					TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
					PJ->SetStringField(TEXT("component_name"), C->GetName());
					// Read properties via reflection
					FProperty* SeedProp = C->GetClass()->FindPropertyByName(TEXT("Seed"));
					if (SeedProp)
					{
						int32 Seed = 0;
						SeedProp->GetValue_InContainer(C, &Seed);
						PJ->SetNumberField(TEXT("seed"), Seed);
					}
					FProperty* GenProp = C->GetClass()->FindPropertyByName(TEXT("bGenerated"));
					if (GenProp)
					{
						bool bGen = false;
						GenProp->GetValue_InContainer(C, &bGen);
						PJ->SetBoolField(TEXT("is_generated"), bGen);
					}
					// Count generated ISM components on the actor
					int32 IsmCount = 0;
					for (UActorComponent* OC : Comps)
					{
						if (OC->GetClass()->GetName().Contains(TEXT("InstancedStaticMeshComponent")) || OC->GetClass()->GetName().Contains(TEXT("HierarchicalInstancedStaticMeshComponent")))
							IsmCount++;
					}
					PJ->SetNumberField(TEXT("instanced_mesh_components"), IsmCount);
					PCGInfos.Add(MakeShared<FJsonValueObject>(PJ));
				}
				OutStructured->SetArrayField(TEXT("pcg_components"), PCGInfos);
				OutStructured->SetStringField(TEXT("actor"), ActorId);
				OutSummary = FString::Printf(TEXT("PCG generation info for %s: %d PCG components"), *ActorId, PCGInfos.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- pcg_graph_set_input_type ----
		Registry.Register({
			TEXT("pcg_graph_set_input_type"),
			TEXT("Set the default input type for a PCG graph's input node settings."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PCG graph asset"))},
					{TEXT("input_type"), FSololmcpSchemaBuilder::String(TEXT("Input type"), {TEXT("Landscape"), TEXT("Volume"), TEXT("Point"), TEXT("EWP"), TEXT("Other")})}
				},
				{TEXT("asset_path"), TEXT("input_type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, InputType;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("input_type"), InputType))
				{ OutError = TEXT("Missing asset_path or input_type."); return false; }
				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;

				// Set via reflection: find input settings property
				FProperty* InputProp = Asset->GetClass()->FindPropertyByName(TEXT("InputType"));
				if (!InputProp) InputProp = Asset->GetClass()->FindPropertyByName(TEXT("DefaultInputType"));
				if (InputProp)
				{
					if (FEnumProperty* EnumProp = CastField<FEnumProperty>(InputProp))
					{
						int64 EnumVal = EnumProp->GetEnum()->GetValueByNameString(InputType);
						if (EnumVal != INDEX_NONE) EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(InputProp->ContainerPtrToValuePtr<void>(Asset), EnumVal);
					}
				}
				Context.Services.SaveAsset(AssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("input_type"), InputType);
				OutSummary = FString::Printf(TEXT("Set PCG graph input type to %s"), *InputType);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- pcg_set_random_seed ----
		Registry.Register({
			TEXT("pcg_set_random_seed"),
			TEXT("Set the random seed on a PCG component for deterministic generation."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label/name"))},
					{TEXT("seed"), FSololmcpSchemaBuilder::Integer(TEXT("Random seed value"))},
					{TEXT("regenerate"), FSololmcpSchemaBuilder::Boolean(TEXT("Trigger regeneration after setting seed (default false)"))}
				},
				{TEXT("actor"), TEXT("seed")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				int32 Seed = 0;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId) || !Arguments->TryGetNumberField(TEXT("seed"), Seed))
				{ OutError = TEXT("Missing actor or seed."); return false; }
				bool bRegenerate = false;
				Arguments->TryGetBoolField(TEXT("regenerate"), bRegenerate);

				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor) return false;

				bool bFound = false;
				TArray<UActorComponent*> Comps;
				Actor->GetComponents(Comps);
				for (UActorComponent* C : Comps)
				{
					if (!C->GetClass()->GetName().Contains(TEXT("PCGComponent"))) continue;
					FProperty* SeedProp = C->GetClass()->FindPropertyByName(TEXT("Seed"));
					if (SeedProp)
					{
						const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "PCGSetSeed", "SOMOLMCP Set PCG Seed"));
						C->Modify();
						SeedProp->SetValue_InContainer(C, &Seed);
						bFound = true;
						if (bRegenerate)
						{
							UFunction* GenFunc = C->GetClass()->FindFunctionByName(TEXT("Generate"));
							if (GenFunc) C->ProcessEvent(GenFunc, nullptr);
						}
					}
					break;
				}
				if (!bFound) { OutError = TEXT("No PCGComponent with Seed property found."); return false; }
				OutStructured->SetStringField(TEXT("actor"), ActorId);
				OutStructured->SetNumberField(TEXT("seed"), Seed);
				OutSummary = FString::Printf(TEXT("Set PCG seed to %d on %s"), Seed, *ActorId);
				return true;
			}
		, nullptr
		, 5
		});

		// ============================================================================
		// v1.8.0 AnimBP State Machine Enhancement (5 tools)
		// ============================================================================

		// ---- anim_bp_get_state_machine_info ----
		Registry.Register({
			TEXT("anim_bp_get_state_machine_info"),
			TEXT("Get complete state machine structure: all states, transitions, entry state, and their properties."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("AnimBlueprint asset path"))},
					{TEXT("state_machine_name"), FSololmcpSchemaBuilder::String(TEXT("State machine name (optional, returns first if omitted)"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, SMName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				Arguments->TryGetStringField(TEXT("state_machine_name"), SMName);
				UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!AnimBP) { OutError = TEXT("Not an AnimBlueprint."); return false; }

				// Walk all anim graphs to find state machine nodes
				TArray<UEdGraph*> AnimGraphs;
				AnimBP->GetAllGraphs(AnimGraphs);

				for (UEdGraph* Graph : AnimGraphs)
				{
					if (!Graph) continue;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
						if (!SMNode) continue;
						FString NodeTitle = SMNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString();
						if (!SMName.IsEmpty() && !NodeTitle.Contains(SMName)) continue;

						UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
						if (!SMGraph) continue;

						OutStructured->SetStringField(TEXT("state_machine"), NodeTitle);
						TArray<TSharedPtr<FJsonValue>> States, Transitions;

						for (UEdGraphNode* SN : SMGraph->Nodes)
						{
							if (UAnimStateNode* StateN = Cast<UAnimStateNode>(SN))
							{
								TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
								SJ->SetStringField(TEXT("name"), StateN->GetStateName());
								SJ->SetStringField(TEXT("node_guid"), StateN->NodeGuid.ToString());
								SJ->SetBoolField(TEXT("is_always_relevant"), StateN->bAlwaysResetOnEntry);
								States.Add(MakeShared<FJsonValueObject>(SJ));
							}
							else if (UAnimStateTransitionNode* TransN = Cast<UAnimStateTransitionNode>(SN))
							{
								TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
								if (TransN->GetPreviousState()) TJ->SetStringField(TEXT("from"), TransN->GetPreviousState()->GetStateName());
								if (TransN->GetNextState()) TJ->SetStringField(TEXT("to"), TransN->GetNextState()->GetStateName());
								TJ->SetNumberField(TEXT("crossfade_duration"), TransN->CrossfadeDuration);
								TJ->SetBoolField(TEXT("automatic_rule"), TransN->bAutomaticRuleBasedOnSequencePlayerInState);
								Transitions.Add(MakeShared<FJsonValueObject>(TJ));
							}
						}
						OutStructured->SetArrayField(TEXT("states"), States);
						OutStructured->SetArrayField(TEXT("transitions"), Transitions);
						OutStructured->SetNumberField(TEXT("state_count"), States.Num());
						OutStructured->SetNumberField(TEXT("transition_count"), Transitions.Num());
						OutSummary = FString::Printf(TEXT("State machine '%s': %d states, %d transitions"), *NodeTitle, States.Num(), Transitions.Num());
						return true;
					}
				}
				OutError = SMName.IsEmpty() ? TEXT("No state machines found.") : FString::Printf(TEXT("State machine '%s' not found."), *SMName);
				return false;
			}
		, nullptr
		, 5
		});

		// ---- anim_bp_set_state_details ----
		Registry.Register({
			TEXT("anim_bp_set_state_details"),
			TEXT("Set properties on a state node in an AnimBP state machine: always-reset-on-entry flag."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("state_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("always_reset_on_entry"), FSololmcpSchemaBuilder::Boolean(TEXT("Reset state on re-entry"))}
				},
				{TEXT("asset_path"), TEXT("state_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, SMName, StateName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("state_name"), StateName))
				{ OutError = TEXT("Missing asset_path or state_name."); return false; }
				Arguments->TryGetStringField(TEXT("state_machine_name"), SMName);
				UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!AnimBP) { OutError = TEXT("Not an AnimBlueprint."); return false; }

				TArray<UEdGraph*> AllGraphs;
				AnimBP->GetAllGraphs(AllGraphs);
				for (UEdGraph* Graph : AllGraphs)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
						if (!SMNode || (!SMName.IsEmpty() && !SMNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString().Contains(SMName))) continue;
						UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
						if (!SMGraph) continue;
						for (UEdGraphNode* SN : SMGraph->Nodes)
						{
							UAnimStateNode* StateN = Cast<UAnimStateNode>(SN);
							if (!StateN || StateN->GetStateName() != StateName) continue;
							const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "AnimBPSetState", "SOMOLMCP Set State Details"));
							StateN->Modify();
							bool bReset = false;
							if (Arguments->TryGetBoolField(TEXT("always_reset_on_entry"), bReset)) StateN->bAlwaysResetOnEntry = bReset;
							FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
							OutStructured->SetStringField(TEXT("state_name"), StateName);
							OutStructured->SetBoolField(TEXT("always_reset_on_entry"), StateN->bAlwaysResetOnEntry);
							OutSummary = FString::Printf(TEXT("Updated state '%s' properties."), *StateName);
							return true;
						}
					}
				}
				OutError = FString::Printf(TEXT("State '%s' not found."), *StateName);
				return false;
			}
		, nullptr
		, 5
		});

		// ---- anim_bp_set_entry_state ----
		Registry.Register({
			TEXT("anim_bp_set_entry_state"),
			TEXT("Set the default entry state for a state machine by connecting the entry node to the specified state."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("state_name"), FSololmcpSchemaBuilder::String(TEXT("State to set as entry point"))}
				},
				{TEXT("asset_path"), TEXT("state_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, SMName, StateName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("state_name"), StateName))
				{ OutError = TEXT("Missing asset_path or state_name."); return false; }
				Arguments->TryGetStringField(TEXT("state_machine_name"), SMName);
				// Audit round 7: distinguish "load failed" from "wrong-type asset" — preserve LoadAsset's
				// own error string when loading fails, but emit the asset_not_anim_blueprint sentinel when
				// the asset loads but is not a UAnimBlueprint. Previously LoadAsset's OutError was
				// overwritten with a generic "Not an AnimBlueprint." which masked missing-asset bugs and
				// gave callers no way to tell wrong-type from missing-asset.
				FString LoadErr;
				UObject* RawAsset = Context.Services.LoadAsset(AssetPath, LoadErr);
				if (!RawAsset) { OutError = FString::Printf(TEXT("asset_not_anim_blueprint: failed to load '%s' (%s)"), *AssetPath, *LoadErr); return false; }
				UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(RawAsset);
				if (!AnimBP) { OutError = FString::Printf(TEXT("asset_not_anim_blueprint: '%s' is %s, not UAnimBlueprint"), *AssetPath, *RawAsset->GetClass()->GetName()); return false; }

				TArray<UEdGraph*> AllGraphs;
				AnimBP->GetAllGraphs(AllGraphs);
				for (UEdGraph* Graph : AllGraphs)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
						if (!SMNode || (!SMName.IsEmpty() && !SMNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString().Contains(SMName))) continue;
						UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
						if (!SMGraph) continue;

						// Find entry node and target state
						UAnimStateEntryNode* EntryNode = nullptr;
						UAnimStateNode* TargetState = nullptr;
						for (UEdGraphNode* SN : SMGraph->Nodes)
						{
							if (UAnimStateEntryNode* EN = Cast<UAnimStateEntryNode>(SN)) EntryNode = EN;
							if (UAnimStateNode* ST = Cast<UAnimStateNode>(SN))
							{
								if (ST->GetStateName() == StateName) TargetState = ST;
							}
						}
						if (!EntryNode) { OutError = TEXT("Entry node not found."); return false; }
						if (!TargetState) { OutError = FString::Printf(TEXT("State '%s' not found."), *StateName); return false; }

						const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "AnimBPSetEntry", "SOMOLMCP Set Entry State"));
						EntryNode->Modify();
						// Break existing connections and connect to target
						#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
						UEdGraphPin* EntryOutputPin = EntryNode->Pins.IsValidIndex(0) ? EntryNode->Pins[0] : nullptr;
						#else
						UEdGraphPin* EntryOutputPin = EntryNode->GetOutputPin();
						#endif
						if (!EntryOutputPin || !TargetState->GetInputPin()) { OutError = TEXT("Entry or target state pin is unavailable."); return false; }
						EntryOutputPin->BreakAllPinLinks();
						EntryOutputPin->MakeLinkTo(TargetState->GetInputPin());
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
						OutStructured->SetStringField(TEXT("entry_state"), StateName);
						OutSummary = FString::Printf(TEXT("Set entry state to '%s'."), *StateName);
						return true;
					}
				}
				OutError = TEXT("State machine not found.");
				return false;
			}
		, nullptr
		, 5
		});

		// ---- anim_bp_get_all_state_machines ----
		Registry.Register({
			TEXT("anim_bp_get_all_state_machines"),
			TEXT("List all state machines in an Animation Blueprint."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath)) { OutError = TEXT("Missing asset_path."); return false; }
				UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!AnimBP) { OutError = TEXT("Not an AnimBlueprint."); return false; }

				TArray<UEdGraph*> AllGraphs;
				AnimBP->GetAllGraphs(AllGraphs);
				TArray<TSharedPtr<FJsonValue>> SMList;
				for (UEdGraph* Graph : AllGraphs)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
						if (!SMNode) continue;
						TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
						SJ->SetStringField(TEXT("name"), SMNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString());
						SJ->SetStringField(TEXT("graph_name"), Graph->GetName());
						int32 StateCount = 0, TransCount = 0;
						if (SMNode->EditorStateMachineGraph)
						{
							for (UEdGraphNode* SN : SMNode->EditorStateMachineGraph->Nodes)
							{
								if (Cast<UAnimStateNode>(SN)) StateCount++;
								else if (Cast<UAnimStateTransitionNode>(SN)) TransCount++;
							}
						}
						SJ->SetNumberField(TEXT("state_count"), StateCount);
						SJ->SetNumberField(TEXT("transition_count"), TransCount);
						SMList.Add(MakeShared<FJsonValueObject>(SJ));
					}
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetArrayField(TEXT("state_machines"), SMList);
				OutSummary = FString::Printf(TEXT("Found %d state machines in %s"), SMList.Num(), *AnimBP->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- anim_bp_add_conduit ----
		Registry.Register({
			TEXT("anim_bp_add_conduit"),
			TEXT("Add a conduit (multi-way transition branch) to a state machine. Conduits allow routing transitions through a single evaluation point."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("state_machine_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("conduit_name"), FSololmcpSchemaBuilder::String(TEXT("Name/label for the conduit"))}
				},
				{TEXT("asset_path"), TEXT("conduit_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, SMName, ConduitName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || !Arguments->TryGetStringField(TEXT("conduit_name"), ConduitName))
				{ OutError = TEXT("Missing asset_path or conduit_name."); return false; }
				Arguments->TryGetStringField(TEXT("state_machine_name"), SMName);
				UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
				if (!AnimBP) { OutError = TEXT("Not an AnimBlueprint."); return false; }

				TArray<UEdGraph*> AllGraphs;
				AnimBP->GetAllGraphs(AllGraphs);
				for (UEdGraph* Graph : AllGraphs)
				{
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
						if (!SMNode || (!SMName.IsEmpty() && !SMNode->GetNodeTitle(ENodeTitleType::EditableTitle).ToString().Contains(SMName))) continue;
						UEdGraph* SMGraph = SMNode->EditorStateMachineGraph;
						if (!SMGraph) continue;

						const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "AnimBPAddConduit", "SOMOLMCP Add Conduit"));
						// Create conduit node  
						FGraphNodeCreator<UAnimStateConduitNode> Creator(*SMGraph);
						UAnimStateConduitNode* Conduit = Creator.CreateNode(true);
						if (!Conduit) { OutError = TEXT("Failed to create conduit node."); return false; }
						Conduit->NodePosX = 300;
						Conduit->NodePosY = 0;
						Creator.Finalize();

						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
						OutStructured->SetStringField(TEXT("conduit_name"), ConduitName);
						OutStructured->SetStringField(TEXT("node_guid"), Conduit->NodeGuid.ToString());
						OutSummary = FString::Printf(TEXT("Added conduit '%s' to state machine."), *ConduitName);
						return true;
					}
				}
				OutError = TEXT("State machine not found.");
				return false;
			}
		, nullptr
		, 5
		});

		// ============================================================================
		// v1.8.0 MegaLights Enhancement (4 tools, Pure C++)
		// ============================================================================

		// ---- megalights_set_light_property ----
		Registry.Register({
			TEXT("megalights_set_light_property"),
			TEXT("Set MegaLights and lighting properties on a specific light actor. Configures intensity, color, attenuation, shadows, and MegaLights-specific settings."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Light actor label/name"))},
					{TEXT("intensity"), FSololmcpSchemaBuilder::Number(TEXT("Light intensity"))},
					{TEXT("color"), ColorSchema()},
					{TEXT("attenuation_radius"), FSololmcpSchemaBuilder::Number(TEXT("Attenuation radius in cm"))},
					{TEXT("cast_shadows"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable shadow casting"))},
					{TEXT("volumetric_scattering"), FSololmcpSchemaBuilder::Number(TEXT("Volumetric scattering intensity (0-1)"))},
					{TEXT("use_temperature"), FSololmcpSchemaBuilder::Boolean(TEXT("Use color temperature"))},
					{TEXT("temperature"), FSololmcpSchemaBuilder::Number(TEXT("Color temperature in Kelvin"))}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId)) { OutError = TEXT("Missing actor."); return false; }
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor) return false;

				ULightComponent* LightComp = Actor->FindComponentByClass<ULightComponent>();
				if (!LightComp) { OutError = TEXT("Actor has no light component."); return false; }

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "MegaLightsSetProp", "SOMOLMCP Set Light Property"));
				LightComp->Modify();
				double Val;
				if (Arguments->TryGetNumberField(TEXT("intensity"), Val)) LightComp->SetIntensity(Val);
				if (Arguments->TryGetNumberField(TEXT("attenuation_radius"), Val))
				{
					if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(LightComp)) Local->SetAttenuationRadius(Val);
				}
				bool bBool = false;
				if (Arguments->TryGetBoolField(TEXT("cast_shadows"), bBool)) LightComp->SetCastShadows(bBool);
				if (Arguments->TryGetNumberField(TEXT("volumetric_scattering"), Val)) LightComp->SetVolumetricScatteringIntensity(Val);
				if (Arguments->TryGetBoolField(TEXT("use_temperature"), bBool)) LightComp->SetUseTemperature(bBool);
				if (Arguments->TryGetNumberField(TEXT("temperature"), Val)) LightComp->SetTemperature(Val);

				TSharedPtr<FJsonObject> ColorObj;
				if (TryGetObjectField(Arguments, TEXT("color"), ColorObj) && ColorObj.IsValid())
				{
					double R = 1, G = 1, B = 1, A = 1;
					ColorObj->TryGetNumberField(TEXT("r"), R);
					ColorObj->TryGetNumberField(TEXT("g"), G);
					ColorObj->TryGetNumberField(TEXT("b"), B);
					ColorObj->TryGetNumberField(TEXT("a"), A);
					LightComp->SetLightColor(FLinearColor(R, G, B, A));
				}

				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutStructured->SetStringField(TEXT("light_type"), LightComp->GetClass()->GetName());
				OutStructured->SetNumberField(TEXT("intensity"), LightComp->Intensity);
				OutSummary = FString::Printf(TEXT("Updated light properties on %s"), *Actor->GetActorLabel());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- megalights_create_light ----
		Registry.Register({
			TEXT("megalights_create_light"),
			TEXT("Create a light actor (Point, Spot, Rect, or Directional) with configurable properties."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("light_type"), FSololmcpSchemaBuilder::String(TEXT("Light type"), {TEXT("PointLight"), TEXT("SpotLight"), TEXT("RectLight"), TEXT("DirectionalLight")})},
					{TEXT("location"), VectorSchema()},
					{TEXT("rotation"), FSololmcpSchemaBuilder::Object({{TEXT("pitch"), FSololmcpSchemaBuilder::Number()}, {TEXT("yaw"), FSololmcpSchemaBuilder::Number()}, {TEXT("roll"), FSololmcpSchemaBuilder::Number()}})},
					{TEXT("intensity"), FSololmcpSchemaBuilder::Number(TEXT("Light intensity (default 5000)"))},
					{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label"))},
					{TEXT("color"), ColorSchema()},
					{TEXT("attenuation_radius"), FSololmcpSchemaBuilder::Number(TEXT("Attenuation radius in cm"))},
					{TEXT("cast_shadows"), FSololmcpSchemaBuilder::Boolean(TEXT("Cast shadows (default true)"))}
				},
				{TEXT("light_type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString LightType;
				if (!Arguments->TryGetStringField(TEXT("light_type"), LightType)) { OutError = TEXT("Missing light_type."); return false; }

				FString ClassPath;
				if (LightType == TEXT("PointLight")) ClassPath = TEXT("/Script/Engine.PointLight");
				else if (LightType == TEXT("SpotLight")) ClassPath = TEXT("/Script/Engine.SpotLight");
				else if (LightType == TEXT("RectLight")) ClassPath = TEXT("/Script/Engine.RectLight");
				else if (LightType == TEXT("DirectionalLight")) ClassPath = TEXT("/Script/Engine.DirectionalLight");
				else { OutError = FString::Printf(TEXT("Unknown light_type: %s"), *LightType); return false; }

				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World) { OutError = TEXT("No editor world."); return false; }
				if (LightType == TEXT("DirectionalLight"))
				{
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

				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				TSharedPtr<FJsonObject> LocObj, RotObj;
				if (TryGetObjectField(Arguments, TEXT("location"), LocObj)) FSololmcpEditorServices::JsonToVector(LocObj, Location);
				if (TryGetObjectField(Arguments, TEXT("rotation"), RotObj))
				{
					double P = 0, Y = 0, R = 0;
					RotObj->TryGetNumberField(TEXT("pitch"), P); RotObj->TryGetNumberField(TEXT("yaw"), Y); RotObj->TryGetNumberField(TEXT("roll"), R);
					Rotation = FRotator(P, Y, R);
				}

				UClass* LightClass = LoadClass<AActor>(nullptr, *ClassPath);
				if (!LightClass) { OutError = TEXT("Light class not found."); return false; }

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "MegaLightsCreate", "SOMOLMCP Create Light"));
				FTransform SpawnTransform(Rotation, Location);
				AActor* LightActor = World->SpawnActor(LightClass, &Location, &Rotation);
				if (!LightActor) { OutError = TEXT("Failed to spawn light."); return false; }

				FString Label;
				if (Arguments->TryGetStringField(TEXT("actor_label"), Label) && !Label.IsEmpty()) LightActor->SetActorLabel(Label);

				ULightComponent* LightComp = LightActor->FindComponentByClass<ULightComponent>();
				if (LightComp)
				{
					double Intensity = 5000.0;
					if (Arguments->TryGetNumberField(TEXT("intensity"), Intensity)) LightComp->SetIntensity(Intensity);
					double Radius;
					if (Arguments->TryGetNumberField(TEXT("attenuation_radius"), Radius))
					{
						if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(LightComp)) Local->SetAttenuationRadius(Radius);
					}
					bool bShadows = true;
					if (Arguments->TryGetBoolField(TEXT("cast_shadows"), bShadows)) LightComp->SetCastShadows(bShadows);
				}

				OutStructured = FSololmcpEditorServices::MakeActorReference(LightActor);
				OutStructured->SetStringField(TEXT("light_type"), LightType);
				OutSummary = FString::Printf(TEXT("Created %s at (%g, %g, %g)"), *LightType, Location.X, Location.Y, Location.Z);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- megalights_batch_configure ----
		Registry.Register({
			TEXT("megalights_batch_configure"),
			TEXT("Batch configure lighting properties on all light actors matching a type filter in the current scene."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("light_type_filter"), FSololmcpSchemaBuilder::String(TEXT("Filter by light type (PointLight, SpotLight, RectLight, DirectionalLight, or All)"), {TEXT("All"), TEXT("PointLight"), TEXT("SpotLight"), TEXT("RectLight"), TEXT("DirectionalLight")})},
					{TEXT("intensity"), FSololmcpSchemaBuilder::Number(TEXT("Set intensity on all matched lights"))},
					{TEXT("cast_shadows"), FSololmcpSchemaBuilder::Boolean(TEXT("Set shadow casting"))},
					{TEXT("attenuation_radius"), FSololmcpSchemaBuilder::Number(TEXT("Set attenuation radius"))},
					{TEXT("temperature"), FSololmcpSchemaBuilder::Number(TEXT("Set color temperature"))}
				},
				{TEXT("light_type_filter")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Filter;
				if (!Arguments->TryGetStringField(TEXT("light_type_filter"), Filter)) Filter = TEXT("All");
				UWorld* World = Context.Services.GetEditorWorld(OutError);
				if (!World) { OutError = TEXT("No editor world."); return false; }

				const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "MegaLightsBatch", "SOMOLMCP Batch Configure Lights"));
				int32 Count = 0;
				TArray<TSharedPtr<FJsonValue>> Modified;

				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					ULightComponent* LC = Actor->FindComponentByClass<ULightComponent>();
					if (!LC) continue;
					FString LCName = LC->GetClass()->GetName();
					if (Filter != TEXT("All") && !LCName.Contains(Filter.Replace(TEXT("Light"), TEXT("")))) continue;

					LC->Modify();
					double Val;
					if (Arguments->TryGetNumberField(TEXT("intensity"), Val)) LC->SetIntensity(Val);
					bool bVal;
					if (Arguments->TryGetBoolField(TEXT("cast_shadows"), bVal)) LC->SetCastShadows(bVal);
					if (Arguments->TryGetNumberField(TEXT("attenuation_radius"), Val))
					{
						if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(LC)) Local->SetAttenuationRadius(Val);
					}
					if (Arguments->TryGetNumberField(TEXT("temperature"), Val)) { LC->SetUseTemperature(true); LC->SetTemperature(Val); }

					TSharedRef<FJsonObject> MJ = MakeShared<FJsonObject>();
					MJ->SetStringField(TEXT("actor"), Actor->GetActorLabel());
					MJ->SetStringField(TEXT("type"), LCName);
					Modified.Add(MakeShared<FJsonValueObject>(MJ));
					Count++;
				}
				OutStructured->SetArrayField(TEXT("modified_lights"), Modified);
				OutStructured->SetNumberField(TEXT("count"), Count);
				OutSummary = FString::Printf(TEXT("Batch configured %d lights (filter: %s)"), Count, *Filter);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- megalights_get_light_info ----
		Registry.Register({
			TEXT("megalights_get_light_info"),
			TEXT("Get detailed lighting information for a specific light actor or all lights in the scene."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Light actor label (optional, returns all lights if omitted)"))},
					{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Max results when listing all lights (default 50)"))}
				},
				{}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				Arguments->TryGetStringField(TEXT("actor"), ActorId);
				int32 MaxResults = 50;
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);

				auto LightToJson = [](AActor* Actor, ULightComponent* LC) -> TSharedRef<FJsonObject>
				{
					TSharedRef<FJsonObject> LJ = MakeShared<FJsonObject>();
					LJ->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
					LJ->SetStringField(TEXT("actor_name"), Actor->GetName());
					LJ->SetStringField(TEXT("light_type"), LC->GetClass()->GetName());
					LJ->SetNumberField(TEXT("intensity"), LC->Intensity);
					LJ->SetBoolField(TEXT("cast_shadows"), LC->CastShadows);
					FLinearColor Color = LC->GetLightColor();
					TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
					CJ->SetNumberField(TEXT("r"), Color.R); CJ->SetNumberField(TEXT("g"), Color.G); CJ->SetNumberField(TEXT("b"), Color.B);
					LJ->SetObjectField(TEXT("color"), CJ);
					LJ->SetBoolField(TEXT("use_temperature"), LC->bUseTemperature);
					LJ->SetNumberField(TEXT("temperature"), LC->Temperature);
					if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(LC)) LJ->SetNumberField(TEXT("attenuation_radius"), Local->AttenuationRadius);
					return LJ;
				};

				if (!ActorId.IsEmpty())
				{
					AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
					if (!Actor) return false;
					ULightComponent* LC = Actor->FindComponentByClass<ULightComponent>();
					if (!LC) { OutError = TEXT("Actor has no light component."); return false; }
					OutStructured = LightToJson(Actor, LC);
					OutSummary = FString::Printf(TEXT("Light info: %s (%s, intensity=%.0f)"), *Actor->GetActorLabel(), *LC->GetClass()->GetName(), LC->Intensity);
				}
				else
				{
					UWorld* World = Context.Services.GetEditorWorld(OutError);
					if (!World) { OutError = TEXT("No editor world."); return false; }
					TArray<TSharedPtr<FJsonValue>> Lights;
					for (TActorIterator<AActor> It(World); It && Lights.Num() < MaxResults; ++It)
					{
						ULightComponent* LC = (*It)->FindComponentByClass<ULightComponent>();
						if (LC) Lights.Add(MakeShared<FJsonValueObject>(LightToJson(*It, LC)));
					}
					OutStructured->SetArrayField(TEXT("lights"), Lights);
					OutStructured->SetNumberField(TEXT("count"), Lights.Num());
					OutSummary = FString::Printf(TEXT("Found %d lights in scene"), Lights.Num());
				}
				return true;
			},
			nullptr, // IsAvailable
			5 // TTL cache
		});

		// ============================================================================
		// v1.8.0 Camera Track Enhancement (4 new tools + rewrite 2 to C++)
		// ============================================================================

		// ---- sequence_camera_orbit ----
		Registry.Register({
			TEXT("sequence_camera_orbit"),
			TEXT("Generate orbital camera keyframes around a target point. Creates a smooth circular/arc camera motion in a level sequence."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level sequence asset path"))},
					{TEXT("camera_actor"), FSololmcpSchemaBuilder::String(TEXT("Camera actor label"))},
					{TEXT("target"), VectorSchema()},
					{TEXT("radius"), FSololmcpSchemaBuilder::Number(TEXT("Orbit radius in cm (default 500)"))},
					{TEXT("start_angle"), FSololmcpSchemaBuilder::Number(TEXT("Start angle in degrees (default 0)"))},
					{TEXT("end_angle"), FSololmcpSchemaBuilder::Number(TEXT("End angle in degrees (default 360)"))},
					{TEXT("height_offset"), FSololmcpSchemaBuilder::Number(TEXT("Height above target (default 0)"))},
					{TEXT("num_keyframes"), FSololmcpSchemaBuilder::Integer(TEXT("Number of keyframes (default 12)"))},
					{TEXT("duration"), FSololmcpSchemaBuilder::Number(TEXT("Total duration in seconds (default 5.0)"))}
				},
				{TEXT("sequence_path"), TEXT("camera_actor"), TEXT("target")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SeqPath, CamLabel;
				if (!Arguments->TryGetStringField(TEXT("sequence_path"), SeqPath) || !Arguments->TryGetStringField(TEXT("camera_actor"), CamLabel))
				{ OutError = TEXT("Missing sequence_path or camera_actor."); return false; }
				FVector Target = FVector::ZeroVector;
				TSharedPtr<FJsonObject> TargetObj;
				if (!TryGetObjectField(Arguments, TEXT("target"), TargetObj) || !FSololmcpEditorServices::JsonToVector(TargetObj, Target))
				{ OutError = TEXT("Missing or invalid target."); return false; }

				double Radius = 500, StartAngle = 0, EndAngle = 360, HeightOffset = 0, Duration = 5.0;
				int32 NumKeyframes = 12;
				Arguments->TryGetNumberField(TEXT("radius"), Radius);
				Arguments->TryGetNumberField(TEXT("start_angle"), StartAngle);
				Arguments->TryGetNumberField(TEXT("end_angle"), EndAngle);
				Arguments->TryGetNumberField(TEXT("height_offset"), HeightOffset);
				Arguments->TryGetNumberField(TEXT("num_keyframes"), NumKeyframes);
				Arguments->TryGetNumberField(TEXT("duration"), Duration);
				NumKeyframes = FMath::Clamp(NumKeyframes, 2, 120);

				// Generate keyframe transforms
				TArray<TSharedPtr<FJsonValue>> KeyframeArr;
				for (int32 i = 0; i < NumKeyframes; ++i)
				{
					const double t = static_cast<double>(i) / (NumKeyframes - 1);
					const double AngleDeg = FMath::Lerp(StartAngle, EndAngle, t);
					const double AngleRad = FMath::DegreesToRadians(AngleDeg);

					FVector CamPos(Target.X + Radius * FMath::Cos(AngleRad), Target.Y + Radius * FMath::Sin(AngleRad), Target.Z + HeightOffset);
					FRotator LookAt = (Target - CamPos).Rotation();
					double TimeS = Duration * t;

					TSharedRef<FJsonObject> KF = MakeShared<FJsonObject>();
					KF->SetNumberField(TEXT("time"), TimeS);
					KF->SetObjectField(TEXT("location"), VectorToJson(CamPos));
					TSharedRef<FJsonObject> RJ = MakeShared<FJsonObject>();
					RJ->SetNumberField(TEXT("pitch"), LookAt.Pitch);
					RJ->SetNumberField(TEXT("yaw"), LookAt.Yaw);
					RJ->SetNumberField(TEXT("roll"), LookAt.Roll);
					KF->SetObjectField(TEXT("rotation"), RJ);
					KeyframeArr.Add(MakeShared<FJsonValueObject>(KF));
				}
				OutStructured->SetStringField(TEXT("sequence_path"), SeqPath);
				OutStructured->SetStringField(TEXT("camera_actor"), CamLabel);
				OutStructured->SetArrayField(TEXT("keyframes"), KeyframeArr);
				OutStructured->SetNumberField(TEXT("keyframe_count"), NumKeyframes);
				OutStructured->SetNumberField(TEXT("duration"), Duration);
				OutStructured->SetStringField(TEXT("usage_hint"), TEXT("Use sequence_set_camera_keyframe to apply each keyframe to the sequence."));
				OutSummary = FString::Printf(TEXT("Generated %d orbital keyframes (%.0f deg arc, radius=%.0f, duration=%.1fs)"), NumKeyframes, EndAngle - StartAngle, Radius, Duration);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- sequence_camera_dolly ----
		Registry.Register({
			TEXT("sequence_camera_dolly"),
			TEXT("Generate linear dolly/zoom camera keyframes between two positions."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level sequence asset path"))},
					{TEXT("camera_actor"), FSololmcpSchemaBuilder::String(TEXT("Camera actor label"))},
					{TEXT("start_location"), VectorSchema()},
					{TEXT("end_location"), VectorSchema()},
					{TEXT("look_at_target"), VectorSchema()},
					{TEXT("num_keyframes"), FSololmcpSchemaBuilder::Integer(TEXT("Number of keyframes (default 6)"))},
					{TEXT("duration"), FSololmcpSchemaBuilder::Number(TEXT("Duration in seconds (default 3.0)"))},
					{TEXT("start_fov"), FSololmcpSchemaBuilder::Number(TEXT("Start FOV (optional)"))},
					{TEXT("end_fov"), FSololmcpSchemaBuilder::Number(TEXT("End FOV (optional)"))}
				},
				{TEXT("sequence_path"), TEXT("camera_actor"), TEXT("start_location"), TEXT("end_location")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SeqPath, CamLabel;
				if (!Arguments->TryGetStringField(TEXT("sequence_path"), SeqPath) || !Arguments->TryGetStringField(TEXT("camera_actor"), CamLabel))
				{ OutError = TEXT("Missing sequence_path or camera_actor."); return false; }
				FVector StartLoc, EndLoc;
				TSharedPtr<FJsonObject> SObj, EObj;
				if (!TryGetObjectField(Arguments, TEXT("start_location"), SObj) || !FSololmcpEditorServices::JsonToVector(SObj, StartLoc) ||
					!TryGetObjectField(Arguments, TEXT("end_location"), EObj) || !FSololmcpEditorServices::JsonToVector(EObj, EndLoc))
				{ OutError = TEXT("Missing start_location or end_location."); return false; }

				FVector LookAt = (StartLoc + EndLoc) * 0.5;
				TSharedPtr<FJsonObject> LookObj;
				if (TryGetObjectField(Arguments, TEXT("look_at_target"), LookObj)) FSololmcpEditorServices::JsonToVector(LookObj, LookAt);

				double Duration = 3.0, StartFov = -1, EndFov = -1;
				int32 NumKF = 6;
				Arguments->TryGetNumberField(TEXT("duration"), Duration);
				Arguments->TryGetNumberField(TEXT("num_keyframes"), NumKF);
				Arguments->TryGetNumberField(TEXT("start_fov"), StartFov);
				Arguments->TryGetNumberField(TEXT("end_fov"), EndFov);
				NumKF = FMath::Clamp(NumKF, 2, 120);

				TArray<TSharedPtr<FJsonValue>> Keyframes;
				for (int32 i = 0; i < NumKF; ++i)
				{
					const double t = static_cast<double>(i) / (NumKF - 1);
					FVector Pos = FMath::Lerp(StartLoc, EndLoc, t);
					FRotator Rot = (LookAt - Pos).Rotation();
					TSharedRef<FJsonObject> KF = MakeShared<FJsonObject>();
					KF->SetNumberField(TEXT("time"), Duration * t);
					KF->SetObjectField(TEXT("location"), VectorToJson(Pos));
					TSharedRef<FJsonObject> RJ = MakeShared<FJsonObject>();
					RJ->SetNumberField(TEXT("pitch"), Rot.Pitch); RJ->SetNumberField(TEXT("yaw"), Rot.Yaw); RJ->SetNumberField(TEXT("roll"), Rot.Roll);
					KF->SetObjectField(TEXT("rotation"), RJ);
					if (StartFov > 0 && EndFov > 0) KF->SetNumberField(TEXT("fov"), FMath::Lerp(StartFov, EndFov, t));
					Keyframes.Add(MakeShared<FJsonValueObject>(KF));
				}
				OutStructured->SetStringField(TEXT("sequence_path"), SeqPath);
				OutStructured->SetStringField(TEXT("camera_actor"), CamLabel);
				OutStructured->SetArrayField(TEXT("keyframes"), Keyframes);
				OutStructured->SetNumberField(TEXT("keyframe_count"), NumKF);
				OutStructured->SetStringField(TEXT("usage_hint"), TEXT("Use sequence_set_camera_keyframe to apply each keyframe to the sequence."));
				OutSummary = FString::Printf(TEXT("Generated %d dolly keyframes (%.1fs)"), NumKF, Duration);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- sequence_get_camera_bindings ----
		Registry.Register({
			TEXT("sequence_get_camera_bindings"),
			TEXT("List all camera bindings in a level sequence, including their tracks and section ranges."),
			FSololmcpSchemaBuilder::Object({{TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("sequence_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString SeqPath;
				if (!Arguments->TryGetStringField(TEXT("sequence_path"), SeqPath)) { OutError = TEXT("Missing sequence_path."); return false; }
				ULevelSequence* Seq = Cast<ULevelSequence>(Context.Services.LoadAsset(SeqPath, OutError));
				if (!Seq) { OutError = TEXT("Not a LevelSequence."); return false; }
				UMovieScene* Movie = Seq->GetMovieScene();
				if (!Movie) { OutError = TEXT("MovieScene unavailable."); return false; }

				TArray<TSharedPtr<FJsonValue>> Bindings;
				for (int32 i = 0; i < Movie->GetPossessableCount(); ++i)
				{
					const FMovieScenePossessable& Poss = Movie->GetPossessable(i);
					FString ClassName = Poss.GetPossessedObjectClass() ? Poss.GetPossessedObjectClass()->GetName() : TEXT("Unknown");
					bool bIsCamera = ClassName.Contains(TEXT("Camera")) || ClassName.Contains(TEXT("CineCamera"));
					TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
					BJ->SetStringField(TEXT("name"), Poss.GetName());
					BJ->SetStringField(TEXT("class"), ClassName);
					BJ->SetBoolField(TEXT("is_camera"), bIsCamera);
					BJ->SetStringField(TEXT("guid"), Poss.GetGuid().ToString());

					// List tracks on this binding
					FMovieSceneBinding* Binding = Movie->FindBinding(Poss.GetGuid());
					if (Binding)
					{
						TArray<TSharedPtr<FJsonValue>> TrackArr;
						for (UMovieSceneTrack* Track : Binding->GetTracks())
						{
							if (!Track) continue;
							TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
							TJ->SetStringField(TEXT("type"), Track->GetClass()->GetName());
							TJ->SetStringField(TEXT("name"), Track->GetTrackName().ToString());
							TrackArr.Add(MakeShared<FJsonValueObject>(TJ));
						}
						BJ->SetArrayField(TEXT("tracks"), TrackArr);
					}
					if (bIsCamera) Bindings.Add(MakeShared<FJsonValueObject>(BJ));
				}
				OutStructured->SetStringField(TEXT("sequence_path"), SeqPath);
				OutStructured->SetArrayField(TEXT("camera_bindings"), Bindings);
				OutStructured->SetNumberField(TEXT("count"), Bindings.Num());
				OutSummary = FString::Printf(TEXT("Found %d camera bindings in sequence"), Bindings.Num());
				return true;
			},
			nullptr, // IsAvailable
			10 // TTL cache
		});

		// ---- sequence_camera_shake ----
		Registry.Register({
			TEXT("sequence_camera_shake"),
			TEXT("Add or configure a camera shake source component on a camera actor for procedural camera shake effects."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Camera actor label"))},
					{TEXT("inner_radius"), FSololmcpSchemaBuilder::Number(TEXT("Inner attenuation radius"))},
					{TEXT("outer_radius"), FSololmcpSchemaBuilder::Number(TEXT("Outer attenuation radius"))},
					{TEXT("falloff"), FSololmcpSchemaBuilder::Number(TEXT("Falloff exponent (default 2.0)"))}
				},
				{TEXT("actor")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId;
				if (!Arguments->TryGetStringField(TEXT("actor"), ActorId)) { OutError = TEXT("Missing actor."); return false; }
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor) return false;

				UCameraShakeSourceComponent* ShakeComp = Actor->FindComponentByClass<UCameraShakeSourceComponent>();
				if (!ShakeComp)
				{
					const FScopedTransaction Txn(NSLOCTEXT("SOMOLMCP", "CameraShakeAdd", "SOMOLMCP Add Camera Shake"));
					ShakeComp = NewObject<UCameraShakeSourceComponent>(Actor, NAME_None, RF_Transactional);
					if (!ShakeComp) { OutError = TEXT("Failed to create CameraShakeSourceComponent."); return false; }
					Actor->AddInstanceComponent(ShakeComp);
					ShakeComp->RegisterComponent();
				}

				double Val;
				if (Arguments->TryGetNumberField(TEXT("inner_radius"), Val)) ShakeComp->InnerAttenuationRadius = Val;
				if (Arguments->TryGetNumberField(TEXT("outer_radius"), Val)) ShakeComp->OuterAttenuationRadius = Val;
				if (Arguments->TryGetNumberField(TEXT("falloff"), Val)) // UE 5.7: AttenuationFalloff property removed - no direct setter available
				// ShakeComp->AttenuationFalloff = Val;

				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutStructured->SetStringField(TEXT("shake_component"), ShakeComp->GetName());
				OutStructured->SetNumberField(TEXT("inner_radius"), ShakeComp->InnerAttenuationRadius);
				OutStructured->SetNumberField(TEXT("outer_radius"), ShakeComp->OuterAttenuationRadius);
				OutSummary = FString::Printf(TEXT("Camera shake configured on %s"), *Actor->GetActorLabel());
				return true;
			}
		, nullptr
		, 5
		});

		// ═══════════════════════════════════════════════════════════════════
		// v1.7.0 — Tier 1: Project Settings / Type Enumeration / DataTable
		//          Enhanced Input / Collision / GameMode  (33 tools)
		// ═══════════════════════════════════════════════════════════════════

		// ---- project_settings_read ----
		Registry.Register({
			TEXT("project_settings_read"),
			TEXT("Read project description, default map, default game mode, and other project-level settings."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				const FProjectDescriptor* Desc = IProjectManager::Get().GetCurrentProject();
				if (Desc)
				{
					OutStructured->SetStringField(TEXT("project_name"), FApp::GetProjectName());
					OutStructured->SetStringField(TEXT("description"), Desc->Description);
				}
				else
				{
					OutStructured->SetStringField(TEXT("project_name"), FApp::GetProjectName());
				}
				FString Val;
				if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameDefaultMap"), Val, GEngineIni))
					OutStructured->SetStringField(TEXT("default_map"), Val);
				if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GlobalDefaultGameMode"), Val, GEngineIni))
					OutStructured->SetStringField(TEXT("default_game_mode"), Val);
				if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("EditorStartupMap"), Val, GEngineIni))
					OutStructured->SetStringField(TEXT("editor_startup_map"), Val);
				OutStructured->SetStringField(TEXT("project_dir"), FPaths::ProjectDir());
				OutStructured->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
				OutSummary = FString::Printf(TEXT("Read project settings for %s"), FApp::GetProjectName());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- project_settings_write ----
		Registry.Register({
			TEXT("project_settings_write"),
			TEXT("Write project settings by modifying ini config values. Specify section, key, and value."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("section"), FSololmcpSchemaBuilder::String(TEXT("INI section path, e.g. /Script/EngineSettings.GameMapsSettings"))},
					{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Config key name"))},
					{TEXT("value"), FSololmcpSchemaBuilder::String(TEXT("Value to set"))},
					{TEXT("ini_file"), FSololmcpSchemaBuilder::String(TEXT("Which ini: Engine, Game, Input, Editor (default: Game)"))}
				},
				{TEXT("section"), TEXT("key"), TEXT("value")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Section = Arguments->GetStringField(TEXT("section"));
				FString Key = Arguments->GetStringField(TEXT("key"));
				FString Value = Arguments->GetStringField(TEXT("value"));
				FString IniFile = Arguments->HasField(TEXT("ini_file")) ? Arguments->GetStringField(TEXT("ini_file")) : TEXT("Game");
				FString IniPath;
				if (IniFile.Equals(TEXT("Engine"), ESearchCase::IgnoreCase)) IniPath = GEngineIni;
				else if (IniFile.Equals(TEXT("Input"), ESearchCase::IgnoreCase)) IniPath = GInputIni;
				else if (IniFile.Equals(TEXT("Editor"), ESearchCase::IgnoreCase)) IniPath = GEditorIni;
				else IniPath = GGameIni;
				GConfig->SetString(*Section, *Key, *Value, IniPath);
				GConfig->Flush(false, IniPath);
				OutStructured->SetStringField(TEXT("section"), Section);
				OutStructured->SetStringField(TEXT("key"), Key);
				OutStructured->SetStringField(TEXT("value"), Value);
				OutSummary = FString::Printf(TEXT("Set [%s] %s = %s"), *Section, *Key, *Value);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- project_plugins_list ----
		Registry.Register({
			TEXT("project_plugins_list"),
			TEXT("List all discovered plugins with their enabled state, version, and category."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("enabled_only"), FSololmcpSchemaBuilder::Boolean(TEXT("Only list enabled plugins"))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				bool bEnabledOnly = false;
				Arguments->TryGetBoolField(TEXT("enabled_only"), bEnabledOnly);
				TArray<TSharedPtr<FJsonValue>> PluginArr;
				TArray<TSharedRef<IPlugin>> AllPlugins = IPluginManager::Get().GetDiscoveredPlugins();
				for (const TSharedRef<IPlugin>& Plug : AllPlugins)
				{
					if (bEnabledOnly && !Plug->IsEnabled()) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Plug->GetName());
					Obj->SetBoolField(TEXT("enabled"), Plug->IsEnabled());
					const FPluginDescriptor& Desc = Plug->GetDescriptor();
					Obj->SetStringField(TEXT("version"), Desc.VersionName);
					Obj->SetStringField(TEXT("category"), Desc.Category);
					Obj->SetStringField(TEXT("description"), Desc.Description);
					Obj->SetBoolField(TEXT("is_engine_plugin"), Plug->GetLoadedFrom() == EPluginLoadedFrom::Engine);
					PluginArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("plugins"), PluginArr);
				OutStructured->SetNumberField(TEXT("count"), PluginArr.Num());
				OutSummary = FString::Printf(TEXT("Found %d plugins"), PluginArr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- project_plugin_set_enabled ----
		Registry.Register({
			TEXT("project_plugin_set_enabled"),
			TEXT("Enable or disable a project plugin by name. Requires editor restart for some plugins."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("plugin_name"), FSololmcpSchemaBuilder::String(TEXT("Plugin name"))},
					{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable or disable"))}
				},
				{TEXT("plugin_name"), TEXT("enabled")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString PluginName = Arguments->GetStringField(TEXT("plugin_name"));
				bool bEnabled = Arguments->GetBoolField(TEXT("enabled"));
				TSharedPtr<IPlugin> Plug = IPluginManager::Get().FindPlugin(PluginName);
				if (!Plug.IsValid()) { OutError = FString::Printf(TEXT("Plugin '%s' not found."), *PluginName); return false; }
				FText FailReason;
				// UE 5.7: SetPluginEnabled moved from IPluginManager to IProjectManager
				bool bOk = IProjectManager::Get().SetPluginEnabled(PluginName, bEnabled, FailReason);
				if (!bOk) { OutError = FString::Printf(TEXT("Failed: %s"), *FailReason.ToString()); return false; }
				FText SaveFailReason;
				if (!IProjectManager::Get().SaveCurrentProjectToDisk(SaveFailReason))
				{
					OutError = FString::Printf(TEXT("Changed project descriptor but failed to save .uproject: %s"), *SaveFailReason.ToString());
					return false;
				}
				TSharedPtr<IPlugin> FreshPlug = IPluginManager::Get().FindPlugin(PluginName);
				OutStructured->SetStringField(TEXT("plugin"), PluginName);
				OutStructured->SetBoolField(TEXT("enabled"), bEnabled);
				OutStructured->SetBoolField(TEXT("project_file_saved"), true);
				OutStructured->SetBoolField(TEXT("runtime_enabled"), FreshPlug.IsValid() && FreshPlug->IsEnabled());
				OutStructured->SetBoolField(TEXT("requires_restart"), true);
				OutSummary = FString::Printf(TEXT("%s plugin '%s'"), bEnabled ? TEXT("Enabled") : TEXT("Disabled"), *PluginName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- project_config_read_ini ----
		Registry.Register({
			TEXT("project_config_read_ini"),
			TEXT("Read a section or specific key from an INI config file."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("section"), FSololmcpSchemaBuilder::String(TEXT("INI section path"))},
					{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Specific key to read (omit for entire section)"))},
					{TEXT("ini_file"), FSololmcpSchemaBuilder::String(TEXT("Which ini: Engine, Game, Input, Editor (default: Game)"))}
				},
				{TEXT("section")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Section = Arguments->GetStringField(TEXT("section"));
				FString IniFile = Arguments->HasField(TEXT("ini_file")) ? Arguments->GetStringField(TEXT("ini_file")) : TEXT("Game");
				FString IniPath;
				if (IniFile.Equals(TEXT("Engine"), ESearchCase::IgnoreCase)) IniPath = GEngineIni;
				else if (IniFile.Equals(TEXT("Input"), ESearchCase::IgnoreCase)) IniPath = GInputIni;
				else if (IniFile.Equals(TEXT("Editor"), ESearchCase::IgnoreCase)) IniPath = GEditorIni;
				else IniPath = GGameIni;
				FString Key;
				if (Arguments->TryGetStringField(TEXT("key"), Key))
				{
					FString Val;
					if (GConfig->GetString(*Section, *Key, Val, IniPath))
					{
						OutStructured->SetStringField(TEXT("value"), Val);
						OutSummary = FString::Printf(TEXT("[%s] %s = %s"), *Section, *Key, *Val);
					}
					else
					{
						OutStructured->SetStringField(TEXT("value"), TEXT(""));
						OutSummary = FString::Printf(TEXT("Key '%s' not found in [%s]"), *Key, *Section);
					}
				}
				else
				{
					TArray<TSharedPtr<FJsonValue>> Entries;
					TArray<FString> Keys;
					GConfig->GetSection(*Section, Keys, IniPath);
					for (const FString& Entry : Keys)
					{
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						FString K, V;
						Entry.Split(TEXT("="), &K, &V);
						Obj->SetStringField(TEXT("key"), K);
						Obj->SetStringField(TEXT("value"), V);
						Entries.Add(MakeShared<FJsonValueObject>(Obj));
					}
					OutStructured->SetArrayField(TEXT("entries"), Entries);
					OutStructured->SetNumberField(TEXT("count"), Entries.Num());
					OutSummary = FString::Printf(TEXT("Read %d entries from [%s]"), Entries.Num(), *Section);
				}
				return true;
			}
		, nullptr
		, 5
		});

		// ---- project_config_write_ini ----
		Registry.Register({
			TEXT("project_config_write_ini"),
			TEXT("Write multiple key-value pairs to an INI config section."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("section"), FSololmcpSchemaBuilder::String(TEXT("INI section path"))},
					{TEXT("values"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Key-value pairs to write"))},
					{TEXT("ini_file"), FSololmcpSchemaBuilder::String(TEXT("Which ini: Engine, Game, Input, Editor (default: Game)"))}
				},
				{TEXT("section"), TEXT("values")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Section = Arguments->GetStringField(TEXT("section"));
				const TSharedPtr<FJsonObject>* ValuesObj;
				if (!Arguments->TryGetObjectField(TEXT("values"), ValuesObj)) { OutError = TEXT("Missing values object."); return false; }
				FString IniFile = Arguments->HasField(TEXT("ini_file")) ? Arguments->GetStringField(TEXT("ini_file")) : TEXT("Game");
				FString IniPath;
				if (IniFile.Equals(TEXT("Engine"), ESearchCase::IgnoreCase)) IniPath = GEngineIni;
				else if (IniFile.Equals(TEXT("Input"), ESearchCase::IgnoreCase)) IniPath = GInputIni;
				else if (IniFile.Equals(TEXT("Editor"), ESearchCase::IgnoreCase)) IniPath = GEditorIni;
				else IniPath = GGameIni;
				int32 Count = 0;
				for (auto& Pair : (*ValuesObj)->Values)
				{
					FString Val;
					if (Pair.Value->TryGetString(Val))
					{
						GConfig->SetString(*Section, *Pair.Key, *Val, IniPath);
						Count++;
					}
				}
				GConfig->Flush(false, IniPath);
				OutStructured->SetStringField(TEXT("section"), Section);
				OutStructured->SetNumberField(TEXT("keys_written"), Count);
				OutSummary = FString::Printf(TEXT("Wrote %d keys to [%s]"), Count, *Section);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- project_maps_list ----
		Registry.Register({
			TEXT("project_maps_list"),
			TEXT("List all map assets (.umap) in the project content directory."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				// v3.10.x worker-safety: avoid FModuleManager::LoadModuleChecked from a worker.
				IAssetRegistry* RegistryPtr = UE::SOMOLMCP::GSololmcpCachedAssetRegistry;
				if (!RegistryPtr)
				{
					OutError = TEXT("AssetRegistry not yet cached (called before plugin initx)");
					return false;
				}
				TArray<FAssetData> Assets;
				RegistryPtr->GetAssetsByClass(UWorld::StaticClass()->GetClassPathName(), Assets, true);
				TArray<TSharedPtr<FJsonValue>> MapArr;
				for (const FAssetData& Asset : Assets)
				{
					if (Asset.PackageName.ToString().StartsWith(TEXT("/Game/")))
					{
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
						Obj->SetStringField(TEXT("path"), Asset.PackageName.ToString());
						MapArr.Add(MakeShared<FJsonValueObject>(Obj));
					}
				}
				OutStructured->SetArrayField(TEXT("maps"), MapArr);
				OutStructured->SetNumberField(TEXT("count"), MapArr.Num());
				OutSummary = FString::Printf(TEXT("Found %d maps in project"), MapArr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- class_list ----
		Registry.Register({
			TEXT("class_list"),
			TEXT("List UClasses in the project, optionally filtered by parent class."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("parent_class"), FSololmcpSchemaBuilder::String(TEXT("Parent class name to filter (e.g. AActor, UActorComponent)"))},
					{TEXT("include_engine"), FSololmcpSchemaBuilder::Boolean(TEXT("Include engine classes (default false)"))},
					{TEXT("max_count"), FSololmcpSchemaBuilder::Integer(TEXT("Max results (default 200)"))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ParentName;
				Arguments->TryGetStringField(TEXT("parent_class"), ParentName);
				bool bIncludeEngine = false;
				Arguments->TryGetBoolField(TEXT("include_engine"), bIncludeEngine);
				int32 MaxCount = 200;
				double MC; if (Arguments->TryGetNumberField(TEXT("max_count"), MC)) MaxCount = static_cast<int32>(MC);
				UClass* ParentClass = nullptr;
				if (!ParentName.IsEmpty())
				{
					// UE 5.7: ANY_PACKAGE removed, use nullptr instead
					ParentClass = FindObject<UClass>(nullptr, *ParentName);
					if (!ParentClass) { OutError = FString::Printf(TEXT("Class '%s' not found."), *ParentName); return false; }
				}
				TArray<TSharedPtr<FJsonValue>> ClassArr;
				for (TObjectIterator<UClass> It; It; ++It)
				{
					UClass* Cls = *It;
					if (ParentClass && !Cls->IsChildOf(ParentClass)) continue;
					if (!bIncludeEngine && !Cls->GetPathName().Contains(TEXT("/Game/"))) continue;
					if (ClassArr.Num() >= MaxCount) break;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Cls->GetName());
					Obj->SetStringField(TEXT("path"), Cls->GetPathName());
					if (Cls->GetSuperClass()) Obj->SetStringField(TEXT("parent"), Cls->GetSuperClass()->GetName());
					Obj->SetBoolField(TEXT("is_abstract"), Cls->HasAnyClassFlags(CLASS_Abstract));
					ClassArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("classes"), ClassArr);
				OutStructured->SetNumberField(TEXT("count"), ClassArr.Num());
				OutSummary = FString::Printf(TEXT("Found %d classes%s"), ClassArr.Num(), ParentClass ? *FString::Printf(TEXT(" derived from %s"), *ParentName) : TEXT(""));
				return true;
		}
		, nullptr
		, 0
		});

		// ---- class_hierarchy ----
		Registry.Register({
			TEXT("class_hierarchy"),
			TEXT("Get the inheritance hierarchy tree for a given class."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("class_name"), FSololmcpSchemaBuilder::String(TEXT("Class name to inspect"))},
					{TEXT("depth"), FSololmcpSchemaBuilder::Integer(TEXT("Max depth for children (default 2)"))}
				},
				{TEXT("class_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ClassName = Arguments->GetStringField(TEXT("class_name"));
				UClass* Cls = FindObject<UClass>(nullptr, *ClassName);
				if (!Cls) { OutError = FString::Printf(TEXT("Class '%s' not found."), *ClassName); return false; }
				int32 Depth = 2;
				double D; if (Arguments->TryGetNumberField(TEXT("depth"), D)) Depth = static_cast<int32>(D);
				TArray<TSharedPtr<FJsonValue>> Ancestors;
				for (UClass* P = Cls->GetSuperClass(); P; P = P->GetSuperClass())
					Ancestors.Add(MakeShared<FJsonValueString>(P->GetName()));
				OutStructured->SetArrayField(TEXT("ancestors"), Ancestors);
				TArray<UClass*> Children;
				GetDerivedClasses(Cls, Children, false);
				TArray<TSharedPtr<FJsonValue>> ChildArr;
				for (UClass* C : Children)
					ChildArr.Add(MakeShared<FJsonValueString>(C->GetName()));
				OutStructured->SetArrayField(TEXT("direct_children"), ChildArr);
				OutStructured->SetStringField(TEXT("class"), Cls->GetName());
				OutStructured->SetNumberField(TEXT("children_count"), ChildArr.Num());
				OutSummary = FString::Printf(TEXT("%s: %d ancestors, %d direct children"), *ClassName, Ancestors.Num(), ChildArr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- struct_list ----
		Registry.Register({
			TEXT("struct_list"),
			TEXT("List UScriptStruct types available in the project."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("filter"), FSololmcpSchemaBuilder::String(TEXT("Name substring filter"))},
					{TEXT("max_count"), FSololmcpSchemaBuilder::Integer(TEXT("Max results (default 100)"))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Filter;
				Arguments->TryGetStringField(TEXT("filter"), Filter);
				int32 MaxCount = 100;
				double MC; if (Arguments->TryGetNumberField(TEXT("max_count"), MC)) MaxCount = static_cast<int32>(MC);
				TArray<TSharedPtr<FJsonValue>> StructArr;
				for (TObjectIterator<UScriptStruct> It; It; ++It)
				{
					UScriptStruct* S = *It;
					if (!Filter.IsEmpty() && !S->GetName().Contains(Filter)) continue;
					if (StructArr.Num() >= MaxCount) break;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), S->GetName());
					Obj->SetStringField(TEXT("path"), S->GetPathName());
					Obj->SetNumberField(TEXT("size"), S->GetStructureSize());
					StructArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("structs"), StructArr);
				OutStructured->SetNumberField(TEXT("count"), StructArr.Num());
				OutSummary = FString::Printf(TEXT("Found %d structs"), StructArr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- enum_list ----
		Registry.Register({
			TEXT("enum_list"),
			TEXT("List all UEnum types registered in the engine and project."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("filter"), FSololmcpSchemaBuilder::String(TEXT("Name substring filter"))},
					{TEXT("max_count"), FSololmcpSchemaBuilder::Integer(TEXT("Max results (default 100)"))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString Filter;
				Arguments->TryGetStringField(TEXT("filter"), Filter);
				int32 MaxCount = 100;
				double MC; if (Arguments->TryGetNumberField(TEXT("max_count"), MC)) MaxCount = static_cast<int32>(MC);
				TArray<TSharedPtr<FJsonValue>> EnumArr;
				for (TObjectIterator<UEnum> It; It; ++It)
				{
					UEnum* E = *It;
					if (!Filter.IsEmpty() && !E->GetName().Contains(Filter)) continue;
					if (EnumArr.Num() >= MaxCount) break;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), E->GetName());
					Obj->SetNumberField(TEXT("num_values"), E->NumEnums());
					EnumArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("enums"), EnumArr);
				OutStructured->SetNumberField(TEXT("count"), EnumArr.Num());
				OutSummary = FString::Printf(TEXT("Found %d enums"), EnumArr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- enum_values ----
		Registry.Register({
			TEXT("enum_values"),
			TEXT("Get all named values of a specific UEnum."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("enum_name"), FSololmcpSchemaBuilder::String(TEXT("Full or short enum name"))}
				},
				{TEXT("enum_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString EnumName = Arguments->GetStringField(TEXT("enum_name"));
				UEnum* E = FindObject<UEnum>(nullptr, *EnumName);
				if (!E) { OutError = FString::Printf(TEXT("Enum '%s' not found."), *EnumName); return false; }
				TArray<TSharedPtr<FJsonValue>> ValArr;
				for (int32 i = 0; i < E->NumEnums(); i++)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), E->GetNameStringByIndex(i));
					Obj->SetStringField(TEXT("display_name"), E->GetDisplayNameTextByIndex(i).ToString());
					Obj->SetNumberField(TEXT("value"), E->GetValueByIndex(i));
					ValArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetStringField(TEXT("enum"), E->GetName());
				OutStructured->SetArrayField(TEXT("values"), ValArr);
				OutStructured->SetNumberField(TEXT("count"), ValArr.Num());
				OutSummary = FString::Printf(TEXT("Enum %s has %d values"), *E->GetName(), ValArr.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_create ----
		Registry.Register({
			TEXT("datatable_create"),
			TEXT("Create a new DataTable asset with a specified row struct type."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path e.g. /Game/Data/DT_Weapons"))},
					{TEXT("row_struct"), FSololmcpSchemaBuilder::String(TEXT("Row struct name e.g. FWeaponData"))}
				},
				{TEXT("asset_path"), TEXT("row_struct")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
				FString RowStructName = Arguments->GetStringField(TEXT("row_struct"));
				UScriptStruct* RowStruct = FindObject<UScriptStruct>(nullptr, *RowStructName);
				if (!RowStruct) { OutError = FString::Printf(TEXT("Struct '%s' not found."), *RowStructName); return false; }
				FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
				FString AssetName = FPackageName::GetShortName(AssetPath);
				UPackage* Package = CreatePackage(*AssetPath);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Package, *AssetName)) { if (!Ex->IsA<UDataTable>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UDataTable"), *AssetPath, *Ex->GetClass()->GetName()); return false; } }
				UDataTable* DT = NewObject<UDataTable>(Package, *AssetName, RF_Public | RF_Standalone);
				DT->RowStruct = RowStruct;
				FAssetRegistryModule::AssetCreated(DT);
				DT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("row_struct"), RowStruct->GetName());
				OutSummary = FString::Printf(TEXT("Created DataTable %s with row type %s"), *AssetName, *RowStruct->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_list_rows ----
		Registry.Register({
			TEXT("datatable_list_rows"),
			TEXT("List all row names in a DataTable."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				TArray<FName> RowNames = DT->GetRowNames();
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FName& Name : RowNames)
					Arr.Add(MakeShared<FJsonValueString>(Name.ToString()));
				OutStructured->SetArrayField(TEXT("rows"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutStructured->SetStringField(TEXT("row_struct"), DT->RowStruct ? DT->RowStruct->GetName() : TEXT("None"));
				OutSummary = FString::Printf(TEXT("DataTable has %d rows"), Arr.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_get_row ----
		Registry.Register({
			TEXT("datatable_get_row"),
			TEXT("Read a specific row from a DataTable as JSON."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("row_name"), FSololmcpSchemaBuilder::String(TEXT("Row name to read"))}
				},
				{TEXT("asset_path"), TEXT("row_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				FString RowName = Arguments->GetStringField(TEXT("row_name"));
				uint8* RowData = DT->FindRowUnchecked(FName(*RowName));
				if (!RowData) { OutError = FString::Printf(TEXT("Row '%s' not found."), *RowName); return false; }
				TSharedRef<FJsonObject> RowJson = MakeShared<FJsonObject>();
				if (DT->RowStruct)
				{
					for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
					{
						FProperty* Prop = *It;
						FString ValStr;
						Prop->ExportTextItem_Direct(ValStr, Prop->ContainerPtrToValuePtr<void>(RowData), nullptr, nullptr, PPF_None);
						RowJson->SetStringField(Prop->GetName(), ValStr);
					}
				}
				OutStructured->SetStringField(TEXT("row_name"), RowName);
				OutStructured->SetObjectField(TEXT("data"), RowJson);
				OutSummary = FString::Printf(TEXT("Read row '%s'"), *RowName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_add_row ----
		Registry.Register({
			TEXT("datatable_add_row"),
			TEXT("Add a new row to a DataTable. Values are provided as string key-value pairs matching property names."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("row_name"), FSololmcpSchemaBuilder::String(TEXT("New row name"))},
					{TEXT("values"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Property name → value string pairs"))}
				},
				{TEXT("asset_path"), TEXT("row_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				if (!DT->RowStruct) { OutError = TEXT("DataTable has no row struct."); return false; }
				FString RowName = Arguments->GetStringField(TEXT("row_name"));
				if (DT->FindRowUnchecked(FName(*RowName))) { OutError = FString::Printf(TEXT("Row '%s' already exists."), *RowName); return false; }
				FStructOnScope RowScope(DT->RowStruct);
				DT->RowStruct->InitializeDefaultValue(RowScope.GetStructMemory());
				const TSharedPtr<FJsonObject>* ValuesObj;
				if (Arguments->TryGetObjectField(TEXT("values"), ValuesObj))
				{
					for (auto& Pair : (*ValuesObj)->Values)
					{
						const FString Key(*Pair.Key);
						FProperty* Prop = DT->RowStruct->FindPropertyByName(FName(*Key));
						if (Prop)
						{
							FString Val; Pair.Value->TryGetString(Val);
							Prop->ImportText_Direct(*Val, Prop->ContainerPtrToValuePtr<void>(RowScope.GetStructMemory()), nullptr, PPF_None);
						}
					}
				}
				SomolAddDataTableRow(DT, FName(*RowName), RowScope.GetStructMemory());
				DT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("row_name"), RowName);
				OutSummary = FString::Printf(TEXT("Added row '%s'"), *RowName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_update_row ----
		Registry.Register({
			TEXT("datatable_update_row"),
			TEXT("Update property values on an existing DataTable row."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("row_name"), FSololmcpSchemaBuilder::String(TEXT("Row name"))},
					{TEXT("values"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Property name → value string pairs"))}
				},
				{TEXT("asset_path"), TEXT("row_name"), TEXT("values")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				FString RowName = Arguments->GetStringField(TEXT("row_name"));
				uint8* RowData = DT->FindRowUnchecked(FName(*RowName));
				if (!RowData) { OutError = FString::Printf(TEXT("Row '%s' not found."), *RowName); return false; }
				const TSharedPtr<FJsonObject>* ValuesObj;
				if (!Arguments->TryGetObjectField(TEXT("values"), ValuesObj)) { OutError = TEXT("Missing values."); return false; }
				int32 Updated = 0;
				for (auto& Pair : (*ValuesObj)->Values)
				{
					const FString Key(*Pair.Key);
					FProperty* Prop = DT->RowStruct->FindPropertyByName(FName(*Key));
					if (Prop)
					{
						FString Val; Pair.Value->TryGetString(Val);
						Prop->ImportText_Direct(*Val, Prop->ContainerPtrToValuePtr<void>(RowData), nullptr, PPF_None);
						Updated++;
					}
				}
				if (Updated == 0)
				{
					OutError = TEXT("No DataTable row properties matched the provided values.");
					return false;
				}
				DT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("row_name"), RowName);
				OutStructured->SetNumberField(TEXT("properties_updated"), Updated);
				OutSummary = FString::Printf(TEXT("Updated %d properties on row '%s'"), Updated, *RowName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_delete_row ----
		Registry.Register({
			TEXT("datatable_delete_row"),
			TEXT("Delete a row from a DataTable by name."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("row_name"), FSololmcpSchemaBuilder::String(TEXT("Row name to delete"))}
				},
				{TEXT("asset_path"), TEXT("row_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				FString RowName = Arguments->GetStringField(TEXT("row_name"));
				if (!DT->FindRowUnchecked(FName(*RowName)))
				{
					OutError = FString::Printf(TEXT("Row '%s' not found."), *RowName);
					return false;
				}
				DT->RemoveRow(FName(*RowName));
				if (DT->FindRowUnchecked(FName(*RowName)))
				{
					OutError = FString::Printf(TEXT("Row '%s' still exists after RemoveRow."), *RowName);
					return false;
				}
				DT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("deleted_row"), RowName);
				OutSummary = FString::Printf(TEXT("Deleted row '%s'"), *RowName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_get_schema ----
		Registry.Register({
			TEXT("datatable_get_schema"),
			TEXT("Get the row struct schema (property names, types, defaults) for a DataTable."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				// Audit round 4: validate asset_path before LoadAsset (round 3 missed this entry).
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(_AP, OutError));
				if (!DT) { if (OutError.IsEmpty()) { OutError = TEXT("Failed to load datatable asset (path may need .ObjectName suffix or asset may not exist): ") + _AP; } return false; }
				if (!DT->RowStruct) { OutError = TEXT("No row struct."); return false; }
				TArray<TSharedPtr<FJsonValue>> PropArr;
				for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
				{
					FProperty* Prop = *It;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Prop->GetName());
					Obj->SetStringField(TEXT("type"), Prop->GetCPPType());
					Obj->SetNumberField(TEXT("offset"), Prop->GetOffset_ForInternal());
					Obj->SetNumberField(TEXT("size"), Prop->GetSize());
					PropArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetStringField(TEXT("row_struct"), DT->RowStruct->GetName());
				OutStructured->SetArrayField(TEXT("properties"), PropArr);
				OutStructured->SetNumberField(TEXT("count"), PropArr.Num());
				OutSummary = FString::Printf(TEXT("Schema %s: %d properties"), *DT->RowStruct->GetName(), PropArr.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_export_csv ----
		Registry.Register({
			TEXT("datatable_export_csv"),
			TEXT("Export a DataTable to CSV format string."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("include_header"), FSololmcpSchemaBuilder::Boolean(TEXT("Include header row (default: true)"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				if (!DT->RowStruct) { OutError = TEXT("No row struct."); return false; }

				bool bIncludeHeader = true;
				if (TSharedPtr<FJsonValue> Val = Arguments->TryGetField(TEXT("include_header")))
					bIncludeHeader = Val->AsBool();

				TArray<FString> Lines;
				TArray<FName> RowNames = DT->GetRowNames();

				// Build header
				if (bIncludeHeader)
				{
					TArray<FString> Headers;
					Headers.Add(TEXT("RowName"));
					for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
						Headers.Add(It->GetName());
					Lines.Add(FString::Join(Headers, TEXT(",")));
				}

				// Build rows
				for (const FName& RowName : RowNames)
				{
					uint8* RowData = DT->FindRowUnchecked(RowName);
					if (!RowData) continue;
					TArray<FString> Cells;
					Cells.Add(RowName.ToString());
					for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
					{
						FProperty* Prop = *It;
						FString ValStr;
						Prop->ExportTextItem_Direct(ValStr, Prop->ContainerPtrToValuePtr<void>(RowData), nullptr, nullptr, PPF_None);
						// Escape quotes and wrap in quotes if contains comma or quote
						if (ValStr.Contains(TEXT(",")) || ValStr.Contains(TEXT("\"")) || ValStr.Contains(TEXT("\n")))
						{
							ValStr = ValStr.Replace(TEXT("\""), TEXT("\"\""));
							ValStr = FString::Printf(TEXT("\"%s\""), *ValStr);
						}
						Cells.Add(ValStr);
					}
					Lines.Add(FString::Join(Cells, TEXT(",")));
				}

				FString CsvContent = FString::Join(Lines, TEXT("\n"));
				OutStructured->SetStringField(TEXT("csv"), CsvContent);
				OutStructured->SetNumberField(TEXT("row_count"), RowNames.Num());
				OutStructured->SetNumberField(TEXT("byte_count"), CsvContent.Len());
				OutSummary = FString::Printf(TEXT("Exported %d rows to CSV (%d bytes)"), RowNames.Num(), CsvContent.Len());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_import_csv ----
		Registry.Register({
			TEXT("datatable_import_csv"),
			TEXT("Import CSV data into a DataTable. Creates new rows or updates existing ones."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("csv_content"), FSololmcpSchemaBuilder::String(TEXT("CSV content string"))},
					{TEXT("has_header"), FSololmcpSchemaBuilder::Boolean(TEXT("First row is header (default: true)"))},
					{TEXT("merge_mode"), FSololmcpSchemaBuilder::String(TEXT("add, update, replace (default: add)"))}
				},
				{TEXT("asset_path"), TEXT("csv_content")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				if (!DT->RowStruct) { OutError = TEXT("No row struct."); return false; }

				FString CsvContent = Arguments->GetStringField(TEXT("csv_content"));
				bool bHasHeader = true;
				if (TSharedPtr<FJsonValue> Val = Arguments->TryGetField(TEXT("has_header")))
					bHasHeader = Val->AsBool();

				FString MergeMode = TEXT("add");
				if (TSharedPtr<FJsonValue> Val = Arguments->TryGetField(TEXT("merge_mode")))
					MergeMode = Val->AsString();

				// Parse CSV
				TArray<FString> Lines;
				CsvContent.ParseIntoArrayLines(Lines);

				if (Lines.Num() == 0) { OutError = TEXT("Empty CSV."); return false; }

				// Build property map from struct
				TMap<FString, FProperty*> PropMap;
				for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
					PropMap.Add(It->GetName().ToLower(), *It);

				TArray<FString> Headers;
				int32 StartLine = 0;

				if (bHasHeader)
				{
					// Parse header line
					FString& HeaderLine = Lines[0];
					TArray<FString> RawHeaders;
					HeaderLine.ParseIntoArray(RawHeaders, TEXT(","), true);
					for (const FString& H : RawHeaders)
						Headers.Add(H.TrimStartAndEnd().ToLower());
					StartLine = 1;
				}

				int32 AddedCount = 0, UpdatedCount = 0, SkippedCount = 0;
				if (MergeMode == TEXT("replace"))
				{
					DT->EmptyTable();
				}

				for (int32 i = StartLine; i < Lines.Num(); ++i)
				{
					FString Line = Lines[i].TrimStartAndEnd();
					if (Line.IsEmpty()) continue;

					// Simple CSV parsing (doesn't handle quoted cells with commas)
					TArray<FString> Cells;
					Line.ParseIntoArray(Cells, TEXT(","), true);

					if (Cells.Num() == 0) continue;

					FString RowName = Cells[0].TrimStartAndEnd();
					if (RowName.IsEmpty()) { SkippedCount++; continue; }

					bool bExists = DT->FindRowUnchecked(FName(*RowName)) != nullptr;

					if (bExists && MergeMode == TEXT("add"))
					{
						SkippedCount++;
						continue;
					}
					if (!bExists && MergeMode == TEXT("update"))
					{
						SkippedCount++;
						continue;
					}

					// Create row data
					FStructOnScope RowScope(DT->RowStruct);
					DT->RowStruct->InitializeDefaultValue(RowScope.GetStructMemory());

					// If updating, copy existing values first
					if (bExists)
					{
						uint8* ExistingRow = DT->FindRowUnchecked(FName(*RowName));
						if (ExistingRow)
							FMemory::Memcpy(RowScope.GetStructMemory(), ExistingRow, DT->RowStruct->GetStructureSize());
					}

					// Apply values from CSV
					for (int32 j = 1; j < Cells.Num(); ++j)
					{
						FString PropName;
						if (bHasHeader && j - 1 < Headers.Num())
							PropName = Headers[j - 1];
						else if (j - 1 < PropMap.Num())
						{
							// Use property index as fallback
							int32 PropIdx = 0;
							for (auto& Pair : PropMap)
							{
								if (PropIdx == j - 1) { PropName = Pair.Key; break; }
								PropIdx++;
							}
						}
						if (PropName.IsEmpty()) continue;

						FProperty** PropPtr = PropMap.Find(PropName);
						if (!PropPtr) continue;

						FProperty* Prop = *PropPtr;
						FString Val = Cells[j].TrimStartAndEnd();
						// Remove surrounding quotes if present
						if (Val.StartsWith(TEXT("\"")) && Val.EndsWith(TEXT("\"")) && Val.Len() >= 2)
							Val = Val.Mid(1, Val.Len() - 2);
						Val = Val.Replace(TEXT("\"\""), TEXT("\""));

						Prop->ImportText_Direct(*Val, Prop->ContainerPtrToValuePtr<void>(RowScope.GetStructMemory()), nullptr, PPF_None);
					}

					SomolAddDataTableRow(DT, FName(*RowName), RowScope.GetStructMemory());

					if (bExists) UpdatedCount++;
					else AddedCount++;
				}
				if (AddedCount + UpdatedCount == 0)
				{
					OutError = TEXT("CSV import did not add or update any rows.");
					return false;
				}

				DT->MarkPackageDirty();
				OutStructured->SetNumberField(TEXT("added"), AddedCount);
				OutStructured->SetNumberField(TEXT("updated"), UpdatedCount);
				OutStructured->SetNumberField(TEXT("skipped"), SkippedCount);
				OutSummary = FString::Printf(TEXT("Imported CSV: %d added, %d updated, %d skipped"), AddedCount, UpdatedCount, SkippedCount);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_export_json ----
		Registry.Register({
			TEXT("datatable_export_json"),
			TEXT("Export a DataTable to JSON format."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("pretty_print"), FSololmcpSchemaBuilder::Boolean(TEXT("Format with indentation (default: false)"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				if (!DT->RowStruct) { OutError = TEXT("No row struct."); return false; }

				bool bPrettyPrint = false;
				if (TSharedPtr<FJsonValue> Val = Arguments->TryGetField(TEXT("pretty_print")))
					bPrettyPrint = Val->AsBool();

				TSharedRef<FJsonObject> RootObj = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> RowsArr;
				TArray<FName> RowNames = DT->GetRowNames();

				for (const FName& RowName : RowNames)
				{
					uint8* RowData = DT->FindRowUnchecked(RowName);
					if (!RowData) continue;

					TSharedRef<FJsonObject> RowObj = MakeShared<FJsonObject>();
					RowObj->SetStringField(TEXT("_row_name"), RowName.ToString());

					for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
					{
						FProperty* Prop = *It;
						FString ValStr;
						Prop->ExportTextItem_Direct(ValStr, Prop->ContainerPtrToValuePtr<void>(RowData), nullptr, nullptr, PPF_None);

						// Try to parse as number or bool
						if (ValStr.IsNumeric())
						{
							if (ValStr.Contains(TEXT(".")))
								RowObj->SetNumberField(Prop->GetName(), FCString::Atof(*ValStr));
							else
								RowObj->SetNumberField(Prop->GetName(), FCString::Atoi(*ValStr));
						}
						else if (ValStr.ToLower() == TEXT("true") || ValStr.ToLower() == TEXT("false"))
						{
							RowObj->SetBoolField(Prop->GetName(), ValStr.ToLower() == TEXT("true"));
						}
						else
						{
							RowObj->SetStringField(Prop->GetName(), ValStr);
						}
					}
					RowsArr.Add(MakeShared<FJsonValueObject>(RowObj));
				}

				RootObj->SetStringField(TEXT("row_struct"), DT->RowStruct->GetName());
				RootObj->SetArrayField(TEXT("rows"), RowsArr);
				RootObj->SetNumberField(TEXT("count"), RowsArr.Num());

				FString JsonStr;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
				FJsonSerializer::Serialize(RootObj, Writer);

				OutStructured->SetStringField(TEXT("json"), JsonStr);
				OutStructured->SetNumberField(TEXT("row_count"), RowsArr.Num());
				OutSummary = FString::Printf(TEXT("Exported %d rows to JSON (%d bytes)"), RowsArr.Num(), JsonStr.Len());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- datatable_import_json ----
		Registry.Register({
			TEXT("datatable_import_json"),
			TEXT("Import JSON data into a DataTable. Creates new rows or updates existing ones."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("DataTable asset path"))},
					{TEXT("json_content"), FSololmcpSchemaBuilder::String(TEXT("JSON content string"))},
					{TEXT("row_name_field"), FSololmcpSchemaBuilder::String(TEXT("Field name to use as row name (default: _row_name)"))},
					{TEXT("merge_mode"), FSololmcpSchemaBuilder::String(TEXT("add, update, replace (default: add)"))}
				},
				{TEXT("asset_path"), TEXT("json_content")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UDataTable* DT = Cast<UDataTable>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!DT) return false;
				if (!DT->RowStruct) { OutError = TEXT("No row struct."); return false; }

				FString JsonContent = Arguments->GetStringField(TEXT("json_content"));
				FString RowNameField = TEXT("_row_name");
				if (TSharedPtr<FJsonValue> Val = Arguments->TryGetField(TEXT("row_name_field")))
					RowNameField = Val->AsString();

				FString MergeMode = TEXT("add");
				if (TSharedPtr<FJsonValue> Val = Arguments->TryGetField(TEXT("merge_mode")))
					MergeMode = Val->AsString();

				// Parse JSON
				TSharedPtr<FJsonObject> RootObj;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
				if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
				{
					OutError = TEXT("Failed to parse JSON.");
					return false;
				}

				// Get rows array
				const TArray<TSharedPtr<FJsonValue>>* RowsArr = nullptr;
				if (RootObj->HasField(TEXT("rows")))
					RootObj->TryGetArrayField(TEXT("rows"), RowsArr);
				else if (RootObj->Values.Num() > 0)
				{
					// Try to treat the whole object as a single row or find nested array
					for (auto& Pair : RootObj->Values)
					{
						if (Pair.Value->Type == EJson::Array)
						{
							RowsArr = &Pair.Value->AsArray();
							break;
						}
					}
				}

				if (!RowsArr || RowsArr->Num() == 0) { OutError = TEXT("No rows found in JSON."); return false; }

				// Build property map
				TMap<FString, FProperty*> PropMap;
				for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
					PropMap.Add(It->GetName().ToLower(), *It);

				int32 AddedCount = 0, UpdatedCount = 0, SkippedCount = 0;
				if (MergeMode == TEXT("replace"))
				{
					DT->EmptyTable();
				}

				for (const TSharedPtr<FJsonValue>& RowVal : *RowsArr)
				{
					if (RowVal->Type != EJson::Object) { SkippedCount++; continue; }
					TSharedPtr<FJsonObject> RowObj = RowVal->AsObject();

					// Get row name
					FString RowName;
					if (!RowObj->TryGetStringField(RowNameField, RowName))
					{
						SkippedCount++;
						continue;
					}

					bool bExists = DT->FindRowUnchecked(FName(*RowName)) != nullptr;

					if (bExists && MergeMode == TEXT("add")) { SkippedCount++; continue; }
					if (!bExists && MergeMode == TEXT("update")) { SkippedCount++; continue; }

					// Create row data
					FStructOnScope RowScope(DT->RowStruct);
					DT->RowStruct->InitializeDefaultValue(RowScope.GetStructMemory());

					if (bExists)
					{
						uint8* ExistingRow = DT->FindRowUnchecked(FName(*RowName));
						if (ExistingRow)
							FMemory::Memcpy(RowScope.GetStructMemory(), ExistingRow, DT->RowStruct->GetStructureSize());
					}

					// Apply values from JSON
					for (auto& Pair : RowObj->Values)
					{
						const FString Key(*Pair.Key);
						if (Key == RowNameField) continue;

						FProperty** PropPtr = PropMap.Find(Key.ToLower());
						if (!PropPtr) continue;

						FProperty* Prop = *PropPtr;
						FString ValStr;

						switch (Pair.Value->Type)
						{
							case EJson::String: ValStr = Pair.Value->AsString(); break;
							case EJson::Number: ValStr = FString::SanitizeFloat(Pair.Value->AsNumber()); break;
							case EJson::Boolean: ValStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false"); break;
							default: continue;
						}

						Prop->ImportText_Direct(*ValStr, Prop->ContainerPtrToValuePtr<void>(RowScope.GetStructMemory()), nullptr, PPF_None);
					}

					SomolAddDataTableRow(DT, FName(*RowName), RowScope.GetStructMemory());

					if (bExists) UpdatedCount++;
					else AddedCount++;
				}
				if (AddedCount + UpdatedCount == 0)
				{
					OutError = TEXT("JSON import did not add or update any rows.");
					return false;
				}

				DT->MarkPackageDirty();
				OutStructured->SetNumberField(TEXT("added"), AddedCount);
				OutStructured->SetNumberField(TEXT("updated"), UpdatedCount);
				OutStructured->SetNumberField(TEXT("skipped"), SkippedCount);
				OutSummary = FString::Printf(TEXT("Imported JSON: %d added, %d updated, %d skipped"), AddedCount, UpdatedCount, SkippedCount);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- curvetable_read ----
		Registry.Register({
			TEXT("curvetable_read"),
			TEXT("Read curve data from a CurveTable asset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("CurveTable asset path"))},
					{TEXT("row_name"), FSololmcpSchemaBuilder::String(TEXT("Optional: specific row to read"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				// Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), _AP) || _AP.IsEmpty() || !_AP.StartsWith(TEXT("/")))
				{
					OutError = TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");
					return false;
				}
				UCurveTable* CT = Cast<UCurveTable>(Context.Services.LoadAsset(_AP, OutError));
				if (!CT) { if (OutError.IsEmpty()) { OutError = TEXT("Failed to load curve table asset (path may need .ObjectName suffix or asset may not exist): ") + _AP; } return false; }
				// UE 5.7: GetRowNames() removed - use GetRowMap().GetKeys()
				TArray<FName> RowNames;
				CT->GetRowMap().GetKeys(RowNames);
				TArray<TSharedPtr<FJsonValue>> RowArr;
				FString TargetRow;
				Arguments->TryGetStringField(TEXT("row_name"), TargetRow);
				for (const FName& Name : RowNames)
				{
					if (!TargetRow.IsEmpty() && Name.ToString() != TargetRow) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Name.ToString());
					RowArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("rows"), RowArr);
				OutStructured->SetNumberField(TEXT("count"), RowArr.Num());
				OutSummary = FString::Printf(TEXT("CurveTable has %d rows"), RowArr.Num());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- input_action_create ----
		Registry.Register({
			TEXT("input_action_create"),
			TEXT("Create an Enhanced Input Action asset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path e.g. /Game/Input/IA_Jump"))},
					{TEXT("value_type"), FSololmcpSchemaBuilder::String(TEXT("Digital, Axis1D, Axis2D, Axis3D (default: Digital)"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
				FString ValueType = Arguments->HasField(TEXT("value_type")) ? Arguments->GetStringField(TEXT("value_type")) : TEXT("Digital");
				FString AssetName = FPackageName::GetShortName(AssetPath);
				UPackage* Package = CreatePackage(*AssetPath);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Package, *AssetName)) { if (!Ex->IsA<UInputAction>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UInputAction"), *AssetPath, *Ex->GetClass()->GetName()); return false; } }
				UInputAction* IA = NewObject<UInputAction>(Package, *AssetName, RF_Public | RF_Standalone);
				if (ValueType.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase)) IA->ValueType = EInputActionValueType::Axis1D;
				else if (ValueType.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase)) IA->ValueType = EInputActionValueType::Axis2D;
				else if (ValueType.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase)) IA->ValueType = EInputActionValueType::Axis3D;
				else IA->ValueType = EInputActionValueType::Boolean;
				FAssetRegistryModule::AssetCreated(IA);
				IA->MarkPackageDirty();
				if (!Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				if (!Context.Services.LoadAsset(AssetPath, OutError))
				{
					OutError = FString::Printf(TEXT("InputAction was created but failed reload verification: %s"), *AssetPath);
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutStructured->SetStringField(TEXT("value_type"), ValueType);
				OutSummary = FString::Printf(TEXT("Created InputAction %s (%s)"), *AssetName, *ValueType);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- input_action_list ----
		Registry.Register({
			TEXT("input_action_list"),
			TEXT("List all InputAction assets in the project."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				TArray<FAssetData> Assets;
				ARM.Get().GetAssetsByClass(UInputAction::StaticClass()->GetClassPathName(), Assets, true);
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FAssetData& A : Assets)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
					Obj->SetStringField(TEXT("path"), A.PackageName.ToString());
					Arr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("actions"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("Found %d InputAction assets"), Arr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- input_mapping_context_create ----
		Registry.Register({
			TEXT("input_mapping_context_create"),
			TEXT("Create an Enhanced Input Mapping Context asset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path e.g. /Game/Input/IMC_Default"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
				FString AssetName = FPackageName::GetShortName(AssetPath);
				UPackage* Package = CreatePackage(*AssetPath);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Package, *AssetName)) { if (!Ex->IsA<UInputMappingContext>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UInputMappingContext"), *AssetPath, *Ex->GetClass()->GetName()); return false; } }
				UInputMappingContext* IMC = NewObject<UInputMappingContext>(Package, *AssetName, RF_Public | RF_Standalone);
				FAssetRegistryModule::AssetCreated(IMC);
				IMC->MarkPackageDirty();
				if (!Context.Services.SaveAsset(AssetPath, false, OutError))
				{
					return false;
				}
				if (!Context.Services.LoadAsset(AssetPath, OutError))
				{
					OutError = FString::Printf(TEXT("InputMappingContext was created but failed reload verification: %s"), *AssetPath);
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("reload_verified"), true);
				OutSummary = FString::Printf(TEXT("Created InputMappingContext %s"), *AssetName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- input_mapping_context_add_mapping ----
		Registry.Register({
			TEXT("input_mapping_context_add_mapping"),
			TEXT("Add a key mapping to an InputMappingContext, binding a key to an InputAction."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("context_path"), FSololmcpSchemaBuilder::String(TEXT("IMC asset path"))},
					{TEXT("action_path"), FSololmcpSchemaBuilder::String(TEXT("InputAction asset path"))},
					{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Key name e.g. SpaceBar, W, LeftMouseButton, Gamepad_FaceButton_Bottom"))}
				},
				{TEXT("context_path"), TEXT("action_path"), TEXT("key")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UInputMappingContext* IMC = Cast<UInputMappingContext>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("context_path")), OutError));
				if (!IMC) return false;
				UInputAction* IA = Cast<UInputAction>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("action_path")), OutError));
				if (!IA) return false;
				FString KeyName = Arguments->GetStringField(TEXT("key"));
				FKey Key(*KeyName);
				if (!Key.IsValid()) { OutError = FString::Printf(TEXT("Invalid key name '%s'."), *KeyName); return false; }
				FEnhancedActionKeyMapping& Mapping = IMC->MapKey(IA, Key);
				IMC->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("action"), IA->GetName());
				OutStructured->SetStringField(TEXT("key"), KeyName);
				OutSummary = FString::Printf(TEXT("Mapped %s to %s"), *KeyName, *IA->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- input_mapping_context_list ----
		Registry.Register({
			TEXT("input_mapping_context_list"),
			TEXT("List all InputMappingContext assets in the project."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				TArray<FAssetData> Assets;
				ARM.Get().GetAssetsByClass(UInputMappingContext::StaticClass()->GetClassPathName(), Assets, true);
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FAssetData& A : Assets)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), A.AssetName.ToString());
					Obj->SetStringField(TEXT("path"), A.PackageName.ToString());
					Arr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("contexts"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("Found %d IMC assets"), Arr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- input_mapping_context_get_mappings ----
		Registry.Register({
			TEXT("input_mapping_context_get_mappings"),
			TEXT("Read all key mappings from an InputMappingContext."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("context_path"), FSololmcpSchemaBuilder::String(TEXT("IMC asset path"))}
				},
				{TEXT("context_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UInputMappingContext* IMC = Cast<UInputMappingContext>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("context_path")), OutError));
				if (!IMC) return false;
				TArray<FEnhancedActionKeyMapping> Mappings = IMC->GetMappings();
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FEnhancedActionKeyMapping& M : Mappings)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("action"), M.Action ? M.Action->GetName() : TEXT("None"));
					Obj->SetStringField(TEXT("key"), M.Key.GetFName().ToString());
					Obj->SetNumberField(TEXT("modifier_count"), M.Modifiers.Num());
					Obj->SetNumberField(TEXT("trigger_count"), M.Triggers.Num());
					Arr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("mappings"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("IMC has %d mappings"), Arr.Num());
				return true;
			}
		, nullptr
		, 5
		});
		// ---- collision_channels_list ----
		Registry.Register({
			TEXT("collision_channels_list"),
			TEXT("List all collision channels (trace and object) configured in the project."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<TSharedPtr<FJsonValue>> ChannelArr;
				for (int32 i = 0; i < ECC_MAX; i++)
				{
					ECollisionChannel Channel = (ECollisionChannel)i;
					FName ChannelName = UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(i);
					if (ChannelName == NAME_None) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), ChannelName.ToString());
					Obj->SetNumberField(TEXT("index"), i);
					Obj->SetStringField(TEXT("type"), (i < 3) ? TEXT("built-in") : (i < 8) ? TEXT("engine") : TEXT("custom"));
					ChannelArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("channels"), ChannelArr);
				OutStructured->SetNumberField(TEXT("count"), ChannelArr.Num());
				OutSummary = FString::Printf(TEXT("Found %d collision channels"), ChannelArr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- collision_presets_list ----
		Registry.Register({
			TEXT("collision_presets_list"),
			TEXT("List all collision presets/profiles."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<TSharedPtr<FJsonValue>> PresetArr;
				const UCollisionProfile* Profile = UCollisionProfile::Get();
				TArray<TSharedPtr<FName>> ProfileNames;
				Profile->GetProfileNames(ProfileNames);
				for (const TSharedPtr<FName>& Name : ProfileNames)
				{
					if (!Name.IsValid()) continue;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Name->ToString());
					PresetArr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("presets"), PresetArr);
				OutStructured->SetNumberField(TEXT("count"), PresetArr.Num());
				OutSummary = FString::Printf(TEXT("Found %d collision presets"), PresetArr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- collision_channel_create ----
		Registry.Register({
			TEXT("collision_channel_create"),
			TEXT("Create a custom collision channel by writing to DefaultEngine.ini. Requires editor restart."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Channel display name"))},
					{TEXT("default_response"), FSololmcpSchemaBuilder::String(TEXT("Block, Overlap, or Ignore (default: Block)"))}
				},
				{TEXT("name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ChannelName = Arguments->GetStringField(TEXT("name"));
				FString DefaultResp = Arguments->HasField(TEXT("default_response")) ? Arguments->GetStringField(TEXT("default_response")) : TEXT("Block");
				int32 SlotIndex = -1;
				for (int32 i = ECC_GameTraceChannel1; i <= ECC_GameTraceChannel18; i++)
				{
					FName Existing = UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(i);
					if (Existing == NAME_None || Existing.ToString().StartsWith(TEXT("ECC_GameTraceChannel")))
					{
						SlotIndex = i;
						break;
					}
				}
				if (SlotIndex < 0) { OutError = TEXT("No available custom collision channel slot."); return false; }
				int32 GameIndex = SlotIndex - ECC_GameTraceChannel1;
				FString Section = TEXT("/Script/Engine.CollisionProfile");
				FString Entry = FString::Printf(TEXT("+DefaultChannelResponses=(Channel=\"ECC_GameTraceChannel%d\",DefaultResponse=%s,bTraceType=False,bStaticObject=False,Name=\"%s\")"), GameIndex + 1, *DefaultResp, *ChannelName);
				GConfig->SetString(*Section, TEXT("DefaultChannelResponses"), *Entry, GEngineIni);
				GConfig->Flush(false, GEngineIni);
				OutStructured->SetStringField(TEXT("name"), ChannelName);
				OutStructured->SetNumberField(TEXT("channel_index"), SlotIndex);
				OutStructured->SetBoolField(TEXT("restart_needed"), true);
				OutSummary = FString::Printf(TEXT("Created collision channel '%s' at slot %d (restart needed)"), *ChannelName, SlotIndex);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- collision_preset_configure ----
		Registry.Register({
			TEXT("collision_preset_configure"),
			TEXT("Set collision response for an actor's root component to a named preset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label or name"))},
					{TEXT("preset"), FSololmcpSchemaBuilder::String(TEXT("Collision preset name (e.g. BlockAll, OverlapAll, NoCollision, Pawn)"))}
				},
				{TEXT("actor"), TEXT("preset")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString ActorId = Arguments->GetStringField(TEXT("actor"));
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!Actor) return false;
				FString Preset = Arguments->GetStringField(TEXT("preset"));
				UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
				if (!Root) { OutError = TEXT("Actor has no primitive root component."); return false; }
				Root->SetCollisionProfileName(FName(*Preset));
				if (Root->GetCollisionProfileName() != FName(*Preset))
				{
					OutError = FString::Printf(TEXT("Collision preset '%s' failed immediate readback."), *Preset);
					return false;
				}
				Actor->MarkPackageDirty();
				OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
				OutStructured->SetStringField(TEXT("collision_preset"), Preset);
				OutSummary = FString::Printf(TEXT("Set collision preset '%s' on %s"), *Preset, *Actor->GetActorLabel());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- gamemode_get_current ----
		Registry.Register({
			TEXT("gamemode_get_current"),
			TEXT("Get the current map's default GameMode and its configured classes (PlayerController, Pawn, HUD, etc)."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No editor world."); return false; }
				AWorldSettings* WS = World->GetWorldSettings();
				if (!WS) { OutError = TEXT("No world settings."); return false; }
				TSubclassOf<AGameModeBase> GMClass = WS->DefaultGameMode;
				OutStructured->SetStringField(TEXT("game_mode_class"), GMClass ? GMClass->GetPathName() : TEXT("None"));
				if (GMClass)
				{
					AGameModeBase* CDO = GMClass->GetDefaultObject<AGameModeBase>();
					if (CDO)
					{
						OutStructured->SetStringField(TEXT("default_pawn_class"), CDO->DefaultPawnClass ? CDO->DefaultPawnClass->GetName() : TEXT("None"));
						OutStructured->SetStringField(TEXT("player_controller_class"), CDO->PlayerControllerClass ? CDO->PlayerControllerClass->GetName() : TEXT("None"));
						OutStructured->SetStringField(TEXT("hud_class"), CDO->HUDClass ? CDO->HUDClass->GetName() : TEXT("None"));
					}
				}
				OutSummary = FString::Printf(TEXT("GameMode: %s"), GMClass ? *GMClass->GetName() : TEXT("None"));
				return true;
		}
		, nullptr
		, 0
		});

		// ---- gamemode_get_classes ----
		Registry.Register({
			TEXT("gamemode_get_classes"),
			TEXT("List all GameMode-derived classes available in the project."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<UClass*> Derived;
				GetDerivedClasses(AGameModeBase::StaticClass(), Derived, true);
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (UClass* Cls : Derived)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Cls->GetName());
					Obj->SetStringField(TEXT("path"), Cls->GetPathName());
					Obj->SetBoolField(TEXT("is_abstract"), Cls->HasAnyClassFlags(CLASS_Abstract));
					Arr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("game_modes"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("Found %d GameMode classes"), Arr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- world_settings_read ----
		Registry.Register({
			TEXT("world_settings_read"),
			TEXT("Read WorldSettings for the current editor world (game mode, kill Z, streaming, etc)."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No editor world."); return false; }
				AWorldSettings* WS = World->GetWorldSettings();
				if (!WS) { OutError = TEXT("No world settings."); return false; }
				OutStructured->SetStringField(TEXT("default_game_mode"), WS->DefaultGameMode ? WS->DefaultGameMode->GetName() : TEXT("None"));
				OutStructured->SetNumberField(TEXT("kill_z"), WS->KillZ);
				OutStructured->SetBoolField(TEXT("enable_world_bounds_checks"), WS->bEnableWorldBoundsChecks);
				OutStructured->SetBoolField(TEXT("enable_navigation_system"), WS->IsNavigationSystemEnabled());
				OutStructured->SetStringField(TEXT("level_name"), World->GetMapName());
				OutSummary = FString::Printf(TEXT("WorldSettings for %s"), *World->GetMapName());
				return true;
		}
		, nullptr
		, 0
		});

		// ============================================================
		// v1.7.0 Tier 2 Batch 2A — AI / Behavior Tree (12 tools)
		// ============================================================

		// ---- behaviortree_create ----
		Registry.Register({
			TEXT("behaviortree_create"),
			TEXT("Create a new Behavior Tree asset with optional Blackboard reference."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path e.g. /Game/AI/BT_Enemy"))},
					{TEXT("blackboard_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Blackboard asset path to link"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
				FString AssetName = FPackageName::GetShortName(AssetPath);
				UPackage* Package = CreatePackage(*AssetPath);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Package, *AssetName)) { if (!Ex->IsA<UBehaviorTree>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UBehaviorTree"), *AssetPath, *Ex->GetClass()->GetName()); return false; } }
				UBehaviorTree* BT = NewObject<UBehaviorTree>(Package, *AssetName, RF_Public | RF_Standalone);
				if (!BT) { OutError = TEXT("Failed to create BehaviorTree."); return false; }
				if (Arguments->HasField(TEXT("blackboard_path")))
				{
					UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *Arguments->GetStringField(TEXT("blackboard_path")));
					if (BB) BT->BlackboardAsset = BB;
				}
				FAssetRegistryModule::AssetCreated(BT);
				BT->MarkPackageDirty();
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(BT->GetPathName(), false, SaveErr);
				(void)bSaved;
				if (!VerifyCreatedAssetReloaded(Context.Services, BT, UBehaviorTree::StaticClass(), OutStructured, OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), BT->GetPathName());
				OutStructured->SetBoolField(TEXT("has_blackboard"), BT->BlackboardAsset != nullptr);
				OutSummary = FString::Printf(TEXT("Created BehaviorTree '%s'"), *AssetName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- behaviortree_list_nodes ----
		Registry.Register({
			TEXT("behaviortree_list_nodes"),
			TEXT("List all nodes in a Behavior Tree compiled structure (composites, tasks, decorators, services)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("BehaviorTree asset path"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBehaviorTree* BT = Cast<UBehaviorTree>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!BT) return false;
				TArray<TSharedPtr<FJsonValue>> Nodes;
				TFunction<void(UBTCompositeNode*, int32)> Traverse = [&](UBTCompositeNode* Node, int32 Depth)
				{
					if (!Node) return;
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
					Obj->SetStringField(TEXT("name"), Node->GetNodeName());
					Obj->SetNumberField(TEXT("depth"), Depth);
					Obj->SetStringField(TEXT("type"), TEXT("Composite"));
					TArray<TSharedPtr<FJsonValue>> SvcArr;
					for (UBTService* Svc : Node->Services)
					{
						if (!Svc) continue;
						TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
						S->SetStringField(TEXT("class"), Svc->GetClass()->GetName());
						S->SetStringField(TEXT("name"), Svc->GetNodeName());
						SvcArr.Add(MakeShared<FJsonValueObject>(S));
					}
					if (SvcArr.Num() > 0) Obj->SetArrayField(TEXT("services"), SvcArr);
					Nodes.Add(MakeShared<FJsonValueObject>(Obj));
					for (int32 i = 0; i < Node->Children.Num(); i++)
					{
						const FBTCompositeChild& Child = Node->Children[i];
						for (UBTDecorator* Dec : Child.Decorators)
						{
							if (!Dec) continue;
							TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
							D->SetStringField(TEXT("class"), Dec->GetClass()->GetName());
							D->SetStringField(TEXT("name"), Dec->GetNodeName());
							D->SetNumberField(TEXT("depth"), Depth + 1);
							D->SetStringField(TEXT("type"), TEXT("Decorator"));
							Nodes.Add(MakeShared<FJsonValueObject>(D));
						}
						if (Child.ChildComposite) Traverse(Child.ChildComposite, Depth + 1);
						else if (Child.ChildTask)
						{
							TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
							T->SetStringField(TEXT("class"), Child.ChildTask->GetClass()->GetName());
							T->SetStringField(TEXT("name"), Child.ChildTask->GetNodeName());
							T->SetNumberField(TEXT("depth"), Depth + 1);
							T->SetStringField(TEXT("type"), TEXT("Task"));
							Nodes.Add(MakeShared<FJsonValueObject>(T));
						}
					}
				};
				Traverse(BT->RootNode, 0);
				OutStructured->SetArrayField(TEXT("nodes"), Nodes);
				OutStructured->SetNumberField(TEXT("total_nodes"), Nodes.Num());
				OutStructured->SetStringField(TEXT("blackboard"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : TEXT("None"));
				OutSummary = FString::Printf(TEXT("BT has %d nodes"), Nodes.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- behaviortree_add_task ----
		Registry.Register({
			TEXT("behaviortree_add_task"),
			TEXT("Add a task node to a Behavior Tree root composite by class name."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("BehaviorTree asset path"))},
					{TEXT("task_class"), FSololmcpSchemaBuilder::String(TEXT("UBTTaskNode subclass name e.g. BTTask_BlueprintBase"))}
				},
				{TEXT("asset_path"), TEXT("task_class")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBehaviorTree* BT = Cast<UBehaviorTree>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!BT) return false;
				FString TaskClassName = Arguments->GetStringField(TEXT("task_class"));
				UClass* TaskClass = FindObject<UClass>(nullptr, *TaskClassName);
				if (!TaskClass) TaskClass = FindObject<UClass>(nullptr, *(TEXT("BTTask_") + TaskClassName));
				if (!TaskClass || !TaskClass->IsChildOf(UBTTaskNode::StaticClass()))
				{
					OutError = FString::Printf(TEXT("Task class '%s' not found or not a BTTaskNode subclass."), *TaskClassName);
					return false;
				}
				if (!BT->RootNode)
					BT->RootNode = NewObject<UBTComposite_Sequence>(BT);
				const int32 BeforeChildren = BT->RootNode->Children.Num();
				UBTTaskNode* NewTask = NewObject<UBTTaskNode>(BT, TaskClass);
				FBTCompositeChild NewChild;
				NewChild.ChildTask = NewTask;
				BT->RootNode->Children.Add(NewChild);
				if (BT->RootNode->Children.Num() != BeforeChildren + 1 || BT->RootNode->Children.Last().ChildTask != NewTask)
				{
					OutError = TEXT("BehaviorTree task add did not survive immediate readback.");
					return false;
				}
				BT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("task_class"), TaskClass->GetName());
				OutStructured->SetNumberField(TEXT("child_index"), BT->RootNode->Children.Num() - 1);
				OutSummary = FString::Printf(TEXT("Added task '%s' to BT root"), *TaskClass->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- behaviortree_add_decorator ----
		Registry.Register({
			TEXT("behaviortree_add_decorator"),
			TEXT("Add a decorator to a child slot in a Behavior Tree root composite."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("BehaviorTree asset path"))},
					{TEXT("decorator_class"), FSololmcpSchemaBuilder::String(TEXT("UBTDecorator subclass name"))},
					{TEXT("child_index"), FSololmcpSchemaBuilder::Integer(TEXT("Child index to attach decorator to (default 0)"))}
				},
				{TEXT("asset_path"), TEXT("decorator_class")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBehaviorTree* BT = Cast<UBehaviorTree>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!BT || !BT->RootNode) { OutError = TEXT("BT has no root node."); return false; }
				FString ClassName = Arguments->GetStringField(TEXT("decorator_class"));
				UClass* DecClass = FindObject<UClass>(nullptr, *ClassName);
				if (!DecClass) DecClass = FindObject<UClass>(nullptr, *(TEXT("BTDecorator_") + ClassName));
				if (!DecClass || !DecClass->IsChildOf(UBTDecorator::StaticClass()))
				{
					OutError = FString::Printf(TEXT("Decorator class '%s' not found."), *ClassName);
					return false;
				}
				int32 ChildIdx = Arguments->HasField(TEXT("child_index")) ? static_cast<int32>(Arguments->GetNumberField(TEXT("child_index"))) : 0;
				if (!BT->RootNode->Children.IsValidIndex(ChildIdx))
				{
					OutError = FString::Printf(TEXT("Child index %d out of range."), ChildIdx);
					return false;
				}
				const int32 BeforeDecorators = BT->RootNode->Children[ChildIdx].Decorators.Num();
				UBTDecorator* Dec = NewObject<UBTDecorator>(BT, DecClass);
				BT->RootNode->Children[ChildIdx].Decorators.Add(Dec);
				if (BT->RootNode->Children[ChildIdx].Decorators.Num() != BeforeDecorators + 1 || BT->RootNode->Children[ChildIdx].Decorators.Last() != Dec)
				{
					OutError = TEXT("BehaviorTree decorator add did not survive immediate readback.");
					return false;
				}
				BT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("decorator_class"), DecClass->GetName());
				OutStructured->SetNumberField(TEXT("child_index"), ChildIdx);
				OutSummary = FString::Printf(TEXT("Added decorator '%s' at child %d"), *DecClass->GetName(), ChildIdx);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- behaviortree_add_service ----
		Registry.Register({
			TEXT("behaviortree_add_service"),
			TEXT("Add a service to the Behavior Tree root composite node."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("BehaviorTree asset path"))},
					{TEXT("service_class"), FSololmcpSchemaBuilder::String(TEXT("UBTService subclass name"))}
				},
				{TEXT("asset_path"), TEXT("service_class")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBehaviorTree* BT = Cast<UBehaviorTree>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!BT || !BT->RootNode) { OutError = TEXT("BT has no root node."); return false; }
				FString ClassName = Arguments->GetStringField(TEXT("service_class"));
				UClass* SvcClass = FindObject<UClass>(nullptr, *ClassName);
				if (!SvcClass) SvcClass = FindObject<UClass>(nullptr, *(TEXT("BTService_") + ClassName));
				if (!SvcClass || !SvcClass->IsChildOf(UBTService::StaticClass()))
				{
					OutError = FString::Printf(TEXT("Service class '%s' not found."), *ClassName);
					return false;
				}
				const int32 BeforeServices = BT->RootNode->Services.Num();
				UBTService* Svc = NewObject<UBTService>(BT, SvcClass);
				BT->RootNode->Services.Add(Svc);
				if (BT->RootNode->Services.Num() != BeforeServices + 1 || BT->RootNode->Services.Last() != Svc)
				{
					OutError = TEXT("BehaviorTree service add did not survive immediate readback.");
					return false;
				}
				BT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("service_class"), SvcClass->GetName());
				OutSummary = FString::Printf(TEXT("Added service '%s' to BT root"), *SvcClass->GetName());
				return true;
			}
		, nullptr
		, 5
		});

		// ---- behaviortree_connect_nodes ----
		Registry.Register({
			TEXT("behaviortree_connect_nodes"),
			TEXT("Set the Blackboard for a Behavior Tree and optionally change root composite type (Selector/Sequence)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("BehaviorTree asset path"))},
					{TEXT("blackboard_path"), FSololmcpSchemaBuilder::String(TEXT("Blackboard asset path to assign"))},
					{TEXT("root_type"), FSololmcpSchemaBuilder::String(TEXT("Root composite: Selector or Sequence"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBehaviorTree* BT = Cast<UBehaviorTree>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!BT) return false;
				if (Arguments->HasField(TEXT("blackboard_path")))
				{
					UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *Arguments->GetStringField(TEXT("blackboard_path")));
					if (!BB) { OutError = TEXT("Blackboard not found."); return false; }
					BT->BlackboardAsset = BB;
				}
				if (Arguments->HasField(TEXT("root_type")))
				{
					FString RootType = Arguments->GetStringField(TEXT("root_type"));
					if (RootType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
					{
						if (!BT->RootNode || !BT->RootNode->IsA<UBTComposite_Sequence>())
							BT->RootNode = NewObject<UBTComposite_Sequence>(BT);
					}
					else if (RootType.Equals(TEXT("Selector"), ESearchCase::IgnoreCase))
					{
						if (!BT->RootNode || !BT->RootNode->IsA<UBTComposite_Selector>())
							BT->RootNode = NewObject<UBTComposite_Selector>(BT);
					}
					else
					{
						OutError = TEXT("root_type must be Sequence or Selector.");
						return false;
					}
				}
				if (!Arguments->HasField(TEXT("blackboard_path")) && !Arguments->HasField(TEXT("root_type")))
				{
					OutError = TEXT("No BehaviorTree configuration fields were provided.");
					return false;
				}
				BT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("blackboard"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : TEXT("None"));
				OutStructured->SetStringField(TEXT("root_type"), BT->RootNode ? BT->RootNode->GetClass()->GetName() : TEXT("None"));
				OutSummary = TEXT("Updated BT configuration");
				return true;
			}
		, nullptr
		, 5
		});

		// ---- blackboard_create ----
		Registry.Register({
			TEXT("blackboard_create"),
			TEXT("Create a new Blackboard Data asset for Behavior Trees."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path e.g. /Game/AI/BB_Enemy"))},
					{TEXT("parent_path"), FSololmcpSchemaBuilder::String(TEXT("Optional parent Blackboard to inherit keys from"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
				FString AssetName = FPackageName::GetShortName(AssetPath);
				UPackage* Package = CreatePackage(*AssetPath);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Package, *AssetName)) { if (!Ex->IsA<UBlackboardData>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UBlackboardData"), *AssetPath, *Ex->GetClass()->GetName()); return false; } }
				UBlackboardData* BB = NewObject<UBlackboardData>(Package, *AssetName, RF_Public | RF_Standalone);
				if (!BB) { OutError = TEXT("Failed to create Blackboard."); return false; }
				if (Arguments->HasField(TEXT("parent_path")))
				{
					UBlackboardData* Parent = LoadObject<UBlackboardData>(nullptr, *Arguments->GetStringField(TEXT("parent_path")));
					if (Parent) BB->Parent = Parent;
				}
				FAssetRegistryModule::AssetCreated(BB);
				BB->MarkPackageDirty();
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(BB->GetPathName(), false, SaveErr);
				(void)bSaved;
				if (!VerifyCreatedAssetReloaded(Context.Services, BB, UBlackboardData::StaticClass(), OutStructured, OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), BB->GetPathName());
				OutSummary = FString::Printf(TEXT("Created Blackboard '%s'"), *AssetName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- blackboard_add_key ----
		Registry.Register({
			TEXT("blackboard_add_key"),
			TEXT("Add a key to a Blackboard Data asset. Types: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blackboard asset path"))},
					{TEXT("key_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the new key"))},
					{TEXT("key_type"), FSololmcpSchemaBuilder::String(TEXT("Key type: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum"))}
				},
				{TEXT("asset_path"), TEXT("key_name"), TEXT("key_type")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBlackboardData* BB = Cast<UBlackboardData>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!BB) return false;
				FString KeyName = Arguments->GetStringField(TEXT("key_name"));
				FString KeyType = Arguments->GetStringField(TEXT("key_type"));
				for (const FBlackboardEntry& Entry : BB->Keys)
				{
					if (Entry.EntryName == FName(*KeyName))
					{
						OutError = FString::Printf(TEXT("Key '%s' already exists."), *KeyName);
						return false;
					}
				}
				const FString LowerCase = KeyType.ToLower();
				UClass* KeyTypeClass = nullptr;
				if (LowerCase == TEXT("bool")) { KeyTypeClass = UBlackboardKeyType_Bool::StaticClass(); }
				else if (LowerCase == TEXT("int")) { KeyTypeClass = UBlackboardKeyType_Int::StaticClass(); }
				else if (LowerCase == TEXT("float")) { KeyTypeClass = UBlackboardKeyType_Float::StaticClass(); }
				else if (LowerCase == TEXT("string")) { KeyTypeClass = UBlackboardKeyType_String::StaticClass(); }
				else if (LowerCase == TEXT("name")) { KeyTypeClass = UBlackboardKeyType_Name::StaticClass(); }
				else if (LowerCase == TEXT("vector")) { KeyTypeClass = UBlackboardKeyType_Vector::StaticClass(); }
				else if (LowerCase == TEXT("rotator")) { KeyTypeClass = UBlackboardKeyType_Rotator::StaticClass(); }
				else if (LowerCase == TEXT("object")) { KeyTypeClass = UBlackboardKeyType_Object::StaticClass(); }
				else if (LowerCase == TEXT("class")) { KeyTypeClass = UBlackboardKeyType_Class::StaticClass(); }
				else if (LowerCase == TEXT("enum")) { KeyTypeClass = UBlackboardKeyType_Enum::StaticClass(); }
				if (!KeyTypeClass)
				{
					OutError = FString::Printf(TEXT("Unknown key type '%s'. Try one of: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum (case-insensitive)."), *KeyType);
					return false;
				}
				FBlackboardEntry NewEntry;
				NewEntry.EntryName = FName(*KeyName);
				NewEntry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyTypeClass);
				BB->Keys.Add(NewEntry);
				if (!BB->Keys.ContainsByPredicate([&KeyName, KeyTypeClass](const FBlackboardEntry& Entry)
					{
						return Entry.EntryName == FName(*KeyName) && Entry.KeyType && Entry.KeyType->IsA(KeyTypeClass);
					}))
				{
					OutError = TEXT("Blackboard key add did not survive immediate readback.");
					return false;
				}
				BB->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("key_name"), KeyName);
				OutStructured->SetStringField(TEXT("key_type"), KeyType);
				OutSummary = FString::Printf(TEXT("Added key '%s' (%s) to Blackboard"), *KeyName, *KeyType);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- blackboard_list_keys ----
		Registry.Register({
			TEXT("blackboard_list_keys"),
			TEXT("List all keys in a Blackboard Data asset including inherited keys."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blackboard asset path"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UBlackboardData* BB = Cast<UBlackboardData>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!BB) return false;
				TArray<TSharedPtr<FJsonValue>> Arr;
				const UBlackboardData* Current = BB;
				while (Current)
				{
					for (const FBlackboardEntry& Entry : Current->Keys)
					{
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
						Obj->SetStringField(TEXT("type"), Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("None"));
						Obj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced);
						Obj->SetBoolField(TEXT("inherited"), Current != BB);
						Arr.Add(MakeShared<FJsonValueObject>(Obj));
					}
					Current = Current->Parent;
				}
				OutStructured->SetArrayField(TEXT("keys"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("Blackboard has %d keys"), Arr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- ai_controller_list ----
		Registry.Register({
			TEXT("ai_controller_list"),
			TEXT("List all AIController-derived classes available in the project."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				TArray<UClass*> Derived;
				GetDerivedClasses(AAIController::StaticClass(), Derived, true);
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (UClass* Cls : Derived)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), Cls->GetName());
					Obj->SetStringField(TEXT("path"), Cls->GetPathName());
					Obj->SetBoolField(TEXT("is_blueprint"), Cls->HasAnyClassFlags(CLASS_CompiledFromBlueprint));
					Arr.Add(MakeShared<FJsonValueObject>(Obj));
				}
				OutStructured->SetArrayField(TEXT("ai_controllers"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("Found %d AIController classes"), Arr.Num());
				return true;
		}
		, nullptr
		, 0
		});

		// ---- eqs_create ----
		Registry.Register({
			TEXT("eqs_create"),
			TEXT("Create a new Environment Query System (EQS) asset."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path e.g. /Game/AI/EQS_FindCover"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
				FString AssetName = FPackageName::GetShortName(AssetPath);
				UPackage* Package = CreatePackage(*AssetPath);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Package, *AssetName)) { if (!Ex->IsA<UEnvQuery>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UEnvQuery"), *AssetPath, *Ex->GetClass()->GetName()); return false; } }
				UEnvQuery* EQ = NewObject<UEnvQuery>(Package, *AssetName, RF_Public | RF_Standalone);
				if (!EQ) { OutError = TEXT("Failed to create EQS query."); return false; }
				FAssetRegistryModule::AssetCreated(EQ);
				EQ->MarkPackageDirty();
				FString SaveErr;
				const bool bSaved = Context.Services.SaveAsset(EQ->GetPathName(), false, SaveErr);
				(void)bSaved;
				if (!VerifyCreatedAssetReloaded(Context.Services, EQ, UEnvQuery::StaticClass(), OutStructured, OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("asset_path"), EQ->GetPathName());
				OutSummary = FString::Printf(TEXT("Created EQS query '%s'"), *AssetName);
				return true;
			}
		, nullptr
		, 5
		});

		// ---- eqs_add_generator ----
		Registry.Register({
			TEXT("eqs_add_generator"),
			TEXT("Add a generator to an EQS query by class name, or list available generators if no class given."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("EQS query asset path"))},
					{TEXT("generator_class"), FSololmcpSchemaBuilder::String(TEXT("Generator class e.g. EnvQueryGenerator_SimpleGrid (omit to list)"))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!Arguments->HasField(TEXT("generator_class")))
				{
					TArray<UClass*> Derived;
					GetDerivedClasses(UEnvQueryGenerator::StaticClass(), Derived, true);
					TArray<TSharedPtr<FJsonValue>> Arr;
					for (UClass* Cls : Derived)
					{
						if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;
						TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
						Obj->SetStringField(TEXT("name"), Cls->GetName());
						Arr.Add(MakeShared<FJsonValueObject>(Obj));
					}
					OutStructured->SetArrayField(TEXT("generators"), Arr);
					OutSummary = FString::Printf(TEXT("Found %d EQS generators"), Arr.Num());
					return true;
				}
				UEnvQuery* EQ = Cast<UEnvQuery>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError));
				if (!EQ) return false;
				FString GenClassName = Arguments->GetStringField(TEXT("generator_class"));
				UClass* GenClass = FindObject<UClass>(nullptr, *GenClassName);
				if (!GenClass) GenClass = FindObject<UClass>(nullptr, *(TEXT("EnvQueryGenerator_") + GenClassName));
				if (!GenClass || !GenClass->IsChildOf(UEnvQueryGenerator::StaticClass()))
				{
					OutError = FString::Printf(TEXT("Generator class '%s' not found."), *GenClassName);
					return false;
				}
				SololmcpError::Set(OutStructured, TEXT("NOT_IMPLEMENTED"), TEXT("generator_class"), TEXT("UE 5.7 hides UEnvQuery::Options from this editor tool; refusing to report a generator add that cannot be attached/read back."));
				OutStructured->SetStringField(TEXT("generator_class"), GenClass->GetName());
				OutError = TEXT("eqs_add_generator is unavailable on this UE version because UEnvQuery::Options is not writable through the current API.");
				return false;
			}
		, nullptr
		, 5
		});

		// ============================================================
		// v1.7.0 Tier 2 Batch 2B — Navigation (5 tools)
		// ============================================================

		Registry.Register({ TEXT("navmesh_get_settings"), TEXT("Read NavMesh settings: cell size, agent radius/height."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No editor world."); return false; }
				ARecastNavMesh* NM = nullptr;
				for (TActorIterator<ARecastNavMesh> It(World); It; ++It) { NM = *It; break; }
				if (!NM) { OutError = TEXT("No RecastNavMesh."); return false; }
				OutStructured->SetStringField(TEXT("cell_size_status"), TEXT("unavailable_in_current_ue_api"));
				OutStructured->SetStringField(TEXT("cell_height_status"), TEXT("unavailable_in_current_ue_api"));
				OutStructured->SetNumberField(TEXT("agent_radius"), NM->AgentRadius);
				OutStructured->SetNumberField(TEXT("agent_height"), NM->AgentHeight);
				OutStructured->SetNumberField(TEXT("agent_max_slope"), NM->AgentMaxSlope);
				OutStructured->SetNumberField(TEXT("agent_max_step_height"), NM->GetAgentMaxStepHeight(ENavigationDataResolution::Default));
				OutStructured->SetNumberField(TEXT("tile_size_uu"), NM->TileSizeUU);
				OutSummary = FString::Printf(TEXT("NavMesh: Agent=%.1f"), NM->AgentRadius);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("navmesh_set_settings"), TEXT("Modify NavMesh settings. Only provided fields changed."),
			FSololmcpSchemaBuilder::Object({{TEXT("cell_size"), FSololmcpSchemaBuilder::Number(TEXT("Cell XY"))},{TEXT("cell_height"), FSololmcpSchemaBuilder::Number(TEXT("Cell H"))},{TEXT("agent_radius"), FSololmcpSchemaBuilder::Number(TEXT("Radius"))},{TEXT("agent_height"), FSololmcpSchemaBuilder::Number(TEXT("Height"))},{TEXT("agent_max_slope"), FSololmcpSchemaBuilder::Number(TEXT("Slope deg"))},{TEXT("agent_max_step_height"), FSololmcpSchemaBuilder::Number(TEXT("Step H"))}}, {}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No world."); return false; }
				ARecastNavMesh* NM = nullptr;
				for (TActorIterator<ARecastNavMesh> It(World); It; ++It) { NM = *It; break; }
				if (!NM) { OutError = TEXT("No RecastNavMesh."); return false; }
				if (Arguments->HasField(TEXT("cell_size")) || Arguments->HasField(TEXT("cell_height")))
				{
					SololmcpError::Set(OutStructured, TEXT("NOT_IMPLEMENTED"), TEXT("cell_size"), TEXT("cell_size/cell_height are not writable through this UE version's ARecastNavMesh API; refusing partial success."));
					OutError = TEXT("cell_size/cell_height are unsupported by navmesh_set_settings in this UE version.");
					return false;
				}
				int32 C = 0;
				// UE 5.7: CellSizeXY removed
			// if (Arguments->HasField(TEXT("cell_size"))) { NM->CellSizeXY = ...; C++; }
				// UE 5.7: CellHeightXY removed
			// if (Arguments->HasField(TEXT("cell_height"))) { NM->CellHeightXY = ...; C++; }
				if (Arguments->HasField(TEXT("agent_radius"))) { NM->AgentRadius = Arguments->GetNumberField(TEXT("agent_radius")); C++; }
				if (Arguments->HasField(TEXT("agent_height"))) { NM->AgentHeight = Arguments->GetNumberField(TEXT("agent_height")); C++; }
				if (Arguments->HasField(TEXT("agent_max_slope"))) { NM->AgentMaxSlope = Arguments->GetNumberField(TEXT("agent_max_slope")); C++; }
				if (Arguments->HasField(TEXT("agent_max_step_height"))) { NM->SetAgentMaxStepHeight(ENavigationDataResolution::Default, static_cast<float>(Arguments->GetNumberField(TEXT("agent_max_step_height")))); C++; }
				if (C == 0)
				{
					OutError = TEXT("No supported NavMesh settings were provided.");
					return false;
				}
				if ((Arguments->HasField(TEXT("agent_radius")) && !FMath::IsNearlyEqual(NM->AgentRadius, static_cast<float>(Arguments->GetNumberField(TEXT("agent_radius"))), 0.01f))
					|| (Arguments->HasField(TEXT("agent_height")) && !FMath::IsNearlyEqual(NM->AgentHeight, static_cast<float>(Arguments->GetNumberField(TEXT("agent_height"))), 0.01f))
					|| (Arguments->HasField(TEXT("agent_max_slope")) && !FMath::IsNearlyEqual(NM->AgentMaxSlope, static_cast<float>(Arguments->GetNumberField(TEXT("agent_max_slope"))), 0.01f))
					|| (Arguments->HasField(TEXT("agent_max_step_height")) && !FMath::IsNearlyEqual(NM->GetAgentMaxStepHeight(ENavigationDataResolution::Default), static_cast<float>(Arguments->GetNumberField(TEXT("agent_max_step_height"))), 0.01f)))
				{
					OutError = TEXT("NavMesh setting write failed immediate readback.");
					return false;
				}
				NM->MarkPackageDirty();
				OutStructured->SetNumberField(TEXT("changed"), C);
				OutStructured->SetNumberField(TEXT("agent_radius"), NM->AgentRadius);
				OutStructured->SetNumberField(TEXT("agent_height"), NM->AgentHeight);
				OutStructured->SetNumberField(TEXT("agent_max_slope"), NM->AgentMaxSlope);
				OutStructured->SetNumberField(TEXT("agent_max_step_height"), NM->GetAgentMaxStepHeight(ENavigationDataResolution::Default));
				OutSummary = FString::Printf(TEXT("Updated %d NavMesh settings"), C);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("navmesh_rebuild"), TEXT("Trigger a full NavMesh rebuild."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No world."); return false; }
				UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
				if (!NavSys) { OutError = TEXT("No nav system."); return false; }
				NavSys->Build();
				OutStructured->SetBoolField(TEXT("triggered"), true);
				OutSummary = TEXT("NavMesh rebuild triggered");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("nav_volume_create"), TEXT("Spawn NavMeshBoundsVolume at location with extent."),
			FSololmcpSchemaBuilder::Object({{TEXT("location"), VectorSchema()},{TEXT("extent"), VectorSchema()}}, {TEXT("location"), TEXT("extent")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No world."); return false; }
				auto L = Arguments->GetObjectField(TEXT("location")); auto E = Arguments->GetObjectField(TEXT("extent"));
				FVector Loc(L->GetNumberField(TEXT("x")), L->GetNumberField(TEXT("y")), L->GetNumberField(TEXT("z")));
				FVector Ext(E->GetNumberField(TEXT("x")), E->GetNumberField(TEXT("y")), E->GetNumberField(TEXT("z")));
				FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				ANavMeshBoundsVolume* V = World->SpawnActor<ANavMeshBoundsVolume>(Loc, FRotator::ZeroRotator, P);
				if (!V) { OutError = TEXT("Spawn failed."); return false; }
				V->SetActorScale3D(Ext / 100.0f);
				if (!V->GetActorLocation().Equals(Loc, 0.1f) || !V->GetActorScale3D().Equals(Ext / 100.0f, 0.01f))
				{
					OutError = TEXT("NavMeshBoundsVolume spawn transform failed immediate readback.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(V);
				OutSummary = TEXT("Spawned NavMeshBoundsVolume");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("nav_link_proxy_create"), TEXT("Spawn NavLinkProxy for off-mesh nav links."),
			FSololmcpSchemaBuilder::Object({{TEXT("location"), VectorSchema()}}, {TEXT("location")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No world."); return false; }
				auto L = Arguments->GetObjectField(TEXT("location"));
				FVector Loc(L->GetNumberField(TEXT("x")), L->GetNumberField(TEXT("y")), L->GetNumberField(TEXT("z")));
				FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				ANavLinkProxy* NL = World->SpawnActor<ANavLinkProxy>(Loc, FRotator::ZeroRotator, P);
				if (!NL) { OutError = TEXT("Spawn failed."); return false; }
				OutStructured = FSololmcpEditorServices::MakeActorReference(NL);
				OutSummary = TEXT("Spawned NavLinkProxy");
				return true;
			}
		, nullptr
		, 5
		});

		// ============================================================
		// v1.7.0 Tier 2 Batch 2C — Physics Constraints (4 tools)
		// ============================================================

		Registry.Register({ TEXT("physics_constraint_create"), TEXT("Spawn PhysicsConstraintActor between two actors."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor1"), FSololmcpSchemaBuilder::String(TEXT("First actor"))},{TEXT("actor2"), FSololmcpSchemaBuilder::String(TEXT("Second actor"))}}, {TEXT("actor1"), TEXT("actor2")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				AActor* A1 = Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("actor1")), OutError); if (!A1) return false;
				AActor* A2 = Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("actor2")), OutError); if (!A2) return false;
				FVector Loc = (A1->GetActorLocation() + A2->GetActorLocation()) / 2.0f;
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (!World) { OutError = TEXT("No world."); return false; }
				FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				APhysicsConstraintActor* CA = World->SpawnActor<APhysicsConstraintActor>(Loc, FRotator::ZeroRotator, P);
				if (!CA) { OutError = TEXT("Spawn failed."); return false; }
				UPhysicsConstraintComponent* ConstraintComp = CA->GetConstraintComp();
				if (!ConstraintComp) { OutError = TEXT("Spawned constraint actor has no constraint component."); return false; }
				ConstraintComp->ConstraintActor1 = A1;
				ConstraintComp->ConstraintActor2 = A2;
				if (ConstraintComp->ConstraintActor1 != A1 || ConstraintComp->ConstraintActor2 != A2)
				{
					OutError = TEXT("Physics constraint actor references failed immediate readback.");
					return false;
				}
				OutStructured = FSololmcpEditorServices::MakeActorReference(CA);
				OutSummary = FString::Printf(TEXT("Constraint: %s <-> %s"), *A1->GetActorLabel(), *A2->GetActorLabel());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("physics_constraint_configure"), TEXT("Configure constraint limits (linear/angular)."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Constraint actor"))},{TEXT("linear_limit"), FSololmcpSchemaBuilder::Number(TEXT("Linear dist"))},{TEXT("swing1_limit"), FSololmcpSchemaBuilder::Number(TEXT("Swing1 deg"))},{TEXT("swing2_limit"), FSololmcpSchemaBuilder::Number(TEXT("Swing2 deg"))},{TEXT("twist_limit"), FSololmcpSchemaBuilder::Number(TEXT("Twist deg"))}}, {TEXT("actor")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				AActor* A = Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("actor")), OutError); if (!A) return false;
				APhysicsConstraintActor* CA = Cast<APhysicsConstraintActor>(A);
				if (!CA) { OutError = TEXT("Not a constraint actor."); return false; }
				UPhysicsConstraintComponent* CC = CA->GetConstraintComp(); int32 N = 0;
				if (Arguments->HasField(TEXT("linear_limit"))) { float L = Arguments->GetNumberField(TEXT("linear_limit")); CC->SetLinearXLimit(L>0?ELinearConstraintMotion::LCM_Limited:ELinearConstraintMotion::LCM_Locked,L); CC->SetLinearYLimit(L>0?ELinearConstraintMotion::LCM_Limited:ELinearConstraintMotion::LCM_Locked,L); CC->SetLinearZLimit(L>0?ELinearConstraintMotion::LCM_Limited:ELinearConstraintMotion::LCM_Locked,L); N++; }
				if (Arguments->HasField(TEXT("swing1_limit"))) { float L = Arguments->GetNumberField(TEXT("swing1_limit")); CC->SetAngularSwing1Limit(L>0?EAngularConstraintMotion::ACM_Limited:EAngularConstraintMotion::ACM_Locked,L); N++; }
				if (Arguments->HasField(TEXT("swing2_limit"))) { float L = Arguments->GetNumberField(TEXT("swing2_limit")); CC->SetAngularSwing2Limit(L>0?EAngularConstraintMotion::ACM_Limited:EAngularConstraintMotion::ACM_Locked,L); N++; }
				if (Arguments->HasField(TEXT("twist_limit"))) { float L = Arguments->GetNumberField(TEXT("twist_limit")); CC->SetAngularTwistLimit(L>0?EAngularConstraintMotion::ACM_Limited:EAngularConstraintMotion::ACM_Locked,L); N++; }
				if (N == 0)
				{
					OutError = TEXT("No physics constraint properties were provided.");
					return false;
				}
				CA->MarkPackageDirty();
				OutStructured->SetNumberField(TEXT("changed"), N);
				OutSummary = FString::Printf(TEXT("Configured %d properties"), N);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("physics_asset_inspect"), TEXT("Inspect PhysicsAsset bodies and constraints."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PA path"))}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UPhysicsAsset* PA = Cast<UPhysicsAsset>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError)); if (!PA) return false;
				TArray<TSharedPtr<FJsonValue>> B;
				#if SOMOLMCP_DOMAIN_HAS_SKELETAL_BODY_SETUP
				for (USkeletalBodySetup* BS : PA->SkeletalBodySetups) { if (!BS) continue; TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("bone"), BS->BoneName.ToString()); O->SetNumberField(TEXT("geoms"), BS->AggGeom.GetElementCount()); B.Add(MakeShared<FJsonValueObject>(O)); }
#endif // SOMOLMCP_DOMAIN_HAS_SKELETAL_BODY_SETUP
				OutStructured->SetArrayField(TEXT("bodies"), B);
				OutStructured->SetNumberField(TEXT("body_count"), B.Num());
				OutStructured->SetNumberField(TEXT("constraint_count"), PA->ConstraintSetup.Num());
				OutSummary = FString::Printf(TEXT("PA: %d bodies, %d constraints"), B.Num(), PA->ConstraintSetup.Num());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("physics_body_configure"), TEXT("Configure physics on actor root (simulate, mass, damping, gravity)."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor"))},{TEXT("simulate_physics"), FSololmcpSchemaBuilder::Boolean(TEXT("Simulate"))},{TEXT("mass_kg"), FSololmcpSchemaBuilder::Number(TEXT("Mass kg"))},{TEXT("linear_damping"), FSololmcpSchemaBuilder::Number(TEXT("Lin damp"))},{TEXT("angular_damping"), FSololmcpSchemaBuilder::Number(TEXT("Ang damp"))},{TEXT("enable_gravity"), FSololmcpSchemaBuilder::Boolean(TEXT("Gravity"))}}, {TEXT("actor")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				AActor* A = Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("actor")), OutError); if (!A) return false;
				UPrimitiveComponent* R = Cast<UPrimitiveComponent>(A->GetRootComponent());
				if (!R) { OutError = TEXT("No primitive root."); return false; }
				int32 N = 0;
				if (Arguments->HasField(TEXT("simulate_physics"))) { R->SetSimulatePhysics(Arguments->GetBoolField(TEXT("simulate_physics"))); N++; }
				if (Arguments->HasField(TEXT("mass_kg"))) { R->SetMassOverrideInKg(NAME_None, Arguments->GetNumberField(TEXT("mass_kg"))); N++; }
				if (Arguments->HasField(TEXT("linear_damping"))) { R->SetLinearDamping(Arguments->GetNumberField(TEXT("linear_damping"))); N++; }
				if (Arguments->HasField(TEXT("angular_damping"))) { R->SetAngularDamping(Arguments->GetNumberField(TEXT("angular_damping"))); N++; }
				if (Arguments->HasField(TEXT("enable_gravity"))) { R->SetEnableGravity(Arguments->GetBoolField(TEXT("enable_gravity"))); N++; }
				if (N == 0)
				{
					OutError = TEXT("No physics body properties were provided.");
					return false;
				}
				if ((Arguments->HasField(TEXT("simulate_physics")) && R->IsSimulatingPhysics() != Arguments->GetBoolField(TEXT("simulate_physics")))
					|| (Arguments->HasField(TEXT("mass_kg")) && !FMath::IsNearlyEqual(R->GetMass(), static_cast<float>(Arguments->GetNumberField(TEXT("mass_kg"))), 0.05f))
					|| (Arguments->HasField(TEXT("linear_damping")) && !FMath::IsNearlyEqual(R->GetLinearDamping(), static_cast<float>(Arguments->GetNumberField(TEXT("linear_damping"))), 0.001f))
					|| (Arguments->HasField(TEXT("angular_damping")) && !FMath::IsNearlyEqual(R->GetAngularDamping(), static_cast<float>(Arguments->GetNumberField(TEXT("angular_damping"))), 0.001f))
					|| (Arguments->HasField(TEXT("enable_gravity")) && R->IsGravityEnabled() != Arguments->GetBoolField(TEXT("enable_gravity"))))
				{
					OutError = TEXT("Physics body settings failed immediate readback.");
					return false;
				}
				A->MarkPackageDirty();
				OutStructured = FSololmcpEditorServices::MakeActorReference(A);
				OutStructured->SetNumberField(TEXT("changed"), N);
				OutStructured->SetBoolField(TEXT("simulate_physics"), R->IsSimulatingPhysics());
				OutStructured->SetNumberField(TEXT("mass_kg"), R->GetMass());
				OutStructured->SetNumberField(TEXT("linear_damping"), R->GetLinearDamping());
				OutStructured->SetNumberField(TEXT("angular_damping"), R->GetAngularDamping());
				OutStructured->SetBoolField(TEXT("enable_gravity"), R->IsGravityEnabled());
				OutSummary = FString::Printf(TEXT("Configured %d physics props"), N);
				return true;
			}
		, nullptr
		, 5
		});

		// ============================================================
		// v1.7.0 Tier 2 Batch 2D — IK / Retarget (6 tools)
		// ============================================================

		Registry.Register({ TEXT("ik_rig_create"), TEXT("Create an IK Rig Definition asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("Skel mesh"))}}, {TEXT("asset_path"), TEXT("skeletal_mesh_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AP = Arguments->GetStringField(TEXT("asset_path")); FString N = FPackageName::GetShortName(AP);
				USkeletalMesh* SM = LoadObject<USkeletalMesh>(nullptr, *Arguments->GetStringField(TEXT("skeletal_mesh_path")));
				if (!SM) { OutError = TEXT("SkeletalMesh not found."); return false; }
				UPackage* Pkg = CreatePackage(*AP);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Pkg, *N)) { if (!Ex->IsA<UIKRigDefinition>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UIKRigDefinition"), *AP, *Ex->GetClass()->GetName()); return false; } }
				UIKRigDefinition* R = NewObject<UIKRigDefinition>(Pkg, *N, RF_Public | RF_Standalone);
				if (!R) { OutError = TEXT("Failed."); return false; }
				FAssetRegistryModule::AssetCreated(R); R->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("path"), R->GetPathName());
				OutSummary = FString::Printf(TEXT("Created IKRig '%s'"), *N);
				return true;
			}
		, nullptr
		, 5
		});

		// UE 5.7 fix: Use FRetargetDefinition::AddBoneChain() instead of direct member access
		Registry.Register({ TEXT("ik_rig_add_chain"), TEXT("Add bone chain to IK Rig."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("IKRig"))},{TEXT("chain_name"), FSololmcpSchemaBuilder::String(TEXT("Name"))},{TEXT("start_bone"), FSololmcpSchemaBuilder::String(TEXT("Start"))},{TEXT("end_bone"), FSololmcpSchemaBuilder::String(TEXT("End"))}}, {TEXT("asset_path"), TEXT("chain_name"), TEXT("start_bone"), TEXT("end_bone")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UIKRigDefinition* R = Cast<UIKRigDefinition>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError)); if (!R) return false;
				FName ChainName = FName(*Arguments->GetStringField(TEXT("chain_name")));
				FName StartBone = FName(*Arguments->GetStringField(TEXT("start_bone")));
				FName EndBone = FName(*Arguments->GetStringField(TEXT("end_bone")));
				UIKRigController* Ctrl = UIKRigController::GetController(R);
				if (!Ctrl) { OutError = TEXT("Failed to get IKRigController."); return false; }
				Ctrl->AddRetargetChain(ChainName, StartBone, EndBone, NAME_None);
				R->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("chain"), ChainName.ToString());
				OutSummary = FString::Printf(TEXT("Added chain '%s'"), *ChainName.ToString());
				return true;
			}
		, nullptr
		, 5
		});

		// UE 5.7 fix: Use UIKRetargeterController::SetIKRig() instead of removed SetSourceIKRig/SetTargetIKRig
		Registry.Register({ TEXT("ik_retargeter_create"), TEXT("Create an IK Retargeter asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("source_rig"), FSololmcpSchemaBuilder::String(TEXT("Source IKRig"))},{TEXT("target_rig"), FSololmcpSchemaBuilder::String(TEXT("Target IKRig"))}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AP = Arguments->GetStringField(TEXT("asset_path")); FString N = FPackageName::GetShortName(AP);
				UPackage* Pkg = CreatePackage(*AP);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if (UObject* Ex = StaticFindObject(nullptr, Pkg, *N)) { if (!Ex->IsA<UIKRetargeter>()) { OutError = FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UIKRetargeter"), *AP, *Ex->GetClass()->GetName()); return false; } }
				UIKRetargeter* RT = NewObject<UIKRetargeter>(Pkg, *N, RF_Public | RF_Standalone);
				if (!RT) { OutError = TEXT("Failed."); return false; }
				UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(RT);
				if (Arguments->HasField(TEXT("source_rig"))) { auto* S = LoadObject<UIKRigDefinition>(nullptr, *Arguments->GetStringField(TEXT("source_rig"))); if (S && Ctrl) Ctrl->SetIKRig(ERetargetSourceOrTarget::Source, S); }
				if (Arguments->HasField(TEXT("target_rig"))) { auto* T = LoadObject<UIKRigDefinition>(nullptr, *Arguments->GetStringField(TEXT("target_rig"))); if (T && Ctrl) Ctrl->SetIKRig(ERetargetSourceOrTarget::Target, T); }
				FAssetRegistryModule::AssetCreated(RT); RT->MarkPackageDirty();
				OutStructured->SetStringField(TEXT("path"), RT->GetPathName());
				OutSummary = FString::Printf(TEXT("Created IKRetargeter '%s'"), *N);
				return true;
			}
		, nullptr
		, 5
		});

		// UE 5.7 fix: Use UIKRetargeterController::SetIKRig() instead of removed SetSourceIKRig/SetTargetIKRig
		Registry.Register({ TEXT("ik_retargeter_set_source"), TEXT("Set source/target IK Rigs on retargeter."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Retargeter"))},{TEXT("source_rig"), FSololmcpSchemaBuilder::String(TEXT("Source"))},{TEXT("target_rig"), FSololmcpSchemaBuilder::String(TEXT("Target"))}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UIKRetargeter* RT = Cast<UIKRetargeter>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError)); if (!RT) return false;
				UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(RT);
				if (Arguments->HasField(TEXT("source_rig"))) { auto* R = LoadObject<UIKRigDefinition>(nullptr, *Arguments->GetStringField(TEXT("source_rig"))); if (R && Ctrl) Ctrl->SetIKRig(ERetargetSourceOrTarget::Source, R); }
				if (Arguments->HasField(TEXT("target_rig"))) { auto* R = LoadObject<UIKRigDefinition>(nullptr, *Arguments->GetStringField(TEXT("target_rig"))); if (R && Ctrl) Ctrl->SetIKRig(ERetargetSourceOrTarget::Target, R); }
				RT->MarkPackageDirty();
				OutSummary = TEXT("Updated retargeter rigs");
				return true;
			}
		, nullptr
		, 5
		});

		// UE 5.7 fix: Use UIKRetargeterController::AutoMapChains() instead of removed UIKRetargeter::AutoMapChains()
		Registry.Register({ TEXT("ik_retargeter_map_chains"), TEXT("Auto-map bone chains in IK Retargeter."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Retargeter"))}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UIKRetargeter* RT = Cast<UIKRetargeter>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError)); if (!RT) return false;
				UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(RT);
				if (Ctrl) Ctrl->AutoMapChains(EAutoMapChainType::Fuzzy, true);
				RT->MarkPackageDirty();
				OutStructured->SetBoolField(TEXT("mapped"), true);
				OutSummary = TEXT("Auto-mapped chains");
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("skeleton_get_bones"), TEXT("List all bones in SkeletalMesh skeleton."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("SkelMesh"))}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				USkeletalMesh* SM = Cast<USkeletalMesh>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")), OutError)); if (!SM) return false;
				const FReferenceSkeleton& Ref = SM->GetRefSkeleton();
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (int32 i = 0; i < Ref.GetNum(); i++) { TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("name"), Ref.GetBoneName(i).ToString()); O->SetNumberField(TEXT("index"), i); O->SetNumberField(TEXT("parent"), Ref.GetParentIndex(i)); Arr.Add(MakeShared<FJsonValueObject>(O)); }
				OutStructured->SetArrayField(TEXT("bones"), Arr);
				OutStructured->SetNumberField(TEXT("count"), Arr.Num());
				OutSummary = FString::Printf(TEXT("%d bones"), Arr.Num());
				return true;
			}
		, nullptr
		, 30
		});

		// ============================================================
		// v1.7.0 Tier 2 Batch 2E — MPC (3) + 2F Foliage (5) + 2G Streaming (4) + 2H Audio (6)
		// ============================================================

		Registry.Register({ TEXT("mpc_create"), TEXT("Create Material Parameter Collection asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Path"))}}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString P=Arguments->GetStringField(TEXT("asset_path")); FString N=FPackageName::GetShortName(P); UPackage*Pkg=CreatePackage(*P); /* Pre-check: refuse class collision to prevent UE check() fatal */ if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<UMaterialParameterCollection>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UMaterialParameterCollection"),*P,*Ex->GetClass()->GetName());return false;}} UMaterialParameterCollection*M=NewObject<UMaterialParameterCollection>(Pkg,*N,RF_Public|RF_Standalone); if(!M){OutError=TEXT("Failed.");return false;} FAssetRegistryModule::AssetCreated(M);M->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(M&&M->IsA<UMaterialParameterCollection>()){const FString CreatedPath=M->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!Context.Services.AssetExists(CreatedPath)){OutError=TEXT("asset_not_persisted_after_create: ")+CreatedPath;return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),M->GetPathName()); OutSummary=FString::Printf(TEXT("Created MPC '%s'"),*N); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("mpc_add_parameter"), TEXT("Add scalar or vector parameter to MPC."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("MPC"))},{TEXT("name"),FSololmcpSchemaBuilder::String(TEXT("Name"))},{TEXT("type"),FSololmcpSchemaBuilder::String(TEXT("scalar/vector"))},{TEXT("default_value"),FSololmcpSchemaBuilder::Number(TEXT("Scalar default"))},{TEXT("default_vector"),FSololmcpSchemaBuilder::Object({{TEXT("r"),FSololmcpSchemaBuilder::Number()},{TEXT("g"),FSololmcpSchemaBuilder::Number()},{TEXT("b"),FSololmcpSchemaBuilder::Number()},{TEXT("a"),FSololmcpSchemaBuilder::Number()}})}},{TEXT("asset_path"),TEXT("name"),TEXT("type")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UMaterialParameterCollection*M=Cast<UMaterialParameterCollection>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError)); if(!M)return false;
				FString PN=Arguments->GetStringField(TEXT("name")); FString PT=Arguments->GetStringField(TEXT("type")); const FName ParamFName(*PN);
				for(const FCollectionScalarParameter& Existing:M->ScalarParameters){if(Existing.ParameterName==ParamFName){OutError=FString::Printf(TEXT("Parameter already exists: %s"),*PN);return false;}}
				for(const FCollectionVectorParameter& Existing:M->VectorParameters){if(Existing.ParameterName==ParamFName){OutError=FString::Printf(TEXT("Parameter already exists: %s"),*PN);return false;}}
				if(PT.Equals(TEXT("scalar"),ESearchCase::IgnoreCase)){FCollectionScalarParameter P;P.ParameterName=ParamFName;P.DefaultValue=Arguments->HasField(TEXT("default_value"))?Arguments->GetNumberField(TEXT("default_value")):0;M->ScalarParameters.Add(P);}
				else if(PT.Equals(TEXT("vector"),ESearchCase::IgnoreCase)){FCollectionVectorParameter P;P.ParameterName=ParamFName;P.DefaultValue=FLinearColor::Black;TSharedPtr<FJsonObject>VectorDefault;if(TryGetObjectField(Arguments,TEXT("default_vector"),VectorDefault)&&VectorDefault.IsValid()){P.DefaultValue=FLinearColor(VectorDefault->HasTypedField<EJson::Number>(TEXT("r"))?VectorDefault->GetNumberField(TEXT("r")):0.0,VectorDefault->HasTypedField<EJson::Number>(TEXT("g"))?VectorDefault->GetNumberField(TEXT("g")):0.0,VectorDefault->HasTypedField<EJson::Number>(TEXT("b"))?VectorDefault->GetNumberField(TEXT("b")):0.0,VectorDefault->HasTypedField<EJson::Number>(TEXT("a"))?VectorDefault->GetNumberField(TEXT("a")):1.0);}M->VectorParameters.Add(P);}
				else{OutError=TEXT("Type: scalar or vector.");return false;}
				M->MarkPackageDirty(); FString SaveErr; const bool bSaved=Context.Services.SaveAsset(M->GetPathName(),false,SaveErr); OutStructured->SetStringField(TEXT("name"),PN); OutStructured->SetBoolField(TEXT("saved"),bSaved); if(!bSaved){OutStructured->SetStringField(TEXT("save_error"),SaveErr);} OutSummary=FString::Printf(TEXT("Added %s '%s'"),*PT,*PN); return bSaved;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("mpc_list_parameters"), TEXT("List MPC parameters."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("MPC"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UMaterialParameterCollection*M=Cast<UMaterialParameterCollection>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError)); if(!M)return false;
				TArray<TSharedPtr<FJsonValue>>Arr;
				for(auto&P:M->ScalarParameters){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),P.ParameterName.ToString());O->SetStringField(TEXT("type"),TEXT("scalar"));O->SetNumberField(TEXT("default"),P.DefaultValue);Arr.Add(MakeShared<FJsonValueObject>(O));}
				for(auto&P:M->VectorParameters){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),P.ParameterName.ToString());O->SetStringField(TEXT("type"),TEXT("vector"));Arr.Add(MakeShared<FJsonValueObject>(O));}
				OutStructured->SetArrayField(TEXT("parameters"),Arr); OutStructured->SetNumberField(TEXT("count"),Arr.Num()); OutSummary=FString::Printf(TEXT("%d params"),Arr.Num()); return true;
			}
		, nullptr
		, 5
		});

		// UE 5.7: Fixed AddInstance signature (removed IFA param, UFoliageType* is now const),
		// FRandRange int ambiguity, GetAllFoliageTypesForSource → iterate assets + FindInfo.
		Registry.Register({ TEXT("foliage_type_list"), TEXT("List FoliageType assets in project."),
			FSololmcpSchemaBuilder::Object({{TEXT("path_filter"),FSololmcpSchemaBuilder::String(TEXT("Path prefix"))}},{}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString F=Arguments->HasField(TEXT("path_filter"))?Arguments->GetStringField(TEXT("path_filter")):TEXT("/Game");
				FAssetRegistryModule&ARM=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				TArray<FAssetData>Assets; ARM.Get().GetAssetsByClass(UFoliageType::StaticClass()->GetClassPathName(),Assets,true);
				TArray<TSharedPtr<FJsonValue>>Arr;
				for(auto&A:Assets){if(!A.GetObjectPathString().StartsWith(F))continue; TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("name"),A.AssetName.ToString()); O->SetStringField(TEXT("path"),A.GetObjectPathString()); Arr.Add(MakeShared<FJsonValueObject>(O));}
				OutStructured->SetArrayField(TEXT("types"),Arr); OutSummary=FString::Printf(TEXT("%d foliage types"),Arr.Num()); return true;
			}
		, nullptr
		, 15
		});

		Registry.Register({ TEXT("foliage_type_add"), TEXT("Create FoliageType from StaticMesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("mesh_path"),FSololmcpSchemaBuilder::String(TEXT("Mesh"))},{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Output path"))}},{TEXT("mesh_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UStaticMesh*Mesh=LoadObject<UStaticMesh>(nullptr,*Arguments->GetStringField(TEXT("mesh_path"))); if(!Mesh){OutError=TEXT("Mesh not found.");return false;}
				FString OP=Arguments->HasField(TEXT("asset_path"))?Arguments->GetStringField(TEXT("asset_path")):(TEXT("/Game/Foliage/FT_")+Mesh->GetName());
				FString N=FPackageName::GetShortName(OP); UPackage*Pkg=CreatePackage(*OP);
				/* Pre-check: refuse class collision to prevent UE check() fatal */
				if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<UFoliageType_InstancedStaticMesh>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UFoliageType_InstancedStaticMesh"),*OP,*Ex->GetClass()->GetName());return false;}}
				UFoliageType_InstancedStaticMesh*FT=NewObject<UFoliageType_InstancedStaticMesh>(Pkg,*N,RF_Public|RF_Standalone);
				if(!FT){OutError=TEXT("Failed.");return false;} FT->SetSource(Mesh);
				FAssetRegistryModule::AssetCreated(FT);FT->MarkPackageDirty();
				FString SaveErr; const bool bSaved=Context.Services.SaveAsset(FT->GetPathName(),false,SaveErr);(void)bSaved;
				if(!VerifyCreatedAssetReloaded(Context.Services,FT,UFoliageType_InstancedStaticMesh::StaticClass(),OutStructured,OutError)){return false;}
				OutStructured->SetStringField(TEXT("path"),FT->GetPathName()); OutSummary=FString::Printf(TEXT("Created FoliageType '%s'"),*N); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("foliage_instances_paint"), TEXT("Place foliage instances around a location."),
			FSololmcpSchemaBuilder::Object({{TEXT("foliage_type_path"),FSololmcpSchemaBuilder::String(TEXT("FT path"))},{TEXT("location"),VectorSchema()},{TEXT("radius"),FSololmcpSchemaBuilder::Number(TEXT("Radius"))},{TEXT("count"),FSololmcpSchemaBuilder::Integer(TEXT("Count"))}},{TEXT("foliage_type_path"),TEXT("location")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UFoliageType*FT=Cast<UFoliageType>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("foliage_type_path")),OutError)); if(!FT)return false;
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				auto L=Arguments->GetObjectField(TEXT("location")); FVector C(L->GetNumberField(TEXT("x")),L->GetNumberField(TEXT("y")),L->GetNumberField(TEXT("z")));
				float R=Arguments->HasField(TEXT("radius"))?static_cast<float>(Arguments->GetNumberField(TEXT("radius"))):500.0f; int32 Cnt=Arguments->HasField(TEXT("count"))?static_cast<int32>(Arguments->GetNumberField(TEXT("count"))):10;
				FModuleManager::Get().LoadModule(TEXT("Foliage"));
				AInstancedFoliageActor*IFA=AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(W,/*bCreateIfNone=*/true);
				if(!IFA){OutError=TEXT("No foliage actor.");return false;}
				// Headless IFA spawns lack a root component; AddInstanceBaseId's class-ignore
				// IsA check has no null guard (FoliageEditModule.cpp) -> 0xC0000005. Ensure a
				// valid root and set BaseComponent on every instance added.
				if(!IFA->GetRootComponent()){USceneComponent*Root=NewObject<USceneComponent>(IFA);IFA->SetRootComponent(Root);Root->RegisterComponent();}
				FFoliageInfo*Info=IFA->FindOrAddMesh(FT); if(!Info){OutError=TEXT("No info.");return false;}
				IFA->Modify();
				const int32 BeforeCount=Info->Instances.Num();
				for(int32 i=0;i<Cnt;i++){FFoliageInstance In;In.Location=C+FVector(FMath::FRandRange(-R,R),FMath::FRandRange(-R,R),0.0f);In.Rotation=FRotator(0,FMath::FRandRange(0.0f,360.0f),0);In.DrawScale3D=FVector3f(1.0f);In.BaseComponent=IFA->GetRootComponent();Info->AddInstance(FT,In);}
				const int32 AddedCount=Info->Instances.Num()-BeforeCount;
				if(AddedCount!=Cnt){SololmcpError::Set(OutStructured,TEXT("OPERATION_FAILED"),TEXT("count"),TEXT("Foliage AddInstance did not add the requested number of instances."));OutError=FString::Printf(TEXT("Added %d/%d foliage instances."),AddedCount,Cnt);return false;}
				IFA->MarkPackageDirty(); OutStructured->SetNumberField(TEXT("added"),AddedCount); OutSummary=FString::Printf(TEXT("Painted %d instances"),AddedCount); return true;
			}
		, nullptr
		, 5
		});

		// foliage_brush_stroke — continuous stroke brush with WIDTH + DENSITY-per-m²
		// (the gap foliage_instances_paint left: it was single-point radius+count).
		// Distributes instances along the start→end segment within `width`, count
		// derived from density × stroke area. Mirrors the AddInstance path above.
		Registry.Register({ TEXT("foliage_brush_stroke"), TEXT("Paint a continuous stroke of foliage along start->end with brush width and density per m^2."),
			FSololmcpSchemaBuilder::Object({{TEXT("foliage_type_path"),FSololmcpSchemaBuilder::String(TEXT("FoliageType asset path"))},{TEXT("start"),VectorSchema()},{TEXT("end"),VectorSchema()},{TEXT("width"),FSololmcpSchemaBuilder::Number(TEXT("Brush width in cm"))},{TEXT("density"),FSololmcpSchemaBuilder::Number(TEXT("Instances per square meter"))},{TEXT("random_scale_min"),FSololmcpSchemaBuilder::Number(TEXT("Min uniform scale (default 1.0)"))},{TEXT("random_scale_max"),FSololmcpSchemaBuilder::Number(TEXT("Max uniform scale (default 1.0)"))}},{TEXT("foliage_type_path"),TEXT("start"),TEXT("end")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UFoliageType*FT=Cast<UFoliageType>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("foliage_type_path")),OutError)); if(!FT)return false;
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				auto SO=Arguments->GetObjectField(TEXT("start")); auto EO=Arguments->GetObjectField(TEXT("end"));
				FVector S(SO->GetNumberField(TEXT("x")),SO->GetNumberField(TEXT("y")),SO->GetNumberField(TEXT("z")));
				FVector E(EO->GetNumberField(TEXT("x")),EO->GetNumberField(TEXT("y")),EO->GetNumberField(TEXT("z")));
				float Width=Arguments->HasField(TEXT("width"))?static_cast<float>(Arguments->GetNumberField(TEXT("width"))):200.0f; if(Width<1.0f)Width=1.0f;
				float Density=Arguments->HasField(TEXT("density"))?static_cast<float>(Arguments->GetNumberField(TEXT("density"))):1.0f; if(Density<=0.0f)Density=1.0f;
				float SMin=Arguments->HasField(TEXT("random_scale_min"))?static_cast<float>(Arguments->GetNumberField(TEXT("random_scale_min"))):1.0f;
				float SMax=Arguments->HasField(TEXT("random_scale_max"))?static_cast<float>(Arguments->GetNumberField(TEXT("random_scale_max"))):FMath::Max(SMin,1.0f);
				const float LenCm=static_cast<float>(FVector::Dist(S,E)); if(LenCm<1.0f){OutError=TEXT("start and end are too close (stroke length < 1cm).");return false;}
				// area in m^2 = (length/100) * (width/100); count = density * area, clamped.
				const float AreaM2=(LenCm/100.0f)*(Width/100.0f);
				int32 Cnt=FMath::RoundToInt(Density*AreaM2); Cnt=FMath::Clamp(Cnt,1,50000);
				FVector Dir=(E-S); Dir.Z=0.0f; if(!Dir.Normalize()){Dir=FVector(1,0,0);}
				const FVector Perp(-Dir.Y,Dir.X,0.0f); // in-plane perpendicular for width jitter
				FModuleManager::Get().LoadModule(TEXT("Foliage"));
				AInstancedFoliageActor*IFA=AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(W,/*bCreateIfNone=*/true); if(!IFA){OutError=TEXT("No foliage actor.");return false;}
				// Same null-base guard as foliage_instances_paint: headless IFA has no root.
				if(!IFA->GetRootComponent()){USceneComponent*Root=NewObject<USceneComponent>(IFA);IFA->SetRootComponent(Root);Root->RegisterComponent();}
				FFoliageInfo*Info=IFA->FindOrAddMesh(FT); if(!Info){OutError=TEXT("No info.");return false;}
				IFA->Modify();
				const int32 BeforeCount=Info->Instances.Num();
				for(int32 i=0;i<Cnt;i++){
					const float T=FMath::FRand(); // position along stroke
					const float Off=FMath::FRandRange(-Width*0.5f,Width*0.5f); // across width
					FFoliageInstance In;
					In.Location=FMath::Lerp(S,E,T)+Perp*Off;
					In.Rotation=FRotator(0,FMath::FRandRange(0.0f,360.0f),0);
					const float Sc=(SMax>SMin)?FMath::FRandRange(SMin,SMax):SMin;
					In.DrawScale3D=FVector3f(Sc);
					In.BaseComponent=IFA->GetRootComponent();
					Info->AddInstance(FT,In);
				}
				const int32 AddedCount=Info->Instances.Num()-BeforeCount;
				if(AddedCount!=Cnt){SololmcpError::Set(OutStructured,TEXT("OPERATION_FAILED"),TEXT("count"),TEXT("Foliage brush stroke did not add the requested number of instances."));OutError=FString::Printf(TEXT("Added %d/%d stroke instances."),AddedCount,Cnt);return false;}
				IFA->MarkPackageDirty();
				OutStructured->SetNumberField(TEXT("added"),AddedCount);
				OutStructured->SetNumberField(TEXT("stroke_length_cm"),LenCm);
				OutStructured->SetNumberField(TEXT("area_m2"),AreaM2);
				OutSummary=FString::Printf(TEXT("Stroke painted %d instances over %.1f m^2 (density %.2f/m^2)"),AddedCount,AreaM2,Density);
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("foliage_instances_remove"), TEXT("Remove foliage instances in sphere."),
			FSololmcpSchemaBuilder::Object({{TEXT("foliage_type_path"),FSololmcpSchemaBuilder::String(TEXT("FT"))},{TEXT("location"),VectorSchema()},{TEXT("radius"),FSololmcpSchemaBuilder::Number(TEXT("Radius"))}},{TEXT("foliage_type_path"),TEXT("location"),TEXT("radius")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UFoliageType*FT=Cast<UFoliageType>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("foliage_type_path")),OutError)); if(!FT)return false;
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				auto L=Arguments->GetObjectField(TEXT("location")); FVector C(L->GetNumberField(TEXT("x")),L->GetNumberField(TEXT("y")),L->GetNumberField(TEXT("z")));
				float RS=FMath::Square(static_cast<float>(Arguments->GetNumberField(TEXT("radius"))));
				FModuleManager::Get().LoadModule(TEXT("Foliage"));
				AInstancedFoliageActor*IFA=AInstancedFoliageActor::GetInstancedFoliageActorForCurrentLevel(W,/*bCreateIfNone=*/true); if(!IFA){OutError=TEXT("No actor.");return false;}
				FFoliageInfo*Info=IFA->FindInfo(FT); if(!Info){OutError=TEXT("Not found.");return false;}
				const int32 BeforeCount=Info->Instances.Num();
				TArray<int32>Rm; for(int32 i=0;i<Info->Instances.Num();i++) if(FVector::DistSquared(Info->Instances[i].Location,C)<=RS) Rm.Add(i);
				Info->RemoveInstances(Rm,true); IFA->MarkPackageDirty();
				const int32 RemovedCount=BeforeCount-Info->Instances.Num();
				if(RemovedCount!=Rm.Num()){OutError=FString::Printf(TEXT("Foliage remove failed readback: requested %d removals, observed %d."),Rm.Num(),RemovedCount);return false;}
				OutStructured->SetNumberField(TEXT("removed"),Rm.Num()); OutSummary=FString::Printf(TEXT("Removed %d"),Rm.Num()); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("foliage_statistics"), TEXT("Foliage stats: types and instance counts."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				// UE 5.7: GetAllFoliageTypesForSource signature changed (TMap → TArray).
				// Use asset registry to iterate all UFoliageType assets, then query FindInfo on each IFA.
				FAssetRegistryModule&ARM=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				TArray<FAssetData>FTAssets; ARM.Get().GetAssetsByClass(UFoliageType::StaticClass()->GetClassPathName(),FTAssets,true);
				int32 TotTypes=0,TotInstances=0;
				TArray<TSharedPtr<FJsonValue>>Arr;
				for(const FAssetData&FTData:FTAssets)
				{
					UFoliageType*FT=Cast<UFoliageType>(FTData.GetAsset()); if(!FT)continue;
					int32 TypeInstances=0;
					for(TActorIterator<AInstancedFoliageActor>It(W);It;++It)
					{
						const FFoliageInfo*Info=(*It)->FindInfo(FT);
						if(Info)TypeInstances+=Info->Instances.Num();
					}
					if(TypeInstances>0)
					{
						TotTypes++;TotInstances+=TypeInstances;
						TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();
						O->SetStringField(TEXT("name"),FTData.AssetName.ToString());
						O->SetStringField(TEXT("path"),FTData.GetObjectPathString());
						O->SetNumberField(TEXT("instances"),TypeInstances);
						Arr.Add(MakeShared<FJsonValueObject>(O));
					}
				}
				OutStructured->SetNumberField(TEXT("types"),TotTypes); OutStructured->SetNumberField(TEXT("instances"),TotInstances);
				OutStructured->SetArrayField(TEXT("details"),Arr);
				OutSummary=FString::Printf(TEXT("%d types, %d instances"),TotTypes,TotInstances); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("level_streaming_list"), TEXT("List streaming sub-levels."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				TArray<TSharedPtr<FJsonValue>>Arr;
				for(ULevelStreaming*LS:W->GetStreamingLevels()){if(!LS)continue; TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("package"),LS->GetWorldAssetPackageName()); O->SetBoolField(TEXT("loaded"),LS->GetLoadedLevel()!=nullptr); O->SetBoolField(TEXT("visible"),LS->GetShouldBeVisibleFlag()); Arr.Add(MakeShared<FJsonValueObject>(O));}
				OutStructured->SetArrayField(TEXT("levels"),Arr); OutSummary=FString::Printf(TEXT("%d streaming levels"),Arr.Num()); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("level_streaming_add"), TEXT("Add streaming sub-level."),
			FSololmcpSchemaBuilder::Object({{TEXT("level_path"),FSololmcpSchemaBuilder::String(TEXT("Package"))},{TEXT("offset"),VectorSchema()}},{TEXT("level_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				FString LP=Arguments->GetStringField(TEXT("level_path"));
				ULevelStreamingDynamic*LS=NewObject<ULevelStreamingDynamic>(W);
				LS->SetWorldAssetByPackageName(FName(*LP)); LS->SetShouldBeLoaded(true); LS->SetShouldBeVisible(true);
				if(Arguments->HasField(TEXT("offset"))){auto O=Arguments->GetObjectField(TEXT("offset")); LS->LevelTransform=FTransform(FVector(O->GetNumberField(TEXT("x")),O->GetNumberField(TEXT("y")),O->GetNumberField(TEXT("z"))));}
				W->AddStreamingLevel(LS); W->MarkPackageDirty();
				if(!W->GetStreamingLevels().Contains(LS)){OutError=TEXT("AddStreamingLevel did not retain the streaming level.");return false;}
				OutStructured->SetStringField(TEXT("level"),LP); OutSummary=FString::Printf(TEXT("Added '%s'"),*LP); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("level_streaming_remove"), TEXT("Remove streaming sub-level."),
			FSololmcpSchemaBuilder::Object({{TEXT("level_path"),FSololmcpSchemaBuilder::String(TEXT("Package"))}},{TEXT("level_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				FString LP=Arguments->GetStringField(TEXT("level_path")); ULevelStreaming*F=nullptr;
				for(ULevelStreaming*LS:W->GetStreamingLevels()) if(LS&&LS->GetWorldAssetPackageName().Equals(LP,ESearchCase::IgnoreCase)){F=LS;break;}
				if(!F){OutError=TEXT("Not found.");return false;}
				W->RemoveStreamingLevel(F); W->MarkPackageDirty();
				for(ULevelStreaming*LS:W->GetStreamingLevels()){if(LS&&LS->GetWorldAssetPackageName().Equals(LP,ESearchCase::IgnoreCase)){OutError=TEXT("RemoveStreamingLevel failed immediate readback.");return false;}}
				OutStructured->SetStringField(TEXT("removed"),LP); OutSummary=FString::Printf(TEXT("Removed '%s'"),*LP); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("level_streaming_set_transform"), TEXT("Set streaming level position/rotation offset."),
			FSololmcpSchemaBuilder::Object({{TEXT("level_path"),FSololmcpSchemaBuilder::String(TEXT("Package"))},{TEXT("offset"),VectorSchema()},{TEXT("rotation"),RotatorSchema()}},{TEXT("level_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				FString LP=Arguments->GetStringField(TEXT("level_path")); ULevelStreaming*F=nullptr;
				for(ULevelStreaming*LS:W->GetStreamingLevels()) if(LS&&LS->GetWorldAssetPackageName().Equals(LP,ESearchCase::IgnoreCase)){F=LS;break;}
				if(!F){OutError=TEXT("Not found.");return false;}
				if(!Arguments->HasField(TEXT("offset"))&&!Arguments->HasField(TEXT("rotation"))){OutError=TEXT("No streaming level transform fields were provided.");return false;}
				FVector Off=FVector::ZeroVector; FRotator Rot=FRotator::ZeroRotator;
				if(Arguments->HasField(TEXT("offset"))){auto O=Arguments->GetObjectField(TEXT("offset"));Off=FVector(O->GetNumberField(TEXT("x")),O->GetNumberField(TEXT("y")),O->GetNumberField(TEXT("z")));}
				if(Arguments->HasField(TEXT("rotation"))){auto R=Arguments->GetObjectField(TEXT("rotation"));Rot=FRotator(R->GetNumberField(TEXT("pitch")),R->GetNumberField(TEXT("yaw")),R->GetNumberField(TEXT("roll")));}
				F->LevelTransform=FTransform(Rot,Off); W->MarkPackageDirty();
				if(!F->LevelTransform.GetLocation().Equals(Off,0.1f)||!F->LevelTransform.Rotator().Equals(Rot,0.01f)){OutError=TEXT("Streaming level transform failed immediate readback.");return false;}
				OutSummary=FString::Printf(TEXT("Transform set for '%s'"),*LP); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("audio_sound_class_create"), TEXT("Create Sound Class asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("volume"),FSololmcpSchemaBuilder::Number(TEXT("Vol 0-1"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=FPackageName::GetShortName(P); UPackage*Pkg=CreatePackage(*P); /* Pre-check: refuse class collision to prevent UE check() fatal */ if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<USoundClass>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with SoundClass"),*P,*Ex->GetClass()->GetName());return false;}} USoundClass*S=NewObject<USoundClass>(Pkg,*N,RF_Public|RF_Standalone); if(!S){OutError=TEXT("Failed.");return false;} if(Arguments->HasField(TEXT("volume")))S->Properties.Volume=Arguments->GetNumberField(TEXT("volume")); FAssetRegistryModule::AssetCreated(S);S->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(S&&S->IsA<USoundClass>()){const FString CreatedPath=S->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!VerifyCreatedAssetReloaded(Context.Services,S,USoundClass::StaticClass(),OutStructured,OutError)){return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),S->GetPathName()); OutSummary=FString::Printf(TEXT("Created SoundClass '%s'"),*N); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("audio_sound_mix_create"), TEXT("Create Sound Mix asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=FPackageName::GetShortName(P); UPackage*Pkg=CreatePackage(*P); /* Pre-check class collision */ if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<USoundMix>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with SoundMix"),*P,*Ex->GetClass()->GetName());return false;}} USoundMix*S=NewObject<USoundMix>(Pkg,*N,RF_Public|RF_Standalone); if(!S){OutError=TEXT("Failed.");return false;} FAssetRegistryModule::AssetCreated(S);S->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(S&&S->IsA<USoundMix>()){const FString CreatedPath=S->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!VerifyCreatedAssetReloaded(Context.Services,S,USoundMix::StaticClass(),OutStructured,OutError)){return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),S->GetPathName()); OutSummary=FString::Printf(TEXT("Created SoundMix '%s'"),*N); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("audio_attenuation_create"), TEXT("Create Sound Attenuation asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("inner_radius"),FSololmcpSchemaBuilder::Number(TEXT("Inner"))},{TEXT("falloff_distance"),FSololmcpSchemaBuilder::Number(TEXT("Falloff"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=FPackageName::GetShortName(P); UPackage*Pkg=CreatePackage(*P); /* Pre-check class collision */ if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<USoundAttenuation>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with SoundAttenuation"),*P,*Ex->GetClass()->GetName());return false;}} USoundAttenuation*S=NewObject<USoundAttenuation>(Pkg,*N,RF_Public|RF_Standalone); if(!S){OutError=TEXT("Failed.");return false;} if(Arguments->HasField(TEXT("inner_radius")))S->Attenuation.AttenuationShapeExtents.X=Arguments->GetNumberField(TEXT("inner_radius")); if(Arguments->HasField(TEXT("falloff_distance")))S->Attenuation.FalloffDistance=Arguments->GetNumberField(TEXT("falloff_distance")); FAssetRegistryModule::AssetCreated(S);S->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(S&&S->IsA<USoundAttenuation>()){const FString CreatedPath=S->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!VerifyCreatedAssetReloaded(Context.Services,S,USoundAttenuation::StaticClass(),OutStructured,OutError)){return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),S->GetPathName()); OutSummary=FString::Printf(TEXT("Created Attenuation '%s'"),*N); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("audio_concurrency_create"), TEXT("Create Sound Concurrency asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("max_count"),FSololmcpSchemaBuilder::Integer(TEXT("Max"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=FPackageName::GetShortName(P); UPackage*Pkg=CreatePackage(*P); /* Pre-check class collision */ if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<USoundConcurrency>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with SoundConcurrency"),*P,*Ex->GetClass()->GetName());return false;}} USoundConcurrency*S=NewObject<USoundConcurrency>(Pkg,*N,RF_Public|RF_Standalone); if(!S){OutError=TEXT("Failed.");return false;} if(Arguments->HasField(TEXT("max_count")))S->Concurrency.MaxCount=static_cast<int32>(Arguments->GetNumberField(TEXT("max_count"))); FAssetRegistryModule::AssetCreated(S);S->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(S&&S->IsA<USoundConcurrency>()){const FString CreatedPath=S->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!VerifyCreatedAssetReloaded(Context.Services,S,USoundConcurrency::StaticClass(),OutStructured,OutError)){return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),S->GetPathName()); OutSummary=FString::Printf(TEXT("Created Concurrency '%s'"),*N); return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("audio_asset_inspect"), TEXT("Inspect SoundWave/SoundCue properties."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Sound path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AP=Arguments->GetStringField(TEXT("asset_path"));
				USoundWave*W=Cast<USoundWave>(Context.Services.LoadAsset(AP,OutError));
				if(W){OutStructured->SetStringField(TEXT("type"),TEXT("SoundWave"));OutStructured->SetNumberField(TEXT("duration"),W->Duration);OutStructured->SetNumberField(TEXT("channels"),W->NumChannels);OutStructured->SetNumberField(TEXT("sample_rate"),W->GetSampleRateForCurrentPlatform());OutSummary=FString::Printf(TEXT("%.2fs %dch"),W->Duration,W->NumChannels);return true;}
				OutError.Reset(); USoundCue*C=Cast<USoundCue>(Context.Services.LoadAsset(AP,OutError));
				if(C){OutStructured->SetStringField(TEXT("type"),TEXT("SoundCue"));OutStructured->SetNumberField(TEXT("duration"),C->Duration);OutSummary=FString::Printf(TEXT("Cue %.2fs"),C->Duration);return true;}
				OutError=TEXT("Not found.");return false;
			}
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("audio_soundscape_list"), TEXT("List audio sources in level."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld*W=GEditor->GetEditorWorldContext().World(); if(!W){OutError=TEXT("No world.");return false;}
				TArray<TSharedPtr<FJsonValue>>Arr;
				for(TActorIterator<AActor>It(W);It;++It){TInlineComponentArray<UAudioComponent*>ACs;(*It)->GetComponents(ACs);for(UAudioComponent*AC:ACs){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("actor"),(*It)->GetActorLabel());O->SetStringField(TEXT("sound"),AC->Sound?AC->Sound->GetName():TEXT("None"));O->SetBoolField(TEXT("auto_activate"),AC->bAutoActivate);Arr.Add(MakeShared<FJsonValueObject>(O));}}
				OutStructured->SetArrayField(TEXT("sources"),Arr); OutSummary=FString::Printf(TEXT("%d audio sources"),Arr.Num()); return true;
			}
		, nullptr
		, 5
		});

		// ===== Tier 3A: Render Target Tools (v1.7.0) =====

		Registry.Register({ TEXT("render_target_create"), TEXT("Create a TextureRenderTarget2D asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("width"),FSololmcpSchemaBuilder::Integer(TEXT("Px"))},{TEXT("height"),FSololmcpSchemaBuilder::Integer(TEXT("Px"))},{TEXT("format"),FSololmcpSchemaBuilder::String(TEXT("RTF_RGBA16f etc"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=FPackageName::GetShortName(P);UPackage*Pkg=CreatePackage(*P);/* Pre-check: refuse class collision to prevent UE check() fatal */ if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<UTextureRenderTarget2D>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UTextureRenderTarget2D"),*P,*Ex->GetClass()->GetName());return false;}}UTextureRenderTarget2D*RT=NewObject<UTextureRenderTarget2D>(Pkg,*N,RF_Public|RF_Standalone);if(!RT){OutError=TEXT("Failed.");return false;}RT->SizeX=Arguments->HasField(TEXT("width"))?static_cast<int32>(Arguments->GetNumberField(TEXT("width"))):256;RT->SizeY=Arguments->HasField(TEXT("height"))?static_cast<int32>(Arguments->GetNumberField(TEXT("height"))):256;if(Arguments->HasField(TEXT("format"))){FString Fmt=Arguments->GetStringField(TEXT("format"));if(Fmt==TEXT("RTF_R8"))RT->RenderTargetFormat=RTF_R8;else if(Fmt==TEXT("RTF_RGBA8"))RT->RenderTargetFormat=RTF_RGBA8;else if(Fmt==TEXT("RTF_R16f"))RT->RenderTargetFormat=RTF_R16f;else if(Fmt==TEXT("RTF_RGBA32f"))RT->RenderTargetFormat=RTF_RGBA32f;else RT->RenderTargetFormat=RTF_RGBA16f;}RT->UpdateResourceImmediate(true);FAssetRegistryModule::AssetCreated(RT);RT->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(RT&&RT->IsA<UTextureRenderTarget2D>()){const FString CreatedPath=RT->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!Context.Services.AssetExists(CreatedPath)){OutError=TEXT("asset_not_persisted_after_create: ")+CreatedPath;return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),RT->GetPathName());OutStructured->SetNumberField(TEXT("width"),RT->SizeX);OutStructured->SetNumberField(TEXT("height"),RT->SizeY);OutSummary=FString::Printf(TEXT("Created RT '%s' %dx%d"),*N,RT->SizeX,RT->SizeY);return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("render_target_configure"), TEXT("Change render target size/format."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("width"),FSololmcpSchemaBuilder::Integer(TEXT("Px"))},{TEXT("height"),FSololmcpSchemaBuilder::Integer(TEXT("Px"))},{TEXT("format"),FSololmcpSchemaBuilder::String(TEXT("RTF enum"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UTextureRenderTarget2D*RT=Cast<UTextureRenderTarget2D>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError));if(!RT)return false;if(Arguments->HasField(TEXT("width")))RT->SizeX=static_cast<int32>(Arguments->GetNumberField(TEXT("width")));if(Arguments->HasField(TEXT("height")))RT->SizeY=static_cast<int32>(Arguments->GetNumberField(TEXT("height")));if(Arguments->HasField(TEXT("format"))){FString Fmt=Arguments->GetStringField(TEXT("format"));if(Fmt==TEXT("RTF_R8"))RT->RenderTargetFormat=RTF_R8;else if(Fmt==TEXT("RTF_RGBA8"))RT->RenderTargetFormat=RTF_RGBA8;else if(Fmt==TEXT("RTF_R16f"))RT->RenderTargetFormat=RTF_R16f;else if(Fmt==TEXT("RTF_RGBA32f"))RT->RenderTargetFormat=RTF_RGBA32f;else RT->RenderTargetFormat=RTF_RGBA16f;}RT->UpdateResourceImmediate(true);RT->MarkPackageDirty();OutSummary=FString::Printf(TEXT("Configured RT %dx%d"),RT->SizeX,RT->SizeY);return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("render_target_list"), TEXT("List TextureRenderTarget2D assets."),
			FSololmcpSchemaBuilder::Object({{TEXT("path_filter"),FSololmcpSchemaBuilder::String(TEXT("Opt path prefix"))}}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FAssetRegistryModule&ARM=FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");FARFilter F;F.ClassPaths.Add(UTextureRenderTarget2D::StaticClass()->GetClassPathName());if(Arguments->HasField(TEXT("path_filter")))F.PackagePaths.Add(FName(*Arguments->GetStringField(TEXT("path_filter"))));TArray<FAssetData>Assets;ARM.Get().GetAssets(F,Assets);TArray<TSharedPtr<FJsonValue>>Arr;for(auto&A:Assets){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("path"),A.GetObjectPathString());O->SetStringField(TEXT("name"),A.AssetName.ToString());Arr.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("render_targets"),Arr);OutSummary=FString::Printf(TEXT("%d render targets"),Arr.Num());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("scene_capture2d_spawn_with_render_target"), TEXT("Spawn a SceneCapture2D actor and bind TextureTarget to a render target without forcing an immediate render."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("render_target_asset"), FSololmcpSchemaBuilder::String(TEXT("TextureRenderTarget2D asset path"))},
				{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label"))},
				{TEXT("location"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})},
				{TEXT("rotation"), FSololmcpSchemaBuilder::Object({{TEXT("pitch"), FSololmcpSchemaBuilder::Number()}, {TEXT("yaw"), FSololmcpSchemaBuilder::Number()}, {TEXT("roll"), FSololmcpSchemaBuilder::Number()}})}
			}, {TEXT("render_target_asset")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString RenderTargetAsset;
				if (!Arguments->TryGetStringField(TEXT("render_target_asset"), RenderTargetAsset) || RenderTargetAsset.IsEmpty())
				{
					OutError = TEXT("Missing render_target_asset.");
					return false;
				}
				UTextureRenderTarget2D* RT = Cast<UTextureRenderTarget2D>(Context.Services.LoadAsset(RenderTargetAsset, OutError));
				if (!RT)
				{
					OutError = TEXT("render_target_asset must resolve to TextureRenderTarget2D.");
					return false;
				}
				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				if (!World)
				{
					OutError = TEXT("No editor world.");
					return false;
				}

				FVector Location(0.0, 0.0, 300.0);
				if (const TSharedPtr<FJsonObject>* LocObj = nullptr; Arguments->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
				{
					double X = Location.X, Y = Location.Y, Z = Location.Z;
					(*LocObj)->TryGetNumberField(TEXT("x"), X);
					(*LocObj)->TryGetNumberField(TEXT("y"), Y);
					(*LocObj)->TryGetNumberField(TEXT("z"), Z);
					Location = FVector(X, Y, Z);
				}
				FRotator Rotation(-30.0, 0.0, 0.0);
				if (const TSharedPtr<FJsonObject>* RotObj = nullptr; Arguments->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
				{
					double Pitch = Rotation.Pitch, Yaw = Rotation.Yaw, Roll = Rotation.Roll;
					(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
					(*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
					(*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
					Rotation = FRotator(Pitch, Yaw, Roll);
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SceneCapture2DSpawnWithRenderTarget", "SOMOLMCP Spawn SceneCapture2D With RenderTarget"));
				ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), Location, Rotation);
				if (!CaptureActor)
				{
					OutError = TEXT("Failed to spawn SceneCapture2D.");
					return false;
				}
				FString ActorName;
				Arguments->TryGetStringField(TEXT("actor_name"), ActorName);
				if (ActorName.IsEmpty())
				{
					ActorName = FString::Printf(TEXT("SOMOLMCP_SceneCapture2D_%d"), FMath::Rand());
				}
#if WITH_EDITOR
				CaptureActor->SetActorLabel(ActorName);
#endif
				USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
				if (!Capture)
				{
					OutError = TEXT("SceneCapture2D actor has no capture component.");
					return false;
				}
				Capture->Modify();
				RT->UpdateResourceImmediate(false);
				Capture->TextureTarget = RT;
				Capture->bCaptureEveryFrame = false;
				Capture->bCaptureOnMovement = false;
				Capture->MarkRenderStateDirty();
				CaptureActor->MarkPackageDirty();

				OutStructured->SetStringField(TEXT("actor_label"), ActorName);
				OutStructured->SetStringField(TEXT("actor_path"), CaptureActor->GetPathName());
				OutStructured->SetStringField(TEXT("scene_capture_actor"), CaptureActor->GetPathName());
				OutStructured->SetStringField(TEXT("scene_capture_component"), Capture->GetPathName());
				OutStructured->SetStringField(TEXT("render_target_asset"), RT->GetPathName());
				OutStructured->SetBoolField(TEXT("texture_target_bound"), Capture->TextureTarget == RT);
				OutStructured->SetBoolField(TEXT("capture_scene_called"), false);
				OutStructured->SetStringField(TEXT("capture_scene_policy"), TEXT("skipped_immediate_capture_to_avoid_ue57_render_target_null_crash"));
				OutStructured->SetBoolField(TEXT("mutation_performed"), true);
				OutSummary = FString::Printf(TEXT("Spawned SceneCapture2D '%s' and bound '%s' without immediate capture."), *ActorName, *RT->GetPathName());
				return true;
			}
		, nullptr
		, 5
		});

		// ===== Tier 3A: GAS (Gameplay Ability System) Tools — re-enabled for UE 5.7.4 =====
#if 1
		Registry.Register({ TEXT("gameplay_ability_create"), TEXT("Create Gameplay Ability blueprint asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("ability_name"),FSololmcpSchemaBuilder::String(TEXT("Name"))}},{TEXT("asset_path"),TEXT("ability_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=Arguments->GetStringField(TEXT("ability_name"));const FString FullPath=P/N;if(Context.Services.AssetExists(FullPath)||Context.Services.AssetExists(FullPath+TEXT(".")+N)){OutError=TEXT("Asset already exists: ")+FullPath;return false;}UPackage*Pkg=CreatePackage(*P);UBlueprint*BP=FKismetEditorUtilities::CreateBlueprint(UGameplayAbility::StaticClass(),Pkg,FName(*N),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());if(!BP){OutError=TEXT("Failed to create ability BP.");return false;}FAssetRegistryModule::AssetCreated(BP);BP->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(BP&&BP->IsA<UBlueprint>()){const FString CreatedPath=BP->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!Context.Services.AssetExists(CreatedPath)){OutError=TEXT("asset_not_persisted_after_create: ")+CreatedPath;return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),BP->GetPathName());OutSummary=FString::Printf(TEXT("Created ability '%s'"),*N);return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("gameplay_ability_list"), TEXT("List all Gameplay Ability classes/blueprints."),
			FSololmcpSchemaBuilder::Object({{TEXT("include_native"),FSololmcpSchemaBuilder::Boolean(TEXT("Native too"))}}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ bool bNative=Arguments->HasField(TEXT("include_native"))&&Arguments->GetBoolField(TEXT("include_native"));TArray<TSharedPtr<FJsonValue>>Arr;if(bNative){TArray<UClass*>Derived;GetDerivedClasses(UGameplayAbility::StaticClass(),Derived,true);for(UClass*C:Derived){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("class"),C->GetPathName());O->SetBoolField(TEXT("is_blueprint"),C->HasAnyClassFlags(CLASS_CompiledFromBlueprint));Arr.Add(MakeShared<FJsonValueObject>(O));}}FAssetRegistryModule&ARM=FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");FARFilter F;F.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());TArray<FAssetData>Assets;ARM.Get().GetAssets(F,Assets);for(auto&A:Assets){UBlueprint*BP=Cast<UBlueprint>(A.GetAsset());if(BP&&BP->GeneratedClass&&BP->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass())){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("path"),A.GetObjectPathString());O->SetStringField(TEXT("name"),A.AssetName.ToString());Arr.Add(MakeShared<FJsonValueObject>(O));}}OutStructured->SetArrayField(TEXT("abilities"),Arr);OutSummary=FString::Printf(TEXT("%d abilities"),Arr.Num());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("gameplay_effect_create"), TEXT("Create Gameplay Effect blueprint asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Path"))},{TEXT("effect_name"),FSololmcpSchemaBuilder::String(TEXT("Name"))}},{TEXT("asset_path"),TEXT("effect_name")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=Arguments->GetStringField(TEXT("effect_name"));const FString FullPath=P/N;if(Context.Services.AssetExists(FullPath)||Context.Services.AssetExists(FullPath+TEXT(".")+N)){OutError=TEXT("Asset already exists: ")+FullPath;return false;}UPackage*Pkg=CreatePackage(*P);UBlueprint*BP=FKismetEditorUtilities::CreateBlueprint(UGameplayEffect::StaticClass(),Pkg,FName(*N),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());if(!BP){OutError=TEXT("Failed.");return false;}FAssetRegistryModule::AssetCreated(BP);BP->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(BP&&BP->IsA<UBlueprint>()){const FString CreatedPath=BP->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!Context.Services.AssetExists(CreatedPath)){OutError=TEXT("asset_not_persisted_after_create: ")+CreatedPath;return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),BP->GetPathName());OutSummary=FString::Printf(TEXT("Created effect '%s'"),*N);return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("gameplay_effect_configure"), TEXT("Configure GameplayEffect CDO: duration policy and modifiers."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("BP path"))},{TEXT("duration_policy"),FSololmcpSchemaBuilder::String(TEXT("Instant/Infinite/HasDuration"))},{TEXT("duration_magnitude"),FSololmcpSchemaBuilder::Number(TEXT("Seconds"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UBlueprint*BP=Cast<UBlueprint>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError));if(!BP)return false;UGameplayEffect*GE=Cast<UGameplayEffect>(BP->GeneratedClass?BP->GeneratedClass->GetDefaultObject():nullptr);if(!GE){OutError=TEXT("Not a GE blueprint.");return false;}if(Arguments->HasField(TEXT("duration_policy"))){FString DP=Arguments->GetStringField(TEXT("duration_policy"));if(DP==TEXT("Instant"))GE->DurationPolicy=EGameplayEffectDurationType::Instant;else if(DP==TEXT("Infinite"))GE->DurationPolicy=EGameplayEffectDurationType::Infinite;else GE->DurationPolicy=EGameplayEffectDurationType::HasDuration;}BP->MarkPackageDirty();OutSummary=TEXT("Configured GE.");return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("ability_system_component_inspect"), TEXT("Inspect AbilitySystemComponent on actor."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"),FSololmcpSchemaBuilder::String(TEXT("Label/name"))}},{TEXT("actor")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate actor field before lookup.
				FString _Actor; if(!Arguments->TryGetStringField(TEXT("actor"),_Actor)||_Actor.IsEmpty()){OutError=TEXT("Missing or empty actor");return false;}
				AActor*A=Context.Services.FindActorByLabelOrName(_Actor,OutError);if(!A)return false;UAbilitySystemComponent*ASC=A->FindComponentByClass<UAbilitySystemComponent>();if(!ASC){OutError=TEXT("No ASC found.");return false;}TArray<TSharedPtr<FJsonValue>>Abs;TArray<FGameplayAbilitySpec>&Specs=ASC->GetActivatableAbilities();for(auto&S:Specs){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("ability"),S.Ability?S.Ability->GetClass()->GetName():TEXT("None"));O->SetNumberField(TEXT("level"),S.Level);O->SetBoolField(TEXT("active"),S.IsActive());Abs.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("abilities"),Abs);OutStructured->SetNumberField(TEXT("ability_count"),Abs.Num());OutSummary=FString::Printf(TEXT("ASC: %d abilities"),Abs.Num());return true; }
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("gameplay_tag_list"), TEXT("List registered Gameplay Tags."),
			FSololmcpSchemaBuilder::Object({{TEXT("filter"),FSololmcpSchemaBuilder::String(TEXT("Opt prefix"))}}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FGameplayTagContainer AllTags;UGameplayTagsManager::Get().RequestAllGameplayTags(AllTags,true);FString Filter=Arguments->HasField(TEXT("filter"))?Arguments->GetStringField(TEXT("filter")):TEXT("");TArray<TSharedPtr<FJsonValue>>Arr;for(const FGameplayTag&T:AllTags){if(Filter.IsEmpty()||T.ToString().Contains(Filter))Arr.Add(MakeShared<FJsonValueString>(T.ToString()));}OutStructured->SetArrayField(TEXT("tags"),Arr);OutSummary=FString::Printf(TEXT("%d tags"),Arr.Num());return true; }
		, nullptr
		, 15
		});

		Registry.Register({ TEXT("gameplay_tag_add"), TEXT("Add a new Gameplay Tag to the project."),
			FSololmcpSchemaBuilder::Object({{TEXT("tag"),FSololmcpSchemaBuilder::String(TEXT("Tag.Name.Here"))},{TEXT("comment"),FSololmcpSchemaBuilder::String(TEXT("Opt comment"))}},{TEXT("tag")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString TagStr=Arguments->GetStringField(TEXT("tag"));FString Comment=Arguments->HasField(TEXT("comment"))?Arguments->GetStringField(TEXT("comment")):TEXT("");IGameplayTagsEditorModule&GTE=IGameplayTagsEditorModule::Get();GTE.AddNewGameplayTagToINI(TagStr,Comment);OutStructured->SetStringField(TEXT("tag"),TagStr);OutSummary=FString::Printf(TEXT("Added tag '%s'"),*TagStr);return true;
			}
		, nullptr
		, 5
		});
#endif // GAS tools disabled for UE 5.7.4

		Registry.Register({ TEXT("gameplay_cue_list"), TEXT("List Gameplay Cue notifies in project."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FAssetRegistryModule&ARM=FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");FARFilter F;F.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());F.TagsAndValues.Add(FName("NativeParentClass"), TOptional<FString>());TArray<FAssetData>Assets;ARM.Get().GetAssets(F,Assets);TArray<TSharedPtr<FJsonValue>>Arr;for(auto&A:Assets){FString Name=A.AssetName.ToString();if(Name.Contains(TEXT("GC_"))||Name.Contains(TEXT("GameplayCue"))){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("path"),A.GetObjectPathString());O->SetStringField(TEXT("name"),Name);Arr.Add(MakeShared<FJsonValueObject>(O));}}OutStructured->SetArrayField(TEXT("cues"),Arr);OutSummary=FString::Printf(TEXT("%d cue assets"),Arr.Num());return true; }
		, nullptr
		, 15
		});

		// ===== Tier 3B: Chaos Destruction Tools (v1.7.0) =====

		#if !(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
		Registry.Register({ TEXT("geometry_collection_create"), TEXT("Create Geometry Collection asset from static mesh."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("New path"))},{TEXT("source_mesh"),FSololmcpSchemaBuilder::String(TEXT("StaticMesh path"))}},{TEXT("asset_path"),TEXT("source_mesh")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString P=Arguments->GetStringField(TEXT("asset_path"));FString N=FPackageName::GetShortName(P);UPackage*Pkg=CreatePackage(*P);/* Pre-check: refuse class collision to prevent UE check() fatal */ if(UObject*Ex=StaticFindObject(nullptr,Pkg,*N)){if(!Ex->IsA<UGeometryCollection>()){OutError=FString::Printf(TEXT("class_collision: '%s' exists as %s, refusing replace with UGeometryCollection"),*P,*Ex->GetClass()->GetName());return false;}}UGeometryCollection*GC=NewObject<UGeometryCollection>(Pkg,*N,RF_Public|RF_Standalone);if(!GC){OutError=TEXT("Failed.");return false;}FAssetRegistryModule::AssetCreated(GC);GC->MarkPackageDirty(); /* Audit round 10B: persist + verify on disk. */ if(GC&&GC->IsA<UGeometryCollection>()){const FString CreatedPath=GC->GetPathName();FString SaveErr;bool bSaved=Context.Services.SaveAsset(CreatedPath,false,SaveErr);(void)bSaved;if(!Context.Services.AssetExists(CreatedPath)){OutError=TEXT("asset_not_persisted_after_create: ")+CreatedPath;return false;}OutStructured->SetStringField(TEXT("asset_path"),CreatedPath);} OutStructured->SetStringField(TEXT("path"),GC->GetPathName());OutSummary=FString::Printf(TEXT("Created GeometryCollection '%s'"),*N);return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("geometry_collection_fracture"), TEXT("Apply Voronoi fracture to geometry collection."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("GC path"))},{TEXT("num_sites"),FSololmcpSchemaBuilder::Integer(TEXT("Voronoi sites"))},{TEXT("seed"),FSololmcpSchemaBuilder::Integer(TEXT("Random seed"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UGeometryCollection*GC=Cast<UGeometryCollection>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError));if(!GC)return false;int32 Sites=Arguments->HasField(TEXT("num_sites"))?static_cast<int32>(Arguments->GetNumberField(TEXT("num_sites"))):10;int32 Seed=Arguments->HasField(TEXT("seed"))?static_cast<int32>(Arguments->GetNumberField(TEXT("seed"))):0;OutStructured->SetBoolField(TEXT("ok"),false);OutStructured->SetStringField(TEXT("status"),TEXT("blocked"));OutStructured->SetStringField(TEXT("error_code"),TEXT("native_fracture_writer_not_implemented"));OutStructured->SetStringField(TEXT("reason_code"),TEXT("native_fracture_writer_not_implemented"));OutStructured->SetStringField(TEXT("path"),GC->GetPathName());OutStructured->SetNumberField(TEXT("sites"),Sites);OutStructured->SetNumberField(TEXT("seed"),Seed);OutStructured->SetBoolField(TEXT("mutation_attempted"),false);OutStructured->SetBoolField(TEXT("asset_modified"),false);OutError=TEXT("geometry_collection_fracture is blocked because the native Chaos fracture writer is not implemented. Parameter storage is not a fracture operation; use the forthcoming fracture_voronoi_apply writer.");OutStructured->SetStringField(TEXT("message"),OutError);return false;
			}
		, nullptr
		, 5
		});
		#endif

		Registry.Register({ TEXT("geometry_collection_inspect"), TEXT("Inspect Geometry Collection hierarchy, collision, materials, visibility, damage, simulation and revision readback."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("GC path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				// A2 CH-03: frozen hierarchy/collision/material/visibility/damage/simulation/revision readback.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UGeometryCollection*GC=Cast<UGeometryCollection>(Context.Services.LoadAsset(_AP,OutError));if(!GC){if(OutError.IsEmpty()){OutError=TEXT("Failed to load geometry collection asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}
				// A2 fix 2026-08-05: const accessor so FindAttribute uses the const overload, which returns nullptr on type mismatch instead of the non-const ModifyTypedPtr assertion (ManagedArrayCollection.h:756) that crashed the editor.
				const FGeometryCollection* Geom = GC->GetGeometryCollection().Get();
				OutStructured->SetStringField(TEXT("path"),GC->GetPathName());
				OutStructured->SetStringField(TEXT("asset_class"),GC->GetClass()->GetPathName());
				if(!Geom){OutStructured->SetNumberField(TEXT("num_transforms"),0);OutStructured->SetBoolField(TEXT("simulation_data_dirty"),GC->IsSimulationDataDirty());OutSummary=TEXT("GC: empty collection");return true;}
				const int32 NumTransforms=Geom->NumElements(FGeometryCollection::TransformGroup);
				OutStructured->SetNumberField(TEXT("num_transforms"),NumTransforms);
				OutStructured->SetNumberField(TEXT("transform_count"),NumTransforms);
				OutStructured->SetNumberField(TEXT("geometry_count"),Geom->NumElements(FGeometryCollection::GeometryGroup));
				OutStructured->SetNumberField(TEXT("vertex_count"),Geom->NumElements(FGeometryCollection::VerticesGroup));
				OutStructured->SetNumberField(TEXT("face_count"),Geom->NumElements(FGeometryCollection::FacesGroup));
				int32 RootCount=0,ClusterCount=0,LeafCount=0,MaxLevel=0,VisibleCount=0;
				if(const TManagedArray<int32>* Levels=Geom->FindAttribute<int32>(TEXT("Level"),FGeometryCollection::TransformGroup)){for(int32 i=0;i<Levels->Num();++i){const int32 L=(*Levels)[i];MaxLevel=FMath::Max(MaxLevel,L);if(L==0)++RootCount;else ++ClusterCount;}}
				if(const TManagedArray<TSet<int32>>* Children=Geom->FindAttribute<TSet<int32>>(TEXT("Children"),FGeometryCollection::TransformGroup)){for(int32 i=0;i<Children->Num();++i){if((*Children)[i].Num()==0)++LeafCount;}}
				if(const TManagedArray<bool>* Visibles=Geom->FindAttribute<bool>(TEXT("Visible"),FGeometryCollection::FacesGroup)){for(int32 i=0;i<Visibles->Num();++i){if((*Visibles)[i])++VisibleCount;}}
				OutStructured->SetNumberField(TEXT("root_count"),RootCount);
				OutStructured->SetNumberField(TEXT("cluster_count"),ClusterCount);
				OutStructured->SetNumberField(TEXT("leaf_count"),LeafCount);
				OutStructured->SetNumberField(TEXT("max_level"),MaxLevel);
				OutStructured->SetNumberField(TEXT("visible_count"),VisibleCount);
				// A2 fix 2026-08-05: Visible lives on the Faces group in UE 5.8 (see
				// FGeometryCollection.h); hidden_count therefore counts non-visible faces.
				OutStructured->SetNumberField(TEXT("hidden_count"),Geom->NumElements(FGeometryCollection::FacesGroup)-VisibleCount);
				OutStructured->SetBoolField(TEXT("has_visible_geometry"),GC->HasVisibleGeometry());
				if(!GC->SizeSpecificData.IsEmpty()){const FGeometryCollectionSizeSpecificData& SizeData=GC->SizeSpecificData[0];OutStructured->SetNumberField(TEXT("size_specific_count"),GC->SizeSpecificData.Num());if(!SizeData.CollisionShapes.IsEmpty()){const FGeometryCollectionCollisionTypeData& CollisionData=SizeData.CollisionShapes[0];OutStructured->SetStringField(TEXT("collision_type"),StaticEnum<ECollisionTypeEnum>()->GetNameStringByValue((int64)CollisionData.CollisionType));OutStructured->SetStringField(TEXT("implicit_type"),StaticEnum<EImplicitTypeEnum>()->GetNameStringByValue((int64)CollisionData.ImplicitType));OutStructured->SetNumberField(TEXT("collision_shape_count"),SizeData.CollisionShapes.Num());}else{OutStructured->SetNumberField(TEXT("collision_shape_count"),0);}}else{OutStructured->SetNumberField(TEXT("size_specific_count"),0);}
				OutStructured->SetNumberField(TEXT("materials_count"),GC->Materials.Num());
				TArray<TSharedPtr<FJsonValue>>MaterialNames;for(const TObjectPtr<UMaterialInterface>&M:GC->Materials){MaterialNames.Add(MakeShared<FJsonValueString>(M?M->GetPathName():TEXT("(null)")));}OutStructured->SetArrayField(TEXT("materials"),MaterialNames);
				TArray<TSharedPtr<FJsonValue>>DamageThresholds;for(float T:GC->DamageThreshold)DamageThresholds.Add(MakeShared<FJsonValueNumber>(T));OutStructured->SetArrayField(TEXT("damage_thresholds"),DamageThresholds);
				OutStructured->SetBoolField(TEXT("use_size_specific_damage_threshold"),GC->bUseSizeSpecificDamageThreshold);
				#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				OutStructured->SetBoolField(TEXT("use_material_damage_modifiers"),GC->bUseMaterialDamageModifiers);
#else
				// UGeometryCollection::bUseMaterialDamageModifiers is 5.4+.
				OutStructured->SetBoolField(TEXT("use_material_damage_modifiers"),false);
#endif
				OutStructured->SetBoolField(TEXT("damage_propagation_enabled"),GC->DamagePropagationData.bEnabled);
				OutStructured->SetNumberField(TEXT("damage_propagation_break_factor"),GC->DamagePropagationData.BreakDamagePropagationFactor);
				OutStructured->SetNumberField(TEXT("damage_propagation_shock_factor"),GC->DamagePropagationData.ShockDamagePropagationFactor);
				UEnum* ConnectionTypeEnum=FindObject<UEnum>(nullptr,TEXT("/Script/ChaosSolverEngine.EClusterConnectionTypeEnum"));const FString ConnectionTypeName=ConnectionTypeEnum?ConnectionTypeEnum->GetNameStringByValue((int64)GC->ClusterConnectionType):FString();OutStructured->SetStringField(TEXT("cluster_connection_type"),ConnectionTypeName.IsEmpty()?FString::Printf(TEXT("(raw %d)"),(int32)GC->ClusterConnectionType):ConnectionTypeName);
				OutStructured->SetBoolField(TEXT("simulation_data_dirty"),GC->IsSimulationDataDirty());
				OutStructured->SetStringField(TEXT("id_guid"),GC->GetIdGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
				OutStructured->SetStringField(TEXT("state_guid"),GC->GetStateGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
				FAssetRegistryModule&ARM=FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");const FAssetData AssetData=ARM.Get().GetAssetByObjectPath(FSoftObjectPath(GC->GetPathName()));OutStructured->SetStringField(TEXT("asset_registry_object_path"),AssetData.GetObjectPathString());OutStructured->SetStringField(TEXT("asset_registry_package"),AssetData.PackageName.ToString());
				OutSummary=FString::Printf(TEXT("GC: %d transforms, %d roots, %d clusters"),NumTransforms,RootCount,ClusterCount);return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("destruction_field_create"), TEXT("Author native destruction field (strain/force/velocity/sleep/disable) on a spawned FieldSystem actor with target binding, transaction, level save, runtime readback and cleanup receipt."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("field_effect"),FSololmcpSchemaBuilder::String(TEXT("strain/force/velocity/sleep/disable"),{TEXT("strain"),TEXT("force"),TEXT("velocity"),TEXT("sleep"),TEXT("disable")},1,16)},
				{TEXT("field_type"),FSololmcpSchemaBuilder::String(TEXT("Legacy actor variant name; only FieldSystemActor exists in UE 5.8"))},
				{TEXT("location"),VectorSchema()},
				{TEXT("direction"),VectorSchema()},
				{TEXT("target"),FSololmcpSchemaBuilder::String(TEXT("Optional actor label; field center binds to its location"))},
				{TEXT("label"),FSololmcpSchemaBuilder::String(TEXT("Opt spawned actor label"))},
				{TEXT("magnitude"),FSololmcpSchemaBuilder::Number(TEXT("Field magnitude (defaults: strain 1 / force 1e5 / velocity 1e2 / disable -1 / sleep 0)"))},
				{TEXT("radius"),FSololmcpSchemaBuilder::Number(TEXT("Falloff radius in cm (default 200)"))},
				{TEXT("iterations"),FSololmcpSchemaBuilder::Integer(TEXT("Cluster levels for strain effect (default 5)"))},
				{TEXT("enabled"),FSololmcpSchemaBuilder::Boolean(TEXT("Enable field dispatch (default true)"))}
			},{TEXT("field_effect")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				// A2 CH-04: native strain/force/velocity/sleep/disable field authoring. All field calls are
				// dispatched through UObject reflection (FindFunction/ProcessEvent over a generic UActorComponent
				// base) so no direct FieldSystemEngine link dependency is added to this module.
				UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				if (!W) { OutError = TEXT("No world."); return false; }
				FString Effect = TEXT("strain");
				if (Arguments->HasField(TEXT("field_effect"))) Effect = Arguments->GetStringField(TEXT("field_effect"));
				if (Effect != TEXT("strain") && Effect != TEXT("force") && Effect != TEXT("velocity") && Effect != TEXT("sleep") && Effect != TEXT("disable"))
				{
					OutError = TEXT("Invalid field_effect. Must be strain/force/velocity/sleep/disable."); return false;
				}
				FString FT = Arguments->HasField(TEXT("field_type")) ? Arguments->GetStringField(TEXT("field_type")) : TEXT("RadialFalloff");
				bool bEnabled = !Arguments->HasField(TEXT("enabled")) || Arguments->GetBoolField(TEXT("enabled"));
				FVector Loc = FVector::ZeroVector;
				if (Arguments->HasField(TEXT("location"))) { auto O = Arguments->GetObjectField(TEXT("location")); Loc = FVector(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z"))); }
				FString TargetLabel;
				if (Arguments->HasField(TEXT("target")))
				{
					TargetLabel = Arguments->GetStringField(TEXT("target"));
					AActor* Target = Context.Services.FindActorByLabelOrName(TargetLabel, OutError);
					if (!Target) { OutError = TEXT("Target actor not found: ") + TargetLabel; return false; }
					Loc = Target->GetActorLocation();
				}
				// per-effect defaults when not provided
				float Magnitude = Effect == TEXT("force") ? 100000.f : (Effect == TEXT("velocity") ? 100.f : (Effect == TEXT("disable") ? -1.f : (Effect == TEXT("sleep") ? 0.f : 1.f)));
				float Radius = 200.f;
				FVector Dir = FVector(0.f, 0.f, 1.f);
				int32 Iterations = 5;
				if (Arguments->HasField(TEXT("magnitude"))) Magnitude = static_cast<float>(Arguments->GetNumberField(TEXT("magnitude")));
				if (Arguments->HasField(TEXT("radius"))) Radius = static_cast<float>(Arguments->GetNumberField(TEXT("radius")));
				if (Arguments->HasField(TEXT("iterations"))) Iterations = static_cast<int32>(Arguments->GetNumberField(TEXT("iterations")));
				if (Arguments->HasField(TEXT("direction"))) { auto O = Arguments->GetObjectField(TEXT("direction")); Dir = FVector(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z"))); }
				// spawn the FieldSystem actor (legacy variant class names fall back to the generic actor)
				FString ClassName = FString::Printf(TEXT("FieldSystem%sActor"), *FT);
				UClass* Cls = FindObject<UClass>(nullptr, *ClassName);
				if (!Cls) Cls = FindObject<UClass>(nullptr, TEXT("/Script/FieldSystemEngine.FieldSystemActor"));
				if (!Cls) Cls = FindObject<UClass>(nullptr, TEXT("FieldSystemActor"));
				if (!Cls) { OutError = TEXT("FieldSystemActor class not found (FieldSystem plugin disabled?)."); return false; }
				AActor* A = W->SpawnActor(Cls, &Loc);
				if (!A) { OutError = TEXT("Spawn failed."); return false; }
				if (Arguments->HasField(TEXT("label"))) A->SetActorLabel(Arguments->GetStringField(TEXT("label")));
				// locate the FieldSystemComponent through the generic UActorComponent base to avoid
				// a direct FieldSystemEngine link dependency
				UActorComponent* FieldComp = nullptr;
				{ TInlineComponentArray<UActorComponent*> Cs; A->GetComponents(Cs); for (UActorComponent* C : Cs) { if (C->GetClass()->GetName() == TEXT("FieldSystemComponent")) { FieldComp = C; break; } } }
				if (!FieldComp) { OutError = TEXT("No FieldSystemComponent on spawned actor."); A->Destroy(); return false; }
				// transaction + modify for undo support
				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DestructionFieldCreate", "SOMOLMCP Destruction Field Create"));
				A->Modify();
				FieldComp->Modify();
				// reflection-based invocation of the BlueprintCallable field functions
				auto InvokeFieldFunctionReflect = [](UObject* Comp, const FString& FuncName, bool bOn, const FVector& Pos, const FVector& DirIn, float Mag, float R, int32 Iter, FString& Err) -> bool
				{
					UFunction* Fn = Comp ? Comp->FindFunction(*FuncName) : nullptr;
					if (!Fn) { Err = TEXT("Field function not found: ") + FuncName; return false; }
					FStructOnScope Params(Fn);
					for (TFieldIterator<FProperty> It(Fn); It; ++It)
					{
						FProperty* Prop = *It;
						const FString Name = Prop->GetName();
						void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Params.GetStructMemory());
						if (Name == TEXT("Enabled") && Prop->IsA<FBoolProperty>()) { CastFieldChecked<FBoolProperty>(Prop)->SetPropertyValue(ValuePtr, bOn); }
						else if (Name == TEXT("Position") && Prop->IsA<FStructProperty>()) { CastFieldChecked<FStructProperty>(Prop)->CopyCompleteValue(ValuePtr, &Pos); }
						else if (Name == TEXT("Direction") && Prop->IsA<FStructProperty>()) { CastFieldChecked<FStructProperty>(Prop)->CopyCompleteValue(ValuePtr, &DirIn); }
						else if (Name == TEXT("Magnitude") && Prop->IsA<FNumericProperty>()) { CastFieldChecked<FNumericProperty>(Prop)->SetFloatingPointPropertyValue(ValuePtr, Mag); }
						else if (Name == TEXT("Radius") && Prop->IsA<FNumericProperty>()) { CastFieldChecked<FNumericProperty>(Prop)->SetFloatingPointPropertyValue(ValuePtr, R); }
						else if (Name == TEXT("Iterations") && Prop->IsA<FNumericProperty>()) { CastFieldChecked<FNumericProperty>(Prop)->SetIntPropertyValue(ValuePtr, static_cast<int64>(Iter)); }
					}
					Comp->ProcessEvent(Fn, Params.GetStructMemory());
					return true;
				};
				// disable effect: ApplyPhysicsField with a reflected URadialFalloff node and Field_DisableThreshold target
				auto ApplyDisableFieldReflect = [](UObject* Comp, const FVector& Pos, float R, float Mag, bool bOn, FString& Err) -> bool
				{
					UFunction* Fn = Comp ? Comp->FindFunction(TEXT("ApplyPhysicsField")) : nullptr;
					if (!Fn) { Err = TEXT("ApplyPhysicsField not found."); return false; }
					UEnum* PhysTypeEnum = FindObject<UEnum>(nullptr, TEXT("/Script/Chaos.EFieldPhysicsType"));
					if (!PhysTypeEnum) PhysTypeEnum = FindObject<UEnum>(nullptr, TEXT("/Script/FieldSystemCore.EFieldPhysicsType"));
					if (!PhysTypeEnum) { Err = TEXT("EFieldPhysicsType enum not found."); return false; }
					const int64 DisableThreshold = PhysTypeEnum->GetValueByNameString(TEXT("Field_DisableThreshold"));
					if (DisableThreshold == INDEX_NONE) { Err = TEXT("Field_DisableThreshold value not found."); return false; }
					UClass* FalloffClass = FindObject<UClass>(nullptr, TEXT("/Script/FieldSystemEngine.RadialFalloff"));
					if (!FalloffClass) { Err = TEXT("RadialFalloff class not found."); return false; }
					UObject* Falloff = NewObject<UObject>(GetTransientPackage(), FalloffClass);
					if (!Falloff) { Err = TEXT("RadialFalloff construction failed."); return false; }
					if (FProperty* MagProp = FalloffClass->FindPropertyByName(TEXT("Magnitude"))) { if (FNumericProperty* Num = CastField<FNumericProperty>(MagProp)) Num->SetFloatingPointPropertyValue(MagProp->ContainerPtrToValuePtr<void>(Falloff), Mag); }
					if (FProperty* RProp = FalloffClass->FindPropertyByName(TEXT("Radius"))) { if (FNumericProperty* Num = CastField<FNumericProperty>(RProp)) Num->SetFloatingPointPropertyValue(RProp->ContainerPtrToValuePtr<void>(Falloff), R); }
					if (FProperty* PProp = FalloffClass->FindPropertyByName(TEXT("Position"))) { if (FStructProperty* SP = CastField<FStructProperty>(PProp)) SP->CopyCompleteValue(PProp->ContainerPtrToValuePtr<void>(Falloff), &Pos); }
					FStructOnScope Params(Fn);
					for (TFieldIterator<FProperty> It(Fn); It; ++It)
					{
						FProperty* Prop = *It;
						const FString Name = Prop->GetName();
						void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Params.GetStructMemory());
						if (Name == TEXT("Enabled") && Prop->IsA<FBoolProperty>()) { CastFieldChecked<FBoolProperty>(Prop)->SetPropertyValue(ValuePtr, bOn); }
						else if (Name == TEXT("Target") && (Prop->IsA<FEnumProperty>() || Prop->IsA<FByteProperty>()))
						{
							if (FEnumProperty* EP = CastField<FEnumProperty>(Prop)) { EP->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, DisableThreshold); }
							else { CastFieldChecked<FByteProperty>(Prop)->SetIntPropertyValue(ValuePtr, DisableThreshold); }
						}
						else if (Name == TEXT("Field") && Prop->IsA<FObjectPropertyBase>()) { CastFieldChecked<FObjectPropertyBase>(Prop)->SetObjectPropertyValue(ValuePtr, Falloff); }
					}
					Comp->ProcessEvent(Fn, Params.GetStructMemory());
					return true;
				};
				// dispatch the requested effect
				FString FieldFunction;
				bool bDispatched = false;
				if (Effect == TEXT("disable"))
				{
					FieldFunction = TEXT("ApplyPhysicsField");
					bDispatched = ApplyDisableFieldReflect(FieldComp, Loc, Radius, Magnitude, bEnabled, OutError);
				}
				else
				{
					if (Effect == TEXT("strain")) FieldFunction = TEXT("ApplyStrainField");
					else if (Effect == TEXT("force")) FieldFunction = TEXT("ApplyLinearForce");
					else if (Effect == TEXT("velocity")) FieldFunction = TEXT("ApplyUniformVectorFalloffForce");
					else if (Effect == TEXT("sleep")) FieldFunction = TEXT("ApplyStayDynamicField");
					bDispatched = InvokeFieldFunctionReflect(FieldComp, FieldFunction, bEnabled, Loc, Dir, Magnitude, Radius, Iterations, OutError);
				}
				if (!bDispatched) { A->Destroy(); if (OutError.IsEmpty()) OutError = TEXT("Field dispatch failed."); return false; }
				// persist the level and read back the spawned actor
				FEditorFileUtils::SaveCurrentLevel();
				bool bReadback = false;
				for (TActorIterator<AActor> It(W); It; ++It) { if ((*It) == A) { bReadback = true; break; } }
				// cleanup receipt
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutStructured->SetStringField(TEXT("status"), TEXT("ok"));
				OutStructured->SetStringField(TEXT("receipt_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
				OutStructured->SetStringField(TEXT("actor"), A->GetActorLabel());
				OutStructured->SetStringField(TEXT("class"), A->GetClass()->GetName());
				OutStructured->SetStringField(TEXT("field_effect"), Effect);
				OutStructured->SetStringField(TEXT("field_function"), FieldFunction);
				OutStructured->SetBoolField(TEXT("mutation_applied"), true);
				OutStructured->SetBoolField(TEXT("readback_verified"), bReadback);
				OutStructured->SetBoolField(TEXT("level_saved"), true);
				OutStructured->SetBoolField(TEXT("transaction_scoped"), true);
				OutStructured->SetNumberField(TEXT("magnitude"), Magnitude);
				OutStructured->SetNumberField(TEXT("radius"), Radius);
				OutStructured->SetNumberField(TEXT("iterations"), Iterations);
				OutStructured->SetBoolField(TEXT("enabled"), bEnabled);
				OutStructured->SetStringField(TEXT("location"), Loc.ToString());
				OutStructured->SetStringField(TEXT("direction"), Dir.ToString());
				if (!TargetLabel.IsEmpty()) OutStructured->SetStringField(TEXT("target"), TargetLabel);
				OutStructured->SetStringField(TEXT("component"), FieldComp->GetName());
				OutStructured->SetStringField(TEXT("component_class"), FieldComp->GetClass()->GetName());
				OutStructured->SetBoolField(TEXT("component_registered"), FieldComp->IsRegistered());
				// A2 fix 2026-08-05: UActorComponent::IsActive() requires the component tick to be
				// enabled, and UFieldSystemComponent was constructed with bEnableInEditorWorlds=false,
				// so IsActive() reads false in the editor world by engine design. The raw engine value
				// is still reported; component_tick_enabled and editor_world_disabled document why.
				OutStructured->SetBoolField(TEXT("component_active"), FieldComp->IsActive());
				OutStructured->SetBoolField(TEXT("component_tick_enabled"), FieldComp->IsComponentTickEnabled());
				OutStructured->SetBoolField(TEXT("editor_world_disabled"), !W->IsGameWorld());
				TArray<TSharedPtr<FJsonValue>> TB;
				TB.Add(MakeShared<FJsonValueString>(TEXT("field_command_dispatch_queues_on_the_physics_thread; component-level readback verifies the spawned actor, component registration and level persistence, not the pending physics command queue")));
				TB.Add(MakeShared<FJsonValueString>(TEXT("legacy field_type variants (FieldSystemRadialFalloffActor etc.) do not exist in UE 5.8; the implementation falls back to the generic FieldSystemActor")));
				TB.Add(MakeShared<FJsonValueString>(TEXT("EFieldPhysicsType is reflected from /Script/Chaos.EFieldPhysicsType; Field_DisableThreshold drives the disable effect")));
				TB.Add(MakeShared<FJsonValueString>(TEXT("UFieldSystemComponent is constructed with bEnableInEditorWorlds=false in UE 5.8, so IsActive()/tick stay false in the editor world by engine design; field commands are queued for PIE/runtime physics dispatch")));
				OutStructured->SetArrayField(TEXT("truth_boundary"), TB);
				OutSummary = FString::Printf(TEXT("Spawned %s '%s' with %s field @ %s"), *A->GetClass()->GetName(), *A->GetActorLabel(), *Effect, *Loc.ToString());
				return true;
			}
		, nullptr
		, 5
		});

		// ===== Tier 3B: Water Tools (v1.7.0) =====

		Registry.Register({ TEXT("water_body_list"), TEXT("List water body actors in level."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){OutError=TEXT("No world.");return false;}TArray<TSharedPtr<FJsonValue>>Arr;for(TActorIterator<AActor>It(W);It;++It){if((*It)->GetClass()->GetName().Contains(TEXT("WaterBody"))){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("actor"),(*It)->GetActorLabel());O->SetStringField(TEXT("class"),(*It)->GetClass()->GetName());O->SetStringField(TEXT("location"),(*It)->GetActorLocation().ToString());Arr.Add(MakeShared<FJsonValueObject>(O));}}OutStructured->SetArrayField(TEXT("water_bodies"),Arr);OutSummary=FString::Printf(TEXT("%d water bodies"),Arr.Num());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("water_body_create"), TEXT("Spawn a water body actor by type."),
			FSololmcpSchemaBuilder::Object({{TEXT("water_type"),FSololmcpSchemaBuilder::String(TEXT("WaterBodyRiver/Lake/Ocean/Custom"))},{TEXT("location"),VectorSchema()},{TEXT("label"),FSololmcpSchemaBuilder::String(TEXT("Opt label"))}},{TEXT("water_type")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){OutError=TEXT("No world.");return false;}FString WT=Arguments->GetStringField(TEXT("water_type"));UClass*Cls=FindObject<UClass>(nullptr,*WT);if(!Cls){FString Full=FString::Printf(TEXT("A%s"),*WT);Cls=FindObject<UClass>(nullptr,*Full);}if(!Cls){OutError=FString::Printf(TEXT("Class '%s' not found. Ensure Water plugin is enabled."),*WT);return false;}FVector Loc=FVector::ZeroVector;if(Arguments->HasField(TEXT("location"))){auto O=Arguments->GetObjectField(TEXT("location"));Loc=FVector(O->GetNumberField(TEXT("x")),O->GetNumberField(TEXT("y")),O->GetNumberField(TEXT("z")));}AActor*A=W->SpawnActor(Cls,&Loc);if(!A){OutError=TEXT("Spawn failed.");return false;}if(Arguments->HasField(TEXT("label")))A->SetActorLabel(Arguments->GetStringField(TEXT("label")));OutStructured->SetStringField(TEXT("actor"),A->GetActorLabel());OutStructured->SetStringField(TEXT("class"),A->GetClass()->GetName());OutSummary=FString::Printf(TEXT("Spawned %s"),*A->GetActorLabel());return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("water_zone_inspect"), TEXT("Inspect water zone actor properties."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"),FSololmcpSchemaBuilder::String(TEXT("Label/name"))}},{TEXT("actor")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ AActor*A=Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("actor")),OutError);if(!A)return false;if(!A->GetClass()->GetName().Contains(TEXT("Water"))){OutError=TEXT("Not a water actor.");return false;}OutStructured->SetStringField(TEXT("actor"),A->GetActorLabel());OutStructured->SetStringField(TEXT("class"),A->GetClass()->GetName());OutStructured->SetStringField(TEXT("location"),A->GetActorLocation().ToString());TArray<TSharedPtr<FJsonValue>>Comps;TInlineComponentArray<UActorComponent*>Cs;A->GetComponents(Cs);for(UActorComponent*C:Cs){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),C->GetName());O->SetStringField(TEXT("class"),C->GetClass()->GetName());Comps.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("components"),Comps);OutSummary=FString::Printf(TEXT("%s: %d components"),*A->GetActorLabel(),Comps.Num());return true; }
		, nullptr
		, 5
		});

		// ===== Tier 3B: Texture Tools (v1.7.0) =====

		Registry.Register({ TEXT("texture_inspect"), TEXT("Inspect texture properties: size, format, compression, LOD."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Texture path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UTexture2D*T=Cast<UTexture2D>(Context.Services.LoadAsset(_AP,OutError));if(!T){if(OutError.IsEmpty()){OutError=TEXT("Failed to load texture asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}OutStructured->SetStringField(TEXT("path"),T->GetPathName());OutStructured->SetNumberField(TEXT("width"),T->GetSizeX());OutStructured->SetNumberField(TEXT("height"),T->GetSizeY());OutStructured->SetStringField(TEXT("pixel_format"),GPixelFormats[T->GetPixelFormat()].Name);OutStructured->SetStringField(TEXT("compression"),StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(static_cast<int64>(T->CompressionSettings)));OutStructured->SetStringField(TEXT("texture_group"),StaticEnum<TextureGroup>()->GetNameStringByValue(static_cast<int64>(T->LODGroup)));OutStructured->SetNumberField(TEXT("num_mips"),T->GetNumMips());OutStructured->SetBoolField(TEXT("srgb"),T->SRGB);OutSummary=FString::Printf(TEXT("%s %dx%d %s"),*T->GetName(),T->GetSizeX(),T->GetSizeY(),GPixelFormats[T->GetPixelFormat()].Name);return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("texture_resize"), TEXT("Change texture MaxTextureSize (power-of-2 limit)."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Texture path"))},{TEXT("max_size"),FSololmcpSchemaBuilder::Integer(TEXT("Power of 2"))}},{TEXT("asset_path"),TEXT("max_size")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UTexture2D*T=Cast<UTexture2D>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError));if(!T)return false;int32 MaxSz=static_cast<int32>(Arguments->GetNumberField(TEXT("max_size")));T->MaxTextureSize=MaxSz;T->UpdateResource();T->MarkPackageDirty();OutStructured->SetStringField(TEXT("path"),T->GetPathName());OutStructured->SetNumberField(TEXT("max_size"),MaxSz);OutSummary=FString::Printf(TEXT("MaxTextureSize=%d for '%s'"),MaxSz,*T->GetName());return true;
			}
		, nullptr
		, 5
		});

		// UE 5.7 fix: FindNameStringByValue sig changed → only use GetValueByNameString
		Registry.Register({ TEXT("texture_settings_configure"), TEXT("Configure texture compression, LOD group, sRGB, filter."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Texture path"))},{TEXT("compression"),FSololmcpSchemaBuilder::String(TEXT("TC_Default etc"))},{TEXT("lod_group"),FSololmcpSchemaBuilder::String(TEXT("TEXTUREGROUP_World etc"))},{TEXT("srgb"),FSololmcpSchemaBuilder::Boolean(TEXT("sRGB"))},{TEXT("filter"),FSololmcpSchemaBuilder::String(TEXT("TF_Nearest/Bilinear/Trilinear"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UTexture2D*T=Cast<UTexture2D>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError));if(!T)return false;int32 Changes=0;if(Arguments->HasField(TEXT("compression"))){FString CS=Arguments->GetStringField(TEXT("compression"));int64 V=StaticEnum<TextureCompressionSettings>()->GetValueByNameString(CS);if(V!=INDEX_NONE){T->CompressionSettings=(TextureCompressionSettings)V;Changes++;}}if(Arguments->HasField(TEXT("lod_group"))){FString LG=Arguments->GetStringField(TEXT("lod_group"));int64 V=StaticEnum<TextureGroup>()->GetValueByNameString(LG);if(V!=INDEX_NONE){T->LODGroup=(TextureGroup)V;Changes++;}}if(Arguments->HasField(TEXT("srgb"))){T->SRGB=Arguments->GetBoolField(TEXT("srgb"));Changes++;}if(Arguments->HasField(TEXT("filter"))){FString FI=Arguments->GetStringField(TEXT("filter"));if(FI==TEXT("TF_Nearest"))T->Filter=TF_Nearest;else if(FI==TEXT("TF_Bilinear"))T->Filter=TF_Bilinear;else T->Filter=TF_Trilinear;Changes++;}T->UpdateResource();T->MarkPackageDirty();OutSummary=FString::Printf(TEXT("Configured %d settings on '%s'"),Changes,*T->GetName());return true;
			}
		, nullptr
		, 5
		});

		// ===== Tier 4: Perception Enhancement Tools (v1.7.0) =====

		Registry.Register({ TEXT("actor_component_list"), TEXT("List all components on an actor with class and properties."),
			FSololmcpSchemaBuilder::Object({{TEXT("actor"),FSololmcpSchemaBuilder::String(TEXT("Label/name"))}},{TEXT("actor")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate actor field before lookup.
				FString _Actor; if(!Arguments->TryGetStringField(TEXT("actor"),_Actor)||_Actor.IsEmpty()){OutError=TEXT("Missing or empty actor");return false;}
				AActor*A=Context.Services.FindActorByLabelOrName(_Actor,OutError);if(!A)return false;TArray<TSharedPtr<FJsonValue>>Arr;TInlineComponentArray<UActorComponent*>Cs;A->GetComponents(Cs);for(UActorComponent*C:Cs){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),C->GetName());O->SetStringField(TEXT("class"),C->GetClass()->GetName());O->SetBoolField(TEXT("active"),C->IsActive());if(USceneComponent*SC=Cast<USceneComponent>(C)){O->SetStringField(TEXT("location"),SC->GetRelativeLocation().ToString());O->SetStringField(TEXT("rotation"),SC->GetRelativeRotation().ToString());O->SetStringField(TEXT("scale"),SC->GetRelativeScale3D().ToString());}Arr.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("components"),Arr);OutSummary=FString::Printf(TEXT("%s: %d components"),*A->GetActorLabel(),Arr.Num());return true; }
		, nullptr
		, 5
		});

		// UE 5.7 fix: Added proper includes for SimpleConstructionScript/USCS_Node
		Registry.Register({ TEXT("blueprint_inspect_summary"), TEXT("Summary of Blueprint: parent class, graphs, variables, components."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("BP path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UBlueprint*BP=Cast<UBlueprint>(Context.Services.LoadAsset(Arguments->GetStringField(TEXT("asset_path")),OutError));if(!BP)return false;OutStructured->SetStringField(TEXT("path"),BP->GetPathName());OutStructured->SetStringField(TEXT("parent_class"),BP->ParentClass?BP->ParentClass->GetName():TEXT("None"));OutStructured->SetStringField(TEXT("type"),StaticEnum<EBlueprintType>()->GetNameStringByValue(static_cast<int64>(BP->BlueprintType)));TArray<TSharedPtr<FJsonValue>>Vars;for(FBPVariableDescription&V:BP->NewVariables){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),V.VarName.ToString());O->SetStringField(TEXT("type"),V.VarType.PinCategory.ToString());Vars.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("variables"),Vars);OutStructured->SetNumberField(TEXT("variable_count"),Vars.Num());TArray<TSharedPtr<FJsonValue>>Graphs;for(UEdGraph*G:BP->UbergraphPages){Graphs.Add(MakeShared<FJsonValueString>(G->GetName()));}for(UEdGraph*G:BP->FunctionGraphs){Graphs.Add(MakeShared<FJsonValueString>(G->GetName()));}OutStructured->SetArrayField(TEXT("graphs"),Graphs);OutStructured->SetNumberField(TEXT("graph_count"),Graphs.Num());if(BP->SimpleConstructionScript){TArray<TSharedPtr<FJsonValue>>SCSNodes;for(USCS_Node*N:BP->SimpleConstructionScript->GetAllNodes()){SCSNodes.Add(MakeShared<FJsonValueString>(N->GetVariableName().ToString()+TEXT(" (")+N->ComponentClass->GetName()+TEXT(")")));}OutStructured->SetArrayField(TEXT("scs_components"),SCSNodes);}OutSummary=FString::Printf(TEXT("BP '%s': %d vars, %d graphs"),*BP->GetName(),Vars.Num(),Graphs.Num());return true; }
		, nullptr
		, 10
		});

		// UE 5.7 verified: GetNameStringByValue(int64) still works
		Registry.Register({ TEXT("material_inspect"), TEXT("Inspect material: parameters, textures, blend mode, shading model."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Material path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				// Audit round 9 (group C): round 4's IsValidObjectPath() guard rejects
				// package-form paths like "/Game/Foo/Bar" (no '.AssetName'), so callers
				// that pass the package path never reach the round 8 LoadAsset
				// normalization. Accept either package-form or object-form by
				// only requiring that the path begin with '/' and look syntactically
				// non-empty. LoadAsset itself does the real resolve + cast diagnostics.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path");return false;}
				UMaterialInterface* MI = Cast<UMaterialInterface>(Context.Services.LoadAsset(_AP, OutError));
				if (!MI) { if (OutError.IsEmpty()) { OutError = TEXT("Asset is not a material interface."); } return false; }
				OutStructured->SetStringField(TEXT("path"), MI->GetPathName());
				OutStructured->SetStringField(TEXT("class"), MI->GetClass()->GetName());
				OutStructured->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue(static_cast<int64>(MI->GetBlendMode())));
				OutStructured->SetStringField(TEXT("shading_model"), StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(static_cast<int64>(MI->GetShadingModels().GetFirstShadingModel())));
				OutStructured->SetBoolField(TEXT("two_sided"), MI->IsTwoSided());
				TArray<TSharedPtr<FJsonValue>> Params;
				TArray<FMaterialParameterInfo> ScalarInfos; TArray<FGuid> ScalarGuids;
				MI->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);
				for (int32 i = 0; i < ScalarInfos.Num(); i++)
				{
					float V = 0; MI->GetScalarParameterValue(ScalarInfos[i], V);
					TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("name"), ScalarInfos[i].Name.ToString());
					O->SetStringField(TEXT("type"), TEXT("Scalar"));
					O->SetNumberField(TEXT("value"), V);
					Params.Add(MakeShared<FJsonValueObject>(O));
				}
				TArray<FMaterialParameterInfo> VecInfos; TArray<FGuid> VecGuids;
				MI->GetAllVectorParameterInfo(VecInfos, VecGuids);
				for (int32 i = 0; i < VecInfos.Num(); i++)
				{
					FLinearColor V; MI->GetVectorParameterValue(VecInfos[i], V);
					TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("name"), VecInfos[i].Name.ToString());
					O->SetStringField(TEXT("type"), TEXT("Vector"));
					O->SetStringField(TEXT("value"), V.ToString());
					Params.Add(MakeShared<FJsonValueObject>(O));
				}
				TArray<FMaterialParameterInfo> TexInfos; TArray<FGuid> TexGuids;
				MI->GetAllTextureParameterInfo(TexInfos, TexGuids);
				for (int32 i = 0; i < TexInfos.Num(); i++)
				{
					UTexture* TX = nullptr; MI->GetTextureParameterValue(TexInfos[i], TX);
					TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("name"), TexInfos[i].Name.ToString());
					O->SetStringField(TEXT("type"), TEXT("Texture"));
					O->SetStringField(TEXT("value"), TX ? TX->GetPathName() : TEXT("None"));
					Params.Add(MakeShared<FJsonValueObject>(O));
				}
				OutStructured->SetArrayField(TEXT("parameters"), Params);
				OutSummary = FString::Printf(TEXT("Material '%s': %d params"), *MI->GetName(), Params.Num());
				return true;
			}
		, nullptr
		, 10
		});

		// UE 5.7 verified: EComponentMobility::Type API unchanged
		Registry.Register({ TEXT("lighting_scene_report"), TEXT("Report on all lights in the scene: type, intensity, color, shadows."),
			FSololmcpSchemaBuilder::Object({}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){OutError=TEXT("No world.");return false;}TArray<TSharedPtr<FJsonValue>>Arr;for(TActorIterator<ALight>It(W);It;++It){ALight*L=*It;ULightComponent*LC=L->GetLightComponent();if(!LC)continue;TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("actor"),L->GetActorLabel());O->SetStringField(TEXT("type"),LC->GetClass()->GetName());O->SetNumberField(TEXT("intensity"),LC->Intensity);O->SetStringField(TEXT("color"),LC->LightColor.ToString());O->SetBoolField(TEXT("cast_shadows"),LC->CastShadows);O->SetStringField(TEXT("mobility"),StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(static_cast<int64>(LC->Mobility.GetValue())));Arr.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("lights"),Arr);OutSummary=FString::Printf(TEXT("%d lights in scene"),Arr.Num());return true; }
		, nullptr
		, 10
		});

		// ---- scene_lighting_ensure (idempotent lighting baseline) ----
		// Root problem fixed: scene-production re-runs kept spawning a NEW
		// DirectionalLight+SkyLight each time. Multiple directional lights fight
		// over the single forward-shading slot (UE warns "Multiple directional
		// lights are competing...") and the viewport goes BLACK, so the agent's
		// visual QA never passes -> infinite rebuild loop. This guarantees EXACTLY
		// ONE DirectionalLight + ONE SkyLight (keeps the first, destroys the rest)
		// and sets a sane visible intensity. Safe to call on every run/resume.
		Registry.Register({ TEXT("scene_lighting_ensure"),
			TEXT("Idempotently guarantee a complete UE5 daylight baseline: exactly one primary DirectionalLight, one SkyLight, one SkyAtmosphere, one ExponentialHeightFog, and one unbound PostProcessVolume. Call on every scene-production run/resume; use this INSTEAD OF spawning ad-hoc sky-only or duplicate light actors."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("directional_intensity"), FSololmcpSchemaBuilder::Number(TEXT("DirectionalLight intensity (lux). Default 80000 for readable outdoor daylight."))},
				{TEXT("skylight_intensity"), FSololmcpSchemaBuilder::Number(TEXT("SkyLight intensity multiplier. Default 1.5."))},
				{TEXT("fog_density"), FSololmcpSchemaBuilder::Number(TEXT("ExponentialHeightFog density. Default 0.004."))},
				{TEXT("exposure_compensation"), FSololmcpSchemaBuilder::Number(TEXT("Global PostProcess exposure compensation. Default 0.5."))},
				{TEXT("cached_lighting_pre_exposure"), FSololmcpSchemaBuilder::Number(TEXT("Set r.EyeAdaptation.CachedLightingPreExposure EV. Engine default 4 covers ~[-8;12] EV; 8 fully covers the physically based range [-4;16] (daylight ~EV15). Default 8.0. Only applied when it differs from the current value (live re-sets have crashed the editor)."))},
				{TEXT("create_if_missing"), FSololmcpSchemaBuilder::Boolean(TEXT("If no light of a kind exists, spawn one. Default true."))}
			}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				if (!World) { OutError = TEXT("No editor world."); return false; }
				double DirIntensity = 80000.0; Arguments->TryGetNumberField(TEXT("directional_intensity"), DirIntensity);
				double SkyIntensity = 1.5;  Arguments->TryGetNumberField(TEXT("skylight_intensity"), SkyIntensity);
				double FogDensity = 0.004; Arguments->TryGetNumberField(TEXT("fog_density"), FogDensity);
				double ExposureCompensation = 0.5; Arguments->TryGetNumberField(TEXT("exposure_compensation"), ExposureCompensation);
				// 引擎默认 4 覆盖 ~[-8;12] EV；8 才完整覆盖物理光照范围 [-4;16]（白天 ~EV15）。
				// 旧默认 -10 方向反了：可用范围被推到 [-22;-2]，白天必裁剪（Lumen cached-lighting clip 洗色的根因）。
				double CachedLightingPreExposure = 8.0; Arguments->TryGetNumberField(TEXT("cached_lighting_pre_exposure"), CachedLightingPreExposure);
				bool bCreateIfMissing = true; Arguments->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);

				TArray<ADirectionalLight*> Dirs;
				TArray<ASkyLight*> Skies;
				for (TActorIterator<ADirectionalLight> It(World); It; ++It) { if (*It) Dirs.Add(*It); }
				for (TActorIterator<ASkyLight> It(World); It; ++It) { if (*It) Skies.Add(*It); }

				int32 DestroyedDir = 0, DestroyedSky = 0;
				ADirectionalLight* KeepDir = nullptr;
				for (ADirectionalLight* Candidate : Dirs)
				{
					if (Candidate && Candidate->GetActorLabel() == TEXT("SOM_Daylight_Sun"))
					{
						KeepDir = Candidate;
						break;
					}
				}
				if (!KeepDir && Dirs.Num() > 0) { KeepDir = Dirs[0]; }
				for (ADirectionalLight* Candidate : Dirs)
				{
					if (Candidate && Candidate != KeepDir)
					{
						const bool bDestroyed = Candidate->Destroy();
						if (bDestroyed) { DestroyedDir++; }
					}
				}
				ASkyLight* KeepSky = nullptr;
				for (ASkyLight* Candidate : Skies)
				{
					if (Candidate && Candidate->GetActorLabel() == TEXT("SOM_Daylight_Sky"))
					{
						KeepSky = Candidate;
						break;
					}
				}
				if (!KeepSky && Skies.Num() > 0) { KeepSky = Skies[0]; }
				for (ASkyLight* Candidate : Skies)
				{
					if (Candidate && Candidate != KeepSky)
					{
						const bool bDestroyed = Candidate->Destroy();
						if (bDestroyed) { DestroyedSky++; }
					}
				}

				bool bCreatedDir = false, bCreatedSky = false;
				if (!KeepDir && bCreateIfMissing)
				{
					KeepDir = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FVector(0,0,2000), FRotator(-35.0f, 135.0f, 0.0f));
					bCreatedDir = (KeepDir != nullptr);
				}
				if (!KeepSky && bCreateIfMissing)
				{
					KeepSky = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FVector(0,0,2000), FRotator::ZeroRotator);
					bCreatedSky = (KeepSky != nullptr);
				}

				if (KeepDir)
				{
					KeepDir->SetActorLabel(TEXT("SOM_Daylight_Sun"));
					KeepDir->SetActorRotation(FRotator(-35.0f, 135.0f, 0.0f));
					if (UDirectionalLightComponent* DC = KeepDir->GetComponent())
					{
						DC->SetMobility(EComponentMobility::Movable);
						DC->SetIntensity(static_cast<float>(DirIntensity));
						DC->SetUseTemperature(true);
						DC->SetTemperature(5400.0f);
						DC->SetLightColor(FLinearColor(1.0f, 0.955f, 0.84f), true);
						DC->SetAtmosphereSunLight(true);
						DC->SetAtmosphereSunLightIndex(0);
						DC->SetForwardShadingPriority(1);
						DC->SetVisibility(true);
						DC->MarkRenderStateDirty();
						KeepDir->MarkPackageDirty();
					}
				}
				if (KeepSky)
				{
					KeepSky->SetActorLabel(TEXT("SOM_Daylight_Sky"));
					if (USkyLightComponent* SC = KeepSky->GetLightComponent())
					{
						SC->SetMobility(EComponentMobility::Movable);
						SC->SetIntensity(static_cast<float>(SkyIntensity));
						SC->SetVisibility(true);
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
						SC->SetRealTimeCapture(true);
#else
				// USkyLightComponent::SetRealTimeCapture is 5.5+; the property drives the
				// same capture path, so set it and refresh the render state.
				SC->bRealTimeCapture = true;
				SC->MarkRenderStateDirty();
#endif
						SC->RecaptureSky();
						SC->MarkRenderStateDirty();
						KeepSky->MarkPackageDirty();
					}
				}

				OutStructured->SetNumberField(TEXT("directional_before"), Dirs.Num());
				OutStructured->SetNumberField(TEXT("sky_before"), Skies.Num());
				OutStructured->SetNumberField(TEXT("directional_destroyed"), DestroyedDir);
				OutStructured->SetNumberField(TEXT("sky_destroyed"), DestroyedSky);
				OutStructured->SetBoolField(TEXT("directional_created"), bCreatedDir);
				OutStructured->SetBoolField(TEXT("sky_created"), bCreatedSky);
				OutStructured->SetNumberField(TEXT("directional_after"), KeepDir ? 1 : 0);
				OutStructured->SetNumberField(TEXT("sky_after"), KeepSky ? 1 : 0);
				// ── Ensure EXACTLY ONE SkyAtmosphere (the visible physical sky) + wire it ──
				// Without a SkyAtmosphere the sky renders BLACK and the SkyLight has nothing
				// to capture — the missing piece behind every prior black-sky setup.
				// Idempotent (keep first, destroy extras, spawn if missing). Then make the
				// kept DirectionalLight the atmosphere sun + SkyLight real-time-capture so the
				// atmosphere is actually lit. ASkyAtmosphere is in the Engine module (Build.cs
				// dep); header included at top of this file.
				TArray<ASkyAtmosphere*> Atmos;
				for (TActorIterator<ASkyAtmosphere> It(World); It; ++It) { if (*It) Atmos.Add(*It); }
				int32 DestroyedAtmos = 0;
				ASkyAtmosphere* KeepAtmos = Atmos.Num() > 0 ? Atmos[0] : nullptr;
				for (int32 ai = 1; ai < Atmos.Num(); ++ai)
				{
					if (Atmos[ai])
					{
						const bool bDestroyed = Atmos[ai]->Destroy();
						if (bDestroyed) { DestroyedAtmos++; }
					}
				}
				bool bCreatedAtmos = false;
				if (!KeepAtmos && bCreateIfMissing)
				{
					KeepAtmos = World->SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator);
					bCreatedAtmos = (KeepAtmos != nullptr);
					if (KeepAtmos) { KeepAtmos->MarkPackageDirty(); }
				}
				if (KeepAtmos)
				{
					KeepAtmos->SetActorLabel(TEXT("SOM_SkyAtmosphere"));
					KeepAtmos->MarkPackageDirty();
				}
				OutStructured->SetNumberField(TEXT("atmosphere_before"), Atmos.Num());
				OutStructured->SetNumberField(TEXT("atmosphere_destroyed"), DestroyedAtmos);
				OutStructured->SetBoolField(TEXT("atmosphere_created"), bCreatedAtmos);
				OutStructured->SetNumberField(TEXT("atmosphere_after"), KeepAtmos ? 1 : 0);

				TArray<AExponentialHeightFog*> Fogs;
				for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) { if (*It) Fogs.Add(*It); }
				int32 DestroyedFog = 0;
				AExponentialHeightFog* KeepFog = nullptr;
				for (AExponentialHeightFog* Candidate : Fogs)
				{
					if (Candidate && Candidate->GetActorLabel() == TEXT("SOM_HeightFog"))
					{
						KeepFog = Candidate;
						break;
					}
				}
				if (!KeepFog && Fogs.Num() > 0) { KeepFog = Fogs[0]; }
				for (AExponentialHeightFog* Candidate : Fogs)
				{
					if (Candidate && Candidate != KeepFog)
					{
						const bool bDestroyed = Candidate->Destroy();
						if (bDestroyed) { DestroyedFog++; }
					}
				}
				bool bCreatedFog = false;
				if (!KeepFog && bCreateIfMissing)
				{
					KeepFog = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FVector(0,0,100), FRotator::ZeroRotator);
					bCreatedFog = (KeepFog != nullptr);
				}
				if (KeepFog)
				{
					KeepFog->SetActorLabel(TEXT("SOM_HeightFog"));
					if (UExponentialHeightFogComponent* FC = KeepFog->GetComponent())
					{
						FC->SetMobility(EComponentMobility::Movable);
						FC->SetFogDensity(static_cast<float>(FogDensity));
						FC->SetFogHeightFalloff(0.18f);
						FC->SetFogMaxOpacity(0.75f);
						FC->SetStartDistance(0.0f);
						FC->bEnableVolumetricFog = true;
						FC->MarkRenderStateDirty();
					}
					KeepFog->MarkPackageDirty();
				}
				OutStructured->SetNumberField(TEXT("fog_before"), Fogs.Num());
				OutStructured->SetNumberField(TEXT("fog_destroyed"), DestroyedFog);
				OutStructured->SetBoolField(TEXT("fog_created"), bCreatedFog);
				OutStructured->SetNumberField(TEXT("fog_after"), KeepFog ? 1 : 0);

				TArray<APostProcessVolume*> PostVolumes;
				for (TActorIterator<APostProcessVolume> It(World); It; ++It) { if (*It) PostVolumes.Add(*It); }
				int32 DestroyedPost = 0;
				APostProcessVolume* KeepPost = nullptr;
				for (APostProcessVolume* Candidate : PostVolumes)
				{
					if (Candidate && Candidate->GetActorLabel() == TEXT("SOM_GlobalPostProcess"))
					{
						KeepPost = Candidate;
						break;
					}
				}
				if (!KeepPost && PostVolumes.Num() > 0) { KeepPost = PostVolumes[0]; }
				for (APostProcessVolume* Candidate : PostVolumes)
				{
					if (Candidate && Candidate != KeepPost)
					{
						const bool bDestroyed = Candidate->Destroy();
						if (bDestroyed) { DestroyedPost++; }
					}
				}
				bool bCreatedPost = false;
				if (!KeepPost && bCreateIfMissing)
				{
					KeepPost = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
					bCreatedPost = (KeepPost != nullptr);
				}
				if (KeepPost)
				{
					KeepPost->SetActorLabel(TEXT("SOM_GlobalPostProcess"));
					KeepPost->bUnbound = true;
					KeepPost->Priority = 10.0f;
					KeepPost->BlendRadius = 0.0f;
					KeepPost->Settings.bOverride_AutoExposureBias = true;
					KeepPost->Settings.AutoExposureBias = static_cast<float>(ExposureCompensation);
					KeepPost->Settings.bOverride_BloomIntensity = true;
					KeepPost->Settings.BloomIntensity = 0.25f;
					KeepPost->Settings.bOverride_AmbientOcclusionIntensity = true;
					KeepPost->Settings.AmbientOcclusionIntensity = 0.7f;
					KeepPost->MarkPackageDirty();
				}
				OutStructured->SetNumberField(TEXT("post_process_before"), PostVolumes.Num());
				OutStructured->SetNumberField(TEXT("post_process_destroyed"), DestroyedPost);
				OutStructured->SetBoolField(TEXT("post_process_created"), bCreatedPost);
				OutStructured->SetNumberField(TEXT("post_process_after"), KeepPost ? 1 : 0);
				OutStructured->SetBoolField(TEXT("complete_daylight_rig"), KeepDir && KeepSky && KeepAtmos && KeepFog && KeepPost);
				OutStructured->SetNumberField(TEXT("directional_intensity"), DirIntensity);
				OutStructured->SetNumberField(TEXT("skylight_intensity"), SkyIntensity);
				OutStructured->SetNumberField(TEXT("exposure_compensation"), ExposureCompensation);
				bool bCachedLightingPreExposureApplied = false;
				const double ClampedCachedLightingPreExposure = FMath::Clamp(CachedLightingPreExposure, -16.0, 16.0);
				if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.CachedLightingPreExposure")))
				{
					// 只在值确实不同才 set：活跃会话中重复 set 渲染 CVar 曾当场崩掉编辑器。
					// 正道是把值写进工程 DefaultEngine.ini [SystemSettings] 让它启动期生效，这里只兜底。
					if (!FMath::IsNearlyEqual(CVar->GetFloat(), static_cast<float>(ClampedCachedLightingPreExposure), 0.01f))
					{
						CVar->Set(static_cast<float>(ClampedCachedLightingPreExposure), ECVF_SetByConsole);
					}
					bCachedLightingPreExposureApplied = true;
				}
				OutStructured->SetBoolField(TEXT("cached_lighting_pre_exposure_applied"), bCachedLightingPreExposureApplied);
				OutStructured->SetNumberField(TEXT("cached_lighting_pre_exposure"), ClampedCachedLightingPreExposure);
				OutStructured->SetNumberField(TEXT("directional_forward_shading_priority"), KeepDir ? 1 : 0);
				if (GEditor)
				{
					GEditor->RedrawLevelEditingViewports(true);
				}
				OutSummary = FString::Printf(TEXT("Daylight rig normalized: sun=1 (was %d, destroyed %d, %.0f lux), sky=1 (was %d, destroyed %d), atmosphere=1 (was %d, destroyed %d), fog=1 (was %d, destroyed %d), post=1 (was %d, destroyed %d)."), Dirs.Num(), DestroyedDir, DirIntensity, Skies.Num(), DestroyedSky, Atmos.Num(), DestroyedAtmos, Fogs.Num(), DestroyedFog, PostVolumes.Num(), DestroyedPost);
				return true;
			}
		, nullptr
		, 0
		});

		Registry.Register({ TEXT("world_actor_summary"), TEXT("Summary of all actors grouped by class."),
			FSololmcpSchemaBuilder::Object({{TEXT("class_filter"),FSololmcpSchemaBuilder::String(TEXT("Opt class substring"))}}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ UWorld*W=GEditor->GetEditorWorldContext().World();if(!W){OutError=TEXT("No world.");return false;}FString Filter=Arguments->HasField(TEXT("class_filter"))?Arguments->GetStringField(TEXT("class_filter")):TEXT("");TMap<FString,int32>ClassCounts;int32 Total=0;for(TActorIterator<AActor>It(W);It;++It){FString CN=(*It)->GetClass()->GetName();if(!Filter.IsEmpty()&&!CN.Contains(Filter))continue;ClassCounts.FindOrAdd(CN)++;Total++;}TArray<TSharedPtr<FJsonValue>>Arr;for(auto&Pair:ClassCounts){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("class"),Pair.Key);O->SetNumberField(TEXT("count"),Pair.Value);Arr.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("classes"),Arr);OutStructured->SetNumberField(TEXT("total_actors"),Total);OutStructured->SetNumberField(TEXT("unique_classes"),Arr.Num());OutSummary=FString::Printf(TEXT("%d actors, %d classes"),Total,Arr.Num());return true; }
		, nullptr
		, 10
		});

		// UE 5.7 fix: GetMasterTracks() removed → GetTracks()
		Registry.Register({ TEXT("sequencer_inspect"), TEXT("Inspect LevelSequence: tracks, bindings, length."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("LevelSequence path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				ULevelSequence*LS=Cast<ULevelSequence>(Context.Services.LoadAsset(_AP,OutError));if(!LS){if(OutError.IsEmpty()){OutError=TEXT("Failed to load level sequence asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}UMovieScene*MS=LS->GetMovieScene();if(!MS){OutError=TEXT("No MovieScene.");return false;}const UMovieScene*ConstMS=MS;OutStructured->SetStringField(TEXT("path"),LS->GetPathName());OutStructured->SetNumberField(TEXT("display_rate_fps"),MS->GetDisplayRate().AsDecimal());auto Range=MS->GetPlaybackRange();OutStructured->SetNumberField(TEXT("start_frame"),Range.GetLowerBoundValue().Value);OutStructured->SetNumberField(TEXT("end_frame"),Range.GetUpperBoundValue().Value);TArray<TSharedPtr<FJsonValue>>Tracks;for(UMovieSceneTrack*T:MS->GetTracks()){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),T->GetDisplayName().ToString());O->SetStringField(TEXT("class"),T->GetClass()->GetName());O->SetNumberField(TEXT("sections"),T->GetAllSections().Num());Tracks.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("tracks"),Tracks);OutStructured->SetNumberField(TEXT("binding_count"),ConstMS->GetBindings().Num());OutSummary=FString::Printf(TEXT("Seq '%s': %d tracks, %d bindings"),*LS->GetName(),Tracks.Num(),ConstMS->GetBindings().Num());return true; }
		, nullptr
		, 10
		});

		// UE 5.7 verified: FNiagaraEmitterHandle::GetName()/GetIsEnabled() still exist
		Registry.Register({ TEXT("niagara_system_inspect"), TEXT("Inspect Niagara System: emitters, parameters."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("NiagaraSystem path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UNiagaraSystem*NS=Cast<UNiagaraSystem>(Context.Services.LoadAsset(_AP,OutError));if(!NS){if(OutError.IsEmpty()){OutError=TEXT("Failed to load niagara system asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}OutStructured->SetStringField(TEXT("path"),NS->GetPathName());TArray<TSharedPtr<FJsonValue>>Emitters;for(const FNiagaraEmitterHandle&EH:NS->GetEmitterHandles()){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),EH.GetName().ToString());O->SetBoolField(TEXT("enabled"),EH.GetIsEnabled());Emitters.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("emitters"),Emitters);OutStructured->SetNumberField(TEXT("emitter_count"),Emitters.Num());OutSummary=FString::Printf(TEXT("Niagara '%s': %d emitters"),*NS->GetName(),Emitters.Num());return true; }
		, nullptr
		, 10
		});

		// UE 5.7 verified: UPCGGraph::GetNodes() still exists
		Registry.Register({ TEXT("pcg_graph_inspect"), TEXT("Inspect PCG graph: nodes, edges, input/output pins."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("PCG graph path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UPCGGraphInterface*GI=Cast<UPCGGraphInterface>(Context.Services.LoadAsset(_AP,OutError));if(!GI){if(OutError.IsEmpty()){OutError=TEXT("Failed to load PCG graph asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}UPCGGraph*G=GI->GetMutablePCGGraph();if(!G){OutError=TEXT("No PCG graph.");return false;}OutStructured->SetStringField(TEXT("path"),GI->GetPathName());TArray<TSharedPtr<FJsonValue>>Nodes;for(UPCGNode*N:G->GetNodes()){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),SomolPcgNodeTitle(N));O->SetStringField(TEXT("class"),N->GetSettings()?N->GetSettings()->GetClass()->GetName():TEXT("None"));O->SetNumberField(TEXT("input_pins"),N->GetInputPins().Num());O->SetNumberField(TEXT("output_pins"),N->GetOutputPins().Num());Nodes.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("nodes"),Nodes);OutStructured->SetNumberField(TEXT("node_count"),Nodes.Num());OutSummary=FString::Printf(TEXT("PCG '%s': %d nodes"),*GI->GetName(),Nodes.Num());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("asset_search"),
			TEXT("Search assets by tokens across NAME + CLASS + PATH + Asset Registry TAG VALUES (e.g. materials used, source import path, dimensions, gameplay tags). Case-insensitive, multi-token, relevance-ranked — far stronger than a filename substring. Optional class_name/path filters narrow the set; match_all requires every token; search_tags toggles tag-value matching."),
			FSololmcpSchemaBuilder::Object({
				{TEXT("query"),FSololmcpSchemaBuilder::String(TEXT("Space-separated search terms"))},
				{TEXT("class_name"),FSololmcpSchemaBuilder::String(TEXT("Opt class filter: short name (StaticMesh) or /Script/...path; lenient substring fallback"))},
				{TEXT("path"),FSololmcpSchemaBuilder::String(TEXT("Opt /Game/... path prefix (recursive)"))},
				{TEXT("max_results"),FSololmcpSchemaBuilder::Integer(TEXT("Limit (default 50)"))},
				{TEXT("match_all"),FSololmcpSchemaBuilder::Boolean(TEXT("Require ALL tokens to match (default false = any token, ranked by how many match)"))},
				{TEXT("search_tags"),FSololmcpSchemaBuilder::Boolean(TEXT("Also match Asset Registry tag values (default true)"))}
			},{TEXT("query")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				FARFilter F;
				// class_name filter: try exact class resolution; else keep all and post-filter by class-name substring (lenient).
				FString ClassNameLower;
				if (Arguments->HasField(TEXT("class_name")))
				{
					const FString CN = Arguments->GetStringField(TEXT("class_name"));
					ClassNameLower = CN.ToLower();
					UClass* Cls = FindObject<UClass>(nullptr, *CN);
					if (Cls) { F.ClassPaths.Add(Cls->GetClassPathName()); ClassNameLower.Empty(); /*hard-filtered, no post-filter needed*/ }
				}
				if (Arguments->HasField(TEXT("path"))) { F.PackagePaths.Add(FName(*Arguments->GetStringField(TEXT("path")))); }
				F.bRecursivePaths = true;
				// Default the scope to project content (/Game) when neither a path nor a
				// hard class filter was given — an otherwise-empty FARFilter returns NOTHING,
				// so a bare keyword search would always come back empty (the original bug).
				if (F.PackagePaths.Num() == 0 && F.ClassPaths.Num() == 0) { F.PackagePaths.Add(FName(TEXT("/Game"))); }
				// Ensure the registry has finished its initial scan; a freshly-launched
				// editor returns 0 assets until the scan completes (mirrors asset_query).
				ARM.Get().WaitForCompletion();
				TArray<FAssetData> Assets;
				ARM.Get().GetAssets(F, Assets);

				// Tokenize the query (case-insensitive, whitespace-split).
				TArray<FString> Tokens;
				Arguments->GetStringField(TEXT("query")).ToLower().ParseIntoArrayWS(Tokens);
				const int32 Max = Arguments->HasField(TEXT("max_results")) ? static_cast<int32>(Arguments->GetNumberField(TEXT("max_results"))) : 50;
				const bool bMatchAll = Arguments->HasField(TEXT("match_all")) && Arguments->GetBoolField(TEXT("match_all"));
				const bool bSearchTags = !Arguments->HasField(TEXT("search_tags")) || Arguments->GetBoolField(TEXT("search_tags"));

				struct FHit { FAssetData* A; int32 Score; int32 Matched; };
				TArray<FHit> Hits;
				for (FAssetData& A : Assets)
				{
					const FString NameL = A.AssetName.ToString().ToLower();
					const FString ClsL = A.AssetClassPath.GetAssetName().ToString().ToLower();
					// lenient class post-filter when the class wasn't hard-resolved above
					if (!ClassNameLower.IsEmpty() && !ClsL.Contains(ClassNameLower)) { continue; }
					const FString PathL = A.GetObjectPathString().ToLower();
					int32 Score = 0, Matched = 0;
					for (const FString& Tok : Tokens)
					{
						int32 TokScore = 0;
						if (NameL.Contains(Tok)) { TokScore = 5; }
						else if (ClsL.Contains(Tok)) { TokScore = 3; }
						else if (PathL.Contains(Tok)) { TokScore = 2; }
						else if (bSearchTags)
						{
							for (const TPair<FName, FAssetTagValueRef>& TagPair : A.TagsAndValues)
							{
								if (TagPair.Value.AsString().ToLower().Contains(Tok)) { TokScore = 1; break; }
							}
						}
						if (TokScore > 0) { Score += TokScore; ++Matched; }
					}
					if (Tokens.Num() == 0) { Score = 1; Matched = 0; } // empty query → return the class/path-filtered set
					else if (Matched == 0) { continue; }
					else if (bMatchAll && Matched < Tokens.Num()) { continue; }
					if (Tokens.Num() == 1 && NameL == Tokens[0]) { Score += 10; } // exact-name bonus
					Hits.Add({ &A, Score, Matched });
				}
				// Rank: more tokens matched first, then higher score, then shorter name (more specific).
				Hits.Sort([](const FHit& X, const FHit& Y){ if (X.Matched != Y.Matched) return X.Matched > Y.Matched; if (X.Score != Y.Score) return X.Score > Y.Score; return X.A->AssetName.ToString().Len() < Y.A->AssetName.ToString().Len(); });

				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FHit& H : Hits)
				{
					TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("path"), H.A->GetObjectPathString());
					O->SetStringField(TEXT("name"), H.A->AssetName.ToString());
					O->SetStringField(TEXT("class"), H.A->AssetClassPath.GetAssetName().ToString());
					O->SetNumberField(TEXT("score"), H.Score);
					O->SetNumberField(TEXT("tokens_matched"), H.Matched);
					Arr.Add(MakeShared<FJsonValueObject>(O));
					if (Arr.Num() >= Max) { break; }
				}
				OutStructured->SetArrayField(TEXT("results"), Arr);
				OutStructured->SetNumberField(TEXT("scanned"), Assets.Num());
				OutSummary = FString::Printf(TEXT("%d assets matched (of %d scanned) for %d token(s)"), Arr.Num(), Assets.Num(), Tokens.Num());
				return true;
			}
		, nullptr
		, 5
		});

		Registry.Register({ TEXT("import_settings_read"), TEXT("Read import settings for FBX or texture asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Asset path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ FString AP=Arguments->GetStringField(TEXT("asset_path"));UObject*Obj=Cast<UObject>(Context.Services.LoadAsset(AP,OutError));if(!Obj)return false;OutStructured->SetStringField(TEXT("path"),Obj->GetPathName());OutStructured->SetStringField(TEXT("class"),Obj->GetClass()->GetName());if(UTexture2D*T=Cast<UTexture2D>(Obj)){OutStructured->SetStringField(TEXT("compression"),StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(static_cast<int64>(T->CompressionSettings)));OutStructured->SetBoolField(TEXT("srgb"),T->SRGB);OutStructured->SetStringField(TEXT("lod_group"),StaticEnum<TextureGroup>()->GetNameStringByValue(static_cast<int64>(T->LODGroup)));OutStructured->SetNumberField(TEXT("max_size"),T->MaxTextureSize);}else if(UStaticMesh*SM=Cast<UStaticMesh>(Obj)){OutStructured->SetNumberField(TEXT("lod_count"),SM->GetNumLODs());OutStructured->SetBoolField(TEXT("has_collision"),SM->GetBodySetup()!=nullptr);OutStructured->SetNumberField(TEXT("lightmap_resolution"),SM->GetLightMapResolution());}else if(USkeletalMesh*SK=Cast<USkeletalMesh>(Obj)){OutStructured->SetNumberField(TEXT("lod_count"),SK->GetLODNum());OutStructured->SetNumberField(TEXT("bone_count"),SK->GetRefSkeleton().GetNum());}OutSummary=FString::Printf(TEXT("Import info for '%s' (%s)"),*Obj->GetName(),*Obj->GetClass()->GetName());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("skeletal_mesh_inspect"), TEXT("Inspect skeletal mesh: bones, LODs, materials, physics asset."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("SkeletalMesh path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				USkeletalMesh*SK=Cast<USkeletalMesh>(Context.Services.LoadAsset(_AP,OutError));if(!SK){if(OutError.IsEmpty()){OutError=TEXT("Failed to load skeletal mesh asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}OutStructured->SetStringField(TEXT("path"),SK->GetPathName());OutStructured->SetNumberField(TEXT("bone_count"),SK->GetRefSkeleton().GetNum());OutStructured->SetNumberField(TEXT("lod_count"),SK->GetLODNum());TArray<TSharedPtr<FJsonValue>>Mats;for(int32 i=0;i<SK->GetMaterials().Num();i++){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("index"),i);O->SetStringField(TEXT("name"),SK->GetMaterials()[i].MaterialSlotName.ToString());O->SetStringField(TEXT("material"),SK->GetMaterials()[i].MaterialInterface?SK->GetMaterials()[i].MaterialInterface->GetPathName():TEXT("None"));Mats.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("materials"),Mats);OutStructured->SetStringField(TEXT("skeleton"),SK->GetSkeleton()?SK->GetSkeleton()->GetPathName():TEXT("None"));OutStructured->SetStringField(TEXT("physics_asset"),SK->GetPhysicsAsset()?SK->GetPhysicsAsset()->GetPathName():TEXT("None"));OutSummary=FString::Printf(TEXT("SkMesh '%s': %d bones, %d LODs, %d mats"),*SK->GetName(),SK->GetRefSkeleton().GetNum(),SK->GetLODNum(),Mats.Num());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("animation_montage_inspect"), TEXT("Inspect Animation Montage: slots, sections, notifies."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("Montage path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 4: validate asset_path before LoadAsset (round 3 missed this entry).
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UAnimMontage*AM=Cast<UAnimMontage>(Context.Services.LoadAsset(_AP,OutError));if(!AM){if(OutError.IsEmpty()){OutError=TEXT("Failed to load animation montage asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}OutStructured->SetStringField(TEXT("path"),AM->GetPathName());OutStructured->SetNumberField(TEXT("duration"),AM->GetPlayLength());OutStructured->SetNumberField(TEXT("blend_in"),AM->BlendIn.GetBlendTime());OutStructured->SetNumberField(TEXT("blend_out"),AM->BlendOut.GetBlendTime());TArray<TSharedPtr<FJsonValue>>Slots;for(auto&SD:AM->SlotAnimTracks){Slots.Add(MakeShared<FJsonValueString>(SD.SlotName.ToString()));}OutStructured->SetArrayField(TEXT("slots"),Slots);TArray<TSharedPtr<FJsonValue>>Sections;for(int32 i=0;i<AM->CompositeSections.Num();i++){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),AM->CompositeSections[i].SectionName.ToString());O->SetNumberField(TEXT("start_time"),AM->CompositeSections[i].GetTime());Sections.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("sections"),Sections);OutStructured->SetNumberField(TEXT("notify_count"),AM->Notifies.Num());OutSummary=FString::Printf(TEXT("Montage '%s': %.2fs, %d slots, %d sections"),*AM->GetName(),AM->GetPlayLength(),Slots.Num(),Sections.Num());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("control_rig_inspect"), TEXT("Inspect Control Rig: controls, bones, hierarchy."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("ControlRig BP path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UBlueprint*BP=Cast<UBlueprint>(Context.Services.LoadAsset(_AP,OutError));if(!BP){if(OutError.IsEmpty()){OutError=TEXT("Failed to load control rig blueprint asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}OutStructured->SetStringField(TEXT("path"),BP->GetPathName());OutStructured->SetStringField(TEXT("parent"),BP->ParentClass?BP->ParentClass->GetName():TEXT("None"));TArray<TSharedPtr<FJsonValue>>Vars;for(FBPVariableDescription&V:BP->NewVariables){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),V.VarName.ToString());O->SetStringField(TEXT("type"),V.VarType.PinCategory.ToString());Vars.Add(MakeShared<FJsonValueObject>(O));}OutStructured->SetArrayField(TEXT("variables"),Vars);TArray<TSharedPtr<FJsonValue>>Graphs;for(UEdGraph*G:BP->FunctionGraphs)Graphs.Add(MakeShared<FJsonValueString>(G->GetName()));OutStructured->SetArrayField(TEXT("graphs"),Graphs);OutSummary=FString::Printf(TEXT("ControlRig '%s': %d vars, %d graphs"),*BP->GetName(),Vars.Num(),Graphs.Num());return true; }
		, nullptr
		, 10
		});

		Registry.Register({ TEXT("widget_blueprint_inspect"), TEXT("Inspect Widget Blueprint: widget hierarchy, bindings, animations."),
			FSololmcpSchemaBuilder::Object({{TEXT("asset_path"),FSololmcpSchemaBuilder::String(TEXT("WidgetBP path"))}},{TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{ // Audit round 3: validate asset_path before LoadAsset.
				// Round 9C: accept package-form ("/Game/...") as well as object-form; LoadAsset normalizes.
				FString _AP; if(!Arguments->TryGetStringField(TEXT("asset_path"),_AP)||_AP.IsEmpty()||!_AP.StartsWith(TEXT("/"))){OutError=TEXT("Missing or invalid asset_path. Must start with /Game/ or /Engine/.");return false;}
				UWidgetBlueprint*WBP=Cast<UWidgetBlueprint>(Context.Services.LoadAsset(_AP,OutError));if(!WBP){if(OutError.IsEmpty()){OutError=TEXT("Failed to load widget blueprint asset (path may need .ObjectName suffix or asset may not exist): ")+_AP;}return false;}OutStructured->SetStringField(TEXT("path"),WBP->GetPathName());OutStructured->SetStringField(TEXT("parent_class"),WBP->ParentClass?WBP->ParentClass->GetName():TEXT("None"));TArray<TSharedPtr<FJsonValue>>Widgets;if(WBP->WidgetTree){TArray<UWidget*>AllWidgets;WBP->WidgetTree->GetAllWidgets(AllWidgets);for(UWidget*Wdg:AllWidgets){TSharedPtr<FJsonObject>O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),Wdg->GetName());O->SetStringField(TEXT("class"),Wdg->GetClass()->GetName());O->SetBoolField(TEXT("is_variable"),Wdg->bIsVariable);Widgets.Add(MakeShared<FJsonValueObject>(O));}}OutStructured->SetArrayField(TEXT("widgets"),Widgets);TArray<TSharedPtr<FJsonValue>>Anims;for(UWidgetAnimation*WA:WBP->Animations){Anims.Add(MakeShared<FJsonValueString>(WA->GetName()));}OutStructured->SetArrayField(TEXT("animations"),Anims);OutStructured->SetNumberField(TEXT("binding_count"),WBP->Bindings.Num());OutSummary=FString::Printf(TEXT("Widget '%s': %d widgets, %d anims"),*WBP->GetName(),Widgets.Num(),Anims.Num());return true; }
		, nullptr
		, 10
		});

	} // end RegisterProjectPerceptionTools
}
