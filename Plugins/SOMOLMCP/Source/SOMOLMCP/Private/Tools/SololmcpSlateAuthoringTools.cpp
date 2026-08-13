// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Docking/TabManager.h"
#include "InputCoreTypes.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/Guid.h"
#include "ToolMenus.h"

namespace UE::SOMOLMCP
{
namespace SlateAuthoring
{
	struct FManagedWidget
	{
		FString Id;
		FString Type;
		FString ParentId;
		TSharedPtr<SWidget> Widget;
		TArray<FString> Children;
		TMap<FString, FString> Properties;
	};

	struct FManagedStyle
	{
		FString Id;
		FName RegistryName;
		TSharedPtr<FSlateStyleSet> Style;
		bool bRegistered = false;
		TArray<FString> Entries;
	};

	static TMap<FString, FManagedWidget> Widgets;
	static TMap<FString, TSharedPtr<SWindow>> Windows;
	static TMap<FString, FManagedStyle> Styles;
	static TSet<FName> RegisteredTabs;
	struct FManagedCommand
	{
		FString Id;
		FString TargetWidgetId;
		FString Property;
		FString Value;
		FInputChord Chord;
	};
	static TMap<FString, FManagedCommand> Commands;
	struct FManagedMenuEntry
	{
		FName MenuName;
		FName SectionName;
		FName EntryName;
	};
	static TMap<FString, FManagedMenuEntry> MenuEntries;
	static TMap<FString, TSharedPtr<FJsonObject>> CustomWidgetSchemas;
	static bool ApplyManagedCommand(const FManagedCommand& Command, FString& Error);
	static int64 MutationSequence = 0;

	static FString ReadString(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, const FString& Fallback = FString())
	{
		FString Value;
		return Args->TryGetStringField(Name, Value) ? Value : Fallback;
	}

	static double ReadNumber(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, double Fallback)
	{
		double Value = Fallback;
		Args->TryGetNumberField(Name, Value);
		return Value;
	}

	static bool ReadBool(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, bool Fallback)
	{
		bool Value = Fallback;
		Args->TryGetBoolField(Name, Value);
		return Value;
	}

	static FString MakeId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
	}

	static EVisibility ParseVisibility(const FString& Raw)
	{
		if (Raw.Equals(TEXT("collapsed"), ESearchCase::IgnoreCase)) return EVisibility::Collapsed;
		if (Raw.Equals(TEXT("hidden"), ESearchCase::IgnoreCase)) return EVisibility::Hidden;
		if (Raw.Equals(TEXT("hit_test_invisible"), ESearchCase::IgnoreCase)) return EVisibility::HitTestInvisible;
		if (Raw.Equals(TEXT("self_hit_test_invisible"), ESearchCase::IgnoreCase)) return EVisibility::SelfHitTestInvisible;
		return EVisibility::Visible;
	}

	static FString VisibilityName(const EVisibility Visibility)
	{
		if (Visibility == EVisibility::Collapsed) return TEXT("collapsed");
		if (Visibility == EVisibility::Hidden) return TEXT("hidden");
		if (Visibility == EVisibility::HitTestInvisible) return TEXT("hit_test_invisible");
		if (Visibility == EVisibility::SelfHitTestInvisible) return TEXT("self_hit_test_invisible");
		return TEXT("visible");
	}

	static FLinearColor ParseColor(const FString& Raw, const FLinearColor& Fallback = FLinearColor::White)
	{
		FLinearColor Color = Fallback;
		return Color.InitFromString(Raw) ? Color : Fallback;
	}

	static TSharedPtr<SWidget> CreateNativeWidget(const FString& Type, const FString& Text, FString& OutError)
	{
		const FString Normalized = Type.ToLower();
		if (Normalized == TEXT("text_block")) return SNew(STextBlock).Text(FText::FromString(Text));
		if (Normalized == TEXT("button")) return SNew(SButton)[SNew(STextBlock).Text(FText::FromString(Text))];
		if (Normalized == TEXT("editable_text_box")) return SNew(SEditableTextBox).Text(FText::FromString(Text));
		if (Normalized == TEXT("check_box")) return SNew(SCheckBox).IsChecked(ECheckBoxState::Unchecked);
		if (Normalized == TEXT("image")) return SNew(SImage);
		if (Normalized == TEXT("spacer")) return SNew(SSpacer);
		if (Normalized == TEXT("border")) return SNew(SBorder);
		if (Normalized == TEXT("box")) return SNew(SBox);
		if (Normalized == TEXT("vertical_box")) return SNew(SVerticalBox);
		if (Normalized == TEXT("horizontal_box")) return SNew(SHorizontalBox);
		if (Normalized == TEXT("overlay")) return SNew(SOverlay);
		if (Normalized == TEXT("scroll_box")) return SNew(SScrollBox);
		if (Normalized == TEXT("grid_panel")) return SNew(SGridPanel);
		if (Normalized == TEXT("splitter")) return SNew(SSplitter);
		OutError = FString::Printf(TEXT("Unsupported managed Slate widget type: %s"), *Type);
		return nullptr;
	}

	static bool DetachFromParent(FManagedWidget& Child)
	{
		if (Child.ParentId.IsEmpty()) return true;
		FManagedWidget* Parent = Widgets.Find(Child.ParentId);
		if (!Parent || !Parent->Widget.IsValid())
		{
			Child.ParentId.Reset();
			return true;
		}
		const TSharedRef<SWidget> ChildRef = Child.Widget.ToSharedRef();
		if (Parent->Type == TEXT("vertical_box")) StaticCastSharedPtr<SVerticalBox>(Parent->Widget)->RemoveSlot(ChildRef);
		else if (Parent->Type == TEXT("horizontal_box")) StaticCastSharedPtr<SHorizontalBox>(Parent->Widget)->RemoveSlot(ChildRef);
		else if (Parent->Type == TEXT("overlay")) StaticCastSharedPtr<SOverlay>(Parent->Widget)->RemoveSlot(ChildRef);
		else if (Parent->Type == TEXT("scroll_box")) StaticCastSharedPtr<SScrollBox>(Parent->Widget)->RemoveSlot(ChildRef);
		else if (Parent->Type == TEXT("grid_panel")) StaticCastSharedPtr<SGridPanel>(Parent->Widget)->RemoveSlot(ChildRef);
		else if (Parent->Type == TEXT("splitter"))
		{
			TSharedPtr<SSplitter> Splitter = StaticCastSharedPtr<SSplitter>(Parent->Widget);
			FChildren* SplitterChildren = Splitter->GetChildren();
			for (int32 Index = 0; SplitterChildren && Index < SplitterChildren->Num(); ++Index)
			{
				if (SplitterChildren->GetChildAt(Index) == ChildRef)
				{
					Splitter->RemoveAt(Index);
					break;
				}
			}
		}
		else if (Parent->Type == TEXT("border")) StaticCastSharedPtr<SBorder>(Parent->Widget)->SetContent(SNullWidget::NullWidget);
		else if (Parent->Type == TEXT("box")) StaticCastSharedPtr<SBox>(Parent->Widget)->SetContent(SNullWidget::NullWidget);
		Parent->Children.Remove(Child.Id);
		Child.ParentId.Reset();
		return true;
	}

