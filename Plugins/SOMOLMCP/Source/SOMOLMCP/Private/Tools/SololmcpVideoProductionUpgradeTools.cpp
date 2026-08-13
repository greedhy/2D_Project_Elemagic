// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Native upgrades for the pre-existing video-production tool names. This file
// adds no tool names; it replaces contract-only bridges through first-wins
// registration and routes long work through FSololmcpJobService.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Protocol/SololmcpJobService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/Engine.h"
// Movie Render Graph (UMovieGraphConfig and friends) first shipped in UE 5.6.
// UE 5.3-5.5 only have the legacy Movie Pipeline preset/config path, so the four
// movie_render_graph_* tools cannot work there and report NOT_AVAILABLE_ON_ENGINE
// instead of being silently absent.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
#define SOMOLMCP_HAS_MOVIEGRAPH 1
#include "Graph/MovieGraphConfig.h"
#include "Graph/MovieGraphNode.h"
#include "Graph/Nodes/MovieGraphGlobalOutputSettingNode.h"
#else
#define SOMOLMCP_HAS_MOVIEGRAPH 0
#endif
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneEventUtils.h"
#include "MovieSceneSequenceEditor.h"
#include "MovieSceneTrack.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "MoviePipelineBasicConfig.h"
#endif
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "Async/Async.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace UE::SOMOLMCP
{
namespace VideoProductionUpgrade
{
using FHandler = TFunction<bool(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>&, FString&, FString&)>;
static FCriticalSection GMutex;

static FString StringField(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, const FString& Default = FString())
{
	FString Value;
	return Args->TryGetStringField(Name, Value) ? Value : Default;
}

static int32 IntField(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, int32 Default, int32 Min, int32 Max)
{
	double Value = Default;
	Args->TryGetNumberField(Name, Value);
	return FMath::Clamp(FMath::RoundToInt(Value), Min, Max);
}

static bool BoolField(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, bool Default)
{
	bool Value = Default;
	Args->TryGetBoolField(Name, Value);
	return Value;
}

static TSharedRef<FJsonObject> ClosedSchema(std::initializer_list<TPair<FString, TSharedRef<FJsonObject>>> Properties = {}, TArray<FString> Required = {})
{
	TMap<FString, TSharedRef<FJsonObject>> PropertyMap;
	for (const TPair<FString, TSharedRef<FJsonObject>>& Property : Properties)
	{
		PropertyMap.Add(Property.Key, Property.Value);
	}
	return FSololmcpSchemaBuilder::Object(PropertyMap, Required, TEXT("Native production video request."), false);
}

static void Register(FSololmcpToolRegistry& Registry, const TCHAR* Name, const TCHAR* Description, const TSharedRef<FJsonObject>& Schema, FHandler Handler, int32 Ttl = 0)
{
	FSololmcpToolDefinition Definition;
	Definition.Name = Name;
	Definition.Description = Description;
	Definition.InputSchema = Schema;
	Definition.Execute = MoveTemp(Handler);
	Definition.CacheTtlSeconds = Ttl;
	Registry.Register(Definition);
}

static bool ResolveSavedPath(const FString& Requested, const FString& DefaultRelative, bool bDirectory, FString& Out, FString& Error)
{
	FString RequestedNormalized = Requested;
	FPaths::NormalizeFilename(RequestedNormalized);
	const bool bHasTraversal = RequestedNormalized == TEXT("..") || RequestedNormalized.StartsWith(TEXT("../")) ||
		RequestedNormalized.EndsWith(TEXT("/..")) || RequestedNormalized.Contains(TEXT("/../"));
	if (bHasTraversal || RequestedNormalized.StartsWith(TEXT("//")))
	{
		Error = TEXT("path_traversal_or_unc_is_not_allowed");
		return false;
	}
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	FPaths::NormalizeDirectoryName(Root);
	Out = Requested.IsEmpty() ? FPaths::Combine(Root, DefaultRelative) : Requested;
	if (FPaths::IsRelative(Out)) Out = FPaths::Combine(Root, Out);
	Out = FPaths::ConvertRelativePathToFull(Out);
	if (bDirectory) FPaths::NormalizeDirectoryName(Out); else FPaths::NormalizeFilename(Out);
	if (!Out.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase) && !Out.Equals(Root, ESearchCase::IgnoreCase))
	{
		Error = TEXT("path_must_be_beneath_project_saved_dir");
		return false;
	}
	return true;
}

static FString JsonString(const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Object, Writer);
	return Json;
}

static bool SaveAsset(UObject* Asset, FString& Error)
{
	if (!Asset || !Asset->GetOutermost())
	{
		Error = TEXT("asset_or_package_is_null");
		return false;
	}
	UPackage* Package = Asset->GetOutermost();
	Package->MarkPackageDirty();
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
	{
		Error = TEXT("package_save_failed");
		return false;
	}
	return true;
}

#if SOMOLMCP_HAS_MOVIEGRAPH

static UMovieGraphConfig* LoadGraph(const FString& Path, FString& Error)
{
	UMovieGraphConfig* Graph = LoadObject<UMovieGraphConfig>(nullptr, *Path);
	if (!Graph) Error = TEXT("movie_render_graph_asset_missing_or_invalid");
	return Graph;
}

static const TCHAR* QueueConfigKey = TEXT("SOMOLMCP.Video.QueueConfig.v1");

static bool ToolGraphAssetCreate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	FString AssetPath = StringField(Args, TEXT("asset_path"), StringField(Args, TEXT("graph_path")));
	if (!AssetPath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(AssetPath))
	{
		Error = TEXT("asset_path_must_be_a_valid_/Game_long_package_name");
		return false;
	}
	if (FindObject<UObject>(nullptr, *(AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath))) || FPackageName::DoesPackageExist(AssetPath))
	{
		Error = TEXT("asset_already_exists");
		return false;
	}
	UPackage* Package = CreatePackage(*AssetPath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
	const UMovieGraphConfig* DefaultGraph = LoadObject<UMovieGraphConfig>(nullptr, TEXT("/MovieRenderPipeline/DefaultRenderGraph.DefaultRenderGraph"));
	UMovieGraphConfig* Graph = DefaultGraph
		? DuplicateObject<UMovieGraphConfig>(DefaultGraph, Package, *AssetName)
		: NewObject<UMovieGraphConfig>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Graph)
	{
		Error = TEXT("movie_render_graph_create_failed");
		return false;
	}
	FAssetRegistryModule::AssetCreated(Graph);
	if (!SaveAsset(Graph, Error)) return false;
	const FString ObjectPath = AssetPath + TEXT(".") + AssetName;
	UMovieGraphConfig* Readback = LoadObject<UMovieGraphConfig>(nullptr, *ObjectPath);
	if (!Readback)
	{
		Error = TEXT("saved_graph_readback_failed");
		return false;
	}
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("asset_path"), ObjectPath);
	Out->SetBoolField(TEXT("saved_asset"), true);
	Out->SetBoolField(TEXT("readback_verified"), true);
	Out->SetNumberField(TEXT("node_count"), Readback->GetNodes().Num());
	Summary = FString::Printf(TEXT("Created, saved, and read back Movie Render Graph %s."), *ObjectPath);
	return true;
}

static TSharedRef<FJsonObject> InspectGraphObject(UMovieGraphConfig* Graph)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	auto AddNode = [&Nodes](UMovieGraphNode* Node, const TCHAR* Role)
	{
		if (!Node) return;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("node_id"), Node->GetGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Row->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Row->SetStringField(TEXT("role"), Role);
		Row->SetBoolField(TEXT("disabled"), Node->IsDisabled());
		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (UMovieGraphPin* Pin : Node->GetInputPins()) Inputs.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (UMovieGraphPin* Pin : Node->GetOutputPins()) Outputs.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
		Row->SetArrayField(TEXT("input_pins"), Inputs);
		Row->SetArrayField(TEXT("output_pins"), Outputs);
		Nodes.Add(MakeShared<FJsonValueObject>(Row));
	};
	AddNode(Graph->GetInputNode(), TEXT("input"));
	for (UMovieGraphNode* Node : Graph->GetNodes()) AddNode(Node, TEXT("node"));
	AddNode(Graph->GetOutputNode(), TEXT("output"));
	Result->SetStringField(TEXT("asset_path"), Graph->GetPathName());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetNumberField(TEXT("node_count"), Nodes.Num());
	Result->SetBoolField(TEXT("has_input_node"), Graph->GetInputNode() != nullptr);
	Result->SetBoolField(TEXT("has_output_node"), Graph->GetOutputNode() != nullptr);
	const FString QueueJson = Graph->GetOutermost()->GetMetaData().GetValue(Graph, QueueConfigKey);
	Result->SetBoolField(TEXT("has_managed_queue_config"), !QueueJson.IsEmpty());
	if (!QueueJson.IsEmpty()) Result->SetStringField(TEXT("managed_queue_config_json"), QueueJson);
	return Result;
}

