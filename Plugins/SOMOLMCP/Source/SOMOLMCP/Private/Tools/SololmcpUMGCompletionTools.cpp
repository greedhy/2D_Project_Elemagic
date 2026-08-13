// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "Services/SololmcpEditorServices.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace UE::SOMOLMCP
{
namespace UmgCompletion
{
	static FString ReadString(const TSharedRef<FJsonObject>& Args, const TCHAR* Name)
	{
		FString Value;
		Args->TryGetStringField(Name, Value);
		return Value;
	}

	static bool ReadBool(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, bool Fallback)
	{
		bool Value = Fallback;
		Args->TryGetBoolField(Name, Value);
		return Value;
	}

	static bool GetObject(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, TSharedPtr<FJsonObject>& Out)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (!Args->TryGetObjectField(Name, Found) || !Found || !Found->IsValid()) return false;
		Out = *Found;
		return true;
	}

	static bool ResolveTarget(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		UWidgetBlueprint*& OutBlueprint,
		UWidget*& OutWidget,
		FString& OutError,
		bool bWidgetRequired = true)
	{
		const FString AssetPath = ReadString(Args, TEXT("asset_path"));
		if (AssetPath.IsEmpty()) { OutError = TEXT("asset_path is required"); return false; }
		OutBlueprint = Cast<UWidgetBlueprint>(Context.Services.LoadAsset(AssetPath, OutError));
		if (!OutBlueprint || !OutBlueprint->WidgetTree)
		{
			OutError = TEXT("asset_path must resolve to a Widget Blueprint with a WidgetTree");
			return false;
		}
		const FString WidgetName = ReadString(Args, TEXT("widget_name"));
		if (WidgetName.IsEmpty())
		{
			OutWidget = OutBlueprint->WidgetTree->RootWidget.Get();
		}
		else
		{
			OutWidget = OutBlueprint->WidgetTree->FindWidget(*WidgetName);
		}
		if (bWidgetRequired && !OutWidget)
		{
			OutError = WidgetName.IsEmpty() ? TEXT("Widget Blueprint has no root widget") : TEXT("widget_name was not found in the WidgetTree");
			return false;
		}
		return true;
	}

	static TSharedRef<FJsonObject> ExportProperties(UObject* Object)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Object) return Result;
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It && Count < 256; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance)) continue;
			if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;
			FString Value;
			Property->ExportTextItem_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
			if (Value.Len() > 4096) Value = Value.Left(4096) + TEXT("...<truncated>");
			Result->SetStringField(Property->GetName(), Value);
			++Count;
		}
		Result->SetNumberField(TEXT("_exported_property_count"), Count);
		Result->SetStringField(TEXT("_object_class"), Object->GetClass()->GetPathName());
		return Result;
	}

	static void SetReceipt(TSharedRef<FJsonObject>& Out, const FString& Tool, const UWidgetBlueprint* Blueprint, const UWidget* Widget, bool bMutation)
	{
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetStringField(TEXT("tool"), Tool);
		Out->SetStringField(TEXT("implementation"), TEXT("native_umg_editor_write_readback"));
		Out->SetStringField(TEXT("receipt_id"), FString::Printf(TEXT("umg_receipt_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)));
		Out->SetStringField(TEXT("asset_path"), Blueprint ? Blueprint->GetPathName() : FString());
		Out->SetStringField(TEXT("widget_name"), Widget ? Widget->GetName() : FString());
		Out->SetBoolField(TEXT("mutation_performed"), bMutation);
	}

	static bool ApplyTargetProperties(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		UWidgetBlueprint* Blueprint,
		UWidget* Widget,
		TSharedRef<FJsonObject>& Out,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> Properties;
		TSharedPtr<FJsonObject> SlotProperties;
		const bool bHasWidgetProperties = GetObject(Args, TEXT("properties"), Properties);
		const bool bHasSlotProperties = GetObject(Args, TEXT("slot_properties"), SlotProperties);
		if (!bHasWidgetProperties && !bHasSlotProperties)
		{
			OutError = TEXT("properties or slot_properties is required for this mutation");
			return false;
		}
		if (bHasSlotProperties && !Widget->Slot)
		{
			OutError = TEXT("slot_properties was supplied but the widget has no panel slot");
			return false;
		}

		const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "UmgSemanticConfigure", "SOMOLMCP Configure UMG Widget"));
		Blueprint->Modify();
		Widget->Modify();
		if (bHasWidgetProperties && !Context.Services.ApplyProperties(Widget, Properties, OutError)) return false;
		if (bHasSlotProperties)
		{
			Widget->Slot->Modify();
			if (!Context.Services.ApplyProperties(Widget->Slot, SlotProperties, OutError)) return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();
		Out->SetObjectField(TEXT("widget_properties_readback"), ExportProperties(Widget));
		if (Widget->Slot) Out->SetObjectField(TEXT("slot_properties_readback"), ExportProperties(Widget->Slot));
		return true;
	}

	static bool ValidateDesigner(UWidgetBlueprint* Blueprint, TSharedRef<FJsonObject>& Out)
	{
		TArray<UWidget*> Widgets;
		Blueprint->WidgetTree->GetAllWidgets(Widgets);
		TSet<FName> Names;
		int32 NullWidgets = 0;
		int32 DuplicateNames = 0;
		int32 OrphanWidgets = 0;
		for (UWidget* Widget : Widgets)
		{
			if (!Widget) { ++NullWidgets; continue; }
			if (Names.Contains(Widget->GetFName())) ++DuplicateNames;
			Names.Add(Widget->GetFName());
			if (Widget != Blueprint->WidgetTree->RootWidget && !Widget->GetParent() && !Widget->Slot) ++OrphanWidgets;
		}
		Out->SetNumberField(TEXT("widget_count"), Widgets.Num());
		Out->SetNumberField(TEXT("null_widget_count"), NullWidgets);
		Out->SetNumberField(TEXT("duplicate_name_count"), DuplicateNames);
		Out->SetNumberField(TEXT("orphan_widget_count"), OrphanWidgets);
		Out->SetBoolField(TEXT("has_root_widget"), Blueprint->WidgetTree->RootWidget != nullptr);
		const bool bPassed = Blueprint->WidgetTree->RootWidget && NullWidgets == 0 && DuplicateNames == 0 && OrphanWidgets == 0;
		Out->SetBoolField(TEXT("passed"), bPassed);
		return bPassed;
	}

	static bool ExecuteTool(
		const FString& Tool,
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Args,
		TSharedRef<FJsonObject>& Out,
		FString& Summary,
		FString& Error)
	{
		UWidgetBlueprint* Blueprint = nullptr;
		UWidget* Widget = nullptr;
		const bool bBlueprintOnly = Tool == TEXT("umg_widget_designer_validate") || Tool == TEXT("umg_widget_compile_delivery_gate") || Tool == TEXT("umg_widget_save_verified") || Tool == TEXT("umg_widget_responsive_matrix_validate");
		if (!ResolveTarget(Context, Args, Blueprint, Widget, Error, !bBlueprintOnly)) return false;

		if (Tool == TEXT("umg_widget_property_readback"))
		{
			Out->SetObjectField(TEXT("properties"), ExportProperties(Widget));
			SetReceipt(Out, Tool, Blueprint, Widget, false);
			Summary = TEXT("Read back public editable UMG widget properties.");
			return true;
		}
		if (Tool == TEXT("umg_widget_slot_readback"))
		{
			if (!Widget->Slot) { Error = TEXT("widget has no panel slot"); return false; }
			Out->SetObjectField(TEXT("slot_properties"), ExportProperties(Widget->Slot));
			SetReceipt(Out, Tool, Blueprint, Widget, false);
			Summary = TEXT("Read back UMG panel slot properties.");
			return true;
		}
		if (Tool == TEXT("umg_widget_designer_validate"))
		{
			ValidateDesigner(Blueprint, Out);
			SetReceipt(Out, Tool, Blueprint, Blueprint->WidgetTree->RootWidget, false);
			Summary = TEXT("Validated WidgetTree structure, names, root, and orphan state.");
			return true;
		}
		if (Tool == TEXT("umg_widget_accessibility_validate"))
		{
			TArray<UWidget*> Widgets;
			Blueprint->WidgetTree->GetAllWidgets(Widgets);
			int32 MissingOverride = 0;
			for (UWidget* Candidate : Widgets)
			{
				if (!Candidate) continue;
				const FString ClassName = Candidate->GetClass()->GetName();
				const bool bInteractive = ClassName.Contains(TEXT("Button")) || ClassName.Contains(TEXT("CheckBox")) || ClassName.Contains(TEXT("Slider")) || ClassName.Contains(TEXT("Editable"));
				if (!bInteractive) continue;
				const FBoolProperty* OverrideProperty = FindFProperty<FBoolProperty>(Candidate->GetClass(), TEXT("bOverrideAccessibleDefaults"));
				if (!OverrideProperty || !OverrideProperty->GetPropertyValue_InContainer(Candidate)) ++MissingOverride;
			}
			Out->SetNumberField(TEXT("interactive_widget_count"), Widgets.Num());
			Out->SetNumberField(TEXT("missing_accessibility_override_count"), MissingOverride);
			Out->SetBoolField(TEXT("passed"), MissingOverride == 0);
			SetReceipt(Out, Tool, Blueprint, Widget, false);
			Summary = TEXT("Audited interactive UMG widgets for explicit accessibility configuration.");
			return true;
		}
		if (Tool == TEXT("umg_widget_navigation_validate"))
		{
			TArray<UWidget*> Widgets;
			Blueprint->WidgetTree->GetAllWidgets(Widgets);
			int32 Navigable = 0;
			int32 Configured = 0;
			for (UWidget* Candidate : Widgets)
			{
				if (!Candidate || !Candidate->GetIsEnabled()) continue;
				++Navigable;
				if (Candidate->Navigation) ++Configured;
			}
			const bool bRequireExplicit = ReadBool(Args, TEXT("require_explicit_navigation"), false);
			Out->SetNumberField(TEXT("enabled_widget_count"), Navigable);
			Out->SetNumberField(TEXT("explicit_navigation_widget_count"), Configured);
			Out->SetBoolField(TEXT("passed"), !bRequireExplicit || Configured > 0);
			SetReceipt(Out, Tool, Blueprint, Widget, false);
			Summary = TEXT("Audited UMG navigation configuration.");
			return true;
		}
		if (Tool == TEXT("umg_widget_responsive_matrix_validate"))
		{
			const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
			const bool bHasProfiles = Args->TryGetArrayField(TEXT("device_profiles"), Profiles) && Profiles && !Profiles->IsEmpty();
			Out->SetNumberField(TEXT("device_profile_count"), bHasProfiles ? Profiles->Num() : 0);
			Out->SetBoolField(TEXT("passed"), bHasProfiles && ValidateDesigner(Blueprint, Out));
			Out->SetStringField(TEXT("scope"), TEXT("structural_profile_gate; pair with umg_runtime_preview_capture for pixel evidence"));
			SetReceipt(Out, Tool, Blueprint, Blueprint->WidgetTree->RootWidget, false);
			Summary = TEXT("Validated responsive profile matrix structure and WidgetTree readiness.");
			return true;
		}
		if (Tool == TEXT("umg_widget_compile_delivery_gate"))
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
			const bool bCompiled = Blueprint->Status != BS_Error;
			const bool bDesignerPassed = ValidateDesigner(Blueprint, Out);
			bool bSaved = false;
			FString SaveError;
			if (bCompiled && bDesignerPassed && ReadBool(Args, TEXT("save_asset"), true))
			{
				bSaved = Context.Services.SaveAsset(Blueprint->GetPathName(), false, SaveError);
			}
			Out->SetBoolField(TEXT("compile_ok"), bCompiled);
			Out->SetBoolField(TEXT("designer_ok"), bDesignerPassed);
			Out->SetBoolField(TEXT("saved"), bSaved);
			Out->SetStringField(TEXT("save_error"), SaveError);
			Out->SetBoolField(TEXT("passed"), bCompiled && bDesignerPassed && (bSaved || !ReadBool(Args, TEXT("save_asset"), true)));
			SetReceipt(Out, Tool, Blueprint, Blueprint->WidgetTree->RootWidget, false);
			Summary = TEXT("Compiled, structurally validated, and save-gated the Widget Blueprint.");
			return bCompiled && bDesignerPassed && (bSaved || !ReadBool(Args, TEXT("save_asset"), true));
		}
		if (Tool == TEXT("umg_widget_save_verified"))
		{
			const bool bSaved = Context.Services.SaveAsset(Blueprint->GetPathName(), false, Error);
			if (!bSaved) return false;
			Out->SetBoolField(TEXT("saved"), true);
			Out->SetBoolField(TEXT("asset_exists_after_save"), Context.Services.AssetExists(Blueprint->GetPathName()));
			Out->SetBoolField(TEXT("passed"), Context.Services.AssetExists(Blueprint->GetPathName()));
			SetReceipt(Out, Tool, Blueprint, Blueprint->WidgetTree->RootWidget, false);
			Summary = TEXT("Saved and verified Widget Blueprint persistence.");
			return true;
		}

		if (!ApplyTargetProperties(Context, Args, Blueprint, Widget, Out, Error)) return false;
		SetReceipt(Out, Tool, Blueprint, Widget, true);
		Summary = FString::Printf(TEXT("Applied native UMG configuration through %s."), *Tool);
		return true;
	}

	static TSharedRef<FJsonObject> Schema()
	{
		return FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("Widget Blueprint asset path."))},
			{TEXT("widget_name"), FSololmcpSchemaBuilder::String(TEXT("WidgetTree widget name; omit for root on blueprint-wide validators."))},
			{TEXT("properties"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Public widget properties to apply."))},
			{TEXT("slot_properties"), FSololmcpSchemaBuilder::Object({}, {}, TEXT("Public panel-slot properties to apply."))},
			{TEXT("save_asset"), FSololmcpSchemaBuilder::Boolean(TEXT("Save after compile gate."))},
			{TEXT("require_explicit_navigation"), FSololmcpSchemaBuilder::Boolean(TEXT("Require at least one explicit navigation rule."))},
			{TEXT("device_profiles"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({}), TEXT("Responsive profile receipt rows."))}
		}, {TEXT("asset_path")});
	}
}

