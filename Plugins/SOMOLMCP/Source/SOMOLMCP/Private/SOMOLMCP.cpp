// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "SOMOLMCP.h"
#include "SololmcpServer.h"
#include "Protocol/SololmcpJobService.h"
#include "Tools/SololmcpToolRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "CoreGlobals.h"  // GIsRunningUnattendedScript

#if WITH_EDITOR
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPModule, Log, All);

namespace
{
	bool IsSomolMcpChineseEditorCulture()
	{
		const FCulturePtr Culture = FInternationalization::Get().GetCurrentCulture();
		return Culture.IsValid() && Culture->GetTwoLetterISOLanguageName().Equals(TEXT("zh"), ESearchCase::IgnoreCase);
	}

	FText SomolMcpText(const TCHAR* English, const TCHAR* Chinese)
	{
		return FText::FromString(IsSomolMcpChineseEditorCulture() ? FString(Chinese) : FString(English));
	}
}

namespace UE::SOMOLMCP
{
	FString GetProductVersion()
	{
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SOMOLMCP")))
		{
			return Plugin->GetDescriptor().VersionName;
		}
		return TEXT("unknown");
	}

	static bool bEnable = false;
	static int32 Port = 12000;
	static FString Bind = TEXT("127.0.0.1");
	static bool bHttpEnable = true;
	static int32 HttpPort = 12001;

	// Security console variables
	static FString AuthToken;
	static int32 MaxRpm = 1200;     // Max requests per minute per connection
	static int32 MaxConnections = 8192;

	static TUniquePtr<FSololmcpServer> Server;

	static void OnEnableChanged(IConsoleVariable* Var);
	static void OnPortChanged(IConsoleVariable* Var);
	static void OnBindChanged(IConsoleVariable* Var);

	static bool IsServerRunning()
	{
		return Server && Server->IsRunning();
	}

	static int32 GetActualPort()
	{
		return Server ? Server->GetActualPort() : Port;
	}

	static FSololmcpTcpTransport::FTransportStats GetTransportStats()
	{
		return Server ? Server->GetTransportStats() : FSololmcpTcpTransport::FTransportStats();
	}

	static FString GetClientSummary()
	{
		return Server ? Server->GetClientSummary() : FString();
	}

	static void SetConsoleIntValue(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCommandline);
		}
	}

	static FAutoConsoleVariableRef CVarEnable(
		TEXT("somolmcp.enable"),
		bEnable,
		TEXT("Enable SOMOLMCP server (0/1)."),
		FConsoleVariableDelegate::CreateStatic(OnEnableChanged),
		ECVF_Default);

	static FAutoConsoleVariableRef CVarPort(
		TEXT("somolmcp.port"),
		Port,
		TEXT("SOMOLMCP TCP listen port."),
		FConsoleVariableDelegate::CreateStatic(OnPortChanged),
		ECVF_Default);

	static FAutoConsoleVariableRef CVarBind(
		TEXT("somolmcp.bind"),
		Bind,
		TEXT("SOMOLMCP bind address (IPv4)."),
		FConsoleVariableDelegate::CreateStatic(OnBindChanged),
		ECVF_Default);

	static FAutoConsoleVariableRef CVarAuthToken(
		TEXT("somolmcp.auth.token"),
		AuthToken,
		TEXT("Auth token for client connections. Empty = disabled (open access)."),
		ECVF_Default);

	static FAutoConsoleVariableRef CVarHttpEnable(
		TEXT("somolmcp.http.enable"),
		bHttpEnable,
		TEXT("Enable the standard MCP Streamable HTTP endpoint (0/1)."),
		ECVF_Default);

	static FAutoConsoleVariableRef CVarHttpPort(
		TEXT("somolmcp.http.port"),
		HttpPort,
		TEXT("SOMOLMCP Streamable HTTP listen port. The endpoint is /mcp."),
		ECVF_Default);

	static FAutoConsoleVariableRef CVarMaxRpm(
		TEXT("somolmcp.rate.max_rpm"),
		MaxRpm,
		TEXT("Max requests per minute per connection. 0 = unlimited."),
		ECVF_Default);

	static FAutoConsoleVariableRef CVarMaxConnections(
		TEXT("somolmcp.rate.max_connections"),
		MaxConnections,
		TEXT("Max simultaneous connections. 0 = unlimited."),
		ECVF_Default);

	static void EnsureStarted()
	{
		if (!bEnable)
		{
			return;
		}

		if (!Server)
		{
			Server = MakeUnique<FSololmcpServer>();
		}

		if (!Server->IsRunning())
		{
			Server->Start(Bind, Port, AuthToken, MaxRpm, MaxConnections, bHttpEnable, HttpPort);
		}
	}

	static void EnsureStopped()
	{
		if (Server)
		{
			Server->Stop();
			Server.Reset();
		}
	}

	static void OnEnableChanged(IConsoleVariable* Var)
	{
		if (bEnable)
		{
			EnsureStarted();
		}
		else
		{
			EnsureStopped();
		}
	}

	static void OnPortChanged(IConsoleVariable* Var)
	{
		if (!Server || !Server->IsRunning())
		{
			return;
		}

		// Restart with new port.
		EnsureStopped();
		EnsureStarted();
	}

	static void OnBindChanged(IConsoleVariable* Var)
	{
		if (!Server || !Server->IsRunning())
		{
			return;
		}

		// Restart with new bind address.
		EnsureStopped();
		EnsureStarted();
	}
}

