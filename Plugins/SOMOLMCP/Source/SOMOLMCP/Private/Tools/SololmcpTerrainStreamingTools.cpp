// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpTerrainStreamingTools.cpp
// v2.1.0 — Terrain & Level streaming pipeline, Level/SubLevel CRUD, Enhanced Editor Perception
// Task 1: Heightmap → terrain generation, auto-split tiles, streaming config
// Task 2: Level create/save/load, actor cross-level transfer, WorldPartition data layers
// Task 4: Editor state perception, warnings, undo history, performance stats

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpTerrainModeGuard.h"

// Landscape / Heightmap
#include "Landscape.h"
#include "LandscapeEdit.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
#include "LandscapeEditLayer.h"
#else
// LandscapeEditLayer.h is 5.5+.
#endif
#include "LandscapeEditorUtils.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeUtils.h"
#include "LandscapeProxy.h"

// World Partition
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"

// Level / Streaming
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/Level.h"
#include "EditorLevelUtils.h"
#include "LevelEditorSubsystem.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "PackageTools.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

// Editor Perception
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Engine.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

// JSON / Misc
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Misc/Base64.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectArray.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Editor/TransBuffer.h"

// NavMesh export (navmesh_export_for_server tool)
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Builders/CubeBuilder.h"
#include "NavMesh/RecastNavMesh.h"
#include "Detour/DetourNavMesh.h"

extern ENGINE_API float GAverageFPS;

namespace UE::SOMOLMCP
{

namespace
{

	// ─── Base64 decode helpers ───────────────────────────────────────────────

	bool DecodeBase64(const FString& Base64Str, TArray<uint8>& OutBytes)
	{
		if (Base64Str.IsEmpty())
		{
			return false;
		}
		return FBase64::Decode(Base64Str, OutBytes);
	}

	FString SerializeArgsForTerrainTokens(const TSharedRef<FJsonObject>& Arguments)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Arguments, Writer);
		Out = Out.ToLower();
		return Out;
	}

	bool HasTerrainConstraintProofForHeightmap(const TSharedRef<FJsonObject>& Arguments)
	{
		return Arguments->HasField(TEXT("terrain_constraint_proof")) ||
			Arguments->HasField(TEXT("terrainConstraintProof")) ||
			Arguments->HasField(TEXT("pre_generation_constraints")) ||
			Arguments->HasField(TEXT("preGenerationConstraints")) ||
			Arguments->HasField(TEXT("constrained_heightmap_recipe")) ||
			Arguments->HasField(TEXT("constrainedHeightmapRecipe")) ||
			Arguments->HasField(TEXT("terrain_geomorphology_plan")) ||
			Arguments->HasField(TEXT("landform_constraints")) ||
			Arguments->HasField(TEXT("constraint_profile"));
	}

	void SmoothGrayHeightmap(TArray<uint8>& Data, int32 W, int32 H, int32 Passes)
	{
		if (W < 3 || H < 3 || Passes <= 0 || Data.Num() != W * H)
		{
			return;
		}
		TArray<uint8> Copy;
		Copy.SetNumUninitialized(Data.Num());
		for (int32 Pass = 0; Pass < Passes; ++Pass)
		{
			Copy = Data;
			for (int32 Y = 1; Y < H - 1; ++Y)
			{
				for (int32 X = 1; X < W - 1; ++X)
				{
					const int32 I = Y * W + X;
					const int32 Sum =
						Copy[I] * 4 +
						Copy[I - 1] + Copy[I + 1] +
						Copy[I - W] + Copy[I + W] +
						Copy[I - W - 1] + Copy[I - W + 1] +
						Copy[I + W - 1] + Copy[I + W + 1];
					Data[I] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Sum) / 12.0f), 0, 255));
				}
			}
		}
	}

	void ClampGraySlope(TArray<uint8>& Data, int32 W, int32 H, int32 MaxDelta, int32 Passes)
	{
		if (W < 2 || H < 2 || MaxDelta <= 0 || Data.Num() != W * H)
		{
			return;
		}
		for (int32 Pass = 0; Pass < FMath::Max(1, Passes); ++Pass)
		{
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const int32 I = Y * W + X;
					if (X > 0)
					{
						const int32 N = I - 1;
						const int32 Delta = static_cast<int32>(Data[I]) - static_cast<int32>(Data[N]);
						if (FMath::Abs(Delta) > MaxDelta)
						{
							Data[I] = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Data[N]) + FMath::Clamp(Delta, -MaxDelta, MaxDelta), 0, 255));
						}
					}
					if (Y > 0)
					{
						const int32 N = I - W;
						const int32 Delta = static_cast<int32>(Data[I]) - static_cast<int32>(Data[N]);
						if (FMath::Abs(Delta) > MaxDelta)
						{
							Data[I] = static_cast<uint8>(FMath::Clamp(static_cast<int32>(Data[N]) + FMath::Clamp(Delta, -MaxDelta, MaxDelta), 0, 255));
						}
					}
				}
			}
		}
	}

	bool DecodePngHeightmap16(const TArray<uint8>& PngBytes, int32& OutWidth, int32& OutHeight, TArray<uint16>& OutData)
	{
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(PngBytes.GetData(), PngBytes.Num()))
		{
			return false;
		}

		ERGBFormat Format = ImageWrapper->GetFormat();
		if (Format != ERGBFormat::Gray && Format != ERGBFormat::RGBA && Format != ERGBFormat::BGRA)
		{
			// Unsupported format — try raw decode anyway
		}

		OutWidth = ImageWrapper->GetWidth();
		OutHeight = ImageWrapper->GetHeight();
		if (OutWidth <= 0 || OutHeight <= 0)
		{
			return false;
		}

		// UE 5.7 GetRaw — use TArray<uint8> reference
		TArray<uint8> RawData;
		if (!ImageWrapper->GetRaw(ERGBFormat::Gray, 8, RawData) || RawData.Num() == 0)
		{
			return false;
		}

		OutData.SetNumUninitialized(OutWidth * OutHeight);
		if (Format == ERGBFormat::Gray)
		{
			for (int32 i = 0; i < RawData.Num() && i < OutData.Num(); ++i)
			{
				OutData[i] = static_cast<uint16>(RawData[i]) << 8; // 8-bit → 16-bit high byte
			}
		}
		else
		{
			// RGBA/BGRA: extract red channel
			for (int32 i = 0; i < OutData.Num(); ++i)
			{
				const int32 SrcIdx = i * 4;
				if (SrcIdx + 1 < RawData.Num())
				{
					OutData[i] = static_cast<uint16>(RawData[SrcIdx]) << 8;
				}
			}
		}
		return true;
	}


	// ─── Height data export helper ──────────────────────────────────────────

	bool ExportHeightmapData(ALandscape* Landscape, int32& OutWidth, int32& OutHeight, TArray<uint16>& OutData, FString& OutError)
	{
		if (!Landscape)
		{
			OutError = TEXT("Landscape is null.");
			return false;
		}

		ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
		if (!LandscapeInfo)
		{
			OutError = TEXT("Landscape info is unavailable.");
			return false;
		}

		FIntRect Bounds = Landscape->GetBoundingRect();
		OutWidth = Bounds.Width() + 1;
		OutHeight = Bounds.Height() + 1;
		OutData.SetNumZeroed(OutWidth * OutHeight);

		FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
		TArray<uint16> OneRow;
		OneRow.SetNumUninitialized(OutWidth);

		for (int32 Y = Bounds.Min.Y; Y <= Bounds.Max.Y; ++Y)
		{
			LandscapeEdit.GetHeightDataFast(Bounds.Min.X, Y, Bounds.Max.X, Y, OneRow.GetData(), 0);
			const int32 RowIdx = (Y - Bounds.Min.Y) * OutWidth;
			for (int32 X = 0; X < OutWidth; ++X)
			{
				OutData[RowIdx + X] = OneRow[X];
			}
		}

		return true;
	}

	// ─── Encode heightmap to PNG base64 ─────────────────────────────────────

	FString EncodeHeightmapToPngBase64(int32 Width, int32 Height, const TArray<uint16>& Data)
	{
		// FIX (v12): preserve full 16-bit precision instead of >> 8 lossy convert
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid())
		{
			return FString();
		}

		// Pass raw uint16 buffer with bit-depth 16. PNG supports 16-bit grayscale.
		const int64 NumBytes = static_cast<int64>(Data.Num()) * sizeof(uint16);
		ImageWrapper->SetRaw(Data.GetData(), NumBytes, Width, Height, ERGBFormat::Gray, 16);
		const TArray64<uint8> PngData64 = ImageWrapper->GetCompressed();
		TArray<uint8> PngData;
		PngData.SetNumUninitialized(PngData64.Num());
		FMemory::Memcpy(PngData.GetData(), PngData64.GetData(), PngData64.Num());
		return FBase64::Encode(PngData);
	}

	TSharedRef<FJsonObject> BuildTerrainQaMetrics(int32 Width, int32 Height, const TArray<uint16>& Data, const FString& Source)
	{
		TSharedRef<FJsonObject> Metrics = MakeShared<FJsonObject>();
		Metrics->SetStringField(TEXT("schema"), TEXT("somol.terrain.qa_metrics.v1"));
		Metrics->SetStringField(TEXT("source"), Source);
		Metrics->SetNumberField(TEXT("width"), Width);
		Metrics->SetNumberField(TEXT("height"), Height);
		Metrics->SetNumberField(TEXT("sample_count"), Data.Num());

		if (Width <= 0 || Height <= 0 || Data.Num() < Width * Height)
		{
			Metrics->SetStringField(TEXT("qa_status"), TEXT("invalid"));
			Metrics->SetStringField(TEXT("reason"), TEXT("height data dimensions are empty or inconsistent"));
			return Metrics;
		}

		uint16 MinH = 65535;
		uint16 MaxH = 0;
		double Sum = 0.0;
		for (uint16 H : Data)
		{
			MinH = FMath::Min(MinH, H);
			MaxH = FMath::Max(MaxH, H);
			Sum += static_cast<double>(H);
		}
		const double Mean = Sum / static_cast<double>(Data.Num());
		double VarianceSum = 0.0;
		for (uint16 H : Data)
		{
			const double D = static_cast<double>(H) - Mean;
			VarianceSum += D * D;
		}

		double SlopeSum = 0.0;
		double MaxSlope = 0.0;
		int32 SlopeSamples = 0;
		int32 FlatSamples = 0;
		int32 CliffSamples = 0;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 I = Y * Width + X;
				const double Here = static_cast<double>(Data[I]);
				double LocalSlope = 0.0;
				if (X + 1 < Width)
				{
					LocalSlope = FMath::Max(LocalSlope, FMath::Abs(static_cast<double>(Data[I + 1]) - Here));
				}
				if (Y + 1 < Height)
				{
					LocalSlope = FMath::Max(LocalSlope, FMath::Abs(static_cast<double>(Data[I + Width]) - Here));
				}
				SlopeSum += LocalSlope;
				MaxSlope = FMath::Max(MaxSlope, LocalSlope);
				++SlopeSamples;
				if (LocalSlope <= 2.0)
				{
					++FlatSamples;
				}
				if (LocalSlope >= 2048.0)
				{
					++CliffSamples;
				}
			}
		}

		const double Range = static_cast<double>(MaxH) - static_cast<double>(MinH);
		const double StdDev = FMath::Sqrt(VarianceSum / static_cast<double>(Data.Num()));
		const double MeanSlope = SlopeSamples > 0 ? SlopeSum / static_cast<double>(SlopeSamples) : 0.0;
		const double FlatRatio = SlopeSamples > 0 ? static_cast<double>(FlatSamples) / static_cast<double>(SlopeSamples) : 0.0;
		const double CliffRatio = SlopeSamples > 0 ? static_cast<double>(CliffSamples) / static_cast<double>(SlopeSamples) : 0.0;
		const FString QaStatus = (Range <= 0.0 || CliffRatio > 0.25) ? TEXT("warn") : TEXT("pass");

		Metrics->SetNumberField(TEXT("min_height_u16"), MinH);
		Metrics->SetNumberField(TEXT("max_height_u16"), MaxH);
		Metrics->SetNumberField(TEXT("height_range_u16"), Range);
		Metrics->SetNumberField(TEXT("mean_height_u16"), Mean);
		Metrics->SetNumberField(TEXT("stddev_height_u16"), StdDev);
		Metrics->SetNumberField(TEXT("mean_abs_slope_u16_per_sample"), MeanSlope);
		Metrics->SetNumberField(TEXT("max_abs_slope_u16_per_sample"), MaxSlope);
		Metrics->SetNumberField(TEXT("flat_sample_ratio"), FlatRatio);
		Metrics->SetNumberField(TEXT("cliff_sample_ratio"), CliffRatio);
		Metrics->SetStringField(TEXT("qa_status"), QaStatus);
		Metrics->SetStringField(
			TEXT("receipt_note"),
			TEXT("Metrics are computed from height samples before material/erosion simulation; use as terrain QA evidence for flatness, cliffs, and malformed input."));
		return Metrics;
	}

	FString PythonStringLiteral(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	bool NormalizeLevelPackagePath(const FString& Input, FString& OutPackagePath, FString& OutError)
	{
		OutPackagePath = Input.TrimStartAndEnd();
		if (OutPackagePath.EndsWith(TEXT(".umap")))
		{
			OutPackagePath.LeftChopInline(5);
		}

		const int32 LastSlash = OutPackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 DotAfterSlash = OutPackagePath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, LastSlash + 1);
		if (DotAfterSlash != INDEX_NONE)
		{
			OutPackagePath = OutPackagePath.Left(DotAfterSlash);
		}

		if (!OutPackagePath.StartsWith(TEXT("/Game/")))
		{
			OutError = TEXT("level_path must be a /Game/... long package path.");
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(OutPackagePath))
		{
			OutError = FString::Printf(TEXT("Invalid level package path: %s"), *OutPackagePath);
			return false;
		}
		if (FPackageName::GetLongPackageAssetName(OutPackagePath).IsEmpty())
		{
			OutError = TEXT("level_path must include a map asset name.");
			return false;
		}
		return true;
	}

	FString LevelObjectPathFromPackagePath(const FString& PackagePath)
	{
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	bool IsSafeSimpleName(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 64)
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Ch = Value[Index];
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('_') && Ch != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

	void AddStringIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Issue)
	{
		Issues.Add(MakeShared<FJsonValueString>(Issue));
	}

	bool IsSupportedLandscapeImportTuple(int32 NumSubsections, int32 SubsectionSizeQuads)
	{
		if (NumSubsections != 1 && NumSubsections != 2)
		{
			return false;
		}
		constexpr int32 ValidSectionSizes[] = {7, 15, 31, 63, 127, 255};
		for (int32 Size : ValidSectionSizes)
		{
			if (SubsectionSizeQuads == Size)
			{
				return true;
			}
		}
		return false;
	}

	TSharedRef<FJsonObject> BuildLandscapeImportValidationReceipt(
		const FString& Status,
		const FString& Stage,
		int32 SourceWidth,
		int32 SourceHeight,
		int32 ComponentCountX,
		int32 ComponentCountY,
		int32 NumSubsections,
		int32 SubsectionSizeQuads,
		int32 ImportWidth,
		int32 ImportHeight,
		int32 HeightSampleCount,
		const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somol.landscape.import_validation.v1"));
		Receipt->SetStringField(TEXT("status"), Status);
		Receipt->SetStringField(TEXT("stage"), Stage);
		Receipt->SetNumberField(TEXT("source_width"), SourceWidth);
		Receipt->SetNumberField(TEXT("source_height"), SourceHeight);
		Receipt->SetNumberField(TEXT("component_count_x"), ComponentCountX);
		Receipt->SetNumberField(TEXT("component_count_y"), ComponentCountY);
		Receipt->SetNumberField(TEXT("num_subsections"), NumSubsections);
		Receipt->SetNumberField(TEXT("subsection_size_quads"), SubsectionSizeQuads);
		Receipt->SetNumberField(TEXT("import_width"), ImportWidth);
		Receipt->SetNumberField(TEXT("import_height"), ImportHeight);
		Receipt->SetNumberField(TEXT("height_sample_count"), HeightSampleCount);
		Receipt->SetArrayField(TEXT("issues"), Issues);
		Receipt->SetStringField(TEXT("failure_action"), Issues.Num() > 0
			? TEXT("Refused before ALandscapeProxy::Import; fix dimensions/layer maps and retry.")
			: TEXT("Validation passed; Import may proceed."));
		return Receipt;
	}

	bool ValidateLandscapeImportShape(
		int32 SourceWidth,
		int32 SourceHeight,
		const TArray<uint16>& SourceHeightData,
		int32 ComponentCountX,
		int32 ComponentCountY,
		int32 NumSubsections,
		int32 SubsectionSizeQuads,
		int32 ImportWidth,
		int32 ImportHeight,
		const TArray<uint16>& ImportData,
		TArray<TSharedPtr<FJsonValue>>& OutIssues)
	{
		if (SourceWidth <= 1 || SourceHeight <= 1)
		{
			AddStringIssue(OutIssues, TEXT("source heightmap width/height must both be greater than 1"));
		}
		const int64 ExpectedSourceSamples = static_cast<int64>(SourceWidth) * static_cast<int64>(SourceHeight);
		if (ExpectedSourceSamples <= 0 || ExpectedSourceSamples > MAX_int32 || SourceHeightData.Num() != ExpectedSourceSamples)
		{
			AddStringIssue(OutIssues, FString::Printf(
				TEXT("source height sample count mismatch: got %d, expected %lld"),
				SourceHeightData.Num(), ExpectedSourceSamples));
		}
		if (ComponentCountX <= 0 || ComponentCountY <= 0)
		{
			AddStringIssue(OutIssues, TEXT("component_count_x/y must both be positive"));
		}
		if (!IsSupportedLandscapeImportTuple(NumSubsections, SubsectionSizeQuads))
		{
			AddStringIssue(OutIssues, FString::Printf(
				TEXT("unsupported landscape import tuple: num_subsections=%d, subsection_size_quads=%d"),
				NumSubsections, SubsectionSizeQuads));
		}
		const int64 ExpectedImportWidth = static_cast<int64>(ComponentCountX) * NumSubsections * SubsectionSizeQuads + 1;
		const int64 ExpectedImportHeight = static_cast<int64>(ComponentCountY) * NumSubsections * SubsectionSizeQuads + 1;
		const int64 ExpectedImportSamples = ExpectedImportWidth * ExpectedImportHeight;
		if (ExpectedImportWidth <= 1 || ExpectedImportHeight <= 1 ||
			ExpectedImportWidth > 8193 || ExpectedImportHeight > 8193 ||
			ExpectedImportSamples <= 0 || ExpectedImportSamples > MAX_int32)
		{
			AddStringIssue(OutIssues, FString::Printf(
				TEXT("import resolution %lldx%lld outside safe bounds"),
				ExpectedImportWidth, ExpectedImportHeight));
		}
		if (ImportWidth != ExpectedImportWidth || ImportHeight != ExpectedImportHeight)
		{
			AddStringIssue(OutIssues, FString::Printf(
				TEXT("import dimensions %dx%d do not match component math %lldx%lld"),
				ImportWidth, ImportHeight, ExpectedImportWidth, ExpectedImportHeight));
		}
		if (ImportData.Num() != ExpectedImportSamples)
		{
			AddStringIssue(OutIssues, FString::Printf(
				TEXT("import height sample count mismatch: got %d, expected %lld"),
				ImportData.Num(), ExpectedImportSamples));
		}
		return OutIssues.Num() == 0;
	}

	bool ValidateLandscapeImportMaps(
		const FGuid& ExpectedLayerGuid,
		const TMap<FGuid, TArray<uint16>>& HeightDataPerLoc,
		const TMap<FGuid, TArray<FLandscapeImportLayerInfo>>& MaterialLayerDataPerLoc,
		int32 ExpectedSampleCount,
		TArray<TSharedPtr<FJsonValue>>& OutIssues)
	{
		if (HeightDataPerLoc.Num() != 1)
		{
			AddStringIssue(OutIssues, FString::Printf(TEXT("height import map must contain exactly one layer key, got %d"), HeightDataPerLoc.Num()));
		}
		if (!HeightDataPerLoc.Contains(ExpectedLayerGuid))
		{
			AddStringIssue(OutIssues, TEXT("height import map is missing the final-layer FGuid() key expected by UE Import"));
		}
		else if (const TArray<uint16>* Samples = HeightDataPerLoc.Find(ExpectedLayerGuid))
		{
			if (Samples->Num() != ExpectedSampleCount)
			{
				AddStringIssue(OutIssues, FString::Printf(
					TEXT("height import map sample count mismatch: got %d, expected %d"),
					Samples->Num(), ExpectedSampleCount));
			}
		}
		if (MaterialLayerDataPerLoc.Num() != 1)
		{
			AddStringIssue(OutIssues, FString::Printf(TEXT("material layer import map must contain exactly one layer key, got %d"), MaterialLayerDataPerLoc.Num()));
		}
		if (!MaterialLayerDataPerLoc.Contains(ExpectedLayerGuid))
		{
			AddStringIssue(OutIssues, TEXT("material layer import map key must match the height final-layer FGuid() key"));
		}
		return OutIssues.Num() == 0;
	}

	void SetLandscapeImportFailure(
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError,
		const FString& Error,
		const TSharedRef<FJsonObject>& Receipt)
	{
		OutError = Error;
		OutStructured->SetStringField(TEXT("status"), TEXT("failed_validation"));
		OutStructured->SetStringField(TEXT("error_code"), TEXT("landscape_import_preflight_failed"));
		OutStructured->SetStringField(TEXT("error"), Error);
		OutStructured->SetObjectField(TEXT("landscape_import_validation"), Receipt);
	}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// RegisterTerrainStreamingTools
// ═══════════════════════════════════════════════════════════════════════════════

