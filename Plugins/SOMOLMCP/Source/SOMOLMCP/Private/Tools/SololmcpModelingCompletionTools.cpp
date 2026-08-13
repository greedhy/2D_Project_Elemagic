// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// P2 native C++ Modeling/DynamicMesh authoring tools.

#include "Tools/SololmcpModelingCompletionTools.h"

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeLock.h"
#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshTriangleAttribute.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMeshEditor.h"
#include "DynamicMeshToMeshDescription.h"
#include "Engine/StaticMesh.h"
#include "GeometryTypes.h"
#include "MeshBoundaryLoops.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/Guid.h"
#include "Misc/SecureHash.h"
#include "Operations/InsetMeshRegion.h"
#include "ScopedTransaction.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Package.h"

#endif

namespace UE::SOMOLMCP
{
namespace ModelingCompletion
{
	using FSchema = TSharedRef<FJsonObject>;

	enum class EOperation : uint8
	{
		Inspect,
		Validate,
		SelectVerticesBox,
		SelectTrianglesBox,
		SelectBoundaryEdges,
		SelectGrow,
		SelectShrink,
		SelectInvert,
		SelectComponent,
		EdgeSplit,
		EdgeCollapse,
		EdgeFlip,
		TrianglePoke,
		TrianglesDelete,
		OrientationReverse,
		Compact,
		VerticesWeld,
		HoleFill,
		FacesExtrude,
		FacesInset,
		PlaneTrim,
		SculptSmooth,
		SculptInflate,
		SculptFlatten,
		DeformTranslate,
		DeformRotate,
		DeformScale,
		DeformTwist,
		DeformBend,
		DeformTaper,
		NormalsRecompute,
		VertexColorsFill,
		MaterialIdsSet,
		PolygroupsSet,
		UvPlanarProject,
		BakePositionColor,
		BakeNormalColor,
		SnapshotCreate,
		SnapshotRestore,
		ReceiptGet,
	};

	struct FToolSpec
	{
		const TCHAR* Name;
		const TCHAR* Description;
		EOperation Operation;
		bool bMutation;
	};

	static const FToolSpec ToolSpecs[] = {
		{TEXT("modeling_dynamic_mesh_inspect"), TEXT("Inspect a Static Mesh LOD through a real native FDynamicMesh3 conversion."), EOperation::Inspect, false},
		{TEXT("modeling_dynamic_mesh_validate"), TEXT("Validate topology, bounds, and element counts of a Static Mesh LOD."), EOperation::Validate, false},
		{TEXT("modeling_dynamic_mesh_selection_vertices_box"), TEXT("Select mesh vertices inside an axis-aligned box."), EOperation::SelectVerticesBox, false},
		{TEXT("modeling_dynamic_mesh_selection_triangles_box"), TEXT("Select triangles whose centroids are inside an axis-aligned box."), EOperation::SelectTrianglesBox, false},
		{TEXT("modeling_dynamic_mesh_selection_boundary_edges"), TEXT("Resolve boundary edges for the whole mesh or a triangle selection."), EOperation::SelectBoundaryEdges, false},
		{TEXT("modeling_dynamic_mesh_selection_grow"), TEXT("Grow a triangle selection through native topology adjacency."), EOperation::SelectGrow, false},
		{TEXT("modeling_dynamic_mesh_selection_shrink"), TEXT("Shrink a triangle selection by removing its boundary ring."), EOperation::SelectShrink, false},
		{TEXT("modeling_dynamic_mesh_selection_invert"), TEXT("Invert a triangle selection against all valid triangles."), EOperation::SelectInvert, false},
		{TEXT("modeling_dynamic_mesh_selection_component"), TEXT("Select the connected triangle component containing a seed triangle."), EOperation::SelectComponent, false},
		{TEXT("modeling_dynamic_mesh_edge_split"), TEXT("Split an edge at a validated interpolation parameter."), EOperation::EdgeSplit, true},
		{TEXT("modeling_dynamic_mesh_edge_collapse"), TEXT("Collapse a validated edge while preserving manifold topology."), EOperation::EdgeCollapse, true},
		{TEXT("modeling_dynamic_mesh_edge_flip"), TEXT("Flip a non-boundary edge when the native topology operation permits it."), EOperation::EdgeFlip, true},
		{TEXT("modeling_dynamic_mesh_triangle_poke"), TEXT("Insert a vertex into a triangle using barycentric coordinates."), EOperation::TrianglePoke, true},
		{TEXT("modeling_dynamic_mesh_triangles_delete"), TEXT("Delete validated triangles with an explicit empty-mesh guard."), EOperation::TrianglesDelete, true},
		{TEXT("modeling_dynamic_mesh_orientation_reverse"), TEXT("Reverse the whole mesh or selected triangle orientations."), EOperation::OrientationReverse, true},
		{TEXT("modeling_dynamic_mesh_compact"), TEXT("Compact sparse vertex, edge, triangle, and attribute identifiers."), EOperation::Compact, true},
		{TEXT("modeling_dynamic_mesh_vertices_weld"), TEXT("Weld one vertex into another using the native manifold gate."), EOperation::VerticesWeld, true},
		{TEXT("modeling_dynamic_mesh_hole_fill"), TEXT("Fill a validated boundary loop with a native triangle fan."), EOperation::HoleFill, true},
		{TEXT("modeling_dynamic_mesh_faces_extrude"), TEXT("Extrude a connected triangle region and build closed boundary side walls."), EOperation::FacesExtrude, true},
		{TEXT("modeling_dynamic_mesh_faces_inset"), TEXT("Inset selected face regions by a native local-space boundary distance."), EOperation::FacesInset, true},
		{TEXT("modeling_dynamic_mesh_plane_trim"), TEXT("Safely trim whole triangles on one side of a plane; intersecting triangles fail closed by default."), EOperation::PlaneTrim, true},
		{TEXT("modeling_dynamic_mesh_sculpt_smooth"), TEXT("Apply bounded Laplacian smoothing to selected or brush-filtered vertices."), EOperation::SculptSmooth, true},
		{TEXT("modeling_dynamic_mesh_sculpt_inflate"), TEXT("Inflate selected or brush-filtered vertices along computed normals."), EOperation::SculptInflate, true},
		{TEXT("modeling_dynamic_mesh_sculpt_flatten"), TEXT("Project selected or brush-filtered vertices toward a plane."), EOperation::SculptFlatten, true},
		{TEXT("modeling_dynamic_mesh_deform_translate"), TEXT("Translate a validated vertex selection."), EOperation::DeformTranslate, true},
		{TEXT("modeling_dynamic_mesh_deform_rotate"), TEXT("Rotate a validated vertex selection around a pivot."), EOperation::DeformRotate, true},
		{TEXT("modeling_dynamic_mesh_deform_scale"), TEXT("Scale a validated vertex selection around a pivot."), EOperation::DeformScale, true},
		{TEXT("modeling_dynamic_mesh_deform_twist"), TEXT("Twist selected vertices around an arbitrary axis."), EOperation::DeformTwist, true},
		{TEXT("modeling_dynamic_mesh_deform_bend"), TEXT("Apply a bounded bend deformation around an arbitrary axis."), EOperation::DeformBend, true},
		{TEXT("modeling_dynamic_mesh_deform_taper"), TEXT("Apply an axis-relative taper to selected vertices."), EOperation::DeformTaper, true},
		{TEXT("modeling_dynamic_mesh_normals_recompute"), TEXT("Recompute native per-vertex normals and write them back."), EOperation::NormalsRecompute, true},
		{TEXT("modeling_dynamic_mesh_vertex_colors_fill"), TEXT("Fill native vertex colors on selected or all vertices."), EOperation::VertexColorsFill, true},
		{TEXT("modeling_dynamic_mesh_material_ids_set"), TEXT("Assign a material ID to validated triangles."), EOperation::MaterialIdsSet, true},
		{TEXT("modeling_dynamic_mesh_polygroups_set"), TEXT("Assign a polygroup ID to validated triangles."), EOperation::PolygroupsSet, true},
		{TEXT("modeling_dynamic_mesh_uv_planar_project"), TEXT("Bake planar per-vertex UVs on the selected projection axes."), EOperation::UvPlanarProject, true},
		{TEXT("modeling_dynamic_mesh_bake_position_color"), TEXT("Bake normalized local positions into native vertex colors."), EOperation::BakePositionColor, true},
		{TEXT("modeling_dynamic_mesh_bake_normal_color"), TEXT("Bake computed normals into native vertex colors."), EOperation::BakeNormalColor, true},
		{TEXT("modeling_dynamic_mesh_snapshot_create"), TEXT("Capture a real in-memory geometry snapshot for guarded recovery."), EOperation::SnapshotCreate, false},
		{TEXT("modeling_dynamic_mesh_snapshot_restore"), TEXT("Restore and commit a previously captured geometry snapshot."), EOperation::SnapshotRestore, true},
		{TEXT("modeling_dynamic_mesh_receipt_get"), TEXT("Read a previously issued Modeling/DynamicMesh receipt."), EOperation::ReceiptGet, false},
	};
	static_assert(UE_ARRAY_COUNT(ToolSpecs) == 40, "Modeling completion schema closure must cover all 40 tools.");

	static FCriticalSection StateLock;
	static TMap<FString, TSharedPtr<FJsonObject>> Receipts;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8

	using namespace UE::Geometry;

	struct FSnapshot
	{
		FString AssetPath;
		int32 LodIndex = 0;
		FString SourceHash;
		TSharedPtr<FDynamicMesh3> Mesh;
	};

	struct FTarget
	{
		FString AssetPath;
		int32 LodIndex = 0;
		UStaticMesh* Asset = nullptr;
		FDynamicMesh3 Mesh;
		TSharedPtr<FDynamicMesh3> BeforeMesh;
		TSharedPtr<FMeshDescription> BeforeDescription;
		FString BeforeHash;
		int32 BeforeVertices = 0;
		int32 BeforeEdges = 0;
		int32 BeforeTriangles = 0;
	};

	static TMap<FString, FSnapshot> Snapshots;

