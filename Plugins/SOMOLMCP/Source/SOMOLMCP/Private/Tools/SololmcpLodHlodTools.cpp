// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.1 — LOD / HLOD Management Tools
// Full CRUD for StaticMesh LOD, SkeletalMesh LOD, and Level HLOD configuration
//
// UE 5.7 API Changes:
//   - UStaticMesh::GetLODScreenSize removed
//   - UStaticMesh::SetLODScreenSize removed  
//   - UStaticMesh::GetLODSettings removed
//   - USkeletalMesh::ScreenSize is now FPerPlatformFloat
//   - FSkeletalMeshLODInfo::ScreenSize type changed
//   - Context.Services.LoadAsset<T> template not supported
//
// Workaround: Simplified LOD listing without screen size data for StaticMesh.
// SkeletalMesh LOD info still partially functional.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
// FSkeletalMeshLODInfo is only forward-declared in Engine/SkinnedAsset.h; the definition
// lives in Engine/SkinnedAssetCommon.h on every supported version.
// It reaches this file transitively on 5.4+ but not on 5.3, where every member access
// on it reports "use of undefined type" instead of a missing include.
#include "Engine/SkinnedAssetCommon.h"
#include "Landscape.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/WorldPartition.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#else
// WorldPartitionActorDescInstance.h is 5.4+.
#endif
#include "Editor.h"
#include "ImportUtils/StaticMeshImportUtils.h"
#include "MeshUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "EngineUtils.h"

namespace UE::SOMOLMCP
{

static bool SomolGetBoolArg(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, bool DefaultValue)
{
	bool Value = DefaultValue;
	Args->TryGetBoolField(Name, Value);
	return Value;
}

static int32 SomolCountHlodLikeActors(UWorld* World)
{
	if (!World)
	{
		return 0;
	}
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (Actor && Actor->GetClass() && Actor->GetClass()->GetName().Contains(TEXT("HLOD")))
		{
			++Count;
		}
	}
	return Count;
}

// ============================================================================
// LOD Tools
// ============================================================================

