// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Transport/SololmcpTcpTransport.h"

namespace UE::SOMOLMCP
{
	class FSololmcpToolRegistry;

	class FSololmcpRouter final
	{
	public:
		/// Multi-instance metadata surfaced to clients in `initialize.serverInfo`
		/// so a Tauri client can correlate a TCP socket with the file-system
		/// registry entry written by FSololmcpInstanceRegistry.
		struct FInstanceInfo
		{
			FString InstanceUuid;
			int32   ActualPort = 0;
			uint32  Pid = 0;
		};

		explicit FSololmcpRouter(FSololmcpToolRegistry& InRegistry);

		FString HandleMessage(const FString& JsonMessage);
		FString HandleMessage(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& JsonMessage);

		/// 每帧推进一次所有正在运行的 Job（在 Server Tick 中调用，确保 jobs/get 轮询模式也能推进任务）。
		void TickJobs();

		/// 注入 Transport Stats 获取回调（由 Server 在 Start 后调用，避免 Router 依赖 Server）。
		void SetTransportStatsGetter(TFunction<FSololmcpTcpTransport::FTransportStats()> InGetter);

		/// Inject instance-identity getter so HandleInitialize can fill serverInfo
		/// with the uuid/pid/actual_port written to %APPDATA%/SOMOLMCP/instances/.
		/// Server owns the registry and the transport; Router stays ignorant of both.
		void SetInstanceInfoGetter(TFunction<FInstanceInfo()> InGetter);

		void SetNotificationSender(TFunction<void(FSololmcpTcpTransport::FConnectionId, const FString&)> InSender);
		void RemoveSession(FSololmcpTcpTransport::FConnectionId ConnectionId);
		FString GetClientSummary() const;

	private:
		struct FMcpSession
		{
			FString SessionId;
			FSololmcpTcpTransport::FConnectionId ConnectionId = 0;
			FString ProtocolVersion;
			FString ClientName;
			FString ClientVersion;
			bool bClientRoots = false;
			bool bClientSampling = false;
			bool bClientElicitation = false;
			TArray<FString> RootUris;
			TSet<FString> SubscribedResourceUris;

			/**
			 * Tool groups this session has surfaced, beyond the always-present
			 * bootstrap set.
			 *
			 * An unfiltered tools/list over every registered tool is ~4.1M tokens,
			 * which no model can accept, so a session starts by seeing only the
			 * discovery and generic-dispatch tools and grows from there. Two things
			 * keep that from being a downgrade: tool_catalog still enumerates the
			 * complete inventory, and calling a tool outside this set still works --
			 * it activates the tool's group as a side effect rather than failing.
			 * Nothing is hidden in the sense of being unreachable.
			 *
			 * Mutable because activation happens as a side effect of tools/call,
			 * which is const: the visible set is bookkeeping about the connection,
			 * not part of the router's logical state.
			 */
			mutable TSet<FString> ActiveToolGroups;
			TMap<FString, int32> LastProgressSeqByJob;
			TMap<FString, TSharedPtr<class FJsonObject>> LoadedDataToolManifests;
		};

		struct FPendingServerRequest
		{
			FString RequestId;
			FString Method;
			FSololmcpTcpTransport::FConnectionId ConnectionId = 0;
			FDateTime CreatedUtc;
			FDateTime AnsweredUtc;
			bool bAnswered = false;
			bool bError = false;
			FString ErrorMessage;
			TSharedPtr<class FJsonObject> Response;
		};

		FString ReplyResult(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& ResultObj) const;
		FString ReplyError(const TSharedPtr<class FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<class FJsonObject>& Data = nullptr) const;
		FString HandleInitialize(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		// FIXED #11: BuildToolsList 现在是非 const，需去掉调用它的 handler 的 const 修饰
		FString HandleToolsList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleToolsetsList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleToolsetsDescribe(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleToolsetsLoad(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleToolsCall(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request) const;
		FString HandleResourcesList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id) const;
		FString HandleResourcesRead(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleResourcesSubscribe(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleResourcesUnsubscribe(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleRootsList(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id) const;
		FString HandlePromptsList(const TSharedPtr<class FJsonValue>& Id) const;
		FString HandlePromptsGet(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request) const;
		FString HandleSamplingCreateMessage(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request) const;
		FString HandleServerElicitationCreate(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleServerRequestsStatus(const TSharedPtr<class FJsonValue>& Id) const;
		FString HandleCompletionsComplete(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleServerStats(const TSharedPtr<class FJsonValue>& Id);
		FString HandleJobsSubmit(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleJobsElicit(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleJobsResume(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleJobsGet(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleJobsHeartbeat(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleJobsAwait(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleJobsCancel(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		FString HandleJobsEvents(const TSharedPtr<class FJsonValue>& Id, const TSharedRef<class FJsonObject>& Request);
		void StoreSessionFromInitialize(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedRef<class FJsonObject>& Request);
		void EmitNotification(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& Method, const TSharedRef<class FJsonObject>& Params) const;
		void EmitResourceUpdated(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& Uri, const FString& Reason) const;
		bool SendServerRequest(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& Method, const TSharedRef<class FJsonObject>& Params, FString& OutRequestId);
		FString HandleServerRequestResponse(FSololmcpTcpTransport::FConnectionId ConnectionId, const TSharedRef<class FJsonObject>& Message);
		bool ValidateToolsCallRoots(FSololmcpTcpTransport::FConnectionId ConnectionId, const FString& ToolName, const TSharedRef<class FJsonObject>& Arguments, FString& OutError) const;
		void AppendSessionDataDrivenTools(FSololmcpTcpTransport::FConnectionId ConnectionId, TArray<TSharedPtr<class FJsonValue>>& InOutTools) const;
		bool TryExecuteSessionDataDrivenTool(
			FSololmcpTcpTransport::FConnectionId ConnectionId,
			const FString& ToolName,
			const TSharedRef<class FJsonObject>& Arguments,
			bool& bOutHandled,
			TSharedRef<class FJsonObject>& OutStructured,
			FString& OutSummary,
			FString& OutError) const;

	private:
		FSololmcpToolRegistry& Registry;
		TFunction<FSololmcpTcpTransport::FTransportStats()> TransportStatsGetter;
		TFunction<FInstanceInfo()> InstanceInfoGetter;
		TFunction<void(FSololmcpTcpTransport::FConnectionId, const FString&)> NotificationSender;
		TMap<FSololmcpTcpTransport::FConnectionId, FMcpSession> SessionsByConnection;
		TMap<FString, FPendingServerRequest> PendingServerRequests;
		int64 NextServerRequestSeq = 0;
	};
}
