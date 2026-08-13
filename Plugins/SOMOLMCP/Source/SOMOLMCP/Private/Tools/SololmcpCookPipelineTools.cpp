// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// ============================================================================
// SololmcpCookPipelineTools.cpp  (P5)
// ----------------------------------------------------------------------------
// Cook pipeline / asset hygiene tools for the SOMOLMCP UE plugin.
//
// Five tools registered via RegisterCookPipelineTools(FSololmcpToolRegistry&):
//
//   cook_pipeline_clean              — Remove <Project>/Saved/Cooked/<platform>/.
//   cook_pipeline_validate_assets    — Walk AssetRegistry under filter for
//                                       missing references / orphan redirectors.
//   cook_pipeline_size_report        — Group cooked output by extension; report
//                                       total + per-class + top-N offenders.
//   cook_pipeline_set_chunk_assignment — Best-effort: write a [ChunkAssignment]
//                                       section into <Project>/Config/DefaultGame.ini.
//   cook_pipeline_dependency_graph   — Walk IAssetRegistry::GetDependencies up
//                                       to max_depth; return nodes/edges.
//
// HARD CONSTRAINTS honored:
//   - Does NOT modify SololmcpToolRegistry.{h,cpp}.
//   - Does NOT modify .Build.cs (audit only — see P5_DELIVERY.md).
//   - Does NOT modify any other .cpp file.
//
// File location for integration:
// then add a forward declaration of RegisterCookPipelineTools() to
// SololmcpToolRegistry.h and a call site in SololmcpToolRegistry.cpp's
// constructor (per the existing RegisterX* pattern).
// ============================================================================

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

// Asset registry
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/ARFilter.h"
#include "SololmcpSharedLocks.h" // v3.10.x worker-safety: cached IAssetRegistry pointer

namespace UE::SOMOLMCP
{
namespace CookPipelineImpl
{
	// ------------------------------------------------------------------
	// Common helpers
	// ------------------------------------------------------------------

	// v3.10.x worker-safety: prefer the pre-cached pointer so workers don't call
	// FModuleManager::LoadModuleChecked (GameThread-only). Falls back to the old
	// path when called from the GameThread before Server::Start (defensive).
	static IAssetRegistry* GetAssetRegistry()
	{
		if (UE::SOMOLMCP::GSololmcpCachedAssetRegistry)
		{
			return UE::SOMOLMCP::GSololmcpCachedAssetRegistry;
		}
		FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return &Module.Get();
	}

	/** Recursive directory size visitor. Sum of all file sizes (bytes). */
	class FDirSizeVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		int64 TotalBytes = 0;
		int32 FileCount = 0;

		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
		{
			if (!bIsDirectory)
			{
				const int64 Sz = IFileManager::Get().FileSize(FilenameOrDirectory);
				if (Sz > 0)
				{
					TotalBytes += Sz;
					++FileCount;
				}
			}
			return true;
		}
	};

	/** Per-extension grouping visitor for size_report. */
	struct FExtBucket
	{
		int32 Count = 0;
		int64 Bytes = 0;
	};

