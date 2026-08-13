// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — Substrate Material starter tools (P1-1)
//
// Tools registered (all material_substrate_* prefix):
//   material_substrate_status              — ALWAYS works. Reports build/runtime availability of Substrate.
//   material_substrate_inspect             — Read-only. Reflects an existing material for Substrate slab nodes.
//                                            Returns NOT_AVAILABLE when Substrate API isn't reachable.
//   material_substrate_create_simple       — Creates a UMaterial + Substrate root + 1 Slab BSDF.
//                                            Returns NOT_AVAILABLE when Substrate API isn't reachable.
//   material_substrate_set_slab_property   — Sets diffuse_albedo / f0 / roughness / sss_mfp on a Slab node.
//                                            Returns NOT_AVAILABLE when Substrate API isn't reachable.
//
// Design notes
// ------------
// Substrate is preview / experimental in UE 5.7. The headers and class names continue to shift between
// minor versions, and the project setting `r.Substrate=true` controls whether the engine actually compiles
// Substrate-aware code paths. To stay build-safe across configurations we:
//
//   1. Wrap Substrate-touching write code in WITH_EDITOR and gate execution on runtime
//      evidence: r.Substrate must be on and a Slab BSDF expression class must be registered.
//      UE 5.x preview builds do not consistently expose WITH_SUBSTRATE_MATERIALS /
//      SUBSTRATE_ENABLED even when the runtime classes are available, so live mutation uses
//      reflection instead of treating those defines as authoritative.
//   2. Even inside the guard, we resolve the SubstrateSlabBSDF expression class by *short name*
//      via FindFirstObject<UClass>. This avoids a hard #include on a header whose path drifts
//      ("MaterialExpressionSubstrateSlabBSDF.h" / "MaterialExpressions/Substrate/...") between
//      previews — and lets the tool degrade to NOT_AVAILABLE if the class isn't registered.
//   3. material_substrate_status doubles as a runtime probe: it checks the `r.Substrate` console
//      variable (always present even when the feature is off) plus the class-registration check,
//      so users can distinguish "build doesn't have Substrate" from "Substrate is off in this project".
//
// HARD CONSTRAINTS honored:
//   - Does NOT modify SololmcpToolRegistry.{h,cpp}
//   - Does NOT modify .Build.cs (no new module deps required for the status tool; the slab/inspect
//     paths only use UMaterial + UMaterialExpression which are already linked via Engine).
//   - Does NOT modify any other .cpp file.
//
// TODO(P1-1 follow-up): once UE 5.7 promotes Substrate from preview, replace the FindFirstObject
// path with a typed include of MaterialExpressionSubstrateSlabBSDF.h and add typed property setters
// (DiffuseAlbedo / F0 / Roughness / SSSMFP) instead of the generic GetInput(N) connections.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "HAL/IConsoleManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace UE::SOMOLMCP
{

// ============================================================================
// Detection / helpers
// ============================================================================
namespace SubstrateToolsImpl
{
	/** Find an engine UClass by its short name (no header dependency). */
	static UClass* FindClassByShortName(const TCHAR* ShortName)
	{
		if (!ShortName || !*ShortName) return nullptr;
		if (UClass* Direct = FindFirstObject<UClass>(ShortName, EFindFirstObjectOptions::ExactClass))
		{
			return Direct;
		}
		// Fallback scan – slow path, but only ever taken once per process per name.
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ShortName)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** Resolve any of the candidate Substrate-Slab class names that have shipped across UE 5.x previews. */
	static UClass* FindSubstrateSlabClass()
	{
		static const TCHAR* const Candidates[] = {
			TEXT("MaterialExpressionSubstrateSlabBSDF"),
			TEXT("MaterialExpressionSubstrateSlab"),
			TEXT("MaterialExpressionStrataSlabBSDF"), // pre-rename
			TEXT("MaterialExpressionStrataSlab"),
		};
		for (const TCHAR* Name : Candidates)
		{
			if (UClass* Cls = FindClassByShortName(Name)) return Cls;
		}
		return nullptr;
	}

	/** Read the r.Substrate console variable. Returns false if the CVar doesn't exist (older engines). */
	static bool IsSubstrateCVarEnabled(bool& bOutCVarFound)
	{
		bOutCVarFound = false;
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Substrate")))
		{
			bOutCVarFound = true;
			return CVar->GetInt() != 0;
		}
		return false;
	}

	/** Detection summary used by all 4 tools. `bSlabClassFound` reflects compile/runtime class registration;
	 *  `bCVarOn` reflects the project setting at runtime. Substrate is "available" when both are true. */
	struct FAvailability
	{
		bool bSlabClassFound = false;
		bool bCVarFound      = false;
		bool bCVarOn         = false;
		bool bBuildDefine    = false; // whether WITH_SUBSTRATE_MATERIALS/SUBSTRATE_ENABLED was on at compile time
		bool IsAvailable() const { return bSlabClassFound && bCVarOn; }
	};

	static FAvailability Probe()
	{
		FAvailability A;
#if defined(WITH_SUBSTRATE_MATERIALS) || defined(SUBSTRATE_ENABLED)
		A.bBuildDefine = true;
#endif
		A.bSlabClassFound = (FindSubstrateSlabClass() != nullptr);
		A.bCVarOn = IsSubstrateCVarEnabled(A.bCVarFound);
		return A;
	}

	/** Apply NOT_AVAILABLE response and a stable error message. Returns false (forwarded by callers). */
	static bool ReturnNotAvailable(const TSharedRef<FJsonObject>& OutStructured, FString& OutError, const FAvailability& A)
	{
		SololmcpError::Set(OutStructured, TEXT("NOT_AVAILABLE"), TEXT(""),
			TEXT("Substrate not enabled in this build (project setting r.Substrate=true required)."));
		OutStructured->SetBoolField(TEXT("substrate_enabled"), false);
		OutStructured->SetBoolField(TEXT("slab_class_found"), A.bSlabClassFound);
		OutStructured->SetBoolField(TEXT("cvar_found"),       A.bCVarFound);
		OutStructured->SetBoolField(TEXT("cvar_on"),          A.bCVarOn);
		OutStructured->SetBoolField(TEXT("build_define"),     A.bBuildDefine);
		OutError = TEXT("Substrate API not available in this build.");
		return false;
	}

	/** Parse a JSON value as an FLinearColor. Accepts [r,g,b] or [r,g,b,a]. Returns false if not an array. */
	static bool ParseColorArray(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
	{
		if (!Value.IsValid() || Value->Type != EJson::Array) return false;
		const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
		if (Arr.Num() < 3) return false;
		OutColor.R = (float)Arr[0]->AsNumber();
		OutColor.G = (float)Arr[1]->AsNumber();
		OutColor.B = (float)Arr[2]->AsNumber();
		OutColor.A = (Arr.Num() >= 4) ? (float)Arr[3]->AsNumber() : 1.0f;
		return true;
	}

	/** Split a /Game/Folder/Name path into package + name. Returns false on malformed input. */
	static bool SplitAssetPath(const FString& AssetPath, FString& OutPackage, FString& OutName)
	{
		int32 SlashIdx = INDEX_NONE;
		if (!AssetPath.FindLastChar('/', SlashIdx) || SlashIdx <= 0) return false;
		OutPackage = AssetPath.Left(SlashIdx);
		OutName    = AssetPath.RightChop(SlashIdx + 1);
		// Strip any trailing .AssetName suffix.
		int32 DotIdx = INDEX_NONE;
		if (OutName.FindChar('.', DotIdx)) OutName = OutName.Left(DotIdx);
		return !OutPackage.IsEmpty() && !OutName.IsEmpty();
	}
} // namespace SubstrateToolsImpl

// ============================================================================
// Tool: material_substrate_status — ALWAYS works
// ============================================================================
static void RegisterSubstrateStatus(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("material_substrate_status"),
		TEXT("Report Substrate availability for this build/project. Inspects r.Substrate CVar, expression class registration, and compile-time guards. Always succeeds."),
		FSololmcpSchemaBuilder::Object({}, {}),

		[](const FSololmcpToolExecutionContext& /*Context*/, const TSharedRef<FJsonObject>& /*Arguments*/, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const SubstrateToolsImpl::FAvailability A = SubstrateToolsImpl::Probe();

			OutStructured->SetBoolField(TEXT("enabled"),           A.IsAvailable());
			OutStructured->SetBoolField(TEXT("cvar_found"),        A.bCVarFound);
			OutStructured->SetBoolField(TEXT("cvar_on"),           A.bCVarOn);
			OutStructured->SetBoolField(TEXT("slab_class_found"),  A.bSlabClassFound);

			// build_define string – which compile-time guard (if any) was active.
			FString BuildDefine = TEXT("none");
#if defined(WITH_SUBSTRATE_MATERIALS)
			BuildDefine = TEXT("WITH_SUBSTRATE_MATERIALS");
#elif defined(SUBSTRATE_ENABLED)
			BuildDefine = TEXT("SUBSTRATE_ENABLED");
#endif
			OutStructured->SetStringField(TEXT("build_define"), BuildDefine);

			TSharedRef<FJsonObject> Recommended = MakeShared<FJsonObject>();
			Recommended->SetStringField(TEXT("r.Substrate"),                              TEXT("1"));
			Recommended->SetStringField(TEXT("r.Substrate.OpaqueMaterialRoughRefraction"), TEXT("1 (optional)"));
			Recommended->SetStringField(TEXT("r.Substrate.BytesPerPixel"),                TEXT("80 (default; raise for complex stacks)"));
			Recommended->SetStringField(TEXT("how_to_enable"),
				TEXT("Project Settings -> Rendering -> Substrate Materials (Beta) = ON, then restart the editor."));
			OutStructured->SetObjectField(TEXT("recommended_settings"), Recommended);

			if (A.IsAvailable())
			{
				OutSummary = TEXT("Substrate is ENABLED (CVar on, slab class registered).");
			}
			else if (!A.bCVarFound)
			{
				OutSummary = TEXT("Substrate CVar 'r.Substrate' not registered - engine likely too old.");
			}
			else if (!A.bCVarOn)
			{
				OutSummary = TEXT("Substrate is DISABLED (r.Substrate=0). Enable in Project Settings -> Rendering.");
			}
			else
			{
				OutSummary = TEXT("Substrate CVar is on but Slab BSDF class is not registered (build mismatch).");
			}
			OutError.Reset();
			return true;
		}
	});
}

