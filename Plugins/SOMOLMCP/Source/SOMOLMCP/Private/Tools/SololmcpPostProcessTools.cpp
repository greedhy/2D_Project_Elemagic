// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.0 — Post-Processing Effects Tools
// Full CRUD for PostProcessVolume, material-based PP effects
//
// Tools registered:
//   pp_volume_list          — List all PostProcessVolume actors in the level
//   pp_volume_delete        — Delete a PostProcessVolume
//   pp_volume_set_bounds    — Set volume bounds (extent/location/unbound)
//   pp_volume_set_priority  — Set blend priority
//   pp_volume_add_material  — Add a material-based post process effect
//   pp_volume_remove_material — Remove a material-based post process effect
//   pp_volume_list_materials — List blendable materials on a volume
//   pp_material_create      — Create a post-process material asset
//   pp_material_set_bloom   — Configure bloom effect in a PP material
//   pp_material_set_dof     — Configure depth of field in a PP material
//   pp_material_set_vignette — Configure vignette effect
//   pp_material_delete      — Delete a PP material asset

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/PostProcessVolume.h"
#include "Components/PostProcessComponent.h"
#include "Components/BrushComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"
#include "FileHelpers.h"

namespace UE::SOMOLMCP
{

// ============================================================================
// PP Volume CRUD Tools
// ============================================================================

static void RegisterPostProcessVolumeTools(FSololmcpToolRegistry& Registry)
{
	// ---- pp_volume_list ----
	Registry.Register({
		TEXT("pp_volume_list"),
		TEXT("List all PostProcessVolume actors in the current level."),
		FSololmcpSchemaBuilder::Object({{}}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			TArray<TSharedPtr<FJsonValue>> VolumesJson;
			for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			{
				APostProcessVolume* PPVolume = *It;
				TSharedPtr<FJsonObject> VolObj = MakeShared<FJsonObject>();
				VolObj->SetStringField(TEXT("name"), PPVolume->GetActorLabel());
				VolObj->SetStringField(TEXT("path"), PPVolume->GetPathName());
				VolObj->SetBoolField(TEXT("unbound"), PPVolume->bUnbound);
				VolObj->SetNumberField(TEXT("priority"), PPVolume->Priority);
				VolObj->SetNumberField(TEXT("blend_radius"), PPVolume->BlendRadius);
				VolObj->SetNumberField(TEXT("blend_weight"), PPVolume->BlendWeight);

				FVector Loc = PPVolume->GetActorLocation();
				VolObj->SetStringField(TEXT("location"), Loc.ToString());
				FVector Extent = PPVolume->GetBrushComponent()->Bounds.BoxExtent;
				VolObj->SetStringField(TEXT("extent"), Extent.ToString());

				// List blendable materials
				TArray<TSharedPtr<FJsonValue>> BlendablesJson;
				if (PPVolume->Settings.WeightedBlendables.Array.Num() > 0)
				{
					for (const auto& Blendable : PPVolume->Settings.WeightedBlendables.Array)
					{
						if (IsValid(Blendable.Object.Get()))
						{
							TSharedPtr<FJsonObject> BlendObj = MakeShared<FJsonObject>();
							BlendObj->SetStringField(TEXT("material"), Blendable.Object->GetPathName());
							BlendObj->SetNumberField(TEXT("weight"), Blendable.Weight);
							BlendablesJson.Add(MakeShared<FJsonValueObject>(BlendObj));
						}
					}
				}
				VolObj->SetArrayField(TEXT("blendables"), BlendablesJson);
				VolObj->SetNumberField(TEXT("blendable_count"), BlendablesJson.Num());

				VolumesJson.Add(MakeShared<FJsonValueObject>(VolObj));
			}

			OutStructured->SetArrayField(TEXT("volumes"), VolumesJson);
			OutStructured->SetNumberField(TEXT("total"), VolumesJson.Num());
			OutSummary = FString::Printf(TEXT("Found %d PostProcessVolume actors"), VolumesJson.Num());
			return true;
		}
	});

	// ---- pp_volume_delete ----
	Registry.Register({
		TEXT("pp_volume_delete"),
		TEXT("Delete a PostProcessVolume actor from the level."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("PostProcessVolume actor name to delete"))}
		}, {TEXT("actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			APostProcessVolume* PPVolume = nullptr;
			for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			{
				if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
				{
					PPVolume = *It;
					break;
				}
			}
			if (!PPVolume)
			{
				OutError = FString::Printf(TEXT("PostProcessVolume '%s' not found."), *ActorName);
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DeletePPVolume", "SOMOLMCP Delete PostProcessVolume"));
			if (!PPVolume->Destroy())
			{
				OutError = FString::Printf(TEXT("Destroy returned false for PostProcessVolume '%s'."), *ActorName);
				return false;
			}

			OutStructured->SetStringField(TEXT("deleted_actor"), ActorName);
			OutSummary = FString::Printf(TEXT("Deleted PostProcessVolume '%s'"), *ActorName);
			return true;
		}
	});

	// ---- pp_volume_set_bounds ----
	Registry.Register({
		TEXT("pp_volume_set_bounds"),
		TEXT("Set the bounds of a PostProcessVolume (extent, location, unbound state)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})},
			{TEXT("extent"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})},
			{TEXT("unbound"), FSololmcpSchemaBuilder::Boolean(TEXT("If true, volume affects entire scene"))}
		}, {TEXT("actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			APostProcessVolume* PPVolume = nullptr;
			for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			{
				if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
				{
					PPVolume = *It;
					break;
				}
			}
			if (!PPVolume)
			{
				OutError = FString::Printf(TEXT("PostProcessVolume '%s' not found."), *ActorName);
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SetPPBounds", "SOMOLMCP Set PP Volume Bounds"));
			bool bRequestedChange = false;
			bool bAppliedChange = false;
			bool bPartialSuccess = false;
			TArray<TSharedPtr<FJsonValue>> FailedChanges;

			if (Arguments->HasField(TEXT("unbound")))
			{
				bRequestedChange = true;
				const bool bNewUnbound = Arguments->GetBoolField(TEXT("unbound"));
				if (PPVolume->bUnbound != bNewUnbound)
				{
					PPVolume->Modify();
					PPVolume->bUnbound = bNewUnbound;
					bAppliedChange = true;
				}
			}

			if (const TSharedPtr<FJsonObject>* LocObjPtr; Arguments->TryGetObjectField(TEXT("location"), LocObjPtr))
			{
				const TSharedPtr<FJsonObject>& LocObj = *LocObjPtr;
				FVector NewLoc;
				NewLoc.X = static_cast<float>(LocObj->GetNumberField(TEXT("x")));
				NewLoc.Y = static_cast<float>(LocObj->GetNumberField(TEXT("y")));
				NewLoc.Z = static_cast<float>(LocObj->GetNumberField(TEXT("z")));
				bRequestedChange = true;
				if (!PPVolume->GetActorLocation().Equals(NewLoc))
				{
					PPVolume->Modify();
					if (PPVolume->SetActorLocation(NewLoc))
					{
						bAppliedChange = true;
					}
					else
					{
						bPartialSuccess = true;
						FailedChanges.Add(MakeShared<FJsonValueString>(TEXT("location")));
					}
				}
			}

			if (const TSharedPtr<FJsonObject>* ExtObjPtr; Arguments->TryGetObjectField(TEXT("extent"), ExtObjPtr))
			{
				const TSharedPtr<FJsonObject>& ExtObj = *ExtObjPtr;
				FVector Extent;
				Extent.X = static_cast<float>(ExtObj->GetNumberField(TEXT("x")));
				Extent.Y = static_cast<float>(ExtObj->GetNumberField(TEXT("y")));
				Extent.Z = static_cast<float>(ExtObj->GetNumberField(TEXT("z")));
				// Scale the brush component to match extent
				bRequestedChange = true;
				if (PPVolume->GetBrushComponent())
				{
					const FVector NewScale(Extent.X / 50.0, Extent.Y / 50.0, Extent.Z / 50.0);
					if (!PPVolume->GetBrushComponent()->GetComponentScale().Equals(NewScale))
					{
						PPVolume->GetBrushComponent()->Modify();
						PPVolume->GetBrushComponent()->SetWorldScale3D(NewScale);
						if (PPVolume->GetBrushComponent()->GetComponentScale().Equals(NewScale))
						{
							bAppliedChange = true;
						}
						else
						{
							bPartialSuccess = true;
							FailedChanges.Add(MakeShared<FJsonValueString>(TEXT("extent")));
						}
					}
				}
				else
				{
					bPartialSuccess = true;
					FailedChanges.Add(MakeShared<FJsonValueString>(TEXT("extent_no_brush_component")));
				}
			}

			if (!bRequestedChange)
			{
				OutError = TEXT("No bounds fields were requested.");
				return false;
			}
			if (!bAppliedChange)
			{
				OutStructured->SetArrayField(TEXT("failed_changes"), FailedChanges);
				OutError = TEXT("No PostProcessVolume bounds value changed.");
				return false;
			}

			PPVolume->PostEditChange();

			OutStructured->SetStringField(TEXT("actor_name"), ActorName);
			OutStructured->SetBoolField(TEXT("unbound"), PPVolume->bUnbound);
			OutStructured->SetStringField(TEXT("location"), PPVolume->GetActorLocation().ToString());
			if (bPartialSuccess)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("partial_success"));
				OutStructured->SetArrayField(TEXT("failed_changes"), FailedChanges);
			}
			OutSummary = FString::Printf(TEXT("Updated bounds for PostProcessVolume '%s'"), *ActorName);
			return true;
		}
	});

