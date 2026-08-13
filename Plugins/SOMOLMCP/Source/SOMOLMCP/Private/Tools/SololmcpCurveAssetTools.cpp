// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP — Standalone Curve Asset editing tools (UCurveFloat / UCurveLinearColor)
//
// Tools registered (all curve_* prefix):
//   curve_create             — Create a new UCurveFloat or UCurveLinearColor asset (no keys).
//   curve_add_key            — Add a key (time + value, or RGBA channels) with optional interp.
//   curve_remove_key         — Remove a key by index OR by nearest time (within +/- 0.001s).
//   curve_set_interpolation  — Set interp mode on one key (key_index) or ALL keys when omitted.
//   curve_inspect            — Return curve metadata + all keys (with per-channel values for color).
//
// UCurveFloat layout: single FRichCurve at Curve->FloatCurve.
// UCurveLinearColor layout: 4 FRichCurves at Curve->FloatCurves[0..3] (R, G, B, A).

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"

#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/RichCurve.h"
#include "Curves/KeyHandle.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::SOMOLMCP
{

// ============================================================================
// Helpers
// ============================================================================

namespace CurveAssetToolsImpl
{
	/** Map a string ("linear" | "cubic" | "constant") to ERichCurveInterpMode. Unknown → RCIM_Linear. */
	static ERichCurveInterpMode ParseInterpMode(const FString& InterpStr)
	{
		if (InterpStr.Equals(TEXT("cubic"), ESearchCase::IgnoreCase))    return RCIM_Cubic;
		if (InterpStr.Equals(TEXT("constant"), ESearchCase::IgnoreCase)) return RCIM_Constant;
		// default
		return RCIM_Linear;
	}

	/** Reverse mapping for inspect output. */
	static FString InterpModeToString(ERichCurveInterpMode Mode)
	{
		switch (Mode)
		{
			case RCIM_Cubic:    return TEXT("cubic");
			case RCIM_Constant: return TEXT("constant");
			case RCIM_Linear:
			default:            return TEXT("linear");
		}
	}

	/** Load a curve asset and classify it. Returns the loaded UObject* (Float or LinearColor). */
	static UObject* LoadCurveAsset(const FSololmcpToolExecutionContext& Context, const FString& AssetPath,
		UCurveFloat*& OutFloat, UCurveLinearColor*& OutColor, FString& OutError)
	{
		OutFloat = nullptr;
		OutColor = nullptr;

		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing asset_path.");
			return nullptr;
		}

		UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
		if (!Asset)
		{
			return nullptr;
		}

		// Try the more-derived type first (UCurveLinearColor inherits from UCurveBase, not UCurveFloat).
		if (UCurveLinearColor* AsColor = Cast<UCurveLinearColor>(Asset))
		{
			OutColor = AsColor;
			return Asset;
		}
		if (UCurveFloat* AsFloat = Cast<UCurveFloat>(Asset))
		{
			OutFloat = AsFloat;
			return Asset;
		}

		OutError = FString::Printf(TEXT("Asset '%s' is not a UCurveFloat or UCurveLinearColor."), *AssetPath);
		return nullptr;
	}

	/** Find the key handle in a FRichCurve whose time is closest to TargetTime (within Tolerance). */
	static bool FindKeyHandleByTime(FRichCurve& Curve, float TargetTime, float Tolerance, FKeyHandle& OutHandle, int32& OutIndex, float& OutTime)
	{
		float BestDelta = TNumericLimits<float>::Max();
		bool bFound = false;
		int32 Index = 0;
		int32 BestIndex = -1;
		FKeyHandle BestHandle;
		float BestTime = 0.0f;

		for (auto It = Curve.GetKeyHandleIterator(); It; ++It)
		{
			const FKeyHandle Handle = *It;
			const float KeyTime = Curve.GetKeyTime(Handle);
			const float Delta = FMath::Abs(KeyTime - TargetTime);
			if (Delta < BestDelta)
			{
				BestDelta = Delta;
				BestHandle = Handle;
				BestIndex = Index;
				BestTime = KeyTime;
				bFound = true;
			}
			++Index;
		}

		if (!bFound || BestDelta > Tolerance)
		{
			return false;
		}

		OutHandle = BestHandle;
		OutIndex = BestIndex;
		OutTime = BestTime;
		return true;
	}

	/** Save the curve asset via UEditorAssetLibrary. */
	static bool SaveCurveAsset(UObject* CurveAsset)
	{
		if (!CurveAsset)
		{
			return false;
		}
		CurveAsset->MarkPackageDirty();
		return UEditorAssetLibrary::SaveAsset(CurveAsset->GetPathName(), /*bOnlyIfIsDirty=*/false);
	}
}

