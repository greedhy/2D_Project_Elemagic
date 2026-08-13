// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "HttpResultCallback.h"

class IHttpRouter;
struct FHttpServerRequest;

namespace UE::SOMOLMCP
{
	class FSololmcpHttpTransport final
	{
	public:
		using FConnectionId = uint64;
		using FOnMessage = TFunction<FString(FConnectionId ConnectionId, const FString& SessionId, const FString& Json)>;
		using FOnSessionClosed = TFunction<void(FConnectionId ConnectionId)>;

		struct FConfig
		{
			FString AuthToken;
			int32 MaxRequestsPerMinute = 1200;
			int32 MaxSessions = 8192;
			int32 MaxBodyBytes = 64 * 1024 * 1024;
			int32 IdleSessionTimeoutSeconds = 600;
			int32 SessionCleanupIntervalSeconds = 10;
			int32 ExpiredSessionTombstoneSeconds = 600;
			int32 MaxExpiredSessionTombstones = 8192;
		};

		struct FStats
		{
			int32 ActiveSessions = 0;
			int32 TotalRequests = 0;
			int32 TotalRejected = 0;
			int32 TotalNotifications = 0;
			int32 TotalRateLimited = 0;
			int32 TotalExpiredSessions = 0;
			int32 TotalInvalidSessionRequests = 0;
			int32 TotalExpiredSessionRequests = 0;
			int32 TotalMissingSessionRequests = 0;
			int32 TotalCapacityRejected = 0;
			int32 PeakSessions = 0;
		};

		~FSololmcpHttpTransport();

		bool Start(int32 InPort, const FConfig& InConfig, FOnMessage&& InOnMessage, FOnSessionClosed&& InOnSessionClosed);
		void Stop();
		void Tick();
		bool IsRunning() const { return bRunning; }
		int32 GetPort() const { return Port; }
		FStats GetStats() const;
		bool OwnsConnection(FConnectionId ConnectionId) const;
		void SendJsonString(FConnectionId ConnectionId, const FString& Json);

	private:
		struct FSession
		{
			FString SessionId;
			FConnectionId ConnectionId = 0;
			double LastActivity = 0.0;
			TArray<double> RequestTimestamps;
			TArray<FString> PendingNotifications;
			bool bInitialized = false;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
			TSharedPtr<FHttpResultCallback> SseCallback;
#endif
		};

		bool HandleMcpRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
		bool HandleHealthRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
		bool HandleOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
		bool IsAuthorized(const FHttpServerRequest& Request) const;
		bool IsLoopback(const FHttpServerRequest& Request) const;
		enum class ESessionResolveResult : uint8
		{
			Found,
			Missing,
			Invalid,
			Expired,
			Capacity
		};

		bool IsRateLimited(FSession& Session, int32& OutRetryAfterSeconds);
		ESessionResolveResult ResolveSession(const FHttpServerRequest& Request, bool bCreate, FString& OutSessionId, TSharedPtr<FSession>& OutSession);
		FString ReadHeader(const FHttpServerRequest& Request, const FString& Name) const;
		TUniquePtr<struct FHttpServerResponse> MakeJsonResponse(const FString& Body, enum class EHttpServerResponseCodes Code, const FString& SessionId = TEXT("")) const;
		void AddCommonHeaders(struct FHttpServerResponse& Response, const FString& SessionId = TEXT("")) const;
		void RemoveSession(const FString& SessionId, bool bExpired = false);
		void CleanupExpiredSessions(bool bForce = false);
		void NotifySessionClosed(FConnectionId ConnectionId);
		void FlushPendingNotifications(FSession& Session);

	private:
		int32 Port = 12001;
		FConfig Config;
		FOnMessage OnMessage;
		FOnSessionClosed OnSessionClosed;
		TSharedPtr<IHttpRouter> HttpRouter;
		FHttpRouteHandle McpRoute;
		FHttpRouteHandle HealthRoute;
		TMap<FString, TSharedPtr<FSession>> Sessions;
		TMap<FConnectionId, FString> SessionByConnection;
		TMap<FString, double> ExpiredSessionTombstones;
		FConnectionId NextConnectionId = 0x8000000000000001ull;
		mutable FStats Stats;
		mutable FCriticalSection SessionMutex;
		double LastSessionCleanupSeconds = 0.0;
		bool bRunning = false;
	};
}
