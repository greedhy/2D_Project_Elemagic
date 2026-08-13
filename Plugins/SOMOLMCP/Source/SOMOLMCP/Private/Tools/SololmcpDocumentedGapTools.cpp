// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpDocumentedGapTools.cpp — v3.7.1 — fill documented-but-missing tools.
// ----------------------------------------------------------------------------
// These 7 tools are promised by the source documentation under `docs/`
// but were never registered. Each is registered here so MCP callers no longer
// hit `method_not_found`; complex ones are partial/stub with a `note` field
// explaining scope (real impl tracked for v3.7+).
//
// 1. editor_get_screenshot        — alias forwarding to editor_screenshot_viewport
// 2. batch_asset_thumbnails - real bounded batch renderer.
// 3. asset_ingest_from_disk ? fail-closed compatibility shim.
// 4. pcg_snapshot_hash ? compatibility fingerprint of (graph_path + now_utc_day); not a content hash.
// 5. pcg_snapshot_restore         ? fail-closed compatibility shim; use pcg_graph_restore
// 6. pcg_troubleshoot             - real bounded graph scan + fail-closed receipt
// 7. pcg_validate_hook            — runs pcg_graph_validate's checks (C4 fix; was STUB)
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ObjectThumbnail.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "ThumbnailRendering/ThumbnailManager.h"

// C4 fix: pcg_validate_hook now actually runs PCG graph validation rather than
// returning an empty findings envelope. We re-implement the minimal subset of
// pcg_graph_validate's checks here (the canonical impl lives in
// SololmcpPcgEnhancementTools.cpp's anon-namespace `Tool_PcgGraphValidate`,
// which is not externally linkable, and the FSololmcpToolExecutionContext
// does not expose the FSololmcpToolRegistry, so a Registry.ExecuteTool
// forward is not available — instead we duplicate the few critical checks
// using the same PCG headers the enhancement file already brings into the
// build via SOMOLMCP.Build.cs's "PCG" + "PCGEditor" deps).
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGEdge.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGCommon.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "Data/Registry/PCGDataTypeIdentifier.h"
#endif

namespace UE::SOMOLMCP
{
	namespace
	{
		static bool IsPcgPinTypeExactly(const UPCGPin* Pin, EPCGDataType Type)
		{
			if (!Pin) { return false; }
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
			return Pin->Properties.AllowedTypes.IsSameType(FPCGDataTypeIdentifier(Type));
#else
			return Pin->Properties.AllowedTypes == Type;
#endif
		}

		static bool ArePcgPinTypesCompatible(const UPCGPin* UpstreamPin, const UPCGPin* DownstreamPin)
		{
			if (!UpstreamPin || !DownstreamPin) { return false; }
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
			const FPCGDataTypeIdentifier Up = UpstreamPin->Properties.AllowedTypes;
			const FPCGDataTypeIdentifier Down = DownstreamPin->Properties.AllowedTypes;
			const FPCGDataTypeIdentifier AnyType(EPCGDataType::Any);
			return Down.IsSameType(AnyType) || Up.IsSameType(AnyType) || Up.Intersects(Down);
#else
			const EPCGDataType Up = UpstreamPin->Properties.AllowedTypes;
			const EPCGDataType Down = DownstreamPin->Properties.AllowedTypes;
			return Down == EPCGDataType::Any || Up == EPCGDataType::Any || EnumHasAnyFlags(Up, Down);
#endif
		}

		// 64-bit FNV-1a over a UTF-8 byte stream.
		static uint64 Fnv1a64(const FString& In)
		{
			uint64 Hash = 0xcbf29ce484222325ULL;
			const FTCHARToUTF8 Conv(*In);
			const ANSICHAR* Bytes = Conv.Get();
			for (int32 i = 0; i < Conv.Length(); ++i)
			{
				Hash ^= static_cast<uint64>(static_cast<uint8>(Bytes[i]));
				Hash *= 0x100000001b3ULL;
			}
			return Hash;
		}

		static bool RenderGapThumbnail(
			UObject* Asset,
			const int32 MaxWidth,
			const int32 MaxHeight,
			TArray<uint8>& OutPngData,
			FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("Asset is null.");
				return false;
			}
			if (!GEditor)
			{
				OutError = TEXT("GEditor is unavailable; thumbnail rendering requires editor context.");
				return false;
			}

			FObjectThumbnail TempThumbnail;
			ThumbnailTools::RenderThumbnail(
				Asset,
				FMath::Clamp(MaxWidth, 64, 2048),
				FMath::Clamp(MaxHeight, 64, 2048),
				ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
				nullptr,
				&TempThumbnail);

			const int32 ThumbWidth = TempThumbnail.GetImageWidth();
			const int32 ThumbHeight = TempThumbnail.GetImageHeight();
			const TArray<uint8>& UncompressedData = TempThumbnail.GetUncompressedImageData();
			if (ThumbWidth <= 0 || ThumbHeight <= 0 || UncompressedData.Num() == 0)
			{
				OutError = FString::Printf(
					TEXT("Failed to render thumbnail for '%s'; asset type may not support thumbnails."),
					*Asset->GetName());
				return false;
			}

