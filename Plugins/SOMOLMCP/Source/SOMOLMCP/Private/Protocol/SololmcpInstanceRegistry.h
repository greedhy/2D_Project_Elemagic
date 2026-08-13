// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Copyright (c) SOMOL. Multi-instance coordination registry.
//
// Each running SOMOLMCP server writes a small JSON descriptor to
// `%APPDATA%/SOMOLMCP/instances/<pid>.json` on Start and refreshes its
// `last_seen` timestamp every few seconds while alive. On Stop, the file is
// removed. Clients (Rust/Tauri) enumerate this directory to discover every
// live UE editor on the host — project name, actual bound port, engine version,
// instance uuid — so the user can switch between them in the TopBar.
//
// Why a file-based registry rather than broadcasting on the network:
//   * Localhost is the only realistic scenario where two editors collide on a
//     port, and a per-user AppData directory is already isolated.
//   * Clients need instance metadata *before* opening a TCP connection (the
//     decision of *which* port to connect to). A side-channel like this avoids
//     an "ask all ports to identify themselves" scan.
//   * Stale files (process crashed) are reaped on discovery by comparing
//     `last_seen` to a 5-minute threshold.
//
// The heartbeat runs on the game thread via FTSTicker.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

namespace UE::SOMOLMCP
{
	class FSololmcpInstanceRegistry final
	{
	public:
		FSololmcpInstanceRegistry();
		~FSololmcpInstanceRegistry();

		/// Write the descriptor file and start the heartbeat. Safe to call from the game thread.
		/// `InActualPort` should be the port actually bound by the transport (see FSololmcpTcpTransport::GetActualPort).
		bool Start(int32 InActualPort);

		/// Remove the descriptor file and stop the heartbeat.
		void Stop();

		bool IsRunning() const { return bRunning; }

		/// Stable per-process identifier (generated once at first Start).
		const FString& GetInstanceUuid() const { return InstanceUuid; }

		/// Absolute path to the JSON file this instance owns.
		const FString& GetDescriptorPath() const { return DescriptorPath; }

	private:
		bool WriteDescriptor();
		bool HeartbeatTick(float DeltaTime);

		/// Resolve `%APPDATA%/SOMOLMCP/instances/` (creates the directory on demand).
		static FString GetInstancesDir();

	private:
		static constexpr float HeartbeatIntervalSec = 5.0f;

		bool bRunning = false;
		int32 ActualPort = 0;
		FString InstanceUuid;
		FString DescriptorPath;
		FString StartedAtIso8601;
		FTSTicker::FDelegateHandle HeartbeatHandle;
	};
}
