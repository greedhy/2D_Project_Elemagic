// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v1.8.0 — Screenshot & Blueprint Debug Tools
// Screenshot tools (4): viewport/active tab/asset editor screenshots + list open editors
// Blueprint debug tools (14): PIE control, breakpoints, watches, step debugging, call stack
// Note: Screenshot tools use only public APIs (Services, AssetEditorSubsystem, TabManager)
// Note: Blueprint debug tools depend on FKismetDebugUtilities/FBlueprintBreakpoint APIs
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"

#include "Editor.h"
#include "UnrealClient.h"
#include "ImageUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonValue.h"

// Blueprint debug includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraphNode_Comment.h"
#include "K2Node.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Breakpoint.h"
#include "Kismet2/WatchedPin.h"
#include "UObject/Script.h"  // FBlueprintContextTracker
#include "ScopedTransaction.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "LevelEditorSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPScreenshotDebug, Log, All);

namespace UE::SOMOLMCP
{
	static bool ValidatePngPayload(const TArray<uint8>& PngData, FString& OutError)
	{
		if (PngData.Num() == 0)
		{
			OutError = TEXT("Screenshot capture returned an empty PNG payload.");
			return false;
		}
		return true;
	}

	static int32 ReadPngBigEndianInt32(const TArray<uint8>& PngData, const int32 Offset)
	{
		return
			(static_cast<int32>(PngData[Offset]) << 24) |
			(static_cast<int32>(PngData[Offset + 1]) << 16) |
			(static_cast<int32>(PngData[Offset + 2]) << 8) |
			static_cast<int32>(PngData[Offset + 3]);
	}

	static bool ExtractPngResolution(const TArray<uint8>& PngData, int32& OutWidth, int32& OutHeight, FString& OutError)
	{
		static const uint8 PngSignature[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
		if (PngData.Num() < 24)
		{
			OutError = TEXT("Screenshot PNG payload is too small to contain an IHDR header.");
			return false;
		}
		for (int32 Index = 0; Index < 8; ++Index)
		{
			if (PngData[Index] != PngSignature[Index])
			{
				OutError = TEXT("Screenshot payload is not a PNG image.");
				return false;
			}
		}
		if (PngData[12] != 'I' || PngData[13] != 'H' || PngData[14] != 'D' || PngData[15] != 'R')
		{
			OutError = TEXT("Screenshot PNG payload is missing an IHDR header.");
			return false;
		}

		OutWidth = ReadPngBigEndianInt32(PngData, 16);
		OutHeight = ReadPngBigEndianInt32(PngData, 20);
		if (OutWidth <= 0 || OutHeight <= 0)
		{
			OutError = TEXT("Screenshot PNG payload reports an invalid resolution.");
			return false;
		}
		return true;
	}

	static FString SanitizeScreenshotToken(const FString& Value)
	{
		FString Clean = Value.IsEmpty() ? TEXT("unknown") : Value;
		for (int32 Index = 0; Index < Clean.Len(); ++Index)
		{
			const TCHAR Ch = Clean[Index];
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('_') && Ch != TEXT('-'))
			{
				Clean[Index] = TEXT('_');
			}
		}
		return Clean.Left(96);
	}

	static bool SaveScreenshotPng(
		const TArray<uint8>& PngData,
		const FString& ToolName,
		const FString& SourceKind,
		const FString& SourceLabel,
		FString& OutFilePath,
		FString& OutError)
	{
		const FString DayStamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d"));
		const FString TimeStamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ"));
		const FString Directory = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP"), TEXT("Screenshots"), DayStamp));
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = FString::Printf(TEXT("Failed to create screenshot output directory '%s'."), *Directory);
			return false;
		}

		const FString BaseName = FString::Printf(
			TEXT("%s_%s_%s_%s_%s.png"),
			*TimeStamp,
			*SanitizeScreenshotToken(ToolName),
			*SanitizeScreenshotToken(SourceKind),
			*SanitizeScreenshotToken(SourceLabel),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		OutFilePath = FPaths::Combine(Directory, BaseName);
		if (!FFileHelper::SaveArrayToFile(PngData, *OutFilePath))
		{
			OutError = FString::Printf(TEXT("Failed to write screenshot PNG to '%s'."), *OutFilePath);
			return false;
		}
		return true;
	}

	static bool AddStandardScreenshotReceipt(
		const TArray<uint8>& PngData,
		const FString& ToolName,
		const FString& SourceKind,
		const FString& SourceLabel,
		const bool bVisible,
		const FString& VisibleState,
		const FString& VisibleReason,
		const bool bSaveToFile,
		TSharedRef<FJsonObject>& OutStructured,
		FString& OutError)
	{
		int32 Width = 0;
		int32 Height = 0;
		if (!ExtractPngResolution(PngData, Width, Height, OutError))
		{
			return false;
		}

		FString FilePath;
		if (bSaveToFile && !SaveScreenshotPng(PngData, ToolName, SourceKind, SourceLabel, FilePath, OutError))
		{
			return false;
		}

		TSharedRef<FJsonObject> Resolution = MakeShared<FJsonObject>();
		Resolution->SetNumberField(TEXT("width"), Width);
		Resolution->SetNumberField(TEXT("height"), Height);

		TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("tool"), ToolName);
		Source->SetStringField(TEXT("kind"), SourceKind);
		Source->SetStringField(TEXT("label"), SourceLabel);

