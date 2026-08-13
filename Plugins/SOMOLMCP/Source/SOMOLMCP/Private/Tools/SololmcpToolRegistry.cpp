#include "Tools/SololmcpToolRegistry.h"
#include "Tools/SololmcpHlodForceRebuildShim.h"
#include "Tools/SololmcpAnimationCompletionTools.h"
#include "Tools/SololmcpAuthoringQaPcgCompletionTools.h"
#include "Tools/SololmcpFoliageCompletionTools.h"
#include "Tools/SololmcpMeshTerrainCompletionTools.h"
#include "Tools/SololmcpModelingCompletionTools.h"
#include "SOMOLMCP.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpTerrainProofV2.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h"

namespace UE::SOMOLMCP
{
	void RegisterWorldForgeRevCGapTools(FSololmcpToolRegistry& Registry);
	bool IsExternalPythonSurfaceToolName(const FString& ToolName)
	{
		const FString Normalized = ToolName.TrimStartAndEnd().ToLower();
		return Normalized.Contains(TEXT("python"))
			|| Normalized == TEXT("unreal_call");
	}

	bool IsLegacyPythonBackendToolName(const FString& ToolName)
	{
		static const TSet<FString> HiddenNames = {
#include "SololmcpLegacyPythonBackendNames.inl"
		};
		return HiddenNames.Contains(ToolName);
	}

	namespace
	{
		FCriticalSection RuntimeSnapshotLock;
		FSololmcpToolRuntimeSnapshot RuntimeSnapshot;

		FString MakeArgumentsPreview(const TSharedRef<FJsonObject>& Arguments)
		{
			FString Json;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
			FJsonSerializer::Serialize(Arguments, Writer);
			Json.ReplaceInline(TEXT("\r"), TEXT(" "));
			Json.ReplaceInline(TEXT("\n"), TEXT(" "));
			return Json.Left(220);
		}

		void MarkToolStart(const FString& ToolName, const TSharedRef<FJsonObject>& Arguments)
		{
			FScopeLock Lock(&RuntimeSnapshotLock);
			RuntimeSnapshot.CurrentToolName = ToolName;
			RuntimeSnapshot.CurrentToolArgumentsPreview = MakeArgumentsPreview(Arguments);
			RuntimeSnapshot.ActiveToolExecutions = FMath::Max(0, RuntimeSnapshot.ActiveToolExecutions) + 1;
			++RuntimeSnapshot.TotalToolExecutionsStarted;
		}

		void MarkToolEnd(const FString& ToolName, bool bSuccess, double ElapsedMs)
		{
			FScopeLock Lock(&RuntimeSnapshotLock);
			RuntimeSnapshot.LastToolName = ToolName;
			RuntimeSnapshot.bLastToolSuccess = bSuccess;
			RuntimeSnapshot.LastToolElapsedMs = ElapsedMs;
			RuntimeSnapshot.ActiveToolExecutions = FMath::Max(0, RuntimeSnapshot.ActiveToolExecutions - 1);
			++RuntimeSnapshot.TotalToolExecutionsCompleted;
			if (bSuccess)
			{
				++RuntimeSnapshot.TotalToolExecutionsSucceeded;
			}
			else
			{
				++RuntimeSnapshot.TotalToolExecutionsFailed;
			}
			if (RuntimeSnapshot.ActiveToolExecutions == 0)
			{
				RuntimeSnapshot.CurrentToolName.Reset();
				RuntimeSnapshot.CurrentToolArgumentsPreview.Reset();
			}
		}

		bool HasFailureBoolField(
			const TSharedRef<FJsonObject>& Structured,
			const TCHAR* FieldName,
			const bool bFailureValue)
		{
			bool bValue = false;
			return Structured->TryGetBoolField(FieldName, bValue) && bValue == bFailureValue;
		}

		bool NormalizeToolSuccess(
			const TSharedRef<FJsonObject>& Structured,
			FString& InOutSummary,
			FString& InOutError)
		{
			FString FieldName;
			if (HasFailureBoolField(Structured, TEXT("isError"), true))
			{
				FieldName = TEXT("isError");
			}
			else if (HasFailureBoolField(Structured, TEXT("ok"), false))
			{
				FieldName = TEXT("ok");
			}
			else if (HasFailureBoolField(Structured, TEXT("success"), false))
			{
				FieldName = TEXT("success");
			}
			else if (HasFailureBoolField(Structured, TEXT("passed"), false))
			{
				FieldName = TEXT("passed");
			}

			if (FieldName.IsEmpty())
			{
				return true;
			}

			if (InOutError.IsEmpty())
			{
				FString StructuredError;
				if (!Structured->TryGetStringField(TEXT("error"), StructuredError))
				{
					Structured->TryGetStringField(TEXT("message"), StructuredError);
				}
				InOutError = StructuredError.IsEmpty()
					? FString::Printf(TEXT("Tool reported failure via structured field '%s'."), *FieldName)
					: StructuredError;
			}
			if (InOutSummary.IsEmpty())
			{
				InOutSummary = FString::Printf(TEXT("Tool failed (%s)."), *FieldName);
			}
			return false;
		}

		bool IsSchemaStringProperty(
			const TSharedRef<FJsonObject>& InputSchema,
			const FString& FieldName)
		{
			const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
			if (!InputSchema->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid())
			{
				return false;
			}

			const TSharedPtr<FJsonObject>* PropertyPtr = nullptr;
			if (!(*PropertiesPtr)->TryGetObjectField(FieldName, PropertyPtr) || !PropertyPtr || !PropertyPtr->IsValid())
			{
				return false;
			}

			FString Type;
			return (*PropertyPtr)->TryGetStringField(TEXT("type"), Type) && Type == TEXT("string");
		}

		bool ValidateRequiredArguments(
			const FSololmcpToolDefinition& Tool,
			const TSharedRef<FJsonObject>& Arguments,
			FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* RequiredFields = nullptr;
			if (!Tool.InputSchema->TryGetArrayField(TEXT("required"), RequiredFields) || !RequiredFields)
			{
				return true;
			}

			TArray<FString> MissingFields;
			for (const TSharedPtr<FJsonValue>& RequiredValue : *RequiredFields)
			{
				if (!RequiredValue.IsValid())
				{
					continue;
				}

				const FString FieldName = RequiredValue->AsString();
				if (FieldName.IsEmpty())
				{
					continue;
				}

				const TSharedPtr<FJsonValue> ArgValue = Arguments->TryGetField(FieldName);
				if (!ArgValue.IsValid() || ArgValue->IsNull())
				{
					MissingFields.Add(FieldName);
					continue;
				}

				if (IsSchemaStringProperty(Tool.InputSchema, FieldName))
				{
					FString StringValue;
					if (!Arguments->TryGetStringField(FieldName, StringValue) || StringValue.TrimStartAndEnd().IsEmpty())
					{
						MissingFields.Add(FieldName);
					}
				}
			}

			if (MissingFields.IsEmpty())
			{
				return true;
			}

			OutError = FString::Printf(
				TEXT("Missing required argument%s: %s"),
				MissingFields.Num() == 1 ? TEXT("") : TEXT("s"),
				*FString::Join(MissingFields, TEXT(", ")));
			return false;
		}

