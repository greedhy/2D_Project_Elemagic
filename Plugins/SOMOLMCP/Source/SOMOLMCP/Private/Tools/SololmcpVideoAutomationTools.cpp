// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// Production video automation tools. These are deliberately registered before
// the broad contract wrappers so the native implementations remain authoritative.

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"

#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "MovieSceneSequencePlayer.h"
#include "MoviePipelineDeferredPasses.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineImageSequenceOutput.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"
#include "Graph/MovieGraphConfig.h"
#include "RHI.h"
#include "Protocol/SololmcpJobService.h"
#if SOMOLMCP_WITH_WORLDFORGE
#include "SOMOLRuntimeFaunaSubsystem.h"
#include "SOMOLRuntimeFaunaTypes.h"
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
// UE 5.8's Windows headers expose PW_RENDERFULLCONTENT unconditionally, but
// older engine Windows header chains can omit it; define it for parity.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include "Async/Async.h"
#include "Containers/Ticker.h"

namespace UE::SOMOLMCP
{
namespace VideoAutomation
{
static FCriticalSection GStateMutex;
static const FString GEngineInstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

static FString GetString(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, const FString& Default = FString())
{
	FString Value;
	return Args->TryGetStringField(Name, Value) ? Value : Default;
}

static int32 GetInt(const TSharedRef<FJsonObject>& Args, const TCHAR* Name, const int32 Default, const int32 Min, const int32 Max)
{
	double Value = Default;
	Args->TryGetNumberField(Name, Value);
	return FMath::Clamp(FMath::RoundToInt(Value), Min, Max);
}

static TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

static FString CurrentRhiName()
{
	if (!GDynamicRHI)
	{
		return TEXT("uninitialized");
	}
	FString Name = GDynamicRHI->GetName();
	Name.ToLowerInline();
	if (Name.Contains(TEXT("d3d12")) || Name.Contains(TEXT("directx 12")))
	{
		return TEXT("dx12");
	}
	if (Name.Contains(TEXT("vulkan")))
	{
		return TEXT("vulkan");
	}
	return Name;
}

static UWorld* ResolveAuditWorld()
{
	if (!GEditor)
	{
		return nullptr;
	}
	if (UWorld* PieWorld = GEditor->PlayWorld)
	{
		return PieWorld;
	}
	return GEditor->GetEditorWorldContext().World();
}

#if PLATFORM_WINDOWS
template <typename T>
static void SafeRelease(T*& Value)
{
	if (Value)
	{
		Value->Release();
		Value = nullptr;
	}
}

struct FDesktopSource
{
	FString SourceId;
	FString Type;
	FString DisplayTitle;
	HWND Window = nullptr;
	int32 Width = 0;
	int32 Height = 0;
	double ExpiresAtSeconds = 0.0;
};

struct FDesktopCaptureSession : public TSharedFromThis<FDesktopCaptureSession, ESPMode::ThreadSafe>
{
	FString SessionId;
	FString SourceId;
	FString OutputPath;
	FString State = TEXT("starting");
	FString Error;
	HWND Window = nullptr;
	int32 Width = 0;
	int32 Height = 0;
	int32 Fps = 30;
	int32 BitrateMbps = 20;
	int32 MaxDurationSeconds = 0;
	TAtomic<bool> bStopRequested{false};
	TAtomic<bool> bCancelRequested{false};
	TAtomic<int64> FramesWritten{0};
	TAtomic<int64> FramesDropped{0};
	double StartedAtSeconds = 0.0;
	double FinishedAtSeconds = 0.0;
};

static TMap<FString, FDesktopSource> GDesktopSources;
static TMap<FString, TSharedPtr<FDesktopCaptureSession, ESPMode::ThreadSafe>> GDesktopSessions;

static FString SanitizeWindowTitle(const FString& Raw)
{
	FString Title = Raw.Left(160);
	Title.ReplaceInline(TEXT("\r"), TEXT(" "));
	Title.ReplaceInline(TEXT("\n"), TEXT(" "));
	return Title;
}

static TArray<FDesktopSource> EnumerateDesktopSources()
{
	TArray<FDesktopSource> Sources;
	const double Expiry = FPlatformTime::Seconds() + 300.0;

	FDesktopSource Desktop;
	Desktop.SourceId = FString::Printf(TEXT("monitor-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Desktop.Type = TEXT("monitor");
	Desktop.DisplayTitle = TEXT("Primary desktop");
	Desktop.Window = GetDesktopWindow();
	Desktop.Width = GetSystemMetrics(SM_CXSCREEN);
	Desktop.Height = GetSystemMetrics(SM_CYSCREEN);
	Desktop.ExpiresAtSeconds = Expiry;
	Sources.Add(Desktop);

	struct FEnumContext
	{
		TArray<FDesktopSource>* Items = nullptr;
		double Expiry = 0.0;
	} Context{&Sources, Expiry};

	EnumWindows([](HWND Window, LPARAM Param) -> BOOL
	{
		FEnumContext* ContextPtr = reinterpret_cast<FEnumContext*>(Param);
		if (!ContextPtr || !IsWindowVisible(Window) || IsIconic(Window))
		{
			return 1;
		}
		WCHAR Buffer[512] = {};
		const int32 Length = GetWindowTextW(Window, Buffer, UE_ARRAY_COUNT(Buffer));
		if (Length <= 0)
		{
			return 1;
		}
		RECT Bounds{};
		if (!GetWindowRect(Window, &Bounds))
		{
			return 1;
		}
		const int32 Width = Bounds.right - Bounds.left;
		const int32 Height = Bounds.bottom - Bounds.top;
		if (Width < 320 || Height < 200)
		{
			return 1;
		}

		FDesktopSource Source;
		Source.SourceId = FString::Printf(TEXT("window-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Source.Type = TEXT("window");
		Source.DisplayTitle = SanitizeWindowTitle(FString(Length, Buffer));
		Source.Window = Window;
		Source.Width = Width;
		Source.Height = Height;
		Source.ExpiresAtSeconds = ContextPtr->Expiry;
		ContextPtr->Items->Add(MoveTemp(Source));
		return 1;
	}, reinterpret_cast<LPARAM>(&Context));
	return Sources;
}

static bool CaptureWindowBgra(const HWND Window, int32& OutWidth, int32& OutHeight, TArray<uint8>& OutBytes, FString& OutError)
{
	RECT Rect{};
	if (!Window || !GetWindowRect(Window, &Rect))
	{
		OutError = TEXT("capture_source_unavailable");
		return false;
	}
	OutWidth = Rect.right - Rect.left;
	OutHeight = Rect.bottom - Rect.top;
	if (OutWidth <= 0 || OutHeight <= 0 || OutWidth > 16384 || OutHeight > 16384)
	{
		OutError = TEXT("capture_source_bounds_invalid");
		return false;
	}

	HDC SourceDc = GetWindowDC(Window);
	HDC MemoryDc = SourceDc ? CreateCompatibleDC(SourceDc) : nullptr;
	HBITMAP Bitmap = (SourceDc && MemoryDc) ? CreateCompatibleBitmap(SourceDc, OutWidth, OutHeight) : nullptr;
	if (!SourceDc || !MemoryDc || !Bitmap)
	{
		if (Bitmap) DeleteObject(Bitmap);
		if (MemoryDc) DeleteDC(MemoryDc);
		if (SourceDc) ReleaseDC(Window, SourceDc);
		OutError = TEXT("capture_gdi_allocation_failed");
		return false;
	}

	HGDIOBJ Previous = SelectObject(MemoryDc, Bitmap);
	BOOL Captured = PrintWindow(Window, MemoryDc, PW_RENDERFULLCONTENT);
	if (!Captured)
	{
		Captured = BitBlt(MemoryDc, 0, 0, OutWidth, OutHeight, SourceDc, 0, 0, SRCCOPY | CAPTUREBLT);
	}

	BITMAPINFO Info{};
	Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	Info.bmiHeader.biWidth = OutWidth;
	Info.bmiHeader.biHeight = -OutHeight;
	Info.bmiHeader.biPlanes = 1;
	Info.bmiHeader.biBitCount = 32;
	Info.bmiHeader.biCompression = BI_RGB;
	OutBytes.SetNumUninitialized(OutWidth * OutHeight * 4);
	const int32 ScanLines = Captured ? GetDIBits(MemoryDc, Bitmap, 0, OutHeight, OutBytes.GetData(), &Info, DIB_RGB_COLORS) : 0;

	SelectObject(MemoryDc, Previous);
	DeleteObject(Bitmap);
	DeleteDC(MemoryDc);
	ReleaseDC(Window, SourceDc);
	if (ScanLines != OutHeight)
	{
		OutBytes.Reset();
		OutError = TEXT("capture_readback_failed");
		return false;
	}
	for (int64 Pixel = 0; Pixel < static_cast<int64>(OutWidth) * OutHeight; ++Pixel)
	{
		OutBytes[Pixel * 4 + 3] = 255;
	}
	return true;
}

static HRESULT ConfigureMp4Writer(const FString& OutputPath, int32 Width, int32 Height, int32 Fps, int32 BitrateMbps, IMFSinkWriter*& OutWriter, DWORD& OutStream)
{
	IMFMediaType* OutputType = nullptr;
	IMFMediaType* InputType = nullptr;
	HRESULT Hr = MFCreateSinkWriterFromURL(*OutputPath, nullptr, nullptr, &OutWriter);
	if (SUCCEEDED(Hr)) Hr = MFCreateMediaType(&OutputType);
	if (SUCCEEDED(Hr)) Hr = OutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (SUCCEEDED(Hr)) Hr = OutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	if (SUCCEEDED(Hr)) Hr = OutputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(BitrateMbps * 1000000));
	if (SUCCEEDED(Hr)) Hr = OutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeSize(OutputType, MF_MT_FRAME_SIZE, Width, Height);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(OutputType, MF_MT_FRAME_RATE, Fps, 1);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(OutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if (SUCCEEDED(Hr)) Hr = OutWriter->AddStream(OutputType, &OutStream);

	if (SUCCEEDED(Hr)) Hr = MFCreateMediaType(&InputType);
	if (SUCCEEDED(Hr)) Hr = InputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (SUCCEEDED(Hr)) Hr = InputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
	if (SUCCEEDED(Hr)) Hr = InputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeSize(InputType, MF_MT_FRAME_SIZE, Width, Height);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(InputType, MF_MT_FRAME_RATE, Fps, 1);
	if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(InputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	if (SUCCEEDED(Hr)) Hr = InputType->SetUINT32(MF_MT_DEFAULT_STRIDE, Width * 4);
	if (SUCCEEDED(Hr)) Hr = OutWriter->SetInputMediaType(OutStream, InputType, nullptr);
	if (SUCCEEDED(Hr)) Hr = OutWriter->BeginWriting();
	SafeRelease(InputType);
	SafeRelease(OutputType);
	return Hr;
}

static HRESULT WriteMp4Frame(IMFSinkWriter* Writer, DWORD Stream, const TArray<uint8>& Bgra, int64 FrameIndex, int32 Fps)
{
	IMFMediaBuffer* Buffer = nullptr;
	IMFSample* Sample = nullptr;
	HRESULT Hr = MFCreateMemoryBuffer(Bgra.Num(), &Buffer);
	BYTE* Destination = nullptr;
	DWORD MaxLength = 0;
	DWORD CurrentLength = 0;
	if (SUCCEEDED(Hr)) Hr = Buffer->Lock(&Destination, &MaxLength, &CurrentLength);
	if (SUCCEEDED(Hr)) FMemory::Memcpy(Destination, Bgra.GetData(), Bgra.Num());
	if (Destination) Buffer->Unlock();
	if (SUCCEEDED(Hr)) Hr = Buffer->SetCurrentLength(Bgra.Num());
	if (SUCCEEDED(Hr)) Hr = MFCreateSample(&Sample);
	if (SUCCEEDED(Hr)) Hr = Sample->AddBuffer(Buffer);
	const LONGLONG Duration = 10000000ll / Fps;
	if (SUCCEEDED(Hr)) Hr = Sample->SetSampleTime(FrameIndex * Duration);
	if (SUCCEEDED(Hr)) Hr = Sample->SetSampleDuration(Duration);
	if (SUCCEEDED(Hr)) Hr = Writer->WriteSample(Stream, Sample);
	SafeRelease(Sample);
	SafeRelease(Buffer);
	return Hr;
}

static void RunDesktopCapture(const TSharedPtr<FDesktopCaptureSession, ESPMode::ThreadSafe> Session)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	HRESULT Hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
	IMFSinkWriter* Writer = nullptr;
	DWORD Stream = 0;
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> Frame;
	FString CaptureError;
	if (SUCCEEDED(Hr) && !CaptureWindowBgra(Session->Window, Width, Height, Frame, CaptureError))
	{
		Hr = E_FAIL;
	}
	if (SUCCEEDED(Hr))
	{
		Width &= ~1;
		Height &= ~1;
		if (Width != Session->Width || Height != Session->Height)
		{
			Session->Width = Width;
			Session->Height = Height;
			CaptureWindowBgra(Session->Window, Width, Height, Frame, CaptureError);
		}
		Hr = ConfigureMp4Writer(Session->OutputPath, Width, Height, Session->Fps, Session->BitrateMbps, Writer, Stream);
	}

	if (FAILED(Hr))
	{
		{
			FScopeLock Lock(&GStateMutex);
			Session->State = TEXT("failed");
			Session->Error = CaptureError.IsEmpty() ? FString::Printf(TEXT("media_foundation_init_failed_0x%08x"), static_cast<uint32>(Hr)) : CaptureError;
		}
		FString JobUpdateError;
		FSololmcpJobService::UpdateExternalJob(Session->SessionId, TEXT("failed"), 0.0, TEXT("{\"executor\":\"desktop_capture\"}"), 0, TEXT("E_CAPTURE_INIT"), Session->Error, JobUpdateError);
	}
	else
	{
		{
			FScopeLock Lock(&GStateMutex);
			Session->State = TEXT("capturing");
			Session->StartedAtSeconds = FPlatformTime::Seconds();
		}
		const double FrameInterval = 1.0 / Session->Fps;
		double NextFrameAt = FPlatformTime::Seconds();
		int32 ConsecutiveCaptureFailures = 0;
		bool bCaptureFailed = false;
		while (!Session->bStopRequested.Load())
		{
			const double Now = FPlatformTime::Seconds();
			if (Session->MaxDurationSeconds > 0 && Now - Session->StartedAtSeconds >= Session->MaxDurationSeconds)
			{
				break;
			}
			if (Now < NextFrameAt)
			{
				FPlatformProcess::SleepNoStats(static_cast<float>(FMath::Min(NextFrameAt - Now, 0.01)));
				continue;
			}
			int32 NewWidth = 0;
			int32 NewHeight = 0;
			if (!CaptureWindowBgra(Session->Window, NewWidth, NewHeight, Frame, CaptureError) || NewWidth < Width || NewHeight < Height)
			{
				++Session->FramesDropped;
				++ConsecutiveCaptureFailures;
				if (ConsecutiveCaptureFailures >= FMath::Max(5, Session->Fps * 3))
				{
					bCaptureFailed = true;
					if (CaptureError.IsEmpty()) CaptureError = TEXT("capture_source_lost_or_resized_below_session_dimensions");
					break;
				}
			}
			else
			{
				ConsecutiveCaptureFailures = 0;
				// The encoder contract freezes dimensions at session start. Copy the
				// top-left crop when a resizable window grows during capture.
				if (NewWidth != Width || NewHeight != Height)
				{
					TArray<uint8> Cropped;
					Cropped.SetNumUninitialized(Width * Height * 4);
					for (int32 Row = 0; Row < Height; ++Row)
					{
						FMemory::Memcpy(Cropped.GetData() + Row * Width * 4, Frame.GetData() + Row * NewWidth * 4, Width * 4);
					}
					Frame = MoveTemp(Cropped);
				}
				const int64 FrameIndex = Session->FramesWritten.Load();
				if (SUCCEEDED(WriteMp4Frame(Writer, Stream, Frame, FrameIndex, Session->Fps)))
				{
					++Session->FramesWritten;
				}
				else
				{
					++Session->FramesDropped;
				}
			}
			NextFrameAt += FrameInterval;
			if (NextFrameAt < Now - FrameInterval)
			{
				NextFrameAt = Now + FrameInterval;
			}
		}
		Writer->Finalize();
		{
			FScopeLock Lock(&GStateMutex);
			Session->FinishedAtSeconds = FPlatformTime::Seconds();
			Session->State = Session->bCancelRequested.Load() ? TEXT("cancelled") : (bCaptureFailed ? TEXT("failed") : TEXT("completed"));
			if (bCaptureFailed) Session->Error = CaptureError;
		}
		FString JobUpdateError;
		const FString TerminalStatus = Session->bCancelRequested.Load() ? TEXT("cancelled") : (bCaptureFailed ? TEXT("failed") : TEXT("succeeded"));
		const FString Payload = FString::Printf(TEXT("{\"executor\":\"desktop_capture\",\"output_path\":\"%s\",\"frames_written\":%lld}"), *Session->OutputPath.ReplaceCharWithEscapedChar(), Session->FramesWritten.Load());
		FSololmcpJobService::UpdateExternalJob(Session->SessionId, TerminalStatus, bCaptureFailed ? 0.0 : 1.0, Payload, Session->FramesWritten.Load(),
			bCaptureFailed ? TEXT("E_CAPTURE_SOURCE_LOST") : FString(), bCaptureFailed ? CaptureError : FString(), JobUpdateError);
	}
	SafeRelease(Writer);
	if (SUCCEEDED(Hr)) MFShutdown();
	CoUninitialize();
	if (Session->bCancelRequested.Load())
	{
		IFileManager::Get().Delete(*Session->OutputPath, false, true, true);
	}
}
#endif

struct FPreflightToken
{
	FString Token;
	FString InputHash;
	double ExpiresAtSeconds = 0.0;
};
static TMap<FString, FPreflightToken> GPreflightTokens;

struct FMovieRenderJobState
{
	FString JobId;
	FString SequencePath;
	FString GraphPath;
	FString OutputDirectory;
	FString Status = TEXT("starting");
	FString Error;
	TWeakObjectPtr<UMoviePipelineExecutorJob> PipelineJob;
	TWeakObjectPtr<UMoviePipelineExecutorBase> Executor;
	TStrongObjectPtr<UMoviePipelineQueue> Queue;
	double StartedAtSeconds = 0.0;
	double FinishedAtSeconds = 0.0;
	int64 OutputRevision = 0;
};
static TMap<FString, TSharedPtr<FMovieRenderJobState>> GMovieRenderJobs;

static bool IsPreflightTokenValid(const FString& Token, const TSharedRef<FJsonObject>& Args, FString& OutReason)
{
	FScopeLock Lock(&GStateMutex);
	const FPreflightToken* Found = GPreflightTokens.Find(Token);
	if (!Found)
	{
		OutReason = TEXT("preflight_token_not_found");
		return false;
	}
	if (Found->ExpiresAtSeconds < FPlatformTime::Seconds())
	{
		GPreflightTokens.Remove(Token);
		OutReason = TEXT("preflight_token_expired");
		return false;
	}
	const FString SequencePath = GetString(Args, TEXT("sequence_path"), GetString(Args, TEXT("sequence_asset")));
	const FString GraphPath = GetString(Args, TEXT("graph_path"), GetString(Args, TEXT("preset_path")));
	FString RequiredRhi = GetString(Args, TEXT("required_rhi"), CurrentRhiName());
	RequiredRhi.ToLowerInline();
	FString OutputDirectory = GetString(Args, TEXT("output_directory"), GetString(Args, TEXT("output_path"), FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MovieRenders"))));
	if (FPaths::IsRelative(OutputDirectory)) OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), OutputDirectory);
	FPaths::NormalizeDirectoryName(OutputDirectory);
	const FString CurrentHash = FMD5::HashAnsiString(*(SequencePath + TEXT("|") + GraphPath + TEXT("|") + RequiredRhi + TEXT("|") + OutputDirectory));
	if (CurrentHash != Found->InputHash)
	{
		OutReason = TEXT("preflight_token_binding_changed");
		return false;
	}
	return true;
}

static TArray<FString> FindOutputArtifacts(const FString& Directory, int32 MaxFiles = 1000)
{
	TArray<FString> Files;
	if (IFileManager::Get().DirectoryExists(*Directory))
	{
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.*"), true, false, false);
	}
	Files.Sort();
	if (Files.Num() > MaxFiles) Files.SetNum(MaxFiles, SOMOLMCP_NO_SHRINK);
	return Files;
}

static TSharedPtr<FMovieRenderJobState> FindMovieRenderJob(const FString& JobId)
{
	FScopeLock Lock(&GStateMutex);
	return GMovieRenderJobs.FindRef(JobId);
}

static FString BuildRenderPayload(const FMovieRenderJobState& State, const TArray<FString>& Artifacts = {})
{
	FString Json = FString::Printf(TEXT("{\"executor\":\"movie_render_graph\",\"sequence_path\":\"%s\",\"graph_path\":\"%s\",\"output_directory\":\"%s\",\"artifact_count\":%d}"),
		*State.SequencePath.ReplaceCharWithEscapedChar(), *State.GraphPath.ReplaceCharWithEscapedChar(), *State.OutputDirectory.ReplaceCharWithEscapedChar(), Artifacts.Num());
	return Json;
}

static bool ToolMovieRenderSubmit(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	const bool bRequirePreflight = Args->HasField(TEXT("preflight_token"));
	if (bRequirePreflight && !IsPreflightTokenValid(GetString(Args, TEXT("preflight_token")), Args, Error)) return false;
	const FString SequencePath = GetString(Args, TEXT("sequence_path"), GetString(Args, TEXT("sequence_asset")));
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
	if (!Sequence)
	{
		Error = TEXT("sequence_asset_missing_or_invalid");
		return false;
	}
	UMovieGraphConfig* Graph = nullptr;
	const FString GraphPath = GetString(Args, TEXT("graph_path"), GetString(Args, TEXT("preset_path")));
	if (!GraphPath.IsEmpty())
	{
		Graph = LoadObject<UMovieGraphConfig>(nullptr, *GraphPath);
		if (!Graph)
		{
			Error = TEXT("movie_render_graph_asset_missing_or_invalid");
			return false;
		}
	}
	FString OutputDirectory = GetString(Args, TEXT("output_directory"), GetString(Args, TEXT("output_path"), FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MovieRenders"))));
	if (FPaths::IsRelative(OutputDirectory)) OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), OutputDirectory);
	FPaths::NormalizeDirectoryName(OutputDirectory);
	FString SavedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	FPaths::NormalizeDirectoryName(SavedRoot);
	if (!OutputDirectory.StartsWith(SavedRoot, ESearchCase::IgnoreCase))
	{
		Error = TEXT("output_directory_must_be_beneath_project_saved_dir");
		return false;
	}
	IFileManager::Get().MakeDirectory(*OutputDirectory, true);

	UMoviePipelineQueueSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
	if (!Subsystem || Subsystem->IsRendering())
	{
		Error = Subsystem ? TEXT("movie_render_lane_busy") : TEXT("movie_render_queue_subsystem_unavailable");
		return false;
	}
	const FString ClientRequestId = GetString(Args, TEXT("client_request_id"), FString::Printf(TEXT("mrg-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FString JobId;
	bool bDeduplicated = false;
	const FString InitialPayload = FString::Printf(TEXT("{\"executor\":\"movie_render_graph\",\"sequence_path\":\"%s\"}"), *SequencePath.ReplaceCharWithEscapedChar());
	if (!FSololmcpJobService::CreateExternalJob(ClientRequestId, TEXT("movie_render_graph"), InitialPayload,
		{TEXT("capture:movie_render"), TEXT("sequence:") + SequencePath, TEXT("output:") + OutputDirectory}, JobId, bDeduplicated, Error)) return false;
	if (bDeduplicated)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetBoolField(TEXT("deduplicated"), true);
		Out->SetStringField(TEXT("job_id"), JobId);
		Out->SetStringField(TEXT("status"), TEXT("existing"));
		Summary = FString::Printf(TEXT("Movie render request deduplicated to %s."), *JobId);
		return true;
	}

	TSharedPtr<FMovieRenderJobState> State = MakeShared<FMovieRenderJobState>();
	State->JobId = JobId;
	State->SequencePath = SequencePath;
	State->GraphPath = GraphPath;
	State->OutputDirectory = OutputDirectory;
	State->StartedAtSeconds = FPlatformTime::Seconds();
	State->Queue = TStrongObjectPtr<UMoviePipelineQueue>(NewObject<UMoviePipelineQueue>(GetTransientPackage()));
	UMoviePipelineExecutorJob* PipelineJob = State->Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
	PipelineJob->JobName = GetString(Args, TEXT("job_name"), FString::Printf(TEXT("SOMOLMCP_%s"), *JobId.Left(8)));
	PipelineJob->SetSequence(FSoftObjectPath(Sequence));
	FString MapPath = GetString(Args, TEXT("map_path"));
	if (MapPath.IsEmpty())
	{
		if (UWorld* World = ResolveAuditWorld()) MapPath = World->GetPathName();
	}
	PipelineJob->Map = FSoftObjectPath(MapPath);
	PipelineJob->UserData = JobId;
	if (Graph)
	{
		PipelineJob->SetGraphPreset(Graph);
	}
	else
	{
		UMoviePipelinePrimaryConfig* Config = PipelineJob->GetConfiguration();
		UMoviePipelineOutputSetting* OutputSetting = Cast<UMoviePipelineOutputSetting>(Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));
		OutputSetting->OutputDirectory.Path = OutputDirectory;
		OutputSetting->FileNameFormat = TEXT("{sequence_name}/{shot_name}.{frame_number}");
		OutputSetting->OutputResolution = FIntPoint(GetInt(Args, TEXT("width"), 1920, 64, 16384), GetInt(Args, TEXT("height"), 1080, 64, 16384));
		OutputSetting->bOverrideExistingOutput = GetString(Args, TEXT("overwrite_policy"), TEXT("error")) == TEXT("replace_owned");
		OutputSetting->bFlushDiskWritesPerShot = true;
		Config->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
		Config->FindOrAddSettingByClass(UMoviePipelineDeferredPassBase::StaticClass());
	}
	State->PipelineJob = PipelineJob;
	UMoviePipelinePIEExecutor* Executor = NewObject<UMoviePipelinePIEExecutor>(Subsystem);
	Executor->SetIsRenderingOffscreen(true);
	State->Executor = Executor;
	{
		FScopeLock Lock(&GStateMutex);
		GMovieRenderJobs.Add(JobId, State);
	}
	Executor->OnExecutorFinished().AddLambda([JobId](UMoviePipelineExecutorBase*, bool bSuccess)
	{
		TSharedPtr<FMovieRenderJobState> Completed = FindMovieRenderJob(JobId);
		if (!Completed) return;
		const TArray<FString> Artifacts = FindOutputArtifacts(Completed->OutputDirectory);
		{
			FScopeLock Lock(&GStateMutex);
			const bool bProducedArtifacts = bSuccess && !Artifacts.IsEmpty();
			Completed->Status = bProducedArtifacts ? TEXT("completed") : TEXT("failed");
			Completed->FinishedAtSeconds = FPlatformTime::Seconds();
			Completed->OutputRevision = Artifacts.Num();
			if (!bSuccess) Completed->Error = TEXT("movie_pipeline_executor_failed");
			else if (Artifacts.IsEmpty()) Completed->Error = TEXT("movie_pipeline_completed_without_output_artifacts");
		}
		const bool bProducedArtifacts = bSuccess && !Artifacts.IsEmpty();
		FString UpdateError;
		FSololmcpJobService::UpdateExternalJob(JobId, bProducedArtifacts ? TEXT("succeeded") : TEXT("failed"), 1.0,
			BuildRenderPayload(*Completed, Artifacts), Artifacts.Num(), bProducedArtifacts ? FString() : TEXT("E_MRG_RENDER"), Completed->Error, UpdateError);
	});
	State->Status = TEXT("rendering");
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
	Subsystem->RenderQueueInstanceWithExecutorInstance(State->Queue.Get(), Executor);
#else
	// UE 5.3-5.5 have no queue-instance overload: the subsystem always renders its
	// own transient queue. Copy ours into it first so the render targets the same
	// jobs the 5.6+ path would have used.
	if (UMoviePipelineQueue* SubsystemQueue = Subsystem->GetQueue())
	{
		SubsystemQueue->CopyFrom(State->Queue.Get());
	}
	Subsystem->RenderQueueWithExecutorInstance(Executor);
#endif

	Out->SetBoolField(TEXT("success"), true);
	Out->SetBoolField(TEXT("submitted"), true);
	Out->SetStringField(TEXT("status"), TEXT("rendering"));
	Out->SetStringField(TEXT("job_id"), JobId);
	Out->SetStringField(TEXT("sequence_path"), SequencePath);
	Out->SetStringField(TEXT("graph_path"), GraphPath);
	Out->SetStringField(TEXT("output_directory"), OutputDirectory);
	Summary = FString::Printf(TEXT("Submitted native Movie Render %s job %s."), Graph ? TEXT("Graph") : TEXT("Pipeline"), *JobId);
	return true;
}

static bool ToolMovieRenderSubmitCanonical(const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	if (GetString(Args, TEXT("preflight_token")).IsEmpty())
	{
		Error = TEXT("preflight_token_is_required_for_canonical_mrg_submit");
		return false;
	}
	if (GetString(Args, TEXT("client_request_id")).IsEmpty())
	{
		Error = TEXT("client_request_id_is_required_for_canonical_mrg_submit");
		return false;
	}
	return ToolMovieRenderSubmit(Context, Args, Out, Summary, Error);
}

static bool WriteMovieRenderStatus(const TSharedPtr<FMovieRenderJobState>& State, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	if (!State)
	{
		Error = TEXT("movie_render_job_not_found");
		return false;
	}
	float Progress = 0.0f;
	if (UMoviePipelineExecutorJob* Job = State->PipelineJob.Get()) Progress = Job->GetStatusProgress();
	if (State->Status == TEXT("completed")) Progress = 1.0f;
	const TArray<FString> Artifacts = FindOutputArtifacts(State->OutputDirectory, 1000);
	Out->SetBoolField(TEXT("success"), State->Status != TEXT("failed"));
	Out->SetStringField(TEXT("status"), State->Status);
	Out->SetStringField(TEXT("job_id"), State->JobId);
	Out->SetNumberField(TEXT("progress"), Progress);
	Out->SetNumberField(TEXT("progress_percent"), FMath::RoundToInt(Progress * 100.0f));
	Out->SetNumberField(TEXT("artifact_count"), Artifacts.Num());
	Out->SetStringField(TEXT("output_directory"), State->OutputDirectory);
	if (!State->Error.IsEmpty()) Out->SetStringField(TEXT("error"), State->Error);
	if (State->Status == TEXT("rendering"))
	{
		FString UpdateError;
		FSololmcpJobService::UpdateExternalJob(State->JobId, TEXT("running"), Progress, BuildRenderPayload(*State, Artifacts), Artifacts.Num(), FString(), FString(), UpdateError);
	}
	Summary = FString::Printf(TEXT("Movie render job %s is %s (%d artifacts)."), *State->JobId, *State->Status, Artifacts.Num());
	return State->Status != TEXT("failed");
}

static bool ToolMovieRenderStatus(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	return WriteMovieRenderStatus(FindMovieRenderJob(GetString(Args, TEXT("job_id"), GetString(Args, TEXT("render_id")))), Out, Summary, Error);
}

static bool ToolMovieRenderCancel(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TSharedPtr<FMovieRenderJobState> State = FindMovieRenderJob(GetString(Args, TEXT("job_id"), GetString(Args, TEXT("render_id"))));
	if (!State)
	{
		Error = TEXT("movie_render_job_not_found");
		return false;
	}
	if (UMoviePipelineExecutorBase* Executor = State->Executor.Get()) Executor->CancelAllJobs();
	State->Status = TEXT("cancelled");
	State->FinishedAtSeconds = FPlatformTime::Seconds();
	FString UpdateError;
	FSololmcpJobService::UpdateExternalJob(State->JobId, TEXT("cancelled"), 0.0, BuildRenderPayload(*State), State->OutputRevision, FString(), FString(), UpdateError);
	return WriteMovieRenderStatus(State, Out, Summary, Error);
}

static bool ToolMovieRenderList(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString& Summary, FString&)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	FScopeLock Lock(&GStateMutex);
	for (const TPair<FString, TSharedPtr<FMovieRenderJobState>>& Pair : GMovieRenderJobs)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("job_id"), Pair.Key);
		Row->SetStringField(TEXT("status"), Pair.Value->Status);
		Row->SetStringField(TEXT("sequence_path"), Pair.Value->SequencePath);
		Row->SetStringField(TEXT("output_directory"), Pair.Value->OutputDirectory);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Out->SetBoolField(TEXT("success"), true);
	Out->SetArrayField(TEXT("jobs"), Rows);
	Out->SetNumberField(TEXT("job_count"), Rows.Num());
	Summary = FString::Printf(TEXT("Listed %d native movie render jobs."), Rows.Num());
	return true;
}

static bool ToolMovieRenderArtifacts(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TSharedPtr<FMovieRenderJobState> State = FindMovieRenderJob(GetString(Args, TEXT("job_id"), GetString(Args, TEXT("render_id"))));
	if (!State)
	{
		Error = TEXT("movie_render_job_not_found");
		return false;
	}
	const TArray<FString> Files = FindOutputArtifacts(State->OutputDirectory, GetInt(Args, TEXT("max_files"), 1000, 1, 10000));
	TArray<TSharedPtr<FJsonValue>> Rows;
	int64 TotalBytes = 0;
	for (const FString& File : Files)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		const int64 Bytes = IFileManager::Get().FileSize(*File);
		Row->SetStringField(TEXT("path"), File);
		Row->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		TotalBytes += FMath::Max<int64>(0, Bytes);
	}
	Out->SetBoolField(TEXT("success"), !Files.IsEmpty());
	Out->SetStringField(TEXT("status"), Files.IsEmpty() ? TEXT("no_artifacts") : TEXT("validated"));
	Out->SetStringField(TEXT("job_id"), State->JobId);
	Out->SetArrayField(TEXT("output_artifacts"), Rows);
	Out->SetNumberField(TEXT("artifact_count"), Rows.Num());
	Out->SetNumberField(TEXT("total_bytes"), static_cast<double>(TotalBytes));
	Summary = FString::Printf(TEXT("Read back %d movie render artifacts (%lld bytes)."), Rows.Num(), TotalBytes);
	return true;
}

static bool ToolDesktopSourceList(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
#if PLATFORM_WINDOWS
	const TArray<FDesktopSource> Sources = EnumerateDesktopSources();
	TArray<TSharedPtr<FJsonValue>> Rows;
	{
		FScopeLock Lock(&GStateMutex);
		GDesktopSources.Reset();
		for (const FDesktopSource& Source : Sources)
		{
			GDesktopSources.Add(Source.SourceId, Source);
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("source_id"), Source.SourceId);
			Row->SetStringField(TEXT("source_type"), Source.Type);
			Row->SetStringField(TEXT("title"), Source.DisplayTitle);
			Row->SetNumberField(TEXT("width"), Source.Width);
			Row->SetNumberField(TEXT("height"), Source.Height);
			Row->SetNumberField(TEXT("expires_in_ms"), 300000);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("status"), TEXT("ready"));
	Out->SetStringField(TEXT("engine_instance_id"), GEngineInstanceId);
	Out->SetArrayField(TEXT("sources"), Rows);
	Out->SetNumberField(TEXT("source_count"), Rows.Num());
	Summary = FString::Printf(TEXT("Enumerated %d opaque desktop capture sources."), Rows.Num());
	return true;
#else
	Error = TEXT("desktop_capture_is_win64_only");
	return false;
#endif
}

static bool ToolDesktopStart(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
#if PLATFORM_WINDOWS
	const FString SourceId = GetString(Args, TEXT("source_id"));
	FDesktopSource Source;
	{
		FScopeLock Lock(&GStateMutex);
		const FDesktopSource* Found = GDesktopSources.Find(SourceId);
		if (!Found || Found->ExpiresAtSeconds < FPlatformTime::Seconds())
		{
			Error = TEXT("source_id_missing_or_expired; call desktop_capture_source_list again");
			return false;
		}
		Source = *Found;
	}

	FString OutputDirectory = GetString(Args, TEXT("output_directory"), FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SOMOLMCP/Captures")));
	if (FPaths::IsRelative(OutputDirectory))
	{
		OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), OutputDirectory);
	}
	FPaths::NormalizeDirectoryName(OutputDirectory);
	FString SavedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	FPaths::NormalizeDirectoryName(SavedRoot);
	if (!OutputDirectory.StartsWith(SavedRoot, ESearchCase::IgnoreCase))
	{
		Error = TEXT("output_directory_must_be_beneath_project_saved_dir");
		return false;
	}
	IFileManager::Get().MakeDirectory(*OutputDirectory, true);

	TSharedPtr<FDesktopCaptureSession, ESPMode::ThreadSafe> Session = MakeShared<FDesktopCaptureSession, ESPMode::ThreadSafe>();
	Session->SourceId = Source.SourceId;
	Session->Window = Source.Window;
	Session->Width = Source.Width & ~1;
	Session->Height = Source.Height & ~1;
	Session->Fps = GetInt(Args, TEXT("fps"), 30, 1, 60);
	Session->BitrateMbps = GetInt(Args, TEXT("bitrate_mbps"), 20, 2, 120);
	Session->MaxDurationSeconds = GetInt(Args, TEXT("max_duration_seconds"), 0, 0, 86400);
	const FString ClientRequestId = GetString(Args, TEXT("client_request_id"), FString::Printf(TEXT("desktop-capture-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FString JobId;
	bool bDeduplicated = false;
	const FString InitialPayload = FString::Printf(TEXT("{\"executor\":\"desktop_capture\",\"source_id\":\"%s\"}"), *Source.SourceId);
	if (!FSololmcpJobService::CreateExternalJob(ClientRequestId, TEXT("desktop_capture"), InitialPayload,
		{TEXT("capture:desktop:") + Source.SourceId, TEXT("output:") + OutputDirectory}, JobId, bDeduplicated, Error))
	{
		return false;
	}
	Session->SessionId = JobId;
	Session->OutputPath = FPaths::Combine(OutputDirectory, Session->SessionId + TEXT(".mp4"));
	if (bDeduplicated)
	{
		Out->SetBoolField(TEXT("success"), true);
		Out->SetBoolField(TEXT("deduplicated"), true);
		Out->SetStringField(TEXT("status"), TEXT("existing"));
		Out->SetStringField(TEXT("session_id"), JobId);
		Summary = FString::Printf(TEXT("Desktop capture request deduplicated to Job Runtime id %s."), *JobId);
		return true;
	}
	{
		FScopeLock Lock(&GStateMutex);
		GDesktopSessions.Add(Session->SessionId, Session);
	}
	Async(EAsyncExecution::Thread, [Session]() { RunDesktopCapture(Session); });

	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("status"), TEXT("starting"));
	Out->SetStringField(TEXT("session_id"), Session->SessionId);
	Out->SetStringField(TEXT("job_id"), Session->SessionId);
	Out->SetStringField(TEXT("output_path"), Session->OutputPath);
	Out->SetStringField(TEXT("codec"), TEXT("h264"));
	Out->SetStringField(TEXT("container"), TEXT("mp4"));
	Out->SetNumberField(TEXT("fps"), Session->Fps);
	Out->SetNumberField(TEXT("width"), Session->Width);
	Out->SetNumberField(TEXT("height"), Session->Height);
	Summary = FString::Printf(TEXT("Desktop capture session %s started."), *Session->SessionId);
	return true;
#else
	Error = TEXT("desktop_capture_is_win64_only");
	return false;
#endif
}

static TSharedPtr<FDesktopCaptureSession, ESPMode::ThreadSafe> FindDesktopSession(const FString& SessionId)
{
#if PLATFORM_WINDOWS
	FScopeLock Lock(&GStateMutex);
	return GDesktopSessions.FindRef(SessionId);
#else
	return nullptr;
#endif
}

static bool WriteDesktopStatus(const TSharedPtr<FDesktopCaptureSession, ESPMode::ThreadSafe>& Session, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
#if PLATFORM_WINDOWS
	if (!Session)
	{
		Error = TEXT("capture_session_not_found");
		return false;
	}
	FString State;
	FString SessionError;
	double Started = 0.0;
	double Finished = 0.0;
	{
		FScopeLock Lock(&GStateMutex);
		State = Session->State;
		SessionError = Session->Error;
		Started = Session->StartedAtSeconds;
		Finished = Session->FinishedAtSeconds;
	}
	const double End = Finished > 0.0 ? Finished : FPlatformTime::Seconds();
	const double Duration = Started > 0.0 ? FMath::Max(0.0, End - Started) : 0.0;
	const int64 Frames = Session->FramesWritten.Load();
	Out->SetBoolField(TEXT("success"), State != TEXT("failed"));
	Out->SetStringField(TEXT("status"), State);
	Out->SetStringField(TEXT("session_id"), Session->SessionId);
	Out->SetStringField(TEXT("output_path"), Session->OutputPath);
	Out->SetNumberField(TEXT("frames_written"), static_cast<double>(Frames));
	Out->SetNumberField(TEXT("frames_dropped"), static_cast<double>(Session->FramesDropped.Load()));
	Out->SetNumberField(TEXT("duration_seconds"), Duration);
	Out->SetNumberField(TEXT("actual_fps"), Duration > 0.0 ? Frames / Duration : 0.0);
	Out->SetNumberField(TEXT("file_bytes"), IFileManager::Get().FileSize(*Session->OutputPath));
	FString JobUpdateError;
	const double JobProgress = Session->MaxDurationSeconds > 0 ? FMath::Clamp(Duration / Session->MaxDurationSeconds, 0.0, 0.99) : 0.0;
	if (State == TEXT("capturing") || State == TEXT("finalizing"))
	{
		const FString Payload = FString::Printf(TEXT("{\"executor\":\"desktop_capture\",\"frames_written\":%lld,\"duration_seconds\":%.3f}"), Frames, Duration);
		FSololmcpJobService::UpdateExternalJob(Session->SessionId, TEXT("running"), JobProgress, Payload, Frames, FString(), FString(), JobUpdateError);
	}
	if (!SessionError.IsEmpty()) Out->SetStringField(TEXT("error"), SessionError);
	Summary = FString::Printf(TEXT("Desktop capture %s is %s (%lld frames)."), *Session->SessionId, *State, Frames);
	return State != TEXT("failed");
#else
	Error = TEXT("desktop_capture_is_win64_only");
	return false;
#endif
}

static bool ToolDesktopStatus(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	return WriteDesktopStatus(FindDesktopSession(GetString(Args, TEXT("session_id"))), Out, Summary, Error);
}

static bool ToolDesktopStop(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TSharedPtr<FDesktopCaptureSession, ESPMode::ThreadSafe> Session = FindDesktopSession(GetString(Args, TEXT("session_id")));
	if (!Session)
	{
		Error = TEXT("capture_session_not_found");
		return false;
	}
#if PLATFORM_WINDOWS
	Session->bStopRequested.Store(true);
	{
		FScopeLock Lock(&GStateMutex);
		if (Session->State == TEXT("capturing") || Session->State == TEXT("starting")) Session->State = TEXT("finalizing");
	}
#endif
	return WriteDesktopStatus(Session, Out, Summary, Error);
}

static bool ToolDesktopCancel(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	TSharedPtr<FDesktopCaptureSession, ESPMode::ThreadSafe> Session = FindDesktopSession(GetString(Args, TEXT("session_id")));
	if (!Session)
	{
		Error = TEXT("capture_session_not_found");
		return false;
	}
#if PLATFORM_WINDOWS
	Session->bCancelRequested.Store(true);
	Session->bStopRequested.Store(true);
	{
		FScopeLock Lock(&GStateMutex);
		if (Session->State == TEXT("capturing") || Session->State == TEXT("starting")) Session->State = TEXT("cancelling");
	}
#endif
	return WriteDesktopStatus(Session, Out, Summary, Error);
}

static bool ToolRenderPreflight(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	const FString SequencePath = GetString(Args, TEXT("sequence_path"));
	const FString GraphPath = GetString(Args, TEXT("graph_path"));
	FString RequiredRhi = GetString(Args, TEXT("required_rhi"), CurrentRhiName());
	RequiredRhi.ToLowerInline();
	if (RequiredRhi != TEXT("dx12") && RequiredRhi != TEXT("vulkan"))
	{
		Error = TEXT("required_rhi_must_be_dx12_or_vulkan");
		return false;
	}

	TArray<FString> Blockers;
	TArray<FString> Warnings;
	const FString ActualRhi = CurrentRhiName();
	if (ActualRhi != RequiredRhi)
	{
		Blockers.Add(FString::Printf(TEXT("rhi_mismatch: editor=%s required=%s; restart editor with the required RHI"), *ActualRhi, *RequiredRhi));
	}
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
	if (!Sequence)
	{
		Blockers.Add(TEXT("sequence_asset_missing_or_wrong_type"));
	}
	else if (Sequence->GetOutermost()->IsDirty())
	{
		Blockers.Add(TEXT("sequence_package_is_dirty"));
	}
	UObject* Graph = GraphPath.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *GraphPath);
	if (!GraphPath.IsEmpty() && !Graph)
	{
		Blockers.Add(TEXT("movie_render_graph_asset_missing"));
	}
	else if (Graph && Graph->GetOutermost()->IsDirty())
	{
		Blockers.Add(TEXT("movie_render_graph_package_is_dirty"));
	}

	FString OutputDirectory = GetString(Args, TEXT("output_directory"), FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MovieRenders")));
	if (FPaths::IsRelative(OutputDirectory)) OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), OutputDirectory);
	FPaths::NormalizeDirectoryName(OutputDirectory);
	uint64 TotalBytes = 0;
	uint64 FreeBytes = 0;
	if (!FPlatformMisc::GetDiskTotalAndFreeSpace(FPaths::GetPath(OutputDirectory), TotalBytes, FreeBytes))
	{
		Warnings.Add(TEXT("disk_free_space_query_failed"));
	}
	const int32 MinimumFreeGb = GetInt(Args, TEXT("minimum_free_gb"), 20, 1, 4096);
	if (FreeBytes > 0 && FreeBytes < static_cast<uint64>(MinimumFreeGb) * 1024ull * 1024ull * 1024ull)
	{
		Blockers.Add(TEXT("insufficient_output_disk_space"));
	}

	const FString PreflightIdentity = SequencePath + TEXT("|") + GraphPath + TEXT("|") + RequiredRhi + TEXT("|") + OutputDirectory;
	const FString InputHash = FMD5::HashAnsiString(*PreflightIdentity);
	FString Token;
	const int32 TtlSeconds = GetInt(Args, TEXT("token_ttl_seconds"), 300, 30, 1800);
	if (Blockers.IsEmpty())
	{
		Token = FString::Printf(TEXT("mrpf-%s-%s"), *GEngineInstanceId.Left(8), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FScopeLock Lock(&GStateMutex);
		GPreflightTokens.Add(Token, {Token, InputHash, FPlatformTime::Seconds() + TtlSeconds});
	}

	Out->SetBoolField(TEXT("success"), Blockers.IsEmpty());
	Out->SetStringField(TEXT("status"), Blockers.IsEmpty() ? TEXT("ready") : TEXT("blocked"));
	Out->SetStringField(TEXT("engine_instance_id"), GEngineInstanceId);
	Out->SetStringField(TEXT("required_rhi"), RequiredRhi);
	Out->SetStringField(TEXT("current_rhi"), ActualRhi);
	Out->SetStringField(TEXT("input_hash"), InputHash);
	Out->SetStringField(TEXT("preflight_token"), Token);
	Out->SetNumberField(TEXT("expires_in_ms"), Token.IsEmpty() ? 0 : TtlSeconds * 1000);
	Out->SetStringField(TEXT("output_directory"), OutputDirectory);
	Out->SetNumberField(TEXT("disk_free_bytes"), static_cast<double>(FreeBytes));
	Out->SetArrayField(TEXT("blockers"), StringArray(Blockers));
	Out->SetArrayField(TEXT("warnings"), StringArray(Warnings));
	Summary = Blockers.IsEmpty() ? TEXT("Movie Render Graph preflight passed and issued an instance-bound token.") : FString::Printf(TEXT("Movie Render Graph preflight blocked by %d condition(s)."), Blockers.Num());
	return true;
}

static AActor* ResolveSubject(UWorld* World, const FString& SubjectRef)
{
	if (!World || SubjectRef.IsEmpty()) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->GetActorNameOrLabel().Equals(SubjectRef, ESearchCase::IgnoreCase) || Actor->GetName().Equals(SubjectRef, ESearchCase::IgnoreCase)) return Actor;
		for (const FName& Tag : Actor->Tags)
		{
			if (Tag.ToString().Equals(SubjectRef, ESearchCase::IgnoreCase)) return Actor;
		}
	}
	return nullptr;
}

static bool ToolCameraPathAudit(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	const FString SequencePath = GetString(Args, TEXT("sequence_path"));
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *SequencePath);
	UWorld* World = ResolveAuditWorld();
	if (!Sequence || !Sequence->GetMovieScene())
	{
		Error = TEXT("sequence_asset_missing_or_invalid");
		return false;
	}
	if (!World)
	{
		Error = TEXT("no_editor_or_pie_world_available");
		return false;
	}

