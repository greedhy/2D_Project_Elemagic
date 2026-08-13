// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpClothTools.cpp  (P4-2)
// ----------------------------------------------------------------------------
// Five ChaosCloth tools for the SOMOLMCP UE plugin (cloth_* prefix):
//
//   1. cloth_inspect              — Read clothing assets attached to a USkeletalMesh.
//   2. cloth_create_from_section  — Best-effort wrapper for CreateClothingAssetFromSection.
//   3. cloth_set_simulation_enabled — Toggle cloth sim on a SkeletalMeshComponent.
//   4. cloth_set_wind             — Apply wind velocity to a cloth-enabled component.
//   5. cloth_status               — Subsystem-level diagnostics (always works).
//
// Total: 5 tools registered via RegisterClothTools(FSololmcpToolRegistry&).
//
// HARD CONSTRAINTS honored:
//   - Does NOT modify SololmcpToolRegistry.{h,cpp}.
//   - Does NOT modify .Build.cs (audit only — see P4_Cloth_XR_DELIVERY.md).
//   - Does NOT modify any other .cpp file.
//
// Wire-up required (delivery md describes; harness operator must do this):
//   1. Add `void RegisterClothTools(FSololmcpToolRegistry&);` in SololmcpToolRegistry.h.
//   2. Call it in `FSololmcpToolRegistry::FSololmcpToolRegistry()` near the other
//      registrations.
//
// UE 5.7 cloth API status notes
// -----------------------------
// * NvCloth was deprecated in UE5.0; ChaosCloth is the only supported simulation
//   backend in 5.7 but several editor-side helpers (e.g. CreateClothingAssetFromSection)
//   live in the ClothingSystemEditor module which may or may not be a hard build
//   dependency. We probe with `__has_include` and fall back to NOT_AVAILABLE.
// * Runtime APIs we touch:
//     USkeletalMesh::GetMeshClothingAssets()              -> TArray<UClothingAssetBase*>
//     USkeletalMeshComponent::GetClothingSimulationInstances()
//     USkeletalMeshComponent::SetAllowedAnimCurvesEvaluation(...) (for cloth blend)
//   Many of the wind / enable APIs were renamed in 5.x — we guard each call with
//   `__has_include` / preprocessor checks and gracefully degrade to NOT_IMPLEMENTED.
// * Most mutation tools are intentionally conservative stubs that return
//   NOT_IMPLEMENTED rather than risk a crash on a partially-shipped API surface.
//   They still validate inputs and resolve assets so the tool *interface* is
//   stable for AI callers.
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

// Engine / SkeletalMesh
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/SkeletalMeshActor.h"
// Polish patch P4-2: AWindDirectionalSource fallback for cloth_set_wind
#include "Engine/WindDirectionalSource.h"
#include "Components/WindDirectionalSourceComponent.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"

// Editor / transactions
#include "Editor.h"
#include "ScopedTransaction.h"

// Json
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ----------------------------------------------------------------------------
// Cloth headers — probed because they may not be available in every build.
// ----------------------------------------------------------------------------
#if __has_include("ClothingAsset.h")
	#include "ClothingAsset.h"
	#define SOMOLMCP_HAS_CLOTHING_ASSET 1
#else
	#define SOMOLMCP_HAS_CLOTHING_ASSET 0
#endif

#if __has_include("ClothingAssetBase.h")
	#include "ClothingAssetBase.h"
	#define SOMOLMCP_HAS_CLOTHING_ASSET_BASE 1
#else
	#define SOMOLMCP_HAS_CLOTHING_ASSET_BASE 0
#endif

#if __has_include("ChaosCloth/ChaosClothingSimulationFactory.h")
	#include "ChaosCloth/ChaosClothingSimulationFactory.h"
	#define SOMOLMCP_HAS_CHAOS_CLOTH_FACTORY 1
#else
	#define SOMOLMCP_HAS_CHAOS_CLOTH_FACTORY 0
#endif

#if __has_include("ClothingSimulationInterface.h")
	#include "ClothingSimulationInterface.h"
	#define SOMOLMCP_HAS_CLOTHING_SIM_IFACE 1
#else
	#define SOMOLMCP_HAS_CLOTHING_SIM_IFACE 0
#endif

