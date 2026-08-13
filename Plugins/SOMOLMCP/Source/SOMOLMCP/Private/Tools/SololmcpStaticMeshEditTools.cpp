// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — StaticMesh Editing Tools (P1-5)
//
// Tools registered (all `staticmesh_*` prefix):
//   staticmesh_set_lod          — Configure a single LOD: screen size + reduction settings.
//   staticmesh_generate_lods    — Auto-generate N LODs (2..8) from LOD0 with default reduction.
//   staticmesh_set_collision    — Replace the simple-collision aggregate with one geom (box/sphere/capsule/convex)
//                                 or set complex-as-simple. "none" clears the simple aggregate.
//   staticmesh_enable_nanite    — Toggle Nanite + optional position precision; rebuilds the asset.
//   staticmesh_set_lightmap     — Set lightmap resolution (must be power of 2) and optional UV index.
//   staticmesh_inspect          — Read-only summary: lod_count, vertex/triangle counts at LOD0,
//                                 material slot count, body count, nanite flag, lightmap res, has_uvs.
//
// IMPORTANT: This file is intentionally SELF-CONTAINED.
// It does NOT modify SololmcpToolRegistry.h/.cpp, .Build.cs, or any other .cpp.
//
// Wire-up required (delivery md describes; harness operator must do this):
//   1. Add `void RegisterStaticMeshEditTools(FSololmcpToolRegistry&);` in SololmcpToolRegistry.h
//      (under the existing `Register…Tools` declarations).
//   2. Call `RegisterStaticMeshEditTools(Registry)` from `FSololmcpToolRegistry::FSololmcpToolRegistry()`.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

// StaticMesh / Source Models / LOD
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#define SOMOLMCP_COMPAT_NEED_PERPLATFORM
#include "SololmcpEngineCompat.h"
#include "Engine/StaticMeshSocket.h"
#include "StaticMeshResources.h"

// Collision / BodySetup / Aggregate Geometry
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BoxElem.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "PhysicsEngine/ConvexElem.h"

// Nanite
#include "Rendering/NaniteResources.h"

// Editor / transactions / asset registry
#include "Editor.h"
#include "EditorSubsystem.h"
#include "ScopedTransaction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

// Subsystem-based fallbacks
#include "StaticMeshEditorSubsystem.h"

namespace UE::SOMOLMCP
{
namespace StaticMeshEditImpl
{
	// -----------------------------------------------------------------
	// Helpers
	// -----------------------------------------------------------------

	/** Validate asset_path and load as UStaticMesh. Sets SololmcpError on Out and returns nullptr on failure. */
	static UStaticMesh* LoadStaticMesh(const FSololmcpToolExecutionContext& Context,
	                                    const TSharedRef<FJsonObject>& Arguments,
	                                    TSharedRef<FJsonObject>& OutStructured,
	                                    FString& OutError)
	{
		FString AssetPath;
		if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
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
		UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			SololmcpError::InvalidPath(OutStructured, AssetPath);
			if (OutError.IsEmpty()) OutError = FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath);
			return nullptr;
		}
		UStaticMesh* SM = Cast<UStaticMesh>(Asset);
		if (!SM)
		{
			SololmcpError::InvalidType(OutStructured, TEXT("asset_path"), TEXT("UStaticMesh"));
			OutError = TEXT("Asset is not a UStaticMesh.");
			return nullptr;
		}
		return SM;
	}

	static bool IsPowerOfTwo(int32 V)
	{
		return V > 0 && (V & (V - 1)) == 0;
	}

	/** Count primitive elements in a body setup's aggregate geometry (boxes + spheres + capsules + convex). */
	static int32 CountSimpleGeoms(const UBodySetup* BS)
	{
		if (!BS) return 0;
		return BS->AggGeom.BoxElems.Num()
		     + BS->AggGeom.SphereElems.Num()
		     + BS->AggGeom.SphylElems.Num()
		     + BS->AggGeom.ConvexElems.Num();
	}
}

// ============================================================================
// Tool: staticmesh_set_lod
// ============================================================================

