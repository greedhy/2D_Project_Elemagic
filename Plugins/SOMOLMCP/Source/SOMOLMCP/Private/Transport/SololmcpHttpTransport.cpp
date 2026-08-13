// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Transport/SololmcpHttpTransport.h"

#include "Containers/StringConv.h"
#include "HttpPath.h"
#include "HttpServerConstants.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "IPAddress.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPHttpTransport, Log, All);

namespace UE::SOMOLMCP
{
	static FString JsonError(const int32 Code, const FString& Message, const FString& ReasonCode = TEXT(""))
	{
		const FString Data = ReasonCode.IsEmpty()
			? FString()
			: FString::Printf(TEXT(",\"data\":{\"reason_code\":\"%s\"}"), *ReasonCode.ReplaceCharWithEscapedChar());
		return FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"%s},\"id\":null}"), Code, *Message.ReplaceCharWithEscapedChar(), *Data);
	}

	static bool ParseRequestMethod(const FString& Json, FString& OutMethod)
	{
		TSharedPtr<FJsonObject> RequestObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, RequestObject)
			&& RequestObject.IsValid()
			&& RequestObject->TryGetStringField(TEXT("method"), OutMethod)
			&& !OutMethod.IsEmpty();
	}

	static bool ResponseHasError(const FString& Json)
	{
		TSharedPtr<FJsonObject> ResponseObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, ResponseObject)
			&& ResponseObject.IsValid()
			&& ResponseObject->HasField(TEXT("error"));
	}

	static FString MapInitializeResponseSessionId(const FString& Json, const FString& TransportSessionId)
	{
		TSharedPtr<FJsonObject> ResponseObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, ResponseObject) || !ResponseObject.IsValid())
		{
			return Json;
		}
		const TSharedPtr<FJsonObject>* ResultObject = nullptr;
		if (!ResponseObject->TryGetObjectField(TEXT("result"), ResultObject) || !ResultObject || !ResultObject->IsValid())
		{
			return Json;
		}
		(*ResultObject)->SetStringField(TEXT("sessionId"), TransportSessionId);
		FString MappedJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&MappedJson);
		FJsonSerializer::Serialize(ResponseObject.ToSharedRef(), Writer);
		return MappedJson;
	}

	FSololmcpHttpTransport::~FSololmcpHttpTransport()
	{
		Stop();
	}

	bool FSololmcpHttpTransport::Start(const int32 InPort, const FConfig& InConfig, FOnMessage&& InOnMessage, FOnSessionClosed&& InOnSessionClosed)
	{
		if (bRunning) return true;
		Port = InPort;
		Config = InConfig;
		Config.MaxSessions = FMath::Max(0, Config.MaxSessions);
		Config.SessionCleanupIntervalSeconds = FMath::Max(1, Config.SessionCleanupIntervalSeconds);
		Config.ExpiredSessionTombstoneSeconds = FMath::Max(Config.SessionCleanupIntervalSeconds, Config.ExpiredSessionTombstoneSeconds);
		Config.MaxExpiredSessionTombstones = FMath::Max(1, Config.MaxExpiredSessionTombstones);
		OnMessage = MoveTemp(InOnMessage);
		OnSessionClosed = MoveTemp(InOnSessionClosed);
		LastSessionCleanupSeconds = FPlatformTime::Seconds();

		FHttpServerModule& Module = FHttpServerModule::Get();
		HttpRouter = Module.GetHttpRouter(static_cast<uint32>(Port), true);
		if (!HttpRouter.IsValid())
		{
			UE_LOG(LogSOMOLMCPHttpTransport, Error, TEXT("Unable to bind Streamable HTTP port %d"), Port);
			return false;
		}

		const EHttpServerRequestVerbs McpVerbs = EHttpServerRequestVerbs::VERB_POST
			| EHttpServerRequestVerbs::VERB_GET
			| EHttpServerRequestVerbs::VERB_DELETE
			| EHttpServerRequestVerbs::VERB_OPTIONS;
#if UE_VERSION_OLDER_THAN(5, 4, 0)
		McpRoute = HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), McpVerbs,
			[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				return HandleMcpRequest(Request, OnComplete);
			});
		HealthRoute = HttpRouter->BindRoute(FHttpPath(TEXT("/health")), EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_OPTIONS,
			[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				return HandleHealthRequest(Request, OnComplete);
			});
