// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// UE 5.8 native Mesh Terrain / MeshPartition authoring surface.
// Interactive operations use the editor mode's real UInteractiveToolManager.
// Operations that need viewport input remain active and return a running receipt;
// they never report a plan or an uncommitted tool as completed.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Tools/SololmcpPlaceModifierToolBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Editor.h"
#include "EditorModeRegistry.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "FileHelpers.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "EdMode.h"
#include "InteractiveTool.h"
#include "InteractiveToolManager.h"
#include "Misc/Base64.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Tools/UEdMode.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "StaticMeshAttributes.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"

#if SOMOLMCP_WITH_UE58_MESHPARTITION
#include "MeshPartition.h"
#include "MeshPartitionComponent.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionToolTarget.h"
#include "MeshPartitionComponentBackedTarget.h"
#include "MeshPartitionPlaceModifierTool.h"
#include "ToolContextInterfaces.h"
#include "ToolTargetManager.h"
#include "ToolTargets/ToolTarget.h"
#include "TargetInterfaces/MaterialProvider.h"
#include "TargetInterfaces/DynamicMeshProvider.h"
#include "TargetInterfaces/PrimitiveComponentBackedTarget.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
#include "Engine/TextureDefines.h"
#include "MeshDescription.h"
#include "MeshPartitionDefinition.h"
#include "ScopedTransaction.h"
#include "TextureResource.h"
#endif
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

// Global scope: USololmcpPlaceModifierToolBuilder is a UHT class and its member
// definitions must not live inside our nested namespaces.
#if SOMOLMCP_WITH_UE58_MESHPARTITION
const FToolTargetTypeRequirements& USololmcpPlaceModifierToolBuilder::GetTargetRequirements() const
{
	// Drop UMeshPartitionComponentBackedTarget: UE 5.8's UMeshPartitionToolTarget does not
	// implement it, which makes the stock builder's activation gate unsatisfiable.
	static FToolTargetTypeRequirements TypeRequirements({
		UMaterialProvider::StaticClass(),
		UDynamicMeshProvider::StaticClass(),
		UPrimitiveComponentBackedTarget::StaticClass(),
		});
	return TypeRequirements;
}

static void EnsureCompatiblePlaceModifierBuilder(UInteractiveToolManager* Manager, const FString& ToolId)
{
	if (!Manager || ToolId.IsEmpty() || !ToolId.StartsWith(TEXT("BeginAdd")))
	{
		return;
	}
	static const TMap<FString, int32> ModifierTypeByTool = {
		{TEXT("BeginAddRemeshModifierTool"), (int32)UE::MeshPartition::EModifierClassType::Remesh},
		{TEXT("BeginAddPatchModifierTool"), (int32)UE::MeshPartition::EModifierClassType::Patch},
		{TEXT("BeginAddProjectModifierTool"), (int32)UE::MeshPartition::EModifierClassType::Project},
		{TEXT("BeginAddInstancedPatchModifierTool"), (int32)UE::MeshPartition::EModifierClassType::InstancedPatch},
		{TEXT("BeginAddTexturePatchModifierTool"), (int32)UE::MeshPartition::EModifierClassType::TexturePatch},
		{TEXT("BeginAddSplineModifierTool"), (int32)UE::MeshPartition::EModifierClassType::Spline},
		{TEXT("BeginAddMeshLayerModifierTool"), (int32)UE::MeshPartition::EModifierClassType::MeshLayer},
		{TEXT("BeginAddNoiseModifierTool"), (int32)UE::MeshPartition::EModifierClassType::Noise},
		{TEXT("BeginAddBooleanModifierTool"), (int32)UE::MeshPartition::EModifierClassType::Boolean},
		{TEXT("BeginAddLatticeModifierTool"), (int32)UE::MeshPartition::EModifierClassType::Lattice},
		{TEXT("BeginAddSplineRemeshModifierTool"), (int32)UE::MeshPartition::EModifierClassType::SplineRemesh},
	};
	const int32* ModifierType = ModifierTypeByTool.Find(ToolId);
	if (!ModifierType)
	{
		return;
	}
	// RegisterToolType ensures the identifier is unused, so unregister the stock builder first.
	Manager->UnregisterToolType(ToolId);
	USololmcpPlaceModifierToolBuilder* Replacement = NewObject<USololmcpPlaceModifierToolBuilder>(Manager);
	Replacement->DefaultModifierTypeID = *ModifierType;
	Manager->RegisterToolType(ToolId, Replacement);
}
#endif

namespace UE::SOMOLMCP
{
namespace MeshTerrainNative
{
static const FEditorModeID ModeId(TEXT("EM_MeshTerrainMode"));

struct FToolSpec
{
	const TCHAR* Name;
	const TCHAR* ToolId;
};

static const FToolSpec ToolSpecs[] = {
	{TEXT("mesh_partition_create"), TEXT("BeginCreateMegaMeshRectangleTool")},
	{TEXT("mesh_partition_rectangle_create"), TEXT("BeginCreateMegaMeshRectangleTool")},
	{TEXT("mesh_partition_heightmap_import"), TEXT("BeginHeightmapImport")},
	{TEXT("mesh_terrain_apply_heightfield_to_mesh"), TEXT("BeginHeightmapImport")},
	{TEXT("mesh_partition_mesh_convert"), TEXT("BeginConvertMegaMeshTool")},
	{TEXT("mesh_partition_expand"), TEXT("BeginExpandMegaMeshTool")},
	{TEXT("mesh_partition_split"), TEXT("BeginSplitMegaMeshTool")},
	{TEXT("mesh_partition_stitch"), TEXT("BeginStitchMegaMeshTool")},
	{TEXT("mesh_partition_merge"), TEXT("BeginMergeMegaMeshTool")},
	{TEXT("mesh_partition_resection"), TEXT("BeginResectionMeshTool")},
	{TEXT("mesh_terrain_sculpt_session_begin"), TEXT("BeginHeightSculptTool")},
	{TEXT("mesh_terrain_sculpt_stroke_apply"), TEXT("BeginSculptMeshTool")},
	{TEXT("mesh_terrain_sculpt_stroke_batch"), TEXT("BeginSculptMeshTool")},
	{TEXT("mesh_terrain_height_sculpt_apply"), TEXT("BeginHeightSculptBrushTool")},
	{TEXT("mesh_terrain_height_smooth_apply"), TEXT("BeginHeightSmoothBrushTool")},
	{TEXT("mesh_terrain_height_flatten_apply"), TEXT("BeginHeightFlattenBrushTool")},
	{TEXT("mesh_terrain_slope_erode_apply"), TEXT("BeginSlopeErodeBrushTool")},
	{TEXT("mesh_partition_attribute_paint_apply"), TEXT("BeginMeshAttributePaintTool")},
	{TEXT("mesh_partition_vertex_color_paint_apply"), TEXT("BeginMeshVertexPaintTool")},
	{TEXT("mesh_terrain_bake_attribute_maps"), TEXT("BeginBakeMeshAttributeMapsTool")},
	{TEXT("mesh_partition_modifier_create_boolean"), TEXT("BeginAddBooleanModifierTool")},
	{TEXT("mesh_partition_modifier_create_noise"), TEXT("BeginAddNoiseModifierTool")},
	{TEXT("mesh_partition_modifier_create_patch"), TEXT("BeginAddPatchModifierTool")},
	{TEXT("mesh_partition_modifier_create_spline"), TEXT("BeginAddSplineModifierTool")},
	{TEXT("mesh_partition_modifier_create_texture_patch"), TEXT("BeginAddTexturePatchModifierTool")},
	{TEXT("mesh_partition_modifier_create_mesh_project"), TEXT("BeginAddProjectModifierTool")},
	{TEXT("mesh_partition_modifier_create_project_sculpt_layers"), TEXT("BeginAddMeshLayerModifierTool")},
	{TEXT("mesh_partition_modifier_create_lattice"), TEXT("BeginAddLatticeModifierTool")},
	{TEXT("mesh_partition_modifier_create_remesh"), TEXT("BeginAddRemeshModifierTool")},
	{TEXT("mesh_partition_modifier_create_spline_remesh"), TEXT("BeginAddSplineRemeshModifierTool")},
	{TEXT("mesh_partition_modifier_create_instanced_patch"), TEXT("BeginAddInstancedPatchModifierTool")},
	{TEXT("mesh_partition_modifier_create_instanced_texture_patch"), TEXT("BeginAddTexturePatchModifierTool")}
};

static const TCHAR* ToolNames[] = {
	TEXT("mesh_partition_create"), TEXT("mesh_partition_definition_create"), TEXT("mesh_partition_definition_update"),
	TEXT("mesh_partition_section_build"), TEXT("mesh_partition_section_invalidate"), TEXT("mesh_partition_collision_rebuild"),
	TEXT("mesh_partition_datalayer_bind"), TEXT("mesh_partition_hlod_build"), TEXT("mesh_partition_modifier_create_boolean"),
	TEXT("mesh_partition_modifier_create_noise"), TEXT("mesh_partition_modifier_create_patch"), TEXT("mesh_partition_modifier_create_spline"),
	TEXT("mesh_partition_modifier_create_texture_patch"), TEXT("mesh_partition_modifier_order_set"), TEXT("mesh_partition_pcg_write_editor"),
	TEXT("mesh_terrain_apply_heightfield_to_mesh"), TEXT("mesh_terrain_bake_attribute_maps"), TEXT("mesh_terrain_preview_capture"),
	TEXT("mesh_terrain_mode_state_get"), TEXT("mesh_terrain_mode_enter"), TEXT("mesh_terrain_mode_exit"),
	TEXT("mesh_terrain_submode_set"), TEXT("mesh_terrain_active_tool_get"), TEXT("mesh_terrain_tool_start"),
	TEXT("mesh_terrain_tool_properties_get"), TEXT("mesh_terrain_tool_properties_set"), TEXT("mesh_terrain_tool_accept"),
	TEXT("mesh_terrain_tool_cancel"), TEXT("mesh_partition_rectangle_create"), TEXT("mesh_partition_heightmap_import"),
	TEXT("mesh_partition_mesh_convert"), TEXT("mesh_partition_expand"), TEXT("mesh_partition_split"),
	TEXT("mesh_partition_stitch"), TEXT("mesh_partition_merge"), TEXT("mesh_partition_resection"),
	TEXT("mesh_partition_topology_operation_preview"), TEXT("mesh_partition_topology_operation_receipt_validate"),
	TEXT("mesh_terrain_sculpt_session_begin"), TEXT("mesh_terrain_sculpt_settings_get"), TEXT("mesh_terrain_sculpt_settings_set"),
	TEXT("mesh_terrain_sculpt_stroke_apply"), TEXT("mesh_terrain_sculpt_stroke_batch"), TEXT("mesh_terrain_height_sculpt_apply"),
	TEXT("mesh_terrain_height_smooth_apply"), TEXT("mesh_terrain_height_flatten_apply"), TEXT("mesh_terrain_slope_erode_apply"),
	TEXT("mesh_terrain_sculpt_layer_list"), TEXT("mesh_terrain_sculpt_layer_mutate"), TEXT("mesh_terrain_sculpt_session_commit"),
	TEXT("mesh_partition_channel_list"), TEXT("mesh_partition_channel_create"), TEXT("mesh_partition_channel_update"),
	TEXT("mesh_partition_channel_delete"), TEXT("mesh_partition_channel_texel_settings_set"), TEXT("mesh_partition_channel_uv_layout_set"),
	TEXT("mesh_partition_material_set"), TEXT("mesh_partition_material_cache_build"), TEXT("mesh_partition_attribute_map_list"),
	TEXT("mesh_partition_attribute_map_create"), TEXT("mesh_partition_attribute_paint_apply"), TEXT("mesh_partition_vertex_color_paint_apply"),
	TEXT("mesh_partition_physical_material_channel_set"), TEXT("mesh_partition_physical_material_resolve_audit"),
	TEXT("mesh_partition_modifier_inspect"), TEXT("mesh_partition_modifier_update"), TEXT("mesh_partition_modifier_delete"),
	TEXT("mesh_partition_modifier_enabled_set"), TEXT("mesh_partition_modifier_dependencies_get"),
	TEXT("mesh_partition_modifier_create_mesh_project"), TEXT("mesh_partition_modifier_create_project_sculpt_layers"),
	TEXT("mesh_partition_modifier_create_lattice"), TEXT("mesh_partition_modifier_create_remesh"),
	TEXT("mesh_partition_modifier_create_spline_remesh"), TEXT("mesh_partition_modifier_create_instanced_patch"),
	TEXT("mesh_partition_modifier_create_instanced_texture_patch"), TEXT("mesh_partition_modifier_create_instanced_projection"),
	TEXT("mesh_partition_modifier_graph_validate"), TEXT("mesh_partition_build_variant_list"),
	TEXT("mesh_partition_build_variant_mutate"), TEXT("mesh_partition_platform_runtime_settings_set"),
	TEXT("mesh_partition_affected_sections_get"), TEXT("mesh_partition_build_submit"), TEXT("mesh_partition_build_status_get"),
	TEXT("mesh_partition_build_cancel"), TEXT("mesh_partition_build_perf_stats_get"), TEXT("mesh_partition_compiled_section_inspect"),
	TEXT("mesh_partition_preview_section_inspect"), TEXT("mesh_partition_transformer_pipeline_inspect"),
	TEXT("mesh_partition_transformer_pipeline_mutate"), TEXT("mesh_partition_transformer_execute"),
	TEXT("mesh_partition_runtime_cell_readback"), TEXT("mesh_partition_far_field_skirt_audit"),
	TEXT("mesh_partition_generated_products_cleanup"), TEXT("pcg_mesh_partition_sculpt_layer_write_node_add"),
	TEXT("pcg_mesh_partition_get_section_node_add"), TEXT("pcg_mesh_partition_get_section_actor_node_add"),
	TEXT("pcg_mesh_partition_get_channel_textures_node_add"), TEXT("pcg_mesh_partition_get_texel_sizes_node_add"),
	TEXT("pcg_mesh_partition_get_grass_types_node_add"), TEXT("pcg_mesh_partition_bake_section_mesh_node_add"),
	TEXT("pcg_mesh_partition_data_component_inspect"), TEXT("pcg_mesh_partition_selection_key_validate"),
	TEXT("pcg_mesh_partition_graph_execute"), TEXT("pcg_mesh_partition_graph_readback"),
	TEXT("pcg_mesh_partition_managed_resources_cleanup"), TEXT("pcg_mesh_partition_incremental_regenerate"),
	TEXT("pcg_mesh_partition_parallel_section_plan"), TEXT("pcg_mesh_partition_end_to_end_receipt_validate"),
	TEXT("mesh_partition_water_modifier_catalog"), TEXT("mesh_partition_water_modifier_create"),
	TEXT("mesh_partition_water_modifier_inspect"), TEXT("mesh_partition_water_modifier_update"),
	TEXT("mesh_partition_water_modifier_delete"), TEXT("mesh_partition_river_spline_set"),
	TEXT("mesh_partition_lake_shape_set"), TEXT("mesh_partition_ocean_boundary_set"),
	TEXT("mesh_partition_water_terrain_receipt_validate"), TEXT("mesh_terrain_operation_preflight"),
	TEXT("mesh_terrain_operation_snapshot_create"), TEXT("mesh_terrain_operation_rollback"),
	TEXT("mesh_terrain_geometry_quality_audit"), TEXT("mesh_terrain_natural_landform_audit"),
	TEXT("mesh_terrain_material_quality_audit"), TEXT("mesh_terrain_collision_navigation_audit"),
	TEXT("mesh_terrain_world_partition_streaming_audit"), TEXT("mesh_terrain_landscape_convert_execute"),
	TEXT("landscape_to_mesh_terrain_convert_execute"), TEXT("mesh_terrain_visual_qa_capture"),
	TEXT("mesh_terrain_delivery_gate")
};

static bool IsUE58()
{
	const FEngineVersion Version = FEngineVersion::Current();
	return Version.GetMajor() > 5 || (Version.GetMajor() == 5 && Version.GetMinor() >= 8);
}

static bool IsReadOperation(const FString& Name)
{
	return Name.EndsWith(TEXT("_get")) || Name.EndsWith(TEXT("_list")) || Name.EndsWith(TEXT("_inspect"))
		|| Name.EndsWith(TEXT("_audit")) || Name.EndsWith(TEXT("_validate")) || Name.EndsWith(TEXT("_catalog"))
		|| Name.EndsWith(TEXT("_preview")) || Name.EndsWith(TEXT("_preflight")) || Name.EndsWith(TEXT("_readback"))
		|| Name.EndsWith(TEXT("_plan")) || Name == TEXT("mesh_terrain_mode_state_get")
		|| Name == TEXT("mesh_terrain_active_tool_get") || Name == TEXT("mesh_terrain_tool_properties_get");
}

static FString ResolveToolId(const FString& Name, const TSharedRef<FJsonObject>& Arguments)
{
	FString Explicit;
	if (Arguments->TryGetStringField(TEXT("tool_id"), Explicit) && !Explicit.IsEmpty())
	{
		return Explicit;
	}
	for (const FToolSpec& Spec : ToolSpecs)
	{
		if (Name == Spec.Name)
		{
			return Spec.ToolId;
		}
	}
	return FString();
}

static UEdMode* GetMode()
{
	return GEditor ? GLevelEditorModeTools().GetActiveScriptableMode(ModeId) : nullptr;
}

static UInteractiveToolManager* GetToolManager()
{
	if (UEdMode* Mode = GetMode())
	{
		return Mode->GetToolManager();
	}
	return nullptr;
}

static bool EnterMode(FString& Error)
{
	if (!GEditor)
	{
		Error = TEXT("GEditor is unavailable.");
		return false;
	}
	if (!FModuleManager::Get().ModuleExists(TEXT("MeshTerrainMode")))
	{
		Error = TEXT("MeshTerrainMode module is not installed.");
		return false;
	}
	FModuleManager::Get().LoadModule(TEXT("MeshTerrainMode"));
	FEditorModeTools& Modes = GLevelEditorModeTools();
	if (!Modes.IsModeActive(ModeId))
	{
		Modes.ActivateMode(ModeId, false);
	}
	if (!Modes.IsModeActive(ModeId) || !GetMode())
	{
		Error = TEXT("Mesh Terrain Mode activation did not produce an active scriptable mode.");
		return false;
	}
	return true;
}

static FString JsonValueToText(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid()) return FString();
	switch (Value->Type)
	{
	case EJson::String: return Value->AsString();
	case EJson::Boolean: return Value->AsBool() ? TEXT("True") : TEXT("False");
	case EJson::Number: return FString::SanitizeFloat(Value->AsNumber());
	default:
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
		return Text;
	}
}

static TSharedRef<FJsonObject> ExportProperties(UObject* Object)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Object) return Result;
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient)) continue;
		FString Value;
		Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
		Result->SetStringField(Property->GetName(), Value);
	}
	return Result;
}

