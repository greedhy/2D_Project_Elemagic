// Copyright 2026 SOMOLAGENT. All Rights Reserved.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "EngineUtils.h"
#include "Misc/Crc.h"
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
#endif

namespace UE::SOMOLMCP
{
namespace MeshPaintExtended
{
	// Writers in this unit only succeed after deterministic readback and owner-map persistence.
	static void Fail(TSharedRef<FJsonObject>& Out, FString& Error, const FString& Code, const FString& Message)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("status"), TEXT("failed"));
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("reason_code"), Code);
		Out->SetStringField(TEXT("message"), Message);
		Error = Message;
	}

	static TSharedRef<FJsonObject> ComponentSchema(const bool bRequireLod = false)
	{
		TMap<FString, TSharedRef<FJsonObject>> Properties = {
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional exact StaticMeshComponent name."), {}, 1, 256)},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer(TEXT("Static Mesh LOD index."), 0, 63)},
		};
		TArray<FString> Required = {TEXT("actor_label")};
		if (bRequireLod) Required.Add(TEXT("lod_index"));
		return FSololmcpSchemaBuilder::Object(Properties, Required, FString(), false);
	}

#if SOMOLMCP_MESHPAINT_AVAILABLE
	static UMeshPaintingSubsystem* GetMeshPainting(TSharedRef<FJsonObject>& Out, FString& Error)
	{
		UMeshPaintingSubsystem* Subsystem = GEngine
			? GEngine->GetEngineSubsystem<UMeshPaintingSubsystem>() : nullptr;
		if (!Subsystem)
		{
			Fail(Out, Error, TEXT("mesh_paint_subsystem_unavailable"),
				TEXT("UE 5.8 MeshPainting subsystem is unavailable."));
		}
		return Subsystem;
	}

	static UStaticMeshComponent* FindComponentByName(const FString& ActorLabel, const FString& ComponentName,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Fail(Out, Error, TEXT("editor_world_unavailable"), TEXT("No editor world is available."));
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || (Actor->GetActorLabel() != ActorLabel && Actor->GetName() != ActorLabel)) continue;
			TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
			for (UStaticMeshComponent* Component : Components)
			{
				if (Component && Component->GetStaticMesh() &&
					(ComponentName.IsEmpty() || Component->GetName() == ComponentName))
				{
					return Component;
				}
			}
			Fail(Out, Error, TEXT("static_mesh_component_not_found"),
				FString::Printf(TEXT("Actor '%s' has no matching StaticMeshComponent."), *ActorLabel));
			return nullptr;
		}
		Fail(Out, Error, TEXT("actor_not_found"), FString::Printf(TEXT("Actor '%s' was not found."), *ActorLabel));
		return nullptr;
	}

	static UStaticMeshComponent* FindComponent(const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		FString ComponentName;
		Args->TryGetStringField(TEXT("component_name"), ComponentName);
		return FindComponentByName(Args->GetStringField(TEXT("actor_label")), ComponentName, Out, Error);
	}

	static bool ValidateLod(const UStaticMeshComponent& Component, const int32 Lod,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		const UStaticMesh* Mesh = Component.GetStaticMesh();
		const int32 LodCount = Mesh ? Mesh->GetNumLODs() : 0;
		if (Lod < 0 || Lod >= LodCount)
		{
			Fail(Out, Error, TEXT("invalid_lod_index"),
				FString::Printf(TEXT("lod_index %d is outside [0, %d)."), Lod, LodCount));
			return false;
		}
		return true;
	}

	static uint32 ColorHash(const TArray<FColor>& Colors)
	{
		return Colors.IsEmpty() ? 0u : FCrc::MemCrc32(Colors.GetData(), Colors.Num() * sizeof(FColor));
	}

	static FString HashText(const TArray<FColor>& Colors)
	{
		return FString::Printf(TEXT("%08x"), ColorHash(Colors));
	}

	static void WriteColorStats(const TArray<FColor>& Colors, TSharedRef<FJsonObject>& Out)
	{
		Out->SetNumberField(TEXT("vertex_count"), Colors.Num());
		Out->SetStringField(TEXT("color_hash"), HashText(Colors));
		if (Colors.IsEmpty()) return;
		uint8 Min[4] = {255, 255, 255, 255};
		uint8 Max[4] = {0, 0, 0, 0};
		uint64 Sum[4] = {0, 0, 0, 0};
		for (const FColor& Color : Colors)
		{
			const uint8 Values[4] = {Color.R, Color.G, Color.B, Color.A};
			for (int32 Channel = 0; Channel < 4; ++Channel)
			{
				Min[Channel] = FMath::Min(Min[Channel], Values[Channel]);
				Max[Channel] = FMath::Max(Max[Channel], Values[Channel]);
				Sum[Channel] += Values[Channel];
			}
		}
		static const TCHAR* Names[4] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
		TSharedRef<FJsonObject> Channels = MakeShared<FJsonObject>();
		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
			Stats->SetNumberField(TEXT("min"), Min[Channel]);
			Stats->SetNumberField(TEXT("max"), Max[Channel]);
			Stats->SetNumberField(TEXT("average"), static_cast<double>(Sum[Channel]) / Colors.Num());
			Channels->SetObjectField(Names[Channel], Stats);
		}
		Out->SetObjectField(TEXT("channels"), Channels);
	}

	static TArray<FColor> GetOrCreateInstanceColors(UMeshPaintingSubsystem& Subsystem,
		UStaticMeshComponent& Component, const int32 Lod)
	{
		TArray<FColor> Colors = Subsystem.GetInstanceColorDataForLOD(&Component, Lod);
		if (!Colors.IsEmpty()) return Colors;
		const int32 VertexCount = Subsystem.GetVerticesForLOD(Component.GetStaticMesh(), Lod).Num();
		TArray<FColor> AssetColors = Subsystem.GetColorDataForLOD(Component.GetStaticMesh(), Lod);
		if (AssetColors.Num() == VertexCount) return AssetColors;
		Colors.Init(FColor::White, VertexCount);
		return Colors;
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
			Fail(Out, Error, TEXT("owner_map_path_unavailable"), TEXT("The component has no saveable owner map."));
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

	static void CompleteWrite(TSharedRef<FJsonObject>& Out, const FString& Prefix,
		const UStaticMeshComponent& Component)
	{
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("status"), TEXT("succeeded"));
		Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("%s_%s"), *Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower()));
		Out->SetStringField(TEXT("actor_label"), Component.GetOwner()->GetActorLabel());
		Out->SetStringField(TEXT("component_name"), Component.GetName());
		Out->SetBoolField(TEXT("mutation_applied"), true);
		Out->SetBoolField(TEXT("saved"), true);
		Out->SetBoolField(TEXT("readback_verified"), true);
	}

	static int32 ChannelIndex(const FString& Channel)
	{
		if (Channel.Equals(TEXT("r"), ESearchCase::IgnoreCase)) return 0;
		if (Channel.Equals(TEXT("g"), ESearchCase::IgnoreCase)) return 1;
		if (Channel.Equals(TEXT("b"), ESearchCase::IgnoreCase)) return 2;
		return 3;
	}

	static void SetChannel(FColor& Color, const int32 Channel, const uint8 Value)
	{
		if (Channel == 0) Color.R = Value;
		else if (Channel == 1) Color.G = Value;
		else if (Channel == 2) Color.B = Value;
		else Color.A = Value;
	}

	static TArray<FColor> PropagateNearest(const TArray<FVector>& SourcePositions,
		const TArray<FColor>& SourceColors, const TArray<FVector>& TargetPositions, const int64 MaxPairTests,
		TSharedRef<FJsonObject>& Out, FString& Error)
	{
		TArray<FColor> Result;
		if (SourcePositions.Num() != SourceColors.Num() || SourcePositions.IsEmpty() || TargetPositions.IsEmpty())
		{
			Fail(Out, Error, TEXT("mesh_paint_propagation_source_invalid"),
				TEXT("Source positions/colors or target positions are empty or inconsistent."));
			return Result;
		}
		const int64 PairTests = static_cast<int64>(SourcePositions.Num()) * TargetPositions.Num();
		if (PairTests > MaxPairTests)
		{
			Fail(Out, Error, TEXT("mesh_paint_propagation_budget_exceeded"),
				FString::Printf(TEXT("Nearest-position propagation requires %lld pair tests; maximum is %lld."),
					PairTests, MaxPairTests));
			return Result;
		}
		Result.SetNumUninitialized(TargetPositions.Num());
		for (int32 TargetIndex = 0; TargetIndex < TargetPositions.Num(); ++TargetIndex)
		{
			int32 BestIndex = 0;
			double BestDistance = TNumericLimits<double>::Max();
			for (int32 SourceIndex = 0; SourceIndex < SourcePositions.Num(); ++SourceIndex)
			{
				const double Distance = FVector::DistSquared(TargetPositions[TargetIndex], SourcePositions[SourceIndex]);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					BestIndex = SourceIndex;
				}
			}
			Result[TargetIndex] = SourceColors[BestIndex];
		}
		Out->SetNumberField(TEXT("pair_tests"), static_cast<double>(PairTests));
		Out->SetStringField(TEXT("mapping"), TEXT("nearest_vertex_position"));
		return Result;
	}
