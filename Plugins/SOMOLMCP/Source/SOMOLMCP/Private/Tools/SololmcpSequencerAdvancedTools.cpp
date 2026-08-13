// Copyright 2026 SOMOLAGENT. All Rights Reserved.
// SOMOLMCP P3-5 — Sequencer Advanced Track Types (5 tools)
//
// DROP-IN LOCATION (sandbox blocked direct write):
//
// Tools registered (all native C++, sequence_* prefix; advanced track types
// beyond what SololmcpCinematicTools / SololmcpEnhancedTools already cover):
//
//   sequence_add_master_track       — Add a root-level (master) track
//                                     (shot / audio / event / cinematic_assembly)
//   sequence_add_camera_cuts_track  — Create the special CameraCutTrack and
//                                     optionally pre-populate cut sections
//   sequence_add_fade_track         — Add a UMovieSceneFadeTrack with a default
//                                     color section
//   sequence_add_event_track        — Add a UMovieSceneEventTrack at master or
//                                     binding scope
//   sequence_add_event_key          — Add an event keyframe, bind a Director
//                                     Blueprint endpoint, compile, save, and verify
//
// Registration entry point: UE::SOMOLMCP::RegisterSequencerAdvancedTools().
// NOTE: This file does NOT auto-link itself into the global registry — the
// host module is expected to wire RegisterSequencerAdvancedTools() in once a
// declaration is added to SololmcpToolRegistry.h (we are explicitly disallowed
// from modifying that header in this task).

#include "Tools/SololmcpToolRegistry.h"
#include "SololmcpJsonUtils.h"
#include "SololmcpSchemaBuilder.h"
#include "SololmcpErrorHelpers.h"
#include "SololmcpWriteFlush.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"

#include "Camera/CameraActor.h"
#include "GameFramework/Actor.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneBinding.h"
#include "MovieSceneObjectBindingID.h"

// Master / advanced track headers
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Sections/MovieSceneFadeSection.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Channels/MovieSceneEventChannel.h"
#include "MovieSceneEventUtils.h"
#include "MovieSceneSequenceEditor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"

