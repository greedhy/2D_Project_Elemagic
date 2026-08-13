// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.6 — Media & Ingest Tools (Stage-13)
//
// 职责：
//   1. 本地 .mp4/.mov/.webm 等视频文件 ingest 到 UE 项目 /Game/Media/ 目录
//   2. 图片序列（.exr/.png/.jpg 目录）→ UImgMediaSource
//   3. 流媒体 URL（rtsp://, http(s)://.m3u8, rtmp://）→ UStreamMediaSource
//   4. MediaSource → MediaPlayer → MediaTexture → MediaPlaylist 链式构建
//   5. MediaPlayer 播放/暂停/停止/寻址控制
//   6. 媒体资产元数据回读（用于 QA 验证）
//
// 设计原则：
//   - 所有工具在游戏线程执行（Media 子系统 + AssetRegistry 都要求 GT）
//   - 使用 NewObject + AssetRegistryModule::AssetCreated 直接创建资产，
//     避开 MediaPlayerEditor 模块依赖（Factory 在 Editor 模块中，而 AssetTools 路径
//     需要完整的 *FactoryNew 类名，维护成本高）
//   - 路径格式统一：asset_path = "/Game/Folder/AssetName"，对应 Package
//     "/Game/Folder"、AssetName "AssetName"
//   - ingest 工具会把外部文件拷到项目 Content/Media/ 下并返回 UE 路径

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpJsonUtils.h"

// ── Core / Asset Infra ──
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

// ── Media Assets ──
#include "FileMediaSource.h"
#include "StreamMediaSource.h"
#include "ImgMediaSource.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "MediaPlaylist.h"
#include "MediaSource.h"

// ── Level / Actor (for mount) ──
#include "GameFramework/Actor.h"
#include "EngineUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogSOMOLMCPMediaIngest, Log, All);

namespace UE::SOMOLMCP
{

// ═══════════════════════════════════════════════════════════════════════════
//  Internal Helpers
// ═══════════════════════════════════════════════════════════════════════════

/** Media + ingest tools touch the AssetRegistry / Editor world; require Game Thread. */
static bool EnsureGameThread(FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("Media/Ingest tools must be called from the game thread.");
		return false;
	}
	return true;
}

/** Split "/Game/Foo/Bar" → package "/Game/Foo", asset "Bar". */
static bool SplitAssetPath(const FString& AssetPath, FString& OutPackage, FString& OutName)
{
	int32 LastSlash = INDEX_NONE;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash <= 0 || LastSlash >= AssetPath.Len() - 1)
	{
		return false;
	}
	OutPackage = AssetPath.Left(LastSlash);
	OutName = AssetPath.RightChop(LastSlash + 1);
	// Strip possible "AssetName.AssetName" suffix.
	int32 Dot = INDEX_NONE;
	if (OutName.FindChar('.', Dot))
	{
		OutName = OutName.Left(Dot);
	}
	return !OutName.IsEmpty();
}

/**
 *  Create a new asset via direct NewObject + AssetRegistry notification.
 *  Returns the created UObject (already marked dirty + registered), or nullptr on error.
 */
template <typename TAsset>
static TAsset* CreateAssetDirect(const FString& PackagePath, const FString& AssetName, FString& OutError)
{
	const FString FullPackageName = PackagePath / AssetName;
	const FString FullAssetPath = FullPackageName + TEXT(".") + AssetName;

	// Validate target package does not already exist as a loaded asset.
	if (FPackageName::DoesPackageExist(FullPackageName) || UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
	{
		OutError = FString::Printf(TEXT("Asset already exists: %s"), *FullPackageName);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*FullPackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("CreatePackage failed for %s"), *FullPackageName);
		return nullptr;
	}
	Package->FullyLoad();

	TAsset* Asset = NewObject<TAsset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("NewObject failed for %s"), *AssetName);
		return nullptr;
	}

	Asset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}

/** Save an in-memory UObject back to its package on disk. */
static bool SaveAssetToDisk(UObject* Asset, FString& OutError)
{
	if (!Asset)
	{
		OutError = TEXT("SaveAssetToDisk: null asset");
		return false;
	}
	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("SaveAssetToDisk: null package");
		return false;
	}
	const FString AssetPath = Asset->GetPathName();
	UClass* ExpectedClass = Asset->GetClass();
	Package->SetDirtyFlag(true);

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;

	if (!UPackage::SavePackage(Package, Asset, *PackageFileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("SavePackage failed for %s"), *PackageFileName);
		return false;
	}
	if (!FPackageName::DoesPackageExist(Package->GetName()))
	{
		OutError = FString::Printf(TEXT("Saved package was not found on disk after SavePackage: %s"), *Package->GetName());
		return false;
	}
	UObject* Reloaded = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Reloaded || !Reloaded->IsA(ExpectedClass))
	{
		OutError = FString::Printf(TEXT("Saved asset reload/class validation failed for %s"), *AssetPath);
		return false;
	}
	return true;
}