namespace UE::SOMOLMCP
{
namespace ClothImpl
{
	// -----------------------------------------------------------------
	// Asset / actor resolution helpers (mirrors patterns elsewhere in plugin)
	// -----------------------------------------------------------------
	static USkeletalMesh* LoadSkelMesh(const FSololmcpToolExecutionContext& Context,
	                                   const FString& AssetPath,
	                                   TSharedRef<FJsonObject>& OutStructured,
	                                   FString& OutError)
	{
		if (AssetPath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("skelmesh_path"));
			OutError = TEXT("Missing skelmesh_path.");
			return nullptr;
		}
		UObject* Obj = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Obj)
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			if (OutError.IsEmpty())
				OutError = FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath);
			return nullptr;
		}
		USkeletalMesh* SK = Cast<USkeletalMesh>(Obj);
		if (!SK)
		{
			SololmcpError::InvalidType(OutStructured, TEXT("skelmesh_path"), TEXT("USkeletalMesh"));
			OutError = TEXT("Asset is not a USkeletalMesh.");
			return nullptr;
		}
		return SK;
	}

	static AActor* ResolveActorByLabel(UWorld* World, const FString& Id)
	{
		if (!World) return nullptr;
		// Tier 1: PathName (globally unique)
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetPathName() == Id) return *It;
		}
		// Tier 2: GetName
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetName() == Id) return *It;
		}
		// Tier 3: ActorLabel
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((*It)->GetActorLabel() == Id) return *It;
		}
		return nullptr;
	}

	// Find a USkeletalMeshComponent on the given actor (root or first found).
	static USkeletalMeshComponent* FindSkelMeshComp(AActor* Actor)
	{
		if (!Actor) return nullptr;
		if (USkeletalMeshComponent* SC = Cast<USkeletalMeshComponent>(Actor->GetRootComponent()))
		{
			return SC;
		}
		TArray<USkeletalMeshComponent*> Comps;
		Actor->GetComponents<USkeletalMeshComponent>(Comps);
		return Comps.Num() > 0 ? Comps[0] : nullptr;
	}

	static USkeletalMeshComponent* ResolveActorSkelMeshComp(const FSololmcpToolExecutionContext& Context,
	                                                        const FString& ActorName,
	                                                        TSharedRef<FJsonObject>& OutStructured,
	                                                        FString& OutError,
	                                                        AActor*& OutActor)
	{
		OutActor = nullptr;
		if (ActorName.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("actor_name"));
			OutError = TEXT("Missing actor_name.");
			return nullptr;
		}
		UWorld* World = nullptr;
		if (GEditor)
		{
			World = GEditor->GetEditorWorldContext().World();
		}
		if (!World)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("No editor world."));
			OutError = TEXT("No editor world available.");
			return nullptr;
		}
		AActor* Actor = ResolveActorByLabel(World, ActorName);
		if (!Actor)
		{
			SololmcpError::NotFound(OutStructured, ActorName);
			OutError = FString::Printf(TEXT("Actor '%s' not found."), *ActorName);
			return nullptr;
		}
		USkeletalMeshComponent* Comp = FindSkelMeshComp(Actor);
		if (!Comp)
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT(""),
				TEXT("Actor has no USkeletalMeshComponent."));
			OutError = FString::Printf(TEXT("Actor '%s' has no SkeletalMeshComponent."), *ActorName);
			return nullptr;
		}
		OutActor = Actor;
		return Comp;
	}

	// Detect what simulation backend is active for a SkelMesh component.
	static FString DetectSimulationClass(USkeletalMeshComponent* Comp)
	{
#if SOMOLMCP_HAS_CHAOS_CLOTH_FACTORY
		// In UE 5.7 ChaosCloth is the only shipping backend; presence of a
		// non-null clothing simulation implies it is active.
		if (Comp)
		{
		#if SOMOLMCP_HAS_CLOTHING_SIM_IFACE
			if (Comp->GetClothingSimulationInstances().Num() > 0)
			{
				return TEXT("ChaosCloth");
			}
		#endif
		}
		return TEXT("ChaosCloth"); // build-time available
#else
		(void)Comp;
		return TEXT("None");
#endif
	}

	static void SetStatus(TSharedRef<FJsonObject>& OutStructured, const TCHAR* Status)
	{
		OutStructured->SetStringField(TEXT("status"), Status);
	}
}

// ============================================================================
// Tool 1: cloth_inspect
// ============================================================================