namespace UE::SOMOLMCP
{

// ─────────────────────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace SequencerAdvancedHelpers
{
	/** Resolve the LevelSequence asset and its movie scene from a sequence_path
	 *  arg. Sets OutError + structured error_code on failure and returns false. */
	static bool ResolveSequence(
		const FSololmcpToolExecutionContext& Context,
		const TSharedRef<FJsonObject>& Arguments,
		const TSharedRef<FJsonObject>& OutStructured,
		ULevelSequence*& OutSequence,
		UMovieScene*& OutMovieScene,
		FString& OutError)
	{
		OutSequence = nullptr;
		OutMovieScene = nullptr;

		FString SequencePath;
		if (!Arguments->TryGetStringField(TEXT("sequence_path"), SequencePath) || SequencePath.IsEmpty())
		{
			SololmcpError::MissingParam(OutStructured, TEXT("sequence_path"));
			OutError = TEXT("Missing sequence_path.");
			return false;
		}

		UObject* Asset = Context.Services.LoadAsset(SequencePath, OutError);
		if (!Asset)
		{
			SololmcpError::InvalidPath(OutStructured, SequencePath);
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Failed to load LevelSequence at '%s'."), *SequencePath);
			}
			return false;
		}

		OutSequence = Cast<ULevelSequence>(Asset);
		if (!OutSequence)
		{
			SololmcpError::InvalidType(OutStructured, TEXT("sequence_path"), TEXT("LevelSequence"));
			OutError = FString::Printf(TEXT("Asset at '%s' is not a LevelSequence."), *SequencePath);
			return false;
		}

		OutMovieScene = OutSequence->GetMovieScene();
		if (!OutMovieScene)
		{
			SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""), TEXT("LevelSequence has no MovieScene."));
			OutError = TEXT("LevelSequence has no MovieScene.");
			return false;
		}

		return true;
	}

	/** Write a stable, log-friendly track id (FName-as-string of the track
	 *  UObject within its package). */
	static FString MakeTrackId(const UMovieSceneTrack* Track)
	{
		if (!Track) return FString();
		return Track->GetFName().ToString();
	}

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

	/** Lookup a track by its id (FName string) within a movie scene's master /
	 *  binding tracks. Returns nullptr if not found. */
	static UMovieSceneTrack* FindTrackById(UMovieScene* MovieScene, const FString& TrackId)
	{
		if (!MovieScene || TrackId.IsEmpty()) return nullptr;

		// UE 5.7: master tracks live in GetTracks() (legacy GetMasterTracks()
		// was removed). We also walk binding tracks for completeness.
		for (UMovieSceneTrack* Track : MovieScene->GetTracks())
		{
			if (Track && Track->GetFName().ToString() == TrackId)
			{
				return Track;
			}
		}
		for (const FMovieSceneBinding& Binding : GetMovieSceneBindingsConst(MovieScene))
		{
			for (UMovieSceneTrack* Track : Binding.GetTracks())
			{
				if (Track && Track->GetFName().ToString() == TrackId)
				{
					return Track;
				}
			}
		}
		// Camera-cut track is special — also check it.
		if (UMovieSceneTrack* CCTrack = MovieScene->GetCameraCutTrack())
		{
			if (CCTrack->GetFName().ToString() == TrackId)
			{
				return CCTrack;
			}
		}
		return nullptr;
	}

	/** Set a track's display name if non-empty (best-effort; not all tracks
	 *  support SetDisplayName but UMovieSceneNameableTrack does). */
	static bool TrySetDisplayName(UMovieSceneTrack* Track, const FString& Name)
	{
		// UE 5.7: UMovieSceneTrack::SetDisplayName removed; track name shown via owning binding.
		// For master tracks the name is derived from the track class. Skipping rename for now.
		(void)Track; (void)Name;
		return false;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool 1 — sequence_add_master_track
// ─────────────────────────────────────────────────────────────────────────────

static void Register_SequenceAddMasterTrack(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("sequence_add_master_track"),
		TEXT("Add a root-level (master) track to a LevelSequence. Master tracks live on the MovieScene itself rather than on an actor binding. Supported track_class values: 'shot' (UMovieSceneCinematicShotTrack), 'audio' (UMovieSceneAudioTrack), 'event' (UMovieSceneEventTrack), 'cinematic_assembly' (UMovieSceneSubTrack)."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path, e.g. /Game/Cine/MainSeq"))},
			{TEXT("track_class"),   FSololmcpSchemaBuilder::String(TEXT("One of: 'shot' | 'audio' | 'event' | 'cinematic_assembly'"),
				{TEXT("shot"), TEXT("audio"), TEXT("event"), TEXT("cinematic_assembly")})},
			{TEXT("track_name"),    FSololmcpSchemaBuilder::String(TEXT("Optional display name for the new track"))}
		}, {TEXT("sequence_path"), TEXT("track_class")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			ULevelSequence* Sequence = nullptr;
			UMovieScene*    MovieScene = nullptr;
			if (!SequencerAdvancedHelpers::ResolveSequence(Context, Arguments, OutStructured, Sequence, MovieScene, OutError))
			{
				return false;
			}

			FString TrackClass;
			if (!Arguments->TryGetStringField(TEXT("track_class"), TrackClass) || TrackClass.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("track_class"));
				OutError = TEXT("Missing track_class.");
				return false;
			}
			TrackClass = TrackClass.ToLower();

			FString TrackName;
			Arguments->TryGetStringField(TEXT("track_name"), TrackName);

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddMasterTrack", "SOMOLMCP Add Sequence Master Track"));
			Sequence->Modify();
			MovieScene->Modify();

			UMovieSceneTrack* NewTrack = nullptr;
			FString ResolvedClassName;

			if (TrackClass == TEXT("shot"))
			{
				NewTrack = MovieScene->AddTrack<UMovieSceneCinematicShotTrack>();
				ResolvedClassName = TEXT("UMovieSceneCinematicShotTrack");
			}
			else if (TrackClass == TEXT("audio"))
			{
				NewTrack = MovieScene->AddTrack<UMovieSceneAudioTrack>();
				ResolvedClassName = TEXT("UMovieSceneAudioTrack");
			}
			else if (TrackClass == TEXT("event"))
			{
				NewTrack = MovieScene->AddTrack<UMovieSceneEventTrack>();
				ResolvedClassName = TEXT("UMovieSceneEventTrack");
			}
			else if (TrackClass == TEXT("cinematic_assembly") || TrackClass == TEXT("subsequence") || TrackClass == TEXT("sub"))
			{
				NewTrack = MovieScene->AddTrack<UMovieSceneSubTrack>();
				ResolvedClassName = TEXT("UMovieSceneSubTrack");
			}
			else
			{
				SololmcpError::Set(OutStructured, TEXT("INVALID_TYPE"), TEXT("track_class"),
					TEXT("Expected one of: 'shot', 'audio', 'event', 'cinematic_assembly'."));
				OutError = FString::Printf(TEXT("Unknown track_class '%s'."), *TrackClass);
				return false;
			}

			if (!NewTrack)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("MovieScene::AddTrack returned null — track type may not be allowed at master scope."));
				OutError = FString::Printf(TEXT("Failed to add master track of class '%s'."), *ResolvedClassName);
				return false;
			}

			const bool bTrackNameApplied = !TrackName.IsEmpty()
				? SequencerAdvancedHelpers::TrySetDisplayName(NewTrack, TrackName)
				: false;

			MovieScene->MarkAsChanged();
			Sequence->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Sequence);

			const FString TrackId = SequencerAdvancedHelpers::MakeTrackId(NewTrack);
			OutStructured->SetStringField(TEXT("track_id"), TrackId);
			OutStructured->SetStringField(TEXT("class"),    ResolvedClassName);
			OutStructured->SetBoolField  (TEXT("master"),   true);
			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
			if (!TrackName.IsEmpty())
			{
				OutStructured->SetStringField(TEXT("requested_track_name"), TrackName);
				OutStructured->SetBoolField(TEXT("track_name_applied"), bTrackNameApplied);
				OutStructured->SetStringField(TEXT("track_name_note"), TEXT("UMovieSceneTrack display rename is unavailable in this UE branch."));
			}

			OutSummary = FString::Printf(TEXT("Added master %s track '%s' to '%s'."),
				*ResolvedClassName, *TrackId, *Sequence->GetName());
			return true;
		}
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool 2 — sequence_add_camera_cuts_track
// ─────────────────────────────────────────────────────────────────────────────