/** Resolve "/Game/Foo/Bar" to a loaded UObject*, or nullptr. */
static UObject* LoadAssetByPath(const FString& AssetPath, FString& OutError)
{
	UObject* Obj = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!Obj)
	{
		OutError = FString::Printf(TEXT("Cannot load asset: %s"), *AssetPath);
		return nullptr;
	}
	return Obj;
}

/** Copy an external file to the project Content/Media/ directory and return the on-disk UE /Game/Media/... folder. */
static bool CopyExternalFileToProjectMedia(const FString& ExternalPath, const FString& SubFolder, FString& OutProjectDiskPath, FString& OutUEFolder, FString& OutError)
{
	IPlatformFile& FM = FPlatformFileManager::Get().GetPlatformFile();
	if (!FM.FileExists(*ExternalPath))
	{
		OutError = FString::Printf(TEXT("External file not found: %s"), *ExternalPath);
		return false;
	}

	const FString ContentDir = FPaths::ProjectContentDir();  // e.g. <ProjectDir>/Content/
	const FString MediaRootDisk = ContentDir / TEXT("Media");
	const FString TargetDir = SubFolder.IsEmpty() ? MediaRootDisk : (MediaRootDisk / SubFolder);

	if (!FM.DirectoryExists(*TargetDir))
	{
		if (!FM.CreateDirectoryTree(*TargetDir))
		{
			OutError = FString::Printf(TEXT("Cannot create target directory: %s"), *TargetDir);
			return false;
		}
	}

	const FString FileName = FPaths::GetCleanFilename(ExternalPath);
	OutProjectDiskPath = TargetDir / FileName;

	// Copy with overwrite.
	if (!FM.CopyFile(*OutProjectDiskPath, *ExternalPath, EPlatformFileRead::None, EPlatformFileWrite::AllowRead))
	{
		OutError = FString::Printf(TEXT("File copy failed: %s → %s"), *ExternalPath, *OutProjectDiskPath);
		return false;
	}

	OutUEFolder = SubFolder.IsEmpty() ? FString(TEXT("/Game/Media")) : (FString(TEXT("/Game/Media/")) + SubFolder);
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Tool Implementations
// ═══════════════════════════════════════════════════════════════════════════

// ── media_source_create ─────────────────────────────────────────────────────
// 创建 FileMediaSource（对应本地文件）
static bool Tool_MediaSourceCreate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, FilePath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path (e.g. /Game/Media/MS_Intro)");
		return false;
	}
	if (!Arguments->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		OutError = TEXT("Missing file_path (absolute disk path to media file)");
		return false;
	}

	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = TEXT("Malformed asset_path");
		return false;
	}

	UFileMediaSource* Src = CreateAssetDirect<UFileMediaSource>(Package, Name, OutError);
	if (!Src) return false;

	Src->SetFilePath(FilePath);

	bool bPrecache = false;
	if (Arguments->TryGetBoolField(TEXT("precache_file"), bPrecache))
	{
		Src->PrecacheFile = bPrecache;
	}

	if (!SaveAssetToDisk(Src, OutError))
	{
		return false;
	}

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("FileMediaSource"));
	OutStructured->SetStringField(TEXT("file_path"), FilePath);
	OutSummary = FString::Printf(TEXT("Created FileMediaSource %s → %s"), *AssetPath, *FilePath);
	return true;
}

// ── stream_source_create ────────────────────────────────────────────────────
// 创建 StreamMediaSource（对应 URL）
static bool Tool_StreamSourceCreate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, StreamUrl;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path (e.g. /Game/Media/SS_LiveCam)");
		return false;
	}
	if (!Arguments->TryGetStringField(TEXT("stream_url"), StreamUrl) || StreamUrl.IsEmpty())
	{
		OutError = TEXT("Missing stream_url (rtsp://, http(s)://*.m3u8, rtmp://)");
		return false;
	}

	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = TEXT("Malformed asset_path");
		return false;
	}

	UStreamMediaSource* Src = CreateAssetDirect<UStreamMediaSource>(Package, Name, OutError);
	if (!Src) return false;

	Src->StreamUrl = StreamUrl;
	if (!SaveAssetToDisk(Src, OutError)) return false;

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("StreamMediaSource"));
	OutStructured->SetStringField(TEXT("stream_url"), StreamUrl);
	OutSummary = FString::Printf(TEXT("Created StreamMediaSource %s → %s"), *AssetPath, *StreamUrl);
	return true;
}

// ── img_media_source_create ─────────────────────────────────────────────────
// 创建 ImgMediaSource（对应图片序列目录）
static bool Tool_ImgMediaSourceCreate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, SequencePath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path (e.g. /Game/Media/IMG_Seq01)");
		return false;
	}
	if (!Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath) || SequencePath.IsEmpty())
	{
		OutError = TEXT("Missing sequence_path (absolute directory of image sequence)");
		return false;
	}

	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = TEXT("Malformed asset_path");
		return false;
	}

	UImgMediaSource* Src = CreateAssetDirect<UImgMediaSource>(Package, Name, OutError);
	if (!Src) return false;

	Src->SetSequencePath(SequencePath);

	double FrameRate = 24.0;
	if (Arguments->TryGetNumberField(TEXT("frame_rate"), FrameRate) && FrameRate > 0.0)
	{
		Src->FrameRateOverride = FFrameRate(FMath::RoundToInt(FrameRate * 1000), 1000);
	}

	if (!SaveAssetToDisk(Src, OutError)) return false;

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("ImgMediaSource"));
	OutStructured->SetStringField(TEXT("sequence_path"), SequencePath);
	OutStructured->SetNumberField(TEXT("frame_rate"), FrameRate);
	OutSummary = FString::Printf(TEXT("Created ImgMediaSource %s → %s @ %.2f fps"), *AssetPath, *SequencePath, FrameRate);
	return true;
}

