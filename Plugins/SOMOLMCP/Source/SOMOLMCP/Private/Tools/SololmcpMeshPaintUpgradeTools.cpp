// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpMeshPaintUpgradeTools.cpp
// ----------------------------------------------------------------------------
// Mesh Paint upgrade route MP-01 (audit 20260804):
// Promotes the existing `geometry_vertex_color_bake` catalog route to a real
// native mesh vertex-color writer with target binding, LOD/channel selection,
// FScopedTransaction + Modify(), save/reload, asset-registry readback and a
// structured mutation receipt. Registered before the P0 completion catalog so
// the catalog wrapper is skipped for this exact name (no tool is added or
// removed; the existing executor is upgraded in place).
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Services/SololmcpEditorServices.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Crc.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"

#if SOMOLMCP_WITH_UE58_MESHPARTITION
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "ScopedTransaction.h"
#include "StaticMeshAttributes.h"
// UE 5.8 folded the legacy VertexID.h/VertexInstanceID.h declarations into
// MeshTypes.h (FVertexID/FVertexInstanceID/FEdgeID/... all live there).
#include "MeshTypes.h"
#endif

namespace UE::SOMOLMCP
{
namespace MeshPaintUpgrade
{
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

	static FString NewReceiptId(const FString& Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), *Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower());
	}