			TArray<FColor> Colors;
			Colors.Reserve(ThumbWidth * ThumbHeight);
			for (int32 i = 0; i < ThumbWidth * ThumbHeight; ++i)
			{
				const int32 ByteIdx = i * 4;
				if (ByteIdx + 3 < UncompressedData.Num())
				{
					Colors.Add(FColor(
						UncompressedData[ByteIdx + 2],
						UncompressedData[ByteIdx + 1],
						UncompressedData[ByteIdx],
						UncompressedData[ByteIdx + 3]));
				}
				else
				{
					Colors.Add(FColor::Black);
				}
			}

			return FSololmcpEditorServices::CompressPixelsToPng(
				Colors,
				ThumbWidth,
				ThumbHeight,
				OutPngData,
				OutError);
		}

		// 1. editor_get_screenshot — alias: return a note that redirects callers to
		//    the canonical name editor_screenshot_viewport.
		static bool RunEditorGetScreenshot(
			const FSololmcpToolExecutionContext& /*Context*/,
			const TSharedRef<FJsonObject>& /*Arguments*/,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& /*OutError*/)
		{
			OutStructured->SetStringField(
				TEXT("alias_of"),
				TEXT("editor_screenshot_viewport"));
			OutStructured->SetStringField(
				TEXT("note"),
				TEXT("editor_get_screenshot is a compatibility alias; call editor_screenshot_viewport "
				     "directly for image bytes. This stub returns metadata only to avoid duplicating "
				     "the capture pipeline here."));
			OutSummary = TEXT("editor_get_screenshot alias pointer (call editor_screenshot_viewport for pixels).");
			return true;
		}

		// 2. batch_asset_thumbnails — fail-closed compatibility shim; use asset_get_thumbnail per path.
		static bool RunBatchAssetThumbnails(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
			Arguments->TryGetArrayField(TEXT("asset_paths"), Paths);
			TArray<TSharedPtr<FJsonValue>> Items;
			TArray<TSharedPtr<FJsonValue>> ImageContent;
			int32 Count = Paths ? Paths->Num() : 0;
			if (Count > 50) { Count = 50; }
			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			int32 MaxWidth = 256;
			int32 MaxHeight = 256;
			Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
			Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);
			MaxWidth = FMath::Clamp(MaxWidth, 64, 2048);
			MaxHeight = FMath::Clamp(MaxHeight, 64, 2048);

			for (int32 i = 0; i < Count; ++i)
			{
				FString Path = (*Paths)[i]->AsString();
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("path"), Path);
				Item->SetNumberField(TEXT("image_index"), -1);

				FString LoadError;
				UObject* Asset = Context.Services.LoadAsset(Path, LoadError);
				if (!Asset)
				{
					++FailureCount;
					Item->SetBoolField(TEXT("success"), false);
					Item->SetStringField(TEXT("error_code"), TEXT("LOAD_FAILED"));
					Item->SetStringField(TEXT("error"), LoadError);
					Items.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}

				TArray<uint8> PngData;
				FString ThumbError;
				if (!RenderGapThumbnail(Asset, MaxWidth, MaxHeight, PngData, ThumbError))
				{
					++FailureCount;
					Item->SetBoolField(TEXT("success"), false);
					Item->SetStringField(TEXT("error_code"), TEXT("THUMBNAIL_RENDER_FAILED"));
					Item->SetStringField(TEXT("error"), ThumbError);
					Items.Add(MakeShared<FJsonValueObject>(Item));
					continue;
				}