// ── media_player_create ─────────────────────────────────────────────────────
static bool Tool_MediaPlayerCreate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path (e.g. /Game/Media/MP_Intro)");
		return false;
	}

	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = TEXT("Malformed asset_path");
		return false;
	}

	UMediaPlayer* Player = CreateAssetDirect<UMediaPlayer>(Package, Name, OutError);
	if (!Player) return false;

	bool bPlayOnOpen = true;
	bool bLoop = false;
	Arguments->TryGetBoolField(TEXT("play_on_open"), bPlayOnOpen);
	Arguments->TryGetBoolField(TEXT("loop"), bLoop);
	Player->PlayOnOpen = bPlayOnOpen;
	Player->SetLooping(bLoop);

	if (!SaveAssetToDisk(Player, OutError)) return false;

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("MediaPlayer"));
	OutStructured->SetBoolField(TEXT("play_on_open"), bPlayOnOpen);
	OutStructured->SetBoolField(TEXT("loop"), bLoop);
	OutSummary = FString::Printf(TEXT("Created MediaPlayer %s (play_on_open=%s, loop=%s)"),
		*AssetPath, bPlayOnOpen ? TEXT("true") : TEXT("false"), bLoop ? TEXT("true") : TEXT("false"));
	return true;
}

// ── media_texture_create ────────────────────────────────────────────────────
static bool Tool_MediaTextureCreate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath, PlayerPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path (e.g. /Game/Media/MT_Intro)");
		return false;
	}

	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = TEXT("Malformed asset_path");
		return false;
	}

	UMediaTexture* Tex = CreateAssetDirect<UMediaTexture>(Package, Name, OutError);
	if (!Tex) return false;

	// Optional: bind to a MediaPlayer
	if (Arguments->TryGetStringField(TEXT("media_player_path"), PlayerPath) && !PlayerPath.IsEmpty())
	{
		UObject* Obj = LoadAssetByPath(PlayerPath, OutError);
		if (!Obj) return false;
		UMediaPlayer* Player = Cast<UMediaPlayer>(Obj);
		if (!Player)
		{
			OutError = FString::Printf(TEXT("%s is not a MediaPlayer"), *PlayerPath);
			return false;
		}
		Tex->SetMediaPlayer(Player);
	}

	if (!SaveAssetToDisk(Tex, OutError)) return false;

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("MediaTexture"));
	if (!PlayerPath.IsEmpty())
	{
		OutStructured->SetStringField(TEXT("media_player_path"), PlayerPath);
	}
	OutSummary = FString::Printf(TEXT("Created MediaTexture %s%s"), *AssetPath,
		PlayerPath.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" bound to %s"), *PlayerPath));
	return true;
}

// ── media_playlist_create ───────────────────────────────────────────────────
static bool Tool_MediaPlaylistCreate(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path (e.g. /Game/Media/PL_Show)");
		return false;
	}

	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = TEXT("Malformed asset_path");
		return false;
	}

	UMediaPlaylist* Playlist = CreateAssetDirect<UMediaPlaylist>(Package, Name, OutError);
	if (!Playlist) return false;

	// Optional initial sources.
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	int32 Added = 0;
	if (Arguments->TryGetArrayField(TEXT("sources"), Items) && Items)
	{
		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			const FString SrcPath = V->AsString();
			if (SrcPath.IsEmpty()) continue;
			UObject* Obj = LoadAssetByPath(SrcPath, OutError);
			if (!Obj) return false;
			UMediaSource* Src = Cast<UMediaSource>(Obj);
			if (!Src)
			{
				OutError = FString::Printf(TEXT("%s is not a MediaSource"), *SrcPath);
				return false;
			}
			if (!Playlist->Add(Src))
			{
				OutError = FString::Printf(TEXT("Playlist refused source: %s"), *SrcPath);
				OutStructured->SetStringField(TEXT("status"), Added > 0 ? TEXT("partial_failed") : TEXT("failed"));
				OutStructured->SetNumberField(TEXT("sources_added_before_failure"), Added);
				return false;
			}
			++Added;
		}
	}

	if (!SaveAssetToDisk(Playlist, OutError)) return false;

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("MediaPlaylist"));
	OutStructured->SetNumberField(TEXT("source_count"), Added);
	OutSummary = FString::Printf(TEXT("Created MediaPlaylist %s (%d sources)"), *AssetPath, Added);
	return true;
}

