// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Native character animation assignment, playback control, and runtime inspection.
// This file deliberately has no Python or plan-contract fallback: every successful
// response is direct UE state readback from the explicitly selected world.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "AlphaBlend.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Animation/AnimTypes.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Misc/FrameNumber.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequenceID.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "SingleAnimationPlayData.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4
#include "UniversalObjectLocatorResolveParams.h"
#endif
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace CharacterActionRuntime
{
using SB = FSololmcpSchemaBuilder;

enum class ERequestedWorld : uint8
{
	Editor,
	PIE,
	Preview,
	Standalone,
	Sequencer
};

struct FResolvedTarget
{
	UWorld* World = nullptr;
	AActor* Actor = nullptr;
	USkeletalMeshComponent* Component = nullptr;
	ERequestedWorld RequestedWorld = ERequestedWorld::Editor;
	FString RequestedWorldName;
	FString BaseWorldName;
	int32 PIEInstance = INDEX_NONE;
	bool bSequencerContext = false;
};

static FString WorldTypeName(const EWorldType::Type Type)
{
	switch (Type)
	{
	case EWorldType::None: return TEXT("none");
	case EWorldType::Game: return TEXT("game");
	case EWorldType::Editor: return TEXT("editor");
	case EWorldType::PIE: return TEXT("pie");
	case EWorldType::EditorPreview: return TEXT("editor_preview");
	case EWorldType::GamePreview: return TEXT("game_preview");
	case EWorldType::GameRPC: return TEXT("game_rpc");
	case EWorldType::Inactive: return TEXT("inactive");
	default: return TEXT("unknown");
	}
}

static FString NetModeName(const ENetMode Mode)
{
	switch (Mode)
	{
	case NM_Standalone: return TEXT("standalone");
	case NM_DedicatedServer: return TEXT("dedicated_server");
	case NM_ListenServer: return TEXT("listen_server");
	case NM_Client: return TEXT("client");
	default: return TEXT("unknown");
	}
}

static FString NetRoleName(const ENetRole Role)
{
	switch (Role)
	{
	case ROLE_None: return TEXT("none");
	case ROLE_SimulatedProxy: return TEXT("simulated_proxy");
	case ROLE_AutonomousProxy: return TEXT("autonomous_proxy");
	case ROLE_Authority: return TEXT("authority");
	default: return TEXT("unknown");
	}
}

static bool ParseRequestedWorld(
	const TSharedRef<FJsonObject>& Arguments,
	ERequestedWorld& OutWorld,
	FString& OutRequestedName,
	FString& OutBaseWorld,
	FString& OutError)
{
	if (!Arguments->TryGetStringField(TEXT("world_context"), OutRequestedName))
	{
		OutError = TEXT("world_context is required; no implicit Editor-world fallback is permitted");
		return false;
	}
	OutRequestedName = OutRequestedName.TrimStartAndEnd().ToLower();
	if (OutRequestedName == TEXT("editor")) OutWorld = ERequestedWorld::Editor;
	else if (OutRequestedName == TEXT("pie")) OutWorld = ERequestedWorld::PIE;
	else if (OutRequestedName == TEXT("preview")) OutWorld = ERequestedWorld::Preview;
	else if (OutRequestedName == TEXT("standalone")) OutWorld = ERequestedWorld::Standalone;
	else if (OutRequestedName == TEXT("sequencer"))
	{
		OutWorld = ERequestedWorld::Sequencer;
		if (!Arguments->TryGetStringField(TEXT("sequencer_world_context"), OutBaseWorld))
		{
			OutError = TEXT("sequencer_world_context is required when world_context=sequencer (editor or pie)");
			return false;
		}
		OutBaseWorld = OutBaseWorld.TrimStartAndEnd().ToLower();
		if (OutBaseWorld != TEXT("editor") && OutBaseWorld != TEXT("pie"))
		{
			OutError = TEXT("sequencer_world_context must be editor or pie");
			return false;
		}
	}
	else
	{
		OutError = TEXT("world_context must be editor, pie, preview, standalone, or sequencer");
		return false;
	}
	return true;
}

static bool WorldMatches(
	const FWorldContext& Context,
	const ERequestedWorld Requested,
	const FString& BaseWorld)
{
	if (!Context.World()) return false;
	switch (Requested)
	{
	case ERequestedWorld::Editor: return Context.WorldType == EWorldType::Editor;
	case ERequestedWorld::PIE: return Context.WorldType == EWorldType::PIE;
	case ERequestedWorld::Preview:
		return Context.WorldType == EWorldType::EditorPreview || Context.WorldType == EWorldType::GamePreview;
	case ERequestedWorld::Standalone:
		return Context.WorldType == EWorldType::Game;
	case ERequestedWorld::Sequencer:
		return BaseWorld == TEXT("pie") ? Context.WorldType == EWorldType::PIE : Context.WorldType == EWorldType::Editor;
	default: return false;
	}
}

static bool ResolveWorld(
	const TSharedRef<FJsonObject>& Arguments,
	FResolvedTarget& OutTarget,
	FString& OutError)
{
	FString RequestedName;
	FString BaseWorld;
	ERequestedWorld Requested;
	if (!ParseRequestedWorld(Arguments, Requested, RequestedName, BaseWorld, OutError)) return false;
	if (!GEngine)
	{
		OutError = TEXT("GEngine is unavailable");
		return false;
	}

	int32 RequestedPIEInstance = INDEX_NONE;
	double PIEInstanceNumber = 0.0;
	if (Arguments->TryGetNumberField(TEXT("pie_instance"), PIEInstanceNumber))
	{
		RequestedPIEInstance = static_cast<int32>(PIEInstanceNumber);
	}
	FString RequestedNetMode;
	Arguments->TryGetStringField(TEXT("net_mode"), RequestedNetMode);
	RequestedNetMode = RequestedNetMode.TrimStartAndEnd().ToLower();
	FString RequestedWorldPath;
	Arguments->TryGetStringField(TEXT("world_path"), RequestedWorldPath);

	TArray<const FWorldContext*> Matches;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (!WorldMatches(Context, Requested, BaseWorld)) continue;
		UWorld* World = Context.World();
		if (RequestedPIEInstance != INDEX_NONE && Context.PIEInstance != RequestedPIEInstance) continue;
		if (!RequestedNetMode.IsEmpty() && NetModeName(World->GetNetMode()) != RequestedNetMode) continue;
		if (!RequestedWorldPath.IsEmpty()
			&& World->GetPathName() != RequestedWorldPath
			&& World->GetOutermost()->GetName() != RequestedWorldPath) continue;
		Matches.Add(&Context);
	}
	if (Matches.Num() != 1)
	{
		TArray<FString> CandidateDescriptions;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (UWorld* World = Context.World())
			{
				CandidateDescriptions.Add(FString::Printf(TEXT("%s|%s|pie=%d|net=%s"),
					*World->GetPathName(), *WorldTypeName(Context.WorldType), Context.PIEInstance,
					*NetModeName(World->GetNetMode())));
			}
		}
		OutError = FString::Printf(TEXT("world_context resolution expected exactly one world but found %d. Candidates: %s"),
			Matches.Num(), *FString::Join(CandidateDescriptions, TEXT(", ")));
		return false;
	}
	OutTarget.World = Matches[0]->World();
	OutTarget.RequestedWorld = Requested;
	OutTarget.RequestedWorldName = RequestedName;
	OutTarget.BaseWorldName = BaseWorld;
	OutTarget.PIEInstance = Matches[0]->PIEInstance;
	OutTarget.bSequencerContext = Requested == ERequestedWorld::Sequencer;
	return true;
}

static bool ResolveActorAndComponent(
	const TSharedRef<FJsonObject>& Arguments,
	FResolvedTarget& InOutTarget,
	FString& OutError)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("actor_id is required");
		return false;
	}
	TArray<AActor*> ExactPath;
	TArray<AActor*> ExactName;
	TArray<AActor*> ExactLabel;
	for (TActorIterator<AActor> It(InOutTarget.World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->GetPathName() == ActorId) ExactPath.Add(Actor);
		if (Actor->GetName() == ActorId) ExactName.Add(Actor);
#if WITH_EDITOR
		if (Actor->GetActorLabel() == ActorId) ExactLabel.Add(Actor);
#endif
	}
	const TArray<AActor*>* Selected = !ExactPath.IsEmpty() ? &ExactPath : (!ExactName.IsEmpty() ? &ExactName : &ExactLabel);
	if (!Selected || Selected->Num() == 0)
	{
		OutError = FString::Printf(TEXT("actor_not_found in selected world: %s"), *ActorId);
		return false;
	}
	if (Selected->Num() != 1)
	{
		OutError = FString::Printf(TEXT("actor_id is ambiguous in selected world (%d exact matches): %s"), Selected->Num(), *ActorId);
		return false;
	}
	InOutTarget.Actor = (*Selected)[0];

	TArray<USkeletalMeshComponent*> Components;
	InOutTarget.Actor->GetComponents<USkeletalMeshComponent>(Components);
	FString ComponentId;
	Arguments->TryGetStringField(TEXT("component_id"), ComponentId);
	if (ComponentId.IsEmpty())
	{
		if (Components.Num() != 1)
		{
			OutError = FString::Printf(TEXT("component_id omitted but actor has %d skeletal mesh components; selection must be explicit"), Components.Num());
			return false;
		}
		InOutTarget.Component = Components[0];
	}
	else
	{
		TArray<USkeletalMeshComponent*> ComponentMatches;
		for (USkeletalMeshComponent* Component : Components)
		{
			if (Component && (Component->GetPathName() == ComponentId || Component->GetName() == ComponentId))
			{
				ComponentMatches.Add(Component);
			}
		}
		if (ComponentMatches.Num() != 1)
		{
			OutError = FString::Printf(TEXT("component_id expected one skeletal mesh component but matched %d: %s"),
				ComponentMatches.Num(), *ComponentId);
			return false;
		}
		InOutTarget.Component = ComponentMatches[0];
	}
	return true;
}

static bool ResolveTarget(const TSharedRef<FJsonObject>& Arguments, FResolvedTarget& OutTarget, FString& OutError)
{
	return ResolveWorld(Arguments, OutTarget, OutError)
		&& ResolveActorAndComponent(Arguments, OutTarget, OutError);
}

static TSharedRef<FJsonObject> MakeWorldIdentity(const FResolvedTarget& Target)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("requested_context"), Target.RequestedWorldName);
	if (!Target.BaseWorldName.IsEmpty()) Result->SetStringField(TEXT("sequencer_world_context"), Target.BaseWorldName);
	Result->SetStringField(TEXT("world_path"), Target.World->GetPathName());
	Result->SetStringField(TEXT("world_type"), WorldTypeName(Target.World->WorldType));
	Result->SetNumberField(TEXT("pie_instance"), Target.PIEInstance);
	Result->SetStringField(TEXT("net_mode"), NetModeName(Target.World->GetNetMode()));
	Result->SetBoolField(TEXT("is_game_world"), Target.World->IsGameWorld());
	Result->SetBoolField(TEXT("sequencer_context"), Target.bSequencerContext);
	return Result;
}

static TSharedRef<FJsonObject> MakeNetworkIdentity(const FResolvedTarget& Target)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("net_mode"), NetModeName(Target.World->GetNetMode()));
	Result->SetStringField(TEXT("local_role"), NetRoleName(Target.Actor->GetLocalRole()));
	Result->SetStringField(TEXT("remote_role"), NetRoleName(Target.Actor->GetRemoteRole()));
	Result->SetBoolField(TEXT("has_authority"), Target.Actor->HasAuthority());
	Result->SetBoolField(TEXT("replicates"), Target.Actor->GetIsReplicated());
	return Result;
}

static TSharedRef<FJsonObject> MakeTransformJson(const FTransform& Transform)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	const FVector Location = Transform.GetLocation();
	const FRotator Rotation = Transform.Rotator();
	const FVector Scale = Transform.GetScale3D();
	TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>();
	LocationJson->SetNumberField(TEXT("x"), Location.X);
	LocationJson->SetNumberField(TEXT("y"), Location.Y);
	LocationJson->SetNumberField(TEXT("z"), Location.Z);
	TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
	RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
	TSharedRef<FJsonObject> ScaleJson = MakeShared<FJsonObject>();
	ScaleJson->SetNumberField(TEXT("x"), Scale.X);
	ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
	ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
	Result->SetObjectField(TEXT("location"), LocationJson);
	Result->SetObjectField(TEXT("rotation"), RotationJson);
	Result->SetObjectField(TEXT("scale"), ScaleJson);
	return Result;
}

static FString AnimationModeName(const EAnimationMode::Type Mode)
{
	switch (Mode)
	{
	case EAnimationMode::AnimationBlueprint: return TEXT("animation_blueprint");
	case EAnimationMode::AnimationSingleNode: return TEXT("single_node");
	case EAnimationMode::AnimationCustomMode: return TEXT("custom");
	default: return TEXT("unknown");
	}
}

static UAnimationAsset* GetEffectiveSingleNodeAsset(USkeletalMeshComponent* Component)
{
	if (UAnimSingleNodeInstance* Single = Component ? Component->GetSingleNodeInstance() : nullptr)
	{
		return Single->GetCurrentAsset();
	}
	return Component ? Component->AnimationData.AnimToPlay.Get() : nullptr;
}

