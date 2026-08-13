// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/PrimaryAssetLabel.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace UE::SOMOLMCP
{
namespace PakUpdateTools
{
	static FString PackageNameFromAssetPath(const FString& InPath)
	{
		FString Result = InPath;
		int32 Dot = INDEX_NONE;
		if (Result.FindChar(TEXT('.'), Dot)) Result = Result.Left(Dot);
		return Result;
	}

	static FString ObjectPathFromPackage(const FString& PackageName)
	{
		return PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	}

	static bool IsReleaseNameSafe(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 96) return false;
		for (TCHAR C : Value)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('.') && C != TEXT('_') && C != TEXT('-')) return false;
		}
		return true;
	}

	static bool BuildDependencyClosure(const FString& RootAssetPath, bool bIncludeEngine, int32 MaxAssets, TSet<FName>& OutPackages, TArray<TPair<FName, FName>>& OutEdges, FString& OutError)
	{
		const FString RootPackage = PackageNameFromAssetPath(RootAssetPath);
		if (!FPackageName::IsValidLongPackageName(RootPackage))
		{
			OutError = FString::Printf(TEXT("Invalid root asset path: %s"), *RootAssetPath);
			return false;
		}
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		TArray<FName> Queue;
		Queue.Add(FName(*RootPackage));
		OutPackages.Add(FName(*RootPackage));
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
		{
			TArray<FName> Dependencies;
			Registry.GetDependencies(Queue[Index], Dependencies);
			for (const FName& Dependency : Dependencies)
			{
				const FString Name = Dependency.ToString();
				if (Name.StartsWith(TEXT("/Script/"))) continue;
				if (!bIncludeEngine && Name.StartsWith(TEXT("/Engine/"))) continue;
				OutEdges.Emplace(Queue[Index], Dependency);
				if (!OutPackages.Contains(Dependency))
				{
					if (OutPackages.Num() >= MaxAssets)
					{
						OutError = FString::Printf(TEXT("Dependency closure exceeded max_assets=%d."), MaxAssets);
						return false;
					}
					OutPackages.Add(Dependency);
					Queue.Add(Dependency);
				}
			}
		}
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> NamesToJson(const TSet<FName>& Names)
	{
		TArray<FString> Sorted;
		for (const FName& Name : Names) Sorted.Add(Name.ToString());
		Sorted.Sort();
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Name : Sorted) Result.Add(MakeShared<FJsonValueString>(Name));
		return Result;
	}

	static bool UpsertPrimaryAssetLabel(const FSololmcpToolExecutionContext& Context, const FString& LabelPackagePath, int32 ChunkId, const TSet<FName>& Packages, int32 Priority, bool bRecursive, TSharedRef<FJsonObject> Receipt, FString& OutError)
	{
		if (!LabelPackagePath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(LabelPackagePath))
		{
			OutError = TEXT("label_asset_path must be a valid /Game long package path.");
			return false;
		}
		const FString ObjectPath = ObjectPathFromPackage(LabelPackagePath);
		UPrimaryAssetLabel* Label = Cast<UPrimaryAssetLabel>(Context.Services.LoadAsset(ObjectPath, OutError));
		bool bCreated = false;
		if (!Label)
		{
			OutError.Reset();
			UPackage* Package = CreatePackage(*LabelPackagePath);
			if (!Package) { OutError = TEXT("CreatePackage failed for PrimaryAssetLabel."); return false; }
			const FString AssetName = FPackageName::GetLongPackageAssetName(LabelPackagePath);
			Label = NewObject<UPrimaryAssetLabel>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
			if (!Label) { OutError = TEXT("NewObject<UPrimaryAssetLabel> failed."); return false; }
			FAssetRegistryModule::AssetCreated(Label);
			bCreated = true;
		}
		Label->Modify();
		Label->Rules.Priority = Priority;
		Label->Rules.ChunkId = ChunkId;
		Label->Rules.bApplyRecursively = bRecursive;
		Label->Rules.CookRule = EPrimaryAssetCookRule::AlwaysCook;
		Label->bLabelAssetsInMyDirectory = false;
		Label->bIsRuntimeLabel = true;
		Label->bIncludeRedirectors = false;
		Label->ExplicitAssets.Reset();
		for (const FName& PackageName : Packages)
		{
			Label->ExplicitAssets.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(ObjectPathFromPackage(PackageName.ToString()))));
		}
