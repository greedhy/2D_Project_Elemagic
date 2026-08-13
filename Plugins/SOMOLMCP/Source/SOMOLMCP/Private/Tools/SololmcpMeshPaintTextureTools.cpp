// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"

// Capability, not version: this needs the module, and the module ships on
// engines below 5.8 too. Whether the API matches is what the build decides.
#ifndef SOMOLMCP_HAS_MESHPAINTINGTOOLSET
#define SOMOLMCP_HAS_MESHPAINTINGTOOLSET 0
#endif
#define SOMOLMCP_MESHPAINT_AVAILABLE (SOMOLMCP_HAS_MESHPAINTINGTOOLSET && (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)))

#if SOMOLMCP_MESHPAINT_AVAILABLE
#include "MeshPaintHelpers.h"
#include "VT/MeshPaintVirtualTexture.h"
#endif

namespace UE::SOMOLMCP
{
namespace MeshPaintTextureTools
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

	static TSharedRef<FJsonObject> ComponentSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact StaticMeshComponent name."), {}, 1, 256)},
		}, {TEXT("actor_label")}, FString(), false);
	}

	static TSharedRef<FJsonObject> SessionSchema(const FString& Field, const FString& Description)
	{
		return FSololmcpSchemaBuilder::Object({
			{Field, FSololmcpSchemaBuilder::String(Description, {}, 1, 128)},
		}, {Field}, FString(), false);
	}