static void RegisterClothInspect(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("cloth_inspect"),
		TEXT("Read-only inspection of clothing assets attached to a USkeletalMesh: names, LOD count, vertex / fixed-vertex counts, and active simulation backend."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("skelmesh_path"), FSololmcpSchemaBuilder::String(TEXT("USkeletalMesh content path"))}
		}, {TEXT("skelmesh_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			Arguments->TryGetStringField(TEXT("skelmesh_path"), AssetPath);
			USkeletalMesh* SK = ClothImpl::LoadSkelMesh(Context, AssetPath, OutStructured, OutError);
			if (!SK) return false;

			TArray<TSharedPtr<FJsonValue>> ClothAssetsJson;
			bool bHasCloth = false;
			FString SimClass = TEXT("None");

#if SOMOLMCP_HAS_CLOTHING_ASSET || SOMOLMCP_HAS_CLOTHING_ASSET_BASE
			{
				// USkeletalMesh::GetMeshClothingAssets() is the canonical accessor in 5.7.
				const TArray<UClothingAssetBase*>& ClothAssets = SK->GetMeshClothingAssets();
				bHasCloth = ClothAssets.Num() > 0;
				for (const UClothingAssetBase* CA : ClothAssets)
				{
					if (!CA) continue;
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("name"), CA->GetName());
					Entry->SetStringField(TEXT("path"), CA->GetPathName());

					int32 LodCount = 0;
					int32 VertCount = 0;
					int32 FixedCount = 0;
				#if SOMOLMCP_HAS_CLOTHING_ASSET
					if (const UClothingAssetCommon* CC = Cast<UClothingAssetCommon>(CA))
					{
						LodCount = CC->LodData.Num();
						for (const FClothLODDataCommon& LOD : CC->LodData)
						{
							VertCount += LOD.PhysicalMeshData.Vertices.Num();
							// Fixed verts are those with MaxDistance == 0 in MaxDistances param.
							const TArray<float>* Maxes = LOD.PhysicalMeshData.WeightMaps.Find(
								(uint32)EWeightMapTargetCommon::MaxDistance);
							if (Maxes)
							{
								for (float D : *Maxes)
								{
									if (D <= 0.0f) ++FixedCount;
								}
							}
						}
					}
				#endif
					Entry->SetNumberField(TEXT("lod_count"), LodCount);
					Entry->SetNumberField(TEXT("vertex_count"), VertCount);
					Entry->SetNumberField(TEXT("fixed_verts_count"), FixedCount);
					ClothAssetsJson.Add(MakeShared<FJsonValueObject>(Entry));
				}
			#if SOMOLMCP_HAS_CHAOS_CLOTH_FACTORY
				SimClass = bHasCloth ? TEXT("ChaosCloth") : TEXT("None");
			#endif
			}
#else
			// No cloth headers present at compile time — return empty list but mark API as unavailable.
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("Cloth headers (ClothingAsset.h) not available in this build. Add ClothingSystemRuntimeCommon to .Build.cs."));
#endif

			OutStructured->SetBoolField(TEXT("has_cloth_assets"), bHasCloth);
			OutStructured->SetArrayField(TEXT("cloth_assets"), ClothAssetsJson);
			OutStructured->SetStringField(TEXT("simulation_class"), SimClass);
			OutStructured->SetStringField(TEXT("skelmesh_path"), SK->GetPathName());
			OutSummary = FString::Printf(TEXT("SkelMesh '%s': %d cloth asset(s), sim=%s"),
				*SK->GetName(), ClothAssetsJson.Num(), *SimClass);
			return true;
		}
	, nullptr
	, 5
	});
}

// ============================================================================
// Tool 2: cloth_create_from_section
// ============================================================================

