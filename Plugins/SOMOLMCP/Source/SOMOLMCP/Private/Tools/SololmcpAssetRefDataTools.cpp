// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpAssetRefDataTools.cpp — SOMOLMCP P2-4 + P2-5
//
// P2-4 AssetRegistry references (3 tools):
//   - asset_find_references     enumerate referencers (hard/soft/both) of an asset
//   - asset_replace_references  redirect everything pointing at A to point at B
//   - asset_fix_redirectors     walk + flatten ObjectRedirectors under a path filter
//
// P2-5 DataAsset CRUD (3 tools):
//   - dataasset_create          NewObject<UDataAsset>(Pkg, Class, Name, RF_Public|RF_Standalone)
//   - dataasset_set_property    ImportText_Direct onto a CPF_Edit UPROPERTY
//   - dataasset_get_property    reflection-based read (single property or all CPF_Edit)
//
// All mutators use FScopedTransaction + Asset->Modify() + MarkPackageDirty()
// + SololmcpWriteFlush::EnsureFlushed(Asset) per the project conventions.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

// ── UE Core / Reflection ──
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SoftObjectPath.h"
#include "Engine/DataAsset.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

// ── AssetRegistry ──
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"  // UE 5.7: replaces removed AssetDependencyInfo.h; FAssetIdentifier struct

// ── AssetTools / ObjectTools ──
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"  // ObjectTools::ConsolidateObjects

#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPAssetRefData, Log, All);

namespace UE::SOMOLMCP
{
namespace
{
	// ───────────────────────── Helpers ─────────────────────────

	/** Strip the .Foo suffix from "/Game/Path/Foo.Foo" so the result is just "/Game/Path/Foo". */
	static FString StripObjectSuffix(const FString& In)
	{
		FString Out = In;
		int32 DotIdx = INDEX_NONE;
		if (Out.FindChar('.', DotIdx))
		{
			Out = Out.Left(DotIdx);
		}
		return Out;
	}

	/** Resolve "/Game/Foo/Bar" to FName("/Game/Foo/Bar") suitable for AssetRegistry calls. */
	static FName PackageNameFromAssetPath(const FString& AssetPath)
	{
		return FName(*StripObjectSuffix(AssetPath));
	}

	/** Load a UObject from path (handles both /Game/Foo/Bar and /Game/Foo/Bar.Bar forms). */
	static UObject* LoadAssetByPath(const FString& AssetPath, FString& OutError)
	{
		UObject* Loaded = LoadObject<UObject>(nullptr, *AssetPath, nullptr, LOAD_None, nullptr);
		if (!Loaded)
		{
			// Try with .AssetName suffix
			const FString Stripped = StripObjectSuffix(AssetPath);
			const FString AssetName = FPackageName::GetLongPackageAssetName(Stripped);
			const FString FullObjectPath = Stripped + TEXT(".") + AssetName;
			Loaded = LoadObject<UObject>(nullptr, *FullObjectPath, nullptr, LOAD_None, nullptr);
		}
		if (!Loaded)
		{
			OutError = FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath);
		}
		return Loaded;
	}

	static void SetToolStatus(TSharedRef<FJsonObject>& OutStructured, const bool bSuccess)
	{
		OutStructured->SetBoolField(TEXT("success"), bSuccess);
		OutStructured->SetStringField(TEXT("status"), bSuccess ? TEXT("success") : TEXT("failed"));
	}

	static bool VerifyAssetByPath(
		const FString& AssetPath,
		const UClass* ExpectedClass,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		FString VerifyError;
		UObject* Verified = LoadAssetByPath(AssetPath, VerifyError);
		const bool bVerified = Verified && (!ExpectedClass || Verified->IsA(ExpectedClass));
		OutStructured->SetBoolField(TEXT("verified"), bVerified);
		if (Verified)
		{
			OutStructured->SetStringField(TEXT("verified_path"), Verified->GetPathName());
		}
		if (!bVerified)
		{
			OutError = FString::Printf(TEXT("Post-operation asset verification failed for '%s'."), *AssetPath);
			SetToolStatus(OutStructured, false);
			return false;
		}
		return true;
	}

	/** Get the IAssetRegistry singleton (UE 5.x: IAssetRegistry::Get() or via module). */
	static IAssetRegistry* GetAssetRegistry()
	{
		// IAssetRegistry::Get() returns nullptr if module hasn't loaded; fall back to module API.
		if (IAssetRegistry* AR = IAssetRegistry::Get())
		{
			return AR;
		}
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return &ARM.Get();
	}