// ============================================================================
// Tool: material_substrate_inspect
// ============================================================================
static void RegisterSubstrateInspect(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("material_substrate_inspect"),
		TEXT("Inspect a UMaterial for Substrate usage: counts slab nodes, reports root-node class and parameter list. Read-only."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("/Game/Path/M_Material"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			const SubstrateToolsImpl::FAvailability A = SubstrateToolsImpl::Probe();

#if WITH_EDITOR
			// ---- Substrate-enabled build path ----
			if (!A.IsAvailable())
			{
				return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
			}

			UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Material)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			UClass* SlabClass = SubstrateToolsImpl::FindSubstrateSlabClass();
			int32 SlabCount = 0;
			FString RootClass = TEXT("");
			TArray<TSharedPtr<FJsonValue>> Params;
			for (UMaterialExpression* Expr : Material->GetExpressions())
			{
				if (!Expr) continue;
				if (SlabClass && Expr->IsA(SlabClass))
				{
					if (RootClass.IsEmpty()) RootClass = Expr->GetClass()->GetName();
					++SlabCount;
				}
				if (Expr->IsA(UMaterialExpressionScalarParameter::StaticClass()) ||
					Expr->IsA(UMaterialExpressionVectorParameter::StaticClass()))
				{
					TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
					P->SetStringField(TEXT("name"),  Expr->GetName());
					P->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
					Params.Add(MakeShared<FJsonValueObject>(P));
				}
			}

			const bool bIsSubstrate = (SlabCount > 0);
			OutStructured->SetStringField(TEXT("asset_path"),      Material->GetPathName());
			OutStructured->SetBoolField  (TEXT("is_substrate"),    bIsSubstrate);
			OutStructured->SetNumberField(TEXT("slab_count"),      SlabCount);
			OutStructured->SetStringField(TEXT("root_node_class"), RootClass);
			OutStructured->SetArrayField (TEXT("parameters"),      Params);
			OutSummary = FString::Printf(TEXT("Inspected '%s': substrate=%s, slabs=%d"),
				*Material->GetPathName(), bIsSubstrate ? TEXT("true") : TEXT("false"), SlabCount);
			OutError.Reset();
			return true;
#else
			(void)Context; (void)AssetPath;
			return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
#endif
		}
	});
}