	static bool AttachToParent(FManagedWidget& Child, FManagedWidget& Parent, int32 Row, int32 Column, FString& OutError)
	{
		DetachFromParent(Child);
		const TSharedRef<SWidget> ChildRef = Child.Widget.ToSharedRef();
		if (Parent.Type == TEXT("vertical_box")) StaticCastSharedPtr<SVerticalBox>(Parent.Widget)->AddSlot().AutoHeight()[ChildRef];
		else if (Parent.Type == TEXT("horizontal_box")) StaticCastSharedPtr<SHorizontalBox>(Parent.Widget)->AddSlot().AutoWidth()[ChildRef];
		else if (Parent.Type == TEXT("overlay")) StaticCastSharedPtr<SOverlay>(Parent.Widget)->AddSlot()[ChildRef];
		else if (Parent.Type == TEXT("scroll_box")) StaticCastSharedPtr<SScrollBox>(Parent.Widget)->AddSlot()[ChildRef];
		else if (Parent.Type == TEXT("grid_panel")) StaticCastSharedPtr<SGridPanel>(Parent.Widget)->AddSlot(Column, Row)[ChildRef];
		else if (Parent.Type == TEXT("splitter")) StaticCastSharedPtr<SSplitter>(Parent.Widget)->AddSlot()[ChildRef];
		else if (Parent.Type == TEXT("border")) StaticCastSharedPtr<SBorder>(Parent.Widget)->SetContent(ChildRef);
		else if (Parent.Type == TEXT("box")) StaticCastSharedPtr<SBox>(Parent.Widget)->SetContent(ChildRef);
		else
		{
			OutError = FString::Printf(TEXT("Widget %s (%s) is not a supported managed container."), *Parent.Id, *Parent.Type);
			return false;
		}
		Child.ParentId = Parent.Id;
		Parent.Children.AddUnique(Child.Id);
		return true;
	}