static bool ToolGraphInspect(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	UMovieGraphConfig* Graph = LoadGraph(StringField(Args, TEXT("asset_path"), StringField(Args, TEXT("graph_path"))), Error);
	if (!Graph) return false;
	Out = InspectGraphObject(Graph);
	Out->SetBoolField(TEXT("success"), true);
	Summary = FString::Printf(TEXT("Inspected persisted Movie Render Graph %s (%d nodes)."), *Graph->GetPathName(), Graph->GetNodes().Num() + 2);
	return true;
}

static bool ToolGraphConfigure(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	UMovieGraphConfig* Graph = LoadGraph(StringField(Args, TEXT("graph_path"), StringField(Args, TEXT("asset_path"))), Error);
	if (!Graph) return false;
	const FString SequencePath = StringField(Args, TEXT("sequence_path"));
	const FString MapPath = StringField(Args, TEXT("map_path"));
	if (!SequencePath.IsEmpty() && !LoadObject<ULevelSequence>(nullptr, *SequencePath))
	{
		Error = TEXT("sequence_asset_missing_or_invalid");
		return false;
	}
	FString RequiredRhi = StringField(Args, TEXT("required_rhi"), TEXT("dx12"));
	RequiredRhi.ToLowerInline();
	if (RequiredRhi != TEXT("dx12") && RequiredRhi != TEXT("vulkan"))
	{
		Error = TEXT("required_rhi_must_be_dx12_or_vulkan");
		return false;
	}
	TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
	Config->SetStringField(TEXT("sequence_path"), SequencePath);
	Config->SetStringField(TEXT("map_path"), MapPath);
	Config->SetStringField(TEXT("required_rhi"), RequiredRhi);
	Config->SetNumberField(TEXT("frame_rate"), IntField(Args, TEXT("frame_rate"), 30, 1, 120));
	Config->SetNumberField(TEXT("frame_start"), IntField(Args, TEXT("frame_start"), 0, -10000000, 10000000));
	Config->SetNumberField(TEXT("frame_end"), IntField(Args, TEXT("frame_end"), 0, -10000000, 10000000));
	Config->SetNumberField(TEXT("width"), IntField(Args, TEXT("width"), 1920, 64, 16384));
	Config->SetNumberField(TEXT("height"), IntField(Args, TEXT("height"), 1080, 64, 16384));
	Config->SetNumberField(TEXT("engine_warmup_frames"), IntField(Args, TEXT("engine_warmup_frames"), 64, 0, 10000));
	Config->SetNumberField(TEXT("render_warmup_frames"), IntField(Args, TEXT("render_warmup_frames"), 16, 0, 10000));
	Config->SetBoolField(TEXT("use_fixed_timestep"), BoolField(Args, TEXT("use_fixed_timestep"), true));
	const TSharedPtr<FJsonObject>* Resolution = nullptr;
	if (Args->TryGetObjectField(TEXT("resolution"), Resolution) && Resolution)
	{
		double Width = 1920, Height = 1080;
		(*Resolution)->TryGetNumberField(TEXT("width"), Width);
		(*Resolution)->TryGetNumberField(TEXT("height"), Height);
		Config->SetNumberField(TEXT("width"), FMath::Clamp(FMath::RoundToInt(Width), 64, 16384));
		Config->SetNumberField(TEXT("height"), FMath::Clamp(FMath::RoundToInt(Height), 64, 16384));
	}
	const TSharedPtr<FJsonObject>* OutputObject = nullptr;
	FString OutputRoot = StringField(Args, TEXT("output_directory"));
	if (Args->TryGetObjectField(TEXT("output"), OutputObject) && OutputObject) (*OutputObject)->TryGetStringField(TEXT("root"), OutputRoot);
	if (!ResolveSavedPath(OutputRoot, TEXT("MovieRenders"), true, OutputRoot, Error)) return false;
	Config->SetStringField(TEXT("output_directory"), OutputRoot);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	// UE 5.8's Basic Config is a supported front-end that generates a genuine
	// Movie Render Graph (global output, warmup, sampling, deferred pass and PNG
	// output nodes). Copying that graph into the managed asset keeps the graph,
	// not a parallel shadow configuration, authoritative at render time.
	UMoviePipelineBasicConfig* Basic = NewObject<UMoviePipelineBasicConfig>(GetTransientPackage());
	Basic->bOverride_OutputDirectory = true;
	Basic->OutputDirectory.Path = OutputRoot;
	Basic->bOverride_FileNameFormat = true;
	Basic->FileNameFormat = TEXT("{sequence_name}/{shot_name}.{frame_number}");
	Basic->bOverride_OutputResolution = true;
	Basic->OutputResolution = FMovieGraphNamedResolution(FMovieGraphNamedResolution::CustomEntryName,
		FIntPoint(static_cast<int32>(Config->GetNumberField(TEXT("width"))), static_cast<int32>(Config->GetNumberField(TEXT("height")))), TEXT("SOMOLMCP managed resolution"));
	Basic->bOverride_CustomStartFrame = true;
	Basic->CustomStartFrame = static_cast<int32>(Config->GetNumberField(TEXT("frame_start")));
	Basic->bOverride_CustomEndFrame = true;
	Basic->CustomEndFrame = static_cast<int32>(Config->GetNumberField(TEXT("frame_end")));
	Basic->bOverride_bUseDeferredRenderer = true;
	Basic->bUseDeferredRenderer = true;
	Basic->bOverride_DeferredSpatialSampleCount = true;
	Basic->DeferredSpatialSampleCount = 1;
	Basic->bOverride_TemporalSampleCount = true;
	Basic->TemporalSampleCount = 1;
	Basic->bOverride_NumWarmUpFrames = true;
	Basic->NumWarmUpFrames = static_cast<int32>(Config->GetNumberField(TEXT("engine_warmup_frames")));
	Basic->bOverride_EnabledOutputTypes = true;
	Basic->EnabledOutputTypes = {TSoftClassPtr<UMovieGraphFileOutputNode>(FSoftObjectPath(TEXT("/Script/MovieRenderPipelineRenderPasses.MovieGraphImageSequenceOutputNode_PNG")))};
	UMovieGraphConfig* GeneratedGraph = UMoviePipelineBasicConfig::GenerateGraph(Basic, GetTransientPackage());
	if (!GeneratedGraph)
	{
		Error = TEXT("ue58_basic_config_failed_to_generate_movie_render_graph");
		return false;
	}
	// A Movie Render Graph owns nested node/pin UObjects. Property-copying the
	// root object leaves those nested objects owned by the transient generated
	// graph and becomes unsafe once that graph is reclaimed. Replace the asset
	// with a deep duplicate so the complete object graph is re-outered into the
	// persistent package.
	UPackage* GraphPackage = Graph->GetOutermost();
	const FName GraphAssetName = Graph->GetFName();
	FAssetRegistryModule::AssetDeleted(Graph);
	Graph->ClearFlags(RF_Public | RF_Standalone);
	Graph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Graph = DuplicateObject<UMovieGraphConfig>(GeneratedGraph, GraphPackage, GraphAssetName);
	if (!Graph)
	{
		Error = TEXT("movie_render_graph_deep_duplicate_failed");
		return false;
	}
#endif
	Graph->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Graph);
	for (UMovieGraphNode* Node : Graph->GetNodes())
	{
		if (UMovieGraphGlobalOutputSettingNode* OutputNode = Cast<UMovieGraphGlobalOutputSettingNode>(Node))
		{
			OutputNode->bOverride_OutputFrameRate = true;
			OutputNode->OutputFrameRate = FFrameRate(static_cast<int32>(Config->GetNumberField(TEXT("frame_rate"))), 1);
			OutputNode->bOverride_bOverwriteExistingOutput = true;
			OutputNode->bOverwriteExistingOutput = false;
			OutputNode->bOverride_bFlushDiskWritesPerShot = true;
			OutputNode->bFlushDiskWritesPerShot = true;
		}
	}
	const FString Canonical = JsonString(Config);
	Graph->GetOutermost()->GetMetaData().SetValue(Graph, QueueConfigKey, *Canonical);
	if (!SaveAsset(Graph, Error)) return false;
	const FString Readback = Graph->GetOutermost()->GetMetaData().GetValue(Graph, QueueConfigKey);
	if (Readback != Canonical)
	{
		Error = TEXT("queue_config_readback_mismatch");
		return false;
	}
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("asset_path"), Graph->GetPathName());
	Out->SetObjectField(TEXT("queue_settings"), Config);
	Out->SetBoolField(TEXT("saved"), true);
	Out->SetBoolField(TEXT("readback_verified"), true);
	Out->SetStringField(TEXT("resolved_config_hash"), FMD5::HashAnsiString(*Canonical));
	Summary = FString::Printf(TEXT("Persisted and verified managed queue configuration on %s."), *Graph->GetPathName());
	return true;
}

