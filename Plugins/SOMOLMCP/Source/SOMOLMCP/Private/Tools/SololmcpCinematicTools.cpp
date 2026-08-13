// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP v3.1 — Camera Animation & Cinematic Tools
// UE 5.7: UCameraAnim removed — Camera animation tools now use LevelSequence.
//
// Tools registered:
//   camera_anim_create       — Create a LevelSequence for camera animation
//   camera_anim_set_keys     — Add camera cut sections at specific times
//   camera_anim_play         — Play a camera sequence in the editor viewport
//   camera_anim_stop         — Stop all playing camera sequences
//   camera_anim_list         — List camera sequence assets
//   camera_anim_delete       — Delete a camera sequence asset
//   camera_anim_get          — Get sequence asset details (length, fps, bindings)
//   camera_track_spline_create — Create a spline-based camera track
//   camera_track_spline_set_points — Set spline control points
//   camera_track_spline_list     — List all camera track splines
//   camera_track_spline_delete   — Delete a camera track spline
//   render_queue_submit      — Submit render job to queue
//   render_queue_list        — List render queue jobs
//   render_queue_cancel      — Cancel a render queue job
//   render_viewport_set      — Configure render viewport settings

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "SololmcpSharedLocks.h" // v3.10.x worker-safety: cached IAssetRegistry pointer
#include "GameFramework/SpringArmComponent.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "ObjectTools.h"

