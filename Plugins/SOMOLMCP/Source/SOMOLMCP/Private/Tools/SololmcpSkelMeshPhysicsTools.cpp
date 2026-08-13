// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — SkeletalMesh + PhysicsAsset Editing Tools (P2-1)
//
// Tools registered:
//   skelmesh_set_lod              — Configure a USkeletalMesh LOD: screen size + reduction.
//   skelmesh_inspect              — Read-only summary of bones/LODs/materials/physics/skeleton/morphs.
//   physics_asset_create          — Create a UPhysicsAsset linked to a SkeletalMesh, auto-bodies.
//   physics_asset_add_body        — Add a USkeletalBodySetup for a single bone with chosen geom.
//   physics_asset_add_constraint  — Add a UPhysicsConstraintTemplate between parent and child bone.
//
// IMPORTANT: This file is intentionally SELF-CONTAINED.
// It does NOT modify SololmcpToolRegistry.h/.cpp, .Build.cs, or any other .cpp.
//
// Wire-up required (delivery md describes; harness operator must do this):
//   1. Add `void RegisterSkelMeshPhysicsTools(FSololmcpToolRegistry&);` in SololmcpToolRegistry.h.
//   2. Call it in `FSololmcpToolRegistry::FSololmcpToolRegistry()` near the other registrations.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

// SkeletalMesh
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "PerPlatformProperties.h"
#include "ReferenceSkeleton.h"

// PhysicsAsset
#include "PhysicsEngine/PhysicsAsset.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "PhysicsEngine/SkeletalBodySetup.h"
#else
// SkeletalBodySetup.h is 5.5+; USkeletalBodySetup is declared in PhysicsAsset.h before that.
#include "PhysicsEngine/PhysicsAsset.h"
#endif
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "PhysicsEngine/ConvexElem.h"

// Editor / transactions / asset registry
#include "Editor.h"
#include "ScopedTransaction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// PhysicsAssetUtils — UE 5.x developer module. Requires "PhysicsUtilities" in .Build.cs.
// If absent at compile time, see TODO(P2-1) blocks below for fallback.
#if __has_include("PhysicsAssetUtils.h")
	#include "PhysicsAssetUtils.h"
// FSkeletalMeshLODInfo is only forward-declared in Engine/SkinnedAsset.h; the definition
// lives in Engine/SkinnedAssetCommon.h on every supported version.
// It reaches this file transitively on 5.4+ but not on 5.3, where every member access
// on it reports "use of undefined type" instead of a missing include.
#include "Engine/SkinnedAssetCommon.h"
	#define SOMOLMCP_HAS_PHYSICS_ASSET_UTILS 1
#else
	#define SOMOLMCP_HAS_PHYSICS_ASSET_UTILS 0
#endif

namespace UE::SOMOLMCP
{
namespace SkelMeshPhysImpl
{
	// -----------------------------------------------------------------
	// Helpers
	// -----------------------------------------------------------------

	static USkeletalMesh* LoadSkelMesh(const FSololmcpToolExecutionContext& Context,
	                                    const FString& AssetPath,
	                                    TSharedRef<FJsonObject>& OutStructured,
	                                    FString& OutError)
	{
		if (AssetPath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}
		if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
			return nullptr;
		}
		UObject* Obj = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Obj)
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			if (OutError.IsEmpty()) OutError = FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath);
			return nullptr;
		}
		USkeletalMesh* SK = Cast<USkeletalMesh>(Obj);
		if (!SK)
		{
			SololmcpError::InvalidType(OutStructured, TEXT("asset_path"), TEXT("USkeletalMesh"));
			OutError = TEXT("Asset is not a USkeletalMesh.");
			return nullptr;
		}
		return SK;
	}

	static UPhysicsAsset* LoadPhysicsAsset(const FSololmcpToolExecutionContext& Context,
	                                       const FString& AssetPath,
	                                       TSharedRef<FJsonObject>& OutStructured,
	                                       FString& OutError)
	{
		if (AssetPath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}
		UObject* Obj = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Obj)
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			if (OutError.IsEmpty()) OutError = FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath);
			return nullptr;
		}
		UPhysicsAsset* PA = Cast<UPhysicsAsset>(Obj);
		if (!PA)
		{
			SololmcpError::InvalidType(OutStructured, TEXT("asset_path"), TEXT("UPhysicsAsset"));
			OutError = TEXT("Asset is not a UPhysicsAsset.");
			return nullptr;
		}
		return PA;
	}

	static int32 FindBodyIndexByBone(const UPhysicsAsset* PA, const FName& BoneName)
	{
		if (!PA) return INDEX_NONE;
		for (int32 i = 0; i < PA->SkeletalBodySetups.Num(); ++i)
		{
			const USkeletalBodySetup* BS = PA->SkeletalBodySetups[i];
			if (BS && BS->BoneName == BoneName) return i;
		}
		return INDEX_NONE;
	}
}

