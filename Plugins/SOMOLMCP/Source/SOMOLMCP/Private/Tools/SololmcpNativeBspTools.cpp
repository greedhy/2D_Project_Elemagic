// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Native UE editor BSP/CSG authoring and inspection. No Python execution.

#include "Tools/SololmcpToolRegistry.h"

#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Builders/ConeBuilder.h"
#include "Builders/CubeBuilder.h"
#include "Builders/CurvedStairBuilder.h"
#include "Builders/CylinderBuilder.h"
#include "Builders/EditorBrushBuilder.h"
#include "Builders/LinearStairBuilder.h"
#include "Builders/SheetBuilder.h"
#include "Builders/SpiralStairBuilder.h"
#include "Builders/TetrahedronBuilder.h"
#include "Builders/VolumetricBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Model.h"
#include "Engine/Polys.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "JsonObjectConverter.h"
#include "IMessageLogListing.h"
#include "Logging/TokenizedMessage.h"
#include "Materials/MaterialInterface.h"
#include "MessageLogModule.h"
#include "MeshDescription.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"

namespace UE::SOMOLMCP
{
namespace NativeBsp
{
	static void Fail(
		TSharedRef<FJsonObject>& Out,
		FString& OutError,
		const FString& Code,
		const FString& Message,
		const bool bMutationAttempted = false)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("status"), TEXT("failed_closed"));
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("reason_code"), Code);
		Out->SetStringField(TEXT("message"), Message);
		Out->SetBoolField(TEXT("mutation_attempted"), bMutationAttempted);
		OutError = Message;
	}

	static bool ResolveEditorWorld(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& OutError,
		UWorld*& OutWorld)
	{
		if (!IsInGameThread())
		{
			Fail(Out, OutError, TEXT("game_thread_required"),
				TEXT("Native BSP tools must execute on the game thread."));
			return false;
		}

		FString WorldContext;
		if (!Args->TryGetStringField(TEXT("world_context"), WorldContext))
		{
			Fail(Out, OutError, TEXT("explicit_world_context_required"),
				TEXT("world_context is required; native BSP never falls back to an implicit world."));
			return false;
		}
		if (!WorldContext.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
		{
			Fail(Out, OutError, TEXT("editor_only_operation"),
				FString::Printf(TEXT("Native BSP is editor-only; world_context '%s' is not writable."), *WorldContext));
			return false;
		}

		OutWorld = Context.Services.GetEditorWorld(OutError);
		if (!OutWorld || OutWorld->WorldType != EWorldType::Editor)
		{
			Fail(Out, OutError, TEXT("editor_world_unavailable"),
				OutError.IsEmpty() ? TEXT("A live editor world is required.") : OutError);
			return false;
		}
		Out->SetStringField(TEXT("resolved_world_context"), TEXT("editor"));
		Out->SetStringField(TEXT("resolved_world"), OutWorld->GetPathName());
		return true;
	}

	static ABrush* ResolveBrush(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& OutError)
	{
		FString ActorId;
		if (!Args->TryGetStringField(TEXT("actor"), ActorId) || ActorId.TrimStartAndEnd().IsEmpty())
		{
			Fail(Out, OutError, TEXT("missing_required_parameter"), TEXT("A non-empty 'actor' is required."));
			return nullptr;
		}
		FString FindError;
		AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, FindError);
		ABrush* Brush = Cast<ABrush>(Actor);
		if (!Brush || Brush->IsVolumeBrush() || Brush->IsBrushShape())
		{
			Fail(Out, OutError, TEXT("native_bsp_brush_not_found"),
				Brush ? TEXT("The target is a volume/shape brush, not an editable CSG brush.")
					: (FindError.IsEmpty() ? TEXT("The target is not an ABrush.") : FindError));
			return nullptr;
		}
		if (Brush->GetWorld() && Brush == Brush->GetWorld()->GetDefaultBrush())
		{
			Fail(Out, OutError, TEXT("builder_brush_forbidden"),
				TEXT("The editor builder brush is not a public authoring target."));
			return nullptr;
		}
		return Brush;
	}

	static TSharedRef<FJsonObject> Vec(const FVector& Value)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), Value.X);
		Json->SetNumberField(TEXT("y"), Value.Y);
		Json->SetNumberField(TEXT("z"), Value.Z);
		return Json;
	}

	static TSharedRef<FJsonObject> Vec3f(const FVector3f& Value)
	{
		return Vec(FVector(Value));
	}

	static TSharedRef<FJsonObject> Rot(const FRotator& Value)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("pitch"), Value.Pitch);
		Json->SetNumberField(TEXT("yaw"), Value.Yaw);
		Json->SetNumberField(TEXT("roll"), Value.Roll);
		return Json;
	}

	static FString BrushTypeName(const EBrushType Type)
	{
		switch (Type)
		{
		case Brush_Add: return TEXT("additive");
		case Brush_Subtract: return TEXT("subtractive");
		default: return TEXT("default");
		}
	}

	static uint32 TopologyHash(const ABrush* Brush)
	{
		uint32 Hash = 0;
		if (!Brush || !Brush->Brush || !Brush->Brush->Polys)
		{
			return Hash;
		}
		for (const FPoly& Poly : Brush->Brush->Polys->Element)
		{
			Hash = HashCombine(Hash, GetTypeHash(Poly.Vertices.Num()));
			for (const FVector3f& Vertex : Poly.Vertices)
			{
				Hash = HashCombine(Hash, FCrc::MemCrc32(&Vertex, sizeof(Vertex)));
			}
			Hash = HashCombine(Hash, GetTypeHash(Poly.PolyFlags));
		}
		return Hash;
	}

	static FString TopologyHashString(const ABrush* Brush)
	{
		return FString::Printf(TEXT("%08x"), TopologyHash(Brush));
	}

	static TSharedRef<FJsonObject> SerializeEditableProperties(const UObject* Object)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Object)
		{
			return Result;
		}
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
			TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(Property, Value, 0, 0);
			if (JsonValue.IsValid())
			{
				Result->SetField(Property->GetName(), JsonValue);
			}
		}
		return Result;
	}

	static bool ApplyEditableProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& Properties,
		TArray<TSharedPtr<FJsonValue>>& OutReceipts,
		FString& OutError)
	{
		if (!Object || !Properties.IsValid())
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
		{
			FProperty* Property = Object->GetClass()->FindPropertyByName(*Pair.Key);
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) ||
				Property->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance | CPF_Transient | CPF_Deprecated))
			{
				OutError = FString::Printf(TEXT("Builder property '%s' is unknown or not editable on %s."),
					*Pair.Key, *Object->GetClass()->GetName());
				return false;
			}
			void* Value = Property->ContainerPtrToValuePtr<void>(Object);
			TSharedPtr<FJsonValue> Before = FJsonObjectConverter::UPropertyToJsonValue(Property, Value, 0, 0);
			if (!FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Property, Value, 0, 0))
			{
				OutError = FString::Printf(TEXT("Builder property '%s' rejected the supplied typed value."), *Pair.Key);
				return false;
			}
			TSharedPtr<FJsonValue> After = FJsonObjectConverter::UPropertyToJsonValue(Property, Value, 0, 0);
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			Receipt->SetStringField(TEXT("property"), Pair.Key);
			if (Before.IsValid()) Receipt->SetField(TEXT("before"), Before);
			if (After.IsValid()) Receipt->SetField(TEXT("after"), After);
			OutReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
		}
		return true;
	}

	static UClass* BuilderClass(const FString& BuilderType)
	{
		if (BuilderType.Equals(TEXT("cone"), ESearchCase::IgnoreCase)) return UConeBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("cube"), ESearchCase::IgnoreCase) || BuilderType.Equals(TEXT("box"), ESearchCase::IgnoreCase)) return UCubeBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("curved_stair"), ESearchCase::IgnoreCase)) return UCurvedStairBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase)) return UCylinderBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("linear_stair"), ESearchCase::IgnoreCase)) return ULinearStairBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("sheet"), ESearchCase::IgnoreCase)) return USheetBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("spiral_stair"), ESearchCase::IgnoreCase)) return USpiralStairBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("tetrahedron"), ESearchCase::IgnoreCase)) return UTetrahedronBuilder::StaticClass();
		if (BuilderType.Equals(TEXT("volumetric"), ESearchCase::IgnoreCase)) return UVolumetricBuilder::StaticClass();
		return nullptr;
	}

	static int32 BrushOrder(const ABrush* Brush)
	{
		if (!Brush || !Brush->GetLevel()) return INDEX_NONE;
		int32 Order = 0;
		for (AActor* Actor : Brush->GetLevel()->Actors)
		{
			const ABrush* Candidate = Cast<ABrush>(Actor);
			if (!Candidate || Candidate == Brush->GetWorld()->GetDefaultBrush()) continue;
			if (Candidate == Brush) return Order;
			++Order;
		}
		return INDEX_NONE;
	}

	static TSharedRef<FJsonObject> SerializeBrush(const ABrush* Brush, const bool bIncludeBuilder = true)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Brush) return Result;
		Result->SetStringField(TEXT("path"), Brush->GetPathName());
		Result->SetStringField(TEXT("name"), Brush->GetName());
		Result->SetStringField(TEXT("label"), Brush->GetActorLabel());
		Result->SetStringField(TEXT("class"), Brush->GetClass()->GetPathName());
		Result->SetStringField(TEXT("world"), Brush->GetWorld() ? Brush->GetWorld()->GetPathName() : FString());
		Result->SetStringField(TEXT("level"), Brush->GetLevel() ? Brush->GetLevel()->GetPathName() : FString());
		Result->SetStringField(TEXT("brush_type"), BrushTypeName(static_cast<EBrushType>(Brush->BrushType)));
		Result->SetNumberField(TEXT("poly_flags"), Brush->PolyFlags);
		Result->SetNumberField(TEXT("brush_order"), BrushOrder(Brush));
		Result->SetBoolField(TEXT("not_for_client_or_server"), Brush->IsNotForClientOrServer());
		Result->SetBoolField(TEXT("display_shaded_volume"), Brush->bDisplayShadedVolume != 0);
		Result->SetNumberField(TEXT("shaded_volume_opacity"), Brush->ShadedVolumeOpacityValue);
		Result->SetObjectField(TEXT("location"), Vec(Brush->GetActorLocation()));
		Result->SetObjectField(TEXT("rotation"), Rot(Brush->GetActorRotation()));
		Result->SetObjectField(TEXT("scale"), Vec(Brush->GetActorScale3D()));
		Result->SetObjectField(TEXT("pivot_offset"), Vec(Brush->GetPivotOffset()));
		const AActor* AttachParent = Brush->GetAttachParentActor();
		Result->SetStringField(TEXT("attach_parent"), AttachParent ? AttachParent->GetPathName() : FString());
		const USceneComponent* RootComponent = Brush->GetRootComponent();
		Result->SetStringField(TEXT("attach_socket"), RootComponent ? RootComponent->GetAttachSocketName().ToString() : FString());
		TArray<TSharedPtr<FJsonValue>> DataLayerNames;
		for (const FName DataLayerName : Brush->GetDataLayerInstanceNames())
		{
			DataLayerNames.Add(MakeShared<FJsonValueString>(DataLayerName.ToString()));
		}
		Result->SetArrayField(TEXT("data_layer_instance_names"), DataLayerNames);
		TArray<TSharedPtr<FJsonValue>> DataLayerAssets;
		for (const UDataLayerAsset* DataLayerAsset : Brush->GetDataLayerAssets())
		{
			if (DataLayerAsset) DataLayerAssets.Add(MakeShared<FJsonValueString>(DataLayerAsset->GetPathName()));
		}
		Result->SetArrayField(TEXT("data_layer_assets"), DataLayerAssets);
		const int32 PolyCount = Brush->Brush && Brush->Brush->Polys ? Brush->Brush->Polys->Element.Num() : 0;
		Result->SetNumberField(TEXT("polygon_count"), PolyCount);
		Result->SetStringField(TEXT("topology_hash"), TopologyHashString(Brush));
		if (bIncludeBuilder && Brush->BrushBuilder)
		{
			Result->SetStringField(TEXT("builder_class"), Brush->BrushBuilder->GetClass()->GetPathName());
			Result->SetObjectField(TEXT("builder_parameters"), SerializeEditableProperties(Brush->BrushBuilder));
		}
		else if (bIncludeBuilder)
		{
			Result->SetStringField(TEXT("builder_null_reason"), TEXT("brush_has_no_builder_metadata"));
		}
		return Result;
	}

	static TSharedRef<FJsonObject> SerializePoly(const FPoly& Poly, const int32 Index)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("surface_id"), FString::Printf(TEXT("poly:%d"), Index));
		Result->SetNumberField(TEXT("index"), Index);
		Result->SetObjectField(TEXT("base"), Vec3f(Poly.Base));
		Result->SetObjectField(TEXT("normal"), Vec3f(Poly.Normal));
		Result->SetObjectField(TEXT("texture_u"), Vec3f(Poly.TextureU));
		Result->SetObjectField(TEXT("texture_v"), Vec3f(Poly.TextureV));
		Result->SetNumberField(TEXT("poly_flags"), Poly.PolyFlags);
		Result->SetBoolField(TEXT("selected"), (Poly.PolyFlags & PF_Selected) != 0);
		Result->SetStringField(TEXT("material"), Poly.Material ? Poly.Material->GetPathName() : FString());
		Result->SetStringField(TEXT("item_name"), Poly.ItemName.ToString());
		Result->SetNumberField(TEXT("link"), Poly.iLink);
		Result->SetNumberField(TEXT("link_surface"), Poly.iLinkSurf);
		Result->SetNumberField(TEXT("brush_poly"), Poly.iBrushPoly);
		Result->SetNumberField(TEXT("smoothing_mask"), Poly.SmoothingMask);
		Result->SetNumberField(TEXT("lightmap_scale"), Poly.LightMapScale);
		TArray<TSharedPtr<FJsonValue>> Vertices;
		for (const FVector3f& Vertex : Poly.Vertices)
		{
			Vertices.Add(MakeShared<FJsonValueObject>(Vec3f(Vertex)));
		}
		Result->SetArrayField(TEXT("vertices"), Vertices);
		return Result;
	}

	static bool ReadSurfaceIndices(
		const TSharedRef<FJsonObject>& Args,
		const int32 Count,
		TArray<int32>& OutIndices,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Args->TryGetArrayField(TEXT("surface_indices"), Values) || !Values || Values->IsEmpty())
		{
			OutError = TEXT("surface_indices must be a non-empty integer array.");
			return false;
		}
		TSet<int32> Unique;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			double Number = 0.0;
			if (!Value.IsValid() || !Value->TryGetNumber(Number) || !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
			{
				OutError = TEXT("Every surface index must be an integer.");
				return false;
			}
			const int32 Index = static_cast<int32>(Number);
			if (Index < 0 || Index >= Count)
			{
				OutError = FString::Printf(TEXT("Surface index %d is outside [0, %d)."), Index, Count);
				return false;
			}
			Unique.Add(Index);
		}
		OutIndices.Reserve(Unique.Num());
		for (const int32 Index : Unique)
		{
			OutIndices.Add(Index);
		}
		OutIndices.Sort();
		return true;
	}

	static void MarkBrushChanged(ABrush* Brush)
	{
		if (!Brush) return;
		Brush->MarkPackageDirty();
		Brush->PostEditChange();
		ABrush::SetNeedRebuild(Brush->GetLevel());
		if (GEditor)
		{
			GEditor->RebuildAlteredBSP();
			GEditor->RedrawLevelEditingViewports();
		}
	}

	static void SuccessReceipt(
		TSharedRef<FJsonObject>& Out,
		const FString& Operation,
		const TSharedPtr<FJsonObject>& Before,
		const TSharedPtr<FJsonObject>& After)
	{
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Out->SetStringField(TEXT("operation"), Operation);
		Out->SetStringField(TEXT("receipt_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Out->SetBoolField(TEXT("transactional"), true);
		Out->SetBoolField(TEXT("mutation_attempted"), true);
		if (Before.IsValid()) Out->SetObjectField(TEXT("before"), Before.ToSharedRef());
		if (After.IsValid()) Out->SetObjectField(TEXT("after"), After.ToSharedRef());
	}

	static TSharedRef<FJsonObject> WorldContextSchema()
	{
		return FSololmcpSchemaBuilder::String(
			TEXT("Explicit target world. BSP authoring supports only editor and fails closed elsewhere."),
			{TEXT("editor"), TEXT("preview"), TEXT("pie"), TEXT("standalone"), TEXT("sequencer")});
	}

	static TSharedRef<FJsonObject> ActorSchema()
	{
		return FSololmcpSchemaBuilder::String(TEXT("Unambiguous ABrush path; label/name is accepted when unique."));
	}

	static TSharedRef<FJsonObject> SurfaceArraySchema()
	{
		return FSololmcpSchemaBuilder::Array(
			FSololmcpSchemaBuilder::Integer(TEXT("Zero-based FPoly index.")),
			TEXT("Explicit source-surface indices."), 1);
	}

	static TSharedRef<FJsonObject> BrushInputSchema(
		const TMap<FString, TSharedRef<FJsonObject>>& Extra = {},
		const TArray<FString>& ExtraRequired = {})
	{
		TMap<FString, TSharedRef<FJsonObject>> Fields = {
			{TEXT("world_context"), WorldContextSchema()},
			{TEXT("actor"), ActorSchema()}
		};
		for (const TPair<FString, TSharedRef<FJsonObject>>& Pair : Extra) Fields.Add(Pair.Key, Pair.Value);
		TArray<FString> Required = {TEXT("world_context"), TEXT("actor")};
		Required.Append(ExtraRequired);
		return FSololmcpSchemaBuilder::Object(Fields, Required, FString(), false);
	}

	static TSharedRef<FJsonObject> SurfaceInputSchema(
		const TMap<FString, TSharedRef<FJsonObject>>& Extra = {},
		const TArray<FString>& ExtraRequired = {})
	{
		TMap<FString, TSharedRef<FJsonObject>> Fields = {
			{TEXT("world_context"), WorldContextSchema()},
			{TEXT("actor"), ActorSchema()},
			{TEXT("surface_indices"), SurfaceArraySchema()}
		};
		for (const TPair<FString, TSharedRef<FJsonObject>>& Pair : Extra) Fields.Add(Pair.Key, Pair.Value);
		TArray<FString> Required = {TEXT("world_context"), TEXT("actor"), TEXT("surface_indices")};
		Required.Append(ExtraRequired);
		return FSololmcpSchemaBuilder::Object(Fields, Required, FString(), false);
	}
}

void RegisterNativeBspTools(FSololmcpToolRegistry& Registry)
{
	using namespace NativeBsp;

	Registry.Register({
		TEXT("bsp_brush_create"),
		TEXT("Create an additive or subtractive native ABrush from one of UE's nine editor brush builders. Editor-only, transactional, typed and fail-closed."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("world_context"), WorldContextSchema()},
			{TEXT("builder_type"), FSololmcpSchemaBuilder::String(TEXT("Native builder type."),
				{TEXT("cone"), TEXT("cube"), TEXT("curved_stair"), TEXT("cylinder"), TEXT("linear_stair"), TEXT("sheet"), TEXT("spiral_stair"), TEXT("tetrahedron"), TEXT("volumetric")})},
			{TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("CSG operation."), {TEXT("additive"), TEXT("subtractive")})},
			{TEXT("parameters"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Typed editable properties of the selected builder."))},
			{TEXT("transform"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional location/rotation/scale object."))},
			{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label."))}
		}, {TEXT("world_context"), TEXT("builder_type"), TEXT("operation")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr;
			if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			FString BuilderType, Operation;
			Args->TryGetStringField(TEXT("builder_type"), BuilderType);
			Args->TryGetStringField(TEXT("operation"), Operation);
			UClass* Class = BuilderClass(BuilderType);
			if (!Class)
			{
				Fail(Out, Error, TEXT("unsupported_builder_type"), TEXT("builder_type must name one of the nine native UE builders."));
				return false;
			}
			const bool bAdd = Operation.Equals(TEXT("additive"), ESearchCase::IgnoreCase);
			const bool bSubtract = Operation.Equals(TEXT("subtractive"), ESearchCase::IgnoreCase);
			if (!bAdd && !bSubtract)
			{
				Fail(Out, Error, TEXT("invalid_csg_operation"), TEXT("operation must be additive or subtractive."));
				return false;
			}

			ABrush* DefaultBrush = World->GetDefaultBrush();
			if (!DefaultBrush || !DefaultBrush->Brush)
			{
				Fail(Out, Error, TEXT("builder_brush_unavailable"), TEXT("The editor builder brush is unavailable."));
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspCreate", "SOMOLMCP Create Native BSP Brush"));
			DefaultBrush->Modify();
			UEditorBrushBuilder* Builder = NewObject<UEditorBrushBuilder>(DefaultBrush, Class, NAME_None, RF_Transactional);
			if (!Builder)
			{
				Fail(Out, Error, TEXT("builder_allocation_failed"), TEXT("Failed to allocate the native brush builder."));
				return false;
			}
			DefaultBrush->BrushBuilder = Builder;
			TArray<TSharedPtr<FJsonValue>> PropertyReceipts;
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (Args->TryGetObjectField(TEXT("parameters"), Parameters) && Parameters &&
				!ApplyEditableProperties(Builder, *Parameters, PropertyReceipts, Error))
			{
				Fail(Out, Error, TEXT("invalid_builder_parameter"), Error);
				return false;
			}

			const TSharedPtr<FJsonObject>* TransformJson = nullptr;
			if (Args->TryGetObjectField(TEXT("transform"), TransformJson) && TransformJson)
			{
				FTransform Transform;
				if (!FSololmcpEditorServices::JsonToTransform(*TransformJson, Transform))
				{
					Fail(Out, Error, TEXT("invalid_transform"), TEXT("transform must contain valid location/rotation/scale values."));
					return false;
				}
				DefaultBrush->SetActorTransform(Transform, false, nullptr, ETeleportType::TeleportPhysics);
			}

			if (!Builder->Build(World, DefaultBrush))
			{
				Fail(Out, Error, TEXT("builder_rejected_parameters"), TEXT("The native builder rejected the supplied parameters."), true);
				return false;
			}

			TSet<FString> Existing;
			for (TActorIterator<ABrush> It(World); It; ++It) Existing.Add(It->GetPathName());
			const FString Command = bAdd ? TEXT("BRUSH ADD SELECTNEWBRUSH") : TEXT("BRUSH SUBTRACT SELECTNEWBRUSH");
			if (!GEditor || !GEditor->Exec(World, *Command))
			{
				Fail(Out, Error, TEXT("csg_command_failed"), TEXT("UE rejected the native CSG command."), true);
				return false;
			}

			ABrush* Created = nullptr;
			for (TActorIterator<ABrush> It(World); It; ++It)
			{
				if (*It != DefaultBrush && !Existing.Contains(It->GetPathName())) { Created = *It; break; }
			}
			if (!Created)
			{
				Fail(Out, Error, TEXT("csg_create_no_readback"), TEXT("CSG command completed but no new ABrush could be read back."), true);
				return false;
			}
			FString Label;
			if (Args->TryGetStringField(TEXT("label"), Label) && !Label.TrimStartAndEnd().IsEmpty()) Created->SetActorLabel(Label.TrimStartAndEnd(), true);
			Created->MarkPackageDirty();
			Out->SetArrayField(TEXT("property_receipts"), PropertyReceipts);
			SuccessReceipt(Out, TEXT("bsp_brush_create"), nullptr, SerializeBrush(Created));
			Summary = FString::Printf(TEXT("Created native %s %s brush '%s'."), *BuilderType, bAdd ? TEXT("additive") : TEXT("subtractive"), *Created->GetActorLabel());
			return true;
		}
	});

	Registry.Register({
		TEXT("bsp_brush_inspect"), TEXT("Losslessly inspect a native BSP brush, builder metadata, transform, CSG properties and topology identity without mutation."),
		BrushInputSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("completed"));
			Out->SetObjectField(TEXT("brush"), SerializeBrush(Brush)); Out->SetBoolField(TEXT("side_effect_free"), true);
			Summary = FString::Printf(TEXT("Inspected native BSP brush '%s'."), *Brush->GetActorLabel()); return true;
		}, nullptr, 1
	});

	Registry.Register({
		TEXT("bsp_scene_inspect"), TEXT("Enumerate all native non-volume CSG brushes in the explicit editor world without mutation."),
		FSololmcpSchemaBuilder::Object({{TEXT("world_context"), WorldContextSchema()}}, {TEXT("world_context")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			TArray<TSharedPtr<FJsonValue>> Brushes;
			for (TActorIterator<ABrush> It(World); It; ++It)
			{
				if (*It != World->GetDefaultBrush() && !It->IsVolumeBrush() && !It->IsBrushShape()) Brushes.Add(MakeShared<FJsonValueObject>(SerializeBrush(*It)));
			}
			Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("completed")); Out->SetBoolField(TEXT("side_effect_free"), true);
			Out->SetNumberField(TEXT("brush_count"), Brushes.Num()); Out->SetArrayField(TEXT("brushes"), Brushes);
			Summary = FString::Printf(TEXT("Inspected %d native BSP brushes."), Brushes.Num()); return true;
		}, nullptr, 1
	});

	Registry.Register({
		TEXT("bsp_brush_update"), TEXT("Patch typed editable parameters on a brush's original native builder and deterministically rebuild its UModel."),
		BrushInputSchema(
			{{TEXT("parameters"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Typed editable builder properties."))}},
			{TEXT("parameters")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->BrushBuilder) { Fail(Out, Error, TEXT("builder_metadata_missing"), TEXT("The brush has no native builder metadata and cannot be parameter-patched.")); return false; }
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!Args->TryGetObjectField(TEXT("parameters"), Parameters) || !Parameters) { Fail(Out, Error, TEXT("missing_required_parameter"), TEXT("parameters is required.")); return false; }
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush);
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspUpdate", "SOMOLMCP Update Native BSP Brush"));
			Brush->Modify(); Brush->Brush->Modify(); Brush->BrushBuilder->Modify();
			TArray<TSharedPtr<FJsonValue>> Receipts;
			if (!ApplyEditableProperties(Brush->BrushBuilder, *Parameters, Receipts, Error)) { Fail(Out, Error, TEXT("invalid_builder_parameter"), Error); return false; }
			if (!Brush->BrushBuilder->Build(World, Brush)) { Fail(Out, Error, TEXT("builder_rejected_parameters"), TEXT("The builder rejected the update."), true); return false; }
			MarkBrushChanged(Brush);
			Out->SetArrayField(TEXT("property_receipts"), Receipts); SuccessReceipt(Out, TEXT("bsp_brush_update"), Before, SerializeBrush(Brush));
			Summary = FString::Printf(TEXT("Updated and rebuilt native BSP brush '%s'."), *Brush->GetActorLabel()); return true;
		}
	});

	Registry.Register({
		TEXT("bsp_csg_apply"), TEXT("Set an existing native brush to additive or subtractive CSG and rebuild affected BSP."),
		BrushInputSchema(
			{{TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("CSG operation."), {TEXT("additive"), TEXT("subtractive")})}},
			{TEXT("operation")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			FString Operation; Args->TryGetStringField(TEXT("operation"), Operation);
			const bool bAdd = Operation.Equals(TEXT("additive"), ESearchCase::IgnoreCase), bSub = Operation.Equals(TEXT("subtractive"), ESearchCase::IgnoreCase);
			if (!bAdd && !bSub) { Fail(Out, Error, TEXT("invalid_csg_operation"), TEXT("operation must be additive or subtractive.")); return false; }
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush);
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspCsg", "SOMOLMCP Apply BSP CSG"));
			Brush->Modify(); Brush->BrushType = bAdd ? Brush_Add : Brush_Subtract; MarkBrushChanged(Brush);
			SuccessReceipt(Out, TEXT("bsp_csg_apply"), Before, SerializeBrush(Brush));
			Summary = FString::Printf(TEXT("Set '%s' to %s CSG."), *Brush->GetActorLabel(), bAdd ? TEXT("additive") : TEXT("subtractive")); return true;
		}
	});

	Registry.Register({
		TEXT("bsp_surface_inspect"), TEXT("Inspect all source FPoly surfaces, materials, texture vectors, flags, links and vertices without mutation."),
		BrushInputSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->Brush || !Brush->Brush->Polys) { Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Brush has no source polygon model.")); return false; }
			TArray<TSharedPtr<FJsonValue>> Surfaces; for (int32 I = 0; I < Brush->Brush->Polys->Element.Num(); ++I) Surfaces.Add(MakeShared<FJsonValueObject>(SerializePoly(Brush->Brush->Polys->Element[I], I)));
			Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("completed")); Out->SetBoolField(TEXT("side_effect_free"), true);
			Out->SetStringField(TEXT("brush"), Brush->GetPathName()); Out->SetStringField(TEXT("topology_hash"), TopologyHashString(Brush)); Out->SetArrayField(TEXT("surfaces"), Surfaces);
			Summary = FString::Printf(TEXT("Inspected %d BSP surfaces."), Surfaces.Num()); return true;
		}, nullptr, 1
	});

	Registry.Register({
		TEXT("bsp_topology_inspect"), TEXT("Inspect source BSP polygon/vertex/edge topology with deterministic local-space edge identities."),
		BrushInputSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->Brush || !Brush->Brush->Polys) { Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Brush has no source polygon model.")); return false; }
			TArray<TSharedPtr<FJsonValue>> Polygons, Edges; int32 VertexCount = 0;
			for (int32 P = 0; P < Brush->Brush->Polys->Element.Num(); ++P)
			{
				const FPoly& Poly = Brush->Brush->Polys->Element[P]; Polygons.Add(MakeShared<FJsonValueObject>(SerializePoly(Poly, P))); VertexCount += Poly.Vertices.Num();
				for (int32 V = 0; V < Poly.Vertices.Num(); ++V)
				{
					TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>(); Edge->SetStringField(TEXT("edge_id"), FString::Printf(TEXT("poly:%d:edge:%d"), P, V));
					Edge->SetNumberField(TEXT("polygon_index"), P); Edge->SetNumberField(TEXT("edge_index"), V);
					Edge->SetObjectField(TEXT("start"), Vec3f(Poly.Vertices[V])); Edge->SetObjectField(TEXT("end"), Vec3f(Poly.Vertices[(V + 1) % Poly.Vertices.Num()]));
					Edges.Add(MakeShared<FJsonValueObject>(Edge));
				}
			}
			Out->SetBoolField(TEXT("ok"), true); Out->SetStringField(TEXT("status"), TEXT("completed")); Out->SetBoolField(TEXT("side_effect_free"), true);
			Out->SetStringField(TEXT("topology_hash"), TopologyHashString(Brush)); Out->SetNumberField(TEXT("polygon_count"), Polygons.Num()); Out->SetNumberField(TEXT("vertex_reference_count"), VertexCount); Out->SetNumberField(TEXT("edge_reference_count"), Edges.Num());
			Out->SetArrayField(TEXT("polygons"), Polygons); Out->SetArrayField(TEXT("edges"), Edges);
			Summary = FString::Printf(TEXT("Inspected BSP topology: %d polygons, %d edge references."), Polygons.Num(), Edges.Num()); return true;
		}, nullptr, 1
	});

	Registry.Register({
		TEXT("bsp_topology_edit"),
		TEXT("Transactionally edit a source BSP polygon vertex, split an edge, remove a vertex, or reverse a face using an optimistic topology-hash guard."),
		BrushInputSchema({
			{TEXT("expected_topology_hash"), FSololmcpSchemaBuilder::String(TEXT("Required hash from bsp_topology_inspect; prevents stale index mutation."))},
			{TEXT("operation"), FSololmcpSchemaBuilder::String(TEXT("Topology operation."),
				{TEXT("vertex_set"), TEXT("vertex_insert"), TEXT("vertex_remove"), TEXT("edge_split"), TEXT("face_reverse")})},
			{TEXT("surface_index"), FSololmcpSchemaBuilder::Integer(TEXT("Zero-based source FPoly index."), 0)},
			{TEXT("vertex_or_edge_index"), FSololmcpSchemaBuilder::Integer(TEXT("Vertex index, insertion index, or edge start index."), 0)},
			{TEXT("position"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Required x/y/z for vertex_set and vertex_insert; optional split position for edge_split."))}
		}, {TEXT("expected_topology_hash"), TEXT("operation"), TEXT("surface_index")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr;
			if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			ABrush* Brush = ResolveBrush(Context, Args, Out, Error);
			if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->Brush || !Brush->Brush->Polys)
			{
				Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Brush has no source polygons."));
				return false;
			}

			FString ExpectedHash;
			if (!Args->TryGetStringField(TEXT("expected_topology_hash"), ExpectedHash) || ExpectedHash != TopologyHashString(Brush))
			{
				Fail(Out, Error, TEXT("stale_topology_hash"),
					TEXT("expected_topology_hash does not match the current brush; inspect again before editing."));
				Out->SetStringField(TEXT("current_topology_hash"), TopologyHashString(Brush));
				return false;
			}

			double SurfaceNumber = -1.0;
			if (!Args->TryGetNumberField(TEXT("surface_index"), SurfaceNumber) ||
				!FMath::IsNearlyEqual(SurfaceNumber, FMath::RoundToDouble(SurfaceNumber)))
			{
				Fail(Out, Error, TEXT("invalid_surface_index"), TEXT("surface_index must be an integer."));
				return false;
			}
			const int32 SurfaceIndex = static_cast<int32>(SurfaceNumber);
			if (!Brush->Brush->Polys->Element.IsValidIndex(SurfaceIndex))
			{
				Fail(Out, Error, TEXT("invalid_surface_index"), TEXT("surface_index is outside the source polygon array."));
				return false;
			}

			FString Operation;
			Args->TryGetStringField(TEXT("operation"), Operation);
			const bool bVertexSet = Operation.Equals(TEXT("vertex_set"), ESearchCase::IgnoreCase);
			const bool bVertexInsert = Operation.Equals(TEXT("vertex_insert"), ESearchCase::IgnoreCase);
			const bool bVertexRemove = Operation.Equals(TEXT("vertex_remove"), ESearchCase::IgnoreCase);
			const bool bEdgeSplit = Operation.Equals(TEXT("edge_split"), ESearchCase::IgnoreCase);
			const bool bFaceReverse = Operation.Equals(TEXT("face_reverse"), ESearchCase::IgnoreCase);
			if (!bVertexSet && !bVertexInsert && !bVertexRemove && !bEdgeSplit && !bFaceReverse)
			{
				Fail(Out, Error, TEXT("invalid_topology_operation"), TEXT("Unsupported topology operation."));
				return false;
			}

			FPoly Candidate = Brush->Brush->Polys->Element[SurfaceIndex];
			double ElementNumber = 0.0;
			int32 ElementIndex = INDEX_NONE;
			if (!bFaceReverse)
			{
				if (!Args->TryGetNumberField(TEXT("vertex_or_edge_index"), ElementNumber) ||
					!FMath::IsNearlyEqual(ElementNumber, FMath::RoundToDouble(ElementNumber)))
				{
					Fail(Out, Error, TEXT("invalid_element_index"), TEXT("vertex_or_edge_index must be an integer."));
					return false;
				}
				ElementIndex = static_cast<int32>(ElementNumber);
			}

			FVector Position = FVector::ZeroVector;
			const TSharedPtr<FJsonObject>* PositionJson = nullptr;
			const bool bHasPosition = Args->TryGetObjectField(TEXT("position"), PositionJson) && PositionJson;
			if (bHasPosition && !FSololmcpEditorServices::JsonToVector(*PositionJson, Position))
			{
				Fail(Out, Error, TEXT("invalid_position"), TEXT("position must be a finite x/y/z object."));
				return false;
			}

			if (bVertexSet)
			{
				if (!Candidate.Vertices.IsValidIndex(ElementIndex) || !bHasPosition)
				{
					Fail(Out, Error, TEXT("invalid_vertex_edit"), TEXT("vertex_set requires a valid vertex index and position."));
					return false;
				}
				Candidate.Vertices[ElementIndex] = FVector3f(Position);
			}
			else if (bVertexInsert)
			{
				if (ElementIndex < 0 || ElementIndex > Candidate.Vertices.Num() || !bHasPosition)
				{
					Fail(Out, Error, TEXT("invalid_vertex_edit"), TEXT("vertex_insert requires an insertion index in [0, vertex_count] and position."));
					return false;
				}
				Candidate.Vertices.Insert(FVector3f(Position), ElementIndex);
			}
			else if (bVertexRemove)
			{
				if (!Candidate.Vertices.IsValidIndex(ElementIndex) || Candidate.Vertices.Num() <= 3)
				{
					Fail(Out, Error, TEXT("invalid_vertex_edit"), TEXT("vertex_remove requires a valid index and cannot reduce a face below three vertices."));
					return false;
				}
				Candidate.Vertices.RemoveAt(ElementIndex);
			}
			else if (bEdgeSplit)
			{
				if (!Candidate.Vertices.IsValidIndex(ElementIndex))
				{
					Fail(Out, Error, TEXT("invalid_edge_edit"), TEXT("edge_split requires a valid edge start index."));
					return false;
				}
				const int32 NextIndex = (ElementIndex + 1) % Candidate.Vertices.Num();
				const FVector3f SplitPosition = bHasPosition
					? FVector3f(Position)
					: (Candidate.Vertices[ElementIndex] + Candidate.Vertices[NextIndex]) * 0.5f;
				Candidate.Vertices.Insert(SplitPosition, ElementIndex + 1);
			}
			else
			{
				Candidate.Reverse();
			}

			if (Candidate.Fix() < 3 || Candidate.CalcNormal(true) != 0 || !Candidate.IsConvex())
			{
				Fail(Out, Error, TEXT("invalid_topology_result"),
					TEXT("The requested edit would create a collapsed, non-planar, or non-convex BSP polygon."));
				return false;
			}

			TSharedRef<FJsonObject> Before = SerializeBrush(Brush);
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspTopologyEdit", "SOMOLMCP Edit BSP Topology"));
			Brush->Modify();
			Brush->Brush->Modify();
			Brush->Brush->Polys->Modify();
			Brush->Brush->Polys->Element[SurfaceIndex] = MoveTemp(Candidate);
			MarkBrushChanged(Brush);

			Out->SetObjectField(TEXT("surface_readback"), SerializePoly(Brush->Brush->Polys->Element[SurfaceIndex], SurfaceIndex));
			SuccessReceipt(Out, TEXT("bsp_topology_edit"), Before, SerializeBrush(Brush));
			Summary = FString::Printf(TEXT("Applied %s to BSP surface %d."), *Operation, SurfaceIndex);
			return true;
		}
	});

	Registry.Register({
		TEXT("bsp_surface_select"), TEXT("Transactionally replace or modify native source-surface selection through PF_Selected."),
		SurfaceInputSchema({{TEXT("mode"), FSololmcpSchemaBuilder::String(TEXT("Selection mode."), {TEXT("replace"), TEXT("add"), TEXT("remove")})}}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false; ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->Brush || !Brush->Brush->Polys) { Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Brush has no source polygons.")); return false; }
			TArray<int32> Indices; if (!ReadSurfaceIndices(Args, Brush->Brush->Polys->Element.Num(), Indices, Error)) { Fail(Out, Error, TEXT("invalid_surface_indices"), Error); return false; }
			FString Mode = TEXT("replace"); Args->TryGetStringField(TEXT("mode"), Mode); if (!Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase) && !Mode.Equals(TEXT("add"), ESearchCase::IgnoreCase) && !Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase)) { Fail(Out, Error, TEXT("invalid_selection_mode"), TEXT("mode must be replace, add or remove.")); return false; }
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush); const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspSelect", "SOMOLMCP Select BSP Surfaces")); Brush->Modify(); Brush->Brush->Polys->Modify();
			if (Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase)) for (FPoly& Poly : Brush->Brush->Polys->Element) Poly.PolyFlags &= ~PF_Selected;
			for (int32 Index : Indices) { if (Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase)) Brush->Brush->Polys->Element[Index].PolyFlags &= ~PF_Selected; else Brush->Brush->Polys->Element[Index].PolyFlags |= PF_Selected; }
			Brush->MarkPackageDirty(); SuccessReceipt(Out, TEXT("bsp_surface_select"), Before, SerializeBrush(Brush)); Summary = FString::Printf(TEXT("Updated selection for %d BSP surfaces."), Indices.Num()); return true;
		}
	});

	Registry.Register({
		TEXT("bsp_surface_material_set"), TEXT("Assign or clear a material on explicit native source BSP surfaces with write-after-read verification."),
		SurfaceInputSchema(
			{{TEXT("material"), FSololmcpSchemaBuilder::String(TEXT("Material asset path; empty string clears."))}},
			{TEXT("material")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false; ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->Brush || !Brush->Brush->Polys) { Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Brush has no source polygons.")); return false; }
			TArray<int32> Indices; if (!ReadSurfaceIndices(Args, Brush->Brush->Polys->Element.Num(), Indices, Error)) { Fail(Out, Error, TEXT("invalid_surface_indices"), Error); return false; }
			FString MaterialPath; if (!Args->TryGetStringField(TEXT("material"), MaterialPath)) { Fail(Out, Error, TEXT("missing_required_parameter"), TEXT("material is required; pass an empty string to clear.")); return false; }
			UMaterialInterface* Material = MaterialPath.IsEmpty() ? nullptr : LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
			if (!MaterialPath.IsEmpty() && !Material) { Fail(Out, Error, TEXT("material_not_found"), FString::Printf(TEXT("Material '%s' was not found."), *MaterialPath)); return false; }
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush); const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspMaterial", "SOMOLMCP Set BSP Surface Material")); Brush->Modify(); Brush->Brush->Modify(); Brush->Brush->Polys->Modify();
			for (int32 Index : Indices) Brush->Brush->Polys->Element[Index].Material = Material; MarkBrushChanged(Brush);
			TArray<TSharedPtr<FJsonValue>> Readback; for (int32 Index : Indices) Readback.Add(MakeShared<FJsonValueObject>(SerializePoly(Brush->Brush->Polys->Element[Index], Index))); Out->SetArrayField(TEXT("surface_readback"), Readback);
			SuccessReceipt(Out, TEXT("bsp_surface_material_set"), Before, SerializeBrush(Brush)); Summary = FString::Printf(TEXT("Set material on %d BSP surfaces."), Indices.Num()); return true;
		}
	});

	Registry.Register({
		TEXT("bsp_surface_uv_set"), TEXT("Set explicit TextureU, TextureV and optional Base vectors on native source BSP surfaces."),
		SurfaceInputSchema({
			{TEXT("texture_u"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Required x/y/z texture U vector."))},
			{TEXT("texture_v"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Required x/y/z texture V vector."))},
			{TEXT("base"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional x/y/z polygon base."))}
		}, {TEXT("texture_u"), TEXT("texture_v")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false; ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->Brush || !Brush->Brush->Polys) { Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Brush has no source polygons.")); return false; }
			TArray<int32> Indices; if (!ReadSurfaceIndices(Args, Brush->Brush->Polys->Element.Num(), Indices, Error)) { Fail(Out, Error, TEXT("invalid_surface_indices"), Error); return false; }
			const TSharedPtr<FJsonObject>* UJson = nullptr; const TSharedPtr<FJsonObject>* VJson = nullptr; const TSharedPtr<FJsonObject>* BaseJson = nullptr;
			FVector U, V, Base; if (!Args->TryGetObjectField(TEXT("texture_u"), UJson) || !UJson || !FSololmcpEditorServices::JsonToVector(*UJson, U) || !Args->TryGetObjectField(TEXT("texture_v"), VJson) || !VJson || !FSololmcpEditorServices::JsonToVector(*VJson, V)) { Fail(Out, Error, TEXT("invalid_uv_vectors"), TEXT("texture_u and texture_v must be valid x/y/z objects.")); return false; }
			const bool bHasBase = Args->TryGetObjectField(TEXT("base"), BaseJson) && BaseJson; if (bHasBase && !FSololmcpEditorServices::JsonToVector(*BaseJson, Base)) { Fail(Out, Error, TEXT("invalid_base_vector"), TEXT("base must be a valid x/y/z object.")); return false; }
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush); const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspUvSet", "SOMOLMCP Set BSP Surface UV")); Brush->Modify(); Brush->Brush->Modify(); Brush->Brush->Polys->Modify();
			for (int32 Index : Indices) { FPoly& Poly = Brush->Brush->Polys->Element[Index]; Poly.TextureU = FVector3f(U); Poly.TextureV = FVector3f(V); if (bHasBase) Poly.Base = FVector3f(Base); }
			MarkBrushChanged(Brush); TArray<TSharedPtr<FJsonValue>> Readback; for (int32 Index : Indices) Readback.Add(MakeShared<FJsonValueObject>(SerializePoly(Brush->Brush->Polys->Element[Index], Index))); Out->SetArrayField(TEXT("surface_readback"), Readback);
			SuccessReceipt(Out, TEXT("bsp_surface_uv_set"), Before, SerializeBrush(Brush)); Summary = FString::Printf(TEXT("Set explicit UV vectors on %d BSP surfaces."), Indices.Num()); return true;
		}
	});

	Registry.Register({
		TEXT("bsp_surface_uv_transform"), TEXT("Pan, scale and rotate native BSP TextureU/TextureV vectors transactionally."),
		SurfaceInputSchema({
			{TEXT("pan_u"), FSololmcpSchemaBuilder::Number(TEXT("Texture U pan in world units; default 0."))}, {TEXT("pan_v"), FSololmcpSchemaBuilder::Number(TEXT("Texture V pan in world units; default 0."))},
			{TEXT("scale_u"), FSololmcpSchemaBuilder::Number(TEXT("Texture U vector multiplier; default 1."))}, {TEXT("scale_v"), FSololmcpSchemaBuilder::Number(TEXT("Texture V vector multiplier; default 1."))},
			{TEXT("rotation_degrees"), FSololmcpSchemaBuilder::Number(TEXT("Rotate texture axes around polygon normal; default 0."))}
		}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false; ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			if (!Brush->Brush || !Brush->Brush->Polys) { Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Brush has no source polygons.")); return false; }
			TArray<int32> Indices; if (!ReadSurfaceIndices(Args, Brush->Brush->Polys->Element.Num(), Indices, Error)) { Fail(Out, Error, TEXT("invalid_surface_indices"), Error); return false; }
			double PanU = 0, PanV = 0, ScaleU = 1, ScaleV = 1, Degrees = 0; Args->TryGetNumberField(TEXT("pan_u"), PanU); Args->TryGetNumberField(TEXT("pan_v"), PanV); Args->TryGetNumberField(TEXT("scale_u"), ScaleU); Args->TryGetNumberField(TEXT("scale_v"), ScaleV); Args->TryGetNumberField(TEXT("rotation_degrees"), Degrees);
			if (FMath::IsNearlyZero(ScaleU) || FMath::IsNearlyZero(ScaleV)) { Fail(Out, Error, TEXT("invalid_uv_scale"), TEXT("scale_u and scale_v must be non-zero.")); return false; }
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush); const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspUvTransform", "SOMOLMCP Transform BSP Surface UV")); Brush->Modify(); Brush->Brush->Modify(); Brush->Brush->Polys->Modify();
			for (int32 Index : Indices)
			{
				FPoly& Poly = Brush->Brush->Polys->Element[Index]; Poly.Base += Poly.TextureU * static_cast<float>(PanU) + Poly.TextureV * static_cast<float>(PanV); Poly.TextureU *= static_cast<float>(ScaleU); Poly.TextureV *= static_cast<float>(ScaleV);
				if (!FMath::IsNearlyZero(Degrees)) { const FQuat4f Q(Poly.Normal.GetSafeNormal(), FMath::DegreesToRadians(static_cast<float>(Degrees))); Poly.TextureU = Q.RotateVector(Poly.TextureU); Poly.TextureV = Q.RotateVector(Poly.TextureV); }
			}
			MarkBrushChanged(Brush); TArray<TSharedPtr<FJsonValue>> Readback; for (int32 Index : Indices) Readback.Add(MakeShared<FJsonValueObject>(SerializePoly(Brush->Brush->Polys->Element[Index], Index))); Out->SetArrayField(TEXT("surface_readback"), Readback);
			SuccessReceipt(Out, TEXT("bsp_surface_uv_transform"), Before, SerializeBrush(Brush)); Summary = FString::Printf(TEXT("Transformed UV vectors on %d BSP surfaces."), Indices.Num()); return true;
		}
	});

	Registry.Register({
		TEXT("bsp_brush_properties_set"), TEXT("Set brush CSG flags, runtime solidity policy, display properties and brush order (first/last)."),
		BrushInputSchema({
			{TEXT("poly_flags"), FSololmcpSchemaBuilder::Integer(TEXT("Optional ABrush PolyFlags value."), 0)},
			{TEXT("not_for_client_or_server"), FSololmcpSchemaBuilder::Boolean(TEXT("Optional editor-only/runtime inclusion flag."))},
			{TEXT("display_shaded_volume"), FSololmcpSchemaBuilder::Boolean(TEXT("Optional editor display flag."))},
			{TEXT("shaded_volume_opacity"), FSololmcpSchemaBuilder::Number(TEXT("Optional editor opacity [0,1]."), 0.0, 1.0)},
			{TEXT("order"), FSololmcpSchemaBuilder::String(TEXT("Optional CSG order move."), {TEXT("unchanged"), TEXT("first"), TEXT("last")})}
		}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false; ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush); const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspProperties", "SOMOLMCP Set BSP Brush Properties")); Brush->Modify();
			double PolyFlags = 0; if (Args->TryGetNumberField(TEXT("poly_flags"), PolyFlags)) Brush->PolyFlags = static_cast<int32>(PolyFlags);
			bool BoolValue = false; if (Args->TryGetBoolField(TEXT("not_for_client_or_server"), BoolValue)) { if (BoolValue) Brush->SetNotForClientOrServer(); else Brush->ClearNotForClientOrServer(); }
			if (Args->TryGetBoolField(TEXT("display_shaded_volume"), BoolValue)) Brush->bDisplayShadedVolume = BoolValue;
			double Opacity = 0; if (Args->TryGetNumberField(TEXT("shaded_volume_opacity"), Opacity)) Brush->ShadedVolumeOpacityValue = FMath::Clamp(static_cast<float>(Opacity), 0.0f, 1.0f);
			FString Order = TEXT("unchanged"); Args->TryGetStringField(TEXT("order"), Order);
			if (!Order.Equals(TEXT("unchanged"), ESearchCase::IgnoreCase))
			{
				if (!Order.Equals(TEXT("first"), ESearchCase::IgnoreCase) && !Order.Equals(TEXT("last"), ESearchCase::IgnoreCase)) { Fail(Out, Error, TEXT("invalid_brush_order"), TEXT("order must be unchanged, first or last.")); return false; }
				TArray<AActor*> SelectedActors;
				USelection* ActorSelection = GEditor->GetSelectedActors();
				ActorSelection->GetSelectedObjects<AActor>(SelectedActors);
				GEditor->SelectNone(false, true);
				GEditor->SelectActor(Brush, true, false);
				const bool bSent = GEditor->Exec(World, Order.Equals(TEXT("first"), ESearchCase::IgnoreCase) ? TEXT("MAP SENDTO FIRST") : TEXT("MAP SENDTO LAST"));
				GEditor->SelectNone(false, true);
				for (AActor* Actor : SelectedActors)
				{
					if (IsValid(Actor)) GEditor->SelectActor(Actor, true, false);
				}
				if (!bSent) { Fail(Out, Error, TEXT("brush_order_command_failed"), TEXT("UE rejected the brush-order mutation."), true); return false; }
			}
			MarkBrushChanged(Brush); SuccessReceipt(Out, TEXT("bsp_brush_properties_set"), Before, SerializeBrush(Brush)); Summary = FString::Printf(TEXT("Updated BSP brush properties for '%s'."), *Brush->GetActorLabel()); return true;
		}
	});

	Registry.Register({
		TEXT("bsp_rebuild_validate"), TEXT("Rebuild altered BSP and return bounded native brush/Map Check diagnostics without accepting invalid geometry."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("world_context"), WorldContextSchema()},
			{TEXT("run_map_check"), FSololmcpSchemaBuilder::Boolean(TEXT("Run non-dialog Map Check after rebuild; default true."))},
			{TEXT("timeout_ms"), FSololmcpSchemaBuilder::Integer(TEXT("Explicit operation deadline in milliseconds; default 30000."), 100, 300000)}
		}, {TEXT("world_context")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr;
			if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false;
			const double Start = FPlatformTime::Seconds();
			double TimeoutMs = 30000.0;
			Args->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs);
			if (!FMath::IsFinite(TimeoutMs) || TimeoutMs < 100.0 || TimeoutMs > 300000.0)
			{
				Fail(Out, Error, TEXT("invalid_timeout"), TEXT("timeout_ms must be within [100, 300000]."));
				return false;
			}
			auto ElapsedMs = [&Start]() { return (FPlatformTime::Seconds() - Start) * 1000.0; };

			TArray<TSharedPtr<FJsonValue>> Issues;
			TArray<TSharedPtr<FJsonValue>> AffectedBrushes;
			int32 BrushCount = 0;
			for (TActorIterator<ABrush> It(World); It; ++It)
			{
				if (ElapsedMs() > TimeoutMs)
				{
					Fail(Out, Error, TEXT("bsp_rebuild_timeout"), TEXT("Deadline expired during preflight; rebuild was not started."));
					Out->SetNumberField(TEXT("timeout_ms"), TimeoutMs);
					Out->SetNumberField(TEXT("duration_ms"), ElapsedMs());
					Out->SetBoolField(TEXT("rebuild_started"), false);
					return false;
				}
				ABrush* Brush = *It;
				if (Brush == World->GetDefaultBrush() || Brush->IsVolumeBrush() || Brush->IsBrushShape()) continue;
				++BrushCount;
				AffectedBrushes.Add(MakeShared<FJsonValueString>(Brush->GetPathName()));
				if (!Brush->Brush || !Brush->Brush->Polys || Brush->Brush->Polys->Element.IsEmpty())
				{
					TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("severity"), TEXT("error"));
					Issue->SetStringField(TEXT("code"), TEXT("empty_brush_geometry"));
					Issue->SetStringField(TEXT("actor"), Brush->GetPathName());
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
					continue;
				}
				for (int32 I = 0; I < Brush->Brush->Polys->Element.Num(); ++I)
				{
					const FPoly& Poly = Brush->Brush->Polys->Element[I];
					FPoly PolyForValidation = Poly;
					if (Poly.Vertices.Num() < 3 || Poly.Normal.IsNearlyZero() || !PolyForValidation.IsConvex())
					{
						TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("severity"), TEXT("error"));
						Issue->SetStringField(TEXT("code"), TEXT("invalid_polygon"));
						Issue->SetStringField(TEXT("actor"), Brush->GetPathName());
						Issue->SetNumberField(TEXT("surface_index"), I);
						Issues.Add(MakeShared<FJsonValueObject>(Issue));
					}
				}
				ABrush::SetNeedRebuild(Brush->GetLevel());
			}
			if (!GEditor)
			{
				Fail(Out, Error, TEXT("editor_unavailable"), TEXT("GEditor is unavailable."));
				return false;
			}

			GEditor->RebuildAlteredBSP();
			const double RebuildDurationMs = ElapsedMs();
			bool bTimedOut = RebuildDurationMs > TimeoutMs;
			bool bRunMapCheck = true;
			Args->TryGetBoolField(TEXT("run_map_check"), bRunMapCheck);
			bool bMapCheckAccepted = !bRunMapCheck;
			TArray<TSharedPtr<FJsonValue>> MapCheckMessages;
			int32 MapCheckErrors = 0;
			int32 MapCheckWarnings = 0;

			if (bRunMapCheck && !bTimedOut)
			{
				FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
				TSharedRef<IMessageLogListing> MapCheckListing = MessageLogModule.GetLogListing(FName(TEXT("MapCheck")));
				MapCheckListing->ClearMessages();
				bMapCheckAccepted = GEditor->Exec(World, TEXT("MAP CHECK DONTDISPLAYDIALOG NOCLEARLOG"));
				for (const TSharedRef<FTokenizedMessage>& Message : MapCheckListing->GetFilteredMessages())
				{
					const int32 SeverityValue = static_cast<int32>(Message->GetSeverity());
					FString Severity = TEXT("info");
					if (SeverityValue <= static_cast<int32>(EMessageSeverity::Error))
					{
						Severity = TEXT("error");
						++MapCheckErrors;
					}
					else if (Message->GetSeverity() == EMessageSeverity::PerformanceWarning ||
						Message->GetSeverity() == EMessageSeverity::Warning)
					{
						Severity = TEXT("warning");
						++MapCheckWarnings;
					}
					TSharedRef<FJsonObject> MessageJson = MakeShared<FJsonObject>();
					MessageJson->SetStringField(TEXT("severity"), Severity);
					MessageJson->SetStringField(TEXT("text"), Message->ToText().ToString());
					MapCheckMessages.Add(MakeShared<FJsonValueObject>(MessageJson));
				}
				bTimedOut = ElapsedMs() > TimeoutMs;
			}

			const bool bValid = Issues.IsEmpty() && bMapCheckAccepted && MapCheckErrors == 0 && !bTimedOut;
			Out->SetBoolField(TEXT("ok"), bValid);
			Out->SetStringField(TEXT("status"), bTimedOut ? TEXT("timed_out") : (bValid ? TEXT("completed") : TEXT("validation_failed")));
			Out->SetBoolField(TEXT("mutation_attempted"), false);
			Out->SetBoolField(TEXT("side_effect_free_for_assets"), true);
			Out->SetBoolField(TEXT("rebuild_started"), true);
			Out->SetBoolField(TEXT("rebuild_completed"), true);
			Out->SetBoolField(TEXT("timeout_is_cooperative"), true);
			Out->SetStringField(TEXT("timeout_note"), TEXT("UE RebuildAlteredBSP and Map Check are synchronous engine primitives; deadline is checked before and after each primitive."));
			Out->SetNumberField(TEXT("timeout_ms"), TimeoutMs);
			Out->SetNumberField(TEXT("brush_count"), BrushCount);
			Out->SetArrayField(TEXT("affected_brushes"), AffectedBrushes);
			Out->SetNumberField(TEXT("structural_issue_count"), Issues.Num());
			Out->SetArrayField(TEXT("structural_issues"), Issues);
			Out->SetBoolField(TEXT("map_check_command_accepted"), bMapCheckAccepted);
			Out->SetNumberField(TEXT("map_check_error_count"), MapCheckErrors);
			Out->SetNumberField(TEXT("map_check_warning_count"), MapCheckWarnings);
			Out->SetArrayField(TEXT("map_check_messages"), MapCheckMessages);
			Out->SetNumberField(TEXT("rebuild_duration_ms"), RebuildDurationMs);
			Out->SetNumberField(TEXT("duration_ms"), ElapsedMs());
			if (!bValid)
			{
				Error = bTimedOut
					? TEXT("BSP rebuild validation exceeded the explicit timeout.")
					: TEXT("BSP rebuild validation reported structural or Map Check errors.");
				return false;
			}
			Summary = FString::Printf(TEXT("Rebuilt and validated %d native BSP brushes (%d Map Check warnings)."), BrushCount, MapCheckWarnings);
			return true;
		}
	});

	Registry.Register({
		TEXT("bsp_convert_to_static_mesh"), TEXT("Convert one native BSP brush to a persistent Static Mesh asset; source retention and actor replacement are explicit."),
		BrushInputSchema({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("New /Game/... asset path including asset name."))},
			{TEXT("collision_strategy"), FSololmcpSchemaBuilder::String(TEXT("Explicit output collision policy; default complex_as_simple."),
				{TEXT("none"), TEXT("project_default"), TEXT("simple_and_complex"), TEXT("simple_as_complex"), TEXT("complex_as_simple")})},
			{TEXT("replace_actor"), FSololmcpSchemaBuilder::Boolean(TEXT("Spawn a StaticMeshActor at the brush transform; default false."))},
			{TEXT("delete_source"), FSololmcpSchemaBuilder::Boolean(TEXT("Delete source brush after verified replacement; requires replace_actor=true; default false."))}
		}, {TEXT("asset_path")}),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
		{
			UWorld* World = nullptr; if (!ResolveEditorWorld(Context, Args, Out, Error, World)) return false; ABrush* Brush = ResolveBrush(Context, Args, Out, Error); if (!Brush || Brush->GetWorld() != World) return false;
			FString AssetPath; if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || !FPackageName::IsValidLongPackageName(AssetPath, true)) { Fail(Out, Error, TEXT("invalid_asset_path"), TEXT("asset_path must be a valid /Game/... long package path including asset name.")); return false; }
			if (!Brush->Brush || !Brush->Brush->Polys || Brush->Brush->Polys->Element.IsEmpty()) { Fail(Out, Error, TEXT("brush_geometry_missing"), TEXT("Cannot convert an empty brush.")); return false; }
			bool bReplace = false, bDelete = false; Args->TryGetBoolField(TEXT("replace_actor"), bReplace); Args->TryGetBoolField(TEXT("delete_source"), bDelete); if (bDelete && !bReplace) { Fail(Out, Error, TEXT("unsafe_source_delete"), TEXT("delete_source requires replace_actor=true.")); return false; }
			FString CollisionStrategy = TEXT("complex_as_simple");
			Args->TryGetStringField(TEXT("collision_strategy"), CollisionStrategy);
			ECollisionTraceFlag CollisionTrace = CTF_UseComplexAsSimple;
			bool bEnableSectionCollision = true;
			if (CollisionStrategy.Equals(TEXT("none"), ESearchCase::IgnoreCase))
			{
				CollisionTrace = CTF_UseDefault;
				bEnableSectionCollision = false;
			}
			else if (CollisionStrategy.Equals(TEXT("project_default"), ESearchCase::IgnoreCase)) CollisionTrace = CTF_UseDefault;
			else if (CollisionStrategy.Equals(TEXT("simple_and_complex"), ESearchCase::IgnoreCase)) CollisionTrace = CTF_UseSimpleAndComplex;
			else if (CollisionStrategy.Equals(TEXT("simple_as_complex"), ESearchCase::IgnoreCase)) CollisionTrace = CTF_UseSimpleAsComplex;
			else if (!CollisionStrategy.Equals(TEXT("complex_as_simple"), ESearchCase::IgnoreCase))
			{
				Fail(Out, Error, TEXT("invalid_collision_strategy"), TEXT("Unsupported collision_strategy."));
				return false;
			}
			if (FindObject<UStaticMesh>(nullptr, *AssetPath)) { Fail(Out, Error, TEXT("asset_already_exists"), TEXT("Refusing to overwrite an existing Static Mesh asset.")); return false; }
			const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath); UPackage* Package = CreatePackage(*AssetPath); if (!Package) { Fail(Out, Error, TEXT("package_create_failed"), TEXT("Failed to create the destination package.")); return false; }
			TSharedRef<FJsonObject> Before = SerializeBrush(Brush); const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "NativeBspConvert", "SOMOLMCP Convert BSP to Static Mesh"));
			UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional); if (!Mesh) { Fail(Out, Error, TEXT("static_mesh_create_failed"), TEXT("Failed to allocate the Static Mesh asset."), true); return false; }
			Mesh->AddSourceModel(); FMeshDescription* Description = Mesh->CreateMeshDescription(0); if (!Description) { Fail(Out, Error, TEXT("mesh_description_create_failed"), TEXT("Failed to allocate LOD0 MeshDescription."), true); return false; }
			TArray<FStaticMaterial> Materials; GetBrushMesh(Brush, Brush->Brush, *Description, Materials); if (Description->IsEmpty()) { Fail(Out, Error, TEXT("converted_mesh_empty"), TEXT("Brush conversion produced an empty MeshDescription."), true); return false; }
			Mesh->CommitMeshDescription(0);
			Mesh->SetStaticMaterials(Materials);
			for (int32 SectionIndex = 0; SectionIndex < Materials.Num(); ++SectionIndex)
			{
				FMeshSectionInfo SectionInfo = Mesh->GetSectionInfoMap().Get(0, SectionIndex);
				SectionInfo.MaterialIndex = SectionIndex;
				SectionInfo.bEnableCollision = bEnableSectionCollision;
				Mesh->GetSectionInfoMap().Set(0, SectionIndex, SectionInfo);
				Mesh->GetOriginalSectionInfoMap().Set(0, SectionIndex, SectionInfo);
			}
			Mesh->CreateBodySetup();
			UBodySetup* BodySetup = Mesh->GetBodySetup();
			if (!BodySetup)
			{
				Fail(Out, Error, TEXT("collision_body_setup_failed"), TEXT("Failed to create Static Mesh collision BodySetup."), true);
				return false;
			}
			BodySetup->Modify();
			BodySetup->CollisionTraceFlag = CollisionTrace;
			Mesh->SetLightMapCoordinateIndex(1);
			Mesh->SetLightMapResolution(64);
			Mesh->Build();
			BodySetup->InvalidatePhysicsData();
			if (bEnableSectionCollision) BodySetup->CreatePhysicsMeshes();
			Mesh->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Mesh);
			AStaticMeshActor* Replacement = nullptr; if (bReplace) { Replacement = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Brush->GetActorTransform()); if (!Replacement || !Replacement->GetStaticMeshComponent()) { Fail(Out, Error, TEXT("replacement_spawn_failed"), TEXT("Static Mesh asset was created but replacement actor creation failed; source brush was retained."), true); return false; } Replacement->SetActorLabel(Brush->GetActorLabel() + TEXT("_StaticMesh"), true); Replacement->GetStaticMeshComponent()->SetStaticMesh(Mesh); Replacement->MarkPackageDirty(); }
			const FString SourcePath = Brush->GetPathName(); if (bDelete) World->EditorDestroyActor(Brush, true);
			TArray<TSharedPtr<FJsonValue>> CollisionSections;
			for (int32 SectionIndex = 0; SectionIndex < Materials.Num(); ++SectionIndex)
			{
				TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
				Section->SetNumberField(TEXT("section_index"), SectionIndex);
				Section->SetBoolField(TEXT("collision_enabled"), Mesh->GetSectionInfoMap().Get(0, SectionIndex).bEnableCollision);
				CollisionSections.Add(MakeShared<FJsonValueObject>(Section));
			}
			TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
			After->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
			After->SetNumberField(TEXT("material_count"), Materials.Num());
			After->SetStringField(TEXT("collision_strategy"), CollisionStrategy.ToLower());
			After->SetNumberField(TEXT("collision_trace_flag"), static_cast<int32>(BodySetup->CollisionTraceFlag.GetValue()));
			After->SetNumberField(TEXT("simple_collision_shape_count"), BodySetup->AggGeom.GetElementCount());
			After->SetBoolField(TEXT("complex_collision_available"), bEnableSectionCollision);
			After->SetArrayField(TEXT("collision_sections"), CollisionSections);
			After->SetBoolField(TEXT("source_retained"), !bDelete);
			After->SetStringField(TEXT("source_actor"), SourcePath);
			After->SetStringField(TEXT("replacement_actor"), Replacement ? Replacement->GetPathName() : FString());
			SuccessReceipt(Out, TEXT("bsp_convert_to_static_mesh"), Before, After); Summary = FString::Printf(TEXT("Converted BSP brush to Static Mesh '%s'."), *Mesh->GetPathName()); return true;
		}
	});
}
}
