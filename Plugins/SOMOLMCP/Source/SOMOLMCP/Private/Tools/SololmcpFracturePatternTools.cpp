// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "Dataflow/DataflowSelection.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "FractureEngineFracturing.h"
#include "FractureEngineSelection.h"
#include "GeometryCollection/Facades/CollectionBoundsFacade.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#endif

namespace UE::SOMOLMCP
{
namespace FracturePattern
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

	static TSharedRef<FJsonObject> NoiseProperties()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("amplitude"), FSololmcpSchemaBuilder::Number(TEXT("Cut-surface noise amplitude."), 0.0, 100000.0)},
			{TEXT("frequency"), FSololmcpSchemaBuilder::Number(TEXT("Noise frequency."), 0.000001, 1000.0)},
			{TEXT("persistence"), FSololmcpSchemaBuilder::Number(TEXT("Noise persistence."), 0.0, 1.0)},
			{TEXT("lacunarity"), FSololmcpSchemaBuilder::Number(TEXT("Noise lacunarity."), 1.0, 10.0)},
			{TEXT("octaves"), FSololmcpSchemaBuilder::Integer(TEXT("Noise octave count."), 0, 12)},
			{TEXT("point_spacing"), FSololmcpSchemaBuilder::Number(TEXT("Noise sampling spacing in cm."), 0.01, 100000.0)},
		}, {}, FString(), false);
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	static double Number(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, double Default)
	{
		double Value = Default;
		Args->TryGetNumberField(Name, Value);
		return Value;
	}

	static bool Boolean(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, bool Default)
	{
		bool Value = Default;
		Args->TryGetBoolField(Name, Value);
		return Value;
	}

	static UGeometryCollection* LoadCollection(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FString Path;
		if (!Args->TryGetStringField(TEXT("asset_path"), Path) || !Path.StartsWith(TEXT("/Game/")))
		{
			Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("asset_path must identify a Geometry Collection under /Game/."));
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
		if (Collection.NumElements(FGeometryCollection::GeometryGroup) <= 0)
		{
			Fail(Out, Error, TEXT("geometry_collection_empty"), TEXT("The Geometry Collection has no fractureable geometry."));
			return nullptr;
		}
		return Asset;
	}

	static bool GetBounds(FGeometryCollection& Collection, FBox& OutBounds)
	{
		GeometryCollection::Facades::FBoundsFacade Bounds(Collection);
		if (!Bounds.IsValid())
		{
			Bounds.DefineSchema();
			Bounds.UpdateBoundingBox();
		}
		OutBounds = Bounds.GetBoundingBoxInCollectionSpace();
		return OutBounds.IsValid && OutBounds.GetExtent().GetMin() > UE_SMALL_NUMBER;
	}

	static uint32 Digest(const FGeometryCollection& Collection)
	{
		uint32 Hash = 0;
		auto Add = [&Hash](int32 Value) { Hash = HashCombineFast(Hash, GetTypeHash(Value)); };
		Add(Collection.NumElements(FGeometryCollection::TransformGroup));
		Add(Collection.NumElements(FGeometryCollection::GeometryGroup));
		Add(Collection.NumElements(FGeometryCollection::VerticesGroup));
		Add(Collection.NumElements(FGeometryCollection::FacesGroup));
		if (Collection.Vertex.Num() > 0)
		{
			const TManagedArray<FVector3f>& Vertices = Collection.Vertex;
			for (int32 Index = 0; Index < Vertices.Num(); Index += FMath::Max(1, Vertices.Num() / 97))
			{
				Hash = HashCombineFast(Hash, GetTypeHash(Vertices[Index].X));
				Hash = HashCombineFast(Hash, GetTypeHash(Vertices[Index].Y));
				Hash = HashCombineFast(Hash, GetTypeHash(Vertices[Index].Z));
			}
		}
		return Hash;
	}

	static void Restore(UGeometryCollection& Asset, TUniquePtr<FGeometryCollection>& Snapshot)
	{
		if (Snapshot.IsValid() && Asset.GetGeometryCollection().IsValid())
		{
			*Asset.GetGeometryCollection() = MoveTemp(*Snapshot);
			Asset.InvalidateCollection();
			Asset.CreateSimulationData();
			Asset.RebuildRenderData();
			Asset.PropagateMarkDirtyToComponents();
		}
	}

	static bool SaveReadback(const FSololmcpToolExecutionContext& Context, UGeometryCollection& Asset,
		uint32 ExpectedDigest, TUniquePtr<FGeometryCollection>& Snapshot, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Asset.InvalidateCollection();
		Asset.CreateSimulationData();
		Asset.RebuildRenderData();
		Asset.PropagateMarkDirtyToComponents();
		Asset.MarkPackageDirty();
		FString SaveError;
		if (!Context.Services.SaveAsset(Asset.GetPathName(), false, SaveError))
		{
			Restore(Asset, Snapshot);
			Fail(Out, Error, TEXT("fracture_pattern_save_failed"),
				SaveError.IsEmpty() ? TEXT("Save failed; the in-memory Geometry Collection was rolled back.") : SaveError);
			Out->SetBoolField(TEXT("rollback_applied"), true);
			return false;
		}
		FString ReadbackError;
		UGeometryCollection* Readback = Cast<UGeometryCollection>(Context.Services.LoadAsset(Asset.GetPathName(), ReadbackError));
		if (!Readback || !Readback->GetGeometryCollection().IsValid() || Digest(*Readback->GetGeometryCollection()) != ExpectedDigest)
		{
			Restore(Asset, Snapshot);
			FString RollbackError;
			Context.Services.SaveAsset(Asset.GetPathName(), false, RollbackError);
			Fail(Out, Error, TEXT("fracture_pattern_readback_mismatch"),
				TEXT("Saved pattern result did not match readback; rollback was applied."));
			Out->SetBoolField(TEXT("rollback_applied"), true);
			return false;
		}
		return true;
	}

	static FDataflowTransformSelection LeafSelection(FGeometryCollection& Collection)
	{
		FDataflowTransformSelection Selection;
		Selection.InitializeFromCollection(Collection, false);
		FFractureEngineSelection::SelectLeaf(Collection, Selection);
		return Selection;
	}

	static FIslandSplitSettings IslandSettings(const TSharedRef<FJsonObject>& Args)
	{
		return FIslandSplitSettings(Boolean(Args, TEXT("split_islands"), true),
			Number(Args, TEXT("close_vertex_distance"), 0.001),
			Number(Args, TEXT("vertex_surface_bridge_distance"), 0.0));
	}

	static void Noise(const TSharedRef<FJsonObject>& Args, float& Amplitude, float& Frequency,
		float& Persistence, float& Lacunarity, int32& Octaves, float& PointSpacing)
	{
		Amplitude = static_cast<float>(Number(Args, TEXT("amplitude"), 0.0));
		Frequency = static_cast<float>(Number(Args, TEXT("frequency"), 0.1));
		Persistence = static_cast<float>(Number(Args, TEXT("persistence"), 0.5));
		Lacunarity = static_cast<float>(Number(Args, TEXT("lacunarity"), 2.0));
		Octaves = static_cast<int32>(Number(Args, TEXT("octaves"), Amplitude > 0 ? 3 : 0));
		PointSpacing = static_cast<float>(Number(Args, TEXT("point_spacing"), 10.0));
	}

	static void WriteVector(const TCHAR* Name, const FVector& Value, TSharedRef<FJsonObject>& Out)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueNumber>(Value.X));
		Values.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Values.Add(MakeShared<FJsonValueNumber>(Value.Z));
		Out->SetArrayField(Name, Values);
	}

	static void WriteSites(const TArray<FVector>& Sites, TSharedRef<FJsonObject>& Out)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 Limit = FMath::Min(Sites.Num(), 2048);
		Values.Reserve(Limit);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			TSharedRef<FJsonObject> Site = MakeShared<FJsonObject>();
			Site->SetNumberField(TEXT("x"), Sites[Index].X);
			Site->SetNumberField(TEXT("y"), Sites[Index].Y);
			Site->SetNumberField(TEXT("z"), Sites[Index].Z);
			Values.Add(MakeShared<FJsonValueObject>(Site));
		}
		Out->SetArrayField(TEXT("sites"), Values);
		Out->SetNumberField(TEXT("site_count"), Sites.Num());
		Out->SetBoolField(TEXT("sites_truncated"), Sites.Num() > Limit);
	}

	static TArray<FVector> ClusteredSites(const FBox& Bounds, int32 Clusters, int32 SitesPerCluster,
		float Radius, int32 Seed)
	{
		FRandomStream Random(Seed);
		TArray<FVector> Sites;
		Sites.Reserve(Clusters * SitesPerCluster);
		const FVector Min = Bounds.Min;
		const FVector Size = Bounds.GetSize();
		for (int32 Cluster = 0; Cluster < Clusters; ++Cluster)
		{
			const FVector Center = Min + FVector(Random.FRand(), Random.FRand(), Random.FRand()) * Size;
			for (int32 Site = 0; Site < SitesPerCluster; ++Site)
			{
				Sites.Add(Bounds.GetClosestPointTo(Center + Random.VRand() * Random.FRand() * Radius));
			}
		}
		return Sites;
	}

	static TArray<FVector> RadialSites(const FBox& Bounds, const FVector& CenterOffset, const FVector& Axis,
		int32 AngularSteps, int32 RadialSteps, float Radius, float Variability, int32 Seed)
	{
		FRandomStream Random(Seed);
		FVector Up = Axis.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		FVector BasisX, BasisY;
		Up.FindBestAxisVectors(BasisX, BasisY);
		const FVector Center = Bounds.GetCenter() + CenterOffset;
		TArray<FVector> Sites;
		Sites.Reserve(AngularSteps * RadialSteps);
		for (int32 Ring = 0; Ring < RadialSteps; ++Ring)
		{
			const double Distance = Radius * (Ring + 0.5) / RadialSteps;
			for (int32 Step = 0; Step < AngularSteps; ++Step)
			{
				const double Angle = 2.0 * PI * Step / AngularSteps;
				const FVector Point = Center + Distance * (FMath::Cos(Angle) * BasisX + FMath::Sin(Angle) * BasisY)
					+ Random.VRand() * Random.FRand() * Variability;
				Sites.Add(Point);
			}
		}
		return Sites;
	}

	using FPatternOperation = TFunction<int32(FGeometryCollection&, FDataflowTransformSelection&, const FBox&)>;

	static bool ApplyPattern(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		const FString& Operation, FPatternOperation&& OperationFn, TSharedRef<FJsonObject>& Out,
		FString& Summary, FString& Error)
	{
		UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
		if (!Asset) return false;
		FGeometryCollection& Collection = *Asset->GetGeometryCollection();
		FBox Bounds;
		if (!GetBounds(Collection, Bounds))
		{
			Fail(Out, Error, TEXT("fracture_pattern_invalid_bounds"), TEXT("Geometry Collection bounds are empty or degenerate."));
			return false;
		}
		FDataflowTransformSelection Selection = LeafSelection(Collection);
		if (!Selection.AnySelected())
		{
			Fail(Out, Error, TEXT("fracture_pattern_empty_selection"), TEXT("No leaf geometry is available to fracture."));
			return false;
		}
		const int32 BeforeGeometry = Collection.NumElements(FGeometryCollection::GeometryGroup);
		const uint32 BeforeDigest = Digest(Collection);
		TUniquePtr<FGeometryCollection> Snapshot(Collection.NewCopy<FGeometryCollection>());
		FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FracturePatternApply", "SOMOLMCP Fracture Pattern"));
		Asset->Modify();
		const int32 FirstNewGeometry = OperationFn(Collection, Selection, Bounds);
		const int32 AfterGeometry = Collection.NumElements(FGeometryCollection::GeometryGroup);
		const uint32 AfterDigest = Digest(Collection);
		if (FirstNewGeometry == INDEX_NONE || AfterGeometry <= BeforeGeometry || AfterDigest == BeforeDigest)
		{
			Restore(*Asset, Snapshot);
			Transaction.Cancel();
			Fail(Out, Error, TEXT("fracture_pattern_no_change"), TEXT("The native Chaos cutter produced no accepted geometry; rollback was applied."));
			Out->SetBoolField(TEXT("rollback_applied"), true);
			return false;
		}
		if (!SaveReadback(Context, *Asset, AfterDigest, Snapshot, Out, Error))
		{
			Transaction.Cancel();
			return false;
		}
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("fracture_pattern_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
		Out->SetStringField(TEXT("operation"), Operation);
		Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
		Out->SetStringField(TEXT("pre_digest"), FString::Printf(TEXT("%08x"), BeforeDigest));
		Out->SetStringField(TEXT("post_digest"), FString::Printf(TEXT("%08x"), AfterDigest));
		Out->SetNumberField(TEXT("first_new_geometry_index"), FirstNewGeometry);
		Out->SetNumberField(TEXT("before_geometry_count"), BeforeGeometry);
		Out->SetNumberField(TEXT("after_geometry_count"), AfterGeometry);
		Out->SetNumberField(TEXT("pieces_added"), AfterGeometry - BeforeGeometry);
		Out->SetBoolField(TEXT("mutation_applied"), true);
		Out->SetBoolField(TEXT("saved"), true);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetBoolField(TEXT("rollback_applied"), false);
		Summary = FString::Printf(TEXT("%s created %d Chaos pieces in %s."), *Operation,
			AfterGeometry - BeforeGeometry, *Asset->GetPathName());
		return true;
	}

	static TSharedRef<FJsonObject> CommonCutSchema(const TMap<FString, TSharedRef<FJsonObject>>& Extra,
		const TArray<FString>& ExtraRequired = {})
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties = Extra;
		Properties.Add(TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024));
		Properties.Add(TEXT("seed"), FSololmcpSchemaBuilder::Integer(TEXT("Deterministic random seed."), 0, MAX_int32));
		Properties.Add(TEXT("chance"), FSololmcpSchemaBuilder::Number(TEXT("Chance to fracture each selected leaf."), 0.0, 1.0));
		Properties.Add(TEXT("split_islands"), FSololmcpSchemaBuilder::Boolean(TEXT("Split disconnected islands after cutting.")));
		Properties.Add(TEXT("close_vertex_distance"), FSololmcpSchemaBuilder::Number(TEXT("Island connection tolerance."), 0.0, 100.0));
		Properties.Add(TEXT("vertex_surface_bridge_distance"), FSololmcpSchemaBuilder::Number(TEXT("Optional surface bridge distance."), 0.0, 10000.0));
		Properties.Add(TEXT("grout"), FSololmcpSchemaBuilder::Number(TEXT("Gap between generated pieces."), 0.0, 10000.0));
		Properties.Add(TEXT("amplitude"), FSololmcpSchemaBuilder::Number(TEXT("Cut-surface noise amplitude."), 0.0, 100000.0));
		Properties.Add(TEXT("frequency"), FSololmcpSchemaBuilder::Number(TEXT("Noise frequency."), 0.000001, 1000.0));
		Properties.Add(TEXT("persistence"), FSololmcpSchemaBuilder::Number(TEXT("Noise persistence."), 0.0, 1.0));
		Properties.Add(TEXT("lacunarity"), FSololmcpSchemaBuilder::Number(TEXT("Noise lacunarity."), 1.0, 10.0));
		Properties.Add(TEXT("octaves"), FSololmcpSchemaBuilder::Integer(TEXT("Noise octave count."), 0, 12));
		Properties.Add(TEXT("point_spacing"), FSololmcpSchemaBuilder::Number(TEXT("Noise point spacing."), 0.01, 100000.0));
		Properties.Add(TEXT("collision_sample_spacing"), FSololmcpSchemaBuilder::Number(TEXT("Collision sample spacing."), 0.0, 100000.0));
		TArray<FString> Required = ExtraRequired;
		Required.Add(TEXT("asset_path"));
		return FSololmcpSchemaBuilder::Object(Properties, Required, FString(), false);
	}