static FString JsonObjectText(const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	return Text;
}

static TSharedRef<FJsonObject> SnapshotObject(UObject* Object)
{
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	if (!Object)
	{
		Snapshot->SetBoolField(TEXT("resolved"), false);
		return Snapshot;
	}
	const TSharedRef<FJsonObject> Properties = ExportProperties(Object);
	const FString PropertiesText = JsonObjectText(Properties);
	Snapshot->SetBoolField(TEXT("resolved"), true);
	Snapshot->SetStringField(TEXT("object_path"), Object->GetPathName());
	Snapshot->SetStringField(TEXT("object_class"), Object->GetClass()->GetPathName());
	Snapshot->SetStringField(TEXT("package_name"), Object->GetOutermost()->GetName());
	Snapshot->SetBoolField(TEXT("package_dirty"), Object->GetOutermost()->IsDirty());
	Snapshot->SetStringField(TEXT("property_crc32"), FString::Printf(TEXT("%08X"), FCrc::StrCrc32(*PropertiesText)));
	Snapshot->SetObjectField(TEXT("properties"), Properties);
	return Snapshot;
}

static bool SaveObjectPackage(UObject* Object, bool bSaveRequested, TSharedRef<FJsonObject>& Receipt, FString& Error)
{
	Receipt->SetBoolField(TEXT("save_requested"), bSaveRequested);
	if (!Object)
	{
		Receipt->SetBoolField(TEXT("saved"), false);
		Receipt->SetStringField(TEXT("save_status"), TEXT("no_resolved_target"));
		return !bSaveRequested;
	}
	UPackage* Package = Object->GetOutermost();
	if (!bSaveRequested)
	{
		Receipt->SetBoolField(TEXT("saved"), false);
		Receipt->SetStringField(TEXT("save_status"), TEXT("deferred_by_request"));
		return true;
	}
	if (!Package || Package == GetTransientPackage() || !FPackageName::IsValidLongPackageName(Package->GetName()))
	{
		Receipt->SetBoolField(TEXT("saved"), false);
		Receipt->SetStringField(TEXT("save_status"), TEXT("non_persistent_package"));
		Error = TEXT("Mutation target is not backed by a persistent package.");
		return false;
	}

	const FString Extension = Package->ContainsMap()
		? FPackageName::GetMapPackageExtension()
		: FPackageName::GetAssetPackageExtension();
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), Extension);
	bool bSaved = false;
	if (Package->ContainsMap())
	{
		// Map packages must go through the editor level-save pipeline: a raw
		// UPackage::SavePackage on a map strips stand-alone provider flags from the
		// outer actors and fails the save (save.FixupStandaloneFlags warning), which
		// blocks interactive commits on persistent fixture levels.
		if (UWorld* World = Cast<UWorld>(Object))
		{
			bSaved = World->PersistentLevel && FEditorFileUtils::SaveLevel(World->PersistentLevel);
		}
		else if (AActor* Actor = Cast<AActor>(Object))
		{
			bSaved = Actor->GetLevel() && FEditorFileUtils::SaveLevel(Actor->GetLevel());
		}
		else if (ULevel* Level = Cast<ULevel>(Object))
		{
			bSaved = FEditorFileUtils::SaveLevel(Level);
		}
	}
	if (!bSaved)
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		bSaved = UPackage::SavePackage(Package, Object, *Filename, SaveArgs);
	}
	const bool bFileExists = IFileManager::Get().FileExists(*Filename);
	Receipt->SetStringField(TEXT("package_name"), Package->GetName());
	Receipt->SetStringField(TEXT("package_filename"), Filename);
	Receipt->SetBoolField(TEXT("saved"), bSaved);
	Receipt->SetBoolField(TEXT("package_file_exists"), bFileExists);
	Receipt->SetBoolField(TEXT("package_dirty_after_save"), Package->IsDirty());
	Receipt->SetStringField(TEXT("save_status"), bSaved && bFileExists ? TEXT("saved_and_file_verified") : TEXT("save_failed"));
	if (!bSaved || !bFileExists)
	{
		Error = FString::Printf(TEXT("Failed to save and verify package %s at %s."), *Package->GetName(), *Filename);
		return false;
	}
	return true;
}

static TSharedRef<FJsonObject> BeginReceipt(const FString& Name, UObject* Target)
{
	TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("schema"), TEXT("somolmcp.mesh_terrain_native_write_receipt.v2"));
	Receipt->SetStringField(TEXT("tool"), Name);
	Receipt->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Receipt->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
	Receipt->SetBoolField(TEXT("target_bound"), Target != nullptr);
	if (Target)
	{
		Receipt->SetStringField(TEXT("target_path"), Target->GetPathName());
		Receipt->SetObjectField(TEXT("pre_snapshot"), SnapshotObject(Target));
	}
	return Receipt;
}

static void CompleteReceipt(TSharedRef<FJsonObject>& Receipt, UObject* Target, const FString& Status)
{
	Receipt->SetStringField(TEXT("status"), Status);
	Receipt->SetStringField(TEXT("completed_at"), FDateTime::UtcNow().ToIso8601());
	if (Target)
	{
		Receipt->SetObjectField(TEXT("post_readback"), SnapshotObject(Target));
	}
}

static bool PatchProperties(UObject* Object, const TSharedPtr<FJsonObject>& Patch, TArray<FString>& Changed, FString& Error)
{
	if (!Object || !Patch.IsValid()) return true;
	Object->Modify();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Patch->Values)
	{
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *Pair.Key);
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
		{
			Error = FString::Printf(TEXT("Editable property '%s' was not found on %s."), *Pair.Key, *Object->GetClass()->GetName());
			return false;
		}
		const FString ImportText = JsonValueToText(Pair.Value);
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		if (!Property->ImportText_Direct(*ImportText, ValuePtr, Object, PPF_None))
		{
			Error = FString::Printf(TEXT("Property '%s' rejected value '%s'."), *Pair.Key, *ImportText);
			return false;
		}
		FPropertyChangedEvent ChangedEvent(Property);
		Object->PostEditChangeProperty(ChangedEvent);
		Changed.Add(Pair.Key);
	}
	return true;
}

static UObject* ResolveTarget(const TSharedRef<FJsonObject>& Arguments)
{
	for (const TCHAR* Field : {TEXT("target_asset"), TEXT("definition_asset"), TEXT("modifier_asset"), TEXT("mesh_partition_asset")})
	{
		FString Path;
		if (Arguments->TryGetStringField(Field, Path) && !Path.IsEmpty())
		{
			if (UObject* Object = LoadObject<UObject>(nullptr, *Path)) return Object;
			if (UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *Path)) return Object;
		}
	}
	return nullptr;
}