#if WITH_EDITORONLY_DATA
		Label->UpdateAssetBundleData();
#endif
		Label->PostEditChange();
		Label->MarkPackageDirty();
		SololmcpWriteFlush::EnsureFlushed(Label);
		FString SaveError;
		if (!Context.Services.SaveAsset(Label->GetPathName(), false, SaveError))
		{
			OutError = SaveError.IsEmpty() ? TEXT("Saving PrimaryAssetLabel failed.") : SaveError;
			return false;
		}
		UPrimaryAssetLabel* Verify = Cast<UPrimaryAssetLabel>(Context.Services.LoadAsset(Label->GetPathName(), SaveError));
		if (!Verify || Verify->Rules.ChunkId != ChunkId || Verify->ExplicitAssets.Num() != Packages.Num())
		{
			OutError = TEXT("PrimaryAssetLabel readback verification failed.");
			return false;
		}
		Receipt->SetStringField(TEXT("label_asset_path"), Label->GetPathName());
		Receipt->SetNumberField(TEXT("chunk_id"), ChunkId);
		Receipt->SetNumberField(TEXT("asset_count"), Packages.Num());
		Receipt->SetBoolField(TEXT("created"), bCreated);
		Receipt->SetBoolField(TEXT("saved"), true);
		Receipt->SetBoolField(TEXT("readback_verified"), true);
		return true;
	}

	static bool RunDependencyClosure(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath)) { Error = TEXT("asset_path is required."); return false; }
		bool bIncludeEngine = false;
		Args->TryGetBoolField(TEXT("include_engine_assets"), bIncludeEngine);
		int32 MaxAssets = 5000;
		if (Args->HasField(TEXT("max_assets"))) MaxAssets = FMath::Clamp(static_cast<int32>(Args->GetNumberField(TEXT("max_assets"))), 1, 20000);
		TSet<FName> Packages;
		TArray<TPair<FName, FName>> Edges;
		if (!BuildDependencyClosure(AssetPath, bIncludeEngine, MaxAssets, Packages, Edges, Error)) return false;
		Out->SetStringField(TEXT("root_asset"), AssetPath);
		Out->SetArrayField(TEXT("packages"), NamesToJson(Packages));
		Out->SetNumberField(TEXT("package_count"), Packages.Num());
		Out->SetNumberField(TEXT("edge_count"), Edges.Num());
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = FString::Printf(TEXT("Resolved %d packages for %s."), Packages.Num(), *AssetPath);
		return true;
	}

	static bool BuildChunkPlan(const TSharedRef<FJsonObject>& Args, TArray<FString>& OutRoots, TArray<int32>& OutChunkIds, TArray<TSet<FName>>& OutClosures, TSet<FName>& OutShared, FString& Error)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Args->TryGetArrayField(TEXT("asset_entries"), Entries) || !Entries || Entries->IsEmpty()) { Error = TEXT("asset_entries is required."); return false; }
		TMap<FName, int32> Membership;
		for (int32 Index = 0; Index < Entries->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Entry = (*Entries)[Index].IsValid() ? (*Entries)[Index]->AsObject() : nullptr;
			if (!Entry.IsValid()) continue;
			FString Root;
			if (!Entry->TryGetStringField(TEXT("asset_path"), Root)) { Error = TEXT("Each asset_entries row requires asset_path."); return false; }
			int32 ChunkId = Index + 1;
			Entry->TryGetNumberField(TEXT("chunk_id"), ChunkId);
			if (ChunkId <= 0) { Error = TEXT("Per-asset chunk_id must be greater than zero; chunk 0 is reserved for shared dependencies."); return false; }
			TSet<FName> Closure;
			TArray<TPair<FName, FName>> Edges;
			if (!BuildDependencyClosure(Root, false, 20000, Closure, Edges, Error)) return false;
			OutRoots.Add(Root);
			OutChunkIds.Add(ChunkId);
			OutClosures.Add(Closure);
			for (const FName& Package : Closure) Membership.FindOrAdd(Package)++;
		}
		for (const TPair<FName, int32>& Pair : Membership) if (Pair.Value > 1) OutShared.Add(Pair.Key);
		return !OutRoots.IsEmpty();
	}

	static bool RunChunkPlan(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		TArray<FString> Roots; TArray<int32> ChunkIds; TArray<TSet<FName>> Closures; TSet<FName> Shared;
		if (!BuildChunkPlan(Args, Roots, ChunkIds, Closures, Shared, Error)) return false;
		TArray<TSharedPtr<FJsonValue>> Chunks;
		for (int32 Index = 0; Index < Roots.Num(); ++Index)
		{
			TSet<FName> Exclusive = Closures[Index].Difference(Shared);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("root_asset"), Roots[Index]);
			Row->SetNumberField(TEXT("chunk_id"), ChunkIds[Index]);
			Row->SetArrayField(TEXT("exclusive_packages"), NamesToJson(Exclusive));
			Row->SetNumberField(TEXT("exclusive_count"), Exclusive.Num());
			Chunks.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetNumberField(TEXT("shared_chunk_id"), 0);
		Out->SetArrayField(TEXT("shared_packages"), NamesToJson(Shared));
		Out->SetNumberField(TEXT("shared_count"), Shared.Num());
		Out->SetArrayField(TEXT("chunks"), Chunks);
		Out->SetStringField(TEXT("status"), TEXT("planned"));
		Summary = FString::Printf(TEXT("Planned %d asset chunks with %d shared packages."), Chunks.Num(), Shared.Num());
		return true;
	}

	static bool RunLabelUpsert(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		FString LabelPath;
		if (!Args->TryGetStringField(TEXT("label_asset_path"), LabelPath)) { Error = TEXT("label_asset_path is required."); return false; }
		int32 ChunkId = -1;
		if (!Args->TryGetNumberField(TEXT("chunk_id"), ChunkId) || ChunkId < 0) { Error = TEXT("chunk_id must be non-negative."); return false; }
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		if (!Args->TryGetArrayField(TEXT("asset_paths"), Assets) || !Assets || Assets->IsEmpty()) { Error = TEXT("asset_paths is required."); return false; }
		TSet<FName> Packages;
		for (const TSharedPtr<FJsonValue>& Value : *Assets)
		{
			FString Path;
			if (Value.IsValid() && Value->TryGetString(Path)) Packages.Add(FName(*PackageNameFromAssetPath(Path)));
		}
		int32 Priority = 100;
		Args->TryGetNumberField(TEXT("priority"), Priority);
		bool bRecursive = true;
		Args->TryGetBoolField(TEXT("recursive"), bRecursive);
		if (!UpsertPrimaryAssetLabel(Context, LabelPath, ChunkId, Packages, Priority, bRecursive, Out, Error)) return false;
		Out->SetArrayField(TEXT("packages"), NamesToJson(Packages));
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = FString::Printf(TEXT("PrimaryAssetLabel %s assigned %d packages to chunk %d."), *LabelPath, Packages.Num(), ChunkId);
		return true;
	}

	static bool RunChunkPlanApply(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		TArray<FString> Roots; TArray<int32> ChunkIds; TArray<TSet<FName>> Closures; TSet<FName> Shared;
		if (!BuildChunkPlan(Args, Roots, ChunkIds, Closures, Shared, Error)) return false;
		FString LabelRoot = TEXT("/Game/SOMRes/PakLabels");
		Args->TryGetStringField(TEXT("label_root"), LabelRoot);
		TArray<TSharedPtr<FJsonValue>> Receipts;
		if (!Shared.IsEmpty())
		{
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			if (!UpsertPrimaryAssetLabel(Context, LabelRoot / TEXT("PAL_Shared_Chunk0"), 0, Shared, 1000, true, Receipt, Error)) return false;
			Receipts.Add(MakeShared<FJsonValueObject>(Receipt));
		}
		for (int32 Index = 0; Index < Roots.Num(); ++Index)
		{
			TSet<FName> Exclusive = Closures[Index].Difference(Shared);
			const FString SafeName = FPackageName::GetLongPackageAssetName(PackageNameFromAssetPath(Roots[Index]));
			const FString LabelPath = LabelRoot / FString::Printf(TEXT("PAL_%s_Chunk%d"), *SafeName, ChunkIds[Index]);
			TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
			if (!UpsertPrimaryAssetLabel(Context, LabelPath, ChunkIds[Index], Exclusive, 100, true, Receipt, Error)) return false;
			Receipt->SetStringField(TEXT("root_asset"), Roots[Index]);
			Receipts.Add(MakeShared<FJsonValueObject>(Receipt));
		}
		Out->SetArrayField(TEXT("label_receipts"), Receipts);
		Out->SetNumberField(TEXT("label_count"), Receipts.Num());
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = FString::Printf(TEXT("Created/updated %d PrimaryAssetLabels for the chunk plan."), Receipts.Num());
		return true;
	}

	static bool RunSettingsApply(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		if (!GConfig) { Error = TEXT("GConfig is unavailable."); return false; }
		bool bUseIoStore = false;
		Args->TryGetBoolField(TEXT("use_iostore"), bUseIoStore);
		const FString Ini = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
		const TCHAR* Section = TEXT("/Script/UnrealEd.ProjectPackagingSettings");
		GConfig->SetBool(Section, TEXT("UsePakFile"), true, Ini);
		GConfig->SetBool(Section, TEXT("bUseIoStore"), bUseIoStore, Ini);
		GConfig->SetBool(Section, TEXT("bGenerateChunks"), true, Ini);
		GConfig->SetBool(Section, TEXT("bGenerateNoChunks"), false, Ini);
		GConfig->SetBool(Section, TEXT("bChunkHardReferencesOnly"), false, Ini);
		GConfig->Flush(false, Ini);
		bool bGenerateChunks = false;
		GConfig->GetBool(Section, TEXT("bGenerateChunks"), bGenerateChunks, Ini);
		Out->SetStringField(TEXT("config_file"), Ini);
		Out->SetBoolField(TEXT("use_pak"), true);
		Out->SetBoolField(TEXT("use_iostore"), bUseIoStore);
		Out->SetBoolField(TEXT("generate_chunks"), bGenerateChunks);
		Out->SetBoolField(TEXT("readback_verified"), bGenerateChunks);
		Out->SetStringField(TEXT("status"), bGenerateChunks ? TEXT("completed") : TEXT("failed"));
		Summary = TEXT("Applied and verified Pak/IoStore chunk-generation settings.");
		return bGenerateChunks;
	}

	static bool SubmitReleaseOrPatch(FSololmcpToolRegistry& Registry, const TSharedRef<FJsonObject>& Args, bool bPatch, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		FString Target, OutputDir, Version, BaseVersion, BuildConfig = TEXT("Shipping");
		Args->TryGetStringField(TEXT("target_platform"), Target);
		Args->TryGetStringField(TEXT("output_dir"), OutputDir);
		Args->TryGetStringField(TEXT("release_version"), Version);
		Args->TryGetStringField(TEXT("base_release_version"), BaseVersion);
		Args->TryGetStringField(TEXT("build_config"), BuildConfig);
		if (Target.IsEmpty() || OutputDir.IsEmpty() || !IsReleaseNameSafe(Version) || (bPatch && !IsReleaseNameSafe(BaseVersion)))
		{
			Error = TEXT("target_platform, output_dir, and safe release version fields are required.");
			return false;
		}
		FString Extra = FString::Printf(TEXT("-NoCompileEditor -GenerateChunks -Manifests -CreateReleaseVersion=%s"), *Version);
		bool bUseIoStore = false;
		if (GConfig)
		{
			const FString PackagingIni = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultGame.ini"));
			GConfig->GetBool(TEXT("/Script/UnrealEd.ProjectPackagingSettings"), TEXT("bUseIoStore"), bUseIoStore, PackagingIni);
		}
		if (!bUseIoStore) Extra += TEXT(" -SkipIoStore");
		if (bPatch) Extra += FString::Printf(TEXT(" -GeneratePatch -BasedOnReleaseVersion=%s"), *BaseVersion);
		TSharedRef<FJsonObject> BuildArgs = MakeShared<FJsonObject>();
		BuildArgs->SetStringField(TEXT("target_platform"), Target);
		BuildArgs->SetStringField(TEXT("output_dir"), OutputDir);
		BuildArgs->SetStringField(TEXT("build_config"), BuildConfig);
		BuildArgs->SetStringField(TEXT("additional_args"), Extra);
		if (!Registry.ExecuteTool(TEXT("package_build"), BuildArgs, Out, Summary, Error)) return false;
		Out->SetStringField(TEXT("release_version"), Version);
		Out->SetStringField(TEXT("base_release_version"), BaseVersion);
		Out->SetStringField(TEXT("workflow"), bPatch ? TEXT("patch") : TEXT("base_release"));
		Out->SetStringField(TEXT("status"), TEXT("running"));
		return true;
	}

	class FPackageOutputVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		TArray<FString> Files;
		virtual bool Visit(const TCHAR* Path, bool bDirectory) override
		{
			if (!bDirectory)
			{
				const FString Ext = FPaths::GetExtension(Path).ToLower();
				if (Ext == TEXT("pak") || Ext == TEXT("utoc") || Ext == TEXT("ucas") || Ext == TEXT("sig") || Ext == TEXT("manifest")) Files.Add(Path);
			}
			return true;
		}
	};

	static bool RunManifestExport(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		FString OutputDir, Version;
		Args->TryGetStringField(TEXT("output_dir"), OutputDir);
		Args->TryGetStringField(TEXT("release_version"), Version);
		if (OutputDir.IsEmpty() || !IsReleaseNameSafe(Version)) { Error = TEXT("output_dir and safe release_version are required."); return false; }
		OutputDir = FPaths::ConvertRelativePathToFull(OutputDir);
		FPackageOutputVisitor Visitor;
		IFileManager::Get().IterateDirectoryRecursively(*OutputDir, Visitor);
		if (Visitor.Files.IsEmpty()) { Error = TEXT("No Pak/IoStore output files were found."); return false; }
		TArray<TSharedPtr<FJsonValue>> Files;
		for (const FString& File : Visitor.Files)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), File);
			Row->SetStringField(TEXT("relative_path"), File.Mid(OutputDir.Len()).TrimStartAndEnd().TrimChar(TEXT('/')).TrimChar(TEXT('\\')));
			Row->SetNumberField(TEXT("size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*File)));
			const FMD5Hash Hash = FMD5Hash::HashFile(*File);
			Row->SetStringField(TEXT("md5"), Hash.IsValid() ? BytesToHex(Hash.GetBytes(), Hash.GetSize()) : FString());
			Files.Add(MakeShared<FJsonValueObject>(Row));
		}
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("schema"), TEXT("somolmcp.pak-update-manifest.v1"));
		Manifest->SetStringField(TEXT("release_version"), Version);
		Manifest->SetStringField(TEXT("output_dir"), OutputDir);
		Manifest->SetArrayField(TEXT("files"), Files);
		Manifest->SetNumberField(TEXT("file_count"), Files.Num());
		const FString ManifestDir = FPaths::ProjectSavedDir() / TEXT("SOMOLMCP/PakUpdates");
		IFileManager::Get().MakeDirectory(*ManifestDir, true);
		const FString ManifestPath = ManifestDir / FString::Printf(TEXT("PakManifest_%s.json"), *Version);
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Manifest, Writer);
		if (!FFileHelper::SaveStringToFile(Json, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { Error = TEXT("Failed to save Pak manifest."); return false; }
		Out->SetStringField(TEXT("manifest_path"), ManifestPath);
		Out->SetNumberField(TEXT("file_count"), Files.Num());
		Out->SetArrayField(TEXT("files"), Files);
		Out->SetStringField(TEXT("status"), TEXT("completed"));
		Summary = FString::Printf(TEXT("Exported Pak update manifest with %d files."), Files.Num());
		return true;
	}

	static bool RunOutputValidate(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		FString OutputDir;
		Args->TryGetStringField(TEXT("output_dir"), OutputDir);
		if (OutputDir.IsEmpty()) { Error = TEXT("output_dir is required."); return false; }
		OutputDir = FPaths::ConvertRelativePathToFull(OutputDir);
		FPackageOutputVisitor Visitor;
		IFileManager::Get().IterateDirectoryRecursively(*OutputDir, Visitor);
		int32 PakCount = 0, UtocCount = 0, UcasCount = 0, ValidPakCount = 0;
		TArray<TSharedPtr<FJsonValue>> Checks;
		const FString UnrealPak = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/Win64/UnrealPak.exe"));
		for (const FString& File : Visitor.Files)
		{
			const FString Ext = FPaths::GetExtension(File).ToLower();
			if (Ext == TEXT("utoc")) ++UtocCount;
			else if (Ext == TEXT("ucas")) ++UcasCount;
			else if (Ext == TEXT("pak"))
			{
				++PakCount;
				int32 ReturnCode = -1; FString StdOut, StdErr;
				const bool bExec = FPaths::FileExists(UnrealPak) && FPlatformProcess::ExecProcess(*UnrealPak, *FString::Printf(TEXT("\"%s\" -List"), *File), &ReturnCode, &StdOut, &StdErr);
				const bool bValid = bExec && ReturnCode == 0;
				if (bValid) ++ValidPakCount;
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("path"), File);
				Row->SetBoolField(TEXT("valid"), bValid);
				Row->SetNumberField(TEXT("return_code"), ReturnCode);
				if (!bValid) Row->SetStringField(TEXT("error"), StdErr.Left(1000));
				Checks.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		const bool bIoStorePairsValid = UtocCount == UcasCount;
		const bool bValid = (PakCount > 0 || UtocCount > 0) && ValidPakCount == PakCount && bIoStorePairsValid;
		Out->SetNumberField(TEXT("pak_count"), PakCount);
		Out->SetNumberField(TEXT("valid_pak_count"), ValidPakCount);
		Out->SetNumberField(TEXT("utoc_count"), UtocCount);
		Out->SetNumberField(TEXT("ucas_count"), UcasCount);
		Out->SetBoolField(TEXT("iostore_pairs_valid"), bIoStorePairsValid);
		Out->SetArrayField(TEXT("archive_checks"), Checks);
		Out->SetBoolField(TEXT("valid"), bValid);
		Out->SetStringField(TEXT("status"), bValid ? TEXT("passed") : TEXT("failed"));
		Summary = FString::Printf(TEXT("Pak output validation: pak %d/%d valid, IoStore pairs %s."), ValidPakCount, PakCount, bIoStorePairsValid ? TEXT("valid") : TEXT("invalid"));
		if (!bValid) Error = TEXT("Pak/IoStore output validation failed.");
		return bValid;
	}
}

void RegisterPakUpdateTools(FSololmcpToolRegistry& Registry)
{
	using FSB = FSololmcpSchemaBuilder;
	const auto EntriesSchema = FSB::Array(FSB::Object({
		{TEXT("asset_path"), FSB::String(TEXT("Root asset package/object path."))},
		{TEXT("chunk_id"), FSB::Integer(TEXT("Unique chunk id greater than zero."))}
	}, {TEXT("asset_path")}), TEXT("Assets to package as independently updateable chunks."));
	Registry.Register({TEXT("pak_asset_dependency_closure"), TEXT("Resolve a single asset's recursive cook dependency closure from Asset Registry."), FSB::Object({
		{TEXT("asset_path"), FSB::String()}, {TEXT("include_engine_assets"), FSB::Boolean()}, {TEXT("max_assets"), FSB::Integer()}
	}, {TEXT("asset_path")}), PakUpdateTools::RunDependencyClosure, nullptr, 30});
	Registry.Register({TEXT("pak_chunk_plan"), TEXT("Plan per-asset chunks and lift dependencies shared by multiple roots into common chunk 0."), FSB::Object({{TEXT("asset_entries"), EntriesSchema}}, {TEXT("asset_entries")}), PakUpdateTools::RunChunkPlan, nullptr, 30});
	Registry.Register({TEXT("pak_primary_asset_label_upsert"), TEXT("Create or update a real UPrimaryAssetLabel consumed by Asset Manager cook/chunk generation, then save and read back."), FSB::Object({
		{TEXT("label_asset_path"), FSB::String()}, {TEXT("chunk_id"), FSB::Integer()}, {TEXT("asset_paths"), FSB::Array(FSB::String())}, {TEXT("priority"), FSB::Integer()}, {TEXT("recursive"), FSB::Boolean()}
	}, {TEXT("label_asset_path"), TEXT("chunk_id"), TEXT("asset_paths")}), PakUpdateTools::RunLabelUpsert});
	Registry.Register({TEXT("pak_chunk_plan_apply"), TEXT("Apply a per-asset chunk plan as saved PrimaryAssetLabels, with shared dependencies assigned to chunk 0."), FSB::Object({
		{TEXT("asset_entries"), EntriesSchema}, {TEXT("label_root"), FSB::String(TEXT("Default /Game/SOMRes/PakLabels."))}
	}, {TEXT("asset_entries")}), PakUpdateTools::RunChunkPlanApply});
	Registry.Register({TEXT("pak_update_settings_apply"), TEXT("Enable and verify real Pak/IoStore chunk generation in ProjectPackagingSettings."), FSB::Object({{TEXT("use_iostore"), FSB::Boolean()}}), PakUpdateTools::RunSettingsApply});
	const auto BuildSchema = FSB::Object({
		{TEXT("target_platform"), FSB::String()}, {TEXT("output_dir"), FSB::String()}, {TEXT("release_version"), FSB::String()},
		{TEXT("base_release_version"), FSB::String()}, {TEXT("build_config"), FSB::String(TEXT("Development, Shipping, or Test."))}
	}, {TEXT("target_platform"), TEXT("output_dir"), TEXT("release_version")});
	Registry.Register({TEXT("pak_release_build_submit"), TEXT("Submit an asynchronous UAT base release build with GenerateChunks, manifests, and CreateReleaseVersion."), BuildSchema,
		[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& A, TSharedRef<FJsonObject>& O, FString& S, FString& E){ return PakUpdateTools::SubmitReleaseOrPatch(Registry, A, false, O, S, E); }});
	Registry.Register({TEXT("pak_patch_build_submit"), TEXT("Submit an asynchronous UAT patch build based on an existing release version."), BuildSchema,
		[&Registry](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& A, TSharedRef<FJsonObject>& O, FString& S, FString& E){ return PakUpdateTools::SubmitReleaseOrPatch(Registry, A, true, O, S, E); }});
	Registry.Register({TEXT("pak_manifest_export"), TEXT("Hash Pak/IoStore outputs and write a versioned update manifest under Saved/SOMOLMCP/PakUpdates."), FSB::Object({
		{TEXT("output_dir"), FSB::String()}, {TEXT("release_version"), FSB::String()}
	}, {TEXT("output_dir"), TEXT("release_version")}), PakUpdateTools::RunManifestExport, nullptr, 30});
	Registry.Register({TEXT("pak_output_validate"), TEXT("Validate Pak archives with UnrealPak -List and verify IoStore .utoc/.ucas pairing."), FSB::Object({{TEXT("output_dir"), FSB::String()}}, {TEXT("output_dir")}), PakUpdateTools::RunOutputValidate, nullptr, 30});
}
}