// ── media_playlist_add ──────────────────────────────────────────────────────
static bool Tool_MediaPlaylistAdd(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString PlaylistPath, SourcePath;
	if (!Arguments->TryGetStringField(TEXT("playlist_path"), PlaylistPath) ||
		!Arguments->TryGetStringField(TEXT("source_path"), SourcePath))
	{
		OutError = TEXT("Missing playlist_path or source_path");
		return false;
	}

	UObject* PObj = LoadAssetByPath(PlaylistPath, OutError);
	if (!PObj) return false;
	UMediaPlaylist* Playlist = Cast<UMediaPlaylist>(PObj);
	if (!Playlist) { OutError = TEXT("playlist_path is not a MediaPlaylist"); return false; }

	UObject* SObj = LoadAssetByPath(SourcePath, OutError);
	if (!SObj) return false;
	UMediaSource* Src = Cast<UMediaSource>(SObj);
	if (!Src) { OutError = TEXT("source_path is not a MediaSource"); return false; }

	const int32 BeforeCount = Playlist->Num();
	const bool bAdded = Playlist->Add(Src);
	if (!bAdded || Playlist->Num() <= BeforeCount)
	{
		OutError = FString::Printf(TEXT("Playlist refused source or source count did not increase: %s"), *SourcePath);
		OutStructured->SetStringField(TEXT("playlist_path"), PlaylistPath);
		OutStructured->SetStringField(TEXT("source_path"), SourcePath);
		OutStructured->SetNumberField(TEXT("total_sources"), Playlist->Num());
		OutStructured->SetStringField(TEXT("status"), TEXT("no_op"));
		return false;
	}
	if (!SaveAssetToDisk(Playlist, OutError)) return false;

	OutStructured->SetStringField(TEXT("playlist_path"), PlaylistPath);
	OutStructured->SetStringField(TEXT("source_path"), SourcePath);
	OutStructured->SetNumberField(TEXT("total_sources"), Playlist->Num());
	OutStructured->SetStringField(TEXT("status"), TEXT("updated"));
	OutSummary = FString::Printf(TEXT("Appended %s → %s (now %d sources)"), *SourcePath, *PlaylistPath, Playlist->Num());
	return true;
}

// ── media_player_open_source ────────────────────────────────────────────────
// 让 MediaPlayer 打开（并可选立即播放）给定 MediaSource
static bool Tool_MediaPlayerOpenSource(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString PlayerPath, SourcePath;
	if (!Arguments->TryGetStringField(TEXT("player_path"), PlayerPath) ||
		!Arguments->TryGetStringField(TEXT("source_path"), SourcePath))
	{
		OutError = TEXT("Missing player_path or source_path");
		return false;
	}

	UObject* PObj = LoadAssetByPath(PlayerPath, OutError);
	if (!PObj) return false;
	UMediaPlayer* Player = Cast<UMediaPlayer>(PObj);
	if (!Player) { OutError = TEXT("player_path is not a MediaPlayer"); return false; }

	UObject* SObj = LoadAssetByPath(SourcePath, OutError);
	if (!SObj) return false;
	UMediaSource* Src = Cast<UMediaSource>(SObj);
	if (!Src) { OutError = TEXT("source_path is not a MediaSource"); return false; }

	const bool bOpen = Player->OpenSource(Src);

	OutStructured->SetStringField(TEXT("player_path"), PlayerPath);
	OutStructured->SetStringField(TEXT("source_path"), SourcePath);
	OutStructured->SetBoolField(TEXT("opened"), bOpen);
	OutSummary = FString::Printf(TEXT("%s OpenSource(%s) → %s"),
		*PlayerPath, *SourcePath, bOpen ? TEXT("ok") : TEXT("failed"));
	return bOpen ? true : (OutError = TEXT("OpenSource returned false"), false);
}

// ── media_player_control ────────────────────────────────────────────────────
// 统一的 play/pause/stop/seek/rewind 控制
static bool Tool_MediaPlayerControl(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString PlayerPath, Action;
	if (!Arguments->TryGetStringField(TEXT("player_path"), PlayerPath) ||
		!Arguments->TryGetStringField(TEXT("action"), Action))
	{
		OutError = TEXT("Missing player_path or action");
		return false;
	}

	UObject* Obj = LoadAssetByPath(PlayerPath, OutError);
	if (!Obj) return false;
	UMediaPlayer* Player = Cast<UMediaPlayer>(Obj);
	if (!Player) { OutError = TEXT("player_path is not a MediaPlayer"); return false; }

	const FString ActLower = Action.ToLower();
	bool bResult = false;
	FString ActionNoted = ActLower;

	if (ActLower == TEXT("play"))
	{
		bResult = Player->Play();
	}
	else if (ActLower == TEXT("pause"))
	{
		Player->Pause();
		bResult = true;
	}
	else if (ActLower == TEXT("stop") || ActLower == TEXT("close"))
	{
		Player->Close();
		bResult = true;
	}
	else if (ActLower == TEXT("rewind"))
	{
		bResult = Player->Rewind();
	}
	else if (ActLower == TEXT("seek"))
	{
		double Sec = 0.0;
		Arguments->TryGetNumberField(TEXT("seconds"), Sec);
		bResult = Player->Seek(FTimespan::FromSeconds(Sec));
		ActionNoted = FString::Printf(TEXT("seek(%.3fs)"), Sec);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown action: %s (play|pause|stop|rewind|seek)"), *Action);
		return false;
	}

	OutStructured->SetStringField(TEXT("player_path"), PlayerPath);
	OutStructured->SetStringField(TEXT("action"), ActionNoted);
	OutStructured->SetBoolField(TEXT("ok"), bResult);
	OutStructured->SetBoolField(TEXT("is_playing"), Player->IsPlaying());
	OutStructured->SetBoolField(TEXT("is_paused"), Player->IsPaused());
	OutSummary = FString::Printf(TEXT("MediaPlayer %s action=%s ok=%s playing=%s"),
		*PlayerPath, *ActionNoted,
		bResult ? TEXT("true") : TEXT("false"),
		Player->IsPlaying() ? TEXT("true") : TEXT("false"));
	if (!bResult)
	{
		OutError = FString::Printf(TEXT("MediaPlayer action '%s' returned false."), *ActionNoted);
		return false;
	}
	return true;
}