static TSharedRef<FJsonObject> MakeAnimationSnapshot(const FResolvedTarget& Target)
{
	USkeletalMeshComponent* Component = Target.Component;
	UAnimInstance* AnimInstance = Component->GetAnimInstance();
	UAnimSingleNodeInstance* Single = Component->GetSingleNodeInstance();
	UAnimationAsset* EffectiveAsset = GetEffectiveSingleNodeAsset(Component);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_path"), Target.Actor->GetPathName());
#if WITH_EDITOR
	Result->SetStringField(TEXT("actor_label"), Target.Actor->GetActorLabel());
#endif
	Result->SetStringField(TEXT("component_path"), Component->GetPathName());
	Result->SetStringField(TEXT("animation_mode"), AnimationModeName(Component->GetAnimationMode()));
	Result->SetStringField(TEXT("anim_class"), Component->AnimClass ? Component->AnimClass->GetPathName() : TEXT(""));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	const TSubclassOf<UAnimInstance> PostProcessClass = Component->GetPostProcessAnimBPClassToBeUsed();
	Result->SetStringField(TEXT("post_process_anim_class"), PostProcessClass ? PostProcessClass->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("post_process_anim_class_semantics"), TEXT("authored_override_or_mesh_default"));
#else
	UAnimInstance* LegacyPostProcessInstance = Component->GetPostProcessInstance();
	Result->SetStringField(TEXT("post_process_anim_class"), LegacyPostProcessInstance ? LegacyPostProcessInstance->GetClass()->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("post_process_anim_class_semantics"), TEXT("live_instance_class; authored component override unavailable_before_ue_5_5"));
#endif
	Result->SetBoolField(TEXT("post_process_disabled"), Component->GetDisablePostProcessBlueprint());
	Result->SetStringField(TEXT("authored_single_node_asset"),
		Component->AnimationData.AnimToPlay ? Component->AnimationData.AnimToPlay->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("authored_loop"), Component->AnimationData.bSavedLooping != 0);
	Result->SetBoolField(TEXT("authored_playing"), Component->AnimationData.bSavedPlaying != 0);
	Result->SetNumberField(TEXT("authored_position_seconds"), Component->AnimationData.SavedPosition);
	Result->SetNumberField(TEXT("authored_play_rate"), Component->AnimationData.SavedPlayRate);
	Result->SetStringField(TEXT("effective_asset"), EffectiveAsset ? EffectiveAsset->GetPathName() : TEXT(""));
	Result->SetNumberField(TEXT("effective_length_seconds"), EffectiveAsset ? EffectiveAsset->GetPlayLength() : 0.0);
	Result->SetBoolField(TEXT("playing"), Single ? Single->IsPlaying() : (AnimInstance ? AnimInstance->IsAnyMontagePlaying() : false));
	Result->SetBoolField(TEXT("loop"), Single ? Single->IsLooping() : false);
	Result->SetNumberField(TEXT("position_seconds"), Single ? Single->GetCurrentTime() : 0.0);
	Result->SetNumberField(TEXT("play_rate"), Single ? Single->GetPlayRate() : 0.0);
	Result->SetStringField(TEXT("anim_instance_class"), AnimInstance ? AnimInstance->GetClass()->GetPathName() : TEXT(""));
	UAnimInstance* PostProcessInstance = Component->GetPostProcessInstance();
	Result->SetStringField(TEXT("post_process_instance_class"),
		PostProcessInstance ? PostProcessInstance->GetClass()->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("driver_source"), Target.bSequencerContext
		? TEXT("sequencer_context_requested")
		: (Component->GetAnimationMode() == EAnimationMode::AnimationSingleNode ? TEXT("skeletal_component_single_node")
			: (Component->GetAnimationMode() == EAnimationMode::AnimationBlueprint ? TEXT("animation_blueprint") : TEXT("custom_animation_mode"))));
	Result->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Result->SetObjectField(TEXT("network"), MakeNetworkIdentity(Target));
	return Result;
}

static bool RequireMutationAuthority(const TSharedRef<FJsonObject>& Arguments, const FResolvedTarget& Target, FString& OutError)
{
	const bool bRequireAuthority = !Arguments->HasField(TEXT("require_authority"))
		|| Arguments->GetBoolField(TEXT("require_authority"));
	if (bRequireAuthority && Target.World->IsGameWorld() && !Target.Actor->HasAuthority())
	{
		OutError = FString::Printf(TEXT("network_authority_required: actor local role is %s"),
			*NetRoleName(Target.Actor->GetLocalRole()));
		return false;
	}
	return true;
}

static TUniquePtr<FScopedTransaction> BeginEditorTransaction(const FResolvedTarget& Target, const FText& Description)
{
	if (Target.World->WorldType != EWorldType::Editor && Target.World->WorldType != EWorldType::EditorPreview)
	{
		return nullptr;
	}
	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(Description);
	Target.Actor->Modify();
	Target.Component->Modify();
	return Transaction;
}

static TSharedRef<FJsonObject> MakeReceipt(
	const FResolvedTarget& Target,
	const FString& Operation,
	const TSharedRef<FJsonObject>& Before,
	const TSharedRef<FJsonObject>& After,
	const bool bTransactional,
	const double StartedAt)
{
	TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("operation"), Operation);
	Receipt->SetStringField(TEXT("actor_path"), Target.Actor->GetPathName());
	Receipt->SetStringField(TEXT("component_path"), Target.Component->GetPathName());
	Receipt->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Receipt->SetObjectField(TEXT("network"), MakeNetworkIdentity(Target));
	Receipt->SetBoolField(TEXT("transactional"), bTransactional);
	Receipt->SetObjectField(TEXT("before"), Before);
	Receipt->SetObjectField(TEXT("after"), After);
	Receipt->SetBoolField(TEXT("readback_verified"), true);
	Receipt->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
	return Receipt;
}

static UClass* ResolveAnimInstanceClass(const FString& Path, FString& OutError)
{
	if (Path.IsEmpty()) return nullptr;
	if (UClass* DirectClass = LoadObject<UClass>(nullptr, *Path))
	{
		if (DirectClass->IsChildOf(UAnimInstance::StaticClass())) return DirectClass;
	}
	if (UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, *Path))
	{
		if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UAnimInstance::StaticClass()))
		{
			return Blueprint->GeneratedClass;
		}
	}
	OutError = FString::Printf(TEXT("anim_instance_class_not_found_or_invalid: %s"), *Path);
	return nullptr;
}

static UAnimationAsset* ResolveAnimationAsset(const FString& Path, FString& OutError)
{
	if (Path.IsEmpty()) return nullptr;
	UAnimationAsset* Asset = LoadObject<UAnimationAsset>(nullptr, *Path);
	if (!Asset) OutError = FString::Printf(TEXT("animation_asset_not_found: %s"), *Path);
	return Asset;
}

static UAnimMontage* ResolveMontage(const FString& Path, FString& OutError)
{
	if (Path.IsEmpty()) return nullptr;
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *Path);
	if (!Montage) OutError = FString::Printf(TEXT("anim_montage_not_found: %s"), *Path);
	return Montage;
}

static UAnimMontage* ResolveRequestedOrActiveMontage(
	const TSharedRef<FJsonObject>& Arguments,
	UAnimInstance* AnimInstance,
	FString& OutError)
{
	FString MontagePath;
	Arguments->TryGetStringField(TEXT("montage_asset_path"), MontagePath);
	if (!MontagePath.IsEmpty()) return ResolveMontage(MontagePath, OutError);
	UAnimMontage* Active = AnimInstance ? AnimInstance->GetCurrentActiveMontage() : nullptr;
	if (!Active) OutError = TEXT("no montage_asset_path was supplied and no active montage exists");
	return Active;
}

static TSharedRef<FJsonObject> MakeMontageState(UAnimInstance* AnimInstance, UAnimMontage* RequestedMontage = nullptr)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!AnimInstance)
	{
		Result->SetBoolField(TEXT("available"), false);
		Result->SetStringField(TEXT("null_reason"), TEXT("no_anim_instance"));
		return Result;
	}
	UAnimMontage* Montage = RequestedMontage ? RequestedMontage : AnimInstance->GetCurrentActiveMontage();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4
	FAnimMontageInstance* Instance = Montage ? AnimInstance->GetInstanceForMontage(Montage) : nullptr;
#else
	FAnimMontageInstance* Instance = Montage ? AnimInstance->GetActiveInstanceForMontage(Montage) : nullptr;
#endif
	Result->SetBoolField(TEXT("available"), Montage != nullptr);
	Result->SetStringField(TEXT("montage_asset"), Montage ? Montage->GetPathName() : TEXT(""));
	Result->SetNumberField(TEXT("length_seconds"), Montage ? Montage->GetPlayLength() : 0.0);
	Result->SetBoolField(TEXT("any_montage_playing"), AnimInstance->IsAnyMontagePlaying());
	Result->SetBoolField(TEXT("has_instance"), Instance != nullptr);
	TArray<TSharedPtr<FJsonValue>> SlotNames;
	if (Montage)
	{
		for (const FSlotAnimationTrack& SlotTrack : Montage->SlotAnimTracks)
		{
			SlotNames.Add(MakeShared<FJsonValueString>(SlotTrack.SlotName.ToString()));
		}
	}
	Result->SetArrayField(TEXT("slot_names"), SlotNames);
	if (Instance)
	{
		Result->SetNumberField(TEXT("instance_id"), Instance->GetInstanceID());
		Result->SetStringField(TEXT("current_section"), Instance->GetCurrentSection().ToString());
		Result->SetStringField(TEXT("next_section"), Instance->GetNextSection().ToString());
		Result->SetStringField(TEXT("sync_group"), Instance->GetSyncGroupName().ToString());
		Result->SetNumberField(TEXT("position_seconds"), Instance->GetPosition());
		Result->SetNumberField(TEXT("previous_position_seconds"), Instance->GetPreviousPosition());
		Result->SetNumberField(TEXT("play_rate"), Instance->GetPlayRate());
		Result->SetNumberField(TEXT("weight"), Instance->GetWeight());
		Result->SetNumberField(TEXT("desired_weight"), Instance->GetDesiredWeight());
		Result->SetNumberField(TEXT("blend_time_seconds"), Instance->GetBlendTime());
		Result->SetBoolField(TEXT("playing"), Instance->IsPlaying());
		Result->SetBoolField(TEXT("active"), Instance->IsActive());
		Result->SetBoolField(TEXT("stopped"), Instance->IsStopped());
		Result->SetBoolField(TEXT("root_motion_disabled"), Instance->IsRootMotionDisabled());
	}
	TSharedRef<FJsonObject> Interruption = MakeShared<FJsonObject>();
	Interruption->SetBoolField(TEXT("available"), false);
	Interruption->SetStringField(TEXT("reason"), TEXT("UAnimInstance and FAnimMontageInstance expose interruption through event delegates, not as retained queryable instance state"));
	Interruption->SetStringField(TEXT("public_api_basis"), TEXT("OnMontageEnded/OnMontageBlendingOutStarted callback parameter bInterrupted; no public retrospective getter"));
	Interruption->SetStringField(TEXT("required_capture"), TEXT("subscribe before playback and retain the callback receipt"));
	Result->SetObjectField(TEXT("interruption"), Interruption);
	return Result;
}

static TSharedRef<FJsonObject> CommonSelectorSchema(const bool bMutation)
{
	TMap<FString, TSharedRef<FJsonObject>> Properties = {
		{TEXT("world_context"), SB::String(TEXT("Explicit world selector."), {TEXT("editor"), TEXT("pie"), TEXT("preview"), TEXT("standalone"), TEXT("sequencer")})},
		{TEXT("sequencer_world_context"), SB::String(TEXT("Required base world for sequencer context."), {TEXT("editor"), TEXT("pie")})},
		{TEXT("pie_instance"), SB::Integer(TEXT("PIE instance id when multiple PIE worlds exist."))},
		{TEXT("net_mode"), SB::String(TEXT("Optional exact net-mode selector."), {TEXT("standalone"), TEXT("dedicated_server"), TEXT("listen_server"), TEXT("client")})},
		{TEXT("world_path"), SB::String(TEXT("Optional exact world or package path selector."))},
		{TEXT("actor_id"), SB::String(TEXT("Exact actor path, object name, or editor label."), {}, 1)},
		{TEXT("component_id"), SB::String(TEXT("Exact skeletal component path or object name; required when actor has multiple skeletal components."))}
	};
	if (bMutation) Properties.Add(TEXT("require_authority"), SB::Boolean(TEXT("Default true in game worlds; rejects writes on non-authority roles.")));
	return SB::Object(Properties, {TEXT("world_context"), TEXT("actor_id")}, TEXT("Explicit character target selector."), false);
}