#if SOMOLMCP_MESHPAINT_AVAILABLE
	struct FTextureSourceSnapshot
	{
		bool bHadTexture = false;
		int32 SizeX = 0;
		int32 SizeY = 0;
		int32 NumSlices = 0;
		int32 NumMips = 0;
		ETextureSourceFormat Format = TSF_Invalid;
		bool bSRGB = true;
		TextureCompressionSettings CompressionSettings = TC_Default;
		TextureMipGenSettings MipGenSettings = TMGS_FromTextureGroup;
		TArray<TArray64<uint8>> Mips;
	};

	struct FComponentSnapshot
	{
		bool bEnableTextureColorMeshPainting = false;
		bool bOverrideUv = false;
		bool bOverrideResolution = false;
		int32 UvIndex = 0;
		int32 Resolution = 0;
		FTextureSourceSnapshot Texture;
	};

	struct FStoredSnapshot
	{
		FString Id;
		TWeakObjectPtr<UStaticMeshComponent> Component;
		FComponentSnapshot Snapshot;
		uint32 BeginHash = 0;
	};

	static FCriticalSection GSnapshotMutex;
	static TMap<FString, FStoredSnapshot> GSessions;
	static TMap<FString, FStoredSnapshot> GBackups;

	static UStaticMeshComponent* FindComponent(const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out, FString& Error, const FString& Prefix = FString())
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Fail(Out, Error, TEXT("editor_world_unavailable"), TEXT("No editor world is available."));
			return nullptr;
		}
		const FString ActorField = Prefix + TEXT("actor_label");
		const FString ComponentField = Prefix + TEXT("component_name");
		FString ActorLabel;
		if (!Args->TryGetStringField(ActorField, ActorLabel) || ActorLabel.IsEmpty())
		{
			Fail(Out, Error, TEXT("missing_actor_label"), FString::Printf(TEXT("Missing '%s'."), *ActorField));
			return nullptr;
		}
		FString ComponentName;
		Args->TryGetStringField(ComponentField, ComponentName);
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

	static UMeshPaintingSubsystem* GetSubsystem(TSharedRef<FJsonObject>& Out, FString& Error)
	{
		UMeshPaintingSubsystem* Subsystem = GEngine ? GEngine->GetEngineSubsystem<UMeshPaintingSubsystem>() : nullptr;
		if (!Subsystem)
		{
			Fail(Out, Error, TEXT("mesh_paint_subsystem_unavailable"),
				TEXT("UE 5.8 MeshPainting subsystem is unavailable."));
		}
		return Subsystem;
	}

	static UTexture2D* GetTexture(UStaticMeshComponent& Component, TSharedRef<FJsonObject>& Out,
		FString& Error, const bool bRequired = true)
	{
		UTexture2D* Texture = Cast<UTexture2D>(Component.GetMeshPaintTexture());
		if (!Texture && bRequired)
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_missing"),
				TEXT("The component has no UTexture2D Mesh Paint texture."));
		}
		return Texture;
	}

	static bool CaptureTexture(UTexture2D* Texture, FTextureSourceSnapshot& Snapshot,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Snapshot = FTextureSourceSnapshot();
		if (!Texture) return true;
		if (!Texture->Source.IsValid())
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_source_unavailable"),
				TEXT("The Mesh Paint texture has no readable editor source data."));
			return false;
		}
		if (Texture->Source.GetNumBlocks() != 1 || Texture->Source.GetNumLayers() != 1 ||
			Texture->Source.GetNumSlices() != 1)
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_source_unsupported"),
				TEXT("Only single-block, single-layer, single-slice Mesh Paint textures are supported."));
			return false;
		}
		Snapshot.bHadTexture = true;
		Snapshot.SizeX = static_cast<int32>(Texture->Source.GetSizeX());
		Snapshot.SizeY = static_cast<int32>(Texture->Source.GetSizeY());
		Snapshot.NumSlices = Texture->Source.GetNumSlices();
		Snapshot.NumMips = Texture->Source.GetNumMips();
		Snapshot.Format = Texture->Source.GetFormat();
		Snapshot.bSRGB = Texture->SRGB;
		Snapshot.CompressionSettings = Texture->CompressionSettings;
		Snapshot.MipGenSettings = Texture->MipGenSettings;
		Snapshot.Mips.SetNum(Snapshot.NumMips);
		for (int32 MipIndex = 0; MipIndex < Snapshot.NumMips; ++MipIndex)
		{
			if (!Texture->Source.GetMipData(Snapshot.Mips[MipIndex], MipIndex))
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_snapshot_failed"),
					FString::Printf(TEXT("Failed to read source mip %d."), MipIndex));
				return false;
			}
		}
		return true;
	}

	static bool CaptureComponent(UStaticMeshComponent& Component, FComponentSnapshot& Snapshot,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Snapshot.bEnableTextureColorMeshPainting = Component.bEnableTextureColorMeshPainting;
		Snapshot.bOverrideUv = Component.bOverrideMeshPaintTextureCoordinateIndex;
		Snapshot.bOverrideResolution = Component.bOverrideMeshPaintTextureResolution;
		Snapshot.UvIndex = Component.OverriddenMeshPaintTextureCoordinateIndex;
		Snapshot.Resolution = Component.OverriddenMeshPaintTextureResolution;
		return CaptureTexture(Cast<UTexture2D>(Component.GetMeshPaintTexture()), Snapshot.Texture, Out, Error);
	}

	static uint32 SnapshotHash(const FTextureSourceSnapshot& Snapshot)
	{
		uint32 Hash = 0;
		for (const TArray64<uint8>& Mip : Snapshot.Mips)
		{
			if (!Mip.IsEmpty()) Hash = FCrc::MemCrc32(Mip.GetData(), static_cast<int32>(Mip.Num()), Hash);
		}
		return Hash;
	}

	static bool CurrentHash(UStaticMeshComponent& Component, uint32& Hash,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FTextureSourceSnapshot Snapshot;
		if (!CaptureTexture(Cast<UTexture2D>(Component.GetMeshPaintTexture()), Snapshot, Out, Error)) return false;
		Hash = SnapshotHash(Snapshot);
		return true;
	}

	static bool RestoreTexture(UStaticMeshComponent& Component, const FTextureSourceSnapshot& Snapshot,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		UMeshPaintingSubsystem* Subsystem = GetSubsystem(Out, Error);
		if (!Subsystem) return false;
		if (!Snapshot.bHadTexture)
		{
			Subsystem->RemoveComponentMeshPaintTexture(&Component);
			return Component.GetMeshPaintTexture() == nullptr;
		}
		UTexture2D* Texture = Cast<UTexture2D>(Component.GetMeshPaintTexture());
		if (!Texture)
		{
			Subsystem->CreateComponentMeshPaintTexture(&Component);
			Texture = Cast<UTexture2D>(Component.GetMeshPaintTexture());
		}
		if (!Texture)
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_restore_create_failed"),
				TEXT("Failed to recreate the component Mesh Paint texture."));
			return false;
		}
		Texture->Modify();
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Snapshot.SizeX, Snapshot.SizeY, Snapshot.NumSlices,
			Snapshot.NumMips, Snapshot.Format, nullptr);
		for (int32 MipIndex = 0; MipIndex < Snapshot.NumMips; ++MipIndex)
		{
			const int64 ExpectedSize = Texture->Source.CalcMipSize(0, 0, MipIndex);
			if (ExpectedSize != Snapshot.Mips[MipIndex].Num())
			{
				Texture->PostEditChange();
				Fail(Out, Error, TEXT("mesh_paint_texture_restore_size_mismatch"),
					FString::Printf(TEXT("Mip %d restore size mismatch."), MipIndex));
				return false;
			}
			uint8* Destination = Texture->Source.LockMip(MipIndex);
			if (!Destination)
			{
				Texture->PostEditChange();
				Fail(Out, Error, TEXT("mesh_paint_texture_restore_lock_failed"),
					FString::Printf(TEXT("Failed to lock mip %d for restore."), MipIndex));
				return false;
			}
			FMemory::Memcpy(Destination, Snapshot.Mips[MipIndex].GetData(), Snapshot.Mips[MipIndex].Num());
			Texture->Source.UnlockMip(MipIndex);
		}
		Texture->SRGB = Snapshot.bSRGB;
		Texture->CompressionSettings = Snapshot.CompressionSettings;
		Texture->MipGenSettings = Snapshot.MipGenSettings;
		Texture->PostEditChange();
		Texture->UpdateResource();
		FTextureSourceSnapshot Readback;
		if (!CaptureTexture(Texture, Readback, Out, Error)) return false;
		if (SnapshotHash(Readback) != SnapshotHash(Snapshot))
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_restore_readback_failed"),
				TEXT("Restored texture bytes do not match the snapshot."));
			return false;
		}
		return true;
	}

	static bool RestoreComponent(UStaticMeshComponent& Component, const FComponentSnapshot& Snapshot,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Component.Modify();
		Component.bEnableTextureColorMeshPainting = Snapshot.bEnableTextureColorMeshPainting;
		Component.bOverrideMeshPaintTextureCoordinateIndex = Snapshot.bOverrideUv;
		Component.bOverrideMeshPaintTextureResolution = Snapshot.bOverrideResolution;
		Component.OverriddenMeshPaintTextureCoordinateIndex = Snapshot.UvIndex;
		Component.OverriddenMeshPaintTextureResolution = Snapshot.Resolution;
		if (!RestoreTexture(Component, Snapshot.Texture, Out, Error)) return false;
		Component.MarkRenderStateDirty();
		return true;
	}

	static bool SaveMap(const FSololmcpToolExecutionContext& Context, UStaticMeshComponent& Component,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		Component.MarkPackageDirty();
		UWorld* World = Component.GetWorld();
		const FString PackagePath = World && World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
		if (PackagePath.IsEmpty())
		{
			Fail(Out, Error, TEXT("owner_map_path_unavailable"), TEXT("The component has no saveable owner map."));
			return false;
		}
		FString SaveError;
		if (!Context.Services.SaveAsset(PackagePath, false, SaveError))
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_map_save_failed"),
				SaveError.IsEmpty() ? TEXT("Failed to save the owner map.") : SaveError);
			return false;
		}
		Out->SetStringField(TEXT("map_package"), PackagePath);
		return true;
	}

	static void WriteComponentInfo(UStaticMeshComponent& Component, TSharedRef<FJsonObject>& Out)
	{
		Out->SetStringField(TEXT("actor_label"), Component.GetOwner() ? Component.GetOwner()->GetActorLabel() : FString());
		Out->SetStringField(TEXT("component_name"), Component.GetName());
		Out->SetStringField(TEXT("static_mesh"), Component.GetStaticMesh() ? Component.GetStaticMesh()->GetPathName() : FString());
		Out->SetBoolField(TEXT("texture_paint_enabled"), Component.bEnableTextureColorMeshPainting);
		Out->SetBoolField(TEXT("override_uv"), Component.bOverrideMeshPaintTextureCoordinateIndex);
		Out->SetBoolField(TEXT("override_resolution"), Component.bOverrideMeshPaintTextureResolution);
		Out->SetNumberField(TEXT("uv_channel"), Component.GetMeshPaintTextureCoordinateIndex());
		Out->SetNumberField(TEXT("resolution"), Component.GetMeshPaintTextureResolution());
		if (UTexture2D* Texture = Cast<UTexture2D>(Component.GetMeshPaintTexture()))
		{
			Out->SetStringField(TEXT("texture_path"), Texture->GetPathName());
			Out->SetNumberField(TEXT("source_width"), Texture->Source.GetSizeX());
			Out->SetNumberField(TEXT("source_height"), Texture->Source.GetSizeY());
			Out->SetNumberField(TEXT("source_mips"), Texture->Source.GetNumMips());
		}
		else
		{
			Out->SetField(TEXT("texture_path"), MakeShared<FJsonValueNull>());
		}
	}

	static void CompleteWrite(UStaticMeshComponent& Component, const FString& Operation,
		const uint32 BeforeHash, const uint32 AfterHash, TSharedRef<FJsonObject>& Out)
	{
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("mesh_paint_texture_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
		Out->SetStringField(TEXT("operation"), Operation);
		Out->SetBoolField(TEXT("mutation_applied"), true);
		Out->SetBoolField(TEXT("saved"), true);
		Out->SetBoolField(TEXT("readback_verified"), true);
		Out->SetNumberField(TEXT("before_hash"), BeforeHash);
		Out->SetNumberField(TEXT("after_hash"), AfterHash);
		WriteComponentInfo(Component, Out);
	}

	static bool RollbackAndFail(FScopedTransaction& Transaction, UStaticMeshComponent& Component,
		const FComponentSnapshot& Before, TSharedRef<FJsonObject>& Out, FString& Error,
		const FString& Code, const FString& Message)
	{
		FString RestoreError;
		TSharedRef<FJsonObject> RestoreOut = MakeShared<FJsonObject>();
		const bool bRestored = RestoreComponent(Component, Before, RestoreOut, RestoreError);
		Transaction.Cancel();
		Fail(Out, Error, Code, bRestored ? Message : Message + TEXT(" Rollback also failed: ") + RestoreError);
		Out->SetBoolField(TEXT("rollback_applied"), bRestored);
		return false;
	}

	static bool FillTexture(UTexture2D& Texture, const FColor Color,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		if (Texture.Source.GetFormat() != TSF_BGRA8 || Texture.Source.GetNumLayers() != 1 ||
			Texture.Source.GetNumBlocks() != 1 || Texture.Source.GetNumSlices() != 1)
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_fill_format_unsupported"),
				TEXT("Texture fill requires a single-layer TSF_BGRA8 Mesh Paint texture."));
			return false;
		}
		Texture.Modify();
		Texture.PreEditChange(nullptr);
		for (int32 MipIndex = 0; MipIndex < Texture.Source.GetNumMips(); ++MipIndex)
		{
			const int64 Bytes = Texture.Source.CalcMipSize(0, 0, MipIndex);
			uint8* Data = Texture.Source.LockMip(MipIndex);
			if (!Data || Bytes <= 0 || Bytes % 4 != 0)
			{
				if (Data) Texture.Source.UnlockMip(MipIndex);
				Texture.PostEditChange();
				Fail(Out, Error, TEXT("mesh_paint_texture_fill_lock_failed"),
					FString::Printf(TEXT("Failed to edit mip %d."), MipIndex));
				return false;
			}
			for (int64 Offset = 0; Offset < Bytes; Offset += 4)
			{
				Data[Offset] = Color.B;
				Data[Offset + 1] = Color.G;
				Data[Offset + 2] = Color.R;
				Data[Offset + 3] = Color.A;
			}
			Texture.Source.UnlockMip(MipIndex);
		}
		Texture.PostEditChange();
		Texture.UpdateResource();
		return true;
	}

	static FColor ReadColor(const TSharedRef<FJsonObject>& Args)
	{
		const TSharedPtr<FJsonObject> Color = Args->GetObjectField(TEXT("color"));
		return FColor(static_cast<uint8>(Color->GetNumberField(TEXT("r"))),
			static_cast<uint8>(Color->GetNumberField(TEXT("g"))),
			static_cast<uint8>(Color->GetNumberField(TEXT("b"))),
			Color->HasField(TEXT("a")) ? static_cast<uint8>(Color->GetNumberField(TEXT("a"))) : 255);
	}

	static TSharedRef<FJsonObject> FillSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact component name."), {}, 1, 256)},
			{TEXT("color"), FSololmcpSchemaBuilder::Object({
				{TEXT("r"), FSololmcpSchemaBuilder::Integer(TEXT("Red 0-255."), 0, 255)},
				{TEXT("g"), FSololmcpSchemaBuilder::Integer(TEXT("Green 0-255."), 0, 255)},
				{TEXT("b"), FSololmcpSchemaBuilder::Integer(TEXT("Blue 0-255."), 0, 255)},
				{TEXT("a"), FSololmcpSchemaBuilder::Integer(TEXT("Alpha 0-255."), 0, 255)},
			}, {TEXT("r"), TEXT("g"), TEXT("b")}, FString(), false)},
		}, {TEXT("actor_label"), TEXT("color")}, FString(), false);
	}

	static int32 ChannelOffset(const FString& Channel)
	{
		if (Channel.Equals(TEXT("b"), ESearchCase::IgnoreCase)) return 0;
		if (Channel.Equals(TEXT("g"), ESearchCase::IgnoreCase)) return 1;
		if (Channel.Equals(TEXT("r"), ESearchCase::IgnoreCase)) return 2;
		return 3;
	}

	static TSharedRef<FJsonObject> FilterSchema(const bool bIncludeChannel)
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties = {
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact component name."), {}, 1, 256)},
		};
		if (bIncludeChannel)
		{
			Properties.Add(TEXT("source_channel"), FSololmcpSchemaBuilder::String(TEXT("Source color channel."), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}));
			Properties.Add(TEXT("target_channel"), FSololmcpSchemaBuilder::String(TEXT("Target color channel."), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")}));
			Properties.Add(TEXT("invert"), FSololmcpSchemaBuilder::Boolean(TEXT("Invert the copied channel value.")));
		}
		else
		{
			Properties.Add(TEXT("radius"), FSololmcpSchemaBuilder::Integer(TEXT("Box blur radius in texels."), 1, 8));
			Properties.Add(TEXT("iterations"), FSololmcpSchemaBuilder::Integer(TEXT("Blur iteration count."), 1, 8));
		}
		return FSololmcpSchemaBuilder::Object(Properties,
			bIncludeChannel ? TArray<FString>{TEXT("actor_label"), TEXT("source_channel"), TEXT("target_channel")}
				: TArray<FString>{TEXT("actor_label"), TEXT("radius"), TEXT("iterations")}, FString(), false);
	}

	static bool BlurTexture(UTexture2D& Texture, const int32 Radius, const int32 Iterations,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		if (Texture.Source.GetFormat() != TSF_BGRA8 || Texture.Source.GetNumLayers() != 1 ||
			Texture.Source.GetNumBlocks() != 1 || Texture.Source.GetNumSlices() != 1)
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_filter_format_unsupported"),
				TEXT("Native texture filters require a single-layer TSF_BGRA8 Mesh Paint texture."));
			return false;
		}
		Texture.Modify();
		Texture.PreEditChange(nullptr);
		for (int32 MipIndex = 0; MipIndex < Texture.Source.GetNumMips(); ++MipIndex)
		{
			const int32 Width = FMath::Max(1, static_cast<int32>(Texture.Source.GetSizeX()) >> MipIndex);
			const int32 Height = FMath::Max(1, static_cast<int32>(Texture.Source.GetSizeY()) >> MipIndex);
			const int64 Bytes = Texture.Source.CalcMipSize(0, 0, MipIndex);
			uint8* Data = Texture.Source.LockMip(MipIndex);
			if (!Data || Bytes != static_cast<int64>(Width) * Height * 4)
			{
				if (Data) Texture.Source.UnlockMip(MipIndex);
				Texture.PostEditChange();
				Fail(Out, Error, TEXT("mesh_paint_texture_filter_lock_failed"), FString::Printf(TEXT("Failed to edit mip %d."), MipIndex));
				return false;
			}
			TArray<uint8> Source;
			Source.Append(Data, Bytes);
			TArray<uint8> Target;
			Target.SetNumUninitialized(Bytes);
			for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
			{
				for (int32 Y = 0; Y < Height; ++Y)
				{
					for (int32 X = 0; X < Width; ++X)
					{
						int32 Sum[4] = {0, 0, 0, 0};
						int32 Samples = 0;
						for (int32 DY = -Radius; DY <= Radius; ++DY)
						{
							const int32 SY = FMath::Clamp(Y + DY, 0, Height - 1);
							for (int32 DX = -Radius; DX <= Radius; ++DX)
							{
								const int32 SX = FMath::Clamp(X + DX, 0, Width - 1);
								const int64 SourceOffset = (static_cast<int64>(SY) * Width + SX) * 4;
								for (int32 Channel = 0; Channel < 4; ++Channel) Sum[Channel] += Source[SourceOffset + Channel];
								++Samples;
							}
						}
						const int64 TargetOffset = (static_cast<int64>(Y) * Width + X) * 4;
						for (int32 Channel = 0; Channel < 4; ++Channel) Target[TargetOffset + Channel] = static_cast<uint8>(Sum[Channel] / Samples);
					}
				}
				Swap(Source, Target);
			}
			FMemory::Memcpy(Data, Source.GetData(), Bytes);
			Texture.Source.UnlockMip(MipIndex);
		}
		Texture.PostEditChange();
		Texture.UpdateResource();
		return true;
	}

	static TSharedRef<FJsonObject> CopyComponentSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("source_actor_label"), FSololmcpSchemaBuilder::String(TEXT("Source Actor label."), {}, 1, 256)},
			{TEXT("source_component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional source component name."), {}, 1, 256)},
			{TEXT("target_actor_label"), FSololmcpSchemaBuilder::String(TEXT("Target Actor label."), {}, 1, 256)},
			{TEXT("target_component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional target component name."), {}, 1, 256)},
		}, {TEXT("source_actor_label"), TEXT("target_actor_label")}, FString(), false);
	}

	static bool CopySnapshotToTarget(const FSololmcpToolExecutionContext& Context,
		UStaticMeshComponent& Target, const FTextureSourceSnapshot& SourceTexture, const int32 SourceUv,
		const FString& Operation, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		if (!SourceTexture.bHadTexture)
		{
			Fail(Out, Error, TEXT("mesh_paint_texture_copy_source_missing"), TEXT("The source has no Mesh Paint texture."));
			return false;
		}
		FComponentSnapshot Before;
		if (!CaptureComponent(Target, Before, Out, Error)) return false;
		const uint32 BeforeHash = SnapshotHash(Before.Texture);
		FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureCopy", "SOMOLMCP Copy Mesh Paint Texture"));
		Target.Modify();
		Target.bEnableTextureColorMeshPainting = true;
		Target.bOverrideMeshPaintTextureCoordinateIndex = true;
		Target.OverriddenMeshPaintTextureCoordinateIndex = SourceUv;
		Target.bOverrideMeshPaintTextureResolution = true;
		Target.OverriddenMeshPaintTextureResolution = SourceTexture.SizeX;
		if (!RestoreTexture(Target, SourceTexture, Out, Error))
			return RollbackAndFail(Transaction, Target, Before, Out, Error,
				TEXT("mesh_paint_texture_copy_failed"), TEXT("Failed to copy Mesh Paint texture data."));
		Target.MarkRenderStateDirty();
		uint32 AfterHash = 0;
		if (!CurrentHash(Target, AfterHash, Out, Error) || AfterHash != SnapshotHash(SourceTexture))
			return RollbackAndFail(Transaction, Target, Before, Out, Error,
				TEXT("mesh_paint_texture_copy_readback_failed"), TEXT("Copied texture bytes did not round-trip."));
		if (!SaveMap(Context, Target, Out, Error))
			return RollbackAndFail(Transaction, Target, Before, Out, Error,
				TEXT("mesh_paint_texture_copy_save_failed"), TEXT("Failed to save copied Mesh Paint texture."));
		CompleteWrite(Target, Operation, BeforeHash, AfterHash, Out);
		Summary = FString::Printf(TEXT("Copied Mesh Paint texture to %s.%s."),
			Target.GetOwner() ? *Target.GetOwner()->GetActorLabel() : TEXT("<none>"), *Target.GetName());
		return true;
	}