// ── media_player_play (alias) ───────────────────────────────────────────────
static bool Tool_MediaPlayerPlay(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	// Force action=play and delegate
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	FString PlayerPath;
	if (!Arguments->TryGetStringField(TEXT("player_path"), PlayerPath))
	{
		OutError = TEXT("Missing player_path");
		return false;
	}
	Args->SetStringField(TEXT("player_path"), PlayerPath);
	Args->SetStringField(TEXT("action"), TEXT("play"));
	return Tool_MediaPlayerControl(Context, Args, OutStructured, OutSummary, OutError);
}

// ── media_query ─────────────────────────────────────────────────────────────
// 读取媒体资产元数据，用于 QA 验证
static bool Tool_MediaQuery(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("Missing asset_path");
		return false;
	}

	UObject* Obj = LoadAssetByPath(AssetPath, OutError);
	if (!Obj) return false;

	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("class"), Obj->GetClass()->GetName());

	if (UFileMediaSource* File = Cast<UFileMediaSource>(Obj))
	{
		OutStructured->SetStringField(TEXT("kind"), TEXT("FileMediaSource"));
		OutStructured->SetStringField(TEXT("file_path"), File->GetUrl());
		OutStructured->SetBoolField(TEXT("precache"), File->PrecacheFile);
	}
	else if (UStreamMediaSource* Stream = Cast<UStreamMediaSource>(Obj))
	{
		OutStructured->SetStringField(TEXT("kind"), TEXT("StreamMediaSource"));
		OutStructured->SetStringField(TEXT("stream_url"), Stream->StreamUrl);
	}
	else if (UImgMediaSource* Img = Cast<UImgMediaSource>(Obj))
	{
		OutStructured->SetStringField(TEXT("kind"), TEXT("ImgMediaSource"));
		OutStructured->SetStringField(TEXT("sequence_path"), Img->GetSequencePath());
		const FFrameRate FR = Img->FrameRateOverride;
		OutStructured->SetNumberField(TEXT("frame_rate"), FR.Denominator != 0 ? (double)FR.Numerator / (double)FR.Denominator : 0.0);
	}
	else if (UMediaPlayer* Player = Cast<UMediaPlayer>(Obj))
	{
		OutStructured->SetStringField(TEXT("kind"), TEXT("MediaPlayer"));
		OutStructured->SetBoolField(TEXT("play_on_open"), Player->PlayOnOpen);
		OutStructured->SetBoolField(TEXT("is_looping"), Player->IsLooping());
		OutStructured->SetBoolField(TEXT("is_playing"), Player->IsPlaying());
		OutStructured->SetNumberField(TEXT("duration_seconds"), Player->GetDuration().GetTotalSeconds());
	}
	else if (UMediaTexture* Tex = Cast<UMediaTexture>(Obj))
	{
		OutStructured->SetStringField(TEXT("kind"), TEXT("MediaTexture"));
		if (UMediaPlayer* Bound = Tex->GetMediaPlayer())
		{
			OutStructured->SetStringField(TEXT("media_player_path"), Bound->GetPathName());
		}
	}
	else if (UMediaPlaylist* PL = Cast<UMediaPlaylist>(Obj))
	{
		OutStructured->SetStringField(TEXT("kind"), TEXT("MediaPlaylist"));
		OutStructured->SetNumberField(TEXT("source_count"), PL->Num());
	}
	else
	{
		OutStructured->SetStringField(TEXT("kind"), TEXT("Unknown"));
	}

	OutSummary = FString::Printf(TEXT("Queried %s (%s)"), *AssetPath, *Obj->GetClass()->GetName());
	return true;
}