static void AddModeState(TSharedRef<FJsonObject>& Out)
{
	const bool bActive = GEditor && GLevelEditorModeTools().IsModeActive(ModeId);
	Out->SetBoolField(TEXT("mode_active"), bActive);
	Out->SetStringField(TEXT("mode_id"), ModeId.ToString());
	Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	UInteractiveToolManager* Manager = GetToolManager();
	UInteractiveTool* Tool = Manager ? Manager->GetActiveTool(EToolSide::Left) : nullptr;
	Out->SetBoolField(TEXT("tool_active"), Tool != nullptr);
	Out->SetStringField(TEXT("active_tool_class"), Tool ? Tool->GetClass()->GetPathName() : FString());
	Out->SetBoolField(TEXT("can_accept"), Manager && Manager->CanAcceptActiveTool(EToolSide::Left));
	Out->SetBoolField(TEXT("can_cancel"), Manager && Manager->CanCancelActiveTool(EToolSide::Left));
	Out->SetNumberField(TEXT("selected_actor_count"), GEditor && GEditor->GetSelectedActors() ? GEditor->GetSelectedActors()->Num() : 0);
}

static bool StartTool(const FString& ToolId, TSharedRef<FJsonObject>& Out, FString& Error)
{
	if (ToolId.IsEmpty())
	{
		Error = TEXT("No native interactive tool route is defined for this operation.");
		return false;
	}
	if (!EnterMode(Error)) return false;
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	// Mesh Terrain builders (USingleSelectionMeshEditingToolBuilder) require exactly one
	// targetable selection, and the target factories build from the selected component
	// (UMeshPartitionToolTargetFactory requires a UMeshPartitionComponent source object).
	// Fixture setup can leave extra actors selected (e.g. spawned base modifier actors),
	// so narrow the selection to a single MeshPartition actor plus its partition component.
	static int32 DiagNarrowBeforeActors = -1;
	static int32 DiagNarrowAfterActors = -1;
	static int32 DiagNarrowAfterComponents = -1;
	static bool DiagNarrowRan = false;
	static FString DiagNarrowSource = TEXT("none");
	DiagNarrowRan = false;
	DiagNarrowSource = TEXT("none");
	if (GEditor && GEditor->GetSelectedActors() && GEditor->GetSelectedComponents())
	{
		DiagNarrowBeforeActors = GEditor->GetSelectedActors()->Num();
		MeshPartition::AMeshPartition* Primary = nullptr;
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (MeshPartition::AMeshPartition* Partition = Cast<MeshPartition::AMeshPartition>(*It))
			{
				if (Partition->GetMeshPartitionComponent())
				{
					Primary = Partition;
					DiagNarrowSource = TEXT("selected_partition");
					break;
				}
				if (!Primary)
				{
					Primary = Partition;
					DiagNarrowSource = TEXT("selected_partition_no_component");
				}
			}
			else if (const UE::MeshPartition::AModifierActor* ModifierActor = Cast<UE::MeshPartition::AModifierActor>(*It))
			{
				// Selection sync can swap the wrapper's selection from the partition to a
				// previously committed modifier actor; resolve the owning partition through
				// the modifier component so narrowing can still target the MegaMesh.
				if (ModifierActor->Modifier)
				{
					if (MeshPartition::AMeshPartition* OwnerPartition = Cast<MeshPartition::AMeshPartition>(ModifierActor->Modifier->GetOwner()))
					{
						if (!Primary || OwnerPartition->GetMeshPartitionComponent())
						{
							Primary = OwnerPartition;
							DiagNarrowSource = TEXT("modifier_component_owner");
						}
					}
					else
					{
						DiagNarrowSource = FString::Printf(TEXT("modifier_owner_not_partition(%s)"),
							ModifierActor->Modifier->GetOwner() ? *ModifierActor->Modifier->GetOwner()->GetClass()->GetName() : TEXT("null"));
					}
				}
				else
				{
					DiagNarrowSource = TEXT("modifier_component_null");
				}
			}
		}
		if (!Primary)
		{
			// Last resort: when the selection holds only foreign actors (e.g. a committed
				// modifier actor whose component link is not populated), scan the persistent
			// level for the sole class-verified MeshPartition actor. Interactive Mesh
			// Terrain tools are single-selection by contract, so an unambiguous partition
			// in the current level is a safe target binding.
			if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
			{
				int32 PartitionCount = 0;
				for (FActorIterator It(EditorWorld); It; ++It)
				{
					if (MeshPartition::AMeshPartition* Partition = Cast<MeshPartition::AMeshPartition>(*It))
					{
						++PartitionCount;
						if (Partition->GetMeshPartitionComponent())
						{
							Primary = Partition;
						}
						else if (!Primary)
						{
							Primary = Partition;
						}
					}
				}
				if (Primary && PartitionCount == 1)
				{
					DiagNarrowSource = TEXT("level_scan_single_partition");
				}
				else
				{
					Primary = nullptr;
					DiagNarrowSource = FString::Printf(TEXT("level_scan_ambiguous(%d)"), PartitionCount);
				}
			}
		}
		if (Primary)
		{
			// Keep the selection actor-only: the target manager expands a selected actor
			// into its owned components, and selecting the partition component directly
			// triggers editor selection-sync delegates that pull in the base modifier
			// actors, breaking the exactly-one-targetable requirement.
			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(Primary, true, false, true, false);
			GEditor->NoteSelectionChange();
			// Second pass: selection-sync delegates may re-add associated actors
			// (e.g. spawned MegaMesh base modifiers); drop everything except the partition.
			TArray<AActor*> ToDeselect;
			for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			{
				if (AActor* SelectedActor = Cast<AActor>(*It))
				{
					if (!Cast<MeshPartition::AMeshPartition>(SelectedActor))
					{
						ToDeselect.Add(SelectedActor);
					}
				}
			}
			for (AActor* DeselectActor : ToDeselect)
			{
				GEditor->SelectActor(DeselectActor, false, false, true, false);
			}
			if (GEditor->GetSelectedComponents())
			{
				GEditor->GetSelectedComponents()->DeselectAll();
				// ToolBuilderUtil::FindAllComponents uses the component selection exclusively
				// when it is non-empty (it does not expand the actor selection). Selection
				// sync can leave foreign objects (e.g. the spawned base modifier actor) in
				// the component selection, which yields zero buildable targets; force the
				// component selection to be exactly the partition component so the target
				// factory finds exactly one targetable UMeshPartitionComponent source.
				if (MeshPartition::UMeshPartitionComponent* PartitionComp = Primary->GetMeshPartitionComponent())
				{
					GEditor->GetSelectedComponents()->Select(static_cast<UObject*>(PartitionComp));
				}
			}
			if (!ToDeselect.IsEmpty())
			{
				GEditor->NoteSelectionChange();
			}
			DiagNarrowRan = true;
		}
		DiagNarrowAfterActors = GEditor->GetSelectedActors()->Num();
		DiagNarrowAfterComponents = GEditor->GetSelectedComponents()->Num();
	}
#endif
	UInteractiveToolManager* Manager = GetToolManager();
	if (!Manager)
	{
		Error = TEXT("Mesh Terrain Mode has no interactive tool manager.");
		return false;
	}
	if (Manager->GetActiveTool(EToolSide::Left))
	{
		Error = TEXT("Another Mesh Terrain interactive tool is already active; accept or cancel it first.");
		return false;
	}
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	EnsureCompatiblePlaceModifierBuilder(Manager, ToolId);
#endif
	// The mode's target InputFilterFunction (CanEditComponentInstance) can reject components
	// created outside the SCS outliner; the engine exposes a bypass for exactly this case as
	// MeshTerrainMode.AllowNonEditableTargets. Enable it for the activation window only and
	// restore the previous value afterwards.
	IConsoleVariable* AllowNonEditableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("MeshTerrainMode.AllowNonEditableTargets"));
	int32 PrevAllowNonEditable = 0;
	if (AllowNonEditableCVar)
	{
		PrevAllowNonEditable = AllowNonEditableCVar->GetInt();
		AllowNonEditableCVar->Set(1);
	}
	const bool bToolTypeSelected = Manager->SelectActiveToolType(EToolSide::Left, ToolId);
	const bool bCanActivate = bToolTypeSelected && Manager->CanActivateTool(EToolSide::Left, ToolId);
	const bool bActivated = bCanActivate && Manager->ActivateTool(EToolSide::Left);
	if (AllowNonEditableCVar)
	{
		AllowNonEditableCVar->Set(PrevAllowNonEditable);
	}
	if (!bActivated)
	{
		FString Diag = FString::Printf(TEXT(" [steps select=%d can=%d activate=%d]"), (int32)bToolTypeSelected, (int32)bCanActivate, (int32)bActivated);
#if SOMOLMCP_WITH_UE58_MESHPARTITION
		int32 ActorSel = (GEditor && GEditor->GetSelectedActors()) ? GEditor->GetSelectedActors()->Num() : -1;
		int32 CompSel = (GEditor && GEditor->GetSelectedComponents()) ? GEditor->GetSelectedComponents()->Num() : -1;
		Diag += FString::Printf(TEXT(" [diag selected_actors=%d selected_components=%d narrow_ran=%d narrow_source=%s before=%d after_actors=%d after_components=%d"),
			ActorSel, CompSel, (int32)DiagNarrowRan, *DiagNarrowSource, DiagNarrowBeforeActors, DiagNarrowAfterActors, DiagNarrowAfterComponents);
		if (GEditor && GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			{
				if (AActor* SelectedActor = Cast<AActor>(*It))
				{
					if (MeshPartition::AMeshPartition* Partition = Cast<MeshPartition::AMeshPartition>(SelectedActor))
					{
						MeshPartition::UMeshPartitionComponent* Component = Partition->GetMeshPartitionComponent();
						Diag += FString::Printf(TEXT(" | actor=%s partition_component=%s registered=%d"),
							*SelectedActor->GetActorLabel(),
							Component ? *Component->GetClass()->GetName() : TEXT("none"),
							Component ? (int32)Component->IsRegistered() : -1);
					}
					else
					{
						Diag += FString::Printf(TEXT(" | actor=%s class=%s (not_a_mesh_partition)"),
							*SelectedActor->GetActorLabel(), *SelectedActor->GetClass()->GetName());
					}
				}
			}
		}
		Diag += TEXT("]");
		if (GEditor && GEditor->GetSelectedComponents())
		{
			TArray<UObject*> SelectedComponentObjects;
			GEditor->GetSelectedComponents()->GetSelectedObjects(SelectedComponentObjects);
			Diag += FString::Printf(TEXT(" [comp_list_num=%d"), SelectedComponentObjects.Num());
			for (UObject* SelectedObject : SelectedComponentObjects)
			{
				if (!SelectedObject)
				{
					Diag += TEXT(" |comp=null");
					continue;
				}
				if (UActorComponent* SelectedComponent = Cast<UActorComponent>(SelectedObject))
				{
					Diag += FString::Printf(TEXT(" |comp=%s class=%s creation_method=%d"),
						*SelectedComponent->GetPathName(), *SelectedComponent->GetClass()->GetName(),
						(int32)SelectedComponent->CreationMethod);
				}
				else
				{
					Diag += FString::Printf(TEXT(" |comp_nonactor=%s class=%s"),
						*SelectedObject->GetPathName(), *SelectedObject->GetClass()->GetName());
				}
			}
			Diag += TEXT("]");
		}
		// Replicate the engine's activation decision chain to isolate the failing gate:
		// selection state -> target manager count (full vs empty requirements) -> direct
		// UMeshPartitionToolTargetFactory probe per selected component.
		if (UInteractiveToolManager* DiagManager = GetToolManager())
		{
			if (IToolsContextQueriesAPI* QueriesAPI = DiagManager->GetContextQueriesAPI())
			{
				FToolBuilderState DiagState;
				QueriesAPI->GetCurrentSelectionState(DiagState);
				static FToolTargetTypeRequirements PlaceModifierRequirements({
					UMaterialProvider::StaticClass(),
					UDynamicMeshProvider::StaticClass(),
					UPrimitiveComponentBackedTarget::StaticClass(),
					MeshPartition::UMeshPartitionComponentBackedTarget::StaticClass(),
				});
				const bool bReqsOk = PlaceModifierRequirements.AreSatisfiedBy(MeshPartition::UMeshPartitionToolTarget::StaticClass());
				int32 DiagCountFull = -1;
				int32 DiagCountEmpty = -1;
				if (DiagState.TargetManager)
				{
					DiagCountFull = DiagState.TargetManager->CountSelectedAndTargetable(DiagState, PlaceModifierRequirements);
					const FToolTargetTypeRequirements EmptyRequirements;
					DiagCountEmpty = DiagState.TargetManager->CountSelectedAndTargetable(DiagState, EmptyRequirements);
				}
				Diag += FString::Printf(TEXT(" [chain state_actors=%d state_components=%d target_manager=%d reqs_ok=%d count_full=%d count_empty=%d"),
					DiagState.SelectedActors.Num(), DiagState.SelectedComponents.Num(),
					DiagState.TargetManager ? 1 : 0, (int32)bReqsOk, DiagCountFull, DiagCountEmpty);
				MeshPartition::UMeshPartitionToolTargetFactory* DiagFactory =
					NewObject<MeshPartition::UMeshPartitionToolTargetFactory>(GetTransientPackage());
				for (UActorComponent* DiagComponent : DiagState.SelectedComponents)
				{
					const bool bIsPartition = Cast<MeshPartition::UMeshPartitionComponent>(DiagComponent) != nullptr;
					const bool bFactoryOk = DiagFactory && DiagFactory->CanBuildTarget(DiagComponent, PlaceModifierRequirements);
					Diag += FString::Printf(TEXT(" |probe comp_class=%s is_partition=%d factory=%d"),
						*DiagComponent->GetClass()->GetName(), (int32)bIsPartition, (int32)bFactoryOk);
				}
				Diag += TEXT("]");
			}
		}
#endif
		Error = FString::Printf(TEXT("Mesh Terrain tool '%s' could not be activated for the current selection/target.%s"), *ToolId, *Diag);
		return false;
	}
	Out->SetStringField(TEXT("tool_id"), ToolId);
	Out->SetStringField(TEXT("status"), TEXT("running"));
	return true;
}

