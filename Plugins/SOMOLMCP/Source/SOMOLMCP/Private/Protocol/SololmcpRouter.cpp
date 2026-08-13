// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Protocol/SololmcpRouter.h"
#include "SOMOLMCP.h"  // LogSOMOLMCP

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/Event.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Protocol/SololmcpJobService.h"
#include "Services/SololmcpEditorServices.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SololmcpJsonUtils.h"
#include "Tools/SololmcpToolRegistry.h"

namespace UE::SOMOLMCP
{
	namespace
	{
		FString GetStringParam(const TSharedRef<FJsonObject>& Params, const FString& FieldName)
		{
			FString Value;
			Params->TryGetStringField(FieldName, Value);
			return Value;
		}

		FString JsonRpcIdToStableString(const TSharedPtr<FJsonValue>& Id)
		{
			if (!Id.IsValid())
			{
				return FString();
			}
			if (Id->Type == EJson::String)
			{
				return Id->AsString();
			}
			if (Id->Type == EJson::Number)
			{
				return FString::Printf(TEXT("%.0f"), Id->AsNumber());
			}
			if (Id->Type == EJson::Boolean)
			{
				return Id->AsBool() ? TEXT("true") : TEXT("false");
			}
			if (Id->Type == EJson::Null)
			{
				return TEXT("null");
			}
			return FString();
		}

		TArray<TSharedPtr<FJsonValue>> BuildPromptArgsSchema()
		{
			TArray<TSharedPtr<FJsonValue>> Args;
			{
				TSharedRef<FJsonObject> Arg = MakeShared<FJsonObject>();
				Arg->SetStringField(TEXT("name"), TEXT("goal"));
				Arg->SetStringField(TEXT("description"), TEXT("What you want to accomplish."));
				Arg->SetBoolField(TEXT("required"), true);
				Args.Add(MakeShared<FJsonValueObject>(Arg));
			}
			{
				TSharedRef<FJsonObject> Arg = MakeShared<FJsonObject>();
				Arg->SetStringField(TEXT("name"), TEXT("constraints"));
				Arg->SetStringField(TEXT("description"), TEXT("Optional constraints, one line each."));
				Arg->SetBoolField(TEXT("required"), false);
				Args.Add(MakeShared<FJsonValueObject>(Arg));
			}
			return Args;
		}

