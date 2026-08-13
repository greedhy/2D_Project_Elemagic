// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Input/Reply.h"
#include "Modules/ModuleInterface.h"
#include "Logging/LogCategory.h"
#include "Styling/SlateColor.h"

class SWidget;

SOMOLMCP_API DECLARE_LOG_CATEGORY_EXTERN(LogSOMOLMCP, Log, All);

namespace UE::SOMOLMCP
{
	/** Returns the version from the installed SOMOLMCP descriptor. */
	SOMOLMCP_API FString GetProductVersion();
}

class FSOMOLMCPModule final : public IModuleInterface
{
public:
	static FSOMOLMCPModule* Get();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;


private:
	void StartIfEnabled();
	void Stop();
	bool TickEditorProtection(float DeltaTime);
	void UpdateEditorProtection();
	void RestoreEditorProtection();

	FTSTicker::FDelegateHandle EditorProtectionTickerHandle;
	bool bInitialUnattendedScript = false;
	bool bMcpEditorProtectionActive = false;
	bool bForceMcpUnattended = false;
	bool bForceMcpInteractive = false;

#if WITH_EDITOR
	void RegisterEditorToolbar();
	void RegisterEditorMenus();
	void UnregisterEditorToolbar();
	TSharedRef<SWidget> MakeMcpToolbarWidget();
	TSharedRef<SWidget> MakeMcpStatusBubbleWidget();
	TSharedRef<SWidget> MakeMcpStatusBubblePopupWidget();
	FReply OnMcpToggleClicked();
	FText GetMcpStatusText() const;
	FText GetMcpBubbleText() const;
	FText GetMcpTooltipText() const;
	FSlateColor GetMcpStatusColor() const;
	FSlateColor GetMcpBubbleColor() const;
#endif
};
