// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpEditorUITools.cpp — SOMOLMCP v1.8.0
// 编辑器 UI 自动化工具：模拟菜单点击、按钮操作、地形/PCG 全自动创建等
//
// UE5.7.4: SLevelEditor.h is in LevelEditor/Private/ but accessible since SOMOLMCP.Build.cs
// already has a module dependency on "LevelEditor". No changes needed.
//
// 设计原则：
//   1. 优先使用 UE 高级 API（Subsystem、EditorLibrary、GEditor 命令）而非 Slate 原生输入模拟
//   2. Slate 点击作为后备手段（用于无编程接口的菜单项）
//   3. 所有操作均在游戏线程执行（确保 Slate 安全）
//   4. 每个工具提供清晰的返回信息，供 AI 判断下一步操作

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpTerrainModeGuard.h"
#include "Dom/JsonValue.h"

// ── UE 编辑器核心 ──
#include "Editor.h"
#include "EditorViewportClient.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "EditorModeTools.h"
#include "LevelEditorSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkit.h"
#include "Containers/Ticker.h"

// ── Slate / UI ──
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Widgets/IToolTip.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

// ── Level Editor ──
#include "LevelEditor.h"
#include "LevelEditorActions.h"
// UE 5.7: SLevelEditor.h moved to LevelEditor/Private/ — not accessible from plugins
// #include "SLevelEditor.h"
#include "SLevelViewport.h"

// ── 地形 ──
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeEditorUtils.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"

// ── Editor Modes ──
#include "EditorModes.h"
#include "EdMode.h"
#include "EditorModeManager.h"
// UE 5.7: BuiltinEditorModes.h removed - editor modes are now registered differently
// #include "Tools/Modes/BuiltinEditorModes.h"

// ── Actor / World 操作 ──
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "ScopedTransaction.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"

// ── 通用 ──
#include "Modules/ModuleManager.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "FileHelpers.h"
#include "Misc/FeedbackContext.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Crc.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPEditorUI, Log, All);

namespace UE::SOMOLMCP
{

// ═══════════════════════════════════════════════════════════════════════════
//  内部工具函数
// ═══════════════════════════════════════════════════════════════════════════

/** 在游戏线程安全地执行 Slate 操作。MCP 工具在游戏线程调用，无需 Async。 */
static bool EnsureGameThread(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("EditorUI tools must be called from the game thread.");
		return false;
	}
	return true;
}

/** 获取主关卡编辑器 */
// UE 5.7: SLevelEditor.h moved to LevelEditor/Private/ — use ILevelEditor instead
static TSharedPtr<ILevelEditor> GetLevelEditorInstance(FString& OutError)
{
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	if (!LevelEditorModule)
	{
		OutError = TEXT("LevelEditor module not available.");
		return nullptr;
	}
	TWeakPtr<ILevelEditor> LevelEditorWeak = LevelEditorModule->GetLevelEditorInstance();
	if (TSharedPtr<ILevelEditor> LevelEditor = LevelEditorWeak.Pin())
	{
		return LevelEditor;
	}
	OutError = TEXT("LevelEditor instance not available.");
	return nullptr;
}

/** 通过名称递归查找 Widget */
static TSharedPtr<SWidget> FindWidgetByType(TSharedPtr<SWidget> Root, const FString& TypeName, int32 MaxDepth = 20)
{
	if (!Root.IsValid() || MaxDepth <= 0) return nullptr;
	if (Root->GetTypeAsString() == TypeName) return Root;
	FChildren* Children = Root->GetChildren();
	if (!Children) return nullptr;
	for (int32 i = 0; i < Children->Num(); ++i)
	{
		TSharedPtr<SWidget> Child = Children->GetChildAt(i);
		TSharedPtr<SWidget> Found = FindWidgetByType(Child, TypeName, MaxDepth - 1);
		if (Found.IsValid()) return Found;
	}
	return nullptr;
}

/** 构建 Widget 树摘要，用于返回给 AI 分析 */
static void BuildWidgetTreeSummary(TSharedPtr<SWidget> Widget, TArray<TSharedPtr<FJsonValue>>& OutArray, int32 Depth = 0, int32 MaxDepth = 5)
{
	if (!Widget.IsValid() || Depth > MaxDepth) return;

	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetNumberField(TEXT("depth"), Depth);
	Entry->SetStringField(TEXT("type"), Widget->GetTypeAsString());
	Entry->SetStringField(TEXT("tag"), Widget->GetTag().IsNone() ? TEXT("") : Widget->GetTag().ToString());

	// 获取可见性
	EVisibility Vis = Widget->GetVisibility();
	Entry->SetBoolField(TEXT("visible"), Vis == EVisibility::Visible || Vis == EVisibility::SelfHitTestInvisible || Vis == EVisibility::HitTestInvisible);

	// 获取尺寸
	FVector2D Size = Widget->GetDesiredSize();
	TSharedRef<FJsonObject> SizeObj = MakeShared<FJsonObject>();
	SizeObj->SetNumberField(TEXT("w"), FMath::RoundToInt(Size.X));
	SizeObj->SetNumberField(TEXT("h"), FMath::RoundToInt(Size.Y));
	Entry->SetObjectField(TEXT("desired_size"), SizeObj);

	OutArray.Add(MakeShared<FJsonValueObject>(Entry));

	FChildren* Children = Widget->GetChildren();
	if (Children)
	{
		for (int32 i = 0; i < Children->Num() && i < 32; ++i)
		{
			BuildWidgetTreeSummary(Children->GetChildAt(i), OutArray, Depth + 1, MaxDepth);
		}
	}
}

struct FEditorUIWidgetSelector
{
	FString WidgetId;
	FString Type;
	FString Text;
	FString Tag;
	bool bExact = false;
	bool bCaseSensitive = false;
	bool bButtonLikeOnly = false;
};

struct FEditorUIWidgetCandidate
{
	FString WidgetId;
	FString WidgetPath;
	FString WindowTitle;
	FString WidgetType;
	FString Tag;
	FString Text;
	FString Tooltip;
	FString Visibility;
	FString ReadableLocation;
	int32 WindowIndex = 0;
	int32 Depth = 0;
	bool bVisible = false;
	bool bEnabled = false;
	bool bFocusable = false;
	bool bHasKeyboardFocus = false;
	bool bButtonLike = false;
	bool bHasGeometry = false;
	double X = 0.0;
	double Y = 0.0;
	double W = 0.0;
	double H = 0.0;
	double CenterX = 0.0;
	double CenterY = 0.0;
};

static FString NormalizeEditorUIText(const FString& In)
{
	FString Out = In;
	Out.TrimStartAndEndInline();
	Out.ReplaceInline(TEXT("\r"), TEXT(" "));
	Out.ReplaceInline(TEXT("\n"), TEXT(" "));
	Out.ReplaceInline(TEXT("\t"), TEXT(" "));
	while (Out.Contains(TEXT("  ")))
	{
		Out.ReplaceInline(TEXT("  "), TEXT(" "));
	}
	return Out;
}

static bool StringSelectorMatches(const FString& Candidate, const FString& Selector, bool bExact, bool bCaseSensitive)
{
	const FString Left = NormalizeEditorUIText(Candidate);
	const FString Right = NormalizeEditorUIText(Selector);
	if (Right.IsEmpty())
	{
		return true;
	}
	if (Left.IsEmpty())
	{
		return false;
	}
	const ESearchCase::Type SearchCase = bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
	return bExact ? Left.Equals(Right, SearchCase) : Left.Contains(Right, SearchCase);
}

static bool IsWidgetVisibleForEditorUI(const EVisibility Visibility)
{
	return Visibility.IsVisible();
}

static bool IsEditorUIButtonLike(const FString& WidgetType)
{
	return WidgetType.Contains(TEXT("Button"), ESearchCase::IgnoreCase)
		|| WidgetType.Contains(TEXT("CheckBox"), ESearchCase::IgnoreCase)
		|| WidgetType.Contains(TEXT("Combo"), ESearchCase::IgnoreCase)
		|| WidgetType.Contains(TEXT("MenuEntry"), ESearchCase::IgnoreCase)
		|| WidgetType.Contains(TEXT("Hyperlink"), ESearchCase::IgnoreCase);
}

static void CollectEditorUITextPieces(
	TSharedPtr<SWidget> Widget,
	TArray<FString>& OutPieces,
	int32 Depth,
	int32 MaxDepth,
	int32 MaxPieces)
{
	if (!Widget.IsValid() || Depth > MaxDepth || OutPieces.Num() >= MaxPieces)
	{
		return;
	}

	if (Widget->GetTypeAsString() == TEXT("STextBlock"))
	{
		const TSharedPtr<STextBlock> TextBlock = StaticCastSharedPtr<STextBlock>(Widget);
		if (TextBlock.IsValid())
		{
			const FString Text = NormalizeEditorUIText(TextBlock->GetText().ToString());
			if (!Text.IsEmpty())
			{
				OutPieces.Add(Text);
			}
		}
	}

	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 ChildIndex = 0; ChildIndex < Children->Num() && ChildIndex < 64 && OutPieces.Num() < MaxPieces; ++ChildIndex)
		{
			CollectEditorUITextPieces(Children->GetChildAt(ChildIndex), OutPieces, Depth + 1, MaxDepth, MaxPieces);
		}
	}
}

static FString ExtractEditorUIText(TSharedPtr<SWidget> Widget, int32 MaxDepth = 4, int32 MaxChars = 512)
{
	TArray<FString> Pieces;
	CollectEditorUITextPieces(Widget, Pieces, 0, MaxDepth, 12);
	FString Text = NormalizeEditorUIText(FString::Join(Pieces, TEXT(" | ")));
	if (Text.Len() > MaxChars)
	{
		Text = Text.Left(MaxChars) + TEXT("...");
	}
	return Text;
}

static FString ExtractEditorUITooltipText(TSharedPtr<SWidget> Widget)
{
	if (!Widget.IsValid())
	{
		return FString();
	}

	TSharedPtr<IToolTip> ToolTip = Widget->GetToolTip();
	if (!ToolTip.IsValid() || ToolTip->IsEmpty())
	{
		return FString();
	}
	return ExtractEditorUIText(ToolTip->GetContentWidget(), 4, 512);
}

static TArray<TSharedRef<SWindow>> EnumerateEditorUIWindows()
{
	TArray<TSharedRef<SWindow>> Windows;
	if (!FSlateApplication::IsInitialized())
	{
		return Windows;
	}

	TSet<const SWindow*> Seen;
	for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
	{
		if (Window->IsVisible() && !Seen.Contains(&Window.Get()))
		{
			Seen.Add(&Window.Get());
			Windows.Add(Window);
		}
	}

	TArray<TSharedRef<SWindow>> OrderedVisible;
	FSlateApplication::Get().GetAllVisibleWindowsOrdered(OrderedVisible);
	for (const TSharedRef<SWindow>& Window : OrderedVisible)
	{
		if (Window->IsVisible() && !Seen.Contains(&Window.Get()))
		{
			Seen.Add(&Window.Get());
			Windows.Add(Window);
		}
	}
	return Windows;
}

static TArray<TSharedRef<SWindow>> ResolveEditorUIWindows(const FString& WindowModeRaw, const FString& WindowTitleFilter)
{
	TArray<TSharedRef<SWindow>> Result;
	if (!FSlateApplication::IsInitialized())
	{
		return Result;
	}

	const FString WindowMode = WindowModeRaw.IsEmpty() ? TEXT("focused") : WindowModeRaw.ToLower();
	TSet<const SWindow*> Seen;
	auto AddIfVisibleAndMatches = [&Result, &Seen, &WindowTitleFilter](const TSharedPtr<SWindow>& Window)
	{
		if (!Window.IsValid() || !Window->IsVisible() || Seen.Contains(Window.Get()))
		{
			return;
		}
		const FString Title = NormalizeEditorUIText(Window->GetTitle().ToString());
		if (!WindowTitleFilter.IsEmpty() && !Title.Contains(WindowTitleFilter, ESearchCase::IgnoreCase))
		{
			return;
		}
		Seen.Add(Window.Get());
		Result.Add(Window.ToSharedRef());
	};

	if (WindowMode == TEXT("focused"))
	{
		if (TSharedPtr<SWidget> FocusedWidget = FSlateApplication::Get().GetKeyboardFocusedWidget())
		{
			AddIfVisibleAndMatches(FSlateApplication::Get().FindWidgetWindow(FocusedWidget.ToSharedRef()));
		}
		AddIfVisibleAndMatches(FSlateApplication::Get().GetActiveTopLevelWindow());
	}
	else if (WindowMode == TEXT("main"))
	{
		AddIfVisibleAndMatches(FSlateApplication::Get().GetActiveTopLevelWindow());
		if (Result.IsEmpty())
		{
			for (const TSharedRef<SWindow>& Window : EnumerateEditorUIWindows())
			{
				if (Window->IsRegularWindow())
				{
					AddIfVisibleAndMatches(Window);
					break;
				}
			}
		}
	}
	else if (WindowMode == TEXT("window") || WindowMode == TEXT("all"))
	{
		for (const TSharedRef<SWindow>& Window : EnumerateEditorUIWindows())
		{
			AddIfVisibleAndMatches(Window);
		}
	}
	return Result;
}

static FEditorUIWidgetSelector ReadEditorUIWidgetSelector(const TSharedRef<FJsonObject>& Arguments)
{
	FEditorUIWidgetSelector Selector;

	const TSharedPtr<FJsonObject>* SelectorObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("selector"), SelectorObj) && SelectorObj && SelectorObj->IsValid())
	{
		(*SelectorObj)->TryGetStringField(TEXT("widget_id"), Selector.WidgetId);
		(*SelectorObj)->TryGetStringField(TEXT("type"), Selector.Type);
		(*SelectorObj)->TryGetStringField(TEXT("text"), Selector.Text);
		(*SelectorObj)->TryGetStringField(TEXT("tag"), Selector.Tag);
		(*SelectorObj)->TryGetBoolField(TEXT("exact"), Selector.bExact);
		(*SelectorObj)->TryGetBoolField(TEXT("case_sensitive"), Selector.bCaseSensitive);
		(*SelectorObj)->TryGetBoolField(TEXT("button_like_only"), Selector.bButtonLikeOnly);
	}

	Arguments->TryGetStringField(TEXT("widget_id"), Selector.WidgetId);
	Arguments->TryGetStringField(TEXT("type"), Selector.Type);
	Arguments->TryGetStringField(TEXT("text"), Selector.Text);
	Arguments->TryGetStringField(TEXT("tag"), Selector.Tag);
	Arguments->TryGetBoolField(TEXT("exact"), Selector.bExact);
	Arguments->TryGetBoolField(TEXT("case_sensitive"), Selector.bCaseSensitive);
	Arguments->TryGetBoolField(TEXT("button_like_only"), Selector.bButtonLikeOnly);
	return Selector;
}

static bool EditorUIWidgetMatchesSelector(const FEditorUIWidgetCandidate& Candidate, const FEditorUIWidgetSelector& Selector)
{
	if (!Selector.WidgetId.IsEmpty() && !Candidate.WidgetId.Equals(Selector.WidgetId, ESearchCase::IgnoreCase))
	{
		return false;
	}
	if (Selector.bButtonLikeOnly && !Candidate.bButtonLike)
	{
		return false;
	}
	if (!StringSelectorMatches(Candidate.WidgetType, Selector.Type, Selector.bExact, Selector.bCaseSensitive))
	{
		return false;
	}
	if (!StringSelectorMatches(Candidate.Tag, Selector.Tag, Selector.bExact, Selector.bCaseSensitive))
	{
		return false;
	}
	if (!Selector.Text.IsEmpty())
	{
		const FString CombinedText = Candidate.Text + TEXT(" ") + Candidate.Tooltip;
		if (!StringSelectorMatches(CombinedText, Selector.Text, Selector.bExact, Selector.bCaseSensitive))
		{
			return false;
		}
	}
	return true;
}

static void CollectEditorUIWidgetCandidates(
	TSharedPtr<SWidget> Widget,
	const FEditorUIWidgetSelector& Selector,
	TArray<FEditorUIWidgetCandidate>& OutCandidates,
	int32 WindowIndex,
	const FString& WindowTitle,
	const FString& WidgetPath,
	int32 Depth,
	int32 MaxDepth,
	int32 MaxResults,
	bool bParentVisible,
	bool bParentEnabled)
{
	if (!Widget.IsValid() || Depth > MaxDepth || OutCandidates.Num() >= MaxResults)
	{
		return;
	}

	const EVisibility Visibility = Widget->GetVisibility();
	const bool bVisible = bParentVisible && IsWidgetVisibleForEditorUI(Visibility);
	const bool bEnabled = bParentEnabled && Widget->IsEnabled();

	FEditorUIWidgetCandidate Candidate;
	Candidate.WidgetPath = WidgetPath;
	Candidate.WindowTitle = WindowTitle;
	Candidate.WidgetType = Widget->GetTypeAsString();
	Candidate.Tag = Widget->GetTag().IsNone() ? TEXT("") : Widget->GetTag().ToString();
	Candidate.Text = ExtractEditorUIText(Widget, 3, 512);
	Candidate.Tooltip = ExtractEditorUITooltipText(Widget);
	Candidate.Visibility = Visibility.ToString();
	Candidate.ReadableLocation = Widget->GetReadableLocation();
	Candidate.WindowIndex = WindowIndex;
	Candidate.Depth = Depth;
	Candidate.bVisible = bVisible;
	Candidate.bEnabled = bEnabled;
	Candidate.bFocusable = Widget->SupportsKeyboardFocus();
	Candidate.bHasKeyboardFocus = Widget->HasKeyboardFocus();
	Candidate.bButtonLike = IsEditorUIButtonLike(Candidate.WidgetType);

	const FGeometry Geometry = Widget->GetCachedGeometry();
	const FVector2D LocalSize = Geometry.GetLocalSize();
	const FVector2D TopLeft = Geometry.LocalToAbsolute(FVector2D::ZeroVector);
	const FVector2D BottomRight = Geometry.LocalToAbsolute(LocalSize);
	const FVector2D Size = BottomRight - TopLeft;
	if (Size.X > 1.0 && Size.Y > 1.0)
	{
		Candidate.bHasGeometry = true;
		Candidate.X = TopLeft.X;
		Candidate.Y = TopLeft.Y;
		Candidate.W = Size.X;
		Candidate.H = Size.Y;
		Candidate.CenterX = TopLeft.X + Size.X * 0.5;
		Candidate.CenterY = TopLeft.Y + Size.Y * 0.5;
	}

	const FString StableId = FString::Printf(
		TEXT("%d|%s|%s|%s|%s"),
		WindowIndex,
		*WidgetPath,
		*Candidate.WidgetType,
		*Candidate.Tag,
		*Candidate.Text);
	Candidate.WidgetId = FString::Printf(TEXT("w_%08x"), FCrc::StrCrc32(*StableId));

	if (EditorUIWidgetMatchesSelector(Candidate, Selector))
	{
		OutCandidates.Add(Candidate);
	}

	if (FChildren* Children = Widget->GetChildren())
	{
		for (int32 ChildIndex = 0; ChildIndex < Children->Num() && ChildIndex < 128 && OutCandidates.Num() < MaxResults; ++ChildIndex)
		{
			const FString ChildPath = WidgetPath.IsEmpty()
				? FString::FromInt(ChildIndex)
				: FString::Printf(TEXT("%s.%d"), *WidgetPath, ChildIndex);
			CollectEditorUIWidgetCandidates(
				Children->GetChildAt(ChildIndex),
				Selector,
				OutCandidates,
				WindowIndex,
				WindowTitle,
				ChildPath,
				Depth + 1,
				MaxDepth,
				MaxResults,
				bVisible,
				bEnabled);
		}
	}
}