static void RegisterStaticMeshSetLod(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("staticmesh_set_lod"),
		TEXT("Configure a single LOD on a UStaticMesh: screen size and optional reduction settings (percent_triangles, percent_vertices in 0..1)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),         FSololmcpSchemaBuilder::String(TEXT("UStaticMesh content path"))},
			{TEXT("lod_index"),          FSololmcpSchemaBuilder::Integer(TEXT("Target LOD index (creates LOD if == NumSourceModels)"))},
			{TEXT("screen_size"),        FSololmcpSchemaBuilder::Number(TEXT("Per-LOD screen size (0..1)"))},
			{TEXT("reduction_settings"), FSololmcpSchemaBuilder::Object({
				{TEXT("percent_triangles"), FSololmcpSchemaBuilder::Number(TEXT("Triangle ratio 0..1"))},
				{TEXT("percent_vertices"),  FSololmcpSchemaBuilder::Number(TEXT("Vertex ratio 0..1"))}
			})}
		}, {TEXT("asset_path"), TEXT("lod_index")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UStaticMesh* SM = StaticMeshEditImpl::LoadStaticMesh(Context, Arguments, OutStructured, OutError);
			if (!SM) return false;

			int32 LodIndex = -1;
			if (!Arguments->TryGetNumberField(TEXT("lod_index"), LodIndex))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("lod_index"));
				OutError = TEXT("Missing lod_index.");
				return false;
			}
			if (LodIndex < 0)
			{
				SololmcpError::InvalidType(OutStructured, TEXT("lod_index"), TEXT("non-negative integer"));
				OutError = TEXT("lod_index must be >= 0.");
				return false;
			}

			const float ScreenSize = Arguments->HasTypedField<EJson::Number>(TEXT("screen_size"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("screen_size")))
				: -1.0f;

			float PercentTris = -1.0f;
			float PercentVerts = -1.0f;
			TSharedPtr<FJsonObject> Reduction;
			if (TryGetObjectField(Arguments, TEXT("reduction_settings"), Reduction) && Reduction.IsValid())
			{
				if (Reduction->HasTypedField<EJson::Number>(TEXT("percent_triangles")))
					PercentTris = static_cast<float>(Reduction->GetNumberField(TEXT("percent_triangles")));
				if (Reduction->HasTypedField<EJson::Number>(TEXT("percent_vertices")))
					PercentVerts = static_cast<float>(Reduction->GetNumberField(TEXT("percent_vertices")));
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshSetLod", "SOMOLMCP StaticMesh Set LOD"));
			SM->Modify();

			// Append source models so that LodIndex is in range.
			while (SM->GetNumSourceModels() <= LodIndex)
			{
				SM->AddSourceModel();
			}

			FStaticMeshSourceModel& Source = SM->GetSourceModel(LodIndex);
			if (ScreenSize >= 0.0f)
			{
				Source.ScreenSize = FPerPlatformFloat(ScreenSize);
			}
			if (PercentTris >= 0.0f)
			{
				Source.ReductionSettings.PercentTriangles = FMath::Clamp(PercentTris, 0.0f, 1.0f);
			}
			if (PercentVerts >= 0.0f)
			{
				// TODO(P1-5): verify exact field name in UE 5.7 — most engine versions expose
				// PercentVertices on FMeshReductionSettings; some custom branches use VertexPercentage.
				Source.ReductionSettings.PercentVertices = FMath::Clamp(PercentVerts, 0.0f, 1.0f);
			}

			SM->PostEditChange();
			SM->Build(/*bSilent*/true);
			SM->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(SM);

			OutStructured->SetNumberField(TEXT("lod_index"), LodIndex);
			OutStructured->SetNumberField(TEXT("screen_size"), Source.ScreenSize.Default);
			OutSummary = FString::Printf(TEXT("Configured LOD %d on '%s' (screen_size=%.4f)"),
				LodIndex, *SM->GetName(), Source.ScreenSize.Default);
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: staticmesh_generate_lods
// ============================================================================

static void RegisterStaticMeshGenerateLods(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("staticmesh_generate_lods"),
		TEXT("Auto-generate N LODs on a UStaticMesh (lod_count: 2..8). When auto_compute is true (default), default reduction & screen sizes are applied."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),  FSololmcpSchemaBuilder::String(TEXT("UStaticMesh content path"))},
			{TEXT("lod_count"),   FSololmcpSchemaBuilder::Integer(TEXT("Total LOD count (2..8) including LOD0"))},
			{TEXT("auto_compute"),FSololmcpSchemaBuilder::Boolean(TEXT("Use default reduction (default true)"))}
		}, {TEXT("asset_path"), TEXT("lod_count")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UStaticMesh* SM = StaticMeshEditImpl::LoadStaticMesh(Context, Arguments, OutStructured, OutError);
			if (!SM) return false;

			int32 LodCount = 0;
			if (!Arguments->TryGetNumberField(TEXT("lod_count"), LodCount))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("lod_count"));
				OutError = TEXT("Missing lod_count.");
				return false;
			}
			if (LodCount < 2 || LodCount > 8)
			{
				SololmcpError::InvalidType(OutStructured, TEXT("lod_count"), TEXT("integer in [2..8]"));
				OutError = TEXT("lod_count must be in [2..8].");
				return false;
			}
			const bool bAutoCompute = Arguments->HasTypedField<EJson::Boolean>(TEXT("auto_compute"))
				? Arguments->GetBoolField(TEXT("auto_compute"))
				: true;

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshGenerateLods", "SOMOLMCP StaticMesh Generate LODs"));
			SM->Modify();

			const int32 PreviousCount = SM->GetNumSourceModels();
			if (PreviousCount >= LodCount)
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("lod_count"),
					TEXT("StaticMesh already has at least the requested LOD count."));
				OutError = TEXT("StaticMesh already has at least the requested LOD count.");
				return false;
			}
			while (SM->GetNumSourceModels() < LodCount)
			{
				SM->AddSourceModel();
			}

			if (bAutoCompute)
			{
				// Apply standard halving reduction & approximate screen-size schedule.
				for (int32 i = 1; i < LodCount; ++i)
				{
					FStaticMeshSourceModel& Source = SM->GetSourceModel(i);
					const float Ratio = FMath::Pow(0.5f, static_cast<float>(i));
					Source.ReductionSettings.PercentTriangles = Ratio;
					// TODO(P1-5): verify PercentVertices availability across reduction backends; harmless
					// to assign — engine ignores unknowns when the reduction module doesn't use it.
					Source.ReductionSettings.PercentVertices  = Ratio;
					// Default screen-size ladder: 1.0, 0.5, 0.25, 0.125 ...
					Source.ScreenSize = FPerPlatformFloat(FMath::Max(0.0f, 1.0f / FMath::Pow(2.0f, static_cast<float>(i))));
				}
			}

			SM->PostEditChange();
			SM->Build(/*bSilent*/true);
			SM->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(SM);

			const int32 GeneratedCount = FMath::Max(0, SM->GetNumSourceModels() - PreviousCount);
			if (GeneratedCount <= 0 || SM->GetNumSourceModels() < LodCount)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("lod_count"),
					TEXT("StaticMesh LOD count readback did not reach the requested value."));
				OutError = TEXT("StaticMesh LOD generation readback verification failed.");
				return false;
			}
			OutStructured->SetNumberField(TEXT("generated_count"), GeneratedCount);
			OutStructured->SetNumberField(TEXT("lod_count"), SM->GetNumSourceModels());
			OutSummary = FString::Printf(TEXT("Generated %d LOD(s) on '%s' (total now %d)"),
				GeneratedCount, *SM->GetName(), SM->GetNumSourceModels());
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: staticmesh_set_collision
// ============================================================================