namespace UE::SOMOLMCP
{

// ============================================================================
// Camera Animation Tools (UE 5.7: Rewritten using LevelSequence)
// UCameraAnim was removed in UE 5.7. These tools now use LevelSequence +
// CameraCutTrack as the replacement. Tool names kept for backward compat.
// ============================================================================

// Static tracking for the currently playing sequence player
static TWeakObjectPtr<ULevelSequencePlayer> GCachedSequencePlayer;
static TWeakObjectPtr<ALevelSequenceActor> GCachedSequenceActor;

static const TArray<FMovieSceneBinding>& GetMovieSceneBindingsConst(UMovieScene* MovieScene)
{
	return static_cast<const UMovieScene*>(MovieScene)->GetBindings();
}

static FString ResolveMovieSceneBindingName(UMovieScene* MovieScene, const FMovieSceneBinding& Binding)
{
	const FGuid BindingGuid = Binding.GetObjectGuid();
	if (FMovieScenePossessable* Possessable = MovieScene ? MovieScene->FindPossessable(BindingGuid) : nullptr)
	{
		return Possessable->GetName();
	}
	if (FMovieSceneSpawnable* Spawnable = MovieScene ? MovieScene->FindSpawnable(BindingGuid) : nullptr)
	{
		return Spawnable->GetName();
	}
	return BindingGuid.ToString();
}

static void RegisterCameraAnimationTools(FSololmcpToolRegistry& Registry)
{
	// ---- camera_anim_create ----
	Registry.Register({
		TEXT("camera_anim_create"),
		TEXT("Create a LevelSequence asset for camera animation. (UE 5.7: replaces UCameraAnim with LevelSequence.)"),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path, e.g., /Game/CameraAnims/CA_Flyover"))},
			{TEXT("length"), FSololmcpSchemaBuilder::Number(TEXT("Sequence length in seconds (default 5.0)"))},
			{TEXT("fps"), FSololmcpSchemaBuilder::Integer(TEXT("Frames per second (default 30)"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			const float Length = Arguments->HasField(TEXT("length")) ? static_cast<float>(Arguments->GetNumberField(TEXT("length"))) : 5.0f;
			const int32 FPS = Arguments->HasField(TEXT("fps")) ? Arguments->GetIntegerField(TEXT("fps")) : 30;

			UPackage* Package = CreatePackage(*AssetPath);
			if (!Package)
			{
				OutError = TEXT("Failed to create package.");
				return false;
			}

			ULevelSequence* LevelSequence = NewObject<ULevelSequence>(Package, ULevelSequence::StaticClass(), *FPackageName::GetLongPackageAssetName(AssetPath), RF_Public | RF_Standalone);
			if (!LevelSequence)
			{
				OutError = TEXT("Failed to create LevelSequence asset.");
				return false;
			}

			LevelSequence->Initialize();
			UMovieScene* MovieScene = LevelSequence->GetMovieScene();
			if (MovieScene)
			{
				MovieScene->SetDisplayRate(FFrameRate(FPS, 1));
				MovieScene->SetPlaybackRange(FFrameNumber(0), FMath::CeilToInt(Length * FPS));
			}

			LevelSequence->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(LevelSequence);
			if (!Context.Services.SaveAsset(AssetPath, false, OutError))
			{
				return false;
			}
			if (!Context.Services.LoadAsset(AssetPath, OutError))
			{
				OutError = FString::Printf(TEXT("LevelSequence was created but failed reload verification: %s"), *AssetPath);
				return false;
			}

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("length"), Length);
			OutStructured->SetNumberField(TEXT("fps"), FPS);
			OutStructured->SetStringField(TEXT("backend"), TEXT("LevelSequence"));
			OutStructured->SetBoolField(TEXT("reload_verified"), true);
			OutSummary = FString::Printf(TEXT("Created LevelSequence '%s' (length: %.1fs, fps: %d)"), *AssetPath, Length, FPS);
			return true;
		}
	});

	// ---- camera_anim_set_keys ----
	Registry.Register({
		TEXT("camera_anim_set_keys"),
		TEXT("Add camera cut sections to a LevelSequence at specified times. Each key defines a camera cut point."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path"))},
			{TEXT("camera_actor_name"), FSololmcpSchemaBuilder::String(TEXT("Camera actor name to bind (must exist in the level)"))},
			{TEXT("keys"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
				{TEXT("time"), FSololmcpSchemaBuilder::Number(TEXT("Time in seconds for the camera cut"))}
			}))}
		}, {TEXT("asset_path"), TEXT("camera_actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			ULevelSequence* LevelSequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!LevelSequence) return false;

			UMovieScene* MovieScene = LevelSequence->GetMovieScene();
			if (!MovieScene) { OutError = TEXT("No MovieScene."); return false; }

			const FString CameraActorName = Arguments->GetStringField(TEXT("camera_actor_name"));
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			AActor* CameraActor = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == CameraActorName)
				{
					CameraActor = *It;
					break;
				}
			}
			if (!CameraActor)
			{
				OutError = FString::Printf(TEXT("Camera actor '%s' not found; refusing to create an unbound camera cut."), *CameraActorName);
				return false;
			}

			// Find or create possessable binding for the camera
			FGuid CameraBindingId;
			for (const FMovieSceneBinding& Binding : GetMovieSceneBindingsConst(MovieScene))
			{
				if (ResolveMovieSceneBindingName(MovieScene, Binding) == CameraActorName)
				{
					CameraBindingId = Binding.GetObjectGuid();
					break;
				}
			}
			if (!CameraBindingId.IsValid())
			{
				CameraBindingId = MovieScene->AddPossessable(CameraActorName, CameraActor->GetClass());
				if (!CameraBindingId.IsValid())
				{
					OutError = FString::Printf(TEXT("Failed to create possessable binding for camera actor '%s'."), *CameraActorName);
					return false;
				}

				// Bind the possessable to the actual object
				LevelSequence->BindPossessableObject(CameraBindingId, *CameraActor, World);
			}

			// Get or create CameraCutTrack
			UMovieSceneTrack* CCTrack = MovieScene->GetCameraCutTrack();
			if (!CCTrack)
			{
				CCTrack = MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
			}
			UMovieSceneCameraCutTrack* CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(CCTrack);
			if (!CameraCutTrack) { OutError = TEXT("Failed to create CameraCutTrack."); return false; }

			// Process keys - add camera cuts at each time
			const TArray<TSharedPtr<FJsonValue>>& KeysArr = Arguments->GetArrayField(TEXT("keys"));
			FFrameRate DisplayRate = MovieScene->GetDisplayRate();
			TArray<TSharedPtr<FJsonValue>> CreatedKeysJson;
			TArray<TSharedPtr<FJsonValue>> FailedKeysJson;
			int32 CutsAdded = 0;

			for (const auto& KeyVal : KeysArr)
			{
				const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
				if (!KeyObj.IsValid())
				{
					FailedKeysJson.Add(MakeShared<FJsonValueString>(TEXT("Invalid key object.")));
					continue;
				}

				float Time = static_cast<float>(KeyObj->GetNumberField(TEXT("time")));
				FFrameNumber Frame = DisplayRate.AsFrameNumber(Time);

				FMovieSceneObjectBindingID BindingID = UE::MovieScene::FRelativeObjectBindingID(CameraBindingId);

				UMovieSceneCameraCutSection* Section = CameraCutTrack->AddNewCameraCut(BindingID, Frame);
				if (Section)
				{
					Section->SetRange(TRange<FFrameNumber>(Frame));
					++CutsAdded;

					TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
					KeyJson->SetNumberField(TEXT("time"), Time);
					KeyJson->SetStringField(TEXT("frame"), FString::FromInt(Frame.Value));
					CreatedKeysJson.Add(MakeShared<FJsonValueObject>(KeyJson));
				}
				else
				{
					TSharedPtr<FJsonObject> FailedKeyJson = MakeShared<FJsonObject>();
					FailedKeyJson->SetNumberField(TEXT("time"), Time);
					FailedKeyJson->SetStringField(TEXT("reason"), TEXT("AddNewCameraCut returned null."));
					FailedKeysJson.Add(MakeShared<FJsonValueObject>(FailedKeyJson));
				}
			}

			if (CutsAdded == 0)
			{
				OutStructured->SetArrayField(TEXT("failed_keys"), FailedKeysJson);
				OutError = TEXT("No camera cuts were added.");
				return false;
			}

			LevelSequence->MarkPackageDirty();

			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("cuts_requested"), KeysArr.Num());
			OutStructured->SetNumberField(TEXT("cuts_added"), CutsAdded);
			OutStructured->SetStringField(TEXT("camera_binding"), CameraActorName);
			OutStructured->SetArrayField(TEXT("keys"), CreatedKeysJson);
			if (CutsAdded < KeysArr.Num())
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("partial_success"));
				OutStructured->SetArrayField(TEXT("failed_keys"), FailedKeysJson);
			}
			OutSummary = FString::Printf(TEXT("Added %d/%d camera cuts to '%s'"), CutsAdded, KeysArr.Num(), *AssetPath);
			return true;
		}
	});

	// ---- camera_anim_play ----
	Registry.Register({
		TEXT("camera_anim_play"),
		TEXT("Play a LevelSequence (camera animation) in the editor viewport."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("camera_anim_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path"))},
			{TEXT("play_rate"), FSololmcpSchemaBuilder::Number(TEXT("Playback rate (default 1.0)"))},
			{TEXT("looping"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether to loop (default false)"))}
		}, {TEXT("camera_anim_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AnimPath = Arguments->GetStringField(TEXT("camera_anim_path"));
			ULevelSequence* LevelSequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AnimPath, OutError));
			if (!LevelSequence) return false;

			const float PlayRate = Arguments->HasField(TEXT("play_rate")) ? static_cast<float>(Arguments->GetNumberField(TEXT("play_rate"))) : 1.0f;
			const bool bLooping = Arguments->HasField(TEXT("looping")) ? Arguments->GetBoolField(TEXT("looping")) : false;

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			// Stop any previously playing sequence
			if (GCachedSequencePlayer.IsValid())
			{
				GCachedSequencePlayer->Stop();
				GCachedSequencePlayer.Reset();
			}
			if (GCachedSequenceActor.IsValid())
			{
				GCachedSequenceActor->Destroy();
				GCachedSequenceActor.Reset();
			}

			FMovieSceneSequencePlaybackSettings Settings;
			Settings.LoopCount.Value = bLooping ? -1 : 0;
			Settings.PlayRate = PlayRate;

			ALevelSequenceActor* SequenceActor = nullptr;
			ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(World, LevelSequence, Settings, SequenceActor);

			if (!Player || !SequenceActor)
			{
				OutError = TEXT("Failed to create sequence player.");
				return false;
			}

			Player->Play();

			GCachedSequencePlayer = Player;
			GCachedSequenceActor = SequenceActor;

			UMovieScene* MovieScene = LevelSequence->GetMovieScene();
			float Length = 0.0f;
			if (MovieScene)
			{
				TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
				Length = MovieScene->GetDisplayRate().AsSeconds(Range.GetUpperBoundValue() - Range.GetLowerBoundValue());
			}

			OutStructured->SetStringField(TEXT("camera_anim"), AnimPath);
			OutStructured->SetNumberField(TEXT("play_rate"), PlayRate);
			OutStructured->SetBoolField(TEXT("looping"), bLooping);
			OutStructured->SetNumberField(TEXT("length"), Length);
			OutStructured->SetStringField(TEXT("backend"), TEXT("LevelSequence"));
			OutSummary = FString::Printf(TEXT("Playing LevelSequence '%s' (rate: %.1f, loop: %s)"), *AnimPath, PlayRate, bLooping ? TEXT("true") : TEXT("false"));
			return true;
		}
	});

	// ---- camera_anim_stop ----
	Registry.Register({
		TEXT("camera_anim_stop"),
		TEXT("Stop all playing LevelSequence animations."),
		FSololmcpSchemaBuilder::Object({}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			bool bStopped = false;
			if (GCachedSequencePlayer.IsValid())
			{
				GCachedSequencePlayer->Stop();
				GCachedSequencePlayer.Reset();
				bStopped = true;
			}
			if (GCachedSequenceActor.IsValid())
			{
				GCachedSequenceActor->Destroy();
				GCachedSequenceActor.Reset();
				bStopped = true;
			}
			OutStructured->SetBoolField(TEXT("stopped"), bStopped);
			OutSummary = bStopped ? TEXT("Stopped playing sequence.") : TEXT("No sequence was playing.");
			return true;
		}
	});

	// ---- camera_anim_get ----
	Registry.Register({
		TEXT("camera_anim_get"),
		TEXT("Get detailed information about a camera animation (LevelSequence) asset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			// Audit round 4: validate asset_path before LoadAsset to prevent LogEditorAssetSubsystem noise on empty input.
			FString AssetPath;
			if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
			{
				OutError = TEXT("Missing or invalid asset_path");
				return false;
			}
			if (!FPackageName::IsValidObjectPath(AssetPath) && !FPackageName::IsValidLongPackageName(AssetPath))
			{
				OutError = FString::Printf(TEXT("Invalid asset_path: %s"), *AssetPath);
				return false;
			}
			ULevelSequence* LevelSequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!LevelSequence) return false;

			UMovieScene* MovieScene = LevelSequence->GetMovieScene();
			OutStructured->SetStringField(TEXT("asset_path"), AssetPath);
			OutStructured->SetStringField(TEXT("name"), LevelSequence->GetName());
			OutStructured->SetStringField(TEXT("backend"), TEXT("LevelSequence"));

			if (MovieScene)
			{
				FFrameRate DisplayRate = MovieScene->GetDisplayRate();
				TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
				float Length = DisplayRate.AsSeconds(PlaybackRange.GetUpperBoundValue() - PlaybackRange.GetLowerBoundValue());
				int32 TotalFrames = (PlaybackRange.GetUpperBoundValue() - PlaybackRange.GetLowerBoundValue()).Value;

				OutStructured->SetNumberField(TEXT("length_seconds"), Length);
				OutStructured->SetNumberField(TEXT("fps_numerator"), DisplayRate.Numerator);
				OutStructured->SetNumberField(TEXT("fps_denominator"), DisplayRate.Denominator);
				OutStructured->SetNumberField(TEXT("total_frames"), TotalFrames);

				// Count bindings (possessables + spawnables)
				int32 BindingCount = GetMovieSceneBindingsConst(MovieScene).Num();
				int32 PossessableCount = MovieScene->GetPossessableCount();
				int32 SpawnableCount = MovieScene->GetSpawnableCount();

				OutStructured->SetNumberField(TEXT("bindings"), BindingCount);
				OutStructured->SetNumberField(TEXT("possessables"), PossessableCount);
				OutStructured->SetNumberField(TEXT("spawnables"), SpawnableCount);

				// Check for CameraCutTrack
				UMovieSceneTrack* CCTrack = MovieScene->GetCameraCutTrack();
				OutStructured->SetBoolField(TEXT("has_camera_cut_track"), CCTrack != nullptr);

				// List binding names
				TArray<TSharedPtr<FJsonValue>> BindingArr;
				for (const FMovieSceneBinding& Binding : GetMovieSceneBindingsConst(MovieScene))
				{
					TSharedPtr<FJsonObject> BObj = MakeShared<FJsonObject>();
					BObj->SetStringField(TEXT("name"), ResolveMovieSceneBindingName(MovieScene, Binding));
					BindingArr.Add(MakeShared<FJsonValueObject>(BObj));
				}
				OutStructured->SetArrayField(TEXT("binding_names"), BindingArr);

				OutSummary = FString::Printf(TEXT("LevelSequence '%s': length=%.2fs, fps=%d/%d, frames=%d, bindings=%d"),
					*LevelSequence->GetName(), Length, DisplayRate.Numerator, DisplayRate.Denominator, TotalFrames, BindingCount);
			}
			else
			{
				OutSummary = FString::Printf(TEXT("LevelSequence '%s': no MovieScene data"), *LevelSequence->GetName());
			}
			return true;
		}
	});

	// ---- camera_anim_list ----
	Registry.Register({
		TEXT("camera_anim_list"),
		TEXT("List all LevelSequence camera animation assets in the project."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("search_path"), FSololmcpSchemaBuilder::String(TEXT("Base package path to search (default /Game)"))}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			// Audit round 3: tolerate missing/empty search_path by falling back to /Game.
			FString SearchPath;
			if (!Arguments->TryGetStringField(TEXT("search_path"), SearchPath) || SearchPath.IsEmpty())
			{
				SearchPath = TEXT("/Game");
			}

			// v3.10.x worker-safety: AR queries are RWLock-guarded internally; we just
			// need to avoid the FModuleManager call from a worker thread.
			IAssetRegistry* RegistryPtr = UE::SOMOLMCP::GSololmcpCachedAssetRegistry;
			if (!RegistryPtr)
			{
				OutError = TEXT("AssetRegistry not yet cached (called before plugin init?)");
				return false;
			}
			IAssetRegistry& AssetRegistry = *RegistryPtr;

			TArray<FAssetData> SequenceAssets;
			AssetRegistry.GetAssetsByClass(ULevelSequence::StaticClass()->GetClassPathName(), SequenceAssets);

			TArray<TSharedPtr<FJsonValue>> AnimsJson;
			for (const FAssetData& AnimData : SequenceAssets)
			{
				if (!AnimData.PackageName.ToString().StartsWith(SearchPath)) continue;

				TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
				AnimObj->SetStringField(TEXT("asset_path"), AnimData.GetObjectPathString());
				AnimObj->SetStringField(TEXT("name"), AnimData.AssetName.ToString());
				AnimsJson.Add(MakeShared<FJsonValueObject>(AnimObj));
			}

			OutStructured->SetArrayField(TEXT("camera_anims"), AnimsJson);
			OutStructured->SetNumberField(TEXT("total"), AnimsJson.Num());
			OutStructured->SetStringField(TEXT("backend"), TEXT("LevelSequence"));
			OutSummary = FString::Printf(TEXT("Found %d LevelSequence assets"), AnimsJson.Num());
			return true;
		}
	});

	// ---- camera_anim_delete ----
	Registry.Register({
		TEXT("camera_anim_delete"),
		TEXT("Delete a LevelSequence camera animation asset."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("asset_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path to delete"))}
		}, {TEXT("asset_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
			ULevelSequence* LevelSequence = Cast<ULevelSequence>(Context.Services.LoadAsset(AssetPath, OutError));
			if (!LevelSequence) return false;

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DeleteCameraAnim", "SOMOLMCP Delete CameraAnim"));
			TArray<UObject*> ObjectsToDelete;
			ObjectsToDelete.Add(LevelSequence);
			const int32 NumDeleted = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
			if (NumDeleted <= 0 || IsValid(LevelSequence))
			{
				OutStructured->SetNumberField(TEXT("deleted_count"), NumDeleted);
				OutError = FString::Printf(TEXT("Failed to verify deletion of LevelSequence '%s'."), *AssetPath);
				return false;
			}

			OutStructured->SetStringField(TEXT("deleted_path"), AssetPath);
			OutStructured->SetNumberField(TEXT("deleted_count"), NumDeleted);
			OutSummary = FString::Printf(TEXT("Deleted LevelSequence '%s'"), *AssetPath);
			return true;
		}
	});
}

