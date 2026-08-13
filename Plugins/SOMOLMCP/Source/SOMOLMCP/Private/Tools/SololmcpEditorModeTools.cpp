// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SololmcpEditorModeTools.cpp — SOMOLMCP P3-3
// Editor mode switching tools (4):
//   editor_mode_enter / editor_mode_exit / editor_mode_get_active / editor_mode_set_tool
//
// Notes (UE 5.7):
//   - GLevelEditorModeTools() (FEditorModeTools&) drives Activate/Deactivate/IsActive.
//   - GetActiveModeIDs was removed in 5.x; we poll the well-known FBuiltinEditorModes::EM_*
//     IDs with IsModeActive(...) for the "get active modes" tool.
//   - Sub-tool switching inside a mode requires UInteractiveToolManager (per-mode tool
//     palette) which is not uniformly accessible across mode implementations. For first
//     version we implement only the "selection" reset path (deactivate non-default modes)
//     and return NOT_IMPLEMENTED for everything else. See // TODO(P3-3) below.
//

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"

// UE editor mode core
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "EditorModeTools.h"
// FBuiltinEditorModes lives in EditorFramework module (already in .Build.cs)
// (BuiltinEditorModes.h was removed in 5.7; the FBuiltinEditorModes:: constants
//  are still resolved through "EditorModes.h" which forward-declares them.)

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPEditorMode, Log, All);

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Game-thread guard — Slate / FEditorModeTools must be touched on the GT. */
static bool EditorMode_EnsureGameThread(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Editor mode tools must be called from the game thread.");
		return false;
	}
	return true;
}

/** Public mode-name → FEditorModeID mapping. */
static bool EditorMode_ResolveModeId(const FString& InName, FEditorModeID& OutId)
{
	const FString N = InName.TrimStartAndEnd().ToLower();
	if (N == TEXT("foliage"))         { OutId = FBuiltinEditorModes::EM_Foliage;   return true; }
	if (N == TEXT("landscape"))       { OutId = FBuiltinEditorModes::EM_Landscape; return true; }
	if (N == TEXT("mesh_paint")
	 || N == TEXT("meshpaint"))       { OutId = FBuiltinEditorModes::EM_MeshPaint; return true; }
	if (N == TEXT("mesh_modeling")
	 || N == TEXT("meshmodeling")
	 || N == TEXT("modeling"))
	{
		// UE 5.7: Mesh Modeling is implemented by the ModelingToolsEditorMode plugin.
		// Its mode ID is registered as "EM_ModelingToolsEditorMode" — there is no
		// constant in FBuiltinEditorModes for it.
		// TODO(P3-3): if user enables the plugin, expose a config to override this name.
		OutId = FEditorModeID(TEXT("EM_ModelingToolsEditorMode"));
		return true;
	}
	if (N == TEXT("control_rig")
	 || N == TEXT("controlrig"))
	{
		// TODO(P3-3): ControlRig editor mode ID — verify exact FName at runtime.
		OutId = FEditorModeID(TEXT("EM_ControlRigEditMode"));
		return true;
	}
	if (N == TEXT("selection")
	 || N == TEXT("default")
	 || N == TEXT("select"))          { OutId = FBuiltinEditorModes::EM_Default;   return true; }
	return false;
}

/** Pretty mode name from FEditorModeID for response payload. */
static FString EditorMode_DisplayName(const FEditorModeID& Id)
{
	if (Id == FBuiltinEditorModes::EM_Default)   return TEXT("selection");
	if (Id == FBuiltinEditorModes::EM_Foliage)   return TEXT("foliage");
	if (Id == FBuiltinEditorModes::EM_Landscape) return TEXT("landscape");
	if (Id == FBuiltinEditorModes::EM_MeshPaint) return TEXT("mesh_paint");
	if (Id == FBuiltinEditorModes::EM_Placement) return TEXT("placement");
	if (Id == FBuiltinEditorModes::EM_Level)     return TEXT("level");
	return Id.ToString();
}

/** Returns the first non-default active mode's display name (best-effort "current" mode). */
static FString EditorMode_FindActiveDisplayName()
{
	if (!GEditor) return TEXT("");
	FEditorModeTools& MT = GLevelEditorModeTools();
	const TArray<FEditorModeID> Known = {
		FBuiltinEditorModes::EM_Foliage,
		FBuiltinEditorModes::EM_Landscape,
		FBuiltinEditorModes::EM_MeshPaint,
		FBuiltinEditorModes::EM_Placement,
		FBuiltinEditorModes::EM_Level,
		FEditorModeID(TEXT("EM_ModelingToolsEditorMode")),
		FEditorModeID(TEXT("EM_ControlRigEditMode"))
	};
	for (const FEditorModeID& Id : Known)
	{
		if (MT.IsModeActive(Id))
		{
			return EditorMode_DisplayName(Id);
		}
	}
	if (MT.IsModeActive(FBuiltinEditorModes::EM_Default))
	{
		return EditorMode_DisplayName(FBuiltinEditorModes::EM_Default);
	}
	return TEXT("");
}