	static void Fail(
		const TSharedPtr<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& OutError,
		const FString& Code,
		const FString& Message,
		const FString& Operation = FString())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("status"), TEXT("failed"));
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("reason_code"), Code);
		Out->SetStringField(TEXT("message"), Message);
		Out->SetBoolField(TEXT("mutation_applied"), false);
		Out->SetBoolField(TEXT("readback_verified"), false);
		if (!Operation.IsEmpty()) Out->SetStringField(TEXT("operation"), Operation);
		TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		FString AssetPath;
		double Lod = 0;
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("asset_path"), AssetPath);
			Args->TryGetNumberField(TEXT("lod_index"), Lod);
		}
		Target->SetStringField(TEXT("asset_path"), AssetPath);
		Target->SetNumberField(TEXT("lod_index"), static_cast<int32>(Lod));
		Out->SetObjectField(TEXT("target"), Target);
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("status"), TEXT("failed"));
		Receipt->SetStringField(TEXT("error_code"), Code);
		Receipt->SetBoolField(TEXT("verified"), false);
		Out->SetObjectField(TEXT("receipt"), Receipt);
		OutError = Message;
	}

	static FString MeshHash(const FDynamicMesh3& Mesh)
	{
		// Hash content rather than FDynamicMesh3's internal counters. The explicit
		// mesh stream covers topology and built-in vertex/group channels, while the
		// AttributeSet stream covers every normal/UV/color overlay, material ID,
		// polygroup/weight layer, and remaining attached dynamic attribute.
		FDynamicMesh3 HashMesh(Mesh);
		TArray<uint8> SerializedMesh;
		FMemoryWriter Writer(SerializedMesh, true);
		uint32 HashFormatVersion = 2;
		Writer.Serialize(&HashFormatVersion, sizeof(HashFormatVersion));
		int32 Counts[] = {
			HashMesh.VertexCount(), HashMesh.EdgeCount(), HashMesh.TriangleCount(),
			HashMesh.MaxVertexID(), HashMesh.MaxEdgeID(), HashMesh.MaxTriangleID()};
		Writer.Serialize(Counts, sizeof(Counts));
		bool ChannelFlags[] = {
			HashMesh.HasVertexNormals(), HashMesh.HasVertexUVs(), HashMesh.HasVertexColors(),
			HashMesh.HasTriangleGroups(), HashMesh.HasAttributes()};
		Writer.Serialize(ChannelFlags, sizeof(ChannelFlags));
		for (int32 VertexId : HashMesh.VertexIndicesItr())
		{
			Writer.Serialize(&VertexId, sizeof(VertexId));
			FVector3d Position = HashMesh.GetVertex(VertexId);
			Writer.Serialize(&Position, sizeof(Position));
			if (HashMesh.HasVertexNormals())
			{
				FVector3f Normal = HashMesh.GetVertexNormal(VertexId);
				Writer.Serialize(&Normal, sizeof(Normal));
			}
			if (HashMesh.HasVertexUVs())
			{
				FVector2f Uv = HashMesh.GetVertexUV(VertexId);
				Writer.Serialize(&Uv, sizeof(Uv));
			}
			if (HashMesh.HasVertexColors())
			{
				FVector3f Color = HashMesh.GetVertexColor(VertexId);
				Writer.Serialize(&Color, sizeof(Color));
			}
		}
		for (int32 TriangleId : HashMesh.TriangleIndicesItr())
		{
			Writer.Serialize(&TriangleId, sizeof(TriangleId));
			FIndex3i Triangle = HashMesh.GetTriangle(TriangleId);
			Writer.Serialize(&Triangle, sizeof(Triangle));
			if (HashMesh.HasTriangleGroups())
			{
				int32 GroupId = HashMesh.GetTriangleGroup(TriangleId);
				Writer.Serialize(&GroupId, sizeof(GroupId));
			}
		}
		if (HashMesh.HasAttributes())
		{
			HashMesh.Attributes()->Serialize(Writer, nullptr, false);
		}
		Writer.Close();
		return FMD5::HashBytes(SerializedMesh.GetData(), SerializedMesh.Num());
	}

	static FString CanonicalGeometryHash(const FDynamicMesh3& Mesh)
	{
		// ID-agnostic and order-agnostic hash over topology plus positions at
		// Static Mesh source (float) precision. UE's MeshDescription commit path
		// can renormalize element IDs and derived channels during a round trip,
		// which perturbs the exact MeshHash stream even when the persisted
		// geometry is exactly the intended mutation. This canonical form is the
		// fallback equivalence proof for that case; winding order is preserved
		// so orientation flips are still rejected.
		TArray<TArray<uint8>> TriangleRows;
		TriangleRows.Reserve(Mesh.TriangleCount());
		for (int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			const FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			TArray<uint8> Row;
			Row.Reserve(3 * sizeof(FVector3f));
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FVector3f Position(Mesh.GetVertex(Triangle[Corner]));
				const uint8* Bytes = reinterpret_cast<const uint8*>(&Position);
				Row.Append(Bytes, sizeof(FVector3f));
			}
			TriangleRows.Add(MoveTemp(Row));
		}
		TriangleRows.Sort([](const TArray<uint8>& A, const TArray<uint8>& B)
		{
			const int32 Shared = FMath::Min(A.Num(), B.Num());
			const int32 Compare = Shared > 0 ? FMemory::Memcmp(A.GetData(), B.GetData(), Shared) : 0;
			return Compare != 0 ? Compare < 0 : A.Num() < B.Num();
		});
		TArray<uint8> Serialized;
		FMemoryWriter Writer(Serialized, true);
		uint32 CanonicalFormatVersion = 1;
		Writer.Serialize(&CanonicalFormatVersion, sizeof(CanonicalFormatVersion));
		int32 Counts[] = {Mesh.VertexCount(), Mesh.EdgeCount(), Mesh.TriangleCount()};
		Writer.Serialize(Counts, sizeof(Counts));
		for (const TArray<uint8>& Row : TriangleRows)
		{
			int32 RowSize = Row.Num();
			Writer.Serialize(&RowSize, sizeof(RowSize));
			if (RowSize > 0)
			{
				Writer.Serialize(const_cast<uint8*>(Row.GetData()), RowSize);
			}
		}
		Writer.Close();
		return FMD5::HashBytes(Serialized.GetData(), Serialized.Num());
	}

	static bool ChannelFlagsMatch(const FDynamicMesh3& A, const FDynamicMesh3& B)
	{
		return A.HasVertexNormals() == B.HasVertexNormals()
			&& A.HasVertexUVs() == B.HasVertexUVs()
			&& A.HasVertexColors() == B.HasVertexColors()
			&& A.HasTriangleGroups() == B.HasTriangleGroups();
	}

	static TSharedRef<FJsonObject> MeshReadback(const FDynamicMesh3& Mesh)
	{
		TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
		Readback->SetNumberField(TEXT("vertex_count"), Mesh.VertexCount());
		Readback->SetNumberField(TEXT("edge_count"), Mesh.EdgeCount());
		Readback->SetNumberField(TEXT("triangle_count"), Mesh.TriangleCount());
		Readback->SetStringField(TEXT("geometry_hash"), MeshHash(Mesh));
		Readback->SetBoolField(TEXT("topology_valid"), Mesh.CheckValidity(
			FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly));
		const FAxisAlignedBox3d Bounds = Mesh.GetBounds();
		TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> MinJson = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> MaxJson = MakeShared<FJsonObject>();
		MinJson->SetNumberField(TEXT("x"), Bounds.Min.X);
		MinJson->SetNumberField(TEXT("y"), Bounds.Min.Y);
		MinJson->SetNumberField(TEXT("z"), Bounds.Min.Z);
		MaxJson->SetNumberField(TEXT("x"), Bounds.Max.X);
		MaxJson->SetNumberField(TEXT("y"), Bounds.Max.Y);
		MaxJson->SetNumberField(TEXT("z"), Bounds.Max.Z);
		BoundsJson->SetObjectField(TEXT("min"), MinJson);
		BoundsJson->SetObjectField(TEXT("max"), MaxJson);
		Readback->SetObjectField(TEXT("bounds"), BoundsJson);
		return Readback;
	}

	static TArray<TSharedPtr<FJsonValue>> IntArrayJson(const TArray<int32>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (int32 Value : Values) Result.Add(MakeShared<FJsonValueNumber>(Value));
		return Result;
	}

	static bool ReadIntArray(
		const TSharedRef<FJsonObject>& Args,
		const TCHAR* Field,
		TArray<int32>& OutValues,
		const int32 MaxItems,
		FString& OutError,
		const bool bRequired = true)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Args->TryGetArrayField(Field, Values))
		{
			if (!bRequired) return true;
			OutError = FString::Printf(TEXT("Missing required array '%s'."), Field);
			return false;
		}
		if (!Values || Values->Num() > MaxItems)
		{
			OutError = FString::Printf(TEXT("'%s' exceeds the maximum of %d entries."), Field, MaxItems);
			return false;
		}
		TSet<int32> Unique;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			double Number = 0;
			if (!Value.IsValid() || !Value->TryGetNumber(Number) || !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
			{
				OutError = FString::Printf(TEXT("'%s' must contain integer IDs."), Field);
				return false;
			}
			Unique.Add(static_cast<int32>(Number));
		}
		OutValues = Unique.Array();
		OutValues.Sort();
		return true;
	}

	static bool ReadVector(
		const TSharedRef<FJsonObject>& Args,
		const TCHAR* Field,
		FVector3d& OutValue,
		FString& OutError,
		const FVector3d& DefaultValue,
		const bool bRequired = false)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Args->TryGetObjectField(Field, Object) || !Object || !Object->IsValid())
		{
			if (bRequired)
			{
				OutError = FString::Printf(TEXT("Missing required vector '%s'."), Field);
				return false;
			}
			OutValue = DefaultValue;
			return true;
		}
		double X = 0, Y = 0, Z = 0;
		if (!(*Object)->TryGetNumberField(TEXT("x"), X) || !(*Object)->TryGetNumberField(TEXT("y"), Y) || !(*Object)->TryGetNumberField(TEXT("z"), Z) ||
			!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
		{
			OutError = FString::Printf(TEXT("Vector '%s' requires finite x/y/z values."), Field);
			return false;
		}
		OutValue = FVector3d(X, Y, Z);
		return true;
	}

	static bool ConvertFromMeshDescription(const FMeshDescription* Description, FDynamicMesh3& OutMesh)
	{
		OutMesh.Clear();
		if (!Description || Description->Triangles().Num() == 0)
		{
			return false;
		}

		FMeshDescriptionToDynamicMesh Converter;
		Converter.bTransformVertexColorsLinearToSRGB = false;
		Converter.bVIDsFromNonManifoldMeshDescriptionAttr = true;
		Converter.Convert(Description, OutMesh, true);
		OutMesh.CompactInPlace();
		return OutMesh.TriangleCount() > 0 &&
			OutMesh.CheckValidity(FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly);
	}

	static bool LoadTarget(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		FTarget& OutTarget,
		TSharedRef<FJsonObject>& Out,
		FString& OutError,
		const FString& Operation)
	{
		if (!Args->TryGetStringField(TEXT("asset_path"), OutTarget.AssetPath) || !OutTarget.AssetPath.StartsWith(TEXT("/Game/")))
		{
			Fail(Args, Out, OutError, TEXT("invalid_target_path"), TEXT("asset_path must identify a Static Mesh under /Game/."), Operation);
			return false;
		}
		double LodNumber = 0;
		Args->TryGetNumberField(TEXT("lod_index"), LodNumber);
		OutTarget.LodIndex = static_cast<int32>(LodNumber);
		FString LoadError;
		OutTarget.Asset = Cast<UStaticMesh>(Context.Services.LoadAsset(OutTarget.AssetPath, LoadError));
		if (!OutTarget.Asset)
		{
			Fail(Args, Out, OutError, TEXT("static_mesh_not_found"), LoadError.IsEmpty() ? TEXT("The target is not a loadable Static Mesh.") : LoadError, Operation);
			return false;
		}
		if (OutTarget.LodIndex < 0 || OutTarget.LodIndex >= OutTarget.Asset->GetNumSourceModels())
		{
			Fail(Args, Out, OutError, TEXT("invalid_lod_index"), FString::Printf(TEXT("lod_index %d is outside the editable source-model range [0, %d)."), OutTarget.LodIndex, OutTarget.Asset->GetNumSourceModels()), Operation);
			return false;
		}
		FMeshDescription* Description = OutTarget.Asset->GetMeshDescription(OutTarget.LodIndex);
		if (!Description || Description->Triangles().Num() == 0)
		{
			Fail(Args, Out, OutError, TEXT("mesh_description_unavailable"), TEXT("The requested LOD has no editable MeshDescription triangles."), Operation);
			return false;
		}
		OutTarget.BeforeDescription = MakeShared<FMeshDescription>(*Description);
		if (!ConvertFromMeshDescription(Description, OutTarget.Mesh))
		{
			Fail(Args, Out, OutError, TEXT("dynamic_mesh_conversion_failed"), TEXT("Static Mesh conversion produced empty or invalid DynamicMesh topology."), Operation);
			return false;
		}
		OutTarget.BeforeVertices = OutTarget.Mesh.VertexCount();
		OutTarget.BeforeEdges = OutTarget.Mesh.EdgeCount();
		OutTarget.BeforeTriangles = OutTarget.Mesh.TriangleCount();
		OutTarget.BeforeHash = MeshHash(OutTarget.Mesh);
		OutTarget.BeforeMesh = MakeShared<FDynamicMesh3>(OutTarget.Mesh);
		FString ExpectedHash;
		if (Args->TryGetStringField(TEXT("expected_hash"), ExpectedHash) && !ExpectedHash.IsEmpty() && ExpectedHash != OutTarget.BeforeHash)
		{
			Fail(Args, Out, OutError, TEXT("stale_target_hash"), FString::Printf(TEXT("expected_hash '%s' does not match current geometry hash '%s'."), *ExpectedHash, *OutTarget.BeforeHash), Operation);
			return false;
		}
		return true;
	}

	static FString StoreReceipt(const TSharedRef<FJsonObject>& Receipt)
	{
		const FString ReceiptId = FString::Printf(TEXT("modeling_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(20).ToLower());
		Receipt->SetStringField(TEXT("receipt_id"), ReceiptId);
		FScopeLock Lock(&StateLock);
		Receipts.Add(ReceiptId, Receipt);
		if (Receipts.Num() > 2048)
		{
			Receipts.Remove(Receipts.CreateConstIterator().Key());
		}
		return ReceiptId;
	}

	static void CompleteRead(
		const FString& ToolName,
		const FTarget& Target,
		TSharedRef<FJsonObject>& Out,
		FString& OutSummary,
		const TSharedPtr<FJsonObject>& Details = nullptr)
	{
		TSharedRef<FJsonObject> TargetJson = MakeShared<FJsonObject>();
		TargetJson->SetStringField(TEXT("asset_path"), Target.AssetPath);
		TargetJson->SetNumberField(TEXT("lod_index"), Target.LodIndex);
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("operation"), ToolName);
		Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
		Receipt->SetBoolField(TEXT("mutation_applied"), false);
		Receipt->SetBoolField(TEXT("saved"), false);
		Receipt->SetBoolField(TEXT("verified"), true);
		Receipt->SetStringField(TEXT("geometry_hash"), Target.BeforeHash);
		const FString ReceiptId = StoreReceipt(Receipt);
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
		Out->SetStringField(TEXT("receipt_id"), ReceiptId);
		Out->SetBoolField(TEXT("mutation_applied"), false);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetObjectField(TEXT("target"), TargetJson);
		Out->SetObjectField(TEXT("readback"), Details.IsValid() ? Details.ToSharedRef() : MeshReadback(Target.Mesh));
		Out->SetObjectField(TEXT("receipt"), Receipt);
		OutSummary = FString::Printf(TEXT("%s completed for %s LOD %d; receipt=%s"), *ToolName, *Target.AssetPath, Target.LodIndex, *ReceiptId);
	}

	static void ConvertToMeshDescription(const FDynamicMesh3& Mesh, FMeshDescription& Description)
	{
		FDynamicMeshToMeshDescription Converter;
		// LoadTarget deliberately does not transform vertex colors. Keep the reverse
		// conversion symmetric so a verified write/readback round trip is stable.
		Converter.ConversionOptions.bTransformVtxColorsSRGBToLinear = false;
		Converter.Convert(&Mesh, Description, true);
	}

	static bool RestoreBeforeMesh(FTarget& Target, const bool bPackageWasDirty, FString& OutRollbackError)
	{
		if (!Target.Asset || !Target.BeforeMesh.IsValid() || !Target.BeforeDescription.IsValid())
		{
			OutRollbackError = TEXT("The before-mesh snapshot is unavailable.");
			return false;
		}

		FMeshDescription* Description = Target.Asset->GetMeshDescription(Target.LodIndex);
		if (!Description)
		{
			OutRollbackError = TEXT("The editable MeshDescription is unavailable during rollback.");
			return false;
		}

		*Description = *Target.BeforeDescription;
		UStaticMesh::FCommitMeshDescriptionParams CommitParams;
		CommitParams.bMarkPackageDirty = false;
		CommitParams.bUseHashAsGuid = true;
		Target.Asset->CommitMeshDescription(Target.LodIndex, CommitParams);
		Target.Asset->PostEditChange();

		FMeshDescription* RestoredDescription = Target.Asset->GetMeshDescription(Target.LodIndex);
		FDynamicMesh3 RestoredMesh;
		const bool bRestored = ConvertFromMeshDescription(RestoredDescription, RestoredMesh) &&
			MeshHash(RestoredMesh) == Target.BeforeHash;
		if (UPackage* Package = Target.Asset->GetOutermost())
		{
			Package->SetDirtyFlag(bRestored ? bPackageWasDirty : true);
		}
		if (!bRestored)
		{
			OutRollbackError = TEXT("The before mesh could not be verified after rollback.");
		}
		else
		{
			Target.Mesh = *Target.BeforeMesh;
		}
		return bRestored;
	}

	static bool CommitTarget(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		const FString& ToolName,
		FTarget& Target,
		TSharedRef<FJsonObject>& Out,
		FString& OutSummary,
		FString& OutError)
	{
		if (Target.Mesh.TriangleCount() <= 0 || !Target.Mesh.CheckValidity(FDynamicMesh3::FValidityOptions(), EValidityCheckFailMode::ReturnOnly))
		{
			Fail(Args, Out, OutError, TEXT("post_operation_topology_invalid"), TEXT("The operation produced empty or invalid topology; no asset write was attempted."), ToolName);
			return false;
		}
		const FString MutationHash = MeshHash(Target.Mesh);
		if (MutationHash == Target.BeforeHash)
		{
			Fail(Args, Out, OutError, TEXT("no_geometry_change"), TEXT("The operation did not change geometry; parameter-only success is forbidden."), ToolName);
			return false;
		}
		// StaticMesh source data stores positions and several attributes at float
		// precision, and conversion also rebuilds overlay/element ID spaces. Hash the
		// exact persistable representation, not the pre-conversion DynamicMesh layout.
		Target.Mesh.CompactInPlace();
		FMeshDescription* Description = Target.Asset->GetMeshDescription(Target.LodIndex);
		if (!Description)
		{
			Fail(Args, Out, OutError, TEXT("mesh_description_lost"), TEXT("The editable MeshDescription disappeared before commit."), ToolName);
			return false;
		}
		// Preserve the StaticMesh's registered custom attribute schema while the
		// converter replaces all geometry and attribute values.
		FMeshDescription PersistableDescription(*Description);
		ConvertToMeshDescription(Target.Mesh, PersistableDescription);
		FDynamicMesh3 PersistableMesh;
		if (!ConvertFromMeshDescription(&PersistableDescription, PersistableMesh))
		{
			Fail(Args, Out, OutError, TEXT("persistable_mesh_conversion_failed"), TEXT("The operation could not be represented as valid Static Mesh source geometry."), ToolName);
			return false;
		}
		const FString AfterHash = MeshHash(PersistableMesh);
		if (AfterHash == Target.BeforeHash)
		{
			Fail(Args, Out, OutError, TEXT("no_persisted_geometry_change"), TEXT("The operation produced no change at Static Mesh source precision; no asset write was attempted."), ToolName);
			return false;
		}
		const bool bPackageWasDirty = Target.Asset->GetOutermost() && Target.Asset->GetOutermost()->IsDirty();
		FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("SOMOLMCP %s"), *ToolName)));
		Target.Asset->Modify();
		bool bMutationSaved = false;
		auto RollbackAndFail = [&](const FString& Code, const FString& Message)
		{
			FString RollbackError;
			bool bRollbackVerified = RestoreBeforeMesh(Target, bPackageWasDirty, RollbackError);
			if (bRollbackVerified && bMutationSaved)
			{
				FString RollbackSaveError;
				bRollbackVerified = Context.Services.SaveAsset(Target.AssetPath, false, RollbackSaveError);
				if (!bRollbackVerified)
				{
					RollbackError = RollbackSaveError.IsEmpty() ? TEXT("The restored Static Mesh could not be saved.") : RollbackSaveError;
				}
			}
			Transaction.Cancel();
			const FString FailureMessage = bRollbackVerified || RollbackError.IsEmpty()
				? Message
				: FString::Printf(TEXT("%s Rollback also failed: %s"), *Message, *RollbackError);
			Fail(Args, Out, OutError, Code, FailureMessage, ToolName);
			Out->SetBoolField(TEXT("rollback_attempted"), true);
			Out->SetBoolField(TEXT("rollback_verified"), bRollbackVerified);
			return false;
		};

		*Description = PersistableDescription;
		UStaticMesh::FCommitMeshDescriptionParams CommitParams;
		CommitParams.bMarkPackageDirty = true;
		CommitParams.bUseHashAsGuid = true;
		Target.Asset->CommitMeshDescription(Target.LodIndex, CommitParams);
		Target.Asset->PostEditChange();
		FMeshDescription* VerifyDescription = Target.Asset->GetMeshDescription(Target.LodIndex);
		FDynamicMesh3 VerifyMesh;
		if (!VerifyDescription)
		{
			return RollbackAndFail(TEXT("readback_unavailable"), TEXT("Post-commit MeshDescription readback is unavailable."));
		}
		if (!ConvertFromMeshDescription(VerifyDescription, VerifyMesh))
		{
			return RollbackAndFail(TEXT("post_commit_readback_mismatch"), TEXT("Committed geometry did not produce valid native readback; delivery was rejected."));
		}
		const FString VerifiedHash = MeshHash(VerifyMesh);
		if (VerifiedHash == Target.BeforeHash)
		{
			return RollbackAndFail(TEXT("post_commit_readback_mismatch"), TEXT("Committed geometry read back unchanged from the pre-mutation hash; the mutation was not persisted."));
		}
		// Fast path: the exact stream hash survived the engine round trip. Slow
		// path: UE's commit/readback may renormalize element IDs or derived
		// channels, so accept canonical geometry equivalence (positions at
		// source precision, winding-preserved topology, matching attribute
		// channel layout) and treat the readback hash as the persisted truth.
		bool bExactRoundTrip = (VerifiedHash == AfterHash);
		if (!bExactRoundTrip)
		{
			const bool bCanonicalEquivalent =
				VerifyMesh.VertexCount() == PersistableMesh.VertexCount()
				&& VerifyMesh.TriangleCount() == PersistableMesh.TriangleCount()
				&& ChannelFlagsMatch(VerifyMesh, PersistableMesh)
				&& CanonicalGeometryHash(VerifyMesh) == CanonicalGeometryHash(PersistableMesh);
			if (!bCanonicalEquivalent)
			{
				return RollbackAndFail(TEXT("post_commit_readback_mismatch"), TEXT("Committed geometry did not match native readback; delivery was rejected."));
			}
		}

		bool bSave = true;
		Args->TryGetBoolField(TEXT("save"), bSave);
		if (bSave)
		{
			FString SaveError;
			if (!Context.Services.SaveAsset(Target.AssetPath, false, SaveError))
			{
				return RollbackAndFail(
					TEXT("asset_save_failed"),
					SaveError.IsEmpty() ? TEXT("The modified Static Mesh could not be saved.") : SaveError);
			}
			bMutationSaved = true;
			FMeshDescription* SavedDescription = Target.Asset->GetMeshDescription(Target.LodIndex);
			FDynamicMesh3 SavedMesh;
			bool bSavedEquivalent = ConvertFromMeshDescription(SavedDescription, SavedMesh)
				&& MeshHash(SavedMesh) != Target.BeforeHash
				&& (MeshHash(SavedMesh) == VerifiedHash
					|| (SavedMesh.VertexCount() == PersistableMesh.VertexCount()
						&& SavedMesh.TriangleCount() == PersistableMesh.TriangleCount()
						&& ChannelFlagsMatch(SavedMesh, PersistableMesh)
						&& CanonicalGeometryHash(SavedMesh) == CanonicalGeometryHash(PersistableMesh)));
			if (!bSavedEquivalent)
			{
				return RollbackAndFail(TEXT("post_save_readback_mismatch"), TEXT("Saved Static Mesh source geometry did not match the committed hash; delivery was rejected."));
			}
			VerifyMesh = MoveTemp(SavedMesh);
		}
		Target.Mesh = VerifyMesh;
		TSharedRef<FJsonObject> TargetJson = MakeShared<FJsonObject>();
		TargetJson->SetStringField(TEXT("asset_path"), Target.AssetPath);
		TargetJson->SetNumberField(TEXT("lod_index"), Target.LodIndex);
		TSharedRef<FJsonObject> Readback = MeshReadback(VerifyMesh);
		Readback->SetStringField(TEXT("before_hash"), Target.BeforeHash);
		Readback->SetNumberField(TEXT("before_vertex_count"), Target.BeforeVertices);
		Readback->SetNumberField(TEXT("before_edge_count"), Target.BeforeEdges);
		Readback->SetNumberField(TEXT("before_triangle_count"), Target.BeforeTriangles);
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("operation"), ToolName);
		Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
		Receipt->SetBoolField(TEXT("mutation_applied"), true);
		Receipt->SetBoolField(TEXT("saved"), bSave);
		Receipt->SetBoolField(TEXT("verified"), true);
		Receipt->SetStringField(TEXT("before_hash"), Target.BeforeHash);
		Receipt->SetStringField(TEXT("after_hash"), VerifiedHash);
		Receipt->SetBoolField(TEXT("exact_round_trip"), bExactRoundTrip);
		Receipt->SetStringField(TEXT("intended_hash"), AfterHash);
		Receipt->SetStringField(TEXT("canonical_geometry_hash"), CanonicalGeometryHash(VerifyMesh));
		const FString ReceiptId = StoreReceipt(Receipt);
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
		Out->SetStringField(TEXT("receipt_id"), ReceiptId);
		Out->SetBoolField(TEXT("mutation_applied"), true);
		Out->SetBoolField(TEXT("saved"), bSave);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetObjectField(TEXT("target"), TargetJson);
		Out->SetObjectField(TEXT("readback"), Readback);
		Out->SetObjectField(TEXT("receipt"), Receipt);
		OutSummary = FString::Printf(TEXT("%s changed and verified %s LOD %d; receipt=%s"), *ToolName, *Target.AssetPath, Target.LodIndex, *ReceiptId);
		return true;
	}

	static bool ValidateTriangles(const FDynamicMesh3& Mesh, const TArray<int32>& Ids, FString& OutError, const bool bRequireAny = true)
	{
		if (bRequireAny && Ids.IsEmpty())
		{
			OutError = TEXT("triangle_ids must not be empty.");
			return false;
		}
		for (int32 Id : Ids)
		{
			if (!Mesh.IsTriangle(Id))
			{
				OutError = FString::Printf(TEXT("Triangle ID %d is not valid for the current geometry hash."), Id);
				return false;
			}
		}
		return true;
	}

	static bool ResolveVertices(const TSharedRef<FJsonObject>& Args, const FDynamicMesh3& Mesh, TArray<int32>& OutIds, FString& OutError)
	{
		if (!ReadIntArray(Args, TEXT("vertex_ids"), OutIds, 250000, OutError, false)) return false;
		if (OutIds.IsEmpty())
		{
			for (int32 VertexId : Mesh.VertexIndicesItr()) OutIds.Add(VertexId);
		}
		for (int32 Id : OutIds)
		{
			if (!Mesh.IsVertex(Id))
			{
				OutError = FString::Printf(TEXT("Vertex ID %d is not valid for the current geometry hash."), Id);
				return false;
			}
		}
		return true;
	}

	static void CollectSelectionVertices(const FDynamicMesh3& Mesh, const TArray<int32>& Triangles, TArray<int32>& OutVertices)
	{
		TSet<int32> Unique;
		for (int32 TriangleId : Triangles)
		{
			const FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			Unique.Add(Triangle.A);
			Unique.Add(Triangle.B);
			Unique.Add(Triangle.C);
		}
		OutVertices = Unique.Array();
		OutVertices.Sort();
	}

	static bool ExecuteReadSelection(
		EOperation Operation,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Args,
		FTarget& Target,
		TSharedRef<FJsonObject>& Out,
		FString& OutSummary,
		FString& OutError)
	{
		TArray<int32> Result;
		FString Kind = TEXT("triangles");
		if (Operation == EOperation::SelectVerticesBox || Operation == EOperation::SelectTrianglesBox)
		{
			FVector3d Min, Max;
			if (!ReadVector(Args, TEXT("min"), Min, OutError, FVector3d::Zero(), true) ||
				!ReadVector(Args, TEXT("max"), Max, OutError, FVector3d::Zero(), true) || Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
			{
				Fail(Args, Out, OutError, TEXT("invalid_selection_box"), OutError.IsEmpty() ? TEXT("min must be component-wise less than or equal to max.") : OutError, ToolName);
				return false;
			}
			if (Operation == EOperation::SelectVerticesBox)
			{
				Kind = TEXT("vertices");
				for (int32 Id : Target.Mesh.VertexIndicesItr())
				{
					const FVector3d P = Target.Mesh.GetVertex(Id);
					if (P.X >= Min.X && P.Y >= Min.Y && P.Z >= Min.Z && P.X <= Max.X && P.Y <= Max.Y && P.Z <= Max.Z) Result.Add(Id);
				}
			}
			else
			{
				for (int32 Id : Target.Mesh.TriangleIndicesItr())
				{
					const FVector3d P = Target.Mesh.GetTriCentroid(Id);
					if (P.X >= Min.X && P.Y >= Min.Y && P.Z >= Min.Z && P.X <= Max.X && P.Y <= Max.Y && P.Z <= Max.Z) Result.Add(Id);
				}
			}
		}
		else if (Operation == EOperation::SelectBoundaryEdges)
		{
			Kind = TEXT("edges");
			TArray<int32> Triangles;
			if (!ReadIntArray(Args, TEXT("triangle_ids"), Triangles, 250000, OutError, false))
			{
				Fail(Args, Out, OutError, TEXT("invalid_triangle_ids"), OutError, ToolName);
				return false;
			}
			if (Triangles.IsEmpty())
			{
				for (int32 EdgeId : Target.Mesh.EdgeIndicesItr()) if (Target.Mesh.IsBoundaryEdge(EdgeId)) Result.Add(EdgeId);
			}
			else
			{
				if (!ValidateTriangles(Target.Mesh, Triangles, OutError))
				{
					Fail(Args, Out, OutError, TEXT("invalid_triangle_ids"), OutError, ToolName);
					return false;
				}
				TSet<int32> Selected(Triangles);
				TSet<int32> Boundary;
				for (int32 TriangleId : Triangles)
				{
					const FIndex3i Edges = Target.Mesh.GetTriEdges(TriangleId);
					for (int32 EdgeId : {Edges.A, Edges.B, Edges.C})
					{
						const FIndex2i Adjacent = Target.Mesh.GetEdgeT(EdgeId);
						const int32 Other = Adjacent.A == TriangleId ? Adjacent.B : Adjacent.A;
						if (Other < 0 || !Selected.Contains(Other)) Boundary.Add(EdgeId);
					}
				}
				Result = Boundary.Array();
			}
		}
		else if (Operation == EOperation::SelectComponent)
		{
			double SeedNumber = -1;
			if (!Args->TryGetNumberField(TEXT("seed_triangle_id"), SeedNumber) || !Target.Mesh.IsTriangle(static_cast<int32>(SeedNumber)))
			{
				Fail(Args, Out, OutError, TEXT("invalid_seed_triangle"), TEXT("seed_triangle_id must identify a valid triangle."), ToolName);
				return false;
			}
			TSet<int32> Visited;
			TArray<int32> Queue = {static_cast<int32>(SeedNumber)};
			for (int32 Index = 0; Index < Queue.Num(); ++Index)
			{
				const int32 Current = Queue[Index];
				if (Visited.Contains(Current)) continue;
				Visited.Add(Current);
				const FIndex3i Neighbours = Target.Mesh.GetTriNeighbourTris(Current);
				for (int32 Next : {Neighbours.A, Neighbours.B, Neighbours.C}) if (Next >= 0 && !Visited.Contains(Next)) Queue.Add(Next);
			}
			Result = Visited.Array();
		}
		else
		{
			TArray<int32> Input;
			if (!ReadIntArray(Args, TEXT("triangle_ids"), Input, 250000, OutError) || !ValidateTriangles(Target.Mesh, Input, OutError))
			{
				Fail(Args, Out, OutError, TEXT("invalid_triangle_ids"), OutError, ToolName);
				return false;
			}
			TSet<int32> Selected(Input);
			if (Operation == EOperation::SelectInvert)
			{
				for (int32 TriangleId : Target.Mesh.TriangleIndicesItr()) if (!Selected.Contains(TriangleId)) Result.Add(TriangleId);
			}
			else
			{
				double IterationsNumber = 1;
				Args->TryGetNumberField(TEXT("iterations"), IterationsNumber);
				const int32 Iterations = FMath::Clamp(static_cast<int32>(IterationsNumber), 1, 64);
				for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
				{
					TSet<int32> Next = Selected;
					for (int32 TriangleId : Selected)
					{
						const FIndex3i Neighbours = Target.Mesh.GetTriNeighbourTris(TriangleId);
						if (Operation == EOperation::SelectGrow)
						{
							for (int32 Neighbour : {Neighbours.A, Neighbours.B, Neighbours.C}) if (Neighbour >= 0) Next.Add(Neighbour);
						}
						else
						{
							for (int32 Neighbour : {Neighbours.A, Neighbours.B, Neighbours.C})
							{
								if (Neighbour < 0 || !Selected.Contains(Neighbour)) { Next.Remove(TriangleId); break; }
							}
						}
					}
					Selected = MoveTemp(Next);
				}
				Result = Selected.Array();
			}
		}
		Result.Sort();
		TSharedRef<FJsonObject> Details = MeshReadback(Target.Mesh);
		Details->SetStringField(TEXT("selection_kind"), Kind);
		Details->SetNumberField(TEXT("selection_count"), Result.Num());
		Details->SetArrayField(TEXT("selection_ids"), IntArrayJson(Result));
		CompleteRead(ToolName, Target, Out, OutSummary, Details);
		return true;
	}

	static bool ApplyTopologyOperation(
		EOperation Operation,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Args,
		FTarget& Target,
		TSharedRef<FJsonObject>& Out,
		FString& OutError)
	{
		double IdNumber = -1;
		EMeshResult Result = EMeshResult::Failed_Unsupported;
		if (Operation == EOperation::EdgeSplit)
		{
			double T = 0.5;
			Args->TryGetNumberField(TEXT("edge_id"), IdNumber);
			Args->TryGetNumberField(TEXT("t"), T);
			const int32 EdgeId = static_cast<int32>(IdNumber);
			if (!Target.Mesh.IsEdge(EdgeId) || T <= 0.0 || T >= 1.0)
			{
				Fail(Args, Out, OutError, TEXT("invalid_edge_split"), TEXT("edge_id must be valid and t must be strictly between 0 and 1."), ToolName);
				return false;
			}
			FDynamicMesh3::FEdgeSplitInfo Info;
			Result = Target.Mesh.SplitEdge(EdgeId, Info, T);
		}
		else if (Operation == EOperation::EdgeCollapse)
		{
			double Keep = -1, Remove = -1, T = 0.0;
			Args->TryGetNumberField(TEXT("keep_vertex_id"), Keep);
			Args->TryGetNumberField(TEXT("remove_vertex_id"), Remove);
			Args->TryGetNumberField(TEXT("t"), T);
			if (!Target.Mesh.IsVertex(static_cast<int32>(Keep)) || !Target.Mesh.IsVertex(static_cast<int32>(Remove)) ||
				Target.Mesh.FindEdge(static_cast<int32>(Keep), static_cast<int32>(Remove)) < 0 || T < 0.0 || T > 1.0)
			{
				Fail(Args, Out, OutError, TEXT("invalid_edge_collapse"), TEXT("keep/remove vertices must define an existing edge and t must be in [0,1]."), ToolName);
				return false;
			}
			FDynamicMesh3::FEdgeCollapseInfo Info;
			Result = Target.Mesh.CollapseEdge(static_cast<int32>(Keep), static_cast<int32>(Remove), T, Info);
		}
		else if (Operation == EOperation::EdgeFlip)
		{
			Args->TryGetNumberField(TEXT("edge_id"), IdNumber);
			const int32 EdgeId = static_cast<int32>(IdNumber);
			if (!Target.Mesh.IsEdge(EdgeId) || Target.Mesh.IsBoundaryEdge(EdgeId))
			{
				Fail(Args, Out, OutError, TEXT("invalid_edge_flip"), TEXT("edge_id must identify a non-boundary edge."), ToolName);
				return false;
			}
			FDynamicMesh3::FEdgeFlipInfo Info;
			Result = Target.Mesh.FlipEdge(EdgeId, Info);
		}
		else if (Operation == EOperation::TrianglePoke)
		{
			Args->TryGetNumberField(TEXT("triangle_id"), IdNumber);
			const int32 TriangleId = static_cast<int32>(IdNumber);
			FVector3d Bary(1.0 / 3.0);
			ReadVector(Args, TEXT("barycentric"), Bary, OutError, Bary, false);
			if (!Target.Mesh.IsTriangle(TriangleId) || Bary.X < 0 || Bary.Y < 0 || Bary.Z < 0 || !FMath::IsNearlyEqual(Bary.X + Bary.Y + Bary.Z, 1.0, 1.e-5))
			{
				Fail(Args, Out, OutError, TEXT("invalid_triangle_poke"), TEXT("triangle_id must be valid and barycentric x/y/z must be non-negative and sum to 1."), ToolName);
				return false;
			}
			FDynamicMesh3::FPokeTriangleInfo Info;
			Result = Target.Mesh.PokeTriangle(TriangleId, Bary, Info);
		}
		else if (Operation == EOperation::VerticesWeld)
		{
			double Keep = -1, Discard = -1, T = 0;
			Args->TryGetNumberField(TEXT("keep_vertex_id"), Keep);
			Args->TryGetNumberField(TEXT("discard_vertex_id"), Discard);
			Args->TryGetNumberField(TEXT("t"), T);
			if (!Target.Mesh.IsVertex(static_cast<int32>(Keep)) || !Target.Mesh.IsVertex(static_cast<int32>(Discard)) || Keep == Discard || T < 0 || T > 1)
			{
				Fail(Args, Out, OutError, TEXT("invalid_vertex_weld"), TEXT("keep/discard vertices must be distinct valid IDs and t must be in [0,1]."), ToolName);
				return false;
			}
			FDynamicMesh3::FMergeVerticesInfo Info;
			Result = Target.Mesh.MergeVertices(static_cast<int32>(Keep), static_cast<int32>(Discard), T, Info);
		}
		if (Result != EMeshResult::Ok)
		{
			Fail(Args, Out, OutError, TEXT("native_topology_operation_rejected"), FString::Printf(TEXT("Native topology operation returned EMeshResult %d; no asset write was attempted."), static_cast<int32>(Result)), ToolName);
			return false;
		}
		return true;
	}

	static bool ApplyRegionOperation(
		EOperation Operation,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Args,
		FTarget& Target,
		TSharedRef<FJsonObject>& Out,
		FString& OutError)
	{
		TArray<int32> Triangles;
		if (Operation != EOperation::Compact && Operation != EOperation::OrientationReverse)
		{
			if (!ReadIntArray(Args, TEXT("triangle_ids"), Triangles, 250000, OutError, Operation == EOperation::PlaneTrim ? false : true) ||
				(!Triangles.IsEmpty() && !ValidateTriangles(Target.Mesh, Triangles, OutError)))
			{
				Fail(Args, Out, OutError, TEXT("invalid_triangle_ids"), OutError, ToolName);
				return false;
			}
		}
		if (Operation == EOperation::TrianglesDelete)
		{
			bool bAllowEmpty = false;
			Args->TryGetBoolField(TEXT("allow_empty"), bAllowEmpty);
			if (!bAllowEmpty && Triangles.Num() >= Target.Mesh.TriangleCount())
			{
				Fail(Args, Out, OutError, TEXT("empty_mesh_guard"), TEXT("Deleting all triangles requires allow_empty=true, but empty meshes are not commit-deliverable."), ToolName);
				return false;
			}
			for (int32 TriangleId : Triangles)
			{
				if (Target.Mesh.RemoveTriangle(TriangleId, true, true) != EMeshResult::Ok)
				{
					Fail(Args, Out, OutError, TEXT("triangle_delete_rejected"), FString::Printf(TEXT("Native removal rejected triangle %d; no asset write was attempted."), TriangleId), ToolName);
					return false;
				}
			}
		}
		else if (Operation == EOperation::OrientationReverse)
		{
			ReadIntArray(Args, TEXT("triangle_ids"), Triangles, 250000, OutError, false);
			if (Triangles.IsEmpty()) Target.Mesh.ReverseOrientation(true);
			else
			{
				if (!ValidateTriangles(Target.Mesh, Triangles, OutError))
				{
					Fail(Args, Out, OutError, TEXT("invalid_triangle_ids"), OutError, ToolName);
					return false;
				}
				for (int32 TriangleId : Triangles)
				{
					if (Target.Mesh.ReverseTriOrientation(TriangleId) != EMeshResult::Ok)
					{
						Fail(Args, Out, OutError, TEXT("orientation_reverse_rejected"), TEXT("Selected orientation reversal would create invalid topology."), ToolName);
						return false;
					}
				}
			}
		}
		else if (Operation == EOperation::Compact)
		{
			if (Target.Mesh.IsCompact())
			{
				Fail(Args, Out, OutError, TEXT("mesh_already_compact"), TEXT("The mesh is already compact; parameter-only success is forbidden."), ToolName);
				return false;
			}
			Target.Mesh.CompactInPlace();
		}
		else if (Operation == EOperation::HoleFill)
		{
			double LoopNumber = -1;
			Args->TryGetNumberField(TEXT("loop_index"), LoopNumber);
			FMeshBoundaryLoops Loops(&Target.Mesh, true);
			const int32 LoopIndex = static_cast<int32>(LoopNumber);
			if (Loops.bAborted || LoopIndex < 0 || LoopIndex >= Loops.Loops.Num() || Loops.Loops[LoopIndex].Vertices.Num() < 3)
			{
				Fail(Args, Out, OutError, TEXT("invalid_boundary_loop"), TEXT("loop_index must identify a closed boundary loop with at least three vertices."), ToolName);
				return false;
			}
			FVector3d Center = FVector3d::Zero();
			for (int32 VertexId : Loops.Loops[LoopIndex].Vertices) Center += Target.Mesh.GetVertex(VertexId);
			Center /= static_cast<double>(Loops.Loops[LoopIndex].Vertices.Num());
			const int32 CenterId = Target.Mesh.AppendVertex(Center);
			FDynamicMeshEditor Editor(&Target.Mesh);
			FDynamicMeshEditResult Result;
			if (!Editor.AddTriangleFan_OrderedVertexLoop(CenterId, Loops.Loops[LoopIndex].Vertices, 0, Result) || Result.NewTriangles.IsEmpty())
			{
				Fail(Args, Out, OutError, TEXT("hole_fill_rejected"), TEXT("The native triangle-fan fill could not produce manifold triangles."), ToolName);
				return false;
			}
		}
		else if (Operation == EOperation::FacesInset)
		{
			double Amount = 1.0;
			Args->TryGetNumberField(TEXT("amount"), Amount);
			if (!FMath::IsFinite(Amount) || Amount <= 0)
			{
				Fail(Args, Out, OutError, TEXT("invalid_inset_amount"), TEXT("amount must be a finite positive local-space inset distance."), ToolName);
				return false;
			}

			const TSet<int32> Selected(Triangles);
			int32 BoundaryEdgeCount = 0;
			for (int32 TriangleId : Triangles)
			{
				const FIndex3i Edges = Target.Mesh.GetTriEdges(TriangleId);
				for (int32 EdgeId : {Edges.A, Edges.B, Edges.C})
				{
					const FIndex2i Adjacent = Target.Mesh.GetEdgeT(EdgeId);
					const int32 Other = Adjacent.A == TriangleId ? Adjacent.B : Adjacent.A;
					if (Other < 0 || !Selected.Contains(Other)) ++BoundaryEdgeCount;
				}
			}
			if (BoundaryEdgeCount == 0)
			{
				Fail(Args, Out, OutError, TEXT("inset_region_has_no_boundary"), TEXT("The selected region has no boundary loop and cannot be inset reliably."), ToolName);
				return false;
			}

			FInsetMeshRegion Inset(&Target.Mesh);
			Inset.Triangles = Triangles;
			Inset.InsetDistance = Amount;
			Inset.bReproject = true;
			Inset.bSolveRegionInteriors = true;
			Inset.Softness = 0.0f;
			Inset.AreaCorrection = 1.0f;
			if (Inset.Validate() != EOperationValidationResult::Ok || !Inset.Apply() || Inset.AllModifiedTriangles.IsEmpty())
			{
				Fail(Args, Out, OutError, TEXT("native_inset_rejected"), TEXT("The native region inset could not produce a valid stitched inset; no asset write was attempted."), ToolName);
				return false;
			}
		}
		else if (Operation == EOperation::FacesExtrude)
		{
			double Distance = 0;
			Args->TryGetNumberField(TEXT("distance"), Distance);
			if (!FMath::IsFinite(Distance) || FMath::IsNearlyZero(Distance))
			{
				Fail(Args, Out, OutError, TEXT("invalid_extrude_distance"), TEXT("distance must be finite and non-zero."), ToolName);
				return false;
			}
			TSet<int32> Selected(Triangles);
			TArray<int32> Vertices;
			CollectSelectionVertices(Target.Mesh, Triangles, Vertices);
			FMeshNormals Normals(&Target.Mesh);
			Normals.ComputeVertexNormals(true, true);
			TMap<int32, int32> Duplicates;
			for (int32 VertexId : Vertices)
			{
				const FVector3d Position = Target.Mesh.GetVertex(VertexId) + Normals[VertexId] * Distance;
				Duplicates.Add(VertexId, Target.Mesh.AppendVertex(Position));
			}
			struct FBoundaryEdge { int32 A; int32 B; };
			TArray<FBoundaryEdge> Boundary;
			for (int32 TriangleId : Triangles)
			{
				const FIndex3i Tri = Target.Mesh.GetTriangle(TriangleId);
				const int32 V[3] = {Tri.A, Tri.B, Tri.C};
				const FIndex3i Edges = Target.Mesh.GetTriEdges(TriangleId);
				const int32 E[3] = {Edges.A, Edges.B, Edges.C};
				for (int32 Side = 0; Side < 3; ++Side)
				{
					const FIndex2i Adjacent = Target.Mesh.GetEdgeT(E[Side]);
					const int32 Other = Adjacent.A == TriangleId ? Adjacent.B : Adjacent.A;
					if (Other < 0 || !Selected.Contains(Other)) Boundary.Add({V[Side], V[(Side + 1) % 3]});
				}
			}
			TArray<FIndex3i> Caps;
			for (int32 TriangleId : Triangles)
			{
				const FIndex3i Tri = Target.Mesh.GetTriangle(TriangleId);
				Caps.Add(FIndex3i(Duplicates[Tri.A], Duplicates[Tri.B], Duplicates[Tri.C]));
			}
			for (int32 TriangleId : Triangles) Target.Mesh.RemoveTriangle(TriangleId, false, true);
			for (const FIndex3i& Cap : Caps)
			{
				if (Target.Mesh.AppendTriangle(Cap) < 0)
				{
					Fail(Args, Out, OutError, TEXT("extrude_cap_rejected"), TEXT("Extruded cap would create duplicate or non-manifold triangles."), ToolName);
					return false;
				}
			}
			for (const FBoundaryEdge& Edge : Boundary)
			{
				const int32 NewA = Duplicates[Edge.A];
				const int32 NewB = Duplicates[Edge.B];
				if (Target.Mesh.AppendTriangle(Edge.A, Edge.B, NewB) < 0 || Target.Mesh.AppendTriangle(Edge.A, NewB, NewA) < 0)
				{
					Fail(Args, Out, OutError, TEXT("extrude_wall_rejected"), TEXT("Extruded side walls would create invalid topology."), ToolName);
					return false;
				}
			}
		}
		else if (Operation == EOperation::PlaneTrim)
		{
			FVector3d Origin, Normal;
			if (!ReadVector(Args, TEXT("plane_origin"), Origin, OutError, FVector3d::Zero(), true) ||
				!ReadVector(Args, TEXT("plane_normal"), Normal, OutError, FVector3d::UnitZ(), true) || !Normal.Normalize())
			{
				Fail(Args, Out, OutError, TEXT("invalid_trim_plane"), OutError.IsEmpty() ? TEXT("plane_normal must be non-zero.") : OutError, ToolName);
				return false;
			}
			bool bKeepPositive = true, bAllowIntersecting = false;
			Args->TryGetBoolField(TEXT("keep_positive"), bKeepPositive);
			Args->TryGetBoolField(TEXT("allow_intersecting_triangles"), bAllowIntersecting);
			TArray<int32> Remove;
			for (int32 TriangleId : Target.Mesh.TriangleIndicesItr())
			{
				const FIndex3i Tri = Target.Mesh.GetTriangle(TriangleId);
				const double D[3] = {
					(Target.Mesh.GetVertex(Tri.A) - Origin).Dot(Normal),
					(Target.Mesh.GetVertex(Tri.B) - Origin).Dot(Normal),
					(Target.Mesh.GetVertex(Tri.C) - Origin).Dot(Normal)};
				const bool HasPositive = D[0] > 0 || D[1] > 0 || D[2] > 0;
				const bool HasNegative = D[0] < 0 || D[1] < 0 || D[2] < 0;
				if (HasPositive && HasNegative && !bAllowIntersecting)
				{
					Fail(Args, Out, OutError, TEXT("plane_intersects_triangles"), TEXT("The plane intersects triangles. This conservative native tool fails closed unless allow_intersecting_triangles=true."), ToolName);
					return false;
				}
				const double CenterDistance = (D[0] + D[1] + D[2]) / 3.0;
				if ((bKeepPositive && CenterDistance < 0) || (!bKeepPositive && CenterDistance > 0)) Remove.Add(TriangleId);
			}
			if (Remove.IsEmpty() || Remove.Num() >= Target.Mesh.TriangleCount())
			{
				Fail(Args, Out, OutError, TEXT("trim_would_not_deliver_mesh"), TEXT("The plane trim must remove at least one but not all triangles."), ToolName);
				return false;
			}
			for (int32 TriangleId : Remove) Target.Mesh.RemoveTriangle(TriangleId, true, false);
		}
		return true;
	}

	static FDynamicMeshNormalOverlay* RequireNormalOverlay(FDynamicMesh3& Mesh)
	{
		if (!Mesh.HasAttributes()) Mesh.EnableAttributes();
		if (Mesh.Attributes()->NumNormalLayers() == 0) Mesh.Attributes()->SetNumNormalLayers(1);
		FDynamicMeshNormalOverlay* Overlay = Mesh.Attributes()->PrimaryNormals();
		if (Overlay && Overlay->ElementCount() == 0) Overlay->CreatePerVertex(0.0f, true);
		return Overlay;
	}

	static FDynamicMeshUVOverlay* RequirePrimaryUvOverlay(FDynamicMesh3& Mesh)
	{
		if (!Mesh.HasAttributes()) Mesh.EnableAttributes();
		if (Mesh.Attributes()->NumUVLayers() == 0) Mesh.Attributes()->SetNumUVLayers(1);
		FDynamicMeshUVOverlay* Overlay = Mesh.Attributes()->PrimaryUV();
		if (Overlay && Overlay->ElementCount() == 0) Overlay->CreatePerVertex(0.0f, true);
		return Overlay;
	}

	static FDynamicMeshColorOverlay* RequireColorOverlay(FDynamicMesh3& Mesh)
	{
		if (!Mesh.HasAttributes()) Mesh.EnableAttributes();
		if (!Mesh.Attributes()->HasPrimaryColors()) Mesh.Attributes()->EnablePrimaryColors();
		FDynamicMeshColorOverlay* Overlay = Mesh.Attributes()->PrimaryColors();
		if (Overlay && Overlay->ElementCount() == 0) Overlay->CreatePerVertex(1.0f, true);
		return Overlay;
	}

	static bool ApplyVertexOperation(
		EOperation Operation,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Args,
		FTarget& Target,
		TSharedRef<FJsonObject>& Out,
		FString& OutError)
	{
		TArray<int32> Vertices;
		if (!ResolveVertices(Args, Target.Mesh, Vertices, OutError))
		{
			Fail(Args, Out, OutError, TEXT("invalid_vertex_ids"), OutError, ToolName);
			return false;
		}
		FVector3d Pivot, Axis;
		ReadVector(Args, TEXT("pivot"), Pivot, OutError, Target.Mesh.GetBounds().Center(), false);
		ReadVector(Args, TEXT("axis"), Axis, OutError, FVector3d::UnitZ(), false);
		if (!Axis.Normalize()) Axis = FVector3d::UnitZ();
		if (Operation == EOperation::SculptSmooth)
		{
			double Alpha = 0.25, IterationNumber = 1;
			Args->TryGetNumberField(TEXT("strength"), Alpha);
			Args->TryGetNumberField(TEXT("iterations"), IterationNumber);
			Alpha = FMath::Clamp(Alpha, 0.0, 1.0);
			const int32 Iterations = FMath::Clamp(static_cast<int32>(IterationNumber), 1, 100);
			for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
			{
				TMap<int32, FVector3d> Updates;
				for (int32 VertexId : Vertices)
				{
					FVector3d Average = FVector3d::Zero();
					int32 Count = 0;
					for (int32 Neighbour : Target.Mesh.VtxVerticesItr(VertexId)) { Average += Target.Mesh.GetVertex(Neighbour); ++Count; }
					if (Count > 0) Updates.Add(VertexId, FMath::Lerp(Target.Mesh.GetVertex(VertexId), Average / Count, Alpha));
				}
				for (const TPair<int32, FVector3d>& Pair : Updates) Target.Mesh.SetVertex(Pair.Key, Pair.Value);
			}
		}
		else if (Operation == EOperation::SculptInflate)
		{
			double Distance = 1.0;
			Args->TryGetNumberField(TEXT("distance"), Distance);
			if (!FMath::IsFinite(Distance) || FMath::IsNearlyZero(Distance))
			{
				Fail(Args, Out, OutError, TEXT("invalid_inflate_distance"), TEXT("distance must be finite and non-zero."), ToolName);
				return false;
			}
			FMeshNormals Normals(&Target.Mesh);
			Normals.ComputeVertexNormals(true, true);
			for (int32 VertexId : Vertices) Target.Mesh.SetVertex(VertexId, Target.Mesh.GetVertex(VertexId) + Normals[VertexId] * Distance);
		}
		else if (Operation == EOperation::SculptFlatten)
		{
			FVector3d Origin, Normal;
			double Strength = 1.0;
			ReadVector(Args, TEXT("plane_origin"), Origin, OutError, Pivot, false);
			ReadVector(Args, TEXT("plane_normal"), Normal, OutError, Axis, false);
			Args->TryGetNumberField(TEXT("strength"), Strength);
			if (!Normal.Normalize() || Strength <= 0 || Strength > 1)
			{
				Fail(Args, Out, OutError, TEXT("invalid_flatten_parameters"), TEXT("plane_normal must be non-zero and strength must be in (0,1]."), ToolName);
				return false;
			}
			for (int32 VertexId : Vertices)
			{
				const FVector3d P = Target.Mesh.GetVertex(VertexId);
				const FVector3d Projected = P - Normal * (P - Origin).Dot(Normal);
				Target.Mesh.SetVertex(VertexId, FMath::Lerp(P, Projected, Strength));
			}
		}
		else if (Operation == EOperation::DeformTranslate)
		{
			FVector3d Delta;
			if (!ReadVector(Args, TEXT("delta"), Delta, OutError, FVector3d::Zero(), true) || Delta.IsNearlyZero())
			{
				Fail(Args, Out, OutError, TEXT("invalid_translation"), OutError.IsEmpty() ? TEXT("delta must be non-zero.") : OutError, ToolName);
				return false;
			}
			for (int32 VertexId : Vertices) Target.Mesh.SetVertex(VertexId, Target.Mesh.GetVertex(VertexId) + Delta);
		}
		else if (Operation == EOperation::DeformRotate)
		{
			double Degrees = 0;
			Args->TryGetNumberField(TEXT("degrees"), Degrees);
			if (!FMath::IsFinite(Degrees) || FMath::IsNearlyZero(Degrees))
			{
				Fail(Args, Out, OutError, TEXT("invalid_rotation"), TEXT("degrees must be finite and non-zero."), ToolName);
				return false;
			}
			const FQuat4d Rotation(Axis, FMath::DegreesToRadians(Degrees));
			for (int32 VertexId : Vertices) Target.Mesh.SetVertex(VertexId, Pivot + Rotation.RotateVector(Target.Mesh.GetVertex(VertexId) - Pivot));
		}
		else if (Operation == EOperation::DeformScale)
		{
			FVector3d Scale;
			if (!ReadVector(Args, TEXT("scale"), Scale, OutError, FVector3d::One(), true) ||
				FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y) || FMath::IsNearlyZero(Scale.Z) || Scale.Equals(FVector3d::One()))
			{
				Fail(Args, Out, OutError, TEXT("invalid_scale"), OutError.IsEmpty() ? TEXT("scale must be finite, non-zero, and not identity.") : OutError, ToolName);
				return false;
			}
			for (int32 VertexId : Vertices) Target.Mesh.SetVertex(VertexId, Pivot + (Target.Mesh.GetVertex(VertexId) - Pivot) * Scale);
		}
		else if (Operation == EOperation::DeformTwist || Operation == EOperation::DeformBend || Operation == EOperation::DeformTaper)
		{
			double Strength = 0;
			Args->TryGetNumberField(TEXT("strength"), Strength);
			if (!FMath::IsFinite(Strength) || FMath::IsNearlyZero(Strength))
			{
				Fail(Args, Out, OutError, TEXT("invalid_deform_strength"), TEXT("strength must be finite and non-zero."), ToolName);
				return false;
			}
			FVector3d BasisX = FVector3d::UnitX();
			if (FMath::Abs(BasisX.Dot(Axis)) > 0.9) BasisX = FVector3d::UnitY();
			BasisX = (BasisX - Axis * BasisX.Dot(Axis)).GetSafeNormal();
			const FVector3d BasisY = Axis.Cross(BasisX).GetSafeNormal();
			for (int32 VertexId : Vertices)
			{
				const FVector3d Local = Target.Mesh.GetVertex(VertexId) - Pivot;
				const double Height = Local.Dot(Axis);
				const double X = Local.Dot(BasisX);
				const double Y = Local.Dot(BasisY);
				if (Operation == EOperation::DeformTwist)
				{
					const double Angle = FMath::DegreesToRadians(Strength * Height);
					Target.Mesh.SetVertex(VertexId, Pivot + Axis * Height + BasisX * (X * FMath::Cos(Angle) - Y * FMath::Sin(Angle)) + BasisY * (X * FMath::Sin(Angle) + Y * FMath::Cos(Angle)));
				}
				else if (Operation == EOperation::DeformBend)
				{
					const double Angle = FMath::DegreesToRadians(Strength * Height);
					Target.Mesh.SetVertex(VertexId, Pivot + Axis * Height + BasisX * (X * FMath::Cos(Angle) - Height * FMath::Sin(Angle)) + BasisY * Y);
				}
				else
				{
					const double Factor = FMath::Max(0.001, 1.0 + Strength * Height);
					Target.Mesh.SetVertex(VertexId, Pivot + Axis * Height + (BasisX * X + BasisY * Y) * Factor);
				}
			}
		}
		else if (Operation == EOperation::NormalsRecompute)
		{
			FMeshNormals Normals(&Target.Mesh);
			Normals.ComputeVertexNormals(true, true);
			FDynamicMeshNormalOverlay* Overlay = RequireNormalOverlay(Target.Mesh);
			if (!Overlay)
			{
				Fail(Args, Out, OutError, TEXT("normal_overlay_unavailable"), TEXT("A persistent normal overlay could not be created."), ToolName);
				return false;
			}
			const TSet<int32> Selected(Vertices);
			for (int32 ElementId : Overlay->ElementIndicesItr())
			{
				const int32 VertexId = Overlay->GetParentVertex(ElementId);
				if (Selected.Contains(VertexId)) Overlay->SetElement(ElementId, FVector3f(Normals[VertexId]));
			}
		}
		else if (Operation == EOperation::VertexColorsFill)
		{
			FVector3d Color;
			if (!ReadVector(Args, TEXT("color"), Color, OutError, FVector3d::One(), true) ||
				Color.X < 0 || Color.X > 1 || Color.Y < 0 || Color.Y > 1 || Color.Z < 0 || Color.Z > 1)
			{
				Fail(Args, Out, OutError, TEXT("invalid_vertex_color"), TEXT("color x/y/z must each be in [0,1]."), ToolName);
				return false;
			}
			FDynamicMeshColorOverlay* Overlay = RequireColorOverlay(Target.Mesh);
			if (!Overlay)
			{
				Fail(Args, Out, OutError, TEXT("color_overlay_unavailable"), TEXT("A persistent color overlay could not be created."), ToolName);
				return false;
			}
			const TSet<int32> Selected(Vertices);
			for (int32 ElementId : Overlay->ElementIndicesItr())
			{
				if (!Selected.Contains(Overlay->GetParentVertex(ElementId))) continue;
				FVector4f Value = Overlay->GetElement(ElementId);
				Value.X = Color.X;
				Value.Y = Color.Y;
				Value.Z = Color.Z;
				Overlay->SetElement(ElementId, Value);
			}
		}
		else if (Operation == EOperation::UvPlanarProject)
		{
			FString Axes = TEXT("xy");
			double Scale = 0.01;
			Args->TryGetStringField(TEXT("axes"), Axes);
			Args->TryGetNumberField(TEXT("uv_scale"), Scale);
			if (!FMath::IsFinite(Scale) || FMath::IsNearlyZero(Scale))
			{
				Fail(Args, Out, OutError, TEXT("invalid_uv_scale"), TEXT("uv_scale must be finite and non-zero."), ToolName);
				return false;
			}
			FDynamicMeshUVOverlay* Overlay = RequirePrimaryUvOverlay(Target.Mesh);
			if (!Overlay)
			{
				Fail(Args, Out, OutError, TEXT("uv_overlay_unavailable"), TEXT("A persistent primary UV overlay could not be created."), ToolName);
				return false;
			}
			const TSet<int32> Selected(Vertices);
			for (int32 ElementId : Overlay->ElementIndicesItr())
			{
				const int32 VertexId = Overlay->GetParentVertex(ElementId);
				if (!Selected.Contains(VertexId)) continue;
				const FVector3d P = Target.Mesh.GetVertex(VertexId);
				const FVector2f UV = Axes == TEXT("xz") ? FVector2f(P.X, P.Z) * Scale : Axes == TEXT("yz") ? FVector2f(P.Y, P.Z) * Scale : FVector2f(P.X, P.Y) * Scale;
				Overlay->SetElement(ElementId, UV);
			}
		}
		else if (Operation == EOperation::BakePositionColor)
		{
			FDynamicMeshColorOverlay* Overlay = RequireColorOverlay(Target.Mesh);
			if (!Overlay)
			{
				Fail(Args, Out, OutError, TEXT("color_overlay_unavailable"), TEXT("A persistent color overlay could not be created."), ToolName);
				return false;
			}
			const FAxisAlignedBox3d Bounds = Target.Mesh.GetBoundsForVertexSelection(Vertices);
			const FVector3d Size = Bounds.Diagonal();
			const TSet<int32> Selected(Vertices);
			for (int32 ElementId : Overlay->ElementIndicesItr())
			{
				const int32 VertexId = Overlay->GetParentVertex(ElementId);
				if (!Selected.Contains(VertexId)) continue;
				const FVector3d P = Target.Mesh.GetVertex(VertexId);
				const FVector4f C(
					Size.X > UE_DOUBLE_SMALL_NUMBER ? (P.X - Bounds.Min.X) / Size.X : 0.5,
					Size.Y > UE_DOUBLE_SMALL_NUMBER ? (P.Y - Bounds.Min.Y) / Size.Y : 0.5,
					Size.Z > UE_DOUBLE_SMALL_NUMBER ? (P.Z - Bounds.Min.Z) / Size.Z : 0.5,
					Overlay->GetElement(ElementId).W);
				Overlay->SetElement(ElementId, C);
			}
		}
		else if (Operation == EOperation::BakeNormalColor)
		{
			FMeshNormals Normals(&Target.Mesh);
			Normals.ComputeVertexNormals(true, true);
			FDynamicMeshColorOverlay* Overlay = RequireColorOverlay(Target.Mesh);
			if (!Overlay)
			{
				Fail(Args, Out, OutError, TEXT("color_overlay_unavailable"), TEXT("A persistent color overlay could not be created."), ToolName);
				return false;
			}
			const TSet<int32> Selected(Vertices);
			for (int32 ElementId : Overlay->ElementIndicesItr())
			{
				const int32 VertexId = Overlay->GetParentVertex(ElementId);
				if (!Selected.Contains(VertexId)) continue;
				const FVector3d N = Normals[VertexId].GetSafeNormal();
				const FVector3d Encoded = N * 0.5 + FVector3d(0.5);
				Overlay->SetElement(ElementId, FVector4f(Encoded.X, Encoded.Y, Encoded.Z, Overlay->GetElement(ElementId).W));
			}
		}
		return true;
	}

	static bool ApplyTriangleAttributeOperation(
		EOperation Operation,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Args,
		FTarget& Target,
		TSharedRef<FJsonObject>& Out,
		FString& OutError)
	{
		TArray<int32> Triangles;
		if (!ReadIntArray(Args, TEXT("triangle_ids"), Triangles, 250000, OutError) || !ValidateTriangles(Target.Mesh, Triangles, OutError))
		{
			Fail(Args, Out, OutError, TEXT("invalid_triangle_ids"), OutError, ToolName);
			return false;
		}
		double ValueNumber = 0;
		Args->TryGetNumberField(Operation == EOperation::MaterialIdsSet ? TEXT("material_id") : TEXT("polygroup_id"), ValueNumber);
		const int32 Value = static_cast<int32>(ValueNumber);
		if (Value < 0)
		{
			Fail(Args, Out, OutError, TEXT("invalid_attribute_value"), TEXT("material_id/polygroup_id must be non-negative."), ToolName);
			return false;
		}
		if (Operation == EOperation::MaterialIdsSet)
		{
			if (!Target.Mesh.HasAttributes()) Target.Mesh.EnableAttributes();
			if (!Target.Mesh.Attributes()->HasMaterialID()) Target.Mesh.Attributes()->EnableMaterialID();
			for (int32 TriangleId : Triangles) Target.Mesh.Attributes()->GetMaterialID()->SetValue(TriangleId, Value);
		}
		else
		{
			if (!Target.Mesh.HasTriangleGroups()) Target.Mesh.EnableTriangleGroups(0);
			for (int32 TriangleId : Triangles) Target.Mesh.SetTriangleGroup(TriangleId, Value);
		}
		return true;
	}

	static bool ExecuteOperation(
		const FSololmcpToolExecutionContext& Context,
		const FToolSpec& Spec,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& OutSummary,
		FString& OutError)
	{
		const FString ToolName(Spec.Name);
		if (Spec.Operation == EOperation::ReceiptGet)
		{
			FString ReceiptId;
			if (!Args->TryGetStringField(TEXT("receipt_id"), ReceiptId) || ReceiptId.IsEmpty())
			{
				Fail(Args, Out, OutError, TEXT("missing_receipt_id"), TEXT("receipt_id is required."), ToolName);
				return false;
			}
			TSharedPtr<FJsonObject> Stored;
			{
				FScopeLock Lock(&StateLock);
				Stored = Receipts.FindRef(ReceiptId);
			}
			if (!Stored.IsValid())
			{
				Fail(Args, Out, OutError, TEXT("receipt_not_found"), TEXT("The receipt is not present in this editor session."), ToolName);
				return false;
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			Out->SetStringField(TEXT("receipt_id"), ReceiptId);
			Out->SetObjectField(TEXT("receipt"), Stored.ToSharedRef());
			Out->SetObjectField(TEXT("readback"), Stored.ToSharedRef());
			Out->SetObjectField(TEXT("target"), MakeShared<FJsonObject>());
			OutSummary = FString::Printf(TEXT("Returned Modeling receipt %s."), *ReceiptId);
			return true;
		}

		FTarget Target;
		if (!LoadTarget(Context, Args, Target, Out, OutError, ToolName)) return false;

		if (Spec.Operation == EOperation::Inspect || Spec.Operation == EOperation::Validate)
		{
			CompleteRead(ToolName, Target, Out, OutSummary);
			return true;
		}
		if (Spec.Operation >= EOperation::SelectVerticesBox && Spec.Operation <= EOperation::SelectComponent)
		{
			return ExecuteReadSelection(Spec.Operation, ToolName, Args, Target, Out, OutSummary, OutError);
		}
		if (Spec.Operation == EOperation::SnapshotCreate)
		{
			const FString SnapshotId = FString::Printf(TEXT("mesh_snapshot_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(20).ToLower());
			{
				FScopeLock Lock(&StateLock);
				Snapshots.Add(SnapshotId, {Target.AssetPath, Target.LodIndex, Target.BeforeHash, MakeShared<FDynamicMesh3>(Target.Mesh)});
				if (Snapshots.Num() > 128) Snapshots.Remove(Snapshots.CreateConstIterator().Key());
			}
			TSharedRef<FJsonObject> Details = MeshReadback(Target.Mesh);
			Details->SetStringField(TEXT("snapshot_id"), SnapshotId);
			CompleteRead(ToolName, Target, Out, OutSummary, Details);
			return true;
		}
		if (Spec.Operation == EOperation::SnapshotRestore)
		{
			FString SnapshotId;
			Args->TryGetStringField(TEXT("snapshot_id"), SnapshotId);
			FSnapshot Snapshot;
			{
				FScopeLock Lock(&StateLock);
				const FSnapshot* Found = Snapshots.Find(SnapshotId);
				if (Found) Snapshot = *Found;
			}
			if (!Snapshot.Mesh.IsValid() || Snapshot.AssetPath != Target.AssetPath || Snapshot.LodIndex != Target.LodIndex)
			{
				Fail(Args, Out, OutError, TEXT("snapshot_target_mismatch"), TEXT("snapshot_id is missing or does not belong to the requested asset_path/lod_index."), ToolName);
				return false;
			}
			Target.Mesh = *Snapshot.Mesh;
		}
		else if (Spec.Operation >= EOperation::EdgeSplit && Spec.Operation <= EOperation::TrianglePoke || Spec.Operation == EOperation::VerticesWeld)
		{
			if (!ApplyTopologyOperation(Spec.Operation, ToolName, Args, Target, Out, OutError)) return false;
		}
		else if (Spec.Operation >= EOperation::TrianglesDelete && Spec.Operation <= EOperation::PlaneTrim)
		{
			if (!ApplyRegionOperation(Spec.Operation, ToolName, Args, Target, Out, OutError)) return false;
		}
		else if (Spec.Operation == EOperation::MaterialIdsSet || Spec.Operation == EOperation::PolygroupsSet)
		{
			if (!ApplyTriangleAttributeOperation(Spec.Operation, ToolName, Args, Target, Out, OutError)) return false;
		}
		else
		{
			if (!ApplyVertexOperation(Spec.Operation, ToolName, Args, Target, Out, OutError)) return false;
		}
		return CommitTarget(Context, Args, ToolName, Target, Out, OutSummary, OutError);
	}

#endif

	static FSchema VectorSchema(const FString& Description)
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("X component."))},
			{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("Y component."))},
			{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("Z component."))}},
			{TEXT("x"), TEXT("y"), TEXT("z")}, Description, false);
	}

	static void CloseSchemaRecursively(const FSchema& Schema)
	{
		auto CloseObjectField = [&](const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (Schema->TryGetObjectField(Field, Child) && Child && Child->IsValid())
			{
				CloseSchemaRecursively((*Child).ToSharedRef());
			}
		};
		auto CloseSchemaMap = [&](const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Map = nullptr;
			if (!Schema->TryGetObjectField(Field, Map) || !Map || !Map->IsValid()) return;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Map)->Values)
			{
				if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
				{
					CloseSchemaRecursively(Pair.Value->AsObject().ToSharedRef());
				}
			}
		};
		auto CloseSchemaArray = [&](const TCHAR* Field)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Schema->TryGetArrayField(Field, Values) || !Values) return;
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (Value.IsValid() && Value->Type == EJson::Object)
				{
					CloseSchemaRecursively(Value->AsObject().ToSharedRef());
				}
			}
		};

		FString Type;
		if (Schema->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object"))
		{
			// Dynamic-key maps are allowed only when additionalProperties is an
			// explicit value schema. Bare/unset/true object expansion is closed.
			const TSharedPtr<FJsonObject>* ControlledValues = nullptr;
			if (Schema->TryGetObjectField(TEXT("additionalProperties"), ControlledValues) && ControlledValues && ControlledValues->IsValid())
			{
				CloseSchemaRecursively((*ControlledValues).ToSharedRef());
			}
			else
			{
				Schema->SetBoolField(TEXT("additionalProperties"), false);
			}
		}

		CloseSchemaMap(TEXT("properties"));
		CloseSchemaMap(TEXT("patternProperties"));
		CloseSchemaMap(TEXT("$defs"));
		CloseSchemaMap(TEXT("dependentSchemas"));
		CloseObjectField(TEXT("items"));
		CloseObjectField(TEXT("contains"));
		CloseObjectField(TEXT("not"));
		CloseObjectField(TEXT("if"));
		CloseObjectField(TEXT("then"));
		CloseObjectField(TEXT("else"));
		CloseSchemaArray(TEXT("allOf"));
		CloseSchemaArray(TEXT("anyOf"));
		CloseSchemaArray(TEXT("oneOf"));
	}

	static FSchema BuildSchema(const FToolSpec& Spec)
	{
		TMap<FString, FSchema> Properties;
		TArray<FString> Required;
		if (Spec.Operation == EOperation::ReceiptGet)
		{
			Properties.Add(TEXT("receipt_id"), FSololmcpSchemaBuilder::String(TEXT("Receipt identifier issued by this editor session."), {}, 1, 128));
			Required.Add(TEXT("receipt_id"));
			FSchema Schema = FSololmcpSchemaBuilder::Object(Properties, Required, Spec.Description, false);
			CloseSchemaRecursively(Schema);
			return Schema;
		}
		Properties.Add(TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Target Static Mesh asset path under /Game/."), {}, 1, 512, TEXT("^/Game/")));
		Properties.Add(TEXT("lod_index"), FSololmcpSchemaBuilder::Integer(TEXT("Editable source LOD index."), 0, 63));
		Properties.Add(TEXT("expected_hash"), FSololmcpSchemaBuilder::String(TEXT("Optional optimistic-concurrency geometry hash."), {}, 8, 64));
		Required.Add(TEXT("asset_path"));
		if (Spec.bMutation) Properties.Add(TEXT("save"), FSololmcpSchemaBuilder::WithDefaultBoolean(FSololmcpSchemaBuilder::Boolean(TEXT("Save the package after verified mutation.")), true));
		const FSchema IdArray = FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Integer(TEXT("Element ID."), 0), TEXT("Validated element IDs."), 0, 250000, true);
		auto AddTriangles = [&]() { Properties.Add(TEXT("triangle_ids"), IdArray); Required.Add(TEXT("triangle_ids")); };
		auto AddVertices = [&]() { Properties.Add(TEXT("vertex_ids"), IdArray); };
		switch (Spec.Operation)
		{
		case EOperation::SelectVerticesBox:
		case EOperation::SelectTrianglesBox:
			Properties.Add(TEXT("min"), VectorSchema(TEXT("Minimum local-space box corner.")));
			Properties.Add(TEXT("max"), VectorSchema(TEXT("Maximum local-space box corner.")));
			Required.Append({TEXT("min"), TEXT("max")});
			break;
		case EOperation::SelectBoundaryEdges:
			Properties.Add(TEXT("triangle_ids"), IdArray);
			break;
		case EOperation::SelectGrow:
		case EOperation::SelectShrink:
			AddTriangles();
			Properties.Add(TEXT("iterations"), FSololmcpSchemaBuilder::Integer(TEXT("Topology rings."), 1, 64));
			break;
		case EOperation::SelectInvert:
			AddTriangles();
			break;
		case EOperation::SelectComponent:
			Properties.Add(TEXT("seed_triangle_id"), FSololmcpSchemaBuilder::Integer(TEXT("Connected-component seed triangle."), 0));
			Required.Add(TEXT("seed_triangle_id"));
			break;
		case EOperation::EdgeSplit:
		case EOperation::EdgeFlip:
			Properties.Add(TEXT("edge_id"), FSololmcpSchemaBuilder::Integer(TEXT("Dynamic mesh edge ID."), 0));
			Required.Add(TEXT("edge_id"));
			if (Spec.Operation == EOperation::EdgeSplit) Properties.Add(TEXT("t"), FSololmcpSchemaBuilder::Number(TEXT("Split interpolation."), 0.0, 1.0, true, true));
			break;
		case EOperation::EdgeCollapse:
		case EOperation::VerticesWeld:
			Properties.Add(TEXT("keep_vertex_id"), FSololmcpSchemaBuilder::Integer(TEXT("Vertex that remains."), 0));
			Properties.Add(Spec.Operation == EOperation::EdgeCollapse ? TEXT("remove_vertex_id") : TEXT("discard_vertex_id"), FSololmcpSchemaBuilder::Integer(TEXT("Vertex that is removed."), 0));
			Properties.Add(TEXT("t"), FSololmcpSchemaBuilder::Number(TEXT("Position interpolation."), 0.0, 1.0));
			Required.Append({TEXT("keep_vertex_id"), Spec.Operation == EOperation::EdgeCollapse ? TEXT("remove_vertex_id") : TEXT("discard_vertex_id")});
			break;
		case EOperation::TrianglePoke:
			Properties.Add(TEXT("triangle_id"), FSololmcpSchemaBuilder::Integer(TEXT("Triangle to poke."), 0));
			Properties.Add(TEXT("barycentric"), VectorSchema(TEXT("Barycentric x/y/z coordinates summing to 1.")));
			Required.Add(TEXT("triangle_id"));
			break;
		case EOperation::TrianglesDelete:
			AddTriangles();
			Properties.Add(TEXT("allow_empty"), FSololmcpSchemaBuilder::WithDefaultBoolean(FSololmcpSchemaBuilder::Boolean(TEXT("Explicit empty-mesh acknowledgement; commit still fails closed on empty delivery.")), false));
			break;
		case EOperation::OrientationReverse:
			Properties.Add(TEXT("triangle_ids"), IdArray);
			break;
		case EOperation::HoleFill:
			Properties.Add(TEXT("loop_index"), FSololmcpSchemaBuilder::Integer(TEXT("Boundary loop index from inspect/readback."), 0));
			Required.Add(TEXT("loop_index"));
			break;
		case EOperation::FacesExtrude:
			AddTriangles();
			Properties.Add(TEXT("distance"), FSololmcpSchemaBuilder::Number(TEXT("Signed extrusion distance.")));
			Required.Add(TEXT("distance"));
			break;
		case EOperation::FacesInset:
			AddTriangles();
			Properties.Add(TEXT("amount"), FSololmcpSchemaBuilder::Number(TEXT("Positive local-space inset distance."), 0.0, TOptional<double>(), true, false));
			Required.Add(TEXT("amount"));
			break;
		case EOperation::PlaneTrim:
			Properties.Add(TEXT("plane_origin"), VectorSchema(TEXT("Plane origin in local space.")));
			Properties.Add(TEXT("plane_normal"), VectorSchema(TEXT("Plane normal in local space.")));
			Properties.Add(TEXT("keep_positive"), FSololmcpSchemaBuilder::WithDefaultBoolean(FSololmcpSchemaBuilder::Boolean(TEXT("Keep the positive half-space.")), true));
			Properties.Add(TEXT("allow_intersecting_triangles"), FSololmcpSchemaBuilder::WithDefaultBoolean(FSololmcpSchemaBuilder::Boolean(TEXT("Permit centroid classification for intersecting triangles.")), false));
			Required.Append({TEXT("plane_origin"), TEXT("plane_normal")});
			break;
		case EOperation::SculptSmooth:
			AddVertices();
			Properties.Add(TEXT("strength"), FSololmcpSchemaBuilder::Number(TEXT("Laplacian alpha."), 0.0, 1.0, true, false));
			Properties.Add(TEXT("iterations"), FSololmcpSchemaBuilder::Integer(TEXT("Smoothing iterations."), 1, 100));
			break;
		case EOperation::SculptInflate:
			AddVertices();
			Properties.Add(TEXT("distance"), FSololmcpSchemaBuilder::Number(TEXT("Signed normal offset.")));
			Required.Add(TEXT("distance"));
			break;
		case EOperation::SculptFlatten:
			AddVertices();
			Properties.Add(TEXT("plane_origin"), VectorSchema(TEXT("Flatten plane origin.")));
			Properties.Add(TEXT("plane_normal"), VectorSchema(TEXT("Flatten plane normal.")));
			Properties.Add(TEXT("strength"), FSololmcpSchemaBuilder::Number(TEXT("Projection blend."), 0.0, 1.0, true, false));
			break;
		case EOperation::DeformTranslate:
			AddVertices(); Properties.Add(TEXT("delta"), VectorSchema(TEXT("Translation delta."))); Required.Add(TEXT("delta")); break;
		case EOperation::DeformRotate:
			AddVertices(); Properties.Add(TEXT("pivot"), VectorSchema(TEXT("Rotation pivot."))); Properties.Add(TEXT("axis"), VectorSchema(TEXT("Rotation axis."))); Properties.Add(TEXT("degrees"), FSololmcpSchemaBuilder::Number(TEXT("Rotation in degrees."))); Required.Add(TEXT("degrees")); break;
		case EOperation::DeformScale:
			AddVertices(); Properties.Add(TEXT("pivot"), VectorSchema(TEXT("Scale pivot."))); Properties.Add(TEXT("scale"), VectorSchema(TEXT("Per-axis scale."))); Required.Add(TEXT("scale")); break;
		case EOperation::DeformTwist:
		case EOperation::DeformBend:
		case EOperation::DeformTaper:
			AddVertices(); Properties.Add(TEXT("pivot"), VectorSchema(TEXT("Deformation pivot."))); Properties.Add(TEXT("axis"), VectorSchema(TEXT("Deformation axis."))); Properties.Add(TEXT("strength"), FSololmcpSchemaBuilder::Number(TEXT("Axis-relative deformation strength."))); Required.Add(TEXT("strength")); break;
		case EOperation::VertexColorsFill:
			AddVertices(); Properties.Add(TEXT("color"), VectorSchema(TEXT("Linear RGB in [0,1]."))); Required.Add(TEXT("color")); break;
		case EOperation::MaterialIdsSet:
			AddTriangles(); Properties.Add(TEXT("material_id"), FSololmcpSchemaBuilder::Integer(TEXT("Material slot index."), 0)); Required.Add(TEXT("material_id")); break;
		case EOperation::PolygroupsSet:
			AddTriangles(); Properties.Add(TEXT("polygroup_id"), FSololmcpSchemaBuilder::Integer(TEXT("Triangle group ID."), 0)); Required.Add(TEXT("polygroup_id")); break;
		case EOperation::UvPlanarProject:
			AddVertices(); Properties.Add(TEXT("axes"), FSololmcpSchemaBuilder::String(TEXT("Projection axes."), {TEXT("xy"), TEXT("xz"), TEXT("yz")})); Properties.Add(TEXT("uv_scale"), FSololmcpSchemaBuilder::Number(TEXT("UV units per local unit."))); break;
		case EOperation::BakePositionColor:
		case EOperation::BakeNormalColor:
			AddVertices(); break;
		case EOperation::SnapshotRestore:
			Properties.Add(TEXT("snapshot_id"), FSololmcpSchemaBuilder::String(TEXT("Snapshot identifier from snapshot_create."), {}, 1, 128)); Required.Add(TEXT("snapshot_id")); break;
		default: break;
		}
		FSchema Schema = FSololmcpSchemaBuilder::Object(Properties, Required, Spec.Description, false);
		CloseSchemaRecursively(Schema);
		return Schema;
	}
}

	void RegisterModelingCompletionTools(FSololmcpToolRegistry& Registry)
	{
		for (const ModelingCompletion::FToolSpec& Spec : ModelingCompletion::ToolSpecs)
		{
			if (Registry.HasRegisteredTool(Spec.Name))
			{
				continue;
			}
			FSololmcpToolDefinition Definition;
			Definition.Name = Spec.Name;
			Definition.Description = Spec.Description;
			Definition.InputSchema = ModelingCompletion::BuildSchema(Spec);
			Definition.bUsesExternalPython = false;
			Definition.CacheTtlSeconds = 0;
			Definition.Execute = [Spec](
				const FSololmcpToolExecutionContext& Context,
				const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& Out,
				FString& OutSummary,
				FString& OutError)
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
				return ModelingCompletion::ExecuteOperation(Context, Spec, Args, Out, OutSummary, OutError);
#else
				Out->SetBoolField(TEXT("ok"), false);
				Out->SetStringField(TEXT("status"), TEXT("failed"));
				Out->SetStringField(TEXT("error_code"), TEXT("unsupported_engine_version"));
				Out->SetStringField(TEXT("reason_code"), TEXT("unsupported_engine_version"));
				Out->SetStringField(TEXT("message"), TEXT("P2 Modeling/DynamicMesh completion tools require Unreal Engine 5.8 or newer."));
				Out->SetBoolField(TEXT("mutation_applied"), false);
				OutError = TEXT("P2 Modeling/DynamicMesh completion tools require Unreal Engine 5.8 or newer.");
				return false;
#endif
			};
			Registry.Register(Definition);
		}
	}
}