#endif
}

void RegisterMeshPaintTextureTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_MESHPAINT_AVAILABLE
	using namespace MeshPaintTextureTools;

	Registry.Register({TEXT("mesh_paint_texture_config_inspect"),
		TEXT("Inspect UE 5.8 Mesh Paint texture enablement, UV override, resolution override, and live texture dimensions."),
		ComponentSchema(),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetSubsystem(Out, Error) : nullptr;
			if (!Component || !Subsystem) return false;
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetNumberField(TEXT("available_uv_channels"), Subsystem->GetNumberOfUVs(Component, 0));
			WriteComponentInfo(*Component, Out);
			Summary = FString::Printf(TEXT("Inspected Mesh Paint texture configuration on %s."), *Component->GetName());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_texture_config_set"),
		TEXT("Set UE 5.8 Mesh Paint texture UV/resolution overrides with transactional recreation, save, rollback, and readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional component name."), {}, 1, 256)},
			{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable texture color mesh painting."))},
			{TEXT("uv_channel"), FSololmcpSchemaBuilder::Integer(TEXT("Texture coordinate index."), 0, 7)},
			{TEXT("resolution"), FSololmcpSchemaBuilder::Integer(TEXT("Power-of-two texture size."), 64, 4096)},
			{TEXT("recreate_texture"), FSololmcpSchemaBuilder::Boolean(TEXT("Explicitly recreate an existing texture if resolution changes."))},
		}, {TEXT("actor_label"), TEXT("enabled"), TEXT("uv_channel"), TEXT("resolution")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetSubsystem(Out, Error) : nullptr;
			if (!Component || !Subsystem) return false;
			const int32 Uv = static_cast<int32>(Args->GetNumberField(TEXT("uv_channel")));
			const int32 Resolution = static_cast<int32>(Args->GetNumberField(TEXT("resolution")));
			const bool bEnabled = Args->GetBoolField(TEXT("enabled"));
			const bool bRecreate = Args->HasField(TEXT("recreate_texture")) && Args->GetBoolField(TEXT("recreate_texture"));
			const int32 UvCount = Subsystem->GetNumberOfUVs(Component, 0);
			if (Uv >= UvCount)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_uv_out_of_range"),
					FString::Printf(TEXT("UV channel %d is outside the component's %d channels."), Uv, UvCount));
				return false;
			}
			if (!FMath::IsPowerOfTwo(Resolution) || MeshPaintVirtualTexture::GetAlignedTextureSize(Resolution) != static_cast<uint32>(Resolution))
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_resolution_invalid"),
					TEXT("resolution must be a MeshPaintVirtualTexture-aligned power of two between 64 and 4096."));
				return false;
			}
			UTexture2D* Existing = Cast<UTexture2D>(Component->GetMeshPaintTexture());
			if (Existing && Existing->Source.GetSizeX() != Resolution && !bRecreate)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_recreate_required"),
					TEXT("An existing texture has another resolution; set recreate_texture=true to replace it explicitly."));
				return false;
			}
			FComponentSnapshot Before;
			if (!CaptureComponent(*Component, Before, Out, Error)) return false;
			const uint32 BeforeHash = SnapshotHash(Before.Texture);
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureConfig", "SOMOLMCP Configure Mesh Paint Texture"));
			Component->Modify();
			Component->bEnableTextureColorMeshPainting = bEnabled;
			Component->bOverrideMeshPaintTextureCoordinateIndex = true;
			Component->OverriddenMeshPaintTextureCoordinateIndex = Uv;
			Component->bOverrideMeshPaintTextureResolution = true;
			Component->OverriddenMeshPaintTextureResolution = Resolution;
			if (Existing && Existing->Source.GetSizeX() != Resolution)
			{
				Subsystem->RemoveComponentMeshPaintTexture(Component);
				Subsystem->CreateComponentMeshPaintTexture(Component);
			}
			Component->MarkRenderStateDirty();
			if (Component->GetMeshPaintTextureCoordinateIndex() != Uv || Component->GetMeshPaintTextureResolution() != Resolution ||
				Component->bEnableTextureColorMeshPainting != bEnabled)
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_config_readback_failed"), TEXT("Configuration readback differs from requested values."));
			uint32 AfterHash = 0;
			if (!CurrentHash(*Component, AfterHash, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_config_hash_failed"), TEXT("Could not read back the configured texture."));
			if (!SaveMap(Context, *Component, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_config_save_failed"), TEXT("Failed to save Mesh Paint texture configuration."));
			CompleteWrite(*Component, TEXT("texture_config_set"), BeforeHash, AfterHash, Out);
			Summary = FString::Printf(TEXT("Configured %s UV %d at %dx%d."), *Component->GetName(), Uv, Resolution, Resolution);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_resource_stats"),
		TEXT("Read source mip bytes, GPU resource estimate, dimensions, format, and content hash for a Mesh Paint texture."),
		ComponentSchema(),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetSubsystem(Out, Error) : nullptr;
			UTexture2D* Texture = Component ? GetTexture(*Component, Out, Error) : nullptr;
			if (!Component || !Subsystem || !Texture) return false;
			FTextureSourceSnapshot Snapshot;
			if (!CaptureTexture(Texture, Snapshot, Out, Error)) return false;
			int64 SourceBytes = 0;
			TArray<TSharedPtr<FJsonValue>> Mips;
			for (int32 Index = 0; Index < Snapshot.Mips.Num(); ++Index)
			{
				SourceBytes += Snapshot.Mips[Index].Num();
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("mip_index"), Index);
				Entry->SetNumberField(TEXT("bytes"), static_cast<double>(Snapshot.Mips[Index].Num()));
				Mips.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetNumberField(TEXT("source_bytes"), static_cast<double>(SourceBytes));
			Out->SetNumberField(TEXT("resource_bytes"), Subsystem->GetMeshPaintTextureResourceSize(Component));
			Out->SetNumberField(TEXT("content_hash"), SnapshotHash(Snapshot));
			Out->SetArrayField(TEXT("mips"), Mips);
			WriteComponentInfo(*Component, Out);
			Summary = FString::Printf(TEXT("Mesh Paint texture uses %lld source bytes across %d mips."), SourceBytes, Snapshot.NumMips);
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_texture_fill"),
		TEXT("Fill every source mip of an existing UE 5.8 Mesh Paint texture with one RGBA color, then save and verify."),
		FillSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UTexture2D* Texture = Component ? GetTexture(*Component, Out, Error) : nullptr;
			if (!Component || !Texture) return false;
			FComponentSnapshot Before;
			if (!CaptureComponent(*Component, Before, Out, Error)) return false;
			const uint32 BeforeHash = SnapshotHash(Before.Texture);
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureFill", "SOMOLMCP Fill Mesh Paint Texture"));
			const FColor Color = ReadColor(Args);
			if (!FillTexture(*Texture, Color, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_fill_failed"), TEXT("Failed to fill the Mesh Paint texture."));
			uint32 AfterHash = 0;
			if (!CurrentHash(*Component, AfterHash, Out, Error) || (BeforeHash == AfterHash && Color != FColor::White))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_fill_readback_failed"), TEXT("Texture fill was not observable in source readback."));
			if (!SaveMap(Context, *Component, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_fill_save_failed"), TEXT("Failed to save the filled Mesh Paint texture."));
			CompleteWrite(*Component, TEXT("texture_fill"), BeforeHash, AfterHash, Out);
			Out->SetStringField(TEXT("fill_color"), Color.ToHex());
			Summary = FString::Printf(TEXT("Filled Mesh Paint texture on %s with %s."), *Component->GetName(), *Color.ToHex());
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_clear"),
		TEXT("Clear every source mip to transparent black without removing the texture, with rollback, save, and readback."),
		ComponentSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UTexture2D* Texture = Component ? GetTexture(*Component, Out, Error) : nullptr;
			if (!Component || !Texture) return false;
			FComponentSnapshot Before;
			if (!CaptureComponent(*Component, Before, Out, Error)) return false;
			const uint32 BeforeHash = SnapshotHash(Before.Texture);
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureClear", "SOMOLMCP Clear Mesh Paint Texture"));
			if (!FillTexture(*Texture, FColor(0, 0, 0, 0), Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_clear_failed"), TEXT("Failed to clear the Mesh Paint texture."));
			FTextureSourceSnapshot Readback;
			if (!CaptureTexture(Texture, Readback, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_clear_readback_failed"), TEXT("Could not read back the cleared texture."));
			for (const TArray64<uint8>& Mip : Readback.Mips)
			{
				for (const uint8 Byte : Mip)
				{
					if (Byte != 0)
						return RollbackAndFail(Transaction, *Component, Before, Out, Error,
							TEXT("mesh_paint_texture_clear_readback_failed"), TEXT("Non-zero bytes remain after clear."));
				}
			}
			if (!SaveMap(Context, *Component, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_clear_save_failed"), TEXT("Failed to save the cleared Mesh Paint texture."));
			CompleteWrite(*Component, TEXT("texture_clear"), BeforeHash, SnapshotHash(Readback), Out);
			Summary = FString::Printf(TEXT("Cleared Mesh Paint texture on %s."), *Component->GetName());
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_channel_histogram_inspect"),
		TEXT("Inspect the native BGRA8 source distribution of every Mesh Paint texture channel without mutation."),
		ComponentSchema(),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UTexture2D* Texture = Component ? GetTexture(*Component, Out, Error) : nullptr;
			if (!Component || !Texture) return false;
			FTextureSourceSnapshot Snapshot;
			if (!CaptureTexture(Texture, Snapshot, Out, Error)) return false;
			if (Snapshot.Format != TSF_BGRA8 || Snapshot.Mips.IsEmpty() || Snapshot.Mips[0].Num() % 4 != 0)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_histogram_format_unsupported"),
					TEXT("Channel histogram requires a TSF_BGRA8 source texture."));
				return false;
			}
			const TCHAR* Names[4] = {TEXT("b"), TEXT("g"), TEXT("r"), TEXT("a")};
			TSharedRef<FJsonObject> Histograms = MakeShared<FJsonObject>();
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				int64 Bins[16] = {};
				int64 Sum = 0;
				for (int64 Offset = Channel; Offset < Snapshot.Mips[0].Num(); Offset += 4)
				{
					const uint8 Value = Snapshot.Mips[0][Offset];
					++Bins[Value / 16];
					Sum += Value;
				}
				TSharedRef<FJsonObject> ChannelData = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> BinValues;
				for (const int64 Bin : Bins) BinValues.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Bin)));
				ChannelData->SetArrayField(TEXT("bins_16"), BinValues);
				ChannelData->SetNumberField(TEXT("mean"), static_cast<double>(Sum) / (Snapshot.Mips[0].Num() / 4));
				Histograms->SetObjectField(Names[Channel], ChannelData);
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetObjectField(TEXT("histograms"), Histograms);
			Out->SetNumberField(TEXT("pixel_count"), static_cast<double>(Snapshot.Mips[0].Num() / 4));
			Out->SetNumberField(TEXT("content_hash"), SnapshotHash(Snapshot));
			WriteComponentInfo(*Component, Out);
			Summary = TEXT("Inspected native Mesh Paint texture channel histograms.");
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_texture_blur"),
		TEXT("Apply a native bounded box blur to every Mesh Paint texture mip with transaction, rollback, save, and readback."),
		FilterSchema(false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UTexture2D* Texture = Component ? GetTexture(*Component, Out, Error) : nullptr;
			if (!Component || !Texture) return false;
			FComponentSnapshot Before;
			if (!CaptureComponent(*Component, Before, Out, Error)) return false;
			const uint32 BeforeHash = SnapshotHash(Before.Texture);
			const int32 Radius = static_cast<int32>(Args->GetNumberField(TEXT("radius")));
			const int32 Iterations = static_cast<int32>(Args->GetNumberField(TEXT("iterations")));
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureBlur", "SOMOLMCP Blur Mesh Paint Texture"));
			if (!BlurTexture(*Texture, Radius, Iterations, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_blur_failed"), TEXT("Native texture blur failed."));
			uint32 AfterHash = 0;
			if (!CurrentHash(*Component, AfterHash, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_blur_readback_failed"), TEXT("Blurred texture could not be read back."));
			if (!SaveMap(Context, *Component, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_blur_save_failed"), TEXT("Failed to save blurred Mesh Paint texture."));
			CompleteWrite(*Component, TEXT("texture_blur"), BeforeHash, AfterHash, Out);
			Out->SetNumberField(TEXT("radius"), Radius);
			Out->SetNumberField(TEXT("iterations"), Iterations);
			Summary = FString::Printf(TEXT("Blurred Mesh Paint texture with radius %d for %d iterations."), Radius, Iterations);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_channel_remap"),
		TEXT("Copy or invert one native BGRA8 Mesh Paint texture channel into another with rollback and readback."),
		FilterSchema(true),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UTexture2D* Texture = Component ? GetTexture(*Component, Out, Error) : nullptr;
			if (!Component || !Texture) return false;
			FComponentSnapshot Before;
			if (!CaptureComponent(*Component, Before, Out, Error)) return false;
			if (Before.Texture.Format != TSF_BGRA8)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_remap_format_unsupported"), TEXT("Channel remap requires TSF_BGRA8 source data."));
				return false;
			}
			const FString SourceName = Args->GetStringField(TEXT("source_channel"));
			const FString TargetName = Args->GetStringField(TEXT("target_channel"));
			const bool bInvert = Args->HasField(TEXT("invert")) && Args->GetBoolField(TEXT("invert"));
			const int32 SourceChannel = ChannelOffset(SourceName);
			const int32 TargetChannel = ChannelOffset(TargetName);
			const uint32 BeforeHash = SnapshotHash(Before.Texture);
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureRemap", "SOMOLMCP Remap Mesh Paint Texture Channel"));
			Texture->Modify();
			Texture->PreEditChange(nullptr);
			for (int32 MipIndex = 0; MipIndex < Texture->Source.GetNumMips(); ++MipIndex)
			{
				const int64 Bytes = Texture->Source.CalcMipSize(0, 0, MipIndex);
				uint8* Data = Texture->Source.LockMip(MipIndex);
				if (!Data || Bytes <= 0 || Bytes % 4 != 0)
				{
					if (Data) Texture->Source.UnlockMip(MipIndex);
					Texture->PostEditChange();
					return RollbackAndFail(Transaction, *Component, Before, Out, Error,
						TEXT("mesh_paint_texture_remap_lock_failed"), TEXT("Failed to edit a source mip."));
				}
				for (int64 Offset = 0; Offset < Bytes; Offset += 4)
				{
					const uint8 Value = Data[Offset + SourceChannel];
					Data[Offset + TargetChannel] = bInvert ? 255 - Value : Value;
				}
				Texture->Source.UnlockMip(MipIndex);
			}
			Texture->PostEditChange();
			Texture->UpdateResource();
			uint32 AfterHash = 0;
			if (!CurrentHash(*Component, AfterHash, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_remap_readback_failed"), TEXT("Remapped texture could not be read back."));
			if (!SaveMap(Context, *Component, Out, Error))
				return RollbackAndFail(Transaction, *Component, Before, Out, Error,
					TEXT("mesh_paint_texture_remap_save_failed"), TEXT("Failed to save remapped Mesh Paint texture."));
			CompleteWrite(*Component, TEXT("texture_channel_remap"), BeforeHash, AfterHash, Out);
			Out->SetStringField(TEXT("source_channel"), SourceName);
			Out->SetStringField(TEXT("target_channel"), TargetName);
			Out->SetBoolField(TEXT("invert"), bInvert);
			Summary = FString::Printf(TEXT("Remapped Mesh Paint texture channel %s to %s."), *SourceName, *TargetName);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_copy_component"),
		TEXT("Copy the complete Mesh Paint texture mip chain and UV configuration from one component to another."),
		CopyComponentSchema(),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Source = FindComponent(Args, Out, Error, TEXT("source_"));
			UStaticMeshComponent* Target = Source ? FindComponent(Args, Out, Error, TEXT("target_")) : nullptr;
			if (!Source || !Target) return false;
			if (Source == Target)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_same_component"), TEXT("Source and target components must differ."));
				return false;
			}
			FTextureSourceSnapshot SourceTexture;
			if (!CaptureTexture(Cast<UTexture2D>(Source->GetMeshPaintTexture()), SourceTexture, Out, Error)) return false;
			return CopySnapshotToTarget(Context, *Target, SourceTexture, Source->GetMeshPaintTextureCoordinateIndex(),
				TEXT("texture_copy_component"), Out, Summary, Error);
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_duplicate_to_component"),
		TEXT("Duplicate a single-layer BGRA8 texture asset into a component-owned Mesh Paint texture and persist it in the map."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("source_texture_path"), FSololmcpSchemaBuilder::String(TEXT("Texture2D object path."), {}, 1, 1024)},
			{TEXT("target_actor_label"), FSololmcpSchemaBuilder::String(TEXT("Target Actor label."), {}, 1, 256)},
			{TEXT("target_component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional target component name."), {}, 1, 256)},
			{TEXT("uv_channel"), FSololmcpSchemaBuilder::Integer(TEXT("Target UV channel."), 0, 7)},
		}, {TEXT("source_texture_path"), TEXT("target_actor_label"), TEXT("uv_channel")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Target = FindComponent(Args, Out, Error, TEXT("target_"));
			if (!Target) return false;
			const FString Path = Args->GetStringField(TEXT("source_texture_path"));
			UTexture2D* SourceTextureAsset = Cast<UTexture2D>(FSoftObjectPath(Path).TryLoad());
			if (!SourceTextureAsset)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_asset_not_found"),
					FString::Printf(TEXT("Texture2D '%s' could not be loaded."), *Path));
				return false;
			}
			FTextureSourceSnapshot SourceTexture;
			if (!CaptureTexture(SourceTextureAsset, SourceTexture, Out, Error)) return false;
			return CopySnapshotToTarget(Context, *Target, SourceTexture,
				static_cast<int32>(Args->GetNumberField(TEXT("uv_channel"))),
				TEXT("texture_duplicate_to_component"), Out, Summary, Error);
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_session_begin"),
		TEXT("Begin an in-memory Mesh Paint texture session with a complete configuration and full-mip rollback snapshot."),
		ComponentSchema(),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			if (!Component) return false;
			FStoredSnapshot State;
			State.Id = FString::Printf(TEXT("mpts_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(20).ToLower());
			State.Component = Component;
			if (!CaptureComponent(*Component, State.Snapshot, Out, Error)) return false;
			State.BeginHash = SnapshotHash(State.Snapshot.Texture);
			{
				FScopeLock Lock(&GSnapshotMutex);
				GSessions.Add(State.Id, State);
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("session_id"), State.Id);
			Out->SetNumberField(TEXT("begin_hash"), State.BeginHash);
			Out->SetBoolField(TEXT("rollback_available"), true);
			WriteComponentInfo(*Component, Out);
			Summary = FString::Printf(TEXT("Started Mesh Paint texture session %s."), *State.Id);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_session_commit"),
		TEXT("Commit an active Mesh Paint texture session by saving the map and recording final full-mip readback."),
		SessionSchema(TEXT("session_id"), TEXT("Active Mesh Paint texture session id.")),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const FString Id = Args->GetStringField(TEXT("session_id"));
			FStoredSnapshot State;
			{
				FScopeLock Lock(&GSnapshotMutex);
				const FStoredSnapshot* Found = GSessions.Find(Id);
				if (!Found)
				{
					Fail(Out, Error, TEXT("mesh_paint_texture_session_not_found"), TEXT("The session does not exist or is already closed."));
					return false;
				}
				State = *Found;
			}
			UStaticMeshComponent* Component = State.Component.Get();
			if (!Component)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_session_target_lost"), TEXT("The session component no longer exists."));
				return false;
			}
			uint32 AfterHash = 0;
			if (!CurrentHash(*Component, AfterHash, Out, Error)) return false;
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureCommit", "SOMOLMCP Commit Mesh Paint Texture Session"));
			Component->Modify();
			if (!SaveMap(Context, *Component, Out, Error))
			{
				Transaction.Cancel();
				return false;
			}
			{
				FScopeLock Lock(&GSnapshotMutex);
				GSessions.Remove(Id);
			}
			CompleteWrite(*Component, TEXT("texture_session_commit"), State.BeginHash, AfterHash, Out);
			Out->SetStringField(TEXT("session_id"), Id);
			Summary = FString::Printf(TEXT("Committed Mesh Paint texture session %s."), *Id);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_session_cancel"),
		TEXT("Cancel a Mesh Paint texture session, restore its complete snapshot, save the rollback, and verify its hash."),
		SessionSchema(TEXT("session_id"), TEXT("Active Mesh Paint texture session id.")),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const FString Id = Args->GetStringField(TEXT("session_id"));
			FStoredSnapshot State;
			{
				FScopeLock Lock(&GSnapshotMutex);
				const FStoredSnapshot* Found = GSessions.Find(Id);
				if (!Found)
				{
					Fail(Out, Error, TEXT("mesh_paint_texture_session_not_found"), TEXT("The session does not exist or is already closed."));
					return false;
				}
				State = *Found;
			}
			UStaticMeshComponent* Component = State.Component.Get();
			if (!Component)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_session_target_lost"), TEXT("The session component no longer exists."));
				return false;
			}
			uint32 BeforeCancelHash = 0;
			if (!CurrentHash(*Component, BeforeCancelHash, Out, Error)) return false;
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureCancel", "SOMOLMCP Cancel Mesh Paint Texture Session"));
			if (!RestoreComponent(*Component, State.Snapshot, Out, Error))
			{
				Transaction.Cancel();
				return false;
			}
			uint32 RestoredHash = 0;
			if (!CurrentHash(*Component, RestoredHash, Out, Error) || RestoredHash != State.BeginHash)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_texture_session_cancel_readback_failed"), TEXT("Cancelled session did not restore its begin hash."));
				return false;
			}
			if (!SaveMap(Context, *Component, Out, Error))
			{
				Transaction.Cancel();
				return false;
			}
			{
				FScopeLock Lock(&GSnapshotMutex);
				GSessions.Remove(Id);
			}
			CompleteWrite(*Component, TEXT("texture_session_cancel"), BeforeCancelHash, RestoredHash, Out);
			Out->SetStringField(TEXT("session_id"), Id);
			Out->SetBoolField(TEXT("rollback_applied"), true);
			Summary = FString::Printf(TEXT("Cancelled and restored Mesh Paint texture session %s."), *Id);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_backup_create"),
		TEXT("Create a reusable in-memory full-mip Mesh Paint texture and component-configuration backup."),
		ComponentSchema(),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			if (!Component) return false;
			FStoredSnapshot State;
			State.Id = FString::Printf(TEXT("mptb_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(20).ToLower());
			State.Component = Component;
			if (!CaptureComponent(*Component, State.Snapshot, Out, Error)) return false;
			State.BeginHash = SnapshotHash(State.Snapshot.Texture);
			{
				FScopeLock Lock(&GSnapshotMutex);
				GBackups.Add(State.Id, State);
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetStringField(TEXT("backup_id"), State.Id);
			Out->SetNumberField(TEXT("backup_hash"), State.BeginHash);
			Summary = FString::Printf(TEXT("Created Mesh Paint texture backup %s."), *State.Id);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_backup_restore"),
		TEXT("Restore a reusable Mesh Paint texture backup to its original component, save it, and verify all source mips."),
		SessionSchema(TEXT("backup_id"), TEXT("Mesh Paint texture backup id.")),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			const FString Id = Args->GetStringField(TEXT("backup_id"));
			FStoredSnapshot State;
			{
				FScopeLock Lock(&GSnapshotMutex);
				const FStoredSnapshot* Found = GBackups.Find(Id);
				if (!Found)
				{
					Fail(Out, Error, TEXT("mesh_paint_texture_backup_not_found"), TEXT("The backup id does not exist."));
					return false;
				}
				State = *Found;
			}
			UStaticMeshComponent* Component = State.Component.Get();
			if (!Component)
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_backup_target_lost"), TEXT("The backup component no longer exists."));
				return false;
			}
			uint32 BeforeHash = 0;
			if (!CurrentHash(*Component, BeforeHash, Out, Error)) return false;
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureBackupRestore", "SOMOLMCP Restore Mesh Paint Texture Backup"));
			if (!RestoreComponent(*Component, State.Snapshot, Out, Error))
			{
				Transaction.Cancel();
				return false;
			}
			uint32 RestoredHash = 0;
			if (!CurrentHash(*Component, RestoredHash, Out, Error) || RestoredHash != State.BeginHash)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_texture_backup_restore_readback_failed"), TEXT("Restored backup hash does not match."));
				return false;
			}
			if (!SaveMap(Context, *Component, Out, Error))
			{
				Transaction.Cancel();
				return false;
			}
			CompleteWrite(*Component, TEXT("texture_backup_restore"), BeforeHash, RestoredHash, Out);
			Out->SetStringField(TEXT("backup_id"), Id);
			Out->SetBoolField(TEXT("rollback_available"), true);
			Summary = FString::Printf(TEXT("Restored Mesh Paint texture backup %s."), *Id);
			return true;
		}, nullptr, 2});
#else
	(void)Registry;
#endif
}
}