	/** Resolve a class path string. Accepts:
	 *  - "PrimaryDataAsset" / "DataAsset"  (short native names — search by FName)
	 *  - "/Script/Engine.DataAsset"        (full native object path)
	 *  - "/Game/Schemas/MyBase.MyBase_C"   (Blueprint-generated class)
	 */
	static UClass* ResolveDataAssetClass(const FString& ClassSpec, FString& OutError)
	{
		if (ClassSpec.IsEmpty())
		{
			OutError = TEXT("Empty data_class.");
			return nullptr;
		}

		// 1) Direct path (script class or BP generated class)
		UClass* Found = FindObject<UClass>(nullptr, *ClassSpec);
		if (!Found)
		{
			// 2) LoadClass for /Game/... paths (BP)
			Found = LoadObject<UClass>(nullptr, *ClassSpec);
		}
		if (!Found)
		{
			// 3) Short name — try common Engine variants
			static const TCHAR* CandidatePrefixes[] = {
				TEXT("/Script/Engine."),
				TEXT("/Script/CoreUObject."),
			};
			for (const TCHAR* Prefix : CandidatePrefixes)
			{
				const FString Candidate = FString(Prefix) + ClassSpec;
				Found = FindObject<UClass>(nullptr, *Candidate);
				if (Found) break;
			}
		}
		if (!Found)
		{
			OutError = FString::Printf(TEXT("Could not resolve class '%s'."), *ClassSpec);
			return nullptr;
		}
		if (!Found->IsChildOf(UDataAsset::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a UDataAsset subclass."), *ClassSpec);
			return nullptr;
		}
		if (Found->HasAnyClassFlags(CLASS_Abstract))
		{
			// PrimaryDataAsset itself is fine to instantiate (UPrimaryDataAsset is concrete);
			// only refuse if marked abstract.
			OutError = FString::Printf(TEXT("Class '%s' is abstract; cannot instantiate."), *ClassSpec);
			return nullptr;
		}
		return Found;
	}

	/** Convert a JSON value to a string suitable for FProperty::ImportText_Direct. */
	static FString JsonValueToImportTextString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return FString();
		switch (Value->Type)
		{
		case EJson::String: return Value->AsString();
		case EJson::Number:
		{
			const double N = Value->AsNumber();
			// Avoid scientific notation; ImportText for integers expects integer-looking text.
			if (FMath::IsNearlyEqual(N, FMath::TruncToDouble(N)))
			{
				return FString::Printf(TEXT("%lld"), (int64)N);
			}
			return FString::SanitizeFloat(N);
		}
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Null:    return TEXT("None");
		default:
		{
			// Object/Array — serialise back to JSON string and let ImportText try.
			// ImportText for structs / TArray accepts (X=1,Y=2) / ("a","b") syntax,
			// not raw JSON. The caller is responsible for using the UE text format
			// for compound types. Best-effort: stringify and let ImportText fail loudly.
			FString Out;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
			return Out;
		}
		}
	}

	/** Stringify the current value of an FProperty for the structured response. */
	static FString ExportPropertyToString(FProperty* Property, const void* Container)
	{
		FString Out;
		if (Property && Container)
		{
			Property->ExportText_InContainer(0, Out, Container, Container, nullptr, PPF_None);
		}
		return Out;
	}

	/** Build a JSON object describing one CPF_Edit UPROPERTY: {name, type, value}. */
	static TSharedRef<FJsonObject> PropertyToJson(FProperty* Property, const void* Container)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		if (!Property) return J;
		J->SetStringField(TEXT("name"), Property->GetName());
		J->SetStringField(TEXT("type"), Property->GetCPPType());
		J->SetStringField(TEXT("value"), ExportPropertyToString(Property, Container));
		return J;
	}
}

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