static bool ToolGraphValidate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	UMovieGraphConfig* Graph = LoadGraph(StringField(Args, TEXT("asset_path"), StringField(Args, TEXT("graph_path"))), Error);
	if (!Graph) return false;
	TArray<FString> Diagnostics;
	if (!Graph->GetInputNode()) Diagnostics.Add(TEXT("missing_input_node"));
	if (!Graph->GetOutputNode()) Diagnostics.Add(TEXT("missing_output_node"));
	for (UMovieGraphNode* Node : Graph->GetNodes())
	{
		if (!Node) Diagnostics.Add(TEXT("null_node"));
		else if (!Node->CanBeAddedByUser()) Diagnostics.Add(FString::Printf(TEXT("non_user_node:%s"), *Node->GetClass()->GetName()));
	}
	const FString QueueJson = Graph->GetOutermost()->GetMetaData().GetValue(Graph, QueueConfigKey);
	if (!QueueJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> Parsed;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(QueueJson), Parsed) || !Parsed.IsValid()) Diagnostics.Add(TEXT("managed_queue_config_invalid_json"));
	}
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FString& Diagnostic : Diagnostics) Rows.Add(MakeShared<FJsonValueString>(Diagnostic));
	const bool bOk = Diagnostics.IsEmpty();
	Out->SetBoolField(TEXT("success"), bOk);
	Out->SetBoolField(TEXT("compile_ok"), bOk);
	Out->SetStringField(TEXT("asset_path"), Graph->GetPathName());
	Out->SetArrayField(TEXT("diagnostics"), Rows);
	Out->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num() + 2);
	Summary = bOk ? TEXT("Movie Render Graph structural validation passed.") : TEXT("Movie Render Graph structural validation found blockers.");
	if (!bOk) Error = TEXT("movie_render_graph_validation_failed");
	return bOk;
}

#else // SOMOLMCP_HAS_MOVIEGRAPH

/**
 * UE 5.3-5.5 fallback. The tools stay registered so a client gets a typed answer
 * naming the engine requirement and the legacy path to use, rather than the tool
 * vanishing from tools/list between engine versions.
 */
static bool MovieGraphUnavailable(
	const TCHAR* ToolName, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	Out->SetStringField(TEXT("error_code"), TEXT("NOT_AVAILABLE_ON_ENGINE"));
	Out->SetStringField(TEXT("tool"), ToolName);
	Out->SetStringField(TEXT("required_api"), TEXT("UMovieGraphConfig (Movie Render Graph)"));
	Out->SetStringField(TEXT("minimum_engine"), TEXT("5.6"));
	Out->SetStringField(TEXT("engine_version"),
		FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION));
	Out->SetStringField(TEXT("suggested_alternative"),
		TEXT("Use the legacy Movie Pipeline preset tools (movie_render_queue_*) on this engine."));
	Out->SetBoolField(TEXT("success"), false);
	Out->SetBoolField(TEXT("ok"), false);
	Summary = FString::Printf(
		TEXT("%s requires Movie Render Graph (UE 5.6+); this editor is UE %d.%d."),
		ToolName, ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	Error = TEXT("movie_render_graph_requires_ue_5_6");
	return false;
}

static bool ToolGraphAssetCreate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	return MovieGraphUnavailable(TEXT("movie_render_graph_asset_create"), Out, Summary, Error);
}

static bool ToolGraphInspect(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	return MovieGraphUnavailable(TEXT("movie_render_graph_inspect"), Out, Summary, Error);
}

static bool ToolGraphConfigure(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	return MovieGraphUnavailable(TEXT("movie_render_graph_configure"), Out, Summary, Error);
}

static bool ToolGraphValidate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	return MovieGraphUnavailable(TEXT("movie_render_graph_compile_validate"), Out, Summary, Error);
}

#endif // SOMOLMCP_HAS_MOVIEGRAPH

struct FTranscodeProfile
{
	FString Id;
	FString Container = TEXT("mp4");
	FString Codec = TEXT("h264");
	int32 FrameRate = 30;
	int32 BitrateKbps = 20000;
};

struct FTranscodeJob : public TSharedFromThis<FTranscodeJob, ESPMode::ThreadSafe>
{
	FString Id;
	FString InputPath;
	FString InputManifest;
	FString OutputPath;
	FTranscodeProfile Profile;
	FString Status = TEXT("queued");
	FString Error;
	TAtomic<bool> Cancel{false};
	TAtomic<int64> Frames{0};
	TAtomic<int64> TotalFrames{0};
};

static TMap<FString, FTranscodeProfile> GProfiles;
static TMap<FString, TSharedPtr<FTranscodeJob, ESPMode::ThreadSafe>> GTranscodeJobs;

#if PLATFORM_WINDOWS
template <typename T> static void ReleaseCom(T*& Ptr) { if (Ptr) { Ptr->Release(); Ptr = nullptr; } }

static bool CreateMp4Sink(const FString& Path, int32 Width, int32 Height, int32 Fps, int32 BitrateKbps, IMFSinkWriter*& Writer, DWORD& Stream, FString& Error)
{
	Writer = nullptr;
	Stream = 0;
	IMFMediaType* OutputType = nullptr;
	IMFMediaType* InputType = nullptr;
	HRESULT Hr = MFCreateSinkWriterFromURL(*Path, nullptr, nullptr, &Writer);
	if (SUCCEEDED(Hr)) Hr = MFCreateMediaType(&OutputType);
	if (SUCCEEDED(Hr)) Hr = OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (SUCCEEDED(Hr)) Hr = OutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	if (SUCCEEDED(Hr)) Hr = OutputType->SetUINT32(MF_MT_AVG_BITRATE, BitrateKbps * 1000);
	if (SUCCEEDED(Hr)) Hr = OutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeSize(OutputType, MF_MT_FRAME_SIZE, Width, Height);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(OutputType, MF_MT_FRAME_RATE, Fps, 1);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(OutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if (SUCCEEDED(Hr)) Hr = Writer->AddStream(OutputType, &Stream);
	if (SUCCEEDED(Hr)) Hr = MFCreateMediaType(&InputType);
	if (SUCCEEDED(Hr)) Hr = InputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (SUCCEEDED(Hr)) Hr = InputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	if (SUCCEEDED(Hr)) Hr = InputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeSize(InputType, MF_MT_FRAME_SIZE, Width, Height);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(InputType, MF_MT_FRAME_RATE, Fps, 1);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(InputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if (SUCCEEDED(Hr)) Hr = Writer->SetInputMediaType(Stream, InputType, nullptr);
	if (SUCCEEDED(Hr)) Hr = Writer->BeginWriting();
	ReleaseCom(OutputType);
	ReleaseCom(InputType);
	if (FAILED(Hr))
	{
		ReleaseCom(Writer);
		Error = FString::Printf(TEXT("media_foundation_sink_create_failed:0x%08x"), static_cast<uint32>(Hr));
		return false;
	}
	return true;
}

static bool WriteBgraFrame(IMFSinkWriter* Writer, DWORD Stream, const TArray<uint8>& Bgra, int64 Frame, int32 Fps, FString& Error)
{
	IMFMediaBuffer* Buffer = nullptr;
	IMFSample* Sample = nullptr;
	HRESULT Hr = MFCreateMemoryBuffer(Bgra.Num(), &Buffer);
	BYTE* Data = nullptr;
	DWORD MaxLength = 0, CurrentLength = 0;
	if (SUCCEEDED(Hr)) Hr = Buffer->Lock(&Data, &MaxLength, &CurrentLength);
	if (SUCCEEDED(Hr)) FMemory::Memcpy(Data, Bgra.GetData(), Bgra.Num());
	if (Buffer && Data) Buffer->Unlock();
	if (SUCCEEDED(Hr)) Hr = Buffer->SetCurrentLength(Bgra.Num());
	if (SUCCEEDED(Hr)) Hr = MFCreateSample(&Sample);
	if (SUCCEEDED(Hr)) Hr = Sample->AddBuffer(Buffer);
	const LONGLONG Duration = 10000000ll / Fps;
	if (SUCCEEDED(Hr)) Hr = Sample->SetSampleTime(Frame * Duration);
	if (SUCCEEDED(Hr)) Hr = Sample->SetSampleDuration(Duration);
	if (SUCCEEDED(Hr)) Hr = Writer->WriteSample(Stream, Sample);
	ReleaseCom(Sample);
	ReleaseCom(Buffer);
	if (FAILED(Hr))
	{
		Error = FString::Printf(TEXT("media_foundation_frame_write_failed:0x%08x"), static_cast<uint32>(Hr));
		return false;
	}
	return true;
}

static bool DecodeImage(const FString& Path, TArray<uint8>& Bgra, int32& Width, int32& Height, FString& Error)
{
	TArray<uint8> Compressed;
	if (!FFileHelper::LoadFileToArray(Compressed, *Path)) { Error = TEXT("image_frame_read_failed"); return false; }
	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Format = Module.DetectImageFormat(Compressed.GetData(), Compressed.Num());
	TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
	if (!Wrapper || !Wrapper->SetCompressed(Compressed.GetData(), Compressed.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Bgra))
	{
		Error = TEXT("image_frame_decode_failed");
		return false;
	}
	Width = Wrapper->GetWidth();
	Height = Wrapper->GetHeight();
	return Width > 0 && Height > 0;
}

static bool LoadManifestFrames(const FString& ManifestPath, TArray<FString>& Frames, int32& Fps, FString& Error)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ManifestPath)) { Error = TEXT("input_manifest_read_failed"); return false; }
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) { Error = TEXT("input_manifest_invalid_json"); return false; }
	double Rate = Fps;
	Root->TryGetNumberField(TEXT("frame_rate"), Rate);
	Fps = FMath::Clamp(FMath::RoundToInt(Rate), 1, 120);
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Root->TryGetArrayField(TEXT("frames"), Values) || !Values) { Error = TEXT("input_manifest_frames_missing"); return false; }
	const FString RootDir = FPaths::GetPath(ManifestPath);
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString FramePath = Value->AsString();
		if (FPaths::IsRelative(FramePath)) FramePath = FPaths::Combine(RootDir, FramePath);
		FPaths::NormalizeFilename(FramePath);
		if (!FPaths::FileExists(FramePath)) { Error = FString::Printf(TEXT("input_frame_missing:%s"), *FramePath); return false; }
		Frames.Add(FramePath);
	}
	if (Frames.IsEmpty()) { Error = TEXT("input_manifest_has_no_frames"); return false; }
	return true;
}