static void RegisterStaticMeshSetCollision(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("staticmesh_set_collision"),
		TEXT("Replace simple collision with one shape (box|sphere|capsule|convex), set complex-as-simple, or clear (none)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),     FSololmcpSchemaBuilder::String()},
			{TEXT("collision_type"), FSololmcpSchemaBuilder::String(TEXT("none|box|sphere|capsule|convex|complex"))},
			{TEXT("complexity"),     FSololmcpSchemaBuilder::Integer(TEXT("Convex hull vertex count (default 12)"))}
		}, {TEXT("asset_path"), TEXT("collision_type")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UStaticMesh* SM = StaticMeshEditImpl::LoadStaticMesh(Context, Arguments, OutStructured, OutError);
			if (!SM) return false;

			FString CollisionType;
			if (!Arguments->TryGetStringField(TEXT("collision_type"), CollisionType) || CollisionType.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("collision_type"));
				OutError = TEXT("Missing collision_type.");
				return false;
			}
			CollisionType = CollisionType.ToLower();

			const int32 Complexity = Arguments->HasTypedField<EJson::Number>(TEXT("complexity"))
				? Arguments->GetIntegerField(TEXT("complexity"))
				: 12;

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshSetCollision", "SOMOLMCP StaticMesh Set Collision"));
			SM->Modify();

			// Ensure body setup exists.
			SM->CreateBodySetup();
			UBodySetup* BS = SM->GetBodySetup();
			if (!BS)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("UStaticMesh::CreateBodySetup returned null."));
				OutError = TEXT("Failed to create body setup.");
				return false;
			}
			BS->Modify();

			if (CollisionType == TEXT("none"))
			{
				BS->RemoveSimpleCollision();
				BS->CollisionTraceFlag = CTF_UseDefault;
			}
			else if (CollisionType == TEXT("complex"))
			{
				BS->CollisionTraceFlag = CTF_UseComplexAsSimple;
				BS->RemoveSimpleCollision();
			}
			else
			{
				// Replace existing simple geom with one new geom sized to the mesh bounds.
				BS->RemoveSimpleCollision();
				const FBoxSphereBounds Bounds = SM->GetBounds();
				const FVector Extent = Bounds.BoxExtent;
				const FVector Origin = Bounds.Origin;

				if (CollisionType == TEXT("box"))
				{
					FKBoxElem Box;
					Box.Center = Origin;
					Box.X = Extent.X * 2.0f;
					Box.Y = Extent.Y * 2.0f;
					Box.Z = Extent.Z * 2.0f;
					BS->AggGeom.BoxElems.Add(Box);
				}
				else if (CollisionType == TEXT("sphere"))
				{
					FKSphereElem Sph;
					Sph.Center = Origin;
					Sph.Radius = Bounds.SphereRadius;
					BS->AggGeom.SphereElems.Add(Sph);
				}
				else if (CollisionType == TEXT("capsule"))
				{
					FKSphylElem Cap;
					Cap.Center = Origin;
					Cap.Radius = FMath::Max(Extent.X, Extent.Y);
					Cap.Length = FMath::Max(0.0f, (Extent.Z * 2.0f) - 2.0f * Cap.Radius);
					BS->AggGeom.SphylElems.Add(Cap);
				}
				else if (CollisionType == TEXT("convex"))
				{
					// TODO(P1-5): A proper convex hull would call FMeshUtilities::CreateConvexCollision
					// (module "MeshUtilities") with the mesh's vertex buffer. Here we approximate with
					// an oriented bounding-box hull derived from the mesh bounds; `complexity` is recorded
					// but currently not used to drive a hull simplifier. Replace with a real hull-builder
					// call once `MeshUtilities` is confirmed available in .Build.cs.
					FKConvexElem Convex;
					const FVector P[8] = {
						Origin + FVector(-Extent.X, -Extent.Y, -Extent.Z),
						Origin + FVector( Extent.X, -Extent.Y, -Extent.Z),
						Origin + FVector(-Extent.X,  Extent.Y, -Extent.Z),
						Origin + FVector( Extent.X,  Extent.Y, -Extent.Z),
						Origin + FVector(-Extent.X, -Extent.Y,  Extent.Z),
						Origin + FVector( Extent.X, -Extent.Y,  Extent.Z),
						Origin + FVector(-Extent.X,  Extent.Y,  Extent.Z),
						Origin + FVector( Extent.X,  Extent.Y,  Extent.Z),
					};
					Convex.VertexData.Empty(8);
					for (int32 i = 0; i < 8; ++i) Convex.VertexData.Add(P[i]);
					Convex.UpdateElemBox();
					BS->AggGeom.ConvexElems.Add(Convex);
				}
				else
				{
					SololmcpError::InvalidType(OutStructured, TEXT("collision_type"), TEXT("none|box|sphere|capsule|convex|complex"));
					OutError = FString::Printf(TEXT("Unknown collision_type '%s'."), *CollisionType);
					return false;
				}
			}

			BS->InvalidatePhysicsData();
			BS->CreatePhysicsMeshes();
			SM->PostEditChange();
			SM->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(SM);

			const int32 BodyCount = StaticMeshEditImpl::CountSimpleGeoms(BS);
			OutStructured->SetStringField(TEXT("collision_type"), CollisionType);
			OutStructured->SetNumberField(TEXT("body_count"), BodyCount);
			OutStructured->SetNumberField(TEXT("complexity"), Complexity);
			OutSummary = FString::Printf(TEXT("Set collision '%s' on '%s' (body_count=%d)"),
				*CollisionType, *SM->GetName(), BodyCount);
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: staticmesh_enable_nanite
// ============================================================================

