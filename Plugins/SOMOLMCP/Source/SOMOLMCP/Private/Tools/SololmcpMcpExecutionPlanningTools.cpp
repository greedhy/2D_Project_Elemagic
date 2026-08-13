// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include <initializer_list>

namespace UE::SOMOLMCP
{
namespace
{
	struct FMcpResourceLock
	{
		FString Id;
		FString Mode;
		FString Reason;
	};

	struct FMcpExecutionProfile
	{
		FString ToolName;
		FString Domain;
		FString OperationClass;
		FString Lane;
		FString ThreadModel;
		FString MinEngineVersion = TEXT("5.7");
		bool bRequiresReceipt = false;
		bool bRequiresTargetBinding = false;
		bool bUe58PreviewOnly = false;
		TArray<FString> TargetPaths;
		TArray<FMcpResourceLock> Locks;
		TArray<FString> Notes;
	};

	struct FMcpPlannedCall
	{
		FString CallId;
		FString RoleId;
		FString TaskId;
		FString ToolName;
		FMcpExecutionProfile Profile;
	};

	static bool StartsWithAny(const FString& Lower, const TArray<FString>& Prefixes)
	{
		for (const FString& Prefix : Prefixes)
		{
			if (Lower.StartsWith(Prefix))
			{
				return true;
			}
		}
		return false;
	}

	static bool StartsWithAny(const FString& Lower, std::initializer_list<const TCHAR*> Prefixes)
	{
		for (const TCHAR* Prefix : Prefixes)
		{
			if (Lower.StartsWith(FString(Prefix)))
			{
				return true;
			}
		}
		return false;
	}

	static bool ContainsAny(const FString& Lower, const TArray<FString>& Tokens)
	{
		for (const FString& Token : Tokens)
		{
			if (Lower.Contains(Token))
			{
				return true;
			}
		}
		return false;
	}

	static bool ContainsAny(const FString& Lower, std::initializer_list<const TCHAR*> Tokens)
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Lower.Contains(FString(Token)))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsMutationToolName(const FString& Lower)
	{
		static const TArray<FString> MutationPrefixes = {
			TEXT("add_"), TEXT("apply_"), TEXT("assign_"), TEXT("attach_"), TEXT("bind_"),
			TEXT("build_"), TEXT("compile_"), TEXT("connect_"), TEXT("create_"), TEXT("delete_"),
			TEXT("destroy_"), TEXT("disable_"), TEXT("disconnect_"), TEXT("duplicate_"), TEXT("enable_"),
			TEXT("fill_"), TEXT("generate_"), TEXT("import_"), TEXT("load_"), TEXT("move_"),
			TEXT("paint_"), TEXT("place_"), TEXT("remove_"), TEXT("rename_"), TEXT("repair_"),
			TEXT("reset_"), TEXT("resize_"), TEXT("restore_"), TEXT("save_"), TEXT("set_"),
			TEXT("spawn_"), TEXT("start_"), TEXT("stop_"), TEXT("sync_"), TEXT("update_"),
			TEXT("write_")
		};
		static const TArray<FString> MutationTokens = {
			TEXT("_add_"), TEXT("_apply_"), TEXT("_assign_"), TEXT("_attach_"), TEXT("_bind_"),
			TEXT("_build"), TEXT("_compile"), TEXT("_connect_"), TEXT("_create"), TEXT("_delete"),
			TEXT("_destroy"), TEXT("_disable"), TEXT("_disconnect"), TEXT("_duplicate"),
			TEXT("_enable"), TEXT("_fill_"), TEXT("_generate"), TEXT("_import"), TEXT("_move_"),
			TEXT("_paint_"), TEXT("_place_"), TEXT("_remove"), TEXT("_rename"), TEXT("_repair"),
			TEXT("_reset"), TEXT("_resize"), TEXT("_restore"), TEXT("_save"), TEXT("_set_"),
			TEXT("_spawn"), TEXT("_start"), TEXT("_stop"), TEXT("_sync"), TEXT("_update"),
			TEXT("_write")
		};
		return StartsWithAny(Lower, MutationPrefixes) || ContainsAny(Lower, MutationTokens);
	}