FSOMOLMCPModule* FSOMOLMCPModule::Get()
{
	return FModuleManager::GetModulePtr<FSOMOLMCPModule>("SOMOLMCP");
}

void FSOMOLMCPModule::StartupModule()
{
	// Preserve the editor's launch mode and apply unattended modal suppression only
	// while MCP work is actually runnable. Keeping this process-wide flag enabled for
	// the whole editor session prevented normal interactive dialogs even with an empty
	// queue. Per-tool guards still cover the exact execution window; the ticker below
	// closes the small gap between queued steps without locking an idle editor.
	bInitialUnattendedScript = GIsRunningUnattendedScript;
	bForceMcpInteractive = FParse::Param(FCommandLine::Get(), TEXT("somolmcp.interactive"));
	bForceMcpUnattended = FParse::Param(FCommandLine::Get(), TEXT("somolmcp.unattended"));
	if (bForceMcpUnattended && !bForceMcpInteractive)
	{
		GIsRunningUnattendedScript = true;
		bMcpEditorProtectionActive = true;
	}

	// Auto-enable when the plugin is loaded (i.e. user checked it in the plugin list).
	// Individual overrides via command line still take effect below.
	UE::SOMOLMCP::bEnable = true;

	// Allow enabling/disabling via command line: -somolmcp or -somolmcp=12000
	if (FParse::Param(FCommandLine::Get(), TEXT("somolmcp")))
	{
		UE::SOMOLMCP::bEnable = true;
	}

	int32 CmdPort = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp="), CmdPort) && CmdPort > 0)
	{
		UE::SOMOLMCP::Port = CmdPort;
		UE::SOMOLMCP::bEnable = true;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.port="), CmdPort) && CmdPort > 0)
	{
		UE::SOMOLMCP::Port = CmdPort;
		UE::SOMOLMCP::bEnable = true;
	}

	FString CmdBind;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.bind="), CmdBind) && !CmdBind.IsEmpty())
	{
		UE::SOMOLMCP::Bind = CmdBind;
	}

	FString CmdAuthToken;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.auth.token="), CmdAuthToken))
	{
		UE::SOMOLMCP::AuthToken = CmdAuthToken;
	}

	int32 CmdMaxRpm = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.rate.max_rpm="), CmdMaxRpm) && CmdMaxRpm >= 0)
	{
		UE::SOMOLMCP::MaxRpm = CmdMaxRpm;
	}

	int32 CmdMaxConnections = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.rate.max_connections="), CmdMaxConnections) && CmdMaxConnections >= 0)
	{
		UE::SOMOLMCP::MaxConnections = CmdMaxConnections;
	}

	int32 CmdStrictBinding = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.auth.strict_binding="), CmdStrictBinding))
	{
		UE::SOMOLMCP::SetConsoleIntValue(TEXT("somolmcp.auth.strict_binding"), CmdStrictBinding != 0 ? 1 : 0);
	}

	int32 CmdHttpEnable = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.http.enable="), CmdHttpEnable))
	{
		UE::SOMOLMCP::bHttpEnable = CmdHttpEnable != 0;
	}
	int32 CmdHttpPort = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("somolmcp.http.port="), CmdHttpPort) && CmdHttpPort > 0)
	{
		UE::SOMOLMCP::HttpPort = CmdHttpPort;
	}

	StartIfEnabled();
	EditorProtectionTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FSOMOLMCPModule::TickEditorProtection),
		0.05f);
	UpdateEditorProtection();