static TMap<FString, TSharedRef<FJsonObject>> CommonSelectorProperties(const bool bMutation)
{
	TMap<FString, TSharedRef<FJsonObject>> Properties = {
		{TEXT("world_context"), SB::String(TEXT("Explicit world selector."), {TEXT("editor"), TEXT("pie"), TEXT("preview"), TEXT("standalone"), TEXT("sequencer")})},
		{TEXT("sequencer_world_context"), SB::String(TEXT("Required base world for sequencer context."), {TEXT("editor"), TEXT("pie")})},
		{TEXT("pie_instance"), SB::Integer(TEXT("PIE instance id."))},
		{TEXT("net_mode"), SB::String(TEXT("Exact net mode."), {TEXT("standalone"), TEXT("dedicated_server"), TEXT("listen_server"), TEXT("client")})},
		{TEXT("world_path"), SB::String(TEXT("Exact world/package path."))},
		{TEXT("actor_id"), SB::String(TEXT("Exact actor identity."), {}, 1)},
		{TEXT("component_id"), SB::String(TEXT("Exact skeletal component identity."))}
	};
	if (bMutation) Properties.Add(TEXT("require_authority"), SB::Boolean(TEXT("Default true.")));
	return Properties;
}

} // namespace CharacterActionRuntime
} // namespace UE::SOMOLMCP

namespace UE::SOMOLMCP
{
namespace CharacterActionRuntime
{

static bool ExecuteAnimationInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	Out->SetObjectField(TEXT("state"), MakeAnimationSnapshot(Target));
	Out->SetObjectField(TEXT("montage"), MakeMontageState(Target.Component->GetAnimInstance()));
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = FString::Printf(TEXT("Inspected character animation state for %s"), *Target.Component->GetPathName());
	return true;
}

static bool ExecuteActionStateInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	FString MontagePath;
	Arguments->TryGetStringField(TEXT("montage_asset_path"), MontagePath);
	UAnimMontage* Montage = nullptr;
	if (!MontagePath.IsEmpty() && !(Montage = ResolveMontage(MontagePath, Error))) return false;
	Out->SetObjectField(TEXT("state"), MakeMontageState(AnimInstance, Montage));
	Out->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Out->SetObjectField(TEXT("network"), MakeNetworkIdentity(Target));
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = FString::Printf(TEXT("Inspected Montage action state for %s"), *Target.Component->GetPathName());
	return true;
}

static bool ExecuteStateMachineInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	if (!AnimInstance) { Error = TEXT("no_anim_instance"); return false; }
	const IAnimClassInterface* Interface = IAnimClassInterface::GetFromClass(AnimInstance->GetClass());
	if (!Interface) { Error = TEXT("anim_instance_class_has_no_IAnimClassInterface"); return false; }
	FString MachineFilter;
	Arguments->TryGetStringField(TEXT("machine_name"), MachineFilter);
	const int32 MaxMachines = Arguments->HasField(TEXT("max_machines"))
		? FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_machines"))), 1, 256) : 64;
	const TArray<FBakedAnimationStateMachine>& Machines = Interface->GetBakedStateMachines();
	TArray<TSharedPtr<FJsonValue>> MachineRows;
	for (int32 MachineIndex = 0; MachineIndex < Machines.Num() && MachineRows.Num() < MaxMachines; ++MachineIndex)
	{
		const FBakedAnimationStateMachine& Description = Machines[MachineIndex];
		if (!MachineFilter.IsEmpty() && Description.MachineName.ToString() != MachineFilter) continue;
		const FAnimNode_StateMachine* Runtime = AnimInstance->GetStateMachineInstance(MachineIndex);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("machine_name"), Description.MachineName.ToString());
		Row->SetNumberField(TEXT("machine_index"), MachineIndex);
		Row->SetNumberField(TEXT("initial_state_index"), Description.InitialState);
		Row->SetBoolField(TEXT("runtime_available"), Runtime != nullptr);
		Row->SetNumberField(TEXT("current_state_index"), Runtime ? Runtime->GetCurrentState() : INDEX_NONE);
		Row->SetStringField(TEXT("current_state_name"), Runtime ? Runtime->GetCurrentStateName().ToString() : TEXT(""));
		Row->SetNumberField(TEXT("current_state_elapsed_seconds"), Runtime ? Runtime->GetCurrentStateElapsedTime() : 0.0);
		TArray<TSharedPtr<FJsonValue>> StateRows;
		for (int32 StateIndex = 0; StateIndex < Description.States.Num(); ++StateIndex)
		{
			const FBakedAnimationState& State = Description.States[StateIndex];
			TSharedRef<FJsonObject> StateRow = MakeShared<FJsonObject>();
			StateRow->SetNumberField(TEXT("state_index"), StateIndex);
			StateRow->SetStringField(TEXT("state_name"), State.StateName.ToString());
			StateRow->SetBoolField(TEXT("is_conduit"), State.bIsAConduit);
			StateRow->SetNumberField(TEXT("weight"), Runtime ? Runtime->GetStateWeight(StateIndex) : 0.0);
			StateRow->SetBoolField(TEXT("is_current"), Runtime && Runtime->GetCurrentState() == StateIndex);
			StateRow->SetNumberField(TEXT("relevant_time_seconds"), Runtime ? AnimInstance->GetRelevantAnimTime(MachineIndex, StateIndex) : 0.0);
			StateRow->SetNumberField(TEXT("relevant_time_fraction"), Runtime ? AnimInstance->GetRelevantAnimTimeFraction(MachineIndex, StateIndex) : 0.0);
			StateRow->SetNumberField(TEXT("relevant_time_remaining_seconds"), Runtime ? AnimInstance->GetRelevantAnimTimeRemaining(MachineIndex, StateIndex) : 0.0);
			StateRow->SetNumberField(TEXT("relevant_time_remaining_fraction"), Runtime ? AnimInstance->GetRelevantAnimTimeRemainingFraction(MachineIndex, StateIndex) : 0.0);
			StateRows.Add(MakeShared<FJsonValueObject>(StateRow));
		}
		Row->SetArrayField(TEXT("states"), StateRows);
		TArray<TSharedPtr<FJsonValue>> TransitionRows;
		if (Runtime)
		{
			for (int32 TransitionIndex = 0; TransitionIndex < Description.Transitions.Num(); ++TransitionIndex)
			{
				const FAnimationTransitionBetweenStates& Transition = Description.Transitions[TransitionIndex];
				if (!Runtime->IsTransitionActive(TransitionIndex)) continue;
				TSharedRef<FJsonObject> TransitionRow = MakeShared<FJsonObject>();
				TransitionRow->SetNumberField(TEXT("transition_index"), TransitionIndex);
				TransitionRow->SetNumberField(TEXT("previous_state_index"), Transition.PreviousState);
				TransitionRow->SetNumberField(TEXT("next_state_index"), Transition.NextState);
				TransitionRow->SetNumberField(TEXT("elapsed_seconds"), AnimInstance->GetInstanceTransitionTimeElapsed(MachineIndex, TransitionIndex));
				TransitionRow->SetNumberField(TEXT("elapsed_fraction"), AnimInstance->GetInstanceTransitionTimeElapsedFraction(MachineIndex, TransitionIndex));
				TransitionRow->SetNumberField(TEXT("crossfade_duration_seconds"), Transition.CrossfadeDuration);
				TransitionRow->SetBoolField(TEXT("active"), true);
				TransitionRows.Add(MakeShared<FJsonValueObject>(TransitionRow));
			}
		}
		Row->SetArrayField(TEXT("active_transitions"), TransitionRows);
		MachineRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("state_machines"), MachineRows);
	Out->SetNumberField(TEXT("total_baked_machines"), Machines.Num());
	Out->SetBoolField(TEXT("truncated"), MachineRows.Num() >= MaxMachines && MachineRows.Num() < Machines.Num());
	Out->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = FString::Printf(TEXT("Inspected %d state machines"), MachineRows.Num());
	return true;
}

static bool ExecuteSyncGroupInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	if (!AnimInstance) { Error = TEXT("no_anim_instance"); return false; }
	const IAnimClassInterface* Interface = IAnimClassInterface::GetFromClass(AnimInstance->GetClass());
	if (!Interface) { Error = TEXT("anim_instance_class_has_no_IAnimClassInterface"); return false; }
	const TMap<FName, FAnimGroupInstance>& RuntimeGroups = AnimInstance->GetSyncGroupMapRead();
	FString Filter;
	Arguments->TryGetStringField(TEXT("sync_group_name"), Filter);
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FName GroupName : Interface->GetSyncGroupNames())
	{
		if (!Filter.IsEmpty() && GroupName.ToString() != Filter) continue;
		const FMarkerSyncAnimPosition Position = AnimInstance->GetSyncGroupPosition(GroupName);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("sync_group_name"), GroupName.ToString());
		Row->SetNumberField(TEXT("sync_group_index"), Interface->GetSyncGroupIndex(GroupName));
		Row->SetBoolField(TEXT("marker_position_valid"), Position.IsValid());
		Row->SetStringField(TEXT("previous_marker"), Position.PreviousMarkerName.ToString());
		Row->SetStringField(TEXT("next_marker"), Position.NextMarkerName.ToString());
		Row->SetNumberField(TEXT("position_between_markers"), Position.PositionBetweenMarkers);
		const FAnimGroupInstance* RuntimeGroup = RuntimeGroups.Find(GroupName);
		Row->SetBoolField(TEXT("runtime_group_available"), RuntimeGroup != nullptr);
		TArray<TSharedPtr<FJsonValue>> Players;
		if (RuntimeGroup)
		{
			Row->SetNumberField(TEXT("leader_index"), RuntimeGroup->GroupLeaderIndex);
			Row->SetBoolField(TEXT("can_use_marker_sync"), RuntimeGroup->bCanUseMarkerSync);
			Row->SetNumberField(TEXT("montage_leader_weight"), RuntimeGroup->MontageLeaderWeight);
			Row->SetNumberField(TEXT("previous_animation_length_ratio"), RuntimeGroup->PreviousAnimLengthRatio);
			Row->SetNumberField(TEXT("animation_length_ratio"), RuntimeGroup->AnimLengthRatio);
			for (int32 PlayerIndex = 0; PlayerIndex < RuntimeGroup->ActivePlayers.Num(); ++PlayerIndex)
			{
				const FAnimTickRecord& Player = RuntimeGroup->ActivePlayers[PlayerIndex];
				TSharedRef<FJsonObject> PlayerRow = MakeShared<FJsonObject>();
				PlayerRow->SetNumberField(TEXT("player_index"), PlayerIndex);
				PlayerRow->SetBoolField(TEXT("is_leader"), PlayerIndex == RuntimeGroup->GroupLeaderIndex);
				PlayerRow->SetStringField(TEXT("source_asset"), Player.SourceAsset ? Player.SourceAsset->GetPathName() : TEXT(""));
				PlayerRow->SetNumberField(TEXT("effective_blend_weight"), Player.EffectiveBlendWeight);
				PlayerRow->SetNumberField(TEXT("root_motion_weight"), Player.GetRootMotionWeight());
				PlayerRow->SetNumberField(TEXT("leader_score"), Player.LeaderScore);
				PlayerRow->SetNumberField(TEXT("play_rate_multiplier"), Player.PlayRateMultiplier);
				PlayerRow->SetNumberField(TEXT("time_seconds"), Player.TimeAccumulator ? *Player.TimeAccumulator : 0.0);
				PlayerRow->SetBoolField(TEXT("time_available"), Player.TimeAccumulator != nullptr);
				PlayerRow->SetBoolField(TEXT("looping"), Player.bLooping);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				PlayerRow->SetBoolField(TEXT("exclusive_leader"), Player.bIsExclusiveLeader);
				PlayerRow->SetBoolField(TEXT("exclusive_leader_available"), true);
#else
				PlayerRow->SetBoolField(TEXT("exclusive_leader"), false);
				PlayerRow->SetBoolField(TEXT("exclusive_leader_available"), false);
#endif
				Players.Add(MakeShared<FJsonValueObject>(PlayerRow));
			}
		}
		Row->SetArrayField(TEXT("active_players"), Players);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("sync_groups"), Rows);
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Out->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Summary = FString::Printf(TEXT("Inspected %d Sync Groups"), Rows.Num());
	return true;
}

static bool ExecuteCurveInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	if (!AnimInstance) { Error = TEXT("no_anim_instance"); return false; }
	const int32 MaxCurves = Arguments->HasField(TEXT("max_curves"))
		? FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_curves"))), 1, 4096) : 512;
	FString TypeName = TEXT("attribute");
	Arguments->TryGetStringField(TEXT("curve_type"), TypeName);
	TypeName = TypeName.TrimStartAndEnd().ToLower();
	EAnimCurveType CurveType = EAnimCurveType::AttributeCurve;
	if (TypeName == TEXT("morph_target")) CurveType = EAnimCurveType::MorphTargetCurve;
	else if (TypeName == TEXT("material")) CurveType = EAnimCurveType::MaterialCurve;
	else if (TypeName != TEXT("attribute")) { Error = TEXT("curve_type must be attribute, morph_target, or material"); return false; }
	TArray<FName> Names;
	AnimInstance->GetActiveCurveNames(CurveType, Names);
	Names.Sort(FNameLexicalLess());
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (int32 Index = 0; Index < Names.Num() && Index < MaxCurves; ++Index)
	{
		float Value = 0.0f;
		const bool bFound = AnimInstance->GetCurveValue(Names[Index], Value);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Names[Index].ToString());
		Row->SetNumberField(TEXT("value"), Value);
		Row->SetBoolField(TEXT("found"), bFound);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetStringField(TEXT("curve_type"), TypeName);
	Out->SetArrayField(TEXT("curves"), Rows);
	Out->SetNumberField(TEXT("total_active"), Names.Num());
	Out->SetBoolField(TEXT("truncated"), Names.Num() > MaxCurves);
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = FString::Printf(TEXT("Inspected %d/%d active curves"), Rows.Num(), Names.Num());
	return true;
}