	static FString InferDomain(const FString& Lower)
	{
		if (StartsWithAny(Lower, { TEXT("mcp_"), TEXT("tools_"), TEXT("tool_"), TEXT("toolsets_"), TEXT("job_") })
			|| Lower.StartsWith(TEXT("jobs_")))
		{
			return TEXT("mcp_control");
		}
		if (StartsWithAny(Lower, { TEXT("pcg_") }) || Lower.Contains(TEXT("_pcg_")))
		{
			return TEXT("pcg");
		}
		if (ContainsAny(Lower, { TEXT("landscape"), TEXT("terrain"), TEXT("heightmap"), TEXT("water_"), TEXT("biome"), TEXT("slope"), TEXT("pathability") }))
		{
			return TEXT("terrain");
		}
		if (ContainsAny(Lower, { TEXT("blueprint"), TEXT("bp_"), TEXT("behavior_tree"), TEXT("state_tree"), TEXT("gameplay_ability") }))
		{
			return TEXT("blueprint");
		}
		if (ContainsAny(Lower, { TEXT("material"), TEXT("texture"), TEXT("substrate"), TEXT("shader") }))
		{
			return TEXT("material");
		}
		if (ContainsAny(Lower, { TEXT("niagara"), TEXT("vfx"), TEXT("particle") }))
		{
			return TEXT("vfx");
		}
		if (ContainsAny(Lower, { TEXT("anim"), TEXT("montage"), TEXT("skeleton"), TEXT("skeletal"), TEXT("control_rig"), TEXT("cloth") }))
		{
			return TEXT("animation");
		}
		if (ContainsAny(Lower, { TEXT("staticmesh"), TEXT("static_mesh"), TEXT("mesh_"), TEXT("skelmesh"), TEXT("physics_asset") }))
		{
			return TEXT("mesh");
		}
		if (ContainsAny(Lower, { TEXT("umg"), TEXT("widget") }))
		{
			return TEXT("umg");
		}
		if (ContainsAny(Lower, { TEXT("audio"), TEXT("sound"), TEXT("metasound") }))
		{
			return TEXT("audio");
		}
		if (ContainsAny(Lower, { TEXT("camera"), TEXT("sequence"), TEXT("sequencer"), TEXT("cinematic") }))
		{
			return TEXT("cinematic");
		}
		if (ContainsAny(Lower, { TEXT("lighting"), TEXT("light_"), TEXT("postprocess"), TEXT("reflection_capture"), TEXT("fog") }))
		{
			return TEXT("lighting");
		}
		if (ContainsAny(Lower, { TEXT("world"), TEXT("level"), TEXT("actor"), TEXT("data_layer"), TEXT("foliage"), TEXT("scatter"), TEXT("blockout") }))
		{
			return TEXT("level");
		}
		if (ContainsAny(Lower, { TEXT("cook"), TEXT("package"), TEXT("plugin"), TEXT("sourcecontrol"), TEXT("project_settings"), TEXT("build") }))
		{
			return TEXT("devops");
		}
		if (ContainsAny(Lower, { TEXT("editor_dialog"), TEXT("editor_mode"), TEXT("viewport"), TEXT("pie_"), TEXT("input_"), TEXT("embody") }))
		{
			return TEXT("editor_control");
		}
		if (ContainsAny(Lower, { TEXT("screenshot"), TEXT("qa"), TEXT("receipt"), TEXT("diagnose"), TEXT("validate") }))
		{
			return TEXT("qa");
		}
		return TEXT("asset");
	}

	static FString InferOperationClass(const FString& Lower)
	{
		if (StartsWithAny(Lower, { TEXT("mcp_tool_execution_profile"), TEXT("mcp_resource_lock_plan"), TEXT("mcp_parallel_authoring_plan") })
			|| ContainsAny(Lower, { TEXT("_plan"), TEXT("_preview"), TEXT("_template"), TEXT("_catalog") }))
		{
			return TEXT("plan");
		}
		if (ContainsAny(Lower, { TEXT("watchdog"), TEXT("dialog"), TEXT("editor_mode"), TEXT("viewport"), TEXT("pie_"), TEXT("input_"), TEXT("embody") }))
		{
			return TEXT("editor_ui");
		}
		if (ContainsAny(Lower, { TEXT("cook"), TEXT("package"), TEXT("editor_build"), TEXT("build_lighting"), TEXT("build_navigation"), TEXT("build_ai") }))
		{
			return TEXT("build");
		}
		if (ContainsAny(Lower, { TEXT("compile"), TEXT("recompile"), TEXT("validate_hook") }))
		{
			return TEXT("compile");
		}
		if (ContainsAny(Lower, { TEXT("save"), TEXT("resave") }))
		{
			return TEXT("save");
		}
		if (ContainsAny(Lower, { TEXT("provider"), TEXT("fab_"), TEXT("quixel_"), TEXT("image_generate"), TEXT("audio_generate"), TEXT("model_generate") }))
		{
			return TEXT("provider");
		}
		if (StartsWithAny(Lower, { TEXT("get_"), TEXT("list_"), TEXT("find_"), TEXT("query_"), TEXT("search_"), TEXT("inspect_"), TEXT("describe_"), TEXT("count_"), TEXT("status_") })
			|| ContainsAny(Lower, { TEXT("_get"), TEXT("_list"), TEXT("_find"), TEXT("_query"), TEXT("_search"), TEXT("_inspect"), TEXT("_describe"), TEXT("_status"), TEXT("_explain"), TEXT("_diagnose"), TEXT("_audit"), TEXT("_validate") }))
		{
			return TEXT("read");
		}

		const FString Domain = InferDomain(Lower);
		const bool bMutation = IsMutationToolName(Lower);
		if (bMutation && (Domain == TEXT("level") || Domain == TEXT("terrain") || Domain == TEXT("lighting") || Domain == TEXT("cinematic")
			|| (Domain == TEXT("pcg") && ContainsAny(Lower, { TEXT("generate"), TEXT("spawn") }))))
		{
			return TEXT("write_level");
		}
		if (bMutation)
		{
			return TEXT("write_asset");
		}
		return TEXT("read");
	}