#if WITH_EDITOR
	RegisterEditorToolbar();
#endif
}

void FSOMOLMCPModule::ShutdownModule()
{
	if (EditorProtectionTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(EditorProtectionTickerHandle);
		EditorProtectionTickerHandle.Reset();
	}
	RestoreEditorProtection();
#if WITH_EDITOR
	UnregisterEditorToolbar();
#endif
	Stop();
}

void FSOMOLMCPModule::StartIfEnabled()
{
	if (UE::SOMOLMCP::bEnable)
	{
		UE::SOMOLMCP::EnsureStarted();
		UE_LOG(LogSOMOLMCPModule, Log, TEXT("SOMOLMCP enabled (somolmcp.enable=1)"));
	}
}

void FSOMOLMCPModule::Stop()
{
	UE::SOMOLMCP::EnsureStopped();
	UpdateEditorProtection();
}

bool FSOMOLMCPModule::TickEditorProtection(float DeltaTime)
{
	(void)DeltaTime;
	UpdateEditorProtection();
	return true;
}

void FSOMOLMCPModule::UpdateEditorProtection()
{
	if (bForceMcpInteractive)
	{
		if (bMcpEditorProtectionActive)
		{
			GIsRunningUnattendedScript = bInitialUnattendedScript;
			bMcpEditorProtectionActive = false;
		}
		return;
	}

	bool bHasRunnableQueueWork = false;
	if (UE::SOMOLMCP::IsServerRunning())
	{
		TSharedRef<FJsonObject> JobsSnapshot = MakeShared<FJsonObject>();
		UE::SOMOLMCP::FSololmcpJobService::BuildJobsSnapshotObject(JobsSnapshot);
		double Queued = 0.0;
		double Running = 0.0;
		JobsSnapshot->TryGetNumberField(TEXT("queued"), Queued);
		JobsSnapshot->TryGetNumberField(TEXT("running"), Running);
		const UE::SOMOLMCP::FSololmcpToolRuntimeSnapshot ToolSnapshot = UE::SOMOLMCP::GetToolRuntimeSnapshot();
		bHasRunnableQueueWork = Queued > 0.0 || Running > 0.0 || ToolSnapshot.ActiveToolExecutions > 0;
	}

	const bool bShouldProtect = bForceMcpUnattended || bHasRunnableQueueWork;
	if (bShouldProtect == bMcpEditorProtectionActive)
	{
		return;
	}

	bMcpEditorProtectionActive = bShouldProtect;
	GIsRunningUnattendedScript = bShouldProtect ? true : bInitialUnattendedScript;
	UE_LOG(
		LogSOMOLMCPModule,
		Log,
		TEXT("SOMOLMCP editor protection %s (%s)."),
		bShouldProtect ? TEXT("enabled") : TEXT("released"),
		bShouldProtect ? TEXT("queue/tool execution active") : TEXT("queue idle"));
}

void FSOMOLMCPModule::RestoreEditorProtection()
{
	if (bMcpEditorProtectionActive)
	{
		GIsRunningUnattendedScript = bInitialUnattendedScript;
		bMcpEditorProtectionActive = false;
	}
}

#if WITH_EDITOR
void FSOMOLMCPModule::RegisterEditorToolbar()
{
	if (IsRunningCommandlet() || FApp::IsUnattended() || GIsRunningUnattendedScript || !UToolMenus::IsToolMenuUIEnabled())
	{
		return;
	}

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSOMOLMCPModule::RegisterEditorMenus));

	RegisterEditorMenus();
	UToolMenus::Get()->RefreshAllWidgets();
}

