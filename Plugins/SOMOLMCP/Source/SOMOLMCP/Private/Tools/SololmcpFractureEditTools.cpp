// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "Dataflow/DataflowSelection.h"
#include "FractureEngineClustering.h"
#include "FractureEngineEdit.h"
#include "FractureEngineMaterials.h"
#include "FractureEngineSelection.h"
#include "FractureEngineUtility.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/TransformCollection.h"
#endif

namespace UE::SOMOLMCP
{
namespace FractureEdit
{
	static void Fail(TSharedRef<FJsonObject>& Out, FString& Error, const FString& Code,
		const FString& Message, const FString& Status = TEXT("failed"))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("status"), Status);
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("reason_code"), Code);
		Out->SetStringField(TEXT("message"), Message);
		Error = Message;
	}

	static TSharedRef<FJsonObject> IndexArraySchema(const FString& Description)
	{
		return FSololmcpSchemaBuilder::Array(
			FSololmcpSchemaBuilder::Integer(TEXT("Collection index."), 0, MAX_int32),
			Description, 1, 100000, true);
	}

	static TSharedRef<FJsonObject> BoneSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Selected transform indices."))},
		}, {TEXT("asset_path"), TEXT("bone_indices")}, FString(), false);
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	static UGeometryCollection* LoadCollection(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FString Path;
		if (!Args->TryGetStringField(TEXT("asset_path"), Path) || !Path.StartsWith(TEXT("/Game/")))
		{
			Fail(Out, Error, TEXT("invalid_asset_path"),
				TEXT("asset_path must identify a Geometry Collection under /Game/."));
			return nullptr;
		}

		UGeometryCollection* Asset = Cast<UGeometryCollection>(Context.Services.LoadAsset(Path, Error));
		if (!Asset || !Asset->GetGeometryCollection().IsValid())
		{
			Fail(Out, Error, TEXT("geometry_collection_not_found"),
				Error.IsEmpty() ? FString::Printf(TEXT("Geometry Collection '%s' could not be loaded."), *Path) : Error);
			return nullptr;
		}

		FGeometryCollection& Collection = *Asset->GetGeometryCollection();
		if (Collection.NumElements(FGeometryCollection::TransformGroup) <= 0)
		{
			Fail(Out, Error, TEXT("geometry_collection_empty"),
				TEXT("The target Geometry Collection has no transform hierarchy."));
			return nullptr;
		}
		return Asset;
	}

	static bool ParseIndices(const TSharedRef<FJsonObject>& Args, const TCHAR* Field, const int32 Count,
		TArray<int32>& OutIndices, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Args->TryGetArrayField(Field, Values) || !Values || Values->IsEmpty())
		{
			Fail(Out, Error, TEXT("selection_empty"), FString::Printf(TEXT("%s must not be empty."), Field));
			return false;
		}

		TSet<int32> Unique;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const int32 Index = Value.IsValid() ? static_cast<int32>(Value->AsNumber()) : INDEX_NONE;
			if (Index < 0 || Index >= Count)
			{
				Fail(Out, Error, TEXT("selection_index_out_of_range"),
					FString::Printf(TEXT("%s index %d is outside [0, %d)."), Field, Index, Count));
				return false;
			}
			Unique.Add(Index);
		}
		OutIndices = Unique.Array();
		OutIndices.Sort();
		return true;
	}

	static FDataflowTransformSelection MakeSelection(const FGeometryCollection& Collection,
		const TArray<int32>& Bones)
	{
		FDataflowTransformSelection Selection;
		Selection.InitializeFromCollection(Collection, false);
		for (const int32 Bone : Bones)
		{
			Selection.SetSelected(Bone);
		}
		return Selection;
	}

	static uint32 CollectionDigest(const FGeometryCollection& Collection)
	{
		uint32 Hash = 0;
		auto AddInt = [&Hash](const int32 Value) { Hash = HashCombineFast(Hash, GetTypeHash(Value)); };
		AddInt(Collection.NumElements(FGeometryCollection::TransformGroup));
		AddInt(Collection.NumElements(FGeometryCollection::GeometryGroup));
		AddInt(Collection.NumElements(FGeometryCollection::VerticesGroup));
		AddInt(Collection.NumElements(FGeometryCollection::FacesGroup));

		if (Collection.HasAttribute(FTransformCollection::ParentAttribute, FGeometryCollection::TransformGroup))
		{
			for (const int32 Value : Collection.GetAttribute<int32>(FTransformCollection::ParentAttribute,
				FGeometryCollection::TransformGroup)) AddInt(Value);
		}
		if (Collection.HasAttribute(FGeometryCollection::SimulationTypeAttribute, FGeometryCollection::TransformGroup))
		{
			for (const int32 Value : Collection.GetAttribute<int32>(FGeometryCollection::SimulationTypeAttribute,
				FGeometryCollection::TransformGroup)) AddInt(Value);
		}
		if (Collection.HasAttribute(FGeometryCollection::FaceVisibleAttribute, FGeometryCollection::FacesGroup))
		{
			const TManagedArray<bool>& Visibility = Collection.GetAttribute<bool>(
				FGeometryCollection::FaceVisibleAttribute, FGeometryCollection::FacesGroup);
			for (int32 Index = 0; Index < Visibility.Num(); ++Index) AddInt(Visibility[Index] ? 1 : 0);
		}
		if (Collection.HasAttribute(FGeometryCollection::MaterialIDAttribute, FGeometryCollection::FacesGroup))
		{
			for (const int32 Value : Collection.GetAttribute<int32>(FGeometryCollection::MaterialIDAttribute,
				FGeometryCollection::FacesGroup)) AddInt(Value);
		}
		if (Collection.HasAttribute(FGeometryCollection::VertexNormalAttribute, FGeometryCollection::VerticesGroup))
		{
			for (const FVector3f& Normal : Collection.GetAttribute<FVector3f>(FGeometryCollection::VertexNormalAttribute,
				FGeometryCollection::VerticesGroup))
			{
				Hash = HashCombineFast(Hash, GetTypeHash(Normal.X));
				Hash = HashCombineFast(Hash, GetTypeHash(Normal.Y));
				Hash = HashCombineFast(Hash, GetTypeHash(Normal.Z));
			}
		}
		return Hash;
	}

	static FString DigestString(const uint32 Digest)
	{
		return FString::Printf(TEXT("%08x"), Digest);
	}

	static void RestoreSnapshot(UGeometryCollection& Asset, TUniquePtr<FGeometryCollection>& Snapshot)
	{
		if (Snapshot.IsValid() && Asset.GetGeometryCollection().IsValid())
		{
			*Asset.GetGeometryCollection() = MoveTemp(*Snapshot);
			Asset.InvalidateCollection();
			Asset.RebuildRenderData();
			Asset.PropagateMarkDirtyToComponents();
		}
	}

	static bool SaveAndReadback(const FSololmcpToolExecutionContext& Context, UGeometryCollection& Asset,
		const uint32 ExpectedDigest, TUniquePtr<FGeometryCollection>& Snapshot,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Asset.InvalidateCollection();
		Asset.CreateSimulationData();
		Asset.RebuildRenderData();
		Asset.PropagateMarkDirtyToComponents();
		Asset.MarkPackageDirty();

		FString SaveError;
		if (!Context.Services.SaveAsset(Asset.GetPathName(), false, SaveError))
		{
			RestoreSnapshot(Asset, Snapshot);
			Fail(Out, Error, TEXT("fracture_edit_save_failed"),
				SaveError.IsEmpty() ? TEXT("Geometry Collection save failed; the in-memory snapshot was restored.") : SaveError);
			Out->SetBoolField(TEXT("rollback_applied"), true);
			return false;
		}

		FString ReadbackError;
		UGeometryCollection* Readback = Cast<UGeometryCollection>(Context.Services.LoadAsset(Asset.GetPathName(), ReadbackError));
		if (!Readback || !Readback->GetGeometryCollection().IsValid() ||
			CollectionDigest(*Readback->GetGeometryCollection()) != ExpectedDigest)
		{
			RestoreSnapshot(Asset, Snapshot);
			FString RollbackSaveError;
			Context.Services.SaveAsset(Asset.GetPathName(), false, RollbackSaveError);
			Fail(Out, Error, TEXT("fracture_edit_readback_mismatch"),
				ReadbackError.IsEmpty() ? TEXT("Saved Geometry Collection readback did not match the committed mutation; rollback was applied.") : ReadbackError);
			Out->SetBoolField(TEXT("rollback_applied"), true);
			return false;
		}
		return true;
	}

	static void WriteReceipt(UGeometryCollection& Asset, const FString& Operation,
		const uint32 BeforeDigest, const uint32 AfterDigest, TSharedRef<FJsonObject>& Out)
	{
		const FGeometryCollection& Collection = *Asset.GetGeometryCollection();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("fracture_edit_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
		Out->SetStringField(TEXT("asset_path"), Asset.GetPathName());
		Out->SetStringField(TEXT("operation"), Operation);
		Out->SetStringField(TEXT("pre_digest"), DigestString(BeforeDigest));
		Out->SetStringField(TEXT("post_digest"), DigestString(AfterDigest));
		Out->SetBoolField(TEXT("mutation_applied"), BeforeDigest != AfterDigest);
		Out->SetBoolField(TEXT("saved"), true);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetBoolField(TEXT("rollback_applied"), false);
		Out->SetNumberField(TEXT("transform_count"), Collection.NumElements(FGeometryCollection::TransformGroup));
		Out->SetNumberField(TEXT("geometry_count"), Collection.NumElements(FGeometryCollection::GeometryGroup));
		Out->SetNumberField(TEXT("face_count"), Collection.NumElements(FGeometryCollection::FacesGroup));
	}

	static bool CommitMutation(const FSololmcpToolExecutionContext& Context, UGeometryCollection& Asset,
		const FString& Operation, const uint32 BeforeDigest, TUniquePtr<FGeometryCollection>& Snapshot,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		const uint32 AfterDigest = CollectionDigest(*Asset.GetGeometryCollection());
		if (!SaveAndReadback(Context, Asset, AfterDigest, Snapshot, Out, Error)) return false;
		WriteReceipt(Asset, Operation, BeforeDigest, AfterDigest, Out);
		return true;
	}

	static int32 CountVisibleFaces(const FGeometryCollection& Collection, const bool bVisible)
	{
		if (!Collection.HasAttribute(FGeometryCollection::FaceVisibleAttribute, FGeometryCollection::FacesGroup)) return 0;
		int32 Count = 0;
		const TManagedArray<bool>& Visibility = Collection.GetAttribute<bool>(
			FGeometryCollection::FaceVisibleAttribute, FGeometryCollection::FacesGroup);
		for (int32 Index = 0; Index < Visibility.Num(); ++Index) Count += Visibility[Index] == bVisible ? 1 : 0;
		return Count;
	}

	static int32 CountMaterialFaces(const FGeometryCollection& Collection, const int32 MaterialId)
	{
		if (!Collection.HasAttribute(FGeometryCollection::MaterialIDAttribute, FGeometryCollection::FacesGroup)) return 0;
		int32 Count = 0;
		for (const int32 Value : Collection.GetAttribute<int32>(FGeometryCollection::MaterialIDAttribute,
			FGeometryCollection::FacesGroup)) Count += Value == MaterialId ? 1 : 0;
		return Count;
	}
#endif
}