static bool ExecuteNotifyInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	if (!AnimInstance) { Error = TEXT("no_anim_instance"); return false; }
	const int32 MaxNotifies = Arguments->HasField(TEXT("max_notifies"))
		? FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_notifies"))), 1, 4096) : 512;
	TArray<TSharedPtr<FJsonValue>> ActiveStates;
	for (int32 Index = 0; Index < AnimInstance->ActiveAnimNotifyState.Num() && Index < MaxNotifies; ++Index)
	{
		const FAnimNotifyEvent& Event = AnimInstance->ActiveAnimNotifyState[Index];
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("notify_name"), Event.NotifyName.ToString());
		Row->SetStringField(TEXT("notify_class"), Event.Notify ? Event.Notify->GetClass()->GetPathName() : TEXT(""));
		Row->SetStringField(TEXT("notify_state_class"), Event.NotifyStateClass ? Event.NotifyStateClass->GetClass()->GetPathName() : TEXT(""));
		Row->SetNumberField(TEXT("trigger_time_seconds"), Event.GetTriggerTime());
		Row->SetNumberField(TEXT("duration_seconds"), Event.GetDuration());
		Row->SetBoolField(TEXT("motion_warping_window"), Event.NotifyStateClass
			&& Event.NotifyStateClass->GetClass()->GetName().Contains(TEXT("MotionWarping")));
		ActiveStates.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("active_notify_states"), ActiveStates);
	TArray<TSharedPtr<FJsonValue>> QueuedEvents;
	for (int32 Index = 0; Index < AnimInstance->NotifyQueue.AnimNotifies.Num() && Index < MaxNotifies; ++Index)
	{
		const FAnimNotifyEventReference& Reference = AnimInstance->NotifyQueue.AnimNotifies[Index];
		const FAnimNotifyEvent* Event = Reference.GetNotify();
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetBoolField(TEXT("event_available"), Event != nullptr);
		Row->SetStringField(TEXT("source_object"), Reference.GetSourceObject() ? Reference.GetSourceObject()->GetPathName() : TEXT(""));
		if (Event)
		{
			Row->SetStringField(TEXT("notify_name"), Event->NotifyName.ToString());
			Row->SetStringField(TEXT("event_name"), Event->GetNotifyEventName().ToString());
			Row->SetStringField(TEXT("notify_class"), Event->Notify ? Event->Notify->GetClass()->GetPathName() : TEXT(""));
			Row->SetStringField(TEXT("notify_state_class"), Event->NotifyStateClass ? Event->NotifyStateClass->GetClass()->GetPathName() : TEXT(""));
			Row->SetNumberField(TEXT("trigger_time_seconds"), Event->GetTriggerTime());
			Row->SetNumberField(TEXT("duration_seconds"), Event->GetDuration());
		}
		QueuedEvents.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("queued_notify_events"), QueuedEvents);
	Out->SetNumberField(TEXT("active_notify_state_count"), AnimInstance->ActiveAnimNotifyState.Num());
	Out->SetNumberField(TEXT("queued_notify_event_count"), AnimInstance->NotifyQueue.AnimNotifies.Num());
	Out->SetBoolField(TEXT("truncated"), AnimInstance->ActiveAnimNotifyState.Num() > MaxNotifies
		|| AnimInstance->NotifyQueue.AnimNotifies.Num() > MaxNotifies);
	Out->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = FString::Printf(TEXT("Inspected %d active notify states and %d queued notify events"),
		ActiveStates.Num(), QueuedEvents.Num());
	return true;
}

static FString RootMotionModeName(const uint8 Mode)
{
	switch (static_cast<ERootMotionMode::Type>(Mode))
	{
	case ERootMotionMode::NoRootMotionExtraction: return TEXT("no_root_motion_extraction");
	case ERootMotionMode::IgnoreRootMotion: return TEXT("ignore_root_motion");
	case ERootMotionMode::RootMotionFromEverything: return TEXT("root_motion_from_everything");
	case ERootMotionMode::RootMotionFromMontagesOnly: return TEXT("root_motion_from_montages_only");
	default: return TEXT("unknown");
	}
}

static uint8 ReadRootMotionMode(UAnimInstance* AnimInstance)
{
	if (!AnimInstance) return static_cast<uint8>(ERootMotionMode::NoRootMotionExtraction);
	if (const FByteProperty* Property = FindFProperty<FByteProperty>(AnimInstance->GetClass(), TEXT("RootMotionMode")))
	{
		return Property->GetPropertyValue_InContainer(AnimInstance);
	}
	return static_cast<uint8>(ERootMotionMode::NoRootMotionExtraction);
}

struct FRootMotionObservation
{
	FTransform ComponentTransform;
	double WorldTimeSeconds = 0.0;
	uint64 FrameCounter = 0;
};

static TMap<TWeakObjectPtr<USkeletalMeshComponent>, FRootMotionObservation> GRootMotionObservations;

static TSharedRef<FJsonObject> MakeRootMotionState(const FResolvedTarget& Target)
{
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	ACharacter* Character = Cast<ACharacter>(Target.Actor);
	UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
	FAnimMontageInstance* RootMontage = AnimInstance ? AnimInstance->GetRootMotionMontageInstance() : nullptr;
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("anim_instance_available"), AnimInstance != nullptr);
	Result->SetStringField(TEXT("root_motion_mode"), RootMotionModeName(ReadRootMotionMode(AnimInstance)));
	Result->SetBoolField(TEXT("root_motion_montage_active"), RootMontage != nullptr);
	Result->SetStringField(TEXT("root_motion_montage"), RootMontage && RootMontage->Montage ? RootMontage->Montage->GetPathName() : TEXT(""));
	Result->SetNumberField(TEXT("root_motion_montage_position_seconds"), RootMontage ? RootMontage->GetPosition() : 0.0);
	Result->SetBoolField(TEXT("movement_component_available"), Movement != nullptr);
	Result->SetBoolField(TEXT("movement_has_anim_root_motion"), Movement ? Movement->HasAnimRootMotion() : false);
	Result->SetBoolField(TEXT("movement_has_root_motion_sources"), Movement ? Movement->HasRootMotionSources() : false);
	Result->SetStringField(TEXT("movement_consumer"), Movement ? Movement->GetPathName() : TEXT(""));
	TArray<TSharedPtr<FJsonValue>> SourceRows;
	if (Movement)
	{
		for (const TSharedPtr<FRootMotionSource>& Source : Movement->CurrentRootMotion.RootMotionSources)
		{
			if (!Source.IsValid()) continue;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("instance_name"), Source->InstanceName.ToString());
			Row->SetNumberField(TEXT("local_id"), Source->LocalID);
			Row->SetNumberField(TEXT("priority"), Source->Priority);
			Row->SetNumberField(TEXT("current_time_seconds"), Source->GetTime());
			Row->SetNumberField(TEXT("duration_seconds"), Source->GetDuration());
			Row->SetBoolField(TEXT("active"), Source->IsActive());
			Row->SetBoolField(TEXT("local_space"), Source->bInLocalSpace);
			Row->SetStringField(TEXT("description"), Source->ToSimpleString());
			Row->SetBoolField(TEXT("generated_delta_available"), Source->RootMotionParams.bHasRootMotion);
			Row->SetNumberField(TEXT("generated_delta_blend_weight"), Source->RootMotionParams.BlendWeight);
			if (Source->RootMotionParams.bHasRootMotion)
			{
				Row->SetObjectField(TEXT("generated_delta"), MakeTransformJson(Source->RootMotionParams.GetRootMotionTransform()));
			}
			SourceRows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Result->SetArrayField(TEXT("root_motion_sources"), SourceRows);

	TSharedRef<FJsonObject> Extracted = MakeShared<FJsonObject>();
	Extracted->SetBoolField(TEXT("available"), false);
	Extracted->SetBoolField(TEXT("non_consuming_read"), true);
	Extracted->SetStringField(TEXT("reason"), TEXT("UE 5.8 exposes no public non-consuming getter for UAnimInstance pending extracted root motion; ConsumeExtractedRootMotion would mutate playback state"));
	Extracted->SetStringField(TEXT("rejected_api"), TEXT("UAnimInstance::GetProxyOnGameThread is protected; ConsumeExtractedRootMotion is destructive"));
	Result->SetObjectField(TEXT("pending_extracted_delta"), Extracted);

	for (auto It = GRootMotionObservations.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	const FTransform CurrentTransform = Target.Component->GetComponentTransform();
	const double CurrentWorldTime = Target.World->GetTimeSeconds();
	TSharedRef<FJsonObject> Observed = MakeShared<FJsonObject>();
	if (const FRootMotionObservation* Previous = GRootMotionObservations.Find(Target.Component))
	{
		Observed->SetBoolField(TEXT("available"), true);
		Observed->SetNumberField(TEXT("elapsed_world_seconds"), CurrentWorldTime - Previous->WorldTimeSeconds);
		Observed->SetNumberField(TEXT("elapsed_engine_frames"), static_cast<double>(GFrameCounter - Previous->FrameCounter));
		Observed->SetObjectField(TEXT("component_delta"), MakeTransformJson(CurrentTransform.GetRelativeTransform(Previous->ComponentTransform)));
		Observed->SetStringField(TEXT("attribution"), TEXT("observed component movement; public APIs cannot prove what fraction was consumed animation root motion"));
	}
	else
	{
		Observed->SetBoolField(TEXT("available"), false);
		Observed->SetStringField(TEXT("reason"), TEXT("first inspector sample; call again after one or more world ticks"));
	}
	Observed->SetStringField(TEXT("sample_frame_counter"), FString::Printf(TEXT("%llu"), GFrameCounter));
	Result->SetObjectField(TEXT("observed_delta_since_previous_inspect"), Observed);
	FRootMotionObservation NewObservation;
	NewObservation.ComponentTransform = CurrentTransform;
	NewObservation.WorldTimeSeconds = CurrentWorldTime;
	NewObservation.FrameCounter = GFrameCounter;
	GRootMotionObservations.Add(Target.Component, MoveTemp(NewObservation));

	TSharedRef<FJsonObject> Consumed = MakeShared<FJsonObject>();
	Consumed->SetBoolField(TEXT("available"), false);
	Consumed->SetStringField(TEXT("reason"), TEXT("UCharacterMovementComponent::RootMotionParams is protected and valid only during PerformMovement; no public retained consumed-delta getter exists"));
	Consumed->SetStringField(TEXT("safe_alternative"), TEXT("pending_extracted_delta plus observed_delta_since_previous_inspect, with attribution explicitly unresolved"));
	Result->SetObjectField(TEXT("exact_consumed_animation_delta"), Consumed);
	Result->SetObjectField(TEXT("network"), MakeNetworkIdentity(Target));
	Result->SetStringField(TEXT("pending_extracted_delta_policy"), TEXT("not_consumed_by_inspector"));
	return Result;
}

static bool ExecuteRootMotionInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	Out->SetObjectField(TEXT("state"), MakeRootMotionState(Target));
	Out->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = TEXT("Inspected root motion state without consuming extracted motion");
	return true;
}

static bool ExecuteRootMotionSet(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error) || !RequireMutationAuthority(Arguments, Target, Error)) return false;
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	if (!AnimInstance) { Error = TEXT("no_anim_instance"); return false; }
	FString Mode;
	if (!Arguments->TryGetStringField(TEXT("root_motion_mode"), Mode)) { Error = TEXT("root_motion_mode is required"); return false; }
	Mode = Mode.TrimStartAndEnd().ToLower();
	ERootMotionMode::Type NewMode;
	if (Mode == TEXT("no_root_motion_extraction")) NewMode = ERootMotionMode::NoRootMotionExtraction;
	else if (Mode == TEXT("ignore_root_motion")) NewMode = ERootMotionMode::IgnoreRootMotion;
	else if (Mode == TEXT("root_motion_from_everything")) NewMode = ERootMotionMode::RootMotionFromEverything;
	else if (Mode == TEXT("root_motion_from_montages_only")) NewMode = ERootMotionMode::RootMotionFromMontagesOnly;
	else { Error = TEXT("invalid root_motion_mode"); return false; }
	const double StartedAt = FPlatformTime::Seconds();
	TSharedRef<FJsonObject> Before = MakeRootMotionState(Target);
	TUniquePtr<FScopedTransaction> Transaction = BeginEditorTransaction(Target,
		NSLOCTEXT("SOMOLMCP", "CharacterRootMotionSet", "SOMOLMCP Character Root Motion Set"));
	AnimInstance->Modify();
	AnimInstance->SetRootMotionMode(NewMode);
	TSharedRef<FJsonObject> After = MakeRootMotionState(Target);
	if (After->GetStringField(TEXT("root_motion_mode")) != Mode)
	{
		Error = TEXT("root_motion_mode readback mismatch");
		return false;
	}
	Out->SetObjectField(TEXT("receipt"), MakeReceipt(Target, TEXT("character_root_motion_set"), Before, After,
		Transaction.IsValid(), StartedAt));
	Out->SetObjectField(TEXT("state"), After);
	Summary = FString::Printf(TEXT("Set root motion mode to %s"), *Mode);
	return true;
}

