// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// GeometryScripting coverage — dynamic mesh sessions and queries.
//
// GeometryScripting is 509 BlueprintCallable entry points spread over ~20 function
// libraries, and SOMOLMCP covered about 1% of it: a handful of one-shot operations
// (boolean, remesh, simplify) that each opened a mesh, did one thing and threw it
// away. That shape is why coverage stayed low — every one of those 509 functions
// takes a UDynamicMesh* as its subject, so a per-operation tool has to re-import
// the asset each time, and chaining operations means re-importing per step.
//
// UDynamicMesh is a transient UObject with no asset path, which is exactly the case
// the session handle registry exists for. Opening a mesh once and addressing it by
// handle turns the whole library from "one tool per operation" into "open, operate
// N times, save" — the operations become batchable and the per-op asset round trip
// disappears.
//
// This first batch establishes the session: open from an asset, query, save back.
// The operation-dispatch layer builds on the same handles.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpObjectHandles.h"
#include "Services/SololmcpEditorServices.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Runtime/Launch/Resources/Version.h"

#ifndef SOMOLMCP_HAS_GEOMETRYSCRIPTING
#define SOMOLMCP_HAS_GEOMETRYSCRIPTING 0
#endif
// Engine floor, measured not assumed: 5.5 and above build clean; 5.4 and 5.3 do
// not. CopyMeshFromStaticMeshV2 and the rig hierarchy APIs used here arrived
// after 5.4, so the module being present further back proves nothing.
#if !((ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)))
#undef SOMOLMCP_HAS_GEOMETRYSCRIPTING
#define SOMOLMCP_HAS_GEOMETRYSCRIPTING 0
#endif

#if SOMOLMCP_HAS_GEOMETRYSCRIPTING
#include "UDynamicMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#endif

namespace UE::SOMOLMCP
{
#if !SOMOLMCP_HAS_GEOMETRYSCRIPTING

// L1: the engine does not ship GeometryScripting. Registering nothing is correct
// here — a caller then gets "no such tool" from tools/list rather than a tool that
// exists and always fails, which is the distinction the capability contract rests on.
void RegisterGeometryScriptTools(FSololmcpToolRegistry&)
{
}

#else
namespace GeometryScriptToolsPrivate
{
	/** Handle kind for dynamic meshes held across calls. */
	static const TCHAR* const DynamicMeshKind = TEXT("dynamic_mesh");

	inline UDynamicMesh* ResolveMesh(
		const TSharedRef<FJsonObject>& Args,
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		FString Handle;
		if (!Args->TryGetStringField(TEXT("mesh"), Handle) || Handle.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("mesh"));
			OutError = TEXT("Missing mesh handle.");
			return nullptr;
		}
		UDynamicMesh* Mesh = FSololmcpObjectHandles::Get().ResolveTyped<UDynamicMesh>(Handle, DynamicMeshKind);
		if (Mesh == nullptr)
		{
			SololmcpError::NotFound(OutStructured, Handle);
			OutStructured->SetStringField(TEXT("mesh"), Handle);
			OutStructured->SetStringField(TEXT("suggestion"),
				TEXT("Handles are per editor session. Run handle_list to see live meshes, or "
					 "geometry_mesh_open to load one."));
			OutError = FString::Printf(TEXT("Unknown dynamic mesh handle '%s'."), *Handle);
		}
		return Mesh;
	}

	inline FString OutcomeToString(const EGeometryScriptOutcomePins Outcome)
	{
		return Outcome == EGeometryScriptOutcomePins::Success ? TEXT("success") : TEXT("failure");
	}