		TSharedRef<FJsonObject> BuildResourceItem(const FString& Uri, const FString& Name, const FString& Description, const FString& MimeType)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("uri"), Uri);
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("description"), Description);
			Item->SetStringField(TEXT("mimeType"), MimeType);
			return Item;
		}

		struct FToolListView
		{
			TArray<TSharedPtr<FJsonValue>> Tools;
			int32 Total = 0;
			int32 Cursor = 0;
			int32 Limit = 0;
			int32 NextCursor = -1;
			FString Toolset;
			FString Prefix;
			FString Query;
			bool bHadParams = false;
			bool bIncludeSchemas = true;
			bool bNamesOnly = false;
		};

		FString NormalizeToolsetName(FString Name)
		{
			Name = Name.TrimStartAndEnd().ToLower();
			Name.ReplaceInline(TEXT("-"), TEXT("_"), ESearchCase::CaseSensitive);
			Name.ReplaceInline(TEXT(" "), TEXT("_"), ESearchCase::CaseSensitive);
			while (Name.Contains(TEXT("__")))
			{
				Name.ReplaceInline(TEXT("__"), TEXT("_"), ESearchCase::CaseSensitive);
			}
			return Name;
		}

		/**
		 * Tools every session sees before activating anything.
		 *
		 * Derived from a rule rather than listed by hand, so a newly registered
		 * discovery or dispatch tool joins the bootstrap set automatically instead of
		 * waiting for someone to remember a curation table. The rule is "the tools
		 * that let a client find and reach everything else": the catalog, the generic
		 * dispatchers, and the session handles those dispatchers hand out.
		 */
		bool IsBootstrapTool(const FString& ToolName)
		{
			// geometry_op_/geometry_mesh_ rather than geometry_: the bare prefix also
			// matches ~28 purpose-built geometry tools, which belong to a group a
			// caller activates like any other, not to the bootstrap set.
			return ToolName == TEXT("tool_catalog")
				|| ToolName.StartsWith(TEXT("editor_api_"), ESearchCase::CaseSensitive)
				|| ToolName.StartsWith(TEXT("geometry_op_"), ESearchCase::CaseSensitive)
				|| ToolName.StartsWith(TEXT("geometry_mesh_"), ESearchCase::CaseSensitive)
				|| ToolName.StartsWith(TEXT("handle_"), ESearchCase::CaseSensitive);
		}

		/** The group a tool activates: its first underscore-separated segment. */
		FString ToolGroupOf(const FString& ToolName)
		{
			int32 Underscore = INDEX_NONE;
			return (ToolName.FindChar(TEXT('_'), Underscore) && Underscore > 0)
				? ToolName.Left(Underscore)
				: ToolName;
		}

		FString InferToolsetName(const FString& ToolName)
		{
			const FString Lower = ToolName.ToLower();
			const TCHAR* CompoundPrefixes[] = {
				TEXT("animation_asset"),
				TEXT("asset_graph"),
				TEXT("asset_index"),
				TEXT("asset_intelligence"),
				TEXT("behavior_tree"),
				TEXT("blueprint_component"),
				TEXT("blueprint_graph"),
				TEXT("blueprint_variable"),
				TEXT("control_rig"),
				TEXT("data_layer"),
				TEXT("editor_build"),
				TEXT("editor_dialog"),
				TEXT("editor_mode"),
				TEXT("game_feature"),
				TEXT("gameplay_ability"),
				TEXT("gameplay_tag"),
				TEXT("level_sequence"),
				TEXT("long_queue"),
				TEXT("material_expression"),
				TEXT("mcp_command"),
				TEXT("mcp_connection"),
				TEXT("mission_control"),
				TEXT("node_graph"),
				TEXT("physics_asset"),
				TEXT("project_settings"),
				TEXT("skeletal_mesh"),
				TEXT("static_mesh"),
				TEXT("state_tree"),
				TEXT("texture_studio"),
				TEXT("world_partition")
			};

			for (const TCHAR* Prefix : CompoundPrefixes)
			{
				const FString PrefixString(Prefix);
				if (Lower == PrefixString || Lower.StartsWith(PrefixString + TEXT("_")))
				{
					return PrefixString;
				}
			}

			int32 Underscore = INDEX_NONE;
			if (Lower.FindChar(TEXT('_'), Underscore) && Underscore > 0)
			{
				return Lower.Left(Underscore);
			}
			return Lower.IsEmpty() ? TEXT("misc") : Lower;
		}

		FString MakeToolsetTitle(const FString& Toolset)
		{
			FString Title = Toolset;
			Title.ReplaceInline(TEXT("_"), TEXT(" "), ESearchCase::CaseSensitive);
			return Title;
		}

		TSharedPtr<FJsonObject> GetParamsObject(const TSharedRef<FJsonObject>& Request)
		{
			const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
			if (Request->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr && ParamsPtr->IsValid())
			{
				return *ParamsPtr;
			}
			return nullptr;
		}

		bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, bool DefaultValue)
		{
			bool Value = DefaultValue;
			if (Params.IsValid() && Params->TryGetBoolField(FieldName, Value))
			{
				return Value;
			}
			return DefaultValue;
		}

		FString GetStringParamOptional(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
		{
			FString Value;
			if (Params.IsValid() && Params->TryGetStringField(FieldName, Value))
			{
				return Value.TrimStartAndEnd();
			}
			return FString();
		}

		int32 GetIntParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, int32 DefaultValue)
		{
			if (!Params.IsValid())
			{
				return DefaultValue;
			}

			double NumberValue = 0.0;
			if (Params->TryGetNumberField(FieldName, NumberValue))
			{
				return static_cast<int32>(NumberValue);
			}

			FString StringValue;
			if (Params->TryGetStringField(FieldName, StringValue))
			{
				return FCString::Atoi(*StringValue);
			}

			return DefaultValue;
		}

		int32 GetCursorParam(const TSharedPtr<FJsonObject>& Params)
		{
			return FMath::Max(0, GetIntParam(Params, TEXT("cursor"), 0));
		}

		TSharedRef<FJsonObject> AssetDataToResourceJson(const FAssetData& AssetData)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
			Obj->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
			Obj->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
			Obj->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
			Obj->SetStringField(TEXT("class_path"), AssetData.AssetClassPath.ToString());
			return Obj;
		}

		void BuildAssetIndexResourcePayload(const TSharedRef<FJsonObject>& Payload, const TSharedPtr<FJsonObject>& Params)
		{
			Payload->SetStringField(TEXT("schema"), TEXT("somolmcp.asset_index.resource:v2"));
			Payload->SetStringField(TEXT("project_dir"), FPaths::ProjectDir());
			Payload->SetStringField(TEXT("content_dir"), FPaths::ProjectContentDir());
			Payload->SetStringField(TEXT("asset_uri_template"), TEXT("somolmcp://asset/{asset_id}"));
			Payload->SetStringField(TEXT("index_uri"), TEXT("somolmcp://asset-index"));
			Payload->SetStringField(TEXT("guidance"), TEXT("Use this resource as lightweight read-only context. Pass include_items=true plus package_path/query/class_filter for bounded pages; avoid editor-thread full scans."));
			Payload->SetBoolField(TEXT("read_only"), true);
			Payload->SetBoolField(TEXT("subscribe_supported"), true);

			const bool bIncludeItems = GetBoolParam(Params, TEXT("include_items"), false);
			const FString PackagePath = GetStringParamOptional(Params, TEXT("package_path")).IsEmpty()
				? TEXT("/Game")
				: GetStringParamOptional(Params, TEXT("package_path"));
			const FString Query = GetStringParamOptional(Params, TEXT("query"));
			const FString ClassFilter = GetStringParamOptional(Params, TEXT("class_filter"));
			const int32 Cursor = GetCursorParam(Params);
			const int32 Limit = FMath::Clamp(GetIntParam(Params, TEXT("limit"), 50), 1, 200);
			Payload->SetBoolField(TEXT("items_requested"), bIncludeItems);
			Payload->SetStringField(TEXT("package_path"), PackagePath);
			Payload->SetStringField(TEXT("query"), Query);
			Payload->SetStringField(TEXT("class_filter"), ClassFilter);
			Payload->SetNumberField(TEXT("cursor"), Cursor);
			Payload->SetNumberField(TEXT("limit"), Limit);

			if (!bIncludeItems)
			{
				Payload->SetStringField(TEXT("status"), TEXT("metadata_only"));
				return;
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			Payload->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());

			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*PackagePath));
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssets(Filter, Assets);

			TArray<TSharedPtr<FJsonValue>> Items;
			TMap<FString, int32> ClassCounts;
			int32 Matched = 0;
			const FString QueryLower = Query.ToLower();
			const FString ClassLower = ClassFilter.ToLower();
			for (const FAssetData& AssetData : Assets)
			{
				const FString AssetName = AssetData.AssetName.ToString();
				const FString ObjectPath = AssetData.GetObjectPathString();
				const FString ClassPath = AssetData.AssetClassPath.ToString();
				if (!ClassLower.IsEmpty() && !ClassPath.ToLower().Contains(ClassLower))
				{
					continue;
				}
				if (!QueryLower.IsEmpty())
				{
					const FString Haystack = FString::Printf(TEXT("%s %s %s %s"),
						*AssetName,
						*ObjectPath,
						*AssetData.PackagePath.ToString(),
						*ClassPath).ToLower();
					if (!Haystack.Contains(QueryLower))
					{
						continue;
					}
				}

				ClassCounts.FindOrAdd(ClassPath) += 1;
				if (Matched >= Cursor && Items.Num() < Limit)
				{
					Items.Add(MakeShared<FJsonValueObject>(AssetDataToResourceJson(AssetData)));
				}
				++Matched;
			}

			TArray<TSharedPtr<FJsonValue>> ClassHistogram;
			for (const TPair<FString, int32>& Pair : ClassCounts)
			{
				if (ClassHistogram.Num() >= 32)
				{
					break;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("class_path"), Pair.Key);
				Row->SetNumberField(TEXT("count"), Pair.Value);
				ClassHistogram.Add(MakeShared<FJsonValueObject>(Row));
			}

			Payload->SetStringField(TEXT("status"), TEXT("paged_items"));
			Payload->SetNumberField(TEXT("total_matching"), Matched);
			Payload->SetNumberField(TEXT("returned"), Items.Num());
			if (Cursor + Items.Num() < Matched)
			{
				Payload->SetStringField(TEXT("nextCursor"), FString::FromInt(Cursor + Items.Num()));
			}
			Payload->SetArrayField(TEXT("items"), Items);
			Payload->SetArrayField(TEXT("class_histogram"), ClassHistogram);
			Payload->SetBoolField(TEXT("truncated"), Cursor + Items.Num() < Matched);
		}

		void BuildCurrentLevelManifestResourcePayload(const TSharedRef<FJsonObject>& Payload, const TSharedPtr<FJsonObject>& Params)
		{
			Payload->SetStringField(TEXT("schema"), TEXT("somolmcp.level_manifest.resource:v2"));
			Payload->SetStringField(TEXT("project_name"), FApp::GetProjectName());
			Payload->SetStringField(TEXT("project_dir"), FPaths::ProjectDir());
			Payload->SetStringField(TEXT("content_dir"), FPaths::ProjectContentDir());
			Payload->SetStringField(TEXT("manifest_scope"), TEXT("lightweight_current_editor_world"));
			Payload->SetStringField(TEXT("actor_manifest_tool"), TEXT("actor_list"));
			Payload->SetStringField(TEXT("level_manifest_tool"), TEXT("level_actor_list"));
			Payload->SetStringField(TEXT("guidance"), TEXT("This resource returns bounded current-world context. Pass include_actor_samples=true for a paged sample; use dedicated actor tools for larger reads."));

			UWorld* World = (GEditor ? GEditor->GetEditorWorldContext().World() : nullptr);
			if (!World)
			{
				Payload->SetStringField(TEXT("status"), TEXT("no_editor_world"));
				return;
			}

			Payload->SetStringField(TEXT("status"), TEXT("world_available"));
			Payload->SetStringField(TEXT("world_name"), World->GetName());
			Payload->SetStringField(TEXT("map_name"), World->GetMapName());
			ULevel* PersistentLevel = World->PersistentLevel;
			ULevel* CurrentLevel = World->GetCurrentLevel();
			Payload->SetStringField(TEXT("persistent_level"), PersistentLevel && PersistentLevel->GetOutermost() ? PersistentLevel->GetOutermost()->GetPathName() : FString());
			Payload->SetStringField(TEXT("current_level"), CurrentLevel && CurrentLevel->GetOutermost() ? CurrentLevel->GetOutermost()->GetPathName() : FString());

			TArray<TSharedPtr<FJsonValue>> Levels;
			int32 TotalActors = 0;
			for (ULevel* Level : World->GetLevels())
			{
				if (!Level)
				{
					continue;
				}
				TSharedRef<FJsonObject> LevelObj = MakeShared<FJsonObject>();
				LevelObj->SetStringField(TEXT("name"), Level->GetName());
				LevelObj->SetStringField(TEXT("package_path"), Level->GetOutermost() ? Level->GetOutermost()->GetPathName() : FString());
				LevelObj->SetBoolField(TEXT("is_current"), Level == CurrentLevel);
				LevelObj->SetBoolField(TEXT("is_persistent"), Level == PersistentLevel);
				LevelObj->SetNumberField(TEXT("actor_count"), Level->Actors.Num());
				Levels.Add(MakeShared<FJsonValueObject>(LevelObj));
				TotalActors += Level->Actors.Num();
			}
			Payload->SetNumberField(TEXT("loaded_level_count"), Levels.Num());
			Payload->SetNumberField(TEXT("total_actor_slots"), TotalActors);
			Payload->SetArrayField(TEXT("levels"), Levels);

			const bool bIncludeActors = GetBoolParam(Params, TEXT("include_actor_samples"), false);
			const FString NameFilter = GetStringParamOptional(Params, TEXT("filter_name_contains"));
			const FString ClassFilter = GetStringParamOptional(Params, TEXT("filter_class"));
			const int32 Cursor = GetCursorParam(Params);
			const int32 Limit = FMath::Clamp(GetIntParam(Params, TEXT("limit"), 50), 1, 200);
			Payload->SetBoolField(TEXT("actor_samples_requested"), bIncludeActors);
			Payload->SetStringField(TEXT("filter_name_contains"), NameFilter);
			Payload->SetStringField(TEXT("filter_class"), ClassFilter);
			Payload->SetNumberField(TEXT("cursor"), Cursor);
			Payload->SetNumberField(TEXT("limit"), Limit);
			if (!bIncludeActors)
			{
				return;
			}

			TArray<TSharedPtr<FJsonValue>> Actors;
			TMap<FString, int32> ClassCounts;
			int32 Matched = 0;
			for (ULevel* Level : World->GetLevels())
			{
				if (!Level)
				{
					continue;
				}
				for (AActor* Actor : Level->Actors)
				{
					if (!Actor)
					{
						continue;
					}
					const FString Label = Actor->GetActorLabel();
					const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
					if (!NameFilter.IsEmpty() && !Label.Contains(NameFilter, ESearchCase::IgnoreCase) && !Actor->GetName().Contains(NameFilter, ESearchCase::IgnoreCase))
					{
						continue;
					}
					if (!ClassFilter.IsEmpty() && !ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
					{
						continue;
					}
					ClassCounts.FindOrAdd(ClassName) += 1;
					if (Matched >= Cursor && Actors.Num() < Limit)
					{
						TSharedRef<FJsonObject> ActorObj = MakeShared<FJsonObject>();
						ActorObj->SetStringField(TEXT("label"), Label);
						ActorObj->SetStringField(TEXT("name"), Actor->GetName());
						ActorObj->SetStringField(TEXT("class"), ClassName);
						ActorObj->SetStringField(TEXT("path"), Actor->GetPathName());
						ActorObj->SetStringField(TEXT("level"), Level->GetName());
						const FVector Location = Actor->GetActorLocation();
						TSharedRef<FJsonObject> Loc = MakeShared<FJsonObject>();
						Loc->SetNumberField(TEXT("x"), Location.X);
						Loc->SetNumberField(TEXT("y"), Location.Y);
						Loc->SetNumberField(TEXT("z"), Location.Z);
						ActorObj->SetObjectField(TEXT("location"), Loc);
						Actors.Add(MakeShared<FJsonValueObject>(ActorObj));
					}
					++Matched;
				}
			}

			TArray<TSharedPtr<FJsonValue>> ClassHistogram;
			for (const TPair<FString, int32>& Pair : ClassCounts)
			{
				if (ClassHistogram.Num() >= 32)
				{
					break;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("class"), Pair.Key);
				Row->SetNumberField(TEXT("count"), Pair.Value);
				ClassHistogram.Add(MakeShared<FJsonValueObject>(Row));
			}
			Payload->SetNumberField(TEXT("total_matching_actors"), Matched);
			Payload->SetNumberField(TEXT("returned_actors"), Actors.Num());
			if (Cursor + Actors.Num() < Matched)
			{
				Payload->SetStringField(TEXT("nextCursor"), FString::FromInt(Cursor + Actors.Num()));
			}
			Payload->SetArrayField(TEXT("actors"), Actors);
			Payload->SetArrayField(TEXT("class_histogram"), ClassHistogram);
			Payload->SetBoolField(TEXT("truncated"), Cursor + Actors.Num() < Matched);
		}

		TSharedRef<FJsonObject> MakeToolsetSummaryObject(const FString& Toolset, int32 ToolCount, const TArray<FString>& Samples)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Toolset);
			Obj->SetStringField(TEXT("title"), MakeToolsetTitle(Toolset));
			Obj->SetStringField(TEXT("description"), FString::Printf(
				TEXT("SOMOLMCP tools inferred from the '%s' registry namespace."),
				*Toolset));
			Obj->SetNumberField(TEXT("tool_count"), ToolCount);
			Obj->SetBoolField(TEXT("loaded"), true);
			Obj->SetBoolField(TEXT("lazy"), false);
			Obj->SetStringField(TEXT("source"), TEXT("somolmcp.registry.namespace"));
			Obj->SetStringField(TEXT("engine_api_status"), TEXT("runtime_available"));

			TArray<TSharedPtr<FJsonValue>> SampleValues;
			for (const FString& Sample : Samples)
			{
				SampleValues.Add(MakeShared<FJsonValueString>(Sample));
			}
			Obj->SetArrayField(TEXT("sample_tools"), SampleValues);
			return Obj;
		}

		TArray<TSharedPtr<FJsonValue>> BuildToolsetSummaries(
			const TArray<TSharedPtr<FJsonValue>>& Tools,
			const FString& Query,
			int32& OutTotal)
		{
			TArray<FString> Names;
			TMap<FString, int32> Counts;
			TMap<FString, TArray<FString>> Samples;

			for (const TSharedPtr<FJsonValue>& ToolValue : Tools)
			{
				const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				if (!ToolObj.IsValid())
				{
					continue;
				}

				FString ToolName;
				if (!ToolObj->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
				{
					continue;
				}

				const FString Toolset = InferToolsetName(ToolName);
				if (!Counts.Contains(Toolset))
				{
					Names.Add(Toolset);
				}
				Counts.FindOrAdd(Toolset) += 1;
				TArray<FString>& ToolsetSamples = Samples.FindOrAdd(Toolset);
				if (ToolsetSamples.Num() < 5)
				{
					ToolsetSamples.Add(ToolName);
				}
			}

			Names.Sort();
			TArray<TSharedPtr<FJsonValue>> Out;
			const FString QueryLower = Query.TrimStartAndEnd().ToLower();
			for (const FString& Name : Names)
			{
				const bool bQueryMatch =
					QueryLower.IsEmpty()
					|| Name.Contains(QueryLower, ESearchCase::IgnoreCase)
					|| MakeToolsetTitle(Name).Contains(QueryLower, ESearchCase::IgnoreCase);
				if (!bQueryMatch)
				{
					continue;
				}

				Out.Add(MakeShared<FJsonValueObject>(
					MakeToolsetSummaryObject(Name, Counts.FindRef(Name), Samples.FindRef(Name))));
			}

			OutTotal = Out.Num();
			return Out;
		}

		FToolListView BuildToolListView(
			TArray<TSharedPtr<FJsonValue>> AllTools,
			const TSharedPtr<FJsonObject>& Params,
			const FString& ForcedToolset = FString())
		{
			FToolListView View;
			View.bHadParams = Params.IsValid();
			View.Toolset = ForcedToolset.IsEmpty()
				? GetStringParamOptional(Params, TEXT("toolset"))
				: ForcedToolset;
			if (View.Toolset.IsEmpty())
			{
				View.Toolset = GetStringParamOptional(Params, TEXT("namespace"));
			}
			View.Toolset = NormalizeToolsetName(View.Toolset);
			View.Prefix = GetStringParamOptional(Params, TEXT("prefix")).ToLower();
			View.Query = GetStringParamOptional(Params, TEXT("query")).ToLower();
			View.bIncludeSchemas = GetBoolParam(Params, TEXT("include_schemas"), true);
			View.bNamesOnly = GetBoolParam(Params, TEXT("names_only"), false);
			View.Cursor = GetCursorParam(Params);
			View.Limit = GetIntParam(Params, TEXT("limit"), 0);
			if (View.Limit <= 0)
			{
				View.Limit = GetIntParam(Params, TEXT("page_size"), 0);
			}
			if (View.Limit < 0)
			{
				View.Limit = 0;
			}

			TArray<TSharedPtr<FJsonValue>> Filtered;
			for (const TSharedPtr<FJsonValue>& ToolValue : AllTools)
			{
				const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				if (!ToolObj.IsValid())
				{
					continue;
				}

				FString ToolName;
				ToolObj->TryGetStringField(TEXT("name"), ToolName);
				FString Description;
				ToolObj->TryGetStringField(TEXT("description"), Description);
				const FString ToolNameLower = ToolName.ToLower();
				if (!View.Toolset.IsEmpty() && InferToolsetName(ToolName) != View.Toolset)
				{
					continue;
				}
				if (!View.Prefix.IsEmpty() && !ToolNameLower.StartsWith(View.Prefix))
				{
					continue;
				}
				if (!View.Query.IsEmpty()
					&& !ToolName.Contains(View.Query, ESearchCase::IgnoreCase)
					&& !Description.Contains(View.Query, ESearchCase::IgnoreCase))
				{
					continue;
				}

				if (!View.bIncludeSchemas || View.bNamesOnly)
				{
					ToolObj->RemoveField(TEXT("inputSchema"));
					ToolObj->RemoveField(TEXT("outputSchema"));
				}
				if (View.bNamesOnly)
				{
					ToolObj->RemoveField(TEXT("annotations"));
				}

				Filtered.Add(ToolValue);
			}

			View.Total = Filtered.Num();
			const int32 Start = FMath::Clamp(View.Cursor, 0, View.Total);
			const int32 Take = View.Limit > 0 ? View.Limit : (View.Total - Start);
			const int32 End = FMath::Clamp(Start + Take, Start, View.Total);
			for (int32 Index = Start; Index < End; ++Index)
			{
				View.Tools.Add(Filtered[Index]);
			}
			if (End < View.Total)
			{
				View.NextCursor = End;
			}
			return View;
		}

		void AddToolListMetadata(const TSharedRef<FJsonObject>& Result, const FToolListView& View)
		{
			Result->SetNumberField(TEXT("total"), View.Total);
			Result->SetNumberField(TEXT("returned"), View.Tools.Num());
			Result->SetNumberField(TEXT("cursor"), View.Cursor);
			Result->SetNumberField(TEXT("limit"), View.Limit);
			if (View.NextCursor >= 0)
			{
				Result->SetStringField(TEXT("nextCursor"), FString::FromInt(View.NextCursor));
			}
			if (!View.Toolset.IsEmpty())
			{
				Result->SetStringField(TEXT("toolset"), View.Toolset);
			}
			if (!View.Prefix.IsEmpty())
			{
				Result->SetStringField(TEXT("prefix"), View.Prefix);
			}
			if (!View.Query.IsEmpty())
			{
				Result->SetStringField(TEXT("query"), View.Query);
			}
			Result->SetBoolField(TEXT("include_schemas"), View.bIncludeSchemas);
			Result->SetBoolField(TEXT("names_only"), View.bNamesOnly);
		}

		TAutoConsoleVariable<FString> CVarSamplingEndpoint(
			TEXT("somolmcp.sampling.endpoint"),
			TEXT(""),
			TEXT("Optional HTTP endpoint for sampling/createMessage backend."),
			ECVF_Default);

		TAutoConsoleVariable<FString> CVarSamplingAuthHeader(
			TEXT("somolmcp.sampling.auth_header"),
			TEXT(""),
			TEXT("Optional Authorization header value for sampling backend, e.g. 'Bearer <token>'."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarSamplingTimeoutMs(
			TEXT("somolmcp.sampling.timeout_ms"),
			30000,
			TEXT("HTTP timeout for sampling backend in milliseconds."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarStrictBinding(
			TEXT("somolmcp.auth.strict_binding"),
			0,
			TEXT("Reject JSON-RPC requests whose optional _project_path or _instance_uuid does not match this editor. 0=off, 1=reject mismatches when fields are present."),
			ECVF_Default);

		TAutoConsoleVariable<int32> CVarQueueOnlyExecution(
			TEXT("somolmcp.execution.queue_only"),
			1,
			TEXT("Route registered domain tools invoked through tools/call into the MCP Job queue. Queue-control/status tools remain direct so clients can observe and cancel work. 0=legacy direct execution, 1=queue-only domain execution."),
			ECVF_Default);

		bool IsQueueControlToolName(const FString& ToolName)
		{
			const FString Normalized = ToolName.TrimStartAndEnd().ToLower();
			return Normalized.StartsWith(TEXT("job_"))
				|| Normalized == TEXT("mcp_status")
				|| Normalized == TEXT("plugin_status")
				|| Normalized == TEXT("mcp_capabilities_get")
				|| Normalized == TEXT("mcp_tool_execution_profile")
				|| Normalized == TEXT("mcp_parallel_authoring_plan");
		}

		bool TryGetBindingString(const TSharedRef<FJsonObject>& Request, const FString& FieldName, FString& OutValue)
		{
			if (Request->TryGetStringField(FieldName, OutValue))
			{
				OutValue = OutValue.TrimStartAndEnd();
				return !OutValue.IsEmpty();
			}

			const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
			if (Request->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr && ParamsPtr->IsValid())
			{
				if ((*ParamsPtr)->TryGetStringField(FieldName, OutValue))
				{
					OutValue = OutValue.TrimStartAndEnd();
					return !OutValue.IsEmpty();
				}
			}

			return false;
		}

		FString NormalizeBindingPath(FString Path)
		{
			Path = Path.TrimStartAndEnd();
			if (Path.IsEmpty())
			{
				return Path;
			}

			Path = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeDirectoryName(Path);
			while (Path.EndsWith(TEXT("/")) || Path.EndsWith(TEXT("\\")))
			{
				Path.LeftChopInline(1, SOMOLMCP_NO_SHRINK);
			}
			return Path;
		}

		FString NormalizeRootOrFilePath(FString Path)
		{
			Path = Path.TrimStartAndEnd();
			if (Path.StartsWith(TEXT("file:///"), ESearchCase::IgnoreCase))
			{
				Path = Path.RightChop(8);
			}
			else if (Path.StartsWith(TEXT("file://"), ESearchCase::IgnoreCase))
			{
				Path = Path.RightChop(7);
			}
			Path.ReplaceInline(TEXT("%20"), TEXT(" "), ESearchCase::IgnoreCase);
			Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			if (Path.IsEmpty())
			{
				return Path;
			}
			Path = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeDirectoryName(Path);
			while (Path.EndsWith(TEXT("/")) || Path.EndsWith(TEXT("\\")))
			{
				Path.LeftChopInline(1, SOMOLMCP_NO_SHRINK);
			}
			return Path;
		}

		bool ResolveCandidateToFilesystemPath(FString RawValue, FString& OutPath)
		{
			FString Value = RawValue.TrimStartAndEnd();
			if (Value.IsEmpty())
			{
				return false;
			}
			Value.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			if (Value.StartsWith(TEXT("Game/")) || Value.StartsWith(TEXT("Engine/")))
			{
				Value = TEXT("/") + Value;
			}

			if (Value.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase))
			{
				FString Rest = Value.RightChop(5);
				Rest.RemoveFromStart(TEXT("/"));
				int32 DotIndex = INDEX_NONE;
				if (Rest.FindChar(TEXT('.'), DotIndex))
				{
					Rest = Rest.Left(DotIndex);
				}
				OutPath = NormalizeRootOrFilePath(FPaths::Combine(FPaths::ProjectContentDir(), Rest));
				return !OutPath.IsEmpty();
			}

			if (Value.StartsWith(TEXT("/Engine"), ESearchCase::IgnoreCase))
			{
				FString Rest = Value.RightChop(7);
				Rest.RemoveFromStart(TEXT("/"));
				int32 DotIndex = INDEX_NONE;
				if (Rest.FindChar(TEXT('.'), DotIndex))
				{
					Rest = Rest.Left(DotIndex);
				}
				OutPath = NormalizeRootOrFilePath(FPaths::Combine(FPaths::EngineContentDir(), Rest));
				return !OutPath.IsEmpty();
			}

			const bool bFileUri = Value.StartsWith(TEXT("file://"), ESearchCase::IgnoreCase);
			const bool bAbsolute = Value.Contains(TEXT(":/")) || Value.StartsWith(TEXT("//"));
			if (!bFileUri && !bAbsolute)
			{
				return false;
			}
			OutPath = NormalizeRootOrFilePath(Value);
			return !OutPath.IsEmpty();
		}

		bool RouterToolNameLooksMutating(const FString& ToolName)
		{
			const FString Lower = ToolName.ToLower();
			static const TArray<FString> MutationTokens = {
				TEXT("add"), TEXT("apply"), TEXT("assign"), TEXT("attach"), TEXT("bind"),
				TEXT("build"), TEXT("compile"), TEXT("connect"), TEXT("create"), TEXT("delete"),
				TEXT("destroy"), TEXT("disable"), TEXT("disconnect"), TEXT("duplicate"), TEXT("enable"),
				TEXT("fill"), TEXT("generate"), TEXT("import"), TEXT("move"), TEXT("paint"),
				TEXT("place"), TEXT("remove"), TEXT("rename"), TEXT("repair"), TEXT("reset"),
				TEXT("resize"), TEXT("restore"), TEXT("save"), TEXT("set"), TEXT("spawn"),
				TEXT("start"), TEXT("stop"), TEXT("sync"), TEXT("update"), TEXT("write")
			};
			for (const FString& Token : MutationTokens)
			{
				if (Lower.StartsWith(Token + TEXT("_"))
					|| Lower.Contains(TEXT("_") + Token + TEXT("_"))
					|| Lower.EndsWith(TEXT("_") + Token))
				{
					return true;
				}
			}
			return false;
		}

		template <typename KeyType>
		bool RootKeyMayContainPath(const KeyType& Key)
		{
			const FString Lower = FString(*Key).ToLower();
			return Lower.Contains(TEXT("path"))
				|| Lower.Contains(TEXT("asset"))
				|| Lower.Contains(TEXT("package"))
				|| Lower.Contains(TEXT("file"))
				|| Lower.Contains(TEXT("folder"))
				|| Lower.Contains(TEXT("dir"))
				|| Lower.Contains(TEXT("root"))
				|| Lower.Contains(TEXT("source"))
				|| Lower.Contains(TEXT("destination"))
				|| Lower.Contains(TEXT("output"))
				|| Lower.Contains(TEXT("import"))
				|| Lower.Contains(TEXT("export"));
		}

		void ExtractRootCandidatePaths(const TSharedRef<FJsonObject>& Obj, TArray<FString>& OutPaths)
		{
			for (const auto& Pair : Obj->Values)
			{
				if (!Pair.Value.IsValid())
				{
					continue;
				}
				const bool bPathKey = RootKeyMayContainPath(Pair.Key);
				if (Pair.Value->Type == EJson::String)
				{
					FString Resolved;
					if (bPathKey && ResolveCandidateToFilesystemPath(Pair.Value->AsString(), Resolved))
					{
						OutPaths.AddUnique(Resolved);
					}
				}
				else if (Pair.Value->Type == EJson::Array)
				{
					for (const TSharedPtr<FJsonValue>& Item : Pair.Value->AsArray())
					{
						if (!Item.IsValid())
						{
							continue;
						}
						if (Item->Type == EJson::String)
						{
							FString Resolved;
							if (bPathKey && ResolveCandidateToFilesystemPath(Item->AsString(), Resolved))
							{
								OutPaths.AddUnique(Resolved);
							}
						}
						else if (Item->Type == EJson::Object)
						{
							const TSharedPtr<FJsonObject> Nested = Item->AsObject();
							if (Nested.IsValid())
							{
								ExtractRootCandidatePaths(Nested.ToSharedRef(), OutPaths);
							}
						}
					}
				}
				else if (Pair.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> Nested = Pair.Value->AsObject();
					if (Nested.IsValid())
					{
						ExtractRootCandidatePaths(Nested.ToSharedRef(), OutPaths);
					}
				}
			}
		}

		bool PathIsUnderAnyRoot(const FString& Path, const TArray<FString>& Roots)
		{
			const FString NormalizedPath = NormalizeRootOrFilePath(Path);
			for (const FString& Root : Roots)
			{
				const FString NormalizedRoot = NormalizeRootOrFilePath(Root);
				if (NormalizedRoot.IsEmpty())
				{
					continue;
				}
				if (NormalizedPath.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
					|| NormalizedPath.StartsWith(NormalizedRoot + TEXT("/"), ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}

		bool IsStrictBindingSatisfied(
			const TSharedRef<FJsonObject>& Request,
			const TFunction<FSololmcpRouter::FInstanceInfo()>& InstanceInfoGetter,
			FString& OutError)
		{
			if (CVarStrictBinding.GetValueOnAnyThread() == 0)
			{
				return true;
			}

			FString ExpectedProjectPath;
			if (TryGetBindingString(Request, TEXT("_project_path"), ExpectedProjectPath))
			{
				const FString ExpectedNormalized = NormalizeBindingPath(ExpectedProjectPath);
				const FString CurrentDirNormalized = NormalizeBindingPath(FPaths::ProjectDir());
				const FString CurrentFileNormalized = NormalizeBindingPath(FPaths::GetProjectFilePath());
				if (!ExpectedNormalized.Equals(CurrentDirNormalized, ESearchCase::IgnoreCase)
					&& !ExpectedNormalized.Equals(CurrentFileNormalized, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(
						TEXT("Strict binding rejected request: _project_path '%s' does not match current editor project '%s' or '%s'."),
						*ExpectedNormalized,
						*CurrentDirNormalized,
						*CurrentFileNormalized);
					return false;
				}
			}

			FString ExpectedInstanceUuid;
			if (TryGetBindingString(Request, TEXT("_instance_uuid"), ExpectedInstanceUuid))
			{
				if (!InstanceInfoGetter)
				{
					OutError = TEXT("Strict binding rejected request: _instance_uuid was supplied but this editor has no instance uuid.");
					return false;
				}

				const FSololmcpRouter::FInstanceInfo Info = InstanceInfoGetter();
				const FString CurrentInstanceUuid = Info.InstanceUuid.TrimStartAndEnd();
				if (CurrentInstanceUuid.IsEmpty() || !ExpectedInstanceUuid.Equals(CurrentInstanceUuid, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(
						TEXT("Strict binding rejected request: _instance_uuid '%s' does not match current editor instance '%s'."),
						*ExpectedInstanceUuid,
						CurrentInstanceUuid.IsEmpty() ? TEXT("(empty)") : *CurrentInstanceUuid);
					return false;
				}
			}

			return true;
		}

		bool MakeSamplingBackendCall(
			const TSharedRef<FJsonObject>& RequestPayload,
			TSharedRef<FJsonObject>& OutResult,
			FString& OutError)
		{
			const FString Endpoint = CVarSamplingEndpoint.GetValueOnAnyThread().TrimStartAndEnd();
			if (Endpoint.IsEmpty())
			{
				OutError = TEXT("sampling backend not configured. Set somolmcp.sampling.endpoint.");
				return false;
			}

			FString Body;
			{
				const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
				if (!FJsonSerializer::Serialize(RequestPayload, Writer))
				{
					OutError = TEXT("failed to serialize sampling backend request");
					return false;
				}
			}

			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpReq = FHttpModule::Get().CreateRequest();
			HttpReq->SetURL(Endpoint);
			HttpReq->SetVerb(TEXT("POST"));
			HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			const FString AuthHeader = CVarSamplingAuthHeader.GetValueOnAnyThread().TrimStartAndEnd();
			if (!AuthHeader.IsEmpty())
			{
				HttpReq->SetHeader(TEXT("Authorization"), AuthHeader);
			}
			HttpReq->SetContentAsString(Body);

			struct FHttpCallState
			{
				FCriticalSection Mutex;
				bool bDone = false;
				bool bOk = false;
				int32 StatusCode = 0;
				FString ResponseText;
				FString ErrorText;
			};

			FHttpCallState State;
			HttpReq->OnProcessRequestComplete().BindLambda(
				[&State](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
				{
					FScopeLock Lock(&State.Mutex);
					State.bDone = true;
					if (!bConnected || !Resp.IsValid())
					{
						State.bOk = false;
						State.ErrorText = TEXT("sampling backend connection failed");
						return;
					}
					State.StatusCode = Resp->GetResponseCode();
					State.ResponseText = Resp->GetContentAsString();
					State.bOk = EHttpResponseCodes::IsOk(State.StatusCode);
					if (!State.bOk)
					{
						State.ErrorText = FString::Printf(TEXT("sampling backend HTTP %d"), State.StatusCode);
					}
				});

			if (!HttpReq->ProcessRequest())
			{
				OutError = TEXT("failed to start sampling backend request");
				return false;
			}

			// FIXED #4: 用 HttpManager.Tick() 驱动 HTTP 层，避免纯 sleep 阻塞编辑器帧循环。
			// 注意：此函数应仅在 Job 执行上下文（非游戏线程帧循环内）调用。
			const int32 TimeoutMs = FMath::Max(1000, CVarSamplingTimeoutMs.GetValueOnAnyThread());
			const double Start = FPlatformTime::Seconds();
			while (true)
			{
				{
					FScopeLock Lock(&State.Mutex);
					if (State.bDone)
					{
						break;
					}
				}
				const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
				if (ElapsedMs > TimeoutMs)
				{
					HttpReq->CancelRequest();
					OutError = TEXT("sampling backend timeout");
					return false;
				}
				// 驱动 HTTP Manager 处理 I/O 事件，而非纯 sleep
				FHttpModule::Get().GetHttpManager().Tick(0.016f);
				FPlatformProcess::Sleep(0.016f);
			}

			{
				FScopeLock Lock(&State.Mutex);
				if (!State.bOk)
				{
					OutError = State.ErrorText.IsEmpty() ? TEXT("sampling backend request failed") : State.ErrorText;
					return false;
				}

				const TSharedPtr<FJsonObject> Parsed = ParseJsonObject(State.ResponseText);
				if (!Parsed.IsValid())
				{
					OutError = TEXT("sampling backend returned non-JSON body");
					return false;
				}

				// Format A: already MCP sampling result.
				const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
				if (Parsed->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray)
				{
					OutResult = Parsed.ToSharedRef();
					if (!OutResult->HasField(TEXT("model")))
					{
						OutResult->SetStringField(TEXT("model"), TEXT("external-sampling-backend"));
					}
					if (!OutResult->HasField(TEXT("stopReason")))
					{
						OutResult->SetStringField(TEXT("stopReason"), TEXT("endTurn"));
					}
					return true;
				}

				// Format B: OpenAI-style chat completion.
				const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
				if (Parsed->TryGetArrayField(TEXT("choices"), Choices) && Choices && Choices->Num() > 0)
				{
					const TSharedPtr<FJsonObject> ChoiceObj = (*Choices)[0].IsValid() ? (*Choices)[0]->AsObject() : nullptr;
					const TSharedPtr<FJsonObject>* MsgPtr = nullptr;
					const bool bHasMsg = ChoiceObj.IsValid() && ChoiceObj->TryGetObjectField(TEXT("message"), MsgPtr) && MsgPtr && MsgPtr->IsValid();
					const FString Text = bHasMsg ? (*MsgPtr)->GetStringField(TEXT("content")) : FString();
					if (!Text.IsEmpty())
					{
						OutResult = MakeShared<FJsonObject>();
						FString OutModel;
						if (!Parsed->TryGetStringField(TEXT("model"), OutModel))
						{
							OutModel = TEXT("external-sampling-backend");
						}
						OutResult->SetStringField(TEXT("model"), OutModel);
						OutResult->SetStringField(TEXT("stopReason"), TEXT("endTurn"));
						TArray<TSharedPtr<FJsonValue>> Content;
						TSharedRef<FJsonObject> TextObj = MakeShared<FJsonObject>();
						TextObj->SetStringField(TEXT("type"), TEXT("text"));
						TextObj->SetStringField(TEXT("text"), Text);
						Content.Add(MakeShared<FJsonValueObject>(TextObj));
						OutResult->SetArrayField(TEXT("content"), Content);
						return true;
					}
				}
			}

			OutError = TEXT("sampling backend response format not supported");
			return false;
		}
	}

FSololmcpRouter::FSololmcpRouter(FSololmcpToolRegistry& InRegistry)
	: Registry(InRegistry)
{
}

void FSololmcpRouter::SetTransportStatsGetter(TFunction<FSololmcpTcpTransport::FTransportStats()> InGetter)
{
	TransportStatsGetter = MoveTemp(InGetter);
}

void FSololmcpRouter::SetInstanceInfoGetter(TFunction<FInstanceInfo()> InGetter)
{
	InstanceInfoGetter = MoveTemp(InGetter);
}

	void FSololmcpRouter::SetNotificationSender(TFunction<void(FSololmcpTcpTransport::FConnectionId, const FString&)> InSender)
	{
		NotificationSender = MoveTemp(InSender);
	}

	void FSololmcpRouter::RemoveSession(const FSololmcpTcpTransport::FConnectionId ConnectionId)
	{
		SessionsByConnection.Remove(ConnectionId);
		for (auto It = PendingServerRequests.CreateIterator(); It; ++It)
		{
			if (It.Value().ConnectionId == ConnectionId)
			{
				It.RemoveCurrent();
			}
		}
	}

	FString FSololmcpRouter::HandleMessage(const FString& JsonMessage)
	{
		return HandleMessage(0, JsonMessage);
	}

	FString FSololmcpRouter::HandleMessage(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& JsonMessage)
	{
		// FIXED #6: 去掉此处多余的 TickJobs 调用；Job 驱动统一由 SololmcpServer 的
		// FTSTicker（或外部 Tick）负责，避免 Router 入口 + AwaitJob 双重 TickJobs。

		const TSharedPtr<FJsonObject> Request = ParseJsonObject(JsonMessage);
		if (!Request.IsValid())
		{
			return ReplyError(nullptr, -32700, TEXT("Parse error"));
		}

		FString Method;
		if (!Request->TryGetStringField(TEXT("method"), Method) || Method.IsEmpty())
		{
			if (Request->HasField(TEXT("id")) && (Request->HasField(TEXT("result")) || Request->HasField(TEXT("error"))))
			{
				return HandleServerRequestResponse(ConnectionId, Request.ToSharedRef());
			}
			return ReplyError(Request->TryGetField(TEXT("id")), -32600, TEXT("Invalid request: missing method"));
		}

		const TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
		FString BindingError;
		if (!IsStrictBindingSatisfied(Request.ToSharedRef(), InstanceInfoGetter, BindingError))
		{
			UE_LOG(LogSOMOLMCP, Warning, TEXT("%s"), *BindingError);
			return ReplyError(Id, -32029, BindingError);
		}

		if (Method == TEXT("initialize"))
		{
			return HandleInitialize(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("ping"))
		{
			TSharedRef<FJsonObject> Pong = MakeShared<FJsonObject>();
			return ReplyResult(Id, Pong);
		}
		if (Method == TEXT("tools/list"))
		{
			return HandleToolsList(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("tools/call"))
		{
			return HandleToolsCall(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("toolsets/list"))
		{
			return HandleToolsetsList(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("toolsets/describe"))
		{
			return HandleToolsetsDescribe(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("toolsets/load"))
		{
			return HandleToolsetsLoad(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("resources/list"))
		{
			return HandleResourcesList(ConnectionId, Id);
		}
		if (Method == TEXT("resources/read"))
		{
			return HandleResourcesRead(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("resources/subscribe"))
		{
			return HandleResourcesSubscribe(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("resources/unsubscribe"))
		{
			return HandleResourcesUnsubscribe(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("roots/list"))
		{
			return HandleRootsList(ConnectionId, Id);
		}
		if (Method == TEXT("prompts/list"))
		{
			return HandlePromptsList(Id);
		}
		if (Method == TEXT("prompts/get"))
		{
			return HandlePromptsGet(Id, Request.ToSharedRef());
		}
		if (Method == TEXT("sampling/createMessage"))
		{
			return HandleSamplingCreateMessage(Id, Request.ToSharedRef());
		}
		if (Method == TEXT("server/elicitation/create"))
		{
			return HandleServerElicitationCreate(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("server/requests/status"))
		{
			return HandleServerRequestsStatus(Id);
		}
		if (Method == TEXT("completions/complete"))
		{
			return HandleCompletionsComplete(Id, Request.ToSharedRef());
		}
		if (Method == TEXT("jobs/submit"))
		{
			return HandleJobsSubmit(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("jobs/elicit"))
		{
			return HandleJobsElicit(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("jobs/resume"))
		{
			return HandleJobsResume(ConnectionId, Id, Request.ToSharedRef());
		}
		if (Method == TEXT("jobs/get"))
		{
			return HandleJobsGet(Id, Request.ToSharedRef());
		}
		// FIX-2 (job queue conn design 20260804): 外部执行器租约续期。
		if (Method == TEXT("jobs/heartbeat"))
		{
			return HandleJobsHeartbeat(Id, Request.ToSharedRef());
		}
		if (Method == TEXT("jobs/await"))
		{
			return HandleJobsAwait(Id, Request.ToSharedRef());
		}
		if (Method == TEXT("jobs/cancel"))
		{
			return HandleJobsCancel(Id, Request.ToSharedRef());
		}
		if (Method == TEXT("notifications/cancelled"))
		{
			TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>();
			FString Error;
			FString JobId;
			const TSharedPtr<FJsonObject> Params = GetParamsObject(Request.ToSharedRef());
			if (Params.IsValid())
			{
				Params->TryGetStringField(TEXT("job_id"), JobId);
			}
			if (!JobId.IsEmpty())
			{
				FSololmcpJobService::CancelJob(JobId, Ignored, Error);
			}
			return FString();
		}
		if (Method == TEXT("notifications/roots/list_changed"))
		{
			if (FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
			{
				Session->RootUris.Reset();
			}
			return FString();
		}
		if (Method == TEXT("jobs/events"))
		{
			return HandleJobsEvents(Id, Request.ToSharedRef());
		}
		if (Method == TEXT("server/stats"))
		{
			return HandleServerStats(Id);
		}

		return ReplyError(Id, -32601, FString::Printf(TEXT("Method not found: %s"), *Method));
	}

	FString FSololmcpRouter::ReplyResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& ResultObj) const
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		if (Id.IsValid())
		{
			Response->SetField(TEXT("id"), Id);
		}
		else
		{
			Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
		}
		Response->SetObjectField(TEXT("result"), ResultObj);
		return ToJsonString(Response);
	}

	FString FSololmcpRouter::ReplyError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<FJsonObject>& Data) const
	{
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		if (Id.IsValid())
		{
			Response->SetField(TEXT("id"), Id);
		}
		else
		{
			Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
		}

		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		if (Data.IsValid())
		{
			Error->SetObjectField(TEXT("data"), Data.ToSharedRef());
		}
		Response->SetObjectField(TEXT("error"), Error);
		return ToJsonString(Response);
	}

	void FSololmcpRouter::AppendSessionDataDrivenTools(FSololmcpTcpTransport::FConnectionId ConnectionId, TArray<TSharedPtr<FJsonValue>>& InOutTools) const
	{
		const FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (!Session)
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonObject>>& ManifestPair : Session->LoadedDataToolManifests)
		{
			const TSharedPtr<FJsonObject>& Manifest = ManifestPair.Value;
			const TArray<TSharedPtr<FJsonValue>>* DeclaredTools = nullptr;
			if (!Manifest.IsValid() || !Manifest->TryGetArrayField(TEXT("tools"), DeclaredTools) || !DeclaredTools)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& ToolValue : *DeclaredTools)
			{
				const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				if (!ToolObj.IsValid())
				{
					continue;
				}

				FString ToolName;
				ToolObj->TryGetStringField(TEXT("name"), ToolName);
				ToolName = ToolName.TrimStartAndEnd();
				if (ToolName.IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject> ToolCopy = MakeShared<FJsonObject>();
				FJsonObject::Duplicate(ToolObj, ToolCopy);
				ToolCopy->SetStringField(TEXT("name"), ToolName);
				if (!ToolCopy->HasField(TEXT("description")))
				{
					ToolCopy->SetStringField(TEXT("description"), FString::Printf(TEXT("Session data-driven macro tool from manifest '%s'."), *ManifestPair.Key));
				}
				if (!ToolCopy->HasTypedField<EJson::Object>(TEXT("inputSchema")))
				{
					TSharedRef<FJsonObject> InputSchema = MakeShared<FJsonObject>();
					InputSchema->SetStringField(TEXT("type"), TEXT("object"));
					ToolCopy->SetObjectField(TEXT("inputSchema"), InputSchema);
				}

				TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
				Annotations->SetBoolField(TEXT("dataDriven"), true);
				Annotations->SetBoolField(TEXT("sessionScoped"), true);
				Annotations->SetStringField(TEXT("manifest"), ManifestPair.Key);
				Annotations->SetBoolField(TEXT("runtimeRegistryMutation"), false);
				Annotations->SetStringField(TEXT("executor"), TEXT("trusted_macro_steps"));
				ToolCopy->SetObjectField(TEXT("annotations"), Annotations);
				InOutTools.Add(MakeShared<FJsonValueObject>(ToolCopy.ToSharedRef()));
			}
		}
	}

	bool FSololmcpRouter::TryExecuteSessionDataDrivenTool(
		FSololmcpTcpTransport::FConnectionId ConnectionId,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Arguments,
		bool& bOutHandled,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError) const
	{
		bOutHandled = false;
		const FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (!Session)
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonObject>>& ManifestPair : Session->LoadedDataToolManifests)
		{
			const TSharedPtr<FJsonObject>& Manifest = ManifestPair.Value;
			const TArray<TSharedPtr<FJsonValue>>* DeclaredTools = nullptr;
			if (!Manifest.IsValid() || !Manifest->TryGetArrayField(TEXT("tools"), DeclaredTools) || !DeclaredTools)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& ToolValue : *DeclaredTools)
			{
				const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				FString CandidateName;
				if (!ToolObj.IsValid() || !ToolObj->TryGetStringField(TEXT("name"), CandidateName) || CandidateName.TrimStartAndEnd() != ToolName)
				{
					continue;
				}

				bOutHandled = true;
				const TSharedPtr<FJsonObject>* ExecutionPtr = nullptr;
				if (!ToolObj->TryGetObjectField(TEXT("execution"), ExecutionPtr) || !ExecutionPtr || !ExecutionPtr->IsValid())
				{
					OutError = TEXT("Data-driven tool has no execution binding. Add execution.kind='macro_steps' with a bounded steps array.");
					return false;
				}

				FString Kind;
				(*ExecutionPtr)->TryGetStringField(TEXT("kind"), Kind);
				if (Kind.IsEmpty())
				{
					(*ExecutionPtr)->TryGetStringField(TEXT("type"), Kind);
				}
				Kind = NormalizeToolsetName(Kind);
				if (Kind != TEXT("macro_steps") && Kind != TEXT("tool_macro"))
				{
					OutError = FString::Printf(TEXT("Unsupported data-driven execution kind: %s"), *Kind);
					return false;
				}

				const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
				if (!(*ExecutionPtr)->TryGetArrayField(TEXT("steps"), Steps) || !Steps || Steps->Num() <= 0)
				{
					OutError = TEXT("Data-driven macro execution requires at least one step.");
					return false;
				}
				if (Steps->Num() > 32)
				{
					OutError = TEXT("Data-driven macro execution is capped at 32 steps.");
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> StepResults;
				for (int32 StepIndex = 0; StepIndex < Steps->Num(); ++StepIndex)
				{
					const TSharedPtr<FJsonObject> StepObj = (*Steps)[StepIndex].IsValid() ? (*Steps)[StepIndex]->AsObject() : nullptr;
					if (!StepObj.IsValid())
					{
						OutError = FString::Printf(TEXT("Data-driven macro step %d must be an object."), StepIndex);
						return false;
					}

					FString StepTool;
					StepObj->TryGetStringField(TEXT("tool"), StepTool);
					StepTool = StepTool.TrimStartAndEnd();
					if (StepTool.IsEmpty())
					{
						OutError = FString::Printf(TEXT("Data-driven macro step %d is missing tool."), StepIndex);
						return false;
					}
					if (StepTool == ToolName)
					{
						OutError = TEXT("Data-driven macro recursion is not allowed.");
						return false;
					}

					TSharedRef<FJsonObject> StepArgs = MakeShared<FJsonObject>();
					const TSharedPtr<FJsonObject>* StepArgsPtr = nullptr;
					if (StepObj->TryGetObjectField(TEXT("arguments"), StepArgsPtr) && StepArgsPtr && StepArgsPtr->IsValid())
					{
						FJsonObject::Duplicate(*StepArgsPtr, StepArgs);
					}
					if (GetBoolParam(StepObj, TEXT("pass_through_arguments"), false) || GetBoolParam(*ExecutionPtr, TEXT("pass_through_arguments"), false))
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& ArgPair : Arguments->Values)
						{
							if (!StepArgs->HasField(ArgPair.Key))
							{
								StepArgs->SetField(ArgPair.Key, ArgPair.Value);
							}
						}
					}
					const TSharedPtr<FJsonObject>* BindingsPtr = nullptr;
					if (StepObj->TryGetObjectField(TEXT("argument_bindings"), BindingsPtr) && BindingsPtr && BindingsPtr->IsValid())
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Binding : (*BindingsPtr)->Values)
						{
							const FString TargetField = Binding.Key;
							const FString SourceField = Binding.Value.IsValid() ? Binding.Value->AsString() : FString();
							if (!TargetField.IsEmpty() && !SourceField.IsEmpty())
							{
								const TSharedPtr<FJsonValue> SourceValue = Arguments->TryGetField(SourceField);
								if (SourceValue.IsValid())
								{
									StepArgs->SetField(TargetField, SourceValue);
								}
							}
						}
					}

					FString RootsError;
					if (!ValidateToolsCallRoots(ConnectionId, StepTool, StepArgs, RootsError))
					{
						OutError = RootsError;
						return false;
					}

					TSharedRef<FJsonObject> StepStructured = MakeShared<FJsonObject>();
					FString StepSummary;
					FString StepError;
					// Foliage serialization: macro steps may target foliage_* tools while
					// the job queue dispatches another foliage tool on a worker thread.
					LockFoliageActorMutex();
					const bool bStepSuccess = Registry.ExecuteTool(StepTool, StepArgs, StepStructured, StepSummary, StepError);
					UnlockFoliageActorMutex();
					TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
					StepResult->SetNumberField(TEXT("index"), StepIndex);
					StepResult->SetStringField(TEXT("tool"), StepTool);
					StepResult->SetBoolField(TEXT("success"), bStepSuccess);
					StepResult->SetStringField(TEXT("summary"), bStepSuccess ? StepSummary : StepError);
					StepResult->SetObjectField(TEXT("structured"), StepStructured);
					StepResults.Add(MakeShared<FJsonValueObject>(StepResult));
					if (!bStepSuccess)
					{
						OutStructured->SetStringField(TEXT("schema"), TEXT("somolmcp.data_driven_macro.result:v1"));
						OutStructured->SetStringField(TEXT("macro_tool"), ToolName);
						OutStructured->SetStringField(TEXT("manifest"), ManifestPair.Key);
						OutStructured->SetArrayField(TEXT("steps"), StepResults);
						OutStructured->SetBoolField(TEXT("success"), false);
						OutError = FString::Printf(TEXT("Data-driven macro step %d failed: %s"), StepIndex, *StepError);
						return false;
					}
				}

				OutStructured->SetStringField(TEXT("schema"), TEXT("somolmcp.data_driven_macro.result:v1"));
				OutStructured->SetStringField(TEXT("macro_tool"), ToolName);
				OutStructured->SetStringField(TEXT("manifest"), ManifestPair.Key);
				OutStructured->SetNumberField(TEXT("step_count"), Steps->Num());
				OutStructured->SetArrayField(TEXT("steps"), StepResults);
				OutStructured->SetBoolField(TEXT("success"), true);
				OutSummary = FString::Printf(TEXT("Data-driven macro tool executed: %s (%d step%s)."), *ToolName, Steps->Num(), Steps->Num() == 1 ? TEXT("") : TEXT("s"));
				return true;
			}
		}

		return false;
	}

	void FSololmcpRouter::StoreSessionFromInitialize(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedRef<FJsonObject>& Request)
	{
		FMcpSession Session;
		if (FMcpSession* Existing = SessionsByConnection.Find(ConnectionId))
		{
			Session = *Existing;
		}
		Session.ConnectionId = ConnectionId;
		if (Session.SessionId.IsEmpty())
		{
			Session.SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		}

		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("protocolVersion"), Session.ProtocolVersion);

			const TSharedPtr<FJsonObject>* ClientInfoPtr = nullptr;
			if (Params->TryGetObjectField(TEXT("clientInfo"), ClientInfoPtr) && ClientInfoPtr && ClientInfoPtr->IsValid())
			{
				(*ClientInfoPtr)->TryGetStringField(TEXT("name"), Session.ClientName);
				(*ClientInfoPtr)->TryGetStringField(TEXT("version"), Session.ClientVersion);
			}

			const TSharedPtr<FJsonObject>* CapabilitiesPtr = nullptr;
			if (Params->TryGetObjectField(TEXT("capabilities"), CapabilitiesPtr) && CapabilitiesPtr && CapabilitiesPtr->IsValid())
			{
				Session.bClientRoots = (*CapabilitiesPtr)->HasTypedField<EJson::Object>(TEXT("roots"));
				Session.bClientSampling = (*CapabilitiesPtr)->HasTypedField<EJson::Object>(TEXT("sampling"));
				Session.bClientElicitation = (*CapabilitiesPtr)->HasTypedField<EJson::Object>(TEXT("elicitation"));
			}

			Session.RootUris.Reset();
			auto CaptureRootsArray = [&Session](const TArray<TSharedPtr<FJsonValue>>* RootsArray)
			{
				if (!RootsArray)
				{
					return;
				}
				for (const TSharedPtr<FJsonValue>& Value : *RootsArray)
				{
					if (!Value.IsValid())
					{
						continue;
					}
					if (Value->Type == EJson::String)
					{
						Session.RootUris.AddUnique(Value->AsString());
						continue;
					}
					const TSharedPtr<FJsonObject> RootObj = Value->AsObject();
					if (!RootObj.IsValid())
					{
						continue;
					}
					FString Uri;
					if (RootObj->TryGetStringField(TEXT("uri"), Uri) && !Uri.IsEmpty())
					{
						Session.RootUris.AddUnique(Uri);
					}
				}
			};

			const TArray<TSharedPtr<FJsonValue>>* RootArray = nullptr;
			if (Params->TryGetArrayField(TEXT("roots"), RootArray))
			{
				CaptureRootsArray(RootArray);
			}
			const TSharedPtr<FJsonObject>* RootsObj = nullptr;
			if (Params->TryGetObjectField(TEXT("roots"), RootsObj) && RootsObj && RootsObj->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* NestedRoots = nullptr;
				if ((*RootsObj)->TryGetArrayField(TEXT("roots"), NestedRoots))
				{
					CaptureRootsArray(NestedRoots);
				}
			}
		}

		SessionsByConnection.Add(ConnectionId, MoveTemp(Session));
	}

	FString FSololmcpRouter::GetClientSummary() const
	{
		if (SessionsByConnection.Num() == 0)
		{
			return FString();
		}

		TArray<FString> Names;
		for (const TPair<FSololmcpTcpTransport::FConnectionId, FMcpSession>& Pair : SessionsByConnection)
		{
			FString Label = Pair.Value.ClientName.IsEmpty()
				? FString::Printf(TEXT("连接 %llu"), Pair.Key)
				: Pair.Value.ClientName;
			if (!Pair.Value.ClientVersion.IsEmpty())
			{
				Label += FString::Printf(TEXT(" %s"), *Pair.Value.ClientVersion);
			}
			Names.Add(Label);
		}
		Names.Sort();
		return FString::Join(Names, TEXT(", "));
	}

	bool FSololmcpRouter::ValidateToolsCallRoots(
		FSololmcpTcpTransport::FConnectionId ConnectionId,
		const FString& ToolName,
		const TSharedRef<FJsonObject>& Arguments,
		FString& OutError) const
	{
		if (!RouterToolNameLooksMutating(ToolName))
		{
			return true;
		}

		const FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (!Session || Session->RootUris.Num() == 0)
		{
			return true;
		}

		TArray<FString> CandidatePaths;
		ExtractRootCandidatePaths(Arguments, CandidatePaths);
		for (const FString& CandidatePath : CandidatePaths)
		{
			if (!PathIsUnderAnyRoot(CandidatePath, Session->RootUris))
			{
				OutError = FString::Printf(
					TEXT("MCP roots rejected tools/call '%s': path '%s' is outside client-declared roots."),
					*ToolName,
					*CandidatePath);
				return false;
			}
		}

		return true;
	}

	void FSololmcpRouter::EmitNotification(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& Method, const TSharedRef<FJsonObject>& Params) const
	{
		if (!NotificationSender || ConnectionId == 0 || Method.IsEmpty())
		{
			return;
		}
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Message->SetStringField(TEXT("method"), Method);
		Message->SetObjectField(TEXT("params"), Params);
		NotificationSender(ConnectionId, ToJsonString(Message));
	}

	void FSololmcpRouter::EmitResourceUpdated(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& Uri, const FString& Reason) const
	{
		const FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (!Session || !Session->SubscribedResourceUris.Contains(Uri))
		{
			return;
		}
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("uri"), Uri);
		Params->SetStringField(TEXT("reason"), Reason);
		Params->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
		EmitNotification(ConnectionId, TEXT("notifications/resources/updated"), Params);
	}

	bool FSololmcpRouter::SendServerRequest(
		FSololmcpTcpTransport::FConnectionId ConnectionId,
		const FString& Method,
		const TSharedRef<FJsonObject>& Params,
		FString& OutRequestId)
	{
		OutRequestId.Reset();
		if (!NotificationSender || ConnectionId == 0 || Method.IsEmpty())
		{
			return false;
		}

		OutRequestId = FString::Printf(TEXT("somolmcp-%lld"), ++NextServerRequestSeq);
		FPendingServerRequest Pending;
		Pending.RequestId = OutRequestId;
		Pending.Method = Method;
		Pending.ConnectionId = ConnectionId;
		Pending.CreatedUtc = FDateTime::UtcNow();
		PendingServerRequests.Add(OutRequestId, MoveTemp(Pending));

		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Message->SetStringField(TEXT("id"), OutRequestId);
		Message->SetStringField(TEXT("method"), Method);
		Message->SetObjectField(TEXT("params"), Params);
		NotificationSender(ConnectionId, ToJsonString(Message));
		return true;
	}

	FString FSololmcpRouter::HandleServerRequestResponse(
		FSololmcpTcpTransport::FConnectionId ConnectionId,
		const TSharedRef<FJsonObject>& Message)
	{
		const FString RequestId = JsonRpcIdToStableString(Message->TryGetField(TEXT("id")));
		if (RequestId.IsEmpty())
		{
			return FString();
		}

		FPendingServerRequest* Pending = PendingServerRequests.Find(RequestId);
		if (!Pending)
		{
			UE_LOG(LogSOMOLMCP, Warning, TEXT("Received response for unknown server request id '%s'."), *RequestId);
			return FString();
		}

		Pending->bAnswered = true;
		Pending->AnsweredUtc = FDateTime::UtcNow();
		Pending->bError = Message->HasField(TEXT("error"));
		Pending->ErrorMessage.Reset();
		if (Pending->bError)
		{
			const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
			if (Message->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && ErrorObj->IsValid())
			{
				(*ErrorObj)->TryGetStringField(TEXT("message"), Pending->ErrorMessage);
			}
		}
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Message, Copy);
		Pending->Response = Copy;
		EmitResourceUpdated(Pending->ConnectionId, TEXT("somolmcp://server/requests"), TEXT("server_request_answered"));
		if (Pending->ConnectionId != ConnectionId)
		{
			UE_LOG(LogSOMOLMCP, Warning, TEXT("Server request response id '%s' arrived on connection %llu, expected %llu."),
				*RequestId,
				static_cast<unsigned long long>(ConnectionId),
				static_cast<unsigned long long>(Pending->ConnectionId));
		}
		return FString();
	}

	FString FSololmcpRouter::HandleInitialize(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		StoreSessionFromInitialize(ConnectionId, Request);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString NegotiatedProtocolVersion = TEXT("2025-06-18");
		if (const FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
		{
			if (!Session->ProtocolVersion.IsEmpty()) NegotiatedProtocolVersion = Session->ProtocolVersion;
		}
		Result->SetStringField(TEXT("protocolVersion"), NegotiatedProtocolVersion);

		TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), TEXT("SOMOLMCP"));
		ServerInfo->SetStringField(TEXT("version"), GetProductVersion());
		ServerInfo->SetStringField(TEXT("transportProtocol"), TEXT("somolmcp.multi_transport.v1"));
		ServerInfo->SetArrayField(TEXT("transports"), {
			MakeShared<FJsonValueString>(TEXT("tcp.length_prefixed_jsonrpc")),
			MakeShared<FJsonValueString>(TEXT("mcp.streamable_http"))
		});
		ServerInfo->SetStringField(TEXT("longTaskModel"), TEXT("jobs/submit returns job_id immediately; jobs/get and jobs/events report running/progress/completed/failed/blocked client_status plus receipt_envelope."));
		ServerInfo->SetStringField(TEXT("executionMode"), TEXT("queue_only_named_domain_tools"));
		ServerInfo->SetStringField(TEXT("implementationMode"), TEXT("native_cpp_only"));
		ServerInfo->SetBoolField(TEXT("genericPythonExposed"), false);
		ServerInfo->SetBoolField(TEXT("pythonBackendEnabled"), false);
		// Multi-instance support: expose project identity so a client can connect to
		// several UE editors at once and label them by project. Both fields are
		// additive; clients that don't know these keys ignore them.
		ServerInfo->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		ServerInfo->SetStringField(TEXT("projectPath"),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));

		// v3.7.4 — multi-instance routing: carry the process-level identity the
		// client already has from the on-disk instance registry. Letting the
		// client verify `instance_uuid` after TCP-connect closes a subtle race:
		// port N can legally belong to a *different* UE process between the
		// time the registry was read and the TCP handshake completed (crash
		// recovery, rapid restart). If the uuid mismatches, the client reopens
		// the connection instead of silently routing commands to the wrong
		// editor.
		if (InstanceInfoGetter)
		{
			const FInstanceInfo Info = InstanceInfoGetter();
			ServerInfo->SetStringField(TEXT("instanceUuid"), Info.InstanceUuid);
			ServerInfo->SetNumberField(TEXT("pid"), static_cast<double>(Info.Pid));
			ServerInfo->SetNumberField(TEXT("actualPort"), Info.ActualPort);
		}
		// Engine version — clients sometimes need to branch on UE5.4 vs 5.5 tool
		// availability. Include both long and short forms.
		ServerInfo->SetStringField(TEXT("engineVersion"),
			FString::Printf(TEXT("%d.%d.%d"),
				ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION));

		// When this translation unit was compiled.
		//
		// A green build says nothing about what the editor is actually running:
		// building and deploying are separate steps here, and an editor can serve a
		// binary that is hours older than the source someone just compiled. Without a
		// stamp there is no way to tell from the outside, and "I built it" silently
		// becomes "it is live" — which has already cost a debugging session chasing
		// tools that were compiled out of the deployed binary.
		//
		// __DATE__/__TIME__ is per-translation-unit, so this is the router's compile
		// time, not a whole-build id. That is enough for the question being asked:
		// any source change forces this file to recompile via its dependency on the
		// registry header, so a stale stamp reliably means a stale deployment.
		ServerInfo->SetStringField(TEXT("buildStamp"), TEXT(__DATE__) TEXT(" ") TEXT(__TIME__));
		// Whether the WorldForge tool families are compiled in. A standalone Fab
		// build stubs 23 files out; deploying one into a WorldForge project silently
		// removes ~1700 tools, and the count alone does not say why.
#if defined(SOMOLMCP_WITH_WORLDFORGE) && SOMOLMCP_WITH_WORLDFORGE
		ServerInfo->SetBoolField(TEXT("worldForgeTools"), true);
#else
		ServerInfo->SetBoolField(TEXT("worldForgeTools"), false);
#endif
		{
			// Reported here so a client can compare against a known-good baseline in
			// one round trip, instead of only noticing a shrunken surface when some
			// later call fails with "no such tool".
			TArray<FString> RegisteredNames;
			Registry.GetRegisteredToolNamesSorted(RegisteredNames);
			ServerInfo->SetNumberField(TEXT("toolCount"), static_cast<double>(RegisteredNames.Num()));
		}

		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		if (const FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
		{
			Result->SetStringField(TEXT("sessionId"), Session->SessionId);
		}

		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
		Tools->SetBoolField(TEXT("listChanged"), true);
		Tools->SetBoolField(TEXT("pagination"), true);
		Tools->SetBoolField(TEXT("filtering"), true);
		Tools->SetBoolField(TEXT("toolsets"), true);
		Tools->SetBoolField(TEXT("execution_profiles"), true);
		Tools->SetBoolField(TEXT("resource_lock_planning"), true);
		Tools->SetBoolField(TEXT("parallel_authoring_plan"), true);
		Capabilities->SetObjectField(TEXT("tools"), Tools);
		TSharedRef<FJsonObject> Toolsets = MakeShared<FJsonObject>();
		Toolsets->SetBoolField(TEXT("supported"), true);
		Toolsets->SetBoolField(TEXT("list"), true);
		Toolsets->SetBoolField(TEXT("describe"), true);
		Toolsets->SetBoolField(TEXT("load"), true);
		Toolsets->SetBoolField(TEXT("lazy"), false);
		Toolsets->SetStringField(TEXT("source"), TEXT("somolmcp.registry.namespace"));
		Capabilities->SetObjectField(TEXT("toolsets"), Toolsets);
		TSharedRef<FJsonObject> ExecutionPlanning = MakeShared<FJsonObject>();
		ExecutionPlanning->SetBoolField(TEXT("supported"), true);
		ExecutionPlanning->SetStringField(TEXT("schema"), TEXT("somol.mcp_execution_planning:v1"));
		ExecutionPlanning->SetStringField(TEXT("profile_tool"), TEXT("mcp_tool_execution_profile"));
		ExecutionPlanning->SetStringField(TEXT("lock_plan_tool"), TEXT("mcp_resource_lock_plan"));
		ExecutionPlanning->SetStringField(TEXT("parallel_plan_tool"), TEXT("mcp_parallel_authoring_plan"));
		ExecutionPlanning->SetBoolField(TEXT("job_resource_lock_scheduling"), true);
		ExecutionPlanning->SetBoolField(TEXT("job_resource_lock_auto_inference"), true);
		ExecutionPlanning->SetBoolField(TEXT("tools_call_auto_enqueue"), true);
		ExecutionPlanning->SetBoolField(TEXT("generic_python_hidden"), true);
		ExecutionPlanning->SetBoolField(TEXT("python_backend_enabled"), false);
		ExecutionPlanning->SetStringField(TEXT("implementation_mode"), TEXT("native_cpp_only"));
		ExecutionPlanning->SetStringField(TEXT("execution_mode"), TEXT("queue_only_named_domain_tools"));
		ExecutionPlanning->SetStringField(TEXT("poll_method"), TEXT("jobs/get"));
		ExecutionPlanning->SetStringField(TEXT("events_method"), TEXT("jobs/events"));
		ExecutionPlanning->SetStringField(TEXT("cancel_method"), TEXT("jobs/cancel"));
		ExecutionPlanning->SetBoolField(TEXT("ue58_preview_required"), false);
		Capabilities->SetObjectField(TEXT("executionPlanning"), ExecutionPlanning);
		TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
		Resources->SetBoolField(TEXT("listChanged"), true);
		Resources->SetBoolField(TEXT("subscribe"), true);
		Capabilities->SetObjectField(TEXT("resources"), Resources);
		TSharedRef<FJsonObject> Notifications = MakeShared<FJsonObject>();
		Notifications->SetBoolField(TEXT("progress"), true);
		Notifications->SetBoolField(TEXT("message"), true);
		Notifications->SetBoolField(TEXT("toolsListChanged"), true);
		Notifications->SetBoolField(TEXT("resourcesUpdated"), true);
		Notifications->SetBoolField(TEXT("cancelled"), true);
		Capabilities->SetObjectField(TEXT("notifications"), Notifications);
		TSharedRef<FJsonObject> Roots = MakeShared<FJsonObject>();
		Roots->SetBoolField(TEXT("sessionScoped"), true);
		Roots->SetBoolField(TEXT("clientDeclared"), true);
		Roots->SetStringField(TEXT("enforcement"), TEXT("mutating_tools_call_and_jobs_submit_path_gate"));
		Roots->SetBoolField(TEXT("list"), true);
		Roots->SetBoolField(TEXT("listChanged"), true);
		Capabilities->SetObjectField(TEXT("roots"), Roots);
		TSharedRef<FJsonObject> Elicitation = MakeShared<FJsonObject>();
		Elicitation->SetBoolField(TEXT("create"), true);
		Elicitation->SetBoolField(TEXT("serverRequests"), true);
		Elicitation->SetStringField(TEXT("status"), TEXT("transport_ready_ui_handler_required"));
		Capabilities->SetObjectField(TEXT("elicitation"), Elicitation);
		TSharedRef<FJsonObject> ReverseRequests = MakeShared<FJsonObject>();
		ReverseRequests->SetBoolField(TEXT("supported"), true);
		ReverseRequests->SetStringField(TEXT("status_tool"), TEXT("server/requests/status"));
		ReverseRequests->SetStringField(TEXT("elicitation_probe_method"), TEXT("server/elicitation/create"));
		Capabilities->SetObjectField(TEXT("serverRequests"), ReverseRequests);
		Capabilities->SetObjectField(TEXT("prompts"), MakeShared<FJsonObject>());
		Capabilities->SetObjectField(TEXT("sampling"), MakeShared<FJsonObject>());
		Capabilities->SetObjectField(TEXT("completions"), MakeShared<FJsonObject>());
		TSharedRef<FJsonObject> Jobs = MakeShared<FJsonObject>();
		FSololmcpJobService::BuildCapabilitiesJobsObject(Jobs);
		Capabilities->SetObjectField(TEXT("jobs"), Jobs);
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		Result->SetStringField(TEXT("instructions"),
			TEXT("Use jobs/submit for long or multi-step work. Prefer 8-32 steps per job (hard maximum 64), keep the returned job_id, consume progress notifications or jobs/events, and do not poll more than once per second. Do not resubmit blocked jobs; wait for the blocking lock or use jobs/resume after elicitation. Keep concurrent mutating GameThread jobs at or below 4 and worker-safe read jobs at or below 16. Use client_request_id for idempotent retries."));

		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleToolsList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		FToolListView View = BuildToolListView(Registry.BuildToolsList(), Params);

		// Scope the *unfiltered* request only.
		//
		// A client that asks for a toolset, prefix, query, page or names_only has
		// said what it wants and gets exactly that, unchanged. The bare call is the
		// one that cannot be served literally: it is what every stock MCP client
		// opens with, and answering it in full is ~4.1M tokens, so the connection
		// dies before the client can ask anything more specific. Scoping it to the
		// bootstrap set plus whatever this session has activated is the difference
		// between a usable server and an unusable one.
		//
		// `all: true` opts back into the complete list for callers that want it.
		bool bWantsEverything = false;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("all"), bWantsEverything);
		}
		const bool bUnfiltered = !View.bHadParams
			|| (View.Toolset.IsEmpty() && View.Prefix.IsEmpty() && View.Query.IsEmpty()
				&& View.Limit <= 0 && !View.bNamesOnly && !bWantsEverything);

		int32 ScopedAway = 0;
		if (bUnfiltered && !bWantsEverything)
		{
			const FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
			TArray<TSharedPtr<FJsonValue>> Scoped;
			for (const TSharedPtr<FJsonValue>& ToolValue : View.Tools)
			{
				const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				FString ToolName;
				if (!ToolObj.IsValid() || !ToolObj->TryGetStringField(TEXT("name"), ToolName))
				{
					continue;
				}
				const bool bVisible = IsBootstrapTool(ToolName)
					|| (Session != nullptr && Session->ActiveToolGroups.Contains(ToolGroupOf(ToolName)));
				if (bVisible)
				{
					Scoped.Add(ToolValue);
				}
				else
				{
					++ScopedAway;
				}
			}
			View.Tools = MoveTemp(Scoped);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), View.Tools);
		Result->SetBoolField(TEXT("hidden"), false);
		Result->SetBoolField(TEXT("public_discovery"), true);
		Result->SetBoolField(TEXT("read_only_discovery"), true);
		Result->SetStringField(TEXT("source"), TEXT("somolmcp.public_tools_list"));
		AddToolListMetadata(Result, View);
		if (ScopedAway > 0)
		{
			// Say plainly that this is a view, not the inventory. A client that
			// believes it has seen everything will wrongly conclude a tool does not
			// exist, which is exactly the failure this whole mechanism exists to
			// avoid -- so the count, the reason and the two ways out are all stated.
			Result->SetNumberField(TEXT("not_shown"), ScopedAway);
			Result->SetNumberField(TEXT("total_registered"), ScopedAway + View.Tools.Num());
			Result->SetBoolField(TEXT("scoped"), true);
			Result->SetStringField(TEXT("scope_reason"),
				TEXT("This server registers thousands of tools; listing them all is far larger than "
					 "any model context, so an unfiltered list shows the discovery and dispatch tools "
					 "plus whatever this session has activated."));
			Result->SetStringField(TEXT("discover_all"),
				TEXT("Call the tool_catalog tool: mode=groups, then mode=names, then mode=schemas. It "
					 "enumerates every registered tool and reports whether each listing is complete."));
			Result->SetStringField(TEXT("activate"),
				TEXT("toolsets/load with a toolset name adds it to this session and emits "
					 "notifications/tools/list_changed. Calling any tool by name also works and "
					 "activates its group automatically — nothing here is unreachable."));
		}
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleToolsetsList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		int32 Total = 0;
		TArray<TSharedPtr<FJsonValue>> Toolsets = BuildToolsetSummaries(
			Registry.BuildToolsList(),
			GetStringParamOptional(Params, TEXT("query")),
			Total);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("toolsets"), Toolsets);
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetNumberField(TEXT("returned"), Toolsets.Num());
		Result->SetStringField(TEXT("source"), TEXT("somolmcp.public_toolsets_list"));
		Result->SetBoolField(TEXT("hidden"), false);
		Result->SetBoolField(TEXT("public_discovery"), true);
		Result->SetBoolField(TEXT("read_only_discovery"), true);
		Result->SetBoolField(TEXT("lazy"), false);
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleToolsetsDescribe(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		if (!Params.IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}

		FString Name = GetStringParamOptional(Params, TEXT("name"));
		Name = NormalizeToolsetName(Name);
		if (Name.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.name"));
		}

		TSharedPtr<FJsonObject> ToolParams = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Params, ToolParams);
		if (!ToolParams->HasField(TEXT("include_schemas")))
		{
			ToolParams->SetBoolField(TEXT("include_schemas"), false);
		}
		FToolListView View = BuildToolListView(Registry.BuildToolsList(), ToolParams, Name);
		if (View.Total == 0)
		{
			return ReplyError(Id, -32602, FString::Printf(TEXT("Toolset not found: %s"), *Name));
		}

		TArray<FString> Samples;
		for (int32 Index = 0; Index < FMath::Min(5, View.Tools.Num()); ++Index)
		{
			const TSharedPtr<FJsonObject> ToolObj = View.Tools[Index].IsValid() ? View.Tools[Index]->AsObject() : nullptr;
			FString ToolName;
			if (ToolObj.IsValid() && ToolObj->TryGetStringField(TEXT("name"), ToolName))
			{
				Samples.Add(ToolName);
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("toolset"), MakeToolsetSummaryObject(Name, View.Total, Samples));
		Result->SetArrayField(TEXT("tools"), View.Tools);
		Result->SetBoolField(TEXT("hidden"), false);
		Result->SetBoolField(TEXT("public_discovery"), true);
		Result->SetBoolField(TEXT("read_only_discovery"), true);
		Result->SetStringField(TEXT("source"), TEXT("somolmcp.public_toolset_describe"));
		AddToolListMetadata(Result, View);
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleToolsetsLoad(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		if (!Params.IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}

		FString Name = GetStringParamOptional(Params, TEXT("name"));
		Name = NormalizeToolsetName(Name);
		const TSharedPtr<FJsonObject>* DataManifestPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("data_tool_manifest"), DataManifestPtr) && DataManifestPtr && DataManifestPtr->IsValid())
		{
			FString ManifestName;
			(*DataManifestPtr)->TryGetStringField(TEXT("name"), ManifestName);
			ManifestName = NormalizeToolsetName(ManifestName);
			if (ManifestName.IsEmpty())
			{
				return ReplyError(Id, -32602, TEXT("data_tool_manifest.name is required"));
			}

			const TArray<TSharedPtr<FJsonValue>>* DeclaredTools = nullptr;
			const int32 DeclaredToolCount =
				((*DataManifestPtr)->TryGetArrayField(TEXT("tools"), DeclaredTools) && DeclaredTools)
					? DeclaredTools->Num()
					: 0;
			if (DeclaredToolCount <= 0)
			{
				return ReplyError(Id, -32602, TEXT("data_tool_manifest.tools must contain at least one tool definition"));
			}

			TSet<FString> DeclaredToolNames;
			TArray<FString> RegisteredToolNames;
			Registry.GetRegisteredToolNamesSorted(RegisteredToolNames);
			TSet<FString> RegisteredToolNameSet;
			for (const FString& RegisteredName : RegisteredToolNames)
			{
				RegisteredToolNameSet.Add(RegisteredName);
			}
			TArray<TSharedPtr<FJsonValue>> ToolNameValues;
			for (int32 ToolIndex = 0; ToolIndex < DeclaredToolCount; ++ToolIndex)
			{
				const TSharedPtr<FJsonObject> ToolObj = (*DeclaredTools)[ToolIndex].IsValid()
					? (*DeclaredTools)[ToolIndex]->AsObject()
					: nullptr;
				if (!ToolObj.IsValid())
				{
					return ReplyError(Id, -32602, FString::Printf(TEXT("data_tool_manifest.tools[%d] must be an object"), ToolIndex));
				}

				FString ToolName;
				ToolObj->TryGetStringField(TEXT("name"), ToolName);
				ToolName = ToolName.TrimStartAndEnd();
				if (ToolName.IsEmpty())
				{
					return ReplyError(Id, -32602, FString::Printf(TEXT("data_tool_manifest.tools[%d].name is required"), ToolIndex));
				}
				if (DeclaredToolNames.Contains(ToolName))
				{
					return ReplyError(Id, -32602, FString::Printf(TEXT("data_tool_manifest duplicate tool name: %s"), *ToolName));
				}
				if (RegisteredToolNameSet.Contains(ToolName))
				{
					return ReplyError(Id, -32602, FString::Printf(TEXT("data_tool_manifest tool name collides with registered MCP tool: %s"), *ToolName));
				}
				DeclaredToolNames.Add(ToolName);
				ToolNameValues.Add(MakeShared<FJsonValueString>(ToolName));
			}

			if (FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
			{
				TSharedPtr<FJsonObject> ManifestCopy = MakeShared<FJsonObject>();
				FJsonObject::Duplicate(*DataManifestPtr, ManifestCopy);
				Session->LoadedDataToolManifests.Add(ManifestName, ManifestCopy);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("somolmcp.data_tool_manifest.load_result:v1"));
			Result->SetStringField(TEXT("name"), ManifestName);
			Result->SetBoolField(TEXT("loaded"), true);
			Result->SetBoolField(TEXT("validated"), true);
			Result->SetNumberField(TEXT("declared_tool_count"), DeclaredToolCount);
			Result->SetArrayField(TEXT("declared_tool_names"), ToolNameValues);
			Result->SetBoolField(TEXT("runtime_registry_mutation_enabled"), false);
			Result->SetBoolField(TEXT("session_macro_execution_enabled"), true);
			Result->SetStringField(TEXT("executor"), TEXT("trusted_macro_steps"));
			Result->SetStringField(TEXT("status"), TEXT("manifest_recorded_session_macro_execution_available"));
			Result->SetStringField(TEXT("resource"), TEXT("somolmcp://data-driven-tools"));
			Result->SetObjectField(TEXT("manifest"), *DataManifestPtr);

			TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
			Notification->SetStringField(TEXT("toolset"), ManifestName);
			Notification->SetStringField(TEXT("source"), TEXT("toolsets/load.data_tool_manifest"));
			Notification->SetBoolField(TEXT("runtime_registry_mutation_enabled"), false);
			Notification->SetBoolField(TEXT("session_macro_execution_enabled"), true);
			EmitNotification(ConnectionId, TEXT("notifications/tools/list_changed"), Notification);
			EmitResourceUpdated(ConnectionId, TEXT("somolmcp://data-driven-tools"), TEXT("data_tool_manifest_loaded"));
			return ReplyResult(Id, Result);
		}
		if (Name.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.name"));
		}

		FToolListView View = BuildToolListView(Registry.BuildToolsList(), Params, Name);
		if (View.Total == 0)
		{
			return ReplyError(Id, -32602, FString::Printf(TEXT("Toolset not found: %s"), *Name));
		}

		TArray<FString> Samples;
		for (int32 Index = 0; Index < FMath::Min(5, View.Tools.Num()); ++Index)
		{
			const TSharedPtr<FJsonObject> ToolObj = View.Tools[Index].IsValid() ? View.Tools[Index]->AsObject() : nullptr;
			FString ToolName;
			if (ToolObj.IsValid() && ToolObj->TryGetStringField(TEXT("name"), ToolName))
			{
				Samples.Add(ToolName);
			}
		}

		// Record the activation so a later unfiltered tools/list actually shows this
		// toolset. Without this the load notification would tell the client the list
		// changed and the re-list would look identical.
		if (FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
		{
			for (const TSharedPtr<FJsonValue>& ToolValue : View.Tools)
			{
				const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				FString ToolName;
				if (ToolObj.IsValid() && ToolObj->TryGetStringField(TEXT("name"), ToolName))
				{
					Session->ActiveToolGroups.Add(ToolGroupOf(ToolName));
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("toolset"), MakeToolsetSummaryObject(Name, View.Total, Samples));
		Result->SetArrayField(TEXT("tools"), View.Tools);
		Result->SetBoolField(TEXT("loaded"), true);
		AddToolListMetadata(Result, View);
		TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
		Notification->SetStringField(TEXT("toolset"), Name);
		Notification->SetNumberField(TEXT("total"), View.Total);
		Notification->SetStringField(TEXT("source"), TEXT("toolsets/load"));
		EmitNotification(ConnectionId, TEXT("notifications/tools/list_changed"), Notification);
		EmitResourceUpdated(ConnectionId, TEXT("somolmcp://toolsets"), TEXT("toolsets_load"));
		EmitResourceUpdated(ConnectionId, TEXT("somolmcp://tools"), TEXT("toolsets_load"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleToolsCall(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request) const
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}
		const TSharedRef<FJsonObject> Params = (*ParamsPtr).ToSharedRef();

		FString ToolName;
		if (!Params->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.name"));
		}
		if (IsExternalPythonSurfaceToolName(ToolName))
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("reason_code"), TEXT("external_python_surface_hidden"));
			Data->SetStringField(TEXT("execution_mode"), TEXT("queue_only_named_domain_tools"));
			Data->SetStringField(TEXT("guidance"), TEXT("Use tools/list and the native C++ job queue to select a named SOMOLMCP domain tool."));
			return ReplyError(Id, -32038, TEXT("Python execution tools are not exposed by SOMOLMCP."), Data);
		}

		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("arguments"), ArgsPtr) || !ArgsPtr || !ArgsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.arguments"));
		}

		FString RootsError;
		if (!ValidateToolsCallRoots(ConnectionId, ToolName, (*ArgsPtr).ToSharedRef(), RootsError))
		{
			UE_LOG(LogSOMOLMCP, Warning, TEXT("%s"), *RootsError);
			return ReplyError(Id, -32034, RootsError);
		}

		// --- Safety: dangerous console command interception ---
		if (ToolName == TEXT("console_exec"))
		{
			FString ConsoleCmd;
			if ((*ArgsPtr)->TryGetStringField(TEXT("command"), ConsoleCmd))
			{
				const FString CmdLower = ConsoleCmd.ToLower().TrimStartAndEnd();
				// Block destructive / system-level console commands
				TArray<FString> DangerousPrefixes = {
					TEXT("exit"), TEXT("quit"), TEXT("shutdown"), TEXT("restart"),
					TEXT("obj"), TEXT("deleteall"), TEXT("destroyall"),
					TEXT("purge"), TEXT("wipe"), TEXT("resetall"),
					TEXT("r.setres"), TEXT("fullscreen"),
					TEXT("net."), TEXT("session."),
					TEXT("dllhost"), TEXT("cmd"), TEXT("powershell"), TEXT("bash"), TEXT("sh"),
					TEXT("exec "), TEXT("system("), TEXT("os."),
					TEXT("subprocess"), TEXT("import os"), TEXT("import sys"),
				};
				for (const FString& Prefix : DangerousPrefixes)
				{
					if (CmdLower.StartsWith(Prefix))
					{
						UE_LOG(LogSOMOLMCP, Warning, TEXT("Blocked dangerous console command: %s"), *ConsoleCmd);
						return ReplyError(Id, -32031, FString::Printf(TEXT("Dangerous console command blocked: %s"), *ConsoleCmd.Left(80)));
					}
				}
			}
		}

		// --- Legacy defense in depth for builds that explicitly re-enable the
		// generic Python surface. Current public builds reject these names above. ---
		// Note: import os, sys, json, math, pathlib etc. are allowed.
		// Only actual dangerous CALLS are blocked.
		// Also blocks "slow scan" patterns that would freeze the UE game thread for minutes.
		// Also accepts python_execute (legacy alias) by mirroring on both names.
		if (ToolName == TEXT("python_exec") || ToolName == TEXT("python_execute"))
		{
			FString PythonCode;
			if (!(*ArgsPtr)->TryGetStringField(TEXT("code"), PythonCode))
			{
				(*ArgsPtr)->TryGetStringField(TEXT("script"), PythonCode);
			}
			if (!PythonCode.IsEmpty())
			{
				const FString CodeLower = PythonCode.ToLower();

				// (a) Dangerous OS/eval patterns — refuse outright.
				TArray<FString> DangerousPatterns = {
					TEXT("import subprocess"), TEXT("import shutil"),
					TEXT("os.system"), TEXT("os.popen"), TEXT("os.exec"), TEXT("os.spawn"),
					TEXT("subprocess.call"), TEXT("subprocess.run"), TEXT("subprocess.popen"),
					TEXT("shutil.rmtree"), TEXT("shutil.move"), TEXT("shutil.copy"),
					TEXT("eval("), TEXT("exec("), TEXT("compile("),
					TEXT("__import__"), TEXT("globals("), TEXT("locals("),
					TEXT("socket."), TEXT("http.server"),
					TEXT("ctypes"), TEXT("ctypes.cdll"),
				};
				for (const FString& Pattern : DangerousPatterns)
				{
					if (CodeLower.Contains(Pattern))
					{
						UE_LOG(LogSOMOLMCP, Warning, TEXT("Blocked dangerous python pattern in python_exec"));
						return ReplyError(Id, -32031, FString::Printf(TEXT("Dangerous python code pattern blocked: '%s'"), *Pattern));
					}
				}

				// (b) "Slow scan" patterns — refuse with hint to use lite tools instead.
				// These all enumerate the entire scene / asset registry on the game thread,
				// freezing the editor UI for minutes. Defense-in-depth: client orchestrator
				// also blocks these but a malicious / out-of-date client can bypass.
				struct FSlowPattern { const TCHAR* Pattern; const TCHAR* Replacement; };
				const FSlowPattern SlowPatterns[] = {
					{ TEXT(".get_all_level_actors("),         TEXT("actor_list (paginated, supports filters)") },
					{ TEXT(".get_loaded_regions("),           TEXT("world_partition_status_lite") },
					{ TEXT(".get_all_assets("),               TEXT("asset_query with filters + limit") },
					{ TEXT("get_assets_by_path("),            TEXT("asset_query with category + limit") },
					{ TEXT("get_assets_by_class("),           TEXT("asset_query with class filter + limit") },
				};
				for (const FSlowPattern& SP : SlowPatterns)
				{
					if (CodeLower.Contains(SP.Pattern))
					{
						UE_LOG(LogSOMOLMCP, Warning,
							TEXT("Blocked slow python_exec pattern: %s — suggested replacement: %s"),
							SP.Pattern, SP.Replacement);
						return ReplyError(Id, -32032, FString::Printf(
							TEXT("python_exec script contains a known-slow pattern that freezes the UE editor: '%s'. ")
							TEXT("Use this instead: %s. Reason: this call runs synchronously on the UE game thread for ")
							TEXT("the entire script duration; full-scene/full-asset enumeration takes minutes and freezes ")
							TEXT("the editor UI."),
							SP.Pattern, SP.Replacement));
					}
				}

				// (c) Length cap: scripts > 8000 chars are typically batch operations that take long.
				if (PythonCode.Len() > 8000)
				{
					UE_LOG(LogSOMOLMCP, Warning, TEXT("Blocked oversized python_exec script: %d chars"), PythonCode.Len());
					return ReplyError(Id, -32033, FString::Printf(
						TEXT("python_exec script is %d chars (cap: 8000). Long scripts often contain full-scene scans ")
						TEXT("that freeze the UE editor. Break it into smaller incremental steps, or use specialized tools ")
						TEXT("(actor_list / asset_query / world_partition_status_lite / landscape_actor_list_lite)."),
						PythonCode.Len()));
				}
			}
		}

		// Session-scoped data-driven tools currently execute inside the router
		// because their handlers are not owned by the global job registry. Keep
		// that compatibility path direct; every registered product/domain tool is
		// routed through the queue below.
		TSharedRef<FJsonObject> DataDrivenStructured = MakeShared<FJsonObject>();
		FString DataDrivenSummary;
		FString DataDrivenError;
		bool bDataDrivenHandled = false;
		const bool bDataDrivenSuccess = TryExecuteSessionDataDrivenTool(
			ConnectionId,
			ToolName,
			(*ArgsPtr).ToSharedRef(),
			bDataDrivenHandled,
			DataDrivenStructured,
			DataDrivenSummary,
			DataDrivenError);
		if (bDataDrivenHandled)
		{
			const TSharedRef<FJsonObject> ToolResult = MakeToolCallResult(
				bDataDrivenSuccess ? DataDrivenSummary : DataDrivenError,
				DataDrivenStructured,
				!bDataDrivenSuccess);
			return ReplyResult(Id, ToolResult);
		}

		// Calling a tool this session has not surfaced is allowed and activates its
		// group. This is what keeps the scoped tools/list from being a downgrade: a
		// model that found a tool through tool_catalog can call it immediately, and
		// the client's next list reflects what is actually in use rather than a guess
		// made before the work started. Refusing here would turn discovery into a
		// dead end and force clients to know about activation before they could do
		// anything.
		if (Registry.HasRegisteredTool(ToolName) && !IsBootstrapTool(ToolName))
		{
			if (const FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
			{
				const FString Group = ToolGroupOf(ToolName);
				if (!Session->ActiveToolGroups.Contains(Group))
				{
					Session->ActiveToolGroups.Add(Group);
					TSharedRef<FJsonObject> Activated = MakeShared<FJsonObject>();
					Activated->SetStringField(TEXT("toolset"), Group);
					Activated->SetStringField(TEXT("source"), TEXT("tools/call.auto_activate"));
					Activated->SetStringField(TEXT("trigger_tool"), ToolName);
					EmitNotification(ConnectionId, TEXT("notifications/tools/list_changed"), Activated);
				}
			}
		}

		if (CVarQueueOnlyExecution.GetValueOnAnyThread() != 0
			&& Registry.HasRegisteredTool(ToolName)
			&& !IsQueueControlToolName(ToolName))
		{
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("tool"), ToolName);
			Step->SetStringField(TEXT("label"), FString::Printf(TEXT("Queued tools/call: %s"), *ToolName));
			Step->SetObjectField(TEXT("arguments"), (*ArgsPtr).ToSharedRef());

			TArray<TSharedPtr<FJsonValue>> Steps;
			Steps.Add(MakeShared<FJsonValueObject>(Step));
			TSharedRef<FJsonObject> SubmitParams = MakeShared<FJsonObject>();
			SubmitParams->SetArrayField(TEXT("steps"), Steps);
			SubmitParams->SetStringField(TEXT("plan_label"), FString::Printf(TEXT("tools/call queue adapter: %s"), *ToolName));

			for (const TCHAR* Field : { TEXT("client_request_id"), TEXT("trace_id"), TEXT("priority"), TEXT("_priority"), TEXT("_project_path"), TEXT("_instance_uuid"), TEXT("project_path"), TEXT("instance_uuid") })
			{
				FString Value;
				if (Params->TryGetStringField(Field, Value) && !Value.IsEmpty())
				{
					SubmitParams->SetStringField(Field, Value);
				}
			}

			// Preserve explicit scheduling and target-guard metadata when adapting
			// tools/call to jobs/submit. Stateful editor operations such as managed
			// Slate authoring may not have an asset path in their tool arguments and
			// therefore rely on a scoped editor_ui/world resource lock. Dropping these
			// fields made otherwise valid queue-only calls permanently blocked.
			for (const TCHAR* Field : { TEXT("resource_locks"), TEXT("_resource_locks") })
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (Params->TryGetArrayField(Field, Values) && Values)
				{
					SubmitParams->SetArrayField(Field, *Values);
				}
			}
			for (const TCHAR* Field : { TEXT("target_binding"), TEXT("_target_binding"), TEXT("replay_target_binding"), TEXT("retry_target_binding") })
			{
				const TSharedPtr<FJsonObject>* Value = nullptr;
				if (Params->TryGetObjectField(Field, Value) && Value && Value->IsValid())
				{
					SubmitParams->SetObjectField(Field, (*Value).ToSharedRef());
				}
			}

			TSharedRef<FJsonObject> QueueReceipt = MakeShared<FJsonObject>();
			FString QueueError;
			if (!FSololmcpJobService::SubmitJob(SubmitParams, QueueReceipt, QueueError))
			{
				return ReplyError(Id, -32602, QueueError);
			}
			QueueReceipt->SetStringField(TEXT("execution_mode"), TEXT("queue_only"));
			QueueReceipt->SetStringField(TEXT("submitted_tool"), ToolName);
			QueueReceipt->SetStringField(TEXT("poll_method"), TEXT("jobs/get"));
			QueueReceipt->SetStringField(TEXT("events_method"), TEXT("jobs/events"));
			QueueReceipt->SetStringField(TEXT("cancel_method"), TEXT("jobs/cancel"));
			const TSharedRef<FJsonObject> ToolResult = MakeToolCallResult(
				FString::Printf(TEXT("Queued %s. Poll jobs/get or jobs/events until the returned job reaches a terminal state."), *ToolName),
				QueueReceipt,
				false);
			return ReplyResult(Id, ToolResult);
		}

		TSharedRef<FJsonObject> Structured = MakeShared<FJsonObject>();
		FString Summary;
		FString Error;
		// CRITICAL (parity with SololmcpJobService::ExecuteToolWithSehGuard): the
		// synchronous tools/call lane (atomic direct-channel + texture_studio import
		// handoff) runs ExecuteTool on the GameThread. A tool that pops a MODAL Slate
		// dialog (e.g. landscape edit-layer insert from landscape_batch_generate_world,
		// water_body_create_v2) would block the GameThread modal loop forever — nobody
		// clicks the button headless — wedging ALL subsequent MCP calls. The global
		// modal guard was previously only on the job lane, leaving this lane exposed.
		// GIsRunningUnattendedScript=true makes FSlateApplication::AddModalWindow
		// early-return and routes FMessageDialog to its default. TGuardValue restores
		// the prior value so interactive editor use between calls is unaffected.
		TGuardValue<bool> UnattendedModalGuard(GIsRunningUnattendedScript, true);
		// Foliage serialization: the sync lane runs on the GameThread while the job
		// queue may be executing a foliage_* tool on a worker thread concurrently.
		LockFoliageActorMutex();
		const bool bSuccess = Registry.ExecuteTool(ToolName, (*ArgsPtr).ToSharedRef(), Structured, Summary, Error);
		UnlockFoliageActorMutex();
		const TSharedRef<FJsonObject> ToolResult = MakeToolCallResult(bSuccess ? Summary : Error, Structured, !bSuccess);
		return ReplyResult(Id, ToolResult);
	}

	FString FSololmcpRouter::HandleResourcesList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id) const
	{
		(void)ConnectionId;
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Resources;
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://capabilities"),
			TEXT("Server Capabilities"),
			TEXT("Current MCP capability snapshot."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://tools"),
			TEXT("Registered Tools"),
			TEXT("List of currently available SOMOLMCP tools."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://toolsets"),
			TEXT("Registered Toolsets"),
			TEXT("Namespace-grouped SOMOLMCP tool families for role-scoped loading."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://execution-planning"),
			TEXT("MCP Execution Planning"),
			TEXT("Multi-agent dispatch lanes, resource-lock planning, and UE version contract metadata."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://status"),
			TEXT("Server Status"),
			TEXT("Server runtime status and transport model."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://jobs"),
			TEXT("MCP Job Snapshot"),
			TEXT("Tracked MCP jobs, blocked/running counts, and resumable elicitation state."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://server/requests"),
			TEXT("Server Reverse Requests"),
			TEXT("Pending and answered server-to-client requests such as elicitation/create."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://roots"),
			TEXT("Session Roots"),
			TEXT("Client-declared roots and MCP root enforcement state for this session."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://asset-index"),
			TEXT("Asset Index Context"),
			TEXT("Current project content root and standard resource URI contract for asset index enrichment."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://level/current/manifest"),
			TEXT("Current Level Manifest"),
			TEXT("Lightweight current-project level manifest contract for scene-aware agents."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://rag/tool-catalog"),
			TEXT("Tool Catalog RAG Context"),
			TEXT("Tool catalog and role coverage authority hints exposed as MCP resources."),
			TEXT("application/json"))));
		Resources.Add(MakeShared<FJsonValueObject>(BuildResourceItem(
			TEXT("somolmcp://data-driven-tools"),
			TEXT("Data-driven Tool Manifests"),
			TEXT("Session-loaded data-driven tool manifest foundation and schema guidance."),
			TEXT("application/json"))));
		Result->SetArrayField(TEXT("resources"), Resources);
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleResourcesRead(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}
		FString Uri;
		if (!(*ParamsPtr)->TryGetStringField(TEXT("uri"), Uri) || Uri.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.uri"));
		}
		const TSharedPtr<FJsonObject> Params = *ParamsPtr;

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Contents;
		TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("uri"), Uri);
		Content->SetStringField(TEXT("mimeType"), TEXT("application/json"));

		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		if (Uri == TEXT("somolmcp://capabilities"))
		{
			TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
			Tools->SetBoolField(TEXT("listChanged"), true);
			Tools->SetBoolField(TEXT("pagination"), true);
			Tools->SetBoolField(TEXT("filtering"), true);
			Tools->SetBoolField(TEXT("toolsets"), true);
			Tools->SetBoolField(TEXT("execution_profiles"), true);
			Tools->SetBoolField(TEXT("resource_lock_planning"), true);
			Tools->SetBoolField(TEXT("parallel_authoring_plan"), true);
			Capabilities->SetObjectField(TEXT("tools"), Tools);
			TSharedRef<FJsonObject> Toolsets = MakeShared<FJsonObject>();
			Toolsets->SetBoolField(TEXT("supported"), true);
			Toolsets->SetBoolField(TEXT("list"), true);
			Toolsets->SetBoolField(TEXT("describe"), true);
			Toolsets->SetBoolField(TEXT("load"), true);
			Toolsets->SetBoolField(TEXT("lazy"), false);
			Toolsets->SetStringField(TEXT("source"), TEXT("somolmcp.registry.namespace"));
			Capabilities->SetObjectField(TEXT("toolsets"), Toolsets);
			TSharedRef<FJsonObject> ExecutionPlanning = MakeShared<FJsonObject>();
			ExecutionPlanning->SetBoolField(TEXT("supported"), true);
			ExecutionPlanning->SetStringField(TEXT("schema"), TEXT("somol.mcp_execution_planning:v1"));
			ExecutionPlanning->SetStringField(TEXT("profile_tool"), TEXT("mcp_tool_execution_profile"));
			ExecutionPlanning->SetStringField(TEXT("lock_plan_tool"), TEXT("mcp_resource_lock_plan"));
			ExecutionPlanning->SetStringField(TEXT("parallel_plan_tool"), TEXT("mcp_parallel_authoring_plan"));
			ExecutionPlanning->SetBoolField(TEXT("job_resource_lock_scheduling"), true);
			ExecutionPlanning->SetBoolField(TEXT("job_resource_lock_auto_inference"), true);
			ExecutionPlanning->SetBoolField(TEXT("ue58_preview_required"), false);
			Capabilities->SetObjectField(TEXT("executionPlanning"), ExecutionPlanning);
			TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
			Resources->SetBoolField(TEXT("listChanged"), true);
			Resources->SetBoolField(TEXT("subscribe"), true);
			Capabilities->SetObjectField(TEXT("resources"), Resources);
			TSharedRef<FJsonObject> Notifications = MakeShared<FJsonObject>();
			Notifications->SetBoolField(TEXT("progress"), true);
			Notifications->SetBoolField(TEXT("message"), true);
			Notifications->SetBoolField(TEXT("toolsListChanged"), true);
			Notifications->SetBoolField(TEXT("resourcesUpdated"), true);
			Notifications->SetBoolField(TEXT("cancelled"), true);
			Capabilities->SetObjectField(TEXT("notifications"), Notifications);
			TSharedRef<FJsonObject> Roots = MakeShared<FJsonObject>();
			Roots->SetBoolField(TEXT("sessionScoped"), true);
			Roots->SetBoolField(TEXT("clientDeclared"), true);
			Roots->SetStringField(TEXT("enforcement"), TEXT("mutating_tools_call_and_jobs_submit_path_gate"));
			Roots->SetBoolField(TEXT("list"), true);
			Roots->SetBoolField(TEXT("listChanged"), true);
			Capabilities->SetObjectField(TEXT("roots"), Roots);
			TSharedRef<FJsonObject> Elicitation = MakeShared<FJsonObject>();
			Elicitation->SetBoolField(TEXT("create"), true);
			Elicitation->SetBoolField(TEXT("serverRequests"), true);
			Elicitation->SetStringField(TEXT("status"), TEXT("transport_ready_ui_handler_required"));
			Capabilities->SetObjectField(TEXT("elicitation"), Elicitation);
			TSharedRef<FJsonObject> ReverseRequests = MakeShared<FJsonObject>();
			ReverseRequests->SetBoolField(TEXT("supported"), true);
			ReverseRequests->SetStringField(TEXT("status_tool"), TEXT("server/requests/status"));
			ReverseRequests->SetStringField(TEXT("elicitation_probe_method"), TEXT("server/elicitation/create"));
			Capabilities->SetObjectField(TEXT("serverRequests"), ReverseRequests);
			Capabilities->SetObjectField(TEXT("prompts"), MakeShared<FJsonObject>());
			Capabilities->SetObjectField(TEXT("sampling"), MakeShared<FJsonObject>());
			Capabilities->SetObjectField(TEXT("completions"), MakeShared<FJsonObject>());
			TSharedRef<FJsonObject> Jobs = MakeShared<FJsonObject>();
			FSololmcpJobService::BuildCapabilitiesJobsObject(Jobs);
			Capabilities->SetObjectField(TEXT("jobs"), Jobs);
			Payload->SetObjectField(TEXT("capabilities"), Capabilities);
		}
		else if (Uri == TEXT("somolmcp://tools"))
		{
			TArray<TSharedPtr<FJsonValue>> ToolsForResource = Registry.BuildToolsList();
			AppendSessionDataDrivenTools(ConnectionId, ToolsForResource);
			Payload->SetArrayField(TEXT("tools"), ToolsForResource);
		}
		else if (Uri == TEXT("somolmcp://toolsets"))
		{
			int32 Total = 0;
			TArray<TSharedPtr<FJsonValue>> ToolsForResource = Registry.BuildToolsList();
			AppendSessionDataDrivenTools(ConnectionId, ToolsForResource);
			Payload->SetArrayField(TEXT("toolsets"), BuildToolsetSummaries(ToolsForResource, FString(), Total));
			Payload->SetNumberField(TEXT("total"), Total);
			Payload->SetStringField(TEXT("source"), TEXT("somolmcp.registry.namespace"));
			Payload->SetBoolField(TEXT("lazy"), false);
		}
		else if (Uri == TEXT("somolmcp://execution-planning"))
		{
			Payload->SetStringField(TEXT("schema"), TEXT("somol.mcp_execution_planning:v1"));
			Payload->SetBoolField(TEXT("supported"), true);
			Payload->SetStringField(TEXT("profile_tool"), TEXT("mcp_tool_execution_profile"));
			Payload->SetStringField(TEXT("lock_plan_tool"), TEXT("mcp_resource_lock_plan"));
			Payload->SetStringField(TEXT("parallel_plan_tool"), TEXT("mcp_parallel_authoring_plan"));
			Payload->SetStringField(TEXT("min_engine_version"), TEXT("5.7"));
			Payload->SetBoolField(TEXT("job_resource_lock_scheduling"), true);
			Payload->SetBoolField(TEXT("job_resource_lock_auto_inference"), true);
			Payload->SetBoolField(TEXT("ue58_preview_required"), false);
			TArray<TSharedPtr<FJsonValue>> Lanes;
			for (const TCHAR* Lane : {
				TEXT("read"), TEXT("poll"), TEXT("agent_plan"), TEXT("write_asset"),
				TEXT("write_level"), TEXT("editor_ui"), TEXT("build"), TEXT("provider"),
				TEXT("qa"), TEXT("hermes")
			})
			{
				Lanes.Add(MakeShared<FJsonValueString>(FString(Lane)));
			}
			Payload->SetArrayField(TEXT("lanes"), Lanes);
			TArray<TSharedPtr<FJsonValue>> LockModes;
			LockModes.Add(MakeShared<FJsonValueString>(TEXT("shared")));
			LockModes.Add(MakeShared<FJsonValueString>(TEXT("exclusive")));
			Payload->SetArrayField(TEXT("lock_modes"), LockModes);
			Payload->SetStringField(TEXT("guidance"), TEXT("Use this MCP-side planner before jobs/submit to split agent work into non-conflicting waves; it is additive and does not require UE 5.8 preview APIs."));
		}
		else if (Uri == TEXT("somolmcp://status"))
		{
			Payload->SetStringField(TEXT("name"), TEXT("SOMOLMCP"));
			Payload->SetStringField(TEXT("version"), GetProductVersion());
			Payload->SetStringField(TEXT("transport"), TEXT("tcp_length_prefixed_jsonrpc"));
			Payload->SetStringField(TEXT("connectionModel"), TEXT("multi_client_same_port_no_preemption"));
			Payload->SetBoolField(TEXT("jobs_enabled"), true);
			Payload->SetBoolField(TEXT("execution_planning_enabled"), true);
			Payload->SetStringField(TEXT("execution_planning_schema"), TEXT("somol.mcp_execution_planning:v1"));

			// Include transport stats if getter is available
			if (TransportStatsGetter)
			{
				const auto Stats = TransportStatsGetter();
				TSharedRef<FJsonObject> StatsObj = MakeShared<FJsonObject>();
				StatsObj->SetNumberField(TEXT("active_connections"), Stats.ActiveConnections);
				StatsObj->SetNumberField(TEXT("total_accepted"), Stats.TotalAccepted);
				StatsObj->SetNumberField(TEXT("total_rejected"), Stats.TotalRejected);
				StatsObj->SetNumberField(TEXT("total_messages_received"), Stats.TotalMessagesReceived);
				StatsObj->SetNumberField(TEXT("total_messages_sent"), Stats.TotalMessagesSent);
				Payload->SetObjectField(TEXT("transport_stats"), StatsObj);
			}
		}
		else if (Uri == TEXT("somolmcp://jobs"))
		{
			FSololmcpJobService::BuildJobsSnapshotObject(Payload);
			Payload->SetStringField(TEXT("update_notification"), TEXT("notifications/resources/updated"));
			Payload->SetStringField(TEXT("related_progress_notification"), TEXT("notifications/progress"));
		}
		else if (Uri == TEXT("somolmcp://server/requests"))
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			int32 PendingCount = 0;
			int32 AnsweredCount = 0;
			int32 ErrorCount = 0;
			for (const TPair<FString, FPendingServerRequest>& Pair : PendingServerRequests)
			{
				const FPendingServerRequest& ServerRequest = Pair.Value;
				if (ServerRequest.bAnswered)
				{
					++AnsweredCount;
				}
				else
				{
					++PendingCount;
				}
				if (ServerRequest.bError)
				{
					++ErrorCount;
				}
				if (Items.Num() < 64)
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("request_id"), ServerRequest.RequestId);
					Obj->SetStringField(TEXT("method"), ServerRequest.Method);
					Obj->SetNumberField(TEXT("connection_id"), static_cast<double>(ServerRequest.ConnectionId));
					Obj->SetBoolField(TEXT("answered"), ServerRequest.bAnswered);
					Obj->SetBoolField(TEXT("error"), ServerRequest.bError);
					Obj->SetStringField(TEXT("created_utc"), ServerRequest.CreatedUtc.ToIso8601());
					if (ServerRequest.bAnswered)
					{
						Obj->SetStringField(TEXT("answered_utc"), ServerRequest.AnsweredUtc.ToIso8601());
					}
					if (!ServerRequest.ErrorMessage.IsEmpty())
					{
						Obj->SetStringField(TEXT("error_message"), ServerRequest.ErrorMessage);
					}
					Items.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
			Payload->SetStringField(TEXT("schema"), TEXT("somolmcp.server_requests.status:v1"));
			Payload->SetNumberField(TEXT("pending"), PendingCount);
			Payload->SetNumberField(TEXT("answered"), AnsweredCount);
			Payload->SetNumberField(TEXT("errors"), ErrorCount);
			Payload->SetArrayField(TEXT("requests"), Items);
		}
		else if (Uri == TEXT("somolmcp://roots"))
		{
			TArray<TSharedPtr<FJsonValue>> Roots;
			if (const FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
			{
				for (int32 Index = 0; Index < Session->RootUris.Num(); ++Index)
				{
					TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
					Root->SetStringField(TEXT("uri"), Session->RootUris[Index]);
					Root->SetStringField(TEXT("name"), FString::Printf(TEXT("session_root_%d"), Index + 1));
					Roots.Add(MakeShared<FJsonValueObject>(Root));
				}
				Payload->SetBoolField(TEXT("client_roots_capability"), Session->bClientRoots);
			}
			Payload->SetStringField(TEXT("schema"), TEXT("somolmcp.session_roots:v1"));
			Payload->SetArrayField(TEXT("roots"), Roots);
			Payload->SetStringField(TEXT("source"), TEXT("initialize.params.roots"));
			Payload->SetStringField(TEXT("enforcement"), TEXT("mutating_tools_call_and_jobs_submit_path_gate"));
		}
		else if (Uri == TEXT("somolmcp://asset-index"))
		{
			BuildAssetIndexResourcePayload(Payload, Params);
		}
		else if (Uri == TEXT("somolmcp://level/current/manifest"))
		{
			BuildCurrentLevelManifestResourcePayload(Payload, Params);
		}
		else if (Uri == TEXT("somolmcp://rag/tool-catalog"))
		{
			int32 TotalToolsets = 0;
			TArray<TSharedPtr<FJsonValue>> Tools = Registry.BuildToolsList();
			AppendSessionDataDrivenTools(ConnectionId, Tools);
			Payload->SetStringField(TEXT("schema"), TEXT("somolmcp.rag.tool_catalog:v1"));
			Payload->SetNumberField(TEXT("tool_count"), Tools.Num());
			Payload->SetNumberField(TEXT("toolset_count"), BuildToolsetSummaries(Tools, FString(), TotalToolsets).Num());
			Payload->SetStringField(TEXT("catalog_resource"), TEXT("somolmcp://tools"));
			Payload->SetStringField(TEXT("toolsets_resource"), TEXT("somolmcp://toolsets"));
			Payload->SetStringField(TEXT("role_assignment_authority"), TEXT("docs/SOMOLMCP_TOOLS_CATALOG.json"));
			Payload->SetStringField(TEXT("guidance"), TEXT("Agents should resolve tool names from this authority instead of inventing aliases."));
		}
		else if (Uri == TEXT("somolmcp://data-driven-tools"))
		{
			TArray<TSharedPtr<FJsonValue>> Manifests;
			if (const FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
			{
				const bool bIncludeManifest = GetBoolParam(Params, TEXT("include_manifest"), false);
				for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : Session->LoadedDataToolManifests)
				{
					TSharedRef<FJsonObject> ManifestRow = MakeShared<FJsonObject>();
					ManifestRow->SetStringField(TEXT("name"), Pair.Key);
					ManifestRow->SetStringField(TEXT("status"), TEXT("recorded_session_macro_execution_available"));
					ManifestRow->SetBoolField(TEXT("runtime_registry_mutation_enabled"), false);
					ManifestRow->SetBoolField(TEXT("session_macro_execution_enabled"), true);
					ManifestRow->SetStringField(TEXT("executor"), TEXT("trusted_macro_steps"));
					if (Pair.Value.IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* DeclaredTools = nullptr;
						const int32 DeclaredToolCount =
							(Pair.Value->TryGetArrayField(TEXT("tools"), DeclaredTools) && DeclaredTools)
								? DeclaredTools->Num()
								: 0;
						ManifestRow->SetNumberField(TEXT("declared_tool_count"), DeclaredToolCount);
						if (bIncludeManifest)
						{
							ManifestRow->SetObjectField(TEXT("manifest"), Pair.Value.ToSharedRef());
						}
					}
					Manifests.Add(MakeShared<FJsonValueObject>(ManifestRow));
				}
			}
			Payload->SetStringField(TEXT("schema"), TEXT("somolmcp.data_driven_tools.foundation:v1"));
			Payload->SetBoolField(TEXT("foundation_supported"), true);
			Payload->SetBoolField(TEXT("runtime_registry_mutation_enabled"), false);
			Payload->SetBoolField(TEXT("session_macro_execution_enabled"), true);
			Payload->SetStringField(TEXT("executor"), TEXT("trusted_macro_steps"));
			Payload->SetStringField(TEXT("load_method"), TEXT("toolsets/load"));
			Payload->SetStringField(TEXT("manifest_field"), TEXT("data_tool_manifest"));
			Payload->SetArrayField(TEXT("loaded_manifests"), Manifests);
			Payload->SetStringField(TEXT("guidance"), TEXT("This build validates and records data-driven tool manifests. Session-scoped macro tools with execution.kind='macro_steps' can call existing MCP tools through the trusted macro executor; global runtime C++ registry mutation remains disabled."));
		}
		else
		{
			return ReplyError(Id, -32601, FString::Printf(TEXT("Resource not found: %s"), *Uri));
		}

		Content->SetStringField(TEXT("text"), ToJsonString(Payload));
		Contents.Add(MakeShared<FJsonValueObject>(Content));
		Result->SetArrayField(TEXT("contents"), Contents);
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleResourcesSubscribe(
		FSololmcpTcpTransport::FConnectionId ConnectionId,
		const TSharedPtr<FJsonValue>& Id,
		const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		const FString Uri = GetStringParamOptional(Params, TEXT("uri"));
		if (Uri.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.uri"));
		}
		FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (Session)
		{
			Session->SubscribedResourceUris.Add(Uri);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("subscribed"), true);
		Result->SetStringField(TEXT("uri"), Uri);
		Result->SetStringField(TEXT("status"), TEXT("registered"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleResourcesUnsubscribe(
		FSololmcpTcpTransport::FConnectionId ConnectionId,
		const TSharedPtr<FJsonValue>& Id,
		const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		const FString Uri = GetStringParamOptional(Params, TEXT("uri"));
		if (Uri.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.uri"));
		}
		FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (Session)
		{
			Session->SubscribedResourceUris.Remove(Uri);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("subscribed"), false);
		Result->SetStringField(TEXT("uri"), Uri);
		Result->SetStringField(TEXT("status"), TEXT("removed"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleRootsList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id) const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Roots;
		if (const FMcpSession* Session = SessionsByConnection.Find(ConnectionId))
		{
			for (int32 Index = 0; Index < Session->RootUris.Num(); ++Index)
			{
				TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
				Root->SetStringField(TEXT("uri"), Session->RootUris[Index]);
				Root->SetStringField(TEXT("name"), FString::Printf(TEXT("session_root_%d"), Index + 1));
				Roots.Add(MakeShared<FJsonValueObject>(Root));
			}
		}
		Result->SetArrayField(TEXT("roots"), Roots);
		Result->SetStringField(TEXT("source"), TEXT("initialize.params.roots"));
		Result->SetStringField(TEXT("enforcement"), TEXT("mutating_tools_call_and_jobs_submit_path_gate"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleServerElicitationCreate(
		FSololmcpTcpTransport::FConnectionId ConnectionId,
		const TSharedPtr<FJsonValue>& Id,
		const TSharedRef<FJsonObject>& Request)
	{
		const FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (!Session || !Session->bClientElicitation)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("reason_code"), TEXT("client_elicitation_not_advertised"));
			Data->SetStringField(TEXT("fallback"), TEXT("return_structured_missing_input_or_blocked_job"));
			return ReplyError(Id, -32052, TEXT("Client did not advertise elicitation support for this session."), Data);
		}

		TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		if (!Params.IsValid())
		{
			Params = MakeShared<FJsonObject>();
		}

		FString RequestId;
		if (!SendServerRequest(ConnectionId, TEXT("elicitation/create"), Params.ToSharedRef(), RequestId))
		{
			return ReplyError(Id, -32053, TEXT("Failed to send server elicitation request to client."));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("request_id"), RequestId);
		Result->SetStringField(TEXT("status"), TEXT("sent"));
		Result->SetStringField(TEXT("poll"), TEXT("server/requests/status"));
		Result->SetStringField(TEXT("note"), TEXT("Transport has sent elicitation/create; tool/job code should keep the owning job blocked until a response arrives."));
		EmitResourceUpdated(ConnectionId, TEXT("somolmcp://server/requests"), TEXT("server_request_sent"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleServerRequestsStatus(const TSharedPtr<FJsonValue>& Id) const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Items;
		int32 PendingCount = 0;
		int32 AnsweredCount = 0;
		int32 ErrorCount = 0;
		for (const TPair<FString, FPendingServerRequest>& Pair : PendingServerRequests)
		{
			const FPendingServerRequest& Request = Pair.Value;
			if (Request.bAnswered)
			{
				++AnsweredCount;
			}
			else
			{
				++PendingCount;
			}
			if (Request.bError)
			{
				++ErrorCount;
			}
			if (Items.Num() < 64)
			{
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("request_id"), Request.RequestId);
				Obj->SetStringField(TEXT("method"), Request.Method);
				Obj->SetNumberField(TEXT("connection_id"), static_cast<double>(Request.ConnectionId));
				Obj->SetBoolField(TEXT("answered"), Request.bAnswered);
				Obj->SetBoolField(TEXT("error"), Request.bError);
				Obj->SetStringField(TEXT("created_utc"), Request.CreatedUtc.ToIso8601());
				if (Request.bAnswered)
				{
					Obj->SetStringField(TEXT("answered_utc"), Request.AnsweredUtc.ToIso8601());
				}
				if (!Request.ErrorMessage.IsEmpty())
				{
					Obj->SetStringField(TEXT("error_message"), Request.ErrorMessage);
				}
				Items.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
		Result->SetNumberField(TEXT("pending"), PendingCount);
		Result->SetNumberField(TEXT("answered"), AnsweredCount);
		Result->SetNumberField(TEXT("errors"), ErrorCount);
		Result->SetArrayField(TEXT("requests"), Items);
		Result->SetStringField(TEXT("schema"), TEXT("somolmcp.server_requests.status:v1"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandlePromptsList(const TSharedPtr<FJsonValue>& Id) const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Prompts;
		{
			TSharedRef<FJsonObject> Prompt = MakeShared<FJsonObject>();
			Prompt->SetStringField(TEXT("name"), TEXT("job_plan"));
			Prompt->SetStringField(TEXT("description"), TEXT("Create a robust multi-step job plan with tool sequencing."));
			Prompt->SetArrayField(TEXT("arguments"), BuildPromptArgsSchema());
			Prompts.Add(MakeShared<FJsonValueObject>(Prompt));
		}
		{
			TSharedRef<FJsonObject> Prompt = MakeShared<FJsonObject>();
			Prompt->SetStringField(TEXT("name"), TEXT("debug_tool_failure"));
			Prompt->SetStringField(TEXT("description"), TEXT("Diagnose a failing tool call and propose recovery actions."));
			Prompt->SetArrayField(TEXT("arguments"), BuildPromptArgsSchema());
			Prompts.Add(MakeShared<FJsonValueObject>(Prompt));
		}
		{
			TSharedRef<FJsonObject> Prompt = MakeShared<FJsonObject>();
			Prompt->SetStringField(TEXT("name"), TEXT("terrain_pcg_pipeline"));
			Prompt->SetStringField(TEXT("description"), TEXT("Build a terrain and PCG generation pipeline with validation checkpoints."));
			Prompt->SetArrayField(TEXT("arguments"), BuildPromptArgsSchema());
			Prompts.Add(MakeShared<FJsonValueObject>(Prompt));
		}
		Result->SetArrayField(TEXT("prompts"), Prompts);
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandlePromptsGet(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request) const
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}
		FString Name;
		if (!(*ParamsPtr)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.name"));
		}

		const TSharedRef<FJsonObject> Params = (*ParamsPtr).ToSharedRef();
		const FString Goal = GetStringParam(Params, TEXT("goal"));
		const FString Constraints = GetStringParam(Params, TEXT("constraints"));
		FString PromptText;
		if (Name == TEXT("job_plan"))
		{
			PromptText = FString::Printf(
				TEXT("Goal: %s\nGenerate a tool job plan with 3-8 ordered steps. Each step must include tool, arguments, and optional label. Include validation and rollback checkpoints. Constraints: %s"),
				Goal.IsEmpty() ? TEXT("(none)") : *Goal,
				Constraints.IsEmpty() ? TEXT("(none)") : *Constraints);
		}
		else if (Name == TEXT("debug_tool_failure"))
		{
			PromptText = FString::Printf(
				TEXT("Failure goal: %s\nReturn diagnosis with likely root cause, reproduction steps, and a minimal fix plan. Include when to retry vs fail-fast. Constraints: %s"),
				Goal.IsEmpty() ? TEXT("(none)") : *Goal,
				Constraints.IsEmpty() ? TEXT("(none)") : *Constraints);
		}
		else if (Name == TEXT("terrain_pcg_pipeline"))
		{
			PromptText = FString::Printf(
				TEXT("Pipeline objective: %s\nDesign a terrain+PCG workflow including terrain_spec_validate, terrain_tile_plan, PCG graph steps, generation, and verification checks. Constraints: %s"),
				Goal.IsEmpty() ? TEXT("(none)") : *Goal,
				Constraints.IsEmpty() ? TEXT("(none)") : *Constraints);
		}
		else
		{
			return ReplyError(Id, -32601, FString::Printf(TEXT("Prompt not found: %s"), *Name));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("description"), TEXT("SOMOLMCP prompt template"));
		TArray<TSharedPtr<FJsonValue>> Messages;
		{
			TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
			Msg->SetStringField(TEXT("role"), TEXT("user"));
			TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
			Content->SetStringField(TEXT("type"), TEXT("text"));
			Content->SetStringField(TEXT("text"), PromptText);
			TArray<TSharedPtr<FJsonValue>> MessageContent;
			MessageContent.Add(MakeShared<FJsonValueObject>(Content));
			Msg->SetArrayField(TEXT("content"), MessageContent);
			Messages.Add(MakeShared<FJsonValueObject>(Msg));
		}
		Result->SetArrayField(TEXT("messages"), Messages);
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleSamplingCreateMessage(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request) const
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}

		const TArray<TSharedPtr<FJsonValue>>* MessagesPtr = nullptr;
		if (!(*ParamsPtr)->TryGetArrayField(TEXT("messages"), MessagesPtr) || !MessagesPtr)
		{
			return ReplyError(Id, -32602, TEXT("Missing params.messages"));
		}

		TSharedRef<FJsonObject> BackendReq = MakeShared<FJsonObject>();
		BackendReq->SetArrayField(TEXT("messages"), *MessagesPtr);
		FString Model;
		if ((*ParamsPtr)->TryGetStringField(TEXT("model"), Model) && !Model.IsEmpty())
		{
			BackendReq->SetStringField(TEXT("model"), Model);
		}
		else
		{
			BackendReq->SetStringField(TEXT("model"), TEXT("gpt-4o-mini"));
		}

		double Temp = 0.0;
		if ((*ParamsPtr)->TryGetNumberField(TEXT("temperature"), Temp))
		{
			BackendReq->SetNumberField(TEXT("temperature"), Temp);
		}
		int32 MaxTokens = 0;
		if ((*ParamsPtr)->TryGetNumberField(TEXT("maxTokens"), MaxTokens) || (*ParamsPtr)->TryGetNumberField(TEXT("max_tokens"), MaxTokens))
		{
			BackendReq->SetNumberField(TEXT("max_tokens"), MaxTokens);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		if (!MakeSamplingBackendCall(BackendReq, Result, Error))
		{
			return ReplyError(Id, -32001, Error);
		}
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleCompletionsComplete(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}
		FString Prefix;
		(*ParamsPtr)->TryGetStringField(TEXT("prefix"), Prefix);
		Prefix = Prefix.ToLower();

		TArray<TSharedPtr<FJsonValue>> ToolList = Registry.BuildToolsList();
		TArray<TSharedPtr<FJsonValue>> Values;
		int32 Added = 0;
		for (const TSharedPtr<FJsonValue>& ToolValue : ToolList)
		{
			const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
			if (!ToolObj.IsValid())
			{
				continue;
			}
			FString Name;
			if (!ToolObj->TryGetStringField(TEXT("name"), Name))
			{
				continue;
			}
			const FString NameLower = Name.ToLower();
			if (!Prefix.IsEmpty() && !NameLower.StartsWith(Prefix))
			{
				continue;
			}
			Values.Add(MakeShared<FJsonValueString>(Name));
			++Added;
			if (Added >= 100)
			{
				break;
			}
		}

		// FIXED #5: total 表示匹配候选总数（不截断），与 values（已截断列表）分开统计。
		const int32 TotalMatched = Added >= 100 ? [&]() -> int32
		{
			int32 Count = 0;
			for (const TSharedPtr<FJsonValue>& ToolValue : ToolList)
			{
				const TSharedPtr<FJsonObject> ToolObj = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
				if (!ToolObj.IsValid()) { continue; }
				FString Name;
				if (!ToolObj->TryGetStringField(TEXT("name"), Name)) { continue; }
				if (Prefix.IsEmpty() || Name.ToLower().StartsWith(Prefix)) { ++Count; }
			}
			return Count;
		}() : Added;

		TSharedRef<FJsonObject> Completion = MakeShared<FJsonObject>();
		Completion->SetArrayField(TEXT("values"), Values);
		Completion->SetBoolField(TEXT("hasMore"), Added >= 100);
		Completion->SetNumberField(TEXT("total"), TotalMatched);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("completion"), Completion);
		return ReplyResult(Id, Result);
	}

	void FSololmcpRouter::TickJobs()
	{
		FSololmcpJobService::TickJobs(Registry);
		if (!NotificationSender || SessionsByConnection.Num() == 0)
		{
			return;
		}
		for (TPair<FSololmcpTcpTransport::FConnectionId, FMcpSession>& Pair : SessionsByConnection)
		{
			TArray<TSharedPtr<FJsonObject>> Notifications;
			FSololmcpJobService::CollectProgressNotifications(Pair.Value.LastProgressSeqByJob, Notifications);
			for (const TSharedPtr<FJsonObject>& Payload : Notifications)
			{
				if (Payload.IsValid())
				{
					EmitNotification(Pair.Key, TEXT("notifications/progress"), Payload.ToSharedRef());
				}
			}
			if (Notifications.Num() > 0)
			{
				EmitResourceUpdated(Pair.Key, TEXT("somolmcp://jobs"), TEXT("job_progress"));
				EmitResourceUpdated(Pair.Key, TEXT("somolmcp://status"), TEXT("job_progress"));
			}
		}
	}

	FString FSololmcpRouter::HandleJobsSubmit(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}
		const TSharedRef<FJsonObject> Params = (*ParamsPtr).ToSharedRef();
		const TArray<TSharedPtr<FJsonValue>>* StepsPtr = nullptr;
		if (Params->TryGetArrayField(TEXT("steps"), StepsPtr) && StepsPtr)
		{
			for (const TSharedPtr<FJsonValue>& StepValue : *StepsPtr)
			{
				const TSharedPtr<FJsonObject> StepObj = StepValue.IsValid() ? StepValue->AsObject() : nullptr;
				if (!StepObj.IsValid())
				{
					continue;
				}
				FString ToolName;
				const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
				if (StepObj->TryGetStringField(TEXT("tool"), ToolName)
					&& StepObj->TryGetObjectField(TEXT("arguments"), ArgsPtr)
					&& ArgsPtr && ArgsPtr->IsValid())
				{
					if (IsExternalPythonSurfaceToolName(ToolName))
					{
						TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("reason_code"), TEXT("external_python_surface_hidden"));
						Data->SetStringField(TEXT("execution_mode"), TEXT("queue_only_named_domain_tools"));
						return ReplyError(Id, -32038, TEXT("Python execution tools cannot be submitted as SOMOLMCP jobs."), Data);
					}
					FString RootsError;
					if (!ValidateToolsCallRoots(ConnectionId, ToolName, (*ArgsPtr).ToSharedRef(), RootsError))
					{
						UE_LOG(LogSOMOLMCP, Warning, TEXT("%s"), *RootsError);
						return ReplyError(Id, -32034, RootsError);
					}
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		if (!FSololmcpJobService::SubmitJob(Params, Result, Error))
		{
			return ReplyError(Id, -32602, Error);
		}
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleJobsElicit(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const FMcpSession* Session = SessionsByConnection.Find(ConnectionId);
		if (!Session || !Session->bClientElicitation)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("reason_code"), TEXT("client_elicitation_not_advertised"));
			Data->SetStringField(TEXT("fallback"), TEXT("block_job_with_structured_reason_or_fail_closed"));
			return ReplyError(Id, -32052, TEXT("Client did not advertise elicitation support for this session."), Data);
		}

		TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		if (!Params.IsValid())
		{
			Params = MakeShared<FJsonObject>();
		}

		FString JobId;
		Params->TryGetStringField(TEXT("job_id"), JobId);
		FString Reason;
		if (!Params->TryGetStringField(TEXT("reason"), Reason))
		{
			Params->TryGetStringField(TEXT("message"), Reason);
		}
		if (Reason.IsEmpty())
		{
			Reason = TEXT("Waiting for client elicitation response.");
		}

		FString RequestId;
		if (!SendServerRequest(ConnectionId, TEXT("elicitation/create"), Params.ToSharedRef(), RequestId))
		{
			return ReplyError(Id, -32053, TEXT("Failed to send server elicitation request to client."));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		const bool bOk = JobId.IsEmpty()
			? FSololmcpJobService::CreateBlockedElicitationJob(RequestId, Reason, Result, Error)
			: FSololmcpJobService::BlockJobForElicitation(JobId, RequestId, Reason, Result, Error);
		if (!bOk)
		{
			return ReplyError(Id, -32054, Error);
		}
		Result->SetStringField(TEXT("request_id"), RequestId);
		Result->SetStringField(TEXT("status"), TEXT("blocked"));
		Result->SetStringField(TEXT("poll"), TEXT("server/requests/status"));
		Result->SetStringField(TEXT("resume_method"), TEXT("jobs/resume"));
		EmitResourceUpdated(ConnectionId, TEXT("somolmcp://jobs"), TEXT("job_elicitation_blocked"));
		EmitResourceUpdated(ConnectionId, TEXT("somolmcp://server/requests"), TEXT("server_request_sent"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleJobsResume(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject> Params = GetParamsObject(Request);
		if (!Params.IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}

		FString JobId;
		if (!Params->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.job_id"));
		}
		FString RequestId;
		Params->TryGetStringField(TEXT("request_id"), RequestId);
		const TSharedPtr<FJsonObject>* ResponsePtr = nullptr;
		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		if (Params->TryGetObjectField(TEXT("response"), ResponsePtr) && ResponsePtr && ResponsePtr->IsValid())
		{
			Response = *ResponsePtr;
		}
		else if (Params->HasField(TEXT("result")))
		{
			Response->SetField(TEXT("result"), Params->TryGetField(TEXT("result")));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		if (!FSololmcpJobService::ResumeJobWithElicitation(JobId, RequestId, Response, Result, Error))
		{
			return ReplyError(Id, -32055, Error);
		}
		EmitResourceUpdated(ConnectionId, TEXT("somolmcp://jobs"), TEXT("job_elicitation_resumed"));
		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleJobsGet(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}

		FString JobId;
		if (!(*ParamsPtr)->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.job_id"));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		if (!FSololmcpJobService::GetJob(JobId, Result, Error))
		{
			return ReplyError(Id, -32004, Error);
		}

		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleJobsHeartbeat(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}

		FString JobId;
		if (!(*ParamsPtr)->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.job_id"));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		if (!FSololmcpJobService::HeartbeatExternalJob(JobId, Result, Error))
		{
			return ReplyError(Id, -32004, Error);
		}

		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleJobsAwait(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}

		FString JobId;
		if (!(*ParamsPtr)->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.job_id"));
		}
		int32 TimeoutMs = 60000;
		(*ParamsPtr)->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		if (!FSololmcpJobService::AwaitJob(Registry, JobId, TimeoutMs, Result, Error))
		{
			return ReplyError(Id, -32004, Error);
		}

		return ReplyResult(Id, Result);
	}

	FString FSololmcpRouter::HandleJobsCancel(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
		{
			return ReplyError(Id, -32602, TEXT("Missing params"));
		}
		FString JobId;
		if (!(*ParamsPtr)->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
		{
			return ReplyError(Id, -32602, TEXT("Missing params.job_id"));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		FString Error;
		if (!FSololmcpJobService::CancelJob(JobId, Result, Error))
		{
			return ReplyError(Id, -32004, Error);
		}
		return ReplyResult(Id, Result);
	}

FString FSololmcpRouter::HandleJobsEvents(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Request)
{
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (!Request->TryGetObjectField(TEXT("params"), ParamsPtr) || !ParamsPtr || !ParamsPtr->IsValid())
	{
		return ReplyError(Id, -32602, TEXT("Missing params"));
	}
	FString JobId;
	if (!(*ParamsPtr)->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
	{
		return ReplyError(Id, -32602, TEXT("Missing params.job_id"));
	}
	int32 SinceSeq = 0;
	(*ParamsPtr)->TryGetNumberField(TEXT("since_seq"), SinceSeq);
	int32 WaitMs = 0;
	(*ParamsPtr)->TryGetNumberField(TEXT("wait_ms"), WaitMs);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	FString Error;
	if (!FSololmcpJobService::PollEvents(Registry, JobId, SinceSeq, WaitMs, Result, Error))
	{
		return ReplyError(Id, -32004, Error);
	}
	return ReplyResult(Id, Result);
}

FString FSololmcpRouter::HandleServerStats(const TSharedPtr<FJsonValue>& Id)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	// Transport stats (connections, messages, rejections)
	if (TransportStatsGetter)
	{
		const auto Stats = TransportStatsGetter();
		Result->SetNumberField(TEXT("active_connections"), Stats.ActiveConnections);
		Result->SetNumberField(TEXT("total_accepted"), Stats.TotalAccepted);
		Result->SetNumberField(TEXT("total_rejected"), Stats.TotalRejected);
		Result->SetNumberField(TEXT("total_messages_received"), Stats.TotalMessagesReceived);
		Result->SetNumberField(TEXT("total_messages_sent"), Stats.TotalMessagesSent);
	}

	// Server info
	Result->SetStringField(TEXT("name"), TEXT("SOMOLMCP"));
	Result->SetStringField(TEXT("version"), GetProductVersion());
	Result->SetBoolField(TEXT("running"), true);
	Result->SetStringField(TEXT("transport_protocol"), TEXT("somolmcp.tcp.length_prefixed_jsonrpc.v1"));
	Result->SetStringField(TEXT("frame_format"), TEXT("u32_le_length_prefixed_utf8_json"));
	Result->SetStringField(TEXT("long_task_model"), TEXT("submit_immediate_job_id_poll_jobs_get_with_heartbeat_and_receipt_envelope"));
	Result->SetStringField(TEXT("watchdog_hint"), TEXT("If the port is listening but control-plane requests do not return, inspect modal editor dialogs and record blocked_modal_or_mcp_no_response before retrying once."));
	TSharedRef<FJsonObject> ExecutionPlanning = MakeShared<FJsonObject>();
	ExecutionPlanning->SetBoolField(TEXT("supported"), true);
	ExecutionPlanning->SetStringField(TEXT("schema"), TEXT("somol.mcp_execution_planning:v1"));
	ExecutionPlanning->SetStringField(TEXT("profile_tool"), TEXT("mcp_tool_execution_profile"));
	ExecutionPlanning->SetStringField(TEXT("lock_plan_tool"), TEXT("mcp_resource_lock_plan"));
	ExecutionPlanning->SetStringField(TEXT("parallel_plan_tool"), TEXT("mcp_parallel_authoring_plan"));
	ExecutionPlanning->SetBoolField(TEXT("job_resource_lock_scheduling"), true);
	ExecutionPlanning->SetBoolField(TEXT("job_resource_lock_auto_inference"), true);
	ExecutionPlanning->SetBoolField(TEXT("ue58_preview_required"), false);
	Result->SetObjectField(TEXT("execution_planning"), ExecutionPlanning);
	TArray<FString> ToolNames;
	Registry.GetRegisteredToolNamesSorted(ToolNames);
	Result->SetNumberField(TEXT("registered_tools"), ToolNames.Num());
	TSet<FString> ToolsetNames;
	for (const FString& ToolName : ToolNames)
	{
		ToolsetNames.Add(InferToolsetName(ToolName));
	}
	Result->SetNumberField(TEXT("registered_toolsets"), ToolsetNames.Num());
	TSharedRef<FJsonObject> Jobs = MakeShared<FJsonObject>();
	FSololmcpJobService::BuildMetricsObject(Jobs, ToolNames);
	Result->SetObjectField(TEXT("jobs"), Jobs);

	return ReplyResult(Id, Result);
}
}