static UActorComponent* FindMotionWarpingComponent(AActor* Actor)
{
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (UActorComponent* Component : Components)
	{
		if (Component && Component->GetClass()->GetName().Contains(TEXT("MotionWarpingComponent"))) return Component;
	}
	return nullptr;
}

static bool InvokeMotionWarpingTargetMutation(
	UActorComponent* Component,
	const FString& Operation,
	const FName TargetName,
	const FTransform& TargetTransform,
	FString& Error)
{
	const FName FunctionName = Operation == TEXT("remove") ? TEXT("RemoveWarpTarget") : TEXT("AddOrUpdateWarpTargetFromTransform");
	UFunction* Function = Component->FindFunction(FunctionName);
	if (!Function) { Error = FString::Printf(TEXT("MotionWarping function unavailable: %s"), *FunctionName.ToString()); return false; }
	FStructOnScope Parameters(Function);
	if (FNameProperty* NameProperty = FindFProperty<FNameProperty>(Function, TEXT("WarpTargetName")))
	{
		NameProperty->SetPropertyValue_InContainer(Parameters.GetStructMemory(), TargetName);
	}
	else { Error = TEXT("MotionWarping WarpTargetName parameter missing"); return false; }
	if (Operation != TEXT("remove"))
	{
		FStructProperty* TransformProperty = FindFProperty<FStructProperty>(Function, TEXT("TargetTransform"));
		if (!TransformProperty || TransformProperty->Struct != TBaseStructure<FTransform>::Get())
		{
			Error = TEXT("MotionWarping TargetTransform parameter missing or incompatible");
			return false;
		}
		*TransformProperty->ContainerPtrToValuePtr<FTransform>(Parameters.GetStructMemory()) = TargetTransform;
	}
	Component->ProcessEvent(Function, Parameters.GetStructMemory());
	return true;
}

static TSharedRef<FJsonObject> MakeMotionWarpingState(const FResolvedTarget& Target)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	UActorComponent* Component = FindMotionWarpingComponent(Target.Actor);
	Result->SetBoolField(TEXT("component_available"), Component != nullptr);
	Result->SetStringField(TEXT("component_path"), Component ? Component->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("component_class"), Component ? Component->GetClass()->GetPathName() : TEXT(""));
	TArray<TSharedPtr<FJsonValue>> Targets;
	if (Component)
	{
		if (FArrayProperty* WarpTargetsProperty = FindFProperty<FArrayProperty>(Component->GetClass(), TEXT("WarpTargets")))
		{
			if (FStructProperty* InnerStruct = CastField<FStructProperty>(WarpTargetsProperty->Inner))
			{
				void* ArrayPtr = WarpTargetsProperty->ContainerPtrToValuePtr<void>(Component);
				FScriptArrayHelper Helper(WarpTargetsProperty, ArrayPtr);
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					TSharedRef<FJsonObject> TargetJson = MakeShared<FJsonObject>();
					FJsonObjectConverter::UStructToJsonObject(InnerStruct->Struct, Helper.GetRawPtr(Index), TargetJson, 0, 0);
					Targets.Add(MakeShared<FJsonValueObject>(TargetJson));
				}
			}
		}
	}
	Result->SetArrayField(TEXT("warp_targets"), Targets);
	Result->SetNumberField(TEXT("warp_target_count"), Targets.Num());
	TArray<TSharedPtr<FJsonValue>> ActiveWindows;
	if (UAnimInstance* AnimInstance = Target.Component->GetAnimInstance())
	{
		for (const FAnimNotifyEvent& Event : AnimInstance->ActiveAnimNotifyState)
		{
			if (Event.NotifyStateClass && Event.NotifyStateClass->GetClass()->GetName().Contains(TEXT("MotionWarping")))
			{
				TSharedRef<FJsonObject> Window = MakeShared<FJsonObject>();
				Window->SetStringField(TEXT("class"), Event.NotifyStateClass->GetClass()->GetPathName());
				Window->SetStringField(TEXT("notify_name"), Event.NotifyName.ToString());
				Window->SetNumberField(TEXT("trigger_time_seconds"), Event.GetTriggerTime());
				Window->SetNumberField(TEXT("duration_seconds"), Event.GetDuration());
				ActiveWindows.Add(MakeShared<FJsonValueObject>(Window));
			}
		}
	}
	Result->SetArrayField(TEXT("active_windows"), ActiveWindows);
	return Result;
}

static bool ExecuteMotionWarpingInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	Out->SetObjectField(TEXT("state"), MakeMotionWarpingState(Target));
	Out->SetObjectField(TEXT("world"), MakeWorldIdentity(Target));
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = TEXT("Inspected native reflected Motion Warping component state and active windows");
	return true;
}

static bool ExecuteMotionWarpingTargetSet(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error) || !RequireMutationAuthority(Arguments, Target, Error)) return false;
	UActorComponent* MotionWarping = FindMotionWarpingComponent(Target.Actor);
	if (!MotionWarping) { Error = TEXT("motion_warping_component_not_found"); return false; }
	FString TargetNameString;
	FString Operation = TEXT("upsert");
	if (!Arguments->TryGetStringField(TEXT("target_name"), TargetNameString) || TargetNameString.IsEmpty())
	{
		Error = TEXT("target_name is required"); return false;
	}
	Arguments->TryGetStringField(TEXT("operation"), Operation);
	Operation = Operation.TrimStartAndEnd().ToLower();
	if (Operation != TEXT("upsert") && Operation != TEXT("remove")) { Error = TEXT("operation must be upsert or remove"); return false; }
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	if (Operation == TEXT("upsert"))
	{
		const TSharedPtr<FJsonObject>* TransformObject = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("transform"), TransformObject) || !TransformObject || !TransformObject->IsValid())
		{
			Error = TEXT("upsert requires transform"); return false;
		}
		const TSharedPtr<FJsonObject>* LocationObject = nullptr;
		if ((*TransformObject)->TryGetObjectField(TEXT("location"), LocationObject) && LocationObject && LocationObject->IsValid())
		{
			Location.X = (*LocationObject)->GetNumberField(TEXT("x"));
			Location.Y = (*LocationObject)->GetNumberField(TEXT("y"));
			Location.Z = (*LocationObject)->GetNumberField(TEXT("z"));
		}
		const TSharedPtr<FJsonObject>* RotationObject = nullptr;
		if ((*TransformObject)->TryGetObjectField(TEXT("rotation"), RotationObject) && RotationObject && RotationObject->IsValid())
		{
			Rotation.Pitch = (*RotationObject)->GetNumberField(TEXT("pitch"));
			Rotation.Yaw = (*RotationObject)->GetNumberField(TEXT("yaw"));
			Rotation.Roll = (*RotationObject)->GetNumberField(TEXT("roll"));
		}
		if (Location.ContainsNaN() || Rotation.ContainsNaN()) { Error = TEXT("transform contains non-finite values"); return false; }
	}
	const double StartedAt = FPlatformTime::Seconds();
	TSharedRef<FJsonObject> Before = MakeMotionWarpingState(Target);
	TUniquePtr<FScopedTransaction> Transaction = BeginEditorTransaction(Target,
		NSLOCTEXT("SOMOLMCP", "CharacterMotionWarpingTargetSet", "SOMOLMCP Motion Warping Target Set"));
	MotionWarping->Modify();
	if (!InvokeMotionWarpingTargetMutation(MotionWarping, Operation, FName(*TargetNameString), FTransform(Rotation, Location), Error)) return false;
	TSharedRef<FJsonObject> After = MakeMotionWarpingState(Target);
	Out->SetObjectField(TEXT("receipt"), MakeReceipt(Target, TEXT("character_motion_warping_target_set"), Before, After,
		Transaction.IsValid(), StartedAt));
	Out->SetObjectField(TEXT("state"), After);
	Summary = FString::Printf(TEXT("%s Motion Warping target %s"), Operation == TEXT("remove") ? TEXT("Removed") : TEXT("Upserted"), *TargetNameString);
	return true;
}

static bool SequencerObjectMatchesTarget(UObject* Object, const FResolvedTarget& Target)
{
	if (!Object) return false;
	if (Object == Target.Component || Object == Target.Actor) return true;
	if (const UActorComponent* Component = Cast<UActorComponent>(Object))
	{
		return Component == Target.Component || Component->GetOwner() == Target.Actor;
	}
	return Cast<AActor>(Object) == Target.Actor;
}

static TSharedRef<FJsonObject> InspectSequencerProvenance(
	const FResolvedTarget& Target,
	const TSharedRef<FJsonObject>& Arguments,
	int32& OutActiveSections,
	bool& OutAnyLiveAtRequestedFrame)
{
	OutActiveSections = 0;
	OutAnyLiveAtRequestedFrame = false;
	const int32 DisplayFrame = static_cast<int32>(Arguments->GetNumberField(TEXT("sequencer_frame")));
	FString SequenceActorFilter;
	Arguments->TryGetStringField(TEXT("sequence_actor_id"), SequenceActorFilter);
	FString SequencePathFilter;
	Arguments->TryGetStringField(TEXT("sequence_path"), SequencePathFilter);
	TArray<TSharedPtr<FJsonValue>> SequenceRows;
	int32 SequenceActorsExamined = 0;
	int32 MatchedBindings = 0;
	for (TActorIterator<ALevelSequenceActor> It(Target.World); It; ++It)
	{
		ALevelSequenceActor* SequenceActor = *It;
		if (!SequenceActor) continue;
		if (!SequenceActorFilter.IsEmpty()
			&& SequenceActor->GetPathName() != SequenceActorFilter
			&& SequenceActor->GetName() != SequenceActorFilter
			&& SequenceActor->GetActorLabel() != SequenceActorFilter) continue;
		ULevelSequence* Sequence = SequenceActor->GetSequence();
		if (!Sequence || (!SequencePathFilter.IsEmpty() && Sequence->GetPathName() != SequencePathFilter)) continue;
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene) continue;
		++SequenceActorsExamined;
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber RequestedTickFrame = FFrameRate::TransformTime(
			FFrameTime(FFrameNumber(DisplayFrame)), DisplayRate, TickResolution).RoundToFrame();
		ULevelSequencePlayer* Player = SequenceActor->GetSequencePlayer();
		bool bPlayerAtRequestedFrame = false;
		FFrameNumber PlayerTickFrame(0);
		if (Player)
		{
			const FQualifiedFrameTime Current = Player->GetCurrentTime();
			PlayerTickFrame = FFrameRate::TransformTime(Current.Time, Current.Rate, TickResolution).RoundToFrame();
			bPlayerAtRequestedFrame = PlayerTickFrame == RequestedTickFrame;
		}
		TSharedRef<FJsonObject> SequenceRow = MakeShared<FJsonObject>();
		SequenceRow->SetStringField(TEXT("sequence_actor"), SequenceActor->GetPathName());
		SequenceRow->SetStringField(TEXT("sequence"), Sequence->GetPathName());
		SequenceRow->SetNumberField(TEXT("requested_display_frame"), DisplayFrame);
		SequenceRow->SetNumberField(TEXT("requested_tick_frame"), RequestedTickFrame.Value);
		SequenceRow->SetNumberField(TEXT("display_rate_numerator"), DisplayRate.Numerator);
		SequenceRow->SetNumberField(TEXT("display_rate_denominator"), DisplayRate.Denominator);
		SequenceRow->SetNumberField(TEXT("tick_resolution_numerator"), TickResolution.Numerator);
		SequenceRow->SetNumberField(TEXT("tick_resolution_denominator"), TickResolution.Denominator);
		SequenceRow->SetBoolField(TEXT("player_available"), Player != nullptr);
		SequenceRow->SetBoolField(TEXT("player_playing"), Player && Player->IsPlaying());
		SequenceRow->SetBoolField(TEXT("player_paused"), Player && Player->IsPaused());
		SequenceRow->SetNumberField(TEXT("player_tick_frame"), Player ? PlayerTickFrame.Value : 0);
		SequenceRow->SetBoolField(TEXT("player_at_requested_frame"), bPlayerAtRequestedFrame);
		TArray<TSharedPtr<FJsonValue>> BindingRows;
		for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
		{
			TArray<UObject*> BoundObjects;
			if (Player)
			{
				for (TWeakObjectPtr<> WeakObject : Player->FindBoundObjects(Binding.GetObjectGuid(), MovieSceneSequenceID::Root))
				{
					if (UObject* Object = WeakObject.Get()) BoundObjects.AddUnique(Object);
				}
			}
			TArray<UObject*, TInlineAllocator<1>> LocatedObjects;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			Sequence->LocateBoundObjects(Binding.GetObjectGuid(), UE::UniversalObjectLocator::FResolveParams(Target.World), nullptr, LocatedObjects);
#else
			FLevelSequenceBindingReference::FResolveBindingParams ResolveBindingParams;
			Sequence->LocateBoundObjects(Binding.GetObjectGuid(), Target.World, ResolveBindingParams, LocatedObjects);
#endif
			for (UObject* Object : LocatedObjects) BoundObjects.AddUnique(Object);
			bool bTargetsComponent = false;
			TArray<TSharedPtr<FJsonValue>> BoundObjectRows;
			for (UObject* Object : BoundObjects)
			{
				bTargetsComponent |= SequencerObjectMatchesTarget(Object, Target);
				BoundObjectRows.Add(MakeShared<FJsonValueString>(Object ? Object->GetPathName() : TEXT("")));
			}
			if (!bTargetsComponent) continue;
			++MatchedBindings;
			TSharedRef<FJsonObject> BindingRow = MakeShared<FJsonObject>();
			BindingRow->SetStringField(TEXT("binding_guid"), Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphens));
			BindingRow->SetArrayField(TEXT("resolved_objects"), BoundObjectRows);
			BindingRow->SetStringField(TEXT("resolution_basis"), Player
				? TEXT("live sequence player binding cache plus LevelSequence object locator")
				: TEXT("LevelSequence object locator; no live player cache"));
			TArray<TSharedPtr<FJsonValue>> SectionRows;
			for (UMovieSceneTrack* RawTrack : Binding.GetTracks())
			{
				UMovieSceneSkeletalAnimationTrack* Track = Cast<UMovieSceneSkeletalAnimationTrack>(RawTrack);
				if (!Track) continue;
				for (UMovieSceneSection* RawSection : Track->GetAllSections())
				{
					UMovieSceneSkeletalAnimationSection* Section = Cast<UMovieSceneSkeletalAnimationSection>(RawSection);
					if (!Section) continue;
					const bool bTrackEnabled = !Track->IsEvalDisabled() && !Track->IsRowEvalDisabled(Section->GetRowIndex());
					const bool bCoversFrame = Section->GetRange().Contains(RequestedTickFrame);
					const bool bEvaluatesAtFrame = Section->IsActive() && bTrackEnabled && bCoversFrame;
					if (bEvaluatesAtFrame)
					{
						++OutActiveSections;
						OutAnyLiveAtRequestedFrame |= bPlayerAtRequestedFrame;
					}
					TSharedRef<FJsonObject> SectionRow = MakeShared<FJsonObject>();
					SectionRow->SetStringField(TEXT("track"), Track->GetPathName());
					SectionRow->SetStringField(TEXT("section"), Section->GetPathName());
					SectionRow->SetNumberField(TEXT("row_index"), Section->GetRowIndex());
					SectionRow->SetNumberField(TEXT("overlap_priority"), Section->GetOverlapPriority());
					SectionRow->SetBoolField(TEXT("section_active"), Section->IsActive());
					SectionRow->SetBoolField(TEXT("track_evaluation_enabled"), bTrackEnabled);
					SectionRow->SetBoolField(TEXT("covers_requested_frame"), bCoversFrame);
					SectionRow->SetBoolField(TEXT("evaluates_at_requested_frame"), bEvaluatesAtFrame);
					#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					UAnimSequenceBase* Animation = Section->GetAnimation();
					#else
					UAnimSequenceBase* Animation = Section->Params.Animation;
					#endif
					SectionRow->SetStringField(TEXT("animation"), Animation ? Animation->GetPathName() : TEXT(""));
					SectionRow->SetStringField(TEXT("slot_name"), Section->Params.SlotName.ToString());
					SectionRow->SetBoolField(TEXT("skip_anim_notifies"), Section->Params.bSkipAnimNotifiers);
					SectionRow->SetBoolField(TEXT("force_custom_mode"), Section->Params.bForceCustomMode);
					SectionRow->SetBoolField(TEXT("reverse"), Section->Params.bReverse != 0);
					SectionRow->SetNumberField(TEXT("mapped_animation_time_seconds"), Section->MapTimeToAnimation(FFrameTime(RequestedTickFrame), TickResolution));
					SectionRows.Add(MakeShared<FJsonValueObject>(SectionRow));
				}
			}
			BindingRow->SetArrayField(TEXT("skeletal_animation_sections"), SectionRows);
			BindingRows.Add(MakeShared<FJsonValueObject>(BindingRow));
		}
		SequenceRow->SetArrayField(TEXT("target_bindings"), BindingRows);
		SequenceRows.Add(MakeShared<FJsonValueObject>(SequenceRow));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("scope"), TEXT("root LevelSequence actors in the explicitly selected world"));
	Result->SetNumberField(TEXT("sequence_actors_examined"), SequenceActorsExamined);
	Result->SetNumberField(TEXT("matched_bindings"), MatchedBindings);
	Result->SetNumberField(TEXT("active_skeletal_animation_sections"), OutActiveSections);
	Result->SetBoolField(TEXT("live_player_at_requested_frame"), OutAnyLiveAtRequestedFrame);
	Result->SetArrayField(TEXT("sequences"), SequenceRows);
	return Result;
}