// ============================================================================
// Tool: curve_create
// ============================================================================

static void RegisterCurveCreate(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("curve_create"),
		TEXT("Create a new UCurveFloat or UCurveLinearColor asset at the given path. The new curve has zero keys."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path, e.g. /Game/UI/C_HitFlash"))},
			{TEXT("curve_type"), FSololmcpSchemaBuilder::String(TEXT("'float' (UCurveFloat) or 'linear_color' (UCurveLinearColor)"), {TEXT("float"), TEXT("linear_color")})}
		}, {TEXT("asset_path"), TEXT("curve_type")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			FString CurveType;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("curve_type"), CurveType) || CurveType.IsEmpty())
			{
				OutError = TEXT("Missing curve_type ('float' or 'linear_color').");
				return false;
			}

			if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
			{
				OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
				return false;
			}

			// Strip object suffix if user passed Foo.Foo
			FString PackagePath = AssetPath;
			int32 DotIdx = INDEX_NONE;
			if (PackagePath.FindChar('.', DotIdx))
			{
				PackagePath = PackagePath.Left(DotIdx);
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);

			UPackage* Package = CreatePackage(*PackagePath);
			if (!Package)
			{
				OutError = TEXT("Failed to create package.");
				return false;
			}

			UObject* CreatedAsset = nullptr;
			if (CurveType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
			{
				UCurveFloat* Curve = NewObject<UCurveFloat>(Package, *AssetName, RF_Public | RF_Standalone);
				if (!Curve)
				{
					OutError = TEXT("Failed to create UCurveFloat.");
					return false;
				}
				CreatedAsset = Curve;
			}
			else if (CurveType.Equals(TEXT("linear_color"), ESearchCase::IgnoreCase))
			{
				UCurveLinearColor* Curve = NewObject<UCurveLinearColor>(Package, *AssetName, RF_Public | RF_Standalone);
				if (!Curve)
				{
					OutError = TEXT("Failed to create UCurveLinearColor.");
					return false;
				}
				CreatedAsset = Curve;
			}
			else
			{
				OutError = FString::Printf(TEXT("Unknown curve_type '%s' (expected 'float' or 'linear_color')."), *CurveType);
				return false;
			}

			CreatedAsset->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(CreatedAsset);
			if (!CurveAssetToolsImpl::SaveCurveAsset(CreatedAsset))
			{
				OutError = FString::Printf(TEXT("Failed to save curve asset '%s'."), *CreatedAsset->GetPathName());
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), CreatedAsset->GetPathName());
			OutStructured->SetStringField(TEXT("curve_type"), CurveType.ToLower());
			OutStructured->SetNumberField(TEXT("key_count"), 0);
			OutSummary = FString::Printf(TEXT("Created %s curve asset '%s'"), *CurveType, *CreatedAsset->GetPathName());
			return true;
		}
	});
}

// ============================================================================
// Tool: curve_add_key
// ============================================================================

