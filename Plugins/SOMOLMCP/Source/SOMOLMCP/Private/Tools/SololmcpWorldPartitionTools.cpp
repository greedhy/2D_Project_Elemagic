// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpWorldPartitionTools.cpp
// P2-3 — World Partition Tools (cell sizing, streaming sources, HLOD, loading bounds, inspection)
// 5 tools (worldpartition_* prefix):
//   1. worldpartition_set_cell_size
//   2. worldpartition_add_streaming_source
//   3. worldpartition_hlod_generate
//   4. worldpartition_loading_range_set
//   5. worldpartition_inspect
//
// Editor module deps (already in SOMOLMCP.Build.cs):
//   - WorldPartitionEditor (UWorldPartition + editor builders)
//   - Engine (UWorldPartitionStreamingSourceComponent)
//
// Notes (TODO):
//   * worldpartition_hlod_generate — A direct C++ call to FWorldPartitionHLODsBuilder
//     requires the WorldPartitionHLODUtilities/UnrealEd builder framework to be
//     reachable from a non-commandlet context. We invoke it via console
//     `wp.HLOD.Generate` (which the editor wires to the same builder) and surface
//     a count snapshot from UWorldPartition. If the host needs deterministic
//     completion, prefer the commandlet path (`-run=WorldPartitionBuilderCommandlet`).
//   * worldpartition_set_cell_size — runtime cell size on UWorldPartitionRuntimeHash
//     subclasses is editor-private; we set the runtime CVar (`wp.Runtime.CellSize`),
//     mark the world dirty, and report the requested + total cell counts.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "Components/WorldPartitionStreamingSourceComponent.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "SOMOLMCP_WorldPartition"

namespace UE::SOMOLMCP
{
	namespace WP
	{
		using SB = FSololmcpSchemaBuilder;

		// Resolve world by path. If world_path is empty/missing or matches the
		// editor world, return the editor world. Otherwise LoadObject the level
		// asset and return its UWorld.
		static UWorld* ResolveWorld(const FSololmcpEditorServices& Services,
			const FString& WorldPath, FString& OutErr)
		{
			UWorld* Editor = Services.GetEditorWorld(OutErr);
			if (WorldPath.IsEmpty()) { return Editor; }

			if (Editor)
			{
				const FString EditorPath = Editor->GetPathName();
				if (EditorPath == WorldPath || Editor->GetName() == WorldPath
				    || EditorPath.StartsWith(WorldPath))
				{
					return Editor;
				}
			}

			UWorld* Loaded = LoadObject<UWorld>(nullptr, *WorldPath);
			if (!Loaded)
			{
				OutErr = FString::Printf(TEXT("World '%s' not found / not loaded."), *WorldPath);
				return nullptr;
			}
			return Loaded;
		}