static void Register_SequenceAddCameraCutsTrack(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("sequence_add_camera_cuts_track"),
		TEXT("Add the special CameraCutTrack to a LevelSequence (only one allowed per sequence — errors with OPERATION_FAILED if one already exists). Optionally pre-populates cuts from camera_bindings: each entry is {camera_actor_name, start_frame}. Camera actors are matched by GetActorLabel() in the editor world; missing actors are auto-possessed."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("sequence_path"),   FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path"))},
			{TEXT("camera_bindings"), FSololmcpSchemaBuilder::Array(
				FSololmcpSchemaBuilder::Object({
					{TEXT("camera_actor_name"), FSololmcpSchemaBuilder::String(TEXT("Actor label of the camera"))},
					{TEXT("start_frame"),       FSololmcpSchemaBuilder::Integer(TEXT("FrameNumber.Value at which the cut starts (in MovieScene tick resolution)"))}
				}),
				TEXT("Optional pre-populated cuts (array of {camera_actor_name, start_frame})")
			)}
		}, {TEXT("sequence_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			ULevelSequence* Sequence = nullptr;
			UMovieScene*    MovieScene = nullptr;
			if (!SequencerAdvancedHelpers::ResolveSequence(Context, Arguments, OutStructured, Sequence, MovieScene, OutError))
			{
				return false;
			}

			if (UMovieSceneTrack* Existing = MovieScene->GetCameraCutTrack())
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("CameraCutTrack already exists; only one is allowed per sequence."));
				OutStructured->SetStringField(TEXT("existing_track_id"), SequencerAdvancedHelpers::MakeTrackId(Existing));
				OutError = TEXT("CameraCutTrack already exists on this sequence.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddCameraCutsTrack", "SOMOLMCP Add Camera Cuts Track"));
			Sequence->Modify();
			MovieScene->Modify();

			UMovieSceneTrack* CutTrackBase = MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
			UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(CutTrackBase);
			if (!CutTrack)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("MovieScene::AddCameraCutTrack returned null."));
				OutError = TEXT("Failed to create CameraCutTrack.");
				return false;
			}

			// Optional pre-population
			int32 CutsAdded = 0;
			int32 CutsRequested = 0;
			TArray<TSharedPtr<FJsonValue>> CutsJson;
			TArray<TSharedPtr<FJsonValue>> FailedCutsJson;

			const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
			if (Arguments->TryGetArrayField(TEXT("camera_bindings"), Bindings) && Bindings)
			{
				UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				for (const TSharedPtr<FJsonValue>& Val : *Bindings)
				{
					++CutsRequested;
					if (!Val.IsValid() || Val->Type != EJson::Object)
					{
						FailedCutsJson.Add(MakeShared<FJsonValueString>(TEXT("Invalid camera binding object.")));
						continue;
					}
					const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
					if (!Obj.IsValid())
					{
						FailedCutsJson.Add(MakeShared<FJsonValueString>(TEXT("Invalid camera binding object.")));
						continue;
					}

					FString CamName;
					if (!Obj->TryGetStringField(TEXT("camera_actor_name"), CamName) || CamName.IsEmpty())
					{
						FailedCutsJson.Add(MakeShared<FJsonValueString>(TEXT("Missing camera_actor_name.")));
						continue;
					}

					int32 StartFrame = 0;
					Obj->TryGetNumberField(TEXT("start_frame"), StartFrame);

					// Find or create possessable binding for the camera
					FGuid CameraBindingId;
					for (const FMovieSceneBinding& Binding : SequencerAdvancedHelpers::GetMovieSceneBindingsConst(MovieScene))
					{
						if (SequencerAdvancedHelpers::ResolveMovieSceneBindingName(MovieScene, Binding) == CamName)
						{
							CameraBindingId = Binding.GetObjectGuid();
							break;
						}
					}

					if (!CameraBindingId.IsValid())
					{
						AActor* CameraActor = nullptr;
						if (World)
						{
							for (TActorIterator<AActor> It(World); It; ++It)
							{
								if (It->GetActorLabel() == CamName)
								{
									CameraActor = *It;
									break;
								}
							}
						}
						UClass* CamClass = CameraActor ? CameraActor->GetClass() : ACameraActor::StaticClass();
						CameraBindingId = MovieScene->AddPossessable(CamName, CamClass);
						if (CameraActor && CameraBindingId.IsValid())
						{
							Sequence->BindPossessableObject(CameraBindingId, *CameraActor, World);
						}
					}

					if (!CameraBindingId.IsValid())
					{
						TSharedPtr<FJsonObject> FailedEntry = MakeShared<FJsonObject>();
						FailedEntry->SetStringField(TEXT("camera_actor_name"), CamName);
						FailedEntry->SetStringField(TEXT("reason"), TEXT("Failed to create camera binding."));
						FailedCutsJson.Add(MakeShared<FJsonValueObject>(FailedEntry));
						continue;
					}

					const FFrameNumber Frame(StartFrame);
					const FMovieSceneObjectBindingID BindingID = UE::MovieScene::FRelativeObjectBindingID(CameraBindingId);
					if (UMovieSceneCameraCutSection* Section = CutTrack->AddNewCameraCut(BindingID, Frame))
					{
						Section->SetRange(TRange<FFrameNumber>(Frame));
						++CutsAdded;

						TSharedPtr<FJsonObject> CutEntry = MakeShared<FJsonObject>();
						CutEntry->SetStringField(TEXT("camera_actor_name"), CamName);
						CutEntry->SetNumberField(TEXT("start_frame"), StartFrame);
						CutsJson.Add(MakeShared<FJsonValueObject>(CutEntry));
					}
					else
					{
						TSharedPtr<FJsonObject> FailedEntry = MakeShared<FJsonObject>();
						FailedEntry->SetStringField(TEXT("camera_actor_name"), CamName);
						FailedEntry->SetNumberField(TEXT("start_frame"), StartFrame);
						FailedEntry->SetStringField(TEXT("reason"), TEXT("AddNewCameraCut returned null."));
						FailedCutsJson.Add(MakeShared<FJsonValueObject>(FailedEntry));
					}
				}
			}

			MovieScene->MarkAsChanged();
			Sequence->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Sequence);

			const FString TrackId = SequencerAdvancedHelpers::MakeTrackId(CutTrack);
			OutStructured->SetStringField(TEXT("track_id"), TrackId);
			OutStructured->SetStringField(TEXT("class"),    TEXT("UMovieSceneCameraCutTrack"));
			OutStructured->SetNumberField(TEXT("cuts_requested"), CutsRequested);
			OutStructured->SetNumberField(TEXT("cuts_added"), CutsAdded);
			if (CutsJson.Num() > 0)
			{
				OutStructured->SetArrayField(TEXT("cuts"), CutsJson);
			}
			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
			if (CutsRequested > 0 && CutsAdded < CutsRequested)
			{
				OutStructured->SetStringField(TEXT("status"), TEXT("partial_success"));
				OutStructured->SetArrayField(TEXT("failed_cuts"), FailedCutsJson);
			}

			OutSummary = FString::Printf(TEXT("Added CameraCutTrack '%s' (%d/%d cuts) to '%s'."),
				*TrackId, CutsAdded, CutsRequested, *Sequence->GetName());
			return true;
		}
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool 3 — sequence_add_fade_track
// ─────────────────────────────────────────────────────────────────────────────

