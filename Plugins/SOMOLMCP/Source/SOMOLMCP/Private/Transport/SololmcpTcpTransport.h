// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FSocket;

namespace UE::SOMOLMCP
{
	class FSololmcpTcpTransport final
	{
	public:
		using FConnectionId = uint64;
		using FOnMessage = TFunction<void(FConnectionId ConnectionId, const FString& Json)>;

		struct FSecurityConfig
		{
			FString AuthToken;           // Empty = disabled for loopback only
			int32 MaxRequestsPerMinute = 1200; // 0 = unlimited
			int32 MaxConnections = 8192;       // 0 = unlimited
		};

		~FSololmcpTcpTransport();

		bool Start(const FString& InBindAddress, int32 InPort, const FSecurityConfig& InSecurity, FOnMessage&& InOnMessage);
		void Stop();
		bool Tick();
		bool IsRunning() const;
		void SendJsonString(FConnectionId ConnectionId, const FString& Json);

		/// Returns the port actually bound (may differ from the requested port
		/// when another UE editor already occupies it — see auto-increment in
		/// EnsureListener). Used by SololmcpInstanceRegistry so the file on
		/// disk always reflects reality, and by Router::HandleInitialize so
		/// clients can display/route the real endpoint.
		int32 GetActualPort() const { return Port; }
		const FString& GetBindAddress() const { return BindAddress; }

		/// Connection stats for health monitoring (thread-safe read via GetStats).
		struct FTransportStats
		{
			int32 ActiveConnections = 0;
			int32 TotalAccepted = 0;
			int32 TotalRejected = 0;      // auth/rate-limit/connection cap
			int32 TotalMessagesReceived = 0;
			int32 TotalMessagesSent = 0;
			int32 ActiveConnectionsHighWater = 0;
			int32 TotalAuthRejected = 0;
			int32 TotalRateLimitRejected = 0;
			int32 TotalConnectionLimitRejected = 0;
			int32 TotalClosed = 0;
			int32 TotalPeerClosed = 0;
			int32 TotalStaleClosed = 0;
			int32 TotalConnectionStateClosed = 0;
			int32 TotalReadErrorClosed = 0;
			int32 TotalSendErrorClosed = 0;
			int32 TotalFramingErrorClosed = 0;
			int32 TotalFramingBlockedClosed = 0;
			int32 TotalMissingSocketClosed = 0;
			int32 TotalStopClosed = 0;
			int32 TotalCloseWaitCleanup = 0;
		};
		FTransportStats GetStats() const;

	private:
		enum class ECloseReason : uint8
		{
			Unknown,
			Stop,
			MissingSocket,
			ConnectionState,
			StaleIdle,
			PeerNoPayload,
			ReadError,
			SendError,
			FramingError,
			FramingBlock
		};

		bool EnsureListener();
		bool AcceptClients();
		void CloseClient(FConnectionId ConnectionId, ECloseReason Reason = ECloseReason::Unknown);
		void RecordCloseStats(ECloseReason Reason);
		void PumpIncoming();
		void PumpOutgoing();
		bool IsRateLimited(FConnectionId ConnectionId) const;
		bool IsAuthTokenValid(const FString& JsonPayload) const;

	private:
		// 64 MB — must match client-side MAX_MESSAGE_BYTES in ue_mcp_tcp_client.rs.
		// Heightmap payloads (4K×4K 16-bit) run ~43 MB base64; 16 MB caused mid-transfer desync + crashes.
		static constexpr int32 MaxMessageBytes = 64 * 1024 * 1024;

		FString BindAddress = TEXT("127.0.0.1");
		int32 Port = 12000;
		FSecurityConfig Security;
		FSocket* ListenSocket = nullptr;
		FConnectionId NextConnectionId = 1;
		struct FClientConnection
		{
			FSocket* Socket = nullptr;
			TArray<uint8> RxBuffer;
			TArray<uint8> FrameBuffer;
			// FIXED #1: 用 offset 替代 RemoveAt(0,N) 避免每次消费消息时 O(N) memmove
			int32 FrameBufferOffset = 0;

			// Non-blocking send needs buffering because Send() may do a partial write.
			TArray<uint8> PendingSend;
			int32 PendingSendOffset = 0;

			// Security: rate-limit sliding window
			TArray<double> RequestTimestamps;
			bool bAuthenticated = false;
			FString RemoteAddress;
			// Idle timeout: track last activity for stale connection cleanup
			double LastActivityTime = 0.0;
		};
		TMap<FConnectionId, TSharedPtr<FClientConnection>> Clients;
		FOnMessage OnMessage;
		bool bRunning = false;

		// Stats (written on game thread, read via GetStats)
		mutable FTransportStats Stats;
	};
}