static void RegisterCurveAddKey(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("curve_add_key"),
		TEXT("Add a key to a curve asset. For UCurveFloat, supply 'value'. For UCurveLinearColor, supply any subset of 'r','g','b','a' (missing channels are skipped). Optional 'interp' is 'linear' (default), 'cubic', or 'constant'."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Curve asset path"))},
			{TEXT("time"), FSololmcpSchemaBuilder::Number(TEXT("Time of the key (seconds)"))},
			{TEXT("value"), FSololmcpSchemaBuilder::Number(TEXT("Value (UCurveFloat only)"))},
			{TEXT("r"), FSololmcpSchemaBuilder::Number(TEXT("Red channel (UCurveLinearColor)"))},
			{TEXT("g"), FSololmcpSchemaBuilder::Number(TEXT("Green channel (UCurveLinearColor)"))},
			{TEXT("b"), FSololmcpSchemaBuilder::Number(TEXT("Blue channel (UCurveLinearColor)"))},
			{TEXT("a"), FSololmcpSchemaBuilder::Number(TEXT("Alpha channel (UCurveLinearColor)"))},
			{TEXT("interp"), FSololmcpSchemaBuilder::String(TEXT("'linear' (default), 'cubic', or 'constant'"), {TEXT("linear"), TEXT("cubic"), TEXT("constant")})}
		}, {TEXT("asset_path"), TEXT("time")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			double Time = 0.0;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetNumberField(TEXT("time"), Time))
			{
				OutError = TEXT("Missing 'time'.");
				return false;
			}

			UCurveFloat* FloatCurveAsset = nullptr;
			UCurveLinearColor* ColorCurveAsset = nullptr;
			UObject* Asset = CurveAssetToolsImpl::LoadCurveAsset(Context, AssetPath, FloatCurveAsset, ColorCurveAsset, OutError);
			if (!Asset)
			{
				return false;
			}

			FString InterpStr = TEXT("linear");
			Arguments->TryGetStringField(TEXT("interp"), InterpStr);
			const ERichCurveInterpMode InterpMode = CurveAssetToolsImpl::ParseInterpMode(InterpStr);

			const float TimeF = static_cast<float>(Time);

			if (FloatCurveAsset)
			{
				double Value = 0.0;
				if (!Arguments->TryGetNumberField(TEXT("value"), Value))
				{
					OutError = TEXT("UCurveFloat requires 'value'.");
					return false;
				}
				const float ValueF = static_cast<float>(Value);

				FRichCurve& Curve = FloatCurveAsset->FloatCurve;
				const FKeyHandle Handle = Curve.AddKey(TimeF, ValueF);
				Curve.SetKeyInterpMode(Handle, InterpMode);

				// Compute the linear index of the new key by enumerating handles.
				int32 KeyIndex = -1;
				int32 Idx = 0;
				for (auto It = Curve.GetKeyHandleIterator(); It; ++It)
				{
					if (*It == Handle) { KeyIndex = Idx; break; }
					++Idx;
				}

				if (!CurveAssetToolsImpl::SaveCurveAsset(FloatCurveAsset))
				{
					OutError = FString::Printf(TEXT("Failed to save curve asset '%s'."), *FloatCurveAsset->GetPathName());
					return false;
				}

				OutStructured->SetStringField(TEXT("asset_path"), FloatCurveAsset->GetPathName());
				OutStructured->SetStringField(TEXT("curve_type"), TEXT("float"));
				OutStructured->SetNumberField(TEXT("key_index"), KeyIndex);
				OutStructured->SetNumberField(TEXT("time"), TimeF);
				OutStructured->SetNumberField(TEXT("value"), ValueF);
				OutStructured->SetStringField(TEXT("interp"), CurveAssetToolsImpl::InterpModeToString(InterpMode));
				OutSummary = FString::Printf(TEXT("Added key #%d (t=%.4f, v=%.4f, %s) to '%s'"),
					KeyIndex, TimeF, ValueF, *CurveAssetToolsImpl::InterpModeToString(InterpMode), *FloatCurveAsset->GetPathName());
				return true;
			}

			// UCurveLinearColor — write to whichever channels were supplied.
			static const TCHAR* ChannelNames[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
			TSharedPtr<FJsonObject> ChannelResult = MakeShared<FJsonObject>();
			TArray<FString> WrittenChannels;
			int32 LastKeyIndex = -1;

			for (int32 Ch = 0; Ch < 4; ++Ch)
			{
				double ChannelVal = 0.0;
				if (!Arguments->TryGetNumberField(ChannelNames[Ch], ChannelVal))
				{
					continue; // skip missing channel
				}
				const float ValF = static_cast<float>(ChannelVal);
				FRichCurve& Curve = ColorCurveAsset->FloatCurves[Ch];
				const FKeyHandle Handle = Curve.AddKey(TimeF, ValF);
				Curve.SetKeyInterpMode(Handle, InterpMode);

				int32 KeyIndex = -1;
				int32 Idx = 0;
				for (auto It = Curve.GetKeyHandleIterator(); It; ++It)
				{
					if (*It == Handle) { KeyIndex = Idx; break; }
					++Idx;
				}
				LastKeyIndex = KeyIndex;

				ChannelResult->SetNumberField(ChannelNames[Ch], ValF);
				WrittenChannels.Add(ChannelNames[Ch]);
			}

			if (WrittenChannels.Num() == 0)
			{
				OutError = TEXT("UCurveLinearColor requires at least one of 'r','g','b','a'.");
				return false;
			}

			if (!CurveAssetToolsImpl::SaveCurveAsset(ColorCurveAsset))
			{
				OutError = FString::Printf(TEXT("Failed to save curve asset '%s'."), *ColorCurveAsset->GetPathName());
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), ColorCurveAsset->GetPathName());
			OutStructured->SetStringField(TEXT("curve_type"), TEXT("linear_color"));
			OutStructured->SetNumberField(TEXT("key_index"), LastKeyIndex);
			OutStructured->SetNumberField(TEXT("time"), TimeF);
			OutStructured->SetObjectField(TEXT("value"), ChannelResult);
			OutStructured->SetStringField(TEXT("interp"), CurveAssetToolsImpl::InterpModeToString(InterpMode));
			TArray<TSharedPtr<FJsonValue>> ChannelsJson;
			for (const FString& Name : WrittenChannels)
			{
				ChannelsJson.Add(MakeShared<FJsonValueString>(Name));
			}
			OutStructured->SetArrayField(TEXT("channels_written"), ChannelsJson);
			OutSummary = FString::Printf(TEXT("Added key (t=%.4f, %s, %d channel(s)) to '%s'"),
				TimeF, *CurveAssetToolsImpl::InterpModeToString(InterpMode), WrittenChannels.Num(), *ColorCurveAsset->GetPathName());
			return true;
		}
	});
}

