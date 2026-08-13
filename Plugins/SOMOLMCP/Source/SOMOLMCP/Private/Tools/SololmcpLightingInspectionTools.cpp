// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 lighting inspection and lossless typed patch/readback tools.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Dom/JsonObject.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/Light.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/RectLight.h"
#include "Engine/ReflectionCapture.h"
#include "Engine/RendererSettings.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProperties.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "LevelEditorViewport.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Misc/FrameRate.h"
#include "Misc/QualifiedFrameTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "RHIShaderPlatform.h"
#include "RHIStrings.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "UObject/FieldIterator.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace LightingInspection
{
static FString WorldTypeString(const EWorldType::Type Type)
{
	switch (Type)
	{
	case EWorldType::Editor: return TEXT("editor");
	case EWorldType::PIE: return TEXT("pie");
	case EWorldType::Game: return TEXT("game");
	case EWorldType::GamePreview: return TEXT("game_preview");
	case EWorldType::EditorPreview: return TEXT("editor_preview");
	case EWorldType::Inactive: return TEXT("inactive");
	default: return TEXT("other");
	}
}

static FString NetModeString(const ENetMode Mode)
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

static TSharedRef<FJsonObject> VectorJson(const FVector& Value)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("x"), Value.X);
	Json->SetNumberField(TEXT("y"), Value.Y);
	Json->SetNumberField(TEXT("z"), Value.Z);
	return Json;
}

static TSharedRef<FJsonObject> RotatorJson(const FRotator& Value)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("pitch"), Value.Pitch);
	Json->SetNumberField(TEXT("yaw"), Value.Yaw);
	Json->SetNumberField(TEXT("roll"), Value.Roll);
	return Json;
}

static TSharedRef<FJsonObject> TransformJson(const FTransform& Value)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("location_cm"), VectorJson(Value.GetLocation()));
	Json->SetObjectField(TEXT("rotation_deg"), RotatorJson(Value.Rotator()));
	Json->SetObjectField(TEXT("scale"), VectorJson(Value.GetScale3D()));
	return Json;
}

static TSharedRef<FJsonObject> WorldIdentity(UWorld* World, const FString& RequestedContext)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("requested_context"), RequestedContext);
	Json->SetBoolField(TEXT("resolved"), World != nullptr);
	if (!World)
	{
		Json->SetStringField(TEXT("null_reason"), TEXT("requested_world_context_not_available"));
		return Json;
	}
	Json->SetStringField(TEXT("path"), World->GetPathName());
	Json->SetStringField(TEXT("map_name"), World->GetMapName());
	Json->SetStringField(TEXT("world_type"), WorldTypeString(World->WorldType));
	Json->SetStringField(TEXT("net_mode"), NetModeString(World->GetNetMode()));
	Json->SetBoolField(TEXT("is_game_world"), World->IsGameWorld());
	Json->SetNumberField(TEXT("time_seconds"), World->GetTimeSeconds());
	return Json;
}

static UWorld* ResolveWorld(const TSharedRef<FJsonObject>& Arguments, FString& RequestedContext, FString& Error)
{
	RequestedContext = TEXT("editor");
	Arguments->TryGetStringField(TEXT("world_context"), RequestedContext);
	RequestedContext.ToLowerInline();
	if (!GEditor || !GEngine)
	{
		Error = TEXT("GEditor/GEngine is unavailable.");
		return nullptr;
	}

	if (RequestedContext == TEXT("editor"))
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) Error = TEXT("Editor world is unavailable.");
		return World;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (!World) continue;
		if (RequestedContext == TEXT("pie") && Context.WorldType == EWorldType::PIE) return World;
		if (RequestedContext == TEXT("standalone") &&
			(Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::GamePreview)) return World;
		if (RequestedContext == TEXT("preview") && Context.WorldType == EWorldType::EditorPreview) return World;
		if (RequestedContext == TEXT("any") && Context.WorldType != EWorldType::Inactive) return World;
	}
	Error = FString::Printf(TEXT("Requested world_context '%s' is not currently available."), *RequestedContext);
	return nullptr;
}

static AActor* FindActor(UWorld* World, const FString& ActorId, FString& Error)
{
	if (!World || ActorId.IsEmpty())
	{
		Error = TEXT("actor is required and the requested world must be available.");
		return nullptr;
	}
	AActor* LabelMatch = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		if (Actor->GetPathName().Equals(ActorId, ESearchCase::CaseSensitive)) return Actor;
		if (Actor->GetName().Equals(ActorId, ESearchCase::IgnoreCase)) return Actor;
#if WITH_EDITOR
		if (!LabelMatch && Actor->GetActorLabel().Equals(ActorId, ESearchCase::IgnoreCase)) LabelMatch = Actor;
#endif
	}
	if (LabelMatch) return LabelMatch;
	Error = FString::Printf(TEXT("Actor '%s' was not found in world '%s'."), *ActorId, *World->GetPathName());
	return nullptr;
}

static ULightComponentBase* ResolveLight(UWorld* World, const FString& ActorId, FString& Error)
{
	AActor* Actor = FindActor(World, ActorId, Error);
	if (!Actor) return nullptr;
	ULightComponentBase* Component = Actor->FindComponentByClass<ULightComponentBase>();
	if (!Component)
	{
		Error = FString::Printf(TEXT("Actor '%s' has no ULightComponentBase component."), *ActorId);
	}
	return Component;
}

static FString LightType(ULightComponentBase* Component)
{
	if (Cast<USkyLightComponent>(Component)) return TEXT("sky");
	if (Cast<UDirectionalLightComponent>(Component)) return TEXT("directional");
	if (Cast<USpotLightComponent>(Component)) return TEXT("spot");
	if (Cast<URectLightComponent>(Component)) return TEXT("rect");
	if (Cast<UPointLightComponent>(Component)) return TEXT("point");
	if (Cast<ULocalLightComponent>(Component)) return TEXT("local_other");
	if (Cast<ULightComponent>(Component)) return TEXT("light_other");
	return TEXT("light_base_other");
}

static TSharedPtr<FJsonValue> NormalizeJsonValue(const TSharedPtr<FJsonValue>& Value);

static TArray<TSharedPtr<FJsonValue>> NormalizeJsonArray(const TArray<TSharedPtr<FJsonValue>>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Normalized;
	Normalized.Reserve(Values.Num());
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		Normalized.Add(NormalizeJsonValue(Value));
	}
	return Normalized;
}

static TSharedRef<FJsonObject> NormalizeJsonObject(const TSharedPtr<FJsonObject>& Object)
{
	TSharedRef<FJsonObject> Normalized = MakeShared<FJsonObject>();
	if (!Object.IsValid())
	{
		return Normalized;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		Normalized->SetField(Pair.Key, NormalizeJsonValue(Pair.Value));
	}
	return Normalized;
}

static TSharedRef<FJsonObject> NormalizeJsonObject(const TSharedRef<FJsonObject>& Object)
{
	return NormalizeJsonObject(Object.ToSharedPtr());
}

// FJsonObjectConverter can legitimately produce sparse arrays for reflected
// properties and third-party serializers can insert a null shared pointer into
// an otherwise valid object. Unreal's JSON writer asserts on those pointers.
// Deep-clone the tree at the tool boundary and represent every missing node as
// an explicit JSON null so jobs/get is always serializable without losing a key
// or array position.
static TSharedPtr<FJsonValue> NormalizeJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
	}

	switch (Value->Type)
	{
	case EJson::String:
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Value->AsString()));
	case EJson::Number:
		if (Value->PreferStringRepresentation())
		{
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumberString>(Value->AsString()));
		}
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value->AsNumber()));
	case EJson::Boolean:
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Value->AsBool()));
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Value->TryGetArray(Array) || !Array)
		{
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
		}
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(NormalizeJsonArray(*Array)));
	}
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value->TryGetObject(Object) || !Object || !Object->IsValid())
		{
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
		}
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(NormalizeJsonObject(*Object)));
	}
	case EJson::Null:
	case EJson::None:
	default:
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
	}
}

static TSharedPtr<FJsonValue> TypedPropertyValue(UObject* Object, FProperty* Property)
{
	if (!Object || !Property) return MakeShared<FJsonValueNull>();
	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		const UObject* Value = ObjectProperty->GetObjectPropertyValue(ValuePtr);
		if (Value)
		{
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Value->GetPathName()));
		}
		return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
	}
	TSharedPtr<FJsonValue> Value = NormalizeJsonValue(FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr, 0, 0));
	if (Value.IsValid())
	{
		return Value;
	}
	return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
}

