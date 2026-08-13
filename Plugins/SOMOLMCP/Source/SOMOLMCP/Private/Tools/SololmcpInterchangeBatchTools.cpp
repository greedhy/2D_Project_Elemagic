// Copyright 2026 SOMOLAGENT. All Rights Reserved.
//
// Interchange coverage — Layer D (batch/production) and Layer E (diagnostics),
// batch 4.
//
// This is the layer a long queued session actually spends its time in: scan a
// directory, import everything importable, verify what landed, relink moved
// sources, reimport what drifted. Each tool takes one game-thread entry for N
// files rather than N entries, which is the only lever that moves throughput on
// a game-thread-bound editor with a small concurrent job budget.
//
// UAssetImportData (ExtractFilenames / GetFirstFilename / UpdateFilenameOnly /
// AddFileName / SourceData) was verified identical on UE 5.3 through 5.8, so the
// receipt and relink tools need no engine gates. The import calls themselves reuse
// the same UInterchangeManager entry points as batch 1, including its 5.5+
// OutImportedObjects split.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "Services/SololmcpEditorServices.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"
#include "EditorFramework/AssetImportData.h"
#include "Runtime/Launch/Resources/Version.h"

#if defined(SOMOLMCP_HAS_INTERCHANGEENGINE) && SOMOLMCP_HAS_INTERCHANGEENGINE
#define SOMOLMCP_WITH_INTERCHANGE_BATCH 1
#else
#define SOMOLMCP_WITH_INTERCHANGE_BATCH 0
#endif

#if SOMOLMCP_WITH_INTERCHANGE_BATCH
#include "InterchangeManager.h"
#include "InterchangeSourceData.h"
#include "InterchangeResult.h"
#include "InterchangeResultsContainer.h"
#endif

#define SOMOLMCP_IXB_HAS_OUT_OBJECTS \
	(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5))
#define SOMOLMCP_IXB_HAS_DEST_NAME \
	(ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4))

namespace UE::SOMOLMCP
{
namespace InterchangeBatchToolsPrivate
{
	inline FString EngineVersionString()
	{
		return FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	}

	inline bool RefuseNoModule(
		const TSharedRef<FJsonObject>& OutStructured, FString& OutError, const TCHAR* ToolName)
	{
		SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE_ON_ENGINE"), TEXT(""),
			TEXT("This build was configured without InterchangeEngine."));
		OutStructured->SetStringField(TEXT("tool"), ToolName);
		OutStructured->SetStringField(TEXT("required_module"), TEXT("InterchangeEngine"));
		OutStructured->SetStringField(TEXT("engine_version"), EngineVersionString());
		OutStructured->SetBoolField(TEXT("ok"), false);
		OutError = TEXT("InterchangeEngine is not available in this build.");
		return false;
	}

	inline void AddStringArray(
		const TSharedRef<FJsonObject>& Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		Json.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(Field, Json);
	}

	/** Read the extension filter argument, normalized to lowercase without dots. */
	inline TArray<FString> ReadExtensionFilter(const TSharedRef<FJsonObject>& Args)
	{
		TArray<FString> Extensions;
		const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
		if (Args->TryGetArrayField(TEXT("extensions"), Raw) && Raw != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Raw)
			{
				FString Extension;
				if (Value.IsValid() && Value->TryGetString(Extension) && !Extension.IsEmpty())
				{
					Extensions.Add(Extension.Replace(TEXT("."), TEXT("")).ToLower());
				}
			}
		}
		return Extensions;
	}

	/** Enumerate candidate source files under a directory. */
	inline void ScanDirectory(
		const FString& Directory,
		const bool bRecursive,
		const TArray<FString>& Extensions,
		TArray<FString>& OutFiles)
	{
		TArray<FString> Found;
		if (bRecursive)
		{
			IFileManager::Get().FindFilesRecursive(Found, *Directory, TEXT("*.*"), true, false);
		}
		else
		{
			IFileManager::Get().FindFiles(Found, *(Directory / TEXT("*.*")), true, false);
			for (FString& File : Found)
			{
				File = Directory / File;
			}
		}

		for (const FString& File : Found)
		{
			if (Extensions.Num() > 0)
			{
				const FString Extension = FPaths::GetExtension(File).ToLower();
				if (!Extensions.Contains(Extension))
				{
					continue;
				}
			}
			OutFiles.Add(FPaths::ConvertRelativePathToFull(File));
		}
		OutFiles.Sort();
	}