// ============================================================================
// Tool: skelmesh_set_lod
// ============================================================================

static void RegisterSkelMeshSetLod(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("skelmesh_set_lod"),
		TEXT("Configure a USkeletalMesh LOD: screen size + optional reduction settings (percent_triangles, percent_vertices)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),  FSololmcpSchemaBuilder::String(TEXT("USkeletalMesh content path"))},
			{TEXT("lod_index"),   FSololmcpSchemaBuilder::Integer()},
			{TEXT("screen_size"), FSololmcpSchemaBuilder::Number(TEXT("Per-LOD screen size"))},
			{TEXT("reduction"),   FSololmcpSchemaBuilder::Object({
				{TEXT("percent_triangles"), FSololmcpSchemaBuilder::Number()},
				{TEXT("percent_vertices"),  FSololmcpSchemaBuilder::Number()}
			})}
		}, {TEXT("asset_path"), TEXT("lod_index")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			USkeletalMesh* SK = SkelMeshPhysImpl::LoadSkelMesh(Context, AssetPath, OutStructured, OutError);
			if (!SK) return false;

			int32 LodIndex = -1;
			if (!Arguments->TryGetNumberField(TEXT("lod_index"), LodIndex) || LodIndex < 0)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("lod_index"));
				OutError = TEXT("Missing or invalid lod_index.");
				return false;
			}

			const float ScreenSize = Arguments->HasTypedField<EJson::Number>(TEXT("screen_size"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("screen_size")))
				: -1.0f;

			float PercentTris = -1.0f, PercentVerts = -1.0f;
			TSharedPtr<FJsonObject> Reduction;
			if (TryGetObjectField(Arguments, TEXT("reduction"), Reduction) && Reduction.IsValid())
			{
				if (Reduction->HasTypedField<EJson::Number>(TEXT("percent_triangles")))
					PercentTris = static_cast<float>(Reduction->GetNumberField(TEXT("percent_triangles")));
				if (Reduction->HasTypedField<EJson::Number>(TEXT("percent_vertices")))
					PercentVerts = static_cast<float>(Reduction->GetNumberField(TEXT("percent_vertices")));
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SkelMeshSetLod", "SOMOLMCP SkelMesh Set LOD"));
			SK->Modify();

			// Append LODInfo entries until LodIndex is in range. AddLODInfo() returns FSkeletalMeshLODInfo&.
			while (SK->GetLODNum() <= LodIndex)
			{
				SK->AddLODInfo();
			}

			FSkeletalMeshLODInfo* LodInfo = SK->GetLODInfo(LodIndex);
			if (!LodInfo)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("GetLODInfo returned null after AddLODInfo."));
				OutError = TEXT("Failed to obtain LOD info.");
				return false;
			}

			if (ScreenSize >= 0.0f)
			{
				// UE 5.7: ScreenSize is FPerPlatformFloat — assignment from float sets the default.
				LodInfo->ScreenSize = FPerPlatformFloat(ScreenSize);
			}
			if (PercentTris >= 0.0f)
			{
				LodInfo->ReductionSettings.NumOfTrianglesPercentage = FMath::Clamp(PercentTris, 0.0f, 1.0f);
			}
			if (PercentVerts >= 0.0f)
			{
				// TODO(P2-1): verify field name — UE 5.7 FSkeletalMeshOptimizationSettings exposes
				// NumOfVertPercentage (skeletal-side counterpart to PercentVertices). Adjust if rename.
				LodInfo->ReductionSettings.NumOfVertPercentage = FMath::Clamp(PercentVerts, 0.0f, 1.0f);
			}

			SK->PostEditChange();
			SK->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(SK);

			OutStructured->SetNumberField(TEXT("lod_index"), LodIndex);
			OutStructured->SetNumberField(TEXT("screen_size"), LodInfo->ScreenSize.GetDefault());
			OutSummary = FString::Printf(TEXT("Configured LOD %d on SkelMesh '%s' (screen_size=%.4f)"),
				LodIndex, *SK->GetName(), LodInfo->ScreenSize.GetDefault());
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: skelmesh_inspect
// ============================================================================