static TSharedRef<FJsonObject> PropertySnapshot(UObject* Object, FProperty* Property, bool& bUnsupported)
{
	bUnsupported = false;
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Property->GetName());
	Json->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
	Json->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
	Json->SetStringField(TEXT("declared_in"), Property->GetOwnerClass() ? Property->GetOwnerClass()->GetPathName() : FString());
	Json->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
	Json->SetStringField(TEXT("display_name"), Property->GetDisplayNameText().ToString());
	Json->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
	Json->SetBoolField(TEXT("edit_const"), Property->HasAnyPropertyFlags(CPF_EditConst));
	Json->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
	Json->SetStringField(TEXT("units"), Property->GetMetaData(TEXT("Units")));
	Json->SetStringField(TEXT("clamp_min"), Property->GetMetaData(TEXT("ClampMin")));
	Json->SetStringField(TEXT("clamp_max"), Property->GetMetaData(TEXT("ClampMax")));
	Json->SetStringField(TEXT("edit_condition"), Property->GetMetaData(TEXT("EditCondition")));

	FString Exported;
	Property->ExportText_InContainer(0, Exported, Object, Object, Object, PPF_None);
	Json->SetStringField(TEXT("export_text"), Exported);
	TSharedPtr<FJsonValue> Typed = TypedPropertyValue(Object, Property);
	if (!Typed.IsValid())
	{
		bUnsupported = true;
		Json->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
		Json->SetStringField(TEXT("null_reason"), TEXT("json_conversion_failed_export_text_preserved"));
	}
	else
	{
		Json->SetField(TEXT("value"), Typed);
		if (Typed->Type == EJson::Null && !Property->IsA<FObjectPropertyBase>())
		{
			bUnsupported = true;
			Json->SetStringField(TEXT("null_reason"), TEXT("typed_value_unavailable_export_text_preserved"));
		}
	}
	return Json;
}

static TSharedRef<FJsonObject> EditableObjectSnapshot(UObject* Object)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("object"), FSololmcpEditorServices::MakeObjectReference(Object));
	TArray<TSharedPtr<FJsonValue>> Properties;
	TArray<TSharedPtr<FJsonValue>> Unsupported;
	int32 ReflectedCount = 0;
	if (Object)
	{
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit)) continue;
			bool bUnsupported = false;
			TSharedRef<FJsonObject> Row = PropertySnapshot(Object, Property, bUnsupported);
			Properties.Add(MakeShared<FJsonValueObject>(Row));
			++ReflectedCount;
			if (bUnsupported)
			{
				TSharedRef<FJsonObject> Reason = MakeShared<FJsonObject>();
				Reason->SetStringField(TEXT("property"), Property->GetName());
				Reason->SetStringField(TEXT("reason"), TEXT("typed_json_unavailable_export_text_preserved"));
				Unsupported.Add(MakeShared<FJsonValueObject>(Reason));
			}
		}
	}
	Json->SetNumberField(TEXT("editable_property_count"), ReflectedCount);
	Json->SetArrayField(TEXT("properties"), Properties);
	Json->SetArrayField(TEXT("unsupported_fields"), Unsupported);
	Json->SetBoolField(TEXT("lossless_export_text_included"), true);
	return Json;
}

static TSharedRef<FJsonObject> LightSnapshot(UWorld* World, ULightComponentBase* Component, const FString& RequestedContext)
{
	TSharedRef<FJsonObject> Json = EditableObjectSnapshot(Component);
	Json->SetStringField(TEXT("schema"), TEXT("somol.light_component_snapshot:v1"));
	Json->SetObjectField(TEXT("world"), WorldIdentity(World, RequestedContext));
	Json->SetStringField(TEXT("light_type"), Component ? LightType(Component) : TEXT("none"));
	AActor* Owner = Component ? Component->GetOwner() : nullptr;
	Json->SetObjectField(TEXT("actor"), FSololmcpEditorServices::MakeActorReference(Owner));
	if (Owner)
	{
		Json->SetObjectField(TEXT("actor_transform"), TransformJson(Owner->GetActorTransform()));
#if WITH_EDITOR
		Json->SetStringField(TEXT("actor_label"), Owner->GetActorLabel());
		Json->SetStringField(TEXT("actor_guid"), Owner->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphens));
#endif
	}
	else
	{
		Json->SetStringField(TEXT("null_reason"), TEXT("light_component_has_no_owner"));
	}
	return Json;
}

static FString SnapshotHash(const TSharedRef<FJsonObject>& Snapshot)
{
	FString Text;
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (Snapshot->TryGetArrayField(TEXT("properties"), Properties) && Properties)
	{
		Text = Properties->Num() > 0 ? FString::FromInt(Properties->Num()) : TEXT("0");
		for (const TSharedPtr<FJsonValue>& RowValue : *Properties)
		{
			const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
			if (Row.IsValid()) Text += Row->GetStringField(TEXT("name")) + TEXT("=") + Row->GetStringField(TEXT("export_text")) + TEXT(";");
		}
	}
	return FString::Printf(TEXT("%08x"), GetTypeHash(Text));
}

static TSharedRef<FJsonObject> ActorRow(AActor* Actor, UActorComponent* Component)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("actor"), FSololmcpEditorServices::MakeActorReference(Actor));
#if WITH_EDITOR
	Json->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : FString());
#endif
	if (Actor) Json->SetObjectField(TEXT("transform"), TransformJson(Actor->GetActorTransform()));
	Json->SetObjectField(TEXT("component"), FSololmcpEditorServices::MakeObjectReference(Component));
	return Json;
}

static TSharedRef<FJsonObject> UnavailableValue(const FString& Reason, const FString& Provenance)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("available"), false);
	Json->SetStringField(TEXT("reason"), Reason);
	Json->SetStringField(TEXT("provenance"), Provenance);
	return Json;
}

static TSharedRef<FJsonObject> LevelBuildIdentity(ULevel* Level)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("available"), Level != nullptr);
	if (!Level)
	{
		Json->SetStringField(TEXT("reason"), TEXT("level_not_loaded"));
		return Json;
	}
	UPackage* LevelPackage = Level->GetPackage();
	Json->SetStringField(TEXT("level_path"), Level->GetPathName());
	Json->SetStringField(TEXT("level_package"), LevelPackage ? LevelPackage->GetName() : FString());
	Json->SetStringField(TEXT("package_persistent_guid"), LevelPackage
		? LevelPackage->GetPersistentGuid().ToString(EGuidFormats::DigitsWithHyphens)
		: FString());
	Json->SetStringField(TEXT("level_build_data_id"), Level->LevelBuildDataId.ToString(EGuidFormats::DigitsWithHyphens));
	Json->SetBoolField(TEXT("level_build_data_id_valid"), Level->LevelBuildDataId.IsValid());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	Json->SetBoolField(TEXT("is_map_build_data_owner"), Level->IsMapBuildDataOwner());
	Json->SetBoolField(TEXT("is_map_build_data_owner_available"), true);
#else
	Json->SetBoolField(TEXT("is_map_build_data_owner"), false);
	Json->SetBoolField(TEXT("is_map_build_data_owner_available"), false);
	Json->SetStringField(TEXT("is_map_build_data_owner_unavailable_reason"),
		TEXT("ULevel::IsMapBuildDataOwner is not public before UE 5.8."));
#endif
	Json->SetBoolField(TEXT("is_lighting_scenario"), Level->bIsLightingScenario != 0);
	Json->SetBoolField(TEXT("geometry_dirty_for_lighting"), Level->bGeometryDirtyForLighting != 0);
	Json->SetObjectField(TEXT("map_build_data"), FSololmcpEditorServices::MakeObjectReference(Level->MapBuildData));
	if (Level->MapBuildData)
	{
		UPackage* BuildPackage = Level->MapBuildData->GetPackage();
		Json->SetStringField(TEXT("map_build_data_package"), BuildPackage ? BuildPackage->GetName() : FString());
		Json->SetStringField(TEXT("map_build_data_package_guid"), BuildPackage
			? BuildPackage->GetPersistentGuid().ToString(EGuidFormats::DigitsWithHyphens)
			: FString());
	}
	const FString IdentityMaterial = Json->GetStringField(TEXT("level_package")) + TEXT("|")
		+ Json->GetStringField(TEXT("package_persistent_guid")) + TEXT("|")
		+ Json->GetStringField(TEXT("level_build_data_id")) + TEXT("|")
		+ (Level->MapBuildData ? Level->MapBuildData->GetPathName() : TEXT("none"));
	Json->SetStringField(TEXT("identity_fingerprint"), FString::Printf(TEXT("%08x"), GetTypeHash(IdentityMaterial)));
	Json->SetStringField(TEXT("identity_fingerprint_semantics"), TEXT("identity_only_not_a_content_hash"));
	Json->SetObjectField(TEXT("last_build_timestamp"), UnavailableValue(
		TEXT("UE public map-build data does not retain a trustworthy last-build timestamp."),
		TEXT("engine_public_api_boundary")));
	Json->SetObjectField(TEXT("content_hash"), UnavailableValue(
		TEXT("Computing a complete lighting build content hash requires private build artifacts or a package-level hashing gate."),
		TEXT("engine_public_api_boundary")));
	return Json;
}

static TSharedRef<FJsonObject> ReflectionCaptureBuildIdentity(UWorld* World, UReflectionCaptureComponent* Component)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("component"), FSololmcpEditorServices::MakeObjectReference(Component));
	if (!Component)
	{
		Json->SetStringField(TEXT("null_reason"), TEXT("capture_component_missing"));
		return Json;
	}
	Json->SetStringField(TEXT("map_build_data_id"), Component->MapBuildDataId.ToString(EGuidFormats::DigitsWithHyphens));
	Json->SetBoolField(TEXT("map_build_data_id_valid"), Component->MapBuildDataId.IsValid());
#if WITH_EDITOR
	Json->SetBoolField(TEXT("map_build_data_id_loaded"), Component->bMapBuildDataIdLoaded);
