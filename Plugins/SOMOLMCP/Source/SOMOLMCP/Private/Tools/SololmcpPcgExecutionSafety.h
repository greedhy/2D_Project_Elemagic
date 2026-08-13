// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "Tools/SololmcpToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

namespace UE::SOMOLMCP::PcgExecutionSafety
{
	static constexpr int32 PcgMaxTilesPerGenerate = 4;
	static constexpr const TCHAR* PcgTileCapSchema = TEXT("somol.pcg.tile_cap_guard.v1");

	struct FGenerateComponentTarget
	{
		AActor* Actor = nullptr;
		UPCGComponent* Component = nullptr;
		FString ActorLabel;
		FString ActorName;
		FString ActorPath;
		FString ComponentName;
		FString GraphPath;
	};

	struct FGenerateTargetSet
	{
		FString RequestedActor;
		FString RequestedActorField;
		FString RequestedGraphPath;
		bool bAllowAll = false;
		bool bAllowPartialActorLabel = false;
		TArray<FGenerateComponentTarget> Components;
		TArray<FString> UniqueGraphPaths;
		TArray<TSharedPtr<FJsonValue>> Warnings;
	};

	struct FTileCapDecision
	{
		FString ToolName;
		FString Status = TEXT("pass");
		FString ObservedSource = TEXT("none");
		FString Reason;
		int32 ObservedTileCount = -1;
		bool bStrictOrUnattended = false;
		bool bRequireTileEvidence = false;
		bool bFailClosed = false;

		bool IsBlocked() const
		{
			return Status == TEXT("block");
		}
	};

	static inline FString TrimmedStringField(const TSharedRef<FJsonObject>& Arguments, const TCHAR* FieldName)
	{
		FString Value;
		if (!Arguments->TryGetStringField(FieldName, Value))
		{
			return FString();
		}
		return Value.TrimStartAndEnd();
	}

	static inline FString ReadActorArgument(const TSharedRef<FJsonObject>& Arguments, FString& OutFieldName)
	{
		FString Actor = TrimmedStringField(Arguments, TEXT("actor"));
		if (!Actor.IsEmpty())
		{
			OutFieldName = TEXT("actor");
			return Actor;
		}

		Actor = TrimmedStringField(Arguments, TEXT("actor_label"));
		if (!Actor.IsEmpty())
		{
			OutFieldName = TEXT("actor_label");
			return Actor;
		}

		OutFieldName.Reset();
		return FString();
	}

	static inline FString ReadGraphPathArgument(const TSharedRef<FJsonObject>& Arguments)
	{
		FString GraphPath = TrimmedStringField(Arguments, TEXT("graph_path"));
		if (!GraphPath.IsEmpty())
		{
			return GraphPath;
		}

		// Legacy graph tools usually call this asset_path; accept it as a strict
		// alias so callers can pass one shape across validate/dry_run/generate.
		return TrimmedStringField(Arguments, TEXT("asset_path"));
	}

	static inline FString StripExportTextWrapper(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();

		int32 FirstSingleQuote = INDEX_NONE;
		int32 LastSingleQuote = INDEX_NONE;
		if (Path.FindChar(TCHAR('\''), FirstSingleQuote) &&
			Path.FindLastChar(TCHAR('\''), LastSingleQuote) &&
			LastSingleQuote > FirstSingleQuote)
		{
			Path = Path.Mid(FirstSingleQuote + 1, LastSingleQuote - FirstSingleQuote - 1);
		}

		int32 FirstDoubleQuote = INDEX_NONE;
		int32 LastDoubleQuote = INDEX_NONE;
		if (Path.FindChar(TCHAR('"'), FirstDoubleQuote) &&
			Path.FindLastChar(TCHAR('"'), LastDoubleQuote) &&
			LastDoubleQuote > FirstDoubleQuote)
		{
			Path = Path.Mid(FirstDoubleQuote + 1, LastDoubleQuote - FirstDoubleQuote - 1);
		}

		return Path.TrimStartAndEnd();
	}

	static inline FString PackagePathForCompare(const FString& InPath)
	{
		const FString Path = StripExportTextWrapper(InPath);
		if (Path.IsEmpty())
		{
			return FString();
		}

		FString PackagePath = FPackageName::ObjectPathToPackageName(Path);
		if (!PackagePath.IsEmpty())
		{
			return PackagePath;
		}

		int32 Dot = INDEX_NONE;
		return Path.FindChar(TCHAR('.'), Dot) ? Path.Left(Dot) : Path;
	}

	static inline FString ObjectPathForCompare(const FString& InPath)
	{
		FString Path = StripExportTextWrapper(InPath);
		if (Path.IsEmpty() || Path.Contains(TEXT(".")))
		{
			return Path;
		}

		FString Left;
		FString Leaf;
		if (Path.Split(TEXT("/"), &Left, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !Leaf.IsEmpty())
		{
			return Path + TEXT(".") + Leaf;
		}
		return Path;
	}

	static inline bool GraphPathsMatch(const FString& RequestedPath, const FString& ActualPath)
	{
		const FString RequestedObject = ObjectPathForCompare(RequestedPath);
		const FString ActualObject = ObjectPathForCompare(ActualPath);
		if (!RequestedObject.IsEmpty() && RequestedObject.Equals(ActualObject, ESearchCase::IgnoreCase))
		{
			return true;
		}

		const FString RequestedPackage = PackagePathForCompare(RequestedPath);
		const FString ActualPackage = PackagePathForCompare(ActualPath);
		return !RequestedPackage.IsEmpty() && RequestedPackage.Equals(ActualPackage, ESearchCase::IgnoreCase);
	}

	static inline void AddWarning(
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		const FString& Code,
		const FString& Message,
		const FString& Hint = FString())
	{
		TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("code"), Code);
		Warning->SetStringField(TEXT("message"), Message);
		if (!Hint.IsEmpty())
		{
			Warning->SetStringField(TEXT("hint"), Hint);
		}
		Warnings.Add(MakeShared<FJsonValueObject>(Warning));
	}