void RegisterFractureEditTools(FSololmcpToolRegistry& Registry)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	using namespace FractureEdit;

	Registry.Register({TEXT("fracture_cluster_merge_selected_apply"),
		TEXT("Merge selected Chaos clusters with native hierarchy readback."), BoneSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			if (!FFractureEngineSelection::IsBoneSelectionValid(Collection, Bones)) { Fail(Out, Error, TEXT("invalid_bone_selection"), TEXT("The cluster selection is invalid for this collection.")); return false; }
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureMergeClusters", "SOMOLMCP Merge Selected Clusters")); Asset->Modify();
			if (!FFractureEngineClustering::MergeSelectedClusters(Collection, Bones)) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, TEXT("cluster_merge_no_change"), TEXT("The selected transforms could not be merged into one cluster.")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("cluster_merge_selected"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetNumberField(TEXT("result_cluster_index"), Bones.IsEmpty() ? INDEX_NONE : Bones[0]);
			Summary = TEXT("Merged selected Chaos clusters and verified saved hierarchy readback."); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_cluster_magnet_apply"), TEXT("Grow selected clusters through connected neighbors."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Selected cluster transform indices."))},
			{TEXT("iterations"), FSololmcpSchemaBuilder::Integer(TEXT("Connected-neighbor growth iterations."), 1, 32)},
		}, {TEXT("asset_path"), TEXT("bone_indices"), TEXT("iterations")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureClusterMagnet", "SOMOLMCP Cluster Magnet")); Asset->Modify();
			if (!FFractureEngineClustering::ClusterMagnet(Collection, Bones, static_cast<int32>(Args->GetNumberField(TEXT("iterations"))))) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, TEXT("cluster_magnet_no_change"), TEXT("Cluster magnet found no eligible connected neighbors.")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("cluster_magnet"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetNumberField(TEXT("result_selection_count"), Bones.Num()); Summary = TEXT("Expanded selected clusters through connected neighbors."); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_bones_merge_apply"), TEXT("Merge selected Geometry Collection bones using the native fracture edit API."), BoneSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			if (Bones.Num() < 2) { Fail(Out, Error, TEXT("merge_requires_multiple_bones"), TEXT("At least two transforms are required for merge.")); return false; }
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureMergeBones", "SOMOLMCP Merge Geometry Collection Bones")); Asset->Modify();
			FFractureEngineEdit::Merge(Collection, Bones);
			if (CollectionDigest(Collection) == Before) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, TEXT("bone_merge_no_change"), TEXT("Native bone merge produced no collection change.")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("merge_bones"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Summary = TEXT("Merged selected Geometry Collection bones and verified saved readback."); return true;
		}, nullptr, 3});

	Registry.Register({TEXT("fracture_branch_delete_apply"), TEXT("Delete non-root hierarchy branches with explicit destructive confirmation."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Branch roots to delete."))},
			{TEXT("confirm_delete"), FSololmcpSchemaBuilder::Boolean(TEXT("Must be true; deletion is fail-closed."))},
			{TEXT("expected_transform_count"), FSololmcpSchemaBuilder::Integer(TEXT("Exact pre-mutation transform count."), 1, MAX_int32)},
			{TEXT("max_removed_transforms"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum transforms this operation may remove."), 1, 100000)},
		}, {TEXT("asset_path"), TEXT("bone_indices"), TEXT("confirm_delete"), TEXT("expected_transform_count"), TEXT("max_removed_transforms")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); const int32 BeforeCount = Collection.NumElements(FGeometryCollection::TransformGroup); TArray<int32> Bones;
			if (!Args->GetBoolField(TEXT("confirm_delete"))) { Fail(Out, Error, TEXT("delete_confirmation_required"), TEXT("confirm_delete=true is required for branch deletion."), TEXT("blocked")); return false; }
			if (static_cast<int32>(Args->GetNumberField(TEXT("expected_transform_count"))) != BeforeCount) { Fail(Out, Error, TEXT("delete_revision_mismatch"), TEXT("expected_transform_count does not match current readback; refresh before deleting."), TEXT("blocked")); return false; }
			if (!ParseIndices(Args, TEXT("bone_indices"), BeforeCount, Bones, Out, Error)) return false;
			const TManagedArray<int32>& Parents = Collection.GetAttribute<int32>(FTransformCollection::ParentAttribute, FGeometryCollection::TransformGroup);
			for (const int32 Bone : Bones) if (Parents[Bone] == INDEX_NONE) { Fail(Out, Error, TEXT("root_delete_forbidden"), TEXT("Root transform deletion is forbidden by this tool."), TEXT("blocked")); return false; }
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureDeleteBranch", "SOMOLMCP Delete Geometry Collection Branch")); Asset->Modify();
			FFractureEngineEdit::DeleteBranch(Collection, Bones);
			const int32 Removed = BeforeCount - Collection.NumElements(FGeometryCollection::TransformGroup);
			const int32 MaxRemoved = static_cast<int32>(Args->GetNumberField(TEXT("max_removed_transforms")));
			if (Removed <= 0 || Removed > MaxRemoved) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, Removed <= 0 ? TEXT("branch_delete_no_change") : TEXT("branch_delete_limit_exceeded"), Removed <= 0 ? TEXT("No hierarchy branch was deleted.") : FString::Printf(TEXT("Deletion would remove %d transforms, exceeding max_removed_transforms=%d."), Removed, MaxRemoved), TEXT("blocked")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("delete_branch"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetNumberField(TEXT("removed_transform_count"), Removed); Summary = FString::Printf(TEXT("Deleted %d non-root hierarchy transforms."), Removed); return true;
		}, nullptr, 3});

	Registry.Register({TEXT("fracture_visibility_set_transforms"), TEXT("Set face visibility from Geometry Collection transform selection."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Transform indices whose faces will be updated."))},
			{TEXT("visible"), FSololmcpSchemaBuilder::Boolean(TEXT("Requested visibility."))},
		}, {TEXT("asset_path"), TEXT("bone_indices"), TEXT("visible")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			const bool bVisible = Args->GetBoolField(TEXT("visible")); const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureTransformVisibility", "SOMOLMCP Set Fracture Transform Visibility")); Asset->Modify();
			FFractureEngineEdit::SetVisibilityInCollectionFromTransformSelection(Collection, Bones, bVisible);
			const int32 Matching = CountVisibleFaces(Collection, bVisible);
			if (!CommitMutation(Context, *Asset, TEXT("set_transform_visibility"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetBoolField(TEXT("visible"), bVisible); Out->SetNumberField(TEXT("faces_with_requested_visibility"), Matching); Summary = FString::Printf(TEXT("Set transform-selected faces visible=%s."), bVisible ? TEXT("true") : TEXT("false")); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_visibility_set_faces"), TEXT("Set visibility for explicit Geometry Collection face indices."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("face_indices"), IndexArraySchema(TEXT("Face indices to update."))},
			{TEXT("visible"), FSololmcpSchemaBuilder::Boolean(TEXT("Requested visibility."))},
		}, {TEXT("asset_path"), TEXT("face_indices"), TEXT("visible")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Faces;
			if (!ParseIndices(Args, TEXT("face_indices"), Collection.NumElements(FGeometryCollection::FacesGroup), Faces, Out, Error)) return false;
			const bool bVisible = Args->GetBoolField(TEXT("visible")); const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureFaceVisibility", "SOMOLMCP Set Fracture Face Visibility")); Asset->Modify();
			FFractureEngineEdit::SetVisibilityInCollectionFromFaceSelection(Collection, Faces, bVisible);
			const TManagedArray<bool>& Visibility = Collection.GetAttribute<bool>(FGeometryCollection::FaceVisibleAttribute, FGeometryCollection::FacesGroup);
			for (const int32 Face : Faces) if (Visibility[Face] != bVisible) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, TEXT("face_visibility_readback_failed"), TEXT("At least one selected face did not receive the requested visibility.")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("set_face_visibility"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetNumberField(TEXT("updated_face_count"), Faces.Num()); Out->SetBoolField(TEXT("visible"), bVisible); Summary = FString::Printf(TEXT("Set %d faces visible=%s."), Faces.Num(), bVisible ? TEXT("true") : TEXT("false")); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_material_set"), TEXT("Assign a material ID to internal, external, or all selected-bone faces."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Selected transform indices."))},
			{TEXT("target_faces"), FSololmcpSchemaBuilder::String(TEXT("Face class to update."), {TEXT("internal"), TEXT("external"), TEXT("all")})},
			{TEXT("material_id"), FSololmcpSchemaBuilder::Integer(TEXT("Material slot index."), 0, 4095)},
		}, {TEXT("asset_path"), TEXT("bone_indices"), TEXT("target_faces"), TEXT("material_id")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			const FString Target = Args->GetStringField(TEXT("target_faces")); const int32 MaterialId = static_cast<int32>(Args->GetNumberField(TEXT("material_id")));
			const FFractureEngineMaterials::ETargetFaces TargetFaces = Target == TEXT("internal") ? FFractureEngineMaterials::ETargetFaces::InternalFaces : Target == TEXT("external") ? FFractureEngineMaterials::ETargetFaces::ExternalFaces : FFractureEngineMaterials::ETargetFaces::AllFaces;
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureSetMaterial", "SOMOLMCP Set Fracture Material")); Asset->Modify();
			FFractureEngineMaterials::SetMaterial(Collection, Bones, TargetFaces, MaterialId);
			const int32 Matching = CountMaterialFaces(Collection, MaterialId);
			if (Matching <= 0) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, TEXT("material_assignment_no_faces"), TEXT("No selected faces accepted the requested material ID.")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("set_material"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetNumberField(TEXT("material_id"), MaterialId); Out->SetNumberField(TEXT("faces_with_material"), Matching); Summary = FString::Printf(TEXT("Assigned material ID %d to selected %s faces."), MaterialId, *Target); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_normals_recompute"), TEXT("Recompute Geometry Collection normals or tangents for selected transforms."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Selected transform indices."))},
			{TEXT("only_tangents"), FSololmcpSchemaBuilder::Boolean(TEXT("Recompute tangents without replacing normals."))},
			{TEXT("recompute_sharp_edges"), FSololmcpSchemaBuilder::Boolean(TEXT("Split/recompute sharp edges."))},
			{TEXT("sharp_edge_angle_degrees"), FSololmcpSchemaBuilder::Number(TEXT("Sharp edge threshold in degrees."), 0.0, 180.0)},
			{TEXT("only_internal_surfaces"), FSololmcpSchemaBuilder::Boolean(TEXT("Limit recompute to internal fracture surfaces."))},
		}, {TEXT("asset_path"), TEXT("bone_indices"), TEXT("only_tangents"), TEXT("recompute_sharp_edges"), TEXT("sharp_edge_angle_degrees"), TEXT("only_internal_surfaces")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureRecomputeNormals", "SOMOLMCP Recompute Fracture Normals")); Asset->Modify();
			FFractureEngineUtility::RecomputeNormalsInGeometryCollection(Collection, MakeSelection(Collection, Bones), Args->GetBoolField(TEXT("only_tangents")), Args->GetBoolField(TEXT("recompute_sharp_edges")), static_cast<float>(Args->GetNumberField(TEXT("sharp_edge_angle_degrees"))), Args->GetBoolField(TEXT("only_internal_surfaces")));
			const TManagedArray<FVector3f>& Normals = Collection.GetAttribute<FVector3f>(FGeometryCollection::VertexNormalAttribute, FGeometryCollection::VerticesGroup);
			for (const FVector3f& Normal : Normals) if (Normal.ContainsNaN()) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, TEXT("normal_readback_invalid"), TEXT("Normal recompute produced a non-finite vector.")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("recompute_normals"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetNumberField(TEXT("normal_count"), Normals.Num()); Summary = FString::Printf(TEXT("Recomputed and verified %d Geometry Collection normals."), Normals.Num()); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_tiny_geometry_fix"), TEXT("Merge tiny Geometry Collection pieces using native proximity-aware repair."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Transforms eligible for tiny-geometry repair."))},
			{TEXT("merge_type"), FSololmcpSchemaBuilder::String(TEXT("Repair mode."), {TEXT("geometry"), TEXT("clusters")})},
			{TEXT("min_volume_cube_root"), FSololmcpSchemaBuilder::Number(TEXT("Absolute size threshold in collection units."), 0.0, 1e9)},
			{TEXT("relative_volume"), FSololmcpSchemaBuilder::Number(TEXT("Relative volume threshold."), 0.0, 1.0)},
			{TEXT("neighbor_method"), FSololmcpSchemaBuilder::String(TEXT("Neighbor choice."), {TEXT("largest"), TEXT("nearest"), TEXT("contact")})},
			{TEXT("only_connected"), FSololmcpSchemaBuilder::Boolean(TEXT("Merge only through proximity connections."))},
			{TEXT("only_same_parent"), FSololmcpSchemaBuilder::Boolean(TEXT("Restrict merge to siblings."))},
		}, {TEXT("asset_path"), TEXT("bone_indices"), TEXT("merge_type"), TEXT("min_volume_cube_root"), TEXT("relative_volume"), TEXT("neighbor_method"), TEXT("only_connected"), TEXT("only_same_parent")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			const FString Neighbor = Args->GetStringField(TEXT("neighbor_method"));
			const EFixTinyGeoNeighborSelectionMethod NeighborMethod = Neighbor == TEXT("nearest") ? EFixTinyGeoNeighborSelectionMethod::NearestCenter : Neighbor == TEXT("contact") ? EFixTinyGeoNeighborSelectionMethod::LargestContactArea : EFixTinyGeoNeighborSelectionMethod::LargestNeighbor;
			const EFixTinyGeoMergeType MergeType = Args->GetStringField(TEXT("merge_type")) == TEXT("clusters") ? EFixTinyGeoMergeType::MergeClusters : EFixTinyGeoMergeType::MergeGeometry;
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureFixTinyGeometry", "SOMOLMCP Fix Tiny Geometry")); Asset->Modify();
			FFractureEngineUtility::FixTinyGeo(Collection, MakeSelection(Collection, Bones), MergeType, false,
				EFixTinyGeoGeometrySelectionMethod::VolumeCubeRoot, static_cast<float>(Args->GetNumberField(TEXT("min_volume_cube_root"))),
				static_cast<float>(Args->GetNumberField(TEXT("relative_volume"))), EFixTinyGeoUseBoneSelection::OnlyMergeSelected,
				MergeType == EFixTinyGeoMergeType::MergeClusters, NeighborMethod, Args->GetBoolField(TEXT("only_connected")),
				Args->GetBoolField(TEXT("only_same_parent")), true);
			if (!CommitMutation(Context, *Asset, TEXT("fix_tiny_geometry"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Summary = Out->GetBoolField(TEXT("mutation_applied")) ? TEXT("Merged tiny Geometry Collection pieces.") : TEXT("No tiny pieces matched the repair thresholds; verified idempotent readback."); return true;
		}, nullptr, 3});

	Registry.Register({TEXT("fracture_collision_samples_resample"), TEXT("Resample Geometry Collection collision particles for selected transforms."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), IndexArraySchema(TEXT("Selected transform indices."))},
			{TEXT("sample_spacing"), FSololmcpSchemaBuilder::Number(TEXT("Collision sample spacing in collection units."), 0.001, 100000.0)},
		}, {TEXT("asset_path"), TEXT("bone_indices"), TEXT("sample_spacing")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseIndices(Args, TEXT("bone_indices"), Collection.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			const uint32 Before = CollectionDigest(Collection); TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureResampleCollision", "SOMOLMCP Resample Fracture Collision")); Asset->Modify();
			const int32 AddedSamples = FFractureEngineUtility::ResampleGeometryCollection(Collection, MakeSelection(Collection, Bones), static_cast<float>(Args->GetNumberField(TEXT("sample_spacing"))));
			if (AddedSamples <= 0) { RestoreSnapshot(*Asset, Snapshot); Transaction.Cancel(); Fail(Out, Error, TEXT("collision_resample_no_samples"), TEXT("Collision resampling generated no new samples.")); return false; }
			if (!CommitMutation(Context, *Asset, TEXT("resample_collision_samples"), Before, Snapshot, Out, Error)) { Transaction.Cancel(); return false; }
			Out->SetNumberField(TEXT("samples_added"), AddedSamples); Summary = FString::Printf(TEXT("Added %d collision samples and verified saved readback."), AddedSamples); return true;
		}, nullptr, 2});
#else
	(void)Registry;
#endif
}
}