		bool IsLikelyMutationTool(const FString& ToolName)
		{
			static const TArray<FString> MutationPrefixes = {
				TEXT("add_"), TEXT("apply_"), TEXT("assign_"), TEXT("attach_"), TEXT("bind_"),
				TEXT("build_"), TEXT("compile_"), TEXT("connect_"), TEXT("create_"), TEXT("delete_"),
				TEXT("destroy_"), TEXT("disable_"), TEXT("disconnect_"), TEXT("duplicate_"), TEXT("enable_"),
				TEXT("fill_"), TEXT("generate_"), TEXT("import_"), TEXT("insert_"), TEXT("load_"), TEXT("move_"),
				TEXT("paint_"), TEXT("place_"), TEXT("remove_"), TEXT("rename_"), TEXT("repair_"),
				TEXT("reset_"), TEXT("resize_"), TEXT("restore_"), TEXT("save_"), TEXT("set_"),
				TEXT("spawn_"), TEXT("start_"), TEXT("stop_"), TEXT("sync_"), TEXT("update_"),
				TEXT("write_"), TEXT("mutate_"), TEXT("rebuild_"), TEXT("execute_"), TEXT("commit_"),
				TEXT("rollback_"), TEXT("invalidate_"), TEXT("cleanup_"), TEXT("cancel_"), TEXT("expand_"),
				TEXT("split_"), TEXT("stitch_"), TEXT("merge_"), TEXT("resection_")
			};
			static const TArray<FString> MutationTokens = {
				TEXT("_add_"), TEXT("_apply_"), TEXT("_assign_"), TEXT("_attach_"), TEXT("_bind_"),
				TEXT("_build"), TEXT("_compile"), TEXT("_connect_"), TEXT("_create"), TEXT("_delete"),
				TEXT("_destroy"), TEXT("_disable"), TEXT("_disconnect"), TEXT("_duplicate"),
				TEXT("_enable"), TEXT("_fill_"), TEXT("_generate"), TEXT("_import"), TEXT("_insert_"), TEXT("_move_"),
				TEXT("_paint_"), TEXT("_place_"), TEXT("_remove"), TEXT("_rename"), TEXT("_repair"),
				TEXT("_reset"), TEXT("_resize"), TEXT("_restore"), TEXT("_save"), TEXT("_set_"),
				TEXT("_spawn"), TEXT("_start"), TEXT("_stop"), TEXT("_sync"), TEXT("_update"),
				TEXT("_write"), TEXT("_mutate"), TEXT("_rebuild"), TEXT("_execute"), TEXT("_commit"),
				TEXT("_rollback"), TEXT("_invalidate"), TEXT("_cleanup"), TEXT("_cancel"), TEXT("_expand"),
				TEXT("_split"), TEXT("_stitch"), TEXT("_merge"), TEXT("_resection")
			};

			for (const FString& Prefix : MutationPrefixes)
			{
				if (ToolName.StartsWith(Prefix))
				{
					return true;
				}
			}
			for (const FString& Token : MutationTokens)
			{
				if (ToolName.Contains(Token))
				{
					return true;
				}
			}
			return false;
		}

		bool IsAssetNamingWriteTool(const FString& ToolName)
		{
			if (!IsLikelyMutationTool(ToolName))
			{
				return false;
			}

			static const TArray<FString> AssetLifecycleTokens = {
				TEXT("asset_import"), TEXT("asset_ingest"), TEXT("asset_rename"),
				TEXT("asset_duplicate"), TEXT("asset_move"), TEXT("create_asset"),
				TEXT("blueprint_create"), TEXT("material_create"), TEXT("material_instance_create"),
				TEXT("texture_create"), TEXT("texture_import"), TEXT("staticmesh_create"),
				TEXT("skeletalmesh_create"), TEXT("skelmesh_create"), TEXT("niagara_system_create"),
				TEXT("niagara_emitter_create"), TEXT("umg_create"), TEXT("widget_blueprint_create"),
				TEXT("animbp_create"), TEXT("animation_sequence_create"), TEXT("montage_create"),
				TEXT("sequence_create"), TEXT("metasound_create"), TEXT("soundwave_create"),
				TEXT("sound_cue_create"), TEXT("pcg_graph_create"), TEXT("dataasset_create"),
				TEXT("datatable_create"), TEXT("curvetable_create")
			};
			for (const FString& Token : AssetLifecycleTokens)
			{
				if (ToolName.Contains(Token))
				{
					return true;
				}
			}
			return false;
		}

		FString ExpectedAssetPrefix(const FString& ToolName, const FString& FieldName)
		{
			const FString Probe = (ToolName + TEXT("_") + FieldName).ToLower();
			if (Probe.Contains(TEXT("material_instance"))) return TEXT("MI_");
			if (Probe.Contains(TEXT("material_function"))) return TEXT("MF_");
			if (Probe.Contains(TEXT("material_parameter_collection"))) return TEXT("MPC_");
			if (Probe.Contains(TEXT("material"))) return TEXT("M_");
			if (Probe.Contains(TEXT("widget_blueprint")) || Probe.Contains(TEXT("umg"))) return TEXT("WBP_");
			if (Probe.Contains(TEXT("animation_blueprint")) || Probe.Contains(TEXT("animbp"))) return TEXT("ABP_");
			if (Probe.Contains(TEXT("blueprint_interface"))) return TEXT("BI_");
			if (Probe.Contains(TEXT("blueprint"))) return TEXT("BP_");
			if (Probe.Contains(TEXT("niagara_system"))) return TEXT("FXS_");
			if (Probe.Contains(TEXT("niagara_emitter"))) return TEXT("FXE_");
			if (Probe.Contains(TEXT("niagara_function"))) return TEXT("FXF_");
			if (Probe.Contains(TEXT("staticmesh")) || Probe.Contains(TEXT("static_mesh"))) return TEXT("SM_");
			if (Probe.Contains(TEXT("skeletalmesh")) || Probe.Contains(TEXT("skeletal_mesh")) || Probe.Contains(TEXT("skelmesh"))) return TEXT("SK_");
			if (Probe.Contains(TEXT("physics_asset"))) return TEXT("PHYS_");
			if (Probe.Contains(TEXT("texture"))) return TEXT("T_");
			if (Probe.Contains(TEXT("montage"))) return TEXT("AM_");
			if (Probe.Contains(TEXT("animation_sequence")) || Probe.Contains(TEXT("anim_sequence"))) return TEXT("AS_");
			if (Probe.Contains(TEXT("level_sequence")) || Probe.Contains(TEXT("sequence"))) return TEXT("LS_");
			if (Probe.Contains(TEXT("metasound"))) return TEXT("MS_");
			if (Probe.Contains(TEXT("sound_wave")) || Probe.Contains(TEXT("soundwave"))) return TEXT("SW_");
			if (Probe.Contains(TEXT("sound_cue")) || Probe.Contains(TEXT("soundcue"))) return TEXT("SC_");
			if (Probe.Contains(TEXT("pcg_graph"))) return TEXT("PCG_");
			if (Probe.Contains(TEXT("datatable"))) return TEXT("DT_");
			if (Probe.Contains(TEXT("curvetable"))) return TEXT("CT_");
			if (Probe.Contains(TEXT("dataasset"))) return TEXT("DA_");
			return FString();
		}

		bool IsStrictAssetIdentifier(const FString& Name, FString& OutReason)
		{
			if (Name.IsEmpty())
			{
				OutReason = TEXT("asset name is empty");
				return false;
			}

			for (const TCHAR Character : Name)
			{
				const bool bAllowed =
					(Character >= TEXT('A') && Character <= TEXT('Z')) ||
					(Character >= TEXT('a') && Character <= TEXT('z')) ||
					(Character >= TEXT('0') && Character <= TEXT('9')) ||
					Character == TEXT('_');
				if (!bAllowed)
				{
					OutReason = TEXT("identifiers must use ASCII letters, digits, and underscores only");
					return false;
				}
			}

			static const TSet<FString> ForbiddenNames = {
				TEXT("NewAsset"), TEXT("New"), TEXT("Copy"), TEXT("Final"),
				TEXT("Final2"), TEXT("Test"), TEXT("Temp"), TEXT("Untitled")
			};
			if (ForbiddenNames.Contains(Name))
			{
				OutReason = TEXT("placeholder or ambiguous duplicate name is forbidden");
				return false;
			}

			TArray<FString> Parts;
			Name.ParseIntoArray(Parts, TEXT("_"), false);
			if (Parts.Num() < 2)
			{
				OutReason = TEXT("asset name must include an asset-type prefix and base asset name");
				return false;
			}
			for (int32 Index = 1; Index < Parts.Num(); ++Index)
			{
				if (Parts[Index].IsEmpty() || (!FChar::IsUpper(Parts[Index][0]) && !FChar::IsDigit(Parts[Index][0])))
				{
					OutReason = TEXT("base name, descriptor, and named variants must use PascalCase");
					return false;
				}
			}
			return true;
		}