	static inline bool JsonValueAsInt(const TSharedPtr<FJsonValue>& Value, int32& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			Out = FMath::Max(0, FMath::CeilToInt(Value->AsNumber()));
			return true;
		}
		if (Value->Type == EJson::String)
		{
			return LexTryParseString(Out, *Value->AsString());
		}
		return false;
	}

	static inline bool TryNestedBoolField(
		const TSharedRef<FJsonObject>& Arguments,
		const TCHAR* ObjectField,
		const TCHAR* BoolField,
		bool& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Arguments->TryGetObjectField(ObjectField, Obj) || !Obj || !Obj->IsValid())
		{
			return false;
		}
		return (*Obj)->TryGetBoolField(BoolField, Out);
	}

	static inline bool RequestIsStrictOrUnattended(const TSharedRef<FJsonObject>& Arguments)
	{
		bool b = false;
		if ((Arguments->TryGetBoolField(TEXT("strict"), b) && b) ||
			(Arguments->TryGetBoolField(TEXT("unattended"), b) && b))
		{
			return true;
		}
		if ((TryNestedBoolField(Arguments, TEXT("policy"), TEXT("strict"), b) && b) ||
			(TryNestedBoolField(Arguments, TEXT("policy"), TEXT("unattended"), b) && b) ||
			(TryNestedBoolField(Arguments, TEXT("generation_policy"), TEXT("strict"), b) && b) ||
			(TryNestedBoolField(Arguments, TEXT("generation_policy"), TEXT("unattended"), b) && b))
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* GenPolicy = nullptr;
		if (Arguments->TryGetObjectField(TEXT("generation_policy"), GenPolicy) && GenPolicy && GenPolicy->IsValid())
		{
			FString Mode;
			if ((*GenPolicy)->TryGetStringField(TEXT("mode"), Mode))
			{
				return Mode.Contains(TEXT("strict"), ESearchCase::IgnoreCase) ||
					Mode.Contains(TEXT("unattended"), ESearchCase::IgnoreCase);
			}
		}
		return false;
	}

	static inline bool TryArrayTileCount(
		const TSharedRef<FJsonObject>& Arguments,
		const TCHAR* FieldName,
		int32& OutCount,
		FString& OutSource)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Arguments->TryGetArrayField(FieldName, Arr) && Arr)
		{
			OutCount = Arr->Num();
			OutSource = FieldName;
			return true;
		}
		return false;
	}

	static inline bool TryNumericTileCount(
		const TSharedRef<FJsonObject>& Arguments,
		const TCHAR* FieldName,
		int32& OutCount,
		FString& OutSource)
	{
		const TSharedPtr<FJsonValue> Field = Arguments->TryGetField(FieldName);
		if (JsonValueAsInt(Field, OutCount))
		{
			OutSource = FieldName;
			return true;
		}
		return false;
	}

	static inline bool TryAreaTileCount(
		const TSharedRef<FJsonObject>& Arguments,
		int32& OutCount,
		FString& OutSource)
	{
		double AreaM2 = 0.0;
		if (!Arguments->TryGetNumberField(TEXT("area_m2"), AreaM2) || AreaM2 <= 0.0)
		{
			return false;
		}
		double TileSizeM = 256.0;
		Arguments->TryGetNumberField(TEXT("tile_size_m"), TileSizeM);
		TileSizeM = FMath::Max(1.0, TileSizeM);
		OutCount = FMath::Max(1, FMath::CeilToInt(AreaM2 / (TileSizeM * TileSizeM)));
		OutSource = TEXT("area_m2");
		return true;
	}

	static inline FTileCapDecision EvaluateTileCapForGenerate(
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Arguments)
	{
		FTileCapDecision Decision;
		Decision.ToolName = ToolName;
		Decision.bStrictOrUnattended = RequestIsStrictOrUnattended(Arguments);
		Decision.bRequireTileEvidence = Decision.bStrictOrUnattended;

		int32 Count = -1;
		FString Source;
		if (TryArrayTileCount(Arguments, TEXT("tile_indices"), Count, Source) ||
			TryArrayTileCount(Arguments, TEXT("allowed_tiles"), Count, Source) ||
			TryArrayTileCount(Arguments, TEXT("tiles"), Count, Source) ||
			TryNumericTileCount(Arguments, TEXT("tile_count"), Count, Source) ||
			TryNumericTileCount(Arguments, TEXT("tiles_total"), Count, Source) ||
			TryNumericTileCount(Arguments, TEXT("selected_tile_count"), Count, Source) ||
			TryNumericTileCount(Arguments, TEXT("tile_filter_requested_tile_count"), Count, Source) ||
			TryAreaTileCount(Arguments, Count, Source))
		{
			Decision.ObservedTileCount = Count;
			Decision.ObservedSource = Source;
		}

		if (Decision.ObservedTileCount > PcgMaxTilesPerGenerate)
		{
			Decision.Status = TEXT("block");
			Decision.bFailClosed = true;
			Decision.Reason = FString::Printf(
				TEXT("tile_count_exceeds_cap: observed %d, max %d"),
				Decision.ObservedTileCount,
				PcgMaxTilesPerGenerate);
		}
		else if (Decision.ObservedTileCount < 0 && Decision.bRequireTileEvidence)
		{
			Decision.Status = TEXT("block");
			Decision.bFailClosed = true;
			Decision.Reason = TEXT("tile_count_unavailable_in_strict_unattended_request");
		}
		else if (Decision.ObservedTileCount < 0)
		{
			Decision.Status = TEXT("warn");
			Decision.Reason = TEXT("tile_count_unavailable_best_effort");
		}
		else
		{
			Decision.Status = TEXT("pass");
			Decision.Reason = FString::Printf(
				TEXT("tile_count_within_cap: observed %d, max %d"),
				Decision.ObservedTileCount,
				PcgMaxTilesPerGenerate);
		}
		return Decision;
	}

	static inline void AttachTileCapFields(
		const TSharedRef<FJsonObject>& OutStructured,
		const FTileCapDecision& Decision)
	{
		OutStructured->SetStringField(TEXT("tile_cap_schema"), PcgTileCapSchema);
		OutStructured->SetStringField(TEXT("tile_cap_status"), Decision.Status);
		OutStructured->SetStringField(TEXT("tile_cap_observed_source"), Decision.ObservedSource);
		OutStructured->SetStringField(TEXT("tile_cap_guard_reason"), Decision.Reason);
		OutStructured->SetBoolField(TEXT("tile_cap_fail_closed"), Decision.bFailClosed);
		OutStructured->SetBoolField(TEXT("tile_cap_strict_or_unattended"), Decision.bStrictOrUnattended);
		OutStructured->SetBoolField(TEXT("tile_batch_count_known"), Decision.ObservedTileCount >= 0);
		OutStructured->SetNumberField(TEXT("tile_batch_count"), Decision.ObservedTileCount >= 0 ? Decision.ObservedTileCount : 0);
		OutStructured->SetBoolField(TEXT("tile_mask_native_enforced"), false);
		OutStructured->SetBoolField(TEXT("tile_mask_receipt_only"), Decision.ObservedTileCount >= 0);
		OutStructured->SetStringField(
			TEXT("tile_mask_status"),
			Decision.ObservedTileCount >= 0 ? TEXT("receipt_evidence_only_actor_scope_generate") : TEXT("missing_tile_evidence"));
		OutStructured->SetStringField(
			TEXT("tile_mask_note"),
			TEXT("Native UPCGComponent::Generate has no tile-mask parameter here; tile evidence proves the <=4 cap while execution remains actor/component scoped."));

		TSharedRef<FJsonObject> Policy = MakeShared<FJsonObject>();
		Policy->SetStringField(TEXT("schema"), PcgTileCapSchema);
		Policy->SetNumberField(TEXT("max_tiles_per_generate"), PcgMaxTilesPerGenerate);
		Policy->SetBoolField(TEXT("strict_or_unattended"), Decision.bStrictOrUnattended);
		Policy->SetBoolField(TEXT("require_tile_evidence"), Decision.bRequireTileEvidence);
		Policy->SetBoolField(TEXT("fail_closed"), Decision.bFailClosed);
		Policy->SetBoolField(TEXT("native_tile_mask_available"), false);
		OutStructured->SetObjectField(TEXT("tile_cap_policy"), Policy);
	}

	static inline void AttachTileEvidenceFields(
		const TSharedRef<FJsonObject>& OutStructured,
		const TSharedRef<FJsonObject>& Arguments)
	{
		const TArray<TSharedPtr<FJsonValue>>* AllowedTiles = nullptr;
		if (Arguments->TryGetArrayField(TEXT("allowed_tiles"), AllowedTiles) && AllowedTiles)
		{
			OutStructured->SetArrayField(TEXT("allowed_tiles"), *AllowedTiles);
		}

		const TArray<TSharedPtr<FJsonValue>>* TileIndices = nullptr;
		if (Arguments->TryGetArrayField(TEXT("tile_indices"), TileIndices) && TileIndices)
		{
			OutStructured->SetArrayField(TEXT("tile_indices"), *TileIndices);
		}

		int32 TileCount = 0;
		if (JsonValueAsInt(Arguments->TryGetField(TEXT("tile_count")), TileCount))
		{
			OutStructured->SetNumberField(TEXT("tile_count"), TileCount);
		}

		double AreaM2 = 0.0;
		if (Arguments->TryGetNumberField(TEXT("area_m2"), AreaM2))
		{
			OutStructured->SetNumberField(TEXT("area_m2"), AreaM2);
		}

		double TileSizeM = 0.0;
		if (Arguments->TryGetNumberField(TEXT("tile_size_m"), TileSizeM))
		{
			OutStructured->SetNumberField(TEXT("tile_size_m"), TileSizeM);
		}

		FString TraceId;
		if (Arguments->TryGetStringField(TEXT("trace_id"), TraceId) && !TraceId.IsEmpty())
		{
			OutStructured->SetStringField(TEXT("tile_evidence_trace_id"), TraceId);
		}
	}

	static inline void AttachPcgEditorSubwindowCleanupEvidence(
		const TSharedRef<FJsonObject>& OutStructured,
		const FString& ToolName)
	{
		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("tool"), ToolName);
		Evidence->SetBoolField(TEXT("opened_editor_subwindows"), false);
		Evidence->SetBoolField(TEXT("close_attempted"), false);
		Evidence->SetBoolField(TEXT("closed_after_execution"), true);
		Evidence->SetStringField(TEXT("status"), TEXT("not_needed_no_editor_subwindows_opened"));
		Evidence->SetStringField(
			TEXT("evidence"),
			TEXT("This PCG tool path uses direct asset/component APIs and does not open PCG graph editor tabs or floating editor windows."));
		OutStructured->SetObjectField(TEXT("pcg_editor_subwindow_cleanup"), Evidence);
		OutStructured->SetBoolField(TEXT("pcg_editor_subwindows_closed_after_execution"), true);

		TSharedRef<FJsonObject> Watchdog = MakeShared<FJsonObject>();
		Watchdog->SetStringField(TEXT("tool"), ToolName);
		Watchdog->SetStringField(TEXT("schema"), TEXT("somol.editor_dialog_watchdog_evidence.v1"));
		Watchdog->SetStringField(TEXT("status"), TEXT("not_needed_direct_api_path"));
		Watchdog->SetBoolField(TEXT("preflight_required_on_timeout"), true);
		Watchdog->SetBoolField(TEXT("modal_detected"), false);
		Watchdog->SetBoolField(TEXT("safe_close_attempted"), false);
		Watchdog->SetStringField(
			TEXT("evidence"),
			TEXT("No editor subwindow was opened by this tool path. If initialize/tools/list or follow-up polling times out while the MCP port is listening, run editor_dialog_watchdog_tick dry_run before retrying."));
		OutStructured->SetObjectField(TEXT("editor_dialog_watchdog_evidence"), Watchdog);
	}

	static inline FString InferGraphCategoryFromText(const FString& Text)
	{
		if (Text.Contains(TEXT("terrain"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("landscape"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("height"), ESearchCase::IgnoreCase))
		{
			return TEXT("terrain");
		}
		if (Text.Contains(TEXT("forest"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("foliage"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("vegetation"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("grass"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("tree"), ESearchCase::IgnoreCase))
		{
			return TEXT("vegetation");
		}
		if (Text.Contains(TEXT("river"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("spline"), ESearchCase::IgnoreCase) ||
			Text.Contains(TEXT("road"), ESearchCase::IgnoreCase))
		{
			return TEXT("spline_corridor");
		}
		if (Text.Contains(TEXT("biome"), ESearchCase::IgnoreCase))
		{
			return TEXT("biome");
		}
		return TEXT("pcg_graph");
	}

	static inline TArray<TSharedPtr<FJsonValue>> StringArrayToJsonValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static inline TSharedRef<FJsonObject> MakeRequiredEvidenceJson(
		const FString& Phase,
		const FString& Status,
		const FString& Tool,
		const FString& Detail)
	{
		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("phase"), Phase);
		Evidence->SetStringField(TEXT("status"), Status);
		Evidence->SetStringField(TEXT("tool"), Tool);
		Evidence->SetStringField(TEXT("detail"), Detail);
		return Evidence;
	}

	static inline TSharedRef<FJsonObject> MakeTileCapReceiptJson(const FTileCapDecision& TileCap)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), PcgTileCapSchema);
		Receipt->SetStringField(TEXT("status"), TileCap.Status);
		Receipt->SetStringField(TEXT("observed_source"), TileCap.ObservedSource);
		Receipt->SetStringField(TEXT("reason"), TileCap.Reason);
		Receipt->SetNumberField(TEXT("observed_tile_count"), TileCap.ObservedTileCount >= 0 ? TileCap.ObservedTileCount : 0);
		Receipt->SetBoolField(TEXT("tile_count_known"), TileCap.ObservedTileCount >= 0);
		Receipt->SetBoolField(TEXT("strict_or_unattended"), TileCap.bStrictOrUnattended);
		Receipt->SetBoolField(TEXT("fail_closed"), TileCap.bFailClosed);
		Receipt->SetNumberField(TEXT("max_tiles_per_generate"), PcgMaxTilesPerGenerate);
		return Receipt;
	}

	static inline void AttachGenerateReceiptEnvelope(
		const TSharedRef<FJsonObject>& OutStructured,
		const TSharedRef<FJsonObject>& Arguments,
		const FGenerateTargetSet& Targets,
		const FTileCapDecision& TileCap,
		int32 GeneratedComponentCount,
		int32 SkippedComponentCount)
	{
		TArray<FString> GraphCategories;
		for (const FString& GraphPath : Targets.UniqueGraphPaths)
		{
			GraphCategories.AddUnique(InferGraphCategoryFromText(GraphPath));
		}
		if (GraphCategories.Num() == 0)
		{
			GraphCategories.Add(InferGraphCategoryFromText(Targets.RequestedGraphPath + TEXT(" ") + Targets.RequestedActor));
		}

		const int32 RequestedTileCount = TileCap.ObservedTileCount >= 0 ? TileCap.ObservedTileCount : 0;
		const bool bGenerationPending = GeneratedComponentCount < 0;
		const int32 SafeGeneratedComponentCount = FMath::Max(0, GeneratedComponentCount);
		const int32 GeneratedTileCount = bGenerationPending ? 0 : (RequestedTileCount > 0 && SafeGeneratedComponentCount > 0 ? RequestedTileCount : 0);
		const int32 SkippedTileCount = bGenerationPending ? 0 : (RequestedTileCount > 0 ? FMath::Max(0, RequestedTileCount - GeneratedTileCount) : 0);

		TSharedRef<FJsonObject> TileMask = MakeShared<FJsonObject>();
		TileMask->SetBoolField(TEXT("requested"), RequestedTileCount > 0);
		TileMask->SetStringField(TEXT("observed_source"), TileCap.ObservedSource);
		TileMask->SetNumberField(TEXT("requested_tile_count"), RequestedTileCount);
		TileMask->SetNumberField(TEXT("generated_tile_count"), GeneratedTileCount);
		TileMask->SetNumberField(TEXT("skipped_tile_count"), SkippedTileCount);
		TileMask->SetBoolField(TEXT("native_tile_mask_enforced"), false);
		TileMask->SetStringField(TEXT("status"), bGenerationPending ? TEXT("pending_async_job_poll") : (RequestedTileCount > 0 ? TEXT("receipt_evidence_only_actor_scope_generate") : TEXT("missing_or_not_requested")));
		TileMask->SetStringField(TEXT("note"), TEXT("Native PCG generation is actor/component scoped; tile fields are receipt evidence and cap gates."));
		OutStructured->SetObjectField(TEXT("tile_mask_receipt"), TileMask);

		TSharedRef<FJsonObject> Calibration = MakeShared<FJsonObject>();
		Calibration->SetStringField(TEXT("schema"), TEXT("somol.pcg.generate_calibration_fields.v1"));
		double DryRunEstimate = 0.0;
		if (Arguments->TryGetNumberField(TEXT("dry_run_estimated_points"), DryRunEstimate) ||
			Arguments->TryGetNumberField(TEXT("estimated_spawned_points"), DryRunEstimate) ||
			Arguments->TryGetNumberField(TEXT("estimated_points"), DryRunEstimate))
		{
			Calibration->SetNumberField(TEXT("dry_run_estimated_points"), DryRunEstimate);
		}
		Calibration->SetNumberField(TEXT("actual_generated_component_count"), SafeGeneratedComponentCount);
		Calibration->SetBoolField(TEXT("actual_spawned_points_available"), false);
		Calibration->SetStringField(TEXT("actual_spawned_points_source"), TEXT("pending_pcg_spawned_actor_index_or_attribute_inspect"));
		Calibration->SetStringField(TEXT("status"), bGenerationPending ? TEXT("pending_async_job_poll") : TEXT("pending_post_generate_readback"));
		Calibration->SetStringField(TEXT("next_step"), TEXT("Attach pcg_spawned_actor_index, pcg_generated_actor_health_audit, pcg_spawned_actor_index_repair, and dry-run calibration receipt before closing QA."));
		OutStructured->SetObjectField(TEXT("dry_run_vs_actual_calibration"), Calibration);

		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somol.pcg.generate_receipt.v2"));
		Receipt->SetArrayField(TEXT("graph_paths"), StringArrayToJsonValues(Targets.UniqueGraphPaths));
		Receipt->SetArrayField(TEXT("graph_categories"), StringArrayToJsonValues(GraphCategories));
		Receipt->SetNumberField(TEXT("requested_component_count"), Targets.Components.Num());
		Receipt->SetNumberField(TEXT("generated_component_count"), SafeGeneratedComponentCount);
		Receipt->SetNumberField(TEXT("skipped_component_count"), SkippedComponentCount);
		Receipt->SetObjectField(TEXT("tile_mask"), TileMask);
		Receipt->SetObjectField(TEXT("calibration"), Calibration);
		Receipt->SetObjectField(TEXT("catalog_evidence"), MakeRequiredEvidenceJson(
			TEXT("catalog"),
			TEXT("required_before_generate"),
			TEXT("pcg_node_catalog_lookup"),
			TEXT("Each generate preflight report includes catalog_preflight evidence proving the live PCG settings catalog was reachable before generation.")));
		Receipt->SetObjectField(TEXT("validate_evidence"), MakeRequiredEvidenceJson(
			TEXT("validate"),
			TEXT("required_before_generate"),
			TEXT("pcg_graph_validate"),
			TEXT("Top-level validation[] contains one report per resolved graph; generation is blocked on validate failure.")));
		Receipt->SetObjectField(TEXT("dry_run_evidence"), MakeRequiredEvidenceJson(
			TEXT("dry_run"),
			TEXT("required_before_generate"),
			TEXT("pcg_dry_run"),
			TEXT("Top-level validation[].dry_run contains the point-budget screen; generation is blocked if dry-run fails or exceeds budget.")));
		Receipt->SetObjectField(TEXT("tile_cap_evidence"), MakeTileCapReceiptJson(TileCap));
		Receipt->SetObjectField(TEXT("spawn_evidence"), MakeRequiredEvidenceJson(
			TEXT("spawn"),
			bGenerationPending ? TEXT("pending_async_job_poll") : TEXT("generate_triggered"),
			TEXT("pcg_generate"),
			TEXT("results[] contains generated component provenance; exact spawned actors require post-generate index/readback.")));
		Receipt->SetObjectField(TEXT("readback_evidence"), MakeRequiredEvidenceJson(
			TEXT("readback"),
			bGenerationPending ? TEXT("pending_async_job_poll") : TEXT("pending_post_generate_readback"),
			TEXT("pcg_spawned_actor_index + pcg_generated_actor_health_audit"),
			TEXT("Close QA only after spawned actor index/provenance and generated actor health readback are attached.")));
		Receipt->SetStringField(TEXT("generated_actor_health_required_tool"), TEXT("pcg_generated_actor_health_audit"));
		Receipt->SetStringField(TEXT("generated_actor_provenance_required_tool"), TEXT("pcg_spawned_actor_index"));
		Receipt->SetStringField(TEXT("generated_actor_index_repair_required_tool"), TEXT("pcg_spawned_actor_index_repair"));
		Receipt->SetStringField(TEXT("watchdog_required_tool_on_timeout"), TEXT("editor_dialog_watchdog_tick"));
		Receipt->SetBoolField(TEXT("receipt_gate_complete"), false);
		Receipt->SetStringField(TEXT("receipt_gate_status"), bGenerationPending ? TEXT("pending_async_job_poll") : TEXT("pending_post_generate_readback"));
		OutStructured->SetObjectField(TEXT("generate_receipt"), Receipt);
		OutStructured->SetStringField(TEXT("generate_receipt_schema"), TEXT("somol.pcg.generate_receipt.v2"));
	}

	static inline AActor* FindUniquePartialActor(UWorld* World, const FString& ActorId, FString& OutError)
	{
		AActor* Match = nullptr;
		int32 MatchCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetActorLabel().Contains(ActorId) || Actor->GetName().Contains(ActorId))
			{
				if (!Match)
				{
					Match = Actor;
				}
				++MatchCount;
			}
		}

		if (MatchCount == 1)
		{
			return Match;
		}
		if (MatchCount > 1)
		{
			OutError = FString::Printf(
				TEXT("Ambiguous actor_label '%s': matched %d actors. Pass actor with an exact actor path/name, or a more specific actor_label."),
				*ActorId,
				MatchCount);
			return nullptr;
		}

		OutError = FString::Printf(TEXT("Failed to find actor: %s"), *ActorId);
		return nullptr;
	}

	static inline FString ResolveComponentGraphPath(UPCGComponent* Component)
	{
		if (!Component)
		{
			return FString();
		}

		UPCGGraphInterface* GraphInterface = Component->GetGraphInstance();
		if (!GraphInterface)
		{
			return FString();
		}

		UPCGGraph* Graph = GraphInterface->GetGraph();
		return Graph ? PackagePathForCompare(Graph->GetPathName()) : FString();
	}

	static inline void AddUniqueGraphPath(FGenerateTargetSet& TargetSet, const FString& GraphPath)
	{
		for (const FString& Existing : TargetSet.UniqueGraphPaths)
		{
			if (GraphPathsMatch(Existing, GraphPath))
			{
				return;
			}
		}
		TargetSet.UniqueGraphPaths.Add(GraphPath);
	}

	static inline void CollectActorComponents(
		AActor* Actor,
		const FString& RequestedGraphPath,
		FGenerateTargetSet& TargetSet)
	{
		if (!Actor)
		{
			return;
		}

		TArray<UPCGComponent*> Components;
		Actor->GetComponents<UPCGComponent>(Components);
		if (Components.Num() == 0)
		{
			AddWarning(
				TargetSet.Warnings,
				TEXT("actor_without_pcg_component"),
				FString::Printf(TEXT("Actor '%s' has no UPCGComponent."), *Actor->GetActorLabel()));
			return;
		}

		for (UPCGComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}

			const FString GraphPath = ResolveComponentGraphPath(Component);
			if (GraphPath.IsEmpty())
			{
				AddWarning(
					TargetSet.Warnings,
					TEXT("pcg_component_without_graph"),
					FString::Printf(
						TEXT("Actor '%s' component '%s' has no resolved PCG graph and will not be generated."),
						*Actor->GetActorLabel(),
						*Component->GetName()));
				continue;
			}

			if (!RequestedGraphPath.IsEmpty() && !GraphPathsMatch(RequestedGraphPath, GraphPath))
			{
				continue;
			}

			FGenerateComponentTarget Target;
			Target.Actor = Actor;
			Target.Component = Component;
			Target.ActorLabel = Actor->GetActorLabel();
			Target.ActorName = Actor->GetName();
			Target.ActorPath = Actor->GetPathName();
			Target.ComponentName = Component->GetName();
			Target.GraphPath = GraphPath;
			TargetSet.Components.Add(Target);
			AddUniqueGraphPath(TargetSet, GraphPath);
		}
	}

	static inline bool ResolveGenerateTargets(
		FSololmcpEditorServices& Services,
		const TSharedRef<FJsonObject>& Arguments,
		FGenerateTargetSet& OutTargets,
		FString& OutError)
	{
		OutTargets = FGenerateTargetSet();
		OutTargets.RequestedActor = ReadActorArgument(Arguments, OutTargets.RequestedActorField);
		OutTargets.RequestedGraphPath = ReadGraphPathArgument(Arguments);
		Arguments->TryGetBoolField(TEXT("allow_all"), OutTargets.bAllowAll);
		Arguments->TryGetBoolField(TEXT("allow_partial_actor_label"), OutTargets.bAllowPartialActorLabel);

		if (OutTargets.RequestedActor.IsEmpty() && OutTargets.RequestedGraphPath.IsEmpty() && !OutTargets.bAllowAll)
		{
			OutError = TEXT("pcg_generate requires actor/actor_label or graph_path. Pass allow_all=true to intentionally target every resolved PCG component.");
			return false;
		}

		FString WorldError;
		UWorld* World = Services.GetEditorWorld(WorldError);
		if (!World)
		{
			OutError = WorldError.IsEmpty() ? TEXT("No editor world.") : WorldError;
			return false;
		}

		if (!OutTargets.RequestedActor.IsEmpty())
		{
			FString ActorError;
			AActor* Actor = Services.FindActorByLabelOrName(OutTargets.RequestedActor, ActorError);
			if (!Actor && (OutTargets.RequestedActorField == TEXT("actor_label") || OutTargets.bAllowPartialActorLabel))
			{
				Actor = FindUniquePartialActor(World, OutTargets.RequestedActor, ActorError);
			}
			if (!Actor)
			{
				OutError = ActorError;
				return false;
			}

			CollectActorComponents(Actor, OutTargets.RequestedGraphPath, OutTargets);
		}
		else
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				CollectActorComponents(*It, OutTargets.RequestedGraphPath, OutTargets);
			}
		}

		if (OutTargets.Components.Num() == 0)
		{
			if (!OutTargets.RequestedGraphPath.IsEmpty() && !OutTargets.RequestedActor.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("No PCG component on actor '%s' uses graph_path '%s'."),
					*OutTargets.RequestedActor,
					*OutTargets.RequestedGraphPath);
			}
			else if (!OutTargets.RequestedGraphPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("No PCG component uses graph_path '%s'."), *OutTargets.RequestedGraphPath);
			}
			else if (!OutTargets.RequestedActor.IsEmpty())
			{
				OutError = FString::Printf(TEXT("No resolved PCG graph found on actor '%s'."), *OutTargets.RequestedActor);
			}
			else
			{
				OutError = TEXT("No resolved PCG components found in the level.");
			}
			return false;
		}

		if (OutTargets.RequestedActor.IsEmpty() && !OutTargets.bAllowAll && OutTargets.Components.Num() > 1)
		{
			OutError = FString::Printf(
				TEXT("graph_path resolved to %d PCG components. Pass actor/actor_label to target one actor, or allow_all=true to intentionally generate all matches."),
				OutTargets.Components.Num());
			return false;
		}

		return true;
	}

	static inline TArray<TSharedPtr<FJsonValue>> GraphPathsToJson(const TArray<FString>& GraphPaths)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& GraphPath : GraphPaths)
		{
			Values.Add(MakeShared<FJsonValueString>(GraphPath));
		}
		return Values;
	}

	static inline void AttachResolutionFields(
		const TSharedRef<FJsonObject>& OutStructured,
		const FGenerateTargetSet& Targets)
	{
		OutStructured->SetStringField(TEXT("pcg_safety_mode"), TEXT("resolve_graph_then_validate"));
		if (!Targets.RequestedActor.IsEmpty())
		{
			OutStructured->SetStringField(TEXT("requested_actor"), Targets.RequestedActor);
			OutStructured->SetStringField(TEXT("requested_actor_field"), Targets.RequestedActorField);
		}
		if (!Targets.RequestedGraphPath.IsEmpty())
		{
			OutStructured->SetStringField(TEXT("requested_graph_path"), Targets.RequestedGraphPath);
		}
		OutStructured->SetBoolField(TEXT("allow_all"), Targets.bAllowAll);
		OutStructured->SetNumberField(TEXT("target_component_count"), Targets.Components.Num());
		OutStructured->SetNumberField(TEXT("target_graph_count"), Targets.UniqueGraphPaths.Num());
		OutStructured->SetArrayField(TEXT("graph_paths"), GraphPathsToJson(Targets.UniqueGraphPaths));
		if (Targets.UniqueGraphPaths.Num() == 1)
		{
			OutStructured->SetStringField(TEXT("graph_path"), Targets.UniqueGraphPaths[0]);
		}
		if (Targets.Warnings.Num() > 0)
		{
			OutStructured->SetArrayField(TEXT("warnings"), Targets.Warnings);
		}
		if (Targets.Components.Num() == 1)
		{
			const FGenerateComponentTarget& Target = Targets.Components[0];
			OutStructured->SetStringField(TEXT("actor"), Target.ActorLabel);
			OutStructured->SetStringField(TEXT("actor_name"), Target.ActorName);
			OutStructured->SetStringField(TEXT("actor_path"), Target.ActorPath);
			OutStructured->SetStringField(TEXT("pcg_component"), Target.ComponentName);
		}
	}

	static inline bool GraphDryRunBlocksGenerate(
		const TSharedRef<FJsonObject>& DryRunOut,
		FString& OutReason)
	{
		const TSharedPtr<FJsonObject>* BudgetStatus = nullptr;
		if (DryRunOut->TryGetObjectField(TEXT("budget_status"), BudgetStatus) && BudgetStatus && BudgetStatus->IsValid())
		{
			bool bOverBudget = false;
			if ((*BudgetStatus)->TryGetBoolField(TEXT("over_budget"), bOverBudget) && bOverBudget)
			{
				OutReason = TEXT("pcg_dry_run_over_budget");
				return true;
			}
		}

		double EstimatedSpawned = 0.0;
		if (DryRunOut->TryGetNumberField(TEXT("estimated_spawned_points"), EstimatedSpawned) && EstimatedSpawned > 1000000.0)
		{
			OutReason = FString::Printf(TEXT("pcg_dry_run_estimated_spawned_points_exceeds_cap: %.0f > 1000000"), EstimatedSpawned);
			return true;
		}
		return false;
	}

	static inline bool ValidateGraphPathsForGenerate(
		FSololmcpToolRegistry& Registry,
		const TArray<FString>& GraphPaths,
		TArray<TSharedPtr<FJsonValue>>& OutValidationReports,
		FString& OutError)
	{
		if (GraphPaths.Num() == 0)
		{
			OutError = TEXT("No graph_path resolved for PCG generation.");
			return false;
		}

		for (const FString& GraphPath : GraphPaths)
		{
			TSharedRef<FJsonObject> CatalogArgs = MakeShared<FJsonObject>();
			CatalogArgs->SetStringField(TEXT("query"), TEXT("sampler"));
			CatalogArgs->SetStringField(TEXT("category"), TEXT("sampling"));
			CatalogArgs->SetBoolField(TEXT("include_pins"), true);
			CatalogArgs->SetNumberField(TEXT("limit"), 8);

			TSharedRef<FJsonObject> CatalogOut = MakeShared<FJsonObject>();
			FString CatalogSummary;
			FString CatalogError;
			const bool bCatalogOk = Registry.ExecuteTool(
				TEXT("pcg_node_catalog_lookup"),
				CatalogArgs,
				CatalogOut,
				CatalogSummary,
				CatalogError);

			TSharedRef<FJsonObject> DryRunArgs = MakeShared<FJsonObject>();
			DryRunArgs->SetStringField(TEXT("asset_path"), GraphPath);
			TSharedRef<FJsonObject> DryRunOut = MakeShared<FJsonObject>();
			FString DryRunSummary;
			FString DryRunError;
			const bool bDryRunOk = Registry.ExecuteTool(
				TEXT("pcg_dry_run"),
				DryRunArgs,
				DryRunOut,
				DryRunSummary,
				DryRunError);

			TSharedRef<FJsonObject> ValidateArgs = MakeShared<FJsonObject>();
			ValidateArgs->SetStringField(TEXT("asset_path"), GraphPath);

			TSharedRef<FJsonObject> ValidateOut = MakeShared<FJsonObject>();
			FString ValidateSummary;
			FString ValidateError;
			const bool bValidateOk = Registry.ExecuteTool(
				TEXT("pcg_graph_validate"),
				ValidateArgs,
				ValidateOut,
				ValidateSummary,
				ValidateError);

			TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetStringField(TEXT("graph_path"), GraphPath);
			Report->SetStringField(TEXT("schema"), TEXT("somol.pcg.generate_preflight_report.v1"));
			Report->SetBoolField(TEXT("catalog_ok"), bCatalogOk);
			Report->SetBoolField(TEXT("dry_run_ok"), bDryRunOk);
			Report->SetBoolField(TEXT("validate_ok"), bValidateOk);
			Report->SetBoolField(TEXT("ok"), bCatalogOk && bDryRunOk && bValidateOk);
			if (!CatalogSummary.IsEmpty())
			{
				Report->SetStringField(TEXT("catalog_summary"), CatalogSummary);
			}
			if (!CatalogError.IsEmpty())
			{
				Report->SetStringField(TEXT("catalog_error"), CatalogError);
			}
			if (!DryRunSummary.IsEmpty())
			{
				Report->SetStringField(TEXT("dry_run_summary"), DryRunSummary);
			}
			if (!DryRunError.IsEmpty())
			{
				Report->SetStringField(TEXT("dry_run_error"), DryRunError);
			}
			if (!ValidateSummary.IsEmpty())
			{
				Report->SetStringField(TEXT("summary"), ValidateSummary);
			}
			if (!ValidateError.IsEmpty())
			{
				Report->SetStringField(TEXT("error"), ValidateError);
			}
			Report->SetObjectField(TEXT("catalog_preflight"), CatalogOut);
			Report->SetObjectField(TEXT("dry_run"), DryRunOut);
			Report->SetObjectField(TEXT("result"), ValidateOut);
			OutValidationReports.Add(MakeShared<FJsonValueObject>(Report));

			if (!bCatalogOk)
			{
				OutError = CatalogError.IsEmpty()
					? FString::Printf(TEXT("pcg_node_catalog_lookup failed before generate for '%s'."), *GraphPath)
					: FString::Printf(TEXT("pcg_node_catalog_lookup failed before generate for '%s': %s"), *GraphPath, *CatalogError);
				return false;
			}

			if (!bDryRunOk)
			{
				OutError = DryRunError.IsEmpty()
					? FString::Printf(TEXT("pcg_dry_run failed before generate for '%s'."), *GraphPath)
					: FString::Printf(TEXT("pcg_dry_run failed before generate for '%s': %s"), *GraphPath, *DryRunError);
				return false;
			}

			FString DryRunBlockReason;
			if (GraphDryRunBlocksGenerate(DryRunOut, DryRunBlockReason))
			{
				OutError = FString::Printf(TEXT("pcg_dry_run blocked generate for '%s': %s"), *GraphPath, *DryRunBlockReason);
				return false;
			}

			if (!bValidateOk)
			{
				OutError = ValidateError.IsEmpty()
					? FString::Printf(TEXT("pcg_graph_validate failed for '%s'."), *GraphPath)
					: FString::Printf(TEXT("pcg_graph_validate failed for '%s': %s"), *GraphPath, *ValidateError);
				return false;
			}
		}

		return true;
	}
}