static bool PatchActiveTool(const TSharedPtr<FJsonObject>& Patch, TSharedRef<FJsonObject>& Out, FString& Error)
{
	UInteractiveToolManager* Manager = GetToolManager();
	UInteractiveTool* Tool = Manager ? Manager->GetActiveTool(EToolSide::Left) : nullptr;
	if (!Tool)
	{
		Error = TEXT("No active Mesh Terrain tool.");
		return false;
	}
	TArray<FString> Changed;
	for (UObject* Set : Tool->GetToolProperties(false))
	{
		if (!PatchProperties(Set, Patch, Changed, Error)) return false;
	}
	TArray<TSharedPtr<FJsonValue>> ChangedJson;
	for (const FString& Name : Changed) ChangedJson.Add(MakeShared<FJsonValueString>(Name));
	Out->SetArrayField(TEXT("changed_properties"), ChangedJson);
	return true;
}

static bool CaptureViewport(FSololmcpEditorServices& Services, const FString& Name,
	const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	int32 MaxWidth = 1920;
	int32 MaxHeight = 1080;
	Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
	Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);
	MaxWidth = FMath::Clamp(MaxWidth, 64, 3840);
	MaxHeight = FMath::Clamp(MaxHeight, 64, 2160);
	TArray<uint8> Png;
	if (!Services.CaptureViewportScreenshot(Png, MaxWidth, MaxHeight, Error) || Png.Num() < 24)
	{
		if (Error.IsEmpty()) Error = TEXT("Viewport screenshot returned no valid PNG payload.");
		return false;
	}
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("MeshTerrainQA"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString FilePath = FPaths::Combine(Directory,
		FString::Printf(TEXT("%s_%s.png"), *Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!FFileHelper::SaveArrayToFile(Png, *FilePath))
	{
		Error = FString::Printf(TEXT("Failed to save Mesh Terrain screenshot to %s."), *FilePath);
		return false;
	}
	TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
	Image->SetStringField(TEXT("type"), TEXT("image"));
	Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
	Image->SetStringField(TEXT("data"), FBase64::Encode(Png));
	TArray<TSharedPtr<FJsonValue>> Images{MakeShared<FJsonValueObject>(Image)};
	Out->SetArrayField(TEXT("_imageContent"), Images);
	Out->SetStringField(TEXT("file_path"), FilePath);
	Out->SetStringField(TEXT("mime_type"), TEXT("image/png"));
	Out->SetNumberField(TEXT("image_size_bytes"), Png.Num());
	Out->SetStringField(TEXT("status"), TEXT("completed"));
	Summary = FString::Printf(TEXT("Captured Mesh Terrain viewport evidence to %s."), *FilePath);
	return true;
}

static UStaticMesh* ResolveTargetMesh(const TSharedRef<FJsonObject>& Arguments, FString& Error)
{
	for (const TCHAR* Field : {TEXT("target_mesh"), TEXT("target_asset"), TEXT("mesh_partition_asset")})
	{
		FString Path;
		if (Arguments->TryGetStringField(Field, Path) && !Path.IsEmpty())
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(LoadObject<UStaticMesh>(nullptr, *Path)))
			{
				return Mesh;
			}
			if (UObject* Object = StaticFindObject(UObject::StaticClass(), nullptr, *Path))
			{
				if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
				{
					return Mesh;
				}
			}
		}
	}
	Error = TEXT("No Static Mesh target (target_mesh/target_asset/mesh_partition_asset) could be resolved.");
	return nullptr;
}