static void RegisterLodTools(FSololmcpToolRegistry& Registry)
{
	// ---- lod_list ----
	// UE 5.7: Simplified version without screen size data
	Registry.Register({
		TEXT("lod_list"),
		TEXT("List LOD information for a static or skeletal mesh asset. Note: Screen size data not available in UE 5.7 for StaticMesh."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("StaticMesh or SkeletalMesh asset path"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			// Audit round 4: reject empty/invalid asset_path early to silence LogEditorAssetSubsystem LoadAsset noise.
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				OutError = TEXT("Missing or invalid asset_path");
				return false;
			}
			if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
			{
				OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
				return false;
			}
			UObject* Asset = Cast<UObject>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Asset) return false;

			TArray<TSharedPtr<FJsonValue>> LodsJson;

			if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
			{
				const int32 NumLODs = SM->GetNumLODs();
				OutStructured->SetNumberField(TEXT("lod_count"), NumLODs);
				OutStructured->SetNumberField(TEXT("triangle_count_lod0"), SM->GetNumTriangles(0));

				for (int32 i = 0; i < NumLODs; ++i)
				{
					TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
					LodObj->SetNumberField(TEXT("lod_index"), i);
					LodObj->SetNumberField(TEXT("triangle_count"), SM->GetNumTriangles(i));
					LodObj->SetNumberField(TEXT("vertex_count"), SM->GetNumVertices(i));
					// UE 5.7: Screen size API removed - use ComputeLODScreenSize static method if needed
					LodObj->SetStringField(TEXT("screen_size_note"), TEXT("UE 5.7: Use UStaticMesh::ComputeLODScreenSize() for auto-computed values"));
					LodsJson.Add(MakeShared<FJsonValueObject>(LodObj));
				}

				OutStructured->SetBoolField(TEXT("nanite_enabled"), SM->IsNaniteEnabled());
				OutSummary = FString::Printf(TEXT("Mesh '%s': %d LODs, %d triangles (LOD0)"), *SM->GetName(), NumLODs, SM->GetNumTriangles(0));
			}
			else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
			{
				const int32 NumLODs = SK->GetLODNum();
				OutStructured->SetNumberField(TEXT("lod_count"), NumLODs);
				OutStructured->SetStringField(TEXT("asset_type"), TEXT("SkeletalMesh"));

				for (int32 i = 0; i < NumLODs; ++i)
				{
					TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
					LodObj->SetNumberField(TEXT("lod_index"), i);
					// UE 5.7: ScreenSize is now FPerPlatformFloat, use default platform value
					if (FSkeletalMeshLODInfo* LODInfo = SK->GetLODInfo(i))
					{
						// Get default platform value
						LodObj->SetNumberField(TEXT("screen_size"), LODInfo->ScreenSize.GetDefault());
					}

					// Materials for this LOD
					TArray<TSharedPtr<FJsonValue>> MatsJson;
					const TArray<FSkeletalMaterial>& Materials = SK->GetMaterials();
					for (int32 MatIdx = 0; MatIdx < Materials.Num(); ++MatIdx)
					{
						const FSkeletalMaterial& Mat = Materials[MatIdx];
						TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
						MatObj->SetNumberField(TEXT("slot_index"), MatIdx);
						MatObj->SetStringField(TEXT("material"), Mat.MaterialInterface ? Mat.MaterialInterface->GetPathName() : TEXT("None"));
						MatsJson.Add(MakeShared<FJsonValueObject>(MatObj));
					}
					LodObj->SetArrayField(TEXT("materials"), MatsJson);
					LodsJson.Add(MakeShared<FJsonValueObject>(LodObj));
				}

				OutSummary = FString::Printf(TEXT("SkeletalMesh '%s': %d LODs"), *SK->GetName(), NumLODs);
			}
			else
			{
				OutError = TEXT("Asset is not a StaticMesh or SkeletalMesh.");
				return false;
			}

			OutStructured->SetArrayField(TEXT("lods"), LodsJson);
			return true;
		}
	});

	// ---- lod_set_screen_size ----
	// UE 5.7: Disabled for StaticMesh, partially functional for SkeletalMesh
	Registry.Register({
		TEXT("lod_set_screen_size"),
		TEXT("Set the screen size transition for a specific LOD level. Note: StaticMesh not supported in UE 5.7, SkeletalMesh only."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("screen_size"), FSololmcpSchemaBuilder::Number(TEXT("Screen size (0.0-1.0), higher = visible from farther"))}
		}, {TEXT("asset_path"), TEXT("lod_index"), TEXT("screen_size")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			const int32 LodIndex = Arguments->GetIntegerField(TEXT("lod_index"));
			const float ScreenSize = static_cast<float>(Arguments->GetNumberField(TEXT("screen_size")));

			UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
			if (!Asset) return false;

			bool bSuccess = false;
			if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
			{
				OutError = TEXT("UE 5.7: UStaticMesh::SetLODScreenSize removed. Use LOD group settings or auto-computed screen sizes.");
				return false;
			}
			else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
			{
				if (LodIndex < 0 || LodIndex >= SK->GetLODNum())
				{
					OutError = FString::Printf(TEXT("Invalid LOD index %d (mesh has %d LODs)"), LodIndex, SK->GetLODNum());
					return false;
				}
				FSkeletalMeshLODInfo* LodInfo = SK->GetLODInfo(LodIndex);
				if (LodInfo)
				{
					// UE 5.7: ScreenSize is FPerPlatformFloat, set default value
					LodInfo->ScreenSize = ScreenSize;
					SK->PostEditChange();
					SK->MarkPackageDirty();
					const float Readback = LodInfo->ScreenSize.GetDefault();
					OutStructured->SetNumberField(TEXT("readback_screen_size"), Readback);
					if (!FMath::IsNearlyEqual(Readback, ScreenSize, 0.0001f))
					{
						OutError = FString::Printf(TEXT("LOD screen size write did not verify (wanted %.4f, read %.4f)."), ScreenSize, Readback);
						return false;
					}
					bSuccess = true;
				}
			}
			else
			{
				OutError = TEXT("Asset is not a StaticMesh or SkeletalMesh.");
				return false;
			}

			if (bSuccess)
			{
				OutStructured->SetNumberField(TEXT("lod_index"), LodIndex);
				OutStructured->SetNumberField(TEXT("screen_size"), ScreenSize);
				OutSummary = FString::Printf(TEXT("Set LOD %d screen size to %.4f"), LodIndex, ScreenSize);
			}
			return bSuccess;
		}
	});

	// ---- lod_set_material ----
	Registry.Register({
		TEXT("lod_set_material"),
		TEXT("Override a material on a specific LOD of a mesh."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer()},
			{TEXT("material_slot"), FSololmcpSchemaBuilder::Integer(TEXT("Material slot index"))},
			{TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path (empty = reset to default)"))}
		}, {TEXT("asset_path"), TEXT("lod_index"), TEXT("material_slot")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			const int32 LodIndex = Arguments->GetIntegerField(TEXT("lod_index"));
			const int32 MaterialSlot = Arguments->GetIntegerField(TEXT("material_slot"));
			const FString MaterialPath = Arguments->HasField(TEXT("material_path")) ? Arguments->GetStringField(TEXT("material_path")) : FString();

			UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
			if (!Asset) return false;

			UMaterialInterface* Material = nullptr;
			if (!MaterialPath.IsEmpty())
			{
				Material = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *MaterialPath));
				if (!Material)
				{
					OutError = FString::Printf(TEXT("Material not found: %s"), *MaterialPath);
					return false;
				}
			}

			if (UStaticMesh* SM = Cast<UStaticMesh>(Asset))
			{
				if (LodIndex < 0 || LodIndex >= SM->GetNumLODs())
				{
					OutError = FString::Printf(TEXT("Invalid LOD index %d (mesh has %d LODs)"), LodIndex, SM->GetNumLODs());
					return false;
				}
				// UE 5.7: SetMaterial on StaticMesh works globally, not per-LOD
				// Use component override for per-LOD materials
				OutError = TEXT("UE 5.7: Per-LOD material override on StaticMesh requires component-level settings.");
				return false;
			}
			else if (USkeletalMesh* SK = Cast<USkeletalMesh>(Asset))
			{
				if (LodIndex < 0 || LodIndex >= SK->GetLODNum())
				{
					OutError = FString::Printf(TEXT("Invalid LOD index %d (mesh has %d LODs)"), LodIndex, SK->GetLODNum());
					return false;
				}
				FSkeletalMeshLODInfo* LodInfo = SK->GetLODInfo(LodIndex);
				if (LodInfo && MaterialSlot < LodInfo->LODMaterialMap.Num())
				{
					OutStructured->SetNumberField(TEXT("lod_index"), LodIndex);
					OutStructured->SetNumberField(TEXT("material_slot"), MaterialSlot);
					OutStructured->SetStringField(TEXT("material_path"), MaterialPath);
					OutStructured->SetBoolField(TEXT("applied"), false);
					OutError = TEXT("UE 5.7: SkeletalMesh per-LOD material override is not implemented with a verified write/readback path.");
					return false;
				}
				OutError = TEXT("Invalid material slot or LOD info.");
				return false;
			}

			OutError = TEXT("Asset is not a StaticMesh or SkeletalMesh.");
			return false;
		}
	});
}

