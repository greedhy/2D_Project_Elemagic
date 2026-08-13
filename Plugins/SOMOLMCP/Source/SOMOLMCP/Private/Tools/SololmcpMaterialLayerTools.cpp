// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — P1-3 Material Layer Tools
//
// Adds 5 tools for UE5.x Material Layer system:
//   material_layer_create        — create UMaterialFunctionMaterialLayer (or LayerInstance)
//   material_layer_blend_create  — create UMaterialFunctionMaterialLayerBlend (or Instance)
//   material_add_layer_stack     — add a layer stack to a UMaterial
//   material_set_layer_param     — write a parameter on a MaterialInstance layer override
//   material_layer_inspect       — inspect layer stack on UMaterial / Layer asset / MI
//
// IMPORTANT: This file is NOT auto-wired. The registry author wires it:
//   1. Forward-declare in SololmcpToolRegistry.h:
//        void RegisterMaterialLayerTools(FSololmcpToolRegistry& Registry);
//   2. Call from FSololmcpToolRegistry::FSololmcpToolRegistry() in SololmcpToolRegistry.cpp:
//        RegisterMaterialLayerTools(*this);
//
// Module deps (already in .Build.cs): MaterialEditor, Materials, Engine, UnrealEd.
//
// Engine APIs used (UE 5.x):
//   - UMaterialFunctionMaterialLayer / Instance        (Materials/MaterialFunctionMaterialLayer.h)
//   - UMaterialFunctionMaterialLayerBlend / Instance   (Materials/MaterialFunctionMaterialLayerBlend.h)
//   - FMaterialLayersFunctions                         (Materials/MaterialLayersFunctions.h)
//   - UMaterial::bUseMaterialAttributes, GetMaterialLayers/SetMaterialLayers (UE 5.x)
//   - UMaterialInstanceConstant::StaticParameters / SetMaterialLayers
//   - Factories/MaterialFunctionMaterialLayerFactory.h, MaterialFunctionMaterialLayerBlendFactory.h
//
// All TODO(P1-3) tags mark APIs that may need adjustment for the exact UE point release.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpWriteFlush.h"
#include "SololmcpErrorHelpers.h"
#include "Services/SololmcpEditorServices.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialLayersFunctions.h"
#include "MaterialEditingLibrary.h"

// Factories for creating layer assets via the asset tools pipeline.
// These class paths are resolved by name through CreateAsset, so an unresolved
// include is non-fatal at compile time but kept here for IWYU clarity.
// TODO(P1-3): Confirm these factory headers exist on the target UE branch; if
// not, fall back to NewObject<UMaterialFunctionMaterialLayer>() with manual
// package wiring.

#define LOCTEXT_NAMESPACE "SOMOLMCP_MaterialLayer"

namespace UE::SOMOLMCP
{
	namespace
	{
		// ---------------- Path helpers ----------------

		/** Split "/Game/Folder/Name" into ("/Game/Folder", "Name"). */
		bool SplitPackageAndName(const FString& AssetPath, FString& OutPackage, FString& OutName)
		{
			int32 SlashIdx = INDEX_NONE;
			if (!AssetPath.FindLastChar('/', SlashIdx) || SlashIdx <= 0)
			{
				return false;
			}
			OutPackage = AssetPath.Left(SlashIdx);
			OutName = AssetPath.RightChop(SlashIdx + 1);
			return !OutName.IsEmpty() && !OutPackage.IsEmpty();
		}