		bool ValidateAssetNamingPolicy(
			const FString& ToolName,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutError)
		{
			if (!IsAssetNamingWriteTool(ToolName))
			{
				return true;
			}

			TArray<FString> CandidateFields = {
				TEXT("asset_name"),
				TEXT("material_name"), TEXT("texture_name"), TEXT("blueprint_name"),
				TEXT("widget_blueprint_name"), TEXT("system_name"), TEXT("emitter_name"),
				TEXT("sequence_name"), TEXT("destination_asset_path"),
				TEXT("output_asset_path"), TEXT("new_asset_path")
			};
			if (ToolName.Contains(TEXT("asset_rename")) || ToolName.Contains(TEXT("rename_asset")))
			{
				CandidateFields.Add(TEXT("new_name"));
			}

			for (const FString& FieldName : CandidateFields)
			{
				FString Value;
				if (!Arguments->TryGetStringField(FieldName, Value) || Value.TrimStartAndEnd().IsEmpty())
				{
					continue;
				}

				FString Candidate = Value.TrimStartAndEnd();
				int32 SlashIndex = INDEX_NONE;
				if (Candidate.FindLastChar(TEXT('/'), SlashIndex))
				{
					Candidate = Candidate.Mid(SlashIndex + 1);
				}
				int32 DotIndex = INDEX_NONE;
				if (Candidate.FindChar(TEXT('.'), DotIndex))
				{
					Candidate = Candidate.Left(DotIndex);
				}

				FString Reason;
				const FString ExpectedPrefix = ExpectedAssetPrefix(ToolName, FieldName);
				if (!IsStrictAssetIdentifier(Candidate, Reason) ||
					(!ExpectedPrefix.IsEmpty() && !Candidate.StartsWith(ExpectedPrefix)))
				{
					if (Reason.IsEmpty())
					{
						Reason = FString::Printf(TEXT("asset type requires prefix %s"), *ExpectedPrefix);
					}
					OutError = FString::Printf(
						TEXT("blocked_asset_naming_policy: %s field '%s' value '%s' violates somol_ue_asset_naming_v1: %s"),
						*ToolName, *FieldName, *Value, *Reason);
					OutStructured->SetBoolField(TEXT("ok"), false);
					OutStructured->SetStringField(TEXT("error_code"), TEXT("blocked_asset_naming_policy"));
					OutStructured->SetStringField(TEXT("policy_id"), TEXT("somol_ue_asset_naming_v1"));
					OutStructured->SetStringField(TEXT("field"), FieldName);
					OutStructured->SetStringField(TEXT("rejected_value"), Value);
					OutStructured->SetStringField(TEXT("expected_prefix"), ExpectedPrefix);
					OutStructured->SetStringField(TEXT("message"), OutError);
					return false;
				}
			}
			return true;
		}

		bool IsTerrainConstraintProofKey(const FString& Key)
		{
			FString Normalized = Key.ToLower();
			Normalized.ReplaceInline(TEXT("_"), TEXT(""));
			Normalized.ReplaceInline(TEXT("-"), TEXT(""));
			return Normalized == TEXT("terrainconstraintproof")
				|| Normalized == TEXT("terrainspecir")
				|| Normalized == TEXT("pregenerationconstraints")
				|| Normalized == TEXT("landformconstraints")
				|| Normalized == TEXT("constraintprofile")
				|| Normalized == TEXT("constrainedheightmaprecipe")
				|| Normalized == TEXT("geomorphologyplan")
				|| Normalized == TEXT("terraingeomorphologyplan")
				|| Normalized == TEXT("terrainspecvalidatereceipt");
		}

		bool IsGenericTerrainConstraintProofContainerKey(const FString& Key)
		{
			FString Normalized = Key.ToLower();
			Normalized.ReplaceInline(TEXT("_"), TEXT(""));
			Normalized.ReplaceInline(TEXT("-"), TEXT(""));
			return Normalized == TEXT("terrainconstraintproof");
		}

		bool JsonValueHasContent(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid() || Value->IsNull())
			{
				return false;
			}

			switch (Value->Type)
			{
			case EJson::String:
				return !Value->AsString().TrimStartAndEnd().IsEmpty();
			case EJson::Boolean:
				return Value->AsBool();
			case EJson::Number:
				return true;
			case EJson::Array:
				return Value->AsArray().Num() > 0;
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				return Object.IsValid() && Object->Values.Num() > 0;
			}
			default:
				return false;
			}
		}

		bool HasNonEmptyStringField(const TSharedRef<FJsonObject>& Object, const TCHAR* FieldName)
		{
			FString Value;
			return Object->TryGetStringField(FieldName, Value) && !Value.TrimStartAndEnd().IsEmpty();
		}

		bool HasTerrainRecipeShape(const TSharedRef<FJsonObject>& Object)
		{
			const bool bHasRecipeId =
				HasNonEmptyStringField(Object, TEXT("recipe_id")) ||
				HasNonEmptyStringField(Object, TEXT("recipeId"));
			if (!bHasRecipeId)
			{
				return false;
			}

			static const TArray<FString> ConstraintShapeKeys = {
				TEXT("landform_profile"), TEXT("landformProfile"),
				TEXT("heightmap_recipe"), TEXT("heightmapRecipe"),
				TEXT("elevation_profile"), TEXT("elevationProfile"),
				TEXT("slope_p95_max_deg"), TEXT("slopeP95MaxDeg"),
				TEXT("slope_max_deg"), TEXT("slopeMaxDeg"),
				TEXT("playable_slope_max_deg"), TEXT("playableSlopeMaxDeg"),
				TEXT("max_elevation_m"), TEXT("maxElevationM"),
				TEXT("smoothing_passes"), TEXT("smoothingPasses"),
				TEXT("erosion_passes"), TEXT("erosionPasses")
			};

			int32 ShapeKeyCount = 0;
			for (const FString& ShapeKey : ConstraintShapeKeys)
			{
				if (Object->HasField(ShapeKey))
				{
					++ShapeKeyCount;
				}
			}
			return ShapeKeyCount >= 2;
		}

		bool JsonObjectHasTerrainConstraintProof(const TSharedRef<FJsonObject>& Object);

		bool JsonValueHasTerrainConstraintProof(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid() || Value->IsNull())
			{
				return false;
			}
			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				return Object.IsValid() && JsonObjectHasTerrainConstraintProof(Object.ToSharedRef());
			}
			if (Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					if (JsonValueHasTerrainConstraintProof(Item))
					{
						return true;
					}
				}
			}
			return false;
		}

		bool JsonObjectHasTerrainConstraintProof(const TSharedRef<FJsonObject>& Object)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (IsGenericTerrainConstraintProofContainerKey(Pair.Key))
				{
					if (JsonValueHasTerrainConstraintProof(Pair.Value))
					{
						return true;
					}
					continue;
				}
				if (IsTerrainConstraintProofKey(Pair.Key) && JsonValueHasContent(Pair.Value))
				{
					return true;
				}
			}

			if (HasTerrainRecipeShape(Object))
			{
				return true;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (JsonValueHasTerrainConstraintProof(Pair.Value))
				{
					return true;
				}
			}
			return false;
		}

		bool IsTerrainCreationDryRun(const TSharedRef<FJsonObject>& Arguments)
		{
			bool bDryRun = false;
			if ((Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun) ||
				 Arguments->TryGetBoolField(TEXT("dryRun"), bDryRun)) && bDryRun)
			{
				return true;
			}

			bool bExecute = true;
			if (Arguments->TryGetBoolField(TEXT("execute"), bExecute) && !bExecute)
			{
				return true;
			}

			FString Mode;
			return Arguments->TryGetStringField(TEXT("mode"), Mode) && Mode.Equals(TEXT("dry_run"), ESearchCase::IgnoreCase);
		}

		bool RequiresTerrainCreationConstraints(const FString& ToolName, const TSharedRef<FJsonObject>& Arguments)
		{
			if (IsTerrainCreationDryRun(Arguments))
			{
				return false;
			}

			static const TSet<FString> GuardedTerrainTools = {
				TEXT("landscape_create"),
				TEXT("landscape_create_from_heightmap"),
				TEXT("landscape_create_tile_with_data"),
				TEXT("landscape_batch_generate_world"),
				TEXT("landscape_generate_tile_heightmap"),
				TEXT("landscape_heightmap_from_noise"),
				TEXT("landscape_import_heightmap"),
				TEXT("landscape_import_tile"),
				TEXT("landscape_import_tiles_batch"),
				TEXT("landscape_patch_edit_layer_create"),
				TEXT("landscape_circle_patch_create"),
				TEXT("landscape_texture_patch_create"),
				TEXT("landscape_texture_patch_create_v2"),
				TEXT("terrain_landscape_create_from_spec"),
				TEXT("editor_terrain_create"),
				TEXT("world_create_terrain_scene"),
				TEXT("mesh_partition_create"),
				TEXT("mesh_partition_rectangle_create"),
				TEXT("mesh_partition_heightmap_import"),
				TEXT("mesh_terrain_apply_heightfield_to_mesh"),
				TEXT("mesh_terrain_sculpt_stroke_apply"),
				TEXT("mesh_terrain_sculpt_stroke_batch"),
				TEXT("mesh_terrain_height_sculpt_apply"),
				TEXT("mesh_terrain_height_smooth_apply"),
				TEXT("mesh_terrain_height_flatten_apply"),
				TEXT("mesh_terrain_slope_erode_apply")
			};
			return GuardedTerrainTools.Contains(ToolName);
		}

		bool ValidateTerrainCreationConstraints(
			const FString& ToolName,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutError)
		{
			if (!RequiresTerrainCreationConstraints(ToolName, Arguments))
			{
				return true;
			}

			// Terrain fix batch 0+1 (2026-07-21): proof schema v2 enforcement.
			//   v2 valid   -> pass (proof_sig self-verified, no process cache);
			//   v2 invalid -> reject (fail-closed);
			//   v1 legacy  -> pass with deprecation_warning while
			//                 SOMOLMCP_TERRAIN_PROOF_V1_COMPAT=1, else reject;
			//   no proof   -> blocked_missing_terrain_creation_constraints.
			FString ProofFailReason;
			const SomolmcpTerrainProof::EClass ProofClass = SomolmcpTerrainProof::Classify(Arguments, ProofFailReason);
			if (ProofClass == SomolmcpTerrainProof::EClass::V2Invalid)
			{
				OutError = FString::Printf(
					TEXT("blocked_invalid_terrain_proof_v2: %s received a schema v2 terrain constraint proof that failed validation: %s."),
					*ToolName, *ProofFailReason);
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("error_code"), TEXT("blocked_invalid_terrain_proof_v2"));
				OutStructured->SetStringField(TEXT("reason_code"), TEXT("blocked_invalid_terrain_proof_v2"));
				OutStructured->SetStringField(TEXT("tool"), ToolName);
				OutStructured->SetStringField(TEXT("proof_v2_failure"), ProofFailReason);
				OutStructured->SetStringField(TEXT("message"), OutError);
				return false;
			}

			if (JsonObjectHasTerrainConstraintProof(Arguments))
			{
				if (ProofClass == SomolmcpTerrainProof::EClass::V2Valid)
				{
					OutStructured->SetStringField(TEXT("proof_schema"), TEXT("2"));
					return true;
				}
#if SOMOLMCP_TERRAIN_PROOF_V1_COMPAT
				OutStructured->SetStringField(TEXT("deprecation_warning"), SomolmcpTerrainProof::DeprecationWarningText());
				OutStructured->SetStringField(TEXT("proof_schema"), TEXT("1"));
				return true;
#else
				OutError = FString::Printf(
					TEXT("blocked_terrain_proof_schema_v1_removed: %s requires a schema v2 terrain constraint proof (schema_version=\"2\" + recipe_id + pre_generation_constraints + proof_sig); v1 compatibility is disabled."),
					*ToolName);
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("error_code"), TEXT("blocked_terrain_proof_schema_v1_removed"));
				OutStructured->SetStringField(TEXT("reason_code"), TEXT("blocked_terrain_proof_schema_v1_removed"));
				OutStructured->SetStringField(TEXT("tool"), ToolName);
				OutStructured->SetStringField(TEXT("message"), OutError);
				return false;