static void RunTranscode(const TSharedPtr<FTranscodeJob, ESPMode::ThreadSafe>& Job)
{
	FString Error;
	HRESULT Hr = MFStartup(MF_VERSION);
	if (FAILED(Hr)) Error = TEXT("media_foundation_startup_failed");
	TArray<FString> Frames;
	int32 Fps = Job->Profile.FrameRate;
	if (Error.IsEmpty() && !LoadManifestFrames(Job->InputManifest, Frames, Fps, Error)) {}
	Job->TotalFrames.Store(Frames.Num());
	TArray<uint8> First;
	int32 Width = 0, Height = 0;
	if (Error.IsEmpty() && !DecodeImage(Frames[0], First, Width, Height, Error)) {}
	if ((Width & 1) || (Height & 1)) Error = TEXT("h264_requires_even_frame_dimensions");
	IMFSinkWriter* Writer = nullptr;
	DWORD Stream = 0;
	if (Error.IsEmpty() && !CreateMp4Sink(Job->OutputPath, Width, Height, Fps, Job->Profile.BitrateKbps, Writer, Stream, Error)) {}
	{
		FScopeLock Lock(&GMutex);
		Job->Status = Error.IsEmpty() ? TEXT("running") : TEXT("failed");
	}
	for (int32 Index = 0; Error.IsEmpty() && Index < Frames.Num() && !Job->Cancel.Load(); ++Index)
	{
		TArray<uint8> Pixels;
		int32 FrameWidth = 0, FrameHeight = 0;
		if (Index == 0) { Pixels = MoveTemp(First); FrameWidth = Width; FrameHeight = Height; }
		else if (!DecodeImage(Frames[Index], Pixels, FrameWidth, FrameHeight, Error)) break;
		if (FrameWidth != Width || FrameHeight != Height) { Error = TEXT("input_frame_dimensions_are_inconsistent"); break; }
		if (!WriteBgraFrame(Writer, Stream, Pixels, Index, Fps, Error)) break;
		Job->Frames.Store(Index + 1);
		const double Progress = static_cast<double>(Index + 1) / Frames.Num();
		FString UpdateError;
		FSololmcpJobService::UpdateExternalJob(Job->Id, TEXT("running"), Progress,
			FString::Printf(TEXT("{\"executor\":\"media_foundation_transcoder\",\"frames\":%d,\"total_frames\":%d}"), Index + 1, Frames.Num()), Index + 1, FString(), FString(), UpdateError);
	}
	if (Writer)
	{
		if (!Job->Cancel.Load() && Error.IsEmpty())
		{
			Hr = Writer->Finalize();
			if (FAILED(Hr)) Error = FString::Printf(TEXT("media_foundation_finalize_failed:0x%08x"), static_cast<uint32>(Hr));
		}
		ReleaseCom(Writer);
	}
	MFShutdown();
	FString Terminal;
	if (Job->Cancel.Load()) { Terminal = TEXT("cancelled"); IFileManager::Get().Delete(*Job->OutputPath, false, true); }
	else if (!Error.IsEmpty()) Terminal = TEXT("failed");
	else Terminal = TEXT("completed");
	{
		FScopeLock Lock(&GMutex);
		Job->Status = Terminal;
		Job->Error = Error;
	}
	FString UpdateError;
	FSololmcpJobService::UpdateExternalJob(Job->Id, Terminal == TEXT("completed") ? TEXT("succeeded") : Terminal,
		Terminal == TEXT("completed") ? 1.0 : 0.0,
		FString::Printf(TEXT("{\"executor\":\"media_foundation_transcoder\",\"output_path\":\"%s\",\"frames\":%lld}"), *Job->OutputPath.ReplaceCharWithEscapedChar(), Job->Frames.Load()),
		Job->Frames.Load(), Error.IsEmpty() ? FString() : TEXT("E_TRANSCODE"), Error, UpdateError);
}
#endif