void RegisterUMGCompletionTools(FSololmcpToolRegistry& Registry)
{
	using namespace UmgCompletion;
	const TArray<FString> Names = {
		TEXT("umg_widget_layout_configure"), TEXT("umg_canvas_slot_configure"), TEXT("umg_grid_slot_configure"),
		TEXT("umg_uniform_grid_slot_configure"), TEXT("umg_horizontal_box_slot_configure"), TEXT("umg_vertical_box_slot_configure"),
		TEXT("umg_overlay_slot_configure"), TEXT("umg_wrap_box_slot_configure"), TEXT("umg_widget_render_transform_configure"),
		TEXT("umg_widget_navigation_configure"), TEXT("umg_widget_accessibility_configure"), TEXT("umg_widget_clipping_configure"),
		TEXT("umg_widget_tooltip_configure"), TEXT("umg_widget_style_configure"), TEXT("umg_widget_responsive_profile_apply"),
		TEXT("umg_widget_property_readback"), TEXT("umg_widget_slot_readback"), TEXT("umg_widget_designer_validate"),
		TEXT("umg_widget_accessibility_validate"), TEXT("umg_widget_navigation_validate"), TEXT("umg_widget_responsive_matrix_validate"),
		TEXT("umg_widget_compile_delivery_gate"), TEXT("umg_widget_save_verified")
	};
	for (const FString& Name : Names)
	{
		FSololmcpToolDefinition Definition;
		Definition.Name = Name;
		Definition.Description = TEXT("Native UMG Widget Blueprint configuration/readback/validation operation with fail-closed receipts.");
		Definition.InputSchema = Schema();
		Definition.CacheTtlSeconds = Name.EndsWith(TEXT("readback")) || Name.EndsWith(TEXT("validate")) ? 5 : 0;
		Definition.Execute = [Name](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
		{
			return ExecuteTool(Name, Context, Args, Out, Summary, Error);
		};
		Registry.Register(Definition);
	}
}
}