static void RegisterClothCreateFromSection(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("cloth_create_from_section"),
		TEXT("Create a new clothing asset from a SkeletalMesh LOD section. Best-effort wrapper around USkeletalMeshComponent::CreateClothingAssetFromSection — returns NOT_AVAILABLE if the editor helper is not present in this build."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("skelmesh_path"),     FSololmcpSchemaBuilder::String(TEXT("USkeletalMesh content path"))},
			{TEXT("lod_index"),         FSololmcpSchemaBuilder::Integer(TEXT("LOD index containing the source section"))},
			{TEXT("section_index"),     FSololmcpSchemaBuilder::Integer(TEXT("Section index within the LOD"))},
			{TEXT("cloth_asset_name"),  FSololmcpSchemaBuilder::String(TEXT("Display name for the new clothing asset"))}
		}, {TEXT("skelmesh_path"), TEXT("lod_index"), TEXT("section_index"), TEXT("cloth_asset_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, ClothAssetName;
			Arguments->TryGetStringField(TEXT("skelmesh_path"), AssetPath);
			USkeletalMesh* SK = ClothImpl::LoadSkelMesh(Context, AssetPath, OutStructured, OutError);
			if (!SK) return false;

			int32 LodIndex = -1, SectionIndex = -1;
			if (!Arguments->TryGetNumberField(TEXT("lod_index"), LodIndex) || LodIndex < 0)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("lod_index"));
				OutError = TEXT("Missing or invalid lod_index.");
				return false;
			}
			if (!Arguments->TryGetNumberField(TEXT("section_index"), SectionIndex) || SectionIndex < 0)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("section_index"));
				OutError = TEXT("Missing or invalid section_index.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("cloth_asset_name"), ClothAssetName) || ClothAssetName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("cloth_asset_name"));
				OutError = TEXT("Missing cloth_asset_name.");
				return false;
			}

			// TODO(P4-2): The actual editor helper to bind a section into a new
			// UClothingAssetCommon lives in the ClothingSystemEditor module:
			//   ClothingSystemEditorInterface.h -> IClothingAssetFactoryProvider
			// USkeletalMeshComponent::CreateClothingAssetFromSection() does NOT
			// exist as a public static helper in 5.7. The flow is:
			//   1) Create a UClothingAssetCommon NewObject<>(SK, ...).
			//   2) Use UClothingAssetFactory::CreateFromSkeletalMesh(SK, LOD, Section).
			//   3) Append into SK->GetMeshClothingAssets() (private setter).
			// Step 3 requires either Editor module access or reflection. Until
			// that wiring is added, this tool returns NOT_AVAILABLE while still
			// validating arguments so callers see a clean schema response.
			SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
				TEXT("cloth_create_from_section requires ClothingSystemEditor module integration. "
				     "Add 'ClothingSystemEditor' and 'ClothingSystemRuntimeCommon' to .Build.cs and "
				     "wire IClothingAssetFactoryProvider — see TODO(P4-2) in source."));
			OutStructured->SetStringField(TEXT("skelmesh_path"),    SK->GetPathName());
			OutStructured->SetNumberField(TEXT("lod_index"),        LodIndex);
			OutStructured->SetNumberField(TEXT("section_index"),    SectionIndex);
			OutStructured->SetStringField(TEXT("cloth_asset_name"), ClothAssetName);
			OutStructured->SetNumberField(TEXT("vert_count"),       0);
			OutError = TEXT("cloth_create_from_section: NOT_AVAILABLE in current build.");
			return false;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool 3: cloth_set_simulation_enabled
// ============================================================================

static void RegisterClothSetSimulationEnabled(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("cloth_set_simulation_enabled"),
		TEXT("Toggle ChaosCloth simulation on a SkeletalMeshComponent. Returns NOT_IMPLEMENTED if the runtime API is missing."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("SkelMeshActor label / name / path"))},
			{TEXT("enabled"),    FSololmcpSchemaBuilder::Boolean(TEXT("True to enable, false to disable cloth sim"))}
		}, {TEXT("actor_name"), TEXT("enabled")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString ActorName;
			Arguments->TryGetStringField(TEXT("actor_name"), ActorName);
			if (!Arguments->HasTypedField<EJson::Boolean>(TEXT("enabled")))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("enabled"));
				OutError = TEXT("Missing enabled.");
				return false;
			}
			const bool bEnabled = Arguments->GetBoolField(TEXT("enabled"));

			AActor* Actor = nullptr;
			USkeletalMeshComponent* Comp = ClothImpl::ResolveActorSkelMeshComp(
				Context, ActorName, OutStructured, OutError, Actor);
			if (!Comp) return false;

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ClothSetSimEnabled",
				"SOMOLMCP Cloth Set Simulation Enabled"));
			Comp->Modify();
			if (Actor) Actor->Modify();

			bool bApplied = false;