	const int32 SampleCount = GetInt(Args, TEXT("sample_count"), 120, 2, 2000);
	const float CollisionRadiusCm = static_cast<float>(GetInt(Args, TEXT("collision_radius_cm"), 20, 0, 10000));
	AActor* Subject = ResolveSubject(World, GetString(Args, TEXT("subject_ref")));
	FMovieSceneSequencePlaybackSettings Settings;
	Settings.bPauseAtEnd = true;
	ALevelSequenceActor* SequenceActor = nullptr;
	ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(World, Sequence, Settings, SequenceActor);
	if (!Player || !SequenceActor)
	{
		Error = TEXT("sequence_evaluation_player_create_failed");
		return false;
	}

	const TRange<FFrameNumber> Range = Sequence->GetMovieScene()->GetPlaybackRange();
	const int32 StartFrame = Range.HasLowerBound() ? Range.GetLowerBoundValue().Value : 0;
	const int32 EndFrame = Range.HasUpperBound() ? Range.GetUpperBoundValue().Value : StartFrame + 1;
	int32 MissingCameraSamples = 0;
	int32 CollisionSamples = 0;
	int32 OccludedSamples = 0;
	float MaxStepCm = 0.0f;
	FVector Previous = FVector::ZeroVector;
	bool bHavePrevious = false;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SOMOLMCP_CameraPathAudit), false);
	if (SequenceActor) QueryParams.AddIgnoredActor(SequenceActor);
	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		const double Alpha = SampleCount > 1 ? static_cast<double>(Index) / (SampleCount - 1) : 0.0;
		const FFrameNumber Frame(static_cast<int32>(FMath::RoundToInt64(FMath::Lerp(static_cast<double>(StartFrame), static_cast<double>(EndFrame - 1), Alpha))));
		Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(FFrameTime(Frame), EUpdatePositionMethod::Jump));
		UCameraComponent* Camera = Player->GetActiveCameraComponent();
		if (!Camera)
		{
			++MissingCameraSamples;
			continue;
		}
		const FVector Location = Camera->GetComponentLocation();
		if (bHavePrevious)
		{
			MaxStepCm = FMath::Max(MaxStepCm, FVector::Distance(Previous, Location));
			if (CollisionRadiusCm > 0.0f)
			{
				FHitResult Hit;
				if (World->SweepSingleByChannel(Hit, Previous, Location, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(CollisionRadiusCm), QueryParams)) ++CollisionSamples;
			}
		}
		if (Subject)
		{
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Location, Subject->GetActorLocation(), ECC_Visibility, QueryParams) && Hit.GetActor() != Subject) ++OccludedSamples;
		}
		Previous = Location;
		bHavePrevious = true;
	}
	SequenceActor->Destroy();

	const bool bSubjectRequested = !GetString(Args, TEXT("subject_ref")).IsEmpty();
	const bool bSubjectResolved = Subject != nullptr;
	const bool bPass = MissingCameraSamples == 0 && CollisionSamples == 0 &&
		OccludedSamples == 0 && (!bSubjectRequested || bSubjectResolved);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("status"), bPass ? TEXT("pass") : TEXT("issues_found"));
	Out->SetStringField(TEXT("evaluation_plane"), GEditor && GEditor->PlayWorld ? TEXT("pie_runtime") : TEXT("editor_runtime_evaluation"));
	Out->SetBoolField(TEXT("sampled_estimate"), true);
	Out->SetNumberField(TEXT("sample_count"), SampleCount);
	Out->SetNumberField(TEXT("missing_camera_samples"), MissingCameraSamples);
	Out->SetNumberField(TEXT("collision_samples"), CollisionSamples);
	Out->SetNumberField(TEXT("occluded_subject_samples"), OccludedSamples);
	Out->SetNumberField(TEXT("max_camera_step_cm"), MaxStepCm);
	Out->SetBoolField(TEXT("subject_requested"), bSubjectRequested);
	Out->SetBoolField(TEXT("subject_resolved"), bSubjectResolved);
	Out->SetStringField(TEXT("subject_ref"), GetString(Args, TEXT("subject_ref")));
	Summary = FString::Printf(TEXT("Sampled %d evaluated camera frames: %d collision and %d missing-camera samples."), SampleCount, CollisionSamples, MissingCameraSamples);
	return true;
}