void FSOMOLMCPModule::RegisterEditorMenus()
{
	if (IsRunningCommandlet() || FApp::IsUnattended() || GIsRunningUnattendedScript || !UToolMenus::IsToolMenuUIEnabled())
	{
		return;
	}

	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
	if (!ToolbarMenu)
	{
		return;
	}

	FToolMenuSection& Section = ToolbarMenu->FindOrAddSection(TEXT("Play"));
	FToolMenuEntry Entry = FToolMenuEntry::InitWidget(
		TEXT("SOMOLMCPStatusToggle"),
		MakeMcpToolbarWidget(),
		FText::GetEmpty(),
		true,
		false);
	Section.AddEntry(Entry);

	FToolMenuEntry BubbleEntry = FToolMenuEntry::InitWidget(
		TEXT("SOMOLMCPStatusBubble"),
		MakeMcpStatusBubbleWidget(),
		FText::GetEmpty(),
		true,
		false);
	Section.AddEntry(BubbleEntry);
}

void FSOMOLMCPModule::UnregisterEditorToolbar()
{
	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
}

TSharedRef<SWidget> FSOMOLMCPModule::MakeMcpToolbarWidget()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush(TEXT("ToolPanel.GroupBorder")))
		.BorderBackgroundColor_Raw(this, &FSOMOLMCPModule::GetMcpStatusColor)
		.Padding(FMargin(1.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush(TEXT("ToolPanel.GroupBorder")))
			.BorderBackgroundColor(FSlateColor(FLinearColor(0.035f, 0.040f, 0.050f, 0.94f)))
			.Padding(FMargin(1.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), TEXT("SimpleButton"))
				.ContentPadding(FMargin(7.0f, 2.0f))
				.ToolTipText_Raw(this, &FSOMOLMCPModule::GetMcpTooltipText)
				.OnClicked_Raw(this, &FSOMOLMCPModule::OnMcpToggleClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("●")))
						.ColorAndOpacity_Raw(this, &FSOMOLMCPModule::GetMcpStatusColor)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Raw(this, &FSOMOLMCPModule::GetMcpStatusText)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.94f, 0.98f, 1.0f)))
					]
				]
			]
		];
}

TSharedRef<SWidget> FSOMOLMCPModule::MakeMcpStatusBubbleWidget()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush(TEXT("ToolPanel.GroupBorder")))
		.BorderBackgroundColor_Raw(this, &FSOMOLMCPModule::GetMcpBubbleColor)
		.Padding(FMargin(1.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush(TEXT("ToolPanel.GroupBorder")))
			.BorderBackgroundColor(FSlateColor(FLinearColor(0.035f, 0.040f, 0.050f, 0.94f)))
			.Padding(FMargin(1.0f))
			[
				SNew(SComboButton)
				.HasDownArrow(false)
				.ButtonStyle(FAppStyle::Get(), TEXT("SimpleButton"))
				.ContentPadding(FMargin(8.0f, 2.0f))
				.MenuPlacement(MenuPlacement_AboveAnchor)
				.ToolTipText_Raw(this, &FSOMOLMCPModule::GetMcpBubbleText)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text(SomolMcpText(TEXT("Status"), TEXT("状态")))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.94f, 0.98f, 1.0f)))
				]
				.MenuContent()
				[
					MakeMcpStatusBubblePopupWidget()
				]
			]
		];
}

TSharedRef<SWidget> FSOMOLMCPModule::MakeMcpStatusBubblePopupWidget()
{
	return SNew(SBorder)
		.Visibility(EVisibility::HitTestInvisible)
		.BorderImage(FAppStyle::Get().GetBrush(TEXT("ToolPanel.GroupBorder")))
		.BorderBackgroundColor_Raw(this, &FSOMOLMCPModule::GetMcpBubbleColor)
		.Padding(FMargin(12.0f, 9.0f))
		[
			SNew(SBox)
			.WidthOverride(620.0f)
			[
				SNew(STextBlock)
				.Text_Raw(this, &FSOMOLMCPModule::GetMcpBubbleText)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.94f, 0.98f, 1.0f)))
				.AutoWrapText(true)
			]
		];
}

