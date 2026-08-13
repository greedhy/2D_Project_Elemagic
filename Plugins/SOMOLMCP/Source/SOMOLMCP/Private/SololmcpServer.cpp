// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "SololmcpServer.h"
#include "SOMOLMCP.h"  // LogSOMOLMCP
#include "Protocol/SololmcpInstanceRegistry.h"
#include "Protocol/SololmcpRouter.h"
#include "Tools/SololmcpToolRegistry.h"
#include "Transport/SololmcpTcpTransport.h"
#include "Transport/SololmcpHttpTransport.h"
#include "HAL/PlatformProcess.h"
#include "SololmcpSharedLocks.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"

// Note: LogSOMOLMCP is declared in SOMOLMCP.h and defined here
DEFINE_LOG_CATEGORY(LogSOMOLMCP);

// v3.10.x worker-safety: pre-cached AssetRegistry pointer set at FSololmcpServer::Start.
// Worker-thread tools read this instead of calling FModuleManager::LoadModuleChecked
// (which is documented GameThread-only).
namespace UE::SOMOLMCP
{
	IAssetRegistry* GSololmcpCachedAssetRegistry = nullptr;
}

namespace UE::SOMOLMCP
{
	FSololmcpServer::FSololmcpServer()
	{
	}

	FSololmcpServer::~FSololmcpServer()
	{
		Stop();
	}