#endif
	Json->SetBoolField(TEXT("has_map_build_data"), Component->GetMapBuildData() != nullptr);
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	Json->SetBoolField(TEXT("runtime_capture"), Component->IsRuntimeCapture());
	Json->SetBoolField(TEXT("runtime_capture_available"), true);
	#else
	Json->SetBoolField(TEXT("runtime_capture"), false);
	Json->SetBoolField(TEXT("runtime_capture_available"), false);
	Json->SetStringField(TEXT("runtime_capture_unavailable_reason"),
		TEXT("UReflectionCaptureComponent::IsRuntimeCapture is not public before UE 5.8."));
	#endif
	Json->SetNumberField(TEXT("world_unbuilt_reflection_capture_count"), World ? World->NumUnbuiltReflectionCaptures : 0);
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	Json->SetBoolField(TEXT("global_capture_update_queue_nonempty"), UReflectionCaptureComponent::HasReflectionCapturesToUpdate());
	Json->SetBoolField(TEXT("global_capture_update_queue_available"), true);
	#else
	Json->SetBoolField(TEXT("global_capture_update_queue_nonempty"), false);
	Json->SetBoolField(TEXT("global_capture_update_queue_available"), false);
	Json->SetStringField(TEXT("global_capture_update_queue_unavailable_reason"),
		TEXT("The reflection-capture update-queue query is not public before UE 5.8."));
	#endif
	Json->SetObjectField(TEXT("per_capture_dirty_exact"), UnavailableValue(
		TEXT("The exact bNeedsRecaptureOrUpload flag is private and has no public per-component getter in UE 5.8."),
		TEXT("UReflectionCaptureComponent public API")));
	Json->SetStringField(TEXT("dirty_evidence"), Component->GetMapBuildData()
		? TEXT("map_build_data_present")
		: TEXT("map_build_data_missing"));
	Json->SetStringField(TEXT("dirty_evidence_semantics"),
		TEXT("evidence_only_not_an_exact_per_capture_dirty_verdict"));
	return Json;
}

static TSharedRef<FJsonObject> PerLightSourceChain(ULightComponentBase* Component)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetObjectField(TEXT("component_instance"), FSololmcpEditorServices::MakeObjectReference(Component));
	UObject* Archetype = Component ? Component->GetArchetype() : nullptr;
	Json->SetObjectField(TEXT("archetype"), FSololmcpEditorServices::MakeObjectReference(Archetype));
	Json->SetStringField(TEXT("source_order"), TEXT("class_default_or_archetype -> component_instance -> world/view/renderer -> render_thread_gpu"));
	TArray<TSharedPtr<FJsonValue>> Overrides;
	if (Component && Archetype && Archetype->GetClass() == Component->GetClass())
	{
		for (TFieldIterator<FProperty> It(Component->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit)) continue;
			if (Property->Identical_InContainer(Component, Archetype)) continue;
			bool bUnsupportedInstance = false;
			bool bUnsupportedArchetype = false;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("property"), Property->GetName());
			Row->SetObjectField(TEXT("instance"), PropertySnapshot(Component, Property, bUnsupportedInstance));
			Row->SetObjectField(TEXT("archetype"), PropertySnapshot(Archetype, Property, bUnsupportedArchetype));
			Row->SetStringField(TEXT("winning_source"), TEXT("component_instance"));
			Overrides.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Json->SetArrayField(TEXT("instance_overrides"), Overrides);
	Json->SetNumberField(TEXT("instance_override_count"), Overrides.Num());
	Json->SetBoolField(TEXT("registered"), Component && Component->IsRegistered());
	Json->SetBoolField(TEXT("active"), Component && Component->IsActive());
	Json->SetBoolField(TEXT("visible"), Component && Component->IsVisible());
	AActor* Owner = Component ? Component->GetOwner() : nullptr;
	Json->SetBoolField(TEXT("owner_hidden"), Owner && Owner->IsHidden());
#if WITH_EDITOR
	Json->SetBoolField(TEXT("owner_hidden_in_editor"), Owner && Owner->IsTemporarilyHiddenInEditor());
#endif
	Json->SetObjectField(TEXT("gpu_final_contribution"), UnavailableValue(
		TEXT("Final clustered-light selection, shadowing, Lumen transport and per-pixel contribution are render-thread/GPU results."),
		TEXT("render_thread_gpu")));
	return Json;
}

static TSharedRef<FJsonObject> ReadRendererState(UWorld* World, const FString& RequestedContext)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("schema"), TEXT("somol.lighting_renderer_effective:v2"));
	Out->SetObjectField(TEXT("world"), WorldIdentity(World, RequestedContext));
	TSharedRef<FJsonObject> ProjectSettings = EditableObjectSnapshot(const_cast<URendererSettings*>(GetDefault<URendererSettings>()));
	ProjectSettings->SetStringField(TEXT("provenance"), TEXT("URendererSettings class default object resolved from project config hierarchy"));
	ProjectSettings->SetStringField(TEXT("config_file"), GEngineIni);
	ProjectSettings->SetStringField(TEXT("config_section"), TEXT("/Script/Engine.RendererSettings"));
	Out->SetObjectField(TEXT("project_renderer_settings"), ProjectSettings);
	Out->SetObjectField(TEXT("world_settings"), EditableObjectSnapshot(World ? World->GetWorldSettings() : nullptr));

	UDeviceProfile* ActiveProfile = UDeviceProfileManager::Get().GetActiveProfile();
	TSharedRef<FJsonObject> Platform = MakeShared<FJsonObject>();
	Platform->SetStringField(TEXT("runtime_platform"), ANSI_TO_TCHAR(FPlatformProperties::PlatformName()));
	Platform->SetStringField(TEXT("ini_platform"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
	Platform->SetStringField(TEXT("rhi_shader_platform"), LexToString(GMaxRHIShaderPlatform));
	Platform->SetStringField(TEXT("active_device_profile"), UDeviceProfileManager::Get().GetActiveDeviceProfileName());
	Platform->SetObjectField(TEXT("device_profile_object"), FSololmcpEditorServices::MakeObjectReference(ActiveProfile));
	Platform->SetStringField(TEXT("device_profile_base"), ActiveProfile ? ActiveProfile->BaseProfileName : FString());
	Platform->SetStringField(TEXT("device_type"), ActiveProfile ? ActiveProfile->DeviceType : FString());
	Out->SetObjectField(TEXT("platform_and_device_profile"), Platform);

	static const TCHAR* CVarNames[] = {
		TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("r.ReflectionMethod"),
		TEXT("r.Lumen.DiffuseIndirect.Allow"), TEXT("r.Lumen.Reflections.Allow"),
		TEXT("r.Lumen.HardwareRayTracing"), TEXT("r.MegaLights.Enable"),
		TEXT("r.MegaLights.Allow"), TEXT("r.Shadow.Virtual.Enable"),
		TEXT("r.RayTracing"), TEXT("r.AllowStaticLighting"),
		TEXT("r.DefaultFeature.AutoExposure"), TEXT("r.DefaultFeature.AutoExposure.Method")
	};
	TArray<TSharedPtr<FJsonValue>> CVars;
	for (const TCHAR* Name : CVarNames)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Name);
		TArray<TSharedPtr<FJsonValue>> SourceChain;
		FString ProjectIniValue;
		const bool bProjectIniValue = GConfig && GConfig->GetString(TEXT("SystemSettings"), Name, ProjectIniValue, GEngineIni);
		TSharedRef<FJsonObject> ProjectLayer = MakeShared<FJsonObject>();
		ProjectLayer->SetStringField(TEXT("layer"), TEXT("project_system_settings_ini"));
		ProjectLayer->SetBoolField(TEXT("available"), bProjectIniValue);
		if (bProjectIniValue) ProjectLayer->SetStringField(TEXT("value"), ProjectIniValue);
		else ProjectLayer->SetStringField(TEXT("null_reason"), TEXT("cvar_not_authored_in_project_SystemSettings"));
		SourceChain.Add(MakeShared<FJsonValueObject>(ProjectLayer));

		FString DeviceProfileValue;
		const bool bDeviceProfileValue = ActiveProfile && ActiveProfile->GetConsolidatedCVarValue(Name, DeviceProfileValue, false);
		TSharedRef<FJsonObject> DeviceLayer = MakeShared<FJsonObject>();
		DeviceLayer->SetStringField(TEXT("layer"), TEXT("active_device_profile_hierarchy"));
		DeviceLayer->SetBoolField(TEXT("available"), bDeviceProfileValue);
		DeviceLayer->SetStringField(TEXT("profile"), ActiveProfile ? ActiveProfile->GetName() : FString());
		if (bDeviceProfileValue) DeviceLayer->SetStringField(TEXT("value"), DeviceProfileValue);
		else DeviceLayer->SetStringField(TEXT("null_reason"), TEXT("no_active_device_profile_override"));
		SourceChain.Add(MakeShared<FJsonValueObject>(DeviceLayer));

		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Row->SetBoolField(TEXT("available"), true);
			Row->SetStringField(TEXT("effective_value"), CVar->GetString());
			Row->SetNumberField(TEXT("flags"), static_cast<int64>(CVar->GetFlags()));
			const EConsoleVariableFlags SetBy = static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask);
			Row->SetStringField(TEXT("effective_set_by"), GetConsoleVariableSetByName(SetBy));
			TSharedRef<FJsonObject> EffectiveLayer = MakeShared<FJsonObject>();
			EffectiveLayer->SetStringField(TEXT("layer"), TEXT("effective_console_manager"));
			EffectiveLayer->SetBoolField(TEXT("available"), true);
			EffectiveLayer->SetStringField(TEXT("value"), CVar->GetString());
			EffectiveLayer->SetStringField(TEXT("winning_source"), GetConsoleVariableSetByName(SetBy));
			SourceChain.Add(MakeShared<FJsonValueObject>(EffectiveLayer));
		}
		else
		{
			Row->SetBoolField(TEXT("available"), false);
			Row->SetStringField(TEXT("null_reason"), TEXT("console_variable_not_registered_for_current_renderer_or_platform"));
		}
		Row->SetArrayField(TEXT("source_chain"), SourceChain);
		CVars.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetArrayField(TEXT("effective_console_variables"), CVars);

	TArray<TSharedPtr<FJsonValue>> EffectiveLights;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			TInlineComponentArray<ULightComponentBase*> Components(Actor);
			for (ULightComponentBase* Light : Components)
			{
				if (!Light) continue;
				TSharedRef<FJsonObject> LightRow = ActorRow(Actor, Light);
				LightRow->SetStringField(TEXT("light_type"), LightType(Light));
				LightRow->SetObjectField(TEXT("source_chain"), PerLightSourceChain(Light));
				EffectiveLights.Add(MakeShared<FJsonValueObject>(LightRow));
			}
		}
	}
	Out->SetArrayField(TEXT("per_light_effective_source_chains"), EffectiveLights);
	Out->SetNumberField(TEXT("per_light_count"), EffectiveLights.Num());
	Out->SetBoolField(TEXT("render_thread_exact"), false);
	Out->SetObjectField(TEXT("gpu_final_lighting"), UnavailableValue(
		TEXT("Per-pixel light selection, shadow visibility, Lumen transport and final exposure are render-thread/GPU products."),
		TEXT("render_thread_gpu")));
	Out->SetArrayField(TEXT("unsupported_fields"), {
		MakeShared<FJsonValueString>(TEXT("per-pixel light contribution and clustered-light culling are render-thread/GPU state")),
		MakeShared<FJsonValueString>(TEXT("the source chain reports public config/CVar/component provenance and never fabricates private renderer graph state"))
	});
	return Out;
}