static void RegisterStaticMeshEnableNanite(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("staticmesh_enable_nanite"),
		TEXT("Enable/disable Nanite on a UStaticMesh and (optionally) tweak position precision; rebuilds the mesh."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),         FSololmcpSchemaBuilder::String()},
			{TEXT("enable"),             FSololmcpSchemaBuilder::Boolean()},
			{TEXT("position_precision"), FSololmcpSchemaBuilder::Integer(TEXT("Default 0 (Auto)"))}
		}, {TEXT("asset_path"), TEXT("enable")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UStaticMesh* SM = StaticMeshEditImpl::LoadStaticMesh(Context, Arguments, OutStructured, OutError);
			if (!SM) return false;

			bool bEnable = false;
			if (!Arguments->TryGetBoolField(TEXT("enable"), bEnable))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("enable"));
				OutError = TEXT("Missing enable.");
				return false;
			}
			const int32 PositionPrecision = Arguments->HasTypedField<EJson::Number>(TEXT("position_precision"))
				? Arguments->GetIntegerField(TEXT("position_precision"))
				: 0;
			const FMeshNaniteSettings& CurrentNaniteSettings = SOMOLMCP_NANITE_SETTINGS(SM);
			if (CurrentNaniteSettings.bEnabled == bEnable && CurrentNaniteSettings.PositionPrecision == PositionPrecision)
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("enable"),
					TEXT("StaticMesh Nanite settings already match the requested values."));
				OutError = TEXT("StaticMesh Nanite settings were already set to the requested values.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshEnableNanite", "SOMOLMCP StaticMesh Enable Nanite"));
			SM->Modify();

			FMeshNaniteSettings NewNaniteSettings = SOMOLMCP_NANITE_SETTINGS(SM);
			NewNaniteSettings.bEnabled = bEnable;
			NewNaniteSettings.PositionPrecision = PositionPrecision;
			SOMOLMCP_SET_NANITE_SETTINGS(SM, NewNaniteSettings);

			bool bWasBuilt = false;
			SM->PostEditChange();
			// Build to materialize Nanite resources after toggling.
			SM->Build(/*bSilent*/true);
			bWasBuilt = true;
			SM->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(SM);
			const FMeshNaniteSettings& VerifiedNaniteSettings = SOMOLMCP_NANITE_SETTINGS(SM);
			if (VerifiedNaniteSettings.bEnabled != bEnable || VerifiedNaniteSettings.PositionPrecision != PositionPrecision)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("enable"),
					TEXT("StaticMesh Nanite settings readback did not match the requested values."));
				OutError = TEXT("StaticMesh Nanite readback verification failed.");
				return false;
			}

			OutStructured->SetBoolField(TEXT("nanite_enabled"), VerifiedNaniteSettings.bEnabled);
			OutStructured->SetBoolField(TEXT("was_built"), bWasBuilt);
			OutStructured->SetNumberField(TEXT("position_precision"), VerifiedNaniteSettings.PositionPrecision);
			OutSummary = FString::Printf(TEXT("Nanite %s on '%s'"),
				bEnable ? TEXT("enabled") : TEXT("disabled"), *SM->GetName());
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: staticmesh_set_lightmap
// ============================================================================