	bool FSololmcpServer::Start(const FString& InBindAddress, int32 InPort, const FString& InAuthToken, int32 InMaxRpm, int32 InMaxConnections, const bool bEnableHttp, int32 InHttpPort)
	{
		if (bRunning)
		{
			return true;
		}

		// v3.10.x worker-safety: pre-cache AssetRegistry pointer so worker-thread
		// tools (cook_pipeline_validate_assets, cook_pipeline_dependency_graph,
		// camera_anim_list, project_maps_list) can use it without calling
		// FModuleManager::LoadModuleChecked (which is GameThread-only).
		// FSololmcpServer::Start runs on the GameThread during module init.
		if (GSololmcpCachedAssetRegistry == nullptr)
		{
			FAssetRegistryModule& ARMod = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			GSololmcpCachedAssetRegistry = &ARMod.Get();
		}

		BindAddress = InBindAddress;
		Port = InPort;

		ToolRegistry = MakeUnique<FSololmcpToolRegistry>();
		Router = MakeUnique<FSololmcpRouter>(*ToolRegistry);
		Transport = MakeUnique<FSololmcpTcpTransport>();

		FSololmcpTcpTransport::FSecurityConfig SecurityConfig;
		SecurityConfig.AuthToken = InAuthToken;
		SecurityConfig.MaxRequestsPerMinute = InMaxRpm;
		SecurityConfig.MaxConnections = InMaxConnections;

		if (!Transport->Start(BindAddress, Port, SecurityConfig, [this](FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& Message)
		{
			if (Router && Transport)
			{
				const FString Response = Router->HandleMessage(ConnectionId, Message);
				if (!Response.IsEmpty())
				{
					Transport->SendJsonString(ConnectionId, Response);
				}
			}
		}))
		{
			Transport.Reset();
			Router.Reset();
			ToolRegistry.Reset();
			return false;
		}

		// Inject transport stats getter into Router (avoid circular Server→Router→Server dependency)
		Router->SetTransportStatsGetter([this]() { return Transport->GetStats(); });
		Router->SetNotificationSender([this](FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& Json)
		{
			if (Json.IsEmpty()) return;
			if (HttpTransport && HttpTransport->OwnsConnection(ConnectionId))
			{
				HttpTransport->SendJsonString(ConnectionId, Json);
			}
			else if (Transport)
			{
				Transport->SendJsonString(ConnectionId, Json);
			}
		});

		if (bEnableHttp)
		{
			HttpTransport = MakeUnique<FSololmcpHttpTransport>();
			FSololmcpHttpTransport::FConfig HttpConfig;
			HttpConfig.AuthToken = InAuthToken;
			HttpConfig.MaxRequestsPerMinute = InMaxRpm;
			// One operator-facing CVar controls both TCP connections and HTTP logical
			// sessions. The default is 8192; deployments may lower it (or use 0 for unlimited).
			HttpConfig.MaxSessions = InMaxConnections;
			if (!HttpTransport->Start(InHttpPort, HttpConfig,
				[this](FSololmcpHttpTransport::FConnectionId ConnectionId, const FString& SessionId, const FString& Message) -> FString
				{
					return Router ? Router->HandleMessage(ConnectionId, Message) : FString();
				},
				[this](FSololmcpHttpTransport::FConnectionId ConnectionId)
				{
					if (Router)
					{
						Router->RemoveSession(ConnectionId);
					}
				}))
			{
				UE_LOG(LogSOMOLMCP, Warning, TEXT("SOMOLMCP TCP started, but Streamable HTTP failed on port %d"), InHttpPort);
				HttpTransport.Reset();
			}
		}

		// Register this instance on disk so the Tauri client can enumerate all
		// running UE editors on the host. Uses the *actual* bound port, which
		// may differ from InPort if auto-increment kicked in.
		const int32 ActualPort = Transport->GetActualPort();
		InstanceRegistry = MakeUnique<FSololmcpInstanceRegistry>();
		InstanceRegistry->Start(ActualPort);

		// Expose instance info to the Router so initialize's serverInfo carries
		// uuid/pid/actual_port — clients can correlate a TCP connection with
		// the registry file without a second round-trip.
		Router->SetInstanceInfoGetter([this]() -> FSololmcpRouter::FInstanceInfo
		{
			FSololmcpRouter::FInstanceInfo Info;
			if (InstanceRegistry)
			{
				Info.InstanceUuid = InstanceRegistry->GetInstanceUuid();
			}
			Info.ActualPort = Transport ? Transport->GetActualPort() : 0;
			Info.Pid = FPlatformProcess::GetCurrentProcessId();
			return Info;
		});

		bRunning = true;
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSololmcpServer::Tick));
		UE_LOG(LogSOMOLMCP, Log, TEXT("SOMOLMCP listening on %s:%d"), *BindAddress, ActualPort);
		return true;
	}

	void FSololmcpServer::Stop()
	{
		if (!bRunning)
		{
			return;
		}

		bRunning = false;
		if (TickHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
			TickHandle.Reset();
		}

		if (InstanceRegistry)
		{
			InstanceRegistry->Stop();
		}
		InstanceRegistry.Reset();

		if (Transport)
		{
			Transport->Stop();
		}
		Transport.Reset();
		if (HttpTransport)
		{
			HttpTransport->Stop();
		}
		HttpTransport.Reset();
		Router.Reset();
		ToolRegistry.Reset();
		UE_LOG(LogSOMOLMCP, Log, TEXT("SOMOLMCP stopped"));
	}

	int32 FSololmcpServer::GetActualPort() const
	{
		return Transport ? Transport->GetActualPort() : Port;
	}

	const FString& FSololmcpServer::GetInstanceUuid() const
	{
		static const FString Empty;
		return InstanceRegistry ? InstanceRegistry->GetInstanceUuid() : Empty;
	}

	FString FSololmcpServer::GetClientSummary() const
	{
		return Router ? Router->GetClientSummary() : FString();
	}

	bool FSololmcpServer::IsRunning() const
	{
		return bRunning;
	}

	FSololmcpTcpTransport::FTransportStats FSololmcpServer::GetTransportStats() const
	{
		if (Transport)
		{
			return Transport->GetStats();
		}
		return FSololmcpTcpTransport::FTransportStats();
	}

	bool FSololmcpServer::Tick(float DeltaTime)
	{
		if (!bRunning || !Transport)
		{
			return false;
		}

		// 先推进 Job（每帧驱动一步），再处理 TCP 收发。
		// 这样即使客户端使用 jobs/get 轮询（非阻塞），Job 也能自动推进，
		// 避免了原来只有 jobs/await 内部循环才能推进 Job 的问题。
		if (Router)
		{
			Router->TickJobs();
		}

		Transport->Tick();
		if (HttpTransport)
		{
			HttpTransport->Tick();
		}
		return true;
	}
}