// ============================================================================
// Tool: material_substrate_create_simple
// ============================================================================
static void RegisterSubstrateCreateSimple(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("material_substrate_create_simple"),
		TEXT("Create a new UMaterial with a Substrate root + 1 Slab BSDF. Optional base_color (RGB array, default white) and roughness (0-1)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),  FSololmcpSchemaBuilder::String(TEXT("/Game/Path/M_NewSubstrate"))},
			{TEXT("base_color"),  FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Number(), TEXT("[r,g,b] (default [1,1,1])"))},
			{TEXT("roughness"),   FSololmcpSchemaBuilder::Number(TEXT("0-1 (default 0.5)"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			const SubstrateToolsImpl::FAvailability A = SubstrateToolsImpl::Probe();

#if WITH_EDITOR
			if (!A.IsAvailable())
			{
				return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
			}

			FString PackagePath, AssetName;
			if (!SubstrateToolsImpl::SplitAssetPath(AssetPath, PackagePath, AssetName))
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				OutError = TEXT("Bad asset_path (expected /Game/Folder/Name).");
				return false;
			}

			// Parse optional inputs.
			FLinearColor BaseColor = FLinearColor::White;
			SubstrateToolsImpl::ParseColorArray(Arguments->TryGetField(TEXT("base_color")), BaseColor);
			float Roughness = 0.5f;
			if (Arguments->HasTypedField<EJson::Number>(TEXT("roughness")))
			{
				Roughness = FMath::Clamp((float)Arguments->GetNumberField(TEXT("roughness")), 0.0f, 1.0f);
			}

			UClass* SlabClass = SubstrateToolsImpl::FindSubstrateSlabClass();
			if (!SlabClass)
			{
				return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
			}

			// Create the UMaterial via the standard factory.
			const FString UniqueName = Context.Services.GenerateUniqueAssetName(PackagePath, AssetName);
			const FScopedTransaction Tx(NSLOCTEXT("SOMOLMCP", "SubstrateCreateSimple", "SOMOLMCP Create Substrate Material"));
			UObject* CreatedAsset = Context.Services.CreateAsset(
				PackagePath, UniqueName,
				UMaterial::StaticClass()->GetPathName(),
				UMaterialFactoryNew::StaticClass()->GetPathName(),
				nullptr, OutError);
			UMaterial* Material = Cast<UMaterial>(CreatedAsset);
			if (!Material)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("Material factory failed to create the asset."));
				return false;
			}

			Material->PreEditChange(nullptr);

			// Add the Slab BSDF expression. Use the typed UMaterialEditingLibrary helper first
			// (handles graph registration + undo), with a manual NewObject fallback.
			UMaterialExpression* Slab = Cast<UMaterialExpression>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, SlabClass, /*Px*/0, /*Py*/0));
			if (!Slab)
			{
				Slab = NewObject<UMaterialExpression>(Material, SlabClass);
				if (Slab)
				{
					Slab->MaterialExpressionEditorX = 0;
					Slab->MaterialExpressionEditorY = 0;
					Material->GetExpressionCollection().AddExpression(Slab);
				}
			}
			if (!Slab)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("Failed to create Substrate Slab BSDF expression."));
				return false;
			}
			Slab->Desc = TEXT("Substrate Slab (P1-1 starter)");

			// Color + roughness drive the slab's first/third inputs (DiffuseAlbedo / Roughness in current preview layouts).
			bool bColorConnected = false;
			if (UMaterialExpressionVectorParameter* ColorParam = Cast<UMaterialExpressionVectorParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(Material,
						UMaterialExpressionVectorParameter::StaticClass(), -300, 0)))
			{
				ColorParam->ParameterName = FName(TEXT("DiffuseAlbedo"));
				ColorParam->DefaultValue  = BaseColor;
				if (FExpressionInput* In = Slab->GetInput(0))
				{
					In->Connect(0, ColorParam);
					bColorConnected = (In->Expression == ColorParam);
				}
			}
			if (!bColorConnected)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("base_color"),
					TEXT("Failed to create or connect the DiffuseAlbedo parameter."));
				OutError = TEXT("Substrate material color parameter verification failed.");
				return false;
			}

			bool bRoughnessConnected = false;
			if (UMaterialExpressionScalarParameter* RoughParam = Cast<UMaterialExpressionScalarParameter>(
					UMaterialEditingLibrary::CreateMaterialExpression(Material,
						UMaterialExpressionScalarParameter::StaticClass(), -300, 200)))
			{
				RoughParam->ParameterName = FName(TEXT("Roughness"));
				RoughParam->DefaultValue  = Roughness;
				// Slab input layout in preview: 0=Diffuse, 1=F0, 2=Roughness, 3=Normal.
				if (FExpressionInput* In = Slab->GetInput(2))
				{
					In->Connect(0, RoughParam);
					bRoughnessConnected = (In->Expression == RoughParam);
				}
			}
			if (!bRoughnessConnected)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("roughness"),
					TEXT("Failed to create or connect the Roughness parameter."));
				OutError = TEXT("Substrate material roughness parameter verification failed.");
				return false;
			}

			// Connect Slab -> FrontMaterial via the editor-only data when available.
			bool bFrontConnected = false;
			if (UMaterialEditorOnlyData* Editor = Material->GetEditorOnlyData())
			{
				Editor->FrontMaterial.Connect(0, Slab);
				bFrontConnected = (Editor->FrontMaterial.Expression == Slab);
			}
			if (!bFrontConnected)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("front_material"),
					TEXT("Failed to connect the Substrate slab to the material FrontMaterial root."));
				OutError = TEXT("Substrate material root connection verification failed.");
				return false;
			}

			Material->PostEditChange();

			FString MatPath = Material->GetPathName();
			int32 DotIdx = INDEX_NONE;
			if (MatPath.FindChar('.', DotIdx)) MatPath = MatPath.Left(DotIdx);
			if (!Context.Services.SaveAsset(MatPath, /*bOnlyIfDirty*/false, OutError))
			{
				SololmcpError::Set(OutStructured, TEXT("SAVE_FAILED"), TEXT("asset_path"),
					TEXT("SaveAsset returned false for the Substrate material."));
				if (OutError.IsEmpty()) OutError = FString::Printf(TEXT("Failed to save material '%s'."), *MatPath);
				return false;
			}
			SololmcpWriteFlush::EnsureFlushed(Material);
			FString VerifyError;
			UMaterial* ReloadedMaterial = Cast<UMaterial>(Context.Services.LoadAsset(MatPath, VerifyError));
			if (!ReloadedMaterial || ReloadedMaterial->GetExpressions().Num() <= 0)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("asset_path"),
					TEXT("Saved Substrate material could not be reloaded with expressions."));
				OutError = VerifyError.IsEmpty()
					? FString::Printf(TEXT("Substrate material save verification failed for '%s'."), *MatPath)
					: VerifyError;
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"),       MatPath);
			OutStructured->SetStringField(TEXT("slab_class"),       SlabClass->GetName());
			OutStructured->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
			OutStructured->SetBoolField  (TEXT("is_substrate"),     true);
			OutSummary = FString::Printf(TEXT("Created Substrate material '%s' with slab '%s'."), *MatPath, *SlabClass->GetName());
			OutError.Reset();
			return true;