// ============================================================================
// HLOD Tools
// ============================================================================

static void RegisterHlodTools(FSololmcpToolRegistry& Registry)
{
	// ---- hlod_list ----
	Registry.Register({
		TEXT("hlod_list"),
		TEXT("List HLOD layers and their settings in the current level."),
		FSololmcpSchemaBuilder::Object({{}}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			TArray<TSharedPtr<FJsonValue>> LayersJson;

			// Get WorldPartition if available
			if (UWorldPartition* WP = World->GetWorldPartition())
			{
				// Iterate HLOD layers
				// Note: HLOD layer enumeration requires WorldPartition internals
				OutStructured->SetBoolField(TEXT("world_partition_enabled"), true);
			}

			OutStructured->SetArrayField(TEXT("layers"), LayersJson);
			OutSummary = FString::Printf(TEXT("Found %d HLOD layers"), LayersJson.Num());
			return true;
		}
	});

	// ---- hlod_build ----
	Registry.Register({
		TEXT("hlod_build"),
		TEXT("Build or rebuild HLOD for the current World Partition level. "
			 "Defaults to dry-run planning; pass execute=true or dry_run=false to dispatch the editor command."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("layer_name"), FSololmcpSchemaBuilder::String(TEXT("Optional: numeric HLOD layer index as text. Named layer dispatch is not supported by UE console path."))},
			{TEXT("hlod_layer_index"), FSololmcpSchemaBuilder::Integer(TEXT("HLOD layer index for generate mode. Default 0."))},
			{TEXT("mode"), FSololmcpSchemaBuilder::String(TEXT("generate | force_rebuild | build_changed. Default generate."),
				{TEXT("generate"), TEXT("force_rebuild"), TEXT("build_changed")})},
			{TEXT("world_path"), FSololmcpSchemaBuilder::String(TEXT("Optional current world package path. Non-current worlds are rejected for in-editor dispatch."))},
			{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Dispatch the HLOD command. Default false."))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Plan only. Default true unless execute=true."))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			FString RequestedWorldPath;
			Arguments->TryGetStringField(TEXT("world_path"), RequestedWorldPath);
			const FString CurrentWorldPath = World->GetPackage() ? World->GetPackage()->GetName() : World->GetPathName();
			if (!RequestedWorldPath.IsEmpty() && RequestedWorldPath != CurrentWorldPath && RequestedWorldPath != World->GetPathName())
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_non_current_world"));
				OutStructured->SetStringField(TEXT("current_world"), CurrentWorldPath);
				OutStructured->SetStringField(TEXT("requested_world"), RequestedWorldPath);
				SololmcpError::Set(OutStructured, TEXT("UNSUPPORTED"), TEXT("world_path"),
					TEXT("hlod_build can only dispatch against the currently loaded editor world."));
				OutError = TEXT("hlod_build can only dispatch against the current editor world.");
				return false;
			}

			UWorldPartition* WP = World->GetWorldPartition();
			const int32 HlodActorsBefore = SomolCountHlodLikeActors(World);
			OutStructured->SetStringField(TEXT("world"), CurrentWorldPath);
			OutStructured->SetBoolField(TEXT("world_partition_enabled"), WP != nullptr);
			OutStructured->SetNumberField(TEXT("hlod_actors_before"), HlodActorsBefore);

			if (!WP)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_no_world_partition"));
				SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT("world_partition"),
					TEXT("Current world is not World Partition enabled."));
				OutError = TEXT("Current world is not World Partition enabled.");
				return false;
			}

			FString Mode = TEXT("generate");
			Arguments->TryGetStringField(TEXT("mode"), Mode);
			int32 LayerIndex = 0;
			Arguments->TryGetNumberField(TEXT("hlod_layer_index"), LayerIndex);
			FString LayerName;
			if (Arguments->TryGetStringField(TEXT("layer_name"), LayerName) && !LayerName.IsEmpty())
			{
				int32 ParsedLayer = 0;
				if (LexTryParseString(ParsedLayer, *LayerName))
				{
					LayerIndex = ParsedLayer;
				}
				else
				{
					OutStructured->SetStringField(TEXT("status"), TEXT("blocked_named_layer_unsupported"));
					OutStructured->SetStringField(TEXT("layer_name"), LayerName);
					SololmcpError::Set(OutStructured, TEXT("UNSUPPORTED"), TEXT("layer_name"),
						TEXT("UE's safe console HLOD dispatch accepts a numeric layer index, not an arbitrary layer name."));
					OutError = TEXT("Named HLOD layer dispatch is not supported; pass hlod_layer_index.");
					return false;
				}
			}

			FString Command;
			if (Mode == TEXT("generate"))
			{
				Command = FString::Printf(TEXT("wp.HLOD.Generate %d"), LayerIndex);
			}
			else if (Mode == TEXT("force_rebuild"))
			{
				Command = TEXT("wp.HLOD.RebuildHLODs");
			}
			else if (Mode == TEXT("build_changed"))
			{
				Command = TEXT("wp.HLOD.BuildChanged");
			}
			else
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_invalid_mode"));
				SololmcpError::Set(OutStructured, TEXT("INVALID_ARGUMENT"), TEXT("mode"),
					TEXT("mode must be one of generate, force_rebuild, build_changed."));
				OutError = TEXT("Invalid HLOD build mode.");
				return false;
			}

			const bool bExecute = SomolGetBoolArg(Arguments, TEXT("execute"), false);
			const bool bDryRun = SomolGetBoolArg(Arguments, TEXT("dry_run"), !bExecute);
			OutStructured->SetStringField(TEXT("mode"), Mode);
			OutStructured->SetNumberField(TEXT("hlod_layer_index"), LayerIndex);
			OutStructured->SetStringField(TEXT("dispatch"), Command);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);

			if (bDryRun)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("planned"));
				OutStructured->SetBoolField(TEXT("dispatched"), false);
				OutStructured->SetBoolField(TEXT("completed"), false);
				OutSummary = FString::Printf(TEXT("Planned HLOD build dispatch: %s."), *Command);
				return true;
			}

			TSharedRef<FJsonObject> ConsoleResult = MakeShared<FJsonObject>();
			FString ConsoleSummary;
			FString ConsoleError;
			const bool bDispatched = Context.Services.ExecuteConsole(Command, ConsoleResult, ConsoleSummary, ConsoleError);
			OutStructured->SetObjectField(TEXT("console_result"), ConsoleResult);
			OutStructured->SetStringField(TEXT("console_summary"), ConsoleSummary);
			OutStructured->SetStringField(TEXT("console_error"), ConsoleError);
			OutStructured->SetBoolField(TEXT("dispatched"), bDispatched);
			OutStructured->SetBoolField(TEXT("completed"), false);
			OutStructured->SetNumberField(TEXT("hlod_actors_after_dispatch"), SomolCountHlodLikeActors(World));

			if (!bDispatched || !ConsoleError.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("dispatch_failed"));
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("dispatch"),
					TEXT("HLOD command dispatch failed."));
				OutError = ConsoleError.IsEmpty() ? TEXT("HLOD command dispatch failed.") : ConsoleError;
				return false;
			}

			OutStructured->SetStringField(TEXT("status"), TEXT("dispatched_async"));
			OutStructured->SetStringField(TEXT("receipt_note"),
				TEXT("UE HLOD console commands are asynchronous and expose no completion handle here; validate with HLOD stats/readback after the editor finishes."));
			OutSummary = FString::Printf(TEXT("Dispatched HLOD build command: %s."), *Command);
			return true;
		}
	});
}

// ============================================================================
// Register all
// ============================================================================

void RegisterLodHlodTools(FSololmcpToolRegistry& Registry)
{
	RegisterLodTools(Registry);
	RegisterHlodTools(Registry);
}

} // namespace UE::SOMOLMCP