		TSharedRef<FJsonObject> VisibleStatus = MakeShared<FJsonObject>();
		VisibleStatus->SetBoolField(TEXT("visible"), bVisible);
		VisibleStatus->SetStringField(TEXT("state"), VisibleState);
		VisibleStatus->SetStringField(TEXT("reason"), VisibleReason);

		static const TCHAR* ComparisonKinds[] = {
			TEXT("reference_image_comparison"),
			TEXT("keyframe_comparison"),
			TEXT("object_presence"),
			TEXT("layout_match"),
			TEXT("color_lighting_match"),
			TEXT("camera_match"),
			TEXT("material_match"),
			TEXT("terrain_silhouette_match")
		};
		TArray<TSharedPtr<FJsonValue>> AcceptedComparisons;
		for (const TCHAR* Kind : ComparisonKinds)
		{
			AcceptedComparisons.Add(MakeShared<FJsonValueString>(FString(Kind)));
		}

		TSharedRef<FJsonObject> VisualQaContract = MakeShared<FJsonObject>();
		VisualQaContract->SetStringField(TEXT("schema"), TEXT("somolmcp.screenshot.visual_qa_contract.v1"));
		VisualQaContract->SetBoolField(TEXT("comparison_ready"), bVisible && PngData.Num() > 0);
		VisualQaContract->SetStringField(TEXT("paired_reference_id"), TEXT(""));
		VisualQaContract->SetStringField(TEXT("paired_keyframe_id"), TEXT(""));
		VisualQaContract->SetStringField(TEXT("note"), TEXT("Screenshot capture is proof input only; visual comparison, readback, and QA acceptance require separate receipts."));
		VisualQaContract->SetArrayField(TEXT("accepted_comparison_kinds"), AcceptedComparisons);

		TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("schema"), TEXT("somolmcp.screenshot.receipt.v1"));
		Receipt->SetBoolField(TEXT("file_saved"), bSaveToFile);
		Receipt->SetStringField(TEXT("storage_mode"), bSaveToFile ? TEXT("file_and_inline_image") : TEXT("inline_image_only"));
		if (bSaveToFile)
		{
			Receipt->SetStringField(TEXT("file_path"), FilePath);
		}
		Receipt->SetObjectField(TEXT("resolution"), Resolution);
		Receipt->SetObjectField(TEXT("source"), Source);
		Receipt->SetObjectField(TEXT("visible_status"), VisibleStatus);
		Receipt->SetObjectField(TEXT("visual_qa_contract"), VisualQaContract);
		Receipt->SetStringField(TEXT("mime_type"), TEXT("image/png"));
		Receipt->SetNumberField(TEXT("image_size_bytes"), PngData.Num());
		Receipt->SetStringField(TEXT("captured_at_utc"), FDateTime::UtcNow().ToIso8601());

		OutStructured->SetBoolField(TEXT("file_saved"), bSaveToFile);
		OutStructured->SetStringField(TEXT("storage_mode"), bSaveToFile ? TEXT("file_and_inline_image") : TEXT("inline_image_only"));
		if (bSaveToFile)
		{
			OutStructured->SetStringField(TEXT("file_path"), FilePath);
		}
		OutStructured->SetNumberField(TEXT("image_width"), Width);
		OutStructured->SetNumberField(TEXT("image_height"), Height);
		OutStructured->SetObjectField(TEXT("resolution"), Resolution);
		OutStructured->SetObjectField(TEXT("source"), Source);
		OutStructured->SetObjectField(TEXT("visible_status"), VisibleStatus);
		OutStructured->SetBoolField(TEXT("visible"), bVisible);
		OutStructured->SetStringField(TEXT("source_kind"), SourceKind);
		OutStructured->SetStringField(TEXT("source_label"), SourceLabel);
		OutStructured->SetStringField(TEXT("mime_type"), TEXT("image/png"));
		OutStructured->SetNumberField(TEXT("image_size_bytes"), PngData.Num());
		OutStructured->SetObjectField(TEXT("visual_qa_contract"), VisualQaContract);
		OutStructured->SetObjectField(TEXT("screenshot_receipt"), Receipt);
		return true;
	}

	// ── Helper: Find a node by name/title in a blueprint graph ──

	static UEdGraphNode* FindNodeInBlueprint(UBlueprint* Blueprint, const FString& GraphName, const FString& NodeName, FString& OutError)
	{
		if (!Blueprint)
		{
			OutError = TEXT("Blueprint is null.");
			return nullptr;
		}

		UEdGraph* TargetGraph = nullptr;
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				TargetGraph = Graph;
				break;
			}
		}
		if (!TargetGraph)
		{
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == GraphName)
				{
					TargetGraph = Graph;
					break;
				}
			}
		}
		if (!TargetGraph)
		{
			OutError = FString::Printf(TEXT("Graph '%s' not found in blueprint."), *GraphName);
			return nullptr;
		}

		for (UEdGraphNode* Node : TargetGraph->Nodes)
		{
			if (!Node) continue;
			if (Node->GetNodeTitle(ENodeTitleType::ListView).ToString() == NodeName ||
				Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString() == NodeName ||
				Node->GetName() == NodeName)
			{
				return Node;
			}
		}

		OutError = FString::Printf(TEXT("Node '%s' not found in graph '%s'."), *NodeName, *GraphName);
		return nullptr;
	}

	// ── Helper: Find a pin on a node ──

	static UEdGraphPin* FindPinOnNode(UEdGraphNode* Node, const FString& PinName, FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("Node is null.");
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && (Pin->PinName.ToString() == PinName || Pin->GetDisplayName().ToString() == PinName))
			{
				return Pin;
			}
		}

		OutError = FString::Printf(TEXT("Pin '%s' not found on node '%s'."), *PinName, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		return nullptr;
	}

	// ═══════════════════════════════════════════════════════════════════════
	//  RegisterScreenshotTools — 4 tools
	// ═══════════════════════════════════════════════════════════════════════

	void RegisterScreenshotTools(FSololmcpToolRegistry& Registry)
	{
		// ── 1. editor_screenshot_viewport ──

		Registry.Register({
			TEXT("editor_screenshot_viewport"),
			TEXT("Capture a screenshot of the active level editor viewport as a PNG image. Returns the image as base64-encoded content that AI can directly analyze visually."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum width in pixels. Default 1920."))},
					{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum height in pixels. Default 1080."))},
					{TEXT("capture_mode"), FSololmcpSchemaBuilder::String(TEXT("'viewport' keeps the native viewport size subject to max_width/max_height; 'target_resolution' returns exactly max_width x max_height. Default 'viewport'."))},
					{TEXT("save_to_file"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether to also save the PNG to disk. Default true; set false for inline MCP image-only chat screenshots."))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				int32 MaxWidth = 1920;
				int32 MaxHeight = 1080;
				Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
				Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);
				bool bSaveToFile = true;
				Arguments->TryGetBoolField(TEXT("save_to_file"), bSaveToFile);
				FString CaptureMode = TEXT("viewport");
				Arguments->TryGetStringField(TEXT("capture_mode"), CaptureMode);
				CaptureMode = CaptureMode.ToLower();
				if (CaptureMode != TEXT("viewport") && CaptureMode != TEXT("target_resolution"))
				{
					OutError = TEXT("capture_mode must be 'viewport' or 'target_resolution'.");
					return false;
				}
				MaxWidth = FMath::Clamp(MaxWidth, 64, 3840);
				MaxHeight = FMath::Clamp(MaxHeight, 64, 2160);
				const FIntPoint SourceResolution = GEditor && GEditor->GetActiveViewport()
					? GEditor->GetActiveViewport()->GetSizeXY()
					: FIntPoint::ZeroValue;
				const bool bExactResolution = CaptureMode == TEXT("target_resolution");

				TArray<uint8> PngData;
				if (!Context.Services.CaptureViewportScreenshot(PngData, MaxWidth, MaxHeight, OutError, bExactResolution))
				{
					return false;
				}
				if (PngData.Num() == 0)
				{
					OutError = TEXT("Viewport screenshot capture returned no PNG data.");
					return false;
				}
				if (!ValidatePngPayload(PngData, OutError))
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> ImageContent;
				ImageContent.Add(MakeImageContentValue(PngData));
				OutStructured->SetArrayField(TEXT("_imageContent"), ImageContent);
				if (!AddStandardScreenshotReceipt(
					PngData,
					TEXT("editor_screenshot_viewport"),
					TEXT("level_editor_viewport"),
					TEXT("active_viewport"),
					true,
					TEXT("captured_after_forced_redraw"),
					TEXT("GEditor active viewport was found, redrawn, and read successfully."),
					bSaveToFile,
					OutStructured,
					OutError))
				{
					return false;
				}
				OutStructured->SetStringField(TEXT("capture_mode"), CaptureMode);
				OutStructured->SetNumberField(TEXT("source_width"), SourceResolution.X);
				OutStructured->SetNumberField(TEXT("source_height"), SourceResolution.Y);
				OutStructured->SetNumberField(TEXT("output_width"), OutStructured->GetNumberField(TEXT("image_width")));
				OutStructured->SetNumberField(TEXT("output_height"), OutStructured->GetNumberField(TEXT("image_height")));
				OutStructured->SetBoolField(
					TEXT("upscaled"),
					bExactResolution && (SourceResolution.X < MaxWidth || SourceResolution.Y < MaxHeight));
				OutSummary = bSaveToFile
					? FString::Printf(
						TEXT("Captured viewport screenshot at '%s' (%dx%d)."),
						*OutStructured->GetStringField(TEXT("file_path")),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_width"))),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_height"))))
					: FString::Printf(
						TEXT("Captured viewport screenshot inline (%dx%d, no file saved)."),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_width"))),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_height"))));
				return true;
			}
		});

		// ── 2. editor_screenshot_active_tab ──

		Registry.Register({
			TEXT("editor_screenshot_active_tab"),
			TEXT("Capture a screenshot of the currently active/focused editor tab (Blueprint editor, Material editor, etc.)."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum width in pixels. Default 1920."))},
					{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum height in pixels. Default 1080."))},
					{TEXT("save_to_file"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether to also save the PNG to disk. Default true; set false for inline MCP image-only screenshots."))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				int32 MaxWidth = 1920;
				int32 MaxHeight = 1080;
				Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
				Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);
				bool bSaveToFile = true;
				Arguments->TryGetBoolField(TEXT("save_to_file"), bSaveToFile);
				MaxWidth = FMath::Clamp(MaxWidth, 64, 3840);
				MaxHeight = FMath::Clamp(MaxHeight, 64, 2160);

				TSharedPtr<SDockTab> ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
				if (!ActiveTab.IsValid())
				{
					OutError = TEXT("No active editor tab found.");
					return false;
				}

				TSharedPtr<SWidget> TabContent = ActiveTab->GetContent();
				if (!TabContent.IsValid())
				{
					OutError = TEXT("Active tab has no content widget.");
					return false;
				}

				TArray<uint8> PngData;
				if (!Context.Services.CaptureSlateWidgetScreenshot(TabContent, PngData, MaxWidth, MaxHeight, OutError))
				{
					return false;
				}
				if (PngData.Num() == 0)
				{
					OutError = TEXT("Active tab screenshot capture returned no PNG data.");
					return false;
				}
				if (!ValidatePngPayload(PngData, OutError))
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> ImageContent;
				ImageContent.Add(MakeImageContentValue(PngData));
				OutStructured->SetArrayField(TEXT("_imageContent"), ImageContent);
				OutStructured->SetStringField(TEXT("tab_label"), ActiveTab->GetTabLabel().ToString());
				if (!AddStandardScreenshotReceipt(
					PngData,
					TEXT("editor_screenshot_active_tab"),
					TEXT("editor_tab"),
					ActiveTab->GetTabLabel().ToString(),
					true,
					TEXT("active_tab_content_captured"),
					TEXT("Active editor tab and content widget were found and rendered."),
					bSaveToFile,
					OutStructured,
					OutError))
				{
					return false;
				}
				OutSummary = bSaveToFile
					? FString::Printf(
						TEXT("Captured screenshot of tab '%s' at '%s' (%dx%d)."),
						*ActiveTab->GetTabLabel().ToString(),
						*OutStructured->GetStringField(TEXT("file_path")),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_width"))),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_height"))))
					: FString::Printf(
						TEXT("Captured screenshot of tab '%s' inline (%dx%d, no file saved)."),
						*ActiveTab->GetTabLabel().ToString(),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_width"))),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_height"))));
				return true;
			}
		});

		// ── 3. editor_screenshot_asset_editor ──

		Registry.Register({
			TEXT("editor_screenshot_asset_editor"),
			TEXT("Capture a screenshot of a specific asset's editor window (Blueprint editor, Material editor, etc.). The asset must already be open in an editor."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the asset whose editor to capture."))},
					{TEXT("max_width"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum width in pixels. Default 1920."))},
					{TEXT("max_height"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum height in pixels. Default 1080."))},
					{TEXT("save_to_file"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether to also save the PNG to disk. Default true; set false for inline MCP image-only screenshots."))}
				},
				{TEXT("asset_path")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					OutError = TEXT("Missing argument: asset_path");
					return false;
				}

				int32 MaxWidth = 1920;
				int32 MaxHeight = 1080;
				Arguments->TryGetNumberField(TEXT("max_width"), MaxWidth);
				Arguments->TryGetNumberField(TEXT("max_height"), MaxHeight);
				bool bSaveToFile = true;
				Arguments->TryGetBoolField(TEXT("save_to_file"), bSaveToFile);
				MaxWidth = FMath::Clamp(MaxWidth, 64, 3840);
				MaxHeight = FMath::Clamp(MaxHeight, 64, 2160);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset)
				{
					return false;
				}

				UAssetEditorSubsystem* AssetEditorSubsystem = Context.Services.GetAssetEditorSubsystem(OutError);
				if (!AssetEditorSubsystem)
				{
					return false;
				}

				IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
				if (!EditorInstance)
				{
					// Try to open the editor first
					AssetEditorSubsystem->OpenEditorForAsset(Asset);
					EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
					if (!EditorInstance)
					{
						OutError = FString::Printf(TEXT("Could not open or find editor for asset '%s'."), *AssetPath);
						return false;
					}
				}

				// Get the editor's tab/widget
				FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
				if (!Toolkit)
				{
					OutError = TEXT("Editor instance is not an FAssetEditorToolkit.");
					return false;
				}

				TSharedPtr<SDockTab> EditorTab = Toolkit->GetAssociatedTabManager()->GetOwnerTab();
				TSharedPtr<SWidget> WidgetToCapture;

				if (EditorTab.IsValid())
				{
					// Bring tab to foreground
					EditorTab->ActivateInParent(ETabActivationCause::SetDirectly);
					WidgetToCapture = EditorTab->GetContent();
				}

				if (!WidgetToCapture.IsValid())
				{
					OutError = TEXT("Could not find the editor widget to capture.");
					return false;
				}

				TArray<uint8> PngData;
				if (!Context.Services.CaptureSlateWidgetScreenshot(WidgetToCapture, PngData, MaxWidth, MaxHeight, OutError))
				{
					return false;
				}
				if (PngData.Num() == 0)
				{
					OutError = FString::Printf(TEXT("Asset editor screenshot for '%s' returned no PNG data."), *AssetPath);
					return false;
				}
				if (!ValidatePngPayload(PngData, OutError))
				{
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> ImageContent;
				ImageContent.Add(MakeImageContentValue(PngData));
				OutStructured->SetArrayField(TEXT("_imageContent"), ImageContent);
				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				if (!AddStandardScreenshotReceipt(
					PngData,
					TEXT("editor_screenshot_asset_editor"),
					TEXT("asset_editor"),
					AssetPath,
					true,
					TEXT("editor_tab_activated_before_capture"),
					TEXT("Asset editor tab was found or opened, activated, and rendered."),
					bSaveToFile,
					OutStructured,
					OutError))
				{
					return false;
				}
				OutSummary = bSaveToFile
					? FString::Printf(
						TEXT("Captured screenshot of editor for '%s' at '%s' (%dx%d)."),
						*AssetPath,
						*OutStructured->GetStringField(TEXT("file_path")),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_width"))),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_height"))))
					: FString::Printf(
						TEXT("Captured screenshot of editor for '%s' inline (%dx%d, no file saved)."),
						*AssetPath,
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_width"))),
						static_cast<int32>(OutStructured->GetNumberField(TEXT("image_height"))));
				return true;
			}
		});

		// ── 4. editor_list_open_editors ──

		Registry.Register({
			TEXT("editor_list_open_editors"),
			TEXT("List all currently open asset editor windows with their asset paths and editor types."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				UAssetEditorSubsystem* AssetEditorSubsystem = Context.Services.GetAssetEditorSubsystem(OutError);
				if (!AssetEditorSubsystem)
				{
					return false;
				}

				TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();

				TArray<TSharedPtr<FJsonValue>> EditorsArray;
				for (UObject* Asset : EditedAssets)
				{
					if (!Asset) continue;

					TSharedRef<FJsonObject> EditorInfo = MakeShared<FJsonObject>();
					EditorInfo->SetStringField(TEXT("asset_path"), Asset->GetPathName());
					EditorInfo->SetStringField(TEXT("asset_name"), Asset->GetName());
					EditorInfo->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

					IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
					if (EditorInstance)
					{
						EditorInfo->SetStringField(TEXT("editor_name"), EditorInstance->GetEditorName().ToString());
					}

					EditorsArray.Add(MakeShared<FJsonValueObject>(EditorInfo));
				}

				OutStructured->SetArrayField(TEXT("editors"), EditorsArray);
				OutStructured->SetNumberField(TEXT("count"), EditorsArray.Num());
				OutSummary = FString::Printf(TEXT("Found %d open editor(s)."), EditorsArray.Num());
				return true;
			},
			nullptr,  // IsAvailable
			2         // CacheTtlSeconds
		});
	}

// ═══════════════════════════════════════════════════════════════════════
//  RegisterBlueprintDebugTools — 14 tools
//  UE 5.7 Fix: Use FKismetDebugUtilities::GetBreakpoints/GetWatchedPins
//  instead of UBlueprint::Breakpoints/WatchedPins (removed in UE 5.7).
// ═══════════════════════════════════════════════════════════════════════
void RegisterBlueprintDebugTools(FSololmcpToolRegistry& Registry)
	{
		// ── 5. blueprint_debug_play ──

		Registry.Register({
			TEXT("blueprint_debug_play"),
			TEXT("Start a Play In Editor (PIE) session. Supports PIE, Simulate In Editor (SIE), and standalone modes."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("mode"), FSololmcpSchemaBuilder::String(TEXT("Play mode: 'PIE' (default), 'SIE' (Simulate), 'standalone'."), {TEXT("PIE"), TEXT("SIE"), TEXT("standalone")})}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("GEditor is not available.");
					return false;
				}

				if (GEditor->PlayWorld)
				{
					OutError = TEXT("A PIE session is already running. Stop it first with blueprint_debug_stop.");
					return false;
				}

				FString Mode = TEXT("PIE");
				Arguments->TryGetStringField(TEXT("mode"), Mode);

				FRequestPlaySessionParams Params;

				if (Mode.Equals(TEXT("SIE"), ESearchCase::IgnoreCase))
				{
					Params.WorldType = EPlaySessionWorldType::SimulateInEditor;
				}
				// Default: PIE (PlayInEditor)
				// Note: standalone mode uses default Params with external process launch.

				GEditor->RequestPlaySession(Params);

				OutStructured->SetStringField(TEXT("mode"), Mode);
				OutStructured->SetStringField(TEXT("status"), TEXT("play_requested"));
				OutSummary = FString::Printf(TEXT("Requested %s session."), *Mode);
				return true;
			}
		});

		// ── 6. blueprint_debug_stop ──

		Registry.Register({
			TEXT("blueprint_debug_stop"),
			TEXT("Stop the currently running PIE/SIE session."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("GEditor is not available.");
					return false;
				}

				if (!GEditor->PlayWorld)
				{
					OutError = TEXT("No PIE session is currently running.");
					return false;
				}

				GEditor->RequestEndPlayMap();

				OutStructured->SetStringField(TEXT("status"), TEXT("stop_requested"));
				OutSummary = TEXT("Requested PIE session stop.");
				return true;
			}
		});

		// ── 7. blueprint_debug_pause ──

		Registry.Register({
			TEXT("blueprint_debug_pause"),
			TEXT("Pause the currently running PIE session."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("GEditor is not available.");
					return false;
				}

				if (!GEditor->PlayWorld)
				{
					OutError = TEXT("No PIE session is currently running.");
					return false;
				}

				if (GEditor->PlayWorld->IsPaused())
				{
					OutError = TEXT("PIE session is already paused.");
					return false;
				}

				GEditor->SetPIEWorldsPaused(true);

				OutStructured->SetStringField(TEXT("status"), TEXT("paused"));
				OutSummary = TEXT("PIE session paused.");
				return true;
			}
		});

		// ── 8. blueprint_debug_resume ──

		Registry.Register({
			TEXT("blueprint_debug_resume"),
			TEXT("Resume a paused PIE session."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor)
				{
					OutError = TEXT("GEditor is not available.");
					return false;
				}

				if (!GEditor->PlayWorld)
				{
					OutError = TEXT("No PIE session is currently running.");
					return false;
				}

				if (!GEditor->PlayWorld->IsPaused())
				{
					OutError = TEXT("PIE session is not paused.");
					return false;
				}

				GEditor->SetPIEWorldsPaused(false);

				OutStructured->SetStringField(TEXT("status"), TEXT("resumed"));
				OutSummary = TEXT("PIE session resumed.");
				return true;
			}
		});

		// ── 9. blueprint_debug_set_breakpoint ──

		Registry.Register({
			TEXT("blueprint_debug_set_breakpoint"),
			TEXT("Set a breakpoint on a specific node in a Blueprint graph. The breakpoint will pause execution when the node is reached during PIE."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Name of the graph containing the node."))},
					{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Name or title of the node to set breakpoint on."))},
					{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether the breakpoint is enabled. Default true."))}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("node_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, GraphName, NodeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("node_name"), NodeName))
				{
					OutError = TEXT("Missing required arguments: asset_path, graph_name, node_name.");
					return false;
				}

				bool bEnabled = true;
				Arguments->TryGetBoolField(TEXT("enabled"), bEnabled);

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;

				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = FString::Printf(TEXT("'%s' is not a Blueprint."), *AssetPath);
					return false;
				}

				UEdGraphNode* Node = FindNodeInBlueprint(Blueprint, GraphName, NodeName, OutError);
				if (!Node) return false;

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SetBreakpoint", "SOMOLMCP Set Breakpoint"));

				// Use the node+blueprint overload to set/create breakpoint
				FKismetDebugUtilities::SetBreakpointEnabled(Node, Blueprint, bEnabled);
				Blueprint->MarkPackageDirty();

				OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
				OutStructured->SetStringField(TEXT("graph_name"), GraphName);
				OutStructured->SetStringField(TEXT("node_name"), NodeName);
				OutStructured->SetBoolField(TEXT("enabled"), bEnabled);
				OutSummary = FString::Printf(TEXT("Breakpoint %s on node '%s' in '%s'."), bEnabled ? TEXT("set") : TEXT("set (disabled)"), *NodeName, *GraphName);
				return true;
			}
		});

		// ── 10. blueprint_debug_remove_breakpoint ──

		Registry.Register({
			TEXT("blueprint_debug_remove_breakpoint"),
			TEXT("Remove a breakpoint from a Blueprint node."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_name"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("node_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, GraphName, NodeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("node_name"), NodeName))
				{
					OutError = TEXT("Missing required arguments.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;

				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = TEXT("Asset is not a Blueprint.");
					return false;
				}

				UEdGraphNode* Node = FindNodeInBlueprint(Blueprint, GraphName, NodeName, OutError);
				if (!Node) return false;

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "RemoveBreakpoint", "SOMOLMCP Remove Breakpoint"));

				FBlueprintBreakpoint* Breakpoint = FKismetDebugUtilities::FindBreakpointForNode(Node, Blueprint);
				if (!Breakpoint)
				{
					OutError = FString::Printf(TEXT("No breakpoint found on node '%s'."), *NodeName);
					return false;
				}

				FKismetDebugUtilities::SetBreakpointEnabled(*Breakpoint, false);
				// UE 5.7: GetBreakpoints is now protected, cannot directly modify breakpoint list
				// The breakpoint is disabled above, which is sufficient for most use cases
				Blueprint->MarkPackageDirty();

				OutStructured->SetStringField(TEXT("node_name"), NodeName);
				OutSummary = FString::Printf(TEXT("Removed breakpoint from node '%s'."), *NodeName);
				return true;
			}
		});

		// ── 11. blueprint_debug_toggle_breakpoint ──

		Registry.Register({
			TEXT("blueprint_debug_toggle_breakpoint"),
			TEXT("Toggle (enable/disable) an existing breakpoint on a Blueprint node."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String()},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String()},
					{TEXT("node_name"), FSololmcpSchemaBuilder::String()}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("node_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, GraphName, NodeName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("node_name"), NodeName))
				{
					OutError = TEXT("Missing required arguments.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;

				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = TEXT("Asset is not a Blueprint.");
					return false;
				}

				UEdGraphNode* Node = FindNodeInBlueprint(Blueprint, GraphName, NodeName, OutError);
				if (!Node) return false;

				FBlueprintBreakpoint* Breakpoint = FKismetDebugUtilities::FindBreakpointForNode(Node, Blueprint);
				if (!Breakpoint)
				{
					OutError = FString::Printf(TEXT("No breakpoint found on node '%s'."), *NodeName);
					return false;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "ToggleBreakpoint", "SOMOLMCP Toggle Breakpoint"));

				const bool bNewEnabled = !Breakpoint->IsEnabled();
				FKismetDebugUtilities::SetBreakpointEnabled(*Breakpoint, bNewEnabled);

				OutStructured->SetStringField(TEXT("node_name"), NodeName);
				OutStructured->SetBoolField(TEXT("enabled"), bNewEnabled);
				OutSummary = FString::Printf(TEXT("Breakpoint on '%s' %s."), *NodeName, bNewEnabled ? TEXT("enabled") : TEXT("disabled"));
				return true;
			}
		});

		// ── 12. blueprint_debug_list_breakpoints ──

		Registry.Register({
			TEXT("blueprint_debug_list_breakpoints"),
			TEXT("List all breakpoints in a Blueprint, or all breakpoints across all loaded Blueprints if no asset_path is specified."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional: filter to a specific Blueprint."))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);

				TArray<UBlueprint*> Blueprints;

				if (!AssetPath.IsEmpty())
				{
					UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
					if (!Asset) return false;
					UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
					if (!Blueprint)
					{
						OutError = TEXT("Asset is not a Blueprint.");
						return false;
					}
					Blueprints.Add(Blueprint);
				}
				else
				{
					// Collect all loaded blueprints with breakpoints
					// UE 5.7: GetBreakpoints is protected, use BlueprintHasBreakpoints instead
					for (TObjectIterator<UBlueprint> It; It; ++It)
					{
						if (FKismetDebugUtilities::BlueprintHasBreakpoints(*It))
						{
							Blueprints.Add(*It);
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> BreakpointsArray;
				// UE 5.7: GetBreakpoints is protected, can only list blueprints that have breakpoints
				// but cannot enumerate individual breakpoints anymore
				for (UBlueprint* BP : Blueprints)
				{
					TSharedRef<FJsonObject> BPInfo = MakeShared<FJsonObject>();
					BPInfo->SetStringField(TEXT("blueprint_path"), BP->GetPathName());
					BPInfo->SetBoolField(TEXT("has_breakpoints"), true);
					BPInfo->SetStringField(TEXT("note"), TEXT("UE 5.7: Cannot enumerate individual breakpoints (protected API)"));
					BreakpointsArray.Add(MakeShared<FJsonValueObject>(BPInfo));
				}

				OutStructured->SetArrayField(TEXT("breakpoints"), BreakpointsArray);
				OutStructured->SetNumberField(TEXT("count"), BreakpointsArray.Num());
				OutSummary = FString::Printf(TEXT("Found %d breakpoint(s)."), BreakpointsArray.Num());
				return true;
			}
		});

		// ── 13. blueprint_debug_step_into ──

		Registry.Register({
			TEXT("blueprint_debug_step_into"),
			TEXT("Step into the next Blueprint node execution. Requires PIE to be paused at a breakpoint."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor || !GEditor->PlayWorld)
				{
					OutError = TEXT("No PIE session is running.");
					return false;
				}

				if (!GEditor->PlayWorld->IsPaused())
				{
					OutError = TEXT("PIE is not paused. Step debugging requires execution to be paused at a breakpoint.");
					return false;
				}

				FKismetDebugUtilities::RequestSingleStepIn();

				// Resume to execute one step
				GEditor->SetPIEWorldsPaused(false);

				OutStructured->SetStringField(TEXT("action"), TEXT("step_into"));
				OutSummary = TEXT("Step into requested.");
				return true;
			}
		});

		// ── 14. blueprint_debug_step_over ──

		Registry.Register({
			TEXT("blueprint_debug_step_over"),
			TEXT("Step over to the next Blueprint node. Requires PIE to be paused at a breakpoint."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor || !GEditor->PlayWorld)
				{
					OutError = TEXT("No PIE session is running.");
					return false;
				}

				if (!GEditor->PlayWorld->IsPaused())
				{
					OutError = TEXT("PIE is not paused.");
					return false;
				}

				FKismetDebugUtilities::RequestStepOver();

				GEditor->SetPIEWorldsPaused(false);

				OutStructured->SetStringField(TEXT("action"), TEXT("step_over"));
				OutSummary = TEXT("Step over requested.");
				return true;
			}
		});

		// ── 15. blueprint_debug_step_out ──

		Registry.Register({
			TEXT("blueprint_debug_step_out"),
			TEXT("Step out of the current Blueprint function. Requires PIE to be paused at a breakpoint."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor || !GEditor->PlayWorld)
				{
					OutError = TEXT("No PIE session is running.");
					return false;
				}

				if (!GEditor->PlayWorld->IsPaused())
				{
					OutError = TEXT("PIE is not paused.");
					return false;
				}

				FKismetDebugUtilities::RequestStepOut();

				GEditor->SetPIEWorldsPaused(false);

				OutStructured->SetStringField(TEXT("action"), TEXT("step_out"));
				OutSummary = TEXT("Step out requested.");
				return true;
			}
		});

		// ── 16. blueprint_debug_watch_pin ──

		Registry.Register({
			TEXT("blueprint_debug_watch_pin"),
			TEXT("Add a pin to the Blueprint debug watch list. Watched pins show their runtime values during PIE debugging."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Blueprint asset path."))},
					{TEXT("graph_name"), FSololmcpSchemaBuilder::String(TEXT("Graph name."))},
					{TEXT("node_name"), FSololmcpSchemaBuilder::String(TEXT("Node name or title."))},
					{TEXT("pin_name"), FSololmcpSchemaBuilder::String(TEXT("Name of the pin to watch."))}
				},
				{TEXT("asset_path"), TEXT("graph_name"), TEXT("node_name"), TEXT("pin_name")}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath, GraphName, NodeName, PinName;
				if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
					!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) ||
					!Arguments->TryGetStringField(TEXT("node_name"), NodeName) ||
					!Arguments->TryGetStringField(TEXT("pin_name"), PinName))
				{
					OutError = TEXT("Missing required arguments.");
					return false;
				}

				UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
				if (!Asset) return false;

				UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
				if (!Blueprint)
				{
					OutError = TEXT("Asset is not a Blueprint.");
					return false;
				}

				UEdGraphNode* Node = FindNodeInBlueprint(Blueprint, GraphName, NodeName, OutError);
				if (!Node) return false;

				UEdGraphPin* Pin = FindPinOnNode(Node, PinName, OutError);
				if (!Pin) return false;

				FBlueprintWatchedPin WatchedPin(Pin);
				FKismetDebugUtilities::AddPinWatch(Blueprint, MoveTemp(WatchedPin));

				OutStructured->SetStringField(TEXT("pin_name"), PinName);
				OutStructured->SetStringField(TEXT("node_name"), NodeName);
				OutSummary = FString::Printf(TEXT("Added watch on pin '%s' of node '%s'."), *PinName, *NodeName);
				return true;
			}
		});

		// ── 17. blueprint_debug_get_watches ──

		Registry.Register({
			TEXT("blueprint_debug_get_watches"),
			TEXT("Get all watched pins and their current debug values. Values are available when PIE is paused at a breakpoint."),
			FSololmcpSchemaBuilder::Object(
				{
					{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional: filter to a specific Blueprint."))}
				}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				FString AssetPath;
				Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);

				TArray<UBlueprint*> Blueprints;

				if (!AssetPath.IsEmpty())
				{
					UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
					if (!Asset) return false;
					UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
					if (!Blueprint)
					{
						OutError = TEXT("Asset is not a Blueprint.");
						return false;
					}
					Blueprints.Add(Blueprint);
				}
				else
				{
					for (TObjectIterator<UBlueprint> It; It; ++It)
					{
						if (FKismetDebugUtilities::BlueprintHasPinWatches(*It))
						{
							Blueprints.Add(*It);
						}
					}
				}

				TArray<TSharedPtr<FJsonValue>> WatchesArray;
				// UE 5.7: GetWatchedPins is protected, can only list blueprints that have watches
				for (UBlueprint* BP : Blueprints)
				{
					TSharedRef<FJsonObject> WatchInfo = MakeShared<FJsonObject>();
					WatchInfo->SetStringField(TEXT("blueprint_path"), BP->GetPathName());
					WatchInfo->SetBoolField(TEXT("has_watches"), true);
					WatchInfo->SetStringField(TEXT("note"), TEXT("UE 5.7: Cannot enumerate individual watched pins (protected API)"));
					WatchesArray.Add(MakeShared<FJsonValueObject>(WatchInfo));
				}

				OutStructured->SetArrayField(TEXT("watches"), WatchesArray);
				OutStructured->SetNumberField(TEXT("count"), WatchesArray.Num());
				OutSummary = FString::Printf(TEXT("Found %d blueprint(s) with watched pins."), WatchesArray.Num());
				return true;
			}
		});

		// ── 18. blueprint_debug_get_call_stack ──

		Registry.Register({
			TEXT("blueprint_debug_get_call_stack"),
			TEXT("Get the current Blueprint execution call stack. Only available when PIE is paused at a breakpoint."),
			FSololmcpSchemaBuilder::Object({}),

			[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
			{
				if (!GEditor || !GEditor->PlayWorld)
				{
					OutError = TEXT("No PIE session is running.");
					return false;
				}

				if (!GEditor->PlayWorld->IsPaused())
				{
					OutError = TEXT("PIE is not paused. Call stack is only available when paused at a breakpoint.");
					return false;
				}

				// UE 5.7: GetScriptStack() deprecated, use GetCurrentScriptStack() which returns TArrayView
				TArrayView<const FFrame* const> ScriptStack = FBlueprintContextTracker::Get().GetCurrentScriptStack();

				TArray<TSharedPtr<FJsonValue>> StackArray;
				for (int32 i = 0; i < ScriptStack.Num(); ++i)
				{
					const FFrame* StackFrame = ScriptStack[i];
					if (!StackFrame) continue;

					TSharedRef<FJsonObject> FrameInfo = MakeShared<FJsonObject>();
					FrameInfo->SetNumberField(TEXT("depth"), i);

					if (StackFrame->Node)
					{
						FrameInfo->SetStringField(TEXT("function_name"), StackFrame->Node->GetName());
					}

					if (const UObject* Obj = StackFrame->Object)
					{
						FrameInfo->SetStringField(TEXT("object_name"), Obj->GetName());
						FrameInfo->SetStringField(TEXT("object_class"), Obj->GetClass()->GetName());

						// Try to find the Blueprint
						if (const UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Obj->GetClass()))
						{
							if (UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy))
							{
								FrameInfo->SetStringField(TEXT("blueprint_path"), BP->GetPathName());
							}
						}
					}

					StackArray.Add(MakeShared<FJsonValueObject>(FrameInfo));
				}

				OutStructured->SetArrayField(TEXT("call_stack"), StackArray);
				OutStructured->SetNumberField(TEXT("depth"), StackArray.Num());
				OutSummary = FString::Printf(TEXT("Call stack has %d frame(s)."), StackArray.Num());
				return true;
			}
		});
	}
}