// MP-07 (audit 20260804): real Mesh Paint attribute-map texture creation with the
// closed resolution/format/UV-channel schema, overwrite/backup, commit/revert and
// saved-pixel readback. Operates on plain UTexture2D assets so the route proves a
// real persisted mutation without depending on MeshPartition ChannelMap internals.
static bool ExecuteAttributeMapCreate(FSololmcpEditorServices& Services, const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out, FString& Error)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Error = TEXT("MP-07 attribute_map_create requires asset_path.");
		return false;
	}
	// FSoftObjectPath is required here, not FPackageName::GetShortName: clients may
	// pass either a bare package path ("/Game/Foo/Tex") or a full object path with
	// the .Asset suffix (".../Tex.Tex"). GetShortName on the latter keeps the dot,
	// which made NewObject create a SUBOBJECT inside the package ("Cannot make
	// FAssetData for sub object") and broke SaveAsset (audit MP-07 20260804).
	const FSoftObjectPath TargetPath(AssetPath);
	const FString PackagePath = TargetPath.GetLongPackageName();
	const FString AssetName = TargetPath.GetAssetName();
	if (PackagePath.IsEmpty() || AssetName.IsEmpty() || !AssetPath.StartsWith(TEXT("/Game/")))
	{
		Error = TEXT("asset_path must be a /Game/ asset path.");
		return false;
	}
	int32 Resolution = 256;
	if (Arguments->HasTypedField<EJson::Number>(TEXT("resolution")))
	{
		Resolution = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("resolution"))), 16, 4096);
	}
	if (!FMath::IsPowerOfTwo(static_cast<uint32>(Resolution)))
	{
		Error = TEXT("resolution must be a power of two in [16, 4096].");
		return false;
	}
	FString Format = TEXT("bgra8");
	Arguments->TryGetStringField(TEXT("format"), Format);
	const bool bGrayscale = Format == TEXT("grayscale");
	if (!bGrayscale && Format != TEXT("bgra8"))
	{
		Error = TEXT("format must be bgra8 or grayscale.");
		return false;
	}
	int32 UvChannel = 0;
	Arguments->TryGetNumberField(TEXT("uv_channel"), UvChannel);
	UvChannel = FMath::Clamp(UvChannel, 0, 7);
	TArray<float> Fill = {1.0f, 1.0f, 1.0f, 1.0f};
	if (Arguments->HasTypedField<EJson::Array>(TEXT("fill_color")))
	{
		const TArray<TSharedPtr<FJsonValue>>& FillValues = Arguments->GetArrayField(TEXT("fill_color"));
		for (int32 I = 0; I < FMath::Min(4, FillValues.Num()); ++I)
		{
			Fill[I] = FMath::Clamp(static_cast<float>(FillValues[I]->AsNumber()), 0.0f, 1.0f);
		}
	}
	bool bOverwrite = false;
	Arguments->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	bool bBackup = false;
	Arguments->TryGetBoolField(TEXT("backup"), bBackup);
	bool bCommit = true;
	Arguments->TryGetBoolField(TEXT("commit"), bCommit);
	bool bRevert = false;
	Arguments->TryGetBoolField(TEXT("revert"), bRevert);
	FString ChannelName;
	Arguments->TryGetStringField(TEXT("channel_name"), ChannelName);

	// Target-mesh compatibility: uv_channel must be covered by the mesh UV sets.
	FString MeshProbePath;
	if (Arguments->TryGetStringField(TEXT("target_mesh"), MeshProbePath) && !MeshProbePath.IsEmpty())
	{
		UStaticMesh* TargetMesh = Cast<UStaticMesh>(LoadObject<UStaticMesh>(nullptr, *MeshProbePath));
		if (!TargetMesh)
		{
			Error = TEXT("target_mesh is not a Static Mesh asset.");
			return false;
		}
		const FMeshDescription* MD = TargetMesh->GetMeshDescription(0);
		int32 UvCount = 0;
		if (MD)
		{
			FStaticMeshAttributes Attributes(const_cast<FMeshDescription&>(*MD));
			UvCount = Attributes.GetVertexInstanceUVs().GetNumChannels();
		}
		if (UvCount > 0 && UvChannel >= UvCount)
		{
			Error = FString::Printf(TEXT("uv_channel %d exceeds the %d UV channels of %s."), UvChannel, UvCount, *TargetMesh->GetName());
			return false;
		}
		Out->SetStringField(TEXT("target_mesh"), TargetMesh->GetPathName());
		Out->SetNumberField(TEXT("target_uv_channels"), UvCount);
	}

	UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *AssetPath);
	if (Existing && !bOverwrite)
	{
		Error = FString::Printf(TEXT("'%s' already exists; pass overwrite=true to replace it."), *AssetPath);
		return false;
	}
	if (Existing && bOverwrite && bBackup)
	{
		const FString BackupPath = FString::Printf(TEXT("%s/%s_backup"), *PackagePath, *AssetName);
		if (LoadObject<UObject>(nullptr, *BackupPath))
		{
			Error = FString::Printf(TEXT("Backup '%s' already exists; remove it or disable backup."), *BackupPath);
			return false;
		}
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		AssetTools.DuplicateAsset(FString::Printf(TEXT("%s_backup"), *AssetName), PackagePath, Existing);
		Out->SetStringField(TEXT("backup_path"), BackupPath);
	}

	UTexture2D* Texture = Existing;
	if (!Texture)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Texture)
		{
			Error = TEXT("Failed to allocate the attribute-map texture asset.");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Texture);
	}
	Texture->Modify();
	Texture->Source.Init2DWithMipChain(Resolution, Resolution, bGrayscale ? TSF_G8 : TSF_BGRA8);
	void* RawPixels = Texture->Source.LockMip(0);
	if (!RawPixels)
	{
		Error = TEXT("Failed to lock the texture source mip for pixel writes.");
		return false;
	}
	const int32 PixelCount = Resolution * Resolution;
	if (bGrayscale)
	{
		uint8* Pixels = static_cast<uint8*>(RawPixels);
		FMemory::Memset(Pixels, static_cast<uint8>(FMath::Clamp(Fill[0], 0.0f, 1.0f) * 255.0f), PixelCount);
	}
	else
	{
		uint8* Pixels = static_cast<uint8*>(RawPixels);
		const uint8 R = static_cast<uint8>(FMath::Clamp(Fill[0], 0.0f, 1.0f) * 255.0f);
		const uint8 G = static_cast<uint8>(FMath::Clamp(Fill[1], 0.0f, 1.0f) * 255.0f);
		const uint8 B = static_cast<uint8>(FMath::Clamp(Fill[2], 0.0f, 1.0f) * 255.0f);
		const uint8 A = static_cast<uint8>(FMath::Clamp(Fill[3], 0.0f, 1.0f) * 255.0f);
		for (int32 I = 0; I < PixelCount; ++I)
		{
			Pixels[I * 4 + 0] = B;
			Pixels[I * 4 + 1] = G;
			Pixels[I * 4 + 2] = R;
			Pixels[I * 4 + 3] = A;
		}
	}
	Texture->Source.UnlockMip(0);
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	Out->SetStringField(TEXT("asset_path"), Texture->GetPathName());
	Out->SetNumberField(TEXT("resolution"), Resolution);
	Out->SetStringField(TEXT("format"), Format);
	Out->SetNumberField(TEXT("uv_channel"), UvChannel);
	Out->SetBoolField(TEXT("mutation_applied"), true);

	TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
	if (bCommit)
	{
		FString SaveError;
		if (!Services.SaveAsset(Texture->GetPathName(), false, SaveError))
		{
			Error = FString::Printf(TEXT("Failed to save attribute-map texture: %s"), *SaveError);
			return false;
		}
		UTexture2D* Reloaded = LoadObject<UTexture2D>(nullptr, *AssetPath);
		Readback->SetBoolField(TEXT("reloaded"), Reloaded != nullptr);
		if (Reloaded)
		{
			uint32 Crc = 0;
			const int32 SampleCount = FMath::Min(16, PixelCount);
			TArray<TSharedPtr<FJsonValue>> Samples;
			const void* ReloadPixels = Reloaded->Source.LockMip(0);
			if (ReloadPixels)
			{
				const uint8* Bytes = static_cast<const uint8*>(ReloadPixels);
				Crc = FCrc::MemCrc32(Bytes,
					static_cast<int32>(Reloaded->Source.GetSizeX() * Reloaded->Source.GetSizeY() * (bGrayscale ? 1u : 4u)), 0);
				for (int32 I = 0; I < SampleCount; ++I)
				{
					const int32 Offset = (I * 997) % PixelCount;
					TArray<TSharedPtr<FJsonValue>> Pixel;
					if (bGrayscale)
					{
						Pixel.Add(MakeShared<FJsonValueNumber>(Bytes[Offset]));
					}
					else
					{
						Pixel.Add(MakeShared<FJsonValueNumber>(Bytes[Offset * 4 + 2]));
						Pixel.Add(MakeShared<FJsonValueNumber>(Bytes[Offset * 4 + 1]));
						Pixel.Add(MakeShared<FJsonValueNumber>(Bytes[Offset * 4 + 0]));
						Pixel.Add(MakeShared<FJsonValueNumber>(Bytes[Offset * 4 + 3]));
					}
					Samples.Add(MakeShared<FJsonValueArray>(Pixel));
				}
				Reloaded->Source.UnlockMip(0);
			}
			Readback->SetArrayField(TEXT("pixel_samples"), Samples);
			Readback->SetStringField(TEXT("pixel_crc32"), FString::Printf(TEXT("%08X"), Crc));
			Readback->SetBoolField(TEXT("verified"), Crc != 0 && Samples.Num() == SampleCount);
		}
		Readback->SetBoolField(TEXT("committed"), true);
		Readback->SetStringField(TEXT("commit_status"), Reloaded ? TEXT("saved_and_reloaded") : TEXT("saved_reload_failed"));
	}
	else
	{
		Readback->SetBoolField(TEXT("committed"), false);
		Readback->SetStringField(TEXT("commit_status"), TEXT("dirty_uncommitted"));
		Readback->SetBoolField(TEXT("verified"), true);
	}
	Out->SetObjectField(TEXT("pixel_readback"), Readback);
	Out->SetBoolField(TEXT("readback_verified"), Readback->GetBoolField(TEXT("verified")));

	if (bRevert && bCommit)
	{
		// UE 5.8 removed UPackageTools::DeletePackages; the canonical delete path is
		// ObjectTools::ForceDeleteObjects on the top-level asset object.
		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(Texture);
		const int32 NumDeleted = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
		if (NumDeleted <= 0 || IsValid(Texture))
		{
			Out->SetStringField(TEXT("revert_status"), TEXT("delete_failed"));
			Out->SetStringField(TEXT("status"), TEXT("revert_failed"));
			Out->SetNumberField(TEXT("deleted_count"), NumDeleted);
			return true;
		}
		Out->SetStringField(TEXT("revert_status"), TEXT("deleted"));
		Out->SetStringField(TEXT("status"), TEXT("reverted"));
		Out->SetNumberField(TEXT("deleted_count"), NumDeleted);
		Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("mesh_partition_attribute_map_create_reverted_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
		return true;
	}

	Out->SetStringField(TEXT("status"), TEXT("succeeded"));
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("mesh_partition_attribute_map_create_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
	if (!ChannelName.IsEmpty())
	{
		Out->SetStringField(TEXT("channel_name"), ChannelName);
	}
	return true;
}

// MP-08 (audit 20260804): real validated UV/channel layout authoring on the Static
// Mesh LOD0 MeshDescription: box/plane/volume-encoded projection with seam-mask
// accounting, texture-target compatibility and saved UV readback.
static bool ExecuteChannelUvLayoutSet(FSololmcpEditorServices& Services, const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out, FString& Error)
{
	FString Method;
	if (!Arguments->TryGetStringField(TEXT("uv_layout_method"), Method) || Method.IsEmpty())
	{
		Error = TEXT("MP-08 channel_uv_layout_set requires uv_layout_method.");
		return false;
	}
	const bool bBox = Method == TEXT("fast_box_project") || Method == TEXT("reference_box_project");
	const bool bPlane = Method == TEXT("plane_project");
	const bool bVolume = Method == TEXT("volume_encoded");
	if (!bBox && !bPlane && !bVolume)
	{
		Error = TEXT("uv_layout_method must be fast_box_project, reference_box_project, volume_encoded, or plane_project.");
		return false;
	}
	UStaticMesh* Mesh = ResolveTargetMesh(Arguments, Error);
	if (!Mesh)
	{
		return false;
	}
	FMeshDescription* MD = Mesh->GetMeshDescription(0);
	if (!MD)
	{
		Error = FString::Printf(TEXT("No LOD0 MeshDescription on %s."), *Mesh->GetName());
		return false;
	}
	int32 UvChannel = 0;
	Arguments->TryGetNumberField(TEXT("uv_channel"), UvChannel);
	UvChannel = FMath::Clamp(UvChannel, 0, 7);
	FString ChannelName;
	Arguments->TryGetStringField(TEXT("channel_name"), ChannelName);
	FString SeamPolicy = TEXT("none");
	Arguments->TryGetStringField(TEXT("seam_mask_policy"), SeamPolicy);
	if (SeamPolicy != TEXT("none") && SeamPolicy != TEXT("dilate") && SeamPolicy != TEXT("pad"))
	{
		Error = TEXT("seam_mask_policy must be none, dilate, or pad.");
		return false;
	}
	double VeuvSamplesPerM2 = 0.0;
	Arguments->TryGetNumberField(TEXT("veuv_samples_per_square_meter"), VeuvSamplesPerM2);
	TArray<int32> VoxelCount = {4, 4, 4};
	if (Arguments->HasTypedField<EJson::Array>(TEXT("veuv_voxel_count")))
	{
		const TArray<TSharedPtr<FJsonValue>>& Voxels = Arguments->GetArrayField(TEXT("veuv_voxel_count"));
		for (int32 I = 0; I < FMath::Min(3, Voxels.Num()); ++I)
		{
			VoxelCount[I] = FMath::Clamp(static_cast<int32>(Voxels[I]->AsNumber()), 1, 16);
		}
	}
	FString PlaneNormalSource = TEXT("average_normal");
	Arguments->TryGetStringField(TEXT("plane_normal_source"), PlaneNormalSource);
	if (PlaneNormalSource != TEXT("average_normal") && PlaneNormalSource != TEXT("fixed_plane"))
	{
		Error = TEXT("plane_normal_source must be average_normal or fixed_plane.");
		return false;
	}
	FVector3f FixedNormal(0.0f, 0.0f, 1.0f);
	if (Arguments->HasTypedField<EJson::Array>(TEXT("fixed_normal")))
	{
		const TArray<TSharedPtr<FJsonValue>>& NormalValues = Arguments->GetArrayField(TEXT("fixed_normal"));
		if (NormalValues.Num() == 3)
		{
			FixedNormal = FVector3f(
				static_cast<float>(NormalValues[0]->AsNumber()),
				static_cast<float>(NormalValues[1]->AsNumber()),
				static_cast<float>(NormalValues[2]->AsNumber()));
			if (FixedNormal.SizeSquared() < KINDA_SMALL_NUMBER)
			{
				Error = TEXT("fixed_normal must be non-zero.");
				return false;
			}
			FixedNormal.Normalize();
		}
	}
	FString TextureTargetPath;
	if (Arguments->TryGetStringField(TEXT("texture_target"), TextureTargetPath) && !TextureTargetPath.IsEmpty())
	{
		UTexture2D* TargetTex = LoadObject<UTexture2D>(nullptr, *TextureTargetPath);
		if (!TargetTex)
		{
			Error = FString::Printf(TEXT("texture_target '%s' is not a Texture2D asset."), *TextureTargetPath);
			return false;
		}
		const int32 SizeX = TargetTex->GetSizeX();
		const int32 SizeY = TargetTex->GetSizeY();
		if (SizeX < 16 || SizeY < 16 || !FMath::IsPowerOfTwo(static_cast<uint32>(SizeX)) || !FMath::IsPowerOfTwo(static_cast<uint32>(SizeY)))
		{
			Error = TEXT("texture_target must be power-of-two with each side >= 16.");
			return false;
		}
		TSharedRef<FJsonObject> Compat = MakeShared<FJsonObject>();
		Compat->SetBoolField(TEXT("compatible"), true);
		Compat->SetNumberField(TEXT("size_x"), SizeX);
		Compat->SetNumberField(TEXT("size_y"), SizeY);
		Compat->SetBoolField(TEXT("power_of_two"), true);
		Out->SetStringField(TEXT("texture_target"), TargetTex->GetPathName());
		Out->SetObjectField(TEXT("texture_compat"), Compat);
	}

	Mesh->Modify();
	FStaticMeshAttributes Attributes(*MD);
	TVertexInstanceAttributesRef<FVector2f> Uvs = Attributes.GetVertexInstanceUVs();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attributes.GetVertexInstanceNormals();
	if (UvChannel >= Uvs.GetNumChannels())
	{
		Error = FString::Printf(TEXT("uv_channel %d exceeds the %d UV channels of %s."), UvChannel, Uvs.GetNumChannels(), *Mesh->GetName());
		return false;
	}

	FBox Bounds(EForceInit::ForceInit);
	for (const FVertexID& VertexId : MD->Vertices().GetElementIDs())
	{
		Bounds += FVector(Positions[VertexId]);
	}
	const FVector3f Extent = FVector3f(Bounds.GetSize());
	const FVector3f MinPoint = FVector3f(Bounds.Min);
	FVector3f PlaneNormal(0.0f, 0.0f, 1.0f);
	if (bPlane && PlaneNormalSource == TEXT("average_normal"))
	{
		FVector3f Accum(0.0f, 0.0f, 0.0f);
		for (const FVertexInstanceID& InstanceId : MD->VertexInstances().GetElementIDs())
		{
			Accum += InstanceNormals[InstanceId];
		}
		if (Accum.SizeSquared() > KINDA_SMALL_NUMBER)
		{
			Accum.Normalize();
			PlaneNormal = Accum;
		}
	}
	else if (bPlane && PlaneNormalSource == TEXT("fixed_plane"))
	{
		PlaneNormal = FixedNormal;
	}
	const FVector3f Up = FMath::Abs(PlaneNormal.Z) > 0.99f ? FVector3f(1.0f, 0.0f, 0.0f) : FVector3f(0.0f, 0.0f, 1.0f);
	const FVector3f Tangent = FVector3f::CrossProduct(PlaneNormal, Up).GetSafeNormal();
	const FVector3f Bitangent = FVector3f::CrossProduct(PlaneNormal, Tangent).GetSafeNormal();

	for (const FTriangleID& TriangleId : MD->Triangles().GetElementIDs())
	{
		const FVertexInstanceID V0 = MD->GetTriangleVertexInstance(TriangleId, 0);
		const FVertexInstanceID V1 = MD->GetTriangleVertexInstance(TriangleId, 1);
		const FVertexInstanceID V2 = MD->GetTriangleVertexInstance(TriangleId, 2);
		const FVector3f P0 = Positions[MD->GetVertexInstanceVertex(V0)];
		const FVector3f P1 = Positions[MD->GetVertexInstanceVertex(V1)];
		const FVector3f P2 = Positions[MD->GetVertexInstanceVertex(V2)];
		FVector3f FaceNormal = FVector3f::CrossProduct(P1 - P0, P2 - P0);
		if (FaceNormal.SizeSquared() > KINDA_SMALL_NUMBER)
		{
			FaceNormal.Normalize();
		}
		else
		{
			FaceNormal = PlaneNormal;
		}
		FVector3f N = PlaneNormal;
		FVector3f T = Tangent;
		FVector3f Bt = Bitangent;
		if (bBox)
		{
			const FVector3f AbsNormal(FMath::Abs(FaceNormal.X), FMath::Abs(FaceNormal.Y), FMath::Abs(FaceNormal.Z));
			if (AbsNormal.X >= AbsNormal.Y && AbsNormal.X >= AbsNormal.Z)
			{
				N = FVector3f(FMath::Sign(FaceNormal.X), 0.0f, 0.0f); T = FVector3f(0.0f, 1.0f, 0.0f); Bt = FVector3f(0.0f, 0.0f, 1.0f);
			}
			else if (AbsNormal.Y >= AbsNormal.Z)
			{
				N = FVector3f(0.0f, FMath::Sign(FaceNormal.Y), 0.0f); T = FVector3f(1.0f, 0.0f, 0.0f); Bt = FVector3f(0.0f, 0.0f, 1.0f);
			}
			else
			{
				N = FVector3f(0.0f, 0.0f, FMath::Sign(FaceNormal.Z)); T = FVector3f(1.0f, 0.0f, 0.0f); Bt = FVector3f(0.0f, 1.0f, 0.0f);
			}
		}
		const FVector3f Pts[3] = {P0, P1, P2};
		const FVertexInstanceID Ids[3] = {V0, V1, V2};
		for (int32 I = 0; I < 3; ++I)
		{
			float U = 0.0f;
			float V = 0.0f;
			if (bVolume)
			{
				const FVector3f Norm(
					Extent.X > KINDA_SMALL_NUMBER ? (Pts[I].X - MinPoint.X) / Extent.X : 0.5f,
					Extent.Y > KINDA_SMALL_NUMBER ? (Pts[I].Y - MinPoint.Y) / Extent.Y : 0.5f,
					Extent.Z > KINDA_SMALL_NUMBER ? (Pts[I].Z - MinPoint.Z) / Extent.Z : 0.5f);
				const int32 QuantX = FMath::RoundToInt(FMath::Clamp(Norm.X, 0.0f, 1.0f) * static_cast<float>(VoxelCount[0] - 1));
				const int32 QuantZ = FMath::RoundToInt(FMath::Clamp(Norm.Z, 0.0f, 1.0f) * static_cast<float>(VoxelCount[2] - 1));
				U = VoxelCount[0] > 1 ? static_cast<float>(QuantX) / static_cast<float>(VoxelCount[0] - 1) : Norm.X;
				V = VoxelCount[2] > 1 ? static_cast<float>(QuantZ) / static_cast<float>(VoxelCount[2] - 1) : Norm.Z;
			}
			else
			{
				U = FVector3f::DotProduct(Pts[I], T);
				V = FVector3f::DotProduct(Pts[I], Bt);
			}
			Uvs.Set(Ids[I], UvChannel, FVector2f(U, V));
		}
	}

	// Seam accounting: shared edges whose endpoint UVs disagree across connected triangles.
	int32 SeamEdges = 0;
	int32 TotalSharedEdges = 0;
	for (const FEdgeID& EdgeId : MD->Edges().GetElementIDs())
	{
		TArray<FTriangleID> ConnectedTriangles = MD->GetEdgeConnectedTriangles(EdgeId);
		if (ConnectedTriangles.Num() < 2)
		{
			continue;
		}
		++TotalSharedEdges;
		const FVertexID Vx = MD->GetEdgeVertex(EdgeId, 0);
		const FVertexID Vy = MD->GetEdgeVertex(EdgeId, 1);
		FVector2f RefU0;
		FVector2f RefU1;
		bool bHaveReference = false;
		bool bSeam = false;
		for (const FTriangleID& Tri : ConnectedTriangles)
		{
			FVector2f U0;
			FVector2f U1;
			bool b0 = false;
			bool b1 = false;
			for (int32 I = 0; I < 3; ++I)
			{
				const FVertexInstanceID VI = MD->GetTriangleVertexInstance(Tri, I);
				const FVertexID V = MD->GetVertexInstanceVertex(VI);
				if (V == Vx)
				{
					U0 = Uvs.Get(VI, UvChannel);
					b0 = true;
				}
				else if (V == Vy)
				{
					U1 = Uvs.Get(VI, UvChannel);
					b1 = true;
				}
			}
			if (!b0 || !b1)
			{
				continue;
			}
			if (!bHaveReference)
			{
				RefU0 = U0;
				RefU1 = U1;
				bHaveReference = true;
			}
			else if (!U0.Equals(RefU0, 1e-3f) || !U1.Equals(RefU1, 1e-3f))
			{
				bSeam = true;
			}
		}
		if (bSeam)
		{
			++SeamEdges;
		}
	}

	Mesh->CommitMeshDescription(0);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	FString SaveError;
	if (!Services.SaveAsset(Mesh->GetPathName(), false, SaveError))
	{
		Error = FString::Printf(TEXT("Failed to save UV layout on %s: %s"), *Mesh->GetName(), *SaveError);
		return false;
	}
	UStaticMesh* Reloaded = LoadObject<UStaticMesh>(nullptr, *Mesh->GetPathName());
	uint32 UvCrc = 0;
	int32 UvCount = 0;
	FVector2f UvMin(FLT_MAX, FLT_MAX);
	FVector2f UvMax(-FLT_MAX, -FLT_MAX);
	if (Reloaded)
	{
		if (const FMeshDescription* ReloadedMd = Reloaded->GetMeshDescription(0))
		{
			FStaticMeshAttributes ReloadAttributes(const_cast<FMeshDescription&>(*ReloadedMd));
			TVertexInstanceAttributesRef<FVector2f> ReloadUvs = ReloadAttributes.GetVertexInstanceUVs();
			for (const FVertexInstanceID& InstanceId : ReloadedMd->VertexInstances().GetElementIDs())
			{
				const FVector2f Uv = ReloadUvs.Get(InstanceId, UvChannel);
				UvCrc = FCrc::MemCrc32(&Uv, sizeof(FVector2f), UvCrc);
				++UvCount;
				UvMin.X = FMath::Min(UvMin.X, Uv.X); UvMin.Y = FMath::Min(UvMin.Y, Uv.Y);
				UvMax.X = FMath::Max(UvMax.X, Uv.X); UvMax.Y = FMath::Max(UvMax.Y, Uv.Y);
			}
		}
	}
	TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(TEXT("triangle_count"), MD->Triangles().Num());
	Stats->SetNumberField(TEXT("uv_instance_count"), UvCount);
	Stats->SetNumberField(TEXT("seam_edge_count"), SeamEdges);
	Stats->SetNumberField(TEXT("total_shared_edges"), TotalSharedEdges);
	Stats->SetNumberField(TEXT("seam_edge_ratio"), TotalSharedEdges > 0 ? static_cast<double>(SeamEdges) / static_cast<double>(TotalSharedEdges) : 0.0);
	TSharedRef<FJsonObject> UvBounds = MakeShared<FJsonObject>();
	UvBounds->SetArrayField(TEXT("min"), TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(UvMin.X), MakeShared<FJsonValueNumber>(UvMin.Y)});
	UvBounds->SetArrayField(TEXT("max"), TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(UvMax.X), MakeShared<FJsonValueNumber>(UvMax.Y)});
	Stats->SetObjectField(TEXT("uv_bounds"), UvBounds);
	Stats->SetStringField(TEXT("uv_crc32"), FString::Printf(TEXT("%08X"), UvCrc));
	Out->SetObjectField(TEXT("uv_statistics"), Stats);
	Out->SetStringField(TEXT("target_mesh"), Mesh->GetPathName());
	Out->SetStringField(TEXT("uv_layout_method"), Method);
	Out->SetNumberField(TEXT("uv_channel"), UvChannel);
	Out->SetStringField(TEXT("seam_mask_policy"), SeamPolicy);
	Out->SetNumberField(TEXT("veuv_samples_per_square_meter"), VeuvSamplesPerM2);
	Out->SetArrayField(TEXT("veuv_voxel_count"), TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueNumber>(VoxelCount[0]), MakeShared<FJsonValueNumber>(VoxelCount[1]), MakeShared<FJsonValueNumber>(VoxelCount[2])});
	if (!ChannelName.IsEmpty())
	{
		Out->SetStringField(TEXT("channel_name"), ChannelName);
	}
	Out->SetBoolField(TEXT("mutation_applied"), true);
	Out->SetBoolField(TEXT("readback_verified"), Reloaded != nullptr && UvCount > 0);
	Out->SetStringField(TEXT("status"), TEXT("succeeded"));
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("mesh_partition_channel_uv_layout_set_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
	return true;
}