static bool ExecuteEffectiveInspect(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error)) return false;
	TSharedRef<FJsonObject> State = MakeAnimationSnapshot(Target);
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	TArray<TSharedPtr<FJsonValue>> Contributors;
	auto AddContributor = [&Contributors](const FString& Kind, const FString& Identity, int32 Priority, bool bActive)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("kind"), Kind);
		Row->SetStringField(TEXT("identity"), Identity);
		Row->SetNumberField(TEXT("priority"), Priority);
		Row->SetBoolField(TEXT("active"), bActive);
		Contributors.Add(MakeShared<FJsonValueObject>(Row));
	};
	AddContributor(TEXT("component_animation_mode"), AnimationModeName(Target.Component->GetAnimationMode()), 100, true);
	if (Target.Component->AnimClass) AddContributor(TEXT("animation_blueprint"), Target.Component->AnimClass->GetPathName(), 200, AnimInstance != nullptr);
	if (UAnimationAsset* Asset = GetEffectiveSingleNodeAsset(Target.Component)) AddContributor(TEXT("single_node_asset"), Asset->GetPathName(), 200,
		Target.Component->GetAnimationMode() == EAnimationMode::AnimationSingleNode);
	if (UAnimMontage* Montage = AnimInstance ? AnimInstance->GetCurrentActiveMontage() : nullptr) AddContributor(TEXT("active_montage"), Montage->GetPathName(), 300, true);
	if (UAnimInstance* PostProcessInstance = Target.Component->GetPostProcessInstance()) AddContributor(TEXT("post_process_anim_blueprint"), PostProcessInstance->GetClass()->GetPathName(), 400,
		!Target.Component->GetDisablePostProcessBlueprint());
	int32 SequencerActiveSections = 0;
	bool bSequencerLiveAtFrame = false;
	if (Target.bSequencerContext)
	{
		if (!Arguments->HasField(TEXT("sequencer_frame"))) { Error = TEXT("sequencer_frame is required for effective inspection in sequencer context"); return false; }
		TSharedRef<FJsonObject> Provenance = InspectSequencerProvenance(Target, Arguments, SequencerActiveSections, bSequencerLiveAtFrame);
		State->SetObjectField(TEXT("sequencer_provenance"), Provenance);
		AddContributor(TEXT("sequencer_skeletal_animation_sections"), FString::Printf(TEXT("frame=%lld sections=%d"),
			static_cast<int64>(Arguments->GetNumberField(TEXT("sequencer_frame"))), SequencerActiveSections), 500, SequencerActiveSections > 0);
	}
	State->SetArrayField(TEXT("contributors"), Contributors);
	State->SetStringField(TEXT("effective_driver"), Target.bSequencerContext && SequencerActiveSections > 0 ? TEXT("sequencer_skeletal_animation")
		: (AnimInstance && AnimInstance->GetCurrentActiveMontage() ? TEXT("active_montage")
			: (Target.Component->GetAnimationMode() == EAnimationMode::AnimationBlueprint ? TEXT("animation_blueprint")
				: (Target.Component->GetAnimationMode() == EAnimationMode::AnimationSingleNode ? TEXT("single_node") : TEXT("custom")))));
	State->SetBoolField(TEXT("sequencer_live_player_evaluated_requested_frame"), bSequencerLiveAtFrame);
	State->SetStringField(TEXT("resolution_basis"), TEXT("explicit world/frame plus resolved LevelSequence actor, binding, skeletal animation track/section, live player position, component, AnimInstance, Montage, and post-process state"));
	Out->SetObjectField(TEXT("state"), State);
	Out->SetObjectField(TEXT("montage"), MakeMontageState(AnimInstance));
	Out->SetBoolField(TEXT("side_effect_free"), true);
	Summary = FString::Printf(TEXT("Resolved effective animation driver for %s"), *Target.Component->GetPathName());
	return true;
}

static TSharedRef<FJsonObject> MontageControlSchema()
{
	TMap<FString, TSharedRef<FJsonObject>> P = CommonSelectorProperties(true);
	P.Add(TEXT("operation"), SB::String(TEXT("Montage operation."), {TEXT("play"), TEXT("stop"), TEXT("pause"), TEXT("resume"), TEXT("jump_to_section"), TEXT("jump_to_section_end"), TEXT("set_next_section"), TEXT("set_rate"), TEXT("set_position"), TEXT("set_loop")}));
	P.Add(TEXT("montage_asset_path"), SB::String(TEXT("Montage path; omitted means current active Montage.")));
	P.Add(TEXT("section"), SB::String(TEXT("Section name.")));
	P.Add(TEXT("next_section"), SB::String(TEXT("Next section name; empty clears chaining.")));
	P.Add(TEXT("play_rate"), SB::Number(TEXT("Non-zero play rate.")));
	P.Add(TEXT("position_seconds"), SB::Number(TEXT("Montage position."), 0.0));
	P.Add(TEXT("blend_in_seconds"), SB::Number(TEXT("Explicit blend-in override."), 0.0));
	P.Add(TEXT("blend_out_seconds"), SB::Number(TEXT("Stop blend-out."), 0.0));
	P.Add(TEXT("stop_other_montages"), SB::Boolean(TEXT("Stop other Montage groups on play.")));
	P.Add(TEXT("loop"), SB::Boolean(TEXT("Loop selected section.")));
	return SB::Object(P, {TEXT("world_context"), TEXT("actor_id"), TEXT("operation")}, TEXT("Native Montage controller."), false);
}

// Mutation executors are defined in the independent implementation block below.
static bool ExecuteAnimationAssign(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
	TSharedRef<FJsonObject>&, FString&, FString&);
static bool ExecuteActionControl(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
	TSharedRef<FJsonObject>&, FString&, FString&);
static bool ExecuteMontageControl(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&,
	TSharedRef<FJsonObject>&, FString&, FString&);

} // namespace CharacterActionRuntime