#endif
			}

			OutError = FString::Printf(
				TEXT("blocked_missing_terrain_creation_constraints: %s requires Terrain Spec IR plus pre_generation_constraints, landform_constraints, or constrained_heightmap_recipe before execution."),
				*ToolName);
			OutStructured->SetBoolField(TEXT("ok"), false);
			OutStructured->SetStringField(TEXT("error_code"), TEXT("blocked_missing_terrain_creation_constraints"));
			OutStructured->SetStringField(TEXT("reason_code"), TEXT("blocked_missing_terrain_creation_constraints"));
			OutStructured->SetStringField(TEXT("tool"), ToolName);
			OutStructured->SetStringField(TEXT("message"), OutError);
			return false;
		}
	}

	FSololmcpToolRuntimeSnapshot GetToolRuntimeSnapshot()
	{
		FScopeLock Lock(&RuntimeSnapshotLock);
		return RuntimeSnapshot;
	}

	FSololmcpToolRegistry::FSololmcpToolRegistry()
	{
		RegisterCoreTools(*this);
		RegisterWorldTools(*this);
		RegisterLightingInspectionTools(*this); // Complete typed UE 5.8 light/Sky Light/scene/build/effective-view inspection and patch readback.
		RegisterAssetTools(*this);
		RegisterBlueprintMaterialAnimationTools(*this);
		RegisterSequencerAudioVfxTools(*this); // v1.6.0: Sequencer, Audio, VFX, Material, Niagara tools
		RegisterProjectPerceptionTools(*this); // v1.7.0: Project perception & management tools
		RegisterScreenshotTools(*this);
		RegisterBlueprintDebugTools(*this); // v1.8.0: Blueprint debug tools (breakpoints, watch, execution control) -UE 5.7: uses FKismetDebugUtilities
		RegisterEditorUITools(*this);       // v1.8.0: Editor UI automation (menu clicks, mode switching, terrain/PCG creation)
		RegisterSlateAuthoringTools(*this); // Native Slate authoring/layout/style/window/diagnostics tools
		RegisterUMGCompletionTools(*this); // Native UMG layout/navigation/accessibility/readback/delivery gates
		RegisterAssetToolkitTools(*this);  // v1.9.0: Asset Toolkit (thumbnails, analysis, compare, batch query, references, rename)
		RegisterTerrainStreamingTools(*this);  // v2.1.0: Terrain/Level Streaming Pipeline + Enhanced Editor Perception
		RegisterLodHlodTools(*this);          // v3.0: LOD/HLOD Management
		// Native video implementation must precede legacy cinematic aliases; the
		// registry keeps the first registration for deterministic authority.
		RegisterVideoAutomationTools(*this);  // v3.12.7: MRG/MRQ, desktop capture, preflight and presentation audit
		RegisterCinematicTools(*this);        // v3.0: Camera Animation & Cinematic
		RegisterPostProcessTools(*this);      // v3.0: Post-Processing Effects
		RegisterCharacterAnimationPipelineTools(*this); // v2.0.0: Character Animation Pipeline (identify, retarget, bind, spawn)
		RegisterEnhancedTools(*this);              // v3.1.0: Missing tools, aliases, PIE, console, Python, material, UMG, VFX, PCG, texture, sequencer
		RegisterLargeWorldTools(*this);            // v3.2.0: Large world (origin shift, landscape tiles, WP setup, project config)
		RegisterMaterialTemplateTools(*this);     // legacy material-template feature batch
		RegisterMaterialSemanticTools(*this);     // v3.14.x: material_graph_explain + material_property_trace read-only graph semantics
		RegisterMaterialGraphPatchTools(*this);   // v3.14.x: material_graph_diff + material_safe_patch dry-run contract
		RegisterGameplayContractTools(*this);     // v3.14.x: GAS / AI Perception / StateTree / SmartObject contract probes
		RegisterCharacterSceneTools(*this);       // legacy character scene tools (PBR, Toon, Outline, Dissolve, Hologram, Fresnel, Wind, Diagnose, Repair)
		RegisterCharacterActionRuntimeTools(*this); // v3.13.4: explicit-context character animation assignment/control/runtime inspection
		RegisterLevelPrototypingTools(*this);     // v3.5.0: Level Prototyping (daynight setup, blockout, surface scatter)
		RegisterNativeBspTools(*this);            // v3.13.4: native UE editor BSP/CSG authoring, inspection and conversion
		RegisterMediaIngestTools(*this);          // v3.6.0: Media & Ingest (FileMediaSource/StreamMediaSource/ImgMediaSource/MediaPlayer/MediaTexture/MediaPlaylist + ingest_video + video_probe)
		RegisterPcgEnhancementTools(*this);       // v3.7.0: PCG Enhancement (node_catalog, graph_validate, graph_explain, dry_run) -AI accuracy/effectiveness/efficiency pack for UE5 PCG.
		RegisterMarketplaceTools(*this);          // v3.7.1: fab_search + quixel_search stubs (Epic auth pending).
		RegisterV371DocumentedGapTools(*this);    // v3.7.1 compatibility tools; disk ingest now uses native AssetTools and returns object readback.
		RegisterMegaWorldTools(*this);            // v3.8: world_mpc_weather_override + landscape_hole_punch + swarm_virtual_detachment_mock
		// --- v3.9.0 P0 batch (4 new tool families = 24 tools) ---
		RegisterAnimBPStateMachineTools(*this);   // P0-1: animbp_create_state_machine + add_state + add_transition + set_transition_rule + add_blendspace_node + list_states (6 tools)
		RegisterCurveAssetTools(*this);           // P0-2: curve_create + add_key + remove_key + set_interpolation + inspect (5 tools)
		RegisterBlueprintFlowTools(*this);        // P0-3+P0-4: blueprint_add_for_loop / for_each_loop / while_loop / switch_int / switch_enum / select / event_node (7 tools)
		RegisterBlueprintComponentTools(*this);   // P0-5+P0-8: blueprint_add/remove/set_property/list_components + blueprint_batch_edit + behaviortree_batch_edit (6 tools)
		// --- v3.10.0 P1+P2 batch -all 8 enabled after UE 5.7 API fixes ---
		RegisterMaterialLayerTools(*this);        // P1-3: 5 tools (3 working + 2 NOT_IMPLEMENTED stubs pending UE 5.7 layer stack API)
		RegisterNiagaraScriptGraphTools(*this);   // P1-4: niagara_script_* (5 tools)
		RegisterStaticMeshEditTools(*this);       // P1-5: staticmesh_set/generate/inspect (6 tools)
		RegisterTextureProcessTools(*this);       // P1-6: texture processing + guarded batch preview/configure (7 tools)
		RegisterMetaSoundTools(*this);            // P1-7: metasound_create/inspect/set_input/compile (4 tools)
		RegisterSkelMeshPhysicsTools(*this);      // P2-1: skelmesh_* + physics_asset_* (5 tools)
		RegisterLandscapeAdvancedTools(*this);    // P2-2: 5 tools (4 working + 1 NOT_IMPLEMENTED stub for paint_layer)
		RegisterWorldPartitionTools(*this);       // P2-3: 5 tools (3 working + 2 with empty array fallback for cells/hlod)
		RegisterAssetRefDataTools(*this);         // P2-4+P2-5: asset_find/replace/fix_redirectors + dataasset_create/set/get (6 tools)
		RegisterSubstrateMaterialTools(*this);    // P1-1: material_substrate_status/inspect/create_simple/set_slab_property (4 tools, NOT_AVAILABLE if r.Substrate=0)
		// --- v3.11.0 P3+P4 batch (7 new tool families >=38 tools) ---
		RegisterWorldPartitionHLODTools(*this);   // P3-1: worldpartition_hlod_* (5 tools, force_rebuild dispatches async editor command)
		RegisterHlodForceRebuildShim(*this);      // Override force_rebuild with richer failure diagnostics.
		RegisterBehaviorTreeTemplateTools(*this); // P3-2: bt_template_apply_patrol/combat/list_available (3 tools)
		RegisterEditorModeTools(*this);           // P3-3: editor_mode_enter/exit/get_active/set_tool (4 tools)
		RegisterProjectSettingsTools(*this);      // P3-4: project_settings_get/set + plugin_enable/disable (4 tools)
		RegisterSequencerAdvancedTools(*this);    // P3-5: native master tracks, camera cuts, fade, event endpoint/key authoring.
		RegisterTransactionTools(*this);          // P3-7: transaction_begin/end/abort/list (4 tools)
		RegisterInterchangeTools(*this);          // Interchange batch 1: import/reimport/export orchestration, format and translator queries (15 tools, 5.3+)
		RegisterInterchangeNodeTools(*this);      // Interchange batch 2: node-graph reflection, attribute get/set, batch writes, key catalog (11 tools, 5.3+)
		RegisterInterchangePipelineTools(*this);  // Interchange batch 3: pipeline stacks, property inspect/catalog/set, batch reconfigure (5 tools, 5.3+)
		RegisterInterchangeBatchTools(*this);     // Interchange batch 4: directory scan, bulk import, import-data audit, source relink, receipt validate (5 tools, 5.3+)
		RegisterRigHierarchyTools(*this);         // ControlRig: hierarchy element reflection + transform get/set + batch pose (5 tools)
		RegisterRigVMGraphTools(*this);           // ControlRig: RigVM node/pin reflection, unit-node catalog, batch pin/link editing, graph snapshot (5 tools)
		RegisterRigSequencerTools(*this);         // ControlRig x Sequencer: binding discovery, visible rigs, batch transform sampling, tween (4 tools)
		RegisterObjectHandleTools(*this);         // Session handles for live editor objects: list, inspect, release (3 tools)
		RegisterToolCatalogTools(*this);          // Bounded discovery: groups -> names -> schemas (1 tool)
		RegisterGeometryScriptTools(*this);       // GeometryScripting: dynamic mesh session open/query/save-to-asset (3 tools)
		RegisterGeometryOpTools(*this);           // GeometryScripting: reflection dispatch + catalog over ~509 library ops (3 tools)
		RegisterEditorApiTools(*this);            // Reflection dispatch over 7 domains / 2585 callables: DynamicMaterial, ClonerEffector, USD, Datasmith, Sequencer, MovieRenderQueue, TakeRecorder (4 tools)
		RegisterBrushKernelTools(*this);          // v3.12.8 P0: shared brush profiles, stroke lifecycle, projection, snapshots, receipt gate
		RegisterFractureAuthoringTools(*this);     // v3.12.8 P1: native Chaos fracture, readback, rebuild, receipt gate
		RegisterFractureHierarchyTools(*this);     // v3.12.8 P1: native hierarchy selection, clustering, split and validation
		RegisterFractureEditTools(*this);          // v3.12.8 P1: native hierarchy edit, material, visibility, normals and geometry repair
		RegisterFracturePatternTools(*this);       // v3.12.8 P1: native Voronoi, plane, slice, brick and mesh cutter authoring
		RegisterChaosDataflowAuthoringTools(*this);// v3.12.8 P1: native Chaos Cache and Dataflow authoring, diagnostics and receipts
		RegisterMeshPaintAuthoringTools(*this);    // v3.12.8 P1: native instance vertex colors with save/readback receipts
		RegisterMeshPaintExtendedTools(*this);     // v3.12.8 P1: RGBA channels, weights, LOD transfer, UV and texture targets
		RegisterMeshPaintTextureTools(*this);      // v3.12.8 P1: native texture-paint configuration, sessions, backups and persistence
		RegisterModelingCompletionTools(*this);    // v3.12.8 P2: 40 native DynamicMesh inspect/edit/readback/receipt tools
		RegisterFoliageCompletionTools(*this);     // v3.12.8 P2: 26 native foliage authoring and Foliage/ISM/HISM conversion tools
		RegisterAnimationCompletionTools(*this);   // v3.12.8 P3: 58 native animation, AnimBP, AnimNext, PoseSearch and Control Rig tools
		RegisterMeshTerrainCompletionTools(*this); // v3.12.8 P4: 16 UE 5.8 Mesh Terrain closure and recovery tools
		RegisterAuthoringQaPcgCompletionTools(*this); // v3.12.8 P4: 14 authoring gates + 8 native PCG replacements
		RegisterLegacyNativeCompletionTools(*this); // 2026-08-05: legacy foliage_*/geometry_script_* names promoted to real native executors
		RegisterDevOpsTools(*this);               // P3-8+P4-1+P4-4: sourcecontrol_* + cook_/package_ + net_* (10 tools)
		// --- v3.12.0 final P3+P4 batch (4 new tool families >=21 tools) ---
		RegisterHotReloadTools(*this);            // P3-6: mcp_register_python_tool/unregister/list/reload/get_source (5 tools)
		RegisterClothTools(*this);                // P4-2: cloth_inspect/create/set_simulation/set_wind/status (5 tools, 3 NOT_AVAILABLE without modules)
		RegisterXRTemplateTools(*this);           // P4-3: xr_setup_pawn/add_motion_controllers/add_floor/create_teleport/status (5 tools)
		RegisterEmbodiedLoopTools(*this);         // P4-6: embody_pie_start/stop/state/press_key/axis/get_observation (6 tools)
		// --- v3.13.0 P5 batch (3 new tool families = 13 tools) ---
		RegisterCookPipelineTools(*this);         // P5: cook_pipeline_clean/validate/size_report/chunk_assignment/dependency_graph (5 tools)
		RegisterPakUpdateTools(*this);             // Per-asset Pak/IoStore release + patch closure (9 tools)
		RegisterPluginDiscoveryTools(*this);      // P5: plugin_list_all/inspect/check_compatibility/recommend_for_role (4 tools)
		RegisterNiagaraHLSLTools(*this);          // P5: reflection bridge avoids UE 5.7 MinimalAPI Custom HLSL linker traps.
		// --- v3.14.0 -anti-freeze lite tools (2 tools) ---
		RegisterLiteScanTools(*this);             // world_partition_status_lite + landscape_actor_list_lite -fast non-freezing alternatives to python_exec full-scene scans
		// --- v3.14.x -Tier 1+3 generic dispatcher for client-side Python sidecar ---
		RegisterUnrealCallTool(*this);            // unreal_call -single-statement Python dispatch, used by v3.9.x sidecar
		// --- v3.10 Phase D -Batch APIs (5 tools) ---
		RegisterBatchTools(*this);                // actor_*_batch / asset_query_batch_paths / actor_spawn_batch_lite -one game-thread enter for N items
		RegisterEditorDialogTools(*this);         // H2: editor_dialog_policy_set/list/respond safe modal-dialog control contracts
		RegisterEditorBuildPipelineTools(*this);  // v3.14.x: lighting/nav/AI/shader/reflection/package settings orchestration
		RegisterMcpExecutionPlanningTools(*this); // v3.15.x: multi-agent execution profiles, resource locks, and parallel authoring waves
		RegisterVersionedCapabilityTools(*this);  // v3.16.x: UE 5.7 baseline probes + UE 5.8-only official MCP/toolset gates
		RegisterValidationRenderTools(*this);     // v3.17.x: Data Validation, MRQ, Take Recorder production probe/plan tools
		RegisterVirtualProductionIngestTools(*this); // v3.17.x: Remote Control, variants, LiveLink, camera calibration, USD/Datasmith
		RegisterWorldAiDataUiTools(*this);        // v3.17.x: Chooser, PoseSearch, ZoneGraph, Mass, CommonUI, DataRegistry, Enhanced Input plans
		RegisterProductionBridgeTools(*this);     // v3.17.x: automation, Motion Design, PCG/Niagara, and Editor Utility bridge plans
		RegisterUE58ProductionTools(*this);       // v3.17.x: UE 5.8 production route plans with 5.7 fail-closed guards
		RegisterUE58ToolsetTools(*this);          // v3.18.x: UE 5.8 ToolsetRegistry inventory/schema/wrapper status, excludes MCPClientToolset
		RegisterUE58CallableDiffTools(*this);     // v3.19.x: UE 5.8 callable inventory/diff/gates/rank, 5.7-safe scan/reflection
		RegisterLandscapePatchPcgInteropTools(*this); // v3.18.x: native LandscapePatch + PCG interop / UE 5.8 MeshPartition coverage
		RegisterBlueprintCallableBridgeTools(*this); // v3.19.x: UE 5.7-safe BlueprintCallable reflection bridge, read-only/plan first
		RegisterNiagaraToolsetP1Tools(*this);     // v3.24.x: NiagaraToolsets concrete P1 schema/topology/plan/receipt tools
		RegisterMeshPaintUpgradeTools(*this);     // Mesh Paint MP-01: native vertex-color baker promoted ahead of the P0 catalog wrapper
		RegisterMeshTerrainNativeTools(*this);    // v3.31.x: native UE 5.8 writers registered first to promote legacy plan names
		RegisterPcg58NativeTools(*this);          // v3.32.x: UE 5.8 public PCG manual markers, embedded subgraphs, graph usage, and receipts
		RegisterUE58RenderingTools(*this);         // v3.33.x: UE 5.8 MegaLights and Lumen Lite project/runtime validation
		RegisterUE58CharacterAnimationTools(*this); // v3.34.x: UE 5.8 modular Control Rig authoring and compile validation
		RegisterUE58SequencerTools(*this);          // v3.35.x: UE 5.8 active Sequencer view/filter/linkage read-write tools
		RegisterUE58ControlRigPhysicsTools(*this);  // v3.36.x: UE 5.8 Control Rig Physics force-node authoring and compile validation
		RegisterUE58ControlRigDynamicsTools(*this); // v3.37.x: UE 5.8 Control Rig particle dynamics authoring and compile validation
		RegisterUE58DirectMeshControlTools(*this);  // v3.38.x: UE 5.8 Direct Mesh Control construction, binding, animation, and validation
		RegisterUE58AnimationBridgeTools(*this);    // v3.39.x: UE 5.8 Sequencer autobake, Animation Mixer, Retarget/RigMapper, Mutable, and LiveLink Face bridge tools
		RegisterUE58WorldbuildingBridgeTools(*this); // v3.40.x: UE 5.8 Fast Geometry Streaming, World Partition Insights, and HLOD UX bridge tools
		RegisterMeshTerrainModeP1Tools(*this);    // v3.24.x: MeshTerrainMode/MeshPartition concrete P1 probes/plans/receipts
		RegisterCharacterCustomizationP1Tools(*this); // v3.24.x: MetaHuman/Mutable concrete P1 tools without optional-plugin link deps
		RegisterClothOutfitDataflowP1Tools(*this); // v3.24.x: Cloth/Outfit/Dataflow concrete P1 tools with 5.8 runtime gate
		RegisterP0CompletionTools(*this);         // v3.20.x: complete P0 planned wrapper names without overriding concrete implementations
		RegisterP1CompletionTools(*this);         // v3.21.x: complete P1 production wrapper names without overriding concrete implementations
		RegisterP2CompletionTools(*this);         // v3.22.x: complete P2 broad editor orchestration wrapper names without overriding concrete implementations
		RegisterP3CompletionTools(*this);         // v3.23.x: complete P3 long-tail and experimental wrapper names without overriding concrete implementations
		RegisterUE58ProductionBridgeTools(*this); // v3.41.x: UE 5.8 Audio, Interchange/USD/FBX, and Movie Render Graph bridge tools
		RegisterUE58FrameworkBridgeTools(*this); // v3.42.x: UE 5.8 StateTree, Mass, Navigation, Iris, Mover, and unified input bridge tools
		RegisterUE58DeveloperIterationBridgeTools(*this); // v3.43.x: UE 5.8 Incremental Cook, Zen cooked output store, and Horde performance bridge tools
		RegisterUE58PlatformBridgeTools(*this); // v3.44.x: UE 5.8 desktop, mobile, remote, and XR platform bridge tools
		RegisterUE58EditorMotionBridgeTools(*this); // v3.45.x: UE 5.8 editor UX and Motion Design bridge tools
		RegisterUE58VirtualProductionMediaBridgeTools(*this); // v3.46.x: UE 5.8 virtual production and media bridge tools
		RegisterUE58ChaosNiagaraMcpBridgeTools(*this); // v3.47.x: UE 5.8 Chaos, Niagara, and official-MCP interoperability bridge tools
		RegisterArchitectureTools(*this);         // v3.26.x: modular architecture/settlement/collision/enterability gates
		RegisterCrossProjectAssetTools(*this);    // Native cross-project dependency-complete asset migration and receipt gates
		RegisterWorldCreateTools(*this);          // v3.25.x: World Create orchestration tools (impl SololmcpWorldCreateTools.cpp + decl SololmcpToolRegistry.h pulled in 8ac4842; re-enabled now that the definition exists)
#if SOMOLMCP_WITH_WORLDFORGE
		RegisterWorldForgeMeshPartitionBridgeTools(*this); // v3.27.x: exact WorldForge UE 5.8 MeshPartition bridge names and fail-closed writer gates
		RegisterWorldForgePVEBridgeTools(*this); // v3.28.x: WorldForge UE 5.8 Procedural Vegetation Editor bridge probes/plans/receipt gates
		RegisterWorldForgeRuntimeMapTools(*this); // v3.29.x: RuntimeMap/minimap/tile/fog/player-map orchestration
		RegisterWorldForgeSurfaceMicrostructureTools(*this); // Publisher-driven sharp near-surface material profile and budget readback
		RegisterWorldForgeNavigationPrecisionTools(*this); // Integer global address <-> centimeter local navigation frame
		RegisterWorldForgeAtmosphereTools(*this); // SOMOLAtmosphere M1 batch: director/state/cloud override/presets/noise bake
		RegisterWorldForgeSupercellTools(*this); // RuntimeWeather circulation + wind-shaped supercell volume/readback/exposure
		RegisterWorldForgeCelestialSceneTools(*this); // Generic celestial scene authority, observer transforms, and closed live contracts
		RegisterWorldForgePlanetaryRingTools(*this); // Planetary Ring v3: single-profile authority, 88 typed routes
		RegisterWorldForgeSkyWeatherTools(*this); // SKY-WEATHER v2.1: M7-M11, WX2/WX3, eclipse and server bridge
#endif
		RegisterSettlementTools(*this);           // v3.30.x: somol-settlement IR consumer tools
		RegisterWorldspaceDiagnosticsTools(*this); // v3.30.x: worldspace subtraction toggles + quantified asserts
		RegisterWorldForgePlatformContractTools(*this); // Generic WorldForge schemas/providers/commands; read-only and project-neutral
		RegisterWorldForgeP0ControlTools(*this); // SSOT-generated 14x8 P0 authority-backed control surface
		RegisterWorldForgeEarthSystemControlTools(*this); // SSOT-generated 9x8 Earth-system live control surface
		RegisterWorldForgeEarthSystemBlueprintLiveVerifyTool(*this); // Live reflection + ProcessEvent proof for all 72 Blueprint nodes
		RegisterWorldForgeGranularWaterFXContractTools(*this); // Granular spill + WaterFX 35-name live bridge
		RegisterWorldForgeSwarmTools(*this); // Swarm: 91 generated, closed-envelope runtime routes
		// Rev.C Blueprint-primary implementations are authoritative. Register
		// them before the frozen enrichment compatibility catalog; that catalog
		// explicitly skips canonical names instead of relying on first-wins.
		RegisterWorldForgeRevCGapTools(*this); // Rev.C: 90 native Blueprint-parity routes
		RegisterWorldForgeEnrichmentCapabilityTools(*this); // WorldForge 3.0 enrichment compatibility surface
		RegisterWorldForgeLayeredCouplingTools(*this); // Shared Blueprint/MCP layered-coupling provider runtime
		RegisterWorldForgeGeoTerrainTools(*this); // WP00 WF-00-05: 23 frozen GeoTerrain v1.1 tools, schemas generated from the machine registries
		RegisterWorldForgeTerrainRepresentationTools(*this); // Terrain representation fusion v2: 16 production tools
		RegisterWorldForgeEditorRuntimeParityTools(*this); // erp14: ERP dual-mode audit tool family (10 read-only tools)
		RegisterWorldForgeRevCMappingAuditTools(*this); // Rev.C P0-MAPPING closed-schema/mapping gate

		// Restore legacy public names only when a real native C++ implementation
		// already exists. The alias inherits the native target's schema, executor,
		// availability check, cache policy, and output contract; no script bridge is
		// involved. Keep this block last so every native target is registered first.
		const TPair<const TCHAR*, const TCHAR*> NativeCompatibilityAliases[] = {
			{TEXT("anim_bp_add_state"), TEXT("anim_blueprint_add_state")},
			{TEXT("anim_bp_add_transition"), TEXT("anim_blueprint_add_transition")},
			{TEXT("anim_bp_create_state_machine"), TEXT("anim_blueprint_add_state_machine")},
			{TEXT("anim_bp_list_states"), TEXT("anim_blueprint_list_states")},
			{TEXT("anim_bp_remove_state"), TEXT("anim_blueprint_remove_state")},
			{TEXT("anim_montage_list_sections"), TEXT("animation_montage_inspect")},
			{TEXT("audio_metasound_build_asset"), TEXT("metasound_compile")},
			{TEXT("audio_metasound_set_input_default"), TEXT("metasound_set_input_default")},
			{TEXT("build_lighting"), TEXT("editor_build_lighting")},
			{TEXT("build_navigation"), TEXT("editor_build_navigation")},
			{TEXT("editor_ui_spawn_actor"), TEXT("actor_spawn")},
			{TEXT("skeletal_mesh_reimport"), TEXT("reimport_asset")},
			{TEXT("static_mesh_reimport"), TEXT("reimport_asset")},
			{TEXT("update_reflection_captures"), TEXT("editor_build_reflection_captures")},
		};
		for (const TPair<const TCHAR*, const TCHAR*>& Alias : NativeCompatibilityAliases)
		{
			RegisterNativeCompatibilityAlias(Alias.Key, Alias.Value);
		}
	}

	// FIXED #11: BuildToolsList 鏀逛负闈?const锛岀洿鎺ヤ娇鐢?Services锛堜笉鍐?const_cast锛?
		TArray<TSharedPtr<FJsonValue>> FSololmcpToolRegistry::BuildToolsList()
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		FSololmcpToolExecutionContext Context{Services};
		for (const TPair<FString, FSololmcpToolDefinition>& Pair : ToolsByName)
		{
			const FSololmcpToolDefinition& Tool = Pair.Value;
			if (IsExternalPythonSurfaceToolName(Tool.Name) || Tool.bUsesExternalPython)
			{
				continue;
			}
			FString Reason;
			if (Tool.IsAvailable && !Tool.IsAvailable(Context, Reason))
			{
				continue;
			}

			TSharedRef<FJsonObject> ToolJson = MakeShared<FJsonObject>();
			ToolJson->SetStringField(TEXT("name"), Tool.Name);
			ToolJson->SetStringField(TEXT("description"), Tool.Description);
			ToolJson->SetObjectField(TEXT("inputSchema"), Tool.InputSchema);
			// outputSchema: include if defined (MCP 2025-03 extension)
			if (Tool.OutputSchema.IsValid() && Tool.OutputSchema->Values.Num() > 0)
			{
				ToolJson->SetObjectField(TEXT("outputSchema"), Tool.OutputSchema.ToSharedRef());
			}
			// Cache metadata for clients
			if (Tool.CacheTtlSeconds > 0)
			{
				TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
				Annotations->SetNumberField(TEXT("cacheTtlSeconds"), Tool.CacheTtlSeconds);
				ToolJson->SetObjectField(TEXT("annotations"), Annotations);
			}
			Tools.Add(MakeShared<FJsonValueObject>(ToolJson));
		}

		// FIXED #10: Sort 姣旇緝鍣ㄤ腑闃插尽 nullptr锛岄伩鍏?AsObject() 杩斿洖绌烘寚閽堟椂宕╂簝
		Tools.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject> ObjA = A.IsValid() ? A->AsObject() : nullptr;
			const TSharedPtr<FJsonObject> ObjB = B.IsValid() ? B->AsObject() : nullptr;
			if (!ObjA.IsValid()) { return false; }
			if (!ObjB.IsValid()) { return true; }
			FString NameA, NameB;
			ObjA->TryGetStringField(TEXT("name"), NameA);
			ObjB->TryGetStringField(TEXT("name"), NameB);
			return NameA < NameB;
		});

		return Tools;
	}

	FString FSololmcpToolRegistry::ComputeCacheKey(const FString& ToolName, const TSharedRef<FJsonObject>& Arguments) const
	{
		// Serialize arguments to string for deterministic cache key
		FString ArgsStr = ToJsonString(Arguments);
		return ToolName + TEXT("|") + ArgsStr;
	}

	bool FSololmcpToolRegistry::TryGetCachedResponse(const FString& CacheKey, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) const
	{
		const FCachedResponse* Cached = ResponseCache.Find(CacheKey);
		if (!Cached)
		{
			return false;
		}

		const double Now = FPlatformTime::Seconds();
		if (Now > Cached->ExpiryTimeSec)
		{
			// Expired -cannot remove from const method, just return false (lazy cleanup on next SetCachedResponse)
			return false;
		}

		// Cached responses are immutable after insertion. Reuse the shared snapshot here:
		// FJsonObject::Duplicate asserts when a valid object contains an invalid/null
		// FJsonValue child (possible in queued job receipts assembled by optional fields).
		if (Cached->Structured.IsValid())
		{
			OutStructured = Cached->Structured.ToSharedRef();
			OutSummary = Cached->Summary;
			OutError = Cached->Error;
			return true;
		}

		return false;
	}

	void FSololmcpToolRegistry::SetCachedResponse(const FString& CacheKey, int32 TtlSeconds, const TSharedRef<FJsonObject>& Structured, const FString& Summary, const FString& Error)
	{
		if (TtlSeconds <= 0 || CacheKey.IsEmpty())
		{
			return;
		}

		FCachedResponse Entry;
		// Deep copy structured response
		Entry.Structured = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Structured, Entry.Structured);
		if (Entry.Structured.IsValid())
		{
			Entry.Summary = Summary;
			Entry.Error = Error;
			Entry.ExpiryTimeSec = FPlatformTime::Seconds() + static_cast<double>(TtlSeconds);

			// Evict old entries if cache is too large (max 256)
			if (ResponseCache.Num() > 256)
			{
				// Remove expired entries first
				TArray<FString> ExpiredKeys;
				const double Now = FPlatformTime::Seconds();
				for (const TPair<FString, FCachedResponse>& Pair : ResponseCache)
				{
					if (Now > Pair.Value.ExpiryTimeSec)
					{
						ExpiredKeys.Add(Pair.Key);
					}
				}
				for (const FString& Key : ExpiredKeys)
				{
					ResponseCache.Remove(Key);
				}

				// Still too large? Remove oldest 64
				if (ResponseCache.Num() > 256)
				{
					int32 ToRemove = ResponseCache.Num() - 192;
					for (auto It = ResponseCache.CreateIterator(); It && ToRemove > 0; ++It, --ToRemove)
					{
						It.RemoveCurrent();
					}
				}
			}

			ResponseCache.Add(CacheKey, Entry);
		}
	}

	void FSololmcpToolRegistry::ClearResponseCache()
	{
		ResponseCache.Empty();
	}

	bool FSololmcpToolRegistry::ExecuteTool(const FString& ToolName, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
	{
		if (IsExternalPythonSurfaceToolName(ToolName))
		{
			OutStructured->SetStringField(TEXT("reason_code"), TEXT("external_python_surface_hidden"));
			OutStructured->SetStringField(TEXT("execution_mode"), TEXT("named_domain_tools_only"));
			OutStructured->SetStringField(TEXT("guidance"), TEXT("Use a registered SOMOLMCP domain tool. Arbitrary client-supplied Python is not exposed."));
			OutError = TEXT("Generic Python execution is not exposed by SOMOLMCP. Use a named domain tool through the job queue.");
			return false;
		}

		const FSololmcpToolDefinition* Tool = ToolsByName.Find(ToolName);
		if (!Tool)
		{
			OutError = FString::Printf(TEXT("Unknown tool: %s"), *ToolName);
			return false;
		}
		if (Tool->bUsesExternalPython)
		{
			OutStructured->SetStringField(TEXT("reason_code"), TEXT("legacy_python_backend_hidden"));
			OutStructured->SetStringField(TEXT("execution_mode"), TEXT("queue_only_native_cpp"));
			OutStructured->SetStringField(TEXT("guidance"), TEXT("Use the native C++ domain replacement through jobs/submit."));
			OutError = TEXT("This legacy tool is hidden because its implementation uses Python. Use a native C++ queue tool.");
			return false;
		}

		FSololmcpToolExecutionContext Context{Services};
		FString Reason;
		if (Tool->IsAvailable && !Tool->IsAvailable(Context, Reason))
		{
			OutError = Reason.IsEmpty() ? TEXT("Tool is not available in the current editor configuration.") : Reason;
			return false;
		}

		if (!Tool->Execute)
		{
			OutError = TEXT("Tool has no implementation.");
			return false;
		}

		if (!ValidateRequiredArguments(*Tool, Arguments, OutError))
		{
			return false;
		}

		if (!ValidateTerrainCreationConstraints(ToolName, Arguments, OutStructured, OutError))
		{
			return false;
		}

		if (!ValidateAssetNamingPolicy(ToolName, Arguments, OutStructured, OutError))
		{
			return false;
		}

		const bool bMutationTool = IsLikelyMutationTool(ToolName);
		const bool bVolatileQueueRead =
			ToolName.Equals(TEXT("job_get"), ESearchCase::IgnoreCase) ||
			ToolName.Equals(TEXT("job_events"), ESearchCase::IgnoreCase) ||
			ToolName.Equals(TEXT("jobs/get"), ESearchCase::IgnoreCase) ||
			ToolName.Equals(TEXT("jobs/events"), ESearchCase::IgnoreCase);

		// Check TTL cache before executing. Mutation-shaped tools must never read
		// cached responses even if an individual registration accidentally set TTL.
		if (!bMutationTool && !bVolatileQueueRead && Tool->CacheTtlSeconds > 0)
		{
			const FString CacheKey = ComputeCacheKey(ToolName, Arguments);
			if (TryGetCachedResponse(CacheKey, OutStructured, OutSummary, OutError))
			{
				// Cache hit
				OutSummary += TEXT(" (cached)");
				return true;
			}

			// Execute and cache
		const double StartTimeSec = FPlatformTime::Seconds();
		MarkToolStart(ToolName, Arguments);
		UE_LOG(LogSOMOLMCP, Log, TEXT("Tool start: %s"), *ToolName);
		const bool bSuccess =
			Tool->Execute(Context, Arguments, OutStructured, OutSummary, OutError) &&
			NormalizeToolSuccess(OutStructured, OutSummary, OutError);
		const double ElapsedMs = (FPlatformTime::Seconds() - StartTimeSec) * 1000.0;
		MarkToolEnd(ToolName, bSuccess, ElapsedMs);
		UE_LOG(LogSOMOLMCP, Log, TEXT("Tool end: %s success=%d elapsed_ms=%.1f"),
			*ToolName, bSuccess ? 1 : 0, ElapsedMs);
		if (bSuccess)
		{
			SetCachedResponse(CacheKey, Tool->CacheTtlSeconds, OutStructured, OutSummary, OutError);
			}
			return bSuccess;
		}

		const double StartTimeSec = FPlatformTime::Seconds();
		MarkToolStart(ToolName, Arguments);
		UE_LOG(LogSOMOLMCP, Log, TEXT("Tool start: %s"), *ToolName);
		const bool bSuccess =
			Tool->Execute(Context, Arguments, OutStructured, OutSummary, OutError) &&
			NormalizeToolSuccess(OutStructured, OutSummary, OutError);
		const double ElapsedMs = (FPlatformTime::Seconds() - StartTimeSec) * 1000.0;
		MarkToolEnd(ToolName, bSuccess, ElapsedMs);
		UE_LOG(LogSOMOLMCP, Log, TEXT("Tool end: %s success=%d elapsed_ms=%.1f"),
			*ToolName, bSuccess ? 1 : 0, ElapsedMs);
		if (bSuccess && bMutationTool)
		{
			ClearResponseCache();
		}
		return bSuccess;
	}

	bool FSololmcpToolRegistry::HasRegisteredTool(const FString& ToolName) const
	{
		const FSololmcpToolDefinition* Tool = ToolsByName.Find(ToolName);
		return !IsExternalPythonSurfaceToolName(ToolName) && Tool && !Tool->bUsesExternalPython;
	}

	void FSololmcpToolRegistry::Register(const FSololmcpToolDefinition& Tool)
	{
		// Duplicate tool names must be deterministic. Earlier builds used
		// TMap::Add here, which silently replaced the first implementation with
		// whichever registration happened to run last. That makes tools/list
		// authority unstable across merges and module-order changes. Keep the
		// first registration. Keep the duplicate note at Verbose level: the
		// source still needs cleanup eventually, but startup should not surface
		// noisy warnings for known legacy wrapper aliases.
		if (FSololmcpToolDefinition* Existing = ToolsByName.Find(Tool.Name))
		{
			if (Existing->bUsesExternalPython && !Tool.bUsesExternalPython)
			{
				UE_LOG(LogSOMOLMCP, Log,
					TEXT("[SOMOLMCP] replacing legacy Python-backed tool with native C++ implementation: %s"),
					*Tool.Name);
				*Existing = Tool;
				return;
			}
			UE_LOG(LogSOMOLMCP, Verbose,
				TEXT("[SOMOLMCP] tool name collision: \"%s\" registered twice; keeping first registration and ignoring duplicate"),
				*Tool.Name);
			return;
		}
		ToolsByName.Add(Tool.Name, Tool);
	}

	bool FSololmcpToolRegistry::RegisterNativeCompatibilityAlias(const FString& AliasName, const FString& NativeTargetName)
	{
		const FSololmcpToolDefinition* NativeTarget = ToolsByName.Find(NativeTargetName);
		if (!NativeTarget || NativeTarget->bUsesExternalPython || IsExternalPythonSurfaceToolName(NativeTargetName))
		{
			UE_LOG(LogSOMOLMCP, Warning,
				TEXT("[SOMOLMCP] native compatibility alias skipped: %s -> %s (native target unavailable)"),
				*AliasName,
				*NativeTargetName);
			return false;
		}

		FSololmcpToolDefinition Alias = *NativeTarget;
		Alias.Name = AliasName;
		Alias.Description = FString::Printf(
			TEXT("Native C++ compatibility name for %s. %s"),
			*NativeTargetName,
			*NativeTarget->Description);
		Alias.bUsesExternalPython = false;

		if (FSololmcpToolDefinition* Existing = ToolsByName.Find(AliasName))
		{
			if (!Existing->bUsesExternalPython)
			{
				return Existing->Name == NativeTargetName || Existing->Name == AliasName;
			}
			*Existing = Alias;
		}
		else
		{
			ToolsByName.Add(AliasName, Alias);
		}

		UE_LOG(LogSOMOLMCP, Log,
			TEXT("[SOMOLMCP] restored legacy name with native C++ implementation: %s -> %s"),
			*AliasName,
			*NativeTargetName);
		return true;
	}

	void FSololmcpToolRegistry::GetRegisteredToolNamesSorted(TArray<FString>& OutNames) const
	{
		OutNames.Reset(ToolsByName.Num());
		for (const TPair<FString, FSololmcpToolDefinition>& Pair : ToolsByName)
		{
			if (!IsExternalPythonSurfaceToolName(Pair.Key) && !Pair.Value.bUsesExternalPython)
			{
				OutNames.Add(Pair.Key);
			}
		}
		OutNames.Sort();
	}
}