static TSharedRef<FJsonObject> MakeEditorUIBoundsJson(const FEditorUIWidgetCandidate& Candidate)
{
	TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
	Bounds->SetBoolField(TEXT("has_bounds"), Candidate.bHasGeometry);
	Bounds->SetNumberField(TEXT("x"), FMath::RoundToInt(Candidate.X));
	Bounds->SetNumberField(TEXT("y"), FMath::RoundToInt(Candidate.Y));
	Bounds->SetNumberField(TEXT("w"), FMath::RoundToInt(Candidate.W));
	Bounds->SetNumberField(TEXT("h"), FMath::RoundToInt(Candidate.H));
	Bounds->SetNumberField(TEXT("center_x"), FMath::RoundToInt(Candidate.CenterX));
	Bounds->SetNumberField(TEXT("center_y"), FMath::RoundToInt(Candidate.CenterY));
	return Bounds;
}

static TArray<TSharedPtr<FJsonValue>> MakeEditorUIStringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

static TSharedRef<FJsonObject> MakeEditorUIActionHintsJson(const FEditorUIWidgetCandidate& Candidate)
{
	TSharedRef<FJsonObject> Hints = MakeShared<FJsonObject>();
	const bool bCanClick = Candidate.bButtonLike && Candidate.bVisible && Candidate.bEnabled && Candidate.bHasGeometry;
	Hints->SetBoolField(TEXT("click_candidate"), bCanClick);
	Hints->SetStringField(TEXT("click_tool"), TEXT("editor_ui_click_widget"));
	Hints->SetBoolField(TEXT("click_requires_unique_match"), true);
	Hints->SetBoolField(TEXT("click_requires_allow_click"), true);
	Hints->SetStringField(TEXT("safety"), TEXT("dry-run by default; disabled/hidden/ambiguous/dangerous widgets fail closed"));
	return Hints;
}

static TSharedRef<FJsonObject> MakeEditorUIWidgetJson(const FEditorUIWidgetCandidate& Candidate)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("widget_id"), Candidate.WidgetId);
	Json->SetStringField(TEXT("widget_path"), Candidate.WidgetPath);
	Json->SetNumberField(TEXT("window_index"), Candidate.WindowIndex);
	Json->SetStringField(TEXT("window_title"), Candidate.WindowTitle);
	Json->SetNumberField(TEXT("depth"), Candidate.Depth);
	Json->SetStringField(TEXT("type"), Candidate.WidgetType);
	Json->SetStringField(TEXT("tag"), Candidate.Tag);
	Json->SetStringField(TEXT("text"), Candidate.Text);
	Json->SetStringField(TEXT("tooltip"), Candidate.Tooltip);
	Json->SetStringField(TEXT("visibility"), Candidate.Visibility);
	Json->SetBoolField(TEXT("visible"), Candidate.bVisible);
	Json->SetBoolField(TEXT("enabled"), Candidate.bEnabled);
	Json->SetBoolField(TEXT("focusable"), Candidate.bFocusable);
	Json->SetBoolField(TEXT("has_keyboard_focus"), Candidate.bHasKeyboardFocus);
	Json->SetBoolField(TEXT("button_like"), Candidate.bButtonLike);
	Json->SetStringField(TEXT("readable_location"), Candidate.ReadableLocation);
	Json->SetObjectField(TEXT("bounds"), MakeEditorUIBoundsJson(Candidate));
	Json->SetObjectField(TEXT("action_hints"), MakeEditorUIActionHintsJson(Candidate));
	return Json;
}

static TArray<TSharedPtr<FJsonValue>> MakeEditorUIWidgetArray(const TArray<FEditorUIWidgetCandidate>& Candidates)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FEditorUIWidgetCandidate& Candidate : Candidates)
	{
		Result.Add(MakeShared<FJsonValueObject>(MakeEditorUIWidgetJson(Candidate)));
	}
	return Result;
}

static void CollectEditorUIWidgetsForArgs(
	const TSharedRef<FJsonObject>& Arguments,
	bool bButtonLikeOnly,
	TArray<FEditorUIWidgetCandidate>& OutCandidates,
	TArray<TSharedRef<FJsonObject>>& OutWindowJson)
{
	FString WindowMode = TEXT("focused");
	FString WindowTitle;
	int32 MaxDepth = 8;
	int32 MaxResults = 128;
	Arguments->TryGetStringField(TEXT("window"), WindowMode);
	Arguments->TryGetStringField(TEXT("window_title"), WindowTitle);
	Arguments->TryGetNumberField(TEXT("max_depth"), MaxDepth);
	Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
	MaxDepth = FMath::Clamp(MaxDepth, 1, 16);
	MaxResults = FMath::Clamp(MaxResults, 1, 512);

	FEditorUIWidgetSelector Selector = ReadEditorUIWidgetSelector(Arguments);
	Selector.bButtonLikeOnly = Selector.bButtonLikeOnly || bButtonLikeOnly;

	const TArray<TSharedRef<SWindow>> Windows = ResolveEditorUIWindows(WindowMode, WindowTitle);
	for (int32 WindowIndex = 0; WindowIndex < Windows.Num() && OutCandidates.Num() < MaxResults; ++WindowIndex)
	{
		const TSharedRef<SWindow>& Window = Windows[WindowIndex];
		const FString Title = NormalizeEditorUIText(Window->GetTitle().ToString());

		TSharedRef<FJsonObject> WindowJson = MakeShared<FJsonObject>();
		WindowJson->SetNumberField(TEXT("window_index"), WindowIndex);
		WindowJson->SetStringField(TEXT("title"), Title);
		WindowJson->SetStringField(TEXT("type"), Window->GetTypeAsString());
		WindowJson->SetBoolField(TEXT("visible"), Window->IsVisible());
		WindowJson->SetBoolField(TEXT("is_regular_window"), Window->IsRegularWindow());
		WindowJson->SetBoolField(TEXT("is_modal"), Window->IsModalWindow());
		const FVector2D Position = Window->GetPositionInScreen();
		const FVector2D Size = Window->GetSizeInScreen();
		TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetNumberField(TEXT("x"), FMath::RoundToInt(Position.X));
		Bounds->SetNumberField(TEXT("y"), FMath::RoundToInt(Position.Y));
		Bounds->SetNumberField(TEXT("w"), FMath::RoundToInt(Size.X));
		Bounds->SetNumberField(TEXT("h"), FMath::RoundToInt(Size.Y));
		WindowJson->SetObjectField(TEXT("bounds"), Bounds);
		OutWindowJson.Add(WindowJson);

		CollectEditorUIWidgetCandidates(
			Window,
			Selector,
			OutCandidates,
			WindowIndex,
			Title,
			TEXT("root"),
			0,
			MaxDepth,
			MaxResults,
			true,
			true);
	}
}

static FString EditorUIActionTypeToString(EUserInterfaceActionType ActionType)
{
	switch (ActionType)
	{
	case EUserInterfaceActionType::Button:
		return TEXT("Button");
	case EUserInterfaceActionType::ToggleButton:
		return TEXT("ToggleButton");
	case EUserInterfaceActionType::RadioButton:
		return TEXT("RadioButton");
	case EUserInterfaceActionType::Check:
		return TEXT("Check");
	case EUserInterfaceActionType::CollapsedButton:
		return TEXT("CollapsedButton");
	default:
		return TEXT("None");
	}
}

static FString EditorUIChordToString(const FInputChord& Chord)
{
	return Chord.IsValidChord() ? Chord.GetInputText(true).ToString() : FString();
}

static bool EditorUICommandMatchesFilter(
	const TSharedPtr<FUICommandInfo>& Command,
	const FString& ContextName,
	const FString& TextFilter,
	const FString& ContextFilter,
	bool bToolbarCandidatesOnly)
{
	if (!Command.IsValid())
	{
		return false;
	}
	if (!ContextFilter.IsEmpty() && !ContextName.Contains(ContextFilter, ESearchCase::IgnoreCase))
	{
		return false;
	}
	if (bToolbarCandidatesOnly && Command->GetUserInterfaceType() == EUserInterfaceActionType::None)
	{
		return false;
	}

	const FString CommandName = Command->GetCommandName().ToString();
	const FString FullName = ContextName + TEXT(".") + CommandName;
	const FString Haystack = FullName + TEXT(" ") + Command->GetLabel().ToString() + TEXT(" ") + Command->GetDescription().ToString();
	return TextFilter.IsEmpty() || Haystack.Contains(TextFilter, ESearchCase::IgnoreCase);
}

static TSharedRef<FJsonObject> MakeEditorUICommandJson(const TSharedPtr<FUICommandInfo>& Command, const FString& ContextName)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	const FString CommandName = Command->GetCommandName().ToString();
	const FString FullName = ContextName + TEXT(".") + CommandName;
	Json->SetStringField(TEXT("full_name"), FullName);
	Json->SetStringField(TEXT("command_name"), CommandName);
	Json->SetStringField(TEXT("context"), ContextName);
	Json->SetStringField(TEXT("binding_context"), Command->GetBindingContext().ToString());
	Json->SetStringField(TEXT("label"), Command->GetLabel().ToString());
	Json->SetStringField(TEXT("description"), Command->GetDescription().ToString());
	Json->SetStringField(TEXT("ui_action_type"), EditorUIActionTypeToString(Command->GetUserInterfaceType()));
	Json->SetStringField(TEXT("input_text"), Command->GetInputText().ToString());
	Json->SetStringField(TEXT("active_chord_primary"), EditorUIChordToString(*Command->GetActiveChord(EMultipleKeyBindingIndex::Primary)));
	Json->SetStringField(TEXT("active_chord_secondary"), EditorUIChordToString(*Command->GetActiveChord(EMultipleKeyBindingIndex::Secondary)));
	Json->SetStringField(TEXT("default_chord_primary"), EditorUIChordToString(Command->GetDefaultChord(EMultipleKeyBindingIndex::Primary)));
	Json->SetStringField(TEXT("default_chord_secondary"), EditorUIChordToString(Command->GetDefaultChord(EMultipleKeyBindingIndex::Secondary)));

	TSharedRef<FJsonObject> Hints = MakeShared<FJsonObject>();
	Hints->SetStringField(TEXT("execute_tool"), TEXT("editor_ui_execute_command"));
	Hints->SetStringField(TEXT("execute_command_name"), FullName);
	Hints->SetStringField(TEXT("fallback_command_name"), CommandName);
	Hints->SetStringField(TEXT("safety"), TEXT("read-only listing; executing commands is a separate mutating action"));
	Json->SetObjectField(TEXT("action_hints"), Hints);
	return Json;
}

static void CollectEditorUICommandJson(
	const FString& TextFilter,
	const FString& ContextFilter,
	int32 MaxResults,
	bool bToolbarCandidatesOnly,
	TArray<TSharedPtr<FJsonValue>>& OutCommands,
	int32& OutContextCount)
{
	TArray<TSharedPtr<FBindingContext>> Contexts;
	FInputBindingManager::Get().GetKnownInputContexts(Contexts);
	OutContextCount = Contexts.Num();

	for (const TSharedPtr<FBindingContext>& BindingContext : Contexts)
	{
		if (!BindingContext.IsValid() || OutCommands.Num() >= MaxResults)
		{
			continue;
		}

		const FString ContextName = BindingContext->GetContextName().ToString();
		TArray<TSharedPtr<FUICommandInfo>> CommandInfos;
		FInputBindingManager::Get().GetCommandInfosFromContext(BindingContext->GetContextName(), CommandInfos);
		for (const TSharedPtr<FUICommandInfo>& Command : CommandInfos)
		{
			if (OutCommands.Num() >= MaxResults)
			{
				break;
			}
			if (EditorUICommandMatchesFilter(Command, ContextName, TextFilter, ContextFilter, bToolbarCandidatesOnly))
			{
				OutCommands.Add(MakeShared<FJsonValueObject>(MakeEditorUICommandJson(Command, ContextName)));
			}
		}
	}
}

static bool EditorUIWidgetTextLooksDangerous(const FEditorUIWidgetCandidate& Candidate, FString& OutMatchedToken)
{
	static const TArray<FString> DangerousTokens = {
		TEXT("Save"),
		TEXT("Save All"),
		TEXT("Delete"),
		TEXT("Overwrite"),
		TEXT("Replace"),
		TEXT("Force Delete"),
		TEXT("Continue"),
		TEXT("Run in Editor"),
		TEXT("Yes"),
		TEXT("Build"),
		TEXT("Compile"),
		TEXT("Play"),
		TEXT("PIE"),
		TEXT("Submit"),
		TEXT("Apply")
	};

	const FString Haystack = Candidate.Text + TEXT(" ") + Candidate.Tooltip + TEXT(" ") + Candidate.Tag;
	for (const FString& Token : DangerousTokens)
	{
		if (Haystack.Contains(Token, ESearchCase::IgnoreCase))
		{
			OutMatchedToken = Token;
			return true;
		}
	}
	return false;
}

static bool SimulateEditorUILeftClick(double ScreenX, double ScreenY, bool& bOutDownHandled, bool& bOutUpHandled)
{
	const FVector2D ClickPos(static_cast<float>(ScreenX), static_cast<float>(ScreenY));
	const FKey MouseKey(EKeys::LeftMouseButton);
	FPointerEvent MouseDownEvent(
		0,
		0,
		ClickPos,
		ClickPos,
		TSet<FKey>(),
		MouseKey,
		0.0f,
		FModifierKeysState());

	FPointerEvent MouseUpEvent(
		0,
		0,
		ClickPos,
		ClickPos,
		TSet<FKey>(),
		MouseKey,
		0.0f,
		FModifierKeysState());

	bOutDownHandled = FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, MouseDownEvent);
	FPlatformProcess::Sleep(0.05f);
	bOutUpHandled = FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUpEvent);
	return bOutDownHandled || bOutUpHandled;
}

// ═══════════════════════════════════════════════════════════════════════════
//  RegisterEditorUITools
// ═══════════════════════════════════════════════════════════════════════════