static void Register_SequenceAddFadeTrack(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("sequence_add_fade_track"),
		TEXT("Add a UMovieSceneFadeTrack (master-scope) for fade-to-black / fade-from-black transitions. Creates one default FadeSection covering the sequence playback range, with the supplied default_color (defaults to black). Use sequencer keyframe tools to author the alpha curve."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path"))},
			{TEXT("default_color"), FSololmcpSchemaBuilder::Array(
				FSololmcpSchemaBuilder::Number(),
				TEXT("[r,g,b,a] in 0..1; defaults to [0,0,0,1] (opaque black)")
			)}
		}, {TEXT("sequence_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			ULevelSequence* Sequence = nullptr;
			UMovieScene*    MovieScene = nullptr;
			if (!SequencerAdvancedHelpers::ResolveSequence(Context, Arguments, OutStructured, Sequence, MovieScene, OutError))
			{
				return false;
			}

			// Parse optional default_color [r,g,b,a]
			FLinearColor DefaultColor(0.f, 0.f, 0.f, 1.f);
			const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
			if (Arguments->TryGetArrayField(TEXT("default_color"), ColorArr) && ColorArr)
			{
				auto Get = [&](int32 Idx, float Default) -> float
				{
					if (ColorArr->IsValidIndex(Idx) && (*ColorArr)[Idx].IsValid())
					{
						return static_cast<float>((*ColorArr)[Idx]->AsNumber());
					}
					return Default;
				};
				DefaultColor.R = Get(0, 0.f);
				DefaultColor.G = Get(1, 0.f);
				DefaultColor.B = Get(2, 0.f);
				DefaultColor.A = Get(3, 1.f);
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddFadeTrack", "SOMOLMCP Add Fade Track"));
			Sequence->Modify();
			MovieScene->Modify();

			UMovieSceneFadeTrack* FadeTrack = MovieScene->AddTrack<UMovieSceneFadeTrack>();
			if (!FadeTrack)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("MovieScene::AddTrack<UMovieSceneFadeTrack>() returned null."));
				OutError = TEXT("Failed to add FadeTrack.");
				return false;
			}

			// TODO(P3-5): UMovieSceneFadeSection's color/alpha channel layout
			// changed across UE versions — we create a default section and let
			// the user author keys via existing sequence_* keying tools rather
			// than poking float channels directly here.
			const int32 SectionCountBefore = FadeTrack->GetAllSections().Num();
			UMovieSceneSection* SectionBase = FadeTrack->CreateNewSection();
			if (UMovieSceneFadeSection* FadeSection = Cast<UMovieSceneFadeSection>(SectionBase))
			{
				FadeSection->Modify();
				FadeSection->SetRange(MovieScene->GetPlaybackRange());
#if WITH_EDITORONLY_DATA
				// FadeColor is a UPROPERTY on UMovieSceneFadeSection in UE 5.x.
				FadeSection->FadeColor = DefaultColor;
#endif
				FadeTrack->AddSection(*FadeSection);
			}
			else if (SectionBase)
			{
				SectionBase->Modify();
				SectionBase->SetRange(MovieScene->GetPlaybackRange());
				FadeTrack->AddSection(*SectionBase);
			}
			else
			{
				MovieScene->RemoveTrack(*FadeTrack);
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("FadeTrack was created but CreateNewSection returned null; the track was removed."));
				OutError = TEXT("Failed to create default FadeSection.");
				return false;
			}
			if (FadeTrack->GetAllSections().Num() <= SectionCountBefore)
			{
				MovieScene->RemoveTrack(*FadeTrack);
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("FadeSection creation did not add a section; the track was removed."));
				OutError = TEXT("Failed to verify default FadeSection creation.");
				return false;
			}

			MovieScene->MarkAsChanged();
			Sequence->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Sequence);

			TArray<TSharedPtr<FJsonValue>> ColorJson;
			ColorJson.Add(MakeShared<FJsonValueNumber>(DefaultColor.R));
			ColorJson.Add(MakeShared<FJsonValueNumber>(DefaultColor.G));
			ColorJson.Add(MakeShared<FJsonValueNumber>(DefaultColor.B));
			ColorJson.Add(MakeShared<FJsonValueNumber>(DefaultColor.A));

			const FString TrackId = SequencerAdvancedHelpers::MakeTrackId(FadeTrack);
			OutStructured->SetStringField(TEXT("track_id"), TrackId);
			OutStructured->SetStringField(TEXT("class"),    TEXT("UMovieSceneFadeTrack"));
			OutStructured->SetArrayField (TEXT("default_color"), ColorJson);
			OutStructured->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());

			OutSummary = FString::Printf(TEXT("Added FadeTrack '%s' to '%s' (default_color=[%.2f,%.2f,%.2f,%.2f])."),
				*TrackId, *Sequence->GetName(),
				DefaultColor.R, DefaultColor.G, DefaultColor.B, DefaultColor.A);
			return true;
		}
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool 4 — sequence_add_event_track
// ─────────────────────────────────────────────────────────────────────────────