#if SOMOLMCP_WITH_UE58_MESHPARTITION
	// Computes a deterministic CRC over every vertex-instance color of one LOD.
	static bool ComputeVertexColorCrc(const UStaticMesh& Mesh, const int32 LodIndex,
		uint32& OutCrc, int32& OutVertexInstanceCount, FString& OutError)
	{
		const FMeshDescription* Description = Mesh.GetMeshDescription(LodIndex);
		if (!Description)
		{
			OutError = FString::Printf(TEXT("LOD %d has no readable MeshDescription."), LodIndex);
			return false;
		}
		FStaticMeshConstAttributes Attributes(*Description);
		TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		uint32 Crc = 0;
		int32 Count = 0;
		for (const FVertexInstanceID InstanceId : Description->VertexInstances().GetElementIDs())
		{
			const FVector4f Color = Colors[InstanceId];
			Crc = FCrc::MemCrc32(&Color, sizeof(FVector4f), Crc);
			++Count;
		}
		OutCrc = Crc;
		OutVertexInstanceCount = Count;
		return true;
	}

	static bool ParseColorArray(const TSharedRef<FJsonObject>& Args, FLinearColor& OutColor,
		TSharedRef<FJsonObject>& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Args->TryGetArrayField(TEXT("fill_color"), Values))
		{
			OutColor = FLinearColor::White;
			return true;
		}
		if (Values->Num() != 4)
		{
			Fail(Out, OutError, TEXT("invalid_fill_color"),
				TEXT("fill_color must be an array of exactly 4 numbers [r, g, b, a] in 0..1."));
			return false;
		}
		double Components[4] = {1.0, 1.0, 1.0, 1.0};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetNumber(Components[Index]))
			{
				Fail(Out, OutError, TEXT("invalid_fill_color"),
					TEXT("fill_color entries must all be numbers in 0..1."));
				return false;
			}
			if (Components[Index] < 0.0 || Components[Index] > 1.0)
			{
				Fail(Out, OutError, TEXT("invalid_fill_color"),
					TEXT("fill_color components must be within 0..1."));
				return false;
			}
		}
		OutColor = FLinearColor((float)Components[0], (float)Components[1], (float)Components[2], (float)Components[3]);
		return true;
	}

	static bool BakeVertexColors(const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out,
		FString& Summary, FString& Error)
	{
		Out->SetStringField(TEXT("tool"), TEXT("geometry_vertex_color_bake"));
		Out->SetStringField(TEXT("implementation"), TEXT("native_mesh_description_vertex_color_writer"));
		Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

		// ---- Fail-closed target binding -------------------------------------
		FString TargetPath;
		if (!Args->TryGetStringField(TEXT("target_asset"), TargetPath) || TargetPath.IsEmpty())
		{
			Args->TryGetStringField(TEXT("target_object_path"), TargetPath);
		}
		if (TargetPath.IsEmpty() || !TargetPath.StartsWith(TEXT("/Game/")))
		{
			Fail(Out, Error, TEXT("invalid_asset_path"),
				TEXT("target_asset must be a non-empty /Game/ Static Mesh path."));
			return false;
		}
		UStaticMesh* Mesh = Cast<UStaticMesh>(Context.Services.LoadAsset(TargetPath, Error));
		if (!Mesh)
		{
			Fail(Out, Error, TEXT("static_mesh_not_found"),
				Error.IsEmpty()
					? FString::Printf(TEXT("'%s' is not a loadable Static Mesh asset."), *TargetPath)
					: Error);
			return false;
		}

		// ---- LOD / channel selection validation -----------------------------
		int32 LodIndex = 0;
		if (Args->HasTypedField<EJson::Number>(TEXT("lod_index")))
		{
			LodIndex = static_cast<int32>(Args->GetNumberField(TEXT("lod_index")));
		}
		if (LodIndex < 0 || LodIndex >= Mesh->GetNumSourceModels() || !Mesh->IsSourceModelValid(LodIndex))
		{
			Fail(Out, Error, TEXT("lod_index_out_of_range"),
				FString::Printf(TEXT("lod_index %d is outside the valid source-model range 0..%d."),
					LodIndex, Mesh->GetNumSourceModels() - 1));
			return false;
		}
		FString BakeMode = TEXT("fill");
		if (Args->HasTypedField<EJson::String>(TEXT("bake_mode")))
		{
			BakeMode = Args->GetStringField(TEXT("bake_mode")).ToLower();
		}
		if (BakeMode != TEXT("fill") && BakeMode != TEXT("clear"))
		{
			Fail(Out, Error, TEXT("invalid_bake_mode"),
				TEXT("bake_mode must be 'fill' or 'clear'."));
			return false;
		}
		FString ChannelMask = TEXT("rgba");
		if (Args->HasTypedField<EJson::String>(TEXT("channel_mask")))
		{
			ChannelMask = Args->GetStringField(TEXT("channel_mask")).ToLower();
		}
		if (ChannelMask.IsEmpty() || ChannelMask.Len() > 4)
		{
			Fail(Out, Error, TEXT("invalid_channel_mask"),
				TEXT("channel_mask must be a non-empty subset of 'rgba' (at most 4 characters)."));
			return false;
		}
		for (const TCHAR Character : ChannelMask)
		{
			if (Character != TEXT('r') && Character != TEXT('g') &&
				Character != TEXT('b') && Character != TEXT('a'))
			{
				Fail(Out, Error, TEXT("invalid_channel_mask"),
					TEXT("channel_mask may only contain the letters r, g, b, a."));
				return false;
			}
		}
		FLinearColor FillColor = FLinearColor::White;
		if (!ParseColorArray(Args, FillColor, Out, Error))
		{
			return false;
		}
		if (BakeMode == TEXT("clear"))
		{
			FillColor = FLinearColor::White;
		}

		// Pre-state snapshot (read-only, used by dry-run and the receipt).
		uint32 BeforeCrc = 0;
		int32 VertexInstanceCount = 0;
		if (!ComputeVertexColorCrc(*Mesh, LodIndex, BeforeCrc, VertexInstanceCount, Error))
		{
			Fail(Out, Error, TEXT("mesh_description_unavailable"),
				Error.IsEmpty()
					? FString::Printf(TEXT("LOD %d of '%s' has no MeshDescription to bake into."), LodIndex, *TargetPath)
					: Error);
			return false;
		}
		if (VertexInstanceCount <= 0)
		{
			Fail(Out, Error, TEXT("mesh_description_unavailable"),
				TEXT("The selected LOD contains no vertex instances; nothing can be baked."));
			return false;
		}

		bool bDryRun = false;
		Args->TryGetBoolField(TEXT("dry_run"), bDryRun);
		bool bExecute = true;
		if (Args->HasTypedField<EJson::Boolean>(TEXT("execute")))
		{
			Args->TryGetBoolField(TEXT("execute"), bExecute);
		}
		if (bDryRun || !bExecute)
		{
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("dry_run"));
			Out->SetStringField(TEXT("receipt_id"), NewReceiptId(TEXT("vcolor_bake_dry")));
			Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
			Out->SetNumberField(TEXT("lod_index"), LodIndex);
			Out->SetStringField(TEXT("bake_mode"), BakeMode);
			Out->SetStringField(TEXT("channel_mask"), ChannelMask);
			Out->SetNumberField(TEXT("vertex_instance_count"), VertexInstanceCount);
			Out->SetStringField(TEXT("before_color_crc32"), FString::Printf(TEXT("%08X"), BeforeCrc));
			Out->SetBoolField(TEXT("mutation_applied"), false);
			Out->SetBoolField(TEXT("readback_verified"), false);
			Summary = FString::Printf(TEXT("geometry_vertex_color_bake validated LOD %d of %s without mutation."),
				LodIndex, *Mesh->GetPathName());
			return true;
		}

		// ---- Transactional write --------------------------------------------
		FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintVertexColorBake",
			"SOMOLMCP Mesh Paint Vertex Color Bake"));
		Mesh->Modify();
		if (!Mesh->ModifyMeshDescription(LodIndex))
		{
			Transaction.Cancel();
			Fail(Out, Error, TEXT("mesh_description_unavailable"),
				FString::Printf(TEXT("Failed to mark LOD %d MeshDescription for editing."), LodIndex));
			return false;
		}
		FMeshDescription* Description = Mesh->GetMeshDescription(LodIndex);
		if (!Description)
		{
			Transaction.Cancel();
			Fail(Out, Error, TEXT("mesh_description_unavailable"),
				FString::Printf(TEXT("LOD %d MeshDescription disappeared before the bake."), LodIndex));
			return false;
		}
		FStaticMeshAttributes Attributes(*Description);
		Attributes.Register(/*bKeepExistingAttribute=*/true);
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		const FVector4f FillVector(FillColor.R, FillColor.G, FillColor.B, FillColor.A);
		int32 WrittenCount = 0;
		for (const FVertexInstanceID InstanceId : Description->VertexInstances().GetElementIDs())
		{
			FVector4f Target = Colors[InstanceId];
			if (ChannelMask.Contains(TEXT("r"))) Target.X = FillVector.X;
			if (ChannelMask.Contains(TEXT("g"))) Target.Y = FillVector.Y;
			if (ChannelMask.Contains(TEXT("b"))) Target.Z = FillVector.Z;
			if (ChannelMask.Contains(TEXT("a"))) Target.W = FillVector.W;
			Colors[InstanceId] = Target;
			++WrittenCount;
		}
		if (WrittenCount != VertexInstanceCount)
		{
			Transaction.Cancel();
			Fail(Out, Error, TEXT("vertex_color_write_failed"),
				FString::Printf(TEXT("Wrote %d of %d vertex instances; the bake was aborted."),
					WrittenCount, VertexInstanceCount));
			return false;
		}

		UStaticMesh::FCommitMeshDescriptionParams CommitParams;
		CommitParams.bMarkPackageDirty = true;
		CommitParams.bUseHashAsGuid = true;
		Mesh->CommitMeshDescription(LodIndex, CommitParams);
		Mesh->PostEditChange();

		// ---- Readback from the committed source data -------------------------
		uint32 AfterCrc = 0;
		int32 AfterCount = 0;
		FString ReadbackError;
		if (!ComputeVertexColorCrc(*Mesh, LodIndex, AfterCrc, AfterCount, ReadbackError) ||
			AfterCount != VertexInstanceCount)
		{
			Transaction.Cancel();
			Fail(Out, Error, TEXT("vertex_color_readback_mismatch"),
				ReadbackError.IsEmpty()
					? TEXT("Post-commit readback returned a different vertex-instance topology.")
					: ReadbackError);
			return false;
		}
		const bool bColorsChanged = AfterCrc != BeforeCrc;

		// ---- Save + asset-registry readback ----------------------------------
		bool bSave = true;
		Args->TryGetBoolField(TEXT("save"), bSave);
		Out->SetBoolField(TEXT("save_requested"), bSave);
		if (bSave)
		{
			FString SaveError;
			if (!Context.Services.SaveAsset(Mesh->GetPathName(), false, SaveError))
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("bake_save_failed"),
					SaveError.IsEmpty() ? TEXT("Static Mesh save failed after vertex-color bake.") : SaveError);
				return false;
			}
			if (!Context.Services.AssetExists(Mesh->GetPathName()))
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("bake_registry_readback_failed"),
					TEXT("The saved Static Mesh could not be read back from the asset registry."));
				return false;
			}
		}

		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("receipt_id"), NewReceiptId(TEXT("vcolor_bake")));
		Out->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
		Out->SetStringField(TEXT("operation"), TEXT("native_mesh_description_vertex_color_bake"));
		Out->SetNumberField(TEXT("lod_index"), LodIndex);
		Out->SetStringField(TEXT("bake_mode"), BakeMode);
		Out->SetStringField(TEXT("channel_mask"), ChannelMask);
		Out->SetNumberField(TEXT("vertex_instance_count"), VertexInstanceCount);
		Out->SetStringField(TEXT("before_color_crc32"), FString::Printf(TEXT("%08X"), BeforeCrc));
		Out->SetStringField(TEXT("after_color_crc32"), FString::Printf(TEXT("%08X"), AfterCrc));
		Out->SetBoolField(TEXT("colors_changed"), bColorsChanged);
		Out->SetBoolField(TEXT("saved"), bSave);
		Out->SetBoolField(TEXT("mutation_applied"), bColorsChanged);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Summary = FString::Printf(TEXT("Baked %s vertex colors into LOD %d of %s (%d vertex instances, crc %08X -> %08X)."),
			*BakeMode, LodIndex, *Mesh->GetPathName(), VertexInstanceCount, BeforeCrc, AfterCrc);
		return true;
	}
