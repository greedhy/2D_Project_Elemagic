// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "Dataflow/DataflowSelection.h"
#include "FractureEngineClustering.h"
#include "FractureEngineSelection.h"
#include "FractureEngineUtility.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#endif

namespace UE::SOMOLMCP
{
namespace FractureHierarchy
{
	static void Fail(TSharedRef<FJsonObject>& Out, FString& Error, const FString& Code, const FString& Message)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("status"), TEXT("failed"));
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("reason_code"), Code);
		Out->SetStringField(TEXT("message"), Message);
		Error = Message;
	}

	static TSharedRef<FJsonObject> AssetSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
		}, {TEXT("asset_path")}, FString(), false);
	}

	static TSharedRef<FJsonObject> BoneSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("bone_indices"), FSololmcpSchemaBuilder::Array(
				FSololmcpSchemaBuilder::Integer(TEXT("Transform index."), 0, MAX_int32), TEXT("Selected transform indices."))},
		}, {TEXT("asset_path"), TEXT("bone_indices")}, FString(), false);
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	static UGeometryCollection* LoadCollection(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FString Path;
		if (!Args->TryGetStringField(TEXT("asset_path"), Path) || !Path.StartsWith(TEXT("/Game/")))
		{
			Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("asset_path must be under /Game/."));
			return nullptr;
		}
		UGeometryCollection* Asset = Cast<UGeometryCollection>(Context.Services.LoadAsset(Path, Error));
		if (!Asset || !Asset->GetGeometryCollection().IsValid())
		{
			Fail(Out, Error, TEXT("geometry_collection_not_found"),
				Error.IsEmpty() ? TEXT("Geometry Collection could not be loaded.") : Error);
			return nullptr;
		}
		return Asset;
	}

	static bool ParseBones(const TSharedRef<FJsonObject>& Args, const int32 Count, TArray<int32>& OutBones,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Args->TryGetArrayField(TEXT("bone_indices"), Values) || !Values || Values->IsEmpty())
		{
			Fail(Out, Error, TEXT("bone_selection_empty"), TEXT("bone_indices must not be empty."));
			return false;
		}
		TSet<int32> Unique;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const int32 Index = Value.IsValid() ? static_cast<int32>(Value->AsNumber()) : INDEX_NONE;
			if (Index < 0 || Index >= Count)
			{
				Fail(Out, Error, TEXT("bone_index_out_of_range"),
					FString::Printf(TEXT("Bone index %d is outside [0, %d)."), Index, Count));
				return false;
			}
			Unique.Add(Index);
		}
		OutBones = Unique.Array();
		OutBones.Sort();
		return true;
	}

	static void WriteSelection(const TArray<int32>& Selection, TSharedRef<FJsonObject>& Out)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const int32 Index : Selection) Values.Add(MakeShared<FJsonValueNumber>(Index));
		Out->SetArrayField(TEXT("bone_indices"), Values);
		Out->SetNumberField(TEXT("selected_count"), Selection.Num());
	}

	static bool Save(const FSololmcpToolExecutionContext& Context, UGeometryCollection& Asset,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Asset.InvalidateCollection();
		Asset.CreateSimulationData();
		Asset.RebuildRenderData();
		Asset.PropagateMarkDirtyToComponents();
		Asset.MarkPackageDirty();
		FString SaveError;
		if (!Context.Services.SaveAsset(Asset.GetPathName(), false, SaveError) || !Context.Services.AssetExists(Asset.GetPathName()))
		{
			Fail(Out, Error, TEXT("fracture_hierarchy_save_failed"), SaveError.IsEmpty() ? TEXT("Save/readback failed.") : SaveError);
			return false;
		}
		return true;
	}

	static void WriteReceipt(UGeometryCollection& Asset, const FString& Operation, const bool bChanged,
		TSharedRef<FJsonObject>& Out)
	{
		const TSharedPtr<FGeometryCollection> Collection = Asset.GetGeometryCollection();
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("fracture_hierarchy_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
		Out->SetStringField(TEXT("asset_path"), Asset.GetPathName());
		Out->SetStringField(TEXT("operation"), Operation);
		Out->SetBoolField(TEXT("mutation_applied"), bChanged);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetNumberField(TEXT("transform_count"), Collection->NumElements(FGeometryCollection::TransformGroup));
		Out->SetNumberField(TEXT("geometry_count"), Collection->NumElements(FGeometryCollection::GeometryGroup));
	}
#endif
}