static FLevelEditorViewportClient* ActiveLevelViewport(int32& OutIndex)
{
	OutIndex = INDEX_NONE;
	if (!GEditor) return nullptr;
	FViewport* Active = GEditor->GetActiveViewport();
	const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
	for (int32 Index = 0; Index < Clients.Num(); ++Index)
	{
		if (Clients[Index] && Clients[Index]->Viewport == Active)
		{
			OutIndex = Index;
			return Clients[Index];
		}
	}
	for (int32 Index = 0; Index < Clients.Num(); ++Index)
	{
		if (Clients[Index] && Clients[Index]->IsPerspective())
		{
			OutIndex = Index;
			return Clients[Index];
		}
	}
	return nullptr;
}

static TSharedPtr<ISequencer> ActiveSequencerFor(ULevelSequence* Sequence)
{
	if (!Sequence || !GEditor || ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() != Sequence)
	{
		return nullptr;
	}
	UAssetEditorSubsystem* EditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	IAssetEditorInstance* EditorInstance = EditorSubsystem ? EditorSubsystem->FindEditorForAsset(Sequence, false) : nullptr;
	if (!EditorInstance || EditorInstance->GetEditorName() != FName(TEXT("LevelSequenceEditor")))
	{
		return nullptr;
	}
	ILevelSequenceEditorToolkit* Toolkit = static_cast<ILevelSequenceEditorToolkit*>(EditorInstance);
	return Toolkit ? Toolkit->GetSequencer() : nullptr;
}

static TSharedRef<FJsonObject> SequencerCameraAtFrame(
	UWorld* World,
	const FString& SequencePath,
	const int32 DisplayFrame,
	UCameraComponent*& OutCamera,
	FVector& OutLocation,
	FRotator& OutRotation)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("sequence_path"), SequencePath);
	Json->SetNumberField(TEXT("requested_display_frame"), DisplayFrame);
	Json->SetObjectField(TEXT("requested_world"), FSololmcpEditorServices::MakeObjectReference(World));
	Json->SetBoolField(TEXT("side_effect_free"), true);
	Json->SetStringField(TEXT("evaluation_policy"), TEXT("observe_existing_active_evaluation_only_never_seek_or_spawn"));
	OutCamera = nullptr;
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
	if (!Sequence)
	{
		Json->SetObjectField(TEXT("evaluated_camera"), UnavailableValue(TEXT("Level Sequence asset was not found."), TEXT("asset_load")));
		return Json;
	}
	Json->SetObjectField(TEXT("sequence"), FSololmcpEditorServices::MakeObjectReference(Sequence));
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		Json->SetObjectField(TEXT("evaluated_camera"), UnavailableValue(TEXT("Level Sequence has no MovieScene."), TEXT("ULevelSequence.GetMovieScene")));
		return Json;
	}
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameTime TickTime = FFrameRate::TransformTime(FFrameTime(FFrameNumber(DisplayFrame)), DisplayRate, TickResolution);
	const FFrameNumber TickFrame = TickTime.RoundToFrame();
	Json->SetNumberField(TEXT("display_rate_numerator"), DisplayRate.Numerator);
	Json->SetNumberField(TEXT("display_rate_denominator"), DisplayRate.Denominator);
	Json->SetNumberField(TEXT("tick_resolution_numerator"), TickResolution.Numerator);
	Json->SetNumberField(TEXT("tick_resolution_denominator"), TickResolution.Denominator);
	Json->SetNumberField(TEXT("requested_tick_frame"), TickFrame.Value);

	UMovieSceneCameraCutSection* ActiveCut = nullptr;
	if (UMovieSceneCameraCutTrack* CameraTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack()))
	{
		for (UMovieSceneSection* Section : CameraTrack->GetAllSections())
		{
			UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
			if (Cut && Cut->GetRange().Contains(TickFrame))
			{
				ActiveCut = Cut;
				break;
			}
		}
	}
	if (!ActiveCut)
	{
		Json->SetObjectField(TEXT("camera_cut"), UnavailableValue(TEXT("No camera-cut section contains the requested frame."), TEXT("UMovieSceneCameraCutTrack")));
		Json->SetObjectField(TEXT("evaluated_camera"), UnavailableValue(TEXT("No camera cut exists at the requested frame."), TEXT("sequencer_camera_cut")));
		return Json;
	}
	const FGuid BindingGuid = ActiveCut->GetCameraBindingID().GetGuid();
	TSharedRef<FJsonObject> CutJson = MakeShared<FJsonObject>();
	CutJson->SetStringField(TEXT("section_path"), ActiveCut->GetPathName());
	CutJson->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	CutJson->SetBoolField(TEXT("binding_guid_valid"), BindingGuid.IsValid());
	Json->SetObjectField(TEXT("camera_cut"), CutJson);

	TSharedPtr<ISequencer> Sequencer = ActiveSequencerFor(Sequence);
	if (!Sequencer)
	{
		Json->SetObjectField(TEXT("evaluated_camera"), UnavailableValue(
			TEXT("The requested sequence is not currently open and evaluated by Sequencer; this reader will not seek or spawn a player."),
			TEXT("side_effect_free_sequencer_boundary")));
		return Json;
	}
	const FQualifiedFrameTime LocalTime = Sequencer->GetLocalTime();
	const FFrameTime CurrentDisplayTime = LocalTime.ConvertTo(DisplayRate);
	const FFrameNumber CurrentDisplayFrame = CurrentDisplayTime.RoundToFrame();
	Json->SetNumberField(TEXT("active_sequencer_display_frame"), CurrentDisplayFrame.Value);
	Json->SetNumberField(TEXT("active_sequencer_subframe"), CurrentDisplayTime.GetSubFrame());
	const bool bExactRequestedFrame = CurrentDisplayFrame.Value == DisplayFrame;
	Json->SetBoolField(TEXT("active_sequencer_matches_requested_frame"), bExactRequestedFrame);
	if (!bExactRequestedFrame)
	{
		Json->SetObjectField(TEXT("evaluated_camera"), UnavailableValue(
			TEXT("Sequencer is open but is not currently evaluated at the requested display frame; no seek was performed."),
			TEXT("ISequencer.GetLocalTime")));
		return Json;
	}

	TArrayView<TWeakObjectPtr<>> BoundObjects = Sequencer->FindObjectsInCurrentSequence(BindingGuid);
	TArray<TSharedPtr<FJsonValue>> BoundRows;
	for (const TWeakObjectPtr<>& WeakObject : BoundObjects)
	{
		UObject* Object = WeakObject.Get();
		BoundRows.Add(MakeShared<FJsonValueObject>(FSololmcpEditorServices::MakeObjectReference(Object)));
		if (!OutCamera)
		{
			OutCamera = Cast<UCameraComponent>(Object);
			if (!OutCamera)
			{
				if (AActor* Actor = Cast<AActor>(Object)) OutCamera = Actor->FindComponentByClass<UCameraComponent>();
			}
		}
	}
	Json->SetArrayField(TEXT("evaluated_bound_objects"), BoundRows);
	if (!OutCamera)
	{
		Json->SetObjectField(TEXT("evaluated_camera"), UnavailableValue(
			TEXT("The evaluated camera binding did not resolve to a UCameraComponent."),
			TEXT("ISequencer.FindObjectsInCurrentSequence")));
		return Json;
	}
	OutLocation = OutCamera->GetComponentLocation();
	OutRotation = OutCamera->GetComponentRotation();
	TSharedRef<FJsonObject> CameraJson = EditableObjectSnapshot(OutCamera);
	CameraJson->SetBoolField(TEXT("available"), true);
	CameraJson->SetStringField(TEXT("provenance"), TEXT("active Sequencer evaluated binding at exact requested frame"));
	Json->SetObjectField(TEXT("evaluated_camera"), CameraJson);
	return Json;
}