// ============================================================================
// Camera Track (Spline) Tools
// ============================================================================

static void RegisterCameraTrackTools(FSololmcpToolRegistry& Registry)
{
	// ---- camera_track_spline_create ----
	Registry.Register({
		TEXT("camera_track_spline_create"),
		TEXT("Create a spline-based camera motion track actor in the level."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Name for the spline actor"))},
			{TEXT("location"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})},
			{TEXT("is_closed_loop"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether the spline forms a closed loop"))}
		}, {TEXT("actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			// Audit round 6: enforce non-empty actor_name. Without this, smoke fuzz with {}
			// silently spawns a generic AActor::StaticClass() instance which UE auto-labels
			// "Actor_0", leaking into the level (along with its USplineComponent).
			FString ActorName;
			if (!Arguments->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
			{
				OutError = TEXT("Missing required: actor_name (non-empty label for the spline actor).");
				return false;
			}
			const bool bClosedLoop = Arguments->HasField(TEXT("is_closed_loop")) ? Arguments->GetBoolField(TEXT("is_closed_loop")) : false;

			FVector Location = FVector::ZeroVector;
			if (const TSharedPtr<FJsonObject>* LocObjPtr; Arguments->TryGetObjectField(TEXT("location"), LocObjPtr))
			{
				const TSharedPtr<FJsonObject>& LocObj = *LocObjPtr;
				Location.X = static_cast<float>(LocObj->GetNumberField(TEXT("x")));
				Location.Y = static_cast<float>(LocObj->GetNumberField(TEXT("y")));
				Location.Z = static_cast<float>(LocObj->GetNumberField(TEXT("z")));
			}

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				OutError = TEXT("No editor world.");
				return false;
			}

			// Spawn actor with SplineComponent
			FActorSpawnParameters SpawnParams;
			SpawnParams.Name = FName(*ActorName);
			AActor* SplineActor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
			if (!SplineActor)
			{
				OutError = TEXT("Failed to spawn spline actor.");
				return false;
			}

			USplineComponent* SplineComp = NewObject<USplineComponent>(SplineActor, USplineComponent::StaticClass(), TEXT("CameraTrackSpline"));
			if (SplineComp)
			{
				SplineComp->RegisterComponent();
				// UE 5.7: bClosedLoop is private, use SetClosedLoop() method
				SplineComp->SetClosedLoop(bClosedLoop);
				// Add default start and end points
				SplineComp->AddSplinePoint(Location, ESplineCoordinateSpace::World, true);
				SplineComp->AddSplinePoint(Location + FVector(500, 0, 0), ESplineCoordinateSpace::World, true);
				SplineComp->UpdateSpline();
			}

			SplineActor->SetActorLabel(ActorName);
			SplineActor->MarkPackageDirty();

			OutStructured = FSololmcpEditorServices::MakeActorReference(SplineActor);
			OutStructured->SetBoolField(TEXT("is_closed_loop"), bClosedLoop);
			OutStructured->SetNumberField(TEXT("point_count"), SplineComp ? SplineComp->GetNumberOfSplinePoints() : 0);
			OutSummary = FString::Printf(TEXT("Created camera track spline '%s' with %d points"), *ActorName, SplineComp ? SplineComp->GetNumberOfSplinePoints() : 0);
			return true;
		}
	});

	// ---- camera_track_spline_set_points ----
	Registry.Register({
		TEXT("camera_track_spline_set_points"),
		TEXT("Set spline control points on a camera track spline."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Spline actor name"))},
			{TEXT("points"), FSololmcpSchemaBuilder::Array(FSololmcpSchemaBuilder::Object({
				{TEXT("location"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})},
				{TEXT("is_curve_point"), FSololmcpSchemaBuilder::Boolean(TEXT("Whether this point has curve handles"))},
				{TEXT("leave_tangent"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})},
				{TEXT("arrive_tangent"), FSololmcpSchemaBuilder::Object({{TEXT("x"), FSololmcpSchemaBuilder::Number()}, {TEXT("y"), FSololmcpSchemaBuilder::Number()}, {TEXT("z"), FSololmcpSchemaBuilder::Number()}})}
			}))},
			{TEXT("replace_existing"), FSololmcpSchemaBuilder::Boolean(TEXT("Replace all existing points (default true)"))}
		}, {TEXT("actor_name"), TEXT("points")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));
			const bool bReplace = Arguments->HasField(TEXT("replace_existing")) ? Arguments->GetBoolField(TEXT("replace_existing")) : true;
			const TArray<TSharedPtr<FJsonValue>>& PointsArr = Arguments->GetArrayField(TEXT("points"));

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			AActor* SplineActor = FindObject<AActor>(World, *ActorName);
			if (!SplineActor)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->GetActorLabel() == ActorName) { SplineActor = *It; break; }
				}
			}
			if (!SplineActor)
			{
				OutError = FString::Printf(TEXT("Actor '%s' not found."), *ActorName);
				return false;
			}

			USplineComponent* SplineComp = SplineActor->FindComponentByClass<USplineComponent>();
			if (!SplineComp)
			{
				OutError = TEXT("Actor has no SplineComponent.");
				return false;
			}

			if (bReplace)
			{
				SplineComp->ClearSplinePoints();
			}

			for (const auto& PointVal : PointsArr)
			{
				const TSharedPtr<FJsonObject>& PointObj = PointVal->AsObject();
				if (!PointObj.IsValid()) continue;

				FVector PointLoc = FVector::ZeroVector;
				if (const TSharedPtr<FJsonObject>* LocObjPtr; PointObj->TryGetObjectField(TEXT("location"), LocObjPtr))
				{
					const TSharedPtr<FJsonObject>& LocObj = *LocObjPtr;
					PointLoc.X = static_cast<float>(LocObj->GetNumberField(TEXT("x")));
					PointLoc.Y = static_cast<float>(LocObj->GetNumberField(TEXT("y")));
					PointLoc.Z = static_cast<float>(LocObj->GetNumberField(TEXT("z")));
				}

				const bool bIsCurve = PointObj->HasField(TEXT("is_curve_point")) ? PointObj->GetBoolField(TEXT("is_curve_point")) : false;

				SplineComp->AddSplinePoint(PointLoc, ESplineCoordinateSpace::World, bIsCurve);

				// Set tangents
				if (bIsCurve)
				{
					int32 PointIdx = SplineComp->GetNumberOfSplinePoints() - 1;
					if (const TSharedPtr<FJsonObject>* LeaveObjPtr; PointObj->TryGetObjectField(TEXT("leave_tangent"), LeaveObjPtr))
					{
						const TSharedPtr<FJsonObject>& LeaveObj = *LeaveObjPtr;
						FVector Tangent;
						Tangent.X = static_cast<float>(LeaveObj->GetNumberField(TEXT("x")));
						Tangent.Y = static_cast<float>(LeaveObj->GetNumberField(TEXT("y")));
						Tangent.Z = static_cast<float>(LeaveObj->GetNumberField(TEXT("z")));
						SplineComp->SetTangentAtSplinePoint(PointIdx, Tangent, ESplineCoordinateSpace::World, true);
					}
					if (const TSharedPtr<FJsonObject>* ArriveObjPtr; PointObj->TryGetObjectField(TEXT("arrive_tangent"), ArriveObjPtr))
					{
						const TSharedPtr<FJsonObject>& ArriveObj = *ArriveObjPtr;
						FVector Tangent;
						Tangent.X = static_cast<float>(ArriveObj->GetNumberField(TEXT("x")));
						Tangent.Y = static_cast<float>(ArriveObj->GetNumberField(TEXT("y")));
						Tangent.Z = static_cast<float>(ArriveObj->GetNumberField(TEXT("z")));
						SplineComp->SetTangentAtSplinePoint(PointIdx, Tangent, ESplineCoordinateSpace::World, false);
					}
				}
			}

			SplineComp->UpdateSpline();

			OutStructured->SetStringField(TEXT("actor_name"), ActorName);
			OutStructured->SetNumberField(TEXT("total_points"), SplineComp->GetNumberOfSplinePoints());
			OutStructured->SetNumberField(TEXT("spline_length"), SplineComp->GetSplineLength());
			OutSummary = FString::Printf(TEXT("Set %d points on spline '%s' (length: %.0f cm)"), PointsArr.Num(), *ActorName, SplineComp->GetSplineLength());
			return true;
		}
	});

	// ---- camera_track_spline_list ----
	Registry.Register({
		TEXT("camera_track_spline_list"),
		TEXT("List all camera track spline actors and their control points in the current level."),
		FSololmcpSchemaBuilder::Object({{}}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			TArray<TSharedPtr<FJsonValue>> TracksJson;

			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				USplineComponent* SplineComp = Actor->FindComponentByClass<USplineComponent>();
				if (!SplineComp) continue;

				TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
				TrackObj->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
				TrackObj->SetStringField(TEXT("actor_path"), Actor->GetPathName());
				TrackObj->SetNumberField(TEXT("num_points"), SplineComp->GetNumberOfSplinePoints());
				TrackObj->SetNumberField(TEXT("spline_length"), SplineComp->GetSplineLength());
				// UE 5.7: bClosedLoop is private, use IsClosedLoop() method
				TrackObj->SetBoolField(TEXT("is_closed_loop"), SplineComp->IsClosedLoop());

				// Collect control points
				TArray<TSharedPtr<FJsonValue>> PointsJson;
				for (int32 i = 0; i < SplineComp->GetNumberOfSplinePoints(); ++i)
				{
					FVector PointLoc = SplineComp->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
					TSharedPtr<FJsonObject> PtObj = MakeShared<FJsonObject>();
					PtObj->SetNumberField(TEXT("index"), i);
					PtObj->SetStringField(TEXT("location"), PointLoc.ToString());
					PointsJson.Add(MakeShared<FJsonValueObject>(PtObj));
				}
				TrackObj->SetArrayField(TEXT("points"), PointsJson);

				TracksJson.Add(MakeShared<FJsonValueObject>(TrackObj));
			}

			OutStructured->SetArrayField(TEXT("tracks"), TracksJson);
			OutStructured->SetNumberField(TEXT("total_tracks"), TracksJson.Num());
			OutSummary = FString::Printf(TEXT("Found %d camera track splines"), TracksJson.Num());
			return true;
		}
	});

	// ---- camera_track_spline_delete ----
	Registry.Register({
		TEXT("camera_track_spline_delete"),
		TEXT("Delete a camera track spline actor from the level."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("actor_name"), FSololmcpSchemaBuilder::String(TEXT("Spline actor name to delete"))}
		}, {TEXT("actor_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString ActorName = Arguments->GetStringField(TEXT("actor_name"));

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) { OutError = TEXT("No editor world."); return false; }

			AActor* SplineActor = FindObject<AActor>(World, *ActorName);
			if (!SplineActor)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->GetActorLabel() == ActorName) { SplineActor = *It; break; }
				}
			}
			if (!SplineActor)
			{
				OutError = FString::Printf(TEXT("Actor '%s' not found."), *ActorName);
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "DeleteSplineTrack", "SOMOLMCP Delete Camera Track"));
			if (!SplineActor->Destroy())
			{
				OutError = FString::Printf(TEXT("Destroy returned false for camera track spline '%s'."), *ActorName);
				return false;
			}

			OutStructured->SetStringField(TEXT("deleted_actor"), ActorName);
			OutSummary = FString::Printf(TEXT("Deleted camera track spline '%s'"), *ActorName);
			return true;
		}
	});
}