#else
		McpRoute = HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), McpVerbs,
			FHttpRequestHandler::CreateRaw(this, &FSololmcpHttpTransport::HandleMcpRequest));
		HealthRoute = HttpRouter->BindRoute(FHttpPath(TEXT("/health")), EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_OPTIONS,
			FHttpRequestHandler::CreateRaw(this, &FSololmcpHttpTransport::HandleHealthRequest));
#endif
		if (!McpRoute.IsValid() || !HealthRoute.IsValid())
		{
			Stop();
			return false;
		}

		Module.StartAllListeners();
		bRunning = true;
		UE_LOG(LogSOMOLMCPHttpTransport, Log, TEXT("SOMOLMCP Streamable HTTP listening on http://127.0.0.1:%d/mcp"), Port);
		return true;
	}

	void FSololmcpHttpTransport::Stop()
	{
		bRunning = false;
		if (HttpRouter.IsValid())
		{
			if (McpRoute.IsValid()) HttpRouter->UnbindRoute(McpRoute);
			if (HealthRoute.IsValid()) HttpRouter->UnbindRoute(HealthRoute);
		}
		McpRoute.Reset();
		HealthRoute.Reset();
		HttpRouter.Reset();
		TArray<FConnectionId> ClosedConnections;
		{
			FScopeLock Lock(&SessionMutex);
			SessionByConnection.GenerateKeyArray(ClosedConnections);
			Sessions.Empty();
			SessionByConnection.Empty();
			ExpiredSessionTombstones.Empty();
		}
		for (const FConnectionId ConnectionId : ClosedConnections)
		{
			NotifySessionClosed(ConnectionId);
		}
		OnMessage = nullptr;
		OnSessionClosed = nullptr;
	}

	void FSololmcpHttpTransport::Tick()
	{
		CleanupExpiredSessions(false);
	}

	FSololmcpHttpTransport::FStats FSololmcpHttpTransport::GetStats() const
	{
		FScopeLock Lock(&SessionMutex);
		FStats Snapshot = Stats;
		Snapshot.ActiveSessions = Sessions.Num();
		return Snapshot;
	}

	bool FSololmcpHttpTransport::OwnsConnection(const FConnectionId ConnectionId) const
	{
		FScopeLock Lock(&SessionMutex);
		return SessionByConnection.Contains(ConnectionId);
	}

	FString FSololmcpHttpTransport::ReadHeader(const FHttpServerRequest& Request, const FString& Name) const
	{
		for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase) && Pair.Value.Num() > 0) return Pair.Value[0];
		}
		return TEXT("");
	}

	bool FSololmcpHttpTransport::IsLoopback(const FHttpServerRequest& Request) const
	{
		if (!Request.PeerAddress.IsValid()) return false;
		const FString Peer = Request.PeerAddress->ToString(false);
		return Peer == TEXT("::1") || Peer == TEXT("127.0.0.1") || Peer.StartsWith(TEXT("127."));
	}

	bool FSololmcpHttpTransport::IsAuthorized(const FHttpServerRequest& Request) const
	{
		if (Config.AuthToken.IsEmpty()) return IsLoopback(Request);
		const FString Authorization = ReadHeader(Request, TEXT("Authorization"));
		const FString ExplicitToken = ReadHeader(Request, TEXT("X-SOMOLMCP-Token"));
		return Authorization.Equals(TEXT("Bearer ") + Config.AuthToken, ESearchCase::CaseSensitive) || ExplicitToken == Config.AuthToken;
	}

	bool FSololmcpHttpTransport::IsRateLimited(FSession& Session, int32& OutRetryAfterSeconds)
	{
		OutRetryAfterSeconds = 0;
		if (Config.MaxRequestsPerMinute <= 0) return false;
		const double Now = FPlatformTime::Seconds();
		Session.RequestTimestamps.RemoveAll([Now](const double At) { return Now - At > 60.0; });
		if (Session.RequestTimestamps.Num() >= Config.MaxRequestsPerMinute)
		{
			const double OldestRequest = Session.RequestTimestamps.Num() > 0 ? Session.RequestTimestamps[0] : Now;
			OutRetryAfterSeconds = FMath::Max(1, FMath::CeilToInt(60.0 - (Now - OldestRequest)));
			return true;
		}
		Session.RequestTimestamps.Add(Now);
		return false;
	}

	FSololmcpHttpTransport::ESessionResolveResult FSololmcpHttpTransport::ResolveSession(
		const FHttpServerRequest& Request,
		const bool bCreate,
		FString& OutSessionId,
		TSharedPtr<FSession>& OutSession)
	{
		OutSessionId = ReadHeader(Request, TEXT("Mcp-Session-Id"));
		OutSession.Reset();
		FScopeLock Lock(&SessionMutex);
		if (!OutSessionId.IsEmpty())
		{
			if (const TSharedPtr<FSession>* Existing = Sessions.Find(OutSessionId))
			{
				OutSession = *Existing;
				OutSession->LastActivity = FPlatformTime::Seconds();
				return ESessionResolveResult::Found;
			}
			return ExpiredSessionTombstones.Contains(OutSessionId)
				? ESessionResolveResult::Expired
				: ESessionResolveResult::Invalid;
		}
		if (!bCreate)
		{
			return ESessionResolveResult::Missing;
		}
		if (Config.MaxSessions > 0 && Sessions.Num() >= Config.MaxSessions)
		{
			return ESessionResolveResult::Capacity;
		}

		OutSessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		OutSession = MakeShared<FSession>();
		OutSession->SessionId = OutSessionId;
		OutSession->ConnectionId = NextConnectionId++;
		OutSession->LastActivity = FPlatformTime::Seconds();
		Sessions.Add(OutSessionId, OutSession);
		SessionByConnection.Add(OutSession->ConnectionId, OutSessionId);
		Stats.PeakSessions = FMath::Max(Stats.PeakSessions, Sessions.Num());
		return ESessionResolveResult::Found;
	}

	void FSololmcpHttpTransport::AddCommonHeaders(FHttpServerResponse& Response, const FString& SessionId) const
	{
		Response.Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Authorization, Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version, X-SOMOLMCP-Token") });
		Response.Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, POST, DELETE, OPTIONS") });
		Response.Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
		if (!SessionId.IsEmpty()) Response.Headers.Add(TEXT("Mcp-Session-Id"), { SessionId });
	}

	TUniquePtr<FHttpServerResponse> FSololmcpHttpTransport::MakeJsonResponse(const FString& Body, const EHttpServerResponseCodes Code, const FString& SessionId) const
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = Code;
		AddCommonHeaders(*Response, SessionId);
		return Response;
	}

	bool FSololmcpHttpTransport::HandleOptions(const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Ok();
		Response->Code = EHttpServerResponseCodes::NoContent;
		AddCommonHeaders(*Response);
		OnComplete(MoveTemp(Response));
		return true;
	}

	bool FSololmcpHttpTransport::HandleHealthRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		if (Request.Verb == EHttpServerRequestVerbs::VERB_OPTIONS) return HandleOptions(Request, OnComplete);
		CleanupExpiredSessions(false);
		int32 ActiveSessions = 0;
		int32 InitializedSessions = 0;
		int32 ExpiredTombstones = 0;
		FStats Snapshot;
		{
			FScopeLock Lock(&SessionMutex);
			ActiveSessions = Sessions.Num();
			for (const TPair<FString, TSharedPtr<FSession>>& Pair : Sessions)
			{
				if (Pair.Value.IsValid() && Pair.Value->bInitialized)
				{
					++InitializedSessions;
				}
			}
			ExpiredTombstones = ExpiredSessionTombstones.Num();
			Snapshot = Stats;
		}
		const FString Body = FString::Printf(
			TEXT("{\"status\":\"ok\",\"transport\":\"streamable-http\",\"port\":%d,\"sessions\":%d,\"active_sessions\":%d,\"initialized_sessions\":%d,\"uninitialized_sessions\":%d,\"max_sessions\":%d,\"capacity_remaining\":%d,\"peak_sessions\":%d,\"max_requests_per_minute\":%d,\"idle_session_timeout_seconds\":%d,\"session_cleanup_interval_seconds\":%d,\"expired_session_tombstones\":%d,\"total_rejected\":%d,\"total_rate_limited\":%d,\"total_expired_sessions\":%d,\"total_invalid_session_requests\":%d,\"total_expired_session_requests\":%d,\"total_missing_session_requests\":%d,\"total_capacity_rejected\":%d,\"keepalive_method\":\"ping\",\"endpoint\":\"/mcp\"}"),
			Port,
			ActiveSessions,
			ActiveSessions,
			InitializedSessions,
			ActiveSessions - InitializedSessions,
			Config.MaxSessions,
			Config.MaxSessions > 0 ? FMath::Max(0, Config.MaxSessions - ActiveSessions) : -1,
			Snapshot.PeakSessions,
			Config.MaxRequestsPerMinute,
			Config.IdleSessionTimeoutSeconds,
			Config.SessionCleanupIntervalSeconds,
			ExpiredTombstones,
			Snapshot.TotalRejected,
			Snapshot.TotalRateLimited,
			Snapshot.TotalExpiredSessions,
			Snapshot.TotalInvalidSessionRequests,
			Snapshot.TotalExpiredSessionRequests,
			Snapshot.TotalMissingSessionRequests,
			Snapshot.TotalCapacityRejected);
		OnComplete(MakeJsonResponse(Body, EHttpServerResponseCodes::Ok));
		return true;
	}

	void FSololmcpHttpTransport::NotifySessionClosed(const FConnectionId ConnectionId)
	{
		if (OnSessionClosed)
		{
			OnSessionClosed(ConnectionId);
		}
	}

	void FSololmcpHttpTransport::RemoveSession(const FString& SessionId, const bool bExpired)
	{
		FConnectionId ClosedConnectionId = 0;
		{
			FScopeLock Lock(&SessionMutex);
			if (const TSharedPtr<FSession>* Session = Sessions.Find(SessionId))
			{
				if (bExpired && FPlatformTime::Seconds() - (*Session)->LastActivity < Config.IdleSessionTimeoutSeconds)
				{
					// The request raced the cleanup scan and renewed this session.
					return;
				}
				ClosedConnectionId = (*Session)->ConnectionId;
				SessionByConnection.Remove(ClosedConnectionId);
				Sessions.Remove(SessionId);
				if (bExpired)
				{
					ExpiredSessionTombstones.Add(SessionId, FPlatformTime::Seconds());
					++Stats.TotalExpiredSessions;
					if (ExpiredSessionTombstones.Num() > Config.MaxExpiredSessionTombstones)
					{
						FString OldestId;
						double OldestAt = TNumericLimits<double>::Max();
						for (const TPair<FString, double>& Pair : ExpiredSessionTombstones)
						{
							if (Pair.Value < OldestAt)
							{
								OldestAt = Pair.Value;
								OldestId = Pair.Key;
							}
						}
						ExpiredSessionTombstones.Remove(OldestId);
					}
				}
			}
		}
		if (ClosedConnectionId != 0)
		{
			NotifySessionClosed(ClosedConnectionId);
		}
	}

	void FSololmcpHttpTransport::CleanupExpiredSessions(const bool bForce)
	{
		if (!bRunning || Config.IdleSessionTimeoutSeconds <= 0)
		{
			return;
		}

		const double Now = FPlatformTime::Seconds();
		TArray<FString> ExpiredSessionIds;
		{
			FScopeLock Lock(&SessionMutex);
			if (!bForce && Now - LastSessionCleanupSeconds < Config.SessionCleanupIntervalSeconds)
			{
				return;
			}
			LastSessionCleanupSeconds = Now;
			for (const TPair<FString, TSharedPtr<FSession>>& Pair : Sessions)
			{
				if (Pair.Value.IsValid() && Now - Pair.Value->LastActivity >= Config.IdleSessionTimeoutSeconds)
				{
					ExpiredSessionIds.Add(Pair.Key);
				}
			}
			for (auto It = ExpiredSessionTombstones.CreateIterator(); It; ++It)
			{
				if (Now - It.Value() >= Config.ExpiredSessionTombstoneSeconds)
				{
					It.RemoveCurrent();
				}
			}
			while (ExpiredSessionTombstones.Num() > Config.MaxExpiredSessionTombstones)
			{
				FString OldestId;
				double OldestAt = TNumericLimits<double>::Max();
				for (const TPair<FString, double>& Pair : ExpiredSessionTombstones)
				{
					if (Pair.Value < OldestAt)
					{
						OldestAt = Pair.Value;
						OldestId = Pair.Key;
					}
				}
				ExpiredSessionTombstones.Remove(OldestId);
			}
		}

		for (const FString& SessionId : ExpiredSessionIds)
		{
			RemoveSession(SessionId, true);
		}
		if (ExpiredSessionIds.Num() > 0)
		{
			const int32 Remaining = GetStats().ActiveSessions;
			UE_LOG(LogSOMOLMCPHttpTransport, Log, TEXT("Expired %d idle MCP HTTP session(s); %d remain."), ExpiredSessionIds.Num(), Remaining);
		}
	}

	bool FSololmcpHttpTransport::HandleMcpRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		if (Request.Verb == EHttpServerRequestVerbs::VERB_OPTIONS) return HandleOptions(Request, OnComplete);
		{
			FScopeLock Lock(&SessionMutex);
			++Stats.TotalRequests;
		}
		if (!IsAuthorized(Request))
		{
			FScopeLock Lock(&SessionMutex);
			++Stats.TotalRejected;
			OnComplete(MakeJsonResponse(JsonError(-32001, TEXT("Authentication required")), EHttpServerResponseCodes::Denied));
			return true;
		}

		CleanupExpiredSessions(false);
		FString SessionId;
		TSharedPtr<FSession> Session;
		if (Request.Verb == EHttpServerRequestVerbs::VERB_DELETE)
		{
			const ESessionResolveResult ResolveResult = ResolveSession(Request, false, SessionId, Session);
			if (ResolveResult != ESessionResolveResult::Found)
			{
				const bool bExpired = ResolveResult == ESessionResolveResult::Expired;
				const bool bMissing = ResolveResult == ESessionResolveResult::Missing;
				{
					FScopeLock Lock(&SessionMutex);
					++Stats.TotalRejected;
					if (bMissing) ++Stats.TotalMissingSessionRequests;
					else if (bExpired) ++Stats.TotalExpiredSessionRequests;
					else ++Stats.TotalInvalidSessionRequests;
				}
				OnComplete(MakeJsonResponse(
					JsonError(bExpired ? -32013 : (bMissing ? -32011 : -32012), bExpired ? TEXT("MCP session expired") : (bMissing ? TEXT("Mcp-Session-Id header is required") : TEXT("Unknown Mcp-Session-Id")), bExpired ? TEXT("session_expired") : (bMissing ? TEXT("session_missing") : TEXT("session_invalid"))),
					EHttpServerResponseCodes::BadRequest));
				return true;
			}
			RemoveSession(SessionId);
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Ok();
			Response->Code = EHttpServerResponseCodes::NoContent;
			AddCommonHeaders(*Response);
			OnComplete(MoveTemp(Response));
			return true;
		}

		FString Json;
		FString Method;
		if (Request.Verb == EHttpServerRequestVerbs::VERB_POST)
		{
			if (Request.Body.Num() <= 0 || Request.Body.Num() > Config.MaxBodyBytes)
			{
				FScopeLock Lock(&SessionMutex);
				++Stats.TotalRejected;
				OnComplete(MakeJsonResponse(JsonError(-32700, TEXT("Invalid or oversized JSON body"), TEXT("invalid_json_body")), Request.Body.Num() > Config.MaxBodyBytes ? EHttpServerResponseCodes::RequestTooLarge : EHttpServerResponseCodes::BadRequest));
				return true;
			}
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
			Json = FString(Converter.Length(), Converter.Get());
			if (!ParseRequestMethod(Json, Method))
			{
				FScopeLock Lock(&SessionMutex);
				++Stats.TotalRejected;
				OnComplete(MakeJsonResponse(JsonError(-32700, TEXT("Parse error or missing JSON-RPC method"), TEXT("invalid_json_rpc_request")), EHttpServerResponseCodes::BadRequest));
				return true;
			}
		}

		const bool bInitializeRequest = Request.Verb == EHttpServerRequestVerbs::VERB_POST && Method == TEXT("initialize");
		const bool bHadSessionHeader = !ReadHeader(Request, TEXT("Mcp-Session-Id")).IsEmpty();
		if (bInitializeRequest && !bHadSessionHeader)
		{
			// A forced pass before the capacity check makes idle slots reusable even
			// when the periodic ticker has been delayed by a long editor operation.
			CleanupExpiredSessions(true);
		}
		const ESessionResolveResult ResolveResult = ResolveSession(Request, bInitializeRequest, SessionId, Session);
		if (ResolveResult != ESessionResolveResult::Found)
		{
			int32 ErrorCode = -32012;
			FString ErrorMessage = TEXT("Unknown Mcp-Session-Id");
			FString ReasonCode = TEXT("session_invalid");
			EHttpServerResponseCodes HttpCode = EHttpServerResponseCodes::BadRequest;
			{
				FScopeLock Lock(&SessionMutex);
				++Stats.TotalRejected;
				switch (ResolveResult)
				{
				case ESessionResolveResult::Missing:
					ErrorCode = -32011;
					ErrorMessage = TEXT("Mcp-Session-Id header is required; call initialize first");
					ReasonCode = TEXT("session_missing");
					++Stats.TotalMissingSessionRequests;
					break;
				case ESessionResolveResult::Expired:
					ErrorCode = -32013;
					ErrorMessage = TEXT("MCP session expired; call initialize again");
					ReasonCode = TEXT("session_expired");
					++Stats.TotalExpiredSessionRequests;
					break;
				case ESessionResolveResult::Capacity:
					ErrorCode = -32010;
					ErrorMessage = TEXT("MCP session capacity reached");
					ReasonCode = TEXT("session_capacity_reached");
					HttpCode = EHttpServerResponseCodes::ServiceUnavail;
					++Stats.TotalCapacityRejected;
					break;
				default:
					++Stats.TotalInvalidSessionRequests;
					break;
				}
			}
			OnComplete(MakeJsonResponse(JsonError(ErrorCode, ErrorMessage, ReasonCode), HttpCode));
			return true;
		}

		int32 RetryAfterSeconds = 0;
		bool bRateLimited = false;
		{
			FScopeLock Lock(&SessionMutex);
			bRateLimited = IsRateLimited(*Session, RetryAfterSeconds);
		}
		if (bRateLimited)
		{
			{
				FScopeLock Lock(&SessionMutex);
				++Stats.TotalRejected;
				++Stats.TotalRateLimited;
			}
			TUniquePtr<FHttpServerResponse> Response = MakeJsonResponse(
				JsonError(-32003, FString::Printf(TEXT("Rate limit exceeded; retry after %d second(s)"), RetryAfterSeconds)),
				EHttpServerResponseCodes::TooManyRequests,
				SessionId);
			Response->Headers.Add(TEXT("Retry-After"), { FString::FromInt(RetryAfterSeconds) });
			OnComplete(MoveTemp(Response));
			return true;
		}

		if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
			{
				FScopeLock Lock(&SessionMutex);
				Session->SseCallback = MakeShared<FHttpResultCallback>(OnComplete);
			}
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(TEXT("event: endpoint\ndata: /mcp\n\n"), TEXT("text/event-stream"));
			Response->Code = EHttpServerResponseCodes::Ok;
			Response->HttpVersion = HttpVersion::EHttpServerHttpVersion::HTTP_VERSION_1_1;
			Response->Flags = EHttpServerResponseFlags::MultipleWriteStream | EHttpServerResponseFlags::HasAdditionalWrites;
			Response->Headers.Add(TEXT("Connection"), { TEXT("keep-alive") });
			Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
			AddCommonHeaders(*Response, SessionId);
			OnComplete(MoveTemp(Response));
			FlushPendingNotifications(*Session);