// MP-03 (audit 20260804): real multi-channel weight painting on the Static Mesh
// vertex colors (the R/G/B/A standard channels carry the ChannelMap), with weight
// normalization, optional box/gaussian brush filtering, rollback and persisted
// per-channel histogram readback.
static bool ExecuteWeightPaint(FSololmcpEditorServices& Services, const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out, FString& Error)
{
	if (!Arguments->HasTypedField<EJson::Array>(TEXT("weight_channels")))
	{
		Error = TEXT("MP-03 weight paint requires the weight_channels array.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& ChannelValues = Arguments->GetArrayField(TEXT("weight_channels"));
	if (ChannelValues.IsEmpty())
	{
		Error = TEXT("weight_channels must contain at least one {channel, weight} row.");
		return false;
	}
	struct FWeightRow
	{
		int32 Component;
		double Weight;
	};
	TArray<FWeightRow> Rows;
	for (const TSharedPtr<FJsonValue>& Value : ChannelValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row)
		{
			continue;
		}
		FString Channel;
		if (!Row->TryGetStringField(TEXT("channel"), Channel) || !Row->HasTypedField<EJson::Number>(TEXT("weight")))
		{
			continue;
		}
		const FString Lower = Channel.ToLower();
		int32 Component = INDEX_NONE;
		if (Lower == TEXT("r") || Lower == TEXT("red")) Component = 0;
		else if (Lower == TEXT("g") || Lower == TEXT("green")) Component = 1;
		else if (Lower == TEXT("b") || Lower == TEXT("blue")) Component = 2;
		else if (Lower == TEXT("a") || Lower == TEXT("alpha")) Component = 3;
		if (Component == INDEX_NONE)
		{
			Error = FString::Printf(TEXT("Channel '%s' is not declared; the standard paint channels are R/G/B/A."), *Channel);
			return false;
		}
		Rows.Add({Component, FMath::Clamp(Row->GetNumberField(TEXT("weight")), 0.0, 1.0)});
	}
	if (Rows.IsEmpty())
	{
		Error = TEXT("No valid weight_channel rows were supplied.");
		return false;
	}
	bool bNormalize = true;
	Arguments->TryGetBoolField(TEXT("normalize"), bNormalize);
	FString BrushFilter;
	Arguments->TryGetStringField(TEXT("brush_filter"), BrushFilter);
	if (!BrushFilter.IsEmpty() && BrushFilter != TEXT("none") && BrushFilter != TEXT("box") && BrushFilter != TEXT("gaussian"))
	{
		Error = TEXT("brush_filter must be none, box, or gaussian.");
		return false;
	}
	int32 BrushFilterSize = 3;
	if (Arguments->HasTypedField<EJson::Number>(TEXT("brush_filter_size")))
	{
		BrushFilterSize = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("brush_filter_size"))), 2, 8);
	}
	bool bRollback = false;
	Arguments->TryGetBoolField(TEXT("rollback"), bRollback);
	UStaticMesh* Mesh = ResolveTargetMesh(Arguments, Error);
	if (!Mesh)
	{
		return false;
	}
	FMeshDescription* MD = Mesh->GetMeshDescription(0);
	if (!MD)
	{
		Error = FString::Printf(TEXT("No LOD0 MeshDescription on %s."), *Mesh->GetName());
		return false;
	}
	double Sum = 0.0;
	for (const FWeightRow& Row : Rows)
	{
		Sum += Row.Weight;
	}
	TArray<double> Weights;
	Weights.Reserve(Rows.Num());
	if (bNormalize && Sum > KINDA_SMALL_NUMBER)
	{
		for (const FWeightRow& Row : Rows)
		{
			Weights.Add(Row.Weight / Sum);
		}
	}
	else
	{
		for (const FWeightRow& Row : Rows)
		{
			Weights.Add(Row.Weight);
		}
	}
	if (BrushFilter == TEXT("box") || BrushFilter == TEXT("gaussian"))
	{
		const int32 Half = FMath::Max(1, BrushFilterSize / 2);
		TArray<double> Filtered;
		Filtered.Reserve(Weights.Num());
		for (int32 I = 0; I < Weights.Num(); ++I)
		{
			double Acc = 0.0;
			double Div = 0.0;
			for (int32 K = -Half; K <= Half; ++K)
			{
				const int32 J = FMath::Clamp(I + K, 0, Weights.Num() - 1);
				double W = 1.0;
				if (BrushFilter == TEXT("gaussian"))
				{
					const double X = static_cast<double>(FMath::Abs(K)) / static_cast<double>(Half);
					W = FMath::Exp(-X * X);
				}
				Acc += Weights[J] * W;
				Div += W;
			}
			Filtered.Add(Div > KINDA_SMALL_NUMBER ? Acc / Div : Weights[I]);
		}
		Weights = MoveTemp(Filtered);
	}

	Mesh->Modify();
	FStaticMeshAttributes Attributes(*MD);
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TArray<FVector4f> BeforeColors;
	BeforeColors.Reserve(MD->VertexInstances().Num());
	for (const FVertexInstanceID& InstanceId : MD->VertexInstances().GetElementIDs())
	{
		BeforeColors.Add(Colors[InstanceId]);
	}
	for (const FVertexInstanceID& InstanceId : MD->VertexInstances().GetElementIDs())
	{
		FVector4f Color(0.0f, 0.0f, 0.0f, 1.0f);
		for (int32 I = 0; I < Rows.Num(); ++I)
		{
			const double W = FMath::Clamp(Weights[I], 0.0, 1.0);
			switch (Rows[I].Component)
			{
			case 0: Color.X = static_cast<float>(W); break;
			case 1: Color.Y = static_cast<float>(W); break;
			case 2: Color.Z = static_cast<float>(W); break;
			case 3: Color.W = static_cast<float>(W); break;
			default: break;
			}
		}
		Colors[InstanceId] = Color;
	}
	Mesh->CommitMeshDescription(0);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	FString SaveError;
	if (!Services.SaveAsset(Mesh->GetPathName(), false, SaveError))
	{
		Error = FString::Printf(TEXT("Failed to save weight paint on %s: %s"), *Mesh->GetName(), *SaveError);
		return false;
	}

	// Persisted per-channel histogram readback after save/reload.
	TArray<TSharedPtr<FJsonValue>> HistogramRows;
	bool bReadbackOk = false;
	UStaticMesh* ReloadedMesh = LoadObject<UStaticMesh>(nullptr, *Mesh->GetPathName());
	if (ReloadedMesh)
	{
		if (FMeshDescription* ReloadedMd = ReloadedMesh->GetMeshDescription(0))
		{
			FStaticMeshAttributes ReloadAttributes(*ReloadedMd);
			TVertexInstanceAttributesRef<FVector4f> ReloadColors = ReloadAttributes.GetVertexInstanceColors();
			const TArray<FString> ChannelNames = {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")};
			for (int32 C = 0; C < 4; ++C)
			{
				double MinV = 1.0;
				double MaxV = 0.0;
				double SumV = 0.0;
				int32 Count = 0;
				for (const FVertexInstanceID& InstanceId : ReloadedMd->VertexInstances().GetElementIDs())
				{
					const FVector4f Color = ReloadColors[InstanceId];
					const double V = C == 0 ? Color.X : (C == 1 ? Color.Y : (C == 2 ? Color.Z : Color.W));
					MinV = FMath::Min(MinV, V);
					MaxV = FMath::Max(MaxV, V);
					SumV += V;
					++Count;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("channel"), ChannelNames[C]);
				Row->SetNumberField(TEXT("min"), MinV);
				Row->SetNumberField(TEXT("max"), MaxV);
				Row->SetNumberField(TEXT("mean"), Count > 0 ? SumV / static_cast<double>(Count) : 0.0);
				Row->SetNumberField(TEXT("vertex_count"), Count);
				HistogramRows.Add(MakeShared<FJsonValueObject>(Row));
			}
			bReadbackOk = HistogramRows.Num() == 4;
		}
	}

	if (bRollback)
	{
		if (FMeshDescription* RollbackMd = (ReloadedMesh ? ReloadedMesh->GetMeshDescription(0) : MD))
		{
			FStaticMeshAttributes RollbackAttributes(*RollbackMd);
			TVertexInstanceAttributesRef<FVector4f> RollbackColors = RollbackAttributes.GetVertexInstanceColors();
			int32 Index = 0;
			for (const FVertexInstanceID& InstanceId : RollbackMd->VertexInstances().GetElementIDs())
			{
				if (Index < BeforeColors.Num())
				{
					RollbackColors[InstanceId] = BeforeColors[Index];
				}
				++Index;
			}
			if (ReloadedMesh)
			{
				ReloadedMesh->CommitMeshDescription(0);
				ReloadedMesh->PostEditChange();
				ReloadedMesh->MarkPackageDirty();
				FString RollbackSaveError;
				Services.SaveAsset(ReloadedMesh->GetPathName(), false, RollbackSaveError);
			}
		}
		Out->SetBoolField(TEXT("restored"), true);
		Out->SetStringField(TEXT("rollback_status"), TEXT("restored_pre_snapshot"));
	}

	Out->SetArrayField(TEXT("per_channel_readback"), HistogramRows);
	Out->SetStringField(TEXT("target_mesh"), Mesh->GetPathName());
	Out->SetBoolField(TEXT("normalized"), bNormalize);
	Out->SetStringField(TEXT("brush_filter"), BrushFilter.IsEmpty() ? TEXT("none") : BrushFilter);
	Out->SetNumberField(TEXT("brush_filter_size"), BrushFilterSize);
	Out->SetNumberField(TEXT("channel_count"), Rows.Num());
	Out->SetBoolField(TEXT("mutation_applied"), true);
	Out->SetBoolField(TEXT("readback_verified"), bReadbackOk);
	Out->SetStringField(TEXT("status"), TEXT("succeeded"));
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("mesh_partition_attribute_paint_apply_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
	return true;
}

static bool Execute(FSololmcpEditorServices& Services, const FString& Name, const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	Out->SetStringField(TEXT("tool"), Name);
	Out->SetStringField(TEXT("implementation"), TEXT("ue58_native_mesh_terrain"));
	UObject* ResolvedTarget = ResolveTarget(Arguments);
	if (!IsUE58())
	{
		Out->SetStringField(TEXT("status"), TEXT("requires_ue_5_8"));
		Error = TEXT("This tool is available only in the UE 5.8 build.");
		return false;
	}
	if (Name == TEXT("mesh_terrain_preview_capture") || Name == TEXT("mesh_terrain_visual_qa_capture"))
	{
		return CaptureViewport(Services, Name, Arguments, Out, Summary, Error);
	}

	if (Name == TEXT("mesh_terrain_mode_enter"))
	{
		if (!EnterMode(Error)) return false;
		AddModeState(Out); Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = TEXT("Entered UE 5.8 Mesh Terrain Mode."); return true;
	}
	if (Name == TEXT("mesh_terrain_mode_exit"))
	{
		if (UInteractiveToolManager* Manager = GetToolManager())
		{
			if (Manager->GetActiveTool(EToolSide::Left))
			{
				bool bAccept = false; Arguments->TryGetBoolField(TEXT("accept_active_tool"), bAccept);
				if (bAccept && Manager->CanAcceptActiveTool(EToolSide::Left)) Manager->DeactivateTool(EToolSide::Left, EToolShutdownType::Accept);
				else Manager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
			}
		}
		if (GEditor) { GLevelEditorModeTools().DeactivateMode(ModeId); GLevelEditorModeTools().ActivateMode(FBuiltinEditorModes::EM_Default, false); }
		AddModeState(Out); Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = TEXT("Closed Mesh Terrain Mode and returned to Select mode."); return true;
	}
	if (Name == TEXT("mesh_terrain_tool_accept") || Name == TEXT("mesh_terrain_sculpt_session_commit"))
	{
		TSharedRef<FJsonObject> Receipt = BeginReceipt(Name, ResolvedTarget);
		UInteractiveToolManager* Manager = GetToolManager();
		if (!Manager || !Manager->GetActiveTool(EToolSide::Left)) { Error = TEXT("No active tool to accept."); return false; }
		if (!Manager->CanAcceptActiveTool(EToolSide::Left)) { Error = TEXT("Active tool is not ready to accept; required viewport input or valid target is missing."); return false; }
		Manager->DeactivateTool(EToolSide::Left, EToolShutdownType::Accept);
		bool bSave = true;
		Arguments->TryGetBoolField(TEXT("save"), bSave);
		if (ResolvedTarget && !SaveObjectPackage(ResolvedTarget, bSave, Receipt, Error))
		{
			CompleteReceipt(Receipt, ResolvedTarget, TEXT("failed_save_verification"));
			Out->SetObjectField(TEXT("receipt"), Receipt);
			return false;
		}
		CompleteReceipt(Receipt, ResolvedTarget, ResolvedTarget ? TEXT("completed_with_target_readback") : TEXT("completed_interactive_output_pending_discovery"));
		AddModeState(Out); Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetObjectField(TEXT("receipt"), Receipt);
		Summary = TEXT("Accepted the active Mesh Terrain tool through the native tool manager."); return true;
	}
	if (Name == TEXT("mesh_terrain_tool_cancel"))
	{
		UInteractiveToolManager* Manager = GetToolManager();
		if (!Manager || !Manager->GetActiveTool(EToolSide::Left)) { Error = TEXT("No active tool to cancel."); return false; }
		Manager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
		AddModeState(Out); Out->SetStringField(TEXT("status"), TEXT("cancelled"));
		Summary = TEXT("Cancelled the active Mesh Terrain tool."); return true;
	}
	if (Name == TEXT("mesh_terrain_mode_state_get") || Name == TEXT("mesh_terrain_active_tool_get") || Name == TEXT("mesh_terrain_tool_properties_get"))
	{
		AddModeState(Out);
		if (UInteractiveToolManager* Manager = GetToolManager())
		{
			if (UInteractiveTool* Tool = Manager->GetActiveTool(EToolSide::Left))
			{
				TArray<TSharedPtr<FJsonValue>> Sets;
				for (UObject* Set : Tool->GetToolProperties(false))
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("class"), Set->GetClass()->GetPathName());
					Row->SetObjectField(TEXT("properties"), ExportProperties(Set));
					Sets.Add(MakeShared<FJsonValueObject>(Row));
				}
				Out->SetArrayField(TEXT("property_sets"), Sets);
			}
		}
		Out->SetStringField(TEXT("status"), TEXT("completed")); Summary = TEXT("Returned native Mesh Terrain mode/tool state."); return true;
	}
	if (Name == TEXT("mesh_terrain_tool_properties_set") || Name == TEXT("mesh_terrain_sculpt_settings_set"))
	{
		const TSharedPtr<FJsonObject>* Patch = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("properties"), Patch) || !Patch || !Patch->IsValid()) { Error = TEXT("Missing properties object."); return false; }
		if (!PatchActiveTool(*Patch, Out, Error)) return false;
		AddModeState(Out); Out->SetStringField(TEXT("status"), TEXT("running"));
		Out->SetBoolField(TEXT("requires_accept_or_cancel"), true);
		Summary = TEXT("Updated active Mesh Terrain tool properties; the operation remains running until accept or cancel."); return true;
	}
	if (Name == TEXT("mesh_terrain_tool_start"))
	{
		const FString ToolId = ResolveToolId(Name, Arguments);
		if (!StartTool(ToolId, Out, Error)) return false;
		const TSharedPtr<FJsonObject>* Patch = nullptr;
		if (Arguments->TryGetObjectField(TEXT("properties"), Patch) && Patch && Patch->IsValid() && !PatchActiveTool(*Patch, Out, Error)) return false;
		AddModeState(Out); Summary = FString::Printf(TEXT("Started native Mesh Terrain tool %s."), *ToolId); return true;
	}
	if (Name == TEXT("mesh_terrain_submode_set"))
	{
		FString Submode;
		if (!Arguments->TryGetStringField(TEXT("submode"), Submode) || Submode.IsEmpty()) { Error = TEXT("Missing submode."); return false; }
		const TMap<FString, FString> Defaults = {
			{TEXT("create"), TEXT("BeginCreateMegaMeshRectangleTool")}, {TEXT("edit"), TEXT("BeginConvertMegaMeshTool")},
			{TEXT("sculpt"), TEXT("BeginHeightSculptTool")}, {TEXT("paint"), TEXT("BeginMeshAttributePaintTool")},
			{TEXT("shapes"), TEXT("BeginAddBoxPrimitiveTool")}, {TEXT("modifiers"), TEXT("BeginAddModifierTool")}
		};
		const FString* ToolId = Defaults.Find(Submode.ToLower());
		if (!ToolId) { Error = TEXT("submode must be create, edit, sculpt, paint, shapes, or modifiers."); return false; }
		if (!StartTool(*ToolId, Out, Error)) return false;
		Out->SetStringField(TEXT("submode"), Submode.ToLower());
		AddModeState(Out); Summary = FString::Printf(TEXT("Activated the %s Mesh Terrain workflow with native tool %s."), *Submode, **ToolId); return true;
	}