static void RegisterSkelMeshInspect(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("skelmesh_inspect"),
		TEXT("Read-only summary of a USkeletalMesh: bone_count, lod_count, material_count, physics_asset_path, skeleton_path, has_morphs."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			USkeletalMesh* SK = SkelMeshPhysImpl::LoadSkelMesh(Context, AssetPath, OutStructured, OutError);
			if (!SK) return false;

			const int32 BoneCount = SK->GetRefSkeleton().GetNum();
			const int32 LodCount  = SK->GetLODNum();
			const int32 MatCount  = SK->GetMaterials().Num();
			const UPhysicsAsset* PA = SK->GetPhysicsAsset();
			const USkeleton* Skel   = SK->GetSkeleton();
			const bool bHasMorphs   = SK->GetMorphTargets().Num() > 0;

			OutStructured->SetStringField(TEXT("asset_path"),         SK->GetPathName());
			OutStructured->SetNumberField(TEXT("bone_count"),         BoneCount);
			OutStructured->SetNumberField(TEXT("lod_count"),          LodCount);
			OutStructured->SetNumberField(TEXT("material_count"),     MatCount);
			OutStructured->SetStringField(TEXT("physics_asset_path"), PA   ? PA->GetPathName()   : TEXT(""));
			OutStructured->SetStringField(TEXT("skeleton_path"),      Skel ? Skel->GetPathName() : TEXT(""));
			OutStructured->SetBoolField  (TEXT("has_morphs"),         bHasMorphs);
			OutSummary = FString::Printf(TEXT("SkelMesh '%s': bones=%d, LODs=%d, mats=%d, morphs=%s"),
				*SK->GetName(), BoneCount, LodCount, MatCount, bHasMorphs ? TEXT("yes") : TEXT("no"));
			return true;
		}
	, nullptr
	, 5
	});
}

// ============================================================================
// Tool: physics_asset_create
// ============================================================================

static void RegisterPhysicsAssetCreate(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("physics_asset_create"),
		TEXT("Create a new UPhysicsAsset linked to a SkeletalMesh; auto-generate body setups for each bone."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),         FSololmcpSchemaBuilder::String(TEXT("Destination PA path, e.g. /Game/PA_Hero"))},
			{TEXT("skeletal_mesh_path"), FSololmcpSchemaBuilder::String(TEXT("USkeletalMesh source path"))}
		}, {TEXT("asset_path"), TEXT("skeletal_mesh_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString DestPath, SkelPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), DestPath) || DestPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("skeletal_mesh_path"), SkelPath) || SkelPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("skeletal_mesh_path"));
				OutError = TEXT("Missing skeletal_mesh_path.");
				return false;
			}

			USkeletalMesh* SK = SkelMeshPhysImpl::LoadSkelMesh(Context, SkelPath, OutStructured, OutError);
			if (!SK) return false;

			// Strip object-form suffix.
			FString PackagePath = DestPath;
			int32 DotIdx = INDEX_NONE;
			if (PackagePath.FindChar('.', DotIdx)) PackagePath = PackagePath.Left(DotIdx);
			if (!FPackageName::IsValidLongPackageName(PackagePath))
			{
				SololmcpError::InvalidPath(OutStructured, DestPath);
				OutError = FString::Printf(TEXT("Invalid destination package: %s"), *DestPath);
				return false;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			if (UEditorAssetLibrary::DoesAssetExist(DestPath))
			{
				SololmcpError::Set(OutStructured, TEXT("ALREADY_EXISTS"), TEXT("asset_path"),
					FString::Printf(TEXT("Destination PhysicsAsset already exists: %s"), *DestPath));
				OutError = FString::Printf(TEXT("Destination PhysicsAsset already exists: %s"), *DestPath);
				return false;
			}

			UPackage* Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("CreatePackage failed."));
				OutError = TEXT("CreatePackage failed.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "PhysicsAssetCreate", "SOMOLMCP Create PhysicsAsset"));

			UPhysicsAsset* PA = NewObject<UPhysicsAsset>(Package, *AssetName, RF_Public | RF_Standalone);
			if (!PA)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("NewObject<UPhysicsAsset> failed."));
				OutError = TEXT("Failed to create PhysicsAsset.");
				return false;
			}
			PA->Modify();

			// Bind to SkelMesh so future inspection sees the link.
			PA->SetPreviewMesh(SK);

			int32 BodyCount = 0;