#else
			FString Events;
			{
				FScopeLock Lock(&SessionMutex);
				for (const FString& Pending : Session->PendingNotifications) Events += TEXT("event: message\ndata: ") + Pending + TEXT("\n\n");
				Session->PendingNotifications.Reset();
			}
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Events, TEXT("text/event-stream"));
			Response->Code = EHttpServerResponseCodes::Ok;
			AddCommonHeaders(*Response, SessionId);
			OnComplete(MoveTemp(Response));
#endif
			return true;
		}

		if (Request.Verb != EHttpServerRequestVerbs::VERB_POST)
		{
			OnComplete(MakeJsonResponse(JsonError(-32600, TEXT("Method not allowed")), EHttpServerResponseCodes::BadMethod, SessionId));
			return true;
		}
		const bool bNewSession = bInitializeRequest && !bHadSessionHeader;
		FString Result = OnMessage ? OnMessage(Session->ConnectionId, SessionId, Json) : TEXT("");
		if (bInitializeRequest)
		{
			if (Result.IsEmpty() || ResponseHasError(Result))
			{
				if (bNewSession)
				{
					RemoveSession(SessionId);
					SessionId.Reset();
				}
			}
			else
			{
				{
					FScopeLock Lock(&SessionMutex);
					Session->bInitialized = true;
				}
				Result = MapInitializeResponseSessionId(Result, SessionId);
			}
		}
		if (Result.IsEmpty())
		{
			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Ok();
			Response->Code = EHttpServerResponseCodes::Accepted;
			AddCommonHeaders(*Response, SessionId);
			OnComplete(MoveTemp(Response));
		}
		else
		{
			OnComplete(MakeJsonResponse(Result, EHttpServerResponseCodes::Ok, SessionId));
		}
		return true;
	}

	void FSololmcpHttpTransport::FlushPendingNotifications(FSession& Session)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		TSharedPtr<FHttpResultCallback> Callback;
		TArray<FString> PendingNotifications;
		{
			FScopeLock Lock(&SessionMutex);
			if (!Session.SseCallback.IsValid()) return;
			Callback = Session.SseCallback;
			PendingNotifications = MoveTemp(Session.PendingNotifications);
			Session.PendingNotifications.Reset();
		}
		for (const FString& Pending : PendingNotifications)
		{
			TUniquePtr<FHttpServerResponse> Chunk = FHttpServerResponse::Create(TEXT("event: message\ndata: ") + Pending + TEXT("\n\n"), TEXT("text/event-stream"));
			Chunk->Code = EHttpServerResponseCodes::Ok;
			Chunk->HttpVersion = HttpVersion::EHttpServerHttpVersion::HTTP_VERSION_1_1;
			Chunk->Flags = EHttpServerResponseFlags::MultipleWriteStream | EHttpServerResponseFlags::HasAdditionalWrites | EHttpServerResponseFlags::SkipHeaderWrite;
			(*Callback)(MoveTemp(Chunk));
		}
#endif
	}

	void FSololmcpHttpTransport::SendJsonString(const FConnectionId ConnectionId, const FString& Json)
	{
		TSharedPtr<FSession> Session;
		bool bHasSseCallback = false;
		{
			FScopeLock Lock(&SessionMutex);
			const FString* SessionId = SessionByConnection.Find(ConnectionId);
			if (!SessionId) return;
			const TSharedPtr<FSession>* Existing = Sessions.Find(*SessionId);
			if (!Existing || !Existing->IsValid()) return;
			Session = *Existing;
			++Stats.TotalNotifications;
			Session->PendingNotifications.Add(Json);
			if (Session->PendingNotifications.Num() > 256) Session->PendingNotifications.RemoveAt(0, Session->PendingNotifications.Num() - 256, SOMOLMCP_NO_SHRINK);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
			bHasSseCallback = Session->SseCallback.IsValid();
#endif
		}
		// Server-to-client traffic deliberately does not renew LastActivity. Only
		// authenticated inbound requests (including standard ping) keep a session alive.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		if (bHasSseCallback)
		{
			FlushPendingNotifications(*Session);
			return;
		}
#endif
	}
}