	/**
	 * Map the lod_type argument onto EGeometryScriptLODType.
	 *
	 * This has to be an explicit parameter rather than an internal default, because
	 * the engine's own default (MaxAvailable) ignores LODIndex entirely: a caller who
	 * passed lod=2 and got LOD0 back would have no way to tell. source_model is the
	 * default here instead — it honours the index, and it is the LOD that
	 * CopyMeshToStaticMesh writes to, so read and write stay symmetric.
	 */
	inline bool ParseLodType(
		const TSharedRef<FJsonObject>& Args,
		EGeometryScriptLODType& OutType,
		const TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		FString Raw;
		if (!Args->TryGetStringField(TEXT("lod_type"), Raw) || Raw.IsEmpty())
		{
			OutType = EGeometryScriptLODType::SourceModel;
			return true;
		}
		if (Raw.Equals(TEXT("source_model"), ESearchCase::IgnoreCase))
		{
			OutType = EGeometryScriptLODType::SourceModel;
		}
		else if (Raw.Equals(TEXT("render_data"), ESearchCase::IgnoreCase))
		{
			OutType = EGeometryScriptLODType::RenderData;
		}
		else if (Raw.Equals(TEXT("hires_source_model"), ESearchCase::IgnoreCase))
		{
			OutType = EGeometryScriptLODType::HiResSourceModel;
		}
		else if (Raw.Equals(TEXT("max_available"), ESearchCase::IgnoreCase))
		{
			OutType = EGeometryScriptLODType::MaxAvailable;
		}
		else
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_PARAM"), TEXT("lod_type"),
				TEXT("Expected one of: source_model, render_data, hires_source_model, max_available."));
			OutError = FString::Printf(TEXT("Unknown lod_type '%s'."), *Raw);
			return false;
		}
		return true;
	}

	/** True when the LOD type resolves a single fixed mesh and ignores the index. */
	inline bool LodTypeIgnoresIndex(const EGeometryScriptLODType Type)
	{
		return Type == EGeometryScriptLODType::MaxAvailable
			|| Type == EGeometryScriptLODType::HiResSourceModel;
	}

	/** The lod_type spelling that would round-trip back into ParseLodType. */
	inline const TCHAR* Raw_LodTypeName(const EGeometryScriptLODType Type)
	{
		switch (Type)
		{
		case EGeometryScriptLODType::RenderData:        return TEXT("render_data");
		case EGeometryScriptLODType::HiResSourceModel:  return TEXT("hires_source_model");
		case EGeometryScriptLODType::MaxAvailable:      return TEXT("max_available");
		case EGeometryScriptLODType::SourceModel:
		default:                                        return TEXT("source_model");
		}
	}
} // namespace GeometryScriptToolsPrivate