		/** Persist a freshly created asset and verify on-disk presence. */
		bool PersistNewAsset(FSololmcpEditorServices& Services, UObject* Asset, FString& OutFinalPath, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("PersistNewAsset: null asset");
				return false;
			}
			OutFinalPath = Asset->GetPathName();
			UClass* ExpectedClass = Asset->GetClass();
			Asset->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Asset);
			FString SaveErr;
			if (!Services.SaveAsset(OutFinalPath, false, SaveErr))
			{
				OutError = FString::Printf(TEXT("asset_save_failed_after_create: %s (save_err=%s)"), *OutFinalPath, *SaveErr);
				return false;
			}
			if (!Services.AssetExists(OutFinalPath))
			{
				OutError = FString::Printf(TEXT("asset_not_persisted_after_create: %s (save_err=%s)"), *OutFinalPath, *SaveErr);
				return false;
			}
			SololmcpWriteFlush::EnsureFlushed(Asset);
			FString ReloadErr;
			UObject* Reloaded = Services.LoadAsset(OutFinalPath, ReloadErr);
			if (!Reloaded || !Reloaded->IsA(ExpectedClass))
			{
				OutError = FString::Printf(TEXT("asset_reload_validation_failed_after_create: %s (reload_err=%s)"), *OutFinalPath, *ReloadErr);
				return false;
			}
			return true;
		}

		// ---------------- Layer asset helpers ----------------

		/** Create a UMaterialFunctionMaterialLayer asset directly via NewObject + package.
		 *  We avoid the editor factory path because the factory class lives in UnrealEd and may
		 *  not be available in shipping editor configurations. */
		UMaterialFunctionMaterialLayer* CreateLayerAsset(FSololmcpEditorServices& Services, const FString& PackagePath, const FString& AssetName, FString& OutError)
		{
			const FString FullPath = PackagePath + TEXT("/") + AssetName;
			if (Services.AssetExists(FullPath))
			{
				OutError = FString::Printf(TEXT("asset_already_exists: %s"), *FullPath);
				return nullptr;
			}
			// Try Factory route first (cleanest in editor builds).
			// Class path is resolved by name string so missing header doesn't break build.
			UObject* Created = Services.CreateAsset(
				PackagePath, AssetName,
				UMaterialFunctionMaterialLayer::StaticClass()->GetPathName(),
				TEXT("/Script/UnrealEd.MaterialFunctionMaterialLayerFactory"),
				nullptr, OutError);
			if (Created && Created->IsA<UMaterialFunctionMaterialLayer>())
			{
				return Cast<UMaterialFunctionMaterialLayer>(Created);
			}
			// Fallback: TODO(P1-3) implement raw NewObject path if factory is unavailable.
			if (!Created)
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("CreateAsset returned null for MaterialFunctionMaterialLayer");
				}
			}
			else
			{
				OutError = FString::Printf(TEXT("CreateAsset returned unexpected class: %s"), *Created->GetClass()->GetPathName());
			}
			return nullptr;
		}

		UMaterialFunctionMaterialLayerInstance* CreateLayerInstanceAsset(FSololmcpEditorServices& Services, const FString& PackagePath, const FString& AssetName, UMaterialFunctionMaterialLayer* Parent, FString& OutError)
		{
			TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
			if (Parent)
			{
				// TODO(P1-3): The factory may use a different override key ("Parent" vs "InitialParent").
				Overrides->SetStringField(TEXT("InitialParent"), Parent->GetPathName());
				Overrides->SetStringField(TEXT("Parent"), Parent->GetPathName());
			}
			UObject* Created = Services.CreateAsset(
				PackagePath, AssetName,
				UMaterialFunctionMaterialLayerInstance::StaticClass()->GetPathName(),
				TEXT("/Script/UnrealEd.MaterialFunctionInstanceFactory"),
				Overrides, OutError);
			if (Created && Created->IsA<UMaterialFunctionMaterialLayerInstance>())
			{
				UMaterialFunctionMaterialLayerInstance* Inst = Cast<UMaterialFunctionMaterialLayerInstance>(Created);
				if (Parent && Inst)
				{
					Inst->SetParent(Parent);
				}
				return Inst;
			}
			if (!Created && OutError.IsEmpty())
			{
				OutError = TEXT("CreateAsset returned null for MaterialFunctionMaterialLayerInstance");
			}
			return nullptr;
		}

		UMaterialFunctionMaterialLayerBlend* CreateBlendAsset(FSololmcpEditorServices& Services, const FString& PackagePath, const FString& AssetName, FString& OutError)
		{
			UObject* Created = Services.CreateAsset(
				PackagePath, AssetName,
				UMaterialFunctionMaterialLayerBlend::StaticClass()->GetPathName(),
				TEXT("/Script/UnrealEd.MaterialFunctionMaterialLayerBlendFactory"),
				nullptr, OutError);
			if (Created && Created->IsA<UMaterialFunctionMaterialLayerBlend>())
			{
				return Cast<UMaterialFunctionMaterialLayerBlend>(Created);
			}
			if (!Created && OutError.IsEmpty())
			{
				OutError = TEXT("CreateAsset returned null for MaterialFunctionMaterialLayerBlend");
			}
			return nullptr;
		}

		UMaterialFunctionMaterialLayerBlendInstance* CreateBlendInstanceAsset(FSololmcpEditorServices& Services, const FString& PackagePath, const FString& AssetName, UMaterialFunctionMaterialLayerBlend* Parent, FString& OutError)
		{
			TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
			if (Parent)
			{
				Overrides->SetStringField(TEXT("InitialParent"), Parent->GetPathName());
				Overrides->SetStringField(TEXT("Parent"), Parent->GetPathName());
			}
			UObject* Created = Services.CreateAsset(
				PackagePath, AssetName,
				UMaterialFunctionMaterialLayerBlendInstance::StaticClass()->GetPathName(),
				TEXT("/Script/UnrealEd.MaterialFunctionInstanceFactory"),
				Overrides, OutError);
			if (Created && Created->IsA<UMaterialFunctionMaterialLayerBlendInstance>())
			{
				UMaterialFunctionMaterialLayerBlendInstance* Inst = Cast<UMaterialFunctionMaterialLayerBlendInstance>(Created);
				if (Parent && Inst)
				{
					Inst->SetParent(Parent);
				}
				return Inst;
			}
			if (!Created && OutError.IsEmpty())
			{
				OutError = TEXT("CreateAsset returned null for MaterialFunctionMaterialLayerBlendInstance");
			}
			return nullptr;
		}

		// ---------------- Layer stack helpers ----------------

		/** Build a JSON description of the layer stack on a UMaterial. */
		void LayerStackToJson(const FMaterialLayersFunctions& Layers, TSharedRef<FJsonObject>& Out)
		{
			TArray<TSharedPtr<FJsonValue>> LayerArr;
			const int32 LayerCount = Layers.Layers.Num();
			const int32 BlendCount = Layers.Blends.Num();
			for (int32 i = 0; i < LayerCount; ++i)
			{
				TSharedRef<FJsonObject> LayerObj = MakeShared<FJsonObject>();
				LayerObj->SetNumberField(TEXT("index"), i);
				UMaterialFunctionInterface* LayerFn = Layers.Layers[i];
				LayerObj->SetStringField(TEXT("name"), LayerFn ? LayerFn->GetName() : TEXT(""));
				LayerObj->SetStringField(TEXT("path"), LayerFn ? LayerFn->GetPathName() : TEXT(""));
				// Blend i associates with layer i+1 (layer 0 = base, no blend).
				const int32 BlendIdx = i - 1;
				UMaterialFunctionInterface* BlendFn = (BlendIdx >= 0 && BlendIdx < BlendCount) ? Layers.Blends[BlendIdx] : nullptr;
				LayerObj->SetStringField(TEXT("blend"), BlendFn ? BlendFn->GetPathName() : TEXT(""));
				int32 ParamCount = 0;
				// TODO(P1-3): Walk LayerFn graph for Parameter expressions to count exposed parameters.
				LayerObj->SetNumberField(TEXT("parameter_count"), ParamCount);
				LayerArr.Add(MakeShared<FJsonValueObject>(LayerObj));
			}
			Out->SetArrayField(TEXT("layers"), LayerArr);
			Out->SetNumberField(TEXT("layers_count"), LayerCount);
			Out->SetNumberField(TEXT("blends_count"), BlendCount);
		}

		/** Convert a JSON value to a string suitable for FProperty::ImportText_Direct. */
		FString JsonValueToImport(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return FString();
			switch (Value->Type)
			{
			case EJson::String:  return Value->AsString();
			case EJson::Number:  return FString::SanitizeFloat(Value->AsNumber());
			case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
			case EJson::Null:    return TEXT("None");
			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
				FString Out = TEXT("(");
				for (int32 i = 0; i < Arr.Num(); ++i)
				{
					if (i > 0) Out += TEXT(",");
					Out += JsonValueToImport(Arr[i]);
				}
				Out += TEXT(")");
				return Out;
			}
			default: return FString();
			}
		}
	} // namespace

	// =====================================================================
	// Tool registration
	// =====================================================================
	void RegisterMaterialLayerTools(FSololmcpToolRegistry& Registry)
	{
		using SB = FSololmcpSchemaBuilder;

		// ---------------------------------------------------------------------
		// 1) material_layer_create
		// ---------------------------------------------------------------------
		Registry.Register({
			TEXT("material_layer_create"),
			TEXT("Create a UMaterialFunctionMaterialLayer asset (or LayerInstance if parent_layer_path is provided)."),
			SB::Object({
				{TEXT("asset_path"), SB::String(TEXT("/Game/Materials/MLI_X — destination path"))},
				{TEXT("parent_layer_path"), SB::String(TEXT("Optional UMaterialFunctionMaterialLayer to instance from"))}
			}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
			{
				FString AssetPath;
				if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				FString PackagePath, AssetName;
				if (!SplitPackageAndName(AssetPath, PackagePath, AssetName))
				{
					SololmcpError::InvalidPath(OutStructured, AssetPath);
					OutError = FString::Printf(TEXT("Bad asset_path '%s' (expected /Game/Folder/Name)"), *AssetPath);
					return false;
				}

				FString ParentPath;
				Args->TryGetStringField(TEXT("parent_layer_path"), ParentPath);

				const FScopedTransaction Transaction(LOCTEXT("MatLayerCreate", "SOMOLMCP Create Material Layer"));

				UObject* CreatedObj = nullptr;
				FString TypeStr;
				FString FinalPath;

				if (ParentPath.IsEmpty())
				{
					// Blank layer asset
					UMaterialFunctionMaterialLayer* Layer = CreateLayerAsset(Ctx.Services, PackagePath, AssetName, OutError);
					if (!Layer)
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					if (!PersistNewAsset(Ctx.Services, Layer, FinalPath, OutError))
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					CreatedObj = Layer;
					TypeStr = TEXT("layer");
				}
				else
				{
					// LayerInstance parented to existing layer
					UObject* ParentObj = Ctx.Services.LoadAsset(ParentPath, OutError);
					UMaterialFunctionMaterialLayer* ParentLayer = Cast<UMaterialFunctionMaterialLayer>(ParentObj);
					if (!ParentLayer)
					{
						SololmcpError::InvalidPath(OutStructured, ParentPath);
						OutError = FString::Printf(TEXT("parent_layer_path is not a UMaterialFunctionMaterialLayer: %s"), *ParentPath);
						return false;
					}
					UMaterialFunctionMaterialLayerInstance* Inst = CreateLayerInstanceAsset(Ctx.Services, PackagePath, AssetName, ParentLayer, OutError);
					if (!Inst)
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					if (!PersistNewAsset(Ctx.Services, Inst, FinalPath, OutError))
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					CreatedObj = Inst;
					TypeStr = TEXT("layer_instance");
				}

				OutStructured->SetStringField(TEXT("asset_path"), FinalPath);
				OutStructured->SetStringField(TEXT("type"), TypeStr);
				OutStructured->SetStringField(TEXT("parent"), ParentPath);
				OutSummary = FString::Printf(TEXT("Created %s '%s'"), *TypeStr, *FinalPath);
				return true;
			}
		, nullptr
		, 0
		});

		// ---------------------------------------------------------------------
		// 2) material_layer_blend_create
		// ---------------------------------------------------------------------
		Registry.Register({
			TEXT("material_layer_blend_create"),
			TEXT("Create a UMaterialFunctionMaterialLayerBlend asset (or BlendInstance if parent_blend_path is provided)."),
			SB::Object({
				{TEXT("asset_path"), SB::String(TEXT("/Game/Materials/MLB_Mix — destination path"))},
				{TEXT("parent_blend_path"), SB::String(TEXT("Optional UMaterialFunctionMaterialLayerBlend to instance from"))}
			}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
			{
				FString AssetPath;
				if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
					OutError = TEXT("Missing asset_path.");
					return false;
				}
				FString PackagePath, AssetName;
				if (!SplitPackageAndName(AssetPath, PackagePath, AssetName))
				{
					SololmcpError::InvalidPath(OutStructured, AssetPath);
					OutError = FString::Printf(TEXT("Bad asset_path '%s'"), *AssetPath);
					return false;
				}

				FString ParentPath;
				Args->TryGetStringField(TEXT("parent_blend_path"), ParentPath);

				const FScopedTransaction Transaction(LOCTEXT("MatLayerBlendCreate", "SOMOLMCP Create Material Layer Blend"));

				FString TypeStr;
				FString FinalPath;
				UObject* CreatedObj = nullptr;

				if (ParentPath.IsEmpty())
				{
					UMaterialFunctionMaterialLayerBlend* Blend = CreateBlendAsset(Ctx.Services, PackagePath, AssetName, OutError);
					if (!Blend)
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					if (!PersistNewAsset(Ctx.Services, Blend, FinalPath, OutError))
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					CreatedObj = Blend;
					TypeStr = TEXT("blend");
				}
				else
				{
					UObject* ParentObj = Ctx.Services.LoadAsset(ParentPath, OutError);
					UMaterialFunctionMaterialLayerBlend* ParentBlend = Cast<UMaterialFunctionMaterialLayerBlend>(ParentObj);
					if (!ParentBlend)
					{
						SololmcpError::InvalidPath(OutStructured, ParentPath);
						OutError = FString::Printf(TEXT("parent_blend_path is not a UMaterialFunctionMaterialLayerBlend: %s"), *ParentPath);
						return false;
					}
					UMaterialFunctionMaterialLayerBlendInstance* Inst = CreateBlendInstanceAsset(Ctx.Services, PackagePath, AssetName, ParentBlend, OutError);
					if (!Inst)
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					if (!PersistNewAsset(Ctx.Services, Inst, FinalPath, OutError))
					{
						SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), OutError);
						return false;
					}
					CreatedObj = Inst;
					TypeStr = TEXT("blend_instance");
				}

				OutStructured->SetStringField(TEXT("asset_path"), FinalPath);
				OutStructured->SetStringField(TEXT("type"), TypeStr);
				OutStructured->SetStringField(TEXT("parent"), ParentPath);
				OutSummary = FString::Printf(TEXT("Created %s '%s'"), *TypeStr, *FinalPath);
				return true;
			}
		, nullptr
		, 0
		});

		// ---------------------------------------------------------------------
		// 3) material_add_layer_stack
		// ---------------------------------------------------------------------
		Registry.Register({
			TEXT("material_add_layer_stack"),
			TEXT("Enable Material Attributes on the parent UMaterial and add a base layer (UMaterialFunctionMaterialLayer)."),
			SB::Object({
				{TEXT("material_path"), SB::String(TEXT("/Game/Materials/M_Parent"))},
				{TEXT("base_layer_path"), SB::String(TEXT("/Game/Materials/MLI_Base — UMaterialFunctionMaterialLayer asset"))}
			}, {TEXT("material_path"), TEXT("base_layer_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
			{
				FString MaterialPath;
				if (!Args->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("material_path"));
					OutError = TEXT("Missing material_path.");
					return false;
				}
				FString BaseLayerPath;
				if (!Args->TryGetStringField(TEXT("base_layer_path"), BaseLayerPath) || BaseLayerPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("base_layer_path"));
					OutError = TEXT("Missing base_layer_path.");
					return false;
				}

				UMaterial* Material = Cast<UMaterial>(Ctx.Services.LoadAsset(MaterialPath, OutError));
				if (!Material)
				{
					SololmcpError::InvalidPath(OutStructured, MaterialPath);
					OutError = FString::Printf(TEXT("Material not found or wrong type: %s"), *MaterialPath);
					return false;
				}
				UObject* LayerObj = Ctx.Services.LoadAsset(BaseLayerPath, OutError);
				UMaterialFunctionMaterialLayer* BaseLayer = Cast<UMaterialFunctionMaterialLayer>(LayerObj);
				if (!BaseLayer)
				{
					SololmcpError::InvalidPath(OutStructured, BaseLayerPath);
					OutError = FString::Printf(TEXT("base_layer_path is not a UMaterialFunctionMaterialLayer: %s"), *BaseLayerPath);
					return false;
				}

				const FScopedTransaction Transaction(LOCTEXT("MatAddLayerStack", "SOMOLMCP Add Material Layer Stack"));
				Material->Modify();
				UMaterialExpressionMaterialAttributeLayers* LayerExpression = nullptr;
				for (UMaterialExpression* Expression : Material->GetExpressions())
				{
					if (UMaterialExpressionMaterialAttributeLayers* Candidate = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
					{
						LayerExpression = Candidate;
						break;
					}
				}
				if (!LayerExpression)
				{
					LayerExpression = Cast<UMaterialExpressionMaterialAttributeLayers>(
						UMaterialEditingLibrary::CreateMaterialExpression(
							Material, UMaterialExpressionMaterialAttributeLayers::StaticClass(), -320, 0));
				}
				if (!LayerExpression)
				{
					OutError = TEXT("Failed to create Material Attribute Layers expression.");
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("material_path"), OutError);
					return false;
				}

				LayerExpression->Modify();
				FMaterialLayersFunctions& Layers = LayerExpression->DefaultLayers;
				if (Layers.IsEmpty())
				{
					Layers.AddDefaultBackgroundLayer();
				}
				if (Layers.Layers.IsEmpty())
				{
					OutError = TEXT("Material layer stack failed to allocate its background layer.");
					return false;
				}
				Layers.Layers[0] = BaseLayer;
				if (Layers.EditorOnly.LayerNames.Num() > 0)
				{
					Layers.EditorOnly.LayerNames[0] = FText::FromName(BaseLayer->GetFName());
				}
				LayerExpression->RebuildLayerGraph(false);
				Material->bUseMaterialAttributes = true;
				if (!UMaterialEditingLibrary::ConnectMaterialProperty(LayerExpression, TEXT(""), MP_MaterialAttributes))
				{
					OutError = TEXT("Failed to connect the layer stack expression to Material Attributes.");
					return false;
				}
				Material->PostEditChange();
				UMaterialEditingLibrary::RecompileMaterial(Material);
				Material->MarkPackageDirty();
				SololmcpWriteFlush::EnsureFlushed(Material);
				FString SaveError;
				if (!Ctx.Services.SaveAsset(MaterialPath, false, SaveError))
				{
					OutError = FString::Printf(TEXT("Failed to save material layer stack: %s"), *SaveError);
					return false;
				}

				FMaterialLayersFunctions ReadbackLayers;
				const bool bReadback = Material->GetMaterialLayers(ReadbackLayers)
					&& ReadbackLayers.Layers.Num() > 0
					&& ReadbackLayers.Layers[0] == BaseLayer;
				if (!bReadback)
				{
					OutError = TEXT("Material layer stack did not read back the requested base layer after save.");
					SololmcpError::Set(OutStructured, TEXT("POST_WRITE_READBACK_FAILED"), TEXT("base_layer_path"), OutError);
					return false;
				}

				OutStructured->SetStringField(TEXT("material"), MaterialPath);
				OutStructured->SetStringField(TEXT("base_layer"), BaseLayerPath);
				OutStructured->SetStringField(TEXT("layer_expression"), LayerExpression->GetPathName());
				OutStructured->SetNumberField(TEXT("layer_count"), ReadbackLayers.Layers.Num());
				OutStructured->SetBoolField(TEXT("applied"), true);
				OutStructured->SetBoolField(TEXT("saved"), true);
				OutStructured->SetBoolField(TEXT("readback_verified"), true);
				OutStructured->SetBoolField(TEXT("use_material_attributes"), Material->bUseMaterialAttributes);
				OutSummary = FString::Printf(TEXT("Added base layer '%s' to material '%s'; compiled, saved, and verified."),
					*BaseLayerPath, *MaterialPath);
				return true;
			}
		, nullptr
		, 0
		});

		// ---------------------------------------------------------------------
		// 4) material_set_layer_param
		// ---------------------------------------------------------------------
		Registry.Register({
			TEXT("material_set_layer_param"),
			TEXT("Set a parameter override on a specific layer of a MaterialInstance using the layer stack."),
			SB::Object({
				{TEXT("mli_path"), SB::String(TEXT("MaterialInstance asset path"))},
				{TEXT("layer_index"), SB::Integer(TEXT("0 = base layer"))},
				{TEXT("parameter_name"), SB::String()},
				{TEXT("value"), SB::String(TEXT("ImportText-style: number, true/false, (R=,G=,B=,A=), or asset path"))}
			}, {TEXT("mli_path"), TEXT("layer_index"), TEXT("parameter_name"), TEXT("value")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
			{
				FString MliPath;
				if (!Args->TryGetStringField(TEXT("mli_path"), MliPath) || MliPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("mli_path"));
					OutError = TEXT("Missing mli_path.");
					return false;
				}
				int32 LayerIndex = -1;
				{
					double Tmp = 0;
					if (Args->TryGetNumberField(TEXT("layer_index"), Tmp))
					{
						LayerIndex = static_cast<int32>(Tmp);
					}
					else
					{
						SololmcpError::MissingParam(OutStructured, TEXT("layer_index"));
						OutError = TEXT("Missing layer_index.");
						return false;
					}
				}
				FString ParamName;
				if (!Args->TryGetStringField(TEXT("parameter_name"), ParamName) || ParamName.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("parameter_name"));
					OutError = TEXT("Missing parameter_name.");
					return false;
				}
				const TSharedPtr<FJsonValue> ValueField = Args->TryGetField(TEXT("value"));
				if (!ValueField.IsValid())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("value"));
					OutError = TEXT("Missing value.");
					return false;
				}

				UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Ctx.Services.LoadAsset(MliPath, OutError));
				if (!MIC)
				{
					SololmcpError::InvalidPath(OutStructured, MliPath);
					OutError = FString::Printf(TEXT("MaterialInstanceConstant not found: %s"), *MliPath);
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> SupportedValueTypes;
				SupportedValueTypes.Add(MakeShared<FJsonValueString>(TEXT("scalar:number")));
				SupportedValueTypes.Add(MakeShared<FJsonValueString>(TEXT("vector:array[3|4]")));
				SupportedValueTypes.Add(MakeShared<FJsonValueString>(TEXT("texture:/Game or /Engine asset path")));
				SupportedValueTypes.Add(MakeShared<FJsonValueString>(TEXT("static_switch:boolean")));
				OutStructured->SetStringField(TEXT("schema"), TEXT("somol.material_layer_param_write_boundary.v1"));
				OutStructured->SetStringField(TEXT("parameter_association"), TEXT("LayerParameter"));
				OutStructured->SetArrayField(TEXT("supported_value_types"), SupportedValueTypes);
				OutStructured->SetStringField(TEXT("unsupported_value_types"), TEXT("arbitrary struct/object overrides"));
				OutStructured->SetBoolField(TEXT("parent_material_layer_stack_write_supported"), false);
				OutStructured->SetStringField(TEXT("boundary_note"), TEXT("This tool writes verified MaterialInstanceConstant layer parameter overrides only; it does not push/pop the parent UMaterial layer stack."));

				const FScopedTransaction Transaction(LOCTEXT("MatSetLayerParam", "SOMOLMCP Set Material Layer Param"));
				MIC->Modify();

				// TODO(P1-3): Layer-aware parameter overrides require an FMaterialParameterInfo with
				// Association = LayerParameter / BlendParameter and Index = LayerIndex. The exact
				// SetScalarParameterValueEditorOnly etc. signatures vary across UE5 minor versions.
				// Best-effort path: try common scalar/vector/texture setters; otherwise fall back to
				// generic property write.
				FString OldStr = TEXT("(unknown)");
				FString NewStr = JsonValueToImport(ValueField);

				bool bWrote = false;
				bool bVerified = false;
				const FName ParamFName(*ParamName);

				// Try as scalar first
				if (ValueField->Type == EJson::Number)
				{
					const float NewVal = static_cast<float>(ValueField->AsNumber());
					float CurVal = 0.0f;
					FMaterialParameterInfo Info(ParamFName);
					Info.Association = LayerParameter;
					Info.Index = LayerIndex;
					OutStructured->SetNumberField(TEXT("parameter_index"), LayerIndex);
					OutStructured->SetStringField(TEXT("value_type"), TEXT("scalar"));
					if (MIC->GetScalarParameterValue(Info, CurVal, /*bOveriddenOnly*/ false))
					{
						OldStr = FString::SanitizeFloat(CurVal);
					}
					MIC->SetScalarParameterValueEditorOnly(Info, NewVal);
					bWrote = true;
					float VerifyVal = 0.0f;
					bVerified = MIC->GetScalarParameterValue(Info, VerifyVal, false) && FMath::IsNearlyEqual(VerifyVal, NewVal);
				}
				else if (ValueField->Type == EJson::Boolean)
				{
					FMaterialParameterInfo Info(ParamFName);
					Info.Association = LayerParameter;
					Info.Index = LayerIndex;
					OutStructured->SetNumberField(TEXT("parameter_index"), LayerIndex);
					OutStructured->SetStringField(TEXT("value_type"), TEXT("static_switch"));
					const bool RequestedValue = ValueField->AsBool();
					const FStaticParameterSet BeforeParameters = MIC->GetStaticParameters();
					for (const FStaticSwitchParameter& Existing : BeforeParameters.StaticSwitchParameters)
					{
						if (Existing.ParameterInfo == Info)
						{
							OldStr = Existing.Value ? TEXT("true") : TEXT("false");
							break;
						}
					}
					MIC->SetStaticSwitchParameterValueEditorOnly(Info, RequestedValue);
					bWrote = true;
					const FStaticParameterSet AfterParameters = MIC->GetStaticParameters();
					for (const FStaticSwitchParameter& Existing : AfterParameters.StaticSwitchParameters)
					{
						if (Existing.ParameterInfo == Info && Existing.bOverride && Existing.Value == RequestedValue)
						{
							bVerified = true;
							break;
						}
					}
				}
				else if (ValueField->Type == EJson::String)
				{
					// Could be a texture path or a color string.
					const FString S = ValueField->AsString();
					if (S.StartsWith(TEXT("/Game/")) || S.StartsWith(TEXT("/Engine/")))
					{
						UTexture* Tex = Cast<UTexture>(Ctx.Services.LoadAsset(S, OutError));
						if (!Tex)
						{
							SololmcpError::InvalidPath(OutStructured, S);
							OutError = FString::Printf(TEXT("value asset is not a UTexture: %s"), *S);
							return false;
						}
						FMaterialParameterInfo Info(ParamFName);
						Info.Association = LayerParameter;
						Info.Index = LayerIndex;
						OutStructured->SetNumberField(TEXT("parameter_index"), LayerIndex);
						OutStructured->SetStringField(TEXT("value_type"), TEXT("texture"));
						UTexture* CurTex = nullptr;
						if (MIC->GetTextureParameterValue(Info, CurTex, false))
						{
							OldStr = CurTex ? CurTex->GetPathName() : TEXT("None");
						}
						MIC->SetTextureParameterValueEditorOnly(Info, Tex);
						bWrote = true;
						UTexture* VerifyTex = nullptr;
						bVerified = MIC->GetTextureParameterValue(Info, VerifyTex, false) && VerifyTex == Tex;
					}
					else
					{
						OutError = FString::Printf(TEXT("Unsupported string value '%s' for layer param (use number/asset path)."), *S);
						SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("value"), OutError);
						return false;
					}
				}
				else if (ValueField->Type == EJson::Array)
				{
					// Vector/Color: 3 or 4 numbers
					const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
					if (Arr.Num() < 3 || Arr.Num() > 4)
					{
						OutError = TEXT("Vector value must have 3 or 4 components.");
						SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("value"), OutError);
						return false;
					}
					FLinearColor C(0, 0, 0, 1);
					C.R = static_cast<float>(Arr[0]->AsNumber());
					C.G = static_cast<float>(Arr[1]->AsNumber());
					C.B = static_cast<float>(Arr[2]->AsNumber());
					if (Arr.Num() == 4) C.A = static_cast<float>(Arr[3]->AsNumber());
					FMaterialParameterInfo Info(ParamFName);
					Info.Association = LayerParameter;
					Info.Index = LayerIndex;
					OutStructured->SetNumberField(TEXT("parameter_index"), LayerIndex);
					OutStructured->SetStringField(TEXT("value_type"), TEXT("vector"));
					FLinearColor CurC;
					if (MIC->GetVectorParameterValue(Info, CurC, false))
					{
						OldStr = CurC.ToString();
					}
					MIC->SetVectorParameterValueEditorOnly(Info, C);
					bWrote = true;
					FLinearColor VerifyC;
					bVerified = MIC->GetVectorParameterValue(Info, VerifyC, false) && VerifyC.Equals(C, KINDA_SMALL_NUMBER);
				}
				else
				{
					OutError = TEXT("Unsupported value type for layer parameter.");
					SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("value"), OutError);
					return false;
				}

				if (bWrote)
				{
					MIC->PostEditChange();
					MIC->MarkPackageDirty();
					if (MIC->GetOutermost()) { MIC->GetOutermost()->MarkPackageDirty(); }
					SololmcpWriteFlush::EnsureFlushed(MIC);
				}

				if (!bVerified)
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("parameter_name"),
						TEXT("Layer parameter setter did not read back the requested value; no successful override can be claimed."));
					OutStructured->SetNumberField(TEXT("layer"), LayerIndex);
					OutStructured->SetStringField(TEXT("param"), ParamName);
					OutStructured->SetStringField(TEXT("attempted_new"), NewStr);
					OutStructured->SetBoolField(TEXT("applied_verified"), false);
					OutError = FString::Printf(TEXT("Layer parameter '%s' on layer %d did not verify after write."), *ParamName, LayerIndex);
					return false;
				}
				FString SaveErr;
				if (!Ctx.Services.SaveAsset(MliPath, false, SaveErr))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("mli_path"), SaveErr);
					OutError = FString::Printf(TEXT("Failed to save MaterialInstanceConstant after layer param write: %s"), *SaveErr);
					return false;
				}
				FString ReloadErr;
				UObject* Reloaded = Ctx.Services.LoadAsset(MliPath, ReloadErr);
				if (!Reloaded || !Reloaded->IsA(MIC->GetClass()))
				{
					SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("mli_path"),
						TEXT("MaterialInstanceConstant failed reload/class validation after save."));
					OutError = ReloadErr;
					return false;
				}

				OutStructured->SetNumberField(TEXT("layer"), LayerIndex);
				OutStructured->SetStringField(TEXT("param"), ParamName);
				OutStructured->SetStringField(TEXT("old"), OldStr);
				OutStructured->SetStringField(TEXT("new"), NewStr);
				OutStructured->SetBoolField(TEXT("applied_verified"), true);
				OutStructured->SetBoolField(TEXT("saved"), true);
				OutSummary = FString::Printf(TEXT("Set layer[%d].%s on '%s'"), LayerIndex, *ParamName, *MliPath);
				return true;
			}
		, nullptr
		, 0
		});

		// ---------------------------------------------------------------------
		// 5) material_layer_inspect
		// ---------------------------------------------------------------------
		Registry.Register({
			TEXT("material_layer_inspect"),
			TEXT("Inspect the layer stack on a UMaterial, layer asset (UMaterialFunctionMaterialLayer), or MaterialInstance."),
			SB::Object({
				{TEXT("asset_path"), SB::String(TEXT("UMaterial / UMaterialFunctionMaterialLayer / UMaterialInstanceConstant"))}
			}, {TEXT("asset_path")}),
			[](const FSololmcpToolExecutionContext& Ctx, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
			{
				FString AssetPath;
				if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
				{
					SololmcpError::MissingParam(OutStructured, TEXT("asset_path"));
					OutError = TEXT("Missing asset_path.");
					return false;
				}

				UObject* Asset = Ctx.Services.LoadAsset(AssetPath, OutError);
				if (!Asset)
				{
					SololmcpError::InvalidPath(OutStructured, AssetPath);
					return false;
				}

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);

				if (UMaterial* Material = Cast<UMaterial>(Asset))
				{
					OutStructured->SetStringField(TEXT("type"), TEXT("material"));
					OutStructured->SetBoolField(TEXT("use_material_attributes"), Material->bUseMaterialAttributes);
					FMaterialLayersFunctions Layers;
					const bool bHasLayers = Material->GetMaterialLayers(Layers);
					LayerStackToJson(Layers, OutStructured);
					OutStructured->SetBoolField(TEXT("has_layer_stack"), bHasLayers);
					OutStructured->SetBoolField(TEXT("layer_readback_supported"), true);
					OutStructured->SetStringField(TEXT("layer_stack_boundary"), TEXT("UMaterial layer stack is read from the public FMaterialLayersFunctions API and can be authored by material_add_layer_stack."));
					OutSummary = FString::Printf(TEXT("Inspected material '%s': %d layers, %d blends."),
						*AssetPath, Layers.Layers.Num(), Layers.Blends.Num());
					return true;
				}
				if (UMaterialFunctionMaterialLayer* LayerFn = Cast<UMaterialFunctionMaterialLayer>(Asset))
				{
					OutStructured->SetStringField(TEXT("type"), TEXT("layer"));
					OutStructured->SetNumberField(TEXT("layers_count"), 1);
					TArray<TSharedPtr<FJsonValue>> Arr;
					TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
					One->SetNumberField(TEXT("index"), 0);
					One->SetStringField(TEXT("name"), LayerFn->GetName());
					One->SetStringField(TEXT("blend"), TEXT(""));
					One->SetNumberField(TEXT("parameter_count"), 0); // TODO(P1-3): walk function inputs
					Arr.Add(MakeShared<FJsonValueObject>(One));
					OutStructured->SetArrayField(TEXT("layers"), Arr);
					OutSummary = FString::Printf(TEXT("Inspected layer asset '%s'"), *AssetPath);
					return true;
				}
				if (UMaterialFunctionMaterialLayerBlend* BlendFn = Cast<UMaterialFunctionMaterialLayerBlend>(Asset))
				{
					OutStructured->SetStringField(TEXT("type"), TEXT("blend"));
					OutStructured->SetNumberField(TEXT("layers_count"), 0);
					OutSummary = FString::Printf(TEXT("Inspected blend asset '%s'"), *AssetPath);
					return true;
				}
				if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Asset))
				{
					OutStructured->SetStringField(TEXT("type"), TEXT("material_instance"));
					// TODO(P1-3): MI exposes layer stack via StaticParameters.MaterialLayers in UE5.x.
					FMaterialLayersFunctions Layers;
					Layers.Layers = MIC->GetStaticParameters().MaterialLayers.Layers;
					Layers.Blends = MIC->GetStaticParameters().MaterialLayers.Blends;
					LayerStackToJson(Layers, OutStructured);
					OutStructured->SetBoolField(TEXT("layer_readback_supported"), true);
					OutStructured->SetStringField(TEXT("layer_param_write_boundary"), TEXT("material_set_layer_param supports verified scalar/vector/texture overrides through FMaterialParameterInfo{Association=LayerParameter, Index=layer_index}; static switch/object overrides are not claimed."));
					OutSummary = FString::Printf(TEXT("Inspected material instance '%s'"), *AssetPath);
					return true;
				}

				OutStructured->SetStringField(TEXT("type"), TEXT("unknown"));
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("asset_path"),
					TEXT("Asset is not Material/Layer/Blend/MaterialInstance."));
				OutError = FString::Printf(TEXT("Unsupported asset class for layer inspect: %s"), *Asset->GetClass()->GetPathName());
				return false;
			}
		, nullptr
		, 5
		});
	}
}

#undef LOCTEXT_NAMESPACE