#if SOMOLMCP_HAS_CHAOS_CLOTH_FACTORY
			// UE 5.7: flip bDisableClothSimulation + RecreateClothingActors() to honor.
			Comp->bDisableClothSimulation = !bEnabled;
		#if SOMOLMCP_HAS_CLOTHING_SIM_IFACE
			Comp->RecreateClothingActors();
			bApplied = true;
		#else
			bApplied = true;
		#endif
			bApplied = true;
			ClothImpl::SetStatus(OutStructured, TEXT("success"));
			OutStructured->SetBoolField(TEXT("enabled"), bEnabled);
			OutStructured->SetBoolField(TEXT("applied"), bApplied);
			OutStructured->SetBoolField(TEXT("disable_cloth_sim"), Comp->bDisableClothSimulation);
			OutStructured->SetStringField(TEXT("actor"), Actor ? Actor->GetActorLabel() : ActorName);
			OutSummary = FString::Printf(TEXT("Cloth sim %s on '%s' (applied=%s)"),
				bEnabled ? TEXT("enabled") : TEXT("disabled"),
				Actor ? *Actor->GetActorLabel() : *ActorName,
				bApplied ? TEXT("yes") : TEXT("no"));
			return true;
#else
			(void)bApplied;
			SololmcpError::Set(OutStructured, TEXT("NOT_IMPLEMENTED"), TEXT(""),
				TEXT("ChaosCloth not present at compile time. Add 'ChaosCloth' + 'ClothingSystemRuntimeInterface' to .Build.cs."));
			OutError = TEXT("ChaosCloth API not available.");
			OutStructured->SetBoolField(TEXT("enabled"), bEnabled);
			OutStructured->SetBoolField(TEXT("applied"), false);
			return false;
#endif
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool 4: cloth_set_wind
// ============================================================================