static void Register_SequenceAddEventTrack(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("sequence_add_event_track"),
		TEXT("Add a UMovieSceneEventTrack to a LevelSequence. If 'binding_id' is omitted the track is added at master scope; otherwise it is attached to the matching MovieScene binding (Guid string). Use sequence_add_event_key to create compiled Director Blueprint event endpoints."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("sequence_path"), FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path"))},
			{TEXT("binding_id"),    FSololmcpSchemaBuilder::String(TEXT("Optional FGuid string of an existing binding; omit for a master event track"))}
		}, {TEXT("sequence_path")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			ULevelSequence* Sequence = nullptr;
			UMovieScene*    MovieScene = nullptr;
			if (!SequencerAdvancedHelpers::ResolveSequence(Context, Arguments, OutStructured, Sequence, MovieScene, OutError))
			{
				return false;
			}

			FString BindingIdStr;
			Arguments->TryGetStringField(TEXT("binding_id"), BindingIdStr);

			FGuid BindingGuid;
			const bool bWantsBinding = !BindingIdStr.IsEmpty();
			if (bWantsBinding)
			{
				if (!FGuid::Parse(BindingIdStr, BindingGuid))
				{
					SololmcpError::InvalidType(OutStructured, TEXT("binding_id"), TEXT("FGuid string"));
					OutError = FString::Printf(TEXT("binding_id '%s' is not a valid FGuid."), *BindingIdStr);
					return false;
				}

				bool bFound = false;
				for (const FMovieSceneBinding& B : SequencerAdvancedHelpers::GetMovieSceneBindingsConst(MovieScene))
				{
					if (B.GetObjectGuid() == BindingGuid)
					{
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("binding %s"), *BindingIdStr));
					OutError = FString::Printf(TEXT("Binding '%s' not found in sequence."), *BindingIdStr);
					return false;
				}
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddEventTrack", "SOMOLMCP Add Event Track"));
			Sequence->Modify();
			MovieScene->Modify();

			UMovieSceneEventTrack* EventTrack = nullptr;
			if (bWantsBinding)
			{
				EventTrack = MovieScene->AddTrack<UMovieSceneEventTrack>(BindingGuid);
			}
			else
			{
				EventTrack = MovieScene->AddTrack<UMovieSceneEventTrack>();
			}

			if (!EventTrack)
			{
				SololmcpError::Set(OutStructured, TEXT("OPERATION_FAILED"), TEXT(""),
					TEXT("MovieScene::AddTrack<UMovieSceneEventTrack>() returned null."));
				OutError = TEXT("Failed to add EventTrack.");
				return false;
			}

			MovieScene->MarkAsChanged();
			Sequence->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Sequence);

			const FString TrackId = SequencerAdvancedHelpers::MakeTrackId(EventTrack);
			OutStructured->SetStringField(TEXT("track_id"),           TrackId);
			OutStructured->SetStringField(TEXT("class"),              TEXT("UMovieSceneEventTrack"));
			OutStructured->SetStringField(TEXT("binding_or_master"),  bWantsBinding ? BindingIdStr : TEXT("master"));
			OutStructured->SetStringField(TEXT("sequence_path"),      Sequence->GetPathName());

			OutSummary = FString::Printf(TEXT("Added %s EventTrack '%s' to '%s'."),
				bWantsBinding ? TEXT("binding-scoped") : TEXT("master"),
				*TrackId, *Sequence->GetName());
			return true;
		}
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Tool 5 — sequence_add_event_key
// ─────────────────────────────────────────────────────────────────────────────