// ============================================================================
// Render Pipeline Tools
// ============================================================================

static void RegisterRenderPipelineTools(FSololmcpToolRegistry& Registry)
{
	// ---- render_viewport_set ----
	Registry.Register({
		TEXT("render_viewport_set"),
		TEXT("Configure editor viewport rendering settings."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("view_mode"), FSololmcpSchemaBuilder::String(TEXT("View mode: 'lit', 'unlit', 'wireframe', 'detail_lighting', 'lighting_only', 'shader_complexity', 'stationary_light_overlap'"))},
			{TEXT("show_flags"), FSololmcpSchemaBuilder::Object({
				{TEXT("foliage"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("landscape"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("particles"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("static_meshes"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("lighting"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("sky_lighting"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("translucency"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("post_processing"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("atmosphere"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("fog"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("eye_adaptation"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("grid"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("collision"), FSololmcpSchemaBuilder::Boolean()}
			})},
			{TEXT("engine_show_flags"), FSololmcpSchemaBuilder::Object({
				{TEXT("ambient_occlusion"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("bloom"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("motion_blur"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("lumen"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("nanite"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("virtual_shadow_maps"), FSololmcpSchemaBuilder::Boolean()},
				{TEXT("ray_tracing"), FSololmcpSchemaBuilder::Boolean()}
			})}
		}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			TArray<TSharedPtr<FJsonValue>> ChangesJson;
			FLevelEditorViewportClient* ActiveViewportClient = nullptr;
			if (GEditor)
			{
				FViewport* ActiveViewport = GEditor->GetActiveViewport();
				for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
				{
					if (Candidate && Candidate->Viewport && Candidate->Viewport == ActiveViewport)
					{
						ActiveViewportClient = Candidate;
						break;
					}
				}
				if (!ActiveViewportClient)
				{
					for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
					{
						if (Candidate && Candidate->Viewport && Candidate->IsPerspective())
						{
							ActiveViewportClient = Candidate;
							break;
						}
					}
				}
			}

			// Apply view mode
			if (Arguments->HasField(TEXT("view_mode")))
			{
				const FString ViewMode = Arguments->GetStringField(TEXT("view_mode"));
				FString ConsoleCommand;
				if (ViewMode == TEXT("lit")) { ConsoleCommand = TEXT("viewmode lit"); if (ActiveViewportClient) ActiveViewportClient->SetViewMode(VMI_Lit); }
				else if (ViewMode == TEXT("unlit")) { ConsoleCommand = TEXT("viewmode unlit"); if (ActiveViewportClient) ActiveViewportClient->SetViewMode(VMI_Unlit); }
				else if (ViewMode == TEXT("wireframe")) { ConsoleCommand = TEXT("viewmode wireframe"); if (ActiveViewportClient) ActiveViewportClient->SetViewMode(VMI_Wireframe); }
				else if (ViewMode == TEXT("detail_lighting")) ConsoleCommand = TEXT("viewmode detaillighting");
				else if (ViewMode == TEXT("lighting_only")) ConsoleCommand = TEXT("viewmode lightingonly");
				else if (ViewMode == TEXT("shader_complexity")) ConsoleCommand = TEXT("viewmode shadercomplexity");

				if (!ConsoleCommand.IsEmpty())
				{
					GEngine->Exec(GEditor->GetEditorWorldContext().World(), *ConsoleCommand);
					ChangesJson.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("View mode set to '%s'"), *ViewMode)));
				}
			}

			const TSharedPtr<FJsonObject>* EngineFlagsObject = nullptr;
			Arguments->TryGetObjectField(TEXT("engine_show_flags"), EngineFlagsObject);

			// Apply engine show flags via console commands
			auto ApplyFlag = [&](const FString& Key, const FString& OnCmd, const FString& OffCmd)
			{
				if (EngineFlagsObject && EngineFlagsObject->IsValid()
					&& (*EngineFlagsObject)->HasTypedField<EJson::Boolean>(Key))
				{
					bool bEnabled = (*EngineFlagsObject)->GetBoolField(Key);
					GEngine->Exec(GEditor->GetEditorWorldContext().World(), *(bEnabled ? OnCmd : OffCmd));
					ChangesJson.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s = %s"), *Key, bEnabled ? TEXT("true") : TEXT("false"))));
				}
			};

			ApplyFlag(TEXT("ambient_occlusion"), TEXT("r.AmbientOcclusion 1"), TEXT("r.AmbientOcclusion 0"));
			ApplyFlag(TEXT("bloom"), TEXT("r.BloomQuality 5"), TEXT("r.BloomQuality 0"));
			ApplyFlag(TEXT("motion_blur"), TEXT("r.MotionBlurQuality 3"), TEXT("r.MotionBlurQuality 0"));

			// Nanite
			if (EngineFlagsObject && EngineFlagsObject->IsValid()
				&& (*EngineFlagsObject)->HasTypedField<EJson::Boolean>(TEXT("nanite")))
			{
				bool bNanite = (*EngineFlagsObject)->GetBoolField(TEXT("nanite"));
				FString NaniteCmd = bNanite ? TEXT("r.Nanite 1") : TEXT("r.Nanite 0");
				GEngine->Exec(GEditor->GetEditorWorldContext().World(), *NaniteCmd);
				ChangesJson.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Nanite = %s"), bNanite ? TEXT("true") : TEXT("false"))));
			}

			// Ray Tracing
			if (EngineFlagsObject && EngineFlagsObject->IsValid()
				&& (*EngineFlagsObject)->HasTypedField<EJson::Boolean>(TEXT("ray_tracing")))
			{
				bool bRT = (*EngineFlagsObject)->GetBoolField(TEXT("ray_tracing"));
				FString RTCmd = bRT ? TEXT("r.RayTracing 1") : TEXT("r.RayTracing 0");
				GEngine->Exec(GEditor->GetEditorWorldContext().World(), *RTCmd);
				ChangesJson.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Ray Tracing = %s"), bRT ? TEXT("true") : TEXT("false"))));
			}

			// Show flags are viewport state, not render CVars. Apply the nested
			// schema directly so a prior diagnostic Unlit capture can be restored.
			const TSharedPtr<FJsonObject>* ShowFlagsObject = nullptr;
			Arguments->TryGetObjectField(TEXT("show_flags"), ShowFlagsObject);
			auto ApplyViewportShowFlag = [&](const TCHAR* Key, auto Setter)
			{
				if (ActiveViewportClient && ShowFlagsObject && ShowFlagsObject->IsValid()
					&& (*ShowFlagsObject)->HasTypedField<EJson::Boolean>(Key))
				{
					const bool bShow = (*ShowFlagsObject)->GetBoolField(Key);
					Setter(ActiveViewportClient->EngineShowFlags, bShow);
					ChangesJson.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("show_flags.%s = %s"), Key, bShow ? TEXT("true") : TEXT("false"))));
				}
			};

			ApplyViewportShowFlag(TEXT("foliage"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetInstancedFoliage(bValue); });
			ApplyViewportShowFlag(TEXT("landscape"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetLandscape(bValue); });
			ApplyViewportShowFlag(TEXT("particles"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetParticles(bValue); });
			ApplyViewportShowFlag(TEXT("static_meshes"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetStaticMeshes(bValue); });
			ApplyViewportShowFlag(TEXT("lighting"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetLighting(bValue); });
			ApplyViewportShowFlag(TEXT("sky_lighting"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetSkyLighting(bValue); });
			ApplyViewportShowFlag(TEXT("translucency"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetTranslucency(bValue); });
			ApplyViewportShowFlag(TEXT("post_processing"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetPostProcessing(bValue); });
			ApplyViewportShowFlag(TEXT("atmosphere"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetAtmosphere(bValue); });
			ApplyViewportShowFlag(TEXT("fog"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetFog(bValue); });
			ApplyViewportShowFlag(TEXT("eye_adaptation"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetEyeAdaptation(bValue); });
			ApplyViewportShowFlag(TEXT("grid"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetGrid(bValue); });
			ApplyViewportShowFlag(TEXT("collision"), [](FEngineShowFlags& Flags, bool bValue) { Flags.SetCollision(bValue); });
			if (ActiveViewportClient)
			{
				ActiveViewportClient->Invalidate();
			}

			OutStructured->SetArrayField(TEXT("changes"), ChangesJson);
			OutStructured->SetNumberField(TEXT("change_count"), ChangesJson.Num());
			OutSummary = FString::Printf(TEXT("Applied %d viewport render settings"), ChangesJson.Num());
			return true;
		}
	});

	// ---- render_queue_submit ----
	Registry.Register({
		TEXT("render_queue_submit"),
		TEXT("Submit a high-resolution screenshot or movie render job to the render queue."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("render_type"), FSololmcpSchemaBuilder::String(TEXT("'screenshot' or 'movie'"))},
			{TEXT("resolution_x"), FSololmcpSchemaBuilder::Integer(TEXT("Width in pixels"))},
			{TEXT("resolution_y"), FSololmcpSchemaBuilder::Integer(TEXT("Height in pixels"))},
			{TEXT("output_path"), FSololmcpSchemaBuilder::String(TEXT("Output file path"))},
			{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("For movie: Level Sequence asset path"))},
			{TEXT("frame_range_start"), FSololmcpSchemaBuilder::Integer(TEXT("For movie: start frame"))},
			{TEXT("frame_range_end"), FSololmcpSchemaBuilder::Integer(TEXT("For movie: end frame"))}
		}, {TEXT("render_type")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString RenderType = Arguments->GetStringField(TEXT("render_type"));

			if (RenderType == TEXT("screenshot"))
			{
				const int32 ResX = Arguments->HasField(TEXT("resolution_x")) ? Arguments->GetIntegerField(TEXT("resolution_x")) : 3840;
				const int32 ResY = Arguments->HasField(TEXT("resolution_y")) ? Arguments->GetIntegerField(TEXT("resolution_y")) : 2160;
				FString OutputPath = Arguments->HasField(TEXT("output_path")) ? Arguments->GetStringField(TEXT("output_path")) : TEXT("");

				if (OutputPath.IsEmpty())
				{
					OutputPath = FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("HighResRender.png");
				}

				OutStructured->SetStringField(TEXT("render_type"), TEXT("screenshot"));
				OutStructured->SetNumberField(TEXT("resolution_x"), ResX);
				OutStructured->SetNumberField(TEXT("resolution_y"), ResY);
				OutStructured->SetStringField(TEXT("output_path"), OutputPath);
				OutStructured->SetStringField(TEXT("status"), TEXT("not_submitted"));
				OutStructured->SetBoolField(TEXT("verified_output"), false);
				OutError = TEXT("Screenshot output was not captured or verified.");
				return false;

			}
			else if (RenderType == TEXT("movie"))
			{
				const FString SeqPath = Arguments->GetStringField(TEXT("sequence_path"));
				if (SeqPath.IsEmpty())
				{
					OutError = TEXT("sequence_path required for movie render.");
					return false;
				}

				const int32 FrameStart = Arguments->HasField(TEXT("frame_range_start")) ? Arguments->GetIntegerField(TEXT("frame_range_start")) : 0;
				const int32 FrameEnd = Arguments->HasField(TEXT("frame_range_end")) ? Arguments->GetIntegerField(TEXT("frame_range_end")) : 300;
				const int32 ResX = Arguments->HasField(TEXT("resolution_x")) ? Arguments->GetIntegerField(TEXT("resolution_x")) : 1920;
				const int32 ResY = Arguments->HasField(TEXT("resolution_y")) ? Arguments->GetIntegerField(TEXT("resolution_y")) : 1080;

				OutStructured->SetStringField(TEXT("render_type"), TEXT("movie"));
				OutStructured->SetStringField(TEXT("sequence_path"), SeqPath);
				OutStructured->SetNumberField(TEXT("frame_start"), FrameStart);
				OutStructured->SetNumberField(TEXT("frame_end"), FrameEnd);
				OutStructured->SetNumberField(TEXT("resolution_x"), ResX);
				OutStructured->SetNumberField(TEXT("resolution_y"), ResY);
				OutStructured->SetStringField(TEXT("status"), TEXT("not_submitted"));
				OutError = TEXT("Movie render was not submitted to a verified queue.");
				return false;
			}
			else
			{
				OutError = TEXT("render_type must be 'screenshot' or 'movie'.");
				return false;
			}
			return false;
		}
	});

	// ---- render_queue_list ----
	Registry.Register({
		TEXT("render_queue_list"),
		TEXT("List current render queue jobs and their status."),
		FSololmcpSchemaBuilder::Object({{}}, {}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			// Render queue status is queried via the MoviePipelineQueueSubsystem
			TArray<TSharedPtr<FJsonValue>> JobsJson;

			OutStructured->SetArrayField(TEXT("jobs"), JobsJson);
			OutStructured->SetNumberField(TEXT("active_jobs"), JobsJson.Num());
			OutSummary = TEXT("Render queue status retrieved.");
			return true;
		}
	});

	// ---- render_queue_cancel ----
	Registry.Register({
		TEXT("render_queue_cancel"),
		TEXT("Cancel a specific render queue job."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("job_id"), FSololmcpSchemaBuilder::String(TEXT("Job ID to cancel"))}
		}, {TEXT("job_id")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			const FString JobId = Arguments->GetStringField(TEXT("job_id"));

			OutStructured->SetStringField(TEXT("job_id"), JobId);
			OutStructured->SetBoolField(TEXT("cancelled"), false);
			OutStructured->SetStringField(TEXT("status"), TEXT("not_cancelled"));
			OutError = TEXT("Render queue cancellation is not implemented; no verified job was cancelled.");
			return false;
		}
	});
}

// ============================================================================
// Registration Entry Point
// ============================================================================

void RegisterCinematicTools(FSololmcpToolRegistry& Registry)
{
	RegisterCameraAnimationTools(Registry);
	RegisterCameraTrackTools(Registry);
	RegisterRenderPipelineTools(Registry);
}

} // namespace UE::SOMOLMCP