void RegisterCharacterActionRuntimeTools(FSololmcpToolRegistry& Registry)
{
	using namespace CharacterActionRuntime;
	TMap<FString, TSharedRef<FJsonObject>> Assign = CommonSelectorProperties(true);
	Assign.Add(TEXT("mode"), SB::String(TEXT("Animation driver mode."), {TEXT("single_node"), TEXT("animation_blueprint"), TEXT("custom")}));
	Assign.Add(TEXT("animation_asset_path"), SB::String(TEXT("UAnimationAsset path.")));
	Assign.Add(TEXT("anim_class_path"), SB::String(TEXT("UAnimBlueprint or generated UAnimInstance class path.")));
	Assign.Add(TEXT("post_process_anim_class_path"), SB::String(TEXT("Post-process AnimBP/class path.")));
	Assign.Add(TEXT("clear_animation_asset"), SB::Boolean());
	Assign.Add(TEXT("clear_anim_class"), SB::Boolean());
	Assign.Add(TEXT("clear_post_process_anim_class"), SB::Boolean());
	Assign.Add(TEXT("disable_post_process"), SB::Boolean());
	Assign.Add(TEXT("loop"), SB::Boolean());
	Assign.Add(TEXT("playing"), SB::Boolean());
	Assign.Add(TEXT("position_seconds"), SB::Number(TEXT("Initial position."), 0.0));
	Assign.Add(TEXT("play_rate"), SB::Number(TEXT("Non-zero initial rate.")));
	Registry.Register({TEXT("character_animation_assign"), TEXT("Assign or clear SingleNode, AnimBP, asset, and post-process classes with explicit world selection, transaction, and before/after readback."),
		SB::Object(Assign, {TEXT("world_context"), TEXT("actor_id"), TEXT("mode")}, FString(), false), ExecuteAnimationAssign});

	TMap<FString, TSharedRef<FJsonObject>> Action = CommonSelectorProperties(true);
	Action.Add(TEXT("channel"), SB::String(TEXT("Playback channel."), {TEXT("auto"), TEXT("single_node"), TEXT("montage")}));
	Action.Add(TEXT("operation"), SB::String(TEXT("SingleNode: play/stop/pause/resume/seek/rate/loop; Montage operations match character_montage_control.")));
	Action.Add(TEXT("montage_asset_path"), SB::String()); Action.Add(TEXT("section"), SB::String()); Action.Add(TEXT("next_section"), SB::String());
	Action.Add(TEXT("position_seconds"), SB::Number(TEXT("Position."), 0.0)); Action.Add(TEXT("play_rate"), SB::Number());
	Action.Add(TEXT("loop"), SB::Boolean()); Action.Add(TEXT("fire_notifies"), SB::Boolean());
	Action.Add(TEXT("blend_in_seconds"), SB::Number(TEXT("Blend in."), 0.0)); Action.Add(TEXT("blend_out_seconds"), SB::Number(TEXT("Blend out."), 0.0));
	Action.Add(TEXT("stop_other_montages"), SB::Boolean());
	Registry.Register({TEXT("character_action_control"), TEXT("Control SingleNode or Montage play, stop, pause, resume, seek, rate, loop, and sections with live readback."),
		SB::Object(Action, {TEXT("world_context"), TEXT("actor_id"), TEXT("operation")}, FString(), false), ExecuteActionControl});
	Registry.Register({TEXT("character_montage_control"), TEXT("Control Montage play/blend/section/rate/position/loop and return the current FAnimMontageInstance state."), MontageControlSchema(), ExecuteMontageControl});
	Registry.Register({TEXT("character_animation_inspect"), TEXT("Read authored and live character animation mode, classes, assets, playback, world, driver, and network role."), CommonSelectorSchema(false), ExecuteAnimationInspect, nullptr, 1});

	TMap<FString, TSharedRef<FJsonObject>> MontageInspect = CommonSelectorProperties(false);
	MontageInspect.Add(TEXT("montage_asset_path"), SB::String(TEXT("Optional Montage path; defaults to current active Montage.")));
	Registry.Register({TEXT("character_action_state_inspect"), TEXT("Read active Montage instance identity, section, position, weight, blend, rate, stop state, and root-motion state."),
		SB::Object(MontageInspect, {TEXT("world_context"), TEXT("actor_id")}, FString(), false), ExecuteActionStateInspect, nullptr, 1});

	TMap<FString, TSharedRef<FJsonObject>> SM = CommonSelectorProperties(false);
	SM.Add(TEXT("machine_name"), SB::String()); SM.Add(TEXT("max_machines"), SB::Integer(TEXT("Bounded result limit."), 1, 256));
	Registry.Register({TEXT("anim_instance_state_machine_inspect"), TEXT("Read live AnimBP state machines, current state, elapsed time, weights, and active transitions."),
		SB::Object(SM, {TEXT("world_context"), TEXT("actor_id")}, FString(), false), ExecuteStateMachineInspect, nullptr, 1});
	TMap<FString, TSharedRef<FJsonObject>> Sync = CommonSelectorProperties(false); Sync.Add(TEXT("sync_group_name"), SB::String());
	Registry.Register({TEXT("anim_instance_sync_group_inspect"), TEXT("Read all compiled Sync Groups and current marker positions from the live AnimInstance."),
		SB::Object(Sync, {TEXT("world_context"), TEXT("actor_id")}, FString(), false), ExecuteSyncGroupInspect, nullptr, 1});
	TMap<FString, TSharedRef<FJsonObject>> Curves = CommonSelectorProperties(false);
	Curves.Add(TEXT("curve_type"), SB::String(TEXT("Curve category."), {TEXT("attribute"), TEXT("morph_target"), TEXT("material")}));
	Curves.Add(TEXT("max_curves"), SB::Integer(TEXT("Bounded result limit."), 1, 4096));
	Registry.Register({TEXT("anim_instance_curve_inspect"), TEXT("Read typed active attribute, morph-target, or material curves and live values."),
		SB::Object(Curves, {TEXT("world_context"), TEXT("actor_id")}, FString(), false), ExecuteCurveInspect, nullptr, 1});
	TMap<FString, TSharedRef<FJsonObject>> Notifies = CommonSelectorProperties(false); Notifies.Add(TEXT("max_notifies"), SB::Integer(TEXT("Bounded result limit."), 1, 4096));
	Registry.Register({TEXT("anim_instance_notify_inspect"), TEXT("Read current notify queue counts and typed active NotifyState events, including Motion Warping windows."),
		SB::Object(Notifies, {TEXT("world_context"), TEXT("actor_id")}, FString(), false), ExecuteNotifyInspect, nullptr, 1});
	Registry.Register({TEXT("character_root_motion_inspect"), TEXT("Read RootMotion mode, active root-motion Montage, movement consumer, sources, and network authority without consuming pending delta."),
		CommonSelectorSchema(false), ExecuteRootMotionInspect, nullptr, 1});
	TMap<FString, TSharedRef<FJsonObject>> RootSet = CommonSelectorProperties(true);
	RootSet.Add(TEXT("root_motion_mode"), SB::String(TEXT("Root motion extraction mode."), {TEXT("no_root_motion_extraction"), TEXT("ignore_root_motion"), TEXT("root_motion_from_everything"), TEXT("root_motion_from_montages_only")}));
	Registry.Register({TEXT("character_root_motion_set"), TEXT("Set RootMotion extraction mode with authority check and direct readback."),
		SB::Object(RootSet, {TEXT("world_context"), TEXT("actor_id"), TEXT("root_motion_mode")}, FString(), false), ExecuteRootMotionSet});

	TSharedRef<FJsonObject> Vec = SB::Object({{TEXT("x"), SB::Number()}, {TEXT("y"), SB::Number()}, {TEXT("z"), SB::Number()}}, {TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false);
	TSharedRef<FJsonObject> Rot = SB::Object({{TEXT("pitch"), SB::Number()}, {TEXT("yaw"), SB::Number()}, {TEXT("roll"), SB::Number()}}, {TEXT("pitch"), TEXT("yaw"), TEXT("roll")}, FString(), false);
	TMap<FString, TSharedRef<FJsonObject>> WarpSet = CommonSelectorProperties(true);
	WarpSet.Add(TEXT("operation"), SB::String(TEXT("Target mutation."), {TEXT("upsert"), TEXT("remove")}));
	WarpSet.Add(TEXT("target_name"), SB::String(TEXT("Warp target name."), {}, 1));
	WarpSet.Add(TEXT("transform"), SB::Object({{TEXT("location"), Vec}, {TEXT("rotation"), Rot}}, {TEXT("location"), TEXT("rotation")}, FString(), false));
	Registry.Register({TEXT("character_motion_warping_target_set"), TEXT("Upsert or remove a real MotionWarpingComponent target through reflected native UFunctions with before/after state."),
		SB::Object(WarpSet, {TEXT("world_context"), TEXT("actor_id"), TEXT("operation"), TEXT("target_name")}, FString(), false), ExecuteMotionWarpingTargetSet});
	Registry.Register({TEXT("character_motion_warping_inspect"), TEXT("Read real MotionWarpingComponent WarpTargets plus active Motion Warping notify windows."),
		CommonSelectorSchema(false), ExecuteMotionWarpingInspect, nullptr, 1});
	TMap<FString, TSharedRef<FJsonObject>> Effective = CommonSelectorProperties(false);
	Effective.Add(TEXT("sequencer_frame"), SB::Integer(TEXT("Required display-rate frame for sequencer context.")));
	Effective.Add(TEXT("sequence_actor_id"), SB::String(TEXT("Optional exact LevelSequenceActor path, name, or editor label.")));
	Effective.Add(TEXT("sequence_path"), SB::String(TEXT("Optional exact LevelSequence asset path.")));
	Registry.Register({TEXT("character_animation_effective_inspect"), TEXT("Resolve effective component/AnimBP/SingleNode/Montage/post-process contributors plus actual LevelSequence binding, skeletal track, active section, mapped animation time, and live-player frame provenance."),
		SB::Object(Effective, {TEXT("world_context"), TEXT("actor_id")}, FString(), false), ExecuteEffectiveInspect, nullptr, 1});
}

} // namespace UE::SOMOLMCP

namespace UE::SOMOLMCP
{
namespace CharacterActionRuntime
{

static bool ExecuteAnimationAssign(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error) || !RequireMutationAuthority(Arguments, Target, Error)) return false;
	FString Mode;
	if (!Arguments->TryGetStringField(TEXT("mode"), Mode))
	{
		Error = TEXT("mode is required");
		return false;
	}
	Mode = Mode.TrimStartAndEnd().ToLower();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 5
	FString LegacyPostProcessClassPath;
	Arguments->TryGetStringField(TEXT("post_process_anim_class_path"), LegacyPostProcessClassPath);
	const bool bLegacyClearPostProcess = Arguments->HasField(TEXT("clear_post_process_anim_class"))
		&& Arguments->GetBoolField(TEXT("clear_post_process_anim_class"));
	if (!LegacyPostProcessClassPath.IsEmpty() || bLegacyClearPostProcess)
	{
		Error = TEXT("post_process_anim_class mutation is unavailable before UE 5.5; request rejected before mutation");
		return false;
	}
#endif
	const double StartedAt = FPlatformTime::Seconds();
	const TSharedRef<FJsonObject> Before = MakeAnimationSnapshot(Target);
	TUniquePtr<FScopedTransaction> Transaction = BeginEditorTransaction(
		Target, NSLOCTEXT("SOMOLMCP", "CharacterAnimationAssign", "SOMOLMCP Character Animation Assign"));

	FString AssetPath;
	Arguments->TryGetStringField(TEXT("animation_asset_path"), AssetPath);
	FString AnimClassPath;
	Arguments->TryGetStringField(TEXT("anim_class_path"), AnimClassPath);
	FString PostProcessClassPath;
	Arguments->TryGetStringField(TEXT("post_process_anim_class_path"), PostProcessClassPath);
	const bool bClearAsset = Arguments->HasField(TEXT("clear_animation_asset")) && Arguments->GetBoolField(TEXT("clear_animation_asset"));
	const bool bClearAnimClass = Arguments->HasField(TEXT("clear_anim_class")) && Arguments->GetBoolField(TEXT("clear_anim_class"));
	const bool bClearPostProcess = Arguments->HasField(TEXT("clear_post_process_anim_class")) && Arguments->GetBoolField(TEXT("clear_post_process_anim_class"));
	const bool bLoop = !Arguments->HasField(TEXT("loop")) || Arguments->GetBoolField(TEXT("loop"));
	const bool bPlaying = !Arguments->HasField(TEXT("playing")) || Arguments->GetBoolField(TEXT("playing"));
	const float Position = Arguments->HasField(TEXT("position_seconds"))
		? static_cast<float>(Arguments->GetNumberField(TEXT("position_seconds"))) : 0.0f;
	const float PlayRate = Arguments->HasField(TEXT("play_rate"))
		? static_cast<float>(Arguments->GetNumberField(TEXT("play_rate"))) : 1.0f;
	if (!FMath::IsFinite(Position) || Position < 0.0f || !FMath::IsFinite(PlayRate) || FMath::IsNearlyZero(PlayRate))
	{
		Error = TEXT("position_seconds must be finite and >= 0; play_rate must be finite and non-zero");
		return false;
	}

	UAnimationAsset* RequestedAsset = nullptr;
	UClass* RequestedAnimClass = nullptr;
	UClass* RequestedPostProcessClass = nullptr;
	if (!AssetPath.IsEmpty())
	{
		RequestedAsset = ResolveAnimationAsset(AssetPath, Error);
		if (!RequestedAsset) return false;
		if (Target.Component->GetSkeletalMeshAsset()
			&& Target.Component->GetSkeletalMeshAsset()->GetSkeleton()
			&& RequestedAsset->GetSkeleton()
			&& Target.Component->GetSkeletalMeshAsset()->GetSkeleton() != RequestedAsset->GetSkeleton())
		{
			Error = FString::Printf(TEXT("skeleton_mismatch: component mesh skeleton %s, animation skeleton %s"),
				*Target.Component->GetSkeletalMeshAsset()->GetSkeleton()->GetPathName(),
				*RequestedAsset->GetSkeleton()->GetPathName());
			return false;
		}
	}
	if (!AnimClassPath.IsEmpty())
	{
		RequestedAnimClass = ResolveAnimInstanceClass(AnimClassPath, Error);
		if (!RequestedAnimClass) return false;
	}
	if (!PostProcessClassPath.IsEmpty())
	{
		RequestedPostProcessClass = ResolveAnimInstanceClass(PostProcessClassPath, Error);
		if (!RequestedPostProcessClass) return false;
	}

	if (Mode == TEXT("single_node"))
	{
		if (!RequestedAsset && !bClearAsset)
		{
			Error = TEXT("single_node assignment requires animation_asset_path or clear_animation_asset=true");
			return false;
		}
		Target.Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Target.Component->OverrideAnimationData(bClearAsset ? nullptr : RequestedAsset, bLoop, bPlaying, Position, PlayRate);
	}
	else if (Mode == TEXT("animation_blueprint"))
	{
		if (!RequestedAnimClass && !bClearAnimClass)
		{
			Error = TEXT("animation_blueprint assignment requires anim_class_path or clear_anim_class=true");
			return false;
		}
		Target.Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		Target.Component->SetAnimInstanceClass(bClearAnimClass ? nullptr : RequestedAnimClass);
	}
	else if (Mode == TEXT("custom"))
	{
		Target.Component->SetAnimationMode(EAnimationMode::AnimationCustomMode);
	}
	else
	{
		Error = TEXT("mode must be single_node, animation_blueprint, or custom");
		return false;
	}