	// ---- pp_volume_set_priority ----
	Registry.Register({
		TEXT("pp_volume_set_priority"),
		TEXT("Set the blend priority of a PostProcessVolume."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("priority"), FSololmcpSchemaBuilder::Number(TEXT("Blend priority (higher = evaluated later, takes precedence)"))}
		}, {TEXT("actor_name"), TEXT("priority")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));
			const float Priority = static_cast<float>(Arguments->GetNumberField(TEXT("priority")));

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			APostProcessVolume* PPVolume = nullptr;
			for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			{
				if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
				{
					PPVolume = *It;
					break;
				}
			}
			if (!PPVolume)
			{
				OutError = FString::Printf(TEXT("PostProcessVolume '%s' not found."), *ActorName);
				return false;
			}

			if (FMath::IsNearlyEqual(PPVolume->Priority, Priority))
			{
				OutError = FString::Printf(TEXT("Priority is already %.1f for '%s'; no value changed."), Priority, *ActorName);
				return false;
			}
			PPVolume->Modify();
			PPVolume->Priority = Priority;
			PPVolume->PostEditChange();

			OutStructured->SetStringField(TEXT("actor_name"), ActorName);
			OutStructured->SetNumberField(TEXT("priority"), Priority);
			OutSummary = FString::Printf(TEXT("Set priority to %.1f for '%s'"), Priority, *ActorName);
			return true;
		}
	});

	// ---- pp_volume_add_material ----
	Registry.Register({
		TEXT("pp_volume_add_material"),
		TEXT("Add a material-based post process effect to a PostProcessVolume."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("PostProcessVolume actor name"))},
			{TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Post-process material asset path"))},
			{TEXT("weight"), FSololmcpSchemaBuilder::Number(TEXT("Blend weight (default 1.0)"))}
		}, {TEXT("actor_name"), TEXT("material_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));
			const FString MaterialPath = Arguments->GetStringField(TEXT("material_path"));
			const float Weight = Arguments->HasField(TEXT("weight")) ? static_cast<float>(Arguments->GetNumberField(TEXT("weight"))) : 1.0f;

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			UMaterialInterface* Material = Cast<UMaterialInterface>(StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *MaterialPath));
			if (!Material)
			{
				OutError = FString::Printf(TEXT("Material not found: %s"), *MaterialPath);
				return false;
			}

			APostProcessVolume* PPVolume = nullptr;
			for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			{
				if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
				{
					PPVolume = *It;
					break;
				}
			}
			if (!PPVolume)
			{
				OutError = FString::Printf(TEXT("PostProcessVolume '%s' not found."), *ActorName);
				return false;
			}

			FWeightedBlendable Blendable;
			Blendable.Object = Material;
			Blendable.Weight = Weight;

			const int32 BlendableCountBefore = PPVolume->Settings.WeightedBlendables.Array.Num();
			PPVolume->Modify();
			PPVolume->Settings.WeightedBlendables.Array.Add(Blendable);
			if (PPVolume->Settings.WeightedBlendables.Array.Num() != BlendableCountBefore + 1)
			{
				OutError = FString::Printf(TEXT("Failed to verify blendable append for '%s'."), *MaterialPath);
				return false;
			}
			PPVolume->PostEditChange();

			OutStructured->SetStringField(TEXT("actor_name"), ActorName);
			OutStructured->SetStringField(TEXT("material"), MaterialPath);
			OutStructured->SetNumberField(TEXT("weight"), Weight);
			OutStructured->SetNumberField(TEXT("total_blendables"), PPVolume->Settings.WeightedBlendables.Array.Num());
			OutSummary = FString::Printf(TEXT("Added PP material '%s' to '%s' (weight: %.1f)"), *MaterialPath, *ActorName, Weight);
			return true;
		}
	});

	// ---- pp_volume_remove_material ----
	Registry.Register({
		TEXT("pp_volume_remove_material"),
		TEXT("Remove a material-based post process effect from a PostProcessVolume."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String()},
			{TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Material to remove (or index number as string)"))}
		}, {TEXT("actor_name"), TEXT("material_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));
			const FString MaterialPath = Arguments->GetStringField(TEXT("material_path"));

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			APostProcessVolume* PPVolume = nullptr;
			for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			{
				if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
				{
					PPVolume = *It;
					break;
				}
			}
			if (!PPVolume) { OutError = TEXT("PostProcessVolume not found."); return false; }

			bool bRemoved = false;
			auto& Blendables = PPVolume->Settings.WeightedBlendables.Array;
			for (int32 i = Blendables.Num() - 1; i >= 0; --i)
			{
				if (IsValid(Blendables[i].Object.Get()) && Blendables[i].Object->GetPathName() == MaterialPath)
				{
					Blendables.RemoveAt(i);
					bRemoved = true;
					break;
				}
			}

			if (!bRemoved)
			{
				// Try by index
				if (FCString::IsNumeric(*MaterialPath))
				{
					int32 Idx = FCString::Atoi(*MaterialPath);
					if (Idx >= 0 && Idx < Blendables.Num())
					{
						Blendables.RemoveAt(Idx);
						bRemoved = true;
					}
				}
			}

			PPVolume->PostEditChange();

			if (bRemoved)
			{
				OutStructured->SetStringField(TEXT("removed_material"), MaterialPath);
				OutStructured->SetNumberField(TEXT("remaining_blendables"), Blendables.Num());
				OutSummary = TEXT("Removed blendable from PostProcessVolume.");
				return true;
			}
			OutError = TEXT("Blendable not found.");
			return false;
		}
	});

	// ---- pp_volume_list_materials ----
	Registry.Register({
		TEXT("pp_volume_list_materials"),
		TEXT("List all blendable materials on a PostProcessVolume."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			APostProcessVolume* PPVolume = nullptr;
			for (TActorIterator<APostProcessVolume> It(World); It; ++It)
			{
				if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
				{
					PPVolume = *It;
					break;
				}
			}
			if (!PPVolume) { OutError = TEXT("PostProcessVolume not found."); return false; }

			TArray<TSharedPtr<FJsonValue>> MatsJson;
			const auto& Blendables = PPVolume->Settings.WeightedBlendables.Array;
			for (int32 i = 0; i < Blendables.Num(); ++i)
			{
				TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
				MatObj->SetNumberField(TEXT("index"), i);
				MatObj->SetStringField(TEXT("material"), IsValid(Blendables[i].Object.Get()) ? Blendables[i].Object->GetPathName() : TEXT("None"));
				MatObj->SetNumberField(TEXT("weight"), Blendables[i].Weight);
				MatsJson.Add(MakeShared<FJsonValueObject>(MatObj));
			}

			// Also list built-in PP settings
			// UE 5.7: ChromaticAberrationIntensity removed from FPostProcessSettings
			TSharedPtr<FJsonObject> SettingsObj = MakeShared<FJsonObject>();
			SettingsObj->SetNumberField(TEXT("bloom_intensity"), PPVolume->Settings.BloomIntensity);
			SettingsObj->SetNumberField(TEXT("bloom_threshold"), PPVolume->Settings.BloomThreshold);
			SettingsObj->SetNumberField(TEXT("exposure_compensation"), PPVolume->Settings.AutoExposureBias);
			SettingsObj->SetNumberField(TEXT("vignette_intensity"), PPVolume->Settings.VignetteIntensity);
			SettingsObj->SetNumberField(TEXT("depth_of_field_fstop"), PPVolume->Settings.DepthOfFieldFstop);
			SettingsObj->SetNumberField(TEXT("motion_blur_amount"), PPVolume->Settings.MotionBlurAmount);
			SettingsObj->SetNumberField(TEXT("film_grain_intensity"), PPVolume->Settings.FilmGrainIntensity);
			// ChromaticAberrationIntensity removed in UE 5.7
			SettingsObj->SetNumberField(TEXT("ambient_occlusion_intensity"), PPVolume->Settings.AmbientOcclusionIntensity);

			OutStructured->SetArrayField(TEXT("blendables"), MatsJson);
			OutStructured->SetObjectField(TEXT("builtin_settings"), SettingsObj);
			OutStructured->SetNumberField(TEXT("total_blendables"), MatsJson.Num());
			OutSummary = FString::Printf(TEXT("PostProcessVolume '%s' has %d blendable materials"), *ActorName, MatsJson.Num());
			return true;
		}
	});
}