void RegisterTerrainStreamingTools(FSololmcpToolRegistry& Registry)
{

	// ─── Task 1: Landscape / Heightmap Pipeline ─────────────────────────────

	// ---- landscape_create_from_heightmap ----
	// Create a landscape from base64-encoded heightmap data (PNG 8-bit or raw R16)
	Registry.Register({
		TEXT("landscape_create_from_heightmap"),
		TEXT("Create a landscape from base64-encoded heightmap data. Supports PNG (8-bit grayscale) or raw R16 format. Returns the landscape actor path."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("heightmap_base64"), FSololmcpSchemaBuilder::String(TEXT("Base64-encoded heightmap (PNG grayscale or raw R16)"))},
			{TEXT("format"), FSololmcpSchemaBuilder::String(TEXT("Heightmap format: 'PNG' (default) or 'R16'"))},
			{TEXT("width"), FSololmcpSchemaBuilder::Number(TEXT("Width in pixels (required for R16 format)"))},
			{TEXT("height"), FSololmcpSchemaBuilder::Number(TEXT("Height in pixels (required for R16 format)"))},
			{TEXT("component_count_x"), FSololmcpSchemaBuilder::Number(TEXT("Number of components in X direction (default: 1)"))},
			{TEXT("component_count_y"), FSololmcpSchemaBuilder::Number(TEXT("Number of components in Y direction (default: 1)"))},
			{TEXT("quads_per_component"), FSololmcpSchemaBuilder::Number(TEXT("Quads per component (default: 63)"))},
			{TEXT("cm_per_quad"), FSololmcpSchemaBuilder::Number(TEXT("World cm per quad (default: 100)"))},
			{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Validate import geometry/maps and return a receipt without spawning the landscape"))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({
				{TEXT("x"), FSololmcpSchemaBuilder::Number()},
				{TEXT("y"), FSololmcpSchemaBuilder::Number()},
				{TEXT("z"), FSololmcpSchemaBuilder::Number()}
			})},
			{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Actor label for the new landscape"))},
			{TEXT("material"), FSololmcpSchemaBuilder::String(TEXT("Material instance path to apply (optional)"))}
		}, {TEXT("heightmap_base64")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString HeightmapB64 = Arguments->GetStringField(TEXT("heightmap_base64"));
			const FString Format = Arguments->HasTypedField<EJson::String>(TEXT("format"))
				? Arguments->GetStringField(TEXT("format")).ToUpper() : TEXT("PNG");

			TArray<uint8> RawBytes;
			if (!DecodeBase64(HeightmapB64, RawBytes))
			{
				OutError = TEXT("Failed to decode base64 heightmap data.");
				return false;
			}

			int32 HmWidth = 0, HmHeight = 0;
			TArray<uint16> HeightData;

			if (Format == TEXT("R16"))
			{
				HmWidth = Arguments->GetIntegerField(TEXT("width"));
				HmHeight = Arguments->GetIntegerField(TEXT("height"));
				const int64 ExpectedR16Samples = static_cast<int64>(HmWidth) * static_cast<int64>(HmHeight);
				const int64 ExpectedR16Bytes = ExpectedR16Samples * 2;
				if (HmWidth <= 1 || HmHeight <= 1 || ExpectedR16Samples <= 0 || ExpectedR16Samples > MAX_int32 ||
					ExpectedR16Bytes <= 0 || RawBytes.Num() < ExpectedR16Bytes)
				{
					OutError = TEXT("Invalid R16 dimensions or insufficient data.");
					return false;
				}
				HeightData.SetNumUninitialized(static_cast<int32>(ExpectedR16Samples));
				FMemory::Memcpy(HeightData.GetData(), RawBytes.GetData(), static_cast<SIZE_T>(ExpectedR16Bytes));
			}
			else
			{
				// PNG format
				if (!DecodePngHeightmap16(RawBytes, HmWidth, HmHeight, HeightData))
				{
					OutError = TEXT("Failed to decode PNG heightmap. Ensure it is 8-bit grayscale.");
					return false;
				}
			}

			// Determine component configuration
			const int32 QuadsPerComp = Arguments->HasTypedField<EJson::Number>(TEXT("quads_per_component"))
				? Arguments->GetIntegerField(TEXT("quads_per_component")) : 63;
			const int32 CmPerQuad = Arguments->HasTypedField<EJson::Number>(TEXT("cm_per_quad"))
				? Arguments->GetIntegerField(TEXT("cm_per_quad")) : 100;

			// Auto-calculate component counts from heightmap size if not specified
			int32 CompX = Arguments->HasTypedField<EJson::Number>(TEXT("component_count_x"))
				? Arguments->GetIntegerField(TEXT("component_count_x")) : 1;
			int32 CompY = Arguments->HasTypedField<EJson::Number>(TEXT("component_count_y"))
				? Arguments->GetIntegerField(TEXT("component_count_y")) : 1;

			if (CompX <= 0) CompX = 1;
			if (CompY <= 0) CompY = 1;

			const int32 QPC = QuadsPerComp;
			const int32 NumSubsections = 1;
			const int32 SubsectionSizeQuads = QPC;
			const bool bDryRun = Arguments->HasTypedField<EJson::Boolean>(TEXT("dry_run"))
				? Arguments->GetBoolField(TEXT("dry_run")) : false;
			const int64 SourceSampleCount = static_cast<int64>(HmWidth) * static_cast<int64>(HmHeight);
			if (HmWidth <= 1 || HmHeight <= 1 || SourceSampleCount <= 0 ||
				SourceSampleCount > MAX_int32 || HeightData.Num() != SourceSampleCount)
			{
				TArray<TSharedPtr<FJsonValue>> SourceIssues;
				AddStringIssue(SourceIssues, FString::Printf(
					TEXT("decoded heightmap dimensions/data are invalid: %dx%d with %d samples"),
					HmWidth, HmHeight, HeightData.Num()));
				TSharedRef<FJsonObject> Receipt = BuildLandscapeImportValidationReceipt(
					TEXT("fail"),
					TEXT("source_heightmap"),
					HmWidth,
					HmHeight,
					CompX,
					CompY,
					NumSubsections,
					SubsectionSizeQuads,
					0,
					0,
					HeightData.Num(),
					SourceIssues);
				SetLandscapeImportFailure(OutStructured, OutError, TEXT("Landscape source heightmap validation failed."), Receipt);
				return false;
			}

			// ── Geometry validation: prevent ALandscapeProxy::Import crash ──
			// UE's landscape importer asserts (TMap FindChecked at Map.h.inl:635)
			// when the (NumSubsections, SubsectionSizeQuads) tuple is outside
			// its allowed combinations table. We use NumSubsections=1 here,
			// so QPC must be in UE's allowed set and resulting actor dimensions
			// must be sane. Reject up-front with a clear error rather than
			// crashing the editor (root cause of 2026-04-25 4K heightmap crash).
			{
				if (!IsSupportedLandscapeImportTuple(NumSubsections, SubsectionSizeQuads))
				{
					TArray<TSharedPtr<FJsonValue>> TupleIssues;
					AddStringIssue(TupleIssues, FString::Printf(
						TEXT("quads_per_component=%d not in UE allowed set {7,15,31,63,127,255}"),
						QPC));
					TSharedRef<FJsonObject> Receipt = BuildLandscapeImportValidationReceipt(
						TEXT("fail"), TEXT("component_tuple"), HmWidth, HmHeight,
						CompX, CompY, NumSubsections, SubsectionSizeQuads, 0, 0,
						HeightData.Num(), TupleIssues);
					SetLandscapeImportFailure(OutStructured, OutError, TEXT("Landscape component tuple validation failed."), Receipt);
					return false;
				}

				const int64 TotalQuadsX = static_cast<int64>(CompX) * NumSubsections * SubsectionSizeQuads;
				const int64 TotalQuadsY = static_cast<int64>(CompY) * NumSubsections * SubsectionSizeQuads;
				if (TotalQuadsX <= 0 || TotalQuadsY <= 0 ||
					TotalQuadsX > 8192 || TotalQuadsY > 8192)
				{
					TArray<TSharedPtr<FJsonValue>> SizeIssues;
					AddStringIssue(SizeIssues, FString::Printf(
						TEXT("Resulting landscape (%lldx%lld quads) outside [1,8192]; "
							 "increase quads_per_component or reduce component_count_*"),
						TotalQuadsX, TotalQuadsY));
					TSharedRef<FJsonObject> Receipt = BuildLandscapeImportValidationReceipt(
						TEXT("fail"), TEXT("component_extent"), HmWidth, HmHeight,
						CompX, CompY, NumSubsections, SubsectionSizeQuads, 0, 0,
						HeightData.Num(), SizeIssues);
					SetLandscapeImportFailure(OutStructured, OutError, TEXT("Landscape component extent validation failed."), Receipt);
					return false;
				}

				// Reject the silent-downsample-to-64x64 footgun: if the input
				// heightmap is large but caller forgot to specify component
				// counts, we'd quietly resample to a tiny grid. Fail loud.
				if (HmWidth >= 1024 && CompX == 1 && CompY == 1)
				{
					TArray<TSharedPtr<FJsonValue>> DownsampleIssues;
					AddStringIssue(DownsampleIssues, FString::Printf(
						TEXT("Input heightmap is %dx%d but component_count_x/y both 1 "
							 "would downsample to %dx%d. Pass explicit component_count_x "
							 "and component_count_y (e.g. 8,8 for ~512x512 final, "
							 "16,16 for ~1024x1024)."),
						HmWidth, HmHeight, QPC + 1, QPC + 1));
					TSharedRef<FJsonObject> Receipt = BuildLandscapeImportValidationReceipt(
						TEXT("fail"), TEXT("unsafe_downsample"), HmWidth, HmHeight,
						CompX, CompY, NumSubsections, SubsectionSizeQuads, QPC + 1, QPC + 1,
						HeightData.Num(), DownsampleIssues);
					SetLandscapeImportFailure(OutStructured, OutError, TEXT("Landscape import would unsafe-downsample a large source heightmap."), Receipt);
					return false;
				}
			}

			// Build heightmap data array for FLandscapeEditorUtils
			TArray<uint16> ImportData;
			const int32 TotalPointsX = CompX * NumSubsections * SubsectionSizeQuads + 1;
			const int32 TotalPointsY = CompY * NumSubsections * SubsectionSizeQuads + 1;
			const int32 ExpectedPointsX = CompX * NumSubsections * SubsectionSizeQuads + 1;
			const int32 ExpectedPointsY = CompY * NumSubsections * SubsectionSizeQuads + 1;
			if (TotalPointsX != ExpectedPointsX || TotalPointsY != ExpectedPointsY)
			{
				OutError = FString::Printf(
					TEXT("Invalid landscape import geometry: computed points %dx%d, expected %dx%d for subsections=%d section_size=%d."),
					TotalPointsX, TotalPointsY, ExpectedPointsX, ExpectedPointsY, NumSubsections, SubsectionSizeQuads);
				return false;
			}

			ImportData.SetNumZeroed(TotalPointsX * TotalPointsY);

			// Resample heightmap to target resolution
			for (int32 y = 0; y < TotalPointsY; ++y)
			{
				for (int32 x = 0; x < TotalPointsX; ++x)
				{
					// Bilinear sample from source
					const float SrcX = static_cast<float>(x) / (TotalPointsX - 1) * (HmWidth - 1);
					const float SrcY = static_cast<float>(y) / (TotalPointsY - 1) * (HmHeight - 1);
					const int32 IX = FMath::FloorToInt(SrcX);
					const int32 IY = FMath::FloorToInt(SrcY);
					const float FX = SrcX - IX;
					const float FY = SrcY - IY;
					const int32 IX1 = FMath::Min(IX + 1, HmWidth - 1);
					const int32 IY1 = FMath::Min(IY + 1, HmHeight - 1);

					const float H00 = HeightData[IY * HmWidth + IX];
					const float H10 = HeightData[IY * HmWidth + IX1];
					const float H01 = HeightData[IY1 * HmWidth + IX];
					const float H11 = HeightData[IY1 * HmWidth + IX1];
					const float H = H00 * (1 - FX) * (1 - FY) + H10 * FX * (1 - FY) + H01 * (1 - FX) * FY + H11 * FX * FY;

					ImportData[y * TotalPointsX + x] = static_cast<uint16>(FMath::Clamp(H, 0.0f, 65535.0f));
				}
			}

			TArray<TSharedPtr<FJsonValue>> ValidationIssues;
			ValidateLandscapeImportShape(
				HmWidth,
				HmHeight,
				HeightData,
				CompX,
				CompY,
				NumSubsections,
				SubsectionSizeQuads,
				TotalPointsX,
				TotalPointsY,
				ImportData,
				ValidationIssues);
			if (ValidationIssues.Num() > 0)
			{
				TSharedRef<FJsonObject> Receipt = BuildLandscapeImportValidationReceipt(
					TEXT("fail"),
					TEXT("heightmap_geometry"),
					HmWidth,
					HmHeight,
					CompX,
					CompY,
					NumSubsections,
					SubsectionSizeQuads,
					TotalPointsX,
					TotalPointsY,
					ImportData.Num(),
					ValidationIssues);
				SetLandscapeImportFailure(OutStructured, OutError, TEXT("Landscape import geometry validation failed."), Receipt);
				return false;
			}

			UMaterialInterface* PendingMaterial = nullptr;
			if (Arguments->HasTypedField<EJson::String>(TEXT("material")))
			{
				const FString MaterialPath = Arguments->GetStringField(TEXT("material")).TrimStartAndEnd();
				if (!MaterialPath.IsEmpty())
				{
					FString MatError;
					PendingMaterial = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, MatError));
					if (!PendingMaterial)
					{
						AddStringIssue(ValidationIssues, FString::Printf(TEXT("material could not be loaded as UMaterialInterface: %s"), *MaterialPath));
						TSharedRef<FJsonObject> Receipt = BuildLandscapeImportValidationReceipt(
							TEXT("fail"),
							TEXT("material_preflight"),
							HmWidth,
							HmHeight,
							CompX,
							CompY,
							NumSubsections,
							SubsectionSizeQuads,
							TotalPointsX,
							TotalPointsY,
							ImportData.Num(),
							ValidationIssues);
						SetLandscapeImportFailure(OutStructured, OutError, TEXT("Landscape material validation failed."), Receipt);
						return false;
					}
				}
			}

			const FGuid FinalLayerGuid = FGuid();
			TMap<FGuid, TArray<uint16>> HeightDataPerLoc;
			HeightDataPerLoc.Add(FinalLayerGuid, MoveTemp(ImportData));

			TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLoc;
			MaterialLayerDataPerLoc.Add(FinalLayerGuid, TArray<FLandscapeImportLayerInfo>());

			ValidateLandscapeImportMaps(
				FinalLayerGuid,
				HeightDataPerLoc,
				MaterialLayerDataPerLoc,
				TotalPointsX * TotalPointsY,
				ValidationIssues);
			const TArray<uint16>* FinalImportData = HeightDataPerLoc.Find(FinalLayerGuid);
			if (ValidationIssues.Num() > 0 || !FinalImportData)
			{
				TSharedRef<FJsonObject> Receipt = BuildLandscapeImportValidationReceipt(
					TEXT("fail"),
					TEXT("import_map_preflight"),
					HmWidth,
					HmHeight,
					CompX,
					CompY,
					NumSubsections,
					SubsectionSizeQuads,
					TotalPointsX,
					TotalPointsY,
					FinalImportData ? FinalImportData->Num() : 0,
					ValidationIssues);
				SetLandscapeImportFailure(OutStructured, OutError, TEXT("Landscape import map validation failed."), Receipt);
				return false;
			}

			TSharedRef<FJsonObject> ImportValidationReceipt = BuildLandscapeImportValidationReceipt(
				bDryRun ? TEXT("dry_run_pass") : TEXT("pass"),
				bDryRun ? TEXT("dry_run") : TEXT("pre_import"),
				HmWidth,
				HmHeight,
				CompX,
				CompY,
				NumSubsections,
				SubsectionSizeQuads,
				TotalPointsX,
				TotalPointsY,
				FinalImportData->Num(),
				ValidationIssues);
			TSharedRef<FJsonObject> TerrainQaMetrics = BuildTerrainQaMetrics(
				TotalPointsX,
				TotalPointsY,
				*FinalImportData,
				TEXT("landscape_create_from_heightmap.import_data"));
			OutStructured->SetObjectField(TEXT("landscape_import_validation"), ImportValidationReceipt);
			OutStructured->SetObjectField(TEXT("terrain_qa_metrics"), TerrainQaMetrics);
			if (bDryRun)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("dry_run_pass"));
				OutStructured->SetNumberField(TEXT("width_quads"), TotalPointsX - 1);
				OutStructured->SetNumberField(TEXT("height_quads"), TotalPointsY - 1);
				OutStructured->SetNumberField(TEXT("components_x"), CompX);
				OutStructured->SetNumberField(TEXT("components_y"), CompY);
				OutStructured->SetNumberField(TEXT("cm_per_quad"), CmPerQuad);
				OutSummary = FString::Printf(TEXT("Dry-run passed for landscape import (%dx%d points, %d components)."),
					TotalPointsX, TotalPointsY, CompX * CompY);
				return true;
			}

			TerrainModeGuard::FSelectionScope ModeGuard;
			if (!ModeGuard.Begin(OutError))
			{
				ModeGuard.Attach(OutStructured);
				return false;
			}
			ModeGuard.Attach(OutStructured);

			// Spawn location
			FVector SpawnLoc(0.0, 0.0, 0.0);
			if (const TSharedPtr<FJsonObject>* LocObj = nullptr; Arguments->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
			{
				FSololmcpEditorServices::JsonToVector(*LocObj, SpawnLoc);
			}

			// Create landscape via Python (FLandscapeEditorUtils::ImportLandscapeGlobal not available in UE 5.7)
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CreateLandscapeFromHeightmap", "SOMOLMCP Create Landscape From Heightmap"));

			// HEADLESS C++ DIRECT MEMORY TERRAIN INJECTION
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				OutError = TEXT("No editor world available.");
				return false;
			}
			ALandscape* NewLandscape = World->SpawnActor<ALandscape>(ALandscape::StaticClass(), FTransform(FRotator::ZeroRotator, SpawnLoc, FVector(static_cast<float>(CmPerQuad), static_cast<float>(CmPerQuad), 100.0f)));
			if (!NewLandscape)
			{
				OutError = TEXT("Landscape was created but could not be found.");
				return false;
			}
			FGuid LandscapeGuid = FGuid::NewGuid();

			
			NewLandscape->Import(
				LandscapeGuid,
				0, 0,
				TotalPointsX - 1, TotalPointsY - 1,
				NumSubsections,
				SubsectionSizeQuads,
				HeightDataPerLoc,
				nullptr,
				MaterialLayerDataPerLoc,
				ELandscapeImportAlphamapType::Additive,
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
				TArrayView<const FLandscapeLayer>()
#else
				// 5.3 takes a pointer to the layer array; the view overload arrived in 5.4.
				nullptr
#endif
			);

			if (NewLandscape->LandscapeComponents.Num() <= 0)
			{
				NewLandscape->Destroy();
				OutStructured->SetStringField(TEXT("status"), TEXT("failed_validation"));
				OutStructured->SetStringField(TEXT("error_code"), TEXT("landscape_import_readback_failed"));
				OutError = TEXT("Landscape import returned but no LandscapeComponents were created.");
				return false;
			}

			// Apply scale
			NewLandscape->SetActorScale3D(FVector(static_cast<float>(CmPerQuad), static_cast<float>(CmPerQuad), NewLandscape->GetActorScale3D().Z));
			NewLandscape->SetActorLocation(SpawnLoc);

			// Apply name
			if (Arguments->HasTypedField<EJson::String>(TEXT("name")))
			{
				NewLandscape->SetActorLabel(Arguments->GetStringField(TEXT("name")));
			}

			// Apply material
			if (PendingMaterial)
			{
				// UE5.7.4: EditorSetLandscapeMaterial is not exported (no LANDSCAPE_API).
				// Directly set the member variable after preflight has loaded it.
				NewLandscape->LandscapeMaterial = PendingMaterial;
			}

			GEditor->SelectNone(false, true, false);
			GEditor->SelectActor(NewLandscape, true, true);

			OutStructured->SetStringField(TEXT("actor_name"), NewLandscape->GetActorLabel());
			OutStructured->SetStringField(TEXT("actor_path"), NewLandscape->GetPathName());
			OutStructured->SetNumberField(TEXT("width_quads"), TotalPointsX - 1);
			OutStructured->SetNumberField(TEXT("height_quads"), TotalPointsY - 1);
			OutStructured->SetNumberField(TEXT("components_x"), CompX);
			OutStructured->SetNumberField(TEXT("components_y"), CompY);
			OutStructured->SetNumberField(TEXT("cm_per_quad"), CmPerQuad);

			TSharedRef<FJsonObject> LocJson = MakeShared<FJsonObject>();
			const FVector FinalLoc = NewLandscape->GetActorLocation();
			LocJson->SetNumberField(TEXT("x"), FinalLoc.X);
			LocJson->SetNumberField(TEXT("y"), FinalLoc.Y);
			LocJson->SetNumberField(TEXT("z"), FinalLoc.Z);
			OutStructured->SetObjectField(TEXT("location"), LocJson);

			OutSummary = FString::Printf(TEXT("Created landscape '%s' from heightmap (%dx%d, %d components)"),
				*NewLandscape->GetActorLabel(), TotalPointsX, TotalPointsY, CompX * CompY);
			return true;
		}
	});

	// ---- landscape_get_heightmap_base64 ----
	// Export current landscape heightmap as base64-encoded PNG
	Registry.Register({
		TEXT("landscape_get_heightmap_base64"),
		TEXT("Export a landscape's heightmap as base64-encoded PNG grayscale image. Returns the image data and dimensions."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor name or label"))},
			{TEXT("region_min_x"), FSololmcpSchemaBuilder::Number(TEXT("Optional: region min X (default: full extent)"))},
			{TEXT("region_min_y"), FSololmcpSchemaBuilder::Number(TEXT("Optional: region min Y (default: full extent)"))},
			{TEXT("region_max_x"), FSololmcpSchemaBuilder::Number(TEXT("Optional: region max X (default: full extent)"))},
			{TEXT("region_max_y"), FSololmcpSchemaBuilder::Number(TEXT("Optional: region max Y (default: full extent)"))}
		}, {TEXT("landscape")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString LandscapeId = Arguments->GetStringField(TEXT("landscape"));
			AActor* Actor = Context.Services.FindActorByLabelOrName(LandscapeId, OutError);
			if (!Actor) return false;

			ALandscape* Landscape = Cast<ALandscape>(Actor);
			if (!Landscape)
			{
				if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Actor))
				{
					Landscape = Proxy->GetLandscapeActor();
				}
			}
			if (!Landscape)
			{
				OutError = TEXT("Actor is not a landscape.");
				return false;
			}

			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (!LandscapeInfo)
			{
				OutError = TEXT("Landscape info is unavailable.");
				return false;
			}

			FIntRect Bounds = Landscape->GetBoundingRect();

			// Allow region override
			if (Arguments->HasTypedField<EJson::Number>(TEXT("region_min_x")))
			{
				Bounds.Min.X = Arguments->GetIntegerField(TEXT("region_min_x"));
				Bounds.Min.Y = Arguments->GetIntegerField(TEXT("region_min_y"));
				Bounds.Max.X = Arguments->GetIntegerField(TEXT("region_max_x"));
				Bounds.Max.Y = Arguments->GetIntegerField(TEXT("region_max_y"));
			}

			const int32 W = Bounds.Width() + 1;
			const int32 H = Bounds.Height() + 1;

			TArray<uint16> HeightData;
			HeightData.SetNumZeroed(W * H);

			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			TArray<uint16> OneRow;
			OneRow.SetNumUninitialized(W);

			for (int32 Y = Bounds.Min.Y; Y <= Bounds.Max.Y; ++Y)
			{
				LandscapeEdit.GetHeightDataFast(Bounds.Min.X, Y, Bounds.Max.X, Y, OneRow.GetData(), 0);
				const int32 RowIdx = (Y - Bounds.Min.Y) * W;
				FMemory::Memcpy(&HeightData[RowIdx], OneRow.GetData(), W * sizeof(uint16));
			}

			const FString Base64Png = EncodeHeightmapToPngBase64(W, H, HeightData);

			OutStructured->SetStringField(TEXT("landscape"), Landscape->GetPathName());
			OutStructured->SetNumberField(TEXT("width"), W);
			OutStructured->SetNumberField(TEXT("height"), H);
			OutStructured->SetStringField(TEXT("format"), TEXT("PNG"));
			OutStructured->SetStringField(TEXT("image_base64"), Base64Png);

			// Height statistics
			uint16 MinH = 65535, MaxH = 0;
			for (const uint16 V : HeightData)
			{
				MinH = FMath::Min(MinH, V);
				MaxH = FMath::Max(MaxH, V);
			}
			OutStructured->SetNumberField(TEXT("min_height_raw"), MinH);
			OutStructured->SetNumberField(TEXT("max_height_raw"), MaxH);
			OutStructured->SetObjectField(TEXT("terrain_qa_metrics"), BuildTerrainQaMetrics(W, H, HeightData, TEXT("landscape_get_heightmap_base64.exported_heightmap")));

			OutSummary = FString::Printf(TEXT("Exported heightmap %dx%d from '%s' as base64 PNG (%d bytes)"),
				W, H, *Landscape->GetActorLabel(), Base64Png.Len());
			return true;
		}
	});

	// ---- terrain_qa_metrics ----
	// Compute deterministic QA metrics for an existing landscape or a supplied heightmap.
	Registry.Register({
		TEXT("terrain_qa_metrics"),
		TEXT("Compute terrain QA metrics from a landscape actor or base64 heightmap. Returns height range, roughness, flat/cliff ratios, and pass/warn status for terrain planning receipts."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Optional landscape actor name or label"))},
			{TEXT("heightmap_base64"), FSololmcpSchemaBuilder::String(TEXT("Optional base64 PNG/R16 heightmap"))},
			{TEXT("format"), FSololmcpSchemaBuilder::String(TEXT("Heightmap format for heightmap_base64: PNG (default) or R16"))},
			{TEXT("width"), FSololmcpSchemaBuilder::Number(TEXT("Required for R16 heightmap_base64"))},
			{TEXT("height"), FSololmcpSchemaBuilder::Number(TEXT("Required for R16 heightmap_base64"))}
		}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			FString HeightmapB64;
			if (Arguments->TryGetStringField(TEXT("heightmap_base64"), HeightmapB64) && !HeightmapB64.IsEmpty())
			{
				FString Format = TEXT("PNG");
				Arguments->TryGetStringField(TEXT("format"), Format);

				TArray<uint8> RawBytes;
				if (!DecodeBase64(HeightmapB64, RawBytes))
				{
					OutError = TEXT("Failed to decode base64 heightmap data.");
					return false;
				}

				int32 W = 0;
				int32 H = 0;
				TArray<uint16> HeightData;
				if (Format.Equals(TEXT("R16"), ESearchCase::IgnoreCase))
				{
					double WD = 0.0;
					double HD = 0.0;
					Arguments->TryGetNumberField(TEXT("width"), WD);
					Arguments->TryGetNumberField(TEXT("height"), HD);
					W = static_cast<int32>(WD);
					H = static_cast<int32>(HD);
					if (W <= 0 || H <= 0 || RawBytes.Num() != W * H * 2)
					{
						OutError = TEXT("R16 terrain_qa_metrics requires width/height and exactly width*height*2 bytes.");
						return false;
					}
					HeightData.SetNumUninitialized(W * H);
					FMemory::Memcpy(HeightData.GetData(), RawBytes.GetData(), RawBytes.Num());
				}
				else if (!DecodePngHeightmap16(RawBytes, W, H, HeightData))
				{
					OutError = TEXT("Failed to decode PNG heightmap.");
					return false;
				}

				OutStructured->SetObjectField(TEXT("terrain_qa_metrics"), BuildTerrainQaMetrics(W, H, HeightData, TEXT("heightmap_base64")));
				OutSummary = FString::Printf(TEXT("Computed terrain QA metrics for supplied %dx%d heightmap."), W, H);
				return true;
			}

			FString LandscapeId;
			if (!Arguments->TryGetStringField(TEXT("landscape"), LandscapeId) || LandscapeId.IsEmpty())
			{
				OutError = TEXT("terrain_qa_metrics requires either landscape or heightmap_base64.");
				return false;
			}

			AActor* Actor = Context.Services.FindActorByLabelOrName(LandscapeId, OutError);
			if (!Actor)
			{
				return false;
			}
			ALandscape* Landscape = Cast<ALandscape>(Actor);
			if (!Landscape)
			{
				if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Actor))
				{
					Landscape = Proxy->GetLandscapeActor();
				}
			}
			if (!Landscape)
			{
				OutError = TEXT("Actor is not a landscape.");
				return false;
			}

			int32 W = 0;
			int32 H = 0;
			TArray<uint16> HeightData;
			if (!ExportHeightmapData(Landscape, W, H, HeightData, OutError))
			{
				return false;
			}
			OutStructured->SetStringField(TEXT("landscape"), Landscape->GetPathName());
			OutStructured->SetObjectField(TEXT("terrain_qa_metrics"), BuildTerrainQaMetrics(W, H, HeightData, TEXT("landscape")));
			OutSummary = FString::Printf(TEXT("Computed terrain QA metrics for landscape '%s' (%dx%d)."), *Landscape->GetActorLabel(), W, H);
			return true;
		}
	});

	// ---- landscape_split_to_tiles ----
	// Split an existing landscape into separate tile landscapes (for streaming)
	Registry.Register({
		TEXT("landscape_split_to_tiles"),
		TEXT("Split a landscape into NxM separate tile landscapes. Each tile becomes its own landscape actor, suitable for WorldPartition or level streaming."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor name or label"))},
			{TEXT("tiles_x"), FSololmcpSchemaBuilder::Number(TEXT("Number of tiles in X direction"))},
			{TEXT("tiles_y"), FSololmcpSchemaBuilder::Number(TEXT("Number of tiles in Y direction"))},
			{TEXT("overlap_quads"), FSololmcpSchemaBuilder::Number(TEXT("Overlap quads between tiles for seamless blending (default: 1)"))},
			{TEXT("prefix"), FSololmcpSchemaBuilder::String(TEXT("Name prefix for tiles (default: 'Tile')"))}
		}, {TEXT("landscape"), TEXT("tiles_x"), TEXT("tiles_y")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString LandscapeId = Arguments->GetStringField(TEXT("landscape"));
			AActor* Actor = Context.Services.FindActorByLabelOrName(LandscapeId, OutError);
			if (!Actor) return false;

			ALandscape* SourceLandscape = Cast<ALandscape>(Actor);
			if (!SourceLandscape && Cast<ALandscapeProxy>(Actor))
			{
				SourceLandscape = Cast<ALandscapeProxy>(Actor)->GetLandscapeActor();
			}
			if (!SourceLandscape)
			{
				OutError = TEXT("Actor is not a landscape.");
				return false;
			}

			const int32 TilesX = Arguments->GetIntegerField(TEXT("tiles_x"));
			const int32 TilesY = Arguments->GetIntegerField(TEXT("tiles_y"));
			if (TilesX <= 0 || TilesY <= 0 || TilesX > 64 || TilesY > 64)
			{
				OutError = TEXT("tiles_x and tiles_y must be between 1 and 64.");
				return false;
			}

			const int32 Overlap = Arguments->HasTypedField<EJson::Number>(TEXT("overlap_quads"))
				? Arguments->GetIntegerField(TEXT("overlap_quads")) : 1;
			const FString Prefix = Arguments->HasTypedField<EJson::String>(TEXT("prefix"))
				? Arguments->GetStringField(TEXT("prefix")) : TEXT("Tile");
			if (Overlap < 0)
			{
				OutError = TEXT("overlap_quads must be >= 0.");
				return false;
			}
			if (!IsSafeSimpleName(Prefix))
			{
				OutError = TEXT("prefix must be 1-64 chars and contain only letters, digits, '_' or '-'.");
				return false;
			}
			const FString LongestTileName = FString::Printf(TEXT("%s_%d_%d"), *Prefix, TilesX - 1, TilesY - 1);
			if (!IsSafeSimpleName(LongestTileName))
			{
				OutError = FString::Printf(TEXT("prefix is too long for generated tile names; longest would be '%s'."), *LongestTileName);
				return false;
			}

			// Export full heightmap
			ULandscapeInfo* LandscapeInfo = SourceLandscape->GetLandscapeInfo();
			if (!LandscapeInfo)
			{
				OutError = TEXT("Landscape info is unavailable.");
				return false;
			}

			const FIntRect FullBounds = SourceLandscape->GetBoundingRect();
			const int32 FullW = FullBounds.Width() + 1;
			const int32 FullH = FullBounds.Height() + 1;
			if (FullW <= 1 || FullH <= 1)
			{
				OutError = TEXT("Source landscape bounds are too small to split.");
				return false;
			}
			if (((FullW - 1) % TilesX) != 0 || ((FullH - 1) % TilesY) != 0)
			{
				OutError = FString::Printf(TEXT("Landscape quads (%dx%d) must divide evenly by tiles (%dx%d)."),
					FullW - 1, FullH - 1, TilesX, TilesY);
				return false;
			}

			TArray<uint16> FullHeightData;
			FullHeightData.SetNumZeroed(FullW * FullH);

			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			TArray<uint16> OneRow;
			OneRow.SetNumUninitialized(FullW);
			for (int32 Y = FullBounds.Min.Y; Y <= FullBounds.Max.Y; ++Y)
			{
				LandscapeEdit.GetHeightDataFast(FullBounds.Min.X, Y, FullBounds.Max.X, Y, OneRow.GetData(), 0);
				const int32 RowIdx = (Y - FullBounds.Min.Y) * FullW;
				FMemory::Memcpy(&FullHeightData[RowIdx], OneRow.GetData(), FullW * sizeof(uint16));
			}

			// Calculate tile dimensions
			const int32 TileQuadsX = (FullW - 1) / TilesX;
			const int32 TileQuadsY = (FullH - 1) / TilesY;
			if (TileQuadsX <= 0 || TileQuadsY <= 0)
			{
				OutError = TEXT("Calculated tile dimensions are invalid.");
				return false;
			}
			if (Overlap > TileQuadsX / 2 || Overlap > TileQuadsY / 2)
			{
				OutError = FString::Printf(TEXT("overlap_quads=%d is too large for tile size %dx%d quads."),
					Overlap, TileQuadsX, TileQuadsY);
				return false;
			}
			const int32 OverlapClamped = Overlap;

			const FVector SourceOrigin = SourceLandscape->GetActorLocation();
			const FVector SourceScale = SourceLandscape->GetActorScale3D();

			TArray<TSharedPtr<FJsonValue>> TilesJson;

			// Create each tile
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SplitLandscapeToTiles", "SOMOLMCP Split Landscape To Tiles"));

			for (int32 ty = 0; ty < TilesY; ++ty)
			{
				for (int32 tx = 0; tx < TilesX; ++tx)
				{
					// Calculate tile bounds in heightmap space
					const int32 StartX = tx * TileQuadsX - (tx > 0 ? OverlapClamped : 0);
					const int32 StartY = ty * TileQuadsY - (ty > 0 ? OverlapClamped : 0);
					const int32 EndX = (tx + 1) * TileQuadsX + (tx < TilesX - 1 ? OverlapClamped : 0);
					const int32 EndY = (ty + 1) * TileQuadsY + (ty < TilesY - 1 ? OverlapClamped : 0);

					const int32 TileW = EndX - StartX + 1;
					const int32 TileH = EndY - StartY + 1;
					if (TileW <= 1 || TileH <= 1)
					{
						OutError = FString::Printf(TEXT("Calculated invalid tile size %dx%d for tile %d,%d."), TileW, TileH, tx, ty);
						return false;
					}

					TArray<uint16> TileData;
					TileData.SetNumZeroed(TileW * TileH);

					for (int32 y = 0; y < TileH; ++y)
					{
						for (int32 x = 0; x < TileW; ++x)
						{
							const int32 SrcX = FMath::Clamp(StartX + x, 0, FullW - 1);
							const int32 SrcY = FMath::Clamp(StartY + y, 0, FullH - 1);
							TileData[y * TileW + x] = FullHeightData[SrcY * FullW + SrcX];
						}
					}

					// World offset for this tile
					const FVector TileOrigin = SourceOrigin + FVector(
						static_cast<float>(StartX) * SourceScale.X,
						static_cast<float>(StartY) * SourceScale.Y,
						0.0f
					);

					// Create tile landscape via Python
					const int32 QPC = FMath::Clamp(TileQuadsX, 1, 255);

					FString TilePythonCode = TEXT("import unreal\n");
					TilePythonCode += TEXT("landscape = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.Landscape, unreal.Vector(");
					TilePythonCode += FString::Printf(TEXT("%.2f, %.2f, 0.0))\n"), TileOrigin.X, TileOrigin.Y);
					TilePythonCode += TEXT("if landscape:\n");
					TilePythonCode += TEXT("    print('SOMO_TILE_CREATE_OK:' + landscape.get_path_name())\n");
					TilePythonCode += TEXT("else:\n");
					TilePythonCode += TEXT("    print('SOMO_TILE_CREATE_FAILED')\n");

					TSharedRef<FJsonObject> TileOutStruct = MakeShared<FJsonObject>();
					FString TileSummary, TileError;
					if (!Context.Services.ExecutePython(TilePythonCode, TEXT("ExecuteFile"), true, TileOutStruct, TileSummary, TileError) ||
						!TileSummary.Contains(TEXT("SOMO_TILE_CREATE_OK:")))
					{
						OutError = FString::Printf(TEXT("Failed to create tile %d,%d. Python output: %s Error: %s"),
							tx, ty, *TileSummary, *TileError);
						return false;
					}

					ALandscape* TileLandscape = nullptr;
					UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
					if (World)
					{
						// Find most recently spawned landscape
						for (TActorIterator<ALandscape> It(World); It; ++It)
						{
							TileLandscape = *It;
						}
					}

					if (TileLandscape)
					{
						TileLandscape->SetActorScale3D(SourceScale);
						const FString TileName = FString::Printf(TEXT("%s_%d_%d"), *Prefix, tx, ty);
						if (!IsSafeSimpleName(TileName))
						{
							OutError = FString::Printf(TEXT("Generated unsafe tile name: %s"), *TileName);
							return false;
						}
						TileLandscape->SetActorLabel(TileName);

						TSharedRef<FJsonObject> TileJson = MakeShared<FJsonObject>();
						TileJson->SetStringField(TEXT("name"), TileName);
						TileJson->SetStringField(TEXT("path"), TileLandscape->GetPathName());
						TileJson->SetNumberField(TEXT("tile_x"), tx);
						TileJson->SetNumberField(TEXT("tile_y"), ty);

						TSharedRef<FJsonObject> LocJson = MakeShared<FJsonObject>();
						const FVector Loc = TileLandscape->GetActorLocation();
						LocJson->SetNumberField(TEXT("x"), Loc.X);
						LocJson->SetNumberField(TEXT("y"), Loc.Y);
						LocJson->SetNumberField(TEXT("z"), Loc.Z);
						TileJson->SetObjectField(TEXT("location"), LocJson);

						TilesJson.Add(MakeShared<FJsonValueObject>(TileJson));
					}
					else
					{
						OutError = FString::Printf(TEXT("Tile %d,%d creation reported success but no landscape actor was found."), tx, ty);
						return false;
					}
				}
			}

			if (TilesJson.Num() != TilesX * TilesY)
			{
				OutError = FString::Printf(TEXT("Created %d tiles but expected %d."), TilesJson.Num(), TilesX * TilesY);
				return false;
			}

			const FString SourcePath = SourceLandscape->GetPathName();
			if (!SourceLandscape->Destroy())
			{
				OutError = FString::Printf(TEXT("All tiles were created, but failed to delete source landscape '%s'."), *SourcePath);
				return false;
			}

			OutStructured->SetArrayField(TEXT("tiles"), TilesJson);
			OutStructured->SetStringField(TEXT("deleted_source"), SourcePath);
			OutStructured->SetNumberField(TEXT("tiles_x"), TilesX);
			OutStructured->SetNumberField(TEXT("tiles_y"), TilesY);
			OutStructured->SetNumberField(TEXT("tile_quads_x"), TileQuadsX);
			OutStructured->SetNumberField(TEXT("tile_quads_y"), TileQuadsY);

			OutSummary = FString::Printf(TEXT("Split landscape into %d x %d = %d tiles (%dx%d quads each, %d overlap)"),
				TilesX, TilesY, TilesX * TilesY, TileQuadsX, TileQuadsY, OverlapClamped);
			return true;
		},
	nullptr,
	0,
	nullptr,
	true
	});

	// ---- landscape_heightmap_from_noise ----
	// Generate a heightmap using Perlin noise parameters
	Registry.Register({
		TEXT("landscape_heightmap_from_noise"),
		TEXT("Generate heightmap data using procedural noise (Perlin-like). Returns base64 PNG and raw height values for use with landscape_create_from_heightmap."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("width"), FSololmcpSchemaBuilder::Number(TEXT("Heightmap width in pixels (e.g., 512)"))},
			{TEXT("height"), FSololmcpSchemaBuilder::Number(TEXT("Heightmap height in pixels (e.g., 512)"))},
			{TEXT("noise_scale"), FSololmcpSchemaBuilder::Number(TEXT("Noise frequency scale (default: 0.01)"))},
			{TEXT("octaves"), FSololmcpSchemaBuilder::Number(TEXT("Number of noise octaves for fractal detail (default: 6)"))},
			{TEXT("seed"), FSololmcpSchemaBuilder::Number(TEXT("Random seed (default: 42)"))},
			{TEXT("height_min"), FSololmcpSchemaBuilder::Number(TEXT("Minimum height value 0-65535 (default: 8000)"))},
			{TEXT("height_max"), FSololmcpSchemaBuilder::Number(TEXT("Maximum height value 0-65535 (default: 30000)"))},
			{TEXT("export_png"), FSololmcpSchemaBuilder::Boolean(TEXT("Export as base64 PNG (default: true)"))},
			{TEXT("terrain_constraint_proof"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Terrain Spec IR / pre_generation_constraints / constrained_heightmap_recipe proof."))},
			{TEXT("constrained_heightmap_recipe"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional explicit constrained heightmap recipe."))}
		}, {TEXT("width"), TEXT("height")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const int32 W = Arguments->GetIntegerField(TEXT("width"));
			const int32 H = Arguments->GetIntegerField(TEXT("height"));
			if (W <= 0 || H <= 0 || W > 8192 || H > 8192)
			{
				OutError = TEXT("width/height must be between 1 and 8192.");
				return false;
			}

			const float NoiseScale = Arguments->HasTypedField<EJson::Number>(TEXT("noise_scale"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("noise_scale"))) : 0.01f;
			const int32 Octaves = Arguments->HasTypedField<EJson::Number>(TEXT("octaves"))
				? Arguments->GetIntegerField(TEXT("octaves")) : 6;
			const int32 Seed = Arguments->HasTypedField<EJson::Number>(TEXT("seed"))
				? Arguments->GetIntegerField(TEXT("seed")) : 42;
			const uint16 HMin = Arguments->HasTypedField<EJson::Number>(TEXT("height_min"))
				? static_cast<uint16>(Arguments->GetIntegerField(TEXT("height_min")) / 256) : 32;
			const uint16 HMax = Arguments->HasTypedField<EJson::Number>(TEXT("height_max"))
				? static_cast<uint16>(Arguments->GetIntegerField(TEXT("height_max")) / 256) : 117;
			const bool bExportPng = Arguments->HasTypedField<EJson::Boolean>(TEXT("export_png"))
				? Arguments->GetBoolField(TEXT("export_png")) : true;

			// Simple value noise implementation
			auto Hash = [](int32 X, int32 Y, int32 S) -> float
			{
				int32 H = S + X * 374761393 + Y * 668265263;
				H = (H ^ (H >> 13)) * 1274126177;
				H = H ^ (H >> 16);
				return (static_cast<float>(H & 0x7fffffff) / 2147483647.0f);
			};

			auto SmoothNoise = [&Hash](float X, float Y, int32 S) -> float
			{
				const int32 IX = FMath::FloorToInt(X);
				const int32 IY = FMath::FloorToInt(Y);
				const float FX = X - IX;
				const float FY = Y - IY;
				// Smoothstep
				const float SX = FX * FX * (3.0f - 2.0f * FX);
				const float SY = FY * FY * (3.0f - 2.0f * FY);

				const float V00 = Hash(IX, IY, S);
				const float V10 = Hash(IX + 1, IY, S);
				const float V01 = Hash(IX, IY + 1, S);
				const float V11 = Hash(IX + 1, IY + 1, S);

				const float I0 = V00 * (1 - SX) + V10 * SX;
				const float I1 = V01 * (1 - SX) + V11 * SX;
				return I0 * (1 - SY) + I1 * SY;
			};

			TArray<uint8> HeightGray8;
			HeightGray8.SetNumUninitialized(W * H);

			for (int32 y = 0; y < H; ++y)
			{
				for (int32 x = 0; x < W; ++x)
				{
					float Noise = 0.0f;
					float Amplitude = 1.0f;
					float Frequency = NoiseScale;
					float TotalAmp = 0.0f;

					for (int32 o = 0; o < Octaves; ++o)
					{
						Noise += SmoothNoise(x * Frequency, y * Frequency, Seed + o * 1337) * Amplitude;
						TotalAmp += Amplitude;
						Amplitude *= 0.5f;
						Frequency *= 2.0f;
					}

					Noise = (Noise / TotalAmp); // Normalize to [0, 1]
					const uint8 Val = static_cast<uint8>(FMath::Clamp(HMin + Noise * (HMax - HMin), 0.0f, 255.0f));
					HeightGray8[y * W + x] = Val;
				}
			}

			const bool bTerrainConstraintApplied = HasTerrainConstraintProofForHeightmap(Arguments);
			FString Landform = TEXT("generic");
			int32 SmoothPasses = 2;
			int32 MaxDelta = 18;
			if (bTerrainConstraintApplied)
			{
				// audit-U9 fix (P2): the previous version lowered the entire
				// arguments JSON to a string and substring-matched
				// "plain"/"mountain"/"lake"/"hill". False positives:
				//   - identifiers like "id":"test_plain_001" → matched "plain"
				//   - negations like "no mountain" → matched "mountain"
				//   - asset paths like "/Game/Mountain/M_Foo" → matched "mountain"
				// Result: wrong landform recipe → wrong smoothing/slope clamp
				// → wrong terrain shape, no error to the caller. Now: read a
				// DEDICATED `landform` (or `biome`) field. Substring scan kept
				// as a documented FALLBACK only when neither field is present
				// so existing plans keep working.
				FString LandformField;
				if (!Arguments->TryGetStringField(TEXT("landform"), LandformField))
				{
					Arguments->TryGetStringField(TEXT("biome"), LandformField);
				}
				LandformField = LandformField.ToLower().TrimStartAndEnd();

				auto MatchKeyword = [](const FString& Source, std::initializer_list<const TCHAR*> Words) {
					for (const TCHAR* W : Words) { if (Source.Contains(W)) return true; }
					return false;
				};
				const bool bUseExplicit = !LandformField.IsEmpty();
				const FString FallbackJson = bUseExplicit
					? FString()
					: SerializeArgsForTerrainTokens(Arguments);
				auto Hits = [&](std::initializer_list<const TCHAR*> Words) {
					return bUseExplicit
						? MatchKeyword(LandformField, Words)
						: MatchKeyword(FallbackJson, Words);
				};

				if (Hits({ TEXT("plain"), TEXT("平原") }))
				{
					Landform = TEXT("plain");
					SmoothPasses = 5;
					MaxDelta = 3;
				}
				else if (Hits({ TEXT("hill"), TEXT("rolling"), TEXT("丘陵"), TEXT("缓坡") }))
				{
					Landform = TEXT("hills");
					SmoothPasses = 4;
					MaxDelta = 8;
				}
				else if (Hits({ TEXT("lake"), TEXT("pond"), TEXT("湖") }))
				{
					Landform = TEXT("lake");
					SmoothPasses = 3;
					MaxDelta = 10;
				}
				else if (Hits({ TEXT("mountain"), TEXT("ridge"), TEXT("山地"), TEXT("高山") }))
				{
					Landform = TEXT("mountain");
					SmoothPasses = 1;
					MaxDelta = 36;
				}
				SmoothGrayHeightmap(HeightGray8, W, H, SmoothPasses);
				ClampGraySlope(HeightGray8, W, H, MaxDelta, 2);
			}

			if (bExportPng)
			{
				IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
				TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
				if (ImageWrapper.IsValid())
				{
					ImageWrapper->SetRaw(HeightGray8.GetData(), HeightGray8.Num(), W, H, ERGBFormat::Gray, 8);
					const TArray64<uint8> PngData64 = ImageWrapper->GetCompressed();
					TArray<uint8> PngData;
					PngData.SetNumUninitialized(PngData64.Num());
					FMemory::Memcpy(PngData.GetData(), PngData64.GetData(), PngData64.Num());
					OutStructured->SetStringField(TEXT("image_base64"), FBase64::Encode(PngData));
				}
			}

			OutStructured->SetNumberField(TEXT("width"), W);
			OutStructured->SetNumberField(TEXT("height"), H);
			OutStructured->SetStringField(TEXT("format"), TEXT("PNG_8bit"));
			OutStructured->SetStringField(TEXT("parameters"), FString::Printf(TEXT("scale=%.4f, octaves=%d, seed=%d"), NoiseScale, Octaves, Seed));
			OutStructured->SetBoolField(TEXT("terrain_constraint_applied"), bTerrainConstraintApplied);
			TSharedRef<FJsonObject> RecipeJson = MakeShared<FJsonObject>();
			RecipeJson->SetStringField(TEXT("landform"), Landform);
			RecipeJson->SetNumberField(TEXT("smoothing_passes"), SmoothPasses);
			RecipeJson->SetNumberField(TEXT("max_delta_u8"), MaxDelta);
			RecipeJson->SetBoolField(TEXT("clamp_slope"), bTerrainConstraintApplied);
			RecipeJson->SetStringField(TEXT("source"), TEXT("landscape_heightmap_from_noise"));
			OutStructured->SetObjectField(TEXT("constrained_heightmap_recipe"), RecipeJson);

			OutSummary = FString::Printf(TEXT("Generated noise heightmap %dx%d (scale=%.4f, octaves=%d, seed=%d, constrained=%d)"),
				W, H, NoiseScale, Octaves, Seed, bTerrainConstraintApplied ? 1 : 0);
			return true;
		}
	});

	// ---- landscape_heightmap_blend ----
	// Blend two heightmaps together with configurable mix factor
	Registry.Register({
		TEXT("landscape_heightmap_blend"),
		TEXT("Blend a heightmap patch onto an existing landscape with configurable opacity/falloff. Useful for terrain editing and path carving."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Target landscape actor name"))},
			{TEXT("patch_base64"), FSololmcpSchemaBuilder::String(TEXT("Base64 PNG heightmap patch"))},
			{TEXT("blend_factor"), FSololmcpSchemaBuilder::Number(TEXT("Blend strength 0.0-1.0 (default: 0.5)"))},
			{TEXT("world_center_x"), FSololmcpSchemaBuilder::Number(TEXT("World X center for patch application"))},
			{TEXT("world_center_y"), FSololmcpSchemaBuilder::Number(TEXT("World Y center for patch application"))},
			{TEXT("blend_mode"), FSololmcpSchemaBuilder::String(TEXT("Blend mode: 'add', 'subtract', 'replace', 'multiply' (default: 'add')"))}
		}, {TEXT("landscape"), TEXT("patch_base64"), TEXT("world_center_x"), TEXT("world_center_y")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			// Resolve landscape
			AActor* Actor = Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("landscape")), OutError);
			if (!Actor) return false;
			ALandscape* Landscape = Cast<ALandscape>(Actor);
			if (!Landscape) { OutError = TEXT("Not a landscape."); return false; }

			// Decode patch
			TArray<uint8> PngBytes;
			if (!DecodeBase64(Arguments->GetStringField(TEXT("patch_base64")), PngBytes))
			{
				OutError = TEXT("Failed to decode patch base64.");
				return false;
			}

			int32 PatchW, PatchH;
			TArray<uint16> PatchData;
			if (!DecodePngHeightmap16(PngBytes, PatchW, PatchH, PatchData))
			{
				OutError = TEXT("Failed to decode patch PNG.");
				return false;
			}

			// Get landscape info
			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (!LandscapeInfo) { OutError = TEXT("Landscape info unavailable."); return false; }

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "BlendHeightmap", "SOMOLMCP Blend Heightmap"));

			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			const FVector Scale = Landscape->GetActorScale3D();
			const float BlendFactor = Arguments->HasTypedField<EJson::Number>(TEXT("blend_factor"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("blend_factor"))) : 0.5f;
			const FString BlendMode = Arguments->HasTypedField<EJson::String>(TEXT("blend_mode"))
				? Arguments->GetStringField(TEXT("blend_mode")).ToLower() : TEXT("add");

			const double CenterX = Arguments->GetNumberField(TEXT("world_center_x"));
			const double CenterY = Arguments->GetNumberField(TEXT("world_center_y"));

			const FIntRect Bounds = Landscape->GetBoundingRect();
			const FVector LocalCenter = Landscape->GetTransform().InverseTransformPosition(FVector(CenterX, CenterY, 0));
			const int32 OriginX = FMath::RoundToInt(LocalCenter.X) - PatchW / 2;
			const int32 OriginY = FMath::RoundToInt(LocalCenter.Y) - PatchH / 2;

			int32 ModifiedCount = 0;
			TArray<uint16> SrcRow, DstRow;
			SrcRow.SetNumUninitialized(PatchW);
			DstRow.SetNumUninitialized(PatchW);

			for (int32 Py = 0; Py < PatchH; ++Py)
			{
				const int32 WorldY = OriginY + Py;
				if (WorldY < Bounds.Min.Y || WorldY > Bounds.Max.Y) continue;

				for (int32 Px = 0; Px < PatchW; ++Px)
				{
					const int32 WorldX = OriginX + Px;
					if (WorldX < Bounds.Min.X || WorldX > Bounds.Max.X) continue;

					// Get current height
					LandscapeEdit.GetHeightDataFast(WorldX, WorldY, WorldX, WorldY, &DstRow[0], 0);
					const uint16 CurrentH = DstRow[0];
					const uint16 PatchH_val = PatchData[Py * PatchW + Px];

					uint16 NewH;
					if (BlendMode == TEXT("add"))
					{
						NewH = static_cast<uint16>(FMath::Clamp(static_cast<float>(CurrentH) + PatchH_val * BlendFactor, 0.0f, 65535.0f));
					}
					else if (BlendMode == TEXT("subtract"))
					{
						NewH = static_cast<uint16>(FMath::Clamp(static_cast<float>(CurrentH) - PatchH_val * BlendFactor, 0.0f, 65535.0f));
					}
					else if (BlendMode == TEXT("replace"))
					{
						NewH = static_cast<uint16>(FMath::Lerp(static_cast<float>(CurrentH), static_cast<float>(PatchH_val), BlendFactor));
					}
					else // multiply
					{
						NewH = static_cast<uint16>(FMath::Clamp(static_cast<float>(CurrentH) * (PatchH_val / 65535.0f * BlendFactor + (1.0f - BlendFactor)), 0.0f, 65535.0f));
					}

					SrcRow[0] = NewH;
					LandscapeEdit.SetHeightData(WorldX, WorldY, WorldX, WorldY, SrcRow.GetData(), 0, true);
					++ModifiedCount;
				}
			}

			OutStructured->SetStringField(TEXT("landscape"), Landscape->GetPathName());
			OutStructured->SetNumberField(TEXT("modified_points"), ModifiedCount);
			OutStructured->SetStringField(TEXT("blend_mode"), BlendMode);
			OutStructured->SetNumberField(TEXT("blend_factor"), BlendFactor);

			OutSummary = FString::Printf(TEXT("Blended heightmap patch (%dx%d) onto '%s': %d points modified, mode=%s"),
				PatchW, PatchH, *Landscape->GetActorLabel(), ModifiedCount, *BlendMode);
			return true;
		}
	});

	// ---- landscape_apply_heightmap_patch ----
	// Apply a raw heightmap patch directly to a region of a landscape
	Registry.Register({
		TEXT("landscape_apply_heightmap_patch"),
		TEXT("Apply raw height data to a specific region of a landscape. Takes base64-encoded PNG heightmap and writes it to the specified region."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Target landscape actor name"))},
			{TEXT("heightmap_base64"), FSololmcpSchemaBuilder::String(TEXT("Base64 PNG heightmap to apply"))},
			{TEXT("region_world_min_x"), FSololmcpSchemaBuilder::Number(TEXT("Region min X in world cm"))},
			{TEXT("region_world_min_y"), FSololmcpSchemaBuilder::Number(TEXT("Region min Y in world cm"))}
		}, {TEXT("landscape"), TEXT("heightmap_base64"), TEXT("region_world_min_x"), TEXT("region_world_min_y")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			AActor* Actor = Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("landscape")), OutError);
			if (!Actor) return false;
			ALandscape* Landscape = Cast<ALandscape>(Actor);
			if (!Landscape) { OutError = TEXT("Not a landscape."); return false; }

			TArray<uint8> PngBytes;
			if (!DecodeBase64(Arguments->GetStringField(TEXT("heightmap_base64")), PngBytes))
			{
				OutError = TEXT("Failed to decode heightmap base64.");
				return false;
			}

			int32 PatchW, PatchH;
			TArray<uint16> PatchData;
			if (!DecodePngHeightmap16(PngBytes, PatchW, PatchH, PatchData))
			{
				OutError = TEXT("Failed to decode PNG heightmap.");
				return false;
			}

			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (!LandscapeInfo) { OutError = TEXT("Landscape info unavailable."); return false; }

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ApplyHeightmapPatch", "SOMOLMCP Apply Heightmap Patch"));

			const FVector Scale = Landscape->GetActorScale3D();
			const FVector LocalMin(
				Arguments->GetNumberField(TEXT("region_world_min_x")),
				Arguments->GetNumberField(TEXT("region_world_min_y")),
				0.0f
			);
			const FVector LocalOrigin = Landscape->GetTransform().InverseTransformPosition(LocalMin);
			const int32 OriginX = FMath::RoundToInt(LocalOrigin.X);
			const int32 OriginY = FMath::RoundToInt(LocalOrigin.Y);

			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			const FIntRect Bounds = Landscape->GetBoundingRect();

			int32 Written = 0;
			TArray<uint16> Row;
			Row.SetNumUninitialized(1);

			for (int32 y = 0; y < PatchH; ++y)
			{
				for (int32 x = 0; x < PatchW; ++x)
				{
					const int32 WX = OriginX + x;
					const int32 WY = OriginY + y;
					if (WX < Bounds.Min.X || WX > Bounds.Max.X || WY < Bounds.Min.Y || WY > Bounds.Max.Y) continue;

					Row[0] = PatchData[y * PatchW + x];
					LandscapeEdit.SetHeightData(WX, WY, WX, WY, Row.GetData(), 0, true);
					++Written;
				}
			}

			OutStructured->SetStringField(TEXT("landscape"), Landscape->GetPathName());
			OutStructured->SetNumberField(TEXT("written_points"), Written);
			OutStructured->SetNumberField(TEXT("patch_width"), PatchW);
			OutStructured->SetNumberField(TEXT("patch_height"), PatchH);

			OutSummary = FString::Printf(TEXT("Applied heightmap patch %dx%d to '%s': %d points written"),
				PatchW, PatchH, *Landscape->GetActorLabel(), Written);
			return true;
		}
	});

	// ---- landscape_mirror ----
	// Mirror a landscape along X or Y axis
	Registry.Register({
		TEXT("landscape_mirror"),
		TEXT("Mirror a landscape along the X or Y axis. Creates a symmetrical copy."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor name"))},
			{TEXT("axis"), FSololmcpSchemaBuilder::String(TEXT("Mirror axis: 'X' or 'Y' (default: 'X')"))},
			{TEXT("merge_mode"), FSololmcpSchemaBuilder::String(TEXT("'replace' (default) or 'max' — how to handle overlapping regions"))}
		}, {TEXT("landscape")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			AActor* Actor = Context.Services.FindActorByLabelOrName(Arguments->GetStringField(TEXT("landscape")), OutError);
			if (!Actor) return false;
			ALandscape* Landscape = Cast<ALandscape>(Actor);
			if (!Landscape) { OutError = TEXT("Not a landscape."); return false; }

			ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
			if (!LandscapeInfo) { OutError = TEXT("Landscape info unavailable."); return false; }

			const FString Axis = Arguments->HasTypedField<EJson::String>(TEXT("axis"))
				? Arguments->GetStringField(TEXT("axis")).ToUpper() : TEXT("X");
			const FString MergeMode = Arguments->HasTypedField<EJson::String>(TEXT("merge_mode"))
				? Arguments->GetStringField(TEXT("merge_mode")).ToLower() : TEXT("replace");

			if (Axis != TEXT("X") && Axis != TEXT("Y"))
			{
				OutError = TEXT("axis must be 'X' or 'Y'.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "MirrorLandscape", "SOMOLMCP Mirror Landscape"));

			const FIntRect Bounds = Landscape->GetBoundingRect();
			const int32 W = Bounds.Width() + 1;
			const int32 H = Bounds.Height() + 1;

			// Read all height data
			TArray<uint16> HeightData;
			HeightData.SetNumZeroed(W * H);
			FLandscapeEditDataInterface LandscapeEdit(LandscapeInfo);
			TArray<uint16> Row;
			Row.SetNumUninitialized(W);
			for (int32 Y = Bounds.Min.Y; Y <= Bounds.Max.Y; ++Y)
			{
				LandscapeEdit.GetHeightDataFast(Bounds.Min.X, Y, Bounds.Max.X, Y, Row.GetData(), 0);
				FMemory::Memcpy(&HeightData[(Y - Bounds.Min.Y) * W], Row.GetData(), W * sizeof(uint16));
			}

			// Apply mirrored data
			int32 Modified = 0;
			TArray<uint16> SingleVal;
			SingleVal.SetNumUninitialized(1);

			for (int32 y = 0; y < H; ++y)
			{
				for (int32 x = 0; x < W; ++x)
				{
					int32 SrcX, SrcY;
					if (Axis == TEXT("X"))
					{
						SrcX = W - 1 - x;
						SrcY = y;
					}
					else
					{
						SrcX = x;
						SrcY = H - 1 - y;
					}

					const uint16 MirrorVal = HeightData[SrcY * W + SrcX];

					if (MergeMode == TEXT("max"))
					{
						const uint16 CurrentVal = HeightData[y * W + x];
						SingleVal[0] = FMath::Max(CurrentVal, MirrorVal);
					}
					else
					{
						SingleVal[0] = MirrorVal;
					}

					const int32 WX = Bounds.Min.X + x;
					const int32 WY = Bounds.Min.Y + y;
					LandscapeEdit.SetHeightData(WX, WY, WX, WY, SingleVal.GetData(), 0, true);
					++Modified;
				}
			}

			OutStructured->SetStringField(TEXT("landscape"), Landscape->GetPathName());
			OutStructured->SetStringField(TEXT("axis"), Axis);
			OutStructured->SetNumberField(TEXT("modified_points"), Modified);

			OutSummary = FString::Printf(TEXT("Mirrored landscape '%s' along %s axis (%d points modified)"),
				*Landscape->GetActorLabel(), *Axis, Modified);
			return true;
		}
	});

	// ---- landscape_set_material ----
	// Set a material on a landscape
	Registry.Register({
		TEXT("landscape_set_material"),
		TEXT("Set a material on a landscape actor. Replaces the landscape material with the specified material asset. Accepts 'landscape' or 'landscape_name' for the actor lookup."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape_name"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor name or label"))},
			{TEXT("landscape"), FSololmcpSchemaBuilder::String(TEXT("Alias for landscape_name (matches naming used by other landscape_* tools)"))},
			{TEXT("material_path"), FSololmcpSchemaBuilder::String(TEXT("Material instance asset path"))}
		}, {TEXT("material_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			// FIX (v12-rd2): accept both 'landscape_name' (original) and 'landscape'
			// (consistent with landscape_noise_height_region, landscape_get_*, etc.)
			FString LandscapeName;
			if (!Arguments->TryGetStringField(TEXT("landscape_name"), LandscapeName))
			{
				Arguments->TryGetStringField(TEXT("landscape"), LandscapeName);
			}
			if (LandscapeName.IsEmpty())
			{
				OutError = TEXT("Missing required: landscape_name (or landscape)");
				return false;
			}
			const FString MaterialPath = Arguments->GetStringField(TEXT("material_path"));

			AActor* Actor = Context.Services.FindActorByLabelOrName(LandscapeName, OutError);
			if (!Actor) return false;
			ALandscape* Landscape = Cast<ALandscape>(Actor);
			if (!Landscape) { OutError = TEXT("Not a landscape."); return false; }

			FString MatError;
			UMaterialInterface* Material = Cast<UMaterialInterface>(Context.Services.LoadAsset(MaterialPath, MatError));
			if (!Material)
			{
				OutError = FString::Printf(TEXT("Material not found: %s"), *MaterialPath);
				return false;
			}

			Landscape->LandscapeMaterial = Material;
			Landscape->PostEditChange();

			OutStructured->SetStringField(TEXT("landscape_name"), Landscape->GetActorLabel());
			OutStructured->SetStringField(TEXT("material_path"), MaterialPath);
			OutSummary = FString::Printf(TEXT("Set material '%s' on landscape '%s'"), *MaterialPath, *Landscape->GetActorLabel());
			return true;
		}
	});

	// ---- landscape_export_heightmap ----
	// Export landscape heightmap as base64 PNG (convenience alias with simpler interface)
	Registry.Register({
		TEXT("landscape_export_heightmap"),
		TEXT("Export the current landscape heightmap as a base64-encoded PNG image. Simpler interface than landscape_get_heightmap_base64."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("landscape_name"), FSololmcpSchemaBuilder::String(TEXT("Landscape actor name (optional, defaults to first found)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			ALandscape* Landscape = nullptr;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			if (Arguments->HasTypedField<EJson::String>(TEXT("landscape_name")))
			{
				const FString Name = Arguments->GetStringField(TEXT("landscape_name"));
				AActor* Actor = Context.Services.FindActorByLabelOrName(Name, OutError);
				if (!Actor) return false;
				Landscape = Cast<ALandscape>(Actor);
			}
			else
			{
				for (TActorIterator<ALandscape> It(World); It; ++It)
				{
					Landscape = *It;
					break;
				}
			}

			if (!Landscape)
			{
				OutError = TEXT("No landscape found.");
				return false;
			}

			int32 W, H;
			TArray<uint16> HeightData;
			if (!ExportHeightmapData(Landscape, W, H, HeightData, OutError))
			{
				return false;
			}

			const FString Base64Png = EncodeHeightmapToPngBase64(W, H, HeightData);

			OutStructured->SetStringField(TEXT("heightmap_base64"), Base64Png);
			OutStructured->SetNumberField(TEXT("width"), W);
			OutStructured->SetNumberField(TEXT("height"), H);
			OutStructured->SetStringField(TEXT("landscape_name"), Landscape->GetActorLabel());
			OutSummary = FString::Printf(TEXT("Exported heightmap %dx%d from '%s' as base64 PNG"), W, H, *Landscape->GetActorLabel());
			return true;
		}
	});

	// ─── Task 2: Level / SubLevel Management ────────────────────────────────

	// ---- level_create ----
	// Create a new persistent level asset
	Registry.Register({
		TEXT("level_create"),
		TEXT("Create a new persistent level (.umap) asset. Can optionally make it a streaming sub-level of the current level."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Level name (without extension)"))},
			{TEXT("folder"), FSololmcpSchemaBuilder::String(TEXT("Folder path for the new level asset (e.g., '/Game/Levels')"))},
			{TEXT("make_streaming"), FSololmcpSchemaBuilder::Boolean(TEXT("Add as streaming sub-level to current persistent level"))},
			{TEXT("streaming_offset"), FSololmcpSchemaBuilder::Object({
				{TEXT("x"), FSololmcpSchemaBuilder::Number()},
				{TEXT("y"), FSololmcpSchemaBuilder::Number()},
				{TEXT("z"), FSololmcpSchemaBuilder::Number()}
			})}
		}, {TEXT("name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			FString InputName = Arguments->GetStringField(TEXT("name"));
			if (InputName.Contains(TEXT(".")))
			{
				InputName = FPaths::GetBaseFilename(InputName);
			}
			InputName = InputName.TrimStartAndEnd();
			if (InputName.IsEmpty())
			{
				OutError = TEXT("name must not be empty.");
				return false;
			}

			// FIX (v12): if name is a full /Game/... path, treat it as the
			// authoritative location (split into folder+leaf). Old behavior
			// always glued name onto Folder so '/Game/__v10/MainMap' became
			// '/Game/Levels//Game/__v10/MainMap' (broken double-slash path).
			FString Folder;
			FString LevelName;
			if (InputName.StartsWith(TEXT("/Game/")))
			{
				int32 LastSlash = INDEX_NONE;
				if (InputName.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
				{
					Folder = InputName.Left(LastSlash);
					LevelName = InputName.Mid(LastSlash + 1);
				}
				else
				{
					Folder = TEXT("/Game");
					LevelName = InputName;
				}
			}
			else
			{
				LevelName = InputName;
				Folder = TEXT("/Game/Levels");
				if (Arguments->HasTypedField<EJson::String>(TEXT("folder")))
				{
					Folder = Arguments->GetStringField(TEXT("folder")).TrimStartAndEnd();
					if (!Folder.StartsWith(TEXT("/")))
					{
						Folder = TEXT("/") + Folder;
					}
				}
			}

			const FString PackagePath = Folder + TEXT("/") + LevelName;
			const FString AssetPath = PackagePath + TEXT(".") + LevelName;
			if (!IsSafeSimpleName(LevelName))
			{
				OutError = TEXT("Level name must be 1-64 chars and contain only letters, digits, '_' or '-'.");
				return false;
			}
			if (!Folder.StartsWith(TEXT("/Game/")) && Folder != TEXT("/Game"))
			{
				OutError = TEXT("folder must be /Game or a /Game/... long package folder.");
				return false;
			}
			if (!FPackageName::IsValidLongPackageName(PackagePath))
			{
				OutError = FString::Printf(TEXT("Invalid level package path: %s"), *PackagePath);
				return false;
			}
			UWorld* CurrentWorldForSwitch = Context.Services.GetEditorWorld(OutError);
			const FString CurrentWorldPackage = CurrentWorldForSwitch && CurrentWorldForSwitch->GetOutermost()
				? CurrentWorldForSwitch->GetOutermost()->GetName() : FString();
			if (!CurrentWorldForSwitch || CurrentWorldPackage.StartsWith(TEXT("/Temp/")))
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("BLOCKED_TRANSIENT_CURRENT_WORLD"));
				OutStructured->SetStringField(TEXT("current_world_package"), CurrentWorldPackage);
				OutError = TEXT("Cannot create or switch levels while the current editor world is transient. Save or reopen a persistent /Game map first.");
				return false;
			}
			if (CurrentWorldForSwitch->GetOutermost()->IsDirty())
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("BLOCKED_DIRTY_CURRENT_WORLD"));
				OutStructured->SetStringField(TEXT("current_world_package"), CurrentWorldPackage);
				OutError = TEXT("Cannot create or switch levels while the current world package is dirty. Save it first.");
				return false;
			}

			ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(OutError);
			if (!LevelSubsystem)
			{
				return false;
			}

			const bool bMakeStreaming = Arguments->HasTypedField<EJson::Boolean>(TEXT("make_streaming"))
				&& Arguments->GetBoolField(TEXT("make_streaming"));
			const FString OriginalWorldPath = CurrentWorldPackage;

			if (Context.Services.AssetExists(AssetPath) || FPackageName::DoesPackageExist(PackagePath))
			{
				OutError = FString::Printf(TEXT("A level already exists at '%s'."), *PackagePath);
				return false;
			}
			ULevelStreaming* CreatedStreamingLevel = nullptr;
			if (bMakeStreaming)
			{
				FVector Offset(0, 0, 0);
				if (const TSharedPtr<FJsonObject>* OffObj = nullptr; Arguments->TryGetObjectField(TEXT("streaming_offset"), OffObj) && OffObj)
				{
					FSololmcpEditorServices::JsonToVector(*OffObj, Offset);
				}
				const FString LevelFilename = FPackageName::LongPackageNameToFilename(
					PackagePath, FPackageName::GetMapPackageExtension());
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(LevelFilename), true);
				CreatedStreamingLevel = UEditorLevelUtils::CreateNewStreamingLevelForWorld(
					*CurrentWorldForSwitch,
					ULevelStreamingDynamic::StaticClass(),
					LevelFilename,
					false,
					nullptr,
					false,
					TFunction<void(ULevel*)>(),
					FTransform(FRotator::ZeroRotator, Offset));
				if (!CreatedStreamingLevel || !Context.Services.AssetExists(AssetPath))
				{
					OutError = FString::Printf(TEXT("Native streaming-level creation failed for '%s'."), *PackagePath);
					return false;
				}
			}
			else
			{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				if (!LevelSubsystem->NewLevel(AssetPath, false) || !Context.Services.AssetExists(AssetPath))
#else
				// ULevelEditorSubsystem::NewLevel took only the asset path before 5.4.
				if (!LevelSubsystem->NewLevel(AssetPath) || !Context.Services.AssetExists(AssetPath))
#endif
				{
					OutError = FString::Printf(TEXT("Native level creation failed for '%s'."), *PackagePath);
					return false;
				}

				// UE saves a newly-created map from a transient editor world. Reopen the
				// persisted asset before reporting success so subsequent queued writes
				// never inherit /Temp/Untitled as their target world.
				if (!LevelSubsystem->LoadLevel(AssetPath))
				{
					OutStructured->SetStringField(TEXT("error_code"), TEXT("LEVEL_CREATED_BUT_REOPEN_FAILED"));
					OutError = FString::Printf(TEXT("Created level '%s' but failed to reopen its persisted asset."), *PackagePath);
					return false;
				}
				UWorld* LoadedWorld = Context.Services.GetEditorWorld(OutError);
				const FString LoadedWorldPath = LoadedWorld && LoadedWorld->GetOutermost()
					? LoadedWorld->GetOutermost()->GetName() : FString();
				if (!LoadedWorldPath.Equals(PackagePath, ESearchCase::IgnoreCase))
				{
					OutStructured->SetStringField(TEXT("error_code"), TEXT("LEVEL_CREATE_READBACK_MISMATCH"));
					OutStructured->SetStringField(TEXT("loaded_world_path"), LoadedWorldPath);
					OutError = FString::Printf(TEXT("Created level readback mismatch: expected '%s', loaded '%s'."),
						*PackagePath, *LoadedWorldPath);
					return false;
				}
				OutStructured->SetStringField(TEXT("loaded_world_path"), LoadedWorldPath);
			}

			OutStructured->SetStringField(TEXT("name"), LevelName);
			OutStructured->SetStringField(TEXT("package_path"), PackagePath);
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			OutStructured->SetBoolField(TEXT("verified_exists"), true);

			// Optionally add as streaming level
			if (bMakeStreaming)
			{
				if (!CurrentWorldForSwitch->GetStreamingLevels().Contains(CreatedStreamingLevel))
				{
					OutError = FString::Printf(TEXT("Created level '%s' but streaming-level readback failed."), *PackagePath);
					return false;
				}
				OutStructured->SetBoolField(TEXT("is_streaming"), true);
				OutStructured->SetStringField(TEXT("persistent_level_path"), OriginalWorldPath);
			}

			OutSummary = FString::Printf(TEXT("Created level '%s' at '%s'"), *LevelName, *PackagePath);
			return true;
		},
	nullptr,
	0,
	nullptr,
	false
	});

	// ---- level_save ----
	// Save the current or specified level
	Registry.Register({
		TEXT("level_save"),
		TEXT("Save the current persistent level or a specified level asset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("level_path"), FSololmcpSchemaBuilder::String(TEXT("Level package path to save (default: current level)"))},
			{TEXT("save_all"), FSololmcpSchemaBuilder::Boolean(TEXT("Save all dirty levels (default: false)"))},
			{TEXT("save_current_as"), FSololmcpSchemaBuilder::Boolean(TEXT("When level_path does not exist, save the current persistent level to that explicit /Game path (default: false)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const bool bSaveAll = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_all"))
				? Arguments->GetBoolField(TEXT("save_all")) : false;
			const bool bSaveCurrentAs = Arguments->HasTypedField<EJson::Boolean>(TEXT("save_current_as"))
				? Arguments->GetBoolField(TEXT("save_current_as")) : false;
			FString LevelPath;
			const bool bHasLevelPath = Arguments->TryGetStringField(TEXT("level_path"), LevelPath) && !LevelPath.TrimStartAndEnd().IsEmpty();
			ULevelEditorSubsystem* LevelSubsystem = Context.Services.GetLevelEditorSubsystem(OutError);
			if (!LevelSubsystem)
			{
				return false;
			}
			bool bSaved = false;

			if (bHasLevelPath)
			{
				FString PackagePath;
				if (!NormalizeLevelPackagePath(LevelPath, PackagePath, OutError))
				{
					return false;
				}
				const FString AssetPath = LevelObjectPathFromPackagePath(PackagePath);
				if (!Context.Services.AssetExists(AssetPath))
				{
					if (!bSaveCurrentAs)
					{
						OutError = FString::Printf(TEXT("Level not found for save: '%s'. Set save_current_as=true to explicitly save the current level there."), *PackagePath);
						return false;
					}
					UWorld* CurrentWorld = Context.Services.GetEditorWorld(OutError);
					if (!CurrentWorld || !CurrentWorld->PersistentLevel)
					{
						return false;
					}
					const FString Filename = FPackageName::LongPackageNameToFilename(
						PackagePath, FPackageName::GetMapPackageExtension());
					IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
					FString SavedFilename;
					bSaved = FEditorFileUtils::SaveLevel(CurrentWorld->PersistentLevel, Filename, &SavedFilename);
					OutStructured->SetBoolField(TEXT("saved_as"), bSaved);
					OutStructured->SetStringField(TEXT("saved_filename"), SavedFilename);
					if (bSaved && !Context.Services.AssetExists(AssetPath))
					{
						bSaved = false;
						OutError = FString::Printf(TEXT("Save-as returned success but asset readback failed for '%s'."), *AssetPath);
					}
				}
				else
				{
					bSaved = Context.Services.SaveAsset(AssetPath, false, OutError);
				}
				LevelPath = PackagePath;
			}
			else if (bSaveAll)
			{
				bSaved = LevelSubsystem->SaveAllDirtyLevels();
			}
			else
			{
				UWorld* CurrentWorld = Context.Services.GetEditorWorld(OutError);
				if (!CurrentWorld || !CurrentWorld->PersistentLevel ||
					!CurrentWorld->PersistentLevel->GetOutermost())
				{
					return false;
				}
				const FString PersistentPackagePath = CurrentWorld->PersistentLevel->GetOutermost()->GetName();
				if (PersistentPackagePath.StartsWith(TEXT("/Temp/")))
				{
					OutStructured->SetStringField(TEXT("error_code"), TEXT("BLOCKED_TRANSIENT_CURRENT_WORLD"));
					OutStructured->SetStringField(TEXT("persistent_level_path"), PersistentPackagePath);
					OutError = TEXT("The current persistent level is transient. Provide level_path and save_current_as=true to save it explicitly.");
					return false;
				}
				const FString PersistentAssetPath = LevelObjectPathFromPackagePath(PersistentPackagePath);
				bSaved = Context.Services.SaveAsset(PersistentAssetPath, false, OutError);
				OutStructured->SetStringField(TEXT("persistent_level_path"), PersistentPackagePath);
			}

			if (!bSaved)
			{
				OutError = bHasLevelPath
					? FString::Printf(TEXT("Failed to save exact level '%s'."), *LevelPath)
					: TEXT("Native level save returned false.");
				return false;
			}

			OutStructured->SetBoolField(TEXT("saved"), true);
			OutStructured->SetBoolField(TEXT("save_all"), bSaveAll);
			OutStructured->SetBoolField(TEXT("save_current_as"), bSaveCurrentAs);
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			if (bHasLevelPath)
			{
				OutStructured->SetStringField(TEXT("level_path"), LevelPath);
				OutSummary = FString::Printf(TEXT("Saved level '%s'."), *LevelPath);
			}
			else
			{
				OutSummary = bSaveAll ? TEXT("Saved all dirty levels.") : TEXT("Saved current level.");
			}
			return true;
	},
	nullptr,
	0,
	nullptr,
	false
	});

	// ---- level_load ----
	// Load a level asset (opens it in the editor)
	Registry.Register({
		TEXT("level_load"),
		TEXT("Load a level asset, replacing the current persistent level in the editor."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("level_path"), FSololmcpSchemaBuilder::String(TEXT("Level package path (e.g., '/Game/Levels/MyLevel')"))}
		}, {TEXT("level_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			FString LevelPath;
			if (!NormalizeLevelPackagePath(Arguments->GetStringField(TEXT("level_path")), LevelPath, OutError))
			{
				return false;
			}
			const FString AssetPath = LevelObjectPathFromPackagePath(LevelPath);
			const FString LevelFilename = FPackageName::LongPackageNameToFilename(
				LevelPath, FPackageName::GetMapPackageExtension());
			if (!Context.Services.AssetExists(AssetPath) && !FPaths::FileExists(LevelFilename))
			{
				OutError = FString::Printf(TEXT("Level not found for load: '%s'."), *LevelPath);
				return false;
			}
			UWorld* CurrentWorld = Context.Services.GetEditorWorld(OutError);
			const FString CurrentWorldPackage = CurrentWorld && CurrentWorld->GetOutermost()
				? CurrentWorld->GetOutermost()->GetName() : FString();
			if (!CurrentWorld || CurrentWorldPackage.StartsWith(TEXT("/Temp/")))
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("error_code"), TEXT("BLOCKED_TRANSIENT_CURRENT_WORLD"));
				OutStructured->SetStringField(TEXT("current_world_package"), CurrentWorldPackage);
				OutError = TEXT("Cannot load another level while the current editor world is transient; this can crash UE World Partition cleanup. Save or reopen a persistent /Game map first.");
				return false;
			}
			if (CurrentWorld->GetOutermost()->IsDirty())
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("error_code"), TEXT("BLOCKED_DIRTY_CURRENT_WORLD"));
				OutStructured->SetStringField(TEXT("current_world_package"), CurrentWorldPackage);
				OutError = TEXT("Cannot load another level while the current world package is dirty. Save it first.");
				return false;
			}
			if (CurrentWorldPackage.Equals(LevelPath, ESearchCase::IgnoreCase))
			{
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutStructured->SetBoolField(TEXT("already_loaded"), true);
				OutStructured->SetStringField(TEXT("level_path"), LevelPath);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("level_filename"), LevelFilename);
				OutStructured->SetStringField(TEXT("loaded_package_path"), CurrentWorldPackage);
				OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
				OutStructured->SetStringField(TEXT("receipt_status"), TEXT("already_loaded"));
				OutSummary = FString::Printf(TEXT("Level '%s' is already loaded."), *LevelPath);
				return true;
			}

			// Asset duplication and inspection can preload a map package without making it the
			// editor world. FEditorFileUtils::LoadMap expects that target package to be absent;
			// loading it in this state can trip EditorServer's world-leak fatal before the tool
			// has a chance to return an error. Never attempt an unsafe implicit unload here:
			// callers must save the asset and restart (or otherwise explicitly unload it).
			if (UPackage* PreloadedTargetPackage = FindPackage(nullptr, *LevelPath))
			{
				if (PreloadedTargetPackage != CurrentWorld->GetOutermost() && PreloadedTargetPackage->ContainsMap())
				{
					OutStructured->SetBoolField(TEXT("ok"), false);
					OutStructured->SetBoolField(TEXT("target_package_preloaded"), true);
					OutStructured->SetStringField(TEXT("error_code"), TEXT("BLOCKED_PRELOADED_TARGET_WORLD"));
					OutStructured->SetStringField(TEXT("current_world_package"), CurrentWorldPackage);
					OutStructured->SetStringField(TEXT("target_world_package"), LevelPath);
					OutStructured->SetStringField(TEXT("recovery"), TEXT("Save the target asset, restart or explicitly unload its package, then call level_load again."));
					OutError = FString::Printf(
						TEXT("Cannot load '%s' because its map package is already preloaded outside the current editor world. Save and restart or unload it first."),
						*LevelPath);
					return false;
				}
			}
			if (!FEditorFileUtils::LoadMap(LevelFilename, false, false))
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("level_path"), LevelPath);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("level_filename"), LevelFilename);
				OutError = FString::Printf(TEXT("Native editor map load failed for '%s'."), *LevelPath);
				return false;
			}
			UWorld* LoadedWorld = Context.Services.GetEditorWorld(OutError);
			const FString LoadedPackagePath = LoadedWorld && LoadedWorld->GetOutermost()
				? LoadedWorld->GetOutermost()->GetName() : FString();
			if (!LoadedPackagePath.Equals(LevelPath, ESearchCase::IgnoreCase))
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("loaded_package_path"), LoadedPackagePath);
				OutError = FString::Printf(TEXT("Level load readback mismatch: requested '%s', loaded '%s'."),
					*LevelPath, *LoadedPackagePath);
				return false;
			}

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("level_path"), LevelPath);
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("level_filename"), LevelFilename);
			OutStructured->SetStringField(TEXT("loaded_package_path"), LoadedPackagePath);
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			OutStructured->SetStringField(TEXT("receipt_status"), TEXT("loaded"));
			OutSummary = FString::Printf(TEXT("Loaded level '%s'"), *LevelPath);
			return true;
	},
	nullptr,
	0,
	nullptr,
	false
	});

	// ---- level_list ----
	// List all levels in the project (with filtering)
	Registry.Register({
		TEXT("level_list"),
		TEXT("List all level (.umap) assets in the project, with optional folder filter."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("folder"), FSololmcpSchemaBuilder::String(TEXT("Filter by folder path (e.g., '/Game/Levels')"))},
			{TEXT("recursive"), FSololmcpSchemaBuilder::Boolean(TEXT("Search recursively (default: true)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			FString Folder = Arguments->HasTypedField<EJson::String>(TEXT("folder"))
				? Arguments->GetStringField(TEXT("folder")) : TEXT("/Game");
			Folder = Folder.TrimStartAndEnd();
			Folder.RemoveFromEnd(TEXT("/"));
			if (Folder.IsEmpty())
			{
				Folder = TEXT("/Game");
			}
			if (!Folder.Equals(TEXT("/Game")) && !Folder.StartsWith(TEXT("/Game/")))
			{
				OutError = TEXT("folder must be /Game or a subfolder under /Game.");
				return false;
			}
			const bool bRecursive = Arguments->HasTypedField<EJson::Boolean>(TEXT("recursive"))
				? Arguments->GetBoolField(TEXT("recursive")) : true;
			const TArray<FAssetData> Assets = Context.Services.QueryAssets(
				Folder, TEXT("/Script/Engine.World"), bRecursive, OutError);
			if (!OutError.IsEmpty())
			{
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> Levels;
			Levels.Reserve(Assets.Num());
			for (const FAssetData& Asset : Assets)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
				Item->SetStringField(TEXT("package"), Asset.PackageName.ToString());
				Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());
				Levels.Add(MakeShared<FJsonValueObject>(Item));
			}
			OutStructured->SetArrayField(TEXT("levels"), Levels);
			OutStructured->SetNumberField(TEXT("count"), Levels.Num());
			OutStructured->SetStringField(TEXT("folder"), Folder);
			OutStructured->SetBoolField(TEXT("recursive"), bRecursive);
			OutStructured->SetStringField(TEXT("execution_backend"), TEXT("native_cpp"));
			OutSummary = FString::Printf(TEXT("Listed %d level asset(s) under '%s'."), Levels.Num(), *Folder);
			return true;
		},
	nullptr,
	0,
	nullptr,
	false
	});

	// ---- level_get_current ----
	// Return the currently edited world/level identity. This is intentionally
	// read-only and mirrors the role-level contract used by long-queue agents.
	Registry.Register({
		TEXT("level_get_current"),
		TEXT("Return the current editor world, map, persistent level, current level, and loaded level summary."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("include_levels"), FSololmcpSchemaBuilder::Boolean(TEXT("Include loaded level array (default: true)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const bool bIncludeLevels = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_levels"))
				? Arguments->GetBoolField(TEXT("include_levels")) : true;

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				OutError = TEXT("No editor world.");
				return false;
			}

			ULevel* CurrentLevel = World->GetCurrentLevel();
			ULevel* PersistentLevel = World->PersistentLevel;
			const FString MapName = World->GetMapName();
			const FString WorldPath = World->GetOutermost() ? World->GetOutermost()->GetPathName() : FString();
			const FString CurrentLevelPath = CurrentLevel && CurrentLevel->GetOutermost()
				? CurrentLevel->GetOutermost()->GetPathName() : FString();
			const FString PersistentLevelPath = PersistentLevel && PersistentLevel->GetOutermost()
				? PersistentLevel->GetOutermost()->GetPathName() : FString();

			OutStructured->SetStringField(TEXT("world_name"), World->GetName());
			OutStructured->SetStringField(TEXT("map_name"), MapName);
			OutStructured->SetStringField(TEXT("world_package_path"), WorldPath);
			OutStructured->SetStringField(TEXT("current_level_path"), CurrentLevelPath);
			OutStructured->SetStringField(TEXT("persistent_level_path"), PersistentLevelPath);
			OutStructured->SetBoolField(TEXT("is_partitioned_world"), World->GetWorldPartition() != nullptr);
			OutStructured->SetNumberField(TEXT("loaded_level_count"), World->GetLevels().Num());

			if (bIncludeLevels)
			{
				TArray<TSharedPtr<FJsonValue>> LevelsJson;
				for (ULevel* Level : World->GetLevels())
				{
					if (!Level) continue;
					TSharedRef<FJsonObject> LevelJson = MakeShared<FJsonObject>();
					LevelJson->SetStringField(TEXT("name"), Level->GetName());
					LevelJson->SetStringField(TEXT("package_path"), Level->GetOutermost() ? Level->GetOutermost()->GetPathName() : FString());
					LevelJson->SetBoolField(TEXT("is_current"), Level == CurrentLevel);
					LevelJson->SetBoolField(TEXT("is_persistent"), Level == PersistentLevel);
					LevelJson->SetNumberField(TEXT("actor_count"), Level->Actors.Num());
					LevelsJson.Add(MakeShared<FJsonValueObject>(LevelJson));
				}
				OutStructured->SetArrayField(TEXT("levels"), LevelsJson);
			}

			OutSummary = FString::Printf(TEXT("Current level: %s (map=%s, loaded_levels=%d)"),
				CurrentLevelPath.IsEmpty() ? TEXT("<unknown>") : *CurrentLevelPath,
				*MapName,
				World->GetLevels().Num());
			return true;
		}
	});

	// ---- level_actor_list ----
	// List all actors in the current level (or streaming level)
	Registry.Register({
		TEXT("level_actor_list"),
		TEXT("List all actors in the current level or a specified streaming level."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("filter_class"), FSololmcpSchemaBuilder::String(TEXT("Filter by actor class (e.g., 'StaticMeshActor')"))},
			{TEXT("filter_name_contains"), FSololmcpSchemaBuilder::String(TEXT("Filter by name substring"))},
			{TEXT("include_location"), FSololmcpSchemaBuilder::Boolean(TEXT("Include actor locations (default: true)"))},
			{TEXT("max_results"), FSololmcpSchemaBuilder::Number(TEXT("Maximum number of results (default: 500)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString FilterClass = Arguments->HasTypedField<EJson::String>(TEXT("filter_class"))
				? Arguments->GetStringField(TEXT("filter_class")) : TEXT("");
			const FString FilterName = Arguments->HasTypedField<EJson::String>(TEXT("filter_name_contains"))
				? Arguments->GetStringField(TEXT("filter_name_contains")) : TEXT("");
			const bool bIncludeLoc = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_location"))
				? Arguments->GetBoolField(TEXT("include_location")) : true;
			const int32 MaxResults = Arguments->HasTypedField<EJson::Number>(TEXT("max_results"))
				? Arguments->GetIntegerField(TEXT("max_results")) : 500;

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			TArray<TSharedPtr<FJsonValue>> ActorsJson;
			int32 Count = 0;

			for (ULevel* Level : World->GetLevels())
			{
				if (!Level) continue;
				for (AActor* Actor : Level->Actors)
				{
					if (!Actor) continue;
					if (Count >= MaxResults) break;

					// Apply filters
					if (!FilterClass.IsEmpty() && !Actor->GetClass()->GetName().Contains(FilterClass)) continue;
					if (!FilterName.IsEmpty() && !Actor->GetActorLabel().Contains(FilterName)) continue;

					TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
					ActorJson->SetStringField(TEXT("name"), Actor->GetActorLabel());
					ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
					ActorJson->SetStringField(TEXT("path"), Actor->GetPathName());
					ActorJson->SetBoolField(TEXT("is_selected"), Actor->IsSelected());

					if (bIncludeLoc)
					{
						TSharedRef<FJsonObject> LocJson = MakeShared<FJsonObject>();
						const FVector Loc = Actor->GetActorLocation();
						LocJson->SetNumberField(TEXT("x"), Loc.X);
						LocJson->SetNumberField(TEXT("y"), Loc.Y);
						LocJson->SetNumberField(TEXT("z"), Loc.Z);
						ActorJson->SetObjectField(TEXT("location"), LocJson);
					}

					ActorsJson.Add(MakeShared<FJsonValueObject>(ActorJson));
					++Count;
				}
				if (Count >= MaxResults) break;
			}

			OutStructured->SetArrayField(TEXT("actors"), ActorsJson);
			OutStructured->SetNumberField(TEXT("total_returned"), Count);

			OutSummary = FString::Printf(TEXT("Listed %d actors in current level"), Count);
			return true;
		}
	});

	// ---- level_actor_move ----
	// Move actors between levels/sublevels
	Registry.Register({
		TEXT("level_actor_move"),
		TEXT("Move actors from one level/sublevel to another. Supports moving to streaming sub-levels."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor names to move"))},
			{TEXT("target_level_path"), FSololmcpSchemaBuilder::String(TEXT("Target level package path (e.g., '/Game/Levels/SubLevel1')"))}
		}, {TEXT("actors"), TEXT("target_level_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("actors"), ActorsArr) || !ActorsArr || ActorsArr->Num() == 0)
			{
				OutError = TEXT("actors must be a non-empty array.");
				return false;
			}

			FString TargetLevel;
			if (!NormalizeLevelPackagePath(Arguments->GetStringField(TEXT("target_level_path")), TargetLevel, OutError))
			{
				return false;
			}
			const FString TargetAssetPath = LevelObjectPathFromPackagePath(TargetLevel);

			// Build actor list for Python
			FString ActorListStr;
			for (int32 i = 0; i < ActorsArr->Num(); ++i)
			{
				const FString ActorName = (*ActorsArr)[i]->AsString().TrimStartAndEnd();
				if (ActorName.IsEmpty())
				{
					OutError = TEXT("actors must not contain empty names.");
					return false;
				}
				if (i > 0) ActorListStr += TEXT(", ");
				ActorListStr += PythonStringLiteral(ActorName);
			}

			FString PythonCode = FString::Printf(TEXT(
				"import unreal\n"
				"actor_names = [%s]\n"
				"target_level = %s\n"
				"target_asset_path = %s\n"
				"moved = 0\n"
				"failed = []\n"
				"if not unreal.EditorAssetLibrary.does_asset_exist(target_asset_path):\n"
				"    failed.append(target_level + ':target_level_not_found')\n"
				"else:\n"
				"    all_actors = unreal.EditorLevelLibrary.get_all_level_actors()\n"
				"    for name in actor_names:\n"
				"        matches = [a for a in all_actors if a.get_actor_label() == name or a.get_name() == name or a.get_path_name() == name]\n"
				"        if len(matches) != 1:\n"
				"            failed.append(name + (':not_found' if len(matches) == 0 else ':ambiguous'))\n"
				"            continue\n"
				"        try:\n"
				"            result = unreal.EditorLevelLibrary.move_actor_to_level(matches[0], target_level)\n"
				"            if result is False:\n"
				"                failed.append(name + ':move_returned_false')\n"
				"            else:\n"
				"                moved += 1\n"
				"        except Exception as e:\n"
				"            failed.append(name + ':' + str(e))\n"
				"print('SOMO_LEVEL_ACTOR_MOVE_RESULT:moved=%%d;requested=%%d;failed=%%d;details=%%s' %% (moved, len(actor_names), len(failed), failed))\n"
			), *ActorListStr, *PythonStringLiteral(TargetLevel), *PythonStringLiteral(TargetAssetPath));

			if (!Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError))
			{
				OutError = TEXT("Failed to move actors between levels.");
				return false;
			}
			const FString ExpectedMarker = FString::Printf(TEXT("SOMO_LEVEL_ACTOR_MOVE_RESULT:moved=%d;requested=%d;failed=0"),
				ActorsArr->Num(), ActorsArr->Num());
			if (!OutSummary.Contains(ExpectedMarker))
			{
				OutError = FString::Printf(TEXT("Actor move did not fully succeed. Python output: %s"), *OutSummary);
				return false;
			}

			OutStructured->SetStringField(TEXT("target_level"), TargetLevel);
			OutStructured->SetNumberField(TEXT("requested"), ActorsArr->Num());
			OutStructured->SetNumberField(TEXT("moved"), ActorsArr->Num());

			OutSummary = FString::Printf(TEXT("Moved %d actors to '%s'"), ActorsArr->Num(), *TargetLevel);
			return true;
		},
	nullptr,
	0,
	nullptr,
	true
	});

	// ---- level_delete ----
	// Delete a level/map asset from the project
	Registry.Register({
		TEXT("level_delete"),
		TEXT("Delete a level (.umap) asset from the project permanently."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("level_path"), FSololmcpSchemaBuilder::String(TEXT("Level package path to delete (e.g., '/Game/Levels/OldLevel')"))}
		}, {TEXT("level_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			FString LevelPath;
			if (!NormalizeLevelPackagePath(Arguments->GetStringField(TEXT("level_path")), LevelPath, OutError))
			{
				return false;
			}
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World && World->PersistentLevel && World->PersistentLevel->GetOutermost())
			{
				const FString CurrentPackagePath = World->PersistentLevel->GetOutermost()->GetName();
				if (CurrentPackagePath == LevelPath)
				{
					OutError = FString::Printf(TEXT("Refusing to delete the current persistent level: %s"), *LevelPath);
					return false;
				}
			}

			const FString LevelFilename = FPackageName::LongPackageNameToFilename(
				LevelPath, FPackageName::GetMapPackageExtension());
			const bool bRegisteredAssetExists = Context.Services.AssetExists(LevelPath);
			const bool bPhysicalFileExists = IFileManager::Get().FileExists(*LevelFilename);
			if (!bRegisteredAssetExists && !bPhysicalFileExists)
			{
				OutStructured->SetStringField(TEXT("deleted_path"), LevelPath);
				OutStructured->SetBoolField(TEXT("already_absent"), true);
				OutStructured->SetBoolField(TEXT("verified_absent"), true);
				OutSummary = FString::Printf(TEXT("Level '%s' was already absent."), *LevelPath);
				return true;
			}

			if (UPackage* LoadedPackage = FindPackage(nullptr, *LevelPath))
			{
				LoadedPackage->SetDirtyFlag(false);
				TArray<UPackage*> PackagesToUnload{LoadedPackage};
				UPackageTools::FUnloadPackageParams UnloadParams(PackagesToUnload);
				UnloadParams.bUnloadDirtyPackages = true;
				if (!UPackageTools::UnloadPackages(UnloadParams))
				{
					OutError = FString::Printf(
						TEXT("Failed to unload level '%s' before deletion: %s"),
						*LevelPath, *UnloadParams.OutErrorMessage.ToString());
					return false;
				}
			}

			FString DeleteError;
			if (bRegisteredAssetExists &&
				!Context.Services.DeleteAsset(LevelPath, DeleteError))
			{
				OutError = FString::Printf(
					TEXT("Failed to delete level '%s': %s"),
					*LevelPath, *DeleteError);
				return false;
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			bool bAssetStillExists = Context.Services.AssetExists(LevelPath);
			bool bFileStillExists = IFileManager::Get().FileExists(*LevelFilename);
			bool bDeletedOrphanFile = false;
			if (!bAssetStillExists && bFileStillExists &&
				FindPackage(nullptr, *LevelPath) == nullptr)
			{
				bDeletedOrphanFile = IFileManager::Get().Delete(
					*LevelFilename,
					/*RequireExists=*/false,
					/*EvenReadOnly=*/true,
					/*Quiet=*/true);
				bFileStillExists = IFileManager::Get().FileExists(*LevelFilename);
			}
			if (bAssetStillExists || bFileStillExists)
			{
				OutError = FString::Printf(
					TEXT("Level deletion did not verify for '%s' (asset_exists=%s, file_exists=%s)."),
					*LevelPath,
					bAssetStillExists ? TEXT("true") : TEXT("false"),
					bFileStillExists ? TEXT("true") : TEXT("false"));
				return false;
			}

			OutStructured->SetStringField(TEXT("deleted_path"), LevelPath);
			OutStructured->SetBoolField(TEXT("already_absent"), false);
			OutStructured->SetBoolField(TEXT("verified_absent"), true);
			OutStructured->SetBoolField(TEXT("deleted_orphan_file"), bDeletedOrphanFile);
			OutSummary = FString::Printf(TEXT("Deleted level '%s'"), *LevelPath);
			return true;
		}
	});

	// ---- world_partition_data_layer_create ----
	// Create a WorldPartition data layer
	Registry.Register({
		TEXT("world_partition_data_layer_create"),
		TEXT("Create a new data layer in the current WorldPartition. Data layers control which actors are loaded/visible."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Data layer name"))},
			{TEXT("initial_state"), FSololmcpSchemaBuilder::String(TEXT("Initial state: 'loaded' (default) or 'unloaded'"))}
		}, {TEXT("name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString LayerName = Arguments->GetStringField(TEXT("name"));
			const FString InitialState = Arguments->HasTypedField<EJson::String>(TEXT("initial_state"))
				? Arguments->GetStringField(TEXT("initial_state")).ToLower() : TEXT("loaded");

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
			AWorldDataLayers* WorldDataLayers = World ? World->GetWorldDataLayers() : nullptr;
			if (!World || !Subsystem || !WorldDataLayers)
			{
				OutError = TEXT("Current world does not expose DataLayerEditorSubsystem and WorldDataLayers.");
				return false;
			}

			UDataLayerInstance* TargetLayer = nullptr;
			if (UDataLayerManager* Manager = UDataLayerManager::GetDataLayerManager(World))
			{
				Manager->ForEachDataLayerInstance([&](UDataLayerInstance* Candidate)
				{
					if (Candidate
						&& (Candidate->GetDataLayerShortName() == LayerName
							|| Candidate->GetDataLayerFullName() == LayerName))
					{
						TargetLayer = Candidate;
						return false;
					}
					return true;
				});
			}

			if (!TargetLayer)
			{
				FDataLayerCreationParameters Parameters;
				Parameters.WorldDataLayers = WorldDataLayers;
				Parameters.bIsPrivate = true;
				TargetLayer = Subsystem->CreateDataLayerInstance(Parameters);
			}
			if (!TargetLayer)
			{
				OutError = TEXT("Failed to create data layer.");
				return false;
			}

			Subsystem->SetDataLayerShortName(TargetLayer, LayerName);
			Subsystem->SetDataLayerVisibility(TargetLayer, true);
			const bool bLoaded = InitialState != TEXT("unloaded");
			if (!Subsystem->SetDataLayerIsLoadedInEditor(TargetLayer, bLoaded, true))
			{
				OutError = TEXT("Failed to set data layer initial loaded state.");
				return false;
			}
			TargetLayer->MarkPackageDirty();
			if (World->PersistentLevel)
			{
				World->PersistentLevel->MarkPackageDirty();
			}

			OutStructured->SetStringField(TEXT("name"), LayerName);
			OutStructured->SetStringField(TEXT("initial_state"), InitialState);
			OutStructured->SetStringField(TEXT("short_name"), TargetLayer->GetDataLayerShortName());
			OutStructured->SetStringField(TEXT("full_name"), TargetLayer->GetDataLayerFullName());
			OutStructured->SetBoolField(TEXT("visible"), TargetLayer->IsVisible());
			OutStructured->SetBoolField(TEXT("loaded_in_editor"), TargetLayer->IsLoadedInEditor());
			OutStructured->SetBoolField(TEXT("created_or_existing"), true);
			OutSummary = FString::Printf(TEXT("Created data layer '%s' (initial: %s)"), *LayerName, *InitialState);
			return true;
		}
	});

	// ---- world_partition_data_layer_toggle ----
	// Toggle data layer loaded/unloaded state
	Registry.Register({
		TEXT("world_partition_data_layer_toggle"),
		TEXT("Toggle a WorldPartition data layer between loaded and unloaded states."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("name"), FSololmcpSchemaBuilder::String(TEXT("Data layer name"))},
			{TEXT("state"), FSololmcpSchemaBuilder::String(TEXT("Target state: 'loaded' or 'unloaded'"))}
		}, {TEXT("name"), TEXT("state")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString LayerName = Arguments->GetStringField(TEXT("name"));
			const FString State = Arguments->GetStringField(TEXT("state")).ToLower();

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
			if (!World || !Subsystem)
			{
				OutError = TEXT("Current world does not expose DataLayerEditorSubsystem.");
				return false;
			}
			UDataLayerInstance* TargetLayer = nullptr;
			if (UDataLayerManager* Manager = UDataLayerManager::GetDataLayerManager(World))
			{
				Manager->ForEachDataLayerInstance([&](UDataLayerInstance* Candidate)
				{
					if (Candidate
						&& (Candidate->GetDataLayerShortName() == LayerName
							|| Candidate->GetDataLayerFullName() == LayerName))
					{
						TargetLayer = Candidate;
						return false;
					}
					return true;
				});
			}
			if (!TargetLayer)
			{
				OutError = FString::Printf(TEXT("Data layer '%s' was not found."), *LayerName);
				return false;
			}

			const bool bLoaded = State == TEXT("loaded");
			if (State != TEXT("loaded") && State != TEXT("unloaded"))
			{
				OutError = TEXT("state must be 'loaded' or 'unloaded'.");
				return false;
			}
			if (!Subsystem->SetDataLayerIsLoadedInEditor(TargetLayer, bLoaded, true))
			{
				OutError = TEXT("Failed to update data layer loaded state.");
				return false;
			}
			TargetLayer->MarkPackageDirty();

			OutStructured->SetStringField(TEXT("name"), LayerName);
			OutStructured->SetStringField(TEXT("state"), State);
			OutStructured->SetStringField(TEXT("short_name"), TargetLayer->GetDataLayerShortName());
			OutStructured->SetStringField(TEXT("full_name"), TargetLayer->GetDataLayerFullName());
			OutStructured->SetBoolField(TEXT("visible"), TargetLayer->IsVisible());
			OutStructured->SetBoolField(TEXT("loaded_in_editor"), TargetLayer->IsLoadedInEditor());
			OutSummary = FString::Printf(TEXT("Data layer '%s' set to '%s'"), *LayerName, *State);
			return true;
		}
	});

	// ---- world_partition_actor_assign ----
	// Assign an actor to a specific data layer
	Registry.Register({
		TEXT("world_partition_actor_assign"),
		TEXT("Assign one or more actors to a WorldPartition data layer."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actors"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor names to assign"))},
			{TEXT("data_layer"), FSololmcpSchemaBuilder::String(TEXT("Target data layer name"))}
		}, {TEXT("actors"), TEXT("data_layer")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("actors"), ActorsArr) || !ActorsArr || ActorsArr->Num() == 0)
			{
				OutError = TEXT("actors must be a non-empty array.");
				return false;
			}
			const FString DataLayer = Arguments->GetStringField(TEXT("data_layer"));
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			UDataLayerEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
			if (!World || !Subsystem)
			{
				OutError = TEXT("Current world does not expose DataLayerEditorSubsystem.");
				return false;
			}

			UDataLayerInstance* TargetLayer = nullptr;
			if (UDataLayerManager* Manager = UDataLayerManager::GetDataLayerManager(World))
			{
				Manager->ForEachDataLayerInstance([&](UDataLayerInstance* Candidate)
				{
					if (Candidate &&
						(Candidate->GetDataLayerShortName().Equals(DataLayer, ESearchCase::IgnoreCase) ||
						 Candidate->GetDataLayerFullName().Equals(DataLayer, ESearchCase::IgnoreCase) ||
						 static_cast<const UObject*>(Candidate)->GetName().Equals(DataLayer, ESearchCase::IgnoreCase)))
					{
						TargetLayer = Candidate;
						return false;
					}
					return true;
				});
			}
			if (!TargetLayer)
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("DATA_LAYER_NOT_FOUND"));
				OutError = FString::Printf(TEXT("Data layer '%s' was not found."), *DataLayer);
				return false;
			}

			TArray<AActor*> ResolvedActors;
			TArray<TSharedPtr<FJsonValue>> MissingActors;
			for (const TSharedPtr<FJsonValue>& ActorValue : *ActorsArr)
			{
				FString ActorIdentifier;
				if (!ActorValue.IsValid() || !ActorValue->TryGetString(ActorIdentifier) || ActorIdentifier.IsEmpty())
				{
					MissingActors.Add(MakeShared<FJsonValueString>(TEXT("(invalid actor identifier)")));
					continue;
				}
				FString ResolveError;
				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorIdentifier, ResolveError);
				if (!Actor)
				{
					MissingActors.Add(MakeShared<FJsonValueString>(ActorIdentifier));
					continue;
				}
				ResolvedActors.AddUnique(Actor);
			}
			if (!MissingActors.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("ACTOR_RESOLUTION_FAILED"));
				OutStructured->SetArrayField(TEXT("missing_actors"), MissingActors);
				OutError = TEXT("One or more actor identifiers could not be resolved; no data-layer mutation was attempted.");
				return false;
			}

			const bool bMutationReported = Subsystem->AddActorsToDataLayer(ResolvedActors, TargetLayer);
			TArray<TSharedPtr<FJsonValue>> AssignedActors;
			TArray<TSharedPtr<FJsonValue>> FailedReadback;
			for (AActor* Actor : ResolvedActors)
			{
				if (!Actor)
				{
					continue;
				}
				Actor->MarkPackageDirty();
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("label"), Actor->GetActorLabel());
				Item->SetStringField(TEXT("name"), Actor->GetName());
				Item->SetStringField(TEXT("path"), Actor->GetPathName());
				const bool bContainsLayer = Actor->ContainsDataLayer(TargetLayer);
				Item->SetBoolField(TEXT("contains_data_layer"), bContainsLayer);
				AssignedActors.Add(MakeShared<FJsonValueObject>(Item));
				if (!bContainsLayer)
				{
					FailedReadback.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
				}
			}
			World->MarkPackageDirty();
			OutStructured->SetStringField(TEXT("data_layer"), DataLayer);
			OutStructured->SetNumberField(TEXT("requested"), ActorsArr->Num());
			OutStructured->SetNumberField(TEXT("resolved"), ResolvedActors.Num());
			OutStructured->SetNumberField(TEXT("assigned"), AssignedActors.Num() - FailedReadback.Num());
			OutStructured->SetBoolField(TEXT("mutation_reported"), bMutationReported);
			OutStructured->SetStringField(TEXT("data_layer_short_name"), TargetLayer->GetDataLayerShortName());
			OutStructured->SetStringField(TEXT("data_layer_full_name"), TargetLayer->GetDataLayerFullName());
			OutStructured->SetArrayField(TEXT("actors"), AssignedActors);
			OutStructured->SetArrayField(TEXT("failed_readback"), FailedReadback);
			OutStructured->SetBoolField(TEXT("verified"), FailedReadback.IsEmpty());
			OutStructured->SetStringField(TEXT("receipt_schema"), TEXT("somol.world_partition_actor_assign.v2"));
			if (!FailedReadback.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("error_code"), TEXT("DATA_LAYER_ASSIGN_READBACK_FAILED"));
				OutError = TEXT("One or more actors did not report membership in the target data layer after assignment.");
				return false;
			}
			OutSummary = FString::Printf(TEXT("Assigned and verified %d actor(s) in data layer '%s'."), ResolvedActors.Num(), *TargetLayer->GetDataLayerShortName());
			return true;
		}
	});

	// ---- level_streaming_volume_create ----
	// Create a level streaming volume
	Registry.Register({
		TEXT("level_streaming_volume_create"),
		TEXT("Create a level streaming volume that triggers sub-level loading when the player enters."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("level_path"), FSololmcpSchemaBuilder::String(TEXT("Level package path to associate with this volume"))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({
				{TEXT("x"), FSololmcpSchemaBuilder::Number()},
				{TEXT("y"), FSololmcpSchemaBuilder::Number()},
				{TEXT("z"), FSololmcpSchemaBuilder::Number()}
			})},
			{TEXT("extent_x"), FSololmcpSchemaBuilder::Number(TEXT("Volume half-extent X in cm (default: 5000)"))},
			{TEXT("extent_y"), FSololmcpSchemaBuilder::Number(TEXT("Volume half-extent Y in cm (default: 5000)"))},
			{TEXT("extent_z"), FSololmcpSchemaBuilder::Number(TEXT("Volume half-extent Z in cm (default: 5000)"))}
		}, {TEXT("level_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString LevelPath = Arguments->GetStringField(TEXT("level_path"));
			FVector Location(0, 0, 0);
			if (const TSharedPtr<FJsonObject>* LocObj = nullptr; Arguments->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
			{
				FSololmcpEditorServices::JsonToVector(*LocObj, Location);
			}
			const float ExtX = Arguments->HasTypedField<EJson::Number>(TEXT("extent_x"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("extent_x"))) : 5000.0f;
			const float ExtY = Arguments->HasTypedField<EJson::Number>(TEXT("extent_y"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("extent_y"))) : 5000.0f;
			const float ExtZ = Arguments->HasTypedField<EJson::Number>(TEXT("extent_z"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("extent_z"))) : 5000.0f;

			FString PythonCode = FString::Printf(TEXT(
				"import unreal\n"
				"from unreal import Vector\n"
				"actor_class = unreal.LevelStreamingVolume\n"
				"actor = unreal.EditorLevelLibrary.spawn_actor_from_class(actor_class, unreal.Vector(%f, %f, %f))\n"
				"if actor:\n"
				"    brush = actor.get_editor_property('brush_component')\n"
				"    if brush:\n"
				"        brush.set_editor_property('brush_scale', unreal.Vector(%f, %f, %f))\n"
				"    actor.set_actor_label('StreamingVolume_%s')\n"
				"    print('CREATED:' + actor.get_actor_label())\n"
				"else:\n"
				"    print('FAILED')\n"
			), Location.X, Location.Y, Location.Z, ExtX / 100.0, ExtY / 100.0, ExtZ / 100.0,
				*FPaths::GetCleanFilename(LevelPath));

			if (!Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, OutStructured, OutSummary, OutError))
			{
				OutError = TEXT("Failed to create streaming volume.");
				return false;
			}

			OutStructured->SetStringField(TEXT("level_path"), LevelPath);
			OutStructured->SetStringField(TEXT("extent"), FString::Printf(TEXT("%.0f x %.0f x %.0f"), ExtX, ExtY, ExtZ));
			OutSummary = FString::Printf(TEXT("Created streaming volume for level '%s'"), *LevelPath);
			return true;
		},
	nullptr,
	0,
	nullptr,
	true
	});

	// ---- level_streaming_volume_list ----
	// List all streaming volumes in the current level
	Registry.Register({
		TEXT("level_streaming_volume_list"),
		TEXT("List all level streaming volumes in the current level with their associated levels and properties."),
		FSololmcpSchemaBuilder::Object({{}}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			TArray<TSharedPtr<FJsonValue>> VolumesJson;

			for (ULevelStreaming* SL : World->GetStreamingLevels())
			{
				if (!SL) continue;
				TSharedPtr<FJsonObject> VolObj = MakeShared<FJsonObject>();
				VolObj->SetStringField(TEXT("level_name"), SL->GetWorldAssetPackageFName().ToString());
				VolObj->SetStringField(TEXT("package_name"), SL->GetWorldAssetPackageFName().ToString());
				VolObj->SetBoolField(TEXT("is_loaded"), SL->IsLevelLoaded());
				VolObj->SetBoolField(TEXT("is_visible"), SL->IsLevelVisible());
				VolObj->SetNumberField(TEXT("loaded_status"), SL->IsLevelLoaded() ? 1 : 0);

				TSharedRef<FJsonObject> TransformObj = MakeShared<FJsonObject>();
				const FTransform& Transform = SL->LevelTransform;
				const FVector Loc = Transform.GetTranslation();
				TransformObj->SetNumberField(TEXT("location_x"), Loc.X);
				TransformObj->SetNumberField(TEXT("location_y"), Loc.Y);
				TransformObj->SetNumberField(TEXT("location_z"), Loc.Z);
				VolObj->SetObjectField(TEXT("transform"), TransformObj);

				VolumesJson.Add(MakeShared<FJsonValueObject>(VolObj));
			}

			// Also list ALevelStreamingVolume actors
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor->GetClass()->GetName().Contains(TEXT("StreamingVolume")))
				{
					TSharedPtr<FJsonObject> VolActorObj = MakeShared<FJsonObject>();
					VolActorObj->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
					VolActorObj->SetStringField(TEXT("actor_path"), Actor->GetPathName());

					const FVector Loc = Actor->GetActorLocation();
					VolActorObj->SetStringField(TEXT("location"), Loc.ToString());

					VolumesJson.Add(MakeShared<FJsonValueObject>(VolActorObj));
				}
			}

			OutStructured->SetArrayField(TEXT("volumes"), VolumesJson);
			OutStructured->SetNumberField(TEXT("total"), VolumesJson.Num());
			OutSummary = FString::Printf(TEXT("Found %d streaming volumes"), VolumesJson.Num());
			return true;
		}
	});

	// ---- level_streaming_volume_delete ----
	// Delete a streaming volume from the current level
	Registry.Register({
		TEXT("level_streaming_volume_delete"),
		TEXT("Delete a streaming volume actor from the current level."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("volume_name"), FSololmcpSchemaBuilder::String(TEXT("Streaming volume actor name to delete"))}
		}, {TEXT("volume_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString VolumeName = Arguments->GetStringField(TEXT("volume_name"));

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			AActor* VolumeActor = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == VolumeName || It->GetName() == VolumeName)
				{
					VolumeActor = *It;
					break;
				}
			}
			if (!VolumeActor)
			{
				OutError = FString::Printf(TEXT("Volume actor '%s' not found."), *VolumeName);
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DeleteStreamingVolume", "SOMOLMCP Delete Streaming Volume"));
			VolumeActor->Destroy();

			OutStructured->SetStringField(TEXT("deleted_volume"), VolumeName);
			OutSummary = FString::Printf(TEXT("Deleted streaming volume '%s'"), *VolumeName);
			return true;
		}
	});

	// ---- level_snapshot ----
	// Capture a snapshot of all actors in the current level for later comparison
	Registry.Register({
		TEXT("level_snapshot"),
		TEXT("Capture a detailed snapshot of the current level state — all actors, transforms, properties. Useful for before/after comparisons."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Snapshot label for identification"))},
			{TEXT("include_details"), FSololmcpSchemaBuilder::Boolean(TEXT("Include mesh paths, materials, etc. (default: false)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString Label = Arguments->HasTypedField<EJson::String>(TEXT("label"))
				? Arguments->GetStringField(TEXT("label")) : TEXT("snapshot");
			const bool bDetails = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_details"))
				? Arguments->GetBoolField(TEXT("include_details")) : false;

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			TArray<TSharedPtr<FJsonValue>> ActorsJson;
			int32 TotalActors = 0;

			for (ULevel* Level : World->GetLevels())
			{
				if (!Level) continue;
				for (AActor* Actor : Level->Actors)
				{
					if (!Actor) continue;
					++TotalActors;

					TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
					ActorJson->SetStringField(TEXT("name"), Actor->GetActorLabel());
					ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
					ActorJson->SetStringField(TEXT("outer_level"), Level->GetOutermost()->GetPathName());

					// Transform
					const FTransform Transform = Actor->GetActorTransform();
					TSharedRef<FJsonObject> LocJson = MakeShared<FJsonObject>();
					LocJson->SetNumberField(TEXT("x"), Transform.GetTranslation().X);
					LocJson->SetNumberField(TEXT("y"), Transform.GetTranslation().Y);
					LocJson->SetNumberField(TEXT("z"), Transform.GetTranslation().Z);
					ActorJson->SetObjectField(TEXT("location"), LocJson);

					TSharedRef<FJsonObject> RotJson = MakeShared<FJsonObject>();
					const FRotator Rot = Transform.Rotator();
					RotJson->SetNumberField(TEXT("pitch"), Rot.Pitch);
					RotJson->SetNumberField(TEXT("yaw"), Rot.Yaw);
					RotJson->SetNumberField(TEXT("roll"), Rot.Roll);
					ActorJson->SetObjectField(TEXT("rotation"), RotJson);

					TSharedRef<FJsonObject> ScaleJson = MakeShared<FJsonObject>();
					const FVector Scale = Transform.GetScale3D();
					ScaleJson->SetNumberField(TEXT("x"), Scale.X);
					ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
					ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
					ActorJson->SetObjectField(TEXT("scale"), ScaleJson);

					if (bDetails)
					{
						// Add component info for static mesh actors
						if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor))
						{
							if (UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent())
							{
								if (SMC->GetStaticMesh())
								{
									ActorJson->SetStringField(TEXT("static_mesh"), SMC->GetStaticMesh()->GetPathName());
								}
								for (int32 i = 0; i < SMC->GetNumMaterials(); ++i)
								{
									if (SMC->GetMaterial(i))
									{
										ActorJson->SetStringField(FString::Printf(TEXT("material_%d"), i),
											SMC->GetMaterial(i)->GetPathName());
									}
								}
							}
						}
					}

					ActorsJson.Add(MakeShared<FJsonValueObject>(ActorJson));
				}
			}

			OutStructured->SetStringField(TEXT("label"), Label);
			OutStructured->SetStringField(TEXT("world"), World->GetMapName());
			OutStructured->SetNumberField(TEXT("total_actors"), TotalActors);
			OutStructured->SetNumberField(TEXT("snapshot_actors"), ActorsJson.Num());
			OutStructured->SetArrayField(TEXT("actors"), ActorsJson);

			OutSummary = FString::Printf(TEXT("Snapshot '%s': %d actors captured from %s"),
				*Label, TotalActors, *World->GetMapName());
			return true;
		}
	});

	// ─── Task 4: Enhanced Editor Perception ─────────────────────────────────

	// ---- editor_get_state ----
	// Get comprehensive editor state information
	Registry.Register({
		TEXT("editor_get_state"),
		TEXT("Get comprehensive editor state: selected actors, camera position, editor mode, dirty packages, loaded levels, world partition status."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			// Selected actors — iterate World actors and check IsSelected
			TArray<TSharedPtr<FJsonValue>> SelectedJson;
			UWorld* SelWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (SelWorld)
			{
				for (TActorIterator<AActor> It(SelWorld); It; ++It)
				{
					AActor* Actor = *It;
					if (Actor && Actor->IsSelected())
					{
						TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
						ActorJson->SetStringField(TEXT("name"), Actor->GetActorLabel());
						ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
						ActorJson->SetStringField(TEXT("path"), Actor->GetPathName());

						TSharedRef<FJsonObject> LocJson = MakeShared<FJsonObject>();
						const FVector Loc = Actor->GetActorLocation();
						LocJson->SetNumberField(TEXT("x"), Loc.X);
						LocJson->SetNumberField(TEXT("y"), Loc.Y);
						LocJson->SetNumberField(TEXT("z"), Loc.Z);
						ActorJson->SetObjectField(TEXT("location"), LocJson);

						SelectedJson.Add(MakeShared<FJsonValueObject>(ActorJson));
					}
				}
			}
			OutStructured->SetArrayField(TEXT("selected_actors"), SelectedJson);

			// Editor camera — get from active viewport's view
			{
				TSharedRef<FJsonObject> CamJson = MakeShared<FJsonObject>();
				CamJson->SetStringField(TEXT("note"), TEXT("Camera location not available via public API in UE 5.7"));
				OutStructured->SetObjectField(TEXT("camera_location"), CamJson);
			}

			// Editor mode
			OutStructured->SetStringField(TEXT("editor_mode"), TEXT("Select"));

			// Loaded levels
			TArray<TSharedPtr<FJsonValue>> LevelsJson;
			for (ULevel* Level : World->GetLevels())
			{
				if (!Level) continue;
				TSharedRef<FJsonObject> LevelJson = MakeShared<FJsonObject>();
				LevelJson->SetStringField(TEXT("name"), Level->GetOutermost()->GetPathName());
				LevelJson->SetNumberField(TEXT("actor_count"), Level->Actors.Num());
				LevelJson->SetBoolField(TEXT("is_visible"), Level->bIsVisible);
				LevelsJson.Add(MakeShared<FJsonValueObject>(LevelJson));
			}
			OutStructured->SetArrayField(TEXT("loaded_levels"), LevelsJson);

			// Streaming levels
			TArray<TSharedPtr<FJsonValue>> StreamingJson;
			for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
			{
				if (!StreamingLevel) continue;
				TSharedRef<FJsonObject> SlJson = MakeShared<FJsonObject>();
				SlJson->SetStringField(TEXT("name"), StreamingLevel->GetWorldAssetPackageFName().ToString());
				SlJson->SetBoolField(TEXT("is_loaded"), StreamingLevel->IsLevelLoaded());
				SlJson->SetBoolField(TEXT("is_visible"), StreamingLevel->IsLevelVisible());
				StreamingJson.Add(MakeShared<FJsonValueObject>(SlJson));
			}
			OutStructured->SetArrayField(TEXT("streaming_levels"), StreamingJson);

			// World name
			OutStructured->SetStringField(TEXT("world_name"), World->GetMapName());

			// WorldPartition status
			if (World->IsPartitionedWorld())
			{
				TSharedRef<FJsonObject> WpJson = MakeShared<FJsonObject>();
				WpJson->SetBoolField(TEXT("is_world_partition"), true);
				OutStructured->SetObjectField(TEXT("world_partition"), WpJson);
			}

			OutSummary = FString::Printf(TEXT("Editor state: %d selected, %d levels, world=%s"),
				SelectedJson.Num(), LevelsJson.Num(), *World->GetMapName());
			return true;
		}
	});

	// ---- editor_get_warnings ----
	// Get editor warnings and messages
	Registry.Register({
		TEXT("editor_get_warnings"),
		TEXT("Get current editor warnings, error messages, and notification count. Helps agents understand what went wrong."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("max_messages"), FSololmcpSchemaBuilder::Number(TEXT("Maximum messages to return (default: 50)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const int32 MaxMsg = Arguments->HasTypedField<EJson::Number>(TEXT("max_messages"))
				? Arguments->GetIntegerField(TEXT("max_messages")) : 50;

			// Use Python to access message log
			FString PythonCode = FString::Printf(TEXT(
				"import unreal\n"
				"messages = []\n"
				"max_count = %d\n"
				"# Get log messages\n"
				"log_list = unreal.EditorFilterLibrary()\n"
				"print('WARNINGS_OK')\n"
			), MaxMsg);

			// Get output log via the message log system
			TArray<TSharedPtr<FJsonValue>> WarningsJson;

			// Check compile/PIE status
			TSharedRef<FJsonObject> StatusJson = MakeShared<FJsonObject>();
			StatusJson->SetBoolField(TEXT("is_playing"), GEditor->PlayWorld != nullptr);
			StatusJson->SetBoolField(TEXT("is_simulating"), GEditor->bIsSimulatingInEditor);
			OutStructured->SetObjectField(TEXT("editor_status"), StatusJson);

			// Count dirty packages
			int32 DirtyCount = 0;
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Pkg = *It;
				if (Pkg && Pkg->IsDirty())
				{
					++DirtyCount;
				}
			}
			OutStructured->SetNumberField(TEXT("dirty_packages"), DirtyCount);

			// Check for PIE errors
			if (GEditor->PlayWorld)
			{
				TSharedRef<FJsonObject> PieJson = MakeShared<FJsonObject>();
				PieJson->SetBoolField(TEXT("is_active"), true);
				PieJson->SetStringField(TEXT("status"), GEditor->PlayWorld->IsPaused() ? TEXT("paused") : TEXT("running"));
				OutStructured->SetObjectField(TEXT("play_in_editor"), PieJson);
			}

			// Get selected actors count
			if (UWorld* SelWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
			{
				int32 SelectedCount = 0;
				for (TActorIterator<AActor> It(SelWorld); It; ++It)
				{
					if ((*It)->IsSelected()) ++SelectedCount;
				}
				OutStructured->SetNumberField(TEXT("selected_actor_count"), SelectedCount);
			}

			OutStructured->SetArrayField(TEXT("warnings"), WarningsJson);
			OutSummary = FString::Printf(TEXT("Editor warnings: %d dirty packages, PIE=%s"),
				DirtyCount, GEditor->PlayWorld ? TEXT("active") : TEXT("inactive"));
			return true;
		}
	});

	// ---- editor_get_undo_history ----
	// Get undo/redo history information
	Registry.Register({
		TEXT("editor_get_undo_history"),
		TEXT("Get the current undo/redo history. Shows what transactions are available for undo and redo."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("max_entries"), FSololmcpSchemaBuilder::Number(TEXT("Maximum history entries to return (default: 20)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			// Transaction buffer — GEditor->Trans is TObjectPtr, access via Get()
			const bool bCanUndo = GEditor->Trans.Get() && GEditor->Trans.Get()->CanUndo();
			const bool bCanRedo = GEditor->Trans.Get() && GEditor->Trans.Get()->CanRedo();

			TArray<TSharedPtr<FJsonValue>> UndoJson;
			OutStructured->SetBoolField(TEXT("can_undo"), bCanUndo);
			OutStructured->SetBoolField(TEXT("can_redo"), bCanRedo);
			OutStructured->SetArrayField(TEXT("history"), UndoJson);

			OutSummary = FString::Printf(TEXT("Undo history: can_undo=%s, can_redo=%s"),
				bCanUndo ? TEXT("true") : TEXT("false"), bCanRedo ? TEXT("true") : TEXT("false"));
			return true;
		}
	});

	// ---- editor_get_performance_stats ----
	// Get editor performance statistics
	Registry.Register({
		TEXT("editor_get_performance_stats"),
		TEXT("Get editor performance statistics: frame time, GPU/CPU usage, triangle count, draw calls, etc."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			// Sample the editor's real application delta instead of returning a fixed 60 FPS estimate.
			const double FrameTimeSeconds = FMath::Max(0.0, FApp::GetDeltaTime());
			const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
			TSharedRef<FJsonObject> TimingJson = MakeShared<FJsonObject>();
			TimingJson->SetNumberField(TEXT("frame_time_ms"), FrameTimeSeconds * 1000.0);
			TimingJson->SetNumberField(TEXT("instant_fps"), FrameTimeSeconds > UE_DOUBLE_SMALL_NUMBER ? 1.0 / FrameTimeSeconds : 0.0);
			TimingJson->SetNumberField(TEXT("average_fps"), static_cast<double>(GAverageFPS));
			TimingJson->SetBoolField(TEXT("measured"), true);
			OutStructured->SetObjectField(TEXT("timing"), TimingJson);

			// World actor counts
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			int32 TotalActors = 0;
			if (World)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					++TotalActors;
				}
			}

			TSharedRef<FJsonObject> StatsJson = MakeShared<FJsonObject>();
			StatsJson->SetNumberField(TEXT("total_actors"), TotalActors);
			OutStructured->SetObjectField(TEXT("stats"), StatsJson);

			// Memory info
			TSharedRef<FJsonObject> MemoryJson = MakeShared<FJsonObject>();
			MemoryJson->SetNumberField(TEXT("process_mb"), static_cast<double>(MemoryStats.UsedPhysical / (1024.0 * 1024.0)));
			MemoryJson->SetNumberField(TEXT("peak_process_mb"), static_cast<double>(MemoryStats.PeakUsedPhysical / (1024.0 * 1024.0)));
			MemoryJson->SetNumberField(TEXT("committed_mb"), static_cast<double>(MemoryStats.UsedVirtual / (1024.0 * 1024.0)));
			MemoryJson->SetNumberField(TEXT("peak_committed_mb"), static_cast<double>(MemoryStats.PeakUsedVirtual / (1024.0 * 1024.0)));
			OutStructured->SetObjectField(TEXT("memory"), MemoryJson);

			TSharedRef<FJsonObject> GarbageCollectionJson = MakeShared<FJsonObject>();
			GarbageCollectionJson->SetBoolField(TEXT("in_progress"), IsGarbageCollecting());
			GarbageCollectionJson->SetNumberField(TEXT("last_gc_time_seconds"), GetLastGCTime());
			GarbageCollectionJson->SetNumberField(TEXT("uobject_count"), GUObjectArray.GetObjectArrayNum());
			OutStructured->SetObjectField(TEXT("garbage_collection"), GarbageCollectionJson);

			OutSummary = FString::Printf(TEXT("Performance: %d actors, %.0f MB RAM"),
				TotalActors,
				static_cast<double>(MemoryStats.UsedPhysical / (1024.0 * 1024.0)));
			return true;
		}
	});

	// ─── NavMesh Helper Tools ──────────────────────────────────────────
	
	Registry.Register({
		TEXT("editor_ui_spawn_navmesh"),
		TEXT("Automatically spawn a properly configured NavMeshBoundsVolume that covers the entire map. Required before building paths or exporting NavMesh."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }

			// Remove existing ones first to ensure clean state
			for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
			{
				(*It)->Destroy();
			}

			ANavMeshBoundsVolume* NavVol = World->SpawnActor<ANavMeshBoundsVolume>();
			if (!NavVol) { OutError = TEXT("Failed to spawn NavMeshBoundsVolume."); return false; }

			// Set up the builder to create the geometry
			UCubeBuilder* Builder = NewObject<UCubeBuilder>(NavVol);
			Builder->X = 200000.f;  // 2 km
			Builder->Y = 200000.f;
			Builder->Z = 50000.f;
			
			NavVol->BrushBuilder = Builder;
			Builder->Build(World, NavVol);

			// Trigger BSP/Navigation update
			NavVol->SetNeedRebuild(NavVol->GetLevel());
			NavVol->PostEditChange();
			
			GEditor->Exec(World, TEXT("REBUILDNAV"));

			OutSummary = TEXT("Successfully spawned huge NavMeshBoundsVolume and triggered path rebuild.");
			return true;
		}
	});

	Registry.Register({
		TEXT("editor_ui_live_coding"),
		TEXT("Trigger Unreal Engine's Live Coding compilation (equivalent to pressing Ctrl+Alt+F11)."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			// The safest way to trigger Live Coding across UE versions without linking the LiveCoding module
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World) { OutError = TEXT("No editor world."); return false; }
			
			GEditor->Exec(World, TEXT("LiveCoding.Compile"));

			OutSummary = TEXT("Triggered Live Coding compilation.");
			return true;
		}
	});

	// ─── NavMesh Export for Server ──────────────────────────────────────────
	// Export navigation mesh data for server-side pathfinding (player movement validation & NPC AI)

	Registry.Register({
		TEXT("navmesh_export_for_server"),
		TEXT("Export the navigation mesh (NavMesh) from the current level as a JSON file for server-side usage. "
			 "Includes polygon vertices, adjacency/neighbor data, off-mesh links, area classes, and agent parameters. "
			 "Used by MMORPG server scene service for player movement validation and NPC pathfinding."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Absolute file path for the exported JSON (e.g. 'D:/navmesh/map.json')"))},
			{TEXT("nav_mesh_name"), FSololmcpSchemaBuilder::String(TEXT("Optional: NavMesh actor label/name. Uses first RecastNavMesh if omitted."))},
			{TEXT("include_vertices"), FSololmcpSchemaBuilder::Boolean(TEXT("Include full polygon vertex data (default: true)"))},
			{TEXT("include_neighbors"), FSololmcpSchemaBuilder::Boolean(TEXT("Include polygon adjacency/neighbor relations (default: true)"))},
			{TEXT("include_off_mesh_links"), FSololmcpSchemaBuilder::Boolean(TEXT("Include off-mesh links / jump points (default: true)"))},
			{TEXT("coordinate_scale"), FSololmcpSchemaBuilder::Number(TEXT("Scale factor for coordinates, e.g. 0.01 to convert cm→m (default: 1.0)"))},
			{TEXT("min_bounds"), FSololmcpSchemaBuilder::Object({
				{TEXT("x"), FSololmcpSchemaBuilder::Number()},
				{TEXT("y"), FSololmcpSchemaBuilder::Number()},
				{TEXT("z"), FSololmcpSchemaBuilder::Number()}
			})},
			{TEXT("max_bounds"), FSololmcpSchemaBuilder::Object({
				{TEXT("x"), FSololmcpSchemaBuilder::Number()},
				{TEXT("y"), FSololmcpSchemaBuilder::Number()},
				{TEXT("z"), FSololmcpSchemaBuilder::Number()}
			})},
		}, {TEXT("output_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments,
			TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError) -> bool
		{
			const FString OutputPath = Arguments->GetStringField(TEXT("output_path"));
			if (OutputPath.IsEmpty())
			{
				OutError = TEXT("output_path is required.");
				return false;
			}

			const bool bIncludeVertices = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_vertices"))
				? Arguments->GetBoolField(TEXT("include_vertices")) : true;
			const bool bIncludeNeighbors = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_neighbors"))
				? Arguments->GetBoolField(TEXT("include_neighbors")) : true;
			const bool bIncludeOffMeshLinks = Arguments->HasTypedField<EJson::Boolean>(TEXT("include_off_mesh_links"))
				? Arguments->GetBoolField(TEXT("include_off_mesh_links")) : true;
			const float CoordScale = Arguments->HasTypedField<EJson::Number>(TEXT("coordinate_scale"))
				? static_cast<float>(Arguments->GetNumberField(TEXT("coordinate_scale"))) : 1.0f;

			// Optional spatial filter
			bool bHasBoundsFilter = false;
			FBox BoundsFilter(ForceInit);
			if (const TSharedPtr<FJsonObject>* MinObj = nullptr; Arguments->TryGetObjectField(TEXT("min_bounds"), MinObj) && MinObj)
			{
				if (const TSharedPtr<FJsonObject>* MaxObj = nullptr; Arguments->TryGetObjectField(TEXT("max_bounds"), MaxObj) && MaxObj)
				{
					FVector MinV, MaxV;
					FSololmcpEditorServices::JsonToVector(*MinObj, MinV);
					FSololmcpEditorServices::JsonToVector(*MaxObj, MaxV);
					BoundsFilter = FBox(MinV, MaxV);
					bHasBoundsFilter = true;
				}
			}

			// Get editor world
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				OutError = TEXT("No editor world is available.");
				return false;
			}

			// Find navigation system
			UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
			if (!NavSys)
			{
				OutError = TEXT("NavigationSystem is not available. Make sure a NavMeshBoundsVolume exists in the level and navigation is built.");
				return false;
			}

			// Find RecastNavMesh
			ARecastNavMesh* RecastNavMesh = nullptr;
			const FString NavMeshId = Arguments->HasTypedField<EJson::String>(TEXT("nav_mesh_name"))
				? Arguments->GetStringField(TEXT("nav_mesh_name")) : FString();

			if (!NavMeshId.IsEmpty())
			{
				// Find by name/label
				for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
				{
					if (It->GetActorLabel() == NavMeshId || It->GetName() == NavMeshId)
					{
						RecastNavMesh = *It;
						break;
					}
				}
				if (!RecastNavMesh)
				{
					OutError = FString::Printf(TEXT("NavMesh actor '%s' not found."), *NavMeshId);
					return false;
				}
			}
			else
			{
				// Use default nav data
				ANavigationData* DefaultNavData = NavSys->GetDefaultNavDataInstance();
				RecastNavMesh = Cast<ARecastNavMesh>(DefaultNavData);
				if (!RecastNavMesh)
				{
					// Fallback: iterate
					for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
					{
						RecastNavMesh = *It;
						break;
					}
				}
			}

			if (!RecastNavMesh)
			{
				OutError = TEXT("No RecastNavMesh found. Build navigation first (Build > Build Paths).");
				return false;
			}

			// Access the underlying dtNavMesh
			const dtNavMesh* DetourNavMesh = RecastNavMesh->GetRecastMesh();
			if (!DetourNavMesh)
			{
				OutError = TEXT("Detour NavMesh data is null. Rebuild navigation.");
				return false;
			}

			// ── Extract agent parameters ────────────────────────────────
			TSharedRef<FJsonObject> AgentJson = MakeShared<FJsonObject>();
			AgentJson->SetNumberField(TEXT("radius"), RecastNavMesh->AgentRadius * CoordScale);
			AgentJson->SetNumberField(TEXT("height"), RecastNavMesh->AgentHeight * CoordScale);
			// UE 5.7+: AgentMaxStepHeight/CellSize/CellHeight moved to NavMeshResolutionParams
			{
				const FNavMeshResolutionParam& ResParam = RecastNavMesh->NavMeshResolutionParams[static_cast<uint8>(ENavigationDataResolution::Default)];
				AgentJson->SetNumberField(TEXT("max_climb"), ResParam.AgentMaxStepHeight * CoordScale);
				AgentJson->SetNumberField(TEXT("cell_size"), ResParam.CellSize * CoordScale);
				AgentJson->SetNumberField(TEXT("cell_height"), ResParam.CellHeight * CoordScale);
			}
			AgentJson->SetNumberField(TEXT("max_slope_degrees"), RecastNavMesh->AgentMaxSlope);

			// ── Iterate all tiles and polygons ──────────────────────────
			TArray<TSharedPtr<FJsonValue>> PolygonsJson;
			TArray<TSharedPtr<FJsonValue>> OffMeshLinksJson;
			int32 TotalVertices = 0;
			int32 PolyIdCounter = 0;

			FVector NavBoundsMin(FLT_MAX);
			FVector NavBoundsMax(-FLT_MAX);

			const int32 MaxTiles = DetourNavMesh->getMaxTiles();
			for (int32 TileIdx = 0; TileIdx < MaxTiles; ++TileIdx)
			{
				const dtMeshTile* Tile = DetourNavMesh->getTile(TileIdx);
				if (!Tile || !Tile->header)
				{
					continue;
				}

				const dtMeshHeader* Header = Tile->header;

				for (int32 PolyIdx = 0; PolyIdx < Header->polyCount; ++PolyIdx)
				{
					const dtPoly& Poly = Tile->polys[PolyIdx];

					// Skip off-mesh connection polys (handled separately)
					// UE 5.7+: DT_POLYTYPE_OFFMESH_CONNECTION may be scoped enum
					if (Poly.getType() != 0) // 0 = DT_POLYTYPE_GROUND
					{
						continue;
					}

					// Extract vertices
					TArray<TSharedPtr<FJsonValue>> VerticesJson;
					FVector Center(0.0f);
					const int32 NVerts = Poly.vertCount;

					bool bInBounds = !bHasBoundsFilter; // If no filter, always in bounds

					for (int32 Vi = 0; Vi < NVerts; ++Vi)
					{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
						const dtTileVert& LocalVert = Tile->verts[Poly.verts[Vi]];
						dtReal V[3] = {0, 0, 0};
						LocalVert.toWorld(Header->bmin, V);
#else
						// UE 5.7: Tile->verts is dtReal* with 3 values per vertex.
						const auto* V = &Tile->verts[Poly.verts[Vi] * 3];
#endif
						// Recast uses: X=X, Y=Z(up), Z=Y in UE convention
						const FVector WorldVert(
							static_cast<float>(V[0]) * CoordScale,
							static_cast<float>(V[2]) * CoordScale,
							static_cast<float>(V[1]) * CoordScale);
						Center += WorldVert;

						NavBoundsMin = NavBoundsMin.ComponentMin(WorldVert);
						NavBoundsMax = NavBoundsMax.ComponentMax(WorldVert);

						// Use WorldVert (already axis-swapped + scaled) so bounds filter
						// matches the coordinate space the user provides min/max_bounds in.
						if (bHasBoundsFilter && BoundsFilter.IsInsideOrOn(WorldVert))
						{
							bInBounds = true;
						}

						if (bIncludeVertices)
						{
							TArray<TSharedPtr<FJsonValue>> Coords;
							Coords.Add(MakeShared<FJsonValueNumber>(WorldVert.X));
							Coords.Add(MakeShared<FJsonValueNumber>(WorldVert.Y));
							Coords.Add(MakeShared<FJsonValueNumber>(WorldVert.Z));
							VerticesJson.Add(MakeShared<FJsonValueArray>(Coords));
						}
					}

					if (!bInBounds)
					{
						continue;
					}

					if (NVerts > 0)
					{
						Center /= static_cast<float>(NVerts);
					}
					TotalVertices += NVerts;

					TSharedRef<FJsonObject> PolyJson = MakeShared<FJsonObject>();
					PolyJson->SetNumberField(TEXT("id"), PolyIdCounter);

					if (bIncludeVertices)
					{
						PolyJson->SetArrayField(TEXT("vertices"), VerticesJson);
					}
					PolyJson->SetNumberField(TEXT("vertex_count"), NVerts);

					// Center
					TArray<TSharedPtr<FJsonValue>> CenterArr;
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.X));
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Y));
					CenterArr.Add(MakeShared<FJsonValueNumber>(Center.Z));
					PolyJson->SetArrayField(TEXT("center"), CenterArr);

					// Area class
					PolyJson->SetNumberField(TEXT("area_id"), Poly.getArea());

					// Flags
					PolyJson->SetNumberField(TEXT("flags"), Poly.flags);

					// Neighbors
					if (bIncludeNeighbors)
					{
						TArray<TSharedPtr<FJsonValue>> NeighborsJson;
						for (int32 Ni = 0; Ni < NVerts; ++Ni)
						{
							const dtPolyRef NeighRef = Poly.neis[Ni];
							if (NeighRef != 0)
							{
								// Internal neighbor within same tile
								if (NeighRef & DT_EXT_LINK)
								{
									// External link to another tile — store as negative for identification
									NeighborsJson.Add(MakeShared<FJsonValueNumber>(-(int32)(NeighRef & 0xFFFF)));
								}
								else
								{
									NeighborsJson.Add(MakeShared<FJsonValueNumber>((int32)NeighRef - 1));
								}
							}
						}
						PolyJson->SetArrayField(TEXT("neighbors"), NeighborsJson);
					}

					// Tile info
					PolyJson->SetNumberField(TEXT("tile_index"), TileIdx);
					PolyJson->SetNumberField(TEXT("poly_index_in_tile"), PolyIdx);

					PolygonsJson.Add(MakeShared<FJsonValueObject>(PolyJson));
					++PolyIdCounter;
				}

				// ── Off-mesh links ──────────────────────────────────────
				if (bIncludeOffMeshLinks)
				{
					for (int32 LinkIdx = 0; LinkIdx < Header->offMeshConCount; ++LinkIdx)
					{
						const dtOffMeshConnection& Conn = Tile->offMeshCons[LinkIdx];

						TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();

						// Start point
						TArray<TSharedPtr<FJsonValue>> StartArr;
						StartArr.Add(MakeShared<FJsonValueNumber>(Conn.pos[0] * CoordScale));
						StartArr.Add(MakeShared<FJsonValueNumber>(Conn.pos[2] * CoordScale));
						StartArr.Add(MakeShared<FJsonValueNumber>(Conn.pos[1] * CoordScale));
						LinkJson->SetArrayField(TEXT("start"), StartArr);

						// End point
						TArray<TSharedPtr<FJsonValue>> EndArr;
						EndArr.Add(MakeShared<FJsonValueNumber>(Conn.pos[3] * CoordScale));
						EndArr.Add(MakeShared<FJsonValueNumber>(Conn.pos[5] * CoordScale));
						EndArr.Add(MakeShared<FJsonValueNumber>(Conn.pos[4] * CoordScale));
						LinkJson->SetArrayField(TEXT("end"), EndArr);

						LinkJson->SetNumberField(TEXT("radius"), Conn.rad * CoordScale);
						LinkJson->SetBoolField(TEXT("bidirectional"), (Conn.flags & DT_OFFMESH_CON_BIDIR) != 0);
						// FIXED: Conn.poly is an encoded dtPolyRef, not a local index.
						// Use decodePolyIdPoly() to extract the local poly index within the tile.
						{
							int32 LinkAreaId = 0;
							if (Conn.poly != 0)
							{
								const unsigned int LocalPolyIdx = DetourNavMesh->decodePolyIdPoly(Conn.poly);
								if (LocalPolyIdx < static_cast<unsigned int>(Header->polyCount))
								{
									LinkAreaId = Tile->polys[LocalPolyIdx].getArea();
								}
							}
							LinkJson->SetNumberField(TEXT("area_id"), LinkAreaId);
						}
						LinkJson->SetNumberField(TEXT("flags"), Conn.flags);
						LinkJson->SetNumberField(TEXT("user_id"), Conn.userId);

						OffMeshLinksJson.Add(MakeShared<FJsonValueObject>(LinkJson));
					}
				}
			}

			if (PolygonsJson.Num() == 0)
			{
				OutError = TEXT("NavMesh contains no polygons. Build navigation first.");
				return false;
			}

			// ── Build root JSON ─────────────────────────────────────────
			TSharedRef<FJsonObject> RootJson = MakeShared<FJsonObject>();
			RootJson->SetNumberField(TEXT("version"), 1);
			RootJson->SetStringField(TEXT("world"), World->GetMapName());
			RootJson->SetStringField(TEXT("navmesh_actor"), RecastNavMesh->GetActorLabel());

			// Bounds
			TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
			{
				TSharedRef<FJsonObject> MinJson = MakeShared<FJsonObject>();
				MinJson->SetNumberField(TEXT("x"), NavBoundsMin.X);
				MinJson->SetNumberField(TEXT("y"), NavBoundsMin.Y);
				MinJson->SetNumberField(TEXT("z"), NavBoundsMin.Z);
				BoundsJson->SetObjectField(TEXT("min"), MinJson);

				TSharedRef<FJsonObject> MaxJson = MakeShared<FJsonObject>();
				MaxJson->SetNumberField(TEXT("x"), NavBoundsMax.X);
				MaxJson->SetNumberField(TEXT("y"), NavBoundsMax.Y);
				MaxJson->SetNumberField(TEXT("z"), NavBoundsMax.Z);
				BoundsJson->SetObjectField(TEXT("max"), MaxJson);
			}
			RootJson->SetObjectField(TEXT("bounds"), BoundsJson);

			// Agent
			RootJson->SetObjectField(TEXT("agent"), AgentJson);
			RootJson->SetNumberField(TEXT("coordinate_scale"), CoordScale);

			// Polygons
			RootJson->SetArrayField(TEXT("polygons"), PolygonsJson);
			RootJson->SetNumberField(TEXT("total_polygons"), PolygonsJson.Num());
			RootJson->SetNumberField(TEXT("total_vertices"), TotalVertices);

			// Off-mesh links
			if (bIncludeOffMeshLinks && OffMeshLinksJson.Num() > 0)
			{
				RootJson->SetArrayField(TEXT("off_mesh_links"), OffMeshLinksJson);
				RootJson->SetNumberField(TEXT("total_off_mesh_links"), OffMeshLinksJson.Num());
			}

			// Timestamp
			RootJson->SetStringField(TEXT("export_timestamp"), FDateTime::UtcNow().ToIso8601());

			// ── Serialize and save ──────────────────────────────────────
			FString JsonString;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
			FJsonSerializer::Serialize(RootJson, Writer);

			// Ensure directory exists
			const FString Dir = FPaths::GetPath(OutputPath);
			if (!Dir.IsEmpty())
			{
				IFileManager::Get().MakeDirectory(*Dir, true);
			}

			if (!FFileHelper::SaveStringToFile(JsonString, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(TEXT("Failed to write file: %s"), *OutputPath);
				return false;
			}

			// ── Output ─────────────────────────────────────────────────
			OutStructured->SetStringField(TEXT("output_path"), OutputPath);
			OutStructured->SetStringField(TEXT("world"), World->GetMapName());
			OutStructured->SetStringField(TEXT("navmesh_actor"), RecastNavMesh->GetActorLabel());
			OutStructured->SetNumberField(TEXT("total_polygons"), PolygonsJson.Num());
			OutStructured->SetNumberField(TEXT("total_vertices"), TotalVertices);
			OutStructured->SetNumberField(TEXT("total_off_mesh_links"), OffMeshLinksJson.Num());
			OutStructured->SetNumberField(TEXT("file_size_bytes"), JsonString.Len());
			OutStructured->SetObjectField(TEXT("bounds"), BoundsJson);
			OutStructured->SetObjectField(TEXT("agent"), AgentJson);

			OutSummary = FString::Printf(
				TEXT("Exported NavMesh '%s' → %s (%d polygons, %d vertices, %d off-mesh links, %.1f KB)"),
				*RecastNavMesh->GetActorLabel(), *OutputPath,
				PolygonsJson.Num(), TotalVertices, OffMeshLinksJson.Num(),
				static_cast<float>(JsonString.Len()) / 1024.0f);
			return true;
		}
	});

} // RegisterTerrainStreamingTools

} // namespace UE::SOMOLMCP