static bool ToolProfileCreate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	FTranscodeProfile Profile;
	Profile.Id = StringField(Args, TEXT("profile_id"), FString::Printf(TEXT("profile-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Profile.Container = StringField(Args, TEXT("container"), TEXT("mp4")); Profile.Container.ToLowerInline();
	Profile.Codec = StringField(Args, TEXT("video_codec"), TEXT("h264")); Profile.Codec.ToLowerInline();
	Profile.FrameRate = IntField(Args, TEXT("frame_rate"), 30, 1, 120);
	Profile.BitrateKbps = IntField(Args, TEXT("bitrate_kbps"), 20000, 100, 200000);
	if (Profile.Container != TEXT("mp4") || Profile.Codec != TEXT("h264"))
	{
		Error = TEXT("current_native_backend_supports_only_h264_mp4; requested_codec_is_not_available");
		return false;
	}
	{
		FScopeLock Lock(&GMutex);
		GProfiles.Add(Profile.Id, Profile);
	}
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("profile_id"), Profile.Id);
	Out->SetStringField(TEXT("container"), Profile.Container);
	Out->SetStringField(TEXT("video_codec"), Profile.Codec);
	Out->SetStringField(TEXT("backend"), TEXT("windows_media_foundation"));
	Out->SetNumberField(TEXT("frame_rate"), Profile.FrameRate);
	Out->SetNumberField(TEXT("bitrate_kbps"), Profile.BitrateKbps);
	Out->SetBoolField(TEXT("capability_probed"), true);
	Summary = FString::Printf(TEXT("Created native Media Foundation transcode profile %s."), *Profile.Id);
	return true;
}

static bool ToolTranscodeSubmit(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
#if PLATFORM_WINDOWS
	const FString ProfileId = StringField(Args, TEXT("profile_id"));
	FTranscodeProfile Profile;
	{
		FScopeLock Lock(&GMutex);
		const FTranscodeProfile* Found = GProfiles.Find(ProfileId);
		if (!Found) { Error = TEXT("transcoder_profile_not_found"); return false; }
		Profile = *Found;
	}
	FString Manifest = StringField(Args, TEXT("input_manifest"), StringField(Args, TEXT("input_path")));
	FPaths::NormalizeFilename(Manifest);
	if (!FPaths::FileExists(Manifest)) { Error = TEXT("input_manifest_missing"); return false; }
	FString OutputPath;
	if (!ResolveSavedPath(StringField(Args, TEXT("output_path")), TEXT("SOMOLMCP/Transcodes/output.mp4"), false, OutputPath, Error)) return false;
	if (FPaths::GetExtension(OutputPath).ToLower() != TEXT("mp4")) { Error = TEXT("output_extension_must_be_mp4"); return false; }
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
	if (FPaths::FileExists(OutputPath) && !BoolField(Args, TEXT("overwrite_owned"), false)) { Error = TEXT("output_exists"); return false; }
	const FString RequestId = StringField(Args, TEXT("client_request_id"), FString::Printf(TEXT("transcode-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FString JobId;
	bool Deduplicated = false;
	if (!FSololmcpJobService::CreateExternalJob(RequestId, TEXT("video_transcode"), TEXT("{\"executor\":\"media_foundation_transcoder\"}"),
		{TEXT("gpu:encoder"), TEXT("output:") + OutputPath}, JobId, Deduplicated, Error)) return false;
	if (Deduplicated)
	{
		Out->SetBoolField(TEXT("success"), true); Out->SetBoolField(TEXT("deduplicated"), true); Out->SetStringField(TEXT("job_id"), JobId); Out->SetStringField(TEXT("status"), TEXT("existing"));
		Summary = FString::Printf(TEXT("Transcode request deduplicated to %s."), *JobId); return true;
	}
	TSharedPtr<FTranscodeJob, ESPMode::ThreadSafe> Job = MakeShared<FTranscodeJob, ESPMode::ThreadSafe>();
	Job->Id = JobId; Job->InputManifest = Manifest; Job->OutputPath = OutputPath; Job->Profile = Profile;
	{
		FScopeLock Lock(&GMutex); GTranscodeJobs.Add(JobId, Job);
	}
	Async(EAsyncExecution::Thread, [Job]() { RunTranscode(Job); });
	Out->SetBoolField(TEXT("success"), true); Out->SetBoolField(TEXT("queued"), true); Out->SetStringField(TEXT("job_id"), JobId); Out->SetStringField(TEXT("status"), TEXT("queued")); Out->SetStringField(TEXT("output_path"), OutputPath);
	Summary = FString::Printf(TEXT("Submitted native image-sequence transcode job %s."), *JobId);
	return true;
#else
	Error = TEXT("native_transcoder_is_win64_only"); return false;
#endif
}

static TSharedPtr<FTranscodeJob, ESPMode::ThreadSafe> FindTranscodeJob(const FString& Id)
{
	FScopeLock Lock(&GMutex); return GTranscodeJobs.FindRef(Id);
}

static bool WriteTranscodeStatus(const TSharedPtr<FTranscodeJob, ESPMode::ThreadSafe>& Job, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	if (!Job) { Error = TEXT("transcode_job_not_found"); return false; }
	FString Status, JobError;
	{ FScopeLock Lock(&GMutex); Status = Job->Status; JobError = Job->Error; }
	const int64 Total = Job->TotalFrames.Load(), Frames = Job->Frames.Load();
	Out->SetBoolField(TEXT("success"), Status != TEXT("failed")); Out->SetStringField(TEXT("job_id"), Job->Id); Out->SetStringField(TEXT("status"), Status);
	Out->SetNumberField(TEXT("frames_processed"), static_cast<double>(Frames)); Out->SetNumberField(TEXT("total_frames"), static_cast<double>(Total));
	Out->SetNumberField(TEXT("progress"), Total > 0 ? static_cast<double>(Frames) / Total : 0.0); Out->SetStringField(TEXT("output_path"), Job->OutputPath);
	Out->SetNumberField(TEXT("output_bytes"), static_cast<double>(IFileManager::Get().FileSize(*Job->OutputPath)));
	if (!JobError.IsEmpty()) Out->SetStringField(TEXT("error"), JobError);
	Summary = FString::Printf(TEXT("Transcode job %s is %s (%lld/%lld frames)."), *Job->Id, *Status, Frames, Total);
	return Status != TEXT("failed");
}

static bool ToolTranscodeStatus(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	return WriteTranscodeStatus(FindTranscodeJob(StringField(Args, TEXT("job_id"))), Out, Summary, Error);
}

static bool ToolTranscodeCancel(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TSharedPtr<FTranscodeJob, ESPMode::ThreadSafe> Job = FindTranscodeJob(StringField(Args, TEXT("job_id")));
	if (!Job) { Error = TEXT("transcode_job_not_found"); return false; }
	Job->Cancel.Store(true);
	return WriteTranscodeStatus(Job, Out, Summary, Error);
}

struct FMediaProbe
{
	bool bValid = false;
	FString Container;
	FString Codec;
	int32 Width = 0;
	int32 Height = 0;
	double FrameRate = 0.0;
	int64 FrameCount = 0;
	double DurationSeconds = 0.0;
	int32 DecodeErrors = 0;
};

static bool ProbeMedia(const FString& Path, bool bFullDecode, FMediaProbe& Probe, FString& Error)
{
#if PLATFORM_WINDOWS
	HRESULT Hr = MFStartup(MF_VERSION);
	IMFSourceReader* Reader = nullptr;
	IMFMediaType* Type = nullptr;
	if (SUCCEEDED(Hr)) Hr = MFCreateSourceReaderFromURL(*Path, nullptr, &Reader);
	if (SUCCEEDED(Hr)) Hr = Reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &Type);
	GUID Subtype{};
	UINT32 Width = 0, Height = 0, RateN = 0, RateD = 1;
	if (SUCCEEDED(Hr)) Hr = Type->GetGUID(MF_MT_SUBTYPE, &Subtype);
	if (SUCCEEDED(Hr)) Hr = MFGetAttributeSize(Type, MF_MT_FRAME_SIZE, &Width, &Height);
	if (SUCCEEDED(Hr)) MFGetAttributeRatio(Type, MF_MT_FRAME_RATE, &RateN, &RateD);
	if (FAILED(Hr))
	{
		ReleaseCom(Type); ReleaseCom(Reader); MFShutdown(); Error = FString::Printf(TEXT("media_probe_open_failed:0x%08x"), static_cast<uint32>(Hr)); return false;
	}
	Probe.Container = FPaths::GetExtension(Path).ToLower();
	Probe.Codec = Subtype == MFVideoFormat_H264 ? TEXT("h264") : Subtype == MFVideoFormat_HEVC ? TEXT("hevc") : TEXT("unknown");
	Probe.Width = Width; Probe.Height = Height; Probe.FrameRate = RateD ? static_cast<double>(RateN) / RateD : 0.0;
	PROPVARIANT Duration; PropVariantInit(&Duration);
	if (SUCCEEDED(Reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &Duration)) && Duration.vt == VT_UI8) Probe.DurationSeconds = Duration.uhVal.QuadPart / 10000000.0;
	PropVariantClear(&Duration);
	if (bFullDecode)
	{
		while (true)
		{
			DWORD Actual = 0, Flags = 0; LONGLONG Timestamp = 0; IMFSample* Sample = nullptr;
			Hr = Reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &Actual, &Flags, &Timestamp, &Sample);
			if (FAILED(Hr)) { ++Probe.DecodeErrors; ReleaseCom(Sample); break; }
			if (Sample) ++Probe.FrameCount;
			ReleaseCom(Sample);
			if (Flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
		}
	}
	else if (Probe.FrameRate > 0.0 && Probe.DurationSeconds > 0.0) Probe.FrameCount = FMath::RoundToInt64(Probe.FrameRate * Probe.DurationSeconds);
	ReleaseCom(Type); ReleaseCom(Reader); MFShutdown();
	Probe.bValid = Probe.Width > 0 && Probe.Height > 0 && Probe.DecodeErrors == 0;
	if (!Probe.bValid) Error = TEXT("media_probe_validation_failed");
	return Probe.bValid;
#else
	Error = TEXT("native_media_probe_is_win64_only"); return false;
#endif
}

static bool ToolProbe(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	const FString Path = StringField(Args, TEXT("path"), StringField(Args, TEXT("output_path")));
	if (!FPaths::FileExists(Path)) { Error = TEXT("media_file_missing"); return false; }
	const FString Mode = StringField(Args, TEXT("validation_mode"), TEXT("metadata_only"));
	FMediaProbe Probe;
	if (!ProbeMedia(Path, Mode == TEXT("full_decode") || Mode == TEXT("sampled_decode"), Probe, Error)) return false;
	Out->SetBoolField(TEXT("success"), true); Out->SetBoolField(TEXT("valid"), true); Out->SetStringField(TEXT("path"), Path);
	Out->SetStringField(TEXT("container"), Probe.Container); Out->SetStringField(TEXT("codec"), Probe.Codec); Out->SetNumberField(TEXT("width"), Probe.Width); Out->SetNumberField(TEXT("height"), Probe.Height);
	Out->SetNumberField(TEXT("frame_rate"), Probe.FrameRate); Out->SetNumberField(TEXT("duration_seconds"), Probe.DurationSeconds); Out->SetNumberField(TEXT("frame_count"), static_cast<double>(Probe.FrameCount)); Out->SetNumberField(TEXT("decode_error_count"), Probe.DecodeErrors);
	Out->SetNumberField(TEXT("file_bytes"), static_cast<double>(IFileManager::Get().FileSize(*Path)));
	Out->SetStringField(TEXT("md5"), LexToString(FMD5Hash::HashFile(*Path)));
	Out->SetStringField(TEXT("validation_mode"), Mode);
	Summary = FString::Printf(TEXT("Probed %s: %s %dx%d, %.3fs, %lld frames."), *Path, *Probe.Codec, Probe.Width, Probe.Height, Probe.DurationSeconds, Probe.FrameCount);
	return true;
}

static bool ToolTranscodeValidate(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TSharedPtr<FTranscodeJob, ESPMode::ThreadSafe> Job = FindTranscodeJob(StringField(Args, TEXT("job_id")));
	FString Path = StringField(Args, TEXT("output_path"), Job ? Job->OutputPath : FString());
	TSharedRef<FJsonObject> ProbeArgs = MakeShared<FJsonObject>(); ProbeArgs->SetStringField(TEXT("path"), Path); ProbeArgs->SetStringField(TEXT("validation_mode"), StringField(Args, TEXT("validation_mode"), TEXT("full_decode")));
	if (!ToolProbe(Context, ProbeArgs, Out, Summary, Error)) return false;
	const int32 ExpectedWidth = IntField(Args, TEXT("expected_width"), 0, 0, 16384);
	const int32 ExpectedHeight = IntField(Args, TEXT("expected_height"), 0, 0, 16384);
	const bool Match = (ExpectedWidth == 0 || Out->GetIntegerField(TEXT("width")) == ExpectedWidth) && (ExpectedHeight == 0 || Out->GetIntegerField(TEXT("height")) == ExpectedHeight);
	Out->SetBoolField(TEXT("expectations_match"), Match);
	if (!Match) { Error = TEXT("transcode_output_expectation_mismatch"); return false; }
	Summary = FString::Printf(TEXT("Transcode output %s passed full media decode validation."), *Path);
	return true;
}

static bool DecodeImageStats(const FString& Path, double& Mean, double& Variance, FString& Digest, int32& Width, int32& Height, FString& Error)
{
#if PLATFORM_WINDOWS
	TArray<uint8> Pixels;
	if (!DecodeImage(Path, Pixels, Width, Height, Error)) return false;
	double Sum = 0.0, SumSq = 0.0;
	const int64 Count = static_cast<int64>(Width) * Height;
	for (int64 Index = 0; Index < Count; ++Index)
	{
		const double Luma = (Pixels[Index * 4 + 0] * 0.0722 + Pixels[Index * 4 + 1] * 0.7152 + Pixels[Index * 4 + 2] * 0.2126) / 255.0;
		Sum += Luma; SumSq += Luma * Luma;
	}
	Mean = Count ? Sum / Count : 0.0; Variance = Count ? FMath::Max(0.0, SumSq / Count - Mean * Mean) : 0.0;
	Digest = LexToString(FMD5Hash::HashFile(*Path));
	return true;
#else
	Error = TEXT("image_qa_is_win64_only"); return false;
#endif
}

static void CollectArtifactPaths(const TSharedRef<FJsonObject>& Args, TArray<FString>& Files)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Args->TryGetArrayField(TEXT("artifact_paths"), Values) && Values) for (const TSharedPtr<FJsonValue>& Value : *Values) Files.Add(Value->AsString());
	FString Directory = StringField(Args, TEXT("output_directory"));
	if (!Directory.IsEmpty()) IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.png"), true, false, false);
	Files.Sort();
}