#if SOMOLMCP_WITH_INTERCHANGE_BATCH
	inline UInterchangeManager& Manager()
	{
		return UInterchangeManager::GetInterchangeManager();
	}

	/** Find the UAssetImportData on an arbitrary asset, if it has one. */
	inline UAssetImportData* FindAssetImportData(UObject* Asset)
	{
		if (Asset == nullptr)
		{
			return nullptr;
		}
		// Assets expose AssetImportData as a UPROPERTY of varying declared type
		// (UAssetImportData, UFbxAssetImportData, UInterchangeAssetImportData, ...),
		// so it is resolved reflectively instead of by casting to each asset class.
		for (TFieldIterator<FObjectProperty> It(Asset->GetClass()); It; ++It)
		{
			const FObjectProperty* Property = *It;
			if (Property->GetName() != TEXT("AssetImportData"))
			{
				continue;
			}
			UObject* Value = Property->GetObjectPropertyValue_InContainer(Asset);
			if (UAssetImportData* ImportData = Cast<UAssetImportData>(Value))
			{
				return ImportData;
			}
		}
		return nullptr;
	}

	/**
	 * Import diagnostics capture (Layer E).
	 *
	 * Interchange reports translator and factory messages through a results
	 * container delivered on UInterchangeManager::OnBatchImportComplete, not through
	 * the ImportAsset return value — a "successful" import can still carry warnings
	 * and per-asset errors. Nothing retains that container once the delegate has
	 * fired, so a queued workload has no way to find out what a wave actually
	 * complained about unless the delegate is held from before the wave starts.
	 * Capture is therefore explicit (interchange_result_capture_start) rather than
	 * implicit: binding lazily on first read would miss the very wave being asked
	 * about. Verified stable on UE 5.3 through 5.8.
	 */
	struct FResultCapture
	{
		struct FEntry
		{
			FString Severity;
			FString Message;
			FString SourceAsset;
			FString DestinationAsset;
			FString ResultClass;
			FDateTime CapturedAt;
		};

		bool bBound = false;
		FDelegateHandle Handle;
		TArray<FEntry> Entries;
		int32 MaxEntries = 2000;
		int32 DroppedCount = 0;
		FDateTime StartedAt;

		static FResultCapture& Get()
		{
			static FResultCapture Instance;
			return Instance;
		}
	};

#if SOMOLMCP_WITH_INTERCHANGE_BATCH
	inline FString ClassifySeverity(const UInterchangeResult* Result)
	{
		if (Result == nullptr)
		{
			return TEXT("unknown");
		}
		if (Result->IsA<UInterchangeResultError>())   { return TEXT("error"); }
		if (Result->IsA<UInterchangeResultWarning>()) { return TEXT("warning"); }
		if (Result->IsA<UInterchangeResultSuccess>()) { return TEXT("success"); }
		return TEXT("info");
	}

	inline void OnBatchImportCompleteCaptured(TStrongObjectPtr<UInterchangeResultsContainer> Container)
	{
		FResultCapture& Capture = FResultCapture::Get();
		UInterchangeResultsContainer* Raw = Container.Get();
		if (Raw == nullptr)
		{
			return;
		}
		const FDateTime Now = FDateTime::UtcNow();
		for (const UInterchangeResult* Result : Raw->GetResults())
		{
			if (Result == nullptr)
			{
				continue;
			}
			if (Capture.Entries.Num() >= Capture.MaxEntries)
			{
				// A long queued session can emit far more messages than anyone will
				// read; drop the excess but report the count so truncation is visible.
				++Capture.DroppedCount;
				continue;
			}
			FResultCapture::FEntry Entry;
			Entry.Severity = ClassifySeverity(Result);
			Entry.Message = Result->GetText().ToString();
			Entry.SourceAsset = Result->SourceAssetName;
			Entry.DestinationAsset = Result->DestinationAssetName;
			Entry.ResultClass = Result->GetClass()->GetName();
			Entry.CapturedAt = Now;
			Capture.Entries.Add(MoveTemp(Entry));
		}
	}