void RegisterAssetRefDataTools(FSololmcpToolRegistry& Registry)
{
	using SB = FSololmcpSchemaBuilder;

	// ────────────────────────────────────────────────────────────────────────
	// (P2-4) Tool 1/3 — asset_find_references
	// ────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("asset_find_references"),
		TEXT("List all assets that reference the given asset (P2-4). reference_type: 'hard' (LOAD/HARD package deps), 'soft' (FSoftObjectPath / soft refs), or 'both' (default). Returns {asset_path, reference_count, references:[{path,type,in_package}]}."),
		SB::Object(
			{
				{TEXT("asset_path"),     SB::String(TEXT("Full object/package path of the target asset, e.g. '/Game/Props/SM_Chair'."))},
				{TEXT("reference_type"), SB::String(TEXT("'hard' | 'soft' | 'both' (default 'both')."), {TEXT("hard"), TEXT("soft"), TEXT("both")})},
				{TEXT("max_results"),    SB::Integer(TEXT("Cap returned references (default 200)."))},
			},
			{TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& OutError) -> bool
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing 'asset_path'.");
				return false;
			}

			FString RefType = TEXT("both");
			Arguments->TryGetStringField(TEXT("reference_type"), RefType);
			RefType = RefType.ToLower();
			if (RefType != TEXT("hard") && RefType != TEXT("soft") && RefType != TEXT("both"))
			{
				SololmcpError::InvalidType(OutStructured, TEXT("reference_type"), TEXT("'hard'|'soft'|'both'"));
				OutError = TEXT("reference_type must be 'hard', 'soft', or 'both'.");
				return false;
			}

			int32 MaxResults = 200;
			Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
			MaxResults = FMath::Clamp(MaxResults, 1, 5000);

			IAssetRegistry* AR = GetAssetRegistry();
			if (!AR)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT(""), TEXT("AssetRegistry unavailable."));
				OutError = TEXT("AssetRegistry unavailable.");
				return false;
			}

			const FName PkgName = PackageNameFromAssetPath(AssetPath);
			FString SourceLoadError;
			UObject* SourceAsset = LoadAssetByPath(AssetPath, SourceLoadError);
			if (!SourceAsset)
			{
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetBoolField(TEXT("source_found"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("source_not_found"));
				SololmcpError::Set(OutStructured, TEXT("NOT_FOUND"), TEXT("asset_path"),
					TEXT("Source asset was not found."));
				OutError = FString::Printf(TEXT("Source asset not found: %s"), *AssetPath);
				return false;
			}
			OutStructured->SetBoolField(TEXT("source_found"), true);

			// TODO(P2-4): On older UE 4.x the API was GetReferencers(FName, TArray<FName>&,
			// EAssetRegistryDependencyType::Type). UE 5.x switched to UE::AssetRegistry::FDependencyQuery
			// + EDependencyCategory::Package. We use the simpler GetReferencers(FName, OutArray)
			// overload which returns all package referencers and then we re-query per-result for
			// hard-vs-soft classification via GetDependencies on the referencer. If a tighter API
			// surface is preferred, swap to FDependencyQuery {Categories=Package, Flags=Hard|Soft}.

			TArray<FName> AllReferencers;
			AR->GetReferencers(PkgName, AllReferencers);

			// For hard/soft classification, look up the EACH referencer's dependencies and
			// see whether OUR package appears there as Hard or Soft. We use the overload that
			// returns FAssetIdentifier dependencies if available; fall back to FName otherwise.
			TArray<TSharedPtr<FJsonValue>> RefsJson;
			RefsJson.Reserve(FMath::Min(AllReferencers.Num(), MaxResults));

			int32 EmittedCount = 0;
			for (const FName& Referencer : AllReferencers)
			{
				if (EmittedCount >= MaxResults) break;

				// Determine Hard vs Soft by looking up the referencer's dependencies and
				// checking whether OUR package is listed as Hard or Soft.
				TArray<FName> HardDeps, SoftDeps;
				// EAssetRegistryDependencyType is deprecated in UE 5.x but still present.
				// Use the modern UE::AssetRegistry::FDependencyQuery for the future-proof path.
#if WITH_EDITOR
				{
					UE::AssetRegistry::FDependencyQuery HardQuery;
					HardQuery.Required = UE::AssetRegistry::EDependencyProperty::Hard;
					AR->GetDependencies(Referencer, HardDeps, UE::AssetRegistry::EDependencyCategory::Package, HardQuery);

					UE::AssetRegistry::FDependencyQuery SoftQuery;
					SoftQuery.Excluded = UE::AssetRegistry::EDependencyProperty::Hard;
					AR->GetDependencies(Referencer, SoftDeps, UE::AssetRegistry::EDependencyCategory::Package, SoftQuery);
				}
#endif
				const bool bIsHard = HardDeps.Contains(PkgName);
				const bool bIsSoft = SoftDeps.Contains(PkgName);

				// Filter by requested reference_type
				if (RefType == TEXT("hard") && !bIsHard) continue;
				if (RefType == TEXT("soft") && !bIsSoft) continue;

				FString TypeStr = TEXT("unknown");
				if (bIsHard && bIsSoft) TypeStr = TEXT("both");
				else if (bIsHard)       TypeStr = TEXT("hard");
				else if (bIsSoft)       TypeStr = TEXT("soft");

				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("path"), Referencer.ToString());
				Entry->SetStringField(TEXT("type"), TypeStr);
				Entry->SetBoolField(TEXT("in_package"), true);  // GetReferencers only returns package-level refs
				RefsJson.Add(MakeShared<FJsonValueObject>(Entry));
				++EmittedCount;
			}

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("reference_count"), EmittedCount);
			OutStructured->SetNumberField(TEXT("total_referencer_count"), AllReferencers.Num());
			OutStructured->SetArrayField(TEXT("references"), RefsJson);
			OutStructured->SetStringField(TEXT("reference_type"), RefType);
			OutStructured->SetStringField(TEXT("status"), EmittedCount > 0 ? TEXT("ok") : TEXT("no_referencers"));

			OutSummary = FString::Printf(
				TEXT("Found %d %s referencers for '%s' (of %d total package referencers)."),
				EmittedCount, *RefType, *AssetPath, AllReferencers.Num());
			return true;
		}
	});

	// ────────────────────────────────────────────────────────────────────────
	// (P2-4) Tool 2/3 — asset_replace_references
	// ────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("asset_replace_references"),
		TEXT("Replace references from one asset to another (P2-4). With dry_run=true (default) only enumerates candidate referencers. With dry_run=false, calls ObjectTools::ConsolidateObjects to redirect every reference of from_path → to_path. Returns {candidates, replaced_count, failed:[...]}."),
		SB::Object(
			{
				{TEXT("from_path"), SB::String(TEXT("Asset path to redirect AWAY from."))},
				{TEXT("to_path"),   SB::String(TEXT("Asset path to redirect TO."))},
				{TEXT("dry_run"),   SB::Boolean(TEXT("If true (default), only enumerate; do not modify."))},
			},
			{TEXT("from_path"), TEXT("to_path")}),

		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& OutError) -> bool
		{
			FString FromPath, ToPath;
			if (!Arguments->TryGetStringField(TEXT("from_path"), FromPath) || FromPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("from_path"));
				OutError = TEXT("Missing 'from_path'.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("to_path"), ToPath) || ToPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("to_path"));
				OutError = TEXT("Missing 'to_path'.");
				return false;
			}

			bool bDryRun = true;
			if (Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run")))
			{
				bDryRun = Arguments->GetBoolField(TEXT("dry_run"));
			}

			IAssetRegistry* AR = GetAssetRegistry();
			if (!AR)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT(""), TEXT("AssetRegistry unavailable."));
				OutError = TEXT("AssetRegistry unavailable.");
				return false;
			}

			// Enumerate candidate referencers
			const FName FromPkg = PackageNameFromAssetPath(FromPath);
			TArray<FName> Referencers;
			AR->GetReferencers(FromPkg, Referencers);

			TArray<TSharedPtr<FJsonValue>> CandidatesJson;
			CandidatesJson.Reserve(Referencers.Num());
			for (const FName& R : Referencers)
			{
				CandidatesJson.Add(MakeShared<FJsonValueString>(R.ToString()));
			}
			OutStructured->SetArrayField(TEXT("candidates"), CandidatesJson);
			OutStructured->SetNumberField(TEXT("candidate_count"), Referencers.Num());
			OutStructured->SetStringField(TEXT("from_path"), FromPath);
			OutStructured->SetStringField(TEXT("to_path"), ToPath);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);

			TArray<TSharedPtr<FJsonValue>> FailedJson;

			if (bDryRun)
			{
				OutStructured->SetNumberField(TEXT("replaced_count"), 0);
				OutStructured->SetArrayField(TEXT("failed"), FailedJson);
				SetToolStatus(OutStructured, true);
				OutSummary = FString::Printf(
					TEXT("[dry_run] %d candidate referencers point at '%s'; would redirect to '%s'."),
					Referencers.Num(), *FromPath, *ToPath);
				return true;
			}

			// LIVE PATH — actually consolidate.
			UObject* FromAsset = LoadAssetByPath(FromPath, OutError);
			if (!FromAsset)
			{
				SololmcpError::InvalidPath(OutStructured, FromPath);
				return false;
			}
			UObject* ToAsset = LoadAssetByPath(ToPath, OutError);
			if (!ToAsset)
			{
				SololmcpError::InvalidPath(OutStructured, ToPath);
				return false;
			}

			// Reject obvious type mismatch — ConsolidateObjects requires compatible types.
			if (FromAsset->GetClass() != ToAsset->GetClass()
			    && !FromAsset->GetClass()->IsChildOf(ToAsset->GetClass())
			    && !ToAsset->GetClass()->IsChildOf(FromAsset->GetClass()))
			{
				SololmcpError::Set(
					OutStructured, TEXT("INVALID_TYPE"), TEXT("to_path"),
					FString::Printf(TEXT("Class mismatch: from=%s to=%s. ConsolidateObjects needs compatible types."),
						*FromAsset->GetClass()->GetName(), *ToAsset->GetClass()->GetName()));
				OutError = TEXT("Class mismatch between from/to.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AssetReplaceRefs", "Asset Replace References"));
			ToAsset->Modify();
			FromAsset->Modify();

			// ObjectTools::ConsolidateObjects(NewObject, ObjectsToConsolidate, …):
			//   - NewObject is the survivor (ToAsset).
			//   - ObjectsToConsolidate is the list to be replaced (FromAsset).
			TArray<UObject*> ToConsolidate;
			ToConsolidate.Add(FromAsset);

			// TODO(P2-4): The ConsolidateObjects signature varies between UE versions:
			//   UE 5.x:  FConsolidationResults ConsolidateObjects(UObject* NewObject,
			//               TArray<UObject*>& ObjectsToConsolidate,
			//               TArray<UObject*>& InObjectsToConsolidateWithin,
			//               TSet<UObject*>& ObjectsToNotConsolidateWithin,
			//               bool bShouldShowDeleteConfirmation);
			// We use the most common 2-arg overload + bShouldShowDeleteConfirmation=false.
			// UE 5.7: 3-arg ConsolidateObjects(ObjectToConsolidateTo, ObjectsToConsolidate, bShowDeleteConfirmation).
			// Suppress UI confirmation since this is an MCP tool call (caller decides safety via dry_run).
			ObjectTools::FConsolidationResults Results = ObjectTools::ConsolidateObjects(
				ToAsset,
				ToConsolidate,
				/*bShowDeleteConfirmation=*/ false);

			const int32 ReplacedCount = Results.DirtiedPackages.Num();
			const int32 FailedCount = Results.FailedConsolidationObjs.Num();
			const int32 InvalidCount = Results.InvalidConsolidationObjs.Num();
			for (TObjectPtr<UObject>& Failed : Results.FailedConsolidationObjs)
			{
				if (Failed)
				{
					FailedJson.Add(MakeShared<FJsonValueString>(Failed->GetPathName()));
				}
			}
			for (TObjectPtr<UObject>& Invalid : Results.InvalidConsolidationObjs)
			{
				if (Invalid)
				{
					FailedJson.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("[invalid] %s"), *Invalid->GetPathName())));
				}
			}

			SololmcpWriteFlush::EnsureFlushed(ToAsset);

			OutStructured->SetNumberField(TEXT("replaced_count"), ReplacedCount);
			OutStructured->SetArrayField(TEXT("failed"), FailedJson);
			OutStructured->SetNumberField(TEXT("failed_count"), FailedCount);
			OutStructured->SetNumberField(TEXT("invalid_count"), InvalidCount);
			OutStructured->SetNumberField(TEXT("dirtied_packages"), Results.DirtiedPackages.Num());

			if (FailedCount > 0 || InvalidCount > 0)
			{
				OutStructured->SetBoolField(TEXT("success"), false);
				OutStructured->SetStringField(TEXT("status"),
					ReplacedCount > 0 ? TEXT("partial_success") : TEXT("failed"));
				OutError = FString::Printf(
					TEXT("ConsolidateObjects reported %d failed and %d invalid object(s)."),
					FailedCount, InvalidCount);
				OutSummary = FString::Printf(
					TEXT("Reference replacement incomplete: %s -> %s. Dirtied %d package(s), %d failed, %d invalid."),
					*FromPath, *ToPath, ReplacedCount, FailedCount, InvalidCount);
				return false;
			}

			OutSummary = FString::Printf(
				TEXT("Replaced references: %s → %s. Dirtied %d package(s), %d failed."),
				*FromPath, *ToPath, ReplacedCount, FailedJson.Num());
			SetToolStatus(OutStructured, true);
			return true;
		}
	});

	// ────────────────────────────────────────────────────────────────────────
	// (P2-4) Tool 3/3 — asset_fix_redirectors
	// ────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("asset_fix_redirectors"),
		TEXT("Walk every UObjectRedirector under path_filter (default '/Game') and flatten it via IAssetTools::FixupReferencers. Returns {found, fixed, failed:[...]}."),
		SB::Object(
			{
				{TEXT("path_filter"), SB::String(TEXT("Optional content path scope, e.g. '/Game/Materials'. Default '/Game'."))},
			},
			{}),

		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& OutError) -> bool
		{
			FString PathFilter = TEXT("/Game");
			Arguments->TryGetStringField(TEXT("path_filter"), PathFilter);
			if (PathFilter.IsEmpty()) PathFilter = TEXT("/Game");

			IAssetRegistry* AR = GetAssetRegistry();
			if (!AR)
			{
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT(""), TEXT("AssetRegistry unavailable."));
				OutError = TEXT("AssetRegistry unavailable.");
				return false;
			}

			// Build an ARFilter for ObjectRedirector under the given path
			FARFilter Filter;
			Filter.bRecursivePaths = true;
			Filter.PackagePaths.Add(FName(*PathFilter));
			Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());

			TArray<FAssetData> RedirectorAssets;
			AR->GetAssets(Filter, RedirectorAssets);

			TArray<TSharedPtr<FJsonValue>> FailedJson;
			TArray<UObjectRedirector*> Redirectors;
			Redirectors.Reserve(RedirectorAssets.Num());
			for (const FAssetData& Data : RedirectorAssets)
			{
				if (UObject* Loaded = Data.GetAsset())
				{
					if (UObjectRedirector* Redir = Cast<UObjectRedirector>(Loaded))
					{
						Redirectors.Add(Redir);
					}
				}
				else
				{
					TSharedRef<FJsonObject> Fail = MakeShared<FJsonObject>();
					Fail->SetStringField(TEXT("path"), Data.GetObjectPathString());
					Fail->SetStringField(TEXT("reason"), TEXT("Failed to load redirector."));
					FailedJson.Add(MakeShared<FJsonValueObject>(Fail));
				}
			}

			OutStructured->SetNumberField(TEXT("found"), RedirectorAssets.Num());
			OutStructured->SetStringField(TEXT("path_filter"), PathFilter);

			if (Redirectors.Num() == 0)
			{
				OutStructured->SetNumberField(TEXT("fixed"), 0);
				OutStructured->SetArrayField(TEXT("failed"), FailedJson);
				if (FailedJson.Num() > 0)
				{
					SetToolStatus(OutStructured, false);
					OutError = TEXT("Failed to load one or more redirectors.");
					OutSummary = FString::Printf(
						TEXT("Found %d redirector asset(s) under '%s', but %d failed to load."),
						RedirectorAssets.Num(), *PathFilter, FailedJson.Num());
					return false;
				}
				SetToolStatus(OutStructured, true);
				OutSummary = FString::Printf(TEXT("No redirectors found under '%s'."), *PathFilter);
				return true;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "AssetFixRedirectors", "Fix Redirectors"));

			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			IAssetTools& AssetTools = AssetToolsModule.Get();

			// FixupReferencers patches every referencer in-place to bypass the redirector,
			// then deletes the (now-orphaned) redirectors.
			AssetTools.FixupReferencers(Redirectors);

			TArray<FAssetData> RemainingRedirectors;
			AR->GetAssets(Filter, RemainingRedirectors);
			for (const FAssetData& Remaining : RemainingRedirectors)
			{
				TSharedRef<FJsonObject> Fail = MakeShared<FJsonObject>();
				Fail->SetStringField(TEXT("path"), Remaining.GetObjectPathString());
				Fail->SetStringField(TEXT("reason"), TEXT("Redirector still exists after FixupReferencers."));
				FailedJson.Add(MakeShared<FJsonValueObject>(Fail));
			}

			const int32 FixedCount = FMath::Max(0, Redirectors.Num() - RemainingRedirectors.Num());
			OutStructured->SetNumberField(TEXT("fixed"), FixedCount);
			OutStructured->SetNumberField(TEXT("remaining"), RemainingRedirectors.Num());
			OutStructured->SetArrayField(TEXT("failed"), FailedJson);

			if (RemainingRedirectors.Num() > 0 || FailedJson.Num() > 0)
			{
				OutStructured->SetBoolField(TEXT("success"), false);
				OutStructured->SetStringField(TEXT("status"),
					FixedCount > 0 ? TEXT("partial_success") : TEXT("failed"));
				OutError = FString::Printf(
					TEXT("FixupReferencers left %d redirector(s) under '%s'."),
					RemainingRedirectors.Num(), *PathFilter);
				OutSummary = FString::Printf(
					TEXT("Fixed %d/%d loaded redirector(s) under '%s'; %d issue(s) remain."),
					FixedCount, Redirectors.Num(), *PathFilter, FailedJson.Num());
				return false;
			}

			SetToolStatus(OutStructured, true);
			OutSummary = FString::Printf(
				TEXT("Fixed up %d redirector(s) under '%s' (%d failed to load)."),
				FixedCount, *PathFilter, FailedJson.Num());
			return true;
		}
	});

	// ────────────────────────────────────────────────────────────────────────
	// (P2-5) Tool 1/3 — dataasset_create
	// ────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("dataasset_create"),
		TEXT("Create a new UDataAsset (P2-5). data_class accepts a short name ('PrimaryDataAsset'/'DataAsset'), a script path ('/Script/Engine.PrimaryDataAsset'), or a Blueprint-generated class path ('/Game/Schemas/DA_HeroStatsBase.DA_HeroStatsBase_C'). Returns {asset_path, class}."),
		SB::Object(
			{
				{TEXT("asset_path"), SB::String(TEXT("Target asset path, e.g. '/Game/Data/DA_HeroStats'."))},
				{TEXT("data_class"), SB::String(TEXT("DataAsset subclass spec — see description."))},
			},
			{TEXT("asset_path"), TEXT("data_class")}),

		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& OutError) -> bool
		{
			FString AssetPath, ClassSpec;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing 'asset_path'.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("data_class"), ClassSpec) || ClassSpec.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("data_class"));
				OutError = TEXT("Missing 'data_class'.");
				return false;
			}

			if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
				return false;
			}

			UClass* DataClass = ResolveDataAssetClass(ClassSpec, OutError);
			if (!DataClass)
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("data_class"), OutError);
				return false;
			}

			const FString PackagePath = StripObjectSuffix(AssetPath);
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);

			UPackage* Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("CreatePackage failed."));
				OutError = FString::Printf(TEXT("CreatePackage failed for %s"), *PackagePath);
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DataAssetCreate", "Create DataAsset"));

			UDataAsset* NewDA = NewObject<UDataAsset>(
				Package, DataClass, *AssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			if (!NewDA)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("NewObject<UDataAsset> returned null."));
				OutError = TEXT("NewObject<UDataAsset> returned null.");
				return false;
			}

			NewDA->Modify();
			FAssetRegistryModule::AssetCreated(NewDA);
			NewDA->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(NewDA);

			OutStructured->SetStringField(TEXT("asset_path"), NewDA->GetPathName());
			OutStructured->SetStringField(TEXT("class"), DataClass->GetPathName());
			SetToolStatus(OutStructured, true);
			if (!VerifyAssetByPath(NewDA->GetPathName(), DataClass, OutStructured, OutError))
			{
				return false;
			}

			OutSummary = FString::Printf(TEXT("Created DataAsset '%s' (class %s)."),
				*NewDA->GetPathName(), *DataClass->GetName());
			return true;
		}
	});

	// ────────────────────────────────────────────────────────────────────────
	// (P2-5) Tool 2/3 — dataasset_set_property
	// ────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("dataasset_set_property"),
		TEXT("Set a single UPROPERTY on a UDataAsset by name (P2-5). value is any JSON; it is stringified and applied via FProperty::ImportText_Direct. For struct/array properties, supply UE text format (e.g. '(X=1,Y=2,Z=3)' or '(\"a\",\"b\")') as a string. Returns {property, old_value, new_value}."),
		SB::Object(
			{
				{TEXT("asset_path"),    SB::String(TEXT("Existing DataAsset path."))},
				{TEXT("property_name"), SB::String(TEXT("UPROPERTY name (FName key, case-sensitive)."))},
				{TEXT("value"),         SB::String(TEXT("New value. JSON primitives auto-convert; pass UE text format strings for structs/arrays."))},
			},
			{TEXT("asset_path"), TEXT("property_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& OutError) -> bool
		{
			FString AssetPath, PropName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing 'asset_path'.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("property_name"), PropName) || PropName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("property_name"));
				OutError = TEXT("Missing 'property_name'.");
				return false;
			}

			TSharedPtr<FJsonValue> RawValue = Arguments->TryGetField(TEXT("value"));
			if (!RawValue.IsValid())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("value"));
				OutError = TEXT("Missing 'value'.");
				return false;
			}

			UObject* Asset = LoadAssetByPath(AssetPath, OutError);
			if (!Asset)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}
			if (!Asset->IsA<UDataAsset>())
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("asset_path"),
					FString::Printf(TEXT("Asset '%s' is %s, not a UDataAsset."),
						*AssetPath, *Asset->GetClass()->GetName()));
				OutError = TEXT("Asset is not a UDataAsset.");
				return false;
			}

			FProperty* Property = Asset->GetClass()->FindPropertyByName(*PropName);
			if (!Property)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("property '%s'"), *PropName));
				OutError = FString::Printf(TEXT("Property '%s' not found on %s."),
					*PropName, *Asset->GetClass()->GetName());
				return false;
			}

			// Capture old value for the response
			const FString OldValueStr = ExportPropertyToString(Property, Asset);

			const FString NewValueStr = JsonValueToImportTextString(RawValue);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DataAssetSetProp", "DataAsset Set Property"));
			Asset->Modify();

			// TODO(P2-5): On UE 5.0/5.1 the API was Property->ImportText(*Str, ValuePtr, PPF_None, Object).
			// UE 5.2+ renamed to ImportText_Direct(Buffer, ValuePtr, OwnerObject, PortFlags).
			// We use the modern signature; if the link fails on older engines, switch.
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Asset);
			const TCHAR* ImportResult = Property->ImportText_Direct(*NewValueStr, ValuePtr, Asset, PPF_None);
			if (ImportResult == nullptr)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("value"),
					FString::Printf(TEXT("ImportText_Direct failed for property '%s' with value '%s'."),
						*PropName, *NewValueStr));
				OutError = TEXT("ImportText failed.");
				return false;
			}

			// Notify the asset that a property changed
			FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(ChangeEvent);
			Asset->MarkPackageDirty();

			SololmcpWriteFlush::EnsureFlushed(Asset);

			const FString FinalValueStr = ExportPropertyToString(Property, Asset);

			OutStructured->SetStringField(TEXT("property"), PropName);
			OutStructured->SetStringField(TEXT("old_value"), OldValueStr);
			OutStructured->SetStringField(TEXT("new_value"), FinalValueStr);
			OutStructured->SetStringField(TEXT("type"), Property->GetCPPType());
			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			SetToolStatus(OutStructured, true);
			if (!VerifyAssetByPath(Asset->GetPathName(), UDataAsset::StaticClass(), OutStructured, OutError))
			{
				return false;
			}

			OutSummary = FString::Printf(TEXT("Set %s.%s = %s (was: %s)."),
				*Asset->GetName(), *PropName, *FinalValueStr, *OldValueStr);
			return true;
		}
	});

	// ────────────────────────────────────────────────────────────────────────
	// (P2-5) Tool 3/3 — dataasset_get_property
	// ────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("dataasset_get_property"),
		TEXT("Read UPROPERTY value(s) from a UDataAsset (P2-5). If property_name is given, return that one. Otherwise return ALL properties marked CPF_Edit. Returns {property: {...}} or {properties: [...]}."),
		SB::Object(
			{
				{TEXT("asset_path"),    SB::String(TEXT("Existing DataAsset path."))},
				{TEXT("property_name"), SB::String(TEXT("Optional. If omitted, returns every CPF_Edit property."))},
			},
			{TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& OutError) -> bool
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing 'asset_path'.");
				return false;
			}

			UObject* Asset = LoadAssetByPath(AssetPath, OutError);
			if (!Asset)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}
			if (!Asset->IsA<UDataAsset>())
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("asset_path"),
					FString::Printf(TEXT("Asset '%s' is %s, not a UDataAsset."),
						*AssetPath, *Asset->GetClass()->GetName()));
				OutError = TEXT("Asset is not a UDataAsset.");
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());

			FString PropName;
			const bool bSingle = Arguments->TryGetStringField(TEXT("property_name"), PropName) && !PropName.IsEmpty();

			if (bSingle)
			{
				FProperty* Property = Asset->GetClass()->FindPropertyByName(*PropName);
				if (!Property)
				{
					SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("property '%s'"), *PropName));
					OutError = FString::Printf(TEXT("Property '%s' not found on %s."),
						*PropName, *Asset->GetClass()->GetName());
					return false;
				}
				OutStructured->SetObjectField(TEXT("property"), PropertyToJson(Property, Asset));
				OutSummary = FString::Printf(TEXT("Read %s.%s = %s."),
					*Asset->GetName(), *PropName, *ExportPropertyToString(Property, Asset));
				return true;
			}

			// All CPF_Edit properties
			TArray<TSharedPtr<FJsonValue>> PropsJson;
			int32 Count = 0;
			for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property) continue;
				if (!Property->HasAnyPropertyFlags(CPF_Edit)) continue;
				PropsJson.Add(MakeShared<FJsonValueObject>(PropertyToJson(Property, Asset)));
				++Count;
			}
			OutStructured->SetArrayField(TEXT("properties"), PropsJson);
			OutStructured->SetNumberField(TEXT("property_count"), Count);

			OutSummary = FString::Printf(TEXT("Read %d CPF_Edit properties from '%s'."),
				Count, *Asset->GetPathName());
			return true;
		}
	});
}

} // namespace UE::SOMOLMCP