#if SOMOLMCP_WITH_WORLDFORGE
static FString TierToString(ESomolFaunaPresentationTier Tier)
{
	switch (Tier)
	{
	case ESomolFaunaPresentationTier::T0Individuals: return TEXT("T0Individuals");
	case ESomolFaunaPresentationTier::T1GroupProxy: return TEXT("T1GroupProxy");
	case ESomolFaunaPresentationTier::T2Impostor: return TEXT("T2Impostor");
	default: return TEXT("Unknown");
	}
}

static bool ToolWorldForgePresentationAudit(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>& Args, TSharedRef<FJsonObject>& Out, FString& Summary, FString& Error)
{
	UWorld* World = ResolveAuditWorld();
	USomolRuntimeFaunaSubsystem* Fauna = World ? World->GetSubsystem<USomolRuntimeFaunaSubsystem>() : nullptr;
	if (!Fauna)
	{
		Error = TEXT("runtime_fauna_subsystem_unavailable_in_active_world");
		return false;
	}
	TArray<FSomolFaunaPresentationState> States = Fauna->GetAllFaunaPresentationStates();
	TSet<FString> Filter;
	const TArray<TSharedPtr<FJsonValue>>* RequestedIds = nullptr;
	if (Args->TryGetArrayField(TEXT("logical_ids"), RequestedIds) && RequestedIds)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RequestedIds) Filter.Add(Value->AsString());
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int64 LogicalCount = 0;
	int64 RepresentedCount = 0;
	int64 VisualCount = 0;
	int32 TransitionCount = 0;
	int32 DuplicateOwnerCount = 0;
	int32 InvalidLogicalOwnerCount = 0;
	int32 MissingVisualRepresentationCount = 0;
	TSet<FString> Owners;
	for (const FSomolFaunaPresentationState& State : States)
	{
		if (!Filter.IsEmpty() && !Filter.Contains(State.GroupId) && !Filter.Contains(State.LogicalOwnerKey)) continue;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("domain"), TEXT("fauna"));
		Row->SetStringField(TEXT("logical_id"), State.GroupId);
		Row->SetStringField(TEXT("logical_owner_key"), State.LogicalOwnerKey);
		Row->SetStringField(TEXT("current_tier"), TierToString(State.CurrentTier));
		Row->SetStringField(TEXT("target_tier"), TierToString(State.TargetTier));
		Row->SetBoolField(TEXT("transition_active"), State.bTransitionActive);
		Row->SetNumberField(TEXT("logical_owner_count"), State.LogicalOwnerCount);
		Row->SetNumberField(TEXT("represented_individual_count"), State.RepresentedIndividualCount);
		Row->SetNumberField(TEXT("active_visual_representation_count"), State.ActiveVisualRepresentationCount);
		Row->SetNumberField(TEXT("source_server_revision"), static_cast<double>(State.SourceServerRevision));
		Row->SetStringField(TEXT("state_hash"), State.StateHash);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		LogicalCount += State.LogicalOwnerCount;
		RepresentedCount += State.RepresentedIndividualCount;
		VisualCount += State.ActiveVisualRepresentationCount;
		TransitionCount += State.bTransitionActive ? 1 : 0;
		if (State.LogicalOwnerCount != 1) ++InvalidLogicalOwnerCount;
		if (State.RepresentedIndividualCount > 0 && State.ActiveVisualRepresentationCount <= 0) ++MissingVisualRepresentationCount;
		if (Owners.Contains(State.LogicalOwnerKey)) ++DuplicateOwnerCount;
		Owners.Add(State.LogicalOwnerKey);
	}
	const bool bCountConserved = LogicalCount == Rows.Num();
	const int32 IssueCount = DuplicateOwnerCount + InvalidLogicalOwnerCount + MissingVisualRepresentationCount + (bCountConserved ? 0 : 1);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("status"), IssueCount == 0 ? TEXT("pass") : TEXT("issues_found"));
	Out->SetStringField(TEXT("evaluation_plane"), GEditor && GEditor->PlayWorld ? TEXT("pie_runtime") : TEXT("editor_world"));
	Out->SetArrayField(TEXT("states"), Rows);
	Out->SetNumberField(TEXT("state_count"), Rows.Num());
	Out->SetNumberField(TEXT("logical_owner_count"), static_cast<double>(LogicalCount));
	Out->SetNumberField(TEXT("represented_individual_count"), static_cast<double>(RepresentedCount));
	Out->SetNumberField(TEXT("active_visual_representation_count"), static_cast<double>(VisualCount));
	Out->SetNumberField(TEXT("active_transition_count"), TransitionCount);
	Out->SetNumberField(TEXT("duplicate_logical_owner_count"), DuplicateOwnerCount);
	Out->SetNumberField(TEXT("invalid_logical_owner_count"), InvalidLogicalOwnerCount);
	Out->SetNumberField(TEXT("missing_visual_representation_count"), MissingVisualRepresentationCount);
	Out->SetNumberField(TEXT("issue_count"), IssueCount);
	Out->SetBoolField(TEXT("logical_owner_count_conserved"), bCountConserved);
	Summary = FString::Printf(TEXT("Audited %d live WorldForge fauna presentation states representing %lld individuals."), Rows.Num(), RepresentedCount);
	return true;
}
#endif