// ─────────────────────────────────────────────────────────────────────────────
// RegisterEditorModeTools
// ─────────────────────────────────────────────────────────────────────────────

void RegisterEditorModeTools(FSololmcpToolRegistry& Registry)
{
	const TArray<FString> ModeEnum = {
		TEXT("foliage"), TEXT("landscape"), TEXT("mesh_paint"),
		TEXT("mesh_modeling"), TEXT("control_rig"), TEXT("selection")
	};

	// ─────────────────────────────────────────────────────────────────────────
	// 1. editor_mode_enter
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_mode_enter"),
		TEXT("Activate a named editor mode in the level editor (foliage, landscape, mesh_paint, "
			 "mesh_modeling, control_rig, or selection). Returns the previously active mode and "
			 "the now-active mode's display name."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mode"), FSololmcpSchemaBuilder::String(
					TEXT("Mode name: foliage | landscape | mesh_paint | mesh_modeling | control_rig | selection"),
					ModeEnum)}
			},
			{TEXT("mode")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EditorMode_EnsureGameThread(OutError)) return false;
			if (!GEditor) { SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR")); OutError = TEXT("GEditor not available."); return false; }

			FString ModeName;
			if (!Arguments->TryGetStringField(TEXT("mode"), ModeName) || ModeName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("mode"));
				OutError = TEXT("Missing required argument: mode");
				return false;
			}

			FEditorModeID NewModeId;
			if (!EditorMode_ResolveModeId(ModeName, NewModeId))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("mode"),
					TEXT("Use one of: foliage, landscape, mesh_paint, mesh_modeling, control_rig, selection."));
				OutError = FString::Printf(TEXT("Unknown mode '%s'."), *ModeName);
				return false;
			}

			const FString PreviousMode = EditorMode_FindActiveDisplayName();

			FEditorModeTools& MT = GLevelEditorModeTools();
			if (MT.IsModeActive(NewModeId))
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("mode"),
					TEXT("Requested editor mode is already active."));
				OutError = FString::Printf(TEXT("Editor mode '%s' is already active."), *EditorMode_DisplayName(NewModeId));
				return false;
			}

			MT.ActivateMode(NewModeId, /*bToggle=*/false);
			const bool bActive = MT.IsModeActive(NewModeId);

			OutStructured->SetStringField(TEXT("previous_mode"), PreviousMode);
			OutStructured->SetStringField(TEXT("new_mode"), EditorMode_DisplayName(NewModeId));
			OutStructured->SetStringField(TEXT("mode_id"), NewModeId.ToString());
			OutStructured->SetBoolField(TEXT("is_active"), bActive);

			OutSummary = FString::Printf(TEXT("Entered editor mode '%s' (active=%s)."),
				*EditorMode_DisplayName(NewModeId), bActive ? TEXT("true") : TEXT("false"));
			if (!bActive)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("mode"),
					TEXT("ActivateMode completed but the requested editor mode is not active."));
				OutError = FString::Printf(TEXT("Editor mode '%s' did not become active."), *EditorMode_DisplayName(NewModeId));
				return false;
			}
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 2. editor_mode_exit
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_mode_exit"),
		TEXT("Exit the current non-default editor mode and switch to the given mode (default: 'selection'). "
			 "Returns the previous mode and the now-active mode."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("to"), FSololmcpSchemaBuilder::String(
					TEXT("Optional mode to switch to (default: 'selection')."), ModeEnum)}
			},
			{}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EditorMode_EnsureGameThread(OutError)) return false;
			if (!GEditor) { SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR")); OutError = TEXT("GEditor not available."); return false; }

			FString ToName;
			if (!Arguments->TryGetStringField(TEXT("to"), ToName) || ToName.IsEmpty())
			{
				ToName = TEXT("selection");
			}

			FEditorModeID TargetId;
			if (!EditorMode_ResolveModeId(ToName, TargetId))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("to"),
					TEXT("Use one of: foliage, landscape, mesh_paint, mesh_modeling, control_rig, selection."));
				OutError = FString::Printf(TEXT("Unknown 'to' mode '%s'."), *ToName);
				return false;
			}

			const FString PreviousMode = EditorMode_FindActiveDisplayName();

			FEditorModeTools& MT = GLevelEditorModeTools();
			if (MT.IsModeActive(TargetId) && PreviousMode == EditorMode_DisplayName(TargetId))
			{
				SololmcpError::Set(OutStructured, TEXT("NO_CHANGE"), TEXT("to"),
					TEXT("Target editor mode is already active."));
				OutError = FString::Printf(TEXT("Editor mode '%s' is already active."), *EditorMode_DisplayName(TargetId));
				return false;
			}

			// Deactivate any well-known non-default mode that's currently up.
			const TArray<FEditorModeID> NonDefault = {
				FBuiltinEditorModes::EM_Foliage,
				FBuiltinEditorModes::EM_Landscape,
				FBuiltinEditorModes::EM_MeshPaint,
				FBuiltinEditorModes::EM_Placement,
				FBuiltinEditorModes::EM_Level,
				FEditorModeID(TEXT("EM_ModelingToolsEditorMode")),
				FEditorModeID(TEXT("EM_ControlRigEditMode"))
			};
			for (const FEditorModeID& Id : NonDefault)
			{
				if (MT.IsModeActive(Id))
				{
					MT.DeactivateMode(Id);
				}
			}

			MT.ActivateMode(TargetId, /*bToggle=*/false);
			const bool bActive = MT.IsModeActive(TargetId);
			bool bLingeringNonDefault = false;
			for (const FEditorModeID& Id : NonDefault)
			{
				if (Id != TargetId && MT.IsModeActive(Id))
				{
					bLingeringNonDefault = true;
					break;
				}
			}

			OutStructured->SetStringField(TEXT("previous_mode"), PreviousMode);
			OutStructured->SetStringField(TEXT("new_mode"), EditorMode_DisplayName(TargetId));
			OutStructured->SetBoolField(TEXT("is_active"), bActive);

			OutSummary = FString::Printf(TEXT("Exited '%s', now in '%s'."),
				*PreviousMode, *EditorMode_DisplayName(TargetId));
			if (!bActive)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("to"),
					TEXT("Mode switch completed but the target editor mode is not active."));
				OutError = FString::Printf(TEXT("Target editor mode '%s' did not become active."), *EditorMode_DisplayName(TargetId));
				return false;
			}
			if (bLingeringNonDefault)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT("to"),
					TEXT("One or more non-default editor modes remained active after exit."));
				OutError = TEXT("Editor mode exit did not fully deactivate the previous non-default mode.");
				return false;
			}
			return true;
		}
	, nullptr
	, 0
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 3. editor_mode_get_active
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_mode_get_active"),
		TEXT("List the currently active editor mode(s). Multiple modes may be active simultaneously "
			 "in UE (e.g. selection + a paint mode). Returns each as both display_name and mode_id."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EditorMode_EnsureGameThread(OutError)) return false;
			if (!GEditor) { SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR")); OutError = TEXT("GEditor not available."); return false; }

			FEditorModeTools& MT = GLevelEditorModeTools();

			// Probe the well-known + plugin-registered set. UE 5.7 removed
			// GetActiveModeIDs(); IsModeActive is stable across versions.
			// TODO(P3-3): consider walking GetActiveScriptableModes() when we're
			// confident the symbol is exported across all UE 5.7.x patch revs.
			const TArray<FEditorModeID> Known = {
				FBuiltinEditorModes::EM_Default,
				FBuiltinEditorModes::EM_Foliage,
				FBuiltinEditorModes::EM_Landscape,
				FBuiltinEditorModes::EM_MeshPaint,
				FBuiltinEditorModes::EM_Placement,
				FBuiltinEditorModes::EM_Level,
				FEditorModeID(TEXT("EM_ModelingToolsEditorMode")),
				FEditorModeID(TEXT("EM_ControlRigEditMode"))
			};

			TArray<TSharedPtr<FJsonValue>> ActiveModes;
			for (const FEditorModeID& Id : Known)
			{
				if (MT.IsModeActive(Id))
				{
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("display_name"), EditorMode_DisplayName(Id));
					Obj->SetStringField(TEXT("mode_id"), Id.ToString());
					ActiveModes.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}

			OutStructured->SetArrayField(TEXT("active_modes"), ActiveModes);
			OutStructured->SetNumberField(TEXT("count"), ActiveModes.Num());

			OutSummary = FString::Printf(TEXT("%d active editor mode(s)."), ActiveModes.Num());
			return true;
		}
	, nullptr
	, 2
	});

	// ─────────────────────────────────────────────────────────────────────────
	// 4. editor_mode_set_tool
	// ─────────────────────────────────────────────────────────────────────────
	Registry.Register({
		TEXT("editor_mode_set_tool"),
		TEXT("Switch the active sub-tool inside an active editor mode "
			 "(e.g. landscape sculpt/smooth/flatten/erase, mesh_paint paint). "
			 "Currently implemented only as a 'selection' mode reset; other mode/tool combos "
			 "return NOT_IMPLEMENTED until per-mode UInteractiveToolManager wiring lands."),
		FSololmcpSchemaBuilder::Object(
			{
				{TEXT("mode"), FSololmcpSchemaBuilder::String(
					TEXT("Currently active mode (foliage|landscape|mesh_paint|mesh_modeling|control_rig|selection)."),
					ModeEnum)},
				{TEXT("tool"), FSololmcpSchemaBuilder::String(
					TEXT("Sub-tool ID, e.g. sculpt|smooth|flatten|erase|paint."))}
			},
			{TEXT("mode"), TEXT("tool")}),

		[](const FSololmcpToolExecutionContext& Context,
		   const TSharedRef<FJsonObject>& Arguments,
		   TSharedRef<FJsonObject>& OutStructured,
		   FString& OutSummary, FString& OutError) -> bool
		{
			if (!EditorMode_EnsureGameThread(OutError)) return false;
			if (!GEditor) { SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR")); OutError = TEXT("GEditor not available."); return false; }

			FString ModeName, ToolName;
			if (!Arguments->TryGetStringField(TEXT("mode"), ModeName) || ModeName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("mode"));
				OutError = TEXT("Missing required argument: mode");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("tool"), ToolName) || ToolName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("tool"));
				OutError = TEXT("Missing required argument: tool");
				return false;
			}

			FEditorModeID ModeId;
			if (!EditorMode_ResolveModeId(ModeName, ModeId))
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("mode"),
					TEXT("Use one of: foliage, landscape, mesh_paint, mesh_modeling, control_rig, selection."));
				OutError = FString::Printf(TEXT("Unknown mode '%s'."), *ModeName);
				return false;
			}

			FEditorModeTools& MT = GLevelEditorModeTools();
			if (!MT.IsModeActive(ModeId))
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("active mode '%s'"), *ModeName));
				OutError = FString::Printf(TEXT("Mode '%s' is not currently active. Call editor_mode_enter first."), *ModeName);
				return false;
			}

			// Selection-mode reset is the one path we can safely implement without
			// touching per-mode UInteractiveToolManager APIs.
			const FString ToolLower = ToolName.ToLower();
			if (ModeId == FBuiltinEditorModes::EM_Default
				&& (ToolLower == TEXT("select") || ToolLower == TEXT("reset") || ToolLower == TEXT("none")))
			{
				// "Reset" in selection mode == ensure no transient sub-tool is engaged.
				// Re-activate default mode to clear any lingering tool overlay.
				MT.ActivateMode(FBuiltinEditorModes::EM_Default, /*bToggle=*/true);
				OutStructured->SetStringField(TEXT("mode"), ModeName);
				OutStructured->SetStringField(TEXT("tool"), ToolName);
				OutStructured->SetBoolField(TEXT("ok"), true);
				OutSummary = TEXT("Reset selection mode tool state.");
				return true;
			}

			// TODO(P3-3): wire per-mode UInteractiveToolManager:
			//   - Landscape: ULandscapeEditorObject + brush selection (sculpt/smooth/flatten/erase)
			//   - MeshPaint: UMeshPaintMode::ModeSettings
			//   - Foliage:   FFoliageEditMode tool palette
			//   - Modeling:  UModelingToolsModeProperties::ActivateToolByName(...)
			//   Each requires the corresponding editor module's runtime API which is
			//   not currently linked. Until then, surface NOT_IMPLEMENTED so callers
			//   can branch cleanly instead of seeing a generic failure.
			SololmcpError::Set(OutStructured, TEXT("NOT_IMPLEMENTED"), TEXT("tool"),
				FString::Printf(TEXT("Sub-tool switching for mode '%s' is not implemented in this build. "
					"Only 'selection' mode reset is currently supported."), *ModeName));
			OutStructured->SetStringField(TEXT("mode"), ModeName);
			OutStructured->SetStringField(TEXT("tool"), ToolName);
			OutStructured->SetBoolField(TEXT("ok"), false);

			OutError = FString::Printf(TEXT("editor_mode_set_tool: '%s' inside mode '%s' is not implemented yet."),
				*ToolName, *ModeName);
			return false;
		}
	, nullptr
	, 0
	});
}

} // namespace UE::SOMOLMCP
