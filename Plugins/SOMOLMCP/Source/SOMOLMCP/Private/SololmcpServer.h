// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Transport/SololmcpTcpTransport.h"
namespace UE::SOMOLMCP
{
	class FSololmcpRouter;
	class FSololmcpToolRegistry;
	class FSololmcpInstanceRegistry;
	class FSololmcpHttpTransport;

	class FSololmcpServer final
	{
	public:
		FSololmcpServer();
		~FSololmcpServer();

		bool Start(const FString& InBindAddress, int32 InPort, const FString& InAuthToken = TEXT(""), int32 InMaxRpm = 1200, int32 InMaxConnections = 8192, bool bEnableHttp = true, int32 InHttpPort = 12001);
		void Stop();
		bool IsRunning() const;

		/// Get transport stats for health monitoring.
		FSololmcpTcpTransport::FTransportStats GetTransportStats() const;

		/// Actual port we bound to (may differ from requested port when multiple
		/// editors run on the same host — see auto-increment in EnsureListener).
		int32 GetActualPort() const;

		/// Stable per-process uuid exposed on the wire in initialize.serverInfo.
		const FString& GetInstanceUuid() const;

		/// Human-readable initialized MCP clients, sourced from initialize.clientInfo.
		FString GetClientSummary() const;

	private:
		bool Tick(float DeltaTime);

	private:
		FString BindAddress = TEXT("127.0.0.1");
		int32 Port = 12000;

		TUniquePtr<FSololmcpToolRegistry> ToolRegistry;
		TUniquePtr<FSololmcpRouter> Router;
		TUniquePtr<FSololmcpTcpTransport> Transport;
		TUniquePtr<FSololmcpHttpTransport> HttpTransport;
		TUniquePtr<FSololmcpInstanceRegistry> InstanceRegistry;

		FTSTicker::FDelegateHandle TickHandle;
		bool bRunning = false;
	};
}