// ============================================================================
// Tool: curve_remove_key
// ============================================================================

static void RegisterCurveRemoveKey(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("curve_remove_key"),
		TEXT("Remove a key from a curve asset by 'key_index' (preferred) or by 'time' (nearest key within +/- 0.001s). For UCurveLinearColor, the remove targets all four channels at the matched index/time."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Curve asset path"))},
			{TEXT("key_index"), FSololmcpSchemaBuilder::Integer(TEXT("Index of the key to remove"))},
			{TEXT("time"), FSololmcpSchemaBuilder::Number(TEXT("Time of the key (used if key_index is omitted)"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			UCurveFloat* FloatCurveAsset = nullptr;
			UCurveLinearColor* ColorCurveAsset = nullptr;
			UObject* Asset = CurveAssetToolsImpl::LoadCurveAsset(Context, AssetPath, FloatCurveAsset, ColorCurveAsset, OutError);
			if (!Asset)
			{
				return false;
			}

			const bool bHasIndex = Arguments->HasField(TEXT("key_index"));
			const bool bHasTime  = Arguments->HasField(TEXT("time"));
			if (!bHasIndex && !bHasTime)
			{
				OutError = TEXT("Provide 'key_index' or 'time'.");
				return false;
			}

			// Collect the curves to mutate. For float, just one. For linear_color, all four channels.
			TArray<FRichCurve*> Curves;
			if (FloatCurveAsset)
			{
				Curves.Add(&FloatCurveAsset->FloatCurve);
			}
			else
			{
				for (int32 Ch = 0; Ch < 4; ++Ch)
				{
					Curves.Add(&ColorCurveAsset->FloatCurves[Ch]);
				}
			}

			int32 RemovedIndex = -1;
			float RemovedTime = 0.0f;
			int32 RemovedCount = 0;

			if (bHasIndex)
			{
				const int32 KeyIndex = Arguments->GetIntegerField(TEXT("key_index"));
				for (FRichCurve* Curve : Curves)
				{
					int32 Idx = 0;
					FKeyHandle TargetHandle;
					bool bFound = false;
					float FoundTime = 0.0f;
					for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
					{
						if (Idx == KeyIndex)
						{
							TargetHandle = *It;
							FoundTime = Curve->GetKeyTime(TargetHandle);
							bFound = true;
							break;
						}
						++Idx;
					}
					if (bFound)
					{
						Curve->DeleteKey(TargetHandle);
						RemovedIndex = KeyIndex;
						RemovedTime = FoundTime;
						++RemovedCount;
					}
				}
				if (RemovedCount == 0)
				{
					OutError = FString::Printf(TEXT("No key at index %d."), KeyIndex);
					return false;
				}
			}
			else
			{
				const float TargetTime = static_cast<float>(Arguments->GetNumberField(TEXT("time")));
				constexpr float Tolerance = 0.001f;

				for (FRichCurve* Curve : Curves)
				{
					FKeyHandle Handle;
					int32 Idx = -1;
					float FoundTime = 0.0f;
					if (CurveAssetToolsImpl::FindKeyHandleByTime(*Curve, TargetTime, Tolerance, Handle, Idx, FoundTime))
					{
						Curve->DeleteKey(Handle);
						RemovedIndex = Idx;
						RemovedTime = FoundTime;
						++RemovedCount;
					}
				}
				if (RemovedCount == 0)
				{
					OutError = FString::Printf(TEXT("No key within +/- %.4f of time %.4f."), Tolerance, TargetTime);
					return false;
				}
			}

			if (!CurveAssetToolsImpl::SaveCurveAsset(Asset))
			{
				OutError = FString::Printf(TEXT("Failed to save curve asset '%s'."), *Asset->GetPathName());
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetStringField(TEXT("curve_type"), FloatCurveAsset ? TEXT("float") : TEXT("linear_color"));
			OutStructured->SetNumberField(TEXT("removed_index"), RemovedIndex);
			OutStructured->SetNumberField(TEXT("time"), RemovedTime);
			OutStructured->SetNumberField(TEXT("removed_channel_count"), RemovedCount);
			OutSummary = FString::Printf(TEXT("Removed key (index=%d, t=%.4f) from '%s' [%d channel(s)]"),
				RemovedIndex, RemovedTime, *Asset->GetPathName(), RemovedCount);
			return true;
		}
	});
}

// ============================================================================
// Tool: curve_set_interpolation
// ============================================================================

static void RegisterCurveSetInterpolation(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("curve_set_interpolation"),
		TEXT("Set the interp mode on a single key (key_index) or on ALL keys (omit key_index). For UCurveLinearColor, all four channels are updated."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Curve asset path"))},
			{TEXT("key_index"), FSololmcpSchemaBuilder::Integer(TEXT("Optional: target a specific key. Omit to update ALL keys."))},
			{TEXT("interp"), FSololmcpSchemaBuilder::String(TEXT("'linear', 'cubic', or 'constant'"), {TEXT("linear"), TEXT("cubic"), TEXT("constant")})}
		}, {TEXT("asset_path"), TEXT("interp")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			FString InterpStr;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("interp"), InterpStr) || InterpStr.IsEmpty())
			{
				OutError = TEXT("Missing 'interp'.");
				return false;
			}
			const ERichCurveInterpMode InterpMode = CurveAssetToolsImpl::ParseInterpMode(InterpStr);

			UCurveFloat* FloatCurveAsset = nullptr;
			UCurveLinearColor* ColorCurveAsset = nullptr;
			UObject* Asset = CurveAssetToolsImpl::LoadCurveAsset(Context, AssetPath, FloatCurveAsset, ColorCurveAsset, OutError);
			if (!Asset)
			{
				return false;
			}

			TArray<FRichCurve*> Curves;
			if (FloatCurveAsset)
			{
				Curves.Add(&FloatCurveAsset->FloatCurve);
			}
			else
			{
				for (int32 Ch = 0; Ch < 4; ++Ch)
				{
					Curves.Add(&ColorCurveAsset->FloatCurves[Ch]);
				}
			}

			const bool bHasIndex = Arguments->HasField(TEXT("key_index"));
			int32 UpdatedCount = 0;

			if (bHasIndex)
			{
				const int32 KeyIndex = Arguments->GetIntegerField(TEXT("key_index"));
				for (FRichCurve* Curve : Curves)
				{
					int32 Idx = 0;
					for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
					{
						if (Idx == KeyIndex)
						{
							Curve->SetKeyInterpMode(*It, InterpMode);
							++UpdatedCount;
							break;
						}
						++Idx;
					}
				}
				if (UpdatedCount == 0)
				{
					OutError = FString::Printf(TEXT("No key at index %d."), KeyIndex);
					return false;
				}
			}
			else
			{
				for (FRichCurve* Curve : Curves)
				{
					for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
					{
						Curve->SetKeyInterpMode(*It, InterpMode);
						++UpdatedCount;
					}
				}
			}

			if (!CurveAssetToolsImpl::SaveCurveAsset(Asset))
			{
				OutError = FString::Printf(TEXT("Failed to save curve asset '%s'."), *Asset->GetPathName());
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			OutStructured->SetStringField(TEXT("curve_type"), FloatCurveAsset ? TEXT("float") : TEXT("linear_color"));
			OutStructured->SetStringField(TEXT("interp"), CurveAssetToolsImpl::InterpModeToString(InterpMode));
			OutStructured->SetNumberField(TEXT("updated_count"), UpdatedCount);
			OutSummary = FString::Printf(TEXT("Set interp '%s' on %d key(s) of '%s'"),
				*CurveAssetToolsImpl::InterpModeToString(InterpMode), UpdatedCount, *Asset->GetPathName());
			return true;
		}
	});
}

