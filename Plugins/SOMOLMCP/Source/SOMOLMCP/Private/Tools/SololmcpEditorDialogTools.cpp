// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpEditorDialogTools.cpp
// H2 external UE MCP uplift: first-batch modal dialog control contracts.
//
// This file intentionally keeps responses fail-safe: list is read-only Slate
// observation; respond resolves live windows and can only dispatch the safe
// close path. Button clicks remain planned/receipt-only until separately
// verified against Slate.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace UE::SOMOLMCP
{
namespace
{
	struct FEditorDialogPolicyState
	{
		FString Mode = TEXT("observe_only");
		FString DefaultAction = TEXT("none");
		int32 TimeoutMs = 0;
		bool bEnabled = false;
		TArray<FString> AllowTitleSubstrings;
		TArray<FString> DenyTitleSubstrings;
		FString UpdatedAtUtc;
	};

	struct FDialogButtonCandidate
	{
		FString Text;
		FString WidgetType;
		FString Tag;
		int32 Depth = 0;
		bool bHasGeometry = false;
		double X = 0.0;
		double Y = 0.0;
		double W = 0.0;
		double H = 0.0;
		double CenterX = 0.0;
		double CenterY = 0.0;
	};

	struct FButtonSafetyClassification
	{
		FString Status = TEXT("unknown_not_allowed");
		FString SafeAction;
		FString Reason = TEXT("Button text is not in the unattended safe allow-list.");
		bool bUnattendedAllowed = false;
		bool bForbidden = false;
	};

	static FCriticalSection GEditorDialogPolicyMutex;
	static FEditorDialogPolicyState GEditorDialogPolicy;

	static FString UtcNowIso()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	static TArray<FString> ReadStringArrayField(
		const TSharedRef<FJsonObject>& Arguments,
		const TCHAR* FieldName)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Arguments->TryGetArrayField(FieldName, Values) || !Values)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->IsNull())
			{
				continue;
			}
			FString Item = Value->AsString().TrimStartAndEnd();
			if (!Item.IsEmpty())
			{
				Result.Add(Item);
			}
		}
		return Result;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static TSharedRef<FJsonObject> MakePolicyJson(const FEditorDialogPolicyState& Policy)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("enabled"), Policy.bEnabled);
		Json->SetStringField(TEXT("mode"), Policy.Mode);
		Json->SetStringField(TEXT("default_action"), Policy.DefaultAction);
		Json->SetNumberField(TEXT("timeout_ms"), Policy.TimeoutMs);
		Json->SetArrayField(TEXT("allow_title_substrings"), StringArrayToJson(Policy.AllowTitleSubstrings));
		Json->SetArrayField(TEXT("deny_title_substrings"), StringArrayToJson(Policy.DenyTitleSubstrings));
		Json->SetStringField(TEXT("updated_at_utc"), Policy.UpdatedAtUtc);
		return Json;
	}

	static void SetContractFields(TSharedRef<FJsonObject>& OutStructured)
	{
		OutStructured->SetStringField(TEXT("contract_version"), TEXT("h2.dialog.v1"));
		OutStructured->SetStringField(TEXT("implementation_level"), TEXT("slate_list_plus_safe_close_and_button_click_dispatch"));
		OutStructured->SetStringField(
			TEXT("slate_implementation_note"),
			TEXT("editor_dialog_list now enumerates visible Slate top-level/modal windows. "
			     "editor_dialog_respond resolves live dialogs, may close a matched window, and can dispatch verified Slate button clicks by resolved SButton geometry."));
	}

	static FString NormalizeDialogText(const FString& In)
	{
		FString Out = In;
		Out.TrimStartAndEndInline();
		Out.ReplaceInline(TEXT("\r"), TEXT(" "));
		Out.ReplaceInline(TEXT("\n"), TEXT(" "));
		while (Out.Contains(TEXT("  ")))
		{
			Out.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		return Out;
	}

	static FString MakeDialogId(const TSharedRef<SWindow>& Window, int32 Index)
	{
		const FString StableText = FString::Printf(
			TEXT("%s|%s|%d"),
			*NormalizeDialogText(Window->GetTitle().ToString()),
			*Window->GetTypeAsString(),
			Index);
		return FString::Printf(TEXT("dlg_%08x_%d"), FCrc::StrCrc32(*StableText), Index);
	}

	static TArray<TSharedRef<SWindow>> EnumerateVisibleSlateWindows();
	static FButtonSafetyClassification ClassifyButtonSafetyLabel(const FString& Label);
	static TSharedRef<FJsonObject> MakeDialogClassificationJson(
		const FString& DialogTitle,
		const TArray<FDialogButtonCandidate>& Buttons,
		bool bIsModal = false,
		bool bIsRegularWindow = false,
		bool bLooksLikeNonBlockingNotification = false,
		bool bLooksLikeNonBlockingUtilityWindow = false,
		bool bHasDialogWindow = true);
	static TSharedRef<FJsonObject> MakeSafeActionReceiptJson(
		const FString& DialogTitle,
		const TArray<FDialogButtonCandidate>& Buttons,
		const FString& ChosenAction,
		int32 MatchedButtonIndex,
		bool bDryRun,
		bool bApplied,
		bool bButtonClickAttempted,
		const FString& Status,
		const FString& Reason,
		bool bIsModal = false,
		bool bIsRegularWindow = false,
		bool bLooksLikeNonBlockingNotification = false,
		bool bLooksLikeNonBlockingUtilityWindow = false,
		bool bHasDialogWindow = true);

	static FString ExtractTextFromWidgetTree(
		TSharedPtr<SWidget> Widget,
		int32 Depth,
		int32 MaxDepth)
	{
		if (!Widget.IsValid() || Depth > MaxDepth)
		{
			return FString();
		}

		if (Widget->GetTypeAsString() == TEXT("STextBlock"))
		{
			const TSharedPtr<STextBlock> TextBlock = StaticCastSharedPtr<STextBlock>(Widget);
			if (TextBlock.IsValid())
			{
				return NormalizeDialogText(TextBlock->GetText().ToString());
			}
		}

		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
			{
				FString ChildText = ExtractTextFromWidgetTree(Children->GetChildAt(ChildIndex), Depth + 1, MaxDepth);
				if (!ChildText.IsEmpty())
				{
					return ChildText;
				}
			}
		}
		return FString();
	}

	static void CollectButtonCandidates(
		TSharedPtr<SWidget> Widget,
		TArray<FDialogButtonCandidate>& OutButtons,
		int32 Depth,
		int32 MaxDepth)
	{
		if (!Widget.IsValid() || Depth > MaxDepth || OutButtons.Num() >= 48)
		{
			return;
		}

		if (Widget->GetTypeAsString() == TEXT("SButton"))
		{
			FDialogButtonCandidate Candidate;
			Candidate.Text = ExtractTextFromWidgetTree(Widget, Depth, Depth + 4);
			Candidate.WidgetType = Widget->GetTypeAsString();
			Candidate.Tag = Widget->GetTag().IsNone() ? TEXT("") : Widget->GetTag().ToString();
			Candidate.Depth = Depth;

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
			OutButtons.Add(Candidate);
		}

		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 ChildIndex = 0; ChildIndex < Children->Num() && ChildIndex < 64; ++ChildIndex)
			{
				CollectButtonCandidates(Children->GetChildAt(ChildIndex), OutButtons, Depth + 1, MaxDepth);
			}
		}
	}

	static void WidgetTreeFindDialogHints(
		TSharedPtr<SWidget> Widget,
		int32 Depth,
		int32 MaxDepth,
		bool& bSawTextBlock,
		bool& bSawButton,
		bool& bSawMenu)
	{
		if (!Widget.IsValid() || Depth > MaxDepth)
		{
			return;
		}

		const FString Type = Widget->GetTypeAsString();
		if (Type == TEXT("STextBlock"))
		{
			bSawTextBlock = true;
		}
		if (Type == TEXT("SButton")
			|| Type == TEXT("SCheckBox")
			|| Type == TEXT("SEditableText")
			|| Type == TEXT("SEditableTextBox")
			|| Type.Contains(TEXT("SCombo"), ESearchCase::IgnoreCase))
		{
			bSawButton = true;
		}
		if (Type.Contains(TEXT("SMenu"), ESearchCase::IgnoreCase)
			|| Type.Contains(TEXT("SListView"), ESearchCase::IgnoreCase)
			|| Type.Contains(TEXT("STreeView"), ESearchCase::IgnoreCase)
			|| Type.Contains(TEXT("STableView"), ESearchCase::IgnoreCase))
		{
			bSawMenu = true;
		}

		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 ChildIndex = 0; ChildIndex < Children->Num() && ChildIndex < 64; ++ChildIndex)
			{
				WidgetTreeFindDialogHints(
					Children->GetChildAt(ChildIndex),
					Depth + 1,
					MaxDepth,
					bSawTextBlock,
					bSawButton,
					bSawMenu);
			}
		}
	}

	static TArray<TSharedPtr<FJsonValue>> ButtonCandidatesToJson(const TArray<FDialogButtonCandidate>& Buttons)
	{
		TArray<TSharedPtr<FJsonValue>> JsonButtons;
		for (const FDialogButtonCandidate& Button : Buttons)
		{
			const FButtonSafetyClassification Safety = ClassifyButtonSafetyLabel(Button.Text);
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("text"), Button.Text);
			Json->SetStringField(TEXT("widget_type"), Button.WidgetType);
			Json->SetStringField(TEXT("tag"), Button.Tag);
			Json->SetNumberField(TEXT("depth"), Button.Depth);
			Json->SetBoolField(TEXT("has_geometry"), Button.bHasGeometry);
			Json->SetStringField(TEXT("button_safety"), Safety.Status);
			Json->SetStringField(TEXT("safe_action"), Safety.SafeAction);
			Json->SetStringField(TEXT("safety_reason"), Safety.Reason);
			Json->SetBoolField(TEXT("unattended_allowed"), Safety.bUnattendedAllowed);
			Json->SetBoolField(TEXT("forbidden_unattended"), Safety.bForbidden);
			TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
			Bounds->SetNumberField(TEXT("x"), FMath::RoundToInt(Button.X));
			Bounds->SetNumberField(TEXT("y"), FMath::RoundToInt(Button.Y));
			Bounds->SetNumberField(TEXT("w"), FMath::RoundToInt(Button.W));
			Bounds->SetNumberField(TEXT("h"), FMath::RoundToInt(Button.H));
			Bounds->SetNumberField(TEXT("center_x"), FMath::RoundToInt(Button.CenterX));
			Bounds->SetNumberField(TEXT("center_y"), FMath::RoundToInt(Button.CenterY));
			Json->SetObjectField(TEXT("screen_bounds"), Bounds);
			JsonButtons.Add(MakeShared<FJsonValueObject>(Json));
		}
		return JsonButtons;
	}

	static bool TextMatchesLoose(const FString& Candidate, const FString& Wanted)
	{
		const FString Left = NormalizeDialogText(Candidate);
		const FString Right = NormalizeDialogText(Wanted);
		if (Left.IsEmpty() || Right.IsEmpty())
		{
			return false;
		}
		return Left.Equals(Right, ESearchCase::IgnoreCase)
			|| Left.Contains(Right, ESearchCase::IgnoreCase)
			|| Right.Contains(Left, ESearchCase::IgnoreCase);
	}

	static const TArray<FString>& CancelButtonAliases()
	{
		static const TArray<FString> Aliases = {
			TEXT("取消"),
			TEXT("Cancel"),
			TEXT("关闭"),
			TEXT("Close"),
			TEXT("否"),
			TEXT("No"),
			TEXT("不保存"),
			TEXT("不要保存"),
			TEXT("Don't Save"),
			TEXT("Do Not Save"),
			TEXT("放弃"),
			TEXT("Abort")
		};
		return Aliases;
	}

	static const TArray<FString>& CloseButtonAliases()
	{
		static const TArray<FString> Aliases = {
			TEXT("Close")
		};
		return Aliases;
	}

	static const TArray<FString>& NoButtonAliases()
	{
		static const TArray<FString> Aliases = {
			TEXT("No"),
			TEXT("No to All")
		};
		return Aliases;
	}

	static const TArray<FString>& DontSaveButtonAliases()
	{
		static const TArray<FString> Aliases = {
			TEXT("Don't Save"),
			TEXT("Do Not Save")
		};
		return Aliases;
	}

	static const TArray<FString>& SkipRecoveryButtonAliases()
	{
		static const TArray<FString> Aliases = {
			TEXT("Skip Recovery"),
			TEXT("Skip Package Recovery")
		};
		return Aliases;
	}

	static const TArray<FString>& AcceptButtonAliases()
	{
		static const TArray<FString> Aliases = {
			TEXT("确定"),
			TEXT("OK"),
			TEXT("是"),
			TEXT("Yes"),
			TEXT("继续"),
			TEXT("Continue"),
			TEXT("编辑器中运行"),
			TEXT("Run in Editor"),
			TEXT("运行"),
			TEXT("Run")
		};
		return Aliases;
	}

	static const TArray<FString>& DangerousUnattendedButtonAliases()
	{
		static const TArray<FString> Aliases = {
			TEXT("Run in Editor"),
			TEXT("编辑器中运行"),
			TEXT("运行"),
			TEXT("Continue"),
			TEXT("继续"),
			TEXT("Yes"),
			TEXT("是"),
			TEXT("Save"),
			TEXT("保存"),
			TEXT("Save All"),
			TEXT("全部保存"),
			TEXT("Delete"),
			TEXT("删除"),
			TEXT("Overwrite"),
			TEXT("覆盖"),
			TEXT("Recover Selected"),
			TEXT("Recover All"),
			TEXT("Recover"),
			TEXT("Restore Selected"),
			TEXT("Restore All"),
			TEXT("Restore"),
			TEXT("Force Delete"),
			TEXT("强制删除")
		};
		return Aliases;
	}

	static bool ButtonMatchesAnyAlias(
		const FDialogButtonCandidate& Button,
		const TArray<FString>& Aliases,
		FString& OutMatchedAlias)
	{
		for (const FString& Alias : Aliases)
		{
			if (TextMatchesLoose(Button.Text, Alias))
			{
				OutMatchedAlias = Alias;
				return true;
			}
		}
		return false;
	}

	static bool LabelMatchesAnyAlias(
		const FString& Label,
		const TArray<FString>& Aliases,
		FString& OutMatchedAlias)
	{
		for (const FString& Alias : Aliases)
		{
			if (TextMatchesLoose(Label, Alias))
			{
				OutMatchedAlias = Alias;
				return true;
			}
		}
		return false;
	}

	static FButtonSafetyClassification ClassifyButtonSafetyLabel(const FString& Label)
	{
		FButtonSafetyClassification Safety;
		const FString Normalized = NormalizeDialogText(Label);
		if (Normalized.IsEmpty())
		{
			Safety.Status = TEXT("unknown_empty_label");
			Safety.Reason = TEXT("Button has no readable text.");
			return Safety;
		}

		FString MatchedAlias;
		if (LabelMatchesAnyAlias(Normalized, SkipRecoveryButtonAliases(), MatchedAlias))
		{
			Safety.Status = TEXT("safe_allowlisted");
			Safety.SafeAction = TEXT("skip_recovery");
			Safety.Reason = FString::Printf(TEXT("Matched safe startup recovery alias: %s"), *MatchedAlias);
			Safety.bUnattendedAllowed = true;
			return Safety;
		}
		if (LabelMatchesAnyAlias(Normalized, DontSaveButtonAliases(), MatchedAlias))
		{
			Safety.Status = TEXT("safe_allowlisted");
			Safety.SafeAction = TEXT("dont_save");
			Safety.Reason = FString::Printf(TEXT("Matched safe discard alias: %s"), *MatchedAlias);
			Safety.bUnattendedAllowed = true;
			return Safety;
		}
		if (LabelMatchesAnyAlias(Normalized, CloseButtonAliases(), MatchedAlias))
		{
			Safety.Status = TEXT("safe_allowlisted");
			Safety.SafeAction = TEXT("close");
			Safety.Reason = FString::Printf(TEXT("Matched safe close alias: %s"), *MatchedAlias);
			Safety.bUnattendedAllowed = true;
			return Safety;
		}
		if (LabelMatchesAnyAlias(Normalized, NoButtonAliases(), MatchedAlias))
		{
			Safety.Status = TEXT("safe_allowlisted");
			Safety.SafeAction = TEXT("no");
			Safety.Reason = FString::Printf(TEXT("Matched safe no alias: %s"), *MatchedAlias);
			Safety.bUnattendedAllowed = true;
			return Safety;
		}
		if (LabelMatchesAnyAlias(Normalized, CancelButtonAliases(), MatchedAlias))
		{
			Safety.Status = TEXT("safe_allowlisted");
			Safety.SafeAction = TEXT("cancel");
			Safety.Reason = FString::Printf(TEXT("Matched safe cancel alias: %s"), *MatchedAlias);
			Safety.bUnattendedAllowed = true;
			return Safety;
		}
		if (LabelMatchesAnyAlias(Normalized, DangerousUnattendedButtonAliases(), MatchedAlias))
		{
			Safety.Status = TEXT("dangerous_refused");
			Safety.Reason = FString::Printf(TEXT("Forbidden unattended button alias: %s"), *MatchedAlias);
			Safety.bForbidden = true;
			return Safety;
		}
		return Safety;
	}

	static bool DialogLooksLikeCompileError(const FString& Title)
	{
		return Title.Contains(TEXT("Compile"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Blueprint"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("UMG"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Widget"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Error"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("编译"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("蓝图"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("控件"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("错误"), ESearchCase::IgnoreCase);
	}

	static FString ClassifyDialogTitle(const FString& Title)
	{
		if (Title.Contains(TEXT("Recover Packages"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Restore Packages"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Package Recovery"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Recovery"), ESearchCase::IgnoreCase))
		{
			return TEXT("recover_packages");
		}
		if (Title.Contains(TEXT("Save Content"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Save Assets"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Dirty"), ESearchCase::IgnoreCase))
		{
			return TEXT("dirty_package_save_prompt");
		}
		if (DialogLooksLikeCompileError(Title))
		{
			return TEXT("compile_or_validation_error");
		}
		if (Title.Contains(TEXT("Run in Editor"), ESearchCase::IgnoreCase)
			|| Title.Contains(TEXT("Play"), ESearchCase::IgnoreCase))
		{
			return TEXT("editor_run_or_pie_prompt");
		}
		return TEXT("unknown_modal");
	}

	static TSharedRef<FJsonObject> MakeDialogClassificationJson(
		const FString& DialogTitle,
		const TArray<FDialogButtonCandidate>& Buttons,
		bool bIsModal,
		bool bIsRegularWindow,
		bool bLooksLikeNonBlockingNotification,
		bool bLooksLikeNonBlockingUtilityWindow,
		bool bHasDialogWindow)
	{
		TArray<TSharedPtr<FJsonValue>> SafeLabels;
		TArray<TSharedPtr<FJsonValue>> ForbiddenLabels;
		TArray<TSharedPtr<FJsonValue>> UnknownLabels;
		for (const FDialogButtonCandidate& Button : Buttons)
		{
			const FButtonSafetyClassification Safety = ClassifyButtonSafetyLabel(Button.Text);
			if (Safety.bUnattendedAllowed)
			{
				SafeLabels.Add(MakeShared<FJsonValueString>(Button.Text));
			}
			else if (Safety.bForbidden)
			{
				ForbiddenLabels.Add(MakeShared<FJsonValueString>(Button.Text));
			}
			else if (!Button.Text.IsEmpty())
			{
				UnknownLabels.Add(MakeShared<FJsonValueString>(Button.Text));
			}
		}

		const bool bIsNonBlockingWindow =
			bLooksLikeNonBlockingNotification || bLooksLikeNonBlockingUtilityWindow;
		const FString Category = !bHasDialogWindow
			? TEXT("no_dialog_detected")
			: bLooksLikeNonBlockingNotification
			? TEXT("non_blocking_notification")
			: bLooksLikeNonBlockingUtilityWindow
			? TEXT("non_blocking_utility_window")
			: ClassifyDialogTitle(DialogTitle);
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("category"), Category);
		Json->SetBoolField(TEXT("is_recover_packages_dialog"), Category == TEXT("recover_packages"));
		Json->SetBoolField(TEXT("is_dirty_package_save_prompt"), Category == TEXT("dirty_package_save_prompt"));
		Json->SetBoolField(TEXT("is_compile_or_validation_error"), Category == TEXT("compile_or_validation_error"));
		Json->SetBoolField(TEXT("is_non_blocking_notification"), bLooksLikeNonBlockingNotification);
		Json->SetBoolField(TEXT("is_non_blocking_utility_window"), bLooksLikeNonBlockingUtilityWindow);
		Json->SetBoolField(TEXT("has_dialog_window"), bHasDialogWindow);
		Json->SetBoolField(TEXT("blocks_unattended_lane"), bHasDialogWindow && !bIsNonBlockingWindow);
		Json->SetBoolField(TEXT("is_modal_window"), bIsModal);
		Json->SetBoolField(TEXT("is_regular_window"), bIsRegularWindow);
		Json->SetBoolField(TEXT("unknown_button_policy_fail_closed"), bHasDialogWindow && !bIsNonBlockingWindow);
		Json->SetArrayField(TEXT("safe_button_labels"), SafeLabels);
		Json->SetArrayField(TEXT("forbidden_button_labels"), ForbiddenLabels);
		Json->SetArrayField(TEXT("unknown_button_labels"), UnknownLabels);
		Json->SetNumberField(TEXT("safe_button_count"), SafeLabels.Num());
		Json->SetNumberField(TEXT("forbidden_button_count"), ForbiddenLabels.Num());
		Json->SetNumberField(TEXT("unknown_button_count"), UnknownLabels.Num());
		Json->SetBoolField(TEXT("forbidden_button_seen"), ForbiddenLabels.Num() > 0);
		return Json;
	}

	static TSharedRef<FJsonObject> MakeSafeActionReceiptJson(
		const FString& DialogTitle,
		const TArray<FDialogButtonCandidate>& Buttons,
		const FString& ChosenAction,
		int32 MatchedButtonIndex,
		bool bDryRun,
		bool bApplied,
		bool bButtonClickAttempted,
		const FString& Status,
		const FString& Reason,
		bool bIsModal,
		bool bIsRegularWindow,
		bool bLooksLikeNonBlockingNotification,
		bool bLooksLikeNonBlockingUtilityWindow,
		bool bHasDialogWindow)
	{
		TSharedRef<FJsonObject> Receipt = MakeDialogClassificationJson(
			DialogTitle,
			Buttons,
			bIsModal,
			bIsRegularWindow,
			bLooksLikeNonBlockingNotification,
			bLooksLikeNonBlockingUtilityWindow,
			bHasDialogWindow);
		const bool bIsNonBlockingWindow =
			bLooksLikeNonBlockingNotification || bLooksLikeNonBlockingUtilityWindow;
		Receipt->SetStringField(TEXT("policy"), TEXT("allowlist_only_fail_closed"));
		Receipt->SetStringField(
			TEXT("dialog_category"),
			!bHasDialogWindow
				? TEXT("no_dialog_detected")
				: (bLooksLikeNonBlockingNotification
					? TEXT("non_blocking_notification")
					: (bLooksLikeNonBlockingUtilityWindow
						? TEXT("non_blocking_utility_window")
						: ClassifyDialogTitle(DialogTitle))));
		Receipt->SetStringField(TEXT("chosen_action"), ChosenAction);
		Receipt->SetNumberField(TEXT("matched_button_index"), MatchedButtonIndex);
		Receipt->SetBoolField(TEXT("dry_run"), bDryRun);
		Receipt->SetBoolField(TEXT("applied"), bApplied);
		Receipt->SetBoolField(TEXT("button_click_attempted"), bButtonClickAttempted);
		Receipt->SetStringField(TEXT("status"), Status);
		Receipt->SetStringField(TEXT("reason"), Reason);
		const bool bBlockedModalOrNoResponse =
			bHasDialogWindow
			&& !bIsNonBlockingWindow
			&& !bApplied
			&& Status != TEXT("no_blocking_dialog")
			&& Status != TEXT("dialog_not_found");
		Receipt->SetBoolField(TEXT("blocked_modal_or_mcp_no_response"), bBlockedModalOrNoResponse);
		if (bBlockedModalOrNoResponse)
		{
			Receipt->SetStringField(TEXT("blocked_code"), TEXT("blocked_modal_or_mcp_no_response"));
			Receipt->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_hermes"));
		}
		Receipt->SetBoolField(TEXT("unknown_buttons_fail_closed"), true);
		Receipt->SetArrayField(TEXT("never_unattended_button_texts"), StringArrayToJson(DangerousUnattendedButtonAliases()));
		const TArray<FString> SafeUnattendedButtonTexts = {
			TEXT("Cancel"),
			TEXT("Close"),
			TEXT("No"),
			TEXT("Don't Save"),
			TEXT("Skip Recovery")
		};
		Receipt->SetArrayField(TEXT("safe_unattended_button_texts"), StringArrayToJson(SafeUnattendedButtonTexts));
		return Receipt;
	}

	static int32 ChooseSafeDialogButtonIndex(
		const FString& DialogTitle,
		const FString& Context,
		const TArray<FDialogButtonCandidate>& Buttons,
		FString& OutAction,
		FString& OutReason)
	{
		const bool bCompileContext = DialogLooksLikeCompileError(DialogTitle)
			|| Context.Contains(TEXT("compile"), ESearchCase::IgnoreCase)
			|| Context.Contains(TEXT("pie"), ESearchCase::IgnoreCase)
			|| Context.Contains(TEXT("umg"), ESearchCase::IgnoreCase)
			|| Context.Contains(TEXT("blueprint"), ESearchCase::IgnoreCase);

		for (int32 ButtonIndex = 0; ButtonIndex < Buttons.Num(); ++ButtonIndex)
		{
			const FButtonSafetyClassification Safety = ClassifyButtonSafetyLabel(Buttons[ButtonIndex].Text);
			if (bCompileContext && Safety.bForbidden)
			{
				continue;
			}

			if (Safety.bUnattendedAllowed)
			{
				OutAction = Safety.SafeAction;
				OutReason = Safety.Reason;
				return ButtonIndex;
			}
		}

		for (int32 ButtonIndex = 0; ButtonIndex < Buttons.Num(); ++ButtonIndex)
		{
			const FButtonSafetyClassification Safety = ClassifyButtonSafetyLabel(Buttons[ButtonIndex].Text);
			if (Safety.bForbidden)
			{
				OutReason = FString::Printf(TEXT("only_dangerous_or_unknown_buttons; %s"), *Safety.Reason);
				return INDEX_NONE;
			}
		}

		OutReason = TEXT("no_safe_button_alias_match");
		return INDEX_NONE;
	}

	static int32 FindDialogButtonIndex(
		const FString& Action,
		const FString& ButtonText,
		const TArray<FDialogButtonCandidate>& Buttons,
		FString& OutReason)
	{
		auto MatchAnyAlias = [&Buttons](const TArray<FString>& Aliases) -> int32
		{
			for (int32 ButtonIndex = 0; ButtonIndex < Buttons.Num(); ++ButtonIndex)
			{
				for (const FString& Alias : Aliases)
				{
					if (TextMatchesLoose(Buttons[ButtonIndex].Text, Alias))
					{
						return ButtonIndex;
					}
				}
			}
			return INDEX_NONE;
		};

		if (Action == TEXT("button_text"))
		{
			for (int32 ButtonIndex = 0; ButtonIndex < Buttons.Num(); ++ButtonIndex)
			{
				if (TextMatchesLoose(Buttons[ButtonIndex].Text, ButtonText))
				{
					OutReason = FString::Printf(TEXT("matched_button_text:%s"), *ButtonText);
					return ButtonIndex;
				}
			}
			OutReason = FString::Printf(TEXT("no_button_text_match:%s"), *ButtonText);
			return INDEX_NONE;
		}

		if (Action == TEXT("cancel"))
		{
			const int32 Index = MatchAnyAlias(CancelButtonAliases());
			OutReason = Index == INDEX_NONE ? TEXT("no_cancel_alias_match") : TEXT("matched_cancel_alias");
			return Index;
		}

		if (Action == TEXT("accept"))
		{
			const int32 Index = MatchAnyAlias(AcceptButtonAliases());
			OutReason = Index == INDEX_NONE ? TEXT("no_accept_alias_match") : TEXT("matched_accept_alias");
			return Index;
		}

		if (Action == TEXT("no") || Action == TEXT("dont_save") || Action == TEXT("skip_recovery"))
		{
			for (int32 ButtonIndex = 0; ButtonIndex < Buttons.Num(); ++ButtonIndex)
			{
				const FButtonSafetyClassification Safety = ClassifyButtonSafetyLabel(Buttons[ButtonIndex].Text);
				if (Safety.SafeAction == Action)
				{
					OutReason = FString::Printf(TEXT("matched_safe_action:%s"), *Action);
					return ButtonIndex;
				}
			}
			OutReason = FString::Printf(TEXT("no_safe_action_match:%s"), *Action);
			return INDEX_NONE;
		}

		OutReason = FString::Printf(TEXT("action_has_no_button_mapping:%s"), *Action);
		return INDEX_NONE;
	}

	static TArray<TSharedPtr<FJsonValue>> CollectDirtyPackageJson(
		int32 MaxPackages,
		bool bIncludeTransient,
		int32& OutDirtyCount,
		int32& OutReturnedCount,
		int32& OutOmittedCount,
		int32& OutBlockingCount)
	{
		TArray<TSharedPtr<FJsonValue>> PackagesJson;
		OutDirtyCount = 0;
		OutReturnedCount = 0;
		OutOmittedCount = 0;
		OutBlockingCount = 0;

		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Pkg = *It;
			if (!Pkg || !Pkg->IsDirty())
			{
				continue;
			}

			++OutDirtyCount;
			const FString PackageName = Pkg->GetName();
			const bool bIsScriptPackage = PackageName.StartsWith(TEXT("/Script/"));
			const bool bIsEnginePackage = PackageName.StartsWith(TEXT("/Engine/"));
			const bool bIsTransient = Pkg->HasAnyFlags(RF_Transient)
				|| PackageName.StartsWith(TEXT("/Temp/"))
				|| PackageName.StartsWith(TEXT("/Engine/Transient"));
			const bool bBlocksUnattendedLevelLoad = PackageName.StartsWith(TEXT("/Game/"))
				|| PackageName.StartsWith(TEXT("/Temp/"))
				|| (!bIsScriptPackage && !PackageName.StartsWith(TEXT("/Engine/Transient")));
			if (bBlocksUnattendedLevelLoad)
			{
				++OutBlockingCount;
			}
			if (!bIncludeTransient && bIsTransient)
			{
				++OutOmittedCount;
				continue;
			}
			if (PackagesJson.Num() >= MaxPackages)
			{
				++OutOmittedCount;
				continue;
			}

			TSharedRef<FJsonObject> PackageJson = MakeShared<FJsonObject>();
			PackageJson->SetStringField(TEXT("name"), PackageName);
			PackageJson->SetStringField(TEXT("package_path"), PackageName);
			PackageJson->SetStringField(TEXT("path_name"), Pkg->GetPathName());
			PackageJson->SetBoolField(TEXT("is_transient"), bIsTransient);
			PackageJson->SetBoolField(TEXT("is_script_package"), bIsScriptPackage);
			PackageJson->SetBoolField(TEXT("is_engine_package"), bIsEnginePackage);
			PackageJson->SetBoolField(TEXT("is_play_in_editor_package"), Pkg->HasAnyPackageFlags(PKG_PlayInEditor));
			PackageJson->SetBoolField(TEXT("is_user_content_hint"), PackageName.StartsWith(TEXT("/Game/")));
			PackageJson->SetBoolField(TEXT("is_temp_hint"), PackageName.StartsWith(TEXT("/Temp/")));
			PackageJson->SetBoolField(TEXT("blocks_unattended_level_load"), bBlocksUnattendedLevelLoad);
			PackagesJson.Add(MakeShared<FJsonValueObject>(PackageJson));
			++OutReturnedCount;
		}

		return PackagesJson;
	}

	static bool SimulateSlateLeftClick(double ScreenX, double ScreenY, bool& bOutDownHandled, bool& bOutUpHandled)
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

	static bool ResolveDialogById(
		const FString& DialogId,
		TSharedPtr<SWindow>& OutWindow,
		int32& OutWindowIndex,
		FString& OutTitle)
	{
		OutWindow.Reset();
		OutWindowIndex = INDEX_NONE;
		OutTitle.Reset();

		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}

		const TArray<TSharedRef<SWindow>> Windows = EnumerateVisibleSlateWindows();
		for (int32 WindowIndex = 0; WindowIndex < Windows.Num(); ++WindowIndex)
		{
			const TSharedRef<SWindow>& Window = Windows[WindowIndex];
			if (MakeDialogId(Window, WindowIndex) == DialogId)
			{
				OutWindow = Window;
				OutWindowIndex = WindowIndex;
				OutTitle = NormalizeDialogText(Window->GetTitle().ToString());
				return true;
			}
		}
		return false;
	}

	static bool TitleContainsAny(const FString& Title, const TArray<FString>& Substrings, FString& OutMatched)
	{
		for (const FString& Substring : Substrings)
		{
			if (!Substring.IsEmpty() && Title.Contains(Substring))
			{
				OutMatched = Substring;
				return true;
			}
		}
		return false;
	}

	static bool IsResponseAllowedByPolicy(
		const FEditorDialogPolicyState& Policy,
		const FString& DialogTitle,
		FString& OutReason)
	{
		if (!Policy.bEnabled)
		{
			OutReason = TEXT("policy_disabled_explicit_response_allowed");
			return true;
		}

		FString Matched;
		if (TitleContainsAny(DialogTitle, Policy.DenyTitleSubstrings, Matched))
		{
			OutReason = FString::Printf(TEXT("blocked_by_deny_title_substring:%s"), *Matched);
			return false;
		}

		if (Policy.Mode == TEXT("block_all"))
		{
			OutReason = TEXT("blocked_by_policy_mode:block_all");
			return false;
		}

		if (Policy.Mode == TEXT("allowlist"))
		{
			if (TitleContainsAny(DialogTitle, Policy.AllowTitleSubstrings, Matched))
			{
				OutReason = FString::Printf(TEXT("allowed_by_allow_title_substring:%s"), *Matched);
				return true;
			}
			OutReason = TEXT("blocked_by_policy_mode:allowlist_no_title_match");
			return false;
		}

		OutReason = FString::Printf(TEXT("allowed_by_policy_mode:%s"), *Policy.Mode);
		return true;
	}

	static void BuildWidgetTreeSummary(
		TSharedPtr<SWidget> Widget,
		TArray<TSharedPtr<FJsonValue>>& OutArray,
		int32 Depth,
		int32 MaxDepth)
	{
		if (!Widget.IsValid() || Depth > MaxDepth || OutArray.Num() >= 160)
		{
			return;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("depth"), Depth);
		Entry->SetStringField(TEXT("type"), Widget->GetTypeAsString());
		Entry->SetStringField(TEXT("tag"), Widget->GetTag().IsNone() ? TEXT("") : Widget->GetTag().ToString());
		const EVisibility Visibility = Widget->GetVisibility();
		Entry->SetBoolField(
			TEXT("visible"),
			Visibility == EVisibility::Visible
				|| Visibility == EVisibility::SelfHitTestInvisible
				|| Visibility == EVisibility::HitTestInvisible);
		const FVector2D DesiredSize = Widget->GetDesiredSize();
		TSharedRef<FJsonObject> SizeJson = MakeShared<FJsonObject>();
		SizeJson->SetNumberField(TEXT("w"), FMath::RoundToInt(DesiredSize.X));
		SizeJson->SetNumberField(TEXT("h"), FMath::RoundToInt(DesiredSize.Y));
		Entry->SetObjectField(TEXT("desired_size"), SizeJson);
		OutArray.Add(MakeShared<FJsonValueObject>(Entry));

		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 ChildIndex = 0; ChildIndex < Children->Num() && ChildIndex < 32; ++ChildIndex)
			{
				BuildWidgetTreeSummary(Children->GetChildAt(ChildIndex), OutArray, Depth + 1, MaxDepth);
			}
		}
	}

	static bool IsAllowedNotificationWidgetType(const FString& Type)
	{
		return Type == TEXT("SWindow")
			|| Type == TEXT("SNotificationList")
			|| Type.Contains(TEXT("SNotificationItem"), ESearchCase::IgnoreCase)
			|| Type.Contains(TEXT("SNotificationBackground"), ESearchCase::IgnoreCase)
			|| Type == TEXT("SVerticalBox")
			|| Type == TEXT("SHorizontalBox")
			|| Type == TEXT("SOverlay")
			|| Type == TEXT("SBorder")
			|| Type == TEXT("SBox")
			|| Type == TEXT("SImage")
			|| Type == TEXT("STextBlock")
			|| Type == TEXT("SSpacer")
			|| Type == TEXT("SNullWidget")
			|| Type == TEXT("SInvalidationPanel")
			|| Type == TEXT("SScaleBox")
			|| Type == TEXT("SSeparator");
	}

	static bool WidgetTreeLooksNotificationOnly(
		TSharedPtr<SWidget> Widget,
		int32 Depth,
		int32 MaxDepth,
		bool& bSawNotificationList,
		bool& bSawNotificationItem)
	{
		if (!Widget.IsValid() || Depth > MaxDepth)
		{
			return false;
		}

		const FString Type = Widget->GetTypeAsString();
		if (Type.Contains(TEXT("SNotificationList"), ESearchCase::IgnoreCase))
		{
			bSawNotificationList = true;
		}
		if (Type.Contains(TEXT("SNotificationItem"), ESearchCase::IgnoreCase))
		{
			bSawNotificationItem = true;
		}
		if (!IsAllowedNotificationWidgetType(Type))
		{
			return false;
		}

		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 ChildIndex = 0; ChildIndex < Children->Num() && ChildIndex < 64; ++ChildIndex)
			{
				if (!WidgetTreeLooksNotificationOnly(
					Children->GetChildAt(ChildIndex),
					Depth + 1,
					MaxDepth,
					bSawNotificationList,
					bSawNotificationItem))
				{
					return false;
				}
			}
		}
		return true;
	}

	static void WidgetTreeFindNotificationMarkers(
		TSharedPtr<SWidget> Widget,
		int32 Depth,
		int32 MaxDepth,
		bool& bSawNotificationList,
		bool& bSawNotificationItem)
	{
		if (!Widget.IsValid() || Depth > MaxDepth)
		{
			return;
		}

		const FString Type = Widget->GetTypeAsString();
		if (Type.Contains(TEXT("SNotificationList"), ESearchCase::IgnoreCase))
		{
			bSawNotificationList = true;
		}
		if (Type.Contains(TEXT("SNotificationItem"), ESearchCase::IgnoreCase))
		{
			bSawNotificationItem = true;
		}

		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 ChildIndex = 0; ChildIndex < Children->Num() && ChildIndex < 128; ++ChildIndex)
			{
				WidgetTreeFindNotificationMarkers(
					Children->GetChildAt(ChildIndex),
					Depth + 1,
					MaxDepth,
					bSawNotificationList,
					bSawNotificationItem);
			}
		}
	}

	static bool WindowLooksLikeNonBlockingNotification(
		const TSharedRef<SWindow>& Window,
		const FString& DialogTitle,
		const TArray<FDialogButtonCandidate>& Buttons)
	{
		if (Window->IsModalWindow() || Window->IsRegularWindow() || !DialogTitle.IsEmpty() || Buttons.Num() > 0)
		{
			return false;
		}

		bool bSawNotificationList = false;
		bool bSawNotificationItem = false;
		const bool bOnlyAllowedNotificationWidgets = WidgetTreeLooksNotificationOnly(
			Window,
			0,
			12,
			bSawNotificationList,
			bSawNotificationItem);
		if (!bOnlyAllowedNotificationWidgets)
		{
			bSawNotificationList = false;
			bSawNotificationItem = false;
			WidgetTreeFindNotificationMarkers(Window, 0, 12, bSawNotificationList, bSawNotificationItem);
		}
		return bSawNotificationList && bSawNotificationItem;
	}

	static bool WindowLooksLikeNonBlockingUtilityWindow(
		const TSharedRef<SWindow>& Window,
		const FString& DialogTitle,
		const TArray<FDialogButtonCandidate>& Buttons)
	{
		if (Window->IsModalWindow() || Window->IsRegularWindow() || !DialogTitle.IsEmpty() || Buttons.Num() > 0)
		{
			return false;
		}

		const FVector2D Size = Window->GetSizeInScreen();
		const bool bTinyUntitledSlateWindow = Size.X > 0.0
			&& Size.Y > 0.0
			&& Size.X <= 360.0
			&& Size.Y <= 240.0;
		if (!bTinyUntitledSlateWindow)
		{
			return false;
		}

		bool bSawTextBlock = false;
		bool bSawButton = false;
		bool bSawMenu = false;
		WidgetTreeFindDialogHints(Window, 0, 10, bSawTextBlock, bSawButton, bSawMenu);
		return !bSawButton && !bSawMenu;
	}

	static bool WindowLooksLikeNonBlockingEditorWindow(
		const TSharedRef<SWindow>& Window,
		const FString& DialogTitle,
		const TArray<FDialogButtonCandidate>& Buttons,
		bool& bOutLooksLikeNotification,
		bool& bOutLooksLikeUtilityWindow)
	{
		bOutLooksLikeNotification = WindowLooksLikeNonBlockingNotification(Window, DialogTitle, Buttons);
		bOutLooksLikeUtilityWindow = !bOutLooksLikeNotification
			&& WindowLooksLikeNonBlockingUtilityWindow(Window, DialogTitle, Buttons);
		return bOutLooksLikeNotification || bOutLooksLikeUtilityWindow;
	}

	static TSharedRef<FJsonObject> MakeWindowJson(
		const TSharedRef<SWindow>& Window,
		int32 Index,
		bool bIncludeWidgetTree)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("dialog_id"), MakeDialogId(Window, Index));
		Json->SetStringField(TEXT("title"), NormalizeDialogText(Window->GetTitle().ToString()));
		Json->SetStringField(TEXT("type"), Window->GetTypeAsString());
		Json->SetBoolField(TEXT("visible"), Window->IsVisible());
		Json->SetBoolField(TEXT("is_modal"), Window->IsModalWindow());
		Json->SetBoolField(TEXT("is_regular_window"), Window->IsRegularWindow());

		const FVector2D Position = Window->GetPositionInScreen();
		const FVector2D Size = Window->GetSizeInScreen();
		TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetNumberField(TEXT("x"), FMath::RoundToInt(Position.X));
		Bounds->SetNumberField(TEXT("y"), FMath::RoundToInt(Position.Y));
		Bounds->SetNumberField(TEXT("w"), FMath::RoundToInt(Size.X));
		Bounds->SetNumberField(TEXT("h"), FMath::RoundToInt(Size.Y));
		Json->SetObjectField(TEXT("screen_bounds"), Bounds);

		if (bIncludeWidgetTree)
		{
			TArray<TSharedPtr<FJsonValue>> Widgets;
			BuildWidgetTreeSummary(Window, Widgets, 0, 4);
			Json->SetArrayField(TEXT("widgets"), Widgets);
			Json->SetNumberField(TEXT("widget_count"), Widgets.Num());
		}

		TArray<FDialogButtonCandidate> Buttons;
		CollectButtonCandidates(Window, Buttons, 0, 8);
		const FString DialogTitle = NormalizeDialogText(Window->GetTitle().ToString());
		bool bLooksLikeNonBlockingNotification = false;
		bool bLooksLikeNonBlockingUtilityWindow = false;
		WindowLooksLikeNonBlockingEditorWindow(
			Window,
			DialogTitle,
			Buttons,
			bLooksLikeNonBlockingNotification,
			bLooksLikeNonBlockingUtilityWindow);
		Json->SetObjectField(TEXT("dialog_classification"), MakeDialogClassificationJson(
			DialogTitle,
			Buttons,
			Window->IsModalWindow(),
			Window->IsRegularWindow(),
			bLooksLikeNonBlockingNotification,
			bLooksLikeNonBlockingUtilityWindow));
		Json->SetArrayField(TEXT("button_candidates"), ButtonCandidatesToJson(Buttons));
		Json->SetNumberField(TEXT("button_count"), Buttons.Num());

		return Json;
	}

	static TArray<TSharedRef<SWindow>> EnumerateVisibleSlateWindows()
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

	static TSharedRef<FJsonObject> MakeStringArraySchema(const FString& Description)
	{
		return FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), Description);
	}

	static TSharedRef<FJsonObject> MakePolicySetSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("enabled"), FSololmcpSchemaBuilder::Boolean(TEXT("Enable dialog policy tracking for subsequent dialog tools."))},
				{TEXT("mode"), FSololmcpSchemaBuilder::String(
					TEXT("Policy mode. observe_only never auto-responds; allowlist/denylist are reserved for live Slate implementation."),
					{TEXT("observe_only"), TEXT("allowlist"), TEXT("denylist"), TEXT("block_all")})},
				{TEXT("default_action"), FSololmcpSchemaBuilder::String(
					TEXT("Fallback response reserved for live implementation."),
					{TEXT("none"), TEXT("accept"), TEXT("cancel"), TEXT("close")})},
				{TEXT("timeout_ms"), FSololmcpSchemaBuilder::Integer(TEXT("Optional maximum wait time for future live modal handling."))},
				{TEXT("allow_title_substrings"), MakeStringArraySchema(TEXT("Case-sensitive title substrings allowed for auto-response."))},
				{TEXT("deny_title_substrings"), MakeStringArraySchema(TEXT("Case-sensitive title substrings blocked from auto-response."))},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Preview the resulting policy without storing it."))}
			});
	}

	static TSharedRef<FJsonObject> MakeEditorDirtyPackageListSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("max_packages"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum dirty package names to return. Default: 200."))},
				{TEXT("include_transient"), FSololmcpSchemaBuilder::Boolean(TEXT("Include transient/temp dirty packages in the returned list. Default: true."))}
			});
	}

	static TSharedRef<FJsonObject> MakeDialogListSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("title_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional title substring filter."))},
				{TEXT("include_widget_tree"), FSololmcpSchemaBuilder::Boolean(TEXT("Include a bounded read-only Slate widget tree for each matched window."))},
				{TEXT("include_regular_windows"), FSololmcpSchemaBuilder::Boolean(TEXT("Include regular top-level editor windows as well as modal/non-regular windows."))},
				{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum dialogs/windows to report. Default: 16."))}
			});
	}

	static TSharedRef<FJsonObject> MakeDialogRespondSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("dialog_id"), FSololmcpSchemaBuilder::String(TEXT("Dialog id from editor_dialog_list."))},
				{TEXT("action"), FSololmcpSchemaBuilder::String(
					TEXT("Requested modal response."),
					{TEXT("accept"), TEXT("cancel"), TEXT("close"), TEXT("no"), TEXT("dont_save"), TEXT("skip_recovery"), TEXT("button_text")})},
				{TEXT("button_text"), FSololmcpSchemaBuilder::String(TEXT("Button label to click when action=button_text."))},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Validate the request without applying a response."))}
			},
			{TEXT("dialog_id"), TEXT("action")});
	}

	static TSharedRef<FJsonObject> MakeDialogSafeRespondSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("dialog_id"), FSololmcpSchemaBuilder::String(TEXT("Dialog id from editor_dialog_list."))},
				{TEXT("context"), FSololmcpSchemaBuilder::String(TEXT("Operation context: umg_compile, blueprint_compile, pie_start, save, unknown."))},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("Resolve and choose the safe button without clicking it."))},
				{TEXT("allow_close_without_button"), FSololmcpSchemaBuilder::Boolean(TEXT("If no safe button exists, request SWindow close instead of failing. Default false."))}
			},
			{TEXT("dialog_id")});
	}

	static TSharedRef<FJsonObject> MakeDialogWatchdogTickSchema()
	{
		return FSololmcpSchemaBuilder::Object(
			{
				{TEXT("context"), FSololmcpSchemaBuilder::String(TEXT("Operation context: blueprint_compile, umg_compile, animbp_compile, pie_start, save, unknown."))},
				{TEXT("title_filter"), FSololmcpSchemaBuilder::String(TEXT("Optional title substring filter."))},
				{TEXT("dry_run"), FSololmcpSchemaBuilder::Boolean(TEXT("List and choose the safe response without clicking it."))},
				{TEXT("include_widget_tree"), FSololmcpSchemaBuilder::Boolean(TEXT("Include bounded Slate widget tree evidence for the selected modal."))},
				{TEXT("allow_close_without_button"), FSololmcpSchemaBuilder::Boolean(TEXT("If no safe button exists, request SWindow close instead of failing. Default false."))},
				{TEXT("max_results"), FSololmcpSchemaBuilder::Integer(TEXT("Maximum candidate dialogs/windows to report. Default: 8."))}
			});
	}
}