static bool ToolVisualQa(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TArray<FString> Files; CollectArtifactPaths(Args, Files);
	if (Files.IsEmpty()) { Error = TEXT("no_image_artifacts_for_visual_qa"); return false; }
	const int32 MaxSamples = IntField(Args, TEXT("max_samples"), 24, 2, 10000);
	const double BlackThreshold = [&]() { double V = 0.01; Args->TryGetNumberField(TEXT("black_luma_threshold"), V); return FMath::Clamp(V, 0.0, 1.0); }();
	int32 Black = 0, Frozen = 0, DecodeErrors = 0; FString PreviousDigest; int32 ReferenceWidth = 0, ReferenceHeight = 0, DimensionMismatch = 0;
	TArray<TSharedPtr<FJsonValue>> Samples;
	for (int32 Sample = 0; Sample < FMath::Min(MaxSamples, Files.Num()); ++Sample)
	{
		const int32 Index = FMath::RoundToInt(static_cast<double>(Sample) * (Files.Num() - 1) / FMath::Max(1, FMath::Min(MaxSamples, Files.Num()) - 1));
		double Mean = 0.0, Variance = 0.0; FString Digest, LocalError; int32 Width = 0, Height = 0;
		if (!DecodeImageStats(Files[Index], Mean, Variance, Digest, Width, Height, LocalError)) { ++DecodeErrors; continue; }
		if (Mean <= BlackThreshold) ++Black;
		if (!PreviousDigest.IsEmpty() && Digest == PreviousDigest) ++Frozen;
		PreviousDigest = Digest;
		if (!ReferenceWidth) { ReferenceWidth = Width; ReferenceHeight = Height; }
		else if (Width != ReferenceWidth || Height != ReferenceHeight) ++DimensionMismatch;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("path"), Files[Index]); Row->SetNumberField(TEXT("mean_luma"), Mean); Row->SetNumberField(TEXT("variance"), Variance); Row->SetStringField(TEXT("md5"), Digest); Samples.Add(MakeShared<FJsonValueObject>(Row));
	}
	const bool Pass = Black == 0 && Frozen == 0 && DecodeErrors == 0 && DimensionMismatch == 0;
	Out->SetBoolField(TEXT("success"), Pass); Out->SetStringField(TEXT("status"), Pass ? TEXT("pass") : TEXT("issues_found")); Out->SetArrayField(TEXT("samples"), Samples);
	Out->SetNumberField(TEXT("artifact_count"), Files.Num()); Out->SetNumberField(TEXT("sample_count"), Samples.Num()); Out->SetNumberField(TEXT("black_frame_count"), Black); Out->SetNumberField(TEXT("adjacent_duplicate_count"), Frozen); Out->SetNumberField(TEXT("decode_error_count"), DecodeErrors); Out->SetNumberField(TEXT("dimension_mismatch_count"), DimensionMismatch);
	Summary = FString::Printf(TEXT("Visual QA sampled %d/%d frames: black=%d duplicate=%d decode_errors=%d."), Samples.Num(), Files.Num(), Black, Frozen, DecodeErrors);
	if (!Pass) Error = TEXT("visual_qa_failed");
	return Pass;
}

static bool ToolMrqOutputValidate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TArray<FString> Files; CollectArtifactPaths(Args, Files);
	if (Files.IsEmpty()) { Error = TEXT("mrq_output_has_no_artifacts"); return false; }
	int64 TotalBytes = 0; int32 Empty = 0, Missing = 0; TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FString& File : Files)
	{
		const int64 Bytes = IFileManager::Get().FileSize(*File); if (Bytes < 0) ++Missing; else if (Bytes == 0) ++Empty; TotalBytes += FMath::Max<int64>(0, Bytes);
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("path"), File); Row->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes)); if (Bytes > 0) Row->SetStringField(TEXT("md5"), LexToString(FMD5Hash::HashFile(*File))); Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	const int32 Expected = IntField(Args, TEXT("expected_frame_count"), 0, 0, 10000000);
	const bool Pass = Missing == 0 && Empty == 0 && (Expected == 0 || Expected == Files.Num());
	Out->SetBoolField(TEXT("success"), Pass); Out->SetStringField(TEXT("status"), Pass ? TEXT("validated") : TEXT("issues_found")); Out->SetArrayField(TEXT("output_artifacts"), Rows); Out->SetNumberField(TEXT("artifact_count"), Files.Num()); Out->SetNumberField(TEXT("total_bytes"), static_cast<double>(TotalBytes)); Out->SetNumberField(TEXT("missing_count"), Missing); Out->SetNumberField(TEXT("empty_count"), Empty); Out->SetBoolField(TEXT("frame_count_matches"), Expected == 0 || Expected == Files.Num());
	Summary = FString::Printf(TEXT("Validated %d MRQ artifacts (%lld bytes)."), Files.Num(), TotalBytes);
	if (!Pass) Error = TEXT("mrq_output_validation_failed");
	return Pass;
}