#else
			(void)Context; (void)AssetPath;
			return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
#endif
		}
	});
}

// ============================================================================
// Tool: material_substrate_set_slab_property
// ============================================================================
static void RegisterSubstrateSetSlabProperty(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("material_substrate_set_slab_property"),
		TEXT("Set a property on a Slab BSDF node inside a Substrate material. property_name: diffuse_albedo|f0|roughness|sss_mfp. value is a color array for diffuse_albedo/f0/sss_mfp, a number for roughness."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"),    FSololmcpSchemaBuilder::String()},
			{TEXT("slab_index"),    FSololmcpSchemaBuilder::Integer(TEXT("0-based slab index (default 0)"))},
			{TEXT("property_name"), FSololmcpSchemaBuilder::String(TEXT("diffuse_albedo|f0|roughness|sss_mfp"),
				{TEXT("diffuse_albedo"), TEXT("f0"), TEXT("roughness"), TEXT("sss_mfp")})},
			{TEXT("value"),         FSololmcpSchemaBuilder::String(TEXT("Color array [r,g,b] OR a single number; passed through as JSON."))}
		}, {TEXT("asset_path"), TEXT("property_name"), TEXT("value")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath, PropertyName;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("property_name"));
				OutError = TEXT("Missing property_name.");
				return false;
			}
			PropertyName = PropertyName.ToLower();

			const TSharedPtr<FJsonValue> ValueField = Arguments->TryGetField(TEXT("value"));
			if (!ValueField.IsValid())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("value"));
				OutError = TEXT("Missing value.");
				return false;
			}

			int32 SlabIndex = 0;
			if (Arguments->HasTypedField<EJson::Number>(TEXT("slab_index")))
			{
				SlabIndex = (int32)Arguments->GetNumberField(TEXT("slab_index"));
			}

			const SubstrateToolsImpl::FAvailability A = SubstrateToolsImpl::Probe();