// ============================================================================
// PP Material Tools
// ============================================================================

static void RegisterPostProcessMaterialTools(FSololmcpToolRegistry& Registry)
{
	// UE 5.7: pp_material_create uses new API. bIsPostProcessMaterial removed;
	// ShadingModel enum changed; SetBlendableLocation removed. Use UMaterial::bIsPostProcessMaterial
	// through EBlendableLocation on the PostProcessVolume instead.

	// ---- pp_material_create ----
	Registry.Register({
		TEXT("pp_material_create"),
		TEXT("Create a new post-process material asset with the correct domain."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Material asset path, e.g., /Game/PostProcess/M_PP_Custom"))},
			{TEXT("blend_location"), FSololmcpSchemaBuilder::String(TEXT("Blend location: 'before_tonemapping' (default), 'after_tonemapping', 'before_translucency', 'replacing_tonemapper'"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			const FString BlendLocation = Arguments->HasField(TEXT("blend_location")) ? Arguments->GetStringField(TEXT("blend_location")) : TEXT("before_tonemapping");
			if (Context.Services.AssetExists(AssetPath))
			{
				OutError = FString::Printf(TEXT("Asset already exists: %s. Use a different asset_path or delete it first."), *AssetPath);
				return false;
			}

			UPackage* Package = CreatePackage(*AssetPath);
			if (!Package)
			{
				OutError = TEXT("Failed to create package.");
				return false;
			}

			UMaterial* Material = NewObject<UMaterial>(Package, UMaterial::StaticClass(), *FPackageName::GetLongPackageAssetName(AssetPath), RF_Public | RF_Standalone);
			if (!Material)
			{
				OutError = TEXT("Failed to create material.");
				return false;
			}

			// UE 5.7: Set shading model to Unlit (EMaterialShadingModel::MSM_Unlit)
			Material->SetShadingModel(EMaterialShadingModel::MSM_Unlit);

			// UE 5.7: SetMaterialDomain, SetUsageFlag, and EMaterialUsageFlags removed.
			// Mark the material as post-process via the MaterialDomain property if available.
			// Note: In UE 5.7, post-process materials are identified by their blend mode and domain settings.
			// The material must be configured in the material editor to be a post-process material.

			// Note: Blendable location is set when adding to PostProcessVolume, not on the material itself
			FString MappedBlend = BlendLocation;
			if (BlendLocation == TEXT("before_tonemapping"))
			{
				MappedBlend = TEXT("BL_BeforeTranslucency");
			}
			else if (BlendLocation == TEXT("after_tonemapping"))
			{
				MappedBlend = TEXT("BL_AfterTonemapping");
			}
			else if (BlendLocation == TEXT("replacing_tonemapper"))
			{
				MappedBlend = TEXT("BL_ReplacingTonemapper");
			}

			Material->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Material);

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("blend_location"), MappedBlend);
			OutStructured->SetStringField(TEXT("status"), TEXT("partial_success"));
			OutStructured->SetStringField(TEXT("note"), TEXT("Material asset was created, but post-process material domain/blendable settings were not verified in this UE branch."));
			OutSummary = FString::Printf(TEXT("Created material asset '%s' but did not verify PP domain setup (blend request: %s)"), *AssetPath, *MappedBlend);
			return true;
		}
	});

	// ---- pp_material_set_bloom ----
	// UE 5.7: AddParameterToExpression removed from UMaterial.
	// Use FMaterialParameterInfo + UMaterial::AddScalarParameter/GetScalarParameter instead,
	// or create parameters via the material editor infrastructure.
	Registry.Register({
		TEXT("pp_material_set_bloom"),
		TEXT("Configure bloom effect parameters on a post-process material. Stores parameter names for later use with Material Instance."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Post-process material asset path"))},
			{TEXT("intensity"), FSololmcpSchemaBuilder::Number(TEXT("Bloom intensity (default 1.0)"))},
			{TEXT("threshold"), FSololmcpSchemaBuilder::Number(TEXT("Bloom threshold (default 1.0)"))},
			{TEXT("tint_r"), FSololmcpSchemaBuilder::Number(TEXT("Tint red channel (default 1.0)"))},
			{TEXT("tint_g"), FSololmcpSchemaBuilder::Number(TEXT("Tint green channel (default 1.0)"))},
			{TEXT("tint_b"), FSololmcpSchemaBuilder::Number(TEXT("Tint blue channel (default 1.0)"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Material) return false;

			const double Intensity = Arguments->HasField(TEXT("intensity")) ? Arguments->GetNumberField(TEXT("intensity")) : 1.0;
			const double Threshold = Arguments->HasField(TEXT("threshold")) ? Arguments->GetNumberField(TEXT("threshold")) : 1.0;
			const double TintR = Arguments->HasField(TEXT("tint_r")) ? Arguments->GetNumberField(TEXT("tint_r")) : 1.0;
			const double TintG = Arguments->HasField(TEXT("tint_g")) ? Arguments->GetNumberField(TEXT("tint_g")) : 1.0;
			const double TintB = Arguments->HasField(TEXT("tint_b")) ? Arguments->GetNumberField(TEXT("tint_b")) : 1.0;

			// UE 5.7: SetScalarParameterDefaultValues removed.
			// Parameters are now set via static parameter storage or material instance.
			// Store parameter values as metadata for later use with Material Instance.
			TSharedRef<FJsonObject> ParamsJson = MakeShared<FJsonObject>();
			ParamsJson->SetNumberField(TEXT("BloomIntensity"), Intensity);
			ParamsJson->SetNumberField(TEXT("BloomThreshold"), Threshold);
			ParamsJson->SetNumberField(TEXT("BloomTintR"), TintR);
			ParamsJson->SetNumberField(TEXT("BloomTintG"), TintG);
			ParamsJson->SetNumberField(TEXT("BloomTintB"), TintB);

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("intensity"), Intensity);
			OutStructured->SetNumberField(TEXT("threshold"), Threshold);
			OutStructured->SetStringField(TEXT("tint"), FString::Printf(TEXT("(%.2f, %.2f, %.2f)"), TintR, TintG, TintB));
			OutStructured->SetStringField(TEXT("status"), TEXT("not_applied"));
			OutStructured->SetStringField(TEXT("note"), TEXT("Bloom parameter authoring is not implemented for UMaterial in this UE branch; no material value was changed."));
			OutError = TEXT("No bloom parameters were written.");
			return false;
		}
	});

	// ---- pp_material_set_dof ----
	Registry.Register({
		TEXT("pp_material_set_dof"),
		TEXT("Configure depth of field parameters on a post-process material."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Post-process material asset path"))},
			{TEXT("focal_distance"), FSololmcpSchemaBuilder::Number(TEXT("Focal distance in cm (default 1000)"))},
			{TEXT("focal_region"), FSololmcpSchemaBuilder::Number(TEXT("Focal region in cm (default 200)"))},
			{TEXT("near_transition"), FSololmcpSchemaBuilder::Number(TEXT("Near transition range in cm (default 50)"))},
			{TEXT("far_transition"), FSololmcpSchemaBuilder::Number(TEXT("Far transition range in cm (default 500)"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Material) return false;

			const double FocalDistance = Arguments->HasField(TEXT("focal_distance")) ? Arguments->GetNumberField(TEXT("focal_distance")) : 1000.0;
			const double FocalRegion = Arguments->HasField(TEXT("focal_region")) ? Arguments->GetNumberField(TEXT("focal_region")) : 200.0;
			const double NearTransition = Arguments->HasField(TEXT("near_transition")) ? Arguments->GetNumberField(TEXT("near_transition")) : 50.0;
			const double FarTransition = Arguments->HasField(TEXT("far_transition")) ? Arguments->GetNumberField(TEXT("far_transition")) : 500.0;

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("focal_distance"), FocalDistance);
			OutStructured->SetNumberField(TEXT("focal_region"), FocalRegion);
			OutStructured->SetNumberField(TEXT("near_transition"), NearTransition);
			OutStructured->SetNumberField(TEXT("far_transition"), FarTransition);
			OutStructured->SetStringField(TEXT("status"), TEXT("not_applied"));
			OutError = TEXT("No DOF parameters were written.");
			return false;
		}
	});

	// ---- pp_material_set_vignette ----
	Registry.Register({
		TEXT("pp_material_set_vignette"),
		TEXT("Configure vignette effect parameters on a post-process material."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Post-process material asset path"))},
			{TEXT("intensity"), FSololmcpSchemaBuilder::Number(TEXT("Vignette intensity (default 0.5)"))},
			{TEXT("radius"), FSololmcpSchemaBuilder::Number(TEXT("Vignette radius (default 0.5)"))},
			{TEXT("softness"), FSololmcpSchemaBuilder::Number(TEXT("Vignette softness (default 0.5)"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Material) return false;

			const double Intensity = Arguments->HasField(TEXT("intensity")) ? Arguments->GetNumberField(TEXT("intensity")) : 0.5;
			const double Radius = Arguments->HasField(TEXT("radius")) ? Arguments->GetNumberField(TEXT("radius")) : 0.5;
			const double Softness = Arguments->HasField(TEXT("softness")) ? Arguments->GetNumberField(TEXT("softness")) : 0.5;

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("intensity"), Intensity);
			OutStructured->SetNumberField(TEXT("radius"), Radius);
			OutStructured->SetNumberField(TEXT("softness"), Softness);
			OutStructured->SetStringField(TEXT("status"), TEXT("not_applied"));
			OutError = TEXT("No vignette parameters were written.");
			return false;
		}
	});

	// ---- pp_material_delete ----
	Registry.Register({
		TEXT("pp_material_delete"),
		TEXT("Delete a post-process material asset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("PP material asset path"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Material) return false;

			TArray<UObject*> ObjectsToDelete;
			ObjectsToDelete.Add(Material);
			const int32 NumDeleted = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
			if (NumDeleted <= 0 || IsValid(Material))
			{
				OutStructured->SetNumberField(TEXT("deleted_count"), NumDeleted);
				OutError = FString::Printf(TEXT("Failed to verify deletion of PP material '%s'."), *AssetPath);
				return false;
			}

			OutStructured->SetStringField(TEXT("deleted_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("deleted_count"), NumDeleted);
			OutSummary = FString::Printf(TEXT("Deleted PP material '%s'"), *AssetPath);
			return true;
		}
	});
}

// ============================================================================
// Registration Entry Point
// ============================================================================

void RegisterPostProcessTools(FSololmcpToolRegistry& Registry)
{
	RegisterPostProcessVolumeTools(Registry);
	RegisterPostProcessMaterialTools(Registry);
}

} // namespace UE::SOMOLMCP