#if SOMOLMCP_HAS_PHYSICS_ASSET_UTILS
			{
				// UE 5.x official auto-body generator.
				FPhysAssetCreateParams Params;
				FText OutErrorMessage;
				TArray<int32> NewBodyIndices;
				const bool bOK = FPhysicsAssetUtils::CreateFromSkeletalMesh(PA, SK, Params, OutErrorMessage);
				if (!bOK)
				{
					// TODO(P2-1): some engine branches expose a slightly different signature
					// (e.g. with NewBodyIndices out-array). If link fails, switch overloads here.
					SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT(""), OutErrorMessage.ToString());
					OutError = FString::Printf(TEXT("CreateFromSkeletalMesh failed: %s"), *OutErrorMessage.ToString());
					return false;
				}
				BodyCount = PA->SkeletalBodySetups.Num();
			}
#else
			{
				// Fallback path: build a minimal body per bone using a tiny sphere geom.
				// TODO(P2-1): this fallback runs only when "PhysicsUtilities" module is NOT in .Build.cs.
				// It is intentionally simple — replace with FPhysicsAssetUtils::CreateCollisionFromBone for
				// proper bone-following hulls once the module is added to PrivateDependencyModuleNames.
				const FReferenceSkeleton& RefSkel = SK->GetRefSkeleton();
				for (int32 BoneIdx = 0; BoneIdx < RefSkel.GetNum(); ++BoneIdx)
				{
					const FName BoneName = RefSkel.GetBoneName(BoneIdx);
					USkeletalBodySetup* BS = NewObject<USkeletalBodySetup>(PA, NAME_None, RF_Transactional);
					if (!BS) continue;
					BS->BoneName = BoneName;
					FKSphereElem Sphere;
					Sphere.Center = FVector::ZeroVector;
					Sphere.Radius = 4.0f;
					BS->AggGeom.SphereElems.Add(Sphere);
					PA->SkeletalBodySetups.Add(BS);
					++BodyCount;
				}
				PA->UpdateBodySetupIndexMap();
				PA->UpdateBoundsBodiesArray();
			}
#endif

			if (BodyCount <= 0 || PA->SkeletalBodySetups.Num() <= 0)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("body_count"),
					TEXT("PhysicsAsset creation produced no body setups."));
				OutError = TEXT("PhysicsAsset creation produced no body setups.");
				return false;
			}

			PA->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(PA);
			SololmcpWriteFlush::EnsureFlushed(PA);
			if (!UEditorAssetLibrary::SaveAsset(PA->GetPathName(), /*bOnlyIfIsDirty*/false))
			{
				SololmcpError::Set(OutStructured, TEXT("SAVE_FAILED"), TEXT("asset_path"),
					TEXT("UEditorAssetLibrary::SaveAsset returned false for the PhysicsAsset."));
				OutError = FString::Printf(TEXT("Failed to save PhysicsAsset '%s'."), *PA->GetPathName());
				return false;
			}
			if (!UEditorAssetLibrary::DoesAssetExist(PA->GetPathName()))
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("asset_path"),
					TEXT("Saved PhysicsAsset was not visible to the asset library."));
				OutError = FString::Printf(TEXT("PhysicsAsset save verification failed for '%s'."), *PA->GetPathName());
				return false;
			}

			// Optionally bind to the source skel mesh so its component picks it up by default.
			if (!SK->GetPhysicsAsset())
			{
				SK->Modify();
				SK->SetPhysicsAsset(PA);
				SK->PostEditChange();
				SK->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(SK);
				if (SK->GetPhysicsAsset() != PA)
				{
					SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("skeletal_mesh_path"),
						TEXT("SkeletalMesh physics asset readback did not match the created PhysicsAsset."));
					OutError = TEXT("SkeletalMesh physics asset readback verification failed.");
					return false;
				}
			}

			OutStructured->SetStringField(TEXT("asset_path"), PA->GetPathName());
			OutStructured->SetNumberField(TEXT("body_count"), BodyCount);
			OutSummary = FString::Printf(TEXT("Created PhysicsAsset '%s' (%d bodies)"), *PA->GetPathName(), BodyCount);
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: physics_asset_add_body
// ============================================================================