FReply FSOMOLMCPModule::OnMcpToggleClicked()
{
	const bool bTargetEnabled = !UE::SOMOLMCP::IsServerRunning();
	UE::SOMOLMCP::bEnable = bTargetEnabled;

	if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("somolmcp.enable")))
	{
		Var->Set(bTargetEnabled ? 1 : 0, ECVF_SetByConsole);
	}
	else if (bTargetEnabled)
	{
		UE::SOMOLMCP::EnsureStarted();
	}
	else
	{
		UE::SOMOLMCP::EnsureStopped();
	}

	UE_LOG(LogSOMOLMCPModule, Log, TEXT("SOMOLMCP editor toolbar toggle: %s"), bTargetEnabled ? TEXT("enabled") : TEXT("disabled"));
	return FReply::Handled();
}

FText FSOMOLMCPModule::GetMcpStatusText() const
{
	if (!UE::SOMOLMCP::IsServerRunning())
	{
		return SomolMcpText(TEXT("SOMOLMCP: stopped"), TEXT("SOMOLMCP: 已停止"));
	}

	const UE::SOMOLMCP::FSololmcpTcpTransport::FTransportStats Stats = UE::SOMOLMCP::GetTransportStats();
	return FText::FromString(IsSomolMcpChineseEditorCulture()
		? FString::Printf(TEXT("SOMOLMCP: 运行中 :%d 连接 %d"), UE::SOMOLMCP::GetActualPort(), Stats.ActiveConnections)
		: FString::Printf(TEXT("SOMOLMCP: running :%d connections %d"), UE::SOMOLMCP::GetActualPort(), Stats.ActiveConnections));
}

