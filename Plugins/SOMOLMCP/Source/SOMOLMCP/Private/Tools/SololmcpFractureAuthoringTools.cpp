// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "SOMOLMCP.h"

#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionEngineConversion.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "Dataflow/DataflowSelection.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "FractureEngineClustering.h"
#include "FractureEngineFracturing.h"
#include "FractureEngineMaterials.h"
#include "FractureEngineSelection.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollectionSimulationTypes.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#endif

namespace UE::SOMOLMCP
{
namespace FractureAuthoring
{
	// DynamicMesh3.h is only included in the UE 5.8 block above, and every
	// FDynamicMesh3 use sits inside the 5.8-only implementation below, so the
	// using-declaration has to carry the same guard. UE 5.6/5.7 happened to pull
	// UE::Geometry::FDynamicMesh3 in transitively via the GeometryCollection
	// headers and hid this; 5.3-5.5 do not, and the whole file failed to compile.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	using UE::Geometry::FDynamicMesh3;
#endif

	static void Fail(TSharedRef<FJsonObject>& Out, FString& OutError, const FString& Code,
		const FString& Message, const FString& Status = TEXT("failed"))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("status"), Status);
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("reason_code"), Code);
		Out->SetStringField(TEXT("message"), Message);
		OutError = Message;
	}

	static TSharedRef<FJsonObject> AssetPathSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{{TEXT("asset_path"), FSololmcpSchemaBuilder::String(
				TEXT("Geometry Collection object or package path under /Game/."), {}, 1, 1024)}},
			{TEXT("asset_path")}, FString(), false);
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	static UGeometryCollection* LoadCollection(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& OutError)
	{
		FString Path;
		if (!Args->TryGetStringField(TEXT("asset_path"), Path) || !Path.StartsWith(TEXT("/Game/")))
		{
			Fail(Out, OutError, TEXT("invalid_asset_path"),
				TEXT("asset_path must be a non-empty /Game/ Geometry Collection path."));
			return nullptr;
		}

		UGeometryCollection* Asset = Cast<UGeometryCollection>(Context.Services.LoadAsset(Path, OutError));
		if (!Asset)
		{
			const FString Message = OutError.IsEmpty()
				? FString::Printf(TEXT("Asset '%s' is not a Geometry Collection."), *Path) : OutError;
			Fail(Out, OutError, TEXT("geometry_collection_not_found"), Message);
			return nullptr;
		}

		const TSharedPtr<FGeometryCollection> Collection = Asset->GetGeometryCollection();
		if (!Collection.IsValid() || Collection->NumElements(FGeometryCollection::TransformGroup) <= 0 ||
			Collection->NumElements(FGeometryCollection::GeometryGroup) <= 0)
		{
			Fail(Out, OutError, TEXT("geometry_collection_has_no_source_geometry"),
				TEXT("The Geometry Collection contains no fractureable transform/geometry data. Create it from a valid source mesh first."));
			return nullptr;
		}
		return Asset;
	}

	static void WriteCounts(const FGeometryCollection& Collection, TSharedRef<FJsonObject>& Out)
	{
		Out->SetNumberField(TEXT("transform_count"), Collection.NumElements(FGeometryCollection::TransformGroup));
		Out->SetNumberField(TEXT("geometry_count"), Collection.NumElements(FGeometryCollection::GeometryGroup));
		Out->SetNumberField(TEXT("vertex_count"), Collection.NumElements(FGeometryCollection::VerticesGroup));
		Out->SetNumberField(TEXT("face_count"), Collection.NumElements(FGeometryCollection::FacesGroup));
	}

	static void DetachBoundComponentRenderState(UGeometryCollection& Asset)
	{
		// The asset-only authoring path never owns a component, so deferred dirty marks
		// from PropagateMarkDirtyToComponents only rebuild bound scene proxies on a
		// later render-thread scene update. A proxy that still carries the pre-mutation
		// transform count uploads the grown transform array when the render thread
		// processes that component's dynamic-data mark and trips the engine's
		// NumTransforms == Transforms.Num() assert. Detaching every bound component
		// (and every unbound one left behind by a thumbnail/preview scene) from the
		// scene before the in-place mutation removes the proxy from all render-thread
		// update paths: later MarkRenderDynamicDataDirty calls only set a flag that is
		// ignored for components whose render state no longer exists.
		//
		// Run on the game thread with a FlushRenderingCommands around the loop so the
		// render thread is idle before the mutation starts.
		FlushRenderingCommands();
		for (TObjectIterator<UGeometryCollectionComponent> It; It; ++It)
		{
			const UGeometryCollection* Bound = It->GetRestCollection();
			if (Bound != &Asset && Bound != nullptr) continue;
			const FString BoundPath = Bound ? Bound->GetPathName() : TEXT("(null)");
			const FString WorldName = It->GetWorld() ? It->GetWorld()->GetName() : TEXT("(no-world)");
			UE_LOG(LogSOMOLMCP, Warning, TEXT("DetachBound: %s bound=%s registered=%d renderState=%d world=%s"),
				*It->GetPathName(), *BoundPath, It->IsRegistered() ? 1 : 0,
				It->IsRenderStateCreated() ? 1 : 0, *WorldName);
			if (It->IsRenderStateCreated())
			{
				It->DestroyRenderState_Concurrent();
			}
		}
		FlushRenderingCommands();
	}

	static void RecreateBoundComponentRenderState(UGeometryCollection& Asset)
	{
		// After the in-place mutation, re-create the render states of the same
		// components. The next render-thread scene update builds proxies whose cached
		// transform counts match the mutated collection. No MarkRenderDynamicDataDirty
		// is issued here: new proxies initialize their transform buffers from the
		// mutated collection when they are created, and a dynamic-data mark against a
		// component that is not in the scene can never reach a stale proxy.
		for (TObjectIterator<UGeometryCollectionComponent> It; It; ++It)
		{
			const UGeometryCollection* Bound = It->GetRestCollection();
			if (Bound != &Asset && Bound != nullptr) continue;
			const FString BoundPath = Bound ? Bound->GetPathName() : TEXT("(null)");
			const FString WorldName = It->GetWorld() ? It->GetWorld()->GetName() : TEXT("(no-world)");
			UE_LOG(LogSOMOLMCP, Warning, TEXT("RecreateBound: %s bound=%s registered=%d renderState=%d world=%s"),
				*It->GetPathName(), *BoundPath, It->IsRegistered() ? 1 : 0,
				It->IsRenderStateCreated() ? 1 : 0, *WorldName);
			if (It->IsRegistered())
			{
				// The proxy caches its transform count from the asset at construction
				// time, but InitDynamicData() uploads the component's internal
				// ComponentSpaceTransforms buffer. After the in-place mutation that
				// buffer is stale, so the render thread would assert
				// (NumTransforms == Transforms.Num()) while the new proxy initializes
				// its transform buffer. SetRestCollection() resizes the component's
				// internal buffers to match the mutated collection (via
				// ResetDynamicCollection) - the same sync the engine's
				// FGeometryCollectionEdit performs after an edit.
				It->SetRestCollection(&Asset, false);
			}
			if (It->IsRegistered() && !It->IsRenderStateCreated())
			{
				It->CreateRenderState_Concurrent(nullptr);
			}
		}
		FlushRenderingCommands();
	}

	static bool RebuildAndSave(const FSololmcpToolExecutionContext& Context, UGeometryCollection& Asset,
		TSharedRef<FJsonObject>& Out, FString& OutError)
	{
		UE_LOG(LogSOMOLMCP, Warning, TEXT("RebuildAndSave: begin %s"), *Asset.GetPathName());
		Asset.InvalidateCollection();
		Asset.CreateSimulationData();
		Asset.RebuildRenderData();
		RecreateBoundComponentRenderState(Asset);
		Asset.MarkPackageDirty();
		UE_LOG(LogSOMOLMCP, Warning, TEXT("RebuildAndSave: rebuilt, saving"));

		FString SaveError;
		if (!Context.Services.SaveAsset(Asset.GetPathName(), false, SaveError))
		{
			Fail(Out, OutError, TEXT("fracture_asset_save_failed"),
				SaveError.IsEmpty() ? TEXT("Geometry Collection save failed.") : SaveError);
			return false;
		}
		if (!Context.Services.AssetExists(Asset.GetPathName()))
		{
			Fail(Out, OutError, TEXT("fracture_asset_readback_failed"),
				TEXT("The saved Geometry Collection could not be read back from the asset registry."));
			return false;
		}
		return true;
	}

	static bool ApplyUniformVoronoi(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args, const int32 Sites, const int32 Seed,
		const float Grout, const float NoiseAmplitude, const bool bSplitIslands,
		TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
		if (!Asset) return false;
		TSharedPtr<FGeometryCollection> Collection = Asset->GetGeometryCollection();
		const int32 BeforeGeometry = Collection->NumElements(FGeometryCollection::GeometryGroup);
		const int32 BeforeTransforms = Collection->NumElements(FGeometryCollection::TransformGroup);
		FDataflowTransformSelection Selection;
		Selection.InitializeFromCollection(*Collection, false);
		FFractureEngineSelection::SelectLeaf(*Collection, Selection);
		if (!Selection.AnySelected())
		{
			Fail(Out, Error, TEXT("fracture_selection_empty"), TEXT("No leaf geometry was available for fracture."));
			return false;
		}

		FUniformFractureSettings Settings;
		Settings.Transform = FTransform::Identity;
		Settings.MinVoronoiSites = Sites;
		Settings.MaxVoronoiSites = Sites;
		Settings.InternalMaterialID = INDEX_NONE;
		Settings.RandomSeed = Seed;
		Settings.ChanceToFracture = 1.0f;
		Settings.GroupFracture = false;
		Settings.SplitIslands = bSplitIslands;
		Settings.CloseVertexDistance = 1e-3;
		Settings.VertexToSurfaceBridgeDistance = 0.0;
		Settings.Grout = Grout;
		Settings.NoiseSettings.Amplitude = NoiseAmplitude;
		Settings.NoiseSettings.Frequency = 0.1f;
		Settings.NoiseSettings.Persistence = 0.5f;
		Settings.NoiseSettings.Lacunarity = 2.0f;
		Settings.NoiseSettings.Octaves = NoiseAmplitude > 0.0f ? 3 : 0;
		Settings.NoiseSettings.PointSpacing = 10.0f;
		Settings.AddSamplesForCollision = true;
		Settings.CollisionSampleSpacing = 50.0f;

		// Drain in-flight render work and detach every bound/unbound Geometry Collection
		// component before mutating the collection in place. Without this, the render
		// thread can observe the grown transform array through a proxy that still
		// carries the pre-fracture transform count and crash in
		// FGeometryCollectionTransformBuffer::UpdateDynamicData.
		UE_LOG(LogSOMOLMCP, Warning, TEXT("Fracture: pre-flush before=%d transforms=%d geometry=%d"),
			BeforeTransforms, BeforeGeometry, Collection->NumElements(FGeometryCollection::VerticesGroup));
		DetachBoundComponentRenderState(*Asset);
		UE_LOG(LogSOMOLMCP, Warning, TEXT("Fracture: pre-flush done"));

		FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureVoronoiApply", "SOMOLMCP Voronoi Fracture"));
		Asset->Modify();
		const int32 FirstNewGeometry = FFractureEngineFracturing::UniformFracture(*Collection, Selection, Settings);
		const int32 AfterGeometry = Collection->NumElements(FGeometryCollection::GeometryGroup);
		const int32 AfterTransforms = Collection->NumElements(FGeometryCollection::TransformGroup);
		if (FirstNewGeometry == INDEX_NONE || AfterGeometry <= BeforeGeometry || AfterTransforms <= BeforeTransforms)
		{
			Transaction.Cancel();
			Fail(Out, Error, TEXT("fracture_produced_no_geometry"),
				TEXT("Chaos Fracture returned without creating new geometry. The asset was not accepted as completed."));
			Out->SetNumberField(TEXT("before_geometry_count"), BeforeGeometry);
			Out->SetNumberField(TEXT("after_geometry_count"), AfterGeometry);
			return false;
		}
		if (!RebuildAndSave(Context, *Asset, Out, Error)) return false;

		const FString ReceiptId = FString::Printf(TEXT("fracture_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower());
		UE_LOG(LogSOMOLMCP, Warning, TEXT("Fracture: succeeded before=%d after=%d transforms=%d"),
			BeforeGeometry, AfterGeometry, AfterTransforms);
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("receipt_id"), ReceiptId);
		Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
		Out->SetStringField(TEXT("operation"), TEXT("native_uniform_voronoi_fracture"));
		Out->SetBoolField(TEXT("mutation_applied"), true);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetBoolField(TEXT("simulation_data_rebuilt"), true);
		Out->SetBoolField(TEXT("render_data_rebuilt"), true);
		Out->SetNumberField(TEXT("first_new_geometry_index"), FirstNewGeometry);
		Out->SetNumberField(TEXT("before_geometry_count"), BeforeGeometry);
		Out->SetNumberField(TEXT("after_geometry_count"), AfterGeometry);
		Out->SetNumberField(TEXT("pieces_added"), AfterGeometry - BeforeGeometry);
		Out->SetNumberField(TEXT("site_count"), Sites);
		Out->SetNumberField(TEXT("seed"), Seed);
		WriteCounts(*Collection, Out);
		Summary = FString::Printf(TEXT("Native Voronoi fracture created %d pieces in %s."),
			AfterGeometry - BeforeGeometry, *Asset->GetPathName());
		return true;
	}

	// ===== A2: multi-pattern fracture parameter family (CH-05..CH-10) =====

	struct FFractureParams
	{
		FString Pattern = TEXT("voronoi");
		int32 Sites = 10;
		int32 Seed = 1337;
		float Grout = 0.0f;
		float NoiseAmplitude = 0.0f;
		float NoiseFrequency = 0.1f;
		float NoisePersistence = 0.5f;
		float NoiseLacunarity = 2.0f;
		int32 NoiseOctaves = 0;
		float PointSpacing = 10.0f;
		bool bAddSamplesForCollision = true;
		float CollisionSampleSpacing = 50.0f;
		bool bSplitIslands = true;
		int32 NumPlanes = 2;
		int32 SlicesX = 1, SlicesY = 1, SlicesZ = 1;
		float SliceAngleVariation = 15.0f;
		float SliceOffsetVariation = 10.0f;
		float Radius = 300.0f;
		EFractureBrickBondEnum BrickBond = EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stretcher;
		float BrickLength = 200.0f, BrickHeight = 60.0f, BrickDepth = 100.0f;
		EMeshCutterCutDistribution CutDistribution = EMeshCutterCutDistribution::UniformRandom;
		int32 CutterCount = 1, CutterGridX = 1, CutterGridY = 1, CutterGridZ = 1;
		float CutterVariability = 0.0f, CutterMinScale = 0.5f, CutterMaxScale = 1.0f;
		bool bRandomOrientation = false;
		float RollRange = 0.0f, PitchRange = 0.0f, YawRange = 0.0f;
		float CutterBoxSize = 150.0f;
		TArray<FVector> CustomSites;
	};

	static UClass* ResolveNativeClassRef(const TCHAR* Path)
	{
		if (UClass* Existing = FindObject<UClass>(nullptr, Path)) return Existing;
		return LoadObject<UClass>(nullptr, Path);
	}

	static FString ExportPropValue(UObject* Object, const FName Name)
	{
		if (!Object) return FString();
		if (FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name))
		{
			FString Value;
			Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
			return Value;
		}
		return FString();
	}

	static bool SetEnumPropValue(UObject* Object, const FName Name, const FString& Requested, FString& Error)
	{
		FProperty* Property = Object ? FindFProperty<FProperty>(Object->GetClass(), Name) : nullptr;
		UEnum* Enum = nullptr;
		FNumericProperty* Underlying = nullptr;
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
			Underlying = EnumProperty->GetUnderlyingProperty();
		}
		else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			Enum = ByteProperty->Enum;
			Underlying = ByteProperty;
		}
		if (!Enum || !Underlying)
		{
			Error = FString::Printf(TEXT("Enum property '%s' is unavailable on %s."), *Name.ToString(),
				Object ? *Object->GetClass()->GetPathName() : TEXT("null"));
			return false;
		}
		int64 Value = Enum->GetValueByNameString(Requested, EGetByNameFlags::None);
		if (Value == INDEX_NONE)
		{
			Value = Enum->GetValueByName(FName(*Requested), EGetByNameFlags::None);
		}
		if (Value == INDEX_NONE)
		{
			Error = FString::Printf(TEXT("'%s' is not valid for enum %s."), *Requested, *Enum->GetPathName());
			return false;
		}
		Underlying->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Object), Value);
		return true;
	}

	static bool SetBoolPropValue(UObject* Object, const FName Name, bool Value, FString& Error)
	{
		FBoolProperty* Property = Object ? FindFProperty<FBoolProperty>(Object->GetClass(), Name) : nullptr;
		if (!Property)
		{
			Error = FString::Printf(TEXT("Boolean property '%s' is unavailable."), *Name.ToString());
			return false;
		}
		Property->SetPropertyValue_InContainer(Object, Value);
		return true;
	}

	static bool SetFloatPropValue(UObject* Object, const FName Name, double Value, FString& Error)
	{
		FNumericProperty* Property = Object ? FindFProperty<FNumericProperty>(Object->GetClass(), Name) : nullptr;
		if (!Property || !Property->IsFloatingPoint())
		{
			Error = FString::Printf(TEXT("Floating-point property '%s' is unavailable."), *Name.ToString());
			return false;
		}
		Property->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Object), Value);
		return true;
	}

	static bool SetObjectPropValue(UObject* Object, const FName Name, UObject* Value, FString& Error)
	{
		FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
		if (!Property || (Value && !Value->IsA(Property->PropertyClass)))
		{
			Error = FString::Printf(TEXT("Object property '%s' is unavailable or rejects %s."), *Name.ToString(),
				Value ? *Value->GetClass()->GetPathName() : TEXT("null"));
			return false;
		}
		Property->SetObjectPropertyValue_InContainer(Object, Value);
		return true;
	}

	static bool SaveReflectAndReadback(const FSololmcpToolExecutionContext& Context, UObject* Asset,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		if (!Asset || !Asset->GetOutermost() || !Asset->GetOutermost()->GetName().StartsWith(TEXT("/Game/")))
		{
			Fail(Out, Error, TEXT("non_persistent_asset"), TEXT("Mutation target is not a persistent /Game asset."));
			return false;
		}
		Asset->MarkPackageDirty();
		FString SaveError;
		if (!Context.Services.SaveAsset(Asset->GetPathName(), false, SaveError))
		{
			Fail(Out, Error, TEXT("asset_save_failed"), SaveError.IsEmpty() ? TEXT("Asset save failed.") : SaveError);
			return false;
		}
		FString ReadbackError;
		UObject* Readback = Context.Services.LoadAsset(Asset->GetPathName(), ReadbackError);
		if (!Readback || Readback->GetClass() != Asset->GetClass())
		{
			Fail(Out, Error, TEXT("asset_readback_failed"), ReadbackError.IsEmpty()
				? TEXT("Saved asset could not be read back with the expected class.") : ReadbackError);
			return false;
		}
		Out->SetBoolField(TEXT("saved"), true);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetStringField(TEXT("asset_path"), Readback->GetPathName());
		Out->SetStringField(TEXT("asset_class"), Readback->GetClass()->GetPathName());
		return true;
	}

	static FBox ComputeCollectionBoundingBox(const FGeometryCollection& Collection)
	{
		FBox Box(ForceInit);
		if (const TManagedArray<FBox>* Boxes = Collection.FindAttribute<FBox>(TEXT("BoundingBox"), FGeometryCollection::TransformGroup))
		{
			for (int32 Index = 0; Index < Boxes->Num(); ++Index)
			{
				const FBox& B = (*Boxes)[Index];
				if (B.IsValid) Box += B;
			}
		}
		if (!Box.IsValid)
		{
			Box = FBox(FVector(-100.0f), FVector(100.0f));
		}
		return Box;
	}

	static int32 CountClusters(const FGeometryCollection& Collection)
	{
		if (const TManagedArray<int32>* Levels = Collection.FindAttribute<int32>(TEXT("Level"), FGeometryCollection::TransformGroup))
		{
			int32 Count = 0;
			for (int32 Index = 0; Index < Levels->Num(); ++Index)
			{
				if ((*Levels)[Index] > 0) ++Count;
			}
			return Count;
		}
		return 0;
	}

	static bool BuildTransformSelection(const TSharedRef<FJsonObject>& Args,
		FGeometryCollection& Collection, FDataflowTransformSelection& Selection, FString& Error)
	{
		Selection.InitializeFromCollection(Collection, false);
		const int32 TransformCount = Collection.NumElements(FGeometryCollection::TransformGroup);
		if (Args->HasField(TEXT("selected_bones")))
		{
			const TArray<TSharedPtr<FJsonValue>>& BoneValues = Args->GetArrayField(TEXT("selected_bones"));
			TArray<int32> Indices;
			for (const TSharedPtr<FJsonValue>& Value : BoneValues)
			{
				const int32 Index = static_cast<int32>(Value->AsNumber());
				if (Index < 0 || Index >= TransformCount)
				{
					Error = FString::Printf(TEXT("selected_bones index %d is out of range (transform_count=%d)."),
						Index, TransformCount);
					return false;
				}
				Indices.Add(Index);
			}
			if (Indices.IsEmpty())
			{
				Error = TEXT("selected_bones was provided but is empty; remove it to fracture all leaves.");
				return false;
			}
			if (!Selection.SetFromArray(Indices))
			{
				Error = TEXT("selected_bones contains indices that are not valid for the collection.");
				return false;
			}
		}
		else
		{
			FFractureEngineSelection::SelectLeaf(Collection, Selection);
		}
		if (!Selection.AnySelected())
		{
			Error = TEXT("No selected leaf geometry was available for fracture.");
			return false;
		}
		return true;
	}

	static FDynamicMesh3 BuildCutBoxMesh(const float Size)
	{
		FDynamicMesh3 Mesh;
		const FVector E(Size * 0.5f, Size * 0.5f, Size * 0.5f);
		const int32 V0 = Mesh.AppendVertex(FVector3d(-E.X, -E.Y, -E.Z));
		const int32 V1 = Mesh.AppendVertex(FVector3d(E.X, -E.Y, -E.Z));
		const int32 V2 = Mesh.AppendVertex(FVector3d(E.X, E.Y, -E.Z));
		const int32 V3 = Mesh.AppendVertex(FVector3d(-E.X, E.Y, -E.Z));
		const int32 V4 = Mesh.AppendVertex(FVector3d(-E.X, -E.Y, E.Z));
		const int32 V5 = Mesh.AppendVertex(FVector3d(E.X, -E.Y, E.Z));
		const int32 V6 = Mesh.AppendVertex(FVector3d(E.X, E.Y, E.Z));
		const int32 V7 = Mesh.AppendVertex(FVector3d(-E.X, E.Y, E.Z));
		Mesh.AppendTriangle(V0, V2, V1); Mesh.AppendTriangle(V0, V3, V2);
		Mesh.AppendTriangle(V4, V5, V6); Mesh.AppendTriangle(V4, V6, V7);
		Mesh.AppendTriangle(V0, V1, V5); Mesh.AppendTriangle(V0, V5, V4);
		Mesh.AppendTriangle(V3, V6, V2); Mesh.AppendTriangle(V3, V7, V6);
		Mesh.AppendTriangle(V0, V4, V7); Mesh.AppendTriangle(V0, V7, V3);
		Mesh.AppendTriangle(V1, V2, V6); Mesh.AppendTriangle(V1, V6, V5);
		return Mesh;
	}

	static bool ResolveFractureParams(const TSharedRef<FJsonObject>& Args, FFractureParams& Params,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		if (Args->HasField(TEXT("pattern")))
		{
			const FString Pattern = Args->GetStringField(TEXT("pattern"));
			const TArray<FString> Allowed = {TEXT("voronoi"), TEXT("radial"), TEXT("plane"), TEXT("slice"),
				TEXT("brick"), TEXT("cutter"), TEXT("custom_sites")};
			if (!Allowed.Contains(Pattern))
			{
				Fail(Out, Error, TEXT("invalid_fracture_pattern"), FString::Printf(TEXT(
					"pattern must be one of: %s"), *FString::Join(Allowed, TEXT(", "))));
				return false;
			}
			Params.Pattern = Pattern;
		}
		if (Args->HasField(TEXT("num_sites")))
		{
			Params.Sites = FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("num_sites"))), 2, 5000);
		}
		if (Args->HasField(TEXT("seed")))
		{
			Params.Seed = FMath::Max(0, static_cast<int32>(Args->GetNumberField(TEXT("seed"))));
		}
		if (Args->HasField(TEXT("grout_cm")))
		{
			Params.Grout = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("grout_cm"))), 0.0f, 100.0f);
		}
		if (Args->HasField(TEXT("noise_amplitude")))
		{
			Params.NoiseAmplitude = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("noise_amplitude"))), 0.0f, 1000.0f);
		}
		if (Args->HasField(TEXT("noise_frequency")))
		{
			Params.NoiseFrequency = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("noise_frequency"))), 0.001f, 100.0f);
		}
		if (Args->HasField(TEXT("noise_persistence")))
		{
			Params.NoisePersistence = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("noise_persistence"))), 0.0f, 1.0f);
		}
		if (Args->HasField(TEXT("noise_lacunarity")))
		{
			Params.NoiseLacunarity = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("noise_lacunarity"))), 1.0f, 8.0f);
		}
		if (Args->HasField(TEXT("noise_octaves")))
		{
			Params.NoiseOctaves = FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("noise_octaves"))), 0, 8);
		}
		if (Args->HasField(TEXT("point_spacing_cm")))
		{
			Params.PointSpacing = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("point_spacing_cm"))), 0.1f, 10000.0f);
		}
		if (Args->HasField(TEXT("add_collision_samples")))
		{
			Params.bAddSamplesForCollision = Args->GetBoolField(TEXT("add_collision_samples"));
		}
		if (Args->HasField(TEXT("collision_sample_spacing_cm")))
		{
			Params.CollisionSampleSpacing = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("collision_sample_spacing_cm"))), 0.1f, 10000.0f);
		}
		if (Args->HasField(TEXT("split_islands")))
		{
			Params.bSplitIslands = Args->GetBoolField(TEXT("split_islands"));
		}
		if (Args->HasField(TEXT("num_planes")))
		{
			Params.NumPlanes = FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("num_planes"))), 1, 128);
		}
		if (Args->HasField(TEXT("slices")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Slices = Args->GetArrayField(TEXT("slices"));
			if (Slices.Num() >= 1) Params.SlicesX = FMath::Clamp(static_cast<int32>(Slices[0]->AsNumber()), 0, 32);
			if (Slices.Num() >= 2) Params.SlicesY = FMath::Clamp(static_cast<int32>(Slices[1]->AsNumber()), 0, 32);
			if (Slices.Num() >= 3) Params.SlicesZ = FMath::Clamp(static_cast<int32>(Slices[2]->AsNumber()), 0, 32);
		}
		if (Args->HasField(TEXT("slice_angle_variation")))
		{
			Params.SliceAngleVariation = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("slice_angle_variation"))), 0.0f, 180.0f);
		}
		if (Args->HasField(TEXT("slice_offset_variation")))
		{
			Params.SliceOffsetVariation = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("slice_offset_variation"))), 0.0f, 1000.0f);
		}
		if (Args->HasField(TEXT("radius_cm")))
		{
			Params.Radius = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("radius_cm"))), 10.0f, 100000.0f);
		}
		if (Args->HasField(TEXT("brick_bond")))
		{
			const FString Bond = Args->GetStringField(TEXT("brick_bond"));
			if (Bond == TEXT("stretcher")) Params.BrickBond = EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stretcher;
			else if (Bond == TEXT("stack")) Params.BrickBond = EFractureBrickBondEnum::Dataflow_FractureBrickBond_Stack;
			else if (Bond == TEXT("english")) Params.BrickBond = EFractureBrickBondEnum::Dataflow_FractureBrickBond_English;
			else if (Bond == TEXT("header")) Params.BrickBond = EFractureBrickBondEnum::Dataflow_FractureBrickBond_Header;
			else if (Bond == TEXT("flemish")) Params.BrickBond = EFractureBrickBondEnum::Dataflow_FractureBrickBond_Flemish;
			else { Fail(Out, Error, TEXT("invalid_brick_bond"), TEXT("brick_bond must be stretcher, stack, english, header, or flemish.")); return false; }
		}
		if (Args->HasField(TEXT("brick_length")))
		{
			Params.BrickLength = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("brick_length"))), 5.0f, 10000.0f);
		}
		if (Args->HasField(TEXT("brick_height")))
		{
			Params.BrickHeight = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("brick_height"))), 5.0f, 10000.0f);
		}
		if (Args->HasField(TEXT("brick_depth")))
		{
			Params.BrickDepth = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("brick_depth"))), 5.0f, 10000.0f);
		}
		if (Args->HasField(TEXT("cut_distribution")))
		{
			const FString Dist = Args->GetStringField(TEXT("cut_distribution"));
			if (Dist == TEXT("single")) Params.CutDistribution = EMeshCutterCutDistribution::SingleCut;
			else if (Dist == TEXT("uniform_random")) Params.CutDistribution = EMeshCutterCutDistribution::UniformRandom;
			else if (Dist == TEXT("grid")) Params.CutDistribution = EMeshCutterCutDistribution::Grid;
			else { Fail(Out, Error, TEXT("invalid_cut_distribution"), TEXT("cut_distribution must be single, uniform_random, or grid.")); return false; }
		}
		if (Args->HasField(TEXT("cutter_count")))
		{
			Params.CutterCount = FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("cutter_count"))), 1, 64);
		}
		if (Args->HasField(TEXT("cutter_grid")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Grid = Args->GetArrayField(TEXT("cutter_grid"));
			if (Grid.Num() >= 1) Params.CutterGridX = FMath::Clamp(static_cast<int32>(Grid[0]->AsNumber()), 1, 16);
			if (Grid.Num() >= 2) Params.CutterGridY = FMath::Clamp(static_cast<int32>(Grid[1]->AsNumber()), 1, 16);
			if (Grid.Num() >= 3) Params.CutterGridZ = FMath::Clamp(static_cast<int32>(Grid[2]->AsNumber()), 1, 16);
		}
		if (Args->HasField(TEXT("cutter_variability")))
		{
			Params.CutterVariability = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cutter_variability"))), 0.0f, 1.0f);
		}
		if (Args->HasField(TEXT("cutter_min_scale")))
		{
			Params.CutterMinScale = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cutter_min_scale"))), 0.01f, 10.0f);
		}
		if (Args->HasField(TEXT("cutter_max_scale")))
		{
			Params.CutterMaxScale = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cutter_max_scale"))), 0.01f, 10.0f);
		}
		if (Args->HasField(TEXT("cutter_random_orientation")))
		{
			Params.bRandomOrientation = Args->GetBoolField(TEXT("cutter_random_orientation"));
		}
		if (Args->HasField(TEXT("cutter_roll_range")))
		{
			Params.RollRange = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cutter_roll_range"))), 0.0f, 360.0f);
		}
		if (Args->HasField(TEXT("cutter_pitch_range")))
		{
			Params.PitchRange = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cutter_pitch_range"))), 0.0f, 360.0f);
		}
		if (Args->HasField(TEXT("cutter_yaw_range")))
		{
			Params.YawRange = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cutter_yaw_range"))), 0.0f, 360.0f);
		}
		if (Args->HasField(TEXT("cutter_box_size")))
		{
			Params.CutterBoxSize = FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cutter_box_size"))), 5.0f, 10000.0f);
		}
		if (Args->HasField(TEXT("custom_sites")))
		{
			const TArray<TSharedPtr<FJsonValue>>& SiteValues = Args->GetArrayField(TEXT("custom_sites"));
			if (SiteValues.Num() < 2 || SiteValues.Num() > 5000)
			{
				Fail(Out, Error, TEXT("invalid_custom_sites"), TEXT("custom_sites must contain 2-5000 points."));
				return false;
			}
			for (const TSharedPtr<FJsonValue>& SiteValue : SiteValues)
			{
				const TSharedPtr<FJsonObject>& SiteObject = SiteValue->AsObject();
				if (!SiteObject)
				{
					Fail(Out, Error, TEXT("invalid_custom_sites"), TEXT("Each custom_sites entry must be an object with x/y/z fields."));
					return false;
				}
				Params.CustomSites.Add(FVector(
					static_cast<float>(SiteObject->GetNumberField(TEXT("x"))),
					static_cast<float>(SiteObject->GetNumberField(TEXT("y"))),
					static_cast<float>(SiteObject->GetNumberField(TEXT("z")))));
			}
		}
		return true;
	}

	static bool ApplyPatternFracture(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args, UGeometryCollection& Asset, FGeometryCollection& Collection,
		const FDataflowTransformSelection& Selection, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FFractureParams P;
		if (!ResolveFractureParams(Args, P, Out, Error)) return false;
		const int32 BeforeGeometry = Collection.NumElements(FGeometryCollection::GeometryGroup);
		const int32 BeforeTransforms = Collection.NumElements(FGeometryCollection::TransformGroup);
		const FBox Bounds = ComputeCollectionBoundingBox(Collection);
		FIslandSplitSettings IslandSettings;
		IslandSettings.bSplitIslands = P.bSplitIslands;
		IslandSettings.CloseVertexDistance = 1e-3;
		IslandSettings.VertexToSurfaceBridgeDistance = 0.0;
		const FTransform Identity = FTransform::Identity;
		int32 FirstNewGeometry = INDEX_NONE;
		if (P.Pattern == TEXT("voronoi"))
		{
			FUniformFractureSettings Settings;
			Settings.Transform = Identity;
			Settings.MinVoronoiSites = P.Sites;
			Settings.MaxVoronoiSites = P.Sites;
			Settings.InternalMaterialID = INDEX_NONE;
			Settings.RandomSeed = P.Seed;
			Settings.ChanceToFracture = 1.0f;
			Settings.GroupFracture = false;
			Settings.SplitIslands = P.bSplitIslands;
			Settings.CloseVertexDistance = 1e-3;
			Settings.VertexToSurfaceBridgeDistance = 0.0;
			Settings.Grout = P.Grout;
			Settings.NoiseSettings.Amplitude = P.NoiseAmplitude;
			Settings.NoiseSettings.Frequency = P.NoiseFrequency;
			Settings.NoiseSettings.Persistence = P.NoisePersistence;
			Settings.NoiseSettings.Lacunarity = P.NoiseLacunarity;
			Settings.NoiseSettings.Octaves = P.NoiseOctaves;
			Settings.NoiseSettings.PointSpacing = P.PointSpacing;
			Settings.AddSamplesForCollision = P.bAddSamplesForCollision;
			Settings.CollisionSampleSpacing = P.CollisionSampleSpacing;
			FirstNewGeometry = FFractureEngineFracturing::UniformFracture(Collection, Selection, Settings);
		}
		else if (P.Pattern == TEXT("custom_sites"))
		{
			FirstNewGeometry = FFractureEngineFracturing::VoronoiFracture(Collection, Selection, P.CustomSites,
				Identity, P.Seed, 1.0f, IslandSettings, P.Grout, P.NoiseAmplitude, P.NoiseFrequency,
				P.NoisePersistence, P.NoiseLacunarity, P.NoiseOctaves, P.PointSpacing,
				P.bAddSamplesForCollision, P.CollisionSampleSpacing);
		}
		else if (P.Pattern == TEXT("radial") || P.Pattern == TEXT("plane"))
		{
			const int32 NumPlanes = P.Pattern == TEXT("plane") ? 1 : P.NumPlanes;
			TArray<FTransform> CutPlanes;
			FFractureEngineFracturing::GenerateSliceTransforms(Bounds, P.Seed, NumPlanes, CutPlanes);
			FirstNewGeometry = FFractureEngineFracturing::PlaneCutter(Collection, Selection, Bounds, Identity,
				NumPlanes, P.Seed, 1.0f, IslandSettings, P.Grout, P.NoiseAmplitude, P.NoiseFrequency,
				P.NoisePersistence, P.NoiseLacunarity, P.NoiseOctaves, P.PointSpacing,
				P.bAddSamplesForCollision, P.CollisionSampleSpacing, CutPlanes);
		}
		else if (P.Pattern == TEXT("slice"))
		{
			TArray<FTransform> Tmp;
			FFractureEngineFracturing::GenerateSliceTransforms(Tmp, Bounds, P.SlicesX, P.SlicesY, P.SlicesZ,
				P.Seed, P.SliceAngleVariation, P.SliceOffsetVariation);
			FirstNewGeometry = FFractureEngineFracturing::SliceCutter(Collection, Selection, Bounds,
				P.SlicesX, P.SlicesY, P.SlicesZ, P.SliceAngleVariation, P.SliceOffsetVariation, P.Seed, 1.0f,
				IslandSettings, P.Grout, P.NoiseAmplitude, P.NoiseFrequency, P.NoisePersistence,
				P.NoiseLacunarity, P.NoiseOctaves, P.PointSpacing, P.bAddSamplesForCollision, P.CollisionSampleSpacing);
		}
		else if (P.Pattern == TEXT("brick"))
		{
			TArray<FTransform> Tmp;
			TArray<TTuple<FVector, FVector>> Edges;
			FFractureEngineFracturing::GenerateBrickTransforms(Bounds, Tmp, P.BrickBond, P.BrickLength,
				P.BrickHeight, P.BrickDepth, Edges);
			FirstNewGeometry = FFractureEngineFracturing::BrickCutter(Collection, Selection, Bounds, Identity,
				P.BrickBond, P.BrickLength, P.BrickHeight, P.BrickDepth, P.Seed, 1.0f, IslandSettings, P.Grout,
				P.NoiseAmplitude, P.NoiseFrequency, P.NoisePersistence, P.NoiseLacunarity, P.NoiseOctaves,
				P.PointSpacing, P.bAddSamplesForCollision, P.CollisionSampleSpacing);
		}
		else if (P.Pattern == TEXT("cutter"))
		{
			TArray<FTransform> MeshTransforms;
			FFractureEngineFracturing::GenerateMeshTransforms(MeshTransforms, Bounds, P.Seed, P.CutDistribution,
				P.CutterCount, P.CutterGridX, P.CutterGridY, P.CutterGridZ, P.CutterVariability,
				P.CutterMinScale, P.CutterMaxScale, P.bRandomOrientation, P.RollRange, P.PitchRange, P.YawRange);
			FDynamicMesh3 CutBox = BuildCutBoxMesh(P.CutterBoxSize);
			FirstNewGeometry = FFractureEngineFracturing::MeshCutter(MeshTransforms, Collection, Selection, CutBox,
				P.Seed, 1.0f, IslandSettings, P.CollisionSampleSpacing);
		}
		else
		{
			Fail(Out, Error, TEXT("invalid_fracture_pattern"), FString::Printf(TEXT("Unhandled pattern '%s'."), *P.Pattern));
			return false;
		}
		const int32 AfterGeometry = Collection.NumElements(FGeometryCollection::GeometryGroup);
		const int32 AfterTransforms = Collection.NumElements(FGeometryCollection::TransformGroup);
		if (FirstNewGeometry == INDEX_NONE || AfterGeometry <= BeforeGeometry || AfterTransforms <= BeforeTransforms)
		{
			Fail(Out, Error, TEXT("fracture_produced_no_geometry"), FString::Printf(TEXT(
				"Chaos %s fracture returned without creating new geometry. The asset was not accepted as completed."), *P.Pattern));
			Out->SetNumberField(TEXT("before_geometry_count"), BeforeGeometry);
			Out->SetNumberField(TEXT("after_geometry_count"), AfterGeometry);
			return false;
		}
		Out->SetStringField(TEXT("pattern_readback"), P.Pattern);
		Out->SetNumberField(TEXT("first_new_geometry_index"), FirstNewGeometry);
		Out->SetNumberField(TEXT("before_geometry_count"), BeforeGeometry);
		Out->SetNumberField(TEXT("after_geometry_count"), AfterGeometry);
		Out->SetNumberField(TEXT("pieces_added"), AfterGeometry - BeforeGeometry);
		Out->SetNumberField(TEXT("site_count"), P.Sites);
		Out->SetNumberField(TEXT("seed"), P.Seed);
		return true;
	}

	static bool ApplyClusteringConfig(UGeometryCollection& Asset, FGeometryCollection& Collection,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FString Mode;
		Args->TryGetStringField(TEXT("cluster_mode"), Mode);
		if (Mode.IsEmpty() || Mode == TEXT("none"))
		{
			Out->SetStringField(TEXT("cluster_mode_readback"), TEXT("none"));
			return true;
		}
		FDataflowTransformSelection LeafSelection;
		LeafSelection.InitializeFromCollection(Collection, false);
		FFractureEngineSelection::SelectLeaf(Collection, LeafSelection);
		TArray<int32> Bones = LeafSelection.AsArray();
		if (Bones.IsEmpty())
		{
			Fail(Out, Error, TEXT("cluster_selection_empty"), TEXT("No leaf bones were available for clustering."));
			return false;
		}
		const int32 ClustersBefore = CountClusters(Collection);
		if (Mode == TEXT("autocluster"))
		{
			FString MethodStr;
			Args->TryGetStringField(TEXT("cluster_size_method"), MethodStr);
			EFractureEngineClusterSizeMethod Method = EFractureEngineClusterSizeMethod::BySize;
			if (MethodStr == TEXT("bynumber")) Method = EFractureEngineClusterSizeMethod::ByNumber;
			else if (MethodStr == TEXT("byfraction")) Method = EFractureEngineClusterSizeMethod::ByFractionOfInput;
			else if (MethodStr == TEXT("bygrid")) Method = EFractureEngineClusterSizeMethod::ByGrid;
			const uint32 SiteCount = Args->HasField(TEXT("cluster_site_count"))
				? static_cast<uint32>(FMath::Clamp(static_cast<int64>(Args->GetNumberField(TEXT("cluster_site_count"))), 1LL, 10000LL)) : 200u;
			const float SiteSize = Args->HasField(TEXT("cluster_site_size"))
				? FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("cluster_site_size"))), 1.0f, 1000000.0f) : 1000.0f;
			const bool bEnforceConnectivity = !Args->HasField(TEXT("cluster_enforce_connectivity"))
				|| Args->GetBoolField(TEXT("cluster_enforce_connectivity"));
			const bool bAvoidIsolated = !Args->HasField(TEXT("cluster_avoid_isolated"))
				|| Args->GetBoolField(TEXT("cluster_avoid_isolated"));
			const bool bEnforceSiteParameters = Args->HasField(TEXT("cluster_enforce_site_parameters"))
				&& Args->GetBoolField(TEXT("cluster_enforce_site_parameters"));
			int32 GridX = 2, GridY = 2, GridZ = 2;
			if (Args->HasField(TEXT("cluster_grid")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Grid = Args->GetArrayField(TEXT("cluster_grid"));
				if (Grid.Num() >= 1) GridX = FMath::Clamp(static_cast<int32>(Grid[0]->AsNumber()), 1, 16);
				if (Grid.Num() >= 2) GridY = FMath::Clamp(static_cast<int32>(Grid[1]->AsNumber()), 1, 16);
				if (Grid.Num() >= 3) GridZ = FMath::Clamp(static_cast<int32>(Grid[2]->AsNumber()), 1, 16);
			}
			FFractureEngineClustering::AutoCluster(Collection, Bones, Method, SiteCount, 0.1f, SiteSize,
				bEnforceConnectivity, bAvoidIsolated, bEnforceSiteParameters, GridX, GridY, GridZ,
				0.0f, 500, false, 0.0f);
		}
		else if (Mode == TEXT("magnet"))
		{
			const int32 Iterations = Args->HasField(TEXT("cluster_magnet_iterations"))
				? FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("cluster_magnet_iterations"))), 1, 32) : 4;
			if (!FFractureEngineClustering::ClusterMagnet(Collection, Bones, Iterations))
			{
				Fail(Out, Error, TEXT("cluster_magnet_failed"), TEXT("Chaos cluster magnet did not update the collection."));
				return false;
			}
		}
		else if (Mode == TEXT("selected"))
		{
			if (!FFractureEngineClustering::ClusterSelected(Collection, Bones))
			{
				Fail(Out, Error, TEXT("cluster_selected_failed"), TEXT("Chaos ClusterSelected did not update the collection."));
				return false;
			}
		}
		else
		{
			Fail(Out, Error, TEXT("invalid_cluster_mode"), TEXT("cluster_mode must be none, autocluster, magnet, or selected."));
			return false;
		}
		const int32 ClustersAfter = CountClusters(Collection);
		Out->SetStringField(TEXT("cluster_mode_readback"), Mode);
		Out->SetNumberField(TEXT("cluster_selected_bone_count"), Bones.Num());
		Out->SetNumberField(TEXT("cluster_count_before"), ClustersBefore);
		Out->SetNumberField(TEXT("cluster_count_after"), ClustersAfter);
		Out->SetNumberField(TEXT("cluster_count_added"), ClustersAfter - ClustersBefore);
		return true;
	}

	static bool ApplyCollisionDamageConfig(UGeometryCollection& Asset,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		if (Args->HasField(TEXT("collision_type")) || Args->HasField(TEXT("implicit_type")))
		{
			FGeometryCollectionSizeSpecificData& SizeData = Asset.GetDefaultSizeSpecificData();
			if (SizeData.CollisionShapes.IsEmpty())
			{
				SizeData.CollisionShapes.Add(FGeometryCollectionCollisionTypeData());
			}
			FGeometryCollectionCollisionTypeData& CollisionData = SizeData.CollisionShapes[0];
			if (Args->HasField(TEXT("collision_type")))
			{
				const FString Value = Args->GetStringField(TEXT("collision_type"));
				if (Value == TEXT("volumetric")) CollisionData.CollisionType = ECollisionTypeEnum::Chaos_Volumetric;
				else if (Value == TEXT("surface_volumetric")) CollisionData.CollisionType = ECollisionTypeEnum::Chaos_Surface_Volumetric;
				else { Fail(Out, Error, TEXT("invalid_collision_type"), TEXT("collision_type must be volumetric or surface_volumetric.")); return false; }
				Out->SetStringField(TEXT("collision_type_readback"), Value);
			}
			if (Args->HasField(TEXT("implicit_type")))
			{
				const FString Value = Args->GetStringField(TEXT("implicit_type"));
				if (Value == TEXT("box")) CollisionData.ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Box;
				else if (Value == TEXT("sphere")) CollisionData.ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Sphere;
				else if (Value == TEXT("capsule")) CollisionData.ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Capsule;
				else if (Value == TEXT("level_set")) CollisionData.ImplicitType = EImplicitTypeEnum::Chaos_Implicit_LevelSet;
				else if (Value == TEXT("none")) CollisionData.ImplicitType = EImplicitTypeEnum::Chaos_Implicit_None;
				else if (Value == TEXT("convex")) CollisionData.ImplicitType = EImplicitTypeEnum::Chaos_Implicit_Convex;
				else { Fail(Out, Error, TEXT("invalid_implicit_type"), TEXT("implicit_type must be box, sphere, capsule, level_set, none, or convex.")); return false; }
				Out->SetStringField(TEXT("implicit_type_readback"), Value);
			}
		}
		if (Args->HasField(TEXT("damage_thresholds")))
		{
			TArray<float> Thresholds;
			for (const TSharedPtr<FJsonValue>& Value : Args->GetArrayField(TEXT("damage_thresholds")))
			{
				Thresholds.Add(FMath::Max(0.0f, static_cast<float>(Value->AsNumber())));
			}
			Asset.DamageThreshold = Thresholds;
			Asset.bUseSizeSpecificDamageThreshold = false;
			Out->SetArrayField(TEXT("damage_thresholds_readback"),
				[&Thresholds]() { TArray<TSharedPtr<FJsonValue>> Arr; for (float T : Thresholds) Arr.Add(MakeShared<FJsonValueNumber>(T)); return Arr; }());
		}
		if (Args->HasField(TEXT("damage_model")))
		{
			const FString Value = Args->GetStringField(TEXT("damage_model"));
			if (Value == TEXT("user_defined"))
			{
				Asset.bUseSizeSpecificDamageThreshold = false;
				Asset.bUseMaterialDamageModifiers = false;
			}
			else if (Value == TEXT("material_strength"))
			{
				Asset.bUseMaterialDamageModifiers = true;
				Asset.bUseSizeSpecificDamageThreshold = false;
			}
			else { Fail(Out, Error, TEXT("invalid_damage_model"), TEXT("damage_model must be user_defined or material_strength.")); return false; }
			Out->SetStringField(TEXT("damage_model_readback"), Value);
		}
		if (Args->HasField(TEXT("cluster_connection")))
		{
			const FString Value = Args->GetStringField(TEXT("cluster_connection"));
			FString EnumError;
			if (!SetEnumPropValue(&Asset, TEXT("ClusterConnectionType"), Value, EnumError))
			{
				Fail(Out, Error, TEXT("invalid_cluster_connection"), EnumError);
				return false;
			}
			Out->SetStringField(TEXT("cluster_connection_readback"), ExportPropValue(&Asset, TEXT("ClusterConnectionType")));
		}
		if (Args->HasField(TEXT("mass")))
		{
			const double Mass = Args->GetNumberField(TEXT("mass"));
			FString MassError;
			if (!SetFloatPropValue(&Asset, TEXT("Mass"), Mass, MassError))
			{
				Fail(Out, Error, TEXT("mass_property_unavailable"), MassError);
				return false;
			}
			Out->SetNumberField(TEXT("mass_readback"), Mass);
		}
		if (Args->HasField(TEXT("damage_propagation_enabled")))
		{
			Asset.DamagePropagationData.bEnabled = Args->GetBoolField(TEXT("damage_propagation_enabled"));
		}
		return true;
	}

	static bool ApplyMaterialRemovalConfig(const FSololmcpToolExecutionContext& Context,
		UGeometryCollection& Asset, FGeometryCollection& Collection,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		if (Args->HasField(TEXT("internal_material_path")))
		{
			const FString MaterialPath = Args->GetStringField(TEXT("internal_material_path"));
			if (!MaterialPath.StartsWith(TEXT("/Game/")))
			{
				Fail(Out, Error, TEXT("invalid_material_path"), TEXT("internal_material_path must be under /Game/."));
				return false;
			}
			UMaterialInterface* Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, Error));
			if (!Material)
			{
				Fail(Out, Error, TEXT("material_not_found"),
					Error.IsEmpty() ? FString::Printf(TEXT("'%s' is not a Material Interface."), *MaterialPath) : Error);
				return false;
			}
			const int32 MaterialIndex = Asset.Materials.Add(Material);
			FFractureEngineMaterials::SetMaterialOnGeometryAfter(Collection, 0, FFractureEngineMaterials::ETargetFaces::InternalFaces, MaterialIndex);
			Out->SetNumberField(TEXT("internal_material_index"), MaterialIndex);
			Out->SetStringField(TEXT("internal_material_readback"), Material->GetPathName());
		}
		if (Args->HasField(TEXT("remove_on_max_sleep")))
		{
			FString RemovalError;
			if (!SetBoolPropValue(&Asset, TEXT("bRemoveOnMaxSleep"), Args->GetBoolField(TEXT("remove_on_max_sleep")), RemovalError))
			{
				Fail(Out, Error, TEXT("remove_on_max_sleep_unavailable"), RemovalError);
				return false;
			}
			Out->SetStringField(TEXT("remove_on_max_sleep_readback"), ExportPropValue(&Asset, TEXT("bRemoveOnMaxSleep")));
		}
		Out->SetNumberField(TEXT("materials_count"), Asset.Materials.Num());
		return true;
	}

	static bool ApplyCacheHook(const FSololmcpToolExecutionContext& Context, UGeometryCollection& Asset,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FString CachePath;
		if (!Args->TryGetStringField(TEXT("cache_collection_path"), CachePath) || CachePath.IsEmpty())
		{
			Out->SetBoolField(TEXT("cache_hook_applied"), false);
			return true;
		}
		UClass* CollectionClass = ResolveNativeClassRef(TEXT("/Script/ChaosCaching.ChaosCacheCollection"));
		UClass* CacheClass = ResolveNativeClassRef(TEXT("/Script/ChaosCaching.ChaosCache"));
		if (!CollectionClass || !CacheClass)
		{
			Fail(Out, Error, TEXT("chaos_cache_native_api_unavailable"),
				TEXT("ChaosCaching classes are unavailable (plugin not enabled)."), TEXT("blocked"));
			return false;
		}
		UObject* Collection = Context.Services.LoadAsset(CachePath, Error);
		if (!Collection)
		{
			if (!CachePath.StartsWith(TEXT("/Game/")))
			{
				Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("cache_collection_path must be under /Game/."));
				return false;
			}
			const FString PackagePath = FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(CachePath));
			const FString AssetName = FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(CachePath));
			Collection = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/ChaosCaching.ChaosCacheCollection"),
				TEXT("/Script/ChaosCachingEditor.CacheCollectionFactory"), nullptr, Error, false);
			if (!Collection)
			{
				Fail(Out, Error, TEXT("chaos_cache_collection_create_failed"), Error);
				return false;
			}
		}
		if (!Collection->IsA(CollectionClass))
		{
			Fail(Out, Error, TEXT("asset_type_mismatch"), FString::Printf(TEXT(
				"'%s' is not a ChaosCacheCollection."), *Collection->GetPathName()));
			return false;
		}
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Collection->GetClass(), TEXT("Caches"));
		FObjectPropertyBase* Inner = ArrayProperty ? CastField<FObjectPropertyBase>(ArrayProperty->Inner) : nullptr;
		if (!ArrayProperty || !Inner)
		{
			Fail(Out, Error, TEXT("chaos_cache_native_api_unavailable"),
				TEXT("ChaosCacheCollection Caches array is unavailable."), TEXT("blocked"));
			return false;
		}
		const FName CacheName(*FString::Printf(TEXT("cache_%s"), *FPackageName::GetShortName(Asset.GetPathName())));
		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Collection));
		UObject* Cache = nullptr;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			UObject* Candidate = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index));
			if (Candidate && Candidate->GetFName() == CacheName) { Cache = Candidate; break; }
		}
		bool bCreated = false;
		if (!Cache)
		{
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureEnsureChaosCache", "SOMOLMCP Fracture Chaos Cache Hook"));
			Collection->Modify();
			Cache = NewObject<UObject>(Collection, CacheClass, CacheName, RF_Transactional);
			const int32 NewIndex = Helper.AddValue();
			Inner->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), Cache);
			bCreated = true;
		}
		if (!SaveReflectAndReadback(Context, Collection, Out, Error)) return false;
		Out->SetBoolField(TEXT("cache_hook_applied"), true);
		Out->SetBoolField(TEXT("cache_entry_created"), bCreated);
		Out->SetStringField(TEXT("cache_collection_path"), Collection->GetPathName());
		Out->SetStringField(TEXT("cache_entry_name"), Cache->GetName());
		Out->SetNumberField(TEXT("cache_entry_count"), Helper.Num());
		return true;
	}

	static bool InvokeDataflowEvaluateReflect(UObject* Dataflow, UObject* Target, bool& bResult, FString& Error)
	{
		UClass* LibraryClass = ResolveNativeClassRef(TEXT("/Script/DataflowEngine.DataflowBlueprintLibrary"));
		UObject* Library = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
		UFunction* Function = Library ? Library->FindFunction(TEXT("EvaluateDataflow")) : nullptr;
		if (!Function)
		{
			Error = TEXT("UDataflowBlueprintLibrary.EvaluateDataflow is unavailable.");
			return false;
		}
		FStructOnScope Params(Function);
		FBoolProperty* ReturnProperty = nullptr;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
			if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnProperty = CastField<FBoolProperty>(Property);
			}
			else if (Property->GetFName() == TEXT("Dataflow"))
			{
				CastFieldChecked<FObjectPropertyBase>(Property)->SetObjectPropertyValue(
					Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), Dataflow);
			}
			else if (Property->GetFName() == TEXT("AssetToUpdate"))
			{
				CastFieldChecked<FObjectPropertyBase>(Property)->SetObjectPropertyValue(
					Property->ContainerPtrToValuePtr<void>(Params.GetStructMemory()), Target);
			}
		}
		Library->ProcessEvent(Function, Params.GetStructMemory());
		bResult = ReturnProperty && ReturnProperty->GetPropertyValue(
			ReturnProperty->ContainerPtrToValuePtr<void>(Params.GetStructMemory()));
		return true;
	}

	static bool ApplyDataflowHook(const FSololmcpToolExecutionContext& Context, UGeometryCollection& Asset,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FString DataflowPath;
		if (!Args->TryGetStringField(TEXT("dataflow_asset_path"), DataflowPath) || DataflowPath.IsEmpty())
		{
			Out->SetBoolField(TEXT("dataflow_hook_applied"), false);
			return true;
		}
		UClass* DataflowClass = ResolveNativeClassRef(TEXT("/Script/DataflowEngine.Dataflow"));
		if (!DataflowClass)
		{
			Fail(Out, Error, TEXT("dataflow_native_api_unavailable"),
				TEXT("Dataflow classes are unavailable (plugin not enabled)."), TEXT("blocked"));
			return false;
		}
		UObject* Dataflow = Context.Services.LoadAsset(DataflowPath, Error);
		if (!Dataflow)
		{
			if (!DataflowPath.StartsWith(TEXT("/Game/")))
			{
				Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("dataflow_asset_path must be under /Game/."));
				return false;
			}
			const FString PackagePath = FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(DataflowPath));
			const FString AssetName = FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(DataflowPath));
			Dataflow = Context.Services.CreateAsset(PackagePath, AssetName, TEXT("/Script/DataflowEngine.Dataflow"),
				TEXT("/Script/DataflowEditor.DataflowAssetFactory"), nullptr, Error, false);
			if (!Dataflow)
			{
				Fail(Out, Error, TEXT("dataflow_create_failed"), Error);
				return false;
			}
		}
		if (!Dataflow->IsA(DataflowClass))
		{
			Fail(Out, Error, TEXT("asset_type_mismatch"), FString::Printf(TEXT(
				"'%s' is not a Dataflow asset."), *Dataflow->GetPathName()));
			return false;
		}
		TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		int32 NodeCount = 0;
		int32 LinkCount = 0;
		if (UEdGraph* Graph = Cast<UEdGraph>(Dataflow))
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				++NodeCount;
				for (const UEdGraphPin* Pin : Node->Pins) if (Pin) LinkCount += Pin->LinkedTo.Num();
			}
		}
		bool bEvaluateResult = false;
		if (!InvokeDataflowEvaluateReflect(Dataflow, &Asset, bEvaluateResult, Error))
		{
			Fail(Out, Error, TEXT("dataflow_evaluate_failed"), Error);
			return false;
		}
		// A2 fix 2026-08-05: UDataflowBlueprintLibrary::EvaluateDataflow returns
		// bHasError (true = evaluation errors), not a success flag, so compile_ok
		// is the negation. The raw engine return is kept for audit transparency.
		Snapshot->SetStringField(TEXT("asset_path"), Dataflow->GetPathName());
		Snapshot->SetNumberField(TEXT("node_count"), NodeCount);
		Snapshot->SetNumberField(TEXT("connection_count"), LinkCount / 2);
		Snapshot->SetBoolField(TEXT("compile_ok"), !bEvaluateResult);
		Snapshot->SetBoolField(TEXT("evaluate_reported_errors"), bEvaluateResult);
		Out->SetObjectField(TEXT("dataflow_readback"), Snapshot);
		Out->SetBoolField(TEXT("dataflow_hook_applied"), true);
		return true;
	}