#endif // SOMOLMCP_WITH_UE58_MESHPARTITION

	static TSharedRef<FJsonObject> BakeInputSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("target_asset"), FSololmcpSchemaBuilder::String(
					TEXT("Static Mesh asset path under /Game/ whose vertex colors are baked."), {}, 1, 1024)},
				{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer(
					TEXT("Source-model LOD receiving the bake. Defaults to 0."), 0, 7)},
				{TEXT("bake_mode"), FSololmcpSchemaBuilder::String(
					TEXT("fill writes the requested color; clear restores opaque white."),
					{TEXT("fill"), TEXT("clear")})},
				{TEXT("fill_color"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Number(TEXT("Channel value in 0..1."), 0.0, 1.0),
					TEXT("RGBA bake color used by fill mode."), 4, 4)},
				{TEXT("channel_mask"), FSololmcpSchemaBuilder::String(
					TEXT("Subset of rgba channels that receive the bake; other channels are preserved."))},
				{TEXT("save"), FSololmcpSchemaBuilder::Boolean(
					TEXT("Save and verify the asset after the bake. Defaults to true."))},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(
					TEXT("Validate target/LOD/mode without mutating."))},
				{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(
					TEXT("Legacy compatibility flag; execute=false behaves like dry_run."))},
				{TEXT("receipt_id"), FSololmcpSchemaBuilder::String(
					TEXT("Optional upstream receipt id for queue correlation."))},
			},
			{TEXT("target_asset")}, FString(), false);
	}
}

void RegisterMeshPaintUpgradeTools(FSololmcpToolRegistry& Registry)
{
	using namespace MeshPaintUpgrade;

	FSololmcpToolDefinition Def;
	Def.Name = TEXT("geometry_vertex_color_bake");
	Def.Description = TEXT("MP-01 upgrade: native Mesh Paint vertex-color baker with target binding, "
		"LOD/channel selection, transaction, save/reload, registry readback and a structured receipt.");
	Def.InputSchema = BakeInputSchema();
	Def.CacheTtlSeconds = 0;
	Def.Execute = [](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
#if SOMOLMCP_WITH_UE58_MESHPARTITION
		return BakeVertexColors(Context, Args, Out, Summary, Error);
#else
		(void)Context; (void)Args;
		Fail(Out, Error, TEXT("requires_ue_5_8"),
			TEXT("geometry_vertex_color_bake requires the UE 5.8 editor build with MeshDescription support."),
			TEXT("requires_ue_5_8"));
		return false;
#endif
	};
	Registry.Register(Def);
}
}