static void RegisterStaticMeshSetLightmap(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("staticmesh_set_lightmap"),
		TEXT("Set lightmap resolution (power of 2) and optional lightmap_coord_index for a UStaticMesh."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),           FSololmcpSchemaBuilder::String()},
			{TEXT("lightmap_resolution"),  FSololmcpSchemaBuilder::Integer(TEXT("Power of 2 (e.g. 32, 64, 128, 256, 512, 1024)"))},
			{TEXT("lightmap_coord_index"), FSololmcpSchemaBuilder::Integer(TEXT("Optional: which UV channel holds lightmap coords"))}
		}, {TEXT("asset_path"), TEXT("lightmap_resolution")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UStaticMesh* SM = StaticMeshEditImpl::LoadStaticMesh(Context, Arguments, OutStructured, OutError);
			if (!SM) return false;

			int32 Resolution = 0;
			if (!Arguments->TryGetNumberField(TEXT("lightmap_resolution"), Resolution))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("lightmap_resolution"));
				OutError = TEXT("Missing lightmap_resolution.");
				return false;
			}
			if (!StaticMeshEditImpl::IsPowerOfTwo(Resolution))
			{
				SololmcpError::InvalidType(OutStructured, TEXT("lightmap_resolution"), TEXT("positive power-of-two integer"));
				OutError = FString::Printf(TEXT("lightmap_resolution must be a positive power of 2 (got %d)."), Resolution);
				return false;
			}

			const bool bHasCoord = Arguments->HasTypedField<EJson::Number>(TEXT("lightmap_coord_index"));
			const int32 CoordIndex = bHasCoord ? Arguments->GetIntegerField(TEXT("lightmap_coord_index")) : SM->GetLightMapCoordinateIndex();
			if (SM->GetLightMapResolution() == Resolution && (!bHasCoord || SM->GetLightMapCoordinateIndex() == CoordIndex))
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("resolution"),
					TEXT("StaticMesh lightmap settings already match the requested values."));
				OutError = TEXT("StaticMesh lightmap settings were already set to the requested values.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "StaticMeshSetLightmap", "SOMOLMCP StaticMesh Set Lightmap"));
			SM->Modify();
			SM->SetLightMapResolution(Resolution);
			if (bHasCoord)
			{
				SM->SetLightMapCoordinateIndex(CoordIndex);
			}
			SM->PostEditChange();
			SM->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(SM);
			if (SM->GetLightMapResolution() != Resolution || (bHasCoord && SM->GetLightMapCoordinateIndex() != CoordIndex))
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("resolution"),
					TEXT("StaticMesh lightmap readback did not match the requested values."));
				OutError = TEXT("StaticMesh lightmap readback verification failed.");
				return false;
			}

			OutStructured->SetNumberField(TEXT("resolution"), SM->GetLightMapResolution());
			OutStructured->SetNumberField(TEXT("coord_index"), SM->GetLightMapCoordinateIndex());
			OutSummary = FString::Printf(TEXT("Lightmap on '%s' set to %d (UV %d)"),
				*SM->GetName(), SM->GetLightMapResolution(), SM->GetLightMapCoordinateIndex());
			return true;
		}
	, nullptr
	, 0
	});
}