// ── ingest_video ────────────────────────────────────────────────────────────
// 从磁盘外部路径 ingest 一个视频文件，落到 /Game/Media/<sub_folder>/，并自动建 FileMediaSource
static bool Tool_IngestVideo(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString ExternalPath, SubFolder, AssetName;
	if (!Arguments->TryGetStringField(TEXT("external_path"), ExternalPath) || ExternalPath.IsEmpty())
	{
		OutError = TEXT("Missing external_path (absolute OS path to a video file)");
		return false;
	}
	Arguments->TryGetStringField(TEXT("sub_folder"), SubFolder);
	if (!Arguments->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		// Derive asset name from file base.
		AssetName = FPaths::GetBaseFilename(ExternalPath);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("Cannot derive asset_name from external_path");
			return false;
		}
		if (!AssetName.StartsWith(TEXT("MS_")))
		{
			AssetName = TEXT("MS_") + AssetName;
		}
	}

	FString ProjectDiskPath, UEFolder;
	if (!CopyExternalFileToProjectMedia(ExternalPath, SubFolder, ProjectDiskPath, UEFolder, OutError))
	{
		return false;
	}

	const FString AssetPath = UEFolder / AssetName;
	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = FString::Printf(TEXT("Malformed asset path: %s"), *AssetPath);
		return false;
	}

	UFileMediaSource* Src = CreateAssetDirect<UFileMediaSource>(Package, Name, OutError);
	if (!Src) return false;

	// For FileMediaSource, UE is happy with either an absolute path or a "./Content/..."
	// path relative to project. Using the absolute copy path is the most robust.
	Src->SetFilePath(ProjectDiskPath);
	bool bPrecache = false;
	Arguments->TryGetBoolField(TEXT("precache_file"), bPrecache);
	Src->PrecacheFile = bPrecache;

	if (!SaveAssetToDisk(Src, OutError)) return false;

	OutStructured->SetStringField(TEXT("external_path"), ExternalPath);
	OutStructured->SetStringField(TEXT("dst_project_disk_path"), ProjectDiskPath);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("FileMediaSource"));
	OutSummary = FString::Printf(TEXT("Ingested %s → %s (asset %s)"),
		*ExternalPath, *ProjectDiskPath, *AssetPath);
	return true;
}

// ── ingest_image_sequence ───────────────────────────────────────────────────
// 从一个图片序列目录 ingest（不拷贝图片本身，只登记 ImgMediaSource 指向原目录），
// 也可选 copy=true 把整目录拷到项目 Content/Media/ 下。
static bool Tool_IngestImageSequence(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString ExternalDir, SubFolder, AssetName;
	if (!Arguments->TryGetStringField(TEXT("external_dir"), ExternalDir) || ExternalDir.IsEmpty())
	{
		OutError = TEXT("Missing external_dir (absolute directory of image sequence)");
		return false;
	}
	Arguments->TryGetStringField(TEXT("sub_folder"), SubFolder);

	IPlatformFile& FM = FPlatformFileManager::Get().GetPlatformFile();
	if (!FM.DirectoryExists(*ExternalDir))
	{
		OutError = FString::Printf(TEXT("External directory not found: %s"), *ExternalDir);
		return false;
	}

	if (!Arguments->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		AssetName = FPaths::GetCleanFilename(ExternalDir);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("Cannot derive asset_name from external_dir");
			return false;
		}
		if (!AssetName.StartsWith(TEXT("IMS_")))
		{
			AssetName = TEXT("IMS_") + AssetName;
		}
	}

	const FString UEFolder = SubFolder.IsEmpty() ? FString(TEXT("/Game/Media")) : (FString(TEXT("/Game/Media/")) + SubFolder);
	const FString AssetPath = UEFolder / AssetName;
	FString Package, Name;
	if (!SplitAssetPath(AssetPath, Package, Name))
	{
		OutError = TEXT("Malformed derived asset path");
		return false;
	}

	UImgMediaSource* Src = CreateAssetDirect<UImgMediaSource>(Package, Name, OutError);
	if (!Src) return false;
	Src->SetSequencePath(ExternalDir);

	double FrameRate = 24.0;
	if (Arguments->TryGetNumberField(TEXT("frame_rate"), FrameRate) && FrameRate > 0.0)
	{
		Src->FrameRateOverride = FFrameRate(FMath::RoundToInt(FrameRate * 1000), 1000);
	}

	if (!SaveAssetToDisk(Src, OutError)) return false;

	OutStructured->SetStringField(TEXT("external_dir"), ExternalDir);
	OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
	OutStructured->SetStringField(TEXT("kind"), TEXT("ImgMediaSource"));
	OutStructured->SetNumberField(TEXT("frame_rate"), FrameRate);
	OutSummary = FString::Printf(TEXT("Ingested image sequence %s → %s @ %.2f fps"),
		*ExternalDir, *AssetPath, FrameRate);
	return true;
}