void RegisterEditorDialogTools(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("editor_dialog_policy_set"),
		TEXT("Set the editor modal dialog handling policy. Stores allow/deny policy for dialog list/respond; live button clicks are handled by editor_dialog_respond."),
		MakePolicySetSchema(),
		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& /*OutError*/) -> bool
		{
			FEditorDialogPolicyState NextPolicy;
			{
				FScopeLock Lock(&GEditorDialogPolicyMutex);
				NextPolicy = GEditorDialogPolicy;
			}

			bool bBool = false;
			if (Arguments->TryGetBoolField(TEXT("enabled"), bBool))
			{
				NextPolicy.bEnabled = bBool;
			}

			FString StringValue;
			if (Arguments->TryGetStringField(TEXT("mode"), StringValue) && !StringValue.IsEmpty())
			{
				NextPolicy.Mode = StringValue;
			}
			if (Arguments->TryGetStringField(TEXT("default_action"), StringValue) && !StringValue.IsEmpty())
			{
				NextPolicy.DefaultAction = StringValue;
			}

			int32 TimeoutMs = 0;
			if (Arguments->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs))
			{
				NextPolicy.TimeoutMs = FMath::Max(0, TimeoutMs);
			}

			if (Arguments->HasField(TEXT("allow_title_substrings")))
			{
				NextPolicy.AllowTitleSubstrings = ReadStringArrayField(Arguments, TEXT("allow_title_substrings"));
			}
			if (Arguments->HasField(TEXT("deny_title_substrings")))
			{
				NextPolicy.DenyTitleSubstrings = ReadStringArrayField(Arguments, TEXT("deny_title_substrings"));
			}
			NextPolicy.UpdatedAtUtc = UtcNowIso();

			bool bDryRun = false;
			Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
			if (!bDryRun)
			{
				FScopeLock Lock(&GEditorDialogPolicyMutex);
				GEditorDialogPolicy = NextPolicy;
			}

			SetContractFields(OutStructured);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
			OutStructured->SetBoolField(TEXT("stored"), !bDryRun);
			OutStructured->SetObjectField(TEXT("policy"), MakePolicyJson(NextPolicy));
			OutStructured->SetBoolField(TEXT("live_modal_control_enabled"), true);
			OutStructured->SetStringField(TEXT("status"), bDryRun ? TEXT("preview") : TEXT("stored_stub_policy"));
			OutSummary = bDryRun
				? TEXT("Previewed editor dialog policy for live Slate modal control.")
				: TEXT("Stored editor dialog policy for live Slate modal control.");
			return true;
		}
	});

	Registry.Register({
		TEXT("editor_dirty_package_list"),
		TEXT("List dirty UPackage names and safety status before unattended editor operations such as level_load."),
		MakeEditorDirtyPackageListSchema(),
		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& /*OutError*/) -> bool
		{
			int32 MaxPackages = 200;
			Arguments->TryGetNumberField(TEXT("max_packages"), MaxPackages);
			MaxPackages = FMath::Clamp(MaxPackages, 1, 1000);
			bool bIncludeTransient = true;
			Arguments->TryGetBoolField(TEXT("include_transient"), bIncludeTransient);

			int32 DirtyCount = 0;
			int32 ReturnedCount = 0;
			int32 OmittedCount = 0;
			int32 BlockingDirtyCount = 0;
			TArray<TSharedPtr<FJsonValue>> Packages = CollectDirtyPackageJson(
				MaxPackages,
				bIncludeTransient,
				DirtyCount,
				ReturnedCount,
				OmittedCount,
				BlockingDirtyCount);

			SetContractFields(OutStructured);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("status"), DirtyCount > 0 ? TEXT("dirty_packages_present") : TEXT("clean_no_dirty_packages"));
			OutStructured->SetNumberField(TEXT("dirty_package_count"), DirtyCount);
			OutStructured->SetNumberField(TEXT("count"), DirtyCount);
			OutStructured->SetNumberField(TEXT("blocking_dirty_package_count"), BlockingDirtyCount);
			OutStructured->SetNumberField(TEXT("save_prompt_relevant_dirty_package_count"), BlockingDirtyCount);
			OutStructured->SetNumberField(TEXT("returned_package_count"), ReturnedCount);
			OutStructured->SetNumberField(TEXT("omitted_package_count"), OmittedCount);
			OutStructured->SetBoolField(TEXT("include_transient"), bIncludeTransient);
			OutStructured->SetArrayField(TEXT("packages"), Packages);
			OutStructured->SetBoolField(TEXT("safe_to_level_load_without_save_prompt"), BlockingDirtyCount == 0);
			OutStructured->SetStringField(
				TEXT("failure_route"),
				BlockingDirtyCount > 0 ? TEXT("blocked_dirty_packages") : (DirtyCount > 0 ? TEXT("dirty_non_blocking_packages_present") : TEXT("none")));
			OutStructured->SetStringField(
				TEXT("unattended_policy"),
				TEXT("Read only. Never click Save, Yes, Run in Editor, Recover Selected, or discard dirty user packages automatically."));
			OutSummary = FString::Printf(
				TEXT("Dirty package state: %d dirty package(s), %d save-prompt relevant, %d returned, %d omitted."),
				DirtyCount,
				BlockingDirtyCount,
				ReturnedCount,
				OmittedCount);
			return true;
		}
	});

	Registry.Register({
		TEXT("editor_dialog_list"),
		TEXT("List active editor modal dialogs and bounded Slate window/widget summaries."),
		MakeDialogListSchema(),
		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& /*OutError*/) -> bool
		{
			FEditorDialogPolicyState Policy;
			{
				FScopeLock Lock(&GEditorDialogPolicyMutex);
				Policy = GEditorDialogPolicy;
			}

			FString TitleFilter;
			Arguments->TryGetStringField(TEXT("title_filter"), TitleFilter);
			bool bIncludeWidgetTree = false;
			Arguments->TryGetBoolField(TEXT("include_widget_tree"), bIncludeWidgetTree);
			bool bIncludeRegularWindows = false;
			Arguments->TryGetBoolField(TEXT("include_regular_windows"), bIncludeRegularWindows);
			int32 MaxResults = 16;
			Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
			MaxResults = FMath::Clamp(MaxResults, 1, 64);

			TArray<TSharedPtr<FJsonValue>> Dialogs;
			int32 CandidateCount = 0;
			int32 BlockingDialogCount = 0;
			int32 NonBlockingNotificationCount = 0;
			int32 NonBlockingUtilityWindowCount = 0;
			if (FSlateApplication::IsInitialized())
			{
				const TArray<TSharedRef<SWindow>> Windows = EnumerateVisibleSlateWindows();
				CandidateCount = Windows.Num();
				for (int32 WindowIndex = 0; WindowIndex < Windows.Num() && Dialogs.Num() < MaxResults; ++WindowIndex)
				{
					const TSharedRef<SWindow>& Window = Windows[WindowIndex];
					const FString Title = NormalizeDialogText(Window->GetTitle().ToString());
					const bool bLooksLikeDialog = Window->IsModalWindow() || !Window->IsRegularWindow();
					if (!bIncludeRegularWindows && !bLooksLikeDialog)
					{
						continue;
					}
					if (!TitleFilter.IsEmpty() && !Title.Contains(TitleFilter))
					{
						continue;
					}
					TArray<FDialogButtonCandidate> Buttons;
					CollectButtonCandidates(Window, Buttons, 0, 8);
					bool bLooksLikeNonBlockingNotification = false;
					bool bLooksLikeNonBlockingUtilityWindow = false;
					if (WindowLooksLikeNonBlockingEditorWindow(
						Window,
						Title,
						Buttons,
						bLooksLikeNonBlockingNotification,
						bLooksLikeNonBlockingUtilityWindow))
					{
						if (bLooksLikeNonBlockingNotification)
						{
							++NonBlockingNotificationCount;
						}
						if (bLooksLikeNonBlockingUtilityWindow)
						{
							++NonBlockingUtilityWindowCount;
						}
					}
					else
					{
						++BlockingDialogCount;
					}
					Dialogs.Add(MakeShared<FJsonValueObject>(MakeWindowJson(Window, WindowIndex, bIncludeWidgetTree)));
				}
			}

			SetContractFields(OutStructured);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("status"), FSlateApplication::IsInitialized() ? TEXT("slate_enumerated") : TEXT("slate_unavailable"));
			OutStructured->SetStringField(TEXT("title_filter"), TitleFilter);
			OutStructured->SetBoolField(TEXT("include_widget_tree_requested"), bIncludeWidgetTree);
			OutStructured->SetBoolField(TEXT("include_regular_windows"), bIncludeRegularWindows);
			OutStructured->SetNumberField(TEXT("candidate_window_count"), CandidateCount);
			OutStructured->SetNumberField(TEXT("count"), Dialogs.Num());
			OutStructured->SetNumberField(TEXT("blocking_dialog_count"), BlockingDialogCount);
			OutStructured->SetNumberField(TEXT("non_blocking_notification_count"), NonBlockingNotificationCount);
			OutStructured->SetNumberField(TEXT("non_blocking_utility_window_count"), NonBlockingUtilityWindowCount);
			OutStructured->SetBoolField(TEXT("blocks_unattended_lane"), BlockingDialogCount > 0);
			OutStructured->SetArrayField(TEXT("dialogs"), Dialogs);
			OutStructured->SetObjectField(TEXT("policy"), MakePolicyJson(Policy));
			OutStructured->SetStringField(TEXT("next_implementation_step"), TEXT("Long-queue watchdog should call editor_dialog_respond with cancel/button_text when a blocking modal is detected."));
			OutSummary = FString::Printf(
				TEXT("Enumerated %d candidate Slate window(s), returned %d dialog/window receipt(s), blocking=%d, non_blocking_notifications=%d."),
				CandidateCount,
				Dialogs.Num(),
				BlockingDialogCount,
				NonBlockingNotificationCount);
			return true;
		}
	});

	Registry.Register({
		TEXT("editor_dialog_respond"),
		TEXT("Respond to an editor modal dialog by id. Supports dry-run resolution, safe close dispatch, and verified Slate button clicks for cancel/accept/button_text."),
		MakeDialogRespondSchema(),
		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& /*OutError*/) -> bool
		{
			FString DialogId;
			FString Action;
			FString ButtonText;
			Arguments->TryGetStringField(TEXT("dialog_id"), DialogId);
			Arguments->TryGetStringField(TEXT("action"), Action);
			Arguments->TryGetStringField(TEXT("button_text"), ButtonText);
			ButtonText = NormalizeDialogText(ButtonText);

			bool bDryRun = false;
			Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);

			FEditorDialogPolicyState Policy;
			{
				FScopeLock Lock(&GEditorDialogPolicyMutex);
				Policy = GEditorDialogPolicy;
			}

			TSharedPtr<SWindow> Window;
			int32 WindowIndex = INDEX_NONE;
			FString DialogTitle;
			const bool bDialogFound = ResolveDialogById(DialogId, Window, WindowIndex, DialogTitle);
			TArray<FDialogButtonCandidate> ButtonCandidates;
			if (Window.IsValid())
			{
				CollectButtonCandidates(Window, ButtonCandidates, 0, 8);
			}

			FString PolicyReason;
			const bool bPolicyAllows = bDialogFound
				? IsResponseAllowedByPolicy(Policy, DialogTitle, PolicyReason)
				: false;

			bool bApplied = false;
			bool bButtonClickAttempted = false;
			bool bMouseDownHandled = false;
			bool bMouseUpHandled = false;
			int32 MatchedButtonIndex = INDEX_NONE;
			double ClickX = 0.0;
			double ClickY = 0.0;
			FString Status = TEXT("dialog_not_found");
			FString Reason = FSlateApplication::IsInitialized()
				? TEXT("No live visible Slate window matched dialog_id. Refresh editor_dialog_list and retry.")
				: TEXT("Slate is not initialized.");
			auto IsMatchedButtonAllowedForClick = [&ButtonCandidates](int32 ButtonIndex, FString& InOutStatus, FString& InOutReason) -> bool
			{
				if (!ButtonCandidates.IsValidIndex(ButtonIndex))
				{
					return false;
				}
				const FButtonSafetyClassification Safety = ClassifyButtonSafetyLabel(ButtonCandidates[ButtonIndex].Text);
				if (!Safety.bUnattendedAllowed)
				{
					InOutStatus = Safety.bForbidden ? TEXT("dangerous_button_refused") : TEXT("unknown_button_refused");
					InOutReason = Safety.Reason;
					return false;
				}
				return true;
			};

			if (bDialogFound)
			{
				if (!bPolicyAllows)
				{
					Status = TEXT("blocked_by_policy");
					Reason = PolicyReason;
				}
				else if (bDryRun)
				{
					Status = TEXT("dry_run_resolved");
					Reason = TEXT("Matched live dialog; dry_run requested, so no response was dispatched.");
				}
				else if (Action == TEXT("close"))
				{
					Window->RequestDestroyWindow();
					bApplied = true;
					Status = TEXT("close_requested");
					Reason = TEXT("RequestDestroyWindow dispatched to the matched SWindow.");
				}
				else if (Action == TEXT("button_text"))
				{
					MatchedButtonIndex = FindDialogButtonIndex(Action, ButtonText, ButtonCandidates, Reason);
					if (MatchedButtonIndex == INDEX_NONE)
					{
						Status = TEXT("button_not_found");
					}
					else if (!IsMatchedButtonAllowedForClick(MatchedButtonIndex, Status, Reason))
					{
					}
					else if (!ButtonCandidates[MatchedButtonIndex].bHasGeometry)
					{
						Status = TEXT("button_geometry_unavailable");
						Reason = TEXT("Matched button text, but the SButton has no cached screen geometry yet.");
					}
					else
					{
						const FDialogButtonCandidate& Button = ButtonCandidates[MatchedButtonIndex];
						ClickX = Button.CenterX;
						ClickY = Button.CenterY;
						bButtonClickAttempted = true;
						bApplied = SimulateSlateLeftClick(ClickX, ClickY, bMouseDownHandled, bMouseUpHandled);
						Status = bApplied ? TEXT("button_click_dispatched") : TEXT("button_click_unhandled");
						Reason = bApplied
							? FString::Printf(TEXT("Clicked matched dialog button '%s' at %.0f,%.0f."), *Button.Text, ClickX, ClickY)
							: FString::Printf(TEXT("Matched dialog button '%s', but Slate did not handle the click at %.0f,%.0f."), *Button.Text, ClickX, ClickY);
					}
				}
				else if (Action == TEXT("accept") || Action == TEXT("cancel") || Action == TEXT("no") || Action == TEXT("dont_save") || Action == TEXT("skip_recovery"))
				{
					MatchedButtonIndex = FindDialogButtonIndex(Action, ButtonText, ButtonCandidates, Reason);
					if (MatchedButtonIndex == INDEX_NONE)
					{
						Status = FString::Printf(TEXT("%s_button_not_found"), *Action);
					}
					else if (!IsMatchedButtonAllowedForClick(MatchedButtonIndex, Status, Reason))
					{
					}
					else if (!ButtonCandidates[MatchedButtonIndex].bHasGeometry)
					{
						Status = TEXT("button_geometry_unavailable");
						Reason = TEXT("Matched semantic button, but the SButton has no cached screen geometry yet.");
					}
					else
					{
						const FDialogButtonCandidate& Button = ButtonCandidates[MatchedButtonIndex];
						ClickX = Button.CenterX;
						ClickY = Button.CenterY;
						bButtonClickAttempted = true;
						bApplied = SimulateSlateLeftClick(ClickX, ClickY, bMouseDownHandled, bMouseUpHandled);
						Status = bApplied ? TEXT("semantic_button_click_dispatched") : TEXT("semantic_button_click_unhandled");
						Reason = bApplied
							? FString::Printf(TEXT("Clicked %s dialog button '%s' at %.0f,%.0f."), *Action, *Button.Text, ClickX, ClickY)
							: FString::Printf(TEXT("Matched %s dialog button '%s', but Slate did not handle the click at %.0f,%.0f."), *Action, *Button.Text, ClickX, ClickY);
					}
				}
				else
				{
					Status = TEXT("unsupported_action");
					Reason = FString::Printf(TEXT("Unsupported dialog action '%s'."), *Action);
				}
			}

			SetContractFields(OutStructured);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("status"), Status);
			OutStructured->SetStringField(TEXT("dialog_id"), DialogId);
			OutStructured->SetStringField(TEXT("action"), Action);
			OutStructured->SetStringField(TEXT("button_text"), ButtonText);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
			OutStructured->SetBoolField(TEXT("applied"), bApplied);
			OutStructured->SetBoolField(TEXT("dialog_found"), bDialogFound);
			OutStructured->SetStringField(TEXT("dialog_title"), DialogTitle);
			OutStructured->SetNumberField(TEXT("window_index"), WindowIndex);
			OutStructured->SetBoolField(TEXT("policy_allowed"), bPolicyAllows);
			OutStructured->SetStringField(TEXT("policy_reason"), PolicyReason);
			OutStructured->SetObjectField(TEXT("policy"), MakePolicyJson(Policy));
			OutStructured->SetObjectField(TEXT("dialog_classification"), MakeDialogClassificationJson(DialogTitle, ButtonCandidates));
			OutStructured->SetObjectField(TEXT("safe_action_receipt"), MakeSafeActionReceiptJson(
				DialogTitle,
				ButtonCandidates,
				Action,
				MatchedButtonIndex,
				bDryRun,
				bApplied,
				bButtonClickAttempted,
				Status,
				Reason));
			OutStructured->SetArrayField(TEXT("button_candidates"), ButtonCandidatesToJson(ButtonCandidates));
			OutStructured->SetNumberField(TEXT("matched_button_index"), MatchedButtonIndex);
			OutStructured->SetBoolField(TEXT("button_click_attempted"), bButtonClickAttempted);
			OutStructured->SetBoolField(TEXT("mouse_down_handled"), bMouseDownHandled);
			OutStructured->SetBoolField(TEXT("mouse_up_handled"), bMouseUpHandled);
			OutStructured->SetNumberField(TEXT("click_x"), FMath::RoundToInt(ClickX));
			OutStructured->SetNumberField(TEXT("click_y"), FMath::RoundToInt(ClickY));
			OutStructured->SetStringField(TEXT("reason"), Reason);
			OutStructured->SetStringField(
				TEXT("next_implementation_step"),
				TEXT("Use editor_dialog_list -> editor_dialog_respond(cancel/button_text) in long-queue watchdogs before retrying the blocked UE step."));
			OutSummary = bApplied
				? FString::Printf(TEXT("editor_dialog_respond applied close to %s."), *DialogId)
				: FString::Printf(TEXT("editor_dialog_respond resolved request with status=%s; applied=false."), *Status);
			return true;
		}
	});

	Registry.Register({
		TEXT("editor_dialog_safe_respond"),
		TEXT("Choose and click the safest response for a blocking editor modal. Compile/PIE/UMG contexts prefer Cancel/Close/No/Don't Save and refuse Run in Editor or destructive buttons."),
		MakeDialogSafeRespondSchema(),
		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& /*OutError*/) -> bool
		{
			FString DialogId;
			FString OperationContext;
			Arguments->TryGetStringField(TEXT("dialog_id"), DialogId);
			Arguments->TryGetStringField(TEXT("context"), OperationContext);
			if (OperationContext.IsEmpty())
			{
				OperationContext = TEXT("unknown");
			}

			bool bDryRun = false;
			Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
			bool bAllowCloseWithoutButton = false;
			Arguments->TryGetBoolField(TEXT("allow_close_without_button"), bAllowCloseWithoutButton);

			FEditorDialogPolicyState Policy;
			{
				FScopeLock Lock(&GEditorDialogPolicyMutex);
				Policy = GEditorDialogPolicy;
			}

			TSharedPtr<SWindow> Window;
			int32 WindowIndex = INDEX_NONE;
			FString DialogTitle;
			const bool bDialogFound = ResolveDialogById(DialogId, Window, WindowIndex, DialogTitle);
			TArray<FDialogButtonCandidate> ButtonCandidates;
			if (Window.IsValid())
			{
				CollectButtonCandidates(Window, ButtonCandidates, 0, 8);
			}

			FString PolicyReason;
			const bool bPolicyAllows = bDialogFound
				? IsResponseAllowedByPolicy(Policy, DialogTitle, PolicyReason)
				: false;

			FString ChosenAction;
			FString Reason;
			const int32 MatchedButtonIndex = bDialogFound && bPolicyAllows
				? ChooseSafeDialogButtonIndex(DialogTitle, OperationContext, ButtonCandidates, ChosenAction, Reason)
				: INDEX_NONE;

			bool bApplied = false;
			bool bButtonClickAttempted = false;
			bool bMouseDownHandled = false;
			bool bMouseUpHandled = false;
			double ClickX = 0.0;
			double ClickY = 0.0;
			FString Status = TEXT("dialog_not_found");
			if (bDialogFound)
			{
				if (!bPolicyAllows)
				{
					Status = TEXT("blocked_by_policy");
					Reason = PolicyReason;
				}
				else if (MatchedButtonIndex != INDEX_NONE)
				{
					if (bDryRun)
					{
						Status = TEXT("dry_run_safe_button_resolved");
					}
					else if (!ButtonCandidates[MatchedButtonIndex].bHasGeometry)
					{
						Status = TEXT("button_geometry_unavailable");
						Reason = TEXT("Matched safe button, but the SButton has no cached screen geometry yet.");
					}
					else
					{
						const FDialogButtonCandidate& Button = ButtonCandidates[MatchedButtonIndex];
						ClickX = Button.CenterX;
						ClickY = Button.CenterY;
						bButtonClickAttempted = true;
						bApplied = SimulateSlateLeftClick(ClickX, ClickY, bMouseDownHandled, bMouseUpHandled);
						Status = bApplied ? TEXT("safe_button_click_dispatched") : TEXT("safe_button_click_unhandled");
						Reason = bApplied
							? FString::Printf(TEXT("Clicked safe %s button '%s' at %.0f,%.0f."), *ChosenAction, *Button.Text, ClickX, ClickY)
							: FString::Printf(TEXT("Matched safe %s button '%s', but Slate did not handle the click at %.0f,%.0f."), *ChosenAction, *Button.Text, ClickX, ClickY);
					}
				}
				else if (bAllowCloseWithoutButton)
				{
					ChosenAction = TEXT("close");
					if (ButtonCandidates.Num() > 0)
					{
						Status = TEXT("close_without_button_refused");
						Reason = TEXT("Fail-closed: dialog has button candidates but none matched the unattended safe allow-list.");
					}
					else if (bDryRun)
					{
						Status = TEXT("dry_run_safe_close_resolved");
					}
					else
					{
						Window->RequestDestroyWindow();
						bApplied = true;
						Status = TEXT("safe_close_requested");
						Reason = TEXT("No safe button matched; allow_close_without_button requested SWindow close.");
					}
				}
				else
				{
					Status = TEXT("no_safe_response_available");
				}
			}

			SetContractFields(OutStructured);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("status"), Status);
			OutStructured->SetStringField(TEXT("dialog_id"), DialogId);
			OutStructured->SetStringField(TEXT("context"), OperationContext);
			OutStructured->SetStringField(TEXT("chosen_action"), ChosenAction);
			OutStructured->SetStringField(TEXT("chosen_action_reason"), Reason);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
			OutStructured->SetBoolField(TEXT("applied"), bApplied);
			OutStructured->SetBoolField(TEXT("dialog_found"), bDialogFound);
			OutStructured->SetStringField(TEXT("dialog_title"), DialogTitle);
			OutStructured->SetNumberField(TEXT("window_index"), WindowIndex);
			OutStructured->SetBoolField(TEXT("policy_allowed"), bPolicyAllows);
			OutStructured->SetStringField(TEXT("policy_reason"), PolicyReason);
			OutStructured->SetObjectField(TEXT("dialog_classification"), MakeDialogClassificationJson(DialogTitle, ButtonCandidates));
			OutStructured->SetObjectField(TEXT("safe_action_receipt"), MakeSafeActionReceiptJson(
				DialogTitle,
				ButtonCandidates,
				ChosenAction,
				MatchedButtonIndex,
				bDryRun,
				bApplied,
				bButtonClickAttempted,
				Status,
				Reason));
			OutStructured->SetArrayField(TEXT("button_candidates"), ButtonCandidatesToJson(ButtonCandidates));
			OutStructured->SetNumberField(TEXT("matched_button_index"), MatchedButtonIndex);
			OutStructured->SetBoolField(TEXT("button_click_attempted"), bButtonClickAttempted);
			OutStructured->SetBoolField(TEXT("mouse_down_handled"), bMouseDownHandled);
			OutStructured->SetBoolField(TEXT("mouse_up_handled"), bMouseUpHandled);
			OutStructured->SetNumberField(TEXT("click_x"), FMath::RoundToInt(ClickX));
			OutStructured->SetNumberField(TEXT("click_y"), FMath::RoundToInt(ClickY));
			OutStructured->SetStringField(TEXT("retry_decision"), bApplied ? TEXT("retry_original_step_once") : TEXT("block_lane_and_collect_diagnostics"));
			const bool bBlockedModalOrNoResponse = bDialogFound && !bApplied;
			OutStructured->SetBoolField(TEXT("blocked_modal_or_mcp_no_response"), bBlockedModalOrNoResponse);
			if (bBlockedModalOrNoResponse)
			{
				OutStructured->SetStringField(TEXT("blocked_code"), TEXT("blocked_modal_or_mcp_no_response"));
				OutStructured->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_hermes"));
			}
			OutSummary = bApplied
				? FString::Printf(TEXT("editor_dialog_safe_respond applied %s to %s."), *ChosenAction, *DialogId)
				: FString::Printf(TEXT("editor_dialog_safe_respond status=%s; applied=false."), *Status);
			return true;
		}
	});

	Registry.Register({
		TEXT("editor_dialog_watchdog_tick"),
		TEXT("One-shot unattended modal watchdog: list visible blocking Slate dialogs, choose the safest response, click/close it, and re-list after the action."),
		MakeDialogWatchdogTickSchema(),
		[](const FSololmcpToolExecutionContext& /*Context*/,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary,
		   FString& /*OutError*/) -> bool
		{
			FString OperationContext;
			Arguments->TryGetStringField(TEXT("context"), OperationContext);
			if (OperationContext.IsEmpty())
			{
				OperationContext = TEXT("unknown");
			}
			FString TitleFilter;
			Arguments->TryGetStringField(TEXT("title_filter"), TitleFilter);

			bool bDryRun = false;
			Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
			bool bIncludeWidgetTree = false;
			Arguments->TryGetBoolField(TEXT("include_widget_tree"), bIncludeWidgetTree);
			bool bAllowCloseWithoutButton = false;
			Arguments->TryGetBoolField(TEXT("allow_close_without_button"), bAllowCloseWithoutButton);
			int32 MaxResults = 8;
			Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
			MaxResults = FMath::Clamp(MaxResults, 1, 32);

			FEditorDialogPolicyState Policy;
			{
				FScopeLock Lock(&GEditorDialogPolicyMutex);
				Policy = GEditorDialogPolicy;
			}

			TArray<TSharedPtr<FJsonValue>> BeforeDialogs;
			TSharedPtr<SWindow> PickedWindow;
			int32 PickedWindowIndex = INDEX_NONE;
			FString PickedTitle;
			FString PickedDialogId;
			bool bPickedIsModal = false;
			bool bPickedIsRegularWindow = false;
			int32 BlockingBeforeDialogCount = 0;
			int32 NonBlockingNotificationCount = 0;
			int32 NonBlockingUtilityWindowCount = 0;

			if (FSlateApplication::IsInitialized())
			{
				const TArray<TSharedRef<SWindow>> Windows = EnumerateVisibleSlateWindows();
				for (int32 WindowIndex = 0; WindowIndex < Windows.Num(); ++WindowIndex)
				{
					const TSharedRef<SWindow>& Window = Windows[WindowIndex];
					const bool bLooksLikeDialog = Window->IsModalWindow() || !Window->IsRegularWindow();
					const FString Title = NormalizeDialogText(Window->GetTitle().ToString());
					if (!bLooksLikeDialog || (!TitleFilter.IsEmpty() && !Title.Contains(TitleFilter)))
					{
						continue;
					}
					if (BeforeDialogs.Num() < MaxResults)
					{
						BeforeDialogs.Add(MakeShared<FJsonValueObject>(MakeWindowJson(Window, WindowIndex, bIncludeWidgetTree)));
					}
					TArray<FDialogButtonCandidate> WindowButtons;
					CollectButtonCandidates(Window, WindowButtons, 0, 8);
					bool bLooksLikeNonBlockingNotification = false;
					bool bLooksLikeNonBlockingUtilityWindow = false;
					if (WindowLooksLikeNonBlockingEditorWindow(
						Window,
						Title,
						WindowButtons,
						bLooksLikeNonBlockingNotification,
						bLooksLikeNonBlockingUtilityWindow))
					{
						if (bLooksLikeNonBlockingNotification)
						{
							++NonBlockingNotificationCount;
						}
						if (bLooksLikeNonBlockingUtilityWindow)
						{
							++NonBlockingUtilityWindowCount;
						}
						continue;
					}
					++BlockingBeforeDialogCount;
					if (!PickedWindow.IsValid())
					{
						PickedWindow = Window;
						PickedWindowIndex = WindowIndex;
						PickedTitle = Title;
						PickedDialogId = MakeDialogId(Window, WindowIndex);
						bPickedIsModal = Window->IsModalWindow();
						bPickedIsRegularWindow = Window->IsRegularWindow();
					}
				}
			}

			TArray<FDialogButtonCandidate> ButtonCandidates;
			if (PickedWindow.IsValid())
			{
				CollectButtonCandidates(PickedWindow.ToSharedRef(), ButtonCandidates, 0, 8);
			}

			FString PolicyReason;
			const bool bPolicyAllows = PickedWindow.IsValid()
				? IsResponseAllowedByPolicy(Policy, PickedTitle, PolicyReason)
				: false;

			FString ChosenAction;
			FString Reason;
			const int32 MatchedButtonIndex = PickedWindow.IsValid() && bPolicyAllows
				? ChooseSafeDialogButtonIndex(PickedTitle, OperationContext, ButtonCandidates, ChosenAction, Reason)
				: INDEX_NONE;

			bool bApplied = false;
			bool bButtonClickAttempted = false;
			bool bMouseDownHandled = false;
			bool bMouseUpHandled = false;
			double ClickX = 0.0;
			double ClickY = 0.0;
			FString Status = TEXT("no_blocking_dialog");

			if (PickedWindow.IsValid())
			{
				if (!bPolicyAllows)
				{
					Status = TEXT("blocked_by_policy");
					Reason = PolicyReason;
				}
				else if (MatchedButtonIndex != INDEX_NONE)
				{
					if (bDryRun)
					{
						Status = TEXT("dry_run_safe_button_resolved");
					}
					else if (!ButtonCandidates[MatchedButtonIndex].bHasGeometry)
					{
						Status = TEXT("button_geometry_unavailable");
						Reason = TEXT("Matched safe button, but the SButton has no cached screen geometry yet.");
					}
					else
					{
						const FDialogButtonCandidate& Button = ButtonCandidates[MatchedButtonIndex];
						ClickX = Button.CenterX;
						ClickY = Button.CenterY;
						bButtonClickAttempted = true;
						bApplied = SimulateSlateLeftClick(ClickX, ClickY, bMouseDownHandled, bMouseUpHandled);
						Status = bApplied ? TEXT("safe_button_click_dispatched") : TEXT("safe_button_click_unhandled");
						Reason = bApplied
							? FString::Printf(TEXT("Clicked safe %s button '%s' at %.0f,%.0f."), *ChosenAction, *Button.Text, ClickX, ClickY)
							: FString::Printf(TEXT("Matched safe %s button '%s', but Slate did not handle the click at %.0f,%.0f."), *ChosenAction, *Button.Text, ClickX, ClickY);
					}
				}
				else if (bAllowCloseWithoutButton)
				{
					ChosenAction = TEXT("close");
					if (ButtonCandidates.Num() > 0)
					{
						Status = TEXT("close_without_button_refused");
						Reason = TEXT("Fail-closed: dialog has button candidates but none matched the unattended safe allow-list.");
					}
					else if (bDryRun)
					{
						Status = TEXT("dry_run_safe_close_resolved");
					}
					else
					{
						PickedWindow->RequestDestroyWindow();
						bApplied = true;
						Status = TEXT("safe_close_requested");
						Reason = TEXT("No safe button matched; allow_close_without_button requested SWindow close.");
					}
				}
				else
				{
					Status = TEXT("no_safe_response_available");
				}
			}

			if (bApplied)
			{
				FPlatformProcess::Sleep(0.10f);
				// audit-U2 fix (P1): pump CoreTicker rather than re-entering
				// Slate. The dialog-respond path is invoked from Slate event
				// context; a nested FSlateApplication::Tick can torn-write
				// the dialog widget tree we are about to re-enumerate below.
				FTSTicker::GetCoreTicker().Tick(0.10f);
			}

			int32 AfterDialogCount = 0;
			int32 AfterReturnedWindowCount = 0;
			int32 AfterNonBlockingNotificationCount = 0;
			int32 AfterNonBlockingUtilityWindowCount = 0;
			if (FSlateApplication::IsInitialized())
			{
				const TArray<TSharedRef<SWindow>> AfterWindows = EnumerateVisibleSlateWindows();
				for (int32 WindowIndex = 0; WindowIndex < AfterWindows.Num(); ++WindowIndex)
				{
					const TSharedRef<SWindow>& Window = AfterWindows[WindowIndex];
					const bool bLooksLikeDialog = Window->IsModalWindow() || !Window->IsRegularWindow();
					const FString Title = NormalizeDialogText(Window->GetTitle().ToString());
					if (bLooksLikeDialog && (TitleFilter.IsEmpty() || Title.Contains(TitleFilter)))
					{
						++AfterReturnedWindowCount;
						TArray<FDialogButtonCandidate> WindowButtons;
						CollectButtonCandidates(Window, WindowButtons, 0, 8);
						bool bLooksLikeNonBlockingNotification = false;
						bool bLooksLikeNonBlockingUtilityWindow = false;
						if (WindowLooksLikeNonBlockingEditorWindow(
							Window,
							Title,
							WindowButtons,
							bLooksLikeNonBlockingNotification,
							bLooksLikeNonBlockingUtilityWindow))
						{
							if (bLooksLikeNonBlockingNotification)
							{
								++AfterNonBlockingNotificationCount;
							}
							if (bLooksLikeNonBlockingUtilityWindow)
							{
								++AfterNonBlockingUtilityWindowCount;
							}
						}
						else
						{
							++AfterDialogCount;
						}
					}
				}
			}

			SetContractFields(OutStructured);
			OutStructured->SetBoolField(TEXT("ok"), true);
			OutStructured->SetStringField(TEXT("status"), Status);
			OutStructured->SetStringField(TEXT("context"), OperationContext);
			OutStructured->SetStringField(TEXT("dialog_id"), PickedDialogId);
			OutStructured->SetStringField(TEXT("dialog_title"), PickedTitle);
			OutStructured->SetNumberField(TEXT("window_index"), PickedWindowIndex);
			OutStructured->SetArrayField(TEXT("dialogs_before"), BeforeDialogs);
			OutStructured->SetNumberField(TEXT("before_dialog_count"), BlockingBeforeDialogCount);
			OutStructured->SetNumberField(TEXT("before_returned_window_count"), BeforeDialogs.Num());
			OutStructured->SetNumberField(TEXT("non_blocking_notification_count"), NonBlockingNotificationCount);
			OutStructured->SetNumberField(TEXT("non_blocking_utility_window_count"), NonBlockingUtilityWindowCount);
			OutStructured->SetNumberField(TEXT("after_dialog_count"), AfterDialogCount);
			OutStructured->SetNumberField(TEXT("after_returned_window_count"), AfterReturnedWindowCount);
			OutStructured->SetNumberField(TEXT("after_non_blocking_notification_count"), AfterNonBlockingNotificationCount);
			OutStructured->SetNumberField(TEXT("after_non_blocking_utility_window_count"), AfterNonBlockingUtilityWindowCount);
			OutStructured->SetStringField(TEXT("chosen_action"), ChosenAction);
			OutStructured->SetStringField(TEXT("chosen_action_reason"), Reason);
			OutStructured->SetBoolField(TEXT("dry_run"), bDryRun);
			OutStructured->SetBoolField(TEXT("applied"), bApplied);
			OutStructured->SetBoolField(TEXT("policy_allowed"), bPolicyAllows);
			OutStructured->SetStringField(TEXT("policy_reason"), PolicyReason);
			OutStructured->SetObjectField(TEXT("dialog_classification"), MakeDialogClassificationJson(
				PickedTitle,
				ButtonCandidates,
				bPickedIsModal,
				bPickedIsRegularWindow,
				false,
				false,
				PickedWindow.IsValid()));
			OutStructured->SetObjectField(TEXT("safe_action_receipt"), MakeSafeActionReceiptJson(
				PickedTitle,
				ButtonCandidates,
				ChosenAction,
				MatchedButtonIndex,
				bDryRun,
				bApplied,
				bButtonClickAttempted,
				Status,
				Reason,
				bPickedIsModal,
				bPickedIsRegularWindow,
				false,
				false,
				PickedWindow.IsValid()));
			OutStructured->SetArrayField(TEXT("button_candidates"), ButtonCandidatesToJson(ButtonCandidates));
			OutStructured->SetNumberField(TEXT("matched_button_index"), MatchedButtonIndex);
			OutStructured->SetBoolField(TEXT("button_click_attempted"), bButtonClickAttempted);
			OutStructured->SetBoolField(TEXT("mouse_down_handled"), bMouseDownHandled);
			OutStructured->SetBoolField(TEXT("mouse_up_handled"), bMouseUpHandled);
			OutStructured->SetNumberField(TEXT("click_x"), FMath::RoundToInt(ClickX));
			OutStructured->SetNumberField(TEXT("click_y"), FMath::RoundToInt(ClickY));
			const FString RetryDecision = PickedWindow.IsValid()
				? (bApplied ? TEXT("retry_original_step_once") : TEXT("block_lane_and_collect_diagnostics"))
				: TEXT("continue_original_step");
			OutStructured->SetStringField(TEXT("retry_decision"), RetryDecision);
			const bool bBlockedModalOrNoResponse = PickedWindow.IsValid() && !bApplied;
			OutStructured->SetBoolField(TEXT("blocked_modal_or_mcp_no_response"), bBlockedModalOrNoResponse);
			if (bBlockedModalOrNoResponse)
			{
				OutStructured->SetStringField(TEXT("blocked_code"), TEXT("blocked_modal_or_mcp_no_response"));
				OutStructured->SetStringField(TEXT("failure_route"), TEXT("qa_inspector_and_hermes"));
			}
			OutSummary = bApplied
				? FString::Printf(TEXT("editor_dialog_watchdog_tick applied %s to %s; after_dialog_count=%d."), *ChosenAction, *PickedDialogId, AfterDialogCount)
				: FString::Printf(
					TEXT("editor_dialog_watchdog_tick status=%s; before_dialog_count=%d; non_blocking_notifications=%d."),
					*Status,
					BlockingBeforeDialogCount,
					NonBlockingNotificationCount);
			return true;
		}
	});
}
}