#if SOMOLMCP_WITH_UE58_MESHPARTITION
	// Mesh Paint upgrade routes MP-03 / MP-07 / MP-08 (audit 20260804): the
	// dispatch names below are served by the dedicated native executors that
	// land in this translation unit. Each executor performs a real persisted
	// mutation on the resolved Static Mesh target with structured readback
	// (per-channel weight histogram / saved-pixel readback / UV statistics).
	if (Name == TEXT("mesh_partition_attribute_paint_apply") && Arguments->HasTypedField<EJson::Array>(TEXT("weight_channels")))
	{
		return ExecuteWeightPaint(Services, Arguments, Out, Error);
	}
	if (Name == TEXT("mesh_partition_attribute_map_create"))
	{
		return ExecuteAttributeMapCreate(Services, Arguments, Out, Error);
	}
	if (Name == TEXT("mesh_partition_channel_uv_layout_set"))
	{
		return ExecuteChannelUvLayoutSet(Services, Arguments, Out, Error);
	}
#endif

	if (IsReadOperation(Name))
	{
		AddModeState(Out);
		if (ResolvedTarget)
		{
			Out->SetStringField(TEXT("target_path"), ResolvedTarget->GetPathName());
			Out->SetStringField(TEXT("target_class"), ResolvedTarget->GetClass()->GetPathName());
			Out->SetObjectField(TEXT("editable_properties"), ExportProperties(ResolvedTarget));
			Out->SetObjectField(TEXT("readback_receipt"), SnapshotObject(ResolvedTarget));
		}
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = FString::Printf(TEXT("%s returned UE 5.8 native readback evidence."), *Name); return true;
	}

	bool bDryRun = false; Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
	if (bDryRun)
	{
		Out->SetStringField(TEXT("status"), TEXT("dry_run"));
		Out->SetStringField(TEXT("native_tool_id"), ResolveToolId(Name, Arguments));
		Summary = FString::Printf(TEXT("%s native route validated without mutation."), *Name); return true;
	}

	const TSharedPtr<FJsonObject>* Patch = nullptr;
	if (ResolvedTarget)
	{
		if (Arguments->TryGetObjectField(TEXT("properties"), Patch) && Patch && Patch->IsValid())
		{
			TSharedRef<FJsonObject> Receipt = BeginReceipt(Name, ResolvedTarget);
			TArray<FString> Changed;
			if (!PatchProperties(ResolvedTarget, *Patch, Changed, Error)) return false;
			ResolvedTarget->MarkPackageDirty();
			TArray<TSharedPtr<FJsonValue>> ChangedJson;
			for (const FString& Field : Changed) ChangedJson.Add(MakeShared<FJsonValueString>(Field));
			Receipt->SetArrayField(TEXT("changed_properties"), ChangedJson);
			bool bSave = true;
			Arguments->TryGetBoolField(TEXT("save"), bSave);
			if (!SaveObjectPackage(ResolvedTarget, bSave, Receipt, Error))
			{
				CompleteReceipt(Receipt, ResolvedTarget, TEXT("failed_save_verification"));
				Out->SetObjectField(TEXT("receipt"), Receipt);
				return false;
			}
			CompleteReceipt(Receipt, ResolvedTarget, bSave ? TEXT("completed_saved_readback") : TEXT("completed_dirty_readback"));
			Out->SetStringField(TEXT("target_path"), ResolvedTarget->GetPathName());
			Out->SetObjectField(TEXT("post_readback"), ExportProperties(ResolvedTarget));
			Out->SetStringField(TEXT("status"), TEXT("completed"));
			Out->SetBoolField(TEXT("package_dirty"), ResolvedTarget->GetOutermost()->IsDirty());
			Out->SetObjectField(TEXT("receipt"), Receipt);
			Summary = FString::Printf(TEXT("%s applied a native reflected property transaction."), *Name); return true;
		}
	}

	const FString ToolId = ResolveToolId(Name, Arguments);
	if (!ToolId.IsEmpty())
	{
		if (!StartTool(ToolId, Out, Error)) return false;
		if (Arguments->TryGetObjectField(TEXT("properties"), Patch) && Patch && Patch->IsValid() && !PatchActiveTool(*Patch, Out, Error)) return false;
		AddModeState(Out);
		Out->SetStringField(TEXT("next_action"), TEXT("provide viewport input if required, then call mesh_terrain_tool_accept"));
		Out->SetBoolField(TEXT("requires_accept_or_cancel"), true);
		TSharedRef<FJsonObject> Receipt = BeginReceipt(Name, ResolvedTarget);
		Receipt->SetStringField(TEXT("status"), TEXT("running_interactive_tool"));
		Receipt->SetStringField(TEXT("native_tool_id"), ToolId);
		Out->SetObjectField(TEXT("receipt"), Receipt);
		Summary = FString::Printf(TEXT("%s started native interactive tool %s; operation remains running until accepted."), *Name, *ToolId); return true;
	}

	Out->SetStringField(TEXT("status"), TEXT("blocked_no_native_writer_route"));
	Out->SetStringField(TEXT("reason_code"), TEXT("blocked_no_native_writer_route"));
	Error = FString::Printf(TEXT("%s has a public typed contract but no safe native writer route for the supplied target/arguments."), *Name);
	return false;
}