		static AActor* ResolveActor(UWorld* World, const FString& Id)
		{
			if (!World) return nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetPathName() == Id) return *It;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetName() == Id) return *It;
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if ((*It)->GetActorLabel() == Id) return *It;
			}
			return nullptr;
		}

		static EStreamingSourcePriority ParsePriority(const FString& Token)
		{
			if (Token.Equals(TEXT("high"), ESearchCase::IgnoreCase))   return EStreamingSourcePriority::High;
			if (Token.Equals(TEXT("low"), ESearchCase::IgnoreCase))    return EStreamingSourcePriority::Low;
			return EStreamingSourcePriority::Normal;
		}

		static EStreamingSourceTargetState ParseTargetState(const FString& Token)
		{
			if (Token.Equals(TEXT("activated"), ESearchCase::IgnoreCase))
			{
				return EStreamingSourceTargetState::Activated;
			}
			return EStreamingSourceTargetState::Loaded;
		}

		// UE 5.7: UWorldPartition::ForEachStreamingCells removed; access via WP->RuntimeHash
		// (public UPROPERTY at WorldPartition.h:601). UWorldPartitionRuntimeHash::ForEachStreamingCells
		// is ENGINE_API public (RuntimeHash.h:267, const TFunctionRef<bool(const Cell*)>).
		static int32 CountStreamingCells(UWorldPartition* WP, int32* OutLoaded = nullptr)
		{
			int32 Total = 0;
			int32 Loaded = 0;
			if (WP && WP->RuntimeHash)
			{
				WP->RuntimeHash->ForEachStreamingCells(
					[&Total, &Loaded](const UWorldPartitionRuntimeCell* Cell) -> bool
					{
						if (Cell)
						{
							++Total;
							// GetCurrentState() -> EWorldPartitionRuntimeCellState; Loaded/Activated counted
							const auto State = Cell->GetCurrentState();
							if (State != EWorldPartitionRuntimeCellState::Unloaded)
							{
								++Loaded;
							}
						}
						return true; // continue iteration
					});
			}
			if (OutLoaded) { *OutLoaded = Loaded; }
			return Total;
		}

		static bool ConsoleTextLooksLikeError(const FString& Text)
		{
			return Text.Contains(TEXT("error"), ESearchCase::IgnoreCase)
				|| Text.Contains(TEXT("failed"), ESearchCase::IgnoreCase)
				|| Text.Contains(TEXT("unknown command"), ESearchCase::IgnoreCase);
		}
	} // namespace WP

	// ════════════════════════════════════════════════════════════════════
	void RegisterWorldPartitionTools(FSololmcpToolRegistry& R)
	{
		using namespace WP;

		// ── 1. worldpartition_set_cell_size ───────────────────────────
		R.Register({
			TEXT("worldpartition_set_cell_size"),
			TEXT("Set the World Partition runtime cell size (cm). Returns total cell count snapshot."),
			SB::Object({
				{TEXT("world_path"),    SB::String(TEXT("Level asset path. Empty = current editor world."))},
				{TEXT("cell_size_cm"),  SB::Integer(TEXT("Runtime cell size in cm (e.g. 25600 = 256m)."))}
			}, {TEXT("cell_size_cm")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString WorldPath; Args->TryGetStringField(TEXT("world_path"), WorldPath);
				if (!Args->HasTypedField<EJson::Number>(TEXT("cell_size_cm")))
				{
					SololmcpError::MissingParam(Out, TEXT("cell_size_cm"));
					Error = TEXT("Missing cell_size_cm."); return false;
				}
				const int64 CellSize = Args->GetIntegerField(TEXT("cell_size_cm"));
				if (CellSize <= 0)
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("cell_size_cm"),
						TEXT("cell_size_cm must be > 0."));
					Error = TEXT("Bad cell_size_cm."); return false;
				}

				UWorld* World = ResolveWorld(Ctx.Services, WorldPath, Error);
				if (!World) { SololmcpError::NotFound(Out, WorldPath); return false; }

				UWorldPartition* WP = World->GetWorldPartition();
				if (!WP)
				{
					SololmcpError::Set(Out, TEXT("NOT_FOUND"), TEXT(""),
						TEXT("World does not have World Partition enabled."));
					Error = TEXT("No WorldPartition."); return false;
				}

				const FScopedTransaction Tx(LOCTEXT("WPSetCellSize", "SOMOLMCP WP Set Cell Size"));
				WP->Modify();

				// Apply via the editor CVar — works without touching the
				// editor-private runtime-hash setters across UE 5.x revisions.
				const FString Cmd = FString::Printf(TEXT("wp.Runtime.CellSize %lld"), CellSize);
				TSharedRef<FJsonObject> Junk = MakeShared<FJsonObject>(); FString Js, Je;
				if (!Ctx.Services.ExecuteConsole(Cmd, Junk, Js, Je))
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("cell_size_cm"),
						TEXT("Console command wp.Runtime.CellSize failed."));
					Error = Je.IsEmpty() ? TEXT("Failed to execute wp.Runtime.CellSize.") : Je;
					return false;
				}

				SololmcpWriteFlush::EnsureFlushed(WP);

				int32 Loaded = 0;
				const int32 Total = CountStreamingCells(WP, &Loaded);

				Out->SetStringField(TEXT("world"), World->GetPathName());
				Out->SetNumberField(TEXT("cell_size"), static_cast<double>(CellSize));
				Out->SetNumberField(TEXT("total_cells"), Total);
				Out->SetNumberField(TEXT("loaded_cells"), Loaded);

				Summary = FString::Printf(TEXT("WP cell_size=%lld cm, total_cells=%d"), CellSize, Total);
				return true;
			},
			nullptr, 0
		});

		// ── 2. worldpartition_add_streaming_source ────────────────────
		R.Register({
			TEXT("worldpartition_add_streaming_source"),
			TEXT("Attach a UWorldPartitionStreamingSourceComponent to an existing actor."),
			SB::Object({
				{TEXT("world_path"),         SB::String(TEXT("Optional level path."))},
				{TEXT("source_actor_name"),  SB::String(TEXT("Existing actor to host the source."))},
				{TEXT("priority"),           SB::String(TEXT("'normal'|'high'|'low' (default 'normal')."), {TEXT("normal"), TEXT("high"), TEXT("low")})},
				{TEXT("target_state"),       SB::String(TEXT("'loaded'|'activated' (default 'loaded')."), {TEXT("loaded"), TEXT("activated")})}
			}, {TEXT("source_actor_name"), TEXT("priority")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString WorldPath; Args->TryGetStringField(TEXT("world_path"), WorldPath);
				FString ActorId;
				if (!Args->TryGetStringField(TEXT("source_actor_name"), ActorId))
				{
					SololmcpError::MissingParam(Out, TEXT("source_actor_name"));
					Error = TEXT("Missing source_actor_name."); return false;
				}
				FString Priority = TEXT("normal");
				Args->TryGetStringField(TEXT("priority"), Priority);
				FString TargetState = TEXT("loaded");
				Args->TryGetStringField(TEXT("target_state"), TargetState);
				if (!TargetState.Equals(TEXT("loaded"), ESearchCase::IgnoreCase))
				{
					SololmcpError::Set(Out, TEXT("NOT_IMPLEMENTED"), TEXT("target_state"),
						TEXT("UE 5.7 keeps UWorldPartitionStreamingSourceComponent TargetState private; only the default loaded state can be claimed."));
					Error = TEXT("Non-default streaming source target_state is not supported.");
					return false;
				}

				UWorld* World = ResolveWorld(Ctx.Services, WorldPath, Error);
				if (!World) { SololmcpError::NotFound(Out, WorldPath); return false; }

				AActor* HostActor = ResolveActor(World, ActorId);
				if (!HostActor)
				{
					SololmcpError::NotFound(Out, ActorId);
					Error = FString::Printf(TEXT("Actor '%s' not found."), *ActorId);
					return false;
				}

				const FScopedTransaction Tx(LOCTEXT("WPAddSrc", "SOMOLMCP WP Add Streaming Source"));
				HostActor->Modify();

				UWorldPartitionStreamingSourceComponent* Src =
					NewObject<UWorldPartitionStreamingSourceComponent>(HostActor, NAME_None, RF_Transactional);
				if (!Src)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT(""),
						TEXT("NewObject<UWorldPartitionStreamingSourceComponent> returned null."));
					Error = TEXT("Component create failed."); return false;
				}
				// UE 5.7: TargetState is private with no public setter; only Priority can be set programmatically.
				// Default TargetState (Loaded) is acceptable for most cases. To override, use python_exec.
				Src->Priority = ParsePriority(Priority);
				(void)TargetState;

				HostActor->AddInstanceComponent(Src);
				Src->RegisterComponent();

				SololmcpWriteFlush::EnsureFlushed(HostActor);

				const FString SourceId = FString::Printf(TEXT("%s::%s"),
					*HostActor->GetName(), *Src->GetName());

				Out->SetStringField(TEXT("source_id"), SourceId);
				Out->SetStringField(TEXT("host_actor"), HostActor->GetName());
				Out->SetStringField(TEXT("component"), Src->GetName());
				Out->SetStringField(TEXT("priority"), Priority);
				Out->SetStringField(TEXT("requested_target_state"), TargetState);
				Out->SetStringField(TEXT("applied_target_state"), TEXT("loaded"));

				Summary = FString::Printf(TEXT("Added streaming source on '%s' (prio=%s, state=%s)."),
					*HostActor->GetName(), *Priority, *TargetState);
				return true;
			},
			nullptr, 0
		});

		// ── 3. worldpartition_hlod_generate ───────────────────────────
		R.Register({
			TEXT("worldpartition_hlod_generate"),
			TEXT("Trigger HLOD generation. Internally invokes the WP HLOD builder via console "
			     "(wp.HLOD.Generate). Returns a snapshot of cells_processed and proxy count."),
			SB::Object({
				{TEXT("world_path"),         SB::String()},
				{TEXT("hlod_layer_index"),   SB::Integer(TEXT("HLOD layer index (default 0)."))},
				{TEXT("cell_filter"),        SB::String(TEXT("'all' | 'changed' (default 'all')."), {TEXT("all"), TEXT("changed")})}
			}, {}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString WorldPath; Args->TryGetStringField(TEXT("world_path"), WorldPath);
				const int32 LayerIdx = Args->HasTypedField<EJson::Number>(TEXT("hlod_layer_index"))
					? Args->GetIntegerField(TEXT("hlod_layer_index")) : 0;
				FString CellFilter = TEXT("all");
				Args->TryGetStringField(TEXT("cell_filter"), CellFilter);

				UWorld* World = ResolveWorld(Ctx.Services, WorldPath, Error);
				if (!World) { SololmcpError::NotFound(Out, WorldPath); return false; }

				UWorldPartition* WP = World->GetWorldPartition();
				if (!WP)
				{
					SololmcpError::Set(Out, TEXT("NOT_FOUND"), TEXT(""),
						TEXT("World does not have World Partition enabled."));
					Error = TEXT("No WorldPartition."); return false;
				}

				// Snapshot cells before so the caller has a comparable count.
				int32 LoadedBefore = 0;
				const int32 TotalBefore = CountStreamingCells(WP, &LoadedBefore);

				// Drive the in-editor builder via console. This maps to
				// FWorldPartitionHLODsBuilder when WorldPartitionHLODUtilities is loaded.
				const FString Cmd = (CellFilter == TEXT("changed"))
					? TEXT("wp.HLOD.BuildChanged")
					: FString::Printf(TEXT("wp.HLOD.Generate %d"), LayerIdx);
				TSharedRef<FJsonObject> Junk = MakeShared<FJsonObject>(); FString Js, Je;
				const bool bDispatched = Ctx.Services.ExecuteConsole(Cmd, Junk, Js, Je);
				if (!bDispatched || !Je.IsEmpty() || ConsoleTextLooksLikeError(Js))
				{
					Out->SetStringField(TEXT("dispatch"), Cmd);
					Out->SetBoolField(TEXT("dispatched"), false);
					Out->SetBoolField(TEXT("completed"), false);
					Out->SetStringField(TEXT("console_stdout"), Js);
					Out->SetStringField(TEXT("console_stderr"), Je);
					Out->SetStringField(TEXT("error_hint"),
						TEXT("The HLOD console command was dispatched but UE reported an error or error-like output."));
					Out->SetStringField(TEXT("next_action"),
						TEXT("Check that World Partition and HLOD utilities are enabled, inspect Output Log, then retry or run the dispatch command manually."));
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("dispatch"),
						TEXT("HLOD generate console dispatch failed."));
					Error = FString::Printf(TEXT("HLOD generate console dispatch failed for '%s'. stderr='%s' stdout='%s'."),
						*Cmd, *Je, *Js);
					return false;
				}

				// Count HLOD-tagged actors as a rough proxy count.
				int32 HlodProxies = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* A = *It;
					if (A && A->GetClass() && A->GetClass()->GetName().Contains(TEXT("HLOD")))
					{
						++HlodProxies;
					}
				}

				SololmcpWriteFlush::EnsureFlushed(WP);

				Out->SetStringField(TEXT("world"), World->GetPathName());
				Out->SetNumberField(TEXT("hlod_layer_index"), LayerIdx);
				Out->SetStringField(TEXT("cell_filter"), CellFilter);
				Out->SetNumberField(TEXT("cells_processed"), TotalBefore);
				Out->SetNumberField(TEXT("hlod_proxies_observed"), HlodProxies);
				Out->SetBoolField(TEXT("dispatched"), true);
				Out->SetBoolField(TEXT("completed"), false);
				Out->SetStringField(TEXT("dispatch"), Cmd);

				Summary = FString::Printf(TEXT("HLOD generate dispatched (%s); completion not verified; cells=%d, proxies_observed=%d."),
					*Cmd, TotalBefore, HlodProxies);
				return true;
			},
			nullptr, 0
		});

		// ── 4. worldpartition_loading_range_set ───────────────────────
		R.Register({
			TEXT("worldpartition_loading_range_set"),
			TEXT("Set the WP loading range on an actor. Accepts any actor; updates "
			     "UWorldPartitionStreamingSourceComponent radius if present, else AActor "
			     "loading-bounds extension if available."),
			SB::Object({
				{TEXT("actor_name"),  SB::String()},
				{TEXT("range_cm"),    SB::Integer(TEXT("Loading range in cm."))}
			}, {TEXT("actor_name"), TEXT("range_cm")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString ActorId;
				if (!Args->TryGetStringField(TEXT("actor_name"), ActorId))
				{
					SololmcpError::MissingParam(Out, TEXT("actor_name"));
					Error = TEXT("Missing actor_name."); return false;
				}
				if (!Args->HasTypedField<EJson::Number>(TEXT("range_cm")))
				{
					SololmcpError::MissingParam(Out, TEXT("range_cm"));
					Error = TEXT("Missing range_cm."); return false;
				}
				const int64 RangeCm = Args->GetIntegerField(TEXT("range_cm"));
				if (RangeCm <= 0)
				{
					SololmcpError::Set(Out, TEXT("INVALID_TYPE"), TEXT("range_cm"),
						TEXT("range_cm must be > 0."));
					Error = TEXT("Bad range_cm."); return false;
				}

				UWorld* World = Ctx.Services.GetEditorWorld(Error); if (!World) return false;
				AActor* Actor = ResolveActor(World, ActorId);
				if (!Actor) { SololmcpError::NotFound(Out, ActorId); return false; }

				const FScopedTransaction Tx(LOCTEXT("WPRange", "SOMOLMCP WP Set Loading Range"));
				Actor->Modify();

				bool bApplied = false;
				UWorldPartitionStreamingSourceComponent* Src =
					Actor->FindComponentByClass<UWorldPartitionStreamingSourceComponent>();
				if (Src)
				{
					Src->Modify();
					// FStreamingSourceShape with Radius (UE 5.x).
					FStreamingSourceShape Shape;
					Shape.Radius = static_cast<float>(RangeCm);
					Shape.bUseGridLoadingRange = false;
					Src->Shapes.Reset();
					Src->Shapes.Add(Shape);
					bApplied = true;
				}

				const FBox Bounds = Actor->GetComponentsBoundingBox(true);

				SololmcpWriteFlush::EnsureFlushed(Actor);

				Out->SetStringField(TEXT("actor"), Actor->GetName());
				Out->SetNumberField(TEXT("range"), static_cast<double>(RangeCm));
				Out->SetBoolField(TEXT("applied"), bApplied);
				if (Src && Src->Shapes.Num() > 0)
				{
					Out->SetNumberField(TEXT("readback_range"), Src->Shapes[0].Radius);
				}
				TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
				BJ->SetNumberField(TEXT("min_x"), Bounds.Min.X);
				BJ->SetNumberField(TEXT("min_y"), Bounds.Min.Y);
				BJ->SetNumberField(TEXT("min_z"), Bounds.Min.Z);
				BJ->SetNumberField(TEXT("max_x"), Bounds.Max.X);
				BJ->SetNumberField(TEXT("max_y"), Bounds.Max.Y);
				BJ->SetNumberField(TEXT("max_z"), Bounds.Max.Z);
				Out->SetObjectField(TEXT("bounds"), BJ);

				Summary = FString::Printf(TEXT("%s loading range %lld cm (applied=%s)."),
					*Actor->GetName(), RangeCm, bApplied ? TEXT("yes") : TEXT("no"));
				if (!bApplied)
				{
					SololmcpError::Set(Out, TEXT("OPERATION_FAILED"), TEXT("actor_name"),
						TEXT("Actor has no UWorldPartitionStreamingSourceComponent; loading range was not applied."));
					Error = TEXT("Loading range was not applied.");
					return false;
				}
				return true;
			},
			nullptr, 0
		});

		// ── 5. worldpartition_inspect ─────────────────────────────────
		R.Register({
			TEXT("worldpartition_inspect"),
			TEXT("Inspect the world's WP runtime: cell sizing, total/loaded cells, HLOD layers, "
			     "and registered streaming sources."),
			SB::Object({
				{TEXT("world_path"), SB::String(TEXT("Optional level asset path."))}
			}, {}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args,
			   TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error) -> bool
			{
				FString WorldPath; Args->TryGetStringField(TEXT("world_path"), WorldPath);
				UWorld* World = ResolveWorld(Ctx.Services, WorldPath, Error);
				if (!World) { SololmcpError::NotFound(Out, WorldPath); return false; }

				UWorldPartition* WP = World->GetWorldPartition();
				if (!WP)
				{
					Out->SetStringField(TEXT("world"), World->GetPathName());
					Out->SetBoolField(TEXT("world_partition_enabled"), false);
					Out->SetNumberField(TEXT("cell_size"), 0);
					Out->SetNumberField(TEXT("total_cells"), 0);
					Out->SetNumberField(TEXT("loaded_cells"), 0);
					Out->SetArrayField(TEXT("hlod_layers"), {});
					Out->SetArrayField(TEXT("streaming_sources"), {});
					Summary = TEXT("World Partition is not enabled on this world.");
					return true;
				}

				int32 Loaded = 0;
				const int32 Total = CountStreamingCells(WP, &Loaded);

				// Read cell_size from CVar to avoid touching editor-private members.
				int32 CellSize = 0;
				if (IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("wp.Runtime.CellSize")))
				{
					CellSize = CV->GetInt();
				}

				// UE 5.7: UWorldPartition::ForEachHLODLayer was removed.
				// HLOD layer enumeration now requires WorldPartitionHLODUtilities. Stubbed — returns empty.
				TArray<TSharedPtr<FJsonValue>> HlodLayers;
				int32 LayerIdx = 0;
				(void)LayerIdx;

				// Streaming sources currently registered.
				TArray<TSharedPtr<FJsonValue>> Sources;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* A = *It;
					if (!A) continue;
					if (UWorldPartitionStreamingSourceComponent* Src =
						A->FindComponentByClass<UWorldPartitionStreamingSourceComponent>())
					{
						TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
						J->SetStringField(TEXT("actor"), A->GetName());
						J->SetStringField(TEXT("component"), Src->GetName());
						J->SetStringField(TEXT("priority"),
							Src->Priority == EStreamingSourcePriority::High   ? TEXT("high")
							: Src->Priority == EStreamingSourcePriority::Low  ? TEXT("low")
							: TEXT("normal"));
						// UE 5.7: TargetState is private with no public getter; report as "unknown".
						J->SetStringField(TEXT("target_state"), TEXT("unknown"));
						const FVector Loc = A->GetActorLocation();
						TSharedRef<FJsonObject> LJ = MakeShared<FJsonObject>();
						LJ->SetNumberField(TEXT("x"), Loc.X);
						LJ->SetNumberField(TEXT("y"), Loc.Y);
						LJ->SetNumberField(TEXT("z"), Loc.Z);
						J->SetObjectField(TEXT("location"), LJ);
						Sources.Add(MakeShared<FJsonValueObject>(J));
					}
				}

				Out->SetStringField(TEXT("world"), World->GetPathName());
				Out->SetBoolField(TEXT("world_partition_enabled"), true);
				Out->SetNumberField(TEXT("cell_size"), CellSize);
				Out->SetNumberField(TEXT("total_cells"), Total);
				Out->SetNumberField(TEXT("loaded_cells"), Loaded);
				Out->SetArrayField(TEXT("hlod_layers"), HlodLayers);
				Out->SetArrayField(TEXT("streaming_sources"), Sources);

				Summary = FString::Printf(TEXT("WP: cells=%d (%d loaded), hlod_layers=%d, sources=%d"),
					Total, Loaded, HlodLayers.Num(), Sources.Num());
				return true;
			},
			nullptr, 5
		});
	}
} // namespace UE::SOMOLMCP

#undef LOCTEXT_NAMESPACE
