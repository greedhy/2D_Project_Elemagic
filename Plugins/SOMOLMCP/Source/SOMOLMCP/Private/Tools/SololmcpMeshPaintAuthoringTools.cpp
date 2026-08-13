// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"

// Capability, not version: this needs the module, and the module ships on
// engines below 5.8 too. Whether the API matches is what the build decides.
#ifndef SOMOLMCP_HAS_MESHPAINTINGTOOLSET
#define SOMOLMCP_HAS_MESHPAINTINGTOOLSET 0
#endif
#define SOMOLMCP_MESHPAINT_AVAILABLE (SOMOLMCP_HAS_MESHPAINTINGTOOLSET && (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)))

#if SOMOLMCP_MESHPAINT_AVAILABLE
#include "MeshPaintHelpers.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#endif

namespace UE::SOMOLMCP
{
namespace MeshPaintAuthoring
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

	static TSharedRef<FJsonObject> TargetSchema(const bool bWithColor)
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties = {
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact StaticMeshComponent name."), {}, 1, 256)},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer(TEXT("LOD index; -1 means all LODs."), -1, 64)},
		};
		if (bWithColor)
		{
			Properties.Add(TEXT("color"), FSololmcpSchemaBuilder::Object({
				{TEXT("r"), FSololmcpSchemaBuilder::Integer(TEXT("Red 0-255."), 0, 255)},
				{TEXT("g"), FSololmcpSchemaBuilder::Integer(TEXT("Green 0-255."), 0, 255)},
				{TEXT("b"), FSololmcpSchemaBuilder::Integer(TEXT("Blue 0-255."), 0, 255)},
				{TEXT("a"), FSololmcpSchemaBuilder::Integer(TEXT("Alpha 0-255."), 0, 255)},
			}, {TEXT("r"), TEXT("g"), TEXT("b")}, FString(), false));
		}
		TArray<FString> Required = {TEXT("actor_label")};
		if (bWithColor) Required.Add(TEXT("color"));
		return FSololmcpSchemaBuilder::Object(Properties, Required, FString(), false);
	}

#if SOMOLMCP_MESHPAINT_AVAILABLE
	static UStaticMeshComponent* FindComponent(const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Fail(Out, Error, TEXT("editor_world_unavailable"), TEXT("No editor world is available."));
			return nullptr;
		}
		const FString ActorLabel = Args->GetStringField(TEXT("actor_label"));
		FString ComponentName;
		Args->TryGetStringField(TEXT("component_name"), ComponentName);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || (Actor->GetActorLabel() != ActorLabel && Actor->GetName() != ActorLabel)) continue;
			TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
			for (UStaticMeshComponent* Component : Components)
			{
				if (Component && Component->GetStaticMesh() &&
					(ComponentName.IsEmpty() || Component->GetName() == ComponentName)) return Component;
			}
			Fail(Out, Error, TEXT("static_mesh_component_not_found"),
				FString::Printf(TEXT("Actor '%s' has no matching StaticMeshComponent."), *ActorLabel));
			return nullptr;
		}
		Fail(Out, Error, TEXT("actor_not_found"), FString::Printf(TEXT("Actor '%s' was not found."), *ActorLabel));
		return nullptr;
	}

	static int32 GetPaintedVertexCount(const UStaticMeshComponent& Component, const int32 LodIndex)
	{
		if (!Component.LODData.IsValidIndex(LodIndex) || !Component.LODData[LodIndex].OverrideVertexColors) return 0;
		return static_cast<int32>(Component.LODData[LodIndex].OverrideVertexColors->GetNumVertices());
	}

	static int64 GetPaintedBytes(const UStaticMeshComponent& Component, const int32 LodIndex)
	{
		int32 Bytes = 0;
		if (UMeshPaintingSubsystem* MeshPainting = GEngine
			? GEngine->GetEngineSubsystem<UMeshPaintingSubsystem>() : nullptr)
		{
			MeshPainting->GetInstanceColorDataInfo(&Component, LodIndex, Bytes);
		}
		return Bytes;
	}

	static void WriteReadback(const UStaticMeshComponent& Component, TSharedRef<FJsonObject>& Out)
	{
		const UStaticMesh* Mesh = Component.GetStaticMesh();
		const int32 LodCount = Mesh ? Mesh->GetNumLODs() : 0;
		int64 TotalBytes = 0;
		int64 TotalVertices = 0;
		TArray<TSharedPtr<FJsonValue>> Lods;
		for (int32 Lod = 0; Lod < LodCount; ++Lod)
		{
			const int64 Bytes = GetPaintedBytes(Component, Lod);
			const int32 Vertices = GetPaintedVertexCount(Component, Lod);
			TotalBytes += Bytes;
			TotalVertices += Vertices;
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("lod_index"), Lod);
			Entry->SetNumberField(TEXT("painted_vertex_count"), Vertices);
			Entry->SetNumberField(TEXT("override_color_bytes"), static_cast<double>(Bytes));
			if (Vertices > 0)
			{
				const FColor C = Component.LODData[Lod].OverrideVertexColors->VertexColor(0);
				Entry->SetStringField(TEXT("first_color"), C.ToHex());
			}
			Lods.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Out->SetNumberField(TEXT("lod_count"), LodCount);
		Out->SetNumberField(TEXT("painted_vertex_count"), static_cast<double>(TotalVertices));
		Out->SetNumberField(TEXT("override_color_bytes"), static_cast<double>(TotalBytes));
		Out->SetArrayField(TEXT("lods"), Lods);
	}

	static bool SaveOwnerMap(const FSololmcpToolExecutionContext& Context, UStaticMeshComponent& Component,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Component.MarkPackageDirty();
		AActor* Owner = Component.GetOwner();
		UWorld* World = Owner ? Owner->GetWorld() : nullptr;
		const FString PackagePath = World && World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
		if (PackagePath.IsEmpty())
		{
			Fail(Out, Error, TEXT("owner_map_path_unavailable"), TEXT("The painted component has no saveable owner map."));
			return false;
		}
		FString SaveError;
		if (!Context.Services.SaveAsset(PackagePath, false, SaveError))
		{
			Fail(Out, Error, TEXT("mesh_paint_map_save_failed"),
				SaveError.IsEmpty() ? TEXT("Failed to save the owner map.") : SaveError);
			return false;
		}
		Out->SetStringField(TEXT("map_package"), PackagePath);
		return true;
	}