	static TSharedRef<FJsonObject> WidgetJson(const FManagedWidget& Item)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("widget_id"), Item.Id);
		Json->SetStringField(TEXT("type"), Item.Type);
		Json->SetStringField(TEXT("parent_id"), Item.ParentId);
		Json->SetBoolField(TEXT("valid"), Item.Widget.IsValid());
		TArray<TSharedPtr<FJsonValue>> Children;
		for (const FString& Id : Item.Children) Children.Add(MakeShared<FJsonValueString>(Id));
		Json->SetArrayField(TEXT("children"), Children);
		TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Item.Properties) Props->SetStringField(Pair.Key, Pair.Value);
		Json->SetObjectField(TEXT("properties"), Props);
		if (Item.Widget.IsValid())
		{
			Json->SetStringField(TEXT("native_type"), Item.Widget->GetTypeAsString());
			Json->SetBoolField(TEXT("enabled"), Item.Widget->IsEnabled());
			Json->SetStringField(TEXT("visibility"), VisibilityName(Item.Widget->GetVisibility()));
			const FVector2D Size = Item.Widget->GetDesiredSize();
			Json->SetNumberField(TEXT("desired_width"), Size.X);
			Json->SetNumberField(TEXT("desired_height"), Size.Y);
		}
		return Json;
	}

	static void SetReceipt(TSharedRef<FJsonObject>& Out, const FString& Tool, bool bMutation)
	{
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("tool"), Tool);
		Out->SetStringField(TEXT("implementation"), TEXT("native_managed_slate"));
		Out->SetNumberField(TEXT("mutation_sequence"), bMutation ? ++MutationSequence : MutationSequence);
		Out->SetStringField(TEXT("receipt_id"), MakeId(TEXT("slate_receipt")));
	}

	static bool ExecuteWidgetTool(const FString& Tool, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		if (!IsInGameThread())
		{
			Error = TEXT("Slate authoring must execute on the game thread.");
			return false;
		}

		if (Tool == TEXT("slate_widget_class_catalog"))
		{
			const TArray<FString> Types = {TEXT("text_block"), TEXT("button"), TEXT("editable_text_box"), TEXT("check_box"), TEXT("image"), TEXT("spacer"), TEXT("border"), TEXT("box"), TEXT("vertical_box"), TEXT("horizontal_box"), TEXT("overlay"), TEXT("scroll_box"), TEXT("grid_panel"), TEXT("splitter")};
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Type : Types) Values.Add(MakeShared<FJsonValueString>(Type));
			Out->SetArrayField(TEXT("types"), Values);
			Out->SetNumberField(TEXT("count"), Values.Num());
			SetReceipt(Out, Tool, false);
			Summary = FString::Printf(TEXT("Listed %d native managed Slate widget types."), Values.Num());
			return true;
		}

		if (Tool == TEXT("slate_widget_create") || Tool == TEXT("slate_widget_tree_create") || Tool == TEXT("slate_panel_create"))
		{
			FString Type = ReadString(Args, TEXT("type"), Tool == TEXT("slate_widget_tree_create") ? TEXT("vertical_box") : TEXT("text_block"));
			Type = Type.ToLower();
			const FString Id = ReadString(Args, TEXT("widget_id"), MakeId(TEXT("slate_widget")));
			if (Widgets.Contains(Id)) { Error = TEXT("widget_id already exists"); return false; }
			FManagedWidget Item;
			Item.Id = Id;
			Item.Type = Type;
			Item.Widget = CreateNativeWidget(Type, ReadString(Args, TEXT("text")), Error);
			if (!Item.Widget.IsValid()) return false;
			Item.Widget->SetTag(FName(*Id));
			Item.Properties.Add(TEXT("text"), ReadString(Args, TEXT("text")));
			Widgets.Add(Id, Item);
			const FString ParentId = ReadString(Args, TEXT("parent_id"));
			if (!ParentId.IsEmpty())
			{
				FManagedWidget* Parent = Widgets.Find(ParentId);
				FManagedWidget* Child = Widgets.Find(Id);
				if (!Parent || !AttachToParent(*Child, *Parent, static_cast<int32>(ReadNumber(Args, TEXT("row"), 0)), static_cast<int32>(ReadNumber(Args, TEXT("column"), 0)), Error))
				{
					Widgets.Remove(Id);
					if (Error.IsEmpty()) Error = TEXT("parent_id was not found");
					return false;
				}
			}
			Out->SetObjectField(TEXT("widget"), WidgetJson(Widgets.FindChecked(Id)));
			SetReceipt(Out, Tool, true);
			Summary = FString::Printf(TEXT("Created native Slate %s as %s."), *Type, *Id);
			return true;
		}

		FString Id = ReadString(Args, TEXT("widget_id"));
		if (Id.IsEmpty() && (Tool == TEXT("slate_panel_add_slot") || Tool == TEXT("slate_widget_reparent")))
		{
			Id = ReadString(Args, TEXT("child_id"));
		}
		FManagedWidget* Item = Widgets.Find(Id);
		if (!Item && Tool != TEXT("slate_widget_validate") && Tool != TEXT("slate_layout_readback"))
		{
			Error = FString::Printf(TEXT("Managed Slate widget not found: %s"), *Id);
			return false;
		}

		if (Tool == TEXT("slate_widget_property_get") || Tool == TEXT("slate_layout_readback") || Tool == TEXT("slate_widget_validate"))
		{
			if (!Item) { Error = FString::Printf(TEXT("Managed Slate widget not found: %s"), *Id); return false; }
			Out->SetObjectField(TEXT("widget"), WidgetJson(*Item));
			Out->SetBoolField(TEXT("passed"), Item->Widget.IsValid());
			SetReceipt(Out, Tool, false);
			Summary = TEXT("Read back and validated managed Slate widget.");
			return true;
		}

		if (Tool == TEXT("slate_widget_property_set") || Tool == TEXT("slate_widget_visibility_set") || Tool == TEXT("slate_widget_enabled_set") || Tool == TEXT("slate_widget_tooltip_set") || Tool == TEXT("slate_slot_property_set") || Tool.Contains(TEXT("_layout_configure")))
		{
			const FString Property = ReadString(Args, TEXT("property"), Tool == TEXT("slate_widget_visibility_set") ? TEXT("visibility") : Tool == TEXT("slate_widget_enabled_set") ? TEXT("enabled") : Tool == TEXT("slate_widget_tooltip_set") ? TEXT("tooltip") : TEXT("layout"));
			const FString Value = ReadString(Args, TEXT("value"), ReadString(Args, TEXT("text")));
			if (Property == TEXT("visibility")) Item->Widget->SetVisibility(ParseVisibility(Value));
			else if (Property == TEXT("enabled")) Item->Widget->SetEnabled(Value.ToBool());
			else if (Property == TEXT("tooltip")) Item->Widget->SetToolTipText(FText::FromString(Value));
			else if (Property == TEXT("text") && Item->Type == TEXT("text_block")) StaticCastSharedPtr<STextBlock>(Item->Widget)->SetText(FText::FromString(Value));
			else if (Property == TEXT("text") && Item->Type == TEXT("editable_text_box")) StaticCastSharedPtr<SEditableTextBox>(Item->Widget)->SetText(FText::FromString(Value));
			else if (Property == TEXT("width") && Item->Type == TEXT("box")) StaticCastSharedPtr<SBox>(Item->Widget)->SetWidthOverride(FCString::Atof(*Value));
			else if (Property == TEXT("height") && Item->Type == TEXT("box")) StaticCastSharedPtr<SBox>(Item->Widget)->SetHeightOverride(FCString::Atof(*Value));
			else if (Tool == TEXT("slate_splitter_configure") && Item->Type == TEXT("splitter"))
			{
				TSharedPtr<SSplitter> Splitter = StaticCastSharedPtr<SSplitter>(Item->Widget);
				const FString Orientation = ReadString(Args, TEXT("orientation"), Value);
				if (!Orientation.IsEmpty())
				{
					if (!Orientation.Equals(TEXT("horizontal"), ESearchCase::IgnoreCase) && !Orientation.Equals(TEXT("vertical"), ESearchCase::IgnoreCase))
					{
						Error = TEXT("orientation must be horizontal or vertical");
						return false;
					}
					Splitter->SetOrientation(Orientation.Equals(TEXT("vertical"), ESearchCase::IgnoreCase) ? Orient_Vertical : Orient_Horizontal);
					Item->Properties.Add(TEXT("orientation"), Orientation.ToLower());
				}
				const int32 SlotIndex = static_cast<int32>(ReadNumber(Args, TEXT("slot_index"), -1));
				if (SlotIndex >= 0)
				{
					const FChildren* SplitterChildren = Splitter->GetChildren();
					if (!SplitterChildren || SlotIndex >= SplitterChildren->Num()) { Error = TEXT("slot_index is outside the splitter slot range"); return false; }
					SSplitter::FSlot& Slot = Splitter->SlotAt(SlotIndex);
					Slot.SetSizeValue(static_cast<float>(ReadNumber(Args, TEXT("size_value"), Slot.GetSizeValue())));
					Slot.SetMinSize(static_cast<float>(ReadNumber(Args, TEXT("min_size"), Slot.GetMinSize())));
					Slot.SetResizable(ReadBool(Args, TEXT("resizable"), Slot.IsResizable()));
				}
			}
			else if (Tool.Contains(TEXT("_layout_configure")) || Tool == TEXT("slate_slot_property_set"))
			{
				Error = FString::Printf(TEXT("%s does not support widget type %s with the supplied property"), *Tool, *Item->Type);
				return false;
			}
			else
			{
				Error = FString::Printf(TEXT("Unsupported Slate property '%s' for widget type %s"), *Property, *Item->Type);
				return false;
			}
			Item->Properties.Add(Property, Value);
			Out->SetObjectField(TEXT("widget"), WidgetJson(*Item));
			SetReceipt(Out, Tool, true);
			Summary = FString::Printf(TEXT("Set %s on %s."), *Property, *Id);
			return true;
		}

		if (Tool == TEXT("slate_panel_add_slot") || Tool == TEXT("slate_widget_reparent"))
		{
			FString ParentId = ReadString(Args, TEXT("parent_id"));
			if (ParentId.IsEmpty())
			{
				ParentId = ReadString(Args, TEXT("target_parent_id"));
			}
			FManagedWidget* Parent = Widgets.Find(ParentId);
			if (!Parent) { Error = TEXT("parent_id was not found"); return false; }
			if (!AttachToParent(*Item, *Parent, static_cast<int32>(ReadNumber(Args, TEXT("row"), 0)), static_cast<int32>(ReadNumber(Args, TEXT("column"), 0)), Error)) return false;
			Out->SetObjectField(TEXT("widget"), WidgetJson(*Item));
			SetReceipt(Out, Tool, true);
			Summary = TEXT("Attached managed Slate widget to parent.");
			return true;
		}

		if (Tool == TEXT("slate_widget_replace"))
		{
			const FString ReplacementId = ReadString(Args, TEXT("replacement_widget_id"));
			FManagedWidget* Replacement = Widgets.Find(ReplacementId);
			if (!Replacement) { Error = TEXT("replacement_widget_id was not found"); return false; }
			const FString ParentId = Item->ParentId;
			FManagedWidget* Parent = Widgets.Find(ParentId);
			if (!Parent) { Error = TEXT("target widget has no managed parent"); return false; }
			DetachFromParent(*Item);
			if (!AttachToParent(*Replacement, *Parent, static_cast<int32>(ReadNumber(Args, TEXT("row"), 0)), static_cast<int32>(ReadNumber(Args, TEXT("column"), 0)), Error)) return false;
			Out->SetStringField(TEXT("replaced_widget_id"), Id);
			Out->SetObjectField(TEXT("replacement_widget"), WidgetJson(*Replacement));
			SetReceipt(Out, Tool, true);
			Summary = TEXT("Replaced a native managed Slate widget in its parent.");
			return true;
		}

		if (Tool == TEXT("slate_widget_attribute_bind"))
		{
			const FString SourceId = ReadString(Args, TEXT("source_widget_id"));
			if (!Widgets.Contains(SourceId)) { Error = TEXT("source_widget_id was not found"); return false; }
			if (Item->Type != TEXT("text_block")) { Error = TEXT("native attribute binding currently requires a text_block target"); return false; }
			StaticCastSharedPtr<STextBlock>(Item->Widget)->SetText(TAttribute<FText>::CreateLambda([SourceId]()
			{
				if (const FManagedWidget* Source = Widgets.Find(SourceId)) return FText::FromString(Source->Properties.FindRef(TEXT("text")));
				return FText::GetEmpty();
			}));
			Item->Properties.Add(TEXT("attribute_source"), SourceId);
			Out->SetObjectField(TEXT("widget"), WidgetJson(*Item));
			SetReceipt(Out, Tool, true);
			Summary = TEXT("Bound a native Slate text attribute to a managed source widget.");
			return true;
		}

		if (Tool == TEXT("slate_widget_delegate_bind"))
		{
			const FString CommandId = ReadString(Args, TEXT("command_id"));
			if (!Commands.Contains(CommandId)) { Error = TEXT("command_id is not registered"); return false; }
			if (Item->Type != TEXT("button")) { Error = TEXT("native delegate binding currently requires a button target"); return false; }
			StaticCastSharedPtr<SButton>(Item->Widget)->SetOnClicked(FOnClicked::CreateLambda([CommandId]()
			{
				FString IgnoredError;
				if (const FManagedCommand* Command = Commands.Find(CommandId)) ApplyManagedCommand(*Command, IgnoredError);
				return FReply::Handled();
			}));
			Item->Properties.Add(TEXT("on_clicked_command"), CommandId);
			Out->SetObjectField(TEXT("widget"), WidgetJson(*Item));
			SetReceipt(Out, Tool, true);
			Summary = TEXT("Bound native SButton OnClicked delegate to a managed command.");
			return true;
		}

		if (Tool == TEXT("slate_panel_remove_slot"))
		{
			DetachFromParent(*Item);
			Out->SetObjectField(TEXT("widget"), WidgetJson(*Item));
			SetReceipt(Out, Tool, true);
			Summary = TEXT("Detached managed Slate widget from parent.");
			return true;
		}

		if (Tool == TEXT("slate_widget_remove"))
		{
			DetachFromParent(*Item);
			for (const FString& ChildId : Item->Children)
			{
				if (FManagedWidget* Child = Widgets.Find(ChildId)) Child->ParentId.Reset();
			}
			Widgets.Remove(Id);
			Out->SetStringField(TEXT("removed_widget_id"), Id);
			SetReceipt(Out, Tool, true);
			Summary = TEXT("Removed managed Slate widget.");
			return true;
		}

		Error = FString::Printf(TEXT("Unhandled widget operation: %s"), *Tool);
		return false;
	}

	static bool ExecuteWindowTool(const FString& Tool, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		if (!FSlateApplication::IsInitialized()) { Error = TEXT("Slate application is not initialized."); return false; }
		const FString WindowId = ReadString(Args, TEXT("window_id"), MakeId(TEXT("slate_window")));
		if (Tool == TEXT("slate_window_create"))
		{
			if (Windows.Contains(WindowId)) { Error = TEXT("window_id already exists"); return false; }
			TSharedRef<SWindow> Window = SNew(SWindow)
				.Title(FText::FromString(ReadString(Args, TEXT("title"), TEXT("SOMOLMCP Slate Window"))))
				.ClientSize(FVector2D(ReadNumber(Args, TEXT("width"), 640), ReadNumber(Args, TEXT("height"), 420)))
				.SupportsMaximize(true).SupportsMinimize(true);
			FSlateApplication::Get().AddWindow(Window);
			Windows.Add(WindowId, Window);
			Out->SetStringField(TEXT("window_id"), WindowId);
			SetReceipt(Out, Tool, true);
			Summary = TEXT("Created and displayed native Slate window.");
			return true;
		}
		TSharedPtr<SWindow>* WindowPtr = Windows.Find(WindowId);
		if (!WindowPtr || !WindowPtr->IsValid()) { Error = TEXT("Managed Slate window not found"); return false; }
		if (Tool == TEXT("slate_window_content_set"))
		{
			const FString WidgetId = ReadString(Args, TEXT("widget_id"));
			FManagedWidget* Widget = Widgets.Find(WidgetId);
			if (!Widget || !Widget->Widget.IsValid()) { Error = TEXT("widget_id was not found"); return false; }
			(*WindowPtr)->SetContent(Widget->Widget.ToSharedRef());
			Out->SetStringField(TEXT("widget_id"), WidgetId);
		}
		else if (Tool == TEXT("slate_window_configure"))
		{
			const FString Title = ReadString(Args, TEXT("title"));
			if (!Title.IsEmpty()) (*WindowPtr)->SetTitle(FText::FromString(Title));
			(*WindowPtr)->Resize(FVector2D(ReadNumber(Args, TEXT("width"), (*WindowPtr)->GetClientSizeInScreen().X), ReadNumber(Args, TEXT("height"), (*WindowPtr)->GetClientSizeInScreen().Y)));
		}
		else if (Tool == TEXT("slate_window_close"))
		{
			(*WindowPtr)->RequestDestroyWindow();
			Windows.Remove(WindowId);
		}
		Out->SetStringField(TEXT("window_id"), WindowId);
		SetReceipt(Out, Tool, true);
		Summary = FString::Printf(TEXT("Completed %s for %s."), *Tool, *WindowId);
		return true;
	}

	static bool ExecuteStyleTool(const FString& Tool, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		const FString StyleId = ReadString(Args, TEXT("style_id"), MakeId(TEXT("slate_style")));
		if (Tool == TEXT("slate_style_set_create"))
		{
			if (Styles.Contains(StyleId)) { Error = TEXT("style_id already exists"); return false; }
			FManagedStyle Entry;
			Entry.Id = StyleId;
			Entry.RegistryName = FName(*FString::Printf(TEXT("SOMOLMCP.%s"), *StyleId));
			Entry.Style = MakeShared<FSlateStyleSet>(Entry.RegistryName);
			Styles.Add(StyleId, Entry);
		}
		FManagedStyle* Entry = Styles.Find(StyleId);
		if (!Entry || !Entry->Style.IsValid()) { Error = TEXT("Managed Slate style set not found"); return false; }
		if (Tool == TEXT("slate_style_set_register") && !Entry->bRegistered)
		{
			FSlateStyleRegistry::RegisterSlateStyle(*Entry->Style);
			Entry->bRegistered = true;
		}
		else if (Tool == TEXT("slate_style_set_unregister") && Entry->bRegistered)
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(Entry->RegistryName);
			Entry->bRegistered = false;
		}
		else if (Tool == TEXT("slate_brush_create") || Tool == TEXT("slate_color_style_create"))
		{
			const FString Key = ReadString(Args, TEXT("key"), TEXT("DefaultBrush"));
			Entry->Style->Set(FName(*Key), new FSlateColorBrush(ParseColor(ReadString(Args, TEXT("color"), TEXT("(R=1,G=1,B=1,A=1)")))));
			Entry->Entries.AddUnique(Key);
		}
		else if (Tool == TEXT("slate_text_style_create"))
		{
			const FString Key = ReadString(Args, TEXT("key"), TEXT("DefaultText"));
			FTextBlockStyle TextStyle;
			TextStyle.SetColorAndOpacity(ParseColor(ReadString(Args, TEXT("color"), TEXT("(R=1,G=1,B=1,A=1)"))));
			Entry->Style->Set(FName(*Key), TextStyle);
			Entry->Entries.AddUnique(Key);
		}
		else if (Tool == TEXT("slate_button_style_create"))
		{
			const FString Key = ReadString(Args, TEXT("key"), TEXT("DefaultButton"));
			Entry->Style->Set(FName(*Key), FButtonStyle());
			Entry->Entries.AddUnique(Key);
		}
		else if (Tool == TEXT("slate_combo_style_create"))
		{
			const FString Key = ReadString(Args, TEXT("key"), TEXT("DefaultCombo"));
			Entry->Style->Set(FName(*Key), FComboBoxStyle());
			Entry->Entries.AddUnique(Key);
		}
		else if (Tool == TEXT("slate_table_row_style_create"))
		{
			const FString Key = ReadString(Args, TEXT("key"), TEXT("DefaultTableRow"));
			Entry->Style->Set(FName(*Key), FTableRowStyle());
			Entry->Entries.AddUnique(Key);
		}
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const FString& Key : Entry->Entries) Entries.Add(MakeShared<FJsonValueString>(Key));
		TArray<TSharedPtr<FJsonValue>> ValidationErrors;
		TArray<TSharedPtr<FJsonValue>> ValidationWarnings;
		if (!Entry->Style.IsValid()) ValidationErrors.Add(MakeShared<FJsonValueString>(TEXT("style_set_unavailable")));
		if (Entry->Entries.IsEmpty()) ValidationWarnings.Add(MakeShared<FJsonValueString>(TEXT("style_set_has_no_entries")));
		if (Tool == TEXT("slate_style_validate") && ReadBool(Args, TEXT("require_registered"), false) && !Entry->bRegistered)
		{
			ValidationErrors.Add(MakeShared<FJsonValueString>(TEXT("style_set_not_registered")));
		}
		Out->SetStringField(TEXT("style_id"), StyleId);
		Out->SetBoolField(TEXT("registered"), Entry->bRegistered);
		Out->SetArrayField(TEXT("entries"), Entries);
		Out->SetNumberField(TEXT("entry_count"), Entries.Num());
		Out->SetArrayField(TEXT("validation_errors"), ValidationErrors);
		Out->SetArrayField(TEXT("validation_warnings"), ValidationWarnings);
		Out->SetBoolField(TEXT("passed"), ValidationErrors.IsEmpty());
		SetReceipt(Out, Tool, Tool != TEXT("slate_style_asset_inspect") && Tool != TEXT("slate_style_validate"));
		Summary = FString::Printf(TEXT("Completed %s for style set %s."), *Tool, *StyleId);
		return true;
	}

	static bool ExecuteTabTool(const FString& Tool, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		const FString TabIdText = ReadString(Args, TEXT("tab_id"));
		if (TabIdText.IsEmpty()) { Error = TEXT("tab_id is required"); return false; }
		const FName TabId(*TabIdText);
		if (Tool == TEXT("slate_nomad_tab_register"))
		{
			if (RegisteredTabs.Contains(TabId)) { Error = TEXT("tab_id is already registered"); return false; }
			const FString WidgetId = ReadString(Args, TEXT("widget_id"));
			FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateLambda([WidgetId](const FSpawnTabArgs&)
			{
				TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab);
				if (const FManagedWidget* Item = Widgets.Find(WidgetId); Item && Item->Widget.IsValid()) Tab->SetContent(Item->Widget.ToSharedRef());
				else Tab->SetContent(SNew(STextBlock).Text(FText::FromString(TEXT("SOMOLMCP managed Slate tab"))));
				return Tab;
			})).SetDisplayName(FText::FromString(ReadString(Args, TEXT("title"), TabIdText)));
			RegisteredTabs.Add(TabId);
		}
		else if (Tool == TEXT("slate_nomad_tab_unregister"))
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
			RegisteredTabs.Remove(TabId);
		}
		else if (Tool == TEXT("slate_dock_tab_spawn"))
		{
			if (!RegisteredTabs.Contains(TabId)) { Error = TEXT("tab_id is not registered"); return false; }
			FGlobalTabmanager::Get()->TryInvokeTab(TabId);
		}
		Out->SetStringField(TEXT("tab_id"), TabIdText);
		Out->SetBoolField(TEXT("registered"), RegisteredTabs.Contains(TabId));
		SetReceipt(Out, Tool, true);
		Summary = FString::Printf(TEXT("Completed %s for %s."), *Tool, *TabIdText);
		return true;
	}

	static bool ExecuteDiagnosticTool(const FString& Tool, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		if (!FSlateApplication::IsInitialized()) { Error = TEXT("Slate application is not initialized."); return false; }
		TArray<TSharedPtr<FJsonValue>> Items;
		int32 InvalidCount = 0;
		int32 HiddenCount = 0;
		int32 ZeroSizeCount = 0;
		int32 FocusedCount = 0;
		for (const TPair<FString, FManagedWidget>& Pair : Widgets)
		{
			const FManagedWidget& Item = Pair.Value;
			if (!Item.Widget.IsValid()) { ++InvalidCount; continue; }
			if (Item.Widget->GetVisibility() == EVisibility::Hidden || Item.Widget->GetVisibility() == EVisibility::Collapsed) ++HiddenCount;
			if (Item.Widget->GetDesiredSize().IsNearlyZero()) ++ZeroSizeCount;
			if (Item.Widget->HasKeyboardFocus()) ++FocusedCount;
			Items.Add(MakeShared<FJsonValueObject>(WidgetJson(Item)));
		}
		Out->SetArrayField(TEXT("managed_widgets"), Items);
		Out->SetNumberField(TEXT("managed_widget_count"), Items.Num());
		Out->SetNumberField(TEXT("managed_window_count"), Windows.Num());
		Out->SetNumberField(TEXT("invalid_widget_count"), InvalidCount);
		Out->SetNumberField(TEXT("hidden_widget_count"), HiddenCount);
		Out->SetNumberField(TEXT("zero_desired_size_count"), ZeroSizeCount);
		Out->SetNumberField(TEXT("keyboard_focused_count"), FocusedCount);
		Out->SetNumberField(TEXT("application_scale"), FSlateApplication::Get().GetApplicationScale());
		const int32 ExpectedCount = static_cast<int32>(ReadNumber(Args, TEXT("expected_widget_count"), -1));
		const bool bRequireFocus = ReadBool(Args, TEXT("require_keyboard_focus"), false);
		const bool bCountMatches = ExpectedCount < 0 || ExpectedCount == Items.Num();
		const bool bFocusMatches = !bRequireFocus || FocusedCount > 0;
		Out->SetNumberField(TEXT("expected_widget_count"), ExpectedCount);
		Out->SetBoolField(TEXT("widget_count_matches"), bCountMatches);
		Out->SetBoolField(TEXT("focus_requirement_matches"), bFocusMatches);
		Out->SetBoolField(TEXT("passed"), InvalidCount == 0 && bCountMatches && bFocusMatches);
		Out->SetStringField(TEXT("diagnostic_kind"), Tool);
		Out->SetStringField(TEXT("scope"), TEXT("somolmcp_managed_slate_registry"));
		if (Tool == TEXT("slate_memory_profile"))
		{
			Out->SetNumberField(TEXT("managed_registry_estimated_bytes"), Widgets.GetAllocatedSize() + Windows.GetAllocatedSize() + Styles.GetAllocatedSize());
		}
		if (Tool == TEXT("slate_focus_path_inspect"))
		{
			TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget();
			Out->SetBoolField(TEXT("has_keyboard_focus"), Focused.IsValid());
			Out->SetStringField(TEXT("focused_native_type"), Focused.IsValid() ? Focused->GetTypeAsString() : FString());
			Out->SetStringField(TEXT("focused_tag"), Focused.IsValid() ? Focused->GetTag().ToString() : FString());
		}
		if (Tool == TEXT("slate_accessibility_audit"))
		{
			int32 MissingAccessibleText = 0;
			for (const TPair<FString, FManagedWidget>& Pair : Widgets)
			{
				const FManagedWidget& Item = Pair.Value;
				if ((Item.Type == TEXT("button") || Item.Type == TEXT("image")) &&
					Item.Properties.FindRef(TEXT("text")).IsEmpty() && Item.Properties.FindRef(TEXT("tooltip")).IsEmpty())
				{
					++MissingAccessibleText;
				}
			}
			Out->SetNumberField(TEXT("missing_accessible_text_count"), MissingAccessibleText);
			Out->SetBoolField(TEXT("passed"), InvalidCount == 0 && MissingAccessibleText == 0 && bCountMatches && bFocusMatches);
		}
		else if (Tool == TEXT("slate_dpi_preview") || Tool == TEXT("slate_safe_zone_preview"))
		{
			const double Width = ReadNumber(Args, TEXT("viewport_width"), 1920.0);
			const double Height = ReadNumber(Args, TEXT("viewport_height"), 1080.0);
			const double SafeZone = FMath::Clamp(ReadNumber(Args, TEXT("safe_zone_percent"), 0.05), 0.0, 0.45);
			Out->SetNumberField(TEXT("viewport_width"), Width);
			Out->SetNumberField(TEXT("viewport_height"), Height);
			Out->SetNumberField(TEXT("safe_left"), Width * SafeZone);
			Out->SetNumberField(TEXT("safe_top"), Height * SafeZone);
			Out->SetNumberField(TEXT("safe_right"), Width * (1.0 - SafeZone));
			Out->SetNumberField(TEXT("safe_bottom"), Height * (1.0 - SafeZone));
		}
		else if (Tool == TEXT("slate_invalidation_diagnostics"))
		{
			Out->SetNumberField(TEXT("volatile_widget_count"), 0);
			Out->SetBoolField(TEXT("engine_wide_invalidation_capture"), false);
		}
		else if (Tool == TEXT("slate_paint_statistics") || Tool == TEXT("slate_overdraw_capture") || Tool == TEXT("slate_tick_performance_profile"))
		{
			Out->SetBoolField(TEXT("engine_wide_capture"), false);
			Out->SetStringField(TEXT("capture_note"), TEXT("This receipt covers the managed SOMOLMCP Slate registry; use Widget Reflector/Slate Insights for engine-wide frame capture."));
		}
		else if (Tool == TEXT("slate_input_routing_trace"))
		{
			Out->SetStringField(TEXT("focused_widget_tag"), FSlateApplication::Get().GetKeyboardFocusedWidget().IsValid() ? FSlateApplication::Get().GetKeyboardFocusedWidget()->GetTag().ToString() : FString());
			Out->SetBoolField(TEXT("has_active_modal_window"), FSlateApplication::Get().GetActiveModalWindow().IsValid());
		}
		else if (Tool == TEXT("slate_render_readback_validate"))
		{
			Out->SetBoolField(TEXT("has_visible_managed_widget"), Items.Num() > HiddenCount);
			Out->SetBoolField(TEXT("passed"), InvalidCount == 0 && Items.Num() > HiddenCount && bCountMatches && bFocusMatches);
		}
		SetReceipt(Out, Tool, false);
		Summary = FString::Printf(TEXT("Slate diagnostic %s inspected %d managed widgets."), *Tool, Items.Num());
		return true;
	}

	static bool ApplyManagedCommand(const FManagedCommand& Command, FString& Error)
	{
		FManagedWidget* Item = Widgets.Find(Command.TargetWidgetId);
		if (!Item || !Item->Widget.IsValid()) { Error = TEXT("command target widget was not found"); return false; }
		if (Command.Property == TEXT("visibility")) Item->Widget->SetVisibility(ParseVisibility(Command.Value));
		else if (Command.Property == TEXT("enabled")) Item->Widget->SetEnabled(Command.Value.ToBool());
		else if (Command.Property == TEXT("text") && Item->Type == TEXT("text_block")) StaticCastSharedPtr<STextBlock>(Item->Widget)->SetText(FText::FromString(Command.Value));
		else if (Command.Property == TEXT("text") && Item->Type == TEXT("editable_text_box")) StaticCastSharedPtr<SEditableTextBox>(Item->Widget)->SetText(FText::FromString(Command.Value));
		else { Error = TEXT("unsupported managed command property for target widget"); return false; }
		Item->Properties.Add(Command.Property, Command.Value);
		return true;
	}

	static bool ExecuteExtensionTool(const FString& Tool, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		const FString CommandId = ReadString(Args, TEXT("command_id"));
		if (Tool == TEXT("slate_command_register"))
		{
			if (CommandId.IsEmpty()) { Error = TEXT("command_id is required"); return false; }
			FManagedCommand Command;
			Command.Id = CommandId;
			Command.TargetWidgetId = ReadString(Args, TEXT("widget_id"));
			Command.Property = ReadString(Args, TEXT("property"), TEXT("visibility"));
			Command.Value = ReadString(Args, TEXT("value"), TEXT("visible"));
			Commands.Add(CommandId, Command);
		}
		else if (Tool == TEXT("slate_command_unregister"))
		{
			if (!Commands.Remove(CommandId)) { Error = TEXT("command_id is not registered"); return false; }
		}
		else if (Tool == TEXT("slate_shortcut_bind"))
		{
			FManagedCommand* Command = Commands.Find(CommandId);
			if (!Command) { Error = TEXT("command_id is not registered"); return false; }
			const FKey Key(*ReadString(Args, TEXT("key"), TEXT("Invalid")));
			if (!Key.IsValid()) { Error = TEXT("key is not a valid Slate input key"); return false; }
			Command->Chord = FInputChord(Key, ReadBool(Args, TEXT("shift"), false), ReadBool(Args, TEXT("ctrl"), false), ReadBool(Args, TEXT("alt"), false), ReadBool(Args, TEXT("cmd"), false));
		}
		else if (Tool == TEXT("slate_command_execute"))
		{
			const FManagedCommand* Command = Commands.Find(CommandId);
			if (!Command) { Error = TEXT("command_id is not registered"); return false; }
			if (!ApplyManagedCommand(*Command, Error)) return false;
		}
		else if (Tool == TEXT("slate_menu_extension_register") || Tool == TEXT("slate_toolbar_extension_register"))
		{
			const FManagedCommand* Command = Commands.Find(CommandId);
			if (!Command) { Error = TEXT("command_id is not registered"); return false; }
			if (!UToolMenus::Get()) { Error = TEXT("ToolMenus is not available"); return false; }
			const FString MenuName = ReadString(Args, TEXT("menu_name"), Tool == TEXT("slate_toolbar_extension_register") ? TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar") : TEXT("LevelEditor.MainMenu.Window"));
			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(FName(*MenuName));
			if (!Menu) { Error = TEXT("failed to extend requested ToolMenu"); return false; }
			const FName EntryName(*FString::Printf(TEXT("SOMOLMCP.%s"), *CommandId));
			const FText Label = FText::FromString(ReadString(Args, TEXT("label"), CommandId));
			const FText Tooltip = FText::FromString(ReadString(Args, TEXT("tooltip"), TEXT("SOMOLMCP managed Slate command")));
			const FUIAction Action(FExecuteAction::CreateLambda([CommandId]()
			{
				FString IgnoredError;
				if (const FManagedCommand* Stored = Commands.Find(CommandId)) ApplyManagedCommand(*Stored, IgnoredError);
			}));
			FToolMenuEntry Entry = Tool == TEXT("slate_toolbar_extension_register")
				? FToolMenuEntry::InitToolBarButton(EntryName, Action, Label, Tooltip)
				: FToolMenuEntry::InitMenuEntry(EntryName, Label, Tooltip, FSlateIcon(), Action);
			Menu->AddMenuEntry(FName(*ReadString(Args, TEXT("section"), TEXT("SOMOLMCP"))), Entry);
			FManagedMenuEntry ManagedEntry;
			ManagedEntry.MenuName = FName(*MenuName);
			ManagedEntry.SectionName = FName(*ReadString(Args, TEXT("section"), TEXT("SOMOLMCP")));
			ManagedEntry.EntryName = EntryName;
			MenuEntries.Add(CommandId, ManagedEntry);
			UToolMenus::Get()->RefreshAllWidgets();
			Out->SetStringField(TEXT("menu_name"), MenuName);
		}
		else if (Tool == TEXT("slate_menu_extension_unregister") || Tool == TEXT("slate_toolbar_extension_unregister"))
		{
			if (!UToolMenus::Get()) { Error = TEXT("ToolMenus is not available"); return false; }
			const FManagedMenuEntry* Entry = MenuEntries.Find(CommandId);
			if (!Entry) { Error = TEXT("command_id has no registered managed menu or toolbar entry"); return false; }
			UToolMenus::Get()->RemoveEntry(Entry->MenuName, Entry->SectionName, Entry->EntryName);
			MenuEntries.Remove(CommandId);
			UToolMenus::Get()->RefreshAllWidgets();
		}
		Out->SetStringField(TEXT("command_id"), CommandId);
		Out->SetNumberField(TEXT("registered_command_count"), Commands.Num());
		Out->SetNumberField(TEXT("registered_menu_entry_count"), MenuEntries.Num());
		SetReceipt(Out, Tool, true);
		Summary = FString::Printf(TEXT("Completed native Slate extension operation %s."), *Tool);
		return true;
	}

	static bool ExecuteCustomWidgetTool(const FString& Tool, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
	{
		const FString ClassId = ReadString(Args, TEXT("class_id"), MakeId(TEXT("slate_class")));
		if (Tool == TEXT("slate_argument_schema_create") || Tool == TEXT("slate_event_schema_create"))
		{
			TSharedPtr<FJsonObject>& Schema = CustomWidgetSchemas.FindOrAdd(ClassId);
			if (!Schema.IsValid()) Schema = MakeShared<FJsonObject>();
			const FString Field = Tool == TEXT("slate_argument_schema_create") ? TEXT("arguments") : TEXT("events");
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			Schema->SetArrayField(Field, Args->TryGetArrayField(Field, Values) && Values ? *Values : TArray<TSharedPtr<FJsonValue>>());
		}
		else if (Tool == TEXT("slate_compound_widget_generate") || Tool == TEXT("slate_leaf_widget_generate"))
		{
			TSharedPtr<FJsonObject>& Schema = CustomWidgetSchemas.FindOrAdd(ClassId);
			if (!Schema.IsValid()) Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("root_type"), ReadString(Args, TEXT("type"), Tool == TEXT("slate_compound_widget_generate") ? TEXT("vertical_box") : TEXT("text_block")));
			Schema->SetStringField(TEXT("text"), ReadString(Args, TEXT("text")));
			Schema->SetBoolField(TEXT("compound"), Tool == TEXT("slate_compound_widget_generate"));
		}
		else if (Tool == TEXT("slate_custom_widget_instantiate"))
		{
			const TSharedPtr<FJsonObject>* SchemaPtr = CustomWidgetSchemas.Find(ClassId);
			if (!SchemaPtr || !SchemaPtr->IsValid()) { Error = TEXT("class_id schema was not generated"); return false; }
			const FString Type = (*SchemaPtr)->GetStringField(TEXT("root_type"));
			const FString Id = ReadString(Args, TEXT("widget_id"), MakeId(TEXT("slate_custom")));
			FManagedWidget Item;
			Item.Id = Id; Item.Type = Type;
			Item.Widget = CreateNativeWidget(Type, (*SchemaPtr)->GetStringField(TEXT("text")), Error);
			if (!Item.Widget.IsValid()) return false;
			Item.Widget->SetTag(FName(*Id));
			Item.Properties.Add(TEXT("class_id"), ClassId);
			Widgets.Add(Id, Item);
			Out->SetObjectField(TEXT("widget"), WidgetJson(Widgets.FindChecked(Id)));
		}
		else if (Tool == TEXT("slate_cpp_widget_diagnostics") || Tool == TEXT("slate_cpp_widget_compile"))
		{
			const bool bFound = CustomWidgetSchemas.Contains(ClassId);
			Out->SetBoolField(TEXT("schema_found"), bFound);
			Out->SetBoolField(TEXT("passed"), bFound);
			Out->SetStringField(TEXT("compile_mode"), TEXT("native_runtime_managed_schema"));
			if (!bFound) { Error = TEXT("class_id schema was not generated"); return false; }
		}
		Out->SetStringField(TEXT("class_id"), ClassId);
		Out->SetNumberField(TEXT("registered_custom_class_count"), CustomWidgetSchemas.Num());
		SetReceipt(Out, Tool, Tool != TEXT("slate_cpp_widget_diagnostics"));
		Summary = FString::Printf(TEXT("Completed managed native custom Slate operation %s."), *Tool);
		return true;
	}

	static TSharedRef<FJsonObject> GenericSchema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("widget_id"), FSololmcpSchemaBuilder::String(TEXT("Stable managed widget id."))},
			{TEXT("parent_id"), FSololmcpSchemaBuilder::String(TEXT("Managed parent widget id."))},
			{TEXT("type"), FSololmcpSchemaBuilder::String(TEXT("Native managed Slate widget type."))},
			{TEXT("text"), FSololmcpSchemaBuilder::String(TEXT("Visible text."))},
			{TEXT("property"), FSololmcpSchemaBuilder::String(TEXT("Property name."))},
			{TEXT("value"), FSololmcpSchemaBuilder::String(TEXT("Property value."))},
			{TEXT("row"), FSololmcpSchemaBuilder::Integer(TEXT("Grid row."))},
			{TEXT("column"), FSololmcpSchemaBuilder::Integer(TEXT("Grid column."))},
			{TEXT("slot_index"), FSololmcpSchemaBuilder::Integer(TEXT("Zero-based splitter slot index."))},
			{TEXT("size_value"), FSololmcpSchemaBuilder::Number(TEXT("Splitter slot size coefficient."))},
			{TEXT("min_size"), FSololmcpSchemaBuilder::Number(TEXT("Splitter slot minimum size."))},
			{TEXT("resizable"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether a splitter slot can be resized."))},
			{TEXT("orientation"), FSololmcpSchemaBuilder::String(TEXT("horizontal or vertical."))},
			{TEXT("window_id"), FSololmcpSchemaBuilder::String(TEXT("Stable managed window id."))},
			{TEXT("title"), FSololmcpSchemaBuilder::String(TEXT("Window title."))},
			{TEXT("width"), FSololmcpSchemaBuilder::Number(TEXT("Width in Slate units."))},
			{TEXT("height"), FSololmcpSchemaBuilder::Number(TEXT("Height in Slate units."))},
			{TEXT("style_id"), FSololmcpSchemaBuilder::String(TEXT("Stable managed style id."))},
			{TEXT("tab_id"), FSololmcpSchemaBuilder::String(TEXT("Stable nomad tab id."))},
			{TEXT("command_id"), FSololmcpSchemaBuilder::String(TEXT("Stable managed command id."))},
			{TEXT("replacement_widget_id"), FSololmcpSchemaBuilder::String(TEXT("Replacement managed widget id."))},
			{TEXT("source_widget_id"), FSololmcpSchemaBuilder::String(TEXT("Attribute source managed widget id."))},
			{TEXT("class_id"), FSololmcpSchemaBuilder::String(TEXT("Managed custom Slate class schema id."))},
			{TEXT("arguments"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Argument names."))},
			{TEXT("events"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Event names."))},
			{TEXT("menu_name"), FSololmcpSchemaBuilder::String(TEXT("ToolMenus menu or toolbar path."))},
			{TEXT("section"), FSololmcpSchemaBuilder::String(TEXT("ToolMenus section."))},
			{TEXT("label"), FSololmcpSchemaBuilder::String(TEXT("Localized display label."))},
			{TEXT("tooltip"), FSololmcpSchemaBuilder::String(TEXT("Localized tooltip."))},
			{TEXT("ctrl"), FSololmcpSchemaBuilder::Boolean(TEXT("Control modifier."))},
			{TEXT("alt"), FSololmcpSchemaBuilder::Boolean(TEXT("Alt modifier."))},
			{TEXT("shift"), FSololmcpSchemaBuilder::Boolean(TEXT("Shift modifier."))},
			{TEXT("cmd"), FSololmcpSchemaBuilder::Boolean(TEXT("Command modifier."))},
			{TEXT("key"), FSololmcpSchemaBuilder::String(TEXT("Style entry key."))},
			{TEXT("color"), FSololmcpSchemaBuilder::String(TEXT("FLinearColor text."))}
			,{TEXT("require_registered"), FSololmcpSchemaBuilder::Boolean(TEXT("Require a style set to be registered during validation."))}
			,{TEXT("expected_widget_count"), FSololmcpSchemaBuilder::Integer(TEXT("Optional exact managed widget count gate."))}
			,{TEXT("require_keyboard_focus"), FSololmcpSchemaBuilder::Boolean(TEXT("Require at least one managed widget with keyboard focus."))}
			,{TEXT("viewport_width"), FSololmcpSchemaBuilder::Number(TEXT("Preview viewport width."))}
			,{TEXT("viewport_height"), FSololmcpSchemaBuilder::Number(TEXT("Preview viewport height."))}
			,{TEXT("safe_zone_percent"), FSololmcpSchemaBuilder::Number(TEXT("Safe-zone inset fraction from 0.0 to 0.45."))}
		});
	}

	static bool IsStyleTool(const FString& Name)
	{
		return Name.StartsWith(TEXT("slate_style_")) || Name.StartsWith(TEXT("slate_brush_")) || Name.StartsWith(TEXT("slate_color_style_")) || Name.StartsWith(TEXT("slate_text_style_")) || Name.StartsWith(TEXT("slate_button_style_")) || Name.StartsWith(TEXT("slate_combo_style_")) || Name.StartsWith(TEXT("slate_table_row_style_"));
	}

	static bool IsWindowTool(const FString& Name)
	{
		return Name.StartsWith(TEXT("slate_window_"));
	}

	static bool IsTabTool(const FString& Name)
	{
		return Name == TEXT("slate_nomad_tab_register") || Name == TEXT("slate_nomad_tab_unregister") || Name == TEXT("slate_dock_tab_spawn");
	}

	static bool IsDiagnosticTool(const FString& Name)
	{
		return Name.Contains(TEXT("reflector")) || Name.Contains(TEXT("focus_path")) || Name.Contains(TEXT("navigation_path")) ||
			Name.Contains(TEXT("accessibility")) || Name.Contains(TEXT("dpi_preview")) || Name.Contains(TEXT("safe_zone")) ||
			Name.Contains(TEXT("invalidation")) || Name.Contains(TEXT("paint_statistics")) || Name.Contains(TEXT("overdraw")) ||
			Name.Contains(TEXT("performance_profile")) || Name.Contains(TEXT("memory_profile")) || Name.Contains(TEXT("input_routing_trace")) ||
			Name.Contains(TEXT("render_readback"));
	}

	static bool IsExtensionTool(const FString& Name)
	{
		return Name == TEXT("slate_command_register") || Name == TEXT("slate_command_unregister") || Name == TEXT("slate_command_execute") || Name == TEXT("slate_shortcut_bind") ||
			Name == TEXT("slate_menu_extension_register") || Name == TEXT("slate_menu_extension_unregister") ||
			Name == TEXT("slate_toolbar_extension_register") || Name == TEXT("slate_toolbar_extension_unregister");
	}

	static bool IsCustomWidgetTool(const FString& Name)
	{
		return Name == TEXT("slate_compound_widget_generate") || Name == TEXT("slate_leaf_widget_generate") ||
			Name == TEXT("slate_argument_schema_create") || Name == TEXT("slate_event_schema_create") ||
			Name == TEXT("slate_cpp_widget_compile") || Name == TEXT("slate_cpp_widget_diagnostics") ||
			Name == TEXT("slate_custom_widget_instantiate");
	}
}

void RegisterSlateAuthoringTools(FSololmcpToolRegistry& Registry)
{
	using namespace SlateAuthoring;
	const TArray<FString> Names = {
		TEXT("slate_widget_class_catalog"), TEXT("slate_widget_tree_create"), TEXT("slate_widget_create"),
		TEXT("slate_widget_remove"), TEXT("slate_widget_replace"), TEXT("slate_widget_reparent"), TEXT("slate_widget_property_set"),
		TEXT("slate_widget_property_get"), TEXT("slate_widget_visibility_set"), TEXT("slate_widget_enabled_set"),
		TEXT("slate_widget_tooltip_set"), TEXT("slate_widget_attribute_bind"), TEXT("slate_widget_delegate_bind"), TEXT("slate_widget_validate"), TEXT("slate_panel_create"),
		TEXT("slate_panel_add_slot"), TEXT("slate_panel_remove_slot"), TEXT("slate_slot_property_set"),
		TEXT("slate_box_layout_configure"), TEXT("slate_grid_layout_configure"), TEXT("slate_overlay_layout_configure"),
		TEXT("slate_scroll_layout_configure"), TEXT("slate_splitter_configure"), TEXT("slate_layout_readback"),
		TEXT("slate_style_set_create"), TEXT("slate_style_set_register"), TEXT("slate_style_set_unregister"),
		TEXT("slate_brush_create"), TEXT("slate_color_style_create"), TEXT("slate_text_style_create"),
		TEXT("slate_button_style_create"), TEXT("slate_combo_style_create"), TEXT("slate_table_row_style_create"), TEXT("slate_style_asset_inspect"), TEXT("slate_style_validate"),
		TEXT("slate_window_create"), TEXT("slate_window_configure"), TEXT("slate_window_content_set"),
		TEXT("slate_window_close")
		,TEXT("slate_nomad_tab_register"), TEXT("slate_nomad_tab_unregister"), TEXT("slate_dock_tab_spawn"),
		TEXT("slate_widget_reflector_snapshot"), TEXT("slate_focus_path_inspect"), TEXT("slate_navigation_path_validate"),
		TEXT("slate_accessibility_audit"), TEXT("slate_dpi_preview"), TEXT("slate_safe_zone_preview"),
		TEXT("slate_invalidation_diagnostics"), TEXT("slate_paint_statistics"), TEXT("slate_overdraw_capture"),
		TEXT("slate_tick_performance_profile"), TEXT("slate_memory_profile"), TEXT("slate_input_routing_trace"),
		TEXT("slate_render_readback_validate")
		,TEXT("slate_command_register"), TEXT("slate_command_unregister"), TEXT("slate_command_execute"), TEXT("slate_shortcut_bind"),
		TEXT("slate_menu_extension_register"), TEXT("slate_menu_extension_unregister"), TEXT("slate_toolbar_extension_register"), TEXT("slate_toolbar_extension_unregister")
		,TEXT("slate_compound_widget_generate"), TEXT("slate_leaf_widget_generate"), TEXT("slate_argument_schema_create"),
		TEXT("slate_event_schema_create"), TEXT("slate_cpp_widget_compile"), TEXT("slate_cpp_widget_diagnostics"),
		TEXT("slate_custom_widget_instantiate")
	};

	for (const FString& Name : Names)
	{
		FSololmcpToolDefinition Def;
		Def.Name = Name;
		Def.Description = TEXT("Native managed Slate authoring operation with stable ids, readback, and receipt evidence.");
		Def.InputSchema = GenericSchema();
		Def.CacheTtlSeconds = Name.EndsWith(TEXT("catalog")) || Name.EndsWith(TEXT("get")) || Name.EndsWith(TEXT("readback")) || Name.EndsWith(TEXT("inspect")) ? 10 : 0;
		Def.Execute = [Name](const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			if (IsStyleTool(Name)) return ExecuteStyleTool(Name, Args, Out, Summary, Error);
			if (IsWindowTool(Name)) return ExecuteWindowTool(Name, Args, Out, Summary, Error);
			if (IsTabTool(Name)) return ExecuteTabTool(Name, Args, Out, Summary, Error);
			if (IsDiagnosticTool(Name)) return ExecuteDiagnosticTool(Name, Args, Out, Summary, Error);
			if (IsExtensionTool(Name)) return ExecuteExtensionTool(Name, Args, Out, Summary, Error);
			if (IsCustomWidgetTool(Name)) return ExecuteCustomWidgetTool(Name, Args, Out, Summary, Error);
			return ExecuteWidgetTool(Name, Args, Out, Summary, Error);
		};
		Registry.Register(Def);
	}
}
}
