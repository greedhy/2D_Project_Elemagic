// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "Transport/SololmcpTcpTransport.h"
#include "SololmcpJsonUtils.h"
#include "Containers/StringConv.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPTransport, Log, All);

namespace UE::SOMOLMCP
{
	static bool IsLoopbackBindAddress(const FString& InBindAddress)
	{
		const FString NormalizedInput = InBindAddress.TrimStartAndEnd();
		if (NormalizedInput.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		FIPv4Address ParsedAddress;
		if (!FIPv4Address::Parse(NormalizedInput, ParsedAddress))
		{
			return false;
		}

		// IPv4 loopback is the whole 127.0.0.0/8 range.
		const FString NormalizedAddress = ParsedAddress.ToString();
		return NormalizedAddress == TEXT("127.0.0.1") || NormalizedAddress.StartsWith(TEXT("127."));
	}

	FSololmcpTcpTransport::~FSololmcpTcpTransport()
	{
		Stop();
	}

	bool FSololmcpTcpTransport::Start(const FString& InBindAddress, int32 InPort, const FSecurityConfig& InSecurity, FOnMessage&& InOnMessage)
	{
		if (bRunning)
		{
			return true;
		}

		BindAddress = InBindAddress;
		Port = InPort;
		Security = InSecurity;
		OnMessage = MoveTemp(InOnMessage);

		if (Security.AuthToken.IsEmpty() && !IsLoopbackBindAddress(BindAddress))
		{
			UE_LOG(LogSOMOLMCPTransport, Error,
				TEXT("Refusing to start SOMOLMCP transport on non-loopback or unresolved bind %s with empty somolmcp.auth.token"),
				*BindAddress);
			OnMessage = nullptr;
			return false;
		}

		if (!EnsureListener())
		{
			return false;
		}

		bRunning = true;

		FString SecurityNote;
		if (!Security.AuthToken.IsEmpty())
		{
			SecurityNote = TEXT(" (auth enabled)");
		}
		if (Security.MaxRequestsPerMinute > 0)
		{
			if (!SecurityNote.IsEmpty()) { SecurityNote += TEXT(", "); }
			SecurityNote += FString::Printf(TEXT("rate-limit=%d/min"), Security.MaxRequestsPerMinute);
		}
		if (Security.MaxConnections > 0)
		{
			if (!SecurityNote.IsEmpty()) { SecurityNote += TEXT(", "); }
			SecurityNote += FString::Printf(TEXT("max_conn=%d"), Security.MaxConnections);
		}

		UE_LOG(LogSOMOLMCPTransport, Log, TEXT("SOMOLMCP transport listening on %s:%d%s"), *BindAddress, Port, *SecurityNote);
		return true;
	}

	void FSololmcpTcpTransport::Stop()
	{
		// FIXED #3: 简化早期退出条件——只要 bRunning 已为 false 且无资源需要清理即可跳过。
		// 原条件 (!bRunning && !ListenSocket && Clients.Num() == 0) 在 Stop() 被重复调用时正确，
		// 但如果 Start 失败（bRunning=false 但 ListenSocket 可能非 null），会漏掉清理。
		// 新条件：若资源均已干净则跳过，否则始终清理。
		if (!bRunning && !ListenSocket && Clients.Num() == 0)
		{
			return;
		}

		bRunning = false;
		// On shutdown ISocketSubsystem may be torn down before us — guard the
		// pointer (deref crash on quit otherwise). If the subsystem is gone
		// we just leak the FSocket; the OS reclaims it when the process exits.
		ISocketSubsystem* SocketSubsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		for (TPair<FConnectionId, TSharedPtr<FClientConnection>>& Pair : Clients)
		{
			if (Pair.Value.IsValid())
			{
				RecordCloseStats(ECloseReason::Stop);
				if (Pair.Value->Socket)
				{
					Pair.Value->Socket->Close();
					if (SocketSubsys) { SocketSubsys->DestroySocket(Pair.Value->Socket); }
					Pair.Value->Socket = nullptr;
				}
			}
		}
		Clients.Empty();
		Stats.ActiveConnections = 0;
		if (ListenSocket)
		{
			ListenSocket->Close();
			if (SocketSubsys) { SocketSubsys->DestroySocket(ListenSocket); }
			ListenSocket = nullptr;
		}
		OnMessage = nullptr;
	}

	bool FSololmcpTcpTransport::Tick()
	{
		if (!bRunning)
		{
			return false;
		}
		// Always poll pending accepts so a fresh bridge process can take over promptly.
		AcceptClients();
		PumpIncoming();
		PumpOutgoing();
		return true;
	}

	bool FSololmcpTcpTransport::IsRunning() const
	{
		return bRunning;
	}

	FSololmcpTcpTransport::FTransportStats FSololmcpTcpTransport::GetStats() const
	{
		Stats.ActiveConnections = Clients.Num();
		Stats.ActiveConnectionsHighWater = FMath::Max(Stats.ActiveConnectionsHighWater, Stats.ActiveConnections);
		return Stats;
	}

	void FSololmcpTcpTransport::SendJsonString(FConnectionId ConnectionId, const FString& Json)
	{
		const TSharedPtr<FClientConnection>* ConnPtr = Clients.Find(ConnectionId);
		if (!ConnPtr || !ConnPtr->IsValid() || !(*ConnPtr)->Socket)
		{
			return;
		}

		FTCHARToUTF8 Utf8(*Json);
		const int32 MsgLen = Utf8.Length();
		if (MsgLen <= 0 || MsgLen > MaxMessageBytes)
		{
			return;
		}

		TArray<uint8> Out;
		Out.SetNumUninitialized(sizeof(int32) + MsgLen);
		FMemory::Memcpy(Out.GetData(), &MsgLen, sizeof(int32));
		FMemory::Memcpy(Out.GetData() + sizeof(int32), Utf8.Get(), MsgLen);
		TSharedPtr<FClientConnection> Conn = (*ConnPtr);
		// If we previously partially sent and advanced the offset, compact before enqueueing.
		if (Conn->PendingSendOffset > 0 && Conn->PendingSendOffset < Conn->PendingSend.Num())
		{
			Conn->PendingSend.RemoveAt(0, Conn->PendingSendOffset, SOMOLMCP_NO_SHRINK);
			Conn->PendingSendOffset = 0;
		}

		// If offset >= Num, clear it (message queue is effectively empty).
		if (Conn->PendingSendOffset >= Conn->PendingSend.Num())
		{
			Conn->PendingSend.Reset();
			Conn->PendingSendOffset = 0;
		}

		Conn->PendingSend.Append(Out);
		++Stats.TotalMessagesSent;
	}

	bool FSololmcpTcpTransport::EnsureListener()
	{
		FIPv4Address Addr;
		if (!FIPv4Address::Parse(BindAddress, Addr))
		{
			UE_LOG(LogSOMOLMCPTransport, Error, TEXT("Invalid bind address: %s"), *BindAddress);
			return false;
		}

		// Auto-increment port: if the configured port is already in use (e.g.
		// another UE editor is running on this host) try Port+1, Port+2, ...
		// up to `MaxBindAttempts` ports before giving up. This lets several
		// editors on the same machine each get their own MCP listener and
		// enables client-side discovery by port-scanning a small range.
		const int32 MaxBindAttempts = 20;
		const int32 StartPort = Port;
		for (int32 Attempt = 0; Attempt < MaxBindAttempts; ++Attempt)
		{
			const int32 TryPort = StartPort + Attempt;
			if (TryPort < 0 || TryPort > 65535)
			{
				break;
			}
			const FIPv4Endpoint Endpoint(Addr, static_cast<uint16>(TryPort));
			// Phase 3D-1: bumped backlog from 8 to 64. Default UE/OS backlog (5-8)
			// overflows under burst Connect load (4-16 parallel clients × ~10 calls/s
			// each opening a fresh TCP per call), causing "Connect refused" on the
			// client side before the server can Accept(). 64 covers a comfortable
			// burst margin without pushing OS limits (Windows SOMAXCONN default 200).
			ListenSocket = FTcpSocketBuilder(TEXT("SOMOLMCPListenSocket"))
				.AsReusable()
				.BoundToEndpoint(Endpoint)
				.Listening(8192);
			if (ListenSocket)
			{
				// Record which port we actually bound so serverInfo and logs reflect reality.
				Port = TryPort;
				if (Attempt > 0)
				{
					UE_LOG(LogSOMOLMCPTransport, Warning,
						TEXT("Configured port %d busy; SOMOLMCP listening on %s instead (attempt %d)"),
						StartPort, *Endpoint.ToString(), Attempt + 1);
				}
				else
				{
					UE_LOG(LogSOMOLMCPTransport, Log,
						TEXT("SOMOLMCP listening on %s"), *Endpoint.ToString());
				}
				ListenSocket->SetNonBlocking(true);
				return true;
			}
		}
		UE_LOG(LogSOMOLMCPTransport, Error,
			TEXT("Failed to bind any port in [%d, %d] on %s"),
			StartPort, StartPort + MaxBindAttempts - 1, *BindAddress);
		return false;
	}

	bool FSololmcpTcpTransport::IsAuthTokenValid(const FString& JsonPayload) const
	{
		if (Security.AuthToken.IsEmpty())
		{
			// No token configured = open access
			return true;
		}

		// Token can be provided in the "params._auth" field (for tools/call)
		// or in a top-level "_auth" field (for initialize / general messages)
		const TSharedPtr<FJsonObject> Parsed = ParseJsonObject(JsonPayload);
		if (!Parsed.IsValid())
		{
			return false;
		}

		// Check top-level _auth
		FString AuthValue;
		if (Parsed->TryGetStringField(TEXT("_auth"), AuthValue) && AuthValue == Security.AuthToken)
		{
			return true;
		}

		// Check params._auth
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (Parsed->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj && ParamsObj->IsValid())
		{
			if ((*ParamsObj)->TryGetStringField(TEXT("_auth"), AuthValue) && AuthValue == Security.AuthToken)
			{
				return true;
			}
		}

		return false;
	}

	bool FSololmcpTcpTransport::IsRateLimited(FConnectionId ConnectionId) const
	{
		if (Security.MaxRequestsPerMinute <= 0)
		{
			return false;
		}

		const TSharedPtr<FClientConnection>* ConnPtr = Clients.Find(ConnectionId);
		if (!ConnPtr || !ConnPtr->IsValid())
		{
			return false;
		}

		const TSharedPtr<FClientConnection> Conn = *ConnPtr;
		const double Now = FPlatformTime::Seconds();
		const double WindowSec = 60.0;

		// Remove timestamps older than 60 seconds
		while (!Conn->RequestTimestamps.IsEmpty() && (Now - Conn->RequestTimestamps[0]) > WindowSec)
		{
			Conn->RequestTimestamps.RemoveAt(0);
		}

		return Conn->RequestTimestamps.Num() >= Security.MaxRequestsPerMinute;
	}

	bool FSololmcpTcpTransport::AcceptClients()
	{
		if (!ListenSocket)
		{
			return false;
		}

		// Enforce max connections limit
		if (Security.MaxConnections > 0 && Clients.Num() >= Security.MaxConnections)
		{
			return false;
		}

		// Phase 3D-2: per-tick accept cap. Drains pending sockets in batches
		// instead of one-per-Tick (16ms is too slow for burst Connect storms),
		// but caps at 32 per tick so other Tick work (PumpIncoming/Outgoing)
		// doesn't starve when a flood arrives.
		constexpr int32 MaxAcceptsPerTick = 32;

		// Phase 3D-3: per-addr connection cap. Tally current Clients by remote
		// addr once up-front (cheap for our scales of 4-16 conns); reject any
		// new accept from an addr that already holds > PerAddrMax sockets.
		// Existing connections are not killed.
		// Phase 3D-3 hotfix v2: was 8 → 64 → 128 per user request. Localhost dev
		// accumulates many idle conns from test re-runs. Listen backlog is 64 so
		// 128 cap means listen backlog is the binding constraint, not per-addr.
		const int32 PerAddrMax = Security.MaxConnections > 0 ? Security.MaxConnections : 8192;
		TMap<FString, int32> PerAddrCount;
		for (const TPair<FConnectionId, TSharedPtr<FClientConnection>>& Pair : Clients)
		{
			if (Pair.Value.IsValid())
			{
				PerAddrCount.FindOrAdd(Pair.Value->RemoteAddress) += 1;
			}
		}

		bool bAnyAccepted = false;
		int32 AcceptedThisTick = 0;
		while (AcceptedThisTick < MaxAcceptsPerTick)
		{
			if (!ListenSocket)
			{
				break;
			}
			bool bHasPendingConnection = false;
			if (!ListenSocket->HasPendingConnection(bHasPendingConnection) || !bHasPendingConnection)
			{
				break;
			}

			// Re-check max connections inside loop
			if (Security.MaxConnections > 0 && Clients.Num() >= Security.MaxConnections)
			{
				break;
			}

			FSocket* NewSocket = ListenSocket->Accept(TEXT("SOMOLMCPClient"));
			if (!NewSocket)
			{
				break;
			}

			NewSocket->SetNoDelay(true);
			NewSocket->SetNonBlocking(true);

			// Extract remote address for logging — guard against post-shutdown SocketSubsystem.
			FString RemoteAddrStr(TEXT("unknown"));
			if (ISocketSubsystem* AcceptSocketSubsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				TSharedRef<FInternetAddr> RemoteAddr = AcceptSocketSubsys->CreateInternetAddr();
				if (NewSocket->GetPeerAddress(*RemoteAddr))
				{
					RemoteAddrStr = RemoteAddr->ToString(false);
				}
			}

			// Phase 3D-3: enforce per-addr cap. We've already accepted the socket
			// (FSocket::Accept does not let us peek peer + reject), so close it
			// immediately if this addr is over quota. One Warning log per refusal
			// — clients typically retry, which would spam Log if used at Log level.
			if (int32* ExistingCount = PerAddrCount.Find(RemoteAddrStr); ExistingCount && *ExistingCount >= PerAddrMax)
			{
				UE_LOG(LogSOMOLMCPTransport, Warning,
					TEXT("client overloaded: %s (active=%d, cap=%d) — refusing new connection"),
					*RemoteAddrStr, *ExistingCount, PerAddrMax);
				NewSocket->Close();
				if (ISocketSubsystem* DestroySubsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
				{
					DestroySubsys->DestroySocket(NewSocket);
				}
				++Stats.TotalRejected;
				++Stats.TotalConnectionLimitRejected;
				++AcceptedThisTick; // count toward the per-tick budget so a hostile addr can't starve good clients in the same loop
				continue;
			}

			const FConnectionId ConnectionId = NextConnectionId++;
			TSharedPtr<FClientConnection> Conn = MakeShared<FClientConnection>();
			Conn->Socket = NewSocket;
			Conn->RemoteAddress = RemoteAddrStr;
			Conn->LastActivityTime = FPlatformTime::Seconds();

			// If no auth token required, mark as authenticated immediately
			Conn->bAuthenticated = Security.AuthToken.IsEmpty();

			Clients.Add(ConnectionId, Conn);
			PerAddrCount.FindOrAdd(RemoteAddrStr) += 1;
			Stats.ActiveConnections = Clients.Num();
			Stats.ActiveConnectionsHighWater = FMath::Max(Stats.ActiveConnectionsHighWater, Stats.ActiveConnections);
			++Stats.TotalAccepted;
			++AcceptedThisTick;
			UE_LOG(LogSOMOLMCPTransport, Log, TEXT("SOMOLMCP client connected (id=%llu, addr=%s, auth=%s)"),
				ConnectionId, *RemoteAddrStr, Conn->bAuthenticated ? TEXT("skip") : TEXT("pending"));
			bAnyAccepted = true;
		}

		if (AcceptedThisTick >= MaxAcceptsPerTick)
		{
			UE_LOG(LogSOMOLMCPTransport, Verbose,
				TEXT("Hit per-tick accept cap (%d) — remaining pending sockets will drain next Tick"),
				MaxAcceptsPerTick);
		}
		return bAnyAccepted;
	}

	void FSololmcpTcpTransport::CloseClient(FConnectionId ConnectionId, FSololmcpTcpTransport::ECloseReason Reason)
	{
		TSharedPtr<FClientConnection>* ConnPtr = Clients.Find(ConnectionId);
		if (!ConnPtr || !ConnPtr->IsValid())
		{
			return;
		}

		TSharedPtr<FClientConnection> Conn = *ConnPtr;
		RecordCloseStats(Reason);
		UE_LOG(LogSOMOLMCPTransport, Log, TEXT("SOMOLMCP client disconnected (id=%llu, addr=%s, reason=%d)"),
			ConnectionId, *Conn->RemoteAddress, static_cast<int32>(Reason));
		if (Conn->Socket)
		{
			Conn->Socket->Close();
			if (ISocketSubsystem* CloseSocketSubsys = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				CloseSocketSubsys->DestroySocket(Conn->Socket);
			}
			Conn->Socket = nullptr;
		}
		Clients.Remove(ConnectionId);
		Stats.ActiveConnections = Clients.Num();
	}

	void FSololmcpTcpTransport::RecordCloseStats(FSololmcpTcpTransport::ECloseReason Reason)
	{
		++Stats.TotalClosed;
		switch (Reason)
		{
		case ECloseReason::Stop:
			++Stats.TotalStopClosed;
			break;
		case ECloseReason::MissingSocket:
			++Stats.TotalMissingSocketClosed;
			break;
		case ECloseReason::ConnectionState:
			++Stats.TotalConnectionStateClosed;
			++Stats.TotalPeerClosed;
			break;
		case ECloseReason::StaleIdle:
			++Stats.TotalStaleClosed;
			++Stats.TotalCloseWaitCleanup;
			break;
		case ECloseReason::PeerNoPayload:
			++Stats.TotalPeerClosed;
			++Stats.TotalCloseWaitCleanup;
			break;
		case ECloseReason::ReadError:
			++Stats.TotalReadErrorClosed;
			break;
		case ECloseReason::SendError:
			++Stats.TotalSendErrorClosed;
			break;
		case ECloseReason::FramingError:
			++Stats.TotalFramingErrorClosed;
			break;
		case ECloseReason::FramingBlock:
			++Stats.TotalFramingBlockedClosed;
			break;
		default:
			break;
		}
	}

	void FSololmcpTcpTransport::PumpIncoming()
	{
		// Per-addr framing-error rate-limit state. Static locals are Live-Coding safe.
		// GFramingErrorTimes: rolling 60s window of error timestamps per addr.
		// GFramingBlockUntil: addrs whose connections must be killed-on-sight until this time.
		static TMap<FString, TArray<double>> GFramingErrorTimes;
		static TMap<FString, double> GFramingBlockUntil;

		const double Now = FPlatformTime::Seconds();
		// Idle timeout: close connections that have been silent for too long.
		// Bumped to 300s (Apr 2026): a 30s threshold killed the Tauri client's
		// persistent control channel between user-driven actions and during
		// long PCG generations (validate → dry_run → generate can sit idle on
		// the wire for >30s while UE chews through Python work). The server-
		// side close looked like `WARN UE TCP read loop ended` on the client
		// and forced a reconnect storm during smoke runs. 300s still recovers
		// dead/crashed clients within 5 min, which is the original goal.
		// TODO: replace with TCP keepalive at socket level + bidirectional
		// heartbeat ping so we don't need a magic timeout at all.
		constexpr double IdleTimeoutSeconds = 300.0;

		TArray<FConnectionId> ClientIds;
		Clients.GenerateKeyArray(ClientIds);
		for (const FConnectionId ConnectionId : ClientIds)
		{
			TSharedPtr<FClientConnection>* ConnPtr = Clients.Find(ConnectionId);
			TSharedPtr<FClientConnection> Conn = (ConnPtr && ConnPtr->IsValid()) ? *ConnPtr : nullptr;
			if (!Conn.IsValid() || !Conn->Socket)
			{
				CloseClient(ConnectionId, ECloseReason::MissingSocket);
				continue;
			}

			// --- Per-addr block check ---
			// If this addr is currently blocked (recent framing-error burst), close
			// without logging per-connection spam. Single warning was emitted at
			// threshold-cross time; expired blocks are pruned lazily.
			if (const double* BlockUntil = GFramingBlockUntil.Find(Conn->RemoteAddress))
			{
				if (Now < *BlockUntil)
				{
					CloseClient(ConnectionId, ECloseReason::FramingBlock);
					continue;
				}
				else
				{
					GFramingBlockUntil.Remove(Conn->RemoteAddress);
				}
			}

			// --- Robust connection liveness check ---
			// GetConnectionState() alone is unreliable on Windows for CLOSE_WAIT.
			// Strategy: check ConnectionState first, then use Peek for idle connections.
			{
				const ESocketConnectionState State = Conn->Socket->GetConnectionState();
				if (State != SCS_Connected)
				{
					UE_LOG(LogSOMOLMCPTransport, Log, TEXT("ConnectionState!=Connected: closing conn=%llu (state=%d)"),
						ConnectionId, static_cast<int32>(State));
					CloseClient(ConnectionId, ECloseReason::ConnectionState);
					continue;
				}
			}

			// Idle timeout: if no activity for IdleTimeoutSeconds, probe with Peek then force close.
			if (Now - Conn->LastActivityTime > IdleTimeoutSeconds)
			{
				// Peek probe: 1-byte recv with Peek flag to detect FIN without consuming data.
				// On Windows CLOSE_WAIT: Peek should return 0 bytes (graceful close detected).
				// If Peek fails entirely, connection is definitely dead.
				uint8 PeekByte = 0;
				int32 PeekRead = 0;
				const bool bPeekOk = Conn->Socket->Recv(&PeekByte, 1, PeekRead, ESocketReceiveFlags::Peek);

				if (!bPeekOk || PeekRead == 0)
				{
					// Either Peek failed (dead) or returned 0 bytes (FIN received = CLOSE_WAIT)
					UE_LOG(LogSOMOLMCPTransport, Log, TEXT("Idle timeout: closing stale conn=%llu (addr=%s, idle=%.0fs, peekOk=%d, peekRead=%d)"),
						ConnectionId, *Conn->RemoteAddress, Now - Conn->LastActivityTime, bPeekOk ? 1 : 0, PeekRead);
					CloseClient(ConnectionId, ECloseReason::StaleIdle);
					continue;
				}
				// Peek returned data — connection is alive, reset idle timer
				Conn->LastActivityTime = Now;
			}

			uint32 PendingSize = 0;
			if (!Conn->Socket->HasPendingData(PendingSize))
			{
				// HasPendingData(false) can simply mean "no bytes available right
				// now" on idle request/response clients. Treat it as zero pending
				// data and let the readable+Peek probe below distinguish a live idle
				// socket from a peer that has already closed.
				PendingSize = 0;
			}

			if (PendingSize == 0)
			{
				// Windows can report CLOSE_WAIT as SCS_Connected with no pending
				// payload. Only probe when the socket is readable; a live idle
				// persistent client is not readable, while a FIN/closed peer is.
				if (Conn->PendingSend.Num() == 0 &&
					Conn->Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
				{
					uint8 PeekByte = 0;
					int32 PeekRead = 0;
					const bool bPeekOk = Conn->Socket->Recv(&PeekByte, 1, PeekRead, ESocketReceiveFlags::Peek);
					if (!bPeekOk || PeekRead == 0)
					{
						UE_LOG(LogSOMOLMCPTransport, Verbose,
							TEXT("Peer closed idle conn=%llu (addr=%s, peekOk=%d, peekRead=%d)"),
							ConnectionId, *Conn->RemoteAddress, bPeekOk ? 1 : 0, PeekRead);
						CloseClient(ConnectionId, ECloseReason::PeerNoPayload);
						continue;
					}
				}
				continue;
			}

			const int32 ReadSize = FMath::Min<int32>(static_cast<int32>(PendingSize), 64 * 1024);
			Conn->RxBuffer.SetNumUninitialized(ReadSize, SOMOLMCP_NO_SHRINK);

			int32 BytesRead = 0;
			if (!Conn->Socket->Recv(Conn->RxBuffer.GetData(), Conn->RxBuffer.Num(), BytesRead))
			{
				CloseClient(ConnectionId, ECloseReason::ReadError);
				continue;
			}

			if (BytesRead <= 0)
			{
				continue;
			}

			Conn->FrameBuffer.Append(Conn->RxBuffer.GetData(), BytesRead);
			Conn->LastActivityTime = FPlatformTime::Seconds();

			// FIXED #1: 用逻辑 offset 替代 RemoveAt(0,N)，避免每次消费后 O(N) memmove。
			// 当 offset 超过一半 buffer 大小或 >= 64KB 时做一次 compact 以控制内存。
			constexpr int32 CompactThreshold = 64 * 1024;

			while (Conn->FrameBuffer.Num() - Conn->FrameBufferOffset >= static_cast<int32>(sizeof(int32)))
			{
				const uint8* BufHead = Conn->FrameBuffer.GetData() + Conn->FrameBufferOffset;
				int32 MsgLen = 0;
				FMemory::Memcpy(&MsgLen, BufHead, sizeof(int32));
				if (MsgLen <= 0 || MsgLen > MaxMessageBytes)
				{
					// Better diagnostic: if length looks suspiciously high (>50MB) or negative,
					// dump first 4 bytes as hex + ASCII so caller can spot HTTP/text-prefix mistakes.
					constexpr int32 SuspiciousHigh = 50 * 1024 * 1024;
					if (MsgLen < 0 || MsgLen > SuspiciousHigh)
					{
						const uint8 B0 = BufHead[0];
						const uint8 B1 = BufHead[1];
						const uint8 B2 = BufHead[2];
						const uint8 B3 = BufHead[3];
						auto IsPrintable = [](uint8 C) { return C >= 0x20 && C < 0x7F; };
						const TCHAR C0 = IsPrintable(B0) ? static_cast<TCHAR>(B0) : TEXT('.');
						const TCHAR C1 = IsPrintable(B1) ? static_cast<TCHAR>(B1) : TEXT('.');
						const TCHAR C2 = IsPrintable(B2) ? static_cast<TCHAR>(B2) : TEXT('.');
						const TCHAR C3 = IsPrintable(B3) ? static_cast<TCHAR>(B3) : TEXT('.');
						const FString AsciiPreview = FString::Printf(TEXT("%c%c%c%c"), C0, C1, C2, C3);
						const bool bLooksHttp = (B0 == 0x43 && B1 == 0x6F && B2 == 0x6E && B3 == 0x74) // "Cont"
							|| (B0 == 0x47 && B1 == 0x45 && B2 == 0x54 && B3 == 0x20)                  // "GET "
							|| (B0 == 0x50 && B1 == 0x4F && B2 == 0x53 && B3 == 0x54);                 // "POST"
						UE_LOG(LogSOMOLMCPTransport, Error,
							TEXT("Invalid framing: first 4 bytes = 0x%02X 0x%02X 0x%02X 0x%02X (\"%s\")%s — server expects 4-byte LE length prefix, not HTTP headers (conn=%llu)"),
							B0, B1, B2, B3, *AsciiPreview,
							bLooksHttp ? TEXT(" (looks like HTTP \"Cont\"-prefixx)") : TEXT(""),
							ConnectionId);
					}
					else
					{
						UE_LOG(LogSOMOLMCPTransport, Error, TEXT("Invalid message length: %d (conn=%llu)"), MsgLen, ConnectionId);
					}

					// Record framing error for rate-limiting (keyed by addr).
					// Uses function-scope statics declared at top of PumpIncoming.
					{
						const double NowSec = FPlatformTime::Seconds();
						TArray<double>& Times = GFramingErrorTimes.FindOrAdd(Conn->RemoteAddress);
						// Prune entries older than 60s.
						Times.RemoveAll([NowSec](double T) { return (NowSec - T) > 60.0; });
						Times.Add(NowSec);
						if (Times.Num() >= 5)
						{
							// Block this addr for 30s: future iterations will close
							// any of its connections on sight (see per-iter check above).
							const double* Existing = GFramingBlockUntil.Find(Conn->RemoteAddress);
							if (!Existing || NowSec >= *Existing)
							{
								GFramingBlockUntil.Add(Conn->RemoteAddress, NowSec + 30.0);
								UE_LOG(LogSOMOLMCPTransport, Warning,
									TEXT("rate-limited addr=%s err=invalid_length"), *Conn->RemoteAddress);
							}
							// Reset window so we don't spam the warning on every subsequent error.
							Times.Reset();
						}
					}

					CloseClient(ConnectionId, ECloseReason::FramingError);
					break;
				}

				if (Conn->FrameBuffer.Num() - Conn->FrameBufferOffset < static_cast<int32>(sizeof(int32)) + MsgLen)
				{
					break;
				}

				const uint8* MsgBytes = BufHead + sizeof(int32);
				const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(MsgBytes), MsgLen);
				FString Json(Converter.Length(), Converter.Get());

				// 推进逻辑指针，而非 memmove
				Conn->FrameBufferOffset += static_cast<int32>(sizeof(int32)) + MsgLen;

				// --- Security checks ---
				// 1. Rate limiting (sliding window)
				if (IsRateLimited(ConnectionId))
				{
					UE_LOG(LogSOMOLMCPTransport, Warning, TEXT("Rate limited conn=%llu (addr=%s)"), ConnectionId, *Conn->RemoteAddress);
					++Stats.TotalRejected;
					++Stats.TotalRateLimitRejected;

					// Send rate-limit error
					TSharedRef<FJsonObject> ErrorResp = MakeShared<FJsonObject>();
					ErrorResp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
					ErrorResp->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
					TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
					ErrorObj->SetNumberField(TEXT("code"), -32029);
					ErrorObj->SetStringField(TEXT("message"), TEXT("Rate limit exceeded. Slow down."));
					ErrorResp->SetObjectField(TEXT("error"), ErrorObj);
					SendJsonString(ConnectionId, ToJsonString(ErrorResp));
					continue;
				}

				// Record request timestamp for rate limiting
				if (Security.MaxRequestsPerMinute > 0)
				{
					Conn->RequestTimestamps.Add(FPlatformTime::Seconds());
				}

				// 2. Auth token validation (once authenticated, skip for subsequent messages)
				if (!Conn->bAuthenticated)
				{
					if (!IsAuthTokenValid(Json))
					{
						UE_LOG(LogSOMOLMCPTransport, Warning, TEXT("Auth failed for conn=%llu (addr=%s)"), ConnectionId, *Conn->RemoteAddress);
						++Stats.TotalRejected;
						++Stats.TotalAuthRejected;

						// Send auth error
						TSharedRef<FJsonObject> ErrorResp = MakeShared<FJsonObject>();
						ErrorResp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
						ErrorResp->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
						TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
						ErrorObj->SetNumberField(TEXT("code"), -32028);
						ErrorObj->SetStringField(TEXT("message"), TEXT("Authentication required. Provide _auth in request."));
						ErrorResp->SetObjectField(TEXT("error"), ErrorObj);
						SendJsonString(ConnectionId, ToJsonString(ErrorResp));
						continue;
					}
					// Mark as authenticated for future messages
					Conn->bAuthenticated = true;
					UE_LOG(LogSOMOLMCPTransport, Log, TEXT("Conn=%llu authenticated (addr=%s)"), ConnectionId, *Conn->RemoteAddress);
				}

				++Stats.TotalMessagesReceived;
				if (OnMessage)
				{
					OnMessage(ConnectionId, Json);
				}
			}

			// Compact：offset 超过阈值时删除已消费字节
			if (Conn->FrameBufferOffset > 0 &&
				(Conn->FrameBufferOffset >= CompactThreshold ||
				 Conn->FrameBufferOffset >= Conn->FrameBuffer.Num() / 2))
			{
				Conn->FrameBuffer.RemoveAt(0, Conn->FrameBufferOffset, SOMOLMCP_NO_SHRINK);
				Conn->FrameBufferOffset = 0;
			}
		}
	}

	void FSololmcpTcpTransport::PumpOutgoing()
	{
		TArray<FConnectionId> ClientIds;
		Clients.GenerateKeyArray(ClientIds);
		for (const FConnectionId ConnectionId : ClientIds)
		{
			TSharedPtr<FClientConnection>* ConnPtr = Clients.Find(ConnectionId);
			TSharedPtr<FClientConnection> Conn = (ConnPtr && ConnPtr->IsValid()) ? *ConnPtr : nullptr;
			if (!Conn.IsValid() || !Conn->Socket)
			{
				CloseClient(ConnectionId, ECloseReason::MissingSocket);
				continue;
			}

			if (Conn->Socket->GetConnectionState() != SCS_Connected)
			{
				CloseClient(ConnectionId, ECloseReason::ConnectionState);
				continue;
			}

			if (Conn->PendingSendOffset >= Conn->PendingSend.Num() || Conn->PendingSend.Num() == 0)
			{
				Conn->PendingSend.Reset();
				Conn->PendingSendOffset = 0;
				continue;
			}

			const uint8* Data = Conn->PendingSend.GetData() + Conn->PendingSendOffset;
			const int32 Len = Conn->PendingSend.Num() - Conn->PendingSendOffset;

			int32 BytesSent = 0;
			if (!Conn->Socket->Send(Data, Len, BytesSent))
			{
				CloseClient(ConnectionId, ECloseReason::SendError);
				continue;
			}

			if (BytesSent > 0)
			{
				Conn->PendingSendOffset += BytesSent;
				Conn->LastActivityTime = FPlatformTime::Seconds();
			}

			if (Conn->PendingSendOffset >= Conn->PendingSend.Num())
			{
				Conn->PendingSend.Reset();
				Conn->PendingSendOffset = 0;
			}
		}
	}
}