#endif
}

void RegisterMeshPaintExtendedTools(FSololmcpToolRegistry& Registry)
{
#if SOMOLMCP_MESHPAINT_AVAILABLE
	using namespace MeshPaintExtended;

	Registry.Register({TEXT("mesh_paint_vertex_channel_inspect"),
		TEXT("Inspect deterministic per-channel RGBA statistics for one component LOD without mutation."), ComponentSchema(true),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			const int32 Lod = static_cast<int32>(Args->GetNumberField(TEXT("lod_index")));
			if (!Component || !Subsystem || !ValidateLod(*Component, Lod, Out, Error)) return false;
			const TArray<FColor> InstanceColors = Subsystem->GetInstanceColorDataForLOD(Component, Lod);
			const TArray<FColor> AssetColors = Subsystem->GetColorDataForLOD(Component->GetStaticMesh(), Lod);
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetNumberField(TEXT("lod_index"), Lod);
			Out->SetBoolField(TEXT("has_instance_override"), !InstanceColors.IsEmpty());
			TSharedRef<FJsonObject> InstanceStats = MakeShared<FJsonObject>();
			WriteColorStats(InstanceColors, InstanceStats);
			Out->SetObjectField(TEXT("instance_colors"), InstanceStats);
			TSharedRef<FJsonObject> AssetStats = MakeShared<FJsonObject>();
			WriteColorStats(AssetColors, AssetStats);
			Out->SetObjectField(TEXT("asset_colors"), AssetStats);
			Summary = FString::Printf(TEXT("Inspected RGBA channels for %s LOD %d."), *Component->GetName(), Lod);
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_vertex_channel_fill"),
		TEXT("Fill exactly one RGBA instance-color channel, save the map, and verify all written values."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional component name."), {}, 1, 256)},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer(TEXT("LOD index."), 0, 63)},
			{TEXT("channel"), FSololmcpSchemaBuilder::String(TEXT("RGBA channel."), {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")})},
			{TEXT("value"), FSololmcpSchemaBuilder::Integer(TEXT("Channel value."), 0, 255)},
		}, {TEXT("actor_label"), TEXT("lod_index"), TEXT("channel"), TEXT("value")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			const int32 Lod = static_cast<int32>(Args->GetNumberField(TEXT("lod_index")));
			if (!Component || !Subsystem || !ValidateLod(*Component, Lod, Out, Error)) return false;
			TArray<FColor> Colors = GetOrCreateInstanceColors(*Subsystem, *Component, Lod);
			if (Colors.IsEmpty())
			{
				Fail(Out, Error, TEXT("mesh_paint_lod_has_no_vertices"), TEXT("The selected LOD has no vertices."));
				return false;
			}
			const int32 Channel = ChannelIndex(Args->GetStringField(TEXT("channel")));
			const uint8 Value = static_cast<uint8>(Args->GetNumberField(TEXT("value")));
			for (FColor& Color : Colors) SetChannel(Color, Channel, Value);
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintChannelFill", "SOMOLMCP Fill Vertex Color Channel"));
			Component->Modify();
			Subsystem->SetInstanceColorDataForLOD(Component, Lod, Colors);
			const TArray<FColor> Readback = Subsystem->GetInstanceColorDataForLOD(Component, Lod);
			if (Readback.Num() != Colors.Num() || ColorHash(Readback) != ColorHash(Colors))
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_channel_readback_mismatch"), TEXT("Vertex channel write did not round-trip exactly."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			CompleteWrite(Out, TEXT("mesh_paint_channel"), *Component);
			Out->SetNumberField(TEXT("lod_index"), Lod);
			Out->SetStringField(TEXT("channel"), Args->GetStringField(TEXT("channel")));
			Out->SetNumberField(TEXT("value"), Value);
			WriteColorStats(Readback, Out);
			Summary = FString::Printf(TEXT("Filled channel %s on %s LOD %d."),
				*Args->GetStringField(TEXT("channel")), *Component->GetName(), Lod);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_weight_target_inspect"),
		TEXT("Inspect UE 5.8 vertex-weight target encodings for two through five blended layers."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("num_weights"), FSololmcpSchemaBuilder::Integer(TEXT("Blend target count."), 2, 5)},
		}, {TEXT("num_weights")}, FString(), false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UMeshPaintingSubsystem* Subsystem = GetMeshPainting(Out, Error);
			if (!Subsystem) return false;
			const int32 NumWeights = static_cast<int32>(Args->GetNumberField(TEXT("num_weights")));
			TArray<TSharedPtr<FJsonValue>> Targets;
			for (int32 Weight = 0; Weight < NumWeights; ++Weight)
			{
				const FLinearColor Linear = Subsystem->GenerateColorForTextureWeight(NumWeights, Weight);
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("weight_index"), Weight);
				Entry->SetStringField(TEXT("encoded_color"), Linear.ToFColor(false).ToHex());
				Targets.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetNumberField(TEXT("num_weights"), NumWeights);
			Out->SetArrayField(TEXT("targets"), Targets);
			Summary = FString::Printf(TEXT("Inspected %d UE vertex-weight encodings."), NumWeights);
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_weight_fill"),
		TEXT("Fill one UE vertex-weight target across a component LOD, then save and verify its exact encoded color."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional component name."), {}, 1, 256)},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer(TEXT("LOD index."), 0, 63)},
			{TEXT("num_weights"), FSololmcpSchemaBuilder::Integer(TEXT("Blend target count."), 2, 5)},
			{TEXT("weight_index"), FSololmcpSchemaBuilder::Integer(TEXT("Target weight index; must be below num_weights."), 0, 4)},
		}, {TEXT("actor_label"), TEXT("lod_index"), TEXT("num_weights"), TEXT("weight_index")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			const int32 Lod = static_cast<int32>(Args->GetNumberField(TEXT("lod_index")));
			if (!Component || !Subsystem || !ValidateLod(*Component, Lod, Out, Error)) return false;
			const int32 NumWeights = static_cast<int32>(Args->GetNumberField(TEXT("num_weights")));
			const int32 WeightIndex = static_cast<int32>(Args->GetNumberField(TEXT("weight_index")));
			if (WeightIndex >= NumWeights)
			{
				Fail(Out, Error, TEXT("invalid_weight_index"), TEXT("weight_index must be less than num_weights."));
				return false;
			}
			const FColor Encoded = Subsystem->GenerateColorForTextureWeight(NumWeights, WeightIndex).ToFColor(false);
			const int32 VertexCount = Subsystem->GetVerticesForLOD(Component->GetStaticMesh(), Lod).Num();
			if (VertexCount <= 0)
			{
				Fail(Out, Error, TEXT("mesh_paint_lod_has_no_vertices"), TEXT("The selected LOD has no vertices."));
				return false;
			}
			TArray<FColor> Colors;
			Colors.Init(Encoded, VertexCount);
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintWeightFill", "SOMOLMCP Fill Vertex Weight"));
			Component->Modify();
			Subsystem->SetInstanceColorDataForLOD(Component, Lod, Colors);
			const TArray<FColor> Readback = Subsystem->GetInstanceColorDataForLOD(Component, Lod);
			if (Readback.Num() != Colors.Num() || ColorHash(Readback) != ColorHash(Colors))
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_weight_readback_mismatch"), TEXT("Weight fill did not round-trip exactly."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			CompleteWrite(Out, TEXT("mesh_paint_weight"), *Component);
			Out->SetNumberField(TEXT("lod_index"), Lod);
			Out->SetNumberField(TEXT("num_weights"), NumWeights);
			Out->SetNumberField(TEXT("weight_index"), WeightIndex);
			Out->SetStringField(TEXT("encoded_color"), Encoded.ToHex());
			WriteColorStats(Readback, Out);
			Summary = FString::Printf(TEXT("Filled weight %d/%d on %s LOD %d."), WeightIndex, NumWeights, *Component->GetName(), Lod);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_lod_copy"),
		TEXT("Copy instance colors between equal-vertex-count LODs, save, and verify the exact target hash."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional component name."), {}, 1, 256)},
			{TEXT("source_lod"), FSololmcpSchemaBuilder::Integer(TEXT("Source LOD."), 0, 63)},
			{TEXT("target_lod"), FSololmcpSchemaBuilder::Integer(TEXT("Target LOD."), 0, 63)},
		}, {TEXT("actor_label"), TEXT("source_lod"), TEXT("target_lod")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			const int32 SourceLod = static_cast<int32>(Args->GetNumberField(TEXT("source_lod")));
			const int32 TargetLod = static_cast<int32>(Args->GetNumberField(TEXT("target_lod")));
			if (!Component || !Subsystem || !ValidateLod(*Component, SourceLod, Out, Error) ||
				!ValidateLod(*Component, TargetLod, Out, Error)) return false;
			if (SourceLod == TargetLod)
			{
				Fail(Out, Error, TEXT("mesh_paint_same_lod"), TEXT("source_lod and target_lod must differ."));
				return false;
			}
			const TArray<FColor> Source = Subsystem->GetInstanceColorDataForLOD(Component, SourceLod);
			const int32 TargetVertices = Subsystem->GetVerticesForLOD(Component->GetStaticMesh(), TargetLod).Num();
			if (Source.IsEmpty() || Source.Num() != TargetVertices)
			{
				Fail(Out, Error, TEXT("mesh_paint_lod_copy_vertex_count_mismatch"),
					FString::Printf(TEXT("Source colors %d and target vertices %d must be equal; use mesh_paint_lod_propagate otherwise."),
						Source.Num(), TargetVertices));
				return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintLodCopy", "SOMOLMCP Copy Vertex Colors Between LODs"));
			Component->Modify();
			Subsystem->SetInstanceColorDataForLOD(Component, TargetLod, Source);
			const TArray<FColor> Readback = Subsystem->GetInstanceColorDataForLOD(Component, TargetLod);
			if (Readback.Num() != Source.Num() || ColorHash(Readback) != ColorHash(Source))
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_lod_copy_readback_mismatch"), TEXT("Target LOD colors differ from the source after copy."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			CompleteWrite(Out, TEXT("mesh_paint_lod_copy"), *Component);
			Out->SetNumberField(TEXT("source_lod"), SourceLod);
			Out->SetNumberField(TEXT("target_lod"), TargetLod);
			Out->SetStringField(TEXT("source_hash"), HashText(Source));
			Out->SetStringField(TEXT("target_hash"), HashText(Readback));
			Summary = FString::Printf(TEXT("Copied vertex colors from LOD %d to LOD %d."), SourceLod, TargetLod);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_lod_propagate"),
		TEXT("Propagate instance colors between different LOD topologies by deterministic nearest vertex position."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_label"), FSololmcpSchemaBuilder::String(TEXT("Exact Actor label or object name."), {}, 1, 256)},
			{TEXT("component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional component name."), {}, 1, 256)},
			{TEXT("source_lod"), FSololmcpSchemaBuilder::Integer(TEXT("Source LOD."), 0, 63)},
			{TEXT("target_lod"), FSololmcpSchemaBuilder::Integer(TEXT("Target LOD."), 0, 63)},
			{TEXT("max_pair_tests"), FSololmcpSchemaBuilder::Integer(TEXT("Safety cap for deterministic nearest-position comparisons."), 1, 50000000)},
		}, {TEXT("actor_label"), TEXT("source_lod"), TEXT("target_lod")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			const int32 SourceLod = static_cast<int32>(Args->GetNumberField(TEXT("source_lod")));
			const int32 TargetLod = static_cast<int32>(Args->GetNumberField(TEXT("target_lod")));
			if (!Component || !Subsystem || !ValidateLod(*Component, SourceLod, Out, Error) ||
				!ValidateLod(*Component, TargetLod, Out, Error)) return false;
			if (SourceLod == TargetLod)
			{
				Fail(Out, Error, TEXT("mesh_paint_same_lod"), TEXT("source_lod and target_lod must differ."));
				return false;
			}
			const TArray<FColor> SourceColors = Subsystem->GetInstanceColorDataForLOD(Component, SourceLod);
			const TArray<FVector> SourcePositions = Subsystem->GetVerticesForLOD(Component->GetStaticMesh(), SourceLod);
			const TArray<FVector> TargetPositions = Subsystem->GetVerticesForLOD(Component->GetStaticMesh(), TargetLod);
			const int64 MaxPairTests = Args->HasField(TEXT("max_pair_tests"))
				? static_cast<int64>(Args->GetNumberField(TEXT("max_pair_tests"))) : 10000000ll;
			const TArray<FColor> TargetColors = PropagateNearest(SourcePositions, SourceColors, TargetPositions, MaxPairTests, Out, Error);
			if (TargetColors.IsEmpty()) return false;
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintLodPropagate", "SOMOLMCP Propagate Vertex Colors Between LODs"));
			Component->Modify();
			Subsystem->SetInstanceColorDataForLOD(Component, TargetLod, TargetColors);
			const TArray<FColor> Readback = Subsystem->GetInstanceColorDataForLOD(Component, TargetLod);
			if (Readback.Num() != TargetColors.Num() || ColorHash(Readback) != ColorHash(TargetColors))
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_lod_propagate_readback_mismatch"), TEXT("Propagated target colors did not round-trip exactly."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			CompleteWrite(Out, TEXT("mesh_paint_lod_propagate"), *Component);
			Out->SetNumberField(TEXT("source_lod"), SourceLod);
			Out->SetNumberField(TEXT("target_lod"), TargetLod);
			Out->SetNumberField(TEXT("source_vertex_count"), SourceColors.Num());
			Out->SetNumberField(TEXT("target_vertex_count"), Readback.Num());
			Out->SetStringField(TEXT("target_hash"), HashText(Readback));
			Summary = FString::Printf(TEXT("Propagated LOD %d colors to LOD %d by nearest position."), SourceLod, TargetLod);
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_component_color_copy"),
		TEXT("Copy exact per-instance colors between compatible StaticMeshComponents and verify the target map save."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("source_actor_label"), FSololmcpSchemaBuilder::String(TEXT("Source Actor label or name."), {}, 1, 256)},
			{TEXT("source_component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional source component name."), {}, 1, 256)},
			{TEXT("target_actor_label"), FSololmcpSchemaBuilder::String(TEXT("Target Actor label or name."), {}, 1, 256)},
			{TEXT("target_component_name"), FSololmcpSchemaBuilder::String(TEXT("Optional target component name."), {}, 1, 256)},
			{TEXT("lod_index"), FSololmcpSchemaBuilder::Integer(TEXT("LOD index copied on both components."), 0, 63)},
		}, {TEXT("source_actor_label"), TEXT("target_actor_label"), TEXT("lod_index")}, FString(), false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString SourceName, TargetName;
			Args->TryGetStringField(TEXT("source_component_name"), SourceName);
			Args->TryGetStringField(TEXT("target_component_name"), TargetName);
			UStaticMeshComponent* Source = FindComponentByName(Args->GetStringField(TEXT("source_actor_label")), SourceName, Out, Error);
			if (!Source) return false;
			UStaticMeshComponent* Target = FindComponentByName(Args->GetStringField(TEXT("target_actor_label")), TargetName, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Target ? GetMeshPainting(Out, Error) : nullptr;
			const int32 Lod = static_cast<int32>(Args->GetNumberField(TEXT("lod_index")));
			if (!Target || !Subsystem || !ValidateLod(*Source, Lod, Out, Error) || !ValidateLod(*Target, Lod, Out, Error)) return false;
			if (Source == Target)
			{
				Fail(Out, Error, TEXT("mesh_paint_same_component"), TEXT("Source and target components must differ."));
				return false;
			}
			const TArray<FColor> SourceColors = Subsystem->GetInstanceColorDataForLOD(Source, Lod);
			const int32 TargetVertices = Subsystem->GetVerticesForLOD(Target->GetStaticMesh(), Lod).Num();
			if (SourceColors.IsEmpty() || SourceColors.Num() != TargetVertices)
			{
				Fail(Out, Error, TEXT("mesh_paint_component_copy_incompatible"),
					FString::Printf(TEXT("Source colors %d and target vertices %d must match exactly."), SourceColors.Num(), TargetVertices));
				return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintComponentCopy", "SOMOLMCP Copy Component Vertex Colors"));
			Target->Modify();
			Subsystem->SetInstanceColorDataForLOD(Target, Lod, SourceColors);
			const TArray<FColor> Readback = Subsystem->GetInstanceColorDataForLOD(Target, Lod);
			if (Readback.Num() != SourceColors.Num() || ColorHash(Readback) != ColorHash(SourceColors))
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_component_copy_readback_mismatch"), TEXT("Target colors differ from source after copy."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Target, Out, Error)) return false;
			CompleteWrite(Out, TEXT("mesh_paint_component_copy"), *Target);
			Out->SetStringField(TEXT("source_component"), Source->GetPathName());
			Out->SetStringField(TEXT("target_component"), Target->GetPathName());
			Out->SetNumberField(TEXT("lod_index"), Lod);
			Out->SetStringField(TEXT("source_hash"), HashText(SourceColors));
			Out->SetStringField(TEXT("target_hash"), HashText(Readback));
			Summary = FString::Printf(TEXT("Copied LOD %d vertex colors from %s to %s."), Lod, *Source->GetName(), *Target->GetName());
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_uv_target_inspect"),
		TEXT("Inspect paintable UV-channel counts, active mesh-paint UV index, and texture resolution by LOD."), ComponentSchema(false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			if (!Component || !Subsystem) return false;
			TArray<TSharedPtr<FJsonValue>> Lods;
			for (int32 Lod = 0; Lod < Component->GetStaticMesh()->GetNumLODs(); ++Lod)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("lod_index"), Lod);
				Entry->SetNumberField(TEXT("uv_channel_count"), Subsystem->GetNumberOfUVs(Component, Lod));
				Lods.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetNumberField(TEXT("mesh_paint_uv_channel"), Component->GetMeshPaintTextureCoordinateIndex());
			Out->SetNumberField(TEXT("mesh_paint_texture_resolution"), Component->GetMeshPaintTextureResolution());
			Out->SetArrayField(TEXT("lods"), Lods);
			Summary = FString::Printf(TEXT("Inspected mesh-paint UV targets for %s."), *Component->GetName());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_texture_target_inspect"),
		TEXT("Inspect UE 5.8 component texture-paint capability, assigned texture, UV target, resolution, and resource bytes."), ComponentSchema(false),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			if (!Component || !Subsystem) return false;
			UTexture* Texture = Component->GetMeshPaintTexture();
			Out->SetBoolField(TEXT("ok"), true);
			Out->SetStringField(TEXT("status"), TEXT("succeeded"));
			Out->SetBoolField(TEXT("supports_texture_color_paint"), Component->CanMeshPaintTextureColors());
			Out->SetBoolField(TEXT("has_mesh_paint_texture"), Texture != nullptr);
			Out->SetStringField(TEXT("texture"), Texture ? Texture->GetPathName() : FString());
			Out->SetNumberField(TEXT("uv_channel"), Component->GetMeshPaintTextureCoordinateIndex());
			Out->SetNumberField(TEXT("resolution"), Component->GetMeshPaintTextureResolution());
			Out->SetNumberField(TEXT("resource_bytes"), Subsystem->GetMeshPaintTextureResourceSize(Component));
			Summary = FString::Printf(TEXT("Inspected texture-paint target on %s."), *Component->GetName());
			return true;
		}, nullptr, 15});

	Registry.Register({TEXT("mesh_paint_texture_target_create"),
		TEXT("Create the native UE 5.8 component Mesh Paint texture, save the map, and verify texture/resource readback."), ComponentSchema(false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			if (!Component || !Subsystem) return false;
			if (!Component->CanMeshPaintTextureColors())
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_not_supported"), TEXT("This component/mesh is not configured for Mesh Paint textures."));
				return false;
			}
			if (Component->GetMeshPaintTexture())
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_already_exists"), TEXT("The component already has a Mesh Paint texture."));
				return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureCreate", "SOMOLMCP Create Mesh Paint Texture"));
			Component->Modify();
			Subsystem->CreateComponentMeshPaintTexture(Component);
			UTexture* Texture = Component->GetMeshPaintTexture();
			const uint32 ResourceBytes = Subsystem->GetMeshPaintTextureResourceSize(Component);
			if (!Texture || ResourceBytes == 0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_texture_create_readback_failed"), TEXT("No texture or resource bytes were readable after creation."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			CompleteWrite(Out, TEXT("mesh_paint_texture_create"), *Component);
			Out->SetStringField(TEXT("texture"), Texture->GetPathName());
			Out->SetNumberField(TEXT("resource_bytes"), ResourceBytes);
			Out->SetNumberField(TEXT("uv_channel"), Component->GetMeshPaintTextureCoordinateIndex());
			Out->SetNumberField(TEXT("resolution"), Component->GetMeshPaintTextureResolution());
			Summary = FString::Printf(TEXT("Created Mesh Paint texture on %s."), *Component->GetName());
			return true;
		}, nullptr, 2});

	Registry.Register({TEXT("mesh_paint_texture_target_remove"),
		TEXT("Remove the native component Mesh Paint texture, save the map, and verify null texture and zero resource bytes."), ComponentSchema(false),
		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args,
			TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			UStaticMeshComponent* Component = FindComponent(Args, Out, Error);
			UMeshPaintingSubsystem* Subsystem = Component ? GetMeshPainting(Out, Error) : nullptr;
			if (!Component || !Subsystem) return false;
			if (!Component->GetMeshPaintTexture())
			{
				Fail(Out, Error, TEXT("mesh_paint_texture_missing"), TEXT("The component has no Mesh Paint texture to remove."));
				return false;
			}
			FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MeshPaintTextureRemove", "SOMOLMCP Remove Mesh Paint Texture"));
			Component->Modify();
			Subsystem->RemoveComponentMeshPaintTexture(Component);
			if (Component->GetMeshPaintTexture() || Subsystem->GetMeshPaintTextureResourceSize(Component) != 0)
			{
				Transaction.Cancel();
				Fail(Out, Error, TEXT("mesh_paint_texture_remove_readback_failed"), TEXT("Mesh Paint texture data remains after removal."));
				return false;
			}
			if (!SaveOwnerMap(Context, *Component, Out, Error)) return false;
			CompleteWrite(Out, TEXT("mesh_paint_texture_remove"), *Component);
			Out->SetBoolField(TEXT("has_mesh_paint_texture"), false);
			Out->SetNumberField(TEXT("resource_bytes"), 0);
			Summary = FString::Printf(TEXT("Removed Mesh Paint texture from %s."), *Component->GetName());
			return true;
		}, nullptr, 2});
#else
	(void)Registry;
#endif
}
}