static TSharedRef<FJsonObject> SessionIdSchema()
{
	return FSololmcpSchemaBuilder::Object({
		{TEXT("session_id"), FSololmcpSchemaBuilder::String(TEXT("Opaque capture session identifier."), {}, 1, 128)}
	}, {TEXT("session_id")}, TEXT("Capture session lookup."), false);
}

static void RegisterTool(FSololmcpToolRegistry& Registry, const FString& Name, const FString& Description, const TSharedRef<FJsonObject>& Schema,
	TFunction<bool(const FSololmcpToolExecutionContext&, const TSharedRef<FJsonObject>&, TSharedRef<FJsonObject>&, FString&, FString&)> Execute, int32 CacheTtl = 0)
{
	FSololmcpToolDefinition Def;
	Def.Name = Name;
	Def.Description = Description;
	Def.InputSchema = Schema;
	Def.Execute = MoveTemp(Execute);
	Def.CacheTtlSeconds = CacheTtl;
	Registry.Register(Def);
}
}

void RegisterVideoAutomationTools(FSololmcpToolRegistry& Registry)
{
	using namespace VideoAutomation;
	// Promote the existing production names before the legacy bridge wrappers and
	// before the aliases below. FSololmcpToolRegistry is intentionally first-wins.
	RegisterVideoProductionUpgradeTools(Registry);
	const TSharedRef<FJsonObject> RenderSubmitSchema = FSololmcpSchemaBuilder::Object({
		{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence object path."))},
		{TEXT("sequence_asset"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for sequence_path."))},
		{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Movie Render Graph asset path."))},
		{TEXT("preset_path"), FSololmcpSchemaBuilder::String(TEXT("Optional graph/preset alias."))},
		{TEXT("map_path"), FSololmcpSchemaBuilder::String(TEXT("Optional world asset path; active world is used when omitted."))},
		{TEXT("output_directory"), FSololmcpSchemaBuilder::String(TEXT("Project/Saved-bounded output directory."))},
		{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for output_directory."))},
		{TEXT("preflight_token"), FSololmcpSchemaBuilder::String(TEXT("Optional instance-bound preflight token."))},
		{TEXT("client_request_id"), FSololmcpSchemaBuilder::String(TEXT("Shared Job Runtime idempotency key."))},
		{TEXT("job_name"), FSololmcpSchemaBuilder::String(TEXT("Human-readable render job label."))},
		{TEXT("width"), FSololmcpSchemaBuilder::Integer(TEXT("Output width for basic mode."), 64, 16384)},
		{TEXT("height"), FSololmcpSchemaBuilder::Integer(TEXT("Output height for basic mode."), 64, 16384)},
		{TEXT("overwrite_policy"), FSololmcpSchemaBuilder::String(TEXT("error or replace_owned."), {TEXT("error"), TEXT("replace_owned")})}
	}, {}, TEXT("Native Movie Render Graph/Pipeline submission."), false);
	const TSharedRef<FJsonObject> RenderJobSchema = FSololmcpSchemaBuilder::Object({
		{TEXT("job_id"), FSololmcpSchemaBuilder::String(TEXT("Canonical Job Runtime id."))},
		{TEXT("render_id"), FSololmcpSchemaBuilder::String(TEXT("Legacy alias for job_id."))},
		{TEXT("max_files"), FSololmcpSchemaBuilder::Integer(TEXT("Artifact readback cap."), 1, 10000)}
	}, {}, TEXT("Movie render job lookup."), false);
	// Existing names are promoted from contract wrappers to one canonical native
	// executor. Registration order guarantees every alias shares the same job ids.
	RegisterTool(Registry, TEXT("movie_render_graph_job_submit"), TEXT("Submit a native UE Movie Render Graph/Pipeline job into the shared Job Runtime; a bound preflight token and idempotency key are mandatory."), RenderSubmitSchema, ToolMovieRenderSubmitCanonical);
	for (const FString& Name : {TEXT("render_queue_submit"), TEXT("mrq_job_create"), TEXT("sequencer_mrq_job_from_sequence"), TEXT("sequencer_mrq_queue_submit")})
	{
		RegisterTool(Registry, Name, TEXT("Submit a native UE Movie Render Graph/Pipeline job into the shared Job Runtime."), RenderSubmitSchema, ToolMovieRenderSubmit);
	}
	for (const FString& Name : {TEXT("movie_render_graph_job_status_get"), TEXT("mrq_render_status"), TEXT("sequencer_mrq_job_poll")})
	{
		RegisterTool(Registry, Name, TEXT("Read canonical native movie render progress and artifact state."), RenderJobSchema, ToolMovieRenderStatus);
	}
	for (const FString& Name : {TEXT("movie_render_graph_job_cancel"), TEXT("render_queue_cancel"), TEXT("sequencer_mrq_job_cancel")})
	{
		RegisterTool(Registry, Name, TEXT("Cancel a native movie render job and release its shared Job Runtime locks."), RenderJobSchema, ToolMovieRenderCancel);
	}
	for (const FString& Name : {TEXT("render_queue_list"), TEXT("mrq_queue_list")})
	{
		RegisterTool(Registry, Name, TEXT("List canonical native movie render jobs."), FSololmcpSchemaBuilder::Object({}, {}, TEXT("No arguments."), false), ToolMovieRenderList);
	}
	for (const FString& Name : {TEXT("movie_render_graph_output_artifact_readback"), TEXT("movie_render_graph_visual_qa"), TEXT("mrq_output_validate")})
	{
		RegisterTool(Registry, Name, TEXT("Read back and validate native movie render output artifacts."), RenderJobSchema, ToolMovieRenderArtifacts);
	}
	RegisterTool(Registry, TEXT("desktop_capture_source_list"), TEXT("Enumerate opaque Win64 desktop/window capture sources without exposing native handles."),
		FSololmcpSchemaBuilder::Object({}, {}, TEXT("No arguments."), false), ToolDesktopSourceList, 0);
	RegisterTool(Registry, TEXT("desktop_capture_session_start"), TEXT("Start a native continuous H.264/MP4 desktop capture session under the project Saved directory."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("source_id"), FSololmcpSchemaBuilder::String(TEXT("Opaque source id returned by desktop_capture_source_list."), {}, 1, 128)},
			{TEXT("client_request_id"), FSololmcpSchemaBuilder::String(TEXT("Optional idempotency key in the shared Job Runtime."), {}, 1, 256)},
			{TEXT("output_directory"), FSololmcpSchemaBuilder::String(TEXT("Absolute or Saved-relative output directory; must remain beneath Project/Saved."))},
			{TEXT("fps"), FSololmcpSchemaBuilder::Integer(TEXT("Capture frames per second."), 1, 60)},
			{TEXT("bitrate_mbps"), FSololmcpSchemaBuilder::Integer(TEXT("H.264 target bitrate in Mbps."), 2, 120)},
			{TEXT("max_duration_seconds"), FSololmcpSchemaBuilder::Integer(TEXT("Automatic stop duration; 0 disables."), 0, 86400)}
		}, {TEXT("source_id")}, TEXT("Desktop capture start request."), false), ToolDesktopStart);
	RegisterTool(Registry, TEXT("desktop_capture_session_status_get"), TEXT("Read live frame, duration, drop and output statistics for a desktop capture session."), SessionIdSchema(), ToolDesktopStatus);
	RegisterTool(Registry, TEXT("desktop_capture_session_stop"), TEXT("Request graceful MP4 finalization for an active desktop capture session."), SessionIdSchema(), ToolDesktopStop);
	RegisterTool(Registry, TEXT("desktop_capture_session_cancel"), TEXT("Cancel a desktop capture session and delete only its owned partial output."), SessionIdSchema(), ToolDesktopCancel);
	RegisterTool(Registry, TEXT("movie_render_graph_capture_preflight"), TEXT("Fail-closed preflight for sequence, graph, dirty packages, output disk and launch-bound DX12/Vulkan RHI; issues an instance-bound token."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence object path."), {}, 1, 512)},
			{TEXT("graph_path"), FSololmcpSchemaBuilder::String(TEXT("Optional Movie Render Graph object path."), {}, 0, 512)},
			{TEXT("required_rhi"), FSololmcpSchemaBuilder::String(TEXT("Launch-bound RHI."), {TEXT("dx12"), TEXT("vulkan")})},
			{TEXT("output_directory"), FSololmcpSchemaBuilder::String(TEXT("Render output directory."))},
			{TEXT("minimum_free_gb"), FSololmcpSchemaBuilder::Integer(TEXT("Minimum free disk budget."), 1, 4096)},
			{TEXT("token_ttl_seconds"), FSololmcpSchemaBuilder::Integer(TEXT("Preflight token lifetime."), 30, 1800)}
		}, {TEXT("sequence_path"), TEXT("required_rhi")}, TEXT("Movie Render Graph preflight request."), false), ToolRenderPreflight);
	RegisterTool(Registry, TEXT("sequencer_camera_path_audit"), TEXT("Evaluate a Level Sequence in the active editor/PIE world and sample active camera collision, continuity and subject occlusion."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence object path."), {}, 1, 512)},
			{TEXT("subject_ref"), FSololmcpSchemaBuilder::String(TEXT("Optional actor label/name/tag or WorldForge logical id."), {}, 0, 256)},
			{TEXT("sample_count"), FSololmcpSchemaBuilder::Integer(TEXT("Uniform path evaluation sample count."), 2, 2000)},
			{TEXT("collision_radius_cm"), FSololmcpSchemaBuilder::Integer(TEXT("Camera collision sweep radius in centimeters."), 0, 10000)}
		}, {TEXT("sequence_path")}, TEXT("Camera path audit request."), false), ToolCameraPathAudit);
#if SOMOLMCP_WITH_WORLDFORGE
	RegisterTool(Registry, TEXT("worldforge_runtime_presentation_audit"), TEXT("Audit live WorldForge fauna logical-owner, representation-tier, transition and represented-individual continuity state."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("logical_ids"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::String(), TEXT("Optional fauna group/logical owner filter."), 0, 4096, true)}
		}, {}, TEXT("Runtime presentation audit request."), false), ToolWorldForgePresentationAudit);
#endif
}
}