void RegisterFractureHierarchyTools(FSololmcpToolRegistry& Registry)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	using namespace FractureHierarchy;

	auto RegisterSelection = [&Registry](const TCHAR* Name, const TCHAR* Description,
		TFunction<void(FGeometryCollection&, TArray<int32>&)> Select)
	{
		Registry.Register({Name, Description, AssetSchema(),
			[Select](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
			{
				UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
				TArray<int32> Result; Select(*Asset->GetGeometryCollection(), Result); WriteSelection(Result, Out);
				Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("succeeded"));
				Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
				Summary = FString::Printf(TEXT("Selected %d transforms."), Result.Num()); return true;
			}, nullptr, 15});
	};

	RegisterSelection(TEXT("fracture_selection_roots"), TEXT("Return root transforms."),
		[](FGeometryCollection& C, TArray<int32>& R) { FFractureEngineSelection::GetRootBones(C, R); });
	RegisterSelection(TEXT("fracture_selection_leaves"), TEXT("Return leaf transforms."),
		[](FGeometryCollection& C, TArray<int32>& R) { FFractureEngineSelection::SelectLeaf(C, R); });
	RegisterSelection(TEXT("fracture_selection_clusters"), TEXT("Return cluster transforms."),
		[](FGeometryCollection& C, TArray<int32>& R) { FFractureEngineSelection::SelectCluster(C, R); });

	auto RegisterRangeSelection = [&Registry](const TCHAR* Name, const TCHAR* Description, const bool bVolume)
	{
		Registry.Register({Name, Description,
			FSololmcpSchemaBuilder::Object({
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
				{TEXT("min_value"), FSololmcpSchemaBuilder::Number(TEXT("Minimum inclusive value."), 0.0, 1e18)},
				{TEXT("max_value"), FSololmcpSchemaBuilder::Number(TEXT("Maximum inclusive value."), 0.0, 1e18)},
			}, {TEXT("asset_path"), TEXT("min_value"), TEXT("max_value")}, FString(), false),
			[bVolume](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
				TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
			{
				UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
				FGeometryCollection& C = *Asset->GetGeometryCollection(); TArray<int32> Result;
				FFractureEngineSelection::SelectLeaf(C, Result);
				const float Min = static_cast<float>(Args->GetNumberField(TEXT("min_value")));
				const float Max = static_cast<float>(Args->GetNumberField(TEXT("max_value")));
				if (Max < Min) { Fail(Out, Error, TEXT("invalid_range"), TEXT("max_value must be >= min_value.")); return false; }
				if (bVolume) FFractureEngineSelection::SelectByVolume(C, Result, Min, Max);
				else FFractureEngineSelection::SelectBySize(C, Result, Min, Max);
				WriteSelection(Result, Out); Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("succeeded"));
				Summary = FString::Printf(TEXT("Selected %d transforms by %s."), Result.Num(), bVolume ? TEXT("volume") : TEXT("size")); return true;
			}, nullptr, 15});
	};
	RegisterRangeSelection(TEXT("fracture_selection_by_size"), TEXT("Select leaf transforms by size range."), false);
	RegisterRangeSelection(TEXT("fracture_selection_by_volume"), TEXT("Select leaf transforms by volume range."), true);

	Registry.Register({TEXT("fracture_auto_cluster_apply"), TEXT("Auto-cluster leaf pieces and verify hierarchy growth."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("cluster_count"), FSololmcpSchemaBuilder::Integer(TEXT("Target cluster count."), 1, 10000)},
		}, {TEXT("asset_path"), TEXT("cluster_count")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& C = *Asset->GetGeometryCollection(); TArray<int32> Leaves; FFractureEngineSelection::SelectLeaf(C, Leaves);
			const int32 Before = C.NumElements(FGeometryCollection::TransformGroup);
			FScopedTransaction Tx(NSLOCTEXT("SOMOLMCP", "FractureAutoCluster", "SOMOLMCP Auto Cluster")); Asset->Modify();
			FFractureEngineClustering::AutoCluster(C, Leaves, EFractureEngineClusterSizeMethod::ByNumber,
				static_cast<uint32>(Args->GetNumberField(TEXT("cluster_count"))), 0.0f, 0.0f, true, true, true);
			const int32 Added = C.NumElements(FGeometryCollection::TransformGroup) - Before;
			if (Added <= 0) { Tx.Cancel(); Fail(Out, Error, TEXT("auto_cluster_no_change"), TEXT("Auto-cluster created no hierarchy nodes.")); return false; }
			if (!Save(Context, *Asset, Out, Error)) return false; WriteReceipt(*Asset, TEXT("auto_cluster"), true, Out);
			Out->SetNumberField(TEXT("clusters_added"), Added); Summary = FString::Printf(TEXT("Added %d clusters."), Added); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_cluster_selected_apply"), TEXT("Create a cluster from explicit transform indices."), BoneSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& C = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseBones(Args, C.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			FScopedTransaction Tx(NSLOCTEXT("SOMOLMCP", "FractureClusterSelected", "SOMOLMCP Cluster Selected")); Asset->Modify();
			if (!FFractureEngineClustering::ClusterSelected(C, Bones)) { Tx.Cancel(); Fail(Out, Error, TEXT("cluster_selected_no_change"), TEXT("Selection could not form a cluster.")); return false; }
			if (!Save(Context, *Asset, Out, Error)) return false; WriteReceipt(*Asset, TEXT("cluster_selected"), true, Out); WriteSelection(Bones, Out);
			Summary = TEXT("Created cluster from selected transforms."); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_split_islands_apply"), TEXT("Split disconnected geometry islands for selected transforms."), BoneSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& C = *Asset->GetGeometryCollection(); TArray<int32> Bones;
			if (!ParseBones(Args, C.NumElements(FGeometryCollection::TransformGroup), Bones, Out, Error)) return false;
			FDataflowTransformSelection Selection; Selection.InitializeFromCollection(C, false); for (const int32 Bone : Bones) Selection.SetSelected(Bone);
			const int32 Before = C.NumElements(FGeometryCollection::GeometryGroup);
			FScopedTransaction Tx(NSLOCTEXT("SOMOLMCP", "FractureSplitIslands", "SOMOLMCP Split Islands")); Asset->Modify();
			FFractureEngineUtility::SplitIslands(C, Selection, 0.001f, 0.0f);
			const int32 Added = C.NumElements(FGeometryCollection::GeometryGroup) - Before;
			if (Added <= 0) { Tx.Cancel(); Fail(Out, Error, TEXT("split_islands_no_change"), TEXT("No disconnected islands were found.")); return false; }
			if (!Save(Context, *Asset, Out, Error)) return false; WriteReceipt(*Asset, TEXT("split_islands"), true, Out);
			Out->SetNumberField(TEXT("geometry_added"), Added); Summary = FString::Printf(TEXT("Split %d islands."), Added); return true;
		}, nullptr, 2});

	Registry.Register({TEXT("fracture_collection_validate_repair"), TEXT("Run native hierarchy validation/repair and persist deterministic readback."), AssetSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error); if (!Asset) return false;
			FGeometryCollection& C = *Asset->GetGeometryCollection(); const int32 BT = C.NumElements(FGeometryCollection::TransformGroup); const int32 BG = C.NumElements(FGeometryCollection::GeometryGroup);
			FScopedTransaction Tx(NSLOCTEXT("SOMOLMCP", "FractureValidateRepair", "SOMOLMCP Validate Geometry Collection")); Asset->Modify();
			FFractureEngineUtility::ValidateGeometryCollection(C, true, true, true);
			const bool Changed = BT != C.NumElements(FGeometryCollection::TransformGroup) || BG != C.NumElements(FGeometryCollection::GeometryGroup);
			if (!Save(Context, *Asset, Out, Error)) return false; WriteReceipt(*Asset, TEXT("validate_repair"), Changed, Out);
			Out->SetBoolField(TEXT("repair_applied"), Changed); Summary = Changed ? TEXT("Repaired Geometry Collection hierarchy.") : TEXT("No hierarchy defects required repair."); return true;
		}, nullptr, 3});
#else
	(void)Registry;
#endif
}
}