void RegisterGeometryScriptTools(FSololmcpToolRegistry& Registry)
{
	using namespace GeometryScriptToolsPrivate;

	// ── geometry_mesh_open ─────────────────────────────────────────────────
	Registry.Register({
		TEXT("geometry_mesh_open"),
		TEXT("Load a Static Mesh asset into a dynamic mesh session and return a handle. Every "
			 "GeometryScripting operation acts on a dynamic mesh, so opening once and reusing the "
			 "handle avoids re-importing the asset for each step of a chain. Release it with "
			 "handle_release when the session is done — an open mesh is pinned against GC."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(
					TEXT("Object path of the Static Mesh, e.g. /Game/Meshes/SM_Rock.SM_Rock."))},
				{TEXT("lod"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("LOD index to read. Ignored when lod_type is max_available or "
							 "hires_source_model, which each name one specific mesh.")), 0)},
				{TEXT("lod_type"), FSololmcpSchemaBuilder::WithDefaultString(
					FSololmcpSchemaBuilder::WithEnum(
						FSololmcpSchemaBuilder::String(
							TEXT("Which mesh to read. source_model is the editable authoring mesh and "
								 "is what saving writes back to. render_data is the cooked mesh, split "
								 "at UV seams and hard edges, so a round trip through it is lossy.")),
						{TEXT("source_model"), TEXT("render_data"), TEXT("hires_source_model"),
						 TEXT("max_available")}),
					TEXT("source_model"))},
				{TEXT("request_tangents"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Include tangents. Costs time and memory; only needed for operations "
							 "that consume them.")),
					false)}
			},
			{TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			UStaticMesh* StaticMesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
			if (StaticMesh == nullptr)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("'%s' is not a Static Mesh."), *AssetPath);
				}
				return false;
			}

			int32 Lod = 0;
			Args->TryGetNumberField(TEXT("lod"), Lod);
			bool bRequestTangents = false;
			Args->TryGetBoolField(TEXT("request_tangents"), bRequestTangents);
			EGeometryScriptLODType LodType = EGeometryScriptLODType::SourceModel;
			if (!ParseLodType(Args, LodType, OutStructured, OutError))
			{
				return false;
			}

			UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
			if (Mesh == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("NewObject<UDynamicMesh> returned null."));
				OutError = TEXT("Could not create dynamic mesh.");
				return false;
			}

			FGeometryScriptCopyMeshFromAssetOptions AssetOptions;
			AssetOptions.bRequestTangents = bRequestTangents;
			FGeometryScriptMeshReadLOD RequestedLOD;
			RequestedLOD.LODType = LodType;
			RequestedLOD.LODIndex = Lod;
			EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

			UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
				StaticMesh, Mesh, AssetOptions, RequestedLOD, Outcome);

			if (Outcome != EGeometryScriptOutcomePins::Success)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("lod"),
					TEXT("The asset could not be read at that LOD. Confirm the LOD exists and that "
						 "the mesh allows CPU access."));
				OutStructured->SetStringField(TEXT("asset_path"), StaticMesh->GetPathName());
				OutStructured->SetNumberField(TEXT("lod"), Lod);
				OutError = FString::Printf(TEXT("CopyMeshFromStaticMesh failed for '%s' LOD %d."),
					*AssetPath, Lod);
				return false;
			}

			const bool bIndexIgnored = LodTypeIgnoresIndex(LodType);
			const FString LodLabel = bIndexIgnored
				? FString::Printf(TEXT("%s"), Raw_LodTypeName(LodType))
				: FString::Printf(TEXT("%s LOD %d"), Raw_LodTypeName(LodType), Lod);

			const FString Handle = FSololmcpObjectHandles::Get().Add(
				Mesh, DynamicMeshKind,
				FString::Printf(TEXT("%s#%s"), *StaticMesh->GetPathName(), *LodLabel));

			OutStructured->SetStringField(TEXT("mesh"), Handle);
			OutStructured->SetStringField(TEXT("asset_path"), StaticMesh->GetPathName());
			OutStructured->SetStringField(TEXT("lod_type"), Raw_LodTypeName(LodType));
			if (bIndexIgnored)
			{
				// Echoing the requested index here would suggest it was used.
				OutStructured->SetBoolField(TEXT("lod_ignored"), true);
				OutStructured->SetStringField(TEXT("lod_note"),
					FString::Printf(TEXT("lod_type %s names one specific mesh, so the lod index was "
										 "not used."), Raw_LodTypeName(LodType)));
			}
			else
			{
				OutStructured->SetNumberField(TEXT("lod"), Lod);
			}
			OutStructured->SetNumberField(TEXT("triangle_count"),
				UGeometryScriptLibrary_MeshQueryFunctions::GetNumTriangleIDs(Mesh));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Opened '%s' %s as %s."),
				*StaticMesh->GetName(), *LodLabel, *Handle);
			return true;
		},
		nullptr,
		0
	});

	// ── geometry_mesh_query ────────────────────────────────────────────────
	Registry.Register({
		TEXT("geometry_mesh_query"),
		TEXT("Measure an open dynamic mesh: counts, bounds, surface area and volume, whether it is "
			 "closed, open border loops and connected components. Use this to validate a mesh before "
			 "and after an operation — most GeometryScripting operations report success without "
			 "saying whether they changed anything."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mesh"), FSololmcpSchemaBuilder::String(
					TEXT("Dynamic mesh handle from geometry_mesh_open."))}
			},
			{TEXT("mesh")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UDynamicMesh* Mesh = ResolveMesh(Args, OutStructured, OutError);
			if (Mesh == nullptr)
			{
				return false;
			}

			using FQuery = UGeometryScriptLibrary_MeshQueryFunctions;

			const int32 TriangleIds = FQuery::GetNumTriangleIDs(Mesh);
			const bool bDense = FQuery::GetIsDenseMesh(Mesh);
			const bool bClosed = FQuery::GetIsClosedMesh(Mesh);
			const bool bHasAttributes = FQuery::GetMeshHasAttributeSet(Mesh);
			const int32 Components = FQuery::GetNumConnectedComponents(Mesh);
			const int32 BorderEdges = FQuery::GetNumOpenBorderEdges(Mesh);

			bool bAmbiguousTopology = false;
			const int32 BorderLoops = FQuery::GetNumOpenBorderLoops(Mesh, bAmbiguousTopology);

			float SurfaceArea = 0.0f;
			float Volume = 0.0f;
			FQuery::GetMeshVolumeArea(Mesh, SurfaceArea, Volume);

			const FBox Bounds = FQuery::GetMeshBoundingBox(Mesh);

			OutStructured->SetNumberField(TEXT("triangle_id_count"), TriangleIds);
			OutStructured->SetBoolField(TEXT("is_dense"), bDense);
			OutStructured->SetBoolField(TEXT("is_closed"), bClosed);
			OutStructured->SetBoolField(TEXT("has_attribute_set"), bHasAttributes);
			OutStructured->SetNumberField(TEXT("connected_components"), Components);
			OutStructured->SetNumberField(TEXT("open_border_edges"), BorderEdges);
			OutStructured->SetNumberField(TEXT("open_border_loops"), BorderLoops);
			if (bAmbiguousTopology)
			{
				// The loop count is unreliable on non-manifold topology; saying so is
				// more useful than a number the caller would treat as exact.
				OutStructured->SetBoolField(TEXT("open_border_loops_ambiguous"), true);
				OutStructured->SetStringField(TEXT("open_border_loops_note"),
					TEXT("Topology is non-manifold, so the loop count is approximate."));
			}
			OutStructured->SetNumberField(TEXT("surface_area"), SurfaceArea);
			OutStructured->SetNumberField(TEXT("volume"), Volume);

			TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> MinJson = MakeShared<FJsonObject>();
			MinJson->SetNumberField(TEXT("x"), Bounds.Min.X);
			MinJson->SetNumberField(TEXT("y"), Bounds.Min.Y);
			MinJson->SetNumberField(TEXT("z"), Bounds.Min.Z);
			TSharedRef<FJsonObject> MaxJson = MakeShared<FJsonObject>();
			MaxJson->SetNumberField(TEXT("x"), Bounds.Max.X);
			MaxJson->SetNumberField(TEXT("y"), Bounds.Max.Y);
			MaxJson->SetNumberField(TEXT("z"), Bounds.Max.Z);
			BoundsJson->SetObjectField(TEXT("min"), MinJson);
			BoundsJson->SetObjectField(TEXT("max"), MaxJson);
			OutStructured->SetObjectField(TEXT("bounding_box"), BoundsJson);

			OutStructured->SetStringField(TEXT("info"), FQuery::GetMeshInfoString(Mesh));
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(
				TEXT("%d triangle id(s), %d component(s), %s, area %.2f, volume %.2f."),
				TriangleIds, Components, bClosed ? TEXT("closed") : TEXT("open"), SurfaceArea, Volume);
			return true;
		},
		nullptr,
		0
	});

	// ── geometry_mesh_save_to_asset ────────────────────────────────────────
	Registry.Register({
		TEXT("geometry_mesh_save_to_asset"),
		TEXT("Write an open dynamic mesh back into a Static Mesh asset. This overwrites the target "
			 "LOD, so it is the terminal step of a session rather than something to call between "
			 "operations. The handle stays open afterwards so the session can continue."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mesh"), FSololmcpSchemaBuilder::String(TEXT("Dynamic mesh handle."))},
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(
					TEXT("Destination Static Mesh asset. Must already exist."))},
				{TEXT("lod"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("Target source-model LOD index to overwrite. Ignored when "
							 "write_hires_source is true.")), 0)},
				{TEXT("write_hires_source"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Write into the HiRes source model instead of a numbered LOD. Pair this "
							 "with lod_type=hires_source_model on the open side.")),
					false)},
				{TEXT("recompute_normals"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Recompute normals on the written asset. Leave off to preserve normals "
							 "carried through from the source mesh.")),
					false)},
				{TEXT("recompute_tangents"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Recompute tangents on the written asset.")),
					false)},
				{TEXT("save_asset"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Save the package to disk afterwards. When false the change only exists "
							 "in memory and is lost if the editor closes without saving.")),
					true)}
			},
			{TEXT("mesh"), TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UDynamicMesh* Mesh = ResolveMesh(Args, OutStructured, OutError);
			if (Mesh == nullptr)
			{
				return false;
			}
			FString AssetPath;
			if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			UStaticMesh* StaticMesh = Cast<UStaticMesh>(Context.Services.LoadAsset(AssetPath, OutError));
			if (StaticMesh == nullptr)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("'%s' is not a Static Mesh."), *AssetPath);
				}
				return false;
			}

			int32 Lod = 0;
			Args->TryGetNumberField(TEXT("lod"), Lod);
			bool bSaveAsset = true;
			Args->TryGetBoolField(TEXT("save_asset"), bSaveAsset);
			bool bWriteHiRes = false;
			Args->TryGetBoolField(TEXT("write_hires_source"), bWriteHiRes);
			bool bRecomputeNormals = false;
			Args->TryGetBoolField(TEXT("recompute_normals"), bRecomputeNormals);
			bool bRecomputeTangents = false;
			Args->TryGetBoolField(TEXT("recompute_tangents"), bRecomputeTangents);

			FGeometryScriptCopyMeshToAssetOptions Options;
			Options.bEnableRecomputeNormals = bRecomputeNormals;
			Options.bEnableRecomputeTangents = bRecomputeTangents;
			FGeometryScriptMeshWriteLOD TargetLOD;
			TargetLOD.bWriteHiResSource = bWriteHiRes;
			TargetLOD.LODIndex = Lod;
			EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

			UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
				Mesh, StaticMesh, Options, TargetLOD, Outcome);

			OutStructured->SetStringField(TEXT("asset_path"), StaticMesh->GetPathName());
			if (bWriteHiRes)
			{
				OutStructured->SetStringField(TEXT("target"), TEXT("hires_source_model"));
			}
			else
			{
				OutStructured->SetStringField(TEXT("target"), TEXT("source_model"));
				OutStructured->SetNumberField(TEXT("lod"), Lod);
			}
			OutStructured->SetStringField(TEXT("outcome"), OutcomeToString(Outcome));
			if (Outcome != EGeometryScriptOutcomePins::Success)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("asset_path"),
					TEXT("The mesh could not be written to that asset LOD."));
				OutError = FString::Printf(TEXT("CopyMeshToStaticMesh failed for '%s' LOD %d."),
					*AssetPath, Lod);
				return false;
			}

			bool bSaved = false;
			FString SaveError;
			if (bSaveAsset)
			{
				bSaved = Context.Services.SaveAsset(StaticMesh->GetPathName(), /*bOnlyIfDirty=*/false, SaveError);
				if (!bSaved && !SaveError.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("save_error"), SaveError);
				}
			}
			OutStructured->SetBoolField(TEXT("saved"), bSaved);
			if (bSaveAsset && !bSaved)
			{
				// The mesh did land in the asset; only persistence failed. Reporting
				// success here would let a queued wave move on from unsaved work.
				OutStructured->SetStringField(TEXT("save_note"),
					TEXT("The asset was updated in memory but not written to disk; the change will "
						 "be lost if the editor closes without saving."));
			}
			OutStructured->SetBoolField(TEXT("ok"), true);
			const FString TargetLabel = bWriteHiRes
				? FString(TEXT("HiRes source"))
				: FString::Printf(TEXT("LOD %d"), Lod);
			OutSummary = FString::Printf(TEXT("Wrote mesh into '%s' %s%s."),
				*StaticMesh->GetName(), *TargetLabel,
				bSaved ? TEXT(" and saved it") : TEXT(" (unsaved)"));
			return true;
		},
		nullptr,
		0
	});
}

#endif // SOMOLMCP_HAS_GEOMETRYSCRIPTING

} // namespace UE::SOMOLMCP