#if WITH_EDITOR
			if (!A.IsAvailable())
			{
				return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
			}

			UMaterial* Material = Cast<UMaterial>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!Material)
			{
				SololmcpError::InvalidPath(OutStructured, AssetPath);
				return false;
			}

			UClass* SlabClass = SubstrateToolsImpl::FindSubstrateSlabClass();
			if (!SlabClass)
			{
				return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
			}

			// Find the N-th slab.
			UMaterialExpression* TargetSlab = nullptr;
			int32 SeenSlabs = 0;
			for (UMaterialExpression* Expr : Material->GetExpressions())
			{
				if (Expr && Expr->IsA(SlabClass))
				{
					if (SeenSlabs == SlabIndex)
					{
						TargetSlab = Expr;
						break;
					}
					++SeenSlabs;
				}
			}
			if (!TargetSlab)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("slab[%d]"), SlabIndex));
				OutError = FString::Printf(TEXT("No Substrate slab at index %d (found %d total)."), SlabIndex, SeenSlabs);
				return false;
			}

			// Map property -> slab input index. These map to the documented Slab BSDF input order.
			//   0 = DiffuseAlbedo, 1 = F0, 2 = Roughness, 3 = (Normal/MFP varies between previews)
			// The SSS MFP index has shifted between 4-6 across previews; index 4 is the most common.
			int32 InputIdx = -1;
			bool bExpectColor = true;
			if      (PropertyName == TEXT("diffuse_albedo")) { InputIdx = 0; bExpectColor = true;  }
			else if (PropertyName == TEXT("f0"))             { InputIdx = 1; bExpectColor = true;  }
			else if (PropertyName == TEXT("roughness"))      { InputIdx = 2; bExpectColor = false; }
			else if (PropertyName == TEXT("sss_mfp"))        { InputIdx = 4; bExpectColor = true;  }
			else
			{
				SololmcpError::InvalidType(OutStructured, TEXT("property_name"), TEXT("diffuse_albedo|f0|roughness|sss_mfp"));
				OutError = FString::Printf(TEXT("Unknown property_name '%s'."), *PropertyName);
				return false;
			}

			FExpressionInput* SlabInput = TargetSlab->GetInput(InputIdx);
			if (!SlabInput)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("slab.input[%d]"), InputIdx));
				OutError = FString::Printf(TEXT("Slab input %d is not exposed by this preview build of %s."),
					InputIdx, *SlabClass->GetName());
				return false;
			}

			Material->PreEditChange(nullptr);
			const FScopedTransaction Tx(NSLOCTEXT("SOMOLMCP", "SubstrateSetSlab", "SOMOLMCP Set Substrate Slab Property"));

			// Drive the input via a freshly-created parameter expression. This always works regardless
			// of which property we're targeting and lets users tweak the value afterwards in the editor.
			bool bConnected = false;
			if (bExpectColor)
			{
				FLinearColor Color(1.f, 1.f, 1.f, 1.f);
				if (!SubstrateToolsImpl::ParseColorArray(ValueField, Color))
				{
					// Allow a single number -> grayscale fallback.
					if (ValueField->Type == EJson::Number)
					{
						const float Gray = (float)ValueField->AsNumber();
						Color = FLinearColor(Gray, Gray, Gray, 1.f);
					}
					else
					{
						SololmcpError::InvalidType(OutStructured, TEXT("value"), TEXT("array of 3-4 numbers"));
						OutError = TEXT("value must be an [r,g,b] array for color properties.");
						return false;
					}
				}
				if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(
						UMaterialEditingLibrary::CreateMaterialExpression(Material,
							UMaterialExpressionVectorParameter::StaticClass(), -400, InputIdx * 200)))
				{
					VP->ParameterName = FName(*FString::Printf(TEXT("Slab%d_%s"), SlabIndex, *PropertyName));
					VP->DefaultValue  = Color;
					SlabInput->Connect(0, VP);
					bConnected = (SlabInput->Expression == VP);
				}
			}
			else
			{
				if (ValueField->Type != EJson::Number)
				{
					SololmcpError::InvalidType(OutStructured, TEXT("value"), TEXT("number"));
					OutError = TEXT("value must be a number for scalar properties.");
					return false;
				}
				const float Scalar = FMath::Clamp((float)ValueField->AsNumber(), 0.0f, 1.0f);
				if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(
						UMaterialEditingLibrary::CreateMaterialExpression(Material,
							UMaterialExpressionScalarParameter::StaticClass(), -400, InputIdx * 200)))
				{
					SP->ParameterName = FName(*FString::Printf(TEXT("Slab%d_%s"), SlabIndex, *PropertyName));
					SP->DefaultValue  = Scalar;
					SlabInput->Connect(0, SP);
					bConnected = (SlabInput->Expression == SP);
				}
			}
			if (!bConnected)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("property_name"),
					TEXT("Failed to create or connect the requested slab property parameter."));
				OutError = FString::Printf(TEXT("Slab property '%s' connection verification failed."), *PropertyName);
				return false;
			}

			Material->PostEditChange();

			FString MatPath = Material->GetPathName();
			int32 DotIdx = INDEX_NONE;
			if (MatPath.FindChar('.', DotIdx)) MatPath = MatPath.Left(DotIdx);
			if (!Context.Services.SaveAsset(MatPath, /*bOnlyIfDirty*/false, OutError))
			{
				SololmcpError::Set(OutStructured, TEXT("SAVE_FAILED"), TEXT("asset_path"),
					TEXT("SaveAsset returned false for the Substrate material."));
				if (OutError.IsEmpty()) OutError = FString::Printf(TEXT("Failed to save material '%s'."), *MatPath);
				return false;
			}
			SololmcpWriteFlush::EnsureFlushed(Material);
			FString VerifyError;
			UMaterial* ReloadedMaterial = Cast<UMaterial>(Context.Services.LoadAsset(MatPath, VerifyError));
			if (!ReloadedMaterial || ReloadedMaterial->GetExpressions().Num() <= Material->GetExpressions().Num() - 1)
			{
				SololmcpError::Set(OutStructured, TEXT("VERIFY_FAILED"), TEXT("asset_path"),
					TEXT("Saved material could not be reloaded with the new slab property expression."));
				OutError = VerifyError.IsEmpty()
					? FString::Printf(TEXT("Substrate material property save verification failed for '%s'."), *MatPath)
					: VerifyError;
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"),    MatPath);
			OutStructured->SetNumberField(TEXT("slab_index"),    SlabIndex);
			OutStructured->SetStringField(TEXT("property_name"), PropertyName);
			OutStructured->SetNumberField(TEXT("input_index"),   InputIdx);
			OutSummary = FString::Printf(TEXT("Set %s on slab[%d] of '%s'."), *PropertyName, SlabIndex, *MatPath);
			OutError.Reset();
			return true;
#else
			(void)Context; (void)AssetPath; (void)PropertyName; (void)SlabIndex; (void)ValueField;
			return SubstrateToolsImpl::ReturnNotAvailable(OutStructured, OutError, A);
#endif
		}
	});
}

// ============================================================================
// Registration entry point - called from the registry (DO NOT modify the registry header)
// ============================================================================
void RegisterSubstrateMaterialTools(FSololmcpToolRegistry& Registry)
{
	RegisterSubstrateStatus(Registry);
	RegisterSubstrateInspect(Registry);
	RegisterSubstrateCreateSimple(Registry);
	RegisterSubstrateSetSlabProperty(Registry);
}

} // namespace UE::SOMOLMCP
