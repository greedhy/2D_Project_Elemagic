// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "EditorModeTools.h"

namespace UE::SOMOLMCP::TerrainModeGuard
{
inline FString ModeDisplayName(const FEditorModeID& Id)
{
	if (Id == FBuiltinEditorModes::EM_Default)   return TEXT("selection");
	if (Id == FBuiltinEditorModes::EM_Landscape) return TEXT("landscape");
	if (Id == FBuiltinEditorModes::EM_Foliage)   return TEXT("foliage");
	if (Id == FBuiltinEditorModes::EM_MeshPaint) return TEXT("mesh_paint");
	if (Id == FBuiltinEditorModes::EM_Placement) return TEXT("placement");
	if (Id == FBuiltinEditorModes::EM_Level)     return TEXT("level");
	return Id.ToString();
}

inline TArray<FEditorModeID> NonDefaultModes()
{
	return {
		FBuiltinEditorModes::EM_Foliage,
		FBuiltinEditorModes::EM_Landscape,
		FBuiltinEditorModes::EM_MeshPaint,
		FBuiltinEditorModes::EM_Placement,
		FBuiltinEditorModes::EM_Level,
		FEditorModeID(TEXT("EM_ModelingToolsEditorMode")),
		FEditorModeID(TEXT("EM_ControlRigEditMode"))
	};
}

inline FString ActiveModeName()
{
	if (!GEditor)
	{
		return TEXT("unknown");
	}

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	for (const FEditorModeID& Id : NonDefaultModes())
	{
		if (ModeTools.IsModeActive(Id))
		{
			return ModeDisplayName(Id);
		}
	}

	if (ModeTools.IsModeActive(FBuiltinEditorModes::EM_Default))
	{
		return TEXT("selection");
	}
	return TEXT("unknown");
}

inline bool ForceSelectionMode(FString& OutPreviousMode, FString& OutCurrentMode, FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Terrain editor mode guard must run on the game thread.");
		return false;
	}
	if (!GEditor)
	{
		OutError = TEXT("GEditor is unavailable; cannot switch editor mode before terrain mutation.");
		return false;
	}

	OutPreviousMode = ActiveModeName();
	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	for (const FEditorModeID& Id : NonDefaultModes())
	{
		if (ModeTools.IsModeActive(Id))
		{
			ModeTools.DeactivateMode(Id);
		}
	}
	ModeTools.ActivateMode(FBuiltinEditorModes::EM_Default, /*bToggle=*/false);

	OutCurrentMode = ActiveModeName();
	const bool bOk = ModeTools.IsModeActive(FBuiltinEditorModes::EM_Default)
		&& !ModeTools.IsModeActive(FBuiltinEditorModes::EM_Landscape);
	if (!bOk)
	{
		OutError = FString::Printf(
			TEXT("Failed to force editor Selection mode before terrain mutation; previous='%s', current='%s'."),
			*OutPreviousMode,
			*OutCurrentMode);
	}
	return bOk;
}

class FSelectionScope
{
public:
	FSelectionScope()
		: Receipt(MakeShared<FJsonObject>())
	{
	}

	bool Begin(FString& OutError)
	{
		FString PreviousMode;
		FString CurrentMode;
		const bool bOk = ForceSelectionMode(PreviousMode, CurrentMode, OutError);
		bStarted = true;

		Receipt->SetStringField(TEXT("policy"), TEXT("force_selection_before_and_after_terrain_mutation"));
		Receipt->SetStringField(TEXT("mode_before_pre_switch"), PreviousMode);
		Receipt->SetStringField(TEXT("mode_after_pre_switch"), CurrentMode);
		Receipt->SetBoolField(TEXT("was_landscape_mode"), PreviousMode.Equals(TEXT("landscape"), ESearchCase::IgnoreCase));
		Receipt->SetBoolField(TEXT("pre_switch_attempted"), true);
		Receipt->SetBoolField(TEXT("pre_switch_ok"), bOk);
		if (!bOk)
		{
			Receipt->SetStringField(TEXT("pre_switch_error"), OutError);
		}
		return bOk;
	}

	void Attach(const TSharedRef<FJsonObject>& OutStructured) const
	{
		OutStructured->SetObjectField(TEXT("editor_mode_guard"), Receipt);
	}

	~FSelectionScope()
	{
		if (!bStarted)
		{
			return;
		}

		FString PreviousMode;
		FString CurrentMode;
		FString Error;
		const bool bOk = ForceSelectionMode(PreviousMode, CurrentMode, Error);
		Receipt->SetBoolField(TEXT("post_switch_attempted"), true);
		Receipt->SetBoolField(TEXT("post_switch_ok"), bOk);
		Receipt->SetStringField(TEXT("mode_before_post_switch"), PreviousMode);
		Receipt->SetStringField(TEXT("mode_after_post_switch"), CurrentMode);
		if (!bOk)
		{
			Receipt->SetStringField(TEXT("post_switch_error"), Error);
		}
	}

private:
	TSharedRef<FJsonObject> Receipt;
	bool bStarted = false;
};
} // namespace UE::SOMOLMCP::TerrainModeGuard