// ── ingest_list ─────────────────────────────────────────────────────────────
// 列出 /Game/Media/ 下现有媒体资产，供 ResourceFinder / QA 使用
static bool Tool_IngestList(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString Folder = TEXT("/Game/Media");
	Arguments->TryGetStringField(TEXT("folder"), Folder);

	TArray<FAssetData> Assets = Context.Services.QueryAssets(Folder, FString(), true, OutError);
	if (!OutError.IsEmpty()) return false;

	TArray<TSharedPtr<FJsonValue>> Items;
	int32 MediaCount = 0;
	for (const FAssetData& AD : Assets)
	{
		const FString ClassName = AD.AssetClassPath.GetAssetName().ToString();
		if (!ClassName.Contains(TEXT("Media")) && !ClassName.Contains(TEXT("ImgMediaSource")))
		{
			continue;
		}
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AD.GetObjectPathString());
		Row->SetStringField(TEXT("class"), ClassName);
		Items.Add(MakeShared<FJsonValueObject>(Row));
		++MediaCount;
	}

	OutStructured->SetStringField(TEXT("folder"), Folder);
	OutStructured->SetNumberField(TEXT("count"), MediaCount);
	OutStructured->SetArrayField(TEXT("items"), Items);
	OutSummary = FString::Printf(TEXT("Listed %d media assets under %s"), MediaCount, *Folder);
	return true;
}