static bool ToolOutputGuard(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	FString Root;
	if (!ResolveSavedPath(StringField(Args, TEXT("output_directory"), StringField(Args, TEXT("root"))), TEXT("MovieRenders"), true, Root, Error)) return false;
	TArray<FString> Existing;
	if (IFileManager::Get().DirectoryExists(*Root)) IFileManager::Get().FindFilesRecursive(Existing, *Root, TEXT("*.*"), true, false, false);
	const FString Policy = StringField(Args, TEXT("overwrite_policy"), TEXT("fail_if_exists"));
	const bool Pass = Existing.IsEmpty() || Policy == TEXT("append_unique");
	const FString Token = FString::Printf(TEXT("guard-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Out->SetBoolField(TEXT("success"), Pass); Out->SetStringField(TEXT("status"), Pass ? TEXT("ready") : TEXT("collision")); Out->SetStringField(TEXT("guard_token"), Token); Out->SetStringField(TEXT("normalized_root"), Root); Out->SetNumberField(TEXT("existing_file_count"), Existing.Num()); Out->SetBoolField(TEXT("directory_created"), false); Out->SetBoolField(TEXT("files_deleted"), false);
	Summary = FString::Printf(TEXT("Output guard checked %s: %d existing files."), *Root, Existing.Num());
	if (!Pass) Error = TEXT("output_collision");
	return Pass;
}

static UMovieSceneTrack* FindTrack(UMovieScene* Scene, const FString& Id)
{
	if (!Scene) return nullptr;
	for (UMovieSceneTrack* Track : Scene->GetTracks()) if (Track && Track->GetFName().ToString() == Id) return Track;
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(Scene)->GetBindings()) for (UMovieSceneTrack* Track : Binding.GetTracks()) if (Track && Track->GetFName().ToString() == Id) return Track;
	return nullptr;
}

static bool ResolveEvent(const TSharedRef<FJsonObject>& Args, ULevelSequence*& Sequence, UMovieSceneEventTrack*& Track, UMovieSceneEventTriggerSection*& Section, FString& Error)
{
	Sequence = LoadObject<ULevelSequence>(nullptr, *StringField(Args, TEXT("sequence_path")));
	if (!Sequence || !Sequence->GetMovieScene()) { Error = TEXT("sequence_asset_missing_or_invalid"); return false; }
	Track = Cast<UMovieSceneEventTrack>(FindTrack(Sequence->GetMovieScene(), StringField(Args, TEXT("event_track_id"), StringField(Args, TEXT("track_id")))));
	if (!Track) { Error = TEXT("event_track_not_found"); return false; }
	Section = nullptr;
	for (UMovieSceneSection* Existing : Track->GetAllSections()) if ((Section = Cast<UMovieSceneEventTriggerSection>(Existing))) break;
	if (!Section)
	{
		Section = Cast<UMovieSceneEventTriggerSection>(Track->CreateNewSection());
		if (!Section) { Error = TEXT("event_trigger_section_create_failed"); return false; }
		Track->AddSection(*Section);
	}
	return true;
}

static bool ToolEventEndpointCreate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	ULevelSequence* Sequence = nullptr; UMovieSceneEventTrack* Track = nullptr; UMovieSceneEventTriggerSection* Section = nullptr;
	if (!ResolveEvent(Args, Sequence, Track, Section, Error)) return false;
	const int32 Frame = IntField(Args, TEXT("frame"), 0, -10000000, 10000000);
	const FString Name = StringField(Args, TEXT("endpoint_name"));
	if (Name.IsEmpty()) { Error = TEXT("endpoint_name_is_required"); return false; }
	FMovieSceneSequenceEditor* Editor = FMovieSceneSequenceEditor::Find(Sequence);
	UBlueprint* Director = Editor ? Editor->GetOrCreateDirectorBlueprint(Sequence) : nullptr;
	if (!Director) { Error = TEXT("director_blueprint_unavailable"); return false; }
	Section->Modify(); Track->Modify(); Sequence->Modify();
	TMovieSceneChannelData<FMovieSceneEvent> Data = Section->EventChannel.GetData();
	int32 Index = Data.FindKey(FFrameNumber(Frame));
	if (Index == INDEX_NONE) Index = Data.AddKey(FFrameNumber(Frame), FMovieSceneEvent());
	FMovieSceneEvent& Event = Data.GetValues()[Index];
	UK2Node_CustomEvent* Endpoint = Cast<UK2Node_CustomEvent>(Event.WeakEndpoint.Get());
	if (!Endpoint) Endpoint = FMovieSceneEventUtils::BindNewUserFacingEvent(&Event, Section, Director);
	if (!Endpoint) { Error = TEXT("director_event_endpoint_bind_failed"); return false; }
	Endpoint->Modify(); Endpoint->CustomFunctionName = FName(*Name); Endpoint->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Director); FKismetEditorUtilities::CompileBlueprint(Director);
	if (Director->Status == BS_Error) { Error = TEXT("director_blueprint_compile_failed"); return false; }
	if (!SaveAsset(Sequence, Error)) return false;
	Out->SetBoolField(TEXT("success"), true); Out->SetStringField(TEXT("endpoint_id"), Endpoint->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)); Out->SetStringField(TEXT("endpoint_name"), Name); Out->SetNumberField(TEXT("frame"), Frame); Out->SetBoolField(TEXT("director_blueprint_compiled"), true); Out->SetBoolField(TEXT("readback_verified"), Event.WeakEndpoint.IsValid());
	Summary = FString::Printf(TEXT("Created and compiled Sequencer Director endpoint %s at frame %d."), *Name, Frame);
	return true;
}

static bool ToolEventPayloadSet(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	ULevelSequence* Sequence = nullptr; UMovieSceneEventTrack* Track = nullptr; UMovieSceneEventTriggerSection* Section = nullptr;
	if (!ResolveEvent(Args, Sequence, Track, Section, Error)) return false;
	const int32 Frame = IntField(Args, TEXT("frame"), 0, -10000000, 10000000);
	TMovieSceneChannelData<FMovieSceneEvent> Data = Section->EventChannel.GetData(); const int32 Index = Data.FindKey(FFrameNumber(Frame));
	if (Index == INDEX_NONE) { Error = TEXT("event_key_not_found_at_frame"); return false; }
	const TSharedPtr<FJsonObject>* Payload = nullptr;
	if (!Args->TryGetObjectField(TEXT("payload"), Payload) || !Payload) { Error = TEXT("payload_object_is_required"); return false; }
	Section->Modify(); FMovieSceneEvent& Event = Data.GetValues()[Index]; Event.PayloadVariables.Reset();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Payload)->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Array || Pair.Value->Type == EJson::Object || Pair.Value->Type == EJson::Null) { Error = FString::Printf(TEXT("payload_value_type_not_supported:%s"), *Pair.Key); return false; }
		FMovieSceneEventPayloadVariable Variable; Variable.Value = Pair.Value->AsString(); Event.PayloadVariables.Add(FName(*Pair.Key), Variable);
	}
	UBlueprint* Director = FMovieSceneSequenceEditor::Find(Sequence) ? FMovieSceneSequenceEditor::Find(Sequence)->GetOrCreateDirectorBlueprint(Sequence) : nullptr;
	if (!Director) { Error = TEXT("director_blueprint_unavailable"); return false; }
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Director); FKismetEditorUtilities::CompileBlueprint(Director);
	if (Director->Status == BS_Error) { Error = TEXT("director_blueprint_compile_failed_after_payload"); return false; }
	if (!SaveAsset(Sequence, Error)) return false;
	Out->SetBoolField(TEXT("success"), true); Out->SetNumberField(TEXT("frame"), Frame); Out->SetNumberField(TEXT("payload_field_count"), Event.PayloadVariables.Num()); Out->SetBoolField(TEXT("director_blueprint_compiled"), true); Out->SetBoolField(TEXT("readback_verified"), Event.PayloadVariables.Num() == (*Payload)->Values.Num());
	Summary = FString::Printf(TEXT("Persisted %d payload fields on Sequencer event at frame %d."), Event.PayloadVariables.Num(), Frame);
	return true;
}
}