static TSharedRef<FJsonObject> PiePlayerCamera(
	UWorld* World,
	const int32 PlayerIndex,
	UCameraComponent*& OutCamera,
	FVector& OutLocation,
	FRotator& OutRotation,
	FString& Error)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("player_index"), PlayerIndex);
	OutCamera = nullptr;
	if (!World || !(World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game || World->WorldType == EWorldType::GamePreview))
	{
		Error = TEXT("pie_player view_source requires a PIE, standalone, game, or game-preview world.");
		return Json;
	}
	APlayerController* Controller = UGameplayStatics::GetPlayerController(World, PlayerIndex);
	if (!Controller || !Controller->PlayerCameraManager)
	{
		Error = FString::Printf(TEXT("Player %d or its PlayerCameraManager is unavailable in the requested world."), PlayerIndex);
		return Json;
	}
	APlayerCameraManager* CameraManager = Controller->PlayerCameraManager;
	AActor* ViewTarget = CameraManager->GetViewTarget();
	const FMinimalViewInfo& POV = CameraManager->GetCameraCacheView();
	OutLocation = CameraManager->GetCameraLocation();
	OutRotation = CameraManager->GetCameraRotation();
	OutCamera = ViewTarget ? ViewTarget->FindComponentByClass<UCameraComponent>() : nullptr;
	Json->SetBoolField(TEXT("available"), true);
	Json->SetStringField(TEXT("provenance"), TEXT("APlayerCameraManager current evaluated camera cache"));
	Json->SetObjectField(TEXT("player_controller"), FSololmcpEditorServices::MakeActorReference(Controller));
	Json->SetObjectField(TEXT("player_camera_manager"), FSololmcpEditorServices::MakeActorReference(CameraManager));
	Json->SetObjectField(TEXT("view_target"), FSololmcpEditorServices::MakeActorReference(ViewTarget));
	Json->SetObjectField(TEXT("camera_component"), FSololmcpEditorServices::MakeObjectReference(OutCamera));
	Json->SetObjectField(TEXT("location_cm"), VectorJson(OutLocation));
	Json->SetObjectField(TEXT("rotation_deg"), RotatorJson(OutRotation));
	Json->SetNumberField(TEXT("fov_degrees"), CameraManager->GetFOVAngle());
	Json->SetNumberField(TEXT("post_process_blend_weight"), POV.PostProcessBlendWeight);
	TSharedRef<FJsonObject> CameraCacheJson = MakeShared<FJsonObject>();
	if (FJsonObjectConverter::UStructToJsonObject(FMinimalViewInfo::StaticStruct(), &POV, CameraCacheJson, 0, 0))
	{
		Json->SetObjectField(TEXT("camera_cache_view"), NormalizeJsonObject(CameraCacheJson));
	}
	else
	{
		Json->SetObjectField(TEXT("camera_cache_view"), UnavailableValue(
			TEXT("FMinimalViewInfo conversion failed for the current camera cache."),
			TEXT("FJsonObjectConverter")));
	}
	Json->SetStringField(TEXT("network_role"), Controller->GetLocalRole() == ROLE_Authority ? TEXT("authority") : TEXT("client_or_simulated"));
	return Json;
}

static TSharedRef<FJsonObject> EffectiveViewInput()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Camera actor label, name, or path for camera_actor/auto source."))},
		{TEXT("world_context"), FSololmcpSchemaBuilder::String(TEXT("Explicit world selection."), {TEXT("editor"), TEXT("pie"), TEXT("standalone"), TEXT("preview"), TEXT("any")})},
		{TEXT("view_source"), FSololmcpSchemaBuilder::String(TEXT("auto, editor_viewport, camera_actor, pie_player, or sequencer."), {TEXT("auto"), TEXT("editor_viewport"), TEXT("camera_actor"), TEXT("pie_player"), TEXT("sequencer")})},
		{TEXT("player_index"), FSololmcpSchemaBuilder::Integer(TEXT("PIE/standalone local player index."), 0, 16)},
		{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("Level Sequence asset path for sequencer source."))},
		{TEXT("sequence_frame"), FSololmcpSchemaBuilder::Integer(TEXT("Requested display-rate frame. Reader never seeks; exact evaluated camera requires active Sequencer already at this frame."))}
	}, {}, FString(), false);
}

static TSharedRef<FJsonObject> CommonReadInput(bool bActorRequired)
{
	TMap<FString, TSharedRef<FJsonObject>> Fields = {
		{TEXT("actor"), FSololmcpSchemaBuilder::String(TEXT("Actor label, object name, or full object path."))},
		{TEXT("world_context"), FSololmcpSchemaBuilder::String(TEXT("Explicit world selection."), {TEXT("editor"), TEXT("pie"), TEXT("standalone"), TEXT("preview"), TEXT("any")})}
	};
	return FSololmcpSchemaBuilder::Object(Fields, bActorRequired ? TArray<FString>{TEXT("actor")} : TArray<FString>{}, FString(), false);
}

static TSharedRef<FJsonObject> GenericOutputSchema()
{
	return FSololmcpSchemaBuilder::Object({}, {}, TEXT("Structured native UE 5.8 lighting result; see schema field for version."), true);
}
} // namespace LightingInspection