// ============================================================================
// Tool: curve_inspect
// ============================================================================

static void RegisterCurveInspect(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("curve_inspect"),
		TEXT("Inspect a curve asset: returns curve_type, key_count, and the list of keys. For UCurveLinearColor, each key's 'value' is an object {r,g,b,a} merged across channels by index."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Curve asset path"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
			{
				OutError = TEXT("Missing asset_path.");
				return false;
			}

			UCurveFloat* FloatCurveAsset = nullptr;
			UCurveLinearColor* ColorCurveAsset = nullptr;
			UObject* Asset = CurveAssetToolsImpl::LoadCurveAsset(Context, AssetPath, FloatCurveAsset, ColorCurveAsset, OutError);
			if (!Asset)
			{
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> KeysJson;
			int32 KeyCount = 0;

			if (FloatCurveAsset)
			{
				FRichCurve& Curve = FloatCurveAsset->FloatCurve;
				for (auto It = Curve.GetKeyHandleIterator(); It; ++It)
				{
					const FKeyHandle Handle = *It;
					const float KeyTime = Curve.GetKeyTime(Handle);
					const float KeyVal = Curve.GetKeyValue(Handle);
					const ERichCurveInterpMode Mode = Curve.GetKeyInterpMode(Handle);

					TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
					KeyObj->SetNumberField(TEXT("time"), KeyTime);
					KeyObj->SetNumberField(TEXT("value"), KeyVal);
					KeyObj->SetStringField(TEXT("interp"), CurveAssetToolsImpl::InterpModeToString(Mode));
					KeysJson.Add(MakeShared<FJsonValueObject>(KeyObj));
					++KeyCount;
				}

				OutStructured->SetStringField(TEXT("asset_path"), FloatCurveAsset->GetPathName());
				OutStructured->SetStringField(TEXT("curve_type"), TEXT("float"));
				OutStructured->SetNumberField(TEXT("key_count"), KeyCount);
				OutStructured->SetArrayField(TEXT("keys"), KeysJson);
				OutSummary = FString::Printf(TEXT("UCurveFloat '%s': %d key(s)"), *FloatCurveAsset->GetPathName(), KeyCount);
				return true;
			}

			// UCurveLinearColor — merge channels by index. We use the channel with the most keys
			// as the index master, then for each index, look up the matching value in each channel
			// (by index). Channels with shorter key arrays contribute null for missing indices.
			static const TCHAR* ChannelNames[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };

			int32 MaxKeys = 0;
			for (int32 Ch = 0; Ch < 4; ++Ch)
			{
				MaxKeys = FMath::Max(MaxKeys, ColorCurveAsset->FloatCurves[Ch].GetNumKeys());
			}

			// Pre-collect per-channel key lists for index access.
			TArray<TArray<FKeyHandle>> ChannelHandles;
			ChannelHandles.SetNum(4);
			for (int32 Ch = 0; Ch < 4; ++Ch)
			{
				FRichCurve& Curve = ColorCurveAsset->FloatCurves[Ch];
				for (auto It = Curve.GetKeyHandleIterator(); It; ++It)
				{
					ChannelHandles[Ch].Add(*It);
				}
			}

			for (int32 i = 0; i < MaxKeys; ++i)
			{
				// Use the first channel with a key at this index to define the time + interp.
				float KeyTime = 0.0f;
				ERichCurveInterpMode Mode = RCIM_Linear;
				bool bHaveTime = false;
				TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();

				for (int32 Ch = 0; Ch < 4; ++Ch)
				{
					if (i < ChannelHandles[Ch].Num())
					{
						FRichCurve& Curve = ColorCurveAsset->FloatCurves[Ch];
						const FKeyHandle H = ChannelHandles[Ch][i];
						const float T = Curve.GetKeyTime(H);
						const float V = Curve.GetKeyValue(H);
						if (!bHaveTime)
						{
							KeyTime = T;
							Mode = Curve.GetKeyInterpMode(H);
							bHaveTime = true;
						}
						ValueObj->SetNumberField(ChannelNames[Ch], V);
					}
				}

				TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
				KeyObj->SetNumberField(TEXT("time"), KeyTime);
				KeyObj->SetObjectField(TEXT("value"), ValueObj);
				KeyObj->SetStringField(TEXT("interp"), CurveAssetToolsImpl::InterpModeToString(Mode));
				KeysJson.Add(MakeShared<FJsonValueObject>(KeyObj));
			}
			KeyCount = MaxKeys;

			// Per-channel key counts for diagnostic clarity.
			TSharedPtr<FJsonObject> PerChannelCounts = MakeShared<FJsonObject>();
			for (int32 Ch = 0; Ch < 4; ++Ch)
			{
				PerChannelCounts->SetNumberField(ChannelNames[Ch], ColorCurveAsset->FloatCurves[Ch].GetNumKeys());
			}

			OutStructured->SetStringField(TEXT("asset_path"), ColorCurveAsset->GetPathName());
			OutStructured->SetStringField(TEXT("curve_type"), TEXT("linear_color"));
			OutStructured->SetNumberField(TEXT("key_count"), KeyCount);
			OutStructured->SetObjectField(TEXT("key_count_per_channel"), PerChannelCounts);
			OutStructured->SetArrayField(TEXT("keys"), KeysJson);
			OutSummary = FString::Printf(TEXT("UCurveLinearColor '%s': %d merged key(s)"), *ColorCurveAsset->GetPathName(), KeyCount);
			return true;
		}
	});
}

// ============================================================================
// Registration entry point
// ============================================================================

void RegisterCurveAssetTools(FSololmcpToolRegistry& Registry)
{
	RegisterCurveCreate(Registry);
	RegisterCurveAddKey(Registry);
	RegisterCurveRemoveKey(Registry);
	RegisterCurveSetInterpolation(Registry);
	RegisterCurveInspect(Registry);
}

} // namespace UE::SOMOLMCP