				const int32 ImageIndex = ImageContent.Num();
				ImageContent.Add(MakeImageContentValue(PngData));
				++SuccessCount;
				Item->SetBoolField(TEXT("success"), true);
				Item->SetStringField(TEXT("asset_name"), Asset->GetName());
				Item->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetName() : TEXT(""));
				Item->SetNumberField(TEXT("image_index"), ImageIndex);
				Item->SetNumberField(TEXT("image_size_bytes"), PngData.Num());
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			OutStructured->SetArrayField(TEXT("items"), Items);
			OutStructured->SetArrayField(TEXT("_imageContent"), ImageContent);
			OutStructured->SetBoolField(TEXT("success"), SuccessCount > 0 && FailureCount == 0);
			OutStructured->SetStringField(TEXT("status"), FailureCount == 0 ? TEXT("ok") : (SuccessCount > 0 ? TEXT("partial") : TEXT("failed")));
			OutStructured->SetNumberField(TEXT("requested_count"), Count);
			OutStructured->SetNumberField(TEXT("success_count"), SuccessCount);
			OutStructured->SetNumberField(TEXT("failure_count"), FailureCount);
			OutStructured->SetNumberField(TEXT("max_width"), MaxWidth);
			OutStructured->SetNumberField(TEXT("max_height"), MaxHeight);
			OutSummary = FString::Printf(
				TEXT("batch_asset_thumbnails rendered %d/%d thumbnails (%d failed)."),
				SuccessCount,
				Count,
				FailureCount);
			if (SuccessCount == 0 && Count > 0)
			{
				OutError = TEXT("No requested asset thumbnails rendered successfully.");
				return false;
			}
			return FailureCount == 0;
		}

		// 3. asset_ingest_from_disk — one-file native import compatibility entry point.
		static bool RunAssetIngestFromDisk(
			const FSololmcpToolExecutionContext& /*Context*/,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString Src, Target;
			if (!Arguments->TryGetStringField(TEXT("source_path"), Src) ||
				!Arguments->TryGetStringField(TEXT("target_folder"), Target))
			{
				OutError = TEXT("Missing required args: source_path, target_folder");
				return false;
			}
			if (!FPaths::FileExists(Src))
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("SOURCE_FILE_NOT_FOUND"));
				OutError = FString::Printf(TEXT("Source file does not exist: %s"), *Src);
				return false;
			}
			Target = Target.TrimStartAndEnd();
			while (Target.EndsWith(TEXT("/")))
			{
				Target.LeftChopInline(1);
			}
			if (!(Target == TEXT("/Game") || Target.StartsWith(TEXT("/Game/"))))
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("INVALID_DESTINATION_PATH"));
				OutError = TEXT("target_folder must be /Game or a folder below /Game.");
				return false;
			}

			FString Ext = FPaths::GetExtension(Src, false).ToLower();
			FString Kind = TEXT("Unknown");
			if (Ext == TEXT("fbx")) { Kind = TEXT("StaticMesh"); }
			else if (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg") ||
			         Ext == TEXT("tga") || Ext == TEXT("exr")) { Kind = TEXT("Texture2D"); }
			else if (Ext == TEXT("wav") || Ext == TEXT("ogg")) { Kind = TEXT("SoundWave"); }
			else if (Ext == TEXT("uasset")) { Kind = TEXT("PackageCopy"); }

			bool bReplaceExisting = true;
			bool bSave = true;
			bool bAutomated = true;
			Arguments->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
			Arguments->TryGetBoolField(TEXT("save"), bSave);
			Arguments->TryGetBoolField(TEXT("automated"), bAutomated);

			UAssetImportTask* Task = NewObject<UAssetImportTask>(GetTransientPackage());
			Task->Filename = Src;
			Task->DestinationPath = Target;
			Task->bAutomated = bAutomated;
			Task->bReplaceExisting = bReplaceExisting;
			Task->bSave = bSave;
			TArray<UAssetImportTask*> Tasks{Task};
			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			AssetToolsModule.Get().ImportAssetTasks(Tasks);

			TArray<TSharedPtr<FJsonValue>> ImportedAssets;
			for (UObject* ImportedObject : Task->GetObjects())
			{
				if (!ImportedObject)
				{
					continue;
				}
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), ImportedObject->GetName());
				Item->SetStringField(TEXT("class"), ImportedObject->GetClass()->GetPathName());
				Item->SetStringField(TEXT("object_path"), ImportedObject->GetPathName());
				Item->SetStringField(TEXT("package_path"), ImportedObject->GetOutermost()->GetName());
				ImportedAssets.Add(MakeShared<FJsonValueObject>(Item));
			}
			if (ImportedAssets.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("IMPORT_FAILED"));
				OutStructured->SetStringField(TEXT("source_path"), Src);
				OutStructured->SetStringField(TEXT("target_folder"), Target);
				OutStructured->SetArrayField(TEXT("assets"), ImportedAssets);
				OutError = FString::Printf(TEXT("Import produced no assets for source file: %s"), *Src);
				return false;
			}

			OutStructured->SetStringField(TEXT("source_path"), Src);
			OutStructured->SetStringField(TEXT("target_folder"), Target);
			OutStructured->SetStringField(TEXT("kind"), Kind);
			OutStructured->SetArrayField(TEXT("assets"), ImportedAssets);
			OutStructured->SetNumberField(TEXT("count"), ImportedAssets.Num());
			OutStructured->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
			OutStructured->SetBoolField(TEXT("saved"), bSave);
			OutStructured->SetBoolField(TEXT("success"), true);
			OutStructured->SetStringField(TEXT("status"), TEXT("completed"));
			OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.asset_ingest_from_disk.v2"));
			OutSummary = FString::Printf(TEXT("Imported '%s' into '%s' as %d asset(s)."), *Src, *Target, ImportedAssets.Num());
			return true;
		}

		// 4. pcg_snapshot_hash — deterministic hash of (graph_path + now_utc_day)
		static bool RunPcgSnapshotHash(
			const FSololmcpToolExecutionContext& /*Context*/,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString GraphPath;
			if (!Arguments->TryGetStringField(TEXT("graph_path"), GraphPath))
			{
				OutError = TEXT("Missing required arg: graph_path");
				return false;
			}
			const FString Stamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d"));
			const uint64 H1 = Fnv1a64(GraphPath + TEXT("|") + Stamp);
			const uint64 H2 = Fnv1a64(GraphPath + TEXT("|") + Stamp + TEXT("|salt"));
			OutStructured->SetStringField(
				TEXT("hash"),
				FString::Printf(TEXT("%016llx%016llx"), H1, H2));
			OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
			OutStructured->SetStringField(TEXT("hash_kind"), TEXT("path_day_fingerprint"));
			OutStructured->SetStringField(TEXT("basis"), TEXT("fnv1a-64(graph_path|utc_day)"));
			OutStructured->SetBoolField(TEXT("content_hash"), false);
			OutStructured->SetBoolField(TEXT("stable_across_days"), false);
			OutStructured->SetBoolField(TEXT("usable_for_restore"), false);
			OutStructured->SetStringField(TEXT("replacement_tool"), TEXT("pcg_graph_snapshot"));
			OutStructured->SetStringField(
				TEXT("warning"),
				TEXT("This is not a PCG graph content hash. It is only a compatibility fingerprint "
				     "derived from graph_path and utc_day; use pcg_graph_snapshot for a real snapshot."));
			OutSummary = FString::Printf(TEXT("pcg_snapshot_hash compatibility fingerprint %016llx%016llx"), H1, H2);
			return true;
		}

		FString MakeLegacyPcgSnapshotPath(const FString& GraphPath, const FString& SnapshotId)
		{
			if (SnapshotId.StartsWith(TEXT("/Game/")))
			{
				return SnapshotId;
			}
			FString GraphName = FPaths::GetBaseFilename(GraphPath);
			int32 DotIndex = INDEX_NONE;
			if (GraphName.FindChar(TEXT('.'), DotIndex))
			{
				GraphName = GraphName.Left(DotIndex);
			}
			return FString::Printf(TEXT("/Game/PCG/__Snapshots/%s__%s"), *GraphName, *SnapshotId);
		}

		// 5. pcg_snapshot_restore — legacy argument adapter with a real restore path.
		static bool RunPcgSnapshotRestore(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString GraphPath, SnapshotId;
			if (!Arguments->TryGetStringField(TEXT("graph_path"), GraphPath) || GraphPath.IsEmpty() ||
				!Arguments->TryGetStringField(TEXT("snapshot_id"), SnapshotId) || SnapshotId.IsEmpty())
			{
				OutError = TEXT("Missing graph_path or snapshot_id.");
				return false;
			}
			bool bForce = false;
			Arguments->TryGetBoolField(TEXT("force"), bForce);
			const FString SnapshotPath = MakeLegacyPcgSnapshotPath(GraphPath, SnapshotId);

			FString LoadError;
			UObject* SnapshotAsset = Context.Services.LoadAsset(SnapshotPath, LoadError);
			if (!SnapshotAsset)
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("SNAPSHOT_NOT_FOUND"));
				OutStructured->SetStringField(TEXT("snapshot_asset_path"), SnapshotPath);
				OutError = FString::Printf(TEXT("Snapshot asset could not be loaded: %s"), *LoadError);
				return false;
			}
			if (!SnapshotAsset->IsA<UPCGGraph>())
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("SNAPSHOT_TYPE_MISMATCH"));
				OutError = FString::Printf(TEXT("Snapshot asset is not a UPCGGraph: %s"), *SnapshotAsset->GetClass()->GetPathName());
				return false;
			}

			FString ExistingError;
			UObject* ExistingTarget = Context.Services.LoadAsset(GraphPath, ExistingError);
			if (ExistingTarget && !bForce)
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("TARGET_EXISTS"));
				OutError = TEXT("Target PCG graph exists. Pass force=true to replace it.");
				return false;
			}
			if (ExistingTarget)
			{
				FString DeleteError;
				if (!Context.Services.DeleteAsset(GraphPath, DeleteError))
				{
					OutStructured->SetStringField(TEXT("error_code"), TEXT("TARGET_DELETE_FAILED"));
					OutError = FString::Printf(TEXT("Failed to delete existing target: %s"), *DeleteError);
					return false;
				}
			}

			FString DuplicateError;
			UObject* RestoredAsset = Context.Services.DuplicateAsset(SnapshotPath, GraphPath, DuplicateError);
			if (!RestoredAsset || !RestoredAsset->IsA<UPCGGraph>())
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("RESTORE_DUPLICATE_FAILED"));
				OutError = DuplicateError.IsEmpty() ? TEXT("Restored object was not a UPCGGraph.") : DuplicateError;
				return false;
			}
			FString SaveError;
			if (!Context.Services.SaveAsset(GraphPath, false, SaveError))
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("RESTORE_SAVE_FAILED"));
				OutError = SaveError;
				return false;
			}

			FString VerifyError;
			UObject* ReadbackAsset = Context.Services.LoadAsset(GraphPath, VerifyError);
			const bool bVerified = ReadbackAsset && ReadbackAsset->IsA<UPCGGraph>();
			OutStructured->SetBoolField(TEXT("restored"), bVerified);
			OutStructured->SetBoolField(TEXT("verified"), bVerified);
			OutStructured->SetStringField(TEXT("status"), bVerified ? TEXT("completed") : TEXT("failed"));
			OutStructured->SetStringField(TEXT("snapshot_asset_path"), SnapshotPath);
			OutStructured->SetStringField(TEXT("target_asset_path"), GraphPath);
			OutStructured->SetBoolField(TEXT("overwrote_existing"), ExistingTarget != nullptr);
			OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.pcg_snapshot_restore.v2"));
			OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
			OutStructured->SetStringField(TEXT("snapshot_id"), SnapshotId);
			if (!bVerified)
			{
				OutError = VerifyError.IsEmpty() ? TEXT("Restored PCG graph readback failed.") : VerifyError;
				return false;
			}
			OutSummary = FString::Printf(TEXT("Restored and verified PCG snapshot '%s' to '%s'."), *SnapshotPath, *GraphPath);
			return true;
		}

		// 6. pcg_troubleshoot — stub checklist.
		static bool RunPcgTroubleshoot(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			FString GraphPath;
			if (!Arguments->TryGetStringField(TEXT("graph_path"), GraphPath))
			{
				OutError = TEXT("Missing required arg: graph_path");
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Issues;
			TArray<TSharedPtr<FJsonValue>> RecommendedTools;
			RecommendedTools.Add(MakeShared<FJsonValueString>(TEXT("pcg_graph_validate")));
			RecommendedTools.Add(MakeShared<FJsonValueString>(TEXT("pcg_graph_explain")));
			RecommendedTools.Add(MakeShared<FJsonValueString>(TEXT("pcg_graph_diff")));
			RecommendedTools.Add(MakeShared<FJsonValueString>(TEXT("pcg_dry_run")));

			auto AddIssue = [&Issues](const FString& Severity, const FString& NodeName, const FString& Message)
			{
				TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("severity"), Severity);
				Issue->SetStringField(TEXT("node"), NodeName);
				Issue->SetStringField(TEXT("message"), Message);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
			};

			FString LoadErr;
			UObject* Asset = Context.Services.LoadAsset(GraphPath, LoadErr);
			if (!Asset)
			{
				AddIssue(TEXT("error"), TEXT(""), FString::Printf(TEXT("LoadAsset failed: %s"), *LoadErr));
				OutStructured->SetArrayField(TEXT("issues"), Issues);
				OutStructured->SetArrayField(TEXT("recommended_tools"), RecommendedTools);
				OutStructured->SetBoolField(TEXT("scan_completed"), false);
				OutStructured->SetBoolField(TEXT("issues_known_complete"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("load_failed"));
				OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
				OutError = LoadErr;
				OutSummary = FString::Printf(TEXT("pcg_troubleshoot could not load %s."), *GraphPath);
				return false;
			}

			UPCGGraph* Graph = Cast<UPCGGraph>(Asset);
			if (!Graph)
			{
				if (UPCGGraphInterface* GI = Cast<UPCGGraphInterface>(Asset))
				{
					Graph = GI->GetMutablePCGGraph();
				}
			}
			if (!Graph)
			{
				AddIssue(TEXT("error"), TEXT(""), TEXT("Asset is not a UPCGGraph / UPCGGraphInterface."));
				OutStructured->SetArrayField(TEXT("issues"), Issues);
				OutStructured->SetArrayField(TEXT("recommended_tools"), RecommendedTools);
				OutStructured->SetBoolField(TEXT("scan_completed"), false);
				OutStructured->SetBoolField(TEXT("issues_known_complete"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("not_pcg_graph"));
				OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
				OutError = TEXT("Asset is not a PCG graph.");
				OutSummary = FString::Printf(TEXT("pcg_troubleshoot rejected non-PCG asset %s."), *GraphPath);
				return false;
			}

			int32 ErrorCount = 0;
			int32 WarningCount = 0;
			auto AddCountedIssue = [&](const FString& Severity, const FString& NodeName, const FString& Message)
			{
				AddIssue(Severity, NodeName, Message);
				if (Severity == TEXT("error")) { ++ErrorCount; }
				else { ++WarningCount; }
			};

			const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
			if (Nodes.Num() == 0)
			{
				AddCountedIssue(TEXT("warning"), TEXT(""), TEXT("Graph has no nodes."));
			}

			for (UPCGNode* Node : Nodes)
			{
				if (!Node)
				{
					AddCountedIssue(TEXT("error"), TEXT(""), TEXT("Graph contains a null node reference."));
					continue;
				}

				const FString NodeName = Node->GetName();
				const UPCGSettings* Settings = Node->GetSettings();
				if (!Settings)
				{
					AddCountedIssue(TEXT("error"), NodeName, TEXT("Node has no Settings CDO."));
					continue;
				}

				for (UPCGPin* InPin : Node->GetInputPins())
				{
					if (!InPin) { continue; }
					const FString PinLabel = InPin->Properties.Label.ToString();
					if (InPin->Edges.Num() == 0)
					{
						const bool bLikelyRequired =
							!IsPcgPinTypeExactly(InPin, EPCGDataType::Param) &&
							!IsPcgPinTypeExactly(InPin, EPCGDataType::Settings);
						AddCountedIssue(
							bLikelyRequired ? TEXT("error") : TEXT("warning"),
							NodeName,
							FString::Printf(TEXT("Input pin '%s' has no incoming edges."), *PinLabel));
						continue;
					}

					for (const TObjectPtr<UPCGEdge>& Edge : InPin->Edges)
					{
						if (!Edge) { continue; }
						UPCGPin* UpstreamPin = Edge->InputPin.Get();
						if (!UpstreamPin)
						{
							AddCountedIssue(
								TEXT("error"),
								NodeName,
								FString::Printf(TEXT("Input pin '%s' has a broken upstream edge."), *PinLabel));
							continue;
						}
						const bool bCompat = ArePcgPinTypesCompatible(UpstreamPin, InPin);
						if (!bCompat)
						{
							AddCountedIssue(
								TEXT("error"),
								NodeName,
								FString::Printf(
									TEXT("Pin type mismatch on input '%s' from upstream '%s'."),
									*PinLabel,
									UpstreamPin->Node ? *UpstreamPin->Node->GetName() : TEXT("x")));
						}
					}
				}
			}

			OutStructured->SetArrayField(TEXT("issues"), Issues);
			OutStructured->SetArrayField(TEXT("recommended_tools"), RecommendedTools);
			OutStructured->SetBoolField(TEXT("scan_completed"), true);
			OutStructured->SetBoolField(TEXT("issues_known_complete"), true);
			OutStructured->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("ok") : TEXT("issues_found"));
			OutStructured->SetNumberField(TEXT("node_count"), Nodes.Num());
			OutStructured->SetNumberField(TEXT("error_count"), ErrorCount);
			OutStructured->SetNumberField(TEXT("warning_count"), WarningCount);
			OutStructured->SetStringField(TEXT("graph_path"), GraphPath);
			OutSummary = FString::Printf(
				TEXT("pcg_troubleshoot scanned %s: %d errors, %d warnings."),
				*GraphPath,
				ErrorCount,
				WarningCount);
			if (ErrorCount > 0)
			{
				OutError = FString::Printf(TEXT("pcg_troubleshoot found %d errors."), ErrorCount);
				return false;
			}
			return true;
		}

		// 7. pcg_validate_hook — actually runs the PCG graph's validation checks
		// (C4 fix in PLAN — was a STUB that always returned passed=true / empty
		// findings; now duplicates the core checks from
		// SololmcpPcgEnhancementTools.cpp::Tool_PcgGraphValidate).
		//
		// Args:
		//   asset_path / graph_path — content path of the UPCGGraph (or wrapper).
		//                             Both names accepted for backwards compat.
		//   hook_stage              — pre_generation | post_generation | post_compile (default pre_generation).
		//   min_severity            — "error" | "warning" (default "error").
		//                             "error":   findings only contains errors; passed = (error_count == 0).
		//                             "warning": findings contains both; passed = (error_count == 0).
		//
		// Fail closed: a missing/non-PCG asset OR an unloadable graph yields
		//   passed=false, error_count=0, findings=[{message: "validate impl unavailable: ..."}]
		// rather than a hard error — this lets DAG hook callers continue.
		static bool RunPcgValidateHook(
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError)
		{
			// Accept both the historical "graph_path" name and the requested "asset_path".
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				Arguments->TryGetStringField(TEXT("graph_path"), AssetPath);
			}
			if (AssetPath.IsEmpty())
			{
				OutError = TEXT("Missing required arg: asset_path (or legacy alias graph_path)");
				return false;
			}

			FString HookStage;
			Arguments->TryGetStringField(TEXT("hook_stage"), HookStage);
			if (HookStage.IsEmpty()) { HookStage = TEXT("pre_generation"); }
			if (HookStage != TEXT("pre_generation") &&
			    HookStage != TEXT("post_generation") &&
			    HookStage != TEXT("post_compile"))
			{
				OutError = FString::Printf(
					TEXT("Unknown hook_stage '%s'. Accepted: pre_generation, post_generation, post_compile."),
					*HookStage);
				return false;
			}

			FString MinSeverity;
			Arguments->TryGetStringField(TEXT("min_severity"), MinSeverity);
			MinSeverity = MinSeverity.ToLower();
			if (MinSeverity.IsEmpty()) { MinSeverity = TEXT("error"); }
			if (MinSeverity != TEXT("error") && MinSeverity != TEXT("warning"))
			{
				OutError = FString::Printf(
					TEXT("Unknown min_severity '%s'. Accepted: error, warning."),
					*MinSeverity);
				return false;
			}

			// Helper for the fail-closed path: emit a single synthetic finding so
			// callers can see *why* validation could not run.
			auto SoftFail = [&](const FString& Reason)
			{
				TArray<TSharedPtr<FJsonValue>> Findings;
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("severity"), TEXT("error"));
				F->SetStringField(TEXT("node"), TEXT(""));
				F->SetStringField(TEXT("message"),
					FString::Printf(TEXT("validate impl unavailable: %s"), *Reason));
				Findings.Add(MakeShared<FJsonValueObject>(F));

				OutStructured->SetBoolField(TEXT("passed"), false);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("stage"), HookStage);
				OutStructured->SetStringField(TEXT("min_severity"), MinSeverity);
				OutStructured->SetNumberField(TEXT("error_count"), 0);
				OutStructured->SetNumberField(TEXT("warning_count"), 0);
				OutStructured->SetArrayField(TEXT("findings"), Findings);
				OutError = Reason;
				OutSummary = FString::Printf(
					TEXT("pcg_validate_hook(%s) soft-failed: %s"), *HookStage, *Reason);
				return false;
			};

			// Try to load the asset via the shared services (does not pull
			// PCG-side code beyond what's already linked).
			FString LoadErr;
			UObject* Asset = Context.Services.LoadAsset(AssetPath, LoadErr);
			if (!Asset)
			{
				return SoftFail(FString::Printf(TEXT("LoadAsset failed: %s"), *LoadErr));
			}

			// Accept either the graph directly or a graph-interface wrapper.
			UPCGGraph* Graph = Cast<UPCGGraph>(Asset);
			if (!Graph)
			{
				if (UPCGGraphInterface* GI = Cast<UPCGGraphInterface>(Asset))
				{
					Graph = GI->GetMutablePCGGraph();
				}
			}
			if (!Graph)
			{
				return SoftFail(TEXT("asset is not a UPCGGraph / UPCGGraphInterface"));
			}

			// Walk the graph collecting findings. Mirrors the canonical
			// Tool_PcgGraphValidate's check set (subset that does not need the
			// PinTypeToString helper, which is anon-namespace in the other TU).
			TArray<TSharedPtr<FJsonValue>> AllFindings;
			int32 ErrorCount = 0;
			int32 WarningCount = 0;

			auto AddFinding = [&](const FString& Severity, const FString& NodeName, const FString& Message)
			{
				TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("severity"), Severity);
				F->SetStringField(TEXT("node"), NodeName);
				F->SetStringField(TEXT("message"), Message);
				AllFindings.Add(MakeShared<FJsonValueObject>(F));
				if (Severity == TEXT("error")) { ++ErrorCount; } else { ++WarningCount; }
			};

			const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
			if (Nodes.Num() == 0)
			{
				AddFinding(TEXT("warning"), TEXT(""), TEXT("Graph has no nodes."));
			}

			for (UPCGNode* Node : Nodes)
			{
				if (!Node)
				{
					AddFinding(TEXT("error"), TEXT(""), TEXT("Graph contains a null node reference."));
					continue;
				}
				const FString NodeName = Node->GetName();
				const UPCGSettings* Settings = Node->GetSettings();
				if (!Settings)
				{
					AddFinding(TEXT("error"), NodeName, TEXT("Node has no Settings CDO."));
					continue;
				}

				// Unconnected input pins. Param/Settings inputs are usually optional;
				// everything else is treated as required.
				for (UPCGPin* InPin : Node->GetInputPins())
				{
					if (!InPin) { continue; }
					if (InPin->Edges.Num() == 0)
					{
						const bool bLikelyRequired =
							!IsPcgPinTypeExactly(InPin, EPCGDataType::Param) &&
							!IsPcgPinTypeExactly(InPin, EPCGDataType::Settings);
						AddFinding(
							bLikelyRequired ? TEXT("error") : TEXT("warning"),
							NodeName,
							FString::Printf(TEXT("Input pin '%s' has no incoming edges."),
								*InPin->Properties.Label.ToString()));
						continue;
					}
					// Per-edge type compatibility.
					for (const TObjectPtr<UPCGEdge>& Edge : InPin->Edges)
					{
						if (!Edge) { continue; }
						UPCGPin* UpstreamPin = Edge->InputPin.Get();
						if (!UpstreamPin)
						{
							AddFinding(TEXT("error"), NodeName,
								FString::Printf(TEXT("Input pin '%s' has a broken upstream edge."),
									*InPin->Properties.Label.ToString()));
							continue;
						}
						const bool bCompat = ArePcgPinTypesCompatible(UpstreamPin, InPin);
						if (!bCompat)
						{
							AddFinding(TEXT("error"), NodeName,
								FString::Printf(TEXT("Pin type mismatch on input '%s' from upstream '%s'."),
									*InPin->Properties.Label.ToString(),
									UpstreamPin->Node ? *UpstreamPin->Node->GetName() : TEXT("x")));
						}
					}
				}
			}

			// Filter by min_severity. "error" hides warnings; "warning" keeps both.
			TArray<TSharedPtr<FJsonValue>> Filtered;
			Filtered.Reserve(AllFindings.Num());
			for (const TSharedPtr<FJsonValue>& V : AllFindings)
			{
				const TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
				if (!O.IsValid()) { continue; }
				FString Sev;
				O->TryGetStringField(TEXT("severity"), Sev);
				if (MinSeverity == TEXT("error") && Sev != TEXT("error")) { continue; }
				Filtered.Add(V);
			}

			// passed semantics:
			//   - error_count > 0  → false (regardless of min_severity)
			//   - only warnings    → true (caller decides whether to gate on findings)
			const bool bPassed = (ErrorCount == 0);

			OutStructured->SetBoolField(TEXT("passed"), bPassed);
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("stage"), HookStage);
			OutStructured->SetStringField(TEXT("min_severity"), MinSeverity);
			OutStructured->SetNumberField(TEXT("error_count"), ErrorCount);
			OutStructured->SetNumberField(TEXT("warning_count"), WarningCount);
			OutStructured->SetArrayField(TEXT("findings"), Filtered);

			OutSummary = FString::Printf(
				TEXT("pcg_validate_hook(%s) on '%s': %d errors, %d warnings — %s"),
				*HookStage, *AssetPath, ErrorCount, WarningCount,
				bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			if (!bPassed)
			{
				OutError = FString::Printf(
					TEXT("pcg_validate_hook failed for '%s': %d validation errors."),
					*AssetPath, ErrorCount);
			}
			return bPassed;
		}
	} // anon

	void RegisterV371DocumentedGapTools(FSololmcpToolRegistry& Registry)
	{
		using S = FSololmcpSchemaBuilder;

		Registry.Register({
			TEXT("editor_get_screenshot"),
			TEXT("Compatibility alias for editor_screenshot_viewport. Returns a metadata pointer; "
			     "callers should invoke editor_screenshot_viewport for the actual image."),
			S::Object({}),
			&RunEditorGetScreenshot
		});

		Registry.Register({
			TEXT("batch_asset_thumbnails"),
			TEXT("Render thumbnails for up to 50 Unreal asset paths in one call. Returns per-item "
			     "success/error receipts plus MCP image content for each rendered PNG."),
			S::Object(
				{
					{TEXT("asset_paths"), S::Array(S::String(TEXT("Content-path strings; capped at 50.")))},
					{TEXT("max_width"), S::Integer(TEXT("Maximum thumbnail width in pixels. Default 256; range 64-2048."))},
					{TEXT("max_height"), S::Integer(TEXT("Maximum thumbnail height in pixels. Default 256; range 64-2048."))}
				},
				{TEXT("asset_paths")}),
			&RunBatchAssetThumbnails
		});

		Registry.Register({
			TEXT("asset_ingest_from_disk"),
			TEXT("Import one disk file through UE AssetTools and return native object/package readback."),
			S::Object(
				{
					{TEXT("source_path"), S::String(TEXT("Absolute disk path of the source file (.fbx/.png/.wav/...)."))},
					{TEXT("target_folder"), S::String(TEXT("Content-path folder, e.g. /Game/Incoming."))},
					{TEXT("replace_existing"), S::Boolean(TEXT("Replace an existing asset. Default true."))},
					{TEXT("save"), S::Boolean(TEXT("Save imported packages. Default true."))},
					{TEXT("automated"), S::Boolean(TEXT("Use automated import without dialogs. Default true."))}
				},
				{TEXT("source_path"), TEXT("target_folder")}),
			&RunAssetIngestFromDisk
		});

		Registry.Register({
			TEXT("pcg_snapshot_hash"),
			TEXT("Compatibility fingerprint for legacy callers. Not a content hash, not stable across days, "
			     "and not usable for restore; use pcg_graph_snapshot for real snapshots."),
			S::Object(
				{
					{TEXT("graph_path"), S::String(TEXT("Content path of the PCG graph asset."))}
				},
				{TEXT("graph_path")}),
			&RunPcgSnapshotHash
		});

		Registry.Register({
			TEXT("pcg_snapshot_restore"),
			TEXT("Restore a legacy snapshot_id to a PCG graph target with overwrite protection, save, and type readback."),
			S::Object(
				{
					{TEXT("graph_path"), S::String()},
					{TEXT("snapshot_id"), S::String(TEXT("Snapshot tag or full /Game snapshot asset path."))},
					{TEXT("force"), S::Boolean(TEXT("Replace an existing target graph. Default false."))}
				},
				{TEXT("graph_path"), TEXT("snapshot_id")}),
			&RunPcgSnapshotRestore
		});

		Registry.Register({
			TEXT("pcg_troubleshoot"),
			TEXT("Scan a PCG graph for common troubleshooting issues: missing settings, empty graphs, "
			     "unconnected likely-required input pins, broken edges, and pin type mismatches. "
			     "Returns a fail-closed issue receipt and recommended follow-up tools."),
			S::Object(
				{
					{TEXT("graph_path"), S::String()}
				},
				{TEXT("graph_path")}),
			&RunPcgTroubleshoot
		});

		Registry.Register({
			TEXT("pcg_validate_hook"),
			TEXT("Hook-stage-tagged validation for a PCG graph — runs the same node/pin/edge "
			     "checks as pcg_graph_validate (dangling required pins, type mismatches, missing "
			     "settings CDOs) and returns a {passed, error_count, warning_count, findings[]} "
			     "envelope. Stage is one of pre_generation | post_generation | post_compile. "
			     "min_severity (\"error\" default | \"warning\") controls what shows up in findings; "
			     "passed is always (error_count == 0). Soft-fails to passed=false + a synthetic "
			     "finding when the asset cannot be loaded so DAG callers never crash."),
			S::Object(
				{
					{TEXT("asset_path"), S::String(TEXT("Content path of the PCG graph (or wrapper). Alias: graph_path."))},
					{TEXT("graph_path"), S::String(TEXT("Legacy alias of asset_path."))},
					{TEXT("hook_stage"), S::String(TEXT("pre_generation | post_generation | post_compile (default pre_generation)"))},
					{TEXT("min_severity"), S::String(TEXT("error (default) | warning — filters which findings are surfaced."))}
				},
				{}),
			&RunPcgValidateHook
		});
	}
}