	static FString InferLane(const FString& Domain, const FString& OperationClass, const FString& Lower)
	{
		if (OperationClass == TEXT("plan"))
		{
			return TEXT("agent_plan");
		}
		if (OperationClass == TEXT("provider"))
		{
			return TEXT("provider");
		}
		if (OperationClass == TEXT("build") || OperationClass == TEXT("compile") || OperationClass == TEXT("save"))
		{
			return TEXT("build");
		}
		if (OperationClass == TEXT("editor_ui"))
		{
			return TEXT("editor_ui");
		}
		if (OperationClass == TEXT("read"))
		{
			return ContainsAny(Lower, { TEXT("status"), TEXT("poll"), TEXT("heartbeat"), TEXT("watch") }) ? TEXT("poll") : TEXT("read");
		}
		if (OperationClass == TEXT("write_level") || Domain == TEXT("terrain") || Domain == TEXT("level") || Domain == TEXT("lighting") || Domain == TEXT("cinematic"))
		{
			return TEXT("write_level");
		}
		if (Domain == TEXT("qa"))
		{
			return TEXT("qa");
		}
		return TEXT("write_asset");
	}

	static FString InferThreadModel(const FString& OperationClass, const FString& Lane)
	{
		if (Lane == TEXT("read") || Lane == TEXT("poll") || Lane == TEXT("agent_plan"))
		{
			return TEXT("worker_safe_candidate");
		}
		if (OperationClass == TEXT("provider"))
		{
			return TEXT("external_process");
		}
		if (Lane == TEXT("build"))
		{
			return TEXT("async_job");
		}
		return TEXT("game_thread_commit");
	}

	static void AddStringArray(TSharedRef<FJsonObject> Obj, const FString& Field, const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		Obj->SetArrayField(Field, Values);
	}