#endif // SOMOLMCP_WITH_INTERCHANGE_BATCH

	/** Run one import, tolerating the 5.3/5.4 lack of an OutImportedObjects overload. */
	inline bool ImportOne(
		const FString& ContentPath,
		const FString& SourceFile,
		const bool bAutomated,
		TArray<FString>& OutImportedPaths,
		bool& bOutObjectsReported)
	{
		UInterchangeSourceData* SourceData = UInterchangeManager::CreateSourceData(SourceFile);
		if (SourceData == nullptr)
		{
			return false;
		}
		FImportAssetParameters Parameters;
		Parameters.bIsAutomated = bAutomated;

#if SOMOLMCP_IXB_HAS_OUT_OBJECTS
		TArray<UObject*> Imported;
		const bool bOk = Manager().ImportAsset(ContentPath, SourceData, Parameters, Imported);
		for (const UObject* Object : Imported)
		{
			if (Object != nullptr)
			{
				OutImportedPaths.Add(Object->GetPathName());
			}
		}
		bOutObjectsReported = true;
		return bOk;
#else
		bOutObjectsReported = false;
		return Manager().ImportAsset(ContentPath, SourceData, Parameters);
#endif
	}
#endif // SOMOLMCP_WITH_INTERCHANGE_BATCH
} // namespace InterchangeBatchToolsPrivate

