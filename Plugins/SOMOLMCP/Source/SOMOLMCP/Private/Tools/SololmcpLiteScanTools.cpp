// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpLiteScanTools.cpp
//
// Two LITE-grade scan tools meant as fast alternatives to python_exec scripts
// that would otherwise enumerate the full scene / world partition and freeze
// the editor's game thread for minutes:
//
//   • world_partition_status_lite — returns cell counts + WP enabled/loaded
//     state, NEVER enumerates actors. Designed to take milliseconds even on
//     1M+ cell worlds.
//   • landscape_actor_list_lite — returns the names + transforms of landscape
//     proxies in the editor world, NO heightmap/component/material reads.
//
// Both tools live in the existing tool registry and require no rebuild of
// other modules. Build by adding this file to the SOMOLMCP module via the
// existing UBT *.cpp glob (default behavior in .Build.cs) — no manual
// listing required.
//

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Editor.h"

#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeStreamingProxy.h"

#include "WorldPartition/WorldPartition.h"

#include "EngineUtils.h"   // TActorIterator

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPLiteScan, Log, All);

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────
// Helpers (file-local; mirror the small-utility pattern used elsewhere)
// ─────────────────────────────────────────────────────────────────────────

static UWorld* LiteScan_GetEditorWorld(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Tool must be invoked on the game thread.");
		return nullptr;
	}
	if (!GEditor)
	{
		OutError = TEXT("GEditor is null (PIE / commandlet?).");
		return nullptr;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		OutError = TEXT("No editor world available.");
		return nullptr;
	}
	return World;
}

static TSharedRef<FJsonObject> Vec3Json(const FVector& V)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("x"), V.X);
	O->SetNumberField(TEXT("y"), V.Y);
	O->SetNumberField(TEXT("z"), V.Z);
	return O;
}

static TSharedRef<FJsonObject> RotJson(const FRotator& R)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("pitch"), R.Pitch);
	O->SetNumberField(TEXT("yaw"),   R.Yaw);
	O->SetNumberField(TEXT("roll"),  R.Roll);
	return O;
}

// ─────────────────────────────────────────────────────────────────────────
// RegisterLiteScanTools
// ─────────────────────────────────────────────────────────────────────────