	if (RequestedPostProcessClass || bClearPostProcess)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		Target.Component->SetOverridePostProcessAnimBP(bClearPostProcess ? nullptr : RequestedPostProcessClass, true);
#else
		Error = TEXT("post_process_anim_class mutation is unavailable before UE 5.5");
		return false;
#endif
	}
	if (Arguments->HasField(TEXT("disable_post_process")))
	{
		Target.Component->SetDisablePostProcessBlueprint(Arguments->GetBoolField(TEXT("disable_post_process")));
	}
	Target.Component->MarkRenderDynamicDataDirty();
	Target.Component->MarkPackageDirty();
	Target.Actor->MarkPackageDirty();

	const TSharedRef<FJsonObject> After = MakeAnimationSnapshot(Target);
	const FString ExpectedMode = Mode;
	FString ReadMode;
	After->TryGetStringField(TEXT("animation_mode"), ReadMode);
	if (ReadMode != ExpectedMode)
	{
		Error = FString::Printf(TEXT("readback_mismatch: expected mode %s, got %s"), *ExpectedMode, *ReadMode);
		return false;
	}
	if (Mode == TEXT("single_node"))
	{
		FString ReadAsset;
		After->TryGetStringField(TEXT("authored_single_node_asset"), ReadAsset);
		const FString ExpectedAsset = bClearAsset ? TEXT("") : RequestedAsset->GetPathName();
		if (ReadAsset != ExpectedAsset)
		{
			Error = FString::Printf(TEXT("readback_mismatch: expected asset %s, got %s"), *ExpectedAsset, *ReadAsset);
			return false;
		}
	}
	if (Mode == TEXT("animation_blueprint"))
	{
		FString ReadClass;
		After->TryGetStringField(TEXT("anim_class"), ReadClass);
		const FString ExpectedClass = bClearAnimClass ? TEXT("") : RequestedAnimClass->GetPathName();
		if (ReadClass != ExpectedClass)
		{
			Error = FString::Printf(TEXT("readback_mismatch: expected AnimClass %s, got %s"), *ExpectedClass, *ReadClass);
			return false;
		}
	}
	Out->SetObjectField(TEXT("receipt"), MakeReceipt(Target, TEXT("character_animation_assign"), Before, After,
		Transaction.IsValid(), StartedAt));
	Out->SetObjectField(TEXT("state"), After);
	Summary = FString::Printf(TEXT("Assigned %s animation driver on %s"), *Mode, *Target.Component->GetPathName());
	return true;
}

static bool ApplyMontageOperation(
	const TSharedRef<FJsonObject>& Arguments,
	const FResolvedTarget& Target,
	FString& Error,
	UAnimMontage*& OutMontage)
{
	UAnimInstance* AnimInstance = Target.Component->GetAnimInstance();
	if (!AnimInstance)
	{
		Error = TEXT("selected component has no UAnimInstance; Montage control requires AnimationBlueprint or initialized SingleNode mode");
		return false;
	}
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation))
	{
		Error = TEXT("operation is required");
		return false;
	}
	Operation = Operation.TrimStartAndEnd().ToLower();
	OutMontage = ResolveRequestedOrActiveMontage(Arguments, AnimInstance, Error);
	if (!OutMontage) return false;
	const float PlayRate = Arguments->HasField(TEXT("play_rate"))
		? static_cast<float>(Arguments->GetNumberField(TEXT("play_rate"))) : 1.0f;
	const float Position = Arguments->HasField(TEXT("position_seconds"))
		? static_cast<float>(Arguments->GetNumberField(TEXT("position_seconds"))) : 0.0f;
	const float BlendIn = Arguments->HasField(TEXT("blend_in_seconds"))
		? static_cast<float>(Arguments->GetNumberField(TEXT("blend_in_seconds"))) : -1.0f;
	const float BlendOut = Arguments->HasField(TEXT("blend_out_seconds"))
		? static_cast<float>(Arguments->GetNumberField(TEXT("blend_out_seconds"))) : 0.25f;
	if (!FMath::IsFinite(PlayRate) || FMath::IsNearlyZero(PlayRate)
		|| !FMath::IsFinite(Position) || Position < 0.0f
		|| !FMath::IsFinite(BlendIn) || !FMath::IsFinite(BlendOut) || BlendOut < 0.0f)
	{
		Error = TEXT("invalid montage numeric argument");
		return false;
	}
	if (Operation == TEXT("play"))
	{
		const bool bStopOthers = !Arguments->HasField(TEXT("stop_other_montages"))
			|| Arguments->GetBoolField(TEXT("stop_other_montages"));
		const float Result = BlendIn >= 0.0f
			? AnimInstance->Montage_PlayWithBlendIn(OutMontage, FAlphaBlendArgs(BlendIn), PlayRate,
				EMontagePlayReturnType::Duration, Position, bStopOthers)
			: AnimInstance->Montage_Play(OutMontage, PlayRate, EMontagePlayReturnType::Duration, Position, bStopOthers);
		if (Result <= 0.0f)
		{
			Error = TEXT("Montage_Play returned zero; verify skeleton compatibility and slot availability");
			return false;
		}
		FString StartSection;
		if (Arguments->TryGetStringField(TEXT("section"), StartSection) && !StartSection.IsEmpty())
		{
			if (OutMontage->GetSectionIndex(FName(*StartSection)) == INDEX_NONE)
			{
				Error = FString::Printf(TEXT("montage_section_not_found: %s"), *StartSection);
				AnimInstance->Montage_Stop(0.0f, OutMontage);
				return false;
			}
			AnimInstance->Montage_JumpToSection(FName(*StartSection), OutMontage);
		}
	}
	else if (Operation == TEXT("stop"))
	{
		AnimInstance->Montage_Stop(BlendOut, OutMontage);
	}
	else if (Operation == TEXT("pause")) AnimInstance->Montage_Pause(OutMontage);
	else if (Operation == TEXT("resume")) AnimInstance->Montage_Resume(OutMontage);
	else if (Operation == TEXT("jump_to_section") || Operation == TEXT("jump_to_section_end"))
	{
		FString Section;
		if (!Arguments->TryGetStringField(TEXT("section"), Section) || OutMontage->GetSectionIndex(FName(*Section)) == INDEX_NONE)
		{
			Error = TEXT("a valid section is required");
			return false;
		}
		if (Operation == TEXT("jump_to_section_end")) AnimInstance->Montage_JumpToSectionsEnd(FName(*Section), OutMontage);
		else AnimInstance->Montage_JumpToSection(FName(*Section), OutMontage);
	}
	else if (Operation == TEXT("set_next_section"))
	{
		FString Section;
		FString NextSection;
		if (!Arguments->TryGetStringField(TEXT("section"), Section)
			|| !Arguments->TryGetStringField(TEXT("next_section"), NextSection)
			|| OutMontage->GetSectionIndex(FName(*Section)) == INDEX_NONE
			|| (!NextSection.IsEmpty() && OutMontage->GetSectionIndex(FName(*NextSection)) == INDEX_NONE))
		{
			Error = TEXT("valid section and next_section (or empty next_section) are required");
			return false;
		}
		AnimInstance->Montage_SetNextSection(FName(*Section), FName(*NextSection), OutMontage);
	}
	else if (Operation == TEXT("set_rate")) AnimInstance->Montage_SetPlayRate(OutMontage, PlayRate);
	else if (Operation == TEXT("set_position"))
	{
		if (Position > OutMontage->GetPlayLength())
		{
			Error = FString::Printf(TEXT("position_seconds %.6f exceeds montage length %.6f"), Position, OutMontage->GetPlayLength());
			return false;
		}
		AnimInstance->Montage_SetPosition(OutMontage, Position);
	}
	else if (Operation == TEXT("set_loop"))
	{
		const bool bLoop = Arguments->HasField(TEXT("loop")) && Arguments->GetBoolField(TEXT("loop"));
		FString Section;
		Arguments->TryGetStringField(TEXT("section"), Section);
		if (Section.IsEmpty())
		{
			Section = AnimInstance->Montage_GetCurrentSection(OutMontage).ToString();
		}
		if (OutMontage->GetSectionIndex(FName(*Section)) == INDEX_NONE)
		{
			Error = TEXT("set_loop requires a valid section or an active current section");
			return false;
		}
		AnimInstance->Montage_SetNextSection(FName(*Section), bLoop ? FName(*Section) : NAME_None, OutMontage);
	}
	else
	{
		Error = TEXT("unsupported montage operation");
		return false;
	}
	return true;
}

static bool ExecuteMontageControl(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error) || !RequireMutationAuthority(Arguments, Target, Error)) return false;
	const double StartedAt = FPlatformTime::Seconds();
	TSharedRef<FJsonObject> Before = MakeMontageState(Target.Component->GetAnimInstance());
	TUniquePtr<FScopedTransaction> Transaction = BeginEditorTransaction(
		Target, NSLOCTEXT("SOMOLMCP", "CharacterMontageControl", "SOMOLMCP Character Montage Control"));
	UAnimMontage* Montage = nullptr;
	if (!ApplyMontageOperation(Arguments, Target, Error, Montage)) return false;
	TSharedRef<FJsonObject> After = MakeMontageState(Target.Component->GetAnimInstance(), Montage);
	Out->SetObjectField(TEXT("receipt"), MakeReceipt(Target, TEXT("character_montage_control"), Before, After,
		Transaction.IsValid(), StartedAt));
	Out->SetObjectField(TEXT("state"), After);
	Summary = FString::Printf(TEXT("Applied Montage operation on %s"), *Target.Component->GetPathName());
	return true;
}

static bool ExecuteActionControl(
	const FSololmcpToolExecutionContext&,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out,
	FString& Summary,
	FString& Error)
{
	FResolvedTarget Target;
	if (!ResolveTarget(Arguments, Target, Error) || !RequireMutationAuthority(Arguments, Target, Error)) return false;
	FString Channel = TEXT("auto");
	Arguments->TryGetStringField(TEXT("channel"), Channel);
	Channel = Channel.TrimStartAndEnd().ToLower();
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation))
	{
		Error = TEXT("operation is required");
		return false;
	}
	Operation = Operation.TrimStartAndEnd().ToLower();
	if (Channel == TEXT("auto"))
	{
		Channel = Arguments->HasField(TEXT("montage_asset_path"))
			|| (Target.Component->GetAnimationMode() != EAnimationMode::AnimationSingleNode
				&& Target.Component->GetAnimInstance() && Target.Component->GetAnimInstance()->GetCurrentActiveMontage())
			? TEXT("montage") : TEXT("single_node");
	}
	const double StartedAt = FPlatformTime::Seconds();
	TSharedRef<FJsonObject> Before = MakeAnimationSnapshot(Target);
	Before->SetObjectField(TEXT("montage"), MakeMontageState(Target.Component->GetAnimInstance()));
	TUniquePtr<FScopedTransaction> Transaction = BeginEditorTransaction(
		Target, NSLOCTEXT("SOMOLMCP", "CharacterActionControl", "SOMOLMCP Character Action Control"));

	UAnimMontage* ControlledMontage = nullptr;
	if (Channel == TEXT("montage"))
	{
		if (!ApplyMontageOperation(Arguments, Target, Error, ControlledMontage)) return false;
	}
	else if (Channel == TEXT("single_node"))
	{
		if (Target.Component->GetAnimationMode() != EAnimationMode::AnimationSingleNode)
		{
			Error = TEXT("single_node control requires the component to be in AnimationSingleNode mode; assign it explicitly first");
			return false;
		}
		UAnimSingleNodeInstance* Single = Target.Component->GetSingleNodeInstance();
		if (!Single)
		{
			Error = TEXT("single_node_instance_unavailable");
			return false;
		}
		if (Operation == TEXT("play") || Operation == TEXT("resume")) Single->SetPlaying(true);
		else if (Operation == TEXT("stop")) Single->StopAnim();
		else if (Operation == TEXT("pause")) Single->SetPlaying(false);
		else if (Operation == TEXT("seek"))
		{
			if (!Arguments->HasField(TEXT("position_seconds"))) { Error = TEXT("seek requires position_seconds"); return false; }
			const float Position = static_cast<float>(Arguments->GetNumberField(TEXT("position_seconds")));
			if (!FMath::IsFinite(Position) || Position < 0.0f || Position > Single->GetLength())
			{
				Error = TEXT("position_seconds is outside the animation range");
				return false;
			}
			const bool bFireNotifies = !Arguments->HasField(TEXT("fire_notifies")) || Arguments->GetBoolField(TEXT("fire_notifies"));
			Single->SetPosition(Position, bFireNotifies);
		}
		else if (Operation == TEXT("rate"))
		{
			if (!Arguments->HasField(TEXT("play_rate"))) { Error = TEXT("rate requires play_rate"); return false; }
			const float Rate = static_cast<float>(Arguments->GetNumberField(TEXT("play_rate")));
			if (!FMath::IsFinite(Rate) || FMath::IsNearlyZero(Rate)) { Error = TEXT("play_rate must be finite and non-zero"); return false; }
			Single->SetPlayRate(Rate);
		}
		else if (Operation == TEXT("loop"))
		{
			if (!Arguments->HasField(TEXT("loop"))) { Error = TEXT("loop operation requires loop boolean"); return false; }
			Single->SetLooping(Arguments->GetBoolField(TEXT("loop")));
		}
		else { Error = TEXT("unsupported single_node operation"); return false; }
	}
	else
	{
		Error = TEXT("channel must be auto, single_node, or montage");
		return false;
	}

	TSharedRef<FJsonObject> After = MakeAnimationSnapshot(Target);
	After->SetObjectField(TEXT("montage"), MakeMontageState(Target.Component->GetAnimInstance(), ControlledMontage));
	Out->SetStringField(TEXT("resolved_channel"), Channel);
	Out->SetObjectField(TEXT("receipt"), MakeReceipt(Target, TEXT("character_action_control"), Before, After,
		Transaction.IsValid(), StartedAt));
	Out->SetObjectField(TEXT("state"), After);
	Summary = FString::Printf(TEXT("Applied %s on %s channel"), *Operation, *Channel);
	return true;
}

} // namespace CharacterActionRuntime
} // namespace UE::SOMOLMCP
