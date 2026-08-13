// Copyright 2026 SOMOLAGENT. All Rights Reserved.
#include "SololmcpInstanceRegistry.h"

#include "SOMOLMCP.h"
#include "SololmcpJsonUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Runtime/Launch/Resources/Version.h"

namespace UE::SOMOLMCP
{
	namespace
	{
		FString MakeIso8601Now()
		{
			// Use UTC to keep comparisons across timezones sane (client may be on a
			// different TZ than UE when both run remote).
			return FDateTime::UtcNow().ToIso8601();
		}

		FString EngineVersionString()
		{
			// ENGINE_{MAJOR,MINOR,PATCH}_VERSION live in Launch/Resources/Version.h.
			// ENGINE_VERSION_STRING is also available but includes changelist which is
			// noisy for display — keep it short.
			return FString::Printf(TEXT("%d.%d.%d"),
				ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION);
		}
	}

	FSololmcpInstanceRegistry::FSololmcpInstanceRegistry() = default;

	FSololmcpInstanceRegistry::~FSololmcpInstanceRegistry()
	{
		Stop();
	}

	FString FSololmcpInstanceRegistry::GetInstancesDir()
	{
		// Prefer the user AppData roaming dir so the file survives project switches
		// but stays per-user. UE's FPlatformProcess::UserSettingsDir() resolves to
		// %APPDATA% on Windows, ~/Library/Application Support on macOS, and
		// ~/.config on Linux — all appropriate for this kind of transient metadata.
		const FString BaseDir = FString(FPlatformProcess::UserSettingsDir());
		const FString Dir = FPaths::Combine(BaseDir, TEXT("SOMOLMCP"), TEXT("instances"));
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		return Dir;
	}

	bool FSololmcpInstanceRegistry::Start(int32 InActualPort)
	{
		if (bRunning)
		{
			return true;
		}

		ActualPort = InActualPort;
		InstanceUuid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		StartedAtIso8601 = MakeIso8601Now();

		const FString Dir = GetInstancesDir();
		const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
		DescriptorPath = FPaths::Combine(Dir, FString::Printf(TEXT("%u.json"), Pid));

		if (!WriteDescriptor())
		{
			UE_LOG(LogSOMOLMCP, Warning,
				TEXT("InstanceRegistry: failed to write descriptor %s — multi-instance discovery will degrade"),
				*DescriptorPath);
			// Non-fatal: server still runs, clients just won't auto-list this instance.
		}

		HeartbeatHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FSololmcpInstanceRegistry::HeartbeatTick),
			HeartbeatIntervalSec);

		bRunning = true;
		UE_LOG(LogSOMOLMCP, Log,
			TEXT("InstanceRegistry: registered pid=%u uuid=%s port=%d at %s"),
			Pid, *InstanceUuid, ActualPort, *DescriptorPath);
		return true;
	}

	void FSololmcpInstanceRegistry::Stop()
	{
		if (!bRunning)
		{
			return;
		}
		bRunning = false;
		if (HeartbeatHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatHandle);
			HeartbeatHandle.Reset();
		}
		if (!DescriptorPath.IsEmpty() && IFileManager::Get().FileExists(*DescriptorPath))
		{
			IFileManager::Get().Delete(*DescriptorPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
		}
		UE_LOG(LogSOMOLMCP, Log, TEXT("InstanceRegistry: deregistered uuid=%s"), *InstanceUuid);
	}

	bool FSololmcpInstanceRegistry::WriteDescriptor()
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
		Obj->SetNumberField(TEXT("port"), ActualPort);
		Obj->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		Obj->SetStringField(TEXT("project_path"),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Obj->SetStringField(TEXT("engine_version"), EngineVersionString());
		Obj->SetStringField(TEXT("instance_uuid"), InstanceUuid);
		Obj->SetStringField(TEXT("started_at"), StartedAtIso8601);
		Obj->SetStringField(TEXT("last_seen"), MakeIso8601Now());
		// Bind address helps when the server listens on a non-loopback interface.
		Obj->SetStringField(TEXT("bind_address"), TEXT("127.0.0.1"));
		// Human-readable product tag so clients can distinguish SOMOLMCP from
		// other MCP implementations that happen to share the directory layout.
		Obj->SetStringField(TEXT("product"), TEXT("SOMOLMCP"));
		Obj->SetStringField(TEXT("product_version"), GetProductVersion());

		const FString Json = ToJsonString(Obj, /*bPretty=*/true);
		return FFileHelper::SaveStringToFile(Json, *DescriptorPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool FSololmcpInstanceRegistry::HeartbeatTick(float /*DeltaTime*/)
	{
		if (!bRunning)
		{
			return false;
		}
		// Rewrite with a refreshed `last_seen` so clients can distinguish live
		// instances from stale ones (crashed editor leaving a zombie file).
		WriteDescriptor();
		return true;
	}
}