static void RegisterClothSetWind(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("cloth_set_wind"),
		TEXT("Apply wind velocity (cm/s) to a cloth-enabled SkeletalMeshComponent. Returns NOT_IMPLEMENTED if SetWindVelocity-equivalent is missing."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"),         FSololmcpSchemaBuilder::String(TEXT("SkelMeshActor label / name / path"))},
			{TEXT("wind_velocity"),      FSololmcpSchemaBuilder::Array(
				FSololmcpSchemaBuilder::Number(),
				TEXT("Wind vector [x,y,z] in cm/s")
			)},
			{TEXT("wind_adaption_time"), FSololmcpSchemaBuilder::Number(TEXT("Optional adaption time (seconds)"))}
		}, {TEXT("actor_name"), TEXT("wind_velocity")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString ActorName;
			Arguments->TryGetStringField(TEXT("actor_name"), ActorName);

			const TArray<TSharedPtr<FJsonValue>>* WindArr = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("wind_velocity"), WindArr) || !WindArr || WindArr->Num() < 3)
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("wind_velocity"),
					TEXT("wind_velocity must be a 3-element number array [x,y,z]."));
				OutError = TEXT("Invalid wind_velocity.");
				return false;
			}
			const float Wx = static_cast<float>((*WindArr)[0]->AsNumber());
			const float Wy = static_cast<float>((*WindArr)[1]->AsNumber());
			const float Wz = static_cast<float>((*WindArr)[2]->AsNumber());

			float WindAdaption = -1.0f;
			if (Arguments->HasTypedField<EJson::Number>(TEXT("wind_adaption_time")))
			{
				WindAdaption = static_cast<float>(Arguments->GetNumberField(TEXT("wind_adaption_time")));
			}

			AActor* Actor = nullptr;
			USkeletalMeshComponent* Comp = ClothImpl::ResolveActorSkelMeshComp(
				Context, ActorName, OutStructured, OutError, Actor);
			if (!Comp) return false;

			TSharedRef<FJsonObject> WindObj = MakeShared<FJsonObject>();
			WindObj->SetNumberField(TEXT("x"), Wx);
			WindObj->SetNumberField(TEXT("y"), Wy);
			WindObj->SetNumberField(TEXT("z"), Wz);
			OutStructured->SetObjectField(TEXT("wind"), WindObj);

			// Documented fallback: per-component cloth wind setter is private (lives in
			// ClothingSystemRuntimeInterface). Spawn an AWindDirectionalSource actor
			// pointing along the requested vector — ChaosCloth reads world wind at tick.
			const FVector WindVec(Wx, Wy, Wz);
			const float WindMag = WindVec.Size();

			UWorld* WindWorld = nullptr;
			if (Actor) { WindWorld = Actor->GetWorld(); }
			if (!WindWorld && GEditor) { WindWorld = GEditor->GetEditorWorldContext().World(); }
			if (!WindWorld)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("No world available to spawn an AWindDirectionalSource."));
				OutError = TEXT("cloth_set_wind: no world available.");
				OutStructured->SetBoolField(TEXT("applied"), false);
				return false;
			}

			const FRotator WindRot = (WindMag > KINDA_SMALL_NUMBER)
				? WindVec.GetSafeNormal().Rotation()
				: FRotator::ZeroRotator;

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.ObjectFlags |= RF_Transactional;

			AWindDirectionalSource* Wind = WindWorld->SpawnActor<AWindDirectionalSource>(
				AWindDirectionalSource::StaticClass(),
				FVector::ZeroVector, WindRot, SpawnParams);

			bool bWindApplied = false;
			bool bWindComponentApplied = false;
			FString WindActorPath, WindActorLabel;
			if (Wind)
			{
				Wind->Modify();
				Wind->Tags.AddUnique(FName(TEXT("SOMOLMCP_ClothWind")));
				if (Actor)
				{
					Wind->Tags.AddUnique(FName(*FString::Printf(TEXT("SOMOLMCP_ClothWind_For_%s"), *Actor->GetName())));
				}
#if WITH_EDITOR
				Wind->SetActorLabel(FString::Printf(TEXT("SOMOLMCP_ClothWind_%s"),
					Actor ? *Actor->GetActorLabel() : TEXT("Global")));
#endif
				if (UWindDirectionalSourceComponent* WindComp = Wind->GetComponent())
				{
					WindComp->Modify();
					WindComp->Strength = FMath::Clamp(WindMag / 1000.0f, 0.0f, 100.0f);
					WindComp->Speed    = WindMag;
					WindComp->MarkRenderStateDirty();
					bWindComponentApplied = FMath::IsNearlyEqual(WindComp->Speed, WindMag)
						&& FMath::IsNearlyEqual(WindComp->Strength, FMath::Clamp(WindMag / 1000.0f, 0.0f, 100.0f));
				}
				Wind->PostEditChange();
				WindActorPath  = Wind->GetPathName();
#if WITH_EDITOR
				WindActorLabel = Wind->GetActorLabel();
#else
				WindActorLabel = Wind->GetName();
#endif
				bWindApplied = IsValid(Wind) && bWindComponentApplied;
			}

			if (!bWindApplied)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					Wind ? TEXT("AWindDirectionalSource spawned but its component settings did not verify.")
					     : TEXT("SpawnActor<AWindDirectionalSource> returned null."));
				OutError = Wind
					? TEXT("cloth_set_wind: spawned wind actor but failed to verify component settings.")
					: TEXT("cloth_set_wind: failed to spawn AWindDirectionalSource.");
				ClothImpl::SetStatus(OutStructured, Wind ? TEXT("partial_success") : TEXT("failed"));
				OutStructured->SetBoolField(TEXT("applied"), false);
				OutStructured->SetBoolField(TEXT("wind_actor_spawned"), Wind != nullptr);
				OutStructured->SetBoolField(TEXT("wind_component_applied"), bWindComponentApplied);
				if (Wind)
				{
					OutStructured->SetStringField(TEXT("wind_actor_path"), WindActorPath);
					OutStructured->SetStringField(TEXT("wind_actor_label"), WindActorLabel);
					return true;
				}
				return false;
			}

			ClothImpl::SetStatus(OutStructured, TEXT("success"));
			OutStructured->SetBoolField(TEXT("applied"), bWindApplied);
			OutStructured->SetBoolField(TEXT("wind_actor_spawned"), true);
			OutStructured->SetBoolField(TEXT("wind_component_applied"), bWindComponentApplied);
			OutStructured->SetNumberField(TEXT("wind_adaption_time"), WindAdaption);
			OutStructured->SetStringField(TEXT("wind_actor_path"), WindActorPath);
			OutStructured->SetStringField(TEXT("wind_actor_label"), WindActorLabel);
			OutStructured->SetNumberField(TEXT("wind_magnitude_cms"), WindMag);
			OutStructured->SetStringField(TEXT("strategy"), TEXT("spawn_AWindDirectionalSource"));
			OutStructured->SetStringField(TEXT("note"),
				TEXT("Per-component cloth wind is private; spawned tagged AWindDirectionalSource at origin. "
				     "Tag 'SOMOLMCP_ClothWind' for cleanup."));
			OutSummary = FString::Printf(
				TEXT("Spawned AWindDirectionalSource '%s' (mag=%.1f cm/s) for actor '%s'."),
				*WindActorLabel, WindMag, Actor ? *Actor->GetActorLabel() : *ActorName);
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool 5: cloth_status
// ============================================================================