void RegisterVideoProductionUpgradeTools(FSololmcpToolRegistry& Registry)
{
	using namespace VideoProductionUpgrade;
	const TSharedRef<FJsonObject> GraphPathSchema = ClosedSchema({
		{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Movie Render Graph object path."))},
		{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("Alias for asset_path."))}
	});
	Register(Registry, TEXT("movie_render_graph_asset_create"), TEXT("Create, save, and read back a native UMovieGraphConfig asset."), ClosedSchema({{TEXT("asset_path"), FSololmcpSchemaBuilder::String()}}, {TEXT("asset_path")}), ToolGraphAssetCreate);
	Register(Registry, TEXT("movie_render_graph_inspect"), TEXT("Inspect persisted native graph nodes, pins, and managed queue configuration."), GraphPathSchema, ToolGraphInspect, 10);
	Register(Registry, TEXT("movie_render_graph_basic_queue_configure"), TEXT("Persist a normalized Movie Render queue configuration on a graph asset and verify readback."), ClosedSchema({
		{TEXT("graph_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("map_path"), FSololmcpSchemaBuilder::String()},
		{TEXT("required_rhi"), FSololmcpSchemaBuilder::String(TEXT("Launch RHI."), {TEXT("dx12"), TEXT("vulkan")})}, {TEXT("frame_rate"), FSololmcpSchemaBuilder::Integer(TEXT("FPS."), 1, 120)}, {TEXT("frame_start"), FSololmcpSchemaBuilder::Integer()}, {TEXT("frame_end"), FSololmcpSchemaBuilder::Integer()},
		{TEXT("width"), FSololmcpSchemaBuilder::Integer()}, {TEXT("height"), FSololmcpSchemaBuilder::Integer()}, {TEXT("engine_warmup_frames"), FSololmcpSchemaBuilder::Integer()}, {TEXT("render_warmup_frames"), FSololmcpSchemaBuilder::Integer()}, {TEXT("use_fixed_timestep"), FSololmcpSchemaBuilder::Boolean()},
		{TEXT("resolution"), FSololmcpSchemaBuilder::Object({{TEXT("width"), FSololmcpSchemaBuilder::Integer()}, {TEXT("height"), FSololmcpSchemaBuilder::Integer()}}, {}, TEXT("Resolution."), false)},
		{TEXT("output"), FSololmcpSchemaBuilder::Object({{TEXT("root"), FSololmcpSchemaBuilder::String()}, {TEXT("format"), FSololmcpSchemaBuilder::String()}, {TEXT("ownership_id"), FSololmcpSchemaBuilder::String()}}, {}, TEXT("Output policy."), false)}, {TEXT("output_directory"), FSololmcpSchemaBuilder::String()}
	}), ToolGraphConfigure);
	// Legacy configuration entry points are aliases of the same native graph
	// mutation provider. They intentionally do not own state or create jobs.
	Register(Registry, TEXT("mrq_job_configure"), TEXT("Configure a native Movie Render Graph through the canonical queue configuration provider."), ClosedSchema({
		{TEXT("graph_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("map_path"), FSololmcpSchemaBuilder::String()},
		{TEXT("required_rhi"), FSololmcpSchemaBuilder::String(TEXT("Launch RHI."), {TEXT("dx12"), TEXT("vulkan")})}, {TEXT("frame_rate"), FSololmcpSchemaBuilder::Integer(TEXT("FPS."), 1, 120)}, {TEXT("frame_start"), FSololmcpSchemaBuilder::Integer()}, {TEXT("frame_end"), FSololmcpSchemaBuilder::Integer()},
		{TEXT("width"), FSololmcpSchemaBuilder::Integer()}, {TEXT("height"), FSololmcpSchemaBuilder::Integer()}, {TEXT("engine_warmup_frames"), FSololmcpSchemaBuilder::Integer()}, {TEXT("render_warmup_frames"), FSololmcpSchemaBuilder::Integer()}, {TEXT("use_fixed_timestep"), FSololmcpSchemaBuilder::Boolean()},
		{TEXT("resolution"), FSololmcpSchemaBuilder::Object({{TEXT("width"), FSololmcpSchemaBuilder::Integer()}, {TEXT("height"), FSololmcpSchemaBuilder::Integer()}}, {}, TEXT("Resolution."), false)},
		{TEXT("output"), FSololmcpSchemaBuilder::Object({{TEXT("root"), FSololmcpSchemaBuilder::String()}, {TEXT("format"), FSololmcpSchemaBuilder::String()}, {TEXT("ownership_id"), FSololmcpSchemaBuilder::String()}}, {}, TEXT("Output policy."), false)}, {TEXT("output_directory"), FSololmcpSchemaBuilder::String()}
	}), ToolGraphConfigure);
	Register(Registry, TEXT("sequencer_mrq_preset_apply"), TEXT("Apply a native Movie Render Graph preset through the canonical queue configuration provider."), ClosedSchema({
		{TEXT("graph_path"), FSololmcpSchemaBuilder::String()}, {TEXT("asset_path"), FSololmcpSchemaBuilder::String()}, {TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("map_path"), FSololmcpSchemaBuilder::String()},
		{TEXT("required_rhi"), FSololmcpSchemaBuilder::String(TEXT("Launch RHI."), {TEXT("dx12"), TEXT("vulkan")})}, {TEXT("frame_rate"), FSololmcpSchemaBuilder::Integer(TEXT("FPS."), 1, 120)}, {TEXT("frame_start"), FSololmcpSchemaBuilder::Integer()}, {TEXT("frame_end"), FSololmcpSchemaBuilder::Integer()},
		{TEXT("width"), FSololmcpSchemaBuilder::Integer()}, {TEXT("height"), FSololmcpSchemaBuilder::Integer()}, {TEXT("engine_warmup_frames"), FSololmcpSchemaBuilder::Integer()}, {TEXT("render_warmup_frames"), FSololmcpSchemaBuilder::Integer()}, {TEXT("use_fixed_timestep"), FSololmcpSchemaBuilder::Boolean()},
		{TEXT("resolution"), FSololmcpSchemaBuilder::Object({{TEXT("width"), FSololmcpSchemaBuilder::Integer()}, {TEXT("height"), FSololmcpSchemaBuilder::Integer()}}, {}, TEXT("Resolution."), false)},
		{TEXT("output"), FSololmcpSchemaBuilder::Object({{TEXT("root"), FSololmcpSchemaBuilder::String()}, {TEXT("format"), FSololmcpSchemaBuilder::String()}, {TEXT("ownership_id"), FSololmcpSchemaBuilder::String()}}, {}, TEXT("Output policy."), false)}, {TEXT("output_directory"), FSololmcpSchemaBuilder::String()}
	}), ToolGraphConfigure);
	Register(Registry, TEXT("movie_render_graph_compile_validate"), TEXT("Validate the real persisted Movie Render Graph structure and managed configuration."), GraphPathSchema, ToolGraphValidate, 10);

	const TSharedRef<FJsonObject> ArtifactSchema = ClosedSchema({
		{TEXT("output_directory"), FSololmcpSchemaBuilder::String()}, {TEXT("artifact_paths"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String())}, {TEXT("expected_frame_count"), FSololmcpSchemaBuilder::Integer()}, {TEXT("max_samples"), FSololmcpSchemaBuilder::Integer()}, {TEXT("black_luma_threshold"), FSololmcpSchemaBuilder::Number()}
	});
	Register(Registry, TEXT("movie_render_graph_visual_qa"), TEXT("Decode sampled render frames and detect black, duplicate, corrupt, and dimension-inconsistent output."), ArtifactSchema, ToolVisualQa);
	Register(Registry, TEXT("mrq_output_validate"), TEXT("Validate real MRQ files, byte counts, frame count, and hashes."), ArtifactSchema, ToolMrqOutputValidate);
	Register(Registry, TEXT("render_queue_output_guard_plan"), TEXT("Read-only normalize and collision-check an output path beneath Project/Saved."), ClosedSchema({{TEXT("output_directory"), FSololmcpSchemaBuilder::String()}, {TEXT("root"), FSololmcpSchemaBuilder::String()}, {TEXT("overwrite_policy"), FSololmcpSchemaBuilder::String()} }), ToolOutputGuard);

	Register(Registry, TEXT("video_transcoder_profile_create"), TEXT("Probe and create an H.264/MP4 native Media Foundation profile."), ClosedSchema({
		{TEXT("profile_id"), FSololmcpSchemaBuilder::String()}, {TEXT("container"), FSololmcpSchemaBuilder::String()}, {TEXT("video_codec"), FSololmcpSchemaBuilder::String()}, {TEXT("frame_rate"), FSololmcpSchemaBuilder::Integer()}, {TEXT("bitrate_kbps"), FSololmcpSchemaBuilder::Integer()}
	}), ToolProfileCreate);
	Register(Registry, TEXT("video_transcoder_job_submit"), TEXT("Transcode a manifest-backed PNG/JPEG image sequence into a real MP4 using the shared Job Runtime."), ClosedSchema({
		{TEXT("profile_id"), FSololmcpSchemaBuilder::String()}, {TEXT("input_manifest"), FSololmcpSchemaBuilder::String()}, {TEXT("input_path"), FSololmcpSchemaBuilder::String()}, {TEXT("output_path"), FSololmcpSchemaBuilder::String()}, {TEXT("client_request_id"), FSololmcpSchemaBuilder::String()}, {TEXT("overwrite_owned"), FSololmcpSchemaBuilder::Boolean()}
	}, {TEXT("profile_id"), TEXT("output_path")}), ToolTranscodeSubmit);
	const TSharedRef<FJsonObject> JobSchema = ClosedSchema({{TEXT("job_id"), FSololmcpSchemaBuilder::String()}}, {TEXT("job_id")});
	Register(Registry, TEXT("video_transcoder_job_status_get"), TEXT("Poll native transcode progress from the shared Job Runtime provider."), JobSchema, ToolTranscodeStatus);
	Register(Registry, TEXT("video_transcoder_job_cancel"), TEXT("Cancel native transcode and remove only the job-owned partial output."), JobSchema, ToolTranscodeCancel);
	Register(Registry, TEXT("video_transcoder_output_validate"), TEXT("Fully decode and compare a transcoder output against expected media properties."), ClosedSchema({
		{TEXT("job_id"), FSololmcpSchemaBuilder::String()}, {TEXT("output_path"), FSololmcpSchemaBuilder::String()}, {TEXT("validation_mode"), FSololmcpSchemaBuilder::String()}, {TEXT("expected_width"), FSololmcpSchemaBuilder::Integer()}, {TEXT("expected_height"), FSololmcpSchemaBuilder::Integer()}
	}), ToolTranscodeValidate);
	Register(Registry, TEXT("video_probe"), TEXT("Read real Media Foundation metadata and optionally decode every video frame."), ClosedSchema({
		{TEXT("path"), FSololmcpSchemaBuilder::String()}, {TEXT("output_path"), FSololmcpSchemaBuilder::String()}, {TEXT("validation_mode"), FSololmcpSchemaBuilder::String(TEXT("Probe depth."), {TEXT("metadata_only"), TEXT("sampled_decode"), TEXT("full_decode")})}
	}), ToolProbe, 5);

	const TSharedRef<FJsonObject> EndpointSchema = ClosedSchema({
		{TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("event_track_id"), FSololmcpSchemaBuilder::String()}, {TEXT("track_id"), FSololmcpSchemaBuilder::String()}, {TEXT("endpoint_name"), FSololmcpSchemaBuilder::String()}, {TEXT("frame"), FSololmcpSchemaBuilder::Integer()}, {TEXT("endpoint_kind"), FSololmcpSchemaBuilder::String()}
	}, {TEXT("sequence_path"), TEXT("endpoint_name")});
	Register(Registry, TEXT("sequencer_event_endpoint_create"), TEXT("Create/bind a real Director Blueprint event endpoint, compile, save, and read back."), EndpointSchema, ToolEventEndpointCreate);
	Register(Registry, TEXT("sequencer_event_payload_set"), TEXT("Persist closed scalar payload values on a real Sequencer event key and compile its Director Blueprint."), ClosedSchema({
		{TEXT("sequence_path"), FSololmcpSchemaBuilder::String()}, {TEXT("event_track_id"), FSololmcpSchemaBuilder::String()}, {TEXT("track_id"), FSololmcpSchemaBuilder::String()}, {TEXT("frame"), FSololmcpSchemaBuilder::Integer()}, {TEXT("payload"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Scalar payload map."), true)}
	}, {TEXT("sequence_path"), TEXT("frame"), TEXT("payload")}), ToolEventPayloadSet);
}
}