void RegisterInterchangeBatchTools(FSololmcpToolRegistry& Registry)
{
	using namespace InterchangeBatchToolsPrivate;

	// ── interchange_directory_scan ─────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_directory_scan"),
		TEXT("List source files under a directory and report which ones Interchange can translate. "
			 "Read-only. Run this first to plan a queued import wave without spending job slots on "
			 "files that would be rejected."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("directory"), FSololmcpSchemaBuilder::String(TEXT("Directory to scan."))},
				{TEXT("recursive"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Descend into subdirectories.")), true)},
				{TEXT("extensions"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Extension without the dot, e.g. fbx.")),
					TEXT("Restrict to these extensions. Omit for all files."))},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Maximum files to report.")), 500)}
			},
			{TEXT("directory")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_directory_scan"));
#else
			FString Directory;
			if (!Args->TryGetStringField(TEXT("directory"), Directory) || Directory.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("directory"));
				OutError = TEXT("Missing directory.");
				return false;
			}
			const FString AbsoluteDir = FPaths::ConvertRelativePathToFull(Directory);
			if (!IFileManager::Get().DirectoryExists(*AbsoluteDir))
			{
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("directory"),
					FString::Printf(TEXT("No such directory: %s"), *AbsoluteDir));
				OutError = FString::Printf(TEXT("Directory not found: %s"), *AbsoluteDir);
				return false;
			}

			bool bRecursive = true;
			Args->TryGetBoolField(TEXT("recursive"), bRecursive);
			int32 Limit = 500;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			Limit = FMath::Clamp(Limit, 1, 20000);

			TArray<FString> Files;
			ScanDirectory(AbsoluteDir, bRecursive, ReadExtensionFilter(Args), Files);

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Translatable = 0;
			for (const FString& File : Files)
			{
				if (Rows.Num() >= Limit)
				{
					break;
				}
				UInterchangeSourceData* SourceData = UInterchangeManager::CreateSourceData(File);
				const bool bCanTranslate = SourceData != nullptr
					&& Manager().CanTranslateSourceData(SourceData);
				if (bCanTranslate)
				{
					++Translatable;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("file"), File);
				Row->SetStringField(TEXT("extension"), FPaths::GetExtension(File).ToLower());
				Row->SetNumberField(TEXT("size_bytes"),
					static_cast<double>(IFileManager::Get().FileSize(*File)));
				Row->SetBoolField(TEXT("can_translate"), bCanTranslate);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("files"), Rows);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("total_found"), Files.Num());
			OutStructured->SetNumberField(TEXT("translatable"), Translatable);
			OutStructured->SetBoolField(TEXT("truncated"), Files.Num() > Rows.Num());
			OutStructured->SetStringField(TEXT("directory"), AbsoluteDir);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("%d file(s) under %s, %d translatable by Interchange."),
				Files.Num(), *AbsoluteDir, Translatable);
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_batch_import ───────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_batch_import"),
		TEXT("Import many source files in ONE game-thread entry. This is the bulk ingest tool for "
			 "queued workloads: N separate import calls cost N entries against a small concurrent "
			 "job budget, while this costs one. Per-file results are returned so a partial failure "
			 "is diagnosable without re-running the wave."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("content_path"), FSololmcpSchemaBuilder::String(
					TEXT("Destination content path, e.g. /Game/Imported."))},
				{TEXT("source_files"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Path to a source file.")),
					TEXT("Explicit file list. Use this or directory."))},
				{TEXT("directory"), FSololmcpSchemaBuilder::String(
					TEXT("Directory to import from, as an alternative to source_files."))},
				{TEXT("recursive"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Descend into subdirectories when using directory.")), true)},
				{TEXT("extensions"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Extension without the dot.")),
					TEXT("Restrict directory scanning to these extensions."))},
				{TEXT("skip_untranslatable"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Skip files no translator accepts instead of recording them as failures.")),
					true)},
				{TEXT("stop_on_error"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(TEXT("Abort the remaining files after the first failure.")), false)},
				{TEXT("max_files"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("Safety cap on how many files one call will import.")), 200)}
			},
			{TEXT("content_path")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_batch_import"));
#else
			FString ContentPath;
			if (!Args->TryGetStringField(TEXT("content_path"), ContentPath) || ContentPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("content_path"));
				OutError = TEXT("Missing content_path.");
				return false;
			}

			TArray<FString> Files;
			const TArray<TSharedPtr<FJsonValue>>* Explicit = nullptr;
			if (Args->TryGetArrayField(TEXT("source_files"), Explicit) && Explicit != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Explicit)
				{
					FString File;
					if (Value.IsValid() && Value->TryGetString(File) && !File.IsEmpty())
					{
						Files.Add(FPaths::ConvertRelativePathToFull(File));
					}
				}
			}

			FString Directory;
			if (Files.Num() == 0 && Args->TryGetStringField(TEXT("directory"), Directory) && !Directory.IsEmpty())
			{
				const FString AbsoluteDir = FPaths::ConvertRelativePathToFull(Directory);
				if (!IFileManager::Get().DirectoryExists(*AbsoluteDir))
				{
					SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("directory"),
						FString::Printf(TEXT("No such directory: %s"), *AbsoluteDir));
					OutError = FString::Printf(TEXT("Directory not found: %s"), *AbsoluteDir);
					return false;
				}
				bool bRecursive = true;
				Args->TryGetBoolField(TEXT("recursive"), bRecursive);
				ScanDirectory(AbsoluteDir, bRecursive, ReadExtensionFilter(Args), Files);
			}

			if (Files.Num() == 0)
			{
				SololmcpError::Set(OutStructured, TEXT("MISSING_PARAM"), TEXT("source_files"),
					TEXT("Pass source_files, or a directory that contains matching files."));
				OutError = TEXT("Nothing to import.");
				return false;
			}

			int32 MaxFiles = 200;
			Args->TryGetNumberField(TEXT("max_files"), MaxFiles);
			MaxFiles = FMath::Clamp(MaxFiles, 1, 5000);
			bool bSkipUntranslatable = true;
			Args->TryGetBoolField(TEXT("skip_untranslatable"), bSkipUntranslatable);
			bool bStopOnError = false;
			Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Imported = 0;
			int32 Failed = 0;
			int32 Skipped = 0;
			bool bAnyObjectsReported = false;

			for (int32 Index = 0; Index < Files.Num() && Index < MaxFiles; ++Index)
			{
				const FString& File = Files[Index];
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);
				Row->SetStringField(TEXT("file"), File);

				if (bStopOnError && Failed > 0)
				{
					Row->SetStringField(TEXT("status"), TEXT("skipped"));
					Row->SetStringField(TEXT("reason"), TEXT("stop_on_error"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Skipped;
					continue;
				}

				if (!IFileManager::Get().FileExists(*File))
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("reason"), TEXT("file_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				UInterchangeSourceData* Probe = UInterchangeManager::CreateSourceData(File);
				if (Probe == nullptr || !Manager().CanTranslateSourceData(Probe))
				{
					Row->SetStringField(TEXT("status"), bSkipUntranslatable ? TEXT("skipped") : TEXT("failed"));
					Row->SetStringField(TEXT("reason"), TEXT("no_translator"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					bSkipUntranslatable ? ++Skipped : ++Failed;
					continue;
				}

				TArray<FString> ImportedPaths;
				bool bObjectsReported = false;
				const bool bOk = ImportOne(ContentPath, File, /*bAutomated=*/true, ImportedPaths, bObjectsReported);
				bAnyObjectsReported |= bObjectsReported;

				Row->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("failed"));
				if (bObjectsReported)
				{
					AddStringArray(Row, TEXT("imported_objects"), ImportedPaths);
					Row->SetNumberField(TEXT("imported_object_count"), ImportedPaths.Num());
				}
				Results.Add(MakeShared<FJsonValueObject>(Row));
				bOk ? ++Imported : ++Failed;
			}

			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetStringField(TEXT("content_path"), ContentPath);
			OutStructured->SetNumberField(TEXT("candidates"), Files.Num());
			OutStructured->SetNumberField(TEXT("attempted"), Results.Num());
			OutStructured->SetNumberField(TEXT("imported"), Imported);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetNumberField(TEXT("skipped"), Skipped);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetBoolField(TEXT("imported_objects_reported"), bAnyObjectsReported);
			if (!bAnyObjectsReported)
			{
				OutStructured->SetStringField(TEXT("imported_objects_note"),
					FString::Printf(
						TEXT("UE %s ImportAsset has no OutImportedObjects overload (added in 5.5); ")
						TEXT("use asset_list on '%s' to enumerate results."),
						*EngineVersionString(), *ContentPath));
			}
			if (Files.Num() > Results.Num())
			{
				OutStructured->SetBoolField(TEXT("truncated_by_max_files"), true);
			}
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(
				TEXT("Imported %d/%d file(s) into %s in one game-thread entry (%d failed, %d skipped)."),
				Imported, Results.Num(), *ContentPath, Failed, Skipped);
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d of %d imports failed."), Failed, Results.Num());
				return false;
			}
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_asset_import_data_inspect ──────────────────────────────
	Registry.Register({
		TEXT("interchange_asset_import_data_inspect"),
		TEXT("Report the source files recorded on imported assets, and whether each still exists on "
			 "disk. Accepts many assets so a whole imported set can be audited in one entry."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("object_paths"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Asset object path.")),
					TEXT("Assets to inspect."))}
			},
			{TEXT("object_paths")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_asset_import_data_inspect"));
#else
			const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
			if (!Args->TryGetArrayField(TEXT("object_paths"), Paths) || Paths == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("object_paths"));
				OutError = TEXT("Missing object_paths array.");
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 WithImportData = 0;
			int32 MissingSources = 0;

			for (const TSharedPtr<FJsonValue>& Value : *Paths)
			{
				FString ObjectPath;
				if (!Value.IsValid() || !Value->TryGetString(ObjectPath) || ObjectPath.IsEmpty())
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("object_path"), ObjectPath);

				UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad();
				if (Asset == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("not_found"));
					Rows.Add(MakeShared<FJsonValueObject>(Row));
					continue;
				}

				UAssetImportData* ImportData = FindAssetImportData(Asset);
				if (ImportData == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("no_import_data"));
					Rows.Add(MakeShared<FJsonValueObject>(Row));
					continue;
				}

				++WithImportData;
				TArray<FString> Filenames;
				ImportData->ExtractFilenames(Filenames);
				AddStringArray(Row, TEXT("source_files"), Filenames);

				TArray<FString> Missing;
				for (const FString& Filename : Filenames)
				{
					if (!Filename.IsEmpty() && !IFileManager::Get().FileExists(*Filename))
					{
						Missing.Add(Filename);
					}
				}
				if (Missing.Num() > 0)
				{
					AddStringArray(Row, TEXT("missing_source_files"), Missing);
					++MissingSources;
				}
				Row->SetStringField(TEXT("status"), TEXT("ok"));
				Row->SetBoolField(TEXT("all_sources_present"), Missing.Num() == 0);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("assets"), Rows);
			OutStructured->SetNumberField(TEXT("inspected"), Rows.Num());
			OutStructured->SetNumberField(TEXT("with_import_data"), WithImportData);
			OutStructured->SetNumberField(TEXT("with_missing_sources"), MissingSources);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(
				TEXT("Inspected %d asset(s): %d carry import data, %d have missing source files."),
				Rows.Num(), WithImportData, MissingSources);
			return true;
#endif
		},
		nullptr,
		5
	});

	// ── interchange_source_file_relink ─────────────────────────────────────
	Registry.Register({
		TEXT("interchange_source_file_relink"),
		TEXT("Repoint assets at moved source files by rewriting their recorded import paths. "
			 "Use after content is relocated so reimport keeps working. Applies to many assets in "
			 "one game-thread entry."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("items"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object(
						{
							{TEXT("object_path"), FSololmcpSchemaBuilder::String(TEXT("Asset to relink."))},
							{TEXT("new_source_file"), FSololmcpSchemaBuilder::String(
								TEXT("New absolute path to the source file."))}
						},
						{TEXT("object_path"), TEXT("new_source_file")}),
					TEXT("Relink operations."))},
				{TEXT("require_file_exists"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Refuse to record a path that does not exist on disk. Defaults to true; "
							 "recording a bad path silently breaks later reimports.")),
					true)}
			},
			{TEXT("items")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_source_file_relink"));
#else
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Args->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("items"));
				OutError = TEXT("Missing items array.");
				return false;
			}
			bool bRequireExists = true;
			Args->TryGetBoolField(TEXT("require_file_exists"), bRequireExists);

			TArray<TSharedPtr<FJsonValue>> Results;
			int32 Relinked = 0;
			int32 Failed = 0;

			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);

				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item) || Item == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("item_not_object"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				FString ObjectPath;
				FString NewSource;
				(*Item)->TryGetStringField(TEXT("object_path"), ObjectPath);
				(*Item)->TryGetStringField(TEXT("new_source_file"), NewSource);
				Row->SetStringField(TEXT("object_path"), ObjectPath);
				Row->SetStringField(TEXT("new_source_file"), NewSource);

				if (ObjectPath.IsEmpty() || NewSource.IsEmpty())
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("missing_fields"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				const FString AbsoluteSource = FPaths::ConvertRelativePathToFull(NewSource);
				if (bRequireExists && !IFileManager::Get().FileExists(*AbsoluteSource))
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"), TEXT("new_source_file_not_found"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad();
				UAssetImportData* ImportData = FindAssetImportData(Asset);
				if (ImportData == nullptr)
				{
					Row->SetStringField(TEXT("status"), TEXT("failed"));
					Row->SetStringField(TEXT("failure"),
						Asset == nullptr ? TEXT("asset_not_found") : TEXT("no_import_data"));
					Results.Add(MakeShared<FJsonValueObject>(Row));
					++Failed;
					continue;
				}

				Asset->Modify();
				ImportData->UpdateFilenameOnly(AbsoluteSource);
				Asset->MarkPackageDirty();

				Row->SetStringField(TEXT("status"), TEXT("ok"));
				Row->SetStringField(TEXT("recorded_source"), ImportData->GetFirstFilename());
				Results.Add(MakeShared<FJsonValueObject>(Row));
				++Relinked;
			}

			OutStructured->SetArrayField(TEXT("results"), Results);
			OutStructured->SetNumberField(TEXT("requested"), Items->Num());
			OutStructured->SetNumberField(TEXT("relinked"), Relinked);
			OutStructured->SetNumberField(TEXT("failed"), Failed);
			OutStructured->SetNumberField(TEXT("game_thread_entries"), 1);
			OutStructured->SetBoolField(TEXT("ok"), Failed == 0);
			OutSummary = FString::Printf(TEXT("Relinked %d/%d asset(s) in one game-thread entry."),
				Relinked, Items->Num());
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("%d of %d relinks failed."), Failed, Items->Num());
				return false;
			}
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_import_receipt_validate ────────────────────────────────
	Registry.Register({
		TEXT("interchange_import_receipt_validate"),
		TEXT("Verify that an expected set of assets exists after an import wave, and that each one "
			 "records a source file that is still on disk. Use this as the terminal check of a queued "
			 "ingest so a partially-applied wave is detected before downstream work depends on it."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("expected_object_paths"), FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::String(TEXT("Asset object path that should exist.")),
					TEXT("Assets the wave was expected to produce."))},
				{TEXT("require_source_files"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Also require each asset to record an existing source file.")),
					true)}
			},
			{TEXT("expected_object_paths")}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_import_receipt_validate"));