#endif
}

void RegisterMeshPaintAuthoringTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_MESHPAINT_AVAILABLE
	using namespace MeshPaintAuthoring;
	Registry.Register({TEXT("mesh_paint_component_inspect"),
		TEXT("Inspect per-instance StaticMeshComponent vertex color overrides by LOD."), TargetSchema(false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			if (!Component) return false;
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("actor_label"), Component->GetOwner()->GetActorLabel());
			Out->SetStringField(TEXT("component_name"), Component->GetName());
			Out->SetStringField(TEXT("static_mesh"), Component->GetStaticMesh()->GetPathName());
			WriteReadback(*Component, Out);
			Summary = FString::Printf(TEXT("Inspected mesh paint on %s.%s."),
				*Component->GetOwner()->GetActorLabel(), *Component->GetName());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_vertex_color_fill"),
		TEXT("Fill instance vertex colors on one or all StaticMeshComponent LODs, save the map, and verify override buffers."),
		TargetSchema(true),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			if (!Component) return false;
			const TSharedPtr<FJsonObject> Color = Args->GetObjectField(TEXT("color"));
			const FColor Fill(
				static_cast<uint8>(Color->GetNumberField(TEXT("r"))),
				static_cast<uint8>(Color->GetNumberField(TEXT("g"))),
				static_cast<uint8>(Color->GetNumberField(TEXT("b"))),
				Color->HasField(TEXT("a")) ? static_cast<uint8>(Color->GetNumberField(TEXT("a"))) : 255);
			const int32 Lod = Args->HasField(TEXT("lod_index"))
				? static_cast<int32>(Args->GetNumberField(TEXT("lod_index"))) : -1;
			const int32 LodCount = Component->GetStaticMesh()->GetNumLODs();
			if (Lod >= LodCount)
			{
				Fail(Out, Error, TEXT("invalid_lod_index"), FString::Printf(TEXT("lod_index %d exceeds %d LODs."), Lod, LodCount));
				return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintFill", "SOMOLMCP Fill Vertex Colors"));
			Component->Modify();
			UMeshPaintingSubsystem* MeshPainting = GEngine
				? GEngine->GetEngineSubsystem<UMeshPaintingSubsystem>() : nullptr;
			if (!MeshPainting)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_subsystem_unavailable"),
					TEXT("UE MeshPainting subsystem is unavailable."));
				return false;
			}
			MeshPainting->FillStaticMeshVertexColors(Component, Lod, Fill, FColor::White);
			const int32 FirstLod = Lod < 0 ? 0 : Lod;
			const int32 LastLod = Lod < 0 ? LodCount - 1 : Lod;
			for (int32 Index = FirstLod; Index <= LastLod; ++Index)
			{
				if (GetPaintedVertexCount(*Component, Index) <= 0)
				{
					Transaction.Cancel();
					Fail(Out, Error, TEXT("mesh_paint_readback_empty"),
						FString::Printf(TEXT("LOD %d has no override vertex colors after fill."), Index));
					return false;
				}
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("mesh_paint_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
			Out->SetStringField(TEXT("actor_label"), Component->GetOwner()->GetActorLabel());
			Out->SetStringField(TEXT("component_name"), Component->GetName());
			Out->SetStringField(TEXT("fill_color"), Fill.ToHex());
			Out->SetBoolField(TEXT("mutation_applied"), true);
			Out->SetBoolField(TEXT("readback_verified"), true);
			WriteReadback(*Component, Out);
			Summary = FString::Printf(TEXT("Filled instance vertex colors on %s.%s."),
				*Component->GetOwner()->GetActorLabel(), *Component->GetName());
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_vertex_color_clear"),
		TEXT("Remove all instance vertex color overrides, save the map, and verify zero painted bytes."), TargetSchema(false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			if (!Component) return false;
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintClear", "SOMOLMCP Clear Vertex Colors"));
			Component->Modify();
			UMeshPaintingSubsystem* MeshPainting = GEngine
				? GEngine->GetEngineSubsystem<UMeshPaintingSubsystem>() : nullptr;
			if (!MeshPainting)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_subsystem_unavailable"),
					TEXT("UE MeshPainting subsystem is unavailable."));
				return false;
			}
			MeshPainting->RemoveComponentInstanceVertexColors(Component);
			WriteReadback(*Component, Out);
			if (Out->GetNumberField(TEXT("override_color_bytes")) != 0.0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_clear_readback_failed"), TEXT("Override vertex color data remains after clear."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("mesh_paint_clear_%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
			Out->SetStringField(TEXT("actor_label"), Component->GetOwner()->GetActorLabel());
			Out->SetStringField(TEXT("component_name"), Component->GetName());
			Out->SetBoolField(TEXT("mutation_applied"), true);
			Out->SetBoolField(TEXT("readback_verified"), true);
			Summary = FString::Printf(TEXT("Cleared instance vertex colors on %s.%s."),
				*Component->GetOwner()->GetActorLabel(), *Component->GetName());
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_operation_receipt_validate"),
		TEXT("Validate a saved mesh-paint writer receipt before downstream material blending is allowed."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("receipt_id"), FSololmcpSchemaBuilder::String(TEXT("Mesh paint receipt id."), {}, 1, 128)},
			{TEXT("status"), FSololmcpSchemaBuilder::String(TEXT("succeeded/completed."), {TEXT("succeeded"), TEXT("completed")})},
			{TEXT("mutation_applied"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("readback_verified"), FSololmcpSchemaBuilder::Boolean()},
			{TEXT("map_package"), FSololmcpSchemaBuilder::String(TEXT("Saved /Game/ map package."), {}, 1, 1024)},
		}, {TEXT("receipt_id"), TEXT("status"), TEXT("mutation_applied"), TEXT("readback_verified"), TEXT("map_package")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const FString Status = Args->GetStringField(TEXT("status"));
			const bool bValid = (Status == TEXT("succeeded") || Status == TEXT("completed")) &&
				Args->GetBoolField(TEXT("mutation_applied")) && Args->GetBoolField(TEXT("readback_verified")) &&
				Args->GetStringField(TEXT("map_package")).StartsWith(TEXT("/Game/"));
			if (!bValid)
			{
				Fail(Out, Error, TEXT("mesh_paint_receipt_rejected"), TEXT("Mesh-paint receipt lacks saved mutation/readback evidence."));
				return false;
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetBoolField(TEXT("valid"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("receipt_id"), Args->GetStringField(TEXT("receipt_id")));
			Summary = TEXT("Mesh-paint receipt accepted.");
			return true;
		}, nullptr, 15});
#else
	(void)Registry;
#endif
}
}