#endif
}

void RegisterFractureAuthoringTools(FSololmcpToolRegistry& Registry)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	using namespace FractureAuthoring;

	Registry.Register({TEXT("geometry_collection_create"),
		TEXT("Create a populated Geometry Collection from a Static Mesh, initialize simulation data, save, and verify readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("New Geometry Collection package path under /Game/."), {}, 1, 1024)},
			{TEXT("source_mesh"), FSololmcpSchemaBuilder::String(TEXT("Source Static Mesh object or package path under /Game/."), {}, 1, 1024)},
			{TEXT("split_components"), FSololmcpSchemaBuilder::Boolean(TEXT("Split disconnected mesh components into collection pieces."))},
		}, {TEXT("asset_path"), TEXT("source_mesh")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const FString AssetPath = Args->GetStringField(TEXT("asset_path"));
			const FString SourcePath = Args->GetStringField(TEXT("source_mesh"));
			if (!AssetPath.StartsWith(TEXT("/Game/")) || !SourcePath.StartsWith(TEXT("/Game/")))
			{
				Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("asset_path and source_mesh must both be under /Game/."));
				return false;
			}
			UStaticMesh* SourceMesh = Cast<UStaticMesh>(Context.Services.LoadAsset(SourcePath, Error));
			if (!SourceMesh || !SourceMesh->GetRenderData() || SourceMesh->GetRenderData()->LODResources.IsEmpty())
			{
				Fail(Out, Error, TEXT("source_static_mesh_invalid"),
					Error.IsEmpty() ? TEXT("source_mesh is not a renderable Static Mesh.") : Error);
				return false;
			}
			const FString PackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
			const FString AssetName = FPackageName::GetShortName(PackagePath);
			UPackage* Package = CreatePackage(*PackagePath);
			if (UObject* Existing = StaticFindObject(nullptr, Package, *AssetName))
			{
				Fail(Out, Error, TEXT("asset_already_exists"),
					FString::Printf(TEXT("'%s' already exists as %s; refusing destructive replacement."),
						*AssetPath, *Existing->GetClass()->GetName()));
				return false;
			}

			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CreateGeometryCollection", "SOMOLMCP Create Geometry Collection"));
			UGeometryCollection* Asset = NewObject<UGeometryCollection>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
			if (!Asset)
			{
				Fail(Out, Error, TEXT("geometry_collection_create_failed"), TEXT("Failed to allocate Geometry Collection asset."));
				return false;
			}
			Asset->Modify();
			TArray<UMaterialInterface*> Materials;
			for (const FStaticMaterial& StaticMaterial : SourceMesh->GetStaticMaterials())
			{
				Materials.Add(StaticMaterial.MaterialInterface);
			}
			const bool bSplitComponents = Args->HasField(TEXT("split_components")) && Args->GetBoolField(TEXT("split_components"));
			if (!FGeometryCollectionEngineConversion::AppendStaticMesh(SourceMesh, Materials, FTransform::Identity,
				Asset, true, true, bSplitComponents, false))
			{
				Transaction.Cancel();
				Asset->MarkAsGarbage();
				Fail(Out, Error, TEXT("static_mesh_conversion_failed"), TEXT("UE failed to append source mesh geometry."));
				return false;
			}
			Asset->InitializeMaterials(false);
			GeometryCollectionAlgo::PrepareForSimulation(Asset->GetGeometryCollection().Get());
			FAssetRegistryModule::AssetCreated(Asset);
			if (!RebuildAndSave(Context, *Asset, Out, Error)) return false;
			const TSharedPtr<FGeometryCollection> Collection = Asset->GetGeometryCollection();
			if (!Collection.IsValid() || Collection->NumElements(FGeometryCollection::GeometryGroup) <= 0 ||
				Collection->NumElements(FGeometryCollection::VerticesGroup) <= 0)
			{
				Fail(Out, Error, TEXT("geometry_collection_readback_empty"),
					TEXT("Created asset contains no geometry after save/readback."));
				return false;
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("geometry_collection_create_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
			Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			Out->SetStringField(TEXT("source_mesh"), SourceMesh->GetPathName());
			Out->SetBoolField(TEXT("mutation_applied"), true);
			Out->SetBoolField(TEXT("readback_verified"), true);
			WriteCounts(*Collection, Out);
			Summary = FString::Printf(TEXT("Created populated Geometry Collection %s from %s."),
				*Asset->GetPathName(), *SourceMesh->GetPathName());
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("geometry_collection_fracture"),
		TEXT("Native UE 5.8 Chaos multi-pattern fracture (voronoi/radial/plane/slice/brick/cutter/custom_sites) with bone selection, clustering, collision/damage, internal materials, Chaos Cache and Dataflow hooks, save and readback evidence."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
				{TEXT("pattern"), FSololmcpSchemaBuilder::String(TEXT("Fracture pattern."), {TEXT("voronoi"), TEXT("radial"), TEXT("plane"), TEXT("slice"), TEXT("brick"), TEXT("cutter"), TEXT("custom_sites")})},
				{TEXT("num_sites"), FSololmcpSchemaBuilder::Integer(TEXT("Voronoi/radial site count, 2-5000."), 2, 5000)},
				{TEXT("seed"), FSololmcpSchemaBuilder::Integer(TEXT("Deterministic seed."), 0, MAX_int32)},
				{TEXT("grout_cm"), FSololmcpSchemaBuilder::Number(TEXT("Gap between generated pieces in centimeters."), 0.0, 100.0)},
				{TEXT("noise_amplitude"), FSololmcpSchemaBuilder::Number(TEXT("Internal surface noise amplitude."), 0.0, 1000.0)},
				{TEXT("noise_frequency"), FSololmcpSchemaBuilder::Number(TEXT("Noise frequency."), 0.001, 100.0)},
				{TEXT("noise_persistence"), FSololmcpSchemaBuilder::Number(TEXT("Noise persistence."), 0.0, 1.0)},
				{TEXT("noise_lacunarity"), FSololmcpSchemaBuilder::Number(TEXT("Noise lacunarity."), 1.0, 8.0)},
				{TEXT("noise_octaves"), FSololmcpSchemaBuilder::Integer(TEXT("Noise octaves."), 0, 8)},
				{TEXT("point_spacing_cm"), FSololmcpSchemaBuilder::Number(TEXT("Noise point spacing in cm."), 0.1, 10000.0)},
				{TEXT("add_collision_samples"), FSololmcpSchemaBuilder::Boolean(TEXT("Add samples for collision."))},
				{TEXT("collision_sample_spacing_cm"), FSololmcpSchemaBuilder::Number(TEXT("Collision sample spacing in cm."), 0.1, 10000.0)},
				{TEXT("split_islands"), FSololmcpSchemaBuilder::Boolean(TEXT("Split disconnected islands after fracture."))},
				{TEXT("selected_bones"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Integer(TEXT("Transform index to fracture."), 0, MAX_int32),
					TEXT("Optional subset of transform indices; defaults to all leaves."), 1, 5000, true)},
				{TEXT("num_planes"), FSololmcpSchemaBuilder::Integer(TEXT("Radial pattern plane count, 1-128."), 1, 128)},
				{TEXT("slices"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Integer(TEXT("Slice count on one axis."), 0, 32),
					TEXT("[x,y,z] slice counts."), 1, 3)},
				{TEXT("slice_angle_variation"), FSololmcpSchemaBuilder::Number(TEXT("Slice angle variation in degrees."), 0.0, 180.0)},
				{TEXT("slice_offset_variation"), FSololmcpSchemaBuilder::Number(TEXT("Slice offset variation in cm."), 0.0, 1000.0)},
				{TEXT("brick_bond"), FSololmcpSchemaBuilder::String(TEXT("Brick bond pattern."), {TEXT("stretcher"), TEXT("stack"), TEXT("english"), TEXT("header"), TEXT("flemish")})},
				{TEXT("brick_length"), FSololmcpSchemaBuilder::Number(TEXT("Brick length in cm."), 5.0, 10000.0)},
				{TEXT("brick_height"), FSololmcpSchemaBuilder::Number(TEXT("Brick height in cm."), 5.0, 10000.0)},
				{TEXT("brick_depth"), FSololmcpSchemaBuilder::Number(TEXT("Brick depth in cm."), 5.0, 10000.0)},
				{TEXT("cut_distribution"), FSololmcpSchemaBuilder::String(TEXT("Cutter scatter distribution."), {TEXT("single"), TEXT("uniform_random"), TEXT("grid")})},
				{TEXT("cutter_count"), FSololmcpSchemaBuilder::Integer(TEXT("Number of scattered cutters."), 1, 64)},
				{TEXT("cutter_grid"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Integer(TEXT("Grid count on one axis."), 1, 16),
					TEXT("[x,y,z] cutter grid."), 1, 3)},
				{TEXT("cutter_variability"), FSololmcpSchemaBuilder::Number(TEXT("Cutter placement variability."), 0.0, 1.0)},
				{TEXT("cutter_min_scale"), FSololmcpSchemaBuilder::Number(TEXT("Cutter min scale factor."), 0.01, 10.0)},
				{TEXT("cutter_max_scale"), FSololmcpSchemaBuilder::Number(TEXT("Cutter max scale factor."), 0.01, 10.0)},
				{TEXT("cutter_random_orientation"), FSololmcpSchemaBuilder::Boolean(TEXT("Randomize cutter orientation."))},
				{TEXT("cutter_roll_range"), FSololmcpSchemaBuilder::Number(TEXT("Cutter roll range in degrees."), 0.0, 360.0)},
				{TEXT("cutter_pitch_range"), FSololmcpSchemaBuilder::Number(TEXT("Cutter pitch range in degrees."), 0.0, 360.0)},
				{TEXT("cutter_yaw_range"), FSololmcpSchemaBuilder::Number(TEXT("Cutter yaw range in degrees."), 0.0, 360.0)},
				{TEXT("cutter_box_size"), FSololmcpSchemaBuilder::Number(TEXT("Procedural cutter box size in cm."), 5.0, 10000.0)},
				{TEXT("custom_sites"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("Site X."), -1.0e9, 1.0e9)},
						{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("Site Y."), -1.0e9, 1.0e9)},
						{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("Site Z."), -1.0e9, 1.0e9)}},
						{TEXT("x"), TEXT("y"), TEXT("z")}, FString(), false),
					TEXT("2-5000 explicit Voronoi site points."), 2, 5000)},
				{TEXT("cluster_mode"), FSololmcpSchemaBuilder::String(TEXT("Post-fracture clustering mode."), {TEXT("none"), TEXT("autocluster"), TEXT("magnet"), TEXT("selected")})},
				{TEXT("cluster_size_method"), FSololmcpSchemaBuilder::String(TEXT("Cluster sizing method."), {TEXT("bynumber"), TEXT("byfraction"), TEXT("bysize"), TEXT("bygrid")})},
				{TEXT("cluster_site_count"), FSololmcpSchemaBuilder::Integer(TEXT("Cluster site count."), 1, 10000)},
				{TEXT("cluster_site_size"), FSololmcpSchemaBuilder::Number(TEXT("Cluster site size in cm."), 1.0, 1000000.0)},
				{TEXT("cluster_enforce_connectivity"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("cluster_avoid_isolated"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("cluster_enforce_site_parameters"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("cluster_grid"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Integer(TEXT("Grid count on one axis."), 1, 16),
					TEXT("[x,y,z] cluster grid."), 1, 3)},
				{TEXT("cluster_magnet_iterations"), FSololmcpSchemaBuilder::Integer(TEXT("Cluster magnet iterations."), 1, 32)},
				{TEXT("collision_type"), FSololmcpSchemaBuilder::String(TEXT("Collision type."), {TEXT("volumetric"), TEXT("surface_volumetric")})},
				{TEXT("implicit_type"), FSololmcpSchemaBuilder::String(TEXT("Implicit collision shape."), {TEXT("box"), TEXT("sphere"), TEXT("capsule"), TEXT("level_set"), TEXT("none"), TEXT("convex")})},
				{TEXT("damage_thresholds"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Number(TEXT("Damage threshold."), 0.0, 1.0e9),
					TEXT("Per-level damage thresholds."), 1, 32)},
				{TEXT("damage_model"), FSololmcpSchemaBuilder::String(TEXT("Damage model."), {TEXT("user_defined"), TEXT("material_strength")})},
				{TEXT("cluster_connection"), FSololmcpSchemaBuilder::String(TEXT("Chaos cluster connection enum value (e.g. Chaos_PointImplicit)."), {}, 1, 128)},
				{TEXT("mass"), FSololmcpSchemaBuilder::Number(TEXT("Rigid body mass in kg."), 0.001, 1.0e9)},
				{TEXT("damage_propagation_enabled"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("internal_material_path"), FSololmcpSchemaBuilder::String(TEXT("Material Interface path for internal faces under /Game/."), {}, 1, 1024)},
				{TEXT("remove_on_max_sleep"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("cache_collection_path"), FSololmcpSchemaBuilder::String(TEXT("Chaos Cache Collection path under /Game/ to create or extend with one entry."), {}, 1, 1024)},
				{TEXT("dataflow_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Dataflow asset path under /Game/ to create and bind with compile diagnostics."), {}, 1, 1024)},
			},
			{TEXT("asset_path")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
			if (!Asset) return false;
			TSharedPtr<FGeometryCollection> Collection = Asset->GetGeometryCollection();
			FDataflowTransformSelection Selection;
			if (!BuildTransformSelection(Args, *Collection, Selection, Error))
			{
				Fail(Out, Error, TEXT("fracture_selection_empty"), Error);
				return false;
			}
			FString Pattern = TEXT("voronoi");
			Args->TryGetStringField(TEXT("pattern"), Pattern);
			UE_LOG(LogSOMOLMCP, Warning, TEXT("Fracture: pattern=%s selected=%d transforms=%d geometry=%d"),
				*Pattern, Selection.NumSelected(),
				Collection->NumElements(FGeometryCollection::TransformGroup),
				Collection->NumElements(FGeometryCollection::GeometryGroup));
			DetachBoundComponentRenderState(*Asset);
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FracturePatternApply", "SOMOLMCP Multi-Pattern Fracture"));
			Asset->Modify();
			if (!ApplyPatternFracture(Context, Args, *Asset, *Collection, Selection, Out, Error))
			{
				Transaction.Cancel();
				return false;
			}
			if (!ApplyClusteringConfig(*Asset, *Collection, Args, Out, Error)) { Transaction.Cancel(); return false; }
			if (!ApplyCollisionDamageConfig(*Asset, Args, Out, Error)) { Transaction.Cancel(); return false; }
			if (!ApplyMaterialRemovalConfig(Context, *Asset, *Collection, Args, Out, Error)) { Transaction.Cancel(); return false; }
			if (!RebuildAndSave(Context, *Asset, Out, Error)) { Transaction.Cancel(); return false; }
			if (!ApplyCacheHook(Context, *Asset, Args, Out, Error)) { Transaction.Cancel(); return false; }
			if (!ApplyDataflowHook(Context, *Asset, Args, Out, Error)) { Transaction.Cancel(); return false; }
			const FString ReceiptId = FString::Printf(TEXT("fracture_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower());
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), ReceiptId);
			Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			Out->SetStringField(TEXT("operation"), FString::Printf(TEXT("native_%s_fracture"), *Pattern));
			Out->SetBoolField(TEXT("mutation_applied"), true);
			Out->SetBoolField(TEXT("readback_verified"), true);
			Out->SetBoolField(TEXT("simulation_data_rebuilt"), true);
			Out->SetBoolField(TEXT("render_data_rebuilt"), true);
			Out->SetBoolField(TEXT("transaction_scoped"), true);
			Out->SetBoolField(TEXT("no_growth_detected"), false);
			Out->SetBoolField(TEXT("no_growth_cancels_transaction"), true);
			Out->SetNumberField(TEXT("selected_leaf_count"), Selection.NumSelected());
			WriteCounts(*Collection, Out);
			TArray<TSharedPtr<FJsonValue>> Boundaries;
			Boundaries.Add(MakeShared<FJsonValueString>(TEXT(
				"debris_and_anchor_asset_properties_are_not_present_in_ue58_geometry_collection_assets; removal_config_uses_the_remove_on_max_sleep_family")));
			Out->SetArrayField(TEXT("truth_boundary"), Boundaries);
			Summary = FString::Printf(TEXT("Native %s fracture created %d pieces in %s."), *Pattern,
				static_cast<int32>(Out->GetNumberField(TEXT("pieces_added"))), *Asset->GetPathName());
			return true;
		}, nullptr, 1});

	Registry.Register({TEXT("fracture_asset_preflight"),
		TEXT("Validate that a Geometry Collection contains native fractureable geometry and report its current structure."),
		AssetPathSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
			if (!Asset) return false;
			const TSharedPtr<FGeometryCollection> Collection = Asset->GetGeometryCollection();
			FDataflowTransformSelection Selection;
			Selection.InitializeFromCollection(*Collection, false);
			FFractureEngineSelection::SelectLeaf(*Collection, Selection);
			if (!Selection.AnySelected())
			{
				Fail(Out, Error, TEXT("fracture_selection_empty"), TEXT("No leaf geometry was available for fracture."));
				return false;
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			Out->SetNumberField(TEXT("selected_leaf_count"), Selection.NumSelected());
			WriteCounts(*Collection, Out);
			Summary = FString::Printf(TEXT("Fracture preflight passed for %s (%d leaf transforms)."),
				*Asset->GetPathName(), Selection.NumSelected());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("fracture_voronoi_apply"),
		TEXT("Apply native UE 5.8 Chaos uniform Voronoi fracture, rebuild simulation/render data, save, and verify geometry growth."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Geometry Collection path under /Game/."), {}, 1, 1024)},
				{TEXT("site_count"), FSololmcpSchemaBuilder::Integer(TEXT("Voronoi cell count, 2-5000."), 2, 5000)},
				{TEXT("seed"), FSololmcpSchemaBuilder::Integer(TEXT("Deterministic random seed."), 0, MAX_int32)},
				{TEXT("grout_cm"), FSololmcpSchemaBuilder::Number(TEXT("Gap between generated pieces in centimeters."), 0.0, 100.0)},
				{TEXT("noise_amplitude"), FSololmcpSchemaBuilder::Number(TEXT("Internal surface noise amplitude."), 0.0, 1000.0)},
				{TEXT("split_islands"), FSololmcpSchemaBuilder::Boolean(TEXT("Split disconnected islands after fracture."))},
			},
			{TEXT("asset_path"), TEXT("site_count")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const int32 Sites = FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("site_count"))), 2, 5000);
			const int32 Seed = Args->HasField(TEXT("seed"))
				? FMath::Max(0, static_cast<int32>(Args->GetNumberField(TEXT("seed")))) : 1337;
			const float Grout = Args->HasField(TEXT("grout_cm"))
				? FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("grout_cm"))), 0.0f, 100.0f) : 0.0f;
			const float NoiseAmplitude = Args->HasField(TEXT("noise_amplitude"))
				? FMath::Clamp(static_cast<float>(Args->GetNumberField(TEXT("noise_amplitude"))), 0.0f, 1000.0f) : 0.0f;
			const bool bSplitIslands = !Args->HasField(TEXT("split_islands")) || Args->GetBoolField(TEXT("split_islands"));

			return ApplyUniformVoronoi(Context, Args, Sites, Seed, Grout, NoiseAmplitude,
				bSplitIslands, Out, Summary, Error);
		}, nullptr, 1});

	Registry.Register({TEXT("fracture_asset_inspect"),
		TEXT("Read back Geometry Collection structure, simulation-data state, and visibility after authoring."),
		AssetPathSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
			if (!Asset) return false;
			const TSharedPtr<FGeometryCollection> Collection = Asset->GetGeometryCollection();
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			Out->SetBoolField(TEXT("simulation_data_dirty"), Asset->IsSimulationDataDirty());
			Out->SetBoolField(TEXT("has_visible_geometry"), Asset->HasVisibleGeometry());
			WriteCounts(*Collection, Out);
			Summary = FString::Printf(TEXT("Inspected Geometry Collection %s."), *Asset->GetPathName());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("fracture_rebuild_simulation_data"),
		TEXT("Rebuild Geometry Collection simulation/render data, save it, and verify registry readback."),
		AssetPathSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UGeometryCollection* Asset = LoadCollection(Context, Args, Out, Error);
			if (!Asset) return false;
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "FractureRebuildData", "SOMOLMCP Rebuild Fracture Data"));
			Asset->Modify();
			if (!RebuildAndSave(Context, *Asset, Out, Error)) return false;
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("fracture_rebuild_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
			Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			Out->SetBoolField(TEXT("mutation_applied"), true);
			Out->SetBoolField(TEXT("readback_verified"), true);
			Out->SetBoolField(TEXT("simulation_data_dirty_after"), Asset->IsSimulationDataDirty());
			Summary = FString::Printf(TEXT("Rebuilt fracture data for %s."), *Asset->GetPathName());
			return true;
		}, nullptr, 3});

	Registry.Register({TEXT("fracture_operation_receipt_validate"),
		TEXT("Validate a fracture authoring receipt before downstream destruction setup is allowed."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("receipt_id"), FSololmcpSchemaBuilder::String(TEXT("Receipt returned by a fracture writer."), {}, 1, 128)},
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Expected target asset path."), {}, 1, 1024)},
				{TEXT("status"), FSololmcpSchemaBuilder::String(TEXT("Expected succeeded/completed status."), {TEXT("succeeded"), TEXT("completed")})},
				{TEXT("mutation_applied"), FSololmcpSchemaBuilder::Boolean(TEXT("Writer confirmed mutation."))},
				{TEXT("readback_verified"), FSololmcpSchemaBuilder::Boolean(TEXT("Writer confirmed post-save readback."))},
				{TEXT("pieces_added"), FSololmcpSchemaBuilder::Integer(TEXT("Number of newly created pieces."), 1, MAX_int32)},
			},
			{TEXT("receipt_id"), TEXT("asset_path"), TEXT("status"), TEXT("mutation_applied"),
				TEXT("readback_verified"), TEXT("pieces_added")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const bool bValid = Args->GetBoolField(TEXT("mutation_applied")) &&
				Args->GetBoolField(TEXT("readback_verified")) && Args->GetNumberField(TEXT("pieces_added")) >= 1.0 &&
				(Args->GetStringField(TEXT("status")) == TEXT("succeeded") ||
				 Args->GetStringField(TEXT("status")) == TEXT("completed")) &&
				Args->GetStringField(TEXT("asset_path")).StartsWith(TEXT("/Game/"));
			if (!bValid)
			{
				Fail(Out, Error, TEXT("fracture_receipt_rejected"),
					TEXT("Fracture receipt does not prove a saved, read-back mutation with at least one new piece."));
				return false;
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetBoolField(TEXT("valid"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), Args->GetStringField(TEXT("receipt_id")));
			Out->SetStringField(TEXT("asset_path"), Args->GetStringField(TEXT("asset_path")));
			Summary = TEXT("Fracture receipt accepted.");
			return true;
		}, nullptr, 15});
#else
	(void)Registry;
#endif
}
}