// ── video_convert_probe ─────────────────────────────────────────────────────
// 探测磁盘视频文件，返回基本元数据（不启动解码器，仅用 FFileStatData）
// 真正的帧率/分辨率需要 FFmpeg；此处给出占位信息，便于 QA 先行通过文件存在性校验
static bool Tool_VideoProbe(
	const FSololmcpToolExecutionContext& Context,
	const TSharedRef<FJsonObject>& Arguments,
	TSharedRef<FJsonObject>& OutStructured,
	FString& OutSummary, FString& OutError)
{
	if (!EnsureGameThread(OutError)) return false;

	FString FilePath;
	if (!Arguments->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		OutError = TEXT("Missing file_path");
		return false;
	}

	IPlatformFile& FM = FPlatformFileManager::Get().GetPlatformFile();
	if (!FM.FileExists(*FilePath))
	{
		OutError = FString::Printf(TEXT("File not found: %s"), *FilePath);
		return false;
	}

	const FFileStatData Stat = FM.GetStatData(*FilePath);
	OutStructured->SetStringField(TEXT("file_path"), FilePath);
	OutStructured->SetNumberField(TEXT("size_bytes"), (double)Stat.FileSize);
	OutStructured->SetStringField(TEXT("extension"), FPaths::GetExtension(FilePath, false));
	OutStructured->SetStringField(TEXT("modified_utc"), Stat.ModificationTime.ToIso8601());
	OutSummary = FString::Printf(TEXT("Probed %s (%lld bytes, .%s)"),
		*FilePath, Stat.FileSize, *FPaths::GetExtension(FilePath, false));
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════════════════════════

void RegisterMediaIngestTools(FSololmcpToolRegistry& Registry)
{
	using SB = FSololmcpSchemaBuilder;

	Registry.Register({
		TEXT("media_source_create"),
		TEXT("Create a FileMediaSource asset pointing at a local video file."),
		SB::Object(
			{
				{TEXT("asset_path"), SB::String(TEXT("Target UE path, e.g. /Game/Media/MS_Intro"))},
				{TEXT("file_path"), SB::String(TEXT("Absolute OS path to the media file"))},
				{TEXT("precache_file"), SB::Boolean(TEXT("Precache file to memory (default false)"))}
			},
			{TEXT("asset_path"), TEXT("file_path")}),
		Tool_MediaSourceCreate
	});

	Registry.Register({
		TEXT("stream_source_create"),
		TEXT("Create a StreamMediaSource asset pointing at an HLS/RTSP/RTMP/MP4-over-HTTP URL."),
		SB::Object(
			{
				{TEXT("asset_path"), SB::String(TEXT("e.g. /Game/Media/SS_LiveCam"))},
				{TEXT("stream_url"), SB::String(TEXT("rtsp:// / rtmp:// / https://*.m3u8 / https://*.mp4"))}
			},
			{TEXT("asset_path"), TEXT("stream_url")}),
		Tool_StreamSourceCreate
	});

	Registry.Register({
		TEXT("img_media_source_create"),
		TEXT("Create an ImgMediaSource asset for an OpenEXR/PNG/JPG image sequence directory."),
		SB::Object(
			{
				{TEXT("asset_path"), SB::String(TEXT("e.g. /Game/Media/IMS_ShotA"))},
				{TEXT("sequence_path"), SB::String(TEXT("Absolute directory of sequential images"))},
				{TEXT("frame_rate"), SB::Number(TEXT("Playback FPS (default 24)"))}
			},
			{TEXT("asset_path"), TEXT("sequence_path")}),
		Tool_ImgMediaSourceCreate
	});

	Registry.Register({
		TEXT("media_player_create"),
		TEXT("Create a MediaPlayer asset (used by MediaTexture/MediaSoundComponent)."),
		SB::Object(
			{
				{TEXT("asset_path"), SB::String(TEXT("e.g. /Game/Media/MP_Intro"))},
				{TEXT("play_on_open"), SB::Boolean(TEXT("Default true"))},
				{TEXT("loop"), SB::Boolean(TEXT("Default false"))}
			},
			{TEXT("asset_path")}),
		Tool_MediaPlayerCreate
	});

	Registry.Register({
		TEXT("media_texture_create"),
		TEXT("Create a MediaTexture and optionally bind it to a MediaPlayer."),
		SB::Object(
			{
				{TEXT("asset_path"), SB::String(TEXT("e.g. /Game/Media/MT_Intro"))},
				{TEXT("media_player_path"), SB::String(TEXT("Optional /Game/Media/MP_* to bind"))}
			},
			{TEXT("asset_path")}),
		Tool_MediaTextureCreate
	});

	Registry.Register({
		TEXT("media_playlist_create"),
		TEXT("Create a MediaPlaylist asset with optional initial sources."),
		SB::Object(
			{
				{TEXT("asset_path"), SB::String(TEXT("e.g. /Game/Media/PL_Show"))},
				{TEXT("sources"), SB::Array(SB::String(TEXT("MediaSource asset path")), TEXT("Optional initial sources"))}
			},
			{TEXT("asset_path")}),
		Tool_MediaPlaylistCreate
	});

	Registry.Register({
		TEXT("media_playlist_add"),
		TEXT("Append a MediaSource to an existing MediaPlaylist."),
		SB::Object(
			{
				{TEXT("playlist_path"), SB::String()},
				{TEXT("source_path"), SB::String()}
			},
			{TEXT("playlist_path"), TEXT("source_path")}),
		Tool_MediaPlaylistAdd
	});

	Registry.Register({
		TEXT("media_player_open_source"),
		TEXT("Open a MediaSource in a MediaPlayer (optionally auto-plays via play_on_open)."),
		SB::Object(
			{
				{TEXT("player_path"), SB::String()},
				{TEXT("source_path"), SB::String()}
			},
			{TEXT("player_path"), TEXT("source_path")}),
		Tool_MediaPlayerOpenSource
	});

	Registry.Register({
		TEXT("media_player_control"),
		TEXT("Unified MediaPlayer control: action=play|pause|stop|rewind|seek (with seconds)."),
		SB::Object(
			{
				{TEXT("player_path"), SB::String()},
				{TEXT("action"), SB::String(TEXT("play|pause|stop|rewind|seek"), {TEXT("play"), TEXT("pause"), TEXT("stop"), TEXT("rewind"), TEXT("seek")})},
				{TEXT("seconds"), SB::Number(TEXT("Seek target, only used when action=seek"))}
			},
			{TEXT("player_path"), TEXT("action")}),
		Tool_MediaPlayerControl
	});

	Registry.Register({
		TEXT("media_player_play"),
		TEXT("Shorthand for media_player_control action=play."),
		SB::Object(
			{{TEXT("player_path"), SB::String()}},
			{TEXT("player_path")}),
		Tool_MediaPlayerPlay
	});

	Registry.Register({
		TEXT("media_query"),
		TEXT("Read metadata of an existing media asset (FileMediaSource / StreamMediaSource / ImgMediaSource / MediaPlayer / MediaTexture / MediaPlaylist)."),
		SB::Object(
			{{TEXT("asset_path"), SB::String()}},
			{TEXT("asset_path")}),
		Tool_MediaQuery
	});

	Registry.Register({
		TEXT("ingest_video"),
		TEXT("Copy an external video file into ProjectContent/Media/<sub_folder>/ and create a FileMediaSource pointing at it."),
		SB::Object(
			{
				{TEXT("external_path"), SB::String(TEXT("Absolute OS path to source video"))},
				{TEXT("sub_folder"), SB::String(TEXT("Optional sub-folder under /Game/Media"))},
				{TEXT("asset_name"), SB::String(TEXT("Override asset name; default MS_<base>"))},
				{TEXT("precache_file"), SB::Boolean()}
			},
			{TEXT("external_path")}),
		Tool_IngestVideo
	});

	Registry.Register({
		TEXT("ingest_image_sequence"),
		TEXT("Register an external image-sequence directory as an ImgMediaSource (no image copy by default)."),
		SB::Object(
			{
				{TEXT("external_dir"), SB::String(TEXT("Absolute OS path to image sequence directory"))},
				{TEXT("sub_folder"), SB::String()},
				{TEXT("asset_name"), SB::String(TEXT("Override; default IMS_<dirname>"))},
				{TEXT("frame_rate"), SB::Number(TEXT("Playback FPS (default 24)"))}
			},
			{TEXT("external_dir")}),
		Tool_IngestImageSequence
	});

	Registry.Register({
		TEXT("ingest_list"),
		TEXT("List existing media assets under a /Game/Media folder (recursive)."),
		SB::Object(
			{{TEXT("folder"), SB::String(TEXT("Default /Game/Media"))}}),
		Tool_IngestList
	});

	Registry.Register({
		TEXT("video_probe"),
		TEXT("Probe an OS-level video file (size / extension / mtime). Does not decode."),
		SB::Object(
			{{TEXT("file_path"), SB::String()}},
			{TEXT("file_path")}),
		Tool_VideoProbe
	});
}

} // namespace UE::SOMOLMCP