void RegisterEditorUITools(FSololmcpToolRegistry& Registry)
{
	// ─────────────────────────────────────────────────────────────────────────
	// 1. editor_ui_execute_command  —  通过 FUICommandList 执行已注册 UI 命令
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_execute_command"),
		TEXT("Execute a named UE editor UI command (equivalent to clicking menu items). "
			 "Use 'editor_ui_list_commands' to discover available command names. "
			 "Examples: 'LevelEditor.NewLevel', 'LevelEditor.Build.BuildAll', "
			 "'MainFrame.SaveAll', 'LevelEditor.OpenLandscapeEditor'."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("command"),  FSololmcpSchemaBuilder::String(TEXT("Command name, e.g. 'LevelEditor.Build.BuildAll'."))},
				{TEXT("context"), FSololmcpSchemaBuilder::String(TEXT("Optional command context: 'LevelEditor' (default), 'MainFrame', 'Blueprint', 'MaterialEditor'."))}
			},
			{TEXT("command")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			FString CommandName;
			if (!Arguments->TryGetStringField(TEXT("command"), CommandName) || CommandName.IsEmpty())
			{
				OutError = TEXT("Missing required argument: command");
				return false;
			}

			// 尝试通过 IConsoleManager 执行（支持 console 命令形式）
			// UE 5.7: FindConsoleObject no longer a template, returns IConsoleObject*
			// UE 5.7: IsCommand() removed, use AsCommand() instead
			IConsoleObject* ConsoleObj = IConsoleManager::Get().FindConsoleObject(*CommandName);
			if (ConsoleObj && ConsoleObj->AsCommand())
			{
				IConsoleCommand* ConsoleCmd = ConsoleObj->AsCommand();
				TArray<FString> Args;
				ConsoleCmd->Execute(Args, GEditor->GetEditorWorldContext().World(), *GLog);
				OutStructured->SetStringField(TEXT("method"), TEXT("console_command"));
				OutSummary = FString::Printf(TEXT("Executed console command: %s"), *CommandName);
				return true;
			}

			// 尝试通过全局命令执行器（Ctrl+Shift+P 风格）
			if (GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
			{
				// 对于地形编辑器模式特殊处理
				if (CommandName == TEXT("LevelEditor.OpenLandscapeEditor") ||
					CommandName == TEXT("FBuiltinEditorModes::EM_Landscape"))
				{
					GLevelEditorModeTools().ActivateMode(FBuiltinEditorModes::EM_Landscape);
					OutStructured->SetStringField(TEXT("method"), TEXT("mode_activation"));
					OutStructured->SetStringField(TEXT("mode"), TEXT("Landscape"));
					OutSummary = TEXT("Activated Landscape Editor mode.");
					return true;
				}
				if (CommandName == TEXT("LevelEditor.OpenPlacementMode") ||
					CommandName == TEXT("FBuiltinEditorModes::EM_Placement"))
				{
					GLevelEditorModeTools().ActivateMode(FBuiltinEditorModes::EM_Placement);
					OutStructured->SetStringField(TEXT("method"), TEXT("mode_activation"));
					OutStructured->SetStringField(TEXT("mode"), TEXT("Placement"));
					OutSummary = TEXT("Activated Placement mode.");
					return true;
				}
				if (CommandName == TEXT("LevelEditor.OpenMeshPaintMode") ||
					CommandName == TEXT("FBuiltinEditorModes::EM_MeshPaint"))
				{
					GLevelEditorModeTools().ActivateMode(FBuiltinEditorModes::EM_MeshPaint);
					OutStructured->SetStringField(TEXT("method"), TEXT("mode_activation"));
					OutStructured->SetStringField(TEXT("mode"), TEXT("MeshPaint"));
					OutSummary = TEXT("Activated Mesh Paint mode.");
					return true;
				}
			}

			// 通过 Slate Application 执行已注册命令
			FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
			if (LevelEditorModule)
			{
				TWeakPtr<ILevelEditor> LevelEditorWeak = LevelEditorModule->GetLevelEditorInstance();
				if (TSharedPtr<ILevelEditor> LevelEditor = LevelEditorWeak.Pin())
				{
					// 尝试从 Level Editor 的命令列表中执行
					TSharedPtr<FUICommandList> CmdList = LevelEditor->GetLevelEditorActions();
					if (CmdList.IsValid())
					{
						// 通过 FInputChord 匹配或直接通过名称
						// 使用 GEditor Execute 内置命令
						if (!GEditor->Exec(GEditor->GetEditorWorldContext().World(), *CommandName))
						{
							OutStructured->SetStringField(TEXT("method"), TEXT("gexec"));
							OutStructured->SetStringField(TEXT("command"), CommandName);
							OutError = FString::Printf(TEXT("Unknown or unhandled editor command: %s"), *CommandName);
							return false;
						}
						OutStructured->SetStringField(TEXT("method"), TEXT("gexec"));
						OutSummary = FString::Printf(TEXT("Sent GEditor::Exec command: %s"), *CommandName);
						return true;
					}
				}
			}

			// 最终后备: GEditor->Exec
			if (!GEditor->Exec(GEditor->GetEditorWorldContext().World(), *CommandName))
			{
				OutStructured->SetStringField(TEXT("method"), TEXT("gexec_fallback"));
				OutStructured->SetStringField(TEXT("command"), CommandName);
				OutError = FString::Printf(TEXT("Unknown or unhandled editor command: %s"), *CommandName);
				return false;
			}
			OutStructured->SetStringField(TEXT("method"), TEXT("gexec_fallback"));
			OutSummary = FString::Printf(TEXT("Executed via GEditor::Exec: %s"), *CommandName);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 2. editor_ui_activate_mode  —  激活编辑器模式 (Landscape/Placement/Foliage等)
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_activate_mode"),
		TEXT("Activate a named editor mode. "
			 "Supported modes: 'Default' (SelectActor), 'Landscape', 'Foliage', 'Placement', 'MeshPaint', 'MorphTarget', 'Geometry'. "
			 "This is equivalent to clicking the mode buttons in the editor toolbar."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mode"), FSololmcpSchemaBuilder::String(
					TEXT("Mode name: Default | Landscape | Foliage | Placement | MeshPaint | Geometry"),
					{TEXT("Default"), TEXT("Landscape"), TEXT("Foliage"), TEXT("Placement"), TEXT("MeshPaint"), TEXT("Geometry")})}
			},
			{TEXT("mode")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString ModeName;
			Arguments->TryGetStringField(TEXT("mode"), ModeName);
			ModeName = ModeName.TrimStartAndEnd();

			FEditorModeID ModeId;
			if (ModeName == TEXT("Default") || ModeName == TEXT("SelectActor"))
				ModeId = FBuiltinEditorModes::EM_Default;
			else if (ModeName == TEXT("Landscape"))
				ModeId = FBuiltinEditorModes::EM_Landscape;
			else if (ModeName == TEXT("Foliage"))
				ModeId = FBuiltinEditorModes::EM_Foliage;
			else if (ModeName == TEXT("Placement"))
				ModeId = FBuiltinEditorModes::EM_Placement;
			else if (ModeName == TEXT("MeshPaint"))
				ModeId = FBuiltinEditorModes::EM_MeshPaint;
			else if (ModeName == TEXT("Geometry"))
				ModeId = FBuiltinEditorModes::EM_Level; // UE 5.7: EM_Geometry removed
			else
			{
				// 尝试作为原始 FName
				ModeId = FEditorModeID(*ModeName);
			}

			GLevelEditorModeTools().ActivateMode(ModeId, true);

			const bool bActive = GLevelEditorModeTools().IsModeActive(ModeId);
			OutStructured->SetStringField(TEXT("mode_id"), ModeId.ToString());
			OutStructured->SetBoolField(TEXT("is_active"), bActive);
			OutSummary = FString::Printf(TEXT("Activated editor mode '%s' (active=%s)."), *ModeName, bActive ? TEXT("true") : TEXT("false"));
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 3. editor_ui_deactivate_mode  —  退出当前编辑器模式，回到默认选择模式
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_deactivate_mode"),
		TEXT("Deactivate the current editor mode and return to the default selection mode. "
			 "Should be called after finishing landscape/foliage/mesh paint operations."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			// 获取当前活跃模式（用于返回信息）
			// UE 5.7: GetActiveModeIDs removed, use IsModeActive to check known modes
			FString PreviousModeStr;
			TArray<FEditorModeID> KnownModes = {
				FBuiltinEditorModes::EM_Landscape,
				FBuiltinEditorModes::EM_Foliage,
				FBuiltinEditorModes::EM_MeshPaint,
				FBuiltinEditorModes::EM_Placement,
				FBuiltinEditorModes::EM_Level
			};
			for (const FEditorModeID& ID : KnownModes)
			{
				if (GLevelEditorModeTools().IsModeActive(ID))
				{
					PreviousModeStr = ID.ToString();
					break;
				}
			}

			GLevelEditorModeTools().ActivateMode(FBuiltinEditorModes::EM_Default, true);

			OutStructured->SetStringField(TEXT("previous_mode"), PreviousModeStr);
			OutStructured->SetStringField(TEXT("current_mode"), FBuiltinEditorModes::EM_Default.ToString());
			OutSummary = FString::Printf(TEXT("Deactivated mode '%s', returned to Default mode."), *PreviousModeStr);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 4. editor_ui_get_active_mode  —  获取当前活跃的编辑器模式
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_get_active_mode"),
		TEXT("Get the currently active editor mode(s). Returns mode ID and display name."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			// 获取当前活跃模式
			// UE 5.7: GetActiveModeIDs removed, check known modes with IsModeActive
			TArray<TSharedPtr<FJsonValue>> Modes;
			TArray<FEditorModeID> KnownModes = {
				FBuiltinEditorModes::EM_Default,
				FBuiltinEditorModes::EM_Landscape,
				FBuiltinEditorModes::EM_Foliage,
				FBuiltinEditorModes::EM_MeshPaint,
				FBuiltinEditorModes::EM_Placement,
				FBuiltinEditorModes::EM_Level
			};
			for (const FEditorModeID& ModeID : KnownModes)
			{
				if (GLevelEditorModeTools().IsModeActive(ModeID))
				{
					TSharedRef<FJsonObject> ModeObj = MakeShared<FJsonObject>();
					ModeObj->SetStringField(TEXT("id"), ModeID.ToString());
					if (FEdMode* Mode = GLevelEditorModeTools().GetActiveMode(ModeID))
					{
						// UE 5.7: GetName() removed, use GetID()
						ModeObj->SetStringField(TEXT("name"), Mode->GetID().ToString());
					}
					Modes.Add(MakeShared<FJsonValueObject>(ModeObj));
				}
			}

			OutStructured->SetArrayField(TEXT("active_modes"), Modes);
			OutStructured->SetNumberField(TEXT("count"), Modes.Num());
			OutSummary = FString::Printf(TEXT("Found %d active editor mode(s)."), Modes.Num());
			return true;
		},
		nullptr,  // IsAvailable
		2         // CacheTtlSeconds
	});

	// Semantic, read-mostly Slate/UI discovery tools.
	Registry.Register({
		TEXT("editor_ui_list_commands"),
		TEXT("List registered UE editor UI commands from FInputBindingManager. "
			 "This is read-only discovery for command labels, descriptions, chords, contexts, and action hints."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("text"), FSololmcpSchemaBuilder::String(TEXT("Optional substring filter across full_name, label, and description."))},
				{TEXT("context"), FSololmcpSchemaBuilder::String(TEXT("Optional binding context substring filter, e.g. LevelEditor."))},
				{TEXT("toolbar_candidates_only"), FSololmcpSchemaBuilder::Boolean(TEXT("Only include commands with a UI action type. Default: false."))},
				{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum commands to return. Default: 256."))}
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString TextFilter;
			FString ContextFilter;
			bool bToolbarCandidatesOnly = false;
			int32 MaxResults = 256;
			Arguments->TryGetStringField(TEXT("text"), TextFilter);
			Arguments->TryGetStringField(TEXT("context"), ContextFilter);
			Arguments->TryGetBoolField(TEXT("toolbar_candidates_only"), bToolbarCandidatesOnly);
			Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
			MaxResults = FMath::Clamp(MaxResults, 1, 1024);

			TArray<TSharedPtr<FJsonValue>> Commands;
			int32 ContextCount = 0;
			CollectEditorUICommandJson(TextFilter, ContextFilter, MaxResults, bToolbarCandidatesOnly, Commands, ContextCount);

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("tool"), TEXT("editor_ui_list_commands"));
			OutStructured->SetStringField(TEXT("contract_version"), TEXT("editor_ui.semantic.v1"));
			OutStructured->SetStringField(TEXT("status"), TEXT("success"));
			OutStructured->SetNumberField(TEXT("known_context_count"), ContextCount);
			OutStructured->SetNumberField(TEXT("command_count"), Commands.Num());
			OutStructured->SetArrayField(TEXT("commands"), Commands);
			OutSummary = FString::Printf(TEXT("Listed %d editor UI command(s)."), Commands.Num());
			return true;
		},
		nullptr,
		2
	});

	Registry.Register({
		TEXT("editor_ui_list_toolbar_buttons"),
		TEXT("List visible Slate toolbar/button-like candidates from a focused/main/all editor window. "
			 "Returns bounds, text, tooltip, enabled/visible/focusable state, and safe action hints. Read-only."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("window"), FSololmcpSchemaBuilder::String(
					TEXT("Window scope: focused (default), main, window, all."),
					{TEXT("focused"), TEXT("main"), TEXT("window"), TEXT("all")})},
				{TEXT("window_title"), FSololmcpSchemaBuilder::String(TEXT("Optional title substring when window=window/all."))},
				{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("Optional widget type substring, e.g. SButton."))},
				{TEXT("text"), FSololmcpSchemaBuilder::String(TEXT("Optional text/tooltip substring."))},
				{TEXT("tag"), FSololmcpSchemaBuilder::String(TEXT("Optional Slate widget tag substring."))},
				{TEXT("exact"), FSololmcpSchemaBuilder::Boolean(TEXT("Use exact matching for type/text/tag. Default: false."))},
				{TEXT("case_sensitive"), FSololmcpSchemaBuilder::Boolean(TEXT("Use case-sensitive matching. Default: false."))},
				{TEXT("max_depth"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum Slate tree depth. Default: 8."))},
				{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum widgets to return. Default: 128."))},
				{TEXT("include_command_hints"), FSololmcpSchemaBuilder::Boolean(TEXT("Also include matching command catalog hints. Default: true."))}
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!FSlateApplication::IsInitialized())
			{
				OutError = TEXT("Slate application is not initialized.");
				return false;
			}

			bool bIncludeCommandHints = true;
			Arguments->TryGetBoolField(TEXT("include_command_hints"), bIncludeCommandHints);

			TArray<FEditorUIWidgetCandidate> Candidates;
			TArray<TSharedRef<FJsonObject>> WindowObjects;
			CollectEditorUIWidgetsForArgs(Arguments, true, Candidates, WindowObjects);

			TArray<TSharedPtr<FJsonValue>> Windows;
			for (const TSharedRef<FJsonObject>& WindowObject : WindowObjects)
			{
				Windows.Add(MakeShared<FJsonValueObject>(WindowObject));
			}

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("tool"), TEXT("editor_ui_list_toolbar_buttons"));
			OutStructured->SetStringField(TEXT("contract_version"), TEXT("editor_ui.semantic.v1"));
			OutStructured->SetStringField(TEXT("status"), TEXT("success"));
			OutStructured->SetArrayField(TEXT("windows"), Windows);
			OutStructured->SetNumberField(TEXT("window_count"), Windows.Num());
			OutStructured->SetArrayField(TEXT("buttons"), MakeEditorUIWidgetArray(Candidates));
			OutStructured->SetNumberField(TEXT("button_count"), Candidates.Num());

			if (bIncludeCommandHints)
			{
				FString TextFilter;
				FString ContextFilter;
				int32 ContextCount = 0;
				int32 MaxResults = 128;
				Arguments->TryGetStringField(TEXT("text"), TextFilter);
				Arguments->TryGetStringField(TEXT("context"), ContextFilter);
				Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
				MaxResults = FMath::Clamp(MaxResults, 1, 512);

				TArray<TSharedPtr<FJsonValue>> Commands;
				CollectEditorUICommandJson(TextFilter, ContextFilter, MaxResults, true, Commands, ContextCount);
				OutStructured->SetArrayField(TEXT("command_hints"), Commands);
				OutStructured->SetNumberField(TEXT("command_hint_count"), Commands.Num());
			}

			OutSummary = FString::Printf(TEXT("Listed %d visible Slate button candidate(s)."), Candidates.Num());
			return true;
		},
		nullptr,
		1
	});

	Registry.Register({
		TEXT("editor_ui_query_widget"),
		TEXT("Query Slate widgets by window scope plus type/text/tag/widget_id selector. "
			 "Returns bounds, text, tooltip, enabled/visible/focusable state, and action hints. Read-only."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("window"), FSololmcpSchemaBuilder::String(
					TEXT("Window scope: focused (default), main, window, all."),
					{TEXT("focused"), TEXT("main"), TEXT("window"), TEXT("all")})},
				{TEXT("window_title"), FSololmcpSchemaBuilder::String(TEXT("Optional title substring when window=window/all."))},
				{TEXT("widget_id"), FSololmcpSchemaBuilder::String(TEXT("Widget id from a previous query/list result."))},
				{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("Optional widget type substring, e.g. SButton or STextBlock."))},
				{TEXT("text"), FSololmcpSchemaBuilder::String(TEXT("Optional text/tooltip substring."))},
				{TEXT("tag"), FSololmcpSchemaBuilder::String(TEXT("Optional Slate widget tag substring."))},
				{TEXT("button_like_only"), FSololmcpSchemaBuilder::Boolean(TEXT("Only return button-like widgets. Default: false."))},
				{TEXT("exact"), FSololmcpSchemaBuilder::Boolean(TEXT("Use exact matching for type/text/tag. Default: false."))},
				{TEXT("case_sensitive"), FSololmcpSchemaBuilder::Boolean(TEXT("Use case-sensitive matching. Default: false."))},
				{TEXT("max_depth"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum Slate tree depth. Default: 8."))},
				{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum widgets to return. Default: 128."))}
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!FSlateApplication::IsInitialized())
			{
				OutError = TEXT("Slate application is not initialized.");
				return false;
			}

			TArray<FEditorUIWidgetCandidate> Candidates;
			TArray<TSharedRef<FJsonObject>> WindowObjects;
			CollectEditorUIWidgetsForArgs(Arguments, false, Candidates, WindowObjects);

			TArray<TSharedPtr<FJsonValue>> Windows;
			for (const TSharedRef<FJsonObject>& WindowObject : WindowObjects)
			{
				Windows.Add(MakeShared<FJsonValueObject>(WindowObject));
			}

			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("tool"), TEXT("editor_ui_query_widget"));
			OutStructured->SetStringField(TEXT("contract_version"), TEXT("editor_ui.semantic.v1"));
			OutStructured->SetStringField(TEXT("status"), TEXT("success"));
			OutStructured->SetArrayField(TEXT("windows"), Windows);
			OutStructured->SetNumberField(TEXT("window_count"), Windows.Num());
			OutStructured->SetArrayField(TEXT("widgets"), MakeEditorUIWidgetArray(Candidates));
			OutStructured->SetNumberField(TEXT("match_count"), Candidates.Num());
			OutSummary = FString::Printf(TEXT("Matched %d Slate widget(s)."), Candidates.Num());
			return true;
		},
		nullptr,
		1
	});

	Registry.Register({
		TEXT("editor_ui_click_widget"),
		TEXT("Fail-closed semantic click wrapper for a visible enabled unique button-like Slate widget. "
			 "Defaults to dry_run. Requires allow_click=true for live clicks and refuses ambiguous, hidden, disabled, geometry-less, or dangerous labels."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("window"), FSololmcpSchemaBuilder::String(
					TEXT("Window scope: focused (default), main, window, all."),
					{TEXT("focused"), TEXT("main"), TEXT("window"), TEXT("all")})},
				{TEXT("window_title"), FSololmcpSchemaBuilder::String(TEXT("Optional title substring when window=window/all."))},
				{TEXT("widget_id"), FSololmcpSchemaBuilder::String(TEXT("Widget id from editor_ui_query_widget/editor_ui_list_toolbar_buttons."))},
				{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("Optional widget type substring. Usually SButton/SCheckBox."))},
				{TEXT("text"), FSololmcpSchemaBuilder::String(TEXT("Required unless widget_id is supplied. Text/tooltip substring."))},
				{TEXT("tag"), FSololmcpSchemaBuilder::String(TEXT("Optional Slate widget tag substring."))},
				{TEXT("exact"), FSololmcpSchemaBuilder::Boolean(TEXT("Use exact matching for type/text/tag. Default: false."))},
				{TEXT("case_sensitive"), FSololmcpSchemaBuilder::Boolean(TEXT("Use case-sensitive matching. Default: false."))},
				{TEXT("max_depth"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum Slate tree depth. Default: 8."))},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Preview without clicking. Default: true."))},
				{TEXT("allow_click"), FSololmcpSchemaBuilder::Boolean(TEXT("Must be true to dispatch a live click. Default: false."))}
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!FSlateApplication::IsInitialized())
			{
				OutError = TEXT("Slate application is not initialized.");
				return false;
			}

			bool bDryRun = true;
			bool bAllowClick = false;
			Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
			Arguments->TryGetBoolField(TEXT("allow_click"), bAllowClick);

			FEditorUIWidgetSelector Selector = ReadEditorUIWidgetSelector(Arguments);
			if (Selector.WidgetId.IsEmpty() && Selector.Text.IsEmpty() && Selector.Tag.IsEmpty())
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("tool"), TEXT("editor_ui_click_widget"));
				OutStructured->SetStringField(TEXT("contract_version"), TEXT("editor_ui.semantic.v1"));
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_missing_selector"));
				OutError = TEXT("editor_ui_click_widget requires widget_id, text, or tag selector.");
				return false;
			}

			TSharedRef<FJsonObject> QueryArgs = MakeShared<FJsonObject>();
			for (const auto& Pair : Arguments->Values)
			{
				QueryArgs->SetField(FString(*Pair.Key), Pair.Value);
			}
			QueryArgs->SetBoolField(TEXT("button_like_only"), true);
			QueryArgs->SetNumberField(TEXT("max_results"), 8);

			TArray<FEditorUIWidgetCandidate> Candidates;
			TArray<TSharedRef<FJsonObject>> WindowObjects;
			CollectEditorUIWidgetsForArgs(QueryArgs, true, Candidates, WindowObjects);

			OutStructured->SetStringField(TEXT("tool"), TEXT("editor_ui_click_widget"));
			OutStructured->SetStringField(TEXT("contract_version"), TEXT("editor_ui.semantic.v1"));
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
			OutStructured->SetBoolField(TEXT("allow_click"), bAllowClick);
			OutStructured->SetArrayField(TEXT("matches"), MakeEditorUIWidgetArray(Candidates));
			OutStructured->SetNumberField(TEXT("match_count"), Candidates.Num());

			if (Candidates.Num() != 1)
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("status"), Candidates.IsEmpty() ? TEXT("blocked_no_match") : TEXT("blocked_ambiguous_match"));
				OutError = Candidates.IsEmpty()
					? TEXT("No unique clickable widget matched the selector.")
					: TEXT("Selector matched multiple button-like widgets; refusing to click.");
				return false;
			}

			const FEditorUIWidgetCandidate& Candidate = Candidates[0];
			OutStructured->SetObjectField(TEXT("target"), MakeEditorUIWidgetJson(Candidate));

			if (!Candidate.bVisible || !Candidate.bEnabled || !Candidate.bHasGeometry)
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_not_clickable"));
				OutError = TEXT("Matched widget is hidden, disabled, or has no usable bounds.");
				return false;
			}

			FString DangerousToken;
			if (EditorUIWidgetTextLooksDangerous(Candidate, DangerousToken))
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_dangerous_label"));
				OutStructured->SetStringField(TEXT("dangerous_token"), DangerousToken);
				OutError = FString::Printf(TEXT("Refusing to click potentially dangerous widget label/token: %s"), *DangerousToken);
				return false;
			}

			if (bDryRun)
			{
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutStructured->SetStringField(TEXT("status"), TEXT("dry_run"));
				OutStructured->SetBoolField(TEXT("would_click"), true);
				OutSummary = FString::Printf(
					TEXT("Dry run: would click widget %s at (%.0f, %.0f)."),
					*Candidate.WidgetId,
					Candidate.CenterX,
					Candidate.CenterY);
				return true;
			}

			if (!bAllowClick)
			{
				OutStructured->SetBoolField(TEXT("ok"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("blocked_allow_click_required"));
				OutError = TEXT("Live click requires allow_click=true; dry_run=false alone is not enough.");
				return false;
			}

			bool bDownHandled = false;
			bool bUpHandled = false;
			const bool bClicked = SimulateEditorUILeftClick(Candidate.CenterX, Candidate.CenterY, bDownHandled, bUpHandled);
			OutStructured->SetBoolField(TEXT("ok"), bClicked);
			OutStructured->SetStringField(TEXT("status"), bClicked ? TEXT("clicked") : TEXT("blocked_unhandled"));
			OutStructured->SetBoolField(TEXT("mouse_down_handled"), bDownHandled);
			OutStructured->SetBoolField(TEXT("mouse_up_handled"), bUpHandled);
			if (!bClicked)
			{
				OutError = TEXT("Slate did not handle the semantic click target.");
				return false;
			}
			OutSummary = FString::Printf(TEXT("Clicked widget %s at (%.0f, %.0f)."), *Candidate.WidgetId, Candidate.CenterX, Candidate.CenterY);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 5. editor_terrain_create  —  全自动创建地形（无需点击菜单）
	//    这是 "点击地形菜单 → 创建地形" 操作的完整自动化版本
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_terrain_create"),
		TEXT("Fully automatically create a landscape/terrain in the editor. "
			 "This automates the complete 'Landscape > Manage > Create Landscape' workflow. "
			 "The landscape is created with the specified dimensions and position. "
			 "Parameters follow UE5 Landscape creation dialog defaults."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("section_size"),
					FSololmcpSchemaBuilder::Integer(TEXT("Quads per landscape section. Valid: 7, 15, 31, 63, 127, 255. Default: 63."))},
				{TEXT("sections_per_component"),
					FSololmcpSchemaBuilder::Integer(TEXT("Sections per component: 1 or 2. Default: 2."))},
				{TEXT("num_components_x"),
					FSololmcpSchemaBuilder::Integer(TEXT("Number of components along X axis. Default: 8."))},
				{TEXT("num_components_y"),
					FSololmcpSchemaBuilder::Integer(TEXT("Number of components along Y axis. Default: 8."))},
				{TEXT("location_x"),  FSololmcpSchemaBuilder::Number(TEXT("World X position (cm). Default: 0."))},
				{TEXT("location_y"),  FSololmcpSchemaBuilder::Number(TEXT("World Y position (cm). Default: 0."))},
				{TEXT("location_z"),  FSololmcpSchemaBuilder::Number(TEXT("World Z position (cm). Default: 0."))},
				{TEXT("scale_x"),     FSololmcpSchemaBuilder::Number(TEXT("X scale (cm/unit). Default: 100."))},
				{TEXT("scale_y"),     FSololmcpSchemaBuilder::Number(TEXT("Y scale (cm/unit). Default: 100."))},
				{TEXT("scale_z"),     FSololmcpSchemaBuilder::Number(TEXT("Z scale (cm/unit). Default: 100."))},
				{TEXT("material"),    FSololmcpSchemaBuilder::String(TEXT("Optional landscape material asset path."))},
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				OutError = TEXT("Editor world is not available.");
				return false;
			}
			if (!World) { OutError = TEXT("No editor world available."); return false; }

			// 读取参数
			int32 SectionSize = 63;
			int32 SectionsPerComponent = 2;
			int32 NumCompX = 8;
			int32 NumCompY = 8;
			double LocX = 0.0, LocY = 0.0, LocZ = 0.0;
			double ScaleX = 100.0, ScaleY = 100.0, ScaleZ = 100.0;
			FString MaterialPath;

			Arguments->TryGetNumberField(TEXT("section_size"),           SectionSize);
			Arguments->TryGetNumberField(TEXT("sections_per_component"), SectionsPerComponent);
			Arguments->TryGetNumberField(TEXT("num_components_x"),       NumCompX);
			Arguments->TryGetNumberField(TEXT("num_components_y"),       NumCompY);
			Arguments->TryGetNumberField(TEXT("location_x"), LocX);
			Arguments->TryGetNumberField(TEXT("location_y"), LocY);
			Arguments->TryGetNumberField(TEXT("location_z"), LocZ);
			Arguments->TryGetNumberField(TEXT("scale_x"), ScaleX);
			Arguments->TryGetNumberField(TEXT("scale_y"), ScaleY);
			Arguments->TryGetNumberField(TEXT("scale_z"), ScaleZ);
			Arguments->TryGetStringField(TEXT("material"), MaterialPath);

			// 验证 SectionSize 合法值
			const TArray<int32> ValidSectionSizes = {7, 15, 31, 63, 127, 255};
			if (!ValidSectionSizes.Contains(SectionSize))
			{
				OutError = FString::Printf(TEXT("Invalid section_size %d. Valid values: 7, 15, 31, 63, 127, 255."), SectionSize);
				return false;
			}
			if (SectionsPerComponent != 1 && SectionsPerComponent != 2)
			{
				OutError = TEXT("sections_per_component must be 1 or 2.");
				return false;
			}

			// 激活地形编辑器模式
			TerrainModeGuard::FSelectionScope ModeGuard;
			if (!ModeGuard.Begin(OutError))
			{
				ModeGuard.Attach(OutStructured);
				return false;
			}
			ModeGuard.Attach(OutStructured);

			// 计算实际地图大小
			const int32 QuadsPerComponent = SectionSize * SectionsPerComponent;
			const int32 TotalQuadsX = QuadsPerComponent * NumCompX;
			const int32 TotalQuadsY = QuadsPerComponent * NumCompY;

			// 材质加载（可选）
			UMaterialInterface* LandscapeMaterial = nullptr;
			if (!MaterialPath.IsEmpty())
			{
				LandscapeMaterial = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
				if (!LandscapeMaterial)
				{
					UE_LOG(LogSOMOLMCPEditorUI, Warning, TEXT("Could not load landscape material: %s"), *MaterialPath);
				}
			}

			// 构建高度图数据（全部填充为中间值 32768 = 水平）
			const int32 HeightmapSizeX = TotalQuadsX + 1;
			const int32 HeightmapSizeY = TotalQuadsY + 1;
			TArray<uint16> HeightData;
			HeightData.Init(32768, HeightmapSizeX * HeightmapSizeY);

			// Transform
			FTransform LandscapeTransform;
			LandscapeTransform.SetLocation(FVector(LocX, LocY, LocZ));
			LandscapeTransform.SetScale3D(FVector(ScaleX, ScaleY, ScaleZ));

			// 使用 FScopedTransaction 以支持撤销
			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "CreateLandscape", "SOMOLMCP Create Landscape"));

			// 创建地形 Actor
			ALandscape* LandscapeActor = World->SpawnActor<ALandscape>(
				ALandscape::StaticClass(),
				LandscapeTransform
			);

			if (!LandscapeActor)
			{
				OutError = TEXT("Failed to spawn Landscape actor.");
				return false;
			}

			// 初始化地形数据
			// ALandscape::Import() 签名因 UE5 版本略有不同，使用 TMap<FGuid,...> 形式
			{
				FGuid LandscapeGuid = FGuid::NewGuid();
				TMap<FGuid, TArray<uint16>> HeightDataPerLoc;
				HeightDataPerLoc.Add(FGuid(), MoveTemp(HeightData));

				TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLoc;
				MaterialLayerDataPerLoc.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

				// UE 5.7: Import now requires InImportLayers parameter
				
				LandscapeActor->Import(
					LandscapeGuid,
					0, 0,                   // MinX, MinY
					TotalQuadsX,            // MaxX
					TotalQuadsY,            // MaxY
					SectionsPerComponent,
					SectionSize,
					HeightDataPerLoc,
					nullptr,                // ImportPath (nullptr = no file import)
					MaterialLayerDataPerLoc,
					ELandscapeImportAlphamapType::Additive,
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 4
					nullptr
#else
					TArrayView<const FLandscapeLayer>()
#endif
				);

				if (ULandscapeInfo* Info = LandscapeActor->GetLandscapeInfo())
				{
					Info->UpdateLayerInfoMap(LandscapeActor);
				}
				LandscapeActor->RegisterAllComponents();
				LandscapeActor->PostEditChange();
				LandscapeActor->MarkComponentsRenderStateDirty();
			}

			// 设置材质
			if (LandscapeMaterial)
			{
				LandscapeActor->LandscapeMaterial = LandscapeMaterial;
			}

			// 标记为已修改
			LandscapeActor->MarkPackageDirty();
			GEditor->RedrawAllViewports();

			// 构建返回信息
			OutStructured->SetStringField(TEXT("actor_name"), LandscapeActor->GetActorLabel());
			OutStructured->SetStringField(TEXT("actor_path"), LandscapeActor->GetPathName());
			OutStructured->SetNumberField(TEXT("total_quads_x"), TotalQuadsX);
			OutStructured->SetNumberField(TEXT("total_quads_y"), TotalQuadsY);
			OutStructured->SetNumberField(TEXT("heightmap_resolution_x"), HeightmapSizeX);
			OutStructured->SetNumberField(TEXT("heightmap_resolution_y"), HeightmapSizeY);
			OutStructured->SetNumberField(TEXT("section_size"), SectionSize);
			OutStructured->SetNumberField(TEXT("sections_per_component"), SectionsPerComponent);
			OutStructured->SetNumberField(TEXT("num_components_x"), NumCompX);
			OutStructured->SetNumberField(TEXT("num_components_y"), NumCompY);
			OutStructured->SetBoolField(TEXT("has_material"), LandscapeMaterial != nullptr);

			OutSummary = FString::Printf(
				TEXT("Created Landscape '%s' (%dx%d quads, %dx%d components)."),
				*LandscapeActor->GetActorLabel(),
				TotalQuadsX, TotalQuadsY,
				NumCompX, NumCompY
			);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 6. editor_terrain_sculpt  —  地形雕刻操作（通过 Python 调用地形 API）
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_terrain_sculpt"),
		TEXT("FAIL-CLOSED (terrain fix batch 0+1, 2026-07-21): landscape sculpt writing is blocked. "
			 "Always returns blocked_pending_sculpt_writer - the previous implementation activated "
			 "Landscape mode and reported success without applying any height modification (fake success). "
			 "Use landscape_patch_edit_layer_create / landscape_circle_patch_create / "
			 "mesh_terrain_height_sculpt_apply (planned writers) or world_create_terrain_scene instead. "
			 "A dedicated sculpt writer may be promoted after live fixture proof."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("landscape_actor"), FSololmcpSchemaBuilder::String(TEXT("Label or path of the landscape actor to sculpt."))},
				{TEXT("operation"),       FSololmcpSchemaBuilder::String(
					TEXT("Sculpt operation: Sculpt | Smooth | Flatten | Erosion | HydraErosion | Noise | Retopologize"),
					{TEXT("Sculpt"), TEXT("Smooth"), TEXT("Flatten"), TEXT("Erosion"), TEXT("HydraErosion"), TEXT("Noise"), TEXT("Retopologize")})},
				{TEXT("brush_size"),      FSololmcpSchemaBuilder::Number(TEXT("Brush radius in cm. Default: 2048."))},
				{TEXT("brush_falloff"),   FSololmcpSchemaBuilder::Number(TEXT("Brush falloff 0.0-1.0. Default: 0.5."))},
				{TEXT("tool_strength"),   FSololmcpSchemaBuilder::Number(TEXT("Tool strength 0.0-1.0. Default: 0.3."))},
				{TEXT("positions"),       FSololmcpSchemaBuilder::Array(
					FSololmcpSchemaBuilder::Object({
						{TEXT("x"), FSololmcpSchemaBuilder::Number()},
						{TEXT("y"), FSololmcpSchemaBuilder::Number()},
						{TEXT("height"), FSololmcpSchemaBuilder::Number(TEXT("Target height (for Flatten). Otherwise ignored."))}
					}),
					TEXT("World positions where the brush is applied."))},
			},
			{TEXT("landscape_actor"), TEXT("operation"), TEXT("positions")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			// Terrain fix batch 0+1 (2026-07-21): fail-closed. The previous
			// implementation activated Landscape mode, echoed parameters, and
			// reported success without modifying any height data (fake success).
			// Same fail-closed pattern as blocked_pending_mesh_terrain_writer
			// (SololmcpMeshTerrainModeP1Tools.cpp).
			FString LandscapeActorId, Operation;
			Arguments->TryGetStringField(TEXT("landscape_actor"), LandscapeActorId);
			Arguments->TryGetStringField(TEXT("operation"), Operation);
			OutStructured->SetBoolField(TEXT("success"), false);
			OutStructured->SetBoolField(TEXT("ok"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("blocked_pending_sculpt_writer"));
			OutStructured->SetStringField(TEXT("error_code"), TEXT("blocked_pending_sculpt_writer"));
			OutStructured->SetStringField(TEXT("reason_code"), TEXT("blocked_pending_sculpt_writer"));
			OutStructured->SetStringField(TEXT("failure_route"), TEXT("promote_dedicated_sculpt_writer_after_live_fixture_and_receipt"));
			OutStructured->SetStringField(TEXT("landscape_actor"), LandscapeActorId);
			OutStructured->SetStringField(TEXT("operation"), Operation);
			OutError = TEXT("editor_terrain_sculpt blocked_pending_sculpt_writer: landscape sculpt execute is blocked until a dedicated sculpt writer has live fixture proof. Use landscape_patch_edit_layer_create / landscape_circle_patch_create / mesh_terrain_height_sculpt_apply instead.");
			OutSummary = OutError;
			return false;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 7. editor_pcg_create_volume  —  创建 PCG 体积并绑定 PCG Graph
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_pcg_create_volume"),
		TEXT("Create a PCG (Procedural Content Generation) volume actor in the level and optionally attach a PCG graph to it. "
			 "This automates the 'Place Actor > PCG Volume' + 'Set PCG Graph' workflow. "
			 "After creation, call 'pcg_generate' to generate the procedural content."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("location_x"), FSololmcpSchemaBuilder::Number(TEXT("World X position. Default: 0."))},
				{TEXT("location_y"), FSololmcpSchemaBuilder::Number(TEXT("World Y position. Default: 0."))},
				{TEXT("location_z"), FSololmcpSchemaBuilder::Number(TEXT("World Z position. Default: 0."))},
				{TEXT("extent_x"),   FSololmcpSchemaBuilder::Number(TEXT("Box half-extent X (cm). Default: 5000."))},
				{TEXT("extent_y"),   FSololmcpSchemaBuilder::Number(TEXT("Box half-extent Y (cm). Default: 5000."))},
				{TEXT("extent_z"),   FSololmcpSchemaBuilder::Number(TEXT("Box half-extent Z (cm). Default: 5000."))},
				{TEXT("pcg_graph"),  FSololmcpSchemaBuilder::String(TEXT("Optional PCG Graph asset path, e.g. /Game/PCG/MyGraph."))},
				{TEXT("actor_label"),FSololmcpSchemaBuilder::String(TEXT("Optional label for the new actor."))},
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			double LocX = 0.0, LocY = 0.0, LocZ = 0.0;
			double ExtX = 5000.0, ExtY = 5000.0, ExtZ = 5000.0;
			FString PCGGraphPath, ActorLabel;

			Arguments->TryGetNumberField(TEXT("location_x"), LocX);
			Arguments->TryGetNumberField(TEXT("location_y"), LocY);
			Arguments->TryGetNumberField(TEXT("location_z"), LocZ);
			Arguments->TryGetNumberField(TEXT("extent_x"),   ExtX);
			Arguments->TryGetNumberField(TEXT("extent_y"),   ExtY);
			Arguments->TryGetNumberField(TEXT("extent_z"),   ExtZ);
			Arguments->TryGetStringField(TEXT("pcg_graph"),  PCGGraphPath);
			Arguments->TryGetStringField(TEXT("actor_label"),ActorLabel);

			// 通过 Python 创建 PCG Volume（最可靠的方式）
			FString PythonCode;
			PythonCode += TEXT("import unreal\n");
			PythonCode += TEXT("import json\n");
			PythonCode += FString::Printf(TEXT("loc = unreal.Vector(%f, %f, %f)\n"), LocX, LocY, LocZ);
			PythonCode += FString::Printf(TEXT("ext = unreal.Vector(%f, %f, %f)\n"), ExtX, ExtY, ExtZ);
			PythonCode += TEXT("world = unreal.EditorLevelLibrary.get_editor_world()\n");
			PythonCode += TEXT("pcg_actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.PCGVolume, loc)\n");
			PythonCode += TEXT("if pcg_actor:\n");

			if (!ActorLabel.IsEmpty())
			{
				PythonCode += FString::Printf(TEXT("    pcg_actor.set_actor_label('%s')\n"), *ActorLabel);
			}

			// 设置 Box Extent
			PythonCode += TEXT("    box_comp = pcg_actor.get_component_by_class(unreal.BoxComponent)\n");
			PythonCode += TEXT("    if box_comp:\n");
			PythonCode += FString::Printf(TEXT("        box_comp.set_box_extent(ext)\n"));

			// 设置 PCG Graph
			if (!PCGGraphPath.IsEmpty())
			{
				PythonCode += FString::Printf(TEXT("    pcg_graph = unreal.load_asset('%s')\n"), *PCGGraphPath);
				PythonCode += TEXT("    if pcg_graph:\n");
				PythonCode += TEXT("        pcg_comp = pcg_actor.get_component_by_class(unreal.PCGComponent)\n");
				PythonCode += TEXT("        if pcg_comp:\n");
				PythonCode += TEXT("            pcg_comp.set_graph(pcg_graph)\n");
				PythonCode += TEXT("            print(f'PCG Graph set: {pcg_graph.get_path_name()}')\n");
			}

			PythonCode += TEXT("    pcg_actor.modify()\n");
			PythonCode += TEXT("    print(f'PCG_ACTOR_CREATED:{pcg_actor.get_actor_label()}:{pcg_actor.get_path_name()}')\n");
			PythonCode += TEXT("else:\n");
			PythonCode += TEXT("    print('ERROR:Failed to spawn PCGVolume actor')\n");

			TSharedRef<FJsonObject> PythonResult = MakeShared<FJsonObject>();
			FString PythonSummary, PythonError;
			bool bOk = Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, PythonResult, PythonSummary, PythonError);

			OutStructured->SetBoolField(TEXT("python_success"), bOk);
			OutStructured->SetStringField(TEXT("python_output"), PythonSummary);
			if (!PythonError.IsEmpty())
				OutStructured->SetStringField(TEXT("python_error"), PythonError);
			OutStructured->SetNumberField(TEXT("location_x"), LocX);
			OutStructured->SetNumberField(TEXT("location_y"), LocY);
			OutStructured->SetNumberField(TEXT("location_z"), LocZ);
			OutStructured->SetNumberField(TEXT("extent_x"), ExtX);
			OutStructured->SetNumberField(TEXT("extent_y"), ExtY);
			OutStructured->SetNumberField(TEXT("extent_z"), ExtZ);
			OutStructured->SetBoolField(TEXT("has_graph"), !PCGGraphPath.IsEmpty());

			GEditor->RedrawAllViewports();

			OutSummary = FString::Printf(
				TEXT("PCG Volume created at (%.0f, %.0f, %.0f) with extent (%.0f, %.0f, %.0f)%s."),
				LocX, LocY, LocZ, ExtX, ExtY, ExtZ,
				PCGGraphPath.IsEmpty() ? TEXT("") : TEXT(" with PCG Graph attached")
			);
			return bOk;
		},
	nullptr,
	0,
	nullptr,
	true
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 8. editor_pcg_full_setup  —  PCG 全自动工作流：创建 Graph + Volume + 生成
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_pcg_full_setup"),
		TEXT("Complete PCG full automation workflow: creates a PCG Graph asset with node network, "
			 "creates a PCG Volume actor, attaches the graph, then triggers generation. "
			 "This is equivalent to manually: File>New PCG Graph → Add nodes → Place Volume → Generate."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("graph_asset_path"), FSololmcpSchemaBuilder::String(TEXT("Asset path for the new PCG Graph, e.g. /Game/PCG/MyForestGraph."))},
				{TEXT("setup_type"),       FSololmcpSchemaBuilder::String(
					TEXT("Preset PCG setup type: 'scatter_foliage' (random foliage scatter), 'scatter_rocks' (rock scatter), 'scatter_on_landscape' (landscape-aware scatter), 'custom' (empty graph)."),
					{TEXT("scatter_foliage"), TEXT("scatter_rocks"), TEXT("scatter_on_landscape"), TEXT("custom")})},
				{TEXT("volume_location_x"),FSololmcpSchemaBuilder::Number(TEXT("Volume world X. Default: 0."))},
				{TEXT("volume_location_y"),FSololmcpSchemaBuilder::Number(TEXT("Volume world Y. Default: 0."))},
				{TEXT("volume_location_z"),FSololmcpSchemaBuilder::Number(TEXT("Volume world Z. Default: 0."))},
				{TEXT("volume_extent"),    FSololmcpSchemaBuilder::Number(TEXT("Volume box half-extent (all axes, cm). Default: 10000."))},
				{TEXT("seed"),             FSololmcpSchemaBuilder::Integer(TEXT("Random seed for generation. Default: 42."))},
				{TEXT("density"),          FSololmcpSchemaBuilder::Number(TEXT("Point density (points per m^2). Default: 0.1."))},
				{TEXT("mesh_paths"),       FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Static mesh asset paths to scatter."))},
			},
			{TEXT("graph_asset_path"), TEXT("setup_type")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			FString GraphAssetPath, SetupType;
			double VolLocX = 0.0, VolLocY = 0.0, VolLocZ = 0.0, VolExtent = 10000.0;
			int32 Seed = 42;
			double Density = 0.1;

			Arguments->TryGetStringField(TEXT("graph_asset_path"), GraphAssetPath);
			Arguments->TryGetStringField(TEXT("setup_type"),        SetupType);
			Arguments->TryGetNumberField(TEXT("volume_location_x"), VolLocX);
			Arguments->TryGetNumberField(TEXT("volume_location_y"), VolLocY);
			Arguments->TryGetNumberField(TEXT("volume_location_z"), VolLocZ);
			Arguments->TryGetNumberField(TEXT("volume_extent"),     VolExtent);
			Arguments->TryGetNumberField(TEXT("seed"),              Seed);
			Arguments->TryGetNumberField(TEXT("density"),           Density);

			// 收集 mesh paths
			const TArray<TSharedPtr<FJsonValue>>* MeshPathsArray = nullptr;
			TArray<FString> MeshPaths;
			if (Arguments->TryGetArrayField(TEXT("mesh_paths"), MeshPathsArray))
			{
				for (const auto& Val : *MeshPathsArray)
				{
					FString Path;
					if (Val.IsValid() && Val->TryGetString(Path))
						MeshPaths.Add(Path);
				}
			}

			// 构建 PCG Graph 创建 Python 代码
			FString PythonCode;
			PythonCode += TEXT("import unreal\n");
			PythonCode += TEXT("import json\n\n");

			// 第一步：创建 PCG Graph 资产
			PythonCode += FString::Printf(TEXT("graph_path = '%s'\n"), *GraphAssetPath);

			// 提取包路径和资产名称
			FString PackagePath = FPackageName::GetLongPackagePath(GraphAssetPath);
			FString AssetName   = FPackageName::GetLongPackageAssetName(GraphAssetPath);

			PythonCode += FString::Printf(TEXT("pkg_path = '%s'\n"), *PackagePath);
			PythonCode += FString::Printf(TEXT("asset_name = '%s'\n"), *AssetName);
			PythonCode += TEXT("asset_tools = unreal.AssetToolsHelpers.get_asset_tools()\n");
			PythonCode += TEXT("pcg_factory = unreal.PCGGraphFactory()\n");
			PythonCode += TEXT("pcg_graph = asset_tools.create_asset(asset_name, pkg_path, unreal.PCGGraph, pcg_factory)\n");
			PythonCode += TEXT("if not pcg_graph:\n");
			PythonCode += TEXT("    print('ERROR:Failed to create PCG Graph asset')\n");
			PythonCode += TEXT("    exit()\n\n");

			// 第二步：根据 setup_type 配置节点
			if (SetupType == TEXT("scatter_foliage") || SetupType == TEXT("scatter_rocks"))
			{
				PythonCode += TEXT("# Configure scatter nodes\n");
				PythonCode += TEXT("controller = pcg_graph.get_mutable_pcg_graph_controller()\n");
				PythonCode += FString::Printf(TEXT("density_val = %f\n"), Density);
				PythonCode += FString::Printf(TEXT("seed_val = %d\n"), Seed);

				// 添加 Surface Sampler 节点
				PythonCode += TEXT("# Add Surface Sampler\n");
				PythonCode += TEXT("sampler_settings = unreal.PCGSurfaceSamplerSettings()\n");
				PythonCode += TEXT("sampler_settings.set_editor_property('points_per_squared_meter', density_val)\n");
				PythonCode += TEXT("sampler_settings.set_editor_property('seed', seed_val)\n");
				PythonCode += TEXT("sampler_node = pcg_graph.add_node(sampler_settings)\n");
				PythonCode += TEXT("if sampler_node:\n");
				PythonCode += TEXT("    sampler_node.set_editor_property('node_title', 'Surface Sampler')\n");
				PythonCode += TEXT("    print(f'Added Surface Sampler node: {sampler_node}')\n");

				// 添加 Static Mesh Spawner
				if (MeshPaths.Num() > 0)
				{
					PythonCode += TEXT("# Add Static Mesh Spawner\n");
					PythonCode += TEXT("spawner_settings = unreal.PCGStaticMeshSpawnerSettings()\n");
					PythonCode += TEXT("mesh_entries = []\n");
					for (const FString& MeshPath : MeshPaths)
					{
						PythonCode += FString::Printf(TEXT("mesh = unreal.load_asset('%s')\n"), *MeshPath);
						PythonCode += TEXT("if mesh:\n");
						PythonCode += TEXT("    entry = unreal.PCGSoftISMComponentDescriptor()\n");
						PythonCode += TEXT("    entry.set_editor_property('static_mesh', mesh)\n");
						PythonCode += TEXT("    mesh_entries.append(entry)\n");
					}
					PythonCode += TEXT("spawner_settings.set_editor_property('mesh_entries', mesh_entries)\n");
					PythonCode += TEXT("spawner_node = pcg_graph.add_node(spawner_settings)\n");
					PythonCode += TEXT("if spawner_node and sampler_node:\n");
					PythonCode += TEXT("    sampler_out = sampler_node.get_output_pin('Out')\n");
					PythonCode += TEXT("    spawner_in = spawner_node.get_input_pin('In')\n");
					PythonCode += TEXT("    if sampler_out and spawner_in:\n");
					PythonCode += TEXT("        pcg_graph.add_labeled_edge(sampler_out, spawner_in)\n");
				}
			}
			else if (SetupType == TEXT("scatter_on_landscape"))
			{
				PythonCode += TEXT("# Landscape-aware scatter\n");
				PythonCode += TEXT("# Get landscape bounds input\n");
				PythonCode += TEXT("print('INFO: landscape-aware setup requires connecting to landscape input manually')\n");
			}

			// 保存资产
			PythonCode += TEXT("unreal.EditorAssetLibrary.save_asset(pcg_graph.get_path_name())\n");

			// 第三步：创建 PCG Volume Actor
			PythonCode += FString::Printf(TEXT("\n# Create PCG Volume\nloc = unreal.Vector(%f, %f, %f)\n"), VolLocX, VolLocY, VolLocZ);
			PythonCode += FString::Printf(TEXT("ext = unreal.Vector(%f, %f, %f)\n"), VolExtent, VolExtent, VolExtent);
			PythonCode += TEXT("pcg_actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.PCGVolume, loc)\n");
			PythonCode += TEXT("if pcg_actor:\n");
			PythonCode += TEXT("    box_comp = pcg_actor.get_component_by_class(unreal.BoxComponent)\n");
			PythonCode += TEXT("    if box_comp: box_comp.set_box_extent(ext)\n");
			PythonCode += TEXT("    pcg_comp = pcg_actor.get_component_by_class(unreal.PCGComponent)\n");
			PythonCode += TEXT("    if pcg_comp:\n");
			PythonCode += TEXT("        pcg_comp.set_graph(pcg_graph)\n");
			PythonCode += TEXT("        pcg_comp.generate(True)  # blocking generate\n");
			PythonCode += TEXT("    print(f'PCG_FULL_SETUP_DONE:{pcg_actor.get_actor_label()}:{pcg_graph.get_path_name()}')\n");
			PythonCode += TEXT("else:\n");
			PythonCode += TEXT("    print('ERROR:Failed to create PCG Volume')\n");

			TSharedRef<FJsonObject> PythonResult = MakeShared<FJsonObject>();
			FString PythonSummary, PythonError;
			bool bOk = Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, PythonResult, PythonSummary, PythonError);

			OutStructured->SetBoolField(TEXT("success"), bOk);
			OutStructured->SetStringField(TEXT("graph_asset_path"), GraphAssetPath);
			OutStructured->SetStringField(TEXT("setup_type"), SetupType);
			OutStructured->SetStringField(TEXT("python_output"), PythonSummary);
			if (!PythonError.IsEmpty())
				OutStructured->SetStringField(TEXT("python_error"), PythonError);

			GEditor->RedrawAllViewports();
			OutSummary = FString::Printf(TEXT("PCG full setup '%s' (%s): graph created, volume placed, generation triggered."),
				*GraphAssetPath, *SetupType);
			return bOk;
		},
	nullptr,
	0,
	nullptr,
	true
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 9. editor_ui_open_asset  —  在对应编辑器中打开资产
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_open_asset"),
		TEXT("Open an asset in its editor window (Blueprint Editor, Material Editor, Texture Editor, etc.). "
			 "This is equivalent to double-clicking an asset in the Content Browser."),
		FSololmcpSchemaBuilder::Object(
			{{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the asset, e.g. /Game/BP_MyActor."))}},
			{TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				OutError = TEXT("Missing argument: asset_path");
				return false;
			}

			UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
			if (!Asset) return false;

			UAssetEditorSubsystem* EditorSubsystem = Context.Services.GetAssetEditorSubsystem(OutError);
			if (!EditorSubsystem) return false;

			TWeakObjectPtr<UObject> AssetPtr(Asset);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([AssetPtr](float)
			{
				if (UObject* DeferredAsset = AssetPtr.Get())
				{
					if (GEditor)
					{
						if (UAssetEditorSubsystem* DeferredSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
						{
							DeferredSubsystem->OpenEditorForAsset(DeferredAsset);
						}
					}
				}
				return false;
			}), 0.01f);

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
			OutStructured->SetBoolField(TEXT("open_scheduled"), true);
			OutSummary = FString::Printf(TEXT("Scheduled asset '%s' (%s) to open in editor."),
				*AssetPath, *Asset->GetClass()->GetName());
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 10. editor_ui_close_asset  —  关闭资产编辑器
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_close_asset"),
		TEXT("Close the editor window for a specific asset. Optionally save before closing."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Content path of the asset to close."))},
				{TEXT("save"),       FSololmcpSchemaBuilder::Boolean(TEXT("Save before closing. Default: false."))}
			},
			{TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString AssetPath;
			bool bSave = false;
			Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
			Arguments->TryGetBoolField(TEXT("save"), bSave);

			UObject* Asset = Context.Services.LoadAsset(AssetPath, OutError);
			if (!Asset) return false;

			UAssetEditorSubsystem* EditorSubsystem = Context.Services.GetAssetEditorSubsystem(OutError);
			if (!EditorSubsystem) return false;

			if (bSave)
			{
				FString SaveError;
				Context.Services.SaveAsset(AssetPath, false, SaveError);
			}

			IAssetEditorInstance* EditorInstance = EditorSubsystem->FindEditorForAsset(Asset, false);
			if (EditorInstance)
			{
				EditorInstance->CloseWindow(EAssetEditorCloseReason::EditorRefreshRequested);
				OutStructured->SetBoolField(TEXT("closed"), true);
			}
			else
			{
				OutStructured->SetBoolField(TEXT("closed"), false);
				OutStructured->SetStringField(TEXT("note"), TEXT("Asset editor was not open."));
			}

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutSummary = FString::Printf(TEXT("Closed editor for '%s' (saved=%s)."),
				*AssetPath, bSave ? TEXT("true") : TEXT("false"));
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 11. editor_ui_focus_viewport  —  聚焦到指定 Actor 或显式设置相机位姿
	//
	// FIX (v12): in MCP-driven mode, the original impl called
	// MoveViewportCamerasToActor() + RedrawAllViewports(), but the level
	// editor viewport client wasn't being driven by Slate input each tick, so
	// the camera state never actually applied to the next render frame. Result:
	// every screenshot showed the SAME stuck view regardless of focus calls.
	//
	// New impl directly grabs FLevelEditorViewportClient and:
	//   - if actor_id given: compute frame-fitting position from actor bounds
	//   - if location/rotation given: set those explicitly
	//   - call SetViewLocation/SetViewRotation, then Invalidate(true,true)
	//   - call GEditor->RedrawAllViewports(true) to flush
	// This finally lets MCP drive the camera reliably.
	// ─────────────────────────────────────────────────────────────────────────
	// Headless MCP runs can leave the level viewport non-realtime, causing stale or
	// black editor_screenshot_viewport captures. Force realtime and a redraw.
	Registry.Register({
		TEXT("editor_viewport_set_realtime"),
		TEXT("Set realtime on the active level editor viewport, or on perspective level viewport clients when no active viewport is available, then force redraw."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Realtime enabled flag. Default true."))}
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			bool bEnabled = true;
			Arguments->TryGetBoolField(TEXT("enabled"), bEnabled);

			FViewport* ActiveViewport = GEditor->GetActiveViewport();
			const TArray<FLevelEditorViewportClient*>& ViewportClients = GEditor->GetLevelViewportClients();
			TArray<FLevelEditorViewportClient*> TargetClients;
			bool bMatchedActiveViewport = false;
			for (FLevelEditorViewportClient* VC : ViewportClients)
			{
				if (VC && VC->Viewport && VC->Viewport == ActiveViewport)
				{
					TargetClients.Add(VC);
					bMatchedActiveViewport = true;
					break;
				}
			}
			if (TargetClients.IsEmpty())
			{
				for (FLevelEditorViewportClient* VC : ViewportClients)
				{
					if (VC && VC->IsPerspective())
					{
						TargetClients.Add(VC);
					}
				}
			}
			if (TargetClients.IsEmpty())
			{
				for (FLevelEditorViewportClient* VC : ViewportClients)
				{
					if (VC)
					{
						TargetClients.Add(VC);
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 UpdatedCount = 0;
			int32 DrawCount = 0;
			for (int32 Index = 0; Index < ViewportClients.Num(); ++Index)
			{
				FLevelEditorViewportClient* VC = ViewportClients[Index];
				if (!VC)
				{
					continue;
				}

				const bool bSelected = TargetClients.Contains(VC);
				const bool bWasRealtime = VC->IsRealtime();
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("index"), Index);
				Row->SetBoolField(TEXT("selected"), bSelected);
				Row->SetBoolField(TEXT("matched_active_viewport"), VC->Viewport && VC->Viewport == ActiveViewport);
				Row->SetBoolField(TEXT("is_perspective"), VC->IsPerspective());
				Row->SetBoolField(TEXT("had_viewport"), VC->Viewport != nullptr);
				Row->SetBoolField(TEXT("was_realtime"), bWasRealtime);

				if (bSelected)
				{
					VC->SetRealtime(bEnabled);
					if (bEnabled)
					{
						VC->RequestRealTimeFrames(3);
					}
					VC->Invalidate(true, true);
					if (VC->Viewport)
					{
						VC->Viewport->Invalidate();
						VC->Viewport->Draw(true);
						++DrawCount;
					}
					++UpdatedCount;
				}

				Row->SetBoolField(TEXT("is_realtime"), VC->IsRealtime());
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			if (UpdatedCount <= 0)
			{
				OutStructured->SetBoolField(TEXT("success"), false);
				OutStructured->SetStringField(TEXT("status"), TEXT("no_level_viewport_clients"));
				OutStructured->SetNumberField(TEXT("level_viewport_client_count"), ViewportClients.Num());
				OutError = TEXT("No level editor viewport clients were available.");
				return false;
			}

			GEditor->RedrawLevelEditingViewports(true);
			GEditor->RedrawAllViewports(true);

			OutStructured->SetBoolField(TEXT("success"), true);
			OutStructured->SetStringField(TEXT("tool"), TEXT("editor_viewport_set_realtime"));
			OutStructured->SetStringField(TEXT("status"), TEXT("realtime_updated"));
			OutStructured->SetBoolField(TEXT("enabled"), bEnabled);
			OutStructured->SetBoolField(TEXT("active_viewport_available"), ActiveViewport != nullptr);
			OutStructured->SetBoolField(TEXT("matched_active_viewport"), bMatchedActiveViewport);
			OutStructured->SetNumberField(TEXT("level_viewport_client_count"), ViewportClients.Num());
			OutStructured->SetNumberField(TEXT("updated_count"), UpdatedCount);
			OutStructured->SetNumberField(TEXT("draw_count"), DrawCount);
			OutStructured->SetArrayField(TEXT("viewport_clients"), Rows);
			OutSummary = FString::Printf(
				TEXT("Set realtime=%s on %d level editor viewport client(s); forced redraw (%d direct viewport draw calls)."),
				bEnabled ? TEXT("true") : TEXT("false"),
				UpdatedCount,
				DrawCount);
			return true;
		}
	});

	Registry.Register({
		TEXT("editor_ui_focus_viewport"),
		TEXT("Move the level editor viewport camera. Either focus on an actor (frame-fits to bounds) "
			 "or set camera location+rotation explicitly. After this call the next screenshot will see "
			 "the new camera. Both modes call FLevelEditorViewportClient directly + force redraw."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("actor_id"),     FSololmcpSchemaBuilder::String(TEXT("Actor label or path to focus on. If empty and no location given, focuses on selected actors."))},
				{TEXT("location"),     FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}}, {TEXT("x"), TEXT("y"), TEXT("z")}, TEXT("Canonical camera world position in Unreal units (centimeters). Overrides actor focus if both are provided."), false)},
				{TEXT("rotation"),     FSololmcpSchemaBuilder::Object({{TEXT("pitch"), FSololmcpSchemaBuilder::Number()}, {TEXT("yaw"), FSololmcpSchemaBuilder::Number()}, {TEXT("roll"), FSololmcpSchemaBuilder::Number()}}, {TEXT("pitch"), TEXT("yaw"), TEXT("roll")}, TEXT("Camera rotation in degrees. Default is pitch=-15, yaw=0, roll=0 for an explicit location."), false)},
				{TEXT("x"), FSololmcpSchemaBuilder::Number(TEXT("Compatibility alias for location.x in Unreal units (centimeters). x, y, and z must be supplied together; canonical location wins if both forms are present."))},
				{TEXT("y"), FSololmcpSchemaBuilder::Number(TEXT("Compatibility alias for location.y in Unreal units (centimeters)."))},
				{TEXT("z"), FSololmcpSchemaBuilder::Number(TEXT("Compatibility alias for location.z in Unreal units (centimeters)."))},
				{TEXT("camera_speed"),FSololmcpSchemaBuilder::Number(TEXT("Optional camera speed multiplier 1-8. Default: 4."))},
				{TEXT("fixed_ev100"), FSololmcpSchemaBuilder::Number(TEXT("Optional: pin the level-editor viewport exposure to this EV100 (editor viewports ignore scene PostProcessVolume exposure). Daylight ~9-12; lower = brighter. Omit to leave viewport exposure unchanged."))},
				{TEXT("view_mode"), FSololmcpSchemaBuilder::String(TEXT("Optional viewport view mode: 'unlit' (truthful vertex/authored colors, no lighting/exposure — the worldspace setting view), 'lit' (default), or 'wireframe'."))},
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			FString ActorId;
			Arguments->TryGetStringField(TEXT("actor_id"), ActorId);

			AActor* TargetActor = nullptr;
			if (!ActorId.IsEmpty())
			{
				TargetActor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (!TargetActor)
				{
					OutError = FString::Printf(TEXT("Actor not found: %s"), *ActorId);
					return false;
				}
				GEditor->SelectNone(true, true);
				GEditor->SelectActor(TargetActor, true, true, true, true);
				OutStructured->SetStringField(TEXT("focused_actor"), TargetActor->GetActorLabel());
			}

			// Parse explicit location/rotation if provided
			bool bHaveExplicitLoc = false;
			FVector ExplicitLoc(0,0,0);
			FString LocationInputShape = TEXT("none");
			const TSharedPtr<FJsonObject>* LocObj = nullptr;
			const bool bHasLocationField = Arguments->HasField(TEXT("location"));
			if (bHasLocationField)
			{
				if (!Arguments->TryGetObjectField(TEXT("location"), LocObj) || !LocObj || !LocObj->IsValid())
				{
					OutError = TEXT("Invalid argument 'location': expected an object {x,y,z}, in centimeters.");
					return false;
				}
				double x=0,y=0,z=0;
				if (!(*LocObj)->TryGetNumberField(TEXT("x"), x)
					|| !(*LocObj)->TryGetNumberField(TEXT("y"), y)
					|| !(*LocObj)->TryGetNumberField(TEXT("z"), z))
				{
					OutError = TEXT("Invalid argument 'location': numeric fields x, y, and z are all required (Unreal units are centimeters).");
					return false;
				}
				ExplicitLoc = FVector(x, y, z);
				bHaveExplicitLoc = true;
				LocationInputShape = TEXT("nested_canonical");
			}

			const bool bHasTopX = Arguments->HasField(TEXT("x"));
			const bool bHasTopY = Arguments->HasField(TEXT("y"));
			const bool bHasTopZ = Arguments->HasField(TEXT("z"));
			const bool bHasAnyTopLocation = bHasTopX || bHasTopY || bHasTopZ;
			if (bHasAnyTopLocation && !(bHasTopX && bHasTopY && bHasTopZ))
			{
				OutError = TEXT("Invalid top-level location alias: x, y, and z must be supplied together (values are centimeters). Prefer canonical location:{x,y,z}.");
				return false;
			}
			if (bHasAnyTopLocation && !bHaveExplicitLoc)
			{
				double x=0,y=0,z=0;
				if (!Arguments->TryGetNumberField(TEXT("x"), x)
					|| !Arguments->TryGetNumberField(TEXT("y"), y)
					|| !Arguments->TryGetNumberField(TEXT("z"), z))
				{
					OutError = TEXT("Invalid top-level location alias: x, y, and z must be finite JSON numbers in centimeters.");
					return false;
				}
				ExplicitLoc = FVector(x, y, z);
				bHaveExplicitLoc = true;
				LocationInputShape = TEXT("top_level_xyz_compatibility");
			}
			bool bHaveExplicitRot = false;
			FRotator ExplicitRot(0,0,0);
			const TSharedPtr<FJsonObject>* RotObj = nullptr;
			if (Arguments->HasField(TEXT("rotation")))
			{
				if (!Arguments->TryGetObjectField(TEXT("rotation"), RotObj) || !RotObj || !RotObj->IsValid())
				{
					OutError = TEXT("Invalid argument 'rotation': expected an object {pitch,yaw,roll}, in degrees.");
					return false;
				}
				double pitch=0, yaw=0, roll=0;
				if (!(*RotObj)->TryGetNumberField(TEXT("pitch"), pitch)
					|| !(*RotObj)->TryGetNumberField(TEXT("yaw"), yaw)
					|| !(*RotObj)->TryGetNumberField(TEXT("roll"), roll))
				{
					OutError = TEXT("Invalid argument 'rotation': numeric fields pitch, yaw, and roll are all required (degrees).");
					return false;
				}
				ExplicitRot = FRotator(pitch, yaw, roll);
				bHaveExplicitRot = true;
			}
			OutStructured->SetStringField(TEXT("location_input_shape"), LocationInputShape);
			OutStructured->SetStringField(TEXT("location_units"), TEXT("centimeters"));
			OutStructured->SetStringField(TEXT("rotation_units"), TEXT("degrees"));
			if (bHaveExplicitLoc && bHasAnyTopLocation && bHasLocationField)
			{
				OutStructured->SetStringField(TEXT("compatibility_note"), TEXT("Both canonical location and top-level x/y/z were supplied; canonical location was used."));
			}

			// Compute target camera transform
			FVector NewCamLoc = FVector::ZeroVector;
			FRotator NewCamRot = FRotator::ZeroRotator;
			bool bDriveCamera = false;

			if (bHaveExplicitLoc)
			{
				NewCamLoc = ExplicitLoc;
				NewCamRot = bHaveExplicitRot ? ExplicitRot : FRotator(-15.0, 0.0, 0.0);
				bDriveCamera = true;
			}
			else if (TargetActor)
			{
				FBox Bounds = TargetActor->GetComponentsBoundingBox(true);
				if (!Bounds.IsValid || Bounds.GetSize().IsNearlyZero())
				{
					Bounds = FBox::BuildAABB(TargetActor->GetActorLocation(), FVector(100,100,100));
				}
				FVector Center = Bounds.GetCenter();
				FVector Extent = Bounds.GetExtent();
				double Radius = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
				if (Radius < 50.0) Radius = 50.0;
				// position camera diagonally above & to the SE of target
				FVector Offset(-Radius * 4.0, -Radius * 4.0, Radius * 3.0);
				NewCamLoc = Center + Offset;
				NewCamRot = (Center - NewCamLoc).Rotation();
				bDriveCamera = true;
			}

			if (bDriveCamera)
			{
				// Get the actual level-editor viewport client and SET CAMERA DIRECTLY
				FLevelEditorModule& LevelEditor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
				bool bEjectedPilot = false;
				if (ULevelEditorSubsystem* LevelSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
				{
					if (LevelSubsystem->GetPilotLevelActor())
					{
						LevelSubsystem->EjectPilotLevelActor();
						bEjectedPilot = true;
					}
				}
				OutStructured->SetBoolField(TEXT("ejected_existing_pilot"), bEjectedPilot);
				// 可选：固定视口曝光（编辑器视口默认不吃场景 PostProcessVolume 的曝光，
				// 世界编辑器的设定配色需要固定 EV100 才能如实呈现）。
				double FixedEV100 = 0.0;
				const bool bHasFixedEV100 = Arguments->TryGetNumberField(TEXT("fixed_ev100"), FixedEV100);
				FString ViewModeName;
				Arguments->TryGetStringField(TEXT("view_mode"), ViewModeName);
				ViewModeName.TrimStartAndEndInline();
				if (!ViewModeName.IsEmpty()
					&& !ViewModeName.Equals(TEXT("unlit"), ESearchCase::IgnoreCase)
					&& !ViewModeName.Equals(TEXT("lit"), ESearchCase::IgnoreCase)
					&& !ViewModeName.Equals(TEXT("wireframe"), ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(TEXT("Invalid view_mode '%s'. Expected one of: lit, unlit, wireframe."), *ViewModeName);
					return false;
				}
				double CameraSpeed = 4.0;
				const bool bHasCameraSpeed = Arguments->TryGetNumberField(TEXT("camera_speed"), CameraSpeed);
				if (bHasCameraSpeed && (CameraSpeed < 1.0 || CameraSpeed > 8.0))
				{
					OutError = TEXT("Invalid camera_speed: expected a value in the inclusive range 1..8.");
					return false;
				}
				bool bDrove = false;
				int32 UpdatedViewportCount = 0;
				FVector ReadbackLoc = FVector::ZeroVector;
				FRotator ReadbackRot = FRotator::ZeroRotator;
				for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
				{
					if (!VC) continue;
					VC->SetViewLocation(NewCamLoc);
					VC->SetViewRotation(NewCamRot);
					if (bHasCameraSpeed)
					{
						VC->SetCameraSpeedSetting(FMath::Clamp(FMath::RoundToInt(CameraSpeed), 1, 8));
					}
					if (bHasFixedEV100)
					{
						VC->ExposureSettings.bFixed = true;
						VC->ExposureSettings.FixedEV100 = static_cast<float>(FixedEV100);
					}
					if (!ViewModeName.IsEmpty())
					{
						// unlit = 顶点色/设定配色的如实视图（无光照无曝光）；lit = 恢复默认。
						if (ViewModeName.Equals(TEXT("unlit"), ESearchCase::IgnoreCase)) { VC->SetViewMode(VMI_Unlit); }
						else if (ViewModeName.Equals(TEXT("lit"), ESearchCase::IgnoreCase)) { VC->SetViewMode(VMI_Lit); }
						else if (ViewModeName.Equals(TEXT("wireframe"), ESearchCase::IgnoreCase)) { VC->SetViewMode(VMI_Wireframe); }
					}
					VC->Invalidate(true, true);
					if (!bDrove)
					{
						ReadbackLoc = VC->GetViewLocation();
						ReadbackRot = VC->GetViewRotation();
					}
					bDrove = true;
					++UpdatedViewportCount;
				}
				if (!ViewModeName.IsEmpty())
				{
					OutStructured->SetStringField(TEXT("view_mode"), ViewModeName);
				}
				if (bHasFixedEV100)
				{
					OutStructured->SetNumberField(TEXT("fixed_ev100"), FixedEV100);
				}
				OutStructured->SetBoolField(TEXT("camera_set"), bDrove);
				OutStructured->SetNumberField(TEXT("updated_viewport_count"), UpdatedViewportCount);
				OutStructured->SetNumberField(TEXT("cam_x"), NewCamLoc.X);
				OutStructured->SetNumberField(TEXT("cam_y"), NewCamLoc.Y);
				OutStructured->SetNumberField(TEXT("cam_z"), NewCamLoc.Z);
				OutStructured->SetNumberField(TEXT("cam_pitch"), NewCamRot.Pitch);
				OutStructured->SetNumberField(TEXT("cam_yaw"), NewCamRot.Yaw);
				OutStructured->SetNumberField(TEXT("cam_roll"), NewCamRot.Roll);
				if (bHasCameraSpeed)
				{
					OutStructured->SetNumberField(TEXT("camera_speed"), FMath::Clamp(FMath::RoundToInt(CameraSpeed), 1, 8));
				}
				if (!bDrove)
				{
					OutError = TEXT("No level editor viewport client accepted the camera update.");
					return false;
				}
				TSharedRef<FJsonObject> ReadbackLocation = MakeShared<FJsonObject>();
				ReadbackLocation->SetNumberField(TEXT("x"), ReadbackLoc.X);
				ReadbackLocation->SetNumberField(TEXT("y"), ReadbackLoc.Y);
				ReadbackLocation->SetNumberField(TEXT("z"), ReadbackLoc.Z);
				TSharedRef<FJsonObject> ReadbackRotation = MakeShared<FJsonObject>();
				ReadbackRotation->SetNumberField(TEXT("pitch"), ReadbackRot.Pitch);
				ReadbackRotation->SetNumberField(TEXT("yaw"), ReadbackRot.Yaw);
				ReadbackRotation->SetNumberField(TEXT("roll"), ReadbackRot.Roll);
				OutStructured->SetObjectField(TEXT("camera_location_readback"), ReadbackLocation);
				OutStructured->SetObjectField(TEXT("camera_rotation_readback"), ReadbackRotation);
				OutStructured->SetBoolField(TEXT("camera_transform_readback_matches"),
					ReadbackLoc.Equals(NewCamLoc, 0.01) && ReadbackRot.Equals(NewCamRot, 0.01));
			}
			else
			{
				// fall back to old behavior: F-key on selected actors
				TArray<AActor*> SelectedActors;
				for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
				{
					if (AActor* Actor = Cast<AActor>(*It))
						SelectedActors.Add(Actor);
				}
				if (SelectedActors.Num() > 0)
				{
					GEditor->MoveViewportCamerasToActor(SelectedActors, false);
				}
				else
				{
					OutError = TEXT("No actor_id, explicit location, or selected actor was provided for viewport focus.");
					return false;
				}
			}

			// Force a fresh render so the next screenshot sees the new camera
			GEditor->RedrawAllViewports(true);

			OutStructured->SetBoolField(TEXT("focused"), true);
			OutSummary = bHaveExplicitLoc
				? FString::Printf(TEXT("Camera moved to (%.0f, %.0f, %.0f) cm using %s input; transform readback returned."),
					NewCamLoc.X, NewCamLoc.Y, NewCamLoc.Z, *LocationInputShape)
				: (ActorId.IsEmpty() ? TEXT("Focused viewport on selected actors.")
				                     : FString::Printf(TEXT("Focused viewport on actor '%s'."), *ActorId));
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 12. editor_ui_select_actors  —  程序化选择 Actor
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_select_actors"),
		TEXT("Select one or more actors in the level editor by label or path. "
			 "Supports 'add' to add to selection or 'replace' to replace current selection."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("actor_ids"),  FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Actor labels or paths to select."))},
				{TEXT("mode"),       FSololmcpSchemaBuilder::String(
					TEXT("Selection mode: 'replace' (default) or 'add'."),
					{TEXT("replace"), TEXT("add")})}
			},
			{TEXT("actor_ids")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			const TArray<TSharedPtr<FJsonValue>>* ActorIdsArray = nullptr;
			if (!Arguments->TryGetArrayField(TEXT("actor_ids"), ActorIdsArray) || !ActorIdsArray)
			{
				OutError = TEXT("Missing argument: actor_ids");
				return false;
			}

			FString Mode = TEXT("replace");
			Arguments->TryGetStringField(TEXT("mode"), Mode);

			if (Mode == TEXT("replace"))
				GEditor->SelectNone(true, true);

			int32 Selected = 0;
			TArray<TSharedPtr<FJsonValue>> SelectedActors;
			for (const auto& Val : *ActorIdsArray)
			{
				FString ActorId;
				if (!Val.IsValid() || !Val->TryGetString(ActorId)) continue;

				AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
				if (Actor)
				{
					GEditor->SelectActor(Actor, true, true, true, false);
					++Selected;

					TSharedRef<FJsonObject> ActorInfo = MakeShared<FJsonObject>();
					ActorInfo->SetStringField(TEXT("label"), Actor->GetActorLabel());
					ActorInfo->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
					SelectedActors.Add(MakeShared<FJsonValueObject>(ActorInfo));
				}
			}

			GEditor->NoteSelectionChange();
			OutStructured->SetArrayField(TEXT("selected"), SelectedActors);
			OutStructured->SetNumberField(TEXT("count"), Selected);
			OutStructured->SetStringField(TEXT("status"), Selected == ActorIdsArray->Num() ? TEXT("success") : (Selected > 0 ? TEXT("partial_success") : TEXT("failed")));
			OutSummary = FString::Printf(TEXT("Selected %d actor(s) (mode=%s)."), Selected, *Mode);
			if (Selected == 0)
			{
				OutError = TEXT("No requested actors were selected.");
				return false;
			}
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 13. editor_ui_invoke_toolbar_button  —  通过名称点击工具栏按钮
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_invoke_toolbar_button"),
		TEXT("Invoke a specific toolbar button action by command name or alias. "
			 "Common buttons: 'Build.BuildAll' (Build All), 'LevelEditor.Build.BuildGeometry', "
			 "'LevelEditor.Build.BuildLighting', 'LevelEditor.Build.BuildPaths', "
			 "'LevelEditor.Play.Simulate', 'LevelEditor.Play.StopPlaySession'. "
			 "Use 'editor_ui_list_toolbar_buttons' to discover available buttons."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("button"),  FSololmcpSchemaBuilder::String(TEXT("Toolbar button command name or alias."))},
			},
			{TEXT("button")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			FString Button;
			Arguments->TryGetStringField(TEXT("button"), Button);

			// 常见按钮别名映射
			static const TMap<FString, FString> ButtonAliases = {
				{TEXT("build_all"),         TEXT("BuildAll")},
				{TEXT("build_lighting"),    TEXT("BuildLighting")},
				{TEXT("build_geometry"),    TEXT("BuildGeometry")},
				{TEXT("play"),              TEXT("LevelEditor.Play.PlayInViewport")},
				{TEXT("simulate"),          TEXT("LevelEditor.Play.Simulate")},
				{TEXT("stop"),              TEXT("LevelEditor.Play.StopPlaySession")},
				{TEXT("save_all"),          TEXT("MainFrame.SaveAll")},
				{TEXT("save_level"),        TEXT("LevelEditor.Save")},
				{TEXT("new_level"),         TEXT("LevelEditor.NewLevel")},
				{TEXT("open_level"),        TEXT("LevelEditor.OpenLevel")},
				{TEXT("landscape_mode"),    TEXT("LevelEditor.LandscapeMode")},
				{TEXT("foliage_mode"),      TEXT("LevelEditor.FoliageMode")},
				{TEXT("paint_mode"),        TEXT("LevelEditor.MeshPaintMode")},
			};

			if (const FString* Resolved = ButtonAliases.Find(Button.ToLower()))
				Button = *Resolved;

			// 执行 GEditor::Exec
			UWorld* World = GEditor->GetEditorWorldContext().World();
			const bool bExecuted = GEditor->Exec(World, *Button);

			OutStructured->SetStringField(TEXT("button"), Button);
			OutStructured->SetBoolField(TEXT("executed"), bExecuted);
			if (!bExecuted)
			{
				OutError = FString::Printf(TEXT("Toolbar command was not handled: %s"), *Button);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Invoked toolbar button: %s"), *Button);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 14. editor_ui_get_widget_tree  —  获取当前聚焦窗口的 Widget 树（调试用）
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_get_widget_tree"),
		TEXT("Inspect the Slate widget tree of the currently focused editor window. "
			 "Useful for understanding the UI hierarchy to identify interactive elements. "
			 "Returns a flattened list of widgets with type, tag, visibility, and size."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("max_depth"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum depth to traverse. Default: 4."))},
				{TEXT("window"),    FSololmcpSchemaBuilder::String(TEXT("Window to inspect: 'focused' (default), 'main', 'all'."))}
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			int32 MaxDepth = 4;
			FString WindowMode = TEXT("focused");
			Arguments->TryGetNumberField(TEXT("max_depth"), MaxDepth);
			Arguments->TryGetStringField(TEXT("window"),    WindowMode);
			MaxDepth = FMath::Clamp(MaxDepth, 1, 8);

			TArray<TSharedPtr<FJsonValue>> WidgetEntries;

			if (WindowMode == TEXT("focused") || WindowMode == TEXT("main"))
			{
				TSharedPtr<SWindow> Window;
				if (WindowMode == TEXT("focused"))
					Window = FSlateApplication::Get().GetKeyboardFocusedWidget() ?
						FSlateApplication::Get().FindWidgetWindow(FSlateApplication::Get().GetKeyboardFocusedWidget().ToSharedRef()) :
						FSlateApplication::Get().GetActiveTopLevelWindow();
				else
					Window = FSlateApplication::Get().GetActiveTopLevelWindow();

				if (Window.IsValid())
				{
					TSharedRef<FJsonObject> WindowInfo = MakeShared<FJsonObject>();
					WindowInfo->SetStringField(TEXT("title"), Window->GetTitle().ToString());
					WindowInfo->SetStringField(TEXT("type"), Window->GetTypeAsString());

					TArray<TSharedPtr<FJsonValue>> Widgets;
					BuildWidgetTreeSummary(Window, Widgets, 0, MaxDepth);
					WindowInfo->SetArrayField(TEXT("widgets"), Widgets);
					WindowInfo->SetNumberField(TEXT("widget_count"), Widgets.Num());
					WidgetEntries.Add(MakeShared<FJsonValueObject>(WindowInfo));
				}
			}

			OutStructured->SetArrayField(TEXT("windows"), WidgetEntries);
			OutStructured->SetNumberField(TEXT("window_count"), WidgetEntries.Num());
			OutSummary = FString::Printf(TEXT("Captured widget tree from %d window(s)."), WidgetEntries.Num());
			return true;
		},
		nullptr,  // IsAvailable
		1         // CacheTtlSeconds
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 15. editor_ui_simulate_click  —  在指定屏幕坐标模拟鼠标点击（高级/后备）
	//    警告：尽量避免使用此工具，优先使用上面的语义化工具
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_simulate_click"),
		TEXT("Simulate a mouse click at specific screen coordinates in the UE5 editor. "
			 "WARNING: This is a low-level fallback. Prefer semantic tools like 'editor_ui_activate_mode' or 'editor_ui_invoke_toolbar_button'. "
			 "Use 'editor_screenshot_viewport' first to identify coordinates. "
			 "Coordinates are in screen pixels from top-left corner."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("x"),          FSololmcpSchemaBuilder::Number(TEXT("Screen X coordinate (pixels from left)."))},
				{TEXT("y"),          FSololmcpSchemaBuilder::Number(TEXT("Screen Y coordinate (pixels from top)."))},
				{TEXT("button"),     FSololmcpSchemaBuilder::String(TEXT("Mouse button: 'left' (default), 'right', 'middle'."),
					{TEXT("left"), TEXT("right"), TEXT("middle")})},
				{TEXT("double_click"),FSololmcpSchemaBuilder::Boolean(TEXT("Perform double-click. Default: false."))},
				{TEXT("restore_focus"),FSololmcpSchemaBuilder::Boolean(TEXT("Restore focus to original widget after click. Default: true."))}
			},
			{TEXT("x"), TEXT("y")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			double ScreenX = 0.0, ScreenY = 0.0;
			FString ButtonStr = TEXT("left");
			bool bDoubleClick = false;
			bool bRestoreFocus = true;

			Arguments->TryGetNumberField(TEXT("x"),        ScreenX);
			Arguments->TryGetNumberField(TEXT("y"),        ScreenY);
			Arguments->TryGetStringField(TEXT("button"),   ButtonStr);
			Arguments->TryGetBoolField(TEXT("double_click"),  bDoubleClick);
			Arguments->TryGetBoolField(TEXT("restore_focus"), bRestoreFocus);

			const FVector2D ClickPos(static_cast<float>(ScreenX), static_cast<float>(ScreenY));

			// 保存当前焦点
			TSharedPtr<SWidget> PreviousFocusWidget = FSlateApplication::Get().GetKeyboardFocusedWidget();

			// 确定鼠标按键
			EMouseButtons::Type MouseButton = EMouseButtons::Left;
			if (ButtonStr == TEXT("right"))  MouseButton = EMouseButtons::Right;
			if (ButtonStr == TEXT("middle")) MouseButton = EMouseButtons::Middle;

			// Use the explicit Slate user-index constructor. A default FInputDeviceId is invalid
			// in UE 5.7 and can assert in FSlateApplication::RegisterNewUser.
			FPointerEvent MouseDownEvent(
				0,           // Slate user index
				0,           // PointerIndex
				ClickPos,    // ScreenSpacePosition
				ClickPos,    // LastScreenSpacePosition
				TSet<FKey>(), // PressedButtons
				FKey(MouseButton == EMouseButtons::Left ? EKeys::LeftMouseButton :
					 MouseButton == EMouseButtons::Right ? EKeys::RightMouseButton : EKeys::MiddleMouseButton),
				0.0f,        // WheelDelta
				FModifierKeysState()
			);

			FPointerEvent MouseUpEvent(
				0,           // Slate user index
				0,
				ClickPos,
				ClickPos,
				TSet<FKey>(),
				FKey(MouseButton == EMouseButtons::Left ? EKeys::LeftMouseButton :
					 MouseButton == EMouseButtons::Right ? EKeys::RightMouseButton : EKeys::MiddleMouseButton),
				0.0f,
				FModifierKeysState()
			);

			// 执行点击
			bool bDownHandled = FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, MouseDownEvent);
			FPlatformProcess::Sleep(0.05f); // 短暂等待确保事件处理
			bool bUpHandled = FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUpEvent);

			if (bDoubleClick)
			{
				FPlatformProcess::Sleep(0.05f);
				bDownHandled |= FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr, MouseDownEvent);
				FPlatformProcess::Sleep(0.05f);
				bUpHandled |= FSlateApplication::Get().ProcessMouseButtonUpEvent(MouseUpEvent);
			}

			// 恢复焦点
			if (bRestoreFocus && PreviousFocusWidget.IsValid())
			{
				FSlateApplication::Get().SetKeyboardFocus(PreviousFocusWidget, EFocusCause::SetDirectly);
			}

			OutStructured->SetNumberField(TEXT("x"), ScreenX);
			OutStructured->SetNumberField(TEXT("y"), ScreenY);
			OutStructured->SetStringField(TEXT("button"), ButtonStr);
			OutStructured->SetBoolField(TEXT("double_click"), bDoubleClick);
			OutStructured->SetBoolField(TEXT("mouse_down_handled"), bDownHandled);
			OutStructured->SetBoolField(TEXT("mouse_up_handled"), bUpHandled);
			if (!bDownHandled && !bUpHandled)
			{
				OutError = FString::Printf(TEXT("Click at (%.0f, %.0f) was not handled by any Slate widget."), ScreenX, ScreenY);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Simulated %s %s click at (%.0f, %.0f)."),
				bDoubleClick ? TEXT("double") : TEXT("single"),
				*ButtonStr, ScreenX, ScreenY);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 16. editor_ui_simulate_key  —  模拟键盘输入
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_simulate_key"),
		TEXT("Simulate keyboard key press in the UE5 editor. "
			 "Useful for triggering keyboard shortcuts like F (focus), Del (delete), Ctrl+Z (undo). "
			 "Key names follow UE FKey naming: F1-F12, Delete, Enter, Escape, A-Z, Ctrl, Alt, Shift."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("key"),    FSololmcpSchemaBuilder::String(TEXT("Key name: Delete | Enter | Escape | F1-F12 | A-Z | Tab | Space | etc."))},
				{TEXT("ctrl"),   FSololmcpSchemaBuilder::Boolean(TEXT("Hold Ctrl modifier. Default: false."))},
				{TEXT("shift"),  FSololmcpSchemaBuilder::Boolean(TEXT("Hold Shift modifier. Default: false."))},
				{TEXT("alt"),    FSololmcpSchemaBuilder::Boolean(TEXT("Hold Alt modifier. Default: false."))},
			},
			{TEXT("key")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString KeyName;
			bool bCtrl = false, bShift = false, bAlt = false;
			Arguments->TryGetStringField(TEXT("key"),   KeyName);
			Arguments->TryGetBoolField(TEXT("ctrl"),    bCtrl);
			Arguments->TryGetBoolField(TEXT("shift"),   bShift);
			Arguments->TryGetBoolField(TEXT("alt"),     bAlt);

			if (KeyName.IsEmpty())
			{
				OutError = TEXT("Missing argument: key");
				return false;
			}

			// 构建 UE FKey
			FKey Key(*KeyName);  // UE 5.7: Use const TCHAR* constructor
			if (!Key.IsValid())
			{
				OutError = FString::Printf(TEXT("Unknown key: '%s'. Use UE FKey names (e.g. 'Delete', 'F', 'Z')."), *KeyName);
				return false;
			}

			FModifierKeysState Modifiers(bShift, bShift, bCtrl, bCtrl, bAlt, bAlt, false, false, false);

			// UE 5.7: FKeyEvent constructor signature: (FKey, FModifierKeysState, uint32 UserIndex, bool IsRepeat, uint32 CharacterCode, uint32 KeyCode)
			FKeyEvent KeyDownEvent(Key, Modifiers, 0, false, 0, 0);
			FKeyEvent KeyUpEvent(Key, Modifiers, 0, false, 0, 0);

			FSlateApplication::Get().ProcessKeyDownEvent(KeyDownEvent);
			FPlatformProcess::Sleep(0.03f);
			FSlateApplication::Get().ProcessKeyUpEvent(KeyUpEvent);

			OutStructured->SetStringField(TEXT("key"), KeyName);
			OutStructured->SetBoolField(TEXT("ctrl"),  bCtrl);
			OutStructured->SetBoolField(TEXT("shift"), bShift);
			OutStructured->SetBoolField(TEXT("alt"),   bAlt);
			OutSummary = FString::Printf(TEXT("Simulated key press: %s%s%s%s"),
				bCtrl ? TEXT("Ctrl+") : TEXT(""),
				bShift ? TEXT("Shift+") : TEXT(""),
				bAlt ? TEXT("Alt+") : TEXT(""),
				*KeyName);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 17. editor_ui_build_level  —  触发关卡 Build 操作（灯光/几何/路径等）
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_build_level"),
		TEXT("Trigger a level build operation. Supports building lighting, geometry, paths, navigation, or all. "
			 "This is equivalent to clicking the 'Build' menu in the toolbar."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("build_type"), FSololmcpSchemaBuilder::String(
					TEXT("What to build: 'All' | 'Lighting' | 'ReflectionCaptures' | 'Paths' | 'Geometry' | 'Navigation'"),
					{TEXT("All"), TEXT("Lighting"), TEXT("ReflectionCaptures"), TEXT("Paths"), TEXT("Geometry"), TEXT("Navigation")})},
				{TEXT("quality"),    FSololmcpSchemaBuilder::String(
					TEXT("Lighting quality: 'Preview' | 'Medium' | 'High' | 'Production'. Only affects lighting builds."),
					{TEXT("Preview"), TEXT("Medium"), TEXT("High"), TEXT("Production")})}
			},
			{TEXT("build_type")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			FString BuildType = TEXT("All");
			FString Quality = TEXT("Preview");
			Arguments->TryGetStringField(TEXT("build_type"), BuildType);
			Arguments->TryGetStringField(TEXT("quality"),    Quality);

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				OutError = TEXT("Editor world is not available.");
				return false;
			}
			bool bExecuted = false;

			if (BuildType == TEXT("Lighting"))
			{
				// 设置光照质量
				if (Quality == TEXT("Preview"))          GEditor->Exec(World, TEXT("LIGHTMASSIMPORTANCEVOLUME QUALITY=0"));
				else if (Quality == TEXT("Medium"))      GEditor->Exec(World, TEXT("LIGHTMASSIMPORTANCEVOLUME QUALITY=1"));
				else if (Quality == TEXT("High"))        GEditor->Exec(World, TEXT("LIGHTMASSIMPORTANCEVOLUME QUALITY=2"));
				else if (Quality == TEXT("Production"))  GEditor->Exec(World, TEXT("LIGHTMASSIMPORTANCEVOLUME QUALITY=3"));

				bExecuted = GEditor->Exec(World, TEXT("BUILDLIGHTING SWARM=1"));
			}
			else if (BuildType == TEXT("Geometry"))
			{
				bExecuted = GEditor->Exec(World, TEXT("BUILDGEOMETRY"));
			}
			else if (BuildType == TEXT("Paths"))
			{
				bExecuted = GEditor->Exec(World, TEXT("REBUILDNAV"));
			}
			else if (BuildType == TEXT("Navigation"))
			{
				bExecuted = GEditor->Exec(World, TEXT("REBUILDNAV"));
			}
			else if (BuildType == TEXT("ReflectionCaptures"))
			{
				bExecuted = GEditor->Exec(World, TEXT("BUILDREFLECTIONCAPTURES"));
			}
			else // All
			{
				bExecuted = GEditor->Exec(World, TEXT("BUILDALL SWARM=1"));
			}

			OutStructured->SetStringField(TEXT("build_type"), BuildType);
			OutStructured->SetStringField(TEXT("quality"),    Quality);
			OutStructured->SetBoolField(TEXT("executed"), bExecuted);
			if (!bExecuted)
			{
				OutError = FString::Printf(TEXT("Build command was not handled: %s"), *BuildType);
				return false;
			}
			OutSummary = FString::Printf(TEXT("Triggered Build '%s' (quality=%s)."), *BuildType, *Quality);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 18. editor_ui_play_in_editor  —  在编辑器中播放 (PIE)
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_play_in_editor"),
		TEXT("Start, stop, or pause Play In Editor (PIE) session. "
			 "This is equivalent to pressing the Play/Stop/Pause buttons in the toolbar."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("action"), FSololmcpSchemaBuilder::String(
					TEXT("Action: 'play' | 'stop' | 'pause' | 'resume' | 'simulate'"),
					{TEXT("play"), TEXT("stop"), TEXT("pause"), TEXT("resume"), TEXT("simulate")})},
				{TEXT("mode"),   FSololmcpSchemaBuilder::String(
					TEXT("Play mode (for 'play'): 'viewport' | 'new_window' | 'mobile_preview'. Default: 'viewport'."),
					{TEXT("viewport"), TEXT("new_window"), TEXT("mobile_preview")})}
			},
			{TEXT("action")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }

			FString Action, Mode = TEXT("viewport");
			Arguments->TryGetStringField(TEXT("action"), Action);
			Arguments->TryGetStringField(TEXT("mode"),   Mode);

			if (Action == TEXT("play"))
			{
				if (GEditor->PlayWorld)
				{
					OutError = TEXT("PIE is already running.");
					return false;
				}
				FRequestPlaySessionParams PlayParams;
				if (Mode == TEXT("new_window"))
				{
					// 在新窗口中 PIE — 不设置 DestinationSlateViewport
					PlayParams.DestinationSlateViewport.Reset();
				}
				// viewport 是默认行为
				GEditor->RequestPlaySession(PlayParams);
				OutSummary = FString::Printf(TEXT("Started Play In Editor (mode=%s)."), *Mode);
			}
			else if (Action == TEXT("simulate"))
			{
				if (GEditor->PlayWorld)
				{
					OutError = TEXT("PIE/SIE is already running.");
					return false;
				}
				FRequestPlaySessionParams SimParams;
				// UE 5.7: bSimulateInEditor removed - use SessionDestination
				SimParams.SessionDestination = EPlaySessionDestinationType::InProcess;
				GEditor->RequestPlaySession(SimParams);
				OutSummary = TEXT("Started Simulate In Editor.");
			}
			else if (Action == TEXT("stop"))
			{
				if (!GEditor->PlayWorld)
				{
					OutError = TEXT("No active PIE session to stop.");
					return false;
				}
				GEditor->RequestEndPlayMap();
				OutSummary = TEXT("Requested end of PIE session.");
			}
			else if (Action == TEXT("pause"))
			{
				// 暂停/恢复通过 console 命令
				GEditor->Exec(GEditor->GetEditorWorldContext().World(), TEXT("PAUSE"));
				OutSummary = TEXT("Sent PAUSE command to PIE session.");
			}
			else if (Action == TEXT("resume"))
			{
				GEditor->Exec(GEditor->GetEditorWorldContext().World(), TEXT("PAUSE")); // Toggle
				OutSummary = TEXT("Sent resume (PAUSE toggle) to PIE session.");
			}
			else
			{
				OutError = FString::Printf(TEXT("Unknown action: %s. Valid: play | stop | pause | resume | simulate"), *Action);
				return false;
			}

			OutStructured->SetStringField(TEXT("action"), Action);
			OutStructured->SetBoolField(TEXT("is_playing"), GEditor->PlayWorld != nullptr);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 19. editor_ui_content_browser_navigate  —  导航内容浏览器到指定路径
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_content_browser_navigate"),
		TEXT("Navigate the Content Browser to a specific folder path. "
			 "This is equivalent to clicking a folder in the Content Browser tree. "
			 "Also supports syncing to a specific asset."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("path"),       FSololmcpSchemaBuilder::String(TEXT("Content folder path, e.g. /Game/MyFolder."))},
				{TEXT("sync_asset"), FSololmcpSchemaBuilder::String(TEXT("Optional asset path to sync/highlight in the browser."))}
			},
			{TEXT("path")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString FolderPath, SyncAssetPath;
			Arguments->TryGetStringField(TEXT("path"),       FolderPath);
			Arguments->TryGetStringField(TEXT("sync_asset"), SyncAssetPath);

			IContentBrowserSingleton& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();

			if (!SyncAssetPath.IsEmpty())
			{
				UObject* Asset = Context.Services.LoadAsset(SyncAssetPath, OutError);
				if (Asset)
				{
					TArray<UObject*> Assets = {Asset};
					ContentBrowser.SyncBrowserToAssets(Assets);
					OutStructured->SetStringField(TEXT("synced_asset"), SyncAssetPath);
				}
			}
			else
			{
				TArray<FString> Paths = {FolderPath};
				ContentBrowser.SetSelectedPaths(Paths, true);
				OutStructured->SetStringField(TEXT("navigated_path"), FolderPath);
			}

			OutSummary = FString::Printf(TEXT("Content Browser navigated to '%s'."),
				SyncAssetPath.IsEmpty() ? *FolderPath : *SyncAssetPath);
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 20. editor_ui_save_all  —  保存所有未保存的资产和关卡
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_save_all"),
		TEXT("Save all modified assets and the current level. "
			 "Equivalent to Ctrl+Shift+S or File > Save All. "
			 "Use after major editing operations to ensure changes are persisted."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("only_packages"), FSololmcpSchemaBuilder::Boolean(TEXT("Only save packages (assets), not the level map. Default: false."))}
			}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;
			if (!GEditor) { OutError = TEXT("GEditor not available."); return false; }
			if (GEditor->PlayWorld)
			{
				OutError = TEXT("Cannot save all while PIE is running.");
				return false;
			}

			bool bOnlyPackages = false;
			Arguments->TryGetBoolField(TEXT("only_packages"), bOnlyPackages);
			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			if (!bOnlyPackages && !EditorWorld)
			{
				OutError = TEXT("Editor world is not available; map save was not attempted.");
				return false;
			}

			// 使用 FileHelpers 保存
			if (!bOnlyPackages)
			{
				UPackage* WorldPackage = EditorWorld ? EditorWorld->GetPackage() : nullptr;
				const FString WorldPackageName = WorldPackage ? WorldPackage->GetName() : FString();
				const bool bSaveableGameMap =
					WorldPackage &&
					!WorldPackage->HasAnyPackageFlags(PKG_NewlyCreated) &&
					FPackageName::IsValidLongPackageName(WorldPackageName) &&
					WorldPackageName.StartsWith(TEXT("/Game/"));
				OutStructured->SetStringField(TEXT("current_world_path"), WorldPackageName);
				OutStructured->SetBoolField(TEXT("map_saveable"), bSaveableGameMap);
				if (!bSaveableGameMap)
				{
					OutStructured->SetBoolField(TEXT("map_save_skipped"), true);
					OutStructured->SetNumberField(TEXT("packages_processed"), 0);
					OutError = TEXT("Current map is transient or unsaved; save it under /Game first or call editor_ui_save_all with only_packages=true.");
					return false;
				}
			}

			int32 NumSaved = 0;
			if (!bOnlyPackages)
			{
				// 保存关卡 + 所有包
				const bool bSaved = FEditorFileUtils::SaveDirtyPackages(
					/*bPromptUserToSave=*/false,
					/*bSaveMapPackages=*/true,
					/*bSaveContentPackages=*/true,
					/*bFastSave=*/false,
					/*bNotifyNoPackagesSaved=*/false,
					/*bCanBeDeclined=*/false
				);
				NumSaved = bSaved ? 1 : 0;
				OutStructured->SetBoolField(TEXT("saved_map"), !bOnlyPackages);
			}
			else
			{
				const bool bSaved = FEditorFileUtils::SaveDirtyPackages(false, false, true, false, false, false);
				NumSaved = bSaved ? 1 : 0;
			}

			OutStructured->SetNumberField(TEXT("packages_processed"), NumSaved);
			OutSummary = FString::Printf(TEXT("Save All completed (only_packages=%s)."),
				bOnlyPackages ? TEXT("true") : TEXT("false"));
			return true;
		}
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 21. editor_ui_spawn_actor  —  在指定位置生成Actor
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_spawn_actor"),
		TEXT("Spawn an actor in the current level via Python equivalent. Provide class_path and initial transform."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("class_path"), FSololmcpSchemaBuilder::String(TEXT("Path to the class, e.g. '/Script/Engine.PointLight' or '/Game/BP_MyActor.BP_MyActor_C'"))},
				{TEXT("location"), FSololmcpSchemaBuilder::Object({
					{TEXT("x"), FSololmcpSchemaBuilder::Number()},
					{TEXT("y"), FSololmcpSchemaBuilder::Number()},
					{TEXT("z"), FSololmcpSchemaBuilder::Number()}
				})},
				{TEXT("rotation"), FSololmcpSchemaBuilder::Object({
					{TEXT("pitch"), FSololmcpSchemaBuilder::Number()},
					{TEXT("yaw"), FSololmcpSchemaBuilder::Number()},
					{TEXT("roll"), FSololmcpSchemaBuilder::Number()}
				})}
			},
			{TEXT("class_path")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString ClassPath;
			Arguments->TryGetStringField(TEXT("class_path"), ClassPath);
			ClassPath = ClassPath.TrimStartAndEnd();
			if (ClassPath.IsEmpty())
			{
				OutError = TEXT("Missing required argument: class_path");
				return false;
			}
			UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
			if (!ActorClass)
			{
				OutStructured->SetStringField(TEXT("class_path"), ClassPath);
				OutError = FString::Printf(TEXT("Actor class not found or not spawnable: %s"), *ClassPath);
				return false;
			}

			double Lx = 0, Ly = 0, Lz = 0;
			if (const TSharedPtr<FJsonObject>* LocObj = nullptr; Arguments->TryGetObjectField(TEXT("location"), LocObj) && LocObj)
			{
				(*LocObj)->TryGetNumberField(TEXT("x"), Lx);
				(*LocObj)->TryGetNumberField(TEXT("y"), Ly);
				(*LocObj)->TryGetNumberField(TEXT("z"), Lz);
			}

			double Rr = 0, Rp = 0, Ry = 0;
			if (const TSharedPtr<FJsonObject>* RotObj = nullptr; Arguments->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
			{
				(*RotObj)->TryGetNumberField(TEXT("roll"), Rr);
				(*RotObj)->TryGetNumberField(TEXT("pitch"), Rp);
				(*RotObj)->TryGetNumberField(TEXT("yaw"), Ry);
			}

			FString PythonCode = FString::Printf(TEXT(
				"import unreal\n"
				"try:\n"
				"    cls = unreal.load_class(None, '%s')\n"
				"    loc = unreal.Vector(%f, %f, %f)\n"
				"    rot = unreal.Rotator(%f, %f, %f)\n"
				"    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(cls, loc, rot)\n"
				"    if actor:\n"
				"        print('SPAWN_OK:' + actor.get_actor_label())\n"
				"    else:\n"
				"        print('ERR: Spawn returned None')\n"
				"except Exception as e:\n"
				"    print('ERR:' + str(e))\n"
			), *ClassPath, Lx, Ly, Lz, Rr, Rp, Ry);

			TSharedRef<FJsonObject> PythonResult = MakeShared<FJsonObject>();
			FString PythonSummary, PythonErr;
			bool bOk = Context.Services.ExecutePython(PythonCode, TEXT("ExecuteFile"), true, PythonResult, PythonSummary, PythonErr);
			if (bOk && PythonSummary.Contains(TEXT("ERR:")))
			{
				bOk = false;
				PythonErr = PythonSummary;
			}

			OutStructured->SetBoolField(TEXT("success"), bOk);
			OutStructured->SetStringField(TEXT("output"), PythonSummary);
			OutSummary = FString::Printf(TEXT("Attempted to spawn %s at (%f,%f,%f)."), *ClassPath, Lx, Ly, Lz);
			if (!bOk)
			{
				OutError = PythonErr.IsEmpty()
					? FString::Printf(TEXT("Failed to spawn actor class: %s"), *ClassPath)
					: PythonErr;
			}
			return bOk;
		},
	nullptr,
	0,
	nullptr,
	true
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 22. editor_ui_set_actor_properties  —  设置Actor的详细属性
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_ui_set_actor_properties"),
		TEXT("Set arbitrary properties on an actor (e.g. configuring NavAreaClass, bFixedSize, etc.)."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("actor_id"), FSololmcpSchemaBuilder::String(TEXT("Actor Label or Name"))},
				{TEXT("properties"), FSololmcpSchemaBuilder::Object({})}
			},
			{TEXT("actor_id"), TEXT("properties")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString ActorId;
			if (!Arguments->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
			{
				OutError = TEXT("Missing argument: actor_id");
				return false;
			}

			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			Arguments->TryGetObjectField(TEXT("properties"), PropsObj);
			if (!PropsObj || !PropsObj->IsValid())
			{
				OutError = TEXT("Missing properties object.");
				return false;
			}

			AActor* Actor = Context.Services.FindActorByLabelOrName(ActorId, OutError);
			if (!Actor)
			{
				return false;
			}

			const FScopedTransaction Transaction(
				NSLOCTEXT("SOMOLMCP", "EditorUiSetActorProperties", "SOMOLMCP Set Actor Properties"));
			TArray<TSharedPtr<FJsonValue>> PropertyReceipts;
			if (!Context.Services.ApplyPropertiesWithReceipts(
				Actor,
				(*PropsObj).ToSharedRef(),
				PropertyReceipts,
				OutError))
			{
				return false;
			}

			OutStructured = FSololmcpEditorServices::MakeActorReference(Actor);
			OutStructured->SetBoolField(TEXT("success"), true);
			OutStructured->SetArrayField(TEXT("property_receipts"), PropertyReceipts);
			OutStructured->SetNumberField(TEXT("modified_property_count"), PropertyReceipts.Num());
			OutSummary = FString::Printf(
				TEXT("Updated %d reflected actor propert%s on %s."),
				PropertyReceipts.Num(),
				PropertyReceipts.Num() == 1 ? TEXT("y") : TEXT("ies"),
				*Actor->GetActorLabel());
			return true;
		},
	nullptr,
	5,
	nullptr,
	false
	});

	Registry.Register({
		TEXT("umg_authoring_acceptance_contract"),
		TEXT("Read-only fail-closed UMG live-gate contract. Aggregates caller-supplied widget tree, binding, event, preview, and compile diagnostics evidence into an acceptance receipt without mutating assets."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("widget_blueprint_path"), FSololmcpSchemaBuilder::String(TEXT("Target UWidgetBlueprint asset path."))},
				{TEXT("widget_tree_ok"), FSololmcpSchemaBuilder::Boolean(TEXT("True only when widget tree readback exists and matches the requested structure."))},
				{TEXT("binding_ok"), FSololmcpSchemaBuilder::Boolean(TEXT("True only when all required bindings were inspected/read back."))},
				{TEXT("event_ok"), FSololmcpSchemaBuilder::Boolean(TEXT("True only when required event/delegate wiring was inspected/read back."))},
				{TEXT("preview_ok"), FSololmcpSchemaBuilder::Boolean(TEXT("True only when preview/screenshot/runtime evidence exists."))},
				{TEXT("compile_ok"), FSololmcpSchemaBuilder::Boolean(TEXT("True only when UMG/Blueprint compile diagnostics passed."))},
				{TEXT("widget_tree"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional widget tree evidence object."))},
				{TEXT("bindings"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional binding readback evidence object."))},
				{TEXT("events"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional event/delegate readback evidence object."))},
				{TEXT("preview"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional preview/screenshot evidence object."))},
				{TEXT("compile_diagnostics"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Optional compile diagnostics evidence object."))}
			},
			{TEXT("widget_blueprint_path")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& OutError) -> bool
		{
			if (!EnsureGameThread(OutError)) return false;

			FString WidgetBlueprintPath;
			Arguments->TryGetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);

			auto ReadBool = [&Arguments](const TCHAR* FieldName) -> bool
			{
				bool bValue = false;
				return Arguments->TryGetBoolField(FieldName, bValue) && bValue;
			};

			auto AttachOptionalObject = [&Arguments, &OutStructured](const TCHAR* InFieldName, const TCHAR* OutFieldName)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (Arguments->TryGetObjectField(InFieldName, Obj) && Obj && Obj->IsValid())
				{
					OutStructured->SetObjectField(OutFieldName, Obj->ToSharedRef());
				}
			};

			TArray<TSharedPtr<FJsonValue>> Gates;
			TArray<TSharedPtr<FJsonValue>> BlockingReasons;
			auto AddGate = [&Gates, &BlockingReasons](const FString& Name, bool bOk, const FString& RequiredTool, const FString& Detail)
			{
				TSharedRef<FJsonObject> Gate = MakeShared<FJsonObject>();
				Gate->SetStringField(TEXT("name"), Name);
				Gate->SetBoolField(TEXT("ok"), bOk);
				Gate->SetStringField(TEXT("status"), bOk ? TEXT("passed") : TEXT("missing_or_failed"));
				Gate->SetStringField(TEXT("required_tool_or_evidence"), RequiredTool);
				Gate->SetStringField(TEXT("detail"), Detail);
				Gates.Add(MakeShared<FJsonValueObject>(Gate));
				if (!bOk)
				{
					TSharedRef<FJsonObject> Reason = MakeShared<FJsonObject>();
					Reason->SetStringField(TEXT("code"), FString::Printf(TEXT("umg_%s_gate_failed"), *Name));
					Reason->SetStringField(TEXT("detail"), Detail);
					Reason->SetStringField(TEXT("required_tool_or_evidence"), RequiredTool);
					BlockingReasons.Add(MakeShared<FJsonValueObject>(Reason));
				}
			};

			const bool bWidgetTreeOk = ReadBool(TEXT("widget_tree_ok"));
			const bool bBindingOk = ReadBool(TEXT("binding_ok"));
			const bool bEventOk = ReadBool(TEXT("event_ok"));
			const bool bPreviewOk = ReadBool(TEXT("preview_ok"));
			const bool bCompileOk = ReadBool(TEXT("compile_ok"));

			AddGate(TEXT("widget_tree"), bWidgetTreeOk, TEXT("UMG widget tree inspect/readback"), TEXT("Widget hierarchy must be inspected after mutation and match the requested structure."));
			AddGate(TEXT("binding"), bBindingOk, TEXT("UMG binding inspect/readback"), TEXT("Data/view bindings must be inspected and resolved without missing source/member errors."));
			AddGate(TEXT("event"), bEventOk, TEXT("UMG event/delegate inspect/readback"), TEXT("Events/delegates must be inspected and point to expected handlers."));
			AddGate(TEXT("preview"), bPreviewOk, TEXT("UMG preview capture or runtime screenshot"), TEXT("Preview evidence must show the widget rendered without modal blockage or missing content."));
			AddGate(TEXT("compile_diagnostics"), bCompileOk, TEXT("UMG/Blueprint compile diagnostics"), TEXT("Compile diagnostics must pass before production acceptance."));

			const bool bAccepted = bWidgetTreeOk && bBindingOk && bEventOk && bPreviewOk && bCompileOk;
			OutStructured->SetStringField(TEXT("schema"), TEXT("somol.umg.authoring_acceptance_contract.v1"));
			OutStructured->SetStringField(TEXT("widget_blueprint_path"), WidgetBlueprintPath);
			OutStructured->SetStringField(TEXT("asset_path"), WidgetBlueprintPath);
			OutStructured->SetBoolField(TEXT("accepted"), bAccepted);
			OutStructured->SetBoolField(TEXT("receipt_gate_complete"), bAccepted);
			OutStructured->SetStringField(TEXT("receipt_gate_status"), bAccepted ? TEXT("accepted") : TEXT("failed_validation"));
			OutStructured->SetStringField(TEXT("failure_route"), bAccepted ? TEXT("none") : TEXT("qa_inspector_and_hermes"));
			OutStructured->SetStringField(TEXT("watchdog_required_tool_on_timeout"), TEXT("editor_dialog_watchdog_tick"));
			OutStructured->SetStringField(TEXT("safe_dialog_policy"), TEXT("Only Cancel, Close, No, Don't Save, and Skip Recovery are unattended-safe; Save/Yes/Continue/Run in Editor/Delete/Overwrite fail closed."));
			OutStructured->SetArrayField(TEXT("gates"), Gates);
			OutStructured->SetArrayField(TEXT("blocking_reasons"), BlockingReasons);
			AttachOptionalObject(TEXT("widget_tree"), TEXT("widget_tree_evidence"));
			AttachOptionalObject(TEXT("bindings"), TEXT("binding_evidence"));
			AttachOptionalObject(TEXT("events"), TEXT("event_evidence"));
			AttachOptionalObject(TEXT("preview"), TEXT("preview_evidence"));
			AttachOptionalObject(TEXT("compile_diagnostics"), TEXT("compile_diagnostics_evidence"));
			if (!bAccepted)
			{
				TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
				Failure->SetStringField(TEXT("schema"), TEXT("somol.umg.structured_failure.v1"));
				Failure->SetStringField(TEXT("code"), TEXT("UMG_AUTHORING_GATE_FAILED"));
				Failure->SetStringField(TEXT("failed_gate"), TEXT("widget_tree_binding_event_preview_compile"));
				Failure->SetStringField(TEXT("message"), TEXT("UMG authoring evidence is incomplete or failed; do not mark the asset delivered."));
				Failure->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_hermes"));
				OutStructured->SetObjectField(TEXT("structured_failure"), Failure);
			}

			OutSummary = bAccepted
				? FString::Printf(TEXT("UMG acceptance contract passed for '%s'."), *WidgetBlueprintPath)
				: FString::Printf(TEXT("UMG acceptance contract failed closed for '%s'."), *WidgetBlueprintPath);
			return true;
		},
		nullptr,
		1
	});

	UE_LOG(LogSOMOLMCPEditorUI, Log, TEXT("Registered 27 Editor UI automation tools."));
}

} // namespace UE::SOMOLMCP