FText FSOMOLMCPModule::GetMcpBubbleText() const
{
	if (!UE::SOMOLMCP::IsServerRunning())
	{
		return SomolMcpText(
			TEXT("SOMOLMCP is stopped.\nNo active task queue."),
			TEXT("SOMOLMCP 已停止。\n无活动任务队列。"));
	}

	TSharedRef<FJsonObject> JobsSnapshot = MakeShared<FJsonObject>();
	UE::SOMOLMCP::FSololmcpJobService::BuildJobsSnapshotObject(JobsSnapshot);

	double Queued = 0.0;
	double Running = 0.0;
	double Blocked = 0.0;
	double Failed = 0.0;
	double Tracked = 0.0;
	double Submitted = 0.0;
	JobsSnapshot->TryGetNumberField(TEXT("queued"), Queued);
	JobsSnapshot->TryGetNumberField(TEXT("running"), Running);
	JobsSnapshot->TryGetNumberField(TEXT("blocked"), Blocked);
	JobsSnapshot->TryGetNumberField(TEXT("failed"), Failed);
	JobsSnapshot->TryGetNumberField(TEXT("tracked_jobs"), Tracked);
	JobsSnapshot->TryGetNumberField(TEXT("total_submissions"), Submitted);

	const UE::SOMOLMCP::FSololmcpTcpTransport::FTransportStats Stats = UE::SOMOLMCP::GetTransportStats();
	const int32 WarningCount = Stats.TotalRejected + Stats.TotalAuthRejected + Stats.TotalRateLimitRejected
		+ Stats.TotalConnectionLimitRejected + Stats.TotalFramingBlockedClosed + static_cast<int32>(Blocked) + static_cast<int32>(Failed);

	const UE::SOMOLMCP::FSololmcpToolRuntimeSnapshot ToolSnapshot = UE::SOMOLMCP::GetToolRuntimeSnapshot();
	const FString RawClientSummary = UE::SOMOLMCP::GetClientSummary();
	const FString ClientSummary = RawClientSummary.IsEmpty()
		? (IsSomolMcpChineseEditorCulture() ? FString(TEXT("无已初始化客户端")) : FString(TEXT("No initialized clients")))
		: RawClientSummary;
	const FString CurrentTool = ToolSnapshot.CurrentToolName.IsEmpty()
		? (ToolSnapshot.LastToolName.IsEmpty()
			? (IsSomolMcpChineseEditorCulture() ? FString(TEXT("无正在执行的工具")) : FString(TEXT("No tool currently executing")))
			: (IsSomolMcpChineseEditorCulture()
				? FString::Printf(TEXT("当前空闲；最近执行：%s（%s，%.1fms）"), *ToolSnapshot.LastToolName, ToolSnapshot.bLastToolSuccess ? TEXT("成功") : TEXT("失败"), ToolSnapshot.LastToolElapsedMs)
				: FString::Printf(TEXT("Idle; last: %s (%s, %.1fms)"), *ToolSnapshot.LastToolName, ToolSnapshot.bLastToolSuccess ? TEXT("success") : TEXT("failed"), ToolSnapshot.LastToolElapsedMs)))
		: (IsSomolMcpChineseEditorCulture()
			? FString::Printf(TEXT("%s\n参数：%s"), *ToolSnapshot.CurrentToolName, *ToolSnapshot.CurrentToolArgumentsPreview)
			: FString::Printf(TEXT("%s\nArguments: %s"), *ToolSnapshot.CurrentToolName, *ToolSnapshot.CurrentToolArgumentsPreview));
	const FString ProtectionState = bMcpEditorProtectionActive
		? (IsSomolMcpChineseEditorCulture() ? TEXT("已启用（任务正在执行）") : TEXT("enabled (task execution active)"))
		: (GIsRunningUnattendedScript
			? (IsSomolMcpChineseEditorCulture() ? TEXT("由编辑器外部无人值守模式控制") : TEXT("controlled by the editor's external unattended mode"))
			: (IsSomolMcpChineseEditorCulture() ? TEXT("已解除（队列空闲，可人工操作）") : TEXT("released (queue idle; manual editing available)")));

	return FText::FromString(IsSomolMcpChineseEditorCulture()
		? FString::Printf(
			TEXT("编辑器保护：%s。仅在队列或工具执行期间抑制阻塞弹窗；空闲时自动恢复人工操作。\n当前连接工具：%s\n当前执行工具名：%s\n任务队列：排队 %d / 运行 %d / 阻塞 %d / 失败 %d / 当前追踪 %d / 启动后累计 %d\n工具指令累计：开始 %lld / 完成 %lld / 成功 %lld / 失败 %lld / 当前运行 %d\n连接：%d · 收 %d · 发 %d · 告警 %d"),
			*ProtectionState,
			*ClientSummary,
			*CurrentTool,
			static_cast<int32>(Queued),
			static_cast<int32>(Running),
			static_cast<int32>(Blocked),
			static_cast<int32>(Failed),
			static_cast<int32>(Tracked),
			static_cast<int32>(Submitted),
			ToolSnapshot.TotalToolExecutionsStarted,
			ToolSnapshot.TotalToolExecutionsCompleted,
			ToolSnapshot.TotalToolExecutionsSucceeded,
			ToolSnapshot.TotalToolExecutionsFailed,
			ToolSnapshot.ActiveToolExecutions,
			Stats.ActiveConnections,
			Stats.TotalMessagesReceived,
			Stats.TotalMessagesSent,
			WarningCount)
		: FString::Printf(
			TEXT("Editor protection: %s. Blocking dialogs are suppressed only while queued work or a tool is executing; idle mode restores manual editing.\nConnected client tools: %s\nCurrent executing tool: %s\nTask queue: queued %d / running %d / blocked %d / failed %d / tracked %d / submitted since startup %d\nTool commands since editor start: started %lld / completed %lld / succeeded %lld / failed %lld / active %d\nConnections: %d · received %d · sent %d · warnings %d"),
			*ProtectionState,
			*ClientSummary,
			*CurrentTool,
			static_cast<int32>(Queued),
			static_cast<int32>(Running),
			static_cast<int32>(Blocked),
			static_cast<int32>(Failed),
			static_cast<int32>(Tracked),
			static_cast<int32>(Submitted),
			ToolSnapshot.TotalToolExecutionsStarted,
			ToolSnapshot.TotalToolExecutionsCompleted,
			ToolSnapshot.TotalToolExecutionsSucceeded,
			ToolSnapshot.TotalToolExecutionsFailed,
			ToolSnapshot.ActiveToolExecutions,
			Stats.ActiveConnections,
			Stats.TotalMessagesReceived,
			Stats.TotalMessagesSent,
			WarningCount));
}