#else
			const TArray<TSharedPtr<FJsonValue>>* Expected = nullptr;
			if (!Args->TryGetArrayField(TEXT("expected_object_paths"), Expected) || Expected == nullptr)
			{
				SololmcpError::MissingParam(OutStructured, TEXT("expected_object_paths"));
				OutError = TEXT("Missing expected_object_paths array.");
				return false;
			}
			bool bRequireSources = true;
			Args->TryGetBoolField(TEXT("require_source_files"), bRequireSources);

			TArray<FString> Present;
			TArray<FString> Absent;
			TArray<FString> WithoutSource;

			for (const TSharedPtr<FJsonValue>& Value : *Expected)
			{
				FString ObjectPath;
				if (!Value.IsValid() || !Value->TryGetString(ObjectPath) || ObjectPath.IsEmpty())
				{
					continue;
				}
				UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad();
				if (Asset == nullptr)
				{
					Absent.Add(ObjectPath);
					continue;
				}
				Present.Add(ObjectPath);

				if (!bRequireSources)
				{
					continue;
				}
				UAssetImportData* ImportData = FindAssetImportData(Asset);
				bool bHasLiveSource = false;
				if (ImportData != nullptr)
				{
					TArray<FString> Filenames;
					ImportData->ExtractFilenames(Filenames);
					for (const FString& Filename : Filenames)
					{
						if (!Filename.IsEmpty() && IFileManager::Get().FileExists(*Filename))
						{
							bHasLiveSource = true;
							break;
						}
					}
				}
				if (!bHasLiveSource)
				{
					WithoutSource.Add(ObjectPath);
				}
			}

			const bool bValid = Absent.Num() == 0 && WithoutSource.Num() == 0;
			AddStringArray(OutStructured, TEXT("present"), Present);
			AddStringArray(OutStructured, TEXT("absent"), Absent);
			AddStringArray(OutStructured, TEXT("without_live_source"), WithoutSource);
			OutStructured->SetNumberField(TEXT("expected"), Expected->Num());
			OutStructured->SetNumberField(TEXT("present_count"), Present.Num());
			OutStructured->SetNumberField(TEXT("absent_count"), Absent.Num());
			OutStructured->SetBoolField(TEXT("valid"), bValid);
			OutStructured->SetBoolField(TEXT("ok"), bValid);
			OutSummary = bValid
				? FString::Printf(TEXT("All %d expected asset(s) present with live sources."), Present.Num())
				: FString::Printf(TEXT("%d absent, %d without a live source file."),
					Absent.Num(), WithoutSource.Num());
			if (!bValid)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("expected_object_paths"),
					TEXT("Re-run the import for the absent entries, or relink the ones missing sources "
						 "with interchange_source_file_relink."));
				OutError = OutSummary;
				return false;
			}
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_result_capture_start ───────────────────────────────────
	Registry.Register({
		TEXT("interchange_result_capture_start"),
		TEXT("Begin capturing Interchange import diagnostics. Interchange reports translator and "
			 "factory messages through a completion delegate rather than through the import return "
			 "value, and nothing retains them afterwards — so call this BEFORE a queued import wave, "
			 "then read interchange_result_list once the wave finishes."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("clear_existing"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Discard previously captured messages so the next read covers only this wave.")),
					true)},
				{TEXT("max_entries"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(
						TEXT("Cap on retained messages; excess is counted but not stored.")), 2000)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_result_capture_start"));
#else
			FResultCapture& Capture = FResultCapture::Get();

			bool bClear = true;
			Args->TryGetBoolField(TEXT("clear_existing"), bClear);
			if (bClear)
			{
				Capture.Entries.Reset();
				Capture.DroppedCount = 0;
			}
			int32 MaxEntries = 2000;
			Args->TryGetNumberField(TEXT("max_entries"), MaxEntries);
			Capture.MaxEntries = FMath::Clamp(MaxEntries, 1, 100000);

			const bool bWasBound = Capture.bBound;
			if (!Capture.bBound)
			{
				Capture.Handle = Manager().OnBatchImportComplete.AddStatic(&OnBatchImportCompleteCaptured);
				Capture.bBound = true;
				Capture.StartedAt = FDateTime::UtcNow();
			}

			OutStructured->SetBoolField(TEXT("capturing"), true);
			OutStructured->SetBoolField(TEXT("newly_bound"), !bWasBound);
			OutStructured->SetNumberField(TEXT("max_entries"), Capture.MaxEntries);
			OutStructured->SetNumberField(TEXT("retained_entries"), Capture.Entries.Num());
			OutStructured->SetStringField(TEXT("started_at"), Capture.StartedAt.ToIso8601());
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = bWasBound
				? FString::Printf(TEXT("Capture already active; %d message(s) retained."), Capture.Entries.Num())
				: TEXT("Started capturing Interchange import diagnostics.");
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_result_list ────────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_result_list"),
		TEXT("List captured Interchange import diagnostics, newest last. An import can report success "
			 "while still emitting per-asset warnings and errors, so read this after a wave rather "
			 "than trusting the import return value alone."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("severity"), FSololmcpSchemaBuilder::String(
					TEXT("Filter to one severity. Omit for all."),
					{TEXT("error"), TEXT("warning"), TEXT("success"), TEXT("info")})},
				{TEXT("limit"), FSololmcpSchemaBuilder::WithDefaultNumber(
					FSololmcpSchemaBuilder::Integer(TEXT("Maximum messages to return.")), 200)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_result_list"));
#else
			FResultCapture& Capture = FResultCapture::Get();

			FString Severity;
			Args->TryGetStringField(TEXT("severity"), Severity);
			int32 Limit = 200;
			Args->TryGetNumberField(TEXT("limit"), Limit);
			Limit = FMath::Clamp(Limit, 1, 20000);

			int32 Errors = 0;
			int32 Warnings = 0;
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FResultCapture::FEntry& Entry : Capture.Entries)
			{
				if (Entry.Severity == TEXT("error"))   { ++Errors; }
				if (Entry.Severity == TEXT("warning")) { ++Warnings; }
				if (!Severity.IsEmpty() && !Entry.Severity.Equals(Severity, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (Rows.Num() >= Limit)
				{
					continue;
				}
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("severity"), Entry.Severity);
				Row->SetStringField(TEXT("message"), Entry.Message);
				Row->SetStringField(TEXT("source_asset"), Entry.SourceAsset);
				Row->SetStringField(TEXT("destination_asset"), Entry.DestinationAsset);
				Row->SetStringField(TEXT("result_class"), Entry.ResultClass);
				Row->SetStringField(TEXT("captured_at"), Entry.CapturedAt.ToIso8601());
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			OutStructured->SetArrayField(TEXT("results"), Rows);
			OutStructured->SetNumberField(TEXT("returned"), Rows.Num());
			OutStructured->SetNumberField(TEXT("total_captured"), Capture.Entries.Num());
			OutStructured->SetNumberField(TEXT("error_count"), Errors);
			OutStructured->SetNumberField(TEXT("warning_count"), Warnings);
			OutStructured->SetNumberField(TEXT("dropped_over_cap"), Capture.DroppedCount);
			OutStructured->SetBoolField(TEXT("capturing"), Capture.bBound);
			OutStructured->SetBoolField(TEXT("ok"), true);
			if (!Capture.bBound)
			{
				// Distinguish "the wave was clean" from "nothing was listening".
				OutStructured->SetStringField(TEXT("note"),
					TEXT("Capture was never started, so this list is empty regardless of what the "
						 "imports reported. Call interchange_result_capture_start before the wave."));
			}
			OutSummary = Capture.bBound
				? FString::Printf(TEXT("%d captured message(s): %d error(s), %d warning(s)."),
					Capture.Entries.Num(), Errors, Warnings)
				: TEXT("Diagnostics capture is not active; call interchange_result_capture_start first.");
			return true;
#endif
		},
		nullptr,
		0
	});

	// ── interchange_result_clear ───────────────────────────────────────────
	Registry.Register({
		TEXT("interchange_result_clear"),
		TEXT("Discard captured diagnostics, and optionally stop capturing. Use between queued waves "
			 "so each wave's messages can be attributed to it."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("stop_capturing"), FSololmcpSchemaBuilder::WithDefaultBoolean(
					FSololmcpSchemaBuilder::Boolean(
						TEXT("Also unbind the completion delegate.")), false)}
			},
			{}),
		[](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args,
		   TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
#if !SOMOLMCP_WITH_INTERCHANGE_BATCH
			return RefuseNoModule(OutStructured, OutError, TEXT("interchange_result_clear"));
#else
			FResultCapture& Capture = FResultCapture::Get();
			const int32 Discarded = Capture.Entries.Num();
			Capture.Entries.Reset();
			Capture.DroppedCount = 0;

			bool bStop = false;
			Args->TryGetBoolField(TEXT("stop_capturing"), bStop);
			if (bStop && Capture.bBound)
			{
				Manager().OnBatchImportComplete.Remove(Capture.Handle);
				Capture.Handle.Reset();
				Capture.bBound = false;
			}

			OutStructured->SetNumberField(TEXT("discarded"), Discarded);
			OutStructured->SetBoolField(TEXT("capturing"), Capture.bBound);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutSummary = FString::Printf(TEXT("Discarded %d message(s); capture %s."),
				Discarded, Capture.bBound ? TEXT("still active") : TEXT("stopped"));
			return true;
#endif
		},
		nullptr,
		0
	});
}

} // namespace UE::SOMOLMCP