	static void AddStringArray(TSharedRef<FJsonObject> Obj, const FString& Field, std::initializer_list<const TCHAR*> Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(FString(Item)));
		}
		Obj->SetArrayField(Field, Values);
	}

	static FString StripObjectSuffix(FString Path)
	{
		int32 DotIndex = INDEX_NONE;
		if (Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Engine/")) || Path.StartsWith(TEXT("/Script/")))
		{
			if (Path.FindChar(TEXT('.'), DotIndex))
			{
				Path = Path.Left(DotIndex);
			}
		}
		return Path;
	}

	static void AddPotentialTargetPath(const FString& RawValue, TArray<FString>& OutPaths)
	{
		FString Value = RawValue.TrimStartAndEnd();
		if (Value.IsEmpty())
		{
			return;
		}
		Value.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
		if (Value.StartsWith(TEXT("Game/")) || Value.StartsWith(TEXT("Engine/")) || Value.StartsWith(TEXT("Script/")))
		{
			Value = TEXT("/") + Value;
		}

		const bool bLooksLikeUePath =
			Value.StartsWith(TEXT("/Game/"))
			|| Value.StartsWith(TEXT("/Engine/"))
			|| Value.StartsWith(TEXT("/Script/"));
		const bool bLooksLikeFilePath =
			Value.Contains(TEXT(":/"))
			|| Value.StartsWith(TEXT("./"))
			|| Value.StartsWith(TEXT("../"))
			|| Value.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".gltf"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".jpg"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".jpeg"), ESearchCase::IgnoreCase)
			|| Value.EndsWith(TEXT(".wav"), ESearchCase::IgnoreCase);
		if (!bLooksLikeUePath && !bLooksLikeFilePath)
		{
			return;
		}

		Value = StripObjectSuffix(Value);
		if (!OutPaths.Contains(Value))
		{
			OutPaths.Add(Value);
		}
	}

	static void ExtractTargetPaths(const TSharedRef<FJsonObject>& Args, TArray<FString>& OutPaths)
	{
		static const TArray<FString> PreferredFields = {
			TEXT("asset_path"), TEXT("asset"), TEXT("target_asset"), TEXT("target_path"), TEXT("output_asset_path"),
			TEXT("package_path"), TEXT("object_path"), TEXT("level_path"), TEXT("map_path"), TEXT("world_path"),
			TEXT("material_path"), TEXT("blueprint_path"), TEXT("widget_path"), TEXT("graph_path"),
			TEXT("pcg_graph_path"), TEXT("niagara_system_path"), TEXT("sequence_path"), TEXT("camera_path"),
			TEXT("texture_path"), TEXT("mesh_path"), TEXT("skeletal_mesh_path"), TEXT("skeleton_path"),
			TEXT("anim_bp_path"), TEXT("sound_path"), TEXT("file_path"), TEXT("source_path"), TEXT("import_path")
		};
		for (const FString& Field : PreferredFields)
		{
			FString Value;
			if (Args->TryGetStringField(Field, Value))
			{
				AddPotentialTargetPath(Value, OutPaths);
			}
		}

		for (const auto& Pair : Args->Values)
		{
			const FString KeyLower = FString(*Pair.Key).ToLower();
			if (!ContainsAny(KeyLower, { TEXT("path"), TEXT("asset"), TEXT("package"), TEXT("level"), TEXT("map"), TEXT("file"), TEXT("source") }))
			{
				continue;
			}
			if (!Pair.Value.IsValid())
			{
				continue;
			}
			if (Pair.Value->Type == EJson::String)
			{
				AddPotentialTargetPath(Pair.Value->AsString(), OutPaths);
			}
			else if (Pair.Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& ArrayValues = Pair.Value->AsArray();
				for (const TSharedPtr<FJsonValue>& Item : ArrayValues)
				{
					if (Item.IsValid() && Item->Type == EJson::String)
					{
						AddPotentialTargetPath(Item->AsString(), OutPaths);
					}
				}
			}
		}
	}

	static void AddLock(TArray<FMcpResourceLock>& Locks, const FString& Id, const FString& Mode, const FString& Reason)
	{
		if (Id.IsEmpty())
		{
			return;
		}
		for (FMcpResourceLock& Existing : Locks)
		{
			if (Existing.Id == Id)
			{
				if (Existing.Mode != TEXT("exclusive") && Mode == TEXT("exclusive"))
				{
					Existing.Mode = Mode;
				}
				if (!Reason.IsEmpty() && !Existing.Reason.Contains(Reason))
				{
					Existing.Reason += Existing.Reason.IsEmpty() ? Reason : (TEXT("; ") + Reason);
				}
				return;
			}
		}
		Locks.Add({ Id, Mode, Reason });
	}

	static FString PackageLockId(const FString& Path)
	{
		if (Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Engine/")) || Path.StartsWith(TEXT("/Script/")))
		{
			return TEXT("package:") + StripObjectSuffix(Path);
		}
		if (Path.Contains(TEXT(":/")) || Path.StartsWith(TEXT("./")) || Path.StartsWith(TEXT("../")))
		{
			return TEXT("file:") + Path;
		}
		return FString();
	}

	static void BuildLocks(FMcpExecutionProfile& Profile, const FString& Lower)
	{
		const bool bWriteLike =
			Profile.OperationClass == TEXT("write_asset")
			|| Profile.OperationClass == TEXT("write_level")
			|| Profile.OperationClass == TEXT("compile")
			|| Profile.OperationClass == TEXT("save")
			|| Profile.OperationClass == TEXT("build")
			|| Profile.OperationClass == TEXT("editor_ui")
			|| Profile.OperationClass == TEXT("provider");
		const FString TargetMode = bWriteLike ? TEXT("exclusive") : TEXT("shared");
		for (const FString& Path : Profile.TargetPaths)
		{
			const FString LockId = PackageLockId(Path);
			if (!LockId.IsEmpty())
			{
				AddLock(Profile.Locks, LockId, TargetMode, TEXT("target path from arguments"));
			}
		}

		if (Profile.OperationClass == TEXT("write_level") || Profile.Domain == TEXT("terrain"))
		{
			AddLock(Profile.Locks, TEXT("world:current"), TEXT("exclusive"), TEXT("level/world mutation must commit on the editor world"));
		}
		if (Profile.Domain == TEXT("terrain") && bWriteLike)
		{
			AddLock(Profile.Locks, TEXT("landscape:current"), TEXT("exclusive"), TEXT("landscape edit lane"));
		}
		if (Profile.Domain == TEXT("pcg") && ContainsAny(Lower, { TEXT("generate"), TEXT("spawn") }))
		{
			AddLock(Profile.Locks, TEXT("world:current"), TEXT("exclusive"), TEXT("PCG generation mutates spawned world content"));
			AddLock(Profile.Locks, TEXT("pcg:generation"), TEXT("exclusive"), TEXT("PCG generate budget and tile-cap lane"));
		}
		if (Profile.OperationClass == TEXT("compile"))
		{
			AddLock(Profile.Locks, TEXT("compile:") + Profile.Domain, TEXT("exclusive"), TEXT("compile/validate step"));
			if (Profile.Domain == TEXT("material"))
			{
				AddLock(Profile.Locks, TEXT("compile:shader"), TEXT("exclusive"), TEXT("material shader compile"));
			}
		}
		if (Profile.OperationClass == TEXT("save"))
		{
			AddLock(Profile.Locks, TEXT("save:global"), TEXT("exclusive"), TEXT("package save must not race unrelated package writes"));
		}
		if (Profile.OperationClass == TEXT("build"))
		{
			AddLock(Profile.Locks, TEXT("build:global"), TEXT("exclusive"), TEXT("editor build/cook/package lane"));
		}
		if (Profile.OperationClass == TEXT("editor_ui"))
		{
			AddLock(Profile.Locks, TEXT("editor_ui:modal"), TEXT("exclusive"), TEXT("modal/editor UI automation lane"));
		}
		if (Profile.Domain == TEXT("blueprint") && bWriteLike)
		{
			AddLock(Profile.Locks, TEXT("compile:blueprint"), Profile.OperationClass == TEXT("compile") ? TEXT("exclusive") : TEXT("shared"), TEXT("blueprint graph changes require compile gate"));
		}
		if (Profile.Domain == TEXT("umg") && bWriteLike)
		{
			AddLock(Profile.Locks, TEXT("compile:umg"), Profile.OperationClass == TEXT("compile") ? TEXT("exclusive") : TEXT("shared"), TEXT("UMG changes require compile gate"));
		}
		if (Profile.Domain == TEXT("material") && bWriteLike)
		{
			AddLock(Profile.Locks, TEXT("compile:shader"), Profile.OperationClass == TEXT("compile") ? TEXT("exclusive") : TEXT("shared"), TEXT("material changes require shader/stat gate"));
		}
	}

	static FMcpExecutionProfile BuildProfile(const FString& ToolName, const TSharedRef<FJsonObject>& Args)
	{
		FMcpExecutionProfile Profile;
		Profile.ToolName = ToolName;
		const FString Lower = ToolName.ToLower();
		Profile.Domain = InferDomain(Lower);
		Profile.OperationClass = InferOperationClass(Lower);
		Profile.Lane = InferLane(Profile.Domain, Profile.OperationClass, Lower);
		Profile.ThreadModel = InferThreadModel(Profile.OperationClass, Profile.Lane);
		ExtractTargetPaths(Args, Profile.TargetPaths);

		Profile.bRequiresReceipt =
			Profile.OperationClass != TEXT("read")
			&& Profile.OperationClass != TEXT("plan");
		Profile.bRequiresTargetBinding =
			Profile.OperationClass == TEXT("write_asset")
			|| Profile.OperationClass == TEXT("write_level")
			|| Profile.OperationClass == TEXT("compile")
			|| Profile.OperationClass == TEXT("save")
			|| Profile.OperationClass == TEXT("build")
			|| Profile.OperationClass == TEXT("editor_ui");

		if (Profile.ThreadModel == TEXT("game_thread_commit"))
		{
			Profile.Notes.Add(TEXT("Prepare work may run off-thread, but UObject/editor mutation must commit on the GameThread."));
		}
		if (Profile.Lane == TEXT("write_level"))
		{
			Profile.Notes.Add(TEXT("Coordinate with terrain, PCG, lighting, camera, and level-manager agents through world-level locks."));
		}
		if (Profile.Domain == TEXT("pcg"))
		{
			Profile.Notes.Add(TEXT("PCG writes should pass catalog, validate, dry-run, tile-cap, and generation receipt gates before delivery."));
		}
		if (Profile.Domain == TEXT("blueprint") || Profile.Domain == TEXT("umg") || Profile.Domain == TEXT("material") || Profile.Domain == TEXT("vfx") || Profile.Domain == TEXT("animation"))
		{
			Profile.Notes.Add(TEXT("Asset authoring must include post-edit readback plus compile/validate evidence before final receipt."));
		}
		if (Profile.TargetPaths.IsEmpty() && Profile.bRequiresTargetBinding)
		{
			Profile.Notes.Add(TEXT("No target path was detected in arguments; client should bind project and target asset/world before dispatch."));
		}

		BuildLocks(Profile, Lower);
		return Profile;
	}

	static TSharedRef<FJsonObject> LockToJson(const FMcpResourceLock& Lock)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Lock.Id);
		Obj->SetStringField(TEXT("mode"), Lock.Mode);
		Obj->SetStringField(TEXT("reason"), Lock.Reason);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> LocksToJson(const TArray<FMcpResourceLock>& Locks)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FMcpResourceLock& Lock : Locks)
		{
			Values.Add(MakeShared<FJsonValueObject>(LockToJson(Lock)));
		}
		return Values;
	}

	static TSharedRef<FJsonObject> ProfileToJson(const FMcpExecutionProfile& Profile, const bool bIncludeLocks)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("somol.mcp_tool_execution_profile:v1"));
		Obj->SetStringField(TEXT("tool_name"), Profile.ToolName);
		Obj->SetStringField(TEXT("domain"), Profile.Domain);
		Obj->SetStringField(TEXT("operation_class"), Profile.OperationClass);
		Obj->SetStringField(TEXT("lane"), Profile.Lane);
		Obj->SetStringField(TEXT("thread_model"), Profile.ThreadModel);
		Obj->SetBoolField(TEXT("requires_receipt"), Profile.bRequiresReceipt);
		Obj->SetBoolField(TEXT("requires_target_binding"), Profile.bRequiresTargetBinding);
		TSharedRef<FJsonObject> FailClosed = MakeShared<FJsonObject>();
		FailClosed->SetBoolField(TEXT("enabled"), Profile.bRequiresReceipt || Profile.bRequiresTargetBinding);
		FailClosed->SetBoolField(TEXT("target_guard_required"), Profile.bRequiresTargetBinding);
		FailClosed->SetBoolField(TEXT("resource_locks_required"), Profile.bRequiresTargetBinding);
		FailClosed->SetBoolField(TEXT("receipt_envelope_required"), Profile.bRequiresReceipt);
		FailClosed->SetStringField(
			TEXT("missing_target_guard_status"),
			Profile.bRequiresTargetBinding ? TEXT("blocked_no_target_guard") : TEXT("not_required"));
		FailClosed->SetStringField(TEXT("modal_no_response_status"), TEXT("blocked_modal_or_mcp_no_response"));
		FailClosed->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_hermes"));
		Obj->SetObjectField(TEXT("fail_closed_contract"), FailClosed);
		AddStringArray(Obj, TEXT("target_paths"), Profile.TargetPaths);
		if (bIncludeLocks)
		{
			Obj->SetArrayField(TEXT("resource_locks"), LocksToJson(Profile.Locks));
		}

		TSharedRef<FJsonObject> Version = MakeShared<FJsonObject>();
		Version->SetStringField(TEXT("min_engine_version"), Profile.MinEngineVersion);
		Version->SetBoolField(TEXT("ue58_preview_only"), Profile.bUe58PreviewOnly);
		Obj->SetObjectField(TEXT("version_contract"), Version);
		AddStringArray(Obj, TEXT("notes"), Profile.Notes);
		return Obj;
	}

	static TSharedRef<FJsonObject> AnyObjectSchema(const FString& Description)
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetStringField(TEXT("description"), Description);
		Schema->SetBoolField(TEXT("additionalProperties"), true);
		return Schema;
	}

	static TSharedRef<FJsonObject> CallSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("call_id"), FSololmcpSchemaBuilder::String(TEXT("Stable client-side call id. Defaults to call_<index>.")) },
				{ TEXT("role_id"), FSololmcpSchemaBuilder::String(TEXT("Optional agent role id that owns this call.")) },
				{ TEXT("task_id"), FSololmcpSchemaBuilder::String(TEXT("Optional long-queue or blackboard task id.")) },
				{ TEXT("tool_name"), FSololmcpSchemaBuilder::String(TEXT("MCP tool name to classify.")) },
				{ TEXT("arguments"), AnyObjectSchema(TEXT("Tool arguments used to infer target paths and locks.")) }
			},
			{ TEXT("tool_name") },
			TEXT("A planned MCP tool call."));
	}

	static bool ParsePlannedCalls(const TSharedRef<FJsonObject>& Args, TArray<FMcpPlannedCall>& OutCalls, FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
		if (!Args->TryGetArrayField(TEXT("calls"), Calls) || !Calls)
		{
			Error = TEXT("Missing required argument: calls");
			return false;
		}

		for (int32 Index = 0; Index < Calls->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> CallObj = (*Calls)[Index].IsValid() ? (*Calls)[Index]->AsObject() : nullptr;
			if (!CallObj.IsValid())
			{
				Error = FString::Printf(TEXT("calls[%d] must be an object."), Index);
				return false;
			}

			FString ToolName;
			if (!CallObj->TryGetStringField(TEXT("tool_name"), ToolName) || ToolName.TrimStartAndEnd().IsEmpty())
			{
				Error = FString::Printf(TEXT("calls[%d].tool_name is required."), Index);
				return false;
			}
			ToolName = ToolName.TrimStartAndEnd();

			FMcpPlannedCall Call;
			Call.ToolName = ToolName;
			if (!CallObj->TryGetStringField(TEXT("call_id"), Call.CallId) || Call.CallId.TrimStartAndEnd().IsEmpty())
			{
				Call.CallId = FString::Printf(TEXT("call_%d"), Index + 1);
			}
			Call.CallId = Call.CallId.TrimStartAndEnd();
			CallObj->TryGetStringField(TEXT("role_id"), Call.RoleId);
			CallObj->TryGetStringField(TEXT("task_id"), Call.TaskId);

			const TSharedPtr<FJsonObject>* ArgObj = nullptr;
			TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
			if (CallObj->TryGetObjectField(TEXT("arguments"), ArgObj) && ArgObj && ArgObj->IsValid())
			{
				ToolArgs = (*ArgObj).ToSharedRef();
			}
			Call.Profile = BuildProfile(Call.ToolName, ToolArgs);
			OutCalls.Add(Call);
		}
		return true;
	}

	static TSharedRef<FJsonObject> PlannedCallToJson(const FMcpPlannedCall& Call, const bool bIncludeProfile)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("call_id"), Call.CallId);
		Obj->SetStringField(TEXT("tool_name"), Call.ToolName);
		if (!Call.RoleId.IsEmpty())
		{
			Obj->SetStringField(TEXT("role_id"), Call.RoleId);
		}
		if (!Call.TaskId.IsEmpty())
		{
			Obj->SetStringField(TEXT("task_id"), Call.TaskId);
		}
		Obj->SetStringField(TEXT("domain"), Call.Profile.Domain);
		Obj->SetStringField(TEXT("operation_class"), Call.Profile.OperationClass);
		Obj->SetStringField(TEXT("lane"), Call.Profile.Lane);
		Obj->SetStringField(TEXT("thread_model"), Call.Profile.ThreadModel);
		Obj->SetBoolField(TEXT("requires_receipt"), Call.Profile.bRequiresReceipt);
		Obj->SetBoolField(TEXT("requires_target_binding"), Call.Profile.bRequiresTargetBinding);
		Obj->SetStringField(
			TEXT("fail_closed_status_if_unbound"),
			Call.Profile.bRequiresTargetBinding ? TEXT("blocked_no_target_guard") : TEXT("not_required"));
		if (bIncludeProfile)
		{
			Obj->SetObjectField(TEXT("profile"), ProfileToJson(Call.Profile, true));
		}
		return Obj;
	}

	static bool LocksConflict(const FMcpResourceLock& A, const FMcpResourceLock& B)
	{
		return A.Id == B.Id && (A.Mode == TEXT("exclusive") || B.Mode == TEXT("exclusive"));
	}

	static bool ProfilesConflict(const FMcpExecutionProfile& A, const FMcpExecutionProfile& B, FString* OutLockId = nullptr)
	{
		for (const FMcpResourceLock& LockA : A.Locks)
		{
			for (const FMcpResourceLock& LockB : B.Locks)
			{
				if (LocksConflict(LockA, LockB))
				{
					if (OutLockId)
					{
						*OutLockId = LockA.Id;
					}
					return true;
				}
			}
		}
		return false;
	}

	static void AddExecutionPlanningTool(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Tool;
		Tool.Name = TEXT("mcp_tool_execution_profile");
		Tool.Description = TEXT("Classify one MCP tool call into domain, lane, thread model, receipt requirements, target paths, and resource locks for multi-agent dispatch.");
		Tool.InputSchema = FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("tool_name"), FSololmcpSchemaBuilder::String(TEXT("MCP tool name to classify.")) },
				{ TEXT("arguments"), AnyObjectSchema(TEXT("Tool arguments used to infer target paths and locks.")) },
				{ TEXT("include_locks"), FSololmcpSchemaBuilder::Boolean(TEXT("Include inferred resource_locks. Defaults to true.")) }
			},
			{ TEXT("tool_name") });
		Tool.CacheTtlSeconds = 60;
		Tool.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			FString ToolName;
			if (!Args->TryGetStringField(TEXT("tool_name"), ToolName) || ToolName.TrimStartAndEnd().IsEmpty())
			{
				Error = TEXT("Missing required argument: tool_name");
				return false;
			}
			ToolName = ToolName.TrimStartAndEnd();

			const TSharedPtr<FJsonObject>* ArgObj = nullptr;
			TSharedRef<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
			if (Args->TryGetObjectField(TEXT("arguments"), ArgObj) && ArgObj && ArgObj->IsValid())
			{
				ToolArgs = (*ArgObj).ToSharedRef();
			}

			bool bIncludeLocks = true;
			Args->TryGetBoolField(TEXT("include_locks"), bIncludeLocks);
			const FMcpExecutionProfile Profile = BuildProfile(ToolName, ToolArgs);
			Out = ProfileToJson(Profile, bIncludeLocks);
			Out->SetBoolField(TEXT("success"), true);
			Summary = FString::Printf(TEXT("%s -> lane=%s, operation=%s, locks=%d"),
				*ToolName,
				*Profile.Lane,
				*Profile.OperationClass,
				Profile.Locks.Num());
			return true;
		};
		Registry.Register(Tool);
	}

	static void AddResourceLockPlanTool(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Tool;
		Tool.Name = TEXT("mcp_resource_lock_plan");
		Tool.Description = TEXT("Infer resource locks for a batch of MCP tool calls and report conflicts before dispatching multi-agent work.");
		Tool.InputSchema = FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("calls"), FSololmcpSchemaBuilder::Array(CallSchema(), TEXT("Planned MCP calls.")) }
			},
			{ TEXT("calls") });
		Tool.CacheTtlSeconds = 60;
		Tool.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			TArray<FMcpPlannedCall> Calls;
			if (!ParsePlannedCalls(Args, Calls, Error))
			{
				return false;
			}

			Out->SetStringField(TEXT("schema"), TEXT("somol.mcp_resource_lock_plan:v1"));
			Out->SetNumberField(TEXT("call_count"), Calls.Num());

			TArray<TSharedPtr<FJsonValue>> CallValues;
			TArray<TSharedPtr<FJsonValue>> LockValues;
			for (const FMcpPlannedCall& Call : Calls)
			{
				CallValues.Add(MakeShared<FJsonValueObject>(PlannedCallToJson(Call, true)));
				for (const FMcpResourceLock& Lock : Call.Profile.Locks)
				{
					TSharedRef<FJsonObject> LockObj = LockToJson(Lock);
					LockObj->SetStringField(TEXT("call_id"), Call.CallId);
					LockObj->SetStringField(TEXT("tool_name"), Call.ToolName);
					LockValues.Add(MakeShared<FJsonValueObject>(LockObj));
				}
			}
			Out->SetArrayField(TEXT("calls"), CallValues);
			Out->SetArrayField(TEXT("locks"), LockValues);

			TArray<TSharedPtr<FJsonValue>> Conflicts;
			for (int32 A = 0; A < Calls.Num(); ++A)
			{
				for (int32 B = A + 1; B < Calls.Num(); ++B)
				{
					for (const FMcpResourceLock& LockA : Calls[A].Profile.Locks)
					{
						for (const FMcpResourceLock& LockB : Calls[B].Profile.Locks)
						{
							if (!LocksConflict(LockA, LockB))
							{
								continue;
							}
							TSharedRef<FJsonObject> Conflict = MakeShared<FJsonObject>();
							Conflict->SetStringField(TEXT("lock_id"), LockA.Id);
							Conflict->SetStringField(TEXT("call_a"), Calls[A].CallId);
							Conflict->SetStringField(TEXT("tool_a"), Calls[A].ToolName);
							Conflict->SetStringField(TEXT("mode_a"), LockA.Mode);
							Conflict->SetStringField(TEXT("call_b"), Calls[B].CallId);
							Conflict->SetStringField(TEXT("tool_b"), Calls[B].ToolName);
							Conflict->SetStringField(TEXT("mode_b"), LockB.Mode);
							Conflict->SetStringField(TEXT("reason"), TEXT("matching lock id with at least one exclusive mode"));
							Conflicts.Add(MakeShared<FJsonValueObject>(Conflict));
						}
					}
				}
			}
			Out->SetArrayField(TEXT("conflicts"), Conflicts);
			Out->SetBoolField(TEXT("has_conflicts"), Conflicts.Num() > 0);
			Out->SetBoolField(TEXT("success"), true);

			Summary = FString::Printf(TEXT("Resource lock plan: %d calls, %d locks, %d conflicts."),
				Calls.Num(),
				LockValues.Num(),
				Conflicts.Num());
			return true;
		};
		Registry.Register(Tool);
	}

	static void AddParallelAuthoringPlanTool(FSololmcpToolRegistry& Registry)
	{
		FSololmcpToolDefinition Tool;
		Tool.Name = TEXT("mcp_parallel_authoring_plan");
		Tool.Description = TEXT("Schedule MCP tool calls into non-conflicting parallel waves using inferred lanes and resource locks.");
		Tool.InputSchema = FSololmcpSchemaBuilder::Object(
			{
				{ TEXT("calls"), FSololmcpSchemaBuilder::Array(CallSchema(), TEXT("Planned MCP calls.")) },
				{ TEXT("max_wave_size"), FSololmcpSchemaBuilder::Integer(TEXT("Optional maximum calls per wave. Defaults to 16.")) }
			},
			{ TEXT("calls") });
		Tool.CacheTtlSeconds = 60;
		Tool.Execute = [](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			TArray<FMcpPlannedCall> Calls;
			if (!ParsePlannedCalls(Args, Calls, Error))
			{
				return false;
			}

			double RawMaxWave = 16.0;
			Args->TryGetNumberField(TEXT("max_wave_size"), RawMaxWave);
			const int32 MaxWaveSize = FMath::Clamp(static_cast<int32>(RawMaxWave), 1, 64);

			TArray<TArray<int32>> Waves;
			for (int32 CallIndex = 0; CallIndex < Calls.Num(); ++CallIndex)
			{
				bool bPlaced = false;
				for (TArray<int32>& Wave : Waves)
				{
					if (Wave.Num() >= MaxWaveSize)
					{
						continue;
					}
					bool bConflicts = false;
					for (const int32 ExistingIndex : Wave)
					{
						if (ProfilesConflict(Calls[CallIndex].Profile, Calls[ExistingIndex].Profile))
						{
							bConflicts = true;
							break;
						}
					}
					if (!bConflicts)
					{
						Wave.Add(CallIndex);
						bPlaced = true;
						break;
					}
				}
				if (!bPlaced)
				{
					TArray<int32> NewWave;
					NewWave.Add(CallIndex);
					Waves.Add(NewWave);
				}
			}

			TArray<TSharedPtr<FJsonValue>> WaveValues;
			for (int32 WaveIndex = 0; WaveIndex < Waves.Num(); ++WaveIndex)
			{
				TSharedRef<FJsonObject> WaveObj = MakeShared<FJsonObject>();
				WaveObj->SetNumberField(TEXT("wave_index"), WaveIndex + 1);
				TArray<TSharedPtr<FJsonValue>> WaveCalls;
				TMap<FString, int32> LaneCounts;
				TSet<FString> LockIds;
				for (const int32 CallIndex : Waves[WaveIndex])
				{
					const FMcpPlannedCall& Call = Calls[CallIndex];
					WaveCalls.Add(MakeShared<FJsonValueObject>(PlannedCallToJson(Call, false)));
					LaneCounts.FindOrAdd(Call.Profile.Lane) += 1;
					for (const FMcpResourceLock& Lock : Call.Profile.Locks)
					{
						LockIds.Add(Lock.Id);
					}
				}
				WaveObj->SetArrayField(TEXT("calls"), WaveCalls);

				TSharedRef<FJsonObject> LaneObj = MakeShared<FJsonObject>();
				for (const TPair<FString, int32>& Pair : LaneCounts)
				{
					LaneObj->SetNumberField(Pair.Key, Pair.Value);
				}
				WaveObj->SetObjectField(TEXT("lane_counts"), LaneObj);

				TArray<FString> SortedLocks = LockIds.Array();
				SortedLocks.Sort();
				AddStringArray(WaveObj, TEXT("lock_ids"), SortedLocks);
				WaveValues.Add(MakeShared<FJsonValueObject>(WaveObj));
			}

			Out->SetStringField(TEXT("schema"), TEXT("somol.mcp_parallel_authoring_plan:v1"));
			Out->SetNumberField(TEXT("call_count"), Calls.Num());
			Out->SetNumberField(TEXT("wave_count"), Waves.Num());
			Out->SetNumberField(TEXT("max_wave_size"), MaxWaveSize);
			Out->SetArrayField(TEXT("waves"), WaveValues);
			AddStringArray(Out, TEXT("dispatch_guidance"), {
				TEXT("Read, poll, and agent_plan waves may use pooled TCP clients or worker-safe jobs where the concrete tool is worker-approved."),
				TEXT("write_asset waves can run concurrently when package locks do not overlap, but each UObject mutation still commits on the GameThread."),
				TEXT("write_level, editor_ui, build, compile, and save lanes should be treated as serialized commit lanes unless this plan proves no lock overlap."),
				TEXT("Final delivery still requires project/instance binding, readback, validation, screenshot or preview evidence, and receipt gates.")
			});
			Out->SetBoolField(TEXT("success"), true);
			Summary = FString::Printf(TEXT("Parallel authoring plan: %d calls scheduled into %d waves."), Calls.Num(), Waves.Num());
			return true;
		};
		Registry.Register(Tool);
	}
}

void RegisterMcpExecutionPlanningTools(FSololmcpToolRegistry& Registry)
{
	AddExecutionPlanningTool(Registry);
	AddResourceLockPlanTool(Registry);
	AddParallelAuthoringPlanTool(Registry);
}
}