FText FSOMOLMCPModule::GetMcpTooltipText() const
{
	if (!UE::SOMOLMCP::IsServerRunning())
	{
		return SomolMcpText(
			TEXT("SOMOLMCP is stopped. Click to start the MCP TCP/HTTP service."),
			TEXT("SOMOLMCP 当前已停止。点击启动 MCP TCP/HTTP 服务。"));
	}

	const UE::SOMOLMCP::FSololmcpTcpTransport::FTransportStats Stats = UE::SOMOLMCP::GetTransportStats();
	return FText::FromString(IsSomolMcpChineseEditorCulture()
		? FString::Printf(
			TEXT("SOMOLMCP 正在运行\nTCP: %s:%d\nHTTP: %s:%d/mcp\n活动连接: %d\n已接收消息: %d\n已发送消息: %d\n点击停止 MCP 服务。"),
			*UE::SOMOLMCP::Bind,
			UE::SOMOLMCP::GetActualPort(),
			*UE::SOMOLMCP::Bind,
			UE::SOMOLMCP::HttpPort,
			Stats.ActiveConnections,
			Stats.TotalMessagesReceived,
			Stats.TotalMessagesSent)
		: FString::Printf(
			TEXT("SOMOLMCP is running\nTCP: %s:%d\nHTTP: %s:%d/mcp\nActive connections: %d\nMessages received: %d\nMessages sent: %d\nClick to stop the MCP service."),
			*UE::SOMOLMCP::Bind,
			UE::SOMOLMCP::GetActualPort(),
			*UE::SOMOLMCP::Bind,
			UE::SOMOLMCP::HttpPort,
			Stats.ActiveConnections,
			Stats.TotalMessagesReceived,
			Stats.TotalMessagesSent));
}

FSlateColor FSOMOLMCPModule::GetMcpStatusColor() const
{
	if (!UE::SOMOLMCP::IsServerRunning())
	{
		return FSlateColor(FLinearColor(0.45f, 0.48f, 0.52f, 1.0f));
	}

	const UE::SOMOLMCP::FSololmcpTcpTransport::FTransportStats Stats = UE::SOMOLMCP::GetTransportStats();
	if (Stats.ActiveConnections > 0)
	{
		return FSlateColor(FLinearColor(0.05f, 0.72f, 0.24f, 1.0f));
	}

	return FSlateColor(FLinearColor(0.95f, 0.68f, 0.10f, 1.0f));
}

FSlateColor FSOMOLMCPModule::GetMcpBubbleColor() const
{
	if (!UE::SOMOLMCP::IsServerRunning())
	{
		return FSlateColor(FLinearColor(0.08f, 0.09f, 0.10f, 0.82f));
	}

	TSharedRef<FJsonObject> JobsSnapshot = MakeShared<FJsonObject>();
	UE::SOMOLMCP::FSololmcpJobService::BuildJobsSnapshotObject(JobsSnapshot);

	double Blocked = 0.0;
	double Failed = 0.0;
	double Running = 0.0;
	JobsSnapshot->TryGetNumberField(TEXT("blocked"), Blocked);
	JobsSnapshot->TryGetNumberField(TEXT("failed"), Failed);
	JobsSnapshot->TryGetNumberField(TEXT("running"), Running);

	const UE::SOMOLMCP::FSololmcpTcpTransport::FTransportStats Stats = UE::SOMOLMCP::GetTransportStats();
	if (Failed > 0.0 || Stats.TotalAuthRejected > 0 || Stats.TotalFramingBlockedClosed > 0)
	{
		return FSlateColor(FLinearColor(0.38f, 0.08f, 0.08f, 0.86f));
	}
	if (Blocked > 0.0 || Stats.TotalRejected > 0 || Stats.TotalRateLimitRejected > 0 || Stats.TotalConnectionLimitRejected > 0)
	{
		return FSlateColor(FLinearColor(0.40f, 0.24f, 0.05f, 0.86f));
	}
	if (Running > 0.0)
	{
		return FSlateColor(FLinearColor(0.05f, 0.18f, 0.32f, 0.86f));
	}

	return FSlateColor(FLinearColor(0.06f, 0.17f, 0.10f, 0.82f));
}
#endif

IMPLEMENT_MODULE(FSOMOLMCPModule, SOMOLMCP)
