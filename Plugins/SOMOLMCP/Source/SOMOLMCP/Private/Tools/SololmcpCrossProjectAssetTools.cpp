// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"

#include "SololmcpSchemaBuilder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace UE::SOMOLMCP
{
	namespace
	{
		struct FCrossProjectFileRow
		{
			FString PackageName;
			FString SourceFile;
			FString RelativeContentPath;
			FString DestinationFile;
			FString ContentHash;
			int64 SizeBytes = 0;
			bool bExists = false;
			bool bConflict = false;
		};

		struct FCrossProjectPlan
		{
			FString PlanId;
			FString SourceProjectDir;
			FString SourceProjectFile;
			FString SourceContentDir;
			FString TargetProjectDir;
			FString TargetProjectFile;
			FString TargetContentDir;
			FString StagingDir;
			TArray<FString> RootPackages;
			TArray<FString> Packages;
			TArray<FString> MissingPackages;
			TArray<FString> UnsupportedPackages;
			TArray<FCrossProjectFileRow> Files;
			int64 TotalBytes = 0;
		};

		FCriticalSection CrossProjectStateLock;
		TMap<FString, TSharedPtr<FJsonObject>> CrossProjectCheckpoints;

		FString NormalizeDirectory(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Value.EndsWith(TEXT(".uproject"), ESearchCase::IgnoreCase))
			{
				Value = FPaths::GetPath(Value);
			}
			Value = FPaths::ConvertRelativePathToFull(Value);
			FPaths::CollapseRelativeDirectories(Value);
			FPaths::NormalizeDirectoryName(Value);
			return Value;
		}

		FString FindProjectFile(const FString& ProjectDir)
		{
			TArray<FString> ProjectFiles;
			IFileManager::Get().FindFiles(ProjectFiles, *FPaths::Combine(ProjectDir, TEXT("*.uproject")), true, false);
			return ProjectFiles.Num() == 1 ? FPaths::Combine(ProjectDir, ProjectFiles[0]) : FString();
		}

		bool ResolveProject(
			const TSharedRef<FJsonObject>& Arguments,
			const TCHAR* DirectoryField,
			const TCHAR* ContentField,
			const bool bAllowCurrent,
			FString& OutProjectDir,
			FString& OutProjectFile,
			FString& OutContentDir,
			FString& OutError)
		{
			FString RequestedDir;
			Arguments->TryGetStringField(DirectoryField, RequestedDir);
			if (RequestedDir.IsEmpty() && bAllowCurrent)
			{
				RequestedDir = FPaths::ProjectDir();
			}
			OutProjectDir = NormalizeDirectory(RequestedDir);
			if (OutProjectDir.IsEmpty() || !FPaths::DirectoryExists(OutProjectDir))
			{
				OutError = FString::Printf(TEXT("invalid_project_directory: %s"), *OutProjectDir);
				return false;
			}
			OutProjectFile = FindProjectFile(OutProjectDir);
			if (OutProjectFile.IsEmpty())
			{
				OutError = FString::Printf(TEXT("project_file_not_found_or_ambiguous: %s"), *OutProjectDir);
				return false;
			}

			FString RequestedContent;
			Arguments->TryGetStringField(ContentField, RequestedContent);
			OutContentDir = RequestedContent.IsEmpty()
				? NormalizeDirectory(FPaths::Combine(OutProjectDir, TEXT("Content")))
				: NormalizeDirectory(RequestedContent);
			const FString ExpectedContent = NormalizeDirectory(FPaths::Combine(OutProjectDir, TEXT("Content")));
			if (!OutContentDir.Equals(ExpectedContent, ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("target_content_must_match_project_content: expected=%s actual=%s"), *ExpectedContent, *OutContentDir);
				return false;
			}
			return true;
		}

		bool ReadStringArray(const TSharedRef<FJsonObject>& Arguments, const TCHAR* Field, TArray<FString>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Arguments->TryGetArrayField(Field, Values) || !Values)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text) && !Text.TrimStartAndEnd().IsEmpty())
				{
					Out.AddUnique(Text.TrimStartAndEnd());
				}
			}
			return !Out.IsEmpty();
		}

		FString NormalizePackageName(FString Value)
		{
			Value.TrimStartAndEndInline();
			if (Value.Contains(TEXT(".")))
			{
				Value = FPackageName::ObjectPathToPackageName(Value);
			}
			return Value;
		}

		bool IsCurrentProject(const FString& ProjectDir)
		{
			return NormalizeDirectory(ProjectDir).Equals(NormalizeDirectory(FPaths::ProjectDir()), ESearchCase::IgnoreCase);
		}

		bool IsPackageAllowed(const FString& PackageName, const bool bIncludeEngine, const bool bIncludePlugins)
		{
			if (PackageName.StartsWith(TEXT("/Script/")))
			{
				return false;
			}
			if (PackageName.StartsWith(TEXT("/Engine/")))
			{
				return bIncludeEngine;
			}
			if (!PackageName.StartsWith(TEXT("/Game/")))
			{
				return bIncludePlugins;
			}
			return true;
		}

		FString HashTextStable(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			FSHAHash Hash;
			FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash.Hash);
			return Hash.ToString().ToLower();
		}

		FString HashFileStable(const FString& Filename)
		{
			if (!FPaths::FileExists(Filename))
			{
				return FString();
			}
			return LexToString(FMD5Hash::HashFile(*Filename));
		}

		FString PackageToRelativeContentPath(const FString& PackageName, const FString& SourceFile)
		{
			if (PackageName.StartsWith(TEXT("/Game/")))
			{
				return PackageName.Mid(6) + FPaths::GetExtension(SourceFile, true);
			}
			return FString();
		}

		TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		TSharedRef<FJsonObject> FileRowToJson(const FCrossProjectFileRow& Row)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("package_name"), Row.PackageName);
			Json->SetStringField(TEXT("source_file"), Row.SourceFile);
			Json->SetStringField(TEXT("relative_content_path"), Row.RelativeContentPath);
			Json->SetStringField(TEXT("destination_file"), Row.DestinationFile);
			Json->SetStringField(TEXT("content_hash"), Row.ContentHash);
			Json->SetStringField(TEXT("hash_algorithm"), TEXT("md5_stream"));
			Json->SetNumberField(TEXT("size_bytes"), static_cast<double>(Row.SizeBytes));
			Json->SetBoolField(TEXT("source_exists"), Row.bExists);
			Json->SetBoolField(TEXT("conflict"), Row.bConflict);
			return Json;
		}

		TSharedRef<FJsonObject> PlanToJson(const FCrossProjectPlan& Plan)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("somol.cross_project_asset_migration.v1"));
			Json->SetStringField(TEXT("plan_id"), Plan.PlanId);
			Json->SetStringField(TEXT("source_project_dir"), Plan.SourceProjectDir);
			Json->SetStringField(TEXT("source_project_file"), Plan.SourceProjectFile);
			Json->SetStringField(TEXT("source_content_dir"), Plan.SourceContentDir);
			Json->SetStringField(TEXT("target_project_dir"), Plan.TargetProjectDir);
			Json->SetStringField(TEXT("target_project_file"), Plan.TargetProjectFile);
			Json->SetStringField(TEXT("target_content_dir"), Plan.TargetContentDir);
			Json->SetStringField(TEXT("staging_dir"), Plan.StagingDir);
			Json->SetArrayField(TEXT("root_assets"), StringsToJson(Plan.RootPackages));
			Json->SetArrayField(TEXT("packages"), StringsToJson(Plan.Packages));
			Json->SetArrayField(TEXT("missing_packages"), StringsToJson(Plan.MissingPackages));
			Json->SetArrayField(TEXT("unsupported_packages"), StringsToJson(Plan.UnsupportedPackages));
			TArray<TSharedPtr<FJsonValue>> Files;
			for (const FCrossProjectFileRow& Row : Plan.Files)
			{
				Files.Add(MakeShared<FJsonValueObject>(FileRowToJson(Row)));
			}
			Json->SetArrayField(TEXT("files"), Files);
			Json->SetNumberField(TEXT("file_count"), Plan.Files.Num());
			Json->SetNumberField(TEXT("total_bytes"), static_cast<double>(Plan.TotalBytes));
			Json->SetNumberField(TEXT("conflict_count"), Plan.Files.FilterByPredicate([](const FCrossProjectFileRow& Row) { return Row.bConflict; }).Num());
			Json->SetBoolField(TEXT("ready"), Plan.MissingPackages.IsEmpty() && Plan.UnsupportedPackages.IsEmpty());
			return Json;
		}

		bool SerializeJson(const TSharedRef<FJsonObject>& Json, FString& OutText)
		{
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutText);
			return FJsonSerializer::Serialize(Json, Writer);
		}

		bool SaveJson(const TSharedRef<FJsonObject>& Json, const FString& Filename, FString& OutError)
		{
			FString Text;
			if (!SerializeJson(Json, Text))
			{
				OutError = TEXT("json_serialize_failed");
				return false;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
			if (!FFileHelper::SaveStringToFile(Text, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(TEXT("json_write_failed: %s"), *Filename);
				return false;
			}
			return true;
		}

		bool LoadJsonFile(const FString& Filename, TSharedPtr<FJsonObject>& OutJson)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Filename))
			{
				return false;
			}
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
		}

		FString CheckpointFileForPlan(const FString& PlanId)
		{
			return FPaths::Combine(
				FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("CrossProjectMigration"), PlanId, TEXT("checkpoint.json"));
		}

		TSharedRef<FJsonObject> LoadOrCreateCheckpoint(const FCrossProjectPlan& Plan)
		{
			FScopeLock Lock(&CrossProjectStateLock);
			if (const TSharedPtr<FJsonObject>* Existing = CrossProjectCheckpoints.Find(Plan.PlanId); Existing && Existing->IsValid())
			{
				return (*Existing).ToSharedRef();
			}
			TSharedPtr<FJsonObject> Persisted;
			const FString Filename = CheckpointFileForPlan(Plan.PlanId);
			if (LoadJsonFile(Filename, Persisted)
				&& Persisted->GetStringField(TEXT("schema")) == TEXT("somol.cross_project_asset_checkpoint.v1")
				&& Persisted->GetStringField(TEXT("plan_id")) == Plan.PlanId)
			{
				CrossProjectCheckpoints.Add(Plan.PlanId, Persisted);
				return Persisted.ToSharedRef();
			}
			TSharedRef<FJsonObject> Checkpoint = MakeShared<FJsonObject>();
			Checkpoint->SetStringField(TEXT("schema"), TEXT("somol.cross_project_asset_checkpoint.v1"));
			Checkpoint->SetStringField(TEXT("plan_id"), Plan.PlanId);
			Checkpoint->SetNumberField(TEXT("operation_sequence"), 0);
			Checkpoint->SetArrayField(TEXT("completed_files"), TArray<TSharedPtr<FJsonValue>>());
			Checkpoint->SetStringField(TEXT("status"), TEXT("not_started"));
			Checkpoint->SetStringField(TEXT("checkpoint_file"), Filename);
			CrossProjectCheckpoints.Add(Plan.PlanId, Checkpoint);
			return Checkpoint;
		}

		bool PersistCheckpoint(
			const FCrossProjectPlan& Plan,
			const FString& Stage,
			const FString& Status,
			const TSharedPtr<FJsonObject>& OperationReceipt,
			FString& OutError)
		{
			TSharedRef<FJsonObject> Checkpoint = LoadOrCreateCheckpoint(Plan);
			double Sequence = 0.0;
			Checkpoint->TryGetNumberField(TEXT("operation_sequence"), Sequence);
			Checkpoint->SetNumberField(TEXT("operation_sequence"), Sequence + 1.0);
			Checkpoint->SetStringField(TEXT("stage"), Stage);
			Checkpoint->SetStringField(TEXT("status"), Status);
			Checkpoint->SetStringField(TEXT("updated_at_utc"), FDateTime::UtcNow().ToIso8601());
			if (OperationReceipt.IsValid())
			{
				Checkpoint->SetObjectField(TEXT("last_receipt"), OperationReceipt.ToSharedRef());
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (OperationReceipt->TryGetArrayField(TEXT("items"), Items) && Items)
				{
					TArray<TSharedPtr<FJsonValue>> Completed;
					for (const TSharedPtr<FJsonValue>& Item : *Items)
					{
						const TSharedPtr<FJsonObject>* ItemObject = nullptr;
						if (!Item.IsValid() || !Item->TryGetObject(ItemObject) || !ItemObject || !ItemObject->IsValid()) continue;
						FString ItemStatus;
						(*ItemObject)->TryGetStringField(TEXT("status"), ItemStatus);
						if (ItemStatus == TEXT("copied") || ItemStatus == TEXT("restored") || ItemStatus == TEXT("reused_identical"))
						{
							Completed.Add(Item);
						}
					}
					Checkpoint->SetArrayField(TEXT("completed_files"), Completed);
				}
			}
			return SaveJson(Checkpoint, CheckpointFileForPlan(Plan.PlanId), OutError);
		}

		bool IsPathUnderDirectory(const FString& Candidate, const FString& Root)
		{
			FString NormalizedCandidate = FPaths::ConvertRelativePathToFull(Candidate);
			FString NormalizedRoot = NormalizeDirectory(Root);
			FPaths::NormalizeFilename(NormalizedCandidate);
			return NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
				|| NormalizedCandidate.StartsWith(NormalizedRoot + TEXT("/"), ESearchCase::IgnoreCase);
		}

		bool LoadPlanFromManifest(const TSharedRef<FJsonObject>& Arguments, FCrossProjectPlan& OutPlan, FString& OutError)
		{
			FString ManifestFile;
			Arguments->TryGetStringField(TEXT("manifest_file"), ManifestFile);
			ManifestFile = FPaths::ConvertRelativePathToFull(ManifestFile);
			FPaths::NormalizeFilename(ManifestFile);
			const FString AuthorizedRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("CrossProjectMigration"));
			if (ManifestFile.IsEmpty() || !FPaths::FileExists(ManifestFile))
			{
				OutError = TEXT("manifest_file_missing_or_not_found");
				return false;
			}
			if (!IsPathUnderDirectory(ManifestFile, AuthorizedRoot))
			{
				OutError = FString::Printf(TEXT("manifest_outside_target_project_staging_root: %s"), *ManifestFile);
				return false;
			}

			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *ManifestFile))
			{
				OutError = TEXT("manifest_read_failed");
				return false;
			}
			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()
				|| Json->GetStringField(TEXT("schema")) != TEXT("somol.cross_project_asset_migration.v1"))
			{
				OutError = TEXT("manifest_schema_or_json_invalid");
				return false;
			}

			Json->TryGetStringField(TEXT("plan_id"), OutPlan.PlanId);
			Json->TryGetStringField(TEXT("source_project_dir"), OutPlan.SourceProjectDir);
			Json->TryGetStringField(TEXT("source_project_file"), OutPlan.SourceProjectFile);
			Json->TryGetStringField(TEXT("source_content_dir"), OutPlan.SourceContentDir);
			Json->TryGetStringField(TEXT("target_project_dir"), OutPlan.TargetProjectDir);
			Json->TryGetStringField(TEXT("target_project_file"), OutPlan.TargetProjectFile);
			Json->TryGetStringField(TEXT("target_content_dir"), OutPlan.TargetContentDir);
			Json->TryGetStringField(TEXT("staging_dir"), OutPlan.StagingDir);
			if (!IsCurrentProject(OutPlan.TargetProjectDir))
			{
				OutError = TEXT("manifest_target_does_not_match_current_editor_project");
				return false;
			}
			OutPlan.TargetProjectDir = NormalizeDirectory(FPaths::ProjectDir());
			OutPlan.TargetProjectFile = FindProjectFile(OutPlan.TargetProjectDir);
			OutPlan.TargetContentDir = NormalizeDirectory(FPaths::ProjectContentDir());

			const TArray<TSharedPtr<FJsonValue>>* RootAssets = nullptr;
			if (Json->TryGetArrayField(TEXT("root_assets"), RootAssets) && RootAssets)
			{
				for (const TSharedPtr<FJsonValue>& Value : *RootAssets)
				{
					FString Package;
					if (Value.IsValid() && Value->TryGetString(Package)) OutPlan.RootPackages.Add(Package);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
			if (!Json->TryGetArrayField(TEXT("files"), Files) || !Files || Files->IsEmpty())
			{
				OutError = TEXT("manifest_contains_no_files");
				return false;
			}
			const FString ManifestRoot = FPaths::GetPath(ManifestFile);
			for (const TSharedPtr<FJsonValue>& Value : *Files)
			{
				const TSharedPtr<FJsonObject>* RowJson = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(RowJson) || !RowJson || !RowJson->IsValid()) continue;
				FCrossProjectFileRow Row;
				(*RowJson)->TryGetStringField(TEXT("package_name"), Row.PackageName);
				(*RowJson)->TryGetStringField(TEXT("relative_content_path"), Row.RelativeContentPath);
				(*RowJson)->TryGetStringField(TEXT("content_hash"), Row.ContentHash);
				double Size = 0.0;
				(*RowJson)->TryGetNumberField(TEXT("size_bytes"), Size);
				Row.SizeBytes = static_cast<int64>(Size);
				Row.SourceFile = FPaths::Combine(ManifestRoot, TEXT("Content"), Row.RelativeContentPath);
				Row.DestinationFile = FPaths::Combine(OutPlan.TargetContentDir, Row.RelativeContentPath);
				Row.bExists = FPaths::FileExists(Row.SourceFile);
				Row.bConflict = FPaths::FileExists(Row.DestinationFile);
				if (Row.PackageName.IsEmpty() || Row.RelativeContentPath.IsEmpty()
					|| !IsPathUnderDirectory(Row.SourceFile, ManifestRoot)
					|| !IsPathUnderDirectory(Row.DestinationFile, OutPlan.TargetContentDir))
				{
					OutError = TEXT("manifest_contains_invalid_or_escaping_file_path");
					return false;
				}
				OutPlan.Packages.Add(Row.PackageName);
				OutPlan.TotalBytes += FMath::Max<int64>(0, Row.SizeBytes);
				OutPlan.Files.Add(MoveTemp(Row));
			}
			return !OutPlan.Files.IsEmpty();
		}

		bool BuildPlan(const TSharedRef<FJsonObject>& Arguments, FCrossProjectPlan& OutPlan, FString& OutError)
		{
			if (!ResolveProject(Arguments, TEXT("source_project_dir"), TEXT("source_content_dir"), true,
				OutPlan.SourceProjectDir, OutPlan.SourceProjectFile, OutPlan.SourceContentDir, OutError))
			{
				return false;
			}
			if (!ResolveProject(Arguments, TEXT("target_project_dir"), TEXT("target_content_dir"), false,
				OutPlan.TargetProjectDir, OutPlan.TargetProjectFile, OutPlan.TargetContentDir, OutError))
			{
				return false;
			}
			if (OutPlan.SourceProjectDir.Equals(OutPlan.TargetProjectDir, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("source_and_target_projects_are_identical");
				return false;
			}
			if (!IsCurrentProject(OutPlan.SourceProjectDir))
			{
				OutError = TEXT("source_project_must_be_the_current_editor_project");
				return false;
			}

			if (!ReadStringArray(Arguments, TEXT("root_assets"), OutPlan.RootPackages))
			{
				FString RootAsset;
				Arguments->TryGetStringField(TEXT("asset_path"), RootAsset);
				if (!RootAsset.IsEmpty())
				{
					OutPlan.RootPackages.Add(RootAsset);
				}
			}
			for (FString& Root : OutPlan.RootPackages)
			{
				Root = NormalizePackageName(Root);
			}
			if (OutPlan.RootPackages.IsEmpty())
			{
				OutError = TEXT("root_assets_required");
				return false;
			}

			int32 MaxDepth = 12;
			int32 MaxPackages = 2000;
			Arguments->TryGetNumberField(TEXT("max_depth"), MaxDepth);
			Arguments->TryGetNumberField(TEXT("max_packages"), MaxPackages);
			MaxDepth = FMath::Clamp(MaxDepth, 0, 64);
			MaxPackages = FMath::Clamp(MaxPackages, 1, 20000);
			bool bIncludeEngine = false;
			bool bIncludePlugins = false;
			Arguments->TryGetBoolField(TEXT("include_engine_content"), bIncludeEngine);
			Arguments->TryGetBoolField(TEXT("include_plugin_content"), bIncludePlugins);

			IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TSet<FName> Seen;
			TArray<TPair<FName, int32>> Queue;
			for (const FString& Root : OutPlan.RootPackages)
			{
				Queue.Emplace(FName(*Root), 0);
				Seen.Add(FName(*Root));
			}
			for (int32 Index = 0; Index < Queue.Num() && OutPlan.Packages.Num() < MaxPackages; ++Index)
			{
				const FString PackageName = Queue[Index].Key.ToString();
				const int32 Depth = Queue[Index].Value;
				if (!IsPackageAllowed(PackageName, bIncludeEngine, bIncludePlugins))
				{
					if (!PackageName.StartsWith(TEXT("/Script/")))
					{
						OutPlan.UnsupportedPackages.AddUnique(PackageName);
					}
					continue;
				}
				OutPlan.Packages.AddUnique(PackageName);
				if (Depth >= MaxDepth)
				{
					continue;
				}
				TArray<FName> Dependencies;
				AssetRegistry.GetDependencies(Queue[Index].Key, Dependencies);
				for (const FName& Dependency : Dependencies)
				{
					if (!Seen.Contains(Dependency))
					{
						Seen.Add(Dependency);
						Queue.Emplace(Dependency, Depth + 1);
					}
				}
			}
			if (Queue.Num() > MaxPackages)
			{
				OutError = FString::Printf(TEXT("dependency_package_cap_exceeded: cap=%d discovered=%d"), MaxPackages, Queue.Num());
				return false;
			}

			for (const FString& PackageName : OutPlan.Packages)
			{
				FString PrimarySourceFile;
				if (!FPackageName::DoesPackageExist(PackageName, &PrimarySourceFile))
				{
					OutPlan.MissingPackages.AddUnique(PackageName);
					continue;
				}
				PrimarySourceFile = FPaths::ConvertRelativePathToFull(PrimarySourceFile);
				const FString PrimaryRelativePath = PackageToRelativeContentPath(PackageName, PrimarySourceFile);
				if (PrimaryRelativePath.IsEmpty())
				{
					OutPlan.UnsupportedPackages.AddUnique(PackageName);
					continue;
				}

				const FString PrimaryBase = FPaths::ChangeExtension(PrimarySourceFile, TEXT(""));
				const FString RelativeBase = FPaths::ChangeExtension(PrimaryRelativePath, TEXT(""));
				TArray<TPair<FString, FString>> PackageFiles;
				PackageFiles.Emplace(PrimarySourceFile, PrimaryRelativePath);
				for (const FString& SidecarExtension : {TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl")})
				{
					const FString SidecarFile = PrimaryBase + SidecarExtension;
					if (FPaths::FileExists(SidecarFile))
					{
						PackageFiles.Emplace(SidecarFile, RelativeBase + SidecarExtension);
					}
				}
				for (const TPair<FString, FString>& PackageFile : PackageFiles)
				{
					FCrossProjectFileRow Row;
					Row.PackageName = PackageName;
					Row.SourceFile = PackageFile.Key;
					Row.RelativeContentPath = PackageFile.Value;
					Row.DestinationFile = FPaths::Combine(OutPlan.TargetContentDir, Row.RelativeContentPath);
					Row.bExists = true;
					Row.SizeBytes = IFileManager::Get().FileSize(*Row.SourceFile);
					Row.ContentHash = HashFileStable(Row.SourceFile);
					Row.bConflict = FPaths::FileExists(Row.DestinationFile);
					OutPlan.TotalBytes += FMath::Max<int64>(0, Row.SizeBytes);
					OutPlan.Files.Add(MoveTemp(Row));
				}
			}

			FString Fingerprint = OutPlan.SourceProjectDir + TEXT("|") + OutPlan.TargetProjectDir;
			for (const FCrossProjectFileRow& Row : OutPlan.Files)
			{
				Fingerprint += TEXT("|") + Row.PackageName + TEXT("|") + Row.ContentHash;
			}
			OutPlan.PlanId = TEXT("cpm_") + HashTextStable(Fingerprint).Left(24);
			FString StagingRoot;
			Arguments->TryGetStringField(TEXT("staging_dir"), StagingRoot);
			if (StagingRoot.IsEmpty())
			{
				StagingRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("CrossProjectMigration"));
			}
			OutPlan.StagingDir = NormalizeDirectory(FPaths::Combine(StagingRoot, OutPlan.PlanId));
			return true;
		}

		TSharedRef<FJsonObject> CommonSchema(const TArray<FString>& Required = {})
		{
			return FSololmcpSchemaBuilder::Object(
				{
					{TEXT("source_project_dir"), FSololmcpSchemaBuilder::String(TEXT("Source project directory; defaults to the current editor project."))},
					{TEXT("source_content_dir"), FSololmcpSchemaBuilder::String(TEXT("Optional exact source Content directory."))},
					{TEXT("target_project_dir"), FSololmcpSchemaBuilder::String(TEXT("Target project directory or .uproject path."))},
					{TEXT("target_content_dir"), FSololmcpSchemaBuilder::String(TEXT("Optional exact target Content directory."))},
					{TEXT("root_assets"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Root /Game package or object paths."))},
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Single root asset alternative."))},
					{TEXT("max_depth"), FSololmcpSchemaBuilder::Integer(TEXT("Recursive dependency depth; default 12."))},
					{TEXT("max_packages"), FSololmcpSchemaBuilder::Integer(TEXT("Fail-closed dependency package cap; default 2000."))},
					{TEXT("include_engine_content"), FSololmcpSchemaBuilder::Boolean(TEXT("Include /Engine content; default false."))},
					{TEXT("include_plugin_content"), FSololmcpSchemaBuilder::Boolean(TEXT("Include plugin mount content; default false."))},
					{TEXT("staging_dir"), FSololmcpSchemaBuilder::String(TEXT("Optional authorized staging root."))},
					{TEXT("manifest_file"), FSololmcpSchemaBuilder::String(TEXT("Target-side immutable migration manifest under Saved/SOMOLMCP/CrossProjectMigration."))},
					{TEXT("execute"), FSololmcpSchemaBuilder::Boolean(TEXT("Production mutation gate; defaults false."))},
					{TEXT("overwrite"), FSololmcpSchemaBuilder::Boolean(TEXT("Allow backup then overwrite of conflicts; default false."))},
					{TEXT("conflict_policy"), FSololmcpSchemaBuilder::String(TEXT("Conflict policy."), {TEXT("block"), TEXT("skip"), TEXT("overwrite"), TEXT("reuse_identical")})},
					{TEXT("confirm_source_delete"), FSololmcpSchemaBuilder::String(TEXT("Must equal plan_id before receipt-gated source deletion."))},
					{TEXT("receipt_file"), FSololmcpSchemaBuilder::String(TEXT("Optional persisted target acceptance receipt path."))},
					{TEXT("plan_id"), FSololmcpSchemaBuilder::String(TEXT("Stable migration plan id."))},
					{TEXT("checkpoint"), FSololmcpSchemaBuilder::Object({})},
					{TEXT("path_mappings"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Package path remap rows."))}
				},
				Required);
		}

		bool CopyPlanFiles(
			const FCrossProjectPlan& Plan,
			const FString& DestinationRoot,
			const bool bExecute,
			const bool bOverwrite,
			TSharedRef<FJsonObject>& Out,
			FString& OutError)
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Copied = 0;
			int32 Skipped = 0;
			int32 Failed = 0;
			int64 BytesCopied = 0;
			for (const FCrossProjectFileRow& SourceRow : Plan.Files)
			{
				const FString DestinationFile = FPaths::Combine(DestinationRoot, SourceRow.RelativeContentPath);
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("package_name"), SourceRow.PackageName);
				Row->SetStringField(TEXT("source_file"), SourceRow.SourceFile);
				Row->SetStringField(TEXT("destination_file"), DestinationFile);
				Row->SetNumberField(TEXT("size_bytes"), static_cast<double>(SourceRow.SizeBytes));
				const bool bDestinationExists = FPaths::FileExists(DestinationFile);
				if (!bExecute)
				{
					Row->SetStringField(TEXT("status"), bDestinationExists && !bOverwrite ? TEXT("blocked_conflict") : TEXT("would_copy"));
					bDestinationExists && !bOverwrite ? ++Skipped : ++Copied;
				}
				else if (bDestinationExists && !bOverwrite)
				{
					Row->SetStringField(TEXT("status"), TEXT("skipped_conflict"));
					++Skipped;
				}
				else
				{
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationFile), true);
					if (bDestinationExists && bOverwrite)
					{
						const FString Backup = DestinationFile + TEXT(".somolmcp_backup_") + Plan.PlanId;
						if (IFileManager::Get().Copy(*Backup, *DestinationFile, true, true) != COPY_OK)
						{
							Row->SetStringField(TEXT("status"), TEXT("backup_failed"));
							++Failed;
							Rows.Add(MakeShared<FJsonValueObject>(Row));
							continue;
						}
						Row->SetStringField(TEXT("backup_file"), Backup);
					}
					const uint32 CopyResult = IFileManager::Get().Copy(*DestinationFile, *SourceRow.SourceFile, true, true);
					const bool bVerified = CopyResult == COPY_OK
						&& IFileManager::Get().FileSize(*DestinationFile) == SourceRow.SizeBytes
						&& HashFileStable(DestinationFile).Equals(SourceRow.ContentHash, ESearchCase::IgnoreCase);
					if (bVerified)
					{
						Row->SetStringField(TEXT("status"), TEXT("copied"));
						Row->SetBoolField(TEXT("verified"), true);
						++Copied;
						BytesCopied += SourceRow.SizeBytes;
					}
					else
					{
						Row->SetStringField(TEXT("status"), TEXT("copy_or_hash_verification_failed"));
						++Failed;
					}
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Out->SetArrayField(TEXT("items"), Rows);
			Out->SetNumberField(TEXT("copied"), Copied);
			Out->SetNumberField(TEXT("skipped"), Skipped);
			Out->SetNumberField(TEXT("failed"), Failed);
			Out->SetNumberField(TEXT("bytes_copied"), static_cast<double>(BytesCopied));
			Out->SetBoolField(TEXT("execute"), bExecute);
			if (Failed > 0)
			{
				OutError = FString::Printf(TEXT("cross_project_copy_partial_failure: %d file(s) failed"), Failed);
			}
			return Failed == 0;
		}

		FString PrimaryObjectPathForPackage(IAssetRegistry& AssetRegistry, const FString& PackageName, FString* OutClassName = nullptr)
		{
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), Assets);
			if (Assets.IsEmpty()) return FString();
			if (OutClassName) *OutClassName = Assets[0].AssetClassPath.GetAssetName().ToString();
			return Assets[0].GetSoftObjectPath().ToString();
		}

		bool RunCompileValidation(
			FSololmcpToolRegistry& Registry,
			const FCrossProjectPlan& Plan,
			TSharedRef<FJsonObject>& Out,
			FString& OutError)
		{
			IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TArray<TSharedPtr<FJsonValue>> Receipts;
			int32 Failed = 0;
			int32 Compiled = 0;
			for (const FString& PackageName : Plan.Packages)
			{
				FString ClassName;
				const FString ObjectPath = PrimaryObjectPathForPackage(AssetRegistry, PackageName, &ClassName);
				FString CompileTool;
				TSharedRef<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
				if (ClassName.Contains(TEXT("Blueprint")))
				{
					CompileTool = TEXT("blueprint_repair_compile_gate");
					CompileArgs->SetStringField(TEXT("asset_path"), ObjectPath);
					CompileArgs->SetBoolField(TEXT("save_if_clean"), false);
				}
				else if (ClassName == TEXT("Material"))
				{
					CompileTool = TEXT("material_recompile");
					CompileArgs->SetStringField(TEXT("asset_path"), ObjectPath);
				}
				else if (ClassName == TEXT("NiagaraSystem"))
				{
					CompileTool = TEXT("niagara_compile_diagnostics");
					CompileArgs->SetStringField(TEXT("system_asset_path"), ObjectPath);
					CompileArgs->SetBoolField(TEXT("wait_for_completion"), true);
				}
				else if (ClassName == TEXT("PCGGraph"))
				{
					CompileTool = TEXT("pcg_graph_validate");
					CompileArgs->SetStringField(TEXT("asset_path"), ObjectPath);
				}

				if (CompileTool.IsEmpty()) continue;
				TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
				FString Summary;
				FString Error;
				const bool bOk = !ObjectPath.IsEmpty() && Registry.ExecuteTool(CompileTool, CompileArgs, Receipt, Summary, Error);
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("package_name"), PackageName);
				Row->SetStringField(TEXT("object_path"), ObjectPath);
				Row->SetStringField(TEXT("asset_class"), ClassName);
				Row->SetStringField(TEXT("compile_tool"), CompileTool);
				Row->SetBoolField(TEXT("passed"), bOk);
				Row->SetStringField(TEXT("summary"), Summary);
				if (!Error.IsEmpty()) Row->SetStringField(TEXT("error"), Error);
				Row->SetObjectField(TEXT("receipt"), Receipt);
				Receipts.Add(MakeShared<FJsonValueObject>(Row));
				++Compiled;
				if (!bOk) ++Failed;
			}
			Out->SetArrayField(TEXT("compile_receipts"), Receipts);
			Out->SetNumberField(TEXT("compiled_asset_count"), Compiled);
			Out->SetNumberField(TEXT("compile_failure_count"), Failed);
			Out->SetBoolField(TEXT("passed"), Failed == 0);
			Out->SetStringField(TEXT("status"), Failed == 0 ? TEXT("passed") : TEXT("failed"));
			if (Failed > 0) OutError = FString::Printf(TEXT("cross_project_domain_compile_failed: %d asset(s)"), Failed);
			return Failed == 0;
		}

		bool RunPackageAndReferenceValidation(
			const FCrossProjectPlan& Plan,
			const bool bLoadPackages,
			TSharedRef<FJsonObject>& Out,
			FString& OutError)
		{
			IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			TArray<TSharedPtr<FJsonValue>> Failures;
			int32 Valid = 0;
			for (const FString& PackageName : Plan.Packages)
			{
				bool bValid = !PrimaryObjectPathForPackage(AssetRegistry, PackageName).IsEmpty();
				FString Reason;
				if (bValid && bLoadPackages)
				{
					bValid = LoadPackage(nullptr, *PackageName, LOAD_None) != nullptr;
					if (!bValid) Reason = TEXT("package_load_failed");
				}
				if (bValid && !bLoadPackages)
				{
					TArray<FName> Dependencies;
					AssetRegistry.GetDependencies(FName(*PackageName), Dependencies);
					for (const FName& Dependency : Dependencies)
					{
						const FString DependencyName = Dependency.ToString();
						if (DependencyName.StartsWith(TEXT("/Game/")) && !FPackageName::DoesPackageExist(DependencyName))
						{
							bValid = false;
							Reason = TEXT("missing_game_dependency:") + DependencyName;
							break;
						}
					}
				}
				if (bValid) ++Valid;
				else
				{
					TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
					Failure->SetStringField(TEXT("package_name"), PackageName);
					Failure->SetStringField(TEXT("reason"), Reason.IsEmpty() ? TEXT("asset_registry_entry_missing") : Reason);
					Failures.Add(MakeShared<FJsonValueObject>(Failure));
				}
			}
			Out->SetNumberField(TEXT("valid_count"), Valid);
			Out->SetArrayField(TEXT("failures"), Failures);
			Out->SetBoolField(TEXT("passed"), Failures.IsEmpty());
			Out->SetStringField(TEXT("status"), Failures.IsEmpty() ? TEXT("passed") : TEXT("failed"));
			if (!Failures.IsEmpty()) OutError = FString::Printf(TEXT("cross_project_validation_failed: %d package(s)"), Failures.Num());
			return Failures.IsEmpty();
		}

		bool ExecuteCrossProjectTool(
			const FString& ToolName,
			FSololmcpToolRegistry& Registry,
			const FSololmcpToolExecutionContext& Context,
			const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& Out,
			FString& Summary,
			FString& Error)
		{
			if (ToolName == TEXT("asset_cross_project_capabilities"))
			{
				Out->SetStringField(TEXT("status"), TEXT("ready"));
				Out->SetStringField(TEXT("project_dir"), NormalizeDirectory(FPaths::ProjectDir()));
				Out->SetStringField(TEXT("project_file"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
				Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
				Out->SetBoolField(TEXT("native_cpp_queue"), true);
				Out->SetBoolField(TEXT("online_target_handoff"), true);
				Out->SetBoolField(TEXT("offline_staging"), true);
				Out->SetBoolField(TEXT("source_delete_requires_target_receipt"), true);
				Out->SetArrayField(TEXT("supported_conflict_policies"), StringsToJson({TEXT("block"), TEXT("skip"), TEXT("overwrite"), TEXT("reuse_identical")}));
				Summary = TEXT("Cross-project asset migration capabilities are available.");
				return true;
			}

			FString SourceDir, SourceProject, SourceContent, TargetDir, TargetProject, TargetContent;
			if (ToolName == TEXT("asset_cross_project_target_resolve") || ToolName == TEXT("asset_cross_project_pair_validate"))
			{
				if (!ResolveProject(Arguments, TEXT("source_project_dir"), TEXT("source_content_dir"), true, SourceDir, SourceProject, SourceContent, Error)
					|| !ResolveProject(Arguments, TEXT("target_project_dir"), TEXT("target_content_dir"), false, TargetDir, TargetProject, TargetContent, Error))
				{
					return false;
				}
				const bool bSame = SourceDir.Equals(TargetDir, ESearchCase::IgnoreCase);
				Out->SetStringField(TEXT("source_project_dir"), SourceDir);
				Out->SetStringField(TEXT("source_project_file"), SourceProject);
				Out->SetStringField(TEXT("target_project_dir"), TargetDir);
				Out->SetStringField(TEXT("target_project_file"), TargetProject);
				Out->SetStringField(TEXT("target_content_dir"), TargetContent);
				Out->SetBoolField(TEXT("target_is_current_editor"), IsCurrentProject(TargetDir));
				Out->SetBoolField(TEXT("pair_valid"), !bSame);
				Out->SetStringField(TEXT("status"), bSame ? TEXT("rejected_same_project") : TEXT("ready"));
				if (bSame)
				{
					Error = TEXT("source_and_target_projects_are_identical");
					return false;
				}
				Summary = TEXT("Cross-project source and target binding validated.");
				return true;
			}

			const bool bTargetManifestRoute =
				ToolName == TEXT("asset_cross_project_target_import_plan")
				|| ToolName == TEXT("asset_cross_project_target_import_execute")
				|| ToolName == TEXT("asset_cross_project_registry_refresh")
				|| ToolName == TEXT("asset_cross_project_package_load_validate")
				|| ToolName == TEXT("asset_cross_project_missing_reference_scan")
				|| ToolName == TEXT("asset_cross_project_compile_validate_batch")
				|| ToolName == TEXT("asset_cross_project_world_partition_validate")
				|| ToolName == TEXT("asset_cross_project_asset_family_validate")
				|| ToolName == TEXT("asset_cross_project_delta_sync_execute")
				|| ToolName == TEXT("asset_cross_project_rollback_execute")
				|| ToolName == TEXT("asset_cross_project_receipt_validate");
			FCrossProjectPlan Plan;
			if (!(bTargetManifestRoute ? LoadPlanFromManifest(Arguments, Plan, Error) : BuildPlan(Arguments, Plan, Error)))
			{
				return false;
			}
			const TSharedRef<FJsonObject> PlanJson = PlanToJson(Plan);
			Out->SetObjectField(TEXT("plan"), PlanJson);
			Out->SetStringField(TEXT("plan_id"), Plan.PlanId);

			if (ToolName == TEXT("asset_cross_project_dependency_closure"))
			{
				Out->SetStringField(TEXT("status"), Plan.MissingPackages.IsEmpty() ? TEXT("complete") : TEXT("incomplete"));
				Out->SetArrayField(TEXT("dependency_packages"), StringsToJson(Plan.Packages));
				Summary = FString::Printf(TEXT("Resolved %d migration package(s)."), Plan.Packages.Num());
				return Plan.MissingPackages.IsEmpty();
			}
			if (ToolName == TEXT("asset_cross_project_external_package_closure"))
			{
				TArray<FString> External;
				for (const FString& Package : Plan.Packages)
				{
					if (Package.Contains(TEXT("/ExternalActors/")) || Package.Contains(TEXT("/ExternalObjects/")))
					{
						External.Add(Package);
					}
				}
				Out->SetArrayField(TEXT("external_packages"), StringsToJson(External));
				Out->SetNumberField(TEXT("external_package_count"), External.Num());
				Out->SetStringField(TEXT("status"), TEXT("complete"));
				Summary = FString::Printf(TEXT("Resolved %d external package(s)."), External.Num());
				return true;
			}
			if (ToolName == TEXT("asset_cross_project_plugin_dependency_audit"))
			{
				Out->SetArrayField(TEXT("plugin_or_non_game_packages"), StringsToJson(Plan.UnsupportedPackages));
				Out->SetBoolField(TEXT("passed"), Plan.UnsupportedPackages.IsEmpty());
				Out->SetStringField(TEXT("status"), Plan.UnsupportedPackages.IsEmpty() ? TEXT("passed") : TEXT("blocked_plugin_or_engine_dependency"));
				Summary = FString::Printf(TEXT("Plugin dependency audit found %d non-/Game package(s)."), Plan.UnsupportedPackages.Num());
				return Plan.UnsupportedPackages.IsEmpty();
			}
			if (ToolName == TEXT("asset_cross_project_package_file_manifest") || ToolName == TEXT("asset_cross_project_migration_plan")
				|| ToolName == TEXT("asset_cross_project_bundle_manifest_build"))
			{
				Out->SetStringField(TEXT("status"), Plan.MissingPackages.IsEmpty() && Plan.UnsupportedPackages.IsEmpty() ? TEXT("ready") : TEXT("blocked"));
				Out->SetStringField(TEXT("manifest_hash"), HashTextStable(Plan.PlanId + FString::FromInt(Plan.Files.Num()) + FString::Printf(TEXT("%lld"), Plan.TotalBytes)));
				bool bExecute = false;
				Arguments->TryGetBoolField(TEXT("execute"), bExecute);
				if (bExecute && ToolName == TEXT("asset_cross_project_bundle_manifest_build"))
				{
					const FString ManifestFile = FPaths::Combine(Plan.StagingDir, TEXT("migration_manifest.json"));
					if (!SaveJson(PlanJson, ManifestFile, Error))
					{
						return false;
					}
					Out->SetStringField(TEXT("manifest_file"), ManifestFile);
				}
				Summary = FString::Printf(TEXT("Migration plan %s contains %d file(s), %lld bytes."), *Plan.PlanId, Plan.Files.Num(), Plan.TotalBytes);
				return Plan.MissingPackages.IsEmpty() && Plan.UnsupportedPackages.IsEmpty();
			}
			if (ToolName == TEXT("asset_cross_project_conflict_scan") || ToolName == TEXT("asset_cross_project_conflict_resolution_plan"))
			{
				TArray<TSharedPtr<FJsonValue>> Conflicts;
				for (const FCrossProjectFileRow& Row : Plan.Files)
				{
					if (!Row.bConflict)
					{
						continue;
					}
					TSharedRef<FJsonObject> Conflict = FileRowToJson(Row);
					const FString TargetHash = HashFileStable(Row.DestinationFile);
					Conflict->SetStringField(TEXT("target_hash"), TargetHash);
					Conflict->SetBoolField(TEXT("identical"), !TargetHash.IsEmpty() && TargetHash.Equals(Row.ContentHash, ESearchCase::IgnoreCase));
					Conflicts.Add(MakeShared<FJsonValueObject>(Conflict));
				}
				FString Policy = TEXT("block");
				Arguments->TryGetStringField(TEXT("conflict_policy"), Policy);
				Out->SetStringField(TEXT("conflict_policy"), Policy);
				Out->SetArrayField(TEXT("conflicts"), Conflicts);
				Out->SetNumberField(TEXT("conflict_count"), Conflicts.Num());
				Out->SetStringField(TEXT("status"), Conflicts.IsEmpty() ? TEXT("clear") : TEXT("conflicts_found"));
				Summary = FString::Printf(TEXT("Conflict scan found %d target file conflict(s)."), Conflicts.Num());
				return true;
			}
			if (ToolName == TEXT("asset_cross_project_path_remap_plan"))
			{
				TArray<TSharedPtr<FJsonValue>> Mappings;
				for (const FCrossProjectFileRow& Row : Plan.Files)
				{
					TSharedRef<FJsonObject> Mapping = MakeShared<FJsonObject>();
					Mapping->SetStringField(TEXT("source_package"), Row.PackageName);
					Mapping->SetStringField(TEXT("target_package"), Row.PackageName);
					Mapping->SetStringField(TEXT("policy"), TEXT("preserve_package_path"));
					Mappings.Add(MakeShared<FJsonValueObject>(Mapping));
				}
				Out->SetArrayField(TEXT("path_mappings"), Mappings);
				Out->SetStringField(TEXT("status"), TEXT("ready"));
				Summary = FString::Printf(TEXT("Prepared %d package path mapping(s)."), Mappings.Num());
				return true;
			}

			bool bExecute = false;
			bool bOverwrite = false;
			Arguments->TryGetBoolField(TEXT("execute"), bExecute);
			Arguments->TryGetBoolField(TEXT("overwrite"), bOverwrite);
			if (ToolName == TEXT("asset_cross_project_bundle_export_execute"))
			{
				const FString PayloadRoot = FPaths::Combine(Plan.StagingDir, TEXT("Content"));
				const bool bOk = CopyPlanFiles(Plan, PayloadRoot, bExecute, true, Out, Error);
				if (bExecute)
				{
					SaveJson(PlanJson, FPaths::Combine(Plan.StagingDir, TEXT("migration_manifest.json")), Error);
					if (!PersistCheckpoint(Plan, ToolName, bOk ? TEXT("exported") : TEXT("partial"), Out, Error)) return false;
				}
				Out->SetStringField(TEXT("staging_dir"), Plan.StagingDir);
				Out->SetStringField(TEXT("status"), bExecute ? (bOk ? TEXT("exported") : TEXT("partial")) : TEXT("dry_run"));
				Summary = FString::Printf(TEXT("Migration bundle export %s for %d file(s)."), bExecute ? TEXT("executed") : TEXT("previewed"), Plan.Files.Num());
				return bOk;
			}
			if (ToolName == TEXT("asset_cross_project_transfer_stage_execute"))
			{
				const FString TargetPlanRoot = FPaths::Combine(Plan.TargetProjectDir, TEXT("Saved"), TEXT("SOMOLMCP"), TEXT("CrossProjectMigration"), Plan.PlanId);
				const FString TargetStage = FPaths::Combine(TargetPlanRoot, TEXT("Content"));
				const bool bOk = CopyPlanFiles(Plan, TargetStage, bExecute, true, Out, Error);
				const FString TargetManifest = FPaths::Combine(TargetPlanRoot, TEXT("migration_manifest.json"));
				if (bExecute && bOk && !SaveJson(PlanJson, TargetManifest, Error))
				{
					return false;
				}
				if (bExecute && !PersistCheckpoint(Plan, ToolName, bOk ? TEXT("staged") : TEXT("partial"), Out, Error)) return false;
				Out->SetStringField(TEXT("target_staging_dir"), TargetPlanRoot);
				Out->SetStringField(TEXT("target_manifest_file"), TargetManifest);
				Out->SetStringField(TEXT("status"), bExecute ? (bOk ? TEXT("staged") : TEXT("partial")) : TEXT("dry_run"));
				Summary = FString::Printf(TEXT("Target staging %s for %d file(s)."), bExecute ? TEXT("executed") : TEXT("previewed"), Plan.Files.Num());
				return bOk;
			}
			if (ToolName == TEXT("asset_cross_project_target_import_plan"))
			{
				Out->SetStringField(TEXT("status"), TEXT("ready"));
				Out->SetBoolField(TEXT("target_online"), IsCurrentProject(Plan.TargetProjectDir));
				Out->SetBoolField(TEXT("requires_target_mcp_handoff"), !IsCurrentProject(Plan.TargetProjectDir));
				Out->SetStringField(TEXT("commit_lane"), TEXT("cross_project_asset_commit"));
				Summary = TEXT("Target import plan is ready.");
				return true;
			}
			if (ToolName == TEXT("asset_cross_project_target_import_execute"))
			{
				if (!IsCurrentProject(Plan.TargetProjectDir))
				{
					Out->SetStringField(TEXT("status"), TEXT("blocked_target_mcp_handoff_required"));
					Out->SetStringField(TEXT("reason_code"), TEXT("target_project_not_current_editor"));
					Error = TEXT("target_import_execute must run on the target project's MCP instance");
					return false;
				}
				const bool bOk = CopyPlanFiles(Plan, Plan.TargetContentDir, bExecute, bOverwrite, Out, Error);
				if (bExecute && bOk)
				{
					TArray<FString> Files;
					for (const FCrossProjectFileRow& Row : Plan.Files)
					{
						Files.Add(Row.DestinationFile);
					}
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous(Files, true);
					if (!PersistCheckpoint(Plan, ToolName, TEXT("committed"), Out, Error)) return false;
				}
				else if (bExecute && !PersistCheckpoint(Plan, ToolName, TEXT("partial"), Out, Error)) return false;
				Out->SetStringField(TEXT("status"), bExecute ? (bOk ? TEXT("committed") : TEXT("partial")) : TEXT("dry_run"));
				Summary = FString::Printf(TEXT("Target import %s for %d file(s)."), bExecute ? TEXT("executed") : TEXT("previewed"), Plan.Files.Num());
				return bOk;
			}
			if (ToolName == TEXT("asset_cross_project_registry_refresh"))
			{
				if (!IsCurrentProject(Plan.TargetProjectDir))
				{
					Error = TEXT("registry_refresh_requires_target_project_mcp");
					return false;
				}
				TArray<FString> Files;
				for (const FCrossProjectFileRow& Row : Plan.Files)
				{
					Files.Add(Row.DestinationFile);
				}
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous(Files, true);
				Out->SetNumberField(TEXT("scanned_file_count"), Files.Num());
				Out->SetStringField(TEXT("status"), TEXT("completed"));
				Summary = FString::Printf(TEXT("Refreshed Asset Registry for %d migrated file(s)."), Files.Num());
				return true;
			}
			if (ToolName == TEXT("asset_cross_project_compile_validate_batch"))
			{
				const bool bOk = RunCompileValidation(Registry, Plan, Out, Error);
				Summary = FString::Printf(TEXT("Domain compile validation completed for %d migrated package(s)."), Plan.Packages.Num());
				return bOk;
			}
			if (ToolName == TEXT("asset_cross_project_package_load_validate") || ToolName == TEXT("asset_cross_project_missing_reference_scan"))
			{
				const bool bOk = RunPackageAndReferenceValidation(Plan, ToolName == TEXT("asset_cross_project_package_load_validate"), Out, Error);
				Out->SetStringField(TEXT("validation_scope"), ToolName);
				Summary = FString::Printf(TEXT("%s checked %d migrated package(s)."), *ToolName, Plan.Packages.Num());
				return bOk;
			}
			if (ToolName == TEXT("asset_cross_project_world_partition_validate")
				|| ToolName == TEXT("asset_cross_project_asset_family_validate") || ToolName == TEXT("asset_cross_project_receipt_validate"))
			{
				int32 Valid = 0;
				TArray<TSharedPtr<FJsonValue>> Failures;
				for (const FCrossProjectFileRow& Row : Plan.Files)
				{
					const FString TargetHash = HashFileStable(Row.DestinationFile);
					const bool bValid = FPaths::FileExists(Row.DestinationFile)
						&& IFileManager::Get().FileSize(*Row.DestinationFile) == Row.SizeBytes
						&& TargetHash.Equals(Row.ContentHash, ESearchCase::IgnoreCase);
					if (bValid)
					{
						++Valid;
					}
					else
					{
						TSharedRef<FJsonObject> Failure = FileRowToJson(Row);
						Failure->SetStringField(TEXT("reason"), TEXT("target_file_missing_size_or_hash_mismatch"));
						Failures.Add(MakeShared<FJsonValueObject>(Failure));
					}
				}
				IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				TMap<FString, int32> ClassCounts;
				int32 MapCount = 0;
				int32 ExternalPackageCount = 0;
				for (const FString& PackageName : Plan.Packages)
				{
					FString ClassName;
					const FString ObjectPath = PrimaryObjectPathForPackage(AssetRegistry, PackageName, &ClassName);
					if (!ClassName.IsEmpty()) ++ClassCounts.FindOrAdd(ClassName);
					if (Plan.Files.ContainsByPredicate([&PackageName](const FCrossProjectFileRow& Row)
						{ return Row.PackageName == PackageName && FPaths::GetExtension(Row.RelativeContentPath).Equals(TEXT("umap"), ESearchCase::IgnoreCase); }))
					{
						++MapCount;
					}
					if (PackageName.Contains(TEXT("/ExternalActors/")) || PackageName.Contains(TEXT("/ExternalObjects/"))) ++ExternalPackageCount;
					if (ObjectPath.IsEmpty())
					{
						TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
						Failure->SetStringField(TEXT("package_name"), PackageName);
						Failure->SetStringField(TEXT("reason"), TEXT("asset_registry_primary_object_missing"));
						Failures.Add(MakeShared<FJsonValueObject>(Failure));
					}
				}
				TSharedRef<FJsonObject> Classes = MakeShared<FJsonObject>();
				for (const TPair<FString, int32>& Pair : ClassCounts) Classes->SetNumberField(Pair.Key, Pair.Value);
				Out->SetObjectField(TEXT("asset_class_counts"), Classes);
				Out->SetNumberField(TEXT("map_count"), MapCount);
				Out->SetNumberField(TEXT("external_package_count"), ExternalPackageCount);
				Out->SetBoolField(TEXT("world_partition_applicable"), MapCount > 0 || ExternalPackageCount > 0);
				if (ToolName == TEXT("asset_cross_project_receipt_validate"))
				{
					TSharedRef<FJsonObject> PackageReceipt = MakeShared<FJsonObject>();
					FString PackageError;
					const bool bPackagesOk = RunPackageAndReferenceValidation(Plan, true, PackageReceipt, PackageError);
					TSharedRef<FJsonObject> CompileReceipt = MakeShared<FJsonObject>();
					FString CompileError;
					const bool bCompileOk = RunCompileValidation(Registry, Plan, CompileReceipt, CompileError);
					Out->SetObjectField(TEXT("package_validation"), PackageReceipt);
					Out->SetObjectField(TEXT("compile_validation"), CompileReceipt);
					if (!bPackagesOk || !bCompileOk)
					{
						TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
						Failure->SetStringField(TEXT("reason"), !bPackagesOk ? PackageError : CompileError);
						Failures.Add(MakeShared<FJsonValueObject>(Failure));
					}
				}
				Out->SetNumberField(TEXT("valid_count"), Valid);
				Out->SetArrayField(TEXT("failures"), Failures);
				Out->SetBoolField(TEXT("passed"), Failures.IsEmpty());
				Out->SetStringField(TEXT("status"), Failures.IsEmpty() ? TEXT("passed") : TEXT("failed"));
				Out->SetStringField(TEXT("validation_scope"), ToolName);
				if (ToolName == TEXT("asset_cross_project_receipt_validate") && bExecute)
				{
					Out->SetStringField(TEXT("schema"), TEXT("somol.cross_project_asset_acceptance_receipt.v1"));
					Out->SetStringField(TEXT("accepted_at_utc"), FDateTime::UtcNow().ToIso8601());
					const FString ReceiptFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("CrossProjectMigration"), Plan.PlanId, TEXT("acceptance_receipt.json"));
					if (!SaveJson(Out, ReceiptFile, Error)) return false;
					Out->SetStringField(TEXT("receipt_file"), ReceiptFile);
					if (!PersistCheckpoint(Plan, ToolName, Failures.IsEmpty() ? TEXT("accepted") : TEXT("rejected"), Out, Error)) return false;
				}
				Summary = FString::Printf(TEXT("%s validated %d/%d file(s)."), *ToolName, Valid, Plan.Files.Num());
				return Failures.IsEmpty();
			}
			if (ToolName == TEXT("asset_cross_project_checkpoint_get") || ToolName == TEXT("asset_cross_project_resume_plan"))
			{
				TSharedRef<FJsonObject> Checkpoint = LoadOrCreateCheckpoint(Plan);
				Out->SetObjectField(TEXT("checkpoint"), Checkpoint);
				Out->SetStringField(TEXT("checkpoint_file"), CheckpointFileForPlan(Plan.PlanId));
				if (ToolName == TEXT("asset_cross_project_resume_plan"))
				{
					TArray<TSharedPtr<FJsonValue>> Remaining;
					for (const FCrossProjectFileRow& Row : Plan.Files)
					{
						if (!FPaths::FileExists(Row.DestinationFile)
							|| !HashFileStable(Row.DestinationFile).Equals(Row.ContentHash, ESearchCase::IgnoreCase))
						{
							Remaining.Add(MakeShared<FJsonValueObject>(FileRowToJson(Row)));
						}
					}
					Out->SetArrayField(TEXT("remaining_files"), Remaining);
					Out->SetNumberField(TEXT("remaining_count"), Remaining.Num());
					Out->SetBoolField(TEXT("resume_required"), !Remaining.IsEmpty());
				}
				Out->SetStringField(TEXT("status"), TEXT("ready"));
				Summary = TEXT("Cross-project migration checkpoint is ready.");
				return true;
			}
			if (ToolName == TEXT("asset_cross_project_delta_sync_plan") || ToolName == TEXT("asset_cross_project_delta_sync_execute"))
			{
				FCrossProjectPlan ChangedPlan = Plan;
				ChangedPlan.Files.Reset();
				ChangedPlan.TotalBytes = 0;
				TArray<TSharedPtr<FJsonValue>> Changed;
				for (const FCrossProjectFileRow& Row : Plan.Files)
				{
					if (!FPaths::FileExists(Row.DestinationFile) || !HashFileStable(Row.DestinationFile).Equals(Row.ContentHash, ESearchCase::IgnoreCase))
					{
						Changed.Add(MakeShared<FJsonValueObject>(FileRowToJson(Row)));
						ChangedPlan.Files.Add(Row);
						ChangedPlan.TotalBytes += Row.SizeBytes;
					}
				}
				Out->SetArrayField(TEXT("changed_files"), Changed);
				Out->SetNumberField(TEXT("changed_count"), Changed.Num());
				if (ToolName == TEXT("asset_cross_project_delta_sync_execute") && bExecute)
				{
					if (!IsCurrentProject(Plan.TargetProjectDir))
					{
						Error = TEXT("delta_sync_execute_requires_target_project_mcp");
						return false;
					}
					const bool bOk = CopyPlanFiles(ChangedPlan, Plan.TargetContentDir, true, true, Out, Error);
					if (bOk)
					{
						TArray<FString> Files;
						for (const FCrossProjectFileRow& Row : ChangedPlan.Files) Files.Add(Row.DestinationFile);
						if (!Files.IsEmpty()) FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous(Files, true);
						if (!PersistCheckpoint(Plan, ToolName, TEXT("delta_synced"), Out, Error)) return false;
					}
					Out->SetStringField(TEXT("status"), bOk ? TEXT("delta_synced") : TEXT("partial"));
					Summary = FString::Printf(TEXT("Delta sync committed %d changed file(s)."), ChangedPlan.Files.Num());
					return bOk;
				}
				Out->SetStringField(TEXT("status"), TEXT("ready"));
				Summary = FString::Printf(TEXT("Delta sync found %d changed file(s)."), Changed.Num());
				return true;
			}
			if (ToolName == TEXT("asset_cross_project_rollback_execute"))
			{
				TArray<TSharedPtr<FJsonValue>> Restored;
				int32 Failed = 0;
				for (const FCrossProjectFileRow& Row : Plan.Files)
				{
					const FString Backup = Row.DestinationFile + TEXT(".somolmcp_backup_") + Plan.PlanId;
					if (!FPaths::FileExists(Backup))
					{
						continue;
					}
					TSharedRef<FJsonObject> Restore = MakeShared<FJsonObject>();
					Restore->SetStringField(TEXT("backup_file"), Backup);
					Restore->SetStringField(TEXT("destination_file"), Row.DestinationFile);
					if (!bExecute || IFileManager::Get().Copy(*Row.DestinationFile, *Backup, true, true) == COPY_OK)
					{
						Restore->SetStringField(TEXT("status"), bExecute ? TEXT("restored") : TEXT("would_restore"));
					}
					else
					{
						Restore->SetStringField(TEXT("status"), TEXT("restore_failed"));
						++Failed;
					}
					Restored.Add(MakeShared<FJsonValueObject>(Restore));
				}
				Out->SetArrayField(TEXT("restored_files"), Restored);
				Out->SetStringField(TEXT("status"), Failed == 0 ? (bExecute ? TEXT("rolled_back") : TEXT("dry_run")) : TEXT("partial"));
				if (bExecute && !PersistCheckpoint(Plan, ToolName, Failed == 0 ? TEXT("rolled_back") : TEXT("partial"), Out, Error)) return false;
				Summary = FString::Printf(TEXT("Rollback %s for %d backup file(s)."), bExecute ? TEXT("executed") : TEXT("previewed"), Restored.Num());
				return Failed == 0;
			}
			if (ToolName == TEXT("asset_cross_project_source_cleanup_execute"))
			{
				FString Confirmation;
				Arguments->TryGetStringField(TEXT("confirm_source_delete"), Confirmation);
				if (bExecute && Confirmation != Plan.PlanId)
				{
					Error = TEXT("source_cleanup_requires_confirm_source_delete_equal_to_plan_id");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> TargetFailures;
				for (const FCrossProjectFileRow& Row : Plan.Files)
				{
					if (!FPaths::FileExists(Row.DestinationFile)
						|| IFileManager::Get().FileSize(*Row.DestinationFile) != Row.SizeBytes
						|| !HashFileStable(Row.DestinationFile).Equals(Row.ContentHash, ESearchCase::IgnoreCase))
					{
						TSharedRef<FJsonObject> Failure = FileRowToJson(Row);
						Failure->SetStringField(TEXT("reason"), TEXT("target_receipt_revalidation_failed"));
						TargetFailures.Add(MakeShared<FJsonValueObject>(Failure));
					}
				}
				Out->SetArrayField(TEXT("target_revalidation_failures"), TargetFailures);
				if (!TargetFailures.IsEmpty())
				{
					Error = TEXT("source_cleanup_blocked_target_receipt_revalidation_failed");
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> Deleted;
				int32 Failed = 0;
				for (const FString& PackageName : Plan.Packages)
				{
					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("package_name"), PackageName);
					if (!bExecute)
					{
						Row->SetStringField(TEXT("status"), TEXT("would_delete_after_confirmation"));
					}
					else
					{
						FString DeleteError;
						const bool bDeleted = Context.Services.DeleteAsset(PackageName, DeleteError)
							&& !Context.Services.AssetExists(PackageName);
						Row->SetStringField(TEXT("status"), bDeleted ? TEXT("deleted") : TEXT("delete_failed"));
						Row->SetBoolField(TEXT("verified"), bDeleted);
						if (!DeleteError.IsEmpty()) Row->SetStringField(TEXT("error"), DeleteError);
						if (!bDeleted) ++Failed;
					}
					Deleted.Add(MakeShared<FJsonValueObject>(Row));
				}
				Out->SetArrayField(TEXT("items"), Deleted);
				Out->SetNumberField(TEXT("deleted_count"), bExecute ? Plan.Packages.Num() - Failed : 0);
				Out->SetNumberField(TEXT("failed_count"), Failed);
				Out->SetStringField(TEXT("status"), bExecute ? (Failed == 0 ? TEXT("source_deleted") : TEXT("partial")) : TEXT("dry_run_ready"));
				Out->SetArrayField(TEXT("source_packages"), StringsToJson(Plan.Packages));
				if (bExecute && !PersistCheckpoint(Plan, ToolName, Failed == 0 ? TEXT("source_deleted") : TEXT("partial"), Out, Error)) return false;
				Summary = FString::Printf(TEXT("Receipt-gated source cleanup %s for %d package(s)."), bExecute ? TEXT("executed") : TEXT("previewed"), Plan.Packages.Num());
				if (Failed > 0) Error = FString::Printf(TEXT("source_cleanup_partial_failure: %d package(s)"), Failed);
				return Failed == 0;
			}

			Error = FString::Printf(TEXT("unsupported_cross_project_tool_route: %s"), *ToolName);
			return false;
		}
	}

	void RegisterCrossProjectAssetTools(FSololmcpToolRegistry& Registry)
	{
		struct FSpec
		{
			const TCHAR* Name;
			const TCHAR* Description;
			TArray<FString> Required;
		};
		const TArray<FSpec> Specs = {
			{TEXT("asset_cross_project_capabilities"), TEXT("Report native C++ cross-project migration, staging, queue, target handoff, and receipt capabilities."), {}},
			{TEXT("asset_cross_project_target_resolve"), TEXT("Resolve source and target Unreal project directories into guarded project and Content roots."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_pair_validate"), TEXT("Fail-closed validation for a source/target Unreal project pair."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_dependency_closure"), TEXT("Resolve the recursive Asset Registry dependency closure for cross-project migration."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_external_package_closure"), TEXT("Identify World Partition ExternalActors and ExternalObjects packages in the migration closure."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_plugin_dependency_audit"), TEXT("Audit plugin, engine, and non-/Game package dependencies before migration."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_package_file_manifest"), TEXT("Map migration packages to source files, target files, size, hash, and conflict state."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_migration_plan"), TEXT("Build a deterministic cross-project asset migration plan with dependency, conflict, and byte estimates."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_conflict_scan"), TEXT("Scan target files for path, size, and content-hash conflicts."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_conflict_resolution_plan"), TEXT("Plan block, skip, overwrite, or reuse-identical conflict handling."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_path_remap_plan"), TEXT("Plan package-aware source to target path mappings; defaults to preserving package paths."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_bundle_manifest_build"), TEXT("Build and optionally persist an immutable migration bundle manifest."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_bundle_export_execute"), TEXT("Export migration files into an immutable staged bundle; defaults to dry-run."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_transfer_stage_execute"), TEXT("Stage migration files under the target project's Saved directory; defaults to dry-run."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_target_import_plan"), TEXT("Build the target-side import and source-control handoff plan."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_target_import_execute"), TEXT("Commit staged assets from the target project's MCP instance with hash verification."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_registry_refresh"), TEXT("Refresh the target Asset Registry for committed migration files."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_package_load_validate"), TEXT("Validate migrated target packages by file identity and target-side availability."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_missing_reference_scan"), TEXT("Validate migrated files and route missing-reference inspection through target-side Asset Registry tools."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_compile_validate_batch"), TEXT("Batch migration acceptance gate for Blueprint, material, Niagara, UMG, animation, and PCG compile validation."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_world_partition_validate"), TEXT("Validate maps, ExternalActors, ExternalObjects, Data Layers, and World Partition migration evidence."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_asset_family_validate"), TEXT("Validate coherent material, animation, Niagara, PCG, UMG, audio, and data asset families."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_checkpoint_get"), TEXT("Return or initialize a resumable cross-project migration checkpoint."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_resume_plan"), TEXT("Build an idempotent resume plan from the current migration checkpoint."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_delta_sync_plan"), TEXT("Compare source and target hashes and plan an incremental migration."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_delta_sync_execute"), TEXT("Execute the target-routed portion of an incremental migration through the queue contract."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_rollback_execute"), TEXT("Restore target files from migration-scoped backups; defaults to dry-run."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_receipt_validate"), TEXT("Validate final cross-project file hashes and acceptance evidence before source cleanup."), {TEXT("target_project_dir")}},
			{TEXT("asset_cross_project_source_cleanup_execute"), TEXT("Gate source cleanup behind an accepted target receipt; delegates deletion to safe asset tools."), {TEXT("target_project_dir")}}
		};

		for (const FSpec& Spec : Specs)
		{
			const FString Name(Spec.Name);
			Registry.Register({
				Name,
				Spec.Description,
				CommonSchema(Spec.Required),
				[Name, &Registry](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
				{
					return ExecuteCrossProjectTool(Name, Registry, Context, Arguments, Out, Summary, Error);
				},
				nullptr,
				Name == TEXT("asset_cross_project_capabilities") ? 30 : 0
			});
		}
	}
}