	class FSizeReportVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		TMap<FString, FExtBucket> ByExt;
		// path → bytes, used to compute top offenders
		TArray<TPair<FString, int64>> AllFiles;
		int64 TotalBytes = 0;

		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
		{
			if (bIsDirectory)
			{
				return true;
			}
			const FString Path(FilenameOrDirectory);
			const int64 Sz = IFileManager::Get().FileSize(*Path);
			if (Sz <= 0)
			{
				return true;
			}
			TotalBytes += Sz;
			AllFiles.Emplace(Path, Sz);

			FString Ext = FPaths::GetExtension(Path).ToLower();
			if (Ext.IsEmpty()) Ext = TEXT("(noext)");
			FExtBucket& B = ByExt.FindOrAdd(Ext);
			B.Count++;
			B.Bytes += Sz;
			return true;
		}
	};

	static double BytesToMB(int64 Bytes)
	{
		return static_cast<double>(Bytes) / (1024.0 * 1024.0);
	}

	// ------------------------------------------------------------------
	// 1) cook_pipeline_clean
	// ------------------------------------------------------------------
	static bool RunClean(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString TargetPlatform = TEXT("Windows");
		Arguments->TryGetStringField(TEXT("target_platform"), TargetPlatform);

		const FString CookedRoot = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Cooked"), TargetPlatform));

		IFileManager& FM = IFileManager::Get();
		OutStructured->SetStringField(TEXT("target_platform"), TargetPlatform);
		OutStructured->SetStringField(TEXT("path"), CookedRoot);

		if (!FM.DirectoryExists(*CookedRoot))
		{
			OutStructured->SetBoolField(TEXT("cleaned"), false);
			OutStructured->SetNumberField(TEXT("freed_mb"), 0);
			OutStructured->SetStringField(TEXT("note"), TEXT("Cooked directory does not exist; nothing to clean."));
			OutSummary = FString::Printf(TEXT("No cooked directory at %s."), *CookedRoot);
			return true;
		}

		// Tally before delete.
		FDirSizeVisitor Visitor;
		FM.IterateDirectoryRecursively(*CookedRoot, Visitor);
		const int64 BytesBefore = Visitor.TotalBytes;

		const bool bDeleted = FM.DeleteDirectory(*CookedRoot, /*RequireExists*/ false, /*Tree*/ true);
		if (!bDeleted)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("DeleteDirectory failed; check write permissions or running editor handles."));
			OutStructured->SetBoolField(TEXT("cleaned"), false);
			OutStructured->SetNumberField(TEXT("freed_mb"), 0);
			OutError = FString::Printf(TEXT("Failed to delete %s."), *CookedRoot);
			return false;
		}
		if (FM.DirectoryExists(*CookedRoot))
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("DeleteDirectory returned success, but the cooked directory still exists."));
			OutStructured->SetBoolField(TEXT("cleaned"), false);
			OutStructured->SetNumberField(TEXT("freed_mb"), 0);
			OutError = FString::Printf(TEXT("Delete verification failed for %s."), *CookedRoot);
			return false;
		}

		OutStructured->SetBoolField(TEXT("cleaned"), true);
		OutStructured->SetNumberField(TEXT("freed_mb"), BytesToMB(BytesBefore));
		OutStructured->SetNumberField(TEXT("files_removed"), Visitor.FileCount);
		OutSummary = FString::Printf(TEXT("Cleaned %s (%.2f MB, %d files)."),
			*TargetPlatform, BytesToMB(BytesBefore), Visitor.FileCount);
		return true;
	}

	// ------------------------------------------------------------------
	// 2) cook_pipeline_validate_assets
	// ------------------------------------------------------------------
	static bool RunValidateAssets(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		IAssetRegistry* AR = GetAssetRegistry();
		if (!AR)
		{
			SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT(""),
				TEXT("AssetRegistry module is unavailable."));
			OutError = TEXT("AssetRegistry unavailable.");
			return false;
		}

		FString PathFilter = TEXT("/Game");
		Arguments->TryGetStringField(TEXT("path_filter"), PathFilter);
		if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

		// Build a filter limited to the requested subtree (recursive).
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*PathFilter));

		TArray<FAssetData> Assets;
		AR->GetAssets(Filter, Assets);

		int32 TotalScanned = Assets.Num();
		TArray<FString> MissingRefs;
		TArray<FString> OrphanRedirectors;
		int32 WarningsCount = 0;

		// Scan each asset's outgoing dependencies; if any dependency package
		// can't be resolved on disk → that's a "missing ref".
		// Also check redirectors: a redirector with no incoming referencers is
		// considered an "orphan".
		for (const FAssetData& AD : Assets)
		{
			const FString PackageName = AD.PackageName.ToString();

			// Outgoing deps
			TArray<FName> Deps;
			AR->GetDependencies(AD.PackageName, Deps);
			for (const FName& DepName : Deps)
			{
				const FString DepStr = DepName.ToString();
				// /Script/... and /Engine/... are always-resolvable runtime packages.
				if (DepStr.StartsWith(TEXT("/Script/")) || DepStr.StartsWith(TEXT("/Engine/")))
				{
					continue;
				}
				FString Filename;
				const bool bResolved = FPackageName::DoesPackageExist(DepStr, &Filename);
				if (!bResolved)
				{
					MissingRefs.Add(FString::Printf(TEXT("%s -> %s"), *PackageName, *DepStr));
					++WarningsCount;
				}
			}

			// Orphan redirector check
			if (AD.AssetClassPath.GetAssetName() == FName(TEXT("ObjectRedirector")))
			{
				TArray<FName> Referencers;
				AR->GetReferencers(AD.PackageName, Referencers);
				if (Referencers.Num() == 0)
				{
					OrphanRedirectors.Add(PackageName);
					++WarningsCount;
				}
			}
		}

		OutStructured->SetNumberField(TEXT("total_scanned"), TotalScanned);

		auto SetStrArr = [&](const FString& Field, const TArray<FString>& Items, int32 Cap = 200)
		{
			TArray<TSharedPtr<FJsonValue>> Json;
			const int32 N = FMath::Min(Items.Num(), Cap);
			for (int32 i = 0; i < N; ++i)
			{
				Json.Add(MakeShared<FJsonValueString>(Items[i]));
			}
			OutStructured->SetArrayField(Field, Json);
		};
		SetStrArr(TEXT("missing_refs"), MissingRefs);
		SetStrArr(TEXT("orphan_redirectors"), OrphanRedirectors);
		OutStructured->SetNumberField(TEXT("warnings_count"), WarningsCount);
		OutStructured->SetStringField(TEXT("path_filter"), PathFilter);
		if (MissingRefs.Num() > 200 || OrphanRedirectors.Num() > 200)
		{
			OutStructured->SetBoolField(TEXT("truncated"), true);
		}

		OutSummary = FString::Printf(
			TEXT("Validated %d assets under %s — missing_refs=%d orphan_redirectors=%d warnings=%d."),
			TotalScanned, *PathFilter, MissingRefs.Num(), OrphanRedirectors.Num(), WarningsCount);
		return true;
	}

	// ------------------------------------------------------------------
	// 3) cook_pipeline_size_report
	// ------------------------------------------------------------------
	static bool RunSizeReport(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString TargetPlatform;
		if (!Arguments->TryGetStringField(TEXT("target_platform"), TargetPlatform) || TargetPlatform.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("target_platform"));
			OutError = TEXT("Missing target_platform.");
			return false;
		}

		const FString CookedRoot = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Cooked"), TargetPlatform));
		OutStructured->SetStringField(TEXT("path"), CookedRoot);
		OutStructured->SetStringField(TEXT("target_platform"), TargetPlatform);

		IFileManager& FM = IFileManager::Get();
		if (!FM.DirectoryExists(*CookedRoot))
		{
			SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT(""),
				FString::Printf(TEXT("Cooked output for '%s' does not exist. Run cook_target first."), *TargetPlatform));
			OutStructured->SetNumberField(TEXT("total_size_mb"), 0);
			TArray<TSharedPtr<FJsonValue>> Empty;
			OutStructured->SetArrayField(TEXT("by_class"), Empty);
			OutStructured->SetArrayField(TEXT("top_offenders"), Empty);
			OutError = FString::Printf(TEXT("No cooked dir at %s."), *CookedRoot);
			return false;
		}

		FSizeReportVisitor Visitor;
		FM.IterateDirectoryRecursively(*CookedRoot, Visitor);

		// by_class array: [{class, count, size_mb}]
		TArray<TSharedPtr<FJsonValue>> ByClassJson;
		ByClassJson.Reserve(Visitor.ByExt.Num());
		for (const TPair<FString, FExtBucket>& Pair : Visitor.ByExt)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("class"), Pair.Key);
			Row->SetNumberField(TEXT("count"), Pair.Value.Count);
			Row->SetNumberField(TEXT("size_mb"), BytesToMB(Pair.Value.Bytes));
			ByClassJson.Add(MakeShared<FJsonValueObject>(Row));
		}
		// Sort descending by size_mb
		ByClassJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const double SA = A->AsObject()->GetNumberField(TEXT("size_mb"));
			const double SB = B->AsObject()->GetNumberField(TEXT("size_mb"));
			return SA > SB;
		});

		// top_offenders: top 25 files by size
		Visitor.AllFiles.Sort([](const TPair<FString, int64>& A, const TPair<FString, int64>& B)
		{
			return A.Value > B.Value;
		});
		TArray<TSharedPtr<FJsonValue>> TopJson;
		const int32 TopN = FMath::Min(25, Visitor.AllFiles.Num());
		for (int32 i = 0; i < TopN; ++i)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), Visitor.AllFiles[i].Key);
			Row->SetNumberField(TEXT("size_mb"), BytesToMB(Visitor.AllFiles[i].Value));
			TopJson.Add(MakeShared<FJsonValueObject>(Row));
		}

		OutStructured->SetNumberField(TEXT("total_size_mb"), BytesToMB(Visitor.TotalBytes));
		OutStructured->SetArrayField(TEXT("by_class"), ByClassJson);
		OutStructured->SetArrayField(TEXT("top_offenders"), TopJson);
		OutStructured->SetNumberField(TEXT("file_count"), Visitor.AllFiles.Num());

		OutSummary = FString::Printf(TEXT("Cooked[%s] total=%.2f MB across %d files (%d ext groups)."),
			*TargetPlatform, BytesToMB(Visitor.TotalBytes), Visitor.AllFiles.Num(), Visitor.ByExt.Num());
		return true;
	}

	// ------------------------------------------------------------------
	// 4) cook_pipeline_set_chunk_assignment
	// ------------------------------------------------------------------
	static bool RunSetChunkAssignment(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		// asset_paths: array of strings
		const TArray<TSharedPtr<FJsonValue>>* RawPaths = nullptr;
		if (!Arguments->TryGetArrayField(TEXT("asset_paths"), RawPaths) || !RawPaths || RawPaths->IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("asset_paths"));
			OutError = TEXT("Missing or empty 'asset_paths'.");
			return false;
		}
		int32 ChunkId = -1;
		if (!Arguments->TryGetNumberField(TEXT("chunk_id"), ChunkId) || ChunkId < 0)
		{
			SololmcpError::MissingParam(OutStructured, TEXT("chunk_id"));
			OutError = TEXT("Missing or invalid 'chunk_id' (non-negative integer required).");
			return false;
		}

		TArray<FString> AssetPaths;
		AssetPaths.Reserve(RawPaths->Num());
		for (const TSharedPtr<FJsonValue>& V : *RawPaths)
		{
			if (!V.IsValid()) continue;
			FString S;
			if (V->TryGetString(S) && !S.IsEmpty())
			{
				AssetPaths.Add(S);
			}
		}
		if (AssetPaths.IsEmpty())
		{
			SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("asset_paths"),
				TEXT("'asset_paths' must contain at least one non-empty string."));
			OutError = TEXT("Empty asset_paths after parsing.");
			return false;
		}

		const FString IniPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini")));

		// Best-effort approach: write to GConfig under a synthetic
		// "/Script/UnrealEd.ChunkDependencyInfo" + per-asset key. Cook chunking
		// in UE is normally driven by AssetManager / PrimaryDataAssets; this
		// tool exposes a simple INI-level recording so external tooling and
		// future cook hooks can pick it up. The exact section name follows the
		// "[ChunkAssignment]" convention requested by the P5 spec.
		const FString SectionName = TEXT("ChunkAssignment");

		// Use direct INI file edit so the section appears verbatim in the file
		// (GConfig will normalize odd section names). We append-or-update.
		TArray<FString> ExistingLines;
		FFileHelper::LoadFileToStringArray(ExistingLines, *IniPath);

		// Find the section start/end.
		const FString SectionHeader = FString::Printf(TEXT("[%s]"), *SectionName);
		int32 SectionStart = INDEX_NONE;
		int32 SectionEnd = INDEX_NONE; // exclusive
		for (int32 i = 0; i < ExistingLines.Num(); ++i)
		{
			if (ExistingLines[i].TrimStartAndEnd().Equals(SectionHeader))
			{
				SectionStart = i;
				SectionEnd = ExistingLines.Num();
				for (int32 j = i + 1; j < ExistingLines.Num(); ++j)
				{
					const FString T = ExistingLines[j].TrimStartAndEnd();
					if (T.StartsWith(TEXT("[")) && T.EndsWith(TEXT("]")))
					{
						SectionEnd = j;
						break;
					}
				}
				break;
			}
		}

		// Build new section content: keep non-matching keys, then append/replace ours.
		TArray<FString> SectionLines;
		if (SectionStart != INDEX_NONE)
		{
			for (int32 i = SectionStart + 1; i < SectionEnd; ++i)
			{
				SectionLines.Add(ExistingLines[i]);
			}
		}

		auto KeyForAsset = [](const FString& AssetPath) -> FString
		{
			// Sanitize / and . to be config-friendly; preserve readability.
			FString Key = AssetPath;
			Key.ReplaceInline(TEXT("/"), TEXT("_"));
			Key.ReplaceInline(TEXT("."), TEXT("_"));
			if (Key.StartsWith(TEXT("_"))) Key.RemoveAt(0);
			return FString::Printf(TEXT("Chunk_%s"), *Key);
		};

		int32 UpdatedCount = 0;
		for (const FString& AssetPath : AssetPaths)
		{
			const FString Key = KeyForAsset(AssetPath);
			const FString NewLine = FString::Printf(TEXT("%s=(AssetPath=\"%s\",ChunkId=%d)"),
				*Key, *AssetPath, ChunkId);

			bool bReplaced = false;
			for (int32 i = 0; i < SectionLines.Num(); ++i)
			{
				const FString T = SectionLines[i].TrimStartAndEnd();
				if (T.StartsWith(Key + TEXT("=")))
				{
					SectionLines[i] = NewLine;
					bReplaced = true;
					break;
				}
			}
			if (!bReplaced)
			{
				SectionLines.Add(NewLine);
			}
			++UpdatedCount;
		}

		// Splice back together.
		TArray<FString> NewLines;
		if (SectionStart == INDEX_NONE)
		{
			NewLines = ExistingLines;
			if (NewLines.Num() > 0 && !NewLines.Last().IsEmpty())
			{
				NewLines.Add(TEXT(""));
			}
			NewLines.Add(SectionHeader);
			NewLines.Append(SectionLines);
		}
		else
		{
			for (int32 i = 0; i < SectionStart; ++i) NewLines.Add(ExistingLines[i]);
			NewLines.Add(SectionHeader);
			NewLines.Append(SectionLines);
			for (int32 i = SectionEnd; i < ExistingLines.Num(); ++i) NewLines.Add(ExistingLines[i]);
		}

		// Ensure parent dir.
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(IniPath), /*Tree*/ true);
		const bool bWrote = FFileHelper::SaveStringArrayToFile(NewLines, *IniPath);
		if (!bWrote)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("Failed to write DefaultGame.ini. Check permissions / source-control."));
			OutStructured->SetNumberField(TEXT("updated_count"), 0);
			OutStructured->SetStringField(TEXT("ini_path"), IniPath);
			OutError = FString::Printf(TEXT("SaveStringArrayToFile failed for %s."), *IniPath);
			return false;
		}
		if (!FPaths::FileExists(IniPath))
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("DefaultGame.ini write reported success, but the file does not exist."));
			OutStructured->SetNumberField(TEXT("updated_count"), 0);
			OutStructured->SetStringField(TEXT("ini_path"), IniPath);
			OutError = FString::Printf(TEXT("Write verification failed for %s."), *IniPath);
			return false;
		}

		TArray<FString> VerifyLines;
		const bool bReadBack = FFileHelper::LoadFileToStringArray(VerifyLines, *IniPath);
		if (!bReadBack)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
				TEXT("DefaultGame.ini was written but could not be read back for verification."));
			OutStructured->SetNumberField(TEXT("updated_count"), 0);
			OutStructured->SetStringField(TEXT("ini_path"), IniPath);
			OutError = FString::Printf(TEXT("Read-back verification failed for %s."), *IniPath);
			return false;
		}

		for (const FString& AssetPath : AssetPaths)
		{
			const FString Key = KeyForAsset(AssetPath);
			const FString ExpectedLine = FString::Printf(TEXT("%s=(AssetPath=\"%s\",ChunkId=%d)"),
				*Key, *AssetPath, ChunkId);
			bool bFoundExpectedLine = false;
			for (const FString& VerifyLine : VerifyLines)
			{
				if (VerifyLine.TrimStartAndEnd().Equals(ExpectedLine))
				{
					bFoundExpectedLine = true;
					break;
				}
			}
			if (!bFoundExpectedLine)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					FString::Printf(TEXT("DefaultGame.ini write verification missed assignment for %s."), *AssetPath));
				OutStructured->SetNumberField(TEXT("updated_count"), 0);
				OutStructured->SetStringField(TEXT("ini_path"), IniPath);
				OutError = FString::Printf(TEXT("Chunk assignment verification failed for %s."), *AssetPath);
				return false;
			}
		}

		// Reload GConfig view so subsequent reads are current.
		if (GConfig)
		{
			GConfig->LoadFile(IniPath);
		}

		OutStructured->SetNumberField(TEXT("updated_count"), UpdatedCount);
		OutStructured->SetStringField(TEXT("ini_path"), IniPath);
		OutStructured->SetNumberField(TEXT("chunk_id"), ChunkId);
		OutStructured->SetStringField(TEXT("section"), SectionName);
		OutSummary = FString::Printf(TEXT("Wrote %d ChunkAssignment entries (chunk_id=%d) to %s."),
			UpdatedCount, ChunkId, *IniPath);
		return true;
	}

	// ------------------------------------------------------------------
	// 5) cook_pipeline_dependency_graph
	// ------------------------------------------------------------------
	static bool RunDependencyGraph(
		const FSololmcpToolExecutionContext& /*Context*/,
		const TSharedRef<FJsonObject>& Arguments,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutSummary,
		FString& OutError)
	{
		FString AssetPath;
		if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
			OutError = TEXT("Missing asset_path.");
			return false;
		}
		int32 MaxDepth = 3;
		Arguments->TryGetNumberField(TEXT("max_depth"), MaxDepth);
		if (MaxDepth < 0) MaxDepth = 0;
		if (MaxDepth > 16) MaxDepth = 16; // safety cap

		IAssetRegistry* AR = GetAssetRegistry();
		if (!AR)
		{
			SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT(""),
				TEXT("AssetRegistry module is unavailable."));
			OutError = TEXT("AssetRegistry unavailable.");
			return false;
		}

		// Convert asset_path -> package name (strip object suffix if present).
		FString PackageName = AssetPath;
		int32 DotIdx;
		if (PackageName.FindChar(TEXT('.'), DotIdx))
		{
			PackageName = PackageName.Left(DotIdx);
		}
		const FName RootPkg(*PackageName);

		// BFS
		TSet<FName> Visited;
		TArray<TPair<FName, FName>> Edges;
		TArray<TPair<FName, int32>> Frontier; // (pkg, depth)
		Visited.Add(RootPkg);
		Frontier.Emplace(RootPkg, 0);

		bool bDepthCapHit = false;
		const int32 SafetyNodeCap = 5000;

		while (!Frontier.IsEmpty())
		{
			const TPair<FName, int32> Cur = Frontier[0];
			Frontier.RemoveAt(0);

			if (Cur.Value >= MaxDepth)
			{
				continue;
			}
			TArray<FName> Deps;
			AR->GetDependencies(Cur.Key, Deps);
			for (const FName& Dep : Deps)
			{
				Edges.Emplace(Cur.Key, Dep);
				if (!Visited.Contains(Dep))
				{
					Visited.Add(Dep);
					if (Visited.Num() >= SafetyNodeCap)
					{
						bDepthCapHit = true;
						continue;
					}
					Frontier.Emplace(Dep, Cur.Value + 1);
				}
				if (Cur.Value + 1 >= MaxDepth)
				{
					bDepthCapHit = true;
				}
			}
		}

		// Emit nodes.
		TArray<TSharedPtr<FJsonValue>> NodesJson;
		NodesJson.Reserve(Visited.Num());
		for (const FName& N : Visited)
		{
			TSharedRef<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("package"), N.ToString());
			NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		// Emit edges.
		TArray<TSharedPtr<FJsonValue>> EdgesJson;
		EdgesJson.Reserve(Edges.Num());
		for (const TPair<FName, FName>& E : Edges)
		{
			TSharedRef<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
			EdgeObj->SetStringField(TEXT("from"), E.Key.ToString());
			EdgeObj->SetStringField(TEXT("to"), E.Value.ToString());
			EdgesJson.Add(MakeShared<FJsonValueObject>(EdgeObj));
		}

		OutStructured->SetArrayField(TEXT("nodes"), NodesJson);
		OutStructured->SetArrayField(TEXT("edges"), EdgesJson);
		OutStructured->SetNumberField(TEXT("total_nodes"), Visited.Num());
		OutStructured->SetNumberField(TEXT("total_edges"), Edges.Num());
		OutStructured->SetBoolField(TEXT("max_depth_reached"), bDepthCapHit);
		OutStructured->SetNumberField(TEXT("max_depth"), MaxDepth);
		OutStructured->SetStringField(TEXT("root_package"), PackageName);

		OutSummary = FString::Printf(TEXT("Dep graph from %s: %d nodes, %d edges (max_depth=%d, cap_hit=%s)."),
			*PackageName, Visited.Num(), Edges.Num(), MaxDepth, bDepthCapHit ? TEXT("yes") : TEXT("no"));
		return true;
	}

} // namespace CookPipelineImpl