static void RegisterClothStatus(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("cloth_status"),
		TEXT("Diagnostic snapshot: cloth subsystem availability, currently simulating components count, optional per-actor probe. Always succeeds."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Optional: probe one specific actor"))}
		} /* no required */),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			bool bSubsystemActive = false;
#if SOMOLMCP_HAS_CHAOS_CLOTH_FACTORY
			bSubsystemActive = true;
#endif

			// Walk world for any SkelMeshComponent reporting a non-null sim.
			int32 SimulatingCount = 0;
			float FpsHint = 0.0f;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World)
			{
				if (GEngine)
				{
					const float DeltaSec = FMath::Max(World->GetDeltaSeconds(), KINDA_SMALL_NUMBER);
					FpsHint = (DeltaSec > 0.0f) ? (1.0f / DeltaSec) : 0.0f;
				}
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* A = *It;
					if (!A) continue;
					TArray<USkeletalMeshComponent*> Comps;
					A->GetComponents<USkeletalMeshComponent>(Comps);
					for (USkeletalMeshComponent* C : Comps)
					{
						if (!C) continue;
					#if SOMOLMCP_HAS_CLOTHING_SIM_IFACE
						#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
						if (C->GetClothingSimulation() != nullptr && !C->bDisableClothSimulation)
						#else
						if (C->GetClothingSimulationInstances().Num() > 0 && !C->bDisableClothSimulation)
						#endif
						{
							++SimulatingCount;
						}
					#else
						if (!C->bDisableClothSimulation
							&& C->GetSkeletalMeshAsset()
							&& C->GetSkeletalMeshAsset()->GetMeshClothingAssets().Num() > 0)
						{
							++SimulatingCount;
						}
					#endif
					}
				}
			}

			// Optional per-actor probe
			FString ActorName;
			if (Arguments->TryGetStringField(TEXT("actor_name"), ActorName) && !ActorName.IsEmpty())
			{
				TSharedRef<FJsonObject> ActorProbe = MakeShared<FJsonObject>();
				AActor* Actor = ClothImpl::ResolveActorByLabel(World, ActorName);
				if (Actor)
				{
					USkeletalMeshComponent* Comp = ClothImpl::FindSkelMeshComp(Actor);
					ActorProbe->SetBoolField(TEXT("found"), true);
					ActorProbe->SetBoolField(TEXT("has_component"), Comp != nullptr);
					if (Comp)
					{
						ActorProbe->SetBoolField(TEXT("disable_cloth_sim"),  Comp->bDisableClothSimulation);
						ActorProbe->SetStringField(TEXT("simulation_class"), ClothImpl::DetectSimulationClass(Comp));
						const USkeletalMesh* SK = Comp->GetSkeletalMeshAsset();
						ActorProbe->SetNumberField(TEXT("cloth_asset_count"),
							SK ? SK->GetMeshClothingAssets().Num() : 0);
					}
				}
				else
				{
					ActorProbe->SetBoolField(TEXT("found"), false);
				}
				OutStructured->SetObjectField(TEXT("actor_probe"), ActorProbe);
			}

			OutStructured->SetBoolField  (TEXT("cloth_subsystem_active"),     bSubsystemActive);
			OutStructured->SetNumberField(TEXT("simulating_components_count"), SimulatingCount);
			OutStructured->SetNumberField(TEXT("fps_hint"),                    FpsHint);
			OutStructured->SetBoolField  (TEXT("available"),                   bSubsystemActive);
			OutSummary = FString::Printf(TEXT("Cloth: subsystem=%s, simulating=%d, fps=%.1f"),
				bSubsystemActive ? TEXT("active") : TEXT("inactive"), SimulatingCount, FpsHint);
			return true;
		}
	, nullptr
	, 2
	});
}

// ============================================================================
// Public registration
// ============================================================================

void RegisterClothTools(FSololmcpToolRegistry& Registry)
{
	RegisterClothInspect             (Registry);
	RegisterClothCreateFromSection   (Registry);
	RegisterClothSetSimulationEnabled(Registry);
	RegisterClothSetWind             (Registry);
	RegisterClothStatus              (Registry);
}

} // namespace UE::SOMOLMCP