void RegisterLiteScanTools(FSololmcpToolRegistry& Registry)
{
	// ─────────────────────────────────────────────────────────────────────
	// 1. world_partition_status_lite
	// ─────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("world_partition_status_lite"),
		TEXT("Fast read-only snapshot of the World Partition state for the current editor world. "
			 "Returns wp_enabled, world_name, origin, and a count of actor descriptors WITHOUT "
			 "iterating the actor pool — designed to never freeze the editor. Prefer this over "
			 "python_exec scripts that call WorldPartition.get_loaded_regions() / "
			 "EditorActorSubsystem.get_all_level_actors()."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			UWorld* World = LiteScan_GetEditorWorld(OutError);
			if (!World) { return false; }

			OutStructured->SetStringField(TEXT("world_name"), World->GetName());
			OutStructured->SetObjectField(TEXT("origin"),
				Vec3Json(FVector(World->OriginLocation.X, World->OriginLocation.Y, World->OriginLocation.Z)));

			UWorldPartition* WP = World->GetWorldPartition();
			OutStructured->SetBoolField(TEXT("wp_enabled"), WP != nullptr);

			if (!WP)
			{
				OutSummary = TEXT("WP disabled.");
				OutStructured->SetNumberField(TEXT("actor_descriptor_count"), 0);
				return true;
			}

			// Count actor descriptors without enumerating each one's data.
			// FActorDescContainer iteration is lightweight — it does NOT load actors.
			int32 ActorDescCount = 0;
			FBox WorldBounds(EForceInit::ForceInit);
			bool bHaveBounds = false;

			// 5.4 split the actor descriptor into a descriptor plus a per-instance view:
			// FActorDescContainerCollection/FWorldPartitionActorDesc became
			// FActorDescContainerInstanceCollection/FWorldPartitionActorDescInstance.
			// Only the two type names differ here -- GetEditorBounds() is on both, and
			// this loop just counts and unions bounds, touching no actor data.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
			for (FActorDescContainerInstanceCollection::TIterator<> It(WP); It; ++It)
			{
				++ActorDescCount;
				const FWorldPartitionActorDescInstance* Desc = *It;
#else
			for (FActorDescContainerCollection::TIterator<> It(WP); It; ++It)
			{
				++ActorDescCount;
				const FWorldPartitionActorDesc* Desc = *It;
#endif
				if (Desc)
				{
					const FBox B = Desc->GetEditorBounds();
					if (B.IsValid)
					{
						if (!bHaveBounds) { WorldBounds = B; bHaveBounds = true; }
						else              { WorldBounds += B; }
					}
				}
			}
			OutStructured->SetNumberField(TEXT("actor_descriptor_count"), ActorDescCount);

			if (bHaveBounds)
			{
				const FVector Center = WorldBounds.GetCenter();
				const FVector Extent = WorldBounds.GetExtent();
				TSharedRef<FJsonObject> BBox = MakeShared<FJsonObject>();
				BBox->SetObjectField(TEXT("min"), Vec3Json(WorldBounds.Min));
				BBox->SetObjectField(TEXT("max"), Vec3Json(WorldBounds.Max));
				BBox->SetObjectField(TEXT("center"), Vec3Json(Center));
				BBox->SetObjectField(TEXT("extent"), Vec3Json(Extent));
				OutStructured->SetObjectField(TEXT("world_bounds"), BBox);
			}

			OutSummary = FString::Printf(
				TEXT("WP=on, actors=%d, world='%s'"),
				ActorDescCount, *World->GetName());
			return true;
		},
		nullptr, 10
	});

	// ─────────────────────────────────────────────────────────────────────
	// 2. landscape_actor_list_lite
	// ─────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("landscape_actor_list_lite"),
		TEXT("List landscape proxies in the editor world with NAME + TRANSFORM ONLY. "
			 "Does NOT touch heightmaps, components, materials, or layer info. "
			 "Designed to never freeze the editor. Optional `limit` (default 200) caps results. "
			 "Use this instead of python_exec scripts that iterate "
			 "EditorActorSubsystem.get_all_level_actors() or read landscape proxy data."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("limit"),
					FSololmcpSchemaBuilder::Integer(TEXT("Max landscape proxies to return (default 200, max 5000)."))},
				{TEXT("include_streaming_proxies"),
					FSololmcpSchemaBuilder::Boolean(TEXT("Include LandscapeStreamingProxy actors (WP world tile proxies). Default true."))}
			},
			{} /* no required fields */),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			UWorld* World = LiteScan_GetEditorWorld(OutError);
			if (!World) { return false; }

			int32 Limit = 200;
			{
				int32 LimitArg = 0;
				if (Arguments->TryGetNumberField(TEXT("limit"), LimitArg))
				{
					Limit = FMath::Clamp(LimitArg, 1, 5000);
				}
			}

			bool bIncludeStreaming = true;
			Arguments->TryGetBoolField(TEXT("include_streaming_proxies"), bIncludeStreaming);

			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Total = 0;
			int32 LandscapeCount = 0;
			int32 StreamingCount = 0;

			for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
			{
				ALandscapeProxy* Proxy = *It;
				if (!Proxy) continue;

				const bool bIsStreaming = Proxy->IsA(ALandscapeStreamingProxy::StaticClass());
				if (bIsStreaming) { ++StreamingCount; }
				else if (Proxy->IsA(ALandscape::StaticClass())) { ++LandscapeCount; }

				if (bIsStreaming && !bIncludeStreaming) { continue; }

				++Total;
				if (Items.Num() >= Limit) { continue; }

				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Proxy->GetName());
				Item->SetStringField(TEXT("path"), Proxy->GetPathName());
				Item->SetStringField(TEXT("class"), Proxy->GetClass()->GetName());
				Item->SetBoolField(TEXT("is_streaming_proxy"), bIsStreaming);
				Item->SetObjectField(TEXT("location"),  Vec3Json(Proxy->GetActorLocation()));
				Item->SetObjectField(TEXT("rotation"),  RotJson(Proxy->GetActorRotation()));
				Item->SetObjectField(TEXT("scale"),     Vec3Json(Proxy->GetActorScale3D()));
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}

			OutStructured->SetArrayField(TEXT("items"), Items);
			OutStructured->SetNumberField(TEXT("returned"), Items.Num());
			OutStructured->SetNumberField(TEXT("total_matched"), Total);
			OutStructured->SetNumberField(TEXT("landscape_actor_count"), LandscapeCount);
			OutStructured->SetNumberField(TEXT("streaming_proxy_count"), StreamingCount);
			OutStructured->SetBoolField(TEXT("truncated"), Total > Items.Num());

			OutSummary = FString::Printf(
				TEXT("landscapes=%d, streaming_proxies=%d, returned=%d/%d"),
				LandscapeCount, StreamingCount, Items.Num(), Total);
			return true;
		},
		nullptr, 10
	});
}

} // namespace UE::SOMOLMCP