static void RegisterPhysicsAssetAddBody(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("physics_asset_add_body"),
		TEXT("Append a USkeletalBodySetup for a bone with chosen geom (box|sphere|capsule|convex). Idempotent on (bone)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("UPhysicsAsset path"))},
			{TEXT("bone_name"),  FSololmcpSchemaBuilder::String()},
			{TEXT("geom_type"),  FSololmcpSchemaBuilder::String(TEXT("box|sphere|capsule|convex (default sphere)"))},
			{TEXT("kinematic"),  FSololmcpSchemaBuilder::Boolean(TEXT("If true, sets PhysicsType to Kinematic"))}
		}, {TEXT("asset_path"), TEXT("bone_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, BoneNameStr;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("bone_name"), BoneNameStr) || BoneNameStr.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("bone_name"));
				OutError = TEXT("Missing bone_name.");
				return false;
			}
			UPhysicsAsset* PA = SkelMeshPhysImpl::LoadPhysicsAsset(Context, AssetPath, OutStructured, OutError);
			if (!PA) return false;

			const FName BoneName(*BoneNameStr);

			FString GeomType = TEXT("sphere");
			Arguments->TryGetStringField(TEXT("geom_type"), GeomType);
			GeomType = GeomType.ToLower();

			const bool bKinematic = Arguments->HasTypedField<EJson::Boolean>(TEXT("kinematic"))
				? Arguments->GetBoolField(TEXT("kinematic"))
				: false;

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "PAAddBody", "SOMOLMCP PhysicsAsset Add Body"));
			PA->Modify();

			// Reuse existing body for the same bone if present (idempotent).
			int32 BodyIndex = SkelMeshPhysImpl::FindBodyIndexByBone(PA, BoneName);
			USkeletalBodySetup* BS = (BodyIndex != INDEX_NONE) ? PA->SkeletalBodySetups[BodyIndex] : nullptr;
			if (!BS)
			{
				BS = NewObject<USkeletalBodySetup>(PA, NAME_None, RF_Transactional);
				if (!BS)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("NewObject<USkeletalBodySetup> failed."));
					OutError = TEXT("Failed to create body setup.");
					return false;
				}
				BS->BoneName = BoneName;
				BodyIndex = PA->SkeletalBodySetups.Add(BS);
			}
			BS->Modify();

			// Add a fresh geom of the requested type to the aggregate.
			if (GeomType == TEXT("box"))
			{
				FKBoxElem Box; Box.X = Box.Y = Box.Z = 8.0f;
				BS->AggGeom.BoxElems.Add(Box);
			}
			else if (GeomType == TEXT("capsule"))
			{
				FKSphylElem Cap; Cap.Radius = 4.0f; Cap.Length = 8.0f;
				BS->AggGeom.SphylElems.Add(Cap);
			}
			else if (GeomType == TEXT("convex"))
			{
				// TODO(P2-1): a real convex hull would be derived from the bound mesh segment for this bone.
				// Fall back to a unit-cube convex hull until MeshUtilities-driven hull generation is wired.
				FKConvexElem Convex;
				const float E = 4.0f;
				const FVector P[8] = {
					FVector(-E,-E,-E), FVector(E,-E,-E), FVector(-E,E,-E), FVector(E,E,-E),
					FVector(-E,-E, E), FVector(E,-E, E), FVector(-E,E, E), FVector(E,E, E),
				};
				Convex.VertexData.Empty(8);
				for (int32 i = 0; i < 8; ++i) Convex.VertexData.Add(P[i]);
				Convex.UpdateElemBox();
				BS->AggGeom.ConvexElems.Add(Convex);
			}
			else // sphere (default)
			{
				FKSphereElem Sphere; Sphere.Radius = 4.0f;
				BS->AggGeom.SphereElems.Add(Sphere);
			}

			if (bKinematic)
			{
				// TODO(P2-1): verify field name — USkeletalBodySetup::PhysicsType is the standard,
				// EPhysicsType::PhysType_Kinematic is the value in UE 5.7.
				BS->PhysicsType = EPhysicsType::PhysType_Kinematic;
			}

			BS->InvalidatePhysicsData();
			BS->CreatePhysicsMeshes();

			PA->UpdateBodySetupIndexMap();
			PA->UpdateBoundsBodiesArray();
			PA->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(PA);

			OutStructured->SetNumberField(TEXT("body_id"), BodyIndex);
			OutStructured->SetStringField(TEXT("bone"), BoneNameStr);
			OutStructured->SetStringField(TEXT("geom_type"), GeomType);
			OutStructured->SetBoolField  (TEXT("kinematic"), bKinematic);
			OutSummary = FString::Printf(TEXT("Body[%d] for bone '%s' (%s) on '%s'"),
				BodyIndex, *BoneNameStr, *GeomType, *PA->GetName());
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: physics_asset_add_constraint
// ============================================================================