static void Register_SequenceAddEventKey(FSololmcpToolRegistry& Registry)
{
	Registry.Register({
		TEXT("sequence_add_event_key"),
		TEXT("Add an event keyframe to an event track, bind a named Director Blueprint custom-event endpoint, compile the Blueprint, save the sequence, and verify key readback."),
		FSololmcpSchemaBuilder::Object({
			{TEXT("sequence_path"),        FSololmcpSchemaBuilder::String(TEXT("LevelSequence asset path"))},
			{TEXT("track_id"),             FSololmcpSchemaBuilder::String(TEXT("Track id returned by sequence_add_event_track"))},
			{TEXT("frame"),                FSololmcpSchemaBuilder::Integer(TEXT("FrameNumber.Value at which to fire (in MovieScene tick resolution)"))},
			{TEXT("event_function_name"),  FSololmcpSchemaBuilder::String(TEXT("Name of a BlueprintCallable UFUNCTION on the sequence's director Blueprint"))}
		}, {TEXT("sequence_path"), TEXT("track_id"), TEXT("frame"), TEXT("event_function_name")}),

		[](const FSololmcpToolExecutionContext& Context, const TSharedRef<FJsonObject>& Arguments, TSharedRef<FJsonObject>& OutStructured, FString& OutSummary, FString& OutError)
		{
			// Validate inputs so the caller still gets early feedback.
			ULevelSequence* Sequence = nullptr;
			UMovieScene*    MovieScene = nullptr;
			if (!SequencerAdvancedHelpers::ResolveSequence(Context, Arguments, OutStructured, Sequence, MovieScene, OutError))
			{
				return false;
			}

			FString TrackId, FunctionName;
			int32 Frame = 0;
			if (!Arguments->TryGetStringField(TEXT("track_id"), TrackId) || TrackId.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("track_id"));
				OutError = TEXT("Missing track_id.");
				return false;
			}
			if (!Arguments->TryGetNumberField(TEXT("frame"), Frame))
			{
				SololmcpError::MissingParam(OutStructured, TEXT("frame"));
				OutError = TEXT("Missing frame.");
				return false;
			}
			if (!Arguments->TryGetStringField(TEXT("event_function_name"), FunctionName) || FunctionName.IsEmpty())
			{
				SololmcpError::MissingParam(OutStructured, TEXT("event_function_name"));
				OutError = TEXT("Missing event_function_name.");
				return false;
			}

			UMovieSceneTrack* Track = SequencerAdvancedHelpers::FindTrackById(MovieScene, TrackId);
			if (!Track)
			{
				SololmcpError::NotFound(OutStructured, FString::Printf(TEXT("track %s"), *TrackId));
				OutError = FString::Printf(TEXT("Track '%s' not found."), *TrackId);
				return false;
			}
			UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track);
			if (!EventTrack)
			{
				SololmcpError::InvalidType(OutStructured, TEXT("track_id"), TEXT("UMovieSceneEventTrack"));
				OutError = FString::Printf(TEXT("Track '%s' is not an EventTrack."), *TrackId);
				return false;
			}

			FMovieSceneSequenceEditor* SequenceEditor = FMovieSceneSequenceEditor::Find(Sequence);
			if (!SequenceEditor || !SequenceEditor->SupportsEvents(Sequence))
			{
				OutError = TEXT("This sequence type does not expose a Director Blueprint event editor.");
				SololmcpError::Set(OutStructured, TEXT("UE_API_ERROR"), TEXT("sequence_path"), OutError);
				return false;
			}
			UBlueprint* DirectorBlueprint = SequenceEditor->GetOrCreateDirectorBlueprint(Sequence);
			if (!DirectorBlueprint)
			{
				OutError = TEXT("Failed to create or retrieve the sequence Director Blueprint.");
				return false;
			}

			const FScopedTransaction Transaction(NSLOCTEXT("SOMOLMCP", "SequenceAddEventKey", "SOMOLMCP Add Sequencer Event Key"));
			Sequence->Modify();
			MovieScene->Modify();
			EventTrack->Modify();

			UMovieSceneEventTriggerSection* TriggerSection = nullptr;
			for (UMovieSceneSection* ExistingSection : EventTrack->GetAllSections())
			{
				if (UMovieSceneEventTriggerSection* ExistingTrigger = Cast<UMovieSceneEventTriggerSection>(ExistingSection))
				{
					TriggerSection = ExistingTrigger;
					break;
				}
			}
			if (!TriggerSection)
			{
				TriggerSection = Cast<UMovieSceneEventTriggerSection>(EventTrack->CreateNewSection());
				if (!TriggerSection)
				{
					OutError = TEXT("Event track failed to create a trigger section.");
					return false;
				}
				TriggerSection->SetFlags(RF_Transactional);
				EventTrack->AddSection(*TriggerSection);
			}
			TriggerSection->Modify();

			TMovieSceneChannelData<FMovieSceneEvent> ChannelData = TriggerSection->EventChannel.GetData();
			if (ChannelData.FindKey(FFrameNumber(Frame)) != INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("An event key already exists at frame %d on track '%s'."), Frame, *TrackId);
				SololmcpError::Set(OutStructured, TEXT("ALREADY_EXISTS"), TEXT("frame"), OutError);
				return false;
			}
			const int32 EventIndex = ChannelData.AddKey(FFrameNumber(Frame), FMovieSceneEvent());
			FMovieSceneEvent* EventEntry = &ChannelData.GetValues()[EventIndex];
			UK2Node_CustomEvent* Endpoint = FMovieSceneEventUtils::BindNewUserFacingEvent(EventEntry, TriggerSection, DirectorBlueprint);
			if (!Endpoint)
			{
				TriggerSection->EventChannel.GetData().RemoveKey(EventIndex);
				OutError = TEXT("Failed to create and bind the Director Blueprint event endpoint.");
				return false;
			}
			Endpoint->Modify();
			Endpoint->CustomFunctionName = FName(*FunctionName);
			Endpoint->ReconstructNode();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(DirectorBlueprint);
			FKismetEditorUtilities::CompileBlueprint(DirectorBlueprint);
			if (DirectorBlueprint->Status == BS_Error)
			{
				OutError = FString::Printf(TEXT("Director Blueprint compilation failed after binding event '%s'."), *FunctionName);
				return false;
			}

			MovieScene->MarkAsChanged();
			Sequence->MarkPackageDirty();
			SololmcpWriteFlush::EnsureFlushed(Sequence);
			if (!UEditorAssetLibrary::SaveAsset(Sequence->GetPathName(), false))
			{
				OutError = TEXT("Failed to save the LevelSequence after adding the event key.");
				return false;
			}

			const TMovieSceneChannelData<const FMovieSceneEvent> Readback = TriggerSection->EventChannel.GetData();
			const int32 ReadbackIndex = Readback.FindKey(FFrameNumber(Frame));
			if (ReadbackIndex == INDEX_NONE || !Readback.GetValues()[ReadbackIndex].WeakEndpoint.IsValid())
			{
				OutError = TEXT("Event key or endpoint failed readback after save.");
				return false;
			}
			OutStructured->SetStringField(TEXT("track_id"), TrackId);
			OutStructured->SetNumberField(TEXT("frame"), Frame);
			OutStructured->SetStringField(TEXT("function"), FunctionName);
			OutStructured->SetStringField(TEXT("endpoint_node_guid"), Endpoint->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			OutStructured->SetNumberField(TEXT("event_key_count"), Readback.GetTimes().Num());
			OutStructured->SetBoolField(TEXT("director_blueprint_compiled"), true);
			OutStructured->SetBoolField(TEXT("saved"), true);
			OutStructured->SetBoolField(TEXT("readback_verified"), true);
			OutSummary = FString::Printf(TEXT("Added event '%s' at frame %d on '%s'; Director Blueprint compiled, sequence saved, and key verified."),
				*FunctionName, Frame, *TrackId);
			return true;
		}
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Module entry point
// ─────────────────────────────────────────────────────────────────────────────

void RegisterSequencerAdvancedTools(FSololmcpToolRegistry& Registry)
{
	Register_SequenceAddMasterTrack    (Registry);
	Register_SequenceAddCameraCutsTrack(Registry);
	Register_SequenceAddFadeTrack      (Registry);
	Register_SequenceAddEventTrack     (Registry);
	Register_SequenceAddEventKey       (Registry);
}

} // namespace UE::SOMOLMCP