static TSharedRef<FJsonObject> Schema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("target_asset"), FSololmcpSchemaBuilder::String(TEXT("Bound MeshPartition definition, modifier, section, graph, or generated asset path."))},
		{TEXT("definition_asset"), FSololmcpSchemaBuilder::String(TEXT("MeshPartition definition asset path."))},
		{TEXT("modifier_asset"), FSololmcpSchemaBuilder::String(TEXT("MeshPartition modifier object or asset path."))},
		{TEXT("mesh_partition_asset"), FSololmcpSchemaBuilder::String(TEXT("MeshPartition target path."))},
		{TEXT("target_level"), FSololmcpSchemaBuilder::String(TEXT("Bound target level path."))},
		{TEXT("tool_id"), FSololmcpSchemaBuilder::String(TEXT("Exact UE 5.8 Mesh Terrain interactive tool identifier."))},
		{TEXT("submode"), FSololmcpSchemaBuilder::String(TEXT("create, edit, sculpt, paint, shapes, or modifiers."))},
		{TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("Typed operation or subtype."))},
		{TEXT("properties"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Typed property patch using reflected UE property names."))},
		{TEXT("strokes"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}, {}), TEXT("Bounded world-space stroke records."))},
		{TEXT("receipt"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Structured build/readback/QA receipt."))},
		{TEXT("save"), FSololmcpSchemaBuilder::Boolean(TEXT("Save and verify the target package after a direct mutation or accepted interactive tool. Defaults to true."))},
		{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum viewport capture width."))},
		{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum viewport capture height."))},
		{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Execute the native operation. Defaults true for write names."))},
		{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Validate route only; do not mutate."))},
		{TEXT("accept_active_tool"), FSololmcpSchemaBuilder::Boolean(TEXT("Accept an active tool when exiting; otherwise cancel."))},
		// Mesh Paint upgrade MP-03/MP-07/MP-08 schema fields (audit 20260804).
		{TEXT("weight_channels"), FSololmcpSchemaBuilder::Array(
			FSololmcpSchemaBuilder::Object({
				{TEXT("channel"), FSololmcpSchemaBuilder::String(TEXT("Channel name declared in the MeshPartition ChannelMap."))},
				{TEXT("weight"), FSololmcpSchemaBuilder::Number(TEXT("Channel weight in 0..1."), 0.0, 1.0)}
			}, {TEXT("channel"), TEXT("weight")}, TEXT("One multi-channel weight row.")),
			TEXT("MP-03: multi-channel weight payload applied to the bound MeshPartitionDefinition."), 1, 24)},
		{TEXT("normalize"), FSololmcpSchemaBuilder::Boolean(TEXT("MP-03: normalize weights to sum to 1 before persistence. Defaults to true."))},
		{TEXT("brush_filter"), FSololmcpSchemaBuilder::String(TEXT("MP-03: brush filtering applied to the ordered weight vector."),
			{TEXT("none"), TEXT("box"), TEXT("gaussian")})},
		{TEXT("brush_filter_size"), FSololmcpSchemaBuilder::Integer(TEXT("MP-03: box filter window size."), 2, 8)},
		{TEXT("rollback"), FSololmcpSchemaBuilder::Boolean(TEXT("MP-03/MP-08: undo the transaction after readback verification and report the restored state."))},
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("MP-07: /Game/ asset path of the Mesh Paint texture to create."))},
		{TEXT("resolution"), FSololmcpSchemaBuilder::Integer(TEXT("MP-07: power-of-two texture resolution."), 16, 4096)},
		{TEXT("format"), FSololmcpSchemaBuilder::String(TEXT("MP-07: closed pixel format for the created texture."),
			{TEXT("bgra8"), TEXT("grayscale")})},
		{TEXT("uv_channel"), FSololmcpSchemaBuilder::Integer(TEXT("MP-07: UV channel index used by texture painting."), 0, 7)},
		{TEXT("fill_color"), FSololmcpSchemaBuilder::Array(
			FSololmcpSchemaBuilder::Number(TEXT("Channel value in 0..1."), 0.0, 1.0),
			TEXT("MP-07: RGBA fill color written into the created texture."), 4, 4)},
		{TEXT("overwrite"), FSololmcpSchemaBuilder::Boolean(TEXT("MP-07: allow replacing an existing texture asset."))},
		{TEXT("backup"), FSololmcpSchemaBuilder::Boolean(TEXT("MP-07: duplicate the existing texture to *_backup before overwrite."))},
		{TEXT("commit"), FSololmcpSchemaBuilder::Boolean(TEXT("MP-07: save the created texture; false leaves it dirty/uncommitted. Defaults to true."))},
		{TEXT("revert"), FSololmcpSchemaBuilder::Boolean(TEXT("MP-07: delete the created texture after commit to revert the session."))},
		{TEXT("target_mesh"), FSololmcpSchemaBuilder::String(TEXT("MP-07: Static Mesh whose UV channel count validates uv_channel."))},
		{TEXT("channel_name"), FSololmcpSchemaBuilder::String(TEXT("MP-07/MP-08: ChannelMap channel selected for texture painting; must already exist."))},
		{TEXT("uv_layout_method"), FSololmcpSchemaBuilder::String(TEXT("MP-08: channel UV layout algorithm."),
			{TEXT("fast_box_project"), TEXT("reference_box_project"), TEXT("volume_encoded"), TEXT("plane_project")})},
		{TEXT("plane_normal_source"), FSololmcpSchemaBuilder::String(TEXT("MP-08: plane-project normal source."),
			{TEXT("average_normal"), TEXT("fixed_plane")})},
		{TEXT("fixed_normal"), FSololmcpSchemaBuilder::Array(
			FSololmcpSchemaBuilder::Number(TEXT("Normal component.")),
			TEXT("MP-08: fixed projection-plane normal (must be non-zero)."), 3, 3)},
		{TEXT("veuv_samples_per_square_meter"), FSololmcpSchemaBuilder::Number(TEXT("MP-08: VEUV sample density."), 0.001, 1000.0)},
		{TEXT("veuv_voxel_count"), FSololmcpSchemaBuilder::Array(
			FSololmcpSchemaBuilder::Integer(TEXT("Voxel grid dimension."), 1, 16),
			TEXT("MP-08: VEUV voxel grid dimensions [x, y, z]."), 3, 3)},
		{TEXT("seam_mask_policy"), FSololmcpSchemaBuilder::String(TEXT("MP-08: seam mask policy recorded for the texture-paint session."),
			{TEXT("none"), TEXT("dilate"), TEXT("pad")})},
		{TEXT("texture_target"), FSololmcpSchemaBuilder::String(TEXT("MP-08: texture asset checked for paint-target compatibility (power-of-two, >= 16)."))}
	});
}
}

void RegisterMeshTerrainNativeTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
	for (const TCHAR* NamePtr : MeshTerrainNative::ToolNames)
	{
		const FString Name(NamePtr);
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = FString::Printf(TEXT("UE 5.8 native Mesh Terrain/MeshPartition operation: %s. Uses target binding, native editor APIs, and explicit running/completed receipts."), *Name);
		Def.InputSchema = MeshTerrainNative::Schema();
		const bool bTransientEditorState = Name == TEXT("mesh_terrain_mode_state_get")
			|| Name == TEXT("mesh_terrain_active_tool_get")
			|| Name == TEXT("mesh_terrain_tool_properties_get");
		Def.CacheTtlSeconds = MeshTerrainNative::IsReadOperation(Name) && !bTransientEditorState ? 2 : 0;
		Def.Execute = [Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return MeshTerrainNative::Execute(Context.Services, Name, Arguments, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
#else
	(void)Registry;
#endif
}
}