void RegisterLightingInspectionTools(FSololmcpToolRegistry& Registry)
{
	using namespace LightingInspection;

	Registry.Register({
		TEXT("light_component_inspect"),
		TEXT("Read every reflected editable field on a Point, Spot, Rect, Directional, or other ULightComponent without side effects; returns typed JSON, lossless export text, identity, world context, and explicit unsupported reasons."),
		CommonReadInput(true),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			FString ActorId;
			Args->TryGetStringField(TEXT("actor"), ActorId);
			ULightComponentBase* Component = ResolveLight(World, ActorId, Error);
			if (!Component) return false;
			if (Cast<USkyLightComponent>(Component))
			{
				Error = TEXT("Actor contains a Sky Light; use sky_light_component_inspect so the component kind is explicit.");
				return false;
			}
			Out = LightSnapshot(World, Component, ContextName);
			Out->SetStringField(TEXT("snapshot_hash"), SnapshotHash(Out));
			Out->SetBoolField(TEXT("side_effect_free"), true);
			Summary = FString::Printf(TEXT("Inspected %d editable fields on %s."), static_cast<int32>(Out->GetNumberField(TEXT("editable_property_count"))), *LightType(Component));
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 2, GenericOutputSchema()
	});

	Registry.Register({
		TEXT("sky_light_component_inspect"),
		TEXT("Read every reflected editable USkyLightComponent field, including source/cubemap, resolution, lower hemisphere, DFAO, cloud AO, real-time capture, identity and explicit null reasons."),
		CommonReadInput(true),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			FString ActorId;
			Args->TryGetStringField(TEXT("actor"), ActorId);
			USkyLightComponent* Component = Cast<USkyLightComponent>(ResolveLight(World, ActorId, Error));
			if (!Component)
			{
				if (Error.IsEmpty()) Error = TEXT("Actor does not contain a USkyLightComponent.");
				return false;
			}
			Out = LightSnapshot(World, Component, ContextName);
			Out->SetStringField(TEXT("snapshot_hash"), SnapshotHash(Out));
			Out->SetBoolField(TEXT("side_effect_free"), true);
			Summary = FString::Printf(TEXT("Inspected %d editable Sky Light fields."), static_cast<int32>(Out->GetNumberField(TEXT("editable_property_count"))));
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 2, GenericOutputSchema()
	});

	Registry.Register({
		TEXT("light_component_patch"),
		TEXT("Transactionally patch reflected editable fields on all five light types and return per-property readback plus complete before/after snapshots. Sky recapture is explicit and defaults off."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor"), FSololmcpSchemaBuilder::String()},
			{TEXT("world_context"), FSololmcpSchemaBuilder::String(TEXT("Explicit world selection; mutation is intentionally restricted to editor world."), {TEXT("editor")})},
			{TEXT("properties"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Exact reflected property names mapped to typed JSON values."), true)},
			{TEXT("recapture_sky"), FSololmcpSchemaBuilder::Boolean(TEXT("After a Sky Light patch, call RecaptureSky. Default false."))}
		}, {TEXT("actor"), TEXT("properties")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			if (World->WorldType != EWorldType::Editor)
			{
				Error = TEXT("light_component_patch is restricted to editor world; PIE/standalone mutation is fail-closed.");
				return false;
			}
			FString ActorId;
			Args->TryGetStringField(TEXT("actor"), ActorId);
			ULightComponentBase* Component = ResolveLight(World, ActorId, Error);
			if (!Component) return false;
			const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
			if (!Args->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid() || (*PropertiesPtr)->Values.IsEmpty())
			{
				Error = TEXT("properties must be a non-empty object.");
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesPtr)->Values)
			{
				FProperty* Property = Component->GetClass()->FindPropertyByName(*Pair.Key);
				if (!Property)
				{
					Error = FString::Printf(TEXT("Property '%s' does not exist on %s."), *Pair.Key, *Component->GetClass()->GetName());
					return false;
				}
				if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient))
				{
					Error = FString::Printf(TEXT("Property '%s' is not an instance-editable persistent field; patch rejected."), *Pair.Key);
					return false;
				}
			}
			bool bRecaptureSky = false;
			Args->TryGetBoolField(TEXT("recapture_sky"), bRecaptureSky);
			if (bRecaptureSky && !Cast<USkyLightComponent>(Component))
			{
				Error = TEXT("recapture_sky is only valid for a Sky Light component.");
				return false;
			}
			// Validate every conversion against a transient duplicate before opening the
			// editor transaction. This prevents a bad late field from leaving an earlier
			// field partially applied.
			ULightComponentBase* Probe = DuplicateObject<ULightComponentBase>(Component, GetTransientPackage());
			if (!Probe)
			{
				Error = TEXT("Unable to allocate a transient light-component preflight copy.");
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> PreflightReceipts;
			if (!Context.Services.ApplyPropertiesWithReceipts(Probe, *PropertiesPtr, PreflightReceipts, Error))
			{
				Error = TEXT("Light patch preflight failed: ") + Error;
				return false;
			}

			TSharedRef<FJsonObject> Before = LightSnapshot(World, Component, ContextName);
			const FString BeforeHash = SnapshotHash(Before);
			TArray<TSharedPtr<FJsonValue>> Receipts;
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "LightingComponentPatch", "SOMOLMCP Patch Light Component"));
			Component->Modify();
			if (AActor* Owner = Component->GetOwner()) Owner->Modify();
			if (!Context.Services.ApplyPropertiesWithReceipts(Component, *PropertiesPtr, Receipts, Error)) return false;
			Component->MarkRenderStateDirty();
			bool bRecaptured = false;
			if (bRecaptureSky)
			{
				USkyLightComponent* Sky = Cast<USkyLightComponent>(Component);
				Sky->RecaptureSky();
				bRecaptured = true;
			}
			Component->MarkPackageDirty();
			TSharedRef<FJsonObject> After = LightSnapshot(World, Component, ContextName);
			const FString AfterHash = SnapshotHash(After);
			Out->SetStringField(TEXT("schema"), TEXT("somol.light_component_patch_receipt:v1"));
			Out->SetObjectField(TEXT("world"), WorldIdentity(World, ContextName));
			Out->SetObjectField(TEXT("target"), FSololmcpEditorServices::MakeObjectReference(Component));
			Out->SetStringField(TEXT("light_type"), LightType(Component));
			Out->SetStringField(TEXT("before_hash"), BeforeHash);
			Out->SetStringField(TEXT("after_hash"), AfterHash);
			Out->SetArrayField(TEXT("property_receipts"), NormalizeJsonArray(Receipts));
			Out->SetObjectField(TEXT("before"), NormalizeJsonObject(Before));
			Out->SetObjectField(TEXT("after"), NormalizeJsonObject(After));
			Out->SetBoolField(TEXT("sky_recaptured"), bRecaptured);
			Out->SetBoolField(TEXT("transactional"), true);
			Out->SetBoolField(TEXT("undo_supported"), true);
			Out->SetBoolField(TEXT("package_dirty"), Component->GetPackage()->IsDirty());
			Summary = FString::Printf(TEXT("Patched and read back %d fields on %s."), Receipts.Num(), *LightType(Component));
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 0, GenericOutputSchema()
	});

	Registry.Register({
		TEXT("lighting_scene_inspect"),
		TEXT("Side-effect-free scene inventory of all ULightComponentBase types, Sky Lights, reflection captures, atmosphere, volumetric clouds, fog and post-process volumes in an explicit world context."),
		CommonReadInput(false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			UPackage* Package = World->PersistentLevel ? World->PersistentLevel->GetPackage() : nullptr;
			const bool bDirtyBefore = Package && Package->IsDirty();
			TArray<TSharedPtr<FJsonValue>> Lights, Captures, Environment;
			TMap<FString, int32> TypeCounts;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor) continue;
				TInlineComponentArray<ULightComponentBase*> Components(Actor);
				for (ULightComponentBase* Component : Components)
				{
					if (!Component) continue;
					TSharedRef<FJsonObject> Row = ActorRow(Actor, Component);
					const FString Type = LightType(Component);
					Row->SetStringField(TEXT("light_type"), Type);
					Row->SetBoolField(TEXT("visible"), Component->IsVisible());
					TypeCounts.FindOrAdd(Type)++;
					Lights.Add(MakeShared<FJsonValueObject>(Row));
				}
				if (AReflectionCapture* Capture = Cast<AReflectionCapture>(Actor))
				{
					TSharedRef<FJsonObject> CaptureRow = ActorRow(Capture, Capture->GetCaptureComponent());
					CaptureRow->SetObjectField(TEXT("build_identity"), ReflectionCaptureBuildIdentity(World, Capture->GetCaptureComponent()));
					Captures.Add(MakeShared<FJsonValueObject>(CaptureRow));
				}
				if (Actor->IsA<ASkyAtmosphere>() || Actor->IsA<AExponentialHeightFog>() ||
					Actor->IsA<AVolumetricCloud>() || Actor->IsA<APostProcessVolume>())
				{
					TSharedRef<FJsonObject> Row = ActorRow(Actor, nullptr);
					Row->SetStringField(TEXT("environment_type"), Actor->GetClass()->GetPathName());
					Environment.Add(MakeShared<FJsonValueObject>(Row));
				}
			}
			TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Pair : TypeCounts) Counts->SetNumberField(Pair.Key, Pair.Value);
			Out->SetStringField(TEXT("schema"), TEXT("somol.lighting_scene_snapshot:v1"));
			Out->SetObjectField(TEXT("world"), WorldIdentity(World, ContextName));
			Out->SetArrayField(TEXT("lights"), Lights);
			Out->SetArrayField(TEXT("reflection_captures"), Captures);
			Out->SetArrayField(TEXT("environment"), Environment);
			Out->SetObjectField(TEXT("light_type_counts"), Counts);
			Out->SetNumberField(TEXT("light_component_count"), Lights.Num());
			Out->SetNumberField(TEXT("reflection_capture_count"), Captures.Num());
			Out->SetBoolField(TEXT("package_dirty_before"), bDirtyBefore);
			Out->SetBoolField(TEXT("package_dirty_after"), Package && Package->IsDirty());
			Out->SetBoolField(TEXT("side_effect_free"), bDirtyBefore == (Package && Package->IsDirty()));
			Summary = FString::Printf(TEXT("Inspected %d light components and %d reflection captures."), Lights.Num(), Captures.Num());
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 2, GenericOutputSchema()
	});

	Registry.Register({
		TEXT("reflection_capture_inspect"),
		TEXT("Inspect one or all reflection-capture components with all reflected editable fields, build identity and explicit world context without triggering a recapture."),
		CommonReadInput(false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			FString ActorId;
			Args->TryGetStringField(TEXT("actor"), ActorId);
			TArray<TSharedPtr<FJsonValue>> Captures;
			for (TActorIterator<AReflectionCapture> It(World); It; ++It)
			{
				AReflectionCapture* Actor = *It;
				if (!Actor) continue;
				if (!ActorId.IsEmpty() && !Actor->GetPathName().Equals(ActorId) && !Actor->GetName().Equals(ActorId, ESearchCase::IgnoreCase)
#if WITH_EDITOR
					&& !Actor->GetActorLabel().Equals(ActorId, ESearchCase::IgnoreCase)
#endif
				) continue;
				TSharedRef<FJsonObject> Row = ActorRow(Actor, Actor->GetCaptureComponent());
				Row->SetObjectField(TEXT("component_snapshot"), EditableObjectSnapshot(Actor->GetCaptureComponent()));
				Row->SetObjectField(TEXT("build_identity"), ReflectionCaptureBuildIdentity(World, Actor->GetCaptureComponent()));
				Captures.Add(MakeShared<FJsonValueObject>(Row));
			}
			if (!ActorId.IsEmpty() && Captures.IsEmpty())
			{
				Error = FString::Printf(TEXT("Reflection capture '%s' was not found."), *ActorId);
				return false;
			}
			Out->SetStringField(TEXT("schema"), TEXT("somol.reflection_capture_snapshot:v1"));
			Out->SetObjectField(TEXT("world"), WorldIdentity(World, ContextName));
			Out->SetArrayField(TEXT("captures"), Captures);
			Out->SetNumberField(TEXT("count"), Captures.Num());
			Out->SetBoolField(TEXT("side_effect_free"), true);
			Summary = FString::Printf(TEXT("Inspected %d reflection captures."), Captures.Num());
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 2, GenericOutputSchema()
	});

	Registry.Register({
		TEXT("lighting_build_state_inspect"),
		TEXT("Read static-lighting and reflection-capture build state, unbuilt counts, map/package identity, lighting scenarios and WorldSettings/Lightmass fields without starting a build."),
		CommonReadInput(false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			AWorldSettings* Settings = World->GetWorldSettings();
			TArray<TSharedPtr<FJsonValue>> Streaming;
			TArray<TSharedPtr<FJsonValue>> LevelIdentities;
			int32 VisibleLightingScenarios = 0;
			for (ULevelStreaming* Level : World->GetStreamingLevels())
			{
				if (!Level) continue;
				ULevel* LoadedLevel = Level->GetLoadedLevel();
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("world_asset_package"), Level->GetWorldAssetPackageName());
				Row->SetBoolField(TEXT("loaded"), LoadedLevel != nullptr);
				Row->SetBoolField(TEXT("visible"), Level->GetShouldBeVisibleFlag());
				const bool bLightingScenario = LoadedLevel && LoadedLevel->bIsLightingScenario != 0;
				Row->SetBoolField(TEXT("is_static_lighting_scenario"), bLightingScenario);
				if (bLightingScenario && Level->GetShouldBeVisibleFlag()) ++VisibleLightingScenarios;
				Row->SetObjectField(TEXT("build_identity"), LevelBuildIdentity(LoadedLevel));
				Streaming.Add(MakeShared<FJsonValueObject>(Row));
				LevelIdentities.Add(MakeShared<FJsonValueObject>(LevelBuildIdentity(LoadedLevel)));
			}

			TArray<TSharedPtr<FJsonValue>> Warnings;
			auto AddWarning = [&Warnings](const FString& Code, const FString& Severity, const FString& Message, const FString& Provenance)
			{
				TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
				Warning->SetStringField(TEXT("code"), Code);
				Warning->SetStringField(TEXT("severity"), Severity);
				Warning->SetStringField(TEXT("message"), Message);
				Warning->SetStringField(TEXT("provenance"), Provenance);
				Warnings.Add(MakeShared<FJsonValueObject>(Warning));
			};
			if (World->NumLightingUnbuiltObjects > 0)
			{
				AddWarning(TEXT("lighting_unbuilt_objects"), TEXT("warning"),
					FString::Printf(TEXT("%u object(s) require static-lighting rebuild."), World->NumLightingUnbuiltObjects),
					TEXT("UWorld.NumLightingUnbuiltObjects"));
			}
			if (World->NumUnbuiltReflectionCaptures > 0)
			{
				AddWarning(TEXT("reflection_captures_unbuilt"), TEXT("warning"),
					FString::Printf(TEXT("%u reflection capture(s) require rebuild."), World->NumUnbuiltReflectionCaptures),
					TEXT("UWorld.NumUnbuiltReflectionCaptures"));
			}
			if (VisibleLightingScenarios > 1)
			{
				AddWarning(TEXT("multiple_visible_lighting_scenarios"), TEXT("error"),
					FString::Printf(TEXT("%d lighting scenario levels are visible; UE expects at most one."), VisibleLightingScenarios),
					TEXT("ULevel.bIsLightingScenario + ULevelStreaming visibility"));
			}
			if (Settings && Settings->bForceNoPrecomputedLighting && World->NumLightingUnbuiltObjects > 0)
			{
				AddWarning(TEXT("precomputed_lighting_disabled_with_unbuilt_objects"), TEXT("info"),
					TEXT("WorldSettings disables precomputed lighting while the world still reports unbuilt lighting objects."),
					TEXT("AWorldSettings.bForceNoPrecomputedLighting"));
			}
			if (World->PersistentLevel && !World->PersistentLevel->LevelBuildDataId.IsValid())
			{
				AddWarning(TEXT("persistent_level_build_identity_missing"), TEXT("warning"),
					TEXT("Persistent level has no valid LevelBuildDataId."), TEXT("ULevel.LevelBuildDataId"));
			}

			Out->SetStringField(TEXT("schema"), TEXT("somol.lighting_build_state:v2"));
			Out->SetObjectField(TEXT("world"), WorldIdentity(World, ContextName));
			Out->SetNumberField(TEXT("unbuilt_lighting_objects"), World->NumLightingUnbuiltObjects);
			Out->SetNumberField(TEXT("unbuilt_reflection_captures"), World->NumUnbuiltReflectionCaptures);
			Out->SetBoolField(TEXT("lighting_build_running"), GEditor && GEditor->IsLightingBuildCurrentlyRunning());
			Out->SetBoolField(TEXT("lighting_build_exporting"), GEditor && GEditor->IsLightingBuildCurrentlyExporting());
			Out->SetBoolField(TEXT("force_no_precomputed_lighting"), Settings && Settings->bForceNoPrecomputedLighting);
			Out->SetObjectField(TEXT("world_settings"), EditableObjectSnapshot(Settings));
			Out->SetArrayField(TEXT("streaming_levels"), Streaming);
			Out->SetObjectField(TEXT("persistent_level_build_identity"), LevelBuildIdentity(World->PersistentLevel));
			Out->SetArrayField(TEXT("streaming_level_build_identities"), LevelIdentities);
			Out->SetNumberField(TEXT("visible_lighting_scenario_count"), VisibleLightingScenarios);
			Out->SetArrayField(TEXT("warnings"), Warnings);
			Out->SetStringField(TEXT("warning_semantics"), TEXT("deterministic_public_state_diagnostics_not_a_full_editor_MapCheck"));
			Out->SetObjectField(TEXT("full_map_check_diagnostics"), UnavailableValue(
				TEXT("A complete Map Check report is not exposed as a side-effect-free stable public query in this reader."),
				TEXT("editor_map_check_boundary")));
			Out->SetBoolField(TEXT("persistent_package_dirty"), World->PersistentLevel && World->PersistentLevel->GetPackage()->IsDirty());
			Out->SetBoolField(TEXT("build_required"), World->NumLightingUnbuiltObjects > 0 || World->NumUnbuiltReflectionCaptures > 0);
			Out->SetBoolField(TEXT("side_effect_free"), true);
			Summary = FString::Printf(TEXT("Lighting build state: %u unbuilt objects, %u unbuilt captures."), World->NumLightingUnbuiltObjects, World->NumUnbuiltReflectionCaptures);
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 1, GenericOutputSchema()
	});

	Registry.Register({
		TEXT("lighting_renderer_effective_inspect"),
		TEXT("Resolve authored renderer settings and effective Lumen, reflection, MegaLights, VSM, ray tracing, static-lighting and exposure CVars with value-source flags and explicit GPU-only limitations."),
		CommonReadInput(false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			Out = ReadRendererState(World, ContextName);
			Out->SetBoolField(TEXT("side_effect_free"), true);
			Summary = TEXT("Resolved project and current renderer lighting settings.");
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 1, GenericOutputSchema()
	});

	Registry.Register({
		TEXT("lighting_effective_view_inspect"),
		TEXT("Inspect an editor viewport, camera actor, PIE/standalone player camera, or side-effect-free active Sequencer evaluation at a specified frame; returns exposure/PP/light/config provenance and explicit GPU-only unavailable results."),
		EffectiveViewInput(),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ContextName;
			UWorld* World = ResolveWorld(Args, ContextName, Error);
			if (!World) return false;
			FVector ViewLocation = FVector::ZeroVector;
			FRotator ViewRotation = FRotator::ZeroRotator;
			bool bViewLocationAvailable = false;
			FString ViewSource = TEXT("auto");
			Args->TryGetStringField(TEXT("view_source"), ViewSource);
			ViewSource.ToLowerInline();
			FString ActorId;
			Args->TryGetStringField(TEXT("actor"), ActorId);
			FString SequencePath;
			Args->TryGetStringField(TEXT("sequence_path"), SequencePath);
			if (ViewSource == TEXT("auto"))
			{
				if (!SequencePath.IsEmpty()) ViewSource = TEXT("sequencer");
				else if (!ActorId.IsEmpty()) ViewSource = TEXT("camera_actor");
				else if (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game || World->WorldType == EWorldType::GamePreview) ViewSource = TEXT("pie_player");
				else ViewSource = TEXT("editor_viewport");
			}
			UCameraComponent* CameraComponent = nullptr;
			if (ViewSource == TEXT("sequencer"))
			{
				double FrameNumber = 0.0;
				if (SequencePath.IsEmpty() || !Args->TryGetNumberField(TEXT("sequence_frame"), FrameNumber))
				{
					Error = TEXT("sequencer view_source requires sequence_path and integer sequence_frame.");
					return false;
				}
				TSharedRef<FJsonObject> SequenceEvaluation = SequencerCameraAtFrame(
					World, SequencePath, FMath::RoundToInt32(FrameNumber), CameraComponent, ViewLocation, ViewRotation);
				Out->SetObjectField(TEXT("sequencer_evaluation"), SequenceEvaluation);
				bViewLocationAvailable = CameraComponent != nullptr;
			}
			else if (ViewSource == TEXT("pie_player"))
			{
				double PlayerIndexNumber = 0.0;
				Args->TryGetNumberField(TEXT("player_index"), PlayerIndexNumber);
				TSharedRef<FJsonObject> PlayerCamera = PiePlayerCamera(
					World, FMath::Clamp(FMath::RoundToInt32(PlayerIndexNumber), 0, 16), CameraComponent, ViewLocation, ViewRotation, Error);
				if (!Error.IsEmpty()) return false;
				Out->SetObjectField(TEXT("pie_player_camera"), PlayerCamera);
				bViewLocationAvailable = true;
			}
			else if (ViewSource == TEXT("camera_actor"))
			{
				if (ActorId.IsEmpty())
				{
					Error = TEXT("camera_actor view_source requires actor.");
					return false;
				}
				AActor* Actor = FindActor(World, ActorId, Error);
				if (!Actor) return false;
				CameraComponent = Actor->FindComponentByClass<UCameraComponent>();
				if (!CameraComponent)
				{
					Error = FString::Printf(TEXT("Actor '%s' has no UCameraComponent."), *ActorId);
					return false;
				}
				ViewLocation = CameraComponent->GetComponentLocation();
				ViewRotation = CameraComponent->GetComponentRotation();
				bViewLocationAvailable = true;
			}
			else if (ViewSource == TEXT("editor_viewport"))
			{
				int32 ViewportIndex = INDEX_NONE;
				FLevelEditorViewportClient* Client = ActiveLevelViewport(ViewportIndex);
				if (!Client)
				{
					Error = TEXT("No active perspective level editor viewport; provide actor for camera inspection.");
					return false;
				}
				ViewLocation = Client->GetViewLocation();
				ViewRotation = Client->GetViewRotation();
				bViewLocationAvailable = true;
				Out->SetNumberField(TEXT("viewport_index"), ViewportIndex);
				Out->SetBoolField(TEXT("viewport_realtime"), Client->IsRealtime());
				Out->SetNumberField(TEXT("viewport_type"), static_cast<int32>(Client->ViewportType));
				Out->SetNumberField(TEXT("view_mode"), static_cast<int32>(Client->GetViewMode()));
				Out->SetBoolField(TEXT("exposure_fixed"), Client->ExposureSettings.bFixed);
				Out->SetNumberField(TEXT("fixed_ev100"), Client->ExposureSettings.FixedEV100);
				Out->SetStringField(TEXT("lumen_visualization"), Client->CurrentLumenVisualizationMode.ToString());
				#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
				Out->SetStringField(TEXT("megalights_visualization"), Client->CurrentMegaLightsVisualizationMode.ToString());
				Out->SetBoolField(TEXT("megalights_visualization_available"), true);
				#else
				Out->SetStringField(TEXT("megalights_visualization"), TEXT("unavailable"));
				Out->SetBoolField(TEXT("megalights_visualization_available"), false);
				#endif
				Out->SetStringField(TEXT("vsm_visualization"), Client->CurrentVirtualShadowMapVisualizationMode.ToString());
			}
			else
			{
				Error = FString::Printf(TEXT("Unsupported view_source '%s'."), *ViewSource);
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Volumes;
			if (bViewLocationAvailable)
			{
				for (TActorIterator<APostProcessVolume> It(World); It; ++It)
				{
					APostProcessVolume* Volume = *It;
					if (!Volume || !Volume->bEnabled) continue;
					float Distance = 0.0f;
					if (!Volume->bUnbound && !Volume->EncompassesPoint(ViewLocation, 0.0f, &Distance)) continue;
					TSharedRef<FJsonObject> Row = ActorRow(Volume, nullptr);
					Row->SetNumberField(TEXT("priority"), Volume->Priority);
					Row->SetNumberField(TEXT("blend_radius_cm"), Volume->BlendRadius);
					Row->SetNumberField(TEXT("blend_weight"), Volume->BlendWeight);
					Row->SetBoolField(TEXT("unbound"), Volume->bUnbound);
					Row->SetNumberField(TEXT("distance_to_volume_cm"), Distance);
					Row->SetStringField(TEXT("provenance"), TEXT("APostProcessVolume bounds/priority/weight at resolved view location"));
					Volumes.Add(MakeShared<FJsonValueObject>(Row));
				}
			}

			TArray<TSharedPtr<FJsonValue>> CandidateLights;
			if (bViewLocationAvailable)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor) continue;
					TInlineComponentArray<ULightComponentBase*> Components(Actor);
					for (ULightComponentBase* Light : Components)
					{
						if (!Light || !Light->IsVisible()) continue;
						bool bCandidate = Cast<UDirectionalLightComponent>(Light) || Cast<USkyLightComponent>(Light);
						double Distance = FVector::Distance(ViewLocation, Light->GetComponentLocation());
						if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light)) bCandidate = Distance <= Local->AttenuationRadius;
						if (!bCandidate) continue;
						TSharedRef<FJsonObject> Row = ActorRow(Actor, Light);
						Row->SetStringField(TEXT("light_type"), LightType(Light));
						Row->SetNumberField(TEXT("distance_cm"), Distance);
						Row->SetStringField(TEXT("candidate_reason"), Cast<ULocalLightComponent>(Light) ? TEXT("inside_attenuation_radius") : TEXT("global_directional_or_sky"));
						Row->SetObjectField(TEXT("source_chain"), PerLightSourceChain(Light));
						Row->SetStringField(TEXT("candidate_semantics"), TEXT("cpu_geometric_candidate_not_gpu_final_contribution"));
						CandidateLights.Add(MakeShared<FJsonValueObject>(Row));
					}
				}
			}

			Out->SetStringField(TEXT("schema"), TEXT("somol.lighting_effective_view:v2"));
			Out->SetObjectField(TEXT("world"), WorldIdentity(World, ContextName));
			Out->SetStringField(TEXT("view_source"), ViewSource);
			TSharedRef<FJsonObject> ViewSourceChain = MakeShared<FJsonObject>();
			ViewSourceChain->SetStringField(TEXT("resolved_world"), World->GetPathName());
			ViewSourceChain->SetStringField(TEXT("resolved_view_source"), ViewSource);
			ViewSourceChain->SetStringField(TEXT("view_transform_source"),
				ViewSource == TEXT("editor_viewport") ? TEXT("FLevelEditorViewportClient") :
				ViewSource == TEXT("pie_player") ? TEXT("APlayerCameraManager camera cache") :
				ViewSource == TEXT("sequencer") ? TEXT("ISequencer exact-frame evaluated binding") :
				TEXT("UCameraComponent component transform"));
			ViewSourceChain->SetObjectField(TEXT("camera_component"), FSololmcpEditorServices::MakeObjectReference(CameraComponent));
			ViewSourceChain->SetStringField(TEXT("post_process_order"), TEXT("project defaults -> camera/player POV -> ordered overlapping PostProcessVolumes -> render-thread eye adaptation"));
			ViewSourceChain->SetStringField(TEXT("lighting_order"), TEXT("project/platform/device profile -> world -> component instance -> view culling -> render-thread/GPU"));
			Out->SetObjectField(TEXT("view_source_chain"), ViewSourceChain);
			Out->SetBoolField(TEXT("view_location_available"), bViewLocationAvailable);
			if (bViewLocationAvailable)
			{
				Out->SetObjectField(TEXT("view_location_cm"), VectorJson(ViewLocation));
				Out->SetObjectField(TEXT("view_rotation_deg"), RotatorJson(ViewRotation));
			}
			else
			{
				Out->SetObjectField(TEXT("view_location"), UnavailableValue(
					TEXT("The requested source is not currently evaluated at the requested frame."),
					TEXT("view_source_resolution")));
			}
			Out->SetObjectField(TEXT("camera_component"), CameraComponent ? EditableObjectSnapshot(CameraComponent) : MakeShared<FJsonObject>());
			Out->SetArrayField(TEXT("post_process_contributors"), Volumes);
			Out->SetArrayField(TEXT("candidate_lights"), CandidateLights);
			Out->SetObjectField(TEXT("renderer"), ReadRendererState(World, ContextName));
			Out->SetBoolField(TEXT("gpu_per_pixel_exact"), false);
			Out->SetObjectField(TEXT("gpu_final_pixel_lighting"), UnavailableValue(
				TEXT("Final light-grid selection, shadow visibility, Lumen transport, eye-adaptation histogram and pixel exposure require rendered-frame GPU diagnostics/readback."),
				TEXT("render_thread_gpu")));
			Out->SetArrayField(TEXT("unsupported_fields"), {
				MakeShared<FJsonValueString>(TEXT("final eye-adaptation luminance and histogram require a rendered-frame GPU readback")),
				MakeShared<FJsonValueString>(TEXT("per-pixel shadow, light-grid and Lumen contribution require render-thread/GPU diagnostics")),
				MakeShared<FJsonValueString>(TEXT("Sequencer camera is exact only when the requested sequence is actively evaluated at the exact requested frame; the reader never seeks"))
			});
			Out->SetBoolField(TEXT("side_effect_free"), true);
			Summary = FString::Printf(TEXT("Inspected %s with %d post-process and %d candidate-light contributors."), *ViewSource, Volumes.Num(), CandidateLights.Num());
			Out = NormalizeJsonObject(Out);
			return true;
		}, nullptr, 1, GenericOutputSchema()
	});
}
} // namespace UE::SOMOLMCP