#endif
}

void RegisterFracturePatternTools(FSololmcpToolRegistry& Registry)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	using namespace FracturePattern;

	Registry.Register({TEXT("fracture_pattern_capabilities_inspect"),
		TEXT("Inspect the native UE 5.8 Chaos fracture pattern API surface used by SOMOLMCP."),
		FSololmcpSchemaBuilder::Object({}, {}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString& Summary, FString&)
		{
			Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("engine_gate"), TEXT("UE 5.8+")); Out->SetStringField(TEXT("backend"), TEXT("native_fracture_engine_cpp"));
			Out->SetStringField(TEXT("patterns"), TEXT("clustered_voronoi,radial_voronoi,plane,slice,brick,mesh"));
			Out->SetBoolField(TEXT("python_bridge_used"), false); Summary = TEXT("Native UE 5.8 fracture pattern surface is available."); return true;
		}, nullptr, 20});

	Registry.Register({TEXT("fracture_pattern_noise_settings_inspect"), TEXT("Validate and report native cut-surface noise settings without mutation."), NoiseProperties(),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString&)
		{
			float A,F,P,L,S; int32 O; Noise(Args,A,F,P,L,O,S); Out->SetBoolField(TEXT("ok"),true); Out->SetStringField(TEXT("status"),TEXT("succeeded"));
			Out->SetNumberField(TEXT("amplitude"),A); Out->SetNumberField(TEXT("frequency"),F); Out->SetNumberField(TEXT("persistence"),P);
			Out->SetNumberField(TEXT("lacunarity"),L); Out->SetNumberField(TEXT("octaves"),O); Out->SetNumberField(TEXT("point_spacing"),S);
			Out->SetBoolField(TEXT("noise_enabled"),A>0 && O>0); Summary=TEXT("Validated native fracture noise settings."); return true;
		}, nullptr, 20});

	Registry.Register({TEXT("fracture_pattern_bounds_inspect"), TEXT("Inspect Geometry Collection bounds used by native pattern cutters."), AssetSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset=LoadCollection(Context,Args,Out,Error); if(!Asset)return false; FBox Bounds;
			if(!GetBounds(*Asset->GetGeometryCollection(),Bounds)){Fail(Out,Error,TEXT("fracture_pattern_invalid_bounds"),TEXT("Collection bounds are invalid."));return false;}
			Out->SetBoolField(TEXT("ok"),true);Out->SetStringField(TEXT("status"),TEXT("succeeded"));WriteVector(TEXT("bounds_min"),Bounds.Min,Out);
			WriteVector(TEXT("bounds_max"),Bounds.Max,Out);WriteVector(TEXT("bounds_center"),Bounds.GetCenter(),Out);WriteVector(TEXT("bounds_size"),Bounds.GetSize(),Out);
			Summary=TEXT("Inspected Geometry Collection fracture bounds.");return true;
		}, nullptr, 15});

	Registry.Register({TEXT("fracture_pattern_topology_inspect"),
		TEXT("Inspect Geometry Collection transform, geometry, vertex, face, and leaf topology before native fracture authoring."), AssetSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
			if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection();
			FDataflowTransformSelection Leaves = LeafSelection(Collection);
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetNumberField(TEXT("transform_count"), Collection.NumElements(FGeometryCollection::TransformGroup));
			Out->SetNumberField(TEXT("geometry_count"), Collection.NumElements(FGeometryCollection::GeometryGroup));
			Out->SetNumberField(TEXT("vertex_count"), Collection.NumElements(FGeometryCollection::VerticesGroup));
			Out->SetNumberField(TEXT("face_count"), Collection.NumElements(FGeometryCollection::FacesGroup));
			Out->SetNumberField(TEXT("leaf_count"), Leaves.NumSelected());
			Out->SetNumberField(TEXT("content_digest"), Digest(Collection));
			Out->SetBoolField(TEXT("fracture_ready"), Leaves.NumSelected() > 0 && Collection.NumElements(FGeometryCollection::FacesGroup) > 0);
			Summary = FString::Printf(TEXT("Geometry Collection topology contains %d fractureable leaves."), Leaves.NumSelected());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("fracture_pattern_leaf_selection_inspect"),
		TEXT("Return the native Chaos leaf transform selection used by pattern cutters, with bounded index readback."), AssetSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
			if (!Asset) return false;
			FDataflowTransformSelection Leaves = LeafSelection(*Asset->GetGeometryCollection());
			const TArray<int32> Selected = Leaves.AsArray();
			TArray<TSharedPtr<FJsonValue>> Values;
			const int32 Limit = FMath::Min(Selected.Num(), 4096);
			Values.Reserve(Limit);
			for (int32 Index = 0; Index < Limit; ++Index)
			{
				Values.Add(MakeShared<FJsonValueNumber>(Selected[Index]));
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetNumberField(TEXT("leaf_count"), Selected.Num());
			Out->SetArrayField(TEXT("leaf_transform_indices"), Values);
			Out->SetBoolField(TEXT("indices_truncated"), Selected.Num() > Limit);
			Summary = FString::Printf(TEXT("Inspected %d native fracture leaf transforms."), Selected.Num());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("fracture_pattern_site_budget_validate"),
		TEXT("Validate a proposed native pattern site count against collection complexity before mutation."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
			{TEXT("site_count"), FSololmcpSchemaBuilder::Integer(TEXT("Proposed Voronoi or cutter site count."), 1, 1048576)},
			{TEXT("max_output_pieces"), FSololmcpSchemaBuilder::Integer(TEXT("Hard output-piece safety budget."), 1, 1048576)},
		}, {TEXT("asset_path"), TEXT("site_count"), TEXT("max_output_pieces")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
			if (!Asset) return false;
			FGeometryCollection& Collection = *Asset->GetGeometryCollection();
			const int64 Leaves = LeafSelection(Collection).NumSelected();
			const int64 Sites = static_cast<int64>(Args->GetNumberField(TEXT("site_count")));
			const int64 Budget = static_cast<int64>(Args->GetNumberField(TEXT("max_output_pieces")));
			const int64 EstimatedUpperBound = FMath::Max<int64>(1, Leaves) * FMath::Max<int64>(1, Sites);
			const bool bAccepted = EstimatedUpperBound <= Budget;
			Out->SetBoolField(TEXT("ok"), bAccepted);
			Out->SetStringField(TEXT("status"), bAccepted ? TEXT("succeeded") : TEXT("blocked"));
			Out->SetStringField(TEXT("reason_code"), bAccepted ? TEXT("within_piece_budget") : TEXT("fracture_piece_budget_exceeded"));
			Out->SetNumberField(TEXT("leaf_count"), static_cast<double>(Leaves));
			Out->SetNumberField(TEXT("site_count"), static_cast<double>(Sites));
			Out->SetNumberField(TEXT("estimated_piece_upper_bound"), static_cast<double>(EstimatedUpperBound));
			Out->SetNumberField(TEXT("max_output_pieces"), static_cast<double>(Budget));
			Summary = bAccepted ? TEXT("Native fracture site budget accepted.") : TEXT("Native fracture site budget blocked before mutation.");
			if (!bAccepted) Error = Summary;
			return bAccepted;
		}, nullptr, 20});

	auto ClusterSchema=[](bool bWrite){return CommonCutSchema({
		{TEXT("cluster_count"),FSololmcpSchemaBuilder::Integer(TEXT("Number of site clusters."),1,1024)},
		{TEXT("sites_per_cluster"),FSololmcpSchemaBuilder::Integer(TEXT("Voronoi sites per cluster."),2,1024)},
		{TEXT("cluster_radius"),FSololmcpSchemaBuilder::Number(TEXT("Cluster radius in cm."),0.01,1000000.0)}},
		bWrite?TArray<FString>{TEXT("cluster_count"),TEXT("sites_per_cluster"),TEXT("cluster_radius")} : TArray<FString>{});};

	Registry.Register({TEXT("fracture_clustered_voronoi_sites_preview"), TEXT("Preview deterministic clustered Voronoi sites in target collection bounds."), ClusterSchema(false),
		[](const FSololmcpToolExecutionContext& Context,const TSharedRef<FJsonObject>& Args,TSharedRef<FJsonObject>& Out,FString& Summary,FString& Error)
		{UGeometryCollection* A=LoadCollection(Context,Args,Out,Error);if(!A)return false;FBox B;if(!GetBounds(*A->GetGeometryCollection(),B)){Fail(Out,Error,TEXT("invalid_bounds"),TEXT("Invalid bounds."));return false;}
		TArray<FVector>S=ClusteredSites(B,(int32)Number(Args,TEXT("cluster_count"),4),(int32)Number(Args,TEXT("sites_per_cluster"),8),(float)Number(Args,TEXT("cluster_radius"),B.GetExtent().GetMin()*.2),(int32)Number(Args,TEXT("seed"),0));
		Out->SetBoolField(TEXT("ok"),true);Out->SetStringField(TEXT("status"),TEXT("succeeded"));WriteSites(S,Out);Summary=TEXT("Generated clustered Voronoi preview sites.");return true;},nullptr,15});

	auto RadialSchema=[](){return CommonCutSchema({
		{TEXT("angular_steps"),FSololmcpSchemaBuilder::Integer(TEXT("Sites per radial ring."),3,256)},
		{TEXT("radial_steps"),FSololmcpSchemaBuilder::Integer(TEXT("Number of radial rings."),1,256)},
		{TEXT("radius"),FSololmcpSchemaBuilder::Number(TEXT("Pattern radius in cm."),0.01,1000000.0)},
		{TEXT("variability"),FSololmcpSchemaBuilder::Number(TEXT("Per-site random displacement."),0.0,1000000.0)},
		{TEXT("center_x"),FSololmcpSchemaBuilder::Number(TEXT("Center offset X."))},{TEXT("center_y"),FSololmcpSchemaBuilder::Number(TEXT("Center offset Y."))},{TEXT("center_z"),FSololmcpSchemaBuilder::Number(TEXT("Center offset Z."))},
		{TEXT("axis_x"),FSololmcpSchemaBuilder::Number(TEXT("Axis X."))},{TEXT("axis_y"),FSololmcpSchemaBuilder::Number(TEXT("Axis Y."))},{TEXT("axis_z"),FSololmcpSchemaBuilder::Number(TEXT("Axis Z."))}},
		{TEXT("angular_steps"),TEXT("radial_steps"),TEXT("radius")});};

	Registry.Register({TEXT("fracture_radial_voronoi_sites_preview"),TEXT("Preview deterministic radial Voronoi sites in target collection bounds."),RadialSchema(),
		[](const FSololmcpToolExecutionContext& Context,const TSharedRef<FJsonObject>& Args,TSharedRef<FJsonObject>& Out,FString& Summary,FString& Error)
		{UGeometryCollection*A=LoadCollection(Context,Args,Out,Error);if(!A)return false;FBox B;if(!GetBounds(*A->GetGeometryCollection(),B)){Fail(Out,Error,TEXT("invalid_bounds"),TEXT("Invalid bounds."));return false;}
		TArray<FVector>S=RadialSites(B,FVector(Number(Args,TEXT("center_x"),0),Number(Args,TEXT("center_y"),0),Number(Args,TEXT("center_z"),0)),FVector(Number(Args,TEXT("axis_x"),0),Number(Args,TEXT("axis_y"),0),Number(Args,TEXT("axis_z"),1)),(int32)Number(Args,TEXT("angular_steps"),8),(int32)Number(Args,TEXT("radial_steps"),4),(float)Number(Args,TEXT("radius"),B.GetExtent().GetMin()),(float)Number(Args,TEXT("variability"),0),(int32)Number(Args,TEXT("seed"),0));
		Out->SetBoolField(TEXT("ok"),true);Out->SetStringField(TEXT("status"),TEXT("succeeded"));WriteSites(S,Out);Summary=TEXT("Generated radial Voronoi preview sites.");return true;},nullptr,15});

	auto RegisterVoronoi=[&](const TCHAR* Name,const FString& Description,bool bRadial)
	{
		Registry.Register({Name,Description,bRadial?RadialSchema():ClusterSchema(true),
			[bRadial](const FSololmcpToolExecutionContext& Context,const TSharedRef<FJsonObject>& Args,TSharedRef<FJsonObject>& Out,FString& Summary,FString& Error)
			{return ApplyPattern(Context,Args,bRadial?TEXT("native_radial_voronoi"):TEXT("native_clustered_voronoi"),
				[Args,bRadial](FGeometryCollection& C,FDataflowTransformSelection& Sel,const FBox& B)
				{TArray<FVector> Sites=bRadial?RadialSites(B,FVector(Number(Args,TEXT("center_x"),0),Number(Args,TEXT("center_y"),0),Number(Args,TEXT("center_z"),0)),FVector(Number(Args,TEXT("axis_x"),0),Number(Args,TEXT("axis_y"),0),Number(Args,TEXT("axis_z"),1)),(int32)Number(Args,TEXT("angular_steps"),8),(int32)Number(Args,TEXT("radial_steps"),4),(float)Number(Args,TEXT("radius"),B.GetExtent().GetMin()),(float)Number(Args,TEXT("variability"),0),(int32)Number(Args,TEXT("seed"),0)):
					ClusteredSites(B,(int32)Number(Args,TEXT("cluster_count"),4),(int32)Number(Args,TEXT("sites_per_cluster"),8),(float)Number(Args,TEXT("cluster_radius"),B.GetExtent().GetMin()*.2),(int32)Number(Args,TEXT("seed"),0));
					float A,F,P,L,S;int32 O;Noise(Args,A,F,P,L,O,S);return FFractureEngineFracturing::VoronoiFracture(C,Sel,Sites,FTransform::Identity,(int32)Number(Args,TEXT("seed"),0),(float)Number(Args,TEXT("chance"),1),IslandSettings(Args),(float)Number(Args,TEXT("grout"),0),A,F,P,L,O,S,true,(float)Number(Args,TEXT("collision_sample_spacing"),50));},Out,Summary,Error);},nullptr,2});
	};
	RegisterVoronoi(TEXT("fracture_clustered_voronoi_apply"),TEXT("Apply clustered-site Voronoi fracture through the native UE 5.8 Chaos API."),false);
	RegisterVoronoi(TEXT("fracture_radial_voronoi_apply"),TEXT("Apply radial-site Voronoi fracture through the native UE 5.8 Chaos API."),true);

	Registry.Register({TEXT("fracture_plane_cut_apply"),TEXT("Apply native randomized plane cuts with noise, save/readback, receipt, and rollback."),CommonCutSchema({{TEXT("plane_count"),FSololmcpSchemaBuilder::Integer(TEXT("Number of cutting planes."),1,4096)}},{TEXT("plane_count")}),
		[](const FSololmcpToolExecutionContext& C,const TSharedRef<FJsonObject>& A,TSharedRef<FJsonObject>& O,FString& S,FString& E){return ApplyPattern(C,A,TEXT("native_plane_cut"),[A](FGeometryCollection& G,FDataflowTransformSelection& Sel,const FBox&B){float N,F,P,L,PS;int32 Oct;Noise(A,N,F,P,L,Oct,PS);return FFractureEngineFracturing::PlaneCutter(G,Sel,B,FTransform::Identity,(int32)Number(A,TEXT("plane_count"),3),(int32)Number(A,TEXT("seed"),0),(float)Number(A,TEXT("chance"),1),IslandSettings(A),(float)Number(A,TEXT("grout"),0),N,F,P,L,Oct,PS,true,(float)Number(A,TEXT("collision_sample_spacing"),50));},O,S,E);},nullptr,2});

	Registry.Register({TEXT("fracture_slice_cut_apply"),TEXT("Apply native orthogonal slice cuts with angular/offset variation and verified rollback."),CommonCutSchema({
		{TEXT("slices_x"),FSololmcpSchemaBuilder::Integer(TEXT("X slices."),0,256)},{TEXT("slices_y"),FSololmcpSchemaBuilder::Integer(TEXT("Y slices."),0,256)},{TEXT("slices_z"),FSololmcpSchemaBuilder::Integer(TEXT("Z slices."),0,256)},
		{TEXT("angle_variation"),FSololmcpSchemaBuilder::Number(TEXT("Slice angle variation in degrees."),0.0,180.0)},{TEXT("offset_variation"),FSololmcpSchemaBuilder::Number(TEXT("Normalized slice offset variation."),0.0,1.0)}},{TEXT("slices_x"),TEXT("slices_y"),TEXT("slices_z")}),
		[](const FSololmcpToolExecutionContext&C,const TSharedRef<FJsonObject>&A,TSharedRef<FJsonObject>&O,FString&S,FString&E){return ApplyPattern(C,A,TEXT("native_slice_cut"),[A](FGeometryCollection&G,FDataflowTransformSelection&Sel,const FBox&B){float N,F,P,L,PS;int32 Oct;Noise(A,N,F,P,L,Oct,PS);return FFractureEngineFracturing::SliceCutter(G,Sel,B,(int32)Number(A,TEXT("slices_x"),1),(int32)Number(A,TEXT("slices_y"),1),(int32)Number(A,TEXT("slices_z"),1),(float)Number(A,TEXT("angle_variation"),0),(float)Number(A,TEXT("offset_variation"),0),(int32)Number(A,TEXT("seed"),0),(float)Number(A,TEXT("chance"),1),IslandSettings(A),(float)Number(A,TEXT("grout"),0),N,F,P,L,Oct,PS,true,(float)Number(A,TEXT("collision_sample_spacing"),50));},O,S,E);},nullptr,2});

	Registry.Register({TEXT("fracture_brick_cut_apply"),TEXT("Apply native Chaos brick-bond fracture with verified persistence and rollback."),CommonCutSchema({
		{TEXT("bond"),FSololmcpSchemaBuilder::String(TEXT("Brick bond."),{TEXT("stretcher"),TEXT("stack"),TEXT("english"),TEXT("header"),TEXT("flemish")})},
		{TEXT("brick_length"),FSololmcpSchemaBuilder::Number(TEXT("Brick length."),0.01,1000000.0)},{TEXT("brick_height"),FSololmcpSchemaBuilder::Number(TEXT("Brick height."),0.01,1000000.0)},{TEXT("brick_depth"),FSololmcpSchemaBuilder::Number(TEXT("Brick depth."),0.01,1000000.0)}},{TEXT("bond"),TEXT("brick_length"),TEXT("brick_height"),TEXT("brick_depth")}),
		[](const FSololmcpToolExecutionContext&C,const TSharedRef<FJsonObject>&A,TSharedRef<FJsonObject>&O,FString&S,FString&E){return ApplyPattern(C,A,TEXT("native_brick_cut"),[A](FGeometryCollection&G,FDataflowTransformSelection&Sel,const FBox&B){FString Bond=A->GetStringField(TEXT("bond"));EFractureBrickBondEnum BE=Bond==TEXT("stack")?EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stack:Bond==TEXT("english")?EFractureBrickBondEnum::Dataflow_FractureBrickBond_English:Bond==TEXT("header")?EFractureBrickBondEnum::Dataflow_FractureBrickBond_Header:Bond==TEXT("flemish")?EFractureBrickBondEnum::Dataflow_FractureBrickBond_Flemish:EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stretcher;float N,F,P,L,PS;int32 Oct;Noise(A,N,F,P,L,Oct,PS);return FFractureEngineFracturing::BrickCutter(G,Sel,B,FTransform::Identity,BE,(float)Number(A,TEXT("brick_length"),100),(float)Number(A,TEXT("brick_height"),50),(float)Number(A,TEXT("brick_depth"),50),(int32)Number(A,TEXT("seed"),0),(float)Number(A,TEXT("chance"),1),IslandSettings(A),(float)Number(A,TEXT("grout"),1),N,F,P,L,Oct,PS,true,(float)Number(A,TEXT("collision_sample_spacing"),50));},O,S,E);},nullptr,2});

	Registry.Register({TEXT("fracture_mesh_cutter_apply"),TEXT("Cut a Geometry Collection with a Static Mesh through the native Chaos mesh cutter."),CommonCutSchema({
		{TEXT("cutter_mesh"),FSololmcpSchemaBuilder::String(TEXT("Static Mesh cutter path under /Game/."),{},1,1024)},{TEXT("lod_index"),FSololmcpSchemaBuilder::Integer(TEXT("Source mesh LOD."),0,16)},
		{TEXT("distribution"),FSololmcpSchemaBuilder::String(TEXT("Cutter distribution."),{TEXT("single"),TEXT("uniform_random"),TEXT("grid")})},{TEXT("count"),FSololmcpSchemaBuilder::Integer(TEXT("Random cutter count."),1,4096)},
		{TEXT("grid_x"),FSololmcpSchemaBuilder::Integer(TEXT("Grid X."),1,128)},{TEXT("grid_y"),FSololmcpSchemaBuilder::Integer(TEXT("Grid Y."),1,128)},{TEXT("grid_z"),FSololmcpSchemaBuilder::Integer(TEXT("Grid Z."),1,128)}},{TEXT("cutter_mesh")}),
		[](const FSololmcpToolExecutionContext&C,const TSharedRef<FJsonObject>&A,TSharedRef<FJsonObject>&O,FString&S,FString&E){FString MP=A->GetStringField(TEXT("cutter_mesh"));if(!MP.StartsWith(TEXT("/Game/"))){Fail(O,E,TEXT("invalid_cutter_mesh_path"),TEXT("cutter_mesh must be under /Game/."));return false;}UStaticMesh*M=Cast<UStaticMesh>(C.Services.LoadAsset(MP,E));if(!M){Fail(O,E,TEXT("cutter_mesh_not_found"),E.IsEmpty()?TEXT("Static Mesh cutter not found."):E);return false;}int32 LOD=(int32)Number(A,TEXT("lod_index"),0);FMeshDescription*MD=M->GetMeshDescription(LOD);if(!MD||MD->Triangles().Num()==0){Fail(O,E,TEXT("cutter_mesh_has_no_mesh_description"),TEXT("Requested cutter LOD has no editable MeshDescription."));return false;}UE::Geometry::FDynamicMesh3 Dynamic;FMeshDescriptionToDynamicMesh Converter;Converter.bTransformVertexColorsLinearToSRGB=false;Converter.bVIDsFromNonManifoldMeshDescriptionAttr=true;Converter.Convert(MD,Dynamic,true);if(Dynamic.TriangleCount()==0){Fail(O,E,TEXT("cutter_mesh_conversion_failed"),TEXT("Static Mesh conversion produced no cutter triangles."));return false;}return ApplyPattern(C,A,TEXT("native_mesh_cutter"),[A,&Dynamic](FGeometryCollection&G,FDataflowTransformSelection&Sel,const FBox&B){TArray<FTransform>T;FString D;A->TryGetStringField(TEXT("distribution"),D);EMeshCutterCutDistribution Dist=D==TEXT("grid")?EMeshCutterCutDistribution::Grid:D==TEXT("uniform_random")?EMeshCutterCutDistribution::UniformRandom:EMeshCutterCutDistribution::SingleCut;FFractureEngineFracturing::GenerateMeshTransforms(T,B,(int32)Number(A,TEXT("seed"),0),Dist,(int32)Number(A,TEXT("count"),1),(int32)Number(A,TEXT("grid_x"),1),(int32)Number(A,TEXT("grid_y"),1),(int32)Number(A,TEXT("grid_z"),1),0,1,1,false,0,0,0);return FFractureEngineFracturing::MeshCutter(T,G,Sel,Dynamic,(int32)Number(A,TEXT("seed"),0),(float)Number(A,TEXT("chance"),1),IslandSettings(A),(float)Number(A,TEXT("collision_sample_spacing"),50));},O,S,E);},nullptr,2});
#else
	(void)Registry;
#endif
}
}