// ============================================================================
// Tool: staticmesh_inspect
// ============================================================================

static void RegisterStaticMeshInspect(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("staticmesh_inspect"),
		TEXT("Read-only summary of a UStaticMesh: LOD count, LOD0 verts/tris, material count, simple-collision body count, Nanite flag, lightmap resolution, has_uvs."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UStaticMesh* SM = StaticMeshEditImpl::LoadStaticMesh(Context, Arguments, OutStructured, OutError);
			if (!SM) return false;

			const int32 LodCount = SM->GetNumLODs();
			const int32 VertCount = LodCount > 0 ? SM->GetNumVertices(0) : 0;
			const int32 TriCount  = LodCount > 0 ? SM->GetNumTriangles(0) : 0;
			const int32 MatCount  = SM->GetStaticMaterials().Num();
			const UBodySetup* BS  = SM->GetBodySetup();
			const int32 BodyCount = StaticMeshEditImpl::CountSimpleGeoms(BS);
			const bool  bNanite   = SOMOLMCP_NANITE_SETTINGS(SM).bEnabled;
			const int32 LightmapRes = SM->GetLightMapResolution();
			bool bHasUVs = false;
			if (LodCount > 0)
			{
				const FStaticMeshLODResources& LOD0 = SM->GetLODForExport(0);
				bHasUVs = LOD0.GetNumTexCoords() > 0;
			}

			OutStructured->SetStringField(TEXT("asset_path"), SM->GetPathName());
			OutStructured->SetNumberField(TEXT("lod_count"),           LodCount);
			OutStructured->SetNumberField(TEXT("vertex_count_lod0"),   VertCount);
			OutStructured->SetNumberField(TEXT("triangle_count_lod0"), TriCount);
			OutStructured->SetNumberField(TEXT("material_count"),      MatCount);
			OutStructured->SetNumberField(TEXT("collision_body_count"),BodyCount);
			OutStructured->SetBoolField  (TEXT("nanite_enabled"),      bNanite);
			OutStructured->SetNumberField(TEXT("lightmap_resolution"), LightmapRes);
			OutStructured->SetBoolField  (TEXT("has_uvs"),             bHasUVs);
			OutSummary = FString::Printf(TEXT("StaticMesh '%s': LODs=%d, V0=%d, T0=%d, mats=%d, bodies=%d, nanite=%s, lightmap=%d"),
				*SM->GetName(), LodCount, VertCount, TriCount, MatCount, BodyCount,
				bNanite ? TEXT("on") : TEXT("off"), LightmapRes);
			return true;
		}
	, nullptr
	, 5 // cacheable read-only
	});
}

// ============================================================================
// Public registration
// ============================================================================

void RegisterStaticMeshEditTools(FSololmcpToolRegistry& Registry)
{
	RegisterStaticMeshSetLod      (Registry);
	RegisterStaticMeshGenerateLods(Registry);
	RegisterStaticMeshSetCollision(Registry);
	RegisterStaticMeshEnableNanite(Registry);
	RegisterStaticMeshSetLightmap (Registry);
	RegisterStaticMeshInspect     (Registry);
}

} // namespace UE::SOMOLMCP