// ============================================================================
// Registration
// ============================================================================
void RegisterCookPipelineTools(FSololmcpToolRegistry& Registry)
{
	using FSB = FSololmcpSchemaBuilder;

	// 1) cook_pipeline_clean
	Registry.Register({
		TEXT("cook_pipeline_clean"),
		TEXT("Remove the project's cooked output for a target platform "
		     "(<Project>/Saved/Cooked/<platform>/). Returns cleaned + freed_mb."),
		FSB::Object(
			{
				{TEXT("target_platform"), FSB::String(
					TEXT("Target platform folder name. Defaults to 'Windows'."),
					{TEXT("Windows"), TEXT("WindowsServer"), TEXT("Linux"), TEXT("Mac"), TEXT("Android"), TEXT("iOS")})}
			},
			/*required*/ {}),
		&CookPipelineImpl::RunClean,
		nullptr,
		/*CacheTtlSeconds*/ 0
	});

	// 2) cook_pipeline_validate_assets
	Registry.Register({
		TEXT("cook_pipeline_validate_assets"),
		TEXT("Walk the AssetRegistry under an optional /Game subpath, checking outgoing "
		     "dependencies for missing references and listing redirector packages with no "
		     "incoming referencers. Returns counts plus first-N samples."),
		FSB::Object(
			{
				{TEXT("path_filter"), FSB::String(TEXT("Optional /Game subpath (default '/Game'). Recursive."))}
			},
			/*required*/ {}),
		&CookPipelineImpl::RunValidateAssets,
		nullptr,
		/*CacheTtlSeconds*/ 0
	});

	// 3) cook_pipeline_size_report
	Registry.Register({
		TEXT("cook_pipeline_size_report"),
		TEXT("Walk the cooked output directory for a platform, group files by extension, "
		     "and report the top-25 largest individual files (top_offenders)."),
		FSB::Object(
			{
				{TEXT("target_platform"), FSB::String(
					TEXT("Target platform folder name (e.g. 'Windows', 'Linux')."),
					{TEXT("Windows"), TEXT("WindowsServer"), TEXT("Linux"), TEXT("Mac"), TEXT("Android"), TEXT("iOS")})}
			},
			{TEXT("target_platform")}),
		&CookPipelineImpl::RunSizeReport,
		nullptr,
		/*CacheTtlSeconds*/ 0
	});

	// 4) cook_pipeline_set_chunk_assignment
	Registry.Register({
		TEXT("cook_pipeline_set_chunk_assignment"),
		TEXT("Best-effort: append/update entries in [ChunkAssignment] inside <Project>/Config/DefaultGame.ini "
		     "mapping each asset_path to chunk_id. Returns updated_count + ini_path. "
		     "Note: the cook itself still uses AssetManager/PrimaryDataAsset rules — these entries are an "
		     "INI-level record consumed by external tooling / future cook hooks."),
		FSB::Object(
			{
				{TEXT("asset_paths"), FSB::Array(FSB::String(), TEXT("Asset paths (e.g. /Game/Maps/Demo)."))},
				{TEXT("chunk_id"),    FSB::Integer(TEXT("Non-negative chunk id."))}
			},
			{TEXT("asset_paths"), TEXT("chunk_id")}),
		&CookPipelineImpl::RunSetChunkAssignment,
		nullptr,
		/*CacheTtlSeconds*/ 0
	});

	// 5) cook_pipeline_dependency_graph
	Registry.Register({
		TEXT("cook_pipeline_dependency_graph"),
		TEXT("BFS the AssetRegistry dependency graph rooted at asset_path up to max_depth (default 3, capped at 16). "
		     "Returns nodes, edges, total_nodes, max_depth_reached. Safety cap: 5000 unique nodes."),
		FSB::Object(
			{
				{TEXT("asset_path"), FSB::String(TEXT("Root asset (e.g. /Game/Maps/Demo or /Game/Foo.Foo)."))},
				{TEXT("max_depth"),  FSB::Integer(TEXT("Maximum BFS depth (default 3, max 16)."))}
			},
			{TEXT("asset_path")}),
		&CookPipelineImpl::RunDependencyGraph,
		nullptr,
		/*CacheTtlSeconds*/ 0
	});
}

} // namespace UE::SOMOLMCP