static void RegisterPhysicsAssetAddConstraint(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("physics_asset_add_constraint"),
		TEXT("Add a UPhysicsConstraintTemplate between parent_bone and child_bone (constraint_type: swing_twist|linear)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),      FSololmcpSchemaBuilder::String()},
			{TEXT("parent_bone"),     FSololmcpSchemaBuilder::String()},
			{TEXT("child_bone"),      FSololmcpSchemaBuilder::String()},
			{TEXT("constraint_type"), FSololmcpSchemaBuilder::String(TEXT("swing_twist|linear (default swing_twist)"))}
		}, {TEXT("asset_path"), TEXT("parent_bone"), TEXT("child_bone")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, ParentBoneStr, ChildBoneStr;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("parent_bone"), ParentBoneStr) || ParentBoneStr.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("parent_bone"));
				OutError = TEXT("Missing parent_bone.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("child_bone"), ChildBoneStr) || ChildBoneStr.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("child_bone"));
				OutError = TEXT("Missing child_bone.");
				return false;
			}

			UPhysicsAsset* PA = SkelMeshPhysImpl::LoadPhysicsAsset(Context, AssetPath, OutStructured, OutError);
			if (!PA) return false;

			FString ConstraintType = TEXT("swing_twist");
			Arguments->TryGetStringField(TEXT("constraint_type"), ConstraintType);
			ConstraintType = ConstraintType.ToLower();

			const FName ParentBone(*ParentBoneStr);
			const FName ChildBone(*ChildBoneStr);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "PAAddConstraint", "SOMOLMCP PhysicsAsset Add Constraint"));
			PA->Modify();

			UPhysicsConstraintTemplate* CT = NewObject<UPhysicsConstraintTemplate>(PA, NAME_None, RF_Transactional);
			if (!CT)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("NewObject<UPhysicsConstraintTemplate> failed."));
				OutError = TEXT("Failed to create constraint template.");
				return false;
			}

			// Set the participating bone names + a sensible default profile name.
			CT->DefaultInstance.ConstraintBone1 = ChildBone;
			CT->DefaultInstance.ConstraintBone2 = ParentBone;
			CT->DefaultInstance.JointName       = ChildBone;
			CT->SetDefaultProfile(CT->DefaultInstance);

			if (ConstraintType == TEXT("linear"))
			{
				// Free linear, locked angular.
				CT->DefaultInstance.SetLinearXLimit(ELinearConstraintMotion::LCM_Free, 0.0f);
				CT->DefaultInstance.SetLinearYLimit(ELinearConstraintMotion::LCM_Free, 0.0f);
				CT->DefaultInstance.SetLinearZLimit(ELinearConstraintMotion::LCM_Free, 0.0f);
				CT->DefaultInstance.SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Locked, 0.0f);
				CT->DefaultInstance.SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0.0f);
				CT->DefaultInstance.SetAngularTwistLimit (EAngularConstraintMotion::ACM_Locked, 0.0f);
			}
			else
			{
				// swing_twist (default): locked linear, limited angular swing/twist.
				CT->DefaultInstance.SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
				CT->DefaultInstance.SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
				CT->DefaultInstance.SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
				CT->DefaultInstance.SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, 45.0f);
				CT->DefaultInstance.SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, 45.0f);
				CT->DefaultInstance.SetAngularTwistLimit (EAngularConstraintMotion::ACM_Limited, 30.0f);
			}

			const int32 ConstraintIndex = PA->ConstraintSetup.Add(CT);
			PA->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(PA);

			OutStructured->SetNumberField(TEXT("constraint_id"), ConstraintIndex);
			OutStructured->SetStringField(TEXT("parent_bone"), ParentBoneStr);
			OutStructured->SetStringField(TEXT("child_bone"),  ChildBoneStr);
			OutStructured->SetStringField(TEXT("constraint_type"), ConstraintType);
			OutSummary = FString::Printf(TEXT("Constraint[%d] %s -> %s (%s) on '%s'"),
				ConstraintIndex, *ParentBoneStr, *ChildBoneStr, *ConstraintType, *PA->GetName());
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Public registration
// ============================================================================

void RegisterSkelMeshPhysicsTools(FSololmcpToolRegistry& Registry)
{
	RegisterSkelMeshSetLod          (Registry);
	RegisterSkelMeshInspect         (Registry);
	RegisterPhysicsAssetCreate      (Registry);
	RegisterPhysicsAssetAddBody     (Registry);
	RegisterPhysicsAssetAddConstraint(Registry);
}

} // namespace UE::SOMOLMCP
